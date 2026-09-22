# The MIPS program builder the bare-metal exercises in this directory
# write their own firmware with, and the trace-log reader that pairs each
# machine reset with the write that asked for it
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Several tests here boot a few dozen hand-assembled instructions in place
of firmware, because the question they ask -- does a port-reset write
stop the peer, does an ELSC "rst" transaction reset the machine, does an
L1 reset request -- has no answer a real guest can give without dragging
a whole operating system into the measurement. Each of those tests grew
its own identical copy of the assembler; this is that assembler, once.

Plain Python with no qemu_test dependency, the same rule lib_prom.py
follows and for the same reason: test_lib_mips.py exercises it under a
bare `python3` run where qemu_test is not importable.
"""

import re
import struct
from pathlib import Path


# The MIPS registers these exercises use. The union of what the three
# reset exercises and the SMP exercise each declared for themselves.
R_ZERO = 0
R_AT = 1
R_A0 = 4
R_A1 = 5
R_A2 = 6
R_T0 = 8
R_T1 = 9
R_T2 = 10
R_T3 = 11
R_T4 = 12
R_T5 = 13
R_S0 = 16
R_S1 = 17
R_S2 = 18
R_S3 = 19
R_S4 = 20


def mips_i(op, rs, rt, immediate):
    return op << 26 | rs << 21 | rt << 16 | immediate & 0xffff


def mips_r(rs, rt, rd, shift, function):
    return rs << 21 | rt << 16 | rd << 11 | shift << 6 | function


class MipsProgram:
    """A list of big-endian instruction words, written out as a file the
    machine boots with -bios.

    An exercise that needs more than this -- a device's own register
    dance, say -- subclasses it and adds those methods there, where the
    device they belong to is.
    """

    def __init__(self):
        self.instructions = []

    def emit(self, instruction):
        self.instructions.append(instruction)

    def here(self):
        return len(self.instructions)

    def branch(self, op, rs, rt):
        branch = self.here()
        self.emit(mips_i(op, rs, rt, 0))
        self.emit(0)                      # branch delay slot
        return branch

    def patch(self, branch, target):
        displacement = target - branch - 1
        assert -(1 << 15) <= displacement < (1 << 15)
        self.instructions[branch] |= displacement & 0xffff

    def spin(self):
        spin = self.here()
        self.patch(self.branch(0x04, R_ZERO, R_ZERO), spin)

    def address_prefix(self, register, high):
        """register = high << 32, the top half of an xkphys address."""
        self.emit(mips_i(0x0f, R_ZERO, register, high))
        self.emit(mips_r(R_ZERO, register, register, 0, 0x3c))   # dsll32

    def address(self, register, prefix, offset):
        """register = the xkphys address prefix << 32 | offset -- the form
        every R10000-family board reaches its uncached windows through."""
        self.address_prefix(register, prefix)
        self.emit(mips_i(0x0f, R_ZERO, R_AT, offset >> 16))
        self.emit(mips_i(0x0d, R_AT, R_AT, offset & 0xffff))
        self.emit(mips_r(register, R_AT, register, 0, 0x25))     # or

    def write(self, path):
        Path(path).write_bytes(b''.join(
            struct.pack('>I', instruction)
            for instruction in self.instructions))


# The emulator's own log lines these exercises read their answer out of:
# a device write, a translation block entered, and a processor reset.
WRITE_RE = re.compile(
    r"memory_region_ops_write cpu (-?\d+) mr \S+ addr 0x([0-9a-f]+) "
    r"value 0x([0-9a-f]+) size \d+ name '([^']*)'")
TRACE_RE = re.compile(r"^Trace (\d+):")
RESET_RE = re.compile(r"^cpu_reset (\d+)")


def reset_windows(log, committing_write):
    """One entry per machine reset: the requesting processor (the nearest
    preceding write committing_write() claims) and the log lines showing
    another processor entering a translation block before the reset that
    followed.

    committing_write(line) returns the processor index when that line is
    the device write which commits a reset, and None otherwise -- which
    write that is belongs to the device, so each exercise supplies its
    own.

    A reset resets every CPU object, so more than one "cpu_reset" line can
    report one reset event; a run of consecutive lines collapses to its
    first. A board's own reset at machine start happens before the guest
    program has written anything -- a leading run with no preceding write
    is that startup reset, not one the exercise asked for, and is skipped
    rather than treated as a window.
    """
    lines = log.splitlines()
    resets = []
    last_reset_line = None
    for i, line in enumerate(lines):
        if RESET_RE.match(line):
            if last_reset_line is None or i != last_reset_line + 1:
                resets.append(i)
            last_reset_line = i

    windows = []
    search_from = 0
    for reset_index in resets:
        requester = None
        write_index = None
        for i in range(reset_index - 1, search_from - 1, -1):
            cpu = committing_write(lines[i])
            if cpu is not None:
                requester, write_index = cpu, i
                break
        search_from = reset_index + 1
        if requester is None:
            continue
        entered = []
        for line in lines[write_index + 1:reset_index]:
            trace = TRACE_RE.match(line)
            if trace and int(trace.group(1)) != requester:
                entered.append(line)
        windows.append((requester, entered))
    return windows
