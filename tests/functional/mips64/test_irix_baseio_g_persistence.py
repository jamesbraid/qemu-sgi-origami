#!/usr/bin/env python3
#
# IRIX on the one-node Origin 2000's BaseIO-G: what the board keeps across
# an IRIX reboot and across a stopped QEMU
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The one-node Origin 2000's BaseIO-G, checked through IRIX 6.5.30 on
root-diags.qcow2 for what it keeps: parallel-port numbering, an nvram
variable and the Timekeeper clock across an IRIX reboot, and the
variable and the clock across a full stop and relaunch of QEMU on the
same NVRAM image and clock reference. test_irix_baseio_g_ports.py
checks the board's ports and explains why the two are separate files.

An archived capture of the clock across a stop is in the project
checkout (docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00).
"""

import calendar
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_net import HarnessError
from test_irix_origin2000_topologies import (LOGIN_BUDGET_SECONDS,
                                             _drive_to_shell, _output)
from test_irix_baseio_g_ports import BASEIO_G
import test_irix_origin2000_topologies as topologies

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


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

# A fixed guest date, set in UTC: 2030-01-01 20:00:00, the instant the
# archived clock capture set in Pacific time (project checkout,
# docs/boot-logs/irix.boot.2026-09-25T01:30:46-07:00). A fresh clock
# reference makes the M48T35 model follow the host clock, so a check
# that only compared elapsed times would pass even if the guest's write
# never reached the Timekeeper. The test therefore reads the date back
# and requires the set instant plus the host's elapsed time since the
# set, and then carries each reading forward the same way. Nothing here
# depends on the host's own date, so the check does not expire.
GUEST_DATE_SET = '-u 010120002030'
GUEST_DATE_SET_EPOCH = calendar.timegm((2030, 1, 1, 20, 0, 0))
GUEST_DATE_FMT = '%Y-%m-%d %H:%M:%S'
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


if QemuSystemTest is not None:

    class IrixBaseioGPersistence(QemuSystemTest):
        """Boots IRIX on the one-node Origin 2000 and checks what its
        BaseIO-G keeps across a reboot and a stopped QEMU."""

        # The topology file's launcher, shared rather than copied.
        _boot = topologies.IrixOrigin2000Topologies._boot

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
            the variable still reads back. The guest reads the date it set
            at once, and at each later reading has advanced by the host's
            elapsed time: across the reboot, and across the stop with the
            stopped interval included."""
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
                    ('sync', 'sync')):
                self.assertEqual(con.run(cmd, tag), 0, f'{cmd} -- {where}')
            self.assertEqual(con.run(f'date {GUEST_DATE_SET}', 'dateset'), 0,
                             f'date {GUEST_DATE_SET} -- {where}')
            host_at_set = time.time()
            guest_set, host_set = guest_epoch(con, 'dateread')
            self.assertLess(
                abs((guest_set - GUEST_DATE_SET_EPOCH) -
                    (host_set - host_at_set)), CLOCK_TOLERANCE_SECONDS,
                f'the guest read {guest_set - GUEST_DATE_SET_EPOCH}s from '
                f'the instant it set, {host_set - host_at_set:.0f}s after '
                f'setting it -- {where}')

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
            self.assertLess(
                abs((guest_before - guest_set) - (host_before - host_set)),
                CLOCK_TOLERANCE_SECONDS,
                f'across the reboot the guest clock advanced '
                f'{guest_before - guest_set}s while the host advanced '
                f'{host_before - host_set:.0f}s -- {where}')
            self.assertEqual(con.run('sync', 'sync2'), 0, where)
            vm.shutdown()
            time.sleep(STOPPED_SECONDS)

            vm, con, prom, log_path = self._boot(
                'origin2000-one-node', 2, 256, timekeeper,
                name='origin2000-one-node-baseio-g-second')
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
