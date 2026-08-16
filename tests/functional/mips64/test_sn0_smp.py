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

RR_KICK_STATE = 0x00031000
RR_KICK_PEER_ARMED = 0x00
RR_KICK_GO = 0x08
RR_KICK_PEER_READY = 0x10
RR_KICK_DONE = 0x18
RR_KICK_TIMEOUT = 0x20
RR_KICK_DEADLINE_TICKS = 5


# Explicit node-one RAM is shared by all four CPUs.  An unqualified uncached
# address instead resolves to the reading CPU's local node.
FOUR_CPU_TEST_STATE = 0x100030000
FOUR_CPU_ENTRY_NODE0 = 0x00020000
FOUR_CPU_ENTRY_NODE1 = 0x100020000
FOUR_CPU_BASE_PUBLISH = 0x00
FOUR_CPU_BASE_DONE = 0x20
FOUR_CPU_BASE_FAILURE = 0x40
FOUR_CPU_HUB_PUBLISH = 0x60
FOUR_CPU_HUB_DONE = 0x80
FOUR_CPU_HUB_FAILURE = 0xa0
FOUR_CPU_MAILBOX_DONE = 0xc0
FOUR_CPU_STATE_WORDS = 28

FOUR_CPU_COUNT = 4
FOUR_CPU_WAIT_LIMIT = 0x00400000
FOUR_CPU_BASE_TIMEOUT = 0x1000
FOUR_CPU_HUB_TIMEOUT = 0x2000
PI_INT_MODIFY_SET = 0x100
PI_INT_MAILBOX = 12


def mips_i(op, rs, rt, immediate):
    return op << 26 | rs << 21 | rt << 16 | immediate & 0xffff


def mips_r(rs, rt, rd, shift, function):
    return rs << 21 | rt << 16 | rd << 11 | shift << 6 | function


def load_u32(program, register, value):
    assert 0 <= value <= 0xffffffff
    program.emit(mips_i(0x0f, R_ZERO, register, value >> 16))
    program.emit(mips_i(0x0d, register, register, value & 0xffff))


def store_cpu_word(program, offset, value):
    program.emit(mips_i(0x19, R_S0, R_T4, offset))
    program.emit(mips_r(R_T4, R_S2, R_T4, 0, 0x2d))  # daddu t4, t4, s2
    program.emit(mips_i(0x3f, R_T4, value, 0))
    program.emit(0x0000000f)  # sync


def emit_wait_for_words(program, offset):
    load_u32(program, R_T3, FOUR_CPU_WAIT_LIMIT)
    wait = len(program.instructions)
    program.emit(mips_i(0x19, R_ZERO, R_T2, 0))
    for cpu in range(FOUR_CPU_COUNT):
        program.emit(mips_i(0x37, R_S0, R_T0, offset + cpu * 8))
        program.emit(mips_i(0x19, R_ZERO, R_T1, cpu + 1))
        program.emit(mips_r(R_T0, R_T1, R_T0, 0, 0x26))  # xor t0, t0, t1
        program.emit(mips_r(R_T2, R_T0, R_T2, 0, 0x25))  # or t2, t2, t0
    complete = program.branch(0x04, R_T2, R_ZERO)
    program.instructions[complete + 1] = mips_i(0x19, R_T3, R_T3, -1)
    branch = program.branch(0x05, R_T3, R_ZERO)
    program.patch(branch, wait)
    return complete


def emit_timeout(program, offset, marker):
    program.emit(mips_i(0x19, R_S1, R_T0, marker))
    store_cpu_word(program, offset, R_T0)
    spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin)


def emit_wait_for_mailbox(program, want_set):
    load_u32(program, R_T3, FOUR_CPU_WAIT_LIMIT)
    wait = len(program.instructions)
    program.emit(mips_i(0x37, R_S3, R_T0, 8))  # PI_INT_PEND0
    program.emit(mips_i(0x0c, R_T0, R_T0, 1 << PI_INT_MAILBOX))
    complete = program.branch(0x05 if want_set else 0x04,
                              R_T0, R_ZERO)
    program.instructions[complete + 1] = mips_i(0x19, R_T3, R_T3, -1)
    branch = program.branch(0x05, R_T3, R_ZERO)
    program.patch(branch, wait)
    return complete


def atomic_add(program, base, offset, value):
    retry = len(program.instructions)
    program.emit(mips_i(0x34, base, R_T1, offset))  # lld t1, offset(base)
    program.emit(mips_r(R_T1, value, R_T2, 0, 0x2d))
    program.emit(mips_i(0x3c, base, R_T2, offset))  # scd t2, offset(base)
    complete = program.branch(0x05, R_T2, R_ZERO)
    retry_branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(retry_branch, retry)
    program.patch(complete, len(program.instructions))
    program.emit(0x0000000f)  # sync


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


def build_rr_kick_deadline():
    program = MipsProgram()

    # Enter 64-bit kernel mode.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Shared completion markers at 0x9000000000031000.
    program.address_prefix(R_S0, 0x9000)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0003))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x1000))
    program.emit(mips_r(R_S0, R_AT, R_S0, 0, 0x25))

    # Hub PI_PROCESSOR_NUMBER at 0x9200000001000020.
    program.address_prefix(R_S1, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0020))
    program.emit(mips_r(R_S1, R_AT, R_S1, 0, 0x25))

    # Hub PI_RT_COUNT at 0x9200000001030100.
    program.emit(mips_i(0x0f, R_ZERO, R_S2, 0x9200))
    program.emit(mips_r(R_ZERO, R_S2, R_S2, 16, 0x38))
    program.emit(mips_i(0x0d, R_S2, R_S2, 0x0103))
    program.emit(mips_r(R_ZERO, R_S2, R_S2, 16, 0x38))
    program.emit(mips_i(0x0d, R_S2, R_S2, 0x0100))

    program.emit(mips_i(0x37, R_S1, R_T0, 0))
    select_b = program.branch(0x05, R_T0, R_ZERO)

    # CPU A gives the armed peer four microseconds to publish. Only the
    # configured one microsecond RR kick can run CPU B within that interval.
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))
    wait_armed = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T0, RR_KICK_PEER_ARMED))
    branch = program.branch(0x04, R_T0, R_ZERO)
    program.patch(branch, wait_armed)
    program.emit(mips_i(0x3f, R_S0, R_T1, RR_KICK_GO))
    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    program.emit(mips_i(0x19, R_T2, R_T2, RR_KICK_DEADLINE_TICKS))
    wait_peer = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T3, RR_KICK_PEER_READY))
    peer_ready = program.branch(0x05, R_T3, R_ZERO)
    program.emit(mips_i(0x37, R_S2, R_T3, 0))
    program.emit(mips_r(R_T3, R_T2, R_T4, 0, 0x2b))
    branch = program.branch(0x05, R_T4, R_ZERO)
    program.patch(branch, wait_peer)
    program.emit(mips_i(0x3f, R_S0, R_T1, RR_KICK_TIMEOUT))
    timeout_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, timeout_spin)

    program.patch(peer_ready, len(program.instructions))
    program.emit(mips_i(0x3f, R_S0, R_T1, RR_KICK_DONE))
    done_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, done_spin)

    # CPU B arms first, then publishes only after CPU A starts the deadline.
    program.patch(select_b, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))
    program.emit(mips_i(0x3f, R_S0, R_T1, RR_KICK_PEER_ARMED))
    wait_go = len(program.instructions)
    program.emit(mips_i(0x37, R_S0, R_T0, RR_KICK_GO))
    branch = program.branch(0x04, R_T0, R_ZERO)
    program.patch(branch, wait_go)
    program.emit(mips_i(0x3f, R_S0, R_T1, RR_KICK_PEER_READY))
    peer_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, peer_spin)
    return program


def build_four_cpu_rendezvous():
    program = MipsProgram()

    # Enter the same 64-bit kernel mode selected by the real PROM.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Both local CPUs atomically count their entry before device identity is
    # read.  They also add their PI processor number, which must be 0 + 1.
    program.address_prefix(R_S1, 0x9000)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0002))
    program.emit(mips_r(R_S1, R_AT, R_S1, 0, 0x25))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    atomic_add(program, R_S1, 0, R_T0)

    program.address_prefix(R_S3, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0020))
    program.emit(mips_r(R_S3, R_AT, R_S3, 0, 0x25))
    program.emit(mips_i(0x37, R_S3, R_S2, 0))
    atomic_add(program, R_S1, 8, R_S2)

    # The local slice plus the Hub NASID identifies one of four shared slots.
    program.address_prefix(R_T0, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0160))
    program.emit(mips_r(R_T0, R_AT, R_T0, 0, 0x25))
    program.emit(mips_i(0x37, R_T0, R_T0, 0))
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 8, 0x3a))  # dsrl t0, t0, 8
    program.emit(mips_i(0x0c, R_T0, R_T0, 0x1ff))
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 1, 0x38))  # dsll t0, t0, 1
    program.emit(mips_r(R_T0, R_S2, R_S1, 0, 0x25))
    program.emit(mips_r(R_ZERO, R_S1, R_S2, 3, 0x38))  # dsll s2, s1, 3

    # Uncached node-one RAM at 0x9000000100030000.
    program.address_prefix(R_S0, 0x9000)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0001))
    program.emit(mips_r(R_ZERO, R_AT, R_AT, 16, 0x38))
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x0003))
    program.emit(mips_r(R_AT, R_T0, R_AT, 0, 0x25))
    program.emit(mips_r(R_S0, R_AT, R_S0, 0, 0x25))

    # Hub PI_INT_MODIFY at the reading CPU's local node.
    program.address_prefix(R_S3, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0090))
    program.emit(mips_r(R_S3, R_AT, R_S3, 0, 0x25))

    # Baseline: uncached shared RAM only.  Each CPU owns one unique word.
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_BASE_PUBLISH, R_T0)
    complete = emit_wait_for_words(program, FOUR_CPU_BASE_PUBLISH)
    emit_timeout(program, FOUR_CPU_BASE_FAILURE, FOUR_CPU_BASE_TIMEOUT)
    program.patch(complete, len(program.instructions))
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_BASE_DONE, R_T0)

    # SN0 phase starts with the same four-way RAM rendezvous.
    store_cpu_word(program, FOUR_CPU_HUB_PUBLISH, R_T0)
    complete = emit_wait_for_words(program, FOUR_CPU_HUB_PUBLISH)
    emit_timeout(program, FOUR_CPU_HUB_FAILURE, FOUR_CPU_HUB_TIMEOUT)
    program.patch(complete, len(program.instructions))

    # CPU A on each node sets a mailbox bit.  Its local CPU B observes and
    # clears it, then A observes the clear.  The bit is masked, so no handler
    # or interrupt-vector setup is needed for this exchange.
    program.emit(mips_i(0x0c, R_S1, R_T1, 1))
    receiver = program.branch(0x05, R_T1, R_ZERO)
    program.emit(mips_i(0x19, R_ZERO, R_T0,
                        PI_INT_MODIFY_SET | PI_INT_MAILBOX))
    program.emit(mips_i(0x3f, R_S3, R_T0, 0))
    program.emit(0x0000000f)  # sync
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_MAILBOX_DONE, R_T0)
    source_complete = emit_wait_for_mailbox(program, False)
    emit_timeout(program, FOUR_CPU_HUB_FAILURE, FOUR_CPU_HUB_TIMEOUT)
    program.patch(source_complete, len(program.instructions))
    source_done = program.branch(0x04, R_ZERO, R_ZERO)

    program.patch(receiver, len(program.instructions))
    receiver_complete = emit_wait_for_mailbox(program, True)
    emit_timeout(program, FOUR_CPU_HUB_FAILURE, FOUR_CPU_HUB_TIMEOUT)
    program.patch(receiver_complete, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T0, PI_INT_MAILBOX))
    program.emit(mips_i(0x3f, R_S3, R_T0, 0))
    program.emit(0x0000000f)  # sync
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_MAILBOX_DONE, R_T0)

    program.patch(source_done, len(program.instructions))
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_HUB_DONE, R_T0)
    spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin)
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

    def read_state(self, address, words):
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{words}gx {address:#x}')
        state = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(state), words, result)
        self.log.info('state at %#x: %s', address, state)
        return state

    def wait_for_four_cpu_state(self):
        deadline = time.monotonic() + 10
        state = []
        while time.monotonic() < deadline:
            state = self.read_state(FOUR_CPU_TEST_STATE,
                                    FOUR_CPU_STATE_WORDS)
            failures = state[FOUR_CPU_BASE_FAILURE // 8:
                             FOUR_CPU_BASE_FAILURE // 8 + FOUR_CPU_COUNT]
            failures += state[FOUR_CPU_HUB_FAILURE // 8:
                              FOUR_CPU_HUB_FAILURE // 8 + FOUR_CPU_COUNT]
            done = state[FOUR_CPU_HUB_DONE // 8:
                         FOUR_CPU_HUB_DONE // 8 + FOUR_CPU_COUNT]
            if any(failures) or done == [1, 2, 3, 4]:
                return state
            time.sleep(0.01)
        self.fail(f'four-CPU state did not settle: {state}')

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

    def test_rr_kick_period_allows_peer_before_deadline(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('rr-kick-deadline.bin')
        build_rr_kick_deadline().write(firmware)

        self.vm.set_machine(
            'origin200,synthesized-firmware=off,debug-leds=off')
        self.vm.add_args(
            '-accel', 'tcg,thread=single,rr-kick-period-ns=1000',
            '-icount', 'shift=2,align=off,sleep=on',
            '-smp', '2', '-bios', firmware)
        self.vm.launch()

        time.sleep(0.75)
        state = self.read_state(RR_KICK_STATE, 5)
        self.assertEqual(state[RR_KICK_PEER_ARMED // 8], 1)
        self.assertEqual(state[RR_KICK_GO // 8], 1)
        self.assertEqual(state[RR_KICK_PEER_READY // 8], 1)
        self.assertEqual(state[RR_KICK_DONE // 8], 1)
        self.assertEqual(state[RR_KICK_TIMEOUT // 8], 0)

    def test_four_cpu_two_node_rendezvous_separates_ram_and_hub(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('four-cpu-rendezvous.bin')
        build_four_cpu_rendezvous().write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        # MIPS64 does not support MTTCG. Icount gives the supported
        # single-threaded scheduler a bounded instruction budget per CPU.
        self.vm.add_args(
            '-accel', 'tcg,thread=single',
            '-icount', 'shift=2,align=off,sleep=on',
            '-m', '128M',
            '-smp', '4,sockets=2,cores=2,threads=1',
            '-object', 'memory-backend-ram,id=ram0,size=64M',
            '-object', 'memory-backend-ram,id=ram1,size=64M',
            '-numa', 'node,nodeid=0,memdev=ram0,cpus=0-1',
            '-numa', 'node,nodeid=1,memdev=ram1,cpus=2-3',
            '-bios', firmware)
        self.vm.launch()
        state = self.wait_for_four_cpu_state()
        entry_node0 = self.read_state(FOUR_CPU_ENTRY_NODE0, 2)
        entry_node1 = self.read_state(FOUR_CPU_ENTRY_NODE1, 2)

        baseline_publish = state[FOUR_CPU_BASE_PUBLISH // 8:
                                 FOUR_CPU_BASE_PUBLISH // 8 + FOUR_CPU_COUNT]
        baseline_done = state[FOUR_CPU_BASE_DONE // 8:
                              FOUR_CPU_BASE_DONE // 8 + FOUR_CPU_COUNT]
        baseline_failures = state[FOUR_CPU_BASE_FAILURE // 8:
                                  FOUR_CPU_BASE_FAILURE // 8 + FOUR_CPU_COUNT]
        hub_publish = state[FOUR_CPU_HUB_PUBLISH // 8:
                            FOUR_CPU_HUB_PUBLISH // 8 + FOUR_CPU_COUNT]
        hub_done = state[FOUR_CPU_HUB_DONE // 8:
                         FOUR_CPU_HUB_DONE // 8 + FOUR_CPU_COUNT]
        hub_failures = state[FOUR_CPU_HUB_FAILURE // 8:
                             FOUR_CPU_HUB_FAILURE // 8 + FOUR_CPU_COUNT]
        mailbox_done = state[FOUR_CPU_MAILBOX_DONE // 8:
                             FOUR_CPU_MAILBOX_DONE // 8 + FOUR_CPU_COUNT]

        self.assertEqual(entry_node0, [2, 1],
                         f'node-zero CPU entry and PI markers: {state}')
        self.assertEqual(entry_node1, [2, 1],
                         f'node-one CPU entry and PI markers: {state}')
        self.assertEqual(baseline_failures, [0, 0, 0, 0],
                         f'baseline RAM failure markers: {state}')
        self.assertEqual(baseline_publish, [1, 2, 3, 4],
                         f'baseline RAM publication markers: {state}')
        self.assertEqual(baseline_done, [1, 2, 3, 4],
                         f'baseline RAM completion markers: {state}')
        self.assertEqual(hub_failures, [0, 0, 0, 0],
                         f'SN0 mailbox failure markers: {state}')
        self.assertEqual(hub_publish, [1, 2, 3, 4],
                         f'SN0 RAM publication markers: {state}')
        self.assertEqual(mailbox_done, [1, 2, 3, 4],
                         f'SN0 mailbox completion markers: {state}')
        self.assertEqual(hub_done, [1, 2, 3, 4],
                         f'SN0 phase completion markers: {state}')


if __name__ == '__main__':
    QemuSystemTest.main()
