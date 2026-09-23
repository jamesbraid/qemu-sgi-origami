#!/usr/bin/env python3
#
# The graphics device test matrix: one boot-drive-screendump test per
# manifest cell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
One test_<id> method per row of graphics-matrix whose speed column says
quick, attached to GraphicsMatrix at import time by
register_graphics_matrix_cells(); test_graphics_matrix_thorough.py attaches
the thorough rows to its own class the same way, mirroring
test_origin200_matrix.py's own quick/thorough split exactly (see that
file's docstring). Each method parses and validates its own row when it
runs, so a bad row fails that one test instead of taking the whole script
down at collection.

This is the sibling matrix origin200-matrix has no room for: a graphics
device's pass condition is "does the screen look right", not "did a
console-text progress mark appear" (see lib_graphics_matrix.py's own
docstring for why that needed a second manifest and Cell shape rather than
new columns on the existing one). What is shared -- retry/attempts/
min_passes, timeout handling, and the pass/fail/xfail/xpass/error verdict
-- is imported from lib_matrix and lib_graphics_matrix unchanged; only the
boot-drive-screendump attempt body below is new.
"""

import os
import re
import time

from qemu_test import QemuSystemTest

from lib_matrix import (AttemptResult, MatrixError, matrix_rows, qemu_args,
                        verdict)
from lib_graphics_matrix import (SCENARIOS, disk_rel, peek_row, parse_row)
from lib_input import run_script
from lib_prom import FIRMWARE_REL, drive_console, resolve_asset
from lib_screen import (RegionColorCheck, ScreenCheckError,
                        evaluate_screendump, read_ppm, region_average)


MATRIX_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'graphics-matrix')

# The System Maintenance Menu's "Start System" option -- the one PROM
# dialogue step every graphics-matrix cell needs before its scenario's own
# console_wait can ever match, the same "Option?" prompt
# test_irix_diag.py's own DRIVER_STEPS answers the same way.
_START_SYSTEM_STEPS = ((r"Option\?", "1\n"),)

# A graphics scenario's console_wait (IRIX's login(1) prompt, for every
# scenario currently registered) and its own X server's redraw are two
# independent processes -- the serial getty and Xradsgi start from the
# same multiuser boot but race each other, not synchronized by anything
# this test can see on the console. Measured live while building this
# test: a screendump taken immediately (under ~1.3s post-match: one
# short input script's worth of HMP round trips) can still show only the
# root window's stipple pattern, not yet the rendered login dialog, on a
# run where the same manifest cell reliably shows the dialog complete a
# few seconds later.
#
# A fixed sleep before the one screendump this test used to take (3.0s,
# then 15.0s/30.0s once 3.0s started reproducing rad4_login_screen's own
# failure under host load -- both still just a conservative margin around
# an observed measurement, not a derived bound, and both paying that
# margin in full even on a host fast enough to have redrawn already)
# guesses at a number instead of asking the framebuffer whether it is
# still changing. _poll_screendump_until_settled() below asks instead:
# take a screendump every _POLL_INTERVAL_SECONDS, for up to
# _POLL_TIMEOUT_SECONDS / _POLL_INTERVAL_SECONDS attempts, and stop as
# soon as the scenario's checked region (its RegionColorCheck's own rect
# and expected range) reads inside that range. Each attempt's own
# screendump-and-read cost is on top of the interval, so the real
# wall-clock ceiling runs somewhat past the nominal
# _POLL_TIMEOUT_SECONDS -- confirmed live, where the multi-CPU cell (see
# below) needed 11 of its 15 allowed attempts and 21.6s wall-clock to
# settle. That is still well inside the old 30s fixed sleep multi-CPU
# once needed, and, unlike a fixed sleep, is paid only when the guest is
# actually still redrawing. One mechanism and one timeout constant for
# every cpu count: the former single-CPU/multi-CPU split existed only
# because the two fixed sleeps were measured separately, not because
# settling itself depends on cpu count.
#
# A same-value-twice stability rule (settle once the mean stops changing
# between two consecutive dumps, without needing to know the target
# color) was tried first and live testing disproved it: a poll instrumented
# to log every attempt's mean showed attempt 1 (0.02s after console_wait
# matched) reading a transitional frame -- (45.7, 45.7, 45.7) in one
# capture -- then attempts 2 and 3 both reading (63.75, 63.75, 63.75), the
# root window's own unpainted stipple average, not yet xdm's redrawn
# dialog. Two consecutive dumps agreeing is true there at 2.4s, well
# before the real redraw, because the stipple pattern is itself perfectly
# static; bare equality cannot tell "hasn't redrawn yet" from "finished
# redrawing" when both are static. There is no way to tell those apart by
# watching this one rectangle's own history alone -- only reaching the
# color the redraw is known to produce says the redraw happened, so that
# is the sole early-exit condition below. A region that never reaches its
# expected range within the timeout still exhausts every attempt and
# raises, carrying the last two observed means for diagnosis.
_POLL_INTERVAL_SECONDS = 1.0
_POLL_TIMEOUT_SECONDS = 15.0


def _checked_region(screen_checks):
    """The first RegionColorCheck's rect and (min_rgb, max_rgb) among
    screen_checks, or (None, None) if the scenario has no region check --
    NotAllBlackCheck alone gives the poll below no smaller-than-whole-
    frame signal to judge stability from."""
    for check in screen_checks:
        if isinstance(check, RegionColorCheck):
            return check.rect, (check.min_rgb, check.max_rgb)
    return None, None


def _poll_screendump_until_settled(test, vm, screendump_path, screen_checks):
    """Take a screendump every _POLL_INTERVAL_SECONDS, for up to
    _POLL_TIMEOUT_SECONDS / _POLL_INTERVAL_SECONDS attempts, replacing the
    fixed pre-dump sleep this file used to guess with (see the comment
    above). Stops as soon as the
    checked region falls inside its RegionColorCheck's expected range --
    the only signal live testing found that actually distinguishes "xdm
    has redrawn" from "still showing the root window's own static stipple
    pattern" (see the comment above _POLL_INTERVAL_SECONDS).

    A scenario with no RegionColorCheck (NotAllBlackCheck alone) has no
    region or expected range to poll for, so it gets a single dump:
    whether the whole frame is non-black doesn't change with a further
    wait once the guest has painted anything.

    Logs the observed settle time via test.log.info, matching
    replay_kernel.py's own 'elapsed time %.2f sec' convention, so a live
    run's actual timing is on record rather than only asserted in a commit
    message. Raises ScreenCheckError, with the last two observed means and
    the elapsed time, if the checked region never reaches its expected
    range before the timeout.
    """
    rect, expected_range = _checked_region(screen_checks)
    if rect is None:
        vm.cmd('screendump', filename=screendump_path)
        return

    min_rgb, max_rgb = expected_range
    start = time.time()
    attempts = int(_POLL_TIMEOUT_SECONDS / _POLL_INTERVAL_SECONDS)
    last_two = []
    for attempt in range(1, attempts + 1):
        vm.cmd('screendump', filename=screendump_path)
        width, _height, data = read_ppm(screendump_path)
        mean = region_average(width, data, rect)
        last_two = (last_two + [mean])[-2:]
        elapsed = time.time() - start
        if all(lo <= value <= hi for value, lo, hi in
              zip(mean, min_rgb, max_rgb)):
            test.log.info('region %r settled (in expected range) after '
                          '%.2fs (%d dump(s))', rect, elapsed, attempt)
            return
        if attempt < attempts:
            time.sleep(_POLL_INTERVAL_SECONDS)
    elapsed = time.time() - start
    raise ScreenCheckError(
        f'region {rect!r} did not reach its expected range within '
        f'{_POLL_TIMEOUT_SECONDS}s ({elapsed:.2f}s elapsed): last two '
        f'means {last_two}')


def _boot_one_attempt(test, cell, attempt_index):
    """Launch one boot attempt for cell, drive it to its scenario's
    console-wait point, run the scenario's input script, take a
    screendump, and return the resulting AttemptResult.

    A VM name unique to this attempt keeps each attempt's own captured
    console log and screendump isolated from every other attempt of the
    same cell, matching test_origin200_matrix.py's own reasoning for the
    same naming.
    """
    scenario = SCENARIOS[cell.scenario]
    name = f'{cell.id}_attempt{attempt_index}'

    assets_root = os.environ.get('ORIGAMI_ASSETS')
    if not assets_root:
        test.skipTest(
            'ORIGAMI_ASSETS is not set -- point it at a checkout with the '
            'real PROM image and the named IRIX disk variant to run this '
            'test')
    firmware_path = resolve_asset(assets_root, FIRMWARE_REL)
    disk_path = resolve_asset(assets_root, disk_rel(cell.disk))
    if not (firmware_path and disk_path):
        # ORIGAMI_ASSETS IS set, so either missing file is a provisioning
        # failure, not an absent-assets developer run -- fail loudly
        # rather than silently skip a claimed capability, matching
        # test_irix_diag.py's own reasoning for the same distinction.
        test.fail(
            'ORIGAMI_ASSETS is set but the PROM or disk is missing: '
            f'need <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
            f'<ORIGAMI_ASSETS>/{disk_rel(cell.disk)} -- provision them '
            'rather than silently skipping a claimed capability')

    vm = test.get_vm(name=name)
    # synthesized-firmware was a real origin200-machine property once, but
    # QEMU commit bd935b711b ("hw/mips/sgi/sn0: delete the node's
    # synthesized-firmware hooks") -- an ancestor of every commit on this
    # branch -- deleted it from the class entirely; tools/origami's own
    # irix.py harness already treats it as gone
    # (machines.py's has_synthesized_firmware_toggle is false for
    # origin200), so naming it here just fails machine creation outright.
    vm.set_machine('origin200,debug-leds=off')
    vm.set_console()
    vm.add_args(*qemu_args(cell),
               '-cpu', 'R10000',
               '-bios', str(firmware_path),
               '-audio', 'none',
               '-drive',
               f'if=none,id=disk0,file={disk_path},format=qcow2,'
               'snapshot=on',
               '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
               '-device', cell.device,
               '-nic', 'user,id=net0')

    try:
        vm.launch()
    except Exception as exc:  # pylint: disable=broad-except
        # Matches test_origin200_matrix.py's own reasoning: a launch
        # failure is infrastructure, never a quiet reached_pass=False.
        return AttemptResult(reached_pass=False, infra_error=str(exc))

    log_path = test.log_file(f'{name}.log')
    try:
        _transcript, reached = drive_console(
            vm.console_socket, log_path, cell.timeout, _START_SYSTEM_STEPS,
            until=re.compile(scenario.console_wait), vm=vm)
        if not reached:
            return AttemptResult(reached_pass=False, signature='timeout')

        run_script(vm, scenario.input_steps)

        screendump_path = test.scratch_file(f'{name}.ppm')
        try:
            _poll_screendump_until_settled(test, vm, screendump_path,
                                           scenario.screen_checks)
        except ScreenCheckError as exc:
            return AttemptResult(reached_pass=False, signature=str(exc))
        passed, messages = evaluate_screendump(screendump_path,
                                               scenario.screen_checks)
        return AttemptResult(reached_pass=passed,
                             signature=None if passed else '; '.join(messages))
    finally:
        vm.shutdown(hard=vm.exitcode() is not None)


def _make(lineno, line, problem):
    """Build the test method for one manifest row. Mirrors
    test_origin200_matrix.py's own _make(): the row is parsed inside the
    method, not here, so a bad row fails only its own test."""

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

    test_cell.__doc__ = f'graphics matrix row {MATRIX_PATH}:{lineno}: {line}'
    return test_cell


def register_graphics_matrix_cells(cls, speed):
    """Attach one test method to cls per manifest row of the given speed.
    Mirrors test_origin200_matrix.py's register_matrix_cells() exactly."""
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


class GraphicsMatrix(QemuSystemTest):
    """One test method per quick row of graphics-matrix. See _make() above
    for what each one does; the manifest, not this class, is the source of
    truth for what cells exist."""


register_graphics_matrix_cells(GraphicsMatrix, 'quick')


if __name__ == '__main__':
    QemuSystemTest.main()
