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

Before this, the only evidence for these topologies was archived boot
captures in the project checkout (docs/boot-logs/irix.boot.
2026-09-24T10:16:40-07:00, ...T10:27:28-07:00 and ...T12:13:48-07:00)
and qtests of the boards' registers. Neither catches a change that stops
IRIX from finding the boards.

The disk is the published golden-with-diagnostics image
(`assets/irix/6.5.30/root-diags.qcow2`), the disk those captures booted,
opened with snapshot=on so no run writes it. IRIX numbers devices from
the disk's own /etc/ioconfig.conf history, not from the topology, so the
names asserted below (ef1, /hw/input1, SCSI controllers 2 and 3) hold
for this disk and would differ on another. Every boot starts from the
same golden state, so they are deterministic here.

Machine profile: the topology's own catalogue population, the release's
own PROM through -bios, multi-threaded TCG (test_mgras_greeter.py boots
the four-node Origin 2000 the same way) and user-mode networking so
IRIX's network start-up completes without a server. Nothing here asks
for graphics: root-diags.qcow2 carries no driver for the SI head, and
test_mgras_greeter.py owns that path on its own disk.
"""

import os
import re
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


class _Console(IrixConsole):
    STEP_TIMEOUT = STEP_TIMEOUT_SECONDS


def _text(data):
    return _ANSI_RE.sub('', bytes(data).decode('utf-8', errors='replace'))


def _drive_to_shell(con, budget):
    """Answer the System Maintenance Menu with Start System, log in as
    root, answer TERM and switch to /bin/sh. Returns the PROM's output up
    to the menu, which carries its configuration and diagnostics
    summary."""
    deadline = time.monotonic() + budget

    def left(cap):
        return max(1.0, min(cap, deadline - time.monotonic()))

    menu = con.expect(rb"Option\?", left(budget), what="the maintenance menu")
    prom = _text(con.buf[:menu.start()])
    con.send("1")
    con.expect(rb"login:", left(budget), what="the console login prompt")
    con.send("root")
    con.expect(rb"TERM\s*=\s*\(", left(120), what="the TERM prompt")
    con.send("")
    # The login csh prompt is "<hostname> <history number># ".
    con.expect(rb"[0-9]# ", left(120), what="the login csh prompt")
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


if QemuSystemTest is not None:

    class IrixOrigin2000Topologies(QemuSystemTest):
        """Boots IRIX on three Origin 2000 catalogue topologies and
        requires the PROM and IRIX to report what each one builds."""

        def _boot(self, topology, smp, memory_mib, extra_args=()):
            """Launch the topology and drive it to a root Bourne shell.
            Returns (vm, console, PROM output up to the menu, console
            log path)."""
            assets = require_assets(self, FIRMWARE_REL, DISK_REL)
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]

            vm = self.get_vm(name=topology)
            vm.set_machine(f"{MACHINES['origin2000']},topology={topology},"
                           "debug-leds=off")
            vm.set_console()
            vm.add_args(
                '-accel', 'tcg,thread=multi',
                '-cpu', 'R10000', '-smp', str(smp), '-m', str(memory_mib),
                '-bios', str(prom),
                '-audio', 'none',
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-nic', 'user,id=net0',
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
            log_path = self.log_file(f'{topology}-console.log')
            open(log_path, 'wb').close()
            con = _Console(vm.console_socket, log_path,
                           exited=lambda: vm.exitcode() is not None)
            self.addCleanup(con.close)
            started = time.monotonic()
            try:
                prom_text = _drive_to_shell(con, LOGIN_BUDGET_SECONDS)
            except HarnessError as exc:
                self.fail(f'{topology}: {exc} -- console log: {log_path}')
            self.log.info('%s: root shell after %.0fs', topology,
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


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
