"""Generate YCSB core-workload traces for bench/lsm_bench.cpp to replay.

The C++ runner replays the trace so Python never sits on the measured path.
Workloads follow the YCSB core package (workloads/workloada..f):

  A  50% read, 50% update                     zipfian
  B  95% read, 5% update                      zipfian
  C  100% read                                zipfian
  D  95% read, 5% insert                      latest
  E  95% scan (1..100 records), 5% insert     zipfian start keys
  F  50% read, 50% read-modify-write          zipfian

The key space is the --records keys loaded by the fillrandom phase (indices
0..records-1). Inserts take fresh indices from records upward. Zipfian requests
use YCSB's scrambled zipfian (theta 0.99, FNV-64 scrambling), so popular keys
are spread over the key space instead of clustering at small indices. "latest"
draws a zipfian offset back from the newest inserted key.

Trace format, little endian: b"YCSBTRC1", u64 op count, u64 record count, then
per op: u8 type (R, U, I, S or M), u64 key index, u16 scan length.
"""

import argparse
import random
import struct
from pathlib import Path

WORKLOADS = {
    "a": {"R": 0.50, "U": 0.50},
    "b": {"R": 0.95, "U": 0.05},
    "c": {"R": 1.00},
    "d": {"R": 0.95, "I": 0.05},
    "e": {"S": 0.95, "I": 0.05},
    "f": {"R": 0.50, "M": 0.50},
}
THETA = 0.99
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 1099511628211


def fnv64(value: int) -> int:
    """YCSB Utils.fnvhash64: FNV-1 over the 8 bytes of a long, low byte first."""
    hashed = FNV_OFFSET
    for _ in range(8):
        octet = value & 0xFF
        value >>= 8
        hashed = (hashed ^ octet) * FNV_PRIME & 0xFFFFFFFFFFFFFFFF
    return hashed


class Zipfian:
    """YCSB ZipfianGenerator (Gray et al.) over [0, items)."""

    def __init__(self, items: int, rng: random.Random, zetan: float | None = None):
        self.items = items
        self.rng = rng
        self.zeta2 = 1 + 0.5**THETA
        self.zetan = zetan if zetan is not None else zeta(items)
        self.alpha = 1 / (1 - THETA)
        self.eta = (1 - (2 / items) ** (1 - THETA)) / (1 - self.zeta2 / self.zetan)

    def next(self) -> int:
        u = self.rng.random()
        uz = u * self.zetan
        if uz < 1:
            return 0
        if uz < 1 + 0.5**THETA:
            return 1
        return int(self.items * (self.eta * u - self.eta + 1) ** self.alpha)


def zeta(items: int) -> float:
    return sum(1 / (i**THETA) for i in range(1, items + 1))


class ScrambledZipfian:
    """YCSB ScrambledZipfianGenerator: zipfian ranks over a large fixed item
    space, hashed into [0, items)."""

    ITEM_COUNT = 10_000_000_000
    ZETAN = 26.46902820178302  # YCSB's precomputed zeta(10^10, 0.99).

    def __init__(self, items: int, rng: random.Random):
        self.items = items
        self.zipf = Zipfian(self.ITEM_COUNT, rng, zetan=self.ZETAN)

    def next(self) -> int:
        return fnv64(self.zipf.next()) % self.items


def generate(workload: str, records: int, operations: int, seed: int):
    rng = random.Random(seed)
    mix = WORKLOADS[workload]
    kinds = list(mix)
    weights = [mix[k] for k in kinds]
    zipf = ScrambledZipfian(records, rng)
    latest = Zipfian(records, rng) if workload == "d" else None
    inserted = records
    ops = []
    for kind in rng.choices(kinds, weights, k=operations):
        length = 0
        if kind == "I":
            key = inserted
            inserted += 1
        elif workload == "d":
            # Offset back from the newest key, skewed towards recent inserts.
            key = max(0, inserted - 1 - latest.next())
        else:
            key = zipf.next()
        if kind == "S":
            length = rng.randint(1, 100)
        ops.append((kind, key, length))
    return ops


def write_trace(path: Path, records: int, ops) -> None:
    with path.open("wb") as file:
        file.write(b"YCSBTRC1")
        file.write(struct.pack("<QQ", len(ops), records))
        pack = struct.Struct("<cQH").pack
        file.write(b"".join(pack(kind.encode(), key, length) for kind, key, length in ops))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workloads", default="abcdef")
    parser.add_argument("--records", type=int, required=True)
    parser.add_argument("--operations", type=int, required=True)
    parser.add_argument("--seed", type=int, default=24301)
    parser.add_argument("--out", type=Path, required=True, help="directory for trace files")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for offset, workload in enumerate(args.workloads):
        ops = generate(workload, args.records, args.operations, args.seed + offset)
        path = args.out / f"ycsb-{workload}.trace"
        write_trace(path, args.records, ops)
        counts = {kind: sum(1 for op in ops if op[0] == kind) for kind in WORKLOADS[workload]}
        print(f"{path.name}: {len(ops)} ops {counts}")


if __name__ == "__main__":
    main()
