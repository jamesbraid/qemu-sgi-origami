# Real Origin 300 boot-flash and identity-EEPROM seeding for the stock
# IP35 PROM boot tests in this directory
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
origin300 takes only a raw stock PROM image as -bios and refuses -kernel
outright, with no synthesized-firmware bypass (test_origin300_prom.py's
own module docstring); reaching the System Maintenance Menu -- the path
the installed IRIX disk boots through -- also needs a writable boot flash
and the chassis/board/SPD identity records the PROM's own memory and
inventory code reads, not just the -bios image the first-line smoke test
uses.

This ports the same seeding tools/origami_lib/prom.py and
tools/origami_lib/io8_eeprom.py build for a real `tools/origami run
prom --machine origin300`: those are this project's own tools, and the
task that added this module was told copying their logic is permitted.
The boot-flash layout, the environment records, the two population
seed digests and the synthetic IO8 record encoding are the same
reviewed values and byte layout; nothing here is independently
rederived. Plain Python with no qemu_test dependency, the same rule
lib_prom.py follows.

Every image this module builds (the boot flash and the synthesized node
and IO8 identity records) is written fresh into the caller's own scratch
directory, never into the asset tree. The firmware image and the two
SPD records are read from QEMU_TEST_SGI_ASSET_DIR.
"""

import hashlib
import os

from lib_prom import resolve_asset

FIRMWARE_REL = os.path.join('firmware', '6.5.30',
                            'ip35prom.payload.bin')
FIRMWARE_SHA256 = (
    '554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b')

# tools/origami_lib/prom.py's own boot-flash layout: a 16 MiB writable
# part, the stock payload followed by 0xff fill, an outer "PLOI" label
# spliced in at a fixed offset, and -- keyed by fitted processor count --
# a Disable<slot> environment record for each processor-interface slot
# that population leaves empty (slices A and C are always fitted; a
# two-processor launch leaves B and D empty, a four-processor launch
# leaves none).
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
# The reviewed digest of the seeded image for each population, built from
# the identified IRIX 6.5.30 IP35 payload named above -- the same hard
# check tools/origami_lib/prom.py's publish_origin300_boot_flash makes,
# so a change to the layout above or a wrong firmware image fails loudly
# here rather than producing a boot flash that quietly does not match
# what was reviewed.
BOOT_FLASH_SEED_SHA256 = {
    2: '6df02e413cc71206badcd375dfc53499e9fc6a193b76e3e37baca515e92b2f0f',
    4: '277bbe789c0657f048a41820e684f6ddece6af86301bf014293423cc4ab74c5b',
}

NETWORK_MAC = '08:00:69:12:34:56'

# The public matched kit occupies DIMMs 2/3, exposed as L1 components 3/5
# (tools/origami_lib/prom.py's own ORIGIN300_SPD_INPUTS comment).
SPD_INPUTS = (
    ('spd-eeprom.3',
     os.path.join('eeprom', 'origin300-dimm2-spd.bin'),
     'a8bb5857941fefae8e11037fe98b01d199199bd34eb059b269a8c4b6875ca0d3'),
    ('spd-eeprom.5',
     os.path.join('eeprom', 'origin300-dimm3-spd.bin'),
     '9e60c3400772d227b91c5d61cea8de7f89777d41377516de072944c26cef4aa7'),
)

# The relative-path/SHA-256 pairs a caller passes to require_assets(self,
# *ASSET_SPECS) to resolve, and skip or fail on, every real file
# launch_args() below reads.
ASSET_SPECS = ((FIRMWARE_REL, FIRMWARE_SHA256),) + tuple(
    (rel, sha256) for _prop, rel, sha256 in SPD_INPUTS)

# OpenBSD's Information-Area type/length tag: top two bits the type, low
# six bits the length in bytes. Ported from tools/origami_lib/
# io8_eeprom.py, which cites SGI 007-4498-002 (printed pages 181-182) and
# 007-4365-001 (printed page 75) for the field boundaries; the values
# built below use those boundaries but are not factory contents -- no
# EEPROM part, serial number, manufacturing date or encryption key is
# inferred from the printed examples.
_IA_TYPE_BINARY = 0
_IA_TYPE_ASCII = 3
_IA_EOF = 0xC1


def _tl(kind, length):
    if not 0 <= length <= 0x3F:
        raise ValueError(f'Information-Area field too long: {length}')
    return (kind << 6) | length


def _field(kind, data):
    return bytes([_tl(kind, len(data))]) + data


def _mac_digits(mac):
    return ''.join(mac.split(':')).upper().encode('ascii')


def _build_io8_chassis_eeprom_record():
    """The synthetic component-1 (IO8) chassis area: 15-byte body plus
    one checksum byte."""
    body = bytes([0x00, 0x02, 0x00]) + (
        _field(_IA_TYPE_ASCII, b'NA')
        + _field(_IA_TYPE_ASCII, b'NA')
        + bytes([_IA_EOF])
        + bytes(5)
    )
    assert len(body) == 15
    record = bytearray(body)
    record.append((-sum(record)) & 0xFF)
    return bytes(record)


def _build_io8_board_eeprom_record(mac):
    """The synthetic component-1 (IO8) board area carrying mac: 79-byte
    body plus one checksum byte."""
    header = bytes([0x00, 0x0a, 0x00, 0x00, 0x00, 0x00])
    identity = (
        _field(_IA_TYPE_ASCII, b'SYNTHETIC')
        + _field(_IA_TYPE_ASCII, b'IO8')
        + _field(_IA_TYPE_ASCII, b'000000')
        + _field(_IA_TYPE_ASCII, b'NOTOBSERVED0')
    )
    fru_id = bytes([0x00])
    fixed_fields = (
        _field(_IA_TYPE_ASCII, b'00')
        + _field(_IA_TYPE_BINARY, bytes([0x00]))
        + _field(_IA_TYPE_ASCII, b'00')
        + _field(_IA_TYPE_BINARY, bytes(4)) * 3
    )
    network = _field(_IA_TYPE_ASCII, _mac_digits(mac))
    body = (header + identity + fru_id + fixed_fields + network
            + bytes([_IA_EOF, 0x00]))
    assert len(body) == 79
    record = bytearray(body)
    record.append((-sum(record)) & 0xFF)
    return bytes(record)


# The node board's own identity, L1 component 0: tools/origami_lib/
# node_eeprom.py in the project checkout, ported the same way. IRIX builds
# the node vertex's _nic attribute from these two areas, and the online
# diagnostics' harness reads its machine name from that attribute: with no
# node-board record the name stays unset and olsio dies with SIGSEGV
# (the project checkout's olsio root-cause notebook). Every captured real
# Origin 300 inventory shows a node board. The board names, part numbers
# and revision are those real inventories print; everything else is a
# placeholder, as in the IO8 records.
NODE_BOARD_IDENTITY = {
    2: (b'IP45_2CPU', b'030-1780-001', b'-C'),
    4: (b'IP45_4CPU', b'030-1779-001', b'-C'),
}


def _build_node_chassis_eeprom_record():
    """The component-0 chassis area: the same placeholder the IO8 uses."""
    return _build_io8_chassis_eeprom_record()


def _build_node_board_eeprom_record(processors):
    """The synthetic component-0 board area for a node with processors
    fitted, padded to the eight-byte units its length byte counts."""
    try:
        name, part, revision = NODE_BOARD_IDENTITY[processors]
    except KeyError:
        raise ValueError(
            f'no Origin 300 node board carries {processors} processors'
        ) from None
    fields = (
        _field(_IA_TYPE_ASCII, b'SYNTHETIC')
        + _field(_IA_TYPE_ASCII, name)
        + _field(_IA_TYPE_ASCII, b'000000')
        + _field(_IA_TYPE_ASCII, part)
        + bytes([0x00])  # FRU file id
        + _field(_IA_TYPE_ASCII, revision)
        + _field(_IA_TYPE_BINARY, bytes([0x00]))
        + _field(_IA_TYPE_ASCII, b'00')
        + _field(_IA_TYPE_BINARY, bytes(4)) * 3
        + bytes([_IA_EOF])
    )
    units = (6 + len(fields) + 1 + 7) // 8
    record = bytearray(bytes([0x00, units, 0x00, 0x00, 0x00, 0x00]) + fields)
    record += bytes(units * 8 - 1 - len(record))
    record.append((-sum(record)) & 0xFF)
    return bytes(record)


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


def launch_args(assets, cpus, scratch):
    """The -M property string and the remaining argv fragment for a real
    Origin 300 stock-PROM boot with cpus processors fitted.

    assets is QEMU_TEST_SGI_ASSET_DIR; the caller's own require_assets(self,
    *ASSET_SPECS) call should already have resolved and skip/failed on
    every asset this needs. scratch is a fresh, writable, per-test
    directory (self.workdir): the boot flash and the synthesized node and
    IO8 identity records are built there, never into the asset tree.

    Returns (machine, argv). origin300's machine name accepts an
    EEPROM property only in a comma-bearing string, which
    self.set_machine() cannot validate (it checks the bare name against
    query-machines), so the caller applies machine with
    self.vm.set_machine() after self.set_machine('origin300') validates
    the bare name, then passes argv to self.vm.add_args() -- the same
    idiom test_sn0_port_reset.py and test_sn0_elsc_reset.py use for
    origin200,debug-leds=off.
    """
    firmware_path = resolve_asset(assets, FIRMWARE_REL, FIRMWARE_SHA256)
    if firmware_path is None:
        raise ValueError(f'missing or mismatched asset: {FIRMWARE_REL}')

    boot_flash = os.path.join(scratch, 'ip35-boot-flash.raw')
    _build_boot_flash(firmware_path, cpus, boot_flash)

    node_chassis_path = os.path.join(scratch,
                                     'origin300-node-chassis-eeprom.bin')
    _write(node_chassis_path, _build_node_chassis_eeprom_record())

    node_board_path = os.path.join(scratch, 'origin300-node-board-eeprom.bin')
    _write(node_board_path, _build_node_board_eeprom_record(cpus))

    chassis_path = os.path.join(scratch, 'origin300-io8-chassis-eeprom.bin')
    _write(chassis_path, _build_io8_chassis_eeprom_record())

    board_path = os.path.join(scratch, 'origin300-io8-board-eeprom.bin')
    _write(board_path, _build_io8_board_eeprom_record(NETWORK_MAC))

    properties = [
        f'chassis-eeprom.0={node_chassis_path}',
        f'chassis-eeprom.1={chassis_path}',
        f'board-eeprom.0={node_board_path}',
        f'board-eeprom.1={board_path}',
        'spd-eeprom.0=',
        'spd-eeprom.1=',
        'spd-eeprom.2=',
        'spd-eeprom.4=',
    ]
    for prop, rel, sha256 in SPD_INPUTS:
        path = resolve_asset(assets, rel, sha256)
        if path is None:
            raise ValueError(f'missing or mismatched asset: {rel}')
        properties.append(f'{prop}={path}')

    machine = 'origin300,' + ','.join(properties)
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


def _next_mac(mac, step):
    value = (int(mac.replace(':', ''), 16) + step) & ((1 << 48) - 1)
    return ':'.join(f'{(value >> shift) & 0xff:02x}'
                    for shift in range(40, -8, -8))


def launch_args_two_chassis(assets, populations, scratch):
    """The -M property string and argv fragment for the two-chassis
    Origin 300 (topology=origin300-two-chassis), each chassis with its
    own boot flash seeded for its own processor count and its own
    identity records.

    populations is the per-node processor counts in node order, (4, 4)
    or (2, 2) and so on. QEMU splits each identity-record list evenly
    across the two L1s in node order, so node 1's chassis and board
    records take indices 2 and 3 and its SPD records 6 to 11; its
    generated node board record names the board its own processor count
    fits, and its generated IO8 board record carries the MAC one past
    node 0's, the same steps tools/origami_lib/prom.py takes. Node 1's L1 console is
    serial line 5, left unconnected here; the master's is line 0.
    """
    machine, argv = launch_args(assets, populations[0], scratch)
    node_dir = os.path.join(scratch, 'node1')
    os.makedirs(node_dir, exist_ok=True)
    firmware_path = resolve_asset(assets, FIRMWARE_REL, FIRMWARE_SHA256)
    boot_flash = os.path.join(node_dir, 'ip35-boot-flash.raw')
    _build_boot_flash(firmware_path, populations[1], boot_flash)
    node_board_path = os.path.join(node_dir,
                                   'origin300-node-board-eeprom.bin')
    _write(node_board_path, _build_node_board_eeprom_record(populations[1]))
    board_path = os.path.join(node_dir, 'origin300-io8-board-eeprom.bin')
    _write(board_path, _build_io8_board_eeprom_record(
        _next_mac(NETWORK_MAC, 1)))

    extra = []
    for option in machine.split(',')[1:]:
        key, _sep, value = option.partition('=')
        array, _dot, index = key.partition('.')
        stride = 6 if array == 'spd-eeprom' else 2
        if array == 'board-eeprom' and index == '0':
            value = node_board_path
        if array == 'board-eeprom' and index == '1':
            value = board_path
        extra.append(f'{array}.{int(index) + stride}={value}')
    machine = ','.join([machine, *extra,
                        'topology=origin300-two-chassis',
                        'population=' + ':'.join(map(str, populations))])
    argv[argv.index('-m') + 1] = '1024M'
    argv[argv.index('-smp') + 1] = str(sum(populations))
    argv += ['-drive', f'if=pflash,index=1,file={boot_flash},format=raw']
    return machine, argv
