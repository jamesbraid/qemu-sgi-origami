#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks, one boot per machine
# flavour: boot the installed IRIX disk with SGI's online diagnostics,
# log in, and run every check irix-diag-matrix marks for the flavour
#
# SPDX-License-Identifier: BSD-3-Clause
"""Run IRIX online diagnostics on the emulated machine and check each
flavour's supported commands and IOC3 ports.

`lib_irix_diag.FLAVOURS` defines how each configuration boots and which
checks it runs. A flavour boots once; checks continue after an individual
failure, and the test reports all results in a summary file. Reboot,
persistence, and parallel checks may restart QEMU with the same NVRAM and
disk images. Each run uses a private disk overlay.

The device names in `FLAVOURS` apply to the supplied disk images, whose
`/etc/ioconfig.conf` determines IRIX device numbering. They may differ on
other images. Some checks are omitted when the required device or network
topology is not modeled.

`olrti` runs in internal-loopback mode. `olvht` and `linktest` are omitted
because the test topology has no HIPPI channel or router network.
"""

import calendar
import json
import math
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_irix_diag import (FLAVOURS, MANIFEST, CheckFailed, ClockReading,
                           NoConsole, OutOfTime, clock_error, device_numbers,
                           diag_problems, guest_date, launch_args,
                           link_targets, od_bytes, parse_row,
                           command_output, drive_to_shell,
                           processor_count, prom_facts, run_checks, text,
                           tick_warnings,
                           wav_nonzero_samples)
from lib_matrix import MatrixError, matrix_rows
from lib_net import HarnessError, IrixConsole
import lib_origin300
from lib_prom import FORK_ROOT, run_record
from lib_prom_sash import RELEASES

try:
    from qemu.machine.machine import QEMUMachineError
    from qemu_test import QemuSystemTest, get_qemu_img
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
    from qemu_test.asset import LocalAsset
except (ImportError, RuntimeError):
    QemuSystemTest = None


# Each wait is capped by its own limit and by the remaining flavour budget.

# PROM power-on, kernel and init to a root shell: 1075s at most (the
# four-node shoehorn, 391s PROM and 685s IRIX), 125 to 400s on the Origin
# 200 and one-node machines.
BOOT_BUDGET_SECONDS = 1500

# Ordinary shell commands: hinv, netstat and the like took 22s at most.
STEP_TIMEOUT_SECONDS = 120

# Per-diagnostic time limits, including longer limits for disk, memory,
# CD-ROM, serial, stress, and processor tests.
DIAG_TIMEOUT_SECONDS = {
    'olrti': 120,
    'olenet': 120,
    'oldisk': 1200,
    'olmem': 1800,
    'oltape': 120,
    'olcdrom': 900,
    'olpci': 120,
    'olperi': 120,
    'olsio': 900,
    'olusb': 120,
    'olvst': 180,
    'pandora': 2700,
    'torpedo': 7800,
}

# Run diagnostics without color and keep their tagged result lines.
OPERATOR = '-nocolor -operator'

# Run olperi on processor 0 with a fixed seed, instruction count, and loop.
OLPERI_ARGS = '-m -r 0 -s 222 -i 1024 -il 1'

# After a timed-out diagnostic is interrupted it still prints its summary
# before the shell answers again.
RESYNC_TIMEOUT_SECONDS = 180

# sfplay of the shipped confidence tone. Without icount the model's audio
# stream stretches in time, so the check asserts samples arrived, not a
# duration.
CONFIDENCE_TONE = '/usr/lib/Confidence/audioOut44.aiff'
# One play of that tone, 6.757 s of 44.1 kHz mono, reaches the stereo
# output as this many sample words; the capture must hold at least that
# many nonzero ones, not merely one.
TONE_SAMPLE_WORDS = int(6.757 * 44100) * 2
# Measured 39 to 171s.
SFPLAY_TIMEOUT_SECONDS = 600

# From IRIX's reboot command to the PROM's banner: 13 to 46s measured.
REBOOT_TIMEOUT_SECONDS = 180

# The clock check allows the mean console round trip of its two
# readings, 3s of whole-second resolution (lib_irix_diag.clock_error),
# and CLOCK_ALLOWANCE_SECONDS for the guest's own time, which it keeps
# from the Timekeeper read at boot: on a timer that misses no Compare
# match it measured +0.2 to -2.8s off the host across a stop and
# relaunch. A clock that dropped the stopped interval is off by at least
# the whole stop, so the stop is three times that tolerance, sized from
# the round trip measured before it, and never under STOPPED_SECONDS.
CLOCK_ALLOWANCE_SECONDS = 3
STOPPED_SECONDS = 25

# A fixed guest date, set in UTC: 2030-01-01 20:00:00. A fresh clock
# reference makes the Timekeeper follow the host clock, so the check reads
# the date back at once and requires the set instant plus the host's
# elapsed time since the set; nothing depends on the host's own date.
GUEST_DATE_SET = '-u 010120002030'
GUEST_DATE_SET_EPOCH = calendar.timegm((2030, 1, 1, 20, 0, 0))

# The nvram variable written before the reboot and read after the
# relaunch: the graphics textport's background, which every PROM here
# carries (the IP35 PROM has no speaker volume) and none of these
# serial-console machines uses.
NVRAM_VARIABLE = ('pagecolor', '2a2a2a')

# "a" then "b", each pressed and released: the PS/2 make code of each
# letter (0x1c, 0x32) and its break code, 0xf0 then the make code. Both
# letters send the same bytes in scan-code sets 2 and 3.
TYPED_KEYS = ('a', 'b')
TYPED_BYTES = ['1c', 'f0', '1c', '32', 'f0', '32']

MEDIUM_REL = os.path.join('irix', '6.5.30', 'olcdrom-medium.raw')
MEDIUM_SHA256 = \
    'ba0a1466d7125cd350ee4f047677d7a3d9f8a8ed8b13469c92042cfa0bd07829'

IP27_FIRMWARE_SPEC = (
    FLAVOURS['origin200'].firmware, RELEASES['6.5.30']['prom_sha256'])
IP27_DIAG_DISK_SPEC = (
    FLAVOURS['origin200'].disk,
    '82e394e28cbe1bf92aa08f01216636bdff7a0b224d15166f321e1f075b253e66')
IP35_FIRMWARE_SPEC = (lib_origin300.FIRMWARE_REL,
                      lib_origin300.FIRMWARE_SHA256)
IP35_DIAG_DISK_SPEC = (
    FLAVOURS['origin300'].disk,
    '150cf2c125cb3bd55764acfeec00804899ff3140cc536cf63a254765faa29390')
DIAG_MEDIUM_SPEC = (MEDIUM_REL, MEDIUM_SHA256)
IP35_SPD_3_SPEC = lib_origin300.SPD_INPUTS[0][1:]
IP35_SPD_5_SPEC = lib_origin300.SPD_INPUTS[1][1:]


class _Console(IrixConsole):
    STEP_TIMEOUT = STEP_TIMEOUT_SECONDS


class _Launch:
    """One QEMU process of a flavour's run: its VM, console and log."""

    def __init__(self, vm, con, log_path, prom):
        self.vm = vm
        self.con = con
        self.log_path = log_path
        self.prom = prom

    def output(self, cmd, tag, timeout=None):
        return command_output(self.con, cmd, tag, timeout)


def _manifest_line(row_id):
    for lineno, line in matrix_rows(MANIFEST):
        if line.split('|', 1)[0].strip() == row_id:
            return lineno, line
    raise MatrixError(f'{MANIFEST} has no row {row_id!r}')


if QemuSystemTest is not None:

    class IrixDiagMatrix(QemuSystemTest):
        """One flavour's boot, checks and verdict. The test methods come
        from register_irix_diag_row()."""

        clock_before = None
        failed_consoles = ()
        rebooted_at = None
        stopped = 0
        tags = 0

        # -- launching ---------------------------------------------------

        def _prepare(self, flavour):
            self.assets = {
                'root': os.environ['QEMU_TEST_SGI_ASSET_DIR'],
                'firmware': self.ASSET_FIRMWARE.fetch(),
                'disk': self.ASSET_DISK.fetch(),
                'medium': self.ASSET_MEDIUM.fetch(),
            }
            if flavour.origin300:
                self.assets['origin300_spd'] = (
                    self.ASSET_SPD_3.fetch(), self.ASSET_SPD_5.fetch())
            scratch = self.scratch_file()
            os.makedirs(scratch, exist_ok=True)
            self.paths = {name: os.path.join(scratch, leaf)
                          for name, leaf in (('tape', 'oltape.tap'),
                                             ('nvram', 'nvram0.raw'),
                                             ('clock', 'nvram0.clock'),
                                             ('overlay', 'root.qcow2'),
                                             ('wav', 'rad1.wav'))}
            self.paths['scratch'] = scratch
            self.paths['ports'] = {
                port.name: os.path.join(scratch, f'{port.name}.out')
                for port in flavour.serial_ports + flavour.mediaio_ports}
            # A fresh tape for oltape to write, and a zeroed Timekeeper.
            for name, size in (('tape', 0), ('nvram', 0x8000),
                               ('clock', 16)):
                with open(self.paths[name], 'wb') as image:
                    image.write(bytes(size))
            # The relaunch must see what the first boot wrote to the
            # disk, so both open one overlay of the golden image.
            subprocess.run(
                [get_qemu_img(self), 'create', '-q', '-f', 'qcow2',
                 '-F', 'qcow2', '-b', self.assets['disk'],
                 self.paths['overlay']], check=True)

        def _launch(self, name, relaunch=False):
            machine, argv = launch_args(self.flavour, self.row, self.assets,
                                        self.paths, quick_prom=relaunch)
            vm = self.get_vm(name=name)
            vm.set_machine(machine)
            vm.set_console()
            # Start paused: the four-node PROM can reset the machine
            # within seconds of power-on, and the RESET event that raises
            # can reach the QMP client ahead of its capabilities reply.
            vm.add_args(*argv, '-S')
            vm.launch()
            vm.cmd('cont')
            self.addCleanup(lambda: vm.shutdown(
                hard=vm.exitcode() is not None))
            log_path = self.log_file(f'{name}-console.log')
            open(log_path, 'wb').close()
            con = _Console(vm.console_socket, log_path,
                           exited=lambda: vm.exitcode() is not None)
            self.addCleanup(con.close)
            phases = []
            try:
                prom = drive_to_shell(con, self._left(BOOT_BUDGET_SECONDS),
                                      phases, menu=not relaunch)
            except (HarnessError, OutOfTime):
                # Keep what a boot that never reached a shell printed, so
                # the timer check still counts it.
                self.failed_consoles.append(con)
                raise
            finally:
                for phase, seconds in phases:
                    self._phase(f'{name}: {phase}', seconds)
            return _Launch(vm, con, log_path, prom)

        def _phase(self, name, seconds):
            self.phases.append((name, seconds))
            self.log.info('%s: %.1fs', name, seconds)

        # -- the run -----------------------------------------------------

        def _run_row(self, row_id):
            started = time.monotonic()
            lineno, line = _manifest_line(row_id)
            try:
                self.row = parse_row(MANIFEST, lineno, line)
            except MatrixError as exc:
                self.fail(str(exc))
            # Every wait below is cut to what is left before this, so a
            # slow run still writes its summary before meson's timeout.
            self.deadline = started + self.row.budget
            self.flavour = FLAVOURS[self.row.id]
            self.phases, self.results = [], {}
            self.failed_consoles = []
            self._prepare(self.flavour)

            try:
                self.first = self._launch(self.row.id)
                self.here = self.first
                self.results = run_checks(self.row, self._run_check,
                                          self._resync)
            except (HarnessError, OutOfTime) as exc:
                console = self.log_file(self.row.id + '-console.log')
                self.results = {
                    check: ('not run', f'the first boot: {exc} -- console '
                            f'log: {console}', 0) if self.row.runs(check)
                    else ('n/a', self.row.reason(check), 0)
                    for check in self.row.cells}
            self._phase('whole run', round(time.monotonic() - started, 1))
            self._report()

        def _run_check(self, check):
            began = time.monotonic()
            status = 'fail'
            try:
                getattr(self, f'_check_{check}')()
                status = 'pass'
            finally:
                self._phase(f'check {check} ({status})',
                            round(time.monotonic() - began, 1))

        def _left(self, cap):
            """cap, or what is left of the row's budget if that is less.
            Raises OutOfTime once nothing is left."""
            left = self.deadline - time.monotonic()
            if left < 1:
                raise OutOfTime(f'{self.row.id} spent its {self.row.budget}s '
                                'budget')
            return min(cap, left)

        def _resync(self, check):
            """After a timed-out command, interrupt it and see whether the
            shell answers again, so one hung check does not time out every
            check after it."""
            if self.here is None:
                return False
            # A failed reboot or warm boot leaves no shell to
            # resynchronise; the persistence check stops the machine and
            # relaunches it.
            if check in ('reboot', 'warm_boot'):
                return True
            try:
                self.here.con.sock.sendall(b'\x03\n')
                self.here.con.run('true', self._tag(f'resync-{check}'),
                                  self._left(RESYNC_TIMEOUT_SECONDS))
            except (HarnessError, OutOfTime, OSError):
                return False
            return True

        def _report(self):
            summary = self.log_file(f'{self.row.id}-summary.json')
            with open(summary, 'w', encoding='utf-8') as out:
                json.dump({
                    'schema': 1, 'flavour': self.row.id,
                    'results': {c: {'status': s, 'detail': d, 'seconds': t}
                                for c, (s, d, t) in self.results.items()},
                    'phases': self.phases,
                    # The disk and medium by path and size: hashing
                    # 2.7 GB after every run would add minutes, and the
                    # medium's digest is already checked before launch.
                    'identity': run_record(FORK_ROOT, self.qemu_bin,
                                           prom=self.assets['firmware']),
                    'disks': {name: {'path': self.assets[name],
                                     'size_bytes': os.path.getsize(
                                         self.assets[name])}
                              for name in ('disk', 'medium')},
                }, out, indent=2)
                out.write('\n')
            failed = [f'{c}: {d}' for c, (s, d, _t) in self.results.items()
                      if s in ('fail', 'not run')]
            if failed:
                self.fail(f'{self.row.id}: ' + '; '.join(failed) +
                          f' (summary: {summary})')

        def _expect(self, condition, message):
            if not condition:
                raise CheckFailed(
                    f'{message} -- console log: {self.here.log_path}')

        def _tag(self, tag):
            """tag made unique in this session: lib_net matches a command's
            end marker anywhere in the console buffer, so a repeated tag
            would match the earlier command's marker."""
            self.tags += 1
            return f'{tag}-{self.tags}'

        def _out(self, cmd, tag, timeout=STEP_TIMEOUT_SECONDS):
            return self.here.output(cmd, self._tag(tag),
                                    self._left(timeout))[0]

        def _run(self, cmd, tag, timeout=STEP_TIMEOUT_SECONDS):
            self._expect(self.here.con.run(cmd, self._tag(tag),
                                           self._left(timeout)) == 0,
                         f'{cmd!r} failed')

        # -- the checks, in CHECKS order ----------------------------------

        def _check_prom(self):
            for what, pattern in prom_facts(self.flavour, self.row):
                self._expect(re.search(pattern, self.first.prom),
                             f'the PROM did not report {what}')

        def _check_inventory(self):
            hinv = self._out('hinv -v', 'hinv')
            self._expect(processor_count(hinv) == self.row.cpus,
                         f'hinv does not list {self.row.cpus} processors')
            self._expect(re.search(
                rf'Main memory size: {self.row.mem} Mbytes', hinv),
                f'hinv does not list {self.row.mem} MB')
            for what, pattern in self.flavour.hinv_facts:
                self._expect(re.search(pattern, hinv),
                             f'hinv -v lists no {what}')

        def _check_ethernet(self):
            netstat = self._out('netstat -ian', 'netstat')
            for eth in self.flavour.ethernets:
                # A trailing * marks an interface that is down.
                self._expect(re.search(rf'(?m)^{eth.ifname}\*?\s', netstat),
                             f'netstat -ian lists no {eth.ifname}')
                self._expect(eth.mac in netstat,
                             f'{eth.ifname} does not report {eth.mac}')
                self._run(f'ifconfig {eth.ifname} alias {eth.subnet}.15 '
                          'netmask 255.255.255.0 up', f'alias-{eth.ifname}')
                ping = self._out(f'ping -c 5 {eth.subnet}.2',
                                 f'ping-{eth.ifname}')
                self._expect(
                    re.search(r'\b5 packets transmitted, 5 packets received',
                              ping),
                    f'ping over {eth.ifname}: {ping!r}')

        def _write_ports(self, ports):
            """Prove each port's /dev name is its intended vertex by
            device number, write a line to it, and wait for the line to
            reach the port's host file."""
            # One listing per port: on the two-chassis Origin 300 a single
            # listing of four ports, a 260-character line, reached the
            # shell with its end mangled and its end marker never printed.
            numbers = {}
            for port in ports:
                numbers.update(device_numbers(self._out(
                    f'ls -lL {port.dev} {port.vertex}', f'tty-{port.name}')))
            for port in ports:
                self._expect(port.vertex in numbers, f'no {port.vertex}')
                self._expect(numbers.get(port.dev) == numbers[port.vertex],
                             f'{port.dev} is not {port.vertex}')
                self._run(f'echo origami-{port.name} > {port.dev}',
                          f'write-{port.name}')
            deadline = time.monotonic() + 30
            while True:
                missing = []
                for port in ports:
                    with open(self.paths['ports'][port.name], 'rb') as out:
                        if f'origami-{port.name}'.encode() not in out.read():
                            missing.append(port.dev)
                if not missing or time.monotonic() > deadline:
                    break
                time.sleep(1)
            self._expect(not missing,
                         f'lines written to {missing} never reached the host')

        def _check_serial(self):
            self._write_ports(self.flavour.serial_ports)

        def _check_mediaio(self):
            self._write_ports(self.flavour.mediaio_ports)

        def _check_rad1(self):
            self._run(f'/usr/sbin/sfplay {CONFIDENCE_TONE}', 'sfplay',
                      SFPLAY_TIMEOUT_SECONDS)
            with open(self.paths['wav'], 'rb') as wav:
                samples = wav_nonzero_samples(wav.read())
            self.log.info('RAD1 playback: %d nonzero samples', samples)
            self._expect(samples >= TONE_SAMPLE_WORDS,
                         f'RAD1 playback reached {self.paths["wav"]} as '
                         f'{samples} nonzero samples, not the tone\'s '
                         f'{TONE_SAMPLE_WORDS}')

        def _check_keyboard(self):
            for index, kbd in enumerate(self.flavour.keyboards):
                self._type_on(kbd, index)

        def _type_on(self, kbd, index):
            """Prove kbd's input devices are its card's PS/2 keyboard and
            mouse, then type on the keyboard through QEMU and read the scan
            codes back."""
            links = link_targets(self._out(f'ls -l {kbd.dev_dir}',
                                           f'devinput{index}'))
            devices = device_numbers(self._out(
                f'ls -l {kbd.hw_dir}/keyboard {kbd.hw_dir}/mouse '
                f'{kbd.card}/pckb {kbd.card}/pcms', f'hwinput{index}'))
            # The input device IRIX names for the card must be the card's
            # own PS/2 ports: same device numbers as its pckb and pcms.
            for alias, leaf in (('keyboard', 'pckb'), ('mouse', 'pcms')):
                self._expect(links.get(alias) == f'{kbd.hw_dir}/{alias}',
                             f'{kbd.dev_dir}/{alias} is not '
                             f'{kbd.hw_dir}/{alias}')
                card = [n for p, n in devices.items()
                        if p.endswith(f'/{leaf}')]
                self._expect(card, f'no {leaf} under {kbd.card}')
                self._expect(devices.get(f'{kbd.hw_dir}/{alias}') == card[0],
                             f'{kbd.hw_dir}/{alias} is not '
                             f'{kbd.card}/{leaf}')
            # Capture raw bytes as they arrive. Piping dd into od hides
            # partial reads because od buffers output until dd finishes.
            self._run('rm -f /tmp/kbd.raw; kbd_wait=30; '
                      f'(dd if={kbd.dev_dir}/keyboard bs=1 '
                      f'count={len(TYPED_BYTES)} of=/tmp/kbd.raw '
                      '2>/dev/null) & sleep 5',
                      f'kbdopen{index}')
            vm = self.here.vm
            vm.cmd('qom-set', path=kbd.qom, property='activate-keyboard',
                   value=True)
            typed = ''
            for pair, key in enumerate(TYPED_KEYS, 1):
                for down in (True, False):
                    vm.cmd('input-send-event', events=[{
                        'type': 'key',
                        'data': {'down': down,
                                 'key': {'type': 'qcode', 'data': key}}}])
                # Let the guest drain each three-byte make/break pair before
                # sending the next; share the 30-second wait across both.
                count = pair * 3
                typed = self._out(
                    f'while [ "$kbd_wait" -gt 0 ] && '
                    f'[ "`wc -c < /tmp/kbd.raw`" -lt {count} ]; do '
                    'sleep 1; kbd_wait=`expr "$kbd_wait" - 1`; done; '
                    'echo KBD""-BEGIN; wc -c < /tmp/kbd.raw; '
                    'od -An -tx1 /tmp/kbd.raw; echo KBD""-END',
                    f'kbdread{index}')
                match = re.search(r'KBD-BEGIN\s*\n(.*?)KBD-END', typed, re.S)
                lines = match.group(1).splitlines() if match else []
                got_count = int(lines[0].strip()) if lines else 0
                if got_count < count:
                    break
            match = re.search(r'KBD-BEGIN\s*\n(.*?)KBD-END', typed, re.S)
            lines = match.group(1).splitlines() if match else []
            got_count = int(lines[0].strip()) if lines else 0
            got = od_bytes('\n'.join(lines[1:])) if lines else []
            self._expect(got == TYPED_BYTES, f'read {got} from '
                         f'{kbd.dev_dir}/keyboard ({got_count} raw bytes), '
                         'not the typed keys')

        def _check_scsi_card(self):
            card = self.flavour.scsi_card
            channels = re.findall(card.hinv, self._out('hinv', 'hinvscsi'))
            self._expect(sorted(channels) == list(card.controllers),
                         f'hinv lists the card\'s channels as controllers '
                         f'{channels}, not {list(card.controllers)}')
            links = link_targets(self._out('ls -l /hw/scsi_ctlr',
                                           'scsictlr'))
            for number, channel in zip(card.controllers, ('0', '1')):
                self._expect(re.match(
                    rf'^{card.vertex}/scsi_ctlr/{channel}$',
                    links.get(number, '')),
                    f'/hw/scsi_ctlr/{number} is not channel {channel} of '
                    'the card')

        def _diag(self, name, cmd):
            output, status = self.here.output(
                cmd, self._tag(name),
                timeout=self._left(DIAG_TIMEOUT_SECONDS[name]))
            problems = diag_problems(name, output)
            self._expect(not problems,
                         f'{name} exited {status}: ' + '; '.join(problems))

        def _check_olrti(self):
            for args in self.flavour.rti_args:
                self._diag('olrti', f'/usr/diags/bin/olrti {args} -internal')

        def _check_olenet(self):
            self._diag('olenet', '/usr/diags/bin/olenet '
                       f'-v {self.flavour.olenet_vertex}')

        def _check_oldisk(self):
            self._diag('oldisk', '/usr/diags/bin/oldisk '
                       '-filename /tmp/oldisk.tf -filesize 8')

        def _check_olmem(self):
            self._diag('olmem', '/usr/diags/bin/olmem '
                       f'{self.flavour.olmem_args}')

        def _check_oltape(self):
            self._diag('oltape', '/usr/diags/bin/oltape --tapemark --eot '
                       f'-d {self.flavour.tape_dev}')

        def _check_olcdrom(self):
            self._expect(re.search(self.flavour.cd_hinv,
                                   self._out('hinv -c scsi', 'cdrom')),
                         'hinv -c scsi lists no CD-ROM where expected')
            self._diag('olcdrom', '/usr/diags/bin/olcdrom -device '
                       f'{self.flavour.cd_dev}')

        def _check_olpci(self):
            self._diag('olpci', f'/usr/diags/bin/olpci {OPERATOR} '
                       f'-v {self.flavour.olpci_vertex}')

        def _check_olperi(self):
            self._diag('olperi',
                       f'/usr/diags/bin/olperi {OPERATOR} {OLPERI_ARGS}')

        def _check_olsio(self):
            self._diag('olsio', f'/usr/diags/bin/olsio {OPERATOR} '
                       f'-v {self.flavour.olsio_vertex}')

        def _check_olusb(self):
            self._diag('olusb', f'/usr/diags/bin/olusb {OPERATOR} '
                       f'-v {self.flavour.olusb_vertex}')

        def _check_olvst(self):
            self._diag('olvst', f'/usr/diags/bin/olvst {OPERATOR}')

        def _check_pandora(self):
            self._diag('pandora', '/usr/diags/bin/pandora -nocolor')

        def _check_torpedo(self):
            self._diag('torpedo', f'/usr/diags/bin/torpedo {OPERATOR}')

        def _check_timer(self):
            # IRIX warns when a CPU has seen no scheduler clock tick for
            # ten seconds or more. Any such warning fails the row, as the
            # one-node topology test required. The message splits them by
            # the seconds IRIX reports: one whole Count wrap
            # (Flavour.wrap_seconds) is a Compare match the CPU model
            # missed; any other gap is a tick the guest did not get for
            # another reason. The trailer IRIX prints does not identify
            # the cause, so this check classifies gaps by duration.
            # It runs even after the row was cut short or its relaunch
            # failed (lib_irix_diag.OBSERVERS), counting every console the
            # row opened, a relaunch that never reached a shell included.
            consoles = [self.first.con] + self.failed_consoles
            if self.here not in (None, self.first):
                consoles.append(self.here.con)
            wraps, other = 0, 0
            for con in consoles:
                counts = tick_warnings(text(con.buf),
                                       self.flavour.wrap_seconds)
                wraps += counts[0]
                other += counts[1]
            self._expect(wraps + other == 0,
                         f'IRIX reported {wraps + other} missed scheduler '
                         f'ticks: {wraps} one whole Count wrap late '
                         f'({self.flavour.wrap_seconds[0]} to '
                         f'{self.flavour.wrap_seconds[1]}s), {other} after '
                         'another gap')

        # -- reboot, stop and relaunch ------------------------------------

        def _before_reboot(self):
            """Set up, in the first boot, everything the reboot, stop and
            relaunch read back."""
            if self.row.runs('parallel'):
                stale, live, _plp = self.flavour.parallel
                ioconfig = self._out('cat /etc/ioconfig.conf', 'ioconfig')
                self._expect(re.search(stale, ioconfig),
                             'the disk no longer carries the stale parallel '
                             'entry this check removes')
                self._expect(re.search(live, ioconfig),
                             'IRIX recorded no live parallel entry')
            self._run("grep -v '/ecpp$' /etc/ioconfig.conf > /tmp/ioc",
                      'dropecpp')
            self._run('cp /tmp/ioc /etc/ioconfig.conf', 'putioc')
            self._run('nvram %s %s' % NVRAM_VARIABLE, 'nvramset')
            sent = time.time()
            self._run(f'date {GUEST_DATE_SET}', 'dateset')
            clock_set = ClockReading(GUEST_DATE_SET_EPOCH, sent, time.time())
            self.clock_before = self._clock('datebefore')
            self._run('sync', 'sync')
            error, bound = clock_error(clock_set, self.clock_before)
            self._expect(abs(error) <= bound + CLOCK_ALLOWANCE_SECONDS,
                         f'date {GUEST_DATE_SET} reads back {error:+.1f}s '
                         'off the set instant')

        def _clock(self, tag):
            sent = time.time()
            output = self._out("date -u '+DATE=%Y-%m-%d %H:%M:%S'", tag)
            received = time.time()
            guest = guest_date(output)
            self._expect(guest is not None,
                         f'date printed no DATE= line: {output!r}')
            return ClockReading(guest, sent, received)

        def _check_reboot(self):
            self._before_reboot()
            con = self.here.con
            start = len(con.buf)
            t0 = time.monotonic()
            # uadmin's shutdown-and-reboot kills the processes, syncs and
            # resets through the kernel's own reboot path, the part this
            # checks, without reboot's two minutes of rc scripts.
            con.send('/sbin/uadmin 2 1')
            banner = con.expect(self.flavour.prom_banner,
                                self._left(REBOOT_TIMEOUT_SECONDS),
                                what='the PROM banner after reboot',
                                start=start)
            self.rebooted_at = banner.start()
            self._phase('reboot to the PROM banner',
                        round(time.monotonic() - t0, 1))

        def _check_warm_boot(self):
            """IRIX boots again on the same machine after its own reset
            (the PROM reports its configuration but, at the kernel's
            request, runs no diagnostics), and reads back the nvram
            variable, the clock and, where the row checks it, the parallel
            port it had before."""
            if self.rebooted_at is None:
                raise CheckFailed('the reboot never reached the PROM')
            phases = []
            try:
                prom = drive_to_shell(self.here.con,
                                      self._left(BOOT_BUDGET_SECONDS),
                                      phases, menu=False,
                                      start=self.rebooted_at)
            finally:
                for phase, seconds in phases:
                    self._phase(f'warm boot: {phase}', seconds)
            # The kernel asks the PROM to skip its diagnostics on a reboot
            # it requests, and the PROM says so; a PROM that ran them, or
            # printed nothing, was not reset by IRIX's own request.
            self._expect('Skipping diags as requested by kernel' in prom,
                         'the PROM after the reboot did not skip its '
                         'diagnostics at the kernel\'s request')
            for what, pattern in prom_facts(self.flavour, self.row,
                                            diags=False):
                self._expect(re.search(pattern, prom),
                             f'the PROM after the reboot did not report '
                             f'{what}')
            self._read_back('the reboot')
            if self.row.runs('parallel'):
                self._check_parallel()
            self._run('sync', 'syncwarm')

        def _check_persistence(self):
            if not self.row.runs('reboot'):
                self._before_reboot()
            if self.clock_before is None:
                raise CheckFailed('the first boot set nothing to read back')
            # Size the stop from the console's round trip so far: three
            # times the tolerance the reading after it will allow.
            reading = self.clock_before
            tolerance = clock_error(reading, reading)[1] + \
                CLOCK_ALLOWANCE_SECONDS
            self.stopped = max(STOPPED_SECONDS, math.ceil(3 * tolerance))
            self.first.vm.shutdown()
            self._left(self.stopped)
            time.sleep(self.stopped)
            try:
                self.here = self._launch(f'{self.row.id}-relaunch',
                                         relaunch=True)
            except (HarnessError, QEMUMachineError) as exc:
                # The first launch's console closed with it, so there is
                # no shell left for the checks after this one.
                self.here = None
                console = self.log_file(f'{self.row.id}-relaunch-console.log')
                raise NoConsole(f'the relaunch did not reach a shell: {exc} '
                                f'-- console log: {console}') from exc
            self._read_back(f'a {self.stopped}s stop')

        def _read_back(self, across):
            """The nvram variable reads back, and the guest clock has
            advanced by the host's elapsed time since the last reading."""
            after = self._clock('dateafter')
            name, value = NVRAM_VARIABLE
            self._expect(re.search(rf'(?m)^{value}\s*$',
                                   self._out(f'nvram {name}', 'nvramget')),
                         f'nvram {name} lost across {across}')
            error, bound = clock_error(self.clock_before, after)
            bound += CLOCK_ALLOWANCE_SECONDS
            if self.stopped:
                self._expect(bound <= self.stopped / 3,
                             f'the clock check cannot tell a '
                             f'{self.stopped}s loss from its own '
                             f'{bound:.1f}s tolerance')
            self.log.info('clock across %s: %+.1fs off the host, allowed '
                          '%.1fs', across, error, bound)
            self._expect(abs(error) <= bound,
                         f'the guest clock is {error:+.1f}s off the host '
                         f'across {across} (allowed {bound:.1f}s)')
            self.clock_before = after

        def _check_parallel(self):
            plp = self.flavour.parallel[2]
            self._expect(re.search(rf'parallel port: {plp}\b', self._out(
                'hinv | grep parallel', 'plp')),
                f'with no parallel history the port is not {plp}')

    def register_irix_diag_row(cls, row_id):
        """Attach test_<row_id> to cls: one flavour's boot and checks."""

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_row(self):
            self._run_row(row_id)

        test_row.__name__ = f'test_{row_id}'
        test_row.__doc__ = f'irix-diag-matrix row {row_id}'
        setattr(cls, test_row.__name__, test_row)

    class IrixDiag(IrixDiagMatrix):
        """The two-chassis Origin 200."""

        ASSET_FIRMWARE = LocalAsset(*IP27_FIRMWARE_SPEC)
        ASSET_DISK = LocalAsset(*IP27_DIAG_DISK_SPEC)
        ASSET_MEDIUM = LocalAsset(*DIAG_MEDIUM_SPEC)

    register_irix_diag_row(IrixDiag, 'origin200_dual')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
