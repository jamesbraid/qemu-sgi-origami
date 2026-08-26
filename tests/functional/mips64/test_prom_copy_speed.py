#!/usr/bin/env python3
#
# Firmware-boot copy-speed check: boot the real IP27 PROM and require the
# PROM-relocation step to finish quickly, guarding hw/mips/sn0.c's direct
# (romd) view of the boot flash's Hub-space base against a speed regression.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The PROM copies and runs its relocation code through the boot flash's
node-local Hub-space window. With a direct romd view of the chip there the copy
takes a few seconds; with the memory-mapped I/O mirror the translator cannot
back the copy loop and it takes minutes -- an instruction retranslated per
block. See docs/notebooks/prom-boot-copy-slowness.md in the project checkout.

This boots the real firmware and asserts the copy loop stays under a generous
budget, catching a speed regression from any cause -- not only a re-routing to
I/O, which the sn0-address-map qtest already guards structurally.

The copy loop is bracketed by two sgi_hub_led progress codes: 0x5c ("testing
interrupts", the last code the firmware emits before it copies) and 0x3c
("jumping to code running from RAM", the first after it). Their wall-clock
separation is the copy-loop duration -- a few seconds fixed, the whole ~223s
copy unfixed.

copy_bracket_seconds(), copy_is_fast() and record_first_seen() are pure
functions the companion test_prom_copy_speed_verdict.py exercises without
booting, the same split test_prom_boot keeps with its verdict module.
"""

import os
import sys
import time

# lib_matrix and test_prom_boot live one directory up / beside this file;
# inserted on sys.path so a plain `pytest test_prom_copy_speed_verdict.py`
# resolves them without meson, exactly as test_prom_boot does.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lib_matrix import Cell, machine_props, qemu_args  # noqa: E402
from test_prom_boot import FIRMWARE_REL, codes_seen  # noqa: E402

# qemu_test only imports inside meson's own test environment; guarded so the
# pure verdict functions stay importable for the plain-pytest equivalence test.
try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None

# The copy loop's bracket. 0x5c is the last progress code the firmware emits
# before it copies; 0x3c the first after it jumps to the RAM copy.
COPY_BRACKET_START = '0x5c'
COPY_BRACKET_END = '0x3c'

# The budget. Fixed, the two codes are a few seconds apart; a code-from-I/O
# regression makes the loop take ~223s. 90s sits far above the fixed cost --
# tens of seconds even under heavy CI load -- and far below any real regression,
# which is an order of magnitude or more.
COPY_BRACKET_MAX_SECONDS = 90.0

POLL_INTERVAL_SECONDS = 0.5
# Long enough to let even a regressed (minutes-long) copy reach 0x3c so the
# failure can name the real duration; kept under the meson timeout so this loop
# reports its own message and shuts the VM down cleanly.
DEADLINE_SECONDS = 280


def copy_bracket_seconds(first_seen):
    """The copy-loop duration from a {code: first-seen-monotonic} map, or None
    if either bracket code has not appeared yet."""
    start = first_seen.get(COPY_BRACKET_START)
    end = first_seen.get(COPY_BRACKET_END)
    if start is None or end is None:
        return None
    return end - start


def copy_is_fast(first_seen, threshold=COPY_BRACKET_MAX_SECONDS):
    """(ok, duration): ok iff the copy loop was bracketed and finished within
    threshold. A bracket that never closed is not fast."""
    duration = copy_bracket_seconds(first_seen)
    return (duration is not None and duration <= threshold), duration


def record_first_seen(first_seen, trace_text, now):
    """Stamp any bracket code newly present in trace_text with time `now`,
    keeping the earliest sighting. Mutates and returns first_seen."""
    seen = codes_seen(trace_text)
    for code in (COPY_BRACKET_START, COPY_BRACKET_END):
        if code in seen and code not in first_seen:
            first_seen[code] = now
    return first_seen


if QemuSystemTest is not None:

    class PromCopySpeed(QemuSystemTest):
        """Boots the real IP27 PROM and requires the PROM-relocation step to
        finish within COPY_BRACKET_MAX_SECONDS. See the module docstring."""

        def _firmware_path(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            path = os.path.join(assets_root, FIRMWARE_REL)
            return path if os.path.isfile(path) else None

        def test_prom_copy_speed(self):
            firmware = self._firmware_path()
            if firmware is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware at '
                    f'<ORIGAMI_ASSETS>/{FIRMWARE_REL} -- point it at a '
                    'checkout with the real PROM image extracted '
                    '(tools/extract-irix prom) to run this test')

            # The same smallest real-firmware cell test_prom_boot builds.
            cell = Cell(id='prom_copy_speed', guest='prom',
                        scenario='full_boot', cpus=1, nodes=1, mem='64M',
                        accel='wallclock', firmware='prom', speed='thorough',
                        timeout=DEADLINE_SECONDS, attempts=1, min_passes=1,
                        expect='pass', xfail_mark='', xfail_signature='')

            vm = self.get_vm(name='prom_copy_speed')
            vm.set_machine(machine_props(cell))
            vm.add_args(*qemu_args(cell))
            vm.add_args('-bios', firmware, '-d', 'trace:sgi_hub_led')
            vm.launch()

            # QEMUMachine sends the guest's stderr -- where -d trace writes --
            # to <log_dir>/<vm-name>.log, and log_dir is this test's own
            # directory. So the trace lands here precisely because this log name
            # matches the vm name passed to get_vm() above; no explicit -D is
            # needed, and test_prom_boot leans on the same coupling for its own
            # progress-code scrape.
            log_path = self.log_file('prom_copy_speed.log')
            deadline = time.monotonic() + DEADLINE_SECONDS
            first_seen = {}
            try:
                while time.monotonic() < deadline:
                    now = time.monotonic()
                    if os.path.exists(log_path):
                        with open(log_path, encoding='utf-8',
                                  errors='replace') as fobj:
                            record_first_seen(first_seen, fobj.read(), now)
                    if COPY_BRACKET_END in first_seen:
                        break
                    if vm.exitcode() is not None:
                        break
                    time.sleep(POLL_INTERVAL_SECONDS)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)

            self.assertIn(
                COPY_BRACKET_START, first_seen,
                'the firmware never reached the pre-copy progress code '
                f'{COPY_BRACKET_START} -- the boot did not get as far as the '
                'PROM-relocation step, so its speed could not be measured.')

            ok, duration = copy_is_fast(first_seen)
            self.assertIsNotNone(
                duration,
                'the firmware reached the pre-copy code but never emitted '
                f'{COPY_BRACKET_END} within {DEADLINE_SECONDS}s -- the copy '
                'step did not complete. A romd regression makes this step take '
                'minutes; see docs/notebooks/prom-boot-copy-slowness.md.')
            self.assertTrue(
                ok,
                f'PROM-relocation step took {duration:.1f}s, over the '
                f'{COPY_BRACKET_MAX_SECONDS:.0f}s budget. The boot flash\'s '
                'Hub-space base is likely back on the I/O mirror instead of a '
                'romd view -- code run from an I/O region is retranslated per '
                'instruction. See docs/notebooks/prom-boot-copy-slowness.md in '
                'the project checkout.')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
