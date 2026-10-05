#!/usr/bin/env python3
#
# MDK router package on the Origin 2000 module: netboot MDK_router_sso
# BY PATH on four nodes and two Routers and require its link test to pass
#
# SPDX-License-Identifier: BSD-3-Clause
"""
MDK_router_sso is the third MDK 1.6 package, the Router simultaneous
switching test. SGI's MDK-Based Field Diagnostics manual (108-0162-004,
chapter 5, PDF pages 81 to 85) describes it on Router-equipped Origin
2000 and Onyx2 systems: 238 passes in which one CPU per node reads 16 MB
from other nodes so that traffic crosses every Router link, then reads
each Router's per-port error registers and each Hub's network interface
error register, failing on more than 10 checkbit errors in a pass. A
clean run ends with "TEST RESULT: **** TEST PASSED ****" and "MDK
Router_sso test run is complete."

This test boots the Origin 2000 module the machine models (four nodes,
eight CPUs, two Routers, 256 MB), netboots the package and requires that
result: every node and both Routers discovered, all 238 passes, the
diagnostic's own pass line, and none of its error, warning or failure
lines.

It used to run on the two-chassis Origin 200, which joins two nodes
through a null router. The package's release note (MDK 1.6 on Internal
Support Tools 2.7) says it was written for that case, but it never
passed there and cannot pass on evidence we hold. Each pass the
diagnostic reads the Router port error-clear registers (port number in
bits 18:16, offset 0x88) through vector route 0, ignores the read's
status, and takes bits 15:8 of its result buffer as the checkbit count;
the buffer is written only on a successful read response (from the
package's disassembly). Behind a null router, route 0 reaches the peer
Hub, and what a real Hub answers to those addresses is not established
by any Hub document or capture we hold. The model refuses them with an
address error, so the diagnostic prints whatever its stack held and
fails every pass. A capture of router_sso, or of the PROM's vector-read
command at those addresses, on a real two-module Origin 200 would
settle it.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FIRMWARE_REL, FORK_ROOT, MACHINES, drive_console,
                      run_record, utc_now)
from lib_prom_mdk import PROM_SHA256, payload_identity_problems
from lib_prom_mdk_router import NETBOOT_PATH, verdict


MDK_REL = os.path.join('boot', 'mdk', 'MDK_router_sso')
ROUTER_SSO_SHA256 = (
    "8caaf050bbf1a71caf0bb59944415e98c7ffd49242975c2b41e5d29cdd3108f2"
)

# One run of this test on the debug_tcg build at host load 75 to 80 took
# 2847s: the router test started about 140s after launch, then about 11s a
# pass. A run with interrupt logging on took about 19s a pass. This allows
# close to twice the measured run; the meson timeout is 60s above it.
BOOT_TIMEOUT_SECONDS = 5340

DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    # CR-terminated: MDK's line reader ignores LF (test_prom_mdk.py).
    (r"MDK>", "router_sso\r"),
)
DRIVER_UNTIL_RE = re.compile(rb"Router_sso test run is complete")


class PromMdkRouter(QemuSystemTest):
    """Boots the real IP27 PROM on the Origin 2000 module, netboots
    MDK_router_sso, and requires the router test to pass."""

    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_MDK = LocalAsset(MDK_REL, ROUTER_SSO_SHA256)

    def _stage_netboot_tftp(self, mdk):
        root = self.scratch_file('netboot-tftp')
        staged = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged), exist_ok=True)
        shutil.copyfile(mdk, staged)
        return root

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_mdk_router(self):
        prom, mdk = self.ASSET_PROM.fetch(), self.ASSET_MDK.fetch()

        identity_problems = payload_identity_problems(
            prom, mdk, ROUTER_SSO_SHA256, "MDK_router_sso")
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(mdk)

        vm = self.get_vm(name='prom_mdk_router')
        vm.set_machine(MACHINES['origin2000'])
        vm.set_console()
        capture = self.log_file('wire.pcap')
        vm.add_args(
            '-accel', 'tcg,thread=multi',
            '-cpu', 'R10000',
            '-smp', '8',
            '-m', '256',
            '-bios', str(prom),
            '-audio', 'none',
            '-nic', ('user,id=net0,net=192.0.2.0/24,host=192.0.2.2,'
                     f'dhcpstart=192.0.2.15,tftp={netboot_tftp},'
                     f'bootfile={NETBOOT_PATH}'),
            '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
        )

        started = utc_now()
        vm.launch()
        log_path = self.log_file('mdk.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE, vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that '
                'did not start, not a boot that failed early')

        report = verdict(transcript, reached=reached)

        summary_path = self.log_file('mdk-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'reached_completion': reached,
                'report': report,
            }, summary_file, indent=2, sort_keys=True)
            summary_file.write('\n')

        run_path = self.log_file('run.json')
        with open(run_path, 'w', encoding='utf-8') as run_file:
            json.dump({
                'schema': 1,
                'started_utc': started,
                'ended_utc': ended,
                'console_driver_reached_until': reached,
                'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                'identity': run_record(FORK_ROOT, self.qemu_bin,
                                       prom=prom, router_sso=mdk),
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'MDK router problems: ' + '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
