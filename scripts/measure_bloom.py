"""Record five seeded bloom accuracy trials from the real C++ implementation."""

import argparse
import json
import platform
import subprocess
from datetime import UTC, datetime
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--flags", required=True)
    args = parser.parse_args()
    trials = []
    for seed in range(24301, 24306):
        result = subprocess.run(
            [
                str(args.binary.resolve()),
                f"--seed={seed}",
                "--gtest_filter=BloomTest.MeasuredFalsePositiveRate",
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        print(result.stdout, end="")
        line = next(x for x in result.stdout.splitlines() if x.startswith("BLOOM_RESULT "))
        trials.append(json.loads(line.removeprefix("BLOOM_RESULT ")))
    report = {
        "recorded_utc": datetime.now(UTC).isoformat(),
        "platform": platform.platform(),
        "processor": platform.processor(),
        "compiler": args.compiler,
        "flags": args.flags,
        "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "source_status": subprocess.check_output(["git", "status", "--short"], text=True),
        "trials": trials,
        "total_queries": sum(x["queries"] for x in trials),
        "total_positives": sum(x["positives"] for x in trials),
    }
    report["aggregate_fpr"] = report["total_positives"] / report["total_queries"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
