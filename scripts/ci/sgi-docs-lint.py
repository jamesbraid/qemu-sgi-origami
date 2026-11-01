#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Lint the SGI pages of the manual for the conventions the contributor
guide (docs/devel/sgi-manual-pages.rst) sets down.

    scripts/ci/sgi-docs-lint.py [--root DIR]

Prints file:line: message for each finding and exits 1 if there are any.
"""

import argparse
import json
import os
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
# The page must rate it or cite it in the same paragraph.
# The value follows the verb within a few words. A preposition between
# them (a read "at" or "from" an address) means the hex number is where
# the access goes, not what it returns.
HEX_CLAIM = re.compile(
    r'(?:reset value|resets? to|reads?(?: back| as)?|returns?(?! (?:route|ring|'
    r'descriptor|path|queue)\b)|reports?|powers? up (?:as|at|to)|'
    r'hardware value)'
    r'(?:\s+(?!(?:at|from|in|on|of|to|through|by|with|for|into|over)\b)'
    r'[\w`\'-]+){0,4}?\s+`{0,2}0x[0-9a-fA-F]{2,}'
    # Or the other way round: "0x03ff1810 are the reset values".
    r'|\b0x[0-9a-fA-F]{2,}`{0,2}[^.;:\n]{0,80}?\b(?:reset|power-up) values?\b',
    re.I)
# A sentence about what the model or a test does is not a statement about
# the hardware, so it needs no rating. The sentence must say so itself: a
# neighbour that names the model does not cover it.
MODEL_SENTENCE = re.compile(
    r'\bmodel(?:led|s|\'s)?\b|\bLOG_[A-Z_]+|\bbus error\b|\bqtest\b'
    r'|\btests?/|\btrace event\b', re.I)
# A section whose paragraphs describe the tests, not the hardware.
TEST_SECTIONS = ('Tests',)
# The edge of a simple table: its columns carry the rating or describe
# the model.
SIMPLE_TABLE = re.compile(r'=+(?:\s+=+)+\s*$')
LIST_ITEM = re.compile(r'\s*(?:[-*]|\d+\.)\s')
SENTENCE_END = re.compile(r'(?<=[.;:])\s+(?=[A-Z`*(])|\s+(?=-\s)')

CATALOGUE = 'hw/mips/sgi/sgi-machines.json'
# A literal in a "Used in" paragraph is checked when it starts with the
# name of a machine family (origin, onyx, ...) or is followed by "machine"
# or "topology". Other literals are device ids and presets.
MACHINE_WORDS = ('machine', 'machines', 'topology', 'topologies')
PRODUCT_NUMBER = re.compile(r'\b(?:Origin|Onyx) ?\d+\b', re.I)

# Text that points at material outside this repository or at the
# maintainers' own working documents. SGI and IRIX kernel names cannot be
# found generically and are not looked for here.
PRIVATE = (
    (re.compile(r'(?<![\w./-])(?:refs|assets)/'),
     'a path under refs/ or assets/'),
    (re.compile(r'(?<![\w.:/-])/(?:storage|scratch)(?![\w-])'),
     'a host storage or scratch path'),
    (re.compile(r'\bdocs/(?:datasheets|decisions|plans|designs)/'),
     'a path in documents that are not part of this tree'),
    (re.compile(r'(?<![\w./-])[\w-]+\.md\b'),
     'a Markdown document, which this tree does not carry'),
    (re.compile(r'\bD-\d{4}\b'), 'a decision record code'),
    (re.compile(r'\bLibrary\b|\b[Nn]otebooks?\b'),
     'a private reference library or notebook'),
    (re.compile(r'\b[Rr]eviewed datasheets?\b'),
     'a reviewed datasheet, which is a maintainer document'),
)
# /tmp is a plausible path in an example command, so a page's literal
# blocks may use it; prose and comments may not.
TMP_PATH = (re.compile(r'(?<![\w.:/-])/tmp(?![\w-])'), 'a /tmp path')
SOURCE_GLOBS = ('hw/mips/sgi/**/*.[ch]', 'hw/*/sgi-*.[ch]')
# A C string or character literal, or a comment. Only comments are kept.
C_TOKEN = re.compile(
    r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'|/\*.*?\*/|//[^\n]*',
    re.S)

LABEL = re.compile(r'\.\. _[^:]+:\s*$')
LABEL_NAME = re.compile(r'\.\. _([^:]+):\s*$')
# The pages a "Used in" paragraph can send a part page back to: the
# platform pages of the machines (ip27.rst, ..., and the Scalable Node
# overview sn.rst), which sit directly under PAGES.
PLATFORM = re.compile(r'(?:ip\d+|sn)\.rst$')
REF_ROLE = re.compile(r':ref:`(?:[^`<]*<)?([^`>]+?)>?`')
DOC_ROLE = re.compile(r':doc:`(?:[^`<]*<)?([^`>]+?)>?`')
SEEALSO = re.compile(r'\s*\.\. seealso::')
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


def italics(para):
    """Yield (offset, phrase) for each italic phrase outside literals.

    A phrase may wrap across lines; its words are joined by single spaces.
    """
    for m in ITALIC.finditer(LITERAL.sub(blank, para)):
        yield m.start(), ' '.join(m.group(1).split())


def has_rating_or_citation(para):
    if ':src:' in para:
        return True
    return any(word in RATINGS for _, word in italics(para))


def section_of(lines):
    """Return a function giving the section title that line n is in."""
    heads = []
    for i, line in enumerate(lines[:-1]):
        if line.strip() and not ADORN.match(line) and ADORN.match(lines[i + 1]):
            heads.append((i + 1, line.strip()))

    def title(n):
        found = ''
        for first, name in heads:
            if first > n:
                break
            found = name
        return found
    return title


def lint_hex(rel, text):
    """Return errors for a hex value stated about the hardware with no
    rating word or :src: citation in the paragraph.

    Skipped: table rows and simple tables, whose columns carry the rating
    or describe the model; paragraphs in a Tests section; sentences that
    name the model or a test (see MODEL_SENTENCE); and the items of a list whose
    lead-in paragraph ends in a colon and carries the rating. Each other
    list item is checked on its own.
    """
    out = []
    lines = text.split('\n')
    section = section_of(lines)
    lead = ''
    for first, whole in paragraphs(lines):
        before, lead = lead, whole
        flat = ' '.join(whole.split())
        if flat.startswith(('* -', '- ')) and '* -' in flat:
            continue
        if SIMPLE_TABLE.match(whole.split('\n')[0].strip()):
            continue
        if section(first) in TEST_SECTIONS:
            continue
        items = list_items(whole)
        if LIST_ITEM.match(items[0][1]):
            intro = before
        else:
            intro = items[0][1]
        if intro.rstrip().endswith(':') and has_rating_or_citation(intro):
            continue  # "What the kernel does, a *software expectation*:"
        for offset, para in items:
            out += hex_item(rel, first + offset, para)
    return out


def list_items(para):
    """Split a paragraph at its bullet and numbered list items, giving
    (line offset, text) for each, so that an item is checked by itself."""
    items = []
    for i, line in enumerate(para.split('\n')):
        if LIST_ITEM.match(line) or not items:
            items.append([i, []])
        items[-1][1].append(line)
    return [(i, '\n'.join(ls)) for i, ls in items]


def hex_item(rel, first, para):
    """Return the error for one paragraph or list item, as a list."""
    if has_rating_or_citation(para):
        return []
    for sentence in SENTENCE_END.split(' '.join(para.split())):
        if HEX_CLAIM.search(sentence) and not MODEL_SENTENCE.search(sentence):
            return [f'{rel}:{first}: a hex value is stated with no rating '
                    'word or :src: in the paragraph or list item']
    return []


def product_numbers(text):
    """The numbered products in text, such as origin3000, spaces dropped."""
    return {m.group(0).replace(' ', '').lower()
            for m in PRODUCT_NUMBER.finditer(text)}


def page_labels(root):
    """Return ({label: page path relative to root}, {page: its first
    label}) for the SGI pages."""
    where = {}
    top = {}
    for page in sorted((root / PAGES).rglob('*.rst')):
        rel = str(page.relative_to(root))
        for line in page.read_text().split('\n'):
            m = LABEL_NAME.match(line)
            if m:
                where[m.group(1)] = rel
                top.setdefault(rel, m.group(1))
    return where, top


def linked_pages(rel, text, where):
    """The pages a run of text links with :ref: or :doc:."""
    pages = set()
    for m in REF_ROLE.finditer(text):
        if ' '.join(m.group(1).split()) in where:
            pages.add(where[' '.join(m.group(1).split())])
    for m in DOC_ROLE.finditer(text):
        target = m.group(1).strip()
        base = Path(rel).parent if not target.startswith('/') else Path(PAGES)
        pages.add(os.path.normpath(base / (target.lstrip('/') + '.rst')))
    return pages


def seealso_text(lines):
    """The text of every seealso block on a page."""
    out = []
    block = None
    for line in lines:
        if SEEALSO.match(line):
            block = len(line) - len(line.lstrip())
            continue
        if block is not None:
            if line.strip() and len(line) - len(line.lstrip()) <= block:
                block = None
            else:
                out.append(line)
    return '\n'.join(out)


def lint_seealso(rel, text, where):
    """Check that a part page's seealso blocks link every platform page
    its "Used in" paragraph links."""
    lines = text.split('\n')
    found = used_in_paragraph(lines)
    if not found:
        return []
    first, para = found
    used = {p for p in linked_pages(rel, para, where)
            if Path(p).parent == Path(PAGES) and PLATFORM.match(Path(p).name)}
    missing = used - linked_pages(rel, seealso_text(lines), where)
    if not missing:
        return []
    names = ', '.join(sorted(Path(p).stem for p in missing))
    return [f'{rel}:{first}: "Used in" links {names}, which no '
            '".. seealso::" on the page links back']


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


def lint_private(rel, text, literal_tmp_ok=False):
    """Flag references to material that is not in this repository."""
    out = []
    prose = {n for n, _ in prose_lines(text.split('\n'))}
    for pattern, what in PRIVATE + (TMP_PATH,):
        for m in pattern.finditer(text):
            n = text.count(chr(10), 0, m.start()) + 1
            if pattern is TMP_PATH[0] and literal_tmp_ok and n not in prose:
                continue
            out.append(f'{rel}:{n}: "{m.group(0)}" is {what}')
    return out


def c_comments(text):
    """Return the text with everything but comments replaced by blanks."""
    return C_TOKEN.sub(
        lambda m: m.group(0) if m.group(0)[:2] in ('/*', '//')
        else blank(m), text)


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

    if title and in_hw:
        j = title[0] + 2
        if ADORN.match(lines[title[0] - 1] if title[0] else ''):
            j += 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j >= len(lines) or not lines[j].startswith('Used in'):
            add(j + 1, 'the first paragraph after the title must begin '
                'with "Used in"')

    for first, para in paragraphs(lines):
        for offset, word in italics(para):
            if word in RATINGS:
                continue
            low = word.lower()
            if low.startswith(RATING_STEMS) or low in NEAR_MISSES:
                add(first + para.count('\n', 0, offset),
                    f'italic "*{word}*" is not a rating; use one of the '
                    'six in sgi-ratings exactly')

    for first, para in paragraphs(lines):
        masked = QUOTED.sub(blank, para)
        for m in ROADMAP.finditer(masked):
            add(first + masked.count('\n', 0, m.start()),
                f'"{" ".join(m.group(0).split())}" reads as a plan; state what the model '
                'does now, or what the sources leave unknown')
    return out


def lint(root):
    listed = set((root / 'LICENSE.origami.paths').read_text().split())
    findings = []
    pages = sorted((root / PAGES).rglob('*.rst'))
    cat = catalogue(root)
    where, _ = page_labels(root)
    for page in pages:
        rel = str(page.relative_to(root))
        findings += lint_page(rel, page.read_text())
        findings += lint_hex(rel, page.read_text())
        if page.parent.name == 'hardware':
            findings += lint_used_in(rel, page.read_text(), cat)
            findings += lint_seealso(rel, page.read_text(), where)
    for page in pages:
        findings += lint_private(str(page.relative_to(root)),
                                 page.read_text(), literal_tmp_ok=True)
    sources = sorted({p for g in SOURCE_GLOBS for p in root.glob(g)})
    for src in sources:
        findings += lint_private(str(src.relative_to(root)),
                                 c_comments(src.read_text(errors='replace')))
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
