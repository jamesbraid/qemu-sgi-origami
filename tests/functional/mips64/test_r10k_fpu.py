#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 James Braid

"""Exercise R10000-family floating-point Unimplemented exceptions.

The vectors come from the NEC VR10000 Series User's Manual, fourth edition,
printed pages 282-284 and 349. The ROM contains only instructions assembled
here; it needs no firmware or operating system.
"""

import re
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest


MAILBOX = 0x200000
DESTINATION = 0x40400000
FCSR_UNDERFLOW_ENABLED = 0x100
FCSR_CAUSE_UNIMPLEMENTED = 1 << 17
DONE_EXCEPTION = 1
DONE_NO_EXCEPTION = 2

R12000 = ('R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,'
          'r12000-reset-mode=0x198683,r12000-scache-bytes=1048576,'
          'r12000-scache-block-words=16')
R16000 = ('R16000,big-endian=on,r16000-experimental=on,'
          'r16000-reset-mode=0x3a005,r16000-scache-bytes=4194304')


def instruction(op, rs, rt, immediate):
    return op << 26 | rs << 21 | rt << 16 | immediate & 0xffff


def constant(register, value):
    words = [instruction(0x0f, 0, register, value >> 48)]
    for shift in (32, 16, 0):
        words += [register << 16 | register << 11 | 16 << 6 | 0x38,
                  instruction(0x0d, register, register, value >> shift)]
    return words


def mtc0(register, cp0):
    return [0x40800000 | register << 16 | cp0 << 11] + [0] * 4


def mfc0(register, cp0):
    return [0x40000000 | register << 16 | cp0 << 11] + [0] * 4


def mtc1(register, fp_register):
    return 0x44800000 | register << 16 | fp_register << 11


def mfc1(register, fp_register):
    return 0x44000000 | register << 16 | fp_register << 11


def cfc1(register):
    return 0x44400000 | register << 16 | 31 << 11


def ctc1(register):
    return 0x44c00000 | register << 16 | 31 << 11


def fp_s(operation, destination, numerator, divisor):
    return (0x11 << 26 | 0x10 << 21 | divisor << 16 |
            numerator << 11 | destination << 6 | operation)


def rom(operation, numerator, divisor):
    code = constant(8, 0x20410080) + mtc0(8, 12)  # CU1, BEV, DE, KX
    code += [instruction(0x0f, 0, 16, 0xa020)]  # KSEG1 mailbox
    code += constant(8, FCSR_UNDERFLOW_ENABLED) + [ctc1(8)]
    code += constant(8, numerator) + [mtc1(8, 0)]
    code += constant(8, divisor) + [mtc1(8, 2)]
    code += constant(8, DESTINATION) + [mtc1(8, 4)]
    fault_pc = 0xbfc00400 + len(code) * 4
    code += [fp_s(operation, 4, 0, 2), cfc1(8),
             instruction(0x2b, 16, 8, 8),
             mfc1(8, 4), instruction(0x2b, 16, 8, 12)]
    code += constant(8, DONE_NO_EXCEPTION)
    code += [instruction(0x2b, 16, 8, 16), instruction(0x04, 0, 0, -1), 0]

    handler = [instruction(0x0f, 0, 16, 0xa020)]  # KSEG1 mailbox
    handler += mfc0(8, 13) + [instruction(0x2b, 16, 8, 0)]
    handler += mfc0(8, 14) + [instruction(0x2b, 16, 8, 4)]
    handler += [cfc1(8), instruction(0x2b, 16, 8, 8),
                mfc1(8, 4), instruction(0x2b, 16, 8, 12)]
    handler += constant(8, DONE_EXCEPTION)
    handler += [instruction(0x2b, 16, 8, 16), instruction(0x04, 0, 0, -1), 0]
    assert len(handler) * 4 <= 0x80

    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, instruction(0x04, 0, 0, 0xff))
    struct.pack_into(f'>{len(handler)}I', image, 0x380, *handler)
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image, fault_pc


class R10KFPU(QemuSystemTest):
    def check_vector(self, cpu, operation, numerator, divisor):
        image = self.scratch_file(f'fpu-{cpu.split(",")[0]}.bin')
        data, fault_pc = rom(operation, numerator, divisor)
        Path(image).write_bytes(data)
        vm = self.get_vm(name=cpu.split(',')[0])
        vm.set_machine('malta')
        vm.add_args('-accel', 'tcg,thread=single', '-cpu', cpu,
                    '-m', '64M', '-bios', image, '-nodefaults')
        vm.launch()

        deadline = time.monotonic() + 10
        while True:
            output = vm.cmd('human-monitor-command',
                            command_line=f'xp /5wx {MAILBOX:#x}')
            words = [int(word, 16) for word in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            self.assertEqual(len(words), 5, output)
            if words[4]:
                break
            if time.monotonic() >= deadline:
                self.fail(f'FPU ROM did not complete: {output}')
            time.sleep(0.05)

        self.assertEqual(words[4], DONE_EXCEPTION, output)
        self.assertEqual((words[0] >> 2) & 0x1f, 15, output)
        self.assertEqual(words[0] >> 31, 0, output)
        self.assertEqual(words[1], fault_pc, output)
        self.assertEqual(words[2] & (0x3f << 12),
                         FCSR_CAUSE_UNIMPLEMENTED, output)
        self.assertEqual(words[2] & (0x1f << 2), 0, output)
        self.assertEqual(words[2] & (0x1f << 7),
                         FCSR_UNDERFLOW_ENABLED, output)
        self.assertEqual(words[3], DESTINATION, output)

    def test_r10000_exact_subnormal_result(self):
        self.check_vector('R10000', 3, 0x00800000, 0x40000000)

    def test_r12000_exact_subnormal_result(self):
        self.check_vector(R12000, 3, 0x00800000, 0x40000000)

    def test_r16000_exact_subnormal_result(self):
        self.check_vector(R16000, 3, 0x00800000, 0x40000000)

    def test_r10000_add_subnormal_input(self):
        self.check_vector('R10000', 0, 0x3f800000, 0x00000001)

    def test_r12000_add_subnormal_input(self):
        self.check_vector(R12000, 0, 0x3f800000, 0x00000001)

    def test_r10000_div_subnormal_input(self):
        self.check_vector('R10000', 3, 0, 0x00000001)

    def test_r12000_div_subnormal_input(self):
        self.check_vector(R12000, 3, 0, 0x00000001)


if __name__ == '__main__':
    QemuSystemTest.main()
