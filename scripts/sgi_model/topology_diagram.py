# SPDX-License-Identifier: BSD-3-Clause

"""Draw the machine topologies of the SGI manual from the catalogue.

``.. sgi-diagram:: ip27`` (or a product such as ``origin2000``) in a
platform page renders one inline SVG per topology the catalogue offers
for it, from hw/mips/sgi/sgi-machines.json, in docs/sphinx/sgidiagram.py.
The picture is rebuilt each time the manual is built, so it cannot name a
node, board or widget the catalogue lacks, and it draws every one the
catalogue has.

What is drawn, and from which catalogue field:

- Nodes (``nodes``), routers (``routers``), crossbars (``crossbars``),
  PCI host bridges (``pci-host-bridges``), I/O peers (``io-peers``), I/O
  boards (``io-boards``, with their ``pci-functions``) and the modules
  (``modules``) that hold them, with each module's controller
  (``controllers``).
- A link per ``cables`` entry, labelled with the cable name and its
  ports, and per ``xio-cables`` entry.
- A link per ``xio-attachments`` entry, read by the node's ASIC as the
  machine builder reads it (see ``build``): from the node to the crossbar
  for a Hub's or HEART's host entry, to the PCI host bridge that names an
  entry, and from the crossbar (or node) to the board an entry places. A
  Bedrock's entries behind a cable are its XBridge's heads, drawn at the
  crossbar. An entry nothing places ends in a stub.
- A link from a crossbar to each slot (``xio-slots`` and
  ``board-slots``), labelled by the slot's own widget line; a slot a board
  fills is the board.
- Where no I/O board states PCI functions, the devices the machine
  offerings list, in a box with no link: the catalogue does not say
  where they attach.

Nothing else is drawn. The text is real text and the colours are a small
fixed palette that keeps three to one against white and near-black, with
everything else in ``currentColor``, so the picture reads on a light or a
dark page. The output depends only on the catalogue, never on set order.
"""

from __future__ import annotations

import html
import re

from sgi_model import running_doc

# Shown for the ASIC and crossbar kinds the catalogue names.
ASIC_NAMES = {"heart": "HEART", "hub": "Hub", "bedrock": "Bedrock",
              "crime": "CRIME"}
CROSSBAR_NAMES = {"xbow": "Crossbow", "xbridge": "XBridge"}

# Each is at least 3.5:1 against white and against #1a1a1a.
PALETTE = {
    "node": "#2f7ec7",
    "router": "#c26a00",
    "crossbar": "#1f9a6a",
    "board": "#a45fb8",
    "controller": "#d0508a",
}

# Layout, in SVG user units. CHAR_W allows the widest monospace face a
# reader's system falls back to.
CHAR_W = 6.8
LINE_H = 14
BOX_PAD_X = 9
BOX_PAD_Y = 6
BOX_GAP = 10
BAND_PAD = 10
BAND_GAP = 14
HEADER_H = 26
MARGIN = 10
MIN_GAP = 64
ANCHOR_STEP = 12

COLUMNS = {"router": 0, "node": 1, "io-peer": 1, "crossbar": 2, "pic": 2,
           "board": 3, "slot": 3, "devices": 3}
# The class a kind is drawn with.
STYLE = {"io-peer": "node", "pic": "crossbar", "slot": "board",
         "devices": "plain"}

STYLESHEET = (
    ".sgid text{fill:currentColor;font:11px ui-monospace,SFMono-Regular,"
    "Menlo,Consolas,monospace}"
    ".sgid .b{font-weight:700}.sgid .s{font-size:9px}"
    ".sgid .e{fill:none;stroke:currentColor;stroke-width:1.2}"
    ".sgid .x{stroke-dasharray:5 3}"
    ".sgid .w{fill:none;stroke:currentColor;stroke-opacity:.4;"
    "stroke-dasharray:2 3}"
    ".sgid .box rect{fill-opacity:.12;stroke-width:1.5}"
    ".sgid .slot rect,.sgid .devices rect{stroke-dasharray:4 3}"
    ".sgid .plain rect{fill:currentColor;stroke:currentColor;"
    "stroke-opacity:.6}"
    + "".join(f".sgid .{k} rect{{fill:{c};stroke:{c}}}"
              for k, c in PALETTE.items())
    + ".sgid .stub{fill:currentColor}")


class Box:
    def __init__(self, kind, ident, lines, band, col=None):
        self.kind = kind
        self.ident = ident
        self.lines = lines          # [(text, bold)]
        self.band = band            # module index or None
        self.col = COLUMNS[kind] if col is None else col
        self.w = max(len(t) for t, _ in lines) * CHAR_W + 2 * BOX_PAD_X
        self.h = len(lines) * LINE_H + 2 * BOX_PAD_Y
        self.x = self.y = 0.0


class Edge:
    def __init__(self, kind, ident, a, b, label, ports=("", ""),
                 widget=None, via=None, stub=False):
        self.kind = kind
        self.ident = ident
        self.a = a
        self.b = b
        self.label = label
        self.ports = ports
        self.widget = widget
        self.via = via
        self.stub = stub


class Diagram:
    def __init__(self, topology):
        self.topology = topology
        self.bands = []             # [(module index or None, label lines)]
        self.boxes = []
        self.edges = []
        self.controllers = {}       # band -> [(ident, text)]


def products_of(argument, cat):
    """The products an argument names: a running page's platform, such as
    ``ip30``, or a single product."""
    families = {page.removeprefix("running-"): items
                for page, items in running_doc.FAMILIES.items()}
    if argument in families:
        return list(families[argument])
    if argument in running_doc.products(cat):
        return [argument]
    raise ValueError(
        f"{argument!r} is neither a platform ({', '.join(families)}) nor a "
        "catalogue product")


def topology_names(argument, cat):
    names = []
    for product in products_of(argument, cat):
        for name in running_doc.topologies(cat, product):
            if name not in names:
                names.append(name)
    return names


def offered_devices(cat, name):
    """The devices the offerings of a topology list, in the order they
    first appear: audio, network adapters, SCSI buses and serial ports. A
    serial line that only records another's output is not a device."""
    out = []

    def add(text):
        if text not in out:
            out.append(text)

    for o in cat["offerings"]:
        if o["topology"] != name:
            continue
        audio = o["devices"].get("audio")
        if audio:
            add(f"{audio['device']} audio")
        for n in o["networks"]:
            add(f"{n['nic-model']} network")
        for s in o["scsi-adapters"]:
            add(f"{s['bus']} scsi")
        for line in o["serial-lines"]:
            if line["kind"] != "record":
                add(f"{line['name']} serial")
    return out


def _slot_graphics(topo):
    """Slot name -> text for the graphics the slot takes."""
    out = {}
    for s in (topo.get("graphics") or {}).get("slots", []):
        place = {p["name"]: p["value"] for p in s.get("placement", [])}
        if "slot" not in place:
            continue
        devices = ", ".join(dict.fromkeys(f["device"] for f in s["families"]))
        out[place["slot"]] = ("graphics " + devices
                              + (" (default)" if s.get("default") else ""))
    return out


def build(cat, name):
    """The boxes and links of one topology, laid out in no particular
    place yet."""
    topo = next(t for t in cat["topologies"] if t["name"] == name)
    d = Diagram(name)
    modules = topo["modules"]
    show_bands = not (len(modules) == 1
                      and modules[0]["id"] == "implicit-module")

    def band_of(index):
        return index if show_bands and index is not None else None

    node_band = {}
    for mi, m in enumerate(modules):
        for ni in m["nodes"]:
            node_band[ni] = mi
    band_order = []
    if show_bands:
        for mi, m in enumerate(modules):
            parts = [m["id"]]
            if m.get("brick-type"):
                parts.append(f"{m['brick-type']}-brick")
            parts.append(f"module {m['initial-module-number']}")
            band_order.append((mi, "  ".join(parts)))
    d.bands = band_order

    def add(kind, ident, lines, band, col=None):
        box = Box(kind, ident, lines, band, col)
        d.boxes.append(box)
        return box

    nodes = []
    for i, n in enumerate(topo["nodes"]):
        spec = [ASIC_NAMES.get(n["asic-kind"], n["asic-kind"])]
        if n.get("slot") is not None:
            spec.append(f"slot {n['slot']}")
        if n.get("board-kind"):
            spec.append(n["board-kind"])
        band = band_of(n.get("module-index", node_band.get(i)))
        nodes.append(add("node", n["id"], [(n["id"], True),
                                           (" ".join(spec), False)], band))
    peers = [add("io-peer", p["id"],
                 [(p["id"], True),
                  (" ".join([ASIC_NAMES.get(p["asic-kind"], p["asic-kind"]),
                             f"slot {p['slot']}"]), False)],
                 band_of(p.get("module-index")))
             for p in topo.get("io-peers") or []]
    routers = [add("router", r["id"],
                   [(r["id"], True),
                    ("Router" + (f" slot {r['slot']}" if "slot" in r else ""),
                     False)], band_of(r.get("module-index")))
               for r in topo["routers"]]
    crossbars = [add("crossbar", x["id"],
                     [(x["id"], True),
                      (f"{CROSSBAR_NAMES.get(x['kind'], x['kind'])} widget "
                       f"revision {x['widget-revision']}", False)],
                     band_of(x.get("module-index")))
                 for x in topo["crossbars"]]
    pics = []
    for p in topo.get("pci-host-bridges") or []:
        att = topo["xio-attachments"][p["xio-attachment-index"]]
        pics.append(add("pic", p["id"], [(p["id"], True), ("PIC", False)],
                        nodes[att["node-index"]].band))

    boards = []
    for b in topo["io-boards"]:
        spec = " ".join(x for x in (b.get("preset"), b.get("part")) if x)
        lines = [(b["id"], True), (spec, False)]
        if b.get("label") and b["label"] != b["id"]:
            lines.append((f"slot {b['label']}", False))
        for f in b.get("pci-functions", []):
            text = f["kind"]
            if "pci-device" in f:
                text += f" device {f['pci-device']}"
            lines.append((text, False))
        if not b["populated"]:
            lines.append(("not populated", False))
        boards.append(add("board", b["id"], lines,
                          band_of(b.get("module-index"))))

    # Links from xio-attachments. The machine builder reads them by the
    # node's ASIC (hw/mips/sgi/sn-machine.c):
    # - Hub: a node's first entry behind a crossbar is its own host link;
    #   each later entry for that node, and each entry with no crossbar,
    #   places the next board in io-boards order
    #   (sgi_machine_index_xio_attachments).
    # - HEART (Octane): the builder hardwires HEART at widget 8 and the
    #   Bridge at 15 (octane.c); the catalogue's two entries say the same,
    #   so they are drawn by the Hub rule.
    # - Bedrock: each board names its entry with its ``attachment`` field,
    #   an index of xio-attachments. The entries are the node's own widget
    #   numbers for the XBridge heads behind a cable, so they are drawn at
    #   the crossbar, not as a link from the node; an entry no board names
    #   ends in a stub there.
    # - A topology with PCI host bridges names its entries through them.
    atts = topo["xio-attachments"]
    named = set()
    asic = topo["nodes"][0]["asic-kind"]

    def widget_edge(a, b, att_i):
        att = atts[att_i]
        d.edges.append(Edge("widget", f"{a.ident}>{b.ident}#{att_i}", a, b,
                            f"widget {att['widget']}", widget=att["widget"]))
        named.add(att_i)

    def entry_end(att):
        return (crossbars[att["crossbar-index"]] if att["crossbar-index"] >= 0
                else nodes[att["node-index"]])

    if topo.get("pci-host-bridges"):
        for p, pic in zip(topo["pci-host-bridges"], pics):
            widget_edge(nodes[atts[p["xio-attachment-index"]]["node-index"]],
                        pic, p["xio-attachment-index"])
    elif asic == "bedrock":
        for b, box in zip(topo["io-boards"], boards):
            if isinstance(b.get("attachment"), int) \
                    and 0 <= b["attachment"] < len(atts):
                widget_edge(entry_end(atts[b["attachment"]]), box,
                            b["attachment"])
    else:
        owners = []
        claimed = set()
        for i, att in enumerate(atts):
            node = att["node-index"]
            if att["crossbar-index"] < 0:
                owners.append(i)
            elif node not in claimed:
                widget_edge(nodes[node], crossbars[att["crossbar-index"]], i)
            else:
                owners.append(i)
            claimed.add(node)
        for att_i, box in zip(owners, boards):
            widget_edge(entry_end(atts[att_i]), box, att_i)
    for i, att in enumerate(atts):
        if i not in named:
            end_box = (entry_end(att) if asic == "bedrock"
                       else nodes[att["node-index"]])
            d.edges.append(Edge("widget", f"{end_box.ident}#{i}", end_box,
                                end_box, f"widget {att['widget']}",
                                widget=att["widget"], stub=True))

    # A board is otherwise linked only to the PCI host bridge that hosts it.
    for b, box in zip(topo["io-boards"], boards):
        if b.get("pci-host-index") is not None:
            pic = pics[b["pci-host-index"]]
            d.edges.append(Edge("attach", f"{pic.ident}>{box.ident}", pic,
                                box, f"head {b['pci-head']}"
                                if "pci-head" in b else ""))

    def end(e):
        kind = e["kind"]
        pool = {"node": nodes, "router": routers, "io-peer": peers,
                "crossbar": crossbars, "io-board": boards}[kind]
        return pool[e["index"]]

    for c in topo["cables"]:
        a, b = c["ends"]
        d.edges.append(Edge("cable", c["name"], end(a), end(b), c["name"],
                            tuple(f"p{e['port']}" if "port" in e else ""
                                  for e in (a, b)),
                            via=c["kind"]))
    for c in topo.get("xio-cables") or []:
        a, b = c["ends"]
        port = next((e["port"] for e in (a, b) if "port" in e), None)
        d.edges.append(Edge("xio-cable", c["name"], end(a), end(b),
                            c["name"] + ("" if port is None
                                         else f" port {port}")))

    # Slots hang from a crossbar by their widget. A slot a board fills
    # is the board.
    xs = {s["label"]: s for s in topo["xio-slots"]}
    gfx = _slot_graphics(topo)
    slot_names = list(dict.fromkeys(
        [s["name"] for s in topo.get("board-slots") or []] + list(xs)))
    def fan_out(slot):
        # Slots of one crossbar together, in widget order.
        if slot in xs:
            return (xs[slot]["crossbar-index"], xs[slot]["widget"])
        m = re.fullmatch(r"widget-(\d+)", slot)
        return (0, int(m.group(1))) if m else (0, 0)

    for slot in sorted(slot_names, key=fan_out):
        filled = [box for b, box in zip(topo["io-boards"], boards)
                  if b.get("label") == slot]
        if slot in xs:
            xbar = crossbars[xs[slot]["crossbar-index"]]
            widget = xs[slot]["widget"]
        else:
            m = re.fullmatch(r"widget-(\d+)", slot)
            if filled or not m or len(crossbars) != 1:
                continue
            xbar, widget = crossbars[0], int(m.group(1))
        if filled:
            target = filled[0]
        else:
            # A fan of slot links is unreadable with a label on each, so
            # the slot's box carries its widget instead.
            lines = [(slot, True), (f"widget {widget}", False)]
            lines += [(gfx[slot], False)] if slot in gfx else []
            target = add("slot", slot, lines, xbar.band)
        d.edges.append(Edge("widget", f"{xbar.ident}>{slot}", xbar, target,
                            f"widget {widget}" if filled else "",
                            widget=widget))

    # Boards that state no PCI functions leave the devices to the offerings.
    if not any(b.get("pci-functions") for b in topo["io-boards"]):
        devices = offered_devices(cat, name)
        if devices:
            lines = [("Devices the offering lists", True)]
            lines += [(x, False) for x in devices]
            add("devices", "devices", lines,
                boards[0].band if boards else nodes[0].band)

    for c in topo["controllers"]:
        band = band_of(c["module-index"])
        profile = (c.get("l1") or {}).get("profile")
        text = f"{c['kind'].upper()} {c['id']}" + (f" ({profile})"
                                                    if profile else "")
        d.controllers.setdefault(band, []).append((c["id"], text))
    return d


# ---- layout ---------------------------------------------------------------

def _label_w(text, size=CHAR_W):
    return len(text) * size


def layout(d):
    """Place the boxes in columns and the columns in module bands."""
    # A box with many links grows so each meets it at its own point.
    count = {}
    for e in d.edges:
        if e.stub:
            sides = [(e.a, "r")]
        elif e.a.col == e.b.col:
            sides = [(e.a, "l"), (e.b, "l")]
        else:
            lo, hi = sorted((e.a, e.b), key=lambda x: x.col)
            sides = [(lo, "r"), (hi, "l")]
        for box, side in sides:
            count[(id(box), side)] = count.get((id(box), side), 0) + 1
    for b in d.boxes:
        n = max(count.get((id(b), "l"), 0), count.get((id(b), "r"), 0))
        b.h = max(b.h, n * ANCHOR_STEP + 8)
    cols = sorted({b.col for b in d.boxes})
    col_w = {c: max(b.w for b in d.boxes if b.col == c) for c in cols}

    # Gaps between neighbouring columns hold the link labels.
    gap_need = {c: MIN_GAP for c in cols}
    arcs = {}
    for e in d.edges:
        if e.stub:
            continue
        lo, hi = sorted((e.a.col, e.b.col))
        width = _label_w(e.label) + 28
        if lo == hi:
            arcs.setdefault(lo, []).append(e)
            continue
        span = [c for c in cols if lo < c <= hi]
        for c in span:
            gap_need[c] = max(gap_need[c], width / len(span))
    arc_depth = {}
    for c, edges in arcs.items():
        for k, e in enumerate(sorted(edges, key=lambda e: e.ident)):
            arc_depth[id(e)] = 26 + 14 * k
        need = (26 + 14 * (len(edges) - 1)
                + max(_label_w(e.label) for e in edges) + 12)
        gap_need[c] = max(gap_need[c], need)
    stubs = any(e.stub for e in d.edges)
    bands = d.bands or [(None, None)]
    if d.bands and (any(b.band is None for b in d.boxes)
                    or None in d.controllers):
        bands = [(None, None)] + d.bands
    x = MARGIN + (gap_need[cols[0]] if cols[0] in arcs else 0)
    col_x = {}
    for k, c in enumerate(cols):
        if k:
            x += gap_need[c]
        col_x[c] = x
        x += col_w[c]
    width = x + MARGIN + (60 if stubs else 0)
    # Links start at a column's right edge and end at a column's left
    # edge. Keep each heading's text left of the first column's right
    # edge and each controller tag right of the last column's left edge,
    # so no link runs through a heading.
    head_w = max((len(label) * CHAR_W + MARGIN + 12
                  for _, label in bands if label), default=0)
    shift = max(0, head_w - (col_x[cols[0]] + col_w[cols[0]]))
    for c in cols:
        col_x[c] += shift
    width += shift
    tag_w = max((sum(len(t) * CHAR_W + 18 for _, t in tags)
                 for tags in d.controllers.values()), default=0)
    width = max(width, col_x[cols[-1]] + tag_w + 2 * MARGIN)
    # A module's name and its controller share the header line.
    for key, label in bands:
        tags = d.controllers.get(key, [])
        need = (len(label or "") + sum(len(t) + 3 for _, t in tags)
                ) * CHAR_W + 2 * MARGIN + 40
        width = max(width, need)
    y = MARGIN
    band_boxes = []
    for key, label in bands:
        top = y
        header = HEADER_H if label or key in d.controllers else 0
        tallest = 0
        for c in cols:
            cy = top + header + BAND_PAD
            for b in (b for b in d.boxes if b.col == c and b.band == key):
                b.x, b.y = col_x[c], cy
                cy += b.h + BOX_GAP
            tallest = max(tallest, cy - BOX_GAP - (top + header + BAND_PAD))
        h = header + 2 * BAND_PAD + max(tallest, 0)
        band_boxes.append((key, label, top, h))
        y = top + h + BAND_GAP
    return width, y - BAND_GAP + MARGIN, band_boxes, col_x, col_w, arc_depth


def _spread(count, centre, height):
    step = min(ANCHOR_STEP, (height - 8) / max(count, 1))
    return [centre + (k - (count - 1) / 2) * step for k in range(count)]


def anchors(d):
    """The point on each box side where each link meets it."""
    sides = {}
    for i, e in enumerate(d.edges):
        if e.stub:
            sides.setdefault((id(e.a), "r"), []).append((e.a.y, i, "a"))
            continue
        a, b = e.a, e.b
        if a.col == b.col:
            for end, box, other in (("a", a, b), ("b", b, a)):
                sides.setdefault((id(box), "l"), []).append(
                    (other.y, i, end))
        else:
            if a.col > b.col:
                a, b = b, a
            sides.setdefault((id(a), "r"), []).append((b.y, i, "a"))
            sides.setdefault((id(b), "l"), []).append((a.y, i, "b"))
    boxes = {id(b): b for b in d.boxes}
    points = {}
    for (bid, side), items in sides.items():
        box = boxes[bid]
        ys = _spread(len(items), box.y + box.h / 2, box.h)
        for y, (_, i, end) in zip(ys, sorted(items)):
            x = box.x + (box.w if side == "r" else 0)
            points[(i, end)] = (x, y)
    return points


# ---- drawing --------------------------------------------------------------

def esc(text):
    return html.escape(str(text), quote=True)


def num(v):
    return f"{v:.1f}".rstrip("0").rstrip(".")


def _text(x, y, text, cls="", anchor=None):
    attrs = f' class="{cls}"' if cls else ""
    if anchor:
        attrs += f' text-anchor="{anchor}"'
    return f'<text x="{num(x)}" y="{num(y)}"{attrs}>{esc(text)}</text>'


def describe(d):
    """The text alternative: what is drawn, link by link."""
    kinds = {}
    for b in d.boxes:
        kinds.setdefault(b.kind, []).append(b.ident)
    names = {"node": "nodes", "io-peer": "I/O peers", "router": "routers",
             "crossbar": "crossbars", "pic": "PCI host bridges",
             "board": "I/O boards", "slot": "slots"}
    parts = [f"{label}: {', '.join(kinds[k])}."
             for k, label in names.items() if k in kinds]
    for e in d.edges:
        if e.stub:
            parts.append(f"{e.a.ident} has an unconnected {e.label}.")
        else:
            parts.append(f"{e.a.ident} to {e.b.ident}: {e.label}.")
    return " ".join(parts)


def _clip(p, q, rect):
    """The parameter range of the segment p-q inside the rectangle, or
    None (Liang-Barsky)."""
    x0, y0, x1, y1 = rect
    dx, dy = q[0] - p[0], q[1] - p[1]
    lo, hi = 0.0, 1.0
    for pk, qk in ((-dx, p[0] - x0), (dx, x1 - p[0]),
                   (-dy, p[1] - y0), (dy, y1 - p[1])):
        if pk == 0:
            if qk < 0:
                return None
            continue
        r = qk / pk
        if pk < 0:
            lo = max(lo, r)
        else:
            hi = min(hi, r)
        if lo > hi:
            return None
    return lo, hi


def _hits(p, q, rect):
    return _clip(p, q, rect) is not None


def _overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


class Labels:
    """Places labels clear of the boxes, of the labels placed before and
    of every link, trying positions along a link nearest its middle."""

    def __init__(self, obstacles, segments, size):
        self.rects = list(obstacles)
        self.segments = segments    # [(p, q, link index)]
        self.size = size            # (width, height) of the drawing

    def cost(self, rect, skip=None):
        grown = (rect[0] - 1.5, rect[1] - 1.5, rect[2] + 1.5, rect[3] + 1.5)
        outside = (rect[0] < 0 or rect[1] < 0 or rect[2] > self.size[0]
                   or rect[3] > self.size[1])
        return (4 * sum(1 for r in self.rects if _overlap(rect, r))
                + sum(1 for p, q, k in self.segments
                      if k != skip and _hits(p, q, grown))
                + (20 if outside else 0))

    def on_line(self, a, b, text, index):
        """Put the text on the straight link a-b, breaking the line behind
        it, so the label is unmistakably the link's. Return the text's
        markup and the path pieces to draw."""
        w = len(text) * CHAR_W
        best = None
        for t in (0.5, 0.4, 0.6, 0.3, 0.7, 0.2, 0.8, 0.88, 0.12):
            x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            rect = (x - w / 2, y - 9 + 4, x + w / 2, y + 3 + 4)
            c = self.cost(rect, index)
            # The label must sit inside the link, not over a box at its end.
            if best is None or c < best[0]:
                best = (c, x, y, rect)
            if c == 0:
                break
        _, x, y, rect = best
        self.rects.append(rect)
        gap = _clip(a, b, (rect[0] - 3, rect[1] - 1, rect[2] + 3,
                           rect[3] + 1)) or (0.5, 0.5)
        pieces = [(a, (a[0] + (b[0] - a[0]) * gap[0],
                       a[1] + (b[1] - a[1]) * gap[0])),
                  ((a[0] + (b[0] - a[0]) * gap[1],
                    a[1] + (b[1] - a[1]) * gap[1]), b)]
        return _text(x, y + 4, text, anchor="middle"), pieces

    def put(self, candidates, text, cls=""):
        """Draw the text at the first candidate (x, base, anchor) that
        collides with nothing, else at the least colliding one."""
        w = len(text) * CHAR_W * (0.8 if cls == "s" else 1)
        best = None
        for k, (x, base, anchor) in enumerate(candidates):
            x0 = {"start": x, "middle": x - w / 2, "end": x - w}[anchor]
            rect = (x0, base - 9, x0 + w, base + 3)
            c = self.cost(rect)
            if best is None or c < best[0]:
                best = (c, x, base, anchor, rect)
            if c == 0:
                break
        _, x, base, anchor, rect = best
        self.rects.append(rect)
        return _text(x, base, text, cls, None if anchor == "start" else anchor)


def bezier(a, b, dep, steps=10):
    p = [a, (a[0] - dep, a[1]), (b[0] - dep, b[1]), b]
    out = []
    for k in range(steps + 1):
        t = k / steps
        u = 1 - t
        out.append(tuple(u ** 3 * p[0][i] + 3 * u * u * t * p[1][i]
                         + 3 * u * t * t * p[2][i] + t ** 3 * p[3][i]
                         for i in (0, 1)))
    return out


def svg(cat, name):
    d = build(cat, name)
    width, height, bands, col_x, col_w, arc_depth = layout(d)
    pts = anchors(d)
    topo = next(t for t in cat["topologies"] if t["name"] == name)
    uid = "sgid-" + re.sub(r"[^A-Za-z0-9_-]", "-", name)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" class="sgid" '
           f'role="img" aria-labelledby="{uid}-t {uid}-d" '
           f'viewBox="0 0 {num(width)} {num(height)}" width="{num(width)}" '
           f'style="max-width:100%;height:auto">',
           f"<title id=\"{uid}-t\">{esc(topo['description'])}: topology "
           f"{esc(name)}</title>",
           f'<desc id="{uid}-d">{esc(describe(d))}</desc>',
           f"<style>{STYLESHEET}</style>"]

    obstacles = [(b.x, b.y, b.x + b.w, b.y + b.h) for b in d.boxes]
    for key, label, top, h in bands:
        if label is not None:
            out.append(f'<g class="module" data-kind="module" '
                       f'data-id="{esc(topo["modules"][key]["id"])}">'
                       f'<rect class="w" x="{MARGIN / 2:g}" y="{num(top)}" '
                       f'width="{num(width - MARGIN)}" height="{num(h)}" '
                       f'rx="6"/>' + _text(MARGIN + 4, top + 17, label, "b")
                       + "</g>")
            obstacles.append((MARGIN, top + 4, MARGIN + 8
                              + len(label) * CHAR_W, top + 22))
        right = width - MARGIN
        for ident, text in d.controllers.get(key, []):
            w = len(text) * CHAR_W + 12
            out.append(f'<g class="box controller" data-kind="controller" '
                       f'data-id="{esc(ident)}"><rect x="{num(right - w)}" '
                       f'y="{num(top + 4)}" width="{num(w)}" height="18" '
                       f'rx="4"/>'
                       + _text(right - 6, top + 17, text, anchor="end")
                       + "</g>")
            obstacles.append((right - w, top + 4, right, top + 22))
            right -= w + 6

    # Every link's path, so labels can keep clear of all of them.
    paths = {}
    for i, e in enumerate(d.edges):
        a = pts[(i, "a")]
        if e.stub:
            paths[i] = [a, (a[0] + 34, a[1])]
        elif e.a.col == e.b.col:
            paths[i] = bezier(a, pts[(i, "b")], arc_depth[id(e)])
        else:
            paths[i] = [a, pts[(i, "b")]]
    segments = [(p[k], p[k + 1], i) for i, p in paths.items()
                for k in range(len(p) - 1)]
    labels = Labels(obstacles, segments, (width, height))

    # Port numbers first: they sit at a link's ends and keep their place.
    ports = {}
    for i, e in enumerate(d.edges):
        if e.stub or e.a.col == e.b.col:
            continue
        pa, pb = e.ports if e.a.col < e.b.col else e.ports[::-1]
        a, b = pts[(i, "a")], pts[(i, "b")]
        ports[i] = "".join(
            [labels.put([(a[0] + 3, a[1] - 3, "start")], pa, "s")
             if pa else "",
             labels.put([(b[0] - 3, b[1] - 3, "end")], pb, "s")
             if pb else ""])
    for i, e in enumerate(d.edges):
        a = pts[(i, "a")]
        attrs = (f'class="link" data-kind="{e.kind}" '
                 f'data-id="{esc(e.ident)}" data-a="{esc(e.a.ident)}" '
                 f'data-b="{esc(e.b.ident)}"')
        if e.widget is not None:
            attrs += f' data-widget="{e.widget}"'
        if e.via is not None:
            attrs += f' data-via="{esc(e.via)}"'
        parts = [f"<g {attrs}>"]
        style = "e x" if e.via == "null-router" else "e"
        path = paths[i]
        if e.stub:
            ex = path[1][0]
            parts.append(f'<path class="{style}" d="M{num(a[0])} {num(a[1])}'
                         f'H{num(ex)}"/><circle class="stub" cx="{num(ex)}" '
                         f'cy="{num(a[1])}" r="3"/>'
                         + labels.put([(a[0] + 4, a[1] - 3, "start"),
                                       (a[0] + 4, a[1] + 11, "start")],
                                      e.label, "s"))
        elif e.a.col == e.b.col:
            b = pts[(i, "b")]
            dep = arc_depth[id(e)]
            mid = (a[1] + b[1]) / 2 + 4
            parts.append(
                f'<path class="{style}" d="M{num(a[0])} {num(a[1])}'
                f'C{num(a[0] - dep)} {num(a[1])} {num(b[0] - dep)} '
                f'{num(b[1])} {num(b[0])} {num(b[1])}"/>'
                + labels.put([(a[0] - dep * 0.75 - 4, mid + k, "end")
                              for k in (0, -14, 14, -28, 28)], e.label))
        else:
            b = pts[(i, "b")]
            if e.label:
                text, pieces = labels.on_line(a, b, e.label, i)
                for p0, p1 in pieces:
                    parts.append(f'<path class="{style}" d="M{num(p0[0])} '
                                 f'{num(p0[1])}L{num(p1[0])} {num(p1[1])}"/>')
                parts.append(text)
            else:
                parts.append(f'<path class="{style}" d="M{num(a[0])} '
                             f'{num(a[1])}L{num(b[0])} {num(b[1])}"/>')
            parts.append(ports[i])
        parts.append(f"<title>{esc(e.a.ident)} to {esc(e.b.ident)}: "
                     f"{esc(e.label)}</title></g>")
        out.append("".join(parts))

    for b in d.boxes:
        cls = STYLE.get(b.kind, b.kind)
        out.append(f'<g class="box {cls}" data-kind="{b.kind}" '
                   f'data-id="{esc(b.ident)}"><rect x="{num(b.x)}" '
                   f'y="{num(b.y)}" width="{num(b.w)}" height="{num(b.h)}" '
                   f'rx="4"/>'
                   + "".join(_text(b.x + BOX_PAD_X,
                                   b.y + BOX_PAD_Y + LINE_H * (k + 1) - 3,
                                   t, "b" if bold else "")
                             for k, (t, bold) in enumerate(b.lines))
                   + "</g>")
    out.append("</svg>")
    return "\n".join(out)


def render(argument, cat=None):
    """The HTML for a ``sgi-diagram`` directive: one diagram per topology,
    each in a ``details`` element that starts open so a printed or static
    copy shows every one, and a reader can fold the ones they skip."""
    cat = cat or running_doc.load()
    out = []
    for name in topology_names(argument, cat):
        topo = next(t for t in cat["topologies"] if t["name"] == name)
        out.append(f'<details class="sgi-diagram" open>'
                   f"<summary><code>{esc(name)}</code>: "
                   f"{esc(topo['description'])}</summary>\n"
                   f"{svg(cat, name)}\n</details>")
    return "\n".join(out)


if __name__ == "__main__":
    import sys
    print(render(sys.argv[1]))
