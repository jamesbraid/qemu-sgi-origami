#!/usr/bin/env python3
#
# Boots the real IP27 PROM on the Onyx2 rack and checks the slots it
# names
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Onyx2 rack's compute module is the Origin 2000 eight-processor
module, so the stock IP27 PROM 6.156 names its slots exactly as an
Origin 2000's: nodes n1 to n4, the BaseIO-G in io1 at widget 8, and the
pipe under the slot its Xtown occupies, io3, where the real rack capture
has it (docs/datasheets/onyx2.md in the Origami repository).

The harness's own launch reached the menu in about two minutes on
2026-09-27.
"""

from lib_prom import FIRMWARE_REL, require_assets

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern
from qemu_test import exec_command_and_wait_for_pattern


class Onyx2RackProm(QemuSystemTest):

    timeout = 900

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_hinv_names_the_rack_slots(self):
        prom = require_assets(self, FIRMWARE_REL)[FIRMWARE_REL]

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
