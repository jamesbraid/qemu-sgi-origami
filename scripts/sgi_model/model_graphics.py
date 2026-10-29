# SPDX-License-Identifier: BSD-3-Clause

"""Resolve a product's graphics slots against the shared product file.

A product layer's `graphics` section names the slots a topology has for a
graphics pipe, the families, products and setting values each slot
offers, the pipe the machine fits by default and the reason for every
combination that does not fit. `graphics-products.yaml` beside the
topologies holds each family's device, products and full value sets once,
so a product's name and description and a value's description are the
same wherever they appear. The result is the topology's
`machine.graphics`, which the compiler copies into the catalogue; QEMU
checks every graphics -device against it and the launcher reads it.
"""

from __future__ import annotations

import copy
import pathlib
import re
from typing import Any

import yaml

from .exceptions import ResolutionError

GRAPHICS_PRODUCTS_FILE = "graphics-products.yaml"

_NAME = re.compile(r"^[a-z][a-z0-9-]*$")
_SLOT_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")
_FAMILIES = ("mgras", "odyssey", "kona", "psitech")
_FIDELITY = {"validated", "implemented", "approximated"}
_PAIRING = {"established", "inferred"}

_FAMILY_FIELDS = {"family", "device", "name", "products", "settings"}
_PRODUCT_FIELDS = {"product", "name", "description"}
_SETTING_FIELDS = {"name", "description", "values"}
_VALUE_FIELDS = {"value", "description", "evidence"}
_SECTION_FIELDS = {"max_pipes", "max_pipes_sources", "headless",
                   "headless_refused", "machine_properties", "slots"}

# Each pipe slot's one-wire records take graphics instances 16 * index to
# 16 * index + 15 (hw/misc/sgi-kona.c), and an XIO board placed by slot name
# offsets its placeholder serials from instance 64 (hw/mips/sgi/xio-board.c),
# so a machine with several pipes lists at most four slots.
MULTI_PIPE_SLOTS = 4
_MACHINE_PROPERTIES_FIELDS = {"properties", "reason"}
_SLOT_FIELDS = {"name", "names", "description", "placement", "default",
                "families", "refusals"}
_OFFER_FIELDS = {"family", "products"}
_OFFERED_PRODUCT_FIELDS = {"product", "fidelity", "limitations", "pairing",
                           "assembly", "settings"}
_OFFERED_SETTING_FIELDS = {"values", "default"}
_REFUSAL_FIELDS = {"family", "product", "settings", "reason"}
_DEFAULT_FIELDS = {"family", "product", "settings"}


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ResolutionError(f"{label} must be a non-empty string")
    return value


def _reason(value: Any, label: str) -> str:
    reason = _string(value, label)
    if reason.endswith("."):
        raise ResolutionError(
            f"{label} must be a phrase without a final full stop, so QEMU "
            "and the launcher can embed it in a sentence")
    return reason


def load_graphics_products(model_root: pathlib.Path) -> dict[str, dict]:
    """Every family in @model_root's graphics-products.yaml, by family."""
    path = model_root / GRAPHICS_PRODUCTS_FILE
    try:
        document = yaml.safe_load(path.read_text())
    except (OSError, yaml.YAMLError) as error:
        raise ResolutionError(f"cannot read {path}: {error}") from error
    if (not isinstance(document, dict) or
            document.get("graphics_products_version") != 1 or
            not isinstance(document.get("families"), list)):
        raise ResolutionError(f"{path} is not a version 1 graphics product file")
    families: dict[str, dict] = {}
    for raw in document["families"]:
        if not isinstance(raw, dict) or set(raw) != _FAMILY_FIELDS:
            raise ResolutionError(f"{path} has an invalid family {raw!r}")
        family = raw["family"]
        if family not in _FAMILIES:
            raise ResolutionError(f"{path} names unknown family {family!r}")
        if family in families:
            raise ResolutionError(f"{path} lists family '{family}' twice")
        label = f"graphics family '{family}'"
        products: dict[str, dict] = {}
        for item in raw["products"]:
            if not isinstance(item, dict) or set(item) != _PRODUCT_FIELDS:
                raise ResolutionError(f"{label} has an invalid product {item!r}")
            product = _string(item["product"], f"{label} product")
            if not _NAME.match(product) or product in products:
                raise ResolutionError(f"{label} product {product!r} is invalid "
                                      "or repeated")
            products[product] = {
                "product": product,
                "name": _string(item["name"], f"{label} product name"),
                "description": _string(item["description"],
                                       f"{label} product description"),
            }
        if not products:
            raise ResolutionError(f"{label} has no products")
        settings: dict[str, dict] = {}
        for item in raw["settings"]:
            if (not isinstance(item, dict) or set(item) != _SETTING_FIELDS or
                    not isinstance(item["values"], list) or
                    not item["values"]):
                raise ResolutionError(f"{label} has an invalid setting {item!r}")
            name = _string(item["name"], f"{label} setting")
            if not _NAME.match(name) or name in settings or name == "product":
                raise ResolutionError(f"{label} setting {name!r} is invalid "
                                      "or repeated")
            values: dict[str, dict] = {}
            for value in item["values"]:
                if (not isinstance(value, dict) or
                        not {"value", "description"} <= set(value) or
                        set(value) - _VALUE_FIELDS):
                    raise ResolutionError(
                        f"{label} setting '{name}' has an invalid value "
                        f"{value!r}")
                text = _string(value["value"], f"{label} setting '{name}' value")
                if text in values:
                    raise ResolutionError(
                        f"{label} setting '{name}' lists '{text}' twice")
                values[text] = {key: _string(value[key], f"{label} {key}")
                                for key in value}
            settings[name] = {
                "name": name,
                "description": _string(item["description"],
                                       f"{label} setting description"),
                "values": values,
            }
        families[family] = {
            "family": family,
            "device": _string(raw["device"], f"{label} device"),
            "name": _string(raw["name"], f"{label} name"),
            "products": products,
            "settings": settings,
        }
    return families


def _slot_text(text: str, name: str, label: str) -> str:
    """@text with {name} standing for slot @name and {number} for the
    number it ends with."""
    number = re.search(r"([0-9]+)$", name)
    if "{number}" in text and not number:
        raise ResolutionError(f"{label} needs a slot name that ends with a "
                              "number")
    return text.replace("{name}", name).replace(
        "{number}", number.group(1) if number else "")


def _placement(raw: Any, name: str, label: str) -> list[dict]:
    """The -device properties that put a pipe in slot @name: the layer's
    placement mapping, with {name} standing for the slot's name and
    {number} for the number it ends with."""
    if not isinstance(raw, dict) or not raw:
        raise ResolutionError(f"{label} needs a placement mapping")
    result = []
    for key, value in raw.items():
        if not isinstance(key, str) or not isinstance(value, (str, int)):
            raise ResolutionError(f"{label} has an invalid placement")
        result.append({"name": key,
                       "value": _slot_text(str(value), name,
                                           f"{label} placement")})
    return result


def _offered_settings(raw: Any, family: dict, label: str) -> list[dict]:
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ResolutionError(f"{label} settings must be a mapping")
    unknown = set(raw) - set(family["settings"])
    if unknown:
        raise ResolutionError(f"{label} names unknown settings "
                              f"{sorted(unknown)}")
    result = []
    # The family's order, so every record lists settings alike.
    for name, setting in family["settings"].items():
        if name not in raw:
            continue
        item = raw[name]
        if (not isinstance(item, dict) or set(item) - _OFFERED_SETTING_FIELDS or
                not isinstance(item.get("values"), list) or
                not item["values"]):
            raise ResolutionError(f"{label} setting '{name}' needs values")
        values = [str(value) for value in item["values"]]
        if len(values) != len(set(values)):
            raise ResolutionError(f"{label} setting '{name}' repeats a value")
        for value in values:
            if value not in setting["values"]:
                raise ResolutionError(
                    f"{label} setting '{name}' offers '{value}', which "
                    f"{GRAPHICS_PRODUCTS_FILE} does not list")
        if len(values) > 1 and "default" not in item:
            raise ResolutionError(f"{label} setting '{name}' needs a default")
        default = str(item.get("default", values[0]))
        if default not in values:
            raise ResolutionError(
                f"{label} setting '{name}' default '{default}' is not offered")
        result.append({
            "name": name,
            "description": setting["description"],
            "values": [copy.deepcopy(setting["values"][value])
                       for value in values],
            "default": default,
            "changeable": False,
        })
    return result


def _offered_product(raw: Any, family: dict, label: str) -> dict:
    if not isinstance(raw, dict) or set(raw) - _OFFERED_PRODUCT_FIELDS:
        raise ResolutionError(f"{label} has an invalid product {raw!r}")
    product = raw.get("product")
    if product not in family["products"]:
        raise ResolutionError(
            f"{label} offers {family['family']} product {product!r}, which "
            f"{GRAPHICS_PRODUCTS_FILE} does not list")
    label = f"{label} product '{product}'"
    fidelity = raw.get("fidelity")
    if fidelity not in _FIDELITY:
        raise ResolutionError(f"{label} needs a fidelity")
    result: dict[str, Any] = copy.deepcopy(family["products"][product])
    result["fidelity"] = fidelity
    limitations = raw.get("limitations")
    if limitations is not None:
        if (not isinstance(limitations, list) or not limitations or
                not all(isinstance(item, str) and item
                        for item in limitations)):
            raise ResolutionError(f"{label} limitations must be strings")
        result["limitations"] = [
            _reason(item, f"{label} limitation") for item in limitations]
    if fidelity == "approximated" and limitations is None:
        raise ResolutionError(f"{label} is approximated without limitations")
    if "pairing" in raw:
        if raw["pairing"] not in _PAIRING:
            raise ResolutionError(f"{label} has an unknown pairing")
        result["pairing"] = raw["pairing"]
    if "assembly" in raw:
        result["assembly"] = _string(raw["assembly"], f"{label} assembly")
    result["settings"] = _offered_settings(raw.get("settings"), family, label)
    return result


def _properties(raw: Any, label: str) -> list[dict]:
    if not isinstance(raw, dict):
        raise ResolutionError(f"{label} settings must be a mapping")
    return [{"name": str(name), "value": str(value)}
            for name, value in raw.items()]


def _refusal(raw: Any, families: dict[str, dict], label: str) -> dict:
    if (not isinstance(raw, dict) or set(raw) - _REFUSAL_FIELDS or
            "family" not in raw or "reason" not in raw):
        raise ResolutionError(f"{label} has an invalid refusal {raw!r}")
    family = families.get(raw["family"])
    if family is None:
        raise ResolutionError(f"{label} refusal names unknown family "
                              f"{raw['family']!r}")
    result: dict[str, Any] = {"family": family["family"]}
    if "product" in raw:
        if raw["product"] not in family["products"]:
            raise ResolutionError(
                f"{label} refusal names product {raw['product']!r}, which "
                f"{GRAPHICS_PRODUCTS_FILE} does not list")
        result["product"] = raw["product"]
    if "settings" in raw:
        settings = _properties(raw["settings"], f"{label} refusal")
        for item in settings:
            setting = family["settings"].get(item["name"])
            if setting is None or item["value"] not in setting["values"]:
                raise ResolutionError(
                    f"{label} refusal names {item['name']}={item['value']}, "
                    f"which {GRAPHICS_PRODUCTS_FILE} does not list")
        if not settings:
            raise ResolutionError(f"{label} refusal settings are empty")
        result["settings"] = settings
    result["reason"] = _reason(raw["reason"], f"{label} refusal reason")
    return result


def refusal_matches(refusal: dict, family: str, product: str,
                    settings: dict[str, str]) -> bool:
    """Whether @refusal matches a pipe of @family and @product with
    @settings: its family matches, its product matches if it names one,
    and every setting it lists has the requested value. QEMU and the
    launcher apply the same rule."""
    if refusal["family"] != family:
        return False
    if "product" in refusal and refusal["product"] != product:
        return False
    return all(settings.get(item["name"]) == item["value"]
               for item in refusal.get("settings", []))


def _default(raw: Any, slot: dict, label: str) -> dict:
    if not isinstance(raw, dict) or set(raw) - _DEFAULT_FIELDS:
        raise ResolutionError(f"{label} has an invalid default {raw!r}")
    offer = next((item for item in slot["families"]
                  if item["family"] == raw.get("family")), None)
    if offer is None:
        raise ResolutionError(f"{label} default family {raw.get('family')!r} "
                              "is not one the slot takes")
    product = next((item for item in offer["products"]
                    if item["product"] == raw.get("product")), None)
    if product is None:
        raise ResolutionError(f"{label} default product "
                              f"{raw.get('product')!r} is not offered")
    given = {item["name"]: item["value"]
             for item in _properties(raw.get("settings", {}), label)}
    unknown = set(given) - {item["name"] for item in product["settings"]}
    if unknown:
        raise ResolutionError(f"{label} default sets {sorted(unknown)}, "
                              "which the product does not offer here")
    resolved = {}
    for setting in product["settings"]:
        value = given.get(setting["name"], setting["default"])
        if value not in [item["value"] for item in setting["values"]]:
            raise ResolutionError(f"{label} default {setting['name']}={value} "
                                  "is not offered")
        resolved[setting["name"]] = value
    for refusal in slot.get("refusals", []):
        if refusal_matches(refusal, offer["family"], product["product"],
                           resolved):
            raise ResolutionError(f"{label} default is refused: "
                                  f"{refusal['reason']}")
    return {"family": offer["family"], "product": product["product"],
            "settings": [{"name": name, "value": value}
                         for name, value in resolved.items()]}


def resolve_graphics(product_id: str, section: Any,
                     families: dict[str, dict]) -> dict:
    """The topology's graphics record from a product's `graphics` section."""
    label = f"product '{product_id}' graphics"
    if not isinstance(section, dict) or set(section) - _SECTION_FIELDS:
        raise ResolutionError(f"{label} has unknown fields")
    max_pipes = section.get("max_pipes")
    if type(max_pipes) is not int or max_pipes < 1:
        raise ResolutionError(f"{label} max_pipes must be a whole number "
                              "of at least 1")
    sources = section.get("max_pipes_sources")
    if (max_pipes > 1) != (sources is not None):
        raise ResolutionError(f"{label} needs max_pipes_sources exactly when "
                              "max_pipes is above 1")
    if sources is not None and (
            not isinstance(sources, list) or not sources or
            not all(isinstance(item, str) and item for item in sources)):
        raise ResolutionError(f"{label} max_pipes_sources must be a "
                              "non-empty list of citations")
    raw_slots = section.get("slots")
    if not isinstance(raw_slots, list) or not raw_slots:
        raise ResolutionError(f"{label} slots must be a non-empty list")
    slots: list[dict] = []
    for raw in raw_slots:
        if not isinstance(raw, dict) or set(raw) - _SLOT_FIELDS:
            raise ResolutionError(f"{label} has an invalid slot {raw!r}")
        if ("name" in raw) == ("names" in raw):
            raise ResolutionError(f"{label} slot needs one of name or names")
        names = [raw["name"]] if "name" in raw else raw["names"]
        if not isinstance(names, list) or not names:
            raise ResolutionError(f"{label} names must be a non-empty list")
        for name in names:
            if not isinstance(name, str) or not _SLOT_NAME.match(name):
                raise ResolutionError(f"{label} slot name {name!r} is invalid")
            where = f"{label} slot '{name}'"
            slot: dict[str, Any] = {
                "name": name,
                "description": _slot_text(
                    _string(raw.get("description"), f"{where} description"),
                    name, f"{where} description"),
                "placement": _placement(raw.get("placement"), name, where),
                "changeable": False,
                "families": [],
            }
            raw_families = raw.get("families", [])
            if not isinstance(raw_families, list):
                raise ResolutionError(f"{where} families must be a list")
            for item in raw_families:
                if not isinstance(item, dict) or set(item) - _OFFER_FIELDS:
                    raise ResolutionError(f"{where} has an invalid family")
                family = families.get(item.get("family"))
                if family is None:
                    raise ResolutionError(f"{where} names unknown family "
                                          f"{item.get('family')!r}")
                if any(offer["family"] == family["family"]
                       for offer in slot["families"]):
                    raise ResolutionError(
                        f"{where} lists family '{family['family']}' twice")
                products = [_offered_product(product, family, where)
                            for product in item.get("products", [])]
                ids = [product["product"] for product in products]
                if not ids or len(ids) != len(set(ids)):
                    raise ResolutionError(f"{where} family "
                                          f"'{family['family']}' needs "
                                          "distinct products")
                slot["families"].append({
                    "family": family["family"], "device": family["device"],
                    "name": family["name"], "products": products,
                })
            if not slot["families"]:
                raise ResolutionError(f"{where} takes no family")
            refusals = [_refusal(item, families, where)
                        for item in raw.get("refusals", [])]
            if refusals:
                slot["refusals"] = refusals
            if "default" in raw:
                slot["default"] = _default(raw["default"], slot, where)
            slots.append(slot)
    names = [slot["name"] for slot in slots]
    duplicate = next((name for name in names if names.count(name) > 1), None)
    if duplicate:
        raise ResolutionError(f"{label} lists slot '{duplicate}' twice")
    # A launcher shows each slot by its description.
    texts = [slot["description"] for slot in slots]
    duplicate = next((text for text in texts if texts.count(text) > 1), None)
    if duplicate:
        raise ResolutionError(f"{label} describes two slots as "
                              f"'{duplicate}'")
    if max_pipes > len(slots):
        raise ResolutionError(f"{label} max_pipes {max_pipes} is more than "
                              f"its {len(slots)} slots can hold")
    if max_pipes > 1 and len(slots) > MULTI_PIPE_SLOTS:
        raise ResolutionError(
            f"{label} lists {len(slots)} slots, and a topology with several "
            f"pipes lists at most {MULTI_PIPE_SLOTS}: each slot's one-wire "
            "serials take sixteen graphics instances, below the 64 where "
            "boards placed by slot name start")
    defaults = [slot for slot in slots if "default" in slot]
    if len(defaults) > 1:
        raise ResolutionError(f"{label} has more than one default pipe")
    headless = section.get("headless")
    if type(headless) is not bool:
        raise ResolutionError(f"{label} headless must be a boolean")
    if not defaults and not headless:
        raise ResolutionError(f"{label} has no default pipe, so it is "
                              "headless")
    if headless != ("headless_refused" not in section):
        raise ResolutionError(f"{label} needs headless_refused exactly when "
                              "headless is false")
    record: dict[str, Any] = {"max_pipes": max_pipes, "headless": headless}
    if not headless:
        record["headless_refused"] = _reason(section["headless_refused"],
                                             f"{label} headless_refused")
    if "machine_properties" in section:
        record["machine_properties"] = _machine_properties(
            section["machine_properties"], f"{label} machine_properties")
    record["slots"] = slots
    return record


def _machine_properties(raw: Any, label: str) -> dict:
    """The -M properties a launch fitting any pipe sets, and why."""
    if (not isinstance(raw, dict) or set(raw) != _MACHINE_PROPERTIES_FIELDS
            or not isinstance(raw["properties"], dict)
            or not raw["properties"]):
        raise ResolutionError(f"{label} needs a non-empty properties mapping "
                              "and a reason")
    properties = _properties(raw["properties"], label)
    for item in properties:
        if not _NAME.match(item["name"]) or not item["value"] or any(
                c in item["value"] for c in ",= "):
            raise ResolutionError(f"{label} has an invalid property "
                                  f"{item['name']}={item['value']!r}")
    return {"properties": properties,
            "reason": _reason(raw["reason"], f"{label} reason")}


def _offers(topologies: list[dict]):
    """Every (family, product) offered anywhere, and the values each
    product's settings take anywhere."""
    products: set[tuple[str, str]] = set()
    values: dict[tuple[str, str, str], set[str]] = {}
    for topology in topologies:
        for slot in (topology.get("graphics") or {}).get("slots", []):
            for offer in slot["families"]:
                for product in offer["products"]:
                    key = (offer["family"], product["product"])
                    products.add(key)
                    for setting in product["settings"]:
                        values.setdefault((*key, setting["name"]), set()).update(
                            item["value"] for item in setting["values"])
    return products, values


def check_catalogue(topologies: list[dict]) -> None:
    """The rules that span topologies: a product or value one slot offers
    and another slot of the same family does not is explained by a refusal
    there, and no refusal names a product or value no slot offers, so
    unmodelled hardware stays out of the catalogue."""
    products, values = _offers(topologies)
    for topology in topologies:
        for slot in (topology.get("graphics") or {}).get("slots", []):
            where = f"topology '{topology['name']}' slot '{slot['name']}'"
            refusals = slot.get("refusals", [])
            for refusal in refusals:
                family = refusal["family"]
                if ("product" in refusal and
                        (family, refusal["product"]) not in products):
                    raise ValueError(
                        f"{where} refuses {family} product "
                        f"'{refusal['product']}', which no slot offers")
                for item in refusal.get("settings", []):
                    offered = any(
                        item["value"] in found
                        for (fam, prod, name), found in values.items()
                        if fam == family and name == item["name"] and
                        refusal.get("product", prod) == prod)
                    if not offered:
                        raise ValueError(
                            f"{where} refuses {item['name']}={item['value']}, "
                            "which no slot offers")
            for offer in slot["families"]:
                family = offer["family"]
                here = {product["product"]: product
                        for product in offer["products"]}
                for fam, product in sorted(products):
                    if fam != family or product in here:
                        continue
                    if not any("settings" not in refusal and
                               refusal_matches(refusal, family, product, {})
                               for refusal in refusals):
                        raise ValueError(
                            f"{where} takes {family} but neither offers nor "
                            f"refuses product '{product}', which another "
                            "slot offers")
                for name, product in here.items():
                    offered = {setting["name"]: {item["value"] for item in
                                                 setting["values"]}
                               for setting in product["settings"]}
                    for (fam, prod, setting), found in sorted(values.items()):
                        if fam != family or prod != name:
                            continue
                        for value in sorted(found - offered.get(setting,
                                                                set())):
                            if not any(
                                    refusal["family"] == family and
                                    refusal.get("product", name) == name and
                                    refusal.get("settings") ==
                                    [{"name": setting, "value": value}]
                                    for refusal in refusals):
                                raise ValueError(
                                    f"{where} neither offers nor refuses "
                                    f"{name} {setting}={value}, which another "
                                    "slot offers")


def attach_slot_requirements(topology: dict) -> None:
    """Give each XIO slot the node count it exists from: a slot whose
    Crossbow port belongs to a node board that the smaller node
    populations do not fit is there only from that many nodes."""
    graphics = topology.get("graphics")
    if not graphics:
        return
    labels = [slot.get("label") for slot in topology.get("xio_slots", [])]
    populations = topology.get("node_populations", [])
    smallest = min((len(row["nodes"]) for row in populations), default=0)
    for slot in graphics["slots"]:
        placement = {item["name"]: item["value"] for item in slot["placement"]}
        name = placement.get("slot")
        if name is None or name not in labels:
            continue
        index = labels.index(name)
        counts = [len(row["nodes"]) for row in populations
                  if any(item["slot"] == index
                         for item in row.get("xio_slots", []))]
        if counts and min(counts) > smallest:
            slot["requires"] = {"min_nodes": min(counts)}
