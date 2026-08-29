#!/usr/bin/env python3
#
# OpenBSD/IP27 login: boot the installed 6.9 disk directly via -kernel and
# require an authenticated root shell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/net-transfer-diag's own login_origin() console drive (project
checkout, read from fj/main) into this fork's own functional-test
framework, the same way test_prom_boot.py ports tools/boot-gate and
test_linux_kernel.py ports tools/linux-usercheck. A real boot has PROVEN
this reaches an authenticated root login on -M origin200 at 128 MiB, on
this fork's current trunk: a fresh trunk-pinned qemu, clean "origami#"
root shell, "ifconfig -a" exit 0, no ddb>/panic/bus error -- see
.superpowers/sdd/2026-08-25-track4-openbsd-tests/
openbsd-128mb-login-reference.log, this test's own real reference
transcript. This is a genuine PASSING test, not an xfail.

128 MiB, not this machine's default: at 128 MiB or below the guest puts
its whole memory bank in one slot and the machine backs exactly that
range, so the two filed >128MiB memory-map defects (see
docs/plans/ip27-memory-map-handover.md, project checkout) have no address
at which to appear. Above it the guest's map overruns what is backed and
the installed system does not reach a login -- test_openbsd_login_verdict
.py's own SYNTHETIC_DDB_WEDGE_TRANSCRIPT fixture is real evidence of
exactly that defect at 256 MiB.

login_verdict() (test_openbsd_login_verdict.py, imported below) is the
pure port of login_origin()'s own three-stage progression and its wedge
trip wires; see that module's own docstring for the full contract. This
file only wires it to a real boot: _drive_login() drives the console
through the same ordered questions login_origin() answers (root device,
swap device default, login, password), against this fork's own
framework console socket (QEMUMachine.console_socket via
QemuSystemTest.get_vm(), the same real API lib_prom.py's own
drive_console() drives) -- not net-transfer-diag's own Console class,
per this port's SELF-CONTAINED brief. _drive_login() also breaks out of
its read loop early on a TRIPWIRES match (imported from
test_openbsd_login_verdict.py rather than duplicated), matching
login_origin()'s own reasoning for watching every wait for a fault: a
wedged guest never prints what is being waited for, so without an early
trip a run would sit out its whole budget reporting a plain timeout,
against a guest that is meanwhile sitting at a debugger prompt that
already answered the question. login_verdict() is still what decides
PASS/FAIL, from the transcript captured either way -- the early break is
only a runtime-budget optimization, not a second, divergent judgment.

The QEMU argv this file builds is exactly the brief's own spike-verified
command line (-M origin200 -m 128 -nographic -snapshot -kernel ... -drive
if=none,id=hd0,file=...,format=raw -device scsi-hd,bus=scsi.0,scsi-id=1,
drive=hd0 -nic none) -- no -cpu override: the reference transcript's own
command line carries none, so this board's default CPU model is already
what that real run used. vm.set_machine()/vm.set_console() supply -M and
the serial console wiring (see test_prom_sash.py/test_linux_kernel.py for
the same division); -snapshot keeps the 2 GiB disk read-only.

Guarded by @skipBigDataTest() (the disk is 2 GiB) and @skipSlowTest()
(the boot is minutes) -- the framework's own env-var-gated decorators
($QEMU_TEST_ALLOW_LARGE_STORAGE, $QEMU_TEST_ALLOW_SLOW;
tests/functional/qemu_test/decorators.py) -- on top of
@skipIfMissingEnv('ORIGAMI_ASSETS') and a skipTest() at method entry if
either asset is missing. Both assets are provisioned
into ORIGAMI_ASSETS by the nightly.

The pure verdict logic lives entirely in test_openbsd_login_verdict.py
(imported below; it needs no qemu_test itself). What is left here is the
real boot.
"""

import os
import re
import socket
import time

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)

from lib_prom import resolve_asset

from test_openbsd_login_verdict import login_verdict, TRIPWIRES


# Where ORIGAMI_ASSETS resolves the kernel and installed disk, matching
# the brief's own confirmed paths and test_prom_boot.py's own
# FIRMWARE_REL idiom: ORIGAMI_ASSETS names a CHECKOUT, not assets/ itself.
KERNEL_REL = os.path.join('assets', 'openbsd', '6.9', 'bsd.IP27')
DISK_REL = os.path.join('assets', 'openbsd', 'spike', 'root.img')

# Spike-verified: at 128 MiB or below the installed disk reaches an
# authenticated root login (see this module's own docstring).
MEMORY_MIB = 128

# The spike took about 9 minutes under a contended host; kept a 60s
# margin under the 900s meson timeout registered for this test
# (mips64/meson.build), the same margin test_prom_sash.py and
# test_prom_boot.py keep against their own meson timeouts.
BOOT_TIMEOUT_SECONDS = 840

# login_origin()'s own console drive (tools/net-transfer-diag, project
# checkout): answer "root device:" with the disk, take the default swap,
# send "root" at "login:", send the root password at "Password:". Ported
# here rather than imported -- SELF-CONTAINED per this port's brief.
ROOT_PASSWORD = "origami123"
LOGIN_STEPS = (
    (r"root device\s*:", "sd0a\n"),
    (r"swap device[^:]*:", "\n"),
    (r"login:", "root\n"),
    (r"[Pp]assword:", ROOT_PASSWORD + "\n"),
)
_TAIL_MAX = 4096


def _drive_login(sock, log_path, timeout):
    """Answer the kernel's device questions, then log in, against a real
    framework console socket -- the same shape as lib_prom.py's own
    drive_console(), adapted for login_origin()'s own ordered questions
    instead of sash-gate's PROM menu.

    Every byte received is accumulated into the returned transcript and
    appended to log_path, so a run this drove is inspectable exactly like
    a captured -serial file. Returns the transcript (str) captured up to
    whichever comes first: the root shell prompt, a TRIPWIRES match (see
    this module's own docstring for why this is only a runtime-budget
    optimization, not a second judgment), or the timeout -- login_verdict()
    is what actually decides PASS/FAIL from what was captured either way.
    """
    rules = [
        {"re": re.compile(pattern), "response": response, "fired": False}
        for pattern, response in LOGIN_STEPS
    ]
    shell_re = re.compile(r"# ")
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = ""
    tail = ""
    with open(log_path, "a", encoding="utf-8") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            text = chunk.decode("utf-8", errors="replace")
            transcript += text
            log.write(text)
            log.flush()
            tail = (tail + text)[-_TAIL_MAX:]
            for rule in rules:
                if not rule["fired"] and rule["re"].search(tail):
                    sock.sendall(rule["response"].encode())
                    rule["fired"] = True
                    tail = ""
            if shell_re.search(tail):
                break
            if any(rx.search(tail) for _why, rx in TRIPWIRES):
                break
    return transcript


class OpenbsdLogin(QemuSystemTest):
    """Boots the installed OpenBSD 6.9 disk directly via -kernel and
    requires login_verdict() to be PASS. See this module's docstring
    for the full contract; this class only wires it to a real boot,
    the same division test_prom_sash.py and test_linux_kernel.py keep
    between their pure verdict logic and their own QemuSystemTest.
    """

    def _asset_paths(self):
        """The kernel and installed disk under ORIGAMI_ASSETS, or
        None if either is missing -- the runner may not have assets
        provisioned yet (see this method's caller)."""
        assets_root = os.environ['ORIGAMI_ASSETS']
        kernel = resolve_asset(assets_root, KERNEL_REL)
        disk = resolve_asset(assets_root, DISK_REL)
        if not (kernel and disk):
            return None
        return kernel, disk

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_openbsd_login(self):
        paths = self._asset_paths()
        if paths is None:
            self.skipTest(
                'no kernel/disk at '
                f'<ORIGAMI_ASSETS>/{KERNEL_REL} and '
                f'<ORIGAMI_ASSETS>/{DISK_REL} -- point it at a '
                'checkout with the OpenBSD 6.9 kernel and installed '
                'disk provisioned (nightly assets) to run this test')
        kernel, disk = paths

        vm = self.get_vm(name='openbsd_login')
        vm.set_machine('origin200')
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-kernel', kernel,
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0',
            '-nic', 'none')
        vm.launch()

        log_path = self.log_file('openbsd-login.log')
        try:
            transcript = _drive_login(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
        finally:
            # A VM that crashed or exited on its own has already
            # left; a graceful shutdown() against an already-dead
            # process still raises AbnormalShutdown even though qemu
            # is in fact gone. Matches test_prom_sash.py's own
            # PromSash.test_prom_sash and test_prom_boot.py's own
            # PromBoot.test_prom_boot.
            vm.shutdown(hard=vm.exitcode() is not None)

        if not transcript:
            self.fail(
                'the guest printed nothing at all -- a run that did '
                'not start, not a boot that failed early. Check the '
                'emulator built, and that nothing ended the machine '
                'before it printed anything.')

        report = login_verdict(transcript)
        self.assertEqual(
            report['verdict'], 'PASS',
            'OpenBSD login problems: ' + '; '.join(report['problems']) +
            f' (full transcript: {log_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
