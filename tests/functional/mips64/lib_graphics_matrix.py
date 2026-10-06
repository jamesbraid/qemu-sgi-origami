# The graphics test matrix: manifest format, scenario registry and parser
#
# SPDX-License-Identifier: BSD-3-Clause
"""
origin200-matrix's own Cell format (id | guest | scenario | cpus | nodes |
mem | accel | speed | timeout | attempts | min_passes | expect | xfail_mark
| xfail_signature, all parsed by lib_matrix.py) has no room for what a
graphics device's functional test needs to say: which extra `-device` to
attach, which disk image to boot, and what "the screen looks right" means
for this scenario. Stretching that one row shape to cover screen checks
too -- adding a column that has to hold a rectangle, an RGB range and an
input script as flat text -- would make an already-dense row unreadable
for a net gain of nothing, since origin200-matrix's own rows would have to
carry the same new, unused columns. So this is a second, sibling manifest
(graphics-matrix) and a second Cell shape (GraphicsCell), reusing every
piece of lib_matrix.py that does not assume the PROM-progress-code shape:
matrix_rows() (the raw pipe-row reader, already row-shape-agnostic),
AttemptResult, MatrixError and verdict() (all written against attribute
access, not the Cell class itself, so a GraphicsCell with the same id/
expect/min_passes/xfail_signature attributes satisfies them unchanged),
and qemu_args() (reads only cell.accel/cpus/mem/nodes -- GraphicsCell
carries a fixed nodes=1, since no graphics scenario here needs NUMA
topology). Only what genuinely differs -- the row shape, its validation,
and what a screen-check scenario actually does -- is new.

A screen-check scenario is named symbolically from the manifest's
`scenario` column and defined once in SCENARIOS below, the same way
origin200-matrix's pass condition (lib_prom.MARK, a fixed set of progress
codes) is Python data the manifest only refers to by name rather than
spelling out inline. Adding a new device's row means adding a Scenario
entry here (console text to wait for, an optional lib_input.run_script()
script, and a list of lib_screen check objects) and one manifest line
naming it -- no changes to the parsing or attempt-running code below.
"""

import os
import re
from dataclasses import dataclass

import lib_input
from lib_input import run_script, type_text
from lib_matrix import MatrixError
from lib_prom import drive_console
from lib_screen import (DarkPixelFractionCheck, ImageSizeCheck,
                        InkExtentCheck, NotAllBlackCheck, RegionColorCheck,
                        ScreenCheckError,
                        poll_screendump_until_checks_pass)


class GraphicsMatrixError(MatrixError):
    """Raised for any graphics-matrix row that fails to parse or
    validate. Subclasses MatrixError so a caller that already handles
    MatrixError from the PROM matrix catches this too."""


@dataclass
class GraphicsCell:
    id: str
    guest: str
    device: str
    disk: str
    scenario: str
    cpus: int
    mem: str
    accel: str
    speed: str
    timeout: int
    attempts: int
    min_passes: int
    expect: str
    xfail_mark: str
    xfail_signature: str
    # Every graphics scenario below boots one node; this is not a manifest
    # column (see this module's own docstring) but qemu_args() from
    # lib_matrix reads cell.nodes, so it needs to exist.
    nodes: int = 1


# The manifest's field order. Kept as an explicit list, unlike lib_matrix's
# _FIELD_NAMES (derived from Cell's own fields), because GraphicsCell
# carries the extra `nodes` field above that has no column of its own.
_FIELD_NAMES = ['id', 'guest', 'device', 'disk', 'scenario', 'cpus', 'mem',
               'accel', 'speed', 'timeout', 'attempts', 'min_passes',
               'expect', 'xfail_mark', 'xfail_signature']
_INT_FIELDS = {'cpus', 'timeout', 'attempts', 'min_passes'}

_ID_RE = re.compile(r'^[a-z0-9_]+$')
# Only IRIX has a scenario in SCENARIOS below that can drive a guest UI to
# a screen-check point; unlike lib_matrix's origin200-matrix (which also
# validates 'linux' and 'openbsd' rows against the real PROM), there is no
# non-IRIX graphics scenario yet, so the valid set stays narrow rather than
# accepting a guest value nothing here can run.
_VALID_GUESTS = {'irix'}
_VALID_SPEEDS = {'quick', 'thorough'}
_VALID_ACCELS = {'icount', 'mttcg', 'wallclock', 'roundrobin'}
_VALID_EXPECTS = {'pass', 'xfail'}

# The installed-IRIX disk asset convention test_irix_diag.py and
# test_irix_installed_boot.py already use: irix/<release>/
# <variant>.qcow2 under QEMU_TEST_SGI_ASSET_DIR. A graphics-matrix row's
# `disk` column is just <variant>; disk_rel() below builds the same relative
# path those files' own DISK_REL constants build by hand.
RELEASE = '6.5.30'


def disk_rel(variant):
    """Return the asset-relative path of an installed-IRIX disk variant.

    QEMU_TEST_SGI_ASSET_DIR is the asset root. For example,
    disk_rel('root-rad4') -> 'irix/6.5.30/
    root-rad4.qcow2'. The test class declares a checksum-pinned LocalAsset
    for each disk variant selected by its rows."""
    return os.path.join('irix', RELEASE, f'{variant}.qcow2')


@dataclass
class Scenario:
    """What one named graphics scenario does, once its guest has booted.

    `console_wait` is a bytes regex lib_prom.drive_console's `until`
    argument waits for on the console before anything else happens --
    for every scenario here, IRIX's own login(1) prompt. The serial
    getty that prints it and xdm's own X server are two independent
    processes racing out of the same multiuser boot, not synchronized by
    anything visible on the console, so this text match is a necessary
    but not sufficient readiness signal: lib_screen's
    poll_screendump_until_checks_pass() absorbs the remaining gap by
    polling the screendump itself, rather than this field promising a
    stronger guarantee than a console-text match can actually make.
    `input_steps` is a lib_input.run_script() step list (empty for a
    scenario that only needs to look at the screen as booted). It may
    also hold the guest-driving steps run_steps() below adds on top of
    lib_input's (serial console commands, a reboot, a screendump saved
    for the log), for a scenario that has to change the guest before
    there is anything to check.
    `screen_checks` is a list of lib_screen check objects (NotAllBlackCheck,
    RegionColorCheck, ...) evaluated against the screendump taken once
    input_steps has run and the checked region has settled.
    """
    console_wait: bytes
    input_steps: list
    screen_checks: list


# The root shell's prompt. Both sh and csh end theirs in "# " for root.
_ROOT_PROMPT = rb'[#$] '
_SHELL_TIMEOUT_SECONDS = 120
_REBOOT_TIMEOUT_SECONDS = 600
_SCREEN_STEP_TIMEOUT_SECONDS = 120.0
# The PROM's System Maintenance Menu comes back on every reboot.
_PROM_MENU_STEPS = ((r'Option\?', '1\n'),)

# passwd's own complaints; any of these means the account did not change.
_PASSWD_REFUSED_RE = re.compile(
    r'[Tt]ry again|[Ss]orry|[Pp]assword unchanged|[Tt]oo short|'
    r'[Pp]ermission denied')

# The greeter's text fields at the default 1280x1024 mode, measured from
# screendumps of this fork's trunk (370fbb5db9) with the RAD4 model. The
# dialog is drawn some time before the name field takes keyboard focus,
# and keys sent in between are lost, so the typing below waits for the
# text caret and then checks what the field shows.
#
# The caret is a black I-beam that does not blink in this model (identical
# in eight frames over three seconds). In the empty name field it spans
# x=454-459, y=700-718. The ink check below looks only at the rightmost
# dark column of the field's interior, rows 697-720, columns 453-666.
#
# Characters advance the caret about 5 px each in the password field,
# which shows no text, so the password's caret ends at x=459 + 5*length
# (9 characters: 504, 8: 499, empty: 459).
#
# The name field is awkward. The last character typed is not painted
# until the next event: the field shows "gues" and the caret over where
# "t" goes, which is exactly what a lost "t" looks like. Moving the caret
# one place left forces the paint. Rightmost ink then, for the field
# after Left: "guest" 497; one character lost "gues" 491, "gust" 488,
# "uest" 488, "guet" 489, "gest" 488.
_NAME_FIELD = (453, 697, 214, 24)
_PASSWORD_FIELD = (453, 741, 214, 30)
_CARET_EMPTY_RIGHT = (458, 460)
_NAME_RIGHT_AFTER_LEFT = {'guest': (496, 498)}
_PASSWORD_BASE_RIGHT = 459
_PASSWORD_ADVANCE = 5
_ENTRY_ATTEMPTS = 3
# A key sent to the greeter is on screen well within this.
_ENTRY_VERIFY_SECONDS = 6.0
_CARET_WAIT_SECONDS = 90.0

# What run_steps() handles itself. Anything else is handed to
# lib_input.run_script(), which rejects a step it does not know.
GUEST_STEP_KINDS = frozenset({
    'login_root', 'shell', 'set_password', 'login_desktop', 'reboot',
    'wait_screen', 'screendump', 'click_at', 'move_to', 'button',
})


class _StepRunner:
    """Runs one scenario's step list against a booted guest whose serial
    console is sitting at its login prompt.

    Every console exchange goes through lib_prom.drive_console on the
    framework's console socket, one call per step with its own log file,
    so a failed run leaves a transcript per step rather than one
    interleaved stream. Screendumps a step saves go to the test's log
    directory, which survives the run, not its scratch directory."""

    def __init__(self, test, vm, name):
        self.test = test
        self.vm = vm
        self.name = name
        self.count = 0
        self.shell_count = 0

    def _log_path(self, label):
        self.count += 1
        return self.test.log_file(f'{self.name}-{self.count:02d}-{label}.log')

    def _send(self, text):
        self.vm.console_socket.sendall(text.encode())

    def _expect(self, label, until, steps=(), timeout=_SHELL_TIMEOUT_SECONDS):
        transcript, reached = drive_console(
            self.vm.console_socket, self._log_path(label), timeout, steps,
            until=until, vm=None)
        if not reached:
            self.test.fail(
                f'{self.name}: step {label!r} never saw '
                f'{until.pattern!r} on the console within {timeout}s; '
                f'last output: {transcript[-300:]!r}')
        return transcript

    def login_root(self):
        """Log in as root on the serial console and reach a shell prompt.
        The root account has no password; its first login asks for TERM."""
        self._send('root\n')
        self._expect('login-root', re.compile(_ROOT_PROMPT),
                     steps=((r'TERM = \(vt100\)', '\n'),))

    def shell(self, command, timeout=_SHELL_TIMEOUT_SECONDS):
        """Run one command as root and wait until it has finished. The
        marker is spelled with an empty quote pair so the tty's echo of
        the command line does not itself match what the shell prints."""
        self.shell_count += 1
        marker = f'RAD4-DONE-{self.shell_count}'
        self._send(f"{command}; echo {marker[:-1]}''{marker[-1]}\n")
        return self._expect(f'shell-{self.shell_count}',
                            re.compile(marker.encode() + rb'\r?\n'),
                            timeout=timeout)

    def set_password(self, user, password):
        """Give `user` a password. xdm refuses an empty one, and the disk's
        accounts have none. The disk is opened with snapshot=on, so the
        change never reaches the image."""
        self._send(f'passwd {user}\n')
        transcript = self._expect(
            'passwd', re.compile(_ROOT_PROMPT),
            steps=((r'New password:', f'{password}\n'),
                   (r'Re-enter new password:', f'{password}\n')))
        refused = _PASSWD_REFUSED_RE.search(transcript)
        if refused:
            self.test.fail(f'{self.name}: passwd {user} refused: '
                           f'{refused.group(0)!r}')

    def _poll(self, checks, timeout):
        path = self.test.log_file(f'{self.name}-poll.ppm')
        try:
            poll_screendump_until_checks_pass(
                self.test, self.vm, path, checks, timeout_seconds=timeout)
        except ScreenCheckError as exc:
            return str(exc)
        return None

    def _enter(self, label, text, empty_checks, typed_checks, look_left):
        """Type `text` into the focused greeter field and prove the field
        holds all of it, retrying from an emptied field up to
        _ENTRY_ATTEMPTS times. Fails the test if it never does.

        With `look_left` the caret is stepped one place left before the
        check, to make the greeter paint the last character, and back
        after it."""
        problem = None
        for attempt in range(1, _ENTRY_ATTEMPTS + 1):
            type_text(self.vm, text)
            if look_left:
                run_script(self.vm, [('key', 'left')])
            problem = self._poll(typed_checks, _ENTRY_VERIFY_SECONDS)
            if look_left:
                run_script(self.vm, [('key', 'right')])
            if problem is None:
                return
            self.test.log.info(
                '%s: greeter %s field attempt %d: %s', self.name, label,
                attempt, problem)
            # Past the end, then back to the start, so a field holding
            # fewer or more characters than typed ends up empty.
            run_script(self.vm, [('key', 'right')] * (len(text) + 2) +
                       [('key', 'backspace')] * (len(text) + 2))
            if self._poll(empty_checks, _ENTRY_VERIFY_SECONDS) is not None:
                self.test.fail(
                    f'{self.name}: the greeter {label} field would not '
                    'clear after a failed entry')
        self.test.fail(
            f'{self.name}: the greeter {label} field never showed the '
            f'typed text after {_ENTRY_ATTEMPTS} attempts; last: {problem}')

    def login_desktop(self, user, password):
        """Type the greeter's name and password through the keyboard.

        Each field is typed into only once its caret is on screen (that is
        when it has keyboard focus), and what it then shows is checked, so
        a key lost at the greeter is retried here instead of showing up
        later as a refused login. Only the user names in
        _NAME_RIGHT_AFTER_LEFT have a measured extent."""
        if user not in _NAME_RIGHT_AFTER_LEFT:
            raise ValueError(
                f'login_desktop: no measured name-field extent for {user!r}')
        name_empty = [InkExtentCheck(
            'greeter-name-caret', _NAME_FIELD, *_CARET_EMPTY_RIGHT)]
        password_empty = [InkExtentCheck(
            'greeter-password-caret', _PASSWORD_FIELD, *_CARET_EMPTY_RIGHT)]
        problem = self._poll(name_empty, _CARET_WAIT_SECONDS)
        if problem is not None:
            self.test.fail(
                f'{self.name}: the greeter name field never showed its '
                f'caret: {problem}')
        self._enter('name', user, name_empty,
                    [InkExtentCheck('greeter-name-text', _NAME_FIELD,
                                    *_NAME_RIGHT_AFTER_LEFT[user])],
                    look_left=True)
        run_script(self.vm, [('key', 'ret')])
        problem = self._poll(password_empty, _CARET_WAIT_SECONDS)
        if problem is not None:
            self.test.fail(
                f'{self.name}: the greeter password field never showed '
                f'its caret: {problem}')
        right = _PASSWORD_BASE_RIGHT + _PASSWORD_ADVANCE * len(password)
        self._enter('password', password, password_empty,
                    [InkExtentCheck('greeter-password-text',
                                    _PASSWORD_FIELD, right - 1, right + 1)],
                    look_left=False)
        run_script(self.vm, [('key', 'ret')])

    def reboot(self):
        """Reboot from the root shell, answer the PROM menu, and return
        once the serial login prompt is back."""
        self._send('reboot\n')
        self._expect('reboot', re.compile(rb'login:'),
                     steps=_PROM_MENU_STEPS, timeout=_REBOOT_TIMEOUT_SECONDS)

    def wait_screen(self, checks, timeout=_SCREEN_STEP_TIMEOUT_SECONDS):
        """Poll the screen until `checks` pass; fail the test if not."""
        path = self.test.log_file(f'{self.name}-wait.ppm')
        try:
            poll_screendump_until_checks_pass(
                self.test, self.vm, path, checks, timeout_seconds=timeout)
        except Exception as exc:  # pylint: disable=broad-except
            self.test.fail(f'{self.name}: wait_screen: {exc}')

    def screendump(self, label):
        """Save the screen as <name>-<label>.ppm in the log directory."""
        self.vm.cmd('screendump', filename=str(
            self.test.log_file(f'{self.name}-{label}.ppm')))

    def click_at(self, x, y, button=1):
        """Move to the absolute position (x, y) and click `button`."""
        lib_input.click_at(self.vm, x, y, button=button)

    def move_to(self, x, y):
        """Move to the absolute position (x, y)."""
        lib_input.move_to(self.vm, x, y)

    def button(self, mask):
        """Set the held-button mask (1 left, 2 right, 4 middle) and leave
        it, so a menu that tracks the held button stays posted. 0
        releases."""
        self.vm.cmd('human-monitor-command',
                    command_line=f'mouse_button {mask}')

    def run(self, steps):
        for step in steps:
            kind = step[0]
            if kind in GUEST_STEP_KINDS:
                getattr(self, kind)(*step[1:])
            else:
                run_script(self.vm, [step])


def run_steps(test, vm, name, steps):
    """Run a scenario's input_steps: lib_input's own kinds plus the
    guest-driving ones in GUEST_STEP_KINDS.

      ('login_root',)                 -- root login on the serial console
      ('shell', command[, timeout])   -- run a command as root, wait for it
      ('set_password', user, pw)      -- passwd, as root
      ('login_desktop', user, pw)     -- type the greeter's name and password
      ('reboot',)                     -- reboot from the root shell, to the
                                         serial login prompt again
      ('wait_screen', checks[, secs]) -- poll until the checks pass
      ('screendump', label)           -- save the screen to the log directory
      ('click_at', x, y[, button])    -- absolute move, then one click
      ('move_to', x, y)               -- absolute move
      ('button', mask)                -- hold buttons (0 releases)
    """
    _StepRunner(test, vm, name).run(steps)


# RAD4's own login-screen scenario. The region and expected range below
# were measured live against this fork's own tip while building this
# test (a2546528e2 and successors): booting IRIX 6.5.30 with
# `-device psitech-rad4,addr=5` (the vendor-documented slot), waiting for
# "... console login:" on the plain text console, then polling the screendump
# until xdm's own, independently-timed redraw catches up, shows a
# screendump with the rendered login screen; the plain gray area of that
# dialog (below the four user-face icons, above the login-name field)
# reads (217, 217, 217) there. Before it settles, a screendump can still
# show only the root window's stipple pattern (confirmed live: this
# scenario's own region then averages close to (64, 64, 64), the
# stipple's own measured value, not the dialog's). The input steps below
# (a harmless modifier keypress and a chunked mouse move) exercise
# lib_input's helpers end to end without changing what is on screen, so
# they run before the screen checks rather than after.
_LOGIN_DIALOG = RegionColorCheck(
    'login-dialog-chrome', rect=(400, 450, 150, 60),
    min_rgb=(190, 190, 190), max_rgb=(235, 235, 235))

# The desktop scenarios below were measured from screendumps of this
# fork's trunk (370fbb5db9) with `-device psitech-rad4,addr=5`, booting
# the root-rad4 disk, logging in as guest at the default 1280x1024 mode
# and leaving the Icon Catalog window where the session opens it. Every
# rectangle is in full-screen coordinates; the comment on each says which
# number it came from. The frames are deterministic, so the ranges only
# leave room for a one-count rounding difference, not for a different
# picture.

# The Icon Catalog's file pane below its icons: a flat (135, 170, 202).
# Present once 4Dwm has drawn the session, so waiting for it keeps the
# menu clicks from racing the desktop's first paint.
_DESKTOP_READY = RegionColorCheck(
    'icon-catalog-pane', rect=(660, 800, 500, 100),
    min_rgb=(133, 168, 200), max_rgb=(137, 172, 204))

# Everything the desktop scenarios share: a root login on the serial
# console to give the guest account a password (xdm refuses the disk's
# empty one), the greeter, the graphical login, the first desktop paint,
# and pointer acceleration off so every absolute move lands where the
# script says. With the default acceleration the guest scales a relative
# move by an amount that depends on how fast the moves arrive.
_DESKTOP_LOGIN_STEPS = [
    ('login_root',),
    ('set_password', 'guest', 'origami65'),
    ('wait_screen', [_LOGIN_DIALOG], 180.0),
    ('login_desktop', 'guest', 'origami65'),
    ('wait_screen', [_DESKTOP_READY], 240.0),
    ('wait', 5.0),
    ('screendump', 'desktop'),
    ('shell', 'env DISPLAY=:0 /usr/bin/X11/xset m 1 1'),
]

# The Icon Catalog's window-menu button, top left of its title bar.
_WINDOW_MENU_BUTTON = (632, 576)
# Empty root window, clear of every icon and the Icon Catalog.
_DESKTOP_MENU_POINT = (400, 400)

# Menu body grey. Every menu surface is one flat (217, 217, 217) in the
# frames measured; the window behind a menu is the Icon Catalog's blue
# (135, 170, 202) and beige or the teal root-window stipple, so a menu
# that is not composited over the window leaves these regions far from
# the grey.
#
# Window menu: the strip above "Restore" is a flat (217, 217, 217); the
# whole body from the "Restore" row down to "Close" averages
# (193.0, 193.0, 193.0), text included.
_WINDOW_MENU_TOP = RegionColorCheck(
    'window-menu-top-strip', rect=(630, 596, 128, 3),
    min_rgb=(215, 215, 215), max_rgb=(219, 219, 219))
_WINDOW_MENU_BODY = RegionColorCheck(
    'window-menu-body', rect=(626, 616, 134, 170),
    min_rgb=(187, 187, 187), max_rgb=(199, 199, 199))
# The disabled "Restore" row and its "Alt+F5" accelerator are drawn as
# black dots on a stipple over the menu grey. Measured black fraction
# (every channel <= 8) with the overlay-index-compat property on, against
# off: "Restore" 0.125 against 0.067, "Alt+F5" 0.114 against 0.061. The
# floors sit between the two, nearer the on value. With the property off
# the model drops about half of those dots, and the label reads as
# broken. The on fractions are what the current model draws, not
# a hardware measurement.
_WINDOW_MENU_RESTORE = DarkPixelFractionCheck(
    'window-menu-restore-label', rect=(630, 597, 60, 14),
    max_channel=8, min_fraction=0.10)
_WINDOW_MENU_RESTORE_KEY = DarkPixelFractionCheck(
    'window-menu-restore-accelerator', rect=(700, 597, 55, 14),
    max_channel=8, min_fraction=0.09)

# Desktop menu: the strip above its "Desktop" title is a flat
# (217, 217, 217) and the margin right of the longest label averages
# (216.1, 216.1, 216.1). The disabled rows are black stippled dots on the
# menu grey, 0.063 black for "Open" and 0.092 for "Share with Network...";
# with the property off there are no black pixels at all, because the
# desktop's teal (105, 181, 181) and grey (134, 134, 134) stipple shows
# through in their place.
_DESKTOP_MENU_TOP = RegionColorCheck(
    'desktop-menu-top-strip', rect=(405, 403, 170, 3),
    min_rgb=(215, 215, 215), max_rgb=(219, 219, 219))
_DESKTOP_MENU_MARGIN = RegionColorCheck(
    'desktop-menu-margin', rect=(560, 462, 14, 310),
    min_rgb=(213, 213, 213), max_rgb=(219, 219, 219))
_DESKTOP_MENU_OPEN = DarkPixelFractionCheck(
    'desktop-menu-open-label', rect=(406, 462, 60, 20),
    max_channel=8, min_fraction=0.04)
_DESKTOP_MENU_SHARE = DarkPixelFractionCheck(
    'desktop-menu-share-label', rect=(406, 606, 150, 20),
    max_channel=8, min_fraction=0.06)

# A mode switch changes the frame size, so the login dialog is measured
# again at 1024x768 (rad4setres 9): its body is a flat (217, 217, 217),
# the frame around it (230, 230, 230), and the empty login-name field
# averages (210.7, 180.7, 180.7).
_MODE9_SIZE = ImageSizeCheck(1024, 768)
_MODE9_DIALOG_BODY = RegionColorCheck(
    'mode9-login-dialog-body', rect=(300, 250, 400, 150),
    min_rgb=(215, 215, 215), max_rgb=(219, 219, 219))
_MODE9_DIALOG_FRAME = RegionColorCheck(
    'mode9-login-dialog-frame', rect=(210, 560, 100, 20),
    min_rgb=(228, 228, 228), max_rgb=(232, 232, 232))
_MODE9_NAME_FIELD = RegionColorCheck(
    'mode9-login-name-field', rect=(330, 570, 200, 28),
    min_rgb=(207, 177, 177), max_rgb=(214, 184, 184))

SCENARIOS = {
    # Click the Icon Catalog's window-menu button. A menu that is not
    # drawn fails the grey checks. With overlay-index-compat=off the menu
    # is drawn but the greyed "Restore" row loses about half its dots,
    # which only the two black-fraction checks catch.
    'rad4_window_menu': Scenario(
        console_wait=rb'login:',
        input_steps=[
            *_DESKTOP_LOGIN_STEPS,
            ('click_at', *_WINDOW_MENU_BUTTON),
            ('wait', 3.0),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            _WINDOW_MENU_TOP,
            _WINDOW_MENU_BODY,
            _WINDOW_MENU_RESTORE,
            _WINDOW_MENU_RESTORE_KEY,
        ],
    ),
    # Hold the right button on the empty desktop. The root menu stays up
    # only while the button is down, so the button is left pressed for the
    # screendump.
    'rad4_desktop_menu': Scenario(
        console_wait=rb'login:',
        input_steps=[
            *_DESKTOP_LOGIN_STEPS,
            ('move_to', *_DESKTOP_MENU_POINT),
            ('button', 2),
            ('wait', 3.0),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            _DESKTOP_MENU_TOP,
            _DESKTOP_MENU_MARGIN,
            _DESKTOP_MENU_OPEN,
            _DESKTOP_MENU_SHARE,
        ],
    ),
    # Switch the display to mode 9 (1024x768) from the serial console and
    # reboot. The PROM's menu did not come back after a reboot from the
    # running system in the measured runs; the step answers it if it does.
    'rad4_mode_switch': Scenario(
        console_wait=rb'login:',
        input_steps=[
            ('login_root',),
            ('shell', '/usr/bin/rad4setres 9'),
            ('reboot',),
            ('wait_screen', [_MODE9_DIALOG_BODY], 180.0),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            _MODE9_SIZE,
            _MODE9_DIALOG_BODY,
            _MODE9_DIALOG_FRAME,
            _MODE9_NAME_FIELD,
        ],
    ),
    # Turn on the buffered X server, reboot, and require the greeter to
    # be drawn (this mode used to render black), then log in, hold the
    # root menu and require the serial shell to still answer. The final
    # checks assert the menu's grey surfaces only. In this mode the
    # model draws the disabled rows' text in the colour behind the menu
    # instead of black dots (measured: "Open" row has no black pixel and
    # the desktop's teal shows through), unlike the unbuffered server.
    # That is the model as it stands, not asserted here either way.
    'rad4_buffered_mode': Scenario(
        console_wait=rb'login:',
        input_steps=[
            ('login_root',),
            ('shell', 'chkconfig rad4X_buffered_server on'),
            ('reboot',),
            ('wait_screen', [NotAllBlackCheck(), _LOGIN_DIALOG], 180.0),
            ('login_root',),
            ('set_password', 'guest', 'origami65'),
                    ('login_desktop', 'guest', 'origami65'),
            ('wait_screen', [_DESKTOP_READY], 240.0),
            ('wait', 5.0),
            ('shell', 'env DISPLAY=:0 /usr/bin/X11/xset m 1 1'),
            ('move_to', *_DESKTOP_MENU_POINT),
            ('button', 2),
            ('wait', 3.0),
            ('shell', 'echo alive'),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            _DESKTOP_MENU_TOP,
            _DESKTOP_MENU_MARGIN,
        ],
    ),
    'rad4_login_screen': Scenario(
        console_wait=rb'login:',
        input_steps=[
            ('key', 'shift'),
            ('wait', 0.2),
            ('move', 1800, -1300),
            ('wait', 0.2),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            RegionColorCheck('login-dialog-chrome', rect=(400, 450, 150, 60),
                             min_rgb=(190, 190, 190), max_rgb=(235, 235, 235)),
        ],
    ),
}


def peek_row(line):
    """A row's id and speed fields read raw, before any parsing: (id, speed).

    Mirrors lib_matrix.peek_row() exactly, for the same reason: naming and
    routing a test method before parse_row() has judged the row."""
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        return None, None
    values = dict(zip(_FIELD_NAMES, raw_fields))
    cell_id = values['id'] if _ID_RE.match(values['id']) else None
    speed = values['speed'] if values['speed'] in _VALID_SPEEDS else None
    return cell_id, speed


def parse_row(path, lineno, line):
    """Parse and validate one graphics-matrix row into a GraphicsCell.

    Mirrors lib_matrix.parse_row(); the differences are the field list
    (GraphicsCell's, not Cell's) and the extra checks for `device`,
    `disk` and `scenario`."""
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: expected {len(_FIELD_NAMES)} fields, '
            f'got {len(raw_fields)}: {line!r}')

    values = dict(zip(_FIELD_NAMES, raw_fields))
    for name in _INT_FIELDS:
        try:
            values[name] = int(values[name])
        except ValueError as exc:
            raise GraphicsMatrixError(
                f'{path}:{lineno}: {name!r} is not an integer: '
                f'{values[name]!r}') from exc

    cell = GraphicsCell(**values)
    _validate(path, lineno, cell)
    return cell


def _validate(path, lineno, cell):
    if not _ID_RE.match(cell.id):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: id {cell.id!r} is not [a-z0-9_]+')
    if cell.guest not in _VALID_GUESTS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown guest {cell.guest!r}')
    if not cell.device:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: cell {cell.id!r} has an empty device column')
    if not cell.disk:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: cell {cell.id!r} has an empty disk column')
    if cell.scenario not in SCENARIOS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown scenario {cell.scenario!r} -- add '
            'it to lib_graphics_matrix.SCENARIOS')
    if cell.speed not in _VALID_SPEEDS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown speed {cell.speed!r}')
    if cell.accel not in _VALID_ACCELS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown accel {cell.accel!r}')
    if cell.expect not in _VALID_EXPECTS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown expect {cell.expect!r}')
    if cell.expect == 'xfail' and not (cell.xfail_mark and
                                        cell.xfail_signature):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: xfail cell {cell.id!r} needs both '
            'xfail_mark and xfail_signature')
