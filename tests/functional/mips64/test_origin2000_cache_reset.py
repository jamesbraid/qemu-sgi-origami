#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check the SN0 machines' secondary-cache geometry with test ROMs.

Expectations come from Origami's reviewed origin2000-cache-configuration.md
at 72848aa85bee4eda8e76c043d50025d62e91aa76. The reset test checks Config
fields. The tag test distinguishes 1 MiB and 4 MiB secondary-cache indexing.
"""

import json
import re
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest


MAILBOX = 0x30000
RECORD_SIZE = 16
DONE = 0x43414348
SECONDARY_MASK = 0x00070000
SECONDARY_EXPECTED = 0x00030000
PRIMARY_MASK = 0xfc000000
PRIMARY_EXPECTED = 0x6c000000
TAG_A = 0x1234c5aa
TAG_B = 0x56788944
TAG_DONE = 0x54414753


def insn_i(opcode, rs, rt, immediate):
    return opcode << 26 | rs << 21 | rt << 16 | (immediate & 0xffff)


def cache_reset_rom():
    """Read Config before any CP0 write, then publish it in local RAM."""
    code = [
        0x40088000,                  # mfc0 t0, Config
        0x40096000,                  # mfc0 t1, Status
        insn_i(0x0d, 9, 9, 0x80),    # ori t1, t1, Status.KX
        0x40896000,                  # mtc0 t1, Status
        0, 0, 0, 0,                 # CP0 write hazard
        insn_i(0x0d, 0, 10, 0x9200),
    ]
    for part in (0, 0x0100, 0x0020):
        code += [10 << 16 | 10 << 11 | 16 << 6 | 0x38,
                 insn_i(0x0d, 10, 10, part)]
    code += [
        insn_i(0x37, 10, 11, 0),     # ld t3, 0(t2): Hub PI_CPU_NUM
        insn_i(0x0f, 0, 12, 0xa003), # lui t4, uncached local mailbox
        11 << 16 | 13 << 11 | 4 << 6 | 0x38,  # dsll t5, t3, 4
        12 << 21 | 13 << 16 | 12 << 11 | 0x2d,  # daddu t4, t4, t5
        insn_i(0x2b, 12, 8, 0),      # sw t0, Config result
        insn_i(0x2b, 12, 11, 4),     # sw t3, live Hub slice
        insn_i(0x0f, 0, 14, DONE >> 16),
        insn_i(0x0d, 14, 14, DONE & 0xffff),
        0x0000000f,                 # sync: publish completion last
        insn_i(0x2b, 12, 14, 8),
        insn_i(0x04, 0, 0, -1), 0,  # park with a branch delay slot
    ]
    image = bytearray(0x1000)
    # Leave the flash configuration area unused by the test program.
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    for index, instruction in enumerate(code):
        struct.pack_into('>I', image, 0x400 + index * 4, instruction)
    return image


def cache_tag_rom():
    """Store and load way-zero tags at sets one MiB apart."""
    code = [
        insn_i(0x0f, 0, 9, 0xa000),   # lui t1, uncached physical zero
        insn_i(0x0f, 0, 10, 0xa010),  # lui t2, one MiB farther
        insn_i(0x0f, 0, 8, TAG_A >> 16),
        insn_i(0x0d, 8, 8, TAG_A),
        0x4088e000,                   # mtc0 t0, TagLo
        0x4080e800,                   # mtc0 zero, TagHi
        insn_i(0x2f, 9, 0x0b, 0),    # cache Index Store Tag (S), 0(t1)
        insn_i(0x0f, 0, 8, TAG_B >> 16),
        insn_i(0x0d, 8, 8, TAG_B),
        0x4088e000,                   # mtc0 t0, TagLo
        insn_i(0x2f, 10, 0x0b, 0),   # cache Index Store Tag (S), 0(t2)
        0x4080e000,                   # mtc0 zero, TagLo
        insn_i(0x2f, 9, 0x07, 0),    # cache Index Load Tag (S), 0(t1)
        0x4008e000,                   # mfc0 t0, TagLo
        insn_i(0x0f, 0, 12, 0xa003), # lui t4, uncached local mailbox
        insn_i(0x2b, 12, 8, 0),      # sw t0, first tag
        0x4080e000,                   # mtc0 zero, TagLo
        insn_i(0x2f, 10, 0x07, 0),   # cache Index Load Tag (S), 0(t2)
        0x4008e000,                   # mfc0 t0, TagLo
        insn_i(0x2b, 12, 8, 4),      # sw t0, second tag
        0x40088000,                   # mfc0 t0, Config
        insn_i(0x2b, 12, 8, 12),     # sw t0, Config
        insn_i(0x0f, 0, 8, TAG_DONE >> 16),
        insn_i(0x0d, 8, 8, TAG_DONE),
        0x0000000f,                   # sync: publish completion last
        insn_i(0x2b, 12, 8, 8),
        insn_i(0x04, 0, 0, -1), 0,
    ]
    image = bytearray(0x1000)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    for index, instruction in enumerate(code):
        struct.pack_into('>I', image, 0x400 + index * 4, instruction)
    return image


class Origin2000CacheReset(QemuSystemTest):
    timeout = 90

    def read_records(self, cpus):
        records = []
        for cpu in cpus:
            index = cpu['cpu-index']
            address = MAILBOX + cpu['props']['core-id'] * RECORD_SIZE
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /4wx {address:#x}',
                                 cpu_index=index)
            words = [int(value, 16) for value in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            self.assertEqual(len(words), 4, output)
            records.append(words)
        return records

    def check_phase(self, phase, cpus):
        # Generic data loaders clear RAM on every reset without resetting a
        # CPU themselves or setting its PC. A stale completion must fail here.
        self.assertEqual(self.read_records(cpus), [[0] * 4 for _ in cpus],
                         f'{phase}: output was not cleared before execution')
        self.vm.cmd('cont')
        deadline = time.monotonic() + 30
        while True:
            words = self.read_records(cpus)
            if all(record[2] == DONE for record in words):
                break
            if time.monotonic() >= deadline:
                self.vm.cmd('stop')
                self.fail(f'{phase}: fresh ROM output timed out: {words}')
            time.sleep(0.05)
        self.vm.cmd('stop')
        results = []
        for cpu, (config, live_slice, _, _) in zip(cpus, words):
            result = {
                'phase': phase,
                'cpu_global_id': cpu['cpu-index'],
                'physical_node_qmp': cpu['props']['node-id'],
                'slice_qmp': cpu['props']['core-id'],
                'slice_rom': live_slice,
                'config': f'{config:#010x}',
                'secondary': f'{config & SECONDARY_MASK:#010x}',
                'primary': f'{config & PRIMARY_MASK:#010x}',
                'other_bits':
                    f'{config & ~(SECONDARY_MASK | PRIMARY_MASK):#010x}',
            }
            results.append(result)
            self.log.info('cache reset observation: %s', json.dumps(result))
        Path(self.log_file(f'{phase}.json')).write_text(
            json.dumps(results, indent=2) + '\n', encoding='utf-8')
        for result in results:
            self.assertEqual(result['slice_rom'], result['slice_qmp'], result)
            self.assertEqual(int(result['secondary'], 16), SECONDARY_EXPECTED,
                             f'secondary-cache reset field: {result}')
            self.assertEqual(int(result['primary'], 16), PRIMARY_EXPECTED,
                             f'primary-cache reset fields: {result}')

    def test_selected_cache_reset(self):
        for phase in ('cold', 'reset'):
            Path(self.log_file(f'{phase}.json')).unlink(missing_ok=True)
        rom = self.scratch_file('cache-reset.bin')
        Path(rom).write_bytes(cache_reset_rom())
        self.set_machine('origin2000')
        self.vm.add_args('-machine', 'synthesized-firmware=off',
                         '-accel', 'tcg,thread=single', '-cpu', 'R10000',
                         '-m', '256M', '-smp',
                         '8,sockets=4,cores=2,threads=1',
                         '-bios', rom, '-S', '-nodefaults')
        for cpu in range(8):
            for offset in (0, 8):
                address = MAILBOX + (cpu % 2) * RECORD_SIZE + offset
                self.vm.add_args('-device',
                                 f'loader,addr={address:#x},data=0,'
                                 f'data-len=8,cpu-num={cpu}')
        self.vm.launch()
        cpus = sorted(self.vm.cmd('query-cpus-fast'),
                      key=lambda cpu: cpu['cpu-index'])
        self.assertEqual(len(cpus), 8)
        for index, cpu in enumerate(cpus):
            self.assertEqual(cpu['cpu-index'], index)
            self.assertEqual(cpu['props']['node-id'], index // 2)
            self.assertEqual(cpu['props']['socket-id'], index // 2)
            self.assertEqual(cpu['props']['core-id'], index % 2)
        self.check_phase('cold', cpus)
        self.vm.cmd('system_reset')
        self.vm.event_wait('RESET')
        self.assertFalse(self.vm.cmd('query-status')['running'])
        self.check_phase('reset', cpus)

    def check_tag_geometry(self, machine, secondary, expected):
        rom = self.scratch_file(f'{machine}-cache-tags.bin')
        Path(rom).write_bytes(cache_tag_rom())
        cpus = 8 if machine == 'origin2000' else 2
        self.set_machine(machine)
        self.vm.add_args('-machine', 'synthesized-firmware=off',
                         '-accel', 'tcg,thread=single', '-cpu', 'R10000',
                         '-m', '256M', '-smp', str(cpus), '-bios', rom,
                         '-nodefaults')
        self.vm.launch()
        deadline = time.monotonic() + 30
        words = []
        while time.monotonic() < deadline:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /4wx {MAILBOX:#x}')
            words = [int(value, 16) for value in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            if len(words) == 4 and words[2] == TAG_DONE:
                break
            time.sleep(0.05)
        self.vm.cmd('stop')
        self.log.info('%s cache tag observation: %s', machine,
                      [f'{word:#010x}' for word in words])
        self.assertEqual(len(words), 4, words)
        self.assertEqual(words[2], TAG_DONE, words)
        self.assertEqual(words[3] & SECONDARY_MASK, secondary, words)
        self.assertEqual(words[:2], expected, words)

    def test_origin200_one_mib_tags_alias(self):
        self.check_tag_geometry('origin200', 0x00010000, [TAG_B, TAG_B])

    def test_origin2000_four_mib_tags_do_not_alias(self):
        self.check_tag_geometry('origin2000', SECONDARY_EXPECTED,
                                [TAG_A, TAG_B])


if __name__ == '__main__':
    QemuSystemTest.main()
