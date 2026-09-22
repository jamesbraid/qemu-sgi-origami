#!/usr/bin/env python3
#
# OpenBSD/IP27 login: netboot the installed 6.9 disk through the stock PROM
# and require an authenticated root shell
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/net-transfer-diag's own login_origin() console drive (project
checkout, read from fj/main) into this fork's own functional-test
framework, the same way test_prom_boot.py ports tools/boot-gate and
test_linux_kernel.py ports tools/linux-usercheck.

login_origin()'s own reference transcript (.superpowers/sdd/
2026-08-25-track4-openbsd-tests/openbsd-128mb-login-reference.log) was
captured with the kernel loaded directly via -kernel, before commit
b707094c18 made origin200 refuse -kernel outright ("stock-PROM boot
only; boot a kernel through the PROM's own TFTP netboot or a disk
image, not -kernel"). That refusal happens at machine construction,
before a single guest instruction runs, so this test had been failing
outright rather than passing since that commit landed -- undetected
because it is registered only in the thorough suite (mips64/meson.build),
which an ordinary run never exercises. This port now boots the same
kernel and disk the reference transcript did, but through the real
6.5.30 PROM's own netboot path, the same conversion test_xframe_pci.py
already made and for the same reason: lib_prom's prom_netboot_steps()
drives the System Maintenance Menu and command monitor to `boot -f
bootp()/bsd.IP27` over a SLIRP usermode network, landing on the same
root-login progression the reference transcript recorded once the
kernel is running.

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

The QEMU argv this file builds keeps the reference transcript's own
absence of a -cpu override (this board's default CPU model is already
what that real run used) but replaces -kernel with -bios (the 6.5.30
PROM) plus a SLIRP `-nic user,...,tftp=...,bootfile=bsd.IP27` serving
the same kernel file the reference transcript loaded directly, matching
test_xframe_pci.py's own already-converted command line. debug-leds=off
on the machine property matches every other origin200 test that boots
through the real PROM (test_prom_sash.py, test_prom_fx.py): a -kernel
boot never runs the PROM's own LED-lighting code, so its absence from
a build never mattered before; a real -bios boot does run that code,
and this avoids depending on the trace event it uses being compiled in.
vm.set_machine()/vm.set_console() supply -M and the serial console
wiring (see test_prom_sash.py/test_linux_kernel.py for the same
division); -snapshot keeps the 2 GiB disk read-only.

Guarded by @skipBigDataTest() (the disk is 2 GiB) and @skipSlowTest()
(the boot is minutes) -- the framework's own env-var-gated decorators
($QEMU_TEST_ALLOW_LARGE_STORAGE, $QEMU_TEST_ALLOW_SLOW;
tests/functional/qemu_test/decorators.py) -- on top of
@skipIfMissingEnv('ORIGAMI_ASSETS') for a developer with no assets at
all. Both assets are provisioned into ORIGAMI_ASSETS by the nightly, so
ORIGAMI_ASSETS being set with either one missing is a provisioning
failure, not an absent-assets run, and fails the test loudly rather
than skipping it.

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

from lib_prom import FIRMWARE_REL, prom_netboot_steps, resolve_asset
from lib_translation_health import assert_translation_healthy

from test_openbsd_login_verdict import login_verdict, TRIPWIRES


# Where ORIGAMI_ASSETS resolves the kernel and installed disk, matching
# the brief's own confirmed paths; FIRMWARE_REL (the PROM image) comes
# from lib_prom.py, imported above. ORIGAMI_ASSETS names a CHECKOUT, not
# assets/ itself.
KERNEL_REL = os.path.join('assets', 'openbsd', '6.9', 'bsd.IP27')
DISK_REL = os.path.join('assets', 'openbsd', 'spike', 'root.img')

# The boot file this test's own SLIRP TFTP server serves the kernel
# under -- see prom_netboot_steps()'s own docstring for why this need
# not match the kernel's name on disk. Matches test_xframe_pci.py's own
# NETBOOT_BOOTFILE, which boots the identical kernel.
NETBOOT_BOOTFILE = 'bsd.IP27'

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
# prom_netboot_steps() is prepended so the same rule engine first takes
# the PROM from the System Maintenance Menu through `boot -f
# bootp()/bsd.IP27` before any of login_origin()'s own questions appear.
ROOT_PASSWORD = "origami123"
LOGIN_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE) + (
    (r"root device\s*:", "sd0a\n"),
    (r"swap device[^:]*:", "\n"),
    (r"login:", "root\n"),
    (r"[Pp]assword:", ROOT_PASSWORD + "\n"),
)
_TAIL_MAX = 4096


def _drive_login(sock, log_path, timeout, vm=None):
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
    health_checked = False
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
            if vm is not None and not health_checked:
                health_checked = True
                assert_translation_healthy(vm)
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
    """Netboots the installed OpenBSD 6.9 disk through the stock PROM and
    requires login_verdict() to be PASS. See this module's docstring
    for the full contract; this class only wires it to a real boot,
    the same division test_prom_sash.py and test_linux_kernel.py keep
    between their pure verdict logic and their own QemuSystemTest.
    """

    def _asset_paths(self):
        """The firmware, kernel and installed disk under ORIGAMI_ASSETS,
        or None if any is missing -- the runner may not have assets
        provisioned yet (see this method's caller)."""
        assets_root = os.environ['ORIGAMI_ASSETS']
        firmware = resolve_asset(assets_root, FIRMWARE_REL)
        kernel = resolve_asset(assets_root, KERNEL_REL)
        disk = resolve_asset(assets_root, DISK_REL)
        if not (firmware and kernel and disk):
            return None
        return firmware, kernel, disk

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_openbsd_login(self):
        paths = self._asset_paths()
        if paths is None:
            # @skipIfMissingEnv('ORIGAMI_ASSETS') above already skipped
            # the case where it is unset entirely -- getting here means
            # it IS set, so a missing firmware, kernel or disk is a
            # provisioning failure, not an absent-assets developer run,
            # and must fail loudly: a silent skip here would let a real
            # login regression pass CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but the firmware/kernel/disk are '
                f'missing at <ORIGAMI_ASSETS>/{FIRMWARE_REL}, '
                f'<ORIGAMI_ASSETS>/{KERNEL_REL} and '
                f'<ORIGAMI_ASSETS>/{DISK_REL} -- provision them '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')
        firmware, kernel, disk = paths

        self.require_netdev('user')

        vm = self.get_vm(name='openbsd_login')
        vm.set_machine('origin200,debug-leds=off')
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-bios', firmware,
            '-nic', ('user,id=net0,'
                    f'tftp={os.path.dirname(kernel)},'
                    f'bootfile={NETBOOT_BOOTFILE}'),
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0')
        vm.launch()

        log_path = self.log_file('openbsd-login.log')
        try:
            transcript = _drive_login(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS, vm=vm)
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
