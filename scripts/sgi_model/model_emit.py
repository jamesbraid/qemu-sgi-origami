# SPDX-License-Identifier: BSD-3-Clause

"""Emit the compiled catalogue and its manifest.

Takes the dictionaries `model_compile.compile_model` produces, one per
configuration, and renders `hw/mips/sgi/sgi-machines.json` (`topologies`
trimmed to what the catalogue output rule keeps, plus the
launcher-facing `offerings` list -- see `render_catalogue_json`) and the
manifest `sn-model.json` (`format: 4`, format key `"sgi-sn"`). QEMU parses
the catalogue into QAPI-generated types (`qapi/sgi-machines.json`) at build
time; this module no longer renders C.
"""

from __future__ import annotations

import hashlib
import json
import re

from .model_compile import CompileError, compile_model


def render_manifest(source_models: dict[str, dict], outputs: dict[str, str]) -> str:
    """The `sn-model.json` manifest: format 4, format key `sgi-sn`."""
    manifest = {
        "format": 4,
        "format_key": "sgi-sn",
        "source_sha256": hashlib.sha256(json.dumps(
            source_models, sort_keys=True, separators=(",", ":"),
        ).encode()).hexdigest(),
        "outputs": {path: hashlib.sha256(text.encode()).hexdigest()
                     for path, text in sorted(outputs.items())},
    }
    return json.dumps(manifest, indent=2, sort_keys=True) + "\n"


def _strip_none(value):
    """Drop every dict key whose value is `None`, recursively.

    `model_compile.compile_model` marks an unpopulated slot or an unknown
    byte size with `None` (a `SGIModelCpuSlot` with no fitted processor, an
    I/O board with no IOC3 function, an identity resource of undocumented
    size). QAPI represents "not present" by the key being absent, not by an
    explicit JSON `null`, so the catalogue must never emit one.
    """
    if isinstance(value, dict):
        return {key: _strip_none(item) for key, item in value.items()
                 if item is not None}
    if isinstance(value, list):
        return [_strip_none(item) for item in value]
    return value


def _enum(value: str) -> str:
    """A compiled `*_KIND`-style tag as the catalogue's lower-case,
    hyphenated form (`AM29F080` -> `am29f080`, `IP29_IO` -> `ip29-io`).
    Every other `kind` field (a processor's, e.g. `"R10000"`) is a plain
    catalogued string, not an enumeration, and keeps its compiled value
    unchanged.
    """
    return value.lower().replace("_", "-")


def _kebab_keys(value):
    """Recursively rename every dict key from the compiler's snake_case to
    the catalogue's QAPI-convention lower-case-hyphenated form, leaving
    every value untouched. Used only for substructures the ownership rule
    does not trim (a module's membership, a CPU population, a processor
    assembly): every record the rule does trim is rendered by its own
    explicit function below instead, so a field the rule drops cannot leak
    back in by this generic path.
    """
    if isinstance(value, dict):
        return {key.replace("_", "-"): _kebab_keys(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_kebab_keys(item) for item in value]
    return value


# the catalogue ownership rule keeps a node's controller-bus identity (its revisions and
# fitted serial) and drops the addresses a datasheet already fixes for
# every Hub: the I2C controller's own bus offset and the ELSC/NVRAM
# addresses behind it (hw/misc/sgi-hub.c and sn0.c already carry those as
# device constants; nothing in the builder reads them off the descriptor).
def _identity(raw: dict) -> dict:
    identity = {
        "serial": raw["serial"],
        "i2c-controller": raw["i2c_controller"],
        "system-controller": raw["system_controller"],
    }
    for source, target in (("ni_revision", "ni-revision"),
                           ("widget_revision", "widget-revision"),
                           ("clock_hz", "clock-hz")):
        if source in raw:
            identity[target] = raw[source]
    return identity


# Flash kind and size only: which named views (reset/lboot/rboot on SN0;
# reset/local/local-high/remote on SN1) a node's flash controller answers,
# and their addresses, are a per-node-ASIC constant (every view the
# compiler ever emitted was already sized to the flash's own total size),
# so sn1-node.c now names them itself instead of reading them here.
def _flash(raw: dict) -> dict:
    flash = {"kind": _enum(raw["kind"]), "size": raw["size"]}
    for wire in ("select", "strap"):
        if wire in raw:
            flash[wire] = raw[wire]
    return flash


def _node(raw: dict) -> dict:
    if raw["asic_kind"] == "HEART":
        return {
            "id": raw["id"], "module-index": raw["module_index"],
            "slot": raw["slot"], "asic-kind": "heart",
            "heart": _kebab_keys(raw["heart"]),
            "cpu-slots": _kebab_keys(raw["cpu_slots"]),
            "bank-mib": raw["bank_mib"],
            "accepted-memory-mib": raw["accepted_memory_mib"],
            "accepted-bank-mib": raw["accepted_bank_mib"],
            "banks-descending": raw["banks_descending"],
            "flash": _flash(raw["flash"]), "slave-role": False,
        }
    node = {
        "id": raw["id"],
        "module-index": raw["module_index"],
        "slot": raw["slot"],
        "hub-slot-bits": raw["hub_slot_bits"],
        "asic-kind": _enum(raw["asic_kind"]),
        "identity": _identity(raw["identity"]),
        "cpu-slots": _kebab_keys(raw["cpu_slots"]),
        "bank-mib": raw["bank_mib"],
        "accepted-memory-mib": raw["accepted_memory_mib"],
        "flash": _flash(raw["flash"]),
        "slave-role": raw["slave_role"],
    }
    if "option_intx_map" in raw:
        node["option-intx-map"] = raw["option_intx_map"].copy()
    if "bedrock" in raw:
        node["bedrock"] = _kebab_keys(raw["bedrock"])
    if raw.get("board_kind") == "ip53":
        node["board-kind"] = "ip53"
    return node


def _io_peer(raw: dict) -> dict:
    return {
        "id": raw["id"], "module-index": raw["module_index"], "slot": raw["slot"],
        "asic-kind": _enum(raw["asic_kind"]), "identity": _identity(raw["identity"]),
    }



# The compiler checks port count against the selected router family.
def _router(raw: dict) -> dict:
    return {
        "id": raw["id"],
        "module-index": raw["module_index"],
        "slot": raw["slot"],
        "part": raw["part"],
        "revision": raw["revision"],
        "chip-inputs": raw["chip_inputs"],
        "serial": raw["serial"],
    }


def _link_end(raw: dict) -> dict:
    return {"kind": _enum(raw["kind"]), "index": raw["index"], "port": raw["port"]}


def _cable(raw: dict) -> dict:
    return {
        "name": raw["name"],
        "kind": _enum(raw["kind"]),
        "ends": [_link_end(end) for end in raw["ends"]],
    }


# A Crossbow's or an XBridge crossbar's own manufacturer id, part number
# and initially-populated-links mask are fixed by which chip it is
# (sn-machine.c already hardcodes the wired-link-position mask rather than
# reading one, and computes which links start alive by walking
# xio-attachments, never by reading initially_operational_links); the
# builder keys a part-number table on `kind` instead of reading one here.
def _crossbar(raw: dict) -> dict:
    return {
        "id": raw["id"],
        "module-index": raw["module_index"],
        "kind": _enum(raw["kind"]),
        "widget-revision": raw["widget_revision"],
    }


# The decode mask and discovery widget an attachment names are fixed by
# which chip answers a direct XIO link (IP29's Bridge decodes the
# same widget mask on every Origin 200 built), so a direct attachment
# (crossbar-index -1) carries them as a node-device constant instead.
def _xio_attachment(raw: dict) -> dict:
    return {
        "crossbar-index": raw["crossbar_index"],
        "widget": raw["widget"],
        "node-index": raw["node_index"],
    }


def _xio_slot(raw: dict) -> dict:
    return {
        "id": raw["id"],
        "crossbar-index": raw["crossbar_index"],
        "widget": raw["widget"],
        "controlling-node": raw["controlling_node"],
        "label": raw["label"],
    }


# the catalogue ownership rule's own worked example: "a board record carries preset,
# carrier, module, attachment, part, revision and serial". `carrier` is
# derived from `board_kind` alone, the one fact a launcher or the public
# `sgi` command needs to describe where the board sits: fixed to a node
# board, a removable XIO card, or a PCI board the machine places outright.
# `attachment` is the physical slot/head label (`slot_index`); a PCI
# function's own position, interrupt input and IOC3 population flags are
# device facts sgi-baseio's preset table already carries and the compiler
# no longer computes, but the IOC3 function's own one-wire identity is a
# genuine fitted-part fact with no other home, so it survives as
# `ioc3-serial`, absent where this preset's IOC3 head carries no
# population (Origin 300's second XBridge head).
_IO_BOARD_CARRIER = {
    "IP30": "onboard",
    "IP29_IO": "onboard",
    "IP29_IO_CROSSTOWN": "onboard",
    "BASEIO": "xio-slot",
    "BASEIO_G": "xio-slot",
    "IO8": "machine-placed",
    "FUEL": "onboard",
    "IO9": "machine-placed",
    "PCI_SHOEHORN": "xio-slot",
    "PCI_CARRIER": "xio-slot",
    "GIGACHANNEL": "machine-placed",
    "KONA": "xio-slot",
    "MENET": "xio-slot",
    "MSCSI": "xio-slot",
    "MGRAS": "xio-slot",
    "ODYSSEY": "machine-placed",
    "VBRICK": "machine-placed",
}

# The catalogue's own preset names, distinct from the compiler's internal
# `board_kind` tags: the Origin 2000 family's plain and MediaIO-carrying
# BaseIO boards catalog as `io6` and `io6g`, the names sgi-baseio's own
# preset table (and QEMU's launch-selectable `-M ...,io-board=`) use, not
# the `_enum()`-derived `baseio`/`baseio-g`. Every other board kind's
# preset still matches its `_enum()`-derived form.
_IO_BOARD_PRESET = {
    "IP30": "ip30",
    "IP29_IO": "ip29-io",
    "IP29_IO_CROSSTOWN": "ip29-io-crosstown",
    "BASEIO": "io6",
    "BASEIO_G": "io6g",
    "IO8": "io8",
    "FUEL": "fuel",
    "IO9": "io9",
    "PCI_SHOEHORN": "pci-shoehorn",
    "PCI_CARRIER": "pci-carrier",
    "GIGACHANNEL": "gigachannel",
    "KONA": "kona",
    "MENET": "menet",
    "MSCSI": "mscsi",
    "MGRAS": "mgras",
    "ODYSSEY": "odyssey",
    "VBRICK": "vbrick",
}

# The base I/O designs, whose IOC3 carries the board's Timekeeper. An add-in
# card's IOC3 (a Cad Duo in a shoehorn) has none, so it takes no NVRAM.
_TIMEKEEPER_BOARD_KINDS = {
    "IP29_IO", "IP29_IO_CROSSTOWN", "BASEIO", "BASEIO_G", "IO8", "IO9"}


def _io_board(raw: dict) -> dict:
    board = {
        "id": raw["id"],
        "module-index": raw["module_index"],
        "preset": _IO_BOARD_PRESET[raw["board_kind"]],
        "carrier": _IO_BOARD_CARRIER[raw["board_kind"]],
        "populated": raw.get("populated", True),
        "other-identity-parts": raw["other_identities"],
    }
    if "slot_index" in raw:
        board["attachment"] = raw["slot_index"]
    if "pci_host_index" in raw:
        board["pci-host-index"] = raw["pci_host_index"]
        board["pci-head"] = raw["pci_head"]
    if raw.get("label") is not None:
        board["label"] = raw["label"]
    for name, key in (("part", "part"), ("revision", "revision"),
                      ("serial", "serial"), ("ioc3-serial", "ioc3_serial")):
        if raw[key] is not None:
            board[name] = raw[key]
    if "system_timekeeper" in raw:
        board["system-timekeeper"] = _kebab_keys(raw["system_timekeeper"])
    if raw.get("ds1742") is True:
        board["ds1742"] = True
    if "ds1742_fresh_year_base" in raw:
        board["ds1742-fresh-year-base"] = raw["ds1742_fresh_year_base"]

    for key in ("ioc3_subsystem_id", "mac_source"):
        if key in raw:
            board[key.replace("_", "-")] = _kebab_keys(raw[key])
    pci_functions = raw.get("pci_functions", [])
    emit_pci_functions = "pci_functions" in raw
    if raw.get("board_kind") == "IO9":
        pci_functions = raw.get("pci", [])
        emit_pci_functions = "pci" in raw
    if emit_pci_functions:
        functions = []
        for entry in pci_functions:
            function = {"kind": _enum(entry["kind"]),
                        "revision": entry["revision"]}
            for key in ("pci_device", "pci_function", "intx_pin",
                        "pic_input", "clock_hz",
                        "serial_dma_experimental"):
                if key in entry:
                    function[key.replace("_", "-")] = entry[key]
            functions.append(function)
        board["pci-functions"] = functions
    return board


# A shared controller's MicroLAN/NVRAM addresses are fixed by the MSC
# chip and unread by the builder (module-management identity is built
# from the module's own serial and slot, not from these); dropped.
def _controller(raw: dict) -> dict:
    controller = {
        "id": raw["id"],
        "module-index": raw["module_index"],
        "kind": _enum(raw["kind"]),
    }
    if "l1" in raw:
        controller["l1"] = _kebab_keys(raw["l1"])
    return controller


def _render_topology(topology: dict, machine_name: str) -> dict:
    """One `compile_model` entry as the catalogue's `topologies` JSON shape,
    trimmed to what the catalogue output rule keeps: member names
    kebab-cased, enumerations lower-cased, every device-internal fact
    (a PCI function's position, an I2C address, a controller-bus address,
    NVRAM geometry, a crossbar's manufacturer or part id, a Router's port
    count, a flash's named views, an XIO attachment's decode mask and
    discovery widget) left for the device model that already owns it.
    `product` scopes topology= selection. Products with several profiles
    cannot use topology name alone to choose one at runtime.
    """
    result = {
        "name": topology["name"],
        "product": machine_name,
        "generation": _enum(topology["generation"]),
        "description": topology["description"],
        "nodes": [_node(node) for node in topology["nodes"]],
        "modules": _kebab_keys(topology["modules"]),
        "routers": [_router(router) for router in topology["routers"]],
        "cables": [_cable(link) for link in topology["links"]],
        "crossbars": [_crossbar(crossbar) for crossbar in topology["crossbars"]],
        **({"pci-host-bridges": [
            {"id": host["id"], "kind": _enum(host["kind"]),
             "xio-attachment-index": host["xio_attachment_index"],
             **({"compatibility-profile": host["compatibility_profile"]}
                if "compatibility_profile" in host else {})}
            for host in topology["pci_host_bridges"]
        ]} if topology.get("pci_host_bridges") else {}),
        "xio-attachments": [
            _xio_attachment(attachment) for attachment in topology["xio_attachments"]],
        "xio-slots": [_xio_slot(slot) for slot in topology["xio_slots"]],
        "io-boards": [_io_board(board) for board in topology["io_boards"]],
        "controllers": [_controller(controller) for controller in topology["controllers"]],
        "console": topology["console"],
        "junk-uart": topology["junk_uart"],
        "node-populations": [_node_population(row)
                             for row in topology["node_populations"]],
    }

    if topology.get("io_peers"):
        result["io-peers"] = [_io_peer(peer) for peer in topology["io_peers"]]
    if topology.get("io_assemblies"):
        result["io-assemblies"] = [
            {**_kebab_keys(record), "kind": _enum(record["kind"])}
            for record in topology["io_assemblies"]]
    if "l1_management" in topology:
        result["l1-management"] = _kebab_keys(topology["l1_management"])
    if "ip27_firmware" in topology:
        result["ip27-firmware"] = _kebab_keys(topology["ip27_firmware"])
    if "ip35_firmware" in topology:
        result["ip35-firmware"] = _kebab_keys(topology["ip35_firmware"])
    if "processor_options" in topology:
        result["processor-options"] = _kebab_keys(topology["processor_options"])
    if "graphics" in topology:
        result["graphics"] = _kebab_keys(topology["graphics"])
    return result

def _node_population(raw: dict) -> dict:
    """One node-population row. compile_model represents each accepted
    processor population as a bare list of per-node counts;
    SGIModelCpuPopulation wraps it in a record with one member (QAPI has
    no bare-list-of-lists member type), so this wraps here rather than
    passing the compiler's own shape through unchanged the way
    _kebab_keys does for a plain record.
    """
    return {
        "nodes": list(raw["nodes"]),
        "routers": list(raw["routers"]),
        "cables": list(raw["cables"]),
        "xio-slots": [{"slot": item["slot"],
                         "controlling-node": item["controlling_node"]}
                        for item in raw["xio_slots"]],
        "supported-cpu-populations": [
            {"cpus-per-node": list(population)}
            for population in raw["supported_cpu_populations"]],
        "grade": _enum(raw["grade"]),
    }


# Target zero is reserved for the host initiator. The adapter kind sets the
# target range. The two ISP1040 channels stay at the narrow bound because
# their individual widths are unresolved. ISP12160 supports wide Ultra3.
# These ranges are device constants shared by every configuration.
_SCSI_TARGETS = {
    "ISP1040": list(range(1, 8)),
    "ISP12160": list(range(1, 16)),
}

# The `-serial` line order a Hub-family launch places each backend on,
# structurally: ioc3_a (also the primary console), ioc3_b, elsc (the
# system controller's AUX port) and junk (the bring-up UART, node 0
# only) -- tools/podlib.py's `CHARDEV_IOC3_A`/`CHARDEV_IOC3_B`/
# `CHARDEV_SYSTEM_CONTROLLER`/`CHARDEV_BRINGUP_PORT` and `serial_args`,
# which every Hub-family launcher already calls in this order. A Bedrock
# node's L1 combines the console and the system controller dialogue on
# one UART instead (include/hw/mips/sgi/sn-machine.h's
# sgi_sn1_serial_line(): one `-serial chardev:l1` first).
_DS1742_ARGUMENT = "-drive if=none,id=sgi-ds1742-data-BOARD"
_DS1742_BYTES = 2048
_DS1742_CLOCK_BYTES = 16
_PFLASH_ARGUMENT = "-drive if=pflash,unit=N"

# Each Timekeeper kind's NVRAM and the clock reference record its model
# keeps beside it: the m48t35 window (hw/rtc/m48t35.h's M48T35_SIZE and
# M48T35_CLOCK_REF_BYTES), which an IOC3's own Timekeeper also is, and
# the DS1685 (hw/rtc/ds1685.h's NVRAM and DS1685_CLOCK_REF_BYTES).
_TIMEKEEPER_BYTES = {
    "m48t35-compatible": (0x8000, 16),
    "ds1685": (512, 24),
}
# The BASEIO PROM flash part (hw/mips/sgi/baseio-flash.c), on the boards
# that fit one.
_IO_FLASH_BYTES = 0x100000
# include/hw/misc/sgi-bedrock-flash.h's SGI_BEDROCK_FLASH_SIZE.
_EXPANSION_FLASH_BYTES = 16 * 1024 * 1024
_IO_FLASH_BOARD_KINDS = {"BASEIO", "BASEIO_G", "GIGACHANNEL"}
_BOOT_PROM_KINDS = {"SN0": "ip27-prom", "SN1": "ip35-prom", "IP30": "ip30-prom"}


def _scsi_adapters_for(topology: dict) -> list[dict]:
    """One `scsi-adapters` entry per SCSI host adapter any I/O board carries, in
    the same device order `_compile_io_board`/`_compile_bedrock_pci_function`
    already sort PCI functions into. `bus` follows QEMU's own generic-SCSI
    numbering, one bus per host adapter instantiated.
    """
    adapters = []
    for board in topology["io_boards"]:
        for function in board["pci"]:
            targets = _SCSI_TARGETS.get(function["kind"])
            if targets is None:
                continue
            adapters.append({
                "type": "scsi", "bus": f"scsi.{len(adapters)}",
                "targets": list(targets),
            })
    return adapters


# include/hw/mips/sgi/sn-machine.h's SGI_SN1_SERIAL_LINES_PER_NODE.
_SN1_SERIAL_LINES_PER_NODE = 5


def _sn1_serial_line(node_index: int, line: int) -> int:
    """sgi_sn1_serial_line(): the -serial position a node's line occupies."""
    return node_index * _SN1_SERIAL_LINES_PER_NODE + line


def _consoles_for(topology: dict, harness: dict) -> list[dict]:
    """Named serial endpoints exposed by the product's launch policy."""
    if topology["generation"] == "IP30":
        return [
            {"kind": "console", "node": 0, "argument": "-serial chardev:ioc3_a"},
            {"kind": "second-port", "node": 0, "argument": "-serial chardev:ioc3_b"},
        ]
    if topology["generation"] == "SN1":
        # Each chassis has an L1 line. The first node's IOC3 serial A and
        # B follow its L1 (sn-machine.h's sgi_sn1_serial_line()).
        consoles = [{"kind": "console" if harness["console"] == "l1-uart"
                     else "system-controller", "node": 0,
                     "argument": "-serial chardev:l1",
                     "line": _sn1_serial_line(0, 0)}]
        ports = harness.get("ioc3_serial_ports",
                            ["ioc3-serial-a"]
                            if harness["console"] == "ioc3-serial-a" else [])
        for port in sorted(ports):
            consoles.append({
                "kind": ("console" if harness["console"] == port
                         else "second-port"),
                "node": 0,
                "argument": f"-serial chardev:ioc3_{port[-1]}",
                "line": _sn1_serial_line(0, 1 + (port[-1] == "b")),
            })
        for controller in topology["controllers"]:
            nodes = topology["modules"][controller["module_index"]]["nodes"]
            if not nodes:
                continue
            node = nodes[0]
            if controller["kind"] == "L1" and node:
                consoles.append({
                    "kind": "system-controller", "node": node,
                    "argument": f"-serial chardev:l1_node{node}",
                    "line": _sn1_serial_line(node, 0),
                })
        return consoles
    consoles = [
        {"kind": "console", "node": 0, "argument": "-serial chardev:ioc3_a"},
        {"kind": "second-port", "node": 0, "argument": "-serial chardev:ioc3_b"},
        {"kind": "system-controller", "node": 0, "argument": "-serial chardev:elsc"},
    ]
    if topology["junk_uart"] != -1:
        consoles.append({
            "kind": "junk-uart", "node": topology["junk_uart"],
            "argument": "-serial chardev:junk",
        })
    return consoles


def _ip53_console_records(topology: dict, harness: dict,
                          population_nodes: tuple[int, ...]) -> list[dict]:
    """Project authored IP53 endpoints into one offering's node population.

    Input records bind to stable topology node IDs.  The output keeps the
    existing SGIModelConsole meaning: ``node`` is the owner's index in the
    full topology node array, not a launcher endpoint identity.
    """
    if harness.get("launch_recipe") != "ip53-discovery":
        raise ValueError("console_records require launch_recipe ip53-discovery")
    console_policy = harness.get("console")
    if console_policy not in ("l1-uart", "ip53-ioc4"):
        raise ValueError(
            "ip53-discovery requires an explicit L1 or IOC4 console policy"
        )
    records = harness.get("console_records")
    if not isinstance(records, list) or not records:
        raise ValueError("ip53-discovery requires authored console_records")

    topology_nodes = topology["nodes"]
    node_indices = {node["id"]: index
                    for index, node in enumerate(topology_nodes)}
    if len(node_indices) != len(topology_nodes):
        raise ValueError("IP53 console topology node IDs must be unique")
    if (len(set(population_nodes)) != len(population_nodes) or
            any(index < 0 or index >= len(topology_nodes)
                for index in population_nodes)):
        raise ValueError("IP53 console population has invalid node indices")
    selected_nodes = set(population_nodes)

    allowed_kinds = {
        "console", "second-port", "system-controller", "junk-uart",
    }
    launcher_names: set[str] = set()
    bindings: set[str] = set()
    projected = []
    for record in records:
        if (not isinstance(record, dict) or
                not isinstance(record.get("kind"), str) or
                record["kind"] not in allowed_kinds):
            raise ValueError("IP53 console record has an unsupported role")
        node_id = record.get("node_id")
        if not isinstance(node_id, str) or node_id not in node_indices:
            raise ValueError(
                f"IP53 console record names unknown node id {node_id!r}"
            )
        argument = record.get("argument")
        match = (re.fullmatch(r"-serial chardev:([a-z][a-z0-9_-]*)",
                              argument)
                 if isinstance(argument, str) else None)
        if match is None:
            raise ValueError(
                "IP53 console argument must be a serial chardev binding"
            )
        launcher_name = record.get("launcher_name")
        if launcher_name is not None:
            if (not isinstance(launcher_name, str) or
                    not re.fullmatch(r"[a-z][a-z0-9-]*", launcher_name)):
                raise ValueError("IP53 console launcher name is invalid")

        node_index = node_indices[node_id]
        if node_index not in selected_nodes:
            continue
        if argument in bindings:
            raise ValueError(
                "IP53 console records contain a duplicate binding in this offering"
            )
        bindings.add(argument)
        if launcher_name is not None:
            if launcher_name in launcher_names:
                raise ValueError(
                    "IP53 console records contain a duplicate name in this offering"
                )
            launcher_names.add(launcher_name)
        projected_record = {
            "kind": record["kind"], "node": node_index,
            "argument": argument,
        }
        if launcher_name is not None:
            projected_record["launcher-name"] = launcher_name
        projected.append(projected_record)

    primary = [record for record in projected if record["kind"] == "console"]
    if len(primary) != 1 or "launcher-name" not in primary[0]:
        raise ValueError(
            "each ip53-discovery offering requires one named primary console"
        )
    primary_binding = primary[0]["argument"]
    primary_name = primary[0]["launcher-name"]
    if console_policy == "l1-uart":
        if primary_name != "l1" or primary_binding != "-serial chardev:l1":
            raise ValueError(
                "l1-uart console policy must match its named L1 record"
            )
    else:
        endpoint = re.fullmatch(r"ioc4-serial-([0-3])", primary_name)
        if (endpoint is None or
                primary_binding != f"-serial chardev:ioc4_serial_{endpoint[1]}"):
            raise ValueError(
                "ip53-ioc4 console policy must match its named primary IOC4 record"
            )
    return projected


def _networks_for(topology: dict) -> list[dict]:
    """One `ioc3-ethernet` entry per IOC3 that fits an Ethernet MAC. Most
    IOC3s carry onboard Ethernet, but a BaseIO-G MediaIO daughtercard's
    second IOC3 does not, so the offering follows the compiled `ethernet`
    population bit rather than the bare IOC3 kind (`model_compile`'s
    `_board_pci_population`). A launch wires the onboard adapter with no
    explicit `-device` of its own, only a `-nic` or `-netdev` backend.
    """
    networks = []
    for board in topology["io_boards"]:
        for function in board["pci"]:
            if function["kind"] == "IOC3" and function["ethernet"]:
                networks.append({
                    "kind": "ioc3-ethernet",
                    "argument": f"-netdev ...,id=net{len(networks)}",
                })
    return networks


def default_topology(
    machine_name: str, product_models: dict[str, dict]
) -> str | None:
    """Return the topology QEMU builds when no topology= is given, if any."""
    from .registry import PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY

    if machine_name in PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY:
        return None
    return product_models[machine_name]["composition"]["product"]


def _product_facts(machine_name: str, product_models: dict[str, dict]) -> dict:
    """The harness-policy facts for one runtime product, resolved from its
    own base model-input file through the product-model registry. A
    topology that shares this product's name but carries no
    `machine.harness` block of its own (Origin 200's dual-module form)
    borrows these facts rather than going without.
    """
    machine = product_models[machine_name]["machine"]
    harness = machine["harness"]
    firmware_payload = machine["emulation"].get("firmware_payload")
    default_firmware_asset = harness.get("default_firmware_asset")
    if default_firmware_asset is not None:
        firmware_asset = default_firmware_asset
        firmware_sha256 = None
    elif firmware_payload is not None:
        firmware_asset = "assets/" + firmware_payload["path"]
        firmware_sha256 = firmware_payload.get("sha256")
    else:
        firmware_asset = firmware_sha256 = None
    second_selection = (
        firmware_payload.get("second_selection") if firmware_payload else None
    )
    boot_flash_bytes = (
        2 * int(second_selection["size"], 0) if second_selection else None
    )
    return {
        "harness": harness,
        "boot_flash_bytes": boot_flash_bytes,
        "firmware_argument": (
            firmware_payload["argument"] if firmware_payload is not None
            else "-bios"
        ),
        "firmware_asset": firmware_asset,
        "firmware_sha256": firmware_sha256,
    }


def _timekeeper_board(board: dict, harness: dict) -> bool:
    """Whether a board carries a Timekeeper whose NVRAM persists: a selected
    system Timekeeper, unless the product turns it off, or a base I/O
    design's own IOC3 Timekeeper."""
    if board.get("system_timekeeper"):
        return harness.get("timekeeper_nvram", True)
    return (board["board_kind"] in _TIMEKEEPER_BOARD_KINDS
            and any(function["kind"] == "IOC3" for function in board["pci"]))


def _timekeeper_initial(board: dict) -> str:
    """What a new machine's NVRAM for this board's Timekeeper starts
    from: the fresh IP27 image an unbacked part installs, unless the
    board's selected system Timekeeper turns that fixture off. An IOC3's
    own Timekeeper keeps the part's default, which installs it."""
    timekeeper = board.get("system_timekeeper")
    if timekeeper and not timekeeper["initial_ip27_fixture"]:
        return "zero"
    return "ip27-nvram"


# A module system controller's configuration NVRAM: eight 256-byte blocks
# at I2C 0x50-0x57 (hw/nvram/sgi-elsc-nvram.h).
_CONTROLLER_NVRAM_BYTES = 2048


def _module_has_controller_nvram(topology: dict, module: dict,
                                 fitted: list[int]) -> bool:
    """Whether a module has a system controller with a configuration
    NVRAM, by the rule `sgi_model_module_has_nvram()` applies: an Origin
    2000-family module controller (MSC), or an Origin 200 Hub whose
    embedded ELSC sits on one of the module's fitted nodes."""
    index = module["controller_index"]
    if index >= 0 and topology["controllers"][index]["kind"] == "MSC":
        return True
    return any((topology["nodes"][node].get("identity") or {}).get(
                   "system_controller", False)
               for node in module["nodes"] if node in fitted)


def _board_node(topology: dict, board: dict) -> int:
    """The node board a PCI-hosted I/O board's host attaches to."""
    host = topology["pci_host_bridges"][board["pci_host_index"]]
    return topology["xio_attachments"][
        host["xio_attachment_index"]]["node_index"]


def _storage_for(topology: dict, harness: dict, product: dict,
                 fitted: list[int]) -> list[dict]:
    """Every writable persistent store a launch fitting the `fitted` node
    boards has, numbered as the machine numbers what it builds, in the
    order a launcher creates them: each node's boot flash, each BASEIO
    PROM flash, each Timekeeper NVRAM with its clock record, each IO9
    DS1742 with its clock record, then each module system controller's
    configuration NVRAM. The
    name is the file name, the block node name and the machine property
    a launcher binds the store through. `initial` names what a new
    machine's store starts from: an `init-inputs` image, the fresh IP27
    Timekeeper image or zeroes.
    """
    generation = topology["generation"]
    storage = []
    for position, node in enumerate(fitted):
        if generation == "SN1":
            size = product["boot_flash_bytes"]
            if size is None:
                raise ValueError(
                    f"'{topology['name']}' has a Bedrock node but no "
                    "firmware_payload.second_selection to size its flash")
        else:
            size = topology["nodes"][node]["flash"]["size"]
        storage.append({"name": f"node{position}-flash", "size": size,
                        "read-only": False, "initial": "boot-prom"})
    # A population row keeps every I/O board: QEMU refuses a row that
    # would leave one without the node board it attaches to.
    boards = topology["io_boards"]
    flash_boards = [board for board in boards
                    if board["board_kind"] in _IO_FLASH_BOARD_KINDS]
    for index, _ in enumerate(flash_boards):
        storage.append({"name": f"io{index}-flash", "size": _IO_FLASH_BYTES,
                        "read-only": False, "initial": "io-prom"})
    nvram_sizes = [
        (*_TIMEKEEPER_BYTES[board.get("system_timekeeper", {}).get(
            "kind", "m48t35-compatible")], _timekeeper_initial(board))
        for board in boards if _timekeeper_board(board, harness)]
    for index, (size, clock_size, initial) in enumerate(nvram_sizes):
        storage.append({"name": f"nvram{index}", "size": size,
                        "read-only": False, "initial": initial})
        storage.append({"name": f"nvram{index}-clock", "size": clock_size,
                        "read-only": False, "initial": "zero"})
    # An IO9 board's DS1742 keeps its 2 KiB image and a clock record beside
    # it, as a Timekeeper does, named for the board that owns it
    # (hw/rtc/ds1742.c). A fresh, zeroed image takes the time of day.
    for board in boards:
        if (board.get("ds1742") is not True or
                _board_node(topology, board) not in fitted):
            continue
        storage.append({"name": f"ds1742-{board['id']}-data",
                        "size": _DS1742_BYTES, "read-only": False,
                        "initial": "zero"})
        storage.append({"name": f"ds1742-{board['id']}-clock",
                        "size": _DS1742_CLOCK_BYTES, "read-only": False,
                        "initial": "zero"})
    # An Origin 300 PCI expansion module's configuration storage, the
    # 16 MiB two-position Bedrock flash its experiment selects, named for
    # the module. A new machine's starts blank, as every caller-supplied
    # image has.
    for assembly in topology.get("io_assemblies", ()):
        if assembly.get("configuration_storage"):
            module = topology["modules"][assembly["module_index"]]
            storage.append({"name": f"{module['id']}-flash",
                            "size": _EXPANSION_FLASH_BYTES,
                            "read-only": False, "initial": "zero"})
    modules = [module for module in topology["modules"]
               if _module_has_controller_nvram(topology, module, fitted)]
    for index, _ in enumerate(modules):
        storage.append({"name": f"module{index}-nvram",
                        "size": _CONTROLLER_NVRAM_BYTES,
                        "read-only": False, "initial": "controller-nvram"})
    return storage


def _init_inputs_for(topology: dict, storage: list[dict]) -> list[dict]:
    """The images creating a new machine's storage needs, one per
    `initial` source its `storage` list names."""
    inputs = [{"name": "boot-prom",
               "kind": _BOOT_PROM_KINDS[topology["generation"]],
               "required": True}]
    if any(item["initial"] == "io-prom" for item in storage):
        inputs.append({"name": "io-prom", "kind": "io6-prom",
                       "required": True})
    return inputs


def _resources_for(topology: dict, harness: dict, product: dict,
                   dimms_per_node: tuple[int, ...],
                   nodes: int,
                   population_nodes: tuple[int, ...]) -> list[dict]:
    """The catalogue's `resources` list for one offering: identity and SPD
    inputs a launcher may supply beyond its writable `storage`.
    """
    resources = []
    def ds1742_board_survives(board: dict) -> bool:
        return (board.get("ds1742") is True and
                _board_node(topology, board) in population_nodes)

    ds1742_board_ids = sorted(
        board["id"] for board in topology["io_boards"]
        if ds1742_board_survives(board)
    )
    if ds1742_board_ids:
        if (len(set(ds1742_board_ids)) != len(ds1742_board_ids) or
                any(not re.fullmatch(r"[a-z][a-z0-9-]*", board_id)
                    for board_id in ds1742_board_ids)):
            raise ValueError("DS1742 resource owners must be unique stable board IDs")
        resources.append({
            "kind": "ds1742", "scope": "board",
            "count": len(ds1742_board_ids), "board-ids": ds1742_board_ids,
            "argument": _DS1742_ARGUMENT, "part": "ds1742",
            "size": _DS1742_BYTES, "clock-reference": True,
        })
    if harness.get("requires_per_node_flash"):
        resources.append({
            "kind": "boot-flash", "scope": "node", "count": nodes,
            "size": topology["nodes"][0]["flash"]["size"],
            "argument": _PFLASH_ARGUMENT,
        })
    elif harness.get("has_managed_boot_flash"):
        resources.append({
            "kind": "boot-flash", "scope": "board", "count": nodes,
            "size": product["boot_flash_bytes"],
            "argument": _PFLASH_ARGUMENT,
        })
    # Origin 300's raw identity and SPD record overrides: the machine's
    # chassis-eeprom and board-eeprom arrays (one pair per board: the node
    # board and the IO-8) and spd-eeprom (one record per fitted DIMM).
    # Nothing records a byte size for these chips, so this catalogue does
    # not invent one. Each chassis's L1 reads
    # its own pair, so a two-chassis topology has twice as many. The L1
    # count stands in for the chassis count: every Origin 300 chassis has
    # exactly one L1, and no other controller kind carries these inputs.
    identity_inputs = harness.get("identity_inputs", ())
    chassis = max(1, sum(1 for controller in topology["controllers"]
                         if controller["kind"] == "L1"))
    if "chassis-eeprom" in identity_inputs:
        resources.append({
            "kind": "identity-eeprom", "scope": "board",
            "count": len(harness["identity_record_slots"]) * chassis,
            "size": None, "argument": "chassis-eeprom.N=",
        })
    if "board-eeprom" in identity_inputs:
        resources.append({
            "kind": "identity-eeprom", "scope": "board",
            "count": len(harness["identity_record_slots"]) * chassis,
            "size": None, "argument": "board-eeprom.N=",
        })
    if "spd-eeprom" in identity_inputs:
        resources.append({
            "kind": "spd-eeprom", "scope": "dimm",
            "count": sum(dimms_per_node),
            "size": None, "argument": "spd-eeprom.N=",
        })
    return resources


def _hardware_inputs(topology: dict) -> list[dict]:
    """Launch inputs this configuration binds by name. Overrides of the
    catalogue's own identity and board values are listed separately, in
    `_overrides`.
    """
    bindings = []
    for controller in topology["controllers"]:
        record = controller.get("l1", {})
        if "mac_eeprom_input" in record:
            bindings.append(dict(record["mac_eeprom_input"]))
    return sorted(bindings, key=lambda entry: entry["option"])


# The -cpu properties each processor part takes for the values the
# catalogue describes (target/mips/r10k_cpu.c); the machine applies the
# catalogue's values through them unless -cpu names one.
_CPU_OVERRIDES = {
    "R10000": ("r10000-prid",),
    "R12000": ("r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
               "r12000-scache-bytes", "r12000-scache-block-words"),
    "R14000": ("r14000-prid",),
    "R16000": ("r16000-experimental", "r16000-reset-mode",
               "r16000-scache-bytes"),
}


def _overrides(topology: dict, part: str) -> list[dict]:
    """Every catalogue identity or board value this configuration lets a
    launch override, and where: a `-M` property, or a `-cpu` property of
    its processor part @part. The machine refuses a board override its
    configuration does not bind, so each is listed only where one is."""
    machine = ["mac"]
    if any(node["asic_kind"] == "BEDROCK" for node in topology["nodes"]):
        machine += ["board-id-word", "bedrock-revision"]
    if any("l1" in controller for controller in topology["controllers"]):
        machine += ["l1-type-code", "l1-revision"]
    if any(board["board_kind"] in ("IO8", "FUEL") and board.get("populated")
           for board in topology["io_boards"]):
        machine += ["ioc3-subsystem-id"]
    # The Origin 2000's plain BaseIO card may be built as the BaseIO-G
    # instead (baseio-preset); a topology whose own record is the BaseIO-G
    # builds that whatever the property says, so it is not offered there.
    if any(board["board_kind"] == "BASEIO" and board.get("populated", True)
           for board in topology["io_boards"]):
        machine += ["baseio-preset"]
    if topology["generation"] == "IP30":
        machine += ["flash-select", "flash-strap"]
    return ([{"property": name, "target": "machine"}
             for name in sorted(machine)] +
            [{"property": name, "target": "cpu"}
             for name in _CPU_OVERRIDES[part]])


def _machine_options(machine_name: str, topology: dict, nodes: int,
                     processor: str | None = None) -> str:
    """The `-M` value selecting one offering: product, topology and node
    count, the same three for every machine, and the processor module
    where it is not the topology's default."""
    options = f"{machine_name},topology={topology['name']},nodes={nodes}"
    return options if processor is None else f"{options},processor={processor}"


def _processor_choices(topology: dict) -> list[dict]:
    """Every processor a launch of @topology may select, the default first,
    each with the per-node memory totals and processor counts it allows.
    A topology with no processor options offers only its nodes' own part.
    """
    if "processor_options" in topology:
        return [{**option, "default": index == 0}
                for index, option in enumerate(topology["processor_options"])]
    node = topology["nodes"][0]
    return [{"assembly": node["cpu_slots"][0]["assembly"],
             "accepted_memory_mib": node["accepted_memory_mib"],
             "cpus_per_node": None, "default": True}]


_IP53_NORMAL_OFFERINGS = {
    ("origin350", "origin350", (4,)): (2048, 700_000_000, 8, 0x009cad85),
    ("onyx350", "onyx350-compat", (4,)): (8192, 800_000_000, 8, 0x009caf85),
    ("tezro", "tezro-tower-one", (1,)): (4096, 700_000_000, 4, 0x009bad85),
    ("tezro", "tezro-tower-multi", (4,)): (4096, 700_000_000, 4, 0x009bad85),
    ("tezro", "tezro-rack-base", (1,)): (4096, 700_000_000, 4, 0x009bad85),
    ("onyx4-host", "onyx4-host-serial-compat", (4,)):
        (8192, 1_000_000_000, 16, 0x009dad85),
}
_IP53_REQUIRED_CPU_PROPERTIES = [
    "r16000-experimental", "r16000-reset-mode", "r16000-scache-bytes",
]


def _is_ip53_normal_offering(
    product: str, topology: str, cpu_population: tuple[int, ...],
    default_memory: tuple[int, ...], accepted_memory: list[int],
    assembly: dict,
) -> bool:
    """Whether this exact approved IP53 offering needs no caller CPU flags."""
    expected = _IP53_NORMAL_OFFERINGS.get(
        (product, topology, tuple(cpu_population)))
    if expected is None:
        return False
    memory_mib, clock_hz, cache_mib, mode_bits = expected
    return (
        default_memory == (memory_mib,)
        and accepted_memory == [memory_mib]
        and assembly.get("kind") == "R16000"
        and assembly.get("clock_hz") == clock_hz
        and assembly.get("cache_mib") == cache_mib
        and assembly.get("mode_bits") == mode_bits
        and assembly.get("required_properties") ==
            _IP53_REQUIRED_CPU_PROPERTIES
    )


def _is_ip53_runtime_profile(model: dict) -> bool:
    product = model.get("composition", {}).get("product")
    emulation = model.get("machine", {}).get("emulation", {})
    expected = {
        "origin350": ("origin350", "ip53-origin350"),
        "onyx4-host-serial-compat": (
            "onyx4-host", "ip53-onyx4-host-serial-compat"),
        "onyx350-compat": ("onyx350", "ip53-onyx350-compat"),
        "tezro-deskside-700mhz-4mib-serial-trial": (
            "tezro", "tezro-deskside-700mhz-4mib-serial-trial"),
        "tezro-deskside-compat": (
            "tezro", "ip53-tezro-deskside-compat"),
        "tezro-tower-one": (
            "tezro", "tezro-tower-one-serial-compat"),
        "tezro-tower-multi": (
            "tezro", "tezro-tower-multi-serial-compat"),
        "tezro-rack-base": (
            "tezro", "tezro-rack-base-serial-compat"),
    }.get(product)
    return (expected is not None
            and emulation.get("machine_name") == expected[0]
            and emulation.get("profile") == expected[1]
            and model.get("machine", {}).get("harness", {}).get(
                "launch_recipe") == "ip53-discovery")


def _validate_ip53_runtime_consoles(model: dict, topology: dict) -> None:
    harness = model["machine"].get("harness", {})
    records = harness.get("console_records")
    policy = harness.get("console")
    if (policy not in ("l1-uart", "ip53-ioc4")
            or len(topology["nodes"]) != 1
            or not isinstance(records, list)
            or len(records) != 5
            or any(not isinstance(record, dict) for record in records)
            or any(record.get("node_id") != topology["nodes"][0]["id"]
                   for record in records)):
        raise ValueError(
            "IP53 runtime profile requires five ordered single-node serial bindings"
        )
    l1 = ("l1", "-serial chardev:l1")
    ports = [(f"ioc4-serial-{index}",
              f"-serial chardev:ioc4_serial_{index}") for index in range(4)]
    if policy == "l1-uart":
        expected = [("console", *l1)] + [
            ("second-port", name, argument) for name, argument in ports]
        candidates = [expected]
    else:
        candidates = [[("system-controller", *l1)] + [
            ("console" if index == primary else "second-port", name, argument)
            for index, (name, argument) in enumerate(ports)
        ] for primary in range(4)]
    actual = [(record.get("kind"), record.get("launcher_name"),
               record.get("argument")) for record in records]
    if actual not in candidates:
        raise ValueError(
            "IP53 runtime profile requires five ordered single-node serial bindings"
        )


def _validate_export_profiles(model: dict, topology: dict) -> None:
    hosts = topology.get("pci_host_bridges", [])
    experimental = any(
        host.get("compatibility_profile") == "pic-rev3-bedrock-experimental"
        for host in hosts)
    tezro_experimental = any(
        host.get("compatibility_profile") ==
        "pic-rev3-bedrock-tezro-experimental" for host in hosts)
    if experimental and not _is_ip53_runtime_profile(model):
        raise CompileError(
            "experimental PIC profile requires a registered IP53 profile")
    if tezro_experimental and not _is_ip53_runtime_profile(model):
        raise CompileError(
            "Tezro experimental PIC profile requires a registered IP53 profile")
    if any("compatibility_profile" in host
           and host.get("compatibility_profile") not in {
               "pic-rev3-bedrock-experimental",
               "pic-rev3-bedrock-tezro-experimental",
           } for host in hosts):
        raise CompileError("qtest-only PIC profile cannot be exported as a product")


def _offerings_for(model: dict, topology: dict,
                   product_models: dict[str, dict]) -> list[dict]:
    """One offering per node-population row and accepted CPU population
    this compiled topology names, the largest row first, since that is
    what the machine fits by default. Each carries its product's harness
    facts (`_product_facts`) plus what the compiled topology and its own
    resolved model already carry -- never a second, hand-authored copy of
    any of them. A row graded unknown is left out: QEMU refuses it.
    """
    configuration_name = topology["name"]
    machine_name = model["machine"]["emulation"]["machine_name"]
    product = _product_facts(machine_name, product_models)
    harness = product["harness"]
    # Launch preparation and endpoint records describe this selected model's
    # authored runtime inputs. Product facts remain the source for defaults.
    model_harness = model["machine"].get("harness", {})
    launch_harness = dict(harness)
    for field in ("launch_recipe", "console_records"):
        if field in model_harness:
            launch_harness[field] = model_harness[field]
    if model_harness.get("launch_recipe") == "ip53-discovery":
        if "console" not in model_harness:
            raise ValueError("ip53-discovery requires an explicit console policy")
        launch_harness["console"] = model_harness["console"]
    experiment = _is_ip53_runtime_profile(model)
    if experiment:
        _validate_ip53_runtime_consoles(model, topology)
    population = model["composition"]["population"]
    dimms_per_node = tuple(population["dimms_per_node"])
    node = topology["nodes"][0]
    default_memory = tuple(population["memory_mib_per_node"])
    if len(set(default_memory)) != 1:
        raise ValueError(
            f"'{configuration_name}' has heterogeneous per-node memory "
            f"{default_memory}; the catalogue's offerings need one default"
        )
    scsi_adapters = _scsi_adapters_for(topology)
    launch_recipe = launch_harness.get("launch_recipe")
    if launch_recipe not in (None, "ip53-discovery"):
        raise ValueError(f"unsupported offering launch recipe {launch_recipe!r}")
    if launch_recipe is None and (
            "console_records" in launch_harness or
            model_harness.get("console") == "ip53-ioc4" or
            harness["console"] == "ip53-ioc4"):
        raise ValueError(
            "IP53 console policy and records require ip53-discovery"
        )
    if launch_recipe == "ip53-discovery" and launch_harness["console"] not in (
            "l1-uart", "ip53-ioc4"):
        raise ValueError(
            "ip53-discovery requires an explicit L1 or IOC4 console policy"
        )
    networks = _networks_for(topology)
    firmware = {
        "kind": {"SN0": "ip27-prom", "SN1": "ip35-prom", "IP30": "ip30-prom"}[
            topology["generation"]],
        "argument": product["firmware_argument"],
        "asset": product["firmware_asset"],
        "size": node["flash"]["size"],
        "sha256": product["firmware_sha256"],
        "per-node-flash": harness.get(
            "requires_per_node_flash",
            topology["generation"] in ("SN0", "SN1")),
    }
    identity_inputs = harness.get("identity_inputs", ())
    recipe = model["machine"].get("harness", {}).get(
        "smdk", harness.get("smdk"))
    diagnostic = {"smdk": _kebab_keys(recipe)} if recipe is not None else {}
    offerings = []
    rows = sorted((row for row in topology["node_populations"]
                   if row["grade"] != "UNKNOWN"),
                  key=lambda row: -len(row["nodes"]))
    for row in rows:
        nodes = len(row["nodes"])
        resources = _resources_for(topology, harness, product,
                                   dimms_per_node[:nodes], nodes,
                                   tuple(row["nodes"]))
        storage = _storage_for(topology, harness, product,
                               list(row["nodes"]))
        init_inputs = _init_inputs_for(topology, storage)
        consoles = (
            _ip53_console_records(topology, launch_harness, tuple(row["nodes"]))
            if launch_recipe == "ip53-discovery"
            else _consoles_for(topology, harness)
        )
        for choice, cpu_population in (
                (choice, cpu_population)
                for choice in _processor_choices(topology)
                for cpu_population in row["supported_cpu_populations"]
                if choice["cpus_per_node"] is None or
                set(cpu_population) <= set(choice["cpus_per_node"])):
            assembly = choice["assembly"]
            normal_ip53 = _is_ip53_normal_offering(
                machine_name, configuration_name, tuple(cpu_population),
                default_memory, choice["accepted_memory_mib"], assembly,
            )
            offerings.append({
                "product": machine_name,
                "topology": configuration_name,
                **({"default-topology": selected_default}
                   if (selected_default := default_topology(
                       machine_name, product_models)) is not None else {}),
                "display-name": model["machine"]["name"],
                "nodes": nodes,
                "machine-options": _machine_options(
                    machine_name, topology, nodes,
                    None if choice["default"] else assembly["id"]),
                "cpus-per-node": list(cpu_population),
                "dimms-per-node": list(dimms_per_node[:nodes]),
                "smp": sum(cpu_population),
                "memory-per-node-mib": {
                    "default": default_memory[0],
                    "accepted": list(choice["accepted_memory_mib"]),
                },
                "processor": {
                    "id": assembly["id"],
                    "part": assembly["kind"], "clock-hz": assembly["clock_hz"],
                    "cache-mib": assembly["cache_mib"],
                    **({"required-properties": list(assembly["required_properties"])}
                       if ("required_properties" in assembly and
                           not normal_ip53) else {}),
                },
                **({"firmware": firmware}
                   if launch_recipe == "ip53-discovery" else {}),
                "init-inputs": init_inputs,
                "storage": storage,
                "resources": resources,
                "scsi-adapters": scsi_adapters,
                "consoles": consoles,
                "networks": networks,
                "console": launch_harness["console"],
                **({"launch-recipe": launch_recipe}
                   if launch_recipe is not None else {}),
                "prom-dialogue": harness["prom_dialogue"],
                "disk-bus": harness["disk_bus"],
                "netboot-adapter": harness["netboot_adapter"],
                "identity-inputs": list(identity_inputs),
                "hardware-inputs": _hardware_inputs(topology),
                "overrides": _overrides(topology, assembly["kind"]),
                **diagnostic,
                "supported-guests": list(harness["supported_guests"]),
                "install-media": list(harness["install_media"]),
                "default-cpu-model": (harness["default_cpu_model"]
                                      if choice["default"]
                                      else assembly["kind"]),
                "synthesized-firmware-toggle": harness.get(
                    "has_synthesized_firmware_toggle", False),
                "needs-debug-leds-off": harness.get(
                    "needs_debug_leds_off", False),
            })
    return offerings


def render_catalogue_json(models: list[dict], *,
                          product_models: dict[str, dict],
                          references: list[dict] | None = None) -> str:
    """The compiled catalogue: `topologies` and `offerings`, one
    schema-versioned JSON document with no provenance in it. `models` is
    every emulation-target configuration's
    resolved system model, compiled here the same way
    `model_qemu.render_qemu_inputs` compiles them for the catalogue.
    """
    topologies = []
    offerings = []
    for model in models:
        topology = compile_model(model)
        _validate_export_profiles(model, topology)
        machine_name = model["machine"]["emulation"]["machine_name"]
        topologies.append(_render_topology(topology, machine_name))
        offerings.extend(_offerings_for(model, topology, product_models))
    catalogue = {
        "schema": "sgi-machines",
        "version": 1,
        # QAPI's optional-member convention: an absent key, never a JSON
        # null, so both sections drop their None values; the harness reads
        # them with .get().
        "topologies": [_strip_none(topology) for topology in topologies],
        "offerings": [_strip_none(offering) for offering in offerings],
        "references": sorted(references or [], key=lambda row: row["name"]),
    }
    return json.dumps(catalogue, indent=2, sort_keys=True) + "\n"
