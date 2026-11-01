# SPDX-License-Identifier: BSD-3-Clause

"""Render and check the "how to run it" pages of the SGI manual.

The running pages (docs/system/mips/sgi/running*.rst) say how to start a
machine. Two kinds of content keep them honest, and the rest is prose
that nothing checks.

Generated when the manual is built, by the ``sgi-run`` directive in
docs/sphinx/sgirun.py, from hw/mips/sgi/sgi-machines.json:

- the table of selections, the inputs and devices table, the graphics
  heads table, the short list of facts (accepted ``-cpu`` parts, memory,
  default topology) and the commands that create a product's stores and
  start it on them.

Checked by tests:

- ``static_problems`` parses every ``qemu-system-mips64`` and
  ``qemu-sgi-machine-init`` command written in a page and checks its
  machine, topology, node count, board, processor, ``-cpu``, ``-smp``,
  ``-m`` and store names against the catalogue. It needs no build.
- tests/functional/mips64/test_sgi_manual_commands.py starts every such
  command, and every command this module generates, on a built emulator
  with placeholder files (``start_argv``), and checks the refusals the
  pages state.
- ``test_references`` checks that each ``:test:`` role names a functional
  test in the mips64 meson.build lists.

Everything else on a page, the descriptions of what a PROM or guest does
and the wording of the table of guests a functional test boots, is
written by hand. A change to the catalogue does not change it and no test
reads it. Neither check boots a guest.
"""

from __future__ import annotations

import json
import pathlib
import re
import shlex
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parents[2]
CATALOGUE = ROOT / "hw/mips/sgi/sgi-machines.json"
PAGES = ROOT / "docs/system/mips/sgi"

# Each running page and the catalogue products it covers. A product in
# the catalogue that no page lists fails ``coverage_problems``.
FAMILIES = {
    "running-ip30": ("octane", "octane2"),
    "running-ip32": ("o2",),
    "running-ip27": ("origin200", "origin2000", "onyx2"),
    "running-ip35": ("origin300", "fuel", "origin3000"),
    "running-ip53": ("origin350", "onyx350", "tezro", "onyx4"),
}

# The file names the pages use for inputs the reader supplies, and the
# size of the placeholder the check substitutes for each. A name not
# listed here is refused in a page's command, so a page cannot name a
# file the check does not know how to stand in for.
PLACEHOLDERS = {
    "ip27prom.img": 1 << 20,
    "io6prom.img": 1 << 20,
    "ip30prom.bin": 1 << 20,
    "ip32prom.bin": 512 << 10,
    "ip35prom.img": 1476264,
    "disk.img": 1 << 20,
}

# The programs a page's code block may run and the check understands.
PROGRAMS = ("qemu-system-mips64", "qemu-sgi-machine-init")

KIND_NAMES = {
    "ip27-prom": "the IP27 PROM image",
    "io6-prom": "the I/O PROM image",
    "ip30-prom": "the IP30 PROM image",
    "ip32-prom": "the IP32 PROM image",
    "ip35-prom": "the IP35 PROM image",
}
KIND_FILES = {
    "ip27-prom": "ip27prom.img",
    "io6-prom": "io6prom.img",
    "ip30-prom": "ip30prom.bin",
    "ip32-prom": "ip32prom.bin",
    "ip35-prom": "ip35prom.img",
}
INPUT_OPTIONS = {"boot-prom": "--boot-prom", "io-prom": "--io-prom"}

CONSOLE_KINDS = {
    "console": "console",
    "second-port": "second serial port",
    "system-controller": "system controller console",
    "junk-uart": "manufacturing UART",
}
MEDIA_NAMES = {
    "disk-image": "a disk image on the SCSI bus",
    "netboot": "a network boot",
}


def load(path: pathlib.Path = CATALOGUE) -> dict:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def offerings(cat: dict, product: str) -> list[dict]:
    return [o for o in cat["offerings"] if o["product"] == product]


def products(cat: dict) -> list[str]:
    seen: dict[str, None] = {}
    for o in cat["offerings"]:
        seen.setdefault(o["product"], None)
    return list(seen)


def topologies(cat: dict, product: str) -> list[str]:
    seen: dict[str, None] = {}
    for o in offerings(cat, product):
        seen.setdefault(o["topology"], None)
    return list(seen)


def with_property(selection: str, prop: str, value: str) -> str:
    return f"{selection},{prop}={value}"


def selection_without(options: str, prop: str) -> str:
    return ",".join(p for p in options.split(",")
                    if not p.startswith(prop + "="))


def property_of(options: str, prop: str) -> str | None:
    for p in options.split(",")[1:]:
        if p.startswith(prop + "="):
            return p.split("=", 1)[1]
    return None


def ranges(values: list[int]) -> str:
    """1, 2, 3, 5 as "1-3, 5"."""
    out: list[str] = []
    values = sorted(values)
    i = 0
    while i < len(values):
        j = i
        while j + 1 < len(values) and values[j + 1] == values[j] + 1:
            j += 1
        out.append(str(values[i]) if i == j else f"{values[i]}-{values[j]}")
        i = j + 1
    return ", ".join(out)


def size_text(n: int) -> str:
    for unit, shift in (("MiB", 20), ("KiB", 10)):
        if n >= 1 << shift and n % (1 << shift) == 0:
            return f"{n >> shift} {unit}"
    return f"{n} bytes"


def default_offering(cat: dict, product: str) -> dict:
    """The offering a reader gets by naming the product and nothing else:
    the product's default topology at its most nodes and its shipped
    processor, or the first plain offering where the product has no
    default topology."""
    offs = offerings(cat, product)
    plain = [o for o in offs
             if property_of(o["machine-options"], "processor") is None]
    default = offs[0].get("default-topology")
    named = [o for o in plain if o["topology"] == default]
    if named:
        return max(named, key=lambda o: o["nodes"])
    return plain[0] if plain else offs[0]


# ---- rendering -----------------------------------------------------------

def table(header: list[str], rows: list[list[str]], widths=None) -> str:
    out = [".. list-table::", "   :header-rows: 1"]
    if widths:
        out.append("   :widths: " + " ".join(str(w) for w in widths))
    out.append("")
    for line in (header, *rows):
        for i, cell in enumerate(line):
            out.append(("   * - " if i == 0 else "     - ") + cell)
    out.append("")
    return "\n".join(out)


def literal(text: str) -> str:
    return f"``{text}``"


def processor_text(o: dict) -> str:
    p = o["processor"]
    return f"{p['part']} {p['clock-hz'] // 1000000} MHz"


def machines_table(cat: dict, product: str) -> str:
    """One row per selection the catalogue offers for the product. The
    processor modules a selection takes are listed in the row, each as
    the ``processor=`` value that picks it."""
    groups: dict[str, dict] = {}
    for o in offerings(cat, product):
        base = selection_without(o["machine-options"], "processor")
        g = groups.setdefault(base, {"o": o, "smp": set(), "cpus": {},
                                     "mem": set()})
        g["smp"].add(o["smp"])
        g["cpus"].setdefault(o["processor"]["id"], o)
        g["mem"].update(o["memory-per-node-mib"]["accepted"])
        if property_of(o["machine-options"], "processor") is None:
            g["o"] = o
    rows = []
    for base, g in groups.items():
        o = g["o"]
        default = o["processor"]["id"]
        pick = len(g["cpus"]) > 1
        cpus = [(f"{literal(i)}, " if pick else "") + processor_text(c)
                + (" (default)" if pick and i == default else "")
                for i, c in g["cpus"].items()]
        mem = sorted(g["mem"])
        span = (f"{mem[0]}" if len(mem) == 1
                else f"{o['memory-per-node-mib']['default']} default, "
                f"{mem[0]} to {mem[-1]}")
        rows.append([
            literal(base),
            o["display-name"],
            "\n       ".join("- " + c for c in cpus)
            if len(cpus) > 1 else cpus[0],
            ", ".join(str(n) for n in sorted(g["smp"])),
            span,
        ])
    return table(["``-M``", "Name", "Processor module",
                  "``-smp``", "Memory per node, MiB"], rows,
                 [30, 18, 28, 8, 16])


def inputs_table(cat: dict, product: str) -> str:
    o = default_offering(cat, product)
    rows = []
    boot = []
    for item in o["init-inputs"]:
        what = KIND_NAMES.get(item["kind"], item["kind"])
        need = "required" if item["required"] else "optional"
        boot.append(f"{what} ({need})")
    rows.append(["Firmware", "; ".join(boot)])
    if o.get("identity-inputs"):
        rows.append(["Identity inputs", ", ".join(
            literal(i) for i in o["identity-inputs"])])
    if o.get("hardware-inputs"):
        rows.append(["Hardware inputs", ", ".join(
            literal(h["option"]) + (" (required)" if h["required"]
                                    else " (optional)")
            for h in o["hardware-inputs"])])
    if o["storage"]:
        rows.append(["Writable stores", ", ".join(
            f"{literal(s['name'])} {size_text(s['size'])}"
            for s in o["storage"])])
    ports = [CONSOLE_KINDS.get(c["kind"], c["kind"]) for c in o["consoles"]]
    rows.append(["Serial ports, in ``-serial`` order", ", ".join(
        f"{i + 1}: {p}" for i, p in enumerate(ports))])
    scsi = [f"{literal(a['bus'])} ({a['type']}, targets "
            f"{ranges(a['targets']) or 'none'})" for a in o["scsi-adapters"]]
    rows.append(["Disk buses", "; ".join(scsi)])
    nets = [f"{literal(n['nic-model'])}, seen by the guest as "
            f"{literal(n['interface'])}" for n in o["networks"]]
    rows.append(["Onboard network", "; ".join(nets)])
    media = [MEDIA_NAMES.get(m, m) for m in o["install-media"]]
    rows.append(["Install media the catalogue lists",
                 "; ".join(media) if media else "none"])
    return table(["", "Catalogue entry for " + o["display-name"]], rows,
                 [25, 75])


def graphics_table(cat: dict, product: str) -> str:
    tops = {t["name"]: t for t in cat["topologies"]}
    rows = []
    for name in topologies(cat, product):
        g = tops[name].get("graphics") or {}
        slots = g.get("slots") or []
        if not slots:
            continue
        default = []
        optional: dict[str, list[str]] = {}
        for s in slots:
            devices = {f["family"]: f["device"] for f in s["families"]}
            if s.get("default"):
                d = s["default"]
                default.append(f"{literal(devices[d['family']])} "
                               f"product {literal(d['product'])} in "
                               f"{literal(s['name'])}")
            for f in s["families"]:
                optional.setdefault(f["device"], []).append(s["name"])
        fits = "; ".join(
            f"{literal(dev)} in " + ", ".join(literal(n) for n in names)
            for dev, names in optional.items())
        rows.append([
            literal(name),
            "; ".join(default) or "none",
            fits,
        ])
    if not rows:
        return ""
    return table(["Topology", "Head fitted by default",
                  "Devices a slot takes"], rows, [20, 30, 50])


def facts_list(cat: dict, product: str) -> str:
    """The catalogue facts a page would otherwise state by hand."""
    offs = offerings(cat, product)
    parts = sorted({o["processor"]["part"] for o in offs})
    mem = sorted({m for o in offs
                  for m in o["memory-per-node-mib"]["accepted"]})
    bullets = ["``-cpu`` names " + (parts[0] if len(parts) == 1 else
                                    "one of " + ", ".join(parts)) + "."]
    if len(mem) == 1:
        bullets.append(f"Memory is exactly {mem[0]} MiB per node.")
    else:
        bullets.append(f"Memory is {mem[0]} to {mem[-1]} MiB per node.")
    if offs[0].get("default-topology"):
        bullets.append("Without ``topology=`` the machine builds "
                       + literal(offs[0]["default-topology"]) + ".")
    else:
        bullets.append("The product has no default topology: the machine "
                       "refuses a launch without ``topology=``.")
    return "\n".join(f"- {b}" for b in bullets) + "\n"


def launch_commands(cat: dict, product: str) -> list[str]:
    """The two commands that create a product's stores and start it on
    them, for its default selection."""
    o = default_offering(cat, product)
    if not o["storage"] or any(i["kind"] not in KIND_FILES
                               for i in o["init-inputs"]) \
            or "firmware" in o and o["firmware"]["kind"] == "ip32-prom":
        raise ValueError(f"{product} is not started on stores that "
                         "qemu-sgi-machine-init creates")
    options = o["machine-options"]
    init = ["qemu-sgi-machine-init", "--machine", options]
    for item in o["init-inputs"]:
        if item["required"]:
            init += [INPUT_OPTIONS[item["name"]], KIND_FILES[item["kind"]]]
    init.append("state")
    binds = ",".join(f"{s['name']}={s['name']}" for s in o["storage"])
    run = ["qemu-system-mips64", "-M", f"{options},{binds}"]
    for s in o["storage"]:
        run += ["-blockdev", f"driver=file,filename=state/{s['name']}.raw,"
                f"node-name={s['name']}"]
    run += ["-display", "none", "-nodefaults", "-monitor", "none",
            "-serial", "stdio"]
    return [shlex.join(init), shlex.join(run)]


NO_VALUE = ("-nodefaults", "-nographic", "-S")


def wrapped(words: list[str], width: int = 70) -> list[str]:
    """Shell lines for a command: an option stays with its value, and a
    line breaks before the next piece once it would pass width."""
    pieces = [[words[0]]]
    i = 1
    while i < len(words):
        w = words[i]
        if w.startswith("-") and w not in NO_VALUE and i + 1 < len(words):
            pieces.append([w, words[i + 1]])
            i += 2
        else:
            pieces.append([w])
            i += 1
    lines, cur = [], ""
    for piece in pieces:
        text = " ".join(shlex.quote(w) for w in piece)
        if cur and len(cur) + 1 + len(text) > width:
            lines.append(cur)
            cur = "    " + text
        else:
            cur = f"{cur} {text}" if cur else text
    lines.append(cur)
    return [ln + " \\" for ln in lines[:-1]] + [lines[-1]]


def launch_block(cat: dict, product: str) -> str:
    out = [".. code-block:: shell", ""]
    for cmd in launch_commands(cat, product):
        out += ["   " + ln for ln in wrapped(shlex.split(cmd))] + [""]
    return "\n".join(out)


RENDERERS = {
    "machines": machines_table,
    "inputs": inputs_table,
    "graphics": graphics_table,
    "facts": facts_list,
    "launch": launch_block,
}


def render(kind: str, product: str, cat: dict | None = None) -> str:
    cat = cat or load()
    if product not in products(cat):
        raise ValueError(f"the catalogue has no product {product!r}")
    return RENDERERS[kind](cat, product)


# ---- reading the commands a page contains --------------------------------

def code_blocks(text: str) -> list[str]:
    """The body of each shell code block and literal block in a page."""
    lines = text.split("\n")
    blocks = []
    i = 0
    while i < len(lines):
        line = lines[i]
        starts = re.match(r"\s*\.\. code-block:: (shell|sh)\s*$", line) or \
            (line.rstrip().endswith("::") and
             not re.match(r"\s*\.\. ", line))
        if not starts:
            i += 1
            continue
        indent = len(line) - len(line.lstrip())
        j = i + 1
        body = []
        while j < len(lines) and (not lines[j].strip() or
                                  len(lines[j]) - len(lines[j].lstrip())
                                  > indent):
            body.append(lines[j])
            j += 1
        blocks.append("\n".join(body))
        i = j
    return blocks


def command_blocks(text: str) -> list[list[list[str]]]:
    """The commands of each code block that runs one of PROGRAMS, one list
    of argument vectors per block. A block is one session: its
    qemu-sgi-machine-init commands create what its qemu-system-mips64
    command binds."""
    out = []
    for block in code_blocks(text):
        found = commands_of(block)
        if found:
            out.append(found)
    return out


def commands(text: str) -> list[list[str]]:
    """Every command in the page's code blocks that runs one of PROGRAMS,
    as an argument vector."""
    return [c for block in command_blocks(text) for c in block]


def commands_of(block: str) -> list[list[str]]:
    """The PROGRAMS commands of one code block."""
    # As the shell reads it: the block's own indentation is gone, and a
    # backslash and newline vanish without leaving a space.
    joined = textwrap.dedent(block).replace("\\\n", "")
    found = []
    for line in joined.split("\n"):
        line = line.strip()
        if not line:
            continue
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        if words and words[0] in PROGRAMS:
            found.append(words)
    return found


def option_values(argv: list[str], *names: str) -> list[str]:
    return [argv[i + 1] for i, a in enumerate(argv[:-1]) if a in names]


# ---- checks that need no build -------------------------------------------

def _cpu_parts(cat: dict, product: str) -> set[str]:
    return {o["processor"]["part"] for o in offerings(cat, product)}


def _selected(cat: dict, options: str) -> list[dict]:
    """The offerings a -M value could mean. A property the value leaves out
    takes the machine's default: the product's default topology, the most
    nodes the topology accepts and its shipped processor."""
    name = options.split(",")[0]
    offs = offerings(cat, name)
    if not offs:
        return []
    topology = property_of(options, "topology") or offs[0].get(
        "default-topology")
    if topology:
        offs = [o for o in offs if o["topology"] == topology]
    nodes = property_of(options, "nodes")
    if nodes is not None:
        offs = [o for o in offs if str(o["nodes"]) == nodes]
    elif offs:
        most = max(o["nodes"] for o in offs)
        offs = [o for o in offs if o["nodes"] == most]
    board = property_of(options, "board")
    if board is not None:
        offs = [o for o in offs if o.get("board") == board]
    processor = property_of(options, "processor")
    if processor is not None:
        offs = [o for o in offs
                if property_of(o["machine-options"], "processor")
                == processor or (o["processor"]["id"] == processor
                                 and property_of(o["machine-options"],
                                                 "processor") is None)]
    else:
        offs = [o for o in offs
                if property_of(o["machine-options"], "processor") is None]
    return offs


def mem_mib(value: str) -> int:
    m = re.fullmatch(r"(\d+)([KMGT]?)", value.upper())
    if not m:
        raise ValueError(value)
    n, unit = int(m.group(1)), m.group(2)
    if unit == "":
        return n
    return n * {"K": 1, "M": 1, "G": 1024, "T": 1 << 20}[unit] // (
        1024 if unit == "K" else 1)


def machine_problems(cat: dict, argv: list[str], where: str) -> list[str]:
    out = []
    selections = option_values(argv, "-M", "-machine")
    if argv[0] == "qemu-sgi-machine-init":
        selections = option_values(argv, "-M", "--machine")
    if not selections:
        return [f"{where}: no -M selection in {shlex.join(argv)}"]
    options = selections[0]
    name = options.split(",")[0]
    if name not in products(cat):
        return [f"{where}: {name!r} is not a catalogue product"]
    for prop in ("topology", "nodes", "board", "processor"):
        value = property_of(options, prop)
        known = {
            "topology": set(topologies(cat, name)),
            "nodes": {str(o["nodes"]) for o in offerings(cat, name)},
            "board": {o.get("board") for o in offerings(cat, name)},
            "processor": {o["processor"]["id"] for o in offerings(cat, name)},
        }[prop]
        if value is not None and value not in known:
            out.append(f"{where}: {prop}={value} is not offered for {name}")
    offs = _selected(cat, options)
    if not offs:
        return out + [f"{where}: {options} matches no catalogue offering"]
    for cpu in option_values(argv, "-cpu"):
        if cpu.split(",")[0] not in _cpu_parts(cat, name):
            out.append(f"{where}: -cpu {cpu} is not a processor of {name}")
    for smp in option_values(argv, "-smp"):
        count = smp.split(",")[0]
        if count.isdigit() and int(count) not in {o["smp"] for o in offs}:
            out.append(f"{where}: -smp {count} is not offered for {options}")
    stores = {s["name"] for o in offs for s in o["storage"]}
    for prop in options.split(",")[1:]:
        key = prop.split("=")[0]
        if re.fullmatch(r"(node|io)\d+-flash|nvram\d+(-clock)?|module\d+-nvram",
                        key) and key not in stores:
            out.append(f"{where}: {key} is not a store of {options}")
    for mem in option_values(argv, "-m"):
        nodes = {o["nodes"] for o in offs}
        if len(nodes) == 1:
            n = nodes.pop()
            total = mem_mib(mem.split(",")[0])
            accepted = {a for o in offs
                        for a in o["memory-per-node-mib"]["accepted"]}
            if total % n or total // n not in accepted:
                out.append(f"{where}: -m {mem} gives {total / n:g} MiB per "
                           f"node, which {options} does not accept")
    return out


FILE_WORD = re.compile(r"[\w.-]+\.(?:img|bin|raw|qcow2)\Z")


def file_problems(argv: list[str], where: str) -> list[str]:
    out = []
    for word in argv:
        for piece in re.split(r"[=,]", word):
            if FILE_WORD.match(piece) and piece not in PLACEHOLDERS \
                    and not word.startswith("filename=state/") \
                    and not piece.startswith("state/"):
                out.append(f"{where}: {piece!r} has no placeholder; use a "
                           "name from PLACEHOLDERS")
    return out


def static_problems(text: str, name: str, cat: dict | None = None) -> list[str]:
    cat = cat or load()
    out = []
    for n, argv in enumerate(commands(text), 1):
        where = f"{name} command {n}"
        out += machine_problems(cat, argv, where)
        out += file_problems(argv, where)
    return out


def coverage_problems(pages: dict[str, str], cat: dict | None = None) -> list[str]:
    """Every catalogue product has a running page, each page uses the
    generated tables for each of its products, and no product is claimed
    twice."""
    cat = cat or load()
    out = []
    covered: dict[str, str] = {}
    for page, items in FAMILIES.items():
        for product in items:
            if product in covered:
                out.append(f"{product} is listed for {covered[product]} "
                           f"and {page}")
            covered[product] = page
            if product not in products(cat):
                out.append(f"{page}: {product} is not a catalogue product")
                continue
            used = set(re.findall(
                r"\.\. sgi-run:: (\w+) " + re.escape(product) + r"\s*$",
                pages.get(page, ""), re.M))
            for kind in ("machines", "inputs"):
                if kind not in used:
                    out.append(f"{page}: no 'sgi-run:: {kind} {product}'")
    for product in products(cat):
        if product not in covered:
            out.append(f"catalogue product {product} has no running page")
    return out


TEST_REFERENCE = re.compile(r":test:`(\w+)`")
MESON = ROOT / "tests/functional/mips64/meson.build"


def test_lists(meson_text: str | None = None) -> dict[str, list[str]]:
    """The mips64 functional tests by suite, from the meson.build lists
    ``tests_mips64_system_quick`` and ``tests_mips64_system_thorough``.
    Quick tests run in a plain CI run. Thorough tests run in the nightly
    set, and those that boot a guest skip without the SGI asset
    directory."""
    text = meson_text if meson_text is not None else MESON.read_text()
    out = {}
    for suite in ("quick", "thorough"):
        m = re.search(r"^tests_mips64_system_" + suite
                      + r"\s*(\+?=)\s*\[(.*?)\]", text, re.M | re.S)
        body = m.group(2) if m else ""
        body = re.sub(r"#[^\n]*", "", body)
        out[suite] = re.findall(r"'(\w+)'", body)
    # Entries appended after the definition, as `+= ['name']`.
    for suite, name in re.findall(
            r"tests_mips64_system_(quick|thorough)\s*\+=\s*\[\s*'(\w+)'",
            text):
        out[suite].append(name)
    return out


def test_suite(name: str, meson_text: str | None = None) -> str | None:
    for suite, names in test_lists(meson_text).items():
        if name in names:
            return suite
    return None


def test_references(text: str, root: pathlib.Path = ROOT) -> list[str]:
    """Each ``:test:`name``` in a page names a functional test file that
    exists and sits in one of the mips64 meson.build test lists."""
    out = []
    meson = (root / "tests/functional/mips64/meson.build").read_text()
    for name in sorted(set(TEST_REFERENCE.findall(text))):
        if not (root / f"tests/functional/mips64/test_{name}.py").is_file():
            out.append(f"test_{name}.py does not exist")
        if test_suite(name, meson) is None:
            out.append(f"{name} is in neither tests_mips64_system list in "
                       "tests/functional/mips64/meson.build")
    return out


# ---- starting a command for the build-backed check ----------------------

def start_argv(argv: list[str]) -> list[str]:
    """A page's qemu-system-mips64 command made safe to run unattended:
    paused at reset, with a QMP monitor on standard input and output, and
    the serial port and monitor that the page sent to standard input and
    output sent nowhere."""
    out = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-nographic":
            out += ["-display", "none", "-serial", "null"]
        elif a in ("-serial", "-monitor") and argv[i + 1] == "stdio":
            out += [a, "null"]
            i += 1
        else:
            out.append(a)
        i += 1
    if "-monitor" not in out:
        out += ["-monitor", "none"]
    return out + ["-S", "-qmp", "stdio"]


if __name__ == "__main__":
    kind, product = sys.argv[1:3]
    print(render(kind, product))
