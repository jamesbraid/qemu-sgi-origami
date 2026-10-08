#!/usr/bin/env python3
#
# Records a one-processor Origin 200 PROM boot and replays it
#
# SPDX-License-Identifier: BSD-3-Clause
"""Record and replay the IP27 PROM on a one-processor Origin 200.

QEMU's record/replay refuses more than one processor, so every case here
runs one. The PROM resets the machine several times before its first
banner, which is where a reset that waits on the processor thread
deadlocks, and its system controller and cache-as-RAM poll run virtual
timers throughout, which is where a replay that services a timer or
queued work at a different instruction from the recording shows up.

- test_menu_replays_identically records the boot to the System
  Maintenance Menu and replays it, and the two consoles must match byte
  for byte.
- test_module_restart_replays_identically records the same boot with the
  machine's module-start trace on and requires the replay to make the same
  starts. The PROM's own first reset is a Hub port reset, which restarts
  the machine's modules: the start stops the processors, waits for them
  without blocking, resets the modules and releases the processors, which
  is the path that must not wait on a processor thread under replay.
- test_seek_back_reloads_the_snapshot replays part way, seeks back, which
  reloads the initial snapshot, and runs on past where it started.
- test_reverse_debugging reuses QEMU's generic gdb reverse-step and
  reverse-continue check (reverse_debugging.py) on the same machine.
"""

import os
import time
from subprocess import check_output

from lib_prom import FIRMWARE_REL, FIRMWARE_SHA256
from qemu_test import get_qemu_img, skipIfMissingEnv
from qemu_test.asset import LocalAsset
from reverse_debugging import ReverseDebugging


# 128 ns of virtual time per instruction. The boot reaches the menu in
# about 800 million instructions, 20 to 30 seconds of host time.
ICOUNT_SHIFT = 7

# Every PROM boot prints its banner after its first resets. A deadlocked
# reset prints nothing at all.
FIRST_OUTPUT_SECONDS = 120
MENU_SECONDS = 600
MENU_PROMPT = b'Option?'

# Instruction counts for the seek: forward past the first resets and well
# into the arbitration rotation, back to before that point, then on past
# where the seek started.
SEEK_FORWARD = 300_000_000
SEEK_BACK = 100_000_000
SEEK_ON = 400_000_000
SEEK_SECONDS = 120


class Origin200RecordReplay(ReverseDebugging):

    timeout = 1500
    ASSET_PROM = LocalAsset(FIRMWARE_REL, FIRMWARE_SHA256)

    def setUp(self):
        super().setUp()
        self.require_accelerator('tcg')
        self.set_machine('origin200')
        self.prom = self.ASSET_PROM.fetch()
        self.snapshot_image = self.scratch_file('snapshot.qcow2')
        self.replay_log = self.scratch_file('replay.bin')

    def machine_args(self):
        return ['-m', '64M', '-bios', str(self.prom), '-smp', '1']

    def launch(self, mode, console, start=True, extra=()):
        """Start one record or replay run, writing the guest console to a
        file so the two runs compare byte for byte. It starts paused, so
        QMP is up before the PROM's first reset, and runs once start is
        set. extra adds command-line arguments."""
        if mode == 'record':
            check_output([get_qemu_img(self), 'create', '-f', 'qcow2',
                          self.snapshot_image, '128M'])
        vm = self.get_vm(name=mode)
        vm.add_args(*self.machine_args(),
                    '-icount', f'shift={ICOUNT_SHIFT},rr={mode},'
                    f'rrfile={self.replay_log},rrsnapshot=init',
                    '-drive', f'file={self.snapshot_image},if=none',
                    '-serial', f'file:{console}',
                    *extra,
                    '-S')
        vm.launch()
        if start:
            vm.cmd('cont')
        return vm

    @staticmethod
    def read(path):
        try:
            with open(path, 'rb') as f:
                return f.read()
        except FileNotFoundError:
            return b''

    def wait_for_output(self, vm, console, mode):
        """Fail when the PROM prints nothing, which is how a reset-time
        deadlock shows. QMP is no help: the main thread is the one stuck."""
        deadline = time.monotonic() + FIRST_OUTPUT_SECONDS
        while not self.read(console):
            if not vm.is_running():
                self.fail(f'{mode}: QEMU exited with status {vm.exitcode()} '
                          'before the PROM printed anything')
            if time.monotonic() > deadline:
                self.fail(f'{mode}: no console output within '
                          f'{FIRST_OUTPUT_SECONDS}s (a reset-time deadlock, '
                          'such as the Hub waiting for a processor thread '
                          'that needs the replay lock, or a slow start)')
            time.sleep(0.2)

    def wait_for_icount(self, vm, icount, what):
        """Wait for a replay to stop at icount, failing by name when it
        stops advancing instead."""
        deadline = time.monotonic() + SEEK_SECONDS
        while time.monotonic() < deadline:
            reached = vm.cmd('query-replay')['icount']
            if (vm.cmd('query-status')['status'] != 'running' and
                    reached == icount):
                return
            time.sleep(0.2)
        self.fail(f'{what}: replay stuck at instruction {reached} instead of '
                  f'stopping at {icount}. Replay from the loaded snapshot '
                  'diverged from the recording and is waiting for an event '
                  'it will not produce, such as a timer the recording '
                  'serviced at a different instruction')

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_menu_replays_identically(self):
        recorded = self.scratch_file('console-record.txt')
        replayed = self.scratch_file('console-replay.txt')

        vm = self.launch('record', recorded)
        try:
            self.wait_for_output(vm, recorded, 'record')
            deadline = time.monotonic() + MENU_SECONDS
            while MENU_PROMPT not in self.read(recorded):
                if time.monotonic() > deadline:
                    self.fail(f'record: no "{MENU_PROMPT.decode()}" within '
                              f'{MENU_SECONDS}s')
                time.sleep(0.2)
            vm.shutdown()
        finally:
            vm.shutdown(hard=True)

        vm = self.launch('replay', replayed)
        try:
            self.wait_for_output(vm, replayed, 'replay')
            # Replay ends where the recording quit, and QEMU exits there.
            deadline = time.monotonic() + MENU_SECONDS
            while vm.is_running():
                if time.monotonic() > deadline:
                    self.fail(f'replay: still running after {MENU_SECONDS}s '
                              f'with {len(self.read(replayed))} of '
                              f'{len(self.read(recorded))} console bytes')
                time.sleep(0.2)
            vm.wait()
        finally:
            vm.shutdown(hard=True)

        self.assertEqual(self.read(replayed), self.read(recorded))

    def run_to_menu(self, mode, console, trace_log):
        """Record or replay the boot to the menu and return the module
        starts the run traced, with their log prefixes stripped."""
        vm = self.launch(mode, console, extra=(
            '-d', 'trace:sgi_machine_module_start', '-D', trace_log))
        try:
            self.wait_for_output(vm, console, mode)
            deadline = time.monotonic() + MENU_SECONDS
            if mode == 'record':
                while MENU_PROMPT not in self.read(console):
                    if time.monotonic() > deadline:
                        self.fail(f'record: no "{MENU_PROMPT.decode()}" '
                                  f'within {MENU_SECONDS}s')
                    time.sleep(0.2)
                vm.shutdown()
            else:
                while vm.is_running():
                    if time.monotonic() > deadline:
                        self.fail(f'replay: still running after '
                                  f'{MENU_SECONDS}s')
                    time.sleep(0.2)
                vm.wait()
        finally:
            vm.shutdown(hard=True)
        return [line.split(':', 1)[-1].strip()
                for line in self.read(trace_log).decode().splitlines()
                if 'sgi_machine_module_start' in line]

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_module_restart_replays_identically(self):
        recorded = self.scratch_file('console-record.txt')
        replayed = self.scratch_file('console-replay.txt')

        starts = self.run_to_menu('record', recorded,
                                  self.scratch_file('trace-record.txt'))
        self.assertTrue(starts, 'the boot made no module start, so this '
                        'test would prove nothing')
        self.assertEqual(
            self.run_to_menu('replay', replayed,
                             self.scratch_file('trace-replay.txt')),
            starts)
        self.assertEqual(self.read(replayed), self.read(recorded))

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_seek_back_reloads_the_snapshot(self):
        vm = self.launch('record', self.scratch_file('console-record.txt'))
        try:
            deadline = time.monotonic() + MENU_SECONDS
            while vm.cmd('query-replay')['icount'] <= SEEK_ON:
                if time.monotonic() > deadline:
                    self.fail(f'record: instruction {SEEK_ON} not reached '
                              f'in {MENU_SECONDS}s')
                time.sleep(0.2)
            vm.shutdown()
        finally:
            vm.shutdown(hard=True)

        vm = self.launch('replay', self.scratch_file('console-replay.txt'),
                         start=False)
        try:
            vm.cmd('replay-break', icount=SEEK_FORWARD)
            vm.cmd('cont')
            self.wait_for_icount(vm, SEEK_FORWARD, 'forward replay')
            vm.cmd('replay-seek', icount=SEEK_BACK)
            self.wait_for_icount(vm, SEEK_BACK, 'seek back')
            vm.cmd('replay-break', icount=SEEK_ON)
            vm.cmd('cont')
            self.wait_for_icount(vm, SEEK_ON, 'forward after the seek')
        finally:
            vm.shutdown(hard=True)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_reverse_debugging(self):
        self.reverse_debugging(gdb_arch='mips:isa64r2', shift=ICOUNT_SHIFT,
                               args=self.machine_args(), big_endian=True)

    def get_pc(self, gdb):
        """The program counter, read so that it prints in hex: with no
        executable loaded, a plain print shows a decimal.

        The first read also steps over the reset vector's jump. A forward
        stepi runs a branch together with its delay slot, but reverse-stepi
        goes back one instruction at a time and stops in the delay slot, so
        the generic check's ten steps must start on straight-line code.
        """
        if not getattr(self, 'past_reset_jump', False):
            self.past_reset_jump = True
            gdb.cli('stepi')
        return gdb.cli('p/x $pc').get_addr()

    def reverse_debugging_run(self, gdb, vm, port, gdb_arch, last_icount):
        # The remote target does not say which MIPS it is.
        gdb.cli(f'set architecture {gdb_arch}')
        gdb.cli('set mips abi n64')
        super().reverse_debugging_run(gdb, vm, port, gdb_arch, last_icount)


if __name__ == '__main__':
    ReverseDebugging.main()
