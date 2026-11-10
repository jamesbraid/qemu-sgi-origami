#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check facts the SGI manual pages state against the machine catalogue.

    scripts/ci/sgi-docs-model-check.py [--root DIR] [--coverage]

The pages state machine names, topologies, processor identification words,
reset mode words and which board goes in which machine. The catalogue
(hw/mips/sgi/sgi-machines.json) is what the emulator offers, generated from
the model inputs under hw/mips/sgi/models/. This reads the list-tables and
command lines of the pages, compares each stated fact with the catalogue
and prints file:line: message for each disagreement, exiting 1 if there is
one. A page that disagrees is stale or the model is wrong; the check does
not say which.

Every table is found by its header row, wherever it sits, so moving a table
does not hide it. --coverage prints which tables were checked.
"""

import argparse
import json
import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
PAGES = 'docs/system/mips/sgi'
CATALOGUE = 'hw/mips/sgi/sgi-machines.json'
LAYERS = 'hw/mips/sgi/models/layers'

LITERAL = re.compile(r'``([^`]+)``')
HEX = re.compile(r'0x[0-9a-fA-F]+')
NAME = re.compile(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*')
PARTS = ('R10000', 'R12000', 'R14000', 'R16000')


class Table:
    def __init__(self, page, line, header, rows):
        self.page = page
        self.line = line
        self.header = header
        self.rows = rows    # [(line, [cell text, ...])]


def indent_of(line):
    return len(line) - len(line.lstrip())


def tables(page, text):
    """Yield the list-tables of an rst page that have a header row."""
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        if not lines[i].lstrip().startswith('.. list-table::'):
            i += 1
            continue
        base = indent_of(lines[i])
        start = i + 1
        j = start
        header_rows = 0
        while j < len(lines) and (not lines[j].strip() or
                                  indent_of(lines[j]) > base):
            stripped = lines[j].strip()
            if stripped.startswith(':header-rows:'):
                header_rows = int(stripped.split(':')[2])
            if stripped.startswith('* -'):
                break
            j += 1
        rows = []
        row = None
        while j < len(lines) and (not lines[j].strip() or
                                  indent_of(lines[j]) > base):
            line = lines[j]
            stripped = line.strip()
            if stripped.startswith('* -'):
                row = (j + 1, [stripped[3:].strip()])
                rows.append(row)
            elif (stripped == '-' or stripped.startswith('- ')) and \
                    row is not None and \
                    indent_of(line) == indent_of(lines[row[0] - 1]) + 2:
                # A bare "-" is an empty cell.
                row[1].append(stripped[2:].strip())
            elif stripped and row is not None:
                row[1][-1] += ' ' + stripped
            j += 1
        if header_rows == 1 and rows:
            yield Table(page, start, rows[0][1], rows[1:])
        i = j


def literals(cell):
    return LITERAL.findall(cell)


def hexes(cell):
    return {int(h, 16) for h in HEX.findall(cell)}


def plain(cell):
    return ' '.join(LITERAL.sub(r'\1', cell).split())


class Catalogue:
    def __init__(self, data, layer_names=()):
        self.offerings = data['offerings']
        # A topology name repeats once for each board the machine takes.
        self.topology_list = data['topologies']
        self.topologies = {t['name']: t for t in self.topology_list}
        self.products = {o['product'] for o in self.offerings}
        self.machines = self.products | {
            o['machine-options'].split(',')[0] for o in self.offerings}
        self.reference_names = {r['name'] for r in data.get('references', [])}
        self.layer_names = set(layer_names)
        # Every name a page may use for a machine, topology or composition.
        self.known = (self.machines | set(self.topologies) |
                      self.reference_names | self.layer_names)

    def offering(self, product):
        return [o for o in self.offerings if o['product'] == product]

    def topologies_of(self, name):
        """Topologies a machine product or a topology name stands for."""
        if name in self.products:
            return [t for t in self.topology_list if t['product'] == name]
        return [t for t in self.topology_list if t['name'] == name]

    def default_topology(self, product):
        found = {o.get('default-topology') for o in self.offering(product)}
        return found.pop() if len(found) == 1 else None

    def assemblies(self, name):
        """The CPU assemblies a machine or topology can fit."""
        out = []
        for t in self.topologies_of(name):
            for node in t.get('nodes', []):
                for slot in node.get('cpu-slots', []):
                    if 'assembly' in slot:
                        out.append(slot['assembly'])
            for option in t.get('processor-options', []):
                if 'assembly' in option:
                    out.append(option['assembly'])
        return out

    def parts(self, name):
        """The CPU parts a machine or topology can fit."""
        out = {a['kind'] for a in self.assemblies(name)}
        for o in self.offerings:
            if o['product'] == name or \
                    o['machine-options'].split(',')[0] == name:
                out.add(o['default-cpu-model'])
        return out


def product_of(cat, display):
    """Map a product's display name such as "Origin 300" to its id."""
    key = display.replace(' ', '').lower()
    return key if key in cat.products else None


def cpu_findings_row(out, table, line, what, doc, model):
    if doc != model:
        out.append(f'{table.page}:{line}: {what}: page says '
                   f'{fmt(doc)}, model has {fmt(model)}')


def fmt(values):
    if isinstance(values, (set, frozenset)):
        return '{' + ', '.join(sorted(
            hex(v) if isinstance(v, int) else str(v) for v in values)) + '}'
    return hex(values) if isinstance(values, int) else str(values)


# Each check takes a Table and the catalogue and returns findings. HANDLERS
# maps a header predicate to a check; a table matching none is not checked.

def check_machine_names(table, cat):
    """Machines named in a table's first column exist in the catalogue."""
    out = []
    for line, cells in table.rows:
        for word in literals(cells[0]):
            if NAME.fullmatch(word) and word not in cat.known and \
                    re.match(r'(origin|onyx|octane|fuel|tezro|o2)', word):
                out.append(f'{table.page}:{line}: ``{word}`` is not a '
                           f'machine, topology or composition in the model')
    return out


def check_topology_column(table, cat):
    """Topologies listed beside a machine belong to it; "default" holds."""
    out = []
    column = 1
    for line, cells in table.rows:
        machines = [w for w in literals(cells[0]) if w in cat.products]
        if not machines:
            continue
        product = machines[0]
        text = cells[column]
        listed = []
        for m in LITERAL.finditer(text):
            word = m.group(1)
            if not NAME.fullmatch(word):
                continue
            listed.append(word)
            belongs = [t['name'] for t in cat.topologies_of(product)]
            if word not in belongs:
                out.append(f'{table.page}:{line}: ``{word}`` is listed '
                           f'under ``{product}`` but the model has '
                           f'{", ".join(belongs)}')
        default = cat.default_topology(product)
        if listed and default:
            if '(default)' in text:
                marked = re.search(r'``([^`]+)``\s*\(default\)', text)
                if marked and marked.group(1) != default:
                    out.append(f'{table.page}:{line}: page makes '
                               f'``{marked.group(1)}`` the default '
                               f'topology of ``{product}``, model has '
                               f'``{default}``')
            elif 'default first' in ' '.join(table.header):
                if listed[0] != default:
                    out.append(f'{table.page}:{line}: page lists '
                               f'``{listed[0]}`` first for ``{product}``, '
                               f'model default is ``{default}``')
    return out


def check_default_cpu(table, cat):
    """The default -cpu column names the parts the listed machines fit."""
    out = []
    for line, cells in table.rows:
        machines = [w for w in literals(cells[0])
                    if w in cat.machines or w in cat.topologies]
        if not machines:
            continue
        named = {p for p in PARTS if p in cells[-1]}
        defaults = set()
        possible = set()
        for machine in machines:
            for o in cat.offerings:
                if o['product'] == machine or \
                        o['machine-options'].split(',')[0] == machine:
                    defaults.add(o['default-cpu-model'])
            possible |= cat.parts(machine)
        for part in sorted(defaults - named):
            out.append(f'{table.page}:{line}: default part {part} of '
                       f'{", ".join(machines)} is not in the Default '
                       f'-cpu column')
        for part in sorted(named - possible):
            out.append(f'{table.page}:{line}: page gives {part} for '
                       f'{", ".join(machines)}, which the model never fits')
    return out


def check_cpu_types(table, cat):
    """Each machine listed under a CPU type can fit that part."""
    out = []
    for line, cells in table.rows:
        part = (literals(cells[0]) or [''])[0]
        if part not in PARTS:
            continue
        for word in literals(cells[1]):
            if word in cat.machines and part not in cat.parts(word):
                out.append(f'{table.page}:{line}: ``{word}`` is listed '
                           f'under {part}, but the model fits '
                           f'{", ".join(sorted(cat.parts(word)))}')
    return out


def check_machine_prid(table, cat):
    """Machine, PRId and FPU word per machine against the CPU assemblies."""
    out = []
    for line, cells in table.rows:
        for machine in literals(cells[0]):
            if machine not in cat.machines:
                continue
            assemblies = cat.assemblies(machine)
            if not assemblies:
                continue
            for what, key, doc in (('PRId', 'prid', hexes(cells[1])),
                                   ('FPU word', 'fpu-prid', hexes(cells[2]))):
                # An assembly that leaves the field out takes the CPU
                # part's default, which the catalogue does not hold.
                values = {a.get(key) for a in assemblies}
                if None in values or 0 in values:
                    continue
                cpu_findings_row(out, table, line,
                                 f'{what} of ``{machine}``', doc, values)
    return out


def check_processor_options(table, cat):
    """The processor= table: part, revision, PRId, cache and mode word."""
    out = []
    options = {}
    for t in cat.topology_list:
        for option in t.get('processor-options', []):
            options[option['assembly']['id']] = option
    for line, cells in table.rows:
        ident = (literals(cells[0]) or [''])[0]
        if ident not in options:
            out.append(f'{table.page}:{line}: ``{ident}`` is not a '
                       f'processor module in the model')
            continue
        assembly = options[ident]['assembly']
        part = next((p for p in PARTS if p in cells[1]), None)
        cpu_findings_row(out, table, line, f'part of ``{ident}``', part,
                         assembly['kind'])
        if 'prid' in assembly:
            cpu_findings_row(out, table, line, f'PRId of ``{ident}``',
                             hexes(cells[1]), {assembly['prid']})
        rev = re.search(r'rev (\d+)\.(\d+)', cells[1])
        if rev and 'prid' in assembly:
            cpu_findings_row(
                out, table, line, f'revision of ``{ident}``', rev.group(0),
                'rev %d.%d' % ((assembly['prid'] >> 4) & 15,
                               assembly['prid'] & 15))
        cache = re.search(r'(\d+) MiB at (\d+) MHz', cells[2])
        if cache:
            cpu_findings_row(
                out, table, line, f'cache of ``{ident}``',
                (int(cache.group(1)), int(cache.group(2))),
                (assembly['cache-mib'], assembly['cache-clock-hz'] // 10**6))
        cpu_findings_row(out, table, line, f'mode word of ``{ident}``',
                         hexes(cells[4]), {assembly['mode-bits']})
        counts = {int(n) for n in re.findall(r'\d+', cells[3])}
        cpu_findings_row(out, table, line, f'processors per module of '
                         f'``{ident}``', counts,
                         set(options[ident]['cpus-per-node']))
    return out


def check_o2_identity(table, cat):
    """The O2 comparison table's R12000 PRId is the o2plus assembly's."""
    out = []
    assemblies = [a for a in cat.assemblies('o2plus')]
    for line, cells in table.rows:
        if cells[0].startswith('R12000 identity') and assemblies:
            cpu_findings_row(out, table, line, 'PRId of the ``o2`` R12000',
                             hexes(cells[2]) - {0x900},
                             {a['prid'] for a in assemblies if 'prid' in a})
    return out


def revision_text(revision):
    if not revision:
        return 'none'
    return '%(major)d.%(minor)d.%(patch)d' % revision


def check_ip35_identity(table, cat):
    """L1 type, L1 revision and flash machine type per product."""
    out = []
    for line, cells in table.rows:
        product = product_of(cat, cells[0])
        topologies = cat.topologies_of(product) if product else []
        if not topologies:
            continue
        topology = cat.topologies[cat.default_topology(product)] \
            if cat.default_topology(product) in cat.topologies \
            else topologies[0]
        l1 = (topology.get('controllers') or [{}])[0].get('l1', {})
        firmware = topology.get('ip35-firmware', {})
        cpu_findings_row(out, table, line, f'L1 type of {cells[0]}',
                         hexes(cells[2]), {l1.get('reported-type')})
        doc_rev = cells[3].strip()
        if doc_rev != 'none' and not re.fullmatch(r'\d+\.\d+\.\d+', doc_rev):
            doc_rev = None
        if doc_rev is not None:
            cpu_findings_row(out, table, line, f'L1 revision of {cells[0]}',
                             doc_rev, revision_text(l1.get('revision')))
        if cells[4].strip().isdigit():
            cpu_findings_row(out, table, line,
                             f'flash machine type of {cells[0]}',
                             int(cells[4]), firmware.get('machine-type'))
    return out


def check_ip53_inputs(table, cat):
    """Clock, cache and reset mode word of the IP53 and IP59 machines."""
    out = []
    for line, cells in table.rows:
        names = [w for w in literals(cells[0])
                 if w in cat.machines or w in cat.topologies]
        if not names:
            continue
        assemblies = cat.assemblies(names[0])
        if not assemblies:
            continue
        mhz = {int(n) for n in re.findall(r'(\d+) MHz', cells[1])}
        clocks = {a['clock-hz'] // 10**6 for a in assemblies}
        for value in sorted(mhz - clocks):
            out.append(f'{table.page}:{line}: page gives {value} MHz for '
                       f'``{names[0]}``, model has {fmt_set(clocks)}')
        caches = {int(n) for n in re.findall(r'(\d+) MiB', cells[3])}
        model = {a['cache-mib'] for a in assemblies}
        for value in sorted(caches - model):
            out.append(f'{table.page}:{line}: page gives {value} MiB cache '
                       f'for ``{names[0]}``, model has {fmt_set(model)}')
        words = hexes(cells[4])
        wrong = words - {a['mode-bits'] for a in assemblies if 'mode-bits' in a}
        if wrong:
            out.append(f'{table.page}:{line}: reset mode {fmt(wrong)} for '
                       f'``{names[0]}`` is not one the model fits: '
                       f'{fmt({a["mode-bits"] for a in assemblies})}')
    return out


def fmt_set(values):
    return '{' + ', '.join(str(v) for v in sorted(values)) + '}'


def graphics_devices(cat):
    """Map each graphics device to the machine and topology names offering it."""
    out = {}
    for t in cat.topology_list:
        for slot in t.get('graphics', {}).get('slots', []):
            for family in slot.get('families', []):
                names = out.setdefault(family['device'], set())
                names.update({t['name'], t['product']})
    return out


def check_graphics_boards(table, cat):
    """Machines listed for a graphics device offer a board of that device."""
    out = []
    devices = graphics_devices(cat)
    for line, cells in table.rows:
        device = (literals(cells[1]) or [''])[0]
        if device not in devices:
            continue
        listed = [w for w in literals(cells[2])
                  if w in cat.known and (w in cat.machines or
                                         w in cat.topologies)]
        # "only on request" names a board a user adds with -device; the
        # catalogue lists the boards a machine offers, so it cannot confirm
        # that.
        requested = set(re.findall(r'``([^`]+)``\s+only on request',
                                   cells[2]))
        for word in listed:
            if word in requested:
                continue
            if word not in devices[device]:
                out.append(f'{table.page}:{line}: ``{word}`` is listed for '
                           f'``{device}`` but the model offers it no such '
                           f'board')
        rest = LITERAL.sub('', cells[2])
        rest = re.sub(r'only on request|and|[,;\s]', '', rest)
        if not rest:
            for name in sorted(devices[device]):
                if name in cat.machines and name not in listed and \
                        not any(t in listed for t in
                                [x['name'] for x in cat.topologies_of(name)]):
                    out.append(f'{table.page}:{line}: the model offers '
                               f'``{device}`` on ``{name}``, which the '
                               f'Machines column does not list')
    return out


def header_is(table, *words):
    got = [plain(h) for h in table.header]
    return got[:len(words)] == list(words)


HANDLERS = (
    ('machine names', lambda t: plain(t.header[0]) in (
        'Machine', 'Machines', 'Products', 'Machine and topology',
        'Selection'), check_machine_names),
    ('topologies of a machine', lambda t: header_is(
        t, 'Machine') and len(t.header) > 1 and
        plain(t.header[1]).startswith('Topologies'), check_topology_column),
    ('default -cpu', lambda t: plain(t.header[-1]) == 'Default -cpu',
     check_default_cpu),
    ('CPU type to machines', lambda t: header_is(t, 'CPU type', 'Machines'),
     check_cpu_types),
    ('PRId per machine', lambda t: header_is(
        t, 'Machine', 'PRId', 'FPU word'), check_machine_prid),
    ('processor modules', lambda t: header_is(
        t, 'processor=', 'Part, PRId'), check_processor_options),
    ('O2 identity', lambda t: header_is(
        t, 'Item', 'Published machines', 'o2'), check_o2_identity),
    ('product identity words', lambda t: header_is(
        t, 'Product', 'Board word', 'L1 type', 'L1 revision'),
     check_ip35_identity),
    ('IP53 inputs', lambda t: header_is(
        t, 'Machine and topology', 'Processors', 'Memory', 'Cache each',
        'Reset mode'), check_ip53_inputs),
    ('graphics boards', lambda t: header_is(
        t, 'Board', 'QEMU device', 'Machines'), check_graphics_boards),
)

# A machine on a command line: -M NAME[,topology=T...] in a literal.
COMMAND = re.compile(r'-M\s+([a-z][a-z0-9]*)((?:,[a-z-]+=[A-Za-z0-9._-]+)*)')


def check_commands(page, text, cat):
    """Machine and topology names on documented -M command lines."""
    out = []
    for n, line in enumerate(text.split('\n'), 1):
        for m in COMMAND.finditer(line):
            machine, options = m.group(1), m.group(2)
            if machine == 'none':
                continue
            if machine not in cat.machines:
                out.append(f'{page}:{n}: "-M {machine}" names no machine '
                           f'the model offers')
                continue
            topology = re.search(r',topology=([A-Za-z0-9-]+)', options)
            if topology:
                belongs = {t['name'] for t in cat.topologies_of(machine)}
                if topology.group(1) not in belongs and \
                        topology.group(1) != 'help':
                    out.append(f'{page}:{n}: topology '
                               f'``{topology.group(1)}`` is not one of '
                               f'``{machine}``\'s topologies: '
                               f'{", ".join(sorted(belongs))}')
    return out


MACHINE_WORDS = re.compile(
    r'``([a-z][a-z0-9-]*)``\s+(?:machine|topology)\b(?!\s+option)'
    r'|\b(?:machine|topology)\s+``([a-z][a-z0-9-]*)``')


def check_prose_names(page, text, cat):
    """Literals the text calls a machine or topology exist in the model."""
    out = []
    for n, line in enumerate(text.split('\n'), 1):
        for m in MACHINE_WORDS.finditer(line):
            word = m.group(1) or m.group(2)
            if word not in cat.known and re.match(
                    r'(origin|onyx|octane|fuel|tezro|o2)', word):
                out.append(f'{page}:{n}: ``{word}`` is called a machine or '
                           f'topology but is not in the model')
    return out


TOPOLOGY_WORDS = re.compile(
    r'``([a-z][a-z0-9-]*)``(?:,? and ``[a-z][a-z0-9-]*``)*'
    r'\s+(?:(?:is|are) an? |(?:is|are) the )?topolog(?:y|ies)\b'
    r'(?!\s+option)'
    r'|\btopology\s+``([a-z][a-z0-9-]*)``')


def check_topology_names(page, text, cat):
    """A literal the text calls a topology is a topology the catalogue
    offers, not a machine, a reference composition or a layer name.
    The text is joined across line breaks so a wrapped phrase counts."""
    out = []
    joined = re.sub(r'[ \t]*\n[ \t]*', ' ', text)
    breaks = [m for m in re.finditer(r'[ \t]*\n[ \t]*', text)]
    shifts = [(m.start(), m.end()) for m in breaks]
    cuts = []
    delta = 0
    for a, b in shifts:
        cuts.append(a - delta)
        delta += (b - a) - 1
    for m in TOPOLOGY_WORDS.finditer(joined):
        line = 1 + sum(1 for c in cuts if c < m.start())
        for word in LITERAL.findall(m.group(0)):
            if word in cat.topologies or word == 'help' or \
                    not re.match(r'(origin|onyx|octane|fuel|tezro|o2)',
                                 word):
                continue
            out.append(f'{page}:{line}: ``{word}`` is called a topology '
                       f'but the catalogue offers no topology of that '
                       f'name')
    return out


def layer_names(root):
    names = set()
    for path in (root / LAYERS).rglob('*.yaml'):
        names.add(path.stem)
    return names


def check_pages(pages, cat, covered=None):
    """Check {page: text} against the catalogue; return the findings."""
    out = []
    for page, text in sorted(pages.items()):
        for table in tables(page, text):
            for label, matches, check in HANDLERS:
                if matches(table):
                    out += check(table, cat)
                    if covered is not None:
                        covered.append((label, page, table.line))
        out += check_commands(page, text, cat)
        out += check_prose_names(page, text, cat)
        out += check_topology_names(page, text, cat)
    return out


def load(root):
    data = json.loads((root / CATALOGUE).read_text())
    cat = Catalogue(data, layer_names(root))
    pages = {str(p.relative_to(root)): p.read_text()
             for p in sorted((root / PAGES).rglob('*.rst'))}
    return pages, cat


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', type=Path, default=SOURCE)
    ap.add_argument('--coverage', action='store_true',
                    help='list the tables that were checked')
    args = ap.parse_args(argv)
    pages, cat = load(args.root)
    covered = []
    findings = check_pages(pages, cat, covered)
    if args.coverage:
        for label, page, line in covered:
            print(f'{page}:{line}: {label}')
    for finding in findings:
        print(finding)
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
