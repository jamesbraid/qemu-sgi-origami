#!/usr/bin/env python3
#
# POD sanity check: enter the firmware's diagnostics mode by dip switch
# and require sane configuration, memory-test, register and error-state
# output from the commands SGI's own field documentation reaches for
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
POD is the IP27 PROM's own diagnostics mode. With the ELSC debug
switches set to local-POD (0x10) the firmware drops to its POD prompt on
the junk-bus UART after its power-on diagnostics, and the roadmap's
real-PROM milestone names exactly this as its check: an interactive POD
prompt with sane memory and configuration output. The vendor's
MDK-based field-diagnostics manual separately names the commands a field
engineer is told to use when a machine misbehaves (why, error,
error_dump, dumpspool, pr, crb, crbx); this test drives those plus the
configuration, memory-test, TLB and switch commands and holds the
output to fixed lines.

The memory tests take xkphys addresses: POD in cached mode treats a
bare number as a virtual address and a store to unmapped kuseg is a TLB
miss, observed live before this test existed (the project checkout's
firmware-diags notebook records the session). santest/memtest at
0xa800000002000000 -- 32 MiB into node 0's memory, clear of the PROM's
own structures -- pass on this machine.

Deliberately NOT driven: bist (it asks to reset the system), nic and
modnic (the Hub one-wire NIC responder is not modelled -- both answers
are recorded in the notebook, and the one-wire bus is off the critical
path per the roadmap), and dirtest (it randomizes the directory and
warns). The console is the junk-bus UART on serial line 3
(set_console(console_index=3)); the three earlier lines get the null
backend the framework emits for skipped indexes, which is acceptable
here because the dip switches place the firmware's console on the junk
UART by design.

test_prom_pod_verdict.py proves the verdict logic against real captured
POD transcripts without booting anything.
"""

import json
import os
import re
import socket
import time

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FIRMWARE_REL, file_record, file_sha256,
                      fork_git_identity, resolve_asset, utc_now)
from lib_prom_mdk import PROM_SHA256
from lib_prom_pod import COMMANDS, LOCAL_POD_SWITCHES, verdict


# Boot to the POD prompt is about two minutes under a contended host and
# every command but memtest answers immediately; 840 keeps the usual 60s
# margin under the 900s meson timeout.
BOOT_TIMEOUT_SECONDS = 840

PROMPT_RE = re.compile(rb"POD [A-Za-z]* ?(?:Cac|Dex|Unc)>[ \t]*")
SWITCHES_RE = re.compile(rb"DIP switches \[[0-9a-fA-F]*\]:")
KEYPRESS_RE = re.compile(rb"Press ENTER to continue")
PAGER_RE = re.compile(rb"--More--")
_TAIL_MAX = 8192


def _drive_pod(sock, log_path, timeout, commands):
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


_FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))

class PromPod(QemuSystemTest):
    """Boots the real IP27 PROM with local-POD dip switches and
    requires verdict() to be PASS over the driven command list."""

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_pod(self):
        prom = resolve_asset(os.environ['ORIGAMI_ASSETS'], FIRMWARE_REL)
        if prom is None:
            self.skipTest(
                f'no firmware at <ORIGAMI_ASSETS>/{FIRMWARE_REL} -- '
                'point it at a checkout with the real PROM extracted')

        if file_sha256(prom) != PROM_SHA256:
            self.fail('PROM SHA-256 does not match the recorded release')

        vm = self.get_vm(name='prom_pod')
        vm.set_machine(
            'origin200,synthesized-firmware=off,'
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
                COMMANDS)
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
                'identity': {
                    'fork': fork_git_identity(_FORK_ROOT),
                    'qemu_binary': file_record(self.qemu_bin),
                    'payload': {'prom': file_record(prom)},
                },
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
