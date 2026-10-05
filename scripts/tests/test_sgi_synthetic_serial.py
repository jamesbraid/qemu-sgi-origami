# SPDX-License-Identifier: BSD-3-Clause
"""The generator's synthetic-serial scheme against the QEMU header's vectors.

tests/unit/test-sgi-synthetic-serial.c checks the same literals against
include/hw/mips/sgi/sgi-synthetic-serial.h, so the catalogue and the device
defaults cannot disagree about a value.
"""

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import synthetic_serial as ss

HEADER = ROOT / "include/hw/mips/sgi/sgi-synthetic-serial.h"


class SyntheticSerialTests(unittest.TestCase):
    def test_onewire_vectors(self):
        self.assertEqual(ss.onewire_serial("N", 0), 0x51454D554E00)
        self.assertEqual(ss.onewire_serial("C", 3), 0x51454D554303)
        self.assertEqual(ss.onewire_serial("R", 255), 0x51454D5552FF)
        self.assertEqual(ss.onewire_serial("B", 1), 0x51454D554201)
        self.assertTrue(ss.onewire_serial_marked(0x51454D554E00))

    def test_uniqueware_vector(self):
        self.assertEqual(ss.uniqueware_serial(0x5E7, 1), 0x5E7454D55001)
        self.assertEqual(ss.uniqueware_serial(0x5E7, 2), 0x5E7454D55002)
        self.assertTrue(ss.onewire_serial_marked(0x5E7454D55001))

    def test_unmarked(self):
        for serial in (0x060504030201, 0x5E7000000001, 0x304E53494753):
            self.assertFalse(ss.onewire_serial_marked(serial))

    def test_text_vectors(self):
        self.assertEqual(ss.text_serial("N", 0), "EMUN00")
        self.assertEqual(ss.text_serial("I", 12), "EMUI12")
        self.assertEqual(ss.text_serial("M", 1, 10), "EMUM000001")
        self.assertTrue(ss.text_serial_marked("EMUM000001"))
        self.assertFalse(ss.text_serial_marked("SGIMODULE1"))

    def test_barcode_vectors(self):
        self.assertEqual(ss.barcode("N", 0x51454D554203), "EMUN03")
        self.assertEqual(ss.barcode("O", 0x51454D554241), "EMUO65")
        self.assertEqual(ss.barcode("P", 0x5E7454D55064), "EMUP100")
        self.assertEqual(ss.barcode("I", 0x060504030201), "EMUI00")

    def test_refusals(self):
        with self.assertRaises(ValueError):
            ss.onewire_serial("Q", 0)
        with self.assertRaises(ValueError):
            ss.onewire_serial("N", 256)
        with self.assertRaises(ValueError):
            ss.text_serial("N", 100)

    def test_header_constants_and_letters_match(self):
        header = HEADER.read_text()
        self.assertIn("SGI_SYNTHETIC_ONEWIRE_PREFIX  UINT64_C(0x51454d55)", header)
        self.assertIn("SGI_SYNTHETIC_UNIQUEWARE_MARKER UINT64_C(0x454d55)", header)
        self.assertIn('SGI_SYNTHETIC_TEXT_MARKER     "EMU"', header)
        table = re.findall(r"^ \*   ([A-Z])  ", header, re.MULTILINE)
        self.assertEqual(table, list(ss.LETTERS))


if __name__ == "__main__":
    unittest.main()
