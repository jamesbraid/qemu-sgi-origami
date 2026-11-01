#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check scripts/ci/sgi-docs-model-check.py on small fixtures and this tree.

Each check has a page that agrees with the fixture catalogue and a copy with
one stated fact changed, which must be reported.
"""

import importlib.machinery
import importlib.util
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
_LOADER = importlib.machinery.SourceFileLoader(
    'sgi_docs_model_check', str(SOURCE / 'scripts/ci/sgi-docs-model-check.py'))
_SPEC = importlib.util.spec_from_loader('sgi_docs_model_check', _LOADER)
check = importlib.util.module_from_spec(_SPEC)
_LOADER.exec_module(check)


def assembly(ident, kind, prid, mode, mhz, cache, fpu=0x900, **extra):
    return dict(id=ident, kind=kind, prid=prid, **{'mode-bits': mode},
                **{'clock-hz': mhz * 10**6, 'cache-mib': cache,
                   'cache-clock-hz': 120 * 10**6, 'fpu-prid': fpu}, **extra)


R10 = assembly('r10000-180mhz-1mb', 'R10000', 0x926, 0x1211a785, 180, 1)
R12 = assembly('r12000-270mhz-4mb', 'R12000', 0xe23, 0x1213ab85, 270, 4)
R16A = assembly('r16000-700mhz-4mb', 'R16000', 0xf50, 0x9bad85, 700, 4)
R16B = assembly('r16000-1000mhz-16mb', 'R16000', 0xf50, 0xedb385, 1000, 16)


def offering(product, topology, cpu, options=None):
    return {'product': product,
            'machine-options': options or f'{product},topology={topology}',
            'default-topology': topology, 'default-cpu-model': cpu}


def topology(name, product, slots=(), options=(), **extra):
    return dict(name=name, product=product,
                nodes=[{'cpu-slots': [{'assembly': a} for a in slots]}],
                **{'processor-options': [
                    {'assembly': a,
                     'cpus-per-node': [2] if a['kind'] == 'R12000' else [1, 2]}
                    for a in options]}, **extra)


CATALOGUE = {
    'offerings': [
        offering('origin200', 'origin200', 'R10000'),
        offering('origin350', 'origin350', 'R16000'),
        offering('fuel', 'fuel', 'R14000'),
        offering('octane2', 'octane2', 'R12000'),
    ],
    'topologies': [
        topology('origin200', 'origin200', [R10], [R10, R12]),
        topology('origin200-dual', 'origin200', [R10], [R10, R12]),
        # One topology name for each board the machine takes.
        topology('origin350', 'origin350', [R16A]),
        topology('origin350', 'origin350', [R16B]),
        topology('fuel', 'fuel', [dict(R10, kind='R14000')],
                 controllers=[{'l1': {'reported-type': 0x41}}],
                 **{'ip35-firmware': {'machine-type': 2}}),
        topology('octane2', 'octane2', [R12], graphics={'slots': [
            {'families': [{'device': 'sgi-odyssey'}]}]}),
    ],
    'references': [{'name': 'origin2000-xpress', 'product': 'origin2000'}],
}

TABLE_HEAD = '.. list-table::\n   :header-rows: 1\n\n'


def table(*rows):
    out = TABLE_HEAD
    for row in rows:
        out += '   * - ' + '\n     - '.join(row) + '\n'
    return out


GOOD = {
    'machines': table(
        ('Machine', 'Topologies (default first)'),
        ('``origin200``', '``origin200``, ``origin200-dual``'),
        ('``fuel``', '``fuel``')),
    'default-cpu': table(
        ('Products', 'Default ``-cpu``'),
        ('``origin200``', 'R10000, or R12000 with ``processor=``'),
        ('``fuel``', 'R14000')),
    'cpu-types': table(
        ('CPU type', 'Machines'),
        ('``R10000``', '``origin200``'),
        ('``R12000``', '``origin200``, ``octane2``')),
    'prid': table(
        ('Machine', 'PRId', 'FPU word', 'Rating'),
        ('``origin200``', '``0x0926`` or ``0x0e23``', '``0x00000900``',
         'x')),
    'modules': table(
        ('``processor=``', 'Part, PRId', 'Secondary cache',
         'Processors a module', 'Mode word'),
        ('``r10000-180mhz-1mb``', 'R10000 rev 2.6, ``0x0926``',
         '1 MiB at 120 MHz, tap 9', '1 or 2', '``0x1211a785``'),
        ('``r12000-270mhz-4mb``', 'R12000 rev 2.3, ``0x0e23``',
         '4 MiB at 120 MHz, tap 9', '2', '``0x1213ab85``')),
    'identity': table(
        ('Product', 'Board word', 'L1 type', 'L1 revision',
         'Flash machine type'),
        ('Fuel', '``0x4004``', '``0x41`` (``A``)', 'none', '2')),
    'ip53': table(
        ('Machine and topology', 'Processors', 'Memory', 'Cache each',
         'Reset mode'),
        ('``origin350``', 'four 700 MHz R16000', '2 GiB', '4 MiB',
         '``0x009bad85``')),
    'graphics': table(
        ('Board', 'QEMU device', 'Machines'),
        ('VPro', '``sgi-odyssey``', '``octane2``')),
    'command': 'Run ``-M origin200,topology=origin200-dual`` to start.\n',
    'prose': 'The ``origin200`` machine and the ``origin200-dual`` topology.\n',
}


class DocsModelCheckTests(unittest.TestCase):
    def cat(self):
        return check.Catalogue(CATALOGUE)

    def findings(self, key, old=None, new=None):
        text = GOOD[key]
        if old is not None:
            self.assertIn(old, text)
            text = text.replace(old, new)
        return check.check_pages({'docs/x.rst': text}, self.cat())

    def assertClean(self, key):
        self.assertEqual(self.findings(key), [])

    def assertFlags(self, key, old, new, expect):
        out = '\n'.join(self.findings(key, old, new))
        self.assertIn(expect, out)

    def test_fixtures_agree(self):
        for key in GOOD:
            with self.subTest(key=key):
                self.assertClean(key)

    def test_every_check_covers_its_fixture(self):
        covered = []
        for key in GOOD:
            check.check_pages({'p': GOOD[key]}, self.cat(), covered)
        # The O2 table needs its own catalogue; test_o2_identity_row has it.
        self.assertEqual({label for label, _, _ in covered},
                         {label for label, _, _ in check.HANDLERS} -
                         {'O2 identity'})

    def test_unknown_machine_in_table(self):
        self.assertFlags('machines', '``fuel``', '``fuel2``',
                         '``fuel2`` is not a machine')

    def test_topology_of_another_machine(self):
        self.assertFlags('machines', '``origin200-dual``', '``fuel``',
                         '``fuel`` is listed under ``origin200``')

    def test_default_topology_first(self):
        self.assertFlags('machines',
                         '``origin200``, ``origin200-dual``',
                         '``origin200-dual``, ``origin200``',
                         'lists ``origin200-dual`` first')

    def test_default_cpu(self):
        self.assertFlags('default-cpu', 'R14000', 'R12000',
                         'default part R14000 of fuel')
        self.assertFlags('default-cpu', 'R14000', 'R14000 or R16000',
                         'gives R16000 for fuel')

    def test_cpu_type_machine(self):
        self.assertFlags('cpu-types', '``origin200``, ``octane2``',
                         '``origin200``, ``fuel``',
                         '``fuel`` is listed under R12000')

    def test_machine_prid(self):
        self.assertFlags('prid', '``0x0926`` or ``0x0e23``',
                         '``0x0927`` or ``0x0e23``', 'PRId of ``origin200``')
        self.assertFlags('prid', '``0x00000900``', '``0x00000935``',
                         'FPU word of ``origin200``')

    def test_processor_module(self):
        self.assertFlags('modules', 'rev 2.6, ``0x0926``',
                         'rev 2.7, ``0x0927``', 'PRId of')
        self.assertFlags('modules', '``0x1213ab85``', '``0x1213ab86``',
                         'mode word of ``r12000-270mhz-4mb``')
        self.assertFlags('modules', 'R12000 rev 2.3', 'R12000 rev 2.4',
                         'revision of ``r12000-270mhz-4mb``')
        self.assertFlags('modules', '4 MiB at 120', '2 MiB at 120',
                         'cache of ``r12000-270mhz-4mb``')
        self.assertFlags('modules', '``r12000-270mhz-4mb``',
                         '``r12000-300mhz-4mb``', 'not a processor module')
        self.assertFlags('modules', '1 or 2', '3', 'processors per module')

    def test_identity_words(self):
        self.assertFlags('identity', '``0x41``', '``0x42``', 'L1 type')
        self.assertFlags('identity', '     - none\n', '     - 1.2.3\n',
                         'L1 revision')
        self.assertFlags('identity', '     - 2\n', '     - 3\n',
                         'flash machine type')

    def test_ip53_inputs(self):
        self.assertFlags('ip53', 'four 700 MHz', 'four 800 MHz', '800 MHz')
        self.assertFlags('ip53', '4 MiB', '8 MiB', '8 MiB cache')
        self.assertFlags('ip53', '0x009bad85', '0x009bad86', 'reset mode')

    def test_ip53_inputs_use_every_board_of_a_topology(self):
        text = GOOD['ip53'].replace('four 700 MHz', 'four 1000 MHz') \
            .replace('4 MiB', '16 MiB').replace('0x009bad85', '0x00edb385')
        self.assertEqual(check.check_pages({'p': text}, self.cat()), [])

    def test_o2_identity_row(self):
        cat = check.Catalogue(dict(CATALOGUE, topologies=CATALOGUE[
            'topologies'] + [topology('o2plus', 'o2plus', [
                dict(R12, prid=0xe35)])], offerings=CATALOGUE[
                    'offerings'] + [offering('o2plus', 'o2plus', 'R12000')]))
        page = table(('Item', 'Published machines', '``o2``'),
                     ('R12000 identity (PRId)', '``0xe23``',
                      '``0xe35`` and FPU ``0x900``'))
        self.assertEqual(check.check_pages({'p': page}, cat), [])
        out = check.check_pages({'p': page.replace('0xe35', '0xe36')}, cat)
        self.assertIn('PRId of the ``o2`` R12000', '\n'.join(out))

    def test_graphics_board_machines(self):
        self.assertFlags('graphics', '``octane2``', '``fuel``',
                         '``fuel`` is listed for ``sgi-odyssey``')
        self.assertFlags('graphics', '``octane2``', '``octane2``, ``fuel``',
                         '``fuel`` is listed')

    def test_graphics_board_missing_machine(self):
        extra = topology('fuel', 'fuel', graphics={'slots': [
            {'families': [{'device': 'sgi-odyssey'}]}]})
        cat = check.Catalogue(dict(CATALOGUE, topologies=[
            t for t in CATALOGUE['topologies'] if t['name'] != 'fuel'] +
            [extra]))
        out = check.check_pages({'p': GOOD['graphics']}, cat)
        self.assertIn('offers ``sgi-odyssey`` on ``fuel``', '\n'.join(out))

    def test_graphics_only_on_request_is_not_checked(self):
        text = GOOD['graphics'].replace(
            '``octane2``', '``octane2``; ``origin200`` only on request')
        self.assertEqual(check.check_pages({'p': text}, self.cat()), [])

    def test_command_line(self):
        self.assertFlags('command', '-M origin200', '-M origin201',
                         '"-M origin201" names no machine')
        self.assertFlags('command', 'origin200-dual', 'fuel',
                         'topology ``fuel`` is not one of ``origin200``')

    def test_prose_names(self):
        self.assertFlags('prose', 'origin200-dual', 'origin200-trio',
                         '``origin200-trio`` is called a machine')

    def test_machine_option_is_not_a_machine(self):
        text = 'The ``origin300-compat`` machine option turns it on.\n'
        self.assertEqual(check.check_pages({'p': text}, self.cat()), [])

    def test_this_tree(self):
        pages, cat = check.load(SOURCE)
        covered = []
        findings = check.check_pages(pages, cat, covered)
        self.assertEqual(findings, [])
        # A table that is renamed or moved out of the checks must show up.
        self.assertEqual({label for label, _, _ in covered},
                         {label for label, _, _ in check.HANDLERS})


if __name__ == '__main__':
    unittest.main()
