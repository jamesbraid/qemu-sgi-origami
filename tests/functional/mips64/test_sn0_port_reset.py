#!/usr/bin/env python3
#
# A Hub port reset stops every processor where the guest asked for it
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A write of the local-reset bit to the Hub's port-reset register resets
the machine. The board stops both slices of a node together, so no
processor may execute another instruction once that write has landed.

The check is the emulator's own log: the register write, every translation
block entered afterwards, and the processor resets, in the order they
happened. Slice B spins in a loop whose every iteration writes, so any
slice of execution it is given lands in the log. Between each write and
the reset that follows it there must be no block entered by any processor
but the writer.

One reset is not enough to fail on: whether the peer gets a slice of
execution in that window is a race with the main loop, and it loses that
race often enough to pass a run that should fail. The exercise therefore
runs twenty resets and reads every one.
"""

import re
import time
from pathlib import Path

from qemu_test import QemuSystemTest

from lib_mips import (R_S0, R_S1, R_S2, R_T0, R_T1, R_T2, R_T3, R_T4, R_T5,
                      R_ZERO, RESET_RE, TRACE_RE, WRITE_RE, MipsProgram,
                      mips_i, mips_r)

# Shared uncached RAM at physical 0x30000, reached through xkphys. Memory
# survives the reset, which is how each boot knows which cycle it is in.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_DONE = 0x18
STATE_WORDS = 5

# Resets per run, and how far slice B's counter must run in each cycle
# before slice A asks for one: far enough that B is demonstrably
# executing, small enough to keep each cycle short.
CYCLES = 20
B_COUNT_TARGET = 8

# The Hub's own registers, in the node-local window.
HUB_LOCAL = 0x01000000
PI_PROCESSOR_NUMBER = 0x000020
NI_PORT_RESET = 0x600008
# Linux's own value: the warm-reset bit beside the local-reset bit.
PORT_RESET_VALUE = 0x81

STATUS_KX_KERNEL = 0x24400080


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


def reset_windows(log):
    """One entry per port-reset write: the requesting processor and the
    log lines showing another processor entering a translation block
    before the reset that followed.

    Kept here rather than taken from lib_mips: that one pairs each reset
    with the write nearest before it and skips a reset with no write at
    all, where this one walks forward from each write and refuses a write
    that never produced a reset. The two agree on every log this exercise
    has produced, but they are not the same reading of a malformed one,
    so collapsing them is a change to what this test accepts, not a move.
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


class SN0PortReset(QemuSystemTest):

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
        windows = reset_windows(log)
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
        self.assertEqual(len(reset_windows(log)), CYCLES)


if __name__ == '__main__':
    QemuSystemTest.main()
