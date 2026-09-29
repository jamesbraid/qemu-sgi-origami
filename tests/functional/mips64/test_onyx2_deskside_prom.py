#!/usr/bin/env python3
#
# Boots the real IP27 PROM on the Onyx2 deskside and checks the slots it
# names
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Onyx2 deskside's midplane drives Hub slot bits 3 and 2, which the
stock IP27 PROM 6.156 reads as the deskside's own n1 and n2 codes and
answers with that chassis's own Crossbow table (docs/datasheets/onyx2.md
in the Origami repository). A real deskside's PROM hinv -v names node
boards n2 (NASID 0) and n1, the BaseIO in io1 at widget 15 and the KONA
board in io4; this checks the model's does the same.

The harness's own launch reached the menu in about two minutes on
2026-09-27.
"""

from lib_prom import FIRMWARE_REL

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test.asset import LocalAsset


PROM_SHA256 = 'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd'


class Onyx2DesksideProm(QemuSystemTest):

    timeout = 900
    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_hinv_names_the_deskside_slots(self):
        prom = self.ASSET_PROM.fetch()

        self.set_machine('onyx2')
        self.vm.set_console()
        self.vm.set_machine('onyx2,topology=onyx2-deskside,debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=multi', '-smp', '4',
                         '-m', '128M', '-bios', str(prom),
                         # Start paused: the PROM can reset the machine
                         # before the QMP handshake finishes.
                         '-S')
        self.vm.launch()
        self.vm.cmd('cont')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED',
                                 failure_message='Exception')
        exec_command_and_wait_for_pattern(self, '5', 'Command Monitor.')
        out = exec_command_and_wait_for_pattern(
            self, 'hinv -v', 'KONA Graphics Board, Module 1, Slot io4')
        text = out.decode(errors='replace')
        self.assertIn('IP27 Node Board, Module 1, Slot n2', text)
        self.assertIn('IP27 Node Board, Module 1, Slot n1', text)
        self.assertNotIn('Slot n3', text)
        self.assertIn('BASEIO IO Board, Module 1, Slot io1', text)
        self.assertIn('ASIC BRIDGE Rev 4, (widget 15)', text)
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
