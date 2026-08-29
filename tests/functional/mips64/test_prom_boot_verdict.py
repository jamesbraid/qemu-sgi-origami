#!/usr/bin/env python3
#
# Equivalence test: test_prom_boot's scrape+verdict logic implements
# tools/boot-gate's set-subset contract, without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`uv run --with pytest pytest tests/functional/mips64/test_prom_boot_verdict.py
-v`. It exercises codes_seen() and mark_reached(), the two pure functions
test_prom_boot.py factors its firmware-boot check into, against fixture
trace text standing in for a captured QEMU log.

This proves the ported logic reproduces tools/boot-gate's own contract (in
the project checkout): the recorded golden mark must be a SUBSET of the
codes a boot reaches, order-independent; missing any mark code is a
failure that names what is missing; codes beyond the mark do not affect
the verdict; and no codes at all is "did not start", not "failed early".
"""

import unittest

from lib_prom import MARK, codes_seen, mark_reached


def _trace_line(code, kind='progress', cpu=0, meaning='test fixture'):
    """One sgi_hub_led trace line, in the exact shape QEMU emits it."""
    return f'sgi_hub_led CPU {cpu}: {code} ({kind}: {meaning})'


class CodesSeenTest(unittest.TestCase):

    def test_extracts_every_code(self):
        text = '\n'.join(_trace_line(c) for c in ('0x00', '0x01', '0x02'))
        self.assertEqual(codes_seen(text), {'0x00', '0x01', '0x02'})

    def test_counts_every_kind_not_just_progress(self):
        # boot-gate's own CODE regex (tools/boot-gate) is not kind-filtered
        # -- a FAILURE or undecoded line counts the same as a progress
        # line. codes_seen() keeps that contract; only
        # lib_prom.first_progress_code (a different pass condition) filters.
        text = '\n'.join([
            _trace_line('0x10', kind='progress'),
            _trace_line('0x11', kind='FAILURE'),
            _trace_line('0x12', kind='undecoded'),
        ])
        self.assertEqual(codes_seen(text), {'0x10', '0x11', '0x12'})

    def test_ignores_unrelated_lines(self):
        text = '\n'.join([
            'some unrelated console banner',
            _trace_line('0x20'),
            'POD Junk Dex> ',
        ])
        self.assertEqual(codes_seen(text), {'0x20'})

    def test_empty_text_yields_empty_set(self):
        self.assertEqual(codes_seen(''), set())


class MarkReachedTest(unittest.TestCase):

    def test_all_mark_codes_present_passes(self):
        text = '\n'.join(_trace_line(c) for c in sorted(MARK))
        ok, missing = mark_reached(codes_seen(text), MARK)
        self.assertTrue(ok)
        self.assertEqual(missing, [])

    def test_missing_one_mark_code_fails_and_names_it(self):
        held_back = sorted(MARK)[len(MARK) // 2]
        present = sorted(MARK - {held_back})
        text = '\n'.join(_trace_line(c) for c in present)
        ok, missing = mark_reached(codes_seen(text), MARK)
        self.assertFalse(ok)
        self.assertEqual(missing, [held_back])

    def test_missing_several_mark_codes_names_all_of_them(self):
        held_back = set(sorted(MARK)[:3])
        present = sorted(MARK - held_back)
        text = '\n'.join(_trace_line(c) for c in present)
        ok, missing = mark_reached(codes_seen(text), MARK)
        self.assertFalse(ok)
        self.assertEqual(missing, sorted(held_back))

    def test_extra_codes_beyond_the_mark_still_pass(self):
        # Subset, not equality: a boot that goes further than the
        # recorded mark is not a regression.
        text = '\n'.join(_trace_line(c) for c in sorted(MARK)) + '\n' + \
            _trace_line('0xfe') + '\n' + _trace_line('0x99')
        ok, missing = mark_reached(codes_seen(text), MARK)
        self.assertTrue(ok)
        self.assertEqual(missing, [])

    def test_empty_transcript_fails_as_did_not_start(self):
        # boot-gate's own "no codes at all" case: every mark code is
        # unreached, not merely some of them -- the caller (this test's
        # counterpart in test_prom_boot.PromBoot.test_prom_boot) gives
        # this specific instance its "did not start" wording, but the
        # underlying subset comparison already fails maximally.
        ok, missing = mark_reached(codes_seen(''), MARK)
        self.assertFalse(ok)
        self.assertEqual(missing, sorted(MARK))

    def test_mark_is_the_47_codes_from_the_golden_file(self):
        # A change to the embedded MARK is a change to what this test
        # (and the real firmware boot) requires -- pin its size so a
        # silent edit is caught here rather than only in a boot's pass/
        # fail. Source: tests/golden/firmware-boot-progress-mark in the
        # project checkout.
        self.assertEqual(len(MARK), 47)


if __name__ == '__main__':
    unittest.main()
