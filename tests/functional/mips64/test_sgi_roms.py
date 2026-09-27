#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the source-built MIPS CPU ROM matrix through native QEMU tests."""

from pathlib import Path

from qemu_test import QemuSystemTest
from qemu_test.config import BUILD_DIR


CONFIGS = {
    'default': [],
    '4cpu-2node': ['-M', 'topology=origin200-dual', '-m', '128',
                  '-smp', '4', '-accel', 'tcg,thread=multi'],
    'icount': ['-icount', 'shift=3,align=off,sleep=off'],
}


class SGIRoms(QemuSystemTest):
    timeout = 180

    def run_rom(self, rom, cpu, config):
        self.require_accelerator('tcg')
        self.set_machine('origin200' if cpu == 'R10000' else 'malta')
        image = BUILD_DIR / 'tests/functional/mips64/roms' / (rom + '.bin')
        self.assertTrue(image.is_file(), f'Missing source-built ROM: {image}')
        # These programs can exit before the QMP handshake completes.
        self.vm.set_qmp_monitor(False)
        self.vm.add_args('-cpu', cpu, '-nodefaults', '-display', 'none',
                         '-no-reboot', '-semihosting', '-bios', str(image),
                         *CONFIGS[config])
        self.vm.launch()
        self.vm.wait(timeout=self.timeout)
        output = self.vm.get_log() or ''
        self.assertEqual(self.vm.exitcode(), 0, output)
        self.assertIn('PASS ' + rom, output.splitlines(), output)


def add_case(rom, cpu, config):
    def test(self):
        self.run_rom(rom, cpu, config)
    name = f'test_{rom}_{cpu}_{config}'.replace('-', '_')
    if hasattr(SGIRoms, name):
        raise ValueError(f'Duplicate ROM case: {name}')
    setattr(SGIRoms, name, test)


for line in (Path(__file__).parent / 'roms/suite').read_text().splitlines():
    fields = line.split('#', 1)[0].split()
    if not fields:
        continue
    rom, *options = fields
    configs = [item.split(':', 1)[1] for item in options
               if item.startswith('config:')]
    if len(configs) > 1 or any(c not in CONFIGS for c in configs):
        raise ValueError(f'Invalid ROM configuration: {line}')
    config = configs[0] if configs else 'default'
    cpus = [item for item in options
            if not item.startswith('config:') and item != 'timed']
    if not cpus or any(cpu not in ('R4000', 'R10000', 'R14000')
                       for cpu in cpus):
        raise ValueError(f'Invalid ROM CPU list: {line}')
    for cpu in cpus:
        add_case(rom, cpu, config)
        # The throughput ROM must succeed on both one and four CPUs.
        # Wall-clock ratios never determined the old runner's verdict.
        if 'timed' in options:
            if config == 'default':
                raise ValueError(f'Timed ROM needs a configuration: {line}')
            add_case(rom, cpu, 'default')


if __name__ == '__main__':
    QemuSystemTest.main()
