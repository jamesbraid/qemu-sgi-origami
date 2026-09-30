# SPDX-License-Identifier: BSD-3-Clause

"""Resolve layered system models into complete consumer dictionaries."""

from __future__ import annotations

import copy
import pathlib
import re
from typing import Any

import yaml


from .exceptions import ResolutionError
from . import model_composition


def _cpu_variant_pair(value: Any, label: str) -> dict[str, int]:
    if (not isinstance(value, dict) or set(value) - CPU_VARIANT_FIELDS or
            not isinstance(value.get("clock_mhz"), int) or
            not isinstance(value.get("cache_mib"), int)):
        raise ResolutionError(f"{label} must name only clock_mhz and cache_mib")
    return {"clock_mhz": value["clock_mhz"], "cache_mib": value["cache_mib"]}


CPU_IDENTITY_FIELDS = ("processor_identification", "fpu_identification", "cache_clock_mhz")


def _attach_cpu_selection(
        components: list[Any], catalogue: list[Any],
        selection: dict[str, Any] | None) -> None:
    """Attach each named slot's catalogued CPU variant to its component.

    The architecture's processors catalogue carries the part's clock and
    cache SKUs once; a product names the installed default and, for a slot
    whose id appears in this particular components list, the resolver
    copies the matching variant onto it as `identity`. Called once on the
    resolved model's own components and once per module instance on its
    still-unprefixed copy, so a slot id is authored the way its owning
    module or product names it, never with an instance prefix.

    The catalogue may also carry a variant's identity registers and cache
    clock (`CPU_IDENTITY_FIELDS`), established for some clock/cache SKUs and
    not others -- unlike clock_mhz/cache_mib, a component may still author
    these directly (the Origin 200 family does, predating this catalogue
    entry); when both are present they must agree, and when the catalogue
    carries the field and the component does not, this fills it in.
    """
    if selection is None:
        return
    if not isinstance(selection, dict) or set(selection) - CPU_SELECTION_FIELDS:
        raise ResolutionError("cpu_selection must contain only default and slots")
    default = _cpu_variant_pair(selection.get("default"), "cpu_selection default")
    slots = selection.get("slots", [])
    if (not isinstance(slots, list) or
            not all(isinstance(item, str) and item for item in slots)):
        raise ResolutionError("cpu_selection slots must be a list of component ids")
    catalogue_by_part = {
        entry["part"]: entry for entry in catalogue if isinstance(entry, dict)
    }
    by_id = {
        component["id"]: component for component in components
        if isinstance(component, dict) and isinstance(component.get("id"), str)
    }
    for slot in slots:
        component = by_id.get(slot)
        if component is None:
            continue
        if component.get("kind") != "cpu":
            raise ResolutionError(f"cpu_selection slot '{slot}' is not a CPU component")
        identity = component.get("identity") or {}
        catalogued = CPU_VARIANT_FIELDS | {"variants"}
        if catalogued & set(identity):
            raise ResolutionError(
                f"CPU component '{slot}' authors {sorted(catalogued & set(identity))[0]} "
                "directly; cpu_selection must supply it"
            )
        part = component.get("hardware_kind")
        entry = catalogue_by_part.get(part)
        if entry is None:
            raise ResolutionError(
                f"CPU component '{slot}' names uncatalogued part '{part}'"
            )
        variants = entry.get("variants")
        if not isinstance(variants, list) or not variants:
            raise ResolutionError(f"catalogued part '{part}' has no variants")
        variant = next(
            (item for item in variants
             if item.get("clock_mhz") == default["clock_mhz"]
             and item.get("cache_mib") == default["cache_mib"]),
            None,
        )
        if variant is None:
            raise ResolutionError(
                f"CPU component '{slot}' selection {default} is not a "
                f"catalogued '{part}' variant"
            )
        merged = {
            **identity,
            "clock_mhz": variant["clock_mhz"],
            "cache_mib": variant["cache_mib"],
            "variants": [
                {"clock_mhz": item["clock_mhz"], "cache_mib": item["cache_mib"]}
                for item in variants
            ],
        }
        for field in CPU_IDENTITY_FIELDS:
            if field not in variant:
                continue
            if field in identity and identity[field] != variant[field]:
                raise ResolutionError(
                    f"CPU component '{slot}' authors {field} directly with a "
                    "value disagreeing with the catalogued part"
                )
            merged[field] = variant[field]
        component["identity"] = merged


def _attach_processor_reset_mode(
        resolved: dict[str, Any], catalogue: list[Any],
        selection: dict[str, Any] | None) -> None:
    """Fill in the catalogued reset-mode word for the product's default CPU.

    The processors catalogue carries the R10000 reset-mode fields once per
    clock and cache SKU; a product selecting that SKU gets the same mode
    word without re-authoring it. Only touches a product that already
    authors machine.firmware_config (an emulation target's own board and
    flash facts establish that); a reference projection or one naming an
    uncatalogued reset mode is left alone. A product that authors
    processor_reset_mode directly must still agree with the catalogue.
    """
    if not isinstance(selection, dict):
        return
    firmware_config = resolved.get("machine", {}).get("firmware_config")
    if not isinstance(firmware_config, dict):
        return
    default = selection.get("default")
    if not isinstance(default, dict):
        return
    reset_mode = None
    for entry in catalogue:
        if not isinstance(entry, dict):
            continue
        for variant in entry.get("variants", []):
            if (isinstance(variant, dict) and
                    variant.get("clock_mhz") == default.get("clock_mhz") and
                    variant.get("cache_mib") == default.get("cache_mib")):
                reset_mode = variant.get("reset_mode")
    if reset_mode is None:
        return
    existing = firmware_config.get("processor_reset_mode")
    if existing is None:
        firmware_config["processor_reset_mode"] = copy.deepcopy(reset_mode)
    elif existing != reset_mode:
        raise ResolutionError(
            "firmware_config processor_reset_mode disagrees with the "
            "catalogued reset mode for the selected CPU variant"
        )


REQUIRED_KINDS = {"architecture", "assembly", "product"}
KNOWN_KINDS = REQUIRED_KINDS | {"module"}
MODULE_GRAPH_FIELDS = (
    "components", "buses", "connections", "interrupts", "open_questions"
)
MODULE_INSTANCE_FIELDS = {
    "model", "prefix", "parent", "omit_buses",
    "observed_hardware", "module_role", "physical_slot", "bus_bindings",
    "adopt_roots",
}
NESTED_MODULE_INSTANCE_FIELDS = {"model", "id", "parent", "physical_slot"}
OBSERVED_HARDWARE_FIELDS = {"mode", "sources", "note"}
CPU_SELECTION_FIELDS = {"default", "slots"}
CPU_VARIANT_FIELDS = {"clock_mhz", "cache_mib"}


def _resolve_source_key(key: str, table: dict[str, Any], what: str) -> str:
    """Look up one short source-table key and return its citation string.

    A citation checker (tools/check-citations) now resolves a fact's own
    sources: entries directly against the document catalogue, so a table
    entry carries the finished citation and nothing folds several entries'
    shared boilerplate together the way the retired {prefix, suffix} form
    once did; a file still keeping a per-file table is one the citation
    checker reports as not yet converted to direct citations.
    """
    entry = table[key]
    if not isinstance(entry, str) or not entry:
        raise ResolutionError(f"{what} sources table entry '{key}' must be a non-empty string")
    return entry


def _expand_source_table(value: Any, table: dict[str, Any], what: str) -> Any:
    """Replace every "sources:" list's table-key strings with their full
    citation, leaving anything not found in the table untouched -- a plain
    citation string authored directly, without a table entry, still works."""
    if isinstance(value, dict):
        result = {}
        for key, child in value.items():
            if key == "sources" and isinstance(child, list):
                result[key] = [
                    _resolve_source_key(item, table, what) if
                    isinstance(item, str) and item in table else item
                    for item in child
                ]
            else:
                result[key] = _expand_source_table(child, table, what)
        return result
    if isinstance(value, list):
        return [_expand_source_table(item, table, what) for item in value]
    return value


def _read_yaml(path: pathlib.Path, what: str) -> dict[str, Any]:
    try:
        document = yaml.safe_load(path.read_text())
    except FileNotFoundError as exc:
        raise ResolutionError(f"missing {what} '{path}'") from exc
    except yaml.YAMLError as exc:
        raise ResolutionError(f"{what} '{path}' is not parseable YAML: {exc}") from exc

    if not isinstance(document, dict):
        raise ResolutionError(f"{what} '{path}' must be a mapping")
    table = document.pop("sources", None)
    if table is not None:
        label = f"{what} '{path}'"
        if not isinstance(table, dict):
            raise ResolutionError(f"{label} sources table must be a mapping")
        document = _expand_source_table(document, table, label)
    return document


def _layer(path: pathlib.Path, document: dict[str, Any] | None = None) -> dict[str, Any]:
    if document is None:
        document = _read_yaml(path, "layer")
    if document.get("layer_version") != 1:
        raise ResolutionError(f"layer '{path}' has no supported layer_version")
    layer = document.get("layer")
    if not isinstance(layer, dict):
        raise ResolutionError(f"layer '{path}' has no layer mapping")
    kind = layer.get("kind")
    ident = layer.get("id")
    if kind not in KNOWN_KINDS:
        raise ResolutionError(f"layer '{path}' has unknown kind '{kind}'")
    if not isinstance(ident, str) or not ident:
        raise ResolutionError(f"{kind} layer '{path}' has no id")
    if kind == "assembly" and (
            not isinstance(layer.get("platform"), str) or not layer["platform"]):
        raise ResolutionError(f"assembly '{ident}' has no software platform identity")
    if kind == "module":
        data = layer.get("data")
        if (not isinstance(data, dict) or
                set(data) - set(MODULE_GRAPH_FIELDS) - {"module_instances"}):
            raise ResolutionError(f"module '{ident}' must contain only physical graph collections")
        if "module_instances" in data:
            layer["data"] = data = _expand_nested_module_instances(path, data)
        model_composition.validate_component_ids(data)
    return layer


def _require_layers(selected: dict[str, dict[str, Any]]) -> None:
    for layer in selected.values():
        kind = layer["kind"]
        ident = layer["id"]
        requirements = layer.get("requires", {})
        if not isinstance(requirements, dict):
            raise ResolutionError(f"{kind} '{ident}' requirements must be a mapping")
        for required_kind, required_id in requirements.items():
            actual = selected.get(required_kind, {}).get("id")
            if actual != required_id:
                required = f"{required_kind} '{required_id}'"
                detail = (f"missing {required}" if actual is None else
                          f"{required}, got '{actual}'")
                raise ResolutionError(f"{kind} '{ident}' requires {detail}")


def _record_owners(
        owners: dict[str, str], value: Any, path: tuple[str, ...],
        owner: str) -> None:
    owners[".".join(path)] = owner
    if isinstance(value, dict):
        for key, child in value.items():
            _record_owners(owners, child, (*path, key), owner)


def _merge_mapping(
        target: dict[str, Any], incoming: dict[str, Any],
        owners: dict[str, str], owner: str,
        path: tuple[str, ...] = ()) -> None:
    for key, value in incoming.items():
        key_path = (*path, key)
        field = ".".join(key_path)
        if key not in target:
            target[key] = copy.deepcopy(value)
            _record_owners(owners, value, key_path, owner)
            continue
        current = target[key]
        if isinstance(current, dict) and isinstance(value, dict):
            _merge_mapping(current, value, owners, owner, key_path)
            continue
        if current != value:
            raise ResolutionError(
                f"conflicting resolved field '{field}' is supplied by both "
                f"{owners[field]} and {owner}"
            )


def _merge_data(selected: dict[str, dict[str, Any]]) -> dict[str, Any]:
    resolved: dict[str, Any] = {}
    owners: dict[str, str] = {}
    for kind in ("architecture", "assembly"):
        layer = selected[kind]
        contract = layer.get("contract")
        if contract is None:
            continue
        if not isinstance(contract, dict):
            raise ResolutionError(f"{kind} '{layer['id']}' contract must be a mapping")
        _merge_mapping(resolved, {"machine": contract}, owners,
                       f"{kind} '{layer['id']}' contract")
    for layer in selected.values():
        data = layer.get("data", {})
        if not isinstance(data, dict):
            raise ResolutionError(
                f"{layer['kind']} '{layer['id']}' data must be a mapping"
            )
        _merge_mapping(
            resolved, data, owners,
            layer.get("owner", f"{layer['kind']} '{layer['id']}'"),
        )
    return resolved


def contains_observed_hardware(value: Any) -> bool:
    """Whether any nested fact claims a hardware observation."""
    if isinstance(value, dict):
        return (value.get("provenance") == "observed-hardware" or
                any(contains_observed_hardware(child)
                    for child in value.values()))
    if isinstance(value, list):
        return any(contains_observed_hardware(child) for child in value)
    return False


def _validate_observed_hardware_basis(value: Any) -> dict[str, Any] | None:
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ResolutionError("observed_hardware instance basis must be a mapping")
    unknown = sorted(set(value) - OBSERVED_HARDWARE_FIELDS)
    if unknown:
        raise ResolutionError(
            f"observed_hardware instance basis has unsupported field '{unknown[0]}'"
        )
    if value.get("mode") not in {"direct", "projected"}:
        raise ResolutionError(
            "observed_hardware instance basis mode must be direct or projected"
        )
    sources = value.get("sources")
    if (not isinstance(sources, list) or not sources or
            not all(isinstance(source, str) and source for source in sources)):
        raise ResolutionError(
            "observed_hardware instance basis requires non-empty sources"
        )
    note = value.get("note")
    if not isinstance(note, str) or not note:
        raise ResolutionError(
            "observed_hardware instance basis requires a non-empty note"
        )
    return copy.deepcopy(value)


def _apply_observed_hardware_basis(value: Any, basis: dict[str, Any]) -> Any:
    if isinstance(value, dict):
        rewritten = {
            key: _apply_observed_hardware_basis(child, basis)
            for key, child in value.items()
        }
        if value.get("provenance") == "observed-hardware":
            if basis["mode"] == "projected":
                rewritten["provenance"] = "derived"
            rewritten["sources"] = list(dict.fromkeys(
                copy.deepcopy(value.get("sources", [])) +
                copy.deepcopy(basis["sources"])
            ))
        return rewritten
    if isinstance(value, list):
        return [_apply_observed_hardware_basis(child, basis) for child in value]
    return copy.deepcopy(value)


def _instantiate_module_graph(
        contract: dict[str, Any], *, prefix: str, parent: str,
        omit_buses: list[str],
        module_index: int = 0,
        observed_hardware: dict[str, Any] | None = None,
        root_id: str | None = None,
        bus_bindings: dict[str, str] | None = None,
        adopt_roots: bool = False) -> dict[str, list[Any]]:
    """Copy one resolved machine's graph as a prefixed physical module.

    This deliberately imports only the graph collections. Machine identity,
    address spaces, firmware configuration and product population remain owned
    by the composition that instantiates the module.

    `bus_bindings` maps a bus the module names but does not declare -- the
    upstream bus a fitted board plugs into -- to a bus of the enclosing
    graph, so one board layer fits any slot. Every undeclared bus a
    connection names must be bound, and every binding must be used.

    `root_id`, when given, replaces the prefixed containment root's id
    everywhere a reference would otherwise get `prefix` prepended -- a
    nested module_instances declaration names its own instance directly
    (an Origin 2000 node board becomes "node-board-1", not
    "node-board-1-node-board") while its interior still gets the ordinary
    prefix.

    `adopt_roots` accepts a module authored flat, with several containment
    roots, and makes every one of them a child of `parent`. The Origin 300
    base module is such a layer: its one-chassis composition merges it
    directly, where the product itself is the chassis, so it names no
    chassis component of its own. A composition that instances it twice
    supplies the enclosing module component instead. Without the flag a
    second root is still refused, since it usually means a component whose
    parent was left out by mistake.
    """
    graph: dict[str, list[Any]] = {}
    for field in MODULE_GRAPH_FIELDS:
        values = contract.get(field, [])
        if not isinstance(values, list):
            raise ResolutionError(
                f"module contract field '{field}' must be a list"
            )
        graph[field] = copy.deepcopy(values)

    basis = _validate_observed_hardware_basis(observed_hardware)
    if contains_observed_hardware(graph):
        if basis is None:
            raise ResolutionError(
                "module instance copies observed-hardware claims without an instance basis"
            )
        graph = _apply_observed_hardware_basis(graph, basis)

    components = graph["components"]
    omitted_components = {
        component["id"] for component in components
        if (isinstance(component, dict) and
            isinstance(component.get("id"), str) and
            isinstance(component.get("optional"), dict) and
            component["optional"].get("node") != module_index)
    }
    while True:
        omitted_children = {
            component["id"] for component in components
            if (isinstance(component, dict) and
                isinstance(component.get("id"), str) and
                component.get("parent") in omitted_components)
        }
        if omitted_children <= omitted_components:
            break
        omitted_components |= omitted_children
    components = [
        component for component in components
        if component.get("id") not in omitted_components
    ]
    graph["components"] = components
    buses = graph["buses"]
    component_ids = {
        item.get("id") for item in components if isinstance(item, dict)
    }
    bus_ids = {item.get("id") for item in buses if isinstance(item, dict)}
    containment_roots = sorted(
        item["id"] for item in components
        if (isinstance(item, dict) and isinstance(item.get("id"), str) and
            item.get("parent") not in component_ids)
    )
    if not containment_roots:
        raise ResolutionError("module contract has no containment root")
    if len(containment_roots) > 1 and not (adopt_roots and root_id is None):
        raise ResolutionError(
            "module contract has multiple containment roots: " +
            ", ".join(containment_roots)
        )
    root = containment_roots[0]
    roots = set(containment_roots)

    def _renamed(ident: str) -> str:
        return root_id if (root_id is not None and ident == root) else prefix + ident

    unknown_omissions = sorted(set(omit_buses) - bus_ids)
    if unknown_omissions:
        raise ResolutionError(
            f"module instance omits unknown bus '{unknown_omissions[0]}'"
        )

    for bus in buses:
        if bus.get("id") not in omit_buses and "masters" in bus:
            for master in bus["masters"]:
                if master in omitted_components:
                    raise ResolutionError(
                        f"retained I2C bus '{bus['id']}' omits master '{master}'")

    graph["buses"] = [
        bus for bus in buses
        if (bus.get("id") not in omit_buses and
            bus.get("master") not in omitted_components)
    ]
    bindings = {} if bus_bindings is None else bus_bindings
    if (not isinstance(bindings, dict) or
            not all(isinstance(key, str) and key and isinstance(value, str) and value
                    for key, value in bindings.items())):
        raise ResolutionError("module instance bus_bindings must map bus ids to bus ids")
    undeclared = {
        connection.get("bus") for connection in graph["connections"]
        if connection.get("bus") not in bus_ids
    }
    for bus_id in sorted(undeclared - bindings.keys()):
        raise ResolutionError(
            f"module connects to undeclared bus '{bus_id}' with no bus_bindings entry")
    for bus_id in sorted(bindings.keys() - undeclared):
        raise ResolutionError(
            f"module instance binds bus '{bus_id}', which is not an undeclared bus "
            "a connection names")
    retained_bus_ids = {bus.get("id") for bus in graph["buses"]}
    # A connection to a bound bus attaches the module to its enclosing graph
    # (a fitted board's Bridge on its connector's crosstalk bus); one to a
    # bus the instance omits or drops goes with the bus.
    graph["connections"] = [
        connection for connection in graph["connections"]
        if ((connection.get("bus") in retained_bus_ids or
             connection.get("bus") not in bus_ids) and
            connection.get("component") not in omitted_components and
            connection.get("controlled_by") not in omitted_components)
    ]
    graph["interrupts"] = [
        interrupt for interrupt in graph["interrupts"]
        if (interrupt.get("source") not in omitted_components and
            all(hop.get("at") not in omitted_components
                for hop in interrupt.get("chain", [])))
    ]
    for component in components:
        component["regions"] = [
            region for region in component.get("regions", [])
            if region.get("relative_to") not in omitted_components
        ]
    for question in graph["open_questions"]:
        question["about"] = [
            component_id for component_id in question.get("about", [])
            if component_id not in omitted_components
        ]
    for component in components:
        ident = component.get("id")
        if isinstance(ident, str):
            component["id"] = _renamed(ident)
        component_parent = component.get("parent")
        if component_parent in component_ids:
            component["parent"] = _renamed(component_parent)
        for region in component.get("regions", []):
            relative_to = region.get("relative_to")
            if relative_to in component_ids:
                region["relative_to"] = _renamed(relative_to)
        if ident in roots:
            component["parent"] = parent

    for bus in graph["buses"]:
        ident = bus.get("id")
        if isinstance(ident, str):
            bus["id"] = _renamed(ident)
        master = bus.get("master")
        if master in component_ids:
            bus["master"] = _renamed(master)
        if "masters" in bus:
            bus["masters"] = [_renamed(item) if item in component_ids else item
                              for item in bus["masters"]]

    for connection in graph["connections"]:
        component = connection.get("component")
        bus = connection.get("bus")
        controller = connection.get("controlled_by")
        if component in component_ids:
            connection["component"] = _renamed(component)
        if bus in bus_ids:
            connection["bus"] = _renamed(bus)
        elif bus in bindings:
            connection["bus"] = bindings[bus]
        if controller in component_ids:
            connection["controlled_by"] = _renamed(controller)

    for interrupt in graph["interrupts"]:
        ident = interrupt.get("id")
        if isinstance(ident, str):
            interrupt["id"] = _renamed(ident)
        source = interrupt.get("source")
        if source in component_ids:
            interrupt["source"] = _renamed(source)
        for hop in interrupt.get("chain", []):
            at = hop.get("at")
            if at in component_ids:
                hop["at"] = _renamed(at)

    for question in graph["open_questions"]:
        ident = question.get("id")
        if isinstance(ident, str):
            question["id"] = _renamed(ident)
        question["about"] = [
            _renamed(ident) if ident in component_ids else ident
            for ident in question.get("about", [])
        ]

    return graph


def _expand_nested_module_instances(
        path: pathlib.Path, data: dict[str, Any]) -> dict[str, Any]:
    """Expand one module layer's own repeated physical unit.

    A module layer may author a unit that repeats verbatim -- an Origin 2000
    node board, say -- once, as its own small module layer, then list it
    here with only the two facts that differ per copy: `id`, which replaces
    the sub-layer's containment root everywhere (its interior still gets the
    ordinary instance prefix), and `physical_slot`, attached to that root.
    Runs at layer-load time so every later pass sees an ordinary flat graph,
    the same way the product-level `module_instances` already does.
    """
    declarations = data["module_instances"]
    if not isinstance(declarations, list) or not declarations:
        raise ResolutionError(f"module '{path}' module_instances must be a non-empty list")
    base = path.parent.resolve()
    graph: dict[str, list[Any]] = {
        field: copy.deepcopy(data.get(field, [])) for field in MODULE_GRAPH_FIELDS
    }
    seen_ids: dict[str, set[str]] = {
        field: {item.get("id") for item in graph[field]
                if isinstance(item, dict) and isinstance(item.get("id"), str)}
        for field in MODULE_GRAPH_FIELDS
    }
    for index, declaration in enumerate(declarations):
        label = f"module '{path}' module_instances entry {index}"
        if not isinstance(declaration, dict):
            raise ResolutionError(f"{label} must be a mapping")
        unknown = sorted(set(declaration) - NESTED_MODULE_INSTANCE_FIELDS)
        if unknown:
            raise ResolutionError(f"{label} has unsupported field '{unknown[0]}'")
        reference = declaration.get("model")
        instance_id = declaration.get("id")
        parent = declaration.get("parent")
        physical_slot = declaration.get("physical_slot")
        if not all(isinstance(value, str) and value
                   for value in (reference, instance_id, parent)):
            raise ResolutionError(f"{label} requires non-empty model, id and parent")
        if not isinstance(physical_slot, dict) or not physical_slot:
            raise ResolutionError(f"{label} requires a physical_slot mapping")
        if parent not in seen_ids["components"]:
            raise ResolutionError(f"{label} names unknown enclosing parent '{parent}'")
        if instance_id in seen_ids["components"]:
            raise ResolutionError(f"{label} reuses component id '{instance_id}'")

        reference_path = pathlib.Path(reference)
        if reference_path.is_absolute():
            raise ResolutionError(f"{label} has absolute model reference '{reference}'")
        model_path = (path.parent / reference_path).resolve()
        if not model_path.is_relative_to(base):
            raise ResolutionError(
                f"{label} model '{reference}' is outside its composition directory"
            )
        if model_path == path.resolve():
            raise ResolutionError(f"recursive module-instance composition is unsupported: '{reference}'")
        if not model_path.is_file():
            raise ResolutionError(f"missing module-instance model '{model_path}'")

        sub_layer = _layer(model_path)
        if sub_layer["kind"] != "module":
            raise ResolutionError(f"module instance source '{model_path}' is not a module layer")
        contract = copy.deepcopy(sub_layer["data"])
        instance = _instantiate_module_graph(
            contract, prefix=instance_id + "-", parent=parent, module_index=index,
            omit_buses=[], observed_hardware=None, root_id=instance_id,
        )
        root = next(
            component for component in instance["components"]
            if component["id"] == instance_id
        )
        if "physical_slot" in root:
            raise ResolutionError(
                f"{label} supplies physical_slot but sub-layer '{model_path}' "
                "already authors one on its root component"
            )
        root["physical_slot"] = copy.deepcopy(physical_slot)

        for field in MODULE_GRAPH_FIELDS:
            for item in instance[field]:
                ident = item.get("id") if isinstance(item, dict) else None
                if isinstance(ident, str):
                    if ident in seen_ids[field]:
                        raise ResolutionError(
                            f"{label} collides with {field[:-1]} id '{ident}'"
                        )
                    seen_ids[field].add(ident)
            graph[field].extend(instance[field])

    return graph


def _merge_module_instances(
        path: pathlib.Path, composition: dict[str, Any], resolved: dict[str, Any],
        selected: dict[str, dict[str, Any]],
        population: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    declarations = composition.get("module_instances")
    if declarations is None:
        return []
    if not isinstance(declarations, list) or not declarations:
        raise ResolutionError("module_instances must be a non-empty list")
    base = path.parent.resolve()
    prefixes: set[str] = set()
    metadata: list[dict[str, Any]] = []
    instance_nodes = sum(
        component.get("kind") == "node-asic"
        for component in resolved.get("components", [])
        if isinstance(component, dict)
    )
    seen_ids: dict[str, set[str]] = {}
    for field in ("components", "buses", "interrupts", "open_questions"):
        seen_ids[field] = {
            item.get("id") for item in resolved.get(field, [])
            if isinstance(item, dict) and isinstance(item.get("id"), str)
        }

    for index, declaration in enumerate(declarations):
        label = f"module_instances entry {index}"
        if not isinstance(declaration, dict):
            raise ResolutionError(f"{label} must be a mapping")
        unknown = sorted(set(declaration) - MODULE_INSTANCE_FIELDS)
        if unknown:
            raise ResolutionError(f"{label} has unsupported field '{unknown[0]}'")
        reference = declaration.get("model")
        prefix = declaration.get("prefix")
        parent = declaration.get("parent")
        if not all(isinstance(value, str) and value
                   for value in (reference, prefix, parent)):
            raise ResolutionError(
                f"{label} requires non-empty model, prefix and parent"
            )
        if (not prefix[0].islower() or not prefix.endswith("-") or
                any(not (character.islower() or character.isdigit() or
                         character == "-") for character in prefix)):
            raise ResolutionError(
                f"{label} prefix must contain lowercase letters, digits and hyphens, "
                "and end in a hyphen"
            )
        if prefix in prefixes:
            raise ResolutionError(f"{label} reuses prefix '{prefix}'")
        prefixes.add(prefix)
        if parent not in seen_ids["components"]:
            raise ResolutionError(
                f"{label} names unknown enclosing parent '{parent}'"
            )
        reference_path = pathlib.Path(reference)
        if reference_path.is_absolute():
            raise ResolutionError(f"{label} has absolute model reference '{reference}'")
        model_path = (path.parent / reference_path).resolve()
        if not model_path.is_relative_to(base):
            raise ResolutionError(
                f"{label} model '{reference}' is outside its composition directory"
            )
        if model_path == path.resolve():
            raise ResolutionError(
                f"recursive module-instance composition is unsupported: '{reference}'"
            )
        if not model_path.is_file():
            raise ResolutionError(f"missing module-instance model '{model_path}'")

        omit_buses = declaration.get("omit_buses", [])
        observed_hardware = declaration.get("observed_hardware")
        bus_bindings = declaration.get("bus_bindings")
        adopt_roots = declaration.get("adopt_roots", False)
        if not isinstance(adopt_roots, bool):
            raise ResolutionError(f"{label} adopt_roots must be true or false")
        if (not isinstance(omit_buses, list) or
                not all(isinstance(item, str) and item for item in omit_buses)):
            raise ResolutionError(f"{label} omit_buses must be a list of bus ids")
        document = _read_yaml(model_path, "module")
        layer = _layer(model_path, document)
        if layer["kind"] != "module":
            raise ResolutionError(f"module instance source '{model_path}' is not a module layer")
        _require_layers({**selected, "module": layer})
        contract = copy.deepcopy(layer["data"])
        _attach_cpu_selection(
            contract.get("components", []), resolved.get("processors", []),
            selected["product"].get("cpu_selection"),
        )
        model_composition.validate_direct_model_references(contract)
        graph = _instantiate_module_graph(
            contract,
            prefix=prefix,
            parent=parent,
            module_index=index,
            omit_buses=omit_buses,
            observed_hardware=observed_hardware,
            bus_bindings=bus_bindings,
            adopt_roots=adopt_roots,
        )
        node_count = sum(component.get("kind") == "node-asic"
                         for component in graph["components"])
        instance_nodes += node_count
        if (population is not None and node_count and
                declaration.get("module_role") not in {"MASTER", "SLAVE"}):
            raise ResolutionError(
                f"module instance {index} requires module_role MASTER or SLAVE"
            )
        for placeholder, target in sorted((bus_bindings or {}).items()):
            if target not in {bus.get("id") for bus in resolved.get("buses", [])}:
                raise ResolutionError(
                    f"{label} binds '{placeholder}' to unknown bus '{target}'")
        if "physical_slot" in declaration:
            # A board fitted into a connector: the composition places the
            # instance root in its slot, as a nested instance does.
            physical_slot = declaration["physical_slot"]
            if not isinstance(physical_slot, dict) or not physical_slot:
                raise ResolutionError(f"{label} requires a physical_slot mapping")
            root = next(component for component in graph["components"]
                        if component.get("parent") == parent)
            if "physical_slot" in root:
                raise ResolutionError(
                    f"{label} supplies physical_slot but '{model_path}' "
                    "already authors one on its root component"
                )
            root["physical_slot"] = copy.deepcopy(physical_slot)
        for field in MODULE_GRAPH_FIELDS:
            values = graph[field]
            current = resolved.setdefault(field, [])
            if not isinstance(current, list):
                raise ResolutionError(f"resolved field '{field}' must be a list")
            if field in seen_ids:
                for item in values:
                    ident = item.get("id")
                    if ident in seen_ids[field]:
                        raise ResolutionError(
                            f"{label} collides with {field[:-1]} id '{ident}'"
                        )
                    seen_ids[field].add(ident)
            current.extend(values)
        metadata.append(copy.deepcopy(declaration))
    if population is not None and (
            population["nodes"] != instance_nodes or any(
                not isinstance(population.get(field), list) or
                len(population[field]) != instance_nodes
                for field in ("cpus_per_node", "memory_bank_mib_per_node"))):
        raise ResolutionError(
            "module instances and product population must describe the same nodes"
        )
    return metadata


def _num(value: Any) -> int:
    if isinstance(value, int):
        return value
    return int(str(value).replace("_", ""), 0)


POPULATION_MEMORY_FIELDS = {
    "dimms_per_node", "memory_mib_per_node", "memory_bank_mib_per_node",
}


def _derive_memory_kits(product: dict[str, Any], assembly: dict[str, Any]) -> None:
    """Derive a product's per-node DIMM, memory, and bank population from kits.

    A memory kit ({id, dimms, dimm_mib, banks}) on the assembly names one
    fitted DIMM population once; memory_kits_per_node names one kit per node.
    Mirrors cpu_selection: once a product uses it, the fields it derives may
    not also be hand-authored, so the two population styles never drift
    apart from each other.

    A kit's banks fill the node's locational banks starting at position 0
    unless the kit also names bank_positions (one locational bank index per
    entry in banks): a board whose fitted DIMMs land away from bank zero
    (Origin 300's 512 MiB at locational banks two and three) names them
    explicitly instead of the compiler guessing a placement no source
    states.
    """
    kit_ids = product.get("memory_kits_per_node")
    if kit_ids is None:
        return
    product_id = product["id"]
    assembly_id = assembly["id"]
    population = product.setdefault("population", {})
    if not isinstance(population, dict):
        raise ResolutionError(f"product '{product_id}' population must be a mapping")
    authored = POPULATION_MEMORY_FIELDS & set(population)
    if authored:
        raise ResolutionError(
            f"product '{product_id}' authors {sorted(authored)[0]} directly; "
            "memory_kits_per_node must supply it"
        )
    nodes = population.get("nodes")
    if (not isinstance(kit_ids, list) or not kit_ids or
            not all(isinstance(item, str) and item for item in kit_ids)):
        raise ResolutionError(
            f"product '{product_id}' memory_kits_per_node must be a list of kit ids"
        )
    if not isinstance(nodes, int) or len(kit_ids) != nodes:
        raise ResolutionError(
            f"product '{product_id}' memory_kits_per_node has {len(kit_ids)} "
            f"entries for {nodes} nodes"
        )
    capacity = assembly.get("capacity") or {}
    bank_limit = capacity.get("memory_banks_per_node")
    if not isinstance(bank_limit, int) or bank_limit < 1:
        raise ResolutionError(f"assembly '{assembly_id}' has no memory-bank capacity")
    kits = assembly.get("memory_kits")
    if not isinstance(kits, list) or not kits:
        raise ResolutionError(f"assembly '{assembly_id}' has no memory_kits")
    by_id = {
        kit["id"]: kit for kit in kits
        if isinstance(kit, dict) and isinstance(kit.get("id"), str)
    }
    dimms_per_node, memory_mib_per_node, banks_per_node = [], [], []
    for node, kit_id in enumerate(kit_ids):
        kit = by_id.get(kit_id)
        if kit is None:
            raise ResolutionError(
                f"product '{product_id}' node {node} names unknown memory kit '{kit_id}'"
            )
        dimms = kit.get("dimms")
        dimm_mib = kit.get("dimm_mib")
        banks = kit.get("banks")
        if (not isinstance(dimms, int) or not isinstance(dimm_mib, int) or
                not isinstance(banks, list) or not banks or
                not all(isinstance(size, int) and size > 0 for size in banks)):
            raise ResolutionError(f"memory kit '{kit_id}' is incomplete")
        if len(banks) > bank_limit:
            raise ResolutionError(
                f"memory kit '{kit_id}' describes more banks than "
                f"assembly '{assembly_id}' has slots for"
            )
        positions = kit.get("bank_positions")
        if positions is None:
            row = banks + [0] * (bank_limit - len(banks))
        else:
            valid = (isinstance(positions, list) and len(positions) == len(banks) and
                     all(isinstance(p, int) and 0 <= p < bank_limit for p in positions) and
                     len(set(positions)) == len(positions))
            if not valid:
                raise ResolutionError(
                    f"memory kit '{kit_id}' bank_positions must name {len(banks)} "
                    f"distinct locational banks below {bank_limit}"
                )
            row = [0] * bank_limit
            for position, size in zip(positions, banks):
                row[position] = size
        dimms_per_node.append(dimms)
        memory_mib_per_node.append(sum(banks))
        banks_per_node.append(row)
    population["dimms_per_node"] = dimms_per_node
    population["memory_mib_per_node"] = memory_mib_per_node
    population["memory_bank_mib_per_node"] = banks_per_node


def resolve_system_model(
        path: pathlib.Path, composition: dict[str, Any]) -> dict[str, Any]:
    if composition.get("configuration_version") != 1:
        raise ResolutionError(f"composition '{path}' has no supported configuration_version")
    unknown = sorted(set(composition) - {
        "configuration_version", "layers", "module_instances", "connector_population",
    })
    if unknown:
        raise ResolutionError(f"composition '{path}' has unsupported field '{unknown[0]}'")
    references = composition.get("layers")
    if not isinstance(references, list) or not references:
        raise ResolutionError(f"composition '{path}' has no layers")

    selected: dict[str, dict[str, Any]] = {}
    for reference in references:
        if not isinstance(reference, str) or not reference:
            raise ResolutionError(f"composition '{path}' has an invalid layer reference")
        reference_path = pathlib.Path(reference)
        if reference_path.is_absolute():
            raise ResolutionError(
                f"composition '{path}' has absolute layer reference '{reference}'"
            )
        layer_path = path.parent / reference_path
        if not layer_path.is_file():
            raise ResolutionError(f"missing layer '{layer_path}'")
        layer = _layer(layer_path)
        kind = layer["kind"]
        if kind in selected:
            raise ResolutionError(
                f"composition selects both '{selected[kind]['id']}' and "
                f"'{layer['id']}' as its {kind}"
            )
        selected[kind] = layer

    missing = sorted(REQUIRED_KINDS - selected.keys())
    if missing:
        raise ResolutionError(f"composition '{path}' is missing {', '.join(missing)} layers")
    _require_layers(selected)

    _derive_memory_kits(selected["product"], selected["assembly"])
    limits = model_composition.validate_population(selected["product"], selected["assembly"])
    resolved = _merge_data(selected)
    _attach_cpu_selection(
        resolved.get("components", []), resolved.get("processors", []),
        selected["product"].get("cpu_selection"),
    )
    _attach_processor_reset_mode(
        resolved, resolved.get("processors", []),
        selected["product"].get("cpu_selection"),
    )
    machine = resolved.get("machine", {})
    routed = model_composition.reject_routed_firmware_graphs(resolved)
    population = selected["product"]["population"]
    check_population = machine.get("model_role") == "emulation-target" and not routed
    module_instances = _merge_module_instances(
        path, composition, resolved, selected,
        population if check_population else None)
    if routed and any("module_role" in item for item in module_instances):
        raise ResolutionError("routed target must not assign firmware graph module roles")
    exclusions = model_composition.component_exclusions([
        (f"{layer['kind']} '{layer['id']}'", layer.get("excluded_components"))
        for layer in selected.values()
    ], resolved.get("components"))
    if "excluded_components" in resolved:
        raise ResolutionError(
            "excluded_components must be declared on a layer, not inside layer data"
        )
    if exclusions:
        resolved["excluded_components"] = exclusions
    if resolved.get("model_version") != 1:
        raise ResolutionError(f"composition '{path}' does not resolve a model_version")

    product = selected["product"]
    resolved["composition"] = {
        "architecture": selected["architecture"]["id"],
        "platform": selected["assembly"]["platform"],
        "assembly": selected["assembly"]["id"],
        "product": product["id"],
        "layers": [{"kind": kind, "id": layer["id"]} for kind, layer in selected.items()],
        "topology": copy.deepcopy(product["topology"]),
        "population": copy.deepcopy(product["population"]),
        "limits": limits,
    }
    if "connector_population" in composition:
        resolved["composition"]["connector_population"] = copy.deepcopy(
            composition["connector_population"]
        )
    if module_instances:
        resolved["composition"]["module_instances"] = module_instances
    return resolved


def _validate_resolved_system_model(model: dict[str, Any]) -> dict[str, Any]:
    """Validate an already-resolved model dictionary."""
    return model_composition.check_composition(model)


def _load_system_model(
        model_path: pathlib.Path) -> dict[str, Any]:
    document = _read_yaml(model_path, "model")
    if "configuration_version" not in document:
        raise ResolutionError("system model input must be a layered composition")
    model = resolve_system_model(model_path, document)
    return _validate_resolved_system_model(model)


def load_system_model(path: str | pathlib.Path) -> dict[str, Any]:
    """Resolve and validate a layered system-model composition."""
    model_path = pathlib.Path(path).resolve()
    return _load_system_model(model_path)
