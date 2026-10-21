# Block cache, group commit and write stalls

## Block cache

`src/table/block_cache.{hpp,cpp}` is a sharded LRU cache of data blocks keyed
by `(file number, block offset)`. `Options::block_cache_bytes` is split evenly
over 16 shards, each with its own mutex, list and hash index. The charge of an
entry is its block size; a block larger than one shard is not cached, and a
capacity of 0 disables the cache.

Tables are memory-mapped, so a miss copies the block out of the mapping. The
copy is CRC-checked once on its way in, and hits are served without checking
it again. Without a cache every block load is checked, as before. Entries are
`shared_ptr`s: a table iterator holds the block it is reading, so evicting it
only drops the cache's reference. Table file numbers are never reused, so the
blocks of a deleted table are never looked up again and simply age out.

`Stats` reports `block_cache_hits`, `block_cache_misses` and
`block_cache_bytes` (current usage).

## Group commit

Writers queue in `DB::Write`. The writer at the front becomes the leader: it
takes the writers queued behind it, up to 1 MiB (or its own size plus 128 KiB
when it is small, so a small write is not held behind a large group), and
commits them as one WAL record with one sync. Then it inserts the entries into
the memtable, publishes the group's last sequence, and wakes each follower with
the shared status. Batches keep their relative order and stay atomic, because
the record is all-or-nothing on recovery.

A leader that does not sync stops its group at the first sync writer, which
must not be acknowledged without a sync. A sync leader may take non-sync
writers along; they get a durability upgrade.

Only the leader appends to or rotates the WAL, so it drops the read-view mutex
during the append and sync, and readers are not blocked by disk writes.

`Stats::write_calls` counts write calls, `wal_records` counts records written,
and `wal_syncs` counts syncs. In `WritePathTest.SixteenSyncWritersShareFewerSyncsThanWrites`,
16 threads each issue 100 sync puts. On the development machine (Windows,
NVMe), the 1,600 writes needed 163 syncs. The exact number depends on timing.

## Write stalls

Before taking a memtable slot the leader checks the L0 table count:

* at 8 or more tables it sleeps 1 ms, once per write (slowdown);
* at the stop threshold it waits until compaction has brought L0 below it.

The stop threshold is `l0_stop_writes` (default 12), raised to
`l0_compaction_trigger + 1` if needed, because stopping before compaction can
start would wait forever. Waiting for a full immutable memtable to flush is
also counted. `Stats::stall_micros`, `slowed_writes` and `stopped_writes`
report all three.

The tests drive L0 into both thresholds deterministically through an internal
hook that pauses compactions while flushes continue
(`internal::pause_compactions_for_testing`). The hook is not part of the
public API.
