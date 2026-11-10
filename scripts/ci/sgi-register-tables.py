#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract the register tables of the SGI manual pages that tests check.

    scripts/ci/sgi-register-tables.py [--root DIR] [--json | --coverage]

A page marks a list-table of registers whose reset values a qtest reads
back from the model with one or more comments just above it:

    .. sgi-registers: base=0x1f280000 width=64
       qemu="-M o2 -m 64M -nic none"

    .. list-table::
       :header-rows: 1

       * - Offset
         - Name
         - Reset
       ...

The marker is a reST comment, so Sphinx ignores it. Its words are
key=value pairs; a value with spaces is quoted and the comment may
continue on indented lines. Keys:

  qemu     the arguments tests/qtest/sgi-register-reset-test.c starts
           QEMU with (required);
  base     the address that table offsets are added to (default 0);
  width    the access width in bits when the table has no Width column;
  map      PATH:INDEX:ADDRESS, a sysbus device's MMIO region mapped before
           the reads, for a device started on its own (repeatable);
  variant  the label that picks a reset from a cell that lists several,
           such as "0; 0x8118 on revision F" (default: the plain one);
  skip     OFFSET[:reason] items (comma list) the test leaves out. A row
           whose Reset cell is prose may give no reason; a row with a
           number must, as in skip=0x10:poll-flags, and the page explains
           it. The reason shows in --coverage and in the test log;
  expect   OFFSET:VALUE items (comma list) that replace the documented
           reset with the value the model has, for a row where the page
           states the specification's value and the model differs by a
           documented approximation. The test then pins the model.

A table takes several markers to be read in several configurations. Every
numeric Reset cell is checked. A cell that says "n/a", "none", "unknown"
or "unsupported", or is empty, states no reset value and is reported as
skipped; any other cell that is not a number must be named in skip=, so
nothing is skipped by accident. A skip= or expect= offset that matches no
row is an error, so a stale entry cannot linger.

A passing test shows the page and the model agree, not that either matches
the hardware: the rating beside each value says how well it is known.

With no option this validates every marker and exits 1 on a bad one.
--json prints the tables for the qtest to check, --coverage lists which
register tables carry a marker.
"""

import argparse
import importlib.machinery
import importlib.util
import json
import re
import shlex
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
PAGES = 'docs/system/mips/sgi'
MARKER = re.compile(r'\.\.\s+sgi-registers:(.*)')
KEYS = {'qemu', 'base', 'width', 'map', 'variant', 'skip', 'expect'}
WIDTHS = (8, 16, 32, 64)
NO_RESET = {'', 'n/a', 'none', 'unknown', 'unsupported', '-'}
NUMBER = re.compile(r'(?:0x[0-9a-fA-F]+|\d+)$')
LITERAL = re.compile(r'``([^`]+)``')


def load_model_check():
    # The page-table reader is the one scripts/ci/sgi-docs-model-check.py
    # uses for the catalogue tables; its file name has hyphens.
    path = SOURCE / 'scripts/ci/sgi-docs-model-check.py'
    loader = importlib.machinery.SourceFileLoader('sgi_docs_model_check',
                                                  str(path))
    spec = importlib.util.spec_from_loader('sgi_docs_model_check', loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class Error(Exception):
    pass


def indent_of(line):
    return len(line) - len(line.lstrip())


def markers(lines):
    """Map each list-table's 1-based line to the markers above it.

    Return (map, errors), where a marker not followed by a list-table is
    an error.
    """
    found = {}
    errors = []
    pending = []
    i = 0
    while i < len(lines):
        line = lines[i]
        match = MARKER.fullmatch(line.rstrip()) if line.startswith('..') \
            else None
        if match:
            text = match.group(1)
            j = i + 1
            while j < len(lines) and lines[j].strip() and \
                    indent_of(lines[j]) > 0:
                text += ' ' + lines[j].strip()
                j += 1
            pending.append((i + 1, text))
            i = j
            continue
        if line.strip():
            if line.lstrip().startswith('.. list-table::'):
                found[i + 1] = pending
            else:
                errors += [(n, 'marker is not directly above a list-table')
                           for n, _ in pending]
            pending = []
        i += 1
    errors += [(n, 'marker is not directly above a list-table')
               for n, _ in pending]
    return found, errors


def parse_marker(text):
    try:
        words = shlex.split(text)
    except ValueError as exc:
        raise Error(f'cannot split the marker: {exc}')
    options = {'map': []}
    for word in words:
        key, sep, value = word.partition('=')
        if not sep or key not in KEYS:
            raise Error(f'unknown marker word {word!r}; keys are '
                        + ', '.join(sorted(KEYS)))
        if key == 'map':
            options['map'].append(value)
        elif key in options:
            raise Error(f'{key}= appears twice')
        else:
            options[key] = value
    if 'qemu' not in options:
        raise Error('marker has no qemu=')
    try:
        options['base'] = int(options.get('base', '0'), 0)
        if 'width' in options:
            options['width'] = int(options['width'])
            if options['width'] not in WIDTHS:
                raise ValueError
        raw_skip = options.get('skip', '')
        raw_expect = options.get('expect', '')
        options['skip'] = {}
        for item in raw_skip.split(','):
            if item:
                offset, _, reason = item.partition(':')
                options['skip'][int(offset, 0)] = reason
        options['expect'] = {}
        for item in raw_expect.split(','):
            if item:
                offset, sep, value = item.partition(':')
                if not sep:
                    raise ValueError
                options['expect'][int(offset, 0)] = int(value, 0)
        options['map'] = [parse_map(m) for m in options['map']]
    except ValueError:
        raise Error('base, width, skip, expect or map is not a number of the '
                    'right kind')
    return options


def parse_map(value):
    parts = value.rsplit(':', 2)
    if len(parts) != 3:
        raise ValueError
    return {'path': parts[0], 'index': int(parts[1]),
            'address': hex(int(parts[2], 0))}


def column(header, *names):
    for index, cell in enumerate(header):
        if cell.strip().lower() in names:
            return index
    return None


def offsets(cell, width):
    """List the byte offsets a table's Offset cell names."""
    text = cell.replace('``', '')
    pieces = re.split(r'(0x[0-9a-fA-F]+)', text)
    values = [int(p, 16) for p in pieces[1::2]]
    joints = [re.sub(r'\s+', ' ', p).strip() for p in pieces[2:-1:2]]
    if not values or pieces[0].strip() or pieces[-1].strip():
        raise Error(f'cannot read the offsets in {cell!r}')
    step = width // 8
    result = [values[0]]
    pending_range = None
    for joint, value in zip(joints, values[1:]):
        if joint in ('-', 'to'):
            pending_range = (result.pop(), value)
            result.extend(range(pending_range[0], value + 1, step))
        elif joint == ',':
            result.append(value)
        else:
            raise Error(f'cannot read the offsets in {cell!r}')
    return result


def reset_value(cell, variant):
    """Return ('value', int), ('skip', reason) or ('prose', text)."""
    text = ' '.join(cell.split())
    plain = text.replace('``', '').strip()
    if plain.lower() in NO_RESET:
        return 'skip', plain or 'empty'
    chosen = None
    for segment in text.split(';'):
        segment = segment.strip()
        label = None
        on = re.search(r'\s+on\s+(.+)$', segment)
        if on:
            label = on.group(1).strip().lower()
            segment = segment[:on.start()].strip()
        word = segment.replace('``', '').strip()
        if not NUMBER.match(word):
            return 'prose', text
        if label is None and variant is None or \
                label is not None and label == (variant or '').lower():
            chosen = int(word, 0)
        elif label is None and chosen is None:
            chosen = int(word, 0)
    return 'value', chosen


def build(page, table, marker_text, number):
    """Turn one table and one marker into a case, or raise Error."""
    options = parse_marker(marker_text)
    header = table.header
    c_off = column(header, 'offset')
    c_reset = column(header, 'reset')
    if c_off is None or c_reset is None:
        raise Error('table has no Offset and Reset columns')
    c_name = column(header, 'name')
    c_width = column(header, 'width')
    if c_width is None and 'width' not in options:
        raise Error('table has no Width column and the marker no width=')
    variant = options.get('variant')
    seen_variant = variant is None
    registers = []
    skipped = []
    used = set()
    for line, cells in table.rows:
        need = max(c_off, c_reset, c_width or 0, c_name or 0)
        if len(cells) <= need:
            raise Error(f'{page}:{line}: row has too few cells')
        if c_width is not None:
            found = re.search(r'\d+', cells[c_width])
            width = int(found.group()) if found else None
            if width not in WIDTHS:
                if options.get('width') in WIDTHS and not found:
                    width = options['width']
                else:
                    raise Error(f'{page}:{line}: cannot read the width '
                                f'{cells[c_width]!r}')
        else:
            width = options['width']
        name = ' '.join(cells[c_name].replace('``', '').split()) \
            if c_name is not None else ''
        try:
            where = offsets(cells[c_off], width)
        except Error as exc:
            raise Error(f'{page}:{line}: {exc}')
        kind, got = reset_value(cells[c_reset], variant)
        if kind == 'value' and got is None:
            raise Error(f'{page}:{line}: Reset cell {cells[c_reset]!r} '
                        f'has no value for variant {variant!r}')
        if variant is not None and re.search(
                r'\bon\s+' + re.escape(variant) + r'\b', cells[c_reset],
                re.I):
            seen_variant = True
        for offset in where:
            where_ = f'{page}:{line}: offset {offset:#x}'
            note = None
            if offset in options['expect']:
                used.add(offset)
                if offset in options['skip']:
                    raise Error(f'{where_} is in both skip= and expect=')
                if kind == 'value' and got == options['expect'][offset]:
                    raise Error(f'{where_}: expect= repeats the page value')
                kind_here, got_here = 'value', options['expect'][offset]
                note = f'page says {cells[c_reset]}; the model is pinned'
            elif offset in options['skip']:
                used.add(offset)
                reason = options['skip'][offset]
                if kind == 'value' and not reason:
                    raise Error(f'{where_} has a reset number; give skip= '
                                'a reason, as skip=OFFSET:reason')
                kind_here = 'skip'
                got_here = reason or ('listed in skip=: ' + str(got))
            elif kind == 'prose':
                raise Error(f'{where_}: Reset cell {got!r} is not a '
                            'number; name the offset in skip= if it '
                            'states no reset value')
            else:
                kind_here, got_here = kind, got
            if kind_here == 'value':
                reg = {'line': line, 'name': name, 'offset': hex(offset),
                       'address': hex(options['base'] + offset),
                       'width': width, 'expect': hex(got_here)}
                if note:
                    reg['note'] = note
                registers.append(reg)
            else:
                skipped.append({'line': line, 'name': name,
                                'offset': hex(offset), 'reason': got_here})
    stale = (set(options['skip']) | set(options['expect'])) - used
    if stale:
        raise Error('skip= or expect= names no row at offset '
                    + ', '.join(f'{o:#x}' for o in sorted(stale)))
    if not seen_variant:
        raise Error(f'variant={variant!r} matches no Reset cell')
    if not registers:
        raise Error('no register is checked')
    return {'id': f'{page}:{number}', 'page': page, 'line': number,
            'qemu': options['qemu'], 'maps': options['map'],
            'variant': variant, 'registers': registers,
            'skipped': skipped}


def scan(root):
    """Return (cases, annotated tables, unannotated tables, errors)."""
    check = load_model_check()
    cases, annotated, other, errors = [], [], [], []
    counted = 0
    pages = Path(root) / PAGES
    for path in sorted(pages.rglob('*.rst')):
        page = path.relative_to(pages).as_posix()
        text = path.read_text(encoding='utf-8')
        found, bad = markers(text.split('\n'))
        errors += [f'{page}:{n}: {msg}' for n, msg in bad]
        counted += sum(1 for line in text.split('\n')
                       if line.startswith('.. sgi-registers:'))
        for table in check.tables(page, text):
            header = [h.strip().lower() for h in table.header]
            if not any(h.startswith('offset') for h in header) or \
                    not any('reset' in h for h in header):
                continue
            marks = found.get(table.line, [])
            if not marks:
                other.append(table)
                continue
            annotated.append(table)
            for number, marker in marks:
                try:
                    cases.append(build(page, table, marker, number))
                except Error as exc:
                    text_ = str(exc)
                    errors.append(text_ if text_.startswith(page)
                                  else f'{page}:{number}: {text_}')
    # A marker that produced no case was dropped; never let that pass.
    if not errors and counted != len(cases):
        errors.append(f'{counted} .. sgi-registers: markers but '
                      f'{len(cases)} tables read')
    return cases, annotated, other, errors


def ready(table):
    """Count rows whose Reset cell is a plain number and whose offset reads."""
    header = [h.strip().lower() for h in table.header]
    c_off = next(i for i, h in enumerate(header) if h.startswith('offset'))
    c_reset = next(i for i, h in enumerate(header) if h == 'reset') \
        if 'reset' in header else None
    good = 0
    for _, cells in table.rows:
        if c_reset is None or len(cells) <= max(c_off, c_reset):
            continue
        try:
            offsets(cells[c_off], 32)
        except Error:
            continue
        kind, got = reset_value(cells[c_reset], None)
        if kind == 'value' and got is not None:
            good += 1
    return good


def coverage(cases, annotated, other):
    out = []
    out.append(f'{len(annotated)} annotated and {len(other)} unannotated '
               'register tables with an Offset and a Reset column')
    out.append('')
    out.append('annotated (registers checked, skipped):')
    for case in cases:
        label = case['variant'] and f" [{case['variant']}]" or ''
        out.append(f"  {case['id']}{label}: {len(case['registers'])}, "
                   f"{len(case['skipped'])}")
        rows = {}
        for skip in case['skipped']:
            rows.setdefault(skip['line'], []).append(skip)
        for line, group in rows.items():
            first = group[0]
            more = f' and {len(group) - 1} more' if len(group) > 1 else ''
            out.append(f"      skipped {first['offset']}{more} "
                       f"({first['name']}): {first['reason']}")
        for reg in case['registers']:
            if 'note' in reg:
                out.append(f"      pinned {reg['offset']} "
                           f"({reg['name']}) to {reg['expect']}: "
                           f"{reg['note']}")
    out.append('')
    out.append('unannotated (rows with a plain reset number of rows):')
    for table in sorted(other, key=lambda t: (-ready(t), t.page, t.line)):
        out.append(f'  {table.page}:{table.line}: {ready(table)} of '
                   f'{len(table.rows)}')
    return '\n'.join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--root', default=str(SOURCE))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--json', action='store_true',
                      help='print the checked tables as JSON')
    mode.add_argument('--coverage', action='store_true',
                      help='list annotated and unannotated register tables')
    args = parser.parse_args(argv)
    cases, annotated, other, errors = scan(args.root)
    for message in errors:
        print(message, file=sys.stderr)
    if errors:
        return 1
    if args.json:
        json.dump(cases, sys.stdout, indent=1)
        print()
    elif args.coverage:
        print(coverage(cases, annotated, other))
    else:
        print(f'{len(cases)} register markers over {len(annotated)} tables '
              f'checked; {len(other)} register tables unannotated')
    return 0


if __name__ == '__main__':
    sys.exit(main())
