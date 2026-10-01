#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""State shared by the Origin 200 and Origin 300 reset tests."""

import re
import time

# Shared uncached RAM at physical 0x30000, reached through xkphys. Memory
# survives the reset, which is how each boot knows which cycle it is in.
# The same layout serves both test modules; the L1 section names
# its two done words STATE_A_DONE/STATE_B_DONE (its own convention) but
# they are the same two words STATE_DONE and STATE_DONE + 8 the other two
# test classes check together as a single two-word slice.
STATE = 0x30000
STATE_PHASE = 0x00
STATE_B_MARK = 0x08
STATE_B_COUNT = 0x10
STATE_DONE = 0x18
STATE_A_DONE = STATE_DONE
STATE_B_DONE = STATE_DONE + 8
STATE_WORDS = 5

# Resets per run, and how far the peer processor's counter must run in
# each cycle before the requester proceeds: far enough that the peer is
# demonstrably executing, small enough to keep each cycle short. Shared by
# all three reset mechanisms.
CYCLES = 20
B_COUNT_TARGET = 8

# The 64-bit kernel mode the firmware itself selects at entry: clears
# ERL/EXL and sets CP0 Status.KX. Architectural, not board- or
# mechanism-specific, so both test modules use the same value.
STATUS_KX_KERNEL = 0x24400080

class _ResetCycleMixin:
    """read_state and wait_for_both_slices: shared by the three reset
    mechanisms, since every guest program writes the same
    STATE layout regardless of what triggers the reset. Kept here rather
    than in lib_mips, which stays free of the qemu_test dependency these
    need (self.vm, self.assertEqual, self.fail)."""

    def read_state(self):
        result = self.vm.cmd(
            'human-monitor-command',
            command_line=f'xp /{STATE_WORDS}gx {STATE:#x}')
        state = [int(value, 16)
                 for value in re.findall(r'0x([0-9a-fA-F]{16})', result)]
        self.assertEqual(len(state), STATE_WORDS, result)
        return state

    def wait_for_both_slices(self, timeout=60):
        deadline = time.monotonic() + timeout
        state = []
        while time.monotonic() < deadline:
            state = self.read_state()
            if state[STATE_DONE // 8:STATE_DONE // 8 + 2] == [1, 1]:
                return state
            time.sleep(0.1)
        self.fail(f'the machine did not run {CYCLES} resets: {state}')
