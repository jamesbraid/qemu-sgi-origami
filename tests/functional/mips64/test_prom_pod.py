#!/usr/bin/env python3
#
# POD sanity check: enter the firmware's diagnostics mode by dip switch
# and require sane configuration, memory-test, register and error-state
# output from the commands SGI's own field documentation reaches for
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
This test enters POD mode, runs configuration, memory, TLB, switch, and
error-reporting commands, and checks their output. Memory tests use
xkphys addresses. The test uses 0xa800000002000000, 32 MiB into node
0's memory and clear of the PROM's own structures.

The test skips `bist` because it resets the machine, `nic` and `modnic`
because their responder is not modeled, and `dirtest` because it
randomizes directory state. The console is serial line 3.

test_prom_pod_verdict.py proves the verdict logic against real captured
POD transcripts without booting anything.
"""

import json
import os
import re
import socket
import time

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FORK_ROOT, file_sha256, run_record,
                      utc_now)
from lib_prom_pod import COMMANDS, LOCAL_POD_SWITCHES, verdict
from lib_prom_sash import RELEASES
from lib_translation_health import assert_translation_healthy


# lib_prom_pod.ASSERTIONS requires the "SGI Version 6.11" banner, which
# the fixture transcript the assertions were pinned from actually
# printed. That banner belongs to the 6.5-base PROM, not lib_prom's own
# FIRMWARE_REL (6.5.30, banner 6.156) -- booting 6.5.30 here can only
# ever fail the banner assertion, so this test pins its own release
# rather than sharing the other real-PROM tests' 6.5.30 default.
RELEASE = "6.5-base"
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
PROM_SHA256 = RELEASES[RELEASE]['prom_sha256']


# Boot to the POD prompt is about two minutes under a contended host and
# every command but memtest answers immediately; 840 keeps the usual 60s
# margin under the 900s meson timeout.
BOOT_TIMEOUT_SECONDS = 840

PROMPT_RE = re.compile(rb"POD [A-Za-z]* ?(?:Cac|Dex|Unc)>[ \t]*")
SWITCHES_RE = re.compile(rb"DIP switches \[[0-9a-fA-F]*\]:")
KEYPRESS_RE = re.compile(rb"Press ENTER to continue")
PAGER_RE = re.compile(rb"--More--")
_TAIL_MAX = 8192


def _drive_pod(sock, log_path, timeout, commands, vm=None):
    """Answer POD's own prompts and run the command list.

    Unlike the fixed one-shot rules the netboot tests use, POD reprints
    its prompt after every command, so this walks a command queue: each
    prompt match sends the next command, a pager gets a space, a
    keypress prompt gets a newline, and a switches prompt gets the
    local-POD value. Returns (transcript, reached) where reached means
    the prompt came back after the final command.
    """
    queue = list(commands)
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    tail = b""
    reached = False
    health_checked = False
    with open(log_path, "wb") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            transcript += chunk
            log.write(chunk)
            log.flush()
            if vm is not None and not health_checked:
                health_checked = True
                assert_translation_healthy(vm)
            tail = (tail + chunk)[-_TAIL_MAX:]
            if SWITCHES_RE.search(tail):
                sock.sendall(b"%04x\n" % LOCAL_POD_SWITCHES)
                tail = b""
            elif PAGER_RE.search(tail):
                sock.sendall(b" ")
                tail = b""
            elif KEYPRESS_RE.search(tail):
                sock.sendall(b"\n")
                tail = b""
            elif PROMPT_RE.search(tail):
                if not queue:
                    reached = True
                    break
                sock.sendall(queue.pop(0).encode("ascii") + b"\n")
                tail = b""
    return transcript.decode("utf-8", errors="replace"), reached


class PromPod(QemuSystemTest):
    """Boots the real IP27 PROM with local-POD dip switches and
    requires verdict() to be PASS over the driven command list."""

    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_pod(self):
        prom = self.ASSET_PROM.fetch()

        if file_sha256(prom) != PROM_SHA256:
            self.fail('PROM SHA-256 does not match the recorded release')

        vm = self.get_vm(name='prom_pod')
        vm.set_machine(
            'origin200,'
            f'elsc-dip-switches={LOCAL_POD_SWITCHES},junk-uart=on')
        # The junk-bus UART is serial line 3; the framework fills
        # the earlier indexes with null backends.
        vm.set_console(console_index=3)
        vm.add_args('-accel', 'tcg,thread=single',
                    '-cpu', 'R10000',
                    '-smp', '1',
                    '-m', '64',
                    '-bios', str(prom),
                    '-audio', 'none')

        started = utc_now()
        vm.launch()
        log_path = self.log_file('pod.log')
        try:
            transcript, reached = _drive_pod(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                COMMANDS, vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that '
                'did not start, not a POD entry that failed')

        report = verdict(transcript, reached=reached)

        summary_path = self.log_file('pod-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'commands': list(COMMANDS),
                'reached_final_prompt': reached,
                'report': report,
            }, summary_file, indent=2, sort_keys=True)
            summary_file.write('\n')

        run_path = self.log_file('run.json')
        with open(run_path, 'w', encoding='utf-8') as run_file:
            json.dump({
                'schema': 1,
                'started_utc': started,
                'ended_utc': ended,
                'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                'identity': run_record(FORK_ROOT, self.qemu_bin, prom=prom),
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'POD sanity problems: ' + '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
