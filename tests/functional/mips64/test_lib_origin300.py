#!/usr/bin/env python3
#
# Unit tests for lib_origin300: the machine properties and boot flash it
# hands an Origin 300 launch
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import. The launch cases stand
in for qemu-sgi-machine-init, which refuses anything but a real PROM, and
patch the reviewed digests a real flash would have to match.
"""

import os
import tempfile
import unittest
from unittest import mock

import lib_origin300 as lib


class LaunchTests(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = self.tmp.name
        self.firmware = os.path.join(root, lib.FIRMWARE_REL)
        os.makedirs(os.path.dirname(self.firmware), exist_ok=True)
        with open(self.firmware, 'wb') as out:
            out.write(b'\x00' * 4096)
        self.scratch = os.path.join(root, 'scratch')
        os.makedirs(self.scratch)
        self.calls = []
        self.enterContext(mock.patch.object(
            lib, '_run_machine_init', self._machine_init))
        self.enterContext(mock.patch.dict(lib.BOOT_FLASH_SEED_SHA256, {
            2: lib.hashlib.sha256(b'2').hexdigest(),
            4: lib.hashlib.sha256(b'4').hexdigest()}))

    def _machine_init(self, args):
        """Write each node's flash as its processor count, as text."""
        self.calls.append(args)
        state = args[-1]
        os.makedirs(state)
        population = args[args.index('--population') + 1]
        for node, cpus in enumerate(population.split(':')):
            with open(os.path.join(state, f'node{node}-flash.raw'),
                      'wb') as out:
                out.write(cpus.encode())

    @staticmethod
    def _props(machine):
        return dict(option.split('=', 1)
                    for option in machine.split(',')[1:])

    def test_one_chassis_names_only_its_mac(self):
        machine, argv = lib.launch_args(self.firmware, 2, self.scratch)
        self.assertTrue(machine.startswith('origin300,'))
        self.assertEqual(self._props(machine), {'mac': lib.NETWORK_MAC})
        self.assertEqual(argv[argv.index('-smp') + 1], '2')
        self.assertIn(f'if=pflash,index=0,file={self.scratch}/'
                      'origin300-state/node0-flash.raw,format=raw', argv)
        self.assertEqual(self.calls, [[
            '--machine', 'origin300,topology=origin300,nodes=1',
            '--population', '2', '--boot-prom', self.firmware,
            f'{self.scratch}/origin300-state']])

    def test_a_relaunch_starts_from_new_state(self):
        lib.launch_args(self.firmware, 4, self.scratch)
        lib.launch_args(self.firmware, 2, self.scratch)
        self.assertEqual(len(self.calls), 2)

    def test_a_flash_with_another_digest_is_refused(self):
        with mock.patch.dict(lib.BOOT_FLASH_SEED_SHA256, {2: 'other'}):
            with self.assertRaisesRegex(ValueError, 'reviewed SHA-256'):
                lib.launch_args(self.firmware, 2, self.scratch)

    def test_an_unreviewed_population_is_refused(self):
        with self.assertRaisesRegex(ValueError, '3-processor'):
            lib.launch_args(self.firmware, 3, self.scratch)
        self.assertEqual(self.calls, [])

    def test_the_pair_adds_its_topology_and_second_flash(self):
        machine, argv = lib.launch_args_two_chassis(
            self.firmware, (4, 2), self.scratch)
        self.assertEqual(self._props(machine), {
            'mac': lib.NETWORK_MAC,
            'topology': 'origin300-two-chassis',
            'population': '4:2'})
        self.assertEqual(argv[argv.index('-smp') + 1], '6')
        self.assertEqual(argv[argv.index('-m') + 1], '1024M')
        self.assertIn(f'if=pflash,index=1,file={self.scratch}/'
                      'origin300-state/node1-flash.raw,format=raw', argv)
        self.assertEqual(self.calls[0][:4], [
            '--machine', 'origin300,topology=origin300-two-chassis,nodes=2',
            '--population', '4:2'])

    def test_no_identity_or_spd_file_is_written(self):
        lib.launch_args_two_chassis(self.firmware, (4, 4), self.scratch)
        written = sorted(os.path.relpath(os.path.join(where, name),
                                         self.scratch)
                         for where, _dirs, names in os.walk(self.scratch)
                         for name in names)
        self.assertEqual(written, ['origin300-state/node0-flash.raw',
                                   'origin300-state/node1-flash.raw'])


if __name__ == '__main__':
    unittest.main()
