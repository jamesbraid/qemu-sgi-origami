#!/usr/bin/env python3
#
# SGI SN0 SMP scheduling tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import re
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest


R_ZERO = 0
R_AT = 1
R_T0 = 8
R_T1 = 9
R_T2 = 10
R_T3 = 11
R_T4 = 12
R_S0 = 16
R_S1 = 17
R_S2 = 18
R_S3 = 19

TEST_STATE = 0x00030000
STATE_STARTED = 0x00
STATE_ALIVE = 0x08
STATE_PULSE = 0x10
STATE_DONE_A = 0x18
STATE_DONE_B = 0x20
STATE_EARLY_EXIT = 0x28

RTC_TICKS_PER_MS = 1250

PROMDATA_CPU_A = 0x00
PROMDATA_CPU_B = 0x08
PROMDATA_READY = 0x10
PROMDATA_UNCACHED_B = 0x18
PROMDATA_CACHED_A = 0x20
PROMDATA_CACHED_B = 0x28
PROMDATA_DONE = 0x30

PROMDATA_VALUE_A = 0x11
PROMDATA_VALUE_B = 0x22
PROMDATA_SHARED = 0x33


def mips_i(op, rs, rt, immediate):
    return op << 26 | rs << 21 | rt << 16 | immediate & 0xffff


def mips_r(rs, rt, rd, shift, function):
    return rs << 21 | rt << 16 | rd << 11 | shift << 6 | function


class MipsProgram:
    def __init__(self):
        self.instructions = []

    def emit(self, instruction):
        self.instructions.append(instruction)

    def branch(self, op, rs, rt):
        branch = len(self.instructions)
        self.emit(mips_i(op, rs, rt, 0))
        self.emit(0)                 # delay slot
        return branch

    def patch(self, branch, target):
        displacement = target - branch - 1
        assert -(1 << 15) <= displacement < (1 << 15)
        self.instructions[branch] |= displacement & 0xffff

    def address_prefix(self, register, high):
        self.emit(mips_i(0x0f, R_ZERO, register, high))
        self.emit(mips_r(R_ZERO, register, register, 0, 0x3c))  # dsll32

    def write(self, path):
        image = b''.join(struct.pack('>I', instruction)
                         for instruction in self.instructions)
        Path(path).write_bytes(image)


def build_rtc_rendezvous():
    program = MipsProgram()

    # Enter the same 64-bit kernel mode the real PROM selects at entry.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Shared uncached RAM at 0x9000000000030000.
    program.address_prefix(R_S0, 0x9000)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0003))
    program.emit(mips_r(R_S0, R_AT, R_S0, 0, 0x25))

    # Hub PI_PROCESSOR_NUMBER at 0x9200000001000020.
    program.address_prefix(R_S1, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0020))
    program.emit(mips_r(R_S1, R_AT, R_S1, 0, 0x25))

    # Hub PI_RT_COUNT at 0x9200000001030100, as the PROM builds it.
    program.emit(mips_i(0x0f, R_ZERO, R_S2, 0x9200))
    program.emit(mips_r(R_ZERO, R_S2, R_S2, 16, 0x38))  # dsll 16
    program.emit(mips_i(0x0d, R_S2, R_S2, 0x0103))
    program.emit(mips_r(R_ZERO, R_S2, R_S2, 16, 0x38))
    program.emit(mips_i(0x0d, R_S2, R_S2, 0x0100))

    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))  # daddiu t1, zero, 1
    program.emit(mips_i(0x37, R_S1, R_T0, 0))    # ld processor number
    select_b = program.branch(0x05, R_T0, R_ZERO)

    # CPU A waits until B is immediately about to poll the counter.
    wait_started = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_STARTED))
    branch = program.branch(0x04, R_T2, R_ZERO)
    program.patch(branch, wait_started)
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_ALIVE))

    wait_pulse = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_PULSE))
    branch = program.branch(0x04, R_T2, R_ZERO)
    program.patch(branch, wait_pulse)
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_DONE_A))
    spin_a = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin_a)

    # CPU B publishes STARTED, then performs the PROM's 1 ms RTC poll.
    program.patch(select_b, len(program.instructions))
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_STARTED))
    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    program.emit(mips_i(0x19, R_T2, R_T3, RTC_TICKS_PER_MS))
    wait_rtc = len(program.instructions)
    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    program.emit(mips_r(R_T2, R_T3, R_T4, 0, 0x2b))  # sltu t4, t2, t3
    branch = program.branch(0x05, R_T4, R_ZERO)
    program.patch(branch, wait_rtc)

    program.emit(mips_i(0x37, R_S0, R_T2, STATE_ALIVE))
    alive = program.branch(0x05, R_T2, R_ZERO)

    # The lost pulse: B arrived before A and cannot recover.
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_PULSE))
    program.emit(mips_i(0x3f, R_S0, R_ZERO, STATE_PULSE))
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_EARLY_EXIT))
    early_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, early_spin)

    # A ran during the delay, so B leaves a pulse A can observe.
    program.patch(alive, len(program.instructions))
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_PULSE))
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_DONE_B))
    spin_b = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin_b)
    return program


def build_promdata_cache_bypass():
    program = MipsProgram()

    # Enter 64-bit kernel mode with coprocessor 1 available for the PROM BSR.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Set the PROM's dirty-exclusive boot-status bit in FPR 0.
    program.emit(mips_i(0x19, R_ZERO, R_T1, 2))
    program.emit(0x44a00000 | R_T1 << 16)  # dmtc1 t1, f0

    # Shared test state at 0x9000000000030000.
    program.address_prefix(R_S0, 0x9000)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0003))
    program.emit(mips_r(R_S0, R_AT, R_S0, 0, 0x25))

    # Hub PI_PROCESSOR_NUMBER at 0x9200000001000020.
    program.address_prefix(R_S1, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0020))
    program.emit(mips_r(R_S1, R_AT, R_S1, 0, 0x25))

    # Cached and uncached-memory aliases of physical PROM data at 1 MiB.
    program.address_prefix(R_S2, 0xa800)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0010))
    program.emit(mips_r(R_S2, R_AT, R_S2, 0, 0x25))
    program.address_prefix(R_S3, 0x9600)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0010))
    program.emit(mips_r(R_S3, R_AT, R_S3, 0, 0x25))

    program.emit(mips_i(0x37, R_S1, R_T0, 0))
    select_b = program.branch(0x05, R_T0, R_ZERO)

    # CPU A seeds its private cache, then publishes through uncached memory.
    program.emit(mips_i(0x19, R_ZERO, R_T1, PROMDATA_VALUE_A))
    program.emit(mips_i(0x3f, R_S2, R_T1, 0))
    program.emit(mips_i(0x3f, R_S0, R_T1, PROMDATA_CPU_A))
    wait_b = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T2, PROMDATA_CPU_B))
    branch = program.branch(0x04, R_T2, R_ZERO)
    program.patch(branch, wait_b)
    program.emit(mips_i(0x19, R_ZERO, R_T3, PROMDATA_SHARED))
    program.emit(mips_i(0x3f, R_S3, R_T3, 0))
    program.emit(mips_i(0x3f, R_S0, R_T3, PROMDATA_READY))
    program.emit(mips_i(0x37, R_S2, R_T4, 0))
    program.emit(mips_i(0x3f, R_S0, R_T4, PROMDATA_CACHED_A))
    spin_a = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin_a)

    # CPU B must observe shared memory without losing its own cached value.
    program.patch(select_b, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T1, PROMDATA_VALUE_B))
    program.emit(mips_i(0x3f, R_S2, R_T1, 0))
    program.emit(mips_i(0x3f, R_S0, R_T1, PROMDATA_CPU_B))
    wait_ready = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T2, PROMDATA_READY))
    branch = program.branch(0x04, R_T2, R_ZERO)
    program.patch(branch, wait_ready)
    program.emit(mips_i(0x37, R_S3, R_T3, 0))
    program.emit(mips_i(0x3f, R_S0, R_T3, PROMDATA_UNCACHED_B))
    program.emit(mips_i(0x37, R_S2, R_T4, 0))
    program.emit(mips_i(0x3f, R_S0, R_T4, PROMDATA_CACHED_B))
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))
    program.emit(mips_i(0x3f, R_S0, R_T1, PROMDATA_DONE))
    spin_b = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin_b)
    return program


class SN0SMPMachine(QemuSystemTest):

    def read_test_state(self, words):
        self.vm.cmd('human-monitor-command', command_line='stop')
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{words}gx {TEST_STATE:#x}')
        state = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(state), words, result)
        self.log.info('test state: %s', state)
        return state

    def test_rtc_poll_allows_peer_progress(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('rtc-rendezvous.bin')
        build_rtc_rendezvous().write(firmware)

        self.vm.set_machine(
            'origin200,synthesized-firmware=off,debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-icount', 'shift=2,align=off,sleep=on',
                         '-smp', '2', '-bios', firmware)
        self.vm.launch()

        # A monitor request interrupts TCG, so let the race settle first.
        time.sleep(0.75)
        state = self.read_test_state(6)

        self.assertEqual(state[STATE_EARLY_EXIT // 8], 0)
        self.assertEqual(state[STATE_DONE_A // 8], 1)
        self.assertEqual(state[STATE_DONE_B // 8], 1)

    def test_uncached_promdata_bypasses_private_cache(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('promdata-cache-bypass.bin')
        build_promdata_cache_bypass().write(firmware)

        self.vm.set_machine(
            'origin200,synthesized-firmware=off,debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-smp', '2', '-bios', firmware)
        self.vm.launch()

        time.sleep(0.75)
        state = self.read_test_state(7)

        self.assertEqual(state[PROMDATA_DONE // 8], 1)
        self.assertEqual(state[PROMDATA_UNCACHED_B // 8], PROMDATA_SHARED)
        self.assertEqual(state[PROMDATA_CACHED_A // 8], PROMDATA_VALUE_A)
        self.assertEqual(state[PROMDATA_CACHED_B // 8], PROMDATA_VALUE_B)


if __name__ == '__main__':
    QemuSystemTest.main()
