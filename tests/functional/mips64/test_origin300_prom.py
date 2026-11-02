#!/usr/bin/env python3
#
# Boots the real IP35 PROM on the Origin 300 to its first console line
#
# SPDX-License-Identifier: BSD-3-Clause
"""origin300 takes only a raw stock PROM image as -bios and refuses
-kernel outright (hw/mips/sgi/sn1.c, sn1_load_prom); there is no
synthesized-firmware bypass the way origin200 has, so the real image is
the only way to prove the stock boot path runs at all.

Booting with -bios alone, no pflash part behind the boot flash, gives
the guest no way to persist a write across the reset the L1/PROM
handoff performs early in its own sequence (sn1_flash_reject_write in
hw/mips/sgi/sn1.c logs and drops every such write): a real run reaches
the L1's own power-up banner, hands the console to the CPU
("entering console mode ... <CTRL_T> to escape to L1"), the IP35 PROM
prints its first line ("Starting PROM Boot process"), and the whole
sequence then repeats rather than reaching an interactive prompt.
Measured directly (2026-09-16, qemu-system-mips64 built from this
branch, -M origin300 -smp 2 -m 512M -bios ip35prom.payload.bin,
-serial stdio): "Starting PROM Boot process" appears within about a
second, every time, and the four lines above repeat verbatim for as
long as the machine keeps running. This test only requires reaching
that first line -- proof the real image executed through the stock
boot path -- not the full menu a battery-backed board would reach.

test_origin300_prom_four_cpus repeats the same check at -smp 4. Measured
directly (2026-09-18, this branch): the same first line appears within
about a second with or without writable pflash. The menu case below
uses writable pflash and checks diagnostics separately.

test_four_cpus_reach_the_menu_with_all_diags_passed goes past that
first line, through a writable boot flash and the machine's own
synthetic node and IO8 identity records, to the stock PROM's "ALL DIAGS
PASSED" and System Maintenance Menu.
test_irix_origin300_installed_boot.py checks that same outcome
for two CPUs while continuing through an installed IRIX boot.

test_starved_processor_is_still_counted freezes one processor's host thread
for 5 seconds, longer than the model's 2 second park bound, while the PROM
brings the processors up, as a heavily loaded host can a thread it cannot
schedule. The stock PROM must still find all four processors. Before the parks counted
the waited-for processors' own CPU time instead of host time, a thread frozen
for that long ran every park out at its bound, the join was abandoned, and
the PROM reported the processor dead ("Other CPU on this node died") or
started with fewer.

test_reset_keeps_the_nvram_owner resets the machine at that menu and
requires the second boot to accept its NVRAM. The PROM compares a board
number it derives from the node board area's serial with the one it saved,
and with the old all-zero serial "000000" every reset printed "This NVRAM
did not belong to this system" and reinitialized it. The marked serial
"EMUN00" boots the second time with "Checking hardware inventory" ending
in DONE.
"""

import ctypes
import os
import sys
import time

import lib_origin300
from lib_prom import launch_paused

from qemu_test import QemuSystemTest
from qemu_test.asset import LocalAsset
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


PTRACE_SEIZE = 0x4206
PTRACE_INTERRUPT = 0x4207
PTRACE_DETACH = 17
WALL = 0x40000000


def freeze_vcpu_thread(pid, cpu_index):
    """Stop one vCPU host thread. Returns (thread id, libc handle) for the
    detach, or None when the thread is not found or ptrace is refused.

    ptrace stops that thread alone; SIGSTOP would stop the whole process,
    and the other processors must keep running. The thread may be stopped
    while it holds the big QEMU lock, which stalls the whole machine, so
    this is at least as strong as a thread the host merely fails to
    schedule."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.ptrace.restype = ctypes.c_long
    libc.ptrace.argtypes = [ctypes.c_long, ctypes.c_long, ctypes.c_void_p,
                            ctypes.c_void_p]
    for entry in os.listdir(f'/proc/{pid}/task'):
        with open(f'/proc/{pid}/task/{entry}/comm') as comm:
            if comm.read().strip() != f'CPU {cpu_index}/TCG':
                continue
        tid = int(entry)
        if libc.ptrace(PTRACE_SEIZE, tid, None, None) != 0:
            return None
        libc.ptrace(PTRACE_INTERRUPT, tid, None, None)
        os.waitpid(tid, WALL)
        return tid, libc
    return None


class Origin300Prom(QemuSystemTest):

    ASSET_FIRMWARE = LocalAsset(
        lib_origin300.FIRMWARE_REL, lib_origin300.FIRMWARE_SHA256)

    def boot_to_first_line(self, smp):
        prom = self.ASSET_FIRMWARE.fetch()

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.add_args('-smp', str(smp), '-m', '512M', '-bios', str(prom))
        launch_paused(self.vm)
        wait_for_console_pattern(self, 'Starting PROM Boot process')
        self.vm.shutdown()

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin300_prom(self):
        self.boot_to_first_line(2)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin300_prom_four_cpus(self):
        self.boot_to_first_line(4)


class Origin300PromMenu(QemuSystemTest):

    ASSET_FIRMWARE = LocalAsset(
        lib_origin300.FIRMWARE_REL, lib_origin300.FIRMWARE_SHA256)

    def boot_to_menu(self, cpus):
        firmware = self.ASSET_FIRMWARE.fetch()
        machine, argv = lib_origin300.launch_args(
            firmware, cpus, self.workdir)

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(*argv)
        launch_paused(self.vm)
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.shutdown()

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_four_cpus_reach_the_menu_with_all_diags_passed(self):
        self.boot_to_menu(4)

    def boot_with_starved_processor(self, cpu_index, seconds):
        firmware = self.ASSET_FIRMWARE.fetch()
        machine, argv = lib_origin300.launch_args(firmware, 4, self.workdir)

        if not sys.platform.startswith('linux'):
            self.skipTest('needs Linux /proc and ptrace')
        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(*argv)
        self.vm.add_args('-S')
        self.vm.launch()
        frozen = freeze_vcpu_thread(self.vm.get_pid(), cpu_index)
        if frozen is None:
            self.vm.shutdown()
            self.skipTest('cannot ptrace the vCPU thread on this host')
        tid, libc = frozen
        self.vm.cmd('cont')
        time.sleep(seconds)
        libc.ptrace(PTRACE_DETACH, tid, None, None)
        wait_for_console_pattern(
            self, '4 CPUs on 1 nodes found',
            failure_message='Other CPU on this node died')
        wait_for_console_pattern(
            self, 'ALL DIAGS PASSED',
            failure_message='Other CPU on this node died')
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.shutdown()

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_starved_processor_is_still_counted(self):
        # The parks start when the first other processor reaches the reset
        # barrier, some time after 'cont'; five seconds leaves the freeze
        # well past the 2 s bound however long that takes. With the bound
        # counted in virtual time every park ran out in this window.
        self.boot_with_starved_processor(2, 5.0)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_reset_keeps_the_nvram_owner(self):
        firmware = self.ASSET_FIRMWARE.fetch()
        machine, argv = lib_origin300.launch_args(firmware, 2, self.workdir)

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(*argv)
        launch_paused(self.vm)
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.cmd('system_reset')
        wait_for_console_pattern(
            self, 'Checking hardware inventory ...............'
            '              DONE',
            failure_message='did not belong to this system')
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
