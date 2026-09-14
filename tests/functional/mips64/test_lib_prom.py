#!/usr/bin/env python3
#
# Unit tests for lib_prom's console driver and asset resolver: the rule
# table fires once per rule, in order, and the end condition, timeout and
# a closed socket each end the loop the way the netboot tests rely on;
# resolve_asset() skips what is not provisioned and refuses what is wrong
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_prom.py` (or `uv run --with
pytest pytest tests/functional/mips64/test_lib_prom.py -v`). The guest
end of a socketpair stands in for QEMUMachine.console_socket: a helper
thread plays a scripted guest, sending its prompts and reading back what
the driver answers, while drive_console() runs on the other end exactly
as a netboot test calls it. ResolveAssetTest covers the shared asset
resolver against a temporary assets tree. lib_prom's progress-code
helpers are covered by test_prom_boot_verdict.py.
"""

import os
import re
import socket
import tempfile
import threading
import unittest

from lib_prom import (AssetMismatch, FIRMWARE_REL, drive_console,
                      file_sha256, prom_netboot_steps, resolve_asset)

STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", "boot -f bootp()/boot/sash64\n"),
)
UNTIL = re.compile(rb"dlserver=192\.0\.2\.2")


class _Guest(threading.Thread):
    """Plays the guest: for each (send, expect) step, send the bytes,
    then read until `expect` has arrived (or, for expect=None, assert
    nothing arrives within a short window). Records what it saw."""

    def __init__(self, sock, script):
        super().__init__(daemon=True)
        self.sock = sock
        self.script = script
        self.answers = []
        self.unexpected = []

    def run(self):
        self.sock.settimeout(2.0)
        for send, expect in self.script:
            if send is not None:
                self.sock.sendall(send)
            if expect is None:
                self.sock.settimeout(0.3)
                try:
                    self.unexpected.append(self.sock.recv(4096))
                except socket.timeout:
                    pass
                self.sock.settimeout(2.0)
                continue
            got = b""
            while expect not in got:
                got += self.sock.recv(4096)
            self.answers.append(got)
        self.sock.close()


class DriveConsoleTest(unittest.TestCase):

    def setUp(self):
        self.driver_end, self.guest_end = socket.socketpair()
        self.addCleanup(self.driver_end.close)
        self.addCleanup(self.guest_end.close)
        self.log_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.log_dir.cleanup)
        self.log_path = os.path.join(self.log_dir.name, "console.log")

    def _drive(self, script, timeout=5.0, **kwargs):
        guest = _Guest(self.guest_end, script)
        guest.start()
        result = drive_console(self.driver_end, self.log_path, timeout,
                               STEPS, **kwargs)
        guest.join(timeout=5.0)
        self.assertFalse(guest.is_alive(), "the scripted guest never "
                         "finished: the driver did not answer as scripted")
        return guest, result

    def test_rules_fire_once_each_in_order_and_until_ends_the_run(self):
        guest, (transcript, reached) = self._drive([
            (b"System Maintenance Menu\r\nOption? ", b"5\n"),
            (b"Command Monitor.\r\n>> ", b"unsetenv netaddr\n"),
            (b">> ", b"boot -f bootp()/boot/sash64\n"),
            (b"sash: printenv\r\ndlserver=192.0.2.2\r\n", None),
        ], until=UNTIL)

        self.assertTrue(reached)
        self.assertEqual(guest.answers, [
            b"5\n", b"unsetenv netaddr\n", b"boot -f bootp()/boot/sash64\n"])
        self.assertEqual(guest.unexpected, [])
        self.assertIn("dlserver=192.0.2.2", transcript)
        self.assertTrue(transcript.startswith("System Maintenance Menu"))

    def test_a_fired_rule_does_not_refire_on_the_same_prompt(self):
        # The menu prompt reappearing after it was answered must not be
        # answered again; the second ">>" prompt is answered by the second
        # ">>" rule, not the first one twice.
        guest, (_transcript, reached) = self._drive([
            (b"Option? ", b"5\n"),
            (b"Option? ", None),
            (b">> ", b"unsetenv netaddr\n"),
            (b">> ", b"boot -f bootp()/boot/sash64\n"),
            (b">> ", None),
            (b"dlserver=192.0.2.2\r\n", None),
        ], until=UNTIL)

        self.assertTrue(reached)
        self.assertEqual(guest.unexpected, [])

    def test_every_byte_lands_in_the_log(self):
        guest, (transcript, _reached) = self._drive([
            (b"Option? ", b"5\n"),
            (b"noise \xff\xfe\r\n", None),
            (b"dlserver=192.0.2.2\r\n", None),
        ], until=UNTIL)

        with open(self.log_path, "rb") as log:
            logged = log.read()
        self.assertEqual(
            logged, b"Option? noise \xff\xfe\r\ndlserver=192.0.2.2\r\n")
        # The transcript is the same bytes, decoded leniently.
        self.assertEqual(transcript,
                         logged.decode("utf-8", errors="replace"))
        self.assertEqual(guest.answers, [b"5\n"])

    def test_reached_when_sees_the_whole_transcript(self):
        # An end condition that only a count over the whole run can name:
        # the same completion words twice.
        guest, (_transcript, reached) = self._drive([
            (b"Option? ", b"5\n"),
            (b"test has completed\r\n", None),
            (b"test has completed\r\n", None),
        ], reached_when=lambda text: text.count(b"has completed") >= 2)

        self.assertTrue(reached)
        self.assertEqual(guest.answers, [b"5\n"])

    def test_timeout_reports_not_reached(self):
        guest, (transcript, reached) = self._drive([
            (b"Option? ", b"5\n"),
            (b"still waiting\r\n", None),
            (None, None),
        ], timeout=1.5, until=UNTIL)

        self.assertFalse(reached)
        self.assertIn("still waiting", transcript)
        self.assertEqual(guest.answers, [b"5\n"])

    def test_closed_socket_ends_the_loop_not_reached(self):
        guest, (transcript, reached) = self._drive([
            (b"Option? ", b"5\n"),
            (b"crash\r\n", None),
        ], timeout=30.0, until=UNTIL)

        self.assertFalse(reached)
        self.assertIn("crash", transcript)
        self.assertEqual(guest.answers, [b"5\n"])


class PromNetbootStepsTest(unittest.TestCase):
    """prom_netboot_steps() builds the same three-step dialogue every
    netboot test drives by hand today: this module's own STEPS (above)
    is that literal dialogue for sash64, so the two must agree exactly."""

    def test_matches_the_hand_written_sash_dialogue(self):
        self.assertEqual(prom_netboot_steps("boot/sash64"), STEPS)

    def test_appends_kernel_arguments_to_the_boot_command(self):
        steps = prom_netboot_steps("vmlinux", args="console=ttyS0")
        self.assertEqual(steps[-1],
                         (r">>", "boot -f bootp()/vmlinux console=ttyS0\n"))
        # The first two steps -- select the command monitor, clear a
        # stale leased address -- do not depend on the boot file or its
        # arguments.
        self.assertEqual(steps[:-1], STEPS[:-1])

    def test_no_arguments_means_a_bare_boot_command(self):
        steps = prom_netboot_steps("vmlinux")
        self.assertEqual(steps[-1], (r">>", "boot -f bootp()/vmlinux\n"))


class ResolveAssetTest(unittest.TestCase):

    def setUp(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        self.root = root.name

    def _provision(self, rel_path, content=b'firmware bytes'):
        path = os.path.join(self.root, rel_path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as provisioned:
            provisioned.write(content)
        return path

    def test_a_provisioned_file_resolves(self):
        path = self._provision(FIRMWARE_REL)

        self.assertEqual(resolve_asset(self.root, FIRMWARE_REL), path)

    def test_an_unprovisioned_file_is_none(self):
        self.assertIsNone(resolve_asset(self.root, FIRMWARE_REL))

    def test_an_unset_or_empty_root_is_none(self):
        # ORIGAMI_ASSETS unset (None via os.environ.get) or set empty.
        self.assertIsNone(resolve_asset(None, FIRMWARE_REL))
        self.assertIsNone(resolve_asset('', FIRMWARE_REL))

    def test_a_matching_digest_resolves(self):
        path = self._provision(FIRMWARE_REL)

        resolved = resolve_asset(self.root, FIRMWARE_REL,
                                 sha256=file_sha256(path))

        self.assertEqual(resolved, path)

    def test_a_wrong_digest_refuses_loudly(self):
        # A present-but-wrong file must not masquerade as an
        # unprovisioned one (None, a skip) -- it raises, naming both
        # digests.
        path = self._provision(FIRMWARE_REL, content=b'the wrong bytes')
        recorded = '0' * 64

        with self.assertRaises(AssetMismatch) as caught:
            resolve_asset(self.root, FIRMWARE_REL, sha256=recorded)
        self.assertIn(recorded, str(caught.exception))
        self.assertIn(file_sha256(path), str(caught.exception))


if __name__ == "__main__":
    unittest.main()
