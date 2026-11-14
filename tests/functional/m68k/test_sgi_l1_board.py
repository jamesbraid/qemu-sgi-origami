#!/usr/bin/env python3
#
# Boot the stock Origin 3000 C-, I- and R-brick L1 controller firmware on the
# sgi-l1-board machine, with no node attached, to its operator console.
#
# The firmware is not in the QEMU tree. The test finds it under
# QEMU_TEST_SGI_ASSET_DIR (firmware/6.5.30/l1.bin), checks its digest and the
# digest of the flash image cut from it, and is skipped, not passed, when the
# environment variable or the file is missing.
#
# The console strings matched below are what the firmware printed in a run on
# this model. No published document quotes them, so they are run output that
# shows the firmware starts here, not a source for the manual page.
#
# SPDX-License-Identifier: BSD-3-Clause

import hashlib
import os

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test import exec_command_and_wait_for_pattern
from qemu_test import wait_for_console_pattern
from qemu_test.asset import LocalAsset

COMPOSITE_REL = os.path.join('firmware', '6.5.30', 'l1.bin')
COMPOSITE_SHA256 = (
    'c355c542554a41b23b69dca4c201c566e8352e8f9fb870d2365c74cdd8ab0c0f')

# The Base 1MB image range of the composite: its body is the flash contents,
# loaded at 0xffe00000 on the boot chip select, and starts after the range's
# own header and the composite directory.
BODY_OFFSET = 0x68
BODY_LENGTH = 0xefd68
BODY_SHA256 = (
    '3c8a7aa93eb38502289cf0c2921ea91e6800228005bd8abd67425ce615f7b364')


class SgiL1Board(QemuSystemTest):

    ASSET_COMPOSITE = LocalAsset(COMPOSITE_REL, COMPOSITE_SHA256)

    def flash_image(self):
        path = self.ASSET_COMPOSITE.local_file()
        if path is None or not path.is_file():
            self.skipTest(f'the L1 firmware is not at {path}')
        # The asset helper checks the digest of the whole composite.
        composite = self.ASSET_COMPOSITE.fetch()
        with open(composite, 'rb') as src:
            body = src.read()[BODY_OFFSET:BODY_OFFSET + BODY_LENGTH]
        self.assertEqual(len(body), BODY_LENGTH)
        self.assertEqual(hashlib.sha256(body).hexdigest(), BODY_SHA256)
        flash = os.path.join(self.workdir, 'l1-flash.bin')
        with open(flash, 'wb') as out:
            out.write(body)
        return flash

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_l1_board_console(self):
        flash = self.flash_image()
        log = os.path.join(self.workdir, 'l1-board.log')

        self.set_machine('sgi-l1-board')
        self.vm.set_console()
        self.vm.add_args('-bios', flash, '-d', 'unimp', '-D', log)
        self.vm.launch()

        # With no node, no identity EEPROM and no two-wire bus peers, the
        # firmware reports what it cannot read and still reaches its console.
        wait_for_console_pattern(self, 'Error probing BOARD 0 EEPROM')
        wait_for_console_pattern(self, 'SGI SN1 L1 Controller')
        wait_for_console_pattern(self, 'Firmware Image B: Rev. 1.44.0')
        wait_for_console_pattern(self, 'L1>')
        exec_command_and_wait_for_pattern(self, 'version',
                                          'L1 1.44.0 (Image B)')
        self.vm.shutdown()

        # The parts the board does not model were logged, not answered.
        with open(log, encoding='utf-8') as logfile:
            text = logfile.read()
        self.assertIn('read of MBSR (MBAR+0x1ec): not modelled', text)
        self.assertIn('write of MBDR (MBAR+0x1f0)', text)


if __name__ == '__main__':
    QemuSystemTest.main()
