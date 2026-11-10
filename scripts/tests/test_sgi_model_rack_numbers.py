# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
"""Tests for `machine.emulation.rack_numbers`: schema, validation, compiler.

The models here are the resolved Origin 3800 with its bricks moved into
numbered racks, so a rejection names the one rule the change breaks. No
shipped topology numbers a rack yet; the last class checks that.
"""

import copy
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import system_model as _resolver
from sgi_model.model_compile import compile_model
from sgi_model.model_emit import _render_topology
from sgi_model.model_validation import (
    check_schema, load_checked_system_model, schema,
)
from sgi_model.registry import IMPLEMENTED_COMPOSITIONS

_CACHE: dict[str, dict] = {}

_CBRICKS = [f"c{n}-chassis" for n in range(8)]
_RBRICKS = ["rbrick0-brick", "rbrick1-brick"]
_REASON = "A fixture rack."


def _origin3800() -> dict:
    if "origin3800" not in _CACHE:
        _CACHE["origin3800"] = load_checked_system_model(
            INPUTS / "origin3800.yaml", model_schema=schema(INPUTS))
    return copy.deepcopy(_CACHE["origin3800"])


def _component(model: dict, ident: str) -> dict:
    return next(item for item in model["components"] if item["id"] == ident)


def _set_bay(model: dict, ident: str, bay: int) -> None:
    for entry in model["machine"]["emulation"]["initial_module_numbers"]:
        if entry["component"] == ident:
            entry["number"] = bay


def _add_rack(model: dict, ident: str, number: int, members=()) -> None:
    model["components"].append({
        "id": ident, "kind": "chassis", "name": f"fixture rack {number:03d}",
        "provenance": "vendor-document", "sources": ["fixture"]})
    emulation = model["machine"]["emulation"]
    emulation.setdefault("rack_numbers", []).append(
        {"component": ident, "number": number, "reason": _REASON})
    for member in members:
        _component(model, member)["parent"] = ident


def _add_brick(model: dict, ident: str, like: str, parent: str,
               bay: int) -> None:
    brick = copy.deepcopy(_component(model, like))
    brick.update(id=ident, parent=parent)
    model["components"].append(brick)
    model["machine"]["emulation"]["initial_module_numbers"].append(
        {"component": ident, "number": bay, "reason": _REASON})


def _racked_origin3800() -> dict:
    """Compute racks 1 and 2 and I/O rack 101, one I-brick in the I/O rack.

    Rack 1 keeps C-bricks 0 to 3 and R-brick 0, rack 2 takes C-bricks 4 to 7
    and R-brick 1. Bays are the shipped ones, so no bay repeats yet.
    """
    model = _origin3800()
    emulation = model["machine"]["emulation"]
    emulation["rack_numbers"] = [
        {"component": "rack", "number": 1, "reason": _REASON}]
    _add_rack(model, "rack-002", 2,
              _CBRICKS[4:] + _RBRICKS[1:])
    _add_rack(model, "rack-101", 101, ["ibrick-brick"])
    return model


class RackNumberSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = schema(INPUTS)

    def problems(self, model):
        return check_schema(model, self.schema)

    def test_rack_numbers_entry_shape(self):
        model = _racked_origin3800()
        self.assertEqual(self.problems(model), [])
        entry = model["machine"]["emulation"]["rack_numbers"][0]
        for change in ({"number": 0}, {"number": 1024}, {"number": "1"},
                       {"number": True}, {"number": 1.5},
                       {"reason": ""}, {"extra": 1}):
            with self.subTest(change=change):
                changed = copy.deepcopy(model)
                changed["machine"]["emulation"]["rack_numbers"][0] = dict(
                    entry, **change)
                self.assertTrue(self.problems(changed))
        for missing in ("component", "number", "reason"):
            with self.subTest(missing=missing):
                changed = copy.deepcopy(model)
                del changed["machine"]["emulation"]["rack_numbers"][0][missing]
                self.assertTrue(self.problems(changed))


class RackNumberValidationTests(unittest.TestCase):
    def reject(self, model, message):
        with self.assertRaisesRegex(_resolver.ResolutionError, message):
            _resolver._validate_resolved_system_model(copy.deepcopy(model))

    def test_two_compute_racks_and_an_io_rack_resolve(self):
        _resolver._validate_resolved_system_model(_racked_origin3800())

    def test_a_bay_repeats_across_racks(self):
        model = _racked_origin3800()
        _set_bay(model, "c4-chassis", 10)
        _resolver._validate_resolved_system_model(model)

    def test_rack_numbers_need_an_implemented_target(self):
        # The rack check is called on its own: a planned target fails the
        # module number check first when both are present.
        model = _racked_origin3800()
        model["machine"]["emulation"]["lifecycle"] = "planned"
        with self.assertRaisesRegex(_resolver.ResolutionError,
                                    "rack_numbers requires an implemented"):
            _resolver.model_composition.validate_rack_numbers(model)

    def test_rack_numbers_need_an_sn1_target_with_l1_controllers(self):
        validate = _resolver.model_composition.validate_rack_numbers
        model = _racked_origin3800()
        model["machine"]["generation"] = "SN0"
        with self.assertRaisesRegex(_resolver.ResolutionError,
                                    "rack_numbers.*SN1"):
            validate(model)
        model = _racked_origin3800()
        del model["machine"]["emulation"]["l1_controllers"]
        with self.assertRaisesRegex(_resolver.ResolutionError,
                                    "rack_numbers.*L1 controllers"):
            validate(model)

    def test_invalid_entries(self):
        cases = (
            ("unknown component", lambda e: e[0].update(component="nowhere"),
             "unknown component 'nowhere'"),
            ("not a chassis", lambda e: e[0].update(component="c0-chassis"),
             "is not a chassis"),
            ("repeated component", lambda e: e[1].update(component="rack"),
             "repeated component 'rack'"),
            ("repeated number", lambda e: e[1].update(number=1),
             "repeated number 1"),
            ("unreasoned", lambda e: e[0].update(reason=" "),
             "invalid rack number entry"),
            ("extra field", lambda e: e[0].update(extra=1),
             "invalid rack number entry"),
        )
        for name, change, message in cases:
            with self.subTest(name=name):
                model = _racked_origin3800()
                change(model["machine"]["emulation"]["rack_numbers"])
                self.reject(model, message)

    def test_rack_number_must_be_one_the_prom_encodes(self):
        valid = ([*range(1, 9), *range(11, 19), *range(21, 29),
                  *range(31, 39)] +
                 [100 + tens * 10 + unit
                  for tens in range(8) for unit in range(1, 5)])
        self.assertEqual(len(valid), 64)
        for number in (0, 9, 10, 19, 20, 29, 30, 39, 40, 41, 99, 100, 105,
                       110, 115, 175, 200, 1023):
            self.assertNotIn(number, valid)
            with self.subTest(number=number):
                model = _racked_origin3800()
                # Rack 2 holds C-bricks only, so changing its number keeps
                # every other rule satisfied.
                model["machine"]["emulation"]["rack_numbers"][1][
                    "number"] = number
                self.reject(model, f"rack number {number} is not")
        for number in (8, 18, 38, 101, 104, 111, 174):
            with self.subTest(accepted=number):
                model = _racked_origin3800()
                model["machine"]["emulation"]["rack_numbers"][1][
                    "number"] = number
                if number >= 100:
                    # An I/O rack holds no C- or R-bricks. Move the I/O
                    # rack's own number out of the way first.
                    model["machine"]["emulation"]["rack_numbers"][2][
                        "number"] = 111 if number == 104 else 104
                    self.reject(model, "I/O rack")
                else:
                    _resolver._validate_resolved_system_model(model)

    def test_duplicate_rack_and_bay(self):
        model = _racked_origin3800()
        _set_bay(model, "c4-chassis", 10)
        _component(model, "c4-chassis")["parent"] = "rack"
        # Rack 1 would now hold C-bricks 0 to 4 with c0 and c4 both at bay 10.
        self.reject(model, "repeated rack 1 bay 10")

    def test_bay_must_be_1_to_39(self):
        for bay in (0, 40, 63, 255):
            with self.subTest(bay=bay):
                model = _racked_origin3800()
                _set_bay(model, "c0-chassis", bay)
                self.reject(model, "bay must be an integer from 1 through 39")

    def test_module_outside_any_numbered_rack(self):
        model = _racked_origin3800()
        model["components"].append({
            "id": "loose-rack", "kind": "chassis", "name": "unnumbered"})
        _component(model, "ibrick-brick")["parent"] = "loose-rack"
        self.reject(model, "module 'ibrick-brick' is not in a numbered rack")

    def test_module_without_a_rack_parent(self):
        model = _racked_origin3800()
        del _component(model, "ibrick-brick")["parent"]
        self.reject(model, "module 'ibrick-brick' is not in a numbered rack")

    def test_module_inherits_its_nearest_numbered_chassis(self):
        model = _racked_origin3800()
        # A numberless chassis between the module and its rack is skipped.
        model["components"].append({
            "id": "shelf", "kind": "chassis", "name": "shelf",
            "parent": "rack-101"})
        _component(model, "ibrick-brick")["parent"] = "shelf"
        _resolver._validate_resolved_system_model(model)

    def test_rack_without_a_module(self):
        model = _racked_origin3800()
        _add_rack(model, "rack-003", 3)
        self.reject(model, "rack 'rack-003' holds no module")

    def test_compute_rack_holds_only_c_and_r_bricks(self):
        model = _racked_origin3800()
        _add_brick(model, "ibrick2-brick", "ibrick-brick", "rack-101", 5)
        _component(model, "ibrick-brick")["parent"] = "rack"
        self.reject(model, "compute rack 1 holds an I-brick")

    def test_io_rack_holds_only_io_bricks(self):
        model = _racked_origin3800()
        _component(model, "c0-chassis")["parent"] = "rack-101"
        self.reject(model, "I/O rack 101 holds a C-brick")

    def test_compute_rack_holds_at_most_eight_c_bricks(self):
        model = _racked_origin3800()
        _add_brick(model, "c8-chassis", "c0-chassis", "rack-002", 39)
        # Rack 2 now holds five C-bricks and an R-brick: fine.
        _resolver._validate_resolved_system_model(copy.deepcopy(model))
        for ident in _CBRICKS[4:] + ["c8-chassis"]:
            _component(model, ident)["parent"] = "rack"
        # Rack 1 would hold nine, so rack 2 keeps only the R-brick.
        self.reject(model, "compute rack 1 holds 9 C-bricks")

    def test_compute_rack_holds_at_most_two_r_bricks(self):
        model = _racked_origin3800()
        _add_brick(model, "rbrick2-brick", "rbrick0-brick", "rack", 38)
        # Rack 1 holds R-bricks 0 and 2: two is allowed.
        _resolver._validate_resolved_system_model(copy.deepcopy(model))
        _component(model, "rbrick1-brick")["parent"] = "rack"
        _add_brick(model, "c8-chassis", "c0-chassis", "rack-002", 39)
        self.reject(model, "compute rack 1 holds 3 R-bricks")

    def test_cx_brick_claims_its_router_bay(self):
        # A Cx-brick at bay 10 also holds bay 14, so a module at 14 in the
        # same rack collides, and one in another rack does not.
        model = _racked_origin3800()
        _add_brick(model, "cx-brick", "c0-chassis", "rack-002", 5)
        _component(model, "cx-brick")["name"] = "Cx-brick"
        _set_bay(model, "cx-brick", 10)
        _add_brick(model, "r2-brick", "rbrick0-brick", "rack-002", 14)
        self.reject(model, "rack 2 bay 14")
        _component(model, "r2-brick")["parent"] = "rack"
        _resolver._validate_resolved_system_model(model)

    def test_cx_router_bay_must_stay_in_range(self):
        for bay in (36, 39):
            with self.subTest(bay=bay):
                model = _racked_origin3800()
                _add_brick(model, "cx-brick", "c0-chassis", "rack-002", bay)
                _component(model, "cx-brick")["name"] = "Cx-brick"
                self.reject(model, "router bay")
        model = _racked_origin3800()
        _add_brick(model, "cx-brick", "c0-chassis", "rack-002", 30)
        _component(model, "cx-brick")["name"] = "Cx-brick"
        _resolver._validate_resolved_system_model(model)

    def test_origin3800_module_needs_a_brick_letter(self):
        model = _racked_origin3800()
        brick = _component(model, "ibrick-brick")
        brick.pop("brick_type", None)
        brick["name"] = "unlettered"
        self.reject(model, "has no brick letter")

    def test_null_rack_numbers_keeps_the_bay_rules(self):
        # Bypass the schema: a null rack_numbers is not a numbered topology,
        # so the module number rules must still catch a repeated bay.
        model = _origin3800()
        _set_bay(model, "c1-chassis", 10)
        model["machine"]["emulation"]["rack_numbers"] = None
        with self.assertRaisesRegex(_resolver.ResolutionError,
                                    "repeated number 10"):
            _resolver.model_composition.validate_initial_module_numbers(
                model)

    def test_io_rack_holds_at_most_eight_bricks(self):
        model = _racked_origin3800()
        for index in range(8):
            _add_brick(model, f"ibrick{index + 2}-brick", "ibrick-brick",
                       "rack-101", 5 + index)
        self.reject(model, "I/O rack 101 holds 9 bricks")

    def test_rack_rules_apply_only_to_the_origin_3800(self):
        # The per-rack composition rules rest on the Origin 3800 rack
        # tables; another product only gets the numbering rules.
        model = _racked_origin3800()
        _add_brick(model, "ibrick2-brick", "ibrick-brick", "rack-101", 5)
        _component(model, "ibrick-brick")["parent"] = "rack"
        model["machine"]["emulation"]["profile"] = "origin3400"
        _resolver._validate_resolved_system_model(model)

    def test_origin_2000_module_number_rules_are_unchanged(self):
        model = _resolver.load_system_model(INPUTS / "origin2000-rack.yaml")
        _resolver._validate_resolved_system_model(copy.deepcopy(model))
        model["components"].append({"id": "second-module", "kind": "module"})
        model["machine"]["emulation"]["initial_module_numbers"] = [
            {"component": "second-module", "number": 1, "reason": _REASON},
            {"component": "compute-module", "number": 1, "reason": _REASON}]
        self.reject(model, "repeated number 1")
        model["machine"]["emulation"]["initial_module_numbers"][0][
            "number"] = 255
        _resolver._validate_resolved_system_model(copy.deepcopy(model))

    def test_unracked_cx_brick_claims_its_router_number(self):
        # With no rack numbers the number is global: a Cx-brick at 200 also
        # holds 204, which no other module may take.
        model = _origin3800()
        _component(model, "c0-chassis")["name"] = "Cx-brick"
        _set_bay(model, "c0-chassis", 200)
        _set_bay(model, "c1-chassis", 204)
        self.reject(model, "repeated number 204")
        _set_bay(model, "c1-chassis", 205)
        _resolver._validate_resolved_system_model(copy.deepcopy(model))
        # Two Cx-bricks four apart collide through the first one's router.
        _component(model, "c1-chassis")["name"] = "Cx-brick"
        _set_bay(model, "c1-chassis", 196)
        self.reject(model, "repeated number 200")

    def test_unracked_cx_router_number_must_stay_in_range(self):
        model = _origin3800()
        _component(model, "c0-chassis")["name"] = "Cx-brick"
        _set_bay(model, "c0-chassis", 252)
        self.reject(model, "router number 256")
        _set_bay(model, "c0-chassis", 251)
        _resolver._validate_resolved_system_model(model)

    def test_unracked_sn1_keeps_its_bay_rules(self):
        model = _origin3800()
        _set_bay(model, "c1-chassis", 10)
        self.reject(model, "repeated number 10")
        model = _origin3800()
        _set_bay(model, "c1-chassis", 200)
        _resolver._validate_resolved_system_model(model)


class RackNumberCompileTests(unittest.TestCase):
    def modules(self, model):
        return {module["id"]: module
                for module in compile_model(model)["modules"]}

    def test_modules_carry_their_racks_number(self):
        modules = self.modules(_racked_origin3800())
        for ident in _CBRICKS[:4] + _RBRICKS[:1]:
            self.assertEqual(modules[ident]["rack"], 1, ident)
        for ident in _CBRICKS[4:] + _RBRICKS[1:]:
            self.assertEqual(modules[ident]["rack"], 2, ident)
        self.assertEqual(modules["ibrick-brick"]["rack"], 101)
        # The bay is still the module number.
        self.assertEqual(modules["c0-chassis"]["initial_module_number"], 10)

    def test_modules_of_an_unnumbered_topology_have_no_rack(self):
        for module in compile_model(_origin3800())["modules"]:
            self.assertNotIn("rack", module)

    def test_catalogue_record_names_the_rack_only_when_there_is_one(self):
        entry = compile_model(_racked_origin3800())
        emitted = {module["id"]: module for module in
                   _render_topology(entry, "origin3800")["modules"]}
        self.assertEqual(emitted["ibrick-brick"]["rack"], 101)
        plain = _render_topology(compile_model(_origin3800()), "origin3800")
        for module in plain["modules"]:
            self.assertNotIn("rack", module)


class RackFieldIsInTheQapiRecord(unittest.TestCase):
    """The catalogue's module record only uses members the QAPI struct has."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / "scripts"))
        from qapi.parser import QAPISchemaParser
        doc = QAPISchemaParser(str(ROOT / "qapi/sgi-machines.json"))
        struct = next(e for e in doc.exprs
                      if e.get("struct") == "SGIModelModule")
        cls.types = struct["data"]
        cls.members = {name.lstrip("*"): name.startswith("*")
                       for name in cls.types}

    def test_rack_is_an_optional_member(self):
        self.assertTrue(self.members["rack"])
        self.assertEqual(self.types["*rack"], "uint16")

    def test_a_numbered_catalogue_record_is_covered(self):
        topology = _render_topology(compile_model(_racked_origin3800()),
                                    "origin3800")
        required = {name for name, optional in self.members.items()
                    if not optional}
        for module in topology["modules"]:
            self.assertLessEqual(set(module), set(self.members))
            self.assertLessEqual(required, set(module))
            self.assertIn("rack", module)
            self.assertLess(module["rack"], 1 << 16)

    def test_an_unnumbered_record_has_no_rack_member(self):
        topology = _render_topology(compile_model(_origin3800()), "origin3800")
        for module in topology["modules"]:
            self.assertLessEqual(set(module), set(self.members))
            self.assertNotIn("rack", module)


# The topologies that number their racks; every other keeps rack 1.
RACKED_TOPOLOGIES = frozenset({"origin3800-64p"})


class ShippedTopologiesAreUnracked(unittest.TestCase):
    def test_only_the_multi_rack_topologies_number_a_rack(self):
        for name in sorted(IMPLEMENTED_COMPOSITIONS):
            path = INPUTS / f"{name}.yaml"
            with self.subTest(name=name):
                model = _resolver.load_system_model(path)
                emulation = model["machine"].get("emulation", {})
                if name in RACKED_TOPOLOGIES:
                    self.assertIn("rack_numbers", emulation)
                else:
                    self.assertNotIn("rack_numbers", emulation)


if __name__ == "__main__":
    unittest.main()
