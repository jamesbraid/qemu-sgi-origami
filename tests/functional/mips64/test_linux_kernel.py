#!/usr/bin/env python3
#
# Linux/IP27 userspace self-check: boot the mainline kernel with its
# embedded userspace stub and require Linux's own dying-init exit status
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/linux-usercheck's one- and two-CPU checks, in the PROJECT
checkout, into this fork's own functional-test framework, the same way
test_prom_boot.py ports tools/boot-gate and test_diag.py ports
tools/diag-gate. linux-usercheck boots the mainline Linux/IP27 kernel
(assets/linux-ip27/vmlinux, built by tools/linux-userspace with an
embedded freestanding userspace stub) directly via -kernel -- no PROM,
no disk, no terminal device for the guest -- and requires the console
transcript to contain Linux's own "Attempted to kill init!
exitcode=0x0000NN00" panic line with the exact byte the embedded init
chose: 42 << 8 = 0x2a00 at one CPU, or 43 << 8 = 0x2b00 at two CPUs,
reached only after two affinity-pinned workers finish stressing both
CPUs. See test_linux_kernel_verdict.py's own docstring for why a plain
literal-string match against the full 8-hex value already rejects a
fatal-signal death (the low byte would carry a signal number instead)
without needing any extra logic -- and for its fatal-signal fixture,
which proves it.

The four-CPU case is explicitly OUT of scope here: tools/linux-usercheck's
own docstring records a known, pre-existing stop_machine livelock at
that population under wall-clock time, unrelated to this fork's own
work (docs/reviews/mttcg-flip-evidence.md, project checkout). Adding a
four-CPU cell that asserts success would be asserting past a known
condition, which this port's own task brief says explicitly not to do.

missing_assertions() (test_linux_kernel_verdict.py, imported below) is a
close port of linux-usercheck's own function of the same name, for the
one- and two-CPU cases only, with one deliberate addition documented in
that module's own docstring.

The QEMU argv this file builds (cpu_qemu_args) replicates
tools/linux_topology.py's own topology_args()/accelerator_args() for the
one- and two-CPU cells (project checkout): -smp only at two CPUs, and
FIXED instruction counting (-accel tcg,thread=single -icount
shift=3,align=off,sleep=off) at both -- deterministic machine-model
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

Free of any qemu_test import on purpose below the guarded import: this
module's own pure verdict logic lives entirely in
test_linux_kernel_verdict.py (imported unconditionally, since it needs
no qemu_test itself -- see that file's own docstring); only the real
QemuSystemTest below needs qemu_test, which requires MESON_BUILD_ROOT
(see tests/functional/qemu_test/config.py) and is therefore guarded the
same way test_prom_boot.py and test_diag.py guard it.
"""

import os
import socket
import sys
import time

# This file's own directory, so a plain `pytest test_linux_kernel.py` run
# finds test_linux_kernel_verdict.py the same way test_diag.py finds
# itself for test_diag_verdict.py's own import.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_linux_kernel_verdict import missing_assertions  # noqa: E402

try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


# Where ORIGAMI_ASSETS resolves the built kernel, matching tools/linux-run's
# own default kernel path ("assets/linux-ip27/vmlinux", project checkout)
# and test_prom_boot.py's own FIRMWARE_REL idiom: ORIGAMI_ASSETS names a
# CHECKOUT, not assets/ itself.
KERNEL_REL = os.path.join('assets', 'linux-ip27', 'vmlinux')

# tools/linux_topology.py's own default_timeout(cpus): "90" for one and
# two CPUs (project checkout) -- the budget linux-usercheck itself uses
# by default. This is the per-boot FAILURE-detection budget, not typical
# run time: a passing boot reaches its markers in a few seconds. The
# meson timeout registered for this test (mips64/meson.build) covers
# BOTH test methods below run one after another, so it carries a larger
# margin than the usual single-BOOT_TIMEOUT-plus-a-bit this fork's other
# functional tests keep, to stay safe even if both boots independently
# run out their own budgets.
BOOT_TIMEOUT_SECONDS = 90
POLL_INTERVAL_SECONDS = 0.5


def cpu_qemu_args(cpus):
    """The QEMU args tools/linux_topology.py's topology_args() and
    accelerator_args() build for a one- or two-CPU cell (project
    checkout), ported directly: -smp only at two CPUs, and -accel
    tcg,thread=single with fixed instruction counting (icount
    shift=3,align=off,sleep=off) at both -- this is linux_topology's own
    non-four-CPU icount shift, not the four-CPU one (shift=2), since
    four CPUs is out of scope here (see this module's own docstring).
    """
    if cpus not in (1, 2):
        raise ValueError('cpu_qemu_args: cpus must be 1 or 2')
    args = []
    if cpus == 2:
        args += ['-smp', '2']
    args += ['-accel', 'tcg,thread=single',
             '-icount', 'shift=3,align=off,sleep=off']
    return args


if QemuSystemTest is not None:

    class LinuxKernel(QemuSystemTest):
        """Boots the mainline Linux/IP27 kernel's embedded userspace at
        one or two CPUs and requires missing_assertions() to return
        empty. See this module's docstring for the full contract; this
        class only wires it to a real boot, the same division
        test_prom_boot.py keeps between its pure logic and PromBoot.
        """

        def _kernel_path(self):
            """The built kernel under ORIGAMI_ASSETS, or None if the env
            var is unset or the kernel is not there -- the runner may
            not have it built yet (see this method's caller)."""
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            path = os.path.join(assets_root, KERNEL_REL)
            return path if os.path.isfile(path) else None

        def _boot_and_capture(self, cpus, mem_mib, name):
            kernel = self._kernel_path()
            if kernel is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no kernel at '
                    f'<ORIGAMI_ASSETS>/{KERNEL_REL} -- point it at a '
                    'checkout with the embedded-userspace kernel built '
                    '(tools/linux-userspace) to run this test')

            vm = self.get_vm(name=name)
            vm.set_machine('origin200')
            vm.set_console()
            vm.add_args('-cpu', 'R10000', '-m', str(mem_mib),
                       '-kernel', kernel, '-no-reboot',
                       *cpu_qemu_args(cpus))
            vm.launch()

            log_path = self.log_file(f'{name}.log')
            deadline = time.monotonic() + BOOT_TIMEOUT_SECONDS
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

        def test_one_cpu(self):
            name = 'linux_kernel_1cpu'
            transcript = self._boot_and_capture(cpus=1, mem_mib=64, name=name)
            missing = missing_assertions(transcript, 1)
            self.assertEqual(
                missing, [],
                'linux-usercheck one-CPU contract: missing ' +
                ', '.join(missing) +
                f' (full transcript: {self.log_file(f"{name}.log")})')

        def test_two_cpu(self):
            name = 'linux_kernel_2cpu'
            transcript = self._boot_and_capture(cpus=2, mem_mib=64, name=name)
            missing = missing_assertions(transcript, 2)
            self.assertEqual(
                missing, [],
                'linux-usercheck two-CPU contract: missing ' +
                ', '.join(missing) +
                f' (full transcript: {self.log_file(f"{name}.log")})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
