#!/usr/bin/env python3
#
# Equivalence test: test_openbsd_install's verdict logic checks the real
# OpenBSD/sgi 6.9 installer's own completion progression, without booting
# anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_openbsd_install_verdict.py` (or
`uv run --with pytest pytest
tests/functional/mips64/test_openbsd_install_verdict.py -v`). It exercises
install_verdict(), the pure function test_openbsd_install.py calls against
a captured console transcript, over fixture transcripts that stand in for
a captured guest console log.

install_verdict() checks the ordered progression the project checkout's own
tests/golden/b4-openbsd-install-session shows for a real, complete install
(read via `git -C <project checkout> show fj/main:tests/golden/
b4-openbsd-install-session`; that file's own header names the captured run
and the completion-erasure fix it required): the installer's welcome
banner, the first "Location of sets?" prompt (the driver answers "disk"
here -- the sets need fetching), base69.tgz's own install reaching 100%,
etc.tgz's own extraction reaching 100%, comp69.tgz's own install reaching
100%, the second "Location of sets?" prompt (the driver answers "done"
here -- install.sub re-offers the prompt after every successful batch to
ask "add another location?", not a re-ask; see the golden's own header for
why both occurrences are genuine), and finally the "CONGRATULATIONS!"
completion banner. These seven markers are checked STRICTLY IN ORDER
(each search starts where the previous match ended), confirmed against the
real golden transcript itself: all seven occur in strictly increasing
byte position there. This is the same ordered-progression shape
test_openbsd_login_verdict.py's own login_verdict() uses for its three
markers, extended to seven because the installer's own real completion
path has more stages than a login does.

The known failure mode this module exists to catch (see the project
checkout's own docs/NOW.md and the golden's own header): a run that stalls partway
through base69.tgz or comp69.tgz extraction and never reaches
"CONGRATULATIONS!". Under the ordered-stage check, that failure surfaces
as "never reached ... base69.tgz installation completion" or "... comp69.tgz
installation completion" -- whichever set's own 100% mark the run never
printed -- exactly the stall the golden's own header describes, not a
generic "the run stopped early" message.

TRIPWIRES matches test_openbsd_login_verdict.py's own three trip wires
verbatim (ddb>, panic:, Trap cause = 7 -- see that module's docstring for
where each pattern comes from): a wedge that shows up before
"CONGRATULATIONS!" must FAIL, never be laundered by whatever the guest
prints afterward. Reproduced independently here rather than imported,
matching this port's own SELF-CONTAINED brief (test_prom_sash.py keeps its
own independent copy of tools/sashlib.py's ASSERTIONS for the same
reason).

Fixture provenance (every fixture is real captured guest-console text from
the project checkout's own tests/golden/b4-openbsd-install-session,
sliced verbatim -- nothing below is written from scratch; escaped for this
source file's own \r and control-byte sequences without altering a single
byte of the underlying capture, confirmed by round-tripping each fixture's
escaped text back through Python's own string literal parser and diffing
it against the golden file byte for byte before this module was written):

  REAL_INSTALL_TRANSCRIPT
      The golden's own guest-console portion in full, copied from its
      "ARCS64 Firmware" opening line (its header comment block stripped,
      the same way test_openbsd_login_verdict.py's own
      REAL_LOGIN_TRANSCRIPT keeps only the captured guest-console text)
      through its final "Failed!!! Please reset manually." line -- the
      whole real, complete install this module's own PASS case must
      accept, [driver] log lines and all (kept for the same reason the
      golden itself keeps them: what the driver answered at each prompt is
      part of the proof this is a real interactive install, not a canned
      image).

  TRUNCATED_BASE69_TRANSCRIPT
      REAL_INSTALL_TRANSCRIPT's own prefix, cut mid-way through
      "Installing base69.tgz" -- at its own real "43%" progress mark --
      the point a run that stalled during base69.tgz extraction (the
      first-named case of this module's own documented failure mode)
      would stop at. Never reaches base69.tgz's own 100% mark, comp69.tgz,
      or "CONGRATULATIONS!".

  TRUNCATED_COMP69_TRANSCRIPT
      REAL_INSTALL_TRANSCRIPT's own prefix, cut mid-way through
      "Installing comp69.tgz" -- at its own real "61%" progress mark --
      the point a run that got past base69.tgz and etc.tgz (both reach
      their own 100% mark in this fixture) but stalled during
      comp69.tgz's own extraction (the second-named case of the same
      documented failure mode) would stop at. Proves the ordered check
      catches a stall at the LATER of the two named sets independently of
      the earlier one succeeding.
"""

import re
import unittest


# The installer's own ordered completion progression -- see this module's
# docstring for where each marker comes from and the golden citation that
# every one of these occurs, in this order, in a real complete install.
WELCOME_RE = re.compile(
    r"Welcome to the OpenBSD/sgi 6\.9 installation program\.")
LOCATION_OF_SETS_RE = re.compile(r"Location of sets\?")
BASE69_COMPLETE_RE = re.compile(r"Installing base69\.tgz\s+100%")
ETC_TGZ_COMPLETE_RE = re.compile(r"Extracting etc\.tgz\s+100%")
COMP69_COMPLETE_RE = re.compile(r"Installing comp69\.tgz\s+100%")
CONGRATULATIONS_RE = re.compile(
    r"CONGRATULATIONS! Your OpenBSD install has been successfully "
    r"completed")

# A run that shows any of these before it reaches "CONGRATULATIONS!" is a
# wedge, not a completed install (byte-for-byte the same three patterns
# test_openbsd_login_verdict.py's own TRIPWIRES checks -- see that
# module's docstring for where each pattern comes from and why).
TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)


def install_verdict(transcript):
    """PASS iff transcript shows, in order: the welcome banner, the first
    "Location of sets?" prompt, base69.tgz's own install reaching 100%,
    etc.tgz's own extraction reaching 100%, comp69.tgz's own install
    reaching 100%, the second "Location of sets?" prompt, and the
    "CONGRATULATIONS!" completion banner -- and none of TRIPWIRES fires
    before "CONGRATULATIONS!" is reached (or anywhere in the transcript,
    if it is never reached).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...]}.
    """
    welcome_m = WELCOME_RE.search(transcript)
    loc1_m = (LOCATION_OF_SETS_RE.search(transcript, welcome_m.end())
             if welcome_m else None)
    base69_m = (BASE69_COMPLETE_RE.search(transcript, loc1_m.end())
               if loc1_m else None)
    etc_m = (ETC_TGZ_COMPLETE_RE.search(transcript, base69_m.end())
            if base69_m else None)
    comp69_m = (COMP69_COMPLETE_RE.search(transcript, etc_m.end())
               if etc_m else None)
    loc2_m = (LOCATION_OF_SETS_RE.search(transcript, comp69_m.end())
             if comp69_m else None)
    congrats_m = (CONGRATULATIONS_RE.search(transcript, loc2_m.end())
                 if loc2_m else None)

    # Trip wires are checked over the whole span up to where
    # "CONGRATULATIONS!" was reached, or the whole transcript if it never
    # was -- a wedge anywhere before real completion must fail, not just
    # one that happens to sit between the last stage matched and the next.
    trip_end = congrats_m.start() if congrats_m else len(transcript)
    problems = [why for why, rx in TRIPWIRES
               if rx.search(transcript, 0, trip_end)]

    if welcome_m is None:
        problems.append("never reached the installer's welcome banner")
    elif loc1_m is None:
        problems.append("never reached the first Location of sets prompt")
    elif base69_m is None:
        problems.append("never reached base69.tgz installation completion")
    elif etc_m is None:
        problems.append("never reached etc.tgz extraction completion")
    elif comp69_m is None:
        problems.append("never reached comp69.tgz installation completion")
    elif loc2_m is None:
        problems.append("never reached the second Location of sets prompt")
    elif congrats_m is None:
        problems.append("never reached the CONGRATULATIONS completion banner")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# ---------------------------------------------------------------------------
# Fixtures


REAL_INSTALL_TRANSCRIPT = """\
ARCS64 Firmware
Found SGI-IP27, setting up.
Machine is in M mode.
Initial setup done, switching console.

Copyright (c) 1982, 1986, 1989, 1991, 1993

\tThe Regents of the University of California.  All rights reserved.

Copyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org


OpenBSD 6.9 (RAMDISK-IP27) #0: Thu Apr 22 10:25:14 UTC 2021

    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/RAMDISK-IP27

real mem = 268435456 (256MB)

rsvd mem = 16171008 (16MB)

avail mem = 249331712 (237MB)

random: boothowto does not indicate good seed

mainbus0 at root: Origin 200

cpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0

cpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way

clock0 at mainbus0 nasid 0: int 5

xbow0 at mainbus0 nasid 0: Bridge revision 3

xbridge0 at xbow0 widget 8: Bridge revision 3

xbpci0 at xbridge0 bus 0: 33 MHz PCI bus

pci0 at xbpci0 bus 0

qlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62

qlw0: nvram corrupt

qlw0: firmware rev 1.0.0, attrs 0x0

scsibus0 at qlw0: 16 targets, initiator 0

sd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd0: 2048MB, 512 bytes/sector, 4194304 sectors

sd1 at scsibus0 targ 2 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd1: 400MB, 512 bytes/sector, 819200 sectors

qlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61

qlw1: nvram corrupt

qlw1: firmware rev 1.0.0, attrs 0x0

scsibus1 at qlw1: 16 targets, initiator 0

ioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00

onewire0 at ioc0

ioc0: ethernet irq 2, xbow irq 60

ioc0: superio irq 3, xbow irq 59

com0 at ioc0 base 0x20178: ns16550a, 16 byte fifo

com0: console

com1 at ioc0 base 0x20170: ns16550a, 16 byte fifo

iockbc0 at ioc0

iec0 at ioc0: 128KB SSRAM, address ff:ff:ff:ff:ff:ff

nsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1

lpt at ioc0 not configured

dsrtc0 at ioc0: DS1742W

boot device: 'unknown' unrecognized.

root on rd0a swap on rd0b dump on rd0b

WARNING: CHECK AND RESET THE DATE!

erase ^?, werase ^W, kill ^U, intr ^C, status ^T

Welcome to the OpenBSD/sgi 6.9 installation program.

[driver] install-menu -> b'I\\n'
(I)nstall, (U)pgrade, (A)utoinstall or (S)hell? I
At any prompt except password prompts you can escape to a shell by
typing '!'. Default answers are shown in []'s and are selected by
pressing RETURN.  You can exit this program at any time by pressing
Control-C, but this can leave your system in an inconsistent state.


[driver] terminal-type -> b'vt220\\n'
Terminal type? [vt220] vt220

[driver] hostname -> b'origami\\n'
System hostname? (short form, e.g. 'foo') origami

Available network interfaces are: iec0 vlan0.

[driver] net-interface -> b'done\\n'
Which network interface do you wish to configure? (or 'done') [iec0] done

[driver] [UNHANDLED PROMPT] tail=b\" (or 'done') [iec0] done\\r\\nDNS domain name? (e.g. 'example.com') [my.domain] \" -- sending bare newline
DNS domain name? (e.g. 'example.com') [my.domain] 

[driver] [UNHANDLED PROMPT] tail=b\"\\r\\nDNS nameservers? (IP address list or 'none') [none] \" -- sending bare newline
DNS nameservers? (IP address list or 'none') [none] 


[driver] root-password -> b'origami123\\n'
Password for root account? (will not echo) 

[driver] root-password -> b'origami123\\n'
Password for root account? (again) 

[driver] sshd-default -> b'yes\\n'
Start sshd(8) by default? [yes] yes

[driver] setup-user -> b'n\\n'
Setup a user? (enter a lower-case loginname, or 'no') [no] n
Since no user was setup, root logins via sshd(8) might be useful.
WARNING: root is targeted by password guessing attacks, pubkeys are safer.

[driver] [UNHANDLED PROMPT] tail=b't is targeted by password guessing attacks, pubkeys are safer.\\r\\nAllow root ssh login? (yes, no, prohibit-password) [no] ' -- sending bare newline
Allow root ssh login? (yes, no, prohibit-password) [no] 

Available disks are: sd0 sd1.

[driver] root-disk -> b'sd0\\n'
Which disk is the root disk? ('?' for details) [sd0] sd0

Checking SGI Volume Header:

There is no Volume Header found on the disk. A Volume
header is required to be able to boot from the disk.

[driver] [UNHANDLED PROMPT] tail=b'n the disk. A Volume\\r\\nheader is required to be able to boot from the disk.\\r\\nDo you want to install a Volume Header? [y] ' -- sending bare newline
Do you want to install a Volume Header? [y] 
The auto-allocated layout for sd0 is:
#                size           offset  fstype [fsize bsize   cpg]
  a:          1978.2M             3135  4.2BSD   2048 16384     1 # /
  b:            68.3M          4054432    swap                    
  c:          2048.0M                0  unused                    
  p:             1.5M                0 unknown                    

[driver] disklabel-layout -> b'a\\n'
Use (A)uto layout, (E)dit auto layout, or create (C)ustom layout? [a] a
/dev/rsd0a: 1978.2MB in 4051296 sectors of 512 bytes
10 cylinder groups of 202.47MB, 12958 blocks, 25984 inodes each
Available disks are: sd1.

[driver] [UNHANDLED PROMPT] tail=b\"B, 12958 blocks, 25984 inodes each\\r\\nAvailable disks are: sd1.\\r\\nWhich disk do you wish to initialize? (or 'done') [done] \" -- sending bare newline
Which disk do you wish to initialize? (or 'done') [done] 
/dev/sd0a (18cc54198c86b2cb.a) on /mnt type ffs (rw, asynchronous, local)

Let's install the sets!

[driver] location-of-sets (occurrence 1) -> b'disk\\n'
Location of sets? (disk http nfs or 'done') [http] disk

[driver] already-mounted -> b'no\\n'
Is the disk partition already mounted? [yes] no
Available disks are: sd0 sd1.

[driver] which-disk-sets -> b'sd1\\n'
Which disk contains the install media? (or 'done') [sd1] sd1

[driver] set-path -> b'.\\n'
Pathname to the sets? (or 'done') [6.9/sgi] .

Select sets by entering a set name, a file name pattern or 'all'. De-select
sets by prepending a '-', e.g.: '-game*'. Selected sets are labelled '[X]'.
    [X] bsd.IP27      [X] base69.tgz    [X] comp69.tgz

[driver] set-name -> b'done\\n'
Set name(s)? (or 'abort' or 'done') [done] done

Verifying  SHA256.sig     0% |                          |     0       --:-- ETA
Verifying  SHA256.sig   100% |**************************|  2667       00:00    
Signature Verified

Verifying  bsd.IP27       0% |                          |     0       --:-- ETA
Verifying  bsd.IP27      36% |*********                 |  2816 KB    00:01 ETA
Verifying  bsd.IP27     100% |**************************|  7764 KB    00:01    

Verifying  base69.tgz     0% |                          |     0       --:-- ETA
Verifying  base69.tgz     1% |                          |  3968 KB    00:58 ETA
Verifying  base69.tgz     4% |*                         |  9216 KB    00:48 ETA
Verifying  base69.tgz     5% |*                         | 13312 KB    00:48 ETA
Verifying  base69.tgz     7% |*                         | 17280 KB    00:49 ETA
Verifying  base69.tgz    10% |**                        | 23040 KB    00:44 ETA
Verifying  base69.tgz    11% |**                        | 27392 KB    00:44 ETA
Verifying  base69.tgz    14% |***                       | 32384 KB    00:42 ETA
Verifying  base69.tgz    16% |****                      | 36736 KB    00:42 ETA
Verifying  base69.tgz    18% |****                      | 42752 KB    00:39 ETA
Verifying  base69.tgz    21% |*****                     | 48384 KB    00:37 ETA
Verifying  base69.tgz    22% |*****                     | 52480 KB    00:37 ETA
Verifying  base69.tgz    24% |******                    | 57216 KB    00:36 ETA
Verifying  base69.tgz    27% |*******                   | 62720 KB    00:34 ETA
Verifying  base69.tgz    29% |*******                   | 68352 KB    00:33 ETA
Verifying  base69.tgz    31% |********                  | 72192 KB    00:32 ETA
Verifying  base69.tgz    33% |********                  | 77056 KB    00:31 ETA
Verifying  base69.tgz    35% |*********                 | 81920 KB    00:30 ETA
Verifying  base69.tgz    38% |*********                 | 87296 KB    00:29 ETA
Verifying  base69.tgz    40% |**********                | 93440 KB    00:27 ETA
Verifying  base69.tgz    42% |**********                | 98048 KB    00:26 ETA
Verifying  base69.tgz    44% |***********               |   100 MB    00:25 ETA
Verifying  base69.tgz    46% |***********               |   105 MB    00:24 ETA
Verifying  base69.tgz    49% |************              |   110 MB    00:23 ETA
Verifying  base69.tgz    52% |*************             |   116 MB    00:22 ETA
Verifying  base69.tgz    54% |**************            |   122 MB    00:20 ETA
Verifying  base69.tgz    57% |**************            |   128 MB    00:19 ETA
Verifying  base69.tgz    59% |***************           |   133 MB    00:18 ETA
Verifying  base69.tgz    61% |***************           |   138 MB    00:17 ETA
Verifying  base69.tgz    63% |****************          |   142 MB    00:16 ETA
Verifying  base69.tgz    66% |*****************         |   148 MB    00:15 ETA
Verifying  base69.tgz    68% |*****************         |   154 MB    00:14 ETA
Verifying  base69.tgz    70% |******************        |   158 MB    00:13 ETA
Verifying  base69.tgz    72% |******************        |   162 MB    00:12 ETA
Verifying  base69.tgz    74% |*******************       |   165 MB    00:12 ETA
Verifying  base69.tgz    75% |*******************       |   169 MB    00:11 ETA
Verifying  base69.tgz    78% |********************      |   175 MB    00:09 ETA
Verifying  base69.tgz    80% |********************      |   180 MB    00:08 ETA
Verifying  base69.tgz    82% |*********************     |   185 MB    00:07 ETA
Verifying  base69.tgz    84% |*********************     |   190 MB    00:06 ETA
Verifying  base69.tgz    87% |**********************    |   194 MB    00:05 ETA
Verifying  base69.tgz    89% |***********************   |   199 MB    00:04 ETA
Verifying  base69.tgz    91% |***********************   |   205 MB    00:03 ETA
Verifying  base69.tgz    94% |************************  |   210 MB    00:02 ETA
Verifying  base69.tgz    96% |************************  |   216 MB    00:01 ETA
Verifying  base69.tgz    98% |************************* |   220 MB    00:00 ETA
Verifying  base69.tgz   100% |**************************|   223 MB    00:45    

Verifying  comp69.tgz     0% |                          |     0       --:-- ETA
Verifying  comp69.tgz     6% |*                         |  4480 KB    00:15 ETA
Verifying  comp69.tgz    13% |***                       |  9984 KB    00:13 ETA
Verifying  comp69.tgz    22% |*****                     | 16768 KB    00:10 ETA
Verifying  comp69.tgz    30% |*******                   | 22656 KB    00:09 ETA
Verifying  comp69.tgz    37% |*********                 | 27648 KB    00:08 ETA
Verifying  comp69.tgz    44% |***********               | 33152 KB    00:07 ETA
Verifying  comp69.tgz    53% |*************             | 39552 KB    00:06 ETA
Verifying  comp69.tgz    61% |***************           | 45184 KB    00:05 ETA
Verifying  comp69.tgz    68% |*****************         | 50944 KB    00:04 ETA
Verifying  comp69.tgz    75% |*******************       | 55936 KB    00:03 ETA
Verifying  comp69.tgz    83% |*********************     | 62080 KB    00:02 ETA
Verifying  comp69.tgz    92% |***********************   | 68480 KB    00:00 ETA
Verifying  comp69.tgz    99% |************************* | 73600 KB    00:00 ETA
Verifying  comp69.tgz   100% |**************************| 73952 KB    00:13    

Installing bsd.IP27       0% |                          |     0       --:-- ETA
Installing bsd.IP27      62% |****************          |  4864 KB    00:00 ETA
Installing bsd.IP27     100% |**************************|  7764 KB    00:01    

Installing base69.tgz     0% |                          |     0       --:-- ETA
Installing base69.tgz     0% |                          |   896 KB    04:20 ETA
Installing base69.tgz     0% |                          |  1664 KB    04:36 ETA
Installing base69.tgz     1% |                          |  2688 KB    04:13 ETA
Installing base69.tgz     1% |                          |  3584 KB    04:12 ETA
Installing base69.tgz     1% |                          |  4352 KB    04:18 ETA
Installing base69.tgz     2% |                          |  5248 KB    04:16 ETA
Installing base69.tgz     2% |                          |  6016 KB    04:20 ETA
Installing base69.tgz     2% |                          |  6784 KB    04:22 ETA
Installing base69.tgz     3% |                          |  7680 KB    04:20 ETA
Installing base69.tgz     3% |                          |  8320 KB    04:25 ETA
Installing base69.tgz     3% |                          |  8832 KB    04:35 ETA
Installing base69.tgz     4% |*                         |  9728 KB    04:31 ETA
Installing base69.tgz     4% |*                         | 10752 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 11520 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 12416 KB    04:22 ETA
Installing base69.tgz     5% |*                         | 13056 KB    04:25 ETA
Installing base69.tgz     5% |*                         | 13440 KB    04:33 ETA
Installing base69.tgz     6% |*                         | 14208 KB    04:32 ETA
Installing base69.tgz     6% |*                         | 14848 KB    04:34 ETA
Installing base69.tgz     6% |*                         | 15616 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 16384 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 17152 KB    04:32 ETA
Installing base69.tgz     7% |*                         | 17920 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 18560 KB    04:32 ETA
Installing base69.tgz     8% |**                        | 19328 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 20224 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 20992 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 21888 KB    04:25 ETA
Installing base69.tgz     9% |**                        | 22656 KB    04:24 ETA
Installing base69.tgz    10% |**                        | 23552 KB    04:22 ETA
Installing base69.tgz    10% |**                        | 24320 KB    04:21 ETA
Installing base69.tgz    10% |**                        | 25216 KB    04:19 ETA
Installing base69.tgz    11% |**                        | 26112 KB    04:16 ETA
Installing base69.tgz    11% |**                        | 26880 KB    04:16 ETA
Installing base69.tgz    12% |***                       | 27776 KB    04:14 ETA
Installing base69.tgz    12% |***                       | 28672 KB    04:12 ETA
Installing base69.tgz    12% |***                       | 29440 KB    04:11 ETA
Installing base69.tgz    13% |***                       | 30336 KB    04:09 ETA
Installing base69.tgz    13% |***                       | 31232 KB    04:07 ETA
Installing base69.tgz    13% |***                       | 32000 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 32768 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 33280 KB    04:07 ETA
Installing base69.tgz    14% |***                       | 34176 KB    04:05 ETA
Installing base69.tgz    15% |***                       | 34944 KB    04:04 ETA
Installing base69.tgz    15% |***                       | 35840 KB    04:03 ETA
Installing base69.tgz    15% |***                       | 36608 KB    04:02 ETA
Installing base69.tgz    16% |****                      | 37632 KB    03:59 ETA
Installing base69.tgz    16% |****                      | 38528 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 39296 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 40320 KB    03:54 ETA
Installing base69.tgz    17% |****                      | 41216 KB    03:52 ETA
Installing base69.tgz    18% |****                      | 42112 KB    03:51 ETA
Installing base69.tgz    18% |****                      | 42752 KB    03:51 ETA
Installing base69.tgz    19% |****                      | 43648 KB    03:49 ETA
Installing base69.tgz    19% |****                      | 44544 KB    03:48 ETA
Installing base69.tgz    19% |****                      | 45440 KB    03:46 ETA
Installing base69.tgz    20% |*****                     | 46208 KB    03:45 ETA
Installing base69.tgz    20% |*****                     | 47104 KB    03:44 ETA
Installing base69.tgz    20% |*****                     | 48000 KB    03:42 ETA
Installing base69.tgz    21% |*****                     | 48896 KB    03:41 ETA
Installing base69.tgz    21% |*****                     | 49792 KB    03:39 ETA
Installing base69.tgz    22% |*****                     | 50688 KB    03:38 ETA
Installing base69.tgz    22% |*****                     | 51584 KB    03:37 ETA
Installing base69.tgz    22% |*****                     | 52480 KB    03:35 ETA
Installing base69.tgz    23% |*****                     | 53248 KB    03:34 ETA
Installing base69.tgz    23% |*****                     | 54144 KB    03:33 ETA
Installing base69.tgz    23% |*****                     | 54912 KB    03:32 ETA
Installing base69.tgz    24% |******                    | 55808 KB    03:31 ETA
Installing base69.tgz    24% |******                    | 56704 KB    03:30 ETA
Installing base69.tgz    25% |******                    | 57472 KB    03:29 ETA
Installing base69.tgz    25% |******                    | 58240 KB    03:28 ETA
Installing base69.tgz    25% |******                    | 59008 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 59776 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 60544 KB    03:26 ETA
Installing base69.tgz    26% |******                    | 61440 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 61952 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 62592 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 63360 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 64000 KB    03:24 ETA
Installing base69.tgz    28% |*******                   | 64640 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 65280 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 66176 KB    03:22 ETA
Installing base69.tgz    29% |*******                   | 66944 KB    03:21 ETA
Installing base69.tgz    29% |*******                   | 67712 KB    03:20 ETA
Installing base69.tgz    29% |*******                   | 68480 KB    03:19 ETA
Installing base69.tgz    30% |*******                   | 69376 KB    03:18 ETA
Installing base69.tgz    30% |*******                   | 70272 KB    03:16 ETA
Installing base69.tgz    31% |********                  | 71168 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 71680 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 72448 KB    03:14 ETA
Installing base69.tgz    31% |********                  | 72960 KB    03:15 ETA
Installing base69.tgz    32% |********                  | 73600 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74112 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74752 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 75264 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 75904 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76416 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76928 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 77696 KB    03:13 ETA
Installing base69.tgz    34% |********                  | 78336 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 78976 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 79488 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 80000 KB    03:12 ETA
Installing base69.tgz    35% |*********                 | 80896 KB    03:10 ETA
Installing base69.tgz    35% |*********                 | 81920 KB    03:08 ETA
Installing base69.tgz    35% |*********                 | 82560 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83328 KB    03:07 ETA
Installing base69.tgz    36% |*********                 | 83456 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83712 KB    03:09 ETA
Installing base69.tgz    36% |*********                 | 84480 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 84992 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 85760 KB    03:07 ETA
Installing base69.tgz    37% |*********                 | 86528 KB    03:06 ETA
Installing base69.tgz    38% |*********                 | 87424 KB    03:05 ETA
Installing base69.tgz    38% |*********                 | 88064 KB    03:04 ETA
Installing base69.tgz    38% |*********                 | 88832 KB    03:03 ETA
Installing base69.tgz    39% |**********                | 89728 KB    03:02 ETA
Installing base69.tgz    39% |**********                | 90496 KB    03:01 ETA
Installing base69.tgz    39% |**********                | 91392 KB    02:59 ETA
Installing base69.tgz    40% |**********                | 92160 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 92800 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 93696 KB    02:56 ETA
Installing base69.tgz    41% |**********                | 94592 KB    02:55 ETA
Installing base69.tgz    41% |**********                | 95360 KB    02:54 ETA
Installing base69.tgz    42% |**********                | 96384 KB    02:52 ETA
Installing base69.tgz    42% |**********                | 97408 KB    02:50 ETA
Installing base69.tgz    42% |**********                | 98304 KB    02:49 ETA
Installing base69.tgz    43% |***********               | 99200 KB    02:47 ETA
Installing base69.tgz    43% |***********               | 99968 KB    02:46 ETA
Installing base69.tgz    44% |***********               |    98 MB    02:45 ETA
Installing base69.tgz    44% |***********               |    99 MB    02:43 ETA
Installing base69.tgz    44% |***********               |   100 MB    02:41 ETA
Installing base69.tgz    45% |***********               |   101 MB    02:40 ETA
Installing base69.tgz    45% |***********               |   102 MB    02:38 ETA
Installing base69.tgz    46% |***********               |   103 MB    02:37 ETA
Installing base69.tgz    46% |***********               |   104 MB    02:36 ETA
Installing base69.tgz    46% |***********               |   105 MB    02:34 ETA
Installing base69.tgz    47% |************              |   106 MB    02:33 ETA
Installing base69.tgz    47% |************              |   107 MB    02:31 ETA
Installing base69.tgz    48% |************              |   108 MB    02:29 ETA
Installing base69.tgz    48% |************              |   109 MB    02:28 ETA
Installing base69.tgz    49% |************              |   109 MB    02:27 ETA
Installing base69.tgz    49% |************              |   110 MB    02:26 ETA
Installing base69.tgz    49% |************              |   111 MB    02:24 ETA
Installing base69.tgz    50% |*************             |   112 MB    02:23 ETA
Installing base69.tgz    50% |*************             |   113 MB    02:21 ETA
Installing base69.tgz    51% |*************             |   114 MB    02:20 ETA
Installing base69.tgz    51% |*************             |   115 MB    02:18 ETA
Installing base69.tgz    52% |*************             |   116 MB    02:17 ETA
Installing base69.tgz    52% |*************             |   117 MB    02:16 ETA
Installing base69.tgz    52% |*************             |   118 MB    02:15 ETA
Installing base69.tgz    53% |*************             |   119 MB    02:14 ETA
Installing base69.tgz    53% |*************             |   120 MB    02:12 ETA
Installing base69.tgz    54% |**************            |   121 MB    02:11 ETA
Installing base69.tgz    54% |**************            |   122 MB    02:09 ETA
Installing base69.tgz    54% |**************            |   123 MB    02:08 ETA
Installing base69.tgz    55% |**************            |   124 MB    02:06 ETA
Installing base69.tgz    55% |**************            |   124 MB    02:05 ETA
Installing base69.tgz    56% |**************            |   125 MB    02:04 ETA
Installing base69.tgz    56% |**************            |   126 MB    02:03 ETA
Installing base69.tgz    56% |**************            |   127 MB    02:02 ETA
Installing base69.tgz    57% |**************            |   128 MB    02:00 ETA
Installing base69.tgz    57% |**************            |   129 MB    01:59 ETA
Installing base69.tgz    58% |***************           |   130 MB    01:58 ETA
Installing base69.tgz    58% |***************           |   131 MB    01:56 ETA
Installing base69.tgz    58% |***************           |   132 MB    01:55 ETA
Installing base69.tgz    59% |***************           |   133 MB    01:54 ETA
Installing base69.tgz    59% |***************           |   133 MB    01:53 ETA
Installing base69.tgz    60% |***************           |   134 MB    01:51 ETA
Installing base69.tgz    60% |***************           |   135 MB    01:50 ETA
Installing base69.tgz    60% |***************           |   136 MB    01:49 ETA
Installing base69.tgz    61% |***************           |   137 MB    01:49 ETA
Installing base69.tgz    61% |***************           |   138 MB    01:47 ETA
Installing base69.tgz    62% |****************          |   139 MB    01:46 ETA
Installing base69.tgz    62% |****************          |   140 MB    01:44 ETA
Installing base69.tgz    62% |****************          |   140 MB    01:43 ETA
Installing base69.tgz    63% |****************          |   141 MB    01:42 ETA
Installing base69.tgz    63% |****************          |   142 MB    01:41 ETA
Installing base69.tgz    64% |****************          |   143 MB    01:40 ETA
Installing base69.tgz    64% |****************          |   144 MB    01:38 ETA
Installing base69.tgz    65% |****************          |   145 MB    01:37 ETA
Installing base69.tgz    65% |****************          |   146 MB    01:36 ETA
Installing base69.tgz    65% |****************          |   147 MB    01:34 ETA
Installing base69.tgz    66% |*****************         |   148 MB    01:33 ETA
Installing base69.tgz    66% |*****************         |   149 MB    01:32 ETA
Installing base69.tgz    67% |*****************         |   150 MB    01:31 ETA
Installing base69.tgz    67% |*****************         |   151 MB    01:29 ETA
Installing base69.tgz    67% |*****************         |   152 MB    01:28 ETA
Installing base69.tgz    68% |*****************         |   153 MB    01:27 ETA
Installing base69.tgz    68% |*****************         |   154 MB    01:26 ETA
Installing base69.tgz    69% |*****************         |   154 MB    01:25 ETA
Installing base69.tgz    69% |*****************         |   155 MB    01:23 ETA
Installing base69.tgz    69% |*****************         |   156 MB    01:22 ETA
Installing base69.tgz    70% |******************        |   157 MB    01:21 ETA
Installing base69.tgz    70% |******************        |   158 MB    01:20 ETA
Installing base69.tgz    71% |******************        |   159 MB    01:19 ETA
Installing base69.tgz    71% |******************        |   160 MB    01:17 ETA
Installing base69.tgz    72% |******************        |   161 MB    01:16 ETA
Installing base69.tgz    72% |******************        |   162 MB    01:15 ETA
Installing base69.tgz    72% |******************        |   163 MB    01:14 ETA
Installing base69.tgz    73% |******************        |   164 MB    01:13 ETA
Installing base69.tgz    73% |******************        |   165 MB    01:11 ETA
Installing base69.tgz    74% |*******************       |   166 MB    01:10 ETA
Installing base69.tgz    74% |*******************       |   167 MB    01:09 ETA
Installing base69.tgz    75% |*******************       |   168 MB    01:08 ETA
Installing base69.tgz    75% |*******************       |   169 MB    01:06 ETA
Installing base69.tgz    75% |*******************       |   170 MB    01:05 ETA
Installing base69.tgz    76% |*******************       |   170 MB    01:04 ETA
Installing base69.tgz    76% |*******************       |   171 MB    01:03 ETA
Installing base69.tgz    77% |********************      |   172 MB    01:02 ETA
Installing base69.tgz    77% |********************      |   173 MB    01:01 ETA
Installing base69.tgz    77% |********************      |   174 MB    01:00 ETA
Installing base69.tgz    78% |********************      |   175 MB    00:58 ETA
Installing base69.tgz    78% |********************      |   176 MB    00:57 ETA
Installing base69.tgz    79% |********************      |   177 MB    00:56 ETA
Installing base69.tgz    79% |********************      |   178 MB    00:55 ETA
Installing base69.tgz    80% |********************      |   179 MB    00:53 ETA
Installing base69.tgz    80% |********************      |   180 MB    00:53 ETA
Installing base69.tgz    80% |********************      |   180 MB    00:52 ETA
Installing base69.tgz    81% |*********************     |   181 MB    00:51 ETA
Installing base69.tgz    81% |*********************     |   182 MB    00:49 ETA
Installing base69.tgz    81% |*********************     |   183 MB    00:48 ETA
Installing base69.tgz    82% |*********************     |   184 MB    00:47 ETA
Installing base69.tgz    82% |*********************     |   185 MB    00:46 ETA
Installing base69.tgz    83% |*********************     |   186 MB    00:44 ETA
Installing base69.tgz    83% |*********************     |   187 MB    00:43 ETA
Installing base69.tgz    84% |*********************     |   188 MB    00:42 ETA
Installing base69.tgz    84% |*********************     |   189 MB    00:41 ETA
Installing base69.tgz    85% |**********************    |   190 MB    00:40 ETA
Installing base69.tgz    85% |**********************    |   191 MB    00:38 ETA
Installing base69.tgz    86% |**********************    |   192 MB    00:37 ETA
Installing base69.tgz    86% |**********************    |   193 MB    00:36 ETA
Installing base69.tgz    87% |**********************    |   194 MB    00:34 ETA
Installing base69.tgz    87% |**********************    |   195 MB    00:33 ETA
Installing base69.tgz    87% |**********************    |   196 MB    00:32 ETA
Installing base69.tgz    88% |**********************    |   197 MB    00:31 ETA
Installing base69.tgz    88% |**********************    |   198 MB    00:30 ETA
Installing base69.tgz    89% |***********************   |   199 MB    00:29 ETA
Installing base69.tgz    89% |***********************   |   200 MB    00:27 ETA
Installing base69.tgz    90% |***********************   |   201 MB    00:26 ETA
Installing base69.tgz    90% |***********************   |   202 MB    00:25 ETA
Installing base69.tgz    90% |***********************   |   203 MB    00:24 ETA
Installing base69.tgz    91% |***********************   |   204 MB    00:22 ETA
Installing base69.tgz    91% |***********************   |   205 MB    00:21 ETA
Installing base69.tgz    92% |***********************   |   206 MB    00:20 ETA
Installing base69.tgz    92% |***********************   |   207 MB    00:19 ETA
Installing base69.tgz    92% |***********************   |   208 MB    00:18 ETA
Installing base69.tgz    93% |************************  |   209 MB    00:17 ETA
Installing base69.tgz    93% |************************  |   210 MB    00:16 ETA
Installing base69.tgz    94% |************************  |   211 MB    00:15 ETA
Installing base69.tgz    94% |************************  |   212 MB    00:13 ETA
Installing base69.tgz    95% |************************  |   213 MB    00:12 ETA
Installing base69.tgz    95% |************************  |   213 MB    00:12 ETA
Installing base69.tgz    95% |************************  |   214 MB    00:11 ETA
Installing base69.tgz    95% |************************  |   215 MB    00:10 ETA
Installing base69.tgz    96% |************************  |   215 MB    00:09 ETA
Installing base69.tgz    96% |************************  |   216 MB    00:08 ETA
Installing base69.tgz    97% |************************* |   217 MB    00:07 ETA
Installing base69.tgz    97% |************************* |   218 MB    00:06 ETA
Installing base69.tgz    97% |************************* |   219 MB    00:05 ETA
Installing base69.tgz    98% |************************* |   219 MB    00:04 ETA
Installing base69.tgz    98% |************************* |   220 MB    00:03 ETA
Installing base69.tgz    98% |************************* |   221 MB    00:02 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:02 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:00 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:00 ETA
Installing base69.tgz   100% |**************************|   223 MB    04:33    

Extracting etc.tgz        0% |                          |     0       --:-- ETA
Extracting etc.tgz      100% |**************************|   254 KB    00:00    

Installing comp69.tgz     0% |                          |     0       --:-- ETA
Installing comp69.tgz     1% |                          |   768 KB    01:35 ETA
Installing comp69.tgz     1% |                          |  1408 KB    01:43 ETA
Installing comp69.tgz     2% |                          |  2048 KB    01:45 ETA
Installing comp69.tgz     3% |                          |  2816 KB    01:41 ETA
Installing comp69.tgz     5% |*                         |  3712 KB    01:34 ETA
Installing comp69.tgz     6% |*                         |  4480 KB    01:33 ETA
Installing comp69.tgz     6% |*                         |  5120 KB    01:34 ETA
Installing comp69.tgz     7% |*                         |  5888 KB    01:32 ETA
Installing comp69.tgz     9% |**                        |  6784 KB    01:29 ETA
Installing comp69.tgz    10% |**                        |  7680 KB    01:26 ETA
Installing comp69.tgz    11% |**                        |  8576 KB    01:23 ETA
Installing comp69.tgz    12% |***                       |  9344 KB    01:23 ETA
Installing comp69.tgz    13% |***                       | 10240 KB    01:20 ETA
Installing comp69.tgz    15% |***                       | 11136 KB    01:18 ETA
Installing comp69.tgz    16% |****                      | 11904 KB    01:18 ETA
Installing comp69.tgz    17% |****                      | 12800 KB    01:16 ETA
Installing comp69.tgz    18% |****                      | 13696 KB    01:14 ETA
Installing comp69.tgz    19% |****                      | 14720 KB    01:12 ETA
Installing comp69.tgz    21% |*****                     | 15744 KB    01:10 ETA
Installing comp69.tgz    22% |*****                     | 16512 KB    01:09 ETA
Installing comp69.tgz    23% |*****                     | 17664 KB    01:06 ETA
Installing comp69.tgz    24% |******                    | 18432 KB    01:06 ETA
Installing comp69.tgz    26% |******                    | 19456 KB    01:04 ETA
Installing comp69.tgz    27% |*******                   | 20352 KB    01:03 ETA
Installing comp69.tgz    28% |*******                   | 21120 KB    01:02 ETA
Installing comp69.tgz    29% |*******                   | 22016 KB    01:01 ETA
Installing comp69.tgz    30% |*******                   | 22912 KB    01:00 ETA
Installing comp69.tgz    32% |********                  | 23680 KB    00:59 ETA
Installing comp69.tgz    33% |********                  | 24448 KB    00:58 ETA
Installing comp69.tgz    34% |********                  | 25344 KB    00:57 ETA
Installing comp69.tgz    35% |*********                 | 25984 KB    00:57 ETA
Installing comp69.tgz    36% |*********                 | 26752 KB    00:56 ETA
Installing comp69.tgz    37% |*********                 | 27776 KB    00:54 ETA
Installing comp69.tgz    38% |*********                 | 28672 KB    00:53 ETA
Installing comp69.tgz    40% |**********                | 29696 KB    00:52 ETA
Installing comp69.tgz    41% |**********                | 30592 KB    00:51 ETA
Installing comp69.tgz    42% |**********                | 31360 KB    00:50 ETA
Installing comp69.tgz    43% |***********               | 32000 KB    00:49 ETA
Installing comp69.tgz    44% |***********               | 32768 KB    00:49 ETA
Installing comp69.tgz    45% |***********               | 33536 KB    00:48 ETA
Installing comp69.tgz    46% |***********               | 34176 KB    00:47 ETA
Installing comp69.tgz    47% |************              | 35072 KB    00:46 ETA
Installing comp69.tgz    48% |************              | 35712 KB    00:46 ETA
Installing comp69.tgz    49% |************              | 36608 KB    00:44 ETA
Installing comp69.tgz    50% |*************             | 37376 KB    00:44 ETA
Installing comp69.tgz    51% |*************             | 38272 KB    00:42 ETA
Installing comp69.tgz    52% |*************             | 39040 KB    00:42 ETA
Installing comp69.tgz    54% |**************            | 40064 KB    00:40 ETA
Installing comp69.tgz    55% |**************            | 41088 KB    00:39 ETA
Installing comp69.tgz    56% |**************            | 41728 KB    00:38 ETA
Installing comp69.tgz    58% |***************           | 43008 KB    00:36 ETA
Installing comp69.tgz    59% |***************           | 43776 KB    00:35 ETA
Installing comp69.tgz    60% |***************           | 44928 KB    00:34 ETA
Installing comp69.tgz    61% |***************           | 45824 KB    00:33 ETA
Installing comp69.tgz    63% |****************          | 46592 KB    00:32 ETA
Installing comp69.tgz    64% |****************          | 47616 KB    00:30 ETA
Installing comp69.tgz    65% |****************          | 48640 KB    00:29 ETA
Installing comp69.tgz    67% |*****************         | 49664 KB    00:28 ETA
Installing comp69.tgz    68% |*****************         | 50560 KB    00:27 ETA
Installing comp69.tgz    69% |*****************         | 51328 KB    00:26 ETA
Installing comp69.tgz    70% |******************        | 51968 KB    00:25 ETA
Installing comp69.tgz    71% |******************        | 52736 KB    00:24 ETA
Installing comp69.tgz    72% |******************        | 53376 KB    00:24 ETA
Installing comp69.tgz    73% |******************        | 54272 KB    00:23 ETA
Installing comp69.tgz    74% |*******************       | 55296 KB    00:21 ETA
Installing comp69.tgz    75% |*******************       | 55552 KB    00:21 ETA
Installing comp69.tgz    75% |*******************       | 56064 KB    00:21 ETA
Installing comp69.tgz    76% |*******************       | 56576 KB    00:20 ETA
Installing comp69.tgz    77% |********************      | 57088 KB    00:20 ETA
Installing comp69.tgz    77% |********************      | 57600 KB    00:19 ETA
Installing comp69.tgz    78% |********************      | 58112 KB    00:19 ETA
Installing comp69.tgz    78% |********************      | 58368 KB    00:19 ETA
Installing comp69.tgz    79% |********************      | 58624 KB    00:19 ETA
Installing comp69.tgz    79% |********************      | 58880 KB    00:18 ETA
Installing comp69.tgz    80% |********************      | 59392 KB    00:18 ETA
Installing comp69.tgz    81% |*********************     | 59904 KB    00:17 ETA
Installing comp69.tgz    81% |*********************     | 60160 KB    00:17 ETA
Installing comp69.tgz    81% |*********************     | 60544 KB    00:17 ETA
Installing comp69.tgz    82% |*********************     | 61056 KB    00:16 ETA
Installing comp69.tgz    83% |*********************     | 61440 KB    00:16 ETA
Installing comp69.tgz    83% |*********************     | 61824 KB    00:15 ETA
Installing comp69.tgz    83% |*********************     | 62080 KB    00:15 ETA
Installing comp69.tgz    84% |*********************     | 62592 KB    00:15 ETA
Installing comp69.tgz    85% |**********************    | 62976 KB    00:14 ETA
Installing comp69.tgz    86% |**********************    | 63744 KB    00:13 ETA
Installing comp69.tgz    87% |**********************    | 65024 KB    00:11 ETA
Installing comp69.tgz    89% |***********************   | 65920 KB    00:10 ETA
Installing comp69.tgz    90% |***********************   | 67072 KB    00:09 ETA
Installing comp69.tgz    92% |***********************   | 68096 KB    00:07 ETA
Installing comp69.tgz    93% |************************  | 69120 KB    00:06 ETA
Installing comp69.tgz    95% |************************  | 70272 KB    00:04 ETA
Installing comp69.tgz    96% |************************  | 71040 KB    00:03 ETA
Installing comp69.tgz    97% |************************* | 72064 KB    00:02 ETA
Installing comp69.tgz    98% |************************* | 72576 KB    00:01 ETA
Installing comp69.tgz    98% |************************* | 72960 KB    00:01 ETA
Installing comp69.tgz    99% |************************* | 73216 KB    00:00 ETA
Installing comp69.tgz    99% |************************* | 73472 KB    00:00 ETA
Installing comp69.tgz    99% |************************* | 73728 KB    00:00 ETA
Installing comp69.tgz   100% |**************************| 73952 KB    00:00 ETA
Installing comp69.tgz   100% |**************************| 73952 KB    01:39    

[driver] location-of-sets (occurrence 2) -> b'done\\n'
Location of sets? (disk http nfs or 'done') [done] done


[driver] timezone -> b'UTC\\n'
What timezone are you in? ('?' for list) [Canada/Mountain] UTC
Saving configuration files... done.
Making all device nodes... done.
Installing boot loader in volume header.
Writing file /mnt/usr/mdec/boot-IP27
File /mnt/usr/mdec/boot-IP27 has 116816 bytes
disklabel shows 4194304 sectors with 512 bytes per sector
checksum: 00000000
root part: 0
swap part: 1
bootfile: /bsd

Volume header files:
boot     offset    2 blocks, length   116816 bytes (229 blocks)

SGI partitions:
 0:a blocks  4191169 first     3135 type  4 (BSD4.2)
 8:i blocks     3135 first        0 type  0 (Volume Header)
10:k blocks  4194304 first        0 type  6 (Volume)
Relinking to create unique kernel... done.

CONGRATULATIONS! Your OpenBSD install has been successfully completed!

When you login to your new system the first time, please read your mail
using the 'mail' command.


INSTALL.sgi describes how to configure the ARCS PROM to boot OpenBSD.

[driver] [UNHANDLED PROMPT] tail=b'\\n\\r\\nINSTALL.sgi describes how to configure the ARCS PROM to boot OpenBSD.\\r\\nExit to (S)hell, (H)alt or (R)eboot? [reboot] ' -- sending bare newline
Exit to (S)hell, (H)alt or (R)eboot? [reboot] 
syncing disks... done

System restart.

Failed!!! Please reset manually.

"""

TRUNCATED_BASE69_TRANSCRIPT = """\
ARCS64 Firmware
Found SGI-IP27, setting up.
Machine is in M mode.
Initial setup done, switching console.

Copyright (c) 1982, 1986, 1989, 1991, 1993

\tThe Regents of the University of California.  All rights reserved.

Copyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org


OpenBSD 6.9 (RAMDISK-IP27) #0: Thu Apr 22 10:25:14 UTC 2021

    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/RAMDISK-IP27

real mem = 268435456 (256MB)

rsvd mem = 16171008 (16MB)

avail mem = 249331712 (237MB)

random: boothowto does not indicate good seed

mainbus0 at root: Origin 200

cpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0

cpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way

clock0 at mainbus0 nasid 0: int 5

xbow0 at mainbus0 nasid 0: Bridge revision 3

xbridge0 at xbow0 widget 8: Bridge revision 3

xbpci0 at xbridge0 bus 0: 33 MHz PCI bus

pci0 at xbpci0 bus 0

qlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62

qlw0: nvram corrupt

qlw0: firmware rev 1.0.0, attrs 0x0

scsibus0 at qlw0: 16 targets, initiator 0

sd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd0: 2048MB, 512 bytes/sector, 4194304 sectors

sd1 at scsibus0 targ 2 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd1: 400MB, 512 bytes/sector, 819200 sectors

qlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61

qlw1: nvram corrupt

qlw1: firmware rev 1.0.0, attrs 0x0

scsibus1 at qlw1: 16 targets, initiator 0

ioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00

onewire0 at ioc0

ioc0: ethernet irq 2, xbow irq 60

ioc0: superio irq 3, xbow irq 59

com0 at ioc0 base 0x20178: ns16550a, 16 byte fifo

com0: console

com1 at ioc0 base 0x20170: ns16550a, 16 byte fifo

iockbc0 at ioc0

iec0 at ioc0: 128KB SSRAM, address ff:ff:ff:ff:ff:ff

nsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1

lpt at ioc0 not configured

dsrtc0 at ioc0: DS1742W

boot device: 'unknown' unrecognized.

root on rd0a swap on rd0b dump on rd0b

WARNING: CHECK AND RESET THE DATE!

erase ^?, werase ^W, kill ^U, intr ^C, status ^T

Welcome to the OpenBSD/sgi 6.9 installation program.

[driver] install-menu -> b'I\\n'
(I)nstall, (U)pgrade, (A)utoinstall or (S)hell? I
At any prompt except password prompts you can escape to a shell by
typing '!'. Default answers are shown in []'s and are selected by
pressing RETURN.  You can exit this program at any time by pressing
Control-C, but this can leave your system in an inconsistent state.


[driver] terminal-type -> b'vt220\\n'
Terminal type? [vt220] vt220

[driver] hostname -> b'origami\\n'
System hostname? (short form, e.g. 'foo') origami

Available network interfaces are: iec0 vlan0.

[driver] net-interface -> b'done\\n'
Which network interface do you wish to configure? (or 'done') [iec0] done

[driver] [UNHANDLED PROMPT] tail=b\" (or 'done') [iec0] done\\r\\nDNS domain name? (e.g. 'example.com') [my.domain] \" -- sending bare newline
DNS domain name? (e.g. 'example.com') [my.domain] 

[driver] [UNHANDLED PROMPT] tail=b\"\\r\\nDNS nameservers? (IP address list or 'none') [none] \" -- sending bare newline
DNS nameservers? (IP address list or 'none') [none] 


[driver] root-password -> b'origami123\\n'
Password for root account? (will not echo) 

[driver] root-password -> b'origami123\\n'
Password for root account? (again) 

[driver] sshd-default -> b'yes\\n'
Start sshd(8) by default? [yes] yes

[driver] setup-user -> b'n\\n'
Setup a user? (enter a lower-case loginname, or 'no') [no] n
Since no user was setup, root logins via sshd(8) might be useful.
WARNING: root is targeted by password guessing attacks, pubkeys are safer.

[driver] [UNHANDLED PROMPT] tail=b't is targeted by password guessing attacks, pubkeys are safer.\\r\\nAllow root ssh login? (yes, no, prohibit-password) [no] ' -- sending bare newline
Allow root ssh login? (yes, no, prohibit-password) [no] 

Available disks are: sd0 sd1.

[driver] root-disk -> b'sd0\\n'
Which disk is the root disk? ('?' for details) [sd0] sd0

Checking SGI Volume Header:

There is no Volume Header found on the disk. A Volume
header is required to be able to boot from the disk.

[driver] [UNHANDLED PROMPT] tail=b'n the disk. A Volume\\r\\nheader is required to be able to boot from the disk.\\r\\nDo you want to install a Volume Header? [y] ' -- sending bare newline
Do you want to install a Volume Header? [y] 
The auto-allocated layout for sd0 is:
#                size           offset  fstype [fsize bsize   cpg]
  a:          1978.2M             3135  4.2BSD   2048 16384     1 # /
  b:            68.3M          4054432    swap                    
  c:          2048.0M                0  unused                    
  p:             1.5M                0 unknown                    

[driver] disklabel-layout -> b'a\\n'
Use (A)uto layout, (E)dit auto layout, or create (C)ustom layout? [a] a
/dev/rsd0a: 1978.2MB in 4051296 sectors of 512 bytes
10 cylinder groups of 202.47MB, 12958 blocks, 25984 inodes each
Available disks are: sd1.

[driver] [UNHANDLED PROMPT] tail=b\"B, 12958 blocks, 25984 inodes each\\r\\nAvailable disks are: sd1.\\r\\nWhich disk do you wish to initialize? (or 'done') [done] \" -- sending bare newline
Which disk do you wish to initialize? (or 'done') [done] 
/dev/sd0a (18cc54198c86b2cb.a) on /mnt type ffs (rw, asynchronous, local)

Let's install the sets!

[driver] location-of-sets (occurrence 1) -> b'disk\\n'
Location of sets? (disk http nfs or 'done') [http] disk

[driver] already-mounted -> b'no\\n'
Is the disk partition already mounted? [yes] no
Available disks are: sd0 sd1.

[driver] which-disk-sets -> b'sd1\\n'
Which disk contains the install media? (or 'done') [sd1] sd1

[driver] set-path -> b'.\\n'
Pathname to the sets? (or 'done') [6.9/sgi] .

Select sets by entering a set name, a file name pattern or 'all'. De-select
sets by prepending a '-', e.g.: '-game*'. Selected sets are labelled '[X]'.
    [X] bsd.IP27      [X] base69.tgz    [X] comp69.tgz

[driver] set-name -> b'done\\n'
Set name(s)? (or 'abort' or 'done') [done] done

Verifying  SHA256.sig     0% |                          |     0       --:-- ETA
Verifying  SHA256.sig   100% |**************************|  2667       00:00    
Signature Verified

Verifying  bsd.IP27       0% |                          |     0       --:-- ETA
Verifying  bsd.IP27      36% |*********                 |  2816 KB    00:01 ETA
Verifying  bsd.IP27     100% |**************************|  7764 KB    00:01    

Verifying  base69.tgz     0% |                          |     0       --:-- ETA
Verifying  base69.tgz     1% |                          |  3968 KB    00:58 ETA
Verifying  base69.tgz     4% |*                         |  9216 KB    00:48 ETA
Verifying  base69.tgz     5% |*                         | 13312 KB    00:48 ETA
Verifying  base69.tgz     7% |*                         | 17280 KB    00:49 ETA
Verifying  base69.tgz    10% |**                        | 23040 KB    00:44 ETA
Verifying  base69.tgz    11% |**                        | 27392 KB    00:44 ETA
Verifying  base69.tgz    14% |***                       | 32384 KB    00:42 ETA
Verifying  base69.tgz    16% |****                      | 36736 KB    00:42 ETA
Verifying  base69.tgz    18% |****                      | 42752 KB    00:39 ETA
Verifying  base69.tgz    21% |*****                     | 48384 KB    00:37 ETA
Verifying  base69.tgz    22% |*****                     | 52480 KB    00:37 ETA
Verifying  base69.tgz    24% |******                    | 57216 KB    00:36 ETA
Verifying  base69.tgz    27% |*******                   | 62720 KB    00:34 ETA
Verifying  base69.tgz    29% |*******                   | 68352 KB    00:33 ETA
Verifying  base69.tgz    31% |********                  | 72192 KB    00:32 ETA
Verifying  base69.tgz    33% |********                  | 77056 KB    00:31 ETA
Verifying  base69.tgz    35% |*********                 | 81920 KB    00:30 ETA
Verifying  base69.tgz    38% |*********                 | 87296 KB    00:29 ETA
Verifying  base69.tgz    40% |**********                | 93440 KB    00:27 ETA
Verifying  base69.tgz    42% |**********                | 98048 KB    00:26 ETA
Verifying  base69.tgz    44% |***********               |   100 MB    00:25 ETA
Verifying  base69.tgz    46% |***********               |   105 MB    00:24 ETA
Verifying  base69.tgz    49% |************              |   110 MB    00:23 ETA
Verifying  base69.tgz    52% |*************             |   116 MB    00:22 ETA
Verifying  base69.tgz    54% |**************            |   122 MB    00:20 ETA
Verifying  base69.tgz    57% |**************            |   128 MB    00:19 ETA
Verifying  base69.tgz    59% |***************           |   133 MB    00:18 ETA
Verifying  base69.tgz    61% |***************           |   138 MB    00:17 ETA
Verifying  base69.tgz    63% |****************          |   142 MB    00:16 ETA
Verifying  base69.tgz    66% |*****************         |   148 MB    00:15 ETA
Verifying  base69.tgz    68% |*****************         |   154 MB    00:14 ETA
Verifying  base69.tgz    70% |******************        |   158 MB    00:13 ETA
Verifying  base69.tgz    72% |******************        |   162 MB    00:12 ETA
Verifying  base69.tgz    74% |*******************       |   165 MB    00:12 ETA
Verifying  base69.tgz    75% |*******************       |   169 MB    00:11 ETA
Verifying  base69.tgz    78% |********************      |   175 MB    00:09 ETA
Verifying  base69.tgz    80% |********************      |   180 MB    00:08 ETA
Verifying  base69.tgz    82% |*********************     |   185 MB    00:07 ETA
Verifying  base69.tgz    84% |*********************     |   190 MB    00:06 ETA
Verifying  base69.tgz    87% |**********************    |   194 MB    00:05 ETA
Verifying  base69.tgz    89% |***********************   |   199 MB    00:04 ETA
Verifying  base69.tgz    91% |***********************   |   205 MB    00:03 ETA
Verifying  base69.tgz    94% |************************  |   210 MB    00:02 ETA
Verifying  base69.tgz    96% |************************  |   216 MB    00:01 ETA
Verifying  base69.tgz    98% |************************* |   220 MB    00:00 ETA
Verifying  base69.tgz   100% |**************************|   223 MB    00:45    

Verifying  comp69.tgz     0% |                          |     0       --:-- ETA
Verifying  comp69.tgz     6% |*                         |  4480 KB    00:15 ETA
Verifying  comp69.tgz    13% |***                       |  9984 KB    00:13 ETA
Verifying  comp69.tgz    22% |*****                     | 16768 KB    00:10 ETA
Verifying  comp69.tgz    30% |*******                   | 22656 KB    00:09 ETA
Verifying  comp69.tgz    37% |*********                 | 27648 KB    00:08 ETA
Verifying  comp69.tgz    44% |***********               | 33152 KB    00:07 ETA
Verifying  comp69.tgz    53% |*************             | 39552 KB    00:06 ETA
Verifying  comp69.tgz    61% |***************           | 45184 KB    00:05 ETA
Verifying  comp69.tgz    68% |*****************         | 50944 KB    00:04 ETA
Verifying  comp69.tgz    75% |*******************       | 55936 KB    00:03 ETA
Verifying  comp69.tgz    83% |*********************     | 62080 KB    00:02 ETA
Verifying  comp69.tgz    92% |***********************   | 68480 KB    00:00 ETA
Verifying  comp69.tgz    99% |************************* | 73600 KB    00:00 ETA
Verifying  comp69.tgz   100% |**************************| 73952 KB    00:13    

Installing bsd.IP27       0% |                          |     0       --:-- ETA
Installing bsd.IP27      62% |****************          |  4864 KB    00:00 ETA
Installing bsd.IP27     100% |**************************|  7764 KB    00:01    

Installing base69.tgz     0% |                          |     0       --:-- ETA
Installing base69.tgz     0% |                          |   896 KB    04:20 ETA
Installing base69.tgz     0% |                          |  1664 KB    04:36 ETA
Installing base69.tgz     1% |                          |  2688 KB    04:13 ETA
Installing base69.tgz     1% |                          |  3584 KB    04:12 ETA
Installing base69.tgz     1% |                          |  4352 KB    04:18 ETA
Installing base69.tgz     2% |                          |  5248 KB    04:16 ETA
Installing base69.tgz     2% |                          |  6016 KB    04:20 ETA
Installing base69.tgz     2% |                          |  6784 KB    04:22 ETA
Installing base69.tgz     3% |                          |  7680 KB    04:20 ETA
Installing base69.tgz     3% |                          |  8320 KB    04:25 ETA
Installing base69.tgz     3% |                          |  8832 KB    04:35 ETA
Installing base69.tgz     4% |*                         |  9728 KB    04:31 ETA
Installing base69.tgz     4% |*                         | 10752 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 11520 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 12416 KB    04:22 ETA
Installing base69.tgz     5% |*                         | 13056 KB    04:25 ETA
Installing base69.tgz     5% |*                         | 13440 KB    04:33 ETA
Installing base69.tgz     6% |*                         | 14208 KB    04:32 ETA
Installing base69.tgz     6% |*                         | 14848 KB    04:34 ETA
Installing base69.tgz     6% |*                         | 15616 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 16384 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 17152 KB    04:32 ETA
Installing base69.tgz     7% |*                         | 17920 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 18560 KB    04:32 ETA
Installing base69.tgz     8% |**                        | 19328 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 20224 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 20992 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 21888 KB    04:25 ETA
Installing base69.tgz     9% |**                        | 22656 KB    04:24 ETA
Installing base69.tgz    10% |**                        | 23552 KB    04:22 ETA
Installing base69.tgz    10% |**                        | 24320 KB    04:21 ETA
Installing base69.tgz    10% |**                        | 25216 KB    04:19 ETA
Installing base69.tgz    11% |**                        | 26112 KB    04:16 ETA
Installing base69.tgz    11% |**                        | 26880 KB    04:16 ETA
Installing base69.tgz    12% |***                       | 27776 KB    04:14 ETA
Installing base69.tgz    12% |***                       | 28672 KB    04:12 ETA
Installing base69.tgz    12% |***                       | 29440 KB    04:11 ETA
Installing base69.tgz    13% |***                       | 30336 KB    04:09 ETA
Installing base69.tgz    13% |***                       | 31232 KB    04:07 ETA
Installing base69.tgz    13% |***                       | 32000 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 32768 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 33280 KB    04:07 ETA
Installing base69.tgz    14% |***                       | 34176 KB    04:05 ETA
Installing base69.tgz    15% |***                       | 34944 KB    04:04 ETA
Installing base69.tgz    15% |***                       | 35840 KB    04:03 ETA
Installing base69.tgz    15% |***                       | 36608 KB    04:02 ETA
Installing base69.tgz    16% |****                      | 37632 KB    03:59 ETA
Installing base69.tgz    16% |****                      | 38528 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 39296 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 40320 KB    03:54 ETA
Installing base69.tgz    17% |****                      | 41216 KB    03:52 ETA
Installing base69.tgz    18% |****                      | 42112 KB    03:51 ETA
Installing base69.tgz    18% |****                      | 42752 KB    03:51 ETA
Installing base69.tgz    19% |****                      | 43648 KB    03:49 ETA
Installing base69.tgz    19% |****                      | 44544 KB    03:48 ETA
Installing base69.tgz    19% |****                      | 45440 KB    03:46 ETA
Installing base69.tgz    20% |*****                     | 46208 KB    03:45 ETA
Installing base69.tgz    20% |*****                     | 47104 KB    03:44 ETA
Installing base69.tgz    20% |*****                     | 48000 KB    03:42 ETA
Installing base69.tgz    21% |*****                     | 48896 KB    03:41 ETA
Installing base69.tgz    21% |*****                     | 49792 KB    03:39 ETA
Installing base69.tgz    22% |*****                     | 50688 KB    03:38 ETA
Installing base69.tgz    22% |*****                     | 51584 KB    03:37 ETA
Installing base69.tgz    22% |*****                     | 52480 KB    03:35 ETA
Installing base69.tgz    23% |*****                     | 53248 KB    03:34 ETA
Installing base69.tgz    23% |*****                     | 54144 KB    03:33 ETA
Installing base69.tgz    23% |*****                     | 54912 KB    03:32 ETA
Installing base69.tgz    24% |******                    | 55808 KB    03:31 ETA
Installing base69.tgz    24% |******                    | 56704 KB    03:30 ETA
Installing base69.tgz    25% |******                    | 57472 KB    03:29 ETA
Installing base69.tgz    25% |******                    | 58240 KB    03:28 ETA
Installing base69.tgz    25% |******                    | 59008 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 59776 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 60544 KB    03:26 ETA
Installing base69.tgz    26% |******                    | 61440 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 61952 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 62592 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 63360 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 64000 KB    03:24 ETA
Installing base69.tgz    28% |*******                   | 64640 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 65280 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 66176 KB    03:22 ETA
Installing base69.tgz    29% |*******                   | 66944 KB    03:21 ETA
Installing base69.tgz    29% |*******                   | 67712 KB    03:20 ETA
Installing base69.tgz    29% |*******                   | 68480 KB    03:19 ETA
Installing base69.tgz    30% |*******                   | 69376 KB    03:18 ETA
Installing base69.tgz    30% |*******                   | 70272 KB    03:16 ETA
Installing base69.tgz    31% |********                  | 71168 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 71680 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 72448 KB    03:14 ETA
Installing base69.tgz    31% |********                  | 72960 KB    03:15 ETA
Installing base69.tgz    32% |********                  | 73600 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74112 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74752 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 75264 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 75904 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76416 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76928 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 77696 KB    03:13 ETA
Installing base69.tgz    34% |********                  | 78336 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 78976 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 79488 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 80000 KB    03:12 ETA
Installing base69.tgz    35% |*********                 | 80896 KB    03:10 ETA
Installing base69.tgz    35% |*********                 | 81920 KB    03:08 ETA
Installing base69.tgz    35% |*********                 | 82560 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83328 KB    03:07 ETA
Installing base69.tgz    36% |*********                 | 83456 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83712 KB    03:09 ETA
Installing base69.tgz    36% |*********                 | 84480 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 84992 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 85760 KB    03:07 ETA
Installing base69.tgz    37% |*********                 | 86528 KB    03:06 ETA
Installing base69.tgz    38% |*********                 | 87424 KB    03:05 ETA
Installing base69.tgz    38% |*********                 | 88064 KB    03:04 ETA
Installing base69.tgz    38% |*********                 | 88832 KB    03:03 ETA
Installing base69.tgz    39% |**********                | 89728 KB    03:02 ETA
Installing base69.tgz    39% |**********                | 90496 KB    03:01 ETA
Installing base69.tgz    39% |**********                | 91392 KB    02:59 ETA
Installing base69.tgz    40% |**********                | 92160 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 92800 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 93696 KB    02:56 ETA
Installing base69.tgz    41% |**********                | 94592 KB    02:55 ETA
Installing base69.tgz    41% |**********                | 95360 KB    02:54 ETA
Installing base69.tgz    42% |**********                | 96384 KB    02:52 ETA
Installing base69.tgz    42% |**********                | 97408 KB    02:50 ETA
Installing base69.tgz    42% |**********                | 98304 KB    02:49 ETA
Installing base69.tgz    43%"""

TRUNCATED_COMP69_TRANSCRIPT = """\
ARCS64 Firmware
Found SGI-IP27, setting up.
Machine is in M mode.
Initial setup done, switching console.

Copyright (c) 1982, 1986, 1989, 1991, 1993

\tThe Regents of the University of California.  All rights reserved.

Copyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org


OpenBSD 6.9 (RAMDISK-IP27) #0: Thu Apr 22 10:25:14 UTC 2021

    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/RAMDISK-IP27

real mem = 268435456 (256MB)

rsvd mem = 16171008 (16MB)

avail mem = 249331712 (237MB)

random: boothowto does not indicate good seed

mainbus0 at root: Origin 200

cpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0

cpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way

clock0 at mainbus0 nasid 0: int 5

xbow0 at mainbus0 nasid 0: Bridge revision 3

xbridge0 at xbow0 widget 8: Bridge revision 3

xbpci0 at xbridge0 bus 0: 33 MHz PCI bus

pci0 at xbpci0 bus 0

qlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62

qlw0: nvram corrupt

qlw0: firmware rev 1.0.0, attrs 0x0

scsibus0 at qlw0: 16 targets, initiator 0

sd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd0: 2048MB, 512 bytes/sector, 4194304 sectors

sd1 at scsibus0 targ 2 lun 0: <QEMU, QEMU HARDDISK, 2.5+>

sd1: 400MB, 512 bytes/sector, 819200 sectors

qlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61

qlw1: nvram corrupt

qlw1: firmware rev 1.0.0, attrs 0x0

scsibus1 at qlw1: 16 targets, initiator 0

ioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00

onewire0 at ioc0

ioc0: ethernet irq 2, xbow irq 60

ioc0: superio irq 3, xbow irq 59

com0 at ioc0 base 0x20178: ns16550a, 16 byte fifo

com0: console

com1 at ioc0 base 0x20170: ns16550a, 16 byte fifo

iockbc0 at ioc0

iec0 at ioc0: 128KB SSRAM, address ff:ff:ff:ff:ff:ff

nsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1

lpt at ioc0 not configured

dsrtc0 at ioc0: DS1742W

boot device: 'unknown' unrecognized.

root on rd0a swap on rd0b dump on rd0b

WARNING: CHECK AND RESET THE DATE!

erase ^?, werase ^W, kill ^U, intr ^C, status ^T

Welcome to the OpenBSD/sgi 6.9 installation program.

[driver] install-menu -> b'I\\n'
(I)nstall, (U)pgrade, (A)utoinstall or (S)hell? I
At any prompt except password prompts you can escape to a shell by
typing '!'. Default answers are shown in []'s and are selected by
pressing RETURN.  You can exit this program at any time by pressing
Control-C, but this can leave your system in an inconsistent state.


[driver] terminal-type -> b'vt220\\n'
Terminal type? [vt220] vt220

[driver] hostname -> b'origami\\n'
System hostname? (short form, e.g. 'foo') origami

Available network interfaces are: iec0 vlan0.

[driver] net-interface -> b'done\\n'
Which network interface do you wish to configure? (or 'done') [iec0] done

[driver] [UNHANDLED PROMPT] tail=b\" (or 'done') [iec0] done\\r\\nDNS domain name? (e.g. 'example.com') [my.domain] \" -- sending bare newline
DNS domain name? (e.g. 'example.com') [my.domain] 

[driver] [UNHANDLED PROMPT] tail=b\"\\r\\nDNS nameservers? (IP address list or 'none') [none] \" -- sending bare newline
DNS nameservers? (IP address list or 'none') [none] 


[driver] root-password -> b'origami123\\n'
Password for root account? (will not echo) 

[driver] root-password -> b'origami123\\n'
Password for root account? (again) 

[driver] sshd-default -> b'yes\\n'
Start sshd(8) by default? [yes] yes

[driver] setup-user -> b'n\\n'
Setup a user? (enter a lower-case loginname, or 'no') [no] n
Since no user was setup, root logins via sshd(8) might be useful.
WARNING: root is targeted by password guessing attacks, pubkeys are safer.

[driver] [UNHANDLED PROMPT] tail=b't is targeted by password guessing attacks, pubkeys are safer.\\r\\nAllow root ssh login? (yes, no, prohibit-password) [no] ' -- sending bare newline
Allow root ssh login? (yes, no, prohibit-password) [no] 

Available disks are: sd0 sd1.

[driver] root-disk -> b'sd0\\n'
Which disk is the root disk? ('?' for details) [sd0] sd0

Checking SGI Volume Header:

There is no Volume Header found on the disk. A Volume
header is required to be able to boot from the disk.

[driver] [UNHANDLED PROMPT] tail=b'n the disk. A Volume\\r\\nheader is required to be able to boot from the disk.\\r\\nDo you want to install a Volume Header? [y] ' -- sending bare newline
Do you want to install a Volume Header? [y] 
The auto-allocated layout for sd0 is:
#                size           offset  fstype [fsize bsize   cpg]
  a:          1978.2M             3135  4.2BSD   2048 16384     1 # /
  b:            68.3M          4054432    swap                    
  c:          2048.0M                0  unused                    
  p:             1.5M                0 unknown                    

[driver] disklabel-layout -> b'a\\n'
Use (A)uto layout, (E)dit auto layout, or create (C)ustom layout? [a] a
/dev/rsd0a: 1978.2MB in 4051296 sectors of 512 bytes
10 cylinder groups of 202.47MB, 12958 blocks, 25984 inodes each
Available disks are: sd1.

[driver] [UNHANDLED PROMPT] tail=b\"B, 12958 blocks, 25984 inodes each\\r\\nAvailable disks are: sd1.\\r\\nWhich disk do you wish to initialize? (or 'done') [done] \" -- sending bare newline
Which disk do you wish to initialize? (or 'done') [done] 
/dev/sd0a (18cc54198c86b2cb.a) on /mnt type ffs (rw, asynchronous, local)

Let's install the sets!

[driver] location-of-sets (occurrence 1) -> b'disk\\n'
Location of sets? (disk http nfs or 'done') [http] disk

[driver] already-mounted -> b'no\\n'
Is the disk partition already mounted? [yes] no
Available disks are: sd0 sd1.

[driver] which-disk-sets -> b'sd1\\n'
Which disk contains the install media? (or 'done') [sd1] sd1

[driver] set-path -> b'.\\n'
Pathname to the sets? (or 'done') [6.9/sgi] .

Select sets by entering a set name, a file name pattern or 'all'. De-select
sets by prepending a '-', e.g.: '-game*'. Selected sets are labelled '[X]'.
    [X] bsd.IP27      [X] base69.tgz    [X] comp69.tgz

[driver] set-name -> b'done\\n'
Set name(s)? (or 'abort' or 'done') [done] done

Verifying  SHA256.sig     0% |                          |     0       --:-- ETA
Verifying  SHA256.sig   100% |**************************|  2667       00:00    
Signature Verified

Verifying  bsd.IP27       0% |                          |     0       --:-- ETA
Verifying  bsd.IP27      36% |*********                 |  2816 KB    00:01 ETA
Verifying  bsd.IP27     100% |**************************|  7764 KB    00:01    

Verifying  base69.tgz     0% |                          |     0       --:-- ETA
Verifying  base69.tgz     1% |                          |  3968 KB    00:58 ETA
Verifying  base69.tgz     4% |*                         |  9216 KB    00:48 ETA
Verifying  base69.tgz     5% |*                         | 13312 KB    00:48 ETA
Verifying  base69.tgz     7% |*                         | 17280 KB    00:49 ETA
Verifying  base69.tgz    10% |**                        | 23040 KB    00:44 ETA
Verifying  base69.tgz    11% |**                        | 27392 KB    00:44 ETA
Verifying  base69.tgz    14% |***                       | 32384 KB    00:42 ETA
Verifying  base69.tgz    16% |****                      | 36736 KB    00:42 ETA
Verifying  base69.tgz    18% |****                      | 42752 KB    00:39 ETA
Verifying  base69.tgz    21% |*****                     | 48384 KB    00:37 ETA
Verifying  base69.tgz    22% |*****                     | 52480 KB    00:37 ETA
Verifying  base69.tgz    24% |******                    | 57216 KB    00:36 ETA
Verifying  base69.tgz    27% |*******                   | 62720 KB    00:34 ETA
Verifying  base69.tgz    29% |*******                   | 68352 KB    00:33 ETA
Verifying  base69.tgz    31% |********                  | 72192 KB    00:32 ETA
Verifying  base69.tgz    33% |********                  | 77056 KB    00:31 ETA
Verifying  base69.tgz    35% |*********                 | 81920 KB    00:30 ETA
Verifying  base69.tgz    38% |*********                 | 87296 KB    00:29 ETA
Verifying  base69.tgz    40% |**********                | 93440 KB    00:27 ETA
Verifying  base69.tgz    42% |**********                | 98048 KB    00:26 ETA
Verifying  base69.tgz    44% |***********               |   100 MB    00:25 ETA
Verifying  base69.tgz    46% |***********               |   105 MB    00:24 ETA
Verifying  base69.tgz    49% |************              |   110 MB    00:23 ETA
Verifying  base69.tgz    52% |*************             |   116 MB    00:22 ETA
Verifying  base69.tgz    54% |**************            |   122 MB    00:20 ETA
Verifying  base69.tgz    57% |**************            |   128 MB    00:19 ETA
Verifying  base69.tgz    59% |***************           |   133 MB    00:18 ETA
Verifying  base69.tgz    61% |***************           |   138 MB    00:17 ETA
Verifying  base69.tgz    63% |****************          |   142 MB    00:16 ETA
Verifying  base69.tgz    66% |*****************         |   148 MB    00:15 ETA
Verifying  base69.tgz    68% |*****************         |   154 MB    00:14 ETA
Verifying  base69.tgz    70% |******************        |   158 MB    00:13 ETA
Verifying  base69.tgz    72% |******************        |   162 MB    00:12 ETA
Verifying  base69.tgz    74% |*******************       |   165 MB    00:12 ETA
Verifying  base69.tgz    75% |*******************       |   169 MB    00:11 ETA
Verifying  base69.tgz    78% |********************      |   175 MB    00:09 ETA
Verifying  base69.tgz    80% |********************      |   180 MB    00:08 ETA
Verifying  base69.tgz    82% |*********************     |   185 MB    00:07 ETA
Verifying  base69.tgz    84% |*********************     |   190 MB    00:06 ETA
Verifying  base69.tgz    87% |**********************    |   194 MB    00:05 ETA
Verifying  base69.tgz    89% |***********************   |   199 MB    00:04 ETA
Verifying  base69.tgz    91% |***********************   |   205 MB    00:03 ETA
Verifying  base69.tgz    94% |************************  |   210 MB    00:02 ETA
Verifying  base69.tgz    96% |************************  |   216 MB    00:01 ETA
Verifying  base69.tgz    98% |************************* |   220 MB    00:00 ETA
Verifying  base69.tgz   100% |**************************|   223 MB    00:45    

Verifying  comp69.tgz     0% |                          |     0       --:-- ETA
Verifying  comp69.tgz     6% |*                         |  4480 KB    00:15 ETA
Verifying  comp69.tgz    13% |***                       |  9984 KB    00:13 ETA
Verifying  comp69.tgz    22% |*****                     | 16768 KB    00:10 ETA
Verifying  comp69.tgz    30% |*******                   | 22656 KB    00:09 ETA
Verifying  comp69.tgz    37% |*********                 | 27648 KB    00:08 ETA
Verifying  comp69.tgz    44% |***********               | 33152 KB    00:07 ETA
Verifying  comp69.tgz    53% |*************             | 39552 KB    00:06 ETA
Verifying  comp69.tgz    61% |***************           | 45184 KB    00:05 ETA
Verifying  comp69.tgz    68% |*****************         | 50944 KB    00:04 ETA
Verifying  comp69.tgz    75% |*******************       | 55936 KB    00:03 ETA
Verifying  comp69.tgz    83% |*********************     | 62080 KB    00:02 ETA
Verifying  comp69.tgz    92% |***********************   | 68480 KB    00:00 ETA
Verifying  comp69.tgz    99% |************************* | 73600 KB    00:00 ETA
Verifying  comp69.tgz   100% |**************************| 73952 KB    00:13    

Installing bsd.IP27       0% |                          |     0       --:-- ETA
Installing bsd.IP27      62% |****************          |  4864 KB    00:00 ETA
Installing bsd.IP27     100% |**************************|  7764 KB    00:01    

Installing base69.tgz     0% |                          |     0       --:-- ETA
Installing base69.tgz     0% |                          |   896 KB    04:20 ETA
Installing base69.tgz     0% |                          |  1664 KB    04:36 ETA
Installing base69.tgz     1% |                          |  2688 KB    04:13 ETA
Installing base69.tgz     1% |                          |  3584 KB    04:12 ETA
Installing base69.tgz     1% |                          |  4352 KB    04:18 ETA
Installing base69.tgz     2% |                          |  5248 KB    04:16 ETA
Installing base69.tgz     2% |                          |  6016 KB    04:20 ETA
Installing base69.tgz     2% |                          |  6784 KB    04:22 ETA
Installing base69.tgz     3% |                          |  7680 KB    04:20 ETA
Installing base69.tgz     3% |                          |  8320 KB    04:25 ETA
Installing base69.tgz     3% |                          |  8832 KB    04:35 ETA
Installing base69.tgz     4% |*                         |  9728 KB    04:31 ETA
Installing base69.tgz     4% |*                         | 10752 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 11520 KB    04:24 ETA
Installing base69.tgz     5% |*                         | 12416 KB    04:22 ETA
Installing base69.tgz     5% |*                         | 13056 KB    04:25 ETA
Installing base69.tgz     5% |*                         | 13440 KB    04:33 ETA
Installing base69.tgz     6% |*                         | 14208 KB    04:32 ETA
Installing base69.tgz     6% |*                         | 14848 KB    04:34 ETA
Installing base69.tgz     6% |*                         | 15616 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 16384 KB    04:33 ETA
Installing base69.tgz     7% |*                         | 17152 KB    04:32 ETA
Installing base69.tgz     7% |*                         | 17920 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 18560 KB    04:32 ETA
Installing base69.tgz     8% |**                        | 19328 KB    04:31 ETA
Installing base69.tgz     8% |**                        | 20224 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 20992 KB    04:28 ETA
Installing base69.tgz     9% |**                        | 21888 KB    04:25 ETA
Installing base69.tgz     9% |**                        | 22656 KB    04:24 ETA
Installing base69.tgz    10% |**                        | 23552 KB    04:22 ETA
Installing base69.tgz    10% |**                        | 24320 KB    04:21 ETA
Installing base69.tgz    10% |**                        | 25216 KB    04:19 ETA
Installing base69.tgz    11% |**                        | 26112 KB    04:16 ETA
Installing base69.tgz    11% |**                        | 26880 KB    04:16 ETA
Installing base69.tgz    12% |***                       | 27776 KB    04:14 ETA
Installing base69.tgz    12% |***                       | 28672 KB    04:12 ETA
Installing base69.tgz    12% |***                       | 29440 KB    04:11 ETA
Installing base69.tgz    13% |***                       | 30336 KB    04:09 ETA
Installing base69.tgz    13% |***                       | 31232 KB    04:07 ETA
Installing base69.tgz    13% |***                       | 32000 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 32768 KB    04:06 ETA
Installing base69.tgz    14% |***                       | 33280 KB    04:07 ETA
Installing base69.tgz    14% |***                       | 34176 KB    04:05 ETA
Installing base69.tgz    15% |***                       | 34944 KB    04:04 ETA
Installing base69.tgz    15% |***                       | 35840 KB    04:03 ETA
Installing base69.tgz    15% |***                       | 36608 KB    04:02 ETA
Installing base69.tgz    16% |****                      | 37632 KB    03:59 ETA
Installing base69.tgz    16% |****                      | 38528 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 39296 KB    03:57 ETA
Installing base69.tgz    17% |****                      | 40320 KB    03:54 ETA
Installing base69.tgz    17% |****                      | 41216 KB    03:52 ETA
Installing base69.tgz    18% |****                      | 42112 KB    03:51 ETA
Installing base69.tgz    18% |****                      | 42752 KB    03:51 ETA
Installing base69.tgz    19% |****                      | 43648 KB    03:49 ETA
Installing base69.tgz    19% |****                      | 44544 KB    03:48 ETA
Installing base69.tgz    19% |****                      | 45440 KB    03:46 ETA
Installing base69.tgz    20% |*****                     | 46208 KB    03:45 ETA
Installing base69.tgz    20% |*****                     | 47104 KB    03:44 ETA
Installing base69.tgz    20% |*****                     | 48000 KB    03:42 ETA
Installing base69.tgz    21% |*****                     | 48896 KB    03:41 ETA
Installing base69.tgz    21% |*****                     | 49792 KB    03:39 ETA
Installing base69.tgz    22% |*****                     | 50688 KB    03:38 ETA
Installing base69.tgz    22% |*****                     | 51584 KB    03:37 ETA
Installing base69.tgz    22% |*****                     | 52480 KB    03:35 ETA
Installing base69.tgz    23% |*****                     | 53248 KB    03:34 ETA
Installing base69.tgz    23% |*****                     | 54144 KB    03:33 ETA
Installing base69.tgz    23% |*****                     | 54912 KB    03:32 ETA
Installing base69.tgz    24% |******                    | 55808 KB    03:31 ETA
Installing base69.tgz    24% |******                    | 56704 KB    03:30 ETA
Installing base69.tgz    25% |******                    | 57472 KB    03:29 ETA
Installing base69.tgz    25% |******                    | 58240 KB    03:28 ETA
Installing base69.tgz    25% |******                    | 59008 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 59776 KB    03:27 ETA
Installing base69.tgz    26% |******                    | 60544 KB    03:26 ETA
Installing base69.tgz    26% |******                    | 61440 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 61952 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 62592 KB    03:25 ETA
Installing base69.tgz    27% |*******                   | 63360 KB    03:24 ETA
Installing base69.tgz    27% |*******                   | 64000 KB    03:24 ETA
Installing base69.tgz    28% |*******                   | 64640 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 65280 KB    03:23 ETA
Installing base69.tgz    28% |*******                   | 66176 KB    03:22 ETA
Installing base69.tgz    29% |*******                   | 66944 KB    03:21 ETA
Installing base69.tgz    29% |*******                   | 67712 KB    03:20 ETA
Installing base69.tgz    29% |*******                   | 68480 KB    03:19 ETA
Installing base69.tgz    30% |*******                   | 69376 KB    03:18 ETA
Installing base69.tgz    30% |*******                   | 70272 KB    03:16 ETA
Installing base69.tgz    31% |********                  | 71168 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 71680 KB    03:15 ETA
Installing base69.tgz    31% |********                  | 72448 KB    03:14 ETA
Installing base69.tgz    31% |********                  | 72960 KB    03:15 ETA
Installing base69.tgz    32% |********                  | 73600 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74112 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 74752 KB    03:14 ETA
Installing base69.tgz    32% |********                  | 75264 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 75904 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76416 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 76928 KB    03:14 ETA
Installing base69.tgz    33% |********                  | 77696 KB    03:13 ETA
Installing base69.tgz    34% |********                  | 78336 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 78976 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 79488 KB    03:12 ETA
Installing base69.tgz    34% |********                  | 80000 KB    03:12 ETA
Installing base69.tgz    35% |*********                 | 80896 KB    03:10 ETA
Installing base69.tgz    35% |*********                 | 81920 KB    03:08 ETA
Installing base69.tgz    35% |*********                 | 82560 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83328 KB    03:07 ETA
Installing base69.tgz    36% |*********                 | 83456 KB    03:08 ETA
Installing base69.tgz    36% |*********                 | 83712 KB    03:09 ETA
Installing base69.tgz    36% |*********                 | 84480 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 84992 KB    03:08 ETA
Installing base69.tgz    37% |*********                 | 85760 KB    03:07 ETA
Installing base69.tgz    37% |*********                 | 86528 KB    03:06 ETA
Installing base69.tgz    38% |*********                 | 87424 KB    03:05 ETA
Installing base69.tgz    38% |*********                 | 88064 KB    03:04 ETA
Installing base69.tgz    38% |*********                 | 88832 KB    03:03 ETA
Installing base69.tgz    39% |**********                | 89728 KB    03:02 ETA
Installing base69.tgz    39% |**********                | 90496 KB    03:01 ETA
Installing base69.tgz    39% |**********                | 91392 KB    02:59 ETA
Installing base69.tgz    40% |**********                | 92160 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 92800 KB    02:58 ETA
Installing base69.tgz    40% |**********                | 93696 KB    02:56 ETA
Installing base69.tgz    41% |**********                | 94592 KB    02:55 ETA
Installing base69.tgz    41% |**********                | 95360 KB    02:54 ETA
Installing base69.tgz    42% |**********                | 96384 KB    02:52 ETA
Installing base69.tgz    42% |**********                | 97408 KB    02:50 ETA
Installing base69.tgz    42% |**********                | 98304 KB    02:49 ETA
Installing base69.tgz    43% |***********               | 99200 KB    02:47 ETA
Installing base69.tgz    43% |***********               | 99968 KB    02:46 ETA
Installing base69.tgz    44% |***********               |    98 MB    02:45 ETA
Installing base69.tgz    44% |***********               |    99 MB    02:43 ETA
Installing base69.tgz    44% |***********               |   100 MB    02:41 ETA
Installing base69.tgz    45% |***********               |   101 MB    02:40 ETA
Installing base69.tgz    45% |***********               |   102 MB    02:38 ETA
Installing base69.tgz    46% |***********               |   103 MB    02:37 ETA
Installing base69.tgz    46% |***********               |   104 MB    02:36 ETA
Installing base69.tgz    46% |***********               |   105 MB    02:34 ETA
Installing base69.tgz    47% |************              |   106 MB    02:33 ETA
Installing base69.tgz    47% |************              |   107 MB    02:31 ETA
Installing base69.tgz    48% |************              |   108 MB    02:29 ETA
Installing base69.tgz    48% |************              |   109 MB    02:28 ETA
Installing base69.tgz    49% |************              |   109 MB    02:27 ETA
Installing base69.tgz    49% |************              |   110 MB    02:26 ETA
Installing base69.tgz    49% |************              |   111 MB    02:24 ETA
Installing base69.tgz    50% |*************             |   112 MB    02:23 ETA
Installing base69.tgz    50% |*************             |   113 MB    02:21 ETA
Installing base69.tgz    51% |*************             |   114 MB    02:20 ETA
Installing base69.tgz    51% |*************             |   115 MB    02:18 ETA
Installing base69.tgz    52% |*************             |   116 MB    02:17 ETA
Installing base69.tgz    52% |*************             |   117 MB    02:16 ETA
Installing base69.tgz    52% |*************             |   118 MB    02:15 ETA
Installing base69.tgz    53% |*************             |   119 MB    02:14 ETA
Installing base69.tgz    53% |*************             |   120 MB    02:12 ETA
Installing base69.tgz    54% |**************            |   121 MB    02:11 ETA
Installing base69.tgz    54% |**************            |   122 MB    02:09 ETA
Installing base69.tgz    54% |**************            |   123 MB    02:08 ETA
Installing base69.tgz    55% |**************            |   124 MB    02:06 ETA
Installing base69.tgz    55% |**************            |   124 MB    02:05 ETA
Installing base69.tgz    56% |**************            |   125 MB    02:04 ETA
Installing base69.tgz    56% |**************            |   126 MB    02:03 ETA
Installing base69.tgz    56% |**************            |   127 MB    02:02 ETA
Installing base69.tgz    57% |**************            |   128 MB    02:00 ETA
Installing base69.tgz    57% |**************            |   129 MB    01:59 ETA
Installing base69.tgz    58% |***************           |   130 MB    01:58 ETA
Installing base69.tgz    58% |***************           |   131 MB    01:56 ETA
Installing base69.tgz    58% |***************           |   132 MB    01:55 ETA
Installing base69.tgz    59% |***************           |   133 MB    01:54 ETA
Installing base69.tgz    59% |***************           |   133 MB    01:53 ETA
Installing base69.tgz    60% |***************           |   134 MB    01:51 ETA
Installing base69.tgz    60% |***************           |   135 MB    01:50 ETA
Installing base69.tgz    60% |***************           |   136 MB    01:49 ETA
Installing base69.tgz    61% |***************           |   137 MB    01:49 ETA
Installing base69.tgz    61% |***************           |   138 MB    01:47 ETA
Installing base69.tgz    62% |****************          |   139 MB    01:46 ETA
Installing base69.tgz    62% |****************          |   140 MB    01:44 ETA
Installing base69.tgz    62% |****************          |   140 MB    01:43 ETA
Installing base69.tgz    63% |****************          |   141 MB    01:42 ETA
Installing base69.tgz    63% |****************          |   142 MB    01:41 ETA
Installing base69.tgz    64% |****************          |   143 MB    01:40 ETA
Installing base69.tgz    64% |****************          |   144 MB    01:38 ETA
Installing base69.tgz    65% |****************          |   145 MB    01:37 ETA
Installing base69.tgz    65% |****************          |   146 MB    01:36 ETA
Installing base69.tgz    65% |****************          |   147 MB    01:34 ETA
Installing base69.tgz    66% |*****************         |   148 MB    01:33 ETA
Installing base69.tgz    66% |*****************         |   149 MB    01:32 ETA
Installing base69.tgz    67% |*****************         |   150 MB    01:31 ETA
Installing base69.tgz    67% |*****************         |   151 MB    01:29 ETA
Installing base69.tgz    67% |*****************         |   152 MB    01:28 ETA
Installing base69.tgz    68% |*****************         |   153 MB    01:27 ETA
Installing base69.tgz    68% |*****************         |   154 MB    01:26 ETA
Installing base69.tgz    69% |*****************         |   154 MB    01:25 ETA
Installing base69.tgz    69% |*****************         |   155 MB    01:23 ETA
Installing base69.tgz    69% |*****************         |   156 MB    01:22 ETA
Installing base69.tgz    70% |******************        |   157 MB    01:21 ETA
Installing base69.tgz    70% |******************        |   158 MB    01:20 ETA
Installing base69.tgz    71% |******************        |   159 MB    01:19 ETA
Installing base69.tgz    71% |******************        |   160 MB    01:17 ETA
Installing base69.tgz    72% |******************        |   161 MB    01:16 ETA
Installing base69.tgz    72% |******************        |   162 MB    01:15 ETA
Installing base69.tgz    72% |******************        |   163 MB    01:14 ETA
Installing base69.tgz    73% |******************        |   164 MB    01:13 ETA
Installing base69.tgz    73% |******************        |   165 MB    01:11 ETA
Installing base69.tgz    74% |*******************       |   166 MB    01:10 ETA
Installing base69.tgz    74% |*******************       |   167 MB    01:09 ETA
Installing base69.tgz    75% |*******************       |   168 MB    01:08 ETA
Installing base69.tgz    75% |*******************       |   169 MB    01:06 ETA
Installing base69.tgz    75% |*******************       |   170 MB    01:05 ETA
Installing base69.tgz    76% |*******************       |   170 MB    01:04 ETA
Installing base69.tgz    76% |*******************       |   171 MB    01:03 ETA
Installing base69.tgz    77% |********************      |   172 MB    01:02 ETA
Installing base69.tgz    77% |********************      |   173 MB    01:01 ETA
Installing base69.tgz    77% |********************      |   174 MB    01:00 ETA
Installing base69.tgz    78% |********************      |   175 MB    00:58 ETA
Installing base69.tgz    78% |********************      |   176 MB    00:57 ETA
Installing base69.tgz    79% |********************      |   177 MB    00:56 ETA
Installing base69.tgz    79% |********************      |   178 MB    00:55 ETA
Installing base69.tgz    80% |********************      |   179 MB    00:53 ETA
Installing base69.tgz    80% |********************      |   180 MB    00:53 ETA
Installing base69.tgz    80% |********************      |   180 MB    00:52 ETA
Installing base69.tgz    81% |*********************     |   181 MB    00:51 ETA
Installing base69.tgz    81% |*********************     |   182 MB    00:49 ETA
Installing base69.tgz    81% |*********************     |   183 MB    00:48 ETA
Installing base69.tgz    82% |*********************     |   184 MB    00:47 ETA
Installing base69.tgz    82% |*********************     |   185 MB    00:46 ETA
Installing base69.tgz    83% |*********************     |   186 MB    00:44 ETA
Installing base69.tgz    83% |*********************     |   187 MB    00:43 ETA
Installing base69.tgz    84% |*********************     |   188 MB    00:42 ETA
Installing base69.tgz    84% |*********************     |   189 MB    00:41 ETA
Installing base69.tgz    85% |**********************    |   190 MB    00:40 ETA
Installing base69.tgz    85% |**********************    |   191 MB    00:38 ETA
Installing base69.tgz    86% |**********************    |   192 MB    00:37 ETA
Installing base69.tgz    86% |**********************    |   193 MB    00:36 ETA
Installing base69.tgz    87% |**********************    |   194 MB    00:34 ETA
Installing base69.tgz    87% |**********************    |   195 MB    00:33 ETA
Installing base69.tgz    87% |**********************    |   196 MB    00:32 ETA
Installing base69.tgz    88% |**********************    |   197 MB    00:31 ETA
Installing base69.tgz    88% |**********************    |   198 MB    00:30 ETA
Installing base69.tgz    89% |***********************   |   199 MB    00:29 ETA
Installing base69.tgz    89% |***********************   |   200 MB    00:27 ETA
Installing base69.tgz    90% |***********************   |   201 MB    00:26 ETA
Installing base69.tgz    90% |***********************   |   202 MB    00:25 ETA
Installing base69.tgz    90% |***********************   |   203 MB    00:24 ETA
Installing base69.tgz    91% |***********************   |   204 MB    00:22 ETA
Installing base69.tgz    91% |***********************   |   205 MB    00:21 ETA
Installing base69.tgz    92% |***********************   |   206 MB    00:20 ETA
Installing base69.tgz    92% |***********************   |   207 MB    00:19 ETA
Installing base69.tgz    92% |***********************   |   208 MB    00:18 ETA
Installing base69.tgz    93% |************************  |   209 MB    00:17 ETA
Installing base69.tgz    93% |************************  |   210 MB    00:16 ETA
Installing base69.tgz    94% |************************  |   211 MB    00:15 ETA
Installing base69.tgz    94% |************************  |   212 MB    00:13 ETA
Installing base69.tgz    95% |************************  |   213 MB    00:12 ETA
Installing base69.tgz    95% |************************  |   213 MB    00:12 ETA
Installing base69.tgz    95% |************************  |   214 MB    00:11 ETA
Installing base69.tgz    95% |************************  |   215 MB    00:10 ETA
Installing base69.tgz    96% |************************  |   215 MB    00:09 ETA
Installing base69.tgz    96% |************************  |   216 MB    00:08 ETA
Installing base69.tgz    97% |************************* |   217 MB    00:07 ETA
Installing base69.tgz    97% |************************* |   218 MB    00:06 ETA
Installing base69.tgz    97% |************************* |   219 MB    00:05 ETA
Installing base69.tgz    98% |************************* |   219 MB    00:04 ETA
Installing base69.tgz    98% |************************* |   220 MB    00:03 ETA
Installing base69.tgz    98% |************************* |   221 MB    00:02 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:02 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   222 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:01 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:00 ETA
Installing base69.tgz    99% |************************* |   223 MB    00:00 ETA
Installing base69.tgz   100% |**************************|   223 MB    04:33    

Extracting etc.tgz        0% |                          |     0       --:-- ETA
Extracting etc.tgz      100% |**************************|   254 KB    00:00    

Installing comp69.tgz     0% |                          |     0       --:-- ETA
Installing comp69.tgz     1% |                          |   768 KB    01:35 ETA
Installing comp69.tgz     1% |                          |  1408 KB    01:43 ETA
Installing comp69.tgz     2% |                          |  2048 KB    01:45 ETA
Installing comp69.tgz     3% |                          |  2816 KB    01:41 ETA
Installing comp69.tgz     5% |*                         |  3712 KB    01:34 ETA
Installing comp69.tgz     6% |*                         |  4480 KB    01:33 ETA
Installing comp69.tgz     6% |*                         |  5120 KB    01:34 ETA
Installing comp69.tgz     7% |*                         |  5888 KB    01:32 ETA
Installing comp69.tgz     9% |**                        |  6784 KB    01:29 ETA
Installing comp69.tgz    10% |**                        |  7680 KB    01:26 ETA
Installing comp69.tgz    11% |**                        |  8576 KB    01:23 ETA
Installing comp69.tgz    12% |***                       |  9344 KB    01:23 ETA
Installing comp69.tgz    13% |***                       | 10240 KB    01:20 ETA
Installing comp69.tgz    15% |***                       | 11136 KB    01:18 ETA
Installing comp69.tgz    16% |****                      | 11904 KB    01:18 ETA
Installing comp69.tgz    17% |****                      | 12800 KB    01:16 ETA
Installing comp69.tgz    18% |****                      | 13696 KB    01:14 ETA
Installing comp69.tgz    19% |****                      | 14720 KB    01:12 ETA
Installing comp69.tgz    21% |*****                     | 15744 KB    01:10 ETA
Installing comp69.tgz    22% |*****                     | 16512 KB    01:09 ETA
Installing comp69.tgz    23% |*****                     | 17664 KB    01:06 ETA
Installing comp69.tgz    24% |******                    | 18432 KB    01:06 ETA
Installing comp69.tgz    26% |******                    | 19456 KB    01:04 ETA
Installing comp69.tgz    27% |*******                   | 20352 KB    01:03 ETA
Installing comp69.tgz    28% |*******                   | 21120 KB    01:02 ETA
Installing comp69.tgz    29% |*******                   | 22016 KB    01:01 ETA
Installing comp69.tgz    30% |*******                   | 22912 KB    01:00 ETA
Installing comp69.tgz    32% |********                  | 23680 KB    00:59 ETA
Installing comp69.tgz    33% |********                  | 24448 KB    00:58 ETA
Installing comp69.tgz    34% |********                  | 25344 KB    00:57 ETA
Installing comp69.tgz    35% |*********                 | 25984 KB    00:57 ETA
Installing comp69.tgz    36% |*********                 | 26752 KB    00:56 ETA
Installing comp69.tgz    37% |*********                 | 27776 KB    00:54 ETA
Installing comp69.tgz    38% |*********                 | 28672 KB    00:53 ETA
Installing comp69.tgz    40% |**********                | 29696 KB    00:52 ETA
Installing comp69.tgz    41% |**********                | 30592 KB    00:51 ETA
Installing comp69.tgz    42% |**********                | 31360 KB    00:50 ETA
Installing comp69.tgz    43% |***********               | 32000 KB    00:49 ETA
Installing comp69.tgz    44% |***********               | 32768 KB    00:49 ETA
Installing comp69.tgz    45% |***********               | 33536 KB    00:48 ETA
Installing comp69.tgz    46% |***********               | 34176 KB    00:47 ETA
Installing comp69.tgz    47% |************              | 35072 KB    00:46 ETA
Installing comp69.tgz    48% |************              | 35712 KB    00:46 ETA
Installing comp69.tgz    49% |************              | 36608 KB    00:44 ETA
Installing comp69.tgz    50% |*************             | 37376 KB    00:44 ETA
Installing comp69.tgz    51% |*************             | 38272 KB    00:42 ETA
Installing comp69.tgz    52% |*************             | 39040 KB    00:42 ETA
Installing comp69.tgz    54% |**************            | 40064 KB    00:40 ETA
Installing comp69.tgz    55% |**************            | 41088 KB    00:39 ETA
Installing comp69.tgz    56% |**************            | 41728 KB    00:38 ETA
Installing comp69.tgz    58% |***************           | 43008 KB    00:36 ETA
Installing comp69.tgz    59% |***************           | 43776 KB    00:35 ETA
Installing comp69.tgz    60% |***************           | 44928 KB    00:34 ETA
Installing comp69.tgz    61%"""


class InstallVerdictTest(unittest.TestCase):

    def test_real_reference_transcript_passes(self):
        report = install_verdict(REAL_INSTALL_TRANSCRIPT)
        self.assertEqual(report["problems"], [])
        self.assertEqual(report["verdict"], "PASS")

    def test_truncated_base69_transcript_fails_the_documented_stall(self):
        # The first-named case of the documented failure mode: a run that
        # stalled partway through base69.tgz extraction, never reaching
        # its own 100% mark, comp69.tgz's own install, or CONGRATULATIONS.
        self.assertIsNone(BASE69_COMPLETE_RE.search(TRUNCATED_BASE69_TRANSCRIPT))
        self.assertNotIn("Installing comp69.tgz", TRUNCATED_BASE69_TRANSCRIPT)
        self.assertNotIn("CONGRATULATIONS", TRUNCATED_BASE69_TRANSCRIPT)
        report = install_verdict(TRUNCATED_BASE69_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ["never reached base69.tgz installation completion"])

    def test_truncated_comp69_transcript_fails_the_documented_stall(self):
        # The second-named case of the same documented failure mode: a run
        # that got past base69.tgz AND etc.tgz -- both reach their own
        # 100% mark here -- but stalled partway through comp69.tgz,
        # proving the ordered check catches a stall at the later set
        # independently of the earlier one succeeding.
        self.assertIsNotNone(
            BASE69_COMPLETE_RE.search(TRUNCATED_COMP69_TRANSCRIPT))
        self.assertIsNotNone(
            ETC_TGZ_COMPLETE_RE.search(TRUNCATED_COMP69_TRANSCRIPT))
        self.assertIsNone(
            COMP69_COMPLETE_RE.search(TRUNCATED_COMP69_TRANSCRIPT))
        self.assertNotIn("CONGRATULATIONS", TRUNCATED_COMP69_TRANSCRIPT)
        report = install_verdict(TRUNCATED_COMP69_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ["never reached comp69.tgz installation completion"])

    def test_never_reaches_the_welcome_banner(self):
        report = install_verdict("ARCS64 Firmware\r\nFound SGI-IP27.\r\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ["never reached the installer's welcome banner"])

    def test_second_location_of_sets_prompt_needs_its_own_occurrence(self):
        # Ordering matters: the second "Location of sets?" stage must not
        # be satisfied by re-finding the SAME occurrence that already
        # satisfied the first-occurrence stage -- a transcript with only
        # one "Location of sets?" in it, however far it otherwise gets,
        # must fail at the second-occurrence stage specifically, not be
        # read as though that one occurrence covered both.
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? (disk http nfs or 'done') [http] disk\r\n"
            "Installing base69.tgz    100% |**|\r\n"
            "Extracting etc.tgz      100% |**|\r\n"
            "Installing comp69.tgz   100% |**|\r\n"
            "CONGRATULATIONS! Your OpenBSD install has been successfully "
            "completed!\r\n")
        report = install_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ["never reached the second Location of sets prompt"])

    def test_congratulations_before_second_location_of_sets_does_not_count(self):
        # A "CONGRATULATIONS!" that appears before the second "Location of
        # sets?" prompt must not satisfy the completion check -- the
        # search for it only starts after that second prompt is matched,
        # so an earlier one (e.g. spliced in from a different run) is not
        # read as an earlier completion.
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? (disk http nfs or 'done') [http] disk\r\n"
            "CONGRATULATIONS! Your OpenBSD install has been successfully "
            "completed!\r\n"
            "Installing base69.tgz    100% |**|\r\n"
            "Extracting etc.tgz      100% |**|\r\n"
            "Installing comp69.tgz   100% |**|\r\n"
            "Location of sets? (disk http nfs or 'done') [done] done\r\n")
        report = install_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "never reached the CONGRATULATIONS completion banner",
            report["problems"])

    def test_ddb_wedge_before_congratulations_fails_and_is_not_read_as_a_pass(self):
        # The single most important correctness property this module
        # exists to prove: a run that falls into the kernel debugger
        # before "CONGRATULATIONS!" ever appears must FAIL, not be read as
        # a completed install.
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? (disk http nfs or 'done') [http] disk\r\n"
            "Installing base69.tgz    100% |**|\r\n"
            "Extracting etc.tgz      100% |**|\r\n"
            "panic: kernel diagnostic assertion failed\r\n"
            "\r\nddb> ")
        self.assertNotIn("CONGRATULATIONS", transcript)
        report = install_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])
        self.assertIn("the guest panicked", report["problems"])
        self.assertIn(
            "never reached comp69.tgz installation completion",
            report["problems"])

    def test_bus_error_trip_wire_fires_independently_of_ddb(self):
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? (disk http nfs or 'done') [http] disk\r\n"
            "Trap cause = 7 Frame 0xffffffff82b2f9c8\r\nddb> ")
        report = install_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest took a bus error", report["problems"])
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])

    def test_wedge_after_congratulations_is_still_a_pass(self):
        # The install itself completed for real (the whole ordered
        # progression is present); a ddb wedge or panic that shows up only
        # AFTER "CONGRATULATIONS!" -- e.g. during the post-install reboot
        # attempt -- must not retroactively fail a run that already proved
        # the installer's own completion. Trip wires are only checked up
        # to congrats_m.start(name), never past it.
        transcript = REAL_INSTALL_TRANSCRIPT + "\r\nddb> "
        report = install_verdict(transcript)
        self.assertEqual(report["verdict"], "PASS")

    def test_empty_transcript_fails_missing_everything(self):
        report = install_verdict("")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ["never reached the installer's welcome banner"])


if __name__ == "__main__":
    unittest.main()
