#!/usr/bin/env python3
#
# Unit tests for the host-side IRIX eg throughput helpers.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import sys
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_irix_eg_perf as perf  # noqa: E402


class _FakeSocket:
    def __init__(self, clock, lock):
        self.clock = clock
        self.lock = lock
        self.closed = False

    def settimeout(self, timeout):
        pass

    def sendall(self, payload):
        with self.lock:
            self.clock[0] += 1

    def shutdown(self, how):
        pass

    def recv(self, size):
        return b""

    def close(self):
        self.closed = True


class ParallelSendTest(unittest.TestCase):
    def test_connection_setup_is_outside_the_measured_interval(self):
        clock = [0]
        lock = threading.Lock()
        measurement_started = threading.Event()

        def connect(address, timeout):
            if threading.current_thread() is not threading.main_thread():
                measurement_started.wait(5)
            with lock:
                clock[0] += 10
            return _FakeSocket(clock, lock)

        def monotonic():
            measurement_started.set()
            with lock:
                return clock[0]

        with mock.patch.object(perf.socket, "create_connection", connect), \
             mock.patch.object(perf.time, "monotonic", monotonic):
            rate, counts = perf._parallel_send([5001, 5002], [b"a", b"b"])

        self.assertEqual(counts, [1, 1])
        self.assertEqual(rate, 8 / 1e6)

    def test_failed_setup_closes_an_open_socket(self):
        opened = _FakeSocket([0], threading.Lock())
        calls = iter((opened, ConnectionRefusedError("refused")))

        def connect(address, timeout):
            result = next(calls)
            if isinstance(result, Exception):
                raise result
            return result

        with mock.patch.object(perf.socket, "create_connection", connect):
            with self.assertRaisesRegex(perf.base._Harness, "refused"):
                perf._parallel_send([5001, 5002], [b"a", b"b"])

        self.assertTrue(opened.closed)


if __name__ == "__main__":
    unittest.main()
