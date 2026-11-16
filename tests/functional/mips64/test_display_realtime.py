#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 James Braid

"""Wall-clock check of the shared-memory display's test pattern: frame code
rate, drops, interval spread, and how late the probe sees each frame against
QEMU's own publish stamp, measured by scripts/sgi-display-probe.py against a
machine with no disk and no guest."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from qemu_test import QemuSystemTest

PROBE = Path(__file__).parents[3] / 'scripts' / 'sgi-display-probe.py'
SECONDS = float(os.getenv('SGI_REALTIME_SECONDS', '60'))
# Bounds for shared runners come from the first ten nightly runs.
MAX_P99_MS = os.getenv('SGI_REALTIME_MAX_P99_MS', '40')
MISSED = os.getenv('SGI_REALTIME_MAX_PROBE_MISSED', '3')


class DisplayRealtime(QemuSystemTest):
    timeout = 300

    def test_pattern_at_wall_clock(self):
        self.require_accelerator('tcg')
        self.set_machine('octane')
        rom = self.scratch_file('erased.rom')
        Path(rom).write_bytes(b'\xff' * 4)
        # AF_UNIX paths are short; the scratch directory's may not be.
        short = tempfile.mkdtemp(prefix='sgirt')
        self.addCleanup(shutil.rmtree, short, ignore_errors=True)
        sock = os.path.join(short, 'd.sock')
        trace = self.log_file('trace.log')
        out = self.log_file('probe')

        self.vm.set_machine('octane,display-shm=rt%d,display-shm-socket=%s,'
                            'x-display-shm-test-pattern=on' %
                            (os.getpid(), sock))
        # The harness passes -vga none, so fit the head the pattern replaces.
        self.vm.add_args('-device', 'sgi-mgras,slot=widget-11,product=si',
                         '-cpu', 'R10000', '-m', '128M', '-bios', rom,
                         '-D', trace, '-trace', 'sgi_display_pattern_frame')
        self.vm.launch()
        done = subprocess.run(
            [sys.executable, str(PROBE), 'record', '--socket', sock,
             '--out', out, '--duration', str(SECONDS), '--trace', trace,
             '--min-fps', '29.4', '--max-drops', '0', '--max-repeats', '0',
             '--max-probe-missed', MISSED, '--max-p99-ms', MAX_P99_MS],
            capture_output=True, text=True, timeout=SECONDS + 60)
        self.log.info('probe summary: %s', done.stdout)
        summary = json.loads(done.stdout)
        self.assertEqual(done.returncode, 0, summary['failures'])
        self.assertLessEqual(summary['fps'], 30.6)
        self.assertGreater(summary['frames'], SECONDS * 29)
        # The probe sees a frame after QEMU stamped it, by well under a frame.
        late = summary['publish_to_probe_ms']
        self.assertGreater(late['n'], SECONDS * 29)
        self.assertGreaterEqual(late['min'], 0)


if __name__ == '__main__':
    QemuSystemTest.main()
