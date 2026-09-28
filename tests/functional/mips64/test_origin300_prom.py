#!/usr/bin/env python3
#
# Boots the real IP35 PROM on the Origin 300 to its first console line
#
# SPDX-License-Identifier: GPL-2.0-or-later
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
first line, through a writable boot flash and synthetic
chassis/board/SPD identity records, to the stock PROM's "ALL DIAGS
PASSED" and System Maintenance Menu.
test_irix_origin300_installed_boot.py checks that same outcome
for two CPUs while continuing through an installed IRIX boot.
"""

import lib_origin300

from qemu_test import QemuSystemTest
from qemu_test.asset import LocalAsset
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Origin300Prom(QemuSystemTest):

    ASSET_FIRMWARE = LocalAsset(
        lib_origin300.FIRMWARE_REL, lib_origin300.FIRMWARE_SHA256)

    def boot_to_first_line(self, smp):
        prom = self.ASSET_FIRMWARE.fetch()

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.add_args('-smp', str(smp), '-m', '512M', '-bios', str(prom))
        self.vm.launch()
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
    ASSET_SPD_3 = LocalAsset(*lib_origin300.SPD_INPUTS[0][1:])
    ASSET_SPD_5 = LocalAsset(*lib_origin300.SPD_INPUTS[1][1:])

    def boot_to_menu(self, cpus):
        firmware = self.ASSET_FIRMWARE.fetch()
        spd_paths = (self.ASSET_SPD_3.fetch(), self.ASSET_SPD_5.fetch())
        machine, argv = lib_origin300.launch_args(
            firmware, spd_paths, cpus, self.workdir)

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(*argv)
        self.vm.launch()
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.shutdown()

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_four_cpus_reach_the_menu_with_all_diags_passed(self):
        self.boot_to_menu(4)


if __name__ == '__main__':
    QemuSystemTest.main()
