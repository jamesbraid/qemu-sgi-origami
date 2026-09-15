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

copy_bracket_seconds(), copy_is_fast() and record_first_seen()
(lib_prom_copy_speed.py) are pure functions the companion
test_prom_copy_speed_verdict.py exercises without booting, the same split
test_prom_boot keeps with lib_prom.py and its verdict module.
"""

import os
import time

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_matrix import Cell, machine_props, qemu_args
from lib_prom import FIRMWARE_REL, resolve_asset
from lib_prom_copy_speed import (COPY_BRACKET_END, COPY_BRACKET_MAX_SECONDS,
                                 COPY_BRACKET_START, copy_is_fast,
                                 record_first_seen)
from lib_translation_health import assert_translation_healthy

POLL_INTERVAL_SECONDS = 0.5
# Long enough to let even a regressed (minutes-long) copy reach 0x3c so the
# failure can name the real duration; kept under the meson timeout so this loop
# reports its own message and shuts the VM down cleanly.
DEADLINE_SECONDS = 280


class PromCopySpeed(QemuSystemTest):
    """Boots the real IP27 PROM and requires the PROM-relocation step to
    finish within COPY_BRACKET_MAX_SECONDS. See the module docstring."""

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_copy_speed(self):
        firmware = resolve_asset(os.environ['ORIGAMI_ASSETS'],
                                 FIRMWARE_REL)
        if firmware is None:
            # @skipIfMissingEnv('ORIGAMI_ASSETS') above already skipped
            # the case where it is unset entirely -- getting here means
            # it IS set, so a missing firmware image is a provisioning
            # failure, not an absent-assets developer run, and must
            # fail loudly: a silent skip here would let a real
            # copy-speed regression pass CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but no firmware is present '
                f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} -- provision it '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')

        # The same smallest real-firmware cell test_prom_boot builds.
        cell = Cell(id='prom_copy_speed', guest='prom',
                    scenario='full_boot', cpus=1, nodes=1, mem='64M',
                    accel='wallclock', speed='thorough',
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
        health_checked = False
        try:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if os.path.exists(log_path):
                    with open(log_path, encoding='utf-8',
                              errors='replace') as fobj:
                        record_first_seen(first_seen, fobj.read(), now)
                if first_seen and not health_checked:
                    # The first pre-copy progress code is the
                    # firmware's first demonstrable run.
                    health_checked = True
                    assert_translation_healthy(vm)
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
    QemuSystemTest.main()
