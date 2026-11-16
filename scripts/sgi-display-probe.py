#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
Headless client for the SGI shared-memory display (protocol version 3, see
docs/system/mips/sgi/display-shm.rst), standard library only. It records
when a head's picture changes, decodes the test pattern's frame code, and
reports frame rate, skipped codes, repeats and frame intervals.

  sgi-display-probe.py record --socket PATH --out DIR [--duration S]
                              [--trace FILE]
  sgi-display-probe.py analyze DIR [--trace FILE]

Times are CLOCK_MONOTONIC in nanoseconds, the clock QEMU's get_clock()
reads. --trace is QEMU's log (-D FILE) with sgi_display_pattern_frame
enabled, which says when each frame was published. Linux; macOS shares the
SCM_RIGHTS code but has not been run.
"""

import argparse
import array
import json
import mmap
import os
import re
import select
import signal
import socket
import struct
import sys
import time

VERSION, MAGIC = 3, 0x33535347
MSG_HELLO, MSG_WELCOME, MSG_REGION = 0x0001, 0x0081, 0x0082
MSG_REGION_GONE, MSG_WAKE = 0x0083, 0x0084
MSG_LEN = {MSG_WELCOME: 64, MSG_REGION: 56, 0x0083: 16, 0x0084: 16,
           0x0085: 8}
CAP_WAKE = 1 << 2
HEAD_OFFSET, HEAD_BYTES = 4096, 4096
SEQLOCK_SECONDS = 1.0
FMT_X8R8G8B8 = 1

# The frame code the test pattern draws: a marker, a 16-bit number and its
# check, each bit a 16 pixel block with the first at (16, 16).
MARKER = (1, 0, 1, 1, 0, 0, 1, 0)
CODE_X, CODE_Y, BLOCK, CODE_BITS = 16, 16, 16, 32


def host_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def code_check(n):
    return ((n >> 8) ^ n ^ 0xa5) & 0xff


def decode_code(line):
    """The frame number in one row of BGRX pixels through the code, or
    None if the marker or the check does not match."""
    bits = [1 if line[(CODE_X + BLOCK * i + BLOCK // 2) * 4 + 1] >= 128
            else 0 for i in range(CODE_BITS)]
    if tuple(bits[:8]) != MARKER:
        return None
    n = int("".join(map(str, bits[8:24])), 2)
    if int("".join(map(str, bits[24:])), 2) != code_check(n):
        return None
    return n


class Display:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.fds = []
        self.regions = {}
        hello = struct.pack("<HHIIIII16sQ", MSG_HELLO, 0, 0, VERSION,
                            CAP_WAKE, os.getpid(), 0, bytes(16), 0)
        self.sock.sendall(hello)
        msg, fd = self.recv(10)
        if struct.unpack_from("<HI", msg, 0)[0] != MSG_WELCOME or \
                struct.unpack_from("<I", msg, 8)[0] != VERSION:
            raise ConnectionError("not a version 3 WELCOME")
        self.control = mmap.mmap(fd, 65536)
        os.close(fd)
        magic, version, self.nheads = struct.unpack_from("<III", self.control)
        if magic != MAGIC or version != VERSION:
            raise ConnectionError("bad control block")

    def _read(self, n, timeout):
        if timeout is not None and not select.select(
                [self.sock], [], [], timeout)[0]:
            raise TimeoutError
        data, anc, _, _ = self.sock.recvmsg(n, socket.CMSG_SPACE(4))
        for level, kind, payload in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds = array.array("i")
                fds.frombytes(payload[:len(payload) - len(payload) % 4])
                self.fds.extend(fds)
        if not data:
            raise ConnectionError("QEMU closed the display socket")
        return data

    def recv(self, timeout):
        """One message and the descriptor on its first byte, if any. The
        header is read alone, as the page says, so a descriptor is never
        taken for the wrong message."""
        buf = b""
        while len(buf) < 8:
            buf += self._read(8 - len(buf), timeout if not buf else None)
        length = MSG_LEN.get(struct.unpack_from("<H", buf)[0])
        if length is None:
            raise ConnectionError("unknown message type")
        fd = self.fds.pop(0) if self.fds else None
        while len(buf) < length:
            buf += self._read(length - len(buf), None)
        return buf, fd

    def pump(self, timeout):
        """Handle one message; False if none came within timeout."""
        try:
            msg, fd = self.recv(timeout)
        except TimeoutError:
            return False
        kind = struct.unpack_from("<H", msg)[0]
        if kind == MSG_REGION:
            rid, _, _, _, size = struct.unpack_from("<IIIIQ", msg, 8)
            self.regions[rid] = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
        elif kind == MSG_REGION_GONE:
            self.regions.pop(struct.unpack_from("<I", msg, 8)[0], None)
        if fd is not None:
            os.close(fd)
        return True

    def layout(self, h):
        """The head's layout and plane 0, read under the layout seqlock."""
        base = HEAD_OFFSET + HEAD_BYTES * h
        end = time.monotonic() + SEQLOCK_SECONDS
        while time.monotonic() < end:
            gen, = struct.unpack_from("<I", self.control, base + 12)
            if gen & 1:
                continue
            width, height = struct.unpack_from("<II", self.control, base + 16)
            region, fmt, off, _, _, stride = struct.unpack_from(
                "<IIQIII", self.control, base + 80)
            if struct.unpack_from("<I", self.control, base + 12)[0] == gen:
                return dict(width=width, height=height, region=region,
                            format=fmt, offset=off, stride=stride)
        raise TimeoutError("layout_gen stayed odd or kept changing")

    def content_gen(self, h):
        return struct.unpack_from("<Q", self.control,
                                  HEAD_OFFSET + HEAD_BYTES * h + 32)[0]

    def arm(self, h):
        struct.pack_into("<I", self.control,
                         HEAD_OFFSET + HEAD_BYTES * h + 44, 1)

    def row(self, lay, y):
        mem = self.regions.get(lay["region"])
        if mem is None or lay["format"] != FMT_X8R8G8B8 or \
                y >= lay["height"]:
            return None
        start = lay["offset"] + y * lay["stride"]
        return bytes(mem[start:start + lay["width"] * 4])


def percentile(values, p):
    k = (len(values) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def analyze_frames(updates):
    """Frame rate, skipped codes, repeats and intervals, by frame code."""
    seen, missing, repeats = [], [], 0      # seen: (code, first-seen time)
    for u in updates:
        if u["code"] is None:
            continue
        if not seen:
            seen.append((u["code"], u["t"]))
            continue
        delta = (u["code"] - seen[-1][0]) & 0xffff
        if delta == 0:
            repeats += 1
        elif delta < 0x8000:
            missing += [(seen[-1][0] + i) & 0xffff for i in range(1, delta)]
            seen.append((u["code"], u["t"]))
    out = dict(updates=len(updates), frames=len(seen), drops=len(missing),
               repeats=repeats, fps=None, intervals_ms=None)
    if len(seen) >= 2:
        span = (seen[-1][1] - seen[0][1]) / 1e9
        out["fps"] = round((len(seen) - 1) / span, 3)
        gaps = sorted((b[1] - a[1]) / 1e6 for a, b in zip(seen, seen[1:]))
        out["intervals_ms"] = {
            "min": round(gaps[0], 3), "p50": round(percentile(gaps, 50), 3),
            "p99": round(percentile(gaps, 99), 3), "max": round(gaps[-1], 3)}
    return out, dict(seen), missing


FRAME = re.compile(r"sgi_display_pattern_frame frame code (\d+) published "
                   r"at virtual \d+ ns, host (\d+) ns")


def read_trace(path):
    """{code: host_ns} of the frames QEMU says it published."""
    out = {}
    with open(path) as f:
        for line in f:
            m = FRAME.search(line)
            if m:
                out.setdefault(int(m.group(1)), int(m.group(2)))
    return out


def summarize(updates, args):
    summary, seen, missing = analyze_frames(updates)
    if args.trace:
        published = read_trace(args.trace)
        # A code QEMU never published is QEMU's drop; one it did publish and
        # the probe missed was lost to the probe's own scheduling.
        summary["unpublished"] = sum(1 for c in missing
                                     if c not in published)
        summary["probe_missed"] = summary["drops"] - summary["unpublished"]
        late = sorted((seen[c] - published[c]) / 1e6
                      for c in seen if c in published)
        if late:
            summary["publish_to_probe_ms"] = {
                "n": len(late), "min": round(late[0], 3),
                "p50": round(percentile(late, 50), 3),
                "p99": round(percentile(late, 99), 3),
                "max": round(late[-1], 3)}
    fails = []
    if args.min_fps is not None and not (summary["fps"] or 0) >= args.min_fps:
        fails.append("fps %s below %s" % (summary["fps"], args.min_fps))
    drops = summary.get("unpublished", summary["drops"])
    for what, got, limit in (
            ("drops", drops, args.max_drops),
            ("frames missed by the probe", summary.get("probe_missed", 0),
             args.max_probe_missed),
            ("repeats", summary["repeats"], args.max_repeats)):
        if limit is not None and got > limit:
            fails.append("%d %s, at most %d" % (got, what, limit))
    iv = summary["intervals_ms"]
    if args.max_p99_ms is not None and iv and iv["p99"] > args.max_p99_ms:
        fails.append("p99 interval %s ms above %s" % (iv["p99"],
                                                      args.max_p99_ms))
    summary["failures"] = fails
    return summary


def cmd_record(args):
    disp = Display(args.socket)
    os.makedirs(args.out, exist_ok=True)
    stop = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.append(1))
    start = host_ns()
    deadline = start + int(args.duration * 1e9) if args.duration else None
    updates, last, src = [], None, "poll"
    try:
        while not stop and (deadline is None or host_ns() < deadline):
            disp.arm(0)
            gen = disp.content_gen(0)
            stamp = host_ns()     # after the read: never before the publish
            if gen != last:
                last = gen
                lay = disp.layout(0)
                line = disp.row(lay, CODE_Y + BLOCK // 2)
                updates.append(dict(t=stamp, gen=gen, src=src,
                                    code=decode_code(line) if line else None))
            try:
                src = "wake" if disp.pump(0.25) else "poll"
            except ConnectionError as err:
                print("sgi-display-probe: %s" % err, file=sys.stderr)
                break
    finally:
        with open(os.path.join(args.out, "frames.jsonl"), "w") as f:
            f.write("".join(json.dumps(u) + "\n" for u in updates))
    return report(updates, args, args.out)


def report(updates, args, out):
    summary = summarize(updates, args)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))
    return 1 if summary["failures"] else 0


def cmd_analyze(args):
    with open(os.path.join(args.dir, "frames.jsonl")) as f:
        return report([json.loads(line) for line in f], args, args.dir)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def bounds(p):
        p.add_argument("--trace", help="QEMU's log with the trace events")
        p.add_argument("--min-fps", type=float)
        p.add_argument("--max-drops", type=int,
                       help="skipped codes; with --trace, only those QEMU "
                            "never published")
        p.add_argument("--max-probe-missed", type=int,
                       help="published codes the probe did not see")
        p.add_argument("--max-repeats", type=int)
        p.add_argument("--max-p99-ms", type=float)

    rec = sub.add_parser("record", help="record head 0's updates")
    rec.add_argument("--socket", required=True)
    rec.add_argument("--out", required=True)
    rec.add_argument("--duration", type=float, help="seconds (default: "
                     "until SIGINT)")
    bounds(rec)
    ana = sub.add_parser("analyze", help="measure a recording")
    ana.add_argument("dir")
    bounds(ana)
    args = ap.parse_args(argv)
    return {"record": cmd_record, "analyze": cmd_analyze}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
