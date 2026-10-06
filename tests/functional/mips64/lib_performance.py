# SPDX-License-Identifier: BSD-3-Clause
"""Host measurements for correctness-checked performance workloads."""

from collections.abc import Mapping
import math
import os
import re
import statistics
import subprocess
import sys
import time


def _parse_proc_stat(text, ticks):
    # comm may contain spaces and parentheses, so field 3 follows the last ')'.
    fields = text[text.rfind(')') + 1:].split()
    try:
        user, system = int(fields[11]), int(fields[12])
    except (IndexError, ValueError) as error:
        raise ValueError('invalid process CPU accounting') from error
    if ')' not in text or ticks <= 0 or user < 0 or system < 0:
        raise ValueError('invalid process CPU accounting')
    return (user + system) / ticks


def _parse_ps_time(text):
    match = re.fullmatch(r'(?:(\d+)-)?(\d+):(\d{2})(?::(\d{2}))?'
                         r'(\.\d+)?', text.strip())
    if match is None:
        raise ValueError('invalid ps CPU time')
    days, first, second, third, fraction = match.groups()
    first, second = int(first), int(second)
    if second >= 60 or (third is not None and int(third) >= 60):
        raise ValueError('invalid ps CPU time')
    if days is not None and (third is None or first >= 24):
        raise ValueError('invalid ps CPU time')
    seconds = first * 60 + second
    if third is not None:
        seconds = first * 3600 + second * 60 + int(third)
    return int(days or 0) * 86400 + seconds + float(fraction or 0)


def process_cpu_seconds(pid):
    """Process user plus system CPU seconds, or None when unavailable."""
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise ValueError('pid must be a positive integer')
    if sys.platform == 'linux':
        try:
            with open(f'/proc/{pid}/stat', encoding='utf-8') as stat:
                return _parse_proc_stat(stat.read(), os.sysconf('SC_CLK_TCK'))
        except OSError:
            return None
    if sys.platform == 'darwin':
        try:
            result = subprocess.run(['ps', '-p', str(pid), '-o', 'time='],
                                    check=True, capture_output=True, text=True,
                                    timeout=5)
        except (OSError, subprocess.SubprocessError):
            return None
        return _parse_ps_time(result.stdout)
    return None


def _number(value, name, positive=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f'{name} must be a finite number')
    if not math.isfinite(value) or value < 0 or (positive and value == 0):
        raise ValueError(f'invalid {name}: {value!r}')
    return value


def _correct(result):
    return result is True or (isinstance(result, Mapping)
                              and result.get('passed') is True)


def measure(pid, operation, units, unit):
    """Measure checked work. Failed checks and operation exceptions propagate.

    CPU accounting unavailable at both endpoints produces None. Losing
    accounting during an operation invalidates the sample. Warmup operations
    belong to the caller and must be kept out of the measured sample list.
    """
    _number(units, 'units', positive=True)
    if not isinstance(unit, str) or not unit.strip():
        raise ValueError('unit must be a nonempty string')
    cpu_before = process_cpu_seconds(pid)
    started = time.monotonic()
    result = operation()
    wall = time.monotonic() - started
    cpu_after = process_cpu_seconds(pid)
    if not _correct(result):
        raise ValueError('operation did not pass its correctness check')
    _number(wall, 'wall seconds', positive=True)
    if (cpu_before is None) != (cpu_after is None):
        raise ValueError('process CPU accounting disappeared during '
                         'measurement')
    cpu = None
    if cpu_before is not None:
        _number(cpu_before, 'initial CPU seconds')
        _number(cpu_after, 'final CPU seconds')
        cpu = _number(cpu_after - cpu_before, 'CPU seconds')
    # The interval's host monotonic ends let a profile taken alongside the
    # run select the samples that fall inside measured work.
    return {'status': 'passed', 'result': result, 'units': units, 'unit': unit,
            'started_monotonic': started,
            'ended_monotonic': started + wall,
            'wall_seconds': wall, 'cpu_seconds': cpu,
            'units_per_second': units / wall,
            'cpu_seconds_per_unit': None if cpu is None else cpu / units}


def _statistics(values):
    return {'median': statistics.median(values),
            'mean': statistics.mean(values),
            'stdev': statistics.stdev(values) if len(values) > 1 else 0.0,
            'min': min(values), 'max': max(values)}


def summarize(samples):
    """Raw runs and wall/CPU sample statistics for identical checked work."""
    samples = list(samples)
    if not samples:
        raise ValueError('at least one sample is required')
    walls, cpus = [], []
    required = {'status', 'result', 'units', 'unit', 'wall_seconds',
                'cpu_seconds', 'units_per_second', 'cpu_seconds_per_unit'}
    for sample in samples:
        if not isinstance(sample, Mapping) or not required <= sample.keys():
            raise ValueError('incomplete performance sample')
        if sample['status'] != 'passed' or not _correct(sample['result']):
            raise ValueError('performance sample did not pass')
        units = _number(sample['units'], 'units', positive=True)
        unit = sample['unit']
        if not isinstance(unit, str) or not unit.strip():
            raise ValueError('unit must be a nonempty string')
        if (units, unit) != (samples[0]['units'], samples[0]['unit']):
            raise ValueError('samples contain different work')
        wall = _number(sample['wall_seconds'], 'wall seconds', positive=True)
        cpu = sample['cpu_seconds']
        if cpu is not None:
            _number(cpu, 'CPU seconds')
        if (cpu is None) != (samples[0]['cpu_seconds'] is None):
            raise ValueError('samples have inconsistent CPU accounting')
        if sample['units_per_second'] != units / wall:
            raise ValueError('sample throughput does not match measured work')
        cpu_per_unit = None if cpu is None else cpu / units
        if sample['cpu_seconds_per_unit'] != cpu_per_unit:
            raise ValueError('sample CPU cost does not match measured work')
        walls.append(wall)
        cpus.append(cpu)
    return {'count': len(samples), 'samples': samples,
            'wall_seconds': _statistics(walls),
            'cpu_seconds': None if cpus[0] is None else _statistics(cpus)}


def compare_results(base, candidate):
    """Median candidate/base ratios for matching workload configurations.

    The full configuration must match. Revision identities belong outside it.
    Two checked runs are required on each side. A zero baseline CPU median
    cannot support a CPU ratio and is rejected.
    """
    summaries = []
    configs = []
    for report in (base, candidate):
        if not isinstance(report, Mapping):
            raise ValueError('incomplete workload report')
        if (not isinstance(report.get('workload'), str)
                or not report['workload']):
            raise ValueError('workload identity is required')
        config = report.get('config')
        if not isinstance(config, Mapping) or not config:
            raise ValueError('workload configuration is required')
        summary = report.get('summary')
        if not isinstance(summary, Mapping) or 'samples' not in summary:
            raise ValueError('workload summary is required')
        checked = summarize(summary['samples'])
        if summary != checked or checked['count'] < 2:
            raise ValueError('comparison requires complete repeated '
                             'measurements')
        summaries.append(checked)
        configs.append(config)
    if base['workload'] != candidate['workload'] or configs[0] != configs[1]:
        raise ValueError('workload or configuration changed')
    first, second = (summary['samples'][0] for summary in summaries)
    if (first['units'], first['unit']) != (second['units'], second['unit']):
        raise ValueError('measured work changed')
    base_cpu, candidate_cpu = (summary['cpu_seconds'] for summary in summaries)
    if (base_cpu is None) != (candidate_cpu is None):
        raise ValueError('CPU accounting availability changed')
    cpu_ratio = None
    if base_cpu is not None:
        if base_cpu['median'] == 0:
            raise ValueError('baseline CPU median is zero and unmeasurable')
        cpu_ratio = candidate_cpu['median'] / base_cpu['median']
    return {'workload': base['workload'],
            'wall_ratio': summaries[1]['wall_seconds']['median']
                          / summaries[0]['wall_seconds']['median'],
            'cpu_ratio': cpu_ratio}


def _artifact_identity(record, name):
    if not isinstance(record, Mapping):
        raise ValueError(f'{name} artifact identity is required')
    digest, size = record.get('sha256'), record.get('size_bytes')
    if (not isinstance(digest, str)
            or re.fullmatch(r'[0-9a-fA-F]{64}', digest) is None):
        raise ValueError(f'{name} requires a SHA256 digest')
    if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
        raise ValueError(f'{name} requires a positive artifact size')
    return digest.lower(), size


def _checked_report(report):
    if (not isinstance(report, Mapping)
            or type(report.get('schema')) is not int
            or report['schema'] != 1 or report.get('status') != 'passed'):
        raise ValueError('comparison requires a successful schema 1 report')
    identity = report.get('identity')
    if not isinstance(identity, Mapping):
        raise ValueError('report identity is required')
    fork = identity.get('fork')
    if (not isinstance(fork, Mapping)
            or not isinstance(fork.get('commit'), str)
            or not fork['commit'].strip()
            or not isinstance(fork.get('dirty'), bool)):
        raise ValueError('source commit and known dirty state are required')
    _artifact_identity(identity.get('qemu_binary'), 'QEMU binary')
    payload = identity.get('payload')
    if not isinstance(payload, Mapping):
        raise ValueError('input artifact identities are required')
    inputs = {name: _artifact_identity(payload.get(name), name)
              for name in ('prom', 'disk')}
    workloads = report.get('workloads')
    if not isinstance(workloads, list) or not workloads:
        raise ValueError('report requires a nonempty workload list')
    results = {}
    for workload in workloads:
        if not isinstance(workload, Mapping):
            raise ValueError('workload must be a mapping')
        name = workload.get('workload')
        if (not isinstance(name, str) or not name.strip() or name in results
                or workload.get('status') != 'passed'):
            raise ValueError('workloads must have unique names and pass')
        config = workload.get('config')
        if not isinstance(config, Mapping):
            raise ValueError('workload configuration is required')
        for artifact, key in (('prom', 'firmware_sha256'),
                              ('disk', 'disk_sha256')):
            declared = config.get(key)
            if (not isinstance(declared, str)
                    or declared.lower() != inputs[artifact][0]):
                raise ValueError(f'{name} configuration differs from actual '
                                 f'{artifact} SHA256')
        summary = workload.get('summary')
        if (not isinstance(summary, Mapping)
                or not isinstance(summary.get('samples'), list)):
            raise ValueError('workload requires a summary with a sample list')
        results[name] = workload
    return results, inputs


def compare_reports(base, candidate):
    """Compare complete reports whose measured inputs match their records.

    Source commits and emulator binaries may differ outside configuration.
    A dirty source record identifies its base commit and dirty state only;
    it does not identify the changed source contents.
    """
    baseline, base_inputs = _checked_report(base)
    proposed, candidate_inputs = _checked_report(candidate)
    if baseline.keys() != proposed.keys():
        raise ValueError('workload sets differ')
    if base_inputs != candidate_inputs:
        raise ValueError('actual input artifacts differ')
    return {'schema': 1,
            'ratios': [compare_results(workload, proposed[name])
                       for name, workload in baseline.items()]}
