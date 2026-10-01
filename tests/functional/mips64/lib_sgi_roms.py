# SPDX-License-Identifier: BSD-3-Clause
"""Parse the native SGI bare-metal ROM test matrix."""

from dataclasses import dataclass
from pathlib import Path
import re


CONFIGS = ('default', '4cpu-2node', 'icount')
CPUS = ('R4000', 'R10000', 'R14000',
        'R16000-4m', 'R16000-8m', 'R16000-16m')


@dataclass(frozen=True)
class Case:
    rom: str
    cpu: str
    config: str

    @property
    def identity(self):
        config = self.config.replace('-', '_')
        cpu = self.cpu.replace('-', '_')
        return f'SGIRoms.test_{self.rom}_{cpu}_{config}'


def read_suite(path):
    """Return all cases in a suite, rejecting malformed or duplicate rows."""
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError as exc:
        raise ValueError(f'cannot read ROM suite {path}: {exc}') from exc

    cases = []
    seen_rows = set()
    seen_cases = set()
    for line_number, line in enumerate(lines, 1):
        fields = line.split('#', 1)[0].split()
        if not fields:
            continue
        rom, *options = fields
        if not re.fullmatch(r'[a-z][a-z0-9_]*', rom):
            raise ValueError(f'invalid ROM name on row {line_number}: {line}')
        if rom in seen_rows:
            raise ValueError(f'duplicate ROM row {rom!r} on row {line_number}')
        seen_rows.add(rom)

        cpus = []
        configs = []
        timed_count = 0
        for option in options:
            if option in CPUS:
                cpus.append(option)
            elif option.startswith('config:'):
                configs.append(option[len('config:'):])
            elif option == 'timed':
                timed_count += 1
            else:
                raise ValueError(
                    f'invalid ROM option {option!r} on row {line_number}')
        if (not cpus or len(cpus) != len(set(cpus)) or
                len(configs) > 1 or
                (configs and configs[0] not in CONFIGS[1:]) or
                timed_count > 1 or (timed_count and not configs)):
            raise ValueError(f'invalid ROM suite row {line_number}: {line}')
        config = configs[0] if configs else 'default'
        for cpu in cpus:
            row_cases = [Case(rom, cpu, config)]
            # Timed rows also require the single-CPU throughput control.
            if timed_count:
                row_cases.append(Case(rom, cpu, 'default'))
            for case in row_cases:
                if case.identity in seen_cases:
                    raise ValueError(
                        f'duplicate ROM testcase identity: {case.identity}')
                seen_cases.add(case.identity)
                cases.append(case)
    if not cases:
        raise ValueError(f'ROM suite {path} contains no cases')
    return cases
