#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run the source-built MIPS CPU ROM matrix through native QEMU tests."""

from pathlib import Path

from qemu_test import QemuSystemTest
from qemu_test.config import BUILD_DIR
from lib_sgi_roms import CONFIGS, read_suite


CONFIG_ARGS = {
    'default': [],
    '4cpu-2node': ['-M', 'topology=origin200-dual', '-m', '128',
                  '-smp', '4', '-accel', 'tcg,thread=multi'],
    'icount': ['-icount', 'shift=3,align=off,sleep=off'],
}

CPU_ARGS = {
    'R4000': 'R4000',
    'R10000': 'R10000',
    'R14000': 'R14000',
    'R16000-4m': 'R16000,big-endian=on,r16000-experimental=on,'
                 'r16000-reset-mode=0x3a005,r16000-scache-bytes=4194304',
    'R16000-8m': 'R16000,big-endian=on,r16000-experimental=on,'
                 'r16000-reset-mode=0x4a005,r16000-scache-bytes=8388608',
    'R16000-16m': 'R16000,big-endian=on,r16000-experimental=on,'
                  'r16000-reset-mode=0x5a005,r16000-scache-bytes=16777216',
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
        self.vm.add_args(
            '-cpu', CPU_ARGS[cpu], '-nodefaults', '-display', 'none',
            '-no-reboot', '-semihosting', '-bios', str(image),
                         *CONFIG_ARGS[config])
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


for case in read_suite(Path(__file__).parent / 'roms/suite'):
    add_case(case.rom, case.cpu, case.config)


if __name__ == '__main__':
    QemuSystemTest.main()
