"""Run the engine comparison and write every raw result to one JSON file.

For each repetition and engine (the engine order alternates between
repetitions), on a fresh DB directory:

  fillseq      --num keys in order
  fillrandom   --num keys in random order; this DB is then reused by
  readrandom   --reads uniform point reads
  readseq      one full scan
  ycsb a,b,c,f,d,e  --ycsb-ops operations each, in YCSB's recommended order
  bloom        --bloom-num keys (every other index) with 10 and with 0 bloom
               bits, then --reads reads of the missing indices in between,
               with the block cache disabled so every block load is counted

Both engines get the same keys, values, order, seeds and settings (see
SETTINGS). bench/summarize.py turns the raw file into medians and plots.
"""

import argparse
import json
import platform
import shutil
import subprocess
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Shared by both engines. LevelDB's own defaults already match the remaining
# knobs this engine fixes in code: L0 compaction trigger 4, slowdown 8, stop
# 12, 2 MiB table files, 10 MiB L1 and 10x per level.
SETTINGS = {
    "key_bytes": 16,
    "value_bytes": 100,
    "memtable_bytes": 4 << 20,
    "block_bytes": 4096,
    "block_cache_bytes": 64 << 20,
    "bloom_bits_per_key": 10,
    "compression": "none (LevelDB built without Snappy, kNoCompression set)",
    "sync_writes": False,
    "checksums_on_read": "both verify block CRCs on reads from a file",
    "threads": 1,
}


def machine_info(build: Path) -> dict:
    info = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "logical_cpus": __import__("os").cpu_count(),
    }
    if sys.platform == "win32":
        query = (
            "$p = Get-CimInstance Win32_Processor | Select-Object -First 1; "
            "$c = Get-CimInstance Win32_ComputerSystem; "
            "$o = Get-CimInstance Win32_OperatingSystem; "
            "@{cpu=$p.Name.Trim(); cores=$p.NumberOfCores; max_mhz=$p.MaxClockSpeed; "
            "ram_bytes=$c.TotalPhysicalMemory; os=$o.Caption; os_build=$o.BuildNumber} "
            "| ConvertTo-Json"
        )
        output = subprocess.run(
            ["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True
        )
        info.update(json.loads(output.stdout))
    else:
        info["uname"] = " ".join(platform.uname())
        info["lscpu"] = subprocess.run(["lscpu"], capture_output=True, text=True).stdout
        with open("/proc/meminfo", encoding="ascii") as meminfo:
            info["meminfo_total"] = meminfo.readline().strip()
    cache = build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            for name in (
                "CMAKE_CXX_COMPILER:",
                "CMAKE_CXX_COMPILER_VERSION:",
                "CMAKE_BUILD_TYPE:",
                "CMAKE_CXX_FLAGS_RELEASE:",
            ):
                if line.startswith(name):
                    info[name.split(":")[0].lower()] = line.split("=", 1)[1]
    try:
        info["source_commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip()
        info["source_dirty"] = bool(
            subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()
        )
    except (OSError, subprocess.CalledProcessError):
        info["source_commit"] = "unknown"
    return info


def run_phase(args, engine: str, db: Path, phase: str, **extra) -> dict:
    command = [
        str(args.bench),
        f"--engine={engine}",
        f"--db={db}",
        f"--phase={phase}",
        f"--num={extra.get('num', args.num)}",
        f"--reads={args.reads}",
        f"--value_size={SETTINGS['value_bytes']}",
        f"--memtable_bytes={SETTINGS['memtable_bytes']}",
        f"--block_bytes={SETTINGS['block_bytes']}",
        f"--cache_bytes={extra.get('cache_bytes', SETTINGS['block_cache_bytes'])}",
        f"--bloom_bits={extra.get('bloom_bits', SETTINGS['bloom_bits_per_key'])}",
        f"--key_stride={extra.get('key_stride', 1)}",
        f"--seed={args.seed}",
    ]
    if args.cpus:
        command.append(f"--cpus={args.cpus}")
    if "trace" in extra:
        command.append(f"--trace={extra['trace']}")
    began = time.monotonic()
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise SystemExit(f"{engine} {phase} failed: {completed.stderr.strip()}")
    record = json.loads(completed.stdout.strip().splitlines()[-1])
    record["label"] = extra.get("label", phase)
    record["wall_seconds"] = round(time.monotonic() - began, 3)
    print(
        f"  {engine:8} {record['label']:14} {record['ops_per_sec']:>14,.0f} ops/s",
        flush=True,
    )
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bench", type=Path, default=ROOT / "build/release/lsm_bench")
    parser.add_argument("--work", type=Path, required=True, help="scratch directory for DBs")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--engines", nargs="+", default=["lsm", "leveldb"])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--num", type=int, default=10_000_000)
    parser.add_argument("--reads", type=int, default=1_000_000)
    parser.add_argument("--ycsb-ops", type=int, default=1_000_000)
    parser.add_argument("--bloom-num", type=int, default=2_000_000)
    parser.add_argument("--seed", type=int, default=24301)
    parser.add_argument("--cpus", default="", help='process affinity, e.g. "0-7"')
    args = parser.parse_args()
    if sys.platform == "win32" and args.bench.suffix != ".exe":
        args.bench = args.bench.with_suffix(".exe")
    args.work.mkdir(parents=True, exist_ok=True)

    traces = args.work / "traces"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "python/ycsb_driver.py"),
            f"--records={args.num}",
            f"--operations={args.ycsb_ops}",
            f"--seed={args.seed}",
            f"--out={traces}",
        ],
        check=True,
    )

    started = datetime.now(UTC).isoformat()
    records = []
    for run in range(args.runs):
        engines = args.engines if run % 2 == 0 else list(reversed(args.engines))
        for engine in engines:
            print(f"run {run + 1}/{args.runs} {engine}", flush=True)
            db = args.work / f"{engine}-db"

            def phase(name, engine=engine, db=db, run=run, **extra):
                record = run_phase(args, engine, extra.pop("db", db), name, **extra)
                record["run"] = run
                records.append(record)

            phase("fillseq", label="fillseq")
            phase("fillrandom", label="fillrandom")
            phase("readrandom", label="readrandom")
            phase("readseq", label="readseq")
            for workload in "abcfde":
                phase("ycsb", label=f"ycsb-{workload}", trace=traces / f"ycsb-{workload}.trace")
            shutil.rmtree(db, ignore_errors=True)
            for bits in (10, 0):
                bloom_db = args.work / f"{engine}-bloom-{bits}"
                common = {"db": bloom_db, "num": args.bloom_num, "key_stride": 2}
                phase("fillrandom", label=f"bloom{bits}-fill", bloom_bits=bits, **common)
                phase(
                    "readmissing",
                    label=f"bloom{bits}-missing",
                    bloom_bits=bits,
                    cache_bytes=0,
                    **common,
                )
                shutil.rmtree(bloom_db, ignore_errors=True)
            # Persist after every engine so a long campaign is never lost.
            write(args, started, records)
    write(args, started, records)


def write(args, started: str, records: list) -> None:
    result = {
        "started_utc": started,
        "finished_utc": datetime.now(UTC).isoformat(),
        "machine": machine_info(args.bench.parent),
        "settings": SETTINGS,
        "leveldb": "1.23 (99b3c03b3284f5886f9ef9a4ef703d57373e61be), built from source",
        "parameters": {
            "num": args.num,
            "reads": args.reads,
            "ycsb_ops": args.ycsb_ops,
            "bloom_num": args.bloom_num,
            "runs": args.runs,
            "seed": args.seed,
            "cpus": args.cpus or "not pinned",
        },
        "notes": [
            "Windows has no drop_caches equivalent without extra admin tools; the OS file "
            "cache was not cleared between runs. Every data set fits in RAM.",
            "CPU frequency scaling and boost were left on (laptop, not configurable).",
        ],
        "records": records,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
