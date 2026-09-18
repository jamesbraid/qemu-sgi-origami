#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Turn a JSON file into a C string constant, one C string literal per
source line so the generated file stays diffable against the JSON it
embeds. Modelled on scripts/shaderinclude.py, which can emit a source
line's bytes verbatim only because GLSL shader source never contains a
double quote or a backslash; JSON is built from string literals that
usually contain both, so this escapes each line's bytes instead.
"""

import sys


_ESCAPES = {
    ord('"'): '\\"',
    ord('\\'): '\\\\',
    ord('\t'): '\\t',
}


def _escape(line: str) -> str:
    return "".join(_ESCAPES.get(ord(ch), ch) for ch in line)


def main(args: list[str]) -> int:
    file_path = args[1]
    varname = "sn_catalogue_json"

    with open(file_path, "r", encoding="utf-8") as source:
        lines = source.read().splitlines()

    with open(sys.stdout.fileno(), "wt", closefd=False, newline="\n") as out:
        print(f"static const char {varname}[] =", file=out)
        for line in lines:
            print(f'    "{_escape(line)}\\n"', file=out)
        print("    ;", file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
