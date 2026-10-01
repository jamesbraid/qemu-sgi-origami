# The MDK netboot pass condition test_prom_mdk.py boots against, and the
# asset digests and argv the MDK family of tests shares, factored out so
# test_prom_mdk_verdict.py can exercise them without booting
#
# SPDX-License-Identifier: BSD-3-Clause
"""
test_prom_mdk.py's docstring gives the account of MDK and its transcripts.
No qemu_test import, so the verdict companions of the whole MDK family
(test_prom_mdk_memory.py, test_prom_mdk_instgen.py, test_prom_pod.py,
test_prom_fx.py and test_prom_mdk_router.py all build on this module) can
import it under a plain python3 or pytest run.
"""

from lib_prom import file_sha256


# ---------------------------------------------------------------------------
# Verdict logic. No project-tools port exists for MDK the way sashlib.py
# existed for sash; the fixed strings below are the kernel's own output,
# read from the real captured transcripts named in
# test_prom_mdk_verdict.py's fixture provenance.
# ---------------------------------------------------------------------------

# The path-structured file this test serves and asks for, staged the same
# way test_prom_sash.py stages boot/sash64 -- see that module's
# NETBOOT_PATH comment for why by-path, not flat.
NETBOOT_PATH = "mdk/cpu.mdk"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("Nanos boot banner", "Booting Nanos..."),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("one-CPU configuration", "Total no. of CPUs = 1"),
    ("pcache start", "Primary Data Cache Test Started"),
    ("pcache pass", "PCACHE Test Passed"),
    ("pcache completion", "Pcache test run is complete"),
)

# The kernel names failures explicitly: the manual's pcache failure
# output is "ERROR: Test Failed", and a kernel-side wreck prints
# "Unexpected exception occured" (the kernel's own spelling). Any of
# these in the transcript fails the run even with every ASSERTIONS line
# present.
FAILURE_MARKERS = (
    "Test Failed",
    "ERROR:",
    "Unexpected exception",
)


def missing_assertions(log):
    """Which of the fixed netboot-and-pcache lines never appeared."""
    return [name for name, evidence in ASSERTIONS if evidence not in log]


def failure_markers_present(log):
    """Which of the kernel's own failure spellings appear in the log."""
    return [marker for marker in FAILURE_MARKERS if marker in log]


# The real PROM image and the cpu.mdk this test refuses to run without.
# The PROM digest is test_prom_sash.py's 6.5.30 entry. The cpu.mdk digest
# identifies the extracted package used by these tests.
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
)
CPU_MDK_SHA256 = (
    "8a72e5578436b076bae72f6243940a523364ea961604d6070de7f40b408d5d89"
)
# The memory package, recorded by the same extraction; test_prom_mdk_memory
# boots it through this module's shared plumbing.
MEMORY_MDK_SHA256 = (
    "ee12d271bd84f2b3991724d40a211b9d9f511e1a355b58eee71c123c5b3f90bf"
)


def payload_identity_problems(prom_path, payload_path, payload_sha256,
                              payload_label):
    """Which of the PROM/payload SHA-256 digests mismatch -- the same
    refusal test_prom_sash.py makes, so a stale or wrong asset is caught
    before spending a boot on it. The MDK test files share this with
    their own package digest and label."""
    problems = []
    if file_sha256(prom_path) != PROM_SHA256:
        problems.append("PROM SHA-256 does not match the recorded release")
    if file_sha256(payload_path) != payload_sha256:
        problems.append(
            f"{payload_label} SHA-256 does not match the recorded "
            "extraction")
    return problems


def artifact_identity_problems(prom_path, mdk_path):
    return payload_identity_problems(prom_path, mdk_path, CPU_MDK_SHA256,
                                     "cpu.mdk")


def verdict(log, reached=True):
    """PASS iff the driver reached pcache's completion line, every fixed
    assertion is present, and none of the kernel's failure spellings is.

    Returns {"verdict": "PASS"|"FAIL", "missing": [...], "problems": [...]}.
    """
    problems = []
    missing = missing_assertions(log)
    for name in missing:
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach the pcache completion line "
            "before timeout -- the run never proved the test finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "missing": missing,
        "problems": problems,
    }


def mdk_extra_args(prom, tftp, elsc_log, capture,
                   netboot_path=NETBOOT_PATH):
    """The QEMU args beyond set_machine()/set_console(), matching
    test_prom_sash.py's sash_extra_args line for line: one CPU, 64 MB,
    normal power-on diagnostics, the ELSC AUX chardev on serial line 2,
    SLIRP usermode networking serving this test's staged tftp root, and a
    wire capture. Only bootfile= differs."""
    return [
        '-cpu', 'R10000',
        '-smp', '1',
        '-m', '64',
        '-bios', str(prom),
        '-audio', 'none',
        '-chardev', f'file,id=elsc,path={elsc_log}',
        '-serial', 'none',
        '-serial', 'chardev:elsc',
        '-nic', ('user,id=net0,net=192.0.2.0/24,host=192.0.2.2,'
                 f'dhcpstart=192.0.2.15,tftp={tftp},'
                 f'bootfile={netboot_path}'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]
