# The MDK router-package pass condition test_prom_mdk_router.py boots
# against, kept free of qemu_test so it can be checked without booting
#
# SPDX-License-Identifier: BSD-3-Clause
"""
test_prom_mdk_router.py's docstring gives the account of the package and
why it runs on the Origin 2000 module. No qemu_test import, so a plain
python3 or pytest run can import this module.
"""

import re

NETBOOT_PATH = "mdk/MDK_router_sso"

# The diagnostic's own lines, read from the package's strings and from a
# passing run on the Origin 2000 module. The module has four nodes and two
# Routers; the run must see all of them, run every one of its 238 passes
# (numbered 0 to 237) and print its own pass result. "TEST PASSED" carries
# two spaces before the closing stars in the binary.
ASSERTIONS = (
    ("path-structured netboot fetch",
     re.compile(re.escape(f"Obtaining {NETBOOT_PATH} from server"))),
    ("MDK 1.6 banner", re.compile(r"SGI MDK Version 1\.6")),
    ("router test started", re.compile(r"Starting Router SSO test")),
    ("238 passes configured", re.compile(r"End Pass +=  238\b")),
    ("four nodes discovered", re.compile(r"Number of nodes: +4\b")),
    ("two routers discovered", re.compile(r"Number of routers: +2\b")),
    ("last pass completed", re.compile(r"PASS = 237 Completed")),
    ("test passed",
     re.compile(re.escape("TEST RESULT: **** TEST PASSED  ****"))),
    ("router test completion",
     re.compile(r"MDK Router_sso test run is complete")),
)

# The test's own failure and warning spellings (the manual's examples and
# the package's strings): a per-pass checkbit ERROR, a failed vector read,
# a WARNING for sub-threshold counts, the FAIL result, a decline, and a
# kernel exception.
FAILURE_MARKERS = (
    "EXITING router_sso test",
    "ERROR",
    "WARNING: CPU",
    "TEST RESULT: **** WARNING",
    "TEST RESULT: **** FAIL",
    "Unexpected exception",
)


def missing_assertions(log):
    return [name for name, evidence in ASSERTIONS
            if not evidence.search(log)]


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach the router test's completion "
            "line before timeout")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }
