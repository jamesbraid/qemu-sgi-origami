#!/usr/bin/env python3
#
# Unit tests for lib_irix_diag: the matrix manifest, the diagnostic
# verdicts, the shell-output parsers and the launch arguments
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_irix_diag.py`. The console
cases play a scripted guest over a socketpair, the way test_lib_net.py
does.
"""

import re
import socket
import struct
import tempfile
import threading
import unittest
from dataclasses import replace
from pathlib import Path

import lib_irix_diag as lib
from lib_irix_diag import (CHECKS, FLAVOURS, MANIFEST, REASONS, RUN,
                           ClockReading, clock_error, command_output,
                           device_numbers, diag_problems, drive_to_shell,
                           guest_date, launch_args, link_targets, od_bytes,
                           parse_manifest, parse_row, processor_count,
                           prom_facts, text, wav_nonzero_samples)
from lib_matrix import MatrixError
from lib_net import Console, HarnessError


HERE = Path(__file__).parent


def _line(row_id='origin200', cpus='2', mem='128', **cells):
    values = {check: RUN for check in CHECKS}
    values.update(mediaio='no-mediaio', rad1='no-mediaio',
                  scsi_card='no-scsi-card', olenet='no-olenet',
                  parallel='parallel-own-history')
    values.update(cells)
    return ' | '.join([row_id, cpus, mem] + [values[c] for c in CHECKS])


class ManifestTests(unittest.TestCase):

    def test_the_committed_manifest_parses(self):
        rows = parse_manifest()
        self.assertEqual(sorted(r.id for r in rows), sorted(FLAVOURS))

    def test_every_row_has_a_test_file_that_registers_it(self):
        registered = set()
        for path in HERE.glob('test_irix_diag*.py'):
            registered.update(re.findall(
                r"register_irix_diag_row\(\w+, '(\w+)'\)",
                path.read_text(encoding='utf-8')))
        self.assertEqual(registered, {r.id for r in parse_manifest()})

    def test_every_row_file_is_a_meson_target(self):
        meson = (HERE / 'meson.build').read_text(encoding='utf-8')
        for path in HERE.glob('test_irix_diag*.py'):
            target = path.stem[len('test_'):]
            self.assertRegex(meson, rf"(?m)^\s*'{target}'\s*:\s*\d+,")
            self.assertRegex(meson, rf"(?m)^\s*'{target}',\s*$")

    def test_a_row_parses_into_cells(self):
        row = parse_row('m', 1, _line())
        self.assertEqual((row.id, row.cpus, row.mem), ('origin200', 2, 128))
        self.assertTrue(row.runs('olmem'))
        self.assertFalse(row.runs('mediaio'))
        self.assertEqual(row.reason('mediaio'), REASONS['no-mediaio'])
        self.assertIsNone(row.reason('olmem'))

    def test_a_short_row_is_refused(self):
        with self.assertRaisesRegex(MatrixError, r'm:3: \d+ fields'):
            parse_row('m', 3, 'origin200 | 2 | 128 | run')

    def test_an_unknown_reason_is_refused(self):
        with self.assertRaisesRegex(MatrixError, 'known reason'):
            parse_row('m', 1, _line(oltape='because'))

    def test_an_unknown_flavour_is_refused(self):
        with self.assertRaisesRegex(MatrixError, 'no flavour'):
            parse_row('m', 1, _line(row_id='origin9000'))

    def test_a_bad_count_is_refused(self):
        with self.assertRaisesRegex(MatrixError, 'cpus is not an integer'):
            parse_row('m', 1, _line(cpus='two'))
        with self.assertRaisesRegex(MatrixError, 'mem must be positive'):
            parse_row('m', 1, _line(mem='0'))

    def test_a_check_the_flavour_has_nothing_for_is_refused(self):
        with self.assertRaisesRegex(MatrixError,
                                    'mediaio runs.*mediaio_ports'):
            parse_row('m', 1, _line(mediaio=RUN))

    def test_reboot_and_parallel_need_the_relaunch(self):
        for check in ('reboot', 'parallel'):
            with self.subTest(check=check):
                with self.assertRaisesRegex(MatrixError,
                                            f'{check} runs.*persistence'):
                    parse_row('m', 1, _line(
                        row_id='origin2000_one_node',
                        persistence='no-parallel', mediaio=RUN, rad1=RUN,
                        parallel=RUN if check == 'parallel' else
                        'no-parallel',
                        reboot=RUN if check == 'reboot' else 'no-parallel'))

    def test_a_duplicate_row_is_refused(self):
        with tempfile.NamedTemporaryFile('w', suffix='.matrix') as manifest:
            manifest.write(f'# header\n{_line()}\n\n{_line()}\n')
            manifest.flush()
            with self.assertRaisesRegex(MatrixError, r':4: duplicate id'):
                parse_manifest(manifest.name)


# Output shaped like the diagnostics' own, from the captures cited in the
# project checkout's irix-online-diagnostics notebook.
PASSES = {
    'oldisk': 'RSLT async-direct   PASS\n'
              'META                TOTAL             1              0\n',
    'olmem': ''.join(f'RSLT {name:<14} PASS\n' for name in (
        'tagram', 'l1cache', 'l2cache', 'addr-pattern', 'memaddr')) +
             'META                TOTAL             5              0\n',
    'oltape': 'Online Tape Test\nRSLT rw             PASS\n'
              'RSLT thrash         PASS\nRSLT block          PASS\n'
              'META                TOTAL             3              0\n',
    'olcdrom': 'REV olcdrom version 1.1\nRSLT olcdrom        PASS\n'
               'LOOP                Completed Loop 1 of 1\n',
    'olrti': 'RSLT internal       PASS\n',
    'olenet': 'META                enet-probe        1              0\n'
              'META                enet-info         1              0\n'
              'META                TOTAL             2              0\n',
}


class DiagVerdictTests(unittest.TestCase):

    def test_each_pass_transcript_is_clean(self):
        self.assertEqual(set(PASSES), set(lib.DIAGS))
        for name, output in PASSES.items():
            with self.subTest(diag=name):
                self.assertEqual(diag_problems(name, output), [])

    def test_colour_escapes_do_not_hide_a_pass(self):
        coloured = PASSES['olrti'].replace('PASS', '\x1b[32mPASS\x1b[0m')
        self.assertEqual(diag_problems('olrti', coloured), [])

    def test_a_missing_pass_line_is_named(self):
        output = PASSES['olmem'].replace('RSLT l2cache', 'RSLT l2cachX')
        self.assertEqual(diag_problems('olmem', output),
                         ['missing: olmem l2cache pass'])

    def test_a_nonzero_non_pass_count_fails(self):
        output = PASSES['oldisk'].replace('1              0',
                                          '1              1')
        self.assertTrue(diag_problems('oldisk', output))

    def test_a_failure_spelling_fails_even_beside_a_pass(self):
        output = PASSES['oltape'] + 'RSLT block          FAIL\n'
        self.assertEqual(diag_problems('oltape', output),
                         ['tripwire: oltape block failed'])

    def test_a_guest_panic_fails_any_diagnostic(self):
        for name, output in PASSES.items():
            with self.subTest(diag=name):
                self.assertTrue(diag_problems(name, output + 'PANIC\n'))

    def test_an_olcdrom_abort_fails(self):
        output = "olcdrom version 1.1\nABRT can't open device\n"
        self.assertTrue(diag_problems('olcdrom', output))


class ParserTests(unittest.TestCase):

    LS = ('crw-rw-rw-    1 root     sys         0,125 Sep 26 11:31 '
          '/dev/ttyd2\n'
          'crw-rw-rw-    1 root     sys         0,125 Sep 26 11:31 '
          '/hw/module/1/slot/io1/baseio/pci/4/tty/2/d\n'
          'lrwxr-xr-x    1 root     sys           18 Sep 26 11:31 '
          'keyboard -> /hw/input/keyboard\n')

    def test_device_numbers(self):
        self.assertEqual(device_numbers(self.LS), {
            '/dev/ttyd2': (0, 125),
            '/hw/module/1/slot/io1/baseio/pci/4/tty/2/d': (0, 125)})

    def test_a_line_after_a_kernel_warning_still_parses(self):
        console = (b"WARNING: CPU 5 hasn't seen a scheduler clock interrupt\n\r"
                   b'         Probable lost interrupt!\n\r'
                   b'crw-rw-rw-    1 root     sys         0,197 Sep 26 12:57 '
                   b'/hw/input1/keyboard\r\n')
        self.assertEqual(device_numbers(text(console)),
                         {'/hw/input1/keyboard': (0, 197)})

    def test_link_targets(self):
        self.assertEqual(link_targets(self.LS),
                         {'keyboard': '/hw/input/keyboard'})

    def test_processor_count_reads_both_forms(self):
        self.assertEqual(processor_count('2 180 MHZ IP27 Processors\n'), 2)
        self.assertEqual(processor_count('2 500 MHZ IP35 Processors\n'), 2)
        self.assertEqual(processor_count(
            'Processor 0: 180 MHZ IP27\nProcessor 1: 180 MHZ IP27\n'), 2)
        self.assertEqual(processor_count('no processors here'), 0)

    def test_od_bytes(self):
        self.assertEqual(od_bytes(' 1c f0 1c 32 f0 32\n'),
                         ['1c', 'f0', '1c', '32', 'f0', '32'])

    def test_od_bytes_ignores_an_interleaved_kernel_warning(self):
        listing = ("WARNING: CPU 0 hasn't seen a scheduler clock interrupt in "
                   '47 seconds, EPC 0xc0000000001ffba4\n'
                   '         Probable lost interrupt!\n\n'
                   ' 1c f0 1c\n 32 f0 32\n')
        self.assertEqual(od_bytes(listing),
                         ['1c', 'f0', '1c', '32', 'f0', '32'])

    def test_guest_date(self):
        self.assertEqual(guest_date('DATE=2030-01-01 12:00:00\n'),
                         1893499200)
        self.assertIsNone(guest_date('date: bad conversion\n'))

    def test_wav_nonzero_samples_skips_the_header(self):
        header = b'\x7f' * 44
        data = struct.pack('<6h', 0, 5, 0, -3, 0, 0)
        self.assertEqual(wav_nonzero_samples(header + data), 2)
        self.assertEqual(wav_nonzero_samples(header + b'\x00' * 100), 0)
        self.assertEqual(wav_nonzero_samples(b''), 0)


class ClockErrorTests(unittest.TestCase):

    def test_an_exact_clock_is_within_its_bound(self):
        before = ClockReading(1000, 50.0, 50.4)
        after = ClockReading(1100, 150.1, 150.3)
        error, bound = clock_error(before, after)
        self.assertLessEqual(abs(error), bound)
        self.assertLess(bound, 4)

    def test_a_clock_that_dropped_the_stop_is_outside_it(self):
        before = ClockReading(1000, 50.0, 50.4)
        # 100s passed on the host, 30 of them stopped; the guest saw 70.
        after = ClockReading(1070, 150.1, 150.3)
        error, bound = clock_error(before, after)
        self.assertGreater(abs(error), bound)
        self.assertAlmostEqual(error, -30, delta=1)

    def test_slow_round_trips_widen_the_bound(self):
        before = ClockReading(1000, 50.0, 60.0)
        after = ClockReading(1100, 150.0, 160.0)
        self.assertGreater(clock_error(before, after)[1], 10)


class LaunchArgsTests(unittest.TestCase):

    ASSETS = {'root': '/a', 'firmware': '/a/prom', 'disk': '/a/disk',
              'medium': '/a/cd'}
    PATHS = {'scratch': '/s', 'tape': '/s/tape', 'nvram': '/s/nv',
             'clock': '/s/clk', 'overlay': '/s/root.qcow2', 'wav': '/s/w',
             'ports': {n: f'/s/{n}' for n in (
                 'ioc3-b', 'chassis2-a', 'chassis2-b', 'mio-a', 'mio-b')}}

    def _args(self, row_id, **kwargs):
        row = next(r for r in parse_manifest() if r.id == row_id)
        return launch_args(FLAVOURS[row_id], row, self.ASSETS, self.PATHS,
                           **kwargs)

    def test_serial_lines_are_filled_up_to_the_last_port(self):
        _machine, argv = self._args('origin200_dual')
        serial = [argv[i + 1] for i, a in enumerate(argv) if a == '-serial']
        self.assertEqual(serial, ['chardev:ioc3-b'] + ['null'] * 4 +
                         ['chardev:chassis2-a', 'chardev:chassis2-b'])

    def test_each_ethernet_gets_its_own_network_and_address(self):
        _machine, argv = self._args('origin200_dual')
        nics = [argv[i + 1] for i, a in enumerate(argv) if a == '-nic']
        self.assertEqual(nics, [
            'user,id=net0,mac=08:00:69:0e:c6:01,net=10.0.2.0/24',
            'user,id=net1,mac=08:00:69:0e:c6:02,net=10.0.3.0/24'])

    def test_the_mediaio_ports_and_rad1_go_through_globals(self):
        _machine, argv = self._args('origin2000_one_node')
        globals_ = [argv[i + 1] for i, a in enumerate(argv) if a == '-global']
        self.assertEqual(globals_, ['sgi-rad1-audio.audiodev=rad1',
                                    'sgi-xio-baseio.mio-chardev-a=mio-a',
                                    'sgi-xio-baseio.mio-chardev-b=mio-b'])
        self.assertNotIn('-audio', argv)

    def test_a_flavour_without_rad1_has_no_audio(self):
        _machine, argv = self._args('origin200')
        self.assertEqual(argv[argv.index('-audio') + 1], 'none')

    def test_only_a_quick_relaunch_sets_the_dip_switches(self):
        machine, _argv = self._args('origin2000_one_node')
        self.assertNotIn('elsc-dip-switches', machine)
        machine, _argv = self._args('origin2000_one_node', quick_prom=True)
        self.assertTrue(machine.endswith(',elsc-dip-switches=1'))

    def test_the_disk_is_the_overlay_and_the_timekeeper_is_bound(self):
        _machine, argv = self._args('origin200')
        drives = [argv[i + 1] for i, a in enumerate(argv) if a == '-drive']
        self.assertIn('if=none,id=disk0,file=/s/root.qcow2,format=qcow2',
                      drives)
        self.assertIn('if=none,id=sgi-nvram0,file=/s/nv,format=raw', drives)
        self.assertIn('if=none,id=sgi-nvram0-clock,file=/s/clk,format=raw',
                      drives)

    def test_prom_facts_name_the_row_population(self):
        row = next(r for r in parse_manifest() if r.id == 'origin200_dual')
        facts = dict(prom_facts(FLAVOURS['origin200_dual'], row))
        self.assertRegex('4 CPUs on 2 nodes found.',
                         facts['the processors and nodes'])
        self.assertNotRegex('14 CPUs on 2 nodes found.',
                            facts['the processors and nodes'])
        self.assertNotRegex('Mem enabled             = 1128 MB',
                            facts['the enabled memory'])

    def test_the_flavours_match_the_quick_relaunch_contract(self):
        self.assertFalse(FLAVOURS['origin300'].dip_switches)
        flavour = replace(FLAVOURS['origin200'], dip_switches=False)
        row = next(r for r in parse_manifest() if r.id == 'origin200')
        machine, _argv = launch_args(flavour, row, self.ASSETS, self.PATHS,
                                     quick_prom=True)
        self.assertNotIn('elsc-dip-switches', machine)


class _ScriptedGuest(threading.Thread):
    """Answer each line the test sends with the next scripted reply."""

    def __init__(self, sock, greeting, replies):
        super().__init__(daemon=True)
        self.sock, self.greeting, self.replies = sock, greeting, replies

    def run(self):
        self.sock.sendall(self.greeting)
        buf = b''
        for reply in self.replies:
            while b'\n' not in buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    return
                buf += chunk
            line, buf = buf.split(b'\n', 1)
            self.sock.sendall(reply(line) if callable(reply) else reply)


def _marker_reply(output):
    """Echo the command the way a shell does, print output, and end with
    the marker lib_net's run() appended."""
    def reply(line):
        tag = re.search(rb'MARK""-(\w+)-', line).group(1)
        return line + b'\r\n' + output + b'MARK-' + tag + b'-0\r\n# '
    return reply


class ConsoleTests(unittest.TestCase):

    def _console(self, greeting, replies):
        ours, theirs = socket.socketpair()
        self.addCleanup(ours.close)
        self.addCleanup(theirs.close)
        _ScriptedGuest(theirs, greeting, replies).start()
        log = tempfile.NamedTemporaryFile(delete=False)
        log.close()
        con = Console(ours, log.name)
        self.addCleanup(con.close)
        return con

    def test_command_output_drops_the_echo_and_the_prompt(self):
        con = self._console(b'# ', [_marker_reply(b'# hello\r\nworld\r\n')])
        output, status = command_output(con, 'echo hello', 'x')
        self.assertEqual(status, 0)
        self.assertIn('hello', output)
        self.assertNotIn('echo', output)
        self.assertNotIn('# ', output)

    def test_drive_to_shell_times_each_phase(self):
        con = self._console(
            b'PROM output\r\nALL DIAGS PASSED.\r\nOption? ',
            [b'IRIX Release 6.5\r\nconsole login: ',
             b'TERM = (vt100) ',
             b'IRIS 1# ',
             b'# ',
             _marker_reply(b'')])
        phases = []
        prom = drive_to_shell(con, 30, phases)
        self.assertIn('ALL DIAGS PASSED', prom)
        self.assertNotIn('Option?', prom)
        self.assertEqual([name for name, _s in phases], [
            'PROM power-on to menu', 'IRIX kernel and init to login',
            'login to root shell'])

    AUTOBOOT = b'IP27 PROM\r\nStarting up the system...\r\nconsole login: '

    def test_drive_to_shell_follows_an_autoboot_on_a_relaunch(self):
        con = self._console(self.AUTOBOOT, [
            b'TERM = (vt100) ', b'IRIS 1# ', b'# ', _marker_reply(b'')])
        phases = []
        drive_to_shell(con, 30, phases, menu=False)
        self.assertEqual(phases[0][0],
                         'PROM autoboot, kernel and init to login')

    def test_a_power_on_boot_must_reach_the_menu(self):
        con = self._console(self.AUTOBOOT, [])
        with self.assertRaisesRegex(HarnessError, 'maintenance menu'):
            drive_to_shell(con, 2, [])


if __name__ == '__main__':
    unittest.main()
