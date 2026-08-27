#!/usr/bin/env python3
#
# Factory diagnostics: boot the real IP27 PROM through its two-pass
# factory sequence and to sash64, and judge every diagnostic against an
# expected-failure ledger
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports the check that used to live in tools/diag-gate, in the PROJECT
checkout, into this fork's own functional-test framework, the same way
test_prom_boot.py ports tools/boot-gate. diag-gate (since retired --
superseded by this functional test) drove one QEMU invocation through the
whole factory sequence: the cold first pass has no module id, so the
firmware assigns one and warm-resets itself (same process, no new
invocation); the second pass accepts that assigned id, runs the normal
power-on diagnostics, prints the System Configuration and Diagnostics
Summary, reaches the System Maintenance Menu, and is then driven through
the command monitor to netboot sash64 over QEMU's own SLIRP usermode
networking (no external TFTP server). Every `RSLT <name> PASS/FAIL` line on
both passes is parsed and judged against an explicit expected-failure
ledger, ported below from diag-gate's own expected-failures list (the file
that shipped beside it in the project checkout before retirement): an
unexpected FAIL fails the run, and an expected failure that stops happening
is reported as stale rather than silently accepted.

evaluate_run() and the pure functions it calls (split_passes,
parse_rslt_lines, parse_summary, module_ids, extract_identities,
parse_expected_failures, _judge_failures) are a direct, close-to-verbatim
port of diag-gate's own functions of the same names. missing_sash_assertions
below is the same port of tools/sash-gate's ASSERTIONS/missing_assertions
that diag-gate itself called (via sash_gate) as the last stage of its own
evaluate_run -- reimplemented here, fixed at cpus=1 (the only population
diag-gate ever drove), because this fork test must not import origami's
tools/ (no sashlib). test_prom_diagnostics_verdict.py proves this port
against real captured transcripts and the ported ledger, without booting
anything -- see that file's docstring. Both are free of any qemu_test
import on purpose: importing qemu_test requires MESON_BUILD_ROOT and
packages meson's own PYTHONPATH wires in, neither of which a plain
`pytest test_prom_diagnostics_verdict.py` run has -- see the guarded import
below.

The QEMU argv this file builds (diag_extra_args) was diag-gate's own
gate_qemu_command, which was tools/sash-gate's qemu_command with cpus=1,
skip_prom_diags=False -- read directly from tools/sash-gate in the project
checkout (the "-M origin200,synthesized-firmware=off,debug-leds=off" machine
string, the "-nic user,...,tftp=...,bootfile=sash64" SLIRP usermode netboot
line, and the ELSC AUX chardev on serial line 2 that podlib.serial_args
wires so the firmware's power-on-diagnostic console output -- which the
PROM re-opens on the system controller after a failed diagnostic -- is not
lost to a dark port; console=d selects IOC3 serial A on line 0, the
primary console, not this port). tools/netinst-rig,
named in this port's brief as a second reference, turns out to drive a real
irix-netinst server over bridged tap networking, not SLIRP at all -- a
different tool for a different (real-server) netboot path -- so it is not
the source this file's argv was built from; sash_gate.qemu_command is, and
diag-gate called that function directly. A captured confirmation that this
fork's own build now needs none of sash_gate's --slirp-dir/LD_LIBRARY_PATH
plumbing is docs/boot-logs/firmware.sash-netboot-vendored-slirp -- this
fork's meson build vendors libslirp as a static subproject
(subprojects/libslirp.wrap), so the emulator built from this tree links it
in rather than loading it at runtime.
"""

import datetime
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest
# test_prom_diagnostics_verdict.py` run (which imports straight from this
# module) finds it the same way test_prom_boot_verdict.py finds
# test_prom_boot.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# qemu_test is only importable inside meson's own test environment (see
# test_prom_boot.py's docstring for the exact reason). Guarded so that
# every pure function below -- the whole surface
# test_prom_diagnostics_verdict.py exercises -- stays importable without
# it; PromDiagnostics itself is simply not defined in that case, which is
# fine because nothing outside meson ever tries to run the real boot.
try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


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
# that measured it is docs/boot-logs/firmware.diag-gate-all-clear.* in
# the project checkout.
"""

EXPECTED_FAILURES = parse_expected_failures(EXPECTED_FAILURES_TEXT)


# ---------------------------------------------------------------------------
# The real boot: QEMU argv, the interactive drive through the factory
# sequence to sash64, and provenance. Not covered by
# test_prom_diagnostics_verdict.py -- see this module's own docstring for
# why -- so nothing below this line needs to be pure or independently
# testable the way the verdict logic above does.
# ---------------------------------------------------------------------------

# Where ORIGAMI_ASSETS resolves the real firmware and boot tree, matching
# test_prom_boot.py's FIRMWARE_REL and tools/sash-gate's RELEASES["6.5-base"]
# in the project checkout: ORIGAMI_ASSETS names a CHECKOUT, not assets/
# itself.
FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5-base',
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('assets', 'boot', '6.5-base')

# diag-gate's own default (tools/diag-gate's DEFAULT_TIMEOUT_SECONDS,
# before that tool's retirement) covered both passes and the sash leg in
# one budget. Kept a little under the meson timeout registered for this
# test in mips64/meson.build, the same margin test_prom_boot.py keeps
# against its own 700s meson timeout.
BOOT_TIMEOUT_SECONDS = 1740

# tools/serial-console's own console driver, ported: each (pattern,
# response) fires once, the first time PATTERN matches the TAIL of
# what has been received since the last match. The five steps drive the
# PROM from the System Maintenance Menu through the command monitor to an
# interactive sash prompt, matching diag-gate's own console_driver exactly
# (tools/diag-gate's --expect list, before that tool's retirement).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", "boot -f bootp()/sash64\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# diag-gate's own --until was: printenv's own dlserver line, the run's
# proof that sash64 actually loaded and ran rather than merely being
# requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")
_TAIL_MAX = 8192


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


def _drive_console(sock, log_path, timeout):
    """Answer the PROM's factory-sequence prompts and drive it to sash64.

    Ported from tools/serial-console's own expect loop (see that file's
    docstring for why: commands sent before the firmware is listening are
    lost, and a rule that could refire on a prompt reappearing after being
    answered would send the wrong thing next time). Every byte received is
    both accumulated into the returned transcript and appended to
    log_path, so a run this drove is inspectable exactly like a captured
    -serial file.

    Returns (transcript, reached) -- reached is True once DRIVER_UNTIL_RE
    is seen anywhere in the stream, matching diag-gate's own --until exit.
    """
    rules = [
        {"re": re.compile(pattern.encode()), "response": response.encode(),
         "fired": False}
        for pattern, response in DRIVER_STEPS
    ]
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    tail = b""
    reached = False
    with open(log_path, "wb") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            transcript += chunk
            log.write(chunk)
            log.flush()
            tail = (tail + chunk)[-_TAIL_MAX:]
            for rule in rules:
                if not rule["fired"] and rule["re"].search(tail):
                    sock.sendall(rule["response"])
                    rule["fired"] = True
                    tail = b""
            if DRIVER_UNTIL_RE.search(tail):
                reached = True
                break
    return transcript.decode("utf-8", errors="replace"), reached


def _utc_now():
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    resolved = os.path.realpath(path)
    digest = hashlib.sha256()
    with open(resolved, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": str(path),
        "resolved_path": resolved,
        "sha256": digest.hexdigest(),
        "size_bytes": os.path.getsize(resolved),
    }


def _fork_git_identity(tree):
    """This fork checkout's own commit and dirty state -- the analog of
    diag-gate's git_identity(ROOT), where ROOT there is the project tools
    checkout. This is a functional test inside the fork itself, so there
    is no separate "project tools" checkout to report on; the fork's own
    identity is the binding record instead. None on any git failure: a
    missing identity must not read as a fabricated clean one."""
    try:
        commit = subprocess.run(
            ["git", "-C", str(tree), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30)
        status = subprocess.run(
            ["git", "-C", str(tree), "status", "--porcelain=v1",
             "--untracked-files=normal"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if commit.returncode != 0 or status.returncode != 0:
        return None
    return {
        "path": str(tree),
        "commit": commit.stdout.strip(),
        "dirty": bool(status.stdout.strip()),
    }


if QemuSystemTest is not None:

    # This file's own repo root:
    # tests/functional/mips64/test_prom_diagnostics.py is three directories
    # below it.
    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromDiagnostics(QemuSystemTest):
        """Boots the real IP27 PROM firmware through diag-gate's two-pass
        factory sequence to an interactive sash prompt and requires
        evaluate_run's verdict to be PASS. See this module's docstring for
        the full contract; this class only wires it to a real boot, the
        same division test_prom_boot.py keeps between its pure logic and
        PromBoot.
        """

        def _release_paths(self):
            """The real PROM image and sash64 boot tree under
            ORIGAMI_ASSETS, or None if either is missing -- the runner may
            not have assets provisioned yet (see this method's caller)."""
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            prom = os.path.join(assets_root, FIRMWARE_REL)
            tftp = os.path.join(assets_root, TFTP_REL)
            sash = os.path.join(tftp, 'sash64')
            if not (os.path.isfile(prom) and os.path.isfile(sash)):
                return None
            return prom, tftp

        def test_prom_diagnostics(self):
            paths = self._release_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/sash64 '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{TFTP_REL}/sash64 -- point it at a '
                    'checkout with the real PROM and release-matched '
                    'sash64 extracted (tools/extract-irix) to run this '
                    'test')
            prom, tftp = paths

            vm = self.get_vm(name='prom_diagnostics')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            vm.add_args('-accel', 'tcg,thread=single',
                       *diag_extra_args(prom, tftp, elsc_log, capture))

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('diag.log')
            try:
                transcript, reached = _drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
            finally:
                # A VM that crashed or exited on its own has already left;
                # a graceful shutdown() against an already-dead process
                # still raises AbnormalShutdown even though qemu is in
                # fact gone. Matches PromBoot.test_prom_boot's own
                # reasoning.
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

            if not transcript:
                self.fail(
                    'the firmware printed nothing at all -- a run that '
                    'did not start, not a factory sequence that failed '
                    'early. Check the emulator built, and that nothing '
                    'ended the machine before it printed anything.')

            report = evaluate_run(transcript, EXPECTED_FAILURES)
            if not reached:
                report['problems'].append(
                    'console driver did not reach the download-server '
                    'confirmation before timeout -- the run never '
                    'proved sash64 actually loaded')
                report['verdict'] = 'FAIL'

            summary_path = self.log_file('diag-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({
                    'schema': 1,
                    'expected_failures': list(EXPECTED_FAILURES),
                    'expected_failures_source_sha256': hashlib.sha256(
                        EXPECTED_FAILURES_TEXT.encode()).hexdigest(),
                    'nvram_warnings': 'expected',
                    'identities': extract_identities(transcript),
                    'report': report,
                }, summary_file, indent=2, sort_keys=True)
                summary_file.write('\n')

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1,
                    'started_utc': started,
                    'ended_utc': ended,
                    'console_driver_reached_until': reached,
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'command': {
                        'binary': self.qemu_bin,
                        'machine': (
                            'origin200,synthesized-firmware=off,'
                            'debug-leds=off'),
                        'args': diag_extra_args(
                            prom, tftp, elsc_log, capture),
                    },
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'firmware': {
                            'release': '6.5-base',
                            'prom': _file_record(prom),
                            'sash': _file_record(
                                os.path.join(tftp, 'sash64')),
                        },
                    },
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'diag-gate factory sequence problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
