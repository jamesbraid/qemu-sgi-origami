# SPDX-License-Identifier: BSD-3-Clause
"""Expand the repeated per-brick text of an Origin 3000 topology from its plan.

A topology's prose stays hand-written. The runs that repeat per brick or per
cable sit between marker comments in the model and layer files; this module
rewrites them from the plan beside the files, and the check mode fails when a
file disagrees with its plan.
"""

import json
import pathlib
import re

import yaml

MARK = re.compile(r"^(?P<indent> *)# @generated (?P<name>[a-z0-9-]+)$")
END = re.compile(r"^ *# @end$")
PROVENANCE = "vendor-document"


def _flow(items) -> str:
    return "[" + ", ".join(json.dumps(i, ensure_ascii=False) for i in items) + "]"


def _bare(items) -> str:
    return "[" + ", ".join(items) + "]"


class Plan:
    def __init__(self, data: dict):
        self.data = data
        self.routers = {r["id"]: r for r in data["routers"]}
        self.cbricks = data["cbricks"]
        self.links = data.get("links", [])

    def cbricks_from(self, block: dict) -> list:
        return self.cbricks[block.get("cbricks_from", 0):]

    def label(self, router: str) -> str:
        return self.routers[router]["label"]


def _sources(block: dict) -> str:
    return _flow(block["sources"])


def _entry(component, occupancy, reason, block, peer=None) -> list[str]:
    lines = [f"- component: {component}", f"  occupancy: {occupancy}"]
    if peer:
        lines.append(f"  peer: {peer}")
    lines += [f"  reason: {reason}", f"  provenance: {PROVENANCE}",
              f"  sources: {_sources(block)}"]
    return lines


def _cbrick_links(plan, block):
    out = []
    for c in plan.cbricks:
        reason = block["reason"].format(
            c=c["label"], r=plan.label(c["router"]), port=c["port"])
        port = f"{c['router']}-port-{c['port']}"
        out += _entry(f"{c['id']}-link-connector", "cabled", reason, block, port)
        out += _entry(port, "cabled", reason, block, f"{c['id']}-link-connector")
    return out


def _link_ports(plan, block):
    out = []
    for link in plan.links:
        a, b, port = link["a"], link["b"], link["port"]
        for own, other in ((a, b), (b, a)):
            reason = block["reason"].format(
                a=plan.label(a), b=plan.label(b), own=plan.label(own),
                other=plan.label(other), port=port)
            out += _entry(f"{own}-port-{port}", "cabled", reason, block,
                          f"{other}-port-{port}")
    return out


def _empty_ports(block):
    out = []
    pairs = [(r, p) for g in block["groups"] for p in g["ports"] for r in block["routers"]]
    if block.get("order") != "port":
        pairs = [(r, p) for r in block["routers"] for g in block["groups"]
                 for p in g["ports"]]
    for router, port in pairs:
        group = next(g for g in block["groups"] if port in g["ports"])
        out += _entry(f"{router}-port-{port}", "empty",
                      group["reason"].format(port=port), group)
    return out


def _empty_components(plan, block):
    out = []
    for c in plan.cbricks_from(block):
        out += _entry(c["id"] + block["suffix"], "empty", block["reason"], block)
    return out


def _entity(plan, prefix):
    return plan.routers.get(prefix) or next(
        c for c in plan.cbricks if c["id"] == prefix)


def _instances(plan, block):
    out = []
    prefixes = block.get("prefixes") or [c["id"] for c in plan.cbricks_from(block)]
    for prefix in prefixes:
        out += [f"- model: {block['model']}", f"  prefix: {prefix}-",
                f"  parent: {_entity(plan, prefix).get('parent', block['parent'])}"]
        if block.get("comment"):
            out.append(f"  # {block['comment']}")
        if block.get("omit_buses"):
            out.append(f"  omit_buses: {_bare(block['omit_buses'])}")
        if block.get("alias"):
            out.append(f"  observed_hardware: *{block['alias']}")
    return out


def _aliases(plan, block):
    return [f"{c['id']}{block['suffix']}: *{block['anchor']}"
            for c in plan.cbricks_from(block)]


def _chassis_numbers(plan, block):
    return [f"- {{component: {c['id']}-chassis, number: {c['unit']}, reason: "
            + json.dumps(block["reason"].format(unit=c["unit"], rack=c.get("rack", "")),
                         ensure_ascii=False) + "}"
            for c in plan.cbricks]


def _identities(plan, block):
    out = []
    items = plan.cbricks if block["of"] == "cbricks" else list(plan.routers.values())
    for i, item in enumerate(items):
        out += [f"- component: {item['id']}{block['suffix']}",
                f"  serial: \"0x{block['serial_base'] + i:012x}\"",
                f"  reason: {block['reason'].format(i=i)}"]
    return out


def _link_buses(plan, block):
    out = []
    for link in plan.links:
        a, b, port = link["a"], link["b"], link["port"]
        la, lb = plan.label(a), plan.label(b)
        out += [f"- id: {link['id']}",
                f"  name: NUMAlink 3 cable, {la} port {port} to {lb} port {port}",
                "  kind: fabric", f"  master: {a}-router",
                "  link: {kind: cable}", f"  provenance: {PROVENANCE}",
                f"  sources: {_bare(block['sources'])}",
                f"  notes: {block['notes']}"]
    return out


def _row(component, bus, role, block, port=None):
    fabric = f" fabric_port: {port}," if port else ""
    return (f"- {{component: {component}, bus: {bus}, role: {role},{fabric} "
            f"provenance: {PROVENANCE}, sources: {_bare(block['sources'])}}}")


def _cbrick_cables(plan, block):
    out = []
    for c in plan.cbricks:
        bus = f"{c['router']}-port-{c['port']}-cable"
        out += [_row(f"{c['id']}-bedrock", bus, "endpoint", block),
                _row(f"{c['id']}-link-connector", bus, "endpoint", block)]
    return out


def _link_connections(plan, block):
    out = []
    for link in plan.links:
        a, b, port, bus = link["a"], link["b"], link["port"], link["id"]
        out += [_row(f"{a}-router", bus, "master", block, port),
                _row(f"{b}-router", bus, "endpoint", block, port),
                _row(f"{a}-port-{port}", bus, "endpoint", block, port),
                _row(f"{b}-port-{port}", bus, "endpoint", block, port)]
    return out


KINDS = {
    "cbrick-links": _cbrick_links, "link-ports": _link_ports,
    "empty-ports": lambda plan, block: _empty_ports(block), "empty-components": _empty_components,
    "instances": _instances, "aliases": _aliases,
    "chassis-numbers": _chassis_numbers, "identities": _identities,
    "link-buses": _link_buses, "cbrick-cables": _cbrick_cables,
    "link-connections": _link_connections,
}


def render_block(plan: Plan, name: str) -> list[str]:
    block = plan.data["blocks"][name]
    if block.get("provenance", PROVENANCE) != PROVENANCE:
        raise ValueError(f"block {name}: only {PROVENANCE} is generated")
    return KINDS[block["kind"]](plan, block)


def expand(text: str, plan: Plan, path) -> tuple[str, set]:
    """Return text with every marked run rewritten from the plan."""
    out, seen, skipping = [], set(), False
    for line in text.split("\n"):
        if skipping:
            if END.match(line):
                skipping = False
                out.append(line)
            continue
        out.append(line)
        match = MARK.match(line)
        if match:
            name = match["name"]
            if name not in plan.data["blocks"]:
                raise ValueError(f"{path}: marker {name} has no block in the plan")
            seen.add(name)
            indent = match["indent"]
            out += [indent + s for s in render_block(plan, name)]
            skipping = True
    if skipping:
        raise ValueError(f"{path}: marker without @end")
    return "\n".join(out), seen


def load_plan(path: pathlib.Path) -> Plan:
    return Plan(yaml.safe_load(path.read_text()))


def plan_files(models: pathlib.Path, plan_path: pathlib.Path):
    """The model and layer file a plan generates into, by the plan's name."""
    name = plan_path.stem
    return [models / f"{name}.yaml", models / "layers/products" / f"{name}.yaml"]


def expand_plan(models: pathlib.Path, plan_path: pathlib.Path) -> dict:
    """Map each generated-into file to its expanded text."""
    plan = load_plan(plan_path)
    results, seen = {}, set()
    for path in plan_files(models, plan_path):
        text, found = expand(path.read_text(), plan, path)
        results[path] = text
        seen |= found
    missing = set(plan.data["blocks"]) - seen
    if missing:
        raise ValueError(f"{plan_path}: no marker for {sorted(missing)}")
    return results


def plans(models: pathlib.Path) -> list[pathlib.Path]:
    return sorted((models / "plans").glob("*.yaml"))


def unplanned_markers(models: pathlib.Path) -> list[pathlib.Path]:
    """Files that carry a generated run but have no plan."""
    planned = {p.stem for p in plans(models)}
    files = [*models.glob("*.yaml"), *(models / "layers/products").glob("*.yaml")]
    return [f for f in files if f.stem not in planned
            and any(MARK.match(line) for line in f.read_text().split("\n"))]


def main(models: pathlib.Path, write: bool) -> int:
    bad = 0
    for path in unplanned_markers(models):
        bad += 1
        print(f"{path}: generated run with no plan")
    for plan_path in plans(models):
        for path, text in expand_plan(models, plan_path).items():
            if path.read_text() != text:
                bad += 1
                if write:
                    path.write_text(text)
                else:
                    print(f"{path}: differs from {plan_path.name}")
    return 1 if bad and not write else 0
