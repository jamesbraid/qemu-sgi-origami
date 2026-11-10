#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Keep the topology diagrams of the SGI platform pages in step with the
machine catalogue.

The pages draw each topology with ``.. sgi-diagram::`` when the manual is
built (scripts/sgi_model/topology_diagram.py). These tests read the
catalogue's JSON directly, work out for themselves which nodes, routers,
crossbars, boards, slots, cables and widgets each topology has, and check
that the picture names exactly those: none missing and none added. They
also check that changing the catalogue changes the picture, that every
platform page draws every product it covers, and the picture's
accessibility, size and determinism. They need no build.
"""

import copy
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import topology_diagram as td

CATALOGUE = ROOT / "hw/mips/sgi/sgi-machines.json"
PAGES = ROOT / "docs/system/mips/sgi"
SVG = "{http://www.w3.org/2000/svg}"
# The most a single diagram may weigh, as the manual's guide promises.
MAX_BYTES = 30 * 1024


def load():
    with open(CATALOGUE, encoding="utf-8") as f:
        return json.load(f)


CAT = load()
TOPOLOGIES = {t["name"]: t for t in CAT["topologies"]}


def expected(cat, name):
    """What the catalogue says topology ``name`` has, by kind, and the
    widget numbers it states, worked out here from the JSON alone."""
    t = next(t for t in cat["topologies"] if t["name"] == name)
    boards = t["io-boards"]
    slots = {n for _, n, _ in slot_links(t)}
    modules = set()
    if t["modules"] and (len(t["modules"]) > 1
                         or t["modules"][0]["id"] != "implicit-module"):
        modules = {m["id"] for m in t["modules"]}
    # Lists, not sets: two boards may share an id in different modules.
    out = {
        "node": [n["id"] for n in t["nodes"]],
        "io-peer": [p["id"] for p in t.get("io-peers") or []],
        "router": [r["id"] for r in t["routers"]],
        "crossbar": [x["id"] for x in t["crossbars"]],
        "pic": [p["id"] for p in t.get("pci-host-bridges") or []],
        "board": [b["id"] for b in boards],
        "slot": sorted(slots),
        "controller": [c["id"] for c in t["controllers"]],
        "cable": [c["name"] for c in t["cables"]],
        "xio-cable": [c["name"] for c in t.get("xio-cables") or []],
        "module": sorted(modules),
    }
    return out, sorted(set_widgets(t))


def set_widgets(t):
    """The widget number of each attachment and each slot, once each."""
    out = [a["widget"] for a in t["xio-attachments"]]
    return out + [w for _, _, w in slot_links(t)]


def slot_links(t):
    """(crossbar id, slot name, widget) for each slot the catalogue puts
    on a crossbar: the xio-slots entries, which name their crossbar, and
    the ``widget-N`` slots of a topology with one crossbar, which name
    only the widget. A slot a board fills is that board's, so it is left
    to ``board_links``."""
    labels = {b["label"] for b in t["io-boards"] if b.get("label")}
    xs = {s["label"]: s for s in t["xio-slots"]}
    names = list(dict.fromkeys(
        [s["name"] for s in t.get("board-slots") or []] + list(xs)))
    out = []
    for n in names:
        if n in labels:
            continue
        if n in xs:
            out.append((t["crossbars"][xs[n]["crossbar-index"]]["id"], n,
                        xs[n]["widget"]))
        elif re.fullmatch(r"widget-[0-9]+", n) and len(t["crossbars"]) == 1:
            out.append((t["crossbars"][0]["id"], n, int(n.split("-")[1])))
    return out


def board_entries(t):
    """(xio-attachments index, board id) for each board an attachment
    places, by what the machine builder does for the node's ASIC
    (hw/mips/sgi/sn-machine.c). Hub: a node's first entry behind a
    crossbar is its host link, and each later entry for it, like each
    entry with no crossbar, places the next board in ``io-boards`` order.
    Bedrock: a board's ``attachment`` field is the index of its entry.
    HEART (Octane): the builder hardwires widgets 8 and 15, which the
    catalogue's two entries repeat, so the Hub rule applies. A topology
    with PCI host bridges places its boards through them."""
    if t.get("pci-host-bridges"):
        return []
    atts, boards = t["xio-attachments"], t["io-boards"]
    if t["nodes"][0]["asic-kind"] == "bedrock":
        return [(b["attachment"], b["id"]) for b in boards
                if isinstance(b.get("attachment"), int)
                and 0 <= b["attachment"] < len(atts)]
    seen, owners = set(), []
    for i, a in enumerate(atts):
        if a["crossbar-index"] < 0 or a["node-index"] in seen:
            owners.append(i)
        seen.add(a["node-index"])
    return [(i, b["id"]) for i, b in zip(owners, boards)]


def entry_end(t, a):
    """The id an attachment entry meets: its crossbar, or its node."""
    if a["crossbar-index"] >= 0:
        return t["crossbars"][a["crossbar-index"]]["id"]
    return t["nodes"][a["node-index"]]["id"]


def board_links(t):
    """(from id, board id, widget) for each board placed by an entry."""
    return [(entry_end(t, t["xio-attachments"][i]), board,
             t["xio-attachments"][i]["widget"])
            for i, board in board_entries(t)]


def edges(t):
    """Every link the catalogue gives, as (kind, end, end, widget or
    name), ends unordered, worked out from the JSON alone."""
    atts, out = t["xio-attachments"], []
    node = lambda i: t["nodes"][i]["id"]
    xbar = lambda i: t["crossbars"][i]["id"]
    bedrock = t["nodes"][0]["asic-kind"] == "bedrock"
    used = {i for i, _ in board_entries(t)}
    seen = set()
    for i, a in enumerate(atts):
        # A Bedrock's entries are its XBridge heads' widgets, not a link.
        if (not bedrock and a["crossbar-index"] >= 0
                and a["node-index"] not in seen):
            out.append(("widget", node(a["node-index"]),
                        xbar(a["crossbar-index"]), a["widget"]))
            used.add(i)
        seen.add(a["node-index"])
    for p in t.get("pci-host-bridges") or []:
        i = p["xio-attachment-index"]
        out.append(("widget", node(atts[i]["node-index"]), p["id"],
                    atts[i]["widget"]))
        used.add(i)
    for src, board, w in board_links(t):
        out.append(("widget", src, board, w))
    # An entry nothing above uses ends in a stub where it meets.
    for i, a in enumerate(atts):
        if i not in used:
            end = entry_end(t, a) if bedrock else node(a["node-index"])
            out.append(("widget", end, end, a["widget"]))
    for src, slot, w in slot_links(t):
        out.append(("widget", src, slot, w))
    for b in t["io-boards"]:
        if b.get("pci-host-index") is not None:
            out.append(("attach",
                        t["pci-host-bridges"][b["pci-host-index"]]["id"],
                        b["id"], None))
    pools = {"node": [n["id"] for n in t["nodes"]],
             "router": [r["id"] for r in t["routers"]],
             "io-peer": [p["id"] for p in t.get("io-peers") or []],
             "crossbar": [x["id"] for x in t["crossbars"]],
             "io-board": [b["id"] for b in t["io-boards"]]}
    for kind, key in (("cable", "cables"), ("xio-cable", "xio-cables")):
        for c in t.get(key) or []:
            ends = [pools[e["kind"]][e["index"]] for e in c["ends"]]
            out.append((kind, ends[0], ends[1], c["name"]))
    return sorted((k, *sorted((a, b)), str(w)) for k, a, b, w in out)


def offered(cat, name):
    """The device lines a topology's offerings give, as the picture lists
    them, or nothing where a board states PCI functions."""
    t = next(t for t in cat["topologies"] if t["name"] == name)
    if any(b.get("pci-functions") for b in t["io-boards"]):
        return set()
    out = set()
    for o in cat["offerings"]:
        if o["topology"] != name:
            continue
        audio = o["devices"].get("audio")
        if audio:
            out.add(f"{audio['device']} audio")
        out |= {f"{n['nic-model']} network" for n in o["networks"]}
        out |= {f"{s['bus']} scsi" for s in o["scsi-adapters"]}
        out |= {f"{s['name']} serial" for s in o["serial-lines"]
                if s["kind"] != "record"}
    return out


def parse(markup):
    return ET.fromstring(markup)


def drawn(root):
    """What the picture names, by kind, and its widget numbers."""
    kinds = {}
    for g in root.iter(SVG + "g"):
        kind = g.get("data-kind")
        if kind and g.get("data-id") is not None:
            kinds.setdefault(kind, []).append(g.get("data-id"))
    widgets = sorted(int(g.get("data-widget")) for g in root.iter(SVG + "g")
                     if g.get("data-widget"))
    return kinds, widgets


def drawn_edges(root):
    """Each link the picture draws, as ``edges`` lists the catalogue's."""
    out = []
    for g in root.iter(SVG + "g"):
        if g.get("class") != "link":
            continue
        kind = g.get("data-kind")
        if kind == "widget":
            last = g.get("data-widget")
        elif kind in ("cable", "xio-cable"):
            last = g.get("data-id")
        else:
            last = "None"
        out.append((kind, *sorted((g.get("data-a"), g.get("data-b"))), last))
    return sorted(out)


def problems(drawn_cat, want_cat, name):
    """How the picture of ``name`` drawn from ``drawn_cat`` differs from
    what ``want_cat`` says: parts, links and widgets."""
    root = parse(td.svg(drawn_cat, name))
    want, want_widgets = expected(want_cat, name)
    got, got_widgets = drawn(root)
    out = []
    for kind in ("node", "io-peer", "router", "crossbar", "pic", "board",
                 "slot", "controller", "cable", "xio-cable", "module"):
        if sorted(got.get(kind, [])) != sorted(want[kind]):
            out.append(f"{kind}: drew {sorted(got.get(kind, []))}, "
                       f"catalogue has {sorted(want[kind])}")
    if got_widgets != want_widgets:
        out.append(f"widgets: drew {got_widgets}, catalogue has "
                   f"{want_widgets}")
    t = next(t for t in want_cat["topologies"] if t["name"] == name)
    if drawn_edges(root) != edges(t):
        out.append(f"links: drew {drawn_edges(root)}, catalogue gives "
                   f"{edges(t)}")
    return out


def texts(root):
    return [t.text for t in root.iter(SVG + "text")]


class DiagramMatchesCatalogue(unittest.TestCase):
    def test_every_topology_draws_exactly_the_catalogue_parts_and_links(self):
        for name in TOPOLOGIES:
            with self.subTest(topology=name):
                self.assertEqual(problems(CAT, CAT, name), [])
                # Whatever else is drawn is a link or the device box.
                got, _ = drawn(parse(td.svg(CAT, name)))
                self.assertLessEqual(
                    set(got), {"node", "io-peer", "router", "crossbar", "pic",
                               "board", "slot", "controller", "cable",
                               "xio-cable", "module", "widget", "attach",
                               "devices"})

    def widgets_of(self, name, *ends):
        """The widget numbers of the drawn links between the named ends."""
        return sorted(
            int(e[3]) for e in drawn_edges(parse(td.svg(CAT, name)))
            if e[0] == "widget" and e[1:3] == tuple(sorted(ends)))

    def test_octane_widgets_are_the_ones_the_page_states(self):
        # ip30.rst: HEART at Crossbow widget 8, the onboard Bridge at 15.
        for name in ("octane", "octane2"):
            self.assertEqual(self.widgets_of(name, "heart", "crossbow"), [8])
            self.assertEqual(self.widgets_of(name, "crossbow", "ip30-board"),
                             [15])
        # The widget-N slots are attached to the one Crossbow by name.
        self.assertEqual(
            sorted(int(e[3]) for e in edges(TOPOLOGIES["octane"])
                   if e[2].startswith("widget-")), [9, 10, 11, 12, 13])

    def test_onyx2_deskside_matches_its_page_table(self):
        # ip27.rst deskside widget map: the pipe in io4 at 8, node board 2
        # at 9, node board 1 at A, io2 at B, io6 at C, io5 at D, io3 at E,
        # the BaseIO-G in io1 at F.
        n = "onyx2-deskside"
        got = drawn_edges(parse(td.svg(CAT, n)))
        want = {("node-board-2-hub", "crossbow-0"): 9,
                ("node-board-1-hub", "crossbow-0"): 10,
                ("baseio", "crossbow-0"): 15,
                ("crossbow-0", "io4-pipe"): 8,
                ("crossbow-0", "io2"): 11, ("crossbow-0", "io6"): 12,
                ("crossbow-0", "io5"): 13, ("crossbow-0", "io3"): 14}
        for ends, widget in want.items():
            self.assertIn(("widget", *sorted(ends), str(widget)), got, ends)

    def test_rack_compute_module_matches_its_page_table(self):
        # ip27.rst rack module: N1 at Crossbow 0 widget 9, N2 at Crossbow 1
        # widget A, N3 at Crossbow 0 widget A and N4 at Crossbow 1 widget
        # 9; on Crossbow 0, widget 8 is io1.
        for n in ("onyx2-rack", "origin2000-rack"):
            got = drawn_edges(parse(td.svg(CAT, n)))
            for ends, widget in {("node-board-1-hub", "crossbow-0"): 9,
                                 ("node-board-2-hub", "crossbow-1"): 10,
                                 ("node-board-3-hub", "crossbow-0"): 10,
                                 ("node-board-4-hub", "crossbow-1"): 9,
                                 ("baseio", "crossbow-0"): 8}.items():
                self.assertIn(("widget", *sorted(ends), str(widget)), got,
                              (n, ends))

    def test_cabled_bedrocks_draw_xbridge_heads_e_and_f(self):
        # A cabled Bedrock's attachment entries are widgets 14 and 15 of
        # its XBridge (heads E and F), not a labelled link from the node:
        # the node reaches the crossbar by its XIO cable alone.
        for n in ("origin3400", "onyx3400-ir", "origin3200", "origin3800"):
            got = drawn_edges(parse(td.svg(CAT, n)))
            xb = "ibrick-xbridge-crossbar"
            self.assertIn(("widget", xb, xb, "14"), got, n)
            self.assertIn(("widget", *sorted((xb, "ibrick-xbridge-bridge-f")),
                           "15"), got, n)
            self.assertFalse([e for e in got if e[0] == "widget"
                              and "c0-bedrock" in e[1:3]], n)
            self.assertIn(("xio-cable", *sorted(("c0-bedrock", xb)),
                           "c0-to-ibrick"), got, n)
        # The N-bricks of onyx3400-ir show both heads without a board.
        got = drawn_edges(parse(td.svg(CAT, "onyx3400-ir")))
        for k in range(3):
            xb = f"nbrick-xbridge{k}-crossbar"
            for w in ("14", "15"):
                self.assertIn(("widget", xb, xb, w), got)

    def test_origin300_and_fuel_heads_are_the_ones_the_pages_state(self):
        # ip35.rst: heads E and F at widgets 14 and 15; Fuel VPro at 13.
        self.assertEqual(self.widgets_of("origin300", "bedrock",
                                         "xbridge-bridge-e"), [14])
        self.assertEqual(self.widgets_of("origin300", "bedrock",
                                         "xbridge-bridge-f"), [15])
        self.assertEqual(self.widgets_of("fuel", "bedrock", "vpro-v12"), [13])

    def test_names_are_text_not_paths(self):
        for name in TOPOLOGIES:
            with self.subTest(topology=name):
                root = parse(td.svg(CAT, name))
                shown = texts(root)
                want, _ = expected(CAT, name)
                for kind, names in want.items():
                    if kind == "module":
                        continue  # a module is named in its heading
                    for ident in names:
                        self.assertTrue(
                            any(ident in t for t in shown),
                            f"{kind} {ident} is not drawn as text")
                # Each cable's name labels its link.
                for ident in want["cable"] + want["xio-cable"]:
                    self.assertIn(ident, [t.split(" port ")[0]
                                          for t in shown])
                for w in {w for w in expected(CAT, name)[1]}:
                    self.assertIn(f"widget {w}", shown)

    def test_pci_functions_and_devices_come_from_the_catalogue(self):
        for name, t in TOPOLOGIES.items():
            with self.subTest(topology=name):
                root = parse(td.svg(CAT, name))
                boxes = {g.get("data-id"): [x.text for x in g.iter(SVG + "text")]
                         for g in root.iter(SVG + "g")
                         if g.get("data-kind") == "board"}
                for b in t["io-boards"]:
                    for f in b.get("pci-functions", []):
                        self.assertTrue(
                            any(line.startswith(f["kind"])
                                for line in boxes[b["id"]]),
                            f"{b['id']} lacks {f['kind']}")
                devices = [[x.text for x in g.iter(SVG + "text")][1:]
                           for g in root.iter(SVG + "g")
                           if g.get("data-kind") == "devices"]
                want = offered(CAT, name)
                self.assertEqual({d for box in devices for d in box}, want)

    def test_octane_and_o2_show_what_the_catalogue_offers(self):
        octane = texts(parse(td.svg(CAT, "octane")))
        for part in ("heart", "crossbow", "ip30-board", "widget 8",
                     "widget 15", "widget-11", "sgi-ioc3-eth network",
                     "sgi-rad1-octane audio"):
            self.assertTrue(any(part in t for t in octane), part)
        o2 = texts(parse(td.svg(CAT, "o2")))
        self.assertTrue(any("crime" in t for t in o2))
        # GBE and VICE are not in the catalogue, so they are not drawn.
        self.assertFalse(any(re.search(r"\b(gbe|vice)\b", t.lower())
                             for t in o2))


class DiagramFollowsCatalogue(unittest.TestCase):
    def mutated(self, name, mutate):
        cat = copy.deepcopy(CAT)
        mutate(next(t for t in cat["topologies"] if t["name"] == name))
        return cat

    def test_a_changed_catalogue_fails_the_check_against_the_old_one(self):
        def attachment_crossbar(t):
            t["xio-attachments"][2]["crossbar-index"] = 1

        def cable_end(t):
            t["cables"][0]["ends"][1]["index"] = 2

        def slot_widget(t):
            next(s for s in t["xio-slots"]
                 if s["label"] == "io3")["widget"] = 2

        def router_rename(t):
            t["routers"][0]["id"] = "router-renamed"

        for label, mutate in (("attachment crossbar", attachment_crossbar),
                              ("cable end", cable_end),
                              ("slot widget", slot_widget),
                              ("router name", router_rename)):
            with self.subTest(change=label):
                cat = self.mutated("origin2000-deskside", mutate)
                self.assertEqual(
                    problems(cat, cat, "origin2000-deskside"), [])
                self.assertTrue(problems(cat, CAT, "origin2000-deskside"),
                                f"a changed {label} was not noticed")

    def test_a_board_entry_moved_to_another_widget_is_noticed(self):
        def move(t):
            t["xio-attachments"][1]["widget"] = 9

        cat = self.mutated("origin2000-deskside", move)
        self.assertTrue(problems(cat, CAT, "origin2000-deskside"))

    def render_without(self, name, mutate):
        return td.svg(self.mutated(name, mutate), name)

    def test_dropping_a_widget_slot_changes_the_diagram(self):
        for name, label in (("origin2000-deskside", "io3"),
                            ("onyx2-rack", "io9")):
            with self.subTest(topology=name):
                def drop(t):
                    t["xio-slots"] = [s for s in t["xio-slots"]
                                      if s["label"] != label]
                before = td.svg(CAT, name)
                after = self.render_without(name, drop)
                self.assertNotEqual(before, after)
                self.assertIn(f'data-id="{label}"', before)
                self.assertNotIn(f'data-id="{label}"', after)

    def test_dropping_an_ip30_widget_changes_the_diagram(self):
        def drop(t):
            t["board-slots"] = [s for s in t["board-slots"]
                                if s["name"] != "widget-12"]
        before = td.svg(CAT, "octane")
        after = self.render_without("octane", drop)
        self.assertIn("widget-12", before)
        self.assertNotIn("widget-12", after)

    def test_dropping_a_cable_node_or_attachment_changes_the_diagram(self):
        cases = (
            ("origin3400", lambda t: t.update(cables=t["cables"][1:]),
             "rbrick-port-2-cable"),
            ("tezro-ip53", lambda t: t.update(
                **{"xio-attachments": t["xio-attachments"][:1],
                   "pci-host-bridges": t["pci-host-bridges"][:1],
                   "io-boards": []}), "pic-xio-f"),
            ("origin2000-deskside", lambda t: t.update(
                nodes=t["nodes"][:3], cables=t["cables"][:3],
                modules=[dict(t["modules"][0], nodes=[0, 1, 2])],
                **{"xio-attachments": t["xio-attachments"][:3]}),
             "node-board-4-hub"),
        )
        for name, mutate, gone in cases:
            with self.subTest(topology=name):
                before = td.svg(CAT, name)
                after = self.render_without(name, mutate)
                self.assertIn(gone, before)
                self.assertNotIn(gone, after)

    def test_a_renamed_part_is_renamed_in_the_diagram(self):
        def rename(t):
            t["routers"][0]["id"] = "router-renamed"
        after = self.render_without("origin2000-deskside", rename)
        self.assertIn("router-renamed", after)
        self.assertNotIn(">router-1<", after)


class DiagramQuality(unittest.TestCase):
    def test_each_diagram_is_small_and_well_formed(self):
        for name in TOPOLOGIES:
            with self.subTest(topology=name):
                markup = td.svg(CAT, name)
                self.assertLess(len(markup.encode()), MAX_BYTES)
                parse(markup)

    def test_each_diagram_is_accessible_and_self_contained(self):
        for name in TOPOLOGIES:
            with self.subTest(topology=name):
                root = parse(td.svg(CAT, name))
                self.assertEqual(root.get("role"), "img")
                title, desc = (root.find(SVG + "title"),
                               root.find(SVG + "desc"))
                self.assertTrue(title.text and desc.text)
                self.assertEqual(root.get("aria-labelledby"),
                                 f"{title.get('id')} {desc.get('id')}")
                ids = [e.get("id") for e in root.iter() if e.get("id")]
                self.assertEqual(len(ids), len(set(ids)))
                tags = {e.tag.removeprefix(SVG) for e in root.iter()}
                self.assertFalse(tags & {"script", "image",
                                         "foreignObject", "use", "a"})
                # The description names every drawn part.
                for kind, idents in drawn(root)[0].items():
                    if kind in ("node", "router", "crossbar", "board"):
                        for ident in idents:
                            self.assertIn(ident, desc.text)

    def test_colours_are_a_small_palette_that_reads_on_both_themes(self):
        def luminance(colour):
            channels = [int(colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            lin = [c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
                   for c in channels]
            return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]

        def contrast(a, b):
            hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
            return (hi + 0.05) / (lo + 0.05)

        used = set(re.findall(r"#[0-9a-fA-F]{6}", td.STYLESHEET))
        self.assertEqual(used, set(td.PALETTE.values()))
        for colour in used:
            for background in ("#ffffff", "#1a1a1a"):
                self.assertGreaterEqual(contrast(colour, background), 3.0,
                                        f"{colour} on {background}")
        self.assertNotIn("background", td.STYLESHEET)

    def test_output_does_not_depend_on_hash_order(self):
        code = ("import sys,hashlib;sys.path.insert(0,sys.argv[1]);"
                "from sgi_model import topology_diagram as t,running_doc as r;"
                "c=r.load();print(hashlib.sha256('\\n'.join("
                "t.svg(c,x['name']) for x in c['topologies']).encode())"
                ".hexdigest())")
        digests = set()
        for seed in ("1", "2"):
            env = dict(os.environ, PYTHONHASHSEED=seed)
            digests.add(subprocess.run(
                [sys.executable, "-B", "-c", code, str(ROOT / "scripts")],
                env=env, check=True, capture_output=True,
                text=True).stdout.strip())
        self.assertEqual(len(digests), 1)
        here = hashlib.sha256("\n".join(
            td.svg(CAT, n) for n in TOPOLOGIES).encode()).hexdigest()
        self.assertEqual(digests, {here})


if __name__ == "__main__":
    unittest.main()
