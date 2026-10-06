#!/usr/bin/env python3
#
# Records a one-processor Origin 200 booting installed IRIX, with disk
# and network traffic, and replays it
#
# SPDX-License-Identifier: BSD-3-Clause
"""Record and replay installed IRIX on a one-processor Origin 200.

The disk sits behind QEMU's blkreplay driver and the user-mode network
behind a filter-replay object, so both are recorded. The guest boots to
a root login, brings up ef0 and flood-pings the user-mode gateway while
the host floods a forwarded UDP port. The flood fills the IOC3's receive
ring, so frames queue at the backend and are delivered later from the
device's receive flush, which is the work a replay has to place at the
same instruction as its recording. The replay must run to the end of
the log and print the recorded console byte for byte.

Each run gets its own qcow2 overlay on the installed disk, so the
replay starts from the same disk contents and nothing writes the
shared image.
"""

import os
import re
import socket
import sys
import threading
import time
from subprocess import check_call, DEVNULL

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import MACHINES, drive_console
from qemu_test import (QemuSystemTest, get_qemu_img, skipBigDataTest,
                       skipIfMissingEnv, skipSlowTest)
from qemu_test.asset import LocalAsset


RELEASE = '6.5.30'
FIRMWARE_REL = os.path.join('firmware', RELEASE, 'ip27prom.payload.bin')
DISK_REL = os.path.join('irix', RELEASE, 'root.qcow2')
PROM_SHA256 = (
    'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd')
DISK_SHA256 = (
    '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12')

ICOUNT_SHIFT = 7

# On 2026-10-05, at host load averages between 70 and 150, the recording
# reached the end marker in 45 to 60s and its replay in 53 to 66s. One
# replay of the same boot took 480s at the load's peak.
RUN_SECONDS = 900

END = re.compile(rb'RR-DONE')

# The marker is printed as RR-""DONE so the echoed command line cannot
# match it. The shell is csh, hence the sh -c.
DRIVER_STEPS = (
    (r'Option\?', '1\n'),
    (r'login:', 'root\n'),
    (r'TERM = \(vt100\)', '\n'),
    (r'[#$] ', 'ifconfig ef0 inet 10.0.2.15 netmask 255.255.255.0 up\n'),
    (r'[#$] ', "sh -c '/usr/etc/ping -q -f -c 3000 10.0.2.2; "
               "/usr/etc/ping -c 5 -i 0.2 10.0.2.2; netstat -i; "
               "echo RR-\"\"DONE'\n"),
)

# Started once the flood ping has begun. On the measured runs it made the
# IOC3 refuse between 95 and 165 frames.
FLOOD_AFTER = b'PING 10.0.2.2'
FLOOD_DATAGRAMS = 30000
FLOOD_PAYLOAD = b'x' * 1400


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class IrixRecordReplay(QemuSystemTest):

    timeout = 2 * RUN_SECONDS + 120
    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    def launch(self, mode, port):
        overlay = self.scratch_file(f'{mode}.qcow2')
        check_call([get_qemu_img(self), 'create', '-f', 'qcow2',
                    '-b', self.disk, '-F', 'qcow2', overlay],
                   stdout=DEVNULL, stderr=DEVNULL)
        vm = self.get_vm(name=mode)
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        vm.add_args(
            '-accel', 'tcg,thread=single',
            '-cpu', 'R10000', '-smp', '1', '-m', '64',
            '-bios', str(self.prom),
            '-audio', 'none',
            '-icount', f'shift={ICOUNT_SHIFT},rr={mode},'
            f'rrfile={self.replay_log}',
            '-drive', f'if=none,id=disk-direct,file={overlay},format=qcow2',
            '-drive', 'driver=blkreplay,if=none,image=disk-direct,id=disk0',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
            '-nic', f'user,id=net0,hostfwd=udp:127.0.0.1:{port}-:9',
            '-object', 'filter-replay,id=replay,netdev=net0',
            '-S')
        # Started paused: the PROM resets the machine at once, and a reset
        # event that beats the reply to QMP negotiation fails the launch.
        vm.launch()
        vm.cmd('cont')
        return vm

    def flood(self, log_path, port, stop):
        """Send the datagrams once the guest's flood ping has started."""
        while not stop.is_set():
            try:
                with open(log_path, 'rb') as log:
                    if FLOOD_AFTER in log.read():
                        break
            except FileNotFoundError:
                pass
            time.sleep(0.2)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            for _ in range(FLOOD_DATAGRAMS):
                if stop.is_set():
                    return
                sender.sendto(FLOOD_PAYLOAD, ('127.0.0.1', port))

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_boot_and_network_replay_identically(self):
        self.require_accelerator('tcg')
        self.set_machine('origin200')
        self.prom = self.ASSET_PROM.fetch()
        self.disk = self.ASSET_DISK.fetch()
        self.replay_log = self.scratch_file('replay.bin')

        record_log = self.log_file('console-record.log')
        port = free_udp_port()
        stop = threading.Event()
        flooder = threading.Thread(target=self.flood,
                                   args=(record_log, port, stop))
        vm = self.launch('record', port)
        try:
            flooder.start()
            recorded, reached = drive_console(
                vm.console_socket, record_log, RUN_SECONDS, DRIVER_STEPS,
                until=END)
            self.assertTrue(reached, 'record: the guest never printed the '
                            f'end marker, console log: {record_log}')
            vm.shutdown()
        finally:
            stop.set()
            flooder.join()
            vm.shutdown(hard=True)

        replay_log = self.log_file('console-replay.log')
        vm = self.launch('replay', free_udp_port())
        try:
            # Replay ignores console input and exits where the recording
            # stopped, which closes the console.
            replayed, _ = drive_console(vm.console_socket, replay_log,
                                        RUN_SECONDS, ())
            deadline = time.monotonic() + 60
            while vm.is_running() and time.monotonic() < deadline:
                time.sleep(0.2)
            self.assertFalse(
                vm.is_running(),
                f'replay: still running after {len(replayed)} of '
                f'{len(recorded)} console characters. A replay that stops '
                'advancing has diverged from its recording, for example '
                'a device that delivered work from a plain bottom half')
            vm.wait()
        finally:
            vm.shutdown(hard=True)

        # The recording stops at the marker; its replay runs on to where
        # QEMU quit, a few characters later.
        self.assertEqual(replayed[:len(recorded)], recorded)


if __name__ == '__main__':
    QemuSystemTest.main()
