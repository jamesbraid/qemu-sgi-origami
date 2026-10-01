#!/usr/bin/env python3
#
# SI/MGRAS graphical desktop: boot an Origin 2000 with an SI board in IO3 to
# the IRIX greeter, log in as guest, and require the default desktop to
# show its expected colours
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The same boot checks the xdm greeter before login. Everything after a
graphical login -- Xsgi loading the DAC gamma ramp, 4Dwm drawing the
granite backdrop from colour-index image data, the desktop icons' label
plates, the Toolchest -- goes through SI paths the greeter never uses,
so this test carries the same boot one step further and checks the
desktop that a fresh login produces.

The sequence:

  1. Boot the one-node Origin 2000 deskside with the BaseIO-G and an SI
     board fitted in IO3 (two R10000s, 256 MiB, root-si.qcow2) and pick PROM
     menu option 1.
  2. On the serial console, log in as root and give the guest account a
     password. The stock guest account has an empty password, and xdm
     refuses an empty one, so without this the graphical login cannot
     succeed. The disk is opened with snapshot=on, so the change never
     reaches the image.
  3. Wait for the greeter's colour and structural checks to
     pass, so the keystrokes land on a greeter that is up and drawn,
     rather than racing X's startup.
  4. Type the name and password through HMP sendkey: name, Return,
     password, Return. The name field has focus from boot and the
     password field takes it after Return, so no pointer is needed.
  5. Poll screendumps until DESKTOP_COLOR_CHECKS all pass: the granite
     backdrop, the guest, dumpster and Welcome_to_SGI icon label plates
     with their ink, and the Toolchest's title bar and menu body. See
     lib_mgras_regions.py for where each expected colour comes from and
     which side of the DAC ramp it was measured on.

The checks look at fixed patches of the screen rather than a whole-frame
golden image: the pointer, window placement from a previous session and
the moment a window finishes drawing all vary between runs, while these
patches do not.

The live run this test was modelled on (QEMU 5d3590c2db) reached the
greeter about three minutes after boot and passed every region in both
tables.
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lib_input
from lib_mgras_greeter import GREETER_SCREEN_CHECKS
from lib_mgras_regions import DESKTOP_COLOR_CHECKS
from lib_prom import (FORK_ROOT, MACHINES, drive_console, run_record,
                      utc_now)
from lib_screen import ScreenCheckError, poll_screendump_until_checks_pass

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
    from qemu_test.asset import LocalAsset
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = '6.5.30'
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('irix', RELEASE, 'root-si.qcow2')

# The stock deskside fits a plain BaseIO and no graphics, so
# baseio-preset=io6g asks for the BaseIO-G, whose second IOC3 carries the
# keyboard and mouse an interactive session needs, and SI_ARGS fits the SI
# board into the empty IO3 slot. The earlier four-node greeter boot could
# hit a Crossbow link-reset race; QEMU 2324ead1d3 fixed it before this
# profile was qualified.
MACHINE = (MACHINES['origin2000'] +
           ',topology=origin2000-deskside,nodes=1,baseio-preset=io6g,'
           'debug-leds=off')
SI_ARGS = ('-device', 'sgi-mgras,slot=io3')
SMP = '2'
MEMORY_MIB = 256

GUEST_USER = 'guest'
# IRIX's passwd wants at least six characters with letters and a digit.
# Only lowercase letters and digits, so lib_input.type_text can type it.
GUEST_PASSWORD = 'origami65'

# Boot to the serial login prompt, then the root login and passwd on top.
# The earlier greeter-only boot allowed 1200s; the passwd exchange takes
# seconds.
SERIAL_TIMEOUT_SECONDS = 1320

# Each rule fires once, in order of the guest's prompts. The root shell
# is csh, whose first login asks for TERM; a bare newline takes the
# default. The "[#$] " rule is the root prompt.
SERIAL_STEPS = (
    (r'Option\?', '1\n'),
    (r'login:', 'root\n'),
    (r'TERM = \(vt100\)', '\n'),
    (r'[#$] ', f'passwd {GUEST_USER}\n'),
    (r'New password:', f'{GUEST_PASSWORD}\n'),
    (r'Re-enter new password:', f'{GUEST_PASSWORD}\n'),
)
PASSWD_COMMAND = f'passwd {GUEST_USER}'.encode()
PASSWD_DONE_RE = re.compile(rb'Re-enter new password:.*?[#$] ', re.S)
# passwd's own complaints; any of these means the account did not change.
PASSWD_REFUSED_RE = re.compile(
    rb'[Tt]ry again|[Ss]orry|[Pp]assword unchanged|[Tt]oo short|'
    rb'[Pp]ermission denied')


def passwd_outcome(transcript):
    """'done', 'refused' or None, judged only on what the console printed
    after the passwd command was echoed, so a daemon's unrelated
    complaint during boot cannot end the wait."""
    start = transcript.rfind(PASSWD_COMMAND)
    if start < 0:
        return None
    after = transcript[start:]
    refused = PASSWD_REFUSED_RE.search(after)
    if refused:
        return 'refused: ' + refused.group(0).decode(errors='replace')
    if PASSWD_DONE_RE.search(after):
        return 'done'
    return None

# The greeter reached about three minutes after boot on the live run,
# and the serial login prompt comes before the greeter is drawn.
GREETER_SETTLE_SECONDS = 300.0
# A fresh desktop session's first paint. This polls the checks themselves,
# so the bound only matters on failure.
DESKTOP_SETTLE_SECONDS = 600.0
POLL_INTERVAL_SECONDS = 5.0
# Between Return on the name and typing the password, so the password
# field has taken focus.
FIELD_SETTLE_SECONDS = 1.0


if QemuSystemTest is not None:

    class MgrasDesktop(QemuSystemTest):
        """Check the painted SI greeter, then log in and check the desktop."""

        ASSET_FIRMWARE = LocalAsset(
            FIRMWARE_REL,
            'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd')
        ASSET_DISK = LocalAsset(
            DISK_REL,
            '1bd8867a5d34461a21998f07971e0ba4cb6f1e824fc236b1ad0405e39f05a13c')

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_mgras_desktop(self):
            prom = self.ASSET_FIRMWARE.fetch()
            disk = self.ASSET_DISK.fetch()

            vm = self.get_vm(name='mgras_desktop')
            vm.set_machine(MACHINE)
            vm.set_console()
            vm.add_args(
                '-accel', 'tcg,thread=multi',
                '-cpu', 'R10000', '-smp', SMP, '-m', str(MEMORY_MIB),
                '-bios', str(prom),
                '-audio', 'none',
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-nic', 'user,id=net0',
                *SI_ARGS,
                '-S')

            started = utc_now()
            vm.launch()
            vm.cmd('cont')
            log_path = self.log_file('mgras-desktop.log')
            greeter_path = self.log_file('mgras-desktop-greeter.ppm')
            desktop_path = self.log_file('mgras-desktop.ppm')
            timings = {}
            try:
                phase = time.monotonic()
                transcript, _reached = drive_console(
                    vm.console_socket, log_path, SERIAL_TIMEOUT_SECONDS,
                    SERIAL_STEPS,
                    reached_when=lambda text: passwd_outcome(text) is not None,
                    vm=vm)
                timings['serial_seconds'] = round(time.monotonic() - phase, 1)
                if not transcript:
                    self.fail(
                        'the guest printed nothing at all -- a run that '
                        'did not start, not a boot that failed early')
                outcome = passwd_outcome(transcript.encode())
                if outcome is not None and outcome != 'done':
                    self.fail(
                        f'passwd did not set the {GUEST_USER} password '
                        f'({outcome}) -- console log: {log_path}')
                if outcome is None:
                    self.fail(
                        'the serial console never finished setting the '
                        f'{GUEST_USER} password within '
                        f'{SERIAL_TIMEOUT_SECONDS}s -- console log: '
                        f'{log_path}')

                phase = time.monotonic()
                try:
                    greeter = poll_screendump_until_checks_pass(
                        self, vm, greeter_path, GREETER_SCREEN_CHECKS,
                        interval_seconds=POLL_INTERVAL_SECONDS,
                        timeout_seconds=GREETER_SETTLE_SECONDS)
                except ScreenCheckError as exc:
                    self.fail(
                        'the greeter never showed its expected colours, '
                        f'so there was nothing to log in to: {exc} -- '
                        f'last frame: {greeter_path}')
                timings['greeter_seconds'] = round(
                    time.monotonic() - phase, 1)

                lib_input.type_text(vm, GUEST_USER)
                lib_input.send_key(vm, 'ret')
                time.sleep(FIELD_SETTLE_SECONDS)
                lib_input.type_text(vm, GUEST_PASSWORD)
                lib_input.send_key(vm, 'ret')

                phase = time.monotonic()
                try:
                    desktop = poll_screendump_until_checks_pass(
                        self, vm, desktop_path, DESKTOP_COLOR_CHECKS,
                        interval_seconds=POLL_INTERVAL_SECONDS,
                        timeout_seconds=DESKTOP_SETTLE_SECONDS)
                except ScreenCheckError as exc:
                    self.fail(
                        'the desktop never showed its expected colours '
                        f'after logging in as {GUEST_USER}: {exc} -- last '
                        f'frame: {desktop_path}, console log: {log_path}')
                timings['desktop_seconds'] = round(
                    time.monotonic() - phase, 1)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = utc_now()

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1, 'started_utc': started, 'ended_utc': ended,
                    'timings': timings,
                    'greeter_checks': greeter,
                    'desktop_checks': desktop,
                    'identity': run_record(FORK_ROOT, self.qemu_bin,
                                          prom=prom, disk=disk),
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
