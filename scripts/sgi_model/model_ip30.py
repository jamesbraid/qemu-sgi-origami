# SPDX-License-Identifier: BSD-3-Clause
"""Compile the selected IP30 populations without borrowing SN node facts."""

import itertools

from . import synthetic_serial
from .model_shared import _integer, _required, _validate_integer, num

# R12000 Config fields (NEC VR10000 Series User's Manual, U10278EJ4V0UM00,
# Config register): BE bit 15, SS bits 18:16, SB bit 13.
R12000_CONFIG_BE = 1 << 15
R12000_CONFIG_SS_SHIFT = 16
R12000_CONFIG_SB = 1 << 13
BOARD_ROLES = ("processor", "system-board", "frontplane", "power-supply")
# Bank capacities a HEART descriptor's size field can name, 32 MiB units
# up to 2 GiB, that a matched DIMM pair makes: two 32 MB DIMMs at least.
HEART_BANK_MIB = (64, 128, 256, 512, 1024, 2048)
HEART_BANKS = 4


def _bank_layout_accepted(banks, sizes, descending):
    """Whether @banks fills from bank 0 without a gap, every populated
    bank is one of @sizes, and, if @descending, no bank is larger than
    the one before it.
    """
    populated = [size for size in banks if size]
    return (len(banks) == HEART_BANKS and populated and
            banks[:len(populated)] == populated and
            all(size in sizes for size in populated) and
            (not descending or populated == sorted(populated, reverse=True)))


def _accepted_totals(sizes, descending):
    """Every total some accepted layout of one to four banks makes."""
    totals = set()
    for count in range(1, HEART_BANKS + 1):
        for row in itertools.product(sorted(sizes), repeat=count):
            row = list(row) + [0] * (HEART_BANKS - count)
            if _bank_layout_accepted(row, sizes, descending):
                totals.add(sum(row))
    return sorted(totals)


def _r12000_profile(model, cpu, processor):
    """The R12000 identity and reset word the machine gives its CPU unless
    the operator overrides them: PRId and FPU identification from the CPU
    component, the reset-mode word from the processors catalogue entry for
    the selected clock and cache, as R14000's is. Secondary-cache bytes
    and block words follow from that word's SS and SB fields, which must
    agree with the catalogued cache size.
    """
    from .model_compile import CompileError

    identity = cpu.get("identity", {})
    prid = num(_required(identity, "processor_identification", cpu["id"]))
    fpu_prid = num(_required(identity, "fpu_identification", cpu["id"]))
    if prid & ~0xffff or prid & 0xff00 != 0x0e00:
        raise CompileError("IP30 R12000 identity requires implementation 0x0e")
    _validate_integer(fpu_prid, 32, "IP30 R12000 FPU identification")
    clock_mhz = processor["clock_hz"] // 1000000
    variant = next((v for entry in model.get("processors", [])
                    if entry.get("part") == "r12000"
                    for v in entry.get("variants", [])
                    if v.get("clock_mhz") == clock_mhz
                    and v.get("cache_mib") == processor["cache_mib"]), None)
    if variant is None or "reset_mode_bits" not in variant:
        raise CompileError(
            f"no catalogued r12000 {clock_mhz} MHz/{processor['cache_mib']} "
            "MiB variant carries reset_mode_bits")
    mode = num(variant["reset_mode_bits"])
    if mode & ~0x01ffffff:
        raise CompileError("IP30 R12000 reset_mode_bits exceeds 25 bits")
    if not mode & R12000_CONFIG_BE:
        raise CompileError("IP30 R12000 reset_mode_bits must select big-endian")
    # SS encodes 512 KiB << SS: 1 MiB is 001, 2 MiB is 010.
    if ((mode >> R12000_CONFIG_SS_SHIFT) & 7 !=
            processor["cache_mib"].bit_length()):
        raise CompileError("IP30 R12000 reset_mode_bits SS disagrees with "
                           "the catalogued cache size")
    return {"prid": prid, "fpu_prid": fpu_prid, "mode_bits": mode}


def compile_heart_node(model, components, entry):
    from .model_compile import CompileError, _is_within

    heart = entry["component"]
    board = entry["board_id"]
    cpus = [c for c in components.values() if c["kind"] == "cpu"
            and _is_within(components, c, board)]
    population = model["composition"]["population"]
    supported = model["composition"]["topology"]["supported_cpu_populations"]
    if len(cpus) != 1 or population["cpus_per_node"] != [1] or supported != [[1]]:
        raise CompileError("IP30 supports only the selected single-processor population")
    cpu = cpus[0]
    identity = cpu.get("identity", {})
    part = _required(cpu, "hardware_kind", cpu["id"])
    if part not in ("r10000", "r12000"):
        raise CompileError(f"unsupported IP30 processor '{part}'")
    processor = {
        "id": cpu["id"], "kind": part.upper(),
        "clock_hz": _integer(identity, "clock_mhz", cpu["id"], 32) * 1000000,
        "cache_mib": _integer(identity, "cache_mib", cpu["id"], 16),
    }
    if part == "r12000":
        processor.update(_r12000_profile(model, cpu, processor))
    else:
        processor["prid"] = num(_required(identity, "processor_identification", cpu["id"]))
        if processor["prid"] & ~255 != 0x900:
            raise CompileError("IP30 R10000 identity requires implementation 0x09")

    banks = list(population["memory_bank_mib_per_node"][0])
    if len(banks) > 4:
        raise CompileError("HEART supports four selected memory banks")
    banks += [0] * (4 - len(banks))
    constraints = model["machine"]["emulation"]["emulator_constraints"]
    sizes = _required(constraints, "accepted_bank_sizes_mib",
                      "IP30 memory population")
    descending = constraints.get("bank_order", "any") == "descending"
    for size in sizes:
        if type(size) is not int or size not in HEART_BANK_MIB:
            raise CompileError("IP30 banks require a pair making 64, 128, "
                               "256, 512, 1024 or 2048 MiB")
    if not _bank_layout_accepted(banks, sizes, descending):
        raise CompileError("IP30 default bank population breaks the "
                           "selected bank rules")

    records = {}
    for component in components.values():
        if ("board_record" not in component or
                not _is_within(components, component, board)):
            continue
        record = dict(component["board_record"])
        role = record["role"]
        if role in records or role not in BOARD_ROLES:
            raise CompileError(f"invalid or repeated IP30 board record '{role}'")
        record["serial"] = num(record["serial"])
        _validate_integer(record["serial"], 48, "IP30 virtual board ROM serial")
        if (not synthetic_serial.onewire_serial_marked(record["serial"]) or
                not synthetic_serial.text_serial_marked(record["barcode"])):
            raise CompileError(
                f"IP30 board record '{role}' serial and barcode must carry "
                "the synthetic-serial marker")
        if any(other["serial"] == record["serial"] for other in records.values()):
            raise CompileError("IP30 virtual board ROM serials must be distinct")
        records[role] = record
    if set(records) != set(BOARD_ROLES):
        raise CompileError("IP30 requires processor, system-board, frontplane and power-supply records")
    fitted = _required(heart, "identity", heart["id"])
    ioc3s = [c for c in components.values() if c.get("hardware_kind") == "ioc3"
             and _is_within(components, c, board)]
    if len(ioc3s) != 1:
        raise CompileError("IP30 requires exactly one onboard IOC3")
    bridges = [c for c in components.values()
               if c.get("hardware_kind") == "bridge"
               and _is_within(components, c, board)]
    if len(bridges) != 1:
        raise CompileError("IP30 requires exactly one onboard Bridge")
    heart_identity = {
        "widget_revision": _integer(fitted, "widget_revision", heart["id"], 4),
        "widget_id_64": num(_required(fitted, "widget_id_64", heart["id"])),
        "widget_status_f_measured": _required(fitted, "widget_status_f_measured", heart["id"]),
        "gpio_classic": _required(ioc3s[0]["identity"], "gpio_classic", ioc3s[0]["id"]),
        "bridge_widget_revision": _integer(
            _required(bridges[0], "identity", bridges[0]["id"]),
            "widget_revision", bridges[0]["id"], 4),
        "board_records": [records[role] for role in BOARD_ROLES],
    }
    _validate_integer(heart_identity["widget_id_64"], 64, "HEART widget identity")
    wiring = _required(model["machine"]["emulation"], "boot_flash_wiring",
                       "IP30 boot flash")
    if heart_identity["widget_id_64"] >> 28 != heart_identity["widget_revision"]:
        raise CompileError("HEART widget identity disagrees with its revision")
    return {
        "id": heart["id"], "module_index": entry["module_index"],
        "slot": entry["slot"], "hub_slot_bits": None, "asic_kind": "HEART",
        "heart": heart_identity,
        "cpu_slots": [{"interface": 0, "slot": 0, "assembly": processor}],
        "bank_mib": banks,
        "accepted_memory_mib": _accepted_totals(sizes, descending),
        "accepted_bank_mib": sorted(sizes),
        "banks_descending": descending,
        "flash": {
            "kind": "OPERATOR_PROM",
            "size": model["machine"]["harness"]["firmware_bytes"],
            "select": wiring["select"], "strap": wiring["strap"],
        },
        "slave_role": False,
    }
