# Crash testing

`python/crash_test.py` kills a writer process at random times and checks what
survives. It is the M6 acceptance test and also runs in CI in a shorter form.

## Writer

`tests/crash_writer.cpp` builds `lsm_crash_writer`. In `write` mode it opens the
DB with a 64 KiB memtable, 1 KiB blocks and an L0 trigger of 2, so flushes and
compactions run constantly while the parent decides when to kill it. It then
issues single-threaded writes over 20,000 keys:

* 85% single operations through `Put` or `Delete`, 15% `WriteBatch`es of 2 to 8
  operations through `Write`;
* 20% deletes; put values are 0 to 2,000 filler bytes, with 1% at 20,000;
* every value is `"<op id>:"` followed by filler derived from the op id, so a
  damaged or mismatched value is detected on its own.

Before each call the writer appends the batch's intent (batch id and every op)
to a log file and flushes it to the OS. After the call returns OK it appends
`A <batch id>`. An acknowledgement therefore exists only for a call that has
returned. The log is written by the child itself, so a kill can cut its last
line; the harness only reads complete lines and complete intents.

In `dump` mode the tool reopens the DB (running recovery), scans it with an
iterator, checks every value's filler, checks that `Get` agrees with the scan
for all 20,000 keys, and prints the op id behind every live key plus the
table count per level.

## Harness

Each cycle:

1. starts the writer with the DB directory left by the previous cycle;
2. kills it with `SIGKILL` on POSIX or `TerminateProcess` on Windows. In 85% of
   cycles the kill comes 50 ms to 1.5 s after the writer reports it is ready;
   in the other 15% it comes 0 to 300 ms after process start, which often lands
   inside recovery (WAL replay, tail repair, flushing a recovered memtable);
3. runs `dump`. A failed open, a damaged value or a scan/`Get` disagreement
   fails the cycle;
4. replays the logged batches on top of the previous cycle's verified state and
   requires the dumped state to equal the state after some prefix of them:
   * `sync`: the prefix must include every acknowledged batch. Only the single
     in-flight batch (logged, not acknowledged) may go either way;
   * `async`: any prefix is accepted, and acknowledged batches beyond it are
     counted as a lost tail. Anything that is not a prefix, such as half a batch
     or a delete applied without the put before it, is a failure.

The verified state becomes the base for the next cycle. Every 50 cycles the
harness starts a fresh DB, so both new and long-lived directories are covered.
The run stops at the first failure and keeps that cycle's log, dump and DB.

## What it can and cannot find

A process kill leaves the OS page cache intact. The campaign therefore tests
everything recovery does with what the process wrote: WAL replay and tail
repair, the manifest, flush and compaction publication, file cleanup, and
batch atomicity. It cannot show that a missing `fsync` loses data on power
loss; that needs a VM or a fault-injecting filesystem, which is out of scope.
The unit tests cover sync ordering with injected failures instead.

As a check that the harness detects real recovery bugs, WAL replay was changed
locally to skip the final record. The sync campaign failed on its first cycle
("recovered state is not a valid prefix of the history"). The output is
`results/m6/mutation-replay-skips-last-record.json`. The change was reverted
and is not in the repository.

## Running it

```sh
python python/crash_test.py --executable build/debug/lsm_crash_writer --mode sync \
  --cycles 500 --work build/crash/sync --output build/crash/sync.json
```

`--seed` makes the kill points and the writer's operations reproducible for a
given binary; exact timing still varies between runs. CI runs 40 cycles per
mode in each debug and ASan job.
