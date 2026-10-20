# Flush, manifest and recovery

M4 connects the WAL, memtable and standalone SSTables. The public DB methods are
unchanged. Compaction, public snapshots and iterators remain M5 work.

## Durable files and edits

File 1 is `MANIFEST-1`; the first WAL is `2.wal`. Later WAL and SSTable names use
monotonically reserved integer numbers and `.wal` or `.sst` extensions.
`CURRENT` contains `MANIFEST-1` followed by a newline. It is published through a
temp file, file sync, close and native atomic rename. M4 has one growing manifest;
manifest rollover is not implemented.

The manifest is a CRC32C-protected edit log. Each frame has a uint32 CRC, uint32
payload length, one-byte type (1), and payload. CRC covers length, type and
payload. Fixed integers are little endian. The payload contains canonical
varints for format version 1, next file number, last durable sequence, flushed
sequence, current WAL, previous WAL, reserved pending-table number, added-file
count and removed-file count. Each addition contains file number, level, byte
size, and length-prefixed smallest/largest user keys. Removals contain numbers.

Versions contain immutable file metadata and shared mapped readers. Applying an
edit builds a replacement Version, appends and syncs its frame, then publishes
the replacement. Readers retain the previous Version until their call completes.
No SSTable referenced by a Version is deleted in M4.

## Rotation and background flush

One mutex serializes writers and state transitions. Reads take a brief state
snapshot under that mutex, then traverse captured memtables and mapped tables
without holding it. A committed sequence is published only after every operation
in a batch is inserted. A batch never straddles memtables.

When arena allocation exceeds `memtable_bytes`, rotation does the following:

1. Wait for an existing immutable memtable to finish flushing, if necessary.
2. Sync the current WAL, including earlier async writes.
3. Reserve numbers for a new WAL and the pending SSTable. Create, sync and
   atomically publish the empty new WAL.
4. Append and sync a manifest edit recording both WALs, the old WAL's final
   sequence, the reservation and the next unused number.
5. Publish a new mutable memtable/WAL and hand the old memtable to one worker.

The worker exports the frozen skiplist in internal-key order and writes an L0
table with the table writer's sync/rename protocol. It then appends and syncs an
edit adding that table, advancing the flushed sequence and clearing the previous
WAL and pending-table fields. Only then does it release the immutable memtable
and remove the old WAL.

There is at most one mutable and one immutable memtable. A large batch can exceed
the configured limit; the limit is an allocation threshold, not a hard bound.
Arena chunks have a 64 KiB minimum. Waiting for the immutable buffer is necessary
buffer backpressure; L0 slowdown/stop thresholds and stall counters remain M7.
Close joins the worker and lets an outstanding flush finish. The mutable
memtable need not flush on close because its WAL remains recoverable.

## Reads and failures

Reads search the mutable memtable, immutable memtable, then L0 tables newest
first. A tombstone stops the search rather than exposing an older table value.
Metadata ranges, bloom filters, the block index and restart-point seeks narrow
the table lookup. Every data block read checks its CRC. Open checks table
metadata; explicit TableReader::Verify provides a full integrity scan.

An append, sync, rotation or background-flush failure stops subsequent writes.
Reads retain the previously published state. Once a batch is appended and
published in the memtable, Write returns OK even if the following rotation
fails; that failure is returned by later writes. An append or sync failure can
still have an uncertain durable outcome, resolved by reopening.
The original WALs remain available until the corresponding
table edit is durable. Failed or uncertain table publication is never recorded
as a successful flush.

## Reopen

Recovery holds the exclusive directory lock, validates CURRENT, replays edits
and opens only the manifest's live tables. It replays the previous WAL, if any,
starting immediately after the flushed sequence; that log must end at the
manifest's recorded boundary. The current WAL follows that sequence. Missing
referenced files, sequence gaps and middle-record corruption are errors.

A short, bad-CRC final or all-zero manifest suffix is truncated and synced.
Recovery conservatively searches for later valid frames before treating a
damaged length as a tail, with the same embedded-frame ambiguity documented for
the WAL. An empty or damaged initial manifest is corruption.

If a crash left the pending SSTable published but unrecorded, recovery removes
that unreferenced file and rebuilds it from the previous WAL. Only a manifest
edit can make it live. Unreferenced WALs left by a failed rotation can be safely
replaced when their uncommitted numbers are reused. Reappearance of a removed
obsolete WAL after power loss is harmless because recovery reads only referenced
logs. Cleanup removals are not directory-synced; durable publication always is
on POSIX, with the documented Windows write-through rename protocol.

An interrupted initial publication can finish CURRENT from the durable manifest.
If only the first numbered WAL was published, bootstrap preserves it. If later
WALs or SSTables exist without a manifest, Open reports corruption instead of
initializing an empty database.
The legacy M2/M3 `WAL` is copied into the first numbered WAL before publishing
the manifest. The original is retained as an unused migration backup; future
opens use numbered WALs.

## Options and counters

Open rejects zero memtable limits, zero or unrepresentable block targets, and
bloom bits outside 0 through 30 before creating files. Inactive compaction/cache
options are reserved for their milestones.

Stats include both resident memtables, per-level live file counts, user payload
bytes, successful WAL syncs, and recovered WAL-tail bytes. Disk-byte accounting
includes WAL appends, completed SSTable writes, manifest edits and initial
metadata publication during this Open session. Replay does not count as a new
write. WAL sync counts include rotation barriers, but exclude syncing newly
created empty files and manifest files.

## Tests

Tests compare reopened contents with a map after zero, one and many forced
flushes, and during a randomized stream with repeated reopens and a partly
filled current WAL. Other tests cover tombstones over overlapping tables, large
values, concurrent reads/writes during flush, interrupted rotation, orphan
publication, background failures, missing referenced files, legacy WAL migration,
metadata CRCs, truncated records and injected append/sync failures.

The process-kill smoke uses a one-byte memtable threshold to force rotation and
flush. It records the platform and resulting SSTable count, checks acknowledged
sync writes and valid async prefixes, and runs in Windows/Linux Debug and ASan.
The larger 500-cycle crash campaign remains M6.
