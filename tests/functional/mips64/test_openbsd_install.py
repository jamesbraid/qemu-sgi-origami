#!/usr/bin/env python3
#
# OpenBSD/sgi 6.9 install: netboot the install ramdisk through the stock
# PROM, drive the real interactive installer unattended through a full
# install, and require it reach the installer's own "CONGRATULATIONS!"
# completion banner
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/openbsd-install (project checkout, just landed, read from
fj/main) into this fork's own functional-test framework, the same way
test_openbsd_login.py ports tools/net-transfer-diag's login_origin(). That
tool drives the real, interactive OpenBSD/sgi 6.9 installer end to end: it
creates a blank target disk and writes an OpenBSD struct disklabel into its
sector 1 (no SGI volume header -- the installer itself writes one during
install), builds a second "sets" disk containing the install media
(base69.tgz, comp69.tgz, SHA256, SHA256.sig, and bsd.IP27 -- the installed
system's own kernel, distinct from the install ramdisk) as an FFS
filesystem via makefs, boots bsd.rd.IP27 (the install ramdisk kernel) with
both disks attached, and drives the installer's own prompts through an
ordered answer table until it either completes or times out. This is the
FUNCTIONAL-TEST form of that tool: a PASSING test if the install completes
on current trunk, FAIL red (no expected-failure) if it does not -- the
documented failure mode is stalling partway through base69.tgz's or
comp69.tgz's own extraction and never reaching completion (see the
project checkout's own docs/NOW.md, "a reproducible hang mid-extraction,
traced to a double-completion in the device model", and the golden
transcript cited below).

Netboots the ramdisk through the real 6.5.30 PROM rather than loading it
with -kernel, the same conversion test_openbsd_login.py and
test_xframe_pci.py make and for the same reason: commit b707094c18 made
origin200 refuse -kernel outright at machine construction ("stock-PROM
boot only; boot a kernel through the PROM's own TFTP netboot or a disk
image, not -kernel"), and this test's own -kernel command line had been
refused, never running a single guest instruction, since that commit
landed -- undetected because it is registered only in the thorough suite
(mips64/meson.build), which an ordinary run never exercises. Also drops
-no-reboot: a real PROM boot issues a QEMU-classified guest-initiated
reset at machine construction before the PROM runs a single instruction,
and -no-reboot turns that into an immediate silent shutdown with no
console output at all, which would fail this test before the installer
ever started. lib_prom's prom_netboot_steps() drives the System
Maintenance Menu and command monitor to `boot -f bootp()/bsd.rd.IP27`;
because two of its three steps share the literal ">>" prompt text,
_drive_install()'s own window-clearing loop (which otherwise re-fires a
rule on every fresh occurrence of the same text, by design, for the
installer's own repeated prompts) tracks a separate position counter for
these three steps, firing each exactly once in order -- the same
duplicate-prompt problem lib_prom.drive_console()'s per-rule "fired"
flag and test_openbsd_net_transfer.py's own strictly sequential
_Console.expect()/send() solve differently, described in each of those
modules' own docstrings.

install_verdict() below is the judge of PASS/FAIL, checking the same
ordered completion progression tests/golden/b4-openbsd-install-session
shows for a real, complete install (project checkout, read via `git -C
<project checkout> show fj/main:tests/golden/b4-openbsd-install-session`).
It scores STAGES with lib_net.ordered_stages(), whose ordering and
trip-wire rules are unit-tested in test_lib_net.py without booting
anything; this file wires that to a real install, building the two disks,
driving the installer and capturing the console.

RULES and the location-of-sets handling below are install-driver.py's own
answer table (assets/openbsd/spike/install-driver.py, project checkout),
ported verbatim via tools/openbsd-install's own copy of it -- same
patterns, same two documented fixed bugs: the "Location of sets?" loop
(install.sub re-offers this prompt after every successful set-fetch batch
to ask "add another location?", not a re-ask -- the first occurrence
answers "disk", every occurrence after answers "done", tracked by a plain
counter since the right answer depends on which occurrence this is, not
just the prompt text), and driving the installer with bsd.IP27 itself on
the sets disk (the installer never confirms complete without it -- see
tools/openbsd-install's own docstring, "WHAT THIS PROMOTES"). Patterns are
plain str regexes here rather than the tool's own bytes regexes, matching
this framework's own str-decoded console transcript (test_openbsd_login.py
and test_prom_sash.py both keep the same bytes-to-str conversion for the
same reason). The disklabel struct-writing logic (_disklabel_bytes,
_write_disklabel) is the same port of assets/openbsd/spike/
make-disklabel.py's own build()/build_disklabel() -- see that file's own
citations for the field-layout and checksum-rule provenance (OpenBSD 6.9's
own release headers and the pinned refs/openbsd-sgi tree; nothing here is
IRIX/SGI content, per this fork's cleanroom rule -- this is public BSD
interop code already committed and cleared in the project checkout, ported
rather than newly derived from vendor source).

The sets disk is built with makefs directly (`makefs -t ffs -B be`, the
same invocation tools/mk-disk.py's own run_makefs() uses) rather than via
a nested podman invocation: this test process itself already runs inside
the project's own origami-build container whenever the real install is
driven (the built qemu-system-mips64 binary links against that image's
own libraries, so nothing that spawns it can run anywhere else), and that
image already carries makefs (tools/build-env/Containerfile, project
checkout). which('makefs') below still resolves it rather than assuming a
fixed path, and skipTest()s if it is missing -- the same defensive
resolution get_qemu_img() (qemu_test.cmd, imported below) already applies
to qemu-img.

128 MiB, not the golden's own 256 MiB: tools/openbsd-install's own current
default is -m 128, with its own docstring explicitly warning "do not raise
it" -- two guest memory-map defects are filed and unfixed
(docs/plans/ip27-memory-map-handover.md, project checkout), and 128 MiB is
the only size that clears both. tests/golden/b4-openbsd-install-session's
own capture header records a real run made at 256 MiB, before that
default was established in the now-promoted tool; the CONTRACT this port
checks (the ordered completion progression) is unaffected by memory size,
only the argv is, so this test follows the current, landed tool's own
authoritative default rather than the older capture's incidental one --
the same reasoning test_openbsd_login.py's own docstring gives for why its
own login boot is pinned to 128 MiB and not raised.

Guarded by @skipBigDataTest() (the install media total several hundred
MB) and @skipSlowTest() (a full install is 20-30 minutes) -- the
framework's own env-var-gated decorators, matching test_openbsd_login.py
-- on top of @skipIfMissingEnv('ORIGAMI_ASSETS') for a developer with no
assets at all. The install media are published to the forge asset store
now (nightly provisioning for them is being added separately, out of
this test's own scope), so ORIGAMI_ASSETS being set with an install
input missing underneath it is a provisioning failure, not an
absent-assets run, and fails the test loudly rather than skipping it.

The scoring rules install_verdict() applies live in lib_net, which needs
no qemu_test and is unit-tested on its own. What is left here is this
installer's own stages and the real install.
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

from lib_net import OPENBSD_TRIPWIRES, ordered_stages
from lib_prom import (FIRMWARE_REL, MACHINES, prom_netboot_steps,
                      require_assets)
from lib_translation_health import assert_translation_healthy


# The installer's own ordered completion progression. Each marker is
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


# Where ORIGAMI_ASSETS resolves the install media, matching
# test_openbsd_login.py's own KERNEL_REL/DISK_REL idiom: ORIGAMI_ASSETS
# names a CHECKOUT, not assets/ itself.
RELEASE_REL = os.path.join('assets', 'openbsd', '6.9')
# The install ramdisk. Also the TFTP bootfile name the PROM's netboot
# command asks the SLIRP back end for -- it is both the asset's own name
# and the name it is served under, so one constant names both.
INSTALL_KERNEL_NAME = 'bsd.rd.IP27'
# The install media that end up on the sets disk -- base69.tgz/comp69.tgz
# are the sets themselves, SHA256/SHA256.sig are the release's own signed
# manifest, and bsd.IP27 is the installed system's own kernel (required for
# the installer to ever confirm complete -- see this module's docstring).
SETS_FILE_NAMES = (
    'bsd.IP27', 'base69.tgz', 'comp69.tgz', 'SHA256', 'SHA256.sig',
)

# tools/openbsd-install's own current default, "do not raise it" -- see
# this module's docstring for why this port follows it over the golden's
# own incidental 256 MiB.
MEMORY_MIB = 128

TARGET_DISK_SIZE = '2G'   # matches the golden's own sd0: 2048MB
SETS_DISK_SIZE = '512m'   # matches tools/openbsd-install's own default

# A full install is 20-30 minutes under a quiet host (this module's own
# docstring), but a real run on a contended host was still doing
# genuine work (confirmed via /proc/<pid>/io, not a hang) in the final
# kernel-relink step past 40 minutes. 90 minutes gives that headroom;
# kept a 60s margin under the 5400s meson timeout registered for this
# test (mips64/meson.build), the same margin every other test in this
# directory keeps against its own meson timeout.
INSTALL_TIMEOUT_SECONDS = 5340


# ---------------------------------------------------------------------------
# The disklabel, ported from assets/openbsd/spike/make-disklabel.py (see
# this module's docstring for the full provenance). Used for both disks:
# the target disk gets a blank disklabel over raw sectors (the installer's
# own newfs formats it during install), the sets disk gets the same
# disklabel written after makefs has already formatted its FFS content --
# tools/mk-disk.py's own run_makefs()+label sequence, in that order.
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
    """DISKLABELV1_FFS_FRAGBLOCK(fsize, frag), ported from
    make-disklabel.py's own ffs_frag_block()."""
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
    """One partition ('a', root) spanning the whole image, and 'c'
    (RAW_PART) for the same span -- ported field-for-field from
    make-disklabel.py's own build(): no separate swap region, matching
    both disks this test builds (the target disk's own swap is unused by
    the installer's own auto-layout during install; the sets disk carries
    no swap at all)."""
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
# The answer table, ported verbatim from assets/openbsd/spike/
# install-driver.py via tools/openbsd-install's own copy of it (see this
# module's docstring). str patterns rather than the tool's own bytes
# patterns, matching this framework's str-decoded transcript.
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

# The stock PROM's own dialogue from the System Maintenance Menu to the
# netboot command, generalized by lib_prom.prom_netboot_steps() -- see
# this module's own docstring for why _drive_install() answers these by
# position rather than folding them into RULES above.
NETBOOT_STEPS = prom_netboot_steps(INSTALL_KERNEL_NAME)

FALLBACK_QUIET = 3.0
SEND_DEBOUNCE = 0.5
_WINDOW_MAX = 300
_TAIL_MAX = 4096


def _drive_install(sock, log_path, timeout, vm=None):
    """Answers the installer's prompts as install-driver.py proved,
    against a real framework console socket -- the same shape
    test_openbsd_login.py's own _drive_login() and lib_prom.py's own
    drive_console() use, adapted for install-driver.py's own generic
    "prompt went quiet, send a bare newline" fallback (FALLBACK_QUIET),
    which those two simpler drivers do not need: a real install passes
    through several prompts (DNS domain, DNS nameservers, root ssh login,
    "install a Volume Header?", "Which disk do you wish to initialize?")
    that this port does not need to answer specifically, only accept the
    guest's own bracketed default for.

    `window` (not the whole accumulated transcript) is what every rule is
    matched against, and it is cleared to empty after every send -- ported
    directly from install-driver.py's own reasoning: several rules must
    re-fire on a LATER fresh occurrence of the same prompt text (the root
    password is asked twice, "(will not echo)" then "(again)"), so a
    per-rule "already fired" flag would wrongly suppress the second
    occurrence. Clearing the window instead of tracking fired state is
    what lets the exact same regex answer both.

    The netboot steps (NETBOOT_STEPS, module level) are checked before
    everything else, by position rather than by the window-clearing
    reasoning above: two of the three share the literal ">>" prompt text,
    and a rule that fires on whichever text is in the window would answer
    the SAME one of those two forever, never reaching the boot command.
    A plain counter -- the same fix install-driver.py's own author
    already made for "Location of sets?" below, which has the identical
    shape -- answers each of the three exactly once, in order.

    The Location-of-sets special case and the fallback are both
    RE-CHECKED EVERY LOOP TICK, not only when a byte just arrived --
    ported for the same reason install-driver.py's own comment gives:
    once a prompt's full text is already sitting in `window`, the guest
    goes idle waiting for input and no further byte ever arrives to
    re-trigger a byte-driven check.

    Returns the full transcript captured (str) up to whichever comes
    first: the CONGRATULATIONS banner, a TRIPWIRES match, or timeout --
    install_verdict() is what actually decides PASS/FAIL from what was
    captured either way (matching test_openbsd_login.py's own division:
    the early break here is a runtime-budget optimization, not a second,
    divergent judgment).
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
    """Drives a full, unattended OpenBSD/sgi 6.9 install and requires
    install_verdict() be PASS. See this module's docstring for the
    full contract; this class only wires it to a real install, the
    same division test_openbsd_login.py and test_prom_sash.py keep
    between their pure verdict logic and their own QemuSystemTest.
    """

    def _build_target_disk(self, qemu_img):
        """The blank root disk, created raw then labeled -- ported
        from tools/openbsd-install's own step 3 (create, then
        write_disklabel()); the installer's own newfs formats it
        during install, so no filesystem is written here."""
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
        """The sets disk: an FFS filesystem containing the five
        install inputs as top-level files, built with makefs (ported
        from tools/mk-disk.py's own run_makefs()), then labeled --
        ported from tools/openbsd-install's own step 2 (build the
        sets disk via mk-disk.py, which pairs this exact makefs
        invocation with this exact disklabel logic)."""
        sets_tree = self.scratch_file('sets-tree')
        os.makedirs(sets_tree, exist_ok=True)
        for path in sets_files:
            shutil.copyfile(
                path, os.path.join(sets_tree, os.path.basename(path)))

        sets_img = self.scratch_file('sets.img')
        # makefs writes into an existing file; touch it first (mirrors
        # tools/mk-disk.py's own image.touch(exist_ok=True)).
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
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_openbsd_install(self):
        kernel_rel = os.path.join(RELEASE_REL, INSTALL_KERNEL_NAME)
        sets_rels = [os.path.join(RELEASE_REL, name)
                     for name in SETS_FILE_NAMES]
        assets = require_assets(self, FIRMWARE_REL, kernel_rel, *sets_rels)
        firmware, kernel = assets[FIRMWARE_REL], assets[kernel_rel]
        sets_files = [assets[rel] for rel in sets_rels]

        qemu_img = get_qemu_img(self)
        makefs_bin = which('makefs')
        if makefs_bin is None:
            self.skipTest("makefs not found in '$PATH' -- needed to "
                         "build the sets disk (tools/mk-disk.py's own "
                         "invocation; see this module's docstring)")

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
            # module's own docstring), so a live process reaching here
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
