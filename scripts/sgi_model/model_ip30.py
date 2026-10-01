# SPDX-License-Identifier: BSD-3-Clause
"""Compile the selected IP30 populations without borrowing SN node facts."""

from .model_shared import _integer, _required, _validate_integer, num

R12000_PROPERTIES = (
    "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
    "r12000-scache-bytes", "r12000-scache-block-words",
)
BOARD_ROLES = ("processor", "system-board", "frontplane", "power-supply")


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
        properties = identity.get("required_properties")
        if (not isinstance(properties, list) or
                len(properties) != len(R12000_PROPERTIES) or
                set(properties) != set(R12000_PROPERTIES)):
            raise CompileError("R12000 requires all five explicit profile properties")
        if any(field in identity for field in
               ("processor_identification", "fpu_identification", "cache_clock_mhz")):
            raise CompileError("IP30 R12000 identity and cache clocks are operator inputs")
        processor["required_properties"] = properties
    else:
        processor["prid"] = num(_required(identity, "processor_identification", cpu["id"]))
        if processor["prid"] & ~255 != 0x900:
            raise CompileError("IP30 R10000 identity requires implementation 0x09")

    banks = list(population["memory_bank_mib_per_node"][0])
    if len(banks) > 4:
        raise CompileError("HEART supports four selected memory banks")
    banks += [0] * (4 - len(banks))
    layouts = _required(model["machine"]["emulation"]["emulator_constraints"],
                        "accepted_bank_layouts_mib", "IP30 memory population")
    memory = population["memory_mib_per_node"][0]
    if banks not in layouts or any(len(row) != 4 or sum(row) != memory
                                  for row in layouts):
        raise CompileError("IP30 bank layouts must match the selected total and default")
    for row in layouts:
        empty = False
        for capacity in row:
            if type(capacity) is not int or capacity not in (0, 64, 128, 256):
                raise CompileError("IP30 banks require a selected 64, 128 or 256 MiB pair")
            if capacity and empty:
                raise CompileError("IP30 memory banks must be populated sequentially")
            empty |= capacity == 0

    layouts = [banks] + [row for row in layouts if row != banks]

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
    heart_identity = {
        "widget_revision": _integer(fitted, "widget_revision", heart["id"], 4),
        "widget_id_64": num(_required(fitted, "widget_id_64", heart["id"])),
        "widget_status_f_measured": _required(fitted, "widget_status_f_measured", heart["id"]),
        "gpio_classic": _required(ioc3s[0]["identity"], "gpio_classic", ioc3s[0]["id"]),
        "board_records": [records[role] for role in BOARD_ROLES],
    }
    _validate_integer(heart_identity["widget_id_64"], 64, "HEART widget identity")
    if heart_identity["widget_id_64"] >> 28 != heart_identity["widget_revision"]:
        raise CompileError("HEART widget identity disagrees with its revision")
    return {
        "id": heart["id"], "module_index": entry["module_index"],
        "slot": entry["slot"], "hub_slot_bits": None, "asic_kind": "HEART",
        "heart": heart_identity,
        "cpu_slots": [{"interface": 0, "slot": 0, "assembly": processor}],
        "bank_mib": banks, "accepted_memory_mib": [memory],
        "accepted_bank_layouts": [{"bank_mib": row} for row in layouts],
        "flash": {"kind": "OPERATOR_PROM", "size": model["machine"]["harness"]["firmware_bytes"]},
        "slave_role": False,
    }
