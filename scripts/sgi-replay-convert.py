#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Convert SGI graphics replay fixtures to the current format, in place.

Usage: sgi-replay-convert.py FILE...

Version 1 is the MGRAS replay format from before fixtures carried a
header: MGRAS verbs with no version line and no head record.  Version 2,
which tests/qtest/sgi-replay-test.c reads, adds two leading records,
"sgi-replay 2" and "head CODENAME", and keeps every verb unchanged.  A
version 1 file can only be an MGRAS one, so its head is sgi-mgras.

The two records go after a leading SPDX comment line, or at the top when
there is none.  Every line after them moves down by two, so a line number
noted against the old file (a checkpoint's --end-line or --start-line, an
error message) needs two added.

A file that already starts with a version line is left alone when it is
version 2 and refused otherwise.
"""

import sys

CURRENT = 2


def first_record(lines):
    """Return the words of the first line that is neither blank nor a
    comment, or None."""
    for line in lines:
        words = line.split()
        if words and not words[0].startswith("#"):
            return words
    return None


def convert(text):
    """Return @text as a version 2 fixture, or None when it already is."""
    lines = text.splitlines(keepends=True)
    words = first_record(lines)
    if words and words[0] == "sgi-replay":
        if words[1:] == [str(CURRENT)]:
            return None
        raise ValueError("unsupported version line: " + " ".join(words))
    at = 1 if lines and lines[0].startswith("# SPDX-License-Identifier:") else 0
    if at and not lines[0].endswith("\n"):
        lines[0] += "\n"
    header = [f"sgi-replay {CURRENT}\n", "head sgi-mgras\n"]
    return "".join(lines[:at] + header + lines[at:])


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(argv) == 2 else 2
    status = 0
    for path in argv[1:]:
        try:
            with open(path, encoding="utf-8", newline="") as f:
                converted = convert(f.read())
        except (OSError, ValueError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            status = 1
            continue
        if converted is None:
            print(f"{path}: already version {CURRENT}")
            continue
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(converted)
        print(f"{path}: converted to version {CURRENT}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
