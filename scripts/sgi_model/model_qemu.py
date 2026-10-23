# SPDX-License-Identifier: BSD-3-Clause

"""Generate QEMU inputs from the implemented machine profiles.

One export for every product: the compiled catalogue
`hw/mips/sgi/sgi-machines.json` (format 4, `topologies` trimmed to
the catalogue output rule plus the launcher-facing `offerings`
list) and its manifest `sn-model.json`. QEMU parses the catalogue at
build time into the types `qapi/sgi-machines.json` declares; nothing here
renders C any more. `model_compile.compile_model` makes no per-product
distinction, so there is one compile call here too, not one per machine
name.
"""

from __future__ import annotations

import json
import pathlib

from .model_compile import compile_model
from .model_graphics import check_one_configuration_per_id
from .model_emit import default_topology, render_catalogue_json, render_manifest
from .model_shared import install_generated_outputs

from .registry import (
    PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY, PRODUCT_MODELS, PRODUCT_NAMES,
    REFERENCE_TOPOLOGIES,
)
from .model_validation import load_checked_system_models, discover_model_paths

SN_MODEL_JSON = "hw/mips/sgi/sn-model.json"
SGI_MACHINES_JSON = "hw/mips/sgi/sgi-machines.json"

# The supported catalogue product names come from the local registry.
_MACHINE_NAMES = PRODUCT_NAMES


def _check_default_topology(topologies: list[dict], machine_names: dict[str, str],
                            product_models: dict[str, dict]) -> None:
    """Require an implicit default unless the product requires topology=.

    Products in PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY remain available, but
    have no implicit default and must name a topology.
    """
    by_machine: dict[str, list[str]] = {}
    for topology in topologies:
        by_machine.setdefault(machine_names[topology["name"]], []).append(topology["name"])
    for machine_name, names in by_machine.items():
        if machine_name in PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY:
            continue
        if machine_name in names or len(names) == 1:
            continue
        try:
            if default_topology(machine_name, product_models) in names:
                continue
        except ValueError:
            pass
        raise ValueError(
            f"'{machine_name}' compiles {len(names)} topologies "
            f"{sorted(names)} with none named for the product; the catalogue "
            "needs an explicit default"
        )


def render_qemu_inputs(model_root: pathlib.Path, *,
                       machines: tuple[str, ...] = _MACHINE_NAMES) -> dict[str, str]:
    """Render the complete catalogue export for the selected machines,
    without inspecting a QEMU consumer.
    """
    unknown = set(machines) - set(_MACHINE_NAMES)
    if unknown or not machines:
        raise ValueError(f"unsupported QEMU machine selection: {sorted(unknown)}")
    checked = load_checked_system_models(
        discover_model_paths(model_root), model_root=model_root)

    topologies = []
    references = []
    machine_names: dict[str, str] = {}
    source_models: dict[str, dict] = {}
    for path, model in checked:
        machine = model["machine"]
        if (path.stem in REFERENCE_TOPOLOGIES
                and machine.get("model_role") == "reference"):
            references.append({
                "name": path.stem,
                "product": path.stem.split("-")[0],
                "display-name": machine["name"],
                "description": machine.get("notes", ""),
            })
        emulation = machine.get("emulation", {})
        machine_name = emulation.get("machine_name")
        if machine.get("model_role") != "emulation-target" or machine_name not in machines:
            continue
        # A planned target compiles (its own golden proves it) but stays
        # out of the export until the emulator can build it; the catalogue
        # a QEMU tree adopts names only what that tree implements.
        if emulation.get("lifecycle") != "implemented":
            continue
        entry = compile_model(model)
        topologies.append(entry)
        machine_names[entry["name"]] = machine_name
        source_models[path.stem] = model

    if not topologies:
        raise ValueError(f"no model profiles for QEMU machine selection: {sorted(machines)}")

    product_models = {
        name: next(model for _path, model in checked
                   if _path.stem == filename)
        for name, filename in PRODUCT_MODELS.items()
    }
    reference_models = {
        path.stem: model for path, model in checked
        if path.stem in REFERENCE_TOPOLOGIES
        and model["machine"].get("model_role") == "reference"
    }
    _check_default_topology(topologies, machine_names, product_models)
    check_one_configuration_per_id(topologies)
    catalogue = render_catalogue_json(
        list(source_models.values()), product_models=product_models,
        references=references)
    manifest_models = {**source_models, **reference_models}
    manifest = render_manifest(manifest_models, {SGI_MACHINES_JSON: catalogue})
    return {SGI_MACHINES_JSON: catalogue, SN_MODEL_JSON: manifest}


def generate_qemu_inputs(model_root: pathlib.Path, tree: pathlib.Path, *,
                         machines: tuple[str, ...] = _MACHINE_NAMES) -> list[pathlib.Path]:
    """Install an explicit export, retaining unchanged file timestamps."""
    outputs = [(tree / path, text) for path, text in
               render_qemu_inputs(model_root, machines=machines).items()]
    install_generated_outputs(outputs)
    return [path for path, _text in outputs]


def install_qemu_inputs(model_root: pathlib.Path, tree: pathlib.Path, *,
                        machines: tuple[str, ...] = _MACHINE_NAMES) -> list[pathlib.Path]:
    """Adopt an export only into a consumer declaring the format-4 interface."""
    contract = tree / "hw/mips/origami-model-formats.json"
    try:
        formats = json.loads(contract.read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f"cannot read QEMU model format declarations: {contract}") from error
    if not isinstance(formats, dict):
        raise ValueError("QEMU model format declarations must be an object")
    if type(formats.get("sgi-sn")) is not int or formats["sgi-sn"] != 4:
        raise ValueError("QEMU does not accept the sgi-sn model at format 4")
    unknown = set(machines) - set(_MACHINE_NAMES)
    if unknown:
        raise ValueError(f"unsupported QEMU machine '{sorted(unknown)[0]}'")
    return generate_qemu_inputs(model_root, tree, machines=machines)
