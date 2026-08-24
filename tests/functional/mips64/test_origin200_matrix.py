#!/usr/bin/env python3
#
# The Origin 200 test matrix: one boot-and-verdict test per manifest cell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
One test_<id> method per row of origin200-matrix, attached to OriginMatrix
at class-definition time by parse_matrix(). All per-cell configuration,
command-line construction, oracle scoring, retry and expected-failure logic
lives in lib_matrix; this file only wires it to a real boot.
"""

import os
import time

from qemu_test import QemuSystemTest

from lib_matrix import (AttemptResult, first_progress_code, machine_props,
                        parse_matrix, qemu_args, verdict)


MATRIX_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'origin200-matrix')

# How often to re-check the oracle while an attempt's boot is in flight.
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

    try:
        vm.launch()
    except Exception as exc:  # pylint: disable=broad-except
        # Any failure to launch at all -- missing asset, a crash before
        # QMP came up, and so on -- is infrastructure, never a quiet
        # reached_oracle=False: verdict() must never absorb it into an
        # ordinary fail or (worse) an xfail match.
        return AttemptResult(reached_oracle=False, infra_error=str(exc))

    log_path = test.log_file(f'{name}.log')
    result = _wait_for_oracle(test, vm, cell, log_path)
    # A VM that crashed mid-boot has already exited on its own. A
    # graceful shutdown() against an already-dead process still raises
    # AbnormalShutdown even though qemu is in fact gone -- uncaught,
    # that would abort test_cell() before a multi-attempt cell's later
    # attempts run, defeating the retry loop on exactly the crash path
    # retries exist for. hard=True skips the graceful attempt instead.
    vm.shutdown(hard=vm.exitcode() is not None)
    return result


def _wait_for_oracle(test, vm, cell, log_path):
    """Poll for cell's oracle for up to cell.timeout seconds.

    The firmware progress-code oracle (first_progress_code over the
    QEMU process's own captured log, where the sgi_hub_led trace event
    lands) applies to every cell. The 'smoke' scenario additionally
    accepts the VM simply being confirmed alive and QMP-running with
    no code at all: it exists to prove the harness end-to-end --
    enumerate, boot, oracle, verdict -- against a machine that has no
    firmware image to run and so has no code path that could ever
    write a progress indicator. A real firmware or IRIX cell's
    scenario is never 'smoke', so this fallback never weakens their
    oracle.
    """
    deadline = time.monotonic() + cell.timeout
    while time.monotonic() < deadline:
        exitcode = vm.exitcode()
        if exitcode is not None:
            return AttemptResult(
                reached_oracle=False,
                infra_error=f'qemu exited early with code {exitcode}')

        if os.path.exists(log_path):
            with open(log_path, encoding='utf-8', errors='replace') as fobj:
                code = first_progress_code(fobj.read())
            if code is not None:
                return AttemptResult(reached_oracle=True,
                                     signature=f'progress:{code}')

        if cell.scenario == 'smoke':
            try:
                status = vm.cmd('query-status')
            except Exception:  # pylint: disable=broad-except
                status = None
            if status is not None and status.get('running'):
                return AttemptResult(reached_oracle=True,
                                     signature='qmp-running')

        time.sleep(POLL_INTERVAL_SECONDS)

    return AttemptResult(reached_oracle=False, signature='timeout')


def _make(cell):
    """Build the test_<id> method for one matrix cell."""

    def test_cell(self):
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
                      f'but every attempt reached the oracle: {attempts}')
        if outcome == 'error':
            errors = [a.infra_error for a in attempts
                     if a.infra_error is not None]
            self.fail(f'{cell.id}: infrastructure error: {errors}')
        # outcome == 'fail'
        self.fail(f'{cell.id}: did not reach its oracle '
                  f'({cell.min_passes} of {cell.attempts} required): '
                  f'{attempts}')

    test_cell.__name__ = f'test_{cell.id}'
    test_cell.__doc__ = f'matrix cell {cell.id!r}: {cell.guest}/{cell.scenario}'
    return test_cell


class OriginMatrix(QemuSystemTest):
    """One test method per row of origin200-matrix. See _make() above for
    what each one does; the manifest, not this class, is the source of
    truth for what cells exist."""


def _register_matrix_cells():
    """Attach one test_<id> method to OriginMatrix per manifest row.

    A plain module-level for loop would leak its loop variable, which
    NameErrors on del if the manifest is ever empty; wrapping it in a
    function scopes that variable away and stays empty-safe by
    construction -- zero rows means zero iterations, nothing more.
    """
    for cell in parse_matrix(MATRIX_PATH):
        setattr(OriginMatrix, 'test_' + cell.id, _make(cell))


_register_matrix_cells()


if __name__ == '__main__':
    QemuSystemTest.main()
