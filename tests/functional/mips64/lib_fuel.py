# Supplied inputs for the Fuel installed-IRIX functional test
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Use an identified Fuel fixture without manufacturing board defaults."""

import json
import os
import re
import shutil
from pathlib import PurePosixPath

from lib_prom import require_assets


MANIFEST_REL = 'assets/functional/fuel-irix-6.5.30.json'
_REQUIRED_PROPERTIES = {
    'fuel-board-id-word', 'fuel-ioc3-subsystem-id',
    'fuel-l1-type-code', 'fuel-bedrock-revision',
}
_OPTIONAL_PROPERTIES = {'fuel-l1-revision', 'fuel-rtc-decode'}
_REQUIRED_PAYLOADS = {'prom', 'boot_flash', 'disk', 'fuel-mac-eeprom'}
_IDENTITY_PAYLOADS = {'chassis-eeprom.0', 'board-eeprom.0'}
_FIELDS = {
    'schema', 'provenance', 'experimental_inputs', 'console',
    'guest_platform', 'machine_properties', 'payloads',
}


def load_fixture(test):
    """Validate the fixed local manifest and every identified payload."""
    manifest_path = require_assets(test, MANIFEST_REL)[MANIFEST_REL]
    with open(manifest_path, encoding='utf-8') as source:
        fixture = json.load(source)
    if not isinstance(fixture, dict) or set(fixture) != _FIELDS:
        raise ValueError('Fuel fixture has missing or unknown fields')
    if type(fixture['schema']) is not int or fixture['schema'] != 1:
        raise ValueError('Fuel fixture requires schema 1')
    if fixture['experimental_inputs'] is not True:
        raise ValueError('Fuel fixture must identify experimental supplied inputs')
    if (not isinstance(fixture['provenance'], str)
            or not fixture['provenance'].strip()):
        raise ValueError('Fuel fixture requires an input provenance description')
    if fixture['console'] != 'ioc3-a' or fixture['guest_platform'] != 'IP35':
        raise ValueError('Fuel fixture requires IOC3-A and an IP35 guest')

    properties = fixture['machine_properties']
    if (not isinstance(properties, dict)
            or not _REQUIRED_PROPERTIES <= set(properties)
            or set(properties) - _REQUIRED_PROPERTIES - _OPTIONAL_PROPERTIES):
        raise ValueError('Fuel fixture has missing or unknown machine properties')
    for key, value in properties.items():
        if (not isinstance(value, str) or not value
                or re.search(r'[,\s]', value)):
            raise ValueError(f'Fuel fixture has an unsafe value for {key}')

    payloads = fixture['payloads']
    if (not isinstance(payloads, dict)
            or not _REQUIRED_PAYLOADS <= set(payloads)
            or set(payloads) - _REQUIRED_PAYLOADS - _IDENTITY_PAYLOADS):
        raise ValueError('Fuel fixture has missing or unknown payloads')
    specs = []
    for name, spec in payloads.items():
        if not isinstance(spec, dict) or set(spec) != {'path', 'sha256'}:
            raise ValueError(f'Fuel fixture has an invalid payload: {name}')
        rel = spec['path']
        if (not isinstance(rel, str) or not rel.startswith('assets/')
                or '..' in PurePosixPath(rel).parts
                or re.search(r'[,\r\n]', rel)):
            raise ValueError(f'Fuel fixture has an unsafe payload path: {name}')
        digest = spec['sha256']
        if (not isinstance(digest, str)
                or re.fullmatch(r'[0-9a-f]{64}', digest) is None):
            raise ValueError(f'Fuel fixture requires SHA-256 for {name}')
        specs.append((rel, digest))
    assets = require_assets(test, *specs)
    files = {name: assets[spec['path']] for name, spec in payloads.items()}
    files['manifest'] = manifest_path
    if os.path.getsize(files['boot_flash']) != 16 * 1024 * 1024:
        raise ValueError('Fuel fixture boot flash must be exactly 16 MiB')
    return fixture, files


def launch_args(fixture, files, scratch):
    """Copy the supplied flash before constructing the Fuel launch."""
    private_flash = os.path.join(scratch, 'fuel-boot-flash.raw')
    shutil.copyfile(files['boot_flash'], private_flash)
    properties = [
        f'{key}={value}'
        for key, value in sorted(fixture['machine_properties'].items())
    ]
    for key in sorted({'fuel-mac-eeprom', *_IDENTITY_PAYLOADS} & set(files)):
        properties.append(f'{key}={files[key]}')
    machine = 'fuel,' + ','.join(properties)
    argv = [
        '-cpu', 'R14000', '-smp', '1', '-m', '512M',
        '-accel', 'tcg,thread=multi',
        '-bios', files['prom'],
        '-drive', f'if=pflash,index=0,file={private_flash},format=raw',
        '-drive',
        f'if=none,id=disk0,file={files["disk"]},format=qcow2,snapshot=on',
        '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
        '-nic', 'user,id=net0',
        '-audio', 'none',
    ]
    return machine, argv, private_flash
