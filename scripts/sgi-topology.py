#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check or rewrite the generated runs of the Origin 3000 topology files."""

import argparse
import pathlib
import sys

from sgi_model.topology_plan import main

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=pathlib.Path,
                        default=ROOT / "hw/mips/sgi/models",
                        help="model-input directory (default: %(default)s)")
    parser.add_argument("--write", action="store_true",
                        help="rewrite the files instead of reporting drift; overwrites "
                        "hand edits inside a generated run")
    args = parser.parse_args()
    try:
        return main(args.inputs, args.write)
    except (OSError, ValueError, KeyError) as error:
        print(f"sgi-topology: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(run())
