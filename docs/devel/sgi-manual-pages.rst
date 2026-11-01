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

Link related parts with ``.. seealso::``. On the overview pages, mark an
SGI term with ``:term:`` to the glossary at its first use.

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

``scripts/ci/sgi-check docs`` builds the manual with warnings as errors and
runs ``scripts/ci/sgi-docs-lint.py`` first. The lint requires, for every
page under ``docs/system/mips/sgi/``:

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
- a "Used in" first paragraph on every part page;
- machines in the "Used in" paragraph that exist in
  ``hw/mips/sgi/sgi-machines.json``: a literal that starts with a machine
  family or is followed by "machine" or "topology", and a numbered product
  such as "Origin 3000", must be an offered machine, topology or product;
  a part no machine fits says "Used in no machine";
- no pointer to material outside this repository, in the pages and in the
  comments of ``hw/mips/sgi/`` and ``hw/*/sgi-*``: a path under ``refs/``,
  ``assets/``, ``/storage`` or ``/scratch``, or ``/tmp`` outside a literal
  block, a path under
  ``docs/datasheets/``, a Markdown file name, a decision record code
  such as ``D-0021``, "Library", "notebook" or "reviewed datasheet". The
  check cannot recognise vendor kernel names and does not try, so keep
  those out by review;
- a listing of the page, and of this guide, in ``LICENSE.origami.paths``.

``scripts/ci/sgi-check model`` checks the citations and the generated files.
