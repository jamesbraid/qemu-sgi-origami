#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks, one boot per machine
# flavour: boot the installed IRIX disk with SGI's online diagnostics,
# log in, and run every check irix-diag-matrix marks for the flavour
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The layer above the firmware. The MDK, POD and fx legs run diagnostics
before or instead of IRIX; this runs IRIX's own online diagnostics
(`/usr/diags/bin`) on a booted, multi-user system -- the diagnostic
stack a field engineer uses on a live machine, exercising the hardware
through IRIX's real drivers -- and checks each IOC3 port the flavour
brings out, on every machine flavour we model an IOC3 on.

irix-diag-matrix names the flavours and which checks each runs;
lib_irix_diag.FLAVOURS holds how each is launched and the device names
IRIX gives it on the diagnostics disk; lib_irix_diag.REASONS says why a
flavour skips a check. Each flavour is one test method in its own meson
target, so the nightly runs them side by side: this file owns the
two-chassis Origin 200, the flavour this test began on, and each
test_irix_diag_<flavour>.py beside it registers one more. A flavour boots
once and runs every check in that session; only the reboot, persistence
and parallel checks end it, by rebooting IRIX and then stopping and
relaunching QEMU on the same NVRAM and disk images. Every check runs
even after another fails, and the test fails at the end naming each
failed check; the per-check results and the time each boot phase and
check took are written to <flavour>-summary.json.

The disks are the published golden-with-diagnostics images:
`assets/irix/6.5.30/root-diags.qcow2`, the installed IRIX 6.5.30 root
with the `field_diags_irix` product added and its environment loaded by
a post-install reboot (the project checkout's irix-online-diagnostics
notebook records how it is built and why the reboot matters), and
`root-diags-origin300.qcow2`, the Origin 300 install with
field_diags_irix 1.8 from Internal Support Tools 2.7. Each run writes to
its own overlay in the test's scratch directory, never to the golden
image. IRIX numbers devices from the disk's own /etc/ioconfig.conf
history, not from the topology, so the device names in FLAVOURS hold for
these disks and would differ on another.

The diagnostics, and why these:

  - oldisk  -- a disk exerciser: asynchronous direct pattern read/write
    through the SCSI controller;
  - olmem   -- tag RAM, L1 cache, L2 cache, address-pattern and
    moving-inversion memory;
  - oltape  -- rw, thrash and block against an emulated `scsi-tape`
    carrying a composed `ARCHIVE Python 25601` identity IRIX's driver
    recognizes as a DAT (the recorded real drive's own identity matches
    nothing in IRIX's table and the extended-status ioctl it needs
    refuses);
  - olcdrom -- reads an emulated `scsi-cd` holding a generated data CD;
  - olrti   -- the IOC3's real-time interrupt, internal loopback only:
    the external test needs a cable from output to input;
  - olenet  -- the IOC3 Ethernet probe, where the disk carries it.

olperi (the random-instruction test) is not for IP29 and fails on it;
torpedo runs a thousand-loop FPU workload measured in guest time, which
dilates many-fold under emulation, and a bounded run only duplicates the
firmware MDK coverage; pandora is a thirty-minute stress run; olvht and
linktest need a HIPPI channel or a router topology; olvst passes
interactively but stalls under the container the functional tests run
in. The project checkout's irix-online-diagnostics notebook
characterizes each of them.
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
                           processor_count, prom_facts, run_checks,
                           wav_nonzero_samples)
from lib_matrix import MatrixError, matrix_rows
from lib_net import HarnessError, IrixConsole
from lib_prom import FORK_ROOT, require_assets, run_record

try:
    from qemu.machine.machine import QEMUMachineError
    from qemu_test import QemuSystemTest, get_qemu_img
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


# Each wait below is capped twice: by its own limit, about two to four
# times the slowest this matrix measured on 2026-09-26 (debug_tcg build/,
# MTTCG), and by what is left of the row's budget in irix-diag-matrix.
# The slowest figures come from builds with the missed Compare matches of
# jamesb/qemu#866, which slowed disk and CD-ROM I/O several-fold.

# PROM power-on, kernel and init to a root shell: 1075s at most (the
# four-node shoehorn, 391s PROM and 685s IRIX), 125 to 400s on the Origin
# 200 and one-node machines.
BOOT_BUDGET_SECONDS = 1500

# Ordinary shell commands: hinv, netstat and the like took 22s at most.
STEP_TIMEOUT_SECONDS = 120

# Each diagnostic's own limit, against the slowest measured: olrti 27s,
# olenet 15s, oldisk 894s, olmem 1246s, oltape 12s, olcdrom 329s.
DIAG_TIMEOUT_SECONDS = {
    'olrti': 120,
    'olenet': 120,
    'oldisk': 1200,
    'olmem': 1800,
    'oltape': 120,
    'olcdrom': 900,
}

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

MEDIUM_REL = os.path.join('assets', 'irix', '6.5.30', 'olcdrom-medium.raw')
MEDIUM_SHA256 = \
    'ba0a1466d7125cd350ee4f047677d7a3d9f8a8ed8b13469c92042cfa0bd07829'


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
        rebooted_at = None
        stopped = 0
        tags = 0

        # -- launching ---------------------------------------------------

        def _prepare(self, flavour):
            assets = require_assets(self, flavour.firmware, flavour.disk,
                                    (MEDIUM_REL, MEDIUM_SHA256))
            self.assets = {
                'root': os.environ['ORIGAMI_ASSETS'],
                'firmware': assets[flavour.firmware],
                'disk': assets[flavour.disk],
                'medium': assets[MEDIUM_REL],
            }
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
            self._prepare(self.flavour)

            try:
                self.first = self._launch(self.row.id)
                self.here = self.first
                self.results = run_checks(self.row, self._run_check,
                                          self._resync)
            except (HarnessError, OutOfTime) as exc:
                console = self.log_file(self.row.id + '-console.log')
                self.results = {check: ('not run', f'the first boot: {exc} '
                                        f'-- console log: {console}', 0)
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
            numbers = device_numbers(self._out(
                'ls -lL ' + ' '.join(f'{p.dev} {p.vertex}' for p in ports),
                'ttys'))
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
            # Open the keyboard for a read of exactly the expected byte
            # count before any key is sent, so no byte can arrive while
            # nothing has the device open.
            self._run('rm -f /tmp/kbd.out; '
                      f'(dd if={kbd.dev_dir}/keyboard bs=1 '
                      f'count={len(TYPED_BYTES)} 2>/dev/null | '
                      'od -An -tx1 > /tmp/kbd.out) & sleep 5',
                      f'kbdopen{index}')
            vm = self.here.vm
            vm.cmd('qom-set', path=kbd.qom, property='activate-keyboard',
                   value=True)
            for key in TYPED_KEYS:
                for down in (True, False):
                    vm.cmd('input-send-event', events=[{
                        'type': 'key',
                        'data': {'down': down,
                                 'key': {'type': 'qcode', 'data': key}}}])
            # Up to 30 guest seconds for the bytes; an empty file means dd
            # is still blocked and the keystrokes never arrived.
            typed = self._out(
                'i=0; while [ $i -lt 30 ]; do [ -s /tmp/kbd.out ] && break; '
                'sleep 1; i=`expr $i + 1`; done; '
                'echo KBD""-BEGIN; cat /tmp/kbd.out; echo KBD""-END',
                f'kbdread{index}')
            match = re.search(r'KBD-BEGIN\s*\n(.*?)KBD-END', typed, re.S)
            got = od_bytes(match.group(1)) if match else None
            self._expect(got == TYPED_BYTES, f'read {got} from '
                         f'{kbd.dev_dir}/keyboard, not the typed keys')

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

        def _check_timer(self):
            # IRIX prints this when a scheduler tick arrives one whole
            # Count wrap late: a Compare match the CPU model missed (see
            # docs/known-issues.md in the project checkout). A stall of the
            # emulator itself prints "Probably caused by splhi" instead.
            launches = [self.first] + ([self.here] if self.here not in
                                       (None, self.first) else [])
            misses = sum(len(re.findall(rb'Probable lost interrupt!',
                                        launch.con.buf))
                         for launch in launches)
            self._expect(misses == 0,
                         f'IRIX reported {misses} lost scheduler clock '
                         'interrupts')

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
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_row(self):
            self._run_row(row_id)

        test_row.__name__ = f'test_{row_id}'
        test_row.__doc__ = f'irix-diag-matrix row {row_id}'
        setattr(cls, test_row.__name__, test_row)

    class IrixDiag(IrixDiagMatrix):
        """The two-chassis Origin 200."""

    register_irix_diag_row(IrixDiag, 'origin200_dual')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
