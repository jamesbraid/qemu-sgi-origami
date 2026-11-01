# SPDX-License-Identifier: BSD-3-Clause
"""A directive that renders the machine catalogue into the running pages.

``.. sgi-run:: machines origin200`` renders the selections the catalogue
(hw/mips/sgi/sgi-machines.json) offers for a product, so the table is
rebuilt from the catalogue each time the manual is built and cannot
disagree with the machine. The other kinds are ``inputs``, ``graphics``,
``facts`` and ``launch``. scripts/sgi_model/running_doc.py does the rendering. The
``test`` role names a functional test file and the suite it is in.
"""

from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.statemachine import StringList

from sgi_model import running_doc

__version__ = "1.0"


class SgiRun(Directive):
    required_arguments = 2
    has_content = False

    def run(self):
        kind, product = self.arguments
        env = self.state.document.settings.env
        if kind not in running_doc.RENDERERS:
            raise self.error(f"unknown kind {kind!r}; use one of "
                             f"{', '.join(running_doc.RENDERERS)}")
        env.note_dependency(str(running_doc.CATALOGUE))
        env.note_dependency(running_doc.__file__)
        try:
            text = running_doc.render(kind, product)
        except ValueError as err:
            raise self.error(str(err))
        source = f"sgi-run {kind} {product}"
        node = nodes.container()
        self.state.nested_parse(StringList(text.split("\n"), source=source),
                                self.content_offset, node)
        return node.children


def test_role(name, rawtext, text, lineno, inliner, options=None,
              content=None):
    """``:test:`irix_ef``` is the functional test file test_irix_ef.py,
    followed by the meson.build list it is in. A name in neither list is
    a build error, so a removed test cannot stay on a page."""
    suite = running_doc.test_suite(text)
    path = running_doc.ROOT / f"tests/functional/mips64/test_{text}.py"
    if suite is None or not path.is_file():
        msg = inliner.reporter.error(
            f"{text!r} is not a mips64 functional test in a meson.build "
            "list", line=lineno)
        return [inliner.problematic(rawtext, rawtext, msg)], [msg]
    shown = f"test_{text}.py"
    return [nodes.literal(shown, shown),
            nodes.inline(f" ({suite})", f" ({suite})")], []


def setup(app):
    app.add_directive("sgi-run", SgiRun)
    app.add_role("test", test_role)
    return {"version": __version__, "parallel_read_safe": True,
            "parallel_write_safe": True}
