"""Recreate the hash oracle fixture using the independent mmh3 package."""

import argparse
import subprocess
from pathlib import Path

import mmh3


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    lengths = [*range(34), 47, 48, 49, 63, 64, 65, 255, 256, 257, 4096]
    entries = []
    for length in lengths:
        key = bytes((index * 37 + 11) % 256 for index in range(length))
        for seed in (0, 42, 0xFFFFFFFF):
            expected = mmh3.hash64(key, seed, x64arch=True, signed=False)[0]
            entries.append(f"    {{{length}, {seed}U, 0x{expected:016x}ULL}},")
    source = (
        "#pragma once\n\n#include <array>\n#include <cstddef>\n#include <cstdint>\n\n"
        "namespace lsm::test {\n\n"
        "// Oracle: mmh3 5.2.0, MurmurHash3 x64_128, unsigned low 64 bits.\n"
        "// Key byte i is (i * 37 + 11) % 256. Recreate with scripts/hash_vectors.py.\n"
        "struct HashVector {\n  std::size_t length;\n  std::uint32_t seed;\n"
        "  std::uint64_t expected;\n};\n\n"
        f"inline constexpr std::array<HashVector, {len(entries)}> kHashVectors{{{{\n"
        + "\n".join(entries)
        + "\n}};\n\n}  // namespace lsm::test\n"
    )
    formatted = subprocess.run(
        ["clang-format", "--assume-filename=tests/hash_vectors.hpp"],
        input=source,
        text=True,
        capture_output=True,
        check=True,
        cwd=root,
    ).stdout
    target = root / "tests" / "hash_vectors.hpp"
    if args.check:
        if target.read_text(encoding="utf-8") != formatted:
            raise SystemExit("Hash oracle fixture does not match mmh3")
        print(f"All {len(entries)} hash oracle vectors match mmh3 5.2.0")
    else:
        target.write_text(formatted, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
