"""Check project C++ formatting without traversing fetched dependencies."""

import subprocess
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    files = sorted(
        str(path)
        for directory in ("include", "src", "tests", "bench")
        for path in (root / directory).rglob("*")
        if path.suffix in {".hpp", ".cpp", ".h", ".cc"}
    )
    if not files:
        raise SystemExit("No C++ source files found")
    return subprocess.run(
        ["clang-format", "--dry-run", "--Werror", *files], cwd=root, check=False
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
