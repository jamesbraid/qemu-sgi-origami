#!/usr/bin/env python3
#
# IRIX on the Origin 2000 catalogue topologies that fit a board into IO2
# or drop the router: boot each to a root shell and require the PROM and
# IRIX to report the hardware the topology builds
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Three Origin 2000 topologies from the compiled catalogue, each booted
through the stock IP27 PROM to an IRIX 6.5.30 root shell:

  - origin2000-pci-shoehorn: a PCI shoehorn in XIO slot IO2 carrying a
    Cad Duo card at PCI device 1. The PROM passes its diagnostics, hinv
    lists the card's Ethernet at slot io2, pci 1, and the input device
    IRIX names for the card's PS/2 ports is the Cad Duo's keyboard and
    mouse. Host keystrokes routed to that keyboard then arrive, byte for
    byte, on a read of /dev/input1/keyboard.
  - origin2000-pci-carrier: a three-slot PCI carrier in IO2 with a QLogic
    ISP12160 at carrier slot 0. hinv lists both of the card's SCSI
    channels and the hardware graph places them under IO2.
  - origin2000-one-node: one node board, two CPUs and 256 MiB, no
    router. The PROM finds one node, two CPUs and 256 MB and passes its
    diagnostics, and IRIX runs both processors with that memory.

The one-node module is also the catalogue's BaseIO-G machine: QEMU
builds its IO1 board as io6g from the topology alone. Two more tests
check that board through IRIX. One boot covers its ports: both IOC3s and
RAD1 in hinv, olrti's internal real-time interrupt loopback, ef0's
station address and a ping, a line out of each of three serial ports to
a host file, a CD-ROM on the second SCSI controller, and RAD1 playback to
a host wav file. The other covers what the board keeps: parallel-port
numbering and an nvram variable across an IRIX reboot, and the variable
and the Timekeeper clock across a full stop and relaunch of QEMU on the
same NVRAM image. Archived captures of the same checks are in the
project checkout (docs/boot-logs/irix.boot.2026-09-26T00:22:42-07:00 and
docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00).

Before this, the only evidence for these topologies was archived boot
captures in the project checkout (docs/boot-logs/irix.boot.
2026-09-24T10:16:40-07:00, ...T10:27:28-07:00 and ...T12:13:48-07:00)
and qtests of the boards' registers. Neither catches a change that stops
IRIX from finding the boards.

The disk is the published golden-with-diagnostics image
(`assets/irix/6.5.30/root-diags.qcow2`), the disk those captures booted,
opened with snapshot=on so no run writes it. IRIX numbers devices from
the disk's own /etc/ioconfig.conf history, not from the topology, so the
names asserted below (ef1, /hw/input1, SCSI controllers 2 and 3,
/dev/ttyd3 and ttyd4 for the MediaIO ports) hold for this disk and would
differ on another. Every boot starts from the
same golden state, so they are deterministic here.

Machine profile: the topology's own catalogue population, the release's
own PROM through -bios, multi-threaded TCG (test_mgras_greeter.py boots
the four-node Origin 2000 the same way) and user-mode networking so
IRIX's network start-up completes without a server. Nothing here asks
for graphics: root-diags.qcow2 carries no driver for the SI head, and
test_mgras_greeter.py owns that path on its own disk.
"""

import calendar
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import HarnessError, IrixConsole
from lib_prom import MACHINES, require_assets

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')

# The power-on diagnostics, kernel and init to a root shell. Measured
# on the debug_tcg build/ binary at host load averages of 85 to 130 on
# 32 host CPUs: 95 to 500s for the one-node module and 452 to 716s for
# the four-node shoehorn and carrier modules. 1500 is about twice the
# slowest; a boot that needs more is a hang, not a slow host.
LOGIN_BUDGET_SECONDS = 1500

# Shell commands after login are seconds of guest work each.
STEP_TIMEOUT_SECONDS = 180

# The PROM's own summary line when every power-on diagnostic passed.
ALL_DIAGS_PASSED = "ALL DIAGS PASSED"

# The Cad Duo's keyboard controller in the shoehorn topology's QOM tree.
# The plain BaseIO in IO1 has no keyboard, so the Cad Duo's is the only
# one this machine registers; the property is set anyway so the test
# says which keyboard it types on rather than relying on registration
# order (include/hw/input/sgi-ioc3-kbc.h describes the property).
CAD_DUO_KBC = '/machine/module0/slot12/cad-duo/kbc'

# "a" then "b", each pressed and released: the PS/2 make code of each
# letter (0x1c, 0x32) and its break code, 0xf0 then the make code. The
# two letters send the same bytes in scan-code sets 2 and 3, so the
# expectation does not depend on which set IRIX selects. A live read of
# /dev/input1/keyboard returned exactly these bytes while this test was
# written.
TYPED_KEYS = ('a', 'b')
TYPED_BYTES = ['1c', 'f0', '1c', '32', 'f0', '32']

# Lines of ls -l for a character device: permissions, links, owner,
# group, "major, minor", date (three fields), path.
DEVICE_LINE_RE = re.compile(
    r'^c\S+\s+\d+\s+\S+\s+\S+\s+(\d+),\s*(\d+)\s+\S+\s+\S+\s+\S+\s+(/\S+)'
    r'\s*$', re.MULTILINE)

# Lines of ls -l for a symbolic link: the link's name and its target.
LINK_LINE_RE = re.compile(r'^l\S+\s.*\s(\S+) -> (\S+)\s*$', re.MULTILINE)

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

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
SFPLAY_TIMEOUT_SECONDS = 900

# The Timekeeper's 32 KiB storage image and the 16-byte clock reference
# beside it, the pair the M48T35 model reads as sgi-nvram0 and
# sgi-nvram0-clock (tools/origami_lib/nvram.py in the project checkout
# creates the same two files for run irix --nvram-store).
NVRAM_BYTES = 0x8000
NVRAM_CLOCK_BYTES = 16

# How long QEMU stays fully stopped between the two launches that share
# the Timekeeper image, and how far the guest clock may differ from the
# host's elapsed time across them. The tolerance covers two console round
# trips; a clock that dropped the stopped interval is off by all of it.
STOPPED_SECONDS = 120
CLOCK_TOLERANCE_SECONDS = 20

# A fixed guest date well away from the host's, so a clock that fell back
# to host time on the relaunch cannot pass. The same setting as the
# archived clock capture docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00
# (project checkout).
GUEST_DATE_SET = '010112002030'
GUEST_DATE_FMT = '%Y-%m-%d %H:%M:%S'
GUEST_DATE_RE = re.compile(r'DATE=(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)')



class _Console(IrixConsole):
    STEP_TIMEOUT = STEP_TIMEOUT_SECONDS


def _text(data):
    return _ANSI_RE.sub('', bytes(data).decode('utf-8', errors='replace'))


def _drive_to_shell(con, budget, start=0):
    """Answer the System Maintenance Menu with Start System, log in as
    root, answer TERM and switch to /bin/sh. Returns the PROM's output up
    to the menu, which carries its configuration and diagnostics
    summary.

    A PROM whose NVRAM says to autoboot skips the menu and goes straight
    to the kernel: an IRIX reboot, or a relaunch on a kept NVRAM image,
    comes up that way. The login prompt then ends the PROM output
    instead. start is the console offset this boot began at, so a
    reboot's menu or prompt is not matched in the previous boot's
    output."""
    deadline = time.monotonic() + budget

    def left(cap):
        return max(1.0, min(cap, deadline - time.monotonic()))

    first = con.expect(re.compile(rb"Option\?|console login:"),
                       left(budget), what="the maintenance menu",
                       start=start)
    prom = _text(con.buf[start:first.start()])
    pos = first.end()
    if first.group(0) == b"Option?":
        con.send("1")
        pos = con.expect(rb"login:", left(budget),
                         what="the console login prompt", start=pos).end()
    con.send("root")
    pos = con.expect(rb"TERM\s*=\s*\(", left(120), what="the TERM prompt",
                     start=pos).end()
    con.send("")
    # The login csh prompt is "<hostname> <history number># ".
    con.expect(rb"[0-9]# ", left(120), what="the login csh prompt",
               start=pos)
    con.send("exec /bin/sh")
    con.run("PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; "
            "export PATH", "shinit", timeout=int(left(120)))
    return prom


def _output(con, cmd, tag, timeout=None):
    """Run cmd in the guest shell and return what it printed, from the
    shell's echo of the command up to lib_net's end marker."""
    pos = len(con.buf)
    con.run(cmd, tag, timeout)
    # The shell's prompt can land at the start of the first output line;
    # the prompt is always the root "# ".
    return re.sub(r'(?m)^# ', '', _text(con.buf[pos:]))


def device_numbers(listing):
    """{path: (major, minor)} for every character device in ls -l
    output."""
    return {path: (int(major), int(minor))
            for major, minor, path in DEVICE_LINE_RE.findall(listing)}


def link_targets(listing):
    """{name: target} for every symbolic link in ls -l output."""
    return dict(LINK_LINE_RE.findall(listing))


def processor_count(hinv):
    """How many processors hinv lists. hinv prints one "Processor N:"
    line per CPU, or, when every CPU is alike, a single "N <speed> MHZ
    IP27 Processors" line instead; both forms have been seen on this
    machine."""
    summary = re.search(r'(?m)^(\d+) \d+ MHZ IP27 Processors', hinv)
    if summary:
        return int(summary.group(1))
    return len(set(re.findall(r'(?m)^Processor (\d+):', hinv)))


def od_bytes(listing):
    """The hex byte tokens od -An -tx1 printed, in order."""
    return re.findall(r'\b[0-9a-f]{2}\b', listing)


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

    class IrixOrigin2000Topologies(QemuSystemTest):
        """Boots IRIX on three Origin 2000 catalogue topologies and
        requires the PROM and IRIX to report what each one builds."""

        def _boot(self, topology, smp, memory_mib, extra_args=(),
                  name=None, audio=('-audio', 'none'),
                  nic='user,id=net0'):
            """Launch the topology and drive it to a root Bourne shell.
            Returns (vm, console, PROM output up to the menu, console
            log path). name keeps two launches in one test apart; audio
            and nic replace the default arguments for those backends."""
            assets = require_assets(self, FIRMWARE_REL, DISK_REL)
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]
            name = name or topology

            vm = self.get_vm(name=name)
            vm.set_machine(f"{MACHINES['origin2000']},topology={topology},"
                           "debug-leds=off")
            vm.set_console()
            vm.add_args(
                '-accel', 'tcg,thread=multi',
                '-cpu', 'R10000', '-smp', str(smp), '-m', str(memory_mib),
                '-bios', str(prom),
                *audio,
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-nic', nic,
                # Start paused: the four-node PROM can reset the machine
                # within seconds of power-on, and the RESET event that
                # raises can reach the QMP client ahead of its
                # capabilities reply, which fails the launch.
                '-S',
                *extra_args)
            vm.launch()
            vm.cmd('cont')
            self.addCleanup(lambda: vm.shutdown(
                hard=vm.exitcode() is not None))

            # lib_net's Console appends; start each run's log empty.
            log_path = self.log_file(f'{name}-console.log')
            open(log_path, 'wb').close()
            con = _Console(vm.console_socket, log_path,
                           exited=lambda: vm.exitcode() is not None)
            self.addCleanup(con.close)
            started = time.monotonic()
            try:
                prom_text = _drive_to_shell(con, LOGIN_BUDGET_SECONDS)
            except HarnessError as exc:
                self.fail(f'{name}: {exc} -- console log: {log_path}')
            self.log.info('%s: root shell after %.0fs', name,
                          time.monotonic() - started)
            return vm, con, prom_text, log_path

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_pci_shoehorn(self):
            vm, con, prom, log_path = self._boot(
                'origin2000-pci-shoehorn', 8, 256)
            self.assertIn(ALL_DIAGS_PASSED, prom,
                          f'PROM diagnostics -- console log: {log_path}')

            hinv = _output(con, 'hinv', 'hinv')
            self.assertRegex(
                hinv, r'Fast Ethernet: ef1, .*slot io2, pci 1\b',
                f'hinv lists no Cad Duo Ethernet at io2, pci 1 -- '
                f'console log: {log_path}')

            # The input device IRIX names for the card must be the
            # card's own PS/2 ports: /dev/input1 links into /hw/input1,
            # and /hw/input1's keyboard and mouse are the same device
            # nodes (same device numbers) as the pckb and pcms vertices
            # under the card's PCI slot in IO2.
            card = '/hw/module/1/slot/io2/*/pci/1'
            links = link_targets(_output(
                con, 'ls -l /dev/input1', 'devinput'))
            self.assertEqual(links.get('keyboard'), '/hw/input1/keyboard')
            self.assertEqual(links.get('mouse'), '/hw/input1/mouse')
            devices = device_numbers(_output(
                con, f'ls -l /hw/input1/keyboard /hw/input1/mouse '
                f'{card}/pckb {card}/pcms', 'hwinput'))
            by_leaf = {}
            for path, number in devices.items():
                if path.startswith('/hw/module/1/slot/io2/'):
                    self.assertRegex(path, r'/pci/1/(pckb|pcms)$')
                by_leaf.setdefault(path.rsplit('/', 1)[1], number)
            for alias, card_leaf in (('keyboard', 'pckb'),
                                     ('mouse', 'pcms')):
                self.assertIn(f'/hw/input1/{alias}', devices,
                              f'no /hw/input1/{alias} -- console log: '
                              f'{log_path}')
                self.assertIn(card_leaf, by_leaf,
                              f'no {card_leaf} under {card} -- console '
                              f'log: {log_path}')
                self.assertEqual(
                    devices[f'/hw/input1/{alias}'], by_leaf[card_leaf],
                    f'/hw/input1/{alias} is not the Cad Duo {card_leaf} '
                    f'-- console log: {log_path}')

            # Live typing. Open the keyboard for a read of exactly the
            # expected byte count before any key is sent, so no byte can
            # arrive while nothing has the device open; od writes its
            # output only once dd has all of them.
            count = len(TYPED_BYTES)
            con.run('rm -f /tmp/kbd.out; '
                    f'(dd if=/dev/input1/keyboard bs=1 count={count} '
                    '2>/dev/null | od -An -tx1 > /tmp/kbd.out) & '
                    'sleep 5', 'kbdopen')
            vm.cmd('qom-set', path=CAD_DUO_KBC,
                   property='activate-keyboard', value=True)
            for key in TYPED_KEYS:
                for down in (True, False):
                    vm.cmd('input-send-event', events=[{
                        'type': 'key',
                        'data': {'down': down,
                                 'key': {'type': 'qcode', 'data': key}}}])
            # Give the bytes up to 30 guest seconds to arrive. An empty
            # result means dd is still blocked: the keystrokes never
            # reached the guest's keyboard driver.
            typed = _output(
                con, 'for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 '
                '18 19 20 21 22 23 24 25 26 27 28 29 30; do '
                '[ -s /tmp/kbd.out ] && break; sleep 1; done; '
                'echo KBD""-BEGIN; cat /tmp/kbd.out; echo KBD""-END',
                'kbdread')
            match = re.search(r'\nKBD-BEGIN\s*\n(.*?)KBD-END', typed, re.S)
            self.assertIsNotNone(match, typed)
            self.assertEqual(
                od_bytes(match.group(1)), TYPED_BYTES,
                'the bytes read from /dev/input1/keyboard are not the '
                f'typed keys -- console log: {log_path}')

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_pci_carrier(self):
            vm, con, prom, log_path = self._boot(
                'origin2000-pci-carrier', 8, 256,
                ('-device', 'qlogic-isp12160,bus=pci.carrier1,addr=0'))
            self.assertIn(ALL_DIAGS_PASSED, prom,
                          f'PROM diagnostics -- console log: {log_path}')

            hinv = _output(con, 'hinv', 'hinv')
            channels = re.findall(
                r'SCSI controller (\d+): Version QL12160', hinv)
            self.assertEqual(
                sorted(channels), ['2', '3'],
                f'hinv does not list the ISP12160 channels as SCSI '
                f'controllers 2 and 3 -- console log: {log_path}')

            links = link_targets(_output(
                con, 'ls -l /hw/scsi_ctlr', 'scsictlr'))
            for number, channel in zip(sorted(channels), ('0', '1')):
                self.assertRegex(
                    links.get(number, ''),
                    r'^/hw/module/1/slot/io2/[^/]+/pci/0/scsi_ctlr/'
                    f'{channel}$',
                    f'/hw/scsi_ctlr/{number} is not channel {channel} of '
                    f'the card at carrier slot 0 in IO2 -- console log: '
                    f'{log_path}')

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_one_node(self):
            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256)
            self.assertIn('2 CPUs on 1 nodes found.', prom)
            self.assertRegex(prom, r'No\. of NODEs enabled\s*=\s*1\b')
            self.assertRegex(prom, r'No\. of CPUs enabled\s*=\s*2\b')
            self.assertRegex(prom, r'Mem enabled\s*=\s*256 MB')
            self.assertIn(ALL_DIAGS_PASSED, prom,
                          f'PROM diagnostics -- console log: {log_path}')

            hinv = _output(con, 'hinv -c processor; hinv -c memory',
                           'hinv')
            self.assertEqual(
                processor_count(hinv), 2,
                f'IRIX is not running two processors -- console log: '
                f'{log_path}')
            self.assertRegex(
                hinv, r'Main memory size: 256 Mbytes',
                f'IRIX does not see 256 MB -- console log: {log_path}')

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
            self.assertGreater(
                wav_nonzero_samples(wav), 0,
                f'RAD1 playback reached {wav} as silence -- {where}')

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
                name='origin2000-one-node-baseio-g-first')
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
            self.assertIn('42', _output(con, 'nvram volume', 'nvramreboot'),
                          f'nvram volume lost across a reboot -- {where}')

            guest_before, host_before = guest_epoch(con, 'datebefore')
            self.assertEqual(con.run('sync', 'sync2'), 0, where)
            vm.shutdown()
            time.sleep(STOPPED_SECONDS)

            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256, timekeeper,
                name='origin2000-one-node-baseio-g-second')
            where = f'console log: {log_path}'
            guest_after, host_after = guest_epoch(con, 'dateafter')
            self.assertIn('42', _output(con, 'nvram volume', 'nvramstop'),
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
