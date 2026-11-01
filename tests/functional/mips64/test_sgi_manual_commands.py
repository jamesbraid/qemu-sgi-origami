#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Start every command line the SGI manual's running pages show.

Each page under docs/system/mips/sgi/running*.rst has qemu-system-mips64
commands written by hand, and the sgi-run directive generates more from
the machine catalogue. This test starts each one on the built emulator,
paused at reset, and requires QEMU to create the machine and report it
paused. A page that shows an option the machine no longer accepts fails
here. It needs no assets: the firmware, identity and disk files the
commands name are zero-filled stand-ins of the sizes the machines check.

The stand-ins start no guest. Passing shows that the options are accepted
and the machine is created, not that a PROM or an operating system runs.
Which guests a test does boot is the table in the running overview.
"""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from qemu_test import QemuBaseTest

SOURCE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SOURCE / 'scripts'))
from sgi_model import running_doc as rd  # noqa: E402

QMP_SCRIPT = ('{"execute": "qmp_capabilities"}\n'
              '{"execute": "query-status"}\n'
              '{"execute": "quit"}\n')


class SgiManualCommands(QemuBaseTest):
    timeout = 240

    def setUp(self):
        super().setUp()
        self.catalogue = rd.load()
        self.machine_init = self.build_file(
            'tools', 'sgi', 'qemu-sgi-machine-init')

    def stand_ins(self, directory):
        for name, size in rd.PLACEHOLDERS.items():
            with open(directory / name, 'wb') as f:
                f.truncate(size)

    def run_block(self, label, commands):
        """Run one block's commands in order in a fresh directory and
        return a description of each that failed. The machine-init
        commands create the state directory the later qemu-system-mips64
        command binds."""
        problems = []
        with tempfile.TemporaryDirectory(dir=self.workdir) as tmp:
            directory = Path(tmp)
            self.stand_ins(directory)
            for argv in commands:
                if argv[0] == 'qemu-sgi-machine-init':
                    if not os.access(self.machine_init, os.X_OK):
                        return [f'{self.machine_init} is not built']
                    done = subprocess.run(
                        [self.machine_init, *argv[1:]], cwd=directory,
                        capture_output=True, text=True, timeout=60)
                    if done.returncode:
                        problems.append(f'{label}: {" ".join(argv)}\n'
                                        f'{done.stderr}')
                    continue
                done = self.start(argv[1:], directory)
                if self.status(done) != 'prelaunch':
                    problems.append(f'{label}: {" ".join(argv)}\n'
                                    f'QEMU said:\n{done.stderr}')
        return problems

    def start(self, args, directory):
        return subprocess.run(
            [self.qemu_bin, *rd.start_argv(args)], cwd=directory,
            input=QMP_SCRIPT, capture_output=True, text=True, timeout=90)

    @staticmethod
    def status(done):
        status = None
        for line in done.stdout.splitlines():
            try:
                reply = json.loads(line)
            except ValueError:
                continue
            if 'return' in reply and 'status' in reply['return']:
                status = reply['return']['status']
        return status

    def fail_with(self, problems):
        """One failure that names every stale command. A failed subtest
        prints no message in the TAP output, so the log would not say
        which command to fix."""
        if problems:
            self.fail('\n\n'.join(problems))

    def test_commands_written_in_the_pages(self):
        problems = []
        for name in rd.FAMILIES:
            text = (rd.PAGES / f'{name}.rst').read_text()
            blocks = rd.command_blocks(text)
            if not blocks:
                problems.append(f'{name} shows no command')
            for number, block in enumerate(blocks, 1):
                problems += self.run_block(f'{name} block {number}', block)
        self.fail_with(problems)

    def test_commands_generated_from_the_catalogue(self):
        problems = []
        for product in rd.products(self.catalogue):
            if product != 'o2':
                problems += self.run_block(
                    f'generated launch of {product}',
                    rd.commands(rd.render('launch', product,
                                          self.catalogue)))
        self.fail_with(problems)

    def test_every_catalogue_product_is_a_machine(self):
        done = subprocess.run([self.qemu_bin, '-M', 'help'],
                              capture_output=True, text=True, timeout=60)
        missing = [p for p in rd.products(self.catalogue)
                   if f'\n{p} ' not in '\n' + done.stdout]
        self.fail_with([f'-M help lists no {p}' for p in missing])

    def refusal(self, product, extra, directory):
        """QEMU's complaint when the product's default selection is
        started with extra arguments, or None if it started."""
        o = rd.default_offering(self.catalogue, product)
        options = o['machine-options']
        done = subprocess.run(
            [self.qemu_bin, '-M', options, '-bios', 'ip35prom.img',
             *extra, '-display', 'none', '-monitor', 'none', '-S',
             '-qmp', 'stdio', '-serial', 'null'],
            cwd=directory, input=QMP_SCRIPT, capture_output=True,
            text=True, timeout=90)
        return None if self.status(done) == 'prelaunch' else done.stderr

    def test_the_refusals_the_pages_state(self):
        """The pages say every machine refuses -kernel, and that a product
        with no default topology refuses a launch without topology=. The
        O2 refuses -kernel too, but needs its own inputs to get that far."""
        problems = []
        with tempfile.TemporaryDirectory(dir=self.workdir) as tmp:
            directory = Path(tmp)
            self.stand_ins(directory)
            for product in rd.products(self.catalogue):
                if product == 'o2':
                    continue
                said = self.refusal(product, ['-kernel', 'disk.img'],
                                    directory)
                if not said or '-kernel' not in said:
                    problems.append(f'{product} did not refuse -kernel: '
                                    f'{said}')
                offs = rd.offerings(self.catalogue, product)
                if not offs[0].get('default-topology'):
                    done = subprocess.run(
                        [self.qemu_bin, '-M', product, '-bios',
                         'ip35prom.img', '-display', 'none', '-S'],
                        cwd=directory, capture_output=True, text=True,
                        timeout=60)
                    if done.returncode == 0 or 'topology' not in done.stderr:
                        problems.append(f'{product} started without '
                                        'topology=')
        self.fail_with(problems)


if __name__ == '__main__':
    QemuBaseTest.main()
