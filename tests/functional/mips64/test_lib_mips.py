#!/usr/bin/env python3
#
# Unit tests for lib_mips: the instruction encodings, the branch patcher,
# the xkphys address form, and the reset-window reader the bare-metal
# exercises in this directory read their answer out of
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_mips.py`.

PORT_RESET_IMAGE_HEX below is the exact firmware the port-reset exercise
built before this module existed, recorded from its own copy of the
assembler. BuildsTheRecordedProgram reassembles that program through
lib_mips and requires byte-for-byte agreement, so a change to any
encoding here shows up as a diff against a real program rather than as a
green run and a guest that no longer does what it did.
"""

import unittest

import lib_mips
from lib_mips import (R_AT, R_S0, R_S1, R_S2, R_T0, R_T1, R_T2, R_T3, R_T4,
                      R_T5, R_ZERO, MipsProgram, mips_i, mips_r,
                      reset_windows)


# The port-reset exercise's own constants, as its firmware encodes them.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_DONE = 0x18
CYCLES = 20
B_COUNT_TARGET = 8
HUB_LOCAL = 0x01000000
PI_PROCESSOR_NUMBER = 0x000020
NI_PORT_RESET = 0x600008
PORT_RESET_VALUE = 0x81
STATUS_KX_KERNEL = 0x24400080

# 53 big-endian words: the image build_port_reset() produced from the
# SN0PortReset section of test_sn_reset.py's own copy of this assembler,
# at the revision that copy was moved here.
PORT_RESET_IMAGE_HEX = (
    "3c0824403508008040886000000000003c1090000010803c3c010003342100000201"
    "80253c1192000011883c3c01010034210020022188253c1292000012903c3c010160"
    "342100080241902564090001de0c00002d8b00141160001800000000658d0001de28"
    "00001500000d00000000de0a0008154dfffe00000000de0a00102d4b00081560fffd"
    "00000000fe0d000064080081fe4800001000ffff00000000fe000010fe0d0008640a"
    "0000654a0001fe0a00101000fffd00000000de280000000840f80208402dfd090018"
    "1000ffff00000000"
)


def build_port_reset():
    """Slice A resets the machine while slice B spins, CYCLES times over;
    then each slice publishes its own done word. The port-reset
    exercise's own firmware, reassembled here through lib_mips."""
    program = MipsProgram()

    program.emit(mips_i(0x0f, R_ZERO, R_T0, STATUS_KX_KERNEL >> 16))
    program.emit(mips_i(0x0d, R_T0, R_T0, STATUS_KX_KERNEL & 0xffff))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)             # mtc0 Status
    program.emit(0)

    program.address(R_S0, 0x9000, STATE)
    program.address(R_S1, 0x9200, HUB_LOCAL + PI_PROCESSOR_NUMBER)
    program.address(R_S2, 0x9200, HUB_LOCAL + NI_PORT_RESET)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))                  # t1 = 1

    program.emit(mips_i(0x37, R_S0, R_T4, STATE_PHASE))          # ld: cycle
    program.emit(mips_i(0x0b, R_T4, R_T3, CYCLES))               # sltiu
    done = program.branch(0x04, R_T3, R_ZERO)                    # beq
    program.emit(mips_i(0x19, R_T4, R_T5, 1))                    # this cycle

    program.emit(mips_i(0x37, R_S1, R_T0, 0))                    # processor
    slice_b = program.branch(0x05, R_T0, R_ZERO)                 # bne

    wait_mark = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_MARK))
    program.patch(program.branch(0x05, R_T2, R_T5), wait_mark)

    wait_count = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_COUNT))
    program.emit(mips_i(0x0b, R_T2, R_T3, B_COUNT_TARGET))       # sltiu
    program.patch(program.branch(0x05, R_T3, R_ZERO), wait_count)

    program.emit(mips_i(0x3f, R_S0, R_T5, STATE_PHASE))          # sd
    program.emit(mips_i(0x19, R_ZERO, R_T0, PORT_RESET_VALUE))
    program.emit(mips_i(0x3f, R_S2, R_T0, 0))                    # the reset
    program.spin()

    program.patch(slice_b, program.here())
    program.emit(mips_i(0x3f, R_S0, R_ZERO, STATE_B_COUNT))
    program.emit(mips_i(0x3f, R_S0, R_T5, STATE_B_MARK))
    program.emit(mips_i(0x19, R_ZERO, R_T2, 0))
    loop_b = program.here()
    program.emit(mips_i(0x19, R_T2, R_T2, 1))                    # daddiu
    program.emit(mips_i(0x3f, R_S0, R_T2, STATE_B_COUNT))
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), loop_b)

    program.patch(done, program.here())
    program.emit(mips_i(0x37, R_S1, R_T0, 0))                    # processor
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 3, 0x38))            # dsll 3
    program.emit(mips_r(R_S0, R_T0, R_T0, 0, 0x2d))              # daddu
    program.emit(mips_i(0x3f, R_T0, R_T1, STATE_DONE))
    program.spin()
    return program


class BuildsTheRecordedProgram(unittest.TestCase):

    def test_port_reset_firmware_is_byte_for_byte_what_it_was(self):
        program = build_port_reset()
        image = bytes.fromhex(PORT_RESET_IMAGE_HEX)

        self.assertEqual(len(program.instructions) * 4, len(image))
        built = b''.join(word.to_bytes(4, 'big')
                         for word in program.instructions)
        self.assertEqual(built.hex(), image.hex())

    def test_write_lays_the_words_down_big_endian(self):
        import tempfile
        program = MipsProgram()
        program.emit(0x12345678)
        program.emit(0x9abcdef0)
        with tempfile.TemporaryDirectory() as root:
            path = root + '/firmware.bin'
            program.write(path)
            with open(path, 'rb') as written:
                self.assertEqual(written.read(),
                                 b'\x12\x34\x56\x78\x9a\xbc\xde\xf0')


class EncodingTests(unittest.TestCase):

    def test_i_form_packs_opcode_registers_and_immediate(self):
        self.assertEqual(mips_i(0x0d, R_T0, R_T0, 0x0080), 0x35080080)

    def test_i_form_truncates_a_negative_immediate_to_sixteen_bits(self):
        self.assertEqual(mips_i(0x04, R_ZERO, R_ZERO, -2) & 0xffff, 0xfffe)

    def test_r_form_packs_the_three_registers_shift_and_function(self):
        # dsll32 rd, rt, 0
        self.assertEqual(mips_r(R_ZERO, R_S0, R_S0, 0, 0x3c), 0x0010803c)


class BranchTests(unittest.TestCase):

    def test_a_branch_reserves_its_delay_slot(self):
        program = MipsProgram()
        branch = program.branch(0x04, R_ZERO, R_ZERO)

        self.assertEqual(branch, 0)
        self.assertEqual(program.here(), 2)
        self.assertEqual(program.instructions[1], 0)

    def test_patching_counts_from_after_the_delay_slot(self):
        program = MipsProgram()
        branch = program.branch(0x04, R_ZERO, R_ZERO)
        program.emit(0)
        program.patch(branch, program.here())

        # target 3, branch 0: displacement 3 - 0 - 1 == 2.
        self.assertEqual(program.instructions[branch] & 0xffff, 2)

    def test_a_spin_branches_to_itself(self):
        program = MipsProgram()
        program.emit(0)
        program.spin()

        # branch at 1, target 1: displacement -1, encoded 0xffff.
        self.assertEqual(program.instructions[1] & 0xffff, 0xffff)

    def test_an_out_of_range_displacement_is_refused(self):
        program = MipsProgram()
        branch = program.branch(0x04, R_ZERO, R_ZERO)
        with self.assertRaises(AssertionError):
            program.patch(branch, 1 << 16)


class AddressTests(unittest.TestCase):

    def test_the_prefix_is_shifted_into_the_top_half(self):
        program = MipsProgram()
        program.address_prefix(R_S0, 0x9000)

        self.assertEqual(program.instructions,
                         [mips_i(0x0f, R_ZERO, R_S0, 0x9000),
                          mips_r(R_ZERO, R_S0, R_S0, 0, 0x3c)])

    def test_a_full_address_ors_the_offset_in_through_at(self):
        program = MipsProgram()
        program.address(R_S0, 0x9000, 0x30000)

        self.assertEqual(program.instructions[2:], [
            mips_i(0x0f, R_ZERO, R_AT, 0x0003),
            mips_i(0x0d, R_AT, R_AT, 0x0000),
            mips_r(R_S0, R_AT, R_S0, 0, 0x25),
        ])


def _write(cpu, name='pcf8584'):
    return (f"memory_region_ops_write cpu {cpu} mr foo addr 0x1000 "
            f"value 0x47 size 1 name '{name}'")


def _committing_write(line):
    match = lib_mips.WRITE_RE.match(line)
    if not match or match.group(4) != 'pcf8584':
        return None
    return int(match.group(1))


class ResetWindowTests(unittest.TestCase):

    def test_pairs_each_reset_with_the_write_that_asked_for_it(self):
        log = '\n'.join([
            _write(0),
            'Trace 1: 0xdeadbeef',
            'cpu_reset 0',
            'cpu_reset 1',
        ])

        windows = reset_windows(log, _committing_write)

        self.assertEqual(len(windows), 1)
        requester, entered = windows[0]
        self.assertEqual(requester, 0)
        self.assertEqual(entered, ['Trace 1: 0xdeadbeef'])

    def test_a_block_entered_by_the_requester_is_not_a_peer_running(self):
        log = '\n'.join([_write(0), 'Trace 0: 0xdeadbeef', 'cpu_reset 0'])

        self.assertEqual(reset_windows(log, _committing_write), [(0, [])])

    def test_the_startup_reset_before_any_write_is_skipped(self):
        log = '\n'.join([
            'cpu_reset 0',
            'cpu_reset 1',
            _write(1),
            'cpu_reset 0',
        ])

        self.assertEqual(reset_windows(log, _committing_write), [(1, [])])

    def test_consecutive_reset_lines_are_one_reset(self):
        log = '\n'.join([
            _write(0), 'cpu_reset 0', 'cpu_reset 1',
            _write(0), 'cpu_reset 0', 'cpu_reset 1',
        ])

        self.assertEqual(len(reset_windows(log, _committing_write)), 2)

    def test_a_write_to_another_device_is_not_a_request(self):
        log = '\n'.join([_write(0, name='sgi-l1-uart'), 'cpu_reset 0'])

        self.assertEqual(reset_windows(log, _committing_write), [])


if __name__ == '__main__':
    unittest.main()
