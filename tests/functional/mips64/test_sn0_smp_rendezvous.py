#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Dual-module Origin 200 rendezvous between processors on two nodes.

Vector PIO reaches an unnumbered peer Hub; node-number-qualified PIO
reaches one after numbering. The processors take node and slice from the
board's master-module strap and the Hub-local processor number, and the
master module's first slice numbers the peer node's Hub, as the PROM does
before these exchanges. Vector port-error reads and I/O-protect writes
need no number; the host checks some results through node-qualified
physical addresses, which need it.

Every processor runs the same image and takes its role from its own
identity (0 to 3, node times two plus slice); the others park. All the
state the host reads is in Hub registers or node memory.
"""

import re
import time

from lib_mips import (R_S0, R_S1, R_S2, R_S3, R_T0, R_T1, R_T2, R_T3, R_T4,
                      R_ZERO, PromImage, mips_i, mips_r)
from lib_prom import MACHINES
from lib_sn0_hub import (emit_assign_peer_nasid_if_master,
                         emit_read_is_slave, load_u32,
                         emit_wait_for_own_nasid_if_slave)
from qemu_test import QemuSystemTest

IMAGE_SIZE = 0x1000
HUB_LOCAL_WINDOW = 0x9200000001000000
HUB_BY_NODE = 0x20001800000
HUB_NODE_STRIDE = 0x100000000

PI_CPU_NUM = 0x20
HUB_NI_BASE = 0x600000
HUB_NI_SCRATCH_0 = 0x600100
HUB_NI_SCRATCH_1 = 0x600108
HUB_NI_VECTOR_PARMS = 0x600200
HUB_NI_VECTOR = 0x600208
HUB_NI_VECTOR_DATA = 0x600210
HUB_NI_VECTOR_STATUS = 0x600300
HUB_NI_VECTOR_READ_DATA = 0x600310
HUB_NI_PORT_ERROR = 0x608008
HUB_NI_IO_PROTECT = 0x600400

HUB_PI_INT_MODIFY = 0x90
HUB_PI_INT_PEND0 = 0x98
HUB_PI_INT_PEND1 = 0xa0
HUB_PI_INT_MASK0_A = 0xa8
HUB_PI_INT_MASK1_A = 0xb0
HUB_PI_INT_MASK0_B = 0xb8
HUB_PI_INT_MASK1_B = 0xc0
HUB_PI_RT_COUNT = 0x030100

NI_VECTOR_VALID = 1 << 63
NI_VECTOR_RESPONSE_MASK = 0x7
NI_VECTOR_TYPE_WRITE = 1
PI_INT_MODIFY_SET = 0x100

PORT_ERROR_READ_DONE = 0x51
IO_PROTECT_WRITE_DONE = 0x52


def rel(offset):
    return offset - HUB_NI_BASE


def emit_sync(program):
    program.emit(0x0000000f)


def emit_kx(program):
    """64-bit kernel mode, as the PROM selects it."""
    load_u32(program, R_T0, 0x24400080)
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)            # mtc0
    for _ in range(4):
        program.emit(0)


def emit_prologue(program):
    """64-bit kernel mode, this CPU's own Hub-local NI and PI blocks in
    s0 and s1."""
    emit_kx(program)
    program.li64(R_S0, HUB_LOCAL_WINDOW | HUB_NI_BASE)
    program.li64(R_S1, HUB_LOCAL_WINDOW)


def emit_park_slice_b_then_number_peer(program, pi_reg=R_S1):
    """The second slice of each node parks; the first of the master
    module numbers the peer's Hub. Returns the branch to patch to the
    park. Doing it in this order keeps the second slice's own vector
    traffic out of the first's."""
    program.emit(mips_i(0x37, pi_reg, R_T1, PI_CPU_NUM))
    program.emit(mips_i(0x0c, R_T1, R_T1, 1))
    park_b = program.branch(0x05, R_T1, R_ZERO)
    emit_assign_peer_nasid_if_master(program, 1)
    return park_b


def emit_identity(program, reg, pi_reg=R_S1):
    """reg = the global index 0 to 3: the master-module strap's node,
    or-ed with this CPU's own Hub-local slice."""
    emit_read_is_slave(program, R_T1)
    program.emit(mips_r(0, R_T1, R_T1, 1, 0x38))                # dsll
    program.emit(mips_i(0x37, pi_reg, reg, PI_CPU_NUM))
    program.emit(mips_i(0x0c, reg, reg, 1))
    program.emit(mips_r(R_T1, reg, reg, 0, 0x25))               # or


def emit_vector_read(program, hub_reg, target_reg, target_address):
    """Route zero reaches the direct CrayLink peer before node numbering.
    OR target_address, the peer register's offset, into the parameters
    word unsigned: the port-error offset sets bit 15, which an ADDIU load
    would sign-extend."""
    program.emit(mips_i(0x3f, hub_reg, R_ZERO, rel(HUB_NI_VECTOR)))
    program.emit(mips_i(0x0d, R_ZERO, target_reg, target_address))
    program.emit(mips_i(0x3f, hub_reg, target_reg,
                        rel(HUB_NI_VECTOR_PARMS)))
    program.emit(mips_i(0x37, hub_reg, target_reg,
                        rel(HUB_NI_VECTOR_READ_DATA)))


def ori(rt, rs, imm):
    return mips_i(0x0d, rs, rt, imm)


def addiu(rt, rs, imm):
    return mips_i(0x09, rs, rt, imm)


def emit_wait_equal(program, vector_target, expect_reg):
    """Vector-read the peer's scratch word until it equals expect_reg."""
    wait = program.here()
    emit_vector_read(program, R_S0, R_T0, vector_target)
    program.patch(program.branch(0x05, R_T0, expect_reg), wait)


def build_peer_port_error_read():
    """Node zero's A slice (identity 0) vector-reads the peer Hub's LLP
    port error register, the read the stock PROM's link diagnostic makes;
    every other CPU parks. The register reads zero and does not fault."""
    program = PromImage()
    emit_prologue(program)
    park_b = emit_park_slice_b_then_number_peer(program)
    emit_identity(program, R_S2)
    not_actor = program.branch(0x05, R_S2, R_ZERO)

    emit_vector_read(program, R_S0, R_T0, rel(HUB_NI_PORT_ERROR))
    program.emit(addiu(R_T1, R_ZERO, PORT_ERROR_READ_DONE))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))

    park = program.here()
    program.patch(not_actor, park)
    program.patch(park_b, park)
    program.spin()
    return program


def build_peer_io_protect_write():
    """The link diagnostic's first vector operation, before its reads: a
    WRITE granting itself cross-link PIO access to the peer's PIO
    protection register. The completion must latch clean, the write
    response type rather than an address error."""
    program = PromImage()
    emit_prologue(program)
    park_b = emit_park_slice_b_then_number_peer(program)
    emit_identity(program, R_S2)
    not_actor = program.branch(0x05, R_S2, R_ZERO)

    program.emit(mips_i(0x3f, R_S0, R_ZERO, rel(HUB_NI_VECTOR)))
    program.emit(mips_i(0x3f, R_S0, R_ZERO, rel(HUB_NI_VECTOR_DATA)))
    program.emit(addiu(R_T0, R_ZERO,
                       rel(HUB_NI_IO_PROTECT) | NI_VECTOR_TYPE_WRITE))
    program.emit(mips_i(0x3f, R_S0, R_T0, rel(HUB_NI_VECTOR_PARMS)))

    program.emit(addiu(R_T0, R_ZERO, IO_PROTECT_WRITE_DONE))
    program.emit(mips_i(0x3f, R_S0, R_T0, rel(HUB_NI_SCRATCH_1)))

    park = program.here()
    program.patch(not_actor, park)
    program.patch(park_b, park)
    program.spin()
    return program


# Generous host-schedule margin, not a hardware bound: 5 s at 1.25 MHz.
REMOTE_SCRATCH_DEADLINE_TICKS = 6250000
REMOTE_SCRATCH_IDENTITY_BASE = 0x30
REMOTE_SCRATCH_ARMED = 0x31
REMOTE_SCRATCH_GO = 0x32
REMOTE_SCRATCH_DONE = 0x33
REMOTE_SCRATCH_TIMEOUT = 0x3f


def build_remote_scratch_notification():
    """Node zero writes directly into node one's own Hub scratch register
    by its node-qualified physical address (the stock PROM's other way to
    reach a neighbor, once that neighbor's number is known, rather than
    by vector route) and requires the peer to answer inside an
    RTC-ticks deadline. Node one only polls its own local register (the
    one node zero just wrote) and answers on its own scratch word. Both
    slice-B CPUs park; both slice-A CPUs run this same image and take one
    of the two roles by their own identity."""
    program = PromImage()
    emit_prologue(program)
    park_b = emit_park_slice_b_then_number_peer(program)
    program.li64(R_S2, HUB_LOCAL_WINDOW | HUB_PI_RT_COUNT)
    # s3: node one's NI_SCRATCH_0 by its node-qualified physical address
    # (node 1 is physical bit 32).
    program.li64(R_S3, 0x9200000101000000 | HUB_NI_SCRATCH_0)

    emit_identity(program, R_T0)
    program.emit(mips_r(0, R_T0, R_T0, 1, 0x02))                # srl: node

    program.emit(ori(R_T1, R_T0, REMOTE_SCRATCH_IDENTITY_BASE))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_0)))
    node1 = program.branch(0x05, R_T0, R_ZERO)

    # Node zero: wait for the peer's ARMED, then GO with a deadline.
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_ARMED))
    emit_wait_equal(program, rel(HUB_NI_SCRATCH_1), R_T1)

    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    load_u32(program, R_T0, REMOTE_SCRATCH_DEADLINE_TICKS)
    program.emit(mips_r(R_T2, R_T0, R_T2, 0, 0x21))             # addu
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_GO))
    program.emit(mips_i(0x3f, R_S3, R_T1, 0))
    emit_sync(program)
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_DONE))

    wait_done = program.here()
    emit_vector_read(program, R_S0, R_T0, rel(HUB_NI_SCRATCH_1))
    complete = program.branch(0x04, R_T0, R_T1)
    program.emit(mips_i(0x37, R_S2, R_T3, 0))
    program.emit(mips_r(R_T3, R_T2, R_T4, 0, 0x2b))             # sltu
    program.patch(program.branch(0x05, R_T4, R_ZERO), wait_done)

    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_TIMEOUT))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    program.spin()

    program.patch(complete, program.here())
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    program.spin()

    # Node one: publish ARMED, wait for node zero's direct write of GO,
    # publish DONE.
    program.patch(node1, program.here())
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_ARMED))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    emit_sync(program)
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_GO))
    wait_go = program.here()
    program.emit(mips_i(0x37, R_S0, R_T0, rel(HUB_NI_SCRATCH_0)))
    program.patch(program.branch(0x05, R_T0, R_T1), wait_go)
    program.emit(ori(R_T1, R_ZERO, REMOTE_SCRATCH_DONE))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    program.spin()

    program.patch(park_b, program.here())
    program.spin()
    return program


PEER_SCRATCH_DEADLINE_TICKS = 6250000
PEER_SCRATCH_IDENTITY_BASE = 0x10
PEER_SCRATCH_ARMED = 0x21
PEER_SCRATCH_GO = 0x22
PEER_SCRATCH_DONE = 0x23
PEER_SCRATCH_TIMEOUT = 0x2f
PEER_SCRATCH_INTERRUPT = 114
PEER_SCRATCH_PEND1 = 1 << (PEER_SCRATCH_INTERRUPT - 64)


def build_peer_scratch_deadline():
    """Both nodes' A slices publish an identity and ARMED, entirely by
    vector route, no node number needed, the way the stock PROM's own
    pre-numbering handoff works. Node zero then waits for the peer's
    ARMED, starts an RTC-ticks deadline, and answers GO; node one answers
    GO in turn, then raises interrupt 114 on its own PI block as its last,
    device-side action. Node zero must see DONE inside the deadline."""
    program = PromImage()
    emit_prologue(program)
    park_b = emit_park_slice_b_then_number_peer(program)
    program.li64(R_S2, HUB_LOCAL_WINDOW | HUB_PI_RT_COUNT)

    emit_identity(program, R_T0)
    program.emit(mips_r(0, R_T0, R_T0, 1, 0x02))                # srl: node

    program.emit(ori(R_T1, R_T0, PEER_SCRATCH_IDENTITY_BASE))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_0)))
    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_ARMED))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    emit_sync(program)
    node1 = program.branch(0x05, R_T0, R_ZERO)

    # Node zero: wait for the peer's ARMED, then run the timed handoff.
    emit_wait_equal(program, rel(HUB_NI_SCRATCH_1), R_T1)

    program.emit(mips_i(0x37, R_S2, R_T2, 0))
    load_u32(program, R_T0, PEER_SCRATCH_DEADLINE_TICKS)
    program.emit(mips_r(R_T2, R_T0, R_T2, 0, 0x21))             # addu
    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_GO))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    emit_sync(program)
    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_DONE))

    wait_done = program.here()
    emit_vector_read(program, R_S0, R_T0, rel(HUB_NI_SCRATCH_1))
    program.emit(mips_i(0x37, R_S2, R_T3, 0))
    program.emit(mips_r(R_T3, R_T2, R_T4, 0, 0x2b))             # sltu
    timed_out = program.branch(0x04, R_T4, R_ZERO)
    program.patch(program.branch(0x05, R_T0, R_T1), wait_done)

    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    program.spin()

    program.patch(timed_out, program.here())
    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_TIMEOUT))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    program.spin()

    # Node one: answer GO, wait for the coordinator's DONE, then raise
    # interrupt 114 on its own PI block as its last action.
    program.patch(node1, program.here())
    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_GO))
    emit_wait_equal(program, rel(HUB_NI_SCRATCH_1), R_T1)

    program.emit(ori(R_T1, R_ZERO, PEER_SCRATCH_DONE))
    program.emit(mips_i(0x3f, R_S0, R_T1, rel(HUB_NI_SCRATCH_1)))
    emit_sync(program)
    emit_wait_equal(program, rel(HUB_NI_SCRATCH_1), R_T1)

    program.emit(ori(R_T0, R_ZERO,
                     PI_INT_MODIFY_SET | PEER_SCRATCH_INTERRUPT))
    program.emit(mips_i(0x3f, R_S1, R_T0, HUB_PI_INT_MODIFY))
    program.spin()

    program.patch(park_b, program.here())
    program.spin()
    return program


PEND_READ_STATE = 0x100033000            # node one, uncached
PEND_READ_ROUND = 0x00
PEND_READ_WINDOW_B = 0x08
PEND_READ_WINDOW_A = 0x10
PEND_READ_SET_A = 0x18
PEND_READ_DONE_B = 0x20
PEND_READ_DONE_A = 0x28
PEND_READ_TIMEOUT = 0x30
PEND_READ_FAIR = 0x58                    # 4 words
PEND_READ_ROUNDS = 8
PEND_READ_WAIT_LIMIT = 64
PEND_READ_WAIT_LIMIT_UNBOUNDED = 0xffffffff
PEND_READ_INTERRUPT = 114                # bank 1, bit 50
PEND_READ_UNRELATED_BANK1 = 112
PEND_READ_UNRELATED_BANK0 = 12
PEND_READ_PEND1_BIT = 1 << (PEND_READ_INTERRUPT - 64)
PEND_READ_MASK1_B_VALUE = (PEND_READ_PEND1_BIT |
                           (1 << (PEND_READ_UNRELATED_BANK1 - 64)))


def emit_pend1_bit_test(program, pi_reg):
    """Leaves 1 in t0 when this CPU's local PI_INT_PEND1 carries the
    rendezvous interrupt (bank 1, bit 50), 0 otherwise."""
    program.emit(mips_i(0x37, pi_reg, R_T0, HUB_PI_INT_PEND1))
    program.emit(mips_r(0, R_T0, R_T0, PEND_READ_INTERRUPT - 64 - 32,
                        0x3e))                                  # dsrl32
    program.emit(mips_i(0x0c, R_T0, R_T0, 1))


def emit_wait_nonzero(program, base_reg, offset):
    wait = program.here()
    program.emit(mips_i(0x37, base_reg, R_T0, offset))
    program.patch(program.branch(0x04, R_T0, R_ZERO), wait)


def build_pending_read_rendezvous(wait_limit):
    """The stock PROM's slice-pair rendezvous, as its four-CPU/two-node
    boot actually runs it: bank 1's interrupt 114 set by slice A and
    cleared by slice B through PI_INT_MODIFY, each side bounded-polling
    PI_INT_PEND1 for the other's edge, with further bank-1 bits pending
    across every poll. Node one's pair does not participate; its CPUs
    prove that the rendezvous does not starve them across repeated
    rounds."""
    program = PromImage()
    emit_kx(program)
    emit_assign_peer_nasid_if_master(program, 1)
    emit_wait_for_own_nasid_if_slave(program)

    program.li64(R_S0, PEND_READ_STATE | 0x9000000000000000)
    program.li64(R_S3, HUB_LOCAL_WINDOW)                        # local PI
    emit_identity(program, R_S1, R_S3)                  # s1 = global index
    program.emit(mips_r(0, R_S1, R_S2, 3, 0x00))        # sll s2, s1, 3

    select_a = program.branch(0x04, R_S1, R_ZERO)
    program.emit(ori(R_T0, R_ZERO, 1))
    select_b = program.branch(0x04, R_S1, R_T0)

    # Node one's pair: record that this CPU ran at all, without ever
    # giving up. Looping forever means the one fact under test, whether
    # this CPU ever gets a timeslice, survives however long the host
    # makes it wait; a CPU the model truly never schedules still leaves
    # its slot at zero.
    program.emit(mips_r(R_S0, R_S2, R_S3, 0, 0x2d))             # daddu
    emit_wait_nonzero(program, R_S0, PEND_READ_ROUND)
    observe = program.here()
    program.emit(mips_i(0x37, R_S3, R_T0, PEND_READ_FAIR))
    program.emit(ori(R_T0, R_T0, 1))
    program.emit(mips_i(0x3f, R_S3, R_T0, PEND_READ_FAIR))
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), observe)

    # Slice A: publish the interrupt each round, then bounded-poll for
    # B's acknowledging clear.
    program.patch(select_a, program.here())
    program.emit(ori(R_S2, R_ZERO, 1))                          # s2 = round
    a_round = program.here()
    program.emit(mips_i(0x37, R_S0, R_T0, PEND_READ_ROUND))
    program.patch(program.branch(0x05, R_T0, R_S2), a_round)
    program.emit(ori(R_T0, R_ZERO,
                     PI_INT_MODIFY_SET | PEND_READ_INTERRUPT))
    program.emit(mips_i(0x3f, R_S3, R_T0, HUB_PI_INT_MODIFY))
    program.emit(ori(R_T0, R_ZERO, 1))
    program.emit(mips_i(0x3f, R_S0, R_T0, PEND_READ_WINDOW_A))
    emit_sync(program)
    program.emit(mips_i(0x3f, R_S0, R_S2, PEND_READ_SET_A))
    emit_sync(program)
    load_u32(program, R_T2, wait_limit)
    a_poll = program.here()
    emit_pend1_bit_test(program, R_S3)
    cleared = program.branch(0x04, R_T0, R_ZERO)
    program.emit(addiu(R_T2, R_T2, -1))
    program.patch(program.branch(0x05, R_T2, R_ZERO), a_poll)
    program.emit(ori(R_T0, R_ZERO, 2))
    program.emit(mips_i(0x3f, R_S0, R_T0, PEND_READ_TIMEOUT))
    a_timeout = program.here()
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), a_timeout)
    program.patch(cleared, program.here())
    program.emit(mips_i(0x3f, R_S0, R_ZERO, PEND_READ_WINDOW_A))
    emit_sync(program)
    program.emit(mips_i(0x3f, R_S0, R_S2, PEND_READ_DONE_A))
    emit_sync(program)
    program.emit(addiu(R_S2, R_S2, 1))
    program.emit(mips_i(0x0b, R_S2, R_T0, PEND_READ_ROUNDS + 1))  # sltiu
    program.patch(program.branch(0x05, R_T0, R_ZERO), a_round)
    a_park = program.here()
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), a_park)

    # Slice B: owns the mask and the acknowledgement. Arms each round,
    # bounded-polls for A's set, clears only once A's poll is announced.
    program.patch(select_b, program.here())
    program.emit(ori(R_T0, R_ZERO, PEND_READ_MASK1_B_VALUE >> 48))
    program.emit(mips_r(0, R_T0, R_T0, 16, 0x3c))               # dsll32
    program.emit(mips_i(0x3f, R_S3, R_T0, HUB_PI_INT_MASK1_B))
    program.emit(ori(R_T0, R_ZERO,
                     PI_INT_MODIFY_SET | PEND_READ_UNRELATED_BANK1))
    program.emit(mips_i(0x3f, R_S3, R_T0, HUB_PI_INT_MODIFY))
    program.emit(ori(R_T0, R_ZERO,
                     PI_INT_MODIFY_SET | PEND_READ_UNRELATED_BANK0))
    program.emit(mips_i(0x3f, R_S3, R_T0, HUB_PI_INT_MODIFY))

    program.emit(ori(R_S2, R_ZERO, 1))
    b_round = program.here()
    program.emit(mips_i(0x3f, R_S0, R_S2, PEND_READ_ROUND))
    emit_sync(program)
    program.emit(ori(R_T0, R_ZERO, 1))
    program.emit(mips_i(0x3f, R_S0, R_T0, PEND_READ_WINDOW_B))
    emit_sync(program)
    load_u32(program, R_T2, wait_limit)
    b_poll = program.here()
    emit_pend1_bit_test(program, R_S3)
    set_seen = program.branch(0x05, R_T0, R_ZERO)
    program.emit(addiu(R_T2, R_T2, -1))
    program.patch(program.branch(0x05, R_T2, R_ZERO), b_poll)
    program.emit(ori(R_T0, R_ZERO, 1))
    program.emit(mips_i(0x3f, R_S0, R_T0, PEND_READ_TIMEOUT))
    b_timeout = program.here()
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), b_timeout)
    program.patch(set_seen, program.here())
    program.emit(mips_i(0x3f, R_S0, R_ZERO, PEND_READ_WINDOW_B))
    emit_sync(program)
    program.emit(mips_i(0x3f, R_S0, R_S2, PEND_READ_DONE_B))
    emit_sync(program)
    b_wait_set_a = program.here()
    program.emit(mips_i(0x37, R_S0, R_T0, PEND_READ_SET_A))
    program.patch(program.branch(0x05, R_T0, R_S2), b_wait_set_a)
    program.emit(ori(R_T0, R_ZERO, PEND_READ_INTERRUPT))
    program.emit(mips_i(0x3f, R_S3, R_T0, HUB_PI_INT_MODIFY))
    b_wait_done_a = program.here()
    program.emit(mips_i(0x37, R_S0, R_T0, PEND_READ_DONE_A))
    program.patch(program.branch(0x05, R_T0, R_S2), b_wait_done_a)
    program.emit(addiu(R_S2, R_S2, 1))
    program.emit(mips_i(0x0b, R_S2, R_T0, PEND_READ_ROUNDS + 1))  # sltiu
    program.patch(program.branch(0x05, R_T0, R_ZERO), b_round)
    b_park = program.here()
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), b_park)
    return program


class SN0SMPRendezvous(QemuSystemTest):

    def read_word(self, address, unmapped_ok=False):
        result = self.vm.cmd('human-monitor-command',
                             command_line=f'xp /1gx {address:#x}')
        if unmapped_ok and 'Cannot access memory' in result:
            return None
        match = re.search(r'0x([0-9a-fA-F]{16})', result)
        self.assertIsNotNone(match, result)
        return int(match.group(1), 16)

    def launch(self, program, *accel):
        self.set_machine('origin200')
        image = self.scratch_file('sn0-smp-rendezvous.bin')
        program.write(image, IMAGE_SIZE)
        self.vm.set_machine(MACHINES['origin200_dual'])
        self.vm.add_args('-m', '128M', '-smp', '4', '-cpu', 'R10000',
                         *accel, '-bios', image)
        self.vm.launch()

    def wait_for(self, read, done, what, timeout=120):
        # Completion is a guest observation, independent of host speed;
        # the deadline only bounds a hang.
        deadline = time.monotonic() + timeout
        value = read()
        while not done(value):
            if time.monotonic() > deadline:
                self.fail(f'{what}: {value}')
            time.sleep(0.01)
            value = read()
        return value

    def test_peer_port_error_vector_read_completes_clean(self):
        self.launch(build_peer_port_error_read(),
                    '-accel', 'tcg,thread=multi')
        self.wait_for(lambda: self.read_word(HUB_BY_NODE + HUB_NI_SCRATCH_1),
                      bool, 'the vector read did not complete')
        self.assertEqual(self.read_word(HUB_BY_NODE + HUB_NI_SCRATCH_1),
                         PORT_ERROR_READ_DONE)
        status = self.read_word(HUB_BY_NODE + HUB_NI_VECTOR_STATUS)
        read_data = self.read_word(HUB_BY_NODE + HUB_NI_VECTOR_READ_DATA)
        self.assertEqual(status & (NI_VECTOR_VALID | NI_VECTOR_RESPONSE_MASK),
                         NI_VECTOR_VALID)
        self.assertEqual(read_data, 0)

    def test_peer_io_protect_vector_write_completes_clean(self):
        self.launch(build_peer_io_protect_write(),
                    '-accel', 'tcg,thread=multi')
        self.wait_for(lambda: self.read_word(HUB_BY_NODE + HUB_NI_SCRATCH_1),
                      bool, 'the vector write did not complete')
        self.assertEqual(self.read_word(HUB_BY_NODE + HUB_NI_SCRATCH_1),
                         IO_PROTECT_WRITE_DONE)
        status = self.read_word(HUB_BY_NODE + HUB_NI_VECTOR_STATUS)
        self.assertEqual(status & (NI_VECTOR_VALID | NI_VECTOR_RESPONSE_MASK),
                         NI_VECTOR_VALID | NI_VECTOR_TYPE_WRITE)

    def test_remote_hub_scratch_write_schedules_peer(self):
        self.launch(build_remote_scratch_notification(),
                    '-accel', 'tcg,thread=multi')
        hub0 = HUB_BY_NODE
        hub1 = HUB_BY_NODE + HUB_NODE_STRIDE
        self.wait_for(lambda: self.read_word(hub0 + HUB_NI_SCRATCH_1),
                      bool, 'node zero did not finish')
        self.assertEqual(self.read_word(hub0 + HUB_NI_SCRATCH_1),
                         REMOTE_SCRATCH_DONE)
        self.assertEqual(self.read_word(hub0 + HUB_NI_SCRATCH_0),
                         REMOTE_SCRATCH_IDENTITY_BASE)
        self.assertEqual(self.read_word(hub1 + HUB_NI_SCRATCH_0),
                         REMOTE_SCRATCH_GO)
        self.assertEqual(self.read_word(hub1 + HUB_NI_SCRATCH_1),
                         REMOTE_SCRATCH_DONE)

    def check_peer_scratch_deadline(self, *accel):
        self.launch(build_peer_scratch_deadline(), *accel)
        hub0 = HUB_BY_NODE
        hub1 = HUB_BY_NODE + HUB_NODE_STRIDE

        # Node one raises interrupt 114 only after it has seen the
        # coordinator's DONE; the coordinator's TIMEOUT is the only other
        # terminal state. Wait for either.
        def read():
            pend1 = self.read_word(hub1 + HUB_PI_INT_PEND1,
                                   unmapped_ok=True)
            return pend1, self.read_word(hub0 + HUB_NI_SCRATCH_1)

        self.wait_for(
            read,
            lambda v: (v[0] is not None and v[0] & PEER_SCRATCH_PEND1) or
            v[1] == PEER_SCRATCH_TIMEOUT,
            'the handoff did not finish')

        for node in range(2):
            hub = HUB_BY_NODE + node * HUB_NODE_STRIDE
            self.assertEqual(self.read_word(hub + HUB_NI_SCRATCH_0),
                             PEER_SCRATCH_IDENTITY_BASE + node)
            self.assertEqual(self.read_word(hub + HUB_NI_SCRATCH_1),
                             PEER_SCRATCH_DONE)
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_PEND1), 0)
        self.assertEqual(self.read_word(hub1 + HUB_PI_INT_PEND1),
                         PEER_SCRATCH_PEND1)

    def test_peer_scratch_handoff_before_rtc_deadline(self):
        self.check_peer_scratch_deadline(
            '-accel', 'tcg,thread=single,rr-kick-period-ns=100000')

    def test_peer_scratch_handoff_before_rtc_deadline_thread_multi(self):
        self.check_peer_scratch_deadline('-accel', 'tcg,thread=multi')

    def check_pending_read_rendezvous(self, wait_limit, *accel):
        self.launch(build_pending_read_rendezvous(wait_limit), *accel)
        hub0 = HUB_BY_NODE
        hub1 = HUB_BY_NODE + HUB_NODE_STRIDE

        def read():
            return (self.read_word(PEND_READ_STATE + PEND_READ_DONE_A,
                                   unmapped_ok=True),
                    self.read_word(PEND_READ_STATE + PEND_READ_TIMEOUT,
                                   unmapped_ok=True))

        done_a, timeout = self.wait_for(
            read,
            lambda v: v[0] == PEND_READ_ROUNDS or bool(v[1]),
            'the rendezvous did not finish')
        self.assertEqual(timeout, 0)
        self.assertEqual(done_a, PEND_READ_ROUNDS)

        # Node one's pair spins forever recording its own progress, so a
        # slow host scheduler only delays when fair[2] and fair[3] go
        # nonzero rather than erasing the one chance those CPUs had to
        # prove it; a CPU the model genuinely never schedules still
        # leaves its slot at zero and still fails the assertion below.
        def read_fair():
            return [self.read_word(PEND_READ_STATE + PEND_READ_FAIR + 8 * i)
                    for i in range(4)]

        fair = self.wait_for(read_fair, lambda f: f[2] and f[3],
                             'node one\'s processors never ran', timeout=30)
        self.assertEqual(fair[0], 0)
        self.assertEqual(fair[1], 0)
        self.assertGreater(fair[2], 0)
        self.assertGreater(fair[3], 0)

        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_PEND0),
                         1 << PEND_READ_UNRELATED_BANK0)
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_PEND1),
                         1 << (PEND_READ_UNRELATED_BANK1 - 64))
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_MASK0_A), 0)
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_MASK1_A), 0)
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_MASK0_B), 0)
        self.assertEqual(self.read_word(hub0 + HUB_PI_INT_MASK1_B),
                         PEND_READ_MASK1_B_VALUE)
        for offset in (HUB_PI_INT_PEND0, HUB_PI_INT_PEND1,
                       HUB_PI_INT_MASK0_A, HUB_PI_INT_MASK1_A,
                       HUB_PI_INT_MASK0_B, HUB_PI_INT_MASK1_B):
            self.assertEqual(self.read_word(hub1 + offset), 0, hex(offset))

    def test_pending_read_schedules_local_peer(self):
        self.check_pending_read_rendezvous(
            PEND_READ_WAIT_LIMIT,
            '-accel', 'tcg,thread=single,rr-kick-period-ns=10000000')

    def test_pending_read_rendezvous_thread_multi(self):
        self.check_pending_read_rendezvous(
            PEND_READ_WAIT_LIMIT_UNBOUNDED, '-accel', 'tcg,thread=multi')


if __name__ == '__main__':
    QemuSystemTest.main()
