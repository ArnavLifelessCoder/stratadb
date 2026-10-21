"""Kill-and-recover campaign for the storage engine (M6).

Each cycle starts tests/crash_writer.cpp, which writes random puts, deletes and
batches. It logs a batch's intent before calling the DB and an acknowledgement
after the call returns. The harness kills the writer at a random time (SIGKILL
on POSIX, TerminateProcess on Windows), reopens the DB through the dump command
and checks the recovered contents against the logged history:

* The recovered state must equal the state after some prefix of the logged
  batches, applied on top of the previous cycle's verified state. Anything else
  (a torn batch, a resurrected key, a wrong value) is a failure.
* With sync writes the prefix must include every acknowledged batch. Only the
  single in-flight batch may or may not be present.
* With async writes a lost tail of acknowledged batches is allowed and counted.

The DB directory carries over between cycles, so recovery also has to cope with
state left by earlier kills. The campaign stops at the first failure and keeps
that cycle's directory for inspection.
"""

import argparse
import json
import platform
import queue
import random
import shutil
import subprocess
import sys
import threading
import time
from datetime import UTC, datetime
from pathlib import Path


class CycleFailure(Exception):
    pass


def parse_log(path: Path):
    """Return complete batches in log order and the set of acknowledged ids."""
    batches = []
    acked = set()
    lines = path.read_bytes().decode("ascii").split("\n") if path.exists() else []
    # The last element is either "" or a partial line cut by the kill.
    lines = lines[:-1]
    index = 0
    while index < len(lines):
        fields = lines[index].split(" ")
        if fields[0] == "A":
            acked.add(int(fields[1]))
            index += 1
            continue
        if fields[0] != "B":
            raise CycleFailure(f"unexpected log line {lines[index]!r}")
        batch_id, count = int(fields[1]), int(fields[2])
        ops = lines[index + 1 : index + 1 + count]
        if len(ops) < count:
            break  # Intent cut by the kill; Write was never called.
        parsed = []
        for op in ops:
            kind, op_id, key = op.split(" ")
            parsed.append((kind, int(op_id), key))
        batches.append((batch_id, parsed))
        index += 1 + count
    return batches, acked


def parse_dump(path: Path):
    levels = []
    state = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("#levels"):
            levels = [int(x) for x in line.split()[1:]]
            continue
        key, op = line.split(" ")
        state[key] = int(op)
    return state, levels


def match_prefix(base: dict, batches, observed: dict, minimum: int):
    """Smallest prefix length >= minimum whose state equals observed, or None."""
    state = dict(base)
    keys = set(state) | set(observed)
    differ = {key for key in keys if state.get(key) != observed.get(key)}
    for count in range(len(batches) + 1):
        if count >= minimum and not differ:
            return count
        if count == len(batches):
            break
        for kind, op_id, key in batches[count][1]:
            if kind == "P":
                state[key] = op_id
            else:
                state.pop(key, None)
            if state.get(key) != observed.get(key):
                differ.add(key)
            else:
                differ.discard(key)
    return None


def run_cycle(args, rng, directory: Path, cycle_dir: Path, base: dict, ids: dict, sync: bool):
    cycle_dir.mkdir(parents=True, exist_ok=True)
    log = cycle_dir / "writer.log"
    stderr_path = cycle_dir / "writer.stderr"
    early_kill = rng.random() < 0.15
    command = [
        str(args.executable),
        "write",
        str(directory),
        "sync" if sync else "async",
        str(rng.getrandbits(63)),
        str(ids["batch"]),
        str(ids["op"]),
        str(log),
    ]
    with stderr_path.open("wb") as stderr:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stderr)
        ready = queue.Queue()

        def watch_stdout():
            for line in proc.stdout:
                if line.strip() == b"ready":
                    ready.put(time.monotonic())
            ready.put(None)

        watcher = threading.Thread(target=watch_stdout, daemon=True)
        watcher.start()
        started = time.monotonic()
        try:
            if early_kill:
                # Often lands inside recovery: replay, tail repair, or the
                # flush of a recovered immutable memtable.
                time.sleep(rng.uniform(0.0, 0.3))
            else:
                if ready.get(timeout=args.timeout) is None:
                    raise CycleFailure("writer exited before it was ready")
                time.sleep(rng.uniform(args.min_run, args.max_run))
            if proc.poll() is not None:
                raise CycleFailure(f"writer exited on its own with code {proc.returncode}")
            proc.kill()
            proc.wait(timeout=args.timeout)
        except queue.Empty as error:
            proc.kill()
            proc.wait(timeout=args.timeout)
            raise CycleFailure("writer did not become ready in time") from error
        finally:
            watcher.join(timeout=args.timeout)
    lifetime = time.monotonic() - started
    # stderr carries normal recovery notes (tail truncation, deferred cleanup).
    # A real writer error ends the process early, which is caught above.

    batches, acked = parse_log(log)
    ordered = [batch_id for batch_id, _ in batches]
    acknowledged = sum(1 for batch_id in ordered if batch_id in acked)
    if any(batch_id not in acked for batch_id in ordered[:acknowledged]):
        raise CycleFailure("acknowledgements are not a prefix of the intents")
    if len(batches) > acknowledged + 1:
        raise CycleFailure("more than one unacknowledged batch in a single-threaded writer")

    dump_path = cycle_dir / "dump.txt"
    began = time.monotonic()
    completed = subprocess.run(
        [str(args.executable), "dump", str(directory), str(dump_path)],
        capture_output=True,
        timeout=args.timeout,
        check=False,
    )
    recovery_seconds = time.monotonic() - began
    if completed.returncode != 0:
        output = (completed.stdout + completed.stderr).decode("utf-8", "replace")
        raise CycleFailure(f"recovery failed: {output.strip()}")
    observed, levels = parse_dump(dump_path)

    minimum = acknowledged if sync else 0
    prefix = match_prefix(base, batches, observed, minimum)
    if prefix is None:
        raise CycleFailure(
            "recovered state is not a valid prefix of the history "
            f"(acknowledged={acknowledged}, logged={len(batches)})"
        )
    if batches:
        ids["batch"] = batches[-1][0] + 1
        ids["op"] = batches[-1][1][-1][1] + 1
    base.clear()
    base.update(observed)
    return {
        "early_kill": early_kill,
        "lifetime_s": round(lifetime, 3),
        "acknowledged": acknowledged,
        "logged": len(batches),
        "recovered_prefix": prefix,
        "lost_acknowledged": max(0, acknowledged - prefix),
        "in_flight_applied": prefix > acknowledged,
        "live_keys": len(observed),
        "levels": levels,
        "recovery_s": round(recovery_seconds, 3),
    }


def git_commit() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True, help="scratch directory")
    parser.add_argument("--output", type=Path, required=True, help="JSON summary path")
    parser.add_argument("--mode", choices=["sync", "async"], required=True)
    parser.add_argument("--cycles", type=int, default=500)
    parser.add_argument("--cycles-per-db", type=int, default=50)
    parser.add_argument("--seed", type=int, default=24301)
    parser.add_argument("--min-run", type=float, default=0.05)
    parser.add_argument("--max-run", type=float, default=1.5)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--label", default="", help="build label recorded in the output")
    args = parser.parse_args()
    args.executable = args.executable.resolve()
    sync = args.mode == "sync"
    rng = random.Random(args.seed)
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)

    cycles = []
    failure = None
    base: dict = {}
    ids = {"batch": 0, "op": 0}
    database = None
    started = time.monotonic()
    for cycle in range(args.cycles):
        if cycle % args.cycles_per_db == 0:
            if database is not None:
                shutil.rmtree(database)
            database = args.work / f"db-{cycle // args.cycles_per_db}"
            base = {}
            ids = {"batch": 0, "op": 0}
        cycle_dir = args.work / "cycle"
        if cycle_dir.exists():
            shutil.rmtree(cycle_dir)
        try:
            record = run_cycle(args, rng, database, cycle_dir, base, ids, sync)
        except (CycleFailure, subprocess.TimeoutExpired, OSError, ValueError) as error:
            kept = args.work / f"failed-cycle-{cycle}"
            shutil.copytree(cycle_dir, kept / "cycle", dirs_exist_ok=True)
            shutil.copytree(database, kept / "db", dirs_exist_ok=True)
            failure = {"cycle": cycle, "error": str(error), "kept": str(kept)}
            print(f"cycle {cycle}: FAILED: {error}", file=sys.stderr)
            break
        record["cycle"] = cycle
        cycles.append(record)
        if (cycle + 1) % 25 == 0:
            print(f"{args.mode}: {cycle + 1}/{args.cycles} cycles passed", flush=True)

    summary = {
        "recorded_utc": datetime.now(UTC).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "source_commit": git_commit(),
        "executable": args.executable.name,
        "label": args.label,
        "mode": args.mode,
        "seed": args.seed,
        "kill": "SIGKILL" if sys.platform != "win32" else "TerminateProcess",
        "cycles_requested": args.cycles,
        "cycles_passed": len(cycles),
        "failure": failure,
        "elapsed_s": round(time.monotonic() - started, 1),
        "totals": {
            "acknowledged_batches": sum(c["acknowledged"] for c in cycles),
            "lost_acknowledged_batches": sum(c["lost_acknowledged"] for c in cycles),
            "cycles_with_lost_tail": sum(1 for c in cycles if c["lost_acknowledged"]),
            "in_flight_batches_applied": sum(1 for c in cycles if c["in_flight_applied"]),
            "early_kills": sum(1 for c in cycles if c["early_kill"]),
            "max_l0_tables": max((c["levels"][0] for c in cycles if c["levels"]), default=0),
            "max_depth": max((len(c["levels"]) for c in cycles), default=0),
        },
        "cycles": cycles,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "cycles"}, indent=1))
    return 0 if failure is None and len(cycles) == args.cycles else 1


if __name__ == "__main__":
    raise SystemExit(main())
