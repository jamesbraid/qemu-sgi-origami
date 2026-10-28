# SPDX-License-Identifier: BSD-3-Clause
"""Compile the fixed O2 population without Scalable Node topology."""
from .model_compile import CompileError
from .model_shared import num


def compile_ip32(model: dict) -> dict:
    variant = model["composition"]["product"]
    if variant not in ("o2", "o2plus"):
        raise CompileError("unsupported IP32 CPU module selection")
    plus = variant == "o2plus"
    part, clock, cache = ("r12000", 400, 2) if plus else ("r10000", 195, 1)
    required = {
        "ip32-board": "ip32", "cpu0": part, "crime": "crime",
        "mace": "mace", "gbe": "gbe", "aic0": "aic7880",
        "aic1": "aic7880", "rtc": "ds1685", "flash": "at29c040a",
        "phy": "ics1890", "serial-a": "ns16550", "serial-b": "ns16550",
        "ethernet": "mace-ethernet", "ps2": "mace-ps2", "audio": "mace-audio",
    }
    inventory = {c["id"]: c.get("hardware_kind")
                 for c in model["components"]}
    if inventory != required or len(model["components"]) != len(required):
        raise CompileError("O2 requires its fixed onboard component population")
    population = model["composition"]["population"]
    cpus = [c for c in model["components"] if c["kind"] == "cpu"]
    crimes = [c for c in model["components"] if c.get("hardware_kind") == "crime"]
    if (len(cpus) != 1 or len(crimes) != 1 or
            cpus[0].get("hardware_kind") != part or
            cpus[0].get("identity", {}).get("clock_mhz") != clock or
            cpus[0].get("identity", {}).get("cache_mib") != cache or
            population.get("nodes") != 1 or
            population.get("cpus_per_node") != [1] or
            population.get("dimms_per_node") != [8] or
            population.get("memory_mib_per_node") != [1024] or
            population.get("memory_bank_mib_per_node") != [[128] * 8] or
            model["composition"]["topology"].get("supported_cpu_populations") != [[1]]):
        raise CompileError("O2 requires its selected CPU module and eight 128 MiB banks")
    processor = {"id": cpus[0]["id"], "kind": part.upper(),
                 "clock_hz": clock * 1000000, "cache_mib": cache,
                 "mode_bits": 0x00198603}
    if plus:
        identity = cpus[0]["identity"]
        entry = next((v for cpu in model["processors"] if cpu["part"] == part
                      for v in cpu["variants"] if v["clock_mhz"] == clock
                      and v["cache_mib"] == cache), {})
        prid = num(identity.get("processor_identification", 0))
        fpu = num(identity.get("fpu_identification", 0))
        mode = num(entry.get("reset_mode_bits", 0))
        if (prid, fpu, mode) != (0x0e35, 0x0900, 0x011a8603):
            raise CompileError("O2+ requires the selected R12000 identity and reset tuple")
        processor.update({"prid": prid, "fpu_prid": fpu, "mode_bits": mode,
                          "required_properties": [
                              "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
                              "r12000-scache-bytes", "r12000-scache-block-words"]})
    node = {
        "id": crimes[0]["id"],
        "asic_kind": "CRIME", "cpu_slots": [{"interface": 0, "slot": 0,
            "assembly": processor}],
        "bank_mib": list(population["memory_bank_mib_per_node"][0]),
        "accepted_memory_mib": [1024],
        "flash": {"kind": "OPERATOR_PROM", "size": 524288},
        "slave_role": False,
    }
    return {
        "name": model["composition"]["product"], "generation": "IP32",
        "description": model["machine"]["emulation"]["display_description"],
        "nodes": [node], "modules": [], "routers": [], "links": [],
        "crossbars": [], "xio_attachments": [], "xio_slots": [],
        "io_boards": [], "controllers": [], "console": 0, "junk_uart": -1,
        "supported_cpu_populations": [[1]],
        "node_populations": [{"nodes": [0], "routers": [], "cables": [],
            "xio_slots": [], "supported_cpu_populations": [[1]],
            "grade": "INFERRED"}],
    }
