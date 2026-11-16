#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 James Braid

"""scripts/sgi-display-probe.py without a machine: its frame-code decoder,
its measures on logs with known faults, and a session against a stand-in
display server."""

import argparse
import array
import importlib.util
import json
import mmap
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import unittest.mock
from pathlib import Path

from qemu_test import QemuBaseTest

PROBE = Path(__file__).parents[3] / 'scripts' / 'sgi-display-probe.py'
spec = importlib.util.spec_from_file_location('sgi_display_probe', PROBE)
probe = importlib.util.module_from_spec(spec)
sys.modules['sgi_display_probe'] = probe
spec.loader.exec_module(probe)

W, H = 720, 486


def draw_code(fb, n):
    """The pattern's frame code n in fb, a bytearray of BGRX rows."""
    bits = list(probe.MARKER) + [n >> (15 - i) & 1 for i in range(16)] + \
        [probe.code_check(n) >> (7 - i) & 1 for i in range(8)]
    for i, bit in enumerate(bits):
        px = (b'\xff\xff\xff\x00' if bit else bytes(4)) * probe.BLOCK
        for y in range(probe.BLOCK):
            at = ((probe.CODE_Y + y) * W + probe.CODE_X + i * probe.BLOCK) * 4
            fb[at:at + probe.BLOCK * 4] = px


def row_of(n):
    fb = bytearray(W * H * 4)
    draw_code(fb, n)
    y = probe.CODE_Y + probe.BLOCK // 2
    return bytes(fb[y * W * 4:(y + 1) * W * 4])


class FrameCode(QemuBaseTest):
    def test_round_trip(self):
        for n in (0, 1, 0x1234, 0xfffe, 0xffff):
            self.assertEqual(probe.decode_code(row_of(n)), n)

    def test_a_bad_check_or_marker_is_no_code(self):
        row = bytearray(row_of(5))
        last = (probe.CODE_X + 31 * probe.BLOCK + 8) * 4 + 1
        row[last] ^= 0xff                        # flip the last check bit
        self.assertIsNone(probe.decode_code(bytes(row)))
        self.assertIsNone(probe.decode_code(bytes(W * 4)))


class Measures(QemuBaseTest):
    def updates(self, codes, step_ms=33.333, t0=10 * 10 ** 9):
        return [dict(t=int(t0 + i * step_ms * 1e6), code=c)
                for i, c in enumerate(codes)]

    def args(self, **kw):
        base = dict(trace=None, min_fps=None, max_drops=None,
                    max_probe_missed=None, max_repeats=None, max_p99_ms=None)
        return argparse.Namespace(**{**base, **kw})

    def test_clean_run(self):
        out, _, _ = probe.analyze_frames(self.updates(range(300)))
        self.assertEqual((out['drops'], out['repeats'], out['frames']),
                         (0, 0, 300))
        self.assertAlmostEqual(out['fps'], 30, delta=0.01)
        self.assertAlmostEqual(out['intervals_ms']['p99'], 33.333, delta=0.01)

    def test_drops_repeats_and_wrap(self):
        codes = [65530, 65531, 65531, 65533, 65534, 65535, 0, 1, 1, 1, 4]
        out, _, missing = probe.analyze_frames(self.updates(codes))
        self.assertEqual((out['drops'], out['repeats'], out['frames']),
                         (3, 3, 8))
        self.assertEqual(missing, [65532, 2, 3])

    def test_uncoded_updates_are_ignored(self):
        out, _, _ = probe.analyze_frames(self.updates([None, 5, None, 6, 7]))
        self.assertEqual((out['frames'], out['drops'], out['updates']),
                         (3, 0, 5))

    def test_the_trace_log_is_read(self):
        log = ("1@1.0:sgi_display_pattern_frame frame code 7 published at "
               "virtual 233333333 ns, host 5233333333 ns\nnoise\n")
        with tempfile.NamedTemporaryFile('w', suffix='.log') as f:
            f.write(log)
            f.flush()
            self.assertEqual(probe.read_trace(f.name), {7: 5233333333})

    def test_a_drop_is_qemus_only_if_it_never_published_the_code(self):
        ms = 10 ** 6
        ups = self.updates([0, 1, 2, 4, 5, 6, 8, 9])
        log = ''.join(
            '1@1.0:sgi_display_pattern_frame frame code %d published at '
            'virtual 0 ns, host %d ns\n' % (c, ups[0]['t'] + c * 33 * ms)
            for c in (0, 1, 2, 3, 4, 5, 6, 8, 9))     # 7 was never drawn
        with tempfile.NamedTemporaryFile('w', suffix='.log') as f:
            f.write(log)
            f.flush()
            out = probe.summarize(ups, self.args(
                trace=f.name, max_drops=0, max_probe_missed=0))
        self.assertEqual((out['drops'], out['unpublished'],
                          out['probe_missed']), (2, 1, 1))
        self.assertEqual(len(out['failures']), 2)

    def test_bounds_fail_the_run(self):
        out = probe.summarize(self.updates([0, 1, 3, 4]),
                              self.args(min_fps=29.5, max_drops=0))
        self.assertEqual(len(out['failures']), 1)
        self.assertIn('1 drops', out['failures'][0])


class FakeDisplay:
    """The QEMU side of protocol 3 for one head with a counting code."""

    def __init__(self, path, frames, period=0.02):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        self.sock.listen(1)
        self.frames, self.period = frames, period
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def region(size):
        f = tempfile.TemporaryFile()
        f.truncate(size)
        return f, mmap.mmap(f.fileno(), size)

    @staticmethod
    def send(conn, data, fd):
        conn.sendmsg([data[:8]], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                   array.array('i', [fd]))])
        conn.sendall(data[8:])

    def run(self):
        conn, _ = self.sock.accept()
        conn.recv(48, socket.MSG_WAITALL)
        cf, ctl = self.region(65536)
        ff, frame = self.region(65536 * 23)
        struct.pack_into('<IIIIII', ctl, 0, probe.MAGIC, 3, 1, 4096, 4096, 1)
        h = 4096
        struct.pack_into('<IIIII', ctl, h, 1, 1, 1, 0, W)
        struct.pack_into('<II', ctl, h + 20, H, 30000)
        struct.pack_into('<IIQIII', ctl, h + 80, 1, 1, 0, W, H, W * 4)
        self.send(conn, struct.pack('<HHIIIII16s16sQ', 0x81, 0, 0, 3, 3, 1,
                                    3, bytes(16), bytes(16), 0), cf.fileno())
        self.send(conn, struct.pack('<HHIIIIIQQIIQ', 0x82, 0, 0, 1, 1, 3, 0,
                                    65536 * 23, 65536 * 23, 0, 0, 0),
                  ff.fileno())
        fb = bytearray(W * H * 4)
        gen = 1
        for n in range(self.frames):
            time.sleep(self.period)
            draw_code(fb, n)
            frame[:] = fb[:len(frame)] if len(fb) >= len(frame) else \
                fb + bytes(len(frame) - len(fb))
            gen += 1
            struct.pack_into('<Q', ctl, h + 32, gen)
            if struct.unpack_from('<I', ctl, h + 44)[0] == 1:
                struct.pack_into('<I', ctl, h + 44, 0)
                conn.sendall(struct.pack('<HHIII', 0x84, 0, 0, 0, 0))
        conn.close()
        self.sock.close()
        cf.close()
        ff.close()


class Session(QemuBaseTest):
    def test_record_follows_content_gen_and_wake(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'd.sock')
            server = FakeDisplay(path, 60)
            out = os.path.join(tmp, 'rec')
            ns = argparse.Namespace(
                socket=path, out=out, duration=1.6, trace=None, min_fps=None,
                max_drops=0, max_probe_missed=None, max_repeats=0,
                max_p99_ms=None)
            with unittest.mock.patch('builtins.print'):
                status = probe.cmd_record(ns)
            server.thread.join(5)
            summary = json.loads(Path(out, 'summary.json').read_text())
        self.assertEqual(status, 0, summary['failures'])
        self.assertEqual(summary['drops'], 0)
        self.assertGreaterEqual(summary['frames'], 55)


if __name__ == '__main__':
    QemuBaseTest.main()
