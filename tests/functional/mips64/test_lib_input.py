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

from lib_input import (CLAMP_MOVE_DELTA, DRAG_MOVE_STEP_MAX,
                       MOUSE_MOVE_STEP_MAX, _split_delta, click_at,
                       click_mouse, double_click_at, drag_from, drag_mouse,
                       move_mouse, move_to, run_script, send_key, type_text)


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
            ('type', 'hi'),
            ('wait', 0),
        ])

        self.assertEqual(vm.command_lines, [
            'sendkey shift',
            'mouse_button 1',
            'mouse_button 0',
            'mouse_move 100 0',
            'sendkey h',
            'sendkey i',
        ])

    def test_unknown_step_kind_raises(self):
        vm = FakeVm()

        with self.assertRaises(ValueError):
            run_script(vm, [('spin', 1)])


class MoveToTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_clamps_to_the_corner_before_the_real_move(self, _sleep):
        vm = FakeVm()

        move_to(vm, 252, 44, step=500)

        # The clamp (-2000, -2000) comes first, split at the same step
        # bound, then the real move to (252, 44).
        clamp_lines = vm.command_lines[:-1]
        self.assertTrue(all(line.startswith('mouse_move -500 -500')
                            for line in clamp_lines))
        self.assertEqual(sum(1 for _ in clamp_lines),
                         -(-CLAMP_MOVE_DELTA // 500))
        self.assertEqual(vm.command_lines[-1], 'mouse_move 252 44')

    @patch('lib_input.time.sleep')
    def test_click_at_moves_then_clicks(self, _sleep):
        vm = FakeVm()

        click_at(vm, 0, 0, button=1)

        self.assertEqual(vm.command_lines[-2:],
                         ['mouse_button 1', 'mouse_button 0'])

    @patch('lib_input.time.sleep')
    def test_double_click_at_clicks_twice(self, _sleep):
        vm = FakeVm()

        double_click_at(vm, 0, 0, button=1)

        self.assertEqual(vm.command_lines[-4:], [
            'mouse_button 1', 'mouse_button 0',
            'mouse_button 1', 'mouse_button 0',
        ])


class DragMouseTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_holds_the_button_across_every_step(self, _sleep):
        vm = FakeVm()

        drag_mouse(vm, 438, 0, step=30)

        self.assertEqual(vm.command_lines[0], 'mouse_button 1')
        self.assertEqual(vm.command_lines[-1], 'mouse_button 0')
        move_lines = vm.command_lines[1:-1]
        self.assertTrue(all(line.startswith('mouse_move ')
                            for line in move_lines))
        for line in move_lines:
            _cmd, sx, _sy = line.split()
            self.assertLessEqual(abs(int(sx)), 30)

    def test_default_step_is_tighter_than_the_plain_move_default(self):
        # psitech-rad4.md's resize finding: a held-button drag needs
        # smaller steps than an ordinary cursor move.
        self.assertLess(DRAG_MOVE_STEP_MAX, MOUSE_MOVE_STEP_MAX)

    @patch('lib_input.time.sleep')
    def test_drag_from_positions_before_dragging(self, _sleep):
        vm = FakeVm()

        drag_from(vm, 683, 800, 438, 0, step=500)

        press_index = vm.command_lines.index('mouse_button 1')
        self.assertEqual(vm.command_lines[-1], 'mouse_button 0')
        # Every positioning move (the clamp, then the walk to (683,
        # 800)) happens before the button goes down; every move after
        # it is the drag itself.
        position_moves = [line for line in vm.command_lines[:press_index]
                          if line.startswith('mouse_move ')]
        drag_moves = [line for line in vm.command_lines[press_index + 1:-1]]
        self.assertTrue(all(line.startswith('mouse_move ')
                            for line in drag_moves))
        total_x = sum(int(line.split()[1]) for line in position_moves)
        total_y = sum(int(line.split()[2]) for line in position_moves)
        # The clamp (-2000, -2000) plus the walk to (683, 800).
        self.assertEqual((total_x, total_y), (683 - 2000, 800 - 2000))


class TypeTextTest(unittest.TestCase):

    @patch('lib_input.time.sleep')
    def test_types_hinv_dash_v(self, _sleep):
        vm = FakeVm()

        type_text(vm, 'hinv -v')

        self.assertEqual(vm.command_lines, [
            'sendkey h', 'sendkey i', 'sendkey n', 'sendkey v',
            'sendkey spc', 'sendkey minus', 'sendkey v',
        ])

    @patch('lib_input.time.sleep')
    def test_uppercase_goes_through_shift(self, _sleep):
        vm = FakeVm()

        type_text(vm, 'Ab1')

        self.assertEqual(vm.command_lines,
                         ['sendkey shift-a', 'sendkey b', 'sendkey 1'])

    def test_unsupported_character_raises(self):
        vm = FakeVm()

        with self.assertRaises(ValueError):
            type_text(vm, 'hi!')


if __name__ == '__main__':
    unittest.main()
