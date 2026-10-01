#!/usr/bin/env python3
#
# IRIX onboard IOC3 Ethernet data-path test.
#
# SPDX-License-Identifier: BSD-3-Clause
"""Boot installed IRIX and move checked TCP payloads plus UDP packets over ef0.

The Origin 200's machine default NIC is its onboard IOC3 Ethernet, which IRIX
names ef0. The SLIRP backend attaches through ``-nic user``; no add-in adapter
is present in this test. Host-to-guest TCP is checked by POSIX cksum and byte
count. Guest-to-host TCP sends a fixed all-zero stream whose exact bytes and
length are checked by the host. UDP is exercised in each direction with ttcp;
payload bytes are checked exactly in both directions. The PROM and installed
IRIX disk are local test assets. Missing configured assets fail instead of
silently skipping a provisioning error.
"""

import os
import re
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import HarnessError, IrixConsole, drive_irix_to_shell  # noqa: E402
from lib_prom import MACHINES, file_sha256  # noqa: E402

try:
    from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
    from qemu_test import skipSlowTest
    from qemu_test.asset import LocalAsset
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = '6.5.30'
FIRMWARE_REL = os.path.join('firmware', RELEASE, 'ip27prom.payload.bin')
DISK_REL = os.path.join('irix', RELEASE, 'root.qcow2')
PROM_SHA256 = 'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd'
DISK_SHA256 = '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12'

MEMORY_MIB = 128
GUEST_ADDR = '10.0.2.15'
GATEWAY = '10.0.2.2'
IFACE = 'ef0'
TCP_RX_PORT = 5001
UDP_RX_PORT = 5002
TCP_TX_PORT = 5003
UDP_TX_PORT = 5004
PAYLOAD_BYTES = 1024 * 1024
UDP_PAYLOAD = bytes(range(256)) * 4
UDP_PAYLOAD_COUNT = 8
STEP_TIMEOUT = 300


def _ttcp_bytes(text):
    matches = re.findall(r'(\d+)\s+bytes\s+in\s+[\d.]+\s+real\s+sec', text)
    return int(matches[-1]) if matches else 0


def _run_guest(con, command, tag, timeout):
    pos, marker = con._run_to_marker(command, tag, timeout)
    output = bytes(con.buf[pos:marker.start()]).decode('utf-8', 'replace')
    return int(marker.group(1)), output


def _start_guest(con, command, tag):
    pos = len(con.buf)
    con.send(f'{command}; echo "MARK""-{tag}-$?"')
    return pos


def _finish_guest(con, pos, tag, timeout):
    marker = con.expect(rb'MARK-' + tag.encode() + rb'-(\d+)', timeout,
                        what=f'{tag} to finish', start=pos)
    output = bytes(con.buf[pos:marker.start()]).decode('utf-8', 'replace')
    return int(marker.group(1)), output


def _host_cksum(path):
    out = subprocess.run(['cksum', path], capture_output=True, text=True,
                         check=True).stdout
    crc, size = out.split()[:2]
    return crc, int(size)


def _hostfwd_port(vm, proto, guest_port):
    out = vm.cmd('human-monitor-command', command_line='info usernet')
    pattern = re.compile(
        r'%s.HOST_FORWARD.*127\.0\.0\.1\s+(\d+)\s+%s\s+%d\b' %
        (proto.upper(), re.escape(GUEST_ADDR), guest_port))
    for line in out.splitlines():
        match = pattern.search(line)
        if match:
            return int(match.group(1))
    return None


def _send_tcp(host_port, payload):
    sock = socket.create_connection(('127.0.0.1', host_port), timeout=30)
    sock.settimeout(STEP_TIMEOUT)
    try:
        sock.sendall(payload)
    finally:
        sock.shutdown(socket.SHUT_WR)
        sock.close()


def _tcp_sink(port, ready, chunks, errors, stop):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(('0.0.0.0', port))
        server.listen(1)
        server.settimeout(0.25)
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
        server.close()


def _udp_sink(port, ready, packets, controls, errors, stop):
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        server.bind(('0.0.0.0', port))
        server.settimeout(0.25)
        ready.set()
        while not stop.is_set() and len(packets) < 8:
            try:
                data, _ = server.recvfrom(65536)
            except socket.timeout:
                continue
            if not data:
                continue
            if data == bytes(4):
                controls.append(data)
            elif len(data) == 1024:
                packets.append(data)
            else:
                errors.append(
                    f'unexpected UDP datagram length {len(data)} bytes')
                break
    except OSError as error:
        if not stop.is_set():
            errors.append(str(error))
    finally:
        server.close()


def _send_udp(host_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for _ in range(UDP_PAYLOAD_COUNT):
            sock.sendto(UDP_PAYLOAD, ('127.0.0.1', host_port))
            time.sleep(0.02)
        for _ in range(UDP_PAYLOAD_COUNT):
            sock.sendto(b'', ('127.0.0.1', host_port))
            time.sleep(0.02)
    finally:
        sock.close()


if QemuSystemTest is not None:
    class IrixEf(QemuSystemTest):
        """Exercise TCP and UDP in each direction over the onboard ef0 NIC."""

        ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
        ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_irix_ef(self):
            prom = self.ASSET_FIRMWARE.fetch()
            disk = self.ASSET_DISK.fetch()
            if file_sha256(prom) != PROM_SHA256:
                self.fail('6.5.30 PROM SHA-256 mismatch')
            self.require_netdev('user')

            netdev = (
                'user,id=efnet,'
                'hostfwd=tcp:127.0.0.1:0-%s:%d,'
                'hostfwd=udp:127.0.0.1:0-%s:%d' %
                (GUEST_ADDR, TCP_RX_PORT, GUEST_ADDR, UDP_RX_PORT))
            vm = self.get_vm(name='irix_ef')
            vm.set_machine(MACHINES['origin200'])
            vm.set_console()
            elsc = self.log_file('elsc-aux.log')
            vm.add_args(
                '-accel', 'tcg,thread=single', '-cpu', 'R10000',
                '-smp', '1', '-m', str(MEMORY_MIB), '-bios', prom,
                '-audio', 'none', '-chardev', f'file,id=elsc,path={elsc}',
                '-serial', 'none', '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot', '-nic', netdev)
            vm.launch()

            tcp_host_port = _hostfwd_port(vm, 'tcp', TCP_RX_PORT)
            udp_host_port = _hostfwd_port(vm, 'udp', UDP_RX_PORT)
            self.assertIsNotNone(tcp_host_port, 'no TCP host-forward port')
            self.assertIsNotNone(udp_host_port, 'no UDP host-forward port')

            log_path = self.log_file('irix-ef.log')
            con = IrixConsole(vm.console_socket, log_path,
                              exited=lambda: vm.exitcode() is not None)
            tcp_stop = threading.Event()
            tcp_thread = None
            udp_stop = threading.Event()
            udp_thread = None
            try:
                drive_irix_to_shell(con, 1200)
                rc = con.run(f'ifconfig {IFACE} inet {GUEST_ADDR} '
                             'netmask 0xffffff00 up', 'ef_ifup', timeout=60)
                self.assertEqual(rc, 0, 'ifconfig ef0 failed')
                rc, ifconfig = _run_guest(con, f'ifconfig {IFACE}',
                                          'ef_ifshow', 60)
                self.assertEqual(rc, 0, 'ifconfig ef0 query failed')
                self.assertRegex(ifconfig, r'UP[,>]')
                rc, mtu_output = _run_guest(
                    con, "netstat -in | awk '$1 == \"ef0\" "
                    "{print \"MTU=\" $2; exit}'", 'ef_mtu', 60)
                self.assertEqual(rc, 0, 'ef0 MTU query failed')
                mtu = re.search(r'MTU=(\d+)', mtu_output)
                self.assertIsNotNone(mtu, 'ef0 MTU readback missing')
                self.assertEqual(int(mtu.group(1)), 1500,
                                 'ef0 reported a non-standard MTU')

                _, mtu_refusal = _run_guest(
                    con, f'ifconfig {IFACE} mtu 9000',
                    'ef_mtu_refusal', 60)
                # This IRIX ifconfig reports the ioctl refusal but exits zero.
                self.assertIn('ioctl (set mtu): Invalid argument',
                              mtu_refusal,
                              'ef0 MTU refusal diagnostic changed')
                rc, mtu_output = _run_guest(
                    con, "netstat -in | awk '$1 == \"ef0\" "
                    "{print \"MTU=\" $2; exit}'",
                    'ef_mtu_after_refusal', 60)
                self.assertEqual(rc, 0,
                                 'ef0 MTU query after refusal failed')
                mtu = re.search(r'MTU=(\d+)', mtu_output)
                self.assertIsNotNone(
                    mtu, 'ef0 MTU readback after refusal missing')
                self.assertEqual(int(mtu.group(1)), 1500,
                                 'ef0 MTU changed after jumbo refusal')

                def read_ef_counters(tag):
                    rc, output = _run_guest(con, 'netstat -in', tag, 60)
                    self.assertEqual(rc, 0, 'ef0 netstat -in query failed')
                    rows = map(str.split, output.splitlines())
                    row = next((fields for fields in rows if fields and
                                fields[0] in (IFACE, IFACE + '*')), None)
                    self.assertIsNotNone(row,
                                         'ef0 row missing from netstat -in')
                    self.assertGreaterEqual(len(row), 8,
                                            'malformed ef0 row in netstat -in')
                    counters = row[4:8]
                    self.assertTrue(all(value.isdigit()
                                        for value in counters),
                                    'malformed ef0 packet/error counters in '
                                    'netstat -in')
                    return tuple(map(int, counters))

                # Take the baseline after ifconfig has brought ef0 up, then
                # require its own counters to account for the checked traffic.
                ipkts_before, ierrs_before, opkts_before, oerrs_before = \
                    read_ef_counters('ef_counters_before')

                # Host to guest: compare the deterministic host payload using
                # the POSIX cksum implementation on both ends.
                payload = bytes(range(256)) * (PAYLOAD_BYTES // 256)
                path = self.scratch_file('ef-down.bin')
                with open(path, 'wb') as payload_file:
                    payload_file.write(payload)
                host_crc, host_size = _host_cksum(path)
                rx_pos = _start_guest(
                    con, f'ttcp -r -p {TCP_RX_PORT} -l 65536 '
                    '>/tmp/ef-down.bin 2>/dev/null', 'ef_rx_tcp')
                time.sleep(3)
                _send_tcp(tcp_host_port, payload)
                rc, _rx_output = _finish_guest(con, rx_pos, 'ef_rx_tcp',
                                               STEP_TIMEOUT)
                self.assertEqual(rc, 0, 'guest TCP receive failed')
                sum_rc, sum_output = _run_guest(
                    con, 'dd if=/tmp/ef-down.bin bs=8192 count=128 '
                    '2>/dev/null | cksum | sed \'s/^/CK=/\'',
                    'ef_downsum', 60)
                self.assertEqual(sum_rc, 0, 'guest cksum command failed')
                checksum = re.search(r'CK=(\d+)\s+(\d+)', sum_output)
                self.assertIsNotNone(checksum, 'guest cksum output missing')
                guest_crc = checksum.group(1)
                guest_size = int(checksum.group(2))
                self.assertEqual((guest_crc, guest_size),
                                 (host_crc, host_size),
                                 'host-to-guest TCP payload mismatch')

                # Guest to host: a fixed all-zero input stream is checked
                # byte-for-byte by the host-side TCP listener.
                tcp_ready = threading.Event()
                tcp_chunks, tcp_errors = [], []
                tcp_thread = threading.Thread(
                    target=_tcp_sink,
                    args=(TCP_TX_PORT, tcp_ready, tcp_chunks, tcp_errors,
                          tcp_stop), daemon=True)
                tcp_thread.start()
                self.assertTrue(tcp_ready.wait(30), 'TCP sink did not start')
                rc = con.run('dd if=/dev/zero bs=8192 count=128 2>/dev/null '
                             f'| ttcp -t -l 8192 -n 128 -p {TCP_TX_PORT} '
                             f'{GATEWAY}', 'ef_tx_tcp', STEP_TIMEOUT)
                self.assertEqual(rc, 0, 'guest TCP transmit failed')
                tcp_thread.join(30)
                tx_data = b''.join(tcp_chunks)
                self.assertFalse(tcp_thread.is_alive(), 'TCP sink timed out')
                self.assertEqual(tcp_errors, [])
                self.assertEqual(tx_data, bytes(PAYLOAD_BYTES),
                                 'guest-to-host TCP payload mismatch')

                # Host to guest UDP: ttcp's empty datagrams terminate the
                # receive leg, following the established IRIX eg test method.
                # Without -s, ttcp writes each received datagram to stdout.
                # Save that stream and compare its POSIX checksum and length.
                udp_payload_path = self.scratch_file('ef-down-udp.bin')
                with open(udp_payload_path, 'wb') as payload_file:
                    payload_file.write(UDP_PAYLOAD * UDP_PAYLOAD_COUNT)
                udp_host_crc, udp_host_size = _host_cksum(udp_payload_path)
                udp_pos = _start_guest(
                    con, f'ttcp -u -r -p {UDP_RX_PORT} -l 1024 '
                    '>/tmp/ef-down-udp.bin 2>/tmp/ef-down-udp-report',
                    'ef_rx_udp')
                time.sleep(3)
                _send_udp(udp_host_port)
                rc, udp_out = _finish_guest(con, udp_pos, 'ef_rx_udp',
                                            STEP_TIMEOUT)
                self.assertEqual(rc, 0, 'guest UDP receive failed: '
                                 f'{udp_out}')
                report_rc, udp_stderr = _run_guest(
                    con, 'cat /tmp/ef-down-udp-report',
                    'ef_udp_stderr', 60)
                self.assertEqual(report_rc, 0,
                                 'guest UDP report read failed')
                report_rc, udp_stdout = _run_guest(
                    con, 'dd if=/tmp/ef-down-udp.bin bs=1024 skip=8 '
                    '2>/dev/null | grep \'bytes in .* real sec\' || true',
                    'ef_udp_stdout', 60)
                self.assertEqual(report_rc, 0,
                                 'guest UDP report extraction failed')
                udp_out += udp_stderr + udp_stdout
                self.assertGreater(_ttcp_bytes(udp_out), 0,
                                   'guest ttcp received no UDP payload')
                self.assertEqual(_ttcp_bytes(udp_out), udp_host_size,
                                 'guest ttcp UDP byte count mismatch')
                sum_rc, sum_output = _run_guest(
                    con, f'dd if=/tmp/ef-down-udp.bin bs=1024 '
                    f'count={UDP_PAYLOAD_COUNT} '
                    '2>/dev/null | cksum | sed \'s/^/CK=/\'',
                    'ef_down_udp_sum', 60)
                self.assertEqual(sum_rc, 0,
                                 'guest UDP cksum command failed')
                checksum = re.search(r'CK=(\d+)\s+(\d+)', sum_output)
                self.assertIsNotNone(checksum,
                                     'guest UDP cksum output missing')
                self.assertEqual((checksum.group(1), int(checksum.group(2))),
                                 (udp_host_crc, udp_host_size),
                                 'host-to-guest UDP payload mismatch')

                # Permit the observed 4-byte zero control datagrams while
                # requiring eight full 1024-byte payload datagrams.
                udp_ready = threading.Event()
                udp_packets, udp_controls, udp_errors = [], [], []
                udp_thread = threading.Thread(
                    target=_udp_sink,
                    args=(UDP_TX_PORT, udp_ready, udp_packets, udp_controls,
                          udp_errors, udp_stop), daemon=True)
                udp_thread.start()
                self.assertTrue(udp_ready.wait(30), 'UDP sink did not start')
                rc = con.run('dd if=/dev/zero bs=1024 count=8 2>/dev/null '
                             f'| ttcp -u -t -l 1024 -n 8 -p {UDP_TX_PORT} '
                             f'{GATEWAY}', 'ef_tx_udp', STEP_TIMEOUT)
                self.assertEqual(rc, 0, 'guest UDP transmit failed')
                udp_thread.join(30)
                self.assertFalse(udp_thread.is_alive(), 'UDP sink timed out')
                self.assertEqual(udp_errors, [],
                                 'host received unexpected UDP framing')
                self.assertEqual(
                    len(udp_packets), 8,
                    'host did not receive eight 1024-byte UDP payloads; '
                    f'received {len(udp_packets)} payloads and '
                    f'{len(udp_controls)} 4-byte zero controls')
                self.assertEqual(udp_packets, [bytes(1024)] * 8,
                                 'guest-to-host UDP payload mismatch')

                ipkts_after, ierrs_after, opkts_after, oerrs_after = \
                    read_ef_counters('ef_counters_after')
                self.assertGreater(ipkts_after, ipkts_before,
                                   'ef0 Ipkts did not increase during traffic')
                self.assertGreater(opkts_after, opkts_before,
                                   'ef0 Opkts did not increase during traffic')
                self.assertEqual(ierrs_after, ierrs_before,
                                 'ef0 Ierrs increased during traffic')
                self.assertEqual(oerrs_after, oerrs_before,
                                 'ef0 Oerrs increased during traffic')
            except HarnessError as error:
                self.fail(f'IRIX ef network run stalled: {error} '
                          f'(console log: {log_path})')
            finally:
                tcp_stop.set()
                udp_stop.set()
                if tcp_thread is not None:
                    tcp_thread.join(1)
                if udp_thread is not None:
                    udp_thread.join(1)
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
