#!/usr/bin/env python3
#
# OpenBSD/sgi 6.9 install: netboot the install ramdisk through the stock
# PROM, drive the real interactive installer unattended through a full
# install, and require it reach the installer's own "CONGRATULATIONS!"
# completion banner
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot OpenBSD/sgi 6.9 from the stock PROM and run its interactive
installer to completion. The PROM netboots the install ramdisk; the test
answers the installer prompts in order and requires its completion banner.

The test creates a blank target disk with a disklabel in sector 1. The
installer creates the target filesystem. A second disk contains the
release sets and installed kernel in an FFS filesystem made with makefs,
plus a disklabel. The installed kernel and signed release manifest are
needed for the installer to confirm completion.

The test requires 128 MiB of guest memory. This is an observed limitation
of the current model; increasing it causes the install to fail. The test
is skipped when its asset directory is not configured, and is also subject
to QEMU's large-data and slow-test options. If the asset directory is set
but an expected input is missing, asset fetching fails the test.

PASS requires the ordered installer stages to reach "CONGRATULATIONS!"
without an OpenBSD tripwire. A timeout, disconnect, or earlier tripwire
fails the test and records the console transcript.
"""

import os
import re
import shutil
import socket
import struct
import subprocess
import time

from qemu_test import (
    QemuSystemTest, skipBigDataTest, skipIfMissingEnv, skipSlowTest,
    get_qemu_img, which)
from qemu_test.asset import LocalAsset

from lib_net import OPENBSD_TRIPWIRES, ordered_stages
from lib_prom import FIRMWARE_REL, MACHINES, prom_netboot_steps
from lib_prom_sash import RELEASES
from lib_translation_health import assert_translation_healthy


# The installer's ordered completion progression. Each marker is
# searched for only after the one before it, so the second "Location of
# sets?" prompt needs its own occurrence rather than re-finding the
# first, and a "CONGRATULATIONS!" spliced in from elsewhere earlier in
# the capture does not satisfy completion.
CONGRATULATIONS_RE = re.compile(
    r"CONGRATULATIONS! Your OpenBSD install has been successfully "
    r"completed")
LOCATION_OF_SETS_RE = re.compile(r"Location of sets\?")
STAGES = (
    ("never reached the installer's welcome banner",
     re.compile(r"Welcome to the OpenBSD/sgi 6\.9 installation program\.")),
    ("never reached the first Location of sets prompt", LOCATION_OF_SETS_RE),
    ("never reached base69.tgz installation completion",
     re.compile(r"Installing base69\.tgz\s+100%")),
    ("never reached etc.tgz extraction completion",
     re.compile(r"Extracting etc\.tgz\s+100%")),
    ("never reached comp69.tgz installation completion",
     re.compile(r"Installing comp69\.tgz\s+100%")),
    ("never reached the second Location of sets prompt", LOCATION_OF_SETS_RE),
    ("never reached the CONGRATULATIONS completion banner",
     CONGRATULATIONS_RE),
)


def install_verdict(transcript):
    """PASS iff transcript shows STAGES in order and none of
    OPENBSD_TRIPWIRES fires before "CONGRATULATIONS!" is reached (or
    anywhere in the transcript, if it is never reached).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...]}.
    """
    problems = ordered_stages(transcript, STAGES, OPENBSD_TRIPWIRES)
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# Paths below are relative to QEMU_TEST_SGI_ASSET_DIR.
RELEASE_REL = os.path.join('openbsd', '6.9')
# The install ramdisk. Also the TFTP bootfile name the PROM's netboot
# command asks the SLIRP back end for -- it is both the asset's own name
# and the name it is served under, so one constant names both.
INSTALL_KERNEL_NAME = 'bsd.rd.IP27'
# Current guest memory limit observed for this install.
MEMORY_MIB = 128

TARGET_DISK_SIZE = '2G'
SETS_DISK_SIZE = '512m'

# Allow 90 minutes for installation, with a 60-second margin under the
# Meson test timeout.
INSTALL_TIMEOUT_SECONDS = 5340


# ---------------------------------------------------------------------------
# The target disk is labeled before installation; the installer creates
# its filesystem. The sets disk is formatted with makefs, then labeled.
# ---------------------------------------------------------------------------

DISKMAGIC = 0x82564557
DTYPE_SCSI = 4
FS_UNUSED = 0
FS_SWAP = 1
FS_BSDFFS = 7
MAXPARTITIONS = 16  # sys/arch/sgi/include/disklabel.h

HDR_FMT = ">IHH16s16sIIIIII8sIHHIII5IHH4IIHHII"
PART_FMT = ">IIHHBBH"
SECTOR = 512

assert struct.calcsize(HDR_FMT) == 148
assert struct.calcsize(PART_FMT) == 16


def _ffs_frag_block(fsize, frag):
    """Return the FFS fragment-to-block encoding used in the disklabel."""
    def ffs(x):
        if x == 0:
            return 0
        n = 1
        while not (x & 1):
            x >>= 1
            n += 1
        return n
    if fsize * frag == 0:
        return 0
    return ((ffs(fsize * frag) - 13) << 3) | ffs(frag)


def _disklabel_bytes(secperunit, typename, packname, fsize=1024, frag=8):
    """Build a disklabel with a root partition and a whole-disk partition."""
    npartitions = 3  # a (root), b (swap, unused), c (RAW_PART, whole disk)

    header = struct.pack(
        HDR_FMT,
        DISKMAGIC,                          # d_magic
        DTYPE_SCSI,                         # d_type
        0,                                  # d_subtype
        typename.encode()[:15].ljust(16, b"\0"),
        packname.encode()[:15].ljust(16, b"\0"),
        SECTOR,                             # d_secsize
        32,                                 # d_nsectors (synthetic geometry)
        64,                                 # d_ntracks
        secperunit // (32 * 64),            # d_ncylinders
        32 * 64,                            # d_secpercyl
        secperunit,                         # d_secperunit
        b"\0" * 8,                          # d_uid
        0,                                  # d_acylinders
        0, 0,                               # d_bstarth, d_bendh
        0, secperunit,                      # d_bstart, d_bend
        0,                                  # d_flags
        0, 0, 0, 0, 0,                      # d_drivedata[5]
        0,                                  # d_secperunith
        1,                                  # d_version
        0, 0, 0, 0,                         # d_spare[4]
        DISKMAGIC,                          # d_magic2
        0,                                  # d_checksum, filled below
        npartitions,                        # d_npartitions
        8192, 8192,                         # d_bbsize, d_sbsize
    )
    assert len(header) == 148

    fragblock = _ffs_frag_block(fsize, frag)
    parts = [
        (secperunit, 0, FS_BSDFFS, fragblock, 16),  # a: root, whole image
        (0, 0, FS_SWAP, 0, 0),                      # b: no separate region
        (secperunit, 0, FS_UNUSED, 0, 0),           # c: RAW_PART
    ]
    partbytes = b"".join(
        struct.pack(PART_FMT, size, offset, 0, 0, fstype, fb, cpg)
        for size, offset, fstype, fb, cpg in parts
    )
    partbytes += b"\0" * 16 * (MAXPARTITIONS - len(parts))

    label = header + partbytes
    assert len(label) == 148 + MAXPARTITIONS * 16 == 404

    def dkcksum(buf):
        region = buf[:148 + npartitions * 16]
        words = struct.unpack(">%dH" % (len(region) // 2), region)
        s = 0
        for w in words:
            s ^= w
        return s

    checksum = dkcksum(label)
    label = label[:136] + struct.pack(">H", checksum) + label[138:]
    assert dkcksum(label) == 0, "checksum self-check failed"

    return label.ljust(SECTOR, b"\0")


def _write_disklabel(image_path, typename, packname):
    """Writes a struct disklabel into sector 1 of image_path, sized to its
    own current file size. Returns secperunit, for logging."""
    secperunit = os.path.getsize(image_path) // SECTOR
    label = _disklabel_bytes(secperunit, typename, packname)
    with open(image_path, "r+b") as f:
        f.seek(1 * SECTOR)
        f.write(label)
    return secperunit


# ---------------------------------------------------------------------------
# Ordered answers for the interactive installer. Patterns match the
# decoded console transcript.
# ---------------------------------------------------------------------------

RULES = [
    ("install-menu", r"\(I\)nstall, \(U\)pgrade, \(A\)utoinstall or \(S\)hell\?", "I\n"),
    ("terminal-type", r"Terminal type\?", "vt220\n"),
    ("hostname", r"System hostname\?", "origami\n"),
    ("net-interface", r"[Ww]hich network interface.*\?", "done\n"),
    ("root-password", r"Password for root account\?", "origami123\n"),
    ("sshd-default", r"[Ss]tart sshd\(8\) by default\?", "yes\n"),
    ("setup-user", r"Setup a user\?", "n\n"),
    ("timezone", r"[Ww]hat timezone are you in\?", "UTC\n"),
    ("root-disk", r"[Ww]hich disk is the root disk\?", "sd0\n"),
    ("disklabel-duid", r"Use DUID", "yes\n"),
    ("disklabel-layout", r"\(A\)uto layout.*\(C\)ustom", "a\n"),
    ("already-mounted", r"disk partition already mounted\?", "no\n"),
    ("use-sets-anyway", r"Use sets found here anyway\?", "yes\n"),
    ("continue-no-verify", r"Continue without verification\?", "yes\n"),
    ("which-disk-sets", r"[Ww]hich disk.*(sets|distribution|install media)", "sd1\n"),
    ("partition", r"Partition\?", "a\n"),
    ("fs-type", r"[Ff]ile ?system type\?", "ffs\n"),
    ("set-path", r"[Rr]elative path.*sets\?|[Pp]athname.*sets\?", ".\n"),
    # Sets matching known names are pre-selected ("[X] base69.tgz ..."
    # shows before any answer is given), so "done" accepts what's already
    # checked rather than "all", which just re-confirms the same
    # selection forever.
    ("set-name", r"[Ss]et name\(s\)\?", "done\n"),
    ("nonfree-firmware", r"non-free firmware", "no\n"),
]

# PROM menu prompts and the netboot command, in order.
NETBOOT_STEPS = prom_netboot_steps(INSTALL_KERNEL_NAME)

FALLBACK_QUIET = 3.0
SEND_DEBOUNCE = 0.5
_WINDOW_MAX = 300
_TAIL_MAX = 4096


def _drive_install(sock, log_path, timeout, vm=None):
    """Answer PROM and installer prompts on the guest console.

    The rolling window is cleared after each answer so repeated prompts,
    such as the root password prompt, can be answered again. PROM steps
    are answered by position because two steps share the same prompt text.
    The repeated sets-location prompt is answered "disk" first and "done"
    thereafter. When the guest reaches an unanswered prompt with a
    bracketed default, send a newline after it goes quiet.

    Return captured output when the completion banner or a tripwire is
    seen, or when the run ends. install_verdict() determines PASS or FAIL.
    """
    rules = [(label, re.compile(pattern), answer) for label, pattern, answer in RULES]
    netboot_rules = [(re.compile(pattern), answer) for pattern, answer in NETBOOT_STEPS]
    sock.settimeout(1.0)
    started = time.monotonic()
    deadline = started + timeout
    next_report = started + 60
    guest_bytes = 0
    transcript = ""
    window = ""
    tail = ""
    last_byte_time = started
    last_send = 0.0
    netboot_step = 0
    location_of_sets_seen = 0
    fallback_fired_for = None
    health_checked = False

    with open(log_path, "a", encoding="utf-8") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                chunk = None
            except OSError:
                break

            if chunk is not None:
                if not chunk:
                    break
                text = chunk.decode("utf-8", errors="replace")
                guest_bytes += len(chunk)
                transcript += text
                log.write(text)
                log.flush()
                window = (window + text)[-_WINDOW_MAX:]
                tail = (tail + text)[-_TAIL_MAX:]
                last_byte_time = time.monotonic()
                if vm is not None and not health_checked:
                    health_checked = True
                    assert_translation_healthy(vm)
                if (CONGRATULATIONS_RE.search(tail) or
                        any(rx.search(tail) for _why, rx in OPENBSD_TRIPWIRES)):
                    break

            now = time.monotonic()
            if now >= next_report:
                print('openbsd_install: elapsed=%ds guest_bytes=%d idle=%ds '
                      'netboot_step=%d' %
                      (int(now - started), guest_bytes,
                       int(now - last_byte_time), netboot_step), flush=True)
                next_report = now + 60
            if now - last_send < SEND_DEBOUNCE:
                continue

            if netboot_step < len(netboot_rules):
                rx, answer = netboot_rules[netboot_step]
                if rx.search(window):
                    sock.sendall(answer.encode())
                    last_send = now
                    window = ""
                    fallback_fired_for = None
                    netboot_step += 1
                    continue

            if LOCATION_OF_SETS_RE.search(window):
                location_of_sets_seen += 1
                answer = "disk\n" if location_of_sets_seen == 1 else "done\n"
                sock.sendall(answer.encode())
                last_send = now
                window = ""
                fallback_fired_for = None
                continue

            fired = False
            for _label, rx, answer in rules:
                if rx.search(window):
                    sock.sendall(answer.encode())
                    last_send = now
                    window = ""
                    fallback_fired_for = None
                    fired = True
                    break
            if fired:
                continue

            if (window and now - last_byte_time > FALLBACK_QUIET and
                    window != fallback_fired_for and
                    re.search(r"\]\s*$", window)):
                sock.sendall(b"\n")
                last_send = now
                fallback_fired_for = window
                window = ""

    return transcript


# ---------------------------------------------------------------------------
# The real install: media resolution, disk construction, and the
# QemuSystemTest itself. Nothing below this line needs to be pure or
# independently testable the way the scoring above is.
# ---------------------------------------------------------------------------


class OpenbsdInstall(QemuSystemTest):
    """Run the install and require install_verdict() to return PASS."""

    ASSET_FIRMWARE = LocalAsset(
        FIRMWARE_REL, RELEASES['6.5.30']['prom_sha256'])
    ASSET_INSTALL_KERNEL = LocalAsset(
        os.path.join(RELEASE_REL, INSTALL_KERNEL_NAME),
        '4d02fc15e5c02c2f351a6e825f617c2dfae1a8dbfb51d283090e1bebc2b31d15')
    # The sets disk needs the installed kernel, both sets, and their signed
    # release manifest for the installer to confirm completion.
    ASSET_SYSTEM_KERNEL = LocalAsset(
        os.path.join(RELEASE_REL, 'bsd.IP27'),
        '5c132640eb510cbc0e63bbcca1169f587a168610837687b0d1d02950a349a7a1')
    ASSET_BASE_SET = LocalAsset(
        os.path.join(RELEASE_REL, 'base69.tgz'),
        'b71e368f9f820a3c6ca5df53fc2ba617e9f535b23504f01448fb0e2100aa9530')
    ASSET_COMP_SET = LocalAsset(
        os.path.join(RELEASE_REL, 'comp69.tgz'),
        'f65af2b2351873f9966f75a3f36b9dd3446489be0b05af060a6d5e9f97535d0b')
    ASSET_SIGNED_MANIFEST = LocalAsset(
        os.path.join(RELEASE_REL, 'SHA256'),
        '7ad56623376fff7f869b0b1f08920562e477efb71ada3e7a2da54e96064d5106')
    ASSET_MANIFEST_SIGNATURE = LocalAsset(
        os.path.join(RELEASE_REL, 'SHA256.sig'),
        '9b67eac0663a17e73714b896b5d504f26bc022b43607591ee5c7014a680b7494')

    def _build_target_disk(self, qemu_img):
        """Create the blank root disk and write its disklabel."""
        target = self.scratch_file('target.img')
        subprocess.run(
            [qemu_img, 'create', '-f', 'raw', target, TARGET_DISK_SIZE],
            check=True, capture_output=True, text=True)
        secperunit = _write_disklabel(target, 'QEMU-ORIGAMI', 'target')
        self.log.debug(
            'openbsd_install: target disk %s: secperunit=%d',
            target, secperunit)
        return target

    def _build_sets_disk(self, makefs_bin, sets_files):
        """Build an FFS sets disk and write its disklabel."""
        sets_tree = self.scratch_file('sets-tree')
        os.makedirs(sets_tree, exist_ok=True)
        for path in sets_files:
            shutil.copyfile(
                path, os.path.join(sets_tree, os.path.basename(path)))

        sets_img = self.scratch_file('sets.img')
        # makefs writes into an existing file, so create it first.
        open(sets_img, 'a', encoding='utf-8').close()
        subprocess.run(
            [makefs_bin, '-t', 'ffs', '-B', 'be',
             '-o', 'bsize=8192,fsize=1024,minfree=5',
             '-s', SETS_DISK_SIZE, sets_img, sets_tree],
            check=True, capture_output=True, text=True)
        secperunit = _write_disklabel(sets_img, 'QEMU-ORIGAMI', 'sets')
        self.log.debug(
            'openbsd_install: sets disk %s: secperunit=%d',
            sets_img, secperunit)
        return sets_img

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_openbsd_install(self):
        firmware = self.ASSET_FIRMWARE.fetch()
        kernel = self.ASSET_INSTALL_KERNEL.fetch()
        sets_files = [
            self.ASSET_SYSTEM_KERNEL.fetch(),
            self.ASSET_BASE_SET.fetch(),
            self.ASSET_COMP_SET.fetch(),
            self.ASSET_SIGNED_MANIFEST.fetch(),
            self.ASSET_MANIFEST_SIGNATURE.fetch(),
        ]

        qemu_img = get_qemu_img(self)
        makefs_bin = which('makefs')
        if makefs_bin is None:
            self.skipTest("makefs not found in '$PATH' -- needed to "
                         "build the sets disk")

        target_disk = self._build_target_disk(qemu_img)
        sets_disk = self._build_sets_disk(makefs_bin, sets_files)

        self.require_netdev('user')

        vm = self.get_vm(name='openbsd_install')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        vm.add_args(
            '-S',
            '-cpu', 'R10000', '-m', str(MEMORY_MIB),
            '-bios', firmware,
            '-nic', ('user,id=net0,'
                     f'tftp={os.path.dirname(kernel)},'
                     f'bootfile={INSTALL_KERNEL_NAME}'),
            '-drive', f'if=none,id=root-disk,file={target_disk},format=raw',
            '-device', 'scsi-hd,drive=root-disk,bus=scsi.0,scsi-id=1',
            '-drive', f'if=none,id=sets-disk,file={sets_disk},format=raw',
            '-device', 'scsi-hd,drive=sets-disk,bus=scsi.1,scsi-id=1')
        vm.launch()
        # The PROM resets early; connect QMP before that event can arrive.
        vm.qmp('cont')

        log_path = self.log_file('openbsd-install.log')
        try:
            transcript = _drive_install(
                vm.console_socket, log_path, INSTALL_TIMEOUT_SECONDS,
                vm=vm)
        finally:
            # A VM that crashed, exited on its own, or was left running
            # has each been seen here; a graceful shutdown() against an
            # already-dead process still raises AbnormalShutdown even
            # though qemu is in fact gone. No -no-reboot here (a real
            # PROM boot issues its own guest-initiated reset before the
            # PROM runs a single instruction, and -no-reboot would turn
            # that into an immediate silent shutdown -- see this
            # PROM runs a single instruction), so a live process reaches here
            # is the common case, not the exception.
            vm.shutdown(hard=vm.exitcode() is not None)

        if not transcript:
            self.fail(
                'the guest printed nothing at all -- a run that did '
                'not start, not an install that failed early. Check '
                'the emulator built, and that nothing ended the '
                'machine before it printed anything.')

        report = install_verdict(transcript)
        self.assertEqual(
            report['verdict'], 'PASS',
            'OpenBSD install problems: ' + '; '.join(report['problems']) +
            f' (full transcript: {log_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
