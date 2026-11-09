# SPDX-License-Identifier: BSD-3-Clause

"""Render the catalogue's settings as the SGI documentation's settings page.

The compiled catalogue (hw/mips/sgi/sgi-machines.json) lists, for every
offering, the machine inputs a launch may give as NAME[.KEY]=VALUE. This
renders them as docs/system/mips/sgi/settings.rst, one table per product,
so a reader of the manual can find what a machine takes without the
catalogue.
The page is checked in; scripts/tests/test_sgi_model_settings_doc.py fails
when it is stale.

    python3 scripts/sgi_model/settings_doc.py [--check]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

try:
    from .settings import DEFAULT_BASIS
except ImportError:         # run as a script
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
    from sgi_model.settings import DEFAULT_BASIS

ROOT = pathlib.Path(__file__).resolve().parents[2]
CATALOGUE = ROOT / "hw/mips/sgi/sgi-machines.json"
OUTPUT = ROOT / "docs/system/mips/sgi/settings.rst"


def _target(definition: dict) -> str:
    target = definition["target"]
    text = {"machine": "-M", "global": "-global", "cpu": "-cpu"}[
        target["kind"]]
    prop = target["property"]
    if target.get("device"):
        prop = f"{target['device']}.{prop}"
    return f"{text} ``{prop}``"


def _slots(item: dict) -> str:
    slots = item.get("slots")
    if not slots:
        return "none"
    keys = slots["keys"]
    shown = ", ".join(keys) if len(keys) <= 6 else (
        ", ".join(keys[:3]) + f", ... {keys[-1]}")
    return f"{slots['scope']}: {shown}"


def _default(item: dict) -> str:
    if "default" not in item:
        return "required" if item["required"] else "none"
    text = f"``{item['default']}``"
    if item.get("default-provenance"):
        text += f" ({item['default-provenance']})"
    return text


def render(catalogue: dict) -> str:
    definitions = {d["name"]: d for d in catalogue["setting-definitions"]}
    out = [
        ".. SPDX-License-Identifier: BSD-3-Clause",
        "",
        ".. This page is generated from hw/mips/sgi/sgi-machines.json by",
        "   scripts/sgi_model/settings_doc.py. Edit the generator, not this",
        "   page.",
        "",
        ".. _sgi-settings:",
        "",
        "Machine settings",
        "================",
        "",
        "A launch gives a machine input as ``NAME[.KEY]=VALUE``. Each",
        "product's tables list the settings its offerings take: the kind of",
        "value, the keys of a slotted setting, the QEMU property the value",
        "sets, and the default, with the mark of how well the default is",
        "known (see `Where the defaults come from`_). A setting with no",
        "default and no mark is optional; one marked required must be",
        "given. Where an offering of",
        "the product offers a setting under other keys (the Origin 300's",
        "chassis count, say), the keys shown are the product's first",
        "offering's.",
        "",
    ]
    products: dict[str, dict] = {}
    for offering in catalogue["offerings"]:
        products.setdefault(offering["product"], offering)
    for product in sorted(products):
        offering = products[product]
        title = f"``{product}``"
        out += [title, "-" * len(title), ""]
        out += [".. list-table::", "   :header-rows: 1", "",
                "   * - Setting", "     - Kind", "     - Slots",
                "     - Applied as", "     - Default"]
        for item in offering["settings"]:
            definition = definitions[item["name"]]
            kind = definition["kind"]
            if kind == "enum":
                kind += " (" + ", ".join(definition["values"]) + ")"
            out += [f"   * - ``{item['name']}``", f"     - {kind}",
                    f"     - {_slots(item)}",
                    f"     - {_target(definition)}",
                    f"     - {_default(item)}"]
        groups = offering["setting-groups"]
        if groups:
            out += [""]
            for group in groups:
                members = ", ".join(f"``{m}``" for m in group["members"])
                out += [f"Group ``{group['name']}``: {group['rule']} of "
                        f"{members}."]
        out += [""]
    out += _basis_section(catalogue)
    return "\n".join(out).rstrip("\n") + "\n"


def _basis_section(catalogue: dict) -> list[str]:
    """Each defaulted setting's marks and where its value is read from."""
    marks: dict[str, set[str]] = {}
    for offering in catalogue["offerings"]:
        for item in offering["settings"]:
            if "default" in item:
                marks.setdefault(item["name"], set()).add(
                    item["default-provenance"])
    out = ["Where the defaults come from", "-" * 28, "",
           "Every default carries the mark the manual gives the fact:",
           "``established``, ``inferred`` or ``approximated``. The generator",
           "refuses a default whose basis is not written down, so a launch",
           "tool never shows a value with nothing behind it.", "",
           ".. list-table::", "   :header-rows: 1", "",
           "   * - Setting", "     - Marks", "     - Where the value is read from"]
    for name in sorted(marks):
        out += [f"   * - ``{name}``",
                f"     - {', '.join(sorted(marks[name]))}",
                f"     - {DEFAULT_BASIS[name]}"]
    return out + [""]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="fail if the checked-in page is stale")
    args = parser.parse_args()
    text = render(json.loads(CATALOGUE.read_text()))
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != text:
            print(f"{OUTPUT.relative_to(ROOT)} is stale; run {__file__}",
                  file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text)
    print(OUTPUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
