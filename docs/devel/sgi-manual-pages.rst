.. SPDX-License-Identifier: BSD-3-Clause

============================
Writing the SGI manual pages
============================

The SGI pages of the system manual (``docs/system/mips/sgi/``) describe the
machines and the hardware they are built from, with the evidence behind
each modelled behaviour. This guide sets the layout, the rating of facts
and the citation rules. ``scripts/ci/sgi-docs-lint.py`` checks the
mechanical parts.

Layout
------

The manual has three levels.

The index (``index.rst``) explains how facts are rated, then lists the
machines, the firmware, the hardware by role, the references and the
glossary.

A platform page, ``ip<NN>.rst``, is titled "IPnn: products" and covers
machines only: the machines, their components, the address map, firmware
and boot, running, graphics, published machines, model limits, tests and
open questions.

A part page, ``hardware/<part>.rst``, is titled "<Part>: <function>", with
nothing in parentheses. One page covers one part. It opens with a "Used
in" paragraph linking the platform pages whose machines fit the part, or
saying that no machine fits it. It then uses these sections, in this
order, where they apply: Sources, Identity, Register map, the behaviour
sections, Measurements, Model limits and Tests. A part with no registers
or no measurements leaves those sections out rather than keeping them
empty. The family
overview pages ``graphics.rst`` and ``bricks.rst`` open the same way.

Follow the "Used in" paragraph with a ``.. seealso::`` that links the same
platform pages (``ip<NN>.rst``, or ``sn.rst``) back. Link related parts
with a ``.. seealso::`` too. On every page, mark an SGI
glossary term with ``:term:`` at its first use.

A part page that grows past what a reader can scan may move whole behaviour
sections to detail pages, ``hardware/<part>-<topic>.rst``, each titled
"<Part> <topic>: <function>". A detail page opens like its parent: a label
before the title, the same "Used in" paragraph, the same ``.. seealso::`` of
platform pages and a second ``.. seealso::`` linking the parent, then a
paragraph saying what the page covers. The parent keeps its path, Sources,
Identity, Register map, Reset, Migration, Measurements, Model limits and
Tests. Each moved section keeps its heading, labels and subsections as
written, and leaves a heading in the parent with a short summary and a
``:doc:`` link to the detail page. Give the detail page a new top label, not
one a section already uses. List each detail page in ``index.rst`` after its
parent and in ``LICENSE.origami.paths``, and mark a glossary term at its first
use on the detail page as on any other. The lint treats a detail page as a
part page, so every rule below applies to it.

A platform detail page, ``ip<NN>-<topic>.rst`` in the manual root, falls
outside the lint rules for ``hardware/`` and ``ip<NN>.rst`` pages. It needs
the SPDX line, a top label, a title and the licence listing.

The running pages, ``running.rst`` and ``running-ip<NN>.rst``, say how
to start a machine. They state no hardware fact, so they carry no ratings.
Write a table of selections, inputs, devices or heads, or a command that
creates a machine's stores, as ``.. sgi-run:: <kind> <product>``, which
renders it from ``hw/mips/sgi/sgi-machines.json`` when the manual is
built (``scripts/sgi_model/running_doc.py``). Write other commands by hand
in a shell code block, using the placeholder file names that
``running_doc.PLACEHOLDERS`` lists. Say a guest boots only by naming, with
``:test:`name```, a functional test that boots it.
``scripts/tests/test_sgi_running_pages.py`` checks every command in a
page against the catalogue, that every catalogue product has a page and
that each named test exists, and
``tests/functional/mips64/test_sgi_manual_commands.py`` starts each
command on a built emulator.

Facts
-----

Every statement about hardware carries one rating from ``sgi-ratings`` in
the index, in italics: *established*, *observed*, *software expectation*,
*inferred*, *approximation* or *unknown*. Use these exact forms: no
plurals, inflections, capitals or qualifiers inside the italics. Where a
sentence needs a plural, write "each an *approximation*".

- A model run shows only what the model does, never what the hardware does.
- A read taken after firmware initialisation is not a reset value. A reset
  value argued from such a read is *inferred*.
- Where the model differs from a source, state both values and rate the
  model's value *approximation*.
- Describe the current model only, not how it got there.

Citations
---------

Cite with ``:src:`id locator```, using ids from
``hw/mips/sgi/models/references.yaml``. That file is the only registry.
Regenerate ``references.rst`` from it with
``scripts/sgi_model/references_doc.py``. Cite a public driver by tree,
commit and path.

Model YAML cites pages as ``qemu:docs/system/mips/sgi/<page>.rst#label``.
When content moves, keep its label and repoint the YAML.

Sources
-------

Describe sources in sentences, without detail about any restricted source.

- Behaviour observed from a shipped binary, such as the IRIX kernel or a
  stock PROM, is a *software expectation*. Cite it by firmware registry
  entry and image address, never by symbol or file name.
- Leave out a fact whose only basis is non-public source.
- Record physical measurements in the part page's Measurements section and
  describe how they were taken on ``measured-machines.rst``.

Checks
------

Every check below is an error. None is a warning, and the required
pull-request check runs them on every pull request, whatever files it
changes, so the pages stay buildable and correct at all times.

``scripts/ci/sgi-check docs`` runs ``scripts/ci/sgi-docs-lint.py``, then
builds the whole manual with Sphinx and warnings as errors. A broken
table, label or cross-reference fails the build, and so does a ``:src:``
citation of an id that ``references.yaml`` does not list: the role leaves
it an undefined label (``scripts/tests/test_sgi_docs_citations.py`` builds
such a page and expects the failure). The lint requires, for every page
under ``docs/system/mips/sgi/``:

- the BSD-3-Clause SPDX comment on the first line;
- a top-level label before the title;
- a part title of the form "<Part>: <function>" with no parentheses, and
  a platform title of the form "IPnn: products";
- that any italic phrase that begins like a rating word, in any case, is
  exactly one of the six as ``sgi-ratings`` spells them, in prose and in
  directive bodies such as tables and ``seealso`` blocks, though not in
  literal or code blocks;
- no phrase that reads as a plan, such as "planned", "for now", "yet to", "will be modelled" or "not
  yet implemented", outside literal blocks, literals and quoted text; "not yet delivered" and the like describe a hardware state
  and pass;
- for a paragraph or list item that states a register's value in hex
  ("reads ``0x15``", "resets to ``0x80``"), a rating word or a ``:src:``
  citation in it. A hex number that is an address, as in "a read at
  ``0x10``", is not a value. These are exempt: table rows and simple
  tables, paragraphs in a Tests section, a sentence that itself names the
  model (or a log macro, a bus error or a test file), and the items of a
  list whose lead-in paragraph ends in a colon and carries the rating.
  Write a statement about the model as one about "the model"; a
  neighbouring sentence does not cover it;
- the first use on a page of each glossary term that has a capital letter
  (``XIO``, ``Bridge``, ``PROM``, ``Command Monitor``) as a ``:term:``
  link. The check reads running prose only, so it skips literals, roles,
  links, emphasis, titles and literal blocks, and the page whose own title
  holds the term. It also skips the lowercase terms (``node``, ``module``,
  ``widget``, ``brick``, ``node board``, ``hardware graph``), which are
  ordinary words, and ``L1``, which also names a cache level. A form that
  adds a suffix, such as ``XIOs`` or ``XIO-based``, is not the term. The
  glossary and the generated References page are not read;
- a "Used in" first paragraph on every part page;
- a ``.. seealso::`` block on a part page that links, with ``:ref:`` or
  ``:doc:``, each platform page its "Used in" paragraph links;
- machines in the "Used in" paragraph that exist in
  ``hw/mips/sgi/sgi-machines.json``: a literal that starts with a machine
  family or is followed by "machine" or "topology", and a numbered product
  such as "Origin 3000", must be an offered machine, topology or product;
  a part no machine fits says "Used in no machine";
- no pointer to material outside this repository, in the pages and in the
  comments of ``hw/mips/sgi/`` and ``hw/*/sgi-*``: a path under ``refs/``,
  ``assets/``, ``/storage`` or ``/scratch``, or ``/tmp`` outside a literal
  block, a path into a private document tree, a decision record code,
  or a reference to a private library or notebook. The check cannot
  recognise vendor kernel names and does not try, so keep those out by
  review;
- a listing of the page, and of this guide, in ``LICENSE.origami.paths``.

Three more checks run in the same required job, and each fails it:

- ``scripts/ci/sgi-check model`` checks the model citations and the
  generated files, including that ``references.rst`` matches
  ``references.yaml``;
- ``scripts/ci/sgi-license-check.py`` checks the licence listing and the
  SPDX headers of the SGI files;
- ``scripts/ci/sgi-leak-check.py`` rejects text in the change that matches
  a private list of identifiers. It reads the added lines, the new file
  names and the commit messages of a pull request. The full run before each
  land, a push to the trunk and a nightly run scan every tracked file with
  ``--tree``.

``scripts/tests/test_sgi_docs_lint.py`` tests each rule of the lint with a
page it must reject and a page it must pass.

Register reset values
---------------------

A register table with ``Offset`` and ``Reset`` columns can be checked
against the model. Put a comment above the ``.. list-table::``, so Sphinx
ignores it:

.. code-block:: rst

   .. sgi-registers: base=0x1f280000 width=64
      qemu="-m 64M -M o2,register-test-profile=on -nic none"

``qemu`` is the argument string the test starts QEMU with. ``base`` is the
physical address the table's offsets add to (default 0) and ``width`` the
access width in bits for a table without a ``Width`` column; a ``Width``
column wins, and its first number is the width. A device started on its own
with ``-M none`` gets ``map=PATH:INDEX:ADDRESS`` to map one of its MMIO
regions first (repeat it for several).

``tests/qtest/sgi-register-reset-test.c`` runs ``scripts/ci/sgi-register-tables.py
--json``, starts each configuration, reads every documented register and
fails with ``page:line`` when the model reads another value, so the page
is the only source of the expected values. Offsets may be a list
(``0x08``, ``0x10``), a range (``0x20``-``0x3c``, read at the table's
width) or a pair such as ``0x08`` / ``0x0c`` beside the Width cell
``64 / 32``. Several comments above one table check it in several
configurations. A cell that lists a value per variant, ``0; 0x8118 on
revision F``, takes the plain value, or the labelled one when the comment
says ``variant="revision F"``.

A Reset cell that says ``n/a``, ``none``, ``unknown`` or ``unsupported``,
or is empty, states no reset value and is skipped. Any other cell that is
not a number, such as "per machine", must have its offset in ``skip=``:
``skip=0x318000,0x31c000``. A row with a number is left out only with a
reason, ``skip=0x30:reason-in-one-word``. Where the page states the
specification's value and the model differs by a documented approximation,
``expect=0x310010:0x1400`` checks the model's value instead, and the page
says so with the *approximation* rating. An offset in ``skip=`` or
``expect=`` that matches no row, or an ``expect=`` that repeats the page's
value, is an error. Every skip and every pinned value is printed by
``--coverage`` and, with its reason, logged by the test (visible under
``--verbose``). A read taken after firmware initialisation is not a reset
value (`Facts`_), so it does not belong in a checked cell.

Three more keys cover a device that does not answer straight after reset.
``setup=0x20008022004:2,0x20008000214:0x1100`` lists physical writes
(``ADDRESS:VALUE[:WIDTH]``, 32 bits by default) made in order before the
first read: a Bridge attach, for example. Where rows repeat an offset and
other bits choose the register, ``select="S2, clock|0x8=0xa0"`` writes the
listed ``OFFSET=VALUE`` pairs, at the table's width, just before each read
of the row whose Name cell is ``S2, clock``. Repeat the key for another
row. ``skiprow="S1, control:write-only"`` leaves out every offset of the
row with that Name when ``skip=`` would also catch a row that shares the
offset. A ``select=`` or ``skiprow=`` that names no row is an error.
A ``width=``, ``access=``, ``lane=``, ``xor=``, ``stride=``, ``count=``,
``units=`` or ``split=`` that no row uses is an error too, as is a
``label=`` on a table with one comment.

A row such as ``0x310 + 0x1c*n`` needs ``count=4``, which reads copies
``n = 0`` to ``3``. ``label=NAME`` adds a name to the test path, to tell
two comments on one table apart.

``lane=4`` adds 4 bytes to every address the test reads, for registers kept in
the low half of an eight-byte slot (``lane=4/0x60`` for the offsets below
``0x60`` only), and ``stride=8`` spaces the registers
of a range such as ``0x130``-``0x168`` by slot rather than by access width.
``xor=2`` XORs every offset with 2 first, for the halfword registers behind
a Bridge, which places a narrow access by address lane. The offsets in the
output stay the page's.
A row whose Offset is relative to its unit, such as "link +0x08", takes
``units=0x100:0x40:8`` (base, stride, count) and is read once per unit.

``access=64`` reads every register of a table with that width whatever its
Width cell says, for a device whose registers are slots read whole: the
VICE lists a nine-bit register that is still read as a doubleword.

A board that refuses to build without a boot PROM, and runs nothing from
it, takes ``bios=blank``: the test starts it with an erased image.

A register in a window declared little-endian, such as the Xframe's, is
read with ``split=32``: two 32-bit reads, low half at the address and
high half four above. A Reset cell that says ``all ones`` is the width's
all-ones value.

A Reset cell that reads ``not specified; model 0`` states the model's
value, which the test checks.

A Reset cell of comma-separated numbers beside the same number of offsets
(``0x00010000``, 0 beside ``0x240``, ``0x244``) gives one value to each.

The test proves the page and the model agree, not that either matches the
hardware. The rating beside a value says how well it is known: a page may
say that every reset value of a part is the model's choice, an
*approximation*, and the test then only keeps the page honest about it.

``scripts/ci/sgi-check docs`` validates every comment (a bad word, a cell
that is neither a number nor skipped, a variant no cell uses, a comment not
directly above a table, or a marker the extractor dropped).
``scripts/ci/sgi-register-tables.py --coverage``
lists the annotated tables with the registers each checks and skips, then
every register table still unannotated, ordered by how many of its rows
already hold a plain reset number.
