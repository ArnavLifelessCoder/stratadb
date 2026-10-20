# SSTable format and lifetime

M3 adds standalone immutable tables under `src/table/`. DB reads still use the
memtable and WAL until M4 connects flushing, manifests and table discovery.
The public DB API is unchanged.

## Encoding

All fixed-width integers are little endian. Length varints are canonical unsigned
base-128 values. Each key is the user key followed by an eight-byte numeric tag,
`sequence << 8 | type`. User keys sort bytewise ascending; tags sort numerically
descending. Sequences are nonzero and at most 56 bits; type is delete (0) or put (1).
Tombstones have empty values.

The file consists of consecutive data blocks, one filter block, one index block,
and a 48-byte footer. There is no padding.

| Part | Encoding |
| --- | --- |
| Data entry | shared-prefix length, unshared-key length, value length (three varint32s), unshared key bytes, value bytes |
| Data block end | restart offsets (uint32 each), restart count (uint32), CRC32C (uint32) |
| Filter | probe count (one byte), bit array, CRC32C |
| Index | same prefix/restart encoding as data blocks; keys are exact last internal keys of data blocks, values are handles |
| Handle | offset (uint64), size including block CRC (uint64) |
| Footer | filter handle (16 bytes), index handle (16), version 1 (uint32), magic `STRATA03` (8 bytes), CRC32C (uint32) |

The first entry and each subsequent group of 16 entries start with a full key.
Restart offsets point to the first varint in those entries, relative to the block.
An empty block has zero restart offsets, a zero count and its CRC.
Every CRC covers the entire preceding block, including restart metadata, and
excludes its own four bytes. The footer has its own CRC.

The writer targets `block_bytes` using an uncompressed size estimate; one oversized
entry stays whole in its own block. The target is not a maximum entry size. Exact
last keys are valid upper separators and avoid shortening across user-key or
sequence boundaries. A user's versions may span any number of blocks.

The bloom filter contains distinct user keys, including keys with tombstones.
It uses MurmurHash3 x64_128's low 64 bits, split into a low 32-bit starting value
and an odd high 32-bit step. Probe `i` uses `(start + i * step) % bit_count`.
At 10 bits per distinct key there are seven probes. Bit arrays round up to whole
bytes, with a minimum of eight bytes. A probe count of zero disables filtering
and has no bit array. Supported bits-per-key settings are 0 through 30.

## Reading and corruption

Readers own read-only native mappings. The mapping and its file must remain
immutable for the reader's lifetime; in-place modification or truncation is not
supported. Windows uses `CreateFileMappingW` and `MapViewOfFile`; POSIX uses
`mmap(PROT_READ, MAP_PRIVATE)`.

Open checks the footer, metadata ranges, contiguous block coverage, every block
CRC, restart arrays, entry order, separators, tombstones and stored-key bloom
membership before returning a reader. This deliberately makes Open linear in
the table's size in M3. It avoids exposing a table with damage in an unqueried
block. No untrusted length is used before bounds validation.

Get first checks the bloom filter, then binary-searches the index for an internal
seek key. It decodes and CRC-checks the selected block and finds the newest version
at or below the requested sequence. Tombstones are returned as entries so M4 can
distinguish a deletion from absence in this table. ReadAll decodes checked blocks
in order. The reader has no mutable shared state and supports concurrent calls.

Data blocks are decoded on each lookup; restart points are validated but are not
yet used to decode only a suffix. No block cache exists until M7. Index and bloom
data are held in memory; the operating system pages the mapped table bytes.

## Publication and errors

The caller exclusively owns a destination name and its `.tmp` sibling.
WriteTable rejects an existing destination and never intentionally edits a live
table. It writes the sibling temp file, finishes every block and footer, calls
file Sync, closes the handle, then calls the platform AtomicRename.
POSIX uses `fdatasync`, `rename`, and parent-directory `fsync`.
Windows uses `FlushFileBuffers` and
`MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.
New Windows directory creation relies on filesystem metadata recovery, such as
NTFS journaling; it does not perform an explicit directory flush.

A builder stops after any append or sync failure. Failed writes do not publish
the temp file. Failed publication removes an unpublished temp file, but preserves
any destination: a POSIX directory-sync error can occur after the rename succeeded.
The caller must treat such a failure as an uncertain publication and reconcile it
during manifest recovery. M4 will supply that recovery and unique file numbering.

Table settings are validated at the standalone table boundary. DB Options limits,
including `memtable_bytes`, remain deferred until M4 activates them.

## Verification

Tests cover native round trips across block sizes and bloom settings; empty,
binary and long keys; empty and 1 MiB values; versions crossing blocks;
tombstones; concurrent mapped lookups; invalid settings; immutable destinations;
every byte flip and every truncation in a representative complete table;
valid-CRC malformed metadata; and injected partial writes and sync failures.
Block encoding is compared with uncompressed entry vectors and bloom bits with
an independent bit-vector reference.

`scripts/measure_bloom.py` runs five seeded C++ accuracy trials and saves the
counts, rates, theoretical probability, platform, compiler and flags. These are
accuracy measurements, not throughput benchmarks. See `results/m3/` for actual
results and the milestone review packet.
