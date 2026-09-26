#!/usr/bin/env python3
#
# IRIX on the one-node Origin 2000's BaseIO-G: check the board's ports,
# and what it keeps across a reboot and a stopped QEMU
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The one-node Origin 2000 is the catalogue's BaseIO-G machine: QEMU
builds its IO1 board as io6g from the topology alone. Two tests check
that board through IRIX 6.5.30 on root-diags.qcow2, the disk that
carries olrti.

test_one_node_baseio_g_ports covers the ports in one boot: both IOC3s
and RAD1 in hinv, olrti's internal real-time interrupt loopback, ef0's
station address and a ping, a line out of each of three serial ports to
a host file, a CD-ROM on the second SCSI controller, and RAD1 playback to
a host wav file.

test_one_node_baseio_g_reboot_and_stop covers what the board keeps:
parallel-port numbering and an nvram variable across an IRIX reboot, and
the variable and the Timekeeper clock across a full stop and relaunch of
QEMU on the same NVRAM image.

Archived captures of the same checks are in the project checkout
(docs/boot-logs/irix.boot.2026-09-26T00:22:42-07:00 and
docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00).

These live in their own file, not beside the other one-node boot in
test_irix_origin2000_topologies.py, so meson runs them in parallel with
it under a timeout sized for these boots alone; the launch and shell
helpers are that file's. Device names (/dev/ttyd3 and ttyd4 for the
MediaIO ports) follow the disk's /etc/ioconfig.conf history, as that
file's docstring explains.
"""

import calendar
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import HarnessError
from test_irix_origin2000_topologies import (_drive_to_shell, _output,
                                             device_numbers)
import test_irix_origin2000_topologies as topologies

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


# The one-node module's power-on diagnostics, kernel and init to a root
# shell: 95 to 500 s recorded in test_irix_origin2000_topologies.py, and
# 136 to 335 s in the runs of these tests. 900 is almost twice the
# slowest; the topology file's 1500 is sized for its four-node modules.
LOGIN_BUDGET_SECONDS = 900

# The one-node module's BaseIO-G, as IRIX names it once the board's
# identity record gives the slot its name: the board IOC3 at PCI device 2
# enumerates at pci/4, the MediaIO IOC3 at pci/6 and RAD1 at pci/7.
BASEIO_G = '/hw/module/1/slot/io1/baseio/pci'

# An explicit station address for the board IOC3's Ethernet. QEMU never
# rewrites an explicit mac=, so the address netstat reports can only have
# come through the model's one-wire Ethernet-address part.
EF0_MAC = '08:00:69:0e:c6:01'

# QEMU's user-mode network: the guest takes an alias on it and pings the
# gateway. The disk's own ef0 address is on a network QEMU does not route.
USER_NET_ALIAS = '10.0.2.15'
USER_NET_GATEWAY = '10.0.2.2'

# The shipped confidence tone the RAD1 playback check plays. Without
# icount the model's stream stretches in time (docs/NOW.md in the project
# checkout records 1.35 to more than 15 times the source length), so the
# check asserts samples arrived, not a duration.
CONFIDENCE_TONE = '/usr/lib/Confidence/audioOut44.aiff'
# One play of that tone, 6.757 s of 44.1 kHz mono, reaches the stereo
# output as this many sample words; the capture must hold at least that
# many nonzero ones, not merely one.
TONE_SAMPLE_WORDS = int(6.757 * 44100) * 2
SFPLAY_TIMEOUT_SECONDS = 900

# The Timekeeper's 32 KiB storage image and the 16-byte clock reference
# beside it, the pair the M48T35 model reads as sgi-nvram0 and
# sgi-nvram0-clock (tools/origami_lib/nvram.py in the project checkout
# creates the same two files for run irix --nvram-store).
NVRAM_BYTES = 0x8000
NVRAM_CLOCK_BYTES = 16

# The IRIX nvram variable the test sets, and its value read back alone
# on a line.
NVRAM_VALUE_RE = r'(?m)^42\s*$'

# How long QEMU stays fully stopped between the two launches that share
# the Timekeeper image, and how far the guest clock may differ from the
# host's elapsed time across them. The tolerance covers two console round
# trips; a clock that dropped the stopped interval is off by all of it.
STOPPED_SECONDS = 120
CLOCK_TOLERANCE_SECONDS = 20

# A fixed guest date well away from the host's. A fresh clock reference
# makes the M48T35 model follow the host clock, so if the guest's write
# never reached the Timekeeper the guest would read host time on both
# launches and the elapsed-time comparison alone would still pass. The
# test therefore first requires the guest to read a date at least
# DATE_SET_MIN_AHEAD_SECONDS past the host's. The same setting as the
# archived clock capture docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00
# (project checkout).
GUEST_DATE_SET = '010112002030'
GUEST_DATE_FMT = '%Y-%m-%d %H:%M:%S'
DATE_SET_MIN_AHEAD_SECONDS = 3 * 365 * 86400
GUEST_DATE_RE = re.compile(r'DATE=(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)')


def guest_epoch(con, tag):
    """The guest's UTC time as seconds since the epoch, and the host's at
    the moment the guest's answer arrived."""
    text = _output(con, "date -u '+DATE=%Y-%m-%d %H:%M:%S'", tag)
    host = time.time()
    match = GUEST_DATE_RE.search(text)
    if match is None:
        raise HarnessError(f'{tag}: date printed no DATE= line: {text!r}')
    guest = calendar.timegm(time.strptime(match.group(1), GUEST_DATE_FMT))
    return guest, host


def wav_nonzero_samples(path):
    """How many 16-bit sample words after the 44-byte header are not
    zero. QEMU's wav backend leaves the header's lengths at zero, so the
    data is read from byte 44 to the end of the file."""
    with open(path, 'rb') as wav:
        data = wav.read()[44:]
    count = len(data) // 2
    return sum(1 for word in struct.unpack(f'<{count}h', data[:count * 2])
               if word)


if QemuSystemTest is not None:

    class IrixBaseioG(QemuSystemTest):
        """Boots IRIX on the one-node Origin 2000 and checks its
        BaseIO-G through the guest."""

        # The topology file's launcher, shared rather than copied.
        _boot = topologies.IrixOrigin2000Topologies._boot

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_one_node_baseio_g_ports(self):
            """The one-node module's BaseIO-G, port by port, in one boot:
            both IOC3s and RAD1 in hinv, the internal real-time interrupt
            loopback, the Ethernet station address and traffic, the board
            IOC3's second serial port and both MediaIO serial ports to
            host files, a CD-ROM on the second SCSI controller, and RAD1
            playback to a host wav file."""
            host_file = {port: self.scratch_file(f'{port}.out')
                         for port in ('ioc3-b', 'mio-a', 'mio-b')}
            wav = self.scratch_file('rad1.wav')
            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256, (
                    # QEMU serial line 1 is the board IOC3's port B, the
                    # guest's ttyd2; set_console() owns line 0.
                    '-chardev', f'file,id=ioc3b,path={host_file["ioc3-b"]}',
                    '-serial', 'chardev:ioc3b',
                    '-chardev', f'file,id=mioa,path={host_file["mio-a"]}',
                    '-chardev', f'file,id=miob,path={host_file["mio-b"]}',
                    '-global', 'sgi-xio-baseio.mio-chardev-a=mioa',
                    '-global', 'sgi-xio-baseio.mio-chardev-b=miob',
                    # An empty drive: IRIX lists the unit from its inquiry
                    # data alone.
                    '-device', 'scsi-cd,bus=scsi.1,scsi-id=4,lun=0'),
                name='origin2000-one-node-baseio-g-ports',
                budget=LOGIN_BUDGET_SECONDS,
                audio=('-audiodev', f'wav,id=rad1,path={wav}',
                       '-global', 'sgi-rad1-audio.audiodev=rad1'),
                nic=f'user,id=net0,mac={EF0_MAC}')
            where = f'console log: {log_path}'

            hinv = _output(con, 'hinv', 'hinv')
            for serial in (1, 2, 3, 4):
                self.assertRegex(
                    hinv, rf'IOC3/IOC4 serial port: tty{serial}\b',
                    f'hinv lists no tty{serial} -- {where}')
            self.assertRegex(
                hinv, r'Integral Fast Ethernet: ef0, .*slot io1, pci 2\b',
                f'hinv lists no board-IOC3 Ethernet -- {where}')
            self.assertRegex(hinv, r'IOC3 parallel port: plp\d',
                             f'hinv lists no MediaIO parallel port -- {where}')
            self.assertRegex(hinv, r'Iris Audio Processor: version RAD',
                             f'hinv lists no RAD1 -- {where}')
            self.assertIn('IOC3/IOC4 external interrupts: 1', hinv,
                          f'hinv lists no real-time interrupt -- {where}')
            # IRIX 6.5.30 prints the PCI identity in hex; the owner's
            # inventory the BaseIO datasheet cites, from another release,
            # prints "vendor 4265, device 5) pci slot 7".
            self.assertRegex(
                _output(con, 'hinv -v | grep 0x10a9', 'hinvv'),
                r'vendor 0x10a9, device 0x0005\) PCI slot 7\b',
                f'hinv -v places no RAD1 at PCI slot 7 -- {where}')

            olrti = _output(
                con, f'/usr/diags/bin/olrti -vertex {BASEIO_G}/4 '
                '-internal', 'olrti', timeout=600)
            self.assertRegex(olrti, r'RSLT internal\s+PASS',
                             f'olrti internal loopback -- {where}')

            netstat = _output(con, 'netstat -ia', 'netstat')
            self.assertRegex(netstat, r'(?m)^ef0\s',
                             f'netstat -ia lists no ef0 -- {where}')
            self.assertIn(EF0_MAC, netstat,
                          f'ef0 does not report {EF0_MAC} -- {where}')
            self.assertEqual(con.run(
                f'ifconfig ef0 alias {USER_NET_ALIAS} netmask 255.255.255.0',
                'alias'), 0, f'ifconfig alias -- {where}')
            ping = _output(con, f'ping -c 5 {USER_NET_GATEWAY}', 'ping')
            self.assertIn('5 packets transmitted, 5 packets received', ping,
                          f'ping over ef0 -- {where}')

            # Which /dev/ttyd names the disk gives each port depends on its
            # ioconfig history (see the module docstring), so the test
            # proves each one is the intended chip's port by device number
            # before writing to it.
            ports = {'ioc3-b': ('/dev/ttyd2', f'{BASEIO_G}/4/tty/2/d'),
                     'mio-a': ('/dev/ttyd3', f'{BASEIO_G}/6/tty/1/d'),
                     'mio-b': ('/dev/ttyd4', f'{BASEIO_G}/6/tty/2/d')}
            numbers = device_numbers(_output(
                con, 'ls -lL ' + ' '.join(p for pair in ports.values()
                                          for p in pair), 'ttys'))
            for port, (dev, vertex) in ports.items():
                self.assertIn(vertex, numbers, f'no {vertex} -- {where}')
                self.assertEqual(numbers.get(dev), numbers[vertex],
                                 f'{dev} is not {vertex} -- {where}')
                self.assertEqual(con.run(f'echo origami-{port} > {dev}',
                                         f'write-{port}'), 0,
                                 f'writing {dev} -- {where}')
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                received = {}
                for port, path in host_file.items():
                    with open(path, 'rb') as out:
                        received[port] = out.read()
                if all(f'origami-{port}'.encode() in data
                       for port, data in received.items()):
                    break
                time.sleep(1)
            for port, data in received.items():
                self.assertIn(f'origami-{port}'.encode(), data,
                              f'the line written to {ports[port][0]} never '
                              f'reached the host -- {where}')

            # IRIX files a CD-ROM under the scsi class, not disk.
            self.assertRegex(
                _output(con, 'hinv -c scsi', 'cdrom'),
                r'CDROM: unit 4 on SCSI controller 1\b',
                f'hinv -c scsi lists no CD-ROM on controller 1 -- {where}')

            self.assertEqual(
                con.run(f'/usr/sbin/sfplay {CONFIDENCE_TONE}', 'sfplay',
                        timeout=SFPLAY_TIMEOUT_SECONDS), 0,
                f'sfplay -- {where}')
            nonzero = wav_nonzero_samples(wav)
            self.assertGreaterEqual(
                nonzero, TONE_SAMPLE_WORDS,
                f'RAD1 playback left {nonzero} nonzero sample words in '
                f'{wav}, fewer than one play of the tone -- {where}')

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_one_node_baseio_g_reboot_and_stop(self):
            """What the BaseIO-G keeps across a reboot and across a full
            stop of QEMU. A fresh Timekeeper image and clock reference back
            both launches. The first launch sets an IRIX nvram variable and
            a fixed date, and removes every parallel-port entry from
            /etc/ioconfig.conf; after an in-process reboot the one live
            parallel port is plp1 and the variable reads back. QEMU is
            then stopped, left stopped, and relaunched on the same image:
            the variable still reads back, and the guest clock has
            advanced by the host's elapsed time, stopped interval
            included."""
            nvram = self.scratch_file('nvram0.raw')
            clock = self.scratch_file('nvram0.clock')
            with open(nvram, 'wb') as image:
                image.write(bytes(NVRAM_BYTES))
            with open(clock, 'wb') as image:
                image.write(bytes(NVRAM_CLOCK_BYTES))
            timekeeper = (
                '-drive', f'if=none,id=sgi-nvram0,file={nvram},format=raw',
                '-drive',
                f'if=none,id=sgi-nvram0-clock,file={clock},format=raw')

            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256, timekeeper,
                name='origin2000-one-node-baseio-g-first',
                budget=LOGIN_BUDGET_SECONDS)
            where = f'console log: {log_path}'

            # The disk carries a parallel entry from an Origin 200 install
            # at a hardware-graph path this machine does not have, which
            # holds unit 1; this boot then records the live port behind it.
            ioconfig = _output(con, 'cat /etc/ioconfig.conf', 'ioconfig')
            self.assertRegex(ioconfig,
                             r'(?m)^1 \S+/MotherBoard/\S+/ecpp\s*$',
                             f'the disk no longer carries the stale '
                             f'parallel entry this check removes -- {where}')
            self.assertRegex(ioconfig, rf'(?m)^\d+ {BASEIO_G}/6/ecpp\s*$',
                             f'IRIX recorded no MediaIO parallel entry -- '
                             f'{where}')
            for cmd, tag in (
                    ("grep -v '/ecpp$' /etc/ioconfig.conf > /tmp/ioc",
                     'dropecpp'),
                    ('cp /tmp/ioc /etc/ioconfig.conf', 'putioc'),
                    ('nvram volume 42', 'nvramset'),
                    (f'date {GUEST_DATE_SET}', 'dateset'),
                    ('sync', 'sync')):
                self.assertEqual(con.run(cmd, tag), 0, f'{cmd} -- {where}')

            start = len(con.buf)
            con.send('reboot')
            try:
                _drive_to_shell(con, LOGIN_BUDGET_SECONDS, start=start)
            except HarnessError as exc:
                self.fail(f'reboot: {exc} -- {where}')
            self.assertRegex(
                _output(con, 'hinv | grep parallel', 'plp'),
                r'IOC3 parallel port: plp1\b',
                f'with no parallel history the port is not plp1 -- {where}')
            self.assertRegex(_output(con, 'nvram volume', 'nvramreboot'),
                             NVRAM_VALUE_RE,
                             f'nvram volume lost across a reboot -- {where}')

            guest_before, host_before = guest_epoch(con, 'datebefore')
            self.assertGreater(
                guest_before - host_before, DATE_SET_MIN_AHEAD_SECONDS,
                f'the guest does not read the 2030 date it set, so its '
                f'write never reached the Timekeeper -- {where}')
            self.assertEqual(con.run('sync', 'sync2'), 0, where)
            vm.shutdown()
            time.sleep(STOPPED_SECONDS)

            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256, timekeeper,
                name='origin2000-one-node-baseio-g-second',
                budget=LOGIN_BUDGET_SECONDS)
            where = f'console log: {log_path}'
            guest_after, host_after = guest_epoch(con, 'dateafter')
            self.assertRegex(_output(con, 'nvram volume', 'nvramstop'),
                             NVRAM_VALUE_RE,
                             f'nvram volume lost across a stop -- {where}')
            self.assertLess(
                abs((guest_after - guest_before) -
                    (host_after - host_before)), CLOCK_TOLERANCE_SECONDS,
                f'guest clock advanced {guest_after - guest_before}s while '
                f'the host advanced {host_after - host_before:.0f}s '
                f'including {STOPPED_SECONDS}s stopped -- {where}')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
