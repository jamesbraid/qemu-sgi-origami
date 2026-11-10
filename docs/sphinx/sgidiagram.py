# SPDX-License-Identifier: BSD-3-Clause
"""A directive that draws the machine topologies of a platform page.

``.. sgi-diagram:: ip27`` (a platform, as on the running pages) or
``.. sgi-diagram:: origin2000`` (one product) renders an inline SVG for
each topology the machine catalogue (hw/mips/sgi/sgi-machines.json)
offers for it, so the picture is rebuilt with the manual and cannot name
a part the catalogue lacks. scripts/sgi_model/topology_diagram.py does the
drawing.
"""

from docutils import nodes
from docutils.parsers.rst import Directive

from sgi_model import running_doc, topology_diagram

__version__ = "1.0"


class SgiDiagram(Directive):
    required_arguments = 1
    has_content = False

    def run(self):
        env = self.state.document.settings.env
        env.note_dependency(str(running_doc.CATALOGUE))
        env.note_dependency(running_doc.__file__)
        env.note_dependency(topology_diagram.__file__)
        try:
            markup = topology_diagram.render(self.arguments[0])
        except ValueError as err:
            raise self.error(str(err))
        return [nodes.raw("", markup, format="html")]


def setup(app):
    app.add_directive("sgi-diagram", SgiDiagram)
    return {"version": __version__, "parallel_read_safe": True,
            "parallel_write_safe": True}
