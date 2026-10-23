# SPDX-License-Identifier: BSD-3-Clause

"""Resolve a product's graphics positions against the shared catalogue.

A product layer's `graphics` section names the places a head can go and,
for each, the configuration ids it lists. `graphics-heads.yaml` beside
the topologies holds every configuration once, so one id names one
product, parameter set and option set wherever it is listed. The result
is the topology's `machine.graphics`, which the compiler copies into the
catalogue unchanged.
"""

from __future__ import annotations

import copy
import pathlib
import re
from typing import Any

import yaml

from .exceptions import ResolutionError

GRAPHICS_HEADS_FILE = "graphics-heads.yaml"

# A heads.<position> key fragment must be a QAPI name, and a purely
# numeric fragment would be read as a list index.
_POSITION_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")
_CONFIGURATION_ID = re.compile(r"^[a-z0-9][a-z0-9-]*$")

_FAMILIES = {"none", "mgras", "odyssey", "kona", "psitech"}
_PARAMETERS = {
    "geometry_engines", "raster_engines", "texture_mib", "board_memory_mib",
    "geometry_board", "raster_manager", "raster_manager_count",
    "display_generator", "display_channels",
}
_CONFIGURATION_FIELDS = {"id", "family", "family_name", "product",
                         "configuration", "parameters", "options"}
_POSITION_FIELDS = {"name", "names", "description", "built_in", "modelled",
                    "reason", "change", "options"}
_OPTION_FIELDS = {"id", "default", "refused", "fidelity", "limitations",
                  "pairing", "assembly", "change", "accepted_memory_mib",
                  "processors", "requires"}
_FIDELITY = {"validated", "implemented", "approximated"}
_PAIRING = {"established", "inferred"}
_CHANGE = {"at-creation", "between-boots"}
_REQUIREMENT_KINDS = {"crossbow-revision", "power-supply", "prom-revision",
                      "guest-release"}


def load_graphics_configurations(model_root: pathlib.Path) -> dict[str, dict]:
    """Every configuration in @model_root's graphics-heads.yaml, by id."""
    path = model_root / GRAPHICS_HEADS_FILE
    try:
        document = yaml.safe_load(path.read_text())
    except (OSError, yaml.YAMLError) as error:
        raise ResolutionError(f"cannot read {path}: {error}") from error
    if (not isinstance(document, dict) or
            document.get("graphics_heads_version") != 1 or
            not isinstance(document.get("configurations"), list)):
        raise ResolutionError(f"{path} is not a version 1 graphics catalogue")
    configurations: dict[str, dict] = {}
    for entry in document["configurations"]:
        if not isinstance(entry, dict) or set(entry) - _CONFIGURATION_FIELDS:
            raise ResolutionError(f"{path} has an invalid configuration {entry!r}")
        ident = entry.get("id")
        if not isinstance(ident, str) or not _CONFIGURATION_ID.match(ident):
            raise ResolutionError(f"{path} has an invalid configuration id {ident!r}")
        if ident in configurations:
            raise ResolutionError(f"{path} lists configuration '{ident}' twice")
        if entry.get("family") not in _FAMILIES:
            raise ResolutionError(f"configuration '{ident}' has an unknown family")
        for field in ("family_name", "product", "configuration"):
            if not isinstance(entry.get(field), str):
                raise ResolutionError(f"configuration '{ident}' needs a {field} string")
        if not entry["product"]:
            raise ResolutionError(f"configuration '{ident}' needs a product")
        parameters = entry.get("parameters")
        if not isinstance(parameters, dict) or set(parameters) - _PARAMETERS:
            raise ResolutionError(f"configuration '{ident}' has invalid parameters")
        options = entry.get("options")
        if options is not None and (
                not isinstance(options, list) or not options or
                not all(isinstance(item, str) and item for item in options)):
            raise ResolutionError(f"configuration '{ident}' has invalid options")
        if (ident == "none") != (entry["family"] == "none"):
            raise ResolutionError("only configuration 'none' has family none")
        configurations[ident] = copy.deepcopy(entry)
    if "none" not in configurations:
        raise ResolutionError(f"{path} has no 'none' configuration")
    return configurations


def _requirements(value: Any, label: str) -> list[dict]:
    if not isinstance(value, list) or not value:
        raise ResolutionError(f"{label} requires must be a non-empty list")
    result = []
    for item in value:
        if (not isinstance(item, dict) or
                set(item) - {"kind", "value", "evidence", "met"} or
                item.get("kind") not in _REQUIREMENT_KINDS or
                not isinstance(item.get("value"), str) or
                not isinstance(item.get("evidence"), str) or
                ("met" in item and type(item["met"]) is not bool)):
            raise ResolutionError(f"{label} has an invalid requirement")
        result.append(copy.deepcopy(item))
    return result


def _option(raw: Any, configurations: dict[str, dict], change: str,
            label: str) -> dict:
    if not isinstance(raw, dict) or set(raw) - _OPTION_FIELDS:
        raise ResolutionError(f"{label} has an invalid option {raw!r}")
    ident = raw.get("id")
    if ident not in configurations:
        raise ResolutionError(f"{label} lists unknown configuration {ident!r}")
    label = f"{label} option '{ident}'"
    refused = raw.get("refused")
    selectable = refused is None
    if not selectable and (not isinstance(refused, str) or not refused):
        raise ResolutionError(f"{label} refused must be a reason")
    fidelity = raw.get("fidelity")
    if selectable != (fidelity is not None):
        raise ResolutionError(
            f"{label} must carry fidelity exactly when it is selectable")
    if fidelity is not None and fidelity not in _FIDELITY:
        raise ResolutionError(f"{label} has an unknown fidelity")
    option: dict[str, Any] = {
        "configuration": copy.deepcopy(configurations[ident]),
        "default": raw.get("default", False),
        "selectable": selectable,
        "change": raw.get("change", change),
    }
    if type(option["default"]) is not bool:
        raise ResolutionError(f"{label} default must be a boolean")
    if option["default"] and not selectable:
        raise ResolutionError(f"{label} is the default but refused")
    if option["change"] not in _CHANGE:
        raise ResolutionError(f"{label} has an unknown change")
    if not selectable:
        option["refused"] = refused
    if fidelity is not None:
        option["fidelity"] = fidelity
    if "limitations" in raw:
        limitations = raw["limitations"]
        if (not isinstance(limitations, list) or not limitations or
                not all(isinstance(item, str) and item for item in limitations)):
            raise ResolutionError(f"{label} limitations must be strings")
        option["limitations"] = list(limitations)
    if fidelity == "approximated" and "limitations" not in option:
        raise ResolutionError(f"{label} is approximated without limitations")
    if "pairing" in raw:
        if raw["pairing"] not in _PAIRING:
            raise ResolutionError(f"{label} has an unknown pairing")
        option["pairing"] = raw["pairing"]
    if "assembly" in raw:
        if not isinstance(raw["assembly"], str) or not raw["assembly"]:
            raise ResolutionError(f"{label} assembly must be a part")
        option["assembly"] = raw["assembly"]
    if "accepted_memory_mib" in raw:
        sizes = raw["accepted_memory_mib"]
        if (not isinstance(sizes, list) or not sizes or
                not all(type(size) is int and size > 0 for size in sizes)):
            raise ResolutionError(f"{label} accepted_memory_mib must be sizes")
        option["accepted_memory_mib"] = list(sizes)
    if "processors" in raw:
        processors = raw["processors"]
        if (not isinstance(processors, list) or not processors or
                not all(isinstance(item, str) and item for item in processors)):
            raise ResolutionError(f"{label} processors must be option ids")
        option["processors"] = list(processors)
    if "requires" in raw:
        option["requires"] = _requirements(raw["requires"], label)
    return option


def resolve_graphics(product_id: str, section: Any,
                     configurations: dict[str, dict]) -> dict:
    """The topology's graphics record from a product's `graphics` section."""
    label = f"product '{product_id}' graphics"
    if (not isinstance(section, dict) or
            set(section) - {"max_heads", "positions"}):
        raise ResolutionError(f"{label} must contain only max_heads and positions")
    max_heads = section.get("max_heads")
    if type(max_heads) is not int or max_heads < 0:
        raise ResolutionError(f"{label} max_heads must be a count")
    raw_positions = section.get("positions")
    if not isinstance(raw_positions, list) or not raw_positions:
        raise ResolutionError(f"{label} positions must be a non-empty list")
    positions: list[dict] = []
    for raw in raw_positions:
        if not isinstance(raw, dict) or set(raw) - _POSITION_FIELDS:
            raise ResolutionError(f"{label} has an invalid position {raw!r}")
        if ("name" in raw) == ("names" in raw):
            raise ResolutionError(f"{label} position needs one of name or names")
        names = [raw["name"]] if "name" in raw else raw["names"]
        if not isinstance(names, list) or not names:
            raise ResolutionError(f"{label} names must be a non-empty list")
        built_in = raw.get("built_in", False)
        modelled = raw.get("modelled", True)
        if type(built_in) is not bool or type(modelled) is not bool:
            raise ResolutionError(f"{label} built_in and modelled are booleans")
        reason = raw.get("reason")
        if modelled == (reason is not None):
            raise ResolutionError(
                f"{label} position needs a reason exactly when not modelled")
        description = raw.get("description")
        if not isinstance(description, str) or not description:
            raise ResolutionError(f"{label} position needs a description")
        change = raw.get("change",
                         "at-creation" if built_in else "between-boots")
        for name in names:
            if not isinstance(name, str) or not _POSITION_NAME.match(name):
                raise ResolutionError(
                    f"{label} position name {name!r} is not a QAPI name "
                    "starting with a letter")
            where = f"{label} position '{name}'"
            options = [_option(item, configurations, change, where)
                       for item in raw.get("options", [])]
            ids = [option["configuration"]["id"] for option in options]
            duplicate = next((ident for ident in ids if ids.count(ident) > 1),
                             None)
            if duplicate:
                raise ResolutionError(f"{where} lists '{duplicate}' twice")
            defaults = [option for option in options if option["default"]]
            if modelled and len(defaults) != 1:
                raise ResolutionError(f"{where} needs exactly one default")
            if not modelled and any(option["selectable"] for option in options):
                raise ResolutionError(f"{where} is not modelled but offers a head")
            if built_in and modelled and defaults[0]["configuration"]["id"] == "none":
                raise ResolutionError(f"{where} is built in but empty by default")
            position: dict[str, Any] = {
                "name": name, "description": description,
                "built_in": built_in, "modelled": modelled,
                "options": options,
            }
            if reason is not None:
                position["reason"] = reason
            positions.append(position)
    names = [position["name"] for position in positions]
    duplicate = next((name for name in names if names.count(name) > 1), None)
    if duplicate:
        raise ResolutionError(f"{label} lists position '{duplicate}' twice")
    modelled = [position for position in positions if position["modelled"]]
    heads = sum(1 for position in modelled
                if next(option for option in position["options"]
                        if option["default"])["configuration"]["id"] != "none")
    if heads > max_heads:
        raise ResolutionError(
            f"{label} defaults to {heads} heads, more than max_heads")
    headless = all(
        any(option["configuration"]["id"] == "none" and option["default"]
            for option in position["options"])
        for position in modelled)
    return {"headless": headless, "max_heads": max_heads,
            "positions": positions}


def check_one_configuration_per_id(topologies: list[dict]) -> None:
    """Refuse one id that names different configurations in two places."""
    seen: dict[str, tuple[str, dict]] = {}
    for topology in topologies:
        for position in (topology.get("graphics") or {}).get("positions", []):
            for option in position["options"]:
                configuration = option["configuration"]
                ident = configuration["id"]
                previous = seen.setdefault(ident, (topology["name"], configuration))
                if previous[1] != configuration:
                    raise ValueError(
                        f"graphics id '{ident}' names different configurations "
                        f"in topologies '{previous[0]}' and '{topology['name']}'")
