#!/usr/bin/env python3
#
# Boots the real IP35 PROM on the two-chassis Origin 300 to its menu
#
# SPDX-License-Identifier: GPL-2.0-or-later
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

import os

import lib_origin300
from lib_prom import require_assets

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Origin300TwoChassisProm(QemuSystemTest):

    timeout = 1200

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_eight_cpus_on_two_nodes_reach_the_menu(self):
        populations = (4, 4)
        assets_root = os.environ.get('ORIGAMI_ASSETS')
        require_assets(self, *lib_origin300.ASSET_SPECS)
        machine, argv = lib_origin300.launch_args_two_chassis(
            assets_root, populations, self.workdir)

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
