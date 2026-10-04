#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Source-built TLB replacement workloads without proprietary firmware."""

import json
import os
import time
from pathlib import Path

from qemu_test import QemuSystemTest
from qemu_test.config import BUILD_DIR


WORKLOADS = {
    '4k': (4096, 65536),
    '16k': (16384, 65536),
    '64k': (65536, 8192),
    '16m': (16777216, 128),
}


class MipsTLBPerformance(QemuSystemTest):
    timeout = 180

    def run_rom(self, cpu, name):
        self.require_accelerator('tcg')
        image = BUILD_DIR / 'tests/functional/mips64/roms' / (
            'tlbmaint-' + name + '.bin')
        self.assertTrue(image.is_file(), f'Missing source-built ROM: {image}')
        self.set_machine('origin200' if cpu == 'R10000' else 'malta')
        self.vm.set_qmp_monitor(False)
        self.vm.add_args('-accel', 'tcg,thread=single', '-cpu', cpu,
                         '-m', '128M', '-nodefaults', '-display', 'none',
                         '-no-reboot', '-semihosting', '-bios', str(image))
        child_started = os.times()
        started = time.monotonic()
        self.vm.launch()
        self.vm.wait(timeout=self.timeout)
        elapsed = time.monotonic() - started
        child_finished = os.times()
        child_cpu_available = os.name == 'posix'
        child_user = child_finished.children_user - child_started.children_user
        child_system = (child_finished.children_system -
                        child_started.children_system)
        output = self.vm.get_log() or ''
        self.assertEqual(self.vm.exitcode(), 0, output)
        self.assertIn('PASS tlbmaint', output.splitlines(), output)
        page_bytes, writes = WORKLOADS[name]
        result = {'cpu': cpu, 'page_bytes': page_bytes, 'tlb_writes': writes,
                  'reads_per_write': 6, 'wall_seconds': elapsed,
                  'child_user_seconds': (child_user if child_cpu_available
                                         else None),
                  'child_system_seconds': (child_system if child_cpu_available
                                           else None),
                  'includes_startup': True, 'passed': True}
        self.log.info('TLB replacement: %s', result)
        Path(self.log_file('performance.json')).write_text(
            json.dumps(result, indent=2) + '\n', encoding='utf-8')

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
