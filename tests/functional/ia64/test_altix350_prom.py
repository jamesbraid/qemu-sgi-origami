#!/usr/bin/env python3
#
# Boots the stock Altix 350 PROM far enough to prove each tip runs it.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the stock Altix 350 PROM to its EFI shell.

ALTIX350_PROM names the PROM image. Without it the test is skipped, and
the image is never part of the tree, so CI does not run it. The PROM prints
the SAL banner, tests memory, then prints "Switching to RAM and testing
CPU", which ends in DONE only once the PAL's CPU self-test has passed, and
goes on to the EFI shell prompt. The interval-timer check inside that
self-test runs on QEMU's virtual clock, which without -icount follows host
time, so the machine page says how the model arms the match for it.
"""

import os

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Altix350Prom(QemuSystemTest):

    timeout = 540

    @skipIfMissingEnv('ALTIX350_PROM')
    def test_prom_reaches_the_efi_shell(self):
        self.set_machine('altix350')
        self.vm.set_console()
        self.vm.add_args('-m', '2G', '-bios', os.environ['ALTIX350_PROM'],
                         '-display', 'none', '-no-reboot')
        self.vm.launch()
        wait_for_console_pattern(self, 'SGI SAL Version 5.04')
        wait_for_console_pattern(self, 'Testing memory')
        wait_for_console_pattern(self, 'Switching to RAM and testing CPU')
        wait_for_console_pattern(self, 'DONE', 'FAILED')
        wait_for_console_pattern(self, 'Shell>')


if __name__ == '__main__':
    QemuSystemTest.main()
