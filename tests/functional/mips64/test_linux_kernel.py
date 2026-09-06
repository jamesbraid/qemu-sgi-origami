#!/usr/bin/env python3
#
# Linux/IP27 userspace self-check: boot the mainline kernel with its
# embedded userspace stub and require Linux's own dying-init exit status
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/linux-usercheck's one-, two-, and four-CPU checks, in the
PROJECT checkout, into this fork's own functional-test framework, the same
way test_prom_boot.py ports tools/boot-gate and test_diag.py ports
tools/diag-gate. linux-usercheck boots the mainline Linux/IP27 kernel
(assets/linux-ip27/vmlinux, built by tools/linux-userspace with an
embedded freestanding userspace stub) directly via -kernel -- no PROM,
no disk, no terminal device for the guest -- and requires the console
transcript to contain Linux's own "Attempted to kill init!
exitcode=0x0000NN00" panic line with the exact byte the embedded init
chose: 42 << 8 = 0x2a00 at one CPU; 43 << 8 = 0x2b00 at two CPUs, after
two affinity-pinned workers stress both CPUs; or 44 << 8 = 0x2c00 at four
CPUs across two nodes, after a node-0 process moves and verifies a page on
node 1. See test_linux_kernel_verdict.py's own docstring for why a plain
literal-string match against the full 8-hex value already rejects a
fatal-signal death (the low byte would carry a signal number instead)
without needing any extra logic -- and for its fatal-signal fixture,
which proves it.

The four-CPU case is IN scope. It boots the same kernel with two 64 MiB
nodes (-numa node0 cpus 0-1 / node1 cpus 2-3, -smp
4,sockets=2,cores=2,threads=1) under the same deterministic thread=single
-icount regime the one- and two-CPU cells use -- shift=2 here, the
four-CPU-only icount value -- and requires linux-usercheck's own eleven
four-CPU markers: both nodes' CPUs executing, both nodes' memory, both
crosstalk endpoints, and the 0x2c00 remote-memory exit sentinel. Two
blockers once kept it out and are both resolved: a thread=single -icount
pre-console hang, fixed by the fork's icount-translator revert (fork pull
request 178); and a userspace-sentinel shortfall that proved to be a stale
checked-in kernel predating the stub's four-CPU exit path, not a
guest-model bug. An earlier stop_machine livelock at the Hub-RT
clocksource switch was fixed by fork pull request 135. The finding is
docs/notebooks/mttcg-4cpu-usercheck-two-bugs.md (project checkout).

missing_assertions() (test_linux_kernel_verdict.py, imported below) is a
verbatim port of linux-usercheck's own function of the same name, for
the one-, two-, and four-CPU cases.

The QEMU argv this file builds (cpu_qemu_args) replicates
tools/linux_topology.py's own topology_args()/accelerator_args() for the
one-, two-, and four-CPU cells (project checkout): -smp only above one
CPU, the two memory-backends and NUMA nodes at four CPUs, and FIXED
instruction counting (-accel tcg,thread=single -icount) at all -- shift=3
at one and two CPUs, shift=2 at four -- deterministic machine-model
correctness, not a host-speed gate, per that module's own comment.
KERNEL_REL and the rest of the launch (-cpu R10000, -m, -kernel,
-no-reboot) replicate tools/linux-run's own invocation. Neither module
is imported (SELF-CONTAINED, per this port's own brief): linux-run
passes no -append at all -- the embedded userspace's console binds
without one -- and no -bios, since this is a direct kernel boot, not a
firmware boot, which is why this file (unlike test_prom_boot.py and
test_diag.py) never touches ORIGAMI_ASSETS's firmware/ subtree.

The guest never exits on its own: init's death panics the kernel, which
spins rather than terminating the process (linux-run's own -no-reboot
only suppresses reboot-on-panic requests, not this), so this test polls
the captured console transcript against missing_assertions() until it
is satisfied or a timeout expires, then shuts the VM down hard -- the
same shape test_prom_boot.py's own poll loop uses against QEMU's trace
output instead of a guest console.

This module's own pure verdict logic lives entirely in
test_linux_kernel_verdict.py (imported below; it needs no qemu_test
itself -- see that file's own docstring). What is left here is the real
boot.
"""

import os
import socket
import time

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import resolve_asset

from test_linux_kernel_verdict import missing_assertions


# Where ORIGAMI_ASSETS resolves the built kernel, matching tools/linux-run's
# own default kernel path ("assets/linux-ip27/vmlinux", project checkout)
# and test_prom_boot.py's own FIRMWARE_REL idiom: ORIGAMI_ASSETS names a
# CHECKOUT, not assets/ itself.
KERNEL_REL = os.path.join('assets', 'linux-ip27', 'vmlinux')

# Per-boot FAILURE-detection budgets, not typical run time. At one and two
# CPUs tools/linux_topology.py's own default_timeout is 90s (project
# checkout) and a passing boot reaches its markers in a few seconds. The
# four-CPU/two-node boot under fixed instruction counting is far slower --
# every CPU's instructions serialize into one deterministic stream -- so
# it takes a much larger budget; 700s matches the per-4-CPU-leg figure
# origin200-matrix uses for the same topology, and linux_topology's own
# default_timeout(4) of 480s proved too tight for it on a debug build.
# The meson timeout registered for this test (mips64/meson.build) covers
# ALL three methods below run one after another, so it carries a larger
# margin than the usual single-BOOT_TIMEOUT-plus-a-bit this fork's other
# functional tests keep, to stay safe even if every boot runs out its
# budget.
BOOT_TIMEOUT_SECONDS = 90
FOUR_CPU_BOOT_TIMEOUT_SECONDS = 700
POLL_INTERVAL_SECONDS = 0.5


def cpu_qemu_args(cpus):
    """The QEMU args tools/linux_topology.py's topology_args() and
    accelerator_args() build for a one-, two-, or four-CPU cell (project
    checkout), ported directly: -smp only above one CPU; two 64 MiB
    memory-backends and the two NUMA nodes at four CPUs (node 0 gets
    CPUs 0-1, node 1 gets CPUs 2-3); and -accel tcg,thread=single with
    fixed instruction counting at every count -- linux_topology's own
    icount shift=3 at one and two CPUs, shift=2 at four (its four-CPU-only
    value).
    """
    if cpus not in (1, 2, 4):
        raise ValueError('cpu_qemu_args: cpus must be 1, 2, or 4')
    args = []
    if cpus == 2:
        args += ['-smp', '2']
    elif cpus == 4:
        args += ['-object', 'memory-backend-ram,id=ram0,size=64M',
                 '-object', 'memory-backend-ram,id=ram1,size=64M',
                 '-numa', 'node,nodeid=0,cpus=0-1,memdev=ram0',
                 '-numa', 'node,nodeid=1,cpus=2-3,memdev=ram1',
                 '-smp', '4,sockets=2,cores=2,threads=1']
    icount = ('shift=2,align=off,sleep=on' if cpus == 4
              else 'shift=3,align=off,sleep=off')
    args += ['-accel', 'tcg,thread=single', '-icount', icount]
    return args


class LinuxKernel(QemuSystemTest):
    """Boots the mainline Linux/IP27 kernel's embedded userspace at
    one, two, or four CPUs and requires missing_assertions() to return
    empty. See this module's docstring for the full contract; this
    class only wires it to a real boot, the same division
    test_prom_boot.py keeps between its pure logic and PromBoot.
    """

    def _kernel_path(self):
        """The built kernel under ORIGAMI_ASSETS, or None if it is not
        there -- the runner may not have it built yet (see this
        method's caller)."""
        return resolve_asset(os.environ['ORIGAMI_ASSETS'], KERNEL_REL)

    def _boot_and_capture(self, cpus, mem_mib, name):
        kernel = self._kernel_path()
        if kernel is None:
            # Every caller is already @skipIfMissingEnv('ORIGAMI_ASSETS'),
            # so getting here means it IS set and the kernel is simply
            # missing underneath it -- a provisioning failure, not an
            # absent-assets developer run. Fail loudly: a silent skip
            # here would let a real linux-usercheck regression pass CI
            # unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but no kernel is present '
                f'at <ORIGAMI_ASSETS>/{KERNEL_REL} -- provision it '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')

        vm = self.get_vm(name=name)
        vm.set_machine('origin200')
        vm.set_console()
        vm.add_args('-cpu', 'R10000', '-m', str(mem_mib),
                   '-kernel', kernel, '-no-reboot',
                   *cpu_qemu_args(cpus))
        vm.launch()

        log_path = self.log_file(f'{name}.log')
        timeout = (FOUR_CPU_BOOT_TIMEOUT_SECONDS if cpus == 4
                   else BOOT_TIMEOUT_SECONDS)
        deadline = time.monotonic() + timeout
        transcript = ''
        sock = vm.console_socket
        sock.settimeout(POLL_INTERVAL_SECONDS)
        try:
            with open(log_path, 'a', encoding='utf-8') as log:
                while time.monotonic() < deadline:
                    if not missing_assertions(transcript, cpus):
                        break
                    try:
                        chunk = sock.recv(4096)
                    except socket.timeout:
                        chunk = b''
                    except OSError:
                        break
                    if chunk:
                        text = chunk.decode('utf-8', errors='replace')
                        transcript += text
                        log.write(text)
                        log.flush()
                    if vm.exitcode() is not None:
                        break
        finally:
            # A VM that crashed or exited on its own has already
            # left; a graceful shutdown() against an already-dead
            # process still raises AbnormalShutdown even though qemu
            # is in fact gone. Matches PromBoot.test_prom_boot's own
            # reasoning in test_prom_boot.py.
            vm.shutdown(hard=vm.exitcode() is not None)

        return transcript

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_one_cpu(self):
        name = 'linux_kernel_1cpu'
        transcript = self._boot_and_capture(cpus=1, mem_mib=64, name=name)
        missing = missing_assertions(transcript, 1)
        self.assertEqual(
            missing, [],
            'linux-usercheck one-CPU contract: missing ' +
            ', '.join(missing) +
            f' (full transcript: {self.log_file(f"{name}.log")})')

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_two_cpu(self):
        name = 'linux_kernel_2cpu'
        transcript = self._boot_and_capture(cpus=2, mem_mib=64, name=name)
        missing = missing_assertions(transcript, 2)
        self.assertEqual(
            missing, [],
            'linux-usercheck two-CPU contract: missing ' +
            ', '.join(missing) +
            f' (full transcript: {self.log_file(f"{name}.log")})')

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_four_cpu(self):
        name = 'linux_kernel_4cpu'
        transcript = self._boot_and_capture(cpus=4, mem_mib=128, name=name)
        missing = missing_assertions(transcript, 4)
        self.assertEqual(
            missing, [],
            'linux-usercheck four-CPU contract: missing ' +
            ', '.join(missing) +
            f' (full transcript: {self.log_file(f"{name}.log")})')


if __name__ == '__main__':
    QemuSystemTest.main()
