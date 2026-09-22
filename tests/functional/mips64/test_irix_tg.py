#!/usr/bin/env python3
#
# IRIX tg (Broadcom BCM570x) network bring-up: boot the installed 6.5.30 disk
# on the real IP27 PROM, log in, bring up the tg adapter, wait for its link,
# ping the SLIRP gateway, and read the interface's own packet counters back --
# tg1's Ipkts and Opkts climbing (with no input errors) is the device-level
# proof that frames crossed both the adapter's receive and transmit rings.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
This is the tg (bcm570x) counterpart of test_irix_eg_net.py: same real IP27
PROM, same installed IRIX disk, same SLIRP wiring, driven the same way. It is
IRIX on purpose -- the tg adapter is an IRIX-era SGI Ethernet option, so IRIX's
own tg driver is the receiver whose behaviour the model must satisfy.

What it proves: with a network back end placed only on the tg (bcm570x) device
at a real Origin 200 PCI option slot, IRIX names the adapter tg1, its driver
brings the link up (the NOTICE "tg1: Link up: ... Mbps"), and a burst of pings
to the SLIRP gateway moves tg1's own interface counters -- transmitted packets
climb (the adapter's transmit DMA put the echo requests on the wire) and
received packets climb with zero input errors (its receive DMA took the gateway
replies back off it, clean). That is a full round-trip exercise of the device's
data path, transmit and receive, checked at the counters the driver itself
maintains.

Why the interface counters and not the ping's own summary: over QEMU user-mode
SLIRP the guest ping prints "0 packets received" even when the replies did
arrive, because SLIRP answers the gateway echo with an ICMP id the guest ping
does not match back to its request -- a known SLIRP artifact, not a device
fault. So this test never trusts the ping's received count; it trusts tg1's
Ipkts/Opkts in `netstat -in` (both must climb, Ierrs must stay zero) together
with the driver's link-up notice. The register, ring and DMA contract is
exercised separately by the tg qtest.

The network back end is placed only on the tg device; the machine's on-board
IOC3 Ethernet is left with no back end (-nic none), so tg1 is the only interface
that can carry the pings.

tg_acceptance_verdict() (test_irix_tg_verdict.py, imported below) is the pure
PASS/FAIL contract, testable without a boot; see its docstring for why each
clause is scored the way it is.

Guarded by @skipBigDataTest() (the disk is multi-gigabyte) and @skipSlowTest()
(a real multiuser IRIX boot is minutes), on top of
@skipIfMissingEnv('ORIGAMI_ASSETS') (skips only when the env var itself is
unset) and a self.fail() at method entry if it is set but the PROM or
installed disk is missing -- a missing asset with the env var set means CI
provisioning failed, not a reason to skip quietly, matching
test_irix_eg_net.py. The installed IRIX disk is not committed (the fork
ships no IRIX bits); a runner provisions it at
<ORIGAMI_ASSETS>/assets/irix/6.5.30/root.qcow2 the same way the firmware and
miniroot kernel are provisioned out of band.
"""

import json
import os
import re
import sys
import time

# This file's own directory, so a plain `pytest test_irix_tg.py` run finds the
# verdict module the same way the other functional tests here find theirs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import Console, HarnessError  # noqa: E402
from lib_prom import (MACHINES, file_sha256,  # noqa: E402
                      require_assets)
from test_irix_tg_verdict import tg_acceptance_verdict  # noqa: E402

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"

# Where ORIGAMI_ASSETS resolves the real PROM and the installed IRIX disk.
# ORIGAMI_ASSETS names a CHECKOUT (one with its own assets/ directory), not
# assets/ itself, matching test_irix_eg_net.py's FIRMWARE_REL.
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root.qcow2')

# The release-matched PROM SHA-256, so a stale or wrong-release firmware is
# caught before a boot is spent on it. Same digest test_irix_eg_net.py carries.
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
)

# 128 MiB, the same size and reasoning test_irix_eg_net.py uses: at or below
# 128 MiB the guest's whole memory bank fits the one slot the machine backs, so
# the filed >128MiB memory-map defects have no address at which to appear. A
# real multiuser IRIX boot needs the headroom over the 64 MiB the miniroot
# netboot uses.
MEMORY_MIB = 128

# SLIRP defaults: the gateway (which answers ICMP echo) and the guest's first-
# lease address, the values the tg bring-up captures used.
GATEWAY = "10.0.2.2"
GUEST_ADDR = "10.0.2.15"

# The tg (bcm570x) device sits at this Origin 200 PCI option slot; IRIX names
# the Broadcom Gigabit Ethernet adapter there tg1 (confirmed by the tg bring-up
# captures). A card MAC is passed on the command line, matching the bring-up
# invocation; the driver takes the system Ethernet address regardless.
TG_ADDR = 4
TG_MAC = "52:54:00:bc:57:01"
IFACE = "tg1"

# Pings to the gateway, enough that tg1's counters move well past a single
# frame in each direction (each echo is one TX frame and, answered, one RX
# frame, plus the opening ARP exchange).
PING_COUNT = 8

# A real multiuser IRIX boot from the installed disk, on the real PROM, is
# minutes: the PROM's own power-on diagnostics, then the kernel, then init to a
# console login. Kept a little under the meson timeout registered for this test
# in mips64/meson.build; the bring-up steps run after this budget under their
# own fixed per-step timeouts below.
LOGIN_BUDGET_SECONDS = 1200
STEP_TIMEOUT_SECONDS = 300

# The tg driver announces the link a short time after `ifconfig up`, once the
# PHY negotiates. Generous headroom over the ~30-40s the captures showed.
LINK_TIMEOUT_SECONDS = 180


class _Console(Console):
    """lib_net's console driver, watching for the faults an IRIX guest or
    its PROM shows when it wedges rather than answers."""

    TRIPWIRES = (
        (re.compile(rb"PANIC"), "the guest panicked"),
        (re.compile(rb"Data bus error"), "the guest took a data bus error"),
        (re.compile(rb"\bException\b\s*<"), "the PROM took an exception"),
    )
    STEP_TIMEOUT = STEP_TIMEOUT_SECONDS


# The System Maintenance Menu answer and the login exchange, each firing once
# against the tail of what has arrived. "1" is Start System (boot the local SCSI
# disk); root has no password on the installed image; the TERM prompt is
# answered with the vt100 default. Identical to test_irix_eg_net.py.
def _drive_to_shell(con, budget):
    """Drive the PROM menu, log in as root, answer TERM, and switch to /bin/sh.
    Returns once a Bourne shell is confirmed responsive."""
    deadline = time.monotonic() + budget
    left = lambda cap: max(1.0, min(cap, deadline - time.monotonic()))

    con.expect(rb"Option\?", left(budget), what="the maintenance menu")
    con.send("1")
    con.expect(rb"login:", left(budget), what="the console login prompt")
    con.send("root")
    # First login sets TERM; accept the vt100 default. The prompt is answered
    # before the shell appears.
    con.expect(rb"TERM\s*=\s*\(", left(120), what="the TERM prompt")
    con.send("")
    # The login csh prints an "<hostname> <histnum># " prompt; the history
    # number makes it distinctive enough not to match boot text.
    con.expect(rb"[0-9]# ", left(120), what="the login csh prompt")
    # One Bourne idiom for the rest of the drive; a wide PATH so the /usr/etc
    # and /usr/bsd network tools are found by name.
    con.send("exec /bin/sh")
    con.run("PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; "
            "export PATH", "shinit", timeout=int(left(120)))


if QemuSystemTest is not None:

    class IrixTg(QemuSystemTest):
        """Boots installed IRIX 6.5.30 on the real PROM, brings up tg, pings the
        SLIRP gateway, and requires tg_acceptance_verdict() to be PASS. See this
        module's docstring for the full contract."""

        def _read_counters(self, con, tag):
            """tg1's (Ipkts, Ierrs, Opkts) from netstat -in, each an int or
            None if it could not be read. netstat -in columns are
            Name Mtu Network Address Ipkts Ierrs Opkts ... so Ipkts is field 5,
            Ierrs field 6, Opkts field 7."""
            try:
                triple = con.capture_value(
                    "netstat -in | awk '$1 ~ /^" + IFACE +
                    "/ {print \"CNT=\" $5 \",\" $6 \",\" $7; exit}'",
                    tag, rb"CNT=(\d+,\d+,\d+)", timeout=60)
            except HarnessError:
                return None, None, None
            ipkts, ierrs, opkts = (int(v) for v in triple.split(","))
            return ipkts, ierrs, opkts

        def _configure_tg(self, con):
            """Bring tg1 up with the SLIRP guest address on its own subnet (the
            gateway is directly reachable, no route needed). Returns
            (tg1_present, tg_up): whether tg1 shows in `ifconfig -a` and whether
            it reports UP."""
            con.run("hinv | grep -i ether", "hinv", timeout=60)
            con.run(f"ifconfig {IFACE} inet {GUEST_ADDR} "
                    "netmask 0xffffff00 up", "ifup", timeout=60)
            present = False
            try:
                con.capture_value("ifconfig -a", "ifall",
                                  rb"(" + IFACE.encode() + rb"):", timeout=60)
                present = True
            except HarnessError:
                present = False
            up = False
            try:
                con.capture_value("ifconfig " + IFACE, "ifshow",
                                  rb"(UP)[,>]", timeout=60)
                up = True
            except HarnessError:
                up = False
            return present, up

        def _await_link(self, con):
            """Wait for the tg driver's link-up NOTICE on the console. Returns
            (link_up, line): the boolean and the matched notice text (or None)."""
            try:
                m = con.expect(rb"" + IFACE.encode() + rb": Link up[^\r\n]*",
                               LINK_TIMEOUT_SECONDS,
                               what="the tg1 link-up notice")
                return True, m.group(0).decode("utf-8", errors="replace")
            except HarnessError:
                return False, None

        def _ping(self, con):
            """Ping the gateway. The exit code is ignored on purpose: over SLIRP
            the guest ping reports 100% loss even when the replies arrived (a
            SLIRP ICMP-id artifact), so this test reads tg1's own counters
            instead."""
            try:
                con.run(f"ping -c {PING_COUNT} {GATEWAY}", "ping",
                        timeout=120)
            except HarnessError:
                pass

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_tg(self):
            assets = require_assets(self, FIRMWARE_REL, DISK_REL)
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]

            if file_sha256(prom) != PROM_SHA256:
                self.fail('the 6.5.30 PROM SHA-256 does not match -- a stale '
                          'or wrong-release firmware')

            self.require_netdev('user')

            pcap = self.log_file('tg-wire.pcap')

            vm = self.get_vm(name='irix_tg')
            vm.set_machine(MACHINES['origin200'])
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            vm.add_args(
                '-accel', 'tcg,thread=single',
                '-cpu', 'R10000', '-smp', '1', '-m', str(MEMORY_MIB),
                '-bios', prom,
                '-audio', 'none',
                '-chardev', f'file,id=elsc,path={elsc_log}',
                '-serial', 'none',
                '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot',
                '-nic', 'none',
                '-netdev', 'user,id=tgnet',
                '-device',
                f'bcm570x,netdev=tgnet,addr={TG_ADDR},mac={TG_MAC}',
                '-object', f'filter-dump,id=tgcap,netdev=tgnet,file={pcap}')
            vm.launch()

            log_path = self.log_file('irix-tg.log')
            con = _Console(vm.console_socket, log_path,
                          exited=lambda: vm.exitcode() is not None)
            login_reached = False
            tg1_present = False
            tg_up = False
            link_up = False
            link_line = None
            counters_before = (None, None, None)
            counters_after = (None, None, None)
            try:
                try:
                    _drive_to_shell(con, LOGIN_BUDGET_SECONDS)
                    login_reached = True
                    tg1_present, tg_up = self._configure_tg(con)
                    link_up, link_line = self._await_link(con)
                    counters_before = self._read_counters(con, "before")
                    self._ping(con)
                    counters_after = self._read_counters(con, "after")
                    con.send("sync; sync")
                    time.sleep(2)
                    con._pump()
                except HarnessError:
                    # A harness timeout leaves whatever was scraped so far in
                    # place; the verdict below reports the milestone that stalled.
                    pass
            finally:
                transcript = con.buf.decode("utf-8", errors="replace")
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early. Check the emulator '
                    'built, and that nothing ended the machine before it '
                    'printed anything.')

            traffic = {
                "ipkts_before": counters_before[0],
                "ierrs_before": counters_before[1],
                "opkts_before": counters_before[2],
                "ipkts_after": counters_after[0],
                "ierrs_after": counters_after[1],
                "opkts_after": counters_after[2],
            }
            report = tg_acceptance_verdict(
                login_reached, tg1_present, tg_up, link_up, traffic)

            summary_path = self.log_file('irix-tg-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({
                    'schema': 1,
                    'login_reached': login_reached,
                    'tg1_present': tg1_present,
                    'tg_up': tg_up,
                    'link_up': link_up,
                    'link_line': link_line,
                    'traffic': traffic,
                    'report': report,
                }, summary_file, indent=2, sort_keys=True, default=str)
                summary_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX tg network bring-up problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path}, console log: {log_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
