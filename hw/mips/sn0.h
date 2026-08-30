/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) physical address map and family machine.
 *
 * The first half of this header is the address map every SN0 product shares;
 * the second half is the family machine the products parameterize, shared by
 * the family's translation units (sn0.c, sn0-diag.c, sn0-firmware.c and the
 * product files).
 *
 * The machine presents one flat 40-bit physical address space shared by every
 * node. The node number occupies the top eight of those bits, so a node's
 * memory, registers, crosstalk windows and boot flash all appear at a fixed
 * stride and any node's resources are reached by changing those bits alone.
 * Origin 200 has one node, so every stride collapses onto the same window, but
 * the arithmetic is modeled from the start because making the node count a
 * machine option later is cheap and retrofitting it is not.
 *
 * Above the physical address the processor's uncached attribute picks which of
 * four spaces an uncached reference means. That attribute is a signal beside
 * the address rather than part of it, so the memory system sees it in the two
 * bits above the physical address, and the space that means memory is placed at
 * window zero so that a plain physical address still means memory.
 */

#ifndef HW_MIPS_SN0_H
#define HW_MIPS_SN0_H

#include "cpu.h"
#include "qapi/error.h"
#include "hw/block/flash.h"
#include "hw/core/boards.h"
#include "hw/misc/sgi-hub.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"

/*
 * M mode: the node number is address bits 39:32 and a node owns 4 GB. N mode,
 * with nine-bit node numbers and 2 GB windows, is a configuration no guest we
 * care about selects.
 */
#define SN0_NODE_SHIFT      32
#define SN0_NODE_SIZE       (4 * GiB)
#define SN0_NODE_BASE(nasid) ((hwaddr)(nasid) << SN0_NODE_SHIFT)

/*
 * The four uncached spaces, in the order the processor's uncached attribute
 * numbers them, shifted so that memory lands at zero.
 */
#define SN0_SPACE(n)        ((hwaddr)(n) << MIPS_UNCACHED_WINDOW_SHIFT)
#define SN0_SPACE_MEMORY    SN0_SPACE(0)  /* uncached attribute 3 */
#define SN0_SPACE_HUB       SN0_SPACE(1)  /* uncached attribute 0 */
#define SN0_SPACE_XTALK     SN0_SPACE(2)  /* uncached attribute 1 */
#define SN0_SPACE_MEMSPEC   SN0_SPACE(3)  /* uncached attribute 2 */

/*
 * Node memory fills the bottom of a node's window in up to eight banks of 512
 * MB.
 */
#define SN0_MEM_BANK_SIZE   (512 * MiB)

/*
 * A bank is not one run of memory: it is four slots of 128 MB apiece, and a
 * guest works out where memory sits from the slot rather than from the bank.
 * A bank larger than one slot is spread a quarter to each of its four, so a
 * 256 MB bank appears as four 64 MB pieces on the 128 MB stride with gaps
 * between them; a bank of one slot or less sits wholly in the first.
 *
 * Sourced from both guests. IRIX places a bank at a 512 MB stride and its
 * kernel decodes a faulting address back through the same shift; mainline
 * Linux addresses a slot at 128 MB and indexes its bank-size array by the
 * slot over four, which is the same layout counted in smaller units.
 */
#define SN0_MEM_SLOT_SIZE   (128 * MiB)
#define SN0_MEM_BANK_SLOTS  (SN0_MEM_BANK_SIZE / SN0_MEM_SLOT_SIZE)
#define SN0_MEM_BANKS       8

/*
 * The bottom of Hub-special space is an uncached view of the reading
 * processor's own node memory.  Only the first 256 MiB decode through this
 * local alias; the next 256 MiB are the node-local boot window.
 */
#define SN0_HUB_LOCAL_MEM_SIZE (256 * MiB)

/*
 * The two lines the Hub drives into the processor, named by the Cause register
 * bit each one raises: the first pending bank on hardware interrupt 0, which is
 * Cause bit 10, and the second on hardware interrupt 1, Cause bit 11. Stated as
 * the Cause bit rather than as the processor's interrupt index because that is
 * how every source states it, and the index is derived from it here rather than
 * written out — the two differ by the offset of the pending field in Cause, and
 * writing the index directly is how a model ends up two lines off with nothing
 * to compare against.
 */
#define SN0_HUB_BANK0_CAUSE_BIT 10
#define SN0_HUB_BANK1_CAUSE_BIT 11
#define SN0_HUB_RT_CAUSE_BIT    12

/*
 * Crosstalk address space. A crosstalk address is a widget number and an offset
 * within that widget and nothing in between: the crossbar routes on the
 * widget number and carries the offset through untouched. Modeling it as one
 * container indexed by widget is what stops the two ways of reaching a device
 * drifting apart, because a small window and a big window then become two
 * views of one address rather than two sums that have to agree.
 *
 * How far a widget's space runs is set by how far a big window can reach into
 * it. A translation entry supplies crosstalk address bits 33:29, so 32
 * slices of 512 MB, which is 16 GB.
 *
 * A widget has two such spaces — a translation entry's bit 12 chooses between
 * them — and the redirection uses the **memory** space.
 *
 * The bit has two vocabularies and both are the sources' own: Linux and IRIX
 * call its set value I/O space, OpenBSD's header calls the same value device
 * space. Neither is ours and neither is wrong, so both are named here, because
 * a reader checking this against one header or the other has to recognise it.
 * A third meaning sits nearby and is not this bit at all — the Bridge's own
 * header uses device space for its PCI windows, one chip further out.
 *
 * That the redirection selects memory is sourced from live code rather than
 * inferred: IRIX programs this entry, for this purpose,
 * inside a block compiled for this machine family, naming memory mode with
 * widget zero and offset zero. Across the whole corpus the other value appears
 * in exactly one write, and that one programs a different window on a peer node
 * for error injection.
 *
 * The history is worth keeping because the wrong answer was well argued. The
 * value was first sourced from a routine that really does select the other
 * space — whose caller programs a different window for a different job,
 * peer-Hub
 * discovery — and applied here because both write the same register. A fact
 * about one purpose, carried to another on the strength of the mechanism
 * looking the same.
 *
 * So this models the memory space, and a window asking for the I/O space is
 * reported rather than quietly aimed here.
 *
 * **The small windows are modelled as reaching the same space, and the argument
 * is the redirection rather than anything about where registers live.** The
 * guests' own small-window address helper returns the big window's base when
 * the
 * widget is zero and the small window's otherwise — a transparent substitute,
 * chosen at the call site with nothing else changed. A substitute has to
 * present
 * what it replaces, and the window it substitutes is programmed to memory
 * space.
 * Derived, not observed; there is no capture.
 */
#define SN0_XTALK_WIDGETS       16
#define SN0_XTALK_WIDGET_SHIFT  34
#define SN0_XTALK_WIDGET_SIZE   (1ULL << SN0_XTALK_WIDGET_SHIFT)
#define SN0_XTALK_BASE(widget)  ((hwaddr)(widget) << SN0_XTALK_WIDGET_SHIFT)
#define SN0_XTALK_SIZE          (SN0_XTALK_WIDGETS * SN0_XTALK_WIDGET_SIZE)

/*
 * The sixteen small windows fill the bottom sixteenth of a node's crosstalk
 * window, one per widget, each reaching that widget's first 16 MB.
 *
 * Widget 0 and the eight crosstalk port numbers are mapped here. Widget 1 is
 * the Hub, whose window is node-relative in a way a widget-indexed space cannot
 * express, so it keeps its own mapping.
 *
 * **Widget 0's small window has to decode, and this said the opposite until a
 * guest proved otherwise.** Firmware redirects widget 0 onto the last big
 * window, and a defect against the small window was taken as the reason — which
 * led to leaving that window unmapped so anything reaching for it would say so
 * loudly. Linux reaches for it, deliberately, with the address macro whose name
 * says raw, and took a bus error on a machine built to that reasoning.
 *
 * **What the defect actually is has never been established, and two accounts
 * disagree.** SGI's firmware says it avoids widget 0 because of a problem with
 * error handling there, without saying what the problem is; this tree has
 * elsewhere claimed a Hub defect in the small window. Nobody has shown they are
 * the same thing, and neither is needed to justify what this code does.
 *
 * **The two guests use different paths and the machine owes both.** Linux reads
 * widget 0 through this small window; OpenBSD redirects to big window 6 through
 * the one function both its identification paths call. Satisfying either alone
 * leaves the machine broken for the other guest, and the same register must
 * answer identically through both — they are one device seen through two
 * windows, not two devices.
 */
#define SN0_SMALL_WINDOW_SIZE    (16 * MiB)
#define SN0_SMALL_WINDOW(widget) ((hwaddr)(widget) * SN0_SMALL_WINDOW_SIZE)
#define SN0_XTALK_PORT_FIRST     8
#define SN0_XTALK_PORT_LAST      15

/*
 * The seven big windows fill the rest of the node's crosstalk window, starting
 * one big window above its base. The sixteenth between the small windows and
 * this decodes to nothing. How wide each one is and where each one points are
 * the Hub's translation table's business, not this file's.
 */
#define SN0_BIG_WINDOW_BASE      0x20000000

/*
 * What answers at widget 0, and why there is no identity constant here.
 *
 * A real Origin 200 has no crossbar. The Hub has one crosstalk link with the
 * I/O bridge on the far end, and with no switch in between nothing routes by
 * widget number, so every widget address the Hub emits reaches the same chip.
 * SGI's own firmware says so where it probes that link: a directly connected
 * bridge can be given whichever widget number the firmware chooses, and it
 * chooses 8, avoiding 0 because something about error handling there is a
 * problem. Widget 8 is an agreement between two firmware routines, not a place
 * the bridge sits.
 *
 * So widget 0 reports the bridge's part number and manufacturer because it *is*
 * the bridge, and those values live on the bridge rather than here. Every guest
 * reads this register first and branches on it — a bridge's identity means a
 * restricted topology and a probe of widget 8 alone, a Crossbow's means a full
 * crossbar probe through the configuration graph — so getting it wrong changes
 * what kind of machine a guest thinks it is on.
 *
 * The revision stays here because it is not the chip's to know. It varies
 * between real machines: a captured OpenBSD 7.9 GENERIC-IP27 boot on a real
 * Origin 200 announces revision 3 at both widget 0 and widget 8, and another
 * real machine's own firmware inventory reports revision 4. That reads as a
 * property of the silicon fitted to a unit rather than of the machine type,
 * which is why it is a settable option and why nothing here claims to have
 * settled it. It is a value a guest prints and also one it acts on — below
 * revision four it treats the bridge as having a byte-swapping defect and
 * avoids that widget's PCI I/O space altogether — so an earlier guess of 1 took
 * the same branch by luck while printing a line no capture would match.
 */
#define SN0_WIDGET0              0
#define SN0_WIDGET0_REVISION     4
/* The identification field is four bits wide. */
#define SN0_WIDGET0_REVISION_MAX 0xf

/*
 * The redirection the firmware performs, and the Hub register fields it needs
 * to express it. Widget 0's small window is unreliable through a defect on the
 * Hub's processor-interface side, so firmware aims the last big window at
 * widget 0 instead, and both guests expect to find that done.
 *
 * **Nothing in the entry's contents marks it reserved, and a guest reads those
 * contents as meaning the opposite.** OpenBSD's large-window allocator scans
 * the
 * table for a spare entry and treats one whose widget field is zero as free —
 * which is exactly what the redirect holds. What keeps a guest from claiming it
 * is a hard-coded index exclusion in that allocator, not anything about the
 * value. Nothing on this machine asks for a large window yet, so nothing
 * exercises it; the day something does, that one line is the whole protection.
 *
 * They find the redirection done by the translation table's reset value, which
 * already names widget 0, so the write puts back what is there. That is not a
 * property of our
 * model: the entry resets to zero, and zero is field for field what the
 * redirection writes, so a real machine's firmware is doing the same
 * belt-and-braces write on any machine whose I/O bridge sits behind widget 0.
 * The Hub I/O interface datasheet carries it.
 */
#define SN0_HUB_IO_BWIN_TABLE       0x400160
#define SN0_HUB_IO_BWIN_STRIDE      8
#define SN0_HUB_IO_BWIN_ENTRY(n)    (SN0_HUB_IO_BWIN_TABLE + \
                                     (n) * SN0_HUB_IO_BWIN_STRIDE)
#define SN0_HUB_IO_BWIN_WIDGET(w)   ((uint64_t)(w) << 8)
#define SN0_HUB_IO_BWIN_SLICE(x)    ((uint64_t)(x))

/*
 * Which window carries the redirection, and it is the last of the seven.
 * Sourced from the guest side: Linux redirects widget 0 to a window it names as
 * the entry count minus one, giving the processor-interface defect as the
 * reason.
 *
 * **Software numbers these windows 0 through 6 and SGI's hardware manuals
 * number the same seven 1 through 7.** A manual's "big window 7" is this
 * entry. Anyone cross-checking a number here against SGI documentation is one
 * off.
 */
#define SN0_FIRMWARE_WIDGET0_WINDOW 6

/*
 * The entry SGI's firmware writes, term by term rather than as a total, because
 * every device-address error this project has made was a dropped term in a sum.
 * The slice is written out even though it is zero: a field left to a zeroed
 * register and a field deliberately set to zero are indistinguishable until
 * somebody changes how the register is initialised.
 */
#define SN0_FIRMWARE_WIDGET0_SLICE  0
#define SN0_FIRMWARE_WIDGET0_ENTRY  (SN0_HUB_IO_BWIN_WIDGET(SN0_WIDGET0) | \
                                     SN0_HUB_IO_BWIN_SLICE( \
                                         SN0_FIRMWARE_WIDGET0_SLICE))

/*
 * The widget number SGI's firmware names to disable a window. There is no valid
 * bit, so a window is turned off by pointing it at a widget that is not there,
 * and the firmware's headers name three as the invalid one.
 */
#define SN0_XTALK_INVALID_WIDGET    3

/*
 * The Hub's register window, at both halves of the crossbar's widget 1 small
 * window. The lower half resolves to the reading processor's own node whatever
 * node number the address carries; the upper half names a node.
 */
/*
 * The cached compatibility segment, which reaches node-local physical zero. A
 * pointer handed to a kernel has to be one it can dereference, and this is the
 * segment both guests read low memory through.
 */
#define SN0_COMPAT_CACHED 0xffffffff80000000ull

#define SN0_HUB_LOCAL_WINDOW 0x01000000
#define SN0_HUB_NODE_WINDOW  0x01800000

/*
 * The boot flash: one megabyte, and the guest sees it three ways. The
 * processor's reset vector reaches it as node-local physical 0x1fc00000
 * through the compatibility segments. Hub special space reaches the same
 * bytes through local LBOOT at offset 0x10000000 and through node-explicit
 * RBOOT at offset 0x30000000. All three views are real hardware behaviour
 * rather than a software convention, so they share one flash backing store.
 *
 * Each Hub-special view is larger than the flash. SGI gives both LBOOT and
 * RBOOT 256 MB, because their decode is the Hub's chip-select for a boot PROM
 * socket, not a copy of the PROM's own size. Real firmware reaches its own code
 * through these views and expects a real chip's address decode to ignore the
 * extra bits. A view sized to the flash alone answers only the first 1 MB of
 * its 256 MB window and reaches nothing for the rest.
 */
#define SN0_FLASH_SIZE      (1 * MiB)
#define SN0_FLASH_LOCAL     0x1fc00000  /* node offset, reset vector view */
#define SN0_FLASH_HUB       0x10000000  /* node offset within Hub special */
#define SN0_FLASH_RBOOT     0x30000000  /* explicit node's Hub-special view */
#define SN0_LBOOT_SIZE      (256 * MiB) /* the Hub special view's real size */
#define SN0_RBOOT_SIZE      (256 * MiB) /* explicit-node view's real size */

/*
 * The directory and error-correction back doors, reached through Hub special
 * space above the boot flash windows. Real firmware's own memory and
 * directory diagnostics use these windows for two things at once: testing a
 * location it already trusts is populated for stuck bits, and -- the same
 * read-back, against two masks of different width -- discovering whether a
 * bank exists at all and which directory format it carries. We model no
 * coherence directory and no error correction, so the addresses carry no
 * meaning beyond what that classification needs.
 *
 * A bank the board does not wire has no directory or ECC storage behind its
 * share of these windows on real hardware, so nothing written there may
 * survive being read back: real firmware's probe reads that failure as
 * absence, before it ever touches that bank's ordinary memory. A bank the
 * board does wire has real storage, but only as wide as this machine's own
 * directory format -- SGI's manual states the Origin 200 carries none of
 * the richer, premium format a larger machine has, so a populated entry
 * here must keep only the low sixteen bits of whatever is written to it.
 * Each window is therefore width-limited storage (sn0_backdoor_populated_ops
 * in sn0.c) for however many of the eight banks this configuration actually
 * populates, and a fixed, non-RAM floor for the rest (sn0_backdoor_absent_ops).
 */
#define SN0_BACKDOOR_ECC_HUB  0x80000000  /* node offset within Hub special */
#define SN0_BACKDOOR_ECC_SIZE (1 * GiB)
#define SN0_BACKDOOR_DIR_HUB  0xc0000000  /* node offset within Hub special */
#define SN0_BACKDOOR_DIR_SIZE (1 * GiB)

/*
 * How much of a back-door window one memory bank's directory/ECC entries
 * occupy: an even eight-way split of the 1 GB window, 128 MB per bank in
 * the same bank index order ordinary memory uses. Confirmed rather than
 * chosen -- a back-door entry's address is the physical address it
 * describes, shifted right by two (the window is the node's 4 GB memory
 * space compressed four to one) and masked to the window, so the bank bits
 * are carried straight through and land each bank on a 128 MB slice by
 * construction. The masks and the stride are both named in the public
 * Linux headers for this platform, so this is a shared fact rather than a
 * reading of the vendor's private tree; it also happens to land on the
 * same 128 MB quantum SN0_MEM_SLOT_SIZE already uses for a bank's four
 * reporting slots, worth recording as corroboration between two otherwise
 * unrelated corners of this machine's address map.
 */
#define SN0_BACKDOOR_BANK_STRIDE (SN0_BACKDOOR_ECC_SIZE / SN0_MEM_BANKS)

/*
 * Ten entry points at the start of the flash, eight bytes apart. A guest calls
 * them by address, so each has to be a branch rather than whatever follows.
 */
#define SN0_FLASH_ENTRIES   10
#define SN0_FLASH_ENTRY_STRIDE 8

/*
 * The firmware structures in low memory, at the offsets the guests look for
 * them. Every one of these is fixed by the boot structures datasheet.
 */
#define SN0_PARAM_BLOCK         0x1000
#define SN0_PARAM_SIGNATURE     0x53435241  /* a constant, not "ARCS" */
#define SN0_HW_DIRECTORY        0x2000
#define SN0_HW_DIR_ENTRIES      16
#define SN0_HW_DIR_ENTRY_SIZE   64
#define SN0_HW_DIR_SIGNATURE    0x434d5f53505f5357ULL
#define SN0_HW_DIR_LAUNCH       0
#define SN0_HW_DIR_CONFIG_GRAPH 1
#define SN0_HW_DIR_NMI          2
#define SN0_HW_DIR_GDA          3
#define SN0_HW_DIR_FREEMEM      4
#define SN0_HW_DIR_DEBUG_STACKS 5
#define SN0_HW_DIR_PI_ERROR     6
#define SN0_CONFIG_GRAPH        0x4000
#define SN0_CONFIG_GRAPH_SIZE   0xc000

/*
 * The launch blocks: two per node, one per processor, at a fixed offset and
 * stride. Not on the banner path — they matter when a second processor
 * starts — but the directory advertises them from the start, so they are
 * written from the start. Both blocks carry the same eight-byte signature;
 * every other field is zero until there is a second processor to launch.
 * The directory's size field for this entry is the span of both blocks
 * together — stride times count — because the two blocks fill the region
 * the low-memory map reserves for them exactly.
 */
#define SN0_LAUNCH_BLOCKS       0x2400
#define SN0_LAUNCH_ENTRIES      2
#define SN0_LAUNCH_STRIDE       0x200
#define SN0_LAUNCH_DIR_SIZE     (SN0_LAUNCH_ENTRIES * SN0_LAUNCH_STRIDE)
#define SN0_LAUNCH_SIGNATURE    0xaddbead2addbead3ULL

/*
 * The non-maskable-interrupt handler and its two per-processor blocks. The
 * directory's size field for this entry is one block's size, not the span of
 * both: the guest installs its own handler address and complement into a
 * block at this offset plus its processor's stride, and the directory
 * describes the array element a guest indexes rather than the whole region.
 */
#define SN0_NMI_BLOCKS          0x3000
#define SN0_NMI_ENTRIES         2
#define SN0_NMI_STRIDE          0x40
#define SN0_NMI_DIR_SIZE        SN0_NMI_STRIDE

/*
 * First free memory, the debugger stacks and the processor-interface error
 * spools: directory entries with no content of their own to write, only an
 * offset, size, count and stride for a guest to read. First free memory's
 * own offset is SN0_FIRST_FREE, the constant the memory descriptors share.
 */
#define SN0_HWDIR_FREEMEM_SIZE        ((uint64_t)-1)
#define SN0_HWDIR_DEBUG_STACKS_BASE   0x25000
#define SN0_HWDIR_DEBUG_STACKS_SIZE   0xe000
#define SN0_HWDIR_DEBUG_STACKS_N      2
#define SN0_HWDIR_DEBUG_STACKS_STRIDE 0x7000
#define SN0_HWDIR_PI_ERROR_BASE       0x12000
#define SN0_HWDIR_PI_ERROR_SIZE       0x4000

/*
 * Inside the graph: a header, a linked list of board records and the component
 * records they point at. Every link is a 32-bit offset from the start of node
 * memory, so the whole thing is position-independent within the node.
 */
#define SN0_GRAPH_MAGIC         0xbeedbabeULL
#define SN0_GRAPH_CONSOLE_OFF   0x18    /* where the descriptor is embedded */
#define SN0_GRAPH_BOARDS        0x4100
#define SN0_GRAPH_BOARD_SIZE    0xc0
#define SN0_GRAPH_COMPONENTS    0x4400
#define SN0_GRAPH_COMP_STRIDE   0x80

/* Board types: the high nibble is the class, the low nibble the type in it. */
#define SN0_BOARD_NODE          0x11
#define SN0_BOARD_MIDPLANE      0x41
#define SN0_BOARD_BASE_IO       0x21
#define SN0_BOARD_LOCAL         1       /* record kind */

/*
 * A board's record version has a required minimum: IRIX's board-naming
 * routine treats anything below two as unnamed and substitutes a generic
 * label, changing the device path of everything beneath that board. Real
 * firmware writes two on an ordinary board and three on the node board
 * specifically, for a reason its own source states — a later register
 * addition to the per-node error structure the node board's record carries.
 */
#define SN0_BOARD_VERSION_NODE  3
#define SN0_BOARD_VERSION       2       /* every other board */

/* Component types, and the flag that stops a consumer skipping one. */
#define SN0_COMP_PROCESSOR      1
#define SN0_COMP_HUB            2
#define SN0_COMP_MEMORY         3
#define SN0_COMP_BRIDGE         5
#define SN0_COMP_IO_CONTROLLER  6
#define SN0_COMP_SCSI           11
#define SN0_COMP_MODULE_SERIAL  26
#define SN0_COMP_ENABLED        1
#define SN0_COMP_REV_UNKNOWN    0xff
#define SN0_COMP_REV_HUB        3
#define SN0_COMP_REV_BRIDGE     4
#define SN0_COMP_REV_SCSI       5
#define SN0_COMP_REV_IOC3       1

/*
 * A component record's physical identifier, at offset 0x18: distinct from
 * its virtual identifier, and not one of sn0_component()'s own fields
 * because only a disk controller has a confirmed consumer for it. A guest's
 * boot-device walk compares this against the PCI device number the Bridge's
 * own probe finds, so it is each disk controller's PCI device number —
 * SN0_SCSI_DEVICE0/1, the same constants that place the adapters on the
 * bus, not a value asserted separately that merely happens to agree.
 */
#define SN0_COMP_PHYSICAL_ID    0x18

/*
 * The console's serial port, as an offset within the base-I/O widget. It is a
 * composed address and the composition is spelled out here because writing the
 * total as one number is what went wrong twice.
 *
 * A guest splits this with two masks meaning different things. The low twenty
 * bits are the register offset it drives; the low twelve of those select which
 * generation of I/O controller it believes it has, and at 0x380 or above it
 * reads a configuration register this machine does not have. The bits above
 * twenty are the bus base it adds the offset back onto, so they must be right
 * too, and they are invisible to both checks.
 *
 * Composed, this is widget 8's window, plus the I/O bridge's window for the
 * third device on its bus, plus serial port A inside the controller. A real
 * Origin 200 reports its console at exactly this address.
 *
 * Two earlier values were wrong here and both looked right. 0x178 alone is the
 * generation discriminator and not a register. 0x20178 is the register offset
 * within the controller but lands, unqualified, in the bridge's own PCI
 * configuration window rather than at any device. Both keep 0x178 in the low
 * twelve bits, so both pass the only check a guest announces.
 */
#define SN0_BRIDGE_DEVICE2      0x600000  /* the third device's window */

/*
 * Which PCI devices the base I/O board fills. Two SCSI adapters at devices 0
 * and 1 and the I/O controller at device 2 — the board carries two adapters
 * rather than one, and a real machine's own hardware inventory names both.
 *
 * These positions carry a consequence beyond naming the slots, which is why
 * they are constants rather than literals at the one call site. The ratified
 * Bridge pin for the I/O controller's second interrupt is 3 *because* 3 is the
 * first empty device on real hardware: OpenBSD scans devices upward, stops at
 * the first empty one, and uses that index as the pin. A real board fills 0, 1
 * and 2, so the scan stops at 3, and with the I/O controller now a PCI device
 * at slot 2 alongside the two adapters, this machine's own scan stops there
 * too.
 */
#define SN0_SCSI_DEVICE0        0         /* first SCSI adapter's PCI device */
#define SN0_SCSI_DEVICE1        1         /* second SCSI adapter's PCI device */
#define SN0_SCSI_ADAPTERS       2         /* what the base I/O board carries */
#define SN0_IOC3_DEVICE         2         /* the I/O controller's PCI device */

/*
 * The Bridge pin the board wires the I/O controller's second interrupt to,
 * derived above from the same PCI occupancy as SN0_IOC3_DEVICE rather than
 * asserted separately. It does not follow sgi_bridge_map_irq()'s slot-equals-
 * pin rule for a device's own PCI interrupt, because a real board routes this
 * one directly rather than through that chip-level default — the machine's
 * own wiring is what settles it, which is why the constant lives here and not
 * in the Bridge.
 */
#define SN0_IOC3_SUPERIO_PIN    3         /* its second interrupt's Bridge pin */

/*
 * Where firmware places each PCI device's resources, and which of a slot's two
 * spaces its window is pre-aimed at. No source establishes real ARCS PROM's own
 * addresses as a documented fact — but Linux's own platform code
 * (arch/mips/pci/pci-ip27.c) is a source for what the guest requires them to
 * be, and observing it beats guessing: it declares the PCI bus's claimable
 * resource window as starting at the console widget's own crosstalk base plus
 * the bridge's per-device window base (0x08200000 in a boot transcript, widget
 * 8 shifted into position plus 0x200000), and refuses to enable a device whose
 * BAR falls outside that window even when the BAR is correctly non-zero and
 * assigned — "BAR 0 ... not assigned; can't enable device" was this file's own
 * first attempt at these constants, caught only by booting Linux and reading
 * what it actually did rather than assuming a small offset would do.
 *
 * So each device's PCI address is that widget base plus its own slot's window
 * offset — the same offset sgi_bridge_window_offset() computes and the same
 * one the controller's crosstalk address already uses (SN0_BRIDGE_DEVICE2 for
 * slot 2), restated here because this header does not reach into that file's
 * internals. Memory and I/O space are independent regions in this Bridge, so
 * a memory BAR and an I/O BAR sharing the same numeric offset do not collide.
 *
 * Every device's window is pre-aimed, matching its own BAR, the same way real
 * ARCS PROM aims all of them as part of POST — not only the controller's,
 * which carries the console and so has to be reachable before a guest has
 * run any code of its own. An earlier version of this firmware aimed only
 * the controller's window, on the theory that a guest's own driver aims the
 * other two when it attaches. That is true of OpenBSD's qlw, which manages
 * the Bridge's per-device window base itself (its xbridge driver's "devio"
 * machinery) as part of ordinary PCI resource allocation — but it is not
 * true of Linux's driver pair for this hardware. `pci-xtalk-bridge.c` sets
 * `pci_set_flags(PCI_PROBE_ONLY)` unconditionally, so its generic PCI core
 * never reassigns a BAR — it trusts firmware to have already placed the
 * value there, the same way it trusts firmware for every other machine that
 * flag exists for. And `bridge_probe()`'s per-slot loop writes only the
 * byte-swap-direction bit into each device's Bridge register; it never
 * touches the field that selects which PCI address the window is aimed at.
 * So on Linux the two adapters' windows are left aimed at their reset value
 * — PCI address zero, both memory-space-selected, both stacked on top of
 * each other — regardless of what their BARs say. A guest that enumerates
 * the bus, reads back the identity each adapter's config space reports (a
 * separate Bridge mechanism, unaffected by this), and then touches a real
 * register through the mapped BAR finds nothing at the address the window
 * actually reaches. Aiming every device's window here, not only the
 * console's, is what real firmware does and is what both guests need.
 *
 * None of these addresses is zero, which is worth stating because it very
 * nearly was: generic PCI code refuses to map a BAR that resolves to address
 * zero at all, treating it as still unassigned, unless a machine opts in with
 * pci_allow_0_address — which real hardware does not have and this machine
 * does not set. A zero here silently leaves the device unreachable with no
 * error, exactly the failure this firmware exists to prevent, and it is what
 * this file's very first version chose for the controller before a real boot
 * caught it.
 */
#define SN0_PCI_WIDGET_BASE     ((uint32_t)SN0_CONSOLE_WIDGET << 24)
#define SN0_PCI_SCSI0_WINDOW    0x00200000  /* slot 0's own 2 MB window */
#define SN0_PCI_SCSI1_WINDOW    0x00400000  /* slot 1's own 2 MB window */
#define SN0_PCI_IOC3_MEM        (SN0_PCI_WIDGET_BASE + SN0_BRIDGE_DEVICE2)
#define SN0_PCI_SCSI0_MEM       (SN0_PCI_WIDGET_BASE + SN0_PCI_SCSI0_WINDOW)
#define SN0_PCI_SCSI1_MEM       (SN0_PCI_WIDGET_BASE + SN0_PCI_SCSI1_WINDOW)
#define SN0_PCI_SCSI0_IO        SN0_PCI_SCSI0_MEM
#define SN0_PCI_SCSI1_IO        SN0_PCI_SCSI1_MEM
#define SN0_IOC3_SERIAL_A       0x020178  /* serial port A in the controller */
#define SN0_CONSOLE_PORT_OFFSET (SN0_BRIDGE_DEVICE2 + SN0_IOC3_SERIAL_A)
#define SN0_CONSOLE_PORT_MASK   0xfffff   /* the part the guest drives */
#define SN0_CONSOLE_WIDGET      8
#define SN0_CONSOLE_BAUD        9600

/*
 * The machine's -serial line assignment (SN0_IOC3_SERIAL_A_CHARDEV and the
 * rest) lives in hw/misc/sgi-hub.h, the header both this machine and the Hub
 * include. Two of the four backends -- the system controller's AUX port and
 * the bring-up UART -- are wired by the Hub rather than here, so the scheme
 * has to sit somewhere both translation units see it, all four indices in one
 * place where a clash between them is visible.
 */
#define SN0_GDA                 0x11000
#define SN0_GDA_SIZE            0x400
#define SN0_GDA_SIGNATURE       0x58464552  /* "XFER" */
#define SN0_GDA_VERSION         2
#define SN0_GDA_NODE_TABLE      0x80
/*
 * The table is 128 bytes of signed sixteen-bit node identifiers, so 64 of them:
 * the count a kernel for this family is built with, the 128-entry variant being
 * the top-of-line machine with its own firmware image. The width is the other
 * half and is the part that bit us. A guest indexes it with a stride of
 * two and reads each entry with a signed halfword load, then skips the entry if
 * it is negative. Written a byte per entry instead, the first entry reads as
 * 0x00ff, which is positive, so nothing is skipped and the guest goes looking
 * for a node numbered 255.
 */
#define SN0_GDA_NODES           64
#define SN0_GDA_NODE_ABSENT     0xffff
#define SN0_FIRST_FREE          0x19000

/*
 * The memory descriptor list. A guest holds the pointer it is handed and reads
 * through it, so the descriptors live in memory rather than being synthesised
 * per call. Below the parameter block, in the page the real list describes as
 * holding nothing a guest reads.
 */
#define SN0_MEM_LIST            0x900

/*
 * The firmware call vector, in low memory where the parameter block points at
 * it: thirty-five entries of eight bytes, entry n at 8n. Every entry is the
 * address of a stub in the flash, because a guest reads five of them out as
 * pointers and will jump to one the moment anything goes wrong, so none may be
 * null even when the routine behind it does nothing.
 */
#define SN0_ARCS_VECTOR         0x2800
#define SN0_ARCS_ENTRIES        35
#define SN0_ARCS_GET_CHILD      10      /* entry at offset 0x50 */
#define SN0_ARCS_GET_SYSTEM_ID  17      /* entry at offset 0x88 */
#define SN0_ARCS_GET_MEMORY     18      /* entry at offset 0x90 */
#define SN0_ARCS_WRITE          27      /* entry at offset 0xd8 */
#define SN0_ARCS_GET_ENV        30      /* entry at offset 0xf0 */

/*
 * The SGI-private call vector: seventeen more entries of eight bytes each,
 * at the offset the parameter block's private-vector field points at.
 * IRIX and the SGI standalone shell need it; the open-source guests we boot
 * never look at it. Its trap-page slots and dispatcher op numbers continue
 * where the firmware call vector's own thirty-five leave off, in the same
 * trap page, because the mechanism is identical and the two vectors never
 * overlap in address.
 */
#define SN0_ARCS_PRIVATE_VECTOR   (SN0_ARCS_VECTOR + 0x400)
#define SN0_ARCS_PRIVATE_ENTRIES  17
#define SN0_ARCS_PRIVATE_BASE_OP  SN0_ARCS_ENTRIES
/* Capability level, private-vector entry at offset 0x48 -> index 9. */
#define SN0_ARCS_CAPABILITY_OP    (SN0_ARCS_PRIVATE_BASE_OP + 9)
/* Fixed-PROM launch_slave entry, implemented by the synthesized firmware. */
#define SN0_FIRMWARE_LAUNCH_OP    (SN0_ARCS_PRIVATE_BASE_OP + \
                                   SN0_ARCS_PRIVATE_ENTRIES)
/*
 * Level three: the firmware can load a relocatable object. The SGI
 * standalone shell shipped on install media is a relocatable object with no
 * program headers and no entry point, so a firmware answering less than
 * three cannot load it. Not on the banner path; written down so the number
 * is right before anything asks for it.
 */
#define SN0_ARCS_CAPABILITY_LEVEL 3

/*
 * The stubs those entries point at, and the region their one memory access
 * lands in. A stub is four instructions: form the address, load from it,
 * return. The load is what crosses into the firmware implementation, and which
 * slot it reads names the routine.
 */
#define SN0_ARCS_STUB_SIZE      16
#define SN0_ARCS_TRAP_LOCAL     0x1fd00000  /* node offset, past the flash */
#define SN0_ARCS_TRAP_SIZE      0x1000

/*
 * The firmware's own instructions live in a region of the machine's rather than
 * in the flash, because the flash belongs to whatever image was put there: a
 * real firmware, or a bare-metal program under test, either of which is
 * entitled to every byte of it. Only the reset entry is shared, and the address
 * map fixes that anyway.
 */
#define SN0_FW_TEXT_LOCAL       0x1fd01000
#define SN0_FW_TEXT_SIZE        0x1000
#define SN0_FW_BOOTLOADER       0x000       /* offset within that region */
#define SN0_ARCS_STUBS          0x100
#define SN0_FW_LAUNCH_STUB      0x500
#define SN0_FW_SECONDARY_BOOT   0x520

/* launch_slave is fixed entry seven in the IP27 PROM jump table. */
#define SN0_FLASH_LAUNCHSLAVE   (7 * SN0_FLASH_ENTRY_STRIDE)

/* The firmware configuration block, at flash offset 0x60. */
#define SN0_CONFIG_OFFSET   0x60
#define SN0_CONFIG_MAGIC    0x69703237636f6e66ULL  /* "ip27conf" */

/*
 * The width of the processor mode-bits word a board drives into its R10000
 * at reset. **The processor does not accept these from software.** The
 * R10000 user manual gives bits 24:0 of the configuration register as taken
 * from the mode bit settings during a reset sequence, driven in over the
 * system interface; bits above that are hardwired, including both primary
 * cache sizes. So the word is something the machine hands the processor at
 * reset, not something a guest writes — and a model that let a write stick
 * would pass the firmware's mode-bits check while describing a processor
 * that does not exist. The word itself encodes a product's clock ratios and
 * lives in SN0MachineClass.
 */
#define SN0_PROCESSOR_MODE_MASK 0x01ffffff  /* bits 24:0, latched at reset */

/*
 * The AMD 29F080's erase-sector size: the one piece of chip geometry both
 * the chip model (sn0.c) and the flash image builder (sn0-firmware.c) need.
 * The command addresses stay private to the chip model, and the environment
 * log's placement to the image builder.
 */
#define SN0_FLASH_SECTOR_SIZE (64 * KiB)

/*
 * The SN0 family machine, shared by the family's translation units. One
 * chassis is one node: two processor slots beside one Hub, and the family
 * ceiling is the largest configuration any registered product reaches.
 */
#define SN0_CPUS_PER_NODE 2
#define SN0_UNCACHED_SPACES 4

typedef struct SN0BackdoorState {
    uint8_t *data;
    uint32_t data_size;
    uint64_t bank_offset[SN0_MEM_BANKS];
    uint64_t bank_span[SN0_MEM_BANKS];
    unsigned int populated_banks;
} SN0BackdoorState;

typedef struct SN0PromDataState {
    MIPSCPU *cpu;
    hwaddr node_base;
    MemoryRegion data;
    MemoryRegion filter;
    bool present;           /* the filter was installed (real PROM boot) */
    bool leaving_private;   /* the overlay has been (or is being) disabled */
} SN0PromDataState;

typedef struct SN0NodeState {
    SysBusDevice parent_obj;

    /*
     * Properties: what the machine tells a node about itself. The node's
     * memory is a window into the machine's backing RAM starting at
     * ram-start, cut into the populated bank sizes banks-mb carries.
     * local-memory-base is where a NASID-less memory-space address lands:
     * normally the node's own window, but node 0's when synthesized
     * firmware hands a directly loaded kernel control, because Linux
     * disables PI_CALIAS_SIZE before launching remote processors and their
     * unqualified kernel-text addresses then name node zero. promdata-filter
     * installs the PROM's cache-as-RAM overlay, which only a real PROM boot
     * enters.
     */
    uint32_t nasid;
    uint32_t cpu_base;
    uint32_t nr_cpus;
    char *cpu_type;
    MemoryRegion *ram;
    uint64_t ram_start;
    uint64_t local_memory_base;
    bool promdata_filter;
    uint32_t nr_banks_mb;
    uint16_t *banks_mb;

    Clock *cpuclk;
    MIPSCPU *cpu[SN0_CPUS_PER_NODE];
    MemoryRegion cpu_sysmem[SN0_CPUS_PER_NODE];
    MemoryRegion sysmem_alias[SN0_CPUS_PER_NODE];
    MemoryRegion local_space[SN0_CPUS_PER_NODE][SN0_UNCACHED_SPACES];
    MemoryRegion hub_private[SN0_CPUS_PER_NODE][2];
    SN0PromDataState promdata[SN0_CPUS_PER_NODE];
    SGIHubState hub;

    /*
     * The Hub's own view of memory, for its block transfer engines: the
     * global map underneath and this node's memory over the node-less
     * addresses, the same shape as each processor's local view.
     */
    MemoryRegion hub_memory;
    MemoryRegion hub_memory_system;
    MemoryRegion hub_memory_local;

    PFlashCFI02 *flash;
    MemoryRegion flash_reset_view;
    MemoryRegion flash_hub_view;
    MemoryRegion flash_rboot_view;
    MemoryRegion flash_hub_fast;
    MemoryRegion hub_regs_node_view;
    MemoryRegion arcs_trap;
    MemoryRegion fw_text;
    MemoryRegion backdoor_ecc;
    MemoryRegion backdoor_ecc_absent;
    MemoryRegion backdoor_dir;
    MemoryRegion backdoor_dir_absent;
    SN0BackdoorState backdoor_ecc_state;
    SN0BackdoorState backdoor_dir_state;

    MemoryRegion xtalk;
    MemoryRegion prom_pi_probe;
    SGIBridgeState *prom_pi_probe_bridge;
    MemoryRegion small_window[SN0_XTALK_WIDGETS];
    MemoryRegion mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    MemoryRegion hub_local_mem_slot[SN0_MEM_BANKS][SN0_MEM_BANK_SLOTS];
    uint16_t bank_mb[SN0_MEM_BANKS];
    unsigned int populated_banks;

    uint64_t mem_addr;
    unsigned int mem_count;
} SN0NodeState;

#define TYPE_SN0_MACHINE MACHINE_TYPE_NAME("sn0")

/*
 * The flight recorder behind the diagnostic-recording option, implemented in
 * sn0-diag.c. The event and read-cache records are private to that file; the
 * machine holds only pointers, and the per-node sink context each Hub's
 * recording callback receives.
 */
typedef struct SN0DiagnosticEvent SN0DiagnosticEvent;
typedef struct SN0DiagnosticReadCacheEntry SN0DiagnosticReadCacheEntry;

typedef struct SN0DiagnosticSinkContext {
    void *machine;
    uint32_t node;
} SN0DiagnosticSinkContext;

struct SN0MachineState {
    MachineState parent_obj;

    /*
     * The installed chassis, sized by what the command line asked for and
     * bounded by the product's ceiling. Each node is a QOM child named
     * node0, node1, ... and each chassis's I/O bridge a child named bridge,
     * bridge1, ...
     */
    unsigned int num_nodes;
    SN0NodeState **nodes;
    SGIBridgeState **io_bridge;

    /* The product reference clock every node's processors derive from. */
    Clock *cpu_refclk;

    /* Host diagnostics only; not guest-visible and not migrated. */
    bool diagnostic_recording;
    uint64_t diagnostic_query_sequence;
    SN0DiagnosticEvent *diagnostic_events;
    uint32_t diagnostic_event_head;
    uint32_t diagnostic_event_count;
    uint64_t diagnostic_event_next_sequence;
    uint64_t diagnostic_event_dropped;
    SN0DiagnosticReadCacheEntry *diagnostic_read_cache;
    uint16_t *diagnostic_read_cache_next;
    DeviceDiagnosticSink *diagnostic_sink;
    SN0DiagnosticSinkContext *diagnostic_sink_context;

    /*
     * Widget 0, which on this machine is the I/O bridge seen a second time
     * rather than a device of its own. Held here because a MemoryRegion has
     * one parent, so the second view of the bridge's widget space has to be an
     * alias with its own storage.
     */
    MemoryRegion *widget0;

    /*
     * The same I/O bridge seen again at every other non-zero crosstalk widget.
     * A real Origin 200 has no crossbar: the Hub's one link ends at the bridge,
     * and with nothing to route by widget number every widget address the Hub
     * emits reaches the same chip, so firmware may stamp a device pointer with
     * any widget it picks and still reach the bridge. Each view needs its own
     * alias because a MemoryRegion has one parent. Indexed by widget number;
     * only the net-new ones (2 through 7 and 9 through 15) are used — widget 0
     * has its own alias above, widget 1 is the Hub, and widget 8 is the
     * bridge's own mapping.
     */
    MemoryRegion (*widget_alias)[SN0_XTALK_WIDGETS];

    /*
     * Whether the I/O bridge is fitted. **On by default**, because a real
     * Origin 200 always has one and every device on this machine hangs off
     * it — both disk adapters, the base I/O controller with its serial ports,
     * its ethernet and its keyboard controller, and the clock on its byte
     * bus. Turning it off leaves a board that never existed.
     *
     * It was off by default once, and the reason is worth keeping because it
     * says what would have to change to flip it back: the bridge was not
     * merely absent from the default machine, it was worse than absent, since
     * OpenBSD used to enumerate further with it fitted and then stop in its
     * debugger. That is no longer the trade — measured rather than assumed,
     * with the byte-bus clock modelled OpenBSD 7.9 GENERIC-IP27 reaches the
     * same prompt either way and attaches twenty devices against eight, a
     * strict superset with no bus error in the run.
     *
     * The option stays because that bring-up aid is still useful for
     * isolating a fault to the fabric or to what sits behind it. **It does not
     * gate widget 0**, which is aliased unconditionally: turning the option off
     * must not stop widget 0 answering, or a bring-up aid becomes a machine
     * OpenBSD panics on.
     */
    bool io_bridge_present;

    /*
     * Whether the junk-bus UART is fitted; see the class notes. Off by
     * default, because a shipped Origin 200 has no such part and because
     * real firmware treats it as the highest-priority console candidate —
     * fitting it unasked would hide the two real candidates behind it.
     */
    bool junk_uart_present;

    /*
     * Whether the firmware's boot-progress indicator is shown; see the class
     * notes. On by default, because it is the only thing this machine prints
     * while the firmware is still deciding which console to talk to.
     */
    bool debug_leds;

    /*
     * Whether this machine presents firmware of its own; see the class notes.
     */
    bool synthesized_firmware;

    /* Physical switch mask returned through each node's ELSC. */
    uint8_t elsc_dip_switches;

    /* Where the firmware's console output goes. */
    Chardev *console;

    /*
     * The PCI devices on the I/O bridge's bus whose resources firmware
     * assigns after reset (task #43): the base I/O controller and the two
     * SCSI adapters. Kept here because reset is not realize — a device's own
     * reset clears the state a realize-time assignment would have set, so
     * the assignment has to run again every time the machine resets, and it
     * needs these to still have somewhere to reach the devices from.
     */
    PCIDevice **pci_ioc3;
    PCIDevice *(*pci_scsi)[SN0_SCSI_ADAPTERS];

    /*
     * Candidate values: hardware facts this machine asserts without having
     * observed them. They are settable so that two runs can differ in one value
     * and nothing else — a candidate that takes a rebuild to change makes every
     * comparison a comparison of two emulators as well as two values, and a
     * difference then cannot be attributed to either.
     *
     * A value belongs here when it is derived rather than captured. Moving one
     * in is how we say we are not sure; moving one out, to a constant, is how
     * we
     * say an observation settled it.
     */
    uint32_t widget0_revision;

};

typedef struct SN0MachineState SN0MachineState;
typedef struct SN0MachineClass SN0MachineClass;

DECLARE_OBJ_CHECKERS(SN0MachineState, SN0MachineClass,
                     SN0_MACHINE, TYPE_SN0_MACHINE)

/*
 * What a concrete SN0 product's class_init fills in when it registers its
 * machine type; the construction code the products share reads it from
 * here rather than from per-product constants, the same shape PowerNV and
 * Aspeed use for their families. Two kinds of fact live here and they are
 * kept apart: what makes the product the product -- its chassis ceiling,
 * the ip27conf words its firmware expects, how its board fits memory, what
 * I/O it carries -- and the processor module the model installs in it,
 * which is not product identity at all: the same R10000 and R12000 modules
 * at the same range of clocks fit every SN0 product.
 */
struct SN0MachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/
    /* How many chassis the product couples over CrayLink. */
    unsigned int max_nodes;

    /*
     * The node board's own clocks, advertised through the flash
     * configuration block: the Hub clock, which the configuration graph's
     * Hub record repeats, and the real-time clock's tick.
     */
    uint64_t hub_hz;
    uint64_t rtc_hz;

    /*
     * The product-specific words in the flash configuration block: the
     * machine-type word (0 is Origin 2000, 1 is Origin 200) and the two
     * flash timings SGI's own flasher carries per product.
     */
    uint32_t fw_machine_type;
    uint32_t fprom_cyc;
    uint32_t fprom_wr;

    /*
     * The processor module installed in the product's reference population
     * -- not product identity: the whole family takes the same modules.
     * The processor clock is what the CPU model's counter divisor derives
     * the on-chip counter from, and it and the secondary-cache clock appear
     * in the flash configuration block and each configuration-graph
     * processor record. The mode bits are the word this module's board
     * drives into the R10000 at reset and the same word the configuration
     * block advertises: real firmware's first check compares the two and
     * reports "an error copying mode bits" if they disagree, so one place
     * holds both. Selecting another module by machine option waits on the
     * mode word's encoding being documented; until then the population is
     * the recorded one.
     */
    struct {
        uint64_t clock_hz;
        uint32_t scache_clock_mhz;
        uint32_t mode_bits;
    } cpu_module;

    /*
     * Fit a node's memory into the banks the product's board provides.
     * False when the size cannot be expressed in that board's DIMMs.
     */
    bool (*node_memory_layout)(uint64_t ram_size,
                               uint16_t bank_mb[SN0_MEM_BANKS],
                               unsigned int *populated_banks);

    /*
     * Build the product's I/O topology on one node, after the node's Hub and
     * crosstalk container exist: the I/O bridge or crossbar, the devices on
     * it, and the widget decode the product's wiring implies.
     */
    void (*populate_io)(SN0MachineState *s, unsigned int node_index);
};

void sn0_diagnostic_attach(SN0MachineState *s, unsigned int node_index);
void sn0_diagnostic_machine_reset(SN0MachineState *s);
void sn0_diagnostic_finalize(SN0MachineState *s);

/*
 * The board-driven processor mode bits, reapplied on every processor reset
 * (sn0.c). The synthesized firmware's launch path applies them too when it
 * restarts a parked processor.
 */
void sn0_mode_bits_reset(void *opaque);

/*
 * The synthesized firmware and direct kernel boot, implemented in
 * sn0-firmware.c. The two reset handlers redo what real firmware redoes on
 * every reset: the widget-0 window redirection and the PCI resource
 * assignment.
 */
void sn0_firmware_init(MachineState *machine);
void sn0_firmware_reset(void *opaque);
void sn0_assign_pci_resources(void *opaque);

#endif /* HW_MIPS_SN0_H */
