# Scripted keyboard/mouse input over HMP, for functional tests that need
# to drive a guest's UI rather than only read its console
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Every command here goes through the project's existing
`vm.cmd('human-monitor-command', command_line=...)` convention (already
used this way throughout this directory, e.g. lib_translation_health.py
and test_sn0_smp.py) rather than a raw QMP input command, so a captured
QEMU log reads the same ordinary HMP text a human driving the monitor by
hand would have typed.

The one quirk worth a shared, tested home: QEMU's HMP `mouse_move` sends a
*relative* delta, not an absolute position, and the generic PS/2 mouse
model drains only a few queued packets per input-sync call. A single large
move can leave an undelivered backlog that the guest only partially works
through before the next command arrives, so a click or a scripted move
lands somewhere other than where it was aimed. This was independently
diagnosed live against RAD4's login screen this session (four calibrated
click attempts missed their target for exactly this reason before it was
understood): see docs/datasheets/psitech-rad4.md's "Mouse input works"
section and QEMU commit 8d0899df18 ("hw/input/sgi-ioc3-kbc: add mouse
register-contract coverage") in the Origami checkout. move_mouse() below
bakes in the fix that investigation converged on -- walk the cursor in
steps no larger than MOUSE_MOVE_STEP_MAX, with a short settle between
steps -- so a future test that needs to move a guest's pointer does not
have to rediscover it.
"""

import time


# The largest single mouse_move delta, per axis, this helper will ever
# send in one HMP command. Chosen to match the step size the RAD4
# investigation found reliable (see this module's docstring); a QEMU or
# guest driver change that raises the PS/2 model's per-sync drain could
# make a larger step safe, but nothing currently measures that, so this
# stays at the value known to work rather than a larger, unverified one.
MOUSE_MOVE_STEP_MAX = 500

# How long to let the guest drain one step's queued PS/2 packets before
# the next step is sent. Not independently re-measured here -- the
# investigation cited above verified delivery by tracing guest-side
# registers, not by timing -- so this is a conservative default, not a
# measured minimum; a caller with tighter timing evidence may pass its
# own settle_seconds.
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
        elif kind == 'wait':
            time.sleep(step[1])
        else:
            raise ValueError(f'unknown input script step: {step!r}')
