# Tests for the graphics matrix manifest parser and scenario registry
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_graphics_matrix.py`, the same
way test_lib_matrix.py runs.
"""

import logging
import socket
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from lib_graphics_matrix import (GUEST_STEP_KINDS, SCENARIOS,
                                 GraphicsMatrixError, disk_rel, parse_row,
                                 peek_row, run_steps)


STARTER_MANIFEST = Path(__file__).parent / 'graphics-matrix'

FIELDS = ('id', 'guest', 'device', 'disk', 'scenario', 'cpus', 'mem',
          'accel', 'speed', 'timeout', 'attempts', 'min_passes',
          'expect', 'xfail_mark', 'xfail_signature')

BASE_ROW = {
    'id': 'sample_cell',
    'guest': 'irix',
    'device': 'psitech-rad4,addr=5',
    'disk': 'root-rad4',
    'scenario': 'rad4_login_screen',
    'cpus': '1',
    'mem': '64M',
    'accel': 'wallclock',
    'speed': 'quick',
    'timeout': '120',
    'attempts': '1',
    'min_passes': '1',
    'expect': 'pass',
    'xfail_mark': '',
    'xfail_signature': '',
}


def make_row(**overrides):
    values = dict(BASE_ROW, **overrides)
    return ' | '.join(values[field] for field in FIELDS)


class StarterManifestTest(unittest.TestCase):

    def _cells_by_id(self):
        with open(STARTER_MANIFEST, encoding='utf-8') as fobj:
            lines = [line.strip() for line in fobj
                     if line.strip() and not line.strip().startswith('#')]

        cells = [parse_row(STARTER_MANIFEST, lineno, line)
                 for lineno, line in enumerate(lines, start=2)]
        return {cell.id: cell for cell in cells}

    def test_parses_the_rad4_cell(self):
        cell = self._cells_by_id()['rad4_login_screen']

        self.assertEqual(cell.guest, 'irix')
        self.assertEqual(cell.device, 'psitech-rad4,addr=5')
        self.assertEqual(cell.disk, 'root-rad4')
        self.assertEqual(cell.scenario, 'rad4_login_screen')
        self.assertEqual(cell.cpus, 1)
        self.assertEqual(cell.expect, 'pass')
        self.assertEqual(cell.nodes, 1)

    def test_parses_the_dual_cpu_rad4_cell(self):
        # Same scenario and disk as the single-CPU cell above -- only the
        # CPU count differs, so this cell exercises the same login-screen
        # scenario against origin200's own 2-CPU population.
        cell = self._cells_by_id()['rad4_login_screen_smp2']

        self.assertEqual(cell.guest, 'irix')
        self.assertEqual(cell.device, 'psitech-rad4,addr=5')
        self.assertEqual(cell.disk, 'root-rad4')
        self.assertEqual(cell.scenario, 'rad4_login_screen')
        self.assertEqual(cell.cpus, 2)
        self.assertEqual(cell.expect, 'pass')
        self.assertEqual(cell.nodes, 1)


class ParseRowTest(unittest.TestCase):

    def test_parses_a_good_row(self):
        cell = parse_row('matrix', 5, make_row())

        self.assertEqual(cell.id, 'sample_cell')
        self.assertEqual(cell.cpus, 1)
        self.assertEqual(cell.nodes, 1)

    def test_rejects_wrong_field_count(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, 'a | b | c')

    def test_rejects_bad_guest(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(guest='linux'))

    def test_rejects_empty_device(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(device=''))

    def test_rejects_empty_disk(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(disk=''))

    def test_rejects_unknown_scenario(self):
        with self.assertRaisesRegex(GraphicsMatrixError, 'SCENARIOS'):
            parse_row('matrix', 1, make_row(scenario='no_such_scenario'))

    def test_rejects_bad_speed(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(speed='fast'))

    def test_rejects_bad_accel(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(accel='kvm'))

    def test_rejects_xfail_without_signature(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(expect='xfail'))

    def test_accepts_xfail_with_signature(self):
        cell = parse_row('matrix', 1, make_row(
            expect='xfail', xfail_mark='screen',
            xfail_signature='black-screen'))

        self.assertEqual(cell.expect, 'xfail')


class PeekRowTest(unittest.TestCase):

    def test_reads_id_and_speed(self):
        self.assertEqual(
            peek_row(make_row(id='a_cell', speed='thorough')),
            ('a_cell', 'thorough'))

    def test_blanks_what_it_cannot_use(self):
        self.assertEqual(peek_row('a | b | c'), (None, None))


class ScenarioRegistryTest(unittest.TestCase):

    def test_rad4_login_screen_is_registered(self):
        self.assertIn('rad4_login_screen', SCENARIOS)

    def test_rad4_login_screen_has_a_not_all_black_check(self):
        scenario = SCENARIOS['rad4_login_screen']
        kinds = [type(check).__name__ for check in scenario.screen_checks]

        self.assertIn('NotAllBlackCheck', kinds)
        self.assertIn('RegionColorCheck', kinds)


# The step kinds lib_input.run_script() itself handles.
INPUT_STEP_KINDS = {'key', 'click', 'move', 'type', 'wait'}

# One scenario per manifest row, so a registered scenario no row names
# is dead data and a row naming none is already rejected by parse_row().
NEW_RAD4_SCENARIOS = ('rad4_window_menu', 'rad4_desktop_menu',
                      'rad4_mode_switch', 'rad4_buffered_mode')


class DesktopScenarioRegistryTest(unittest.TestCase):

    def test_every_scenario_is_in_the_manifest(self):
        with open(STARTER_MANIFEST, encoding='utf-8') as fobj:
            used = {line.split('|')[4].strip() for line in fobj
                    if line.strip() and not line.startswith('#')}

        self.assertEqual(set(SCENARIOS), used)

    def test_every_step_kind_is_known(self):
        for name in NEW_RAD4_SCENARIOS:
            for step in SCENARIOS[name].input_steps:
                self.assertIn(step[0], GUEST_STEP_KINDS | INPUT_STEP_KINDS,
                              f'{name}: {step!r}')

    def test_every_scenario_ends_in_screen_checks(self):
        for name in NEW_RAD4_SCENARIOS:
            checks = SCENARIOS[name].screen_checks
            self.assertTrue(checks, name)
            self.assertTrue(all(hasattr(check, 'evaluate')
                                for check in checks), name)

    def test_mode_switch_asserts_the_frame_size(self):
        kinds = [type(check).__name__
                 for check in SCENARIOS['rad4_mode_switch'].screen_checks]

        self.assertIn('ImageSizeCheck', kinds)


def paint(frame, width, rect, rgb):
    x, y, rw, rh = rect
    for row in range(y, y + rh):
        offset = (row * width + x) * 3
        frame[offset:offset + rw * 3] = bytes(rgb) * rw


def evaluate(scenario_name, width, height, frame):
    """Each check of a scenario against a synthetic frame, by check name
    (or class name for the checks that have none)."""
    results = {}
    for check in SCENARIOS[scenario_name].screen_checks:
        ok, _message = check.evaluate(width, height, bytes(frame))
        results[getattr(check, 'name', type(check).__name__)] = ok
    return results


def stipple(frame, width, rect, fraction):
    """Overwrite about `fraction` of rect with black, evenly spread."""
    x, y, rw, rh = rect
    total = rw * rh
    count = round(total * fraction)
    for index in range(count):
        pos = index * total // count
        px, py = x + pos % rw, y + pos // rw
        offset = (py * width + px) * 3
        frame[offset:offset + 3] = b'\0\0\0'


class WindowMenuChecksTest(unittest.TestCase):
    """The thresholds against frames built from the measured fractions:
    the greyed row's black dots were 0.125 and 0.114 with the model's
    index compatibility on, 0.067 and 0.061 with it off."""

    WIDTH, HEIGHT = 1280, 1024

    def frame(self, label_fraction, key_fraction):
        frame = bytearray(bytes((135, 170, 202)) * self.WIDTH * self.HEIGHT)
        paint(frame, self.WIDTH, (626, 594, 134, 192), (217, 217, 217))
        # The real text starts on the third row of the measured rectangle
        # (rows 599-609, with the descender dots below), so keep the top
        # strip flat and scale the fraction to the rows that carry ink.
        scale = 14 / 11
        stipple(frame, self.WIDTH, (630, 599, 60, 11), label_fraction * scale)
        stipple(frame, self.WIDTH, (700, 599, 55, 11), key_fraction * scale)
        # Text in the lower body, so its average lands where the real
        # one does (193): about 11% black.
        stipple(frame, self.WIDTH, (626, 616, 134, 170), 0.11)
        return frame

    def test_the_measured_good_frame_passes_every_check(self):
        results = evaluate('rad4_window_menu', self.WIDTH, self.HEIGHT,
                           self.frame(0.125, 0.114))

        self.assertEqual([name for name, ok in results.items() if not ok],
                         [])

    def test_the_compat_off_frame_fails_only_the_dot_checks(self):
        results = evaluate('rad4_window_menu', self.WIDTH, self.HEIGHT,
                           self.frame(0.067, 0.061))

        self.assertEqual(sorted(name for name, ok in results.items()
                                if not ok),
                         ['window-menu-restore-accelerator',
                          'window-menu-restore-label'])

    def test_an_undrawn_menu_fails_the_grey_checks(self):
        frame = bytearray(bytes((135, 170, 202)) * self.WIDTH * self.HEIGHT)

        results = evaluate('rad4_window_menu', self.WIDTH, self.HEIGHT,
                           frame)

        self.assertFalse(results['window-menu-top-strip'])
        self.assertFalse(results['window-menu-body'])


class DesktopMenuChecksTest(unittest.TestCase):
    WIDTH, HEIGHT = 1280, 1024

    def frame(self, dots):
        frame = bytearray(bytes((105, 181, 181)) * self.WIDTH * self.HEIGHT)
        paint(frame, self.WIDTH, (402, 402, 174, 376), (217, 217, 217))
        if dots:
            stipple(frame, self.WIDTH, (406, 462, 60, 20), 0.063)
            stipple(frame, self.WIDTH, (406, 606, 150, 20), 0.092)
        return frame

    def test_black_dots_on_the_menu_grey_pass(self):
        results = evaluate('rad4_desktop_menu', self.WIDTH, self.HEIGHT,
                           self.frame(dots=True))

        self.assertEqual([name for name, ok in results.items() if not ok],
                         [])

    def test_disabled_rows_without_black_fail_only_the_row_checks(self):
        results = evaluate('rad4_desktop_menu', self.WIDTH, self.HEIGHT,
                           self.frame(dots=False))

        self.assertEqual(sorted(name for name, ok in results.items()
                                if not ok),
                         ['desktop-menu-open-label',
                          'desktop-menu-share-label'])


class FakeConsole:
    """A console socket that answers what is sent through `reply`."""

    def __init__(self, reply):
        self.reply = reply
        self.sent = []
        self.pending = []

    def settimeout(self, _seconds):
        pass

    def sendall(self, data):
        text = data.decode()
        self.sent.append(text)
        # A real console delivers a prompt in a later read than the line
        # before it, and drive_console forgets what it matched a rule
        # against, so hand each reply back as separate reads.
        for chunk in self.reply(text):
            self.pending.extend(chunk.splitlines(keepends=True))

    def recv(self, _size):
        if not self.pending:
            raise socket.timeout
        return self.pending.pop(0)


class FakeVm:
    def __init__(self, console=None):
        self.console_socket = console
        self.commands = []

    def cmd(self, name, **kwargs):
        self.commands.append((name, kwargs))


class FakeTest:
    def __init__(self, directory):
        self.directory = directory

    def log_file(self, name):
        return str(Path(self.directory, name))

    @staticmethod
    def fail(message):
        raise AssertionError(message)


class RunStepsTest(unittest.TestCase):

    def run_steps(self, steps, reply=None, vm=None):
        with tempfile.TemporaryDirectory() as directory:
            vm = vm or FakeVm(FakeConsole(reply or (lambda _text: [])))
            run_steps(FakeTest(directory), vm, 'cell', steps)
            return vm

    def test_button_holds_the_mask_through_hmp(self):
        vm = self.run_steps([('button', 2), ('button', 0)])

        self.assertEqual(
            [kwargs['command_line'] for _name, kwargs in vm.commands],
            ['mouse_button 2', 'mouse_button 0'])

    def test_screendump_saves_into_the_log_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            vm = FakeVm()
            run_steps(FakeTest(directory), vm, 'cell',
                      [('screendump', 'menu')])

        self.assertEqual(vm.commands[0][0], 'screendump')
        self.assertEqual(vm.commands[0][1]['filename'],
                         str(Path(directory, 'cell-menu.ppm')))

    def test_an_unknown_step_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_steps([('no_such_step',)])

    def test_shell_returns_once_the_marker_is_printed(self):
        def reply(text):
            if text.startswith('xset'):
                return [b'xset\r\nRAD4-DONE-1\r\n# ']
            return []

        vm = self.run_steps([('shell', 'xset', 5)], reply=reply)

        self.assertEqual(vm.console_socket.sent,
                         ["xset; echo RAD4-DONE-''1\n"])

    def test_shell_does_not_take_its_own_echo_for_completion(self):
        # The tty echoes the command line, which holds the marker's
        # spelling with the empty quote pair, never the marker itself.
        def reply(text):
            return [text.replace('\n', '\r\n').encode()]

        with self.assertRaisesRegex(AssertionError, 'never saw'):
            self.run_steps([('shell', 'hangs', 1)], reply=reply)

    def test_set_password_fails_when_passwd_refuses(self):
        def reply(text):
            if text.startswith('passwd'):
                return [b'New password:\r\nSorry, too short.\r\n# ']
            return []

        with self.assertRaisesRegex(AssertionError, 'refused'):
            self.run_steps([('set_password', 'guest', 'abc')], reply=reply)


WIDTH, HEIGHT = 1280, 1024
# Rightmost ink of the name field after Left, by characters held, taken
# from the measured greeter ("guest" is 5); the password caret is
# 459 + 5 * characters.
NAME_RIGHT = {0: 459, 1: 466, 2: 474, 3: 481, 4: 491, 5: 497}


class FakeGreeter:
    """A greeter as a screendump source: name field, then password field,
    each with a caret that is absent until `focus_after` screendumps have
    been taken, and an optional number of typed letters it drops."""

    def __init__(self, focus_after=0, drop=0):
        self.focus_after = focus_after
        self.drop = drop
        self.dumps = 0
        self.stage = 'name'
        self.count = 0
        self.keys = []
        self.dump_marks = []
        self.base = bytearray(bytes((211, 181, 181)) * WIDTH * HEIGHT)

    def key(self, name):
        self.keys.append(name)
        if name == 'ret' and self.stage == 'name':
            self.stage, self.count = 'password', 0
        elif name == 'backspace':
            self.count = max(0, self.count - 1)
        elif len(name) == 1 or name.startswith('shift-'):
            if self.drop:
                self.drop -= 1
            else:
                self.count += 1

    def frame(self):
        frame = bytearray(self.base)
        if self.dumps > self.focus_after:
            if self.stage == 'name':
                right, rows = NAME_RIGHT.get(self.count, 440), (700, 719)
            else:
                right, rows = 459 + 5 * self.count, (745, 765)
            for y in range(*rows):
                offset = (y * WIDTH + right) * 3
                frame[offset:offset + 3] = b'\0\0\0'
        return bytes(frame)

    def screendump(self, path):
        self.dumps += 1
        self.dump_marks.append(len(self.keys))
        with open(path, 'wb') as fobj:
            fobj.write(f'P6\n{WIDTH} {HEIGHT}\n255\n'.encode('ascii'))
            fobj.write(self.frame())


class GreeterVm:
    def __init__(self, greeter):
        self.greeter = greeter

    def cmd(self, name, **kwargs):
        if name == 'screendump':
            self.greeter.screendump(kwargs['filename'])
        elif kwargs['command_line'].startswith('sendkey '):
            self.greeter.key(kwargs['command_line'].split(' ', 1)[1])


class LoginDesktopTest(unittest.TestCase):

    def login(self, greeter, user='guest', password='origami65'):
        with tempfile.TemporaryDirectory() as directory, \
             patch('time.sleep'), patch('lib_graphics_matrix.'
                                        '_ENTRY_VERIFY_SECONDS', 0.0):
            test = FakeTest(directory)
            test.log = logging.getLogger('test')
            run_steps(test, GreeterVm(greeter), 'cell',
                      [('login_desktop', user, password)])
        return greeter

    @staticmethod
    def typed(keys):
        return ''.join(key for key in keys if len(key) == 1)

    def test_types_the_name_then_the_password(self):
        greeter = self.login(FakeGreeter())

        self.assertEqual(self.typed(greeter.keys), 'guestorigami65')
        self.assertEqual(greeter.keys.count('ret'), 2)
        self.assertEqual(greeter.keys[-1], 'ret')

    def test_looks_one_place_left_to_make_the_name_paint(self):
        greeter = self.login(FakeGreeter())

        after_name = greeter.keys[:greeter.keys.index('ret')]
        self.assertEqual(after_name[-2:], ['left', 'right'])

    def test_sends_no_key_before_the_caret_is_on_screen(self):
        greeter = self.login(FakeGreeter(focus_after=3))

        self.assertEqual(greeter.keys[0], 'g')
        self.assertTrue(all(mark == 0 for mark in greeter.dump_marks[:4]))

    def test_retypes_a_name_that_lost_a_letter(self):
        greeter = self.login(FakeGreeter(drop=1))

        self.assertEqual(greeter.keys.count('backspace'), 7)
        self.assertEqual(self.typed(greeter.keys),
                         'guest' + 'guest' + 'origami65')

    def test_fails_when_the_name_is_lost_every_time(self):
        with self.assertRaisesRegex(AssertionError, 'name field never'):
            self.login(FakeGreeter(drop=100))

    def test_retypes_a_password_that_lost_a_character(self):
        greeter = FakeGreeter()
        original = greeter.key
        letters = []

        def drop_one_password_digit(name):
            if greeter.stage == 'password' and name == '6' and not letters:
                letters.append(name)
                greeter.keys.append(name)
                return
            original(name)

        greeter.key = drop_one_password_digit
        self.login(greeter)

        self.assertEqual(greeter.keys.count('backspace'), 11)
        self.assertEqual(greeter.count, 9)

    def test_fails_when_the_password_is_lost_every_time(self):
        greeter = FakeGreeter()
        original = greeter.key

        def drop_digits(name):
            if greeter.stage == 'password' and name == '5':
                greeter.keys.append(name)
                return
            original(name)

        greeter.key = drop_digits

        with self.assertRaisesRegex(AssertionError, 'password field never'):
            self.login(greeter)

    def test_fails_when_the_caret_never_appears(self):
        with patch('lib_graphics_matrix._CARET_WAIT_SECONDS', 0.0):
            with self.assertRaisesRegex(AssertionError, 'never showed its'):
                self.login(FakeGreeter(focus_after=10 ** 6))

    def test_rejects_a_name_with_no_measured_extent(self):
        with self.assertRaises(ValueError):
            self.login(FakeGreeter(), user='demos')


class DiskRelTest(unittest.TestCase):

    def test_builds_the_sibling_asset_convention_path(self):
        # Matches test_irix_diag.py's own DISK_REL by hand
        # ('irix/6.5.30/root-diags.qcow2') for a different variant
        # name, so a new variant needs no new constant, just a new
        # manifest row naming it.
        self.assertEqual(disk_rel('root-rad4'),
                         'irix/6.5.30/root-rad4.qcow2')


if __name__ == '__main__':
    unittest.main()
