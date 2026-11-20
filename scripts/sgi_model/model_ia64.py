# SPDX-License-Identifier: BSD-3-Clause
"""Compile the fixed Altix 350 population without Scalable Node topology."""
from .model_compile import CompileError
from .model_shared import num


def compile_ia64(model: dict) -> dict:
    if model["composition"]["product"] != "altix350":
        raise CompileError("unsupported IA64 product selection")
    required = {
        "ip57-board": "ip57", "cpu0": "madison", "cpu1": "madison",
        "shub": "shub", "pic": "pic", "ioc4": "ioc4", "scsi": "isp12160",
        "ethernet": "bcm5701",
    }
    inventory = {c["id"]: c.get("hardware_kind") for c in model["components"]}
    if inventory != required or len(model["components"]) != len(required):
        raise CompileError("Altix 350 requires its fixed node-board population")
    population = model["composition"]["population"]
    if (population.get("nodes") != 1 or
            population.get("cpus_per_node") != [2] or
            population.get("memory_mib_per_node") != [2048] or
            population.get("memory_bank_mib_per_node") != [[2048]] or
            model["composition"]["topology"].get(
                "supported_cpu_populations") != [[1], [2]]):
        raise CompileError("Altix 350 requires one node, one or two "
                           "processors and exactly 2048 MiB")
    variant = next(v for part in model["processors"]
                   if part["part"] == "madison" for v in part["variants"])
    clock, cache = variant["clock_mhz"], variant["cache_mib"]
    payload = model["machine"]["emulation"]["firmware_payload"]
    processor = {"id": f"madison-{clock}-{cache}m", "kind": "MADISON",
                 "clock_hz": clock * 1000000, "cache_mib": cache}
    node = {
        "id": "shub", "asic_kind": "SHUB",
        "cpu_slots": [{"interface": 0, "slot": slot, "assembly": processor}
                      for slot in (0, 1)],
        "bank_mib": list(population["memory_bank_mib_per_node"][0]),
        "accepted_memory_mib": [2048],
        "flash": {"kind": "OPERATOR_PROM", "size": num(payload["size"])},
        "slave_role": False,
    }
    return {
        "name": model["composition"]["product"], "generation": "IA64",
        "description": model["machine"]["emulation"]["display_description"],
        "nodes": [node], "modules": [], "routers": [], "links": [],
        "crossbars": [], "xio_attachments": [], "xio_slots": [],
        "io_boards": [], "controllers": [], "console": 0, "junk_uart": -1,
        "scsi_hbas": [c["hardware_kind"] for c in model["components"]
                      if c["kind"] == "storage-hba"],
        "supported_cpu_populations": [[1], [2]],
        "node_populations": [{"nodes": [0], "routers": [], "cables": [],
            "xio_slots": [], "supported_cpu_populations": [[1], [2]],
            "grade": "INFERRED"}],
    }
