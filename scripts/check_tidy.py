"""Run the repository clang-tidy configuration on every library translation unit."""

import argparse
import json
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build/debug"))
    parser.add_argument("--clang-tidy", default="clang-tidy")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    sources = sorted(
        {
            Path(entry["file"]).resolve()
            for entry in database
            if Path(entry["file"]).resolve().is_relative_to(root / "src")
        }
    )
    if not sources:
        raise SystemExit("No library translation units in compilation database")
    result = 0
    for source in sources:
        command = [
            args.clang_tidy,
            str(source),
            "-p",
            str(build),
            f"--config-file={root / '.clang-tidy'}",
        ]
        completed = subprocess.run(command, cwd=root, check=False)
        result |= completed.returncode
    return result


if __name__ == "__main__":
    raise SystemExit(main())
