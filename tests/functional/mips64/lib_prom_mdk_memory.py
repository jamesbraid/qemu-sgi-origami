# The memory.mdk battery pass condition test_prom_mdk_memory.py boots
# against, factored out so test_prom_mdk_memory_verdict.py can exercise
# it without booting
#
# SPDX-License-Identifier: BSD-3-Clause
"""
test_prom_mdk_memory.py's docstring gives the account of the battery and
why its verdict counts completions. No qemu_test import, so the verdict
companion can import this module under a plain python3 or pytest run.
"""

NETBOOT_PATH = "mdk/memory.mdk"

# Every built-in the package's own ls lists, quick ones first so an early
# failure surfaces fast; the two long-running tests close the battery.
TESTS = (
    "scache",
    "mem.qs",
    "memum.dr",
    "memum.qs",
    "memum.nb",
    "memum.in",
    "memum.lo",
    "mem.lo",
)

COMPLETION = "has completed"
# scache reports its summary line without the "Mem test" words the seven
# memory tests share, so the two counts differ by exactly one.
MEM_PASS = "Mem test PASSED"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("one-CPU configuration", "Total no. of CPUs = 1"),
    ("scache data path pass", "Scache data path test PASSED"),
    ("scache cell pass", "Scache cell test PASSED"),
    ("scache random address pass", "Scache random address test PASSED"),
)

FAILURE_MARKERS = (
    "Test Failed",
    "FAILED",
    "ERROR:",
    "Unexpected exception",
)


def missing_assertions(log):
    """Which fixed lines never appeared, plus which driven tests never
    echoed at the prompt."""
    missing = [name for name, evidence in ASSERTIONS if evidence not in log]
    for test in TESTS:
        if f"MDK> {test}" not in log:
            missing.append(f"{test} started at the prompt")
    return missing


def count_problems(log):
    """The completion and pass counts, as equalities.

    Every test ends with the same completion words, so presence proves
    nothing about the later tests; only the counts do. Equalities rather
    than at-least: a command that fired twice would inflate the count and
    must read as a broken run, not a stronger pass.
    """
    problems = []
    completions = log.count(COMPLETION)
    if completions != len(TESTS):
        problems.append(
            f"completion count {completions} != {len(TESTS)} driven tests")
    passes = log.count(MEM_PASS)
    if passes != len(TESTS) - 1:
        problems.append(
            f"memory-test pass count {passes} != {len(TESTS) - 1}")
    return problems


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff every test echoed, every fixed line appeared, the pass
    and completion counts are exact, no failure spelling appears, and
    the driver saw the final completion before timeout."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    problems.extend(count_problems(log))
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see the final completion before "
            "timeout -- the run never proved the battery finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


def reached_all_completions(transcript):
    """The driver's end condition: the whole battery has completed."""
    return transcript.count(COMPLETION.encode()) >= len(TESTS)
