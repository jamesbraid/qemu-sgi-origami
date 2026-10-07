# SPDX-License-Identifier: BSD-3-Clause
"""The generator's synthetic-serial scheme against the QEMU header's vectors.

tests/unit/test-sgi-synthetic-serial.c checks the same literals against
include/hw/mips/sgi/sgi-synthetic-serial.h, so the catalogue and the device
defaults cannot disagree about a value.
"""

import copy
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import synthetic_serial as ss

HEADER = ROOT / "include/hw/mips/sgi/sgi-synthetic-serial.h"
MODELS = ROOT / "hw/mips/sgi/models"


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


class ModuleOnewirePartsTests(unittest.TestCase):
    """The compiler refuses module one-wire parts a machine could confuse:
    unmarked, repeated, missing, under the wrong letter or instance, or a
    midplane serial without its record."""

    @classmethod
    def setUpClass(cls):
        from sgi_model.model_compile import CompileError, compile_model
        from sgi_model.model_validation import load_checked_system_model, schema

        cls.CompileError = CompileError
        cls.compile_model = staticmethod(compile_model)
        cls.rack = load_checked_system_model(
            MODELS / "origin2000-rack.yaml", model_schema=schema(MODELS))

    def setUp(self):
        self.model = copy.deepcopy(self.rack)
        self.emulation = self.model["machine"]["emulation"]
        self.parts = self.emulation["module_onewire_parts"]

    def refused(self, pattern):
        with self.assertRaisesRegex(self.CompileError, pattern):
            self.compile_model(self.model)

    def test_catalogue_parts_compile(self):
        compiled = self.compile_model(self.model)
        module = compiled["modules"][0]
        self.assertEqual(module["identity_serial"], ss.onewire_serial("Y", 0))
        self.assertEqual(module["midplane_serial"], ss.onewire_serial("L", 0))
        self.assertEqual(module["midplane_record"]["name"], "8P12_MPLN")
        for node in compiled["nodes"]:
            self.assertNotIn("module_responder_serial", node["identity"])

    def test_unmarked_identity_serial_is_refused(self):
        self.parts[0]["identity_serial"] = "0x100000000009"
        self.refused("marker")

    def test_repeated_parts_are_refused(self):
        self.parts.append(dict(self.parts[0]))
        self.refused("more than one")

    def test_missing_parts_are_refused(self):
        self.parts.clear()
        self.refused("has no module one-wire parts")

    def test_identity_under_another_letter_is_refused(self):
        self.parts[0]["identity_serial"] = hex(ss.onewire_serial("M", 0))
        self.refused("not letter Y")

    def test_midplane_at_another_instance_is_refused(self):
        self.parts[0]["midplane_serial"] = hex(ss.onewire_serial("L", 1))
        self.refused("not letter L")

    def test_midplane_serial_without_record_is_refused(self):
        del self.parts[0]["midplane"]
        self.refused("both or neither")

    def test_unknown_module_is_refused(self):
        self.parts[0]["component"] = "no-such-module"
        self.refused("unknown module")


class CatalogueMarkerTests(unittest.TestCase):
    """The compiler refuses a catalogue serial that carries no marker."""

    @classmethod
    def setUpClass(cls):
        from sgi_model.model_compile import CompileError, compile_model
        from sgi_model.model_validation import load_checked_system_model, schema

        cls.CompileError = CompileError
        cls.compile_model = staticmethod(compile_model)
        model_schema = schema(MODELS)
        cls.models = {
            name: load_checked_system_model(MODELS / f"{name}.yaml",
                                            model_schema=model_schema)
            for name in ("origin200", "origin2000-rack", "octane")}

    def model(self, name):
        return copy.deepcopy(self.models[name])

    def test_catalogue_compiles_marked(self):
        for name in self.models:
            self.compile_model(self.model(name))

    def test_unmarked_onewire_serial_is_refused(self):
        model = self.model("origin200")
        identity = model["machine"]["emulation"]["synthetic_identities"][0]
        identity["serial"] = "0x0c0b0a090807"
        with self.assertRaisesRegex(self.CompileError, "marker"):
            self.compile_model(model)

    def test_repeated_identity_is_refused(self):
        model = self.model("origin200")
        identities = model["machine"]["emulation"]["synthetic_identities"]
        identities.append(dict(identities[0]))
        with self.assertRaisesRegex(self.CompileError, "more than one"):
            self.compile_model(model)

    def test_unmarked_module_serial_is_refused(self):
        model = self.model("origin2000-rack")
        model["machine"]["emulation"]["module_semantic_serials"][0][
            "serial"] = "SGIMODULE1"
        with self.assertRaisesRegex(self.CompileError, "marker"):
            self.compile_model(model)

    def test_unmarked_board_record_barcode_is_refused(self):
        model = self.model("octane")
        record = next(c["board_record"] for c in model["components"]
                      if "board_record" in c)
        record["barcode"] = "SIM3010001"
        with self.assertRaisesRegex(self.CompileError, "marker"):
            self.compile_model(model)


if __name__ == "__main__":
    unittest.main()
