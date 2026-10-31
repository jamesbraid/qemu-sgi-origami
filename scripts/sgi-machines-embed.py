#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Emit hw/mips/sgi/sgi-machines.json as the C array sgi_machines_json.

The JSON becomes one string literal per source line, each carrying its
line's newline, so a change to the catalogue shows up as the same lines
changing in the generated file. The input is treated as bytes: quotes and
backslashes are backslash-escaped, and any other byte outside printable
ASCII is written as a three-digit octal escape, which cannot run into the
character after it. The result goes to standard output.
"""

import argparse
import sys

ARRAY_NAME = "sgi_machines_json"

# Printable ASCII passes through, apart from the two characters a C string
# literal gives meaning to.
_LITERAL = {b: chr(b) for b in range(0x20, 0x7f)}
_LITERAL[ord('"')] = '\\"'
_LITERAL[ord("\\")] = "\\\\"


def c_literal(line: bytes) -> str:
    """Spell one line's bytes as the body of a C string literal."""
    return "".join(_LITERAL.get(b) or f"\\{b:03o}" for b in line)


def render(data: bytes) -> str:
    lines = data.split(b"\n")
    if lines[-1] == b"":
        # The file's final newline ends the last line; it starts no new one.
        lines.pop()
    body = "".join(f'    "{c_literal(line)}\\n"\n' for line in lines)
    return f"static const char {ARRAY_NAME}[] =\n{body}    ;\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("json", help="the catalogue file to embed")
    opts = parser.parse_args()

    with open(opts.json, "rb") as src:
        text = render(src.read())
    sys.stdout.buffer.write(text.encode("ascii"))


if __name__ == "__main__":
    main()
