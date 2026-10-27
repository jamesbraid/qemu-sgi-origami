#!/usr/bin/env python3
#
# IRIX tg (Broadcom BCM570x) transport + throughput: boot installed IRIX, bring
# up tg, and move real TCP and UDP traffic across it -- a byte-checked TCP
# transfer, plus ttcp TCP and UDP throughput runs -- measuring the rate and
# confirming the IP/TCP/UDP layers processed it via netstat protocol stats.
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Where test_irix_tg.py proves pings cross tg
byte-for-byte, this test exercises the transport layers more fully and
measures performance:

- TCP correctness: a 1 MiB payload streamed host->guest into nc(1), checked
  by cksum + byte count on both sides (real TCP, not just ICMP or UDP).
- TCP throughput: ttcp(1) receiving a bulk stream, reporting the rate.
- UDP throughput: ttcp -u receiving a bulk datagram stream, reporting the
  rate and the loss.
- UDP transmit: eight exact payload datagrams received by a host socket.
- IP/TCP/UDP proof: netstat -s protocol counters before and after, so the
  segments/datagrams the guest's stacks actually processed are recorded,
  not merely that a program exited 0.

The throughput legs run host->guest through SLIRP hostfwd. The guest sends
the UDP transmit check to a host socket through the SLIRP gateway. ttcp and
nc (/usr/sbin/nc) are both in the installed 6.5.30 base image (confirmed by
probe). Guarded and asset-gated exactly like test_irix_eg_net.py.
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_irix_tg as base  # noqa: E402
from lib_prom import launch_paused  # noqa: E402

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


def _usernet_hostport(vm, proto, guest_port, guest_addr=base.GUEST_ADDR):
    """Host port SLIRP assigned to a hostfwd rule, from 'info usernet'. The
    row is 'PROTO[HOST_FORWARD] <fd> 127.0.0.1 <hostport> <guestip> <gport>'
    (same shape qemu.utils.get_info_usernet_hostfwd_port matches for TCP),
    generalized here to a given protocol and guest port."""
    out = vm.cmd('human-monitor-command', command_line='info usernet')
    rx = re.compile(r'%s.HOST_FORWARD.*127\.0\.0\.1\s+(\d+)\s+%s\s+%d\b'
                    % (proto.upper(), re.escape(guest_addr), guest_port))
    for line in re.split(r'\r?\n', out):
        m = rx.search(line)
        if m:
            return int(m.group(1))
    return None


def _tcp_stream(host_port, data, after_connect=None):
    """Connect to the forwarded TCP port, send data, close. Returns the
    host-side elapsed seconds. after_connect runs after the hostfwd accepts
    the socket and before any payload is sent."""
    s = socket.create_connection(("127.0.0.1", host_port), timeout=30)
    # The guest drains at emulated speed, so the send blocks for as long as the
    # transfer takes; the connect timeout above would cut a bulk one short.
    s.settimeout(SEND_TIMEOUT_SECONDS)
    try:
        if after_connect is not None:
            after_connect()
        t0 = time.monotonic()
        s.sendall(data)
    finally:
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
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
    """base._host_cksum is only reached once a live run gets as far as the
    TCP byte-verify step, so a run without QEMU_TEST_SGI_ASSET_DIR skips
    straight past a regression here (test_irix_tg.py never defined it, unlike
    test_irix_eg_net.py, which test_irix_eg_perf.py's own copy of this
    call relies on). No boot needed to catch it."""

    def test_host_cksum_matches_cksum1(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"origami tg_perf host_cksum regression\n")
            f.flush()
            crc, size = base._host_cksum(f.name)
            want = subprocess.run(["cksum", f.name], capture_output=True,
                                   text=True, check=True).stdout.split()
        self.assertEqual((crc, size), (want[0], int(want[1])))


if base.QemuSystemTest is not None:
    from qemu_test.asset import LocalAsset

    class IrixTgPerf(base.QemuSystemTest):
        """Boots installed IRIX, brings up tg, and moves TCP + UDP traffic
        across it with throughput measurement and byte verification."""

        ASSET_FIRMWARE = LocalAsset(base.FIRMWARE_REL, base.PROM_SHA256)
        ASSET_DISK = LocalAsset(
            base.DISK_REL,
            '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12')

        VM_NAME = 'irix_tg_perf'
        PROM_SHA256 = base.PROM_SHA256
        TG_SLOT = base.TG_ADDR
        GUEST_ADDR = base.GUEST_ADDR
        GATEWAY = base.GATEWAY
        USERNET_OPTIONS = ''
        BACKGROUND_TCP_RECEIVER = False
        TCP_RECEIVER_SCRIPT = '/tmp/qemu-tg-tcp-rx.sh'
        TCP_RECEIVER_PID = '/tmp/qemu-tg-tcp-rx.pid'
        TCP_RECEIVER_STATUS = '/tmp/qemu-tg-tcp-rx.status'
        TCP_RECEIVER_READY_ATTEMPTS = 30

        def _configure_machine(self, vm, prom, disk, elsc):
            """Configure the Origin 200 and its installed-IRIX drives."""
            vm.set_machine(base.MACHINES['origin200'])
            vm.set_console()
            vm.add_args(
                '-accel', base.IRIX_ACCEL,
                '-cpu', 'R10000', '-smp', base.IRIX_SMP,
                '-m', str(base.MEMORY_MIB),
                '-bios', prom, '-audio', 'none',
                '-chardev', f'file,id=elsc,path={elsc}',
                '-serial', 'none', '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot', '-d', 'guest_errors',
                '-D', self.log_file('qemu-dbg.log'))

        def _configure_network(self, vm, netdev, pcap):
            """Attach a test card on the Origin 200's option PCI bus."""
            vm.add_args(
                '-nic', 'none',
                '-netdev', netdev,
                '-device',
                f'bcm570x,netdev=tgnet,addr={base.TG_ADDR},mac={base.TG_MAC}',
                '-object', f'filter-dump,id=tgcap,netdev=tgnet,file={pcap}')

        def _interface(self, con):
            return base.irix_interface_at_pci_slot(con, 'tg', self.TG_SLOT)

        def _netstat_counter(self, con, proto, pattern, tag):
            try:
                return int(con.capture_value(
                    f"netstat -s -p {proto} | " +
                    "awk '/" + pattern + "/ {print \"N=\" $1; exit}'",
                    tag, rb"N=(\d+)", timeout=60))
            except base.HarnessError:
                return None

        def _capture_network_diagnostics(self, con, iface, tag,
                                         receiver=False):
            commands = [
                f'ifconfig {iface}', 'netstat -rn', 'netstat -in', 'arp -an',
                'netstat -s -p ip', 'netstat -s -p tcp',
            ]
            if receiver:
                commands.extend((
                    'netstat -an', "ps -ef | grep '[t]tcp'",
                    'ls -l /tmp/rx.bin'))
            try:
                return con.run('; '.join(commands), tag, timeout=60)
            except (base.HarnessError, OSError) as error:
                return {'error': str(error)}

        @base.skipBigDataTest()
        @base.skipSlowTest()
        @base.skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_irix_tg_perf(self):
            self._run_tg_perf()

        def _run_tg_perf(self):
            prom = self.ASSET_FIRMWARE.fetch()
            disk = self.ASSET_DISK.fetch()
            if base.file_sha256(prom) != self.PROM_SHA256:
                self.fail('6.5.30 PROM SHA-256 mismatch')
            self.require_netdev('user')

            served_dir = self.scratch_file('served')
            os.makedirs(served_dir, exist_ok=True)

            pcap = self.log_file('tg-perf-wire.pcap')
            vm = self.get_vm(name=self.VM_NAME)
            elsc = self.log_file('elsc-aux.log')
            netdev = 'user,id=tgnet'
            if self.USERNET_OPTIONS:
                netdev += ',' + self.USERNET_OPTIONS
            netdev += (',tftp=%s'
                      ',hostfwd=tcp:127.0.0.1:0-%s:%d'
                      ',hostfwd=udp:127.0.0.1:0-%s:%d'
                      % (served_dir, self.GUEST_ADDR, TCP_GUEST_PORT,
                         self.GUEST_ADDR, UDP_GUEST_PORT))
            self._configure_machine(vm, prom, disk, elsc)
            self._configure_network(vm, netdev, pcap)
            launch_paused(vm)

            tcp_hp = _usernet_hostport(vm, 'tcp', TCP_GUEST_PORT,
                                       self.GUEST_ADDR)
            udp_hp = _usernet_hostport(vm, 'udp', UDP_GUEST_PORT,
                                       self.GUEST_ADDR)

            log_path = self.log_file('irix-tg-perf.log')
            con = base.IrixConsole(vm.console_socket, log_path,
                                   exited=lambda: vm.exitcode() is not None)
            results = {"tcp_verify": None, "tcp_mbps": None,
                       "udp_mbps": None, "udp_loss": None, "udp_tx": None,
                       "tcp_tx": None}
            login = tg_up = False
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
            iface = None
            seg_before = None
            foreground_ttcp = None
            receiver_pid = None
            receiver_supervisor_pid = None
            try:
                base.drive_irix_to_shell(con, base.LOGIN_BUDGET_SECONDS)
                login = True
                con.run('uname -a; id; date; hinv -mvv',
                        'machine_inventory', timeout=180)
                iface = self._interface(con)
                con.run(f"ifconfig {iface} inet {self.GUEST_ADDR} "
                        "netmask 0xffffff00 up", "ifup", timeout=60)
                try:
                    con.capture_value("ifconfig " + iface, "ifs",
                                      rb"(UP)[,>]", timeout=60)
                    tg_up = True
                except base.HarnessError:
                    pass

                results['network_state_before'] = (
                    self._capture_network_diagnostics(
                        con, iface, 'network_state_before'))
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
                if self.BACKGROUND_TCP_RECEIVER:
                    rx_script = "\n".join((
                        '#!/bin/sh',
                        f'rm -f {self.TCP_RECEIVER_PID} '
                        f'{self.TCP_RECEIVER_STATUS} /tmp/rx.bin',
                        f'ttcp -r -p {TCP_GUEST_PORT} -l 65536 > /tmp/rx.bin &',
                        'rx_pid=$!',
                        f'echo $rx_pid > {self.TCP_RECEIVER_PID}',
                        'attempt=0',
                        (f'while [ $attempt -lt '
                         f'{self.TCP_RECEIVER_READY_ATTEMPTS} ]; do'),
                        (f"    listening=`/usr/etc/netstat -an | "
                         f"grep '[.*]{TCP_GUEST_PORT} .*LISTEN'`"),
                        '    if [ -n "$listening" ]; then',
                        '        echo RXREADY',
                        '        echo "$listening"',
                        '        wait $rx_pid',
                        '        status=$?',
                        f'        echo $status > {self.TCP_RECEIVER_STATUS}',
                        '        echo "RX""DONE"',
                        '        exit $status',
                        '    fi',
                        '    sleep 1',
                        '    attempt=`expr $attempt + 1`',
                        'done',
                        'echo RXFAILED',
                        'kill -TERM $rx_pid 2>/dev/null',
                        'wait $rx_pid',
                        'status=$?',
                        f'echo $status > {self.TCP_RECEIVER_STATUS}',
                        'echo "RX""DONE"',
                        'exit 124',
                        ''))
                    script_bytes = rx_script.encode('ascii')
                    if con.run(f'rm -f {self.TCP_RECEIVER_SCRIPT}',
                               'rx_script_rm', timeout=60) != 0:
                        raise base.HarnessError('could not remove the old ttcp script')
                    for offset in range(0, len(script_bytes), 40):
                        encoded = script_bytes[offset:offset + 40].hex()
                        write_command = (
                            f'''perl -e 'print pack("H*","{encoded}")' >> '''
                            f'{self.TCP_RECEIVER_SCRIPT}')
                        if con.run(write_command, f'rx_script_{offset}',
                                   timeout=60) != 0:
                            raise base.HarnessError(
                                f'could not write ttcp script at byte {offset}')
                    if con.run(f'chmod 700 {self.TCP_RECEIVER_SCRIPT}',
                               'rx_script_chmod', timeout=60) != 0:
                        raise base.HarnessError('could not chmod the ttcp script')

                    ready_start = len(con.buf)
                    con.send(f'/bin/sh {self.TCP_RECEIVER_SCRIPT} &')
                    receiver_supervisor_pid = int(con.capture_value(
                        'echo RX_SUPERVISOR=$!', 'rx_supervisor',
                        rb'RX_SUPERVISOR=(\d+)', timeout=30))
                    results['tcp_receiver_supervisor_pid'] = (
                        receiver_supervisor_pid)
                    receiver_event = con.expect(
                        rb'RXREADY|RXFAILED',
                        self.TCP_RECEIVER_READY_ATTEMPTS + 5,
                        what='the ttcp receiver to report its listen state',
                        start=ready_start)
                    receiver_pid = int(con.capture_value(
                        f"cat {self.TCP_RECEIVER_PID} | sed 's/^/RXPID=/'",
                        'rx_pid', rb'RXPID=(\d+)', timeout=30))
                    results['tcp_receiver_pid'] = receiver_pid
                    if receiver_event.group(0) != b'RXREADY':
                        results['tcp_receiver_ready'] = False
                        raise base.HarnessError(
                            f'ttcp receiver did not listen on TCP port '
                            f'{TCP_GUEST_PORT} within '
                            f'{self.TCP_RECEIVER_READY_ATTEMPTS} seconds')
                    results['tcp_receiver_ready'] = True

                    results['network_state_before_syn'] = (
                        self._capture_network_diagnostics(
                            con, iface, 'network_state_before_syn',
                            receiver=True))
                    # Preserve the current neighbor table above, then clear
                    # only the gateway entry before testing its reply path.
                    results['gateway_neighbor_reset_status'] = con.run(
                        f'/usr/etc/arp -d {self.GATEWAY} '
                        '>/dev/null 2>&1 || :',
                        'clear_gateway_neighbor', timeout=60)
                    receiver_finished = con.run(
                        f'test -f {self.TCP_RECEIVER_STATUS}',
                        'rx_status_before_syn', timeout=10)
                    if receiver_finished == 0:
                        receiver_status = int(con.capture_value(
                            f"cat {self.TCP_RECEIVER_STATUS} | "
                            "sed 's/^/STATUS=/'", 'rx_status_before_syn_read',
                            rb'STATUS=(\d+)', timeout=10))
                        results['tcp_receiver_status_before_syn'] = (
                            receiver_status)
                        raise base.HarnessError(
                            'ttcp receiver exited after LISTEN before the host '
                            f'connection (status {receiver_status})')
                    seg_before_syn = self._netstat_counter(
                        con, 'tcp', 'packets received', 'segbefore_syn')
                    results['tcp_segments_before_syn'] = seg_before_syn

                    def capture_after_syn():
                        results['network_state_after_syn'] = (
                            self._capture_network_diagnostics(
                                con, iface, 'network_state_after_syn',
                                receiver=True))
                        seg_after_syn = self._netstat_counter(
                            con, 'tcp', 'packets received', 'segafter_syn')
                        results['tcp_segments_after_syn'] = seg_after_syn
                        if seg_before_syn is not None and seg_after_syn is not None:
                            results['tcp_segments_syn_delta'] = (
                                seg_after_syn - seg_before_syn)
                        if con.run(
                                f'test -f {self.TCP_RECEIVER_STATUS}',
                                'rx_status_after_syn', timeout=10) == 0:
                            receiver_status = int(con.capture_value(
                                f"cat {self.TCP_RECEIVER_STATUS} | "
                                "sed 's/^/STATUS=/'", 'rx_status_after_syn_read',
                                rb'STATUS=(\d+)', timeout=10))
                            results['tcp_receiver_status_after_syn'] = (
                                receiver_status)
                            raise base.HarnessError(
                                'ttcp receiver exited after the host SYN '
                                f'(status {receiver_status})')

                    rx_done_start = len(con.buf)
                    _tcp_stream(tcp_hp, verify_bytes,
                                after_connect=capture_after_syn)
                    con.expect(rb"RXDONE", 300, what="ttcp receive to finish",
                               start=rx_done_start)
                    receiver_status = int(con.capture_value(
                        f"cat {self.TCP_RECEIVER_STATUS} | sed 's/^/STATUS=/'",
                        'rx_status', rb'STATUS=(\d+)', timeout=30))
                    results['tcp_receiver_status'] = receiver_status
                    results['tcp_receiver_supervisor_status'] = con.run(
                        f'wait {receiver_supervisor_pid}', 'rx_supervisor_wait',
                        timeout=30)
                    results['tcp_receiver_files_cleanup_status'] = con.run(
                        f'rm -f {self.TCP_RECEIVER_PID} '
                        f'{self.TCP_RECEIVER_STATUS} '
                        f'{self.TCP_RECEIVER_SCRIPT}',
                        'rx_files_cleanup', timeout=30)
                    receiver_pid = None
                    receiver_supervisor_pid = None
                else:
                    # ttcp -r without -s writes the received stream to stdout.
                    foreground_ttcp = 'TCP byte-check receiver'
                    con.send(f"ttcp -r -p {TCP_GUEST_PORT} -l 65536 "
                             ">/tmp/rx.bin; echo \"RX\"\"DONE\"")
                    time.sleep(3)
                    _tcp_stream(tcp_hp, verify_bytes)
                    con.expect(rb"RXDONE", 300, what="ttcp receive to finish")
                    foreground_ttcp = None
                # ttcp appends its own report to the same stream, so checksum
                # exactly the payload rather than whatever else landed after it.
                # Tagged, because the echoed command line carries two numbers of
                # its own ("count=128 2>/dev/null") that an untagged pair of
                # integers matches before the output ever arrives.
                gcrc, gbytes = con.two_values(
                    "dd if=/tmp/rx.bin bs=8192 count=%d 2>/dev/null | cksum | "
                    "sed 's/^/CK=/'" % (VERIFY_BYTES // 8192), "vfy",
                    rb"CK=(\d+)\s+(\d+)", timeout=180)
                if self.BACKGROUND_TCP_RECEIVER:
                    results['tcp_rx_file_cleanup_status'] = con.run(
                        'rm -f /tmp/rx.bin', 'rx_data_cleanup', timeout=30)
                results["tcp_verify"] = {
                    "host_cksum": host_crc, "host_bytes": host_bytes,
                    "guest_cksum": gcrc, "guest_bytes": gbytes}

                # --- TCP throughput (ttcp -r) ---
                # -s sinks the payload. Without it ttcp writes every received
                # byte to stdout, which here is the serial console: megabytes
                # through an emulated uart, and the rate measured would be the
                # console's rather than the interface's.
                foreground_ttcp = 'TCP throughput receiver'
                con.send(f"ttcp -r -s -p {TCP_GUEST_PORT} -l 65536 2>&1; "
                         "echo \"TCP\"\"TP-$?\"")
                time.sleep(3)
                _tcp_stream(tcp_hp, os.urandom(TCP_THROUGHPUT_BYTES))
                con.expect(rb"TCPTP-\d+", 240, what="ttcp TCP to finish")
                foreground_ttcp = None
                tcp_out = con.buf.decode("utf-8", "replace")
                mbps, _, _ = _parse_ttcp_rate(
                    tcp_out)
                results["tcp_mbps"] = mbps

                # --- UDP throughput (ttcp -u -r) ---
                foreground_ttcp = 'UDP throughput receiver'
                con.send(f"ttcp -u -r -s -p {UDP_GUEST_PORT} -l {UDP_BUFLEN} "
                         "2>&1; echo \"UDP\"\"TP-$?\"")
                time.sleep(3)
                self.assertIsNotNone(udp_hp, "no UDP hostfwd port")
                _udp_ttcp_stream(udp_hp, UDP_BUFLEN, UDP_BUFS)
                con.expect(rb"UDPTP-\d+", 240, what="ttcp UDP to finish")
                foreground_ttcp = None
                udp_out = con.buf.decode("utf-8", "replace")
                umbps, ubytes, _ = _parse_ttcp_rate(
                    udp_out)
                results["udp_mbps"] = umbps
                results["udp_bytes"] = ubytes
                if ubytes is not None:
                    results["udp_loss"] = 1.0 - (ubytes /
                                                 float(UDP_BUFLEN * UDP_BUFS))

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
                foreground_ttcp = 'guest-to-host UDP sender'
                udp_tx_rc = con.run(
                    "dd if=/dev/zero bs=1024 count=8 2>/dev/null "
                    f"| ttcp -u -t -l 1024 -n 8 -p {udp_tx_port[0]} "
                    f"{self.GATEWAY}", "udp_tx", 180)
                foreground_ttcp = None
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
                foreground_ttcp = 'guest-to-host TCP sender'
                tx_rc = con.run(
                    "awk 'BEGIN { for (i = 0; i < 65536; i++) "
                    "printf \"%c\", (i * 37 + 11) % 256 }' "
                    "> /tmp/tg-tx.bin; "
                    "dd if=/tmp/tg-tx.bin bs=8192 count=8 2>/dev/null "
                    f"| ttcp -t -l 8192 -n 8 -p {tcp_tx_port[0]} "
                    f"{self.GATEWAY}", "tcp_tx", 180)
                foreground_ttcp = None
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
                shell_resumed = True
                if foreground_ttcp is not None:
                    results['foreground_ttcp'] = foreground_ttcp
                    try:
                        # Use the installed-disk diagnostic's existing shell
                        # resync pattern, limited to a foreground ttcp this
                        # test started.
                        con.sock.sendall(b'\x03\n')
                        recovery_rc = con.run(
                            'true', 'ttcp_resync', timeout=30)
                        shell_resumed = recovery_rc == 0
                        results['ttcp_recovery'] = {
                            'shell_resumed': shell_resumed,
                            'exit_status': recovery_rc,
                        }
                    except (base.HarnessError, OSError) as recovery_error:
                        shell_resumed = False
                        results['ttcp_recovery'] = {
                            'shell_resumed': False,
                            'error': str(recovery_error),
                        }
                if shell_resumed and iface is not None:
                    results['network_state_after_error'] = (
                        self._capture_network_diagnostics(
                            con, iface, 'network_state_after_error',
                            receiver=True))
                    seg_after_error = self._netstat_counter(
                        con, 'tcp', 'packets received', 'sega_after_error')
                    results['tcp_segments_after_error'] = seg_after_error
                    if seg_before is not None and seg_after_error is not None:
                        results['tcp_segments_delta'] = (
                            seg_after_error - seg_before)
                # Drain the console so a guest panic dump is captured before
                # the VM is torn down.
                for _ in range(25):
                    try:
                        con._pump()
                    except base.HarnessError:
                        break
                    time.sleep(1)
            finally:
                udp_tx_stop.set()
                tcp_tx_stop.set()
                if udp_tx_thread is not None:
                    udp_tx_thread.join(1)
                if tcp_tx_thread is not None:
                    tcp_tx_thread.join(1)
                if receiver_supervisor_pid is not None:
                    try:
                        if receiver_pid is None:
                            try:
                                receiver_pid = int(con.capture_value(
                                    f"cat {self.TCP_RECEIVER_PID} | "
                                    "sed 's/^/RXPID=/'", 'rx_pid_finally',
                                    rb'RXPID=(\d+)', timeout=10))
                                results['tcp_receiver_pid_finally'] = (
                                    receiver_pid)
                            except base.HarnessError:
                                pass
                        status_exists = con.run(
                            f'test -f {self.TCP_RECEIVER_STATUS}',
                            'rx_status_finally', timeout=10) == 0
                        if not status_exists and receiver_pid is not None:
                            results['tcp_receiver_kill_status_finally'] = (
                                con.run(
                                    f'kill -TERM {receiver_pid} 2>/dev/null',
                                    'rx_stop_finally', timeout=10))
                        results['tcp_receiver_supervisor_status_finally'] = (
                            con.run(f'wait {receiver_supervisor_pid}',
                                    'rx_wait_finally', timeout=30))
                        try:
                            results['tcp_receiver_status_finally'] = int(
                                con.capture_value(
                                    f"cat {self.TCP_RECEIVER_STATUS} | "
                                    "sed 's/^/STATUS=/'", 'rx_status_read_finally',
                                    rb'STATUS=(\d+)', timeout=10))
                        except base.HarnessError as status_error:
                            results['tcp_receiver_status_error_finally'] = (
                                str(status_error))
                        results['tcp_receiver_files_cleanup_finally'] = (
                            con.run(
                                f'rm -f {self.TCP_RECEIVER_PID} '
                                f'{self.TCP_RECEIVER_STATUS} '
                                f'{self.TCP_RECEIVER_SCRIPT} /tmp/rx.bin',
                                'rx_files_finally', timeout=30))
                    except (base.HarnessError, OSError) as cleanup_error:
                        results['tcp_receiver_cleanup_error_finally'] = (
                            str(cleanup_error))
                transcript = con.buf.decode("utf-8", errors="replace")
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)

            summary = self.log_file('irix-tg-perf-summary.json')
            import json
            with open(summary, 'w', encoding='utf-8') as f:
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
