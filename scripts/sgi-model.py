#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate the checked SGI system-model catalogue for QEMU."""

import argparse
import pathlib
import sys

from sgi_model.model_qemu import generate_qemu_inputs, install_qemu_inputs

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=pathlib.Path,
                        default=ROOT / "hw/mips/sgi/models",
                        help="model-input directory (default: %(default)s)")
    parser.add_argument("--tree", type=pathlib.Path,
                        help="QEMU tree receiving adopted generated files")
    parser.add_argument("--output", type=pathlib.Path,
                        help="write generated files directly to this directory")
    args = parser.parse_args()
    if args.tree is not None and args.output is not None:
        parser.error("--tree and --output cannot be used together")
    try:
        if args.output is not None:
            outputs = generate_qemu_inputs(args.inputs, args.output)
        else:
            outputs = install_qemu_inputs(args.inputs, args.tree or ROOT)
    except (OSError, ValueError) as error:
        print(f"sgi-model: {error}", file=sys.stderr)
        return 1
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
