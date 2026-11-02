# SPDX-License-Identifier: BSD-3-Clause
"""Helpers shared by the L1 hub and L2 kernel stand-in tests."""

import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time

SLOTS = 40
KIND_SEGMENT = 1


def wait_for(path, timeout=10.0):
    end = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > end:
            raise TimeoutError(path)
        time.sleep(0.02)


class Hub:
    """A running qemu-sgi-l2-hub in its own directory."""

    def __init__(self, binary):
        self.dir = tempfile.mkdtemp(prefix="l2hub-")
        self.log_path = os.path.join(self.dir, "hub.log")
        self.proc = subprocess.Popen(
            [binary, "--dir", self.dir, "--log", self.log_path])
        wait_for(os.path.join(self.dir, "l1.sock"))
        wait_for(os.path.join(self.dir, "dev.sock"))

    def log(self):
        with open(self.log_path) as f:
            return f.read()

    def wait_log(self, text, timeout=10.0):
        end = time.monotonic() + timeout
        while text not in self.log():
            if time.monotonic() > end:
                raise AssertionError(f"{text!r} not in hub log:\n{self.log()}")
            time.sleep(0.02)

    def close(self):
        self.proc.terminate()
        self.proc.wait(10)
        shutil.rmtree(self.dir, ignore_errors=True)

    def l1(self):
        return FakeL1(os.path.join(self.dir, "l1.sock"))

    def request(self, op, slot=0, arg=0):
        """Send a plugin request; return (socket, errno, record)."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        s.settimeout(10)
        s.connect(os.path.join(self.dir, "dev.sock"))
        s.send(bytes([ord(op), slot, arg, 0]))
        reply = s.recv(64)
        assert len(reply) == 10, reply
        if reply[0]:
            s.close()
            s = None
        return s, reply[0], reply[1:]


class FakeL1:
    """The L1 model's end of the link: kind byte plus segment."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""

    def send_segment(self, seg):
        self.sock.sendall(bytes([KIND_SEGMENT]) + seg)

    def recv_segment(self, timeout=10.0):
        self.sock.settimeout(timeout)
        while True:
            if len(self.buf) >= 3:
                n = struct.unpack(">H", self.buf[1:3])[0]
                if len(self.buf) >= 1 + n:
                    seg = self.buf[1:1 + n]
                    self.buf = self.buf[1 + n:]
                    return seg
            data = self.sock.recv(4096)
            if not data:
                raise EOFError
            self.buf += data

    def close(self):
        self.sock.close()


def segment(length=28, code=1, cls=0):
    """A request-shaped segment of the given length (test data)."""
    seg = bytearray(length)
    struct.pack_into(">H", seg, 0, length)
    seg[2] = 2
    seg[5] = 0x81
    seg[6] = cls
    struct.pack_into(">I", seg, 8, 0x1ffff00a)
    struct.pack_into(">I", seg, 12, 0x00044215)
    seg[16] = (length - 20) // 8
    struct.pack_into(">H", seg, 18, code)
    return bytes(seg)
