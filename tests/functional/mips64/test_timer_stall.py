#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Reproduce the Compare-store timer race with a host-side GDB stop."""

import os
import signal
import subprocess
from pathlib import Path

from qemu_test import QemuSystemTest
from qemu_test.config import BUILD_DIR


class TimerStall(QemuSystemTest):
    timeout = 300

    def test_compare_store_survives_host_stall(self):
        self.require_accelerator('tcg')
        gdb = os.getenv('QEMU_TEST_GDB')
        if not gdb:
            self.skipTest('QEMU was configured without host GDB')

        rom = BUILD_DIR / 'tests/functional/mips64/roms/timerstall.bin'
        self.assertTrue(rom.is_file(), f'Missing source-built ROM: {rom}')
        script = Path(self.scratch_file('timer-stall.gdb'))
        script.write_text('\n'.join([
            'set pagination off',
            'set confirm off',
            'break cpu_mips_timer_update if '
            '$_caller_is("cpu_mips_store_compare")',
            'commands',
            'silent',
            r'echo TIMER_STALL_BREAKPOINT_HIT\n',
            'shell sleep 0.2',
            'continue',
            'end',
            'run',
            '',
        ]))
        command = [gdb, '-batch', '-x', str(script), '--args',
                   self.qemu_bin, '-M', 'origin200', '-cpu', 'R10000',
                   '-nodefaults', '-display', 'none', '-no-reboot',
                   '-semihosting', '-bios', str(rom)]

        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True,
                                   start_new_session=True)
        try:
            output, _ = process.communicate(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            self.fail(f'GDB/QEMU timed out after {self.timeout}s\n{output}')

        self.log.info('GDB/QEMU output:\n%s', output)
        self.assertEqual(process.returncode, 0, output)
        self.assertIn('TIMER_STALL_BREAKPOINT_HIT', output, output)
        self.assertIn('PASS timerstall', output.splitlines(), output)
        self.assertNotIn('FAIL timerstall', output)


if __name__ == '__main__':
    QemuSystemTest.main()
