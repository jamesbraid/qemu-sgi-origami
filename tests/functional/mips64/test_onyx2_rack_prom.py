#!/usr/bin/env python3
#
# Boots the real IP27 PROM on the Onyx2 rack and checks the slots it
# names
#
# SPDX-License-Identifier: BSD-3-Clause
"""Boot the IP27 PROM with the rack topology and check its `hinv -v`
output for the expected node, BaseIO, and graphics entries. This verifies
the emulated configuration and PROM interaction.

This test runs under QEMU's functional-test framework.
"""

from lib_prom import FIRMWARE_REL

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test.asset import LocalAsset


PROM_SHA256 = 'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd'


class Onyx2RackProm(QemuSystemTest):

    timeout = 900
    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_hinv_names_the_rack_slots(self):
        prom = self.ASSET_PROM.fetch()

        self.set_machine('onyx2')
        self.vm.set_console()
        self.vm.set_machine('onyx2,debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=multi', '-smp', '8',
                         '-m', '256M', '-bios', str(prom),
                         # Start paused: the PROM can reset the machine
                         # before the QMP handshake finishes.
                         '-S')
        self.vm.launch()
        self.vm.cmd('cont')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED',
                                 failure_message='Exception')
        exec_command_and_wait_for_pattern(self, '5', 'Command Monitor.')
        out = exec_command_and_wait_for_pattern(
            self, 'hinv -v', 'KONA Graphics Board, Module 1, Slot io3')
        text = out.decode(errors='replace')
        self.assertIn('IP27 Node Board, Module 1, Slot n4', text)
        self.assertIn('BASEIO IO Board, Module 1, Slot io1', text)
        self.assertIn('ASIC BRIDGE Rev 4, (widget 8)', text)
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
