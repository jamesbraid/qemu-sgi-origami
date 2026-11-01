#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Lint the SGI pages of the manual for the conventions the contributor
guide (docs/devel/sgi-manual-pages.rst) sets down.

    scripts/ci/sgi-docs-lint.py [--root DIR]

Prints file:line: message for each finding and exits 1 if there are any.
"""

import argparse
import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
PAGES = 'docs/system/mips/sgi'
GUIDE = 'docs/devel/sgi-manual-pages.rst'
# Split so the licence check does not read this line as the file's own
# SPDX header naming a different licence.
SPDX = '.. SPDX-' 'License-Identifier: BSD-3-Clause'

RATINGS = ('established', 'observed', 'software expectation', 'inferred',
           'approximation', 'unknown')
# Italic phrases that read as a rating but are not one.
NEAR_MISSES = ('establish', 'approximated', 'approximate', 'estimated',
               'assumed', 'infered', 'expectation', 'software expected',
               'observation')

# Hardware pages whose opening differs from the "Used in" paragraph: the
# graphics and brick pages are overviews of a family of parts.
USED_IN_EXEMPT = ('graphics.rst', 'bricks.rst')

LABEL = re.compile(r'\.\. _[^:]+:\s*$')
ADORN = re.compile(r'([=\-~^"#*+])\1{2,}\s*$')
LITERAL = re.compile(r'``.*?``|:\w+:`[^`]*`|`[^`]*`')
ITALIC = re.compile(r'(?<![*\w])\*([^*\s](?:[^*]*[^*\s])?)\*(?![*\w])')


def find_title(lines):
    """Return (index of title text, title) or None."""
    for i, line in enumerate(lines):
        if i + 1 < len(lines) and line.strip() and not ADORN.match(line) \
                and ADORN.match(lines[i + 1]) \
                and (i == 0 or ADORN.match(lines[i - 1]) or
                     not lines[i - 1].strip()):
            return i, line.strip()
    return None


def first_content(lines):
    """Index of the first line that is not blank or part of a comment."""
    comment = False
    for i, line in enumerate(lines):
        if not line.strip():
            continue
        if comment and line.startswith(' '):
            continue
        comment = line.startswith('.. ') and not LABEL.match(line)
        if not comment:
            return i
    return None


def lint_page(rel, text):
    out = []
    lines = text.split('\n')

    def add(n, msg):
        out.append(f'{rel}:{n}: {msg}')

    if lines[0].rstrip() != SPDX:
        add(1, f'first line must be "{SPDX}"')

    title = find_title(lines)
    first = first_content(lines)
    if first is None or not LABEL.match(lines[first]) or \
            (title and title[0] < first):
        add((first or 0) + 1, 'a top-level label must precede the title')

    name = Path(rel)
    in_hw = name.parent.name == 'hardware'
    if not title:
        add(1, 'no title found')
    else:
        n, t = title[0] + 1, title[1]
        if in_hw:
            if ': ' not in t:
                add(n, f'part title "{t}" must read "<Part>: <function>"')
            elif t.endswith(')'):
                add(n, f'part title "{t}" must not end in a parenthesised '
                    'suffix')
        elif re.match(r'ip\d+\.rst$', name.name) and \
                not re.match(r'IP\d+: ', t):
            add(n, f'platform title "{t}" must match "IPnn: products"')

    if title and in_hw and name.name not in USED_IN_EXEMPT:
        j = title[0] + 2
        if ADORN.match(lines[title[0] - 1] if title[0] else ''):
            j += 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j >= len(lines) or not lines[j].startswith('Used in'):
            add(j + 1, 'the first paragraph after the title must begin '
                'with "Used in"')

    in_block = False
    for n, line in enumerate(lines, 1):
        if in_block:
            if line.strip() and not line.startswith(' '):
                in_block = False
            else:
                continue
        if line.rstrip().endswith('::') or line.startswith('.. code-block'):
            in_block = True
            continue
        for m in ITALIC.finditer(LITERAL.sub('', line)):
            word = m.group(1)
            if word in RATINGS:
                continue
            if word.lower() in RATINGS or word.lower() in NEAR_MISSES:
                add(n, f'italic "*{word}*" is not a rating; use one of the '
                    'six in sgi-ratings')
    return out


def lint(root):
    listed = set((root / 'LICENSE.origami.paths').read_text().split())
    findings = []
    pages = sorted((root / PAGES).rglob('*.rst'))
    for page in pages:
        rel = str(page.relative_to(root))
        findings += lint_page(rel, page.read_text())
    for page in pages + [root / GUIDE]:
        rel = str(page.relative_to(root))
        if page.exists() and rel not in listed:
            findings.append(f'{rel}:1: not listed in LICENSE.origami.paths')
    return findings


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', type=Path, default=SOURCE)
    args = ap.parse_args(argv)
    findings = lint(args.root)
    for f in findings:
        print(f)
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
