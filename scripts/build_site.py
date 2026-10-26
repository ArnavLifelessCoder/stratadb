"""Assemble the GitHub Pages site: site/index.html plus the committed results it
reads at load time (benchmark summary, crash campaigns) and the demo GIF."""

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "site")
    args = parser.parse_args()
    out = args.out
    if out.exists():
        shutil.rmtree(out)
    data = out / "data"
    data.mkdir(parents=True)
    shutil.copy(ROOT / "site" / "index.html", out / "index.html")
    shutil.copy(ROOT / "docs" / "explorer-demo.gif", out / "explorer-demo.gif")
    shutil.copy(ROOT / "results" / "m8" / "bench" / "bench-summary.json", data)

    # Every committed python/crash_test.py campaign (M6 on), renamed by
    # milestone so names stay unique. Earlier smoke-test files use another format.
    files = []
    for path in sorted(ROOT.glob("results/m*/**/crash-*.json")):
        run = json.loads(path.read_text(encoding="utf-8"))
        if "totals" not in run or "mode" not in run:
            continue
        milestone = path.relative_to(ROOT / "results").parts[0]
        name = f"{milestone}-{path.parent.name}-{path.name}"
        shutil.copy(path, data / name)
        files.append(name)
    (data / "crash-files.json").write_text(json.dumps(files, indent=1) + "\n", encoding="utf-8")
    (out / ".nojekyll").write_text("", encoding="utf-8")
    print(f"{out}: {len(files)} crash campaigns")


if __name__ == "__main__":
    main()
