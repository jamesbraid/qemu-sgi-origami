#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Origin 300 L1 reset and console power-cycle checks."""

import os
import re
import struct
import time
from pathlib import Path

from lib_mips import (R_S0, R_S2, R_T0, R_T1, R_T2, R_T3, R_T4, R_T5,
                      R_ZERO, RESET_RE, TRACE_RE, WRITE_RE, MipsProgram,
                      mips_i, reset_windows)
from lib_prom import MACHINES
from lib_sn_reset import (B_COUNT_TARGET, CYCLES, STATE, STATE_A_DONE,
                          STATE_B_DONE, STATE_B_COUNT, STATE_B_MARK,
                          STATE_PHASE, STATUS_KX_KERNEL, _ResetCycleMixin)
from qemu_test import QemuSystemTest
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern
from qemu_test.asset import LocalAsset

# === L1 system reset (origin300/SN1) ========================================

# The Origin 300's own stock PROM, under QEMU_TEST_SGI_ASSET_DIR.
IP35_FIRMWARE_REL = os.path.join('firmware', '6.5.30',
                                 'ip35prom.payload.bin')

# The L1 UART's node-local registers, reached through the Hub-special
# window (attribute 0, window one), at BEDROCK_L1_UART_OFFSET.
L1_UART_DATA = 0x30000080

# The Bedrock memory-configuration register at 0x780008, reached through
# the node register window mapped at
# SN1_BEDROCK_NODE_ADDRESS in hw/mips/sgi/sn1-node.c. A -device loader
# writes to that physical address without the CPU's xkphys decode.
MD_MEMORY_CONFIG = 0x20001800000 + 0x780008

# Configure the modeled memory map used by this test: select locational
# bank two as software bank zero, enable it, and set its size code to 3.
# This test bypasses the PROM, so the fixture must configure memory before
# loading and running the two test programs.
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

# The local general-task destination and reset request used by this test.
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

    ASSET_FIRMWARE = LocalAsset(
        IP35_FIRMWARE_REL,
        '554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b')

    def wait_for_spinners(self, previous):
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            state = self.read_state()
            counters = (state[STATE_A_DONE // 8], state[STATE_B_DONE // 8])
            if all(current != old for current, old in zip(counters, previous)):
                return
            time.sleep(0.1)
        self.fail(f'console spinners did not advance from {previous}: {state}')

    def run_resets(self, *accelerator):
        prom = self.ASSET_FIRMWARE.fetch()

        self.set_machine(MACHINES['origin300'])
        slice_a = self.scratch_file('sn1-l1-reset-a.bin')
        slice_b = self.scratch_file('sn1-l1-reset-b.bin')
        build_slice_a().write(slice_a)
        build_slice_b().write(slice_b)
        log = self.log_file('qemu-exec.log')

        self.vm.set_machine(MACHINES['origin300'])
        self.vm.add_args(
            *accelerator, '-S', '-smp', '2', '-m', '512M', '-bios', str(prom),
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
        self.vm.qmp('cont')
        self.wait_for_both_slices()
        # The log is flushed when the emulator exits, so read it after.
        self.vm.shutdown()
        return Path(log).read_text(errors='replace')

    def run_console_power_cycles(self):
        prom = self.ASSET_FIRMWARE.fetch()

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
        self.wait_for_spinners((0, 0))

        for _ in range(CYCLES):
            exec_command_and_wait_for_pattern(
                self, '\x14power down', '<CTRL_T> to escape to L1')
            exec_command_and_wait_for_pattern(
                self, '\x14power down', '001c01-L1> ')
            self.vm.event_wait('SHUTDOWN')
            state = self.read_state()
            stopped = (state[STATE_A_DONE // 8], state[STATE_B_DONE // 8])
            exec_command_and_wait_for_pattern(
                self, 'power up', '<CTRL_T> to escape to L1')
            self.vm.event_wait('RESET')
            self.wait_for_spinners(stopped)

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

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_round_robin_stops_the_peer_at_the_write(self):
        """Plain single-threaded round robin, with no fixed clock: a
        regression guard, not the discriminating case below."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single'))

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_instruction_counted_stops_the_peer_at_the_write(self):
        """Under the fixed instruction-count clock -- the mode a
        reproducible run uses -- this is the case the fix in
        hw/misc/sgi-l1.c was written against."""
        self.assert_only_the_requester_ran(
            self.run_resets('-accel', 'tcg,thread=single',
                            '-icount', 'shift=0,align=off,sleep=off'))

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_multi_threaded_reset_completes(self):
        """Multi-threaded execution cannot promise the peer stopped before
        any further instruction -- it is running on its own host thread --
        so this only requires that every reset still completes and both
        slices resume."""
        log = self.run_resets('-accel', 'tcg,thread=multi')
        self.assertEqual(len(reset_windows(log, l1_committing_write)),
                         CYCLES)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
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
