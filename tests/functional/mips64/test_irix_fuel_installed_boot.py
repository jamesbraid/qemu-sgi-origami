#!/usr/bin/env python3
#
# Boot the supplied Fuel IRIX disk through its stock PROM on IOC3-A
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require a Fuel IRIX 6.5.30 shell and shutdown back to the PROM menu."""

import json
import re

import lib_fuel
from lib_prom import FORK_ROOT, drive_console, run_record, utc_now
from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv, skipSlowTest


BOOT_TIMEOUT_SECONDS = 900
DRIVER_STEPS = (
    (r'Option\?', '1\n'),
    (r'login:', 'root\n'),
    (r'TERM = \(vt100\)', '\n'),
    (r'[#$] ', 'id\n'),
    (r'[#$] ', 'who -r\n'),
    (r'[#$] ', 'uname -aR\n'),
    (r'[#$] ', 'sync; /etc/shutdown -y -g0 -i0\n'),
)
CPU_RE = re.compile(r'No\. of CPUs enabled\s*=\s*1\b')
MEMORY_RE = re.compile(r'Mem enabled\s*=\s*512 MB\b')
DISK_RE = re.compile(
    r'Walking SCSI Adapter 0, \(pci id 1\)[\r\n]+'
    r'1\+ Device Vendor Product: QEMU QEMU HARDDISK')
ROOT_RE = re.compile(r'\buid=0\(root\)')
UNAME_RE = re.compile(r'IRIX64[^\r\n]*6\.5\.30m\b[^\r\n]*\bIP35\b')
# Scope the return to PROM after the command, excluding the initial menu.
SHUTDOWN_TO_PROM_RE = re.compile(
    rb'sync; /etc/shutdown -y -g0 -i0[\r\n]+'
    rb'.*Shutdown started'
    rb'.*INIT: New run level:[ \t]*0[\r\n]+'
    rb'.*IP35 PROM SGI Version 6\.210\b'
    rb'.*Skipping diags as requested by kernel'
    rb'.*System Maintenance Menu[\r\n]+'
    rb'.*Option\?', re.S)


class IrixFuelInstalledBoot(QemuSystemTest):

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_irix_fuel_installed_boot(self):
        fixture, files = lib_fuel.load_fixture(self)
        machine, args, private_flash = lib_fuel.launch_args(
            fixture, files, self.workdir)
        identity = run_record(FORK_ROOT, self.qemu_bin,
                              **files, private_flash=private_flash)

        self.set_machine('fuel')
        self.vm.set_console(console_index=1)
        self.vm.set_machine(machine)
        self.vm.add_args(*args, '-S')
        log_path = self.log_file('irix-fuel-installed-boot.log')
        record_path = self.log_file('run.json')
        transcript = ''
        reached = False
        error = None
        started = utc_now()
        try:
            self.vm.launch()
            self.vm.cmd('cont')
            transcript, reached = drive_console(
                self.vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS,
                reached_when=lambda t: bool(SHUTDOWN_TO_PROM_RE.search(t)),
                vm=self.vm)
        except Exception as exc:
            error = f'{type(exc).__name__}: {exc}'
            raise
        finally:
            outcomes = {
                'prom_diagnostics': 'ALL DIAGS PASSED' in transcript,
                'maintenance_menu': 'System Maintenance Menu' in transcript,
                'one_cpu': bool(CPU_RE.search(transcript)),
                'memory_512_mib': bool(MEMORY_RE.search(transcript)),
                'disk_target_1': bool(DISK_RE.search(transcript)),
                'root_shell': bool(ROOT_RE.search(transcript)),
                'run_level_2': 'run-level 2' in transcript,
                'irix_release_platform': bool(UNAME_RE.search(transcript)),
                'shutdown_started': 'Shutdown started' in transcript,
                'shutdown_to_prom_menu': reached,
            }
            with open(record_path, 'w', encoding='utf-8') as output:
                json.dump({
                    'schema': 1, 'started_utc': started, 'ended_utc': utc_now(),
                    'fixture_provenance': fixture['provenance'],
                    'experimental_inputs': fixture['experimental_inputs'],
                    'guest_platform': fixture['guest_platform'],
                    'console': {'name': 'ioc3-a', 'index': 1,
                                'required_prom_setting': 'console=d'},
                    'machine': machine,
                    'qemu_argv': list(self.vm._qemu_full_args),
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'outcomes': outcomes, 'error': error, 'identity': identity,
                }, output, indent=2, sort_keys=True)
                output.write('\n')
            self.vm.shutdown(hard=self.vm.exitcode() is not None)

        for name, passed in outcomes.items():
            self.assertTrue(
                passed, f'{name} not reached: {log_path}; {record_path}')


if __name__ == '__main__':
    QemuSystemTest.main()
