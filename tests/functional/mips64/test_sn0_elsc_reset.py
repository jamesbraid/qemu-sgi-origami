#!/usr/bin/env python3
#
# An authenticated ELSC "rst" stops every processor where the guest asked
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""An authenticated "rst" sent to the ELSC system controller resets the
machine once the guest has read its "ok" acknowledgement in full. The
board stops every processor together, so no processor may execute another
instruction once that acknowledgement has been delivered.

The check is the emulator's own log: the register writes that drive the
ELSC's checksummed ASCII protocol (docs/datasheets/pcf8584-elsc.md), every
translation block entered afterwards, and the processor resets, in the
order they happened. Slice B spins in a loop whose every iteration writes,
so any slice of execution it is given lands in the log. Between the write
that completes "rst"'s acknowledgement and the reset that follows it there
must be no block entered by any processor but the requester.

Each cycle sends two full ELSC transactions -- "pas none" to authenticate
(a machine reset clears the controller's own supervisor flag, so this
repeats every cycle) and then "rst" -- so two otherwise-identical STOP
writes land in the log before each reset. Only the second one commits a
deferred action; the first is a no-op the controller answers and forgets.
The log reader tells them apart by proximity: the write nearest a given
reset is always the "rst" that produced it, whichever earlier STOP writes
came before it.

One reset is not enough to fail on: whether the peer is given a slice of
execution in that window is a race with the main loop, and it loses that
race often enough to pass a run that should fail. The exercise therefore
runs twenty resets and reads every one.
"""

import re
import time
from pathlib import Path

from qemu_test import QemuSystemTest

from lib_mips import (R_S0, R_S1, R_S3, R_S4, R_T0, R_T1, R_T2, R_T3, R_T4,
                      R_T5, R_ZERO, WRITE_RE, MipsProgram, mips_i, mips_r,
                      reset_windows)

# Shared uncached RAM at physical 0x30000, reached through xkphys. Memory
# survives the reset, which is how each boot knows which cycle it is in.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_DONE = 0x18
STATE_WORDS = 5

# Resets per run, and how far slice B's counter must run in each cycle
# before slice A proceeds: far enough that B is demonstrably executing,
# small enough to keep each cycle short.
CYCLES = 20
B_COUNT_TARGET = 8

# The Hub's own registers, in the node-local window.
HUB_LOCAL = 0x01000000
PI_PROCESSOR_NUMBER = 0x000020
I2C_DATA = 0x220000
I2C_CTRL = 0x220008

STATUS_KX_KERNEL = 0x24400080

ELSC_ADDR = 0x08

CTL_PIN = 0x80
CTL_ESO = 0x40
CTL_STA = 0x04
CTL_STO = 0x02
CTL_ACK = 0x01

# Framed requests, checksum included: length byte, ASCII text, a checksum
# byte chosen so the whole packet sums to zero modulo 256
# (docs/datasheets/pcf8584-elsc.md). Precomputed rather than assembled by
# the guest program, which has no need to compute a checksum it never
# varies.
PAS_NONE = bytes([0x08]) + b'pas none' + bytes([0xe4])
RST = bytes([0x03]) + b'rst' + bytes([0xa4])

# Every response this exercise drives back is "ok" (length 2): the
# controller's factory-default password is "none", and "rst" is answered
# once "pas none" has set the supervisor flag. The guest program never
# reads the content, only completes the framing, so the fixed length is
# all it needs.
RESP_TOTAL = 4


class ElscProgram(MipsProgram):
    """lib_mips's assembler plus the PCF8584 register dance this exercise
    drives the ELSC through."""

    def write_reg(self, base, value):
        """*base = value, an 8-bit immediate the caller has already
        resolved (every ELSC/PCF8584 register byte this exercise writes
        fits in one)."""
        self.emit(mips_i(0x19, R_ZERO, R_T0, value))              # daddiu
        self.emit(mips_i(0x3f, base, R_T0, 0))                    # sd

    def read_reg_discard(self, base):
        """Fetch *base and drop it. The value never matters here -- only
        that the read happened, advancing the PCF8584's own read-ahead the
        way a real driver's read would."""
        self.emit(mips_i(0x37, base, R_T0, 0))                    # ld

    def elsc_command(self, request):
        """One full ELSC transaction over the PCF8584, register writes
        and reads exactly as tests/qtest/sn0-elsc-test.c's
        elsc_transact_at drives them by hand: address plus write, the
        framed request, a repeated START into read direction, then the
        framed response read to its end with a NAK on the last byte, and
        a STOP. The STOP is the boundary event that fires any deferred
        action the response's last text byte has already committed
        (elsc_fire_committed_action in hw/misc/sgi-elsc.c) -- for "rst"
        once "pas none" has authenticated, that is the reset itself.
        """
        self.write_reg(R_S4, CTL_PIN | CTL_ESO)
        self.write_reg(R_S3, ELSC_ADDR << 1)
        self.write_reg(R_S4, CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK)
        for byte in request:
            self.write_reg(R_S3, byte)
        self.write_reg(R_S4, CTL_ESO | CTL_STA | CTL_ACK)
        self.write_reg(R_S3, (ELSC_ADDR << 1) | 1)
        self.read_reg_discard(R_S3)                     # stale byte
        for i in range(1, RESP_TOTAL):
            if i == RESP_TOTAL - 1:
                self.write_reg(R_S4, CTL_ESO)            # NAK the last byte
            self.read_reg_discard(R_S3)
        self.write_reg(R_S4, CTL_PIN | CTL_ESO | CTL_STO)


def build_elsc_reset():
    """Slice A authenticates and resets the machine while slice B spins,
    CYCLES times over; then each slice publishes its own done word."""
    program = ElscProgram()

    # The 64-bit kernel mode the firmware itself selects at entry.
    program.emit(mips_i(0x0f, R_ZERO, R_T0, STATUS_KX_KERNEL >> 16))
    program.emit(mips_i(0x0d, R_T0, R_T0, STATUS_KX_KERNEL & 0xffff))
    program.emit(0x40800000 | R_T0 << 16 | 12 << 11)             # mtc0 Status
    program.emit(0)

    program.address(R_S0, 0x9000, STATE)
    program.address(R_S1, 0x9200, HUB_LOCAL + PI_PROCESSOR_NUMBER)
    program.address(R_S3, 0x9200, HUB_LOCAL + I2C_DATA)
    program.address(R_S4, 0x9200, HUB_LOCAL + I2C_CTRL)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))                  # t1 = 1

    program.emit(mips_i(0x37, R_S0, R_T4, STATE_PHASE))          # ld: cycle
    program.emit(mips_i(0x0b, R_T4, R_T3, CYCLES))               # sltiu
    done = program.branch(0x04, R_T3, R_ZERO)                    # beq
    program.emit(mips_i(0x19, R_T4, R_T5, 1))                    # this cycle

    program.emit(mips_i(0x37, R_S1, R_T0, 0))                    # processor
    slice_b = program.branch(0x05, R_T0, R_ZERO)                 # bne

    # Slice A waits for B to reach this cycle's loop and run in it.
    wait_mark = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_MARK))
    program.patch(program.branch(0x05, R_T2, R_T5), wait_mark)

    wait_count = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_COUNT))
    program.emit(mips_i(0x0b, R_T2, R_T3, B_COUNT_TARGET))       # sltiu
    program.patch(program.branch(0x05, R_T3, R_ZERO), wait_count)

    # The cycle is counted before it is asked for, so the boot that
    # follows the reset takes the next one.
    program.emit(mips_i(0x3f, R_S0, R_T5, STATE_PHASE))          # sd

    # A machine reset clears the controller's supervisor flag
    # (elsc_reset_hold in hw/misc/sgi-elsc.c), so authenticate fresh every
    # cycle before the destructive command.
    program.elsc_command(PAS_NONE)
    program.elsc_command(RST)
    program.spin()

    # Slice B runs a loop whose every iteration writes, so a slice of
    # execution granted to it is visible in the log and in memory.
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


STOP_VALUE = CTL_PIN | CTL_ESO | CTL_STO


def committing_write(line):
    """The requesting processor, if this line is a STOP write to the
    ELSC's control register. Matched on the register offset and value
    rather than the whole address, the way test_sn0_port_reset.py matches
    the Hub's port-reset register, so the machine's own placement of the
    Hub window does not appear here twice.

    Both "pas none" and "rst" end their transaction with an identical
    STOP write -- only the second one commits a deferred action -- so
    this matches every occurrence and lib_mips.reset_windows() picks the
    one that actually precedes a given reset. The board's own two-pass
    reset (hw/mips/sgi/sn0.c) resets each processor twice at machine
    start, before the guest has written anything; that leading run has no
    preceding write, which is how that reader skips it.
    """
    match = WRITE_RE.match(line)
    if not match:
        return None
    cpu, addr, value, name = match.groups()
    if name != 'pcf8584' or int(addr, 16) & 0xffffff != I2C_CTRL:
        return None
    if int(value, 16) != STOP_VALUE:
        return None
    return int(cpu)


class SN0ElscReset(QemuSystemTest):

    def read_state(self):
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{STATE_WORDS}gx {STATE:#x}')
        state = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(state), STATE_WORDS, result)
        return state

    def wait_for_both_slices(self, timeout=60):
        deadline = time.monotonic() + timeout
        state = []
        while time.monotonic() < deadline:
            state = self.read_state()
            if state[STATE_DONE // 8:STATE_DONE // 8 + 2] == [1, 1]:
                return state
            time.sleep(0.1)
        self.fail(f'the machine did not run {CYCLES} resets: {state}')

    def run_resets(self, *accelerator):
        self.set_machine('origin200')
        firmware = self.scratch_file('sn0-elsc-reset.bin')
        build_elsc_reset().write(firmware)
        log = self.log_file('qemu-exec.log')

        self.vm.set_machine('origin200,debug-leds=off')
        self.vm.add_args(
            *accelerator, '-smp', '2', '-bios', firmware,
            '-d', 'exec,trace:memory_region_ops_write,trace:cpu_reset',
            '-D', log)
        self.vm.launch()
        self.wait_for_both_slices()
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def assert_only_the_requester_ran(self, log):
        windows = reset_windows(log, committing_write)
        self.assertEqual(len(windows), CYCLES,
                         f'{len(windows)} of {CYCLES} resets are in the log')
        ran = [(requester, entered) for requester, entered in windows
               if entered]
        self.log.info('%d resets, %d with a peer running after the '
                      'committing write', len(windows), len(ran))
        self.assertEqual(
            [entered[0] for _, entered in ran[:4]], [],
            f'a peer entered a translation block after the "rst" STOP '
            f'write in {len(ran)} of {len(windows)} resets')

    def test_round_robin_stops_the_peer_at_the_write(self):
        """Plain single-threaded round robin, with no fixed clock: measured
        to already keep the peer from running here before this fix as well
        as after, unlike the icount case below. A regression guard, not the
        discriminating case -- this model states no general guarantee about
        unclocked round robin, only what was measured."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single'))

    def test_instruction_counted_stops_the_peer_at_the_write(self):
        """Under the fixed instruction-count clock -- the mode a
        reproducible run uses -- the peer entered a translation block
        between the "rst" STOP write and the reset in 14 of 20 resets
        before this fix (measured 2026-09-14) and in none after it."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off'))

    def test_multi_threaded_reset_completes(self):
        """Multi-threaded execution cannot promise the peer stopped before
        any further instruction -- it is running on its own host thread --
        so this only requires that every reset still completes and both
        slices resume."""
        log = self.run_resets('-accel', 'tcg,thread=multi')
        self.assertEqual(len(reset_windows(log, committing_write)), CYCLES)


if __name__ == '__main__':
    QemuSystemTest.main()
