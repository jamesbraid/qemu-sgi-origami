# SPDX-License-Identifier: BSD-3-Clause

"""Marked synthetic serials, the generator's copy of the QEMU helper.

include/hw/mips/sgi/sgi-synthetic-serial.h owns the scheme and the
component letters; this module computes the same values for the catalogue,
so the two must agree value for value. A one-wire serial carries "QEMU" in
its top 32 bits, then a component letter and an instance number; a UniqueWare
part keeps its 12-bit customer code and carries "EMU" below it; a text serial
is "EMU", the letter and zero-padded decimal digits.
"""

from __future__ import annotations

ONEWIRE_PREFIX = 0x51454D55
UNIQUEWARE_MARKER = 0x454D55
TEXT_MARKER = "EMU"
TEXT_MIN_WIDTH = 6

# The module letter's one-wire instances split in two on an Origin 2000 or
# Onyx2: a module's serial chip takes its module number, below this, and
# each node's Hub-side module-identity responder takes one at or above it.
MODULE_RESPONDER_FIRST_INSTANCE = 16

# The component letters, in the header's order.
LETTERS = {
    "N": "node or system board",
    "U": "processor module",
    "I": "I/O board",
    "F": "frontplane",
    "S": "power supply",
    "B": "Bridge one-wire part",
    "X": "XBridge one-wire part",
    "C": "IOC3 one-wire parts",
    "R": "Router",
    "M": "module or brick",
    "G": "graphics board",
    "P": "PCI option card",
    "O": "XIO option board",
    "D": "daughtercard",
}


def _letter(letter: str) -> int:
    if letter not in LETTERS:
        raise ValueError(f"unknown synthetic serial component letter {letter!r}")
    return ord(letter)


def onewire_serial(letter: str, index: int) -> int:
    """The 48-bit one-wire ROM serial for `letter` instance `index`."""
    code = _letter(letter)
    if not 0 <= index <= 0xFF:
        raise ValueError(f"synthetic serial index {index} does not fit in a byte")
    return ONEWIRE_PREFIX << 16 | code << 8 | index


def uniqueware_serial(customer: int, index: int) -> int:
    """A UniqueWare part's serial: customer code, "EMU", 12-bit index."""
    if not 0 <= customer <= 0xFFF or not 0 <= index <= 0xFFF:
        raise ValueError("UniqueWare customer code and index are 12 bits")
    return customer << 36 | UNIQUEWARE_MARKER << 12 | index


def onewire_serial_marked(serial: int) -> bool:
    """Whether a 48-bit one-wire serial carries either marker."""
    return (serial >> 16 == ONEWIRE_PREFIX or
            (serial >> 12) & 0xFFFFFF == UNIQUEWARE_MARKER)


def text_serial(letter: str, index: int, width: int = TEXT_MIN_WIDTH) -> str:
    """The `width`-character text serial for `letter` instance `index`."""
    _letter(letter)
    digits = width - len(TEXT_MARKER) - 1
    if width < TEXT_MIN_WIDTH or not 0 <= index < 10 ** digits:
        raise ValueError(f"index {index} does not fit a {width}-character serial")
    return f"{TEXT_MARKER}{letter}{index:0{digits}d}"


def barcode(letter: str, part_serial: int) -> str:
    """A board record's barcode, from the serial of the part holding it."""
    index = 0
    if part_serial >> 16 == ONEWIRE_PREFIX:
        index = part_serial & 0xFF
    elif onewire_serial_marked(part_serial):
        index = part_serial & 0xFFF
    width = 6 if index < 100 else 7 if index < 1000 else 8
    return text_serial(letter, index, width)


def text_serial_marked(text: str) -> bool:
    """Whether a text serial has the marker, a known letter and digits."""
    return (len(text) >= TEXT_MIN_WIDTH and text.startswith(TEXT_MARKER) and
            text[3] in LETTERS and text[4:].isdigit())
