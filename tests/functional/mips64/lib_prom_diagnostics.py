# The factory-diagnostics pass condition test_prom_diagnostics.py boots
# against, and the argv it boots with, factored out so
# test_prom_diagnostics_verdict.py can exercise them without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Pure PROM transcript parsing and verdict logic used by
test_prom_diagnostics.py. No qemu_test import is needed, so the verdict
companion can exercise this module with plain Python or pytest.
"""

import re

from lib_prom_sash import (NETBOOT_PATH,
                           missing_assertions as sash_missing_assertions)


# ---------------------------------------------------------------------------
# PROM transcript parsing and verdict logic.
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

    # PROM 6.11 (6.5-base) needs two passes: a cold pass that discovers no
    # module id, prints the assignment box and warm resets, then a second
    # pass that accepts the now-nonzero id. PROM 6.156 (6.5.30) does not:
    # it accepts its factory id (observed as 0, never reassigned) and runs
    # the whole sequence in one pass, with no box and no warm reset.
    # Captured 6.5.30 boots also show this complete single-pass form.
    # A transcript that only got through the box-and-reset half of a
    # genuine two-pass boot still needs both markers absent to qualify, so
    # it stays a wrong-pass-count failure below.
    single_full_pass = (
        len(passes) == 1
        and MODULE_BOX_MARKER not in passes[0]
        and WARM_RESET_MARKER not in passes[0]
    )

    if len(passes) != 2 and not single_full_pass:
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

    if single_full_pass:
        first, second = None, passes[0]
    else:
        first, second = passes
    ids1 = module_ids(first) if first is not None else None
    ids2 = module_ids(second)
    if first is not None:
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

    if first is not None:
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
    if first is not None:
        if len(ids2) != 1 or ids2 == [0]:
            problems.append(
                f"pass 2 hardware paths used module ids {ids2} rather than the "
                "single assigned nonzero module id"
            )
    elif len(ids2) not in (1, 2) or (len(ids2) == 2 and ids2[0] != 0):
        # A single-pass boot has no box-and-reset boundary. Module 0 may
        # identify the global master before assignment; later paths must
        # then use one assigned nonzero module ID.
        problems.append(
            f"pass 2 hardware paths used module ids {ids2} rather than "
            "module id 0 before assignment and a single id after"
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

    if first is not None:
        after = ids2[0] if len(ids2) == 1 else None
    else:
        # The single-pass shape's own last (highest) id is the one
        # assigned after "Global master is /hw/module/0/..." -- see the
        # module-id check above.
        after = ids2[-1] if ids2 else None
    report["module_id"] = {
        "before": ids1[0] if ids1 and len(ids1) == 1 else None,
        "after": after,
    }

    report["failures"]["observed"] = {
        "pass1": [entry for entry in report["pass1"]["rslt"]
                  if entry["verdict"] == "FAIL"] if report["pass1"] else [],
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
            if first is not None and warning in first:
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

    # Check sash's environment and boot path in the same transcript. The
    # path assertion verifies that the PROM requested boot/sash64.
    report["sash_missing"] = sash_missing_assertions(log, cpus=1)
    for name in report["sash_missing"]:
        problems.append(f"missing {name}")

    report["verdict"] = "PASS" if not problems else "FAIL"
    return report


# The ledger is embedded so the test records exactly which diagnostic
# failures are accepted. Parsing it here keeps its syntax checked by the
# same parser used for other ledger text.
EXPECTED_FAILURES_TEXT = """\
# Expected diagnostic failures for the current PROM configuration.
#
# Every failure must be listed, and every listed failure must still occur.
# Remove a line when its failure is fixed. An empty list requires a clean run.
#
# An empty list requires every diagnostic to pass. If a known failure is
# added, list its exact diagnostic name; a failure outside the list fails,
# and a listed diagnostic that passes is reported as stale. Remove a line
# when its failure is fixed.
"""

EXPECTED_FAILURES = parse_expected_failures(EXPECTED_FAILURES_TEXT)


def diag_extra_args(prom, tftp, elsc_log, capture):
    """Return the QEMU args for one CPU, normal PROM diagnostics and
    SLIRP netboot. `bootfile=` names NETBOOT_PATH (boot/sash64), so `tftp`
    must contain that path; test_prom_diagnostics.py stages the fixture.

    set_console() assigns serial line 0 to the primary console. The first
    serial argument here leaves line 1 unused so the ELSC AUX chardev uses
    line 2. That port captures diagnostic output when the PROM switches
    consoles after a failed diagnostic.
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
