#!/usr/bin/env python3
#
# The Origin 300 sMDK standalone diagnostics: boot the stock PROM, netboot
# the sMDK monitor and run each diagnostic smdk-matrix marks for the
# processor population, one boot per diagnostic
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The layer below IRIX's online diagnostics: sMDK, the Origin 300
generation's standalone diagnostic monitor, which the stock IP35 PROM
netboots from its command monitor. At the monitor's prompt each
diagnostic is fetched over the network, started as a process of its own
and judged by the row the monitor's `ds` status table paints for it:
`Passed`, or `Done` with no failure for xbg (lib_prom_smdk.py says how the
row is read and why).

smdk-matrix names the populations and the diagnostics each runs;
lib_prom_smdk.DIAGS holds each diagnostic's arguments. This file owns the
two-processor Origin 300 and each test_prom_smdk_origin300_<row>.py beside
it registers one more population, each its own meson target. Each
diagnostic gets a fresh boot, as the project checkout's `tools/origami
run smdk` gives it: that is the sequence every recorded verdict was taken
on, and a diagnostic that leaves the memory or directory state changed
cannot then decide the next one's. Every diagnostic runs even after
another fails; the test fails at the end naming each one that did not
pass, and writes the verdicts, the status rows and the times to
<row>-summary.json.

The package is `field_diags_stand` from Internal Support Tools 2.7, IRIX
6.5.17 build, which `tools/extract-irix smdk` in the project checkout
lands in <ORIGAMI_ASSETS>/assets/boot/smdk; each file's SHA-256 is checked
against that checkout's asset manifest before a boot is spent on it.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lib_origin300  # noqa: E402
from lib_host_cpus import pin  # noqa: E402
from lib_matrix import MatrixError  # noqa: E402
from lib_net import Console, HarnessError  # noqa: E402
from lib_prom import FORK_ROOT, require_assets, run_record  # noqa: E402
from lib_prom_smdk import (DIAG_ORDER, DIAGS, PACKAGE_DIR,  # noqa: E402
                           PACKAGE_SHA256, POPULATIONS, SmdkDriver, TftpRoot,
                           bound_end_pass, manifest_row, package_files)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


# The stock PROM's power-on diagnostics, then the monitor's netboot and
# discovery pass, to smdk>: on the project checkout's runs of 2026-09-26
# and 27 a whole one-chassis run took 100 to 370s including its
# diagnostic, and a pair's 740 to 2040s. Each boot and each diagnostic is
# also cut to what is left of the row's budget.
BOOT_TIMEOUT_SECONDS = {1: 600, 2: 1200}

# A diagnostic's own limit, from `run` to its verdict: System_Level_Test's
# one pass took 130s, a pair's scct ran for most of its 2040s, and nmem
# needs about 3600s on the pair to sync the two chassis.
DIAG_TIMEOUT_SECONDS = {1: 900, 2: 3900}

# The address the IO8 board record carries, and the user-mode network
# the monitor netboots from: the PROM asks for its address over BOOTP,
# and the reply names the one boot file every fetch then reads.
NETWORK = ('user,id=net0,mac={mac},net=192.0.2.0/24,host=192.0.2.2,'
           'dhcpstart=192.0.2.15,tftp={tftp},bootfile=smdk')


if QemuSystemTest is not None:

    class PromSmdkMatrix(QemuSystemTest):
        """One population's diagnostics. The test methods come from
        register_smdk_row()."""

        def _left(self, cap):
            left = self.deadline - time.monotonic()
            if left < 1:
                raise HarnessError(f'{self.row.id} spent its '
                                   f'{self.row.budget}s budget')
            return min(cap, left)

        def _run_row(self, row_id):
            started = time.monotonic()
            try:
                self.row = manifest_row(row_id)
            except MatrixError as exc:
                self.fail(str(exc))
            self.deadline = started + self.row.budget
            self.populations = POPULATIONS[row_id]
            files = package_files(self.row)
            rels = [os.path.join(PACKAGE_DIR, name) for name in files]
            assets = require_assets(
                self, *lib_origin300.ASSET_SPECS,
                *[(rel, PACKAGE_SHA256[name])
                  for rel, name in zip(rels, files)])
            self.package = {name: assets[rel]
                            for rel, name in zip(rels, files)}
            results = {}
            for name in DIAG_ORDER:
                if not self.row.runs(name):
                    results[name] = {'status': 'n/a',
                                     'detail': self.row.reason(name)}
                    continue
                began = time.monotonic()
                try:
                    status, row_text = self._run_diag(name)
                    detail = row_text or 'no ds row listed it'
                except HarnessError as exc:
                    status, detail = 'fail', str(exc)
                results[name] = {
                    'status': 'pass' if status == 'pass' else 'fail',
                    'verdict': status, 'detail': detail,
                    'seconds': round(time.monotonic() - began, 1)}
                self.log.info('%s: %s (%s)', name, status, detail)
            self._report(results, round(time.monotonic() - started, 1))

        def _run_diag(self, name):
            diag = DIAGS[name]
            scratch = self.scratch_file(name)
            os.makedirs(scratch, exist_ok=True)
            tftp = TftpRoot(os.path.join(scratch, 'tftp'), self.package)
            if diag.fetch and diag.passes:
                tftp.edit(diag.fetch,
                          lambda data: bound_end_pass(data, diag.passes))
            root = os.environ['ORIGAMI_ASSETS']
            if len(self.populations) > 1:
                machine, argv = lib_origin300.launch_args_two_chassis(
                    root, self.populations, scratch)
            else:
                machine, argv = lib_origin300.launch_args(
                    root, self.populations[0], scratch)
            vm = self.get_vm(name=f'{self.row.id}-{name}')
            vm.set_machine(machine)
            vm.set_console()
            vm.add_args(*argv, '-nic', NETWORK.format(
                mac=lib_origin300.NETWORK_MAC, tftp=tftp.directory), '-S')
            vm.launch()
            if diag.pin_cpus:
                pin(vm.get_pid(), sum(self.populations), self.log)
            vm.cmd('cont')
            con = Console(vm.console_socket,
                          self.log_file(f'{self.row.id}-{name}-console.log'),
                          exited=lambda: vm.exitcode() is not None)

            def paused(action):
                vm.cmd('stop')
                try:
                    action()
                finally:
                    vm.cmd('cont')

            nodes = len(self.populations)
            try:
                driver = SmdkDriver(con, tftp, paused)
                driver.boot(self._left(BOOT_TIMEOUT_SECONDS[nodes]))
                deadline = time.monotonic() + self._left(
                    DIAG_TIMEOUT_SECONDS[nodes])

                def left():
                    return max(1, deadline - time.monotonic())

                if diag.prime_iocfg:
                    driver.run('iocfg', timeout=left())
                start = driver.run(name, diag.args, timeout=left())
                if diag.fetch:
                    driver.serve_follow_on(diag.fetch, driver.pos)
                    driver.await_fetch(diag.fetch, left())
                return driver.poll(name, start, left())
            finally:
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)

        def _report(self, results, seconds):
            summary = self.log_file(f'{self.row.id}-summary.json')
            with open(summary, 'w', encoding='utf-8') as out:
                json.dump({
                    'schema': 1, 'row': self.row.id, 'seconds': seconds,
                    'results': results,
                    'identity': run_record(FORK_ROOT, self.qemu_bin),
                }, out, indent=2)
                out.write('\n')
            failed = [f'{name}: {r["detail"]}' for name, r in results.items()
                      if r['status'] == 'fail']
            if failed:
                self.fail(f'{self.row.id}: ' + '; '.join(failed) +
                          f' (summary: {summary})')

    def register_smdk_row(cls, row_id):
        """Attach test_<row_id> to cls: one population's diagnostics."""

        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_row(self):
            self._run_row(row_id)

        test_row.__name__ = f'test_{row_id}'
        test_row.__doc__ = f'smdk-matrix row {row_id}'
        setattr(cls, test_row.__name__, test_row)

    class PromSmdkOrigin300(PromSmdkMatrix):
        """The two-processor Origin 300."""

    register_smdk_row(PromSmdkOrigin300, 'origin300')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
