#!/usr/bin/env python3
#
# Unit tests for lib_prom_smdk: the manifest, the status-table reading and
# the monitor dialogue
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import. The status-table
fixtures are the monitor's own bytes from the project checkout's runs
prom-20260927T115350-ae27ac (sct) and prom-20260927T121420-ccf678
(System_Level_Test); the dialogue cases play a scripted monitor over a
socketpair.
"""

import os
import re
import socket
import tempfile
import threading
import unittest
from pathlib import Path

import lib_prom_smdk as lib
from lib_matrix import MatrixError
from lib_net import Console, HarnessError
from lib_prom_smdk import (DIAG_ORDER, DIAGS, REASONS, RUN, SmdkDriver,
                           StatusRow, TftpRoot, bound_end_pass,
                           package_files, parse_manifest, parse_row,
                           render_terminal, row_verdict, status_rows,
                           table_is_idle, verdict)


HERE = Path(__file__).parent

HEADER = (b'\x1b[H\x1b[2JSystem Logs: no system errors logged\r\n'
          b'#  Name        Status                   Life       Pass       '
          b'Fail  % Complete\r\n')
HOMES = b'\x1b[1;1H' * 3

# sct's row printed running, then its counters and its new status word
# painted onto it cell by cell.
SCT_PAINTED = (
    HEADER +
    b'1  sct         running            Min: 0x00000426 0x00000000 0x0000'
    b'      0\r\n                                  Max: 0x00000426 '
    b'0x00000000\r\n\x1b[1;1H\x1b[1;1H\x1b[3;16HP\x1b[3;17Ha\x1b[3;18Hs'
    b'\x1b[3;19Hs\x1b[3;20He\x1b[3;21Hd\x1b[3;22H \x1b[3;48H5\x1b[3;49Ha'
    b'\x1b[3;60H1\x1b[3;74H1\x1b[3;75H0\x1b[3;76H0\x1b[4;48H5\x1b[4;49Ha'
    b'\x1b[4;60H1' + HOMES)

SLT_PASSED = (
    HEADER +
    b'1  System_Leve Passed             Min: 0x00000001 0x00000001 0x0000'
    b'      100\r\n                                  Max: 0x00000001 '
    b'0x00000001\r\n' + HOMES)


def _row(status, fails=None):
    return StatusRow(1, 'xbg', status, 3, 2, fails, 100)


class ManifestTests(unittest.TestCase):

    def _line(self, row_id='origin300', cpus='2', budget='2880', **cells):
        values = dict.fromkeys(DIAG_ORDER, RUN)
        values.update(cells)
        return ' | '.join([row_id, cpus, budget] +
                          [values[d] for d in DIAG_ORDER])

    def test_the_committed_manifest_parses(self):
        rows = parse_manifest()
        self.assertEqual([r.id for r in rows],
                         ['origin300', 'origin300_4cpu', 'origin300_pair'])

    def test_every_row_has_a_registered_meson_target(self):
        meson = (HERE / 'meson.build').read_text(encoding='utf-8')
        registered = set()
        for path in HERE.glob('test_prom_smdk_origin300*.py'):
            target = path.stem[len('test_'):]
            registered.update(re.findall(
                r"register_smdk_row\(\w+,\s*'(\w+)'\)",
                path.read_text(encoding='utf-8')))
            timeout = re.search(rf"(?m)^\s*'{target}'\s*:\s*(\d+),", meson)
            self.assertIsNotNone(timeout, target)
            self.assertRegex(meson, rf"(?m)^\s*'{target}',\s*$")
            for row_id in re.findall(r"register_smdk_row\(\w+,\s*'(\w+)'\)",
                                     path.read_text(encoding='utf-8')):
                budget = lib.manifest_row(row_id).budget
                self.assertGreaterEqual(int(timeout.group(1)), budget + 120)
        self.assertEqual(registered, {r.id for r in parse_manifest()})

    def test_a_row_parses_into_cells(self):
        row = parse_row('m', 1, self._line(ndir='ndir-directory-state'))
        self.assertEqual((row.id, row.cpus, row.budget),
                         ('origin300', 2, 2880))
        self.assertTrue(row.runs('sct'))
        self.assertEqual(row.reason('ndir'), REASONS['ndir-directory-state'])

    def test_bad_rows_are_refused(self):
        for line, message in (
                ('origin300 | 2 | 2880 | run', 'fields'),
                (self._line(row_id='origin9000'), 'no population'),
                (self._line(cpus='4'), 'has 2 processors, not 4'),
                (self._line(budget='soon'), 'budget is not an integer'),
                (self._line(xbg='because'), 'known reason')):
            with self.subTest(message=message):
                with self.assertRaisesRegex(MatrixError, message):
                    parse_row('m', 1, line)

    def test_a_row_needs_the_files_its_diagnostics_fetch(self):
        row = parse_row('m', 1, self._line(
            sct='cpu-test-compare', nmem='cpu-test-compare',
            ndir='cpu-test-compare', scct='cpu-test-compare',
            CPU_Test='cpu-test-compare', io8bt='cpu-test-compare'))
        self.assertEqual(package_files(row),
                         ['smdk', 'iocfg', 'System_Level_Test',
                          'SLT_Cmd_Option_File', 'xbg'])

    def test_every_file_has_a_recorded_digest(self):
        for row in parse_manifest():
            for name in package_files(row):
                self.assertRegex(lib.PACKAGE_SHA256[name], r'^[0-9a-f]{64}$')


class StatusTableTests(unittest.TestCase):

    def test_the_render_applies_the_in_place_status_update(self):
        rows = status_rows(SCT_PAINTED)
        self.assertEqual(len(rows), 1)
        self.assertEqual((rows[0].name, rows[0].status, rows[0].life,
                          rows[0].passes, rows[0].fails, rows[0].complete),
                         ('sct', 'Passed', 0x45a, 1, 0, 100))

    def test_the_raw_bytes_alone_still_say_running(self):
        self.assertIn(b'sct         running', SCT_PAINTED)
        self.assertEqual(verdict(SCT_PAINTED, 'sct'), 'pass')

    def test_a_later_screen_replaces_an_earlier_one(self):
        running = HEADER + b'1  sct         running\r\n' + HOMES
        self.assertEqual(verdict(running, 'sct'), 'incomplete')
        self.assertEqual(verdict(running + SCT_PAINTED, 'sct'), 'pass')
        self.assertEqual(verdict(SCT_PAINTED + running, 'sct'), 'incomplete')

    def test_a_cut_name_lists_the_long_diagnostic(self):
        self.assertEqual(verdict(SLT_PASSED, 'System_Level_Test'), 'pass')
        self.assertEqual(verdict(SLT_PASSED, 'System_Lev'), 'incomplete')

    def test_only_the_named_row_answers(self):
        table = (HEADER +
                 b'1  iocfg       Passed             Min: 0x00000001 '
                 b'0x00000001 0x0000      100\r\n'
                 b'2  io8bt       running\r\n' + HOMES)
        self.assertEqual(verdict(table, 'io8bt'), 'incomplete')
        self.assertFalse(table_is_idle(table))
        self.assertEqual(verdict(table, 'xbg'), 'incomplete')

    def test_an_empty_table_is_idle(self):
        self.assertTrue(table_is_idle(HEADER + HOMES))
        self.assertTrue(table_is_idle(SCT_PAINTED))

    def test_row_verdicts(self):
        self.assertEqual(row_verdict(_row('Passed')), 'pass')
        self.assertEqual(row_verdict(_row('Failed', 1)), 'fail')
        self.assertEqual(row_verdict(_row('Done', 0)), 'pass')
        self.assertEqual(row_verdict(_row('Done', 1)), 'fail')
        # Done before its counters are painted says nothing yet.
        self.assertEqual(row_verdict(_row('Done')), 'incomplete')
        self.assertEqual(row_verdict(_row('running', 0)), 'incomplete')
        self.assertEqual(row_verdict(_row('Stopped', 0)), 'incomplete')

    def test_the_render_keeps_rows_and_erases(self):
        self.assertEqual(render_terminal(b'ab\r\ncd\x1b[1;2HX\x1b[K'),
                         'aX\ncd\n')
        self.assertEqual(render_terminal(b'x\x1b[H\x1b[2Jy'), 'y\n')
        self.assertEqual(render_terminal(b'ab\bc\x07'), 'ac\n')


class OptionFileTests(unittest.TestCase):

    def test_the_end_pass_is_replaced(self):
        self.assertEqual(bound_end_pass(b'-t A -s 1 -e 50000000\n', 1),
                         b'-t A -s 1 -e 1\n')

    def test_a_missing_end_pass_is_appended(self):
        self.assertEqual(bound_end_pass(b'-t A -s 1\n', 1),
                         b'-t A -s 1 -e 1\n')

    def test_an_e_inside_another_word_is_left(self):
        self.assertEqual(bound_end_pass(b'-t io-e 5 -e 9\n', 2),
                         b'-t io-e 5 -e 2\n')

    def test_only_slt_fetches_a_file(self):
        self.assertEqual({n for n, d in DIAGS.items() if d.fetch},
                         {'System_Level_Test'})


class TftpRootTests(unittest.TestCase):

    def test_serving_swaps_the_boot_entry_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = {}
            for name, data in (('smdk', b'monitor'), ('iocfg', b'io')):
                files[name] = os.path.join(tmp, name)
                Path(files[name]).write_bytes(data)
            root = TftpRoot(os.path.join(tmp, 'tftp'), files)
            entry = Path(tmp, 'tftp', 'smdk')
            self.assertEqual(entry.read_bytes(), b'monitor')
            self.assertTrue(root.holds('smdk'))
            root.serve('iocfg')
            self.assertEqual(entry.read_bytes(), b'io')
            self.assertFalse(root.holds('smdk'))
            self.assertFalse(root.holds('xbg'))
            root.edit('iocfg', lambda data: data + b'!')
            root.serve('iocfg')
            self.assertEqual(entry.read_bytes(), b'io!')
            # The package copies were edited, not the originals.
            self.assertEqual(Path(files['iocfg']).read_bytes(), b'io')


class _Monitor(threading.Thread):
    """Play the PROM and the monitor: answer each line or command the
    driver sends with the next scripted reply, recording what it sent."""

    def __init__(self, sock, greeting, replies, on_pause=None):
        super().__init__(daemon=True)
        self.sock, self.greeting, self.replies = sock, greeting, replies
        self.sent = []

    def run(self):
        self.sock.sendall(self.greeting)
        buf = b''
        for reply in self.replies:
            while not re.search(rb'[\r\n]', buf):
                chunk = self.sock.recv(4096)
                if not chunk:
                    return
                buf += chunk
            line, buf = re.split(rb'[\r\n]', buf, maxsplit=1)
            self.sent.append(line)
            self.sock.sendall(reply)


class DriverTests(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        files = {}
        for name in ('smdk', 'iocfg', 'sct'):
            files[name] = os.path.join(tmp.name, name)
            Path(files[name]).write_bytes(name.encode())
        self.tftp = TftpRoot(os.path.join(tmp.name, 'tftp'), files)
        self.log = os.path.join(tmp.name, 'console.log')
        self.pauses = []

    def _driver(self, greeting, replies):
        ours, theirs = socket.socketpair()
        self.addCleanup(ours.close)
        self.addCleanup(theirs.close)
        monitor = _Monitor(theirs, greeting, replies)
        monitor.start()
        con = Console(ours, self.log)
        self.addCleanup(con.close)

        def paused(action):
            self.pauses.append(self.tftp.holds('smdk'))
            action()

        return SmdkDriver(con, self.tftp, paused, sleep=lambda s: None), \
            monitor

    BOOT = [b'>> ', b'>> ',
            b'Obtaining smdk from server \n\r137372   \x08|\x08\x08+15732 '
            b'entry: 0xa800000000114490\n\rSMDK SGI Version 6.124\r\n'
            b'Obtaining iocfg from server \n\r   smdk>']

    def test_a_boot_stages_iocfg_before_the_discovery_fetch(self):
        driver, monitor = self._driver(b'Option? ', self.BOOT)
        driver.boot(10)
        monitor.join(5)
        self.assertEqual(monitor.sent, [b'5', b'unsetenv netaddr',
                                        b'boot -f bootp()/smdk'])
        # Paused once, while the entry still held the monitor.
        self.assertEqual(self.pauses, [True])
        self.assertTrue(self.tftp.holds('iocfg'))

    def test_a_boot_that_lost_a_processor_is_refused(self):
        boot = self.BOOT[:2] + [self.BOOT[2].replace(
            b'   smdk>', b'vcpu 1 is not responding\r\nsmdk>')]
        driver, _monitor = self._driver(b'Option? ', boot)
        with self.assertRaisesRegex(lib.BootDegraded, 'vcpu 1'):
            driver.boot(10)

    def test_a_run_is_polled_to_its_own_rows_verdict(self):
        idle = HEADER + HOMES
        running = HEADER + b'1  sct         running\r\n' + HOMES
        replies = self.BOOT + [
            idle,                                   # settle's ds
            b'',                                    # the ESC's CR
            b'Obtaining sct from server \n\r   smdk>',
            running,                                # first poll
            b'',                                    # the ESC's CR
            SCT_PAINTED,                            # second poll
        ]
        driver, monitor = self._driver(b'Option? ', replies)
        driver.boot(10)
        start = driver.run('sct', timeout=10)
        self.assertTrue(self.tftp.holds('sct'))
        result, text = driver.poll('sct', start, 10)
        monitor.join(5)
        self.assertEqual(result, 'pass')
        self.assertIn('Passed', text)
        self.assertEqual(monitor.sent[3:], [b'ds', b'\x1b', b'run sct',
                                            b'ds', b'\x1b', b'ds'])

    def test_a_follow_on_fetch_is_awaited_without_typing(self):
        replies = self.BOOT[:2] + [self.BOOT[2] +
                                   b'Obtaining SLT_Cmd_Option_File from '
                                   b'server \n\r']
        driver, monitor = self._driver(b'Option? ', replies)
        driver.boot(10)
        driver.await_fetch('SLT_Cmd_Option_File', 10)
        with self.assertRaisesRegex(HarnessError, 'to fetch sct'):
            driver.await_fetch('sct', 1)
        monitor.join(5)
        self.assertEqual(len(monitor.sent), 3)

    def test_a_follow_on_fetch_already_made_is_refused(self):
        replies = self.BOOT
        driver, _monitor = self._driver(b'Option? ', replies)
        driver.boot(10)
        since = driver.pos
        driver.con.buf += b'Obtaining sct from server'
        with self.assertRaisesRegex(HarnessError, 'before it could be'):
            driver.serve_follow_on('sct', since)


if __name__ == '__main__':
    unittest.main()
