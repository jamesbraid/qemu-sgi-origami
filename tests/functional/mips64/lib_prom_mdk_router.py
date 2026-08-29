# The MDK router-package pass condition test_prom_mdk_router.py boots
# against, factored out so test_prom_mdk_router_verdict.py can exercise
# it without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
test_prom_mdk_router.py's docstring gives the account of the package and
why its leg is red on current trunk. No qemu_test import, so the verdict
companion can import this module under a plain python3 or pytest run.
"""

NETBOOT_PATH = "mdk/MDK_router_sso"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("two nodes discovered", "Number of nodes:           2"),
    ("router test started", "Starting Router SSO test"),
    ("router test completion", "MDK Router_sso test run is complete"),
)

# The one-node decline is a failure here, not a skip: this leg exists to
# run the test. The manual's own failure spellings follow.
FAILURE_MARKERS = (
    "EXITING router_sso test",
    "TEST RESULT: ****",
    "Headless HUB inaccessible",
    "Unexpected exception",
    "ERROR",
)


def missing_assertions(log):
    return [name for name, evidence in ASSERTIONS if evidence not in log]


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
