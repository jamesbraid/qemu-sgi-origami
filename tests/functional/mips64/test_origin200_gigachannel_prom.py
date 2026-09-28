#!/usr/bin/env python3
#
# Boots the real IP27 PROM on an Origin 200 with a GIGAchannel expansion
# cabinet, and on the same converted module in loopback
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""On QEMU's origin200-gigachannel topology, the stock IP27 PROM 6.156
prints the XBOX load line and loads the supplied BASEIO PROM image from
the cabinet flash. The test checks the flash build's BASEIO PROM banner
and passing diagnostics. Without a flash image, it checks the XBOX load
line, a bus error in the flash window, a retry from the internal copy,
and passing diagnostics. On the loopback topology, it checks the plain
load line and passing diagnostics.
"""

import os

from lib_prom import FIRMWARE_REL

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern
from qemu_test.asset import LocalAsset

# The BASEIO PROM shipped beside the node PROM, and the cabinet flash part
# it fills: 1 MiB, erased (0xff) past the image.
BASEIO_FLASH_REL = (os.path.join('firmware', '6.5.30',
                                 'io6prom.img'),
                    '024d2f28b526933e8f00d964fefdfcd9'
                    'b0e873d504a43d9307099579e2b59ae9')
BASEIO_FLASH_SIZE = 1024 * 1024
PROM_SHA256 = (
    'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd')


class _Origin200GigachannelProm:

    def launch(self, topology, smp, mem, *extra):
        prom = self.ASSET_FIRMWARE.fetch()

        self.set_machine('origin200')
        self.vm.set_console()
        self.vm.set_machine(f'origin200,topology={topology},debug-leds=off')
        self.vm.add_args('-accel', 'tcg,thread=multi', '-smp', str(smp),
                         '-m', mem, '-bios', str(prom), *extra)
        self.vm.launch()


class Origin200GigachannelPromFlash(_Origin200GigachannelProm,
                                   QemuSystemTest):

    timeout = 900
    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_IO6_PROM = LocalAsset(*BASEIO_FLASH_REL)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_cabinet_loads_the_flash_image(self):
        image = self.ASSET_IO6_PROM.fetch()
        flash = self.scratch_file('gigachannel-flash.raw')
        with open(image, 'rb') as source:
            contents = source.read()
        with open(flash, 'wb') as part:
            part.write(contents)
            part.write(b'\xff' * (BASEIO_FLASH_SIZE - len(contents)))

        self.launch('origin200-gigachannel', 2, '128M', '-drive',
                    f'if=none,id=sgi-gigachannel-flash0,file={flash},'
                    'format=raw')
        wait_for_console_pattern(
            self, 'Loading BASEIO prom from XBOX Flashprom....\t\tDONE',
            failure_message='Bus error')
        wait_for_console_pattern(
            self, 'BASEIO PROM Monitor SGI Version 6.156  built 11:26:28',
            failure_message='Retrying')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        self.vm.shutdown()


class Origin200GigachannelProm(_Origin200GigachannelProm,
                              QemuSystemTest):

    timeout = 900
    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_cabinet_takes_the_xbox_path(self):
        self.launch('origin200-gigachannel', 2, '128M')
        wait_for_console_pattern(
            self, 'Loading BASEIO prom from XBOX Flashprom',
            failure_message='resulted in an exception')
        wait_for_console_pattern(
            self, 'Retrying using the IP27 PROM internal copy of BASEIO PROM')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        self.vm.shutdown()

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_loopback_takes_the_plain_path(self):
        self.launch('origin200-gigachannel-loopback', 1, '64M')
        wait_for_console_pattern(self, 'Loading BASEIO prom ....',
                                 failure_message='XBOX')
        wait_for_console_pattern(self, 'ALL DIAGS PASSED')
        self.vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
