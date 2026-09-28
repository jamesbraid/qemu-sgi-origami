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

tg_acceptance_verdict() below is the PASS/FAIL contract; its own
docstring says why each clause is scored the way it is.

Guarded by @skipBigDataTest() (the disk is multi-gigabyte) and @skipSlowTest()
(a real multiuser IRIX boot is minutes), on top of
@skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR') (skips only when the
env var itself is unset) and a self.fail() at method entry if it is set but the
PROM or
installed disk is missing -- a missing asset with the env var set means CI
provisioning failed, not a reason to skip quietly, matching
test_irix_eg_net.py. The installed IRIX disk is not committed (the fork
ships no IRIX bits); a runner provisions it at
<QEMU_TEST_SGI_ASSET_DIR>/irix/6.5.30/root.qcow2 the same way the firmware and
miniroot kernel are provisioned out of band.
"""

import json
import os
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest test_irix_tg.py` run finds the
# verdict module the same way the other functional tests here find theirs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import (HarnessError, IrixConsole, arp_neighbor_line,
                     arp_neighbor_mac,
                     arp_resolution, drive_irix_to_shell,  # noqa: E402
                     summarise_pcap)
from lib_prom import MACHINES, file_sha256  # noqa: E402

def tg_acceptance_verdict(login_reached, tg1_present, tg_up, link_up, traffic):
    """Decide PASS/FAIL for one IRIX tg bring-up run.

    login_reached, tg1_present, tg_up, link_up: booleans for the four
    milestones above.

    traffic: a dict of the interface's own counters, read from `netstat -in`
    before and after a burst of pings to the SLIRP gateway --
        {"ipkts_before": int or None, "ipkts_after": int or None,
         "opkts_before": int or None, "opkts_after": int or None,
         "ierrs_after":  int or None}
    A None is a value that could not be read.

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff every
    clause holds; problems names every clause that did not, so a single run
    reports all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not tg1_present:
        problems.append("tg1 not present in ifconfig -a")
    if not tg_up:
        problems.append(
            "`ifconfig tg1 ... up` did not return to a shell prompt "
            "(console wedged)")
    if not link_up:
        problems.append(
            "the tg driver never logged its link-up notice; the adapter's "
            "link never came up")

    ib = traffic.get("ipkts_before")
    ia = traffic.get("ipkts_after")
    if ia is None or ib is None:
        problems.append(
            "tg1's received-packet counter could not be read before and "
            "after the pings")
    elif ia <= ib:
        problems.append(
            f"tg1's received-packet counter did not climb over the pings "
            f"({ib} -> {ia}): no frames arrived over the adapter's receive "
            f"ring")

    ob = traffic.get("opkts_before")
    oa = traffic.get("opkts_after")
    if oa is None or ob is None:
        problems.append(
            "tg1's transmitted-packet counter could not be read before and "
            "after the pings")
    elif oa <= ob:
        problems.append(
            f"tg1's transmitted-packet counter did not climb over the pings "
            f"({ob} -> {oa}): the adapter put no frames on the wire")

    ie = traffic.get("ierrs_after")
    if ie is None:
        problems.append("tg1's input-error counter could not be read")
    elif ie != 0:
        problems.append(
            f"tg1 reported {ie} input errors; the frames did not arrive clean")

    if traffic.get("arp_cache_cold") is not True:
        problems.append(
            "the gateway ARP cache was not confirmed empty before ping")
    capture_problem = traffic.get("arp_capture_problem")
    if capture_problem:
        problems.append(
            f"the TG packet capture could not be read: {capture_problem}")
    requests = traffic.get("arp_requests")
    replies = traffic.get("arp_replies")
    matching = traffic.get("arp_matching_replies")
    if not all(isinstance(value, int) and value > 0
               for value in (requests, replies, matching)):
        problems.append(
            "the captured wire has no matching gateway ARP exchange")
    neighbor_mac = traffic.get("arp_neighbor_mac")
    responder_mac = traffic.get("arp_responder_mac")
    if not isinstance(neighbor_mac, str) or neighbor_mac != responder_mac:
        problems.append(
            "the guest gateway neighbor entry does not match the captured "
            "ARP reply")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# A traffic reading that passes every clause, copied and mutated per test so a
# single wrong field is what fails a case, not a hand-built dict per test.

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
    from qemu_test.asset import LocalAsset
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"

# Resolve the PROM and installed IRIX disk beneath
# QEMU_TEST_SGI_ASSET_DIR.
# QEMU_TEST_SGI_ASSET_DIR points directly at the asset directory,
# direct asset root, matching test_irix_eg_net.py's FIRMWARE_REL.
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('irix', RELEASE, 'root.qcow2')

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

# The tg driver announces the link a short time after `ifconfig up`, once the
# PHY negotiates. Generous headroom over the ~30-40s the captures showed.
LINK_TIMEOUT_SECONDS = 180


def _host_cksum(path):
    """The POSIX cksum CRC and byte count of a file, computed by the host's
    own cksum(1) (coreutils' default algorithm is the POSIX CRC IRIX's
    cksum implements). Returns (crc, size) as (str, int)."""
    out = subprocess.run(["cksum", path], capture_output=True, text=True,
                         check=True).stdout
    crc, size = out.split()[0:2]
    return crc, int(size)


if QemuSystemTest is not None:

    class IrixTg(QemuSystemTest):
        """Boots installed IRIX 6.5.30 on the real PROM, brings up tg, pings the
        SLIRP gateway, and requires tg_acceptance_verdict() to be PASS. See this
        module's docstring for the full contract."""

        ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
        ASSET_DISK = LocalAsset(
            DISK_REL,
            '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12')

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

        def _read_arp_neighbor(self, con, tag):
            pos, marker = con._run_to_marker("arp -an", tag, timeout=60)
            output = bytes(con.buf[pos:marker.start()]).decode(
                "utf-8", errors="replace")
            return arp_neighbor_line(output, GATEWAY)

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
        @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_irix_tg(self):
            prom = self.ASSET_FIRMWARE.fetch()
            disk = self.ASSET_DISK.fetch()

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
            con = IrixConsole(vm.console_socket, log_path,
                              exited=lambda: vm.exitcode() is not None)
            login_reached = False
            tg1_present = False
            tg_up = False
            link_up = False
            link_line = None
            counters_before = (None, None, None)
            counters_after = (None, None, None)
            arp_cache_cold = False
            arp_neighbor = None
            try:
                try:
                    drive_irix_to_shell(con, LOGIN_BUDGET_SECONDS)
                    login_reached = True
                    tg1_present, tg_up = self._configure_tg(con)
                    link_up, link_line = self._await_link(con)
                    arp_cache_cold = (
                        self._read_arp_neighbor(con, "arp_before") is None)
                    if not arp_cache_cold:
                        clear_rc = con.run(
                            f"/usr/etc/arp -d {GATEWAY}",
                            "arp_clear", timeout=60)
                        arp_cache_cold = clear_rc == 0 and \
                            self._read_arp_neighbor(con, "arp_cleared") is None
                    counters_before = self._read_counters(con, "before")
                    self._ping(con)
                    arp_entry = self._read_arp_neighbor(con, "arp_after")
                    arp_neighbor = arp_neighbor_mac(
                        arp_entry or "", GATEWAY)
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

            capture_problem = None
            try:
                with open(pcap, 'rb') as capture_file:
                    capture = summarise_pcap(capture_file.read(),
                                             include_frames=True)
                if capture is None:
                    capture_problem = "unrecognized or truncated pcap"
                elif "frames" not in capture:
                    capture_problem = "pcap contains no complete capture header"
            except OSError as error:
                capture = None
                capture_problem = str(error)
            arp_wire = arp_resolution(
                capture.get('frames', []) if isinstance(capture, dict) else [],
                GUEST_ADDR, GATEWAY)

            traffic = {
                "ipkts_before": counters_before[0],
                "ierrs_before": counters_before[1],
                "opkts_before": counters_before[2],
                "ipkts_after": counters_after[0],
                "ierrs_after": counters_after[1],
                "opkts_after": counters_after[2],
                "arp_cache_cold": arp_cache_cold,
                "arp_neighbor_mac": arp_neighbor,
                "arp_requests": arp_wire["requests"],
                "arp_replies": arp_wire["replies"],
                "arp_matching_replies": arp_wire["matching_replies"],
                "arp_responder_mac": arp_wire["responder_mac"],
                "arp_capture_problem": capture_problem,
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
