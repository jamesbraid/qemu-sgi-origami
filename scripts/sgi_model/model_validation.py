# SPDX-License-Identifier: BSD-3-Clause

"""Load and validate system models.

Two passes. The first checks the document against system-model.schema.json,
which catches wrong types, unknown keys and malformed addresses. The second
checks the things a schema cannot express: that every identifier a connection
or an interrupt chain refers to actually names a component, a bus or an address
space, and that the graph has no dangling edge.

A model that passes both is safe for a generator or a test to walk without
defensive lookups.

"""

import json
import pathlib

import jsonschema
import yaml

from .exceptions import ResolutionError
from .system_model import contains_observed_hardware, load_system_model
SHARED_ASIC_KINDS = {
    "node-asic", "pci-host-bridge", "io-controller", "storage-hba",
    "flash", "rtc", "bus-controller", "system-controller",
    "router-asic", "crossbar",
}


def _address_views(component: dict) -> list:
    """Region-shaped address entries a component authors.

    Most components list them under "regions". A flash component lists its
    payload aliases under "views" instead: every entry there is the same
    underlying storage, unlike distinct regions.
    """
    return component.get("regions") or component.get("views") or []


def _module_instance_entries(model: dict, prefix: str) -> list[dict]:
    entries = []
    for field in ("components", "buses", "interrupts", "open_questions"):
        entries.extend(
            entry for entry in model.get(field, [])
            if entry["id"].startswith(prefix)
        )
    entries.extend(
        connection for connection in model.get("connections", [])
        if any(connection[field].startswith(prefix)
            for field in ("component", "bus")
        )
    )
    return entries


def check_schema(model: dict, schema: dict) -> list[str]:
    """Validate the model's shape against the JSON schema."""
    problems: list[str] = []
    validator = jsonschema.Draft202012Validator(schema)
    for error in sorted(validator.iter_errors(model), key=lambda e: list(e.path)):
        where = "/".join(str(p) for p in error.path) or "(document root)"
        problems.append(f"schema: {where}: {error.message}")
    return problems


def check_ids_and_references(
        model: dict) -> tuple[list[str], dict[str, dict], dict[str, dict], dict[str, dict]]:
    """Index every id and check the facts that hang off it.

    Duplicate component, bus, address-space and interrupt ids, and validate
    references from component parents, address regions, bus masters, and
    connection controllers. Return the indexes used by later checks.
    Returns the problems alongside the component/bus/address-space indexes
    later checks reuse rather than rebuild. Enforce the strict evidence policy
    and require direct captures for observed-hardware claims.
    """
    problems: list[str] = []

    def index_entries(kind: str, entries: list[dict]) -> dict[str, dict]:
        indexed = {}
        for entry in entries:
            ident = entry["id"]
            if ident in indexed:
                problems.append(f"duplicate {kind} id '{ident}'")
            indexed[ident] = entry
        return indexed

    components = index_entries("component", model["components"])
    buses = index_entries("bus", model["buses"])
    spaces = index_entries("address space", model["address_spaces"])
    index_entries("interrupt", model.get("interrupts", []))

    composition = model["composition"]
    for instance in composition.get("module_instances", []):
        prefix = instance["prefix"]
        basis = instance.get("observed_hardware")
        if (contains_observed_hardware(_module_instance_entries(model, prefix)) and
                (not isinstance(basis, dict) or basis.get("mode") != "direct")):
            problems.append(
                f"module instance '{prefix}' retains observed-hardware claims "
                "without a direct instance capture"
            )

    def require_evidence(fact: dict, where: str) -> None:
        if fact.get("provenance") == "unsourced" or "evidence_gap" in fact:
            problems.append(f"strict evidence policy rejects unsourced {where}")
            return
        if "provenance" not in fact:
            problems.append(f"strict evidence policy requires provenance for {where}")
        if not fact.get("sources"):
            problems.append(f"strict evidence policy requires sources for {where}")

    require_evidence(composition["topology"], "topology")
    require_evidence(composition["population"], "population")
    require_evidence(composition["limits"], "capacity limits")
    for selection in composition.get("connector_population", []):
        component_id = selection["component"]
        require_evidence(selection, f"connector population '{component_id}'")
    if model.get("memory_geometry"):
        require_evidence(model["memory_geometry"], "memory geometry")
    firmware_config = model["machine"].get("firmware_config")
    if firmware_config:
        require_evidence(firmware_config, "firmware configuration")
    firmware_pci = ((model["machine"].get("emulation") or {}).get(
        "firmware_pci_resources"
    ))
    if firmware_pci:
        require_evidence(firmware_pci, "emulator firmware PCI resources")
    firmware_payload = ((model["machine"].get("emulation") or {}).get(
        "firmware_payload"
    ))
    if firmware_payload:
        require_evidence(firmware_payload, "emulator firmware payload")
    population = composition["population"]
    if population:
        fitted_memory = population.get("fitted_memory")
        if fitted_memory:
            require_evidence(fitted_memory, "fitted memory")
    for bus in model["buses"]:
        require_evidence(bus, f"bus '{bus['id']}'")
    for index, connection in enumerate(model["connections"]):
        require_evidence(connection, f"connection {index}")
    for interrupt in model.get("interrupts", []):
        require_evidence(interrupt, f"interrupt '{interrupt['id']}'")
    for exclusion in model.get("excluded_components", []):
        match = exclusion["match"]
        label = ", ".join(
            f"{field}='{match[field]}'"
            for field in ("id", "kind") if field in match
        )
        require_evidence(exclusion, f"excluded components selector {label}")



    for cid, comp in components.items():
        if "parent" in comp:
            known("component", components, comp["parent"], f"component '{cid}' parent",
                  problems)
        for region in _address_views(comp):
            where = f"component '{cid}' region '{region['name']}'"
            if "space" in region:
                known("address space", spaces, region["space"], where, problems)
            else:
                # A chip-relative offset must name the chip, and that chip must
                # be a component — otherwise the offset means nothing.
                known("component", components, region["relative_to"], where, problems)

    for bus in model["buses"]:
        if "master" in bus:
            known("component", components, bus["master"], f"bus '{bus['id']}' master",
                  problems)
        for master in bus.get("masters", []):
            known("component", components, master, f"bus '{bus['id']}' master", problems)

    for index, connection in enumerate(model["connections"]):
        if "controlled_by" in connection:
            known("component", components, connection["controlled_by"],
                  f"connection {index} controller", problems)

    return problems, components, buses, spaces


def known(kind: str, ids: dict | set, ident: str, where: str, problems: list[str]) -> None:
    if ident not in ids:
        problems.append(f"{where}: no such {kind} '{ident}'")


def check_containment_cycles(model: dict, components: dict[str, dict]) -> list[str]:
    """Reject a component containment graph that loops back on itself."""
    problems: list[str] = []
    visiting: list[str] = []
    visited: set[str] = set()

    def visit_parent(cid: str) -> None:
        if cid in visiting:
            start = visiting.index(cid)
            cycle = visiting[start:] + [cid]
            problems.append("component containment cycle: " + " -> ".join(cycle))
            return
        if cid in visited:
            return
        visiting.append(cid)
        parent = components[cid].get("parent")
        if parent in components:
            visit_parent(parent)
        visiting.pop()
        visited.add(cid)

    for cid in components:
        visit_parent(cid)
    return problems


def check_population_capacity(model: dict, components: dict[str, dict]) -> list[str]:
    """Check a declared population against the hardware it populates.

    A router-asic must not own more fabric links than it has ports; the
    per-node CPU and memory-bank populations, paired positionally with the
    node-asic and memory components that carry a comparable capacity, must
    not exceed it.
    """
    problems: list[str] = []
    population = model["composition"]["population"]

    for cid, component in components.items():
        if component["kind"] != "router-asic":
            continue
        endpoint_buses = {
            connection["bus"]
            for connection in model["connections"]
            if connection.get("component") == cid
        }
        fabric_links = sum(
            bus["kind"] == "fabric" and
            (bus.get("master") == cid or bus["id"] in endpoint_buses)
            for bus in model["buses"]
        )
        port_count = component["router_asic"]["port_count"]
        if fabric_links > port_count:
            problems.append(
                f"router-asic '{cid}' must not own more than {port_count} "
                f"fabric links, got {fabric_links}"
            )

    # Node order in the population's per-node lists is a convention shared
    # with the resolver (population["cpus_per_node"][index] means node
    # "index"); node-asic components are authored in the same node order, so
    # pairing them positionally checks each node's population against that
    # node's own ASIC without needing a graph-containment boundary every
    # authoring style in this catalogue actually provides. A count mismatch
    # means the pairing is not established here, and the check is skipped
    # rather than guessed.
    cpus_per_node = population.get("cpus_per_node")
    if isinstance(cpus_per_node, list):
        node_asics = [c for c in model["components"] if c.get("kind") == "node-asic"]
        if len(node_asics) == len(cpus_per_node):
            for node, (comp, count) in enumerate(zip(node_asics, cpus_per_node)):
                facts = comp["node_asic"]
                interfaces = facts["processor_interfaces"]
                per_interface = facts["cpus_per_interface"]
                capacity = interfaces * per_interface
                if isinstance(count, int) and count > capacity:
                    problems.append(
                        f"node {node} populates {count} CPUs, but node-asic "
                        f"'{comp['id']}' supports at most {capacity} "
                        f"({interfaces} processor interface"
                        f"{'s' if interfaces != 1 else ''} x {per_interface} "
                        "CPUs each)"
                    )

    # Same positional pairing for memory banks, over only the memory
    # components that carry a bank count: most memory-kind components
    # (directory memory, a board-local SSRAM) have no comparable geometry
    # and the block is left off them rather than filled with a guess.
    memory_bank_mib_per_node = population.get("memory_bank_mib_per_node")
    if isinstance(memory_bank_mib_per_node, list):
        memory_components = [
            c for c in model["components"]
            if c.get("kind") == "memory" and "banks" in (c.get("memory") or {})
        ]
        if len(memory_components) == len(memory_bank_mib_per_node):
            for node, (comp, banks) in enumerate(
                    zip(memory_components, memory_bank_mib_per_node)):
                bank_limit = comp["memory"]["banks"]
                if not isinstance(banks, list):
                    continue
                populated = sum(1 for size in banks if size)
                if populated > bank_limit:
                    problems.append(
                        f"node {node} populates {populated} memory banks, "
                        f"but memory '{comp['id']}' supports at most "
                        f"{bank_limit}"
                    )
    return problems


def check_widget_and_pci_uniqueness(
        model: dict, components: dict[str, dict], buses: dict[str, dict]) -> list[str]:
    """Check XIO widget and PCI slot claims, and link-bus shape.

    A typed link is a point-to-point join between exactly two ports, and
    only a cable carries cable metadata. A XIO widget number and a PCI
    (bus, device, function) slot must not be claimed by two components; a
    widget's discovery/enumerated/master/decoded/legacy roles must not
    overlap without an exact identity_aliases entry naming them. A connector
    component must appear in exactly the connections that wire it up, all
    with the endpoint role.
    """
    problems: list[str] = []

    connections_per_bus: dict[str, int] = {}
    for conn in model["connections"]:
        bus_id = conn.get("bus")
        connections_per_bus[bus_id] = connections_per_bus.get(bus_id, 0) + 1
    for bus in model["buses"]:
        link = bus.get("link")
        if link is None:
            continue
        # The schema requires kind "fabric" whenever link is present.
        count = connections_per_bus.get(bus["id"], 0)
        if count != 2:
            problems.append(
                f"link bus '{bus['id']}' joins exactly two ports, got {count}"
            )
        if link.get("kind") not in {"cable", "xpress-cable"}:
            for field in ("part", "length_in", "color"):
                if field in link:
                    problems.append(
                        f"link bus '{bus['id']}' kind '{link['kind']}' cannot "
                        f"carry cable field '{field}'"
                    )

    seen_placements: dict[tuple, str] = {}
    physical_ports: dict[tuple[str, int], tuple[str, int]] = {}
    fabric_ports: dict[tuple[str, int], int] = {}
    widget_claims: dict[tuple[str, int], list[tuple[str, str, int]]] = {}
    for i, conn in enumerate(model["connections"]):
        where = f"connection {i} ({conn.get('component')} -> {conn.get('bus')})"
        known("component", components, conn["component"], where, problems)
        known("bus", buses, conn["bus"], where, problems)

        bus = buses.get(conn["bus"])
        if "fabric_port" in conn:
            if bus is None or bus.get("kind") != "fabric":
                problems.append(
                    f"{where}: fabric_port is only valid on a fabric bus")
            else:
                key = (conn["component"], conn["fabric_port"])
                if key in fabric_ports:
                    problems.append(
                        f"{where}: fabric port {conn['fabric_port']} on component "
                        f"'{conn['component']}' is already used by connection "
                        f"{fabric_ports[key]}")
                else:
                    fabric_ports[key] = i
        identities = conn.get("xio")
        if (identities is not None or "widget" in conn) and (
                bus is None or bus["kind"] != "xio"):
            problems.append(f"{where}: widget identities are only valid on an XIO bus")
        if identities is not None:
            if "physical_port" in identities:
                slot = (conn["bus"], identities["physical_port"])
                prior = physical_ports.get(slot)
                if prior is not None and prior != (conn["component"], i):
                    problems.append(
                        f"{where}: physical port {identities['physical_port']} on bus "
                        f"'{conn['bus']}' is already attached to '{prior[0]}'"
                    )
                physical_ports[slot] = (conn["component"], i)

            local_roles: dict[int, set[str]] = {}

            def claim(widget: int, role: str) -> None:
                local_roles.setdefault(widget, set()).add(role)
                widget_claims.setdefault((conn["bus"], widget), []).append(
                    (conn["component"], role, i)
                )

            for field, role in (("discovery_widget", "discovery"),
                                ("enumerated_widget", "enumerated"),
                                ("master_widget", "master")):
                if field in identities:
                    claim(identities[field], role)
            for widget in identities.get("decoded_widgets", []):
                claim(widget, "decoded")
            if "widget" in conn:
                claim(conn["widget"], "legacy")

            aliases = {alias["widget"]: set(alias["roles"])
                       for alias in identities.get("identity_aliases", [])}
            for widget, roles in local_roles.items():
                if len(roles) > 1 and aliases.get(widget) != roles:
                    problems.append(
                        f"{where}: widget {widget} overlaps "
                        f"{' and '.join(sorted(roles))} roles without an exact "
                        f"identity_aliases entry"
                    )
            for widget, roles in aliases.items():
                if local_roles.get(widget) != roles:
                    problems.append(
                        f"{where}: widget {widget} identity_aliases names roles "
                        f"that this attachment does not have"
                    )
        elif "widget" in conn:
            widget_claims.setdefault((conn["bus"], conn["widget"]), []).append(
                (conn["component"], "legacy", i)
            )

        # Two components cannot share a widget number or a PCI slot on one bus.
        for key in ("pci_device",):
            if key in conn:
                slot = (conn["bus"], key, conn[key], conn.get("pci_function", 0))
                prior = seen_placements.get(slot)
                if prior is not None and prior != conn["component"]:
                    problems.append(
                        f"{where}: {key} {conn[key]} on bus '{conn['bus']}' is "
                        f"already taken by '{prior}'")
                seen_placements[slot] = conn["component"]

    for (bus_id, widget), claims in widget_claims.items():
        owners = {(component, index) for component, _role, index in claims}
        roles = {role for _component, role, _index in claims}
        if len(owners) < 2 or roles == {"master"}:
            continue
        duplicate_role = None
        duplicate_owners: list[str] = []
        for role in sorted(roles):
            role_owners = list(dict.fromkeys(
                component for component, claim_role, _index in claims
                if claim_role == role
            ))
            if role != "master" and len(role_owners) > 1:
                duplicate_role = role
                duplicate_owners = role_owners
                break
        if duplicate_role is not None:
            problems.append(
                f"{duplicate_role} widget {widget} on bus '{bus_id}' is claimed "
                f"by '{duplicate_owners[0]}' and '{duplicate_owners[-1]}'"
            )
            continue

        by_role = {role: component for component, role, _index in claims}
        if ("enumerated" in by_role and "decoded" in by_role and
                by_role["enumerated"] != by_role["decoded"]):
            problems.append(
                f"widget {widget} is enumerated by '{by_role['enumerated']}' and "
                f"decoded by '{by_role['decoded']}' on bus '{bus_id}'"
            )
            continue
        details = ", ".join(
            f"{role} by '{component}'"
            for component, role, _index in dict.fromkeys(claims)
        )
        problems.append(
            f"widget {widget} has cross-role claims on bus '{bus_id}': {details}"
        )

    for cid, component in components.items():
        if component["kind"] != "connector":
            continue
        attachments = [connection for connection in model["connections"]
                       if connection["component"] == cid]
        if not attachments:
            problems.append(f"connector '{cid}' has no bus attachment")
            continue
        for connection in attachments:
            if connection.get("role") != "endpoint":
                problems.append(f"connector '{cid}' must use endpoint role")

    return problems


def check_firmware_windows(model: dict) -> list[str]:
    """Check firmware address windows against memory and against themselves.

    A flash component's memory-space region must not overlap a populated
    node's main memory; a firmware payload's views must fit inside the
    payload and have non-zero size.
    """
    problems: list[str] = []
    population = model["composition"]["population"]

    memory_by_node = population.get("memory_mib_per_node", [])
    if isinstance(memory_by_node, list):
        for component in model["components"]:
            if component.get("kind") != "flash":
                continue
            for region in _address_views(component):
                if region.get("space") != "memory" or "size" not in region:
                    continue
                start = int(str(region["offset"]).replace("_", ""), 0)
                end = start + int(str(region["size"]).replace("_", ""), 0)
                for node, memory_mib in enumerate(memory_by_node):
                    memory_end = memory_mib << 20
                    if start < memory_end and end > 0:
                        problems.append(
                            f"firmware window '{component['id']}/{region['name']}' "
                            f"overlaps node {node}'s {memory_mib} MiB memory"
                        )

    firmware_payload = ((model["machine"].get("emulation") or {}).get(
        "firmware_payload"
    ))
    if firmware_payload:
        payload_size = int(str(firmware_payload["size"]).replace("_", ""), 0)
        for view in firmware_payload["views"]:
            offset = int(str(view["payload_offset"]).replace("_", ""), 0)
            size = int(str(view["size"]).replace("_", ""), 0)
            if size == 0:
                problems.append(
                    f"firmware payload view '{view['name']}' has zero size"
                )
            elif offset + size > payload_size:
                problems.append(
                    f"firmware payload view '{view['name']}' exceeds the "
                    "supplied payload"
                )
    return problems


def check_interrupt_chains(model: dict, components: dict[str, dict]) -> list[str]:
    """Check that every interrupt's source and chain hops name real components."""
    problems: list[str] = []
    for irq in model.get("interrupts", []):
        known("component", components, irq["source"], f"interrupt '{irq['id']}' source",
              problems)
        for j, hop in enumerate(irq["chain"]):
            known("component", components, hop["at"], f"interrupt '{irq['id']}' hop {j}",
                  problems)
    return problems


def check_open_question_shape(model: dict, components: dict[str, dict]) -> list[str]:
    """Check that an open question's "about" ids name real components."""
    problems: list[str] = []
    for question in model.get("open_questions", []):
        for ident in question.get("about", []):
            known("component", components, ident, f"open question '{question['id']}'",
                  problems)
    return problems


def check(path: pathlib.Path, schema: dict, *, model: dict | None = None) -> list[str]:
    """Return a list of problems; empty means the model is good."""
    if model is None:
        try:
            model = load_system_model(path)
        except ResolutionError as exc:
            return [f"resolution: {exc}"]

    if not isinstance(model, dict) or "model_version" not in model:
        return []  # not a system model

    problems = check_schema(model, schema)
    if problems:
        # Referential checks assume the shape held; skip them if it did not.
        return problems

    problems, components, buses, _spaces = check_ids_and_references(model)
    problems += check_containment_cycles(model, components)
    problems += check_population_capacity(model, components)
    problems += check_widget_and_pci_uniqueness(model, components, buses)
    problems += check_firmware_windows(model)
    problems += check_interrupt_chains(model, components)
    problems += check_open_question_shape(model, components)
    return problems


class ModelValidationError(ValueError):
    """A resolved model does not satisfy its checked contract."""

    def __init__(self, path: pathlib.Path, problems: list[str]):
        self.path = path
        self.problems = problems
        super().__init__(f"{path}: " + "; ".join(problems))


def schema(model_root: pathlib.Path) -> dict:
    """Read the schema beside the selected model inputs."""
    return json.loads((model_root / "system-model.schema.json").read_text())


def discover_model_paths(model_root: pathlib.Path) -> list[pathlib.Path]:
    """Find every top-level composition in the model-input directory."""
    paths = []
    for path in sorted(model_root.glob("*.yaml")):
        document = yaml.safe_load(path.read_text())
        if isinstance(document, dict) and "configuration_version" in document:
            paths.append(path)
    return paths


def load_checked_system_model(path: pathlib.Path, *, model_schema: dict) -> dict:
    """Resolve one public model and check its schema and graph structure."""
    path = pathlib.Path(path)
    try:
        model = load_system_model(path)
    except ResolutionError as exc:
        raise ModelValidationError(path, [f"resolution: {exc}"]) from exc
    if not isinstance(model, dict) or "model_version" not in model:
        raise ModelValidationError(path, ["not a system model"])
    problems = check(path, model_schema, model=model)
    if problems:
        raise ModelValidationError(path, problems)
    return model


def load_checked_system_models(paths: list[pathlib.Path], *,
                               model_root: pathlib.Path) -> list[tuple[pathlib.Path, dict]]:
    """Load the selected public models with one shared schema."""
    model_schema = schema(model_root)
    models = [
        (pathlib.Path(path), load_checked_system_model(path, model_schema=model_schema))
        for path in paths
    ]
    return models


def describe_model(path: pathlib.Path, model: dict) -> str:
    """Return the compact status line used by the model checker."""
    extra = ""
    mix: dict[str, int] = {}
    for component in model["components"]:
        provenance = component["provenance"]
        mix[provenance] = mix.get(provenance, 0) + 1
    extra += ", " + "/".join(f"{count} {kind}" for kind, count in sorted(mix.items()))
    if "interrupts" not in model:
        extra += ", interrupt chains not recorded"
    questions = model.get("open_questions", [])
    if questions:
        extra += (f", {len(questions)} open question"
                  f"{'s' if len(questions) != 1 else ''} "
                  f"({', '.join(question['id'] for question in questions)})")
    return (f"ok   {path}: {len(model['components'])} components, "
            f"{len(model['buses'])} buses, "
            f"{len(model['connections'])} connections, "
            f"{len(model.get('interrupts', []))} interrupt chains{extra}")
