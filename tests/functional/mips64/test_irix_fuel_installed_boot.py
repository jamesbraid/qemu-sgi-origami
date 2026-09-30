#!/usr/bin/env python3
#
# Boot the supplied Fuel IRIX disk through its stock PROM on IOC3-A
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require a Fuel IRIX 6.5.30 shell and shutdown back to the PROM menu."""

import json
from pathlib import Path
import re

import lib_fuel
from lib_prom import FORK_ROOT, drive_console, run_record, utc_now
from qemu_test import (LocalAsset, QemuSystemTest, skipBigDataTest,
                       skipIfMissingEnv, skipSlowTest)


BOOT_TIMEOUT_SECONDS = 900
DRIVER_STEPS = (
    (r'Option\?', '1\n'),
    (r'login:', 'root\n'),
    (r'TERM = \(vt100\)', '\n'),
    (r'[#$] ', 'id\n'),
    (r'[#$] ', 'who -r\n'),
    (r'[#$] ', 'uname -aR\n'),
    (r'[#$] ', "hinv -c disk; printf '\\n%s\\n' FUEL_SCSI_READY\n"),
)
SCSI_STEPS = (
    (r'[#$] ', "dd if=/dev/zero bs=512 count=1 | tr '\\000' A > /tmp/fuel-a\n"),
    (r'[#$] ', "dd if=/dev/zero bs=512 count=1 | tr '\\000' B > /tmp/fuel-b\n"),
    (r'[#$] ', "dd if=/dev/zero bs=512 count=1 | tr '\\000' C > /tmp/fuel-c\n"),
    (r'[#$] ', "dd if=/dev/zero bs=512 count=1 | tr '\\000' D > /tmp/fuel-d\n"),
    (r'[#$] ', 'dd if=/dev/rdsk/dks0d2vol of=/tmp/fuel-r0 bs=512 skip=128 count=1\n'),
    (r'[#$] ', 'dd if=/dev/rdsk/dks1d2vol of=/tmp/fuel-r1 bs=512 skip=128 count=1\n'),
    (r'[#$] ', 'cmp /tmp/fuel-a /tmp/fuel-r0 && cmp /tmp/fuel-b /tmp/fuel-r1 '
                 "&& printf '\\n%s\\n' FUEL_SCSI_SEEDS_OK\n"),
)
SCSI_WRITE_STEPS = (
    (r'[#$] ', 'dd if=/tmp/fuel-c of=/dev/rdsk/dks0d2vol bs=512 seek=128 count=1; sync\n'),
    (r'[#$] ', 'dd if=/dev/rdsk/dks0d2vol of=/tmp/fuel-r0 bs=512 skip=128 count=1; '
                 'dd if=/dev/rdsk/dks1d2vol of=/tmp/fuel-r1 bs=512 skip=128 count=1\n'),
    (r'[#$] ', 'cmp /tmp/fuel-c /tmp/fuel-r0 && cmp /tmp/fuel-b /tmp/fuel-r1 '
                 "&& printf '\\n%s\\n' FUEL_SCSI_CH0_OK\n"),
    (r'[#$] ', 'dd if=/tmp/fuel-d of=/dev/rdsk/dks1d2vol bs=512 seek=128 count=1; sync\n'),
    (r'[#$] ', 'dd if=/dev/rdsk/dks0d2vol of=/tmp/fuel-r0 bs=512 skip=128 count=1; '
                 'dd if=/dev/rdsk/dks1d2vol of=/tmp/fuel-r1 bs=512 skip=128 count=1\n'),
    (r'[#$] ', 'cmp /tmp/fuel-c /tmp/fuel-r0 && cmp /tmp/fuel-d /tmp/fuel-r1 '
                 "&& printf '\\n%s\\n' FUEL_SCSI_CH1_OK\n"),
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

    ASSET_MANIFEST = LocalAsset(*lib_fuel.ASSET_SPECS['manifest'])
    ASSET_PROM = LocalAsset(*lib_fuel.ASSET_SPECS['prom'])
    ASSET_BOOT_FLASH = LocalAsset(*lib_fuel.ASSET_SPECS['boot_flash'])
    ASSET_DISK = LocalAsset(*lib_fuel.ASSET_SPECS['disk'])
    ASSET_MAC_EEPROM = LocalAsset(*lib_fuel.ASSET_SPECS['fuel-mac-eeprom'])

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_irix_fuel_installed_boot(self):
        fixture, files = lib_fuel.load_fixture(self)
        machine, args, private_flash = lib_fuel.launch_args(
            fixture, files, self.workdir)
        scratch_files, scratch_args = lib_fuel.scsi_scratch_disks(self.workdir)
        args += scratch_args
        identity = run_record(FORK_ROOT, self.qemu_bin, **files,
                              **scratch_files, private_flash=private_flash)

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
            phase_paths = [self.log_file(f'fuel-scsi-phase{i}.log')
                           for i in range(3)]
            transcript, ready = drive_console(
                self.vm.console_socket, phase_paths[0], BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=re.compile(rb'[\r\n]FUEL_SCSI_READY[\r\n]'),
                vm=self.vm)
            self.assertTrue(ready, 'IRIX SCSI inventory was not reached')
            for channel in range(2):
                self.assertRegex(
                    transcript, rf'unit 2 on SCSI controller {channel}\b',
                    'Scratch channel/target mapping is ambiguous: no writes')
            # The previous read can include its trailing prompt in one chunk.
            self.vm.console_socket.sendall(b'\n')
            seeds, seeds_ok = drive_console(
                self.vm.console_socket, phase_paths[1], 90, SCSI_STEPS,
                until=re.compile(rb'[\r\n]FUEL_SCSI_SEEDS_OK[\r\n]'))
            transcript += seeds
            self.assertTrue(seeds_ok, 'Scratch seed mapping failed: no writes')
            self.assertEqual(seeds.count('1+0 records out'), 6,
                             'Every expected payload and seed read must be 512 bytes')
            self.vm.console_socket.sendall(b'\n')
            transfers, reached = drive_console(
                self.vm.console_socket, phase_paths[2], 180, SCSI_WRITE_STEPS,
                reached_when=lambda t: bool(SHUTDOWN_TO_PROM_RE.search(t)))
            transcript += transfers
            self.assertEqual(transfers.count('1+0 records out'), 6,
                             'Every scratch write and reread must be 512 bytes')
            for channel, marker in enumerate((b'C', b'D')):
                with open(scratch_files[f'qualification{channel}'], 'rb') as disk:
                    disk.seek(128 * 512)
                    self.assertEqual(disk.read(512), marker * 512,
                                     'Private backend differs from guest readback')
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
                'scsi_channel0_write_isolation': bool(re.search(
                    r'[\r\n]FUEL_SCSI_CH0_OK[\r\n]', transcript)),
                'scsi_channel1_write_isolation': bool(re.search(
                    r'[\r\n]FUEL_SCSI_CH1_OK[\r\n]', transcript)),
                'shutdown_started': 'Shutdown started' in transcript,
                'shutdown_to_prom_menu': reached,
            }
            with open(log_path, 'wb') as console:
                for phase in locals().get('phase_paths', []):
                    if Path(phase).exists():
                        console.write(Path(phase).read_bytes())
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
                    'scsi_scratch': {
                        'bytes_per_operation': 512, 'sector': 128,
                        'target': 2, 'lun': 0,
                        'guest_devices': ['/dev/rdsk/dks0d2vol',
                                          '/dev/rdsk/dks1d2vol'],
                        'initial_markers': ['A', 'B'],
                        'written_markers': ['C', 'D'],
                        'initial_disk_size_bytes': 64 * 1024 * 1024,
                    },
                    'outcomes': outcomes, 'error': error, 'identity': identity,
                }, output, indent=2, sort_keys=True)
                output.write('\n')
            self.vm.shutdown(hard=self.vm.exitcode() is not None)

        for name, passed in outcomes.items():
            self.assertTrue(
                passed, f'{name} not reached: {log_path}; {record_path}')


if __name__ == '__main__':
    QemuSystemTest.main()
