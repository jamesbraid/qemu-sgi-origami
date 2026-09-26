#!/usr/bin/env python3
#
# IRIX eg (Alteon Tigon) transport + throughput: boot installed IRIX, bring
# up eg, and move real TCP and UDP traffic across it -- a byte-checked TCP
# transfer, plus ttcp TCP and UDP throughput runs -- measuring the rate,
# confirming the IP/TCP/UDP layers processed it via netstat protocol stats,
# and confirming eg's own received-packet counter climbed over the transfer.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
This test exercises the transport layers eg carries and measures
performance:

- TCP correctness: a 1 MiB payload streamed host->guest into nc(1), checked
  by cksum + byte count on both sides (real TCP, not just ICMP or UDP).
- TCP throughput: ttcp(1) receiving a bulk stream, reporting the rate.
- UDP throughput: ttcp -u receiving a bulk datagram stream, reporting the
  rate and the loss.
- IP/TCP/UDP proof: netstat -s protocol counters before and after, so the
  segments/datagrams the guest's stacks actually processed are recorded,
  not merely that a program exited 0.
- Device-level receive proof: eg0's own Ipkts counter (netstat -in) read
  before and after the byte-checked TCP transfer, so the frames are shown
  to have crossed the adapter's own receive ring and not merely the
  guest's TCP stack. This is IRIX on purpose: IRIX's eg driver silently
  frees any received frame whose receive-return descriptor lacks the
  end-of-frame flag, incrementing no counter, which is exactly the "eg
  transmits but never receives, Ipkts stuck at zero" signature the fork's
  receive-return end-of-frame fix ("hw/net/tigon: mark receive-return
  descriptors end-of-frame") repairs. OpenBSD's ti(4) does not check that
  flag, so a green OpenBSD-over-eg transfer would prove nothing about this
  fix; only IRIX exercises the receive path the fix repairs. The register
  and ring contract, including the receive end-of-frame flag itself, is
  exercised separately by tests/qtest/sn0-tigon-test.c.

All legs run host->guest (the guest receives), which is the receive ring the
fork's end-of-frame fix repairs; the guest is the ttcp -r server and
the host drives the load through a SLIRP hostfwd. ttcp (/usr/etc/ttcp) and
nc (/usr/sbin/nc) are both in the installed 6.5.30 base image (confirmed by
probe).

How the boot differs from the OpenBSD login test: IRIX here boots the real
IP27 PROM (-bios), not a direct -kernel, so the drive first answers the
System Maintenance Menu with "1" (Start System) to boot the local SCSI
disk, then logs in as root (no password on the installed image), answers
the TERM prompt, and switches the login csh to /bin/sh so the rest of the
drive can use one Bourne command/response idiom. This is the same real PROM
and SLIRP wiring test_irix_miniroot.py uses, with an installed disk added
at SCSI id 1 and the maintenance-menu answer changed from the command
monitor to Start System.

The network back end is placed only on the eg (tigon) device, at a real
Origin 200 PCI option slot; the machine's on-board IOC3 Ethernet is left
with no back end (-nic none), so the only configured interface is eg0 and
the transfer can only have crossed eg.

eg_perf_verdict() below is the PASS/FAIL contract; its own docstring says
why each clause is scored the way it is.

Guarded by @skipBigDataTest() (the disk is multi-gigabyte) and
@skipSlowTest() (a real multiuser IRIX boot plus the transfer is minutes),
on top of @skipIfMissingEnv('ORIGAMI_ASSETS') (skips only when the env var
itself is unset) and a self.fail() at method entry if it is set but the
PROM or installed disk is missing -- a missing asset with the env var set
means CI provisioning failed, not a reason to skip quietly. The installed
IRIX disk is not committed (the fork ships no IRIX bits); a runner
provisions it at <ORIGAMI_ASSETS>/assets/irix/6.5.30/root.qcow2 the same
way the firmware and miniroot kernel are provisioned out of band.
"""
import os
import re
import socket
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest` run of this file finds the
# shared modules beside it.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import (HarnessError, IrixConsole,  # noqa: E402
                     drive_irix_to_shell)
from lib_prom import (MACHINES, file_sha256,  # noqa: E402
                      require_assets)

def mtu_probe_result(rc, output):
    """Classify one IRIX ifconfig MTU request from status and diagnostics."""
    error = next((line.strip() for line in output.splitlines()
                  if "mtu" in line.lower() and
                  ("sioc" in line.lower() or "ioctl" in line.lower())), None)
    if error:
        return f"refused ({error})"
    return "accepted" if rc == 0 else f"refused (rc {rc})"


def _check_copy(problems, label, copy):
    if not copy:
        problems.append(f"{label} has no byte-integrity evidence")
        return

    gc, gb = copy.get("guest_cksum"), copy.get("guest_bytes")
    hc, hb = copy.get("host_cksum"), copy.get("host_bytes")
    sent = copy.get("sent_bytes")
    if gc is None or gb is None:
        problems.append(f"{label} has no guest checksum and byte count")
        return
    if hb is None or hb <= 0:
        problems.append(f"{label} has no non-empty host byte count")
    if sent is None:
        problems.append(f"{label} has no sender byte count")
    elif hb is not None and sent != hb:
        problems.append(
            f"{label} sender wrote {sent} bytes of the {hb}-byte payload")
    if hb is not None and gb != hb:
        problems.append(
            f"{label} size mismatch: the guest received {gb} bytes, "
            f"the host sent {hb}")
    if hc is None:
        problems.append(f"{label} has no host checksum")
    elif gc != hc:
        problems.append(
            f"{label} checksum mismatch: the guest computed {gc}, "
            f"the host computed {hc}")


def eg_perf_verdict(login_reached, eg_up, results, rx=None):
    """Decide PASS/FAIL for one IRIX eg transport-and-throughput run.

    login_reached, eg_up: booleans for the two upstream milestones.

    results: the standard-mode dict the test records --
        {"tcp_verify": {"host_cksum": str, "host_bytes": int,
                        "guest_cksum": str or None,
                        "guest_bytes": int or None} or None,
         "tcp_mbps": float or None,
         "udp_mbps": float or None,
         "udp_loss": float or None,
         "udp_bytes": int or None,
         "tcp_segments_delta": int or None}

        Parallel mode instead carries mode="parallel" and a parallel dict
        with one single_copy, one copy per stream, both rates, and exact byte
        counts for each timed stream. Each checked copy records host, sender,
        and guest byte counts plus host/guest checksums.

    rx: {"ipkts_before": int or None, "ipkts_after": int or None} -- eg0's
        received-packet counter read either side of the byte-checked TCP
        transfer, or None when it was not collected (the parallel-mode leg
        returns before this point and carries no rx clause).

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff every
    clause holds; problems names every clause that did not, so one run reports
    all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not eg_up:
        problems.append("the eg interface never came up configured")

    if results.get("mode") == "parallel":
        parallel = results.get("parallel")
        if not parallel:
            problems.append("no parallel throughput was measured")
        else:
            streams = parallel.get("streams")
            copies = parallel.get("copies") or []
            if not isinstance(streams, int) or streams <= 0:
                problems.append("the parallel stream count is invalid")
            elif len(copies) != streams:
                problems.append(
                    f"parallel byte evidence covers {len(copies)} of "
                    f"{streams} streams")
            _check_copy(problems, "the single-stream baseline",
                        parallel.get("single_copy"))
            for index, copy in enumerate(copies):
                _check_copy(problems, f"parallel stream {index}", copy)
            if parallel.get("single_mbps") is None:
                problems.append("no single-stream baseline was measured")
            single_rate_bytes = parallel.get("single_rate_bytes")
            if not isinstance(single_rate_bytes, int) or single_rate_bytes <= 0:
                problems.append(
                    "the single-stream baseline byte count is invalid")
            if parallel.get("aggregate_mbps") is None:
                problems.append("no parallel aggregate rate was measured")
            rate_counts = parallel.get("stream_rate_bytes") or []
            if isinstance(streams, int) and streams > 0:
                if len(rate_counts) != streams:
                    problems.append(
                        f"parallel rate counts cover {len(rate_counts)} of "
                        f"{streams} streams")
                elif any(not isinstance(count, int) or count <= 0
                         for count in rate_counts):
                    problems.append("a parallel rate byte count is invalid")
        return {
            "verdict": "PASS" if not problems else "FAIL",
            "problems": problems,
        }

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

    if rx is not None:
        before = rx.get("ipkts_before")
        after = rx.get("ipkts_after")
        if after is None:
            problems.append(
                "the eg received-packet counter could not be read after "
                "the byte-checked TCP transfer")
        elif before is not None and after <= before:
            problems.append(
                f"the eg received-packet counter did not climb over the "
                f"byte-checked TCP transfer ({before} -> {after}): the "
                f"bytes did not arrive over eg's receive ring")
        elif before is None and after <= 0:
            problems.append(
                "the eg received no packets at all (Ipkts is zero after "
                "the byte-checked TCP transfer)")

    segs = results.get("tcp_segments_delta")
    if segs is None:
        problems.append(
            "the guest's TCP segment counter could not be read")
    elif segs <= 0:
        problems.append(
            f"the guest's TCP stack processed no segments ({segs}): the bytes "
            f"did not arrive as TCP through eg")

    if results.get("tcp_mbps") is None:
        problems.append("no TCP throughput was measured")

    udp_bytes = results.get("udp_bytes")
    if udp_bytes is None:
        problems.append("no UDP datagrams were measured")
    elif udp_bytes <= 0:
        problems.append(
            "the guest received no UDP payload at all: the datagram path "
            "carried nothing")

    # Transmit, which every other clause here leaves untested because they all
    # have the guest receiving. A transmit ring that stops releasing
    # descriptors drains after a handful of frames and then stalls with the
    # connection still established and no error anywhere -- so the check that
    # matters is not whether the guest thinks it sent, but whether the bytes
    # arrived. Comparing the two sides is what catches a stall.
    if "tx_bytes_host" in results:
        host_got = results.get("tx_bytes_host") or 0
        guest_sent = results.get("tx_bytes_guest")
        if host_got <= 0:
            problems.append(
                "the guest transmitted nothing that reached the host")
        elif guest_sent and host_got < guest_sent:
            problems.append(
                f"transmit stalled: the guest reported {guest_sent} bytes "
                f"sent but {host_got} arrived")
        if results.get("tx_mbps") is None:
            problems.append("no transmit throughput was measured")

    # Only present when a soak was asked for. Every round carries its own
    # digest and byte count, so a run that degrades partway through -- the
    # shape a ring index or a recycled buffer produces -- names the round it
    # went wrong on rather than just failing at the end.
    soak = results.get("soak")
    if soak:
        failed = soak.get("failed") or []
        if not soak.get("rounds"):
            problems.append("a soak was asked for but no round ran")
        elif failed:
            where = ", ".join(str(r.get("round")) for r in failed)
            problems.append(
                f"{len(failed)} of {len(soak['rounds'])} soak rounds did not "
                f"match byte for byte (round {where})")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"

# Where ORIGAMI_ASSETS resolves the real PROM and the installed IRIX disk.
# ORIGAMI_ASSETS names a CHECKOUT (one with its own assets/ directory), not
# assets/ itself, matching test_irix_miniroot.py's FIRMWARE_REL.
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root.qcow2')

# The release-matched PROM SHA-256, so a stale or wrong-release firmware is
# caught before a boot is spent on it. Same digest test_irix_miniroot.py and
# tools/sashlib.py's RELEASES["6.5.30"] carry.
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
)

# 128 MiB, the same size and reasoning test_openbsd_login.py uses: at or
# below 128 MiB the guest's whole memory bank fits the one slot the machine
# backs, so the filed >128MiB memory-map defects have no address at which to
# appear. A real multiuser IRIX boot needs the headroom over the 64 MiB the
# miniroot netboot uses.
MEMORY_MIB = 128

# SLIRP defaults: the gateway (and built-in TFTP server) and the guest's
# first-lease address, the values the eg bring-up captures used.
GATEWAY = "10.0.2.2"
GUEST_ADDR = "10.0.2.15"

# The eg (tigon) device sits at this Origin 200 PCI option slot; IRIX names
# the first Gigabit Ethernet adapter there eg0 (confirmed by hinv in the eg
# bring-up captures).
EG_ADDR = 4
IFACE = "eg0"

# The real multiuser IRIX boot budget the absorbed eg_net leg measured
# sufficient for a single-CPU boot: the PROM's own power-on diagnostics,
# then the kernel, then init to a console login.
_DEFAULT_LOGIN_BUDGET_SECONDS = 1200


def _host_cksum(path):
    """The POSIX cksum CRC and byte count of a file, computed by the host's
    own cksum(1) (coreutils' default algorithm is the POSIX CRC IRIX's
    cksum implements). Returns (crc, size) as (str, int)."""
    out = subprocess.run(["cksum", path], capture_output=True, text=True,
                         check=True).stdout
    crc, size = out.split()[0:2]
    return crc, int(size)


TCP_GUEST_PORT = 5001
UDP_GUEST_PORT = 5002
TCP_THROUGHPUT_BYTES = 4 * 1024 * 1024    # bulk TCP stream
SEND_TIMEOUT_SECONDS = 900                # a bulk send drains at guest speed
LEG_TIMEOUT_SECONDS = 900                 # and the guest reports after that
UDP_BUFLEN = 8192
UDP_BUFS = 512                            # 4 MiB of datagrams
UDP_PACE_EVERY = 16                       # datagrams between pauses
UDP_PACE_SECONDS = 0.02
VERIFY_BYTES = 1024 * 1024                # byte-checked TCP payload
TX_PORT = 5003                            # host listener the guest sends to
TX_BUFS = 64                              # 64 x 64 KiB = 4 MiB out of the guest

# Machine shape. The defaults are what the test asserts against in CI: one CPU
# on the round-robin scheduler, which is the cheapest thing that boots. Both
# are overridable so the same test can be pointed at a faster configuration
# when the question is how quickly the adapter can be driven rather than
# whether it is correct.
SMP = os.environ.get('EG_PERF_SMP', '1')
ACCEL = os.environ.get('EG_PERF_ACCEL', 'tcg,thread=single')

# Soak: repeat the checked transfer this many times after the measured legs.
# Off by default, because the point of the ordinary run is to be short enough
# to sit in the thorough suite. A soak is for asking whether the adapter still
# behaves after far more traffic than any single leg sends.
SOAK_ROUNDS = int(os.environ.get('EG_PERF_SOAK_ROUNDS', '0'))
SOAK_BYTES = int(os.environ.get('EG_PERF_SOAK_BYTES', str(4 * 1024 * 1024)))

# How long to wait for a root shell. The default is the one the eg transfer
# test proved sufficient for a single-CPU boot; a different machine shape, or
# a busy host, can want more, and a boot that is still making progress when
# the budget runs out reads as a device failure and is not one.
LOGIN_BUDGET_SECONDS = int(
    os.environ.get('EG_PERF_LOGIN_BUDGET', str(_DEFAULT_LOGIN_BUDGET_SECONDS)))

# Ask for jumbo frames before measuring. eg has no ifconfig MTU path -- the
# size is a kernel tunable, eg_mtu[] in /var/sysgen/master.d/if_eg, and it is
# read when the driver attaches, so it needs autoconfig and a reboot to take
# effect. SGI's own guidance is that 1500 and 9000 are the only two supported
# sizes. Off by default because it costs a second boot.
JUMBO = os.environ.get('EG_PERF_JUMBO', '') not in ('', '0')
JUMBO_MTU = 9000

# Socket buffer for the ttcp legs, ttcp's -b. Left at the guest's default unless
# asked for. Worth a knob because throughput on a path with latency is bounded
# by how much can be in flight unacknowledged, which is this and not the link:
# an emulated machine's round trip is long enough for that bound to bite well
# below the interface's own rate.
SOCKBUF = os.environ.get('EG_PERF_SOCKBUF', '')
SOCKBUF_ARG = (' -b ' + SOCKBUF) if SOCKBUF else ''

# Trace events to enable, e.g. tigon_rx_ring, written beside the console log.
TRACE = os.environ.get('EG_PERF_TRACE', '')

# Parallel receive streams. Tests whether aggregate throughput scales across
# processors or plateaus -- the plateau being the device holding the BQL across
# the long per-word DMA, which serialises the streams that were meant to scale.
# Off by default; set with EG_PERF_SMP>1 and EG_PERF_ACCEL=tcg,thread=multi.
PARALLEL = int(os.environ.get('EG_PERF_PARALLEL', '0'))

# Replace the network back end. The default user-mode stack runs TCP/IP in the
# emulator's main loop and never puts a frame larger than the standard size on
# the wire in either direction, so it cannot show what a larger frame is worth.
# A raw peer on the socket back end carries whatever the guest emits.
NETDEV = os.environ.get('EG_PERF_NETDEV', '')
# Address of that peer, and the guest address to match, when it is in use.
RAW_PEER_IP = '10.0.3.2'
RAW_GUEST_IP = '10.0.3.15'
# Datagram size and count for the blast. The size is set from the
# interface's frame size so one datagram is one frame: the whole
# question is what a frame costs, so fragmenting would hide it.
RAW_BLAST_LEN = int(os.environ.get('EG_PERF_RAW_LEN', '1472'))
RAW_BLAST_BUFS = int(os.environ.get('EG_PERF_RAW_BUFS', '20000'))

# Emulator log classes. 'unimp' is off by default because it is noisy, but it
# is the only way to see which registers the driver touches that this model
# does not implement -- a gate the guest programs and the model never reads
# breaks nothing and so goes unnoticed indefinitely.
DFLAGS = os.environ.get('EG_PERF_DFLAGS', 'guest_errors')


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


def _drain_server(port, ready, total):
    """Accept one connection and count what arrives, so the guest has
    somewhere to transmit to. The guest reaches the host at the SLIRP gateway
    address, so a listener here is a listener it can open a connection to."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(1)
    srv.settimeout(SEND_TIMEOUT_SECONDS)
    ready.set()
    try:
        conn, _ = srv.accept()
    except OSError:
        srv.close()
        return
    conn.settimeout(SEND_TIMEOUT_SECONDS)
    n = 0
    try:
        while True:
            b = conn.recv(65536)
            if not b:
                break
            n += len(b)
    except OSError:
        pass
    total.append(n)
    conn.close()
    srv.close()


def _forget_console_history(con):
    """Drop what expect() is allowed to match, keeping the console log.

    expect() searches the whole accumulated buffer from its start, so after a
    reboot every prompt the first boot printed is still there to be matched.
    A drive that expects a menu, a login prompt and a shell prompt in sequence
    then satisfies all three from history and returns in milliseconds, having
    moved nothing. The log file is written as bytes arrive and keeps the lot.
    """
    del con.buf[:]


def _parallel_send(host_ports, payloads):
    """Drive N concurrent TCP senders, one per host port, and return the
    aggregate Mbit/s and exact sent counts. Near one stream's rate means the
    streams serialised; near N times means they scaled."""
    import threading

    if len(host_ports) != len(payloads):
        raise ValueError("each parallel host port needs one payload")
    sockets = []
    try:
        for port in host_ports:
            sock = socket.create_connection(("127.0.0.1", port), timeout=30)
            sock.settimeout(SEND_TIMEOUT_SECONDS)
            sockets.append(sock)
    except OSError as error:
        for sock in sockets:
            sock.close()
        raise HarnessError(f"parallel sender failed: {error}") from error

    start = threading.Event()
    done = [0] * len(host_ports)
    errors = [None] * len(host_ports)

    def one(i, sock, payload):
        start.wait()
        try:
            try:
                try:
                    sock.sendall(payload)
                finally:
                    sock.shutdown(socket.SHUT_WR)
                    sock.settimeout(30)
                    try:
                        while sock.recv(65536):
                            pass
                    except OSError:
                        pass
                    sock.close()
                done[i] = len(payload)
            except OSError as error:
                errors[i] = str(error)
        # pylint: disable-next=broad-exception-caught
        except Exception as error:  # make worker failures visible to the test
            errors[i] = str(error)

    threads = [threading.Thread(target=one, args=(i, sockets[i], payloads[i]))
               for i in range(len(host_ports))]
    for t in threads:
        t.start()
    t0 = time.monotonic()
    start.set()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t0
    if any(errors):
        raise HarnessError("parallel sender failed: " +
                            "; ".join(e for e in errors if e))
    total = sum(done)
    rate = (total * 8 / elapsed / 1e6) if elapsed > 0 else 0.0
    return rate, done


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


if QemuSystemTest is not None:

    class IrixEg(QemuSystemTest):
        """Boots installed IRIX, brings up eg, and moves TCP + UDP traffic
        across it with throughput measurement, byte verification, and a
        device-level receive-counter proof."""

        def _netstat_counter(self, con, proto, pattern, tag):
            try:
                return int(con.capture_value(
                    f"netstat -s -p {proto} | " +
                    "awk '/" + pattern + "/ {print \"N=\" $1; exit}'",
                    tag, rb"N=(\d+)", timeout=60))
            except HarnessError:
                return None

        def _read_ipkts(self, con, tag):
            """eg0's received-packet counter from netstat -i, or None if it
            could not be read. netstat -in columns are
            Name Mtu Network Address Ipkts ... so Ipkts is field 5."""
            try:
                return int(con.capture_value(
                    "netstat -in | awk '$1 ~ /^" + IFACE +
                    "/ {print \"IPKTS=\" $5; exit}'",
                    tag, rb"IPKTS=(\d+)", timeout=60))
            except HarnessError:
                return None

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_eg(self):
            assets = require_assets(
                self, FIRMWARE_REL, DISK_REL)
            prom = assets[FIRMWARE_REL]
            disk = assets[DISK_REL]
            if file_sha256(prom) != PROM_SHA256:
                self.fail('6.5.30 PROM SHA-256 mismatch')
            self.require_netdev('user')

            served_dir = self.scratch_file('served')
            os.makedirs(served_dir, exist_ok=True)

            pcap = self.log_file('eg-perf-wire.pcap')
            vm = self.get_vm(name='irix_eg')
            vm.set_machine(MACHINES['origin200'])
            vm.set_console()
            elsc = self.log_file('elsc-aux.log')
            if NETDEV:
                netdev = NETDEV
            else:
                netdev = ('user,id=egnet,tftp=%s'
                          ',hostfwd=tcp:127.0.0.1:0-%s:%d'
                          ',hostfwd=udp:127.0.0.1:0-%s:%d'
                          % (served_dir, GUEST_ADDR, TCP_GUEST_PORT,
                             GUEST_ADDR, UDP_GUEST_PORT))
                for _pi in range(PARALLEL):
                    netdev += (',hostfwd=tcp:127.0.0.1:0-%s:%d'
                               % (GUEST_ADDR, TCP_GUEST_PORT + 1 + _pi))
            vm.add_args(
                '-accel', ACCEL,
                '-cpu', 'R10000', '-smp', SMP, '-m', str(MEMORY_MIB),
                '-bios', prom, '-audio', 'none',
                '-chardev', f'file,id=elsc,path={elsc}',
                '-serial', 'none', '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot', '-nic', 'none',
                '-d', DFLAGS,
                '-D', self.log_file('qemu-dbg.log'),
                '-netdev', netdev,
                '-device', f'tigon,netdev=egnet,addr={EG_ADDR}',
                '-object', f'filter-dump,id=egcap,netdev=egnet,file={pcap}')
            if TRACE:
                # Which receive ring carried each frame, and how the interrupt
                # line moves, are invisible from the guest and from a rate: a
                # large frame still arrives through the standard ring when the
                # driver posted nothing to the jumbo one.
                #
                # One -trace per event. The option's own parameter separator is
                # a comma, so a comma-joined list makes the second name look
                # like a new parameter and the emulator refuses to start --
                # which reads as a launch failure, not as a bad argument.
                events = [e for e in TRACE.split(',') if e]
                for event in events:
                    vm.add_args('-trace', 'enable=' + event)
                vm.add_args('-trace',
                            'file=' + self.log_file('tigon-trace.log'))
            vm.launch()

            tcp_hp = _usernet_hostport(vm, 'tcp', TCP_GUEST_PORT)
            udp_hp = _usernet_hostport(vm, 'udp', UDP_GUEST_PORT)

            log_path = self.log_file('irix-eg-perf.log')
            con = IrixConsole(vm.console_socket, log_path,
                              exited=lambda: vm.exitcode() is not None)
            results = {"mode": "parallel" if PARALLEL else "standard",
                       "topology": {"smp": SMP, "accel": ACCEL},
                       "tcp_verify": None, "tcp_mbps": None,
                       "udp_mbps": None, "udp_loss": None}
            login = eg_up = False
            ipkts_before = None
            ipkts_after = None
            try:
                drive_irix_to_shell(con, LOGIN_BUDGET_SECONDS)
                login = True

                if JUMBO:
                    # The documented way to raise eg's frame size. The driver
                    # reads eg_mtu[] out of its master.d file when it attaches,
                    # so the value has to be in place before an attach, which
                    # means autoconfig and a reboot. This adapter has no
                    # ifconfig MTU path at all -- that one belongs to tg.
                    con.run("sed -e 's/= { *1500/= { %d/' "
                            "/var/sysgen/master.d/if_eg > /tmp/if_eg.new && "
                            "cp /tmp/if_eg.new /var/sysgen/master.d/if_eg"
                            % JUMBO_MTU, "jset", timeout=120)
                    # Tag the count rather than reading a bare number: the
                    # console echoes the command, so a pattern matching the
                    # size would match the echo of the command that looked
                    # for it and report success whatever the file says.
                    results["jumbo_tunable"] = con.capture_value(
                        "grep -c %d /var/sysgen/master.d/if_eg | sed 's/^/JC=/'"
                        % JUMBO_MTU, "jchk", rb"JC=(\d+)", timeout=60)
                    con.run("autoconfig -f", "jauto", timeout=900)
                    con.send("reboot")
                    # The shell keeps echoing for seconds after reboot is
                    # entered, so wait for the shutdown itself before
                    # believing anything. This string cannot come from the
                    # first boot -- nothing has shut down yet.
                    con.expect(rb"THE SYSTEM IS BEING SHUT DOWN", 300,
                               what="the shutdown to start")
                    _forget_console_history(con)
                    con.expect(rb"IP27 PROM SGI Version", 600,
                               what="the PROM banner after reboot")
                    # Not the same drive as the first boot: a reboot comes up
                    # without stopping at the maintenance menu, so waiting for
                    # that menu here waits for something that never prints.
                    con.expect(rb"console login:", LOGIN_BUDGET_SECONDS,
                               what="the login prompt after reboot")
                    con.send("root")
                    # TERM is asked once per console login on some paths and
                    # not others; treat it as optional rather than assuming.
                    try:
                        con.expect(rb"TERM\s*=\s*\(", 120,
                                   what="the TERM prompt")
                        con.send("")
                    except HarnessError:
                        pass
                    con.expect(rb"[0-9]# ", 300,
                               what="the login csh prompt after reboot")
                    con.send("exec /bin/sh")
                    con.run("PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:"
                            "/usr/bsd:$PATH; export PATH", "shinit2",
                            timeout=120)

                con.run("egconfig -f 1 " + IFACE, "egc", timeout=180)
                if NETDEV:
                    # A raw peer answers ARP and counts what arrives; it speaks
                    # no TCP, so this measures one thing only -- how fast the
                    # guest can put frames on the wire, at whatever size it is
                    # configured for. That is the quantity the per-packet cost
                    # bounds, measured without the user-mode stack in the way.
                    con.run("ifconfig %s inet %s netmask 0xffffff00 up"
                            % (IFACE, RAW_GUEST_IP), "rawup", timeout=60)
                    results["raw_ping"] = con.capture_value(
                        "ping -c 3 %s | awk '/packets received/ "
                        "{print \"RX=\" $4}'" % RAW_PEER_IP,
                        "rawping", rb"RX=(\d+)", timeout=120)
                    # -s makes ttcp generate its own data; without it the
                    # transmit side reads standard input and simply waits.
                    con.run("ttcp -u -t -s -l %d -n %d %s > /dev/null 2>&1"
                            % (RAW_BLAST_LEN, RAW_BLAST_BUFS, RAW_PEER_IP),
                            "rawblast", timeout=900)
                    results["raw_done"] = True
                    return
                con.run(f"ifconfig {IFACE} inet {GUEST_ADDR} "
                        "netmask 0xffffff00 up", "ifup", timeout=60)
                try:
                    con.capture_value("ifconfig " + IFACE, "ifs",
                                      rb"(UP)[,>]", timeout=60)
                    eg_up = True
                except HarnessError:
                    pass

                if PARALLEL:
                    # One stream first for the per-stream baseline, then N
                    # concurrent. If the aggregate is near the single, the
                    # streams serialised (the BQL-held-per-word-DMA plateau);
                    # near N times, they scaled.
                    if eg_up:
                        ports = [
                            _usernet_hostport(
                                vm, 'tcp', TCP_GUEST_PORT + 1 + i)
                            for i in range(PARALLEL)]
                        self.assertNotIn(None, ports,
                                         "missing parallel hostfwd")

                        payloads = []
                        copies = []
                        for i in range(PARALLEL + 1):
                            path = os.path.join(
                                served_dir, f'parallel-{i}.bin')
                            with open(path, 'wb') as payload_file:
                                payload_file.write(
                                    os.urandom(TCP_THROUGHPUT_BYTES))
                            host_crc, host_bytes = _host_cksum(path)
                            with open(path, 'rb') as payload_file:
                                payloads.append(payload_file.read())
                            copies.append({"host_cksum": host_crc,
                                           "host_bytes": host_bytes})

                        # Keep the timed legs on ttcp's sink path so they are
                        # comparable to the retained measurements. Run the
                        # byte-checked copies separately below: writing each
                        # timed stream to the guest disk measures that disk as
                        # well as the adapter.
                        con.send(
                            "ttcp -r -s -p %d -l 65536 "
                            ">/dev/null 2>&1 &" % (TCP_GUEST_PORT + 1))
                        time.sleep(3)
                        single, single_rate_sent = _parallel_send(
                            ports[:1], payloads[:1])
                        con.run("wait", "parsinglerate",
                                timeout=LEG_TIMEOUT_SECONDS)

                        for i in range(PARALLEL):
                            con.send(
                                "ttcp -r -s -p %d -l 65536 "
                                ">/dev/null 2>&1 &" %
                                (TCP_GUEST_PORT + 1 + i))
                        time.sleep(3)
                        aggregate, stream_rate_sent = _parallel_send(
                            ports, payloads[1:])
                        con.run("wait", "paraggregaterate",
                                timeout=LEG_TIMEOUT_SECONDS)

                        con.send(
                            "ttcp -r -p %d -l 65536 "
                            ">/tmp/eg-par-single.bin 2>/dev/null &" %
                            (TCP_GUEST_PORT + 1))
                        time.sleep(3)
                        _, single_copy_sent = _parallel_send(
                            ports[:1], payloads[:1])
                        con.run("wait", "parsinglecopy",
                                timeout=LEG_TIMEOUT_SECONDS)
                        gcrc, gbytes = con.two_values(
                            "dd if=/tmp/eg-par-single.bin bs=8192 count=%d "
                            "2>/dev/null | cksum | sed 's/^/CK=/'" %
                            (TCP_THROUGHPUT_BYTES // 8192),
                            "parsingleck",
                            rb"CK=(\d+)\s+(\d+)",
                            timeout=120)
                        single_copy = dict(copies[0])
                        single_copy.update({"sent_bytes": single_copy_sent[0],
                                            "guest_cksum": gcrc,
                                            "guest_bytes": gbytes})

                        for i in range(PARALLEL):
                            con.send(
                                "ttcp -r -p %d -l 65536 "
                                ">/tmp/eg-par-%d.bin 2>/dev/null &" %
                                (TCP_GUEST_PORT + 1 + i, i))
                        time.sleep(3)
                        _, copy_sent = _parallel_send(
                            ports, payloads[1:])
                        con.run("wait", "paraggregatecopy",
                                timeout=LEG_TIMEOUT_SECONDS)
                        stream_copies = []
                        for i in range(PARALLEL):
                            gcrc, gbytes = con.two_values(
                                "dd if=/tmp/eg-par-%d.bin bs=8192 count=%d "
                                "2>/dev/null | cksum | sed 's/^/CK=/'" %
                                (i, TCP_THROUGHPUT_BYTES // 8192),
                                "parck%d" % i,
                                rb"CK=(\d+)\s+(\d+)",
                                timeout=120)
                            copy = dict(copies[i + 1])
                            copy.update({"sent_bytes": copy_sent[i],
                                         "guest_cksum": gcrc,
                                         "guest_bytes": gbytes})
                            stream_copies.append(copy)

                        results["parallel"] = {
                            "streams": PARALLEL,
                            "single_mbps": round(single, 1),
                            "single_rate_bytes": single_rate_sent[0],
                            "aggregate_mbps": round(aggregate, 1),
                            "stream_rate_bytes": stream_rate_sent,
                            "scaling": (round(aggregate / single, 2)
                                        if single else 0),
                            "single_copy": single_copy,
                            "copies": stream_copies,
                            "load": [round(os.getloadavg()[0], 1)],
                        }
                    else:
                        results["parallel"] = None
                    # Persist the parallel result here: this leg returns early,
                    # before the standard transport legs, so score and write it
                    # here rather than bypassing the verdict.
                    import json as _json
                    _sum = self.log_file('irix-eg-perf-summary.json')
                    _report = eg_perf_verdict(login, eg_up, results)
                    with open(_sum, 'w', encoding='utf-8') as _f:
                        _json.dump({"login": login, "eg_up": eg_up,
                                    "results": results, "report": _report},
                                   _f, indent=2,
                                   default=str)
                        _f.write("\n")
                    self.assertEqual(
                        _report["verdict"], "PASS",
                        "IRIX eg parallel transport problems: " +
                        "; ".join(_report["problems"]) +
                        f" (full report: {_sum}, console log: {log_path})")
                    return

                seg_before = self._netstat_counter(
                    con, "tcp", "packets received", "segb")

                # --- TCP byte-verify (host -> guest, saved by ttcp -r) ---
                # Also the leg the Ipkts clause attaches to: eg0's own
                # received-packet counter must climb across this transfer,
                # proving the bytes crossed the adapter's receive ring and
                # not merely the guest's TCP stack (the now-absorbed
                # eg_net leg proved the same thing over UDP/TFTP).
                self.assertIsNotNone(tcp_hp, "no TCP hostfwd port")
                verify_file = os.path.join(served_dir, 'verify.bin')
                with open(verify_file, 'wb') as vf:
                    vf.write(os.urandom(VERIFY_BYTES))
                host_crc, host_bytes = _host_cksum(verify_file)
                with open(verify_file, 'rb') as vf:
                    verify_bytes = vf.read()
                ipkts_before = self._read_ipkts(con, "ipktsbefore")
                # ttcp -r without -s writes the received stream to stdout.
                con.send(f"ttcp -r -p {TCP_GUEST_PORT} -l 65536 "
                         ">/tmp/rx.bin 2>/dev/null; echo \"RX\"\"DONE\"")
                time.sleep(3)
                _tcp_stream(tcp_hp, verify_bytes)
                con.expect(rb"RXDONE", 300, what="ttcp receive to finish")
                ipkts_after = self._read_ipkts(con, "ipktsafter")
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
                con.send(f"ttcp -r -s -p {TCP_GUEST_PORT} -l 65536{SOCKBUF_ARG} 2>&1; "
                         "echo \"TCP\"\"TP-$?\"")
                time.sleep(3)
                _load_tcp = os.getloadavg()[0]
                _tcp_stream(tcp_hp, os.urandom(TCP_THROUGHPUT_BYTES))
                con.expect(rb"TCPTP-\d+", LEG_TIMEOUT_SECONDS,
                           what="ttcp TCP to finish")
                tcp_out = con.buf.decode("utf-8", "replace")
                mbps, _, _ = _parse_ttcp_rate(
                    tcp_out)
                results["tcp_mbps"] = mbps
                # This host is shared, and a rate taken on a busy host is not
                # comparable to one taken on an idle host. Record what the
                # machine was doing so a number can be discarded rather than
                # believed. Sampled either side: the leg itself moves it.
                results["tcp_load"] = [round(_load_tcp, 1),
                                       round(os.getloadavg()[0], 1)]
                # The GUEST's own load, which the host profile cannot show: an
                # emulated processor with nothing to do still burns a full host
                # thread in its idle loop, so host-side samples say nothing
                # about whether the guest was actually saturated. If this comes
                # back near zero while a transfer is running, the guest is
                # waiting, not working, and the limit is not its processor.
                try:
                    results["guest_load"] = con.capture_value(
                        "uptime | sed 's/.*average.//' | "
                        "awk -F, '{print \"GL=\" $1}'",
                        "gload", rb"GL=\s*([0-9.]+)", timeout=60)
                except HarnessError:
                    results["guest_load"] = None
                results["tcp_write_size"] = 65536
                results["mtu"] = JUMBO_MTU if JUMBO else 1500
                results["build"] = os.environ.get("QEMU_TEST_QEMU_BINARY", "")

                # --- UDP throughput (ttcp -u -r) ---
                con.send(f"ttcp -u -r -s -p {UDP_GUEST_PORT} -l {UDP_BUFLEN}{SOCKBUF_ARG} "
                         "2>&1; echo \"UDP\"\"TP-$?\"")
                time.sleep(3)
                self.assertIsNotNone(udp_hp, "no UDP hostfwd port")
                _udp_ttcp_stream(udp_hp, UDP_BUFLEN, UDP_BUFS)
                con.expect(rb"UDPTP-\d+", LEG_TIMEOUT_SECONDS,
                           what="ttcp UDP to finish")
                udp_out = con.buf.decode("utf-8", "replace")
                umbps, ubytes, _ = _parse_ttcp_rate(
                    udp_out)
                results["udp_mbps"] = umbps
                results["udp_bytes"] = ubytes
                results["udp_datagrams_sent"] = UDP_BUFS
                results["udp_datagram_bytes"] = UDP_BUFLEN
                results["udp_bytes_sent"] = UDP_BUFLEN * UDP_BUFS
                if ubytes is not None:
                    results["udp_loss"] = 1.0 - (ubytes /
                                                 float(UDP_BUFLEN * UDP_BUFS))

                # --- transmit: the guest as sender ---
                #
                # Every other leg here has the guest receiving, which leaves
                # the whole transmit direction untested -- and a transmit ring
                # that stops releasing descriptors drains after a dozen frames
                # and then stalls with the connection still established, which
                # nothing on the receive side would ever notice. The guest
                # reaches the host at the SLIRP gateway, so a listener here is
                # somewhere it can send to.
                import threading
                ready = threading.Event()
                got = []
                srv = threading.Thread(target=_drain_server,
                                       args=(TX_PORT, ready, got), daemon=True)
                srv.start()
                ready.wait(30)
                con.send(f"ttcp -t -s -l 65536 -n {TX_BUFS} -p {TX_PORT} "
                         f"{GATEWAY} 2>&1; echo \"TX\"\"TP-$?\"")
                con.expect(rb"TXTP-\d+", LEG_TIMEOUT_SECONDS,
                           what="ttcp transmit to finish")
                tx_out = con.buf.decode("utf-8", "replace")
                tmbps, tbytes, _ = _parse_ttcp_rate(tx_out)
                srv.join(30)
                results["tx_mbps"] = tmbps
                results["tx_bytes_guest"] = tbytes
                results["tx_bytes_host"] = got[0] if got else 0

                # --- soak: the same checked transfer, over and over ---
                #
                # One 4 MiB transfer is already about 2800 frames, which laps
                # the default 2048-entry return ring once. Repeating it is what
                # turns a fault that needs a lap into one that shows up
                # reliably, and it is the only leg here that would notice a
                # ring index or a buffer that goes wrong slowly.
                if SOAK_ROUNDS:
                    rounds = []
                    for r in range(SOAK_ROUNDS):
                        soak_file = os.path.join(served_dir, 'soak.bin')
                        with open(soak_file, 'wb') as sf:
                            sf.write(os.urandom(SOAK_BYTES))
                        s_crc, s_bytes = _host_cksum(soak_file)
                        with open(soak_file, 'rb') as sf:
                            soak_payload = sf.read()
                        con.send(f"ttcp -r -p {TCP_GUEST_PORT} -l 65536 "
                                 ">/tmp/soak.bin 2>/dev/null; "
                                 "echo \"SK\"\"-$?\"")
                        time.sleep(3)
                        _tcp_stream(tcp_hp, soak_payload)
                        con.expect(rb"SK-\d+", 600,
                                   what=f"soak round {r + 1} to finish")
                        gc2, gb2 = con.two_values(
                            "dd if=/tmp/soak.bin bs=8192 count=%d 2>/dev/null "
                            "| cksum | sed 's/^/CK=/'" % (SOAK_BYTES // 8192),
                            f"soak{r}", rb"CK=(\d+)\s+(\d+)", timeout=300)
                        rounds.append({"round": r + 1,
                                       "host_cksum": s_crc,
                                       "guest_cksum": gc2,
                                       "host_bytes": s_bytes,
                                       "guest_bytes": gb2,
                                       "ok": (gc2 == s_crc and gb2 == s_bytes)})
                    # A one-minute average means something here: the rounds
                    # above ran back to back for a minute or more. The same
                    # reading after a single sub-second leg is mostly the quiet
                    # minute that preceded it, which is why the earlier sample
                    # cannot settle whether the guest is saturated.
                    try:
                        results["guest_load_after_soak"] = con.capture_value(
                            "uptime | sed 's/.*average.//' | "
                            "awk -F, '{print \"GL=\" $1}'",
                            "gload2", rb"GL=\s*([0-9.]+)", timeout=60)
                    except HarnessError:
                        results["guest_load_after_soak"] = None
                    results["soak"] = {
                        "rounds": rounds,
                        "bytes_total": SOAK_ROUNDS * SOAK_BYTES,
                        "failed": [r for r in rounds if not r["ok"]],
                    }

                seg_after = self._netstat_counter(
                    con, "tcp", "packets received", "sega")
                results["tcp_segments_delta"] = (
                    (seg_after - seg_before)
                    if (seg_after is not None and seg_before is not None)
                    else None)
                # Whether the driver will take a larger frame at all. Fewer,
                # bigger packets is the one lever that cuts the guest's
                # per-packet cost without making the emulated CPU faster, so
                # whether it is available decides what rate is reachable here
                # at all. Recorded rather than asserted: a driver that refuses
                # is a fact about the guest, not a failure of the adapter.
                mtu = {}
                for want in (9000, 4096, 2048):
                    try:
                        output_start = len(con.buf)
                        rc = con.run(f"ifconfig {IFACE} mtu {want}",
                                     f"mtu{want}", timeout=60)
                        output = bytes(con.buf[output_start:]).decode(
                            "utf-8", "replace")
                        mtu[want] = mtu_probe_result(rc, output)
                    except HarnessError:
                        mtu[want] = "refused"
                try:
                    mtu["reported"] = con.capture_value(
                        "netstat -in | awk '$1 ~ /^%s/ {print \"M=\" $2; exit}'"
                        % IFACE, "mtushow", rb"M=(\d+)", timeout=60)
                except HarnessError:
                    mtu["reported"] = None
                results["mtu"] = mtu

                con.send("sync; sync")
                time.sleep(2)
                con._pump()
            except HarnessError as e:
                results["error"] = str(e)
                # Drain the console so a guest panic dump is captured before
                # the VM is torn down.
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

            summary = self.log_file('irix-eg-perf-summary.json')
            import json
            with open(summary, 'w', encoding='utf-8') as f:
                json.dump({"login": login, "eg_up": eg_up,
                           "results": results}, f, indent=2, default=str)
                f.write("\n")

            rx = {"ipkts_before": ipkts_before, "ipkts_after": ipkts_after}
            report = eg_perf_verdict(login, eg_up, results, rx)
            self.assertEqual(
                report["verdict"], "PASS",
                "IRIX eg transport problems: " + "; ".join(report["problems"])
                + f" (full report: {summary}, console log: {log_path})")


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
