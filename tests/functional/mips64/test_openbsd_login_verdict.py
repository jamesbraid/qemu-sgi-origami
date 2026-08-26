#!/usr/bin/env python3
#
# Equivalence test: test_openbsd_login's verdict logic ports
# tools/net-transfer-diag's own login_origin() console drive, without
# booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_openbsd_login_verdict.py` (or
`uv run --with pytest pytest tests/functional/mips64/test_openbsd_login_verdict.py
-v`). It exercises login_verdict(), the pure function test_openbsd_login.py
calls against a captured console transcript, over fixture transcripts that
stand in for a captured guest console log.

login_verdict() ports login_origin() (tools/net-transfer-diag, project
checkout, read from fj/main): a kernel started with -kernel has no boot
path naming its root, so it asks -- login_origin() answers "sd0a" at
"root device:", takes the default at "swap device...:", sends "root" at
"login:", sends the root password at "Password:", and expects a bare "# "
for the root shell (its own con.expect(rb"# ", ..., what="a root
shell") -- no hostname, no leading line anchor). This module checks the
three markers login_origin() itself proves IN ORDER -- the root device
question answered, then the login prompt, then that shell prompt -- and
adds one property login_origin() does not itself need, because it is a
harness that raises and stops rather than a verdict that must be
recorded either way: a WEDGE must never read as a login. login_origin()
already carries a version of this (its own Console.TRIPWIRES list, which
this port keeps: ddb>/panic:, ported verbatim including the reasoning for
requiring a leading [\r\n] on ddb> -- this console emits a line as
"\n\rtext", not "\r\ntext", and BOTH orders appear in this module's own
real fixtures, so an anchor assuming one order would silently never
fire). "Trap cause = 7" is added as a third, independent trip wire beyond
what net-transfer-diag's own TRIPWIRES check: it is this fork's real
OpenBSD/IP27 kernel's own trap-dump text for a data bus error (confirmed
against docs/boot-logs/openbsd.boot.2026-08-13T22:33-07:00 in the project
checkout, a real captured run of the filed >128MiB memory-map defect, and
against docs/plans/pci-confspace-reset-values-handoff.md there, which
independently names cause 7 as "data bus error" for this port). That
capture happens to fall into ddb> right afterward, so the ddb> trip wire
alone would already have caught it there -- this is a second, independent
trip on the same real evidence, for the case where a bus error does not
make it as far as a debugger prompt.

Fixture provenance (every fixture is real captured guest-console text,
copied or spliced verbatim -- nothing below is written from scratch; see
each fixture's own comment for the exact source and, for the spliced one,
exactly which real bytes came from where):

  REAL_LOGIN_TRANSCRIPT
      The full guest-console portion of this port's own reference
      capture, .superpowers/sdd/2026-08-25-track4-openbsd-tests/
      openbsd-128mb-login-reference.log -- a real boot of the installed
      OpenBSD 6.9 disk on -M origin200 at 128 MiB, on this fork's current
      trunk, reaching an authenticated root shell (root device sd0a,
      default swap, root/origami123 login, "origami# " prompt, a clean
      "ifconfig -a" exit 0). Copied byte-for-byte from that file's own
      guest-transcript section (between its "# ---" separator lines),
      including its carriage returns and the terminal escape bytes the
      login banner's cursor-positioning sequence carries -- nothing
      normalized or reformatted.

  TRUNCATED_TRANSCRIPT
      REAL_LOGIN_TRANSCRIPT's own prefix, cut right after its "Last
      login: Wed Aug 12 05:40:43 on console" line -- the point a run that
      hung or was killed by timeout right after a correct password, but
      before the shell ever printed its prompt, would stop at. Every byte
      kept is unedited real capture text; nothing after the cut is
      included.

  SYNTHETIC_DDB_WEDGE_TRANSCRIPT
      Spliced from two real captures, not invented: REAL_LOGIN_TRANSCRIPT's
      own prefix through its "starting network" line (the same real
      early-boot text as the PASS fixture, unedited), followed directly by
      the real "Trap cause = 7" .. "ddb> " trap dump from
      docs/boot-logs/openbsd.boot.2026-08-13T22:33-07:00 (project
      checkout) -- a real captured run of the filed >128MiB memory-map
      defect wedging during /etc/rc, at 256 MiB. That source file also
      carries inline "[login-driver] ..." debug lines from the tool that
      captured it; those are that tool's own console-driver notes, never
      guest console output, and are excluded here. This transcript never
      reaches "login:" at all -- the wedge happens first, which is
      exactly the case this module exists to prove is not read as a
      login.
"""

import re
import unittest


# login_origin()'s own three ordered markers (see this module's docstring).
# The swap-device default answer is not itself asserted: reaching the
# login prompt already proves it was answered, since the kernel does not
# print "login:" without getting past it.
ROOT_DEVICE_RE = re.compile(r"root device\s*:")
LOGIN_PROMPT_RE = re.compile(r"login:")
ROOT_SHELL_RE = re.compile(r"# ")

# A run that shows any of these before it reaches the root shell is a
# wedge, not a login (see this module's docstring for where each pattern
# comes from and why).
TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)


def login_verdict(transcript):
    """PASS iff transcript shows, in order: the root device question
    answered, the login prompt, and a root shell prompt -- and none of
    TRIPWIRES fires before that shell prompt is reached (or anywhere in
    the transcript, if the shell prompt is never reached).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...]}.
    """
    device_m = ROOT_DEVICE_RE.search(transcript)
    login_m = (LOGIN_PROMPT_RE.search(transcript, device_m.end())
               if device_m else None)
    shell_m = (ROOT_SHELL_RE.search(transcript, login_m.end())
               if login_m else None)

    # Trip wires are checked over the whole span up to where the shell was
    # reached, or the whole transcript if it never was -- a wedge that
    # shows up anywhere before an actual login must fail, not just one
    # that happens to sit between the last stage matched and the next.
    trip_end = shell_m.start() if shell_m else len(transcript)
    problems = [why for why, rx in TRIPWIRES
               if rx.search(transcript, 0, trip_end)]

    if device_m is None:
        problems.append("never reached the root device question")
    elif login_m is None:
        problems.append("never reached the login prompt")
    elif shell_m is None:
        problems.append("never reached a root shell prompt")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# ---------------------------------------------------------------------------
# Fixtures

REAL_LOGIN_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
\rroot on sd0a swap on sd0b dump on sd0b
\rWARNING: clock gained 14 days
\rWARNING: CHECK AND RESET THE DATE!
\rAutomatic boot in progress: starting file system checks.\r
/dev/sd0a (18cc54198c86b2cb.a): file system is clean; not checking\r
pf enabled\r
starting network\r
reordering libraries: done.\r
starting early daemons: syslogd pflogd ntpd.\r
starting RPC daemons:.\r
kvm_mkdb: can't open /dev/ksyms\r
savecore: /bsd: kvm_read: version misread\r
checking quotas: done.\r
clearing /tmp\r
kern.securelevel: 0 -> 1\r
creating runtime link editor directory cache.\r
preserving editor files.\r
starting network daemons: sshd smtpd sndiod.\r
starting local daemons: cron.\r
Wed Aug 26 07:09:37 UTC 2026\r
\r
OpenBSD/sgi (origami.my.domain) (console)\r
\r
login: root\r
Password:\r
Last login: Wed Aug 12 05:40:43 on console\r
OpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021\r
\r
Welcome to OpenBSD: The proactively secure Unix-like operating system.\r
\r
Please use the sendbug(1) utility to report bugs in the system.\r
Before reporting a bug, please try to reproduce it with the latest\r
version of the code.  With bug reports, please try to ensure that\r
enough information to reproduce the problem is enclosed, and if a\r
known fix for it exists, include that as well.\r
\r
You have new mail.\r
\r\x1b[3g        \x1bH        \x1bH        \x1bH        \x1bH        \x1bH        \x1bH        \x1bH        \x1bH        \x1bH\r\rorigami# ifconfig -a; echo \"MARK\"\"-ifall-$?\"\r\r
lo0: flags=8049<UP,LOOPBACK,RUNNING,MULTICAST> mtu 32768\r
\x09index 3 priority 0 llprio 3\r
\x09groups: lo\r
\x09inet6 ::1 prefixlen 128\r
\x09inet6 fe80::1%lo0 prefixlen 64 scopeid 0x3\r
\x09inet 127.0.0.1 netmask 0xff000000\r
iec0: flags=8802<BROADCAST,SIMPLEX,MULTICAST> mtu 1500\r
\x09lladdr 08:00:69:12:34:56\r
\x09index 1 priority 0 llprio 3\r
\x09media: Ethernet autoselect\r
enc0: flags=0<>\r
\x09index 2 priority 0 llprio 3\r
\x09groups: enc\r
\x09status: active\r
pflog0: flags=141<UP,RUNNING,PROMISC> mtu 33136\r
\x09index 4 priority 0 llprio 3\r
\x09groups: pflog\r
MARK-ifall-0\r
origami# halt -p\r\r

"""

TRUNCATED_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
\rroot on sd0a swap on sd0b dump on sd0b
\rWARNING: clock gained 14 days
\rWARNING: CHECK AND RESET THE DATE!
\rAutomatic boot in progress: starting file system checks.\r
/dev/sd0a (18cc54198c86b2cb.a): file system is clean; not checking\r
pf enabled\r
starting network\r
reordering libraries: done.\r
starting early daemons: syslogd pflogd ntpd.\r
starting RPC daemons:.\r
kvm_mkdb: can't open /dev/ksyms\r
savecore: /bsd: kvm_read: version misread\r
checking quotas: done.\r
clearing /tmp\r
kern.securelevel: 0 -> 1\r
creating runtime link editor directory cache.\r
preserving editor files.\r
starting network daemons: sshd smtpd sndiod.\r
starting local daemons: cron.\r
Wed Aug 26 07:09:37 UTC 2026\r
\r
OpenBSD/sgi (origami.my.domain) (console)\r
\r
login: root\r
Password:\r
Last login: Wed Aug 12 05:40:43 on console\r
"""

SYNTHETIC_DDB_WEDGE_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
\rroot on sd0a swap on sd0b dump on sd0b
\rWARNING: clock gained 14 days
\rWARNING: CHECK AND RESET THE DATE!
\rAutomatic boot in progress: starting file system checks.\r
/dev/sd0a (18cc54198c86b2cb.a): file system is clean; not checking\r
pf enabled\r
starting network\r
Trap cause = 7 Frame 0xffffffff82b2f9c8
\rTrap PC 0xa80000000027bf18 RA 0xa80000000048aab8 fault 0x1c533f4000
\r0xa80000000027bf10 (a800000013ffc000,1,a80000000078d0a0,0)  ra 0xa80000000048aab8 sp 0xffffffff82b2fb20, sz 0
\r0xa80000000048a5b4 (a800000013ffc000,1,a80000000078d0a0,0)  ra 0x0 sp 0xffffffff82b2fb20, sz 0
\rUser-level: pid 72149
\rstopped on non ddb fault
\rStopped at      0xa80000000027bf18:     sd      zero,0(a0)
\rddb> """


class LoginVerdictTest(unittest.TestCase):

    def test_real_reference_transcript_passes(self):
        report = login_verdict(REAL_LOGIN_TRANSCRIPT)
        self.assertEqual(report["problems"], [])
        self.assertEqual(report["verdict"], "PASS")

    def test_truncated_transcript_fails_never_reached_shell(self):
        # A run that hung or was killed right after a correct password,
        # before the shell prompt ever printed, must FAIL -- not read as
        # a login just because it got through the password step.
        self.assertIn("Last login:", TRUNCATED_TRANSCRIPT)
        self.assertNotIn("origami#", TRUNCATED_TRANSCRIPT)
        report = login_verdict(TRUNCATED_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("never reached a root shell prompt", report["problems"])

    def test_ddb_wedge_before_login_fails_and_is_not_read_as_a_login(self):
        # The single most important correctness property this module
        # exists to prove: a run that falls into the kernel debugger
        # before it ever reaches the login prompt must FAIL, not be read
        # as a login -- even though it never gets far enough to fail the
        # "never reached the login prompt" check on its own timing.
        self.assertNotIn("login:", SYNTHETIC_DDB_WEDGE_TRANSCRIPT)
        report = login_verdict(SYNTHETIC_DDB_WEDGE_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])
        self.assertIn("the guest took a bus error", report["problems"])
        self.assertIn("never reached the login prompt", report["problems"])

    def test_panic_before_shell_fails_even_if_a_shell_prompt_appears_later(self):
        # Small targeted probe, not a full captured transcript: proves the
        # panic: trip wire independently of ddb>, and proves a trip wire
        # before the shell fails even when "# " does eventually appear
        # afterward -- the wedge is not laundered by whatever the guest
        # prints once it comes back, if it ever does.
        transcript = (
            "root device: sd0a\r\nswap device (default sd0b): \r\n"
            "login: root\r\nPassword:\r\n"
            "panic: kernel diagnostic assertion failed\r\nddb> # \r\n")
        report = login_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest panicked", report["problems"])

    def test_bus_error_trip_wire_fires_independently_of_ddb(self):
        transcript = (
            "root device: sd0a\r\nswap device (default sd0b): \r\n"
            "login: root\r\nPassword:\r\n"
            "Trap cause = 7 Frame 0xffffffff82b2f9c8\r\nddb> ")
        report = login_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest took a bus error", report["problems"])
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])

    def test_never_reaches_the_root_device_question(self):
        report = login_verdict("ARCS64 Firmware\r\nFound SGI-IP27.\r\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["problems"],
                         ["never reached the root device question"])

    def test_login_without_root_device_question_first_does_not_count(self):
        # Ordering matters: a "login:" that appears without "root
        # device:" ever having been answered first must not be read as
        # having reached that stage.
        report = login_verdict("login: root\r\nPassword:\r\n# \r\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("never reached the root device question",
                      report["problems"])

    def test_shell_prompt_before_login_does_not_satisfy_the_shell_stage(self):
        # A bare "# " that appears before "login:" must not satisfy the
        # shell-prompt check -- the search for it only starts after the
        # login prompt is matched, so an earlier one is not read as an
        # earlier shell.
        report = login_verdict("root device: sd0a\r\n# \r\nlogin: root\r\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("never reached a root shell prompt", report["problems"])

    def test_empty_transcript_fails_missing_everything(self):
        report = login_verdict("")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["problems"],
                         ["never reached the root device question"])


if __name__ == "__main__":
    unittest.main()
