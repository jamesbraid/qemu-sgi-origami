#!/usr/bin/env python3
#
# IRIX tg (Broadcom BCM570x) bring-up and transport on one Origin 200 boot:
# boot the installed 6.5.30 disk on the real IP27 PROM, log in, bring up the
# tg adapter, wait for its link, ping the SLIRP gateway and read the
# interface's own packet counters back, then move byte-checked TCP and UDP
# traffic across it in both directions and time ttcp throughput runs.
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
maintains. It also reads the saved tgconfig settings, checks them against the
installed options file, and requires the driver's L2IPFRAG and L2TCPSEG flags
after bring-up. These are observations of IRIX under QEMU; they do not by
themselves establish physical-board behavior.

Why the interface counters and not the ping's own summary: the saved packet
capture has eight gateway echo replies, each matching a guest request by
sequence and exact payload, while the guest ping still prints "0 packets
received". The capture establishes that the reply reached the adapter's link;
it does not establish that IRIX accepted it. This test therefore checks the
wire match and tg1's Ipkts/Opkts in `netstat -in` (both must climb, Ierrs must
stay zero) together with the driver's link-up notice, without treating the
ping summary as proof of delivery to the guest. The register, ring and DMA
contract is exercised separately by the tg qtest.

The network back end is placed only on the tg device; the machine's on-board
IOC3 Ethernet is left with no back end (-nic none), so tg1 is the only interface
that can carry the pings.

After the ping, the same boot exercises the transport layers and measures
throughput:

- TCP correctness: a 1 MiB payload streamed host->guest into ttcp -r, checked
  by cksum and byte count on both sides.
- TCP throughput: ttcp -r receiving a bulk stream, reporting the rate.
- UDP throughput: ttcp -u -r receiving a bulk datagram stream, reporting the
  rate and the loss.
- UDP transmit: eight exact payload datagrams received by a host socket.
- TCP transmit: a 64 KiB nonuniform pattern received and compared byte for
  byte by a host socket.
- TCP proof: the guest's netstat -s TCP segment counter before and after, so
  the segments its stack processed are recorded, not merely that a program
  exited 0.

The receive legs run host->guest through SLIRP hostfwd. The guest sends the
transmit checks to host sockets through the SLIRP gateway. ttcp is in the
installed 6.5.30 base image.

tg_acceptance_verdict() scores the bring-up and tg_perf_verdict() the
transport legs; both docstrings say why each clause is scored the way it is.
The summary keeps the two verdicts' problems side by side, so a transport
failure does not hide the bring-up result.

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
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

# This file's own directory, so a plain `pytest test_irix_tg.py` run finds the
# verdict module the same way the other functional tests here find theirs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import (HarnessError, IrixConsole, arp_neighbor_line,
                     arp_neighbor_mac,
                     arp_resolution, drive_irix_to_shell,
                     icmp_echo_exchange, icmp_ipv4_exchange,  # noqa: E402
                     summarise_pcap)
from lib_prom import MACHINES, file_sha256  # noqa: E402

def _labelled_values(output):
    """Read the label/value forms printed by tgconfig and its options file."""
    values = {}
    for line in output.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or line.startswith("DEVICE"):
            continue
        match = re.fullmatch(r"([A-Za-z][\w.]*)(?:\s*[=:]\s*|\s+)(\S+)",
                             line)
        if match:
            values[match.group(1).lower()] = match.group(2)
    return values


def _command_payload(output, command, tag):
    """Remove only the exact echoed command wrapper at the capture start."""
    echo = f'{command}; echo "MARK""-{tag}-$?"'.encode()
    if output.startswith(echo + b"\r\n"):
        output = output[len(echo) + 2:]
    elif output.startswith(echo + b"\n"):
        output = output[len(echo) + 1:]
    return output


def _command_output_text(output, command, tag):
    """Decode command output, preserving all bytes after its echoed wrapper."""
    return _command_payload(output, command, tag).decode(
        "utf-8", errors="replace")


def tg_acceptance_verdict(login_reached, tg1_present, tg_up, link_up,
                          tg_flags, board, traffic):
    """Decide PASS/FAIL for one IRIX tg bring-up run.

    login_reached, tg1_present, tg_up, link_up: booleans for the four
    milestones above. tg_flags is the set printed by `ifconfig tg1`;
    board carries the tgconfig readback and options-file checks.

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

    if not board.get("bare_clean"):
        problems.append("bare tgconfig was not a clean no-op")
    saved = board.get("saved", {})
    required_options = {"autoneg", "link_pref", "flow_ctrl"}
    if not board.get("saved_status") or not required_options <= saved.keys():
        problems.append("tgconfig -g did not report the saved link settings")
    if saved.get("link_pref") not in ("AUTO", "1000_FULL"):
        problems.append(
            "tgconfig -g reports a link preference the gigabit PHY cannot "
            "satisfy")
    if not board.get("check_mode"):
        problems.append("tgconfig check mode rejected its installed options")
    options = board.get("options", {})
    if (not board.get("options_status")
            or not required_options <= options.keys()
            or any(saved.get(name) != options[name]
                   for name in required_options)):
        problems.append(
            "tgconfig -g does not match /etc/config/tgconfig.options")
    if not {"L2IPFRAG", "L2TCPSEG"} <= set(tg_flags):
        problems.append("tg1 is missing the L2IPFRAG or L2TCPSEG flag")

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

    guest_to_gateway = traffic.get("icmp_guest_to_gateway")
    gateway_to_guest = traffic.get("icmp_gateway_to_guest")
    if not isinstance(guest_to_gateway, int) or guest_to_gateway < 1:
        problems.append("the capture has no guest-to-gateway ICMP IPv4 frame")
    if not isinstance(gateway_to_guest, int) or gateway_to_guest < 1:
        problems.append("the capture has no gateway-to-guest ICMP IPv4 frame")
    invalid_packets = traffic.get("icmp_invalid_packets")
    if not isinstance(invalid_packets, int) or invalid_packets != 0:
        problems.append(
            "the captured guest/gateway ICMP IPv4 frames include a malformed "
            "IPv4 header or invalid checksum")
    echo_matches = traffic.get("icmp_echo_matching_replies")
    if not isinstance(echo_matches, int) or echo_matches < 1:
        problems.append(
            "the capture has no gateway echo reply matching a guest request "
            "by sequence and exact payload")

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


def tg_perf_verdict(login_reached, tg_up, results):
    """Decide PASS/FAIL for one IRIX tg transport-and-throughput run.

    login_reached, tg_up: booleans for the two upstream milestones.

    results: the dict the test records --
        {"tcp_verify": {"host_cksum": str, "host_bytes": int,
                        "guest_cksum": str or None,
                        "guest_bytes": int or None} or None,
         "tcp_mbps": float or None,
         "udp_mbps": float or None,
         "udp_loss": float or None,
         "udp_bytes": int or None,
         "udp_tx": {"payload_count": int, "payload_matches": bool,
                    "errors": list[str], "timed_out": bool} or None,
         "tcp_tx": {"payload_bytes": int, "payload_matches": bool,
                    "errors": list[str], "timed_out": bool} or None,
         "tcp_segments_delta": int or None}

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff every
    clause holds; problems names every clause that did not, so one run reports
    all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not tg_up:
        problems.append("the tg interface never came up configured")

    verify = results.get("tcp_verify")
    if not verify:
        problems.append("the byte-checked TCP transfer did not run")
    else:
        gc, gb = verify.get("guest_cksum"), verify.get("guest_bytes")
        hc, hb = verify.get("host_cksum"), verify.get("host_bytes")
        if gc is None or gb is None:
            problems.append(
                "the guest never reported a checksum and size for the TCP "
                "payload it received")
        else:
            if gb != hb:
                problems.append(
                    f"TCP size mismatch: the guest received {gb} bytes, "
                    f"the host sent {hb}")
            if gc != hc:
                problems.append(
                    f"TCP checksum mismatch: the guest computed {gc}, "
                    f"the host computed {hc}")

    segs = results.get("tcp_segments_delta")
    if segs is None:
        problems.append(
            "the guest's TCP segment counter could not be read")
    elif segs <= 0:
        problems.append(
            f"the guest's TCP stack processed no segments ({segs}): the bytes "
            f"did not arrive as TCP through tg")

    if results.get("tcp_mbps") is None:
        problems.append("no TCP throughput was measured")

    udp_bytes = results.get("udp_bytes")
    if udp_bytes is None:
        problems.append("no UDP datagrams were measured")
    elif udp_bytes <= 0:
        problems.append(
            "the guest received no UDP payload at all: the datagram path "
            "carried nothing")

    udp_tx = results.get("udp_tx")
    if not udp_tx:
        problems.append("guest-to-host UDP did not run")
    else:
        if udp_tx.get("timed_out"):
            problems.append("the host UDP receiver timed out")
        if udp_tx.get("errors"):
            problems.append("the host UDP receiver reported: " +
                            "; ".join(udp_tx["errors"]))
        if udp_tx.get("payload_count") != 8:
            problems.append(
                "the host did not receive eight guest UDP payloads "
                f"(received {udp_tx.get('payload_count')})")
        if not udp_tx.get("payload_matches"):
            problems.append("guest-to-host UDP payloads did not match")

    tcp_tx = results.get("tcp_tx")
    if not tcp_tx:
        problems.append("guest-to-host TCP did not run")
    else:
        if tcp_tx.get("timed_out"):
            problems.append("the host TCP receiver timed out")
        if tcp_tx.get("errors"):
            problems.append("the host TCP receiver reported: " +
                            "; ".join(tcp_tx["errors"]))
        if tcp_tx.get("payload_bytes") != 65536:
            problems.append(
                "the host did not receive the complete guest TCP payload "
                f"(received {tcp_tx.get('payload_bytes')} bytes)")
        if not tcp_tx.get("payload_matches"):
            problems.append("guest-to-host TCP payload did not match")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


TCP_GUEST_PORT = 5001
UDP_GUEST_PORT = 5002
TCP_THROUGHPUT_BYTES = 4 * 1024 * 1024    # bulk TCP stream
SEND_TIMEOUT_SECONDS = 600                # a bulk send drains at guest speed
UDP_BUFLEN = 8192
UDP_BUFS = 512                            # 4 MiB of datagrams
UDP_PACE_EVERY = 16                       # datagrams between pauses
UDP_PACE_SECONDS = 0.02
VERIFY_BYTES = 1024 * 1024                # byte-checked TCP payload
TCP_TX_BYTES = 65536


def _usernet_hostport(vm, proto, guest_port):
    """Host port SLIRP assigned to a hostfwd rule, from 'info usernet'. The
    row is 'PROTO[HOST_FORWARD] <fd> 127.0.0.1 <hostport> <guestip> <gport>'
    (same shape qemu.utils.get_info_usernet_hostfwd_port matches for TCP),
    generalized here to a given protocol and guest port."""
    out = vm.cmd('human-monitor-command', command_line='info usernet')
    rx = re.compile(r'%s.HOST_FORWARD.*127\.0\.0\.1\s+(\d+)\s+%s\s+%d\b'
                    % (proto.upper(), re.escape(GUEST_ADDR), guest_port))
    for line in re.split(r'\r?\n', out):
        m = rx.search(line)
        if m:
            return int(m.group(1))
    return None


def _tcp_stream(host_port, data):
    """Connect to the forwarded TCP port, send data, close. Returns the
    host-side elapsed seconds."""
    s = socket.create_connection(("127.0.0.1", host_port), timeout=30)
    # The guest drains at emulated speed, so the send blocks for as long as the
    # transfer takes; the connect timeout above would cut a bulk one short.
    s.settimeout(SEND_TIMEOUT_SECONDS)
    t0 = time.monotonic()
    try:
        s.sendall(data)
    finally:
        s.shutdown(socket.SHUT_WR)
        # drain any trailing bytes so the guest sees a clean close
        s.settimeout(30)
        try:
            while s.recv(65536):
                pass
        except OSError:
            pass
        s.close()
    return time.monotonic() - t0


def _udp_sink(ready, port_holder, packets, controls, errors, stop):
    """Receive ttcp guest-to-host UDP payloads on an ephemeral host port."""
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        server.bind(('0.0.0.0', 0))
        server.settimeout(0.25)
        port_holder.append(server.getsockname()[1])
        ready.set()
        drain_until = None
        while not stop.is_set():
            if drain_until is not None and time.monotonic() >= drain_until:
                break
            try:
                data, _ = server.recvfrom(65536)
            except socket.timeout:
                if len(packets) >= 8 and drain_until is None:
                    drain_until = time.monotonic() + 0.5
                continue
            if not data:
                continue
            if data == bytes(4):
                controls.append(data)
            elif len(data) == 1024:
                packets.append(data)
                if len(packets) >= 8 and drain_until is None:
                    drain_until = time.monotonic() + 0.5
            else:
                errors.append(
                    f'unexpected UDP datagram length {len(data)} bytes')
                break
    except OSError as error:
        if not stop.is_set():
            errors.append(str(error))
    finally:
        ready.set()
        server.close()


def _tcp_sink(ready, port_holder, chunks, errors, stop):
    """Receive one guest TCP stream on an ephemeral host port."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('0.0.0.0', 0))
        server.listen(1)
        server.settimeout(0.25)
        port_holder.append(server.getsockname()[1])
        ready.set()
        conn = None
        while not stop.is_set() and conn is None:
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
        if conn is not None:
            with conn:
                conn.settimeout(0.25)
                while not stop.is_set():
                    try:
                        block = conn.recv(65536)
                    except socket.timeout:
                        continue
                    if not block:
                        break
                    chunks.append(block)
    except OSError as error:
        if not stop.is_set():
            errors.append(str(error))
    finally:
        ready.set()
        server.close()


def _udp_ttcp_stream(host_port, buflen, nbufs):
    """Mimic ttcp -u -t into the forwarded UDP port: nbufs datagrams of
    buflen, then several zero-length datagrams as ttcp's UDP end marker."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = bytes((i & 0xff) for i in range(buflen))
    t0 = time.monotonic()
    for i in range(nbufs):
        s.sendto(payload, ("127.0.0.1", host_port))
        # UDP has no flow control, and an emulated guest drains far slower than
        # this loop fills. Unpaced, almost every datagram is dropped before the
        # interface sees it, and what gets measured is how fast the host can
        # call sendto. Pace it to something the guest can plausibly take.
        if (i % UDP_PACE_EVERY) == UDP_PACE_EVERY - 1:
            time.sleep(UDP_PACE_SECONDS)
    for _ in range(8):
        s.sendto(b"", ("127.0.0.1", host_port))
        time.sleep(0.02)
    s.close()
    return time.monotonic() - t0


def _parse_ttcp_rate(text):
    """Pull (megabits_per_sec, bytes, seconds) out of ttcp's report, which
    prints e.g. 'ttcp-r: 16777216 bytes in 3.21 real seconds = ... KB/sec'.
    Returns (mbps, nbytes, secs) or (None, None, None)."""
    # The last such line, not the last "ttcp": the report ends with an I/O-call
    # summary, so slicing from the final "ttcp" cuts the rate line off.
    m = None
    for m in re.finditer(r'(\d+)\s+bytes\s+in\s+([\d.]+)\s+real\s+sec', text):
        pass
    if not m:
        return None, None, None
    nbytes, secs = int(m.group(1)), float(m.group(2))
    mbps = (nbytes * 8.0) / secs / 1e6 if secs > 0 else 0.0
    return mbps, nbytes, secs


class HelpersTest(unittest.TestCase):
    """_host_cksum is only reached once a live run gets as far as the TCP
    byte-verify step, so a run without QEMU_TEST_SGI_ASSET_DIR skips straight
    past a regression here. No boot needed to catch it."""

    def test_host_cksum_matches_cksum1(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"origami tg host_cksum regression\n")
            f.flush()
            crc, size = _host_cksum(f.name)
            want = subprocess.run(["cksum", f.name], capture_output=True,
                                  text=True, check=True).stdout.split()
        self.assertEqual((crc, size), (want[0], int(want[1])))


if QemuSystemTest is not None:

    class IrixTg(QemuSystemTest):
        """Boots installed IRIX 6.5.30 on the real PROM, brings up tg, pings the
        SLIRP gateway, moves TCP and UDP traffic across it, and requires both
        tg_acceptance_verdict() and tg_perf_verdict() to be PASS. See this
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

        def _command_output(self, con, command, tag):
            pos, marker = con._run_to_marker(command, tag, timeout=60)
            output = bytes(con.buf[pos:marker.start()])
            output_text = _command_output_text(output, command, tag)
            return int(marker.group(1)), output_text

        def _read_board_configuration(self, con):
            status, _output = self._command_output(
                con, "tgconfig", "tgconfig")
            bare_clean = status == 0
            status, output = self._command_output(
                con, "tgconfig -g", "tgconfig_get")
            saved = _labelled_values(output)
            saved_status = status == 0
            status, _output = self._command_output(
                con, "tgconfig -c -f /etc/config/tgconfig.options",
                "tgconfig_check")
            check_mode = status == 0
            status, output = self._command_output(
                con, "cat /etc/config/tgconfig.options", "tgconfig_options")
            options = _labelled_values(output) if status == 0 else {}
            return {
                "bare_clean": bare_clean,
                "saved_status": saved_status,
                "saved": saved,
                "check_mode": check_mode,
                "options_status": status == 0,
                "options": options,
            }

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
            flags = set()
            try:
                flag_text = con.capture_value(
                    "ifconfig " + IFACE, "ifflags",
                    rb"flags=[^<]*<([^>]+)>", timeout=60)
                flags = {flag.strip() for flag in flag_text.split(",")}
            except HarnessError:
                pass
            return present, up, flags

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

        def _netstat_counter(self, con, proto, pattern, tag):
            try:
                return int(con.capture_value(
                    f"netstat -s -p {proto} | " +
                    "awk '/" + pattern + "/ {print \"N=\" $1; exit}'",
                    tag, rb"N=(\d+)", timeout=60))
            except HarnessError:
                return None

        def _throughput(self, con, tcp_hp, udp_hp, results):
            """Run the transport legs on the booted guest and record each one
            in results, the dict tg_perf_verdict() scores."""
            seg_before = self._netstat_counter(
                con, "tcp", "packets received", "segb")

            # --- TCP byte-verify (host -> guest, saved by ttcp -r) ---
            self.assertIsNotNone(tcp_hp, "no TCP hostfwd port")
            verify_file = self.scratch_file('verify.bin')
            with open(verify_file, 'wb') as vf:
                vf.write(os.urandom(VERIFY_BYTES))
            host_crc, host_bytes = _host_cksum(verify_file)
            with open(verify_file, 'rb') as vf:
                verify_bytes = vf.read()
            # ttcp -r without -s writes the received stream to stdout.
            con.send(f"ttcp -r -p {TCP_GUEST_PORT} -l 65536 "
                     ">/tmp/rx.bin 2>/dev/null; echo \"RX\"\"DONE\"")
            time.sleep(3)
            _tcp_stream(tcp_hp, verify_bytes)
            con.expect(rb"RXDONE", 300, what="ttcp receive to finish")
            # ttcp appends its own report to the same stream, so checksum
            # exactly the payload rather than whatever else landed after it.
            # Tagged, because the echoed command line carries two numbers of
            # its own ("count=128 2>/dev/null") that an untagged pair of
            # integers matches before the output ever arrives.
            gcrc, gbytes = con.two_values(
                "dd if=/tmp/rx.bin bs=8192 count=%d 2>/dev/null | cksum | "
                "sed 's/^/CK=/'" % (VERIFY_BYTES // 8192), "vfy",
                rb"CK=(\d+)\s+(\d+)", timeout=180)
            results["tcp_verify"] = {
                "host_cksum": host_crc, "host_bytes": host_bytes,
                "guest_cksum": gcrc, "guest_bytes": gbytes}

            # --- TCP throughput (ttcp -r) ---
            # -s sinks the payload. Without it ttcp writes every received
            # byte to stdout, which here is the serial console: megabytes
            # through an emulated uart, and the rate measured would be the
            # console's rather than the interface's.
            con.send(f"ttcp -r -s -p {TCP_GUEST_PORT} -l 65536 2>&1; "
                     "echo \"TCP\"\"TP-$?\"")
            time.sleep(3)
            _tcp_stream(tcp_hp, os.urandom(TCP_THROUGHPUT_BYTES))
            con.expect(rb"TCPTP-\d+", 240, what="ttcp TCP to finish")
            tcp_out = con.buf.decode("utf-8", "replace")
            mbps, _, _ = _parse_ttcp_rate(tcp_out)
            results["tcp_mbps"] = mbps

            # --- UDP throughput (ttcp -u -r) ---
            con.send(f"ttcp -u -r -s -p {UDP_GUEST_PORT} -l {UDP_BUFLEN} "
                     "2>&1; echo \"UDP\"\"TP-$?\"")
            time.sleep(3)
            self.assertIsNotNone(udp_hp, "no UDP hostfwd port")
            _udp_ttcp_stream(udp_hp, UDP_BUFLEN, UDP_BUFS)
            con.expect(rb"UDPTP-\d+", 240, what="ttcp UDP to finish")
            udp_out = con.buf.decode("utf-8", "replace")
            umbps, ubytes, _ = _parse_ttcp_rate(udp_out)
            results["udp_mbps"] = umbps
            results["udp_bytes"] = ubytes
            if ubytes is not None:
                results["udp_loss"] = 1.0 - (ubytes /
                                             float(UDP_BUFLEN * UDP_BUFS))

            udp_tx_stop = threading.Event()
            udp_tx_thread = None
            udp_tx_ready = threading.Event()
            udp_tx_port = []
            udp_tx_packets, udp_tx_controls, udp_tx_errors = [], [], []
            tcp_tx_stop = threading.Event()
            tcp_tx_thread = None
            tcp_tx_ready = threading.Event()
            tcp_tx_port = []
            tcp_tx_chunks, tcp_tx_errors = [], []
            try:
                # --- Guest-to-host UDP correctness ---
                udp_tx_thread = threading.Thread(
                    target=_udp_sink,
                    args=(udp_tx_ready, udp_tx_port, udp_tx_packets,
                          udp_tx_controls, udp_tx_errors, udp_tx_stop),
                    daemon=True)
                udp_tx_thread.start()
                self.assertTrue(udp_tx_ready.wait(30),
                                "host UDP sink did not start")
                self.assertTrue(udp_tx_port, "host UDP sink has no port")
                udp_tx_rc = con.run(
                    "dd if=/dev/zero bs=1024 count=8 2>/dev/null "
                    f"| ttcp -u -t -l 1024 -n 8 -p {udp_tx_port[0]} "
                    f"{GATEWAY}", "udp_tx", 180)
                self.assertEqual(udp_tx_rc, 0, "guest UDP transmit failed")
                udp_tx_thread.join(30)
                udp_tx_timed_out = udp_tx_thread.is_alive()
                results["udp_tx"] = {
                    "payload_count": len(udp_tx_packets),
                    "payload_matches": udp_tx_packets == [bytes(1024)] * 8,
                    "control_count": len(udp_tx_controls),
                    "errors": udp_tx_errors,
                    "timed_out": udp_tx_timed_out,
                }

                # --- Guest-to-host TCP correctness ---
                # The guest writes a bounded nonuniform pattern using awk;
                # the host listener compares every received byte.
                tcp_tx_thread = threading.Thread(
                    target=_tcp_sink,
                    args=(tcp_tx_ready, tcp_tx_port, tcp_tx_chunks,
                          tcp_tx_errors, tcp_tx_stop), daemon=True)
                tcp_tx_thread.start()
                self.assertTrue(tcp_tx_ready.wait(30),
                                "host TCP sink did not start")
                self.assertTrue(tcp_tx_port, "host TCP sink has no port")
                tx_rc = con.run(
                    "awk 'BEGIN { for (i = 0; i < 65536; i++) "
                    "printf \"%c\", (i * 37 + 11) % 256 }' "
                    "> /tmp/tg-tx.bin; "
                    "dd if=/tmp/tg-tx.bin bs=8192 count=8 2>/dev/null "
                    f"| ttcp -t -l 8192 -n 8 -p {tcp_tx_port[0]} "
                    f"{GATEWAY}", "tcp_tx", 180)
                self.assertEqual(tx_rc, 0, "guest TCP transmit failed")
                tcp_tx_thread.join(30)
                tcp_tx_timed_out = tcp_tx_thread.is_alive()
                tx_data = b"".join(tcp_tx_chunks)
                expected_tx = bytes((i * 37 + 11) & 0xff
                                    for i in range(TCP_TX_BYTES))
                results["tcp_tx"] = {
                    "payload_bytes": len(tx_data),
                    "payload_matches": tx_data == expected_tx,
                    "errors": tcp_tx_errors,
                    "timed_out": tcp_tx_timed_out,
                }
            finally:
                udp_tx_stop.set()
                tcp_tx_stop.set()
                if udp_tx_thread is not None:
                    udp_tx_thread.join(1)
                if tcp_tx_thread is not None:
                    tcp_tx_thread.join(1)

            seg_after = self._netstat_counter(
                con, "tcp", "packets received", "sega")
            results["tcp_segments_delta"] = (
                (seg_after - seg_before)
                if (seg_after is not None and seg_before is not None)
                else None)

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
                '-netdev', ('user,id=tgnet'
                            ',hostfwd=tcp:127.0.0.1:0-%s:%d'
                            ',hostfwd=udp:127.0.0.1:0-%s:%d'
                            % (GUEST_ADDR, TCP_GUEST_PORT,
                               GUEST_ADDR, UDP_GUEST_PORT)),
                '-device',
                f'bcm570x,netdev=tgnet,addr={TG_ADDR},mac={TG_MAC}',
                '-object', f'filter-dump,id=tgcap,netdev=tgnet,file={pcap}')
            vm.launch()
            tcp_hp = _usernet_hostport(vm, 'tcp', TCP_GUEST_PORT)
            udp_hp = _usernet_hostport(vm, 'udp', UDP_GUEST_PORT)

            log_path = self.log_file('irix-tg.log')
            con = IrixConsole(vm.console_socket, log_path,
                              exited=lambda: vm.exitcode() is not None)
            login_reached = False
            tg1_present = False
            tg_up = False
            tg_flags = set()
            board = {}
            link_up = False
            link_line = None
            counters_before = (None, None, None)
            counters_after = (None, None, None)
            arp_cache_cold = False
            arp_neighbor = None
            results = {"tcp_verify": None, "tcp_mbps": None,
                       "udp_mbps": None, "udp_loss": None,
                       "udp_bytes": None, "udp_tx": None, "tcp_tx": None,
                       "tcp_segments_delta": None}
            try:
                try:
                    drive_irix_to_shell(con, LOGIN_BUDGET_SECONDS)
                    login_reached = True
                    tg1_present, tg_up, tg_flags = self._configure_tg(con)
                    board = self._read_board_configuration(con)
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
                    if tg_up:
                        self._throughput(con, tcp_hp, udp_hp, results)
                    con.send("sync; sync")
                    time.sleep(2)
                    con._pump()
                except HarnessError as exc:
                    # A harness timeout leaves whatever was scraped so far in
                    # place; the verdict below reports the milestone that
                    # stalled. Drain the console first so a guest panic dump
                    # is captured before the VM is torn down.
                    results["error"] = str(exc)
                    for _ in range(25):
                        try:
                            con._pump()
                        except HarnessError:
                            break
                        time.sleep(1)
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
            icmp_wire = icmp_ipv4_exchange(
                capture.get('frames', []) if isinstance(capture, dict) else [],
                GUEST_ADDR, GATEWAY)
            echo_wire = icmp_echo_exchange(
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
                "icmp_guest_to_gateway": icmp_wire["guest_to_gateway"],
                "icmp_gateway_to_guest": icmp_wire["gateway_to_guest"],
                "icmp_invalid_packets": icmp_wire["invalid_packets"],
                "icmp_echo_requests": echo_wire["requests"],
                "icmp_echo_replies": echo_wire["replies"],
                "icmp_echo_matching_replies": echo_wire["matching_replies"],
            }
            report = tg_acceptance_verdict(
                login_reached, tg1_present, tg_up, link_up, tg_flags,
                board, traffic)
            transport_report = tg_perf_verdict(login_reached, tg_up, results)
            report = {
                'verdict': ('PASS' if report['verdict'] == 'PASS' and
                            transport_report['verdict'] == 'PASS' else 'FAIL'),
                'problems': report['problems'] + transport_report['problems'],
            }

            summary_path = self.log_file('irix-tg-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({
                    'schema': 1,
                    'login_reached': login_reached,
                    'tg1_present': tg1_present,
                    'tg_up': tg_up,
                    'link_up': link_up,
                    'link_line': link_line,
                    'tg_flags': sorted(tg_flags),
                    'board': board,
                    'traffic': traffic,
                    'transport': results,
                    'report': report,
                }, summary_file, indent=2, sort_keys=True, default=str)
                summary_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX tg network problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path}, console log: {log_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
