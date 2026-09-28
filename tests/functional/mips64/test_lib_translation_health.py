#!/usr/bin/env python3
#
# Translation-storm detection contracts, from captured 'info jit' text and
# a fake vm object standing in for QEMUMachine
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_translation_health.py` (or `uv
run --with pytest pytest tests/functional/mips64/test_lib_translation_health
.py -v`), the same way test_lib_prom.py tests lib_prom.py.

EvaluateTests and LaunchSkipReasonTests pin evaluate()/render() and
launch_skip_reason() against two captured HMP ``info jit`` responses from
QEMU using a development harness. SICK_SAMPLE is from an Origin 300
PROM run about ten seconds after launch, with its reset window modeled as
I/O; HEALTHY_SAMPLE is from an Origin 200 PROM run at about the same point.
The captures preserve the observed counter values. Their acquisition
environment and exact QEMU revision are not recorded in this QEMU tree, so
the fixtures establish parser behavior and illustrate the thresholds; they
do not establish a reproducible machine-level regression.

AssertTranslationHealthyTests covers assert_translation_healthy() against
a _FakeVM standing in for QEMUMachine: it records every QMP call it
receives, so these tests can also prove a skip happens BEFORE any
judgment is made (no query-status, no info jit sample) rather than after
one that was then discarded.
"""

import unittest

from lib_translation_health import (
    Verdict,
    assert_translation_healthy,
    evaluate,
    launch_skip_reason,
    render,
)


# Captured from QEMU HMP `info jit` using a development harness,
# about ten seconds into an Origin 300 PROM run. Its reset window was modeled
# as an I/O region. Exact QEMU revision and capture environment are not
# recorded in this QEMU tree.
SICK_SAMPLE = """\
Accelerator settings:
one-insn-per-tb: off

Translation buffer state:
gen code size       1018469075/1073659904
TB count            3121546
TB avg target size  4 max=4 bytes
TB avg host size    87 bytes (expansion ratio: 21.8)
cross page TB count 0 (0%)
direct jump count   3121520 (99%) (2 jumps=152905 4%)
TB hash buckets     0/8192 (0.00% head buckets used)
TB hash occupancy   0.00% avg chain occ. Histogram: [0.0,0.0)%|█|[0.0,0.0]%

Statistics:
TB flush count      1
TB invalidate count 0
TLB full flushes    0
TLB partial flushes 4
TLB elided flushes  1500
"""

# Captured from QEMU HMP `info jit` using a development harness,
# about ten seconds into an Origin 200 PROM run using the stock IP27 PROM.
# Exact QEMU revision and capture environment are not recorded in this QEMU
# tree.
HEALTHY_SAMPLE = """\
Accelerator settings:
one-insn-per-tb: off

Translation buffer state:
gen code size       9559555/1073736704
TB count            18265
TB avg target size  19 max=1108 bytes
TB avg host size    272 bytes (expansion ratio: 13.8)
cross page TB count 0 (0%)
direct jump count   11591 (63%) (2 jumps=6107 33%)
TB hash buckets     7317/8192 (89.32% head buckets used)
TB hash occupancy   50.33% avg chain occ. Histogram: [0,10)%|\u2583 \u2587  \
\u2588\u2582\u2586\u2581\u2583|[90,100]%
TB hash avg chain   1.087 buckets. Histogram: 1|\u2588\u2581\u2581|3

Statistics:
TB flush count      0
TB invalidate count 0
TLB full flushes    0
TLB partial flushes 118
TLB elided flushes  3946
"""

ONE_INSN_PER_TB_SAMPLE = SICK_SAMPLE.replace(
    "one-insn-per-tb: off", "one-insn-per-tb: on"
)

ICOUNT_SAMPLE = HEALTHY_SAMPLE.replace(
    "TB avg target size  19 max=1108 bytes",
    "TB avg target size  3 max=3 bytes",
) + (
    "Host - Guest clock  0 ms\n"
    "Max guest delay     0 ms\n"
    "Max guest advance   0 ms\n"
)


class EvaluateTests(unittest.TestCase):
    def test_the_origin_300_capture_reads_sick_on_both_thresholds(self) -> None:
        result = evaluate(SICK_SAMPLE)

        self.assertEqual(result.verdict, Verdict.SICK)
        self.assertIn("TB avg target size 4 bytes, max 4 bytes", result.reason)
        self.assertIn(
            "TB hash buckets 0/8192 used after 3121546", result.reason)
        self.assertEqual(result.tb_count, 3121546)
        self.assertEqual(result.avg_target_size, 4)
        self.assertEqual(result.max_target_size, 4)
        self.assertEqual(result.hash_used_buckets, 0)
        self.assertEqual(result.hash_total_buckets, 8192)
        self.assertEqual(result.tb_flush_count, 1)
        self.assertFalse(result.one_insn_per_tb)
        self.assertFalse(result.icount_enabled)

    def test_the_origin_200_capture_reads_healthy(self) -> None:
        result = evaluate(HEALTHY_SAMPLE)

        self.assertEqual(result.verdict, Verdict.HEALTHY)
        self.assertEqual(result.reason, "within thresholds")
        self.assertEqual(result.avg_target_size, 19)
        self.assertEqual(result.max_target_size, 1108)
        self.assertEqual(result.hash_used_buckets, 7317)
        self.assertEqual(result.hash_total_buckets, 8192)
        self.assertAlmostEqual(result.hash_occupancy_percent, 50.33)

    def test_one_insn_per_tb_on_is_instrumented_not_sick(self) -> None:
        result = evaluate(ONE_INSN_PER_TB_SAMPLE)

        self.assertEqual(result.verdict, Verdict.INSTRUMENTED)
        self.assertIn("one-insn-per-tb: on", result.reason)
        self.assertTrue(result.one_insn_per_tb)

    def test_icount_enabled_is_instrumented_even_with_sick_looking_sizes(
        self,
    ) -> None:
        result = evaluate(ICOUNT_SAMPLE)

        self.assertEqual(result.verdict, Verdict.INSTRUMENTED)
        self.assertIn("icount", result.reason)
        self.assertTrue(result.icount_enabled)

    def test_small_block_count_below_the_millions_threshold_is_not_sick(
        self,
    ) -> None:
        early = SICK_SAMPLE.replace(
            "TB count            3121546", "TB count            40"
        ).replace(
            "TB avg target size  4 max=4 bytes",
            "TB avg target size  19 max=40 bytes",
        )

        result = evaluate(early)

        self.assertEqual(result.verdict, Verdict.HEALTHY)

    def test_missing_tb_count_line_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "does not look like"):
            evaluate("Accelerator settings:\none-insn-per-tb: off\n")

    def test_render_names_the_check_and_the_numbers(self) -> None:
        text = render(evaluate(SICK_SAMPLE))

        self.assertIn("verdict: sick", text)
        self.assertIn("TB count: 3121546", text)
        self.assertIn("TB hash buckets used: 0/8192", text)
        self.assertIn("TB flush count: 1", text)


class LaunchSkipReasonTests(unittest.TestCase):
    def test_plain_argv_is_not_skipped(self) -> None:
        self.assertIsNone(
            launch_skip_reason(("-M", "origin300", "-cpu", "R14000")))

    def test_dash_d_exec_is_skipped(self) -> None:
        reason = launch_skip_reason(("-d", "exec,cpu"))
        self.assertIn("-d exec", reason)

    def test_dash_d_in_asm_is_skipped(self) -> None:
        reason = launch_skip_reason(("-d", "in_asm"))
        self.assertIn("-d in_asm", reason)

    def test_dash_d_nochain_is_skipped(self) -> None:
        reason = launch_skip_reason(("-d", "unimp,nochain"))
        self.assertIn("-d nochain", reason)

    def test_dash_d_unimp_alone_is_not_skipped(self) -> None:
        self.assertIsNone(launch_skip_reason(("-d", "unimp,guest_errors")))

    def test_singlestep_is_skipped(self) -> None:
        reason = launch_skip_reason(("-M", "origin200", "-singlestep"))
        self.assertIn("-singlestep", reason)

    def test_dash_gdb_is_skipped(self) -> None:
        reason = launch_skip_reason(("-gdb", "tcp::1234"))
        self.assertIn("gdb", reason)

    def test_dash_s_is_skipped(self) -> None:
        reason = launch_skip_reason(("-M", "origin200", "-s"))
        self.assertIn("gdb", reason)


class _FakeVM:
    """Stands in for QEMUMachine: the two calls assert_translation_healthy()
    makes are `.args` (a plain list, matching QEMUMachine's own args
    property) and `.cmd(name, **kwargs)` (matching QEMUMachine.cmd(), which
    returns the QMP reply and raises on failure). Records every call it
    receives so a test can prove a skip happened before any QMP round trip,
    not merely that judgment was discarded after one.
    """

    def __init__(self, argv=(), running=True, jit_text=HEALTHY_SAMPLE):
        self.args = list(argv)
        self.running = running
        self.jit_text = jit_text
        self.calls = []

    def cmd(self, name, **kwargs):
        self.calls.append((name, kwargs))
        if name == "query-status":
            return {"running": self.running,
                    "status": "running" if self.running else "paused"}
        if name == "human-monitor-command":
            return self.jit_text
        raise AssertionError(f"unexpected QMP command for a fake vm: {name!r}")


class AssertTranslationHealthyTests(unittest.TestCase):
    def test_a_sick_sample_fails_naming_the_check_and_the_numbers(self) -> None:
        vm = _FakeVM(jit_text=SICK_SAMPLE)

        with self.assertRaises(AssertionError) as caught:
            assert_translation_healthy(vm)

        message = str(caught.exception)
        self.assertIn("translation health", message)
        self.assertIn("sick", message)
        self.assertIn("TB count: 3121546", message)
        self.assertIn("TB hash buckets used: 0/8192", message)
        self.assertIn("docs/devel/testing/sgi.rst", message)

    def test_a_healthy_sample_passes(self) -> None:
        vm = _FakeVM(jit_text=HEALTHY_SAMPLE)

        assert_translation_healthy(vm)  # must not raise

        self.assertEqual(
            [name for name, _kwargs in vm.calls],
            ["query-status", "human-monitor-command"])

    def test_an_instrumented_sample_is_skipped_not_failed(self) -> None:
        vm = _FakeVM(jit_text=ONE_INSN_PER_TB_SAMPLE)

        assert_translation_healthy(vm)  # must not raise despite tiny blocks

    def test_a_debug_flag_skips_before_any_qmp_call(self) -> None:
        vm = _FakeVM(argv=("-d", "exec"), jit_text=SICK_SAMPLE)

        assert_translation_healthy(vm)  # must not raise: skipped, not judged

        self.assertEqual(vm.calls, [])

    def test_a_gdb_server_skips_before_any_qmp_call(self) -> None:
        vm = _FakeVM(argv=("-gdb", "tcp::1234"), jit_text=SICK_SAMPLE)

        assert_translation_healthy(vm)

        self.assertEqual(vm.calls, [])

    def test_a_paused_guest_is_never_sampled(self) -> None:
        vm = _FakeVM(running=False, jit_text=SICK_SAMPLE)

        assert_translation_healthy(vm)  # must not raise: never sampled

        self.assertEqual(
            [name for name, _kwargs in vm.calls], ["query-status"])

    def test_a_guest_with_nothing_translated_yet_is_skipped(self) -> None:
        vm = _FakeVM(jit_text="Accelerator settings:\none-insn-per-tb: off\n")

        assert_translation_healthy(vm)  # must not raise: cannot judge yet


if __name__ == "__main__":
    unittest.main()
