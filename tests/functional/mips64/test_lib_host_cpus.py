#!/usr/bin/env python3
#
# Unit tests for lib_host_cpus: choosing host CPUs and pinning a QEMU's
# threads to them
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import. /proc is a directory
tree of task/<tid>/comm files and the affinity calls are recorded rather
than made.
"""

import logging
import os
import tempfile
import unittest

import lib_host_cpus as lib
from lib_net import HarnessError


PID = 4242


class FakeProc:
    """A /proc holding one process's threads, and the affinity calls
    made on them."""

    def __init__(self, names):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        for tid, name in names.items():
            self.add(tid, name)
        self.affinity = {}

    def add(self, tid, name):
        task = os.path.join(self.root, str(PID), 'task', str(tid))
        os.makedirs(task)
        with open(os.path.join(task, 'comm'), 'w', encoding='utf-8') as comm:
            comm.write(name + '\n')

    def setaffinity(self, tid, cpus):
        self.affinity[tid] = set(cpus)

    def getaffinity(self, tid):
        return self.affinity[tid]

    def close(self):
        self.dir.cleanup()


def qemu_threads(vcpus):
    """A QEMU's threads as MTTCG names them: main, RCU, then one per
    vCPU."""
    names = {PID: 'qemu-system-mip', PID + 1: 'call_rcu'}
    for index in range(vcpus):
        names[PID + 10 + index] = f'CPU {index}/TCG'
    return names


class ChooseTests(unittest.TestCase):

    def test_the_top_of_the_allowed_set_comes_first(self):
        self.assertEqual(lib.choose_cpus(3, {0, 1, 2, 3, 8, 9}, {}),
                         [9, 8, 3])

    def test_the_environment_names_the_cpus_in_its_order(self):
        env = {lib.ENV: '4,2-3'}
        self.assertEqual(lib.choose_cpus(5, set(range(8)), env), [4, 2, 3])

    def test_the_environment_may_not_leave_the_allowed_set(self):
        with self.assertRaisesRegex(HarnessError, r'\[9\]'):
            lib.choose_cpus(2, {0, 1}, {lib.ENV: '1,9'})

    def test_bad_lists_are_refused(self):
        for text in ('a', '3-1', '1,,2'):
            with self.subTest(text=text):
                with self.assertRaises(HarnessError):
                    lib.parse_cpu_list(text)


class PinTests(unittest.TestCase):

    def setUp(self):
        self.log = logging.getLogger('test_lib_host_cpus')

    def _pin(self, proc, vcpus, allowed, environ=None, **wait):
        return lib.pin(PID, vcpus, self.log, proc=proc.root,
                       environ=environ or {}, allowed=allowed,
                       setaffinity=proc.setaffinity,
                       getaffinity=proc.getaffinity, **wait)

    def test_each_vcpu_gets_its_own_cpu_and_the_rest_share_one(self):
        proc = FakeProc(qemu_threads(4))
        self.addCleanup(proc.close)
        with self.assertLogs(self.log, 'INFO') as logged:
            pinned = self._pin(proc, 4, set(range(32)))
        self.assertEqual(proc.affinity, {
            PID + 10: {31}, PID + 11: {30}, PID + 12: {29}, PID + 13: {28},
            PID: {27}, PID + 1: {27}})
        self.assertEqual(pinned[PID + 12], ('CPU 2/TCG', 29))
        self.assertIn(f'thread {PID + 12} "CPU 2/TCG" affinity {{29}}',
                      '\n'.join(logged.output))

    def test_vcpus_are_placed_by_index_not_thread_id(self):
        proc = FakeProc({PID: 'qemu-system-mip', PID + 20: 'CPU 1/TCG',
                         PID + 30: 'CPU 0/TCG'})
        self.addCleanup(proc.close)
        self._pin(proc, 2, {5, 6, 7})
        self.assertEqual(proc.affinity,
                         {PID + 30: {7}, PID + 20: {6}, PID: {5}})

    def test_too_few_cpus_share_round_robin_and_say_so(self):
        proc = FakeProc(qemu_threads(4))
        self.addCleanup(proc.close)
        with self.assertLogs(self.log, 'WARNING') as logged:
            self._pin(proc, 4, {0, 1, 2})
        self.assertEqual(proc.affinity, {
            PID + 10: {2}, PID + 11: {1}, PID + 12: {0}, PID + 13: {2},
            PID: {1}, PID + 1: {1}})
        self.assertIn('only 3 host CPUs', '\n'.join(logged.output))

    def test_missing_vcpu_threads_are_an_error(self):
        proc = FakeProc({PID: 'qemu-system-mip', PID + 1: 'ALL CPUs/TCG'})
        self.addCleanup(proc.close)
        clock = iter(range(100))
        with self.assertRaisesRegex(HarnessError, 'needs MTTCG'):
            self._pin(proc, 2, {0, 1, 2}, sleep=lambda _: None,
                      clock=lambda: next(clock), timeout=3)
        self.assertEqual(proc.affinity, {})

    def test_a_late_vcpu_thread_is_waited_for(self):
        proc = FakeProc(qemu_threads(1))
        self.addCleanup(proc.close)
        self._pin(proc, 2, {0, 1, 2},
                  sleep=lambda _: proc.add(PID + 11, 'CPU 1/TCG'),
                  clock=lambda: 0)
        self.assertEqual(proc.affinity[PID + 11], {1})

    def test_a_thread_that_exits_mid_pin_is_skipped(self):
        proc = FakeProc(qemu_threads(1))
        self.addCleanup(proc.close)
        setaffinity = proc.setaffinity

        def vanishing(tid, cpus):
            if tid == PID + 1:
                raise ProcessLookupError(tid)
            setaffinity(tid, cpus)

        proc.setaffinity = vanishing
        pinned = self._pin(proc, 1, {0, 1})
        self.assertNotIn(PID + 1, pinned)
        self.assertEqual(proc.affinity, {PID + 10: {1}, PID: {0}})

    def test_an_affinity_the_kernel_did_not_keep_is_an_error(self):
        proc = FakeProc(qemu_threads(1))
        self.addCleanup(proc.close)
        proc.setaffinity = lambda tid, cpus: proc.affinity.update(
            {tid: {0, 1}})
        with self.assertRaisesRegex(HarnessError, r'runs on \[0, 1\]'):
            self._pin(proc, 1, {0, 1})


if __name__ == '__main__':
    unittest.main()
