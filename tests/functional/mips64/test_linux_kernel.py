#!/usr/bin/env python3
#
# Linux/IP27 userspace self-check: netboot the mainline kernel with its
# embedded userspace stub through the stock PROM and require Linux's own
# dying-init exit status
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/linux-usercheck's one-, two-, and four-CPU checks, in the
PROJECT checkout, into this fork's own functional-test framework, the same
way test_prom_boot.py ports tools/boot-gate and test_diag.py ports
tools/diag-gate. linux-usercheck boots the mainline Linux/IP27 kernel
(assets/linux-ip27/vmlinux, built by tools/linux-userspace with an
embedded freestanding userspace stub) -- no disk, no terminal device for
the guest -- and requires the console transcript to contain Linux's own
"Attempted to kill init! exitcode=0x0000NN00" panic line with the exact
byte the embedded init chose: 42 << 8 = 0x2a00 at one CPU; 43 << 8 =
0x2b00 at two CPUs, after two affinity-pinned workers stress both CPUs;
or 44 << 8 = 0x2c00 at four CPUs across two nodes, after a node-0 process
moves and verifies a page on node 1. See test_linux_kernel_verdict.py's
own docstring for why a plain literal-string match against the full
8-hex value already rejects a fatal-signal death (the low byte would
carry a signal number instead) without needing any extra logic -- and
for its fatal-signal fixture, which proves it.

The kernel loads through the real stock IP27 PROM's own netboot path, the
same command-monitor dialogue test_prom_sash.py drives (lib_prom's
prom_netboot_steps(), imported below): the System Maintenance Menu's
option 5, a stale netaddr cleared, then `boot -f bootp()/vmlinux` over
QEMU's own SLIRP usermode networking, serving the kernel straight out of
its ORIGAMI_ASSETS directory (no staged copy -- SLIRP's own TFTP server
only ever reads it). This replaces a direct `-kernel` load: the plan this
test belongs to retires synthesized firmware, so no product boots any
other way any more, and the mainline kernel is a 64-bit ELF the PROM
loads directly, the same as sash64.

The four-CPU case is IN scope. It boots the same kernel on the two-node
topology (topology=origin200-dual, -smp 4;
-m splits evenly into 64 MiB per node) under the same deterministic
thread=single
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
KERNEL_REL and the rest of the launch (-cpu R10000, -m) replicate
tools/linux-run's own invocation; linux-run passes no -append at all, so
neither does the boot command below -- the embedded userspace's console
binds without one. `-no-reboot`, part of the direct-`-kernel` launch this
ports, is dropped here: booting the real PROM (`-bios`) issues a reset
classified as guest-initiated at machine init, before the PROM has run a
single instruction, and `-no-reboot` turns that into an immediate
shutdown with no console output at all -- reproduced directly by
launching this exact command line with and without `-no-reboot`, the
difference between a boot that runs and one that exits in well under a
second. That reset is a QEMU-core classification of the origin200
board's own realize-time reset chain, not guest code, and its
correction belongs to a machine-model change, out of scope for a
tests-only packet; it costs nothing here because the embedded userspace
stub never asks the kernel to reboot on panic (see below), so `-kernel`'s
own reason for carrying `-no-reboot` -- stopping a reboot loop -- never
applied to netboot in the first place.

The guest never exits on its own: init's death panics the kernel, which
spins rather than terminating the process, so this test drives the
netboot dialogue and then keeps reading against missing_assertions()
until it is satisfied or a timeout expires, then shuts the VM down hard.
drive_console() (lib_prom.py) owns that loop; test_prom_sash.py's own
docstring covers why it is the console driver every netboot test in this
directory shares.

This module's own pure verdict logic lives entirely in
test_linux_kernel_verdict.py (imported below; it needs no qemu_test
itself -- see that file's own docstring). What is left here is the real
boot.
"""

import os

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FIRMWARE_REL, drive_console, prom_netboot_steps,
                      resolve_asset)

from test_linux_kernel_verdict import missing_assertions


# Where ORIGAMI_ASSETS resolves the built kernel, matching tools/linux-run's
# own default kernel path ("assets/linux-ip27/vmlinux", project checkout)
# and lib_prom.FIRMWARE_REL's own idiom: ORIGAMI_ASSETS names a CHECKOUT,
# not assets/ itself.
KERNEL_REL = os.path.join('assets', 'linux-ip27', 'vmlinux')

# The boot file this test's own SLIRP TFTP server serves the kernel under
# -- see prom_netboot_steps()'s own docstring for why this need not match
# the kernel's name on disk. Kept flat (no path-structuring check, unlike
# test_prom_sash.py's NETBOOT_PATH): this port carries no assertion that
# would tell the two apart.
NETBOOT_BOOTFILE = 'vmlinux'
NETBOOT_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)

# Per-boot FAILURE-detection budgets, not typical run time -- real
# measurements, not guesses: a run of this exact test against real
# provisioned assets (podman, this worktree mounted read-write and a
# checkout carrying assets/ mounted read-only as ORIGAMI_ASSETS) reached
# its exit sentinel in 52s at one CPU, 58s at two, and 310s at four, on a
# host already busy running other work. Each of these times now covers a
# real PROM power-on self-test and netboot on top of the kernel's own
# boot, so it is well above tools/linux_topology.py's own default_timeout
# (90s at one and two CPUs, project checkout), which timed only a direct
# -kernel load with no firmware in front of it. 240 at one and two CPUs
# keeps roughly four times the measured figure, the same margin this
# directory's own meson.build keeps for sn0_smp; 900 at four CPUs keeps
# closer to three times, in line with the margin xframe_pci keeps over
# its own measured boot. The meson timeout registered for this test
# (mips64/meson.build) covers ALL three methods below run one after
# another, so it carries a larger margin than the usual
# single-BOOT_TIMEOUT-plus-a-bit this fork's other functional tests keep,
# to stay safe even if every boot runs out its budget.
BOOT_TIMEOUT_SECONDS = 240
FOUR_CPU_BOOT_TIMEOUT_SECONDS = 900


def _timeout_multiplier():
    """TIMEOUT_MULTIPLIER, meson's own name and convention
    (scripts/mtest2make.py: unset means 1) for scaling a registered
    timeout under load. Nothing here runs through `meson test`, so
    nothing multiplies this budget unless a caller (tools/linux-boot-check,
    via functional_check.py) sets the variable itself; an unparseable or
    non-positive value falls back to 1 rather than shrinking or zeroing a
    real boot's deadline."""
    raw = os.environ.get('TIMEOUT_MULTIPLIER', '1')
    try:
        value = float(raw)
    except ValueError:
        return 1.0
    return value if value > 0 else 1.0


def machine_props(cpus):
    """The -M origin200,... property string for a one-, two-, or
    four-CPU cell: four CPUs need the two-node topology, named
    explicitly since topology= no longer follows from -smp alone.
    """
    if cpus == 4:
        return 'origin200,topology=origin200-dual,debug-leds=off'
    return 'origin200,debug-leds=off'


def cpu_qemu_args(cpus):
    """The QEMU args tools/linux_topology.py's topology_args() and
    accelerator_args() build for a one-, two-, or four-CPU cell (project
    checkout), ported directly: -smp only above one CPU, splitting
    evenly across the two-node topology's nodes at four CPUs (node 0
    gets CPUs 0-1, node 1 gets CPUs 2-3); and -accel tcg,thread=single
    with fixed instruction counting at every count -- linux_topology's
    own icount shift=3 at one and two CPUs, shift=2 at four (its
    four-CPU-only value).
    """
    if cpus not in (1, 2, 4):
        raise ValueError('cpu_qemu_args: cpus must be 1, 2, or 4')
    args = []
    if cpus == 2:
        args += ['-smp', '2']
    elif cpus == 4:
        args += ['-smp', '4']
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

    def _asset_paths(self):
        """The real PROM image and the built kernel under ORIGAMI_ASSETS,
        or None if either is missing -- the runner may not have them
        provisioned yet (see this method's caller)."""
        assets_root = os.environ['ORIGAMI_ASSETS']
        firmware = resolve_asset(assets_root, FIRMWARE_REL)
        kernel = resolve_asset(assets_root, KERNEL_REL)
        if not (firmware and kernel):
            return None
        return firmware, kernel

    def _boot_and_capture(self, cpus, mem_mib, name):
        paths = self._asset_paths()
        if paths is None:
            # Every caller is already @skipIfMissingEnv('ORIGAMI_ASSETS'),
            # so getting here means it IS set and the firmware or kernel
            # is simply missing underneath it -- a provisioning failure,
            # not an absent-assets developer run. Fail loudly: a silent
            # skip here would let a real linux-usercheck regression pass
            # CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but the PROM/kernel are missing '
                f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{KERNEL_REL} -- provision them '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')
        firmware, kernel = paths

        vm = self.get_vm(name=name)
        vm.set_machine(machine_props(cpus))
        vm.set_console()
        vm.add_args('-cpu', 'R10000', '-m', str(mem_mib),
                   '-bios', firmware,
                   '-nic', ('user,id=net0,'
                            f'tftp={os.path.dirname(kernel)},'
                            f'bootfile={NETBOOT_BOOTFILE}'),
                   *cpu_qemu_args(cpus))
        vm.launch()

        log_path = self.log_file(f'{name}.log')
        timeout = (FOUR_CPU_BOOT_TIMEOUT_SECONDS if cpus == 4
                   else BOOT_TIMEOUT_SECONDS) * _timeout_multiplier()
        try:
            transcript, _reached = drive_console(
                vm.console_socket, log_path, timeout, NETBOOT_STEPS,
                reached_when=lambda t: not missing_assertions(
                    t.decode('utf-8', errors='replace'), cpus),
                vm=vm)
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
