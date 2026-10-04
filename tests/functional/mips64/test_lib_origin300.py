#!/usr/bin/env python3
#
# Unit tests for lib_origin300: the machine properties and boot flash it
# hands an Origin 300 launch
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import. The launch cases build
the boot flash from a stand-in payload, so they patch the reviewed seed
digests that a real firmware image would have to match.
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
        # The stand-in firmware cannot match the reviewed boot-flash digest.
        # Use a fixed hash for this synthetic fixture.
        self.enterContext(mock.patch.object(
            lib, 'hashlib', mock.Mock(sha256=lambda data: mock.Mock(
                hexdigest=lambda: 'seed'))))
        self.enterContext(mock.patch.dict(lib.BOOT_FLASH_SEED_SHA256,
                                          {2: 'seed', 4: 'seed'}))

    @staticmethod
    def _props(machine):
        return dict(option.split('=', 1)
                    for option in machine.split(',')[1:])

    def test_one_chassis_names_only_its_mac(self):
        machine, argv = lib.launch_args(self.firmware, 2, self.scratch)
        self.assertTrue(machine.startswith('origin300,'))
        self.assertEqual(self._props(machine), {'mac': lib.NETWORK_MAC})
        self.assertEqual(argv[argv.index('-smp') + 1], '2')
        self.assertIn(f'if=pflash,index=0,file='
                      f'{self.scratch}/ip35-boot-flash.raw,format=raw', argv)

    def test_the_pair_adds_its_topology_and_second_flash(self):
        machine, argv = lib.launch_args_two_chassis(
            self.firmware, (4, 2), self.scratch)
        self.assertEqual(self._props(machine), {
            'mac': lib.NETWORK_MAC,
            'topology': 'origin300-two-chassis',
            'population': '4:2'})
        self.assertEqual(argv[argv.index('-smp') + 1], '6')
        self.assertEqual(argv[argv.index('-m') + 1], '1024M')
        self.assertIn(f'if=pflash,index=1,file='
                      f'{self.scratch}/node1/ip35-boot-flash.raw,format=raw',
                      argv)

    def test_no_identity_or_spd_file_is_written(self):
        lib.launch_args_two_chassis(self.firmware, (4, 4), self.scratch)
        written = sorted(os.path.relpath(os.path.join(where, name),
                                         self.scratch)
                         for where, _dirs, names in os.walk(self.scratch)
                         for name in names)
        self.assertEqual(written, ['ip35-boot-flash.raw',
                                   'node1/ip35-boot-flash.raw'])


if __name__ == '__main__':
    unittest.main()
