.. SPDX-License-Identifier: BSD-3-Clause

IA-64 CPU import
================

The target comes from `syunnPC/qemu-system-ia64`_ at
``cf190a5bdfdcec0c2a194d3b9a207ee27c1b6381``. Its original
GPL-2.0-or-later notices are retained. The decoder differs only by provenance
comments. The runtime is adapted to maintained QEMU and registered as
``ia64-softmmu``. A platform that supplies the SHub and a PROM image runs
the stock SN2 PROM and a SLES 9 kernel on it; this is not complete CPU, IA-32
or authentic-firmware qualification.

.. _syunnPC/qemu-system-ia64: https://github.com/syunnPC/qemu-system-ia64/tree/cf190a5bdfdcec0c2a194d3b9a207ee27c1b6381

Maintaining the import
----------------------

Keep the donor's original commits rather than a second source copy or a
maintained file manifest. Fetch the pinned history without merging its QEMU
tree::

    git fetch --no-tags https://github.com/syunnPC/qemu-system-ia64.git \
      cf190a5bdfdcec0c2a194d3b9a207ee27c1b6381:refs/remotes/syunn/ia64-import-base

Git supplies the actual import inventory and local changes::

    git log --oneline -- target/ia64 tests/unit/test-ia64-decode.c
    git diff --name-status refs/remotes/syunn/ia64-import-base HEAD -- target/ia64
    git diff refs/remotes/syunn/ia64-import-base HEAD -- \
      target/ia64/decoder.c target/ia64/decoder.h target/ia64/decode

The first diff also lists intentionally unimported target files as deletions.
For an update, fetch the candidate to a different ref, compare the two donor
revisions for the imported paths, and apply only the relevant changes to the
maintained sources. Keep the original notices and update the pin and divergence
rationale here in the same tested commit. Do not merge the donor's boards,
firmware or shared QEMU snapshots. Future donors need the same independent
instruction tests, not a provider framework.

Independent test evidence
-------------------------

``tests/unit/test-ia64-decode.c`` constructs encodings from Intel's
*IA-64 Architecture Software Developer's Manual*, volume 3, revision 1.1,
July 2000, document 245319-002 (`retained public PDF`_). Its SHA256 is
``b76a36be99260eee7d8d61b1a34aaf0770f984890b285257b6b4800b5fca52f4``.
Printed pages differ from PDF page indexes by 264 in chapter 4 and by 20 in
chapter 2.

.. _retained public PDF: https://refspecs.linuxbase.org/IA64-softdevman-vol3.pdf

The tests cover:

* Bundle slot extraction, including the slot crossing the 64-bit boundary:
  figure 4-1, printed p. 4-1 (PDF 265).
* Defined/reserved template codes and execution unit mapping: table 4-2,
  printed p. 4-2 (PDF 266). Stop positions are not asserted.
* A1 register operands and the optional one, A4 signed immediate formation:
  printed pp. 4-9, 4-10 and 4-79 (PDF 273, 274 and 343). All 14-bit immediate
  values are checked in both the M and I units.
* B1 conditional branch displacement: printed pp. 4-63 and 4-80 (PDF 327 and
  344), including the signed limits and the factor of 16.
* BREAK forms I19, M37, B9 and F15: printed pp. 4-28, 4-55, 4-67 and 4-76
  (PDF 292, 319, 331 and 340). Printed p. 2-17 (PDF 37) specifies that B-unit
  BREAK ignores its immediate. Values formerly used by donor firmware portals
  still decode as BREAK; this does not test their eventual exception delivery.

The test is registered in Meson's ``unit`` and ``sgi-unit`` suites. A configured
build can run it with ``meson test -C build test-ia64-decode``. Use the project's
container build/test tooling when available.

Runtime adaptation and limits
-----------------------------

``tests/qtest/ia64-cpu-test.c`` runs small hand-assembled programs on the none
machine through the loader and qtest interfaces. They cover instruction
semantics, exceptions and interruptions (a real guest handler records IIP, IIM
and IPSR.ri), the register stack, the ALAT, the interval timer, two-processor
programs, the PAL's reserved words and the cache-array port. Expected state
and encodings come from Intel 245318-002 and 245319-002. The Altix 350 machine
page lists what each estimate rests on. ``tests/functional/ia64`` boots the
stock PROM when ``ALTIX350_PROM`` names an image.

The runtime adaptations are:

* Full-address debug translation uses the current callback and debug attributes.
* Per-page speculation and memory attributes live in a table in the CPU, not
  in core TLB state. Speculative loads always take the full qualification.
  Ordinary maintained flushes replace donor host jump-cache retention, without
  purging target-owned instruction mappings.
* No IA-32 execution exists, and nothing under ``target/i386`` is changed or
  compiled in. ``br.ia`` without the MSR 1225 service and ``rfi`` with
  ``ipsr.is`` set checkstop the CPU and report the PC. IA-32 is a separate later
  change with its own plan.
* The ALAT is a 32-entry table keyed by register and physical address bits
  19:0, with the replacement order in the Itanium 2 reference manual 5.1.1
  (same register, first invalid entry, then an approximate FIFO).
* Store-then-load ordering of UC and UCE pages is not enforced beyond what
  TCG gives every guest access (*unknown*; no machine maps UC RAM yet, and
  MMIO already runs in program order). The target adds nothing to core TLB
  code; per-page speculation and memory-attribute bytes live in a table in
  the CPU, and speculative loads always take the full qualification path.
* Built-in PAL, FPSWA and firmware-assist dispatch, synthetic identity mappings,
  boot-info reset, firmware debug state, unaligned emulation and bare-loader
  exception suppression are removed. No donor virtual-PC ABI header is needed.
  PAL-based INIT/MCA/PMI delivery checkstops explicitly until its hardware entry
  mechanism is implemented; it does not invoke an in-process SAL handler.

The qtest programs supply their own PC. Madison's ``0xffffffb0`` reset entry
is an explicitly inferred diagnostic choice; initial register/PSR state remains
unqualified. ``0xfffffff0`` is the IA-32 reset code location (Intel
245318-002 section 11.1.6). MSR indices 0 to 2047 are stored; an unknown
write is logged and stored, an unknown read is logged and returns the last
value written, or zero. An index of 2048 or more takes Reserved Register/Field
(inferred: the SDM lists no ``msr`` register file). These diagnostic
assumptions are not hardware support. ``ia64_unimplemented_msr`` traces every
access. A bus error or any access the CPU cannot complete checkstops the CPU
and reports a guest panic; ``-action panic=pause|shutdown|none`` chooses what
the VM does. The CPU is unmigratable.

No firmware bytes, synthetic PAL reply or SAL entry shortcut is part of this
target. Further debug/TLB/order/RSE/FP qualification remains
necessary.
