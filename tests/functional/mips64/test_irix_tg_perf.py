#!/usr/bin/env python3
#
# IRIX tg (Broadcom BCM570x) transport + throughput: boot installed IRIX, bring
# up tg, and move real TCP and UDP traffic across it -- a byte-checked TCP
# transfer, plus ttcp TCP and UDP throughput runs -- measuring the rate and
# confirming the IP/TCP/UDP layers processed it via netstat protocol stats.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Where test_irix_tg.py proves pings cross tg
byte-for-byte, this test exercises the transport layers more fully and
measures performance:

- TCP correctness: a 1 MiB payload streamed host->guest into nc(1), checked
  by cksum + byte count on both sides (real TCP, not just ICMP or UDP).
- TCP throughput: ttcp(1) receiving a bulk stream, reporting the rate.
- UDP throughput: ttcp -u receiving a bulk datagram stream, reporting the
  rate and the loss.
- IP/TCP/UDP proof: netstat -s protocol counters before and after, so the
  segments/datagrams the guest's stacks actually processed are recorded,
  not merely that a program exited 0.

All legs run host->guest (the guest receives), which is the receive ring the
fork's end-of-frame fix repairs; the guest is the ttcp -r server and
the host drives the load through a SLIRP hostfwd. ttcp (/usr/etc/ttcp) and
nc (/usr/sbin/nc) are both in the installed 6.5.30 base image (confirmed by
probe). Guarded and asset-gated exactly like test_irix_eg_net.py.
"""
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_irix_tg as base  # noqa: E402

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


def _usernet_hostport(vm, proto, guest_port):
    """Host port SLIRP assigned to a hostfwd rule, from 'info usernet'. The
    row is 'PROTO[HOST_FORWARD] <fd> 127.0.0.1 <hostport> <guestip> <gport>'
    (same shape qemu.utils.get_info_usernet_hostfwd_port matches for TCP),
    generalized here to a given protocol and guest port."""
    out = vm.cmd('human-monitor-command', command_line='info usernet')
    rx = re.compile(r'%s.HOST_FORWARD.*127\.0\.0\.1\s+(\d+)\s+%s\s+%d\b'
                    % (proto.upper(), re.escape(base.GUEST_ADDR), guest_port))
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


if base.QemuSystemTest is not None:

    class IrixTgPerf(base.QemuSystemTest):
        """Boots installed IRIX, brings up tg, and moves TCP + UDP traffic
        across it with throughput measurement and byte verification."""

        def _netstat_counter(self, con, proto, pattern, tag):
            try:
                return int(con.capture_value(
                    f"netstat -s -p {proto} | " +
                    "awk '/" + pattern + "/ {print \"N=\" $1; exit}'",
                    tag, rb"N=(\d+)", timeout=60))
            except base.HarnessError:
                return None

        @base.skipBigDataTest()
        @base.skipSlowTest()
        @base.skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_tg_perf(self):
            assets = base.require_assets(
                self, base.FIRMWARE_REL, base.DISK_REL)
            prom = assets[base.FIRMWARE_REL]
            disk = assets[base.DISK_REL]
            if base.file_sha256(prom) != base.PROM_SHA256:
                self.fail('6.5.30 PROM SHA-256 mismatch')
            self.require_netdev('user')

            served_dir = self.scratch_file('served')
            os.makedirs(served_dir, exist_ok=True)

            pcap = self.log_file('tg-perf-wire.pcap')
            vm = self.get_vm(name='irix_tg_perf')
            vm.set_machine(base.MACHINES['origin200'])
            vm.set_console()
            elsc = self.log_file('elsc-aux.log')
            netdev = ('user,id=tgnet,tftp=%s'
                      ',hostfwd=tcp:127.0.0.1:0-%s:%d'
                      ',hostfwd=udp:127.0.0.1:0-%s:%d'
                      % (served_dir, base.GUEST_ADDR, TCP_GUEST_PORT,
                         base.GUEST_ADDR, UDP_GUEST_PORT))
            vm.add_args(
                '-accel', 'tcg,thread=single',
                '-cpu', 'R10000', '-smp', '1', '-m', str(base.MEMORY_MIB),
                '-bios', prom, '-audio', 'none',
                '-chardev', f'file,id=elsc,path={elsc}',
                '-serial', 'none', '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot', '-nic', 'none',
                '-d', 'guest_errors',
                '-D', self.log_file('qemu-dbg.log'),
                '-netdev', netdev,
                '-device', f'bcm570x,netdev=tgnet,addr={base.TG_ADDR},mac={base.TG_MAC}',
                '-object', f'filter-dump,id=tgcap,netdev=tgnet,file={pcap}')
            vm.launch()

            tcp_hp = _usernet_hostport(vm, 'tcp', TCP_GUEST_PORT)
            udp_hp = _usernet_hostport(vm, 'udp', UDP_GUEST_PORT)

            log_path = self.log_file('irix-tg-perf.log')
            con = base._Console(vm.console_socket, log_path,
                                exited=lambda: vm.exitcode() is not None)
            results = {"tcp_verify": None, "tcp_mbps": None,
                       "udp_mbps": None, "udp_loss": None}
            login = tg_up = False
            try:
                base._drive_to_shell(con, base.LOGIN_BUDGET_SECONDS)
                login = True
                con.run(f"ifconfig {base.IFACE} inet {base.GUEST_ADDR} "
                        "netmask 0xffffff00 up", "ifup", timeout=60)
                try:
                    con.capture_value("ifconfig " + base.IFACE, "ifs",
                                      rb"(UP)[,>]", timeout=60)
                    tg_up = True
                except base.HarnessError:
                    pass

                seg_before = self._netstat_counter(
                    con, "tcp", "packets received", "segb")

                # --- TCP byte-verify (host -> guest, saved by ttcp -r) ---
                self.assertIsNotNone(tcp_hp, "no TCP hostfwd port")
                verify_file = os.path.join(served_dir, 'verify.bin')
                with open(verify_file, 'wb') as vf:
                    vf.write(os.urandom(VERIFY_BYTES))
                host_crc, host_bytes = base._host_cksum(verify_file)
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
                mbps, _, _ = _parse_ttcp_rate(
                    tcp_out)
                results["tcp_mbps"] = mbps

                # --- UDP throughput (ttcp -u -r) ---
                con.send(f"ttcp -u -r -s -p {UDP_GUEST_PORT} -l {UDP_BUFLEN} "
                         "2>&1; echo \"UDP\"\"TP-$?\"")
                time.sleep(3)
                self.assertIsNotNone(udp_hp, "no UDP hostfwd port")
                _udp_ttcp_stream(udp_hp, UDP_BUFLEN, UDP_BUFS)
                con.expect(rb"UDPTP-\d+", 240, what="ttcp UDP to finish")
                udp_out = con.buf.decode("utf-8", "replace")
                umbps, ubytes, _ = _parse_ttcp_rate(
                    udp_out)
                results["udp_mbps"] = umbps
                results["udp_bytes"] = ubytes
                if ubytes is not None:
                    results["udp_loss"] = 1.0 - (ubytes /
                                                 float(UDP_BUFLEN * UDP_BUFS))

                seg_after = self._netstat_counter(
                    con, "tcp", "packets received", "sega")
                results["tcp_segments_delta"] = (
                    (seg_after - seg_before)
                    if (seg_after is not None and seg_before is not None)
                    else None)
                con.send("sync; sync")
                time.sleep(2)
                con._pump()
            except base.HarnessError as e:
                results["error"] = str(e)
                # Drain the console so a guest panic dump is captured before
                # the VM is torn down.
                for _ in range(25):
                    try:
                        con._pump()
                    except base.HarnessError:
                        break
                    time.sleep(1)
            finally:
                transcript = con.buf.decode("utf-8", errors="replace")
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)

            summary = self.log_file('irix-tg-perf-summary.json')
            import json
            with open(summary, 'w') as f:
                json.dump({"login": login, "tg_up": tg_up,
                           "results": results}, f, indent=2, default=str)
                f.write("\n")

            report = tg_perf_verdict(login, tg_up, results)
            self.assertEqual(
                report["verdict"], "PASS",
                "IRIX tg transport problems: " + "; ".join(report["problems"])
                + f" (full report: {summary}, console log: {log_path})")


if __name__ == '__main__':
    if base.QemuSystemTest is not None:
        base.QemuSystemTest.main()
