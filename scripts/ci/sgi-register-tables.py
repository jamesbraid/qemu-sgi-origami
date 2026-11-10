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
  label    a name for this configuration in the test path, to tell several
           markers on one table apart;
  variant  the label that picks a reset from a cell that lists several,
           such as "0; 0x8118 on revision F" (default: the plain one);
  setup    ADDRESS:VALUE[:WIDTH] items (comma list) written, in order,
           after the maps and before any read: the steps a guest takes to
           make the device answer, such as a Bridge attach (repeatable;
           ADDRESS is physical, WIDTH defaults to 32);
  select   NAME|OFFSET=VALUE[,OFFSET=VALUE...], writes made at the
           table's width just before each read of the row whose Name cell
           is NAME. For a register that is chosen by another register's
           bits at a repeated offset (repeatable, one per row);
  lane     bytes added to every address the test reads or writes (not to
           the offsets printed), for registers kept in one half of a
           wider slot: lane=4 reads a 32-bit register at its slot + 4;
  split    32, to read a 64-bit register as two 32-bit reads, low half at
           the address and high half four above, as a bus window declared
           little-endian delivers it (default: one read of the width);
  access   32 or 64 and so on, the width of every access, whatever the
           Width column says, for a device whose registers are slots read
           whole (a nine-bit register is still read as a doubleword);
  bios     blank, to start the machine with an erased boot PROM image of
           the size a machine needs, for a board that refuses to build
           without one and runs nothing from it (the test appends -bios);
  units    BASE:STRIDE:COUNT, the places that repeat for a row whose Offset
           is relative to its unit, such as "link ``+0x08``" (units=
           0x100:0x40:8 reads it at 0x108, 0x148 and so on);
  xor      a value XORed into every offset before the base and lane are
           added, for a window that places a narrow access by address
           lane (xor=2 for the halfword registers behind a Bridge); the
           offsets printed stay the page's;
  stride   bytes between the registers of a range such as 0x130-0x168
           when that is not the access width (stride=8 for 8-byte slots);
  count    how many copies "n" runs over in an offset such as
           "0x310 + 0x1c*n" (n = 0 to count-1);
  skiprow  NAME:reason, the row whose Name cell is NAME and every offset
           of it left out, for rows that share an offset (repeatable);
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
KEYS = {'qemu', 'base', 'width', 'map', 'variant', 'skip', 'expect',
        'setup', 'select', 'skiprow', 'count', 'label', 'lane',
        'stride', 'units', 'split', 'xor', 'access', 'bios'}
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
    options = {'map': [], 'setup': [], 'select': [], 'skiprow': []}
    for word in words:
        key, sep, value = word.partition('=')
        if not sep or key not in KEYS:
            raise Error(f'unknown marker word {word!r}; keys are '
                        + ', '.join(sorted(KEYS)))
        if key in ('map', 'setup', 'select', 'skiprow'):
            options[key].append(value)
        elif key in options:
            raise Error(f'{key}= appears twice')
        else:
            options[key] = value
    if 'qemu' not in options:
        raise Error('marker has no qemu=')
    try:
        options['base'] = int(options.get('base', '0'), 0)
        options['lane'] = int(options.get('lane', '0'), 0)
        options['xor'] = int(options.get('xor', '0'), 0)
        if 'stride' in options:
            options['stride'] = int(options['stride'], 0)
            if options['stride'] < 1:
                raise ValueError
        if options.get('bios', 'blank') != 'blank':
            raise ValueError
        if 'access' in options:
            options['access'] = int(options['access'])
            if options['access'] not in WIDTHS:
                raise ValueError
        if 'split' in options:
            if options['split'] != '32':
                raise ValueError
            options['split'] = 32
        if 'units' in options:
            base, stride, count = (int(v, 0) for v in
                                   options['units'].split(':'))
            if count < 1 or stride < 1:
                raise ValueError
            options['units'] = [base + stride * n for n in range(count)]
        if 'count' in options:
            options['count'] = int(options['count'])
            if options['count'] < 1:
                raise ValueError
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
        options['setup'] = [step for item in options['setup']
                            for step in parse_setup(item)]
        options['select'] = parse_select(options['select'])
        options['skiprow'] = parse_skiprow(options['skiprow'])
    except ValueError:
        raise Error('a marker value (base, width, access, count, lane, bios, '
                    'xor, stride, units, skip, expect, setup, select or '
                    'skiprow) is not of the right kind')
    return options


def parse_setup(value):
    steps = []
    for item in value.split(','):
        parts = item.split(':')
        if len(parts) not in (2, 3):
            raise ValueError
        width = int(parts[2]) if len(parts) == 3 else 32
        if width not in WIDTHS:
            raise ValueError
        steps.append({'address': hex(int(parts[0], 0)),
                      'value': hex(int(parts[1], 0)), 'width': width})
    return steps


def parse_select(items):
    chosen = {}
    for item in items:
        name, bar, writes = item.partition('|')
        if not bar or not name or name in chosen:
            raise ValueError
        chosen[name] = []
        for write in writes.split(','):
            offset, eq, value = write.partition('=')
            if not eq:
                raise ValueError
            chosen[name].append((int(offset, 0), int(value, 0)))
    return chosen


def parse_skiprow(items):
    rows = {}
    for item in items:
        name, colon, reason = item.rpartition(':')
        if not colon or not name or not reason or name in rows:
            raise ValueError
        rows[name] = reason
    return rows


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


def offsets(cell, width, count=None, stride_bytes=None, units=None):
    """List the byte offsets a table's Offset cell names."""
    text = cell.replace('``', '')
    stride = re.fullmatch(r'\s*(0x[0-9a-fA-F]+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)'
                          r'\s*\*\s*n\s*', text)
    if stride:
        if count is None:
            raise Error(f'{cell!r} needs count= in the marker')
        return [int(stride.group(1), 16) + int(stride.group(2), 0) * n
                for n in range(count)]
    unit = re.fullmatch(r'\s*[A-Za-z]+\s+(\+0x[0-9a-fA-F]+'
                        r'(?:\s*,\s*\+0x[0-9a-fA-F]+)*)\s*', text)
    if unit:
        if units is None:
            raise Error(f'{cell!r} needs units= in the marker')
        return [first + int(rel, 16) for first in units
                for rel in re.findall(r'0x[0-9a-fA-F]+', unit.group(1))]
    pieces = re.split(r'(0x[0-9a-fA-F]+)', text)
    values = [int(p, 16) for p in pieces[1::2]]
    joints = [re.sub(r'\s+', ' ', p).strip() for p in pieces[2:-1:2]]
    if not values or pieces[0].strip() or pieces[-1].strip():
        raise Error(f'cannot read the offsets in {cell!r}')
    step = stride_bytes or width // 8
    result = [values[0]]
    pending_range = None
    for joint, value in zip(joints, values[1:]):
        if joint in ('-', 'to'):
            pending_range = (result.pop(), value)
            result.extend(range(pending_range[0], value + 1, step))
        elif joint in (',', '/'):
            result.append(value)
        else:
            raise Error(f'cannot read the offsets in {cell!r}')
    return result


def reset_value(cell, variant):
    """Classify a Reset cell.

    Return ('value', int), ('list', [int, ...]), ('ones', None) for "all
    ones", ('skip', reason) or ('prose', text).
    """
    text = ' '.join(cell.split())
    plain = text.replace('``', '').strip()
    if plain.lower() in NO_RESET:
        return 'skip', plain or 'empty'
    parts = [p.strip() for p in plain.split(',')]
    if plain.lower() == 'all ones':
        return 'ones', None
    # "not specified; model 0": the specification gives none, the page
    # states the model's choice.
    chosen = re.fullmatch(r'not specified; model (0x[0-9a-fA-F]+|\d+)',
                          plain, re.I)
    if chosen:
        return 'value', int(chosen.group(1), 0)
    if len(parts) > 1 and ';' not in plain and \
            all(NUMBER.match(p) for p in parts):
        # "0x00010000, 0" beside two offsets: one value for each.
        return 'list', [int(p, 0) for p in parts]
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
    if c_width is None and 'width' not in options and \
            'access' not in options:
        raise Error('table has no Width column and the marker no width=')
    variant = options.get('variant')
    seen_variant = variant is None
    registers = []
    skipped = []
    used = set()
    selected = set()
    skipped_rows = set()
    for line, cells in table.rows:
        need = max(c_off, c_reset, c_width or 0, c_name or 0)
        if len(cells) <= need:
            raise Error(f'{page}:{line}: row has too few cells')
        per_offset = None
        if 'access' in options:
            width = options['access']
        elif c_width is not None:
            found = re.search(r'\d+', cells[c_width])
            width = int(found.group()) if found else None
            if '/' in cells[c_width]:
                # "64 / 32" beside "0x08 / 0x0c": one width for each offset.
                per_offset = [int(w) for w in
                              re.findall(r'\d+', cells[c_width])]
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
            where = offsets(cells[c_off], width, options.get('count'),
                            options.get('stride'), options.get('units'))
        except Error as exc:
            raise Error(f'{page}:{line}: {exc}')
        kind, got = reset_value(cells[c_reset], variant)
        if kind == 'value' and got is None:
            kind, got = 'nothing', cells[c_reset]
        if variant is not None and re.search(
                r'\bon\s+' + re.escape(variant) + r'\b', cells[c_reset],
                re.I):
            seen_variant = True
        if per_offset is not None and len(per_offset) != len(where):
            raise Error(f'{page}:{line}: {len(per_offset)} widths for '
                        f'{len(where)} offsets')
        if kind == 'list' and len(got) != len(where):
            raise Error(f'{page}:{line}: {len(got)} reset values for '
                        f'{len(where)} offsets')
        if len(where) > 64 and (kind == 'skip' or name in options['skiprow']):
            # A window with no reset value: one entry, not one per slot.
            reason = got
            if kind != 'skip':
                skipped_rows.add(name)
                reason = options['skiprow'][name]
            skipped.append({'line': line, 'name': name,
                            'offset': hex(where[0]),
                            'reason': f'{reason} ({len(where)} slots)'})
            continue
        kind_row, got_row = kind, got
        for index, offset in enumerate(where):
            if kind_row == 'list':
                kind, got = 'value', got_row[index]
            if per_offset is not None:
                width = per_offset[index]
            if kind_row == 'ones':
                kind, got = 'value', (1 << width) - 1
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
            elif name in options['skiprow']:
                skipped_rows.add(name)
                kind_here = 'skip'
                got_here = options['skiprow'][name]
            elif offset in options['skip']:
                used.add(offset)
                reason = options['skip'][offset]
                if kind == 'value' and not reason:
                    raise Error(f'{where_} has a reset number; give skip= '
                                'a reason, as skip=OFFSET:reason')
                kind_here = 'skip'
                got_here = reason or ('listed in skip=: ' + str(got))
            elif kind == 'nothing':
                raise Error(f'{where_}: Reset cell {got!r} has no value '
                            f'for variant {variant!r}')
            elif kind == 'prose':
                raise Error(f'{where_}: Reset cell {got!r} is not a '
                            'number; name the offset in skip= if it '
                            'states no reset value')
            else:
                kind_here, got_here = kind, got
            if kind_here == 'value':
                if options.get('split') and width != 64:
                    raise Error(f'{where_}: split=32 reads 64-bit registers '
                                f'only, not {width}-bit')
                reg = {'line': line, 'name': name, 'offset': hex(offset),
                       'address': hex(options['base'] + options['lane']
                                      + (offset ^ options['xor'])),
                       'width': width, 'expect': hex(got_here)}
                if name in options['select']:
                    selected.add(name)
                    reg['select'] = [
                        {'address': hex(options['base'] + options['lane']
                                        + (off ^ options['xor'])),
                         'width': width, 'value': hex(val)}
                        for off, val in options['select'][name]]
                if options.get('split'):
                    reg['split'] = options['split']
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
    if set(options['skiprow']) - skipped_rows:
        raise Error('skiprow= names no row: '
                    + ', '.join(sorted(set(options['skiprow'])
                                       - skipped_rows)))
    if set(options['select']) - selected:
        raise Error('select= names no checked row: '
                    + ', '.join(sorted(set(options['select']) - selected)))
    if not seen_variant:
        raise Error(f'variant={variant!r} matches no Reset cell')
    if not registers:
        raise Error('no register is checked')
    return {'id': f'{page}:{number}', 'page': page, 'line': number,
            'qemu': options['qemu'], 'maps': options['map'],
            'setup': options['setup'],
            'variant': variant, 'label': options.get('label'),
            'bios': options.get('bios'),
            'registers': registers,
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
        if kind in ('ones', 'list') or kind == 'value' and got is not None:
            good += 1
    return good


def coverage(cases, annotated, other):
    out = []
    out.append(f'{len(annotated)} annotated and {len(other)} unannotated '
               'register tables with an Offset and a Reset column')
    out.append('')
    out.append('annotated (registers checked, skipped):')
    for case in cases:
        label = case['variant'] or case['label']
        label = f' [{label}]' if label else ''
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
