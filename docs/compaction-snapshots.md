# Compaction, snapshots and iterators

M5 adds leveled compaction on the existing background worker, sequence snapshots,
stable iterators and obsolete-file reclamation. Cache, group commit and L0 write
stall thresholds remain M7 work.

## Selection and merge

L0 triggers at `l0_compaction_trigger` files. A job takes all current L0 files
and the overlapping L1 ranges. Above L0, the target is 10 MiB for L1, multiplied
by `level_size_ratio` for each deeper level. The highest size/target score at
least one selects a level, then a round-robin key pointer selects one file and
its overlapping next-level files. There are seven levels, L0 through L6.
Pointers advance after a successful edit and reset on Open.

The merging iterator uses a heap of child iterators, with one cursor per sorted
input stream. Each table loads one data block at a time and checks its CRC on
every load. The full-sort unit
reference checks heap ordering; randomized map views check retention semantics.
A version is discarded only when a newer version of that key is at or below
the oldest live snapshot. A tombstone at or below that boundary can be discarded
only when no deeper file range can contain the key. Range checks are conservative
and can retain a tombstone even when the deeper file does not actually contain it.

Outputs target 2 MiB of entries and split only between user keys. A large value
or a long history for one key can exceed that target. L1 through L6 ranges are
sorted and non-overlapping; manifest edits violating this rule are rejected.
Reads binary-search each higher level's range index, then use the table's bloom,
block index, restart-point seek and per-read CRC.

## Publication and locking

Only one background thread flushes and compacts. A manifest transaction mutex
serializes file-number allocation and edits with WAL rotation. Compaction holds
it only to persist an output file-number reservation or commit the final edit;
it refreshes WAL metadata at commit. Table reads,
table writes, reader opening, manifest append/sync and obsolete-table deletion
run outside the DB mutex. The DB mutex protects short read-view captures,
memtable insertion, snapshot registration and state transitions. WAL append/sync
holds only the writer mutex. Readers can capture the preceding committed view
while a writer waits for storage. Atomic WAL counters let GetStats run concurrently.

All output tables use temp write, file sync, close and atomic rename with the
platform durability protocol. One synced manifest edit adds all outputs and
removes all inputs. A failure before that edit leaves the inputs live. An uncertain
manifest failure stops further writes; recovery decides which edit is durable.
Unreferenced outputs are reclaimed at the next successful Open.

VersionSet publishes a shared immutable state atomically after manifest sync.
Readers keep the previous state while that I/O runs. An immutable memtable may
briefly coexist with its newly published table; sequence-aware reads and iterator
merge handle that duplicate consistently.

Once a Write has appended its batch and published it in the memtable, it returns
OK even if the subsequent rotation fails. The failure is retained for later
writes. This does not change the uncertain-outcome rule for an append or sync
failure before publication.

## Snapshot and iterator API

`GetSnapshot()` captures the committed sequence, memtables and Version.
`Get` and `NewIterator` accept it through `ReadOptions`. It belongs to the creating
DB; using another DB's snapshot returns InvalidArgument. `ReleaseSnapshot`
releases its ownership. Stop using a snapshot after release. Release all snapshots
and iterators before destroying the DB, and stop concurrent calls before closing.

An iterator starts invalid and supplies `Valid`, `SeekToFirst`, `SeekToLast`,
`Seek`, `Next`, `Prev`, `key`, `value` and `status`. Seek finds the first key at
or above its target. Forward and reverse traversal use ascending user-key order;
deletes and superseded values are hidden. Empty and binary keys/values are valid.
Next/Prev on an invalid iterator are no-ops; key/value return empty views when
invalid. Always check Valid to distinguish a real empty key. Errors are sticky
and reported through status.

NewIterator captures references and a sequence under the DB mutex. It does not
traverse or copy memtable entries, read data blocks, or materialize rows. Each
memtable cursor retains its skiplist and follows acquire-loaded node links.
BlockIterator binary-searches restart points for Seek and replays at most one
restart group for Prev. TableIterator navigates the index and loads blocks lazily.
L0 uses one child per overlapping table; each higher level uses one concatenating
cursor that opens only the table selected by its range index. MergingIterator
merges these children in either direction. DBIter selects the visible sequence
and hides older versions and tombstones, retaining only the current row and
temporary per-key state. Corruption in an unread block is reported when visited.

The read sequence hides later writes even though the active skiplist may grow.
The iterator retains its captured Version and both memtables until destruction,
independently of the Snapshot handle used to create it.
Separate iterators may be used concurrently; one iterator has one owning caller.

## Reclamation

A removed table is queued with a weak reference to its mapped reader. The worker
deletes it only after every old Version, in-flight read, Snapshot and iterator
has released that reader. Snapshot/iterator release wakes cleanup; periodic
cleanup also catches short-lived Get calls that finish after a compaction edit.
Close drains eligible flush/compaction work and attempts cleanup.

Deletion failure is logged, not treated as database corruption. An obsolete WAL
deletion failure never poisons writes. After successful recovery and before
starting the worker, Open removes unreferenced numbered WALs/SSTables, recognized
storage temp files and the unused legacy WAL. It preserves live files and names
outside the storage naming scheme. Cleanup does not recursively delete folders.
Removal need not be directory-synced because recovery ignores a reappearing
unreferenced file and retries its removal.

Windows mappings are read-only but share read/write/delete access; the reason
and native deletion test are documented in [sstable.md](sstable.md). The engine
does not rely on deleting a table while its mapping is open.

## Limits

Compaction streams its inputs and buffers one output chunk for the existing
table writer. That chunk targets 2 MiB but can grow for a single key's history.
Iterators retain child cursors and current keys/values rather than complete
input streams. Table metadata, bloom filters and mappings still live in their
readers, and snapshot pinning can retain old memtables and table files. Full
throughput and memory measurements remain M8 work.

The manifest is append-only and is not rotated. Compaction failures stop later
writes but leave previously published read views available. The 12-cycle smoke
tests exercise flush/compaction and restart; the 500-cycle crash campaign remains
M6.
