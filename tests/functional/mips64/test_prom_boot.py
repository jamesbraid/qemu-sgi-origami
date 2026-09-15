#!/usr/bin/env python3
#
# Firmware-boot progress-code check: boot the real IP27 PROM and require
# every code a known-good boot reaches
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/boot-gate's firmware-boot check, in the PROJECT checkout, into
this fork's own functional-test framework. boot-gate boots the real IP27
PROM firmware on -M origin200, collects the SET of firmware progress codes
QEMU's sgi_hub_led trace event reports, and passes iff a recorded golden
mark is a SUBSET of the codes seen -- order-independent, so a boot that
goes further than the mark still passes, and a boot missing any mark code
has gone backwards. An empty set means the boot never started, not that it
failed early.

The progress codes are QEMU's own trace output, not guest console output:
hw/misc/trace-events (in this tree) declares
`sgi_hub_led(cpu, code, kind, meaning) "CPU %u: 0x%02x (%s: %s)"`, and
tools/pod-console's emulator_argv() (in the project checkout) enables it
unconditionally with `-d trace:sgi_hub_led`, mirrored to stdout/stderr --
which this test framework's own QEMUMachine already redirects to the log
file test.log_file() names (see _post_shutdown/_pre_launch in
python/qemu/machine/machine.py: stdout=stderr=that file). So the codes are
scraped from the VM's captured log after launch, the same file
test_origin200_matrix.py already reads for its own (different) pass
condition.

codes_seen() and mark_reached() (lib_prom.py, beside this file) are the
two pure functions the scrape-and-verdict logic is factored into, so
test_prom_boot_verdict.py can prove they implement boot-gate's set-subset
contract without booting anything.
"""

import os
import time

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_matrix import Cell, machine_props, qemu_args
from lib_prom import (FIRMWARE_REL, MARK, codes_seen, mark_reached,
                      resolve_asset)
from lib_translation_health import assert_translation_healthy


# The set-subset comparison's own budget: boot-gate's default (tools/
# boot-gate, in the project checkout) is 700s, measured against a real
# from-scratch boot reaching the interactive System Maintenance Menu.  Kept
# a little under the meson timeout registered for this test in
# mips64/meson.build so this loop can fail with its own message and shut
# the VM down cleanly instead of meson's own timeout killing the process
# first.
BOOT_TIMEOUT_SECONDS = 660
POLL_INTERVAL_SECONDS = 0.5


class PromBoot(QemuSystemTest):
    """Boots the real IP27 PROM firmware and requires the recorded
    firmware-boot progress-code mark to be a subset of what it reaches.
    See this module's docstring for the full contract; this class only
    wires it to a real boot, the same division test_origin200_matrix.py
    keeps between lib_matrix and its own launch code.
    """

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_boot(self):
        firmware = resolve_asset(os.environ['ORIGAMI_ASSETS'],
                                 FIRMWARE_REL)
        if firmware is None:
            # @skipIfMissingEnv('ORIGAMI_ASSETS') above already skipped
            # the case where it is unset entirely -- getting here means
            # it IS set, so a missing firmware image is a provisioning
            # failure, not an absent-assets developer run, and must
            # fail loudly: a silent skip here would let a real
            # firmware-boot regression pass CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but no firmware is present '
                f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} -- provision it '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')

        # A single CPU and node use lib_matrix's real-PROM machine
        # properties and QEMU arguments rather than duplicating them here.
        cell = Cell(id='prom_boot', guest='prom', scenario='full_boot',
                   cpus=1, nodes=1, mem='64M', accel='wallclock',
                   speed='thorough',
                   timeout=BOOT_TIMEOUT_SECONDS, attempts=1,
                   min_passes=1, expect='pass', xfail_mark='',
                   xfail_signature='')

        vm = self.get_vm(name='prom_boot')
        vm.set_machine(machine_props(cell))
        vm.add_args(*qemu_args(cell))
        # -cpu is left to the machine's own default_cpu_type (R10000,
        # hw/mips/sn0.c) rather than named explicitly as pod-console's
        # standalone invocation does -- test_origin200_matrix.py's own
        # launch pattern relies on the same default, and the two
        # produce an identical running CPU model.
        vm.add_args('-bios', firmware, '-d', 'trace:sgi_hub_led')
        vm.launch()

        log_path = self.log_file('prom_boot.log')
        deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
        seen = set()
        health_checked = False
        try:
            while time.monotonic() < deadline:
                if os.path.exists(log_path):
                    with open(log_path, encoding='utf-8',
                             errors='replace') as fobj:
                        seen = codes_seen(fobj.read())
                if seen and not health_checked:
                    # The first progress code is the firmware's first
                    # demonstrable run; catch a translation storm here
                    # instead of spending the rest of this boot's
                    # budget on a guest running 100 to 300 times
                    # slower than it should.
                    health_checked = True
                    assert_translation_healthy(vm)
                ok, _missing = mark_reached(seen, MARK)
                if ok:
                    break
                if vm.exitcode() is not None:
                    break
                time.sleep(POLL_INTERVAL_SECONDS)
        finally:
            # A VM that crashed or exited on its own has already left;
            # a graceful shutdown() against an already-dead process
            # still raises AbnormalShutdown even though qemu is in
            # fact gone. Matches _boot_one_attempt's own reasoning in
            # test_origin200_matrix.py.
            vm.shutdown(hard=vm.exitcode() is not None)

        if not seen:
            self.fail(
                'the firmware emitted no progress codes whatever -- '
                'a run that did not start, not a boot that failed '
                'early. Check the emulator built, and that nothing '
                'ended the machine before it printed anything.')

        ok, missing = mark_reached(seen, MARK)
        self.assertTrue(
            ok,
            f'boot went backwards: reached {len(MARK) - len(missing)} '
            f'of {len(MARK)} recorded progress codes '
            f'({len(seen)} codes seen this run). '
            f'no longer reached: {" ".join(missing)}. '
            'Recorded mark: tests/golden/firmware-boot-progress-mark '
            'in the project checkout.')


if __name__ == '__main__':
    QemuSystemTest.main()
