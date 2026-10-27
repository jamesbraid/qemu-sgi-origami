# SPDX-License-Identifier: BSD-3-Clause

"""Render the reference registry as the SGI documentation's source list.

hw/mips/sgi/models/references.yaml is the one list of public sources the
machine models and the SGI pages cite. This renders it as
docs/system/mips/sgi/references.rst, so a reader of the manual can look up
any source id without the registry file. The page is checked in;
scripts/tests/test_sgi_model_references_doc.py fails when it is stale.

    python3 scripts/sgi_model/references_doc.py [--check]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from sgi_model.citations import ROOT, load_registry  # noqa: E402

OUTPUT = ROOT / "docs/system/mips/sgi/references.rst"

GROUPS = (
    ("trees", "Source trees",
     "Code citations name one of these trees, a commit and a path."),
    ("documents", "Documents",
     "SGI manuals, service documents and papers, cited by document number."),
    ("web", "Web sources",
     "Published inventories, posts, photographs and messages, cited as "
     "``web:<id>``."),
    ("firmware", "Firmware images",
     "Stock firmware, cited as ``firmware:<id>`` and identified by version "
     "and SHA-256."),
)

RIGHTS = {
    "published": "Published; may be quoted.",
    "public-web": "Public web page; may be quoted.",
    "obtain-only": "Public, without a redistribution grant: cited and linked, "
                   "never copied into this tree.",
}

LOCATORS = {
    "address": "by address", "entry": "by entry", "hunk": "by hunk",
    "line": "by line", "page": "by page", "post": "by post",
    "region": "by region", "section": "by section",
}


def label(group: str, key: str) -> str:
    return "sgi-ref-" + re.sub(r"[^a-z0-9]+", "-", f"{group}-{key}".lower()).strip("-")


def escape(text: str) -> str:
    """Keep registry text literal in reStructuredText."""
    return re.sub(r"([*`|_\\])", r"\\\1", " ".join(str(text).split()))


def link(url: str) -> str:
    return f"`<{url}>`__"


def entry_lines(group: str, key: str, entry: dict) -> list[str]:
    lines = [f".. _{label(group, key)}:", "", f"``{key}``"]
    body = []
    if group == "trees":
        body.append(escape(entry["title"]) + ".")
        if entry.get("author"):
            body.append(f"Author: {escape(entry['author'])}.")
        if entry.get("branch"):
            body.append(f"Branch ``{entry['branch']}``.")
        body.append(link(entry["url"]))
        if entry.get("browse"):
            body.append(f"Browse: {link(entry['browse'])}")
        if entry.get("sha256"):
            body.append(f"SHA-256 ``{entry['sha256']}``.")
        if entry.get("note"):
            body.append(escape(entry["note"]))
    else:
        head = f"*{escape(entry['title'])}*. {escape(entry['publisher'])}."
        kind = entry["kind"].replace("-", " ")
        locator = LOCATORS.get(entry["locator"], f"by {entry['locator']}")
        body.append(f"{head} {kind[0].upper()}{kind[1:]}, cited {locator}.")
        if entry.get("product"):
            body.append(f"For {escape(entry['product'])}.")
        if entry.get("version"):
            body.append(f"Version {escape(entry['version'])}.")
        if entry.get("banner"):
            body.append(f"Banner ``{entry['banner']}``.")
        obtain = entry.get("obtain") or {}
        if obtain.get("url"):
            body.append(link(obtain["url"]))
        if obtain.get("message_id"):
            body.append(f"Message-ID ``{obtain['message_id']}``.")
        if obtain.get("distribution"):
            body.append(f"Found on {escape(obtain['distribution'])}.")
        if obtain.get("image"):
            body.append(f"Hashed as the {escape(obtain['image'])} image.")
        if obtain.get("sha256"):
            body.append(f"SHA-256 ``{obtain['sha256']}``.")
        body.append(RIGHTS[entry["rights"]])
    lines.append("   " + " ".join(body))
    lines.append("")
    return lines


def render(registry: dict) -> str:
    out = [
        ".. SPDX-License-Identifier: BSD-3-Clause",
        "",
        ".. This page is generated from hw/mips/sgi/models/references.yaml by",
        "   scripts/sgi_model/references_doc.py. Edit the registry, not this page.",
        "",
        ".. _sgi-references:",
        "",
        "References",
        "==========",
        "",
        "The public sources the SGI machine models and these pages cite. A",
        "citation names a source by the id shown here and adds a locator: a",
        "page, a line, a post or an address.",
        "",
    ]
    for group, title, intro in GROUPS:
        entries = registry.get(group) or {}
        if not entries:
            continue
        out += [title, "-" * len(title), "", intro, ""]
        for key in sorted(entries):
            out += entry_lines(group, key, entries[key])
    return "\n".join(out).rstrip("\n") + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="fail if the checked-in page is stale")
    args = parser.parse_args()
    text = render(load_registry())
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
