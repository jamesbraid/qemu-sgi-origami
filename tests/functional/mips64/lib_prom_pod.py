# The POD sanity pass condition test_prom_pod.py boots against, factored
# out so test_prom_pod_verdict.py can exercise it without booting
#
# SPDX-License-Identifier: BSD-3-Clause
"""
test_prom_pod.py's docstring gives the account of POD and of which
commands are driven and which are deliberately not. No qemu_test import,
so the verdict companion can import this module under a plain python3 or
pytest run.
"""

import re


# ELSC switch value that selects local POD mode.
LOCAL_POD_SWITCHES = 0x10

# One newline-terminated command per POD prompt, in order.
COMMANDS = (
    "version",
    "why",
    "cfg",
    "pcfg v",
    "santest 0xa800000002000000",
    "memtest 0xa800000002000000 0x100000",
    "error",
    "error_dump",
    "dumpspool",
    "pr",
    "crb",
    "crbx",
    "dips",
    "module",
    "tlbr 0",
    "pf 0",
)

# Fixed firmware output lines for the prompt, configuration, memory,
# error state and registers. Regexes account for tabs in the config block.
ASSERTIONS = (
    ("POD prompt", re.compile(r"POD [A-Za-z]* ?(?:Cac|Dex|Unc)>")),
    ("PROM version banner", re.compile(r"SGI Version 6\.11")),
    ("power-on diagnostics verdict",
     re.compile(r"Reason : 0 \(Device passed diagnostics\.\)")),
    ("CPU speed in the flashed config",
     re.compile(r"CPU speed\s+= 180 MHz")),
    ("HUB speed in the flashed config",
     re.compile(r"HUB speed\s+= 90 MHz")),
    ("topology entry for node 0",
     re.compile(r"NASID=0 Mod=0 Slot=1")),
    ("memory sanity pass", re.compile(r"Sanity test passed\.")),
    ("memory pattern pass",
     re.compile(r"Address verify")),
    ("error dump brackets", re.compile(
        r"END Hardware Error State")),
    ("error spool complete", re.compile(r"Spool dump complete\.")),
    ("register dump", re.compile(r"r28/gp: 0x[0-9a-f]{16}")),
    ("coherent request buffers", re.compile(r"CRB_A")),
    ("local-POD switch value", re.compile(
        r"Hardware switches\s+10")),
    ("module number", re.compile(r"Module number is 0")),
    ("TLB entry zero", re.compile(r"00: ASID 00")),
    ("floating-point register", re.compile(r"\$f00 = 0x[0-9a-f]{16}")),
)

FAILURE_MARKERS = (
    "MEMORY FAILURE",
    "Sanity test failed",
    "Unexpected",
    "Usage:",
    "unknown command",
)


def missing_assertions(log):
    return [name for name, pattern in ASSERTIONS if not pattern.search(log)]


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff every fixed line appeared, no failure spelling did, and
    the driver saw the prompt come back after the last command."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see the POD prompt after the last "
            "command before timeout")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }
