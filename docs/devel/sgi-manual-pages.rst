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
