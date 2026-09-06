#!/usr/bin/env python3
#
# MDK router package on the two-chassis machine: netboot MDK_router_sso
# BY PATH on four CPUs across two nodes and require its link test to run
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
MDK_router_sso is the third MDK 1.6 package: a CrayLink link test
written, per its release note, for a two-node system with a null
router -- the dual-module Origin 200 this machine model also projects.
On one node it declines by design ("EXITING router_sso test because
there is only 1 node board in system"), so this leg boots the
two-chassis shape the machine model demands (four CPUs as two sockets
of two cores, one memory node each) and requires the test to run.

THIS LEG IS RED ON CURRENT TRUNK, and deliberately so: the two-chassis
boot never discovers its second hub ("Found 1 objects (1 hubs, 0
routers)", then "Headless HUB inaccessible") and loops, so the kernel
never loads. The project rule is that a broken subject's test stays red
until it is fixed rather than being marked expected-to-fail; this leg
is the nightly's standing measure of the two-chassis discovery wall,
recorded in the project checkout's firmware-diags notebook. Its
_verdict companion pins the two real failure shapes (the one-node
decline and the two-chassis wedge) so a pass cannot be faked by either.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FIRMWARE_REL, drive_console, file_record,
                      fork_git_identity, resolve_asset, utc_now)
from lib_prom_mdk import mdk_extra_args, payload_identity_problems
from lib_prom_mdk_router import NETBOOT_PATH, verdict


MDK_REL = os.path.join('assets', 'boot', 'mdk', 'MDK_router_sso')
ROUTER_SSO_SHA256 = (
    "8caaf050bbf1a71caf0bb59944415e98c7ffd49242975c2b41e5d29cdd3108f2"
)

BOOT_TIMEOUT_SECONDS = 1740

TWO_CHASSIS_ARGS = (
    '-object', 'memory-backend-ram,id=ram0,size=64M',
    '-object', 'memory-backend-ram,id=ram1,size=64M',
    '-numa', 'node,nodeid=0,cpus=0-1,memdev=ram0',
    '-numa', 'node,nodeid=1,cpus=2-3,memdev=ram1',
)

DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    # CR-terminated: MDK's line reader ignores LF (test_prom_mdk.py).
    (r"MDK>", "router_sso\r"),
)
DRIVER_UNTIL_RE = re.compile(rb"Router_sso test run is complete")


_FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))

class PromMdkRouter(QemuSystemTest):
    """Boots the real IP27 PROM as two chassis, netboots
    MDK_router_sso, and requires the router test to run."""

    def _asset_paths(self):
        assets_root = os.environ['ORIGAMI_ASSETS']
        prom = resolve_asset(assets_root, FIRMWARE_REL)
        mdk = resolve_asset(assets_root, MDK_REL)
        if not (prom and mdk):
            return None
        return prom, mdk

    def _stage_netboot_tftp(self, mdk):
        root = self.scratch_file('netboot-tftp')
        staged = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged), exist_ok=True)
        shutil.copyfile(mdk, staged)
        return root

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_mdk_router(self):
        paths = self._asset_paths()
        if paths is None:
            # @skipIfMissingEnv('ORIGAMI_ASSETS') above already skipped
            # the case where it is unset entirely -- getting here means
            # it IS set, so a missing PROM or MDK_router_sso is a
            # provisioning failure, not an absent-assets developer run,
            # and must fail loudly: a silent skip here would let a real
            # router-test regression pass CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but the firmware/MDK_router_sso '
                f'are missing at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{MDK_REL} -- provision them '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')
        prom, mdk = paths

        identity_problems = payload_identity_problems(
            prom, mdk, ROUTER_SSO_SHA256, "MDK_router_sso")
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(mdk)

        vm = self.get_vm(name='prom_mdk_router')
        # No debug-leds=off: with more than one CPU that option wedged
        # the PROM boot (test_prom_mdk_instgen.py).
        vm.set_machine('origin200,synthesized-firmware=off')
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        extra = mdk_extra_args(prom, netboot_tftp, elsc_log, capture,
                               netboot_path=NETBOOT_PATH)
        extra[extra.index('-smp') + 1] = '4,sockets=2,cores=2,threads=1'
        extra[extra.index('-m') + 1] = '128'
        vm.add_args('-accel', 'tcg,thread=single', *extra,
                    *TWO_CHASSIS_ARGS)

        started = utc_now()
        vm.launch()
        log_path = self.log_file('mdk.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE)
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
                'identity': {
                    'fork': fork_git_identity(_FORK_ROOT),
                    'qemu_binary': file_record(self.qemu_bin),
                    'payload': {
                        'prom': file_record(prom),
                        'router_sso': file_record(mdk),
                    },
                },
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
