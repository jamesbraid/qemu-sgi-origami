# Datasheet: L1 system controller (one Origin 300 chassis)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/l1-controller-datasheet-review.md`): cleared for the register block, the framing
constants and check value, the header and subchannel layout, the address encoding,
the request and reply codes, the board-identity record and its printed test vector,
the module-index conflict and the operator-side facts, position for position, with
the review's eleven findings applied in this revision. The open items stay open.
A partial isolated protocol responder, including destination validation and read-only
settings queries and standalone UART interrupts, is saved through fork commit
`e0f46b5f32`, with seventeen passing L1 qtest subtests. This does not establish stock-PROM or
full-machine acceptance.
Bedrock parent commit `54ce1c3fabaec311812f94df4eb678c1b6122c35` connects the
remote UART through current-cell protection. Its tests verify denied accesses,
queued replies, local reset retention and migration with this L1 device.
The combined device run passes 59 subtests. Physical machine mapping and L1
interrupt wiring remain separate integration work.
The record-corrections amendment of 2026-09-13, from the SN family closure record
(`docs/research/sn-family-closure-20260913.md`), fixes three items: the two-chassis
slot procedure carried a page citation that actually names the brick-type-letter
table, not the cabling steps; the `config` command row gave only one capture's index
values instead of the L1 guide's own index-to-position scheme; and the attached-I/O-brick
behavioral note's account of the Origin 350 and Tezro brick-type digits is corrected
against the datasheet's own IRIX naming table and the public header, leaving what
those two digits name as an open question. It awaits independent review and does not
clear L1, Bridge, XBridge, IO-8, Bedrock, processor or machine implementation.
The firmware-observation amendment of 2026-09-12 corrects the stock PROM's
node-identity selection and records the exact `0x0004` acceptance and fallback
predicates. It awaits independent review and does not clear L1, Bridge, XBridge,
IO-8, Bedrock, processor or machine implementation.
Revision of 2026-09-07: adds the [request `0x000e`](#request-0x000e) contract and a
[firmware cross-check](#firmware-cross-check) of the cleared rows, both read from the
retained stock L1 firmware image for the Origin 300 product group, and a consumer-side
subsection, [what the stock PROM sends and expects](#what-the-stock-prom-sends-and-expects),
read independently from the stock IP35 PROM payload and the retained run. Those
subsections and the `0x000e` table row and checklist bullets were added in this
revision and reviewed as the next sentence records; that addition changed nothing else
in the cleared text.
Reviewed 2026-09-07 by an independent reader
(`docs/reviews/l1-request-000e-contract-review.md`): the request `0x000e` contract,
its checklist bullets and frames, the firmware cross-check as corrected by the review's
findings 3, 4 and 6, and the PROM-side subsection, table row and provenance rows are
cleared for implementation with the review's findings 1 to 10 applied in this revision.
The same revision adds the [Lifecycle](#lifecycle) section, the
[management console](#management-console) subsection, additions to
[console multiplexing](#console-multiplexing) and lifecycle bullets in the qtest
checklist, merged from the lifecycle and console research draft of 2026-09-07 by a
third researcher and reviewed as recorded next. Those additions append new sentences
to the cleared console-multiplexing subsection and record where the draft and the
cleared text differ; no cleared sentence was removed or reworded by the merge.
Reviewed 2026-09-07 by a second independent reader
(`docs/reviews/l1-lifecycle-console-contract-review.md`): the lifecycle,
console-ownership and management-console sections, their state, transitions, command
and display tables and their checklist bullets are cleared position for position with
the review's findings 1 to 21 applied in this revision. Per its finding 20, two cleared
sentences of the console-multiplexing subsection were corrected in the draft's favour:
the power-up message now carries the two spaces before the brick id that every capture
shows, and the sentence on the L1's own messages now names its INFO-prefixed lines.
The review's finding 5 names the request-`0x000e` state paragraph as a second place for
its settings-record observation; that paragraph is the other reviewer's and is unchanged
here, so the observation is recorded on both sides of the conflict paragraph under
[what the L1 keeps](#what-the-l1-keeps).
Protocol-parity revision of 2026-09-07: the [Requests](#requests) section replaces the
previous request table with the firmware's own dispatch, per-task tables, reply codes
and limits, read from the retained Origin 300 L1 image by a fourth researcher; the
[addressing](#addressing) model rules are replaced by the firmware's routing, and the
[L1-initiated packets](#l1-initiated-packets), [subchannel
semantics](#subchannel-semantics), [receive tolerance](#receive-tolerance) and [request
logging switch](#request-logging-switch) subsections are added from an events and
addressing reading of the same image by a fifth researcher. Where those readings
contradict a cleared row or a declared model choice, the firmware reading is applied
and the change is listed under [contract changes in this
revision](#contract-changes-in-this-revision), with the consumer effect drawn from the
PROM and IRIX consumer readings; where two readings disagree, both are recorded. The
requests, addressing, events and tolerance text of this revision, its fifteen listed
changes and its checklist bullets await a third independent review; the `0x000e`
subsection, the PROM-side subsection, the lifecycle, console-ownership and
management-console sections stay cleared as recorded above, and the one linking clause
the lifecycle re-verification asked for is added to the `0x000e` state paragraph.
The same revision adds the [consumer expectations](#consumer-expectations) section: what
the stock IP35 PROM asks of the L1, read from its payload by the PROM-side researcher,
and what the IRIX 6.5.30 kernel asks, read from its compiled objects by a further
researcher, both independently of the firmware readings, with the consumers' own
unknowns and the two readings' recorded differences; the "Sent by" cells of the
request tables are updated from both. That section awaits the same independent review.
Reviewed 2026-09-07 by a third independent reader
(`docs/reviews/l1-protocol-catalogue-review.md`): the [Requests](#requests) section, the
[addressing](#addressing) routing rules and table, the [L1-initiated
packets](#l1-initiated-packets), [subchannel semantics](#subchannel-semantics), [receive
tolerance](#receive-tolerance) and [request logging switch](#request-logging-switch)
subsections, the contract changes (8 withdrawn, 15 rewritten), the [consumer
expectations](#consumer-expectations) section, the "Sent by" cells and the
protocol-parity checklist bullets are cleared with the review's findings 1 to 14 applied
in this revision; the command-interpreter reply shape, the flash-task codes, the PROM's
boot order and per-row uses and the IRIX kernel-object claims stand as their readers',
marked so. Its findings 1 to 5 corrected five implementer-facing statements of the
previous revision: every dispatched request is answered, an EEPROM component index
beyond the table answers minus 104, the short-frame rows of the receive-tolerance table,
the task-5 echo carries the first argument, and task 15 has a request receiver. Its
finding 6 settles the reply-limit disagreement at a 116-byte field in a 128-byte frame,
its finding 3 settles the short-frame cutoff, and its reading of the field accessor
decides the wrong-argument value for the catalogue; the idle type-4 outcome and the
link-good line's origin stay open as recorded.
Power-request model revision of 2026-09-10: adds the [modeled `0x2005` and `0x2006`
actions](#modeled-power-requests-0x2005-and-0x2006) and their qtest checklist bullets.
The addition records bounded QEMU lifecycle choices around the already-reviewed wire
requests. It awaits independent evidence review and does not claim hardware accuracy.
Short-frame revision of 2026-09-17: the model now delivers any frame of three bytes or
more whose check sequence passes, as the [receive tolerance](#receive-tolerance) table
already gave for the firmware's reader. The previous five-byte model minimum, allowed by
the catalogue review as a rejection of well-checked frames shorter than a header and
destination, also rejected every one-character console-output frame, which has no
destination, and so hid the IP35 PROM's keystroke echo, line breaks and padding on the
Origin 300: a two-CPU boot sent `1` at the maintenance menu looked hung while the PROM
waited for Return (QEMU `fdb6a7bd83`,
the start-system stall notebook (citation: `docs/notebooks/origin300-start-system-stall-20260917.md`)).
The checklist bullet is corrected below. The wire frames are observed; the correction
awaits independent review.
The [two-chassis pair](#two-chassis-pair) draft of 2026-09-25 adds a section and
four checklist bullets for two cabled Origin 300s and changes no cleared row. It is
independently reviewed, first as a draft and then as the peer contract implemented
in QEMU `sgi-origami` at `412d4c4460`.
**Fork file(s):** `hw/misc/sgi-l1.c`, `include/hw/misc/sgi-l1.h`, and
`tests/qtest/sn1-l1-test.c`.
**Evidence cutoff:** 2026-09-12, Origami trunk `b7b0271831` for the identity
amendment. The protocol-parity revision used trunk `2de79ed6d8`; the `0x000e` and
lifecycle revisions of the same day used `e2dae3d028`; the 2026-09-03 revision used
`506836b9dd`.

**Sources used:** OpenBSD at `2df91d80c1`: `sys/arch/sgi/sgi/l1.c` (`l1.c,v 1.11`),
`l1.h`, `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27_machdep.c`,
`sys/arch/sgi/pci/ioc.c`, `sys/arch/sgi/dev/spdmem_mainbus.c`, `sys/net/ppp_defs.h`,
`sys/dev/ic/ns16550reg.h`, `comreg.h`. The Linux SN1 port at `v2.5.40`:
`arch/ia64/sn/io/{l1.c,l1_command.c,eeprom.c,module.c,klconflib.c}`,
`include/asm-ia64/sn/ksys/l1.h`, `sn/uart16550.h`, `sn/module.h`, `sn/eeprom.h`
(SGI's own guest-side implementation for the Itanium prototype; a reference
consumer, not a MIPS guest). The out-of-tree MIPS port `refs/linux-ip35`:
`arch/mips/sgi-ip35/{ip35-l1.c,ip35-console.c}` and the later SGI header it carries,
`arch/ia64/include/asm/sn/l1.h`. Linux SN1 `arch/ia64/sn/kernel/setup.c` and
`arch/ia64/config.in` for the raw-UART build, `include/asm-ia64/sn/sn1/hubmd.h` for
the junk-bus timing register. Published manuals in the techpubs
mirror: the SGI L1 and L2 Controller Software User's Guide 007-3938 revisions 001 to
006, the Origin 300 User's Guide 007-4365-002 and -001, the Origin 300 with NUMAlink
Module guide 007-4498-002 (whose appendix prints real EEPROM dumps from an IP45 and
an IO-8). The Origin 3000 System Architecture 108-0240-002 and the Origin 300 scan
tools guide 108-0263-004 for what the L1 is. The cached public forum thread on Origin
300 L1 firmware for version strings only. Reading preserved in the SN1 research
notebook; the exact wire frames below were computed from the framing rules and are
derived values, not captures.
For the 2026-09-07 revision: the retained stock L1 firmware, composite
`assets/firmware/6.5.30/l1.bin` (15,988,636 bytes, SHA-256
`c355c542554a41b23b69dca4c201c566e8352e8f9fb870d2365c74cdd8ab0c0f`), read through the
982,412-byte packaged range that the
packaged-image selection review (citation: `docs/reviews/l1-packaged-image-selection-review.md`)
identifies as the Origin 300 candidate (SHA-256
`0c969f9e98c73826495dc37bff685b3403e248993ba92194a288333d9d42b2f4`), which carries
version `1.44.0`, build stamp 2006-07-17 18:20:38 and the product label
`Fuel/PE/O300 1MB image`. The controller in that image is a ColdFire (68000-family)
microcontroller: the range is an EST flat binary whose first words are the initial
stack pointer and the reset entry, and whose reset code writes a ColdFire-only control
register; the public analyser in `refs/qemu-sgi-project`
(`analysis_tools/prom/sysco_firmware.py`) classes these images the same way. The range
holds the program twice, once per flash bank; the reading used the first copy. Public
vocabulary for the new subsection: 007-3938-006 printed pages 9 to 12 (console
subchannels named per node, "node|blade 0 console subchannel" through "node|blade 3
console subchannel") and printed pages 101 and 103; Linux
mainline `arch/ia64/include/asm/sn/geo.h` at `cf07cb1ff4ea^` (the revision before the
SN2 platform removal) for the slab and slot terms.
For the consumer side of the same request, read by a second researcher: the stock IP35
PROM payload `assets/firmware/6.5.30/ip35prom.payload.bin` (1,476,264 bytes, SHA-256
`554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b`, extracted from
`ip35prom.img`, 1,480,360 bytes, SHA-256
`d74b4b7ef9c11c66692019db9af698104595be7a0573a86feae8a83441921318`, both
manifest-verified; visible version text `SGI Version 6.210  built 02:33:51 PM Aug 26,
2004`), decoded as raw big-endian MIPS IV by the procedure in
`docs/library/ip27prom-symbol-map.md`; the retained trace of run
`prom-20260907T141650-98dc09` on QEMU sgi-origami `351e29f5f5` with launcher
`57764ba21ae5`, preserved with the boot record in Origami PR #844 (head
`bc2f2968aeeb`); linux-mips `origin/linux-2.4` at `65456e0e26`
`include/asm-ia64/sn/ksys/l1.h` lines 74 to 123 for the 2003 request codes and brick
types, including the Twister type.
The 2026-09-12 identity amendment used the complete annotated disassembly with
SHA-256 `dcf0e9a9901b0407828173f50933b24bab8260a48493a4073cac9fd69eb1d4f5`
and run `prom-20260912T075429-15ae87`, whose console has SHA-256
`5033a08636a8eeac482e80b175a6285cb7e47a4668cf433cc31f61a30431adb2`.
The run records QEMU revision `407b192420c5204f2e1aa98c98511bfe71fff1f0`.
For the lifecycle, console-ownership and management-console additions of the same
revision, read by a third researcher: the L1 and L2 Controller Software User's Guide
007-3938-001 (2001, printed pages 1 to 4, 11 to 28, 33 to 35, 38 to 66 and 72 to 82)
and -003 (2002, printed pages 11 to 12, 19 to 31, 40 to 49, 66 to 67, 73 and 81 to 93)
beside -006 (printed pages 28 to 35, 46 to 48, 53 to 66, 77 to 80, 89 to 95, 100 to 102
and 110 to 111); 007-4365-002 printed pages 6 to 7, 34 to 41 and 54 and 007-4365-001
printed pages 42 to 43, 69 and 95; 007-4498-002 printed pages 4, 85 to 91, 120 to 121,
136, 177 to 195, 200 to 206 and 211 to 214, whose appendix C is the only command
reference captured on an Origin 300; the Origin 3000 Series Owner's Guide 007-4240-002
printed pages 46 to 48 and 108; the System Controller Software release notes
007-4576-001 printed pages 14 to 17 (the L1 1.14.x notes: `leds` fixed for CPUs C and D
on Origin 300 systems, and junk-bus PPP errors no longer printed on the console but
still logged in the L1's NVRAM); 108-0240-002 printed pages 2-36, 2-40 to 2-47,
2-49 to 2-51 and 3-2, held at Library root `origami-docs`, path
`library/cache/arch/origin-3000-system-architecture-108-0240-002.pdf`, its printed
pages taken from the page footers. Retained transcripts catalogued in
`docs/library/boot-logs.md`, cited by line of the markup-stripped text: IRIXnet thread
2737 (an Origin 300 on L1 1.18.5), Nekochan threads 16724335 (a two-chassis Origin 300,
L1 1.48.1) and 17314 (a two-chassis Origin 300 whose L1 content is a `cfg v` listing
with no version line), 16727514 (six Origin 300s under an L2), 16729141 (an Origin
350 on L1 1.38.4, a `log` transcript), 16721376 (an Origin 350
L1 flash session), the Origin 350 and Fuel session page
`graphica-sgi-o350-chimera-notes` (the Origin 350 console session prints no L1 version;
the page's `L1 booting 1.42.9` is its NUMAlink router's log, and its `l1cmd ver` prints
L1 1.48.1 from an IRIX host on the Fuel) and the Fuel `l1cmd` page
`tingo-sgi-fuel-irix-console`. The transcript line numbers are those of the
researcher's markup-stripped copies, which are not in the repository and which no
common stripper reproduces (the review record's own numbering differs); each such
citation also quotes the text, and the quoted text, not the number, is the citation.
Usenet comp.sys.sgi.admin messages of 2005
(`1132172144.919058.181550@g49g2000cwa.googlegroups.com`,
`1132778527.644765.40810@g14g2000cwa.googlegroups.com`) and comp.sys.sgi of 2002
(`3D19C210.9020308@brussels.sgi.com`, `slrna7jllc.2b2e.ivanr@omen.melbourne.sgi.com`),
read through the Library's mail service. Public consumers at the lines cited inline:
OpenBSD `origin/sgi-is-alive-at-7.9` `sys/arch/sgi/sgi/{l1.c,l1.h,ip27_machdep.c}`;
the Linux SN1 port at `v2.5.40` `arch/ia64/sn/io/{l1.c,l1_command.c}` and
`include/asm-ia64/sn/ksys/l1.h`; `refs/linux-ip35` at `9f613a321ea0`
`arch/mips/sgi-ip35/{ip35-console.c,ip35-l1.c}`. The same retained firmware range as
above (composite `l1.bin`, 15,988,636 bytes, SHA-256
`c355c542554a41b23b69dca4c201c566e8352e8f9fb870d2365c74cdd8ab0c0f`, verified against
`assets/MANIFEST.sha256` line 46; the 982,412-byte Origin 300 range, SHA-256
`0c969f9e98c73826495dc37bff685b3403e248993ba92194a288333d9d42b2f4`, recomputed; version
`1.44.0`, label `Fuel/PE/O300 1MB image`), read by `strings` at minimum lengths 6 and 3
and a NUL-delimited exact-token search for command names only, with no disassembly; the
range holds the program twice, so every text appears twice. Library queries run before
external retrieval: `--mode artifacts` for `L1 controller "console mode" Origin 300`,
`108-0240-002` and `108-0263-004` (20 hits each), `--mode mail` for `Origin 300 L1
console` (14 hits, five relevant), `reboot_l1` (1), `"L1 running"` (0), `"escaping to
L1"` (0), `"auto power up"` (2, unrelated) and `"power down" L1 IRIX Origin` (0); no
text-mode source query was run, so that restricted vendor source stayed out of the
results, and the source trees were read through git at their pinned revisions.
108-0263-004 was not opened for these additions.
For the protocol-parity revision, two further readers of the same retained firmware
range (composite `l1.bin`, SHA-256
`c355c542554a41b23b69dca4c201c566e8352e8f9fb870d2365c74cdd8ab0c0f`, manifest line 46;
the 982,412-byte Origin 300 range, SHA-256
`0c969f9e98c73826495dc37bff685b3403e248993ba92194a288333d9d42b2f4`, version `1.44.0`),
each disassembling the first of its two bank copies with capstone's M68K mode run
through `uv`. The selected image names an MCF5206e build target, and its
ColdFire M-Bus accesses match the published MCF5206e register map. The
image establishes software compatibility with that layout, not the part
fitted to a particular IP45/I/O-8 board revision; no revision-matched
component list or chip marking has been checked. Its public cross-references: OpenBSD `origin/sgi-is-alive-at-7.9`
`sys/arch/sgi/sgi/{l1.h,l1.c}` (`l1.h,v 1.6`); Linux `v2.5.40` `include/asm-ia64/sn/ksys/l1.h`,
`arch/ia64/sn/io/{l1_command.c,eeprom.c}`; `refs/linux-ip35` `9f613a321ea0`
`arch/ia64/include/asm/sn/l1.h`; 007-3938-006 printed pages 63 to 64 (the `debug`
command and its switch table) and 100 to 102 (`l1dbg promreq`); 007-4498-002 printed
pages 179 (`brick` output `C (IP45)`) and 182 (the EEPROM dump order); 108-0240-002
printed pages 2-42 to 2-43; four broad and refined Library searches over Files, Code and
Mail for the request codes, reply-code names, debug-switch pages and the controller part
(the only public definers of the names are the two copies of the Linux SN1 header; no
Library document names the processor). The events and addressing reading's public
cross-references: Linux `v2.5.40` `arch/ia64/sn/io/l1.c` lines 133 to 139, 743 to 806,
1177 to 1181, 1186 to 1474, 2007, 2129, 2355 to 2369 and 2842 to 3056,
`l1_command.c` lines 572 to 649, 952 to 1023, 1084 to 1139 and 1240 to 1306,
`ksys/l1.h` lines 44 to 66 and 173 to 303; the later system-controller event driver in
Linux history at `981a2edd1922`, `drivers/char/snsc_event.c` lines 56 to 222 and
`drivers/char/snsc.h` lines 28 to 89; OpenBSD `sys/arch/sgi/sgi/l1.c` lines 40 to 70,
255 to 310 and 505 to 567; `refs/linux-ip35` `arch/mips/sgi-ip35/ip35-l1.c` lines 180 to
288 and `ip35-console.c` lines 1 to 80; 007-3938-006 printed pages 9 to 12, 19, 28, 46
to 48, 63 to 64, 77 to 79, 100 to 103 and 110 to 111; 007-3938-001 printed pages 13 to
14, 20 to 21 and 59 to 60; 007-4365-002 printed page 41; 007-4498-002 printed pages 181
and 202 to 203; the Origin 300 captures in `docs/library/boot-logs.md` through the
lifecycle researcher's text extracts; ten Library searches across all three modes (the
mail backend failed twice) and `tools/library-pdf find` on the four manuals for
`promreq`, `l1dbg`, `debug`, `event`, `syscom`, `ATTN`, `log` and `subchannel`. Neither
reader opened `refs/irix-source` or the web; the events reader read three peers'
working notes for setup only and re-read every firmware fact from the image.
For the consumer-expectations section, the stock IP35 PROM payload named above
(`ip35prom.payload.bin`, SHA-256
`554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b`, from `ip35prom.img`,
SHA-256 `d74b4b7ef9c11c66692019db9af698104595be7a0573a86feae8a83441921318`, both
manifest-verified; version 6.210 built 26 August 2004), read by the method of
`docs/library/ip27prom-analysis.md` (a raw MIPS64 listing, data-reference recovery and
control-flow walking, nothing transcribed), its mapping per
`docs/decisions/D-0024-origin300-stock-prom-reset-carrier.md` and its identity per
`docs/library/prom-inventory.md` and `docs/library/firmware-release-provenance.md`; the
Origin 300 transcript of IRIXnet thread 2737 (PROM 6.210, L1 1.18.5) and the emulator run
`prom-20260907T141650-98dc09`; for comparison OpenBSD `sys/arch/sgi/sgi/{l1.c,l1.h}` at
`origin/sgi-is-alive-at-7.9`, Linux `v2.5.40` `ksys/l1.h`, `l1.c` and `l1_command.c`,
Linux `v2.6.5` `ksys/l1.h` and `refs/linux-ip35` `sn/l1.h` for the later brick-type
table, Linux `v2.5.0` `include/asm-ia64/sn/sn1/{hubpi.h,hublb.h}` for the real-time
counter and scratch register names; 007-3938-006 printed pages 100 to 102 and
007-4365-002 printed pages 35 to 41; seven `tools/library-search --json` queries (`PROM
L1 request`, `Sys Ctlr`, `System controller debug switches`, `L1 request 0x1004
display`, `brick type request L1 PROM`, `L1 protocol request code`, `junkbus uart
PROM`), each returning 20 ranked hits and none describing the PROM's requests beyond
this datasheet and the two consumer drivers, and a header comparison across
`linux-history` tags `v2.5.40` to `v2.6.5`, the `linux-mips` branches and `linux-ip35`
that found no public definition of `0x000b`, `0x000c`, `0x000e`, `0x1009`, `0x100a`,
`0x100c`, `0x2006` or `0x3001` to `0x3003`. For the IRIX 6.5.30 kernel: the compiled
objects on the retained installation media at
`refs/irix-media/6.5.30/usr/cpu/sysgen/IP35boot/`, `l1.o`, `l1_command.o`, `eeprom.o`,
`kernel.o`, `sndrv.o`, `pic.o`, `xbridge.o`, `if_ef.o`, `irouter.o` and the archive
members `sduart.a(sio_l1.o)`, `os.a(main.o, printf.o, vmdump.o)`,
`jtagio.a(smpl_jtagio.o)` and `odyssey.a(odsy_dvi.o)`, disassembled with
`tools/mips-disasm` and read with `readelf` symbol and relocation tables; the userland
binaries `usr/sbin/l1cmd`, `usr/etc/sysctlrd` and `usr/sysadm/privbin/sysctlrKill` on the
same media by `strings` only; for comparison Linux `v2.5.40` `arch/ia64/sn/io/l1.c`
(lines 133 to 139, 2335 to 2420 and 2842 to 2960), `include/asm-ia64/sn/ksys/l1.h`
(lines 236 to 277) and `sn/eeprom.h` (lines 256 to 278), Linux `v2.5.0`
`include/asm-ia64/sn/klconfig.h` lines 121 to 152 and `arch/ia64/sn/io/klgraph_hack.c`
line 151, OpenBSD `sys/arch/sgi/xbow/hub.h` lines 43 to 75, `sys/arch/sgi/include/mnode.h`
lines 105 to 150 and `sys/arch/sgi/sgi/l1.c`; 007-3938-001 printed pages 27, 48 and 51,
007-3938-006 printed pages 47, 48 and 72, 007-4365-002 printed page 37, 007-4498-002
printed pages 109 and 186; Library mail-archive messages `alt3c9$m4t$1@news.ucar.edu`,
`b361uj$a5c$1@news.ucar.edu`, `dsDBi.41158$S91.34159@newsfe7-win.ntli.net`,
`20061121023859.40ae69ab.daichi.k@aioros.ocn.ne.jp` and
`slrndi91sn.313.wilhelm.greiner@sammy.erwo.net` for real Origin 300 and Fuel device
paths and `hinv -vm` output. No IRIX source was read by either consumer reader.

**Facts that are IRIX-derived: none from IRIX source.** The [IRIX 6.5.30](#irix-6530)
subsection and the consumer-effect sentences under [contract changes in this
revision](#contract-changes-in-this-revision) carry the IRIX 6.5.30 kernel's L1 client as
read from the compiled IP35 kernel objects on the retained media by a further
researcher, restated with no code, addresses in the objects, offsets or internal names
(hardware addresses are those the public headers name; routine-name prefixes of quoted
messages are omitted); that reader is excluded from implementing or independently
reviewing the L1 device. The [stock IP35 PROM](#the-stock-ip35-prom) subsection carries
the PROM's client as read from its payload by the PROM-side researcher, restated the
same way. Facts read from the L1 firmware image: the
[request `0x000e`](#request-0x000e) and [firmware cross-check](#firmware-cross-check)
subsections, restated in this datasheet's own words with no code, addresses or internal
names; their reader is excluded from implementing or reviewing the L1 device. The
[Requests](#requests) section with its contract changes, the [addressing](#addressing)
routing rules and the [L1-initiated packets](#l1-initiated-packets), [subchannel
semantics](#subchannel-semantics), [receive tolerance](#receive-tolerance) and [request
logging switch](#request-logging-switch) subsections were rewritten from the request
catalogue and the events and addressing reading of the same image by two further
readers, in this datasheet's own words with no code, addresses, offsets or internal
names; both readers are excluded from implementing or independently reviewing the L1
device.
The [Lifecycle](#lifecycle) section, the [management console](#management-console)
subsection and the additions to [console multiplexing](#console-multiplexing) also
carry operator-visible texts read from that image's string table by a third researcher,
with no disassembly and no code, addresses, offsets or internal names; that reader is
likewise excluded from implementing or independently reviewing the L1 device.

## Isolated implementation coverage

The implementation covers the UART register interface, bounded framing and
reply queues, display and rack/type replies, file-backed EEPROM access, console
switching, explicit revision metadata, read-only debug-switch, partition and
port-speed replies, standalone receive/transmitter-empty interrupts, power-off
and migration. Request `0x000e` and the console-select field are saved through fork
commit `4ae3bb88b2` with twenty passing subtests, cleared by clean implementation
review in fork PR #425. The three
array properties `chassis-eeprom`, `board-eeprom` and `spd-eeprom` map file
paths by component index. Empty entries are absent. Files contain at most
65,535 bytes, and SPD files must contain at least 64. Migration preserves the
source's exact record bytes rather than substituting destination file contents.

For the selected two-processor Origin 300, a [first-hand node-board inventory](https://nekonomicon.irixnet.org/forum/14/16726611/1.html)
identifies `IP45_2CPU`, board part `030-1780-001` revision C, and physical
processors A and C. The published node-board EEPROM examples in SGI
007-4365-001 printed page 75 and SGI 007-4498-002 Appendix C, Example C-11
(heading on printed page 181, node-board bytes on printed page 182),
identify `IP45_4CPU`, not that board. Neither the owner inventory nor those examples
provide the selected two-CPU board's complete raw record, checksum coverage
or PSC comparison values. No example from a four-CPU product is converted
into a substitute. In a
firmware run without node-board bytes (citation: `docs/boot-logs/firmware.boot.2026-09-14T21:55:03-07:00`),
the firmware warned of a board EEPROM/PSC CPU-configuration mismatch and
independently reported processor C failed early initialization. That console
alone does not show that the absent EEPROM caused the processor failure.

The device model still has no built-in identity. The launcher
(`tools/origami_lib/node_eeprom.py`) supplies synthetic component-0 chassis
and board areas when the operator supplies none, as it already did for the
IO-8's component 1. Every captured real Origin 300 inventory prints a node board line, so an
absent node-board record is inferred not to occur on the product, and IRIX
depends on it: IRIX builds the node vertex's `_nic` hardware
graph attribute (`Part:...;Name:...;Serial:...;Revision:...`) from those two
areas. It published none with neither area and none with the board area
alone; a chassis area alone was not tried. The online
diagnostics' harness takes its machine name from that attribute's `Name`
field, and `olsio` dereferences the unset name and dies with SIGSEGV
(the olsio root-cause notebook (citation: `docs/notebooks/olsio-sigsegv-rootcause.md`)).
The synthetic board area carries only what real inventories print: the board
name, part number and revision for the node's processor count (`IP45_2CPU`,
`030-1780-001`, `-C` from the first-hand inventory above; `IP45_4CPU`,
`030-1779-001`, `-C` from a [first-hand four-CPU
inventory](https://nekonomicon.irixnet.org/forum/14/17314/1.html)).
Manufacturer, serial number, date and the binary fields are placeholders, as
in the IO-8 records, and no byte is copied from a published dump. It is an
approximation, not an authentic record. What it does not establish: the
real area's other fields and the PSC CPU-configuration comparison. With it,
the 6.210 PROM reported every fitted processor enabled and printed no CPU
configuration mismatch at two and at four processors. It printed none on
the same build without node-board areas either, so these runs do not test
that comparison. IRIX 6.5.30
published `Name:IP45_2CPU` and `Name:IP45_4CPU` respectively. A raw
node-board record from a real board would settle the remaining fields; an
operator-supplied `--node-board-eeprom` still replaces the synthetic area.

Revision metadata is supplied through `revision-major`, `revision-minor`, and
`revision-patch`. Unconfigured metadata is reported unavailable. A protocol
reply or operator version line does not mean L1 firmware executed.

Destination validation resolves local rack and bay selectors independently.
Foreign destinations, reserved address bits and unmodeled tasks are logged and
dropped without a reply. Unsupported commands on modeled tasks return minus 101
before task-specific arguments are decoded. These are the reviewed bounded
model rules documented in this sheet, not captured hardware rejection behavior.

**Two objects.** Since fork PR #426 the model is two devices, the way QEMU splits an
IPMI interface from its BMC: `sgi-l1` is the controller, a bus-less device with the
operator character device, the settings, the frame parser, the request dispatch, the
display and the log; `sgi-l1-uart` is the node-side junk-bus UART window, a SysBus
device with the eight registers, the byte queue toward the processor, the `uart`
interrupt output and a `controller` link that refuses to share one controller between
two windows. The window sits on the node's reset tree; the controller does not.

**Lifecycle model (fork commit `579f5cfc9b`, cleared by clean implementation review in fork PR #427).** The
controller restarts only at realize, which stands for mains applied, and on the
`reboot_l1` command; a node reset clears the window's registers and queue and
nothing the controller keeps, so the checklist sentence "a device reset returns the
value to the `node-position` property" is read as a controller restart. Node power
follows the ELSC precedent chosen on 2026-09-07: `power down`, `* power down` and the
guest's `* pwr d` remove node power, write `Powered Down`, log `power down (COMMAND)`
and request a QEMU shutdown, so a session that powers down and up again needs QEMU's
no-shutdown behavior from the launcher; `power up` on a powered-down node writes
`Powered Up`, logs `power up (COMMAND)`, returns the selection to
CPU 0, prints the `entering` or `returning` console-mode text, requests a system reset
and resumes the machine once that reset has run. The `node-power` output follows the
state. Declared model choices, each with a discriminating qtest: the `auto-power`
property, on by default where the real default is off, powers the node at mains with
no countdown, logging `auto power up initiated` and `power up (COMMAND)` and printing
`INFO: <id> powering up the system.`; `reset` requests a system reset and pulses the
`node-reset` output, `softreset` and `nmi` pulse `soft-reset` and `nmi`, hold times are
not modeled, the display shows only the final texts (`Reset done`, `Sft rst done`, `NMI
done`), and all three do nothing while the node is off; the guest's power-off request
logs its source as `COMMAND`; the L1 prints nothing on its console at boot and shows
the prompt on the first carriage return; a command typed from the escaped state with
the node off leaves the L1 at the prompt; console-mode bytes with the node off print
`WARNING: power appears off, console unavailable` once per entry and are dropped; the
display's first line carries a static `/` where the hardware spins a heartbeat; the log
holds 128 entries of 47 characters with `MM/DD/YY HH:MM:SS` stamps from QEMU's clock
(the `date` command is not yet modeled, so the offset is zero) and begins with
`L1 booting <version>` or `L1 booting` without metadata; `brick` reports `serial:
unavailable` because the model carries no serial numbers, and `brick rack` and `brick
slot` take effect at the next `reboot_l1`; `version` with metadata prints `L1 x.y.z
(Image A) [Fuel/PE/O300 1MB image]` with no build stamp. Console selection through
`select`, `select subchannel`, the console-select request and the volunteer request
names the subchannel in the mode texts as `CPU0` to `CPU3` or `console`; the
event-subchannel request stores its subchannel and the model generates no events.
Further declared choices, from the lifecycle implementation review: `power up` from the
locked prompt prints a fresh prompt line before the `entering` text, as the transitions
table records, and from the escaped state prints the `returning` text alone; `power down`
with the node already off changes nothing and prints nothing but the prompt; the
transient `Powering Up` is not written, the display going straight to `Powered Up`; the
log ring drops its oldest entry when full; processor console output is accepted on
subchannels 0 to 4 and shown in console mode; `select <rack> <slot>` and every other
unmodeled `select` form answer `ERROR: bad subchanel selection.`; the `l1` command
prints the image's `<CTRL-D> to exit.` form; a registered event subchannel of 4 or more
survives `reboot_l1` and a lower one does not, as the request catalogue records.
**Protocol parity (fork commits `6510a6aab8`, `9fe10524b6` and `a86ec10d19`, cleared by clean implementation review in fork PR #428).** The
responder now follows the [requests](#requests), [addressing](#addressing),
[subchannel semantics](#subchannel-semantics) and [receive tolerance](#receive-tolerance)
sections as the catalogue revision gives them, with these declared choices, each
with a discriminating qtest: the router error reply is sent for every non-local
form, including the idle attached-I/O case the contract leaves open between that
reply and silence; the command interpreter runs the same command runner the operator
uses, returns one text field per output line, and answers minus 1 with the
`ERROR: command not found.` line for an unknown command, a status value this earlier
general command contract left unknown (the selected `debug` malformed-token
prediction appears in the selected task-3 reading (citation: `docs/research/l1-debug-task3-firmware-reading.md`)); a command-interpreter request whose single argument is not text
answers minus 100; the Bedrock console tasks echo the first argument's low byte and,
with the node off, the minus-1 warning reply; tasks 2, 14, 15, 16 and 19 are dropped
with a log line; a receive frame of one or two bytes is dropped as short and a longer
one by its check sequence, and any well-checked frame of three bytes or more is
delivered (until 2026-09-17 the model's minimum was five bytes, which also dropped
every one-character console-output frame; see the short-frame revision note
above); the PROM-request log line's source
address carries the configured node position and the request's subchannel; the settings the general task
reads (`debug-switches`, `partition`, `system-serial`, `brick-serial`, `processors`
for the mask reply) are properties with the two-bit setting starting at zero; port
speeds answer two zero integers and the attached-brick requests rack 0, bay 0, type
0 per port; `0x0007` answers minus 104 for every component, `0x0011` minus 106 with
status 1, `0x4000` minus 104, `0x1009` accepts its arguments without a panel action,
`0x100c` is accepted without storing its entry, the PCI slot requests answer minus
100 for an absent handler; the rack-bay-type request returns the character of the
`brick-type` property, `ip45` by default, mapped as the requests table gives; an
EEPROM chunk above 116 bytes answers minus 105 rather than the firmware's counted
but omitted field; `0x1001`, `0x1006` and `0x1008` answer minus 105 for a non-integer
argument as the console-select request does; the firmware's routing-failure log lines
are not written to the model's log ring; `l1dbg console` and its INFO line are not
modeled; the reason codes are 1 COMMAND, 2 PANEL and 3 OS with the
first reply integer of `0x000f` the latest reason; `0x2006` is a power cycle
(power down then up); the first `power down`, `pwr d` or front-panel request while
the node is up sends the `0x8000` event on the registered subchannel with
diagnostic number 0 and defers, and the second acts, while the guest's `0x2005` and
the command interpreter's `* pwr d` act at once; the `0x100b` log line names port 0.
Not modeled yet: `env`, `date`, `config`, `uart`, `debug`, `flash`, `network`, `cpu`,
`port`, `serial`, the `autopower` command, environmental events other than the
deferred power-down, serial-number pre-power checks, and any peer over the RS-422
link.

Several request families, the UART connection to Bedrock, full UART timing and error
causes, raw-UART mode and environmental, JTAG and I2C behavior remain incomplete.
These gaps prevent treating the responder as a qualified physical L1 replacement.

## Role

The L1 is the per-chassis system controller. The published Origin 300 User's
Guide describes IP45 L1 logic for fans, temperature and LEDs, and an I/O-8
assembly with a rear L1 console connector and a separate management USB port
(007-4365-002 printed pages 10 and 12). The published controller guide calls
that USB connection a slave port toward an upstream controller (007-3938-001
printed page 3). The selected L1 software has M-Bus monitoring, cached board
records, JTAG scan and peer-routing paths; those compiled paths do not by
themselves identify fitted chips, bus peers or physical port counts. Public
Linux/OpenBSD consumers and the selected software use a node-facing UART
packet protocol that can carry the operator console. The Origin 300 guide
records "L1 running" on the display with mains power before the rest of the
chassis is powered (007-4365-002 printed page 35).
The current QEMU device answers reviewed node-facing requests for one chassis;
the full controller also has the board-side interfaces described below. Their
implementation needs revision-matched contracts and independent tests.

Three public consumers speak to it. OpenBSD sends four request kinds and never uses it
as a console. The Linux SN1 port sends more and uses it as its early console. The
out-of-tree MIPS port uses it only as a console. What the stock PROM sends is in no
public source and is the firmware trace's first job. The stock PROM's and IRIX 6.5.30's
requests are now read from their own images under [consumer
expectations](#consumer-expectations).

## Register map

The UART is reached at Hub special space, the node's remote-form register window plus
`0x80`: node HSPEC base plus `0x3000_0080`, one 16550 register per 8 bytes, accessed
as 64-bit loads and stores of which only the low byte is significant (OpenBSD `hub.h`
lines 68 to 73 and `l1.c` lines 97 to 142; Linux SN1 `l1.c` lines 61 to 77;
`ip35-l1.c` lines 28 to 50). All three consumers use the remote form; none uses the
local-form window for it. The memory and directory unit's junk-bus timing register
carries setup-and-hold and enable fields for the UART beside the flash PROM and the
Synergy devices (Linux SN1 `sn1/hubmd.h` line 135 and lines 1059 to 1097), and the
L1's own `uart` command lists this port as the Bedrock's; what no public source
states is whether that unit decodes the Hub special window at `+0x80` or another unit
forwards it (open; the memory-directory datasheet carries the register).

**The model also decodes the same eight registers at the local form's own `+0x80`**
(node HSPEC base plus `0x1000_0080`), sharing the remote form's device state, register
layout and refusal rules
([`ip35-address-map.md`'s Hub special space](ip35-address-map.md#hub-special-space)
owns the window split). No public consumer above establishes this: none of the three
reads the local form for the UART. One local-form consumer was identified against
this emulator rather than against a public header, so it is an inferred consumer
requirement, not a measured IP45 decoder fact: sMDK, the retained Origin 300
field-diagnostic package's diagnostic monitor, boots headless under the stock PROM's
`console=d` and writes its own console straight to the L1 UART, polling line status
through the local form: run `prom-20260917T085255-4a78e8` (QEMU
`v11.1.0-3333-g56fd2a52a8-dirty`, `origin300`, `-cpu R14000 -m 512`, IP35 PROM 6.210
built 26 August 2004) reaches the stock PROM's System Maintenance Menu with two CPUs
enabled and `ALL DIAGS PASSED`, netboots sMDK, and reaches its own `smdk>` prompt
(`docs/boot-logs/firmware.smdk-local-uart-v2-banner.2026-09-17T08:52:55+00:00`). The
stock PROM's own L1-protocol console client uses the remote form only, per this
datasheet's ["stock IP35 PROM" consumer reading](#the-stock-ip35-prom). Before this local decode existed, the model routed
local-form offset `0x80` into the Bedrock's general processor-interface register file
instead (`docs/reviews/ip35-l1-local-remote-window-review.md`, now superseded); that
was a modeled register-window shape, not a demonstrated hardware behavior, and no
consumer above ever read it. A discriminating hardware read of Hub special local
offset `0x80` on a physical Origin 300 or Origin 350 is what would settle whether
real silicon answers there at all, in either form.

| Offset from the block | 16550 register | What the consumers require |
|---|---|---|
| `0x00` | receive and transmit data (divisor low with DLAB) | a write hands a byte to the frame parser; a read pops the next byte the L1 has for the processor |
| `0x08` | interrupt enable (divisor high with DLAB) | accepts writes; the Linux SN1 port writes 0; its interrupt build would enable the receive interrupt only, and nothing calls that path |
| `0x10` | interrupt identification on read, FIFO control on write | the SN1 port writes `0x01` then `0x07`; on read, `0x01` (no interrupt) in polled operation |
| `0x18` | line control | a read-write latch; the SN1 port writes `0x80` for the divisor, then `0x03`, and reads it back |
| `0x20` | modem control | accepts writes; the SN1 port writes `0x22` |
| `0x28` | line status | bit 5 (transmitter holding empty) and bit 6 (transmitter empty) set whenever a byte can be accepted; bit 0 set exactly while a received byte waits; bits 2 and 3 clear |
| `0x30` | modem status | bit 4 (clear to send) set; the Linux SN1 early path and the MIPS port spin forever without it (`l1.c` line 1973; `ip35-l1.c` lines 56 to 57) |
| `0x38` | scratch | unused |

The SN1 port programs a divisor of 13 against a 12 MHz clock for 57,600 baud, and the
L1's own `uart` command reports its Bedrock-side port at 57,692 baud, which is that
divisor exactly (007-3938-006 printed pages 110 to 111). A fourth configuration of
the same port, with its L1-protocol option off (`config.in` line 80), registers the
UART with the generic 8250 driver at the same window, register shift 3, and a baud
base of 124,800, which implies a clock assumption of about 2 MHz and a divisor of 2
for 57,600, sending raw bytes with no framing (`kernel/setup.c` lines 236 to 259).
So a model must not enforce any divisor. OpenBSD never programs the UART and polls
line status up to a million times per byte with no wall-clock bound (`l1.c` lines
100 to 144); a silent UART costs it nine such waits per node (one display write,
eight module probes) plus one per on-board IOC3, and a boot without a display line,
memory-module devices or a valid Ethernet address.

## Behavioral contract

### Framing

Frames are HDLC-like, as in PPP: a `0x7e` flag on each end; `0x7d` escapes a `0x7e` or
`0x7d` in the payload or checksum by exclusive-or with `0x20`; a 16-bit frame check
sequence initialised to `0xffff`, updated per byte through the standard table,
complemented and sent low byte first, covering every payload byte from the header
byte to the last argument byte; a receiver's running check over payload plus the two
check bytes must equal `0xf0b8`. All three consumers agree on every one of these
(OpenBSD `l1.c` lines 156 to 306 with `ppp_defs.h`; Linux SN1 `l1.c` lines 737 to
1006 and 1186 to 1474; `ip35-l1.c` lines 99 to 238). Frames shorter than two bytes
are dropped by all three. Repeated flags between frames are tolerated by the SN1
receiver alone (`l1.c` lines 1239 to 1242 and 1327), and the model's own parser
tolerates them too; **OpenBSD and the MIPS port do not**. OpenBSD waits for one flag
and reads to the next, so a doubled flag ends an empty frame it rejects, and its
retry then discards the real frame that follows (`l1.c` lines 266 to 293, 525 to
526); the MIPS port returns to idle after every frame and idle ignores everything but
a flag, so a doubled flag loses the next frame and a closing flag shared with the
next opening flag loses that frame (`ip35-l1.c` lines 193 to 217). **The emitter rule
the model must obey:** every frame it sends carries exactly one opening flag and one
closing flag, no idle flags between frames, and never a closing flag doubling as the
next frame's opening flag. Payloads are at most 127 bytes, the SN1 send-side clamp
and the seven-bit binary length.

The header byte carries the packet type in bits 7:5 and the subchannel in 4:0:
request `0x00` (processor to L1, and console input from the L1), response `0x20`,
event `0x40` (L1 to processor, and console output from the processor). Subchannels 0
to 3 are per-processor consoles, 4 the system console, 16 to 31 for operating-system
requests (OpenBSD uses 16 for everything; the SN1 port allocates 16 to 31, and only its
compiled-out listener would hold 31). A response goes back on the request's subchannel.

A request payload after the header: a four-byte big-endian destination address, a
two-byte request code, an argument count byte, then typed arguments: `0x00` plus four
bytes for an integer, `0x01` plus bytes and a terminating zero for text, `0x80` with
the length in the low seven bits plus the bytes for binary. The SN1 port omits the
count byte on zero-argument requests; OpenBSD's builder always includes it. A
responder accepts both. A response payload: a four-byte signed code, a field count,
then typed fields. Codes: 0 success, minus 100 wrong argument count, minus 101
unknown request, minus 104 data not available, minus 105 argument out of range, minus
107 data invalid (checksum failure). OpenBSD decodes the whole word and maps the named
codes to errors. The SN1 port keeps only the low byte of each reply word and its
callers test for a negative result, so it never sees a negative code: minus 104
arrives as 152 and is taken as success with the output fields left as they were
(`l1.c` lines 133 to 139 and 2467 to 2471). The model's error replies therefore
serve OpenBSD; to the SN1 port only code 0 means anything, which is why the type-4
choice under behavioral notes avoids an error code. The code list in this paragraph is
the consumers' vocabulary; the codes the firmware actually sends, with minus 100 meaning
too few arguments and the additional minus 1, minus 102, minus 106 and minus 108, are the
[reply-code table](#reply-codes) of the protocol-parity revision (change 6 under
[contract changes](#contract-changes-in-this-revision)), and the type-4 choice that
sentence refers to is withdrawn by change 14.

### Addressing

The 32-bit destination: bits 31:28 controller type (0 L1, 1 L2, 2 L3, 3 attached
C-brick, 4 attached I/O brick); 27:18 rack, `0x3ff` meaning local; 17:12 bay, `0x3f`
meaning local; 4:0 task (3 the command interpreter, 4 the environmental monitor, 5
Bedrock, 6 general). The local general address is therefore `0x0ffff006`, the local
command interpreter `0x0ffff003`, the attached I/O brick's general task `0x4ffff006`.

Routing, read in this revision from the firmware's router (the events reading named
under "Sources used"; each statement established unless marked, and each replaced model
rule listed under [contract changes in this
revision](#contract-changes-in-this-revision)). Every request that leaves the link
parser goes through one router with one 20-bit key: bits 31:12 of the destination, the
controller-type nibble plus rack plus bay. Bits 11:5 are never examined for routing. The
router's decisions in order: the key equals the local identity (type 0, own rack, own
bay; for `001c01` the key `0x00041`) or equals `0xffff` (type 0, rack `0x3ff`, bay
`0x3f`), so the packet is local, and the task is bits 4:0 (bits 29:24 when bit 31 is
set), where task 0 or above 24 is "bad task ID" and a task with a registered request
receiver gets the packet while one without is "no receiver"; any other key is looked up
in a forwarding table of exact 20-bit keys, which this image never populates, then handed
to the catch-all receiver if one is registered; with no catch-all, "no receiver". The
catch-all receiver is registered at boot by the brick-to-brick link module for every
brick type (choosing the compute-brick receiver for `4` and `C`), including the Origin
300's, and is replaced by the USB module's receiver
when an L2 attaches over USB and logs `USB0: registered as remote`. A mixed form, one
coordinate concrete and the other the local sentinel, is not local, and neither is any
other controller type, including type 4 with the local sentinels.

Two error paths exist and they differ on the wire. A router failure inside the L1 (bad
task, no receiver on the local key) logs `warning: SCC-<n> inbound request error
IRouter:bad task ID.` or `warning: SCC-<n> inbound request error IRouter:no receiver for
packet.` and sends nothing. A catch-all failure (the link module cannot deliver) sends a
router error reply on the request's subchannel: header `0x20` or-ed with the subchannel,
`ff ff ff ff`, `00 00 00 08`; the first word is the reply code minus 1, which the public
header names the "iRouter error" (`ksys/l1.h` line 229), and the second is the router
error number, 8 meaning "no receiver for packet". On the selected image's type-4
forwarding path, any enqueue failure, including a full peer queue, becomes number 10
before this reply is sent; the lower enqueue helper internally distinguishes a full
queue as number 11, but the type-4 caller masks that distinction. Other catch-all
paths have not been traced to the same depth. Some of these paths also log `B2B: no receiver
(REQ)`. The general task's argument checks (minus 100, minus 101, minus 104, minus 105,
minus 107) apply only after the router delivered the packet; a routing failure never
produces one of them.

The address forms against the firmware's response on a standalone Origin 300 (no L2 on
USB, no peer chassis on the RS-422 ports, no I/O brick with its own L1):

| Destination form | Router outcome | What the processor sees |
|---|---|---|
| type 0, rack `0x3ff`, bay `0x3f` (`0x0ffffxxx`) | local | the normal reply from the addressed task |
| type 0, own rack, own bay (`0x00041xxx` for `001c01`) | local | the normal reply |
| type 0, rack `0x3ff` with own bay, or own rack with bay `0x3f` (mixed) | catch-all; searched as a foreign brick, not found | router error reply, code minus 1, number 8; log `B2B: no receiver (REQ)` |
| type 0, another rack or bay | catch-all; module table search for that rack and bay; with a cabled peer whose entry matches, forwarded over the link; otherwise not found | router error reply, minus 1 / 8; log line |
| type 1 or 2 (L2, L3), any rack and bay | catch-all; the "send to the L2" path; no L2 route recorded | router error reply, minus 1 / 8; log line |
| type 3 (attached C-brick), and type 6 | catch-all rewrites the type to 0 and tries the C-to-C link port for the requester's node; port not connected | router error reply, minus 1 / 8; no log line |
| type 4 (attached I/O brick) | catch-all rewrites the type to 0 and selects software peer slot 9, configured to UART selector 4 on the IP45 branch; a task-5 tag adds its decoded port index | if either read or write handle is closed, router reply minus 1 / 8; if both are open, enqueue with no immediate reply; enqueue failure answers minus 1 / 10 even when the queue is full. The normal uncabled handle state is unknown |
| type 5 | catch-all rewrites the type to 4 and tries C-to-C port 1; not connected | router error reply, minus 1 / 8 |
| types 7 to 13 | catch-all; treated like type 0 with a foreign rack and bay | router error reply, minus 1 / 8; log line |
| bit 31 set (types 8 to 15 as a nibble) | the key can never be local; the catch-all's L2 path | router error reply, minus 1 / 8; log line |
| any of the above with bits 11:5 nonzero | bits ignored | as for the same form with the bits clear |
| local key, task 0 or 25 to 31 | bad task ID | nothing; log line |
| local key, task with a request receiver: 2 (system management port), 3 (command interpreter), 6 (general), 14, 15 (JTAG scan), 16, 18, 19 | delivered | that task's reply (the general task's minus 101 for an unknown code) |
| local key, task 5 or 21 | delivered to the link writer | a two-byte request-type frame ([subchannel semantics](#subchannel-semantics)) |
| local key, any other task, including 4 (environmental monitor) and 1 | no receiver | nothing; log line |

Caveats: which of the two catch-all receivers runs is chosen at boot from the stored
brick-type character, and both candidate characters for an Origin 300 (`4` and `C`)
select the compute-brick receiver described here; the "not connected" outcome on
rows other than type 4 assumes the RS-422 and C-to-I/O ports have no peer,
which is the stock single chassis. With a
peer cabled, type 3 requests reach the peer's L1, which then answers as itself for the
rewritten type-0 form. The independent selected-image peer-link
review (citation: `docs/reviews/l1-rs422-router-evidence-review-20260915.md`) fixes the
type-4 software choice as slot 9 and the conditional handle and enqueue
replies. It does not establish whether an uncabled Origin 300 opens both
handles or whether any packet reaches a peer. A matching stock-PROM type-4
request, L1 handle status and Bedrock reply capture would settle the normal
one-chassis outcome. The router
error reply on the mixed-form, foreign rack and bay and type 1, 2 and 7 to 15 rows
assumes the module's stored L2-route field is clear, which is the "no L2 route recorded"
state the reading found; with the field marking "route unknown" the packet is queued
toward the L2 path and nothing is answered unless that queue is full; the field's value
on a standalone Origin 300 was not traced (settle by the same capture that settles the
type-4 row). The attached-I/O address on an Origin 300: this image has no receiver for a type-4 key and
does not answer any request on the type-4 address as itself; the IO-8 is not an
"attached I/O brick" to the firmware, having no L1 of its own and no link port, and the
`cfg` output on real Origin 300s lists the attached-I/O positions as `not connected` (the
retained captures in `docs/library/boot-logs.md`). The consequences for each consumer,
and the PROM reading's contrary transcript evidence, are recorded in change 14 of the
[contract changes](#contract-changes-in-this-revision). The previous model rules
(independent resolution of each local sentinel, acceptance of mixed forms, bits 11:5
required zero, silent drops for foreign destinations, and the type-4 view answering
`0x0003` locally) are withdrawn; the destination
review (citation: `docs/reviews/l1-destination-routing-review.md`) records the evidence and reasoning
that produced them.

The prompt names the chassis as rack, type letter and slot, `001c01` for a stock single
chassis; the type letter is one of c, i, p, r, x, n, g, or the manual's own `?` for a type it
does not recognize (established: 007-3938-006 printed page 60, "Valid Brick Types"). A second chassis is given slot 02 before cabling
(established: 007-4365-002 printed page 38). Rack and slot live in the L1's non-volatile
memory. A real pair of cabled Origin 300s appears in the PROM's inventory as `Module
001c01` and `Module 001c02` (the openbsd-sgi list capture of 2014 in
`docs/library/boot-logs.md`).

### Management USB device in the selected image

The Origin 300 L1 management connector is a USB **device** connection to an optional
L2, separate from the I/O-8 PCI USB host ports. The selected L1 1.44.0 image
declares USB 1.00, one interface and 64-byte bulk OUT endpoint 1 and IN
endpoint 2 in an 18-byte device descriptor and 32-byte configuration tree.
Under the original USB 1.0 specification, its configuration attributes
`0x60` declare self power and remote wakeup. This describes the image's
function, not measured power or L2 acceptance. The descriptor
review (citation: `docs/reviews/l1-usb-descriptors-routing-evidence-review-20260915.md`)
and USB control evidence
review (citation: `docs/reviews/l1-usb-control-evidence-review-20260915.md`) bind these
facts to the selected range and the USB 1.0 specification.

The exact field audit (citation: `docs/research/l1-usb-descriptor-fields-20260915.md`)
and its independent original-byte review (citation: `docs/reviews/l1-usb-descriptor-fields-evidence-review-20260915.md`)
confirm a USB 1.00 device declaration with endpoint-zero maximum packet size
64, one configuration, device class/subclass/protocol `0xff/0xff/0x00`, and
interface class/subclass/protocol `0x00/0x00/0xff`. The 32-byte tree has bulk
OUT address `0x01` and bulk IN address `0x82`, each with a 64-byte maximum
packet. Its `0x60` attributes set USB 1.0's self-power and remote-wakeup bits
while clearing the bus-power bit; `MaxPower = 0` declares no bus-power draw
in the descriptor. These declarations do not measure the fitted supply.
The selected string selector compares two six-entry language choices,
`0x0009` and `0x0000`, rather than the common `0x0409` US-English value.
The visible `GET_DESCRIPTOR` branch selects the device table, configuration
tree or a string pointer and clips intended data to setup `wLength` in
64-byte stages. Identification fields, string payloads, lower transfer
completion and actual host requests remain unresolved; no exact
host-visible descriptor table is cleared by these selected-image facts.

The visible control dispatcher constructs no request-specific data for
`GET_CONFIGURATION`, `GET_INTERFACE` or `SYNCH_FRAME`; it still calls a
generic lower status stage. `SET_CONFIGURATION(1)` sets its top-level
software state to configured while marking both bulk endpoint states
stalled. A matching successful `CLEAR_FEATURE(ENDPOINT_HALT)` can change
each to the active state that the USB receive task waits for. These are
selected-image control paths, not observed host replies or proof that an L2
issued either clear request. The lower probe makes byte accesses at
controller-base offsets `0x18` and `0x1b`, but its pattern does not
identify a fitted USB chip or a board-level register map. The L1 QEMU
model has no management USB device; a revision-matched component record and
L2 setup/bulk capture would settle fitment and host-visible responses.

The checked instruction trace of four USB C-generation timeouts (citation: `docs/research/l1-usb-timeout-instruction-reading-20260915.md`)
and its independent original-byte review (citation: `docs/reviews/l1-usb-timeout-instruction-evidence-review-20260915.md`)
establish additional selected **software** decisions. The two status handlers
and two setup tasks are fully decoded in both program banks, with matching
instruction shapes; which bank executes on an IP45 remains unknown. The
`GET_STATUS` path constructs device status bit zero as one, interface status
zero, and endpoint status from cached RAM state. It recognizes control
endpoint zero regardless of direction, bulk OUT one only with direction
clear, and bulk IN two only with direction set;
state two produces a clear halt bit, state one produces a set halt bit, and
other query errors build no status value. Its intended two-byte little-endian
data are `01 00` for the fixed device bit or a stalled endpoint and `00 00`
for the interface or active endpoint, clipped to the setup length before a
lower send callback. USB 1.0 defines device bit zero as current power
status, so the image's fixed one does not prove fitted self power. No intended
byte here is a measured L2 response.

The control task proceeds only after its lower read callback returns exactly
eight setup bytes. It swaps the setup value, index and length words, dispatches
by direction, and retries after a nonzero dispatcher result through a lower
recovery callback. The read dispatcher routes `GET_STATUS` and
`GET_DESCRIPTOR` to their builders; `GET_CONFIGURATION`, `GET_INTERFACE`
and `SYNCH_FRAME` construct no request-specific data before the generic
status stage. Its 24-byte indexed **data** table routes write-direction
`CLEAR_FEATURE`, `SET_FEATURE`, `SET_ADDRESS` and `SET_CONFIGURATION` to
helpers, while `SET_DESCRIPTOR` and `SET_INTERFACE` reach a common status
stage without a request-specific state write or data read in this task.
For an initialized non-`0x81` software slot, the indirect lower callback
has a checked operation-dispatcher target. Revision `0x81` instead selects
an unresolved RAM callback. A software retry is not proof of
a USB bus reset. A future management USB qtest can force test-owned cached
endpoint states, distinguish `01 00` from `00 00` and check setup-length
clipping while keeping generic-stage results explicitly unsupported. It
would test a selected-software approximation, not physical host acceptance.
A revision-tied EP0 setup/status/bulk capture together with
the lower callback result and fitted chip record would discriminate intended
software bytes from an actual zero-length stage, stall or timeout. Until
those results are reviewed, a future QEMU management USB device must mark
generic status-stage replies and physical endpoint halt behavior unsupported
or as an explicitly tested approximation; these instruction paths alone
authorize no host-visible success response.

The selected image attempts a ColdFire byte-register software slot at
`0x40000000` when its controller count is positive and a second slot at
`0x60000000` only when that count is two. Each reads revision offset `0x18`
after a scratch write at `0x1b`. Revision `0x81` branches through an
unresolved RAM callback; only other values enter the visible USS-820-family
probe, which checks a high-nibble-one revision and scratch read-back. In that
visible path, endpoint selection at byte offset `0x0a` makes FIFO and transfer
register use stateful; USB interrupt latch and enable are separate from a
16-bit ColdFire parent mask. `SET_ADDRESS` first validates the seven-bit
address without a visible register write, and only after its status stage
writes byte offset `0x10` and updates software state. These are
independently checked firmware register uses (citation: `docs/research/l1-usb-register-map-firmware-reading.md`)
with a bounded source
review (citation: `docs/reviews/l1-usb-register-firmware-evidence-review-20260915.md`).
That earlier firmware-only reading did not match silicon-register names;
the public Agere sheet below supplies a later chip-family offset match.
The alternate callback,
fitted chip, responding software slot, interrupt wiring and L2 host timing
remain unknown.

Agere's public USS-820D revision-4 sheet independently assigns the visible
path's byte offsets `0x18` to `REV`, `0x1b` to `SCRATCH`, `0x0a` to `EPINDEX`,
`0x10` to the seven-bit `FADDR`, `0x14` to endpoint-done status `SBI`, `0x1a`
to the shared-bit update control `PEND`, and `0x1c` to `MCSR`. Its table gives
`REV` values `0x10`, `0x11` and `0x13` for B, C and D respectively, matching
the selected image's quiet probe values. `MCSR.DPEN` drives a D+ pull-up
control **pin** in that chip family; the image's zero/one writes at `0x1c`
suggest connect/disconnect intent only if that family and external pull-up
are fitted and wired. The public register map (citation: `docs/research/uss820d-public-register-map.md`)
and its independent page-image review (citation: `docs/reviews/uss820d-public-register-evidence-review-20260915.md`)
establish chip-family semantics. Neither the offset match nor the `REV`
match establishes which chip is on IP45, which conditional software window
responds, or a hardware-visible USB transition at the type-B connector.

The selected lower-callback trace (citation: `docs/research/l1-usb-lower-controller-trace-20260915.md`)
and its independent original-byte review (citation: `docs/reviews/l1-usb-lower-controller-evidence-review-20260915.md`)
resolve the visible path after a successful non-`0x81` probe. Its installed
operation callback uses an 18-entry signed-offset table. Operation eight
serves the selector-six setup and status-out reads and selectors four/five
for control-IN data on indexed endpoint zero. In particular, the intended
`GET_STATUS` bytes reach endpoint-zero FIFO data/count writes through that
operation. Operation `0x10` belongs to a separate lower write helper and
selects indexed endpoint two for bulk IN. The interrupt callback tracks
pending completion through byte offsets `0x12`, `0x14` and `0x16`; reset
initializes eight internal endpoint pairs, of which the descriptor exposes
only EP0, bulk OUT one and bulk IN two. These are selected-image software
paths, not captured USB transactions. The alternate `0x81` callback,
responding software window, fitted silicon, interrupt wiring and host
status-stage result remain unknown.

#### Management USB router stream at the software callback

The selected image's management USB receive and send tasks pass router
packet segments through bulk callbacks. This is a separate software stream
from the Bedrock UART's PPP flags, escaping and frame check. The
bounded bulk-stream reading (citation: `docs/research/l1-usb-bulk-wire-reading.md`) and its
independent original-byte review (citation: `docs/reviews/l1-usb-bulk-wire-evidence-review-20260915.md`)
clear the following fields **at the ColdFire callback buffer**, not as
captured bytes on the Type B connector or as an L2 parser contract.

| Callback byte offset | Width | Selected software use |
| --- | ---: | --- |
| `0x00` | 2 | Big-endian segment byte count, including the count itself |
| `0x02` | 1 | Builder writes discriminator 2; the receive stream does not check it |
| `0x03` | 1 | Builder flag, complete meaning unknown |
| `0x04` | 1 | Low byte of the transaction number |
| `0x05` | 1 | First-segment bit 7; remaining-segment count in bits 6:0 |
| `0x06` | 1 | Packet class: request 0, response 1 or event 2 |
| `0x07` | 1 | Source port/subchannel field, full external meaning unknown |
| `0x08` | 4 | Big-endian destination router address |
| `0x0c` | 4 | Big-endian source router address |
| `0x10` | 1 | Argument-descriptor count, also copied into continuations |
| `0x11` | 1 | High byte of the transaction number |
| `0x12` | 2 | Big-endian request or event code |
| `0x14` | variable | First-segment argument descriptors and inline data |

The sender's first 16 allocation bytes are internal links and ownership
state; it submits from the following byte, so the table starts at the
callback's byte zero. The first segment has at least 20 callback bytes.
The builder permits at most 64 eight-byte argument descriptors, caps a
segment allocation at 1,024 callback bytes, and rejects a chain needing
128 or more segments. For a valid `N`-segment chain, the first count byte
is `0x80 | N`; continuations count `N-1` down to 1. The receive stream
requires the first flag initially and checks the low seven count bits on
continuations, without a separate repeated-first-flag rejection. A zero
receive result, missing first flag or wrong continuation count frees the
incomplete packet. The receive task asks its lower callback for 256 bytes
at a time and has an overflow-reallocation path; its resulting capacity
and compatibility with the builder's 1,024-byte limit are unresolved.

The writer advances only after a lower write reports the entire embedded
segment length. A zero, short or negative report frees the queued packet;
a negative report on a request-class packet also attempts an error reply.
After five negative lower writes, one wrapper can silently skip later
device writes while reporting the requested count to its caller. A
nonnegative bulk read resets that counter. This is a selected-software
discard path, not evidence that a USB transfer completed. The callback
field map, segmentation and errors can support an internal software test;
they do not clear a QEMU USB device, endpoint transfer success, short- or
zero-length USB packet behavior, fitted silicon, or L2 acceptance. A
revision-matched Type B/L2 capture with setup and bulk boundaries, the
callback result and transaction IDs would discriminate those outcomes.

### Requests

The protocol the L1 serves on its Bedrock-side UART, read in this revision from the
retained firmware's own dispatch and handlers (the request catalogue named under "Sources
used"), per destination task. The rows that the previous revisions cleared from the three
public consumers and the model's own choices are kept where the firmware agrees and
replaced where it does not; every replacement is listed under
[contract changes in this revision](#contract-changes-in-this-revision). Marks: **E**
established (read directly from the firmware's control flow or data tables, or from a
printed manual), **I** inferred (established facts plus a naming or usage assumption),
**A** approximated (a simplification the model may use), **U** unknown (what would settle
it is under [unknowns from the catalogue](#unknowns-from-the-catalogue)). Firmware facts
are the behaviour of one image, version 1.44.0; other versions may differ.

**From frame to request** (E unless marked). A completed frame with a good check sequence
is classed by header bits 7:5: type 0 (`0x00`) is parsed as a request; type 2 (`0x40`) is
processor console output and goes to the L1's console path; types 1 and 3 (`0x20`,
`0x60`) are ignored without a reply. A request is decoded as the header (subchannel in
bits 4:0), a four-byte big-endian destination, a two-byte big-endian code and then, only
when the frame is at least eight bytes long, a count byte followed by that many typed
arguments (`0x00` plus four bytes integer, `0x01` plus text to a terminating zero, any
other tag binary with the length in its low seven bits). A seven-byte frame is a
zero-argument request, so the count byte is optional for zero arguments, as the
consumers already required. Neither the count nor the argument lengths are checked
against the frame length: a count larger than the frame's contents makes the firmware
read stale bytes past the frame. The requester must send well-formed frames; the model
may reject malformed ones (A). Internally the code is argument zero, so a packet holds at
most 63 wire arguments; a frame declaring more is not delivered and gets no reply. The
subchannel is recorded in bits 11:5 of the request's source address, and the reply goes
back to that source, which is how a reply returns on the request's subchannel; any
subchannel 0 to 31 is accepted for a request. Routing then reads bits 31:12 and 4:0 of the
destination as [addressing](#addressing) describes. For a local packet the task must be
1 to 24 and must have registered a request handler, else the request is dropped with no
reply (the link logs `warning: SCC-<n> inbound request error IRouter:<text>.`
unconditionally, as [addressing](#addressing) says). Handlers are either a routine
run in the link's context (tasks 6, 14, 18) or a queue drained by the task (tasks 2, 3, 5, 15, 16, 19, 21); a full queue also drops the request silently. Request codes are
dispatched by exact value; an unrecognised code on task 6 answers minus 101 with no
fields. The argument count is checked per code, almost always as "at least N" (extra
arguments are accepted); too few answers minus 100. Argument types are not checked except
by the display requests (minus 102); an integer handler given a text or binary argument
reads the field's storage pointer as the value and answers no error. Range checks are per
code and answer minus 105. A handler builds its reply as a list beginning with the reply
code; the link serializer emits the code as four big-endian bytes, a count byte equal to
the number of fields, then the fields with the request tags. The dispatcher initialises the list with code
0 before dispatching, so every request it dispatches is answered; an action handler
that adds nothing answers 0 with no fields.

**Tasks that answer requests from the processor in this image** (E):

| Task | Registered as | Kind of service | Scope |
|---|---|---|---|
| 2 | queue | system-management-port command relay: a request with one text argument; replies text | out of scope (peer or L2) |
| 3 | queue | command interpreter: request `0x0000` (and any other code) with one text argument | in scope |
| 5 | queue | Bedrock console: the request is re-sent to the processor as a console-input frame | in scope as a note (I) |
| 6 | routine | general requests, the table below | in scope |
| 14 | routine | configuration exchange between L1s | out of scope |
| 15 | queue | JTAG scan task (the `scan` command family's), codes not catalogued | out of scope |
| 16 | queue | flash programming, codes `0x0072` to `0x0074` | out of scope |
| 18 | routine | loopback test: echoes the request | out of scope |
| 19 | queue | PCI-expansion flash programming, codes `0x0092` to `0x0094` | out of scope |
| 21 | queue | same queue as task 5, chunked output form | out of scope |
| 1, 4, 7 to 13, 17, 20, 22 to 24 | none | no request handler: silent drop | |
| 0, 25 to 31 | invalid | silent drop | |

Task 4, the environmental monitor of the public headers, registers no request handler in
this image; it is the source of the events the L1 sends to the processor
([L1-initiated packets](#l1-initiated-packets)). Task 1, the peer router, has no local
handler either.

#### Task 6, general requests

Column key: Sent by names the consumers known to send the code, from the public sources
and the readings this datasheet records; Args are wire arguments (tag, meaning); Reply is
the code then the fields in order; Class is the storage class of the returned values (C
constant, NV non-volatile setting, L live measurement or state, K counter, X echo of an
argument, D derived from non-volatile identity data). Codes not listed answer minus 101.

| Code | Name (this datasheet's) | Sent by | Args | Success reply | Error replies | Effects | Class | Mark |
|---|---|---|---|---|---|---|---|---|
| `0x0001` | read debug switches | the SN1 port has the function with no kernel caller at the pinned tag; the stock PROM (the [surrounding sequence](#what-the-stock-prom-sends-and-expects)); the stock PROM once on the master CPU after `0x100a`, into its flags word (`0x10d` reproduces the verbose transcript); IRIX has no caller | none | 0; int: the 16-bit virtual debug switches | none | none | NV | E |
| `0x0002` | read rack and bay | the SN1 port has the function, no kernel caller; neither the stock PROM nor IRIX | none | 0; int: rack (10 bits) shifted left 6, or-ed with bay (6 bits) | none | none | NV | E |
| `0x0003` | read rack, bay and brick type | the SN1 port at every node attach (local address) and PCI attach (attached-I/O-brick address), the only request its kernel calls; the stock PROM, before `0x000e`; the stock PROM on the local address for brick classification and module identity, on `0x4ffff006` for its link-good line (three times in the transcript) and on its own concrete rack and bay; IRIX at every controller-context initialisation, per node hwgraph path, per I/O bridge attach and per crosstalk widget, on the local address for the Origin 300's widgets 14 and 15 | none | 0; int as `0x0002`; int: the brick-type character stored in non-volatile memory, `0x34` (`4`) when the type is set to "ip45" (the image's type-name table maps the name `IP45` to `4`), `0x43` (`C`) for "ip35", `0x33` for "ip37" (two other types substitute a different character under conditions out of scope: a type `8` module reports `7` under one identity byte, a type `=` module reports `#` when its per-position settings value is zero) | none | none | NV | E |
| `0x0004` | read brick serial number | the SN1 port has the function, no kernel caller; the stock PROM on the local address as its primary node identity request when `OverrideNIC` is absent, and on its own concrete address for the I/O board record (inferred); IRIX through no reachable path | none | 0; text: the serial number as stored in the board EEPROM (length as stored; the model's six characters remain its choice, from the printed examples) | minus 104 when no serial is available | none | D | E |
| `0x0005` | read firmware revision | the SN1 port has the function, no kernel caller; the stock PROM once, checking against 1.12.5; IRIX per module and per I/O widget for its hwgraph labels and once per R14000 node for its 1.3.20 check | none | 0; three ints parsed from the version string: 1, 44, 0 for this image | none | none | C | E |
| `0x0006` | read EEPROM area | OpenBSD for each memory module's SPD record (a size probe then 64-byte chunks) and, at the on-board Ethernet's attach, for the logic board's board information area to find the Ethernet address (on one Origin 300 under OpenBSD 5.5-beta that query yielded no address and the driver fell back to all ones; the openbsd-sgi list capture of 2014); the SN1 port for chassis and board areas and serial numbers; the stock PROM in 64-byte chunks from offset 0 with no size probe for board records during I/O discovery, and possibly SPD; IRIX for the node board (component 0, chassis then board area) and the I/O board (component 1), 64-byte chunks until the remaining count is 0 | four ints: component index, area (0 to 4), byte offset, byte length | 0; int: bytes of the area remaining after this chunk; then, only when length is non-zero, one binary field of the smaller of length and (area size minus offset) bytes | minus 100 fewer than four args (extra accepted); minus 104 component index absent (its EEPROM read failed, or the index is at or beyond the record count) or the record kind does not fit the area (areas 0 to 3 need a board record, area 4 a memory-module record); minus 105 area above 4, or offset at or past the area size; minus 107 the area was flagged invalid when the L1 read the EEPROM (any area, board and chassis included); the absent check precedes the range check | none | D | E |
| `0x0007` | read EEPROM format | neither the stock PROM nor IRIX | one int: component index | 0; int: record kind (1 board, 2 memory module); for kind 1 six more ints (three area sizes, the constant 8, two area offsets) then two ints; for kind 2 two ints (SPD size and offset) | minus 100; minus 104 not present or index at or beyond the record count (the presence check runs first, so minus 105 is never reached) | none | D | E for shape, U for the meaning of each int |
| `0x0008` | read system serial number | the SN1 port has the function, no kernel caller; the stock PROM during local I/O discovery; IRIX on demand from its system-id call | none | 0; text: the eight-character system serial number (the model's `L` plus seven digits fits) | minus 104 when the stored value is empty | none | NV | E |
| `0x0009` | read partition id | the SN1 port has the function, no kernel caller; the stock PROM in its partition check; IRIX only when it believes the system is partitioned | none | 0; int | none | none | NV | E |
| `0x000a` | read port speeds | the SN1 port has the function, no kernel caller; the stock PROM after its link-good check (first int 0 selects 400); IRIX never | none | 0; two ints derived from per-port non-volatile speed bits (brick-type dependent selection; the model's two zero integers remain its choice) | none | none | NV | E for shape, I for meaning |
| `0x000b` | read a two-bit setting | the stock PROM (a code the payload can send, per the cleared PROM-side reading); the stock PROM in its partition check (nonzero starts a bulk transfer toward an L2); IRIX never | none | 0; int: a two-bit non-volatile field | none | none | NV | E for shape and width, U for meaning |
| `0x000c` | read attached bricks, ports 1 to 4 | the stock PROM (as `0x000b`); the stock PROM once from its crossbar arbitration routine before the banner, on `0x4ffff006` on an Origin 300; IRIX never | none | 0; four pairs of ints: (rack shifted left 6 or-ed with bay, type character) for the module seen on each of ports 1 to 4 | none | none | L | E for shape |
| `0x000d` | read attached bricks, ports 5 and 10 | neither the stock PROM nor IRIX | none | 0; two pairs of ints as `0x000c` for ports 5 and 10 | none | none | L | E for shape |
| `0x000e` | node position of the requester | the stock IRIX 6.5.30 IP35 PROM, once per early-initialisation cycle from its local-identity query, right after `0x0003` on the local general address and before its first console output, without the count byte; on an Origin 300 it discards the value, and the repetition seen in the retained run is a fault loop owned by issue #765, not a retry (Origami issue #846; the boot record `firmware.boot.2026-09-07T07:16:56-07:00`); no public consumer or header has it; IRIX immediately after every `0x0003` in its module-id path and once more per node hwgraph path (its hwgraph `slab/<n>` link) | none; the firmware reads none and rejects none, and the PROM sends none | 0; one integer, the node position the L1 holds for the Bedrock port the request came in on: 0 on an Origin 300. Contract in [Request 0x000e](#request-0x000e). The catalogue reading of this revision called the value "the link index the request arrived on (0 for the Bedrock UART)", marked inferred; the protocol catalogue review (citation: `docs/reviews/l1-protocol-catalogue-review.md`) finds that a looser name for the same value, the position decoded from the request's source field by the cleared subsection's rule, so the subsection's meaning stands. It is 0 on a standalone Origin 300; the [PROM-request debugging switch](#request-logging-switch) on a real machine shows the field | none | none | L | E |
| `0x000f` | read last event reasons | neither the stock PROM nor IRIX | none | 0; two ints copied from the lifecycle module's last-event state | none | none | L | E for shape; I that they are reason codes, which that module names 1 COMMAND, 2 PANEL, 3 OS, 4 ENVIRONMENTAL, 5 DON'T CARE; which int is which is not established |
| `0x0010` | read processor masks | neither the stock PROM nor IRIX | none | 0; int: number of mask words; int: number of processors; then per word two ints (two bitmasks, one bit per processor) | minus 108 when the processor count is zero | none | L | E for shape, I for meaning |
| `0x0011` | read a 64-byte scan record | neither the stock PROM nor IRIX | none | 0; one binary field of 64 bytes | minus 106; int: internal status | none | L | E for shape, U for content (JTAG) |
| `0x1001` | write debug switches | neither the stock PROM nor IRIX | one int | 0 | minus 100 | writes the non-volatile switches | NV write | E |
| `0x1002` | select console subchannel | the SN1 port has the function, no kernel caller; neither the stock PROM nor IRIX | one int: subchannel | 0 | minus 100 | sets the active console subchannel for the requesting port, combined with the requester's node position and stored without a range check ([Request 0x000e](#request-0x000e) and [subchannel semantics](#subchannel-semantics)) | L write | E for shape, I for meaning |
| `0x1003` | volunteer as console node | the SN1 port has the function, no kernel caller; neither the stock PROM nor IRIX | none | 0 | none | records the local module as console owner | L write | E for shape, I for meaning |
| `0x1004` | write display text | the SN1 port has the function, no kernel caller; never the stock PROM (its line-index arithmetic allows it, no caller passes 0) nor IRIX on an Origin 300 | one text | 0 | minus 100; minus 102 the argument is not text | the text is written to display line 2, the software line; non-printable bytes become `?`, short text is space-padded to the line width, up to 72 characters are kept for the scrolling display path | L write | E |
| `0x1005` | write display text | OpenBSD at every node attach, twelve space-padded characters "OpenBSD/sgi " (`ip27_machdep.c` line 439); the SN1 port has the function with no kernel caller; the stock PROM only on failure paths; IRIX never on an Origin 300 | as `0x1004` | as `0x1004` | as `0x1004` | identical to `0x1004`: both codes write the same line, line 2; line 1 keeps the brick identity | L write | E |
| `0x1006` | set partition id | the SN1 port has the function, no kernel caller; the stock PROM only after a nonzero `0x000b`; IRIX through its user-level device | one int | 0 | minus 100 | writes the non-volatile partition id and notifies the configuration exchange | NV write | E |
| `0x1007` | set event subchannel | the SN1 port has the function, no kernel caller (its compiled-out listener would ask for 31); IRIX once, with 23, at its console driver's attach; the stock PROM never | one int: subchannel | 0 | minus 100 | records port and subchannel for L1-to-processor events and persists the subchannel (the low five bits; the position bits are not stored) in non-volatile memory, restored at the next L1 boot when 4 or more and under a start-time condition the review did not identify ([L1-initiated packets](#l1-initiated-packets)) | NV write | E |
| `0x1008` | set the two-bit setting | neither the stock PROM nor IRIX | one int | 0 | minus 100 | writes the field read by `0x000b` | NV write | E for shape |
| `0x1009` | front-panel control | the stock PROM (as `0x000b`; two arguments); the stock PROM in pairs around stage changes and on error paths; IRIX only on IP34 and letter-`9` boards | two ints: value, selector (low four bits 0, 1 or 2; bits 7:4 an extra parameter for selector 2) | 0 | minus 100; minus 105 selector above 2 | one of three panel actions (LED or indicator control, inferred) | L write | E for shape, I for meaning |
| `0x100a` | clear event subchannel | the stock PROM (as `0x000b`); the stock PROM once on the master CPU after brick classification; IRIX never | none | 0, no fields | none | disables L1-to-processor events and clears the persisted subchannel | NV write | E |
| `0x100b` | report PROM version | neither the stock PROM nor IRIX | three ints: major, minor, patch-release number | 0, no fields | minus 100 unless exactly three | stores the value per port; writes `N<port>: PROM <major>.<minor> (PR <patch>)` to the L1's log (major and minor in hexadecimal); if the event subchannel is on this port, clears it as `0x100a` | L write | E |
| `0x100c` | per-processor setting | the stock PROM (as `0x000b`); the stock PROM as the first request of the boot (stage 20, every CPU on its own subchannel) and at diagnostic-monitor entry (stage 70); IRIX with states 30, 10, 50, 80 and 90 | two ints: value (low byte), processor number | 0, no fields | minus 100 unless exactly two | writes a per-processor entry indexed by port times 4 plus processor | L write | E for shape, U for meaning |
| `0x2001` | system reset | IRIX at reboot when its panic-reboot setting is 2; the stock PROM never (the SN1 header's reset is `0x2000`, below the table) | none | 0, no fields (a reply the reset it triggers may cut off, which is why the SN1 helper accepts a timeout) | minus 106; int: internal status | performs the reset the `reset` command performs; the L1 log line is `reset (OS)` (reason code 3 prints as `OS`) | action | E |
| `0x2002` | PCI slot operation A | neither the stock PROM nor IRIX (the SN1 header's PCI codes are `0x2001` to `0x2003`, which do not match) | two ints, low bytes used: bus, slot (`0xff` all) | 0, no fields | minus 100 when the hook is absent (the PCI expander module installs all three at start; whether it runs on an IP45 was not traced); minus 106; int: status | one of the three PCI slot power and reset operations; which is up, down and reset is not established | action | E for shape, U for which |
| `0x2003` | PCI slot operation B | as `0x2002` | as `0x2002` | as `0x2002` | as `0x2002` | second of the three | action | E for shape, U |
| `0x2004` | PCI slot operation C | as `0x2002` | as `0x2002` | as `0x2002` | as `0x2002` | third of the three | action | E for shape, U |
| `0x2005` | power down | IRIX at halt after an L1 power-off event; the stock PROM never | none | 0, no fields | minus 106; int: status | performs the `power down` command's action; log line `power down (OS)` | action | E |
| `0x2006` | power cycle | the stock PROM (as `0x000b`); the stock PROM on failure paths (the secondary-cache test, a PLL workaround) and from its diagnostic monitor; IRIX never | none | 0, no fields | minus 106; int: status | power down then power up, both with reason code 3 (`power down (OS)` then the power-up log line): the power cycle the PROM names it | action | E |
| `0x2007` | read expander byte | IRIX's Odyssey graphics driver, not on an Origin 300 | none (exactly zero) | 0; int: a byte masked with `0xf8` | minus 108 on brick types other than four router and I/O types (an Origin 300 answers minus 108); minus 100; minus 106; int | none | L | E |
| `0x2008` | write expander byte | IRIX's Odyssey graphics driver, not on an Origin 300 | two ints (exactly): value byte, mask (bits 7:4) | 0 | minus 108 as `0x2007`; minus 100; minus 106; int | I2C expander write on those brick types | action | E |
| `0x4000` | read processor and memory inventory | neither the stock PROM nor IRIX | none | 0; one text field per inventory record: comma-separated fields beginning with the record's index and one of `IPMI,...`, `JEDEC,...`, `PIROM,...` with `OKAY`, `DISABLED`, `NOT_PRESENT`, `ERROR,<text>` states, or `UNKNOWN` | minus 104 when the inventory is empty | none | L | E for shape |
| `0x4002` | reboot the L1 | neither the stock PROM nor IRIX | none | 0, no fields, sent if the restart routine returns (not traced) | none | logs `L1REQ_REBOOT requested...` and restarts the controller | action | E for the log line, I for the restart |

Notes on the table.

- **`0x2000`**, the SN1 header's full reset, is not a recognised code: the dispatcher
  serves `0x2001` to `0x2008` and sends `0x2000` down its unknown-request path, minus 101
  unless a board-specific hook is installed, and no installer exists in this image (E).
  The SN1 port has the function, uncalled, and accepts a reply timeout.
- **Shared handler for `0x0002` and `0x0003`.** One routine serves both; the second
  integer is added only for `0x0003`. The rack and bay come from the same non-volatile
  fields the prompt (`001c01`) is built from.
- **EEPROM component table on an Origin 300.** The IP45 board module registers six
  EEPROM records in this order: 0 node board, 1 IO-8 board, 2 memory module 0, 3 memory
  module 2, 4 memory module 1, 5 memory module 3 (E: read from the board module's device
  table and its registration call). The request's component index is the record index
  unchanged (the identity mapper, installed by the general task's initialisation when the
  board installs none; no other installer exists in the image: E). So memory
  module n is component 2 + (n shifted right 1) + 2 times (n and 1). Areas: 0 the
  eight-byte common header, 1 chassis, 2 board, 3 internal use (board records only), 4
  SPD (memory-module records only). Indexes 6 and above answer minus 104: the presence
  check runs first and the range compare is never reached.
- **EEPROM reply size.** The serializers ([limits](#limits)) accept a field when its
  offset plus its length is below 128, counting the header byte in the offset but not
  the field's tag byte, so the largest binary field is 116 bytes and the frame then
  carries 128 payload bytes including the header; 117 bytes or more drops the field, and
  every later one, while the count byte still counts the first dropped field, so the
  reply carries count 2 and one integer. It never returns minus 105 for size. The
  previous revision recorded a disagreement between a 127-byte and a 128-byte limit; the
  protocol catalogue review (citation: `docs/reviews/l1-protocol-catalogue-review.md`) settled it as stated here (both
  readings were partly right: a trailing integer keeps the payload at 127 bytes or fewer).
- **Wrong argument type.** Only the display requests check the tag. Every other handler
  takes an integer argument's value from the field; for a text or binary field it takes
  the field's storage address instead, producing a meaningless value and no error. The cleared [Request 0x000e](#request-0x000e) subsection had described the `0x1002`
  handler as converting a text argument to a number; the protocol catalogue review
  decided the point for this reading, and that wording is corrected. The model's minus 105 for a text argument to
  `0x1002` stays the declared model choice of that subsection.
- **Board-specific unknown-code hook.** The dispatcher would defer an unknown code to a
  board-installed handler before answering minus 101; no such handler is installed in
  this image (E: no writer of that slot exists in the code).
- **PROM-request debugging.** With `l1dbg promreq on` (007-3938-006 printed page 102)
  every task-6 request writes `L1REQ: <source address, 8 hex digits> - <code, 4 hex
  digits>` to the L1's log before it is handled ([request logging
  switch](#request-logging-switch)).

#### Modeled power requests `0x2005` and `0x2006`

The reviewed request table establishes the common wire contract: both requests take no
arguments and answer code 0 with no fields. Request `0x2005` powers the node down, and
`0x2006` cycles it down and up. The published controller and Origin 300 manuals
establish the operator-visible lifecycle used here: the L1 remains available while
node power is off, and it can power the node down and back up. Public consumer code
establishes the framing and reply conventions. These sources do not establish reply
delivery relative to a power transition, physical power sequencing, behavior when the
node is already off, or how an emulator process should represent node power. The
following rules are bounded QEMU model choices, not claims about hardware timing or
electrical behavior.

- The model queues the fieldless success reply before starting either action. A reset
  may clear the UART queue before the guest reads the reply, so the contract promises
  the order in which the model requests the operations, not delivery before reset.
- On a powered node, `0x2005` sets node power false, writes `Powered Down`, logs
  `power down (OS)`, lowers the `node-power` output and requests a QEMU shutdown. A
  normal QEMU process therefore exits. The `-no-shutdown` option may retain it for
  inspection.
- On a powered node, `0x2006` performs the same down-state and log transitions without
  requesting shutdown, immediately sets node power true, writes `Powered Up`, logs
  `power up (OS)` and raises the `node-power` output. It then requests a whole-machine
  QEMU reset and resumes execution after reset. Resetting the whole emulated machine,
  rather than only the selected physical node, and keeping the QEMU process alive are
  model approximations for the current one-node machine.
- With the node already off, `0x2005` still queues success but otherwise does nothing:
  it adds no log entry, changes no power state or output, and requests no QEMU shutdown.
  `0x2006` skips the down transition, powers the node up, logs only `power up (OS)`,
  raises `node-power`, requests the same whole-machine reset and resumes execution.
- Firing a node reset (`0x2001`, the `reset` command, or the front-panel reset), the
  power-off half of `0x2005`/`0x2006`, or the restart half of `0x2006`/`power up` queues
  the QEMU reset or shutdown request on the requesting processor's own work when a guest
  write reached the device (`current_cpu` set). The queued callback makes the QEMU
  request, then calls `cpu_pause` for every processor before the main loop handles
  that request. The L1's host-facing console
  chardev callback reaches the same code from the main thread with no processor to queue
  on; it synchronously pauses all virtual processors under QEMU's big lock before making
  the request. This is an **independently reviewed model choice**, not a
  hardware fact: no published source states a processor-stop timing or contract for
  these commands. It mirrors the landed ELSC scheduling and stop-all-processors
  pattern (QEMU `68fbffd7e21a`). A register-only
  qtest cannot pin it -- `-accel qtest` never runs guest code, so `current_cpu`
  stays unset. `tests/functional/mips64/test_sn1_l1_reset.py` runs real guest
  instructions. One processor spins while the other sends a framed `0x2001`
  across twenty resets; a second case sends twenty power cycles through the
  host console. The exact-head focused check passed at QEMU `7b1558769c6e`.

QEMU commit `2b17c7ad9bee9eb96b3f198b2825d1fb225775c6` implements these
choices in `hw/misc/sgi-l1.c`. In `tests/qtest/sn1-l1-test.c`, its
`sn1/l1/general-actions` case covers `0x2005` from the powered state and both starting
states for `0x2006`. Its `sn1/l1/power-cycle` case starts QEMU
without `-no-shutdown`, sends `0x2006` while the node is on, observes RESET, waits for
the VM to run again and then reads the down/up log entries. That case distinguishes a
node power cycle from a QEMU shutdown. QEMU commit
`b5df1c2b612744150cea2ab1801419779ca0adc2` adds the
`sn1/l1/power-requests-off` case. It starts with node power off and without
`-no-shutdown`, checks that `0x2005` returns success without changing the low power
output or log, then checks that `0x2006` raises the output once, produces RESET, resumes
the VM and logs only `power up (OS)`.

**Model inputs for EEPROM records** (model choices, kept from the cleared text). EEPROM
records are optional runtime inputs supplied through explicit file-backed model
properties. The model has no built-in board, chassis, or memory-module identity. An
unconfigured component within the six-entry table returns minus 104 with no fields, the
firmware's answer for a component whose EEPROM read failed or whose index is beyond the
table. For a configured file, a
size probe reports its length and a nonzero-length request returns at most the requested
bytes from the requested offset. SPD validation covers bytes 0 through 63, whose final
byte checks bytes 0 through 62; a failed check returns minus 107; bytes after byte 63 do
not affect that check. The selected Origin 300 L1 1.44.0 firmware reads 160 SPD bytes,
compares byte 63 with the sum of bytes 0 through 62 modulo 256, and marks the area
invalid on mismatch. The general handler answers minus 107 for an area the controller
marked invalid, including board and chassis areas. The same selected image
accepts the eight-byte board-record header and each nonempty board area only
when the low byte of that span's byte sum is zero. The reader rejects an area
whose encoded end equals or exceeds its configured EEPROM capacity. An area
checksum failure marks that area invalid, so `0x0006` can answer minus 107.
On the normal continuing IP45 firmware branch, record registration probes
DIMM routes 2 through 5, temporarily reads the node-board record, then scans
all six entries before starting the general request task. The IO8 BIA and
present SPD records are loaded into RAM or marked invalid during that scan;
`0x0006` serves the RAM state without clocking the M-Bus. A present board
record with a corrupt common header answers minus 107 for a valid header,
chassis or board-area request because the stored status check precedes offset
bounds. A failed first IO8 read can leave presence set and answer minus 107;
a failed early DIMM probe clears presence and answers minus 104. Later indirect
refresh calls and scan completion on reboot or failure paths are untraced.
The special internal-use-area length rule remains unknown.
The independent M-Bus and SPD evidence review (citation: `docs/reviews/l1-mbus-usb-evidence-review-20260915.md`)
clears the SPD criterion. The board-record evidence
review (citation: `docs/reviews/l1-board-record-validation-evidence-review-20260915.md`)
clears the checksum and strict-capacity rules, and its focused recheck clears
the preload and bad-header/first-read reply paths on the visible normal
continuing IP45 branch. A controlled EEPROM
capture would test the running controller's invalid-state response. Board,
chassis, and other area files are served
unchanged by the model when not flagged; their consumers may apply format-specific checks
after retrieval. Published record dumps document the expected shape but are not copied
into the model or tests. The EEPROM contract
review (citation: `docs/reviews/l1-eeprom-and-attached-io-contract-review.md`) records the distinct
device and consumer validation responsibilities.

#### Environmental M-Bus monitoring

The selected IP45 path also registers three monitor software routes with
8-bit M-Bus address forms `0x58`, `0x5a` and `0x5c`, 17 I/O expander
descriptions, ten supply descriptions, five fan tachometer descriptions
and two temperature records. The high interrupt worker handles monitor
indices 0 and 1, and the low worker handles expander indices 0 through
15; the M-Bus transaction-complete interrupt is separate. Task 4 creates
conditional supply, fan and temperature monitor workers and sends
threshold events, although it registers no request receiver. The `env` and
`fan` console views report monitor state; request `0x0006` serves cached
board-record RAM instead.

The selected M-Bus transaction trace (citation: `docs/research/l1-mbus-env-task-trace-20260915.md`)
and its independent original-byte review (citation: `docs/reviews/l1-mbus-env-task-evidence-review-20260915.md`)
show byte accesses at own-address, divider, control, status and data
offsets matching the published MCF5206e map: `MBAR+0x1e0`, `+0x1e4`,
`+0x1e8`, `+0x1ec` and `+0x1f0`.
The selected software writes divider `0x13`, which the published revision-1
manual maps to 480; its revision-3 errata relates SCL to the processor
clock divided by that value. With no measured IP45 controller clock, the
SCL rate in hertz remains unknown. A serialized selector and software lock
surround transfers. A bus-free check and reset check each stop after at
most 1,000 status reads; neither count is an elapsed time or a complete
transaction retry. The interrupt continuation distinguishes completion,
arbitration loss and ACK failure, and its read sequence uses repeated START,
a final-byte NACK and STOP. A negative cached task-6 reply does not by
itself identify a live M-Bus fault. Task-4 workers conditionally attempt
event sends through the registered subchannel; host receipt and physical
threshold crossings are unmeasured.

Fan and temperature workers make up to five read attempts. On each
successful read, the selected code adds the sample and divides its running
sum by the current attempt count; a later failed attempt leaves the last
computed value unchanged. Numeric task waits are not calibrated sampling
periods. These software tables do not establish fitted sensor or expander
parts, responding address population, PCB branches or real interrupt
timing. The environmental M-Bus
reading (citation: `docs/research/l1-environment-mbus-firmware-reading.md`) and its
independent corrected-finding
review (citation: `docs/reviews/l1-environment-mbus-evidence-review-20260915.md`) bind the
selected-image paths and their limits. The current QEMU L1 model has no
M-Bus sensor transactions; a matching board inventory and timed bus/IRQ
capture would discriminate the physical topology and threshold behavior.

#### Task 3, command interpreter

| Code | Sent by | Args | Success reply | Error replies | Effects | Mark |
|---|---|---|---|---|---|---|
| any (`0x0000` by convention) | OpenBSD and the SN1 port send "* pwr d" to power off; IRIX sends `* pwr d` at halt (resent every three seconds), `env reset` after an auto-power-down event, and `pci ...` slot commands rewritten to the local address on an Origin 300; the stock PROM sends no text command | one text: the command line, with the same target prefixes the L1 prompt accepts (`*`, module names) | status code (0 when the command ran), then zero or more text fields, one per non-null output item | when the argument count is not one, or the command fails to parse or send: a non-zero status code in the same shape (general values unknown; see the selected `debug` reading (citation: `docs/research/l1-debug-task3-firmware-reading.md`) for its narrower prediction) | whatever the command does: `pwr d` powers down, `pwr u` powers up, `rst` resets, and so on; commands for other modules are forwarded over the peer links | E for structure, U for the general status values; the text fields are read from the output collection path, the specific output of `pwr d` was not (medium confidence; the reply shape was not re-verified by the protocol catalogue review) |

The request code is not examined: any code with one text argument runs the text as a
command. The argument tag is not checked. Two optional board-installed filters may
rewrite the reply before it is sent; whether either is installed on the fitted
board remains unknown. The
machine powers off on `pwr d` as the cleared row required.

In the selected 1.44.0 image, `env reset` returns success with no text even before
environmental monitoring initializes. After initialization it clears cached
supply, fan and temperature warning and fault state, reloads thresholds, and
restores the environmental power-down semaphore count to one. If monitoring is
initialized and a countdown is running, its next 500-task-unit check can take
the restored semaphore count, emit event `0x4102` with diagnostic `0x163`, and
exit without calling node power-down. A node that is already off has a separate
exit with the same diagnostic; a pre-initialization success cannot guarantee an
abort. A countdown that reaches zero instead emits diagnostic
`0x164` and powers down with environmental reason four. A persistent fault may
start another countdown. The task-clock interval, fitted sensors, threshold
crossings and whether an IRIX request reaches the L1 before the next check are
unmeasured. The checked firmware
reading (citation: `docs/research/l1-env-reset-debug-firmware-reading.md`) and its
independent evidence
review (citation: `docs/reviews/l1-env-reset-debug-firmware-evidence-review-20260915.md`)
establish these selected-image branches, not an Origin 300 elapsed time or sensor
contract. The QEMU L1 currently does not model the countdown or the command's
monitor-state effect.

#### Task 5 and task 21, Bedrock console (I)

A request delivered to the local task 5 is not answered; it is passed to the Bedrock
link's writer, which sends the low byte of the first argument to the processor as a
two-byte request frame (`0x00` plus subchannel, one character) on the port and subchannel
encoded in the destination's bits 11:5; when the node's power is not on it instead
answers the requester with a minus-1 reply whose text is `WARNING: power on <id> appears
off!`. This is the operator-keystroke path of console mode seen from inside. Task 21 shares the queue and uses a chunked output form. Neither
is something a processor-side model needs to serve beyond the frame it echoes.

#### Tasks 2, 14, 15, 16, 18 and 19, out of the single-chassis model's scope

| Task | Codes | Args | Reply | Mark |
|---|---|---|---|---|
| 2 | not examined | one text | status code, text fields (command relay to the system management port) | E for shape |
| 14 | not catalogued; responses are dispatched on codes 0 to 15 | | | U |
| 15 | JTAG scan task (the `scan` command family's); a request queue registered at start, codes not catalogued | | | U |
| 16 | `0x0072` (flash status; first int argument at most 4), `0x0073`, `0x0074` | at least one int for `0x0072` | `0x0072`: 0 then about eleven fields (integers and text describing the two flash images); others unknown; an unknown code is logged as `Flash task received unknown request <n>` with no reply seen | E for codes, U for detail |
| 18 | any | up to three arguments are echoed | reply code equal to the request code, fields echoing the arguments | I |
| 19 | `0x0092`, `0x0093`, `0x0094` | exactly one argument, else logged as invalid | unknown; unknown codes logged as `PBay flash task received unknown request <n>` | E for codes |

#### Reply codes

Codes the firmware can send on the Bedrock link, with the condition for each:

| Code | Condition | Fields |
|---|---|---|
| 0 | success | as tabulated |
| minus 1 | routing-layer error from the peer-link layer or the chunk assembler: cannot forward a non-local request (detail 8 no receiver, 10 queueing error), chunked-transfer faults (details 23 to 26) | no count byte: the four code bytes `ff ff ff ff` are followed by four bytes `00 00 00 dd` with dd the detail number; a consumer reading a count byte sees 0 |
| minus 100 | too few arguments for the code (extra arguments are accepted); also `0x2002` to `0x2004` when their hook is absent | none |
| minus 101 | unrecognised request code on task 6 | none |
| minus 102 | display request whose argument is not a text field | none |
| minus 104 | data not available: EEPROM component absent or its index at or beyond the record count, record of the wrong kind, brick serial absent, empty system serial, empty inventory | none |
| minus 105 | value out of range: EEPROM area above 4, offset past the area; `0x1009` selector above 2 | none |
| minus 106 | an action failed | one int: the internal status |
| minus 107 | the requested EEPROM area was flagged invalid when the L1 read the device | none |
| minus 108 | not supported on this brick type (`0x2007`, `0x2008` on an Origin 300), or no processors known (`0x0010`) | none |

Minus 103 and other values from the public header's numbering are never sent. Local
delivery failures (unknown task, unregistered task, full queue) produce no reply at all
on the Bedrock link. How each consumer reads these codes is under [framing](#framing):
OpenBSD decodes the whole word; the SN1 port keeps the low byte, so minus 1 arrives as 255
and minus 104 as 152, both taken as success; the stock PROM and IRIX decode the whole word
and require the count byte to equal the fields they expect ([consumer expectations](#consumer-expectations)).

#### Limits

- **Receive frame.** Up to 256 unescaped payload bytes plus the two check bytes; the
  257th and 258th bytes are accepted only as the check sequence, and a 259th byte drops
  the frame. Frames of one or two bytes are dropped as short frames and frames of three
  bytes or more are check-sequenced ([receive tolerance](#receive-tolerance)); an empty frame (two flags in a row) silently
  restarts; an escape byte followed by a flag drops the frame; bytes before the first
  flag are discarded (a diagnostic is logged once). A bad check sequence drops the frame
  with no reply; the next good frame is handled normally (E).
- **Arguments.** At most 63 wire arguments; text arguments end at the first zero byte;
  binary arguments carry at most 127 bytes; none of the lengths is checked against the
  frame (E).
- **Reply frame.** The response and event serializers accept a field when its offset plus
  its length is below 128, counting the header byte in the offset but not the field's
  tag byte: a trailing integer keeps the payload at 127 bytes or fewer, the largest binary
  field is 116 bytes and the frame then carries 128 payload bytes including the header. A
  field that would exceed the limit is dropped and every later field with it, but the
  count byte still counts the first dropped field (E from the serializer, read twice; not
  observed on hardware). For the EEPROM request this allows a 116-byte data field.
- **Subchannels and types.** Requests are accepted on any subchannel 0 to 31 and answered
  on the same one. Response and type-3 frames from the processor are ignored; event
  frames are console output (E).

#### Unknowns from the catalogue

| Unknown | What would settle it |
|---|---|
| Type-4 outcome with no peer: whether software slot 9 has both read and write handles open on an idle Origin 300 decides between a minus 1 (detail 8) reply and deferred forwarding with no immediate reply | a matching stock-PROM type-4 request, L1 handle-status and Bedrock reply capture on an uncabled Origin 300; the selected-image branch and error masking are in the independent peer-link review |
| PCI operation order for `0x2002` to `0x2004` (which is power up, power down, reset); the three hooks exist in this image, installed by the PCI expander module's initialisation | reading the three operation routines behind the hooks, or a capture with `l1dbg promreq on` |
| The meaning of the two-bit setting (`0x000b`, `0x1008`), the `0x000f` pair, the `0x0010` masks, the `0x0007` integers, the `0x1009` panel selectors and the `0x100c` per-processor entry | reading their helper routines against the operator commands that print the same fields |
| Command-interpreter status values and whether `pwr d` produces output lines | reading the command executor's return paths, or a capture of OpenBSD's power-off |
| What "active subchannel" and "console owner" (`0x1002`, `0x1003`) change in the console path | the lifecycle and console material under [console multiplexing](#console-multiplexing) and [subchannel semantics](#subchannel-semantics) |
| The serializer count quirk (a dropped field still counted) and the argument parser's lack of bounds checks are read from the code, not observed | executing the image or a hardware capture with an oversize EEPROM request |
| Tasks 2, 14, 15, 16 and 19 request semantics | out of scope until a supported guest uses them |

#### Contract changes in this revision

Each entry names the cleared wording it replaces, the wording now in force, the evidence
class (established in the firmware's own handling, or inferred) and the effect on the
consumers as the PROM and IRIX readings describe them ([consumer expectations](#consumer-expectations)). Where a change also rewrites a cleared qtest
bullet, the bullet is named. These changes were reviewed as recorded under "Status"; change 8 is withdrawn and
change 15 rewritten by that review.

1. **Brick type character in `0x0003`.** Before: "The letter is the model's chosen
   value: `0x43`, upper-case C, as the 2002 header defines and the consumer lower-cases",
   with the choice among `0x43`, `0x63` and `0x34` left to a trace or capture. Now: the
   stored brick-type character, `0x34` (`4`) when the type is set to "ip45", `0x43` for
   "ip35", `0x33` for "ip37". Evidence: established for the handler and for the mapping of the name `IP45`
   to `4` in the image's type-name table; that an Origin 300 stores that name is what the
   L1's `brick` command printing `C (IP45)` on an Origin 300 shows (007-4498-002 printed
   page 179): the stored name, not the character. Consumers: the SN1 port
   lower-cases the letter; the stock PROM maps `4`, `A`, `6`, `7`, `8`, `9`, `Q` and `0` to
   `C` for its module identity, so `0x34` and `0x43` both give `001c01`; IRIX derives
   `001c01` from `4` by the same table and names the I/O side "Ibrick" from the raw letter,
   where `C` would name it "Cbrick" and produce device paths no real Origin 300 has, so
   for IRIX the answer must be `4`. Rewrites the checklist bullet on the rack-bay-type
   request (type `0x34`, not C).
2. **Reset and power requests.** Before: "full reset (`0x2000`): a reset; the reply is
   optional and a timeout is accepted", and the [node reset](#node-reset-the-l1-drives)
   paragraph's unknown about whether the image honours `0x2000`. Now: `0x2000` is
   unrecognised (minus 101); `0x2001` performs the `reset` command's action, answers 0 with no fields (a reply
   the reset it triggers may cut off) and logs `reset (OS)`; `0x2005` performs `power
   down`, answers 0 with no fields and logs `power down (OS)`; `0x2006` is a power cycle,
   power down then power up, both with reason code 3. Evidence:
   established (dispatch arithmetic and handlers). Consumers: the SN1 port has `0x2000`
   uncalled and accepts a timeout; IRIX sends `0x2001` at reboot when its panic-reboot
   setting is 2, treats no reply as success and falls back to the Bedrock
   network-interface reset on failure, and sends `0x2005` at the end of a shutdown begun
   by an L1 power-off event; the stock PROM sends `0x2006` only on failure paths and
   prints `ERROR: power cycle requst failed[%d], old L1 version ?` if it keeps running.
   Rewrites the checklist bullet on `0x2000`. The [lifecycle unknown](#lifecycle-unknowns)
   on `0x2000` is settled by this entry.
3. **Display lines.** Before: "display line 2 (`0x1005`), and line 1 (`0x1004`)". Now:
   both codes write line 2, the software line; line 1 keeps the brick identity; a
   non-text argument answers minus 102; non-printable bytes become `?`, short text is
   space-padded. Evidence: established. Consumers: OpenBSD writes `0x1005`; the stock
   PROM writes only `0x1005`, and only on failure paths (`Mem Cfg Err`, `Prom Cpy Err`,
   `Scache Err`); IRIX never writes the display on an Origin 300.
4. **EEPROM request `0x0006`.** Before: areas 1, 2 and 4; minus 104 for an absent
   component or record; minus 107 when the stored record fails its 8-bit sum; a chunk
   larger than 115 bytes answered minus 105 (declared model choice); board and chassis
   areas served unchanged with validation left to the consumer; the component index of
   memory module n a source conflict to be implemented as a property. Now: areas 0 to 4
   (0 the eight-byte common header, 3 internal use); the error mapping and its order in
   the row (minus 100 for fewer than four arguments, minus 105 for an area above 4 or an offset at or past the area, minus 104 for an
   index at or beyond the six-entry table, a failed component read or a record of the
   wrong kind, the absent check first, minus 107
   for any area the L1 flagged invalid, board and chassis included); an oversize chunk is
   never minus 105 but a success reply whose count includes the dropped binary field,
   the largest binary field being 116 bytes; the IP45
   component table 0 node board, 1 IO-8, 2 memory module 0, 3 module 2, 4 module 1, 5
   module 3. Evidence: established for the mapping, areas, order and table and for the
   identity component mapper. The selected L1 1.44.0 image checks memory SPD
   byte 63 against the sum of bytes 0 through 62 modulo 256, matching the
   model's SPD check. The board-record header and each nonempty board area
   must have an eight-bit byte sum of zero in the selected image; area
   checksum failure sets its invalid flag. The area end must be strictly below
   the firmware's configured EEPROM capacity. On the normal continuing IP45
   startup path, an all-six record scan precedes general request handling;
   a present bad-header or first-read IO8 entry can answer minus 107, while a
   failed early DIMM probe clears presence and answers minus 104. The
   internal-use-area length exception remains unknown. The model does not
   validate board or chassis area checksums. Consumers: OpenBSD
   probes then reads 64-byte chunks and rejects a longer field; the SN1 port advances by a
   fixed 64; the stock PROM reads 64-byte chunks from offset 0 until the remaining count
   is 0 with no size probe and needs an absent component answered minus 104 quickly;
   IRIX reads 64-byte chunks, copies exactly the field's length and repeats until the
   integer is 0, component 0 chassis then board area, component 1 for the I/O board, and
   its own IP45 arm computes components 2, 4, 3, 5 for memory modules 0 to 3, the
   firmware's table. Rewrites the checklist bullet on the 115-byte boundary.
5. **Command-interpreter reply.** Before: "code 0, no fields; the machine powers off".
   Now: a status code (0 when the command ran) followed by zero or more text fields, one
   per line of output; the request code is not examined; the machine powers off on `pwr
   d`. Evidence: established for the structure; the text fields are read from the output
   collection path at medium confidence, the output of `pwr d` itself unread. Consumers:
   OpenBSD and the SN1 port send `* pwr d`; IRIX accepts code 0 with no fields or with one
   text field, which it copies out to the caller, and its parser requires the count to
   match exactly; the stock PROM sends no text command.
6. **Reply codes and argument checking.** Before: "Codes: 0 success, minus 100 wrong
   argument count, minus 101 unknown request, minus 104 data not available, minus 105
   argument out of range, minus 107 data invalid (checksum failure)" and the checklist's
   "an unknown request code answers minus 101; a wrong argument count minus 100: chosen
   behaviour from the code names, which no consumer exercises". Now: the [reply-code
   table](#reply-codes), adding minus 1 with its detail word and no count byte, minus 102,
   minus 106 with a status integer and minus 108; minus 100 means too few arguments and
   extra arguments are accepted; argument types are unchecked except by the display
   requests. Evidence: established. Consumers: the SN1 port sees only the low byte, so
   minus 1 arrives as 255 and is taken as success; the stock PROM and IRIX decode the
   whole word and fail a reply whose field count or tags differ from what they expect.
   Rewrites the checklist bullet on unknown codes and wrong counts.
7. **Codes served beyond the public header.** Before: every code outside the cleared rows
   answered minus 101. Now: the general task also serves `0x0007`, `0x000b` to `0x0011`,
   `0x1001`, `0x1008` to `0x100c`, `0x2001` to `0x2008`, `0x4000` and `0x4002` as the table
   gives them. Evidence: established for the dispatch and shapes; meanings marked per row.
   Consumers: the stock PROM sends `0x000b`, `0x000c`, `0x1009`, `0x100a`, `0x100c` and
   `0x2006`; IRIX sends `0x100c`, `0x2001` and `0x2005`, and `0x1009` only on boards other
   than the Origin 300's.
8. **Withdrawn.** This entry claimed that `0x2001` to `0x2006`, `0x100a` to `0x100c` and
   `0x4002` send nothing on success. The protocol catalogue review (citation: `docs/reviews/l1-protocol-catalogue-review.md`)
   finds the opposite: the dispatcher initialises the reply with code 0 before it
   dispatches and sends it whenever it is non-empty, so every dispatched request is
   answered and those codes answer 0 with no fields (the ten-byte fieldless frame), the
   `0x2001` reply possibly cut off by the reset it triggers. The consumer sentences stay
   true: IRIX discards the `0x100c` reply and accepts no reply to `0x2001`; the stock
   PROM ignores the `0x100a` and `0x100c` replies.
9. **Event subchannel persistence.** Before: `0x1007` "code 0, no fields", and the
   [controller restart](#controller-restart) paragraph's unknown about whether a guest's
   registration survives `reboot_l1`. Now: the reply is unchanged; the subchannel is
   persisted in non-volatile memory (five bits; the position bits are not stored) and
   restored at the next L1 boot when 4 or more, under a start-time condition the review
   did not identify, so subchannels 0 to 3 do not survive; `0x100a` clears it. Evidence: established.
   Consumers: IRIX registers 23 once at its console driver's attach; the SN1 port's
   compiled-out listener would register 31.
10. **Error replies added to cleared rows.** Before: `0x0004` and `0x0008` "code 0 and
    the documented field shapes". Now: `0x0004` answers minus 104 when no serial is
    available and `0x0008` minus 104 when the stored value is empty. Evidence:
    established. Consumers: IRIX sends `0x0008` on demand from its system-id call and
    `0x0004` through no reachable path; the stock PROM sends local `0x0004` as its
    primary node identity request when `OverrideNIC` is absent.
11. **Destination bits 11:5.** Before: "reserved bits 11:5 are zero" as a condition of
    acceptance, and "nonzero reserved bits receive no response". Now: bits 11:5 of a
    destination are ignored for routing and the request is answered as if they were
    clear; the same bits of a source address carry the node position and subchannel.
    Evidence: established (routing arithmetic). Consumers: none sends them nonzero on a
    one-chassis boot. Rewrites the checklist bullet on destination forms.
12. **Mixed local and concrete forms.** Before: "resolves each local sentinel
    independently ... accepts local, concrete, and mixed forms only when both resolved
    values match". Now: the router compares the whole 20-bit key; rack `0x3ff` with a
    concrete bay, or a concrete rack with bay `0x3f`, is a foreign destination and on a
    standalone chassis receives the router error reply (assuming the module's L2-route
    field is clear, the caveat under [addressing](#addressing)). Evidence: established.
    Consumers: none uses a mixed form. Rewrites the same bullet.
13. **Foreign destinations.** Before: "Other controller types, nonmatching coordinates,
    and nonzero reserved bits receive no response". Now: types 1 to 15 and type 0 with
    another rack or bay go to the catch-all peer-link layer rather than resolve
    locally. With no matching peer or L2 route, many such forms answer the
    router error reply (header `0x20` or-ed with the subchannel, `ff ff ff ff`,
    `00 00 00 08`, no count byte); a queued foreign request has no immediate
    reply. Only routing failures inside the L1 (bad task or no receiver on a
    local key) are silent before catch-all dispatch. Evidence: established for
    the catch-all path, conditional on the L2-route state described under
    [addressing](#addressing). The "not connected" outcome for types 3 and 5
    assumes no peer on their RS-422 paths. Type 4 instead depends on software
    slot 9: a closed read or write handle answers detail 8, both handles open
    allow enqueue without an immediate reply, and enqueue failure answers
    detail 10. The normal uncabled slot-9 handle state is unknown. Consumers:
    the SN1 port sees minus 1 as 255
    and takes it for success with its output fields uninitialised; IRIX sends concrete
    forms only to verify an event from an unknown module; the stock PROM sends concrete
    forms of its own rack and bay, which are local. Rewrites the same bullet.
14. **The attached-I/O address (type 4).** Before: "Type 0 routes to the L1 and type 4 to
    its attached-I/O view", "Type 4 task 6 currently implements only rack/bay/type
    request `0x0003`", and the behavioural note "The model answers code 0 with the
    module's own rack and bay and type `0x34` on the type-4 address". Now: this image has
    no receiver for a type-4 key and never answers a type-4 request as itself; the request
    is rewritten to type 0 and forwarded through software peer slot 9, assigned
    UART selector 4 on the selected IP45 branch. A closed read or write handle
    answers router detail 8; both open handles queue without an immediate
    reply, and an enqueue failure answers detail 10 after the caller masks the
    lower helper's full-queue distinction. The handle state on an uncabled
    Origin 300 remains unknown; the IO-8
    is not an attached I/O brick to the firmware (no L1, no link port; real Origin 300s
    list the attached-I/O positions as `not connected` in `cfg`). Evidence: established
    that no local handler exists and, after the independent selected-image
    peer-link review, established for the conditional slot and error branches;
    the outcome on an idle chassis is the recorded unknown.
    Version and capture limits: the PROM sends `0x0003` to `0x4ffff006` for its
    link-good check and expects code 0 with two integers. A real Origin 300
    transcript with L1 1.18.5 prints `hubii_link_good: 4-brick attached to module
    001c01.`, the success text with the raw type character, but has no retained
    request or reply frame. The selected L1 1.44.0 image has no local type-4
    handler and forwards conditionally through slot 9. Its local `0x0003`
    can return `4`; a peer reply or older-version route cannot be excluded
    for the real line. A matching link capture (an `l1dbg promreq on` log
    shows codes and sources but not destinations; a serial tap shows both)
    would settle which address and reply supplied it. Consumers: the SN1 port
    asks on the type-4 address at PCI attach and sees any error as success with its
    fields uninitialised; IRIX uses the local address for the Origin 300's I/O widgets
    14 and 15 and retries locally after a failure on the type-4 address; the stock PROM
    prints the unknown-brick line on an error and continues. The cleared [Request
    0x000e](#request-0x000e) subsection's sentence on the model's type-4 view now says the
    address is routed, not served, per the protocol catalogue review's finding 14; the
    subsection is otherwise unchanged. Rewrites the checklist bullets on the type-4
    `0x0003` and `0x000e` answers and on task-4 routing; the [behavioural
    note](#behavioral-notes) on the attached-I/O address is rewritten.
15. **Tasks without a handler.** Before: "An unmodeled task receives no response". Now:
    a local task with no registered receiver (1, 4, 7 to 13, 17, 20, 22 to 24; task 15
    has the scan module's request queue) and a
    task number of 0 or 25 to 31 are dropped with a log line and no reply; tasks 5 and 21
    echo a two-byte request-type frame carrying the low byte of the request's first
    argument on the
    subchannel in the destination's bits 9:5. Evidence: established. Consumers: none
    addresses those tasks on a one-chassis boot.

### Request 0x000e

Read from the firmware's own handling of the request in the image named under
"Sources used". Each statement is marked established (read directly in that handling),
inferred (the firmware combined with a manual or a public consumer), a model choice, or
unknown with what would settle it. The reply for the request the stock PROM sends follows
from the reading; nothing here was chosen to advance the PROM.

**Name and task.** Established: the local general task (6) serves `0x000e` through the
same dispatcher as `0x0001` to `0x000a`; the command interpreter (task 3) does not. No
public header or consumer names it: the SN1 header at `v2.5.40` stops at `0x000a`, and a
Library-wide search for the public request-mnemonic prefix found only that header, its
Linux users and OpenBSD. This datasheet calls it "node position of the requester". A
brick can hold more than one node, and the L1 guide names console subchannels per node,
"node|blade 0 console subchannel" through "node|blade 3 console subchannel"
(007-3938-006 printed page 10, with the note that on non-blade systems the number is the
node); the request reports which of those positions the L1 believes the requesting
Bedrock occupies.

**Arguments.** Established: none. The dispatcher requires only that the frame parsed
into a request code; the handler reads no argument and checks neither the count byte
nor any tag. A request carrying extra integer, text or binary arguments receives the
same success reply; there is no minus 100 or minus 105 path. Unknown: whether the
firmware's byte parser accepts the SN1 form with the count byte omitted; the reading did
not reach the byte parser. The model accepts both forms for every request already.

**Success reply.** Established: code 0 and exactly one integer field (tag `0x00`, four
bytes big-endian). The value is computed from the request, not stored for it. The L1
attaches a source address to every request it receives, and for a request from the
Bedrock UART that source is the L1's own address for the port (the stamping happens in
the link's receive path; the reviewer confirmed the address's maintenance and the
handler's use of the source but did not reach the stamping site, and the PROM-request
debugging switch on a real machine is the direct check): the module's rack and
bay in bits 27:12, the Bedrock task (5) in bits 4:0, and in bits 11:5 a seven-bit field
that combines node position and console subchannel. The reply is the position part of
that field. For the Origin 300's brick class the position is the field's upper two bits
(address bits 11:10) and the subchannel its lower five (bits 9:5); two multi-node brick
classes of later products, whose type characters are `6` and `@`, divide the field by
24 or by 20 instead, so their subchannel ranges are 0 to 23 and 0 to 19. The seven-bit
field is the local module's stored console-subchannel byte described under "State".
Inferred: on an Origin 300 the position bits of that byte are zero, because the settings
record initialises to zeros, the console-select request preserves the position bits,
and every other writer is an operator command, a peer-link event or a brick-type
change, none of which runs on its own on a standalone chassis; an operator's console
selection naming a node other than 0 would change it. So
the stock PROM's request to `0x0ffff006` is answered with code 0 and the integer 0.
Established: the same dispatcher serves `0x0007`, `0x000b`, `0x000c`, `0x000d`,
`0x000f`, `0x0010`, `0x0011`, `0x1001`, `0x1004` to `0x100c`, `0x2001` to `0x2008`,
`0x4000` and `0x4002` in this firmware; their contracts are not read here.

**Error replies.** Established: none. The handler has no failure path. Minus 101 comes
only from the dispatcher for a code it does not serve, so a request that is routed to
the general task with code `0x000e` never receives an error. On the attached-I/O
address (controller type 4) the firmware forwards to a peer controller rather than
answering locally (see the cross-check); the type-4 address is routed, not
served (change 14 under [contract changes](#contract-changes-in-this-revision)).

**Externally visible effects.** Established: none. The handler builds the reply and
changes no setting, writes no non-volatile storage, changes no display line and drives
no reset or power line. The next request observes nothing different.

**State, reset and migration.** Established: the seven-bit field is one byte of a
128-byte settings record that the L1 checksums, keeps with a backup copy, and
re-initialises to zeros, apart from a validity marker in its first byte and the check
byte, when both it and its backup copy fail their checksums at L1 start, or
unconditionally when a restart flag is set. The console-select request
`0x1002` rewrites that byte as (current field and `0x60`) or-ed with its integer
argument, then recomputes the port address from the low seven bits: it preserves the
position bits and does not mask the argument, so an argument of 32 or more lands in the
position bits. Established: the record sits at a fixed address inside the on-chip RAM
window the reset code maps, not in flash and not in an I2C device. Inferred: it
survives an L1 firmware restart and is lost with standby power; the opposing inference
and the string-level observation that bears on both are recorded under [what the L1
keeps](#what-the-l1-keeps). Inferred from the
manuals (the L1 issues the reset and its prompt returns afterwards, Example 3-53 on
printed page 91): a processor reset does not restart the L1,
which runs whenever the chassis has mains power and drives the node reset lines itself
(007-3938-006 printed pages 80, 91 and 94; 007-4365-002 printed page 35). Model choice:
keep one seven-bit port field for the single Bedrock port, initialised to the
`node-position` property shifted left five (default 0, the Origin 300 value; valid
values 0 to 3, other values are rejected at realize); `0x1002`
updates it as the firmware does, `((field & 0x60) | argument) & 0x7f`, and an argument
of another type answers minus 105 (model choice; the firmware uses the field's
storage pointer as the value); `0x000e` answers
`(field >> 5) & 3`. Device reset stands for L1 power-on and returns the field to the
property value; a guest processor reset that is not wired to the L1 device leaves it;
migration carries it.

**What a test can vary.** The single integer field rejects an empty handler and a
fieldless success. The `node-position` property, or `0x1002` with an argument of 32 or
more, moves the value off zero and rejects a zero-filled reply: `0x1002` with `0x40`
makes `0x000e` answer 2, and a following `0x1002` with 4 leaves it at 2. Extra or
mistyped arguments must not change the reply. Unknown, and what would settle it: whether
a standalone Origin 300 ever records a nonzero position through the internal
discovery event; the L1's PROM-request debugging switch (007-3938-006 printed page 102)
prints each request's source address on a real machine and would show the field.

#### What the stock PROM sends and expects

Read by a second researcher from the consumer side: the stock IRIX 6.5.30 IP35 PROM
payload named under "Sources used", decoded as MIPS, together with the retained
emulator trace of run `prom-20260907T141650-98dc09` (Origami PR #844). Marks as above;
established here means read in the PROM's decoded instructions or in that trace.

**The request as sent.** Established: the PROM sends `0x000e` to the local general
address with no arguments and no count byte, six payload bytes after the header, so the
frame from the first processor is `7e 10 0f ff f0 06 00 0e e6 b9 7e` (check bytes
computed from the framing rules; the trace shows the responder's check over this frame
passing). The header's subchannel is 16 plus the sending processor's number from the
Bedrock processor-interface CPU-number register, opened for the request and released
after the reply or the deadline; a second processor's frame differs only in the header
and check, `7e 11 0f ff f0 06 00 0e 33 26 7e`. The destination is the local-L1 address
with task 6 or-ed in. Nothing in the frame depends on an earlier reply or a loop index.
The count byte is omitted only for zero-argument requests, as in the SN1 port, so a
responder must keep accepting both forms.

**What it accepts.** Established: exactly one reply shape, a whole 32-bit reply word of
zero, field count 1, and one integer-tagged field. The PROM decodes the reply code as a
whole word, unlike the SN1 port, so minus 101 arrives as minus 101. Any nonzero code,
any other field count, a first field not tagged integer, or extra fields fails the parse,
and the sender then proceeds with the value 0. A missing reply, after a receive deadline
of 2,000,000 units of the PROM's scaled real-time count (units unknown; two seconds if
the count is microseconds), or a transmit failure yields the PROM's own transport-failure
value, not an L1 code, and also proceeds. The request is never retried at this level.
Transmit uses up to 50 attempts 500 units apart if the UART accepts fewer bytes than the
frame. The reply must return on the request's subchannel; whether the PROM also checks
the packet-type bits is unknown.

**What it does with the value.** Established: the request is issued once from the PROM's
local-identity query, immediately after `0x0003` on the same destination, and only when
the packed rack and bay that `0x0003` returned are nonzero; a reply of rack 0 and bay 0
suppresses it. Its value is consumed only there. If the type letter from `0x0003` is `6` (the 2003 public header's
Twister type, IP53 and router), the PROM adds the value modulo 4 to the bay it took from
`0x0003`, and a value of exactly 4 replaces the type with `R`. For every other type,
including the IP45's `4` and the `C` the model answers today, the value is discarded, so
on an Origin 300 no reply to `0x000e` changes any PROM state or decision. That use is
consistent with the firmware reading above, where the value is the requester's position
within a multi-node brick and the module's bay is the brick's; the two readings were
made independently and neither names the code.

**Repetition.** Established from the full trace, read by the PROM-side researcher (the
retained boot record preserves one cycle): the 11,416 repetitions are not an L1 retry.
Each cycle carries one `0x000e` request rejected with minus 101, one posted store to the
local boot window, one faulting eight-byte load from guest address
`0x900000001a1e0000`, and one data-bus-error exception whose handler re-enters early
initialisation, which re-initialises the L1 link and repeats the requests. Nothing in a
`0x000e` reply ends that loop; the boot proceeds identically whether the reply is minus
101, a success with any integer, or nothing. The exit condition belongs to the
flash-window decode question of Bedrock issue #765.

**Disagreement recorded.** Issue #846 and the previous revision's table row describe
the PROM as repeating `0x000e` until it is answered. The PROM-side reading establishes
that the repetition is the fault loop above and that the minus 101 reply is tolerated.
The row now follows the PROM-side reading; the firmware-side value contract is
unchanged by this, since it never depended on the PROM's use. The two readings also
differ in what they can say about the value on an Origin 300: the firmware side infers
0 from the settings record and its writers; the PROM side, having no access to the
L1's meaning, leaves it unknown and notes that any integer satisfies this consumer. Both
stand as written.

**Surrounding sequence.** Established: link initialisation on the local Bedrock UART,
waiting up to 1,000,000 units for transmitter-empty, then 57,600 baud from the 12 MHz
clock with divisor 13, eight data bits, no parity, one stop bit; then `0x0003` (answered
by the model with rack 1, bay 1 and `C`, no trace line, consistent with acceptance);
then `0x000e`; then `0x0001`, whose integer the PROM keeps to sixteen bits and whose
bits 1:0 both set or bit 9 set raise an internal flag on the link record (the model's
zero leaves it clear); then a PROM step carrying a text argument that produced no
console traffic; then the boot-window store and the faulting load. So once `0x000e` is
answered the next request is `0x0001`, already modeled, and no further L1 traffic
precedes the fault. The identity query is also reachable from later boot steps, always
sending `0x0003` then `0x000e`; `0x000e` has one sender in the payload. For `0x0003` the
PROM takes bay from the first integer's low six bits and rack from the next ten, and maps
the letters `4`, `A`, `6`, `7`, `8`, `9`, `Q` and `0` to `C` for its module identity,
so the choice between `C` and `4` on the local address does not change the PROM's
module type. Request codes the payload can send on the local general task that no
public header names, for advance notice: `0x000b`, `0x000c`, `0x1009` (two arguments),
`0x100a`, `0x100c` and `0x2006`; the firmware reading above finds all six served by the
general task, their contracts unread.

**PROM-side unknowns.** The units of the PROM's delays, which follow from the Bedrock
clock-configuration register that scales its real-time count; which processor sent the
retained run's request, which a UART byte trace or a responder log of the subchannel
would settle; and the L1's meaning for the code from any public source, which only a
real-machine capture with the PROM-request debugging switch on would confirm
independently of the firmware reading.

### Firmware cross-check

Findings against the rows cleared in the previous revisions, from the same image,
recorded here as evidence. Each states the datasheet's claim at that time, the
firmware's behavior and a confidence. The previous revision left the cleared rows as
they were; this revision applies these findings and the catalogue's through the
[contract changes](#contract-changes-in-this-revision) under [Requests](#requests).

- **Destination bits 11:5.** Claim: reserved, must be zero, nonzero gets no reply.
  Firmware: routing compares the destination's bits 31:12 with the module's identity or
  the all-local sentinel and takes the task from bits 4:0; bits 11:5 carry the node
  position and console subchannel of Bedrock-task addresses and are ignored when
  routing, so a request with them nonzero is answered. The general-task handlers that
  need the position read it from the request's source, not its destination. Confidence:
  high for the routing arithmetic; the model's stricter rule stays a model choice.
- **Attached-I/O address (type 4).** Claim: the model answers `0x0003` there with the
  module's rack, bay and `0x34`. Firmware: a destination whose bits 31:12 match neither
  the module nor the sentinel is looked up in a peer-forwarding table and sent on; no
  local handler answers it, and on a chassis with no registered peer the request is
  dropped inside the router (whether the public header's "iRouter error", code minus 1,
  is then returned to the requester was not traced). The `4` in the real trace's
  `4-brick attached` line is also what the local `0x0003` reply carries (next finding).
  Confidence: medium; the trace's origin of the digit is not settled by this reading.
- **Rack, bay and type (`0x0003`).** Claim: the type letter is a model choice between
  `0x43`, `0x63` and `0x34`. Firmware: `0x0002` and `0x0003` share one handler; both
  return rack shifted left six or-ed with bay, and `0x0003` adds the module's stored
  type character verbatim, which on an IP45 is `4` (`0x34`) by the later public header
  and the owners' traces; two substitutions exist for other products (a type `8` module
  reports `7` under one identity byte, a type `=` module reports `#` when its
  per-position settings value is zero). Confidence: high for the handler, medium for the
  stored character being `4` on an Origin 300.
- **Full reset (`0x2000`).** Claim: a reset, reply optional. Firmware: the general
  task's dispatcher serves `0x2001` to `0x2008` but sends `0x2000` down its
  unknown-request path, minus 101 unless a board-specific hook is installed (no
  installer was found). Confidence: high for the dispatch arithmetic.
- **Read EEPROM (`0x0006`).** Claim: four integers, minus 104 for an absent component or
  record, minus 107 for a failed record sum, chunk clamped to the remainder. Firmware:
  fewer than four arguments minus 100 (extra arguments tolerated); component index
  beyond the module's table minus 105; component absent minus 104; area outside 0 to 4
  minus 105 (0 is a fifth area the public header does not name; its record must be of
  one kind, else minus 104); a record whose stored invalid flag is set minus 107 (when
  the flag is set was not traced); offset at or beyond the
  record length minus 105; length clamped to the remainder; the integer field is the
  bytes left after the chunk and the binary field is present only when the requested
  length is nonzero; the absent check precedes the range check. Confidence: high. The
  firmware's own limit is 128 bytes including the header byte, which allows a 116-byte
  data field; a larger chunk yields a success reply whose count includes a field it does
  not carry. The model's minus 105 above 115 bytes stays a model choice.
- **Debug switches (`0x0001`), firmware revision (`0x0005`).** Claims: one integer;
  three integers. Firmware: one stored setting; three integers parsed from the version
  string, `1.44.0` giving 1, 44 and 0. Confidence: high.
- **Console select (`0x1002`).** Claim: code 0, no fields. Firmware: agrees, and adds
  minus 100 when the argument is missing and the stored-field update described under
  "Request 0x000e". Confidence: high.
- **Display lines (`0x1004`, `0x1005`).** Claim: one text, code 0, no fields. Firmware:
  a display request whose argument is not text answers minus 102. Confidence: high.
- **Error codes.** Claim: minus 100, 101, 104, 105, 107. Firmware: all five are produced
  by the general task, and so are minus 102 (the display requests, when the argument is
  not text), minus 106 (`0x0011` and `0x2003` to `0x2008`) and minus 108 (`0x0010`,
  `0x2007` and `0x2008`), in handlers not read in detail; none of the three is in the
  public header. Confidence: high that they exist, their conditions unread.
- **Reply layout.** Claim: four-byte code, field count, typed fields. Firmware: the
  reply's serializer writes the header, the four-byte code, a count of the fields after
  the code, and the fields with the public tags, so the wire layout is established from
  the firmware as well as the consumers; its buffer is 128 bytes including the header,
  and a field that does not fit is omitted from the bytes while still being counted.
  Confidence: high.

### L1-initiated packets

Read in this revision from the firmware's serializers (the events reading named under
"Sources used"); each statement is established unless marked. Address words are written
as the public headers define them: controller type in bits 31:28, rack in 27:18, bay in
17:12, task in 4:0. The L1 sends four kinds of packet on the link and no other: it emits
no request-type frame except console input, and never a response-type frame on its own
initiative.

| Packet | Trigger | Header byte | Subchannel | Payload after the header | Acknowledgement |
|---|---|---|---|---|---|
| Environmental event | the environmental monitor detects a voltage, fan or temperature threshold crossing or recovery, a power-supply fault, redundancy loss, an automatic power-down countdown or completion; the operator's first power-off request while the system is up; an operator-issued environmental test event | `0x40` or-ed with the subchannel | the event subchannel the processor set with request `0x1007` (low five bits of the stored value); none is sent while no event subchannel is set | four-byte big-endian source address, four-byte big-endian event code, one count byte, then typed fields exactly as in requests: `0x00` plus four bytes for an integer, `0x01` plus text plus terminator; every environmental event carries two fields, an integer diagnostic number and a text line | none expected or waited for; the firmware queues the event to the link writer with a time limit (the "event suspend" value of `l1dbg qsusp`) and forgets it |
| Console input | a character typed on the L1's console while the L1 is in console mode | `0x00` or-ed with the subchannel | the currently selected console subchannel ([subchannel semantics](#subchannel-semantics)) | exactly one byte, the character; no address, no code, no count | none |
| Router error reply | a request whose destination the L1 cannot route ([addressing](#addressing)) | `0x20` or-ed with the subchannel | mirrors the request's subchannel | `ff ff ff ff` then a four-byte big-endian number giving the router error (8 for "no receiver"); no count byte, no fields; nine payload bytes in all | it is a reply |
| Normal reply | a request that reached a task | `0x20` or-ed with the subchannel | mirrors the request's subchannel | four-byte code, count byte, typed fields, as [framing](#framing) says | it is a reply |

**Environmental events.** Source address: the local module's address with task 4, the
environmental monitor's task, and bits 11:5 zero; for a chassis configured `001c01` that
is `0x00041004`. Which events cross the link: the link writer forwards an event only when
its source task is 4; every other event the firmware generates internally (the `l1dbg
printf` "printf call events", which carry task 1, and link-state and power-sequencing
events on other tasks) is discarded at the link, so the processors see environmental
events and nothing else. Event code: a 16-bit value in the low half of the four-byte
code; the later Linux system-controller driver documents the layout the firmware uses,
class in bits 15:12, severity in bits 11:8, component index in bits 7:0
(`drivers/char/snsc.h` lines 60 to 89 at `981a2edd1922`). The codes this image generates:

| Code | Diagnostic number (integer field) | Text (before the prefix described below) | When |
|---|---|---|---|
| `0x1000` + supply | `0x160` | `<supply> level stabilized @ <v>V.` | a voltage returned within limits |
| `0x1100` + supply | `0x15a` | `<supply> low warning limit reached @ <v>V.` | low voltage warning |
| `0x1200` + supply | `0x159` | `<supply> high warning limit reached @ <v>V.` | high voltage warning |
| `0x1300` + supply | `0x158` | `<supply> high fault limit reached @ <v>V.` | high voltage fault; the power-off sequence follows |
| `0x1400` + supply | `0x157` | `<supply> low fault limit reached @ <v>V.` | low voltage fault; the power-off sequence follows |
| `0x2000` + fan | `0x161` | `FAN <n> stabilized @ <rpm> RPM.` | fan speed returned to normal |
| `0x2100` + fan | `0x15c` | `FAN <n> warning limit reached @ <rpm> RPM.` | fan warning |
| `0x2200` + fan | `0x15b` | `FAN <n> fault limit reached @ <rpm> RPM.` | fan fault |
| `0x3000` + sensor | `0x162` | `Sensor '<name>' temperature stabilized @ <c> C/ <f> F.` | temperature back to normal |
| `0x3100` + sensor | `0x15f` | `Sensor '<name>' advisory temperature reached @ <c> C/ <f> F.` | advisory |
| `0x3200` + sensor | `0x15e` | `Sensor '<name>' critical temperature reached @ <c> C/ <f> F.` | critical |
| `0x3300` + sensor | `0x15d` | `Sensor '<name>' fault temperature reached @ <c> C/ <f> F.` (a second generator prints `TEMP <n> fault temperature reached @ <c> C/ <f> F.`) | fault; the automatic power-down countdown starts |
| `0x3401` | `0x1a2` | `VRM not present: type <t>, number <n>` | regulator missing |
| `0x3402` | `0x1a3` | `VRM not OK: ...`, `VRM over voltage: ...`, `VRM under voltage: type <t>, number <n>` | regulator fault |
| `0x3500` | `0x1a4` | `48V power fail warning` | main input warning |
| `0x4000` | `0xb1` | `Environmental redundancy lost.` | redundancy loss |
| `0x4101` | `0x143` to `0x148` by remaining time | `brick auto power down in <s> seconds` | every countdown step of an automatic power-down |
| `0x4102` | `0x163` | `brick appears to have been powered down`, `power down aborted, environmental monitor reset`, or `Skipping brick powerdown, environmental power control is in MONITOR-ONLY mode.` | end of the countdown |
| `0x4102` | `0x164` | `brick is powering down now!` | end of the countdown, power actually being removed |
| `0x5000` | `0x160` | `SPURIOUS <supply> <limit> reading @ <v>V.` | a sensor read the monitor rejected |
| `0x5000` / `0x6000` | `0x165` / `0x166` | `Environmental fault test`, `Environmental fault/powerdown test` / `Environmental warning test`, `Environmental advisory temp test`, `Environmental critical temp test` | the operator's environmental test commands |
| `0x7200`, `0x7300`, `0x7400` families | `0x1b7` and up, `0x1bd` and up, `0xbc0` | power-bay supply inserted, removed and failed texts | power-bay bricks only; never on an Origin 300 (inferred: the chassis has no power bay) |
| `0x8000` | `0` | `Power Down: press power button again to immediately power down.` or `Power Down: issue 'pwr d' again to immediately power down.` | the first power-off request (front-panel button or `power down`/`pwr d` command) while the system is powered up; sent once, and the second request powers off |

The Linux driver names the classes `0x1000` power, `0x2000` fan, `0x3000` temperature,
`0x4000` environment, `0x5000` test fault, `0x6000` test warning, `0x8000` power-down
notification, and the code `0x4101` "power-down pending"; on `0x8000` and `0x4101` it
signals init to shut down (`snsc_event.c` lines 201 to 217). The VRM and 48 V codes do
not fit the driver's severity table; the driver prints them at its default level. The
diagnostic numbers are what the driver prints as `$(0x...)` before the text. Text field:
carriage return, line feed, the brick identity (`001c01`), a space, `ATTN: `, the message
from the table, carriage return, line feed, terminator; for example `\r\n001c01 ATTN:
3.3V low warning limit reached @ 2.970V.\r\n`. That is why the Linux parser skips two
leading bytes and strips trailing CR and LF (`snsc_event.c` lines 95 to 103). Every
environmental event says `ATTN:`; there is no `ERROR:` or `INFO:` variant in this path.
The same message without the prefix is written to the L1's log. Severity filter: each
event carries an internal level (0 informational, 1 warning, 2 fault); when the operator
has selected "show environmental fault events only" (the `env` command's option; the
image's wordings are `show environmental fault events only.` and `show all environmental
events.`), only level-2 events are generated at all. Wire size: the event serializer is
the response serializer's twin, accepting a field when its offset plus its length is
below 128 with the header byte counted and the tag byte not ([limits](#limits)); a field
that would not fit is not written and serialisation stops there, but the count byte is incremented for the field
that was dropped (established from the code; it looks like a firmware defect; whether a
real event ever reaches that length is unknown). This answers the [two-step
power-down](#node-power-off-and-power-on) unknown in part: the first `power down` while
the system is up sends the `0x8000` event on the registered subchannel; what the L1 does
in the deferred interval beyond that remains as recorded there.

**The `0x1007` rule and its persistence.** Request `0x1007` with one integer argument
stores position times 32 plus the argument as the event subchannel field and persists
the subchannel (five bits; the position bits are not stored) in the settings store; reply code 0, no fields; fewer than one argument answers minus
100. At L1 start the field is "none" (minus 1); a stored value is restored only if it is 4
or more, and under a start-time condition the review did not identify, so subchannels 0
to 3 do not survive an L1 restart. While the field is "none" no
event is sent. `0x100a` disables events and clears the persisted value. The `syscom`
(`junkbus`) command prints `system event subchannel <n>` or `system events are disabled`
(`<n>` is the stored field, which includes the position bits). The SN1 port's
compiled-out listener would ask for subchannel 31 (`l1.c` lines 3016 to 3023).

**Console input.** In console mode each character the operator types leaves as one
request-type frame whose payload is the single character: header `0x00` or-ed with the
subchannel, then the byte; no address, code or count. With the link in raw (non-PPP) mode
the character is written bare. If the system is powered down the character is not sent
and the L1 prints `WARNING: power on 001c01 appears off!` on its console (the image also
carries the `WARNING: power appears off, console unavailable` form recorded under
[console multiplexing](#console-multiplexing); which prints when is unknown). If the link
write fails, the console task receives an error reply whose text is `no response from
001c01 bedrock <subchannel name>` followed by `, system not responding` (time-out) or the
link error text, and shows it; the manual prints the form `no response from 002c01
bedrock console UART:UART_TIMEOUT` (007-3938-001 printed page 21). That the console task
prints the reply is inferred from the manual's example; the printing path was not traced.

### Subchannel semantics

All established from the link reader, the link writer and the request handlers.

- **Header decoding.** The reader takes bits 7:5 of the header as the packet type and bits
  4:0 as the subchannel. Type `0x00` frames go to the request parser, type `0x40` frames
  to the console-output parser. Type `0x20` (a processor sending a response) and types
  `0x60` to `0xe0` are discarded silently: no log line, no counter.
- **Requests on any subchannel.** A request is parsed identically whatever its
  subchannel, including 0 to 4. The firmware does not reserve subchannels 0 to 4 for
  console traffic on the receive side. It records the subchannel, together with the node
  position (0 on a single-node Origin 300), in bits 11:5 of the source address it attaches
  to the request: source = local module address with task 5 and bits 11:5 = position
  times 32 plus subchannel. For `001c01` and subchannel 16 that is `0x00041205`.
- **Replies mirror the request's subchannel.** The reply's destination is the request's
  source, and the link writer takes the reply's subchannel from bits 9:5 of that
  destination. There is no separate response subchannel and no case in which a reply
  leaves on another subchannel.
- **Events use one configured subchannel**, set by `0x1007` as described under
  [L1-initiated packets](#l1-initiated-packets).
- **Console output on any subchannel.** A type-`0x40` frame is console output: the bytes
  after the header are treated as text and delivered to the L1's console path with no
  interpretation. The subchannel it arrived on becomes the new console-input subchannel:
  the firmware stores it, persists it, recomputes the link's console address, and prints
  `INFO: console subchannel changed: <name>` on the L1 console when `l1dbg console on` is
  set. No range check is made, so output on subchannel 16 redirects typed characters to
  subchannel 16. This is the mechanism behind the `console mode 001c01 CPU0` wording
  after PROM output on CPU 0's channel (007-4365-002 printed page 41 shows the `console`
  form; the Origin 300 verbose trace in `docs/library/boot-logs.md` shows `CPU0`). Console
  output is dropped entirely while a stored configuration flag, whose purpose was not
  identified, is set.
- **Requests addressed to task 5 or task 21.** These are queued to the link writer,
  which treats a queued request as console input: the L1 sends back a request-type frame
  on the subchannel taken from the destination's bits 9:5 carrying one byte, the low byte
  of the request's first argument (a request with no argument echoes whatever byte the
  stale field holds); with the node's power off the handler instead answers a minus-1
  reply whose text is `WARNING: power on <id> appears off!`. Task 21 is the same queue for the multi-frame transport below. A
  request from the processors to `0x0ffff005` therefore produces a two-byte request
  frame, not a reply.
- **Multi-frame transport.** Codes `0x3001`, `0x3002` and `0x3003` are consumed at the
  link layer as a multi-frame transport (first fragment with a 16-bit length,
  continuation, last). Requests received that way get their source task set to 21, and
  their replies go back in 113-byte fragments with the same codes. No public consumer
  uses it; the stock PROM carries the path only toward an L2 ([consumer expectations](#consumer-expectations)).
- **Counters.** Request frames are counted (frames and bytes) per direction; the counters
  are internal.

### Receive tolerance

All established from the link frame reader. The reader is generic; the link opens it in
PPP mode with a 256-byte payload cap (header byte included, check sequence excluded).

| Input | Behaviour |
|---|---|
| bytes before any flag | the first such byte is reported once as `PPP:read stream not in frame`; then bytes are discarded silently until a flag |
| flag, flag (doubled or idle flags) | tolerated: an empty frame just restarts; nothing logged, nothing lost |
| frame of 1 or 2 bytes between flags | discarded; reported once as `PPP:short read/frame` |
| frame of 3 bytes or more with a bad check sequence | discarded; reported as `PPP:bad FCS (checksum)`; the closing flag is taken as the opening flag of the next frame, so the following frame is not lost (the last two bytes are taken as the check sequence, so a three-byte frame whose check passes is delivered as a one-byte payload; the previous revision's short-frame rows, and both readings behind them, were corrected by the protocol catalogue review) |
| escape (`0x7d`) then flag | discarded; reported as `PPP:escape sequence in read stream`; resynchronised at that flag |
| escape then escape | not an error: the escape state is simply re-armed and the next byte is exclusive-or'd with `0x20` |
| escape outside a frame | ignored |
| payload over 256 bytes | two extra bytes are held (they may be the check sequence); a third is `PPP:read buffer overflow`; the frame is discarded and bytes are discarded until the next flag |
| carriage return | ordinary data in PPP mode; in raw mode it ends the read (`PPP:control sequence in read stream`) |

"Reported" means a line in the L1's log: the link reader writes `SCC-<n> read error (PPP)
<text>` and the framer's error hook writes `SCC <n> PPP error (len=<l>): <text>`, each
once per distinct consecutive error; with `l1dbg pppdump on` every error also dumps the
raw bytes as `PPP data`, `PPP FCS` and `PPP RAW` lines. `<n>` is the serial-controller
index of the link, not established (settle by reading the link port's descriptor index
in the firmware, or by a `log` capture after a deliberate framing error). No error ever
produces a frame on the link, and none changes the protocol state beyond the
resynchronisation described. Receive-side check: the standard reflected CRC-16 with
initial `0xffff`, run over the header byte, the unescaped payload and the two check
bytes, must equal `0xf0b8`; the table in the image is the one the public consumers carry
(`l1.c` lines 743 to 776). Emit side: one opening flag, header, payload, check sequence
complemented and sent low byte first, one closing flag; `0x7e` and `0x7d` are always
escaped, `0x11` and `0x13` are escaped only when the port's flow-control-escape option is
on, `0x0d` only when its carriage-return option is on (both options' settings for the
link were not traced: unknown; settle by reading the link port's option flags at
initialisation, or by a capture containing those bytes); outgoing payloads are limited as the
[limits](#limits) give: a field is accepted when its offset plus length is below 128, so
a frame may carry 128 payload bytes including the header when its last field is binary. The firmware's tolerance is broader than the consumers': doubled flags,
escape-escape and a bad check sequence all resynchronise without losing the following
frame, and the receive payload cap is 256 bytes, not 127. The emitter rule under
[framing](#framing), one flag at each end and no idle flags, remains the right one because
the consumers, not the L1, are intolerant. Two further unknowns from this reading: the
console-input subchannel's factory default before any output has retargeted it (settle by
reading the settings-store defaults or by a capture of the first typed character after a
cold start), and what sets the configuration flag that discards console output (settle by
finding its writer in the firmware).

### Request logging switch

Established. `l1dbg promreq on` (alias `req`; 007-3938-006 printed page 102, 007-3938-001
printed pages 59 to 60) sets a RAM flag that the general task's dispatcher tests on every
request it receives. While set, each request produces one line in the L1's log:

```
L1REQ: <source address, 8 hex digits> - <request code, 4 hex digits>
```

The source address is the one the link parser attached ([subchannel
semantics](#subchannel-semantics)), so a request from the processors on subchannel 16 of
a `001c01` chassis logs as `L1REQ: 00041205 - 0003`. Arguments, replies and timing are
not logged. Only requests that reach the general task (task 6) are logged;
command-interpreter requests, console traffic and events are not. The `l1dbg` status
line reads `L1 PROM request debugging is on` or `off` (007-3938-006 printed page 103).
The flag is a plain variable, not a stored setting, so it does not survive an L1 restart
(inferred from its placement outside the settings store; the manual says only that "some
values are stored in NVRAM"). The log is the same one `log` displays and `log insert`
writes to (007-3938-006 printed pages 77 to 79): the firmware's log-append routine is the
one those commands call, and entries are stored with a timestamp. A switched-on log line
therefore reaches the operator through `log`, not through the console and not through
the link. Because the log is a fixed-size ring, a long PROM boot with the switch on
overwrites its own oldest lines; how many lines fit was not measured (unknown; settle by
measuring the ring size in the settings record). A real-machine capture with the switch
on would give the PROM's request codes and subchannels but not its arguments or
destinations.

### Console multiplexing

Processor console output travels as event packets on subchannel 4 with raw bytes
after the header; console input from the operator arrives as request-type packets on
subchannel 4 (or 0 to 3 for a selected processor). In console mode the L1 passes
bytes between its console connector and those packets; in L1 mode it takes them as
commands. The state machine, from the Origin 300 User's Guide: the L1 boots in L1
mode, showing its prompt (printed page 36); Ctrl-D at the prompt prints "entering
console mode 001c01 console, <CTRL-T> to escape to L1" and enters console mode; Ctrl-T
in console mode prints "escaping to L1 system controller" and the prompt, accepts
one command, and when that command completes the L1 re-enters console mode on its
own, printing the re-entering message; only the `l1` command holds L1 mode, until
the next Ctrl-D (printed page 41). After `power up` at the prompt a real Origin 300's
L1 prints `returning to console mode  001c01 CPU0, <CTRL_T> to escape to L1` (two
spaces before the id in every capture) and the
PROM's first line, `Starting PROM Boot process`, follows (the Origin 300 verbose
trace in `docs/library/boot-logs.md`). A model that made Ctrl-T sticky would forward the
wrong bytes to the guest.
The display's first line is the L1's own ("001c01"), the second a status such as
"powered up" until software writes it. The L1's own warnings and errors are "<brick id>
<ERROR or ATTN>: <text>" (the ATTN table on 007-3938-006 printed pages 47 to 48, ERROR
examples on printed pages 19 and 35); it also prints INFO-prefixed lines of its own, for
the auto-power countdown and, as a debug option, console tracking (the firmware texts
under [Lifecycle](#lifecycle)); every INFO example in that guide is an L2
message. The operator console runs at 38,400 baud, eight bits,
no parity, one stop bit, with hardware flow control (printed page 35).

The lifecycle and console research of 2026-09-07 adds the following, with the marks
explained under [Lifecycle](#lifecycle); each statement is established unless marked.
Where the draft and the cleared text above differ, both statements are recorded and
neither is chosen.

**The port.** The L1's console connector is the only console for the L1 and the PROM;
the IOC3 serial ports are operating-system ports ("the PROM and L1 consoles only come
on the L1 port", an SGI engineer, 2002, `3D19C210.9020308@brussels.sgi.com`; the
Origin 300 User's Guide connects the console to "the server's L1 console port",
007-4365-002 printed page 35). The console speed is not adjustable (an SGI engineer in
2002: "As far as I know you can't change the baud rate on the L1",
`slrna7jllc.2b2e.ivanr@omen.melbourne.sgi.com`). Console mode from the L1 is
unsupported when an L2 is present (007-3938-001 printed page 16).

**Two modes.** L1 mode: the prompt is visible and all input goes to the command
processor. Console mode: output from the system is visible and all input goes to the
system (007-4365-002 printed page 41; 007-3938-001 printed page 16).

**Transitions and their exact texts.**

| Operator action | State before | Text the L1 prints | State after |
|---|---|---|---|
| Ctrl-D (`0x04`) at the prompt | L1 mode (locked or fresh) | `entering console mode 001c01 console, <CTRL-T> to escape to L1` as printed (007-4365-002 printed page 41; 007-3938-001 printed page 19); real firmware from 1.18.5 to 1.48.1 prints `<CTRL_T>` with an underscore and two spaces before the id (three captures; the image has `entering console mode <id>, <CTRL_T> to escape to L1` with a placeholder where the id goes) | console mode |
| Ctrl-T (`0x14`) in console mode | console mode | `escaping to L1 system controller` then the prompt | escaped: L1 mode for one command |
| any command completes | escaped | `re-entering console mode 001c01 console, <CTRL-T> to escape to L1` as printed (all manual revisions); real firmware prints `returning to console mode  001c01 CPU0, <CTRL_T> to escape to L1` (IRIXnet 2737 line 1457; `graphica` lines 1792 and 2030; the image has only `returning`, no `re-entering`) | console mode |
| `l1` from the escaped state | escaped | `L1 command processor engaged, <CTRL-D> for console mode.` in the console-mode sections (007-4365-002 printed page 41; 007-3938-001 printed page 19); `L1 command processor engaged, <CTRL-D> to exit.` in the command reference (007-3938-001 printed page 59) and in the image; a third form, `L1 command processor engaged, <CTRL-T> to exit.`, is printed in the L2 chapter (007-3938-001 printed page 15); no capture shows any of the three | locked L1 mode until Ctrl-D |
| `power up` completes | locked | a prompt line, then `entering console mode  <id> CPU0, <CTRL_T> to escape to L1` | console mode |
| `power up` completes | escaped (inferred for the one capture, as the mode bullets under [node power-off and power-on](#node-power-off-and-power-on) record) | `returning to console mode  <id> CPU0, <CTRL_T> to escape to L1` | console mode |
| Ctrl-D in console mode | console mode | nothing from the L1; the byte goes to the system (inferred from the Origin 300 guide's console-forwarding description, 007-4365-002 printed page 41; no L1 capture of this exact byte) | console mode |
| any input in console mode with the node off | console mode | `WARNING: power appears off, console unavailable` (the image; when it is printed, at entry or per byte, is unknown) | unknown |
| console mode toward a hung node or a wrong subchannel | console mode | `no response from 002c01 bedrock console UART:UART_TIMEOUT` (007-3938-001 printed page 21); the 2006 print says `junk bus console` (007-3938-006 printed page 35); the image builds `no response from <id>` and `, system not responding`; the 2001 L2 example prints `no response from 003c01 bedrock CPU2 system not responding` twice on entry (007-3938-001 printed page 14) | console mode; "either the brick is hung or the subchannel is incorrect" |

Both `entering` and `returning` texts are in the image with a single space after
"mode" and a placeholder for the id, so the two spaces in every capture belong to the
inserted identity string, which the image builds from an id format and a designation
string that itself begins with a space (` CPU0` to ` CPU3`, ` console`) (established
texts; the assembly inferred). On the IRIXnet forum page the two characters before the
id are a non-breaking space followed by a space, the forum's way of keeping a doubled
space; the `graphica` page holds two plain spaces inside a preformatted block. The word
is `entering` when the L1 was holding L1 mode and `returning` when it had been escaped
from console mode with Ctrl-T (inferred from the two states in the captures). The
manuals print neither transition after `power up`; they print the `entering` text only
for an operator's Ctrl-D (007-4365-002 printed page 41). The designation after the
brick id names the selected subchannel: `console` for subchannel 4, `CPU0` to `CPU3`
for a processor's subchannel (the manuals print `CPU C` for `select 2`, 007-3938-001
printed page 21; real firmware prints `CPU0`, and the image's designation strings
` CPU0` to ` CPU3` and ` console` establish the digit form beside the captures).

**Where the draft and the cleared text differed.** Two differences were recorded at the
merge, and the lifecycle and console
review (citation: `docs/reviews/l1-lifecycle-console-contract-review.md`) settled both in the
draft's favour, so its finding 20 corrected the cleared sentences above. First, the
cleared paragraph quoted the power-up message with one space before the id, a
transcription slip from the one-space fragment in `docs/library/boot-logs.md`; every
capture has two (IRIXnet 2737 line 1457; `graphica` lines 1061, 1792, 1924 and 2030),
and the image builds the text as the previous paragraph records. Second, the cleared
paragraph said anything the L1 says on its own is "<brick id> <ERROR or ATTN>:
<text>"; the image holds INFO-prefixed texts the L1 itself prints: `INFO: <id> will
power up system in NN seconds...`, `INFO: <id> powering up the system.`, four `INFO:
<id> auto power up aborted` forms, `INFO: console subchannel changed: <x>`, `INFO:
system console changed: <x>` and `INFO: command not supported on this brick type`
(established that the texts exist; the console-tracking pair is printed only with the
`l1dbg console` debug option). The narrower cleared claim, that every INFO example in
the controller guide is an L2 message, is about the manual and still holds.

**Selection.** `select` alone prints `console input: 001c07 console` and `console
output: not filtered.`; `select <rack> <slot>` moves input to another brick; `select
subchannel console|0|1|2|3|4|a|b|c|d` picks the subchannel; `select filter on|off`
makes the L1 show output only from the selected brick during a multi-brick boot; the
2001 Origin 300 appendix adds `select local`, `select ctc` and the bare `select
console|a|b|c|d|<subchannel>` forms (007-3938-001 printed pages 20 to 21 and 76 to 77;
007-4498-002 printed pages 202 to 203). Errors: `ERROR: no brick at that rack and
slot`, `ERROR: brick at that rack and slot is not acceptable for console I/O`, `ERROR:
bad subchanel selection.` (sic, the image) (established texts). The processors can
move the selection too: request `0x1002` with one integer argument selects "this node's
console subchannel", and `0x1003` with no argument volunteers the requesting node as
the console-hosting node (`ksys/l1.h` lines 250 to 253; `l1_command.c` lines 572 to
641) (established as the consumer's meaning; neither has a kernel caller at `v2.5.40`;
whether the stock PROM sends them is unknown). The designation the L1 prints after the
brick id changes from `CPU0` to `console` later in a boot: on the Fuel, an escape and
`env` after the PROM had finished device discovery ended with `returning to console
mode  001a01 console, <CTRL_T> to escape to L1` (`graphica` line 2030) (established for
the Fuel). The manuals' rule: "If the system has been powered on and either one of the
bricks has a request to be the system console, then the C-brick attempts to communicate
with that brick" (007-3938-001 printed page 20); by default "the C-brick attempts to
communicate with its local CPUs" (printed page 20) (established). So the PROM, through
`0x1002` and `0x1003`, moves the selection to the console subchannel 4 during its boot
(inferred; whether the Origin 300's PROM does it, and when, is unknown; settle by the
emulated machine's request log). The image can print `INFO: console subchannel
changed: <x>` and `INFO: system console changed: <x>`, but the console-tracking
messages are a debug option (`l1dbg console on|off`, 007-3938-006 printed page 101),
so a quiet L1 prints neither (inferred).

**Subchannels and packet types.** Subchannels 0 to 3 are the per-processor consoles
(CPU A to D), 4 is the console subchannel; console output travels as event-type packets
(`0x40`) on subchannel 4 and console input arrives as request-type packets (`0x00`) on
subchannel 4 or a processor's subchannel: the Linux SN1 receiver accepts only
request-type headers on subchannels at or below 4 and only response or event headers
above (`l1.c` lines 1177 to 1183); its console write sends `0x40 | 4` (`l1.c` lines
2007 and 2129); the MIPS port does the same and supports no other subchannel
(`ip35-console.c` lines 9 and 62; `ip35-l1.c` lines 245 to 262); OpenBSD names
subchannel 4 the console and discards any packet that is not its awaited response,
noting it "may receive event packets from the console" (`l1.c` line 55 and lines
510 to 534). The per-processor subchannels 0 to 3 carry the same byte stream for one
processor; the SN1 kernel reserves them and drops their input (`l1.c` line 1505)
(established as consumer behaviour; what the PROM does with them, and whether its
per-CPU POD prompts use them or the console subchannel with the `A 000 001c01:`
prefix, is unknown).

**Output while the operator is in L1 mode.** The manuals define output visibility only
for console mode, and for the L2 they say output "will not be visible" outside it
(007-3938-001 printed page 15). Whether the L1 buffers, discards or still shows
processor output that arrives while the operator is at the prompt is **unknown**: in
the one capture that escapes during activity, the POD monitor was idle, so nothing
discriminates (`graphica` lines 1788 to 1792). Settle by escaping to the prompt while
the PROM prints its memory test and observing what appears on return.

**Input while no processor listens.** With the node off the image can say `WARNING:
power appears off, console unavailable`; with the node on but not accepting bytes the
L1 reports the UART write timeout named in the transitions table; the L1 does not queue operator
input for a later processor in any capture (established texts; behaviour inferred).
The error name `UART_TIMEOUT` and the `uart` command's `Write Timeouts` column say the
L1 detects the condition at its Bedrock-side UART, that is, the processor side must
accept bytes for console input to be delivered (inferred).

**Under an L2** the L1's own console mode is replaced by the L2's; the L2 prints
`entering system console mode (001c01 CPU0), <CTRL_T> to escape to L2` (Nekochan
16724335 line 144) (established; out of the single-chassis scope).

### Management console

The operator command set as the 2001 to 2002 manuals give it, from the same research.
Each row: the command, its arguments, the output shape on an Origin 300 with the
printed or captured example named, what it changes, and whether the retained image
knows the name. "Present" means the exact command name or its alias group appears as a
NUL-delimited string in the image; that proves the interpreter knows the word, not that
it acts on an IP45 (the image also carries `INFO: command not supported on this brick
type` for a catch-all group). Unknown commands answer `ERROR: command not found.` (the
Fuel at 1.48.1 to `?`, `graphica` line 1883; the image) and bad arguments `002c01
ERROR: invalid arguments for 'ver' command, try "help ver"` (007-3938-001 printed page
11; the brick id prefix is present only when the command was targeted at another brick,
printed page 18) (established). The display, log, refusal and mode effects of the
power, reset and restart commands are under [Lifecycle](#lifecycle).

| Command | Arguments | Origin 300 output shape and example | Effect | In image |
|---|---|---|---|---|
| `power` (`pwr`) | none; `check`; `vrm`; `up` (`u`); `down` (`d`); `up hold` | `power`: a `Supply State Voltage Margin Value` table listing `12V`, `12V IO`, `12V DIG`, `5V`, `3.3V`, `5V aux`, `3.3V aux`, `2.5V`, `Speedo2 CPU`, `1.5V` (007-4498-002 printed page 200); `power check`: `power appears on` (printed page 200), `power appears off` or `power appears mixed - some supplies on, some supplies off` (the image); `power up` and `power down`: nothing but the prompt (007-3938-001 printed page 73; 007-4365-002 printed pages 36 to 37) | applies or removes node power; see [node power-off and power-on](#node-power-off-and-power-on) | yes (`power\|pwr`; help texts `power up`, `power up, hold reset`, `power down`) |
| `* power up`, `* power down`, `ctc power up`, `ctc power down` | the target prefix `*` (local brick and everything attached) or `ctc` (the cabled peer, 2001 vocabulary) | nothing but the prompt (007-4365-002 printed pages 36 to 37 and 54; 007-4365-001 printed page 43); each refusing brick prints its own `<id> ERROR: …` line (Usenet 2005) | powers the local and attached chassis over the RS-422 link | yes (`*`; `nia\|ni\|ctc`) |
| `reset` (`rst`) | none | nothing but the prompt (007-3938-001 printed page 74; 007-4498-002 printed page 201) | resets the node; log `reset (COMMAND)` | yes |
| `softreset` (`softrst`) | none | nothing but the prompt; display `Sft rst...` then `Sft rst done` or `Sft rst fail` (007-3938-001 printed page 78; 007-4498-002 printed page 203) | asserts soft reset | yes |
| `nmi` | none (the image also lists `nmi <node>` forms) | nothing but the prompt; display `NMI...` then `NMI done` or `NMI Fail` (007-3938-001 printed pages 64 to 65; 007-4498-002 printed page 194) | asserts NMI; IRIX writes `/var/adm/crash` | yes |
| `version` (`ver`) | none | one line: `L1 1.8.1 (Image A), Built 09/21/2001 12:24:28 [P1 support]` (007-4498-002 printed page 205); on real Origin 300s `L1 1.48.1 (Image B), Built 01/22/2007 11:34:20  [Fuel/PE/O300 1MB image]` (Nekochan 16724335 line 1112); `* version` prefixes each brick's id on its own line (007-4498-002 printed page 178) | none | yes (`version\|ver`; the image's line format and its label `Fuel/PE/O300 1MB image`) |
| `brick` | none; `rack <n>`; `slot <n>` (1 to 39; `bay`, `upos`); `rackslot <r> <s>`; `partition none\|<n>`; `type <str>` | `brick`: `rack: 001 slot: 20 partition:none,type: C (IP45), serial MHE132, source: EEPROM` (007-4498-002 printed page 179); the image assembles `rack: 001, slot: 20, partition: none, type: C (IP45) [1MB flash], serial:MHE132, source: EEPROM` (inferred assembly of its fragments, matching the 2006 print of a later brick, 007-3938-006 printed page 59); `brick slot 02`: `brick slot set to 02.` (007-4365-002 printed page 38) or `brick slot set to 22 (takes effect on next L1 reboot/power cycle)` (007-4498-002 printed page 179; the image); an out-of-range slot: `Invalid slot specified.  Valid slot numbers are 1-39.` (the image) | stores rack, slot, type or partition in non-volatile memory; rack, slot and type apply at the next L1 boot | yes (`brick`; `set brick type, <str> is one of r,p,x,i,ip35,ip37,ip45,g`) |
| `serial` | none; `all`; `dimm`; `verify`; `clear`; `security on`; a four-word SGI key | `serial`: `BSN: MMH445    SSN: M2100887    Time: 03/22/2012 14:16:37 CST` (Nekochan 16724335 line 2380); `serial all`: a `Data Location Value` table with `Local System Serial Number NVRAM`, `Reference System Serial Number Attached C`, `Local Brick Serial Number EEPROM` rows (lines 810 to 816) | none unless setting | yes |
| `select` (`sel`) | see the selection paragraph under [console multiplexing](#console-multiplexing) | `console input: 001c07 console` and `console output: not filtered.` (007-3938-001 printed page 77; 007-4498-002 printed page 202) | moves console input and output selection | yes (`select\|sel`, `subchannel\|sub\|s`, `console\|con`) |
| `display` (`dsp`) | none; `1\|2 <text>`; `power\|attention\|failure on\|off` | 2001: `firmware revision: 2.0`, `line 1: "001c20 /2Powered Up"`, `line 2: "Powered Up"` (007-4498-002 printed page 181); 1.48.1: `revision: 2.0`, `line 1: "001c02     /"`, `line 2: "L1 running  "`, `leds:  PWR=off   SERVICE=off     FAULT=off` (Nekochan 16724335 lines 2394 to 2398; the image has these four formats) | writes a display line or an LED | yes |
| `log` | none; `clear\|reset`; `insert <text>` | one entry per line, `MM/DD/YY HH:MM:SS text` on firmware from 1.28.3 up (Nekochan 16729141 and 16724335); the 2001 print has no timestamps (007-3938-001 printed page 63); empty: `log is empty.`; `log reset`: `log reset.`; `log insert`: `log entry made.` | none unless clearing or inserting | yes |
| `env` | none; `check`; `on\|off`; `temp on\|off`; `altitude high\|low`; `reset`; `test …` | `Environmental monitoring is enabled and running.` then three tables: supplies (`12V IO`, `12V DIG`, `5V`, `3.3V`, `5V aux`, `3.3V aux`, `2.5V`, `Speedo2 CPU`, `1.5V` with 10 percent warning and 20 percent fault limits), fans `FAN 0 LEFT` to `FAN 4 PS'` with warning 2160 rpm, temperatures `NODE 0` and `NODE 1` (007-4498-002 printed page 184 with the node running; IRIXnet 2737 lines 1365 to 1419 with the node off, every state `Wait Pwr`) | none unless setting | yes |
| `date` | none; `<value>`; `tz`; `tz <value>` | `03/09/2021 09:48:54 CST` (IRIXnet 2737 line 1361); `10/17/2001 22:47:07 CDT` (007-4498-002 printed page 180); set format `mmddHHMMYYYY.SS` (printed page 180) or `yyyymmddHHMMSS` (007-3938-001 printed page 44), unknown which the Origin 300 firmware accepts; time zone as an offset of at most ±12 hours (both manuals) or, in the image, "set UNIX timezone string (ex: date tz CST6CDT)" | sets the clock or zone | yes |
| `config` (`cfg`) | none; `verbose` (`v`); `reset`; `ring` | one chassis: `:0  - 001c01` (IRIXnet 2737 line 1355; the 2001 print `0: - 001c20`, 007-4498-002 printed page 179); cabled pair, `cfg v`: `:0  - 001c02 (local)`, `:1  - not connected    (attached I/O)`, `:5  - 001c01 (attached C)`, `:6  - not connected    (attached I/O)` (Nekochan 17314 lines 916 to 928); `config reset`: `resetting L1 configuration` (the image). The index after the colon names a controller-side attachment position, but the manual does not use one fixed numbering across every topology. Its own two-chassis worked example gives five values: 0 the local brick, 1 IIA (II0), 2 IIB (II1), 5 NIA (Nl0), 10 NIB (N11); NI is the manual's own abbreviation for "network interface" (established: 007-3938-006 printed page 26 for the index list, printed page 59 for the abbreviation). In that same example the `iia` targeting command sets the entry at index 1 to the attached I-brick's own rack, type and slot (`004i01`), while index 5 already holds an attached C-brick's own rack, type and slot (`001c08`) before any command touches it, so index 5 names the attached C-brick itself, not a further brick attached beyond it; an earlier reading of 5 and 10 as that C-brick's own IIA and IIB, renamed, is withdrawn. A second worked example two pages on, for a larger three-brick chassis, numbers its attached bricks 1, 2 and 3 instead (an I-brick, a C-brick, then an X-brick), with no 5 or 10 in use (007-3938-006 printed page 60, Example 3-12): the five-value list is established only for the topology that produced it, not as a fixed scheme generally. The capture's index 6 needs no manual page to place it: the same Nekochan transcript already labels it `(attached I/O)` in its own console text. The page-26 example independently shows an untouched `:6 - 001p01` entry sitting outside its stated five-value list, corroborating an attached-I/O-type position at that index; whether it is a directly attached I/O brick or one reached through a C-brick is not stated. | `reset` starts a new controller discovery | yes |
| `uart` | none; `bedrock reset` (2001) or `junkbus reset` (2002 to 2006; `r`) | a `Baud Read Read Read Read Read Write Write Write` table with rows `CTI 107142 Discon Ready`, `CTC 107142 Discon Ready`, `SMP 37500 Discon Ready`, `BED 57692 Connect Suspend … Connect Ready` (007-4498-002 printed page 204); firmware 1.48.1 labels the Bedrock row `JNK 0` and shows `SMP 37500 Connect Ready` for the console port (`graphica` lines 1902 to 1908); the image has a numbered `JNK` label and `junkbus uart reset` | `reset` resets the L1's Bedrock-side UART | yes |
| `debug` | none; one numeric token | `debug switches set to 0x0000` (007-4498-002 printed page 181; `graphica` line 1058 shows `0x010d` after setting) | reads or sets the 16-bit debug switches the PROM reads through request `0x0001`; the 2006 table names bit values (`0x8` global POD, `0x10` local POD, `0x4` verbose, `0x100` no-disable, 007-3938-006 printed page 64) | yes |
| `l1` | none | `L1 command processor engaged, <CTRL-D> for console mode.`, `… <CTRL-D> to exit.` or, in the 2001 L2 chapter, `… <CTRL-T> to exit.` (the transitions table under [console multiplexing](#console-multiplexing)) | holds L1 mode | yes |
| `reboot_l1` | none; `a\|b`; `current\|old\|new\|other`; `addr <exp>`; `raw <exp>` | nothing captured on the console; the L1 restarts | restarts the controller; see [controller restart](#controller-restart) | yes |
| `help` (`hlp`), `help <cmd>` | | `Commands are:` then the alias table, then `enter 'hlp <cmd>' for more help on a single command.` (the image; the 2002 table is printed on 007-3938-003 printed page 40, and the 2007 Fuel/Origin 350 table at `graphica` lines 1885 to 1900) | none | yes |
| `autopower` (`apwr`) | none; `on`; `off`; `abort` | `auto power on is disabled.` (007-4498-002 printed page 178) | arms the boot-time countdown | yes |
| `flash` | `status`; `status verbose`; `default a\|b\|current\|new\|old\|reset` | `Flash image B currently booted` then an `Image Status Revision Built` table with rows `A default 1.18.5 12/18/2002 08:58:31` and `B default …` or `valid` (IRIXnet 2737 lines 1435 to 1451; 007-4498-002 printed page 187) | chooses the default image | yes |
| `network` | none; `usb`; `422`; `autodetect on\|off` | `network interface communication is 422` and `network autodetection is enabled` (007-4498-002 printed page 194); on change `nvram parameter changed, reset required to affect change.` (the manuals; the form is also in the image); the image's change text beside its network strings reads `nvram parameter changed, reboot required to affect change.` | selects the peer-link mode; 422 is required for a cabled pair (007-4365-002 printed page 38) | yes |
| `cpu` | none; `e`; `d`; `<n> e\|d` | `CPU Present Enabled` table (007-3938-001 printed page 43; on the Origin 350 session, which prints no L1 version, the rows read `0A 1 1` to `0D 1 1`, `graphica` lines 992 to 998); the image also holds `cpu commands are not supported on this brick type`, for a brick type the string does not name | enables or disables processors for the next boot | yes |
| `port` (`prt`) | none | `Port Stat Remote Pwr Local Pwr Link LED SW LED` with rows `A 0x02 none okay off off` and `B …` (IRIXnet 2737 lines 1423 to 1431) | none | yes |
| `leds`, `link`, `eeprom`, `fan`, `history`, `istat`, `usb`, `junkbus` (`syscom`, `jb`, `bedrock`), `l1dbg`, `nvram reset`, `partdb`, `pimm`, `margin`, `scan`, `security`, `test`, `verbose`, `ioport`, `sysstate`, `button` | per the manuals | diagnostic and setting commands outside a boot session; `leds` was fixed for CPUs C and D on Origin 300 systems in L1 1.14.x (007-4576-001 printed pages 14 to 17); `junkbus` prints `junkbus protocol is PPP` (007-3938-001 printed page 58) and the image's status form is `L1 <-> system protocol is PPP` followed by the system-event subchannel state | | yes for every name listed |
| `ctc <cmd>`, `cti <cmd>`, `ctci <cmd>` (2001) | a command | the target's id on one line, then its output (007-4498-002 printed page 180) | forwards over RS-422 | as aliases only: the image knows `nia\|ni\|ctc` and `iia\|ii\|cti` (the 2002 renaming, 007-3938-003 printed page 40); `ctci` is absent |
| `fru`, `pci`, `prom`, `node` | per the manuals | one alias group in the image whose help text calls it the unsupported-command catch-all, beside `INFO: command not supported on this brick type` (established as strings; that an IP45 answers so is inferred); `pci` is also in the 2001 table's Origin 300 column, and the image holds a separate `ERROR: unknown pci command.` | none | present as one alias group |
| `router`, `pbay` | per the manuals | present as names with their own texts (`router type is unknown`, `PBay state unknown`); what they answer on an Origin 300 is unknown | | present as names |
| 2002's `fuel`; the 2007 Origin 350 list's `check`, `make`, `try`, `pic`, `pwm`, `promver`, `shell`; the 2006 Altix lists (`blade`, `b2b`, `quit`, `deadlock`, `shcfg`, `tio`, `rmmr`, `wmmr`, `tdr`, `pktgen`, `tune`, `lbi`, `psic`, `war`, `shdbg`, `mbrick`, `ssn`, `ssi`, `isync`, `slit`, `psmi`, `hour`, `daytona`, `abacus`, `athena`, `nicfg`, `nlcfg`) | | | | absent as names, except `check` and `b2b` (present as tokens; `check` is also the argument word of `power check` and `env check`, so its presence proves nothing about the 2007 command) and `promice` (absent in that case; the mixed-case token `promIce` is present beside `promice commands are not supported on this brick type`, so whether the 2007 command is meant is unknown) |

The selected L1 1.44.0 IP45 `debug` command has a no-value read form and a
one-value write form. Its stored brick type `4` aliases to the command's `c`
class, so both forms reach the setting handler on IP45. A different brick
class can instead return an unsupported-command information field. Both
accepted IP45 forms describe the current word with a `0x` prefix, lowercase
hexadecimal digits padded to at least four places and a line feed; a plain
local command has no brick-name line. A qualified remote route may add a
brick-name line. The selected-image command reading (citation: `docs/research/l1-debug-console-parser-firmware-reading.md`)
and independent firmware review (citation: `docs/reviews/l1-debug-parser-firmware-evidence-review-20260915.md`)
establish the static branches. The exact fitted-controller response and
operator-terminal rendering remain unmeasured.

The selected task-3 response reading (citation: `docs/research/l1-debug-task3-firmware-reading.md`)
and independent instruction review (citation: `docs/reviews/l1-debug-task3-firmware-evidence-review-20260915.md`)
settle the **unmodified image's static response path**, before two optional
installed filters that could rewrite a reply. Task 3 puts the command's signed
result in the four-byte response code, then counts its non-null output strings in one
byte and emits each as a tag-`0x01` text field with a terminating zero. The
reply keeps the request subchannel. On an IP45 local `debug` read or an
accepted `debug 269` write, the predicted code is zero, count one, and the
only text is the switch-word line; 269 yields `0x010d` after the write.

For `debug nope` and `debug 12junk`, whole-token numeric parsing fails before
the setter. In the selected command registration order, later unrelated
records clear the partial-match marker, so the predicted response is code
`-1200` (`0xfffffb50`), count one, and a generic command-not-found text
field. This is an **inference from the checked registration and instruction
order**. An unlocated later table or an installed response filter could alter
the fitted reply. The model may use this unmodified-path prediction only with
that limitation stated and a raw task-3 qtest that checks code, count, text
bytes and unchanged debug state. A selected-version raw exchange would settle
the fitted result; an emulator reply alone would not.

The `*` prefix enumerates connected module records. With none, `* debug 269`
can reply code zero with no text and no local setting change. With exactly one
target resolving to this L1, successful `* debug 269` predicts code zero and
two text fields: a module-name line ending in `:\n`, then the debug line.
Malformed `* debug nope` on that one target predicts code `-1` and one
combined module-name/error text field. These are **conditional selected-path
inferences**, not a claim that every fitted Origin 300 has one wildcard
target. The current QEMU wildcard shortcut and any proposed `debug` extension
need a declared target-list limitation and a discriminating task-3 test before
their output is treated as modeled behavior.

The `ctc` prefix addresses NI port A over the peer link. A closed port
predicts code `-1` and one inactive-port text field without changing local
switches. An open port forwards the text request; its result depends on the
peer's reply, and an unanswered open peer may produce code zero with an error
text. The fitted peer state, module names, optional response filters and L2
host-visible rendering remain unknown. QEMU keeps this peer route explicitly
unsupported until a revision-matched peer contract is reviewed.

The current QEMU model chooses a bounded qualified-`debug` fallback instead
of enumerating connected modules or forwarding to NI port A. For a task-3
`* debug 0` request it returns code `-1`, count one, and one tag-`0x01` text
item `INFO: wildcard debug is unsupported without connected modules.\n\0`.
For `ctc debug 0` it returns the same code/count/tag with text
`INFO: peer debug is unsupported.\n\0`. Both routes leave the local debug
word unchanged and log the unsupported route. These exact texts and codes are
**model choices**, not selected-controller replies. In particular, the
wildcard fallback differs from the selected static zero-connected-target
prediction of code zero/count zero, and the peer text differs from the
selected closed-port inactive warning. The limitation is the missing
connected-module inventory and peer-link contract. A task-3 raw exchange
with a selected L1 1.44.0 and known connected-target and port state would
discriminate the fallback; QEMU's own output would not.

The write form parses one whole numeric token with automatic base: decimal,
leading-zero octal or `0x` hexadecimal, with an optional sign. It stores only
the low 16 bits, so an accepted `debug -1` reads back `0xffff` and
`debug 65536` reads back zero. Static control flow also accepts a bare
`debug 0x` as zero and appears to accept a wholly numeric 32-bit overflow as
the saturated value `0xffffffff`, because the common command parser ignores
the conversion error. A nonnumeric token or numeric suffix skips the setting
handler. These unusual cases need a discriminating selected-version command
exchange before their transport status or console response is asserted as
observed hardware behavior. The QEMU model currently has no operator `debug`
form; its read-only general-task switch field is a separate implemented path.

The Origin 300 column of the 2001 command table lists every command the C-brick column
lists except `pbay` and `security verify`, so it includes `select`, `softreset`, `nmi`,
`debug`, `cpu`, `ctc`, `cti`, `network`, `leds`, `partdb` and `pimm`; it adds `pci` and
excludes `router` (007-3938-001 printed pages 33 to 35) (established).

The selected 1.44.0 `debug` setter updates the checksum of the primary core
settings record and refreshes its backup. On L1 firmware startup a valid
primary is restored, or a valid backup if the primary fails, or defaults if
both fail. The no-value formatter and negative/overflow parser branches above
are selected-image static facts; the task-3 response reading gives
prefilter branch predictions, while fitted response filters and terminal
rendering remain unmeasured. The fitted NVRAM chip, its
retention without mains power and behavior of L1 1.18.5 remain unknown. The
checked command reading (citation: `docs/research/l1-env-reset-debug-firmware-reading.md`)
and independent evidence
review (citation: `docs/reviews/l1-env-reset-debug-firmware-evidence-review-20260915.md`)
locate the handler, shared accessor and restore branches.

**Display.** Two lines of twelve characters (007-3938-001 printed page 25,
family-level; Origin 300 geometry is unmeasured). Line 1 is the brick
identification with a spinning heartbeat character at its right end: captures show
`001c02     /` and `001c20 /2Powered Up` (the second a garbled print), and the image
carries the four-character spinner set `|/-\` (established for the id; the spinner
inferred). Line 2 is the status the L1 last wrote, or what software wrote through the
display requests. Who writes what, by stage:

| Stage | Line 1 | Line 2 | Writer | Mark and source |
|---|---|---|---|---|
| L1 boot | brick id (or the unset form) | `L1 booting`, then `L1 running` | the L1 | established: the image; 007-4365-002 printed page 35 |
| firmware image of the wrong family | `  UNKNOWN   ` | `BRICK TYPE!!` | the L1 | established: the image; Usenet 2005 |
| L1 idle with the node off after a mains cycle | `001c02     /` | `L1 running  ` | the L1 | established: Nekochan 16724335 lines 2394 to 2397 |
| `power up` or the button | id | `Powering Up`, then `Powered Up`; `Power Up Error` is a fourteen-character text in the image whose twelve-character visible form is unknown | the L1 | established: the image; 007-4498-002 printed page 181; Usenet 2005 |
| `power down` or the button | id | `Pwring Down`, then `Powered Down`; `Power Down Error` is a sixteen-character text in the image whose twelve-character visible form is unknown | the L1 | established: the image; 007-4498-002 printed page 89 |
| an environmental shutdown | id | unknown: the countdown and shutdown texts are console messages (the paragraph after this table), and none of them fits twelve characters | the L1 | unknown |
| `softreset` | id | `Sft rst...` (`Soft reset..` in the image), then `Sft rst done` or `Sft rst fail` | the L1 | established |
| `nmi` or the NMI button | id | `NMI...`, then `NMI done` or `NMI Fail` (`NMI fail` in the image) | the L1 | established |
| `reset` or the reset button | id | `Reset...`, then `Reset done` | the L1 | inferred from the image's display texts |
| a disabled button pressed | id | `Pwr Btn Disabled`, `Reset Btn Disabled` or `NMI Btn Disabled`: texts in the image at sixteen, eighteen and sixteen characters; that line 2 shows them, and in what twelve-character form, is unknown | the L1 | texts established in the image; the display form unknown |
| the PROM running | id | unknown: no capture reads the display while the PROM runs, and no public source shows the PROM writing it | ? | unknown; settle by the emulated machine's request log (`0x1004`, `0x1005`) |
| IRIX running | id | unknown | ? | unknown; settle by an `l1cmd display` capture under IRIX |
| OpenBSD | id | `OpenBSD/sgi ` through request `0x1005` at each node attach | the guest | established (the display row of the request table) |
| `display 2 <text>` | id | the text | the operator | established: 007-3938-001 printed page 46 |
| an environmental warning | id | the manuals say the display carries "warning of required service or failure" and "identification of failed components"; the exact 12-character texts are not printed | the L1 | unknown text; established that it happens (007-3938-001 printed page 25) |

Whatever software wrote stays until the L1's next state change overwrites line 2; the
L1 does not restore a software line after `Powered Up` (inferred: the writes are
unordered overwrites of the same line; no source says otherwise). The ATTN and ERROR
messages (`<id> ATTN: FAN 0 warning limit reached @ 0 RPM.`, `<id> ERROR: SerNum:…`)
go to the console, and to the L2 display when one exists (007-3938-001 printed pages
11, 25 to 27; `graphica` lines 400 to 406) (established); they are not the display's
lines. On an environmental fault the L1 prints `Auto power down in xx seconds` every 5
seconds until it shuts down, then `Brick or server appears to have been powered down`
(007-3938-001 printed page 27, a row of the same table as the ATTN messages); the
image's forms are `brick auto power down in <n> seconds` and `brick appears to have
been powered down` (established texts; where they print, console or log, inferred from
the manual). What the display shows during that shutdown is unknown.

### Behavior alone

No L2 is required for one or two chassis; console mode from the L1 exists for exactly
that case and is unsupported when an L2 is present (007-3938-001 printed page 16;
007-4365-002 printed pages 34 to 37; 007-4498-002 printed page 4). `power up` at the
prompt then Ctrl-D reaches the power-on diagnostics and the System Maintenance Menu;
`power down` after `init 0` powers off; the broadcast form `* power up` is for two
chassis or a chassis with a PCI expansion module, where the L1s talk over their
RS-422 ports and the L1's network mode must be 422 (007-4365-002 printed pages 36 to
38; which cable carries the pair is not on those pages). The L1 knows only itself, its attached I/O brick if any, and a cabled
peer.

### Two-chassis pair

Draft of 2026-09-25 for the two-chassis Origin 300, independently reviewed as a
draft and again as implemented; it changes no cleared row above. Read from the
published manuals and the retained captures named inline, and from this
datasheet's own cleared rows. The researcher read no L1 firmware image, PROM
disassembly or vendor source, and may implement or review the L1 pair. Marks as
in the lifecycle section: established, inferred, unknown.

**What joins the two L1s.** The C-brick L1 has two RS-422 ports; one of them
joins two C-bricks when there is no R-brick, over the NUMAlink 3 cable, and
"routes console and diagnostic data and propagates resets, NMI, and power
between the two C bricks" (established: 108-0240-002 printed page 2-47). That
the Origin 300 is that case is **inferred**: the book describes Origin 3000
C-bricks, and the Origin 300 pair has the same shape, two servers with one
NUMAlink 3 cable between their LINK connectors and no router (established:
007-4365-002 printed pages 14 and 38).
Nothing public describes the bytes the two L1s exchange, their timing, or how
either detects the other (unknown; settle by a capture on the cable's RS-422
pair, or 108-0241-001 System Controllers, not held).

**Operator setup.** Before cabling, the operator designates one server master
and the other slave, powers the slave with a console on it, and sets its
system ID with `brick slot 02`, answered `brick slot set to 02.`; the master
keeps 01, so the prompts read `001c01-L1>` and `001c02-L1>`. After cabling,
the console goes on the master and both servers are powered on. The
manual's note reads: "For server-to-server communication to work correctly,
the network mode on the server must be set to 422 (when connecting a PC
[system console] to a serial port)", set with the `network` command
(established: 007-4365-002 printed page 38). The slot is
stored at once and applies at the next L1 boot, and the network mode likewise
needs an L1 reset (established: the state table under
[what the L1 keeps](#what-the-l1-keeps)). The master and slave designation is
the operator's; the manual's only mechanism for it is the slot number.

**What each L1 then reports.** Each L1 keeps its own rack and slot and answers
the rack-and-bay request with them, so the PROM on each chassis names its own
module: a cabled pair appears as `Module 001c01` and `Module 001c02` in PROM and
IRIX inventories (established: the openbsd-sgi list capture of 2014, lines 17
to 62; Nekochan thread 17314). The slave's L1 lists the master as its attached
C-brick: `cfg v` on `001c02` prints `:0 - 001c02 (local)`, `:5 - 001c01
(attached C)` and the two attached-I/O positions `not connected` (established:
Nekochan 17314; index 5 is NIA on 007-3938-006 printed page 26, the
[`config` row](#management-console)). The master's listing is not captured;
that it shows `001c02` at index 5 is inferred from the symmetry of the link.

**Requests the peer answers.** Under the cleared [addressing](#addressing)
rules, a request whose destination is a concrete rack and bay that is not the
local one goes to the catch-all receiver, which forwards it over the link when
the cabled peer's entry matches; a type-3 (attached C-brick) request is
rewritten to type 0 and tried on the C-to-C port, and with a peer cabled "type
3 requests reach the peer's L1, which then answers as itself" (established as
this datasheet's cleared routing; the peer's side of the exchange is inferred
from it). Without a peer both forms get the router error reply, minus 1 with
detail 8. What the peer returns for a request it cannot serve, and how long the
requester waits, are unknown.

**Power, reset and NMI.** `* power up` and `* power down` act on the local
chassis and the peer; the User's Guide gives `* power down` for "a system that
contains two servers" (established: 007-4365-002 printed pages 36 to 37). A
refused power-up prints one `<id> ERROR: …` line per brick (established:
Usenet 2005, the [node power-off and power-on](#node-power-off-and-power-on)
paragraph). The `ctc` prefix addresses the peer over the link (established:
the [management console](#management-console) table). `reset` on the master
after cabling "perform[s] a reset of the system" when the slave was not
discovered (007-4365-002 printed page 38); whether a plain `reset` also resets
the peer, or only `* reset` does, is unknown. That NMI crosses the link is
established only as the architecture book's sentence above; which command sends
it (`* nmi`, the button) is unknown.

**Console.** The operator's console is on the master (established). The cable
carries "console and diagnostic data" (established), so the slave node's
console output can reach the master's console; whether the slave's L1 forwards
its node's console stream to the master unprompted, only on `select`, or not at
all during a normal boot is unknown. No verbose console from a direct-cabled pair
is held; the routed pair's (Nekochan 16724335) goes through an L2.

**What the stock PROM needs from the peer L1.** Each chassis runs its own PROM
from its own flash and talks to its own L1 through its own Bedrock's UART, as on
one chassis (established for one chassis in [the stock IP35 PROM](#the-stock-ip35-prom);
inferred for each chassis of a pair, since each module reports its own PROM
revision and module id). The PROM's concrete-address forms of `0x0003`,
`0x0004`, `0x0009`, `0x000a` and `0x000b` are "used for peer bricks" (the
request table); whether a pair's PROM sends them through its local L1, so the
link forwards them, or reaches the peer's L1 through the peer Bedrock's UART
over NUMAlink, is unknown. The PROM's discovery itself runs over NUMAlink
through the Bedrock vector engine, not through the L1 (the Bedrock local-block
datasheet). Settle by logging every L1 request, per node, with its destination
field, during a two-node stock PROM boot on the emulator; a peer request that
goes unanswered then shows as a two-second wait and the wrapper's fallback,
not a hang (the reply-waiting paragraph).

**Model contract.** Proposed with the draft and implemented as proposed in
QEMU's `hw/misc/sgi-l1.c` (the peer link) and `hw/mips/sgi/sn-machine.c` (the
builder joins the two L1s when it wires the cable), in `sgi-origami` at
`412d4c4460`, and the implementation is independently reviewed. Each item names
what the evidence leaves to the model. One implementation detail the proposal
left open: both chassis are one QEMU machine, so a broadcast `* power` or `*
reset` is carried out once by the requesting L1's own machine action, which
already covers both nodes, and the peer only records the power state and the log
entry. That is a model choice; what a real pair exchanges to propagate the
command is unknown. Of the `* power` forms only `up` and `down` reach the peer
(`* power check` does not), and a power-down reaches it only once the requesting
chassis has gone down, so a first `* power down` that is waiting for its
confirmation leaves both chassis up. A `ctc` relayed to the peer that names
`ctc` again is refused with `INFO: nested ctc is unsupported.` and status minus
1, another model choice. Tests: `sn1-l1-test`'s `sn1/l1/peer-pair`,
`sn1/l1/peer-power` and `sn1/l1/peer-nested-ctc`, and `sn-builder-test`'s
`origin300-two-chassis-l1s-are-peers`.

- An `sgi-l1` gains a link to its peer controller, standing for the RS-422 pair
  in the cable. The link carries no wire format; the exchange is an in-process
  call, a stated abstraction since the protocol is unknown.
- The link works only while both controllers' network mode is 422. The mode is
  a property, default 422: the modeled pair is the manual's configuration, and
  the factory default is unknown.
- A request whose destination is type 0 with the peer's own rack and bay, or
  type 3 with any rack and bay, is answered by the peer's dispatcher as if it
  were local there, on the requester's subchannel. Without a peer, both keep
  today's router error reply.
- `* power up` and `* power down` apply to the local node and then to the peer,
  as the User's Guide documents; `* reset` doing the same is a **model choice**,
  since no manual page gives it for the pair. `ctc <command>` runs the command
  on the peer and prints the peer's id line first. `* nmi`, plain `reset` propagation and `cfg` stay unmodeled
  until evidence settles them.
- The slave's slot is the `bay` property the builder sets from the module's
  number; the controller answers with it exactly as today.
- Console forwarding from the slave stays unmodeled; the slave's L1 keeps its
  own console line, which a launcher may leave unconnected.

### Reset, NMI and power

The L1 drives reset, soft reset and non-maskable interrupt to the node electronics on
its `reset`, `softreset` and `nmi` commands and on the front-panel buttons
(007-3938-006 printed pages 80, 91 and 94; 007-4498-002 printed pages 194 and 203;
007-4365-002 printed pages 6 to 7). No
public guest asks it to reset: OpenBSD reboots through the Bedrock's network-interface
reset register and uses the L1 only to power off. The emulated L1 is therefore on the
power-off path and the operator path, not the reboot path.

## Consumer expectations

The consumers decide which answers must be right and in what order. Two were read for
this revision, independently of the firmware and of each other: the stock IP35 PROM,
from its payload, and the IRIX 6.5.30 kernel, from its compiled objects. Both readings
are restated here in this datasheet's words; the firmware's reply to each request is in
the [Requests](#requests) tables and is not repeated, and the "Sent by" cells of those
tables carry what follows in brief. Marks as in the rest of the sheet: established (read
directly in the decoded code or seen in a real-machine transcript), inferred (follows
from the code plus a public source but was not observed on hardware), unknown (with what
would settle it). Quoted messages are the guests' exact texts; where a kernel or PROM
message begins with the name of the routine that prints it, that prefix is omitted here
and the rest of the text quoted.

### The stock IP35 PROM

Read from the payload named under "Sources used", IP35 PROM version 6.210 built 26 August
2004, the revision that produced the Origin 300 verbose transcript in
`docs/library/boot-logs.md` (IRIXnet thread 2737, L1 firmware 1.18.5, debug switches
`0x10d`), so that transcript is a direct witness for this image. Facts that are
derived from `refs/irix-source`: none. The 2026-09-12 identity amendment was read
from the restricted stock-PROM disassembly. Its reader is source-exposed for the
L1, Bridge and XBridge identity paths, IO-8, Bedrock, the processor and the Origin
300 machine, and may neither implement nor review those components. No QEMU source
was inspected for the amendment. Terms: "local general task" is destination `0x0ffff006`;
"attached-I/O general task" is `0x4ffff006`; "concrete" means the rack and bay fields
carry real numbers instead of the local sentinels. The request `0x000e` and its
surrounding sequence are in the cleared [PROM-side
subsection](#what-the-stock-prom-sends-and-expects); this subsection adds the rest of
the client and records where the two PROM readings differ. The protocol catalogue review (citation: `docs/reviews/l1-protocol-catalogue-review.md`)
re-verified the request inventory, its destinations, the letter table, the stage values,
the transmit retry and wait constants and the quoted texts; the boot order and the
per-row uses and error actions stand as the PROM reader's, marked so.

**Transport** (established unless marked). The PROM reaches the L1 UART through the
node's uncached Hub special window at offset `0x3000_0080`, one 16550 register per 8
bytes, 64-bit loads and stores with the low byte significant; it reads line status at
register offset `0x28` and writes data at offset `0x00`, the layout the three public
consumers use. Remote nodes are reached through the same registers over a routed access
path that a one-chassis boot does not exercise. Receive: a byte is taken only when
line-status bit 0 (data ready) is set; if bit 0 is clear and either bit 2 or bit 3
(parity or framing error) is set, the byte is a line error and the current frame is
abandoned; if bits 0, 2 and 3 are all clear, "no data" is reported and the caller keeps
polling; bits 5 and 6 are not consulted for receive. Transmit: before every byte the
PROM polls line-status bit 5 (transmit holding register empty) every 10 microseconds for
up to 50 milliseconds; on timeout it records an internal progress code and retries the
same byte, and after 30 consecutive timeouts on one byte it abandons the frame and
reports a short send; an emulated UART that always reports bit 5 set never enters this
path. Modem status (clear to send) is not consulted, unlike the Linux SN1 and MIPS ports.
UART programming: the client's set-up routine waits up to 100 milliseconds for
line-status bit 6 (transmitter empty), then writes line control `0x80`, divisor high
byte `0x00`, divisor low byte from 12,000,000 divided by (baud times 16), line control
`0x03`, modem control `0x22`, interrupt enable `0x00`, FIFO control `0x01` then `0x07`,
with short spins between writes; the baud rate is 57,600 when the console device is the
L1 protocol UART (divisor 13, exactly the Linux SN1 sequence), else 9,600. Established (established by a later pass of the same PROM reading, its symbol mapping of the payload, made after the protocol catalogue review and not covered by it): the local L1 client programs the UART at
57,600 baud during its initialisation, before its first request, which an earlier pass
had left unknown; the model must accept the sequence, and the [register map](#register-map)'s
rule that no divisor is enforced stands for the other consumers. Time base: all client timeouts are measured against the Bedrock's real-time
counter scaled to microseconds, not instruction counts, so wall-clock behaviour in the
emulator follows the guest's real-time counter rate; this settles the cleared
subsection's unknown about the units of its 2,000,000 and 1,000,000 waits, which are
microseconds.

**Framing on the wire** (established). Flag, escaped payload, check sequence and flag
exactly as [framing](#framing) gives them; the check is computed over the unescaped
header and payload bytes and is itself subject to escaping. The encoder sends at most
127 payload bytes after the header; longer requests are truncated before the check
sequence. Header: bits 7:5 packet type, 4:0 subchannel; requests carry type `0x00`; the
request subchannel is 16 plus the issuing CPU's index (16 to 19 on a four-CPU node); the
PROM never uses subchannel 16 for every CPU the way OpenBSD does. Request payload: the
four-byte destination, the two-byte code, then, only when there is at least one
argument, the count byte and typed arguments; zero-argument requests are six payload
bytes, the count byte omitted as in the Linux SN1 port. Destination forms: the local
general task, the attached-I/O general task, and concrete forms with controller type 0.
The PROM also carries a bulk-transfer path addressed to controller type 1 (an L2), task
`0xa`, reached only through the partition-record path and never on a one-chassis boot.
No request in the boot path uses the command interpreter: the PROM sends no text
commands to the L1 (established by the absence of any such request construction in the
image and of any command text among its strings).

**Reply decoding and waiting** (established). After sending, the PROM polls the UART,
assembling frames, until a complete frame arrives on the request's subchannel or
2,000,000 microseconds elapse; a timeout returns its own transport code, which the
wrappers report to their callers as minus 100 ("communication failure"); there is no
automatic retry after a reply timeout. A short send is retried after a 500 microsecond
pause, up to 49 further attempts, before the wait starts. The receiver accepts a frame
whose header subchannel is 5 to 23 with type response (`0x20`) or event (`0x40`) and whose
subchannel is currently open (a request outstanding); frames for closed subchannels,
subchannels 24 to 31, or with type `0x00` on subchannels 5 to 31, are dropped; frames on
subchannels 0 to 4 are accepted only with type `0x00` and are console input. A frame
shorter than three bytes (two for console subchannels) is dropped. A frame whose running
check is not `0xf0b8` is dropped silently and the PROM keeps waiting for the rest of its
two seconds, so a corrupted reply costs the same two seconds as no reply. Repeated `0x7e`
between frames are ignored, and a closing flag also serves as the next opening flag; the
emitter rule under [framing](#framing) still applies because of the other consumers.
Reply payload: the first four bytes are a 32-bit big-endian signed reply code returned
as-is when nonzero, so negative codes are seen correctly; byte four is the field count;
the parser requires the count to equal the number of fields the wrapper expects and each
tag to match, and reports a mismatch as minus 1, passed on as minus 112 ("bad reply"),
so every accepted reply must carry exactly the listed fields, and extra or missing fields
fail the request even with code 0. Each unanswered request costs two seconds and the
wrapper's "on error" action; no request in the boot path halts the PROM on error. The
PROM's error decoder maps its internal codes to texts such as `System controller
response timeout` and `Timed out getting ELSC lock`, which appear in diagnostic-monitor
commands, not on the boot path.

**Protocol versus raw mode** (established for the texts). Early in the boot the PROM
prints either `Booting prom with L1 in PROTOCOL MODE.` or a fourteen-line box beginning
`********* WARNING! ********** WARNING! *********`, `* You are running the L1 in raw
mode.`, telling the operator to enter `bedrock ppp` at the L1 prompt and reset, ending
`Press enter to continue booting in raw mode:`. Which is chosen depends on the console
driver the PROM selected, not on an L1 reply. Neither line appears in the Origin 300
transcript; unknown which condition suppresses them there (candidate: the text goes
through a debug-only print path; settle by a run with the emulated console and, if
needed, by checking whether the transcript's debug switch value `0x10d` enables it).

**Request table.** Each row: destination task and form; arguments; the reply the PROM
accepts; what it does with the result; what happens on an error or no reply; the
fidelity required. "Answered" means code 0 with exactly the listed fields. The
firmware's reply for each code is in the [general-task table](#task-6-general-requests).

| Code | Destination | Arguments | Accepted reply | Use of the result | On error or no reply | Fidelity required | Mark |
|---|---|---|---|---|---|---|---|
| `0x0001` read debug switches | local general | none | one int | low byte and second byte stored in the PROM's flags word; drives `*** Warning: System controller debug switches are non-zero (0x%x)`, diag level, verbose info level and boot-stop selection (the transcript shows `0x10d`) | flags stay zero; the diagnostic monitor prints `Error reading debug switches:` only when asked interactively | a plain 16-bit switch word; the model defaults to 0. Bit 15 bypasses the whole empty-position recovery loop, but is not an authentic CPU-population input. `0x10d` reproduces the nonmatching four-CPU verbose transcript. The factory value for a real `IP45_2CPU` is unknown | established (shape and decode), unknown (factory setting) |
| `0x0003` rack, bay and type | local general | none | two ints: rack shifted left 6 or-ed with bay, then the type character | first use: brick classification (boot step 4); second use: module identity printed as `Bedrock Rev: 2, Module: 1 (001c01) from Sys Ctlr` and stored for `/hw/module` names, `hinv` and KLCONFIG; rack 0 with bay 0 means "no module id" | classification treated as "not a special brick"; module identity falls back to a stored value and the line prints `from PROM backup`; boot continues | rack and bay must be nonzero and stable across the boot (every later concrete-address request echoes them); the type character is canonicalised before use: `4`, `A`, `6`, `7`, `8`, `9`, `Q`, `0` all become module class `C`, so either `0x34` or `0x43` yields `001c01` | established |
| `0x0003` rack, bay and type | attached-I/O general (`0x4ffff006`) | none | two ints as above, if a peer answers | a successful reply prints `hubii_link_good: %c-brick attached to module 001c01.` with the raw type character and keeps its rack and bay as the attached module's identity. An L1 1.18.5 transcript prints `4`, but contains no frame proving which request supplied it | an error or timeout prints `hubii_link_good: Unknown I/O brick attached to module 001c01.` and boot continues (the payload also carries `hubii_link_good: No I/O brick attached to module %M.`) | the selected L1 1.44.0 image routes this type-4 address through peer slot 9 and has no local task-6 handler for it. Do not synthesize a local `0x34` success from the old transcript; preserve the conditional route and its error/no-immediate-reply outcomes. The old capture's L1 1.18.5 route and reply remain unknown | established (PROM request and success/error use; selected-image route), unknown (real reply source) |
| `0x0003` rack, bay and type | concrete rack and bay, type 0 | none | two ints | the PROM compares the reply's rack and bay with the address it used; a mismatch prints `L1 provides an inconsistent rack and bay location` | error reported to the caller; used for peer bricks and for the module-identity path when the crossbar identity says a bridge is directly attached | must echo the destination's rack and bay | established |
| `0x0004` brick serial number | local general (primary node identity when `OverrideNIC` is absent), or concrete own rack and bay (I/O board record) | none | one text | (a) the wrapper converts the text to a number; the boot accepts it only when the wrapper succeeded and the number is neither zero nor all ones; (b) during crosstalk discovery the I/O board's serial for its KLCONFIG record is asked from the concrete address of the chassis's own rack and bay; also used for peer bricks and by diagnostic-monitor commands | (a) enters the separate IO-8 fallback decision below; (b) the record keeps no serial and discovery continues; POD prints `Could not read serial number.` | the wire contract is code 0 and exactly one text field. No stock identity text was observed, so do not add a default serial to satisfy this path. With no configured serial, retain the general task's minus-104 response. The concrete form must return the configured text unchanged if one exists | established (reply shape and fallback (a)); inferred ((b) is reached on an Origin 300: the wrapper and its discovery-time caller exist) |
| `0x0005` firmware revision | local general | none | three ints: major, minor, patch | combined as major times 1,000,000 plus minor times 1,000 plus patch and compared with 1,012,005; a lower value prints `*** WARNING : Please upgrade L1version to at least 1.12.5` and continues; also printed by a diagnostic-monitor command | a zero or negative combined value skips the warning; boot continues | at least 1, 12, 5; the transcript's L1 was 1.18.5, the held image is 1.44.0 | established |
| `0x0006` read EEPROM | local general (and node-specific clients) | four ints: component, area, offset, length | one int (bytes remaining after this chunk) then one binary chunk | read in 64-byte chunks from offset 0 until the remaining count reaches 0; no zero-length size probe; the board area (2) is parsed for `Part:`, `Name:`, `Serial:`, `Revision:` and `Laser:` fields to build KLCONFIG identity records; the separate IO8 BIA ASCII Ethernet field supplies the production `ef` address; the SPD area (4) is read for memory modules | returns an error to the board-record code, which prints `WARNING: <where>: bad part number format in EEPROM` style lines and continues | absent components must answer minus 104 quickly; present ones must return the remaining-count field first and never a chunk longer than requested; exact record contents are the EEPROM datasheet's business | established (protocol); inferred (which components are asked on an Origin 300; the unknowns below) |
| `0x0008` system serial number | local general | none | one text | stored in the KLCONFIG record of the crossbar's I/O board during local I/O discovery | the record keeps an empty serial; boot continues | any short text; the transcript does not show it | established |
| `0x0009` partition identity | local general and concrete | none | one int | partition number used in `Checking partitioning information` and printed as `PARTITION=0` in the discovery results | treated as no partition information; boot continues as a single partition | answer 0 for the one-chassis system | established (use); inferred (0 is the transcript's value) |
| `0x000a` port speed | local general and concrete | none | two ints | first int 0 selects 400, nonzero selects 600 (the link speed figure used in crosstalk set-up); second int ignored | error reported; the caller uses its default | either value works; unknown which the real L1 returns (the firmware derives it from per-port non-volatile speed bits; settle by a request log on hardware) | established (decode); unknown (value) |
| `0x000b` partition record available (the name is the PROM's use, not a public name) | local general and concrete | none | one int | nonzero makes the PROM fetch a partition record through a bulk transfer addressed to an L2 (controller type 1, task `0xa`, request codes `0x3001` to `0x3003`, four-second reply timeout) and then set the partition with `0x1006`; zero means nothing to do | any error means nothing to do | answer 0, or minus 101; both end the path immediately (the firmware answers a two-bit non-volatile field) | established (decode and effect); inferred (semantic name) |
| `0x000c` attached module identities (name from the PROM's use) | local general when the crossbar identity says so, else attached-I/O general (`0x4ffff006`) on an Origin 300 | none | eight ints: four pairs of rack shifted left 6 or-ed with bay, then a value | issued once from the crossbar arbitration routine before the banner, to label attached I/O (established by a later pass of the same PROM reading, its symbol mapping of the payload, made after the protocol catalogue review and not covered by it; an earlier pass placed it in per-bridge crosstalk set-up); on failure a line ending `failed. rv= %d` | the routine continues with the message | the reply shape is what matters; unknown what the four value fields mean and what a real Origin 300 L1 answers (the firmware's row: the module seen on each of ports 1 to 4; settle by a hardware request log); an error reply is tolerated | established (shape, address and issuing site); unknown (values) |
| `0x000e` | local general | none | one int | the cleared [PROM-side subsection](#what-the-stock-prom-sends-and-expects) | as there | any reply ends it; only the two-second timeout matters on an IP45 | established |
| `0x1004`, `0x1005` display lines | local general | one text, at most 12 characters | code 0, no fields | the PROM's display helper writes the first 12 characters of a message to line index 1, request `0x1005` (the second display line); the texts are in the display table below | ignored | accept and record the text | established |
| `0x1006` set partition | local general and concrete | one int | code 0, no fields | only after a nonzero `0x000b` answer or from a diagnostic-monitor command | ignored | never sent on a one-chassis boot when `0x000b` answers 0 | established |
| `0x1009` | local general | two ints: bits 11:8 of a packed value, then its low byte | code 0, no fields | sent in pairs around stage changes and on error paths; values seen: (1,1) and (2,2) together early when a crossbar identity test passes; (2,2) before the `Prom Booting` display on IP53-class bricks only; (2,1) then (1,2) on a memory-configuration failure, (1,2) before `Mem Cfg Err` and `Prom Cpy Err`, (4,`0x22`) then (1,2) before `Scache Err` | ignored | accept two ints; unknown meaning (the firmware's row calls it front-panel control with a selector of 0 to 2; whether the early pair is sent on an Origin 300 is unknown, below) | established (shape); unknown (meaning) |
| `0x100a` | local general | none | code 0, no fields  | sent once on the master CPU immediately after the brick classification and before the debug-switch read | ignored | accept; the firmware's row: clear event subchannel | established (position) |
| `0x100c` | local general | two ints: a stage number, then the CPU's processor-number register value | code 0, no fields  | the first request of the boot (stage 20) on every CPU that runs the boot entry; stage 70 on entry to the diagnostic monitor | ignored | accept; the firmware's row: a per-processor setting of unknown meaning | established (position) |
| `0x2006` power cycle (name from the diagnostic-monitor text `power cycle CBrick`) | local general | none | code 0, no fields; the machine is expected to reset | sent on a secondary-cache test failure (`FAILED.  Disabling and resetting system...`), during the first three failed starts of each CPU position, and by the diagnostic-monitor command; after sending it the PROM spins and, if still running, prints `ERROR: power cycle requst failed[%d], old L1 version ?` | as above | cycles the whole module, not one CPU position. On the fourth failed start the PROM records that position as disabled in CPU boot flash and does not send this request | established |
| `0x3001` to `0x3003` bulk transfer | controller type 1 (L2), task `0xa` | hand-built | hand-built | partition-record fetch and firmware-flash paths only | not reached | out of scope for one chassis | established (existence) |

**Node-identity selection observed in the stock PROM.** `OverrideNIC`, when set,
is converted without sending `0x0004`. Otherwise the PROM sends code `0x0004`
to local task 6 through the L1 UART at node-zero address
`0x9000_0000_3000_0080`. The wrapper requires reply code 0, exactly one text
field and a successful conversion. The boot then rejects zero and all ones even
after wrapper success.

After any rejection, the PROM checks Bedrock I/O-interface scratch 0 at
`0x9200_0000_0140_0150`. Bit 1 is set earlier when the `DisableIO` environment
variable exists. If that bit is set, the PROM skips the IO-8 candidate. If it is
clear, a nonzero candidate previously saved by the
[widget-15 one-wire path](xbridge.md#stock-prom-and-baseio-prom-io-8-identity-access) is accepted
and the PROM prints `using BaseIO nic`; a zero candidate selects the time-derived
value and prints `no BaseIO, using random value`. The random line therefore means

```text
primary value rejected and (DisableIO bit 1 set or IO-8 candidate zero)
```

Run `prom-20260912T075429-15ae87` reached that line. It proves the combined
predicate, but its retained trace has no request frames or register values and
does not identify which term failed. The address-bound branch sequence is at
PROM PCs `0xc00000001fc6a544` through `0xc00000001fc6a59c`; this is a
firmware-observed software expectation, not evidence for an L1 serial value or
for the hardware behind the IO-8 candidate.

Requests the public headers list but the PROM never builds: `0x0002`, `0x0007`,
`0x1002`, `0x1003`, `0x1007`, `0x2000` to `0x2003`, and `0x1004` (line 1 is never
written by the display helper; the line-index arithmetic allows it but no caller passes
0). The model's minus 101 for codes the firmware does not serve is right for them; the
firmware serves `0x0002`, `0x0007`, `0x1002`, `0x1003`, `0x1007` and `0x2001` to `0x2003`
whether or not the PROM asks.

**Boot order for a one-chassis Origin 300.** "E" is established from the code and
confirmed by transcript text, "I" inferred from the code alone. The retained emulator run
(`prom-20260907T141650-98dc09`, Origami PR #844, QEMU `351e29f5f5`, launcher
`57764ba21a`) shows one request line, `sgi-l1: request 0x000e unavailable at
0x0ffff006`, before an unrelated bus error; the unknowns below say why that excerpt
cannot yet confirm this order.

Master CPU, from the reset trampoline to the memory copy (one function, straight line):

1. Client initialisation for the local node: the UART is programmed at 57,600 baud here,
   before the first request (no wire traffic). E
2. `0x100c` with stage 20 and the CPU number. Every CPU that reaches the boot entry sends
   this, each on its own subchannel (16 plus CPU index). I: the slave CPUs take a
   different exit shortly after, so on a two-CPU model expect two of these, one per CPU. E
   for the master.
3. Conditional: if a crosstalk identity register masked `0xf000` reads `0x4000`,
   `0x1009` (1,1) then `0x1009` (2,2). Unknown on the Origin 300 (the crossbar is an XBOW,
   whose identity low nibble is 0; likely skipped).
4. Brick classification: reads the crossbar identity; unless it is one of three special
   values, and only if the Bedrock's local-block scratch register 1 does not already hold
   a cached result, sends `0x0003` to the local general task and classifies the type
   character (`7`, `8`, `9`, `0`, `Q` are IP53-family classes; anything else, including
   `4`, is class 0). The result is cached in that scratch register, so later
   classification calls send nothing; on a warm restart with the register preserved the
   request is skipped. E
5. Conditional on class 3 only: `0x1009` (2,2) and display `Prom Booting`. Skipped on an
   Origin 300. E
6. `0x100a`. E
7. `0x0001` debug switches, stored into the flags word. E
8. Module identity: `0x0003` local, then `0x000e`. This reading finds the caller always
   passing the "also ask" flag; the cleared PROM-side subsection, confirmed by its
   reviewer, adds that a reply of rack 0 and bay 0 from `0x0003` suppresses the `0x000e`
   request; both stand (the flag is always passed, the suppression is a separate test).
   The result becomes the module id printed later. E
9. Console enable: from here console output is delivered as event frames (below). Then
   the protocol or raw banner decision. E
10. Per-position failed-start recovery: the stock PROM skips board product nibble
    `0x4`, but not IP45 nibble `0x8`. Its position-mask table has no IP45 entry.
    A zero startup scratch value enters recovery. Attempts one through three append
    a position-specific counter to CPU boot flash, print `[CPU-X PLL n]`, and send
    `0x2006`. The number is the persistent count from the preceding failed start,
    not a live PLL measurement. The fourth failure writes `DisableX` and continues
    without another power cycle. The state is in CPU boot flash, not the L1.
    Switch bit 15 bypasses the whole loop but prints a nonzero-switch warning.
    These are reviewed PROM predicates and model output (citation: `docs/reviews/origin300-population-environment-review.md`),
    not an established factory flash image for a physical `IP45_2CPU`. E for the
    PROM and model. U for matching hardware.
11. `0x0005` firmware revision; version check against 1.12.5. E
12. Prints `Starting PROM Boot process` (from the PROM, not the L1). E, transcript.
13. Local I/O interface initialisation: the link-good check sends `0x0003` to the
    attached-I/O general task (`0x4ffff006`) on an Origin 300 (crossbar identity low
    nibble 0, class 0) and prints `hubii_link_good: 4-brick attached to module 001c01.`;
    when the link is good it asks the attached brick's module identity, which on the same
    address selection is a second `0x0003` to `0x4ffff006`; then `0x000a` port speed to
    the local general task (reply first int 0 selects 400, then compared with the
    crosstalk link's own setting). E (the transcript shows the link-good line here); I
    (the second `0x0003` and the port-speed position).
14. Crosstalk initialisation and a second link-good pass: `0x0003` to `0x4ffff006` again
    (the transcript shows the line twice before the banner), `0x000c` from the crossbar arbitration routine (address per the table), and, to record
    the I/O board's serial, `0x0004` to the concrete address of the chassis's own rack and
    bay. E for the second line and for `0x000c`'s issuing site (the later pass of the PROM
    reading); I for the concrete `0x0004`.
15. Banner `IP35 PROM SGI Version 6.210  built 02:33:51 PM Aug 26, 2004`, `  built for
    bedrock rev. 1.1 or greater`, `running in IP45 mode`; three more classification calls
    (cached, no traffic); `Local master CPU A revision: f24`; `PROM length: ...`;
    `Configured bedrock clock: 200.0 MHz`; `Status of local IO: ...`; `Bedrock Rev: 2,
    Module: 1 (001c01) from Sys Ctlr` (or `from PROM backup` when step 8 failed); `On PROM
    entry: ERR_EPC=...`. E
16. `Configuring memory` (memory configuration; may read SPD records with `0x0006`, the
    unknowns below); on failure `0x1009` (2,1), `0x1009` (1,2) and display `Mem Cfg Err`. E
17. `*** Warning: System controller debug switches are non-zero (0x10d)`, `*** Diag level
    set to None (2)`, `*** Info level set to verbose`, `*** Boot stop requested at Global
    (2)` from the step-7 value. E
18. `before reading NIC`; `OverrideNIC` if configured, otherwise local task-6
    `0x0004`. A rejected wrapper result, zero or all ones enters the IO-8 fallback
    above. A usable primary or fallback is printed as `Hub NIC: 0x52c5878c` in the
    retained real-machine transcript; the transcript does not identify which source
    supplied it. `SR1 set to ...`, `SR0 set to ...`. E for the messages and selection
    predicates; unknown for the source of the transcript's value.
19. `Testing/Initializing memory ...` then `DONE`; on failure display `Mem Cfg Err` as in
    step 16. E
20. `Copying PROM code to memory ...` `Copy PROM (...) to RAM (...), len 0x1686a8`,
    `Done`, `DONE`; on failure `0x1009` (1,2) and display `Prom Cpy Err`. E
21. Client re-initialisation after the copy (the client state is rebuilt in RAM; still no
    wire traffic), console re-enable. E
22. Secondary cache test (skipped in the transcript: `Skipping secondary cache diags`); on
    failure `0x2006`, then `0x1009` (4,`0x22`), `0x1009` (1,2), display `Scache Err`. E
23. `CPU A switching stack into UALIAS and invalidating D-cache`, `... switching into
    node 0 cached RAM`, `... running cached`. E

After the memory copy, to the menu:

24. `Initializing kldir.`, `Initializing klconfig.`; `Discovering local IO ...`: crossbar
    and bridge widget discovery builds KLCONFIG records. This is where `0x0008` (system serial into the crossbar I/O record) and `0x0006`
    board-area reads (`Part:`, `Serial:`, `Laser:` for board identity)
    are issued, plus a module-identity lookup (cached; no traffic). I for the exact
    requests per widget; the transcript shows `Check_master: link 10 is master` twice and
    `DONE` here.
25. `Discovering NUMAlink connectivity ...`, `Local hub NUMAlink is down.`, `Found 1
    objects (1 hubs, 0 routers)`, discovery results with `MODULE=001c01 PARTITION=0`, `No
    other nodes present; becoming global master`, NASID assignment, UALIAS switches: no L1
    traffic found in these steps. E for the text; I for "no traffic".
26. `Intializing any CPUless nodes`: per-node initialisation runs only for remote nodes;
    on one chassis nothing is sent. I
27. `hubii_link_good: 4-brick attached to module 001c01.` a third time (the transcript
    shows it right before `Checking partitioning information`), so `0x0003` to
    `0x4ffff006` again. E
28. `Checking partitioning information ...`: `0x0009` to the local general task, `0x000b`
    to the local general task (preceded by the cached module-identity lookup), and
    `0x1006` only if `0x000b` answered nonzero. Then `No other nodes present; becoming
    partition master`, `Erecting partition fences`, router configuration, cache flushes,
    the slave loop, and either the System Maintenance Menu or, with the transcript's
    boot-stop switch, `*** Entering POD mode on node 0` and the `POD SysCt Cac>` prompt. E
    for the text; I for the request placement within this stage.
29. Diagnostic monitor entry sends `0x100c` with stage 70. E

Per-CPU and per-slot loops: per CPU, `0x100c` at the boot entry (step 2), each CPU on its
own subchannel; the boot entry has no direct caller in the image and is reached by a jump
from the reset path, so which CPUs run it is inferred from the absence of a master-only
test before that request; no other per-CPU request (I). Per crosstalk widget, the board-record `0x0006` reads of step 24 (I); `0x000c` is
issued once from the crossbar arbitration routine before the banner, not per bridge (E). Per memory module, `0x0006` SPD reads
if memory configuration uses them (unknown). Per remote node, the whole of the module and
serial requests through a routed client; not applicable to one chassis (E).

**Display texts** (request `0x1005`, 12 characters, the second line):

| Text | When | Mark |
|---|---|---|
| `Prom Booting` | IP53-class bricks only, before `0x100a`; not on an Origin 300 | established |
| `Mem Cfg Err` | memory configuration or initialisation failure | established |
| `Prom Cpy Err` | PROM-to-RAM copy failure | established |
| `Scache Err` | secondary cache test failure, after `0x2006` | established |
| a formatted error text | one further error path builds its text with a format; the exact text is not resolved | unknown |
| a text before jumping to a loaded program | written by the program-launch path; the exact text is not resolved | unknown |

On a healthy Origin 300 boot to the menu the PROM writes no display line, so the display
keeps the L1's own `001c01` and status text, as the Origin 300 User's Guide describes and
as the [display table](#management-console) left unknown; the [lifecycle
unknown](#lifecycle-unknowns) about what the display shows while the PROM runs is
settled for the healthy path by this reading.

**Console** (established). Output from the PROM is delivered as event frames with header
`0x40` or-ed with the CPU index (subchannel 0 to 3, the issuing CPU), raw bytes after the
header, no request fields; lines are flushed at newlines; before the console-enable point
of step 9 nothing is sent, so the L1 console stays empty until then, which is consistent
with the empty console in the retained emulator run given that the run had not reached
step 9's successor prints. Input: the PROM reads console input from request-type frames
(`0x00` or-ed with the subchannel) on its own CPU's subchannel (0 to 3), one byte or more
of raw payload per frame; a model that forwards operator keystrokes on subchannel 4 only
will not be read by the PROM, so forward console input on the master CPU's subchannel (0
for CPU A). Whether the real L1 uses the CPU subchannel or 4 for keyboard input to the
PROM is not established from the image alone (the code also accepts frames on subchannel
4 into a console ring, but the PROM's reader looks at the CPU subchannel first); the
firmware reading under [subchannel semantics](#subchannel-semantics) establishes that
console output arriving on a subchannel retargets the L1's console input to that
subchannel, which with the PROM's output on subchannel 0 puts the L1's keystrokes on
subchannel 0 (the combination of the two readings is inferred; settle with a hardware
capture of a keystroke). Events: nothing in the boot path waits for an event from the L1;
event frames on request subchannels are stored like responses and none is consumed by
the boot path.

**PROM-side unknowns of this reading, and what would settle them.**

| Unknown | What would settle it |
|---|---|
| Observed prefix versus the code order: the retained run's excerpt shows `0x000e` as the only logged request, while the code sends `0x100c`, `0x0003`, `0x100a` and `0x0001` before it on the same path, all of which the model would have logged as unavailable or answered; either the excerpt was trimmed to the first unhandled code of interest or the run took a path not covered here (the reading's recorded discrepancy) | rerun with every `sgi-l1` request logged in order |
| The meaning of the `0x1009`, `0x100a`, `0x100c` and `0x000c` values and the `0x000a` value | the firmware's rows give the shapes and, for `0x100a`, the meaning; the rest by a hardware capture with `l1dbg promreq on` (007-3938-006 printed page 102) or a further firmware reading |
| Whether the early `0x1009` (1,1), (2,2) pair is sent on an Origin 300 (it depends on a crosstalk identity register value) | the same capture |
| SPD reads during memory configuration: the board-record reader that walks memory modules exists and uses area 4, but its caller on the Origin 300 path was not traced to the memory-configuration stage | log `0x0006` arguments in a run that reaches `Configuring memory` |
| The console input subchannel (above) | a hardware capture of a keystroke |
| Which display texts appear beyond the four resolved ones | a run through each error path, or a further reading |
| The raw or protocol banner (above) | a run with the emulated console |

### Processor frequency record for the IP35 PROM

The 2026-09-12 processor-frequency reading was read from the restricted stock-PROM
disassembly. Its reader is source-exposed for the base-I/O PROM's frequency and timing
code, the node configuration area it walks, and the Origin 200 base-I/O PROM used for
comparison, and may neither implement nor review those components. No QEMU source was
inspected for this reading.

**The lookup has one source and no fallback** (established). The base-I/O PROM's
frequency lookup (`0x130d92b0`) pre-zeroes a result slot, then walks the node's
configuration area in memory through a callback (`0x130ac248`). It sends no L1 request,
reads no EEPROM or SPD part, reads no Bedrock or CP0 register, and calibrates against no
fixed timer.

**How the walk works** (established). The traversal helper (`0x130dc9e0`) takes the base
of the configuration area from a fixed slot in the local node's address window (node base
plus `0x2000 + 0xd0`) and checks a four-byte ASCII signature at its head before looking at
anything. It then offers each board record to the callback, which accepts only records
whose one-byte class field at record offset 5 equals `0x11`, enumerates that board's
type-1 (processor) component records (`0x130ded28`), and for each requires bit 0 of the
status byte at component offset 2 set and the 32-bit field at component offset `0x1c`
equal to the requested processor number. On a match it reads the 16-bit field at
component offset `0x3c` as megahertz and multiplies by 1,000,000
(`0x130ac2d8`–`0x130ac2f8`) into the caller's slot.

**What bit 0 means** (inferred). Most defensibly a record-format or validity flag rather
than an enable flag: another consumer of the same byte (`0x130a286c`) uses it to choose
between a 16-bit node id at component offset `0x22` and an 8-bit one at `0x18`, a layout
selector. Whether a processor the first-stage PROM marked disabled also clears it is
unknown from the base-I/O PROM alone; the run this reading is based on reported zero
processors enabled and a mismatch between the node board EEPROM and the PIMM device, so
both readings stay live until the record itself is read.

**What failure leaves behind** (established). If no record matches, the lookup prints
`*** Warning: Cannot find frequency of CPU %d` and returns the pre-zeroed slot: zero.

**How the value is used** (established). The seconds routine (`0x1303fbfc`) calls the
lookup exactly once, on its first invocation, and stores the result at
`0xc000000013a704e0`, the word this reading is built around. Its three neighbours are the
whole-seconds counter (`…4d4`), the leftover ticks (`…4d8`) and the previous CP0 Count
sample (`…4dc`), the last also serving as the "already initialised" flag. On every later
call the routine samples CP0 Count (`0x130e4c60`, a bare `mfc0` of CP0 register 9), adds
the delta to the leftover, and converts to whole seconds by repeatedly subtracting the
word at `…4e0`. Everything that asks the PROM the time goes through that routine, and the
netboot's only timer does: the per-block TFTP read arms a 30-second alarm (`0x130409a4`),
the device poll checks the deadline every sixteenth call (`0x13001b4c`), and on expiry the
read re-sends the last acknowledgement and gives up after five attempts. The TFTP
retransmit interval is this word, directly and solely — this is the mechanism behind
QEMU issue #554.

**Zero does not shorten the timeout, it wedges the PROM** (established). The conversion
is an unsigned `while (word < leftover) { leftover -= word; seconds++; }`
(`0x1303fca4`–`0x1303fcbc`). With the word zero the subtraction is a no-op and the test
stays true, so the routine never returns once a single Count tick has accumulated — the
netboot in QEMU issue #554 does not merely retry too fast, it hangs. A garbage non-zero
value behaves differently: it scales the timeout linearly, too small making the alarm
expire at once.

**What the emulated Origin 300 must provide** (established, and binding on the model).
For the PROM to find the frequency without a patch, the node's configuration area must
contain a board record of class `0x11` carrying at least one processor component whose
status byte has bit 0 set, whose processor-number field at offset `0x1c` matches the
running processor, and whose 16-bit field at offset `0x3c` holds a non-zero megahertz
value. That area is built by the first-stage PROM from the node board EEPROM and the
PIMM configuration values, so the emulator's task is upstream of the record itself: a
clean implementer must supply node board EEPROM and PIMM configuration inputs that agree
with each other, so the first-stage PROM does not report a mismatch and does write a
usable processor record — an absent, disabled or zeroed record reproduces QEMU issue
#554's hang rather than fixing it.

**What rate to write there** (inferred). Because the time base is CP0 Count, the value
that makes the timer correct is whatever rate the model advances Count at, not the
nominal part speed — R10000-family silicon increments Count at half the pipeline clock,
so a nominal 500 MHz part would want 250,000,000 and the raw 500,000,000 would make the
alarm twice as long. Two CP0 Count samples a known wall-clock interval apart would settle
it.

**The Origin 200 comparison** (established). The Origin 200 base-I/O PROM contains the
byte-identical lookup (`0x11c557b8`): same class `0x11`, same component type, same status
bit, same offsets `0x1c` and `0x3c`, same multiply by a million, same warning string. It
finds its frequency today, so on that machine the configuration area does carry a
matching processor record with the status bit set and a non-zero megahertz field. The
difference is entirely in what builds the records.

### IRIX 6.5.30

Read from the compiled IP35 kernel objects on the retained media named under "Sources
used", by disassembly and symbol and relocation tables, with no IRIX source; facts that
rest on the objects are marked established from them. Hardware addresses named here are
those the public headers name and the datasheet already carries; addresses inside the
objects are not given. The protocol catalogue review (citation: `docs/reviews/l1-protocol-catalogue-review.md`)
re-verified the request codes, the constants, the subchannel limit and the quoted texts
against the objects; the claims that rest on the kernel object alone (the version-check
arithmetic, the board-identification word, the widget rule, the console type, the
component formulas) stand as the kernel reader's, marked so.

**What changes against the reviewed rows, in brief.** On the Origin 300 the kernel sends
its attached-I/O identity, version, serial and EEPROM requests to the local L1 address,
not the attached-I/O-brick address, because it special-cases this board and the I/O
widgets it uses (14 and 15); the local answer to `0x0003` must therefore carry the type
letter `4` (`0x34`), not `C`, since the kernel derives the module name `001c01` from `4`
by its own table and names the I/O side "Ibrick" from the same letter, where `C` would
name it "Cbrick" and produce device paths no real Origin 300 has. Two requests unknown to
the public headers are on the boot path: `0x000e` (one integer, answer 0) and the
"system state" notice `0x100c` (two integers, nothing back). The reset request the kernel
uses is `0x2001`, not `0x2000`; the power-off request is `0x2005`. The kernel requires L1
firmware 1.3.20 or later when the node carries R14000 processors and reboots itself
otherwise. It decodes the whole 32-bit reply code and requires the field-count byte to
match; it never writes the L1 display on an Origin 300; it registers subchannel 23 for
events and expects event packets with the layout under [L1-initiated
packets](#l1-initiated-packets).

**UART programming and access** (established). The kernel reaches the UART through the
node's Hub special window at `0x9000000030000080` plus the node number shifted left 33,
one 16550 register per 8 bytes, 64-bit loads and stores with the low byte used: data at
`+0x00`, interrupt enable `+0x08`, interrupt identification and FIFO control `+0x10`,
line control `+0x18`, modem control `+0x20`, line status `+0x28`, the remote form under
[register map](#register-map). Each time the kernel initialises a controller context for
the local L1 it programs the UART afresh: it waits up to 100,000 microseconds for
line-status bit 6, then writes line control `0x80`, the divisor 13 (12,000,000 divided
by 16 times 57,600) into the divisor high and low registers, line control `0x03`, modem
control `0x22`, interrupt enable `0x01` (received-data interrupt on), FIFO control `0x01`
then `0x07`, with 10-microsecond delays between writes; the baud rate is a fixed 57,600.
The kernel initialises such a context many times during boot (the timing paragraphs), so
the model must tolerate repeated reprogramming without losing queued output. The Linux
SN1 port programs the same values except interrupt enable (it writes 0). Receive: the
kernel reads line status; if bits 0, 2 and 3 are all clear there is nothing to read; if
bit 0 is set it reads the data register; if bit 0 is clear but bit 2 or 3 is set it
reports a line error and discards. Transmit: the kernel writes up to 16 bytes to the data
register, then reads line status and continues only while bit 5 is set; a frame that
could not be fully written is continued later, from the transmitter-empty interrupt (it
enables interrupt-enable bit 1 for that) or from the next poll. A model that keeps bit 5
set whenever it can accept a byte satisfies this, as the register map already requires.

**Interrupt or poll** (established). The console driver connects Bedrock interrupt bit 4
as the UART interrupt when it attaches (the connection failure message is `Can't connect
UART interrupt.`), then enables the UART's received-data interrupt. The interrupt thread
drains the UART: it continues pending transmissions while line-status bit 5 is set and
runs the frame receiver while the interrupt-identification register reports a receive
cause (bits 1 or 2 of the low byte), then unblocks bit 4. Independently, a clock-tick
routine reads the 64-bit word at node local-register offset `0x01800098` (base
`0x9200000001800000` plus node number shifted left 33), tests bit 4, and when set writes
the value 4 to offset `0x01800090` and drains the UART the same way; the meaning of those
two registers (interrupt pending and its clear) is inferred from the bit and the write,
and their names are the Bedrock datasheet's business. A third path also runs the
receiver: the console polling thread, which polls the console subchannel. Consequence for
a model: replies to requests sent through the polled request path are collected by the
requester itself, so they need no interrupt; replies through the interrupt-waiting path,
events on subchannel 23 and console input are collected only by one of the three drain
paths, and without the Bedrock interrupt and pending bit they depend on the console poll
thread and arrive late, each such request waiting up to two seconds. This is the
[interrupt](#interrupts) section's open wiring question seen from the consumer.

**Framing and subchannels** (established). Framing identical to [framing](#framing); the
buffered sender caps a frame's payload at 127 bytes and reports how many bytes it took,
so a longer console write is split across frames by the caller; an unbuffered console
sender used for early boot and panic output writes flag, header `0x44`, escaped bytes,
check and flag straight to the data register with no cap of its own. The receiver
dispatches on the header byte: on subchannels 0 to 4 only type `0x00` (request) is
accepted and the bytes are console input; on subchannels 5 to 23 only type `0x20`
(response) or `0x40` (event) is accepted, anything else is a protocol error and the frame
is dropped; subchannels 24 to 31 are rejected, so the L1 must never use subchannels above
23 toward this kernel. Requests use the first free subchannel from 16 upward, at most 23;
every request opens one, sends, waits and closes it, so in practice requests go out on
subchannel 16 (17 to 22 only under concurrency from the user-level device). Subchannel 23
is reserved by the event listener and is the number the kernel tells the L1 to use for
events. Subchannel 4 is the system console; 0 to 3 are accepted as console input but the
kernel never sends on them.

**Request construction** (established). A request payload is the four-byte big-endian
destination, the two-byte big-endian code and, only when there are arguments, a count
byte followed by typed arguments; a zero-argument request is six bytes, with no count
byte, as in the Linux SN1 port (OpenBSD always sends it). Payloads are built in a
128-byte buffer. Destinations are stored per subchannel when it is opened and or-ed with
the task number when a request is built:

| Form | Value | Used for |
|---|---|---|
| local L1, general task | `0x0ffff006` | every general request below unless noted |
| local L1, command interpreter | `0x0ffff003` | text commands |
| attached I/O brick, general task | `0x4ffff006` | I/O identity on boards and widgets other than the Origin 300's (board identification, below) |
| attached I/O brick, command interpreter | `0x4ffff003` | PCI slot power text commands, rewritten to the local form on an Origin 300 |
| L2, command interpreter | `0x1ffff003` | partition power-down text, only when the kernel believes the system is partitioned |
| concrete rack and bay | type nibble, rack in bits 27:18, bay in 17:12, task 6 | verifying the source of an event from a module the kernel does not know: the event's source address with its task bits cleared, plus task 6 |
| user-level device | `0x0ffff000` or `0x4ffff000` plus task | the character device lets root pick the local L1 or the attached I/O brick |

**Reply interpretation** (established). The kernel takes the first four bytes as a
big-endian signed 32-bit code and returns the whole word to the caller when it is not
zero, so negative codes are seen as negative (unlike the Linux SN1 port's low-byte read).
On code 0 it reads the field-count byte, which must be present even for a reply with no
fields (callers zero their buffers, so a four-byte reply would pass by accident; the
model should send the count byte), then walks the expected fields: each field's type byte
must match the type the caller expects, integers are four big-endian bytes, strings are
copied to the terminator, binary fields carry their length in the low seven bits of the
type byte. The count byte must equal exactly the number of fields the caller expected; a
surplus or missing field, or a field of the wrong type, is a format error that the caller
treats as failure even though the code was 0. The client returns minus 100 to its callers
for a failure to open a subchannel or to send or receive, minus 103 for a failure to
build the request, minus 112 for a reply that did not parse; its own message table for
negative codes minus 125 to minus 100 (`System controller response timeout`, `Response
format error`, `Command send error`) is not printed on any path found.

**Send and receive** (established). Two request paths exist. The kernel-mode path sends
the frame (waiting for the transmitter), then polls for a response on the request's
subchannel by running the receiver repeatedly until a message is queued or the deadline
passes; the deadline is 2,000,000 units measured on the Bedrock real-time counter
converted through a clock-rate field, microseconds by the conversion's constants
(inferred), so two seconds; no message by the deadline is reported to the caller as minus
100. Most boot-time requests use this path. The busy-retrying path sends; if the send
returns busy (fewer bytes accepted than the frame), it delays 500 microseconds and
retries, up to 50 times; then, for the local L1, it waits on a synchronisation variable
for two seconds for the reply to be queued by one of the drain paths, and for a remote
controller polls as above. The system-serial, reset, partition-set, port-speed,
debug-switch and user-device requests use this path. There is no retry of an unanswered
request except one: the attached-I/O identity request is retried once on the local
address after a failure on the attached-I/O address. When the L1 never answers, each
request costs its caller two seconds and an error; nothing on the boot path waits
forever.

**Board identification, a Bedrock matter.** Throughout the IP35 kernel the board type is
read from a 64-bit word at node Hub special offset `0x34000000` (the kernel also reads
the local-form alias of the same word), a region OpenBSD's `hub.h` (lines 69 to 75)
names the first Synergy chip-select window; bits 15:12 identify the board. The kernel's
own decoding, used when it has to make up a product name for an unreadable EEPROM record,
is `0x4` for "IP34", `0x8` for "IP45", and otherwise "IP35" unless the L1's type letter
says a later board ("IP53"). It follows that an Origin 300 presents `0x8` in bits 15:12
(inferred from the decoding; the PROM trace or a hardware read of that word would confirm
it). This word is a Bedrock matter for the local-block datasheet, not this device's, and
the emulated Bedrock must present it: everything in this subsection about local versus
attached-I/O addressing depends on it. On an `0x8` board the kernel skips its early L1
type probe entirely (the probe is issued only when the word is not `0x4`, `0x8` or `0xc`)
and routes the requests that concern the attached I/O brick as follows: with an I/O
widget number of 0, 14 or 15 it uses the local L1 address; with widget 1 or any other
number it uses the attached-I/O-brick address, and on failure retries locally. Public
captures from real Origin 300s show the I/O bridges at widgets 14 and 15
(`/hw/module/001c01/Ibrick/xtalk/14/...` and `.../xtalk/15/...` in the Library mail
archive, messages `alt3c9$m4t$1@news.ucar.edu` and
`dsDBi.41158$S91.34159@newsfe7-win.ntli.net`). So on a one-chassis Origin 300 the IRIX
kernel addresses the local L1 for everything (established for widgets 14 and 15; whether
the kernel ever sees another widget on this board is not shown by the objects). The same
word selects the memory-module component numbering for EEPROM reads and the PCI power
text-command destination.

**Request table.** Argument counts are fields. "Reply accepted" is what the kernel takes
as success; the firmware's reply is in the [general-task table](#task-6-general-requests).
"Fidelity" says whether a fixed plausible answer suffices. All destinations are the local
general task unless a row says otherwise.

| Code | Arguments | Reply accepted | Who issues it and when | What the kernel does with it | On error or no reply | Fidelity |
|---|---|---|---|---|---|---|
| `0x0003` rack, bay, type | none | code 0, two integers: the first holds rack in bits 15:6 and bay in bits 5:0, the second the type letter | (a) every controller-context initialisation, which happens per module registration, per node hwgraph path, per I/O bridge attach and per crosstalk widget; (b) the early type probe on boards other than IP34, IP45 and `0xc`; (c) verification of an event from an unknown module, sent to that module's concrete address | module id: rack, type, bay; type letters `4 A 6 7 8 9 Q 0` become `C`, `% # E` become `P`, `= V` become `I`; the letter is lower-cased and must be one of c, r, i, k, x, d, p, n, %, #, 0, 1, 2, 3, 4, 5 (its position in that order is the packed type index) to form the id printed as `001c01`. For the I/O side the raw letter names the brick: `4` and `A` give "Ibrick", `7 8 9 Q` give "IXbrick", `0` "CGbrick", `Y` "Xbrick", `&` "Nbrick", `E` and `#` "PXbrick", `C R P N I X V` their own names, anything else "Unknown" | module id minus 1, brick name "Unknown"; the early probe logs a failure line with the request's status | must be right: rack and bay stable across all answers and equal to what the PROM recorded; type `4` on an Origin 300 so that the module is `001c01` and the I/O side is "Ibrick" as on real machines |
| `0x000e` | none | code 0, one integer | immediately after every `0x0003` in the module-id path (so per controller initialisation) and once more per node hwgraph path | the node's hwgraph link `module/<id>/slab/<n>/node`; for type `6` bricks it also adjusts the bay by the value's low two bits | a missing reply stores minus 100, so the link name becomes `slab/-100`; a reply that fails to parse stores 0 | answer 0; the kernel's name for it is its hwgraph path's `slab`, consistent with the cleared [Request 0x000e](#request-0x000e) meaning (the position within a brick) and not a firmware name |
| `0x0005` firmware revision | none | code 0, three integers major, minor, patch | per module for the hwgraph `FWVersion:%s` label (the version formatted first); per I/O widget for the same label on the I/O side; once per node that carries an R14000 (processor implementation `0xf`) for the version check | the version check computes major times 1,000,000 plus minor times 1,000 plus patch and requires at least 1,003,020 | version check unanswered: `Unable to obtain version of L1 firmware on brick %M, errno = %d.` and boot continues; too old: `L1 firmware not version %d.%d.%d or later on brick %M (found %s).` and `The R14000 processor requires version %d.%d.%d or later.` with the version numbers filled in (1.3.20 is the value the kernel reader found the code supplies; not re-verified by the review), `Please contact your service representative.`, a five-second delay, then the kernel reboots | must be right with R14000 guests: at least 1.3.20; real Origin 300s report 1.18.5 or 1.48.1 and the held image 1.44.0 |
| `0x0006` read EEPROM | four integers: component, area, offset, length 64 | code 0, one integer (bytes remaining after this chunk) and one binary field; the kernel copies exactly the field's length and repeats with offset plus 64 until the integer is 0 | node board record when the node is added to the hwgraph and at an I/O widget's node label (component 0: chassis area 1, then board area 2); I/O logic board record at each PIC or XBridge attach and by the on-board Ethernet driver (component 1, chassis then board area); memory-module SPD (area 4) and processor-module records have readers but no boot caller was found | hwgraph labels (`Part:`, `;Name:`, `;Serial:`, `;Revision:`, `;Group:ff;Capability:ffffffff;Variety:ff`), shown by `hinv -vm` as "Board: barcode ... part ... rev ..."; the I/O board's network field is the Ethernet address of the IOC3 at `pci/4` under an "Ibrick" path | the label is made up (a "fake" product name, part number and a random serial); the Ethernet driver prints a failure line ending `using OSLoadOptions`, takes the address from the PROM variable `OSLoadOptions` if set, else `%v: OSLoadOptions not set.  Could not get MAC address.` and the interface does not come up | records must pass the kernel's checks (below); the I/O board record must carry the network field or the guest needs `OSLoadOptions` |
| `0x0008` system serial | none | code 0, one string | on demand from the system-id system call (used by `sysinfo`, licensing), and by the Ethernet driver only on IP34 boards | copied as the system serial number | the system call fails; on IP34 a failure line ending `using OSLoadOptions` | a fixed plausible string (`L` plus seven digits, as the datasheet chose) suffices |
| `0x0009` partition id | none | code 0, one integer | only when the kernel believes the system is partitioned (a partition count above zero), at power-down; and through the user device | selects `p <id> pwr d` for the L2 | falls back to the local power-down | not issued on a one-chassis system |
| `0x100c` system state | two integers: state, and the low word of the processor interface's CPU-number register | code 0, no fields  | state 30 at the start of boot (after the master processor's early setup and again from the boot thread), 10 when the boot thread has started the system threads (twice), 50 at reboot, 80 before and 90 after a crash dump | nothing; the reply is discarded | ignored; each costs up to two seconds | any code-0 reply or none; not in any public header (established) |
| `0x1007` event subchannel | one integer: 23 | code 0, no fields | once, when the L1 console driver attaches | starts the event listener thread on subchannel 23 | `failed to set sysctl event-monitoring subchannel.  Sysctl events will not be monitored.`; a second attempt prints `sysctl event subchannel in use! Not monitoring sysctl events.` | answer code 0 and then send events on subchannel 23 |
| `0x2001` system reset | none | code 0, no fields, or no reply at all | at reboot when the kernel's panic-reboot setting is 2 | none; on failure `MSC Reset command failed. Doing warm reset` and a reset through two Bedrock network-interface registers | as left | the reply is optional; the reset itself must happen if the model claims it; the code differs from the public header's `0x2000`, as the firmware's row confirms |
| `0x2005` power off | none | code 0, no fields  | at halt, when an earlier L1 event of class `0x8000` set the kernel's power-off-received flag; the kernel then spins forever | none | none | must power the machine off |
| `0x1004` plus line, display | one string of at most 12 characters | code 0, no fields | only on boards whose type probe returned code 3 (a letter `9` board): `OS Running` at the end of boot, `Powering off` on the power-off event, `System NMI'd` or `System Panic`; the line index starts at 1 so the first line written is `0x1005` | none | strings longer than 12 are refused before sending | never issued on an Origin 300 (established) |
| `0x1009` LED | two integers: index (bits 11:8 of a packed value), state (bits 7:0) | code 0, no fields | only on IP34 boards (word `0x4`) at boot start and on panic or NMI, and on letter-`9` boards on panic or NMI | none | ignored | never issued on an Origin 300 (established); not in any public header |
| `0x0001` debug switches | none | code 0, one integer, high and low bytes returned separately | no caller in the IP35 objects | | | not issued |
| `0x0004` brick serial | none | code 0, one string | reachable only through an unreferenced helper | | | not issued |
| `0x000a` port speed | none | code 0, two integers; the first nonzero means 600, zero means 400 | no caller | | | not issued |
| `0x000c` attached bricks | none | code 0, eight integers: four pairs of packed rack and bay and type | no caller | | | not issued |
| `0x1002` console subchannel (one integer), `0x1003` console volunteer, `0x100a` event unregister, `0x2006` power cycle | | code 0, no fields | no callers | | | not issued |
| `0x1006` partition set | one integer | code 0, no fields | through the user-level device only | | | operator path |
| `0x2007` get and `0x2008` set (two integers) expander | | code 0, one integer / no fields | the Odyssey graphics driver | | | not on an Origin 300 |

**Text commands** (command interpreter, task 3, request code `0x0000`, one string;
established). The reply accepted is code 0 with no fields, or code 0 with one string,
which the kernel copies out to the caller when one is present; its parser requires the
count to match exactly.

| Text | Destination | When |
|---|---|---|
| `* pwr d` | `0x0ffff003` | at halt when the power-off flag is set; the master processor resends it every three seconds until power drops (007-4365-002 printed page 37 describes the operator's `init 0` followed by `power down` at the L1 prompt as the documented procedure) |
| `p <partition> pwr d` | `0x1ffff003` (L2) | as above when the kernel believes the system is partitioned; then `* pwr d` |
| `env reset` | the event source's own address, task 3 | every 1,000 clock ticks after an auto-power-down event (events, below) |
| `pci <bus> u`, `pci <bus> d`, `pci <bus> rst`, and `pci <bus> <slot> u` / `d` / `rst` | `0x4ffff003`, rewritten to `0x0ffff003` on an Origin 300 | PCI slot power and reset from the bridge driver's slot-power and reset entry points (operator hot-plug paths) |
| operator text | `0x0ffff003` or `0x4ffff003` | from the user-level device (operator tools, below) |

**Events from the L1** (established from the objects). After the console driver attaches,
a kernel thread waits on subchannel 23 without a timeout. An event frame carries header
`0x57` (type event, subchannel 23) and a payload of: four-byte source address, four-byte
big-endian event code, one-byte argument count, then typed arguments in the request
format, the layout the firmware sends ([L1-initiated packets](#l1-initiated-packets)).
The source address's task field (bits 4:0) must be 4, the environmental monitor; other
tasks are ignored. The source is then verified: type nibble 0, 3 or 4 with a rack and bay
that the kernel knows as a module, I/O brick or router is accepted; an unknown rack and
bay makes the kernel open a subchannel to that concrete address and ask it `0x0003`,
accepting the event only if the answer's type is `R`; type nibbles 1 and 2 (L2, L3) are
accepted without lookup. Code `0x8000`: the kernel prints `Power off request received.
Halting...` (once per event) and sends signal 2 to process 1, which begins an orderly
shutdown; on a letter-`9` board it also writes `Powering off` to the display; when the
kernel later halts it sends `0x2005`. Code `0x4101`: the kernel records the source,
prints `Auto power down will be delayed until shutdown is complete.` and from then on
sends the text command `env reset` to the source every 1,000 ticks until a send fails,
printing `Auto power down will not be delayed.` if the first send fails or `Unable to
allocate l1event structure.` if it cannot record the source; the manuals describe the L1
side, "Auto power down in xx seconds" every 5 seconds until shutdown (007-3938-001
printed page 27, 007-3938-006 printed page 48, 007-4498-002 printed page 109) and `env
reset` "to reset all current warnings and faults that are set" (007-3938-001 printed
page 48, 007-3938-006 printed page 72); the firmware's event table names `0x4101` as the
countdown step and `0x4102` as its end. Those public sources alone do not establish
the reset's effect. The selected 1.44.0 command
reading (citation: `docs/research/l1-env-reset-debug-firmware-reading.md`) shows that a reset
cancels the active countdown at its next semaphore check, rather than adding a
fixed delay to it; a recurring fault may start another countdown. Whether this
guest's requests arrive before a real next check remains unmeasured.

Any other code with exactly two arguments,
an integer followed by a string: the string, with trailing carriage returns and line
feeds removed, is logged with the integer as its tag; errors receiving print `Error
receiving sysctl event on nasid %d`. The Linux SN1 port at `v2.5.40` parses the same
layout (`l1.c` lines 2908 to 2960) and only logs; the power-off and auto-power-down
handling and the source verification are IRIX additions.

**When the requests are issued** (established from the call structure; the exact order
within a phase is inferred). Boot, before the hwgraph: the kernel's low-level reset
registers the L1 as its console when the PROM's configuration says so (console use,
below), and the early console port is set up; on an Origin 300 the early type probe is
skipped; the first requests come when the module table is built: for each node, a
controller context is initialised (UART programmed, `0x0003`, `0x000e`), and the R14000
version check sends `0x0005`. Boot, hwgraph construction: each node's hwgraph entry costs
another context initialisation (`0x0003`, `0x000e`), a `0x0005` for the module's firmware
label, and EEPROM reads of the node board (`0x0006`, component 0, chassis then board
area); each I/O bridge attach (PIC or XBridge) sends `0x0003` for the I/O identity (twice
through two helpers) and reads the I/O logic board's EEPROM (component 1); each crosstalk
widget initialisation sends `0x0003` twice, `0x0005` once, and reads the node board
EEPROM again. All of these are on the path to a login prompt; a two-second timeout on
each merely slows the boot, but the identity answers shape the device paths. Boot, main
thread: system state 30, later 10 (`0x100c`); the console driver's attach connects the
UART interrupt and registers subchannel 23 with `0x1007`; the Ethernet driver's attach
reads the I/O board EEPROM for its address. Runtime: the clock tick polls the
interrupt-pending bit; events arrive on subchannel 23; the system-serial request is sent
on demand by the system-id system call; operator tools send text commands and raw
packets through the character device; there is no periodic environmental polling by the
kernel, which expects the L1 to send events. Shutdown and reboot: `0x100c` with 50 at
reboot, 80 and 90 around a crash dump; at halt with the power-off flag, `* pwr d`
repeatedly; at reboot the Bedrock network-interface reset unless the panic-reboot
setting is 2, in which case `0x2001` first; after an L1 power-off event, `0x2005` at the
end of the shutdown.

**Console use** (established). The kernel uses the L1 as its system console when the
PROM-built configuration says so: it follows the KLCONFIG directory to the console record
and reads that record's 32-bit console type (the record layout is public: `klconfig.h`
lines 121 to 152 at `v2.5.0` and OpenBSD's `mnode.h` lines 111 to 144). Type `0x0300fafa`
makes the L1 the console for both output and the kernel's own console device;
`0x0300fafb` registers the console device only. The Linux SN1 port's configuration hack
writes `0x300fafa` into the same field (`v2.5.0` `arch/ia64/sn/io/klgraph_hack.c` line
151). The meaning of the `fafb` variant is unknown; the PROM's configuration writer would
settle it, and what the stock PROM writes on an Origin 300 decides which path the guest
takes. Output: until the kernel has taken over from the PROM it writes console text
through the PROM's own ARCS write vector, so the earliest kernel messages come from the
PROM's L1 driver, not the kernel's; afterwards, with the L1 as console, it sends event
packets on subchannel 4 (header `0x44`) carrying raw bytes, at most 127 per frame through
the buffered path, panic and early output through the unbuffered sender; with the L1 not
the console it writes to the IOC3 or IOC4 serial port and, in panic mode only, also to
the L1 port if it is ready. The kernel exposes the L1 console as the hwgraph device
`module/<id>/L1/tty`, a "console" edge, and the character device `.../L1/controller`.
Input: request-type packets on subchannels 0 to 4 are queued as console input; the
kernel reads subchannel 4. Ctrl-T and the L1's own mode switching are outside the
kernel's view. The kernel never sends the console-select or console-volunteer requests.
Note the difference from the PROM, which sends and reads on the CPU's own subchannel; the
firmware's retargeting rule under [subchannel semantics](#subchannel-semantics) moves
the L1's console input to subchannel 4 once the kernel's output arrives there (inferred
from the two readings).

**Operator tools** (established from strings and the device driver). The character
device at `/hw/module/<id>/L1/controller` gives root: an ioctl that sends an arbitrary
text command of up to 119 bytes to the command interpreter and returns the reply text;
ioctls for partition set (`0x1006`), firmware revision (`0x0005`) and partition id
(`0x0009`); raw writes of complete request payloads (up to 128 bytes) on a subchannel the
open reserved, and raw reads of whatever arrives there, including events. `l1cmd` finds
`/hw/module/*c*/L1/controller` and drives the L1 through this device, so any request the
firmware supports can reach the model at an operator's choosing. `sysctlrd` is the
Origin 2000 and Challenge system-controller daemon (it opens `/dev/sysctlr` and speaks of
MC3 and IO4 boards) and is not an L1 client; `sysctlrKill` drives system controllers
through a remote-service library (NMI, ping, reset, power cycle), not this kernel path.

**Which answers must be right.** Rack, bay and type (`0x0003`) on the local address: the
module id is used for every hwgraph path and the type letter picks the I/O brick's name;
for an Origin 300 the type must be `4`, rack 1 and bay 1 give `001c01`, and the same rack
and bay must be returned on every request and match what the PROM wrote in the
configuration, since the kernel takes the module id from the PROM's board records
elsewhere and the two must agree for its module table to recognise the source when
verifying events (established for the mechanism; that real Origin 300s answer `4` locally
is inferred from the kernel's normalisation table and the "Ibrick" names in public
captures, and the firmware's `0x0003` row now establishes `0x34` for an "ip45" type).
Firmware revision (`0x0005`): at least 1.3.20 with R14000 processors, else the kernel
reboots five seconds after printing the messages above (established). EEPROM records
(`0x0006`): the board area is parsed as an IPMI-style board info area: byte 0 format,
byte 1 length in 8-byte units, three date bytes, then length-prefixed fields
(manufacturer, product name, serial number, part number; the low six bits of the prefix
are the length), then SGI fields (a length-prefixed field, three single bytes, another
length-prefixed field), optional extension blocks distinguished by a format byte (1
licensing fields, 2 none, 3 configuration fields; anything else prints `unknown extended
data format found`), a terminator `0xc1`, and the 8-bit sum of all bytes in the area must
be zero or the record is rejected; the I/O board's network field, when present after the
extension blocks, is the Ethernet address as text; the chassis area is parsed similarly
with its own fields; a record that fails yields made-up labels. An
`OSLoadOptions` Ethernet fallback also exists, but its exact branch order
relative to record failure is not cleared. The 007-4498-002 appendix's printed
IP45 and IO-8 dumps are the
reference records (established for the checks; the exact SGI field
semantics are not needed by the kernel beyond copying them). `0x000e`: 0; anything else
appears in a hwgraph link name; no reply gives `slab/-100` (established). Event
subchannel (`0x1007`) and system state (`0x100c`): any code-0 reply with no fields (established). Events: header `0x57`, source address with type 0, the
module's rack and bay, task 4 (established). `* pwr d`: must power off; the kernel keeps
resending it every three seconds. UART: line-status bit 5 whenever a byte can be
accepted, bit 0 exactly while a byte waits; the repeated reprogramming must not drop
queued output; the Bedrock interrupt bit 4 and the pending word at local offset
`0x01800098` are what make events and console input timely.

#### Origin 300 IO8 Ethernet identity

The production `ef` path in the loaded
IP35 BASEIO PROM obtains its station address from L1 request `0x0006`,
component 1, board area 2. It reads the record in two chunks, parses an ASCII
Ethernet field. An `OSLoadOptions` fallback exists, but its exact branch
order is not independently cleared here. This is
separate from the IOC3 MicroLAN diagnostic reader and from the stock PROM's
"using BaseIO nic" module-identity message. The public OpenBSD IP35 IOC3
driver likewise calls `l1_get_brick_ethernet_address` when it has no attached
one-wire address part (`sys/arch/sgi/pci/ioc.c`, lines 530–546, and
`sys/arch/sgi/sgi/l1.c`, lines 850–913, revision
`2df91d80c104636deeef8124252ddd5951b55fbf`). The BASEIO path is
established by firmware disassembly and a QEMU debugger capture; the public
driver independently supports the board-area source, not the firmware's exact
instruction sequence.

The stock PROM constructs the module-identity `Laser` text from the IO8 BIA's
third ASCII field, converting its barcode from base 36 to a hexadecimal
number before selecting the BaseIO identity path. A zero numeric barcode
causes a timer-derived fallback in that conversion; the final `Laser` need
not be zero. This stock software path does not establish a fitted DS19xx part or
its electrical route. SGI's generic one-wire manufacturing reader also uses
the word `Laser` for a factory ROM serial, but that separate convention must
not be substituted for the PROM's BIA-derived value. The
identity investigation (citation: `docs/research/origin300-io8-laser-chip-population.md`)
records both meanings and their physical limits.

SGI's *NUMAlink Module System User's Guide* `007-4498-002`, printed page 182,
and *Origin 300 User's Guide* `007-4365-001`, printed page 75, both show an IO8
board information area (BIA) of 80 bytes and a chassis information area (CIA)
of 16 bytes. Their BIA examples place a `0xcc` ASCII-field tag at byte 64 and
twelve hexadecimal address characters at bytes 65–76, followed by `0xc1`, a
zero pad byte and an additive checksum. Three `0x04`-tagged, four-byte fields
occupy bytes 49–63 before that tag. The examples agree on the field boundaries;
their serial numbers, manufacturing dates, key contents and Ethernet addresses
are examples, not machine-wide constants. The meaning of the three binary
values is not settled here. Both areas must have a byte sum of zero modulo
256. The physical EEPROM package and L1-to-IO8 storage wiring have not been
identified by these sources.

The retained L1 1.44.0 firmware disassembly, packaged-image SHA-256
`0c969f9e98c73826495dc37bff685b3403e248993ba92194a288333d9d42b2f4`,
registers the IO8 board record on M-Bus controller 0 with an 8-bit I²C address
base `0xa4` (7-bit `0x52`) and a configured capacity of 512 bytes. Its generic
reader caps each M-Bus subtransaction at 16 bytes. It uses an 8-bit internal
offset and slave-address page bits for higher offsets; byte-wide controller
registers perform the transfer. Motorola's *MCF5206e User's Manual*, revision 1,
Section 13, describes that M-Bus controller register shape. This establishes
the L1 software's two-wire EEPROM access contract, not the fitted IO8 chip,
PCB route, factory origin of `Laser`, or any DS1742W role. It does not show an
IOC3 or XBridge one-wire access to obtain the BIA.

The earlier generated board record was *not* a faithful encoding of that BIA:
its overlong first two identity strings and absent three binary fields put the
MAC tag at byte 65 rather than 64. On QEMU
`816f74ec2abbe864605d1072a7f4c33d9ee4010a`, BASEIO decoded adjacent
non-hex record bytes as `e1:8b:00:3a:d0:d0` and refused to netboot. A
checksum-valid record preserving the published field lengths, with the same
configured address substituted into its ASCII field, reached `sash:` without
`OSLoadOptions` on run `irix-20260915T050544-2b61ec`. A second run,
`irix-20260915T051821-78b0fa`, also reached `sash:` with a synthetic serial
and zeroed unknown fields, plus the published 16-byte CIA shape. The current
generator produces the published field boundaries with explicitly synthetic
contents; run `irix-20260915T063803-6193a6` reached `sash:` from that
generated BIA/CIA without `OSLoadOptions`, on the same QEMU revision.
These are discriminating emulator runs, not physical EEPROM captures or an
IRIX boot.
The generated BIA and CIA must use the published field shape, mark synthetic
values as such, keep valid checksums, and put the same station address in the
board record that the IOC3 Ethernet device was configured to use. A machine
qtest must read both L1 areas through request `0x0006` and independently
check their lengths, checksum, MAC-field location and configured-address
equality; a sash run is a separate acceptance check.

**Differences from the Linux SN1 consumer (`v2.5.40`).** Reply code: IRIX uses the whole
32-bit word; Linux keeps the low byte (`l1.c` lines 133 to 139). Field count: IRIX
requires the count byte to match the expected fields exactly; Linux decrements it per
field but does not require it to reach zero. Requests Linux has that IRIX never sends:
console select and volunteer, port speed, debug switches, PCI up, down and reset as
requests (IRIX sends PCI power as text commands). Requests IRIX has that Linux lacks:
`0x000e`, LED `0x1009`, event unregister `0x100a`, system state `0x100c`, power off
`0x2005`, power cycle `0x2006`, expander `0x2007` and `0x2008`, attached bricks `0x000c`.
Reset: IRIX `0x2001` with an optional reply; the Linux header `0x2000` (`ksys/l1.h` line
260); the firmware serves `0x2001` and not `0x2000` (change 2 under [contract
changes](#contract-changes-in-this-revision)). Events: the same layout; IRIX adds source
verification, the power-off and auto-power-down behaviour and `env reset`. Interrupts:
IRIX connects Bedrock bit 4 and polls a pending bit; Linux's interrupt path is compiled
out. Memory-module component numbering: IRIX picks by the board word: `0x8` boards (IP45)
use 2 plus half n plus 2 when n is odd, for n below 4, which computes to components 2, 4,
3 and 5 for modules 0 to 3, the firmware's table under [Requests](#requests); `0x4`
boards 4 plus half n plus 2 when odd; plain IP35 3 plus half n plus 4 when odd (the same
as OpenBSD's C-brick arm); letter-`9` boards 5 plus half n plus 4 when odd (OpenBSD's
Origin 350 arm). The datasheet's earlier conflict was a board dependency, not a
disagreement. No boot-time caller of the SPD path was found in this kernel.

**IRIX-side unknowns, and what would settle them.**

| Unknown | What would settle it |
|---|---|
| The value bits 15:12 of the Synergy-0 word take on a real Origin 300: inferred `0x8` from the kernel's decoding; a Bedrock local-block matter on which this subsection's local-versus-attached-I/O addressing depends | a PROM trace or a hardware read of the word at `0x9000000034000000` |
| The type letter a real Origin 300 L1 returns on the local address: inferred `4` here, established `0x34` for an "ip45" type by the firmware reading | a capture of `0x0003` on the local address (the PROM sends it: `Module: 1 (001c01) from Sys Ctlr`) |
| What `0x000e` means and what the firmware answers: inferred 0 here | the cleared [Request 0x000e](#request-0x000e) reading gives the meaning and the value; a capture would confirm it |
| Whether `0x2001` or `0x2000` is the firmware's reset code, and whether `0x2005` powers off | settled by the firmware reading: `0x2001` and `0x2005` (change 2) |
| The event the L1 sends before an auto power-down and its exact code: the kernel only shows `0x4101` | the firmware's event table gives `0x4101` per countdown step and `0x4102` at its end; a capture would confirm the arguments |
| The meaning of console type `0x0300fafb` versus `0x0300fafa` | the PROM's configuration writer |
| The names of the Bedrock registers at local offsets `0x01800098` and `0x01800090` (read for bit 4, written with 4) | the Bedrock datasheet's interrupt block |
| Whether the interrupt-thread request path ever waits on an Origin 300 without the Bedrock interrupt wired: it depends on the console poll thread's period, which was not traced | tracing that thread, or a run with the UART interrupt unwired and a timed request |
| Which hwgraph builder issues the second node-board EEPROM read and whether any boot path reads the SPD records: the call sites exist but their component codes were not fully traced | a request log of `0x0006` arguments from a boot to the login prompt |

## Lifecycle

Merged from the lifecycle and console research draft of 2026-09-07, written
clean-room from the published manuals, the retained transcripts catalogued in
`docs/library/boot-logs.md`, the three public consumers and the operator-visible
strings of the retained firmware range named under "Sources used". That original
draft used string inspection only. The selected 1.44.0 disassembly now supports
the separately cited environmental reset and debug-switch software paths; it
does not establish physical timing or fitted parts.

Scope: one Origin 300 chassis (board IP45), no L2, at most a cabled peer, with the
stock IP35 PROM and IRIX as guests, in
the 2001 to 2002 vocabulary this datasheet models; later firmware (1.44.0 in the
retained image, 1.48.1 on owners' machines) is cited where it prints something the
2001 manuals print differently. Marks: established means a manual states it, a
transcript shows it, or an operator-visible text in the image shows it directly;
inferred follows from established facts, with the step named; approximated is a shape
that no source pins exactly; unknown means no source examined settles it, and "settle
by" names what would. A text quoted from the image is a string the firmware prints;
that it exists proves the firmware can print it, not when. Where a manual and the
image differ, both are quoted and the difference is marked. "The image" means the
982,412-byte Origin 300 range at version 1.44.0.

The L1 and the node have separate lifecycles. The published Origin 300 guide
shows the L1 display running with mains power before the server is powered on
(007-4365-002 printed pages 35–37). A base-module L1 also remains powered
after module power-down in the named NUMAlink-module topology
(007-4498-002 printed page 89). The rail voltages, regulators and switching
circuit used by the Origin 300 IP45 hardware are unknown. Everything in this
section separates what the L1 keeps from what the node keeps.

### What the L1 keeps

| State | Where it lives | Survives node reset | Survives node power-off | Survives L1 reboot or mains loss | Mark and source |
|---|---|---|---|---|---|
| rack and slot (the prompt's `001c01`) | non-volatile memory | yes | yes | yes; a change made with `brick rack` or `brick slot` is stored at once but "takes effect on next L1 reboot/power cycle" | established: 007-3938-001 printed page 23 ("stored in non-volatile memory on the L1"); 007-4498-002 printed page 179; the image prints the same "(takes effect on next L1 reboot/power cycle)" suffix |
| brick type and pseudo-type | non-volatile memory | yes | yes | yes, applied at next L1 boot | established for the image ("brick type changed (nvram) (takes effect on next L1 reboot/power cycle)"); the `brick` command reports `source: EEPROM` for the type on an Origin 300 (007-4498-002 printed page 179) |
| partition number | non-volatile memory | yes | yes | inferred yes (set with `brick partition`, reported by `brick`; no source names the store) | 007-3938-001 printed page 39; "WARNING: brick appears to get partition from L2." in the image |
| network mode (USB or RS-422) and autodetect | non-volatile memory | yes | yes | yes; "nvram parameter changed, reset required to affect change." | established: 007-3938-001 printed page 64; 007-4498-002 printed page 194 |
| debug switches | checksummed primary and backup core settings records in the selected L1 software; fitted NVRAM chip unknown | yes while the L1 remains powered and running; physical capture still needed | yes while the L1 remains powered and running; physical capture still needed | a selected-image L1 firmware restart restores a valid primary or backup, or defaults if both fail; mains-off retention unknown | selected 1.44.0 software: firmware reading (citation: `docs/research/l1-env-reset-debug-firmware-reading.md`) and independent review (citation: `docs/reviews/l1-env-reset-debug-firmware-evidence-review-20260915.md`); published command: 007-3938-001 printed page 45; Origin 350 `graphica` lines 1057 to 1061 and PROM warning `0x10d` in IRIXnet 2737 support only a value after power-up, not IP45 restart or chip fitment |
| environmental thresholds and altitude setting | not stated | inferred yes | inferred yes | unknown (the image prints "temperature limits set."; no source names the store) | 007-3938-001 printed page 48 describes the commands; no source shows a threshold crossing any event; settle the first two cells by an `env` read after a `reset` and after a `power down`, `power up` pair |
| the event log | non-volatile memory | yes | yes | yes: one `log` listing spans three L1 boots over six years, each opening with `L1 booting 1.38.4` | established: 007-3938-001 printed page 3 ("Logs information in NVRAM"); Nekochan 16729141 lines 94 to 214; the image logs "nvram log size changed - log reset." when a new image changes the log size |
| date and time | battery-backed timekeeping RAM with a real-time clock, the only socketed part on the L1 | yes | yes | yes while the battery lasts; a dead or blank chip boots to `02/07/2106 06:28:15` with "nvram checksum error - initializing core data." in the log | established for the family: 108-0240-002 printed page 2-45; the router and Fuel sessions in `graphica` lines 365 to 386 and 1909; an Origin 300 owner found an unidentified Dallas chip "hanging by a few pins" after removing the PCI cage (IRIXnet 2737 line 611; the post names neither a part number nor its function) |
| system and brick serial numbers | non-volatile memory (system) and EEPROM (brick) | yes | yes | yes | established: 007-3938-001 printed pages 77 to 78; 108-0240-002 printed page 3-2 |
| auto-power setting | non-volatile memory | yes | yes | yes ("auto power up aborted, nvram changed" in the image's log texts) | established: 007-3938-001 printed page 38; the image |
| console mode (L1 mode or console mode) | volatile | inferred yes (the inference under [node reset the L1 drives](#node-reset-the-l1-drives)) | unknown after `power down` typed from the escaped state (see [node power-off and power-on](#node-power-off-and-power-on)) | no: the L1 comes up in L1 mode | established for boot (the Origin 300 power-on procedure starts at the prompt, 007-4365-002 printed pages 35 to 36); the rest of this row is under [console multiplexing](#console-multiplexing) |
| console selection (which subchannel gets operator input) | volatile | inferred yes (rests on `graphica` line 1792, which follows a `debug 0x0` and no reset) | prints `CPU0` after every captured `power up` (three captures); whether that is a reset of a moved selection or the untouched default is inferred; settle by a capture of `power up` after the designation had changed to `console` | inferred no; the conflict paragraph after this table records the opposing inference | three captures print `CPU0` right after `power up` (IRIXnet 2737 line 1457; `graphica` lines 1061 and 1924), none of which shows the selection anywhere but the default before that power-up; the rule "by default, the C-brick attempts to communicate with its local CPUs" (007-3938-001 printed page 20) |
| the display's two lines | volatile, rewritten by the L1 on every state change and by software through the display requests | line 2 shows the reset text, then keeps it | line 2 shows `Powered Down` | no: the L1 rewrites both lines at boot | established: the display table under [management console](#management-console) |
| command history (`history`) | not stated | inferred yes | inferred yes | unknown | 007-3938-001 printed page 54 describes the command; settle by a `history` read after a `reset`, after a `power down`, `power up` pair and after `reboot_l1` |
| the flash images A and B and the default choice | flash | yes | yes | yes; the boot image validates both and runs the newer unless `flash default` says otherwise | established: 007-3938-001 printed pages 21 to 22 and 52 to 53 |

**Where the draft and the request-`0x000e` reading differ.** The console-selection row
infers that the selection does not survive an L1 reboot, and the
[lifecycle unknowns](#lifecycle-unknowns) leave it open. The
[request `0x000e`](#request-0x000e) subsection, read by disassembly, establishes that
the 128-byte settings record holding the console-subchannel byte that `0x1002` rewrites
sits in the controller's on-chip RAM, not in flash or an I2C device, and infers that it
survives an L1 firmware restart and is lost with standby power. Both survival
statements are inferences. The lifecycle and console
review (citation: `docs/reviews/l1-lifecycle-console-contract-review.md`) adds one string-level
observation that bears on both sides: the image's log texts `nvram checksum error -
initializing core data.`, `nvram checksum error - core data invalid, using backup
copy`, the same pair for an extended record, and `nvram checksum error - log pointers
invalid, ...` describe a checksummed record kept with a backup copy and re-initialised
at L1 start when its checksum fails, which are the three properties the `0x000e`
reading attributes to the settings record, and the texts name that record NVRAM. If the
settings record is that core data, the selection survives an L1 reboot and a mains loss
while the battery lasts, the opposite of both inferences; whether the two are the same
record cannot be shown by strings. The conflict stands unresolved and both statements
stand as written; the settling observation is `select` and `0x000e` before and after
`reboot_l1`, and again after a mains cycle. The `0x000e` subsection's own state
paragraph is the other reviewer's and does not yet carry this observation.

### Node reset the L1 drives

Events: the `reset` command (alias `rst`); the `softreset` command (alias `softrst`);
the `nmi` command; the front-panel reset and NMI buttons; a peer's `ctc reset` or a
broadcast `* reset` over the RS-422 link; and, if honoured, the protocol's full-reset
request `0x2000`.

**What each does.** `reset` "sets various control and status signals back to their
default values, and reboots the operating system" (007-3938-001 printed page 74;
007-4498-002 printed page 201) (established). The front-panel reset button "resets the
internal processors and ASICs. The reset will cause a memory loss" (007-4365-002
printed pages 6 and 40) (established). `softreset` asserts the soft-reset signal
through an I/O expander; the NMI button and `nmi` assert the non-maskable interrupt
and keep memory, IRIX writing a crash file (007-3938-001 printed pages 64 and 78;
007-4365-002 printed page 7; 007-4498-002 printed pages 194 and 203) (established).
The L1 drives all three signals "on command from software" as well (007-3938-001
printed page 2) (established); the only public request that maps to one is `0x2000`,
which the Linux SN1 port sends to the local general task with no arguments, treating a
reply timeout as success because "we've sent the reset" (`l1_command.c` lines 866 to
906); no kernel path calls it at `v2.5.40` (a `git grep` over `arch/ia64` and
`include/asm-ia64` finds the request's definition and its one use inside the reset
helper, and the helper has no caller), OpenBSD never sends it and reboots
through the Bedrock's network-interface reset register instead (`ip27_machdep.c`
lines 700 to 729), and the MIPS out-of-tree port sends only console bytes. Whether the
image honours `0x2000`: unknown; string inspection cannot show a request-code
table. Settle by a source-reader's read of the image's request dispatch, or by the
stock PROM or IRIX sending it on the emulated machine (the request log that the
[`0x000e` row](#requests) already relies on). The
draft and the [firmware cross-check](#firmware-cross-check) differ here: the
cross-check, read by disassembly, finds the general task's dispatcher sending `0x2000`
down its unknown-request path, minus 101 unless a board-specific hook is installed;
the string reading leaves it unknown. Both stand as written. The request catalogue of
the protocol-parity revision settles the question the same way as the cross-check:
`0x2000` is unrecognised and `0x2001` is the reset the `reset` command performs, answering the
ten-byte fieldless success (which the reset may cut off) and logging `reset (OS)`
(change 2 under [contract
changes](#contract-changes-in-this-revision)).

**Keeps.** Everything in the state table: the L1 is not reset by any of these. Its log
gains one entry naming the source: `reset (COMMAND)`, `reset (PANEL)`, and the image
also has `soft reset (…)`, `NMI (…)`, `reset hold (…)` and `reset release (…)` with
sources `COMMAND`, `PANEL`, `ENVIRONMENTAL`, `DON'T CARE` or `unknown` (established
for the family: Nekochan 16724335 lines 2019 and 2043 show `reset (COMMAND)` on an L1
at 1.28.3; the image carries the texts). The environmental monitor keeps running
through a reset (no source says otherwise; inferred).

**Prints.** On the console, `reset`, `softreset` and `nmi` print nothing but the next
prompt (007-3938-001 printed pages 65, 74 and 78) (established). A button press prints
nothing on the console in any capture (unknown; no capture spans one). On the display,
line 2 shows `Sft rst...` then `Sft rst done` or `Sft rst fail` for a soft reset and
`NMI...` then `NMI done` or `NMI Fail` for an NMI (007-3938-001 printed pages 64 to 65
and 78; the image prints `Soft reset..`, `Sft rst done`, `Sft rst fail`, `NMI...`,
`NMI done` and `NMI fail`, so the 1.44.0 texts differ from the 2001 print in two words)
(established, with the variant marked). For `reset` and the reset button the manuals
say nothing about the display; the image carries `Reset...` and `Reset done` beside the
button texts (inferred: line 2 shows them on a reset).

**Sends to the processors.** The reset, soft-reset or NMI signal. No source shows an
event packet on reset (unknown). After the reset the PROM starts again and its first
line, `Starting PROM Boot process`, appears on the console if the L1 is in console
mode (all captures) (established).

**Mode.** The reset does not change the L1's mode. If the command was typed from the
escaped state (after Ctrl-T), the L1 returns to console mode when the command
completes, as after any command (inferred from the general rule under
[console multiplexing](#console-multiplexing) and the Origin 350 session, where `debug
0x0` from the escaped state was followed by `returning to console mode  001c01 CPU0,
<CTRL_T> to escape to L1`, `graphica` lines 1788 to 1792).

**In-flight request.** Unknown. The L1's reply to a request it had already accepted is
presumably still transmitted; whether the Bedrock UART discards received bytes on node
reset is a Bedrock question, not an L1 one. Settle by a wire capture at the
Bedrock-side UART across a reset, which no held document provides.

**The processor-facing UART.** The L1's own view of that port persists: the `uart`
command shows the Bedrock-side port `Connect`ed with read-error and write-timeout
counters that accumulate across events (007-4498-002 printed page 204 shows 13 read
errors on an Origin 300; the Fuel session shows 42) (established that the counters live
on the L1; what a node reset does to bytes queued in either direction is unknown).

**A second reset after power-up.** On an Origin 350 the L1 logged `power up
(COMMAND)`, five seconds later two link-configuration lines, one second later `reset
again MIPS`, and the same two link lines five seconds after that (Nekochan 16729141
lines 112 to 127, repeated identically at lines 154 to 169) (established for that
machine). The image carries `reset again MIPS`, so the Origin 300's L1 has the same
step (inferred). This means the node is reset by the L1 once more, about six seconds
after power is applied; the PROM lines seen on the console after `power up` are the
lines printed after that second reset (inferred; no capture timestamps the console).

### Node power-off and power-on

Events: `power down` (alias `pwr d`); `* power down` to the local brick and its
attached peer; a guest's text command `* pwr d` through the command-interpreter
request (OpenBSD `ip27_machdep.c` line 665 and `l1.c` lines 926 to 940; Linux SN1
`l1_command.c` lines 1000 to 1019, which sends it to the local L1, or to the L2 when
routers exist); the front-panel power button; an environmental fault, on which the L1
prints `Auto power down in xx seconds` every 5 seconds until it shuts down, then
`Brick or server appears to have been powered down` (007-3938-001 printed page 27; the
image's forms are `brick auto power down in <n> seconds` and `brick appears to have
been powered down`, established texts; where they print, console or log, is inferred
from the manual, and none fits the twelve-character display); and, when an L2 exists,
its `pwr d`. Power-on: `power up` (`pwr u`), `* power up`, `power up hold` (listed on
007-3938-001 printed page 72 and 007-3938-003 printed page 81 without a description;
the image's help text `power up, hold reset` names it as power on with reset held; the
router log shows `power up hold (COMMAND)`, Nekochan 16724335 line 2016), the button,
and the auto-power countdown.

**What happens on power-off.** The L1 requests node power-off and stays
running; the Origin 300 IP45 rail voltage and switching circuit are unknown:
"Powered Down appears on the L1 display when the module is
powered off. The L1 controller is still powered on" (007-4498-002 printed page 89,
the named NUMAlink-module configuration). The environmental monitor reports
every supply, fan and sensor in state
`Wait Pwr` while the node is off (IRIXnet 2737 lines 1365 to 1419, an Origin 300 on
1.18.5) (established). `power check` reports `power appears off` (the image; the 2001
shape is `power appears on`, 007-3938-001 printed page 73) (established).

**Two-step power-down.** The image carries `Power Down: issue 'pwr d' again to
immediately power down.` and `Power Down: press power button again to immediately
power down.` (established that the texts exist). So a first power-down request can be
deferred, and a second forces it. What the L1 does in the deferred interval (whether
it sends the running system an event on the registered system-event subchannel and
waits for an orderly shutdown, and for how long) is **unknown**; no manual describes
it, and the Origin 300 procedure avoids it by halting IRIX first (`init 0`, Ctrl-T,
`power down`, 007-4365-002 printed page 37). The guest-side power-off, `* pwr d` after
the guest has halted itself, is the immediate case: OpenBSD waits one second and then
prints a failure message if it is still running (`ip27_machdep.c` lines 666 to 669),
which on the family is not reached in any held capture (no Origin 300 power-off
capture exists; unknown). Settle the deferred case by a capture of `power down` typed
while IRIX runs on hardware, or by an image reader.

**Keeps.** Everything in the state table. The log gains `power down (COMMAND)` or
`power down (PANEL)` (Nekochan 16729141 lines 133, 175 and 214; 16724335 lines 2013 to
2124) (established for the family). The designation printed after the next `power up`
is `CPU0` in every capture (three captures); whether that is a reset of a moved
selection or the untouched default is inferred, and a capture of `power up` after the
designation had changed to `console` would settle it (the state table).

**Prints on power-off.** `power down` prints nothing but the next prompt (the Origin
300 procedure shows no output, 007-4365-002 printed page 37; the L2 form `* power
down` likewise, Nekochan 16724335 line 2656) (established). The display shows `Pwring
Down` then `Powered Down` (the image; `Powered Down` also 007-4498-002 printed page 89)
(established); `Power Down Error` is a sixteen-character text in the image whose
twelve-character visible form is unknown. A refused power-down from a disabled
button prints `Power down attempted from disabled front-panel power button` (the
image, whose `button` command enables or disables each front-panel button;
established that the text exists).

**Prints on power-on.** `power up` prints nothing itself; in the 2001 manuals the next
prompt follows (007-3938-001 printed pages 18 and 73), and on real machines the L1
then switches into console mode on its own and prints the mode message (next
paragraph). A refused power-up prints an error in the standard message form and
leaves the node off: `001c01 ERROR: SerNum:No assigned System Serial Number. See log
for details.`, once per brick for `* power up` on a two-chassis Origin 300 (Usenet
2005, `1132778527.644765.40810@g14g2000cwa.googlegroups.com`); a router printed
`001r07 ERROR: SerNum:Invalid System Serial Number format. See log for details.`
(`graphica` line 406) (established). The image's pre-power check texts are `System
failed pre-power check`, `Brick serial number mismatch`, `System serial number
mismatch`, `Brick disabled`, `System serial number invalid format` and `No system
serial number assigned` (established that they exist; the check rules are on
108-0240-002 printed page 3-2 for the family: the brick serial in NVRAM must match the
EEPROM, the system serial must be of the form `Lnnnnnnn`, must not be the disabling
value, and must match the neighbours; a blank `L0000000` is learned from a neighbour).
A `power up` while already on prints `Power is already on` (the image) (established
that the text exists). The display shows `Powering Up` then `Powered Up` (the image;
`Powered Up` on 007-4498-002 printed page 181 and in a 2005 owner's words, "the L1
display tells me 'Powered up'", `1132172144.919058.181550@g49g2000cwa.googlegroups.com`)
(established). The log gains `power up (COMMAND)` or `power up (PANEL)` (Nekochan
16729141 lines 115 and 196) (established for the family).

**Mode after `power up`.** The L1 enters console mode on its own, selecting CPU 0's
subchannel, and says so before the PROM's first line:

- from the escaped state, on an Origin 300 at 1.18.5:
  `returning to console mode  001c01 CPU0, <CTRL_T> to escape to L1` then
  `Starting PROM Boot process` (IRIXnet 2737 lines 1453 to 1459); that capture shows
  no `escaping to L1 system controller` line before the session's commands, so the
  escaped state there is inferred from the word `returning`, by the rule derived from
  the `graphica` captures where the escape line is present (the transitions table and
  the power-on sequence inherit this inference);
- from the locked prompt, on an Origin 350 whose session prints no L1 version and a
  Fuel at 1.48.1: a fresh prompt line, then `entering console mode  001c01 CPU0,
  <CTRL_T> to escape to L1` then `Starting PROM Boot process` (`graphica` lines 1059
  to 1062 and 1922 to 1925).

Both texts are in the image with a single space after "mode" and a placeholder for
the id, so the two spaces in every capture belong to the inserted identity string,
which the image builds from an id format and a designation string that itself begins
with a space (` CPU0` to ` CPU3`, ` console`) (established texts; the assembly
inferred). The word is `entering` when the L1
was holding L1 mode and `returning` when it had been escaped from console mode with
Ctrl-T (inferred from the two states in the captures). The manuals print neither
transition after `power up`; they print `entering console mode 001c01 console,
<CTRL-T> to escape to L1` only for an operator's Ctrl-D (007-4365-002 printed page
41). What the mode becomes after a `power down` typed from the escaped state is
**unknown**: the general rule says the L1 returns to console mode when the command
completes, and the image can print `WARNING: power appears off, console unavailable`
in that situation; no capture shows which. Settle by a capture of `power down` from
the escaped state.

**Sends to the processors.** On power-off, nothing that survives. On power-on, the L1
applies power, releases reset (twice; see [node reset the L1
drives](#node-reset-the-l1-drives)) and the PROM speaks first (its first request on
the emulated machine is the [`0x000e` row](#requests)).
No source shows the L1 sending a packet on its own at power-up (unknown).

**In-flight request.** A request received before a power-off is presumably answered
into a dead UART; unknown. After a power-on the processor side starts empty (inferred:
the Bedrock is reset).

**Auto power-on.** With `autopower on` the L1 begins a countdown at its own boot,
logging `auto power up countdown initiated` and, when it fires, `auto power up
initiated` followed by `power up (COMMAND)`; an Origin 350 measured 85 seconds in two
samples, and a button press during the countdown logged `power up (PANEL)` five
seconds in (Nekochan 16729141 lines 109 to 115, 151 to 157 and 193 to 196)
(established for the family). The image prints `INFO: <id> will power up system in NN
seconds...` and aborts the countdown on `autopower abort`, on an NVRAM change, or when
an L2 is detected (`auto power up aborted, user abort`, `…, nvram changed`, `…, L2
detected`) (established texts); that the countdown ends in `* power up` is inferred
from that text standing among the auto-power texts, not an observed command. The
Origin 300 default is off:
`auto power on is disabled.` (007-4498-002 printed page 178) (established).

### Controller restart

The [Origin 300 System with NUMAlink guide's `reboot_l1`
entry](https://techpubs.jurassic.nl/library/manuals/4000/007-4498-001/sgi_html/apb.html)
describes no-argument restart as selecting the newest valid image. The
selected 1.44.0 image's actual restart choice remains
unmeasured. Events: `reboot_l1` (alone; `reboot_l1 a`, `b`,
`current`, `old`, `new`, `other`; `addr <exp>` and `raw <exp>` boot a flash image by
address), a mains power cycle (unplugging the module), a firmware fault (the image
prints `controller firmware hang!   resetting...`, `controller firmware panic!
resetting...`, `memory fault!   resetting...`, `bus (alignment) error!
resetting...`, `unexpected interrupt N!  resetting...` and `reset for unknown reason
(N)!  resetting...`, then restarts) (007-3938-001 printed pages 65 and 74;
007-4498-002 printed pages 195 and 201). The separate `nvram reset` command
returns settings to factory defaults. Whether it also restarts the controller
is unknown; the manuals do not say.

In the selected L1 1.44.0 IP45 image, no-argument `reboot_l1` chooses its
normal restart branch with zero alternate-image address. That branch attempts
a 100-unit task wait before asking lower reset support to restart the
controller. Its reset routine stores reboot state, calls a lower helper,
conditionally executes ColdFire `HALT` when a device control bit is clear,
then writes a boot-address pair and its complement before waiting in a loop.
The fresh seeded disassembly (citation: `docs/research/l1-debug-console-parser-firmware-reading.md`)
and independent evidence review (citation: `docs/reviews/l1-debug-parser-firmware-evidence-review-20260915.md`)
clear that instruction order, not the fitted reset mechanism, real elapsed
time, actual image chosen or reply delivery. If reset re-enters this selected
image at its normal vector, startup validates the primary settings record,
recovers a valid backup if needed and otherwise installs defaults. The command
does not restore NVRAM itself, and physical retention across mains loss is
unknown.

**Boot image choice.** The boot image validates images A and B and runs the newer
unless `flash default` chose one; a new image is used only after `reboot_l1` or a
power cycle (007-3938-001 printed pages 21 to 22 and 52 to 53) (established). If the
chosen image fails during boot the L1 boots the other and logs `Running Flash image X
because image Y failed during boot and is probably corrupt.`; with no valid image it
prints `WARNING!  NO VALID BOOT FLASH IMAGE!!` (the image) (established texts). An
image built for another product family shows `  UNKNOWN   ` and `BRICK TYPE!!` on the
two display lines (the image's two 12-character strings; an Origin 300 owner saw
"UNKNOWN BRICK TYPE!!" on the front LCD after a flash, Usenet 2005) (established).

**Keeps.** The non-volatile rows of the state table, and applies the pending ones:
rack, slot and brick type take effect at this boot, so the prompt and display change
here (established). The log keeps its entries and gains `L1 booting <version>` (three
boots in one listing, Nekochan 16729141; the router logged `L1 booting 1.42.9` at the
moment of an L2-issued `reboot_l1`, `graphica` lines 348 to 386) (established for the
family). The image also has `L1 rebooted`, a text of unknown destination, display or
log: it sits with the display texts `L1 booting`, `  UNKNOWN   `, `BRICK TYPE!!` and
`L1 running`, while the log form `L1 booting <version>` is elsewhere; which event
writes it is unknown.

**The node is not affected.** An Origin 350 running IRIX flashed both its L1s and
rebooted them with `l2cmd l1 reboot_l1 a`, then queried them with `l2cmd l1 ver` from
the same IRIX session (Nekochan 16721376 lines 1053 to 1080) (established for that
machine and route; inferred for an Origin 300 through its serial console, same
firmware family). So an L1 restart neither removes node power nor resets the node.
Whether the L1 re-asserts anything toward the node when it comes back up: unknown.

**Prints.** On the console during the restart: unknown (no serial-console capture
spans an L1 reboot; the L2-side captures show only the L2's own `INFO: closed
connection to 000r00` and `INFO: opened USB device` lines, which are not the L1's);
what, if anything, a model prints on its console during `reboot_l1` is therefore a
choice it must declare. On
the display: `L1 booting` then `L1 running` (the image carries both as display texts;
`L1 running` is what the manuals say a freshly powered L1 shows, 007-4365-002 printed
page 35; 007-4498-002 printed pages 85 and 87; 007-4240-002 printed pages 47 and 48)
(established). A brick re-plugged and left unpowered showed `line 1: "001c02     /"`
and `line 2: "L1 running  "` on `dsp` (Nekochan 16724335 lines 2394 to 2397)
(established).

**Mode.** L1 mode, at the prompt, after the operator presses Enter (the power-on
procedure, 007-4365-002 printed page 35) (established for mains power-on; inferred for
`reboot_l1`). Console selection returns to its default (inferred; volatile; the
request-`0x000e` reading's differing inference is recorded under the state table).

**Sends to the processors.** Nothing known. A running guest that had registered an
event subchannel (request `0x1007`) would find the registration gone; the image's
status text for that state is `system events are disabled` beside `system event
subchannel N` (inferred from adjacency to the `L1 <-> system protocol is PPP` status
text; unknown whether the registration survives, settle by an image reader or by
`syscom` before and after `reboot_l1` on hardware). The events reading of the
protocol-parity revision settles it: the subchannel is persisted and restored at the
next L1 boot when it is 4 or more, so a registration of 23 survives and one of 0 to 3
does not ([L1-initiated packets](#l1-initiated-packets), change 9).

**In-flight request and UART bytes.** Lost: the controller restarts (inferred). The
Bedrock-side UART's own state is the Bedrock's (out of scope here).

### Power-on sequence from mains

1. Mains applied. The L1 runs before the node is powered on in the
   published Origin 300 procedure (007-4365-002 printed pages 35–37);
   its fitted standby rail is unknown. The display
   shows `L1 booting` then `L1 running` (the image; the manuals) (established). The
   log gains `L1 booting <version>` (established for the family). Environmental state
   is `Wait Pwr` on every row (established, IRIXnet 2737).
2. If auto-power is on, the countdown starts (about 85 seconds on the Origin 350
   samples) and ends in a power-up, inferred to be `* power up`, unless aborted; the
   default is off (established under [node power-off and
   power-on](#node-power-off-and-power-on)).
3. The operator's terminal, at 38,400 baud, 8 data bits, no parity, one stop bit,
   hardware flow control, not adjustable (007-4365-002 printed page 35; an SGI
   engineer in 2002: "As far as I know you can't change the baud rate on the L1",
   `slrna7jllc.2b2e.ivanr@omen.melbourne.sgi.com`) (established). The operator presses
   Enter and the prompt `001c01-L1>` appears; the L1 is in L1 mode and accepts
   commands now (007-4365-002 printed page 35) (established). Whether the L1 prints a
   banner or prompt on its own at boot: unknown (no capture).
4. `power up` (or `* power up` for a cabled pair, or the button). Pre-power checks
   run; on failure the `ERROR: SerNum:…` line appears and nothing else happens
   (established). On success the display shows `Powering Up` then `Powered Up`, the
   log gains `power up (COMMAND)`, and the console prints `returning to console mode
   001c01 CPU0, <CTRL_T> to escape to L1` (or `entering …` from the locked prompt),
   after which the L1 is in console mode with the designation `CPU0` (established;
   that this is a reset of a moved selection rather than the untouched default, the
   two wordings' dependence on the prior state, and the escaped state of the IRIXnet
   capture are inferred). About six seconds after power-up
   the L1 resets the node once more (`reset again MIPS`; established for the Origin
   350, inferred here).
5. The PROM's first console line is `Starting PROM Boot process`; the verbose trace
   then prints the link and crossbar lines and the banner `IP35 PROM SGI Version
   6.210` (IRIXnet 2737 lines 1457 to 1499) (established). Between the mode message and
   that line no L1 text appears in any capture (established for the three captures).
   The elapsed time between them is not captured (unknown).
6. Later, the designation the L1 prints after the brick id changes from `CPU0` to
   `console`: on the Fuel, an escape and `env` after the PROM had finished device
   discovery ended with `returning to console mode  001a01 console, <CTRL_T> to
   escape to L1` (`graphica` line 2030) (established for the Fuel). The manuals' rule:
   "If the system has been powered on and either one of the bricks has a request to
   be the system console, then the C-brick attempts to communicate with that brick"
   (007-3938-001 printed page 20) (established). So the PROM, through the console
   requests `0x1002` and `0x1003`, moves the selection to the console subchannel 4
   during its boot (inferred; whether the Origin 300's PROM does it, and when, is
   unknown; settle by the emulated machine's request log).
7. With the debug switches clear the PROM prints its normal lines and the System
   Maintenance Menu (007-4365-002 printed pages 36 to 37) (established).

### Lifecycle unknowns

| Unknown | What would settle it |
|---|---|
| Whether the image honours request `0x2000`, and what reply it sends | settled in the protocol-parity revision: minus 101; the reset is `0x2001` ([Requests](#requests), change 2) |
| What the L1 does between a first deferred `power down` and the second, and what it sends the running system | a capture of `power down` typed while IRIX runs on hardware (a possible physical test on the user's Origin 350, not requested here); or an image reader |
| Whether processor output arriving during L1 mode is buffered, discarded or shown | escape to the prompt during the PROM memory test on hardware and watch what follows the returning message |
| The mode after `power down` from the escaped state, and whether `WARNING: power appears off, console unavailable` is printed at entry or per byte | a capture of that sequence |
| Whether the L1 prints anything on its console at its own boot, and the exact console text during `reboot_l1` | a serial-console capture spanning an L1 reboot |
| Whether fitted IP45 NVRAM retains debug switches after mains loss, and whether environmental thresholds and console selection survive `reboot_l1` | a revision-matched NVRAM component record and permitted capture of `debug`, `env`, `select` and request `0x000e` before and after a controller restart and mains cycle; the selected firmware's primary/backup restore alone does not establish retention time or other settings |
| Whether a guest's `0x1007` registration survives `reboot_l1` | settled in the protocol-parity revision: yes when the subchannel is 4 or more, no for 0 to 3 ([L1-initiated packets](#l1-initiated-packets), change 9); `syscom` output before and after would confirm it on hardware |
| What the display's line 2 shows while the PROM and IRIX run, and whether either writes the display | the emulated machine's request log for `0x1004` and `0x1005`; an `l1cmd display` capture under IRIX |
| Whether the Origin 300's PROM sends `0x1002` or `0x1003`, and when the designation changes from `CPU0` to `console` | the emulated machine's request log |
| The time between `power up` and `Starting PROM Boot process` | a timestamped capture |
| Which `date` set format and time-zone form the 1.18.5 firmware accepts | a capture, or the 6.5.30 firmware's `help date` text (the image says a UNIX zone string) |
| Whether a fault in the deferred power-down interval, a button press during the auto-power countdown, and a reset button press while off do anything visible | captures |
| What `L1 rebooted` (an image text of unknown destination, display or log) records, where it goes, and how it differs from `L1 booting <version>` | a `log` and `display` capture after a firmware-fault restart, or an image reader |

## Interrupts

The independent UART review of 2026-09-05 clears a standalone active-high
interrupt output. Its public evidence is National Semiconductor's June 1995
PC16550D datasheet, printed pages 11 and 13–18, retained at Library root
`origami-docs`, path
`library/cache/misc/www.scs.stanford.edu_10wi-cs140_pintos_specs_pc16550d.pdf`.
This is a bounded UART implementation, with the existing synchronous transport
and polled IIR behavior retained.

IER bit 0 enables received-data interrupts. Any byte waiting in the processor
receive queue selects IIR cause `0x04`, ahead of transmitter-empty. IIR reads
leave received-data pending. Reading the last queued byte through RBR clears
that cause. Disabling its IER bit suppresses the interrupt while preserving
queued data and the LSR data-ready indication.

IER bit 1 enables transmitter-holding-register-empty (THRE) interrupts. Changing
that bit from zero to one while empty creates a pending occurrence. Rewriting
one while it is already one does not create another. IIR reports `0x02` when
THRE is selected; that read acknowledges the occurrence without immediately
reasserting it from an unchanged empty state. Reading IIR while received-data
has priority does not acknowledge THRE. Writing THR clears the old occurrence.
Once the synchronous parser accepts that byte, it creates one new empty
occurrence if IER bit 1 is enabled. This acceptance cadence is an emulator
choice: no baud-derived delay or observable nonempty LSR interval is promised.
Clearing IER bit 1 suppresses THRE; a subsequent enable while empty rearms it.

The output is high while either supported enabled cause remains and low after
the final cause is serviced or disabled. Reset clears IER and THRE occurrence
state and deasserts the output. Migration preserves the occurrence state and
queued data, then recomputes the output without inventing a new occurrence.
DLAB accesses change divisor latches, not IER or THR interrupt state.

With IER zero or no pending enabled cause, IIR remains `0x01`. The model reports
only the low IIR cause codes even after FCR writes, preserving the existing
polled contract. It does not claim FIFO-identification bits, character timing,
receive timeouts, line errors or modem-change interrupts. Enabling unsupported
causes must not fabricate an event.

**The output now reaches Bedrock ("hw/mips/sgi/sn1: deliver the L1 UART
interrupt to the CPU", QEMU PR #665).** sMDK reached its own `smdk>`
prompt (the local-window fix above) but never accepted typed
input: a live register read found Line Status Register bit 0 and Interrupt
Status Register `0x04` both latched correctly with the guest's receive
interrupt enabled, yet nothing ever asserted the CPU's hardware interrupt
line. Reading every reference to this device's IRQ output in
`hw/mips/sgi/sn1-node.c` and `hw/misc/sgi-bedrock.c` found it created
(`qdev_init_gpio_out_named`) and never connected. The public SN1 header
names pending-bank 0 bit 4 for UART (established;
[bedrock-processor-interface.md](bedrock-processor-interface.md#interrupts)),
and the sibling Origin 2000 Hub uses the same bit for its own serial port and
node microcontroller (`hub-processor-interface.md`), corroborating the bit
assignment across the family without adding a new source. The output is now
connected to a new "uart-irq" input on the `sgi-bedrock` composite device,
broadcast to both PI0 and PI1's pending bit 4 the same way that device
already broadcasts local-block, network-interface and crossbar errors to
both interfaces. **Which processor-interface destination real silicon uses,
and whether it broadcasts to both interfaces or targets one, is still not
established by any public source** -- the broadcast is an emulator
approximation carried by structural precedent in this fork's own code, not
a confirmed hardware fact. The dormant Linux history `7570df54ef8c`
receive-interrupt routine remains intent evidence only, not observed
stock-PROM wiring, and does not settle the destination question either. A
discriminating qtest (`sn1/address-map/l1-uart-irq-reaches-cpu` in the QEMU
repository's `tests/qtest/sn1-address-map-test.c`) drives a byte through the
operator console chardev, the same path sMDK's own input takes, and checks
pending bit 4 on both PI0 and PI1, the CPU's Cause register while masked and
unmasked, and the line dropping once the queue drains.

UART qtests must cover IER-zero polling, receive assertion through final-byte
consumption, THRE acknowledgement without spontaneous reassertion, rearm after
an accepted THR byte or enable transition, receive priority over THRE, disable,
DLAB isolation, reset, and migration of acknowledged and pending occurrences.
These observe the standalone output directly; the processor-interface
connection above has its own qtest instead, in the QEMU repository.

## DMA

None.

## Current unsupported controller subsystems

The current QEMU L1 packet responder does not run the ColdFire firmware or
model its environmental M-Bus peers, JTAG scan master, identity EEPROM
producer, flash management, L2-facing USB device port, RS-422 peer links or
the operator commands beyond the reviewed paths. These are part of the full
controller-model assignment. Each hardware-visible behavior needs a
revision-matched contract, independent expectation and qtest before it is
implemented. An absent board peer or unsupported command remains observable;
the node-facing responder cannot stand in for a successful physical exchange.

## Behavioral notes

**Which component index holds memory module n was a source conflict, now read from the
firmware.** The SN1 port uses 3 plus n. OpenBSD's Origin 300 arm (its default, shared
with the Origin 350) uses 5 plus half n, plus 4 when n is odd; its Fuel arm 4 plus n;
its C-brick arm 3 plus half n plus 4 when odd (`l1.c` lines 996 to 1015). The printed
IP45 dump lists node, IO-8, then modules 0, 2, 1, 3, which fits an interleaved order but
does not fix the base. The firmware's IP45 board module registers the records as 0
node board, 1 IO-8, 2 memory module 0, 3 module 2, 4 module 1, 5 module 3, and indexes 6
and above answer minus 104 (established for the order and for the identity component
mapper; the [Requests](#requests) notes). OpenBSD's Origin 300
arm therefore addresses indexes this firmware does not serve as memory modules; keep
the map as a property so the model can answer that consumer as configured, and settle
the mapper question by probing components 0 to 15 on real hardware.

**What the PROM sends is partly observed.** The L1 has a debugging switch for PROM
requests (007-3938-006 printed page 102), and the verbose traces show two answers
the PROM gets from it: the module identity (`Bedrock Rev: 2, Module: 1 (001c01) from
Sys Ctlr`) and the attached brick type above. The Origin 300 owners' captures also
show which L1 firmware answered: 1.18.5 built 2002-12-18 on two machines, and `L1
1.48.1 (Image B), Built 01/22/2007 [Fuel/PE/O300 1MB image]` on a third, so the
Origin 300's L1 runs the composite file's 1 MB image; and a 2005 owner who flashed an
image the L1 did not recognise saw `UNKNOWN BRICK TYPE!!` on the display (Usenet
`1132778527.644765.40810@g14g2000cwa.googlegroups.com`). Once the stock PROM runs on
the emulated Bedrock, log every request and extend the table above. The PROM's request
sequence as read from its payload is under [the stock IP35 PROM](#the-stock-ip35-prom);
the log would confirm it.

**The source of the real attached-I/O identity reply is unmeasured.** The
SN1 port asks for the I/O brick's module identity at PCI attach; OpenBSD uses the
local address on every subtype but the C-brick and never asks on the type-4 address.
The SN1 port cannot act on an error code (it sees only the low byte), so an error
reply there would leave its PCI attach storing a module id computed from
uninitialised data (`sn1/pcibr.c` line 3601; `pci_bus_cvlink.c` lines 704 to 708),
while a success reply with rack 0 and bay 0 is rejected cleanly by its module lookup
(`l1_command.c` lines 1152 to 1172). The PROM does ask on that address: a verbose
trace from a real Origin 300 prints `hubii_link_good: 4-brick attached to module
001c01` before its banner, where a Tezro prints `9-brick` and an Origin 350
`8-brick`. `4` is the later public SGI header's IP45 brick-type code (`0x34`).
The transcript does not retain the request destination or reply fields, so it
does not prove that the displayed digit came from a type-4 reply rather than
the local identity request. The earlier type-4-reply reading was an inference
from three transcripts and the header. The `8` and `9` digits are
not established the same way: the header checked (`refs/linux-ip35`
`arch/ia64/include/asm/sn/l1.h`) does not name them, and a later public header has not
been checked, so what the L1 actually returns for those chassis types is unknown.
Whether the digits are the attached I/O brick's own letter is also open. The SN family
closure record (`docs/research/sn-family-closure-20260913.md`) argues they are not,
reading an Origin 350 and a Tezro both naming their I/O brick "IXbrick" in this
datasheet's own IRIX naming table above, while printing different digits, as evidence
that the digit is instead the reporting node's own stored chassis type, `8` for an
Origin 350 and `9` for a Tezro, the same role `4` plays for an Origin 300. That table
already maps four different letters (`7 8 9 Q`) to the one name "IXbrick", so two
machines sharing a brick name while printing different digits is what the table
predicts either way, and does not by itself distinguish the two readings. The same
checked header that gives `4` for an IP45 also gives `6` for "IP53 & ROUTER"
(`L1_BRICKTYPE_TWISTER`, `0x36`); the closure record's own family table makes both the
Origin 350 and the Tezro IP53 boards, so the reporting-chassis-type reading predicts
the same digit for both and does not explain the observed `8` versus `9`. Neither
reading is settled: a post-2005 public L1 header naming `8` and `9`, or a verbose trace
read alongside a KLCONFIG dump on each machine, would settle it. The previous revisions had the
model answer code 0 with the module's own rack and bay and type `0x34` on the type-4
address. The selected L1 1.44.0 image has no local type-4 receiver: its IP45
branch forwards through software peer slot 9 under the handle conditions in
[addressing](#addressing), while its local `0x0003` can return `4`. The PROM
reading establishes that the stock PROM sends `0x0003` to the type-4 address
for this line. The visible real capture used L1 1.18.5, whose relevant route
and reply were not captured; neither firmware reading proves what supplied
the real line. Both readings are recorded in change 14 of the
[contract changes](#contract-changes-in-this-revision), and the [addressing](#addressing)
rules now route the type-4 form instead of serving it.

**The command vocabulary drifted.** The 2001 appendix in the Origin 300 NUMAlink guide
and the 2002 revision of the controller guide name the Bedrock-side port "bedrock"
and "junkbus" and use `ctc` for the peer; the 2006 revision is Altix-era. Model the
2001 to 2002 vocabulary.

## Open questions

- What the stock PROM prints on a direct-cabled two-chassis Origin 300 at
  power-on, and by what rule one L1 becomes master without an L2 (the
  [two-chassis pair](#two-chassis-pair) draft records what is known). The
  router-joined pairing's known banner ("Found 3 objects (2 hubs, 1
  routers); Global master is /hw/rack/001/bay/01") may or may not describe
  the direct-cabled pairing; no posted direct-pair PROM discovery banner has
  been found. A reference composition of a direct-cabled pair assigns no
  PROM banner text, no L1 master/slave protocol detail beyond the closure
  record's cited facts, and no header name for the L1's chassis-type
  character. Settled by a posted two-chassis power-on console; 108-0241-001
  System Controllers (not online); a post-2005 public L1 header.

## qtest checklist

Two-chassis pair, proposed with the [draft above](#two-chassis-pair):

- Two controllers linked as peers, racks 1 and bays 1 and 2: a rack-and-bay
  request from node 0's UART to type 0 rack 1 bay 2 answers bay 2; a type-3
  request answers the same; with the link removed both get the minus 1, detail
  8 router error reply.
- `* power down` on the first controller powers both nodes down; `* power up`
  on either powers both up. `power down` alone leaves the peer on; this
  bullet encodes an inference (no source says a plain command stays local),
  not an established behaviour.
- `ctc version` on the first controller prints the peer's id line and its
  version line; with no peer it keeps today's unsupported text.
- With either controller's network mode set to USB, the peer forms behave as
  unlinked.

- Line status reads bits 5 and 6 set and bit 0 clear at reset; modem status reads bit 4 set; bit 0 sets when the model queues a byte for the processor and clears after the data read.
- Line control retains `0x03` after the divisor sequence; interrupt identification reads `0x01`.
- Writing the display request frame byte by byte yields exactly the ten-byte success response frame, and the display property reads "OpenBSD/sgi ".
- The rack-bay-type request is answered both with and without the count byte, with rack 1 and bay 1 as the model's configured values and the type character `0x34` (`4`), the stored brick type of an IP45 (change 1 under [contract changes](#contract-changes-in-this-revision)); the reply's count byte is 2 in both cases, not an echo of the request's. The previous bullet's type C is withdrawn.
- The same request on the attached-I/O address (type 4) is not answered by the general task; the type-4 bullet below replaces the previous success test, and the scoped contract review (citation: `docs/reviews/l1-eeprom-and-attached-io-contract-review.md`) records the reasoning that produced it.
- Check the local/local and concrete/concrete destination forms against nonzero configured rack and bay values: each is local and answered. The mixed forms and the reserved-bit and foreign-controller cases are in the protocol-parity bullets below; the previous silent-drop expectations are withdrawn.
- Command execution succeeds only on type 0 task 3. A general request succeeds on type 0 task 6 and returns minus 101 on task 3 only where task 3 examines the code, which it does not: a general code with one text argument on task 3 runs the text as a command. Tasks without a handler produce no reply; type 4 is routed, not served (the bullets below).
- Every frame the model emits begins with exactly one `0x7e` and ends with exactly one, with no flag between frames: two requests answered back to back yield two frames whose bytes contain exactly four flags.
- An unconfigured EEPROM probe returns minus 104 with no fields. A test-owned synthetic file reports its exact size, and chunk requests reconstruct its bytes, including short final chunks and requested lengths smaller than the remainder.
- A local task-6 `0x0004` request with no arguments returns code 0 and exactly
  one text field when a test-owned serial fixture is configured. The test compares
  that field byte for byte with the fixture and assigns it no product identity or
  required value. With no configured serial it returns minus 104 with no fields.
  A reply with an integer field, no field or an extra field is not an acceptable
  PROM reply even when its code is 0.
- Synthetic board and SPD files prove that board bytes are preserved and only SPD bytes 0 through 63 affect device checksum validation. A changed byte inside that range returns minus 107. A changed byte after it does not. Fixtures contain no SGI record bytes or installed identity.
- The 115-byte boundary test is restated in the protocol-parity bullets below (change 4): a request for 116 bytes succeeds and 117 is the firmware's drop boundary.
- A frame with a corrupted check sequence gets no reply; the next good frame does.
- A display text containing `0x7e` and `0x7d` arrives unescaped in the property.
- An unknown request code on task 6 answers minus 101; too few arguments answers minus 100 and extra arguments are accepted: established from the firmware's dispatcher (change 6), no longer a chosen behaviour.
- A console event frame in console mode reaches the console character device and nothing comes back on the data register; a character typed on the console device yields a subchannel-4 request frame on the data register.
- From console mode, Ctrl-T on the console device produces the escape message and the prompt; `version` produces a version line of the documented shape followed by the re-entering-console-mode message with no further input; a Ctrl-D typed after that is forwarded to the guest as a subchannel-4 request byte, not consumed. After `l1`, the prompt persists until Ctrl-D, which produces the entering-console-mode message.
- The "* pwr d" frame answers success and requests a machine power-off.
- Request `0x000e` on subchannel 16 with the count byte, `7e 10 0f ff f0 06 00 0e 00 f9 72 7e`, answers exactly `7e 30 00 00 00 00 01 00 00 00 00 00 30 98 7e` at the default position: code 0, count 1, one integer 0. The same request without the count byte, `7e 10 0f ff f0 06 00 0e e6 b9 7e`, gets the identical reply. Frames computed from the framing rules as the other vectors are.
- With `node-position` set to 2 the same request answers `7e 30 00 00 00 00 01 00 00 00 00 02 22 bb 7e`; a fieldless success or a zero-filled field fails.
- A `0x000e` request carrying two integer arguments, `7e 10 0f ff f0 06 00 0e 02 00 00 00 00 01 00 00 00 00 02 33 52 7e`, or one text argument, `7e 10 0f ff f0 06 00 0e 01 01 78 00 f1 6d 7e`, gets the same success reply as the bare request, never minus 100 or minus 105.
- Console select `0x1002` with integer `0x40`, `7e 10 0f ff f0 06 10 02 01 00 00 00 00 40 4b ce 7e`, answers the ten-byte fieldless success frame and moves the position to 2, so `0x000e` then answers the position-2 frame above; `0x1002` with integer 4, `7e 10 0f ff f0 06 10 02 01 00 00 00 00 04 6b ca 7e`, keeps it at 2. Starting from position 0, `0x1002` with 4 leaves `0x000e` at 0.
- The port field migrates: after migration the destination answers the value set before it. A device reset returns the value to the `node-position` property. A `0x1002` without its argument answers minus 100 and leaves the field unchanged.
- A `node-position` outside 0 to 3 is rejected at realize. A `0x1002` whose argument is text, `7e 10 0f ff f0 06 10 02 01 01 78 00 71 58 7e`, answers the ten-byte minus 105 frame `7e 30 ff ff ff 97 00 71 92 7e` and leaves the field unchanged, so `0x000e` still reports the position set before it. Frames computed from the framing rules as the other vectors are.
- On the attached-I/O address (type 4) request `0x000e`, like every code there, is routed and not served: the type-4 bullet below applies (change 14); the previous minus 101 expectation is withdrawn.
- Fresh device: L1 mode; the prompt `001c01-L1>` after a carriage return; `display` reads line 1 as the brick id with a spinner and line 2 in the twelve-character padded form the hardware printed, `line 2: "L1 running  "`; `env` reports every row `Wait Pwr`; `power check` prints `power appears off`.
- `power up` from locked L1 mode: a prompt line, then `entering console mode  001c01 CPU0, <CTRL_T> to escape to L1`, then console mode; the node is powered and released; line 2 reads `Powered Up`; `log` gains `power up (COMMAND)`.
- `power up` from the escaped state prints `returning to console mode  001c01 CPU0, <CTRL_T> to escape to L1` instead. The `entering` or `returning` word by prior state and the doubled space before the id are the model's reading of three captures, declared as inferred under [node power-off and power-on](#node-power-off-and-power-on).
- `power up` while on prints `Power is already on` and changes nothing (the text exists; the behaviour is the model's reading of it).
- A configured serial-number failure makes `power up` print `001c01 ERROR: SerNum:No assigned System Serial Number. See log for details.` and leave the node off (only if the model carries serial numbers; otherwise out of scope).
- The re-entering-console-mode message of the Ctrl-T bullet above is, immediately after a power-up, the firmware's `returning to console mode  001c01 CPU0, <CTRL_T> to escape to L1`, not the manuals' `re-entering` form; its designation follows the current selection.
- `power down` with the node on and no guest running: line 2 `Powered Down`, `env` rows `Wait Pwr`, `log` gains `power down (COMMAND)`, and every setting (rack, slot, debug switches, date, the log itself) reads back unchanged; the L1 still answers commands. That the first `power down` removes power at once is a model choice: the image's two-step texts and the deferred interval with a guest running are unknown.
- The guest's `* pwr d` through the command interpreter: reply code 0, then the same powered-off state; the L1 remains and its console still answers Ctrl-T and commands.
- Console mode with the node off: operator bytes are not queued for the next power-up (whichever refusal text the model prints is an approximation and must be declared as one).
- `reset` while on: the node resets and restarts at its reset vector, the L1's mode and settings do not change, the reply queue toward the processor starts empty after the reset (a model choice: in-flight handling across a node reset is unknown and a Bedrock question), and `log` gains `reset (COMMAND)`.
- Whether a reset or power action stops every processor before it fires cannot be a register-only qtest: `-accel qtest` never runs guest code. `tests/functional/mips64/test_sn1_l1_reset.py` pins the independently reviewed processor-stop model choice recorded under [modeled power requests](#modeled-power-requests-0x2005-and-0x2006) for both guest and host-console requests, with real guest instructions. This is not a physical processor-stop timing claim.
- `softreset` and `nmi`: the corresponding signal, the prompt, and line 2 `Sft rst done` or `NMI done`.
- `reboot_l1` with the node on: the node keeps running with power applied; the L1 returns in L1 mode with line 2 `L1 running`; a `brick slot` change made before the reboot now shows in the prompt and in `brick`; the log persists and gains `L1 booting <version>`; any request accepted before the reboot gets no reply.
- Request `0x1002` with subchannel 4 changes the designation in the next mode message from `CPU0` to `console`; with 1, to `CPU1`; `select` reports the same (model choice consistent with the consumer's meaning and the `0x000e` reading).
- Request `0x1003` from the only node changes nothing visible.
- Request `0x1007` registers an event subchannel; the L1 sends any event it generates on that subchannel and none before the registration (the model's event set is its own choice; none is required by this datasheet).
- Request `0x2000` answers minus 101 and resets nothing; the reset request is `0x2001` (the protocol-parity bullets below, change 2).
- `date` set and read back survives `power down`, `power up` and `reboot_l1`.
- `display 2 hello` overwrites line 2; the next `power up` overwrites it with `Powering Up` then `Powered Up`.
- Protocol-parity bullets of this revision (the firmware readings under [Requests](#requests), [addressing](#addressing) and [receive tolerance](#receive-tolerance); frames are given as payload bytes where the readings give them, and the check bytes follow from the framing rules):
- Bits 11:5 of a destination do not matter: `0x0003` sent to `0x0ffff206` and to `0x0ffff006` answers the same reply; a request to the module's own concrete rack and bay (`0x00041006` for rack 1, bay 1) answers as the local form does.
- A mixed form (`0x0ffc1006`, rack `0x3ff` with concrete bay 1, or `0x0007f006`, concrete rack 1 with bay `0x3f`), another rack or bay, and controller types 1, 2 and 7 to 15 each answer the router error reply on the request's subchannel: header `0x20` or-ed with the subchannel, payload `ff ff ff ff 00 00 00 08`, no count byte; no state changes and the next local request is answered.
- A request on the attached-I/O address `0x4ffff006`, and on types 3 and 5, is never answered by the general task: the model answers the router error reply above or stays silent, one of the two declared as its choice for the idle chassis until a capture settles the [recorded unknown](#addressing); either way no `0x0003` success comes back on that address and no state changes.
- A local request to task 0, to task 25 or above, to task 4 or to task 1 gets no reply; a local request to task 5 (`0x0ffff005`) with one integer argument `0x41` produces on the data register a two-byte request-type frame, header `0x00` (subchannel 0 from the destination's bits 9:5) then `0x41`, and no reply; a request with no argument echoes whatever byte the stale field holds; a request to task 15 is served, not dropped.
- `0x2000` answers the ten-byte minus 101 frame; `0x2001` resets the node and answers the ten-byte fieldless success (a reply the reset may cut off, so a test reads it before the reset takes effect or accepts its absence as the SN1 helper does); `0x2005` powers the node off and answers the ten-byte fieldless success; the log gains `reset (OS)` and `power down (OS)` respectively.
- `0x2005` on a powered node queues success before lowering `node-power`, logs
  `power down (OS)` and requests SHUTDOWN. The existing `sn1/l1/general-actions`
  case at QEMU commit `2b17c7ad9bee9eb96b3f198b2825d1fb225775c6` covers that path.
  The `sn1/l1/power-requests-off` case at QEMU commit
  `b5df1c2b612744150cea2ab1801419779ca0adc2` repeats the request while already off
  and requires success without another transition, log entry or SHUTDOWN request.
- `0x2006` on a powered node queues success, lowers and raises `node-power`, logs
  `power down (OS)` then `power up (OS)`, produces RESET without SHUTDOWN and resumes
  VM execution. Starting off skips the down transition and log, then powers up,
  produces RESET and resumes. The dedicated case runs without `-no-shutdown` so an
  accidental process shutdown fails the test. These are the bounded model choices
  recorded under [modeled power requests](#modeled-power-requests-0x2005-and-0x2006)
  and tested by `sn1/l1/general-actions`, `sn1/l1/power-cycle` and
  `sn1/l1/power-requests-off` at QEMU commit
  `b5df1c2b612744150cea2ab1801419779ca0adc2`.
- `0x1004` and `0x1005` each write line 2 and leave line 1 as the brick identity; a display request whose argument is an integer answers minus 102 and changes neither line; a text of fewer than twelve characters reads back space-padded to twelve.
- EEPROM `0x0006`: component 6 answers minus 104; area 5 answers minus 105; an offset equal to the record length answers minus 105; a component within the six-entry table with no file answers minus 104 even when the area or offset would also be out of range; a request carrying five integer arguments is served as if it carried four; a request carrying three answers minus 100.
- EEPROM oversize chunk: with at least 118 bytes remaining, a request for 116 bytes succeeds with 116 bytes in a frame of 128 payload bytes; a request for 117 answers a success frame whose count byte is 2 followed by the integer alone (the firmware's behaviour) or the model's declared minus 105; with 100 bytes remaining a request for 127 succeeds with all 100.
- The command interpreter's reply to `* pwr d` is a status word (0 when the command ran) followed by zero or more text fields, one per output line, and any request code with one text argument on task 3 runs the text.
- A local task-3 request carrying plain `debug` or accepted `debug 269` has the selected unmodified-path prediction code zero, count one, one tag-`0x01` switch-word line. Malformed `debug nope` and `debug 12junk` predict code `-1200`, count one, one generic command-not-found text field, and no setting change. A focused qtest checks the exact frame and then reads task-6 `0x0001` independently to prove malformed commands did not write the word. Independently calculated subchannel-16 expected replies are `7e 30 00 00 00 00 01 01 64 65 62 75 67 20 73 77 69 74 63 68 65 73 20 73 65 74 20 74 6f 20 30 78 30 31 30 64 0a 00 15 5b 7e` for accepted `debug 269` and `7e 30 ff ff fb 50 01 01 45 52 52 4f 52 3a 20 63 6f 6d 6d 61 6e 64 20 6e 6f 74 20 66 6f 75 6e 64 2e 0a 00 42 ac 7e` for malformed `debug nope`. These are static-path predictions, not controller captures. This models the reviewed prefilter branch as an approximation while installed response filters and fitted acceptance remain unmeasured; a revision-matched raw exchange would discriminate the approximation.
- A wildcard `* debug` must not silently become an unqualified local `debug`: the selected route depends on connected module records. Until the model has a reviewed connected-module list, keep this `debug` wildcard route explicitly unsupported and observable, with a qtest proving it does not change the local switch word. The model's fixed subchannel-16 fallback frames for `* debug 0` and `ctc debug 0` are, respectively, `7e 30 ff ff ff ff 01 01 49 4e 46 4f 3a 20 77 69 6c 64 63 61 72 64 20 64 65 62 75 67 20 69 73 20 75 6e 73 75 70 70 6f 72 74 65 64 20 77 69 74 68 6f 75 74 20 63 6f 6e 6e 65 63 74 65 64 20 6d 6f 64 75 6c 65 73 2e 0a 00 f4 6e 7e` and `7e 30 ff ff ff ff 01 01 49 4e 46 4f 3a 20 70 65 65 72 20 64 65 62 75 67 20 69 73 20 75 6e 73 75 70 70 6f 72 74 65 64 2e 0a 00 f6 65 7e`. Their FCS residuals independently calculate to `0xf0b8`; a qtest checks the full code, count, tag, text and check bytes plus unchanged task-6 `0x0001` state. They test a declared approximation, not fitted-controller output. A later route model tests zero targets and one test-owned local target against the conditional `0/0`, `0/2` and `-1/1` selected-path predictions before claiming those outputs. Peer forwarding stays unsupported until its contract is reviewed.
- `0x1007` with integer 23 answers the ten-byte fieldless success and survives `reboot_l1` (`syscom` reports `system event subchannel 23` after the restart); `0x1007` with 2 does not survive (`system events are disabled` after the restart); `0x100a` answers the ten-byte fieldless success and clears it. Any event the model generates goes on the registered subchannel with header `0x40` or-ed with it, source `0x00041004` for rack 1, bay 1, a four-byte code, count 2, one integer and one text of the form `\r\n001c01 ATTN: <text>\r\n`; before registration none is sent.
- `0x100a` and `0x100c` each answer the ten-byte fieldless success frame and change nothing visible on the data register otherwise.
- A request to task 5 with the node powered off answers a minus-1 reply carrying the power-off warning text `WARNING: power on 001c01 appears off!` (rack 1, bay 1).
- Receive tolerance: two flags in a row, then a good frame, answer that frame; a bad check sequence on a frame of three bytes or more, then a good frame sharing the closing flag, answer the good frame; an escape followed by an escape decodes the next byte exclusive-or `0x20`; frames of one or two bytes are dropped as short frames, and three or four bytes of a request header by their check sequence, without a reply; a well-checked header alone is a request with no destination and draws no reply; a well-checked four-byte console-output frame (header `0x44`, one character, check) reaches the console and retargets input to subchannel 4, the IP35 PROM's keystroke-echo, line-break and padding form (`sn1/l1/console-short-frames`); bytes before the first flag are discarded; a payload beyond 256 bytes is dropped and the next flagged frame is answered.
- `l1dbg promreq on` then `0x0003` on subchannel 16 from the local address makes `log` show `L1REQ: 00041205 - 0003` (rack 1, bay 1); a command-interpreter request adds no line.
- Console output arriving as a `0x40` frame on subchannel 1 makes the next operator keystroke leave as a request-type frame on subchannel 1, and `select` reports the change; with `l1dbg console on` the L1 prints `INFO: console subchannel changed: <name>`.
- Four choices the lifecycle tests depend on that no source fixes, each to be declared by the model rather than asserted as hardware behaviour: the spinner's cadence on line 1; what, if anything, the console shows during `reboot_l1`; the log's capacity and its behaviour when full; and the exact bytes of the prompt line printed before the entering message after `power up` from the locked state (the capture shows the prompt on its own line before the entering text).
