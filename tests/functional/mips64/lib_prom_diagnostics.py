# The factory-diagnostics pass condition test_prom_diagnostics.py boots
# against, and the argv it boots with, factored out so
# test_prom_diagnostics_verdict.py can exercise them without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ported from tools/diag-gate in the project checkout (since retired);
test_prom_diagnostics.py's docstring gives the provenance of every name
here. No qemu_test import, so the verdict companion can import this
module under a plain python3 or pytest run.
"""

import re


# ---------------------------------------------------------------------------
# Verdict logic, ported from tools/diag-gate (project checkout). Kept as
# close to diag-gate's own wording and shape as possible; departures are
# called out where they happen.
# ---------------------------------------------------------------------------

PROM_BANNER_RE = re.compile(r"^IP27 PROM SGI Version.*$", re.M)
BASEIO_BANNER_RE = re.compile(r"^BASEIO PROM Monitor SGI Version.*$", re.M)
RSLT_RE = re.compile(
    r"^RSLT\s+(\S+)\s+(PASS|FAIL)\b[ \t]*(.*?)[ \t\r]*$", re.M
)
DIAG_RC_RE = re.compile(r"diag_rc\s*=\s*(\d+)[ \t]*(.*)")
MODULE_PATH_RE = re.compile(r"/hw/module/(\d+)/")

MODULE_BOX_MARKER = "did not have module id"
WARM_RESET_MARKER = "Resetting the system..."
MENU_MARKER = "System Maintenance Menu"
SUMMARY_START = "**** System Configuration and Diagnostics Summary ****"
SUMMARY_END = "**** End System Configuration and Diagnostics Summary ****"
ALL_PASSED_MARKER = "ALL DIAGS PASSED"
NVRAM_WARNINGS = (
    "NVRAM checksum is incorrect",
    "hardware inventory is invalid",
)

SUMMARY_COUNTS = (
    ("nodes_enabled", re.compile(r"No\. of NODEs enabled\s*=\s*(\d+)")),
    ("nodes_disabled", re.compile(r"No\. of NODEs disabled\s*=\s*(\d+)")),
    ("cpus_enabled", re.compile(r"No\. of CPUs enabled\s*=\s*(\d+)")),
    ("cpus_disabled", re.compile(r"No\. of CPUs disabled\s*=\s*(\d+)")),
    ("mem_enabled_mb", re.compile(r"Mem enabled\s*=\s*(\d+) MB")),
    ("mem_disabled_mb", re.compile(r"Mem disabled\s*=\s*(\d+) MB")),
    ("routers_enabled", re.compile(r"No\. of RTRs enabled\s*=\s*(\d+)")),
    ("routers_disabled", re.compile(r"No\. of RTRs disabled\s*=\s*(\d+)")),
)

EXPECTED_SUMMARY = {
    "nodes_enabled": 1,
    "nodes_disabled": 0,
    "cpus_enabled": 1,
    "cpus_disabled": 0,
    "mem_enabled_mb": 64,
    "mem_disabled_mb": 0,
    "routers_enabled": 0,
    "routers_disabled": 0,
}

DIAG_RESULT_RE = re.compile(r"^\s*(/hw/\S+):\s*(.*?)[ \t\r]*$")
DIAG_REASON_RE = re.compile(r"^\s*Reason:\s*(.*?)[ \t\r]*$")
ENV_VALUE_RES = {
    "console_out": re.compile(r"ConsoleOut=(\S+)"),
    "netaddr": re.compile(r"netaddr=(\S+)"),
    "dlserver": re.compile(r"dlserver=(\S+)"),
    "eaddr": re.compile(r"eaddr=(\S+)"),
}

# tools/sash-gate's ASSERTIONS, project checkout, fixed at cpus=1 -- the
# only population diag-gate drove. diag-gate's own evaluate_run() called
# sash_gate.missing_assertions(log, cpus=1) as its last check; this is that
# same check, reimplemented so this file never imports tools/.
_SASH_ASSERTIONS = (
    ("1 CPU inventory", "1 180 MHz IP27 Processors"),
    ("1 CPUs enabled", "No. of CPUs enabled     = 1"),
    ("no CPUs disabled", "No. of CPUs disabled    = 0"),
    ("64 MiB inventory", "Main memory size: 64 Mbytes"),
    ("IP27 system inventory", "System  SGI-IP27"),
    ("IOC3 Ethernet inventory", "Integral Fast Ethernet"),
    ("IOC3 serial inventory", "IOC3 serial port"),
    ("IOC3 output console", "ConsoleOut=/dev/tty/ioc30"),
    ("leased guest address", "netaddr=192.0.2.15"),
    ("download server address", "dlserver=192.0.2.2"),
)


def missing_sash_assertions(log):
    """Which of sash's post-netboot one-CPU inventory lines never appeared.

    See _SASH_ASSERTIONS above for provenance: this is tools/sash-gate's
    own ASSERTIONS/missing_assertions(cpus=1), ported rather than imported.
    """
    return [name for name, evidence in _SASH_ASSERTIONS if evidence not in log]


def split_passes(log):
    starts = [match.start() for match in PROM_BANNER_RE.finditer(log)]
    bounds = zip(starts, [*starts[1:], len(log)])
    return [log[start:end] for start, end in bounds]


def parse_rslt_lines(text):
    entries = []
    for match in RSLT_RE.finditer(text):
        name, verdict, rest = match.groups()
        diag_rc = None
        detail = rest
        rc_match = DIAG_RC_RE.match(rest)
        if rc_match:
            diag_rc = int(rc_match.group(1))
            detail = rc_match.group(2)
        entries.append({
            "name": name,
            "verdict": verdict,
            "diag_rc": diag_rc,
            "detail": detail,
        })
    return entries


def parse_summary(text):
    start = text.find(SUMMARY_START)
    if start < 0:
        return None
    end = text.find(SUMMARY_END, start)
    block = text[start:end if end >= 0 else len(text)]

    summary = {}
    for key, pattern in SUMMARY_COUNTS:
        match = pattern.search(block)
        summary[key] = int(match.group(1)) if match else None

    results = []
    in_results = False
    for line in block.splitlines():
        if line.strip() == "DIAG RESULTS:":
            in_results = True
            continue
        if not in_results:
            continue
        entry = DIAG_RESULT_RE.match(line)
        if entry:
            results.append({
                "path": entry.group(1),
                "finding": entry.group(2),
                "reason": None,
            })
            continue
        reason = DIAG_REASON_RE.match(line)
        if reason and results:
            results[-1]["reason"] = reason.group(1)
    summary["diag_results"] = results
    # The firmware's own positive verdict. Its absence with no RSLT failure
    # observed would mean the summary never judged the diagnostics at all,
    # which an empty ledger must not read as a pass.
    summary["all_passed"] = ALL_PASSED_MARKER in block
    return summary


def module_ids(text):
    return sorted({int(number) for number in MODULE_PATH_RE.findall(text)})


def extract_identities(log):
    identities = {
        "prom_banners": sorted({
            match.rstrip("\r") for match in PROM_BANNER_RE.findall(log)
        }),
        "baseio_banners": sorted({
            match.rstrip("\r") for match in BASEIO_BANNER_RE.findall(log)
        }),
    }
    for key, pattern in ENV_VALUE_RES.items():
        # The last match is the machine's end state: printenv's own value
        # is what the run actually finished with.
        matches = pattern.findall(log)
        identities[key] = matches[-1] if matches else None
    return identities


def parse_expected_failures(text):
    names = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if re.search(r"\s", line):
            raise ValueError(
                f"line {number}: a diagnostic name cannot contain whitespace"
            )
        if line in names:
            raise ValueError(f"line {number}: duplicate diagnostic: {line}")
        names.append(line)
    return names


def _judge_failures(report, problems, expected_failures):
    observed = {
        entry["name"]
        for results in report["failures"]["observed"].values()
        for entry in results
        if entry["verdict"] == "FAIL"
    }
    expected = list(expected_failures)
    unexpected = sorted(observed - set(expected))
    stale = sorted(set(expected) - observed)
    report["failures"]["unexpected"] = unexpected
    report["failures"]["stale"] = stale
    for name in unexpected:
        problems.append(f"unexpected diagnostic failure: {name}")
    for name in stale:
        problems.append(f"expected diagnostic failure did not occur: {name}")


def evaluate_run(log, expected_failures=(), nvram_warnings="expected"):
    problems = []
    passes = split_passes(log)
    report = {
        "passes": len(passes),
        "pass1": None,
        "pass2": None,
        "module_id": {"before": None, "after": None},
        "failures": {"observed": {}, "unexpected": [], "stale": []},
        "sash_missing": [],
        "problems": problems,
    }

    if len(passes) != 2:
        problems.append(
            f"expected exactly 2 PROM boot passes, saw {len(passes)}"
        )
        report["failures"]["observed"]["whole-log"] = [
            entry for entry in parse_rslt_lines(log)
            if entry["verdict"] == "FAIL"
        ]
        _judge_failures(report, problems, expected_failures)
        report["verdict"] = "FAIL"
        return report

    first, second = passes
    ids1 = module_ids(first)
    ids2 = module_ids(second)
    report["pass1"] = {
        "rslt": parse_rslt_lines(first),
        "module_ids": ids1,
        "module_box": MODULE_BOX_MARKER in first,
        "warm_reset": WARM_RESET_MARKER in first,
    }
    report["pass2"] = {
        "rslt": parse_rslt_lines(second),
        "module_ids": ids2,
        "module_box": MODULE_BOX_MARKER in second,
        "menu": MENU_MARKER in second,
        "summary": parse_summary(second),
        "nvram_warnings": [
            warning for warning in NVRAM_WARNINGS if warning in second
        ],
    }

    if not report["pass1"]["module_box"]:
        problems.append("pass 1 did not print the module-id assignment box")
    if not report["pass1"]["warm_reset"]:
        problems.append("pass 1 did not warm reset after the factory boot")
    if ids1 != [0]:
        problems.append(
            f"pass 1 hardware paths used module ids {ids1} rather than [0]"
        )
    if MENU_MARKER in first:
        problems.append(
            "pass 1 reached the System Maintenance Menu before the warm reset"
        )

    if report["pass2"]["module_box"]:
        problems.append("pass 2 repeated the module-id assignment box")
    if WARM_RESET_MARKER in second:
        problems.append("pass 2 warm reset again")
    if len(ids2) != 1 or ids2 == [0]:
        problems.append(
            f"pass 2 hardware paths used module ids {ids2} rather than the "
            "single assigned nonzero module id"
        )
    if not report["pass2"]["menu"]:
        problems.append("pass 2 did not reach the System Maintenance Menu")
    summary = report["pass2"]["summary"]
    if summary is None:
        problems.append(
            "pass 2 printed no System Configuration and Diagnostics Summary"
        )
    else:
        for key, expected_value in EXPECTED_SUMMARY.items():
            if summary[key] != expected_value:
                problems.append(
                    f"summary {key} = {summary[key]} rather than "
                    f"{expected_value}"
                )
        if not summary["all_passed"] and not any(
                report["failures"]["observed"].values()):
            problems.append(
                "pass 2 summary does not report ALL DIAGS PASSED although "
                "no RSLT failure was observed"
            )

    report["module_id"] = {
        "before": ids1[0] if len(ids1) == 1 else None,
        "after": ids2[0] if len(ids2) == 1 else None,
    }

    report["failures"]["observed"] = {
        "pass1": [entry for entry in report["pass1"]["rslt"]
                  if entry["verdict"] == "FAIL"],
        "pass2": [entry for entry in report["pass2"]["rslt"]
                  if entry["verdict"] == "FAIL"],
    }
    _judge_failures(report, problems, expected_failures)

    if nvram_warnings == "expected":
        for warning in NVRAM_WARNINGS:
            if warning not in second:
                problems.append(
                    f"pass 2 did not print the expected warning: {warning}"
                )
            if warning in first:
                problems.append(
                    f"pass 1 printed a warning expected only on pass 2: "
                    f"{warning}"
                )
    else:
        for warning in NVRAM_WARNINGS:
            if warning in log:
                problems.append(
                    f"run printed a warning forbidden by "
                    f"nvram_warnings=absent: {warning}"
                )

    report["sash_missing"] = missing_sash_assertions(log)
    for name in report["sash_missing"]:
        problems.append(f"missing {name}")

    report["verdict"] = "PASS" if not problems else "FAIL"
    return report


# The expected-failure ledger, ported verbatim (comments included) from
# tools/diag-gate-expected-failures in the project checkout -- the file
# this port cannot read at test run time, so its content is embedded here
# the same way test_prom_boot.py embeds MARK from tests/golden/. Feeding
# it through parse_expected_failures (the ported function itself, not a
# hand-picked list) exercises that port against its own real ledger text.
EXPECTED_FAILURES_TEXT = """\
# Expected diagnostic failures for tools/diag-gate on the current trunk.
#
# This is the red baseline, not a tolerance. The gate reports any failure
# not listed here AND any listed diagnostic that stops failing, so a fix
# that lands must remove its line in the same change. The final acceptance
# run takes no list at all: an empty list is the full-acceptance contract.
#
# The red baseline (one CPU, 64 MiB, PROM release 6.5-base, normal
# diagnostics; docs/library/one-cpu-normal-diagnostics-gate.md and the
# boot archive) listed serial_dma, enet_ssram, scsi_ram and scsi_dma. A
# run of this test on the 2026-08-27 trunk observed no failure in either
# pass -- the serial-DMA engines, the IOC3 ethernet series and the qlogic
# fixes had each landed without retiring their lines -- so all four are
# retired here per the rule above, and the list is empty: the
# full-acceptance contract is now what this test asserts. The capture
# that measured it is docs/boot-logs/firmware.diagnostics-all-clear.* in
# the project checkout.
"""

EXPECTED_FAILURES = parse_expected_failures(EXPECTED_FAILURES_TEXT)


def diag_extra_args(prom, tftp, elsc_log, capture):
    """The QEMU args beyond what QEMUMachine's set_machine()/set_console()
    already provide, for diag-gate's fixed configuration: one CPU, normal
    (not skipped) power-on diagnostics, and SLIRP usermode networking
    serving sash64 with no external TFTP server. Ported from
    tools/sash-gate's qemu_command(cpus=1, skip_prom_diags=False) in the
    project checkout -- see this module's own docstring for exactly which
    call diag-gate made and why netinst-rig is not this argv's source.

    set_console() already emits "-serial chardev:console" for serial line
    0 (IOC3 port A, the primary console); the "-serial none" here fills
    line 1 (IOC3 port B, unused) so the "-serial chardev:elsc" that
    follows lands on line 2, the ELSC AUX port -- the same
    podlib.serial_args(ioc3_a=..., elsc=...) numbering sash_gate.
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
                 f'dhcpstart=192.0.2.15,tftp={tftp},bootfile=sash64'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]
