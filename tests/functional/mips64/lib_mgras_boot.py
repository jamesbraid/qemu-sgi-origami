# One IRIX boot per SI/MGRAS board: desktop, hinv line, then each feature
#
# SPDX-License-Identifier: BSD-3-Clause
"""
run_board() is the body the test_mgras_matrix_<board>.py files share. It
reuses test_mgras_desktop's launch recipe, serial login steps and
password handling, and the desktop's colour regions, and adds what the
SI-only desktop test does not do: a four-node machine, the board option,
the hinv graphics line, and one representative program per feature
checked against a colour-statistics reference (lib_mgras_matrix.py).

The machine is the Origin 2000 with four nodes and the BaseIO-G, with the
board in IO3. test_mgras_desktop boots one node; whether the four-node
topology reaches the same desktop with these disks is part of what the
matrix establishes, and has not been run yet.
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lib_input
import lib_mgras_matrix as matrix
from lib_mgras_greeter import GREETER_SCREEN_CHECKS
from lib_mgras_regions import DESKTOP_COLOR_CHECKS
from lib_prom import (FORK_ROOT, MACHINES, drive_console, run_record,
                      utc_now)
from lib_screen import (NotAllBlackCheck, ScreenCheckError,
                        poll_screendump_until_checks_pass, read_ppm)
from test_mgras_desktop import (DESKTOP_SETTLE_SECONDS,
                                FIELD_SETTLE_SECONDS,
                                GREETER_SETTLE_SECONDS, GUEST_PASSWORD,
                                GUEST_USER, POLL_INTERVAL_SECONDS,
                                SERIAL_STEPS, passwd_outcome)

__all__ = ['run_board', 'board_timeout']

MACHINE = (MACHINES['origin2000'] +
           ',nodes=4,baseio-preset=io6g,debug-leds=off')
# Two R10000s per node, as the stock rack installation has; with one per
# node the PROM cannot find its own CPU's KLCONFIG entry and four-node
# discovery times out.
SMP = '8'
# One 64 MiB bank per node, the bank size the Origin 2000 rack model
# accepts and the stock installation it models.
MEMORY_MIB = 256
SERIAL_TIMEOUT_SECONDS = 1320

COMMAND_TIMEOUT_SECONDS = 60
REQUIRE_REFERENCES_ENV = 'QEMU_TEST_MGRAS_REQUIRE_REFERENCES'

# The disk every board boots: test_mgras_desktop's root-si.qcow2 with the
# OpenGL links pointed at the MGRAS IP27 builds (without them no board gets
# a direct OpenGL context), a guest password, and the 2RSS timing table
# Xsgi needs on a two-RE board.
DISK_REL = os.path.join('irix', '6.5.30', 'root-mgras.qcow2')
DISK_SHA256 = ('c97ba11c8c639caf7f9acc35efaeb7dd'
               'd32ea25c4b50c0d1815de45884b79248')


def board_timeout(board_key):
    """The meson timeout a board's boot needs: the desktop test's own
    sequence plus each feature's settle and a command's worth of slack."""
    board = matrix.BOARDS[board_key]
    features = matrix.features_for(board)
    extra = sum(f.settle_seconds + 2 * COMMAND_TIMEOUT_SECONDS
                for f in features)
    return int(SERIAL_TIMEOUT_SECONDS + GREETER_SETTLE_SECONDS +
               DESKTOP_SETTLE_SECONDS + extra + 180)


def _serial(test, vm, name, command, marker, timeout=COMMAND_TIMEOUT_SECONDS):
    """Run COMMAND in the root shell and return the console text up to
    MARKER's output. The marker is built with a
    csh backtick so the echoed command line does not match it."""
    log_path = test.log_file(f'mgras-matrix-{name}.log')
    line = f'{command}; echo {marker[:-3]}`echo {marker[-3:]}`\n'
    # The previous command's prompt is already queued on the socket (or
    # was consumed by the login steps), so send the line directly rather
    # than waiting for a prompt that may never be seen again.
    vm.console_socket.sendall(line.encode())
    transcript, reached = drive_console(
        vm.console_socket, log_path, timeout, (),
        until=re.compile(marker.encode()), vm=None)
    if not reached:
        test.fail(f'{name}: no {marker!r} within {timeout}s -- '
                  f'console log: {log_path}')
    return transcript


def _check_hinv(test, vm, board):
    out = _serial(test, vm, 'hinv', 'hinv -v | grep -i graphics',
                  'HINV-END')
    want = re.compile(
        rf'(?m)Graphics board: {re.escape(board.hinv_name)}\s*$')
    if not want.search(out):
        test.fail(f'hinv has no "Graphics board: {board.hinv_name}" line; '
                  f'the graphics lines were: {out[-600:]!r}')


def _run_feature(test, vm, board, feature, desktop_frame):
    """Start FEATURE's program, wait for it to draw, check and stop it."""
    base = test.log_file(f'mgras-{board.option}-{feature.name}')
    _serial(test, vm, f'{feature.name}-start', feature.command, 'RUN-END')
    time.sleep(feature.settle_seconds)
    shot = base + '.ppm'
    vm.cmd('screendump', filename=shot)
    width, height, data = read_ppm(shot)

    drawn = matrix.changed_fraction(desktop_frame, (width, height, data))
    candidate = base + '.stats.json'
    matrix.write_stats(candidate, width, data)
    result = {'changed_fraction': round(drawn, 4), 'candidate': candidate}
    try:
        if NotAllBlackCheck().evaluate(width, height, data)[0] is False:
            test.fail(f'{feature.name}: the frame is all black -- {shot}')
        if drawn < matrix.MIN_CHANGED_FRACTION:
            test.fail(f'{feature.name}: {feature.command!r} left the '
                      f'desktop unchanged ({drawn:.1%} of pixels differ, '
                      f'need {matrix.MIN_CHANGED_FRACTION:.0%}) -- {shot}')
        ref_path = matrix.reference_path(board.option, feature.name)
        if os.path.exists(ref_path):
            passed, message = matrix.compare_stats(
                matrix.load_stats(ref_path), width, data)
            result['reference'] = message
            if not passed:
                test.fail(f'{feature.name} on {board.option} does not '
                          f'match {ref_path}: {message} -- {shot}')
        else:
            result['reference'] = 'missing'
            test.log.warning(
                '%s: no reference %s yet; candidate statistics are in %s',
                feature.name, ref_path, candidate)
            if os.environ.get(REQUIRE_REFERENCES_ENV):
                test.fail(f'{feature.name}: reference {ref_path} is '
                          f'missing and {REQUIRE_REFERENCES_ENV} is set')
    finally:
        # One killall per name: IRIX killall with no name signals every
        # process. The ps line shows any target that survived.
        kills = ''.join(f'killall -9 {p}; ' for p in feature.processes)
        names = '|'.join(feature.processes)
        _serial(test, vm, f'{feature.name}-stop',
                f'{kills}cat {matrix.program_log(feature.name)}; '
                f"ps -e | egrep '{names}'", 'STOP-END')
    return result


def run_board(test, board_key, prom, disk):
    """Boot IRIX on a four-node Origin 2000 with BOARD_KEY's board in IO3
    and run the whole matrix for it."""
    board = matrix.BOARDS[board_key]
    vm = test.get_vm(name=f'mgras_matrix_{board_key}')
    vm.set_machine(MACHINE)
    vm.set_console()
    vm.add_args(
        '-accel', 'tcg,thread=multi',
        '-cpu', 'R10000', '-smp', SMP, '-m', str(MEMORY_MIB),
        '-bios', str(prom),
        '-audio', 'none',
        '-drive', f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
        '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
        '-nic', 'user,id=net0',
        *matrix.device_args(board),
        '-S')

    started = utc_now()
    vm.launch()
    vm.cmd('cont')
    log_path = test.log_file('mgras-matrix.log')
    greeter_path = test.log_file('mgras-matrix-greeter.ppm')
    desktop_path = test.log_file('mgras-matrix-desktop.ppm')
    results = {}
    try:
        transcript, _reached = drive_console(
            vm.console_socket, log_path, SERIAL_TIMEOUT_SECONDS,
            SERIAL_STEPS,
            reached_when=lambda text: passwd_outcome(text) is not None,
            vm=vm)
        if not transcript:
            test.fail('the guest printed nothing at all')
        outcome = passwd_outcome(transcript.encode())
        if outcome != 'done':
            test.fail(f'setting the {GUEST_USER} password: '
                      f'{outcome or "never finished"} -- {log_path}')

        try:
            poll_screendump_until_checks_pass(
                test, vm, greeter_path, GREETER_SCREEN_CHECKS,
                interval_seconds=POLL_INTERVAL_SECONDS,
                timeout_seconds=GREETER_SETTLE_SECONDS)
        except ScreenCheckError as exc:
            test.fail(f'the greeter never showed its colours: {exc} -- '
                      f'{greeter_path}')

        lib_input.type_text(vm, GUEST_USER)
        lib_input.send_key(vm, 'ret')
        time.sleep(FIELD_SETTLE_SECONDS)
        lib_input.type_text(vm, GUEST_PASSWORD)
        lib_input.send_key(vm, 'ret')

        # 2D: the desktop.
        try:
            results['desktop'] = poll_screendump_until_checks_pass(
                test, vm, desktop_path, DESKTOP_COLOR_CHECKS,
                interval_seconds=POLL_INTERVAL_SECONDS,
                timeout_seconds=DESKTOP_SETTLE_SECONDS)
        except ScreenCheckError as exc:
            test.fail(f'the desktop never showed its colours: {exc} -- '
                      f'{desktop_path}')
        desktop_frame = read_ppm(desktop_path)

        _check_hinv(test, vm, board)
        _serial(test, vm, 'display', matrix.DISPLAY_SETUP, 'DISP-END')
        for feature in matrix.features_for(board):
            results[feature.name] = _run_feature(
                test, vm, board, feature, desktop_frame)
    finally:
        vm.shutdown(hard=vm.exitcode() is not None)
    ended = utc_now()

    with open(test.log_file('run.json'), 'w', encoding='utf-8') as out:
        json.dump({'schema': 1, 'board': board_key, 'started_utc': started,
                   'ended_utc': ended, 'results': results,
                   'identity': run_record(FORK_ROOT, test.qemu_bin,
                                          prom=prom, disk=disk)},
                  out, indent=2, sort_keys=True)
        out.write('\n')
