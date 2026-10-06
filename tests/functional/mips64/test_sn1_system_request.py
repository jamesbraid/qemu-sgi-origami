#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Origin 300 processor system requests racing other accesses to the node.

A secondary-cache instruction or coherent prefetch reaches the node's
memory unit as a system request that has no memory access behind it. The
processor issues it from its own thread, outside the lock that serializes
device access, so it can run at the same moment as another processor's
ordinary register read on the same node, or another processor's system
request. Neither may cost the other anything: the register read must not
fault and no request may be lost.

Both checks need guest code on two processors at once, so each case boots a
generated PROM on two processors under multi-threaded TCG. The unmapped and
refused reads check, on the same machine, what the emulator's log says the
bus error was.
"""

import re
import time
from pathlib import Path

from lib_mips import (R_T0, R_T1, R_T2, R_T3, R_T4, R_ZERO, PromImage,
                      mips_i, mips_r)
from lib_prom import MACHINES, launch_paused
from qemu_test import QemuSystemTest

# The Bedrock memory-configuration register at 0x780008, reached through
# the node register window, selecting software bank zero on the installed
# memory as the stock PROM leaves it. The loader device writes it on every
# reset, before any processor runs: nothing here runs the PROM that would.
SN1_MD_MEMORY_CONFIG = 0x20001800000 + 0x780008
SN1_MD_INSTALLED_SELECT2 = (2 << 32) | 0x3300

# Lines the processors flush, one per slice, and their shared results.
LINE_PHYS = 0x580000
LINE_STRIDE = 0x80
RESULT_PHYS = 0x600000
RESULT_SLOTS = 11
RESULT_SUM_BASE = 0x20
RESULT_ERRORS = 0x40
RESULT_CAUSE = 0x48
RESULT_GO = 0x50


def result_done(slice_):
    return slice_ * 8


def result_sum(slice_):
    return RESULT_SUM_BASE + slice_ * 8


# The board identification register reads this on an Origin 300.
BOARD_ID_VALUE = 0x8001

# Iterations per processor; enough for the loops to overlap many times.
REQUEST_LOOPS = 1 << 20
FOLLOWED_LOOPS = 1 << 16

CP0_STATUS = 12
CP0_CAUSE = 13
CP0_EPC = 14
STATUS_CH_SHIFT = 18
HIT_WRITEBACK_INVALIDATE_S = 0x17
ERET = 0x42000018
SYNC = 0x0000000f

R_K0 = 26
R_K1 = 27
R_T5 = 13
R_T6 = 14


def lui(rt, imm):
    return mips_i(0x0f, 0, rt, imm)


def ori(rt, rs, imm):
    return mips_i(0x0d, rs, rt, imm)


def ld(rt, offset, base):
    return mips_i(0x37, base, rt, offset)


def sd(rt, offset, base):
    return mips_i(0x3f, base, rt, offset)


def daddiu(rt, rs, imm):
    return mips_i(0x19, rs, rt, imm)


def daddu(rd, rs, rt):
    return mips_r(rs, rt, rd, 0, 0x2d)


def emit_address(program, reg, hi, hi_low, lo):
    """reg = the upper 32 bits hi:hi_low, then the zero-extended 32-bit
    lo added."""
    program.emit(lui(reg, hi))
    program.emit(ori(reg, reg, hi_low))
    program.emit(mips_r(0, reg, reg, 0, 0x3c))           # dsll32
    program.emit(lui(R_T3, lo >> 16))
    program.emit(ori(R_T3, R_T3, lo & 0xffff))
    program.emit(mips_r(0, R_T3, R_T3, 0, 0x3c))         # dsll32
    program.emit(mips_r(0, R_T3, R_T3, 0, 0x3e))         # dsrl32
    program.emit(daddu(reg, reg, R_T3))


def emit_count(program, count):
    program.emit(lui(R_T5, count >> 16))
    program.emit(ori(R_T5, R_T5, count & 0xffff))


def emit_loop_end(program, top):
    """Count down and branch back to top while the counter is nonzero."""
    program.emit(daddiu(R_T5, R_T5, -1))
    program.patch(program.branch(0x05, R_T5, R_ZERO), top)


def emit_finish(program, slice_):
    """Publish the sum for slice_, then that it is done, and stop."""
    program.emit(sd(R_T6, result_sum(slice_), R_T2))
    program.emit(ori(R_T3, R_ZERO, 1))
    program.emit(SYNC)
    program.emit(sd(R_T3, result_done(slice_), R_T2))
    program.spin()


def flush_repeatedly(program):
    """Hit WriteBack Invalidate (S) this slice's line, over and over."""
    emit_count(program, REQUEST_LOOPS)
    top = program.here()
    program.emit(mips_i(0x2f, R_T0, HIT_WRITEBACK_INVALIDATE_S, 0))
    emit_loop_end(program, top)


def emit_board_id_address(program):
    """Point t1 at the local board identification register, uncached."""
    program.emit(lui(R_T1, 0x9000))
    program.emit(mips_r(0, R_T1, R_T1, 0, 0x3c))
    program.emit(lui(R_T3, 0x1400))
    program.emit(daddu(R_T1, R_T1, R_T3))


def read_board_id_repeatedly(program):
    """Read the local board identification register over and over,
    summing what each read returns; a read that faults adds nothing."""
    emit_board_id_address(program)
    emit_count(program, REQUEST_LOOPS)
    top = program.here()
    program.emit(daddu(R_T3, R_ZERO, R_ZERO))
    program.emit(ld(R_T3, 0, R_T1))
    program.emit(daddu(R_T6, R_T6, R_T3))
    emit_loop_end(program, top)


def emit_dirty_and_flush_loop(program):
    """Dirty this slice's line, flush it with Hit WriteBack Invalidate
    (S) and add Status.CH to the sum, over and over."""
    emit_count(program, FOLLOWED_LOOPS)
    top = program.here()
    program.emit(sd(R_ZERO, 0, R_T0))
    program.emit(mips_i(0x2f, R_T0, HIT_WRITEBACK_INVALIDATE_S, 0))
    program.emit(0x40000000 | R_T3 << 16 | CP0_STATUS << 11)    # mfc0
    program.emit(mips_r(0, R_T3, R_T3, STATUS_CH_SHIFT, 0x02))  # srl
    program.emit(mips_i(0x0c, R_T3, R_T3, 1))                   # andi
    program.emit(daddu(R_T6, R_T6, R_T3))
    emit_loop_end(program, top)


def follow_page_then_dirty_and_flush(program):
    """Name slice A's line and read its directory word, which has the
    memory unit follow the line's page; then let slice C go, and loop."""
    # Node zero's directory word for the line, through the uncached form.
    emit_address(program, R_T1, 0x9000, 0x0001,
                 0xc0000000 | ((LINE_PHYS >> 3) & ~0xf))
    program.emit(mips_i(0x2f, R_T0, HIT_WRITEBACK_INVALIDATE_S, 0))
    program.emit(SYNC)
    program.emit(ld(R_T3, 0, R_T1))
    program.emit(ori(R_T3, R_ZERO, 1))
    program.emit(SYNC)
    program.emit(sd(R_T3, RESULT_GO, R_T2))
    emit_dirty_and_flush_loop(program)


def wait_then_dirty_and_flush(program):
    wait = program.here()
    program.emit(ld(R_T3, RESULT_GO, R_T2))
    program.patch(program.branch(0x04, R_T3, R_ZERO), wait)
    emit_dirty_and_flush_loop(program)


def read_board_id_narrow(program):
    """A four-byte read of the board ID, which answers only eight bytes."""
    emit_board_id_address(program)
    program.emit(mips_i(0x23, R_T1, R_T3, 0))                   # lw


def read_past_board_id(program):
    """An eight-byte read 0x0500_0000 above the board ID register, which
    the IP35 address map leaves unmapped."""
    emit_board_id_address(program)
    program.emit(lui(R_T3, 0x0500))
    program.emit(daddu(R_T1, R_T1, R_T3))
    program.emit(ld(R_T3, 0, R_T1))


def do_nothing(program):
    pass


def build_prom(bodies):
    """Every processor runs the same image and finds its slice (A to D,
    0 to 3) in its CPU number, then runs bodies[slice]. A general
    exception counts itself, records its cause and resumes after the
    faulting instruction, so a bus error on a load shows as a count and a
    short sum, not a hang."""
    program = PromImage()
    program.set_vector(0x380, [
        ld(R_K1, RESULT_ERRORS, R_T2),
        daddiu(R_K1, R_K1, 1),
        sd(R_K1, RESULT_ERRORS, R_T2),
        0x40000000 | R_K1 << 16 | CP0_CAUSE << 11,              # mfc0
        sd(R_K1, RESULT_CAUSE, R_T2),
        0x40200000 | R_K0 << 16 | CP0_EPC << 11,                # dmfc0
        daddiu(R_K0, R_K0, 4),
        0x40a00000 | R_K0 << 16 | CP0_EPC << 11,                # dmtc0
        ERET,
    ])

    # 64-bit kernel addressing for the xkphys pointers below, and error
    # level off, so that ERET returns to EPC rather than ErrorEPC.
    program.emit(0x40000000 | R_T4 << 16 | CP0_STATUS << 11)    # mfc0
    program.emit(ori(R_T4, R_T4, 0x84))
    program.emit(mips_i(0x0e, R_T4, R_T4, 0x04))                # xori
    program.emit(0x40800000 | R_T4 << 16 | CP0_STATUS << 11)    # mtc0
    program.emit(0)
    emit_address(program, R_T2, 0xa800, 0, RESULT_PHYS)
    # Which slice this processor is: bits 1:0 of its CPU number.
    program.emit(lui(R_T4, 0x9200))
    program.emit(mips_r(0, R_T4, R_T4, 0, 0x3c))                # dsll32
    program.emit(lui(R_T3, 0x0100))
    program.emit(daddu(R_T4, R_T4, R_T3))
    program.emit(ld(R_T4, 0x20, R_T4))
    program.emit(mips_i(0x0c, R_T4, R_T4, 3))                   # andi
    jumps = []
    for slice_ in range(4):
        program.emit(ori(R_T3, R_ZERO, slice_))
        jumps.append(program.branch(0x04, R_T4, R_T3))
    program.spin()

    for slice_ in range(4):
        target = (0x1000 + slice_ * 0x1000) // 4
        assert program.here() <= target
        program.instructions += [0] * (target - program.here())
        program.patch(jumps[slice_], target)
        emit_address(program, R_T0, 0xa800, 0,
                     LINE_PHYS + slice_ * LINE_STRIDE)
        program.emit(daddu(R_T6, R_ZERO, R_ZERO))
        body = bodies.get(slice_)
        if body:
            body(program)
            emit_finish(program, slice_)
        else:
            program.spin()
    return program


class SN1SystemRequest(QemuSystemTest):

    def read_results(self):
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{RESULT_SLOTS}gx {RESULT_PHYS:#x}')
        words = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(words), RESULT_SLOTS, result)
        return {offset: words[offset // 8]
                for offset in range(0, RESULT_SLOTS * 8, 8)}

    def run_bodies(self, bodies, *extra):
        """Start both processors, with further arguments extra, and wait
        for slices A and C to finish; return the results."""
        self.set_machine('origin300')
        prom = self.scratch_file('sn1-system-request-prom.bin')
        build_prom(bodies).write(prom)

        self.vm.set_machine(MACHINES['origin300'])
        self.vm.add_args(
            '-cpu', 'R14000', '-smp', '2', '-m', '512M', '-bios', prom,
            '-accel', 'tcg,thread=multi', '-no-shutdown',
            '-device',
            f'loader,addr={SN1_MD_MEMORY_CONFIG:#x},'
            f'data={SN1_MD_INSTALLED_SELECT2:#x},data-len=8,data-be=on,'
            'cpu-num=0',
            *extra)
        launch_paused(self.vm)

        # Completion is a guest observation, independent of host speed;
        # the test's own timeout bounds a hang.
        deadline = time.monotonic() + 120
        results = self.read_results()
        while not (results[result_done(0)] and results[result_done(2)]):
            if time.monotonic() > deadline:
                self.fail(f'slices A and C did not finish: {results}')
            time.sleep(0.1)
            results = self.read_results()
        self.assertEqual(results[result_done(0)], 1, results)
        self.assertEqual(results[result_done(2)], 1, results)
        return results

    def test_register_read_beside_requests(self):
        """One processor's cache instructions run while the other reads a
        register of the same node. The register read happens under the
        device lock; the cache instruction does not, and must not make
        the node refuse the read."""
        results = self.run_bodies({0: flush_repeatedly,
                                   2: read_board_id_repeatedly})
        self.assertEqual(results[RESULT_CAUSE], 0, results)
        self.assertEqual(results[RESULT_ERRORS], 0, results)
        self.assertEqual(results[result_sum(2)],
                         REQUEST_LOOPS * BOARD_ID_VALUE, results)

    def test_concurrent_requests_all_served(self):
        """Both processors of a node dirty their own line on a followed
        page and flush it with Hit WriteBack Invalidate (S), at the same
        time. Every flush finds the dirty copy the store before it made,
        and says so in Status.CH, only if the node served every request;
        a refused one reads as a miss."""
        results = self.run_bodies({0: follow_page_then_dirty_and_flush,
                                   2: wait_then_dirty_and_flush})
        self.assertEqual(results[RESULT_CAUSE], 0, results)
        self.assertEqual(results[RESULT_ERRORS], 0, results)
        self.assertEqual(results[result_sum(0)], FOLLOWED_LOOPS, results)
        self.assertEqual(results[result_sum(2)], FOLLOWED_LOOPS, results)

    def run_logged_bus_error(self, body0):
        """Run body0 on slice A with the unimplemented-access log on,
        check it took one data bus error, and return the log's text."""
        log = self.log_file('qemu-unimp.log')
        results = self.run_bodies({0: body0, 2: do_nothing},
                                  '-d', 'unimp', '-D', log)
        self.assertEqual(results[RESULT_CAUSE] & 0x7c, 7 << 2, results)
        self.assertEqual(results[RESULT_ERRORS], 1, results)
        # The log is flushed when the emulator exits.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def test_refused_read_is_named(self):
        """The bus error a device's refusal raises names the result, a
        device error. The log said the access "reached no device" for
        every failure, which sent the investigation of the refused board
        ID reads looking for a hole in the address map."""
        text = self.run_logged_bus_error(read_board_id_narrow)
        self.assertIn('mips: load of 4 bytes at 0x9000000014000000 '
                      '(physical 0x10014000000) failed with a device '
                      'error; raising a bus error', text)
        self.assertNotIn('decode error', text)

    def test_unmapped_read_is_named(self):
        """A read of an unmapped address names its result, a decode
        error."""
        text = self.run_logged_bus_error(read_past_board_id)
        self.assertIn('mips: load of 8 bytes at 0x9000000019000000 '
                      '(physical 0x10019000000) failed with a decode '
                      'error; raising a bus error', text)
        self.assertNotIn('device error', text)


if __name__ == '__main__':
    QemuSystemTest.main()
