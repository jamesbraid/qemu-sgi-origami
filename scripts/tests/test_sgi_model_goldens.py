# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Golden captures for the catalogued machine configurations.

Each configuration gets its own directory under system-model-fixtures
holding its resolved descriptor as canonical JSON and the SHA-256 of that
JSON -- the resolver's own output, independent of any compiler. The
compiled-and-emitted format-2 descriptor is a separate golden,
`test-model-format2.py`'s own `system-model-fixtures/format2/<configuration>/`:
format 2 compiles every product's implemented configurations into one
`sn-model.c`, so a per-configuration byte-for-byte copy of
`tools/origami model qemu-inputs`'s own output (this file's job before the
format-2 cut-over, when each product still had its own compiler and export)
no longer distinguishes one configuration from a sibling sharing its
product. ORIGAMI_UPDATE_GOLDENS=1 rewrites the golden directories instead
of comparing against them.
"""

from __future__ import annotations

import difflib
import hashlib
import json
import os
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
FIXTURES = pathlib.Path(__file__).resolve().parent / "system-model-fixtures"

sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model.model_validation import load_checked_system_model, schema


# Configuration directory name -> docs/machines file.
CONFIGURATIONS = {
    "origin200": "origin200.yaml",
    "origin200-dual": "origin200-dual.yaml",
    "origin200-gigachannel": "origin200-gigachannel.yaml",
    "origin200-gigachannel-loopback": "origin200-gigachannel-loopback.yaml",
    "origin2000": "origin2000.yaml",
    "origin2000-rack": "origin2000-rack.yaml",
    "origin2000-deskside": "origin2000-deskside.yaml",
    "origin300": "origin300.yaml",
    "origin300-two-chassis": "origin300-two-chassis.yaml",
    "onyx2-rack": "onyx2-rack.yaml",
    "onyx2-deskside": "onyx2-deskside.yaml",
}


def canonical_descriptor(model: dict) -> str:
    return json.dumps(model, sort_keys=True, indent=2) + "\n"


def captured_files(configuration: str) -> dict[str, str]:
    """Every golden file this configuration's directory should hold."""
    model = load_checked_system_model(INPUTS / CONFIGURATIONS[configuration], model_schema=schema(INPUTS))
    descriptor = canonical_descriptor(model)
    digest = hashlib.sha256(descriptor.encode()).hexdigest() + "\n"
    return {"resolved.json": descriptor, "resolved.sha256": digest}


class GoldenTests(unittest.TestCase):
    def test_every_configuration_matches_its_golden_directory(self):
        update = os.environ.get("ORIGAMI_UPDATE_GOLDENS") == "1"
        for configuration in CONFIGURATIONS:
            with self.subTest(configuration=configuration):
                directory = FIXTURES / configuration
                expected = captured_files(configuration)
                if update:
                    directory.mkdir(parents=True, exist_ok=True)
                    for stale in directory.iterdir():
                        if stale.name not in expected:
                            stale.unlink()
                    for name, text in expected.items():
                        (directory / name).write_text(text)
                    continue
                self.assertTrue(
                    directory.is_dir(),
                    f"missing golden directory {directory}; "
                    "run with ORIGAMI_UPDATE_GOLDENS=1 to capture it",
                )
                existing = {path.name for path in directory.iterdir()}
                extra = sorted(existing - set(expected))
                self.assertEqual(
                    extra, [],
                    f"{directory} has golden files no longer produced: {extra}",
                )
                for name, text in expected.items():
                    with self.subTest(file=name):
                        golden = directory / name
                        self.assertTrue(
                            golden.exists(),
                            f"missing golden file {golden}; "
                            "run with ORIGAMI_UPDATE_GOLDENS=1 to capture it",
                        )
                        actual = golden.read_text()
                        if actual != text:
                            diff = "".join(difflib.unified_diff(
                                actual.splitlines(keepends=True),
                                text.splitlines(keepends=True),
                                fromfile=str(golden),
                                tofile=f"{golden} (regenerated)",
                            ))
                            self.fail(f"{golden} differs from the resolved model:\n{diff}")


if __name__ == "__main__":
    unittest.main()
