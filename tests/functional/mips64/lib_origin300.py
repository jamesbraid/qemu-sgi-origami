# Origin 300 boot-flash seeding for the stock IP35 PROM boot tests in this
# directory
#
# SPDX-License-Identifier: BSD-3-Clause
"""
origin300 takes only a raw stock PROM image as -bios and refuses -kernel
outright, with no synthesized-firmware bypass (test_origin300_prom.py's
own module docstring); reaching the System Maintenance Menu -- the path
the installed IRIX disk boots through -- also needs a writable boot flash,
not just the -bios image the first-line smoke test uses. With that flash
bound the machine boots from it alone and refuses -bios.

This helper creates that boot flash with qemu-sgi-machine-init, the tool
a launcher uses for a new machine, so a test boots the bytes a user's
machine starts from: the stock payload with its configuration block set
for Origin 300, the outer "PLOI" label, and a Disable<slot> environment
record for each processor slot the population leaves empty. It uses plain
Python and has no qemu_test dependency, as in lib_prom.py. The machine
builds the node and IO8 identity records itself: each node's board record
from its fitted processor count, and the IO8 records from the machine's
mac= property. No SPD record is supplied; the L1 answers each DIMM slot as
not present.

The machine state is written fresh into the caller's own scratch
directory, never into the asset tree. The firmware image is read from
QEMU_TEST_SGI_ASSET_DIR; the tool is the build's own, named by
QEMU_TEST_SGI_MACHINE_INIT.
"""

import hashlib
import os
import shutil
import subprocess

FIRMWARE_REL = os.path.join('firmware', '6.5.30',
                            'ip35prom.payload.bin')
FIRMWARE_SHA256 = (
    '554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b')

# Expected SHA-256 digests of a new 16 MiB boot flash, keyed by the node's
# processor count, for the firmware asset named above. Two processors add
# DisableB and DisableD records to the four-processor image; both differ
# from the boot-flash datasheet's reviewed seeds only in the configuration
# block the tool sets for Origin 300 and its processor module.
BOOT_FLASH_SEED_SHA256 = {
    2: '90426e4ab2addcc1d4a08b3416185b041aabc7100097e720d8db3cd2054a72ca',
    4: 'a5533c60127a65be5b5ab9fc493c51daba0fc46a1e75baac848f56fd24503bc7',
}

# Node 0's onboard Ethernet address, passed as mac=. The machine writes
# it into the IO8 board record and gives the IOC3 the same address.
NETWORK_MAC = '08:00:69:12:34:56'

# The relative-path/SHA-256 pairs used by callers to declare the exact
# firmware input each test needs.
ASSET_SPECS = ((FIRMWARE_REL, FIRMWARE_SHA256),)


def _run_machine_init(args):
    """Run the build's qemu-sgi-machine-init with args."""
    tool = os.environ.get('QEMU_TEST_SGI_MACHINE_INIT')
    if not tool:
        raise RuntimeError('QEMU_TEST_SGI_MACHINE_INIT names no '
                           'qemu-sgi-machine-init binary')
    result = subprocess.run([tool] + args, capture_output=True, text=True,
                            check=False)
    if result.returncode:
        raise RuntimeError(f'{tool} failed: {result.stderr.strip()}')


def _create_boot_flash(firmware_path, topology, populations, scratch):
    """Create a new machine's state for topology with populations, the
    per-node processor counts, in scratch/origin300-state, replacing any
    an earlier launch made there. Returns each node's boot flash path.

    Raises ValueError if a population has no expected digest, or if a
    flash does not hash to it.
    """
    for population in populations:
        if population not in BOOT_FLASH_SEED_SHA256:
            raise ValueError(
                'origin300 boot flash has no reviewed seed for a '
                f'{population}-processor population')

    state = os.path.join(scratch, 'origin300-state')
    shutil.rmtree(state, ignore_errors=True)
    _run_machine_init([
        '--machine',
        f'origin300,topology={topology},nodes={len(populations)}',
        '--population', ':'.join(map(str, populations)),
        '--boot-prom', firmware_path, state])

    flashes = []
    for node, population in enumerate(populations):
        flash = os.path.join(state, f'node{node}-flash.raw')
        with open(flash, 'rb') as image:
            digest = hashlib.sha256(image.read()).hexdigest()
        expected = BOOT_FLASH_SEED_SHA256[population]
        if digest != expected:
            raise ValueError(
                f'node {node}\'s {population}-processor boot flash built '
                f'from {firmware_path} does not have its reviewed SHA-256 '
                f'({digest} != {expected})')
        flashes.append(flash)
    return flashes


def launch_args(firmware_path, cpus, scratch):
    """The -M property string and the remaining argv fragment for a real
    Origin 300 stock-PROM boot with cpus processors fitted.

    firmware_path is the already fetched and hash-checked firmware.
    scratch is a fresh, writable, per-test directory (self.workdir): the
    boot flash is built there, never into the asset tree.

    Returns (machine, argv). origin300's machine name takes mac= only in a
    comma-bearing string, which self.set_machine() cannot validate (it
    checks the bare name against query-machines), so the caller applies
    machine with self.vm.set_machine() after self.set_machine('origin300')
    validates the bare name, then passes argv to self.vm.add_args() -- the
    same idiom test_sn0_port_reset.py and test_sn0_elsc_reset.py use for
    origin200,debug-leds=off.
    """
    boot_flash, = _create_boot_flash(firmware_path, 'origin300', (cpus,),
                                     scratch)
    return _launch_args(boot_flash, cpus)


def _launch_args(boot_flash, cpus):
    machine = f'origin300,mac={NETWORK_MAC}'
    argv = [
        '-cpu', 'R14000',
        '-m', '512M',
        '-drive',
        f'if=pflash,index=0,file={boot_flash},format=raw',
        '-smp', str(cpus),
        '-accel', 'tcg,thread=multi',
    ]
    return machine, argv


def launch_args_two_chassis(firmware_path, populations, scratch):
    """The -M property string and argv fragment for the two-chassis
    Origin 300 (topology=origin300-two-chassis), each chassis with its
    own boot flash seeded for its own processor count.

    populations is the per-node processor counts in node order, (4, 4)
    or (2, 2) and so on. The machine names each node's board for its own
    processor count and gives node 1's IO8 the address one past
    NETWORK_MAC. Node 1's L1 console is serial line 5, left unconnected
    here; the master's is line 0.
    """
    flashes = _create_boot_flash(firmware_path, 'origin300-two-chassis',
                                 populations, scratch)
    machine, argv = _launch_args(flashes[0], populations[0])

    machine = ','.join([machine, 'topology=origin300-two-chassis',
                        'population=' + ':'.join(map(str, populations))])
    argv[argv.index('-m') + 1] = '1024M'
    argv[argv.index('-smp') + 1] = str(sum(populations))
    argv += ['-drive', f'if=pflash,index=1,file={flashes[1]},format=raw']
    return machine, argv
