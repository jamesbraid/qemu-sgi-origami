# Translation-storm detection: read QEMU's own `info jit` counters and
# decide whether a guest is stuck translating one throwaway instruction
# per block instead of making real progress
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
When a guest CPU fetches instructions from a region this model treats as
I/O instead of RAM or ROM, TCG translates each instruction as a throwaway
one-instruction block and never caches it. The guest still makes
progress, 100 to 300 times slower, with no symptom besides "this boot is
taking forever" -- which reads exactly like an ordinary slow boot until
someone runs `info jit` by hand and notices the counters. This cost a day
on Origin 300 bring-up (the PROM reset window was modeled as an I/O
region) and had already hit Origin 200 bring-up once before, both in the
Origami project checkout, not this fork.

This is a plain port of that project's own `tools/origami_lib/
translation_health.py` (its docstring carries the full narrative and the
live captures the thresholds come from) into this fork's functional-test
tree, split the same way lib_prom.py already is from the *_verdict.py
companions it feeds: plain Python, no qemu_test import, so
test_lib_translation_health.py runs under a bare `python3` or `pytest`.
`evaluate()` turns one `info jit` sample into a healthy/sick/instrumented
verdict, `render()` prints the numbers behind it, `launch_skip_reason()`
recognizes a run already started under a mode that produces the same
numbers on purpose, and `assert_translation_healthy()` is the one call a
boot test's own console driver makes to turn a sick verdict into a
failure that names the check instead of a timeout with no diagnosis.

QEMU owns these regression thresholds. The representative counter captures
in test_lib_translation_health.py preserve their rationale without requiring
an Origami checkout. Review threshold changes against those captures and a
measured failing run.
"""

from __future__ import annotations

import enum
import logging
import re
from collections.abc import Sequence
from dataclasses import dataclass


SICK_AVG_TARGET_SIZE_MAX_BYTES = 8
SICK_MAX_TARGET_SIZE_MAX_BYTES = 8
SICK_ZERO_OCCUPANCY_MIN_BLOCKS = 1_000_000


class Verdict(enum.Enum):
    """A sample reads as exactly one of these, never two at once."""

    HEALTHY = "healthy"
    SICK = "sick"
    # The sample itself reports a mode that changes block sizing on
    # purpose (one-insn-per-tb, icount); this check does not judge it.
    INSTRUMENTED = "instrumented"


@dataclass(frozen=True)
class TranslationHealth:
    """One verdict from one ``info jit`` sample, with the numbers behind it.

    Every optional field is None when its own line was absent from the
    sample -- a caller should not treat a missing figure as zero.
    """

    verdict: Verdict
    reason: str
    tb_count: int
    avg_target_size: int
    max_target_size: int
    hash_used_buckets: int | None
    hash_total_buckets: int | None
    hash_occupancy_percent: float | None
    tb_flush_count: int | None
    gen_code_size: int | None
    gen_code_capacity: int | None
    one_insn_per_tb: bool
    icount_enabled: bool


def _search(pattern: str, text: str) -> re.Match[str] | None:
    return re.search(pattern, text, re.MULTILINE)


def evaluate(text: str) -> TranslationHealth:
    """Parse one ``human-monitor-command info jit`` reply and decide.

    Raises ValueError when the text does not carry the two lines this
    check cannot do without -- the caller passed the wrong command's
    reply, the guest has not translated anything yet, or QEMU changed the
    format underneath this parser.
    """
    one_insn_per_tb = _search(r"^one-insn-per-tb:\s*on\s*$", text) is not None
    icount_enabled = "Host - Guest clock" in text

    count_match = _search(r"^TB count\s+(\d+)\s*$", text)
    size_match = _search(
        r"^TB avg target size\s+(\d+)\s+max=(\d+)\s+bytes\s*$", text
    )
    if count_match is None or size_match is None:
        raise ValueError(
            "does not look like 'info jit' output: missing the 'TB count' "
            "or 'TB avg target size' line"
        )
    tb_count = int(count_match.group(1))
    avg_target_size = int(size_match.group(1))
    max_target_size = int(size_match.group(2))

    hash_match = _search(r"^TB hash buckets\s+(\d+)/(\d+)\s", text)
    hash_used = int(hash_match.group(1)) if hash_match else None
    hash_total = int(hash_match.group(2)) if hash_match else None

    occupancy_match = _search(r"^TB hash occupancy\s+([\d.]+)%\s", text)
    hash_occupancy = (
        float(occupancy_match.group(1)) if occupancy_match else None
    )

    flush_match = _search(r"^TB flush count\s+(\d+)\s*$", text)
    tb_flush_count = int(flush_match.group(1)) if flush_match else None

    gen_match = _search(r"^gen code size\s+(\d+)/(\d+)\s*$", text)
    gen_code_size = int(gen_match.group(1)) if gen_match else None
    gen_code_capacity = int(gen_match.group(2)) if gen_match else None

    common = {
        "tb_count": tb_count,
        "avg_target_size": avg_target_size,
        "max_target_size": max_target_size,
        "hash_used_buckets": hash_used,
        "hash_total_buckets": hash_total,
        "hash_occupancy_percent": hash_occupancy,
        "tb_flush_count": tb_flush_count,
        "gen_code_size": gen_code_size,
        "gen_code_capacity": gen_code_capacity,
        "one_insn_per_tb": one_insn_per_tb,
        "icount_enabled": icount_enabled,
    }

    if one_insn_per_tb:
        return TranslationHealth(
            verdict=Verdict.INSTRUMENTED,
            reason="the accelerator reports one-insn-per-tb: on, so every "
                   "block is one instruction on purpose",
            **common,
        )
    if icount_enabled:
        return TranslationHealth(
            verdict=Verdict.INSTRUMENTED,
            reason="icount is enabled, which sizes blocks to the "
                   "instruction budget on purpose",
            **common,
        )

    reasons = []
    if (avg_target_size <= SICK_AVG_TARGET_SIZE_MAX_BYTES
            and max_target_size <= SICK_MAX_TARGET_SIZE_MAX_BYTES):
        reasons.append(
            f"TB avg target size {avg_target_size} bytes, max "
            f"{max_target_size} bytes (both <= "
            f"{SICK_AVG_TARGET_SIZE_MAX_BYTES})"
        )
    if (hash_used == 0 and hash_total
            and tb_count >= SICK_ZERO_OCCUPANCY_MIN_BLOCKS):
        reasons.append(
            f"TB hash buckets 0/{hash_total} used after {tb_count} blocks"
        )

    if reasons:
        return TranslationHealth(
            verdict=Verdict.SICK, reason="; ".join(reasons), **common,
        )
    return TranslationHealth(
        verdict=Verdict.HEALTHY, reason="within thresholds", **common,
    )


def render(result: TranslationHealth) -> str:
    """The numbers behind one verdict, for a person or a check's stderr."""
    hash_bit = (
        f"{result.hash_used_buckets}/{result.hash_total_buckets}"
        if result.hash_total_buckets is not None else "unknown"
    )
    occupancy_bit = (
        f"{result.hash_occupancy_percent:.2f}%"
        if result.hash_occupancy_percent is not None else "unknown"
    )
    flush_bit = (
        str(result.tb_flush_count)
        if result.tb_flush_count is not None else "unknown"
    )
    gen_bit = (
        f"{result.gen_code_size}/{result.gen_code_capacity}"
        if result.gen_code_capacity is not None else "unknown"
    )
    return (
        f"verdict: {result.verdict.value} ({result.reason})\n"
        f"TB count: {result.tb_count}\n"
        f"TB avg target size: {result.avg_target_size} bytes "
        f"max={result.max_target_size} bytes\n"
        f"TB hash buckets used: {hash_bit}\n"
        f"TB hash occupancy: {occupancy_bit}\n"
        f"TB flush count: {flush_bit}\n"
        f"gen code size: {gen_bit}\n"
        f"one-insn-per-tb: {'on' if result.one_insn_per_tb else 'off'}\n"
        f"icount enabled: {result.icount_enabled}"
    )


def launch_skip_reason(argv: Sequence[str]) -> str | None:
    """Whether ``argv`` already runs this launch under a mode this check
    must not judge, read from the run's own recorded QEMU arguments rather
    than its ``info jit`` sample (which does not self-report these).

    Returns None when nothing in ``argv`` asks for one of those modes --
    which is not the same as a clean bill of health, only that a caller
    should go on and sample the run.
    """
    items = list(argv)
    for flag, value in zip(items, items[1:]):
        if flag != "-d":
            continue
        tokens = value.split(",")
        for token, label in (
            ("exec", "-d exec"),
            ("in_asm", "-d in_asm"),
            ("nochain", "-d nochain"),
        ):
            if token in tokens:
                return f"the run was started with {label}"
    if "-singlestep" in items:
        return "the run was started with -singlestep"
    if "-gdb" in items or "-s" in items:
        return "the run was started with a gdb server"
    return None


_LOG = logging.getLogger("qemu-test")


def assert_translation_healthy(vm) -> None:
    """Sample ``vm``'s ``info jit`` counters once and fail loudly on a
    translation storm, naming this check by name so a sick guest fails
    fast instead of a boot test spending its whole timeout on a guest
    running 100 to 300 times slower than it should.

    ``vm`` is anything with QEMUMachine's own ``args`` (the recorded
    launch argument list) and ``cmd()`` (a QMP call that raises on
    failure) -- a real QEMUMachine in a boot test, or a stand-in in a
    unit test.

    Skips silently, logging why at info level, whenever the run cannot be
    judged: launched under one of the modes launch_skip_reason() names,
    the sample itself reports one-insn-per-tb or icount (a run whose
    accelerator was reconfigured after launch, or an icount mode
    launch_skip_reason() does not itself recognize from argv), the guest
    is not currently running (a paused guest, including one a debugger
    has stopped), or the guest has not translated anything yet.
    """
    skip = launch_skip_reason(vm.args)
    if skip is not None:
        _LOG.info("translation health: skipped (%s)", skip)
        return

    status = vm.cmd("query-status")
    if not status.get("running"):
        _LOG.info(
            "translation health: skipped (guest is not running: %s)",
            status.get("status", "unknown"),
        )
        return

    text = vm.cmd("human-monitor-command", command_line="info jit")
    try:
        result = evaluate(text)
    except ValueError as exc:
        _LOG.info("translation health: skipped (%s)", exc)
        return

    if result.verdict is Verdict.INSTRUMENTED:
        _LOG.info("translation health: skipped (%s)", result.reason)
        return

    _LOG.info("translation health: %s", render(result))
    if result.verdict is Verdict.SICK:
        raise AssertionError(
            "translation health: sick -- one instruction is being "
            "translated per block instead of real progress (see "
            "docs/known-issues.md in the Origami project checkout)\n"
            + render(result)
        )
