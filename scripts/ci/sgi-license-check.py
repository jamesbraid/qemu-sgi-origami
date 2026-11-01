#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check that LICENSE.origami.paths lists the SGI files and that they say so.

LICENSE.origami.paths names the files LICENSE.origami (BSD 3-Clause)
covers. Nothing else ties the list to the tree, so a new SGI file can go
unlisted, carry another licence or have no header. This reports, one
"file: message" line each:

  - a file matching SGI_PATTERNS that the list omits,
  - a listed path that does not exist,
  - a listed or matching file without SPDX-License-Identifier:
    BSD-3-Clause in its first lines (data formats that cannot hold a
    comment are exempt, see NO_COMMENT_EXTENSIONS and NO_COMMENT_FILES),
  - a listed or matching file naming another SPDX licence.

It exits 1 if it reports anything. Upstream QEMU files that merely sit in
an SGI directory and carry GPL are named in UPSTREAM_FILES.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[2]
LIST = 'LICENSE.origami.paths'
LICENCE = 'BSD-3-Clause'
HEADER_LINES = 40

# Where SGI work lives. '**' crosses directories, '*' does not. The
# listed files outside these (the 1-Wire, RTC, SCSI and network device
# models, the R10000 CPU code, the workflows) are listed by name only: a
# new file there must be added to the list by hand.
SGI_PATTERNS = [
    'hw/mips/sgi/**',
    'hw/display/sgi/**',
    'hw/*/sgi-*',
    'hw/*/*sgi*',
    'hw/display/psitech-*',
    'hw/input/psitech-*',
    'hw/1wire/**',
    'include/hw/1wire/**',
    'include/hw/mips/sgi/**',
    'include/hw/mips/sgi-*',
    'include/hw/display/sgi/**',
    'include/hw/*/sgi-*',
    'include/hw/*/psitech-*',
    'tests/qtest/sgi-*',
    'tests/qtest/sn*',
    'tests/qtest/octane*',
    'tests/qtest/o2-*',
    'tests/qtest/psitech*',
    'tests/qtest/fixtures/sgi-*',
    'tests/functional/mips64/**',
    'tests/unit/*sgi*',
    'tests/data/mgras/**',
    'tests/data/sgi-replay/**',
    'scripts/sgi*',
    'scripts/sgi_model/**',
    'scripts/ci/sgi-*',
    'scripts/tests/*sgi*',
    'scripts/tests/system-model-fixtures/**',
    'docs/system/mips/sgi/**',
    'docs/system/devices/sgi-*',
    'docs/devel/sgi-*',
]

# Upstream QEMU files inside SGI_PATTERNS. They keep their GPL header.
UPSTREAM_FILES = {
    'tests/functional/mips64/meson.build',
    'tests/functional/mips64/test_malta.py',
    'tests/functional/mips64/test_tuxrun.py',
}

# Formats that take no comment, so carry no SPDX line. Generated and
# recorded data: a comment would break the parser or the recorded hash.
NO_COMMENT_EXTENSIONS = {'.json', '.sha256'}
NO_COMMENT_FILES = {
    # Licence text, and a one-line expected output compared byte for byte.
    'hw/mips/sgi/models/LICENSE',
    'scripts/tests/system-model-fixtures/format2/origin2000/refused.txt',
}

SPDX = re.compile(r'SPDX-License-Identifier:\s*([^\s*]+)')


def glob_regex(pattern):
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith('**', i):
            out.append('.*')
            i += 2
        elif pattern[i] == '*':
            out.append('[^/]*')
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile(''.join(out) + r'\Z')


PATTERN_RES = [glob_regex(p) for p in SGI_PATTERNS]


def matches(path):
    return path not in UPSTREAM_FILES and any(r.match(path) for r in PATTERN_RES)


def tree_files(root):
    """Return the tracked files under root, or all files outside a repo."""
    try:
        out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                             capture_output=True, check=True).stdout
        return set(os.fsdecode(p) for p in out.split(b'\0') if p)
    except (OSError, subprocess.CalledProcessError):
        found = set()
        for base, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if d != '.git']
            for name in names:
                found.add(os.path.relpath(os.path.join(base, name), root))
        return found


def listed_paths(root):
    text = (Path(root) / LIST).read_text(encoding='utf-8')
    return [line.strip() for line in text.splitlines() if line.strip()]


def licences(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            head = [f.readline() for _ in range(HEADER_LINES)]
    except OSError:
        return []
    return [m.group(1) for line in head for m in SPDX.finditer(line)]


def exempt(path):
    return (Path(path).suffix in NO_COMMENT_EXTENSIONS
            or path in NO_COMMENT_FILES)


def check(root):
    root = Path(root)
    listed = listed_paths(root)
    listed_set = set(listed)
    findings = []
    for path in sorted(set(f for f in tree_files(root) if matches(f))
                       - listed_set):
        findings.append((path, f'matches an SGI path pattern but is not '
                               f'listed in {LIST}'))
    seen = set()
    for path in listed:
        if path in seen:
            findings.append((path, f'listed more than once in {LIST}'))
        seen.add(path)
    for path in sorted(listed_set | set(f for f in tree_files(root)
                                        if matches(f))):
        full = root / path
        if not full.is_file():
            findings.append((path, f'listed in {LIST} but does not exist'))
            continue
        found = licences(full)
        other = sorted(set(found) - {LICENCE})
        if other:
            findings.append((path, 'carries ' + ', '.join(other) +
                             f', not {LICENCE}'))
        elif not found and not exempt(path):
            findings.append((path, f'has no SPDX-License-Identifier: '
                                   f'{LICENCE} in its first {HEADER_LINES} '
                                   'lines'))
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--root', type=Path, default=SOURCE,
                        help='source tree to check (default: this one)')
    args = parser.parse_args(argv)
    findings = check(args.root)
    for path, message in findings:
        print(f'{path}: {message}')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
