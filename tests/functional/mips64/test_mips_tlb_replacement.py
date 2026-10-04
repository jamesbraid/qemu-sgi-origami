#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check both halves of a mapped TLB entry after each indexed replacement.

The ROMs are the mips_tlb_performance workloads built with a few
iterations, so a stale translation left behind by a TLB flush fails here
without the benchmark's run time.
"""

from qemu_test import QemuSystemTest
from qemu_test.config import BUILD_DIR


class MipsTLBReplacement(QemuSystemTest):
    timeout = 60

    def run_rom(self, cpu, name):
        self.require_accelerator('tcg')
        image = BUILD_DIR / 'tests/functional/mips64/roms' / (
            'tlbmaint-' + name + '-check.bin')
        self.assertTrue(image.is_file(), f'Missing source-built ROM: {image}')
        self.set_machine('origin200' if cpu == 'R10000' else 'malta')
        self.vm.set_qmp_monitor(False)
        self.vm.add_args('-accel', 'tcg', '-cpu', cpu, '-m', '128M',
                         '-nodefaults', '-display', 'none', '-no-reboot',
                         '-semihosting', '-bios', str(image))
        self.vm.launch()
        self.vm.wait(timeout=self.timeout)
        output = self.vm.get_log() or ''
        self.assertEqual(self.vm.exitcode(), 0, output)
        self.assertIn('PASS tlbmaint', output.splitlines(), output)

    def test_r10000_4k(self):
        self.run_rom('R10000', '4k')

    def test_r10000_16k(self):
        self.run_rom('R10000', '16k')

    def test_r10000_64k(self):
        self.run_rom('R10000', '64k')

    def test_r10000_16m(self):
        self.run_rom('R10000', '16m')

    def test_r4000_4k(self):
        self.run_rom('R4000', '4k')

    def test_r4000_16k(self):
        self.run_rom('R4000', '16k')

    def test_r4000_64k(self):
        self.run_rom('R4000', '64k')

    def test_r4000_16m(self):
        self.run_rom('R4000', '16m')


if __name__ == '__main__':
    QemuSystemTest.main()
