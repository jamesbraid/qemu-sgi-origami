# SPDX-License-Identifier: BSD-3-Clause

"""Check the machine models' citations against the reference registry.

Every "sources:" entry and "datasheet_contracts" entry in
hw/mips/sgi/models names where a fact came from. A citation a reader can
follow takes one of these forms, each resolved against
hw/mips/sgi/models/references.yaml or this tree:

    <doc-id> <locator>                 007-3708-002 Table 1-1 printed page 5
    <tree>:<commit> <path> [lines]     linux:db2ddb871435 arch/mips/...
    web:<key> <locator>                web:dmesgd-8994 the onewire0 line
    firmware:<key> <locator>           firmware:ip27-prom-6.156 string "..."
    qemu:<path>[#label]                qemu:docs/specs/sgi-ip27.rst#label

The check runs in report mode. It fails only on a citation that uses one
of those forms wrongly: a malformed id, or one the registry or the tree
does not hold. Everything else a public reader could not follow is
reported by class and counted, so the counts can fall commit by commit
until the check can fail on them too. Firmware citations are reported for
the record and never fail.

    python3 scripts/sgi_model/citations.py [--list CLASS]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field

import jsonschema
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODELS = ROOT / "hw/mips/sgi/models"
REGISTRY = "references.yaml"
REGISTRY_SCHEMA = "references.schema.json"

FAILING = ("malformed", "unregistered")

# What each reported class means, in the order the summary prints them.
REPORTED = {
    "origami-path": "names a document outside this tree",
    "copied-datasheet": "names a docs/datasheets copy this tree carries",
    "decision-id": "names a decision record",
    "tool-path": "names a tools/ program",
    "library-path": "names a file in a document cache, not a registry key",
    "bare-url": "gives a URL instead of a registry key",
    "nickname": "names a capture by an internal nickname",
    "vendor-source": "fact's provenance is vendor source",
    "qemu-code-only": "fact cites only QEMU code",
    "free-text-only": "fact cites only free text",
    "missing-locator": "registered id with no locator",
    "unpinned-commit": "source-tree path with no commit",
    "unknown-tree": "commit in a tree with no known public location",
    "internal-note": "notes or open-question text with an internal pointer",
    "firmware": "cites a stock firmware image",
    "obtain-only": "cites a source with no redistribution grant",
}

DOC_ID = re.compile(r"^(?P<id>\d{3}-\d{4}-\d{3})(?=\s|$|[,:;])\s*(?P<rest>.*)$", re.S)
PREFIXED = re.compile(r"^(?P<kind>web|firmware):(?P<key>\S*)\s*(?P<rest>.*)$", re.S)
QEMU = re.compile(r"^qemu:(?P<path>\S*)\s*(?P<rest>.*)$", re.S)
TREE = re.compile(r"^(?P<tree>[a-z][a-z0-9-]*):(?P<ref>\S+)\s*(?P<rest>.*)$", re.S)
KEY = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
SHA = re.compile(r"^[0-9a-f]{12,40}$")
HEXISH = re.compile(r"^[0-9a-f]{7,40}$")
QEMU_CODE = re.compile(r"^(hw|include|scripts|target|tests)/|^query-sgi-machines\b|^QEMU\b")
LIBRARY_PATH = re.compile(
    r"^[a-z0-9][a-z0-9_.-]*(/[A-Za-z0-9_.-]+)+\.(pdf|html?|jpe?g|png|txt|tar\.gz)\b")
URL = re.compile(r"https?://")
# A mainline Linux path named in prose, without a commit.
MAINLINE = re.compile(r"^Mainline Linux \S+/")

# Pointers a public reader cannot follow, in citation text and in notes.
# Documents under docs/specs, docs/system and the other QEMU documentation
# directories ship with this tree; these do not.
INTERNAL_DOC = re.compile(
    r"\bdocs/(datasheets|notebooks|decisions|plans|designs|known-issues|research|"
    r"analysis|library|journey|boot-logs|provenance|reviews|machines)\b"
    r"|\bOrigami\b")
DECISION = re.compile(r"\bD-0\d{3}\b")
TOOL = re.compile(r"(^|[\s(`])tools/[a-z]")
# Internal names for real-hardware captures. A capture is cited by its
# registry key instead.
NICKNAMES = re.compile(r"\b(joint-buck|true-wombat)\b")

# Fields whose prose the internal-pointer scan reads.
NOTE_FIELDS = {"notes", "note", "reason", "evidence_gap", "description"}


@dataclass
class Finding:
    file: str
    where: str
    text: str


@dataclass
class Report:
    entries: int = 0
    facts: int = 0
    files: int = 0
    findings: dict[str, list[Finding]] = field(default_factory=dict)

    def add(self, cls: str, file: str, where: str, text: str) -> None:
        self.findings.setdefault(cls, []).append(Finding(file, where, text))

    def count(self, cls: str) -> int:
        return len(self.findings.get(cls, []))

    @property
    def failures(self) -> list[str]:
        return [f"{cls}: {f.file}: {f.where}: {f.text}"
                for cls in FAILING for f in self.findings.get(cls, [])]

    def summary(self) -> str:
        lines = [f"citations: {self.entries} entries on {self.facts} facts "
                 f"in {self.files} files"]
        lines.append("failing:")
        for cls in FAILING:
            lines.append(f"  {self.count(cls):5d}  {cls}")
        lines.append("reported:")
        for cls, meaning in REPORTED.items():
            lines.append(f"  {self.count(cls):5d}  {cls}: {meaning}")
        return "\n".join(lines)


def load_registry(models: pathlib.Path = MODELS) -> dict:
    """Read the registry and check it against its schema."""
    registry = yaml.safe_load((models / REGISTRY).read_text())
    schema = json.loads((models / REGISTRY_SCHEMA).read_text())
    errors = sorted(jsonschema.Draft202012Validator(schema).iter_errors(registry),
                    key=lambda error: list(error.path))
    if errors:
        raise ValueError("; ".join(
            f"{REGISTRY}: {'/'.join(map(str, e.path)) or '(root)'}: {e.message}"
            for e in errors))
    return registry


def rst_labels(path: pathlib.Path) -> set[str]:
    return set(re.findall(r"^\.\. _([^:]+):\s*$", path.read_text(), re.M))


def internal_pointers(text: str) -> list[str]:
    """The internal-pointer classes a piece of text falls in."""
    found = []
    if INTERNAL_DOC.search(text):
        found.append("origami-path")
    if DECISION.search(text):
        found.append("decision-id")
    if TOOL.search(text):
        found.append("tool-path")
    if NICKNAMES.search(text):
        found.append("nickname")
    return found


def classify(text: str, registry: dict, tree: pathlib.Path) -> tuple[str, list[tuple[str, str]]]:
    """Classify one citation string.

    Return its form -- "document", "web", "firmware", "tree", "qemu" for
    the citable forms, otherwise "internal", "library", "url",
    "qemu-code" or "free-text" -- and the classes it falls in, each with
    a detail.
    """
    text = text.strip()
    problems: list[tuple[str, str]] = []

    match = DOC_ID.match(text)
    if match:
        doc = match["id"]
        if doc not in registry["documents"]:
            return "document", [("unregistered", f"document {doc}")]
        if not match["rest"].strip(" ,:;"):
            problems.append(("missing-locator", doc))
        return "document", problems + _rights(registry["documents"][doc])

    match = PREFIXED.match(text)
    if match:
        kind, key = match["kind"], match["key"]
        if not KEY.match(key):
            return kind, [("malformed", f"{kind} key {key!r}")]
        if key not in registry[kind]:
            return kind, [("unregistered", f"{kind}:{key}")]
        if not match["rest"].strip(" ,:;"):
            problems.append(("missing-locator", f"{kind}:{key}"))
        if kind == "firmware":
            problems.append(("firmware", key))
        return kind, problems + _rights(registry[kind][key])

    match = QEMU.match(text)
    if match:
        path, _, label = match["path"].partition("#")
        target = tree / path
        if not path or path.startswith("/") or ".." in pathlib.PurePosixPath(path).parts:
            return "qemu", [("malformed", f"qemu path {match['path']!r}")]
        if not target.is_file():
            return "qemu", [("unregistered", f"qemu:{path} is not in this tree")]
        if label and (target.suffix != ".rst" or label not in rst_labels(target)):
            return "qemu", [("unregistered", f"qemu:{path} has no label {label!r}")]
        return "qemu", []

    match = TREE.match(text)
    if match and not URL.match(text):
        name, ref = match["tree"], match["ref"]
        trees = registry["trees"]
        if name in trees:
            if not HEXISH.match(ref):
                return "tree", [("unpinned-commit", f"{name}: {ref}")]
            if not SHA.match(ref):
                return "tree", [("malformed", f"{name}:{ref} needs at least 12 digits")]
            if not match["rest"].strip():
                problems.append(("missing-locator", f"{name}:{ref}"))
            if trees[name]["url"] is None:
                problems.append(("unknown-tree", name))
            return "tree", problems
        if HEXISH.match(ref):
            return "tree", [("unregistered", f"tree {name}")]

    if text.startswith("docs/datasheets/"):
        path = re.split(r"[\s,:;]", text, maxsplit=1)[0]
        if (tree / path).is_file():
            problems.append(("copied-datasheet", path))
    for cls in internal_pointers(text):
        if not (cls == "origami-path" and problems):
            problems.append((cls, text))
    if problems:
        return "internal", problems
    if QEMU_CODE.match(text):
        return "qemu-code", problems
    if MAINLINE.match(text):
        return "tree", [("unpinned-commit", text)]
    if LIBRARY_PATH.match(text):
        return "library", [("library-path", text.split()[0])]
    if URL.search(text):
        return "url", [("bare-url", URL.search(text).group(0))]
    return "free-text", problems


def _rights(entry: dict) -> list[tuple[str, str]]:
    return [("obtain-only", entry["title"])] if entry["rights"] == "obtain-only" else []


def _expand(value, table: dict):
    """Replace per-file source-table keys with the citations they name."""
    if isinstance(value, dict):
        return {key: ([table.get(item, item) if isinstance(item, str) else item
                       for item in child]
                      if key == "sources" and isinstance(child, list)
                      else _expand(child, table))
                for key, child in value.items()}
    if isinstance(value, list):
        return [_expand(item, table) for item in value]
    return value


def _walk(value, where: str, in_question: bool = False):
    """Yield (where, mapping, inside an open question) for every mapping."""
    if isinstance(value, dict):
        yield where, value, in_question
        for key, child in value.items():
            yield from _walk(child, f"{where}/{key}",
                             in_question or key == "open_questions")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from _walk(child, f"{where}[{index}]", in_question)


def check_file(path: pathlib.Path, models: pathlib.Path, registry: dict,
               tree: pathlib.Path, report: Report) -> None:
    relative = path.relative_to(models).as_posix()
    document = yaml.safe_load(path.read_text())
    if not isinstance(document, dict):
        return
    table = document.pop("sources", None)
    if isinstance(table, dict):
        document = _expand(document, table)
    report.files += 1
    for where, mapping, in_question in _walk(document, ""):
        for key in sorted(NOTE_FIELDS & mapping.keys()):
            if isinstance(mapping[key], str):
                if internal_pointers(mapping[key]):
                    report.add("internal-note", relative, f"{where}/{key}", mapping[key])
        if in_question:
            for key, text in mapping.items():
                if (key not in NOTE_FIELDS and isinstance(text, str)
                        and internal_pointers(text)):
                    report.add("internal-note", relative, f"{where}/{key}", text)
        for key in ("sources", "datasheet_contracts"):
            entries = mapping.get(key)
            if not isinstance(entries, list):
                continue
            forms = []
            for entry in entries:
                report.entries += 1
                if not isinstance(entry, str) or not entry.strip():
                    report.add("malformed", relative, f"{where}/{key}", repr(entry))
                    continue
                form, problems = classify(entry, registry, tree)
                forms.append(form)
                for cls, _detail in problems:
                    report.add(cls, relative, f"{where}/{key}", entry)
            if key != "sources" or not forms:
                continue
            report.facts += 1
            if mapping.get("provenance") == "vendor-source":
                report.add("vendor-source", relative, where, "; ".join(map(str, entries)))
            if all(form == "qemu-code" for form in forms):
                report.add("qemu-code-only", relative, where, "; ".join(entries))
            elif all(form == "free-text" for form in forms):
                report.add("free-text-only", relative, where, "; ".join(entries))


def check(models: pathlib.Path = MODELS, tree: pathlib.Path = ROOT) -> Report:
    registry = load_registry(models)
    report = Report()
    for path in sorted(models.rglob("*.yaml")):
        if path.name != REGISTRY:
            check_file(path, models, registry, tree, report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--models", type=pathlib.Path, default=MODELS)
    parser.add_argument("--tree", type=pathlib.Path, default=ROOT)
    parser.add_argument("--list", metavar="CLASS", action="append", default=[],
                        help="also list every citation in CLASS")
    args = parser.parse_args(argv)
    report = check(args.models, args.tree)
    print(report.summary())
    for cls in args.list:
        print(f"\n{cls}:")
        for finding in report.findings.get(cls, []):
            print(f"  {finding.file}: {finding.where}: {finding.text}")
    for line in report.failures:
        print(f"citations: {line}", file=sys.stderr)
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
