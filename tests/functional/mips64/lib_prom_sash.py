# The sash netboot pass condition test_prom_diagnostics.py boots against,
# and the argv it boots with, factored out so test_prom_diagnostics_verdict.py
# can exercise them without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ported from tools/sash-gate and tools/sashlib.py in the project checkout;
test_prom_diagnostics.py's own docstring gives the provenance of every name
here and the one departure (NETBOOT_PATH) -- test_prom_sash.py, this
module's original own consumer, carried that provenance until its separate
1-CPU netboot coverage was folded into test_prom_diagnostics.py and it was
deleted. test_prom_pod.py also imports RELEASES from here directly. No
qemu_test import, so a verdict companion can import this module under a
plain python3 or pytest run.
"""

from lib_prom import file_sha256


# ---------------------------------------------------------------------------
# Verdict logic, ported from tools/sashlib.py (project checkout, read from
# fj/main). Kept byte-for-byte on the fixed strings; departures are called
# out where they happen.
# ---------------------------------------------------------------------------

# tools/sashlib.py's own ASSERTIONS tuple, copied verbatim.
ASSERTIONS = (
    ("IP27 system inventory", "System  SGI-IP27"),
    ("64 MiB inventory", "Main memory size: 64 Mbytes"),
    ("IOC3 Ethernet inventory", "Integral Fast Ethernet"),
    ("IOC3 serial inventory", "IOC3 serial port"),
    ("IOC3 output console", "ConsoleOut=/dev/tty/ioc30"),
    ("leased guest address", "netaddr=192.0.2.15"),
    ("download server address", "dlserver=192.0.2.2"),
)

# The generic, non-vendor path this test's real boot serves sash64 under
# and requests by name -- see this module's docstring for why "boot/
# sash64" and not a flat name or a vendor IRIX release path. This is a
# DEPARTURE from tools/sashlib.py's own ASSERTIONS above (which stay a
# byte-for-byte port): sash-gate's own config serves a flat
# "bootfile=sash64", so it never had a path to assert. Checking this line
# is what turns this test from "sash netbooted" into "sash netbooted
# BY THE PATH THIS TEST SERVED IT UNDER" -- the guard a plain flat-name
# check cannot tell apart from a PROM that silently ignored the requested
# path and fetched something else with the same content.
NETBOOT_PATH = "boot/sash64"
NETBOOT_PATH_ASSERTION = (
    "path-structured netboot fetch",
    f"Obtaining {NETBOOT_PATH} from server",
)


def missing_assertions(log, cpus=1):
    """Which of sash's post-netboot hardware-inventory lines never appeared,
    PLUS whether the PROM's own fetch line named the path-structured file
    this test serves (NETBOOT_PATH_ASSERTION -- new to this port, not part
    of the sashlib.py port below).

    Ports tools/sashlib.py's own missing_assertions(log, cpus): for the
    given population, the two CPU-count lines and the memory-size line are
    built from `cpus`, and every other ASSERTIONS entry is checked as-is
    (sashlib's own assertions tuple rebuilds the memory-size line from
    `cpus` too and drops the fixed "64 MiB inventory" entry from
    ASSERTIONS so it is not checked twice; at cpus=1 the two are the same
    string, so that rebuild is not a behavior change here).

    This port carries ONLY the proven 1-CPU contract (see this module's
    docstring): cpus values other than 1 would build sash-smp-gate's
    two-node topology, which this fork test does not drive, so they are
    refused rather than silently mischecked.
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


# tools/sashlib.py's own RELEASES table, SHA-256 digests copied verbatim.
# The prom/tftp Path entries sashlib.py carries alongside them point at
# the project checkout's own assets/ layout; this port's real boot below
# resolves the same relative layout under ORIGAMI_ASSETS itself (see
# FIRMWARE_REL/TFTP_REL), so only the digests are reproduced here.
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
    """Which of the release-matched PROM/sash64 SHA-256 digests mismatch.

    Ports tools/sashlib.py's own artifact_identity_problems(artifacts,
    prom, sash): sash-gate refuses to launch a run against a PROM or
    sash64 that does not match the selected --release's recorded digest,
    so a stale or wrong-release asset is caught before spending a boot on
    it. stdlib hashlib only -- no sashlib import.
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
    every fixed hardware-inventory assertion is present in the transcript.

    This is sash-gate's own check, reimplemented: sash-gate's main() runs
    missing_assertions(log, args.cpus) and fails on anything missing, and
    separately fails if its console driver's own --until match never
    fires (a run that timed out before proving sash64 actually loaded).
    Unlike test_prom_diagnostics.py's evaluate_run, there is no RSLT
    ledger to judge here -- sash-gate drives exactly one PROM pass and
    makes exactly this one check, so this port makes exactly that one
    check too.

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
    """The QEMU args beyond what QEMUMachine's set_machine()/set_console()
    already provide, for sash-gate's fixed 1-CPU configuration: normal
    (not skipped) power-on diagnostics and SLIRP usermode networking
    serving sash64 with no external TFTP server. Ported from
    tools/sashlib.py's qemu_command(cpus=1, skip_prom_diags=False) -- see
    this module's docstring for exactly which call sash-gate makes -- with
    one departure: `bootfile=` names NETBOOT_PATH (`boot/sash64`), not
    sash-gate's own flat `bootfile=sash64`. `tftp` must therefore be a
    directory that actually has a `boot/sash64` under it (PromSash stages
    one; see its own comment), not sash-gate's flat assets/boot/<release>
    tree directly.

    set_console() already emits "-serial chardev:console" for serial line
    0 (IOC3 port A, the primary console); the "-serial none" here fills
    line 1 (IOC3 port B, unused) so the "-serial chardev:elsc" that
    follows lands on line 2, the ELSC AUX port -- the same
    podlib.serial_args(ioc3_a=..., elsc=...) numbering tools/sashlib.py's
    qemu_command itself uses. console=d selects IOC3 serial A on line 0,
    captured by set_console() above; the ELSC AUX on line 2 carries the
    firmware's power-on-diagnostic console output, which the PROM re-opens
    on the system controller after a failed diagnostic, captured here
    rather than lost to a dark port.
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
