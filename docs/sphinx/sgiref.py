# SPDX-License-Identifier: BSD-3-Clause
"""A citation role for the SGI machine pages.

``:src:`007-3439-002 p. 2-15``` cites a source from
hw/mips/sgi/models/references.yaml by its id and adds a locator. It
renders as ``[007-3439-002, p. 2-15]``, linked to that source's entry on
the generated References page. An id the page does not list is an
undefined label, so a build with warnings as errors rejects it.
"""

import re

from docutils import nodes
from sphinx import addnodes

__version__ = "1.0"


def label(source_id):
    return "sgi-ref-" + re.sub(r"[^a-z0-9]+", "-", source_id.lower()).strip("-")


def src_role(name, rawtext, text, lineno, inliner, options=None, content=None):
    # A line wrap can fall between the id and its locator.
    parts = text.split(None, 1)
    source_id = parts[0]
    locator = " ".join(parts[1].split()) if len(parts) > 1 else ""
    shown = f"[{source_id}, {locator}]" if locator else f"[{source_id}]"
    ref = addnodes.pending_xref(rawtext, refdomain="std", reftype="ref",
                                reftarget=label(source_id), refexplicit=True,
                                refwarn=True)
    ref += nodes.inline(shown, shown, classes=["sgi-cite"])
    return [ref], []


def setup(app):
    app.add_role("src", src_role)
    return {"version": __version__, "parallel_read_safe": True,
            "parallel_write_safe": True}
