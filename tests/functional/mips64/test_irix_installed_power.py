#!/usr/bin/env python3
#
# IRIX installed-disk power control: boot the installed IRIX disk on each
# SN0 board, power it off with shutdown -p or restart it with reboot, and
# require what the hardware does: QEMU exits with status 0 after the
# controllers remove power, and a reboot comes back to a login prompt
#
# SPDX-License-Identifier: BSD-3-Clause
"""IRIX 6.5.30 power-off and reboot on the Origin 200, the two-module
Origin 200 and the Origin 2000.

The tests start from `irix/6.5.30/root.qcow2`, as test_irix_installed_boot.py
does, with the same release PROM. `shutdown -y -g0 -p` makes IRIX send
`pwr d 1` to the module controllers and spin; once every module is off with
nothing left to power one up, the emulator ends. A two-module Origin 200
needs the master controller to tell its slave to power off as well, so a
master that powered only itself off would leave QEMU running with the slave
on. `reboot` goes through the Hubs' port reset and must bring the PROM back
and boot to a login prompt again.

An unset QEMU_TEST_SGI_ASSET_DIR skips the tests. If the directory is set
but the PROM or disk is missing, asset fetching fails the test.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (IRIX_ACCEL, MACHINES, drive_console, launch_paused)
from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)
from qemu_test.asset import LocalAsset
from test_irix_installed_boot import (DISK_REL, DISK_SHA256, FIRMWARE_REL,
                                      PROM_SHA256)


# Each board's processors and memory: the Origin 200 as the plain boot test
# has it, the pair with two processors per module, and the four-node
# Origin 2000 module as the diagnostics matrix boots it.
BOARDS = {
    'origin200': (MACHINES['origin200'], 2, 64),
    'origin200_dual': (MACHINES['origin200_dual'], 4, 128),
    'origin2000': ('origin2000,debug-leds=off', 8, 256),
}

# The plain boot to a root login measured about 265s on a quiet host for
# the single module; the larger boards take longer. One bound for all three
# stays well under the meson timeout registered for this file.
BOOT_TIMEOUT_SECONDS = 900

# How long QEMU may take to exit once IRIX has said it is shutting down:
# IRIX stops its services, the controller removes power a second after
# `pwr d 1`, and the emulator ends. The sync and the shutdown itself take
# tens of seconds on a loaded host; a machine that is going to stay up
# stays up, so the bound only has to be longer than the removal.
POWER_OFF_TIMEOUT_SECONDS = 150

LOGIN_PROMPT = 'console login:'
SHUTTING_DOWN = b'The system is shutting down'

LOGIN_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
)


class IrixInstalledPower(QemuSystemTest):
    """Boots the plain installed IRIX disk, logs in as root and powers the
    machine off or restarts it."""

    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    def _launch(self, board):
        machine, cpus, memory = BOARDS[board]
        prom = self.ASSET_PROM.fetch()
        disk = self.ASSET_DISK.fetch()
        vm = self.get_vm(name=f'irix_installed_power_{board}')
        vm.set_machine(machine)
        vm.set_console()
        vm.add_args(
            '-accel', IRIX_ACCEL,
            '-cpu', 'R10000', '-smp', str(cpus), '-m', str(memory),
            '-bios', str(prom),
            '-audio', 'none',
            '-drive',
            f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
            '-nic', 'user,id=net0')
        launch_paused(vm)
        return vm

    def _power_off(self, board):
        vm = self._launch(board)
        log_path = self.log_file(f'irix-power-off-{board}.log')
        steps = LOGIN_STEPS + (
            (r"[#$] ", "sync; /etc/shutdown -y -g0 -p\n"),)
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS, steps,
                reached_when=lambda t: SHUTTING_DOWN in t, vm=vm)
            if not reached:
                self.fail(
                    'IRIX never started shutting down -- console log: '
                    f'{log_path}')
            deadline = time.monotonic() + POWER_OFF_TIMEOUT_SECONDS
            while vm.exitcode() is None and time.monotonic() < deadline:
                time.sleep(1)
            code = vm.exitcode()
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        self.assertIsNotNone(
            code,
            f'IRIX shut down on the {board} but QEMU was still running '
            f'{POWER_OFF_TIMEOUT_SECONDS}s later -- the controllers did '
            f'not remove power from every module; console log: {log_path}')
        self.assertEqual(
            code, 0,
            f'QEMU exited with status {code} after IRIX powered the '
            f'{board} off -- console log: {log_path}')

    def _reboot(self, board):
        vm = self._launch(board)
        log_path = self.log_file(f'irix-reboot-{board}.log')
        # The PROM may stop at its menu again after the restart, so the
        # menu rule is listed twice: each fires once.
        steps = LOGIN_STEPS + (
            (r"[#$] ", "sync; /etc/reboot\n"),
            (r"Option\?", "1\n"),
        )
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS * 2,
                steps,
                reached_when=lambda t: t.count(LOGIN_PROMPT.encode()) >= 2,
                vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        self.assertTrue(
            reached,
            f'the {board} never came back to a login prompt after reboot '
            f'-- console log: {log_path}')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin200_shutdown_exits(self):
        self._power_off('origin200')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin200_dual_shutdown_exits(self):
        self._power_off('origin200_dual')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin2000_shutdown_exits(self):
        self._power_off('origin2000')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin200_dual_reboot_returns_to_login(self):
        self._reboot('origin200_dual')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin2000_reboot_returns_to_login(self):
        self._reboot('origin2000')


if __name__ == '__main__':
    QemuSystemTest.main()
