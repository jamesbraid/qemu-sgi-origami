#!/usr/bin/env python3
#
# Boots the real IP35 PROM on the two-chassis Origin 300 to its menu
#
# SPDX-License-Identifier: BSD-3-Clause
"""Two Origin 300 chassis joined by one NUMAlink cable, each with its own
four processors, boot flash, identity records and L1
(topology=origin300-two-chassis). Each chassis's stock IP35 PROM runs from
its own flash; the PROMs find each other through NUMAlink discovery, the
master reports "8 CPUs on 2 nodes found" and "ALL DIAGS PASSED" and
reaches the System Maintenance Menu on its console, while the slave
chassis waits in its slave loop on its own L1, which this test leaves
unconnected.

Measured end to end against the debug build (2026-09-25, host load
about 60): 364 s for the one boot, container start included. 1200 s
keeps better than three times that.
"""

import lib_origin300

from qemu_test import QemuSystemTest
from qemu_test.asset import LocalAsset
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Origin300TwoChassisProm(QemuSystemTest):

    timeout = 1200
    ASSET_FIRMWARE = LocalAsset(
        lib_origin300.FIRMWARE_REL, lib_origin300.FIRMWARE_SHA256)
    ASSET_SPD_3 = LocalAsset(*lib_origin300.SPD_INPUTS[0][1:])
    ASSET_SPD_5 = LocalAsset(*lib_origin300.SPD_INPUTS[1][1:])

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_eight_cpus_on_two_nodes_reach_the_menu(self):
        populations = (4, 4)
        firmware = self.ASSET_FIRMWARE.fetch()
        spd_paths = (self.ASSET_SPD_3.fetch(), self.ASSET_SPD_5.fetch())
        machine, argv = lib_origin300.launch_args_two_chassis(
            firmware, spd_paths, populations, self.workdir)

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(*argv)
        self.vm.launch()
        wait_for_console_pattern(self, '8 CPUs on 2 nodes found')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        wait_for_console_pattern(self, 'System Maintenance Menu')
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
