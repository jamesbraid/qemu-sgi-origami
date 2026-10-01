# The instgen pass condition test_prom_mdk_instgen.py boots against,
# factored out so test_prom_mdk_instgen_verdict.py can exercise it
# without booting
#
# SPDX-License-Identifier: BSD-3-Clause
"""
test_prom_mdk_instgen.py's docstring gives the account of instgen and its
two-CPU requirement. No qemu_test import, so the verdict companion can
import this module under a plain python3 or pytest run.
"""

NETBOOT_PATH = "mdk/cpu.mdk"

END_PASS = 1000
INSTGEN_COMMAND = f"instgen -s 1 -e {END_PASS}"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("two-CPU configuration", "Total no. of CPUs = 2"),
    ("two CPUs driven", "Num_Cpus_To_Run              =  2"),
    ("first pass", "Instgen: Starting PASS  1 "),
    (f"final pass", f"Instgen: Starting PASS  {END_PASS} "),
    ("instgen completion",
     "MDK Random Instruction Generator test run is complete"),
)

# "ERROR: PASS: <n> GPR miscompare: ..." is the miscompare shape the
# manual documents; "miscompare" alone catches the FPR and other
# variants, and the wreck marker catches a kernel-side death.
FAILURE_MARKERS = (
    "ERROR",
    "miscompare",
    "FAILED",
    "Unexpected exception",
)


def missing_assertions(log):
    return [name for name, evidence in ASSERTIONS if evidence not in log]


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff the run reached pass 1000 and its completion line with
    every fixed assertion present and no failure spelling anywhere."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach the instgen completion line "
            "before timeout -- the run never proved the passes finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }
