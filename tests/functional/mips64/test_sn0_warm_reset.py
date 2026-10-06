#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Origin 200 (two modules, four processors) guest-requested warm reset.

Every processor must re-enter a generated ROM after a guest-requested
reset. Public Linux arch/mips/sgi-ip27/ip27-reset.c writes NI_PORT_RESET
with NPR_PORTRESET | NPR_LOCALRESET. Its asm/sn/sn0/hubni.h and hubpi.h
define the reset bits, node ID and processor-number registers used here.

No PROM runs, so nothing numbers node one's Hub; the master module's
processors do it, as the PROM does, and node one's wait for it. A full
machine reset clears the number again, so every boot repeats the exchange.

Everything the host would otherwise have to write is done by the guest
itself, because a functional test only reads guest memory (human monitor
xp): the programs keep their state in node memory, which a reset leaves
alone, and the host reads it back.
"""

import re
import time

from lib_mips import (R_AT, R_S0, R_S1, R_S2, R_S3, R_S4, R_S5, R_T0,
                      R_T1, R_T2, R_T3, R_T4, R_T5, R_ZERO, PromImage,
                      mips_i, mips_r)
from lib_prom import MACHINES
from lib_sn0_hub import (emit_assign_peer_nasid_if_master,
                         emit_read_is_slave,
                         emit_wait_for_own_nasid_if_slave)
from qemu_test import QemuSystemTest

R_T6 = 14
R_T7 = 15

IMAGE_SIZE = 0x1000
NODE_STRIDE = 0x100000000
HUB_LOCAL_WINDOW = 0x9200000001000000
PI_CPU_NUM = 0x20
PI_INT_PEND_MOD = 0x90
PI_INT_PEND0 = 0x98
NI_PORT_RESET = 0x600008
NI_SCRATCH_REG0 = 0x600100
NI_SCRATCH_REG1 = 0x600108
WARM_LOCAL_RESET = 0x81

# Each node's own memory, uncached, past the page the PROM data window
# filters. The words below are per node.
PAGE = 0x00200000
RESET_CYCLES = 32


def emit_set_kx(program):
    # Enter the same 64-bit kernel mode selected by the real PROM.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, 0x2440))
    program.emit(mips_i(0x0d, R_T0, R_T0, 0x0080))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)            # mtc0
    program.emit(0)


def emit_number_nodes(program):
    emit_assign_peer_nasid_if_master(program, 1)
    emit_wait_for_own_nasid_if_slave(program)


def emit_page(program, reg, node_reg):
    """reg = node memory's PAGE, uncached, of the node in node_reg."""
    program.li64(reg, 0x9000000000000000 | PAGE)
    program.emit(mips_r(0, node_reg, R_AT, 0, 0x3c))             # dsll32
    program.emit(mips_r(reg, R_AT, reg, 0, 0x25))                # or


def build_rendezvous(cycles):
    """Every processor counts its entries in its own mailbox word. The
    first processor of node zero waits for all four to reach its own
    count, then requests the reset, until the count passes cycles."""
    program = PromImage()
    emit_set_kx(program)
    emit_number_nodes(program)

    emit_read_is_slave(program, R_S4)                    # s4 = node
    program.li64(R_S3, HUB_LOCAL_WINDOW)
    program.emit(mips_i(0x37, R_S3, R_T1, PI_CPU_NUM))   # ld t1, cpu number
    program.emit(mips_i(0x0c, R_T1, R_T1, 1))            # slot
    emit_page(program, R_S0, R_S4)                       # s0 = node page
    program.emit(mips_r(0, R_T1, R_T2, 3, 0x38))         # dsll t2, slot, 3
    program.emit(mips_r(R_S0, R_T2, R_S1, 0, 0x2d))      # s1 = mailbox

    program.emit(mips_i(0x37, R_S1, R_S2, 0))
    program.emit(mips_i(0x19, R_S2, R_S2, 1))            # s2 = this entry
    program.emit(mips_i(0x3f, R_S1, R_S2, 0))
    program.emit(0x0000000f)                             # sync

    # Only node zero, slice zero requests resets; other CPUs keep
    # executing.
    to_park = program.branch(0x05, R_S4, R_ZERO)
    to_park_b = program.branch(0x05, R_T1, R_ZERO)

    # The four mailboxes, by global index.
    program.li64(R_T6, 0x9000000000000000 | PAGE)
    program.li64(R_T7, 0x9000000000000000 | PAGE | NODE_STRIDE)
    poll = program.here()
    program.emit(mips_i(0x19, R_ZERO, R_T4, 0))          # mismatch accum
    for base, offset in ((R_T6, 0), (R_T6, 8), (R_T7, 0), (R_T7, 8)):
        program.emit(mips_i(0x37, base, R_T5, offset))
        program.emit(mips_r(R_T5, R_S2, R_T5, 0, 0x26))  # xor t5, t5, s2
        program.emit(mips_r(R_T4, R_T5, R_T4, 0, 0x25))  # or t4, t4, t5
    program.patch(program.branch(0x05, R_T4, R_ZERO), poll)

    # Everyone arrived at this count. Past the last cycle, stop.
    program.emit(mips_i(0x0b, R_S2, R_T4, cycles + 1))   # sltiu
    to_park_done = program.branch(0x04, R_T4, R_ZERO)    # count > cycles
    program.emit(0x0000000f)
    program.emit(mips_i(0x0d, R_ZERO, R_T4, WARM_LOCAL_RESET))
    program.li64(R_T5, HUB_LOCAL_WINDOW | NI_PORT_RESET)
    program.emit(mips_i(0x3f, R_T5, R_T4, 0))            # the reset
    program.patch(to_park, program.here())
    program.patch(to_park_b, program.here())
    program.patch(to_park_done, program.here())
    program.spin()
    return program


def build_scratch_publish():
    """Both nodes publish through Hub scratch registers, in two phases
    separated by a full machine reset.

    Each node's first slice (A) seeds scratch 0 with a token, clears
    scratch 1 and the notification bits, then publishes: it stores the
    token to scratch 1, reads it back, syncs and only then raises the
    publish bit. The second slice (B) waits for the publish bit and reads
    scratch 1, which must be the token. A reads B's acknowledgement,
    raises done, and records what it saw in node memory. Node zero's A
    waits for node one's record, then requests the reset for phase one.
    A published token that is not yet visible, or a notification that
    precedes its data, is recorded as a failure bit rather than a hang."""
    program = PromImage()
    publish, ack, done, rb_fail, stale = 7, 8, 9, 10, 11
    owned = 0x1f << publish

    def set_pending(bit):
        program.emit(mips_i(0x0d, R_ZERO, R_T4, 0x100 | bit))
        program.emit(mips_i(0x3f, R_S3, R_T4, PI_INT_PEND_MOD))
        program.emit(mips_i(0x37, R_S3, R_T4, PI_INT_PEND0))
        program.emit(0x0000000f)

    def wait_pending(mask):
        wait = program.here()
        program.emit(mips_i(0x37, R_S3, R_T4, PI_INT_PEND0))
        program.emit(mips_i(0x0c, R_T4, R_T4, mask))
        program.patch(program.branch(0x04, R_T4, R_ZERO), wait)
        program.emit(0x0000000f)

    emit_set_kx(program)
    emit_number_nodes(program)
    emit_read_is_slave(program, R_S4)                    # s4 = node
    program.li64(R_S3, HUB_LOCAL_WINDOW)
    program.emit(mips_i(0x37, R_S3, R_T1, PI_CPU_NUM))
    program.emit(mips_i(0x0c, R_T1, R_T1, 1))            # slot
    emit_page(program, R_S2, R_S4)                       # s2 = own page
    # The phase this boot is: a node memory word a reset leaves alone.
    program.emit(mips_i(0x37, R_S2, R_S5, 0))            # s5 = phase
    # The token: a value per phase and node.
    program.li64(R_S0, 0x13579bdf24680000)
    program.emit(mips_r(0, R_S5, R_T2, 8, 0x38))         # dsll t2, ph, 8
    program.emit(mips_r(R_S0, R_T2, R_S0, 0, 0x2d))
    program.emit(mips_r(R_S0, R_S4, R_S0, 0, 0x2d))
    program.li64(R_T7, HUB_LOCAL_WINDOW | NI_SCRATCH_REG0)   # t7 = REG0

    to_b = program.branch(0x05, R_T1, R_ZERO)

    # Slice A: seed, then publish.
    for bit in range(publish, stale + 1):
        program.emit(mips_i(0x19, R_ZERO, R_T4, bit))     # clear the bit
        program.emit(mips_i(0x3f, R_S3, R_T4, PI_INT_PEND_MOD))
    program.emit(mips_i(0x3f, R_T7, R_S0, 0))            # REG0 = token
    program.emit(mips_i(0x3f, R_T7, R_ZERO, 8))          # REG1 = 0
    program.emit(0x0000000f)
    program.emit(mips_i(0x19, R_S5, R_T4, 1))
    program.emit(mips_i(0x3f, R_S2, R_T4, 8))            # SEEDED = ph + 1
    program.emit(0x0000000f)

    program.emit(mips_i(0x3f, R_T7, R_S0, 8))            # REG1 = token
    program.emit(mips_i(0x37, R_T7, R_T3, 8))            # read it back
    program.emit(0x0000000f)
    to_rb_fail = program.branch(0x05, R_T3, R_S0)
    set_pending(publish)
    wait_pending((1 << ack) | (1 << stale))
    set_pending(done)
    to_record = program.branch(0x04, R_ZERO, R_ZERO)
    program.patch(to_rb_fail, program.here())
    set_pending(rb_fail)
    program.patch(to_record, program.here())

    # Record the outcome in own node memory, by phase.
    program.emit(mips_i(0x37, R_S3, R_T4, PI_INT_PEND0))
    program.emit(mips_i(0x0c, R_T4, R_T4, owned))
    program.emit(mips_i(0x0d, R_T4, R_T4, 1))
    program.emit(mips_r(0, R_S5, R_T5, 3, 0x38))         # phase * 8
    program.emit(mips_r(R_S2, R_T5, R_T5, 0, 0x2d))
    program.emit(mips_i(0x37, R_T7, R_T3, 8))
    program.emit(mips_i(0x3f, R_T5, R_T3, 0x20))         # RES_REG1[phase]
    program.emit(mips_i(0x3f, R_T5, R_T4, 0x10))         # RES_PEND[phase]
    program.emit(0x0000000f)

    # Node one parks; node zero's A waits for node one's record.
    to_park_a = program.branch(0x05, R_S4, R_ZERO)
    program.li64(R_T6, 0x9000000000000000 | PAGE | NODE_STRIDE)
    program.emit(mips_r(0, R_S5, R_T5, 3, 0x38))
    program.emit(mips_r(R_T6, R_T5, R_T5, 0, 0x2d))
    peer = program.here()
    program.emit(mips_i(0x37, R_T5, R_T4, 0x10))
    program.patch(program.branch(0x04, R_T4, R_ZERO), peer)
    # Phase zero: both nodes learn the phase, then the machine resets.
    to_park_p1 = program.branch(0x05, R_S5, R_ZERO)
    program.emit(mips_i(0x19, R_ZERO, R_T4, 1))
    program.emit(mips_i(0x3f, R_S2, R_T4, 0))
    program.emit(mips_i(0x3f, R_T6, R_T4, 0))
    program.emit(0x0000000f)
    program.emit(mips_i(0x0d, R_ZERO, R_T4, WARM_LOCAL_RESET))
    program.li64(R_T5, HUB_LOCAL_WINDOW | NI_PORT_RESET)
    program.emit(mips_i(0x3f, R_T5, R_T4, 0))            # the reset
    program.patch(to_park_a, program.here())
    program.patch(to_park_p1, program.here())
    program.spin()

    # Slice B: wait for the seed, then for the publish, then check.
    program.patch(to_b, program.here())
    program.emit(mips_i(0x19, R_S5, R_T4, 1))
    seeded = program.here()
    program.emit(mips_i(0x37, R_S2, R_T5, 8))
    program.patch(program.branch(0x05, R_T5, R_T4), seeded)
    program.emit(mips_i(0x37, R_T7, R_S0, 0))            # token from REG0
    wait_pending(1 << publish)
    program.emit(mips_i(0x37, R_T7, R_T3, 8))
    to_stale = program.branch(0x05, R_T3, R_S0)
    set_pending(ack)
    program.spin()
    program.patch(to_stale, program.here())
    set_pending(stale)
    program.spin()
    return program


class SN0WarmReset(QemuSystemTest):

    def read_page(self, node, words):
        """The node's page words, or None while the address does not
        decode yet: node one's memory is reachable only once its number
        is assigned."""
        address = node * NODE_STRIDE + PAGE
        result = self.vm.cmd('human-monitor-command',
                             command_line=f'xp /{words}gx {address:#x}')
        if 'Cannot access memory' in result:
            return None
        state = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(state), words, result)
        return state

    def launch(self, build, *accel):
        self.set_machine('origin200')
        image = self.scratch_file('sn0-warm-reset.bin')
        build().write(image, IMAGE_SIZE)
        self.vm.set_machine(MACHINES['origin200_dual'])
        self.vm.add_args('-m', '128M', '-cpu', 'R10000', '-smp', '4',
                         *accel, '-bios', image, '-no-shutdown')
        self.vm.launch()

    def wait_for(self, read, done, what, timeout=120):
        # Completion is a guest observation, independent of host speed;
        # the deadline only bounds a hang, which does not by itself prove
        # a lost processor.
        deadline = time.monotonic() + timeout
        state = read()
        while not done(state):
            if time.monotonic() > deadline:
                self.vm.cmd('human-monitor-command', command_line='stop')
                self.fail(f'{what}: {read()}')
            time.sleep(0.01)
            state = read()
        return state

    def test_four_cpu_rendezvous(self):
        """Repeated resets look for a processor that misses re-entry only
        now and then. Each processor counts its entries; the requester
        resets only after all four reached its own count, so the final
        counts show every processor re-entered after every reset."""
        self.launch(lambda: build_rendezvous(RESET_CYCLES),
                    '-accel', 'tcg,thread=multi')
        final = RESET_CYCLES + 1

        def read():
            node0 = self.read_page(0, 2)
            node1 = self.read_page(1, 2)
            return None if node1 is None else node0 + node1

        def settled(state):
            if state is None:
                return False
            self.assertTrue(all(value <= final for value in state), state)
            return state == [final] * 4

        self.wait_for(read, settled, 'not every processor re-entered')

    def check_scratch_publish(self, thread):
        self.launch(build_scratch_publish,
                    '-accel', f'tcg,thread={thread}')

        def read():
            nodes = [self.read_page(node, 8) for node in range(2)]
            return None if None in nodes else nodes

        # Record words: 0x10 and 0x18 are RES_PEND for phases 0 and 1,
        # 0x20 and 0x28 RES_REG1.
        def settled(nodes):
            return nodes is not None and all(n[3] for n in nodes)

        nodes = self.wait_for(read, settled, 'phase one did not finish')
        complete = (1 << 7) | (1 << 8) | (1 << 9)
        for node, words in enumerate(nodes):
            for phase in range(2):
                pend = words[2 + phase]
                token = 0x13579bdf24680000 + phase * 0x100 + node
                self.assertEqual(pend & ~1, complete,
                                 f'node {node} phase {phase}: {nodes}')
                self.assertEqual(words[4 + phase], token,
                                 f'node {node} phase {phase}: {nodes}')

    def test_scratch_publish_single(self):
        self.check_scratch_publish('single')

    def test_scratch_publish_multi(self):
        self.check_scratch_publish('multi')


if __name__ == '__main__':
    QemuSystemTest.main()
