#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Origin 200 Hub-port and ELSC reset checks."""

from pathlib import Path

from lib_mips import (R_S0, R_S1, R_S2, R_S3, R_S4, R_T0, R_T1, R_T2, R_T3,
                      R_T4, R_T5, R_ZERO, RESET_RE, TRACE_RE, WRITE_RE,
                      MipsProgram, mips_i, mips_r, reset_windows)
from lib_prom import launch_paused
from lib_sn_reset import (B_COUNT_TARGET, CYCLES, STATE, STATE_B_COUNT,
                          STATE_B_MARK, STATE_DONE, STATE_PHASE,
                          STATUS_KX_KERNEL, _ResetCycleMixin)
from qemu_test import QemuSystemTest

# === Hub port reset (origin200/SN0) =========================================

NI_PORT_RESET = 0x600008
# Linux's own value: the warm-reset bit beside the local-reset bit.
PORT_RESET_VALUE = 0x81

# The Hub's own registers, in the node-local window. Shared by the port-
# reset and ELSC sections, both of which run on origin200/SN0.
HUB_LOCAL = 0x01000000
PI_PROCESSOR_NUMBER = 0x000020


def build_port_reset():
    """Slice A resets the machine while slice B spins, CYCLES times over;
    then each slice publishes its own done word."""
    program = MipsProgram()

    # The 64-bit kernel mode the firmware itself selects at entry.
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
    program.emit(mips_i(0x19, R_ZERO, R_T0, PORT_RESET_VALUE))
    program.emit(mips_i(0x3f, R_S2, R_T0, 0))                    # the reset
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


def port_reset_write(line):
    """The requesting processor, if this line is the guest's port-reset
    write. Matched on the register offset rather than the whole address so
    the machine's own placement of the Hub window does not appear here
    twice."""
    match = WRITE_RE.match(line)
    if not match:
        return None
    cpu, addr, value, name = match.groups()
    if name != 'sgi-hub.regs' or int(addr, 16) & 0xffffff != NI_PORT_RESET:
        return None
    if not int(value, 16) & 1:
        return None
    return int(cpu)


def port_reset_windows(log):
    """One entry per port-reset write: the requesting processor and the
    log lines showing another processor entering a translation block
    before the reset that followed.

    Kept here rather than taken from lib_mips.reset_windows: that one
    pairs each reset with the write nearest before it and skips a reset
    with no write at all, where this one walks forward from each write and
    refuses a write that never produced a reset. The two agree on every
    log this exercise has produced, but they are not the same reading of a
    malformed one, so collapsing them is a change to what this test
    accepts, not a move.
    """
    windows = []
    requester = None
    entered = []
    for line in log.splitlines():
        if requester is None:
            requester = port_reset_write(line)
            continue
        if RESET_RE.match(line):
            windows.append((requester, entered))
            requester, entered = None, []
            continue
        trace = TRACE_RE.match(line)
        if trace and int(trace.group(1)) != requester:
            entered.append(line)
    if requester is not None:
        raise AssertionError('the machine never reset after a port-reset '
                             'write')
    return windows


class SN0PortReset(_ResetCycleMixin, QemuSystemTest):

    def run_resets(self, *accelerator):
        self.set_machine('origin200')
        firmware = self.scratch_file('sn0-port-reset.bin')
        build_port_reset().write(firmware)
        log = self.log_file('qemu-exec.log')

        self.vm.set_machine('origin200,debug-leds=off')
        self.vm.add_args(
            *accelerator, '-smp', '2', '-bios', firmware,
            '-d', 'exec,trace:memory_region_ops_write,trace:cpu_reset',
            '-D', log)
        launch_paused(self.vm)
        self.wait_for_both_slices()
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def assert_only_the_requester_ran(self, log):
        windows = port_reset_windows(log)
        self.assertEqual(len(windows), CYCLES,
                         f'{len(windows)} of {CYCLES} resets are in the log')
        ran = [(requester, entered) for requester, entered in windows
               if entered]
        self.log.info('%d resets, %d with a peer running after the write',
                      len(windows), len(ran))
        self.assertEqual(
            [entered[0] for _, entered in ran[:4]], [],
            f'a peer entered a translation block after the port-reset write '
            f'in {len(ran)} of {len(windows)} resets')

    def test_round_robin_stops_the_peer_at_the_write(self):
        """Single-threaded execution: the peer runs nothing at all after
        the write, because the writer's own slice is the last one."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single'))

    def test_instruction_counted_stops_the_peer_at_the_write(self):
        """The same under the fixed instruction-count clock, the mode a
        reproducible run uses."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off'))

    def test_multi_threaded_reset_completes(self):
        """Multi-threaded execution cannot promise the peer stopped before
        any further instruction -- it is running on its own host thread --
        so this only requires that every reset still completes and both
        slices resume."""
        log = self.run_resets('-accel', 'tcg,thread=multi')
        self.assertEqual(len(port_reset_windows(log)), CYCLES)


# === ELSC "rst" (origin200/SN0) =============================================

I2C_DATA = 0x220000
I2C_CTRL = 0x220008

ELSC_ADDR = 0x08

CTL_PIN = 0x80
CTL_ESO = 0x40
CTL_STA = 0x04
CTL_STO = 0x02
CTL_ACK = 0x01

# Framed requests, checksum included: length byte, ASCII text, and a
# checksum byte chosen so the whole packet sums to zero modulo 256.
# Precomputed rather than assembled by
# the guest program, which has no need to compute a checksum it never
# varies.
RST = bytes([0x03]) + b'rst' + bytes([0xa4])
PWR_C = bytes([0x05]) + b'pwr c' + bytes([0x1f])

# The one response this exercise drives back is "ok " (length 3): "rst"
# needs no supervisor mode over I2C. The guest program never reads the
# content, only completes the framing, so the fixed length is all it
# needs.
RESP_TOTAL = 5

# An acknowledged "rst" resets the machine a second later, as the controller
# firmware does, so each cycle costs a second of guest time; a few cycles
# show every requested reset arriving. A "pwr c" holds the machine
# unpowered for nine seconds, so it runs fewer.
ELSC_CYCLES = 3
PWR_C_CYCLES = 2


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
        that is the reset itself.
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


def build_elsc_reset(request=RST, cycles=ELSC_CYCLES):
    """Slice A sends request (resetting the machine) while slice B spins,
    cycles times over; then each slice publishes its own done word."""
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
    program.emit(mips_i(0x0b, R_T4, R_T3, cycles))               # sltiu
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

    program.elsc_command(request)
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


def elsc_committing_write(line):
    """The requesting processor, if this line is a STOP write to the
    ELSC's control register. Matched on the register offset and value
    rather than the whole address, the way this file's port-reset section
    above matches the Hub's port-reset register, so the machine's own
    placement of the Hub window does not appear here twice.

    Every ELSC transaction ends with an identical STOP write -- only an
    "rst" one commits a deferred action -- so this matches every
    occurrence and lib_mips.reset_windows() picks the one that actually
    precedes a given reset. The board's own two-pass
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


class SN0ElscReset(_ResetCycleMixin, QemuSystemTest):
    """An ELSC "rst" resets the machine one second after the guest has read
    its "ok", as the module controller's firmware does: it answers, waits
    a second and then pulses reset. Processors keep running through that
    second, so the peer is free to run after the committing write; those
    cases require that every requested reset arrives, from the slice that
    asked, and that both slices resume after the last one.

    A "pwr c" removes power once its "ok" is read, and the requesting
    processor stops every processor at its next block boundary, as the
    Hub's port reset does. Power comes back after the off interval and the
    power-up hold, through a second reset. Those cases keep the guard that
    no peer runs past the committing write."""

    def run_resets(self, request, cycles, *accelerator, exec_log=False):
        self.set_machine('origin200')
        firmware = self.scratch_file('sn0-elsc-reset.bin')
        build_elsc_reset(request, cycles).write(firmware)
        log = self.log_file('qemu-trace.log')
        events = 'trace:memory_region_ops_write,trace:cpu_reset'

        self.vm.set_machine('origin200,debug-leds=off')
        self.vm.add_args(
            *accelerator, '-smp', '2', '-bios', firmware,
            '-d', ('exec,' if exec_log else '') + events, '-D', log)
        launch_paused(self.vm)
        self.wait_for_both_slices(timeout=180)
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def assert_every_reset_arrived(self, log, resets):
        windows = reset_windows(log, elsc_committing_write)
        self.assertEqual(len(windows), resets,
                         f'{len(windows)} of {resets} resets are in the log')
        self.assertEqual([requester for requester, _ in windows],
                         [0] * resets)
        return windows

    def assert_only_the_requester_ran(self, log):
        # Each power cycle resets twice, as power goes and as it returns,
        # and nothing runs between the two, so the log shows one run.
        windows = self.assert_every_reset_arrived(log, PWR_C_CYCLES)
        ran = [entered for _, entered in windows if entered]
        self.assertEqual(
            [entered[0] for entered in ran[:4]], [],
            f'a peer entered a translation block after the "pwr c" STOP '
            f'write in {len(ran)} of {len(windows)} resets')

    def test_round_robin_reset_arrives(self):
        """Plain single-threaded round robin."""
        self.assert_every_reset_arrived(
            self.run_resets(RST, ELSC_CYCLES, '-accel', 'tcg,thread=single'),
            ELSC_CYCLES)

    def test_instruction_counted_reset_arrives(self):
        """The fixed instruction-count clock a reproducible run uses, where
        the second's wait is counted in instructions."""
        self.assert_every_reset_arrived(
            self.run_resets(RST, ELSC_CYCLES, '-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off'),
            ELSC_CYCLES)

    def test_multi_threaded_reset_completes(self):
        """Multi-threaded execution."""
        self.assert_every_reset_arrived(
            self.run_resets(RST, ELSC_CYCLES, '-accel', 'tcg,thread=multi'),
            ELSC_CYCLES)

    def test_power_cycle_stops_the_peer_at_the_write(self):
        """Round robin under the fixed instruction-count clock: no peer
        enters a translation block between the "pwr c" STOP write and
        either reset of its power cycle."""
        self.assert_only_the_requester_ran(
            self.run_resets(PWR_C, PWR_C_CYCLES, '-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off',
                            exec_log=True))

    def test_round_robin_power_cycle_stops_the_peer(self):
        """Plain single-threaded round robin, with no fixed clock."""
        self.assert_only_the_requester_ran(
            self.run_resets(PWR_C, PWR_C_CYCLES, '-accel', 'tcg,thread=single',
                            exec_log=True))

if __name__ == '__main__':
    QemuSystemTest.main()
