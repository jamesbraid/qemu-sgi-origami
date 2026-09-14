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

The banner alone is a late, generic marker, though: many unrelated
failures keep it from printing. So this test also checks an earlier,
generic progress marker -- the kernel's own first line of console output
(reached_first_console() below), printed once the kernel has brought up
early state and its IOC3 port-A console, well before the release banner.

Reaching that first line is a positive proof worth checking on its own:
it requires the boot to clear everything between kernel entry and first
kernel console output. The system-controller (MSC/ELSC) console path is
in that stretch, and is where the boot historically stalled -- the
miniroot once loaded into a dark console at what the record calls the
ELSC "dark point" (a harness gap on the firmware's diagnostic console;
the IRIX console itself is the IOC3 serial, not the ELSC). A regression
that re-darkened the console would fail this check. But the marker's
ABSENCE does not by itself name a cause: the FrameMask kernel-handoff and
the IOC3 serial-init paths also sit between entry and first console
output, and any of them could keep that line from printing. So the
failure message reports only what was observed and names the historical
dark point as one known context, not the cause.

reached_banner() and reached_first_console() below are the two pure
functions this file factors its progress checks into.
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


# The kernel's first line of its own console output, read on the IOC3 port-A
# console. The miniroot kernel prints nothing between PROM entry and this line
# while it brings up early CPU state and its console; this warning is the
# first thing it emits, ahead of the release banner. Reaching it proves the
# boot cleared that whole entry-to-console stretch -- the system-controller
# (MSC/ELSC) console path among it, where the boot once stalled at the "dark
# point" -- so it is an earlier progress check than the banner and a
# regression that re-darkens the console fails it. Matched on the kernel's
# unconditional date-sanity warning, the earliest line present in every
# recorded miniroot-to-banner boot (docs/boot-logs/irix.boot.2026-08-24T08:24
# -07:00 and the later captures) and absent at the dark point (the 2026-08-22
# wedge).
MINIROOT_FIRST_CONSOLE_RE = re.compile(
    rb"WARNING: Check the date to make sure it is correct")


def reached_first_console(transcript):
    """Whether the kernel produced its first line of console output.

    A str or bytes transcript both work: the marker is ASCII, so a str is
    encoded before the search. The marker is the kernel's first console line,
    printed once it has brought up early state and its IOC3 console, well
    before the release banner. It is strictly earlier than reached_banner():
    reaching the banner implies this too, but this can be true when the banner
    is not, localizing a failure that let the kernel speak but never reached
    the banner. Its presence proves the boot cleared everything between kernel
    entry and first console output, including the MSC/ELSC console path; its
    absence does not by itself name which of those stalled.
    """
    if isinstance(transcript, str):
        transcript = transcript.encode("utf-8", errors="replace")
    return bool(MINIROOT_FIRST_CONSOLE_RE.search(transcript))


# The PROM's kernel-entry line ("entry: 0x<addr>"), printed right before it
# jumps into the loaded kernel image. The ELSC console exchange sits just past
# this boundary -- it runs only after the PROM enters the kernel -- so this
# line tells a pre-entry failure (power-on diagnostics, the SLIRP netboot, or
# the kernel never loading), where the exchange never ran, from a boot that
# reached entry and then wedged at the exchange (the dark point). Present in
# every recorded boot that loaded a kernel, the 2026-08-22 wedge included, and
# absent from the PROM's own pre-entry diagnostics and menu.
MINIROOT_KERNEL_ENTRY_RE = re.compile(rb"entry: 0x[0-9a-fA-F]+")


def reached_kernel_entry(transcript):
    """Whether the PROM printed its kernel-entry line ("entry: 0x...").

    A str or bytes transcript both work. Used to attribute a missing
    ELSC-exchange marker correctly: the exchange runs only after the PROM
    enters the kernel, so a transcript that never reached this line failed
    before the exchange could run and must not be blamed on it.
    """
    if isinstance(transcript, str):
        transcript = transcript.encode("utf-8", errors="replace")
    return bool(MINIROOT_KERNEL_ENTRY_RE.search(transcript))


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
            # skipIfMissingEnv above already skipped the case where
            # ORIGAMI_ASSETS is unset entirely -- getting here means it
            # IS set, so a missing PROM or kernel is a provisioning
            # failure, not an absent-assets developer run, and must fail
            # loudly rather than skip quietly.
            self.fail(
                'ORIGAMI_ASSETS is set but the firmware/kernel are '
                f'missing at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{TFTP_REL}/{KERNEL_NAME} -- provision '
                'them (tools/extract-irix, nightly assets) rather than '
                'silently skipping a claimed capability')
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
                DRIVER_STEPS, until=DRIVER_UNTIL_RE, vm=vm)
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

        # reached_first is the earlier of the two progress markers: the
        # kernel's first console line. It is OR-ed with the banner, which is
        # downstream of it, so a fully successful boot never fails it on a
        # warning-text change. reached_entry only disambiguates the failure
        # message -- a pre-entry failure from a post-entry, pre-console stall.
        reached_entry = reached_kernel_entry(transcript)
        reached_first = reached_first_console(transcript) or reached

        run_path = self.log_file('run.json')
        with open(run_path, 'w', encoding='utf-8') as run_file:
            json.dump({
                'schema': 1,
                'started_utc': started,
                'ended_utc': ended,
                'reached_kernel_entry': reached_entry,
                'reached_first_console': reached_first,
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

        # Checked first, and separately from the banner, reporting only what
        # was observed. reached_first's absence means the kernel never printed
        # its first console line; many things between kernel entry and first
        # console output could cause that, so the message names the observed
        # stall, not a cause, and cites the historical dark point (the
        # system-controller/console path) as one known context. Gated on
        # kernel entry so a pre-entry failure, where the kernel never ran, is
        # reported as itself rather than as a console stall.
        if reached_entry:
            first_console_failure = (
                'the miniroot kernel reached PROM entry ("entry: 0x...") but '
                'never produced its first line of console output within '
                f'{BOOT_TIMEOUT_SECONDS}s. Something between kernel entry and '
                'first kernel console output stalled the boot; the '
                'system-controller (MSC/ELSC) console path, where it once '
                'wedged dark at the "dark point", is one known cause, as are '
                'the early CPU-state and IOC3 serial-init paths. This names '
                f'the observed stall, not its cause (full transcript: '
                f'{log_path}).')
        else:
            first_console_failure = (
                'the boot never reached kernel entry ("entry: 0x...") within '
                f'{BOOT_TIMEOUT_SECONDS}s: a pre-entry failure (power-on '
                'diagnostics, the SLIRP netboot, or the kernel load), before '
                f'the kernel ran at all (full transcript: {log_path}).')
        self.assertTrue(reached_first, first_console_failure)

        self.assertTrue(
            reached,
            'the miniroot kernel never reached its release banner '
            f'("IRIX Release 6.5 IP27") within {BOOT_TIMEOUT_SECONDS}s. '
            'The PROM was driven to a route-A direct boot of '
            f'{KERNEL_NAME}; the kernel either did not load or did not '
            f'reach its own console output (full transcript: {log_path}).')


if __name__ == '__main__':
    QemuSystemTest.main()
