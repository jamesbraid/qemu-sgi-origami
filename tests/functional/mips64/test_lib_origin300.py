#!/usr/bin/env python3
#
# Unit tests for lib_origin300: the synthesized identity records and the
# machine properties that carry them
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import. The launch cases build
the boot flash from a stand-in payload, so they patch the reviewed seed
digests that a real firmware image would have to match.
"""

import hashlib
import os
import tempfile
import unittest
from unittest import mock

import lib_origin300 as lib


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


class IdentityRecordTests(unittest.TestCase):

    # The digests tools/origami_lib/node_eeprom.py in the project checkout
    # produces for the same records; a run of `tools/origami run prom
    # --machine origin300` records the chassis one in its run.json.
    def test_the_node_records_match_the_project_builders(self):
        self.assertEqual(
            _sha256(lib._build_node_chassis_eeprom_record()),
            '1a1d69a51ffd994593aca8f754d85c46'
            '360b81c08044fc852d0f4dfd0dd59b05')
        self.assertEqual(
            _sha256(lib._build_node_board_eeprom_record(2)),
            'd423543d1e31228f35a1c5de84886c5e'
            '4379027735c805ffc026341d4dfbd6a7')
        self.assertEqual(
            _sha256(lib._build_node_board_eeprom_record(4)),
            '5cdea5248cd97b84471714a33910c272'
            '92f0463c58b5453150ee1673a5d135a5')

    def test_each_node_board_names_its_processor_count(self):
        self.assertIn(b'IP45_2CPU', lib._build_node_board_eeprom_record(2))
        self.assertIn(b'IP45_4CPU', lib._build_node_board_eeprom_record(4))

    def test_a_record_sums_to_zero_in_whole_units(self):
        record = lib._build_node_board_eeprom_record(4)
        self.assertEqual(sum(record) & 0xff, 0)
        self.assertEqual(len(record), record[1] * 8)

    def test_no_node_board_carries_three_processors(self):
        with self.assertRaisesRegex(ValueError, '3 processors'):
            lib._build_node_board_eeprom_record(3)


class LaunchTests(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = self.tmp.name
        for rel, contents in ((lib.FIRMWARE_REL, b'\x00' * 4096),
                              (lib.SPD_INPUTS[0][1], b'\x01' * 256),
                              (lib.SPD_INPUTS[1][1], b'\x02' * 256)):
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'wb') as out:
                out.write(contents)
        self.scratch = os.path.join(root, 'scratch')
        os.makedirs(self.scratch)
        # A stand-in payload cannot match the reviewed seeds or asset
        # digests, so accept whatever it builds.
        self.enterContext(mock.patch.object(lib, 'resolve_asset',
                                            lambda a, rel, sha=None:
                                            os.path.join(a, rel)))
        self.enterContext(mock.patch.object(
            lib, 'hashlib', mock.Mock(sha256=lambda data: mock.Mock(
                hexdigest=lambda: 'seed'))))
        self.enterContext(mock.patch.dict(lib.BOOT_FLASH_SEED_SHA256,
                                          {2: 'seed', 4: 'seed'}))

    @staticmethod
    def _props(machine):
        return dict(option.split('=', 1)
                    for option in machine.split(',')[1:])

    def test_one_chassis_carries_the_node_board_record(self):
        machine, _argv = lib.launch_args(self.tmp.name, 2, self.scratch)
        props = self._props(machine)
        with open(props['board-eeprom.0'], 'rb') as record:
            self.assertEqual(record.read(),
                             lib._build_node_board_eeprom_record(2))
        with open(props['chassis-eeprom.0'], 'rb') as record:
            self.assertEqual(record.read(),
                             lib._build_node_chassis_eeprom_record())

    def test_the_pair_gives_each_node_its_own_board_record(self):
        machine, argv = lib.launch_args_two_chassis(self.tmp.name, (4, 2),
                                                    self.scratch)
        props = self._props(machine)
        with open(props['board-eeprom.0'], 'rb') as record:
            self.assertIn(b'IP45_4CPU', record.read())
        with open(props['board-eeprom.2'], 'rb') as record:
            self.assertIn(b'IP45_2CPU', record.read())
        self.assertEqual(props['chassis-eeprom.2'], props['chassis-eeprom.0'])
        self.assertNotEqual(props['board-eeprom.3'], props['board-eeprom.1'])
        self.assertEqual(props['topology'], 'origin300-two-chassis')
        self.assertEqual(argv[argv.index('-smp') + 1], '6')


if __name__ == '__main__':
    unittest.main()
