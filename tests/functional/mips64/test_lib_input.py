# Tests for the scripted keyboard/mouse input helper
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_input.py`. _split_delta() is
pure logic and gets exercised directly; send_key()/click_mouse()/
move_mouse()/run_script() are exercised against a FakeVm stub that
records the HMP command lines it was given, rather than a real VM.
"""

import unittest
from unittest.mock import patch

from lib_input import (MOUSE_MOVE_STEP_MAX, _split_delta, click_mouse,
                       move_mouse, run_script, send_key)


class SplitDeltaTest(unittest.TestCase):

    def test_zero_delta_is_no_steps(self):
        self.assertEqual(_split_delta(0, 0, 500), [])

    def test_delta_within_one_step_is_a_single_step(self):
        self.assertEqual(_split_delta(300, -100, 500), [(300, -100)])

    def test_large_delta_is_split_into_bounded_steps(self):
        steps = _split_delta(1800, -1300, 500)

        for sx, sy in steps:
            self.assertLessEqual(abs(sx), 500)
            self.assertLessEqual(abs(sy), 500)
        self.assertEqual((sum(sx for sx, _ in steps),
                          sum(sy for _, sy in steps)), (1800, -1300))

    def test_single_axis_move_does_not_add_useless_steps(self):
        # Only y is large; x is zero throughout and must stay so, not
        # gain steps it does not need.
        steps = _split_delta(0, 2000, 500)

        self.assertTrue(all(sx == 0 for sx, _sy in steps))
        self.assertEqual(sum(sy for _sx, sy in steps), 2000)

    def test_default_step_matches_the_documented_quirk_fix(self):
        # MOUSE_MOVE_STEP_MAX is the constant move_mouse()'s default
        # bounds every step by; this pins it against the value the RAD4
        # investigation (docs/datasheets/psitech-rad4.md in the Origami
        # checkout) found reliable, so a future edit that loosens it
        # fails a test instead of silently drifting.
        self.assertEqual(MOUSE_MOVE_STEP_MAX, 500)


class FakeVm:
    """Records every HMP command line it is given, instead of running
    one."""

    def __init__(self):
        self.command_lines = []

    def cmd(self, name, **kwargs):
        assert name == 'human-monitor-command'
        self.command_lines.append(kwargs['command_line'])


class SendKeyClickTest(unittest.TestCase):

    def test_send_key(self):
        vm = FakeVm()

        send_key(vm, 'shift')

        self.assertEqual(vm.command_lines, ['sendkey shift'])

    @patch('lib_input.time.sleep')
    def test_click_mouse_presses_then_releases(self, _sleep):
        vm = FakeVm()

        click_mouse(vm, button=1)

        self.assertEqual(vm.command_lines,
                         ['mouse_button 1', 'mouse_button 0'])


class MoveMouseTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_move_mouse_sends_one_command_per_step(self, _sleep):
        vm = FakeVm()

        move_mouse(vm, 1800, -1300, step=500, settle_seconds=0)

        self.assertEqual(len(vm.command_lines), 4)
        for line in vm.command_lines:
            self.assertTrue(line.startswith('mouse_move '))

    @patch('lib_input.time.sleep')
    def test_move_mouse_settles_between_steps(self, sleep_mock):
        vm = FakeVm()

        move_mouse(vm, 1800, -1300, step=500, settle_seconds=0.2)

        self.assertEqual(sleep_mock.call_count, 4)
        sleep_mock.assert_called_with(0.2)


class RunScriptTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_runs_every_step_kind_in_order(self, _sleep):
        vm = FakeVm()

        run_script(vm, [
            ('key', 'shift'),
            ('click', 1),
            ('move', 100, 0),
            ('wait', 0),
        ])

        self.assertEqual(vm.command_lines, [
            'sendkey shift',
            'mouse_button 1',
            'mouse_button 0',
            'mouse_move 100 0',
        ])

    def test_unknown_step_kind_raises(self):
        vm = FakeVm()

        with self.assertRaises(ValueError):
            run_script(vm, [('spin', 1)])


if __name__ == '__main__':
    unittest.main()
