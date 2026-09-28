#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Separate R10000 TLB remapping from ordinary data-cache storage.

Public contract: MIPS R10000 User's Manual v2.0, October 10, 1996,
pp. 48, 53-54, 162-163, 239, 280, 283 and 287-288. No firmware is required.
"""

import re
import socket
import struct
import time
from pathlib import Path

from qemu_test import QemuSystemTest


MAILBOX = 0x1000
PAGE = 0x20010000
VIRTUAL = 0x40000000
FIRST = 0x10203040
SECOND = 0x55667788
DONE = 0x43414348


def insn_i(op, rs, rt, imm):
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xffff)


def constant(reg, value):
    words = [insn_i(0x0d, 0, reg, value >> 48)]
    for shift in (32, 16, 0):
        words += [reg << 16 | reg << 11 | 16 << 6 | 0x38,
                  insn_i(0x0d, reg, reg, value >> shift)]
    return words


def mtc0(reg, cp0):
    return [0x40800000 | reg << 16 | cp0 << 11, 0, 0, 0, 0]


def mfc0(reg, cp0):
    return [0x40000000 | reg << 16 | cp0 << 11, 0, 0, 0, 0]


def mapping(paddr, cca):
    code = constant(8, VIRTUAL) + mtc0(8, 10)
    code += mtc0(0, 0) + mtc0(0, 5)
    for cp0, page in ((2, paddr), (3, paddr + 0x1000)):
        code += constant(8, (page >> 6) | (cca << 3) | 7)
        code += mtc0(8, cp0)
    return code + [0x42000002, 0, 0, 0, 0]  # tlbwi


def diagnostic_rom(mode, migration=False):
    # Keep code, stack-free working state and output outside the data cache.
    code = constant(8, 0x00410080) + mtc0(8, 12)  # BEV, DE, KX
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    code += mtc0(0, 2) + mtc0(0, 3) + mtc0(0, 5)
    code += constant(8, 63) + constant(9, 0x10000000)
    loop = len(code)
    code += mtc0(8, 0) + mtc0(9, 10)
    code += [0x42000002, 0, 0, 0, 0,
             insn_i(0x09, 9, 9, 0x2000),
             insn_i(0x09, 8, 8, -1)]
    code += [insn_i(0x01, 8, 1, loop - len(code) - 1), 0]  # bgez
    code += constant(9, 0x9000000000000000 | PAGE)
    for offset, value in ((0, FIRST), (0x1000, SECOND)):
        code += constant(8, value) + [insn_i(0x2b, 9, 8, offset)]
    code += [insn_i(0x23, 9, 8, 0), 0x0000000f]

    if mode == 'remap':
        code += mapping(PAGE, 2) + constant(10, VIRTUAL)
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0)]
        code += mapping(PAGE + 0x1000, 2)
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 4)]
    else:
        code += mapping(PAGE, 3)
        address = VIRTUAL if mode == 'mapped' else 0x9800000000000000 | PAGE
        code += constant(10, address)
        code += mtc0(0, 28) + mtc0(0, 29)
        # Invalidate both ways at the tested primary and secondary indices.
        for way in (0, 1):
            for operation in (0x09, 0x0b):
                code += [insn_i(0x2f, 10, operation, way), 0, 0, 0, 0]
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0)]
        code += constant(8, SECOND)
        code += [insn_i(0x2b, 10, 8, 0), 0x0000000f,
                 insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 4)]
        for way in (0, 1):
            code += [insn_i(0x2f, 10, 0x05, way), 0, 0, 0, 0]
            code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 8 + way * 12)]
            code += mfc0(8, 29) + [insn_i(0x2b, 12, 8, 12 + way * 12)]
            code += [insn_i(0x2f, 10, 0x19, way), 0, 0, 0, 0]
            code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 16 + way * 12)]
        # A dirty primary line must remain invisible through the RAM alias.
        code += [insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 36)]
        # Rewriting only CCA must discard the translated cache access path.
        code += mapping(PAGE, 2) + constant(11, VIRTUAL)
        code += [insn_i(0x23, 11, 8, 0), insn_i(0x2b, 12, 8, 56)]
        code += mapping(PAGE, 3)
        code += [insn_i(0x23, 11, 8, 0), insn_i(0x2b, 12, 8, 60)]
        if migration:
            # Keep one dirty L1 line and a different dirty L2 line in flight.
            code += constant(8, 0xabcdef01)
            code += [insn_i(0x2b, 10, 8, 128),
                     insn_i(0x2f, 10, 0x15, 128), 0, 0, 0, 0]
            code += constant(8, 0x52454144)
            code += [insn_i(0x2b, 12, 8, 68),
                     insn_i(0x23, 12, 8, 72),
                     insn_i(0x04, 8, 0, -2), 0]
        # Primary writeback updates secondary storage without updating RAM.
        code += [insn_i(0x2f, 10, 0x15, 0), 0, 0, 0, 0,
                 insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 40)]
        code += [insn_i(0x2f, 10, 0x07, 0), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 44)]
        code += [insn_i(0x2f, 10, 0x1b, 0), 0, 0, 0, 0]
        code += mfc0(8, 29) + [insn_i(0x2b, 12, 8, 48)]
        code += [insn_i(0x2f, 10, 0x17, 0), 0, 0, 0, 0,
                 insn_i(0x23, 9, 8, 0), insn_i(0x2b, 12, 8, 52)]
        if migration:
            code += [insn_i(0x2f, 10, 0x17, 128), 0, 0, 0, 0,
                     insn_i(0x23, 9, 8, 128), insn_i(0x2b, 12, 8, 64)]
        if mode == 'operations':
            code += constant(8, 0x20410080) + mtc0(8, 12)  # CU1
            code += constant(10, 0x9800000000000000 | (PAGE + 0x100))
            for size, offset, value in ((0x28, 0, 0x12), (0x29, 2, 0x3456),
                                       (0x2b, 4, 0x789abcde),
                                       (0x3f, 8, 0x1122334455667788)):
                code += constant(8, value) + [insn_i(size, 10, 8, offset)]
            for operation, offset, output in ((0x24, 0, 64), (0x25, 2, 68),
                                              (0x27, 4, 72)):
                code += [insn_i(operation, 10, 8, offset),
                         insn_i(0x2b, 12, 8, output)]
            code += [insn_i(0x37, 10, 8, 8), insn_i(0x3f, 12, 8, 80)]
            code += [insn_i(0x30, 10, 8, 4), insn_i(0x09, 8, 8, 1),
                     insn_i(0x38, 10, 8, 4), insn_i(0x2b, 12, 8, 88),
                     insn_i(0x23, 10, 8, 4), insn_i(0x2b, 12, 8, 92)]
            code += [insn_i(0x31, 10, 0, 4), insn_i(0x39, 10, 0, 16),
                     insn_i(0x35, 10, 2, 8), insn_i(0x3d, 10, 2, 24),
                     insn_i(0x23, 10, 8, 16), insn_i(0x2b, 12, 8, 96),
                     insn_i(0x37, 10, 8, 24), insn_i(0x3f, 12, 8, 104)]
            code += constant(8, SECOND)
            code += [insn_i(0x2a, 10, 8, 33), insn_i(0x2e, 10, 8, 36),
                     insn_i(0x23, 10, 8, 32), insn_i(0x2b, 12, 8, 112),
                     insn_i(0x23, 10, 8, 36), insn_i(0x2b, 12, 8, 116)]
            code += constant(8, 0x1122334455667788)
            code += [insn_i(0x2c, 10, 8, 41), insn_i(0x2d, 10, 8, 48),
                     insn_i(0x37, 10, 8, 40), insn_i(0x3f, 12, 8, 120),
                     insn_i(0x23, 10, 8, 48), insn_i(0x2b, 12, 8, 128)]
    code += constant(8, DONE)
    code += [0x0000000f, insn_i(0x2b, 12, 8, 32),
             insn_i(0x04, 0, 0, -1), 0]
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


def reset_rom():
    # Record reset status before enabling 64-bit address construction.
    code = mfc0(16, 12)
    code += constant(8, 0x00410080) + mtc0(8, 12)
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    code += constant(9, 0x9000000000000000 | PAGE)
    code += constant(10, 0x9800000000000000 | PAGE)
    code += [insn_i(0x23, 12, 8, 72)]
    branch = len(code)
    code += [0, 0]
    code += constant(8, 0x12345000) + mtc0(8, 18)
    code += constant(8, FIRST)
    code += [insn_i(0x2b, 9, 8, 0), insn_i(0x2b, 9, 8, 128)]
    code += constant(8, SECOND) + [insn_i(0x2b, 10, 8, 0)]
    code += constant(8, 0xabcdef01)
    code += [insn_i(0x2b, 10, 8, 128),
             insn_i(0x2f, 10, 0x15, 128), 0, 0, 0, 0]
    code += constant(8, 0x52454144)
    code += [insn_i(0x2b, 12, 8, 68), insn_i(0x04, 0, 0, -1), 0]
    code[branch] = insn_i(0x05, 8, 0, len(code) - branch - 1)
    # The soft-reset handler uses uncached instructions and CACHE only.
    for operation, offset, register, output in ((0x05, 0, 28, 0),
                                               (0x19, 0, 28, 4),
                                               (0x07, 128, 28, 8),
                                               (0x1b, 128, 29, 12)):
        code += [insn_i(0x2f, 10, operation, offset), 0, 0, 0, 0]
        code += mfc0(8, register) + [insn_i(0x2b, 12, 8, output)]
    for offset, output in ((0, 16), (128, 20)):
        code += [insn_i(0x23, 9, 8, offset), insn_i(0x2b, 12, 8, output)]
    for offset, output in ((0, 24), (128, 28)):
        code += [insn_i(0x2f, 10, 0x17, offset), 0, 0, 0, 0,
                 insn_i(0x23, 9, 8, offset), insn_i(0x2b, 12, 8, output)]
    code += [insn_i(0x2b, 12, 16, 36)]
    code += mfc0(8, 18) + [insn_i(0x2b, 12, 8, 40)]
    code += constant(8, DONE)
    code += [insn_i(0x2b, 12, 8, 32), insn_i(0x04, 0, 0, -1), 0]
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


def rom_data_rom():
    code = constant(8, 0x00410080) + mtc0(8, 12)
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    code += constant(10, 0x980000001fc00200)
    code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 0),
             insn_i(0x23, 10, 8, 4), insn_i(0x2b, 12, 8, 4)]
    # The tag/data observation proves the ROM read allocated a cache line.
    code += [insn_i(0x2f, 10, 0x05, 0), 0, 0, 0, 0]
    code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 8)]
    code += [insn_i(0x2f, 10, 0x19, 0), 0, 0, 0, 0]
    code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 12)]
    code += constant(8, DONE)
    code += [insn_i(0x2b, 12, 8, 32), insn_i(0x04, 0, 0, -1), 0]
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into('>II', image, 0x200, FIRST, SECOND)
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


def refused_rom_store_rom(romd):
    address = 0x1e000200 if romd else 0x1fc00200
    code = constant(8, 0x00410080) + mtc0(8, 12)
    code += constant(12, 0xffffffffa0000000 | MAILBOX)
    code += constant(10, 0x9800000000000000 | address)
    if romd:
        code += [insn_i(0x23, 10, 8, 0)]
    code += constant(8, SECOND) + [insn_i(0x2b, 10, 8, 0)]
    # A refused store must not allocate either primary way.
    for way in (0, 1):
        code += [insn_i(0x2f, 10, 0x05, way), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, way * 4)]
    if not romd:
        # Refuse the same store when the immutable ROM line is already cached.
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 8)]
        code += [insn_i(0x2f, 10, 0x05, 0), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 12)]
        code += constant(8, SECOND) + [insn_i(0x2b, 10, 8, 0)]
        code += [insn_i(0x2f, 10, 0x05, 0), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 16)]
        code += [insn_i(0x23, 10, 8, 0), insn_i(0x2b, 12, 8, 20)]
    # Neither refusal nor subsequent writeback may issue a device command.
    for operation in (0x15, 0x17):
        code += [insn_i(0x2f, 10, operation, 0), 0, 0, 0, 0]
    for way in (0, 1):
        code += [insn_i(0x2f, 10, 0x07, way), 0, 0, 0, 0]
        code += mfc0(8, 28) + [insn_i(0x2b, 12, 8, 24 + way * 4)]
    if romd:
        # Positive control: an uncached Read Array command invokes the callback.
        code += constant(9, 0xffffffffa0000000 | address)
        code += constant(8, 0xff) + [insn_i(0x28, 9, 8, 0)]
    code += constant(8, DONE)
    code += [insn_i(0x2b, 12, 8, 32), insn_i(0x04, 0, 0, -1), 0]
    image = bytearray(0x400 + len(code) * 4)
    struct.pack_into('>I', image, 0, insn_i(0x04, 0, 0, 0xff))
    struct.pack_into('>I', image, 0x200, FIRST)
    if romd:
        # Record the refused load's exception, then resume after that load.
        handler = mfc0(26, 13) + [insn_i(0x2b, 12, 26, 36)]
        handler += mfc0(26, 14) + [insn_i(0x09, 26, 26, 4)]
        handler += mtc0(26, 14) + [0x42000018, 0]
        assert len(handler) * 4 <= 0x80
        struct.pack_into(f'>{len(handler)}I', image, 0x380, *handler)
    struct.pack_into(f'>{len(code)}I', image, 0x400, *code)
    return image


class R10000TLBCache(QemuSystemTest):
    def run_rom(self, mode, cache=True):
        image = self.scratch_file(f'{mode}.bin')
        Path(image).write_bytes(diagnostic_rom(mode))
        self.set_machine('octane')
        cpu = 'R10000,r10k-data-cache=on' if cache else 'R10000'
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-cpu', cpu,
                         '-m', '128M', '-bios', image, '-nodefaults')
        self.vm.launch()
        deadline = time.monotonic() + 10
        while True:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /33wx {MAILBOX:#x}')
            words = [int(word, 16) for word in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            self.assertEqual(len(words), 33, output)
            if words[8] == DONE:
                break
            if time.monotonic() >= deadline:
                self.fail(f'{mode} ROM did not complete: {output}')
            time.sleep(0.05)
        self.vm.cmd('stop')
        self.log.info('%s result: %s', mode, output)
        Path(self.log_file('mailbox.txt')).write_text(output, encoding='utf-8')
        self.assertEqual(words[:2], [FIRST, SECOND], output)
        return words, output

    def check_cache(self, mode):
        words, output = self.run_rom(mode)
        ways = [words[2:5], words[5:8]]
        matches = [(lo, hi, data) for lo, hi, data in ways
                   if (lo & 0xc0) and
                   (((hi & 0xf) << 24) | (lo >> 8)) == PAGE >> 12]
        self.assertEqual(len(matches), 1,
                         f'cached store left no unique valid tag: {output}')
        self.assertEqual(matches[0][2], SECOND,
                         f'ordinary store missing from cache data: {output}')

        self.assertEqual(words[9:11], [FIRST, FIRST], output)
        self.assertEqual((words[11] >> 10) & 3, 3, output)
        self.assertEqual(words[12:14], [SECOND, SECOND], output)
        self.assertEqual(words[14:16], [FIRST, SECOND], output)

    def test_data_widths_and_llsc(self):
        words, output = self.run_rom('operations')
        self.assertEqual(words[16:19], [0x12, 0x3456, 0x789abcde], output)
        self.assertEqual(words[20:24], [0x11223344, 0x55667788,
                                      1, 0x789abcdf], output)
        self.assertEqual(words[24], 0x789abcdf, output)
        self.assertEqual(words[26:33], [0x11223344, 0x55667788,
                                      0x00556677, 0x88000000,
                                      0x00112233, 0x44556677,
                                      0x88000000], output)

    def test_dirty_cache_migration(self):
        image = self.scratch_file('migration.bin')
        rom = diagnostic_rom('mapped', migration=True)
        Path(image).write_bytes(rom)
        self.set_machine('octane')
        destination = self.get_vm('destination')
        for vm in (self.vm, destination):
            vm.add_args('-accel', 'tcg,thread=single',
                        '-cpu', 'R10000,r10k-data-cache=on',
                        '-m', '128M', '-bios', image, '-nodefaults', '-S')
        control = f'{self.socket_dir().name}/cache-control.sock'
        destination.add_args('-incoming', 'defer',
                             '-qtest', f'unix:{control},server=on,wait=off')
        self.vm.launch()
        destination.launch()
        self.vm.cmd('cont')
        deadline = time.monotonic() + 10
        while True:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /1wx {MAILBOX + 68:#x}')
            if '0x52454144' in output:
                break
            self.assertLess(time.monotonic(), deadline, output)
            time.sleep(0.05)
        self.vm.cmd('stop')
        uri = f'unix:{self.socket_dir().name}/migration.sock'
        destination.cmd('migrate-incoming', uri=uri)
        self.vm.cmd('migrate', uri=uri)
        for vm in (self.vm, destination):
            deadline = time.monotonic() + 20
            while True:
                state = vm.cmd('query-migrate')
                if state.get('status') in ('completed', 'failed'):
                    break
                self.assertLess(time.monotonic(), deadline, state)
                time.sleep(0.05)
            self.assertEqual(state['status'], 'completed', state)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.settimeout(10)
            peer.connect(control)
            peer.sendall(f'writel {MAILBOX + 72:#x} 1\n'.encode())
            self.assertEqual(peer.recv(128), b'OK\n')
        destination.cmd('cont')
        deadline = time.monotonic() + 10
        while True:
            output = destination.cmd('human-monitor-command',
                                     command_line=f'xp /17wx {MAILBOX:#x}')
            words = [int(word, 16) for word in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            if len(words) == 17 and words[8] == DONE:
                break
            self.assertLess(time.monotonic(), deadline, output)
            time.sleep(0.05)
        self.assertEqual(words[12:14], [SECOND, SECOND], output)
        self.assertEqual(words[16], 0xabcdef01, output)

    def launch_reset_rom(self):
        image = self.scratch_file('reset.bin')
        Path(image).write_bytes(reset_rom())
        self.set_machine('octane')
        control = f'{self.socket_dir().name}/reset-control.sock'
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-cpu', 'R10000,r10k-data-cache=on',
                         '-m', '128M', '-bios', image, '-nodefaults',
                         '-qtest', f'unix:{control},server=on,wait=off')
        self.vm.launch()
        deadline = time.monotonic() + 10
        while True:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /1wx {MAILBOX + 68:#x}')
            if '0x52454144' in output:
                break
            self.assertLess(time.monotonic(), deadline, output)
            time.sleep(0.05)
        self.vm.cmd('stop')
        return control

    def read_reset_result(self):
        self.vm.cmd('cont')
        deadline = time.monotonic() + 10
        while True:
            output = self.vm.cmd('human-monitor-command',
                                 command_line=f'xp /11wx {MAILBOX:#x}')
            words = [int(word, 16) for word in
                     re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
            if len(words) == 11 and words[8] == DONE:
                break
            self.assertLess(time.monotonic(), deadline, output)
            time.sleep(0.05)
        self.vm.cmd('stop')
        Path(self.log_file('reset-mailbox.txt')).write_text(output)
        return words

    def check_reset(self, soft):
        control = self.launch_reset_rom()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.settimeout(10)
            peer.connect(control)
            if not soft:
                self.vm.cmd('system_reset')
                self.vm.event_wait('RESET', timeout=10)
            # Octane cold reset clears the low-memory command mailbox.
            peer.sendall(f'writel {MAILBOX + 72:#x} 1\n'.encode())
            self.assertEqual(peer.recv(128), b'OK\n')
            if soft:
                cpu = self.vm.cmd('query-cpus-fast')[0]['qom-path']
                for level in (1, 0):
                    peer.sendall(f'set_irq_in {cpu} soft-reset 0 '
                                 f'{level}\n'.encode())
                    self.assertEqual(peer.recv(128), b'OK\n')
        words = self.read_reset_result()
        self.assertEqual(words[4:6], [FIRST, FIRST])
        self.assertEqual(bool(words[9] & (1 << 20)), soft)
        self.assertEqual(words[10], 0x12345000 if soft else 0)
        if soft:
            self.assertEqual((words[0] >> 6) & 3, 3)
            self.assertEqual((words[2] >> 10) & 3, 3)
            self.assertEqual([words[1], words[3]], [SECOND, 0xabcdef01])
            self.assertEqual(words[6:8], [SECOND, 0xabcdef01])
        else:
            self.assertEqual(words[:4], [0, 0, 0, 0])
            self.assertEqual(words[6:8], [FIRST, FIRST])

    def test_soft_reset_preserves_dirty_caches(self):
        self.check_reset(soft=True)

    def test_cold_reset_clears_dirty_caches(self):
        self.check_reset(soft=False)

    def test_cached_rom_data_read(self):
        image = self.scratch_file('rom-data.bin')
        Path(image).write_bytes(rom_data_rom())
        self.set_machine('malta')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-cpu', 'R10000,r10k-data-cache=on',
                         '-m', '128M', '-bios', image, '-nodefaults')
        self.vm.launch()
        words = self.read_reset_result()
        self.assertEqual(words[:2], [FIRST, SECOND])
        self.assertEqual((words[2] >> 6) & 3, 2)
        self.assertEqual(words[2] >> 8, 0x1fc00)
        self.assertEqual(words[3], FIRST)

    def check_refused_rom_store(self, romd):
        image = self.scratch_file('refused-rom-store.bin')
        Path(image).write_bytes(refused_rom_store_rom(romd))
        trace = self.log_file('flash.trace')
        self.set_machine('malta')
        self.vm.add_args('-accel', 'tcg,thread=single',
                         '-cpu', 'R10000,r10k-data-cache=on',
                         '-m', '128M', '-bios', image, '-nodefaults',
                         '-d', 'unimp',
                         '-trace', f'enable=pflash_io_write,file={trace}')
        self.vm.launch()
        words = self.read_reset_result()
        self.vm.shutdown()
        self.assertEqual(words[:2], [0, 0])
        self.assertIn('experimental data cache: unsupported write',
                      Path(trace).read_text())
        callbacks = [line for line in Path(trace).read_text().splitlines()
                     if 'pflash_io_write' in line]
        if romd:
            self.assertEqual(words[9] & 0x7c, 7 << 2)
            self.assertIn('experimental data cache: unsupported read',
                          Path(trace).read_text())
            self.assertEqual(words[6:8], [0, 0])
            self.assertEqual(len(callbacks), 1, callbacks)
            self.assertIn('value:0x00ff', callbacks[0])
        else:
            self.assertEqual(words[2], FIRST)
            self.assertEqual(words[3], words[4])
            self.assertEqual((words[4] >> 6) & 3, 2)
            self.assertEqual(words[5], FIRST)
            self.assertEqual((words[6] >> 10) & 3, 0)
            self.assertEqual(callbacks, [])

    def test_cached_plain_rom_store_refused(self):
        self.check_refused_rom_store(romd=False)

    def test_cached_romd_store_has_no_device_callback(self):
        self.check_refused_rom_store(romd=True)

    def test_default_cpu_bypasses_data_cache(self):
        words, output = self.run_rom('mapped', cache=False)
        self.assertEqual(words[2:8], [0] * 6, output)
        self.assertEqual(words[9:11], [SECOND, SECOND], output)
        self.assertEqual(words[14:16], [SECOND, SECOND], output)

    def test_uncached_tlb_remap(self):
        self.run_rom('remap')

    def test_mapped_cached_store(self):
        self.check_cache('mapped')

    def test_unmapped_cached_store(self):
        self.check_cache('unmapped')


if __name__ == '__main__':
    QemuSystemTest.main()
