#!/usr/bin/env python3
#
# Linux/IP27 userspace self-check: netboot the mainline kernel with its
# embedded userspace stub through the stock PROM and require Linux's own
# dying-init exit status
#
# SPDX-License-Identifier: BSD-3-Clause
"""
This test boots the mainline Linux/IP27 kernel
(linux-ip27/vmlinux, built by tools/linux-userspace with an
embedded freestanding userspace stub) -- no disk, no terminal device for
the guest -- and requires the console transcript to contain Linux's own
"Attempted to kill init! exitcode=0x0000NN00" panic line with the exact
byte the embedded init chose: 42 << 8 = 0x2a00 at one CPU; 43 << 8 =
0x2b00 at two CPUs, after two affinity-pinned workers stress both CPUs;
or 44 << 8 = 0x2c00 at four CPUs across two nodes, after a node-0 process
moves and verifies a page on node 1. A plain literal-string match against
the full 8-hex value already rejects a fatal-signal death (the low byte
would carry a signal number instead) without needing any extra logic;
MARKERS below says so where the values are.

The kernel loads through the real stock IP27 PROM's own netboot path, the
same command-monitor dialogue test_prom_sash.py drives (lib_prom's
prom_netboot_steps(), imported below): the System Maintenance Menu's
option 5, a stale netaddr cleared, then `boot -f bootp()/vmlinux` over
QEMU's own SLIRP usermode networking, serving the kernel straight out of
its QEMU_TEST_SGI_ASSET_DIR directory (no staged copy -- SLIRP's own TFTP server
only ever reads it). This replaces a direct `-kernel` load: the plan this
test belongs to retires synthesized firmware, so no product boots any
other way any more, and the mainline kernel is a 64-bit ELF the PROM
loads directly, the same as sash64.

The four-CPU case uses the two-node topology and checks both nodes' CPUs,
memory, crosstalk endpoints, and the 0x2c00 remote-memory exit sentinel.

The test loads the PROM and kernel from QEMU_TEST_SGI_ASSET_DIR and serves
the kernel over SLIRP TFTP. It supplies no -append argument because the
embedded userspace configures its own console. It omits -no-reboot so the
PROM can run after the machine's initial reset.

The guest does not exit after init panics, so the test reads the console
until the required markers appear or a timeout expires, then stops QEMU.
A fatal signal cannot pass the exit check because the full status value
is matched, including its low byte.

missing_assertions() checks the expected markers for each CPU count. The
full exit status is matched so a fatal signal cannot satisfy the check.
"""

import os

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FIRMWARE_REL, MACHINES, drive_console,
                      prom_netboot_steps, verdict)
from lib_prom_sash import RELEASES


# What the kernel's embedded userspace self-check prints, per CPU count.
# The one-CPU case checks exactly two markers and deliberately
# does NOT check a discovery line: a real one-CPU capture happening to
# print one says nothing about whether every one must, and the shipped
# tool's own contract does not require it.
#
# A fatal signal cannot masquerade as a chosen exit without any extra
# logic here: the kernel prints the full eight hex digits every time, with
# the chosen status in the upper byte and a signal's number in the lower
# one, so a signal death is a wholly different string rather than a prefix
# of the wanted one.
ONE_CPU_MARKERS = (
    ('init handoff', 'Run /init as init process'),
    ('userspace exit status', 'exitcode=0x00002a00'),
)
TWO_CPU_MARKERS = (
    ('two-CPU discovery', 'Discovered 2 cpus on 1 nodes'),
    ('secondary CPU execution', 'CPU1 revision is:'),
    ('two-CPU scheduler bring-up', 'smp: Brought up 1 node, 2 CPUs'),
    ('init handoff', 'Run /init as init process'),
    ('userspace exit status', 'exitcode=0x00002b00'),
)
# The four-CPU exit sentinel (0x2c00 = 44 << 8) is a node-0 process that
# moved and verified one page on node 1.
FOUR_CPU_MARKERS = (
    ('four-CPU discovery', 'Discovered 4 cpus on 2 nodes'),
    ('node-zero secondary CPU execution', 'CPU1 revision is:'),
    ('node-one primary CPU execution', 'CPU2 revision is:'),
    ('node-one secondary CPU execution', 'CPU3 revision is:'),
    ('four-CPU scheduler bring-up', 'smp: Brought up 2 nodes, 4 CPUs'),
    ('node-zero memory',
     'node   0: [mem 0x0000000000000000-0x0000000003ffffff]'),
    ('node-one memory',
     'node   1: [mem 0x0000000100000000-0x0000000103ffffff]'),
    ('node-zero crosstalk endpoint', 'xtalk:n0/8 bridge widget'),
    ('node-one crosstalk endpoint', 'xtalk:n1/8 bridge widget'),
    ('init handoff', 'Run /init as init process'),
    ('node-one remote-memory userspace proof', 'exitcode=0x00002c00'),
)
MARKERS = {1: ONE_CPU_MARKERS, 2: TWO_CPU_MARKERS, 4: FOUR_CPU_MARKERS}


def missing_assertions(log, cpus):
    """Which of the one-, two-, or four-CPU self-check markers never
    appeared in log. An empty return means every required marker was
    found; any other CPU count raises rather than silently answering a
    question this test does not cover."""
    if cpus not in MARKERS:
        raise ValueError('missing_assertions: cpus must be 1, 2, or 4')
    return verdict(log, MARKERS[cpus])


# The native QEMU functional test is registered as
# func-mips64-linux_kernel. Resolve its kernel input beneath
# QEMU_TEST_SGI_ASSET_DIR, which points directly at the directory containing
# linux-ip27/vmlinux and the PROM asset used by lib_prom.FIRMWARE_REL.
KERNEL_REL = os.path.join('linux-ip27', 'vmlinux')

# The boot file this test's own SLIRP TFTP server serves the kernel under
# -- see prom_netboot_steps()'s own docstring for why this need not match
# the kernel's name on disk. Kept flat (no path-structuring check, unlike
# test_prom_sash.py's NETBOOT_PATH): this port carries no assertion that
# would tell the two apart.
NETBOOT_BOOTFILE = 'vmlinux'
NETBOOT_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)

# These boots include the real PROM self-test and netboot before Linux.
# A debug_tcg one-CPU run took 296s. Matched two-CPU runs on 2026-10-03
# took 523s on trunk and 455s with the CPU lookup optimizations, including
# temporary timing probes. Both exceeded the earlier 240s deadline before
# Linux started. Keep 600s for one and two CPUs to cover these observed
# runs, and the existing 900s for four CPUs. The Meson timeout registered
# for this test (mips64/meson.build) covers all three methods sequentially,
# with additional time for VM launch and shutdown.
ONE_CPU_BOOT_TIMEOUT_SECONDS = 600
TWO_CPU_BOOT_TIMEOUT_SECONDS = 600
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
    return MACHINES['origin200_dual' if cpus == 4 else 'origin200']


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

    ASSET_FIRMWARE = LocalAsset(
        FIRMWARE_REL, RELEASES['6.5.30']['prom_sha256'])
    ASSET_KERNEL = LocalAsset(
        KERNEL_REL,
        'd095c9033e2f8aeca0a75adbbdc1d0acdf764230999b63eb6261574d65c77bd4')

    def _boot_and_capture(self, cpus, mem_mib, name):
        firmware = self.ASSET_FIRMWARE.fetch()
        kernel = self.ASSET_KERNEL.fetch()

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
        timeout = (FOUR_CPU_BOOT_TIMEOUT_SECONDS if cpus == 4 else
                   TWO_CPU_BOOT_TIMEOUT_SECONDS if cpus == 2 else
                   ONE_CPU_BOOT_TIMEOUT_SECONDS) * _timeout_multiplier()
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

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_one_cpu(self):
        name = 'linux_kernel_1cpu'
        transcript = self._boot_and_capture(cpus=1, mem_mib=64, name=name)
        missing = missing_assertions(transcript, 1)
        self.assertEqual(
            missing, [],
            'linux-usercheck one-CPU contract: missing ' +
            ', '.join(missing) +
            f' (full transcript: {self.log_file(f"{name}.log")})')

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_two_cpu(self):
        name = 'linux_kernel_2cpu'
        transcript = self._boot_and_capture(cpus=2, mem_mib=64, name=name)
        missing = missing_assertions(transcript, 2)
        self.assertEqual(
            missing, [],
            'linux-usercheck two-CPU contract: missing ' +
            ', '.join(missing) +
            f' (full transcript: {self.log_file(f"{name}.log")})')

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
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
