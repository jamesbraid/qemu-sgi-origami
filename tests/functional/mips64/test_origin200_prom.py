#!/usr/bin/env python3
#
# The Origin 200 PROM-boot family: one script, one boot per manifest cell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Merges what were four separate scripts, each booting the real IP27 PROM on
-M origin200 on its own, into this one: the origin200-matrix manifest's own
per-cell boots (test_origin200_matrix.py and test_origin200_matrix_thorough.py,
one script per manifest speed) and the two fixed single-cell checks that
duplicated the matrix's own smallest cell in all but their pass condition
(test_prom_boot.py's firmware-boot progress-code check and
test_prom_copy_speed.py's PROM-relocation copy-speed check).

Every row now boots the real firmware (the synthesized-firmware axis PR #741
removed), so the fixed checks' own cell -- one CPU, one node, 64M, wallclock --
is no longer a special case: it is the new prom_full_boot_1cpu row below,
and the copy-speed check attaches to that row's own boot as an extra
assertion, not a second launch. codes_seen()/mark_reached() (lib_prom.py)
and copy_bracket_seconds()/copy_is_fast()/record_first_seen()
(lib_prom_copy_speed.py) stay the pure functions their own verdict
companions exercise without booting; this file only wires them to real
boots, the same division the four absorbed scripts each kept.

One meson target now covers the whole family instead of four: every row
needs QEMU_TEST_SGI_ASSET_DIR (the real firmware), which is exactly
the "tests that fetch assets" case this directory's own meson.build already
carves out for
the thorough suite (see that file's header comment) -- the one manifest row
that used to run under the quick suite (prom_full_boot_mttcg) gained nothing
from that split, since it already no-op-skipped in a no-asset quick run.
register_matrix_cells() is still called once per speed so a manifest row's
speed column keeps documenting the boot's own weight, but both calls attach
their methods to the one class below, which meson now runs as a single
script covering the whole manifest.
"""

import os
import time

from qemu_test import QemuSystemTest
from qemu_test.asset import AssetError, LocalAsset

from lib_matrix import (AttemptResult, MatrixError, machine_props,
                        matrix_rows, parse_row, peek_row, prom_args,
                        qemu_args, verdict)
from lib_prom import FIRMWARE_REL, MARK, codes_seen, mark_reached
from lib_prom_copy_speed import (COPY_BRACKET_END, COPY_BRACKET_MAX_SECONDS,
                                 COPY_BRACKET_START, copy_is_fast,
                                 record_first_seen)
from lib_translation_health import assert_translation_healthy


MATRIX_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'origin200-matrix')

# How often to re-check the pass condition while an attempt's boot is in
# flight. Matches test_origin200_matrix.py's own value.
POLL_INTERVAL_SECONDS = 0.05

# The one manifest cell the PROM-relocation copy-speed check attaches to:
# the smallest real boot (one CPU, one node), the same cell test_prom_boot.py
# and test_prom_copy_speed.py each built by hand before this merge.
COPY_SPEED_CELL_ID = 'prom_full_boot_1cpu'
PROM_SHA256 = 'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd'


def _boot_one_attempt(test, cell, attempt_index, first_seen=None):
    """Launch one boot attempt for cell and return its AttemptResult.

    A VM name unique to this attempt keeps each attempt's own captured
    QEMU log (and QMP connection) isolated from every other attempt of
    the same cell, so a retry never reads a previous attempt's output.

    first_seen, when given, is a {code: first-seen-monotonic} map that
    _wait_for_pass() records the PROM-relocation bracket codes into as
    they appear in this attempt's own log -- the extra bookkeeping
    COPY_SPEED_CELL_ID's own attempt needs, on the very same boot,
    rather than a second launch.
    """
    name = f'{cell.id}_attempt{attempt_index}'
    vm = test.get_vm(name=name)
    vm.set_machine(machine_props(cell))
    vm.add_args(*qemu_args(cell))

    # Every cell boots the real PROM now that the synthesized-firmware
    # axis is gone. Resolve it the same way the absorbed scripts did
    # (QEMU_TEST_SGI_ASSET_DIR points directly at the asset directory) and
    # skip if it is not provisioned, rather than launching
    # a machine with no firmware at all.
    try:
        firmware_path = test.ASSET_FIRMWARE.fetch()
    except AssetError as exc:
        test.fail(str(exc))
    if firmware_path is None:
        test.skipTest('QEMU_TEST_SGI_ASSET_DIR is not set: no PROM to run')
    vm.add_args(*prom_args(firmware_path))

    # The PROM can reset the machine before QMP capability negotiation ends.
    # Establish the monitor connection before letting the CPUs run.
    vm.add_args('-S')
    try:
        vm.launch()
    except Exception as exc:  # pylint: disable=broad-except
        # Any failure to launch at all -- missing asset, a crash before
        # QMP came up, and so on -- is infrastructure, never a quiet
        # reached_pass=False: verdict() must never absorb it into an
        # ordinary fail or (worse) an xfail match.
        return AttemptResult(reached_pass=False, infra_error=str(exc))

    log_path = test.log_file(f'{name}.log')
    try:
        vm.cmd('cont')
        result = _wait_for_pass(test, vm, cell, log_path, first_seen)
    finally:
        # _wait_for_pass() can itself raise (assert_translation_healthy()
        # failing a regression check) without ever reaching the shutdown
        # below; finally covers that path too; a VM leaked here keeps
        # running underneath a multi-attempt cell's later attempts.
        #
        # A VM that crashed mid-boot has already exited on its own. A
        # graceful shutdown() against an already-dead process still raises
        # AbnormalShutdown even though qemu is in fact gone -- uncaught,
        # that would abort test_cell() before a multi-attempt cell's later
        # attempts run, defeating the retry loop on exactly the crash path
        # retries exist for. hard=True skips the graceful attempt instead.
        vm.shutdown(hard=vm.exitcode() is not None)
    return result


def _wait_for_pass(_test, vm, cell, log_path, first_seen=None):
    """Poll for cell's pass condition for up to cell.timeout seconds.

    The recorded firmware-boot progress mark (lib_prom.MARK) must be a
    subset of what the log's sgi_hub_led trace lines show -- lib_prom's
    own mark_reached(codes_seen(log_text), MARK), read directly here
    (rather than through lib_matrix.pass_condition_met()) so this loop
    also has `seen` on hand to tell "reached nothing at all" apart from
    an ordinary timeout partway through, the distinction
    test_prom_boot.py used to draw in its own final failure message.

    assert_translation_healthy() runs once, the first time any progress
    code appears -- the boot's first demonstrable run -- catching a
    translation storm here instead of spending the rest of the budget on
    a guest running 100 to 300 times slower than it should. Both
    absorbed fixed-cell scripts ran this same check on their own single
    boot, which is the same boot the new prom_full_boot_1cpu row now
    is: gated on first_seen (only ever passed for that cell) so the
    other matrix rows keep exactly the boot behavior they had before
    this merge, with no assertion this file's own scope did not already
    cover for them.

    first_seen, when given, also gets the PROM-relocation bracket codes'
    first-seen times recorded into it via lib_prom_copy_speed's own
    record_first_seen() -- the bookkeeping the copy-speed check reads
    after this boot passes, so that check runs as one more assertion
    over the same driver session rather than a second launch.
    """
    deadline = time.monotonic() + cell.timeout
    seen = set()
    health_checked = False
    while time.monotonic() < deadline:
        exitcode = vm.exitcode()
        if exitcode is not None:
            return AttemptResult(
                reached_pass=False,
                infra_error=f'qemu exited early with code {exitcode}')

        if os.path.exists(log_path):
            with open(log_path, encoding='utf-8', errors='replace') as fobj:
                log_text = fobj.read()
            seen = codes_seen(log_text)
            if first_seen is not None:
                record_first_seen(first_seen, log_text, time.monotonic())
        if seen and not health_checked and first_seen is not None:
            health_checked = True
            assert_translation_healthy(vm)

        ok, _missing = mark_reached(seen, MARK)
        if ok:
            return AttemptResult(reached_pass=True, signature='progress-mark')

        time.sleep(POLL_INTERVAL_SECONDS)

    if not seen:
        # A run that never started, not a boot that failed early --
        # test_prom_boot.py's own distinction, kept here as its own
        # signature so test_cell() can give the same specific message.
        return AttemptResult(reached_pass=False, signature='no-progress-codes')
    return AttemptResult(reached_pass=False, signature='timeout')


def _assert_copy_speed(test, first_seen):
    """The three assertions test_prom_copy_speed.py made about
    first_seen, ported unchanged: the pre-copy code was reached, the
    post-copy code eventually followed it, and the two were no more
    than COPY_BRACKET_MAX_SECONDS apart.
    """
    test.assertIn(
        COPY_BRACKET_START, first_seen,
        'the firmware never reached the pre-copy progress code '
        f'{COPY_BRACKET_START} -- the boot did not get as far as the '
        'PROM-relocation step, so its speed could not be measured.')

    ok, duration = copy_is_fast(first_seen)
    test.assertIsNotNone(
        duration,
        'the firmware reached the pre-copy code but never emitted '
        f'{COPY_BRACKET_END} -- the copy step did not complete. Check '
        'whether the boot flash Hub-space base resolves through a romd '
        'view: when it resolves through the I/O mirror, code fetched from '
        'that I/O region is retranslated per instruction and the copy can '
        'take minutes.')
    test.assertTrue(
        ok,
        f'PROM-relocation step took {duration:.1f}s, exceeding the '
        f'{COPY_BRACKET_MAX_SECONDS:.0f}s budget; check whether the boot '
        'flash Hub-space base resolves through the I/O mirror instead of '
        'a romd view, since code fetched from an I/O region is '
        'retranslated per instruction.')


def _make(lineno, line, problem):
    """Build the test method for one manifest row.

    The row is parsed inside the method, not here: parse_row() can
    raise on a bad row, and raising at import would fail collection of
    every cell in the script rather than the one test the row is.
    `problem` carries what register_matrix_cells() already knows is
    wrong with the row (a duplicate id) and the method fails on it
    before spending a boot.
    """

    def test_cell(self):
        if problem is not None:
            self.fail(problem)
        try:
            cell = parse_row(MATRIX_PATH, lineno, line)
        except MatrixError as exc:
            raise self.failureException(str(exc)) from None

        self.set_machine('origin200')

        first_seen = {} if cell.id == COPY_SPEED_CELL_ID else None
        attempts = [_boot_one_attempt(self, cell, i, first_seen=first_seen)
                   for i in range(cell.attempts)]

        outcome = verdict(cell, attempts)

        if outcome in ('pass', 'xfail') and first_seen is not None:
            _assert_copy_speed(self, first_seen)

        if outcome in ('pass', 'xfail'):
            # xfail is a pass for the suite: every failing attempt matched
            # the cell's own recorded xfail_signature, which is exactly
            # what an expected failure asserts.
            return
        if outcome == 'xpass':
            self.fail(f'{cell.id}: expected xfail ({cell.xfail_signature!r}) '
                      f'but every attempt reached its pass condition: '
                      f'{attempts}')
        if outcome == 'error':
            errors = [a.infra_error for a in attempts
                     if a.infra_error is not None]
            self.fail(f'{cell.id}: infrastructure error: {errors}')
        # outcome == 'fail'
        if any(a.signature == 'no-progress-codes' for a in attempts):
            self.fail(
                f'{cell.id}: the firmware emitted no progress codes '
                'whatever -- a run that did not start, not a boot that '
                'failed early. Check the emulator built, and that '
                'nothing ended the machine before it printed anything.')
        self.fail(f'{cell.id}: did not reach its pass condition '
                  f'({cell.min_passes} of {cell.attempts} required): '
                  f'{attempts}')

    test_cell.__doc__ = f'matrix row {MATRIX_PATH}:{lineno}: {line}'
    return test_cell


def register_matrix_cells(cls, speed):
    """Attach one test method to cls per manifest row of the given speed.

    Rows are only peeked at here (peek_row(): the raw id and speed
    fields); the parse that can fail happens inside each method. A row
    whose speed field is unusable is routed to the quick script, where
    it fails on the run everyone runs; a row whose id field is unusable
    is named test_row<lineno>, so it still appears and still fails.
    Duplicate ids are found across the whole manifest, whichever speed
    each copy names, and every copy fails. Ported unchanged from
    test_origin200_matrix.py, where this used to be the only caller.
    """
    rows = matrix_rows(MATRIX_PATH)
    ids = [peek_row(line)[0] for _lineno, line in rows]
    duplicates = {cell_id for cell_id in ids
                  if cell_id is not None and ids.count(cell_id) > 1}
    for lineno, line in rows:
        cell_id, row_speed = peek_row(line)
        if (row_speed or 'quick') != speed:
            continue
        problem = None
        if cell_id in duplicates:
            problem = f'{MATRIX_PATH}:{lineno}: duplicate cell id {cell_id!r}'
        name = cell_id if cell_id is not None and cell_id not in duplicates \
            else f'row{lineno}'
        method = _make(lineno, line, problem)
        method.__name__ = f'test_{name}'
        setattr(cls, method.__name__, method)


class OriginPromMatrix(QemuSystemTest):
    """One test method per row of origin200-matrix, quick and thorough
    alike: the whole origin200 PROM-boot family runs as a single meson
    target now (see this module's docstring for why). See _make() above
    for what each row does; the manifest, not this class, is the source
    of truth for what cells exist."""

    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)


register_matrix_cells(OriginPromMatrix, 'quick')
register_matrix_cells(OriginPromMatrix, 'thorough')


if __name__ == '__main__':
    QemuSystemTest.main()
