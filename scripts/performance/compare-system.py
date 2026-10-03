#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare correctness-checked reports from native functional workloads."""

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] /
                       'tests/functional/mips64'))
from lib_performance import compare_reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    args = parser.parse_args()
    try:
        with args.baseline.open(encoding='utf-8') as stream:
            baseline = json.load(stream)
        with args.candidate.open(encoding='utf-8') as stream:
            candidate = json.load(stream)
        compared = compare_reports(baseline, candidate)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f'comparison refused: {error}\n')
    print(json.dumps(compared, indent=2))


if __name__ == '__main__':
    main()
