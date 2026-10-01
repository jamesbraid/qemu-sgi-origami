# SPDX-License-Identifier: BSD-3-Clause

"""Composition invariants: can this system model be built.

Population against capacity, slots against an ASIC's shape, cables between
real ports. `check_composition` is the one entry the resolver calls after a
model is fully resolved; the resolver also calls a few of these directly
mid-resolution, where a composition fact (the population's memory-bank
limits, a layer's component ids) is needed before the model is complete.
Each invariant is its own named function so either caller can reach the one
it needs. Model provenance is carried separately from these structural checks.
"""

from __future__ import annotations

import copy
import re
from typing import Any

from .exceptions import ResolutionError


EXCLUSION_FIELDS = {"match", "reason", "provenance", "sources", "evidence_gap"}
EXCLUSION_MATCH_FIELDS = {"id", "kind"}
IMPLEMENTATION_FIELDS = {
    "emulation", "modelled", "implementation_status", "implementation_sources",
}


def _fabric_topology(model: dict[str, Any]) -> str:
    """Classify the installed fabric from ASICs and attached endpoints."""
    components = model.get("components", [])
    if any(item.get("kind") == "router-asic" for item in components):
        return "router"
    by_id = {item.get("id"): item for item in components}
    buses = {item.get("id"): item for item in model.get("buses", [])}
    for connection_bus in {
        item.get("bus") for item in model.get("connections", [])
    }:
        if buses.get(connection_bus, {}).get("kind") != "fabric":
            continue
        endpoints = {
            item.get("component") for item in model.get("connections", [])
            if item.get("bus") == connection_bus
            and item.get("component") in by_id
        }
        if len(endpoints) > 1:
            return "direct-craylink"
    return "none"


def _node_counts(population: dict[str, Any], field: str, nodes: int) -> list[int] | None:
    values = population.get(field)
    if values is None:
        return None
    if not isinstance(values, list) or not all(isinstance(v, int) for v in values):
        raise ResolutionError(f"population {field} must be a list of integers")
    if len(values) != nodes:
        raise ResolutionError(
            f"population {field} has {len(values)} entries for {nodes} nodes"
        )
    return values


def _cpu_population_counts(entry: Any, label: str) -> tuple[list[int], bool]:
    if isinstance(entry, list) and all(isinstance(value, int) for value in entry):
        return entry, False
    if not isinstance(entry, dict):
        raise ResolutionError(f"{label} must be node counts or an unspecified orientation")

    counts = entry.get("node_counts")
    nodes = entry.get("nodes")
    total = entry.get("total_cpus")
    if not isinstance(counts, list) or not all(isinstance(value, int) for value in counts):
        raise ResolutionError(f"{label} node_counts must be integers")
    if nodes != len(counts):
        raise ResolutionError(
            f"{label} nodes {nodes} does not match {len(counts)} node_counts entries"
        )
    if total != sum(counts):
        raise ResolutionError(
            f"{label} total_cpus {total} does not match node_counts total {sum(counts)}"
        )
    return counts, True


def _selector_label(match: dict[str, str]) -> str:
    return ", ".join(
        f"{field}='{match[field]}'" for field in ("id", "kind") if field in match
    )


def validate_component_ids(model: dict[str, Any]) -> None:
    seen: set[str] = set()
    for component in model.get("components", []):
        cid = component.get("id")
        if not isinstance(cid, str):
            raise ResolutionError("installed component has no stable identifier")
        if cid in seen:
            raise ResolutionError(f"duplicate component id '{cid}'")
        seen.add(cid)


def validate_population(product: dict[str, Any], assembly: dict[str, Any]) -> dict[str, Any]:
    product_id = product["id"]
    assembly_id = assembly["id"]
    topology = product.get("topology", {})
    population = product.get("population", {})
    capacity = assembly.get("capacity", {})
    if not all(isinstance(value, dict) for value in (topology, population, capacity)):
        raise ResolutionError("topology, population, and capacity must be mappings")

    nodes = population.get("nodes")
    minimum = topology.get("min_nodes")
    maximum = topology.get("max_nodes")
    if not isinstance(nodes, int) or nodes < 1:
        raise ResolutionError(f"product '{product_id}' population has invalid node count")
    if not isinstance(minimum, int) or not isinstance(maximum, int):
        raise ResolutionError(f"product '{product_id}' topology has no node limits")
    if nodes < minimum:
        raise ResolutionError(
            f"product '{product_id}' requires at least {minimum} nodes, got {nodes}"
        )
    if nodes > maximum:
        raise ResolutionError(
            f"product '{product_id}' permits at most {maximum} nodes, got {nodes}"
        )

    cpu_limit = capacity.get("cpu_slots_per_node")
    dimm_limit = capacity.get("dimm_slots_per_node")
    if not isinstance(cpu_limit, int) or not isinstance(dimm_limit, int):
        raise ResolutionError(f"assembly '{assembly_id}' has incomplete slot capacity")

    cpus = _node_counts(population, "cpus_per_node", nodes)
    dimms = _node_counts(population, "dimms_per_node", nodes)
    memory = _node_counts(population, "memory_mib_per_node", nodes)
    if cpus is None:
        raise ResolutionError(f"product '{product_id}' has no CPU population")
    for node, count in enumerate(cpus):
        if count < 0 or count > cpu_limit:
            raise ResolutionError(
                f"node {node} populates {count} CPUs, but assembly "
                f"'{assembly_id}' has {cpu_limit} slots"
            )
    if dimms is not None:
        for node, count in enumerate(dimms):
            if count < 0 or count > dimm_limit:
                raise ResolutionError(
                    f"node {node} populates {count} DIMMs, but assembly "
                    f"'{assembly_id}' has {dimm_limit} slots"
                )

    limits = {
        "nodes": maximum,
        "cpus_per_node": cpu_limit,
        "dimms_per_node": dimm_limit,
    }
    for field in (
            "main_memory_slots_per_node",
            "main_memory_dimm_positions_per_node",
            "directory_memory_slots_per_node"):
        if field in capacity:
            limits[field] = copy.deepcopy(capacity[field])
    for field in ("provenance", "sources", "evidence_gap"):
        if field in capacity:
            limits[field] = copy.deepcopy(capacity[field])

    bank_limit = capacity.get("memory_banks_per_node")
    bank_layouts = population.get("memory_bank_mib_per_node")
    if bank_layouts is not None:
        dimms_per_bank = capacity.get("dimms_per_bank")
        supported_dimms = capacity.get("supported_dimm_sizes_mib")
        supported = capacity.get("supported_bank_sizes_mib")
        if memory is None:
            raise ResolutionError(
                f"product '{product_id}' has no memory population"
            )
        if not isinstance(bank_limit, int) or bank_limit < 1:
            raise ResolutionError(
                f"assembly '{assembly_id}' has no memory-bank capacity"
            )
        if not isinstance(dimms_per_bank, int) or dimms_per_bank < 1:
            raise ResolutionError(
                f"assembly '{assembly_id}' has no DIMM-to-bank rule"
            )
        if (not isinstance(supported, list) or not supported or
                not all(isinstance(size, int) and size > 0
                        for size in supported)):
            raise ResolutionError(
                f"assembly '{assembly_id}' has no supported bank sizes"
            )
        if supported_dimms is not None:
            expected_banks = [size * dimms_per_bank for size in supported_dimms]
            if expected_banks != supported:
                raise ResolutionError(
                    f"assembly '{assembly_id}' DIMM and bank sizes disagree"
                )
        if (not isinstance(bank_layouts, list) or
                len(bank_layouts) != nodes):
            raise ResolutionError(
                f"population memory_bank_mib_per_node has "
                f"{len(bank_layouts) if isinstance(bank_layouts, list) else 0} "
                f"entries for {nodes} nodes"
            )
        for node, banks in enumerate(bank_layouts):
            if (not isinstance(banks, list) or len(banks) != bank_limit or
                    not all(isinstance(size, int) and size >= 0
                            for size in banks)):
                raise ResolutionError(
                    f"node {node} must describe exactly {bank_limit} memory banks"
                )
            # Populated positions form one contiguous run, which need not
            # start at locational bank zero.
            positions = [index for index, size in enumerate(banks) if size != 0]
            if not positions or positions != list(
                    range(positions[0], positions[0] + len(positions))):
                raise ResolutionError(
                    f"node {node} memory banks must populate one contiguous run"
                )
            populated = [banks[index] for index in positions]
            invalid = [size for size in populated if size not in supported]
            if invalid:
                raise ResolutionError(
                    f"node {node} uses unsupported {invalid[0]} MiB memory bank"
                )
            if sum(populated) != memory[node]:
                raise ResolutionError(
                    f"node {node} bank sizes total {sum(populated)} MiB, "
                    f"but population declares {memory[node]} MiB"
                )
            if dimms is not None and len(populated) * dimms_per_bank != dimms[node]:
                raise ResolutionError(
                    f"node {node} bank population needs "
                    f"{len(populated) * dimms_per_bank} DIMMs, got {dimms[node]}"
                )
        limits["memory_banks_per_node"] = bank_limit
        limits["supported_bank_sizes_mib"] = copy.deepcopy(supported)

    return limits


def validate_model_cpu_populations(model: dict[str, Any]) -> None:
    composition = model.get("composition")
    if not isinstance(composition, dict):
        return
    topology = composition.get("topology", {})
    supported = topology.get("supported_cpu_populations")
    selected = composition.get("population", {}).get("cpus_per_node")
    if supported is None:
        role = model.get("machine", {}).get("model_role")
        if role != "reference" and isinstance(selected, list):
            raise ResolutionError(
                "supported_cpu_populations is required when a CPU population is selected"
            )
        return
    if not isinstance(supported, list) or not supported:
        raise ResolutionError("supported_cpu_populations must be a non-empty list")

    limits = composition.get("limits", {})
    node_limit = limits.get("nodes")
    cpu_limit = limits.get("cpus_per_node")
    min_nodes = topology.get("min_nodes")
    max_nodes = topology.get("max_nodes")
    if (isinstance(min_nodes, int) and isinstance(max_nodes, int) and
            min_nodes > max_nodes):
        raise ResolutionError(
            f"topology min_nodes {min_nodes} exceeds max_nodes {max_nodes}"
        )
    exact: set[tuple[int, ...]] = set()
    unoriented: set[tuple[int, ...]] = set()

    def validate_counts(counts: list[int], label: str) -> None:
        if not counts or any(value < 1 for value in counts):
            raise ResolutionError(f"{label} must populate at least one CPU per node")
        if isinstance(min_nodes, int) and len(counts) < min_nodes:
            raise ResolutionError(
                f"{label} uses {len(counts)} nodes, but the topology requires "
                f"at least {min_nodes}"
            )
        if isinstance(max_nodes, int) and len(counts) > max_nodes:
            raise ResolutionError(
                f"{label} uses {len(counts)} nodes, but the topology permits "
                f"at most {max_nodes}"
            )
        if isinstance(node_limit, int) and len(counts) > node_limit:
            raise ResolutionError(
                f"{label} uses {len(counts)} nodes, but the product permits {node_limit}"
            )
        for value in counts:
            if isinstance(cpu_limit, int) and value > cpu_limit:
                raise ResolutionError(
                    f"{label} node count {value} exceeds the {cpu_limit}-CPU assembly capacity"
                )

    for index, entry in enumerate(supported):
        label = f"supported CPU population {index}"
        counts, unspecified = _cpu_population_counts(entry, label)
        validate_counts(counts, label)
        key = tuple(sorted(counts)) if unspecified else tuple(counts)
        (unoriented if unspecified else exact).add(key)

    def is_supported(counts: list[int]) -> bool:
        return tuple(counts) in exact or tuple(sorted(counts)) in unoriented

    if isinstance(selected, list):
        validate_counts(selected, "selected CPU population")
        if not is_supported(selected):
            raise ResolutionError(f"selected CPU population {selected} is not supported")


def validate_model_memory_population(model: dict[str, Any]) -> None:
    composition = model.get("composition")
    if isinstance(composition, dict) and "population" in model:
        raise ResolutionError(
            "model cannot contain both root population and composition"
        )
    population = (composition or {}).get("population")
    role = model.get("machine", {}).get("model_role")
    if not isinstance(population, dict):
        if role == "emulation-target":
            raise ResolutionError(
                "emulation target requires a concrete fitted memory population"
            )
        raise ResolutionError(
            "strict model requires concrete or unknown fitted memory"
        )

    memory = population.get("memory_mib_per_node")
    fitted = population.get("fitted_memory")
    if memory is not None and fitted is not None:
        raise ResolutionError(
            "population cannot contain both concrete and unknown fitted memory"
        )
    if fitted is not None:
        for field in ("memory_bank_mib_per_node", "dimms_per_node"):
            if field in population:
                raise ResolutionError(
                    f"unknown fitted memory cannot include concrete {field}"
                )
    if memory is not None:
        if (not isinstance(memory, list) or
                not all(isinstance(value, int) for value in memory)):
            raise ResolutionError(
                "memory_mib_per_node must be a list of integer MiB values"
            )
        nodes = population.get("nodes")
        if isinstance(nodes, int) and len(memory) != nodes:
            raise ResolutionError(
                f"memory_mib_per_node has {len(memory)} entries for {nodes} nodes"
            )
        if any(value <= 0 for value in memory):
            raise ResolutionError(
                "memory_mib_per_node must contain positive MiB values"
            )
        return
    if fitted is not None:
        if role != "reference":
            raise ResolutionError(
                "emulation target requires a concrete fitted memory population"
            )
        return
    if role == "emulation-target":
        raise ResolutionError(
            "emulation target requires a concrete fitted memory population"
        )
    raise ResolutionError(
        "strict model requires concrete or unknown fitted memory"
    )


def validate_emulation_profile(model: dict[str, Any]) -> None:
    machine = model.get("machine")
    if not isinstance(machine, dict):
        return
    emulation = machine.get("emulation")
    if (machine.get("model_role") != "emulation-target" or
            not isinstance(emulation, dict)):
        return
    profile = emulation.get("profile")
    if not isinstance(profile, str) or not re.fullmatch(r"[a-z][a-z0-9-]*", profile):
        raise ResolutionError(
            "emulation target requires a profile identifier containing "
            "lowercase letters, digits and hyphens"
        )


def validate_direct_model_references(model: dict[str, Any]) -> None:
    components = {
        component["id"]: component
        for component in model.get("components", [])
        if isinstance(component, dict) and isinstance(component.get("id"), str)
    }
    buses = {
        bus["id"]: bus
        for bus in model.get("buses", [])
        if isinstance(bus, dict) and isinstance(bus.get("id"), str)
    }
    supported_populations = (
        model.get("composition", {}).get("topology", {}).get(
            "supported_cpu_populations", []
        )
    )

    for component_id, component in components.items():
        optional = component.get("optional")
        if optional is None:
            continue
        if not supported_populations:
            continue
        node = optional.get("node")
        if not any(
                isinstance(population, list) and node < len(population)
                for population in supported_populations):
            raise ResolutionError(
                f"optional component '{component_id}' names node {node} outside "
                "every supported profile"
            )

    for bus_id, bus in buses.items():
        if "masters" in bus and bus.get("kind") != "i2c":
            raise ResolutionError("masters is supported only on I2C buses")
        if bus.get("kind") != "i2c":
            continue
        if "masters" in bus:
            if "master" in bus:
                raise ResolutionError(f"I2C bus '{bus_id}' declares both master and masters")
            declared = bus["masters"]
            if (not isinstance(declared, list) or not declared or
                    any(not isinstance(item, str) or not item for item in declared)):
                raise ResolutionError(f"I2C bus '{bus_id}' requires a nonempty master list")
            if len(set(declared)) != len(declared):
                raise ResolutionError(f"I2C bus '{bus_id}' masters must be unique")
        else:
            declared = [bus.get("master")]
        for master in declared:
            if master not in components:
                raise ResolutionError(f"I2C bus '{bus_id}' has unknown master '{master}'")
        attachments = [
            connection for connection in model.get("connections", [])
            if connection.get("bus") == bus_id
        ]
        seen = set()
        for connection in attachments:
            component = connection.get("component")
            if component in seen:
                raise ResolutionError(f"I2C bus '{bus_id}' has duplicate attachment '{component}'")
            seen.add(component)
            role = connection.get("role")
            if role not in {"master", "endpoint"}:
                raise ResolutionError(f"I2C bus '{bus_id}' has invalid attachment role '{role}'")
            if role == "master" and component not in declared:
                raise ResolutionError(f"I2C bus '{bus_id}' has undeclared master '{component}'")
            if role == "endpoint" and "i2c_address" not in connection:
                raise ResolutionError(f"I2C endpoint '{component}' has no address")
            if "i2c_address_count" in connection and "i2c_address" not in connection:
                raise ResolutionError(f"I2C attachment '{component}' has address count without address")
        for master in declared:
            if sum(connection.get("role") == "master" and
                   connection.get("component") == master for connection in attachments) != 1:
                raise ResolutionError(
                    f"I2C bus '{bus_id}' requires one connection for master '{master}'")
        endpoints = [connection for connection in attachments
                     if connection.get("role") == "endpoint" or "i2c_address" in connection]
        if not endpoints:
            raise ResolutionError(f"I2C bus '{bus_id}' has no endpoints")
        claimed: dict[int, str] = {}
        for connection in endpoints:
            component_id = connection.get("component")
            address = connection.get("i2c_address")
            if not isinstance(address, int) or isinstance(address, bool):
                raise ResolutionError(
                    f"I2C endpoint '{component_id}' has no address"
                )
            count = connection.get("i2c_address_count", 1)
            if (not isinstance(count, int) or isinstance(count, bool)
                    or count < 1 or address < 0 or address + count > 0x80):
                raise ResolutionError(
                    f"I2C endpoint '{component_id}' span exceeds 7-bit address "
                    "space"
                )
            for claimed_address in range(address, address + count):
                previous = claimed.get(claimed_address)
                if previous is not None:
                    raise ResolutionError(
                        f"I2C address {claimed_address:#x} on bus '{bus_id}' "
                        f"for '{component_id}' overlaps '{previous}'"
                    )
                claimed[claimed_address] = component_id


def validate_physical_slots(model: dict[str, Any]) -> None:
    validate_component_ids(model)
    components = {item["id"]: item for item in model.get("components", [])}
    slots: dict[tuple[str, str, int], str] = {}
    for cid, component in components.items():
        slot = component.get("physical_slot")
        if slot is None:
            continue
        if (component.get("kind") != "board" or not isinstance(slot, dict) or
                not {"kind", "number"} <= set(slot) <= {"kind", "number", "hub_slot_bits"} or
                slot.get("kind") not in ("node", "router", "io") or
                type(slot.get("number")) is not int or slot["number"] < 1):
            raise ResolutionError(f"component '{cid}' has an invalid physical slot")
        if "hub_slot_bits" in slot and (
                slot["kind"] != "node" or type(slot["hub_slot_bits"]) is not int or
                not 0 <= slot["hub_slot_bits"] <= 7):
            raise ResolutionError(
                f"component '{cid}' names Hub slot bits outside a node slot's 0 to 7")
        parent = components.get(component.get("parent"))
        seen = {cid}
        while parent is not None and parent.get("kind") != "module":
            if parent["id"] in seen:
                raise ResolutionError(f"component '{cid}' has cyclic containment")
            seen.add(parent["id"])
            parent = components.get(parent.get("parent"))
        if parent is None:
            raise ResolutionError(f"component '{cid}' physical slot has no module")
        key = (parent["id"], slot["kind"], slot["number"])
        if key in slots:
            raise ResolutionError(
                f"physical slot {key!r} is repeated by '{slots[key]}' and '{cid}'"
            )
        slots[key] = cid


def validate_connector_population(model: dict[str, Any]) -> None:
    selections = model.get("composition", {}).get("connector_population", [])
    if not isinstance(selections, list):
        raise ResolutionError("connector_population must be a list")
    components = {item["id"]: item for item in model.get("components", [])}
    selected: set[str] = set()
    occupants: set[str] = set()
    buses: dict[str, set[str]] = {}
    for connection in model.get("connections", []):
        buses.setdefault(connection["component"], set()).add(connection["bus"])
    for selection in selections:
        required = {"component", "occupancy", "reason", "provenance", "sources"}
        if (not isinstance(selection, dict) or not required <= selection.keys() or
                selection.keys() - required - {"occupant"} or
                not isinstance(selection["component"], str) or
                selection["occupancy"] not in ("empty", "occupied") or
                not isinstance(selection["reason"], str) or not selection["reason"] or
                not isinstance(selection["provenance"], str) or not selection["provenance"] or
                not isinstance(selection["sources"], list) or not selection["sources"] or
                any(not isinstance(source, str) or not source
                    for source in selection["sources"])):
            raise ResolutionError("connector_population has an invalid selection")
        cid = selection["component"]
        if cid in selected:
            raise ResolutionError(f"connector population '{cid}' is repeated")
        selected.add(cid)
        connector = components.get(cid)
        if connector is None:
            raise ResolutionError(f"connector population names unknown connector '{cid}'")
        if connector.get("kind") != "connector":
            raise ResolutionError(f"connector population '{cid}' is not a connector")
        occupancy = selection["occupancy"]
        known = connector["connector"]["occupancy"]
        if known != "opaque" and known != occupancy:
            raise ResolutionError(f"connector population '{cid}' conflicts with inventory")
        occupant = selection.get("occupant")
        if occupancy == "empty":
            if "occupant" in selection:
                raise ResolutionError(f"empty connector '{cid}' cannot name an occupant")
            continue
        if not isinstance(occupant, str) or not occupant:
            raise ResolutionError(f"occupied connector '{cid}' requires an occupant")
        if occupant not in components:
            raise ResolutionError(f"connector '{cid}' names unknown occupant '{occupant}'")
        if components[occupant].get("kind") not in {"board", "carrier", "module"}:
            raise ResolutionError(
                f"connector '{cid}' occupant '{occupant}' is not a board or module")
        if occupant in occupants:
            raise ResolutionError(f"connector occupant '{occupant}' is repeated")
        occupants.add(occupant)
        descendants = {occupant}
        while True:
            children = {item["id"] for item in components.values()
                        if item.get("parent") in descendants}
            if children <= descendants:
                break
            descendants.update(children)
        occupant_buses = set().union(*(buses.get(item, set()) for item in descendants))
        if not buses.get(cid, set()) & occupant_buses:
            raise ResolutionError(f"occupant '{occupant}' is not connected to '{cid}'")
    machine = model.get("machine", {})
    if (machine.get("model_role") == "emulation-target" and
            machine.get("emulation", {}).get("lifecycle") == "implemented" and
            _fabric_topology(model) == "router"):
        for cid, component in components.items():
            if (component.get("kind") == "connector" and
                    component["connector"]["occupancy"] == "opaque" and cid not in selected):
                raise ResolutionError(f"implemented target has unresolved connector '{cid}'")


def validate_synthetic_identities(model: dict[str, Any]) -> None:
    if _fabric_topology(model) != "router":
        return
    emulation = model.get("machine", {}).get("emulation", {})
    identities = emulation.get("synthetic_identities")
    if identities is None and emulation.get("lifecycle") != "implemented":
        return
    if not isinstance(identities, list):
        raise ResolutionError("routed target requires synthetic_identities")
    components = {item["id"]: item for item in model.get("components", [])}
    # A bridge-less XIO widget board that attaches to XIO directly
    # (XG, the InfiniteReality pipe's host interface, and HQ4, the SI
    # graphics board's; model_compile._XIO_WIDGET_KINDS draws the same line
    # for the same structural reason) is identity-bearing exactly as a
    # `pci-host-bridge` is: either is the one component an occupied
    # connector's XIO bus attaches to.
    required = {cid for cid, item in components.items()
                if item.get("kind") in {"node-asic", "router-asic", "pci-host-bridge"}
                or item.get("hardware_kind") in {"ioc3", "xg", "hq4"}}
    seen_components: set[str] = set()
    seen_serials: set[int] = set()
    for identity in identities:
        if (not isinstance(identity, dict) or
                set(identity) != {"component", "serial", "reason"} or
                not isinstance(identity["component"], str) or
                not isinstance(identity["reason"], str) or not identity["reason"].strip()):
            raise ResolutionError("invalid synthetic identity policy entry")
        cid = identity["component"]
        if cid not in components:
            raise ResolutionError(f"synthetic identity names unknown component '{cid}'")
        if cid not in required:
            raise ResolutionError(f"component '{cid}' is not identity-bearing")
        if cid in seen_components:
            raise ResolutionError(f"synthetic identity has repeated component '{cid}'")
        seen_components.add(cid)
        literal = identity["serial"]
        if (not isinstance(literal, str) or
                not re.fullmatch(r"0x[0-9a-f]+(?:_[0-9a-f]+)*", literal)):
            raise ResolutionError(f"component '{cid}' serial must be a hexadecimal literal")
        serial = int(literal.replace("_", ""), 16)
        if not 0 < serial < 1 << 48:
            raise ResolutionError(f"component '{cid}' serial is outside the 48-bit range")
        if serial in seen_serials:
            raise ResolutionError(f"synthetic identity has repeated serial {literal}")
        seen_serials.add(serial)
    missing = required - seen_components
    if missing:
        raise ResolutionError(f"missing synthetic identity for '{sorted(missing)[0]}'")


def validate_initial_module_numbers(model: dict[str, Any]) -> None:
    machine = model.get("machine", {})
    emulation = machine.get("emulation", {})
    if (machine.get("model_role") != "emulation-target" or
            emulation.get("lifecycle") != "implemented"):
        if "initial_module_numbers" in emulation:
            raise ResolutionError(
                "initial_module_numbers requires an implemented emulation target"
            )
        return
    policy = emulation.get("initial_module_numbers")
    # A routed target must select every module's number; a router-less one
    # (one Origin 2000 node board) still has a module and may select it.
    if policy is None and _fabric_topology(model) != "router":
        return
    if not isinstance(policy, list):
        raise ResolutionError("routed target requires initial_module_numbers")
    validate_component_ids(model)
    components = {item["id"]: item for item in model.get("components", [])}
    required = {cid for cid, item in components.items() if item.get("kind") == "module"}
    seen_components: set[str] = set()
    seen_numbers: set[int] = set()
    for entry in policy:
        if (not isinstance(entry, dict) or set(entry) != {"component", "number", "reason"}
                or not isinstance(entry["component"], str)
                or not isinstance(entry["reason"], str) or not entry["reason"].strip()):
            raise ResolutionError("invalid initial module number entry")
        cid = entry["component"]
        if cid not in components:
            raise ResolutionError(f"initial module number names unknown component '{cid}'")
        if cid not in required:
            raise ResolutionError(f"initial module number component '{cid}' is not a module")
        if cid in seen_components:
            raise ResolutionError(f"initial module number has repeated component '{cid}'")
        number = entry["number"]
        if type(number) is not int or not 1 <= number <= 255:
            raise ResolutionError(f"module '{cid}' number must be an integer from 1 through 255")
        if number in seen_numbers:
            raise ResolutionError(f"initial module identity has repeated number {number}")
        seen_components.add(cid)
        seen_numbers.add(number)
    missing = required - seen_components
    if missing:
        raise ResolutionError(f"initial_module_numbers missing module '{sorted(missing)[0]}'")


def validate_hub_ni_revision_overrides(model: dict[str, Any]) -> None:
    """`machine.emulation.hub_ni_revision_overrides`, if present, is sparse
    by design (unlike `synthetic_identities`'s exhaustive coverage): a
    product names only the Hubs whose `NI_STATUS_REV_ID` it needs to state
    explicitly, and every other Hub keeps the reusable node-board module's
    own authored value. See docs/datasheets/hub-network-interface.md.
    """
    emulation = model.get("machine", {}).get("emulation", {})
    overrides = emulation.get("hub_ni_revision_overrides")
    if overrides is None:
        return
    if not isinstance(overrides, list):
        raise ResolutionError("hub_ni_revision_overrides must be a list")
    components = {item["id"]: item for item in model.get("components", [])}
    seen: set[str] = set()
    for entry in overrides:
        if (not isinstance(entry, dict) or
                set(entry) != {"component", "ni_revision", "reason"} or
                not isinstance(entry["component"], str) or
                not isinstance(entry["reason"], str) or not entry["reason"].strip()):
            raise ResolutionError("invalid hub NI revision override entry")
        cid = entry["component"]
        if cid not in components:
            raise ResolutionError(f"hub NI revision override names unknown component '{cid}'")
        if components[cid].get("hardware_kind") != "hub":
            raise ResolutionError(f"component '{cid}' is not a Hub")
        if cid in seen:
            raise ResolutionError(f"hub NI revision override has repeated component '{cid}'")
        seen.add(cid)
        value = entry["ni_revision"]
        if type(value) is not int or not 0 <= value <= 15:
            raise ResolutionError(
                f"component '{cid}' ni_revision must be an integer from 0 through 15"
            )


def validate_system_timekeepers(model: dict[str, Any]) -> None:
    selected = model.get("machine", {}).get("emulation", {}).get(
        "system_timekeepers", {})
    components = {c["id"]: c for c in model.get("components", [])}
    for board, policy in selected.items():
        fitted = components.get(board, {})
        machine = model["machine"]
        is_fuel = (machine.get("generation") == "SN1"
                   and machine.get("board_ip") == "IP34"
                   and machine["emulation"].get("machine_name") == "fuel"
                   and fitted.get("kind") == "board"
                   and fitted.get("hardware_kind") == "ip34")
        parent = components.get(fitted.get("parent"), {})
        is_io8 = (machine.get("generation") == "SN1"
                  and machine.get("board_ip") == "IP45"
                  and machine["emulation"].get("machine_name") == "origin300"
                  and fitted.get("kind") == "subfunction"
                  and parent.get("hardware_kind") == "xbridge")
        if is_fuel:
            has_ioc3 = any(c.get("parent") == board
                           and c.get("hardware_kind") == "ioc3"
                           for c in components.values())
        elif is_io8:
            buses = {bus["id"] for bus in model.get("buses", [])
                     if bus.get("kind") == "pci" and bus.get("master") == board}
            has_ioc3 = any(
                connection.get("bus") in buses
                and components.get(connection.get("component"), {}).get(
                    "hardware_kind") == "ioc3"
                for connection in model.get("connections", []))
        else:
            raise ResolutionError(
                "system Timekeeper selection requires Fuel or Origin 300")
        if not has_ioc3:
            raise ResolutionError(
                f"system Timekeeper board '{board}' has no fitted IOC3")
        if (policy.get("kind") != "m48t35-compatible"
                or type(policy.get("year_base")) is not int
                or policy["year_base"] != 1968
                or policy.get("initial_ip27_fixture") is not False):
            raise ResolutionError("unsupported system Timekeeper policy")


def validate_board_identity_overrides(model: dict[str, Any]) -> None:
    """`machine.emulation.board_identity_overrides`, if present, corrects the
    compiled one-wire `identity.part` of a board component a product selects
    from a reusable module layer it cannot edit in place. Sparse by design,
    the same way `hub_ni_revision_overrides` is: a product names only the
    boards whose part it needs to state explicitly, and every other board
    keeps its own authored value.
    """
    emulation = model.get("machine", {}).get("emulation", {})
    overrides = emulation.get("board_identity_overrides")
    if overrides is None:
        return
    if not isinstance(overrides, list):
        raise ResolutionError("board_identity_overrides must be a list")
    components = {item["id"]: item for item in model.get("components", [])}
    seen: set[str] = set()
    for entry in overrides:
        if (not isinstance(entry, dict) or
                set(entry) != {"component", "part", "reason"} or
                not isinstance(entry["component"], str) or
                not isinstance(entry["part"], str) or not entry["part"].strip() or
                not isinstance(entry["reason"], str) or not entry["reason"].strip()):
            raise ResolutionError("invalid board identity override entry")
        cid = entry["component"]
        if cid not in components:
            raise ResolutionError(f"board identity override names unknown component '{cid}'")
        if components[cid].get("kind") != "board":
            raise ResolutionError(f"component '{cid}' is not a board")
        if cid in seen:
            raise ResolutionError(f"board identity override has repeated component '{cid}'")
        seen.add(cid)


def reject_reference_implementation(model: dict[str, Any]) -> None:
    machine = model.get("machine")
    if not isinstance(machine, dict) or machine.get("model_role") != "reference":
        return

    def visit(value: Any, path: str) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                child_path = f"{path}.{key}" if path else key
                if key in IMPLEMENTATION_FIELDS:
                    raise ResolutionError(
                        "reference model cannot contain implementation state at "
                        f"{child_path}"
                    )
                visit(child, child_path)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, f"{path}[{index}]")

    visit(model, "")


def reject_routed_firmware_graphs(model):
    composition = model.get("composition", {})
    if (model.get("machine", {}).get("model_role") != "emulation-target" or
            _fabric_topology(model) != "router"):
        return False
    if any("module_role" in item for item in composition.get("module_instances", [])):
        raise ResolutionError("routed target must not assign firmware graph module roles")
    return True


def component_exclusions(
        declarations: list[tuple[str, Any]], components: Any) -> list[dict[str, Any]]:
    exclusions: list[dict[str, Any]] = []
    components = components if isinstance(components, list) else []
    seen: dict[tuple[str | None, str | None], str] = {}
    for owner, value in declarations:
        if value is None:
            continue
        if not isinstance(value, list) or not value:
            raise ResolutionError(f"{owner} excluded_components must be a non-empty list")
        for index, exclusion in enumerate(value):
            where = f"{owner} excluded_components entry {index}"
            if not isinstance(exclusion, dict):
                raise ResolutionError(f"{where} must be a mapping")
            unknown = sorted(set(exclusion) - EXCLUSION_FIELDS)
            if unknown:
                raise ResolutionError(f"{where} has unsupported field '{unknown[0]}'")
            match = exclusion.get("match")
            if (not isinstance(match, dict) or not match or
                    set(match) - EXCLUSION_MATCH_FIELDS):
                raise ResolutionError(f"{where} match must select by id, kind, or both")
            if any(not isinstance(item, str) or not item for item in match.values()):
                raise ResolutionError(f"{where} selector values must be non-empty strings")
            reason = exclusion.get("reason")
            if not isinstance(reason, str) or not reason:
                raise ResolutionError(f"{where} requires a reason")
            selector = (match.get("id"), match.get("kind"))
            label = _selector_label(match)
            if selector in seen:
                raise ResolutionError(
                    f"duplicate excluded_components selector {label} in "
                    f"{seen[selector]} and {owner}"
                )
            seen[selector] = owner
            for component in components:
                if (isinstance(component, dict) and
                        all(component.get(field) == value
                            for field, value in match.items())):
                    raise ResolutionError(
                        f"{owner} excludes component '{component.get('id')}' matching "
                        f"{label}, but the resolved inventory contains it"
                    )
            exclusions.append(copy.deepcopy(exclusion))
    return exclusions


def check_composition(model: dict[str, Any]) -> dict[str, Any]:
    """Run every post-resolution composition invariant, in order.

    The order matches what a resolved model must satisfy end to end: routed
    firmware graphs, reference-model purity, the emulation profile, CPU and
    memory populations, direct references, physical slots, connector
    population, synthetic identities, initial module numbers, hub NI
    revision overrides, board identity overrides, and the excluded_components
    ledger the resolved model itself carries. Each check raises on its own
    first violation; nothing here collects a list, matching the resolver's
    fail-fast style.
    """
    if not isinstance(model.get("composition"), dict):
        raise ResolutionError("system model requires composition")
    reject_routed_firmware_graphs(model)
    reject_reference_implementation(model)
    validate_emulation_profile(model)
    validate_model_cpu_populations(model)
    validate_model_memory_population(model)
    validate_direct_model_references(model)
    validate_physical_slots(model)
    validate_connector_population(model)
    validate_synthetic_identities(model)
    validate_initial_module_numbers(model)
    validate_hub_ni_revision_overrides(model)
    validate_board_identity_overrides(model)
    validate_system_timekeepers(model)
    component_exclusions([("model", model.get("excluded_components"))], model.get("components"))
    return model
