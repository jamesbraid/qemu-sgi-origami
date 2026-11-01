#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Lint the SGI pages of the manual for the conventions the contributor
guide (docs/devel/sgi-manual-pages.rst) sets down.

    scripts/ci/sgi-docs-lint.py [--root DIR]

Prints file:line: message for each finding and exits 1 if there are any.
"""

import argparse
import json
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
# An italic phrase starting with one of these, in any case, is a rating or
# a variant of one (a plural, an inflection, a capital, a qualifier), so
# it must be one of the six exactly.
RATING_STEMS = ('establish', 'observ', 'software expect', 'infer',
                'approximat', 'unknown')
# Italic phrases that read as a rating but share no stem with one.
NEAR_MISSES = ('estimated', 'assumed', 'infered', 'expectation',
               'expectations')

# Directives whose body is literal text, not prose.
LITERAL_DIRECTIVE = re.compile(
    r'\s*\.\. (code-block|code|sourcecode|literalinclude|parsed-literal)::')
DIRECTIVE = re.compile(r'\s*\.\. [\w:-]+::')

# Hardware pages whose opening differs from the "Used in" paragraph: the
# graphics and brick pages are overviews of a family of parts.
USED_IN_EXEMPT = ('graphics.rst', 'bricks.rst')

# Phrases that put a plan, not the current model, on a page. "not yet" is
# a roadmap only before a verb that says what the model does or the page
# records; "not yet delivered" or "not yet acknowledged" describes a
# hardware state and is fine.
ROADMAP = re.compile(
    r'\b(?:yet to|planned|for now|in a later|in future)\b'
    r'|\bwill be\s+(?:modell?ed|supported|implemented|added|emulated)\b'
    r'|\bnot yet (?:implemented|modell?ed|supported|emulated|handled|'
    r'covered|wired|described|documented|built|'
    r'offered|exposed|understood|measured|captured|reviewed|'
    r'verified|cleared|settled|public|held|tested|checked|decided)\b'
    r'|\b(?:support|model|implement)\w*(?: \w+){0,2} yet\b'
    r'|\bTODO\b', re.I)
# Text that quotes a source or a guest message rather than the page's own
# words: a double-quoted span, which may wrap across lines, and literals.
QUOTED = re.compile(r'"[^"]*"|\u201c[^\u201d]*\u201d|``.*?``', re.S)

# A sentence that states what a register holds or returns as a hex value.
# The page must rate it or cite it in the same paragraph. Many such
# sentences describe the model, so this only warns.
HEX_CLAIM = re.compile(
    r'(?:reset value|resets? to|reads?(?: back| as)?|returns?|reports?|'
    r'powers? up (?:as|at|to)|hardware value)\b[^.\n]*?\b0x[0-9a-fA-F]{2,}',
    re.I)

CATALOGUE = 'hw/mips/sgi/sgi-machines.json'
# A literal in a "Used in" paragraph is checked when it starts with the
# name of a machine family (origin, onyx, ...) or is followed by "machine"
# or "topology". Other literals are device ids and presets.
MACHINE_WORDS = ('machine', 'machines', 'topology', 'topologies')
PRODUCT_NUMBER = re.compile(r'\b(?:Origin|Onyx) ?\d+\b', re.I)

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


def prose_lines(lines):
    """Yield (line number, line) for the lines that are prose.

    Skip literal text only: a code or literal directive's body and the
    block after a paragraph ending in "::". Other directives, such as
    list-table, seealso and note, hold prose and are included.
    """
    block_indent = None
    for n, line in enumerate(lines, 1):
        indent = len(line) - len(line.lstrip())
        if block_indent is not None:
            if line.strip() and indent <= block_indent:
                block_indent = None
            else:
                continue
        if LITERAL_DIRECTIVE.match(line) or (
                line.rstrip().endswith('::') and not DIRECTIVE.match(line)):
            block_indent = indent
            if LITERAL_DIRECTIVE.match(line):
                continue
            line = line.rstrip()[:-2]
        yield n, line


def paragraphs(lines):
    """Yield (first line number, text) for each run of prose lines."""
    run = []
    last = 0
    for n, line in prose_lines(lines):
        if not line.strip() or (run and n != last + 1):
            if run:
                yield run[0][0], '\n'.join(t for _, t in run)
            run = []
        if line.strip():
            run.append((n, line))
        last = n
    if run:
        yield run[0][0], '\n'.join(t for _, t in run)


def blank(m):
    """Replace a match by spaces, keeping its newlines."""
    return re.sub(r'[^\n]', ' ', m.group(0))


def has_rating_or_citation(para):
    if ':src:' in para:
        return True
    return any(m.group(1) in RATINGS
               for m in ITALIC.finditer(LITERAL.sub('', para)))


def warn_page(rel, text):
    """Return warnings: hex value statements with no rating or citation."""
    out = []
    for first, para in paragraphs(text.split('\n')):
        flat = ' '.join(para.split())
        if flat.startswith(('* -', '- ')) and '* -' in flat:
            continue  # a table row: its columns carry the rating
        if HEX_CLAIM.search(flat) and not has_rating_or_citation(para):
            out.append(f'{rel}:{first}: warning: a hex value is stated '
                       'with no rating word or :src: in the paragraph')
    return out


def product_numbers(text):
    """The numbered products in text, such as origin3000, spaces dropped."""
    return {m.group(0).replace(' ', '').lower()
            for m in PRODUCT_NUMBER.finditer(text)}


def catalogue(root):
    """Return (machine and topology names, product names with a number)
    that the model offers, or None if the catalogue is absent."""
    path = root / CATALOGUE
    if not path.exists():
        return None
    data = json.loads(path.read_text())
    names = set()
    products = set()
    for o in data.get('offerings', []):
        names.add(o['product'])
        names.add(o['machine-options'].split(',')[0])
        products.update(product_numbers(o['display-name'] + ' ' + o['product']))
    for t in data.get('topologies', []):
        names.add(t['name'])
        names.add(t['product'])
        products.update(product_numbers(t['name'] + ' ' + t['product']))
    return names, products


def used_in_paragraph(lines):
    """Return (line number, text) of the paragraph starting "Used in"."""
    for n, text in paragraphs(lines):
        if text.startswith('Used in'):
            return n, text
    return None


def lint_used_in(rel, text, cat):
    """Check that machines named in "Used in" are in the catalogue."""
    out = []
    found = used_in_paragraph(text.split('\n'))
    if not found or cat is None:
        return out
    names, products = cat
    families = {re.match(r'[a-z]+', n).group(0) for n in names}
    first, para = found

    def add(offset, what):
        out.append(f'{rel}:{first + para.count(chr(10), 0, offset)}: '
                   f'"Used in" names {what}, which is not a machine in '
                   f'{CATALOGUE}')

    for m in re.finditer(r'``([^`]+)``(\s+\w+)?', para):
        word = m.group(1)
        follower = (m.group(2) or '').strip().lower()
        if word in names or not re.fullmatch(r'[a-z][a-z0-9-]*', word):
            continue
        if follower in MACHINE_WORDS or \
                re.match(r'[a-z]+', word).group(0) in families:
            add(m.start(), f'``{word}``')
    plain = re.sub(r'``[^`]*``', blank, para)
    for m in PRODUCT_NUMBER.finditer(plain):
        if m.group(0).replace(' ', '').lower() not in products:
            add(m.start(), f'"{m.group(0)}"')
    return out


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
            elif '(' in t:
                add(n, f'part title "{t}" must not name a machine or '
                    'anything else in parentheses')
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

    for n, line in prose_lines(lines):
        for m in ITALIC.finditer(LITERAL.sub('', line)):
            word = m.group(1)
            if word in RATINGS:
                continue
            low = word.lower()
            if low.startswith(RATING_STEMS) or low in NEAR_MISSES:
                add(n, f'italic "*{word}*" is not a rating; use one of the '
                    'six in sgi-ratings exactly')

    for first, para in paragraphs(lines):
        masked = QUOTED.sub(blank, para)
        for m in ROADMAP.finditer(masked):
            add(first + masked.count('\n', 0, m.start()),
                f'"{" ".join(m.group(0).split())}" reads as a plan; state what the model '
                'does now, or what the sources leave unknown')
    return out


def warnings(root):
    out = []
    for page in sorted((root / PAGES).rglob('*.rst')):
        out += warn_page(str(page.relative_to(root)), page.read_text())
    return out


def lint(root):
    listed = set((root / 'LICENSE.origami.paths').read_text().split())
    findings = []
    pages = sorted((root / PAGES).rglob('*.rst'))
    cat = catalogue(root)
    for page in pages:
        rel = str(page.relative_to(root))
        findings += lint_page(rel, page.read_text())
        if page.parent.name == 'hardware':
            findings += lint_used_in(rel, page.read_text(), cat)
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
    for w in warnings(args.root):
        print(w, file=sys.stderr)
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
