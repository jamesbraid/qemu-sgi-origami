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

test_origin300_prom_four_cpus repeats the same check at -smp 4, the
population the origin300 catalogue's supported_cpu_populations gained
beside [2] (docs/machines/layers/products/origin300.yaml). Measured
directly (2026-09-18, this branch): the same first line appears within
about a second, every time, whether or not a writable pflash backs the
boot flash. A writable-pflash run that continues past that line into
the PROM's own diagnostics is not exercised here: it surfaces a
register-reset-value miscompare in the XBridge/IOC3 self-test
(bridge_sanity, io_config_space) and, separately, an unreliable "Hub
chip failed interrupt test" outcome whose failure shape changes between
otherwise-identical runs -- a timing-sensitive four-processor
rendezvous question of its own, tracked apart from this smoke check
(see the known two-processor MTTCG rendezvous issue this shares a
family with), not a defect this test's narrow first-line bar claims to
rule out.
"""

import os

from lib_prom import resolve_asset

from qemu_test import QemuSystemTest
from qemu_test import skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class Origin300Prom(QemuSystemTest):

    def boot_to_first_line(self, smp):
        assets_root = os.environ.get('ORIGAMI_ASSETS')
        prom = resolve_asset(
            assets_root,
            os.path.join('assets', 'firmware', '6.5.30',
                         'ip35prom.payload.bin'))
        if prom is None:
            # skipIfMissingEnv above already skipped the case where
            # ORIGAMI_ASSETS is unset entirely -- getting here means it
            # IS set, so a missing PROM image is a provisioning
            # failure, not an absent-assets developer run, and must
            # fail loudly rather than silently skip a claimed
            # capability.
            self.fail(
                'ORIGAMI_ASSETS is set but the Origin 300 stock PROM is '
                'missing at <ORIGAMI_ASSETS>/assets/firmware/6.5.30/'
                'ip35prom.payload.bin -- provision it rather than '
                'silently skipping a real check')

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.add_args('-smp', str(smp), '-m', '512M', '-bios', str(prom))
        self.vm.launch()
        wait_for_console_pattern(self, 'Starting PROM Boot process')
        self.vm.shutdown()

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_origin300_prom(self):
        self.boot_to_first_line(2)

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_origin300_prom_four_cpus(self):
        self.boot_to_first_line(4)


if __name__ == '__main__':
    QemuSystemTest.main()
