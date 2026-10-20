# Memtable and WAL

M2 exposes `DB::Open`, `Put`, `Delete`, `Write`, `Get`, and `GetStats`.
The public header uses `std::filesystem::path` for native Windows paths.
Snapshots and iterators will be added in M5. A non-null snapshot in
`ReadOptions` is currently rejected. Callers must stop using a DB before
destroying it and must not mutate a batch during `Write`.

## Visibility

A writer mutex serializes writes. Each operation receives a 56-bit sequence.
Nodes compare user-key bytes ascending, then `(sequence << 8 | type)`
descending. Put is type 1 and Delete is type 0. Empty and binary keys and
values are valid.

The arena owns node and string storage until the memtable is destroyed.
The skiplist has 12 levels and promotion probability 1/4. Links use acquire
loads and release stores. Readers never touch the arena's allocation bookkeeping.
They capture the committed sequence once, then skip newer nodes. A batch
advances that sequence only after every node has been installed. A reader
therefore observes either the state before a batch or the complete batch,
including when it repeatedly changes one key.

Lookup retains its checked level-zero candidate. Reloading the predecessor's
link after comparison would allow a concurrent writer to substitute a newer,
ineligible version. The concurrent-reader regression test exercises this race.

## WAL bytes

All fixed-width integers are little endian:

| Field | Encoding |
| --- | --- |
| CRC | uint32 CRC32C over the following length, type and payload |
| Payload length | uint32 |
| Record type | uint8, currently 1 for WriteBatch |
| Starting sequence | canonical uint64 varint, restricted to 1 through 2^56-1 |
| Operation count | canonical uint32 varint, nonzero |
| Operation | uint8 type, key length varint32, key bytes |
| Put value | value length varint32, value bytes; omitted for Delete |

Batch operations are encoded in insertion order. The decoder checks the whole
batch before replay applies it. Overflow, unknown types, noncanonical varints,
truncated fields, trailing bytes and sequence gaps are errors. Empty batches
are successful no-ops and create no WAL record.

CRC dispatch checks SSE4.2 support at runtime on x86-64. Only the specialized
function uses those instructions; other CPUs use the table. Both implementations
are directly tested against all RFC 3720 B.4 vectors and a bitwise reference.

## Durability and recovery

The DB holds an exclusive OS lock on `LOCK` for its entire lifetime.
A new WAL is created as `WAL.tmp`, truncated, synced, closed, and atomically
renamed. On Windows the platform layer uses `FlushFileBuffers` and
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. On POSIX it uses `fdatasync`,
`rename`, and directory `fsync`; newly created parent entries are also synced.

Write order is append, optional sync, memtable insertion, committed-sequence
publication, then success. A sync write waits for the native file-sync call.
An append or sync error poisons the writer until the DB is closed and reopened;
reads still see the last committed state. A write that returned an I/O error
may replay after reopening if its complete record reached disk.

Replay checks every record CRC. A short final record or a CRC failure whose
declared record ends at EOF is truncated to its starting offset, synced, and
reported on stderr. A CRC failure before EOF is corruption. A damaged length
extending past EOF is also corruption if a later valid batch is discoverable;
recovery must not discard that later record as a torn tail. Ambiguous damage
with no recoverable later record is treated as a torn tail. This format cannot
distinguish every possible multi-byte corruption from an interrupted append.
Valid-CRC records with invalid payloads are always corruption.

Recovery builds a fresh memtable. On failure, Open leaves the caller's output
unchanged. On success, the next append follows the recovered sequence and the
repaired file boundary.

## Scope and tests

M2 retains all versions in memory and one WAL for the directory's lifetime.
The configured memtable limit becomes active with flushing in M4. There are
no SSTables, file rotation, compaction, snapshots, iterators or group commit yet.
GetStats write counters cover the current Open session and exclude replay;
file counts are empty while there are no SSTables.

The differential test executes 10,000 seeded streams of 64 operations against
`std::map`, covering puts, deletes, ordered batches and reads. A database is
reused for 100 streams; every stream checks all 32 possible keys, and each group
is closed, reopened, checked, appended to, and reopened again. Failure output
contains the exact stream seed. `--seed=N` selects the first seed.

Other tests cover empty/binary keys, 1 MiB values, concurrent readers and writers,
every truncation point in a final record, a flipped byte at every position of a
middle record, and deterministic read/write/sync/truncate failures.
`tests/crash_smoke.py` uses `proc.kill()` for six sync and six async cycles,
recording the OS in its JSON result. It records acknowledgments only after
successful Write calls and verifies recovered values and batch completeness.
Async recovery may lose a tail, but must not contain holes. These smoke checks
do not replace the 500-cycle M6 campaign.
