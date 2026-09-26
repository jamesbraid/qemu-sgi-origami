#!/usr/bin/env python3
#
# IRIX on the one-node Origin 2000's BaseIO-G: check the board's ports
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The one-node Origin 2000 is the catalogue's BaseIO-G machine: QEMU
builds its IO1 board as io6g from the topology alone. This test checks
that board's ports through IRIX 6.5.30 on root-diags.qcow2, the disk
that carries olrti, in one boot: both IOC3s and RAD1 in hinv, olrti's
internal real-time interrupt loopback, ef0's station address and a
ping, a line out of each of three serial ports to a host file, a CD-ROM
on the second SCSI controller, and RAD1 playback to a host wav file.
test_irix_baseio_g_persistence.py checks what the board keeps across a
reboot and a stopped QEMU.

An archived capture of the same checks is in the project checkout
(docs/boot-logs/irix.boot.2026-09-26T00:22:42-07:00).

The BaseIO-G tests are two files, apart from each other and from
test_irix_origin2000_topologies.py, so meson runs them in parallel and
each file's timeout stays inside the nightly functional job's 150
minutes; the launch and shell helpers are the topology file's. Device
names (/dev/ttyd3 and ttyd4 for the MediaIO ports) follow the disk's
/etc/ioconfig.conf history, as that file's docstring explains.
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_origin2000_topologies import _output, device_numbers
import test_irix_origin2000_topologies as topologies

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


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

    class IrixBaseioGPorts(QemuSystemTest):
        """Boots IRIX on the one-node Origin 2000 and checks its
        BaseIO-G's ports through the guest."""

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


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
