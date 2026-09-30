#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require successful SGI test results in a Meson JUnit report."""

import argparse
import importlib.util
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


_ROM_PARSER_PATH = (Path(__file__).parents[1] /
                    'tests/functional/mips64/lib_sgi_roms.py')
_ROM_PARSER_SPEC = importlib.util.spec_from_file_location(
    'qemu_sgi_rom_suite', _ROM_PARSER_PATH)
_ROM_PARSER = importlib.util.module_from_spec(_ROM_PARSER_SPEC)
sys.modules[_ROM_PARSER_SPEC.name] = _ROM_PARSER
_ROM_PARSER_SPEC.loader.exec_module(_ROM_PARSER)

# This cross-version migration case needs a separately supplied v9 QEMU.
# Its explicit skip is expected when that reference binary is unavailable.
_OPTIONAL_SKIP = ('qtest-mips64/sn0-origin2000-mgras-test',
                  '/mips64/sn0/origin2000/mgras/migration-from-v9-format-default')


class ResultError(Exception):
    pass


@dataclass
class JUnitReport:
    cases: dict
    suites: dict


def _testcase_name(name):
    """Remove Meson's TAP counter and QEMU's script-name prefix."""
    normalized = re.sub(r'^\d+\s+', '', name)
    return re.sub(r'^test_[^.]+\.', '', normalized)


def _suite_name(name):
    """Keep the selected Meson test name, dropping setup-generated prefixes."""
    return name.rsplit(' / ', 1)[-1]


def read_expected(path):
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError as exc:
        raise ResultError(
            f"cannot read expected identities {path}: {exc}") from exc

    expected = [line.strip() for line in lines
                if line.strip() and not line.lstrip().startswith('#')]
    if not expected:
        raise ResultError(f"expected identity file {path} is empty")
    duplicates = sorted(
        identity for identity, count in Counter(expected).items() if count > 1)
    if duplicates:
        raise ResultError(
            'duplicate expected identities: ' + ', '.join(duplicates))
    return expected


def read_skip_allowlist(path):
    """Read exact Meson suite and normalized testcase identities."""
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError as exc:
        raise ResultError(
            f"cannot read skipped identity allowlist {path}: {exc}") from exc

    identities = []
    for number, line in enumerate(lines, 1):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) != 2 or not all(fields):
            raise ResultError(
                f"invalid skipped identity on line {number} in {path}")
        identities.append(tuple(fields))
    if not identities:
        raise ResultError(f"skipped identity allowlist {path} is empty")
    duplicates = sorted(
        identity for identity, count in Counter(identities).items()
        if count > 1)
    if duplicates:
        raise ResultError(
            'duplicate skipped identities: ' + ', '.join(
                f'{suite}::{case}' for suite, case in duplicates))
    return set(identities)


def _count_attribute(element, attribute, actual, context):
    value = element.get(attribute)
    if value is None:
        return
    try:
        declared = int(value)
    except ValueError as exc:
        raise ResultError(
            f"{context} has invalid {attribute} count {value!r}") from exc
    if declared != actual:
        raise ResultError(
            f"{context} declares {declared} {attribute} but contains {actual}")


def read_report(path, optional_skips=()):
    if not path.is_file():
        raise ResultError(f"JUnit report does not exist: {path}")
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        raise ResultError(f"cannot parse JUnit report {path}: {exc}") from exc

    if root.tag not in ('testsuite', 'testsuites'):
        raise ResultError(f"unexpected JUnit root element {root.tag!r}")
    cases = root.findall('.//testcase')
    if not cases:
        raise ResultError(f"JUnit report contains no testcases: {path}")

    for suite in root.iter('testsuite'):
        suite_cases = suite.findall('.//testcase')
        label = suite.get('name', '<unnamed testsuite>')
        _count_attribute(suite, 'tests', len(suite_cases), label)
        for kind, child in (('failures', 'failure'),
                            ('errors', 'error'), ('skipped', 'skipped')):
            actual = sum(len(case.findall(child)) for case in suite_cases)
            _count_attribute(suite, kind, actual, label)
    if root.tag == 'testsuites':
        _count_attribute(root, 'tests', len(cases), 'testsuites')
        for kind, child in (('failures', 'failure'),
                            ('errors', 'error'), ('skipped', 'skipped')):
            actual = sum(len(case.findall(child)) for case in cases)
            _count_attribute(root, kind, actual, 'testsuites')

    found = {}
    suites = {}
    failures = []
    skips = []
    allowed_skips = {_OPTIONAL_SKIP, *optional_skips}
    for case in cases:
        name = case.get('name')
        if not name:
            raise ResultError('JUnit testcase is missing its name')
        classname = case.get('classname')
        identity = f'{classname}::{name}' if classname else name
        for key in {name, _testcase_name(name), _suite_name(name),
                    identity}:
            found.setdefault(key, []).append(case)
        skip_identity = (_suite_name(classname or ''), _testcase_name(name))
        if case.findall('skipped') and skip_identity not in allowed_skips:
            skips.append(identity)
        if case.findall('failure') or case.findall('error'):
            failures.append(identity)

    for suite in root.iter('testsuite'):
        name = suite.get('name')
        if name:
            cases = suite.findall('.//testcase')
            suites.setdefault(name, []).extend(cases)
            suites.setdefault(_suite_name(name), []).extend(cases)

    if skips:
        raise ResultError(
            'JUnit report contains skipped testcases: ' + ', '.join(skips))
    if failures:
        raise ResultError(
            'JUnit report contains failed testcases: ' + ', '.join(failures))
    return JUnitReport(found, suites)


def validate(report, expected):
    missing = []
    ambiguous = []
    for identity in expected:
        cases = report.cases.get(identity, [])
        suites = report.suites.get(identity, [])
        if not cases and suites:
            cases = suites
        if not cases:
            missing.append(identity)
        elif len(cases) != 1 and not suites:
            ambiguous.append(identity)
    if missing:
        raise ResultError('expected testcases or suites are missing: '
                          + ', '.join(missing))
    if ambiguous:
        raise ResultError('expected testcase identities are ambiguous: '
                          + ', '.join(ambiguous))


def rom_identities(path):
    try:
        return [case.identity for case in _ROM_PARSER.read_suite(path)]
    except ValueError as exc:
        raise ResultError(str(exc)) from exc


def meson_test_identities(build_dir, prefix=None, suites=None,
                          excluded_suites=None, names=None):
    meson = build_dir / 'pyvenv' / 'bin' / 'meson'
    try:
        output = subprocess.check_output(
            [str(meson), 'introspect', '--tests', str(build_dir)], text=True)
        tests = json.loads(output)
    except (OSError, subprocess.CalledProcessError,
            json.JSONDecodeError) as exc:
        raise ResultError(
            f"cannot read Meson tests from {build_dir}: {exc}") from exc
    requested_names = set(names or [])
    include_suites = {_normalise_meson_suite(suite) for suite in suites or []}
    exclude_suites = {_normalise_meson_suite(suite)
                      for suite in excluded_suites or []}
    registered_names = set()
    selected = set()
    for item in tests:
        if not isinstance(item, dict) or not isinstance(item.get('name'), str):
            continue
        name = item['name']
        registered_names.add(name)
        if prefix and not name.startswith(prefix):
            continue
        if requested_names and name not in requested_names:
            continue
        item_suites = {_normalise_meson_suite(suite)
                       for suite in item.get('suite', [])
                       if isinstance(suite, str)}
        if include_suites and not item_suites.intersection(include_suites):
            continue
        if item_suites.intersection(exclude_suites):
            continue
        selected.add(name)

    missing = sorted(requested_names - registered_names)
    if missing:
        raise ResultError('Meson has not registered requested tests: '
                          + ', '.join(missing))
    if not selected:
        raise ResultError(
            'Meson registered no tests for the requested selection')
    return sorted(selected)


def _normalise_meson_suite(suite):
    """Compare Meson suite labels with or without the project prefix."""
    return suite.split(':', 1)[-1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--junit', type=Path,
                        help='Meson JUnit XML for this invocation')
    mode.add_argument('--list-rom-identities', type=Path,
                      help='print required identities from a ROM suite')
    mode.add_argument('--list-meson-tests', type=Path,
                      help='print registered Meson test identities')
    parser.add_argument('--prefix',
                        help='required name prefix with --list-meson-tests')
    parser.add_argument('--suite', action='append', default=[],
                        help='include a Meson suite (repeatable)')
    parser.add_argument('--exclude-suite', action='append', default=[],
                        help='exclude a Meson suite (repeatable)')
    parser.add_argument('--names-from', type=Path,
                        help='select exact Meson test names from a file')
    parser.add_argument('--require-rom-target', type=Path,
                        metavar='BUILD_DIR',
                        help='require func-mips64-sgi_roms registration when '
                             'listing ROM identities')
    parser.add_argument('--expect', type=Path,
                        help='UTF-8 file with one required identity per line')
    parser.add_argument('--allow-skips-from', type=Path,
                        help='exact optional skip identities for this run')
    args = parser.parse_args(argv)

    try:
        if args.list_rom_identities is not None:
            if (args.expect or args.prefix or args.suite or
                    args.exclude_suite or args.names_from or
                    args.allow_skips_from):
                parser.error('Meson selection options do not apply to '
                             '--list-rom-identities')
            if args.require_rom_target is not None:
                meson_test_identities(
                    args.require_rom_target,
                    names=['func-mips64-sgi_roms'])
            print('\n'.join(rom_identities(args.list_rom_identities)))
            return 0
        if args.list_meson_tests is not None:
            if (args.expect or args.require_rom_target or
                    args.allow_skips_from):
                parser.error('--expect and --require-rom-target do not apply '
                             'to --list-meson-tests')
            names = read_expected(args.names_from) if args.names_from else None
            if not (args.prefix or args.suite or args.exclude_suite or names):
                parser.error('--list-meson-tests requires a prefix, '
                             'suite, or names file')
            print('\n'.join(meson_test_identities(
                args.list_meson_tests, args.prefix, args.suite,
                args.exclude_suite, names)))
            return 0
        if (args.expect is None or args.prefix or args.suite or
                args.exclude_suite or args.names_from or
                args.require_rom_target):
            parser.error('Meson selection options do not apply to --junit')
        expected = read_expected(args.expect)
        optional_skips = (read_skip_allowlist(args.allow_skips_from)
                          if args.allow_skips_from else ())
        report = read_report(args.junit, optional_skips)
        validate(report, expected)
    except ResultError as exc:
        print(f'check-sgi-test-results: {exc}', file=sys.stderr)
        return 1

    print(f'checked {len(expected)} required testcase or suite identity(s) '
          f'in {args.junit}')
    for identity in expected:
        print(f'PASS {identity}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
