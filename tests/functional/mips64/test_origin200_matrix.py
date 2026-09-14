#!/usr/bin/env python3
#
# The Origin 200 test matrix: one boot-and-verdict test per manifest cell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
One test_<id> method per row of origin200-matrix whose speed column says
quick, attached to OriginMatrix at import time by register_matrix_cells();
test_origin200_matrix_thorough.py attaches the thorough rows to its own
class the same way, so the manifest's speed column maps onto meson's
quick/thorough split. Each method parses and validates its own row when
it runs, so a bad row fails that one test instead of taking the whole
script down at collection. All per-cell configuration, command-line
construction, pass-condition scoring, retry and expected-failure logic
lives in lib_matrix; this file only wires it to a real boot.
"""

import os
import time

from qemu_test import QemuSystemTest

from lib_matrix import (AttemptResult, MatrixError, machine_props,
                        matrix_rows, parse_row, pass_condition_met,
                        peek_row, prom_args, qemu_args, verdict)
from lib_prom import FIRMWARE_REL, resolve_asset


MATRIX_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'origin200-matrix')

# How often to re-check the pass condition while an attempt's boot is in
# flight.
POLL_INTERVAL_SECONDS = 0.05


def _boot_one_attempt(test, cell, attempt_index):
    """Launch one boot attempt for cell and return its AttemptResult.

    A VM name unique to this attempt keeps each attempt's own captured
    QEMU log (and QMP connection) isolated from every other attempt of
    the same cell, so a retry never reads a previous attempt's output.
    """
    name = f'{cell.id}_attempt{attempt_index}'
    vm = test.get_vm(name=name)
    vm.set_machine(machine_props(cell))
    vm.add_args(*qemu_args(cell))

    # Every cell boots the real PROM now that the synthesized-firmware
    # axis is gone. Resolve it the same way test_prom_boot.py does
    # (ORIGAMI_ASSETS names a checkout, not the assets/ directory
    # itself) and skip if it is not provisioned, rather than launching
    # a machine with no firmware at all.
    assets_root = os.environ.get('ORIGAMI_ASSETS')
    if not assets_root:
        # No decorator covers this cell test -- every row needs
        # ORIGAMI_ASSETS now, so the unset case is handled here rather
        # than by @skipIfMissingEnv. A developer with no assets
        # provisioned gets the usual skip.
        test.skipTest(
            'ORIGAMI_ASSETS is not set -- point it at a checkout '
            'with the real PROM image extracted (tools/extract-irix '
            'prom) to run this test')
    firmware_path = resolve_asset(assets_root, FIRMWARE_REL)
    if firmware_path is None:
        # ORIGAMI_ASSETS IS set, so a missing firmware image is a
        # provisioning failure, not an absent-assets developer run,
        # and must fail loudly: a silent skip here would let a
        # real firmware-boot regression pass CI unnoticed.
        test.fail(
            'ORIGAMI_ASSETS is set but no firmware is present '
            f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} -- provision it '
            '(nightly assets) rather than silently skipping a '
            'claimed capability')
    vm.add_args(*prom_args(firmware_path))

    try:
        vm.launch()
    except Exception as exc:  # pylint: disable=broad-except
        # Any failure to launch at all -- missing asset, a crash before
        # QMP came up, and so on -- is infrastructure, never a quiet
        # reached_pass=False: verdict() must never absorb it into an
        # ordinary fail or (worse) an xfail match.
        return AttemptResult(reached_pass=False, infra_error=str(exc))

    log_path = test.log_file(f'{name}.log')
    result = _wait_for_pass(test, vm, cell, log_path)
    # A VM that crashed mid-boot has already exited on its own. A
    # graceful shutdown() against an already-dead process still raises
    # AbnormalShutdown even though qemu is in fact gone -- uncaught,
    # that would abort test_cell() before a multi-attempt cell's later
    # attempts run, defeating the retry loop on exactly the crash path
    # retries exist for. hard=True skips the graceful attempt instead.
    vm.shutdown(hard=vm.exitcode() is not None)
    return result


def _wait_for_pass(test, vm, cell, log_path):
    """Poll for cell's pass condition for up to cell.timeout seconds.

    lib_matrix.pass_condition_met() decides from the log text alone: the
    full recorded firmware-boot progress mark (over the QEMU process's
    own captured log, where the sgi_hub_led trace event lands) must be a
    subset of what was seen. A boot that wedges after an early code does
    not reach the mark, and times out instead.
    """
    deadline = time.monotonic() + cell.timeout
    while time.monotonic() < deadline:
        exitcode = vm.exitcode()
        if exitcode is not None:
            return AttemptResult(
                reached_pass=False,
                infra_error=f'qemu exited early with code {exitcode}')

        if os.path.exists(log_path):
            with open(log_path, encoding='utf-8', errors='replace') as fobj:
                log_text = fobj.read()
            if pass_condition_met(cell.scenario, log_text):
                return AttemptResult(reached_pass=True,
                                     signature='progress-mark')

        time.sleep(POLL_INTERVAL_SECONDS)

    return AttemptResult(reached_pass=False, signature='timeout')


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

        attempts = [_boot_one_attempt(self, cell, i)
                   for i in range(cell.attempts)]

        outcome = verdict(cell, attempts)

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
    each copy names, and every copy fails.
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


class OriginMatrix(QemuSystemTest):
    """One test method per quick row of origin200-matrix. See _make()
    above for what each one does; the manifest, not this class, is the
    source of truth for what cells exist."""


register_matrix_cells(OriginMatrix, 'quick')


if __name__ == '__main__':
    QemuSystemTest.main()
