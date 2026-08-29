# The PROM-relocation copy-speed pass condition test_prom_copy_speed.py
# boots against, factored out so test_prom_copy_speed_verdict.py can
# exercise it without booting
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The copy loop is bracketed by two sgi_hub_led progress codes (see
test_prom_copy_speed.py for the full account); this module turns the
first-seen times of those two codes into a fast/slow verdict. No
qemu_test import, so the verdict companion can import it under a plain
python3 or pytest run.
"""

from lib_prom import codes_seen

# The copy loop's bracket. 0x5c is the last progress code the firmware emits
# before it copies; 0x3c the first after it jumps to the RAM copy.
COPY_BRACKET_START = '0x5c'
COPY_BRACKET_END = '0x3c'

# The budget. Fixed, the two codes are a few seconds apart; a code-from-I/O
# regression makes the loop take ~223s. 90s sits far above the fixed cost --
# tens of seconds even under heavy CI load -- and far below any real regression,
# which is an order of magnitude or more.
COPY_BRACKET_MAX_SECONDS = 90.0


def copy_bracket_seconds(first_seen):
    """The copy-loop duration from a {code: first-seen-monotonic} map, or None
    if either bracket code has not appeared yet."""
    start = first_seen.get(COPY_BRACKET_START)
    end = first_seen.get(COPY_BRACKET_END)
    if start is None or end is None:
        return None
    return end - start


def copy_is_fast(first_seen, threshold=COPY_BRACKET_MAX_SECONDS):
    """(ok, duration): ok iff the copy loop was bracketed and finished within
    threshold. A bracket that never closed is not fast."""
    duration = copy_bracket_seconds(first_seen)
    return (duration is not None and duration <= threshold), duration


def record_first_seen(first_seen, trace_text, now):
    """Stamp any bracket code newly present in trace_text with time `now`,
    keeping the earliest sighting. Mutates and returns first_seen."""
    seen = codes_seen(trace_text)
    for code in (COPY_BRACKET_START, COPY_BRACKET_END):
        if code in seen and code not in first_seen:
            first_seen[code] = now
    return first_seen
