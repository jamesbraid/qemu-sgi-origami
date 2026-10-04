# Origin 300 boot-flash seeding for the stock IP35 PROM boot tests in this
# directory
#
# SPDX-License-Identifier: BSD-3-Clause
"""
origin300 takes only a raw stock PROM image as -bios and refuses -kernel
outright, with no synthesized-firmware bypass (test_origin300_prom.py's
own module docstring); reaching the System Maintenance Menu -- the path
the installed IRIX disk boots through -- also needs a writable boot flash,
not just the -bios image the first-line smoke test uses.

This helper builds that boot flash. It uses plain Python and has no
qemu_test dependency, as in lib_prom.py. The machine builds the node and
IO8 identity records itself: each node's board record from its fitted
processor count, and the IO8 records from the machine's mac= property.
No SPD record is supplied; the L1 answers each DIMM slot as not present.

The boot flash is written fresh into the caller's own scratch directory,
never into the asset tree. The firmware image is read from
QEMU_TEST_SGI_ASSET_DIR.
"""

import hashlib
import os

FIRMWARE_REL = os.path.join('firmware', '6.5.30',
                            'ip35prom.payload.bin')
FIRMWARE_SHA256 = (
    '554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b')

# The test boot flash is 16 MiB: it contains the stock payload followed
# by 0xff fill, an outer "PLOI" label at a fixed offset, and a
# Disable<slot> environment record for each empty processor-interface
# slot in the selected population.
BOOT_FLASH_BYTES = 16777216
BOOT_FLASH_FILL = 0xff
BOOT_FLASH_LABEL_OFFSET = 0x009e0000
BOOT_FLASH_LABEL = b'\xff' * 16 + b'PLOI' + b'\x00\x00\x00\x01' * 2
BOOT_FLASH_DISABLE_VALUE = b'32: CPU failed early init.'
BOOT_FLASH_ENVIRONMENT = {
    2: (
        (0x009e0100,
         b'\x10DisableB' + b'\x00' * 7
         + BOOT_FLASH_DISABLE_VALUE + b'\x00' * 22),
        (0x009e0140,
         b'\x10DisableD' + b'\x00' * 7
         + BOOT_FLASH_DISABLE_VALUE + b'\x00' * 22),
    ),
    4: (),
}
# Expected SHA-256 digests for the generated boot-flash image, keyed by
# processor population. Each value pins the fixture built from the
# firmware asset named above.
BOOT_FLASH_SEED_SHA256 = {
    2: '6df02e413cc71206badcd375dfc53499e9fc6a193b76e3e37baca515e92b2f0f',
    4: '277bbe789c0657f048a41820e684f6ddece6af86301bf014293423cc4ab74c5b',
}

# Node 0's onboard Ethernet address, passed as mac=. The machine writes
# it into the IO8 board record and gives the IOC3 the same address.
NETWORK_MAC = '08:00:69:12:34:56'

# The relative-path/SHA-256 pairs used by callers to declare the exact
# firmware input each test needs.
ASSET_SPECS = ((FIRMWARE_REL, FIRMWARE_SHA256),)


def _write(path, contents):
    partial = path + '.partial'
    with open(partial, 'wb') as out:
        out.write(contents)
    os.replace(partial, path)


def _build_boot_flash(firmware_path, population, destination):
    """Write a 16 MiB writable boot flash seeded from the stock PROM
    payload at firmware_path, with the Disable<slot> records this
    population leaves fitted-but-unused, to destination.

    Raises ValueError if population has no reviewed seed, or if the
    resulting image does not hash to that seed.
    """
    try:
        environment = BOOT_FLASH_ENVIRONMENT[population]
        expected_seed_sha256 = BOOT_FLASH_SEED_SHA256[population]
    except KeyError:
        raise ValueError(
            'origin300 boot flash has no reviewed seed for a '
            f'{population}-processor population') from None

    with open(firmware_path, 'rb') as source:
        payload = source.read()
    fill = BOOT_FLASH_BYTES - len(payload)
    if fill < 0:
        raise ValueError(
            f'{firmware_path} is larger than the {BOOT_FLASH_BYTES}-byte '
            'boot flash')
    if len(payload) > BOOT_FLASH_LABEL_OFFSET:
        raise ValueError(f'{firmware_path} overlaps the boot-flash label')

    image = payload + bytes([BOOT_FLASH_FILL]) * fill
    label_end = BOOT_FLASH_LABEL_OFFSET + len(BOOT_FLASH_LABEL)
    image = (image[:BOOT_FLASH_LABEL_OFFSET] + BOOT_FLASH_LABEL
             + image[label_end:])
    for offset, record in environment:
        image = image[:offset] + record + image[offset + len(record):]

    digest = hashlib.sha256(image).hexdigest()
    if digest != expected_seed_sha256:
        raise ValueError(
            f'the {population}-processor boot-flash seed built from '
            f'{firmware_path} does not have its reviewed SHA-256 '
            f'({digest} != {expected_seed_sha256})')

    _write(destination, image)


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
    boot_flash = os.path.join(scratch, 'ip35-boot-flash.raw')
    _build_boot_flash(firmware_path, cpus, boot_flash)

    machine = f'origin300,mac={NETWORK_MAC}'
    argv = [
        '-cpu', 'R14000',
        '-m', '512M',
        '-bios', str(firmware_path),
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
    machine, argv = launch_args(firmware_path, populations[0], scratch)
    node_dir = os.path.join(scratch, 'node1')
    os.makedirs(node_dir, exist_ok=True)
    boot_flash = os.path.join(node_dir, 'ip35-boot-flash.raw')
    _build_boot_flash(firmware_path, populations[1], boot_flash)

    machine = ','.join([machine, 'topology=origin300-two-chassis',
                        'population=' + ':'.join(map(str, populations))])
    argv[argv.index('-m') + 1] = '1024M'
    argv[argv.index('-smp') + 1] = str(sum(populations))
    argv += ['-drive', f'if=pflash,index=1,file={boot_flash},format=raw']
    return machine, argv
