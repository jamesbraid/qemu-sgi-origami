# The sash netboot pass condition test_prom_diagnostics.py boots against,
# and the argv it boots with, factored out so test_prom_diagnostics_verdict.py
# can exercise them without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
This module holds the fixed assertions, asset digests and verdict helpers
shared by the stock-PROM netboot tests. NETBOOT_PATH records the file path
those tests serve and require the PROM to fetch. No qemu_test import is
needed, so verdict companions can import this module with plain Python or
pytest.
"""

from lib_prom import file_sha256


# ---------------------------------------------------------------------------
# Fixed transcript checks for the stock-PROM netboot test.
# ---------------------------------------------------------------------------

# Required post-netboot inventory lines.
ASSERTIONS = (
    ("IP27 system inventory", "System  SGI-IP27"),
    ("64 MiB inventory", "Main memory size: 64 Mbytes"),
    ("IOC3 Ethernet inventory", "Integral Fast Ethernet"),
    ("IOC3 serial inventory", "IOC3 serial port"),
    ("IOC3 output console", "ConsoleOut=/dev/tty/ioc30"),
    ("leased guest address", "netaddr=192.0.2.15"),
    ("download server address", "dlserver=192.0.2.2"),
)

# Serve sash64 under this path and check the PROM's fetch line names it.
# A content match alone cannot show which path the PROM fetched.
NETBOOT_PATH = "boot/sash64"
NETBOOT_PATH_ASSERTION = (
    "path-structured netboot fetch",
    f"Obtaining {NETBOOT_PATH} from server",
)


def missing_assertions(log, cpus=1):
    """Return missing inventory and fetch-path assertions for a one-CPU
    boot. Other CPU counts raise because this test does not cover them.
    """
    if cpus != 1:
        raise NotImplementedError(
            "test_prom_sash ports only sash-gate's proven 1-CPU contract; "
            "--cpus 2/4 build the two-node topology this port does not "
            "carry (see this module's docstring)")
    memory = 64
    assertions = (
        (f"{cpus} CPU inventory", f"{cpus} 180 MHz IP27 Processors"),
        (f"{cpus} CPUs enabled", f"No. of CPUs enabled     = {cpus}"),
        ("no CPUs disabled", "No. of CPUs disabled    = 0"),
        (f"{memory} MiB inventory", f"Main memory size: {memory} Mbytes"),
        NETBOOT_PATH_ASSERTION,
        *(
            assertion
            for assertion in ASSERTIONS
            if assertion[0] != "64 MiB inventory"
        ),
    )
    return [name for name, evidence in assertions if evidence not in log]


# SHA-256 digests of the PROM and sash64 assets used for each release.
# The tests resolve their files under QEMU_TEST_SGI_ASSET_DIR.
RELEASES = {
    "6.5-base": {
        "prom_sha256": (
            "14e0d0a77b92154e3b33573f30608f241ded249db6060fba9f97bfd00b2145b2"
        ),
        "sash_sha256": (
            "d1a00a1a592022d9939026168a8e040548c4986284dcb29310d569f0c8b7d631"
        ),
    },
    "6.5.30": {
        "prom_sha256": (
            "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
        ),
        "sash_sha256": (
            "7b86941c14cb8679301decc5ebb192b1344510d16f8f7d9f986fea7ec4a17142"
        ),
    },
}


def artifact_identity_problems(release, prom_path, sash_path):
    """Return release identity mismatches for the supplied PROM and sash64.
    """
    artifacts = RELEASES[release]
    problems = []
    if file_sha256(prom_path) != artifacts["prom_sha256"]:
        problems.append("PROM SHA-256 does not match the selected release")
    if file_sha256(sash_path) != artifacts["sash_sha256"]:
        problems.append("sash64 SHA-256 does not match the selected release")
    return problems


def verdict(log, reached=True, cpus=1):
    """PASS iff the console driver reached sash's post-netboot state and
    every required inventory assertion is present in the transcript.

    Returns {"verdict": "PASS"|"FAIL", "missing": [...], "problems": [...]}.
    """
    problems = []
    missing = missing_assertions(log, cpus)
    for name in missing:
        problems.append(f"missing {name}")
    if not reached:
        problems.append(
            "console driver did not reach the download-server "
            "confirmation before timeout -- the run never proved sash64 "
            "actually loaded")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "missing": missing,
        "problems": problems,
    }


def sash_extra_args(prom, tftp, elsc_log, capture):
    """The additional QEMU args for the one-CPU stock-PROM netboot.
    SLIRP serves NETBOOT_PATH from the supplied TFTP root without an
    external TFTP server.

    set_console() already emits "-serial chardev:console" for serial line
    0 (IOC3 port A, the primary console); the "-serial none" here fills
    line 1 (IOC3 port B, unused) so the "-serial chardev:elsc" that
    follows lands on line 2, the ELSC AUX port. set_console() selects
    IOC3 serial A on line 0. After a failed power-on diagnostic, the PROM
    reopens output on the ELSC AUX line; capturing line 2 preserves that
    failure output.
    """
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
                 f'dhcpstart=192.0.2.15,tftp={tftp},bootfile={NETBOOT_PATH}'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]
