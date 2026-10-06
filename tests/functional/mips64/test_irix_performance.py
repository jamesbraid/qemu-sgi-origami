#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Checked, repeated whole-system workloads; boot is outside measurement."""

import hashlib
import json
import os
import platform
import shlex
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import IrixConsole, drive_irix_to_shell, posix_cksum
from lib_performance import measure, summarize
from lib_prom import (FORK_ROOT, MACHINES, file_sha256, launch_paused,
                      run_record, utc_now)
from test_irix_ef import (DISK_REL, DISK_SHA256, FIRMWARE_REL, PROM_SHA256,
                          GATEWAY, GUEST_ADDR, TCP_RX_PORT, _finish_guest,
                          _host_cksum, _hostfwd_port, _run_guest, _send_tcp,
                          _start_guest, _tcp_sink, _ttcp_bytes)
from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset


WORKLOADS = ('compress', 'tcp-receive', 'tcp-transmit', 'idle',
             'tcp-receive-sink', 'tcp-transmit-source')
DEFAULT_WORKLOADS = 'compress,tcp-receive,tcp-transmit,idle'


class IrixPerformance(QemuSystemTest):
    """Boot once, warm each workload once, retain checked measured samples."""

    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)
    timeout = 2700

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_irix_performance(self):
        repeats = int(os.getenv('QEMU_TEST_PERF_REPEATS', '3'))
        size = int(os.getenv('QEMU_TEST_PERF_BYTES', str(4 * 1024 * 1024)))
        iterations = int(os.getenv('QEMU_TEST_PERF_ITERATIONS', '8'))
        idle_seconds = float(os.getenv('QEMU_TEST_PERF_IDLE_SECONDS', '5'))
        cpus = int(os.getenv('QEMU_TEST_PERF_CPUS', '1'))
        selected = os.getenv('QEMU_TEST_PERF_WORKLOADS',
                             DEFAULT_WORKLOADS).split(',')
        extra_args = shlex.split(os.getenv('QEMU_TEST_PERF_QEMU_ARGS', ''))
        self.assertTrue(2 <= repeats <= 20, 'repeats must be 2..20')
        self.assertTrue(8192 <= size <= 32 * 1024 * 1024 and size % 8192 == 0,
                        'bytes must be 8192..33554432 and divisible by 8192')
        self.assertTrue(1 <= iterations <= 100, 'iterations must be 1..100')
        self.assertTrue(1 <= idle_seconds <= 60, 'idle seconds must be 1..60')
        self.assertIn(cpus, (1, 2))
        self.assertTrue(selected and set(selected) <= set(WORKLOADS)
                        and len(set(selected)) == len(selected),
                        f'workloads must be distinct names from {WORKLOADS}')
        prom, disk = self.ASSET_FIRMWARE.fetch(), self.ASSET_DISK.fetch()
        self.require_netdev('user')
        vm = self.get_vm(name='irix_performance')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        accel = 'tcg,thread=' + ('single' if cpus == 1 else 'multi')
        vm.add_args(
            '-accel', accel, '-cpu', 'R10000', '-smp', str(cpus), '-m', '128',
            '-bios', prom, '-audio', 'none',
            '-chardev', f'file,id=elsc,path={self.log_file("elsc-aux.log")}',
            '-serial', 'none', '-serial', 'chardev:elsc',
            '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
            '-snapshot', '-nic', 'user,id=efnet,'
            f'hostfwd=tcp:127.0.0.1:0-{GUEST_ADDR}:{TCP_RX_PORT}')
        vm.add_args(*extra_args)
        report = {'schema': 1, 'status': 'running', 'started_utc': utc_now(),
                  'workloads': []}
        report_path = self.log_file('performance.json')
        con = None
        try:
            launch_paused(vm)
            con = IrixConsole(vm.console_socket, self.log_file('console.log'),
                              exited=lambda: vm.exitcode() is not None)
            drive_irix_to_shell(con, 1200)
            self.assertEqual(con.run(
                f'ifconfig ef0 inet {GUEST_ADDR} netmask 0xffffff00 up',
                'perf_ifup', 60), 0)
            port = _hostfwd_port(vm, 'tcp', TCP_RX_PORT)
            self.assertIsNotNone(port, 'TCP host forwarding unavailable')
            # Hash-counter data repeats every 64 KiB and grows the LZW
            # dictionary. Force output because compress may expand this data.
            block = b''.join(hashlib.sha256(i.to_bytes(4, 'big')).digest()
                             for i in range(2048))
            payload = (block * ((size + len(block) - 1) // len(block)))[:size]
            payload_path = self.scratch_file('payload.bin')
            with open(payload_path, 'wb') as stream:
                stream.write(payload)
            crc, expected_size = _host_cksum(payload_path)
            counter = 0

            def tag(name):
                nonlocal counter
                counter += 1
                return f'perf_{name}_{counter}'

            def receive_prepare():
                name = tag('rx')
                pos = _start_guest(
                    con, f'ttcp -r -p {TCP_RX_PORT} -l 65536 '
                    '>/tmp/perf-input 2>/tmp/perf-ttcp', name)
                # Listener startup belongs to setup, outside the timed leg.
                time.sleep(3)
                return pos, name

            def receive(prepared):
                _send_tcp(port, payload)
                rc, output = _finish_guest(con, *prepared, 300)
                self.assertEqual(rc, 0, output)
                rc, output = _run_guest(
                    con, 'dd if=/tmp/perf-input of=/tmp/perf-report '
                    f'bs=8192 skip={size // 8192} 2>/dev/null '
                    '&& cat /tmp/perf-report /tmp/perf-ttcp',
                    tag('rx_report'), 60)
                self.assertEqual(rc, 0, output)
                self.assertEqual(_ttcp_bytes(output), size, output)
                rc, output = _run_guest(
                    con, f'dd if=/tmp/perf-input of=/tmp/perf-checked bs=8192 '
                    f'count={size // 8192} 2>/dev/null '
                    '&& cksum /tmp/perf-checked', tag('cksum'), 60)
                self.assertEqual(rc, 0, output)
                self.assertEqual(posix_cksum(output, '/tmp/perf-checked'),
                                 (crc, expected_size))
                return True

            receive(receive_prepare())
            # ttcp may append its own report to stdout; keep only checked data.
            self.assertEqual(con.run(
                f'dd if=/tmp/perf-input of=/tmp/perf-data bs=8192 '
                f'count={size // 8192} 2>/dev/null', tag('copy'), 60), 0)
            self.assertEqual(con.run(
                'test -x /usr/bsd/compress && test -x /usr/bsd/uncompress '
                '&& test -x /usr/bin/cmp', tag('tools'), 60), 0)
            options_path = self.build_file('meson-info',
                                           'intro-buildoptions.json')
            with open(options_path, encoding='utf-8') as stream:
                build_options = {o['name']: o['value']
                                 for o in json.load(stream)}
            common = {
                'machine': MACHINES['origin200'], 'cpu': 'R10000',
                'cpus': cpus, 'memory_mib': 128, 'accel': accel,
                'network': 'slirp-ioc3', 'disk_mode': 'snapshot',
                'qemu_extra_args': extra_args,
                'harness_sha256': {
                    name: file_sha256(os.path.join(os.path.dirname(__file__),
                                                   name))
                    for name in ('test_irix_performance.py',
                                 'lib_performance.py', 'test_irix_ef.py',
                                 'lib_net.py')},
                'firmware_sha256': PROM_SHA256, 'disk_sha256': DISK_SHA256,
                'host': {'system': platform.system(),
                         'release': platform.release(),
                         'machine': platform.machine(),
                         'node': os.getenv('QEMU_TEST_PERF_HOST',
                                           platform.node()),
                         'cpus': os.cpu_count()},
                'build_options': build_options,
                'malloc_perturb': os.getenv('MALLOC_PERTURB_', '0'),
                'warmups': 1, 'repeats': repeats,
            }
            report['identity'] = run_record(FORK_ROOT, self.qemu_bin,
                                             prom=prom, disk=disk)

            def record(name, prepare, operation, units, unit, config):
                if name not in selected:
                    return
                result = {'workload': name, 'config': {**common, **config},
                          'status': 'running', 'samples': []}
                report['workloads'].append(result)
                self.log.info('Starting %s, QEMU pid=%s', name, vm.get_pid())
                operation(prepare())
                for _ in range(repeats):
                    prepared = prepare()
                    result['samples'].append(measure(
                        vm.get_pid(), lambda: operation(prepared),
                        units, unit))
                result['summary'] = summarize(result.pop('samples'))
                result['status'] = 'passed'
                self.log.info('%s: %s', name, result['summary'])

            # IRIX's serial input line is short. Provision checked script
            # lines outside timing instead of sending the loop as one command.
            self.assertEqual(con.run(': >/tmp/perf-compress',
                                     tag('script_init'), 60), 0)
            script = [
                'i=0', f'while test $i -lt {iterations}; do',
                '/usr/bsd/compress -f -c /tmp/perf-data >/tmp/perf-data.Z '
                '|| exit $?',
                '/usr/bsd/uncompress -c /tmp/perf-data.Z >/tmp/perf-output '
                '|| exit $?',
                '/usr/bin/cmp /tmp/perf-data /tmp/perf-output || exit $?',
                'i=`expr $i + 1` || exit $?', 'done',
            ]
            for line in script:
                self.assertEqual(con.run(
                    f'echo {shlex.quote(line)} >>/tmp/perf-compress',
                    tag('script_line'), 60), 0)

            def compress(_):
                rc, output = _run_guest(con, '/bin/sh /tmp/perf-compress',
                                        tag('compress'), 300)
                self.assertEqual(rc, 0, output)
                return True

            record('compress', lambda: None, compress, size * iterations,
                   'input-byte', {'bytes': size, 'iterations': iterations,
                                  'payload_sha256': hashlib.sha256(
                                      payload).hexdigest(),
                                  'script': script,
                                  'interval': 'command through '
                                  'checked round trips'})
            record('tcp-receive', receive_prepare, receive, size, 'byte',
                   {'bytes': size, 'payload_sha256': hashlib.sha256(
                                      payload).hexdigest(),
                    'interval': 'send through guest completion and checksum'})

            # ttcp -s discards what it receives, so the interval holds the
            # network path and not the guest's file system or checksum.
            def receive_sink_prepare():
                name = tag('rx_sink')
                pos = _start_guest(
                    con, f'ttcp -r -s -p {TCP_RX_PORT} -l 65536 2>&1', name)
                time.sleep(3)
                return pos, name

            def receive_sink(prepared):
                _send_tcp(port, payload)
                rc, output = _finish_guest(con, *prepared, 300)
                self.assertEqual(rc, 0, output)
                self.assertEqual(_ttcp_bytes(output), size, output)
                return True

            record('tcp-receive-sink', receive_sink_prepare, receive_sink,
                   size, 'byte',
                   {'bytes': size, 'payload_sha256': hashlib.sha256(
                                      payload).hexdigest(),
                    'interval': 'send through guest completion and '
                    'received count'})

            self.assertEqual(con.run(
                f'dd if=/dev/zero of=/tmp/perf-zeros bs=8192 '
                f'count={size // 8192} 2>/dev/null', tag('zeros'), 60), 0)

            def transmit_checked(command, check):
                # Bind an ephemeral listener before the guest starts. Reuse
                # the IOC3 test's receiver and verify every received byte.
                ready, stop = threading.Event(), threading.Event()
                chunks, errors = [], []
                with socket.socket() as reservation:
                    reservation.bind(('127.0.0.1', 0))
                    host_port = reservation.getsockname()[1]
                thread = threading.Thread(target=_tcp_sink,
                                          args=(host_port, ready, chunks,
                                                errors, stop), daemon=True)
                thread.start()
                try:
                    self.assertTrue(ready.wait(10),
                                    'TCP sink startup timed out')
                    rc, output = _run_guest(con, command(host_port),
                                            tag('tx'), 300)
                    self.assertEqual(rc, 0, output)
                    thread.join(30)
                    self.assertFalse(thread.is_alive(),
                                     'TCP receive timed out')
                    self.assertEqual(errors, [])
                    check(b''.join(chunks))
                    return True
                finally:
                    stop.set()
                    thread.join(2)

            def transmit(_):
                return transmit_checked(
                    lambda host_port: f'ttcp -t -l 8192 -n {size // 8192} '
                    f'-p {host_port} {GATEWAY} </tmp/perf-zeros',
                    lambda data: self.assertEqual(data, bytes(size)))

            def check_source(data):
                # ttcp -s sends one printable pattern buffer repeatedly.
                block = data[:8192]
                self.assertEqual(len(data), size)
                self.assertTrue(all(0x20 <= b <= 0x7e for b in block))
                self.assertEqual(data, block * (size // 8192))

            def transmit_source(_):
                # Without a file to read, the interval holds the network path.
                return transmit_checked(
                    lambda host_port: f'ttcp -t -s -l 8192 '
                    f'-n {size // 8192} -p {host_port} {GATEWAY}',
                    check_source)

            record('tcp-transmit', lambda: None, transmit, size, 'byte',
                   {'bytes': size, 'payload': 'zero',
                    'interval': 'listener startup through checked host EOF'})
            record('tcp-transmit-source', lambda: None, transmit_source, size,
                   'byte', {'bytes': size, 'payload': 'ttcp-pattern',
                            'interval': 'listener startup through checked '
                            'host EOF'})

            def idle(_):
                end = time.monotonic() + idle_seconds
                while time.monotonic() < end:
                    con.sock.settimeout(max(0.001, end - time.monotonic()))
                    con._pump()
                con.sock.settimeout(1.0)
                self.assertEqual(con.run('true', tag('idle'), 60), 0)
                self.assertEqual(vm.cmd('query-status')['status'], 'running')
                return True

            record('idle', lambda: None, idle, 1, 'window',
                   {'seconds': idle_seconds,
                    'interval': 'idle window through shell and running check'})
            report['status'] = 'passed'
        except Exception as error:
            report['status'] = 'failed'
            report['error'] = str(error)
            raise
        finally:
            report['ended_utc'] = utc_now()
            with open(report_path, 'w', encoding='utf-8') as stream:
                json.dump(report, stream, indent=2, sort_keys=True)
                stream.write('\n')
            if con is not None:
                con.close()
            vm.shutdown()


if __name__ == '__main__':
    QemuSystemTest.main()
