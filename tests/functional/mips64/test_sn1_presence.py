#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""The Bedrock presence-publication join under TCG, taken by the
processors themselves.

Right after the reset-edge rendezvous the stock IP35 PROM's lowest
processor on each processor interface publishes the interface's fitted
processors as bits 4 to 7 of II scratch 0, under the hub lock, and every
processor's first barrier then reads those bits to decide whom to wait
for: a processor whose bit is still clear is treated as absent. On
hardware the publisher's two stores land long before any peer's first
such read. Under multi-threaded TCG the publisher's host thread can be
left unscheduled across that window, so its peers read the bits clear,
pass their first barrier without it, and stay a barrier ahead of it for
the rest of the boot -- until the copy barrier, where they switch to the
PROM's RAM copy while it is still being made. A successful first barrier
therefore does not prove the processors stayed synchronized.

Four processors run the same program, with the roles the PROM gives them:
every one makes its first LB scratch 1 write; the publishers (slots A and
C) make their second, take the hub lock, publish their interface's two
bits and release, C only after half a second of the real-time counter
has passed; the others (B and D) make their second write at once. Every
processor then records its first read of II scratch 0. The model must
show each publisher seeing all four bits on that read, since a
publisher's first presence read is of the other interface, in the
handshake between the two publishers: A must see C's and D's bits, which
only its park at the release gives it. B's and D's reads are recorded,
not judged: this program has no liveness handshake, so the partner park
the second case covers does not apply. Without the join, A reads before
C's publication and records 0x30.

The handshake case covers B and D. Between the join and the publication
the PROM runs a pair liveness handshake: each processor writes a status
value into its own real-time compare register and polls its partner's for
it. The publisher then publishes; the partner waits out about 3 ms of the
real-time counter and then makes its first presence read. Here both
publishers are late after the handshake, by half a second, so the
partners' 3 ms run out first unless the model parks them until their own
publisher has published. Without that park B reads 0 and D reads 0.

The reset barrier comes before all of this: every processor's first LB
scratch 1 write clears it, and the master's second posts the release the
others poll for. A late processor's clear would erase that release, so the
model withholds the release and parks the master until every processor has
made its first write. The late-join case has B half a second late to its
first write and checks that A's release store took at least that long.

Every case runs under each way TCG can schedule the four processors:
multi-threaded, where a processor's host thread can be left unscheduled;
round robin, where one host thread takes the processors in turn; and
instruction counting, where round robin also drives the virtual clock the
real-time counter follows, so the counter only advances while some
processor executes.
"""

import re
import time

from lib_mips import (R_A0, R_A1, R_A2, R_S0, R_S1, R_S2, R_S3, R_S4, R_S5,
                      R_S6, R_S7, R_T0, R_T1, R_T2, R_T3, R_T4, R_T5, R_ZERO,
                      PromImage, mips_i, mips_r)
from lib_prom import MACHINES, launch_paused
from qemu_test import QemuSystemTest

R_FP = 30

SPACE_IO = 0x20000000000
SPACE_HUB = 0x10000000000
BEDROCK_LOCAL = SPACE_IO + 0x01000000
II_SCRATCH_0 = 0x400150
LB_SCRATCH_1 = 0x608008
LB_SCRATCH_3 = 0x608018
LB_SCRATCH_3_TEST_AND_SET = 0x608058
PI_CPU_NUMBER = 0x20
PI_RT_COMPARE_A = 0x108
PI_RT_COUNTER = 0x30100

# State words in low RAM, clear of the flash and of the 32 KiB
# per-processor window the firmware filter overlays at 1 MiB, reached by
# the guest through Hub special space's uncached alias of its own node
# memory and by the host through the same alias: plain node memory sits
# behind the memory unit's bank translation, which answers nothing until
# firmware configures it. One block per processor, by the PI's own
# processor number.
STATE_PHYS = 0x180000
STATE_HOST = SPACE_HUB + STATE_PHYS
STATE_SLOT_STRIDE = 0x100
STATE_SEEN = 0x00
STATE_DONE = 0x08
STATE_LIVE = 0x10
STATE_WAITED = 0x18

DONE_MAGIC = 0xd04e

# Every fitted slot's bit, once both interfaces have published.
PRESENCE_ALL = 0xf0

# How long C holds back its publication: half a second of the 1.25 MHz
# real-time counter, which follows the host clock under multi-threaded
# TCG, so it is the same under any host load. Far longer than the others
# need to reach their first read, and a quarter of the model's hold
# bound, so a parked processor is woken by the publication rather than
# timing out.
PUBLISH_DELAY_TICKS = 625000

# The handshake case's values: the status each processor posts, and the
# partner's timed wait, 3 ms of the real-time counter, the PROM's own.
POST_VALUE = 0x24

# The reset barrier: the master's second LB scratch 1 write, the value the
# stock PROM releases the other processors with, and a progress code the
# master then writes to its own real-time compare register before any
# liveness post.
SCRATCH1_RELEASE = 0x7f80000
PROGRESS_CODE = 0x3
PARTNER_WAIT_TICKS = 3750

# The liveness case's values: how late B posts (half a second of the
# real-time counter, as a host thread left waiting for a host processor
# through the PROM's fixed 3 s real-time counter delay would make it), and
# how many times each processor reads its partner's register before giving
# up. The PROM reads 200,000 times; this is shorter so the case is quick,
# and still far longer than a partner posting on time needs.
LATE_POST_TICKS = 625000
LIVENESS_TRIES = 20000

# The model's park bound, 2 s, in real-time counter ticks, and the longest
# a poll woken by its partner's post may take: under the bound, so a park
# that only its bound released cannot pass for one the post released, with
# room for round robin's slower turns.
PARK_BOUND_TICKS = 2500000
WOKEN_BY_POST_TICKS = PARK_BOUND_TICKS * 9 // 10

# Each interface's own two presence bits, by interface.
PRESENCE_PI0 = 0x30
PRESENCE_PI1 = 0xc0

STATUS_KX = 0x80
HUB_WINDOW = 0x9200000001000000

OPC_BEQ = 0x04
OPC_BNE = 0x05
OPC_ANDI = 0x0c
OPC_ORI = 0x0d
OPC_XORI = 0x0e
OPC_LUI = 0x0f
OPC_DADDIU = 0x19
OPC_LD = 0x37
OPC_SD = 0x3f


def ori(rt, rs, imm):
    return mips_i(OPC_ORI, rs, rt, imm)


def li32(program, register, value):
    program.emit(mips_i(OPC_LUI, R_ZERO, register, value >> 16))
    program.emit(ori(register, register, value))


def daddu(program, rd, rs, rt):
    program.emit(mips_r(rs, rt, rd, 0, 0x2d))


def dsubu(program, rd, rs, rt):
    program.emit(mips_r(rs, rt, rd, 0, 0x2f))


def sltu(program, rd, rs, rt):
    program.emit(mips_r(rs, rt, rd, 0, 0x2b))


def dsll(program, rd, rt, sa):
    program.emit(mips_r(0, rt, rd, sa, 0x38))


def ld(program, rt, base, offset):
    program.emit(mips_i(OPC_LD, base, rt, offset))


def sd(program, rt, base, offset):
    program.emit(mips_i(OPC_SD, base, rt, offset))


def loop_back(program, op, rs, rt, target):
    program.patch(program.branch(op, rs, rt), target)


# Guest code. s0 is the local hub window, s1 this processor's state block,
# s2 LB scratch 1, s3 II scratch 0, s4 the hub lock's test-and-set twin, s5
# plain LB scratch 3, s6 the real-time counter, and t2 this processor's
# number from the PI: bit 0 its slot within the interface, bit 1 the
# interface.

def emit_rt_wait(program, ticks):
    """Wait out ticks of the real-time counter."""
    ld(program, R_T0, R_S6, 0)
    li32(program, R_T1, ticks)
    daddu(program, R_T5, R_T0, R_T1)                  # deadline
    wait = program.here()
    ld(program, R_T0, R_S6, 0)
    sltu(program, R_T1, R_T0, R_T5)                   # now < deadline
    loop_back(program, OPC_BNE, R_T1, R_ZERO, wait)


def emit_prologue(program, late_b=False):
    # 64-bit kernel addressing, without which the pointers below fault.
    program.emit(0x40000000 | R_T0 << 16 | 12 << 11)           # mfc0
    program.emit(ori(R_T0, R_T0, STATUS_KX))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)           # mtc0
    for _ in range(4):
        program.emit(0)

    # Uncached, I/O space: the local hub register window.
    program.li64(R_S0, HUB_WINDOW)
    program.li64(R_S2, HUB_WINDOW | LB_SCRATCH_1)
    program.li64(R_S3, HUB_WINDOW | II_SCRATCH_0)
    program.li64(R_S4, HUB_WINDOW | LB_SCRATCH_3_TEST_AND_SET)
    program.li64(R_S5, HUB_WINDOW | LB_SCRATCH_3)
    program.li64(R_S6, HUB_WINDOW | PI_RT_COUNTER)

    # This processor's state block, by its processor number.
    program.li64(R_S1, 0x9000000000000000 | STATE_PHYS)
    ld(program, R_T2, R_S0, PI_CPU_NUMBER)
    program.emit(mips_i(OPC_ANDI, R_T2, R_T2, 3))
    dsll(program, R_T0, R_T2, 8)
    daddu(program, R_S1, R_S1, R_T0)

    # Every processor's first LB scratch 1 write: the reset-edge join.
    # With late_b, B makes it half a second late, as a host thread left
    # unscheduled would.
    if late_b:
        program.emit(mips_i(OPC_XORI, R_T2, R_T4, 1))          # zero for B
        to_first = program.branch(OPC_BNE, R_T4, R_ZERO)
        emit_rt_wait(program, LATE_POST_TICKS)
        program.patch(to_first, program.here())
    sd(program, R_ZERO, R_S2, 0)

    # Then the barrier's release, as the stock PROM does it: the master
    # (A) writes the release value, and the others spin until they read
    # it. Every later scratch 1 write stores the same value, so a late
    # reader still sees it.
    li32(program, R_A2, SCRATCH1_RELEASE)
    to_master = program.branch(OPC_BEQ, R_T2, R_ZERO)
    spin = program.here()
    ld(program, R_T0, R_S2, 0)
    loop_back(program, OPC_BNE, R_T0, R_A2, spin)
    to_released = program.branch(OPC_BEQ, R_ZERO, R_ZERO)
    program.patch(to_master, program.here())
    # The master records how long its release store took, in ticks. A
    # branch follows the store, as the PROM's return does, so the store
    # ends a translation block before the second counter read.
    ld(program, R_A0, R_S6, 0)
    sd(program, R_A2, R_S2, 0)
    to_on = program.branch(OPC_BEQ, R_ZERO, R_ZERO)
    program.patch(to_on, program.here())
    ld(program, R_A1, R_S6, 0)
    dsubu(program, R_A1, R_A1, R_A0)
    sd(program, R_A1, R_S1, STATE_WAITED)
    program.patch(to_released, program.here())


def emit_publish(program):
    """Take the hub lock, publish this interface's two bits, release."""
    lock = program.here()
    ld(program, R_T0, R_S4, 0)                         # test-and-set
    loop_back(program, OPC_BNE, R_T0, R_ZERO, lock)
    ld(program, R_T0, R_S3, 0)
    program.emit(ori(R_T1, R_ZERO, 0x30))              # both slots' bits
    program.emit(mips_r(R_T2, R_T1, R_T1, 0, 0x14))    # dsllv: my interface
    program.emit(mips_r(R_T0, R_T1, R_T0, 0, 0x25))    # or
    sd(program, R_T0, R_S3, 0)
    sd(program, R_ZERO, R_S5, 0)                       # release


def emit_record(program):
    """The first presence read, recorded as is, and then a halt in place."""
    ld(program, R_T0, R_S3, 0)
    sd(program, R_T0, R_S1, STATE_SEEN)
    program.emit(ori(R_T0, R_ZERO, DONE_MAGIC))
    sd(program, R_T0, R_S1, STATE_DONE)
    program.spin()


def emit_program_after_join(program):
    # Its slot within the interface: the second one only reads.
    program.emit(mips_i(OPC_ANDI, R_T2, R_T3, 1))
    to_peer = program.branch(OPC_BNE, R_T3, R_ZERO)

    # A publisher. C alone (interface bit set) first waits out the delay
    # on the real-time counter, the way a late host thread would.
    program.emit(mips_i(OPC_ANDI, R_T2, R_T4, 2))
    branch = program.branch(OPC_BEQ, R_T4, R_ZERO)
    emit_rt_wait(program, PUBLISH_DELAY_TICKS)
    program.patch(branch, program.here())

    # The second scratch 1 write, then publish under the hub lock.
    sd(program, R_A2, R_S2, 0)
    emit_publish(program)
    to_read = program.branch(OPC_BEQ, R_ZERO, R_ZERO)

    # The interface's other processor: its second write, then the read.
    program.patch(to_peer, program.here())
    sd(program, R_A2, R_S2, 0)

    program.patch(to_read, program.here())
    emit_record(program)


def build_program():
    program = PromImage()
    emit_prologue(program)
    emit_program_after_join(program)
    return program


def build_late_join_program():
    """The same with B half a second late to its first scratch 1 write,
    so the reset-barrier park withholds A's release until B has
    written."""
    program = PromImage()
    emit_prologue(program, late_b=True)
    emit_program_after_join(program)
    return program


def emit_partner_registers(program):
    """s7 is this processor's own real-time compare register and fp its
    partner's: the register of the other slot of the same interface."""
    program.emit(mips_i(OPC_ANDI, R_T2, R_T3, 1))              # t3 = slot
    dsll(program, R_T4, R_T3, 3)
    daddu(program, R_S7, R_S0, R_T4)
    program.emit(mips_i(OPC_XORI, R_T3, R_T4, 1))        # the partner's slot
    dsll(program, R_T4, R_T4, 3)
    daddu(program, R_FP, R_S0, R_T4)


def build_handshake_program():
    """After both scratch 1 writes, each processor posts to and polls its
    partner's real-time compare register."""
    program = PromImage()
    emit_prologue(program)
    sd(program, R_A2, R_S2, 0)                          # past the join
    emit_partner_registers(program)

    # Post, then poll the partner's register for its post.
    program.emit(ori(R_T0, R_ZERO, POST_VALUE))
    sd(program, R_T0, R_S7, PI_RT_COMPARE_A)
    poll = program.here()
    ld(program, R_T1, R_FP, PI_RT_COMPARE_A)
    loop_back(program, OPC_BNE, R_T1, R_T0, poll)

    to_partner = program.branch(OPC_BNE, R_T3, R_ZERO)

    # A publisher, late: half a second, then publish.
    emit_rt_wait(program, PUBLISH_DELAY_TICKS)
    emit_publish(program)
    to_read = program.branch(OPC_BEQ, R_ZERO, R_ZERO)

    # A partner: its timed wait, then its read.
    program.patch(to_partner, program.here())
    emit_rt_wait(program, PARTNER_WAIT_TICKS)

    program.patch(to_read, program.here())
    emit_record(program)
    return program


def build_liveness_program(b_posts):
    """The handshake program with the PROM's bound on the pair liveness
    poll. Each processor reads its partner's register a fixed number of
    times rather than forever, records in STATE_LIVE whether the
    partner's post arrived, and in STATE_WAITED how many real-time
    counter ticks the poll took from its own post. B posts late, or with
    b_posts false never posts at all. On the PROM a poller that gives up
    clears its partner's CPU enable, and the late processor is lost
    ("CPU failed early init"). The poll counts reads rather than timing
    on the real-time counter, so the partner park never sees it."""
    program = PromImage()
    emit_prologue(program)

    # The master's progress code: a write to its own compare register
    # after the barrier's release and before its join, which is not a
    # post.
    to_not_master = program.branch(OPC_BNE, R_T2, R_ZERO)
    program.emit(ori(R_T0, R_ZERO, PROGRESS_CODE))
    sd(program, R_T0, R_S0, PI_RT_COMPARE_A)
    program.patch(to_not_master, program.here())
    sd(program, R_A2, R_S2, 0)                          # past the join

    emit_partner_registers(program)

    # B alone (processor number 1) posts late, or not at all.
    program.emit(mips_i(OPC_XORI, R_T2, R_T4, 1))        # zero only for B
    to_post = program.branch(OPC_BNE, R_T4, R_ZERO)
    to_partner = None
    if b_posts:
        # Start B's delay only after A sampled the counter and posted.
        # This keeps B's host scheduling before A's post out of A's
        # measurement.
        li32(program, R_T0, POST_VALUE)
        wait_for_partner_post = program.here()
        ld(program, R_T1, R_FP, PI_RT_COMPARE_A)
        loop_back(program, OPC_BNE, R_T1, R_T0, wait_for_partner_post)
        emit_rt_wait(program, LATE_POST_TICKS)
    else:
        to_partner = program.branch(OPC_BEQ, R_ZERO, R_ZERO)
    program.patch(to_post, program.here())

    # Post, then read the partner's register at most LIVENESS_TRIES times.
    ld(program, R_A0, R_S6, 0)                          # when it posted
    program.emit(ori(R_T0, R_ZERO, POST_VALUE))
    sd(program, R_T0, R_S7, PI_RT_COMPARE_A)
    li32(program, R_T5, LIVENESS_TRIES)
    poll = program.here()
    ld(program, R_T1, R_FP, PI_RT_COMPARE_A)
    to_seen = program.branch(OPC_BEQ, R_T1, R_T0)
    program.emit(mips_i(OPC_DADDIU, R_T5, R_T5, 0xffff))   # one try fewer
    loop_back(program, OPC_BNE, R_T5, R_ZERO, poll)
    to_after = program.branch(OPC_BEQ, R_ZERO, R_ZERO)     # gave up
    program.patch(to_seen, program.here())
    program.emit(ori(R_T1, R_ZERO, 1))
    sd(program, R_T1, R_S1, STATE_LIVE)
    program.patch(to_after, program.here())
    ld(program, R_A1, R_S6, 0)
    dsubu(program, R_A1, R_A1, R_A0)
    sd(program, R_A1, R_S1, STATE_WAITED)

    # Then as the handshake program: late publishers, timed partners.
    # With a silent partner, A has already been held for the park's
    # bound, so it publishes at once; C's publisher park, which waits for
    # A's publication, then ends satisfied rather than at its own bound.
    to_poll = program.branch(OPC_BNE, R_T3, R_ZERO)
    to_publish = None
    if not b_posts:
        to_publish = program.branch(OPC_BEQ, R_T2, R_ZERO)
    emit_rt_wait(program, PUBLISH_DELAY_TICKS)
    if not b_posts:
        program.patch(to_publish, program.here())
    emit_publish(program)
    to_read = program.branch(OPC_BEQ, R_ZERO, R_ZERO)
    program.patch(to_poll, program.here())
    if not b_posts:
        program.patch(to_partner, program.here())
    emit_rt_wait(program, PARTNER_WAIT_TICKS)
    program.patch(to_read, program.here())
    emit_record(program)
    return program


# The ways TCG can schedule the four processors.
ACCEL_MTTCG = ('-accel', 'tcg,thread=multi')
ACCEL_RR = ('-accel', 'tcg,thread=single')
ACCEL_ICOUNT = ('-accel', 'tcg,thread=single',
                '-icount', 'shift=5,align=off,sleep=off')


class SN1Presence(QemuSystemTest):

    def read_words(self, address, count=1):
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{count}gx {address:#x}')
        words = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(words), count, result)
        return words

    def state_word(self, number, offset):
        return self.read_words(
            STATE_HOST + number * STATE_SLOT_STRIDE + offset)[0]

    def run_program(self, build, accel):
        """Boot the four processors on build's program under accel, wait
        for every one to record its first presence read, and return what
        each read, by processor number, with the liveness and wait words.
        Both interfaces must have published and the hub lock must have
        come back free, whatever the reads were."""
        self.set_machine('origin300')
        prom = self.scratch_file('sn1-presence-prom.bin')
        build().write(prom)

        self.vm.set_machine(MACHINES['origin300'])
        self.vm.add_args('-cpu', 'R14000', '-smp', '4', *accel,
                         '-bios', prom, '-no-shutdown')
        launch_paused(self.vm)

        # Completion is a guest observation, independent of host speed;
        # the test's own timeout bounds a hang.
        deadline = time.monotonic() + 120
        done = [self.state_word(n, STATE_DONE) for n in range(4)]
        while done != [DONE_MAGIC] * 4:
            if time.monotonic() > deadline:
                self.fail(f'not all four processors finished: {done}')
            time.sleep(0.05)
            done = [self.state_word(n, STATE_DONE) for n in range(4)]

        seen = [self.state_word(n, STATE_SEEN) for n in range(4)]
        live = [self.state_word(n, STATE_LIVE) for n in range(4)]
        waited = [self.state_word(n, STATE_WAITED) for n in range(4)]
        self.log.info('first presence reads: %s, partner post seen: %s, '
                      'polled ticks: %s',
                      [hex(v) for v in seen], live, waited)
        self.assertEqual(self.read_words(BEDROCK_LOCAL + II_SCRATCH_0)[0],
                         PRESENCE_ALL)
        self.assertEqual(self.read_words(BEDROCK_LOCAL + LB_SCRATCH_3)[0], 0)
        return seen, live, waited

    def woken_by_post(self, accel, waited):
        """Whether B's late post can be told apart from A's park bound.
        Both are measured in the real-time counter, which follows the
        virtual clock. Under icount that clock counts instructions.
        Without icount it follows host time, and a host that deschedules
        B's thread for more than a second lets A's park run to its bound
        before B's post is due, which is what the model should do for that
        much guest time. So only the icount variant asserts that the post
        released A. The others log a release by the bound and return
        false, so the caller skips what only a post would have done."""
        if accel == ACCEL_ICOUNT:
            self.assertLess(waited, WOKEN_BY_POST_TICKS)
        elif waited >= WOKEN_BY_POST_TICKS:
            self.log.info('A waited %d ticks, long enough for the park '
                          'bound to release it; guest time follows host '
                          'time under this accelerator', waited)
            return False
        return True

    def check_first_read_sees_every_interface(self, accel):
        seen, _, _ = self.run_program(build_program, accel)
        # Neither publisher read the other interface's bits early.
        self.assertEqual(seen[0], PRESENCE_ALL)
        self.assertEqual(seen[2], PRESENCE_ALL)

    def check_partner_read_sees_its_interface(self, accel):
        seen, _, _ = self.run_program(build_handshake_program, accel)
        # Each partner read its own interface's bits, published late.
        self.assertEqual(seen[1] & PRESENCE_PI0, PRESENCE_PI0)
        self.assertEqual(seen[3] & PRESENCE_PI1, PRESENCE_PI1)
        # And the publishers still saw both interfaces.
        self.assertEqual(seen[0], PRESENCE_ALL)
        self.assertEqual(seen[2], PRESENCE_ALL)

    def check_late_partner_post_survives_liveness_poll(self, accel):
        _, live, waited = self.run_program(
            lambda: build_liveness_program(True), accel)
        # A posted on time and waited for B: its progress code after the
        # barrier's release did not count as its post and park it until
        # B's.
        self.assertGreaterEqual(waited[0], LATE_POST_TICKS // 2)
        # Every processor saw its partner's post, B's late one included,
        # and A was woken by that post, not by the park's bound.
        for n in range(0 if self.woken_by_post(accel, waited[0]) else 1, 4):
            self.assertEqual(live[n], 1, f'processor {n}: {live}')

    def check_silent_partner_releases_at_bound(self, accel):
        seen, live, waited = self.run_program(
            lambda: build_liveness_program(False), accel)
        # A's partner never posted: its park ran to the bound, then it
        # gave up.
        self.assertEqual(live[0], 0)
        self.assertGreaterEqual(waited[0], WOKEN_BY_POST_TICKS)
        # The other pair's handshake went on as usual.
        self.assertEqual(live[2], 1)
        self.assertEqual(live[3], 1)
        # And the join was not abandoned: C's publisher park still waited
        # for A's publication, and D's partner park for C's.
        self.assertEqual(seen[2], PRESENCE_ALL)
        self.assertEqual(seen[3] & PRESENCE_PI1, PRESENCE_PI1)

    def check_release_waits_for_late_first_write(self, accel):
        seen, _, waited = self.run_program(build_late_join_program, accel)
        # A's release did not land until B's late first write had.
        self.assertGreaterEqual(waited[0], LATE_POST_TICKS // 2)
        self.woken_by_post(accel, waited[0])
        # And the publication join after it still held.
        self.assertEqual(seen[0], PRESENCE_ALL)
        self.assertEqual(seen[2], PRESENCE_ALL)

    def test_first_read_sees_every_interface_mttcg(self):
        self.check_first_read_sees_every_interface(ACCEL_MTTCG)

    def test_first_read_sees_every_interface_rr(self):
        self.check_first_read_sees_every_interface(ACCEL_RR)

    def test_first_read_sees_every_interface_icount(self):
        self.check_first_read_sees_every_interface(ACCEL_ICOUNT)

    def test_partner_read_sees_its_interface_mttcg(self):
        self.check_partner_read_sees_its_interface(ACCEL_MTTCG)

    def test_partner_read_sees_its_interface_rr(self):
        self.check_partner_read_sees_its_interface(ACCEL_RR)

    def test_partner_read_sees_its_interface_icount(self):
        self.check_partner_read_sees_its_interface(ACCEL_ICOUNT)

    def test_late_partner_post_mttcg(self):
        self.check_late_partner_post_survives_liveness_poll(ACCEL_MTTCG)

    def test_late_partner_post_rr(self):
        self.check_late_partner_post_survives_liveness_poll(ACCEL_RR)

    def test_late_partner_post_icount(self):
        self.check_late_partner_post_survives_liveness_poll(ACCEL_ICOUNT)

    def test_silent_partner_releases_at_bound_mttcg(self):
        self.check_silent_partner_releases_at_bound(ACCEL_MTTCG)

    def test_silent_partner_releases_at_bound_rr(self):
        self.check_silent_partner_releases_at_bound(ACCEL_RR)

    def test_silent_partner_releases_at_bound_icount(self):
        self.check_silent_partner_releases_at_bound(ACCEL_ICOUNT)

    def test_release_waits_for_late_first_write_mttcg(self):
        self.check_release_waits_for_late_first_write(ACCEL_MTTCG)

    def test_release_waits_for_late_first_write_rr(self):
        self.check_release_waits_for_late_first_write(ACCEL_RR)

    def test_release_waits_for_late_first_write_icount(self):
        self.check_release_waits_for_late_first_write(ACCEL_ICOUNT)


if __name__ == '__main__':
    QemuSystemTest.main()
