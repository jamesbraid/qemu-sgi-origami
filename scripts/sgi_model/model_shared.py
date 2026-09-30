# SPDX-License-Identifier: BSD-3-Clause

"""Small helpers shared by the model compiler and the QEMU-input writer.

Shared helpers for model compilation and safe installation of generated
files.
"""

from __future__ import annotations

import os
import pathlib
import stat
import tempfile

from .system_model import _num as num


def _required(mapping: dict, field: str, label: str):
    if field not in mapping:
        raise ValueError(f"{label} has no {field}")
    return mapping[field]


def _integer(mapping: dict, field: str, label: str, bits: int = 64):
    value = _required(mapping, field, label)
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError(f"{label} {field} is not an unsigned {bits}-bit integer")
    return value


def _validate_integer(value: int, bits: int, label: str, *, signed=False):
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"compiled descriptor {label} is not an integer")
    minimum = -(1 << (bits - 1)) if signed else 0
    maximum = (1 << (bits - (1 if signed else 0))) - 1
    if value < minimum or value > maximum:
        kind = "signed" if signed else "unsigned"
        raise ValueError(
            f"compiled descriptor {label} value {value} does not fit "
            f"{kind} {bits}-bit field"
        )


def _mode_field(mode, field, encodings):
    value = _required(mode, field, "processor reset mode")
    for setting, encoded in encodings:
        if type(value) is type(setting) and value == setting:
            return encoded
    raise ValueError(
        f"processor reset mode {field} has unsupported value {value!r}"
    )


def _compile_r10000_reset_mode(firmware: dict, cpu_clock_hz: int, hub_clock_hz: int) -> int:
    """Pack the R10000's structured reset-mode fields into SysAD bits 31:0.

    One table per part, as the component taxonomy names it (this generation
    ships only r10000).
    """
    mode = _required(firmware, "processor_reset_mode", "firmware configuration")
    if not isinstance(mode, dict):
        raise ValueError("firmware configuration processor_reset_mode is not a mapping")
    fields = {
        "kseg0_cache_algorithm", "processor_device_number",
        "coherent_request_target", "processor_eliminate_requests",
        "maximum_outstanding_processor_requests", "system_clock_divisor",
        "secondary_cache_block_words", "secondary_cache_error_correction",
        "memory_endianness", "secondary_cache_size_kib",
        "secondary_cache_clock_divisor", "secondary_cache_clock_tap",
        "open_drain_system_interface", "cache_test_mode",
    }
    unexpected = sorted(set(mode) - fields)
    if unexpected:
        raise ValueError(
            f"processor reset mode has unsupported field '{unexpected[0]}'"
        )

    settings = (
        ("kseg0_cache_algorithm", 0, (
            ("uncached", 2),
            ("cacheable-noncoherent", 3),
            ("cacheable-coherent-exclusive", 4),
            ("cacheable-coherent-exclusive-on-write", 5),
            ("uncached-accelerated", 7),
        )),
        ("processor_device_number", 3, tuple((value, value) for value in range(4))),
        ("coherent_request_target", 5, (("external-agent", 0), ("broadcast", 1))),
        ("processor_eliminate_requests", 6, ((False, 0), (True, 1))),
        ("maximum_outstanding_processor_requests", 7,
         tuple((value, value - 1) for value in range(1, 5))),
        ("system_clock_divisor", 9,
         ((1, 1), (1.5, 2), (2, 3), (2.5, 4), (3, 5), (3.5, 6), (4, 7))),
        ("secondary_cache_block_words", 13, ((16, 0), (32, 1))),
        ("secondary_cache_error_correction", 14,
         (("retry-access-through-corrector", 0),
          ("always-access-through-corrector", 1))),
        ("memory_endianness", 15, (("little", 0), ("big", 1))),
        ("secondary_cache_size_kib", 16,
         ((512, 0), (1024, 1), (2048, 2), (4096, 3),
          (8192, 4), (16384, 5))),
        ("secondary_cache_clock_divisor", 19,
         ((1, 1), (1.5, 2), (2, 3), (2.5, 4), (3, 5))),
        ("secondary_cache_clock_tap", 25,
         tuple((value, value) for value in (*range(6), *range(8, 14)))),
        ("open_drain_system_interface", 30, ((False, 0), (True, 1))),
        ("cache_test_mode", 31, ((False, 0), (True, 1))),
    )
    encoded = {}
    result = 0
    for field, shift, values in settings:
        encoded[field] = _mode_field(mode, field, values)
        result |= encoded[field] << shift

    if cpu_clock_hz * 2 != hub_clock_hz * (encoded["system_clock_divisor"] + 1):
        raise ValueError(
            "processor reset mode system_clock_divisor does not match CPU and Hub clocks"
        )
    return result


def install_generated_outputs(outputs: list[tuple[pathlib.Path, str]],
                              replace=os.replace) -> None:
    """Install complete generated files without rewriting equal bytes."""
    prepared = [(pathlib.Path(path), text.encode()) for path, text in outputs]
    existing_modes = {}
    for destination, _ in prepared:
        try:
            metadata = destination.lstat()
        except FileNotFoundError:
            existing_modes[destination] = None
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(
                f"generated destination '{destination}' is not a regular file"
            )
        existing_modes[destination] = stat.S_IMODE(metadata.st_mode)

    staged = []
    try:
        for destination, content in prepared:
            if destination.exists() and destination.read_bytes() == content:
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{destination.name}.", dir=destination.parent
            )
            temporary = pathlib.Path(temporary_name)
            try:
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(content)
                mode = existing_modes[destination]
                os.chmod(temporary, 0o644 if mode is None else mode)
            except BaseException:
                temporary.unlink(missing_ok=True)
                raise
            staged.append((temporary, destination))
        for temporary, destination in staged:
            replace(temporary, destination)
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)
