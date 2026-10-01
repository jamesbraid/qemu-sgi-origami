# Generic graphics-device evidence capture: log in, open Console, resize
# it, run a command, screendump
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The sequence is: log in through the graphical greeter, open the desktop's
Console, resize it to hold command output, type a command, and save a
screendump. The desktop interactions use the stock IRIX 6.5.30 xdm greeter
and 4Dwm layout observed at 1280x1024. They are independent of which
graphics device renders the desktop; this sequence does not validate that
device's hardware behavior.

The coordinates below were measured from screendumps in that guest and
resolution. The Login name field spans x=452..668, y=695..724; the Password
field is 46 pixels below it. The Console icon is centered near (252, 44).
A newly opened Console window spans (9, 615)..(681, 1014), or 672x399
pixels. After widening and then raising its top border, it spans
(9, ~330)..(1119, 999), enough to display the observed `hinv -v` output.
The measurements describe this guest desktop at this resolution only.

The resize uses two separate drags: widen the right border, then raise the
top border. In QEMU runs, dragging the right border through more than a
dozen correctly sized movement steps in one held-button pass produced no
visible change, on two attempts; a later step did commit the resize. In a
separate attempt, releasing and pressing again without repositioning the
pointer dragged a desktop icon instead. The input helpers keep the button
held throughout each drag and position the pointer explicitly before each
interaction. A screendump taken immediately after release can show a
partially redrawn window, so the sequence waits before continuing.

The coordinates and interaction behavior are observations from QEMU runs
of this guest setup. The original capture notes and image artifacts are
not included here, so the measurements cannot be independently checked
from this file alone. This module expects an already booted VM at the
graphical login screen and uses its QMP command interface; guest startup,
login credentials, and graphics-device selection belong to the caller.
"""

import time

import lib_input


# The IRIX 6.5.30 stock xdm greeter's Login name and Password field
# centers, in on-screen pixels at 1280x1024. Both fields sit pre-focused
# (Login name from boot, Password once it appears after Return), but
# login() still clicks each one first: focus-follows-mouse can leave a
# stale hover target from wherever the cursor last was, and clicking is
# cheap insurance against that.
LOGIN_NAME_CLICK = (560, 709)
LOGIN_PASSWORD_CLICK = (560, 755)

# How long after submitting the password to wait before the desktop is
# assumed ready. The serial console's own login prompt and the parallel
# xdm/4Dwm startup race each other (test_graphics_matrix.py's own
# screendump settle poll documents the same race for the login screen
# itself); this is the same kind of conservative fixed margin, not a
# derived bound.
LOGIN_SETTLE_SECONDS = 3.0

# Delay between typed key presses. The existing 50 ms pace works for the
# original capture targets, while callers can raise it for input models that
# drain queued scancodes more slowly.
KEY_SETTLE_SECONDS = 0.05

# The stock desktop's Console icon, centered in pixels. It is a fixed
# desktop icon (present at every fresh login, independent of which
# windows -- if any -- a previous session left open), not something a
# caller creates.
CONSOLE_ICON = (252, 44)

# How long to wait after double-clicking the Console icon before its
# window is assumed fully open and ready to resize.
OPEN_SETTLE_SECONDS = 2.0

# A freshly-opened Console window's measured default rectangle
# (left, top, right, bottom), and the two border-drag grab points and
# deltas that reliably grow it enough to hold a command's full output.
# The two drags are independent -- width first (right border), then
# height (top border) -- this is the observed working order. A combined
# corner drag has not been tried and is not assumed equivalent.
CONSOLE_INITIAL_RECT = (9, 615, 681, 1014)
CONSOLE_WIDTH_GRAB = (683, 800)
CONSOLE_WIDTH_DELTA = 438
CONSOLE_HEIGHT_GRAB = (300, 615)
CONSOLE_HEIGHT_DELTA = -285

# A point inside the Console's text body, safely below the title bar and
# left of the right border even before any resize -- used to (re)focus
# the window before typing.
CONSOLE_BODY_CLICK = (300, 500)

# How long to settle after each border drag before the next step (or a
# caller's own verification) trusts the screen. A drag's own effect can
# still be mid-redraw for a moment after the mouse button is released.
# A screendump taken immediately after release can catch a partial frame.
RESIZE_SETTLE_SECONDS = 1.5

# How long to wait after pressing Return on a typed command before its
# output is assumed complete. The default suits a fast command like
# `hinv -v`; a caller running something slower should pass a larger
# value rather than trust this one.
COMMAND_SETTLE_SECONDS = 2.0


def login(vm, username, password, settle_seconds=LOGIN_SETTLE_SECONDS,
          key_settle_seconds=KEY_SETTLE_SECONDS):
    """Log in through the stock xdm greeter: click the Login name
    field, type `username`, Return, click the Password field, type
    `password`, Return, then wait `settle_seconds` for the desktop."""
    lib_input.click_at(vm, *LOGIN_NAME_CLICK)
    lib_input.type_text(vm, username,
                        key_settle_seconds=key_settle_seconds)
    lib_input.send_key(vm, 'ret')
    time.sleep(0.5)
    lib_input.click_at(vm, *LOGIN_PASSWORD_CLICK)
    lib_input.type_text(vm, password,
                        key_settle_seconds=key_settle_seconds)
    lib_input.send_key(vm, 'ret')
    time.sleep(settle_seconds)


def open_console(vm, settle_seconds=OPEN_SETTLE_SECONDS):
    """Double-click the desktop's Console icon and wait for its window
    to open."""
    lib_input.double_click_at(vm, *CONSOLE_ICON)
    time.sleep(settle_seconds)


def resize_console(vm, *, width_delta=CONSOLE_WIDTH_DELTA,
                   height_delta=CONSOLE_HEIGHT_DELTA,
                   settle_seconds=RESIZE_SETTLE_SECONDS):
    """Grow the Console window enough to fit a command's full output
    legibly: drag its right border out by `width_delta`, settle, then
    drag its top border up by `height_delta` (a negative value grows
    the window; see CONSOLE_HEIGHT_DELTA), settle again. Defaults are
    the measured deltas for the observed 1280x1024 desktop."""
    lib_input.drag_from(vm, *CONSOLE_WIDTH_GRAB, width_delta, 0)
    time.sleep(settle_seconds)
    lib_input.drag_from(vm, *CONSOLE_HEIGHT_GRAB, 0, height_delta)
    time.sleep(settle_seconds)


def run_command(vm, command, settle_seconds=COMMAND_SETTLE_SECONDS,
                key_settle_seconds=KEY_SETTLE_SECONDS):
    """Click inside the Console body to focus it, type `command`, press
    Return, and wait `settle_seconds` for its output."""
    lib_input.click_at(vm, *CONSOLE_BODY_CLICK)
    lib_input.type_text(vm, command,
                        key_settle_seconds=key_settle_seconds)
    lib_input.send_key(vm, 'ret')
    time.sleep(settle_seconds)


def screendump(vm, out_path, image_format=None):
    """QMP `screendump` to `out_path`, in `image_format` ('png' or
    'ppm', QEMU's own QMP ImageFormat enum) when given, or QEMU's own
    default (ppm) otherwise. A thin wrapper so a caller scripting the
    whole sequence does not need to know vm.cmd()'s raw argument
    shape."""
    kwargs = {'filename': str(out_path)}
    if image_format is not None:
        kwargs['format'] = image_format
    vm.cmd('screendump', **kwargs)


def capture_evidence(vm, *, username, password, command, out_path,
                     image_format=None,
                     login_settle_seconds=LOGIN_SETTLE_SECONDS,
                     open_settle_seconds=OPEN_SETTLE_SECONDS,
                     resize_settle_seconds=RESIZE_SETTLE_SECONDS,
                     command_settle_seconds=COMMAND_SETTLE_SECONDS,
                     key_settle_seconds=KEY_SETTLE_SECONDS,
                     width_delta=CONSOLE_WIDTH_DELTA,
                     height_delta=CONSOLE_HEIGHT_DELTA):
    """The whole sequence this module exists for: log in, open Console,
    resize it, run `command`, and screendump to `out_path`.

    `vm` must already be booted and sitting at the graphical login
    screen. This function does not start the guest or select a graphics
    device; it automates the guest desktop through QMP.
    """
    login(vm, username, password, settle_seconds=login_settle_seconds,
          key_settle_seconds=key_settle_seconds)
    open_console(vm, settle_seconds=open_settle_seconds)
    resize_console(vm, width_delta=width_delta, height_delta=height_delta,
                   settle_seconds=resize_settle_seconds)
    run_command(vm, command, settle_seconds=command_settle_seconds,
                key_settle_seconds=key_settle_seconds)
    screendump(vm, out_path, image_format=image_format)
