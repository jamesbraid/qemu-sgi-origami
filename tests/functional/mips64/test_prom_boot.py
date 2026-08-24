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
test_origin200_matrix.py already reads for its own (different) oracle.

codes_seen() and mark_reached() below are the two pure functions this file
factors the scrape-and-verdict logic into, so test_prom_boot_verdict.py can
prove they implement boot-gate's set-subset contract without booting
anything. Both are free of any qemu_test import on purpose: importing
qemu_test requires MESON_BUILD_ROOT (see tests/functional/qemu_test/
config.py) and the qemu.machine/pycotap packages meson's own PYTHONPATH
wires in, neither of which the equivalence unit test's plain `pytest
test_prom_boot_verdict.py` run has -- see the guarded import below.
"""

import os
import re
import sys
import time

# lib_matrix lives one directory up (tests/functional/lib_matrix.py). Under
# meson this is already on PYTHONPATH (tests/functional/meson.build prepends
# meson.current_source_dir()); inserted explicitly here too so that a plain
# `pytest test_prom_boot_verdict.py` run -- which imports codes_seen and
# mark_reached from this file without going through meson at all -- finds it
# the same way tools/pod-console inserts tools/ before importing podlib.
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lib_matrix import Cell, machine_props, qemu_args, _PROGRESS_CODE_RE  # noqa: E402

# qemu_test is only importable inside meson's own test environment (it
# raises at import time without MESON_BUILD_ROOT, and its own imports need
# packages meson's PYTHONPATH supplies). Guarded so that codes_seen(),
# mark_reached() and MARK -- the pure logic test_prom_boot_verdict.py
# exercises -- stay importable without it; PromBoot itself is simply not
# defined in that case, which is fine because nothing outside meson ever
# tries to run the real boot.
try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


# Where ORIGAMI_ASSETS resolves the real firmware image, matching
# tools/containerlib.py's resolve_assets_root() and tools/pod-console's
# DEFAULT_PROM in the project checkout: ORIGAMI_ASSETS names a CHECKOUT
# (one with its own assets/ directory), not the assets/ directory itself --
# confirmed against tools/README.md's own description of the variable
# ("resolves asset paths (assets/firmware/...) against `path`").
FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5-base',
                            'ip27prom.payload.bin')

# The set-subset comparison's own budget: boot-gate's default (tools/
# boot-gate, in the project checkout) is 700s, measured against a real
# from-scratch boot reaching the interactive System Maintenance Menu.  Kept
# a little under the meson timeout registered for this test in
# mips64/meson.build so this loop can fail with its own message and shut
# the VM down cleanly instead of meson's own timeout killing the process
# first.
BOOT_TIMEOUT_SECONDS = 660
POLL_INTERVAL_SECONDS = 0.5

# The firmware-boot golden mark: the progress codes a known-good boot
# reaches, embedded here because this fork test cannot read the project
# checkout at runtime. Source: tests/golden/firmware-boot-progress-mark in
# the PROJECT checkout (/home/jamesb/joy/tech/origami, not this fork) --
# its non-comment data lines, counted directly (47 codes; 0x80 and 0xff
# appear only in that file's retirement comment and are correctly excluded
# here, same as boot-gate's own read_mark() excludes them).
MARK = frozenset({
    '0x00', '0x01', '0x02', '0x03', '0x04', '0x05', '0x06', '0x0a',
    '0x0b', '0x0c', '0x0d', '0x0e', '0x0f', '0x1d', '0x1e', '0x24',
    '0x28', '0x2a', '0x2b', '0x35', '0x36', '0x38', '0x3c', '0x3d',
    '0x3e', '0x3f', '0x40', '0x41', '0x4a', '0x4b', '0x4c', '0x4d',
    '0x4f', '0x50', '0x51', '0x52', '0x53', '0x54', '0x55', '0x56',
    '0x57', '0x58', '0x59', '0x5a', '0x5b', '0x5c', '0x5d',
})


def codes_seen(trace_text):
    """Every progress code sgi_hub_led's trace line reports, as a set.

    Matches tools/boot-gate's own CODE regex in the project checkout: every
    0xNN this trace event reports counts, regardless of KIND (progress,
    FAILURE, or undecoded) -- boot-gate's set-subset comparison was never
    kind-filtered, so this port keeps that contract rather than narrowing
    it. (Contrast lib_matrix.first_progress_code, which deliberately keeps
    only kind == 'progress' lines for its own, different, single-shot "did
    the boot get anywhere at all" oracle -- a different question this
    function does not answer.)

    Reuses lib_matrix's own _PROGRESS_CODE_RE for the line shape rather
    than re-deriving it, so a change to the trace event's printf format
    only has one regex to correct.
    """
    return {code for code, _kind in _PROGRESS_CODE_RE.findall(trace_text)}


def mark_reached(seen, mark):
    """Whether seen is a superset of mark, and which mark codes are missing.

    Returns (True, []) when every code in mark also appears in seen --
    extra codes in seen beyond mark do not affect the verdict, matching
    tools/boot-gate's own subset comparison (order-independent; a boot
    that goes further than the recorded mark still passes). Otherwise
    returns (False, sorted(mark - seen)), naming exactly what is missing.

    An empty seen is the degenerate case where every mark code is
    "missing" -- boot-gate's own "a run that did not start" case. This
    function does not give that case separate wording; the caller (this
    module's test_prom_boot, and boot-gate's own main()) does, the same
    way boot-gate's codes_reached() and its "not seen" check are two
    separate pieces of that tool.
    """
    missing = sorted(mark - seen)
    return not missing, missing


if QemuSystemTest is not None:

    class PromBoot(QemuSystemTest):
        """Boots the real IP27 PROM firmware and requires the recorded
        firmware-boot progress-code mark to be a subset of what it reaches.
        See this module's docstring for the full contract; this class only
        wires it to a real boot, the same division test_origin200_matrix.py
        keeps between lib_matrix and its own launch code.
        """

        def _firmware_path(self):
            """The real PROM image under ORIGAMI_ASSETS, or None if the env
            var is unset or the image is not there -- the runner may not
            have it provisioned yet (see this module's skip message)."""
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            path = os.path.join(assets_root, FIRMWARE_REL)
            return path if os.path.isfile(path) else None

        def test_prom_boot(self):
            firmware = self._firmware_path()
            if firmware is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware at '
                    f'<ORIGAMI_ASSETS>/{FIRMWARE_REL} -- point it at a '
                    'checkout with the real PROM image extracted '
                    '(tools/extract-irix prom) to run this test')

            # A single CPU, single node -- the smallest cell that boots the
            # real firmware -- built via lib_matrix's own Cell/machine_props/
            # qemu_args rather than hand-writing -m/-smp/-accel, the same
            # reuse the brief for this test calls for. firmware='prom' makes
            # machine_props() emit synthesized-firmware=off, debug-leds=on --
            # the real firmware, indicator writes decoded -- matching
            # boot-gate's own machine string exactly.
            cell = Cell(id='prom_boot', guest='prom', scenario='full_boot',
                       cpus=1, nodes=1, mem='64M', accel='wallclock',
                       firmware='prom', speed='thorough',
                       timeout=BOOT_TIMEOUT_SECONDS, attempts=1,
                       min_passes=1, expect='pass', xfail_oracle='',
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
            try:
                while time.monotonic() < deadline:
                    if os.path.exists(log_path):
                        with open(log_path, encoding='utf-8',
                                 errors='replace') as fobj:
                            seen = codes_seen(fobj.read())
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
    if QemuSystemTest is not None:
        QemuSystemTest.main()
