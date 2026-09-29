# Pin a running QEMU's vCPU threads one-to-one to host CPUs
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Under MTTCG each guest processor is a host thread, and QEMU_CLOCK_VIRTUAL
follows the host clock. A guest that times one device against its own
CP0 Count -- sMDK's io8bt checks the IOC3 serial receive timer against a
10 ms Count busy wait -- therefore sees the window stretch whenever its
vCPU thread waits for a host core, and a busy host fails a check the
model passes on an idle one. Pinning each vCPU thread to a host CPU of
its own, and every other QEMU thread to one more, is the condition such
a diagnostic's verdict is accepted under.

The CPUs come from QEMU_TEST_HOST_CPUS when it is set (a list such as
"8-15" or "4,6,8-11"), otherwise from the top of this process's own
allowed set, the end a desktop session is least likely to be using. When
there are fewer CPUs than threads to place, the vCPUs share them round
robin and pin() says so; each thread is still held to one CPU. Tests run
in parallel each choose from the top of the same set, so two pinned runs
at once share cores; give each its own QEMU_TEST_HOST_CPUS to keep them
apart.

Plain Python with no qemu_test dependency, the same rule lib_prom.py
follows: test_lib_host_cpus.py exercises it under a bare python3.
"""

import os
import re
import time

from lib_net import HarnessError


ENV = 'QEMU_TEST_HOST_CPUS'

# The name MTTCG gives each vCPU thread (accel/tcg/tcg-accel-ops-mttcg.c).
# Round-robin TCG runs every vCPU on one "ALL CPUs/TCG" thread, which has
# nothing to pin one-to-one.
VCPU_THREAD = re.compile(r'^CPU (\d+)/TCG$')

# vCPU threads start while QEMU initialises; a launch that has returned
# has created them, but a short wait keeps a slow start from reading as
# a missing processor.
THREAD_WAIT_SECONDS = 10.0
THREAD_POLL_SECONDS = 0.1


def parse_cpu_list(text):
    """"0-3,8" as [0, 1, 2, 3, 8], in the order written, duplicates
    dropped. Raises HarnessError on anything else."""
    cpus = []
    for part in text.split(','):
        part = part.strip()
        match = re.fullmatch(r'(\d+)(?:-(\d+))?', part)
        if match is None:
            raise HarnessError(f'{ENV}: cannot read {part!r} in {text!r}')
        first = int(match.group(1))
        last = int(match.group(2)) if match.group(2) else first
        if last < first:
            raise HarnessError(f'{ENV}: range {part!r} runs backwards')
        for cpu in range(first, last + 1):
            if cpu not in cpus:
                cpus.append(cpu)
    return cpus


def choose_cpus(count, allowed, environ=None):
    """Up to count host CPUs to pin to, in the order to use them: the
    environment's list, which must lie inside allowed, or else allowed
    from its highest CPU down."""
    environ = os.environ if environ is None else environ
    if environ.get(ENV):
        cpus = parse_cpu_list(environ[ENV])
        outside = sorted(set(cpus) - set(allowed))
        if outside:
            raise HarnessError(f'{ENV} names CPUs {outside} that this '
                               f'process may not run on')
    else:
        cpus = sorted(allowed, reverse=True)
    return cpus[:count]


def threads(pid, proc='/proc'):
    """{tid: name} for every thread of pid."""
    task = os.path.join(proc, str(pid), 'task')
    found = {}
    for entry in os.listdir(task):
        try:
            with open(os.path.join(task, entry, 'comm'),
                      encoding='utf-8') as comm:
                found[int(entry)] = comm.read().rstrip('\n')
        except FileNotFoundError:
            continue  # the thread exited between listdir and open
    return found


def vcpu_threads(pid, vcpus, proc='/proc', sleep=time.sleep,
                 clock=time.monotonic, timeout=THREAD_WAIT_SECONDS):
    """({index: tid} for the vCPU threads, {tid: name} for all threads),
    once all vcpus are present. Raises HarnessError if they never are."""
    deadline = clock() + timeout
    while True:
        named = threads(pid, proc)
        found = {}
        for tid, name in named.items():
            match = VCPU_THREAD.match(name)
            if match:
                found[int(match.group(1))] = tid
        if len(found) >= vcpus:
            return found, named
        if clock() >= deadline:
            raise HarnessError(
                f'QEMU {pid} has {len(found)} "CPU n/TCG" threads, not '
                f'{vcpus}; pinning needs MTTCG (threads: '
                f'{sorted(set(named.values()))})')
        sleep(THREAD_POLL_SECONDS)


def plan(vcpu_tids, others, cpus):
    """{tid: cpu}: vCPU n to cpus[n], round robin once cpus run out, and
    every other thread to the CPU after the vCPUs'."""
    if not cpus:
        raise HarnessError('no host CPU to pin to')
    placement = {}
    order = sorted(vcpu_tids)
    for slot, index in enumerate(order):
        placement[vcpu_tids[index]] = cpus[slot % len(cpus)]
    rest = cpus[len(order) % len(cpus)]
    for tid in others:
        placement[tid] = rest
    return placement


def pin(pid, vcpus, log, proc='/proc', environ=None, allowed=None,
        setaffinity=os.sched_setaffinity, getaffinity=os.sched_getaffinity,
        **wait):
    """Pin QEMU pid's vcpus vCPU threads one-to-one to host CPUs and its
    other threads to one more. Logs each thread's affinity as read back
    from the kernel through log (a logging.Logger) and returns
    {tid: (name, cpu)}. Raises HarnessError if a read-back differs.

    Threads QEMU starts afterwards inherit their creator's CPU; most are
    started by the main thread, so they land beside it. Run this before
    the guest is released from -S so the whole run is pinned."""
    if allowed is None:
        allowed = os.sched_getaffinity(0)
    found, named = vcpu_threads(pid, vcpus, proc, **wait)
    vcpu_tids = {index: found[index] for index in sorted(found)[:vcpus]}
    others = [tid for tid in named if tid not in vcpu_tids.values()]
    cpus = choose_cpus(vcpus + 1, allowed, environ)
    if len(cpus) < vcpus + 1:
        log.warning('pinning %d vCPUs and QEMU\'s other threads to only '
                    '%d host CPUs %s: some share a CPU', vcpus, len(cpus),
                    cpus)
    placement = plan(vcpu_tids, others, cpus)
    pinned = {}
    for tid, cpu in placement.items():
        try:
            setaffinity(tid, {cpu})
            held = getaffinity(tid)
        except ProcessLookupError:
            continue  # a short-lived thread already gone
        if held != {cpu}:
            raise HarnessError(f'QEMU {pid} thread {tid} "{named[tid]}" '
                               f'runs on {sorted(held)} after pinning it '
                               f'to CPU {cpu}')
        pinned[tid] = (named[tid], cpu)
    for tid, (name, cpu) in sorted(pinned.items(), key=lambda i: i[1][1]):
        log.info('QEMU %d thread %d "%s" affinity {%d}', pid, tid, name, cpu)
    return pinned
