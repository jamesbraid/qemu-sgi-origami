# SPDX-License-Identifier: BSD-3-Clause

"""Compile a resolved system model into the format-4 catalogue data.

One compile path takes a resolved composition and produces the record shapes
consumed by QEMU's SGI catalogue emitter, over one typed index of the physical
component graph. The compiler never branches on a
product name: every distinction it makes reads a fact already in the
resolved model (an asic kind, whether a crossbar is present, an authored
physical slot number), so a new product that fits the same vocabulary needs
no change here.

A reference-role configuration (`machine.model_role != "emulation-target"`)
is refused, never compiled: format 2 carries no facts for a system nobody
has committed to emulating. A configuration this compiler cannot yet ground
in the resolved model -- a processor identity register the catalogue does
not carry, a reset-mode encoding no product here authors -- is refused the
same way, naming the missing fact, rather than inventing a value.
"""

from __future__ import annotations

from collections.abc import Callable

from .model_shared import (
    _compile_r10000_reset_mode, _integer, _required, _validate_integer, num,
)

# The format-2 memory record always names eight banks (SGI_MODEL_MAX_MEMORY_BANKS
# in sn-model.h); a node that ships fewer is zero-padded.
MAX_MEMORY_BANKS = 8


class CompileError(ValueError):
    """A resolved model lacks a fact format 2 needs, or names one this
    compiler does not yet support. Never invents the missing value."""


# ---------------------------------------------------------------------------
# Typed graph index
#
# The physical containment graph (components, their `parent` chain, and the
# buses/connections between them) is read once into a small set of typed
# records -- nodes, routers, crossbars, I/O boards, fabric links, XIO
# attachments and slots -- that every later extraction function reads
# instead of walking `model["components"]` again. This is the "typed graph
# index" the plan's nested-instances and format-2 steps both describe: one
# index shape whether a product authors SGI's numbered physical slots
# (Origin 2000's node and router boards) or ships as a single flat board
# with none (the Origin 200 family), and whether its nodes sit directly
# under one module component or under an implicit one this index supplies
# for a unibody product that authors no module component at all.
# ---------------------------------------------------------------------------

def _components_by_id(model: dict) -> dict[str, dict]:
    return {item["id"]: item for item in model["components"]}


# The implicit single module a flat, unibody product with no authored
# `kind: module` component needs, since format 2 still groups every node
# under exactly one `SGIModelModule`. One shared sentinel: every top-level
# component (each its own "outermost ancestor" once the parent chain runs
# out) must resolve to the *same* implicit module, not one each, or a
# product whose physical components are all authored at the top level
# (Origin 300's Bedrock, L1, XBridge and I/O functions all have no parent
# at all) would compile as one module per component instead of one module
# for the whole product.
_IMPLICIT_MODULE = {"id": "implicit-module"}


def _grouping_module(components: dict[str, dict], component: dict) -> dict:
    """The nearest 'module'-kind ancestor, or `_IMPLICIT_MODULE` when the
    product authors no module component at all.
    """
    node = component
    seen = set()
    while True:
        if node.get("kind") == "module":
            return node
        parent_id = node.get("parent")
        if parent_id is None:
            return _IMPLICIT_MODULE
        if node["id"] in seen:
            raise CompileError(f"cyclic containment at '{node['id']}'")
        seen.add(node["id"])
        node = components[parent_id]


def _board_ancestor(components: dict[str, dict], component: dict) -> dict | None:
    """The nearest 'board'-kind ancestor, or None past the containment root.

    Its authored `physical_slot.number`, when present, is SGI's own
    numbered node, router or XIO position (the Origin 2000 family); a
    flat, unibody product with no such board (the Origin 200 family)
    leaves `_positioned` to fall back to positional order within the
    module.
    """
    node = component
    seen = set()
    while node.get("kind") != "board":
        parent_id = node.get("parent")
        if parent_id is None or node["id"] in seen:
            return None
        seen.add(node["id"])
        node = components[parent_id]
    return node


def _node_scope(components: dict[str, dict], component: dict) -> dict | None:
    """The nearest ancestor scoping one physical node.

    A 'carrier' (SGI's own term for the Origin 200's per-node daughtercard
    set -- the component taxonomy's physical-unit level between module and
    board) if the containment chain reaches one before a module; otherwise
    the nearest 'board' (the Origin 2000 family, whose node board is both
    the carrier and the direct parent of its Hub and CPUs). A Hub and its
    own node's CPU, flash and memory share this ancestor even when SGI put
    them on separate boards within it, as the Origin 200 does with its
    system board and PIMM daughtercard.
    """
    node = component
    seen = set()
    while True:
        kind = node.get("kind")
        if kind == "carrier":
            return node
        if kind == "module" or node.get("parent") is None:
            return _board_ancestor(components, component)
        if node["id"] in seen:
            raise CompileError(f"cyclic containment at '{node['id']}'")
        seen.add(node["id"])
        node = components[node["parent"]]


def _positioned(components: dict[str, dict], predicate: Callable[[dict], bool],
                modules: dict[str, int]) -> list[dict]:
    """Every component matching `predicate`, indexed into its module and
    given a slot.

    `modules` maps a grouping module's component id to its index, shared
    across every kind positioned for one configuration so a node, a router
    and an I/O board that share a module agree on its index; callers seed
    it by positioning nodes first. Ordering is by component id, which is
    deterministic and -- for every configuration this compiler currently
    reaches -- already the SGI-documented order (module_instances prefixes
    "node0-", "node1-", ...; Origin 2000 node board instances prefix
    "node-board-1-".."node-board-4-", so their Hub ids sort the same way).
    """
    found = sorted((item for item in components.values() if predicate(item)),
                   key=lambda item: item["id"])
    per_module_next: dict[str, int] = {}
    entries = []
    for component in found:
        module = _grouping_module(components, component)
        module_index = modules.setdefault(module["id"], len(modules))
        board = _board_ancestor(components, component)
        slot = None
        hub_slot_bits = None
        if board is not None:
            declared = board.get("physical_slot")
            if isinstance(declared, dict) and isinstance(declared.get("number"), int):
                slot = declared["number"]
                hub_slot_bits = declared.get("hub_slot_bits")
        if slot is None:
            slot = per_module_next.get(module["id"], 0)
        per_module_next[module["id"]] = slot + 1
        scope = _node_scope(components, component)
        entries.append({"id": component["id"], "component": component,
                        "board_id": scope["id"] if scope is not None else component["id"],
                        "module_id": module["id"], "module_index": module_index,
                        "slot": slot, "hub_slot_bits": hub_slot_bits})
    return entries


def _fabric_links(model: dict, components: dict[str, dict],
                   node_index: dict[str, int], router_index: dict[str, int]) -> list[dict]:
    """Fabric-bus links between two Hub/Router endpoints, port-numbered.

    Mirrors `origin2000_model.compile_physical_graph`'s link extraction,
    which already makes no per-product distinction; a node's port is
    always 0 (one network interface per Hub or Bedrock), a router's port
    comes from its `fabric_port` connection attribute.

    A chassis that authors node-population rows lists the links of every
    row it accepts, and two rows may wire one Hub differently (a
    two-node deskside's Null Router, a four-node one's Router port), so
    its endpoints are checked once per row instead
    (`_compile_node_populations`), not across the whole list here.
    """
    per_row = bool((model["composition"].get("topology") or {}).get("node_populations"))
    vertices = {**{cid: ("NODE", index) for cid, index in node_index.items()},
                **{cid: ("ROUTER", index) for cid, index in router_index.items()}}
    occupied = set()
    links = []
    for bus in model["buses"]:
        if bus["kind"] != "fabric":
            continue
        attachments = _connections_for_bus(model, bus["id"])
        real = [item for item in attachments if item["component"] in vertices]
        if len(real) < 2:
            # A one-ended fabric bus (an expansion port this configuration
            # leaves unwired, such as the Origin 200's single-node
            # `craylink`, with no second attachment at all) or one cabled
            # to an authored but unpopulated connector (the Origin 300's
            # NUMAlink port, "cabled to nothing on one chassis") names no
            # link at all; that is not a missing fact, it is the absence
            # of a link.
            others = [item for item in attachments if item["component"] not in vertices]
            if any(components.get(item["component"], {}).get("kind") != "connector"
                   for item in others):
                raise CompileError(
                    f"fabric bus '{bus['id']}' has an endpoint that is "
                    "neither a Hub/Router nor an unpopulated connector"
                )
            continue
        if len(real) > 2:
            raise CompileError(
                f"fabric bus '{bus['id']}' has more than two Hub/Router endpoints")
        link = bus.get("link")
        if not isinstance(link, dict) or "kind" not in link:
            raise CompileError(
                f"fabric bus '{bus['id']}' has two endpoints but authors no "
                "link.kind; format 2 needs it to select SGIModelFabricLinkKind"
            )
        kind = link["kind"]
        if kind not in {"midplane", "cable", "xpress-cable", "carrier", "null-router"}:
            raise CompileError(f"fabric bus '{bus['id']}' has an unsupported link kind '{kind}'")
        ends = []
        for attachment in real:
            cid = attachment["component"]
            vertex_kind, index = vertices[cid]
            if vertex_kind == "NODE":
                port = 0
            else:
                port = _integer(attachment, "fabric_port", f"router '{cid}' fabric endpoint", 8)
            key = (vertex_kind, index, port)
            if key in occupied and not per_row:
                raise CompileError(f"fabric endpoint {key} is attached twice")
            occupied.add(key)
            ends.append({"kind": vertex_kind, "index": index, "port": port})
        links.append({"name": bus["id"], "ends": ends, "kind": kind})
    return links


def _connections_for_bus(model: dict, bus_id: str) -> list[dict]:
    return [item for item in model["connections"] if item["bus"] == bus_id]


def _split_bridge_parents(model: dict, components: dict[str, dict]) -> set[str]:
    """`pci-host-bridge` components superseded by their own head children.

    The Origin 2000/200 family's Bridge is itself both the identity-holder
    and the one XIO attachment point for its board. The Origin 300
    XBridge instead splits into two independently-widgeted heads, each a
    `subfunction` child that masters its own PCI bus (the option bus and
    the IO-8 bus); the parent XBridge component never itself attaches to
    XIO. A parent whose children already do this is excluded from
    `pci-host-bridge` positioning so it does not appear as an orphaned,
    unattached board; the Bedrock compile path positions its head children
    directly instead (see `_compile_bedrock_io_boards`).
    """
    pci_masters = {bus["master"] for bus in model["buses"]
                   if bus["kind"] == "pci" and "master" in bus}
    parents = set()
    for cid in pci_masters:
        component = components.get(cid)
        if component is None:
            continue
        parent = components.get(component.get("parent"))
        if parent is not None and parent.get("kind") == "pci-host-bridge":
            parents.add(parent["id"])
    return parents


_OCCUPANT_PLACEMENT_FIELDS = ("widget", "controlled_by", "xio")

# XIO widgets that attach to a connector's bus directly, with no
# separate `pci-host-bridge` chip standing in for the board: XG, the
# InfiniteReality pipe's XIO-to-graphics ASIC on its geometry board
# (docs/datasheets/onyx2.md, Pipe identity), and HQ4, the SI graphics
# board's (MGRAS) host interface.
# A board's `hardware_kind` here is what `_XIO_BOARD_KINDS` (this module,
# `_compile_io_board`) later maps to a catalogue preset; the two dicts
# name the same boards from two different components of the same board
# (this one keys the widget attachment, `_XIO_BOARD_KINDS` keys the
# enclosing board). A direct Odyssey widget is a board preset; a V-brick's
# internal Odyssey belongs to the enclosing V-brick preset instead.
_XIO_WIDGET_KINDS = frozenset({"xg", "hq4", "odyssey"})


def _is_xio_occupant(component: dict) -> bool:
    """True for the one component an occupied connector's XIO bus
    attaches to: a `pci-host-bridge` (a PCI Bridge/XBridge chip), or a
    board of a declared XIO widget kind that attaches to XIO
    directly with no separate Bridge (`_XIO_WIDGET_KINDS`). Either is
    identity-bearing: `model_composition.validate_synthetic_identities`
    makes the same distinction for the same structural reason.
    """
    return (component.get("kind") == "pci-host-bridge"
            or component.get("hardware_kind") in _XIO_WIDGET_KINDS)


def _inside_vbrick(components: dict[str, dict], component: dict) -> bool:
    """A V-brick's internal V12 belongs to its enclosing XBridge preset."""
    parent = component.get("parent")
    while parent is not None:
        ancestor = components[parent]
        if ancestor.get("kind") == "module":
            return ancestor.get("hardware_kind") == "vbrick"
        parent = ancestor.get("parent")
    return False


def _slot_label(connector: dict) -> str:
    """The IRIX slot name of an XIO connector position, from its authored
    location: `IO5` is `io5`, the name `/hw/module/1/slot/io5` and a
    `-device ...,slot=io5` both use.
    """
    location = (connector.get("connector") or {}).get("location")
    if not isinstance(location, str) or not location.strip():
        raise CompileError(f"XIO connector '{connector['id']}' has no location")
    return location.strip().lower()


def _occupied_connectors(model: dict, components: dict[str, dict]) -> dict[str, str]:
    """Each occupied connector's id, mapped to the Bridge (or bridge-less
    XIO widget board, `_is_xio_occupant`) of the board in it.

    The composition's `connector_population` names the occupant board; the
    occupant component is that board's one Bridge or widget attached to
    the connector's own XIO bus. The connector keeps the slot's
    placement (widget, controlling node, physical port), so that
    attachment must carry none of its own: two widget claims on one slot
    would be a contradiction, not a refinement.
    """
    occupied = {}
    for selection in model["composition"].get("connector_population", []):
        if selection.get("occupancy") != "occupied":
            continue
        connector = selection["component"]
        occupant = selection["occupant"]
        # Everything fitted under the connector, not only the occupant's
        # own subtree: an Onyx2 rack's Xtown occupies the slot, and the
        # pipe whose XG answers on the slot's link hangs beside it.
        within = {connector}
        while True:
            children = {cid for cid, item in components.items()
                        if item.get("parent") in within}
            if children <= within:
                break
            within |= children
        buses = {item["bus"] for item in model["connections"]
                 if item["component"] == connector}
        attachments = [
            item for item in model["connections"]
            if item["bus"] in buses and item["component"] in within
            and _is_xio_occupant(components[item["component"]])]
        if len(attachments) != 1:
            raise CompileError(
                f"occupant '{occupant}' has no Bridge on connector '{connector}'"
                if not attachments else
                f"occupant '{occupant}' has more than one Bridge on connector "
                f"'{connector}'")
        attachment = attachments[0]
        for field in _OCCUPANT_PLACEMENT_FIELDS:
            if field in attachment:
                raise CompileError(
                    f"occupant Bridge '{attachment['component']}' authors its own "
                    f"'{field}'; it takes its placement from connector '{connector}'")
        occupied[connector] = attachment["component"]
    return occupied


def index_graph(model: dict) -> dict:
    """Build the typed graph index a configuration's compile reads.

    Returns node/router/crossbar/io_board positions, XIO attachments and
    slots, and fabric links -- every fact the record-level `_compile_*`
    functions below need, gathered once instead of per record.
    """
    components = _components_by_id(model)
    modules: dict[str, int] = {}
    nodes = _positioned(components, lambda item: item.get("kind") == "node-asic", modules)
    routers = _positioned(components, lambda item: item.get("kind") == "router-asic", modules)
    split_bridges = _split_bridge_parents(model, components)
    io_board_components = {cid: item for cid, item in components.items()
                            if cid not in split_bridges}
    io_boards = _positioned(
        io_board_components,
        lambda item: _is_xio_occupant(item)
        and not (item.get("hardware_kind") == "odyssey"
                 and _inside_vbrick(components, item)),
        modules,
    )
    for component in sorted(components.values(), key=lambda item: item["id"]):
        if component.get("kind") == "module":
            modules.setdefault(component["id"], len(modules))
    crossbars = sorted(
        (item for item in components.values() if item.get("kind") == "crossbar"),
        key=lambda item: item["id"])
    crossbar_index = {item["id"]: index for index, item in enumerate(crossbars)}
    node_index = {item["id"]: index for index, item in enumerate(nodes)}
    # A XIO attachment's `controlled_by` names the controlling
    # node's *board*, not its Hub component, so a lookup by board id
    # reaches the same node index.
    node_index_by_board = {item["board_id"]: index for index, item in enumerate(nodes)}
    router_index = {item["id"]: index for index, item in enumerate(routers)}
    io_index = {item["id"]: index for index, item in enumerate(io_boards)}
    # An occupied connector's attachment places the Bridge of the board
    # fitted in it; that Bridge's own attachment carries no placement and
    # is skipped below.
    occupied = _occupied_connectors(model, components)
    occupant_bridges = set(occupied.values())
    # A board sharing an occupied connector's link with that connector's
    # widget, answering no widget itself: the Onyx2 rack's Xtown, which
    # extends the link over a cable to the pipe.
    link_extenders = {
        item["component"] for connector in occupied
        for bus in {c["bus"] for c in model["connections"] if c["component"] == connector}
        for item in _connections_for_bus(model, bus)
        if item["component"] != connector
        and components.get(item["component"], {}).get("kind") == "board"
        and _is_within(components, components[item["component"]], connector)}
    # A widget whose own board names no slot (the Onyx2 rack's pipe, in
    # its graphics module) sits at the slot of the board occupying its
    # connector (the Xtown).
    occupant_slots = {}
    for selection in model["composition"].get("connector_population", []):
        declared = components.get(selection.get("occupant"), {}).get("physical_slot")
        if selection["component"] in occupied and isinstance(declared, dict):
            occupant_slots[occupied[selection["component"]]] = declared["number"]
    for entry in io_boards:
        board = _board_ancestor(components, entry["component"])
        if (entry["id"] in occupant_slots and
                not isinstance((board or {}).get("physical_slot"), dict)):
            entry["slot"] = occupant_slots[entry["id"]]
    # The IRIX slot name of the XIO position a board sits in (io1, io5):
    # its connector's location when fitted into one, else its own board's
    # numbered io slot. Onboard I/O has neither.
    connector_of = {widget: connector for connector, widget in occupied.items()}
    for entry in io_boards:
        board = _board_ancestor(components, entry["component"])
        declared = (board or {}).get("physical_slot")
        if entry["id"] in connector_of:
            entry["label"] = _slot_label(components[connector_of[entry["id"]]])
        elif isinstance(declared, dict) and declared.get("kind") == "io":
            entry["label"] = f"io{declared['number']}"
        else:
            entry["label"] = None
    board_order: list[str] = []

    # Every SGIModelXioAttachment names which node hosts one occupied
    # widget: an I/O board's Bridge, whether reached through a crossbar
    # (crossbar_index >= 0, node_index from the attachment's own
    # `controlled_by`) or wired directly to the node's own Hub
    # (crossbar_index -1, node_index the attaching node, decode facts from
    # the Bridge's own `XIO` connection detail); or a node's own Hub
    # entering its midplane Crossbow at a widget number (crossbar_index
    # >= 0, node_index the attaching node itself) -- the module's own
    # host port on the crossbar it is wired to, per the reviewed YAML's
    # XIO buses.
    xio_attachments = []
    xio_slots = []
    seen_widgets: set[tuple[int, int]] = set()
    for bus in model["buses"]:
        if bus["kind"] != "xio":
            continue
        master = bus.get("master")
        master_component = components.get(master)
        if master_component is not None and master_component.get("hardware_kind") == "bedrock":
            # The IO-8 heads are handled by the Bedrock compiler. An
            # occupied rear connector still places an external board on
            # the node XBridge, with its widget number on the connector.
            for attachment in _connections_for_bus(model, bus["id"]):
                connector = attachment["component"]
                board = occupied.get(connector)
                if board is None:
                    continue
                widget = _integer(
                    attachment, "widget", f"XIO connector '{connector}'", 8)
                if board not in io_index:
                    raise CompileError(
                        f"XIO connector '{connector}' has no board occupant")
                io_boards[io_index[board]]["crossbar_index"] = -1
                io_boards[io_index[board]]["widget"] = widget
                board_order.append(board)
                xio_attachments.append({
                    "crossbar_index": -1, "widget": widget,
                    "node_index": node_index[master],
                })
            continue
        if (master_component is not None
                and master_component.get("hardware_kind") == "xbridge"
                and _inside_vbrick(components, master_component)):
            # The V-brick preset owns its internal XIO ports and V12.
            continue
        if master_component is not None and master_component.get("kind") == "board":
            # A passive cable adapter's own links (the GIGAchannel's
            # Crosstown boards and the XIO cables between them) are
            # physical cabling: no widget answers on them, and the
            # Crossbow's own link buses carry each widget they reach.
            continue
        crossbar = crossbar_index.get(master)
        master_node = node_index.get(master)
        for attachment in _connections_for_bus(model, bus["id"]):
            cid = attachment["component"]
            if cid == master or cid in occupant_bridges or cid in link_extenders:
                continue
            component = components.get(cid)
            if component is None:
                raise CompileError(f"XIO attachment '{cid}' names no component")
            if crossbar is None and component.get("kind") == "board":
                # A Hub cabled to a passive adapter rather than a widget:
                # the Crosstown board that replaces the XTalk jumper.
                continue
            if crossbar is None:
                # Direct attachment: the master is the node's own Hub, and
                # the one endpoint (a Bridge) is the widget this attachment
                # describes, decoded from its own `XIO` detail.
                if master_node is None:
                    raise CompileError(
                        f"XIO bus '{bus['id']}' has neither a Crossbow "
                        "master nor a node master"
                    )
                detail = attachment.get("xio")
                if not isinstance(detail, dict):
                    raise CompileError(
                        f"direct XIO attachment '{cid}' on bus '{bus['id']}' "
                        "has no 'xio' placement detail"
                    )
                widget = _integer(detail, "enumerated_widget", f"component '{cid}'", 8)
                discovery = _integer(detail, "discovery_widget", f"component '{cid}'", 8)
                decoded = detail.get("decoded_widgets")
                if not isinstance(decoded, list) or not decoded:
                    raise CompileError(
                        f"direct XIO attachment '{cid}' has no decoded_widgets"
                    )
                mask = 0
                for value in decoded:
                    _validate_integer(value, 8, f"component '{cid}' decoded widget")
                    mask |= 1 << value
                xio_attachments.append({
                    "crossbar_index": -1, "widget": widget, "node_index": master_node,
                    "decoded_widget_mask": mask, "discovery_widget": discovery,
                })
                if cid in io_index:
                    io_boards[io_index[cid]]["crossbar_index"] = -1
                    io_boards[io_index[cid]]["widget"] = widget
                    board_order.append(cid)
                continue
            if "widget" not in attachment:
                if cid in occupied:
                    raise CompileError(
                        f"occupied connector '{cid}' has no widget number")
                if component.get("kind") == "connector":
                    # An unpopulated position whose widget number is
                    # itself unresolved (an open question in the
                    # resolved model, not merely an empty slot with a
                    # known number) -- nothing format 2 can name yet.
                    continue
                raise CompileError(
                    f"XIO attachment '{cid}' has no widget number"
                )
            widget = _integer(attachment, "widget", f"XIO attachment '{cid}'", 8)
            key = (crossbar, widget)
            if key in seen_widgets:
                raise CompileError(f"Crossbow widget {widget} is attached twice")
            seen_widgets.add(key)
            board = occupied.get(cid, cid)
            if cid in node_index:
                # The node's own Hub entering this crossbar: a host
                # attachment naming the node itself, not a controlling
                # node.
                xio_attachments.append({
                    "crossbar_index": crossbar, "widget": widget,
                    "node_index": node_index[cid],
                    "decoded_widget_mask": 0, "discovery_widget": 0,
                })
            elif board in io_index:
                controller = attachment.get("controlled_by")
                controller_index = node_index_by_board.get(controller)
                if controller_index is None:
                    raise CompileError(
                        f"I/O board '{board}' names no known controlling node"
                    )
                io_boards[io_index[board]]["crossbar_index"] = crossbar
                io_boards[io_index[board]]["widget"] = widget
                board_order.append(board)
                xio_attachments.append({
                    "crossbar_index": crossbar, "widget": widget,
                    "node_index": controller_index,
                    "decoded_widget_mask": 0, "discovery_widget": 0,
                })
            elif component.get("kind") == "connector":
                controller = attachment.get("controlled_by")
                controller_index = node_index_by_board.get(controller)
                if controller_index is None:
                    raise CompileError(
                        f"XIO slot '{cid}' names no known controlling node"
                    )
                xio_slots.append({
                    "crossbar_index": crossbar, "widget": widget,
                    "controlling_node": controller_index,
                    "id": cid, "label": _slot_label(component),
                })
            else:
                raise CompileError(
                    f"XIO attachment '{cid}' is neither an I/O board, "
                    "a node's own host port nor an unpopulated slot"
                )
    for board in io_boards:
        if "crossbar_index" not in board:
            raise CompileError(f"I/O board '{board['id']}' has no XIO attachment")
    # QEMU pairs board-owning attachments with io_boards by order
    # (sgi_machine_index_xio_attachments), so the two must agree.
    if board_order != [board["id"] for board in io_boards]:
        raise CompileError(
            f"I/O boards {[board['id'] for board in io_boards]} are not in "
            f"XIO attachment order {board_order}")

    links = _fabric_links(model, components, node_index, router_index)

    module_order = sorted(modules.items(), key=lambda item: item[1])
    return {
        "components": components,
        "nodes": nodes, "routers": routers, "crossbars": crossbars,
        "io_boards": io_boards, "xio_attachments": xio_attachments,
        "xio_slots": xio_slots, "links": links,
        "modules": [{"id": module_id, "index": index} for module_id, index in module_order],
    }


# ---------------------------------------------------------------------------
# Record extraction
# ---------------------------------------------------------------------------

def _synthetic_serial(model: dict, component_id: str, kind: str, node_index: int) -> int:
    """A component's emulator serial, from either serial mechanism in use.

    The Origin 200 family names one base and a per-node stride per kind
    (`machine.emulation.synthetic_serials`); the Origin 2000 family names
    one explicit value per component (`machine.emulation.synthetic_identities`).
    Both are emulator-assigned identities, never hardware evidence -- see
    each field's own `reason`/provenance in the resolved model.
    """
    emulation = model["machine"].get("emulation", {})
    explicit = emulation.get("synthetic_identities")
    if isinstance(explicit, list):
        for entry in explicit:
            if entry.get("component") == component_id:
                return num(entry["serial"])
    bases = emulation.get("synthetic_serials")
    if isinstance(bases, dict) and kind in bases:
        entry = bases[kind]
        return num(entry["base"]) + node_index * _integer(
            entry, "per_node_stride", f"synthetic_serials.{kind}")
    raise CompileError(f"component '{component_id}' has no synthetic serial")


_FLASH_KINDS = {"am29f080": "AM29F080"}


def _flash(model: dict, components: dict[str, dict], asic_kind: str, board_id: str) -> dict:
    """A node's boot-flash kind and total chip size. Which named views the
    flash controller answers, and their addresses, are fixed by the node
    ASIC (`SGIModelFlash`'s own QAPI comment) and are not carried here; the
    node device (`sn0.c` for HUB, `sn1-node.c` for BEDROCK) names them
    itself.

    A Hub finds its flash as a `kind: flash` component on its own board and
    sizes the chip from that component's "reset" view. Origin 300 authors
    no such component at all: its PROM is one operator-supplied payload
    (`machine.emulation.firmware_payload`), stated compatibility policy
    rather than a physical decoder claim (the "physical-reset-decoder" open
    question in the model-input directory/layers/modules/origin300-base.yaml). AM29F080
    is the one kind format 2 carries either way; naming it for a Bedrock
    node is the same stated emulator-compatibility choice this project's
    open questions already use for an unidentified physical part elsewhere,
    never a claim about the real IP45 chip. Flagged for independent review.
    """
    if asic_kind == "bedrock":
        firmware = model["machine"].get("emulation", {}).get("firmware_payload")
        if not isinstance(firmware, dict):
            raise CompileError(
                "machine.emulation.firmware_payload is absent; a Bedrock "
                "node has no other boot flash fact"
            )
        size = num(_required(firmware, "size", "firmware_payload"))
        return {"kind": "AM29F080", "size": size}
    flash_id = _one_flash(components, board_id)
    flash = components[flash_id]
    # SGIModelFlashKind names exactly one part today; a product that leaves
    # its boot flash's `hardware_kind` unauthored (the Origin 200 family
    # does) still gets that one supported kind, since there is no other
    # value the type could mean. A product that authors a different part is
    # refused rather than silently coerced to it.
    hardware_kind = flash.get("hardware_kind", "am29f080")
    if hardware_kind not in _FLASH_KINDS:
        raise CompileError(f"flash '{flash_id}' names unsupported part '{hardware_kind}'")
    views = flash.get("views") or []
    reset_view = next((view for view in views if view.get("id") == "reset"), None)
    if reset_view is None:
        raise CompileError(f"flash '{flash_id}' has no reset view to size the chip")
    size = num(_required(reset_view, "size", f"flash '{flash_id}' view 'reset'"))
    return {"kind": _FLASH_KINDS[hardware_kind], "size": size}


def _processor(model: dict, cpu: dict) -> dict:
    """One catalogued processor assembly, keyed by part and selected SKU.

    prid/fpu_prid are identity registers a product authors directly on its
    populated CPU component today (the plan's vocabulary section moves
    them into the processors catalogue; that move has not landed for
    R10000). A product whose CPU component omits prid is refused by name
    rather than filled from another product's chip of the same part: the
    same part number does not guarantee the same revision's PRID, and this
    compiler does not read across configurations to find out.

    fpu_prid is different for R14000: no held source gives Origin 300's
    FPU identification register (the resolved model carries no
    `fpu_identification` for either r14000 variant). Per the packet's
    instruction, zero is emitted as the default when the register is
    absent; the QEMU header does not yet state this rule, but the
    builder step will adopt one when it lands. R10000 still requires
    the field explicitly authored, since Origin 200 and Origin 2000 both
    carry it.
    """
    part = _required(cpu, "hardware_kind", cpu["id"])
    identity = cpu.get("identity") or {}
    if part == "r10000":
        clock_mhz = _integer(identity, "clock_mhz", f"CPU '{cpu['id']}'", 32)
        cache_mib = _integer(identity, "cache_mib", f"CPU '{cpu['id']}'", 16)
        cache_clock_mhz = _integer(identity, "cache_clock_mhz", f"CPU '{cpu['id']}'", 32)
        for field in ("processor_identification", "fpu_identification"):
            if field not in identity:
                raise CompileError(
                    f"CPU '{cpu['id']}' authors no {field}; format 2 needs an "
                    "identity register this catalogue does not yet carry for "
                    "this product (the vocabulary step's processors catalogue "
                    "is the planned home for it)"
                )
        return {
            "id": f"r10000-{clock_mhz}mhz",
            "kind": "R10000",
            "prid": num(identity["processor_identification"]),
            "fpu_prid": num(identity["fpu_identification"]),
            "clock_hz": clock_mhz * 1_000_000,
            "cache_clock_hz": cache_clock_mhz * 1_000_000,
            "cache_mib": cache_mib,
            # mode_bits is filled in by the caller, once per configuration:
            # the reset-mode word is a property of the machine's firmware
            # configuration, not of one processor's identity.
        }
    if part == "r14000":
        clock_mhz = _integer(identity, "clock_mhz", f"CPU '{cpu['id']}'", 32)
        cache_mib = _integer(identity, "cache_mib", f"CPU '{cpu['id']}'", 16)
        cache_clock_mhz = _integer(identity, "cache_clock_mhz", f"CPU '{cpu['id']}'", 32)
        if "processor_identification" not in identity:
            raise CompileError(
                f"CPU '{cpu['id']}' authors no processor_identification; "
                "format 2 needs this identity register"
            )
        return {
            "id": f"r14000-{clock_mhz}mhz",
            "kind": "R14000",
            "prid": num(identity["processor_identification"]),
            "fpu_prid": num(identity.get("fpu_identification", 0)),
            "clock_hz": clock_mhz * 1_000_000,
            "cache_clock_hz": cache_clock_mhz * 1_000_000,
            "cache_mib": cache_mib,
            # mode_bits filled in by the caller, as above.
        }
    raise CompileError(
        f"CPU '{cpu['id']}' names unsupported part '{part}' "
        "(only r10000 and r14000 have a reset-mode encoding today)"
    )


def _mode_bits(model: dict, node: dict, processor: dict) -> int:
    """The reset-mode word every populated CPU in this configuration shares.

    Dispatches on the *processor's* catalogued family, not the node ASIC:
    an R10000 word is assembled field by field, since Config0's write mask
    is established for it; the R14000 write mask is not established, so the
    processor catalogue carries a literal reset word for the selected SKU.
    """
    if processor["kind"] == "R14000":
        return _r14000_mode_bits(
            model, processor["clock_hz"] // 1_000_000, processor["cache_mib"])
    if processor["kind"] == "R10000":
        return _r10000_mode_bits(model, node, processor["clock_hz"])
    raise CompileError(
        f"processor kind '{processor['kind']}' has no reset-mode encoding"
    )


def _r10000_mode_bits(model: dict, hub: dict, cpu_clock_hz: int) -> int:
    """The R10000 reset-mode word, assembled field by field.

    `firmware_config.cpu_clock_hz`/`hub_clock_hz` author both clocks
    directly on a routed (Origin 2000 family) composition; a flat (Origin
    200 family) one instead carries the CPU clock on the catalogued
    processor and the Hub clock on the Hub's own identity, so this falls
    back to the Hub's `identity.clock_hz` when firmware configuration
    omits it.
    """
    firmware = model["machine"].get("firmware_config")
    if not isinstance(firmware, dict):
        raise CompileError(
            "machine.firmware_config is absent; this compiler has no "
            "field-by-field reset-mode encoding for an R10000 "
            "configuration without one"
        )
    hub_clock_hz = firmware.get("hub_clock_hz")
    if hub_clock_hz is None:
        hub_clock_hz = (hub.get("identity") or {}).get("clock_hz")
    if not isinstance(hub_clock_hz, int):
        raise CompileError(
            f"neither firmware_config.hub_clock_hz nor node '{hub['id']}' "
            "identity.clock_hz names the Hub clock"
        )
    return _compile_r10000_reset_mode(firmware, cpu_clock_hz, hub_clock_hz)


def _r14000_mode_bits(model: dict, clock_mhz: int, cache_mib: int) -> int:
    """The R14000 reset-mode word for the selected clock/cache SKU, read as
    the one literal the processors catalogue carries
    (`layers/architectures/sn1.yaml`, `reset_mode_bits`). A SKU the
    catalogue does not carry a word for (the 600 MHz variant today) is
    refused by name rather than derived from the 500 MHz word.
    """
    for entry in model.get("processors", []):
        if entry.get("part") != "r14000":
            continue
        for variant in entry.get("variants", []):
            if (variant.get("clock_mhz") == clock_mhz
                    and variant.get("cache_mib") == cache_mib):
                if "reset_mode_bits" not in variant:
                    raise CompileError(
                        f"the r14000 {clock_mhz} MHz/{cache_mib} MiB variant "
                        "carries no reset_mode_bits in the processors catalogue"
                    )
                return num(variant["reset_mode_bits"])
    raise CompileError(
        f"no catalogued r14000 variant matches {clock_mhz} MHz/{cache_mib} MiB"
    )


def _cpu_slots(cpus: list[dict], node_component: dict,
               node_asic: dict, processor: dict, max_population: int) -> list[dict]:
    """Every processor-interface slot the node's Hub exposes.

    Sized from `node_asic` (the ASIC's own interface and per-interface
    capacity, a hardware constant unaffected by how many CPUs this
    particular configuration ships), and filled with the same catalogued
    processor object up to the largest population this configuration's
    topology accepts for the node -- the wiring accepts any count up to
    that maximum of the one catalogued part, and `-smp` selects how many
    are actually realized. A slot beyond the accepted maximum is left
    empty (`assembly=None`), matching the header's "NULL assembly is
    empty". `cpus` is this node's own fitted processors (`_node_cpus`),
    whose authored slices place them.
    """
    interfaces = _integer(node_asic, "processor_interfaces", "node_asic", 8)
    per_interface = _integer(node_asic, "cpus_per_interface", "node_asic", 8)
    total = interfaces * per_interface
    if max_population > total:
        raise CompileError(
            f"node '{node_component['id']}' accepts {max_population} CPUs but "
            f"its node_asic exposes only {total} interface slots"
        )
    authored_slices = [cpu.get("processor_slice") for cpu in cpus]
    fitted_positions = None
    if any(value is not None for value in authored_slices):
        if any(not isinstance(value, str) or value not in "ABCD"
               for value in authored_slices):
            raise CompileError("every fitted processor needs a physical slice A through D")
        fitted_positions = {"ABCD".index(value) for value in authored_slices}
        if len(fitted_positions) != len(authored_slices):
            raise CompileError("fitted processors must occupy distinct physical slices")
        if len(fitted_positions) != max_population:
            raise CompileError(
                f"node '{node_component['id']}' authors {len(fitted_positions)} "
                f"fitted processor slices but accepts {max_population} CPUs"
            )

    slots = []
    filled = 0
    for interface in range(interfaces):
        for slot in range(per_interface):
            position = interface * per_interface + slot
            filled += 1
            slots.append({
                "interface": interface, "slot": slot,
                "assembly": processor if (
                    position in fitted_positions if fitted_positions is not None
                    else filled <= max_population
                ) else None,
            })
    return slots


def _accepted_memory_mib(model: dict) -> list[int]:
    constraints = model["machine"].get("emulation", {}).get("emulator_constraints")
    if not isinstance(constraints, dict) or "accepted_bank_sizes_mib" not in constraints:
        return []
    sizes = constraints["accepted_bank_sizes_mib"]
    for value in sizes:
        _validate_integer(value, 64, "accepted_bank_sizes_mib entry")
    return list(sizes)


def _node_max_population(model: dict, node_position: int, node_count: int) -> int:
    """The largest CPU count any accepted population fits at this node.

    Every accepted population in `composition.topology.supported_cpu_populations`
    (or, for a routed composition, `machine.emulation`'s equivalent) names a
    per-node CPU count list; format 2 wires each node for the largest one
    names for it, since a smaller `-smp` selection only leaves slots idle.
    """
    populations = (model["composition"].get("topology") or {}).get(
        "supported_cpu_populations")
    if not populations:
        # No accepted-population menu: the shipped population is the only
        # one, so the wiring matches exactly what is installed.
        shipped = model["composition"]["population"]["cpus_per_node"]
        if node_position >= len(shipped):
            raise CompileError("population.cpus_per_node does not cover every node")
        return shipped[node_position]
    best = 0
    for population in populations:
        if len(population) != node_count:
            raise CompileError(
                "a supported_cpu_populations entry does not cover every node"
            )
        best = max(best, population[node_position])
    return best


def _supported_cpu_populations(model: dict, node_count: int) -> list[list[int]]:
    populations = (model["composition"].get("topology") or {}).get(
        "supported_cpu_populations")
    if not populations:
        return [list(model["composition"]["population"]["cpus_per_node"])]
    result = []
    for population in populations:
        if len(population) != node_count:
            raise CompileError(
                "a supported_cpu_populations entry does not cover every node"
            )
        result.append(list(population))
    return result


def _node_i2c_adapter(model: dict, components: dict[str, dict], hub: dict) -> tuple[str, dict] | None:
    """The i2c bus this node's own I2C adapter masters, found by register
    placement rather than by name or `hardware_kind`: the adapter's own
    data-register region names the Hub it is wired into (`relative_to`),
    which both a private per-node adapter (the Origin 200's own PCF8584)
    and one of several adapters sharing a module-wide bus (the Origin
    2000's per-Hub adapters on `module-management-i2c`) author alike.
    """
    for bus in model["buses"]:
        if bus["kind"] != "i2c":
            continue
        for master_id in ([bus["master"]] if "master" in bus else bus.get("masters", [])):
            adapter = components.get(master_id)
            if adapter is None:
                continue
            data = _region(adapter, "data-register")
            if data is not None and data.get("relative_to") == hub["id"]:
                return bus["id"], adapter
    return None


# Which identity block field a node ASIC's two XIO widget revision
# registers come from. Hub components author the two revision fields
# independently. Origin 300 has no established equivalent for the first
# field, so a Bedrock node reuses `identity.hardware_revision` for both
# rather than inventing a second value. That is an approximation, not a
# hardware claim.
_ASIC_IDENTITY_REVISION_FIELDS = {
    "hub": ("ni_revision", "widget_revision"),
    "bedrock": ("hardware_revision", "hardware_revision"),
}


def _board_identity_part(model: dict, board_ancestor: dict) -> str | None:
    """A board's one-wire `identity.part`, the board component's own
    authored value unless this product's `machine.emulation.
    board_identity_overrides` names this board explicitly.

    A product that fits a reusable module's own board component (whose
    `identity.part` it cannot edit in place) states the fitted board's real
    part here instead, the way `the model-input directory/layers/products/
    onyx2-rack.yaml` does for the BaseIO-G it fits under the shared
    `origin2000-rack.yaml` layer's own `baseio` component.
    """
    default = (board_ancestor.get("identity") or {}).get("part")
    emulation = model["machine"].get("emulation", {})
    override = next(
        (item for item in emulation.get("board_identity_overrides", [])
         if item.get("component") == board_ancestor["id"]), None)
    return default if override is None else override["part"]


def _hub_ni_revision(model: dict, node: dict, identity: dict) -> int:
    """A Hub's `NI_STATUS_REV_ID`, the reusable node-board module's own
    authored value unless this product's `machine.emulation.
    hub_ni_revision_overrides` names this Hub explicitly.

    `the model-input directory/layers/modules/ip27-node-board.yaml` authors the general
    baseline (Hub 2.1, code 3, the earliest revision IRIX 6.5.30's generic
    inventory routine accepts) once, for every product that reuses it. A
    product whose fielded configuration needs a different revision --
    Origin 2000's graphics-capable four-node module, where the master node
    requires at least code 4 -- states that
    explicitly here instead of forking the reusable module file. This does
    not touch the separate `widget_revision` field.
    """
    default = _integer(identity, "ni_revision", f"node '{node['id']}'", 8)
    emulation = model["machine"].get("emulation", {})
    override = next(
        (item for item in emulation.get("hub_ni_revision_overrides", [])
         if item.get("component") == node["id"]), None)
    return default if override is None else override["ni_revision"]


def _node_identity(model: dict, components: dict[str, dict], node: dict,
                    asic_kind: str, node_index: int) -> dict:
    """A node ASIC's `SGIModelAsicIdentity`: its own fitted-part revisions
    and serial, and whether it carries an embedded I2C controller that also
    answers as the node's system controller (the Origin 200 family's own
    embedded ELSC). A Bedrock node's controller role belongs to its
    module's or brick's L1 instead (`SGIModelController`), never to the
    node itself, so both fields come back false: `_node_i2c_adapter` finds
    no i2c bus wired to a Bedrock's own data-register offset, since Origin
    300 authors none.
    """
    identity = node.get("identity") or {}
    ni_field, widget_field = _ASIC_IDENTITY_REVISION_FIELDS[asic_kind]
    ni_revision = (_hub_ni_revision(model, node, identity) if asic_kind == "hub"
                   else _integer(identity, ni_field, f"node '{node['id']}'", 8))
    widget_revision = _integer(identity, widget_field, f"node '{node['id']}'", 8)
    serial = _synthetic_serial(model, node["id"], asic_kind, node_index)

    found = _node_i2c_adapter(model, components, node)
    system_controller = False
    if found is not None:
        bus_id, _adapter = found
        # Only an endpoint the adapter itself addresses as a slave is this
        # node's own system controller; a shared module controller that is
        # a co-master of the same bus (the Origin 2000's MSC) belongs to
        # `controllers[]` instead, not to any one node's identity.
        for connection in _connections_for_bus(model, bus_id):
            if connection.get("role") != "endpoint":
                continue
            component = components.get(connection["component"])
            if component is not None and component.get("kind") == "system-controller":
                system_controller = True
                break
    return {
        "ni_revision": ni_revision, "widget_revision": widget_revision, "serial": serial,
        "i2c_controller": found is not None, "system_controller": system_controller,
    }


def _region(component: dict, region_id: str) -> dict | None:
    for region in component.get("regions") or component.get("views") or []:
        if region.get("id") == region_id:
            return region
    return None


# ---------------------------------------------------------------------------
# Configuration assembly
# ---------------------------------------------------------------------------

def _node_bank_mib(model: dict, position: int, node_count: int) -> list[int]:
    population = model["composition"]["population"]
    banks = population.get("memory_bank_mib_per_node")
    if not isinstance(banks, list) or len(banks) != node_count:
        raise CompileError("population.memory_bank_mib_per_node does not cover every node")
    row = list(banks[position])
    if len(row) > MAX_MEMORY_BANKS:
        raise CompileError(f"node {position} names more than {MAX_MEMORY_BANKS} memory banks")
    return row + [0] * (MAX_MEMORY_BANKS - len(row))


def _junk_uart_node(components: dict[str, dict], nodes: list[dict]) -> int:
    """The node index offering the manufacturing junk-bus UART, or -1.

    Found structurally: a `subfunction` marked `optional` directly under a
    node's own Hub, the same shape `machine_model._compile_junk_uart`
    already reads. Every configuration this compiler currently reaches
    ships at most one.
    """
    for index, node in enumerate(nodes):
        for component in components.values():
            if (component.get("kind") == "subfunction"
                    and component.get("parent") == node["id"]
                    and isinstance(component.get("optional"), dict)
                    and "junk" in component["id"]):
                return index
    return -1


def _is_within(components: dict[str, dict], component: dict, scope_id: str) -> bool:
    node = component
    seen = set()
    while True:
        if node["id"] == scope_id:
            return True
        parent_id = node.get("parent")
        if parent_id is None or node["id"] in seen:
            return False
        seen.add(node["id"])
        node = components[parent_id]


def _one_flash(components: dict[str, dict], scope_id: str) -> str:
    candidates = [item for item in components.values()
                  if item.get("kind") == "flash" and _is_within(components, item, scope_id)
                  and any(view.get("id") == "reset"
                          for view in (item.get("views") or item.get("regions") or []))]
    if len(candidates) != 1:
        raise CompileError(f"node scope '{scope_id}' does not name exactly one boot flash")
    return candidates[0]["id"]


_ASIC_RECORD_KIND = {"hub": "HUB", "bedrock": "BEDROCK"}


def _node_cpus(model: dict, components: dict[str, dict], node: dict,
               asic_kind: str, board_id: str) -> list[dict]:
    """The populated `cpu` components this node's Hub or Bedrock carries.

    A Hub-family board parents its CPUs under the node's own board or
    carrier, so the usual `_is_within` scope walk finds them. Origin 300
    ships no such component to parent them under: its CPUs sit beside the
    Bedrock, at the top level of a one-chassis model or directly under
    each chassis's module component on a two-chassis one. A Bedrock's
    processors are instead the CPUs that master one of the SysAD buses it
    terminates, which is the processor-interface wiring itself.
    """
    if asic_kind == "bedrock":
        sysad = {item["bus"] for item in model["connections"]
                 if item["component"] == node["id"] and item.get("role") == "endpoint"
                 and any(bus["id"] == item["bus"] and bus["kind"] == "sysad"
                         for bus in model["buses"])}
        cpu_ids = {item["component"] for item in model["connections"]
                   if item["bus"] in sysad and item.get("role") == "master"}
        return [item for item in components.values()
                if item.get("kind") == "cpu" and item["id"] in cpu_ids]
    return [item for item in components.values()
            if item.get("kind") == "cpu" and _is_within(components, item, board_id)]


def _compile_node(model: dict, components: dict[str, dict], entry: dict,
                   position: int, node_count: int) -> dict:
    node = entry["component"]
    node_asic = node.get("node_asic")
    if not isinstance(node_asic, dict):
        raise CompileError(f"node '{node['id']}' has no node_asic block")
    # Only a Bedrock node authors hardware_kind at all; a Hub-family
    # product leaves it unauthored, so "not bedrock" is "hub", the same
    # dispatch the pre-unification pair used.
    asic_kind = "bedrock" if node.get("hardware_kind") == "bedrock" else "hub"

    cpus = _node_cpus(model, components, node, asic_kind, entry["board_id"])
    if not cpus:
        raise CompileError(f"node '{node['id']}' has no populated CPU component")
    processor = _processor(model, cpus[0])
    for other in cpus[1:]:
        if _processor(model, other)["id"] != processor["id"]:
            raise CompileError(
                f"node '{node['id']}' mixes CPU assemblies; format 2 needs "
                "one catalogued part per node"
            )
    processor["mode_bits"] = _mode_bits(model, node, processor)
    max_population = _node_max_population(model, position, node_count)
    identity = _node_identity(model, components, node, asic_kind, position)
    return {
        "id": node["id"],
        "module_index": entry["module_index"],
        "slot": entry["slot"],
        # What the node slot's midplane drives on the Hub's slot-identity
        # pins, where that differs from the eight-processor module's own
        # wiring (the Onyx2 deskside); None elsewhere, which QEMU reads as
        # that default wiring.
        "hub_slot_bits": entry["hub_slot_bits"],
        "asic_kind": _ASIC_RECORD_KIND[asic_kind],
        "identity": identity,
        "cpu_slots": _cpu_slots(cpus, node, node_asic, processor, max_population),
        "bank_mib": _node_bank_mib(model, position, node_count),
        "accepted_memory_mib": _accepted_memory_mib(model),
        "flash": _flash(model, components, asic_kind, entry["board_id"]),
        "slave_role": position != 0,
    }


def _one_wire_identities(model: dict, components: dict[str, dict],
                          master_id: str) -> list[str]:
    """Every `identity.part` an `identity`-kind component answers with on the
    one-wire bus `master_id` masters, in the bus's own connection order.

    A one-wire chain can enumerate more than one board's record: the
    InfiniteReality pipe's chain answers for its geometry board and, on
    the Onyx2 rack, for the Ktown board in the graphics module the pipe
    is cabled through. Neither board carries an `identity` of its own on
    its board component, so the endpoint record is the only place either
    part number is established, and both belong to the widget's record.
    A record whose own board instead already declares its own
    `identity.part` directly (the GIGAchannel's Crosstown board) names
    that separately identified board, not a fact about the widget's; its
    own declared identity is what excludes it, not its bus topology.
    """
    bus = next((item["id"] for item in model["buses"]
                if item["kind"] == "one-wire" and item.get("master") == master_id), None)
    if bus is None:
        return []
    parts = []
    for connection in _connections_for_bus(model, bus):
        if connection.get("role") != "endpoint":
            continue
        component = components.get(connection["component"])
        if component is None or component.get("kind") != "identity":
            continue
        ancestor = _board_ancestor(components, component)
        if ancestor is not None and (ancestor.get("identity") or {}).get("part") is not None:
            # A fitted daughter board's own record on the same line
            # (the GIGAchannel's Crosstown board beside the IP29's)
            # names that board, not this one's.
            continue
        part = (component.get("identity") or {}).get("part")
        if part is not None:
            parts.append(part)
    return parts


_HUB_PCI_KINDS = {"isp1040": "ISP1040", "ioc3": "IOC3"}
_BEDROCK_PCI_KINDS = {"isp12160": "ISP12160", "ioc3": "IOC3", "lucent-ohci": "LUCENT_OHCI"}


def _ioc3_has_ethernet(components: dict[str, dict], ioc3_id: str) -> bool:
    """True when this IOC3 fits an Ethernet MAC, read from the contract as
    a directly-parented ethernet subfunction. Two contract dialects name
    it: the SN family types the subfunction with `hardware_kind:
    ioc3-ethernet` (Origin 2000's `baseio-ethernet`, Origin 300's
    `ioc3-ethernet`), while the Origin 200 family types PCI parts by
    component id and names the MAC subfunction `ioc3-eth` (a module
    instance prefixes that id on origin200-dual, so the part token is read
    as any hyphen-delimited segment, matching `_board_pci_population`). A
    BaseIO-G MediaIO daughtercard's second IOC3 carries serial, keyboard,
    mouse and parallel but no Ethernet, so it authors neither form and
    contributes no network offering.
    """
    for component in components.values():
        if component.get("parent") != ioc3_id:
            continue
        if component.get("hardware_kind") == "ioc3-ethernet":
            return True
        if (component.get("hardware_kind") is None
                and "eth" in component["id"].split("-")):
            return True
    return False


def _ioc3_has_keyboard_mouse(components: dict[str, dict], ioc3_id: str) -> bool:
    """True when this IOC3 fits a PS/2 keyboard and mouse, read from the
    contract as a directly-parented keyboard-mouse subfunction. Only the
    BaseIO-G MediaIO daughtercard's own second IOC3 carries it; the plain
    BaseIO board's first IOC3 does not. Its presence selects the BaseIO-G
    board kind, whose `sgi-baseio` preset builds that second IOC3
    (the catalogue ownership rule), so the keyboard and mouse reach a guest.
    """
    return any(component.get("parent") == ioc3_id
               and component.get("hardware_kind") == "ioc3-keyboard-mouse"
               for component in components.values())


def _board_pci_population(model: dict, components: dict[str, dict], kinds: dict[str, str],
                           master_ids: set[str]) -> tuple[list[dict], int | None]:
    """This board's PCI function kinds, in device/function slot order, and
    the one-wire serial of its fitted IOC3 if it carries one.

    A function's own device number, revision, interrupt input, subsystem
    id and most IOC3 population booleans are `sgi-baseio`'s own preset
    facts (the catalogue ownership rule) and are not carried past this sort. The kind
    order survives, and for an IOC3 so does whether it fits an Ethernet
    MAC, since the catalogue's launcher-facing `offerings` need both to
    name this configuration's storage and network adapters
    (`model_emit._storage_for`/`_networks_for`): a board with two IOC3s
    where only one is wired for Ethernet must offer one network, not two.
    The IOC3's own fitted-part identity has no other home and survives as
    `ioc3-serial` on the board record itself.
    """
    pci_buses = {item["id"] for item in model["buses"]
                 if item["kind"] == "pci" and item.get("master") in master_ids}
    entries = []
    for connection in model["connections"]:
        if connection["bus"] not in pci_buses or connection["component"] in master_ids:
            continue
        component = components.get(connection["component"])
        if component is None or component.get("kind") == "connector":
            continue
        # hardware_kind is unauthored on the Origin 200 family's own PCI
        # parts (they are typed by component id there instead), and a
        # multi-node product (origin200-dual) prefixes that id with its
        # module instance ("node0-isp1040"), so the part name is read as
        # any hyphen-delimited segment of the id, not only its first.
        kind = component.get("hardware_kind") or next(
            (segment for segment in component["id"].split("-") if segment in kinds), None)
        if kind not in kinds:
            raise CompileError(f"PCI component '{component['id']}' is unsupported")
        device = _integer(connection, "pci_device", component["id"], 8)
        function = connection.get("pci_function", 0)
        entries.append((device, function, kinds[kind], component["id"]))
    entries.sort(key=lambda item: (item[0], item[1]))
    seen = set()
    for device, function, _kind, cid in entries:
        if (device, function) in seen:
            raise CompileError(f"PCI devfn repeated on component '{cid}'")
        seen.add((device, function))
    pci = []
    for _device, _function, kind, cid in entries:
        entry = {"kind": kind}
        if kind == "IOC3":
            entry["ethernet"] = _ioc3_has_ethernet(components, cid)
        pci.append(entry)
    ioc3_serial = next(
        (_synthetic_serial(model, cid, "ioc3", 0)
         for _device, _function, kind, cid in entries if kind == "IOC3"),
        None)
    return pci, ioc3_serial


def _compile_bedrock_io_boards(model: dict, components: dict[str, dict], bedrock_id: str,
                                node_index: int, module_index: int,
                                first_attachment: int = 0) -> tuple[list[dict], list[dict]]:
    """The Origin 300 IO-8 board's two `SGIModelIoBoard` entries (the
    XBridge's two independently PCI-hosting heads) and their
    `SGIModelXioAttachment` records -- found structurally from the
    Bedrock's own XIO bus rather than by authored id, so this reads
    any product whose Bedrock hosts one XBridge in XBridge mode, not only
    today's one origin300 configuration. A two-chassis product calls this
    once per Bedrock, each chassis carrying its own IO-8. A head's
    `slot_index` is its XIO attachment's position in the whole
    topology's list, which QEMU's builder indexes by, so the second
    chassis's heads start at `first_attachment`.

    Origin 300 has no separate Crossbow chip: the XBridge's crossbar
    section and both PCI bridge heads are numbered widgets directly on
    the Bedrock's own XIO bus (`index_graph` skips this bus for a
    Bedrock master; see its own comment). A head is a `subfunction` child
    of the `pci-host-bridge`-kind XBridge component, carrying its own
    widget and PCI bus but no identity of its own -- revision, serial and
    part all come from the parent, the one physical chip both heads share.
    The XBridge's own crossbar section is part of the IP45 node board, as
    the L1 and the boot flash are (the catalogue ownership rule's own corner case): the
    node device creates it directly, so it never gets a catalogue record
    of its own, and its two heads attach directly to the node rather than
    through a crossbar index.
    """
    xio_buses = [
        bus for bus in model["buses"] if bus["kind"] == "xio"
        and bus.get("master") == bedrock_id
    ]
    if len(xio_buses) != 1:
        raise CompileError(
            f"Bedrock '{bedrock_id}' needs exactly one XIO bus it masters")
    bus = xio_buses[0]
    attachments = _connections_for_bus(model, bus["id"])

    crossbar_present = False
    heads = []
    for attachment in attachments:
        component = components.get(attachment["component"])
        if component is None or attachment["component"] == bus.get("master"):
            continue
        if component.get("kind") == "crossbar":
            if crossbar_present:
                raise CompileError(f"XIO bus '{bus['id']}' hosts more than one crossbar")
            crossbar_present = True
        elif "widget" in attachment:
            parent = components.get(component.get("parent"))
            if parent is not None and parent.get("kind") == "pci-host-bridge":
                heads.append((component, parent, attachment))
    if not crossbar_present:
        raise CompileError(f"XIO bus '{bus['id']}' hosts no crossbar")
    if not heads:
        raise CompileError(f"XIO bus '{bus['id']}' hosts no PCI bridge head")
    heads.sort(key=lambda item: item[0]["id"])

    io_boards = []
    xio_attachments = []
    for slot, (head, parent, attachment) in enumerate(heads):
        widget = _integer(attachment, "widget", f"XIO attachment '{head['id']}'", 8)
        pci, ioc3_serial = _board_pci_population(
            model, components, _BEDROCK_PCI_KINDS, {head["id"]})
        parent_identity = parent.get("identity") or {}
        io_boards.append({
            "id": head["id"],
            "module_index": module_index,
            "board_kind": "IO8",
            "slot_index": first_attachment + slot,
            "revision": _integer(parent_identity, "widget_revision", parent["id"], 8),
            "serial": _synthetic_serial(model, parent["id"], "xbridge", 0),
            "part": _required(parent_identity, "part", parent["id"]),
            "other_identities": [],
            "pci": pci,
            "ioc3_serial": ioc3_serial,
        })
        xio_attachments.append({
            "crossbar_index": -1, "widget": widget, "node_index": node_index,
        })
    return io_boards, xio_attachments


# Crossbow-attached add-in boards that name their own design by the board
# component's `hardware_kind`, rather than being recognised as a BaseIO by
# the parts fitted to them.
_XIO_BOARD_KINDS = {
    "pci-shoehorn": "PCI_SHOEHORN",
    "pci-carrier": "PCI_CARRIER",
    "gigachannel-midplane": "GIGACHANNEL",
    "kona": "KONA",
    "menet": "MENET",
    "mgras": "MGRAS",
    "mscsi": "MSCSI",
    "odyssey": "ODYSSEY",
}


def _compile_io_board(model: dict, components: dict[str, dict], entry: dict) -> dict:
    bridge = entry["component"]
    board_ancestor = _board_ancestor(components, bridge)
    identity = bridge.get("identity") or {}
    widget_revision = _integer(identity, "widget_revision", f"I/O board '{bridge['id']}'", 8)
    # A distinct board component's own identity.part names this board's
    # catalog part; a bridge with no separate board (the Origin 200's
    # system board carries it directly) instead answers it on the
    # Bridge's own one-wire master, which is where a real Origin 200's
    # base I/O identity NIC lives.
    part = None
    other_identities = []
    if board_ancestor is not None:
        part = _board_identity_part(model, board_ancestor)
    if part is None:
        one_wire_parts = _one_wire_identities(model, components, bridge["id"])
        if one_wire_parts:
            part, *other_identities = one_wire_parts
    # A board component of its own, distinct from the node's own board (the
    # Origin 2000 family's plug-in I/O board), keeps its authored id; a
    # Bridge with no such board -- the Origin 200 family wires it directly
    # into the shared system board its own node's Hub also sits on -- gets
    # the descriptive name the plan's board-device work already uses for
    # this one board kind, since no authored id names it.
    shares_board_with_a_node = board_ancestor is not None and any(
        component.get("kind") == "node-asic"
        and _is_within(components, component, board_ancestor["id"])
        for component in components.values()
    )
    # The same structural signal picks the board kind: a Bridge sharing
    # its node's own board is IP29's own onboard I/O (the Origin 200
    # family), never SGI_MODEL_IO_BASEIO -- that enumerator names the
    # Origin 2000 family's own standalone BaseIO/IO6 board, the
    # `board_ancestor["id"]` case below.
    if (bridge.get("hardware_kind") == "xbridge"
            and _inside_vbrick(components, bridge)):
        board_id = _grouping_module(components, bridge)["id"]
        board_kind = "VBRICK"
    elif board_ancestor is not None and not shares_board_with_a_node:
        board_id = board_ancestor["id"]
        # The BaseIO-G variant carries a MediaIO daughtercard whose own
        # second IOC3 fits a PS/2 keyboard and mouse. That second IOC3 is
        # the structural signal: the catalogue ownership rule gives BaseIO-G its own
        # `sgi-baseio` preset (`io6g`), which builds both IOC3s, where
        # the plain BASEIO preset (`io6`) builds only the board's own
        # first one.
        # An add-in card names its own kind; only a board naming none is
        # read as a BaseIO by its fitted parts.
        declared = board_ancestor.get("hardware_kind")
        if declared is not None:
            board_kind = _XIO_BOARD_KINDS.get(declared)
            if board_kind is None:
                raise CompileError(
                    f"I/O board '{board_id}' has board kind '{declared}', "
                    "which has no catalogue preset")
        else:
            carries_media_io = any(
                component.get("hardware_kind") == "ioc3"
                and _is_within(components, component, board_id)
                and _ioc3_has_keyboard_mouse(components, component["id"])
                for component in components.values())
            board_kind = "BASEIO_G" if carries_media_io else "BASEIO"
    else:
        board_id = "baseio"
        # The GIGAchannel's Crosstown board, fitted in place of the XTalk
        # jumper, adds its own board record beside the IP29's; that is
        # the one difference between the two presets, whether the Bridge
        # is then cabled to a Crossbow or looped back to its own Hub.
        fitted_crosstown = board_ancestor is not None and any(
            component.get("hardware_kind") == "crosstown"
            and _is_within(components, component, board_ancestor["id"])
            for component in components.values())
        board_kind = "IP29_IO_CROSSTOWN" if fitted_crosstown else "IP29_IO"
    pci, ioc3_serial = _board_pci_population(
        model, components, _HUB_PCI_KINDS, {bridge["id"]})
    return {
        "id": board_id,
        "module_index": entry["module_index"],
        "board_kind": board_kind,
        "slot_index": entry["slot"],
        "label": entry.get("label"),
        "revision": widget_revision,
        "serial": _synthetic_serial(model, bridge["id"], "bridge", 0),
        "part": part or "",
        "other_identities": other_identities,
        "pci": pci,
        "ioc3_serial": ioc3_serial,
    }


def _compile_module(model: dict, module_id: str, module_index: int,
                     node_entries: list[dict], controllers: list[dict]) -> dict:
    emulation = model["machine"].get("emulation", {})
    serial = ""
    for item in emulation.get("module_semantic_serials", []) or []:
        if item.get("component") == module_id:
            serial = item["serial"]
    initial_number = 0
    for item in emulation.get("initial_module_numbers", []) or []:
        if item.get("component") == module_id:
            initial_number = item["number"]
    nodes = [index for index, entry in enumerate(node_entries)
             if entry["module_index"] == module_index]
    owned = [index for index, controller in enumerate(controllers)
             if controller["module_index"] == module_index]
    if len(owned) > 1:
        raise CompileError(f"module '{module_id}' has more than one controller")
    return {
        "id": module_id, "serial": serial, "initial_module_number": initial_number,
        "controller_index": owned[0] if owned else -1, "nodes": nodes,
    }


def _compile_router(model: dict, entry: dict, router_index: int) -> dict:
    router = entry["component"]
    router_asic = router.get("router_asic")
    if not isinstance(router_asic, dict):
        raise CompileError(f"router '{router['id']}' has no router_asic block")
    identity = router.get("identity") or {}
    return {
        "id": router["id"],
        "module_index": entry["module_index"],
        "slot": entry["slot"],
        "port_count": _integer(router_asic, "port_count", f"router '{router['id']}'", 8),
        "part": num(_required(identity, "part", f"router '{router['id']}'")),
        "revision": _integer(
            identity, "hardware_revision", f"router '{router['id']}'", 8),
        "chip_inputs": _integer(
            identity, "chip_inputs", f"router '{router['id']}'", 8),
        # A router's serial is an emulator-assigned synthetic identity,
        # the same mechanism a node's Hub uses (`_synthetic_serial`); the
        # router's own `identity` block never carries one.
        "serial": _synthetic_serial(model, router["id"], "router", router_index),
    }


def _crossbar_module_index(components: dict[str, dict], modules: dict[str, int],
                            crossbar: dict) -> int:
    module = _grouping_module(components, crossbar)
    if module["id"] not in modules:
        raise CompileError(
            f"crossbar '{crossbar['id']}' is not grouped under a positioned module")
    return modules[module["id"]]


def _compile_crossbars(model: dict, components: dict[str, dict], modules: dict[str, int],
                        crossbars: list[dict]) -> list[dict]:
    """SGIModelCrossbar records: id, containing module, kind and fitted
    silicon revision. A crossbar's manufacturer id, part number and
    initially-populated-links mask are fixed by which chip it is
    (the catalogue ownership rule) and are not carried here: sn-machine.c already
    hardcodes the wired-link-position mask rather than reading one, and
    computes which links start alive by walking xio-attachments, never
    by reading a startup-health policy.

    The XBridge's own crossbar section is part of the IP45 node board, as
    the L1 and the boot flash are a board-integrated part: the node
    device creates it directly, so it never gets a catalogue record of its
    own, unlike a standalone Crossbow chip. `crossbars` is `index_graph`'s
    generic scan over every `kind: crossbar` component, Origin 300's
    XBridge crossbar included; this is the one place that distinction is
    made, so the same call serves every generation.
    """
    kinds = {"xbow": "XBOW", "xbridge": "XBRIDGE"}
    result = []
    for crossbar in crossbars:
        kind = (crossbar.get("crossbar") or {}).get("kind")
        if kind not in kinds:
            raise CompileError(f"crossbar '{crossbar['id']}' names unsupported kind '{kind}'")
        if kind == "xbridge":
            continue
        identity = crossbar.get("identity") or {}
        result.append({
            "id": crossbar["id"],
            "module_index": _crossbar_module_index(components, modules, crossbar),
            "kind": kinds[kind],
            "widget_revision": _integer(identity, "widget_revision", crossbar["id"], 4),
        })
    return result


def _compile_controllers(model: dict, components: dict[str, dict],
                          modules: dict[str, int]) -> list[dict]:
    """SGIModelController records: one per module-scoped system-controller
    component (Origin 2000's module MSC, Origin 300's chassis L1).

    A `system-controller` component that only ever answers as an
    endpoint (the Origin 200 family's own per-node ELSC, subordinate to
    that node's own i2c adapter) is not one of these records: its
    identity is already the node's own `system_controller` field from
    `_node_identity` (the ELSC's own bus address is a chip constant, not
    carried). Everything else is a controller distinct
    from any one node: a component that masters a bus itself (a
    module-wide co-master other nodes' adapters share, as Origin 2000's
    MSC does), and a component modeled with no bus relationship at all
    (Origin 300's L1, "a packet protocol over the Bedrock's UART, not a
    register file" -- it neither masters nor answers on any bus the
    resolved model authors).
    """
    kinds = {"msc": "MSC", "l1": "L1"}
    # I2C addressing and management NVRAM layout per kind (from reviewed datasheets).
    controller_addresses = {
        "msc": {
            "msc_address": 0x08,
            "nvram_address": 0x50,
            "nvram_address_count": 8,
            "nvram_bytes": 2048,
        },
        "l1": {
            "msc_address": 0,
            "nvram_address": 0,
            "nvram_address_count": 0,
            "nvram_bytes": 0,
        },
    }
    result = []
    for component in sorted(components.values(), key=lambda item: item["id"]):
        if component.get("kind") != "system-controller":
            continue
        connections = [item for item in model["connections"]
                       if item.get("component") == component["id"]]
        masters_bus = any(item.get("role") == "master" for item in connections)
        answers_as_endpoint = any(item.get("role") == "endpoint" for item in connections)
        if not masters_bus and answers_as_endpoint:
            # Subordinate to whichever node's own i2c adapter masters that
            # bus (the Origin 200 family's ELSC); recorded on that node's
            # own identity instead, by `_node_identity`.
            continue
        system_controller = component.get("system_controller")
        if not isinstance(system_controller, dict):
            raise CompileError(f"controller '{component['id']}' has no system_controller block")
        kind = system_controller.get("kind")
        if kind not in kinds:
            raise CompileError(f"controller '{component['id']}' names unsupported kind '{kind}'")
        module = _grouping_module(components, component)
        if module["id"] not in modules:
            raise CompileError(
                f"controller '{component['id']}' is not grouped under a positioned module")
        addresses = controller_addresses.get(kind, {})
        result.append({
            "id": component["id"],
            "kind": kinds[kind],
            "module_index": modules[module["id"]],
            "msc_address": addresses.get("msc_address", 0),
            "nvram_address": addresses.get("nvram_address", 0),
            "nvram_address_count": addresses.get("nvram_address_count", 0),
            "nvram_bytes": addresses.get("nvram_bytes", 0),
        })
    return result


_EVIDENCE_GRADES = {"established": "ESTABLISHED", "inferred": "INFERRED",
                    "unknown": "UNKNOWN"}


def _compile_node_populations(model: dict, graph: dict, nodes: list[dict],
                              io_boards: list[dict], xio_attachments: list[dict],
                              xio_slots: list[dict],
                              supported: list[list[int]]) -> list[dict]:
    """The chassis's node-population rows, as positions into its own lists.

    A product whose topology authors `node_populations` accepts each row's
    count of node boards, fitted in the listed node slots; the compiled
    chassis carries every node slot, Router and link any row fits, and a
    row names its own part of them. A product that authors none accepts
    exactly its composition: one row fitting everything, graded
    established like the rest of its strictly checked model.
    """
    authored = (model["composition"].get("topology") or {}).get("node_populations")
    links = graph["links"]
    if not authored:
        return [{
            "nodes": list(range(len(nodes))),
            "routers": list(range(len(graph["routers"]))),
            "cables": list(range(len(links))),
            "xio_slots": [{"slot": index, "controlling_node": slot["controlling_node"]}
                            for index, slot in enumerate(xio_slots)],
            "supported_cpu_populations": [list(item) for item in supported],
            "grade": "ESTABLISHED",
        }]

    node_by_slot: dict[int, int] = {}
    for index, entry in enumerate(graph["nodes"]):
        if entry["slot"] in node_by_slot:
            raise CompileError(
                f"node slot {entry['slot']} holds more than one node board; "
                "node-population rows name node boards by slot")
        node_by_slot[entry["slot"]] = index
    router_by_id = {entry["id"]: index for index, entry in enumerate(graph["routers"])}
    link_by_name = {link["name"]: index for index, link in enumerate(links)}
    slot_by_id = {slot["id"]: index for index, slot in enumerate(xio_slots)}
    # Which Crossbow each node's own Hub enters: a node controls only
    # positions on that Crossbow.
    host_crossbar: dict[int, int] = {}
    board_nodes = set()
    for attachment in xio_attachments:
        node = attachment["node_index"]
        if attachment["crossbar_index"] >= 0 and node not in host_crossbar:
            host_crossbar[node] = attachment["crossbar_index"]
        else:
            board_nodes.add(node)
    per_node_max = [max(population[index] for population in supported)
                    for index in range(len(nodes))]

    rows = []
    previous: list[int] = []
    for position, row in enumerate(authored):
        label = f"node-population row {position}"
        slots = row["node_slots"]
        missing = [slot for slot in slots if slot not in node_by_slot]
        if missing:
            raise CompileError(f"{label} names node slot {missing[0]}, which has no node board")
        fitted = [node_by_slot[slot] for slot in slots]
        if len(set(fitted)) != len(fitted):
            raise CompileError(f"{label} names a node slot twice")
        if len(fitted) <= len(previous) or fitted[:len(previous)] != previous:
            raise CompileError(
                f"{label} must add node boards to the previous row's, in fill order")
        previous = fitted
        if 0 not in fitted:
            raise CompileError(f"{label} leaves out the console node, node slot "
                               f"{graph['nodes'][0]['slot']}")
        missing_boards = sorted(node for node in board_nodes if node not in fitted)
        if missing_boards:
            raise CompileError(
                f"{label} leaves out node slot {graph['nodes'][missing_boards[0]]['slot']}, "
                "which controls one of the chassis's own I/O boards")

        routers = []
        for router in row.get("routers", []):
            if router not in router_by_id:
                raise CompileError(f"{label} names unknown Router '{router}'")
            routers.append(router_by_id[router])
        cables = []
        occupied: set[tuple[str, int, int]] = set()
        for name in row.get("fabric_links", []):
            if name not in link_by_name:
                raise CompileError(f"{label} names unknown fabric link '{name}'")
            link = links[link_by_name[name]]
            for end in link["ends"]:
                present = (end["index"] in fitted if end["kind"] == "NODE"
                           else end["index"] in routers)
                if not present:
                    raise CompileError(
                        f"{label} fits fabric link '{name}' without the "
                        f"{end['kind'].lower()} at one of its ends")
                key = (end["kind"], end["index"], end["port"])
                if key in occupied:
                    raise CompileError(f"{label} attaches fabric endpoint {key} twice")
                occupied.add(key)
            cables.append(link_by_name[name])

        usable = []
        for connector, controller_slot in row.get("xio_slots", {}).items():
            if connector not in slot_by_id:
                raise CompileError(f"{label} names unknown empty XIO slot '{connector}'")
            if controller_slot not in slots:
                raise CompileError(
                    f"{label} gives XIO slot '{connector}' to node slot "
                    f"{controller_slot}, which the row does not fit")
            controller = node_by_slot[controller_slot]
            slot_index = slot_by_id[connector]
            if host_crossbar.get(controller) != xio_slots[slot_index]["crossbar_index"]:
                raise CompileError(
                    f"{label} gives XIO slot '{connector}' to node slot "
                    f"{controller_slot}, whose Hub is on another Crossbow")
            usable.append({"slot": slot_index, "controlling_node": controller})

        populations = row.get("supported_cpu_populations")
        if not populations:
            raise CompileError(f"{label} accepts no processor population")
        for counts in populations:
            if len(counts) != len(fitted):
                raise CompileError(
                    f"{label} population {counts} does not cover its {len(fitted)} nodes")
            for node, count in zip(fitted, counts):
                if not 1 <= count <= per_node_max[node]:
                    raise CompileError(
                        f"{label} population {counts} fits {count} processors on "
                        f"node slot {graph['nodes'][node]['slot']}, which is wired "
                        f"for 1 to {per_node_max[node]}")
        grade = _EVIDENCE_GRADES.get(row.get("grade"))
        if grade is None:
            raise CompileError(f"{label} has no evidence grade")
        rows.append({
            "nodes": fitted, "routers": routers, "cables": cables,
            "xio_slots": sorted(usable, key=lambda item: item["slot"]),
            "supported_cpu_populations": [list(counts) for counts in populations],
            "grade": grade,
        })
    if len(previous) != len(nodes):
        raise CompileError(
            "the largest node-population row must fit every node board the chassis names")
    return rows


def compile_model(model: dict) -> dict:
    """Compile one resolved system model into a format-2 topology dictionary.

    Refuses a reference-role configuration, and any configuration this
    compiler cannot yet ground in the resolved model, by raising
    `CompileError` naming the missing fact -- never by inventing one.
    """
    machine = model.get("machine", {})
    if machine.get("model_role") != "emulation-target":
        raise CompileError(
            f"'{machine.get('name', model.get('composition', {}).get('product'))}' "
            f"is model_role '{machine.get('model_role')}', not an emulation "
            "target; format 2 carries no facts for a configuration nobody "
            "has committed to emulating"
        )
    generation = machine.get("generation")
    if generation not in ("SN0", "SN1"):
        raise CompileError(
            f"generation '{generation}' is not supported by this compiler yet "
            "(only SN0/Hub and SN1/Bedrock nodes have a grounded reset-mode "
            "encoding today)"
        )

    graph = index_graph(model)
    components = graph["components"]
    node_count = len(graph["nodes"])
    if node_count == 0:
        raise CompileError("configuration has no node-asic component")
    modules_by_id = {module["id"]: module["index"] for module in graph["modules"]}

    try:
        nodes = [_compile_node(model, components, entry, position, node_count)
                 for position, entry in enumerate(graph["nodes"])]
        routers = [_compile_router(model, entry, index)
                   for index, entry in enumerate(graph["routers"])]
        controllers = _compile_controllers(model, components, modules_by_id)
        modules = [_compile_module(model, module["id"], module["index"],
                                    graph["nodes"], controllers)
                   for module in graph["modules"]]
        supported_cpu_populations = _supported_cpu_populations(model, node_count)
        description = _required(
            machine.get("emulation", {}), "display_description", "emulation")
        # `_compile_crossbars` reads `graph["crossbars"]` either way: its
        # own xbridge-kind check is what excludes Origin 300's XBridge from
        # getting a record, so this one call serves every generation.
        crossbars = _compile_crossbars(model, components, modules_by_id, graph["crossbars"])
        if generation == "SN1":
            # Origin 300 has no separate Crossbow: the XBridge's two
            # PCI-hosting heads are found together, off each Bedrock's
            # own XIO bus (see `_compile_bedrock_io_boards`'s own
            # comment), not through the Hub/Crossbow-oriented graph the
            # shared io-board/xio-attachment compilers below read.
            io_boards, xio_attachments = [], []
            for index, entry in enumerate(graph["nodes"]):
                boards, attachments = _compile_bedrock_io_boards(
                    model, components, entry["id"], node_index=index,
                    module_index=entry["module_index"],
                    first_attachment=len(xio_attachments))
                io_boards.extend(boards)
                xio_attachments.extend(attachments)
            for entry, attachment in zip(graph["io_boards"],
                                         graph["xio_attachments"], strict=True):
                board = _compile_io_board(model, components, entry)
                board["slot_index"] = len(xio_attachments)
                io_boards.append(board)
                xio_attachments.append(attachment)
            xio_slots = []
        else:
            io_boards = [_compile_io_board(model, components, entry)
                         for entry in graph["io_boards"]]
            xio_attachments = graph["xio_attachments"]
            xio_slots = graph["xio_slots"]
    except CompileError:
        raise
    except ValueError as exc:
        # A shared helper (machine_model._integer/_required and similar)
        # raises plain ValueError; re-typed here so every way this
        # function can fail is a CompileError, never a bare one a caller
        # has to guess the meaning of.
        raise CompileError(str(exc)) from exc

    console = 0 if node_count else -1
    junk_uart = _junk_uart_node(components, graph["nodes"])
    node_populations = _compile_node_populations(
        model, graph, nodes, io_boards, xio_attachments, xio_slots,
        supported_cpu_populations)

    return {
        "name": model["composition"]["product"],
        "generation": generation,
        "description": description,
        "nodes": nodes,
        "modules": modules,
        "routers": routers,
        "links": graph["links"],
        "crossbars": crossbars,
        "xio_attachments": xio_attachments,
        "xio_slots": xio_slots,
        "io_boards": io_boards,
        "controllers": controllers,
        "console": console,
        "junk_uart": junk_uart,
        "supported_cpu_populations": supported_cpu_populations,
        "node_populations": node_populations,
    }
