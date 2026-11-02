#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run a static PowerPC program under qemu-ppc with the sgi-l2-shim plugin
and the L1 hub, and check what the program saw and what the hub and the
backing file recorded.

The program (guest.c) opens the L1 control node and a slot node, issues the
driver's ioctls, exchanges a segment with a fake L1, and drives the flash
behind /dev/mem. Skipped (exit status 77) without the PowerPC cross
compiler.

usage: l2-shim-test.py QEMU-PPC PLUGIN HUB
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2_hub_lib as lib  # noqa: E402

QEMU_PPC = PLUGIN = HUB = None
HERE = os.path.dirname(os.path.abspath(__file__))
MEM_FILE_SIZE = 0x2400000
FLASH_FILE = 0x2000000
FLASH_SIZE = 0x400000


class ShimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("QEMU_TEST_PPC_CC", "powerpc-linux-gnu-gcc")
        if not shutil.which(cc):
            print(f"skipped: no PowerPC cross compiler ({cc})")
            sys.exit(77)
        cls.tmp = tempfile.mkdtemp(prefix="l2shim-")
        cls.guest = os.path.join(cls.tmp, "guest")
        subprocess.run(
            [cc, "-O1", "-Wall", "-static", "-nostdlib", "-nostartfiles",
             "-ffreestanding", "-o", cls.guest,
             os.path.join(HERE, "guest.c")], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_nodes_and_flash(self):
        root = os.path.join(self.tmp, "root")
        os.makedirs(os.path.join(root, "dev"))
        mem = os.path.join(root, "dev", "mem")
        with open(mem, "wb") as f:
            f.truncate(MEM_FILE_SIZE)
            f.seek(FLASH_FILE)
            f.write(b"\xff" * FLASH_SIZE)
        hub = lib.Hub(HUB)
        l1 = hub.l1()
        try:
            hub.wait_log("attach slot 0")
            request = lib.segment(28)
            l1.send_segment(request)
            shim_log = os.path.join(self.tmp, "shim.log")
            run = subprocess.run(
                [QEMU_PPC, "-L", root, "-r", "2.4.7",
                 "-plugin", f"{PLUGIN},hub={hub.dir},log={shim_log}",
                 self.guest],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertNotIn("FAIL", run.stdout)
            self.assertIn("done 0 failures", run.stdout)

            first = l1.recv_segment()
            second = l1.recv_segment()
            self.assertEqual(first[:2], b"\x00\x1c")
            self.assertEqual(first[6], 1)
            self.assertEqual(len(second), 20)
            self.assertEqual(second[:2], b"\x00\x14")

            log = hub.log()
            for line in ("open control node", "attach slot 0", "open slot 0",
                         "reset slot 0 ioctl 0x5305",
                         "segment L1->L2 slot 0 len 28",
                         "stamped to 28", "stamped to 20",
                         "segment L2->L1 slot 0 len 28",
                         "node close slot 0"):
                self.assertIn(line, log)

            # The mapped window is the file: what the program programmed and
            # erased is there, and no command word was left behind.
            with open(mem, "rb") as f:
                f.seek(FLASH_FILE)
                flash = f.read(FLASH_SIZE)
            word = lambda off: struct.unpack(">I", flash[off:off + 4])[0]
            self.assertEqual(word(0x100), 0x02305608)
            self.assertEqual(word(0x1554), 0xffffffff)
            self.assertEqual(word(0x0aa8), 0xffffffff)
            self.assertEqual(word(0), 0xffffffff)
            self.assertEqual(word(0x3f0000), 0xffffffff)
            self.assertEqual(word(0x3ec000), 0x11111111)
            self.assertEqual(word(0x3f4000), 0x44444444)
            self.assertEqual(word(0x3ffff4), 0x55555555)
            self.assertEqual(word(0x60000), 0x77777777)
            # The bus with nothing on it was never written.
            with open(mem, "rb") as f:
                self.assertEqual(f.read(FLASH_SIZE), bytes(FLASH_SIZE))
        finally:
            l1.close()
            hub.close()

    def test_observation_without_a_hub(self):
        """With no hub the nodes pass through to the host and are logged."""
        root = os.path.join(self.tmp, "root-observe")
        os.makedirs(os.path.join(root, "dev"))
        shim_log = os.path.join(self.tmp, "observe.log")
        run = subprocess.run(
            [QEMU_PPC, "-L", root, "-r", "2.4.7",
             "-plugin", f"{PLUGIN},log={shim_log}", self.guest],
            capture_output=True, text=True, timeout=120)
        self.assertIn("FAIL", run.stdout)
        with open(shim_log) as f:
            log = f.read()
        self.assertIn("open(/dev/sgil1_cs, 0x0)", log)
        self.assertIn("-> -2 No such file or directory", log)


if __name__ == "__main__":
    QEMU_PPC, PLUGIN, HUB = sys.argv[1:4]
    del sys.argv[1:4]
    unittest.main()
