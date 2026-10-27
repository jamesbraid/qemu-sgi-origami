#!/usr/bin/env python3
#
# IRIX tg traffic on the BCM5701 fitted to the Origin 350 IO9.
#
# SPDX-License-Identifier: BSD-3-Clause
"""Run the existing TCP and UDP tg workload on the Origin 350's IO9.

The BCM5701 is part of the selected machine profile. The test gives QEMU's
default NIC configuration to that machine-created device and reuses the same
byte checks, throughput transfers, and verdict as the Origin 200 option-card
test.
"""

import os
import subprocess

import test_irix_tg_perf as tg_perf


if tg_perf.base.QemuSystemTest is not None:
    from qemu_test.asset import LocalAsset

    RELEASE = '6.5.30'
    FIRMWARE_REL = os.path.join('firmware', RELEASE,
                                'ip35prom.payload.bin')
    FIRMWARE_SHA256 = (
        '554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b')
    DISK_REL = os.path.join('irix', RELEASE, 'root-diags-origin300.qcow2')
    DISK_SHA256 = (
        '150cf2c125cb3bd55764acfeec00804899ff3140cc536cf63a254765faa29390')

    class IrixIo9TgPerf(tg_perf.IrixTgPerf):
        """Exercise the BCM5701 already installed on the Origin 350 IO9."""

        test_irix_tg_perf = None

        MACHINE_OPTIONS = 'origin350,topology=origin350,nodes=1'
        CPU = 'R16000'
        SMP = 4
        MEMORY_MIB = 2048
        MAC = tg_perf.base.TG_MAC
        VM_NAME = 'irix_io9_tg_perf'
        BACKGROUND_TCP_RECEIVER = True
        # The prepared IP35 disk starts tg0 on this subnet. Keep usernet on
        # that subnet so ifconfig updates the primary address, not an alias.
        GUEST_ADDR = '192.0.2.15'
        GATEWAY = '192.0.2.2'
        USERNET_OPTIONS = (
            'net=192.0.2.0/24,host=192.0.2.2,dhcpstart=192.0.2.15')
        PROM_SHA256 = FIRMWARE_SHA256
        ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, FIRMWARE_SHA256)
        ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)
        TG_SLOT = 4

        def _configure_machine(self, vm, prom, disk, _elsc):
            machine_state = self.scratch_file('origin350-machine-state')
            machine_init = self.build_file(
                'tools', 'sgi', 'qemu-sgi-machine-init')
            result = subprocess.run(
                [machine_init, '--machine', self.MACHINE_OPTIONS,
                 '--boot-prom', prom, machine_state],
                capture_output=True, text=True, check=False)
            self.assertEqual(
                result.returncode, 0,
                'qemu-sgi-machine-init failed: '
                f'{result.stdout}{result.stderr}')

            blockdev_args = []
            machine_options = self.MACHINE_OPTIONS
            stores = sorted(os.listdir(machine_state))
            self.assertTrue(stores, 'machine-init created no machine stores')
            for required in ('node0-flash', 'nvram0', 'nvram0-clock'):
                self.assertIn(f'{required}.raw', stores,
                              f'machine-init omitted required store {required}')
            for filename in stores:
                self.assertTrue(filename.endswith('.raw'),
                                f'unexpected machine-init output {filename}')
                name = filename[:-4]
                blockdev_args.extend((
                    '-blockdev',
                    f'driver=raw,node-name={name},file.driver=file,'
                    f'file.filename={os.path.join(machine_state, filename)}'))
                machine_options += f',{name}={name}'
            machine_options += f',mac={self.MAC}'

            vm.set_machine(machine_options)
            vm.set_console()  # serial 0 is the profile's L1 console
            vm.add_args(
                '-accel', tg_perf.base.IRIX_ACCEL,
                '-cpu', self.CPU, '-smp', str(self.SMP),
                '-m', str(self.MEMORY_MIB),
                '-audio', 'none', *blockdev_args,
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-d', 'guest_errors',
                '-D', self.log_file('qemu-dbg.log'))

        def _configure_network(self, vm, netdev, pcap):
            # The machine constructor connects this default NIC configuration
            # to its catalogue-selected BCM5701. Do not add another device.
            vm.add_args(
                '-nic', f'{netdev},mac={self.MAC}',
                '-object', f'filter-dump,id=tgcap,netdev=tgnet,file={pcap}')

        def _interface(self, con):
            iface = super()._interface(con)
            self.assertEqual(iface, 'tg0',
                             'the Origin 350 IO9 BCM5701 should enumerate '
                             'as tg0 at PCI device 4')
            return iface

        @tg_perf.base.skipBigDataTest()
        @tg_perf.base.skipSlowTest()
        @tg_perf.base.skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
        def test_irix_io9_tg_perf(self):
            self._run_tg_perf()


if __name__ == '__main__' and tg_perf.base.QemuSystemTest is not None:
    IrixIo9TgPerf.main()
