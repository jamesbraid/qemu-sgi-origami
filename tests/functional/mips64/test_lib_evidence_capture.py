# Tests for the generic graphics-device evidence-capture sequence
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_evidence_capture.py`. Every
function in lib_evidence_capture.py only ever calls lib_input.py's own
helpers against a vm, so these tests exercise the ORDER and ARGUMENTS of
those calls against a FakeVm stub (the same pattern test_lib_input.py
uses), not real mouse/keyboard delivery -- that is lib_input.py's own
job, already covered by test_lib_input.py.
"""

import unittest
from unittest.mock import patch

import lib_evidence_capture as evidence


class FakeVm:
    """Records every HMP command line and every other QMP command this
    module's functions send, instead of running one."""

    def __init__(self):
        self.command_lines = []
        self.other_commands = []

    def cmd(self, name, **kwargs):
        if name == 'human-monitor-command':
            self.command_lines.append(kwargs['command_line'])
        else:
            self.other_commands.append((name, kwargs))


class LoginTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_types_username_then_password_with_returns_between(self, _sleep):
        vm = FakeVm()

        evidence.login(vm, 'guest', 'Captur01')

        joined = ' '.join(vm.command_lines)
        username_end = joined.index('sendkey ret')
        # The username's own keys appear before the first Return, the
        # password's keys (going through shift-c for the capital C)
        # appear after it.
        self.assertIn('sendkey g', joined[:username_end])
        self.assertIn('sendkey shift-c', joined[username_end:])
        self.assertEqual(vm.command_lines.count('sendkey ret'), 2)

    @patch('lib_input.type_text')
    @patch('lib_input.time.sleep')
    def test_forwards_key_settle_to_both_fields(self, _sleep, type_text):
        vm = FakeVm()

        evidence.login(vm, 'guest', 'Captur01', key_settle_seconds=0.25)

        self.assertEqual(type_text.call_args_list, [
            unittest.mock.call(vm, 'guest', key_settle_seconds=0.25),
            unittest.mock.call(vm, 'Captur01', key_settle_seconds=0.25),
        ])


class OpenConsoleTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_double_clicks_the_console_icon(self, _sleep):
        vm = FakeVm()

        evidence.open_console(vm)

        # double_click_at() moves to the icon, then clicks it twice.
        self.assertEqual(vm.command_lines.count('mouse_button 1'), 2)
        self.assertEqual(vm.command_lines.count('mouse_button 0'), 2)


class ResizeConsoleTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_drags_width_then_height(self, _sleep):
        vm = FakeVm()

        evidence.resize_console(vm)

        press_indices = [i for i, line in enumerate(vm.command_lines)
                         if line == 'mouse_button 1']
        release_indices = [i for i, line in enumerate(vm.command_lines)
                           if line == 'mouse_button 0']
        self.assertEqual(len(press_indices), 2)
        self.assertEqual(len(release_indices), 2)
        # Within the first held drag (width), only horizontal moves
        # happen; within the second (height), only vertical ones.
        first_drag = vm.command_lines[press_indices[0] + 1:release_indices[0]]
        second_drag = vm.command_lines[press_indices[1] + 1:release_indices[1]]
        for line in first_drag:
            _cmd, _sx, sy = line.split()
            self.assertEqual(int(sy), 0)
        for line in second_drag:
            _cmd, sx, _sy = line.split()
            self.assertEqual(int(sx), 0)

    @patch('lib_input.time.sleep')
    def test_deltas_are_overridable(self, _sleep):
        vm = FakeVm()

        evidence.resize_console(vm, width_delta=100, height_delta=-50)

        joined = ' '.join(vm.command_lines)
        # The overridden deltas are what gets split into steps, not the
        # module's own defaults.
        self.assertNotIn(str(evidence.CONSOLE_WIDTH_DELTA), joined)


class RunCommandTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_focuses_types_and_returns(self, _sleep):
        vm = FakeVm()

        evidence.run_command(vm, 'hinv -v')

        self.assertEqual(vm.command_lines[-1], 'sendkey ret')
        joined = ' '.join(vm.command_lines)
        self.assertIn('sendkey h', joined)
        self.assertIn('sendkey minus', joined)

    @patch('lib_input.type_text')
    @patch('lib_input.time.sleep')
    def test_forwards_key_settle(self, _sleep, type_text):
        vm = FakeVm()

        evidence.run_command(vm, 'hinv -v', key_settle_seconds=0.25)

        type_text.assert_called_once_with(
            vm, 'hinv -v', key_settle_seconds=0.25)


class ScreendumpTest(unittest.TestCase):

    def test_sends_a_screendump_command(self):
        vm = FakeVm()

        evidence.screendump(vm, '/tmp/out.ppm')

        self.assertEqual(vm.other_commands,
                         [('screendump', {'filename': '/tmp/out.ppm'})])

    def test_passes_an_explicit_image_format(self):
        vm = FakeVm()

        evidence.screendump(vm, '/tmp/out.png', image_format='png')

        self.assertEqual(vm.other_commands, [
            ('screendump', {'filename': '/tmp/out.png', 'format': 'png'}),
        ])


class CaptureEvidenceTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_runs_every_stage_in_order_and_screendumps_last(self, _sleep):
        vm = FakeVm()

        evidence.capture_evidence(vm, username='guest', password='x',
                                  command='hinv -v', out_path='/tmp/out.ppm')

        # Two Returns from login(), one from run_command().
        self.assertEqual(vm.command_lines.count('sendkey ret'), 3)
        self.assertEqual(vm.other_commands,
                         [('screendump', {'filename': '/tmp/out.ppm'})])

    @patch('lib_input.type_text')
    @patch('lib_input.time.sleep')
    def test_forwards_key_settle_to_login_and_command(self, _sleep, type_text):
        vm = FakeVm()

        evidence.capture_evidence(
            vm, username='guest', password='x', command='hinv -v',
            out_path='/tmp/out.ppm', key_settle_seconds=0.25)

        self.assertEqual(type_text.call_args_list, [
            unittest.mock.call(vm, 'guest', key_settle_seconds=0.25),
            unittest.mock.call(vm, 'x', key_settle_seconds=0.25),
            unittest.mock.call(vm, 'hinv -v', key_settle_seconds=0.25),
        ])


if __name__ == '__main__':
    unittest.main()
