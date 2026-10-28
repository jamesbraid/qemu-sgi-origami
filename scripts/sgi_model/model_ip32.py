# SPDX-License-Identifier: BSD-3-Clause
"""Compile the fixed O2 population without Scalable Node topology."""
from .model_compile import CompileError


def compile_ip32(model: dict) -> dict:
    required = {
        "ip32-board": "ip32", "cpu0": "r10000", "crime": "crime",
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
            cpus[0].get("hardware_kind") != "r10000" or
            cpus[0].get("identity", {}).get("clock_mhz") != 195 or
            cpus[0].get("identity", {}).get("cache_mib") != 1 or
            population.get("nodes") != 1 or
            population.get("cpus_per_node") != [1] or
            population.get("dimms_per_node") != [8] or
            population.get("memory_mib_per_node") != [1024] or
            population.get("memory_bank_mib_per_node") != [[128] * 8] or
            model["composition"]["topology"].get("supported_cpu_populations") != [[1]]):
        raise CompileError("O2 supports one 195 MHz R10000 with 1 MiB L2 "
                           "and eight 128 MiB banks")
    node = {
        "id": crimes[0]["id"],
        "asic_kind": "CRIME", "cpu_slots": [{"interface": 0, "slot": 0,
            "assembly": {"id": cpus[0]["id"], "kind": "R10000",
                         "clock_hz": 195000000, "cache_mib": 1,
                         "mode_bits": 0x00198603}}],
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
