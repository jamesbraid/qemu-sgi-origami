#!/usr/bin/env python3
#
# SGI SN0 SMP scheduling tests
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
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

SLICE_B_STATE = 0x00032000
SLICE_B_PROCESSOR_NUMBER = 0x00
SLICE_B_IOC3_ID = 0x08
SLICE_B_UART_SCRATCH = 0x10
SLICE_B_ARB_HELD = 0x18
SLICE_B_START_ADDRESS = 0x20
SLICE_B_START_STATUS = 0x28
SLICE_B_VER_PREFIX = 0x30
SLICE_B_STATE_WORDS = 7

ELSC_HOLD_TICKS = 313
ELSC_PRESTART_TICKS = 625
PCF_LRB = 0x08


# QEMU's 100 ms default is the negative control.  The regular test pins
# 100 us so the peer can answer inside its 10 ms protocol deadline.
PEER_SCRATCH_RR_KICK_PERIOD_NS = int(os.getenv(
    'QEMU_TEST_SN0_RR_KICK_PERIOD_NS', '100000'))
PEER_SCRATCH_DEADLINE_TICKS = 12500  # 10 milliseconds at 1.25 MHz
PEER_SCRATCH_IDENTITY_BASE = 0x10
PEER_SCRATCH_ARMED = 0x21
PEER_SCRATCH_GO = 0x22
PEER_SCRATCH_DONE = 0x23
PEER_SCRATCH_TIMEOUT = 0x2f

HUB_BY_NODE = 0x20001800000
HUB_NODE_STRIDE = 0x100000000
HUB_PI_INT_MODIFY = 0x000090
HUB_PI_INT_PEND1 = 0x0000a0
HUB_PI_RT_COUNT = 0x030100
HUB_NI_STATUS = 0x600000
HUB_NI_SCRATCH_0 = 0x600100
HUB_NI_SCRATCH_1 = 0x600108
HUB_NI_VECTOR_PARMS = 0x600200
HUB_NI_VECTOR = 0x600208
HUB_NI_VECTOR_STATUS = 0x600300
HUB_NI_VECTOR_READ_DATA = 0x600310
HUB_NI_BASE = HUB_NI_STATUS
HUB_NI_SCRATCH_0_REL = HUB_NI_SCRATCH_0 - HUB_NI_BASE
HUB_NI_SCRATCH_1_REL = HUB_NI_SCRATCH_1 - HUB_NI_BASE
HUB_NI_VECTOR_PARMS_REL = HUB_NI_VECTOR_PARMS - HUB_NI_BASE
HUB_NI_VECTOR_REL = HUB_NI_VECTOR - HUB_NI_BASE
HUB_NI_VECTOR_READ_DATA_REL = HUB_NI_VECTOR_READ_DATA - HUB_NI_BASE
NI_VECTOR_VALID = 1 << 63
NI_VECTOR_RESPONSE_MASK = 0x7  # includes the modeled address-error response
PEER_SCRATCH_INTERRUPT = 114
PEER_SCRATCH_PEND1 = 1 << (PEER_SCRATCH_INTERRUPT - 64)


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
# Effectively unbounded, for the thread=multi twin. Under real MTTCG a
# busy-waiting vCPU that gets a full host core burns through any finite
# guest instruction budget in real time far faster than a host-starved
# peer can publish, so a bounded budget false-positives under load. The
# host-side poll deadline in wait_for_four_cpu_state is the real timeout
# for that test; this value only keeps the guest from declaring failure
# first.
#
# load_u32's lui sign-extends bit 31 on mips64, and this value's bit 31
# is set, so the register it loads into is not 0x00000000ffffffff but
# the 64-bit maximum, 0xffffffffffffffff. Intended: the guest side of
# this budget must never fire, and 2**64-1 iterations is further from
# firing than 2**32-1 would have been.
FOUR_CPU_WAIT_LIMIT_UNBOUNDED = 0xffffffff
FOUR_CPU_BASE_TIMEOUT = 0x1000
FOUR_CPU_HUB_TIMEOUT = 0x2000
PI_INT_MODIFY_SET = 0x100
PI_INT_MAILBOX = 12


# Dual-node stress state, node-one-explicit like FOUR_CPU_TEST_STATE, on its
# own page so the two tests' firmware images never share an address.
#
# Addressing fact this file's ROM builders rely on throughout, stated once
# here because this test is the first to depend on both directions of it:
# hw/mips/sn0.c gives every CPU a private address space in which an
# uncached reference with no NASID bits set (offset below 4 GiB) always
# resolves to the reading CPU's own node -- true for every CPU, on every
# node, including node zero. Node one's memory additionally has a
# NASID-explicit address (offset at or above 4 GiB) that reaches it from
# any CPU regardless of node, which is what FOUR_CPU_TEST_STATE already
# relies on. Node zero's memory has no such reader-independent address
# under a synthesized-firmware -bios boot (the one exception in the model,
# an alias substitution used only when booting a real kernel image, does
# not apply here) -- so a node-one CPU has no way to name node zero's RAM
# explicitly, only its own. This is a fact about this QEMU model as built,
# not a claim about what real Origin/SN0 hardware does with a NASID-zero
# address; that is an open question for the machine stream, not this file.
DUAL_NODE_TEST_STATE = 0x100040000
DUAL_NODE_PAGE = 0x0004
DUAL_COUNTER_A = 0x00      # node-0 pair only, reached by every CPU that
                           # touches it via an explicit node-1 address
DUAL_COUNTER_B = 0x08      # node-1 pair only, reached via their own local
                           # (unqualified) address -- the same physical
                           # word as DUAL_COUNTER_A's form, by construction
DUAL_COUNTER_C = 0x10      # all four CPUs: node-0's pair explicit, node-1's
                           # pair local, on the same word -- mixed
                           # local/remote contention
DUAL_PHASE1_DONE = 0x18    # 4 words
DUAL_PHASE2_FLAG = 0x38    # 4 words, one per CPU's own outbound channel
DUAL_PHASE2_PAYLOAD = 0x58  # 4 words
DUAL_PHASE2_FAULTS = 0x78  # 4 words: this CPU's own mismatch count, not a
                           # recorded value -- the state dump covers that
DUAL_PHASE2_DONE = 0x98    # 4 words
DUAL_PHASE3_DONE = 0xb8    # 4 words
DUAL_STATE_WORDS = 27

# Sized to stay comfortably inside the 30 s host poll ceiling in CI, not
# tuned as a soak test -- a real soak run scales these up externally.
# DUAL_NODE_COUNT_REMOTE and DUAL_NODE_COUNT_MIXED specifically also carry
# this test's only mutation coverage of the contended-counter phase: at
# 200/50 a lowered (non-atomic) compare-exchange lost updates on only
# roughly 3 of 15 mutated runs, because a lost update needs two CPUs
# inside the same read-modify-write window and that is rare at this
# volume. This method is the only place that phase is covered at all --
# no qtest or functional soak path exercises it -- so the budget is
# raised 10x here to make that window common enough to catch reliably in
# one run, not tuned as a soak test either.
DUAL_NODE_COUNT_REMOTE = 2000
DUAL_NODE_COUNT_MIXED = 500
DUAL_NODE_ORDER_ROUNDS = 20
DUAL_NODE_MAILBOX_ROUNDS = 20

# The icount method's reduced budget: enough rounds to still exercise every
# phase, small enough that single-threaded round-robin scheduling covers it
# well inside a few seconds.
DUAL_NODE_COUNT_REMOTE_ICOUNT = 20
DUAL_NODE_COUNT_MIXED_ICOUNT = 8
DUAL_NODE_ORDER_ROUNDS_ICOUNT = 4
DUAL_NODE_MAILBOX_ROUNDS_ICOUNT = 4


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


def emit_wait_for_words(program, offset, wait_limit):
    load_u32(program, R_T3, wait_limit)
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


def emit_wait_for_all_done(program, offset, wait_limit):
    """Spin until all FOUR_CPU_COUNT words at offset(s0) hold literal 1 --
    the DONE-marker convention store_cpu_word's dual-node-stress callers
    use for phase-boundary rendezvous (every CPU writes the same value 1
    into its own slot, so the host's own done == [1, 1, 1, 1] checks line
    up). Deliberately distinct from emit_wait_for_words above, whose
    per-CPU cpu+1 publish values suit build_four_cpu_rendezvous but not
    this uniform marker."""
    load_u32(program, R_T3, wait_limit)
    wait = len(program.instructions)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))  # t1 = 1, the expected value
    program.emit(mips_i(0x19, R_ZERO, R_T2, 0))  # t2 = 0, mismatch accumulator
    for cpu in range(FOUR_CPU_COUNT):
        program.emit(mips_i(0x37, R_S0, R_T0, offset + cpu * 8))
        program.emit(mips_r(R_T0, R_T1, R_T0, 0, 0x26))  # xor t0, t0, t1
        program.emit(mips_r(R_T2, R_T0, R_T2, 0, 0x25))  # or t2, t2, t0
    complete = program.branch(0x04, R_T2, R_ZERO)
    program.instructions[complete + 1] = mips_i(0x19, R_T3, R_T3, -1)
    branch = program.branch(0x05, R_T3, R_ZERO)
    program.patch(branch, wait)
    return complete


def emit_unreachable(program):
    """A bare infinite spin: somewhere harmless for a wait's own timeout
    branch to go when its wait_limit is FOUR_CPU_WAIT_LIMIT_UNBOUNDED and
    that branch is therefore provably dead code, since every caller in
    this file needs the function's shape either way."""
    spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin)


def emit_timeout(program, offset, marker):
    program.emit(mips_i(0x19, R_S1, R_T0, marker))
    store_cpu_word(program, offset, R_T0)
    spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin)


def emit_wait_for_mailbox(program, want_set, wait_limit):
    load_u32(program, R_T3, wait_limit)
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


def emit_repeat(program, count, body, counter=R_T3):
    """Run body(program) count times. count must fit daddiu's 16-bit
    signed immediate, true of every count this file passes. Pass a
    different counter register when body itself calls something that
    clobbers t3, such as emit_wait_for_mailbox's own timeout counter."""
    program.emit(mips_i(0x19, R_ZERO, counter, count))  # daddiu ctr,zero,count
    loop = len(program.instructions)
    body(program)
    program.emit(mips_i(0x19, counter, counter, -1))    # daddiu ctr, ctr, -1
    branch = program.branch(0x05, counter, R_ZERO)      # bne ctr, zero, loop
    program.patch(branch, loop)


def emit_wait_for_flag(program, base, offset, want_set):
    """Spin (guest-side unbounded -- the host poll deadline is the real
    timeout for the tests that use this) until the word at offset(base)
    is nonzero (want_set) or zero (not want_set)."""
    wait = len(program.instructions)
    program.emit(mips_i(0x37, base, R_T0, offset))  # ld t0, offset(base)
    branch = program.branch(0x04 if want_set else 0x05, R_T0, R_ZERO)
    program.patch(branch, wait)


def emit_dual_node_page_base(program, reg, explicit):
    """Compute DUAL_NODE_TEST_STATE's page into reg: node one's real
    memory if explicit, or the reader's own node's memory if not -- see
    the addressing-fact comment by DUAL_NODE_TEST_STATE. Both forms name
    the identical physical page for a node-one reader; only a node-zero
    reader needs explicit=True to reach it at all."""
    program.address_prefix(reg, 0x9000)
    if explicit:
        program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0001))    # lui at, 1
        program.emit(mips_r(R_ZERO, R_AT, R_AT, 16, 0x38))  # dsll at,at,16
        program.emit(mips_i(0x0f, R_ZERO, R_T0, DUAL_NODE_PAGE))
        program.emit(mips_r(R_AT, R_T0, R_AT, 0, 0x25))     # or at,at,t0
        program.emit(mips_r(reg, R_AT, reg, 0, 0x25))       # or reg,reg,at
    else:
        program.emit(mips_i(0x0f, R_ZERO, R_AT, DUAL_NODE_PAGE))
        program.emit(mips_r(reg, R_AT, reg, 0, 0x25))       # or reg,reg,at


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


def load_xkphys_address(program, register, prefix, offset):
    program.address_prefix(register, prefix)
    load_u32(program, R_AT, offset)
    program.emit(mips_r(register, R_AT, register, 0, 0x25))


def emit_local_hub_address(program, register, offset):
    load_xkphys_address(program, register, 0x9200, 0x01000000 + offset)


def emit_peer_scratch_read(program):
    # A zero route names the other Origin 200 chassis directly.
    program.emit(mips_i(0x3f, R_S0, R_ZERO, HUB_NI_VECTOR_REL))
    program.emit(mips_i(0x19, R_ZERO, R_T0,
                        HUB_NI_SCRATCH_1_REL))
    program.emit(mips_i(0x3f, R_S0, R_T0, HUB_NI_VECTOR_PARMS_REL))
    program.emit(mips_i(0x37, R_S0, R_T0,
                        HUB_NI_VECTOR_READ_DATA_REL))


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


def build_slice_b_console_boundary():
    program = MipsProgram()

    # Enter the 64-bit kernel mode selected by the real PROM.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Shared uncached state at 0x9000000000032000.
    load_xkphys_address(program, R_S0, 0x9000, SLICE_B_STATE)

    # Select slice B using this CPU's live Hub processor-number register.
    load_xkphys_address(program, R_S1, 0x9200, 0x01000020)
    program.emit(mips_i(0x37, R_S1, R_S3, 0))
    select_b = program.branch(0x05, R_S3, R_ZERO)
    park_a = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, park_a)
    program.patch(select_b, len(program.instructions))
    program.emit(mips_i(0x3f, R_S0, R_S3, SLICE_B_PROCESSOR_NUMBER))

    # Traverse slice B's local xTalk path to IOC3 configuration and UART A.
    load_xkphys_address(program, R_T0, 0x9200, 0x08022000)
    program.emit(mips_i(0x23, R_T0, R_T1, 0))
    program.emit(mips_i(0x3f, R_S0, R_T1, SLICE_B_IOC3_ID))

    load_xkphys_address(program, R_T0, 0x9200, 0x0862017f)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 0x5a))
    program.emit(mips_i(0x28, R_T0, R_T1, 0))
    program.emit(mips_i(0x24, R_T0, R_T1, 0))
    program.emit(mips_i(0x3f, R_S0, R_T1, SLICE_B_UART_SCRATCH))

    # Hub PCF8584 data/control and real-time counter registers.
    load_xkphys_address(program, R_S2, 0x9200, 0x01220000)
    program.emit(mips_i(0x19, R_S2, R_S1, 8))
    load_xkphys_address(program, R_T4, 0x9200, 0x01030100)
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0xc0))
    program.emit(mips_i(0x3f, R_S1, R_T0, 0))

    # Derive code 1 from PI_PROCESSOR_NUMBER, then require absence, signal,
    # and an uninterrupted hold beyond the controller's 250 us threshold.
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0xe0))
    program.emit(mips_r(R_T0, R_S3, R_S3, 0, 0x2d))
    wait_absent = len(program.instructions)
    program.emit(mips_i(0x37, R_S2, R_T0, 0))
    program.emit(mips_r(R_T0, R_S3, R_T1, 0, 0x26))
    program.emit(mips_i(0x0c, R_T1, R_T1, 0xfff7))
    branch = program.branch(0x04, R_T1, R_ZERO)
    program.patch(branch, wait_absent)
    program.emit(mips_i(0x37, R_S1, R_T0, 0))
    program.emit(mips_i(0x0c, R_T0, R_T0, 1))
    branch = program.branch(0x04, R_T0, R_ZERO)
    program.patch(branch, wait_absent)

    wait_signal = len(program.instructions)
    program.emit(mips_i(0x37, R_S2, R_T0, 0))
    program.emit(mips_r(R_T0, R_S3, R_T1, 0, 0x26))
    program.emit(mips_i(0x0c, R_T1, R_T1, 0xfff7))
    got_signal = program.branch(0x04, R_T1, R_ZERO)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, wait_signal)
    program.patch(got_signal, len(program.instructions))
    program.emit(mips_i(0x37, R_S1, R_T1, 0))
    program.emit(mips_i(0x0c, R_T1, R_T1, 1))
    branch = program.branch(0x04, R_T1, R_ZERO)
    program.patch(branch, wait_signal)
    program.emit(mips_r(R_ZERO, R_T0, R_S3, 0, 0x2d))
    program.emit(mips_i(0x37, R_T4, R_T2, 0))

    hold_signal = len(program.instructions)
    program.emit(mips_i(0x37, R_S2, R_T0, 0))
    branch = program.branch(0x05, R_T0, R_S3)
    program.patch(branch, wait_absent)
    program.emit(mips_i(0x37, R_S1, R_T0, 0))
    program.emit(mips_i(0x0c, R_T0, R_T0, 1))
    branch = program.branch(0x04, R_T0, R_ZERO)
    program.patch(branch, wait_absent)
    program.emit(mips_i(0x37, R_T4, R_T0, 0))
    program.emit(mips_r(R_T0, R_T2, R_T0, 0, 0x2f))
    program.emit(mips_i(0x0b, R_T0, R_T1, ELSC_HOLD_TICKS))
    branch = program.branch(0x05, R_T1, R_ZERO)
    program.patch(branch, hold_signal)
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    program.emit(mips_i(0x3f, R_S0, R_T0, SLICE_B_ARB_HELD))

    # Delay a staged first address across an arbitration update, then issue
    # one `ver` command and capture its response prefix.
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0x10))
    program.emit(mips_i(0x3f, R_S2, R_T0, 0))
    program.emit(mips_i(0x37, R_T4, R_T2, 0))
    wait_prestart = len(program.instructions)
    program.emit(mips_i(0x37, R_T4, R_T0, 0))
    program.emit(mips_r(R_T0, R_T2, R_T0, 0, 0x2f))
    program.emit(mips_i(0x0b, R_T0, R_T1, ELSC_PRESTART_TICKS))
    branch = program.branch(0x05, R_T1, R_ZERO)
    program.patch(branch, wait_prestart)
    program.emit(mips_i(0x37, R_S2, R_T0, 0))
    program.emit(mips_i(0x3f, R_S0, R_T0, SLICE_B_START_ADDRESS))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0xc5))
    program.emit(mips_i(0x3f, R_S1, R_T0, 0))
    program.emit(mips_i(0x37, R_S1, R_T0, 0))
    program.emit(mips_i(0x3f, R_S0, R_T0, SLICE_B_START_STATUS))
    for value in (3, ord('v'), ord('e'), ord('r'), 0xb0):
        program.emit(mips_i(0x19, R_ZERO, R_T0, value))
        program.emit(mips_i(0x3f, R_S2, R_T0, 0))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0x45))
    program.emit(mips_i(0x3f, R_S1, R_T0, 0))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0x11))
    program.emit(mips_i(0x3f, R_S2, R_T0, 0))
    program.emit(mips_i(0x37, R_S2, R_T0, 0))  # discard PCF data latch
    program.emit(mips_i(0x37, R_S2, R_T0, 0))  # response length
    program.emit(mips_i(0x37, R_S2, R_T1, 0))
    program.emit(mips_i(0x37, R_S2, R_T0, 0))
    program.emit(mips_r(R_ZERO, R_T1, R_T1, 8, 0x38))
    program.emit(mips_r(R_T1, R_T0, R_T1, 0, 0x25))
    program.emit(mips_i(0x3f, R_S0, R_T1, SLICE_B_VER_PREFIX))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 0xc2))
    program.emit(mips_i(0x3f, R_S1, R_T0, 0))

    spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, spin)
    return program


def build_peer_scratch_deadline():
    program = MipsProgram()

    # Enter the same 64-bit kernel mode selected by the real PROM.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # s0 is this CPU's NI register block, s1 its PI block, and s2 its RTC.
    emit_local_hub_address(program, R_S0, HUB_NI_BASE)
    emit_local_hub_address(program, R_S1, 0)
    emit_local_hub_address(program, R_S2, HUB_PI_RT_COUNT)

    # Only the A slice on each node participates; both B slices stay parked.
    program.emit(mips_i(0x37, R_S1, R_T0, 0x20))  # PI_PROCESSOR_NUMBER
    park_b = program.branch(0x05, R_T0, R_ZERO)

    # Publish a node-specific identity and announce readiness locally.
    program.emit(mips_i(0x37, R_S0, R_T0, 0))  # NI_STATUS
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 8, 0x3a))  # dsrl t0, t0, 8
    program.emit(mips_i(0x0c, R_T0, R_T0, 0x1ff))
    program.emit(mips_i(0x19, R_T0, R_T1, PEER_SCRATCH_IDENTITY_BASE))
    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_0_REL))
    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_ARMED))
    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_1_REL))
    program.emit(0x0000000f)  # sync
    node1 = program.branch(0x05, R_T0, R_ZERO)

    # Node zero waits until its peer is armed, then starts the timed handoff.
    wait_armed = len(program.instructions)
    emit_peer_scratch_read(program)
    branch = program.branch(0x05, R_T0, R_T1)
    program.patch(branch, wait_armed)

    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    program.emit(mips_i(0x19, R_T2, R_T2,
                        PEER_SCRATCH_DEADLINE_TICKS))
    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_GO))
    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_1_REL))
    program.emit(0x0000000f)  # sync
    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_DONE))

    wait_done = len(program.instructions)
    emit_peer_scratch_read(program)
    program.emit(mips_i(0x37, R_S2, R_T3, 0))
    program.emit(mips_r(R_T3, R_T2, R_T4, 0, 0x2b))  # sltu t4,t3,t2
    timed_out = program.branch(0x04, R_T4, R_ZERO)
    branch = program.branch(0x05, R_T0, R_T1)
    program.patch(branch, wait_done)

    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_1_REL))
    done_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, done_spin)

    program.patch(timed_out, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_TIMEOUT))
    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_1_REL))
    timeout_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, timeout_spin)

    # Node one answers GO, waits for the coordinator's terminal marker, then
    # leaves interrupt 114 as its final device-side action.
    program.patch(node1, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_GO))
    wait_go = len(program.instructions)
    emit_peer_scratch_read(program)
    branch = program.branch(0x05, R_T0, R_T1)
    program.patch(branch, wait_go)

    program.emit(mips_i(0x19, R_ZERO, R_T1, PEER_SCRATCH_DONE))
    program.emit(mips_i(0x3f, R_S0, R_T1, HUB_NI_SCRATCH_1_REL))
    program.emit(0x0000000f)  # sync
    wait_peer_done = len(program.instructions)
    emit_peer_scratch_read(program)
    branch = program.branch(0x05, R_T0, R_T1)
    program.patch(branch, wait_peer_done)

    program.emit(mips_i(0x19, R_ZERO, R_T0,
                        PI_INT_MODIFY_SET | PEER_SCRATCH_INTERRUPT))
    program.emit(mips_i(0x3f, R_S1, R_T0, HUB_PI_INT_MODIFY))
    node1_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, node1_spin)

    program.patch(park_b, len(program.instructions))
    b_spin = len(program.instructions)
    branch = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(branch, b_spin)
    return program


def build_four_cpu_rendezvous(wait_limit=FOUR_CPU_WAIT_LIMIT):
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
    complete = emit_wait_for_words(program, FOUR_CPU_BASE_PUBLISH, wait_limit)
    emit_timeout(program, FOUR_CPU_BASE_FAILURE, FOUR_CPU_BASE_TIMEOUT)
    program.patch(complete, len(program.instructions))
    program.emit(mips_i(0x19, R_S1, R_T0, 1))
    store_cpu_word(program, FOUR_CPU_BASE_DONE, R_T0)

    # SN0 phase starts with the same four-way RAM rendezvous.
    store_cpu_word(program, FOUR_CPU_HUB_PUBLISH, R_T0)
    complete = emit_wait_for_words(program, FOUR_CPU_HUB_PUBLISH, wait_limit)
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
    source_complete = emit_wait_for_mailbox(program, False, wait_limit)
    emit_timeout(program, FOUR_CPU_HUB_FAILURE, FOUR_CPU_HUB_TIMEOUT)
    program.patch(source_complete, len(program.instructions))
    source_done = program.branch(0x04, R_ZERO, R_ZERO)

    program.patch(receiver, len(program.instructions))
    receiver_complete = emit_wait_for_mailbox(program, True, wait_limit)
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


def build_dual_node_stress(counter_n=DUAL_NODE_COUNT_REMOTE,
                           counter_nc=DUAL_NODE_COUNT_MIXED,
                           order_rounds=DUAL_NODE_ORDER_ROUNDS,
                           mailbox_rounds=DUAL_NODE_MAILBOX_ROUNDS):
    program = MipsProgram()

    # Enter the same 64-bit kernel mode selected by the real PROM.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)  # mtc0 t0, Status
    program.emit(0)

    # Identity: the same NASID-and-slice decode build_four_cpu_rendezvous
    # uses. s1 ends up the global index 0..3 (node*2 + slice), s2 its byte
    # offset into any four-slot array, s3 this CPU's own node's Hub
    # PI_INT_MODIFY address.
    program.address_prefix(R_S3, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0020))
    program.emit(mips_r(R_S3, R_AT, R_S3, 0, 0x25))
    program.emit(mips_i(0x37, R_S3, R_S2, 0))  # ld s2, PI_PROCESSOR_NUMBER

    program.address_prefix(R_T0, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0160))
    program.emit(mips_r(R_T0, R_AT, R_T0, 0, 0x25))
    program.emit(mips_i(0x37, R_T0, R_T0, 0))
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 8, 0x3a))  # dsrl t0, t0, 8
    program.emit(mips_i(0x0c, R_T0, R_T0, 0x1ff))
    program.emit(mips_r(R_ZERO, R_T0, R_T0, 1, 0x38))  # dsll t0, t0, 1
    program.emit(mips_r(R_T0, R_S2, R_S1, 0, 0x25))    # or s1, t0, s2 (index)
    program.emit(mips_r(R_ZERO, R_S1, R_S2, 3, 0x38))  # dsll s2, s1, 3 (byte)

    program.address_prefix(R_S3, 0x9200)
    program.emit(mips_i(0x0f, R_ZERO, R_AT, 0x0100))
    program.emit(mips_i(0x0d, R_AT, R_AT, 0x0090))
    program.emit(mips_r(R_S3, R_AT, R_S3, 0, 0x25))    # s3 = own PI_INT_MODIFY

    # Phase 1: remote contended counters. Node zero's pair reaches
    # DUAL_COUNTER_A only through the explicit node-one address -- a
    # genuine crossbar crossing on every increment; node one's pair reaches
    # DUAL_COUNTER_B only through its own local address, which (see the
    # addressing-fact comment by DUAL_NODE_TEST_STATE) names the identical
    # physical word. Both pairs then add a short run on DUAL_COUNTER_C, so
    # one word sees both shapes of contention -- two genuinely remote CPUs
    # and two same-node CPUs -- at once. s0 ends this phase holding
    # whichever page-base form this CPU used; that form stays correct for
    # every later access through s0, by the same equivalence.
    program.emit(mips_i(0x0c, R_S1, R_T1, 2))    # andi t1, s1, 2 (node one?)
    node0 = program.branch(0x04, R_T1, R_ZERO)   # beq t1, zero -> node0

    emit_dual_node_page_base(program, R_S0, explicit=False)
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    emit_repeat(program, counter_n,
               lambda p: atomic_add(p, R_S0, DUAL_COUNTER_B, R_T0))
    emit_repeat(program, counter_nc,
               lambda p: atomic_add(p, R_S0, DUAL_COUNTER_C, R_T0))
    phase1_join = program.branch(0x04, R_ZERO, R_ZERO)

    program.patch(node0, len(program.instructions))
    emit_dual_node_page_base(program, R_S0, explicit=True)
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    emit_repeat(program, counter_n,
               lambda p: atomic_add(p, R_S0, DUAL_COUNTER_A, R_T0))
    emit_repeat(program, counter_nc,
               lambda p: atomic_add(p, R_S0, DUAL_COUNTER_C, R_T0))

    program.patch(phase1_join, len(program.instructions))
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    store_cpu_word(program, DUAL_PHASE1_DONE, R_T0)

    # All four CPUs rendezvous here before any of them starts phase 2.
    # Without this, DUAL_PHASE1_DONE and DUAL_PHASE2_DONE are written but
    # never consumed by anything -- a scheduler could legally run one node
    # through every phase to completion before the other node's CPUs ever
    # left phase 1, and the host would still see phase 3's final markers
    # settle to [1, 1, 1, 1] and score the run green, without phase 2's
    # cross-node traffic ever actually overlapping phase 1's contended
    # counters on the other node. wait_limit is unbounded for the same
    # reason phase 3's mailbox waits are (see below): a thread=multi vCPU
    # that wins a host core can burn through any finite guest budget before
    # a host-starved peer publishes, so the host's own poll deadline in
    # wait_for_dual_node_state is the only timeout that can fire correctly.
    complete = emit_wait_for_all_done(program, DUAL_PHASE1_DONE,
                                      FOUR_CPU_WAIT_LIMIT_UNBOUNDED)
    emit_unreachable(program)
    program.patch(complete, len(program.instructions))

    # Phase 2: cross-node writer/reader ordering. Every CPU owns one
    # outbound channel -- its own DUAL_PHASE2_FLAG/PAYLOAD slot -- that its
    # partner on the other node reads, and reads its partner's channel in
    # turn; both directions run concurrently, order_rounds each, gated by
    # the flag so a reader's round always observes its writer's matching
    # round (the flag ack blocks the writer's next round until the reader
    # has consumed the current one, so the two sides' round counts can
    # never drift apart). s0 still holds whichever page-base form phase 1
    # left it with, and both t2 (own channel) and t1 (partner's channel)
    # below are built from that same s0 -- so the asymmetry phase 1 already
    # has is total here too, not per-leg: DUAL_NODE_TEST_STATE's backing
    # page is node one's physical RAM (see the addressing-fact comment by
    # its definition), so node-one CPUs' write AND read both stay local,
    # and node-zero CPUs' write AND read are both remote. Only node-zero
    # CPUs issue remote accesses in this phase -- the same model constraint
    # DUAL_COUNTER_A/B already document, not new here. The phase still
    # exercises a genuinely remote write and a genuinely remote read, just
    # not from both nodes as requester; symmetric requester-node coverage
    # would need a second state page living on node zero, which the
    # model's own addressing fact rules out under a synthesized -bios boot.
    # t2 holds this CPU's own channel base (s0+s2) and t1 its partner's
    # (s0 + partner index * 8, partner index = this index XOR 2) for the
    # whole phase.
    program.emit(mips_r(R_S0, R_S2, R_T2, 0, 0x2d))    # daddu t2, s0, s2
    program.emit(mips_i(0x0e, R_S1, R_AT, 2))          # xori at, s1, 2
    program.emit(mips_r(R_ZERO, R_AT, R_AT, 3, 0x38))  # dsll at, at, 3
    program.emit(mips_r(R_S0, R_AT, R_T1, 0, 0x2d))    # daddu t1, s0, at

    def emit_order_round(p):
        # Wait for my own previous round to be acked (my flag cleared by
        # my partner's read) before overwriting the payload -- without
        # this a faster CPU can race ahead and overwrite its channel
        # several times before a slower partner ever reads it, which is
        # exactly the mismatch this phase would then wrongly report.
        # Round one's wait is immediate: the flag starts at zero.
        emit_wait_for_flag(p, R_T2, DUAL_PHASE2_FLAG, want_set=False)

        # Write my channel: publish the round tag, release, then flag it.
        # t3 is this round's tag -- emit_repeat's own countdown, read here
        # only, never written, so reusing it costs nothing extra.
        p.emit(mips_i(0x3f, R_T2, R_T3, DUAL_PHASE2_PAYLOAD))
        p.emit(0x0000000f)  # sync
        p.emit(mips_i(0x19, R_ZERO, R_T0, 1))
        p.emit(mips_i(0x3f, R_T2, R_T0, DUAL_PHASE2_FLAG))

        # Read my partner's channel: wait for their flag, acquire, then
        # compare their payload against the same round tag -- correct
        # because the flag handshake never lets either side's round count
        # get ahead of the other's.
        emit_wait_for_flag(p, R_T1, DUAL_PHASE2_FLAG, want_set=True)
        p.emit(0x0000000f)  # sync -- required: after flag, before payload
        p.emit(mips_i(0x37, R_T1, R_T0, DUAL_PHASE2_PAYLOAD))
        mismatch = p.branch(0x05, R_T0, R_T3)
        ok = p.branch(0x04, R_ZERO, R_ZERO)
        p.patch(mismatch, len(p.instructions))
        p.emit(mips_i(0x37, R_T2, R_T0, DUAL_PHASE2_FAULTS))
        p.emit(mips_i(0x19, R_T0, R_T0, 1))
        p.emit(mips_i(0x3f, R_T2, R_T0, DUAL_PHASE2_FAULTS))
        p.patch(ok, len(p.instructions))

        # Ack: clear the partner's flag so their next round can start.
        p.emit(mips_i(0x3f, R_T1, R_ZERO, DUAL_PHASE2_FLAG))

    emit_repeat(program, order_rounds, emit_order_round)
    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    store_cpu_word(program, DUAL_PHASE2_DONE, R_T0)

    # Second rendezvous: all four CPUs before phase 3 starts, same reason
    # and same unbounded wait_limit as the phase 1/2 barrier above -- phase
    # 3's mailbox traffic must provably overlap phase 2's cross-node
    # ordering rounds on the other node, not merely follow them once both
    # nodes happen to finish in whatever order the scheduler picked.
    complete = emit_wait_for_all_done(program, DUAL_PHASE2_DONE,
                                      FOUR_CPU_WAIT_LIMIT_UNBOUNDED)
    emit_unreachable(program)
    program.patch(complete, len(program.instructions))

    # Phase 3: the four-CPU test's local mailbox exchange, sustained for
    # mailbox_rounds instead of run once, concurrently on both node pairs
    # -- this is node-local by construction (see PI_INT_MODIFY at s3, the
    # reading CPU's own node), the same as it is in build_four_cpu_
    # rendezvous; the "cross-node" property under test here is that it
    # keeps working correctly while phases 1 and 2 hammer the crossbar
    # with real remote traffic on the other CPUs, not that any single
    # exchange itself crosses nodes. Both waits use emit_wait_for_mailbox
    # with an unbounded budget, same as everywhere else in this file's
    # thread=multi paths -- the host poll deadline is the real timeout --
    # so its own timeout branch is genuinely unreachable; give it
    # somewhere harmless to go rather than omit it, since the function's
    # shape always needs one (emit_unreachable, defined above).
    # counter=R_T4 here because emit_wait_for_mailbox uses t3 as its own
    # timeout counter, which would otherwise collide with emit_repeat's
    # default round counter.
    def emit_mailbox_round_a(p):
        p.emit(mips_i(0x19, R_ZERO, R_T0,
                      PI_INT_MODIFY_SET | PI_INT_MAILBOX))
        p.emit(mips_i(0x3f, R_S3, R_T0, 0))
        p.emit(0x0000000f)  # sync
        complete = emit_wait_for_mailbox(p, False,
                                         FOUR_CPU_WAIT_LIMIT_UNBOUNDED)
        emit_unreachable(p)
        p.patch(complete, len(p.instructions))

    def emit_mailbox_round_b(p):
        complete = emit_wait_for_mailbox(p, True,
                                         FOUR_CPU_WAIT_LIMIT_UNBOUNDED)
        emit_unreachable(p)
        p.patch(complete, len(p.instructions))
        p.emit(mips_i(0x19, R_ZERO, R_T0, PI_INT_MAILBOX))
        p.emit(mips_i(0x3f, R_S3, R_T0, 0))
        p.emit(0x0000000f)  # sync

    program.emit(mips_i(0x0c, R_S1, R_T1, 1))    # andi t1, s1, 1 (odd = B)
    role_b = program.branch(0x05, R_T1, R_ZERO)
    emit_repeat(program, mailbox_rounds, emit_mailbox_round_a, counter=R_T4)
    to_done = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(role_b, len(program.instructions))
    emit_repeat(program, mailbox_rounds, emit_mailbox_round_b, counter=R_T4)
    program.patch(to_done, len(program.instructions))

    program.emit(mips_i(0x19, R_ZERO, R_T0, 1))
    store_cpu_word(program, DUAL_PHASE3_DONE, R_T0)
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

    def wait_for_four_cpu_state(self, timeout=10):
        deadline = time.monotonic() + timeout
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

        # Stop before the diagnostic read, the same order read_test_state
        # uses, so the dump is one consistent snapshot rather than a value
        # from mid-poll that may already be stale by the time it prints.
        self.vm.cmd('human-monitor-command', command_line='stop')
        state = self.read_state(FOUR_CPU_TEST_STATE, FOUR_CPU_STATE_WORDS)
        self.fail(f'four-CPU state did not settle: {state}')

    def wait_for_dual_node_state(self, timeout=30):
        deadline = time.monotonic() + timeout
        state = []
        while time.monotonic() < deadline:
            state = self.read_state(DUAL_NODE_TEST_STATE, DUAL_STATE_WORDS)
            done = state[DUAL_PHASE3_DONE // 8:
                        DUAL_PHASE3_DONE // 8 + FOUR_CPU_COUNT]
            if done == [1, 1, 1, 1]:
                return state
            time.sleep(0.01)

        self.vm.cmd('human-monitor-command', command_line='stop')
        state = self.read_state(DUAL_NODE_TEST_STATE, DUAL_STATE_WORDS)
        self.fail(f'dual-node stress state did not settle: {state}')

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

    def test_slice_b_reaches_console_boundary(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('slice-b-console-boundary.bin')
        build_slice_b_console_boundary().write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-m', '64M', '-smp', '2', '-bios', firmware)
        self.vm.launch()

        time.sleep(0.75)
        self.vm.cmd('human-monitor-command', command_line='stop')
        state = self.read_state(SLICE_B_STATE, SLICE_B_STATE_WORDS)
        self.assertEqual(state[SLICE_B_PROCESSOR_NUMBER // 8], 1, state)
        self.assertEqual(state[SLICE_B_IOC3_ID // 8], 0x000310a9, state)
        self.assertEqual(state[SLICE_B_UART_SCRATCH // 8], 0x5a, state)
        self.assertEqual(state[SLICE_B_ARB_HELD // 8], 1, state)
        self.assertEqual(state[SLICE_B_START_ADDRESS // 8], 0x10, state)
        self.assertEqual(state[SLICE_B_START_STATUS // 8] & PCF_LRB, 0, state)
        self.assertEqual(state[SLICE_B_VER_PREFIX // 8], 0x6f6b, state)

    def test_peer_scratch_handoff_before_rtc_deadline(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('peer-scratch-deadline.bin')
        build_peer_scratch_deadline().write(firmware)

        # Preassigned NASIDs represent the node-numbering phase that precedes
        # this scratch exchange in the stock PROM.
        self.vm.set_machine(
            'origin200,synthesized-firmware=on,debug-leds=off')
        self.vm.add_args(
            '-accel',
            f'tcg,thread=single,rr-kick-period-ns='
            f'{PEER_SCRATCH_RR_KICK_PERIOD_NS}',
            '-m', '128M',
            '-smp', '4,sockets=2,cores=2,threads=1',
            '-object', 'memory-backend-ram,id=ram0,size=64M',
            '-object', 'memory-backend-ram,id=ram1,size=64M',
            '-numa', 'node,nodeid=0,memdev=ram0,cpus=0-1',
            '-numa', 'node,nodeid=1,memdev=ram1,cpus=2-3',
            '-bios', firmware)
        self.vm.launch()

        # A monitor request interrupts TCG. Let the race settle, stop exactly
        # once, and only then collect the independent per-node snapshots.
        time.sleep(0.75)
        self.vm.cmd('human-monitor-command', command_line='stop')
        hubs = []
        for node in range(2):
            hub = HUB_BY_NODE + node * HUB_NODE_STRIDE
            scratch = self.read_state(hub + HUB_NI_SCRATCH_0, 2)
            vector = self.read_state(hub + HUB_NI_VECTOR, 1)[0]
            status = self.read_state(hub + HUB_NI_VECTOR_STATUS, 1)[0]
            read_data = self.read_state(
                hub + HUB_NI_VECTOR_READ_DATA, 1)[0]
            pend1 = self.read_state(hub + HUB_PI_INT_PEND1, 1)[0]
            hubs.append((scratch, vector, status, read_data, pend1))

        for node, (scratch, vector, status, read_data, _) in enumerate(hubs):
            self.assertEqual(
                scratch,
                [PEER_SCRATCH_IDENTITY_BASE + node, PEER_SCRATCH_DONE], hubs)
            self.assertEqual(vector, 0, hubs)
            self.assertEqual(
                status & (NI_VECTOR_VALID | NI_VECTOR_RESPONSE_MASK),
                NI_VECTOR_VALID, hubs)
            self.assertEqual(read_data, PEER_SCRATCH_DONE, hubs)
        self.assertEqual(hubs[0][4], 0, hubs)
        self.assertEqual(hubs[1][4], PEER_SCRATCH_PEND1, hubs)

    def test_four_cpu_two_node_rendezvous_separates_ram_and_hub(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('four-cpu-rendezvous.bin')
        build_four_cpu_rendezvous().write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        # icount is the deterministic reference: a bounded instruction
        # budget per CPU under the single-threaded scheduler.
        # test_four_cpu_two_node_rendezvous_thread_multi is the twin that
        # covers thread=multi, which QEMU accepts for this target with a
        # warning until mips64 advertises MTTCG support.
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

    def test_four_cpu_two_node_rendezvous_thread_multi(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('four-cpu-rendezvous-thread-multi.bin')
        build_four_cpu_rendezvous(
            wait_limit=FOUR_CPU_WAIT_LIMIT_UNBOUNDED).write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        # No -icount: QEMU hard-errors combining it with thread=multi.
        self.vm.add_args(
            '-accel', 'tcg,thread=multi',
            '-m', '128M',
            '-smp', '4,sockets=2,cores=2,threads=1',
            '-object', 'memory-backend-ram,id=ram0,size=64M',
            '-object', 'memory-backend-ram,id=ram1,size=64M',
            '-numa', 'node,nodeid=0,memdev=ram0,cpus=0-1',
            '-numa', 'node,nodeid=1,memdev=ram1,cpus=2-3',
            '-bios', firmware)
        self.vm.launch()
        state = self.wait_for_four_cpu_state(timeout=30)
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

    def test_dual_node_stress_thread_multi(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('dual-node-stress-thread-multi.bin')
        build_dual_node_stress().write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        self.vm.add_args(
            '-accel', 'tcg,thread=multi',
            '-m', '128M',
            '-smp', '4,sockets=2,cores=2,threads=1',
            '-object', 'memory-backend-ram,id=ram0,size=64M',
            '-object', 'memory-backend-ram,id=ram1,size=64M',
            '-numa', 'node,nodeid=0,memdev=ram0,cpus=0-1',
            '-numa', 'node,nodeid=1,memdev=ram1,cpus=2-3',
            '-bios', firmware)
        self.vm.launch()
        state = self.wait_for_dual_node_state(timeout=30)

        counter_a = state[DUAL_COUNTER_A // 8]
        counter_b = state[DUAL_COUNTER_B // 8]
        counter_c = state[DUAL_COUNTER_C // 8]
        order_faults = state[DUAL_PHASE2_FAULTS // 8:
                             DUAL_PHASE2_FAULTS // 8 + FOUR_CPU_COUNT]

        self.assertEqual(counter_a, 2 * DUAL_NODE_COUNT_REMOTE,
                         f'remote-only counter A: {state}')
        self.assertEqual(counter_b, 2 * DUAL_NODE_COUNT_REMOTE,
                         f'local-only counter B: {state}')
        self.assertEqual(counter_c, 4 * DUAL_NODE_COUNT_MIXED,
                         f'mixed local/remote counter C: {state}')
        self.assertEqual(order_faults, [0, 0, 0, 0],
                         f'cross-node writer/reader ordering faults: {state}')

    def test_dual_node_stress_icount(self):
        self.set_machine('origin200')
        firmware = self.scratch_file('dual-node-stress-icount.bin')
        build_dual_node_stress(
            counter_n=DUAL_NODE_COUNT_REMOTE_ICOUNT,
            counter_nc=DUAL_NODE_COUNT_MIXED_ICOUNT,
            order_rounds=DUAL_NODE_ORDER_ROUNDS_ICOUNT,
            mailbox_rounds=DUAL_NODE_MAILBOX_ROUNDS_ICOUNT).write(firmware)

        self.vm.set_machine('origin200,debug-leds=off')
        # The deterministic test-of-the-test: the same ROM, a reduced
        # budget, and the single-threaded round-robin scheduler icount
        # requires, proving the ROM's correctness does not depend on
        # thread=multi's scheduling.
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
        state = self.wait_for_dual_node_state(timeout=30)

        counter_a = state[DUAL_COUNTER_A // 8]
        counter_b = state[DUAL_COUNTER_B // 8]
        counter_c = state[DUAL_COUNTER_C // 8]
        order_faults = state[DUAL_PHASE2_FAULTS // 8:
                             DUAL_PHASE2_FAULTS // 8 + FOUR_CPU_COUNT]

        self.assertEqual(counter_a, 2 * DUAL_NODE_COUNT_REMOTE_ICOUNT,
                         f'remote-only counter A: {state}')
        self.assertEqual(counter_b, 2 * DUAL_NODE_COUNT_REMOTE_ICOUNT,
                         f'local-only counter B: {state}')
        self.assertEqual(counter_c, 4 * DUAL_NODE_COUNT_MIXED_ICOUNT,
                         f'mixed local/remote counter C: {state}')
        self.assertEqual(order_faults, [0, 0, 0, 0],
                         f'cross-node writer/reader ordering faults: {state}')


if __name__ == '__main__':
    QemuSystemTest.main()
