# The fx exercise pass condition test_prom_fx.py boots against, factored
# out so test_prom_fx_verdict.py can exercise it without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
test_prom_fx.py's docstring gives the account of the exercise and of
fx's own error ledger. No qemu_test import, so the verdict companion can
import this module under a plain python3 or pytest run.
"""

NETBOOT_PATH = "stand/fx.64"

NBLOCKS = 100000

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("fx banner", "SGI Version 6.5 ARCS BE64"),
    ("disk opened", "...opening dksc(0,1,0)"),
    ("drive self-test", "...drive selftest...OK"),
    ("write-compare scan started",
     f"sequential pass 1: scanning [0, {NBLOCKS}] ({NBLOCKS} blocks)"),
    ("scan completed", "100%"),
    ("butterfly scan started",
     f"butterfly pass 1: scanning [0, {NBLOCKS}] ({NBLOCKS} blocks)"),
)

ZERO_LEDGER = "TOTAL ERR BLOCKS r0(0 hard) w0(0 hard)"
LEDGERS_EXPECTED = 2

# fx's own error ledger (the zero-error line above) is the authority for
# miscompares and hard errors; the word "miscompare" itself appears in
# the exercise menu and its help, so it cannot be a marker.
FAILURE_MARKERS = (
    "hard error",
    "Failed to open",
    "no such device",
    "Unexpected",
)


def missing_assertions(log):
    missing = [name for name, evidence in ASSERTIONS if evidence not in log]
    ledgers = log.count(ZERO_LEDGER)
    if ledgers != LEDGERS_EXPECTED:
        missing.append(
            f"zero-error ledger after each exercise ({ledgers} of "
            f"{LEDGERS_EXPECTED})")
    return missing


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff fx netbooted by path, opened the disk, ran the full
    write-compare scan, reported zero error blocks, and printed no
    failure spelling; and the driver saw the error ledger before
    timeout."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see fx's error ledger after both "
            "exercises before timeout -- the run never proved they "
            "finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }
