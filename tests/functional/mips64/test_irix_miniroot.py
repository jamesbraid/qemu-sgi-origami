#!/usr/bin/env python3
#
# IRIX miniroot direct-boot check: boot the real IP27 PROM, direct-boot the
# 6.5.30 miniroot kernel (unix.IP27) over SLIRP usermode networking, and
# require the miniroot kernel's own release banner
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Boots the IRIX miniroot kernel with no netinst server and no external
network: the whole install dance (a BOOTP/TFTP server container, fx to
partition a disk, the maintenance-menu install that copies a miniroot to
that disk) is skipped. QEMU's own SLIRP usermode networking serves the
one file this needs, the miniroot kernel image, from a local directory,
and the PROM is driven straight to a route-A direct boot of it
(`boot -f bootp()/unix.IP27`) -- the same direct-kernel boot the real
install's manual route uses, with the network reduced to QEMU's built-in
stack.

This is the sibling of test_prom_sash.py: same real IP27 PROM, same SLIRP
usermode netboot with no external TFTP server, same ELSC-AUX-on-serial-2
wiring and the same ported serial-console expect loop driving the System
Maintenance Menu. Two things differ, and both are the point of this test:
the file SLIRP serves is the miniroot kernel rather than sash64, and the
success signal is the miniroot kernel's own release banner rather than a
sash inventory.

One SLIRP detail this test depends on: QEMU's usermode stack answers a
PROM BOOTP request with the netdev's configured `bootfile`, whatever path
the boot command names, so the netdev must set `bootfile=unix.IP27` for
the direct boot to load the kernel (with `bootfile=sash64` the PROM loads
sash instead, no matter what the boot line asks for). A real dnsmasq-backed
netinst server honours the requested path instead; this test does not use
one.

What "the miniroot booted" means here, and what it deliberately does not:
the marker is the kernel's release banner, printed right after kernel
entry and before the kernel touches a disk or brings up its root
filesystem. It proves the PROM loaded and entered the miniroot kernel and
the kernel reached its own early console output over the IOC3 port-A
console. It says nothing about the later root-filesystem bring-up, which
on this machine still cannot enumerate the SCSI disk the miniroot roots
on -- a separate, open frontier. An earlier, stable marker on the base
this test lands against was chosen deliberately: the base has the
crosstalk-widget bridge fix the boot needs to clear power-on diagnostics
but not every later kernel-console fix, and the banner is reached well
before any of those matter.

reached_banner() below is the one pure function this file factors the
success check into.
"""

import datetime
import hashlib
import json
import os
import re
import subprocess

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import drive_console, resolve_asset


# The miniroot kernel's own release banner, printed at kernel entry. The
# board name (IP27) is a public identifier; matched loosely on the release
# and board so a later build-date or feature-string change in the same
# 6.5 release does not break the match.
MINIROOT_BANNER_RE = re.compile(rb"IRIX Release 6\.5 IP27")


def reached_banner(transcript):
    """Whether the miniroot kernel's release banner appears in transcript.

    A str or bytes transcript both work: the marker is ASCII, so a str is
    encoded before the search. This is the whole success contract -- the
    banner is printed at kernel entry, so its presence is proof the PROM
    loaded and entered the miniroot kernel and the kernel reached its own
    console output.
    """
    if isinstance(transcript, str):
        transcript = transcript.encode("utf-8", errors="replace")
    return bool(MINIROOT_BANNER_RE.search(transcript))


# The release this test's real boot drives. 6.5.30 is the miniroot the
# single-CPU IRIX bring-up runs against; its PROM and kernel digests are
# checked below so a stale or wrong-release asset is caught before a boot
# is spent on it.
RELEASE = "6.5.30"

# Where ORIGAMI_ASSETS resolves the real firmware and boot tree, matching
# test_prom_sash.py's FIRMWARE_REL/TFTP_REL: ORIGAMI_ASSETS names a
# CHECKOUT (one with its own assets/ directory), not assets/ itself.
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('assets', 'boot', RELEASE)
KERNEL_NAME = 'unix.IP27'

# The release-matched PROM and miniroot-kernel SHA-256 digests. The PROM
# digest matches tools/sashlib.py's RELEASES["6.5.30"] and test_prom_sash's
# own copy; the kernel digest is assets/MANIFEST.sha256's own
# boot/6.5.30/unix.IP27 row.
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
)
KERNEL_SHA256 = (
    "cbdb02ea5d334ecac949a6524a363e6c945f94baf438d41e8d9248eda0377bdf"
)

# sash-gate's own single-CPU budget (tools/sashlib.py) is 1200s for the
# whole run; this test drives only the boot-and-direct-boot leg, but the
# miniroot kernel is a 12 MiB image the PROM fetches over SLIRP TFTP before
# it even enters, so a larger margin than test_prom_sash's is kept, a
# little under the meson timeout registered for this test in
# mips64/meson.build (900s).
BOOT_TIMEOUT_SECONDS = 840

# tools/serial-console's own console driver, ported the same way
# test_prom_sash.py ports it: each (pattern, response) fires once, the
# first time PATTERN matches the TAIL of what has been received since the
# last match. These three steps drive the PROM from the System Maintenance
# Menu through the command monitor to a route-A direct boot of the miniroot
# kernel. `unsetenv netaddr` clears the documentation-range default the PROM
# powers up with, which otherwise makes bootp() report no server. The
# responses end in '\n', the newline this console's driver uses (matching
# test_prom_sash's own DRIVER_STEPS).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", "boot -f bootp()/" + KERNEL_NAME + "\n"),
)
# The run's proof the kernel actually loaded and entered, not merely that
# its image was requested: the kernel's own release banner.
DRIVER_UNTIL_RE = MINIROOT_BANNER_RE


def _file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_identity_problems(prom_path, kernel_path):
    """Which of the release-matched PROM / miniroot-kernel SHA-256 digests
    mismatch, the same guard test_prom_sash's artifact_identity_problems
    makes against a stale or wrong-release asset before spending a boot on
    it. stdlib hashlib only."""
    problems = []
    if _file_sha256(prom_path) != PROM_SHA256:
        problems.append("PROM SHA-256 does not match release " + RELEASE)
    if _file_sha256(kernel_path) != KERNEL_SHA256:
        problems.append(
            KERNEL_NAME + " SHA-256 does not match release " + RELEASE)
    return problems


def miniroot_extra_args(prom, tftp, elsc_log, capture):
    """The QEMU args beyond what set_machine()/set_console() already
    provide, for the fixed 1-CPU miniroot direct boot: the real PROM,
    normal power-on diagnostics, and SLIRP usermode networking serving the
    miniroot kernel with no external TFTP server. This mirrors
    test_prom_sash.py's sash_extra_args, with the netdev's bootfile pointed
    at the kernel rather than sash64.

    set_console() already emits "-serial chardev:console" for serial line 0
    (IOC3 port A, the primary console and where the miniroot kernel's banner
    lands); the "-serial none" here fills line 1 (IOC3 port B, unused) so the
    "-serial chardev:elsc" that follows lands on line 2, the ELSC AUX port,
    which is given a file backend so a guest that relocates its session there
    (console=d) is captured rather than lost to a dark port.
    """
    return [
        '-cpu', 'R10000',
        '-smp', '1',
        '-m', '64',
        '-bios', str(prom),
        '-audio', 'none',
        '-chardev', f'file,id=elsc,path={elsc_log}',
        '-serial', 'none',
        '-serial', 'chardev:elsc',
        '-nic', ('user,id=net0,net=192.0.2.0/24,host=192.0.2.2,'
                 f'dhcpstart=192.0.2.15,tftp={tftp},bootfile={KERNEL_NAME}'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]


def _utc_now():
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    resolved = os.path.realpath(path)
    digest = hashlib.sha256()
    with open(resolved, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": str(path),
        "resolved_path": resolved,
        "sha256": digest.hexdigest(),
        "size_bytes": os.path.getsize(resolved),
    }


def _fork_git_identity(tree):
    """This fork checkout's own commit and dirty state, the binding record
    for a functional test inside the fork itself. None on any git failure:
    a missing identity must not read as a fabricated clean one. Matches
    test_prom_sash._fork_git_identity."""
    try:
        commit = subprocess.run(
            ["git", "-C", str(tree), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30)
        status = subprocess.run(
            ["git", "-C", str(tree), "status", "--porcelain=v1",
             "--untracked-files=normal"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if commit.returncode != 0 or status.returncode != 0:
        return None
    return {
        "path": str(tree),
        "commit": commit.stdout.strip(),
        "dirty": bool(status.stdout.strip()),
    }


# This file's own repo root: tests/functional/mips64/test_irix_miniroot.py
# is three directories below it.
_FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))

class IrixMiniroot(QemuSystemTest):
    """Boots the real IP27 PROM, direct-boots the 6.5.30 miniroot kernel
    over SLIRP usermode networking, and requires the kernel's release
    banner. See this module's docstring for the full contract; this
    class only wires it to a real boot, the same division
    test_prom_sash.py keeps between its pure logic and its QemuSystemTest
    class.
    """

    def _release_paths(self):
        """The real PROM image and miniroot-kernel TFTP tree under
        ORIGAMI_ASSETS, or None if either is missing -- the runner may
        not have assets provisioned yet (see this method's caller)."""
        assets_root = os.environ['ORIGAMI_ASSETS']
        prom = resolve_asset(assets_root, FIRMWARE_REL)
        kernel = resolve_asset(assets_root,
                               os.path.join(TFTP_REL, KERNEL_NAME))
        if not (prom and kernel):
            return None
        return prom, os.path.join(assets_root, TFTP_REL)

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_irix_miniroot(self):
        paths = self._release_paths()
        if paths is None:
            self.skipTest(
                'no firmware/kernel '
                f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{TFTP_REL}/{KERNEL_NAME} -- point it '
                'at a checkout with the real PROM and release-matched '
                'miniroot kernel extracted (tools/extract-irix) to run '
                'this test')
        prom, tftp = paths
        kernel = os.path.join(tftp, KERNEL_NAME)

        # Refuse a stale or wrong-release PROM/kernel before spending a
        # boot on assets that would silently produce a meaningless pass
        # or a confusing failure, the same refusal test_prom_sash makes.
        identity_problems = artifact_identity_problems(prom, kernel)
        if identity_problems:
            self.fail(
                f'release {RELEASE} asset identity problems: ' +
                '; '.join(identity_problems))

        vm = self.get_vm(name='irix_miniroot')
        vm.set_machine(
            'origin200,synthesized-firmware=off,debug-leds=off')
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                   *miniroot_extra_args(prom, tftp, elsc_log, capture))

        started = _utc_now()
        vm.launch()
        log_path = self.log_file('miniroot.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE)
        finally:
            # A VM that crashed or exited on its own has already left; a
            # graceful shutdown() against an already-dead process still
            # raises AbnormalShutdown even though qemu is in fact gone.
            # Matches test_prom_sash.py's PromSash.test_prom_sash.
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = _utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that did '
                'not start, not a netboot that failed early. Check the '
                'emulator built, and that nothing ended the machine '
                'before it printed anything.')

        run_path = self.log_file('run.json')
        with open(run_path, 'w', encoding='utf-8') as run_file:
            json.dump({
                'schema': 1,
                'started_utc': started,
                'ended_utc': ended,
                'reached_banner': reached,
                'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                'command': {
                    'binary': self.qemu_bin,
                    'machine': (
                        'origin200,synthesized-firmware=off,'
                        'debug-leds=off'),
                    'args': miniroot_extra_args(
                        prom, tftp, elsc_log, capture),
                },
                'identity': {
                    'fork': _fork_git_identity(_FORK_ROOT),
                    'qemu_binary': _file_record(self.qemu_bin),
                    'firmware': {
                        'release': RELEASE,
                        'prom': _file_record(prom),
                        'kernel': _file_record(kernel),
                    },
                },
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertTrue(
            reached,
            'the miniroot kernel never reached its release banner '
            f'("IRIX Release 6.5 IP27") within {BOOT_TIMEOUT_SECONDS}s. '
            'The PROM was driven to a route-A direct boot of '
            f'{KERNEL_NAME}; the kernel either did not load or did not '
            f'reach its own console output (full transcript: {log_path}).')


if __name__ == '__main__':
    QemuSystemTest.main()
