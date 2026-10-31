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
from .model_graphics import check_catalogue
from .model_emit import default_topology, render_catalogue_json, render_manifest
from .model_shared import install_generated_outputs

from .registry import (
    PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY, PRODUCT_MODELS, PRODUCT_NAMES,
    REFERENCE_TOPOLOGIES,
)
from .model_validation import load_checked_system_models, discover_model_paths
from .system_model import is_default_variant, model_variant

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


def _check_variant_totals(topologies: list[dict]) -> None:
    """Require records sharing a name and board to accept disjoint CPU
    totals.

    A composition's board variants all compile under its one name; QEMU
    picks the board by board= and then the record whose supported CPU
    totals contain -smp, so within one board no total may select two of
    them.
    """
    owners: dict[tuple[str, str | None, int], int] = {}
    for index, topology in enumerate(topologies):
        board = topology.get("board")
        for total in {sum(population)
                      for population in topology["supported_cpu_populations"]}:
            other = owners.setdefault(
                (topology["name"], board, total), index)
            if other != index:
                raise ValueError(
                    f"topology '{topology['name']}' has two variants that "
                    f"both accept {total} CPUs"
                    + (f" as {board}" if board else "")
                    + "; QEMU selects a variant by its CPU total")


def _check_processor_ids(topologies: list[dict]) -> None:
    """Require one R16000 id to name one processor everywhere.

    Offerings and launchers select a processor by its id, so two fitted
    processors that share an id must agree on every fact. Only the R16000
    is held to it so far: the Fuel's and the Origin 300's 500 MHz R14000
    share an id with different PRId revisions.
    """
    seen: dict[str, str] = {}
    # SN records, the ones that name a board, share processor ids.
    for topology in (item for item in topologies if "board" in item):
        for node in topology["nodes"]:
            for slot in node["cpu_slots"]:
                assembly = slot["assembly"]
                if not assembly or assembly["kind"] != "R16000":
                    continue
                facts = json.dumps(assembly, sort_keys=True)
                if seen.setdefault(assembly["id"], facts) != facts:
                    raise ValueError(
                        f"processor id '{assembly['id']}' names two "
                        f"different processors (topology "
                        f"'{topology['name']}')")


def _model_key(path: pathlib.Path, model: dict) -> str:
    """A resolved model's name in the manifest: its composition's, with
    the board variant after it for a composition that declares several."""
    variant = model_variant(model)
    return path.stem if variant is None else f"{path.stem}/{variant}"


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
        source_models[_model_key(path, model)] = model

    if not topologies:
        raise ValueError(f"no model profiles for QEMU machine selection: {sorted(machines)}")

    product_models = {
        name: next(model for _path, model in checked
                   if _path.stem == filename and is_default_variant(model))
        for name, filename in PRODUCT_MODELS.items()
    }
    reference_models = {
        path.stem: model for path, model in checked
        if path.stem in REFERENCE_TOPOLOGIES
        and model["machine"].get("model_role") == "reference"
    }
    _check_default_topology(topologies, machine_names, product_models)
    _check_variant_totals(topologies)
    _check_processor_ids(topologies)
    check_catalogue(topologies)
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
