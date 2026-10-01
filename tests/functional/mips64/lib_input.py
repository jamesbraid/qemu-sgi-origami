# Scripted keyboard/mouse input over HMP, for functional tests that need
# to drive a guest's UI rather than only read its console
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Every command here goes through the project's existing
`vm.cmd('human-monitor-command', command_line=...)` convention (already
used this way throughout this directory, e.g. lib_translation_health.py
and test_sn0_smp.py) rather than a raw QMP input command, so a captured
QEMU log reads the same ordinary HMP text a human driving the monitor by
hand would have typed.

HMP `mouse_move` sends a relative delta. Large moves can leave queued PS/2
packets undelivered before the next command, so a scripted click may miss
its target. `move_mouse()` splits movement into steps no larger than
`MOUSE_MOVE_STEP_MAX` and waits briefly between them. The step size and
settle interval are conservative values; this helper does not measure the
guest's packet-drain rate.
"""

import time


# The largest single mouse_move delta, per axis, sent in one HMP command.
# Keep this conservative unless packet delivery is measured again.
MOUSE_MOVE_STEP_MAX = 500

# How long to wait between steps. This is a conservative default, not a
# measured minimum; callers may pass their own settle_seconds.
DEFAULT_SETTLE_SECONDS = 0.2


def _split_delta(dx, dy, step):
    """The list of (sx, sy) per-step deltas that sum to (dx, dy), none
    larger than `step` on either axis.

    Pure function, no VM: this is the part of the mouse-move quirk fix
    that is actual logic rather than an HMP call, so it is what
    test_lib_input.py exercises directly. Splits each axis independently
    by its own magnitude, so a call with a large dx and a small dy does
    not force extra, unnecessary steps on the small axis.
    """
    if dx == 0 and dy == 0:
        return []
    steps_needed = max(1, -(-abs(dx) // step) if dx else 0,
                       -(-abs(dy) // step) if dy else 0)
    result = []
    sent_x = sent_y = 0
    for i in range(1, steps_needed + 1):
        target_x = round(dx * i / steps_needed)
        target_y = round(dy * i / steps_needed)
        result.append((target_x - sent_x, target_y - sent_y))
        sent_x, sent_y = target_x, target_y
    return result


def send_key(vm, key):
    """Send one HMP `sendkey` press for `key` (e.g. 'ret', 'shift',
    'a')."""
    vm.cmd('human-monitor-command', command_line=f'sendkey {key}')


def click_mouse(vm, button=1, settle_seconds=DEFAULT_SETTLE_SECONDS):
    """Press and release one mouse button (HMP's bitmask: 1 left, 2
    right, 4 middle), with a settle between press and release so the
    guest sees two distinct edges rather than a press folded into the
    same sync as its own release."""
    vm.cmd('human-monitor-command', command_line=f'mouse_button {button}')
    time.sleep(settle_seconds)
    vm.cmd('human-monitor-command', command_line='mouse_button 0')


def move_mouse(vm, dx, dy, step=MOUSE_MOVE_STEP_MAX,
               settle_seconds=DEFAULT_SETTLE_SECONDS):
    """Move the mouse by the relative delta (dx, dy), walked in steps no
    larger than `step` per axis with a settle between each -- the fix for
    the dropped-packet quirk this module's docstring describes. A caller
    after only the queue-draining behavior (not a specific destination)
    can still call this the same way: the steps always sum exactly to
    (dx, dy).
    """
    for sx, sy in _split_delta(dx, dy, step):
        vm.cmd('human-monitor-command', command_line=f'mouse_move {sx} {sy}')
        time.sleep(settle_seconds)


def run_script(vm, steps):
    """Run a sequence of input actions against vm, in order.

    Each step is one of:
      ('key', key)              -- send_key(vm, key)
      ('click', button)         -- click_mouse(vm, button)
      ('move', dx, dy)          -- move_mouse(vm, dx, dy)
      ('type', text)            -- type_text(vm, text)
      ('wait', seconds)         -- time.sleep(seconds)

    This is the shape a manifest-driven scenario registry (e.g.
    lib_graphics_matrix.py's SCENARIOS) hands a plain list of tuples to,
    so a new device's test can describe "press this, wait, move there" as
    data rather than writing its own driver loop.
    """
    for step in steps:
        kind = step[0]
        if kind == 'key':
            send_key(vm, step[1])
        elif kind == 'click':
            click_mouse(vm, step[1])
        elif kind == 'move':
            move_mouse(vm, step[1], step[2])
        elif kind == 'type':
            type_text(vm, step[1])
        elif kind == 'wait':
            time.sleep(step[1])
        else:
            raise ValueError(f'unknown input script step: {step!r}')


# The largest single mouse_move delta this module will ever send to force
# the cursor to a screen corner regardless of where it already was (see
# move_to() below). 1280x1024 is the largest graphics resolution currently
# supported, so this clears either axis of the emulated framebuffers here.
CLAMP_MOVE_DELTA = 2000

# The step size a held-button drag needs, tighter than MOUSE_MOVE_STEP_MAX
# above. A held-button drag can queue several correctly-sized moves with
# no visible effect, then apply them together later. Smaller steps limit
# how far that delayed movement can overshoot. This is conservative; it is
# not a measured minimum.
DRAG_MOVE_STEP_MAX = 30


def move_to(vm, x, y, step=MOUSE_MOVE_STEP_MAX,
           settle_seconds=DEFAULT_SETTLE_SECONDS):
    """Move the cursor to the absolute position (x, y).

    HMP's mouse_move only takes a relative delta (this module's own
    docstring) -- there is no way to address a position directly. This
    clamps the cursor to (0, 0) first, by moving past both screen edges
    at once (CLAMP_MOVE_DELTA), then walks the remaining (x, y) the
    normal chunked way. Whatever uncertainty existed about where the
    cursor already was is gone after the clamp, at the cost of a few
    extra HMP round trips -- worth it for a script that only clicks a
    handful of times per run rather than tracking position itself.
    """
    move_mouse(vm, -CLAMP_MOVE_DELTA, -CLAMP_MOVE_DELTA, step=step,
              settle_seconds=settle_seconds)
    move_mouse(vm, x, y, step=step, settle_seconds=settle_seconds)


def click_at(vm, x, y, button=1, settle_seconds=DEFAULT_SETTLE_SECONDS):
    """move_to(vm, x, y), then one click_mouse()."""
    move_to(vm, x, y, settle_seconds=settle_seconds)
    click_mouse(vm, button=button, settle_seconds=settle_seconds)


def double_click_at(vm, x, y, button=1, settle_seconds=DEFAULT_SETTLE_SECONDS):
    """move_to(vm, x, y), then two click_mouse() presses close enough
    together for the guest's window manager to see one double-click
    rather than two separate single clicks."""
    move_to(vm, x, y, settle_seconds=settle_seconds)
    click_mouse(vm, button=button, settle_seconds=settle_seconds)
    click_mouse(vm, button=button, settle_seconds=settle_seconds)


def drag_mouse(vm, dx, dy, button=1, step=DRAG_MOVE_STEP_MAX,
              settle_seconds=DEFAULT_SETTLE_SECONDS):
    """Press `button`, walk the cursor by (dx, dy) in steps no larger
    than `step`, then release -- the same chunked-delivery fix
    move_mouse() applies, but at DRAG_MOVE_STEP_MAX's smaller default.
    A caller must not assume the drag's effect (a resize, a
    repositioned window) lands after any one step, or even immediately
    after release: a held-button drag can visibly do nothing for many
    correctly-sized steps, then jump to the final result once the guest's
    PS/2 queue drains, sometimes after the button is already back up.
    Check the result (e.g. with a screendump), retrying the check with
    a short wait rather than assuming a fixed number of steps or a
    fixed settle is always enough.
    """
    vm.cmd('human-monitor-command', command_line=f'mouse_button {button}')
    time.sleep(settle_seconds)
    move_mouse(vm, dx, dy, step=step, settle_seconds=settle_seconds)
    vm.cmd('human-monitor-command', command_line='mouse_button 0')
    time.sleep(settle_seconds)


def drag_from(vm, x, y, dx, dy, button=1, step=DRAG_MOVE_STEP_MAX,
             settle_seconds=DEFAULT_SETTLE_SECONDS):
    """move_to(vm, x, y), then drag_mouse(vm, dx, dy, ...) -- the shape a
    border-drag resize needs: position on the border first, then drag
    without an intervening click (a click there would just reposition
    the cursor, not grab the resize handle)."""
    move_to(vm, x, y, settle_seconds=settle_seconds)
    drag_mouse(vm, dx, dy, button=button, step=step,
              settle_seconds=settle_seconds)


# Keys HMP's sendkey accepts for typing ordinary command text: plain
# letters and digits map to themselves, a handful of punctuation marks
# map to their HMP key names, and uppercase letters go through
# shift-<key>. This covers what a shell command like `hinv -v` or
# `ls -la /` needs; it deliberately stays a fixed table rather than a
# full keysym mapping, so type_text() below can raise immediately on an
# unsupported character rather than silently sending the wrong key or
# dropping it.
_TYPE_KEYS = {
    ' ': 'spc', '-': 'minus', '_': 'shift-minus', '/': 'slash',
    '.': 'dot', ',': 'comma', '=': 'equal', ':': 'shift-semicolon',
    ';': 'semicolon', "'": 'apostrophe',
}
for _char in 'abcdefghijklmnopqrstuvwxyz0123456789':
    _TYPE_KEYS[_char] = _char
for _char in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    _TYPE_KEYS[_char] = f'shift-{_char.lower()}'


def type_text(vm, text, key_settle_seconds=0.05):
    """Send one sendkey per character of `text`, in order, with a short
    settle between keys so the guest's keyboard driver sees distinct
    scancodes rather than a run collapsed into fewer than `text` has.

    Raises ValueError immediately on a character outside _TYPE_KEYS --
    an unsupported character must fail loudly, not type something else
    or silently skip it.
    """
    for char in text:
        key = _TYPE_KEYS.get(char)
        if key is None:
            raise ValueError(f'type_text: no HMP key mapped for {char!r}')
        send_key(vm, key)
        time.sleep(key_settle_seconds)
