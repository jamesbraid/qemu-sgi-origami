#!/usr/bin/env python3
#
# A guest-requested reset stops every processor on the node or system
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Three ways a guest can ask the emulated hardware for a reset that stops
every processor before any of them executes another instruction: a Hub
port reset on the Origin 200 (a single register write), an authenticated
ELSC "rst" command on the same machine (a checksummed ASCII protocol over
the PCF8584), and a guest-issued L1 system-reset request on the Origin 300
(a framed UART protocol to the L1 system controller). Each mechanism is a
different piece of hardware, and the Origin 300 is a different machine
model (SN1/Bedrock) from the Origin 200's SN0/Hub the other two exercise,
so each keeps its own class and its own VM launch; what they share is the
same discriminating question and the same reading of the emulator's own
execution log to answer it.

The check, for all three: the register write that commits the reset,
every translation block entered afterwards, and the processor resets, in
the order they happened. The peer processor spins in a loop whose every
iteration writes, so any slice of execution it is given lands in the log.
Between the write that commits the reset and the reset that follows it
there must be no block entered by any processor but the requester.

One reset is not enough to fail on: whether the peer is given a slice of
execution in that window is a race with the main loop, and it loses that
race often enough to pass a run that should fail. Each exercise therefore
runs twenty resets and reads every one.

Hub port reset (SN0PortReset): a write of the local-reset bit to the
Hub's port-reset register (NI_PORT_RESET) resets the machine directly,
with no protocol on top of the write itself.

ELSC "rst" (SN0ElscReset): an authenticated "rst" sent to the ELSC system
controller resets the machine once the guest has read its "ok"
acknowledgement in full (docs/datasheets/pcf8584-elsc.md). Each cycle
sends two full ELSC transactions -- "pas none" to authenticate (a machine
reset clears the controller's own supervisor flag, so this repeats every
cycle) and then "rst" -- so two otherwise-identical STOP writes land in
the log before each reset. Only the second one commits a deferred action;
the first is a no-op the controller answers and forgets. The log reader
tells them apart by proximity: the write nearest a given reset is always
the "rst" that produced it, whichever earlier STOP writes came before it.

L1 system reset (SN1L1Reset): a guest-issued "system reset" request
(0x2001) to the L1 system controller resets the machine once the guest's
write has completed the request frame (docs/datasheets/l1-controller.md).
The Origin 300 accepts only a full-size stock PROM as -bios and refuses
-kernel outright (hw/mips/sn1.c, sn1_load_prom), so there is no
synthesized-firmware bypass the way the Origin 200 sections above have.
Both processors' own programs are instead placed in RAM with the generic
loader device (hw/core/generic-loader.c): one -device loader with
force-raw and no cpu-num copies each processor's raw image into memory
without touching its program counter, and a second, file-less -device
loader with only addr and cpu-num re-points that processor's PC there on
every reset -- including the L1-triggered ones this test drives, not just
the first. Each entry point is given in the legacy kseg1 form
(sign-extended, unmapped, uncached) rather than the processor's own load
address, so every fetch is valid from the very first instruction, before
the guest program has set up CP0 Status.KX for 64-bit addressing itself,
the same guarantee the real reset vector 0xbfc00000 gives the stock PROM.

Physical memory is not backed until something programs the Bedrock
memory/directory chip's memory-configuration register (hw/misc/sgi-
bedrock-md.c, docs/datasheets/bedrock-memory-directory.md): it resets to
zero, so every bank starts disabled, and the stock PROM's own self-test
and provisional-word sequence is what enables and maps the installed
banks before trusting memory. A third, file-less -device loader with
addr, data and data-len writes the minimum equivalent of that sequence
directly -- enabling the shipped 256 MiB bank at locational bank two and
selecting it onto software bank zero, physical 0x0 through 0x1fffffff,
where STATE and both slices' own load addresses live -- on every reset
its own hook runs for, the same way the PC-setting loaders above do.

The L1 UART sits in the node's Hub-special register window, reached only
through the uncached xkphys attribute the R10000 family drives out to the
system interface (target/mips/system/r10k_addr.c): attribute 0 places the
window at physical 0x10030000000 and attribute 3 is plain memory, window
zero -- physical addresses this section reaches as 0x9000_00000000_0000 |
offset and 0x9600_00000000_0000 | offset respectively, the same
`prefix << 48 | offset` construction the SN0ElscReset section above and
test_sn0_smp.py use for R10000, where the attribute bits are inert and
any value maps straight to the given offset.
"""

import os
import re
import struct
import time
from pathlib import Path

from lib_mips import (R_S0, R_S1, R_S2, R_S3, R_S4, R_T0, R_T1, R_T2, R_T3,
                      R_T4, R_T5, R_ZERO, RESET_RE, TRACE_RE, WRITE_RE,
                      MipsProgram, mips_i, mips_r, reset_windows)
from lib_prom import MACHINES, require_assets

from qemu_test import QemuSystemTest
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern

# Shared uncached RAM at physical 0x30000, reached through xkphys. Memory
# survives the reset, which is how each boot knows which cycle it is in.
# The same layout serves all three sections below; the L1 section names
# its two done words STATE_A_DONE/STATE_B_DONE (its own convention) but
# they are the same two words STATE_DONE and STATE_DONE + 8 the other two
# sections check together as a single two-word slice.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_DONE = 0x18
STATE_A_DONE = STATE_DONE
STATE_B_DONE = STATE_DONE + 8
STATE_WORDS = 5

# Resets per run, and how far the peer processor's counter must run in
# each cycle before the requester proceeds: far enough that the peer is
# demonstrably executing, small enough to keep each cycle short. Shared by
# all three sections.
CYCLES = 20
B_COUNT_TARGET = 8

# The 64-bit kernel mode the firmware itself selects at entry: clears
# ERL/EXL and sets CP0 Status.KX. Architectural, not board- or
# mechanism-specific, so all three sections below use the same value.
STATUS_KX_KERNEL = 0x24400080

# The Hub's own registers, in the node-local window. Shared by the port-
# reset and ELSC sections, both of which run on origin200/SN0.
HUB_LOCAL = 0x01000000
PI_PROCESSOR_NUMBER = 0x000020


class _ResetCycleMixin:
    """read_state and wait_for_both_slices: identical in all three
    sections below, since every mechanism's guest program writes the same
    STATE layout regardless of what triggers the reset. Kept here rather
    than in lib_mips, which stays free of the qemu_test dependency these
    need (self.vm, self.assertEqual, self.fail)."""

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


# === Hub port reset (origin200/SN0) =========================================

NI_PORT_RESET = 0x600008
# Linux's own value: the warm-reset bit beside the local-reset bit.
PORT_RESET_VALUE = 0x81


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
        self.vm.launch()
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


def elsc_committing_write(line):
    """The requesting processor, if this line is a STOP write to the
    ELSC's control register. Matched on the register offset and value
    rather than the whole address, the way this file's port-reset section
    above matches the Hub's port-reset register, so the machine's own
    placement of the Hub window does not appear here twice.

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


class SN0ElscReset(_ResetCycleMixin, QemuSystemTest):

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
        windows = reset_windows(log, elsc_committing_write)
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
        self.assertEqual(len(reset_windows(log, elsc_committing_write)),
                         CYCLES)


# === L1 system reset (origin300/SN1) ========================================

# The Origin 300's own stock PROM, under ORIGAMI_ASSETS.
IP35_FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5.30',
                                 'ip35prom.payload.bin')

# The L1 UART's node-local registers, reached through the Hub-special
# window (attribute 0, window one): established from the stock PROM's own
# transport (docs/datasheets/l1-controller.md, "Transport") and
# hw/misc/sgi-bedrock.c's BEDROCK_L1_UART_OFFSET.
L1_UART_DATA = 0x30000080

# The Bedrock memory/directory chip's memory-configuration register
# (docs/datasheets/bedrock-memory-directory.md, register 0x780008),
# reached through the Bedrock node register window mapped at
# SN1_BEDROCK_NODE_ADDRESS in hw/mips/sgi/sn1-node.c. A -device loader
# writes to that physical address without the CPU's xkphys decode.
MD_MEMORY_CONFIG = 0x20001800000 + 0x780008

# The shipped Origin 300 installs two 256 MiB DIMM banks at locational
# banks two and three, not locational bank zero (hw/mips/sgi/sn1-node.c,
# sn1_installed_bank_mib); the memory-configuration register's bank-select
# field (bits 33:32) maps a locational bank onto a software bank by
# exclusive-or (docs/datasheets/bedrock-memory-directory.md, "Bank
# geometry"), so selecting 2 there puts locational bank two -- the first
# installed one -- at software bank zero, physical 0x0 through
# 0x1fffffff, where STATE and both slices' own load addresses live.
# Bit 8 enables that bank; bits 15:12 carry its size code, 3 for 256 MiB
# (same datasheet, the memory-configuration register's field table).
# The stock PROM performs the equivalent programming itself before
# trusting memory (same datasheet, "Stock-PROM self-test requirement");
# this section never runs the PROM (see the module docstring), so it must
# do the minimum of that job itself or physical memory stays exactly as
# unbacked as it is before the PROM runs on real hardware.
MD_MEMORY_CONFIG_VALUE = (2 << 32) | (3 << 12) | (1 << 8)

# Physical load addresses for each processor's own program; well clear of
# STATE and of each other.
SLICE_A_LOAD = 0x40000
SLICE_B_LOAD = 0x50000
# The kseg1 form of each load address: sign-extended, unmapped, uncached,
# valid from the first fetch after reset -- the same segment the real
# reset vector 0xbfc00000 uses, generalised to an arbitrary low physical
# offset.
KSEG1_BASE = 0xffffffffa0000000
SLICE_A_PC = KSEG1_BASE | SLICE_A_LOAD
SLICE_B_PC = KSEG1_BASE | SLICE_B_LOAD

# The general task's local destination (docs/datasheets/l1-controller.md,
# "The stock IP35 PROM", "local general task"); request 0x2001 is the
# node reset the "reset" command itself performs (change 2 under
# "Contract changes in this revision"), no arguments.
L1_LOCAL_GENERAL_TASK = 0x0ffff006
L1_REQUEST_RESET = 0x2001
# The request subchannel is 16 plus the issuing CPU's index.
L1_REQUESTER_CPU = 0


def l1_fcs(value, byte):
    """The controller's own frame check sequence (hw/misc/sgi-l1.c,
    l1_fcs): CRC-16/CCITT-FCS, the standard PPP async-HDLC checksum."""
    value ^= byte
    for _ in range(8):
        value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value & 0xffff


def l1_frame(payload):
    """Flag, escaped payload, escaped little-endian check sequence, flag --
    exactly l1_emit's own encoding, so l1_parse's magic-value check
    (crc over payload plus check bytes equals 0xf0b8) accepts it."""
    crc = 0xffff
    for byte in payload:
        crc = l1_fcs(crc, byte)
    crc = (~crc) & 0xffff
    raw = bytes(payload) + bytes([crc & 0xff, crc >> 8])
    encoded = bytearray([0x7e])
    for byte in raw:
        if byte in (0x7e, 0x7d):
            encoded.append(0x7d)
            encoded.append(byte ^ 0x20)
        else:
            encoded.append(byte)
    encoded.append(0x7e)
    return bytes(encoded)


def l1_reset_request(cpu_index):
    """The framed bytes of a zero-argument general request for 0x2001
    (system reset) from the given requesting CPU: header, the local
    general task's four-byte destination and the two-byte command --
    the seven-byte payload a zero-argument request is (frame_len - 2 ==
    7 in l1_request's own reading)."""
    header = 0x10 | (cpu_index & 0x1f)
    payload = bytes([header]) + struct.pack('>I', L1_LOCAL_GENERAL_TASK) \
        + struct.pack('>H', L1_REQUEST_RESET)
    return l1_frame(payload)


class L1Program(MipsProgram):
    """lib_mips's assembler plus the two sequences every program in this
    section opens with."""

    def write_reg(self, base, value):
        """*base = value, an 8-bit immediate the caller has already
        resolved."""
        self.emit(mips_i(0x19, R_ZERO, R_T0, value))              # daddiu
        self.emit(mips_i(0x3f, base, R_T0, 0))                    # sd

    def status_kx_kernel(self):
        """Clear ERL/EXL and set CP0 Status.KX -- the 64-bit kernel mode
        every xkphys reference below needs, entered fresh on every reset
        since the kseg1 entry point is unaffected by it."""
        self.emit(mips_i(0x0f, R_ZERO, R_T0, STATUS_KX_KERNEL >> 16))
        self.emit(mips_i(0x0d, R_T0, R_T0, STATUS_KX_KERNEL & 0xffff))
        self.emit(0x40800000 | R_T0 << 16 | 12 << 11)             # mtc0
        self.emit(0)


def build_slice_a():
    """CPU 0: waits for CPU 1 to reach each cycle, then sends the L1's
    framed 0x2001 request -- real guest instructions, not a register
    poke -- CYCLES times over."""
    program = L1Program()
    program.status_kx_kernel()
    program.address(R_S0, 0x9600, STATE)
    program.address(R_S2, 0x9000, L1_UART_DATA)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))                  # t1 = 1

    top = program.here()
    program.emit(mips_i(0x37, R_S0, R_T4, STATE_PHASE))          # ld: cycle
    program.emit(mips_i(0x0b, R_T4, R_T3, CYCLES))                # sltiu
    done = program.branch(0x04, R_T3, R_ZERO)                     # beq
    program.emit(mips_i(0x19, R_T4, R_T5, 1))                     # this cycle

    wait_mark = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_MARK))
    program.patch(program.branch(0x05, R_T2, R_T5), wait_mark)    # bne

    wait_count = program.here()
    program.emit(mips_i(0x37, R_S0, R_T2, STATE_B_COUNT))
    program.emit(mips_i(0x0b, R_T2, R_T3, B_COUNT_TARGET))        # sltiu
    program.patch(program.branch(0x05, R_T3, R_ZERO), wait_count) # bne

    # The cycle is counted before it is asked for, so the boot that
    # follows the reset takes the next one.
    program.emit(mips_i(0x3f, R_S0, R_T5, STATE_PHASE))           # sd

    for byte in l1_reset_request(L1_REQUESTER_CPU):
        program.write_reg(R_S2, byte)
    program.spin()

    program.patch(done, program.here())
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_A_DONE))
    program.spin()
    return program


def build_slice_b():
    """CPU 1: marks the cycle it has reached and spins incrementing a
    counter, so any execution it is given after the closing frame byte
    lands in the log and in memory."""
    program = L1Program()
    program.status_kx_kernel()
    program.address(R_S0, 0x9600, STATE)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 1))                  # t1 = 1

    program.emit(mips_i(0x37, R_S0, R_T4, STATE_PHASE))          # ld: cycle
    program.emit(mips_i(0x0b, R_T4, R_T3, CYCLES))                # sltiu
    done = program.branch(0x04, R_T3, R_ZERO)                     # beq
    program.emit(mips_i(0x19, R_T4, R_T5, 1))                     # this cycle

    program.emit(mips_i(0x3f, R_S0, R_ZERO, STATE_B_COUNT))
    program.emit(mips_i(0x3f, R_S0, R_T5, STATE_B_MARK))
    program.emit(mips_i(0x19, R_ZERO, R_T2, 0))
    loop = program.here()
    program.emit(mips_i(0x19, R_T2, R_T2, 1))                     # daddiu
    program.emit(mips_i(0x3f, R_S0, R_T2, STATE_B_COUNT))
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), loop)

    program.patch(done, program.here())
    program.emit(mips_i(0x3f, R_S0, R_T1, STATE_B_DONE))
    program.spin()
    return program


def build_console_spinner(counter_offset):
    """A processor that continuously writes, so every scheduled block is
    visible in the execution log while a console action is pending."""
    program = L1Program()
    program.status_kx_kernel()
    program.address(R_S0, 0x9600, STATE)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 0))
    loop = program.here()
    program.emit(mips_i(0x19, R_T1, R_T1, 1))
    program.emit(mips_i(0x3f, R_S0, R_T1, counter_offset))
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), loop)
    return program


SHUTDOWN_RE = re.compile(r"^qemu_system_shutdown_request reason=6$")


def l1_committing_write(line):
    """The requesting processor, if this line is a flag-byte write to the
    L1 UART's data register. Both the opening and the closing flag of a
    frame carry the same 0x7e value; lib_mips.reset_windows() picks
    whichever one is nearest a given reset, which the closing flag always
    is."""
    match = WRITE_RE.match(line)
    if not match:
        return None
    cpu, addr, value, name = match.groups()
    if name != 'sgi-l1-uart' or int(addr, 16) & 0xff != 0:
        return None
    if int(value, 16) != 0x7e:
        return None
    return int(cpu)


def shutdown_windows(log):
    """Translation blocks entered after each shutdown request and before
    the power-up reset which ends that stopped interval."""
    windows = []
    entered = None
    for line in log.splitlines():
        if SHUTDOWN_RE.match(line):
            entered = []
            windows.append(entered)
        elif entered is not None and RESET_RE.match(line):
            entered = None
        elif entered is not None and TRACE_RE.match(line):
            entered.append(line)
    return windows


class SN1L1Reset(_ResetCycleMixin, QemuSystemTest):

    def run_resets(self, *accelerator):
        prom = require_assets(self, IP35_FIRMWARE_REL)[IP35_FIRMWARE_REL]

        self.set_machine(MACHINES['origin300'])
        slice_a = self.scratch_file('sn1-l1-reset-a.bin')
        slice_b = self.scratch_file('sn1-l1-reset-b.bin')
        build_slice_a().write(slice_a)
        build_slice_b().write(slice_b)
        log = self.log_file('qemu-exec.log')

        self.vm.set_machine(MACHINES['origin300'])
        self.vm.add_args(
            *accelerator, '-smp', '2', '-m', '512M', '-bios', str(prom),
            '-device',
            f'loader,file={slice_a},force-raw=on,addr={SLICE_A_LOAD:#x}',
            '-device', f'loader,addr={SLICE_A_PC:#x},cpu-num=0',
            '-device',
            f'loader,file={slice_b},force-raw=on,addr={SLICE_B_LOAD:#x}',
            '-device', f'loader,addr={SLICE_B_PC:#x},cpu-num=1',
            # Neither slice's own program runs the PROM, so nothing else
            # programs the memory-configuration register the PROM
            # otherwise writes before trusting memory; do that minimum
            # here, on every reset this loader device's reset hook runs
            # for, the same way the other loader devices above re-point
            # each slice's PC on every reset rather than only the first.
            '-device',
            f'loader,addr={MD_MEMORY_CONFIG:#x},'
            f'data={MD_MEMORY_CONFIG_VALUE:#x},data-len=8,data-be=on,'
            'cpu-num=0',
            '-d', 'exec,trace:memory_region_ops_write,trace:cpu_reset',
            '-D', log)
        self.vm.launch()
        self.wait_for_both_slices()
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def run_console_power_cycles(self):
        prom = require_assets(self, IP35_FIRMWARE_REL)[IP35_FIRMWARE_REL]

        self.set_machine(MACHINES['origin300'])
        slice_a = self.scratch_file('sn1-l1-console-a.bin')
        slice_b = self.scratch_file('sn1-l1-console-b.bin')
        build_console_spinner(STATE_A_DONE).write(slice_a)
        build_console_spinner(STATE_B_DONE).write(slice_b)
        log = self.log_file('qemu-console-exec.log')

        self.vm.set_console()
        self.vm.add_args(
            '-accel', 'tcg,thread=multi', '-smp', '2', '-m', '512M',
            '-bios', str(prom), '-no-shutdown',
            '-device',
            f'loader,file={slice_a},force-raw=on,addr={SLICE_A_LOAD:#x}',
            '-device', f'loader,addr={SLICE_A_PC:#x},cpu-num=0',
            '-device',
            f'loader,file={slice_b},force-raw=on,addr={SLICE_B_LOAD:#x}',
            '-device', f'loader,addr={SLICE_B_PC:#x},cpu-num=1',
            # The spinner's counter lives at STATE, in the same bank
            # run_resets() above enables by hand -- without this loader
            # here too, the bank stays disabled and the spinner's own
            # first access to STATE faults instead of spinning.
            '-device',
            f'loader,addr={MD_MEMORY_CONFIG:#x},'
            f'data={MD_MEMORY_CONFIG_VALUE:#x},data-len=8,data-be=on,'
            'cpu-num=0',
            '-d', 'exec,trace:qemu_system_shutdown_request,trace:cpu_reset',
            '-D', log)
        self.vm.launch()
        wait_for_console_pattern(self, '<CTRL_T> to escape to L1')

        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            state = self.read_state()
            if state[STATE_A_DONE // 8] and state[STATE_B_DONE // 8]:
                break
            time.sleep(0.1)
        else:
            self.fail(f'console spinners did not both run: {state}')

        for _ in range(CYCLES):
            exec_command_and_wait_for_pattern(
                self, '\x14power down', '<CTRL_T> to escape to L1')
            exec_command_and_wait_for_pattern(
                self, '\x14power down', '001c01-L1> ')
            self.vm.event_wait('SHUTDOWN')
            exec_command_and_wait_for_pattern(
                self, 'power up', '<CTRL_T> to escape to L1')
            self.vm.event_wait('RESET')

        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def assert_only_the_requester_ran(self, log):
        windows = reset_windows(log, l1_committing_write)
        self.assertEqual(len(windows), CYCLES,
                         f'{len(windows)} of {CYCLES} resets are in the log')
        ran = [(requester, entered) for requester, entered in windows
               if entered]
        self.log.info('%d resets, %d with a peer running after the '
                      'committing write', len(windows), len(ran))
        self.assertEqual(
            ran, [],
            f'a peer entered a translation block after the closing L1 '
            f'frame flag write in {len(ran)} of {len(windows)} resets')

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_round_robin_stops_the_peer_at_the_write(self):
        """Plain single-threaded round robin, with no fixed clock: a
        regression guard, not the discriminating case below."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single'))

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_instruction_counted_stops_the_peer_at_the_write(self):
        """Under the fixed instruction-count clock -- the mode a
        reproducible run uses -- this is the case the fix in
        hw/misc/sgi-l1.c was written against."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off'))

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_multi_threaded_reset_completes(self):
        """Multi-threaded execution cannot promise the peer stopped before
        any further instruction -- it is running on its own host thread --
        so this only requires that every reset still completes and both
        slices resume."""
        log = self.run_resets('-accel', 'tcg,thread=multi')
        self.assertEqual(len(reset_windows(log, l1_committing_write)),
                         CYCLES)

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_console_power_down_stops_every_cpu(self):
        windows = shutdown_windows(self.run_console_power_cycles())
        self.assertEqual(len(windows), CYCLES)
        ran = [entered for entered in windows if entered]
        self.log.info('%d power-downs, %d with a processor running after '
                      'the shutdown request', len(windows), len(ran))
        self.assertEqual(
            ran, [],
            f'a processor entered a translation block after the L1 console '
            f'requested shutdown in {len(ran)} of {len(windows)} cycles')


if __name__ == '__main__':
    QemuSystemTest.main()
