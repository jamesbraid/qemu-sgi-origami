#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""R10000 processor transactions against a synthetic external agent.

Public contract: MIPS R10000 User's Manual 007-2490-001, pp. 48-58,
113, 127-130 and 141-148. These fixtures contain no firmware.
"""

import re
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest
from test_r10000_tlb_cache import constant, insn_i, mtc0, mfc0

# The synthetic agent overlays one page of Malta RAM (hw/mips/malta.c).
PAGE = 0x100000
MAILBOX = 0x1000
DONE = 0xc0de
ERROR = 0xdbed
VALUE = 0x12345678
VALUE2 = 0x76543210


def fixture(operations, cca=5, route='xkphys'):
    code = constant(8, 0x00410080) + mtc0(8, 12)
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    address = (2 << 62) | (cca << 59) | PAGE
    if route == 'kseg0':
        code += constant(8, cca) + mtc0(8, 16)
        address = 0xffffffff80000000 | PAGE
    elif route == 'tlb':
        code += constant(8, 0x40000000) + mtc0(8, 10)
        code += mtc0(0, 0) + mtc0(0, 5)
        for cp0 in (2, 3):
            code += constant(8, (PAGE >> 6) | (cca << 3) | 7)
            code += mtc0(8, cp0)
        code += [0x42000002, 0, 0, 0, 0]
        address = 0x40000000
    code += constant(10, address)
    code += mtc0(0, 28) + mtc0(0, 29)
    for offset in (0, 32, 64, 96):
        for way in (0, 1):
            for operation in (9, 11):
                code += [insn_i(0x2f, 10, operation, offset + way)] + [0] * 4
    for operation in operations:
        if operation == 'load':
            code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0)]
        elif operation == 'store':
            code += constant(8, VALUE) + [insn_i(0x2b, 10, 8, 0)]
        elif operation == 'store-subset':
            code += constant(8, VALUE2) + [insn_i(0x2b, 10, 8, 32)]
        elif operation == 'k0-5':
            code += constant(8, 5) + mtc0(8, 16)
        elif operation == 'next-line':
            code += constant(10, address + 128)
        elif operation == 'synonym':
            code += constant(10, address + 0x1000)
        elif operation == 'replace':
            code += constant(10, address + 0x80000)
            code += [insn_i(0x23, 10, 8, 0)]
        elif operation == 'writeback':
            code += [insn_i(0x2f, 10, 0x17, 0)] + [0] * 4
        elif operation == 'sync':
            code += [0x0000000f]
        elif operation == 'tag':
            code += [insn_i(0x2f, 10, 7, 0)] + [0] * 4
            code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 4)]
        else:
            raise ValueError(operation)
    code += constant(8, DONE) + [insn_i(0x2b, 12, 8, 32)]
    code += [insn_i(0x04, 0, 0, -1), 0]
    handler = mfc0(8, 13) + [insn_i(0x2b, 12, 8, 8)]
    handler += mfc0(8, 14) + [insn_i(0x3f, 12, 8, 16)]
    handler += constant(8, ERROR) + [insn_i(0x2b, 12, 8, 32)]
    handler += [insn_i(0x04, 0, 0, -1), 0]
    assert len(handler) * 4 <= 0x80
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into(f'>{len(handler)}I', image, 0x380, *handler)
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


class R10000Coherence(QemuSystemTest):
    timeout = 180

    def start(self, operations, cca=5, route='xkphys', line64=False):
        name = f'cpu{len(self._vms)}'
        image = self.scratch_file(name + '.bin')
        Path(image).write_bytes(fixture(operations, cca, route))
        vm = self.get_vm(name=name)
        vm.set_machine('malta')
        cpu = 'R10000,r10k-data-cache=on'
        if line64:
            cpu += ',r10k-scache-line-size=64'
        vm.add_args('-accel', 'tcg,thread=single', '-cpu', cpu,
                    '-m', '128M', '-bios', image, '-nodefaults',
                    '-device', f'r10k-test-agent,id=agent,base={PAGE}')
        vm.launch()
        self.agent = '/machine/peripheral/agent'
        return vm

    def get(self, vm, prop):
        return vm.cmd('qom-get', path=self.agent, property=prop)

    def put(self, vm, prop, value):
        return vm.cmd('qom-set', path=self.agent, property=prop, value=value)

    def command(self, vm, command, identity=None, reject=False):
        if identity is not None:
            self.put(vm, 'id', identity)
        if reject:
            result = vm.qmp('qom-set', path=self.agent,
                            property='command', value=command)
            self.assertIn('error', result)
            return
        self.put(vm, 'command', command)

    def mailbox(self, vm):
        output = vm.cmd('human-monitor-command',
                        command_line=f'xp /9wx {MAILBOX:#x}')
        return [int(word, 16) for word in
                re.findall(r'0x([0-9a-fA-F]{8})\b', output)]

    def wait(self, callback):
        deadline = time.monotonic() + 10
        while not (value := callback()):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        return value

    def request(self, vm, kind, previous=0, address=PAGE):
        identity = self.wait(lambda: (i := self.get(vm, 'request-id'))
                             and i != previous and i)
        self.assertEqual(self.get(vm, 'request-kind'), kind)
        self.assertEqual(self.get(vm, 'request-address'), address)
        return identity

    def data(self, vm, identity, state=2, value=0x1111111122222222):
        self.put(vm, 'offset', 0)
        self.put(vm, 'data-word', value)
        self.put(vm, 'size', self.get(vm, 'request-size'))
        self.put(vm, 'state', state)
        self.command(vm, 2, identity)

    def fill(self, vm, identity, state=2):
        self.command(vm, 1, identity)
        self.data(vm, identity, state)
        self.command(vm, 3, identity)

    def done(self, vm):
        return self.wait(lambda: self.mailbox(vm)[8] == DONE)

    def test_request_matrix(self):
        for cca in (3, 5):
            for route in ('xkphys', 'kseg0', 'tlb'):
                for store in (False, True):
                    vm = self.start(['store' if store else 'load', 'tag'],
                                    cca, route)
                    kind = 5 if cca == 3 else 7 if store else 6
                    identity = self.request(vm, kind)
                    self.assertEqual(self.get(vm, 'request-cca'), cca)
                    self.assertEqual(self.get(vm, 'request-cause'), int(store))
                    self.assertEqual(self.mailbox(vm)[8], 0)
                    self.fill(vm, identity)
                    self.done(vm)
                    self.assertEqual((self.mailbox(vm)[1] >> 10) & 3,
                                     3 if store else 2)
                    self.assertEqual(self.get(vm, 'notifications'), 1)
                    vm.shutdown()

    def test_live_k0_change(self):
        vm = self.start(['load', 'k0-5', 'next-line', 'load'],
                        cca=3, route='kseg0')
        first = self.request(vm, 5)
        self.assertEqual(self.get(vm, 'request-cca'), 3)
        self.fill(vm, first)
        # A fresh line in the same translated page exposes stale TLB attrs.
        second = self.request(vm, 6, first, PAGE + 128)
        self.assertEqual(self.get(vm, 'request-cca'), 5)
        self.fill(vm, second)
        self.done(vm)
        self.assertEqual(self.get(vm, 'notifications'), 2)

    def test_data_and_ack_are_separate(self):
        for state in (1, 2, 3):
            vm = self.start(['load', 'tag'])
            identity = self.request(vm, 6)
            self.command(vm, 1, identity)
            self.command(vm, 3, identity, reject=True)
            self.data(vm, identity, state)
            self.assertEqual(self.mailbox(vm)[8], 0)
            self.command(vm, 3, identity)
            self.done(vm)
            self.assertEqual((self.mailbox(vm)[1] >> 10) & 3, state)
            vm.shutdown()
        vm = self.start(['store'])
        identity = self.request(vm, 7)
        self.command(vm, 1, identity)
        self.put(vm, 'state', 1)
        self.command(vm, 2, identity, reject=True)
        self.command(vm, 5, identity)
        self.wait(lambda: self.mailbox(vm)[8] == ERROR)
        self.assertEqual((self.mailbox(vm)[2] >> 2) & 31, 7)

    def test_nack_and_error(self):
        vm = self.start(['load'])
        identity = self.request(vm, 6)
        self.command(vm, 1, identity)
        self.data(vm, identity)
        self.command(vm, 4, identity)
        next_id = self.request(vm, 6, identity)
        self.assertNotEqual(identity, next_id)
        self.command(vm, 1, next_id)
        self.data(vm, next_id, value=0x3333333344444444)
        self.command(vm, 3, next_id)
        self.done(vm)
        self.assertEqual(self.mailbox(vm)[0], 0x33333333)
        vm.shutdown()
        vm = self.start(['load'])
        identity = self.request(vm, 6)
        self.command(vm, 1, identity)
        self.command(vm, 5, identity)
        self.wait(lambda: self.mailbox(vm)[8] == ERROR)
        self.assertEqual((self.mailbox(vm)[2] >> 2) & 31, 7)

    def test_upgrade_and_invalidation(self):
        for cca in (3, 5):
            vm = self.start(['load', 'store', 'tag'], cca)
            first = self.request(vm, 5 if cca == 3 else 6)
            self.fill(vm, first, 1)
            upgrade = self.request(vm, 8, first)
            self.command(vm, 1, upgrade)
            self.command(vm, 8, 100)
            self.assertEqual(self.get(vm, 'external-state'), 1)
            self.command(vm, 3, upgrade, reject=True)
            self.command(vm, 10, 100)
            self.command(vm, 3, upgrade, reject=True)
            self.data(vm, upgrade, 2)
            self.assertEqual(self.mailbox(vm)[8], 0)
            self.command(vm, 3, upgrade)
            self.done(vm)
            self.command(vm, 7, 101)
            self.assertEqual(self.get(vm, 'external-state'), 3)
            self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)
            vm.shutdown()

    def test_intervention_matrix(self):
        for state in (0, 1, 2, 3):
            for command in (6, 7, 8):
                vm = self.start(['load'] if state < 3 else
                                ['store', 'store-subset'])
                identity = self.request(vm, 6 if state < 3 else 7)
                if state:
                    self.fill(vm, identity, state if state < 3 else 2)
                    self.done(vm)
                self.command(vm, command, 100)
                self.assertEqual(self.get(vm, 'external-state'), state)
                have_data = state == 3 and command != 8
                self.assertEqual(self.get(vm, 'external-have-data'), have_data)
                if have_data:
                    self.assertEqual(self.get(vm, 'external-data-state'),
                                     1 if command == 6 else 3)
                    self.put(vm, 'offset', 0)
                    self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)
                    self.put(vm, 'offset', 32)
                    self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE2)
                    self.command(vm, 10, 100, reject=True)
                    if command == 6:
                        self.command(vm, 16)
                        self.assertEqual(self.get(vm, 'memory-word') >> 32,
                                         VALUE2)
                    self.command(vm, 9, 100)
                self.command(vm, 10, 100)
                self.command(vm, 8, 101)
                self.assertEqual(self.get(vm, 'external-state'),
                                 1 if state and command == 6 else 0)
                vm.shutdown()

    def test_pending_writeback_data_lifetime(self):
        for external in (0, 6, 7, 8):
            vm = self.start(['store', 'store-subset', 'writeback', 'replace'])
            identity = self.request(vm, 7)
            self.fill(vm, identity)
            replacement = self.request(vm, 6, identity, PAGE + 0x80000)
            self.fill(vm, replacement)
            self.done(vm)
            outgoing = self.get(vm, 'writeback-id')
            self.assertNotEqual(outgoing, 0)
            if external:
                self.command(vm, external, 100)
                self.assertEqual(self.get(vm, 'external-state'), 3)
                self.assertEqual(self.get(vm, 'writeback-id'), 0)
                if external != 8:
                    self.assertEqual(self.get(vm, 'external-data-state'), 3)
                    self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)
                    self.command(vm, 9, 100)
                else:
                    self.assertEqual(self.get(vm, 'external-have-data'), 0)
                self.command(vm, 10, 100)
                self.command(vm, 11, outgoing, reject=True)
            else:
                self.command(vm, 1, outgoing)
                self.command(vm, 3, outgoing, reject=True)
                self.command(vm, 11, outgoing)
                self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)
                self.put(vm, 'offset', 32)
                self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE2)
            vm.shutdown()

    def test_sync_waits_for_global_performance(self):
        vm = self.start(['store', 'sync'])
        identity = self.request(vm, 7)
        self.fill(vm, identity)
        self.assertEqual(self.mailbox(vm)[8], 0)
        self.command(vm, 13)
        self.done(vm)
        self.assertEqual(self.get(vm, 'writeback-id'), 0)
        self.command(vm, 7, 100)
        self.assertEqual(self.get(vm, 'external-state'), 3)
        self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)

    def test_pending_issue_conflicts(self):
        vm = self.start(['load', 'store'])
        first = self.request(vm, 6)
        self.command(vm, 6, 100)
        self.assertEqual(self.get(vm, 'external-state'), 0)
        self.command(vm, 1, first, reject=True)
        self.command(vm, 10, 100)
        self.fill(vm, first, 1)
        upgrade = self.request(vm, 8, first)
        self.command(vm, 7, 101)
        self.assertEqual(self.get(vm, 'external-state'), 1)
        self.assertEqual(self.get(vm, 'request-id'), upgrade)
        self.command(vm, 1, upgrade, reject=True)
        self.command(vm, 10, 101)
        reissued = self.request(vm, 7, upgrade)
        self.fill(vm, reissued)
        self.done(vm)

    def test_external_fifo(self):
        vm = self.start(['load'])
        identity = self.request(vm, 6)
        self.command(vm, 6, 100)
        self.put(vm, 'address', PAGE + 128)
        self.command(vm, 7, 101)
        self.command(vm, 8, 102, reject=True)
        self.command(vm, 10, 101, reject=True)
        self.command(vm, 10, 100)
        self.command(vm, 10, 101)
        self.fill(vm, identity)
        self.done(vm)

    def test_line_sizes_and_synonyms(self):
        for line64 in (False, True):
            for route in ('xkphys', 'tlb'):
                operations = ['store']
                if route == 'tlb':
                    operations.append('synonym')
                operations.append('store-subset')
                vm = self.start(operations, route=route, line64=line64)
                identity = self.request(vm, 7)
                self.assertEqual(self.get(vm, 'request-size'),
                                 64 if line64 else 128)
                self.fill(vm, identity)
                self.done(vm)
                self.assertEqual(self.get(vm, 'notifications'), 1)
                self.command(vm, 6, 100)
                self.assertEqual(self.get(vm, 'external-state'), 3)
                self.put(vm, 'offset', 0)
                self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE)
                self.put(vm, 'offset', 32)
                self.assertEqual(self.get(vm, 'data-word') >> 32, VALUE2)
                vm.shutdown()

    def test_agent_blocks_migration(self):
        vm = self.start(['store'])
        identity = self.request(vm, 7)
        uri = 'file:' + self.scratch_file('rejected.migration')
        result = vm.qmp('migrate', uri=uri)
        self.assertIn('error', result)
        self.assertIn('r10k-test-agent', result['error']['desc'])
        self.fill(vm, identity)
        self.done(vm)
        result = vm.qmp('migrate', uri=uri)
        self.assertIn('error', result)
        self.assertIn('r10k-test-agent', result['error']['desc'])


if __name__ == '__main__':
    QemuSystemTest.main()
