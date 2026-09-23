# Generic graphics-device evidence capture: log in, open Console, resize
# it, run a command, screendump
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Validating a new graphics device visually has been the same manual sequence
every time regardless of which card is attached: boot IRIX, log in through
the graphical greeter, open the desktop's Console icon, drag it big enough
to hold a command's full output, type the command, and screendump the
result. Nothing about that sequence is specific to RAD4, RAD1, or any other
card this project models or will model -- it is IRIX's stock 4Dwm desktop,
identical regardless of which device rendered it. This module is that
sequence as reusable functions, so a caller (tools/graphics-capture-evidence
in the Origami checkout, or a future graphics-matrix scenario) states which
guest account, which command and where to write the screendump, and does
not re-derive the desktop layout.

Every coordinate and delta below is measured, not estimated, against IRIX
6.5.30's stock xdm greeter and 4Dwm desktop at 1280x1024 -- this project's
only graphics resolution so far. Two sources agree:

  - docs/datasheets/psitech-rad4.md's "Window resize is confirmed working"
    finding (Origami checkout), which first measured the Console window's
    default size (~670x390) and a working resize target (~1112x677).
  - A fresh, independent live capture this session (Origami-checkout run
    irix-20260918T044948-592655, disk copied from
    /storage/nvme/origami/rad4-bringup-20260916/root.qcow2 with a new
    `guest` account password set via `passwd guest` at an authenticated
    root shell), which pixel-measured the same greeter and desktop
    directly from its own screendumps: the Login name field at
    x=[452,668] y=[695,724], the Password field 46px below it, the
    Console desktop icon centered near (252, 44), the freshly-opened
    Console window at x=[9,681] y=[615,1014] (672x399, matching the
    datasheet's "~670x390"), and -- after the same two-stage border drag
    this module implements -- a final window at x=[9,1119] y=[~330,999]
    (comfortably holding the full `hinv -v` output this tool exists to
    capture, confirmed by eye against that live screendump).

That same live capture also reproduced psitech-rad4.md's mouse-drag
quirk directly: dragging the Console window's right border the full
measured distance in one held-button pass produced no visible change
across more than a dozen correctly-sized steps, twice, before either
committing on a later step or (once, mid-investigation) turning out to
have silently dragged a desktop icon instead because the cursor's
position was not reset between two separate press/drag/release attempts.
Two things fixed it, and both are load-bearing in the functions below:
never release and re-press mid-drag (drag_mouse() in lib_input.py holds
the button for every step of one delta and releases once), and always
re-clamp the cursor to a known position with lib_input.move_to() before
a new interaction rather than trust wherever the last one left it.

`vm` throughout this module is anything whose `.cmd(name, **kwargs)`
executes a QMP command by name -- qemu_test's own VM class satisfies
this directly (see lib_input.py's own docstring for the convention this
project already uses it for), and so does a caller's own thin adapter
over another control path, as tools/graphics-capture-evidence's does over
Origami's `tools/origami qmp`/`tools/origami hmp`. Nothing here launches a
guest, drives its serial console to a login prompt, or knows what
graphics device is attached -- that is the caller's job, once for
whichever card is under test.
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
# height (top border) -- because that is the order this module's own
# docstring found working live; a single combined corner drag was not
# tried and is not assumed equivalent.
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
# still be mid-redraw for a moment after its button-release HMP command
# returns; this module's docstring's own live capture saw a screendump
# taken immediately after release catch a stale partial-redraw frame
# that a later screendump did not.
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
    the exact deltas this module's own docstring measured working."""
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
    screen -- this function has no idea how it got there or which
    graphics device is attached, which is the point: it is generic
    desktop automation, not tied to any one card. tools/graphics-
    capture-evidence in the Origami checkout calls this directly; a
    future graphics-matrix scenario needing the same sequence should
    too, rather than reimplementing it.
    """
    login(vm, username, password, settle_seconds=login_settle_seconds,
          key_settle_seconds=key_settle_seconds)
    open_console(vm, settle_seconds=open_settle_seconds)
    resize_console(vm, width_delta=width_delta, height_delta=height_delta,
                   settle_seconds=resize_settle_seconds)
    run_command(vm, command, settle_seconds=command_settle_seconds,
                key_settle_seconds=key_settle_seconds)
    screendump(vm, out_path, image_format=image_format)
