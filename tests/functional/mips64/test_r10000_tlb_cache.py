#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise R10000 TLB remapping and diagnostic cache separation.

Public contract: MIPS R10000 User's Manual v2.0, October 10, 1996,
pp. 48, 53-54, 162-163, 239, 280, 283 and 287-288. No firmware is required.
"""

import re
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest


MAILBOX = 0x1000
# Malta maps its first 128 MiB at physical zero (hw/mips/malta.c).
PAGE = 0x01010000
VIRTUAL = 0x40000000
FIRST = 0x10203040
SECOND = 0x55667788
DONE = 0x43414348


def insn_i(op, rs, rt, imm):
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xffff)


def constant(reg, value):
    words = [insn_i(0x0d, 0, reg, value >> 48)]
    for shift in (32, 16, 0):
        words += [reg << 16 | reg << 11 | 16 << 6 | 0x38,
                  insn_i(0x0d, reg, reg, value >> shift)]
    return words


def mtc0(reg, cp0):
    return [0x40800000 | reg << 16 | cp0 << 11, 0, 0, 0, 0]


def mfc0(reg, cp0):
    return [0x40000000 | reg << 16 | cp0 << 11, 0, 0, 0, 0]


def mapping(paddr, cca):
    code = constant(8, VIRTUAL) + mtc0(8, 10)
    code += mtc0(0, 0) + mtc0(0, 5)
    for cp0, page in ((2, paddr), (3, paddr + 0x1000)):
        entry = (page >> 6) | (cca << 3) | 7
        if cca == 2:
            # Attribute 3 selects ordinary memory in this model.
            entry |= 3 << 62
        code += constant(8, entry)
        code += [0x40a00000 | 8 << 16 | cp0 << 11, 0, 0, 0, 0]
    return code + [0x42000002, 0, 0, 0, 0]


def diagnostic_rom(mode):
    signed_remap = mode == 'cached-remap-signed'
    first = 0x12345678 if signed_remap else FIRST
    second = 0x87654321 if signed_remap else SECOND
    # Keep code, stack-free working state and output outside the data cache.
    code = constant(8, 0x00410080) + mtc0(8, 12)  # BEV, DE, KX
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    code += mtc0(0, 2) + mtc0(0, 3) + mtc0(0, 5)
    code += constant(8, 63) + constant(9, 0x10000000)
    loop = len(code)
    code += mtc0(8, 0) + mtc0(9, 10)
    code += [0x42000002, 0, 0, 0, 0,
             insn_i(0x09, 9, 9, 0x2000),
             insn_i(0x09, 8, 8, -1)]
    code += [insn_i(0x01, 8, 1, loop - len(code) - 1), 0]  # bgez
    code += constant(9, 0x9600000000000000 | PAGE)
    for offset, value in ((0, first), (0x1000, second)):
        code += constant(8, value) + [insn_i(0x2b, 9, 8, offset)]
    code += [insn_i(0x23, 9, 8, 0), 0x0000000f]

    if mode in ('remap', 'cached-remap', 'cached-remap-signed'):
        cca = 2 if mode == 'remap' else 3
        code += mapping(PAGE, cca) + constant(10, VIRTUAL)
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0)]
        code += mapping(PAGE + 0x1000, cca)
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 4)]
        if signed_remap:
            code += [insn_i(0x3f, 12, 8, 8)]
    else:
        code += mapping(PAGE, 3)
        address = VIRTUAL if mode == 'mapped' else 0x9800000000000000 | PAGE
        code += constant(10, address)
        code += mtc0(0, 28) + mtc0(0, 29)
        # Invalidate both ways at the tested primary and secondary indices.
        for way in (0, 1):
            for operation in (0x09, 0x0b):
                code += [insn_i(0x2f, 10, operation, way), 0, 0, 0, 0]
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0)]
        code += constant(8, SECOND)
        code += [insn_i(0x2b, 10, 8, 0), 0x0000000f,
                 insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 4)]
        for way in (0, 1):
            code += [insn_i(0x2f, 10, 0x05, way), 0, 0, 0, 0]
            code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 8 + way * 12)]
            code += mfc0(8, 29) + [insn_i(0x2b, 12, 8, 12 + way * 12)]
            code += [insn_i(0x2f, 10, 0x19, way), 0, 0, 0, 0]
            code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 16 + way * 12)]
        # Default CPU references update RAM rather than diagnostic arrays.
        code += [insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 36)]
        # Rewriting only CCA must discard the translated cache access path.
        code += mapping(PAGE, 2) + constant(11, VIRTUAL)
        code += [insn_i(0x23, 11, 8, 0), insn_i(0x2b, 12, 8, 56)]
        code += mapping(PAGE, 3)
        code += [insn_i(0x23, 11, 8, 0), insn_i(0x2b, 12, 8, 60)]
        # Diagnostic CACHE operations leave ordinary memory current.
        code += [insn_i(0x2f, 10, 0x15, 0), 0, 0, 0, 0,
                 insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 40)]
        code += [insn_i(0x2f, 10, 0x07, 0), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 44)]
        code += [insn_i(0x2f, 10, 0x1b, 0), 0, 0, 0, 0]
        code += mfc0(8, 29) + [insn_i(0x2b, 12, 8, 48)]
        code += [insn_i(0x2f, 10, 0x17, 0), 0, 0, 0, 0,
                 insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 52)]
    code += constant(8, DONE)
    code += [0x0000000f, insn_i(0x2b, 12, 8, 32),
             insn_i(0x04, 0, 0, -1), 0]
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


class R10000TLBCache(QemuSystemTest):
    timeout = 60

    def run_rom(self, mode):
        image = self.scratch_file(f'{mode}.bin')
        Path(image).write_bytes(diagnostic_rom(mode))
        self.set_machine('malta')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-cpu', 'R10000',
                         '-m', '128M', '-bios', image, '-nodefaults')
        self.vm.launch()
        deadline = time.monotonic() + 10
        while True:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /33wx {MAILBOX:#x}')
            words = [int(word, 16) for word in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            self.assertEqual(len(words), 33, output)
            if words[8] == DONE:
                break
            if time.monotonic() >= deadline:
                registers = self.vm.cmd('human-monitor-command',
                                        command_line='info registers')
                self.fail(f'{mode} ROM did not complete: {output}\n{registers}')
            time.sleep(0.05)
        self.vm.cmd('stop')
        self.log.info('%s result: %s', mode, output)
        Path(self.log_file('mailbox.txt')).write_text(output, encoding='utf-8')
        expected = ([0x12345678, 0x87654321]
                    if mode == 'cached-remap-signed' else [FIRST, SECOND])
        self.assertEqual(words[:2], expected, output)
        return words, output

    def test_default_cpu_bypasses_data_cache(self):
        words, output = self.run_rom('mapped')
        self.assertEqual(words[2:8], [0] * 6, output)
        self.assertEqual(words[9:11], [SECOND, SECOND], output)
        self.assertEqual(words[14:16], [SECOND, SECOND], output)

    def test_uncached_tlb_remap(self):
        self.run_rom('remap')

    def test_cached_tlb_remap_without_data_cache(self):
        self.run_rom('cached-remap')

    def test_cached_tlb_remap_sign_extends(self):
        words, output = self.run_rom('cached-remap-signed')
        self.assertEqual(words[2:4], [0xffffffff, 0x87654321], output)


if __name__ == '__main__':
    QemuSystemTest.main()
