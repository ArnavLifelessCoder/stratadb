"""Summarize bench/run_benchmarks.py output: medians, spreads and plots.

Writes <prefix>.json (machine-readable), <prefix>.md (the tables the README
quotes) and <prefix>-*.png next to it.
"""

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path

ORDER = [
    "fillseq",
    "fillrandom",
    "readrandom",
    "readseq",
    "ycsb-a",
    "ycsb-b",
    "ycsb-c",
    "ycsb-d",
    "ycsb-e",
    "ycsb-f",
    "bloom10-missing",
    "bloom0-missing",
]


def spread(values):
    values = sorted(values)
    return {
        "median": statistics.median(values),
        "min": values[0],
        "max": values[-1],
        "n": len(values),
    }


def summarize(raw: dict) -> dict:
    groups = defaultdict(list)
    for record in raw["records"]:
        groups[(record["label"], record["engine"])].append(record)
    rows = {}
    for (label, engine), records in groups.items():
        row = {"ops_per_sec": spread([r["ops_per_sec"] for r in records])}
        latencies = [r["latency"] for r in records if r.get("latency", {}).get("count")]
        if latencies:
            for key in ("p50_ns", "p99_ns", "p999_ns"):
                row[key] = spread([lat[key] for lat in latencies])
        if label.startswith("fill") or label.endswith("-fill"):
            # Bytes the process wrote through the OS while loading, per user byte.
            row["write_amp_os"] = spread([r["os_bytes_written"] / r["user_bytes"] for r in records])
            row["write_amp_os_through_close"] = spread(
                [r["os_bytes_written_through_close"] / r["user_bytes"] for r in records]
            )
            if engine == "lsm":
                row["write_amp_engine"] = spread(
                    [r["engine_disk_bytes"] / r["engine_user_bytes"] for r in records]
                )
        if label.endswith("-missing") and engine == "lsm":
            row["data_blocks_read_per_lookup"] = spread(
                [r["data_blocks_read_per_lookup"] for r in records]
            )
        if label.startswith("ycsb"):
            for kind in "RUISM":
                samples = [r[f"latency_{kind}"] for r in records if r[f"latency_{kind}"]["count"]]
                if samples:
                    row[f"{kind}_p50_ns"] = spread([s["p50_ns"] for s in samples])
                    row[f"{kind}_p99_ns"] = spread([s["p99_ns"] for s in samples])
        rows[f"{label}/{engine}"] = row
    return rows


def fmt_ops(value: float) -> str:
    return f"{value / 1000:,.0f}K" if value >= 1000 else f"{value:,.0f}"


def fmt_us(ns: float) -> str:
    return f"{ns / 1000:,.1f}"


def markdown(raw: dict, rows: dict, engines) -> str:
    lines = []
    params = raw["parameters"]
    lines.append(
        f"Median of {params['runs']} runs; min-max in brackets. {params['num']:,} keys for "
        f"fills and YCSB, {params['reads']:,} point reads, {params['ycsb_ops']:,} ops per YCSB "
        f"workload, {params['bloom_num']:,} keys for the bloom test."
    )
    lines.append("")
    header = "| Workload | " + " | ".join(f"{e} ops/s | {e} p50 us | {e} p99 us" for e in engines)
    lines.append(header + " |")
    lines.append("|---" * (1 + 3 * len(engines)) + "|")
    for label in ORDER:
        cells = [label]
        present = False
        for engine in engines:
            row = rows.get(f"{label}/{engine}")
            if not row:
                cells += ["-", "-", "-"]
                continue
            present = True
            ops = row["ops_per_sec"]
            cells.append(f"{fmt_ops(ops['median'])} [{fmt_ops(ops['min'])}-{fmt_ops(ops['max'])}]")
            cells.append(fmt_us(row["p50_ns"]["median"]) if "p50_ns" in row else "-")
            cells.append(fmt_us(row["p99_ns"]["median"]) if "p99_ns" in row else "-")
        if present:
            lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    lines.append(
        "| Load | Engine | WA (OS bytes, end of load) | WA (OS bytes, through close) "
        "| WA (engine stats) |"
    )
    lines.append("|---|---|---|---|---|")
    for label in ("fillseq", "fillrandom"):
        for engine in engines:
            row = rows.get(f"{label}/{engine}")
            if not row:
                continue
            engine_wa = row.get("write_amp_engine")
            lines.append(
                f"| {label} | {engine} | {row['write_amp_os']['median']:.2f} | "
                f"{row['write_amp_os_through_close']['median']:.2f} | "
                f"{engine_wa['median']:.2f} |"
                if engine_wa
                else f"| {label} | {engine} | {row['write_amp_os']['median']:.2f} | "
                f"{row['write_amp_os_through_close']['median']:.2f} | n/a |"
            )
    lines.append("")
    lines.append("| Missing-key reads | Engine | ops/s | data blocks read per lookup |")
    lines.append("|---|---|---|---|")
    for bits in (10, 0):
        for engine in engines:
            row = rows.get(f"bloom{bits}-missing/{engine}")
            if not row:
                continue
            blocks = row.get("data_blocks_read_per_lookup")
            lines.append(
                f"| bloom {bits} bits/key | {engine} | {fmt_ops(row['ops_per_sec']['median'])} | "
                f"{blocks['median']:.3f} |"
                if blocks
                else f"| bloom {bits} bits/key | {engine} | "
                f"{fmt_ops(row['ops_per_sec']['median'])} | not exposed |"
            )
    return "\n".join(lines) + "\n"


def plot(rows: dict, engines, prefix: Path) -> list:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    written = []
    labels = [label for label in ORDER if all(f"{label}/{e}" in rows for e in engines)]
    figure, axis = plt.subplots(figsize=(11, 4.5))
    width = 0.8 / len(engines)
    for i, engine in enumerate(engines):
        medians = [rows[f"{label}/{engine}"]["ops_per_sec"]["median"] for label in labels]
        lows = [
            m - rows[f"{label}/{engine}"]["ops_per_sec"]["min"]
            for label, m in zip(labels, medians, strict=True)
        ]
        highs = [
            rows[f"{label}/{engine}"]["ops_per_sec"]["max"] - m
            for label, m in zip(labels, medians, strict=True)
        ]
        positions = [x + i * width for x in range(len(labels))]
        axis.bar(positions, medians, width, yerr=[lows, highs], capsize=2, label=engine)
    axis.set_xticks([x + width * (len(engines) - 1) / 2 for x in range(len(labels))])
    axis.set_xticklabels(labels, rotation=30, ha="right")
    axis.set_ylabel("ops/sec (median, min-max bars)")
    axis.set_yscale("log")
    axis.legend()
    axis.set_title("Throughput by workload")
    figure.tight_layout()
    path = prefix.with_name(prefix.name + "-throughput.png")
    figure.savefig(path, dpi=130)
    written.append(path)
    plt.close(figure)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("raw", type=Path)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()
    raw = json.loads(args.raw.read_text(encoding="utf-8"))
    engines = sorted({r["engine"] for r in raw["records"]}, key=lambda e: e != "lsm")
    rows = summarize(raw)
    args.prefix.parent.mkdir(parents=True, exist_ok=True)
    summary = {
        "source": args.raw.name,
        "machine": raw["machine"],
        "settings": raw["settings"],
        "parameters": raw["parameters"],
        "rows": rows,
    }
    args.prefix.with_suffix(".json").write_text(json.dumps(summary, indent=1) + "\n", "utf-8")
    args.prefix.with_suffix(".md").write_text(markdown(raw, rows, engines), "utf-8")
    if not args.no_plots:
        for path in plot(rows, engines, args.prefix):
            print(path)
    print(args.prefix.with_suffix(".md").read_text("utf-8"))


if __name__ == "__main__":
    main()
