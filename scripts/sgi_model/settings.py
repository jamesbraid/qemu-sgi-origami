# SPDX-License-Identifier: BSD-3-Clause

"""The machine inputs a launch may give, written once for the catalogue.

SETTING_DEFINITIONS is the one table of what each setting is: its kind,
the QEMU property it sets and the limits on its value. Kinds and targets
describe QEMU properties, so the table lives beside the code rather than
in the model files, where it could drift from seventeen products.
`offering_settings` then says which of them one offering offers, under
which slot keys, and with what default.
"""

from __future__ import annotations

_TEXT = r"[!-+--~]+"          # printable, no space or comma
_L1_REVISION = r"[0-9]+\.[0-9]+\.[0-9]+"


def _definition(name: str, kind: str, target_kind: str, property_name: str,
                form: str, description: str, *, device: str | None = None,
                identity: bool = False, allow_empty: bool = False,
                **limits) -> dict:
    target = {"kind": target_kind, "property": property_name, "form": form}
    if device is not None:
        target["device"] = device
    return {"name": name, "kind": kind, "target": target,
            "allow-empty": allow_empty, "identity": identity,
            "description": description, **limits}


SETTING_DEFINITIONS: list[dict] = [
    _definition("chassis-eeprom", "file", "machine", "chassis-eeprom", "list",
                "A captured chassis record for one board's one-wire part.",
                identity=True),
    _definition("board-eeprom", "file", "machine", "board-eeprom", "list",
                "A captured board record for one board's one-wire part.",
                identity=True),
    _definition("spd-eeprom", "file", "machine", "spd-eeprom", "list",
                "A captured SPD record for one DIMM; an empty value means "
                "no record.", allow_empty=True),
    _definition("fuel-mac-eeprom", "file", "machine", "fuel-mac-eeprom",
                "plain", "The Fuel L1's station address image.",
                identity=True, size=512),
    _definition("controller-board-eeprom", "file", "machine",
                "controller-board-eeprom", "keyed-list",
                "A captured board record for one component of an "
                "expansion controller, keyed CONTROLLER/COMPONENT.",
                identity=True),
    _definition("controller-brick-serial", "text", "machine",
                "controller-brick-serial", "keyed-list",
                "The brick serial one expansion controller reports; it "
                "beats brick-serial for that controller.",
                identity=True, **{"min-length": 1, "max-length": 32,
                                  "pattern": _TEXT}),
    _definition("brick-serial", "text", "global", "brick-serial", "plain",
                "The brick serial every system controller reports.",
                device="sgi-l1", identity=True,
                **{"min-length": 1, "max-length": 32, "pattern": _TEXT}),
    _definition("debug-switches", "integer", "global", "debug-switches",
                "plain", "The L1's debug switch word.", device="sgi-l1",
                minimum=0, maximum=65535),
    _definition("origin300-l1cmd-compat", "boolean", "machine",
                "origin300-l1cmd-compat", "plain",
                "Accept the L1 commands the model marks synthetic."),
    _definition("board-id-word", "integer", "machine", "board-id-word",
                "plain", "The Bedrock board identity response.",
                minimum=0, maximum=(1 << 64) - 1),
    _definition("bedrock-revision", "integer", "machine", "bedrock-revision",
                "plain", "The Bedrock revision.", minimum=0, maximum=255),
    _definition("l1-type-code", "integer", "machine", "l1-type-code",
                "plain", "The L1's reported type character.",
                minimum=0, maximum=255),
    _definition("l1-revision", "text", "machine", "l1-revision", "plain",
                "The L1's reported firmware revision, MAJOR.MINOR.PATCH.",
                **{"min-length": 5, "max-length": 32,
                   "pattern": _L1_REVISION}),
    _definition("ioc3-subsystem-id", "integer", "machine",
                "ioc3-subsystem-id", "plain",
                "The IOC3 PCI subsystem id.", minimum=0, maximum=65535),
    _definition("flash-select", "boolean", "machine", "flash-select",
                "plain", "The Octane board's flash select input."),
    _definition("flash-strap", "boolean", "machine", "flash-strap",
                "plain", "The Octane board's flash strap input."),
    _definition("prid", "integer", "cpu", "prid", "plain",
                "The processor revision register of the offering's part "
                "(the part's own -cpu property: r10000-prid, r12000-prid, "
                "r14000-prid or r16000-prid).",
                minimum=0, maximum=(1 << 32) - 1),
    _definition("fpu-id", "integer", "cpu", "fpu-id", "plain",
                "The FPU revision register of the offering's part (the "
                "-cpu property r12000-fpu-id).",
                minimum=0, maximum=(1 << 32) - 1),
    _definition("password-clear-input", "enum", "machine",
                "password-clear-input", "plain",
                "The O2 board's password clear input, whose fitted value "
                "is unknown.", values=["0", "1"]),
    _definition("rom-at29c040a-protection", "enum", "machine",
                "rom-at29c040a-protection", "plain",
                "The O2 ROM part's software data protection at power-up, "
                "whose value is unknown.", values=["disabled", "enabled"]),
    _definition("rom-at29c040a-lower-lock", "enum", "machine",
                "rom-at29c040a-lower-lock", "plain",
                "The O2 ROM part's lower boot block lock at power-up, "
                "whose state is unknown.", values=["unlocked", "locked"]),
    _definition("rom-at29c040a-upper-lock", "enum", "machine",
                "rom-at29c040a-upper-lock", "plain",
                "The O2 ROM part's upper boot block lock at power-up, "
                "whose state is unknown.", values=["unlocked", "locked"]),
    _definition("identity-eprom-file", "file", "machine",
                "identity-eprom-file", "plain",
                "A captured O2 identity chip image.", identity=True,
                size=128),
    _definition("dip-switches", "integer", "machine", "elsc-dip-switches",
                "plain", "The system controller's DIP switch word, as "
                "asserted bits.", minimum=0, maximum=255),
    _definition("junk-uart", "boolean", "machine", "junk-uart", "plain",
                "A debug input: fit the Hub's junk-bus UART, a bring-up "
                "serial port no shipped machine is known to have."),
]

# The firmware's own names for the switch values; podlib-style lookups OR
# the names a launch lists.
DIP_SWITCH_NAMES = {
    "no-diag": 0x01, "heavy-diag": 0x02, "mfg-diag": 0x03, "verbose": 0x04,
    "global-pod": 0x08, "local-pod": 0x10, "no-mem-pod": 0x18,
    "default-env": 0x20, "bypass-io6": 0x40, "no-global-master": 0x80,
}

# Where each default comes from, and how well that is known. The mark is
# the manual page's word for the fact, for the value this offering shows.
# DEFAULT_BASIS says where the value is read from; the settings page
# prints it. A default whose value has no entry here is refused, so a new
# default needs its basis written down or must be left out.
DEFAULT_BASIS = {
    "board-id-word": (
        "The topology's :term:`Bedrock` record in the model file, or for an Origin "
        "3000 offering the board-id-word of the processor option it "
        "selects. The C-brick words are marked in "
        "hardware/bricks.rst; the other products' in the board identity "
        "inputs of ip35.rst and ip53.rst, whose product-code bits are a "
        "software expectation and whose low bits are a model choice."),
    "prid": (
        "The processor assembly in the model file, for the part revision "
        "the product's inventory prints. The Origin 3000's R14000 values "
        "are marked in hardware/bricks.rst; no product's raw PRId was "
        "read."),
    "fpu-id": (
        "The processor assembly in the model file, for the FPU revision "
        "the product's inventory prints; not a raw read."),
    "flash-select": (
        "The Octane board's flash record in the model file, a model "
        "choice; see ip30.rst."),
    "flash-strap": (
        "The Octane board's flash record in the model file, a model "
        "choice; see ip30.rst."),
    "debug-switches": (
        "The sgi-l1 device's own default; the stock :term:`PROM`'s read of the "
        "word is marked in hardware/l1.rst."),
    "origin300-l1cmd-compat": (
        "The machine property's own default, off; the commands it "
        "enables are an approximation (hardware/l1.rst)."),
}


def default_provenance(product: str, name: str, value: str) -> str:
    """How well @value is known as @product's default for setting @name:
    established, inferred, approximated, per DEFAULT_BASIS."""
    if name == "board-id-word":
        if product == "origin3000":
            # The 400 MHz row (2) and the 500 and 800 MHz PIMMs (1, 5) are
            # model choices; the 600 MHz PIMM's 3 is inferred.
            return {"1": "approximated", "2": "approximated",
                    "3": "inferred", "5": "approximated"}[value]
        return "approximated"
    if name == "prid" and product == "origin3000":
        # The R14000 revisions are established for the part; the R12000's
        # follows from the printed revision.
        return {"0xf14": "established", "0xf24": "established",
                "0xe35": "inferred"}[value]
    if name in ("prid", "fpu-id"):
        return "inferred"
    if name in ("flash-select", "flash-strap", "debug-switches",
                "origin300-l1cmd-compat"):
        return "approximated"
    raise KeyError(f"no basis for the default of {name} on {product}")


_BEDROCK = ("board-id-word", "bedrock-revision", "l1-type-code",
            "l1-revision")


def _slots(scope: str, keys: list[str], labels: list[str]) -> dict:
    return {"scope": scope, "keys": keys, "labels": labels}


def _entry(name: str, **fields) -> dict:
    return {"name": name, "required": fields.pop("required", False),
            **fields}


def _identity_records(offering: dict) -> list[dict]:
    """chassis-eeprom and board-eeprom, keyed by the offering's records."""
    count = max((r["count"] for r in offering["resources"]
                 if r["kind"] == "identity-eeprom"), default=0)
    if not count and offering["product"] in ("origin350", "onyx350",
                                             "onyx4", "tezro"):
        # An IP53 node board's L1 takes the records too, the one board's.
        count = 1
    if not count:
        return []
    keys = [str(i) for i in range(count)]
    if count == 1:
        labels = ["node 0 node-board"]
    else:
        labels = [f"node {i // 2} {'node-board' if i % 2 == 0 else 'io-board'}"
                  for i in range(count)]
    return [_entry("chassis-eeprom", slots=_slots("board", keys, labels)),
            _entry("board-eeprom", slots=_slots("board", keys, labels))]


def _spd_records(offering: dict) -> list[dict]:
    count = max((r["count"] for r in offering["resources"]
                 if r["kind"] == "spd-eeprom"), default=0)
    if not count:
        return []
    per_node = offering["dimms-per-node"][0]
    keys = [str(i) for i in range(count)]
    labels = [f"node {i // per_node} DIMM {i % per_node}"
              for i in range(count)]
    return [_entry("spd-eeprom", slots=_slots("dimm", keys, labels))]


def _expansion_controllers(topology: dict) -> list[int]:
    return [record["controller_index"]
            for record in topology.get("io_assemblies", [])
            if record.get("kind") == "ORIGIN300_PCI_EXPANSION"]


def offering_settings(offering: dict, topology: dict,
                      default_prid: int | None,
                      default_fpu: int | None,
                      option_board_id: int | None = None
                      ) -> tuple[list[dict], list[dict]]:
    """The settings and setting groups @offering offers, in definition
    order. @topology is the compiled record; the processor registers'
    defaults come from the offering's own processor, and the board
    identity word from the processor option it selects when that option
    carries one (a PIMM brings its own), else from the topology's first
    node."""
    product = offering["product"]
    part = offering["processor"]["part"]
    chosen: list[dict] = []
    groups: list[dict] = []

    def prid_entries() -> list[dict]:
        found = [_entry("prid", **({} if default_prid is None else
                                   {"default": f"{default_prid:#x}"}))]
        if part == "R12000":
            found.append(_entry("fpu-id", **(
                {} if default_fpu is None else
                {"default": f"{default_fpu:#x}"})))
        return found

    def node_board(name: str):
        if option_board_id is not None:
            return option_board_id
        slot = topology["nodes"][0]
        value = slot.get("bedrock", {}).get("board_id_word")
        return value if name == "board-id-word" else None

    if product == "origin200":
        chosen += prid_entries() + [_dip_switches(offering)]
    elif product in ("origin2000", "onyx2"):
        chosen += prid_entries() + [_dip_switches(offering)]
    elif product in ("origin300", "fuel"):
        board_id = node_board("board-id-word")
        chosen += [_entry("board-id-word", **(
            {} if board_id is None else {"default": str(board_id)}))]
        chosen += [_entry(name) for name in _BEDROCK[1:]]
        chosen += [_entry("ioc3-subsystem-id")]
        chosen += prid_entries() + _identity_records(offering)
        if product == "origin300":
            chosen += _spd_records(offering)
            chosen += [_entry("brick-serial"),
                       _entry("debug-switches", default="0"),
                       _entry("origin300-l1cmd-compat", default="off")]
            controllers = _expansion_controllers(topology)
            if controllers:
                keys = [str(c) for c in controllers]
                chosen += [
                    _entry("controller-brick-serial", slots=_slots(
                        "controller", keys,
                        [f"expansion controller {k}" for k in keys])),
                    _entry("controller-board-eeprom", slots=_slots(
                        "controller-component",
                        [f"{c}/{k}" for c in keys for k in range(4)],
                        [f"expansion controller {c} component {k}"
                         for c in keys for k in range(4)]))]
        else:
            chosen += [_entry("fuel-mac-eeprom")]
            for entry in chosen:
                if entry["name"] in ("chassis-eeprom", "board-eeprom"):
                    entry["group"] = "fuel-node-records"
            groups.append({"name": "fuel-node-records",
                           "members": ["chassis-eeprom", "board-eeprom"],
                           "rule": "all-or-none"})
    elif product == "origin3000":
        board_id = node_board("board-id-word")
        chosen += [_entry("board-id-word", **(
            {} if board_id is None else {"default": str(board_id)}))]
        chosen += [_entry(name) for name in _BEDROCK[1:]]
        chosen += prid_entries()
    elif product in ("origin350", "onyx350", "onyx4", "tezro"):
        board_id = node_board("board-id-word")
        chosen += [_entry("board-id-word", **(
            {} if board_id is None else {"default": str(board_id)}))]
        chosen += [_entry(name) for name in _BEDROCK[1:]]
        chosen += prid_entries() + _identity_records(offering)
    elif product in ("octane", "octane2"):
        flash = topology["nodes"][0]["flash"]
        for name in ("flash-select", "flash-strap"):
            chosen.append(_entry(
                name, default="on" if flash.get(name[6:]) else "off"))
        chosen += prid_entries()
    elif product == "o2":
        chosen += [_entry("password-clear-input", required=True),
                   _entry("rom-at29c040a-protection", required=True),
                   _entry("rom-at29c040a-lower-lock", required=True),
                   _entry("rom-at29c040a-upper-lock", required=True),
                   _entry("identity-eprom-file")]
    if topology["junk_uart"] != -1:
        # Off unless a launch fits it, so there is no default to mark.
        chosen.append(_entry("junk-uart"))
    for entry in chosen:
        if "default" in entry:
            entry["default-provenance"] = default_provenance(
                product, entry["name"], entry["default"])
    return chosen, groups


def _dip_switches(offering: dict) -> dict:
    pod = 0x08 if offering["nodes"] > 1 else 0x10
    choices = [{"name": name, "value": value,
                "description": f"the firmware's {name} switch"}
               for name, value in DIP_SWITCH_NAMES.items()]
    choices += [
        {"name": "pod", "value": pod, "description":
         "power-on diagnostics mode: " + (
             "global POD, which a multi-node machine arbitrates across its "
             "modules" if pod == 0x08 else "local POD, which a single-node "
             "machine arbitrates itself")},
        {"name": "skip-prom-diags", "value": 0x01,
         "description": "skip the PROM's power-on diagnostics"}]
    return _entry("dip-switches", choices=choices)
