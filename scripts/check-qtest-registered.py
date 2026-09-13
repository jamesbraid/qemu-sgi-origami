#!/usr/bin/env python3
"""Reject qtest sources that are absent from the Meson test lists."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
QTEST = ROOT / "tests" / "qtest"
FAMILIES = ("sn0-", "sn1-", "mips64-")


def main() -> int:
    text = "\n".join(
        line for line in (QTEST / "meson.build").read_text().splitlines()
        if not line.lstrip().startswith("#")
    )
    names = set(re.findall(r"'([^']+)'", text))
    problems = []
    for prefix in FAMILIES:
        sources = {p.stem for p in QTEST.glob(f"{prefix}*-test.c") if p.is_file()}
        registered = {n for n in names if n.startswith(prefix)}
        problems.extend(f"{n}.c is not registered" for n in sources - registered)
        problems.extend(f"{n} has no source" for n in registered - sources)
    for problem in sorted(problems):
        print(f"FAIL {problem}")
    if problems:
        return 1
    print("qtest sources are registered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
