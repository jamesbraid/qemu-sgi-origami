#!/usr/bin/env python3
#
# A guest-driven L1 node reset stops every processor on the Origin 300
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""A guest-issued "system reset" request (0x2001) to the L1 system
controller resets the machine once the guest's write has completed the
request frame. The board stops every processor together, so no processor
may execute another instruction once that write has landed.

The check is the emulator's own log: the register writes that drive the
L1's framed UART protocol (docs/datasheets/l1-controller.md), every
translation block entered afterwards, and the processor resets, in the
order they happened. CPU 1 spins in a loop whose every iteration writes,
so any slice of execution it is given lands in the log. Between the write
that completes the closing frame flag and the reset that follows it there
must be no block entered by any processor but the requester.

The Origin 300 machine accepts only a full-size stock PROM as -bios and
refuses -kernel outright (hw/mips/sn1.c, sn1_load_prom), so there is no
synthesized-firmware bypass the way origin200 has. Both processors' own
programs are instead placed in RAM with the generic loader device
(hw/core/generic-loader.c): one -device loader with force-raw and no
cpu-num copies each processor's raw image into memory without touching
its program counter, and a second, file-less -device loader with only
addr and cpu-num re-points that processor's PC there on every reset --
including the L1-triggered ones this test drives, not just the first.
Each entry point is given in the legacy kseg1 form (sign-extended,
unmapped, uncached) rather than the processor's own load address, so
every fetch is valid from the very first instruction, before the guest
program has set up CP0 Status.KX for 64-bit addressing itself, the same
guarantee the real reset vector 0xbfc00000 gives the stock PROM.

The L1 UART sits in the node's Hub-special register window, reached only
through the uncached xkphys attribute the R10000 family drives out to
the system interface (target/mips/system/r10k_addr.c): attribute 0 places
the window at physical 0x10030000000 and attribute 3 is plain memory,
window zero -- physical addresses this test reaches as 0x9000_00000000_0000
| offset and 0x9600_00000000_0000 | offset respectively, the same
`prefix << 48 | offset` construction test_sn0_elsc_reset.py and
test_sn0_smp.py use for R10000, where the attribute bits are inert and
any value maps straight to the given offset.

One reset is not enough to fail on: whether the peer is given a slice of
execution in that window is a race with the main loop, and it loses that
race often enough to pass a run that should fail. The exercise therefore
runs twenty resets and reads every one.
"""

import os
import re
import struct
import time
from pathlib import Path

from lib_prom import resolve_asset

from qemu_test import QemuSystemTest
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


R_ZERO = 0
R_AT = 1
R_T0 = 8
R_T1 = 9
R_T2 = 10
R_T3 = 11
R_T4 = 12
R_T5 = 13
R_S0 = 16
R_S2 = 18

# Shared uncached RAM, reached through the plain-memory xkphys window
# (attribute 3, window zero). Persists across the resets this test drives,
# which is how each boot knows which cycle it is in.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_A_DONE = 0x18
STATE_B_DONE = 0x20
STATE_WORDS = 5

# Resets per run, and how far CPU 1's counter must run in each cycle before
# CPU 0 proceeds: far enough that CPU 1 is demonstrably executing, small
# enough to keep each cycle short.
CYCLES = 20
B_COUNT_TARGET = 8

# The L1 UART's node-local registers, reached through the Hub-special
# window (attribute 0, window one): established from the stock PROM's own
# transport (docs/datasheets/l1-controller.md, "Transport") and
# hw/misc/sgi-bedrock.c's BEDROCK_L1_UART_OFFSET.
L1_UART_DATA = 0x30000080

# The 64-bit kernel mode the firmware itself selects at entry: clears
# ERL/EXL and sets CP0 Status.KX, the same constant
# test_sn0_elsc_reset.py uses (architectural, not board-specific).
STATUS_KX_KERNEL = 0x24400080

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


def mips_i(op, rs, rt, immediate):
    return op << 26 | rs << 21 | rt << 16 | immediate & 0xffff


def mips_r(rs, rt, rd, shift, function):
    return rs << 21 | rt << 16 | rd << 11 | shift << 6 | function


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


class MipsProgram:
    def __init__(self):
        self.instructions = []

    def emit(self, instruction):
        self.instructions.append(instruction)

    def here(self):
        return len(self.instructions)

    def branch(self, op, rs, rt):
        branch = self.here()
        self.emit(mips_i(op, rs, rt, 0))
        self.emit(0)                      # branch delay slot
        return branch

    def patch(self, branch, target):
        displacement = target - branch - 1
        assert -(1 << 15) <= displacement < (1 << 15)
        self.instructions[branch] |= displacement & 0xffff

    def spin(self):
        spin = self.here()
        self.patch(self.branch(0x04, R_ZERO, R_ZERO), spin)

    def address(self, register, prefix, offset):
        """register = (prefix << 48) | offset -- the xkphys form every
        R10000-family board reaches its uncached windows through
        (test_sn0_elsc_reset.py's address(), test_sn0_smp.py's
        address_prefix()/load_xkphys_address())."""
        self.emit(mips_i(0x0f, R_ZERO, register, prefix))
        self.emit(mips_r(R_ZERO, register, register, 0, 0x3c))   # dsll32
        self.emit(mips_i(0x0f, R_ZERO, R_AT, offset >> 16))
        self.emit(mips_i(0x0d, R_AT, R_AT, offset & 0xffff))
        self.emit(mips_r(register, R_AT, register, 0, 0x25))     # or

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

    def write(self, path):
        Path(path).write_bytes(b''.join(
            struct.pack('>I', instruction)
            for instruction in self.instructions))


def build_slice_a():
    """CPU 0: waits for CPU 1 to reach each cycle, then sends the L1's
    framed 0x2001 request -- real guest instructions, not a register
    poke -- CYCLES times over."""
    program = MipsProgram()
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
    program = MipsProgram()
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
    program = MipsProgram()
    program.status_kx_kernel()
    program.address(R_S0, 0x9600, STATE)
    program.emit(mips_i(0x19, R_ZERO, R_T1, 0))
    loop = program.here()
    program.emit(mips_i(0x19, R_T1, R_T1, 1))
    program.emit(mips_i(0x3f, R_S0, R_T1, counter_offset))
    program.patch(program.branch(0x04, R_ZERO, R_ZERO), loop)
    return program


WRITE_RE = re.compile(
    r"memory_region_ops_write cpu (-?\d+) mr \S+ addr 0x([0-9a-f]+) "
    r"value 0x([0-9a-f]+) size \d+ name '([^']*)'")
TRACE_RE = re.compile(r"^Trace (\d+):")
RESET_RE = re.compile(r"^cpu_reset (\d+)")
SHUTDOWN_RE = re.compile(r"^qemu_system_shutdown_request reason=6$")


def committing_write(line):
    """The requesting processor, if this line is a flag-byte write to the
    L1 UART's data register. Both the opening and the closing flag of a
    frame carry the same 0x7e value; reset_windows() below picks whichever
    one is nearest a given reset, which the closing flag always is."""
    match = WRITE_RE.match(line)
    if not match:
        return None
    cpu, addr, value, name = match.groups()
    if name != 'sgi-l1-uart' or int(addr, 16) & 0xff != 0:
        return None
    if int(value, 16) != 0x7e:
        return None
    return int(cpu)


def reset_windows(log):
    """One entry per machine reset: the requesting processor (the nearest
    preceding flag-byte write) and the log lines showing another processor
    entering a translation block before the reset that followed.

    A reset resets every CPU object, so more than one "cpu_reset" line can
    report one reset event; a run of consecutive lines collapses to its
    first. The board's own reset at machine start happens before either
    guest program has written anything -- a leading run with no preceding
    write is that startup reset, not one this exercise asked for, and is
    skipped rather than treated as a window."""
    lines = log.splitlines()
    resets = []
    last_reset_line = None
    for i, line in enumerate(lines):
        if RESET_RE.match(line):
            if last_reset_line is None or i != last_reset_line + 1:
                resets.append(i)
            last_reset_line = i

    windows = []
    search_from = 0
    for reset_index in resets:
        requester = None
        write_index = None
        for i in range(reset_index - 1, search_from - 1, -1):
            cpu = committing_write(lines[i])
            if cpu is not None:
                requester, write_index = cpu, i
                break
        search_from = reset_index + 1
        if requester is None:
            continue
        entered = []
        for line in lines[write_index + 1:reset_index]:
            trace = TRACE_RE.match(line)
            if trace and int(trace.group(1)) != requester:
                entered.append(line)
        windows.append((requester, entered))
    return windows


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


class SN1L1Reset(QemuSystemTest):

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
            if state[STATE_A_DONE // 8] == 1 and state[STATE_B_DONE // 8] == 1:
                return state
            time.sleep(0.1)
        self.fail(f'the machine did not run {CYCLES} resets: {state}')

    def run_resets(self, *accelerator):
        assets_root = os.environ.get('ORIGAMI_ASSETS')
        prom = resolve_asset(
            assets_root,
            os.path.join('assets', 'firmware', '6.5.30',
                         'ip35prom.payload.bin'))
        if prom is None:
            self.fail(
                'ORIGAMI_ASSETS is set but the Origin 300 stock PROM is '
                'missing at <ORIGAMI_ASSETS>/assets/firmware/6.5.30/'
                'ip35prom.payload.bin -- provision it rather than '
                'silently skipping a real check')

        self.set_machine('origin300')
        slice_a = self.scratch_file('sn1-l1-reset-a.bin')
        slice_b = self.scratch_file('sn1-l1-reset-b.bin')
        build_slice_a().write(slice_a)
        build_slice_b().write(slice_b)
        log = self.log_file('qemu-exec.log')

        self.vm.set_machine('origin300')
        self.vm.add_args(
            *accelerator, '-smp', '2', '-m', '512M', '-bios', str(prom),
            '-device',
            f'loader,file={slice_a},force-raw=on,addr={SLICE_A_LOAD:#x}',
            '-device', f'loader,addr={SLICE_A_PC:#x},cpu-num=0',
            '-device',
            f'loader,file={slice_b},force-raw=on,addr={SLICE_B_LOAD:#x}',
            '-device', f'loader,addr={SLICE_B_PC:#x},cpu-num=1',
            '-d', 'exec,trace:memory_region_ops_write,trace:cpu_reset',
            '-D', log)
        self.vm.launch()
        self.wait_for_both_slices()
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def run_console_power_cycles(self):
        assets_root = os.environ.get('ORIGAMI_ASSETS')
        prom = resolve_asset(
            assets_root,
            os.path.join('assets', 'firmware', '6.5.30',
                         'ip35prom.payload.bin'))
        if prom is None:
            self.fail(
                'ORIGAMI_ASSETS is set but the Origin 300 stock PROM is '
                'missing at <ORIGAMI_ASSETS>/assets/firmware/6.5.30/'
                'ip35prom.payload.bin -- provision it rather than '
                'silently skipping a real check')

        self.set_machine('origin300')
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
            '-d', 'exec,trace:qemu_system_shutdown_request,trace:cpu_reset',
            '-D', log)
        self.vm.launch()
        wait_for_console_pattern(self, '<CTRL_T> to escape to L1')

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
        windows = reset_windows(log)
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
        self.assertEqual(len(reset_windows(log)), CYCLES)

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
