#!/usr/bin/env python3
#
# Boots the real IP27 PROM on an Origin 200 with a GIGAchannel expansion
# cabinet, and on the same converted module in loopback
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""The stock IP27 PROM 6.156 treats an Origin 200 node as a GIGAchannel
cabinet's host when widget 0 answers a Crossbow's part through its Hub,
and then loads its BASEIO PROM from the cabinet Bridge's external flash
(docs/datasheets/sgi-gigachannel.md in the Origami repository). No flash
part is fitted in the model, since neither the part nor its image is
established, so the PROM's own fallback is the expected path: the XBOX
load line, a bus error in the flash window, a retry from its internal
copy, and then its diagnostics passing. The loopback form has no Crossbow
at widget 0, so the PROM takes the plain load line instead.

The harness's own launches of these topologies reached the menu in under
four minutes on a heavily loaded host (2026-09-25, load average
about 125), well inside the 900 s budget per boot.
"""

import os

from lib_prom import FIRMWARE_REL, require_assets

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Origin200GigachannelProm(QemuSystemTest):

    timeout = 900

    def launch(self, topology, smp, mem):
        prom = require_assets(self, FIRMWARE_REL)[FIRMWARE_REL]

        self.set_machine('origin200')
        self.vm.set_console()
        self.vm.set_machine(f'origin200,topology={topology},debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=multi', '-smp', str(smp),
                         '-m', mem, '-bios', str(prom))
        self.vm.launch()

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_cabinet_takes_the_xbox_path(self):
        self.launch('origin200-gigachannel', 2, '128M')
        wait_for_console_pattern(
            self, 'Loading BASEIO prom from XBOX Flashprom',
            failure_message='resulted in an exception')
        wait_for_console_pattern(
            self, 'Retrying using the IP27 PROM internal copy of BASEIO PROM')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        self.vm.shutdown()

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_loopback_takes_the_plain_path(self):
        self.launch('origin200-gigachannel-loopback', 1, '64M')
        wait_for_console_pattern(self, 'Loading BASEIO prom ....',
                                 failure_message='XBOX')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
