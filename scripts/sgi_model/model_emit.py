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

from .model_compile import compile_model


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
                           ("widget_revision", "widget-revision")):
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
            "accepted-bank-layouts": _kebab_keys(raw["accepted_bank_layouts"]),
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
    if "bedrock" in raw:
        node["bedrock"] = _kebab_keys(raw["bedrock"])
    return node


def _io_peer(raw: dict) -> dict:
    return {
        "id": raw["id"], "module-index": raw["module_index"], "slot": raw["slot"],
        "asic-kind": _enum(raw["asic_kind"]), "identity": _identity(raw["identity"]),
    }



# A Router's port count is fixed by the chip (six ports on every fitted
# Router this catalogue names) and unread by the builder already; dropped.
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
        "attachment": raw["slot_index"],
        "populated": raw.get("populated", True),
        "other-identity-parts": raw["other_identities"],
    }
    if raw.get("label") is not None:
        board["label"] = raw["label"]
    for name, key in (("part", "part"), ("revision", "revision"),
                      ("serial", "serial"), ("ioc3-serial", "ioc3_serial")):
        if raw[key] is not None:
            board[name] = raw[key]
    if "system_timekeeper" in raw:
        board["system-timekeeper"] = _kebab_keys(raw["system_timekeeper"])

    for key in ("ioc3_subsystem_id", "mac_source"):
        if key in raw:
            board[key.replace("_", "-")] = _kebab_keys(raw[key])
    if "pci_functions" in raw:
        board["pci-functions"] = [{"kind": _enum(entry["kind"]),
                                   "revision": entry["revision"]}
                                  for entry in raw["pci_functions"]]
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
    `product` names which product this topology belongs to -- not a
    compiled field, but the fact `sgi_model_default_topology()` needs to
    pick a product's default topology out of the catalogue at load time,
    the same structural rule this module's own default-topology lookup
    used to apply at compile time.
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
# one UART instead (the model tooling/prom.py's `behind_l1` branch: one
# `-serial chardev:l1`).

# The m48t35 Timekeeper's NVRAM window and the clock reference record its
# model keeps beside it (hw/rtc/m48t35.h's M48T35_SIZE and
# M48T35_CLOCK_REF_BYTES); IP30's DS1685-family NVRAM and its clock
# record (hw/rtc/ds1685.h's DS1685_CLOCK_REF_BYTES).
_TIMEKEEPER_NVRAM_BYTES = 0x8000
_TIMEKEEPER_CLOCK_BYTES = 16
_IP30_NVRAM_BYTES = 512
_IP30_CLOCK_BYTES = 24
# The BASEIO PROM flash part (hw/mips/sgi/baseio-flash.c), on the boards
# that fit one.
_IO_FLASH_BYTES = 0x100000
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
                     "argument": "-serial chardev:l1"}]
        ports = harness.get("ioc3_serial_ports",
                            ["ioc3-serial-a"]
                            if harness["console"] == "ioc3-serial-a" else [])
        for port in sorted(ports):
            consoles.append({
                "kind": ("console" if harness["console"] == port
                         else "second-port"),
                "node": 0,
                "argument": f"-serial chardev:ioc3_{port[-1]}",
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


def _networks_for(topology: dict) -> list[dict]:
    """One `ioc3-ethernet` entry per IOC3 that fits an Ethernet MAC. Most
    IOC3s carry onboard Ethernet, but a BaseIO-G MediaIO daughtercard's
    second IOC3 does not, so the offering follows the compiled `ethernet`
    population bit rather than the bare IOC3 kind (`model_compile`'s
    `_board_pci_population`). `the model tooling/irix.py`'s
    `IRIX_NETWORK_ADAPTERS["ef"]` wires the onboard adapter with no
    explicit `-device` of its own, only a `-netdev ...,id=netN` backend.
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


def default_topology(machine_name: str, product_models: dict[str, dict]) -> str:
    """The topology QEMU builds for a product when no topology= is given:
    the product of its base composition (`model_inputs.MACHINE_FILENAMES`),
    whatever further topologies the product compiles.
    """
    return product_models[machine_name]["composition"]["product"]


def _product_facts(machine_name: str, product_models: dict[str, dict]) -> dict:
    """The harness-policy facts for one runtime product, resolved from its
    own base model-input file through the product-model registry. A
    topology that shares this product's name but carries no
    `machine.harness` block of its own (Origin 200's dual-module form)
    borrows these facts rather than going without.
    """
    machine = product_models[machine_name]["machine"]
    firmware_payload = machine["emulation"].get("firmware_payload")
    second_selection = (
        firmware_payload.get("second_selection") if firmware_payload else None
    )
    boot_flash_bytes = (
        2 * int(second_selection["size"], 0) if second_selection else None
    )
    return {
        "harness": machine["harness"],
        "boot_flash_bytes": boot_flash_bytes,
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


def _storage_for(topology: dict, harness: dict, product: dict,
                 fitted: list[int]) -> list[dict]:
    """Every writable persistent store a launch fitting the `fitted` node
    boards has, numbered as the machine numbers what it builds, in the
    order a launcher creates them: each node's boot flash, each BASEIO
    PROM flash, then each Timekeeper NVRAM with its clock record. The
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
    if generation == "IP30":
        nvram_sizes = [(_IP30_NVRAM_BYTES, _IP30_CLOCK_BYTES, "zero")]
    else:
        nvram_sizes = [(_TIMEKEEPER_NVRAM_BYTES, _TIMEKEEPER_CLOCK_BYTES,
                        _timekeeper_initial(board))
                       for board in boards
                       if _timekeeper_board(board, harness)]
    for index, (size, clock_size, initial) in enumerate(nvram_sizes):
        storage.append({"name": f"nvram{index}", "size": size,
                        "read-only": False, "initial": initial})
        storage.append({"name": f"nvram{index}-clock", "size": clock_size,
                        "read-only": False, "initial": "zero"})
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


def _resources_for(topology: dict, harness: dict,
                   dimms_per_node: tuple[int, ...]) -> list[dict]:
    """The catalogue's `resources` list for one offering: identity and SPD
    inputs a launcher may supply beyond its writable `storage`.
    """
    resources = []
    # Origin 300's identity and SPD inputs -- the model tooling/prom.py's
    # ORIGIN300_EEPROM_INPUTS (two chassis EEPROMs and two board EEPROMs,
    # one pair per board: the node board and the IO-8) and
    # ORIGIN300_SPD_INPUTS (one record per fitted DIMM). Neither module
    # carries a byte size for these chips; this catalogue does not invent
    # one (see the packet report's open question). Each chassis's L1 reads
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
    """Launch inputs this configuration binds by name. The family overrides
    of catalogue board values (board-id-word and the rest) are machine
    properties every SN product accepts, so they are not listed here.
    """
    bindings = []
    for controller in topology["controllers"]:
        record = controller.get("l1", {})
        if "mac_eeprom_input" in record:
            bindings.append(dict(record["mac_eeprom_input"]))
    return sorted(bindings, key=lambda entry: entry["option"])


def _machine_options(machine_name: str, topology: dict, nodes: int) -> str:
    """The `-M` value selecting one offering. Octane builds its one HEART
    node board without a node count, so it has no nodes= property and
    takes only the topology."""
    options = f"{machine_name},topology={topology['name']}"
    if topology["generation"] == "IP30":
        return options
    return f"{options},nodes={nodes}"


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
    population = model["composition"]["population"]
    dimms_per_node = tuple(population["dimms_per_node"])
    node = topology["nodes"][0]
    assembly = node["cpu_slots"][0]["assembly"]
    default_memory = tuple(population["memory_mib_per_node"])
    if len(set(default_memory)) != 1:
        raise ValueError(
            f"'{configuration_name}' has heterogeneous per-node memory "
            f"{default_memory}; the catalogue's offerings need one default"
        )
    scsi_adapters = _scsi_adapters_for(topology)
    consoles = _consoles_for(topology, harness)
    networks = _networks_for(topology)
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
        resources = _resources_for(topology, harness, dimms_per_node[:nodes])
        storage = _storage_for(topology, harness, product,
                               list(row["nodes"]))
        init_inputs = _init_inputs_for(topology, storage)
        for cpu_population in row["supported_cpu_populations"]:
            offerings.append({
                "product": machine_name,
                "topology": configuration_name,
                "default-topology": default_topology(machine_name, product_models),
                "display-name": model["machine"]["name"],
                "nodes": nodes,
                "machine-options": _machine_options(
                    machine_name, topology, nodes),
                "cpus-per-node": list(cpu_population),
                "dimms-per-node": list(dimms_per_node[:nodes]),
                "smp": sum(cpu_population),
                "memory-per-node-mib": {
                    "default": default_memory[0],
                    "accepted": list(node["accepted_memory_mib"]),
                },
                "processor": {
                    "part": assembly["kind"], "clock-hz": assembly["clock_hz"],
                    "cache-mib": assembly["cache_mib"],
                },
                "init-inputs": init_inputs,
                "storage": storage,
                "resources": resources,
                "scsi-adapters": scsi_adapters,
                "consoles": consoles,
                "networks": networks,
                "console": harness["console"],
                "prom-dialogue": harness["prom_dialogue"],
                "disk-bus": harness["disk_bus"],
                "netboot-adapter": harness["netboot_adapter"],
                "identity-inputs": list(identity_inputs),
                "hardware-inputs": _hardware_inputs(topology),
                **diagnostic,
                "supported-guests": list(harness["supported_guests"]),
                "install-media": list(harness["install_media"]),
                "default-cpu-model": harness["default_cpu_model"],
                "synthesized-firmware-toggle": harness.get(
                    "has_synthesized_firmware_toggle", False),
                "needs-debug-leds-off": harness.get(
                    "needs_debug_leds_off", False),
            })
    return offerings


def render_catalogue_json(models: list[dict], *,
                          product_models: dict[str, dict]) -> str:
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
    }
    return json.dumps(catalogue, indent=2, sort_keys=True) + "\n"
