/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Origin 200/2000 ("SN0", IP27) physical address map.
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
 * Widget 0 and the crossbar's eight device ports are mapped here. Widget 1 is
 * the Hub, whose window is node-relative in a way a widget-indexed space cannot
 * express, so it keeps its own mapping.
 *
 * **Widget 0's small window has to decode, and this said the opposite until a
 * guest proved otherwise.** A defect is recorded against that window and
 * firmware redirects widget 0 onto the last big window because of it — which
 * led to leaving the small window unmapped so that anything reaching for it
 * would say so loudly. Linux reaches for it, deliberately, with the address
 * macro whose name says raw, and took a bus error on a machine built to that
 * reasoning.
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
 * What answers at widget 0, and what it says it is.
 *
 * On a full crossbar this is the Crossbow's own register block, reporting the
 * part number and manufacturer of a Crossbow — both of which are zero, which is
 * counter-intuitive and is what a real chip reports. An Origin 200 has a
 * cut-down crossbar that reports the I/O bridge's identity here instead, and
 * that is not a detail: both guests read this register first and branch on it.
 * Seeing the bridge's part number, each concludes the machine has a limited
 * topology and probes widget 8 alone; seeing a crossbar's, each runs a full
 * crossbar probe, consults the configuration graph for a probe order and
 * reaches the same bridge by a different route, printing different things on
 * the way. So this is the machine's identity rather than a device default,
 * which is why it is set here.
 *
 * The revision was a choice this file made until it was sourced: a captured
 * OpenBSD 7.9 GENERIC-IP27 boot on a real Origin 200 of this configuration
 * announces revision 3 at both widget 0 and widget 8. It is a value the guest
 * prints and also one it acts on — below revision four it treats the bridge as
 * having a byte-swapping defect and avoids that widget's PCI I/O space
 * altogether — so the earlier guess of 1 took the same branch by luck while
 * printing a line no capture would match.
 */
#define SN0_XBOW_WIDGET          0
#define SN0_WIDGET0_PART         0xc002
#define SN0_WIDGET0_MANUFACTURER 0x036
#define SN0_WIDGET0_REVISION     3
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
#define SN0_FIRMWARE_WIDGET0_ENTRY  (SN0_HUB_IO_BWIN_WIDGET(SN0_XBOW_WIDGET) | \
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
 * The bring-up serial port, an NS16550-compatible interface on the Hub's junk
 * bus. Eight standard 16550 registers inside the Hub's own register window, one
 * register per eight bytes rather than one per byte, which is the alignment the
 * junk bus wants. The public Linux header arch/mips/include/asm/sn/sn0/hubmd.h
 * names the sixteen-register block these sit in, MD_UREG1_0 through
 * MD_UREG1_15, and gives each offset; this interface takes the upper eight.
 *
 * A shipped Origin 200 has no such part — SGI's PROM documentation calls it a
 * manufacturing and bring-up fitting — so the machine leaves it out unless it
 * is asked for. Fitting it is not a free addition either: real firmware tries
 * this port first of all its console candidates and stops at the first that
 * answers, so a machine that answers here gets its console here and never
 * reaches the two candidates a real machine uses.
 */
#define SN0_JUNK_UART_OFFSET    0x2200c0  /* Hub-local, MD_UREG1_8 */
#define SN0_JUNK_UART_REGSHIFT  3         /* one register per eight bytes */

/*
 * The port's reference oscillator. SGI's driver forms its divisor latch from
 * 12 MHz and the standard sixteen samples per bit, and asks for 9600 baud.
 *
 * QEMU's 16550 wants that already divided by sixteen, which is not what the
 * argument's name suggests: it computes the line rate as the value it was
 * given divided by the divisor latch alone. Handing it the oscillator would
 * make it report a rate sixteen times too fast to whatever is behind the
 * character device, which costs nothing against a pipe and produces garbage
 * against a real serial port -- the one case a bring-up instrument is for.
 * Every other machine in the tree that drives a 16550 from a named crystal
 * writes the division at the call for the same reason.
 */
#define SN0_JUNK_UART_CLOCK_HZ  12000000
#define SN0_JUNK_UART_SAMPLES   16

/*
 * The boot flash: one megabyte, and the guest sees it two ways. The processor's
 * reset vector reaches it as node-local physical 0x1fc00000 through the
 * compatibility segments, and the Hub special space reaches the same bytes at
 * the node's base plus 0x10000000. Both views are real hardware behaviour
 * rather than a software convention, so they are one region and a second
 * mapping onto it.
 *
 * The Hub special view is not the same size as the flash. SGI names it LBOOT
 * and gives it 256 MB, because that view's decode is the Hub's chip-select for
 * a boot PROM socket, not a copy of the PROM's own size — real firmware
 * reaches its own code through this view as a matter of course, at whatever
 * offset the code lives at beyond the flash's real 1 MB, and expects a real
 * chip's address decode to have ignored the extra bits and answered with the
 * same byte a smaller, in-range offset would. A view sized to the flash alone
 * answers only the first 1 MB of that 256 MB window and reaches nothing for
 * the rest, which is not what the window is for.
 */
#define SN0_FLASH_SIZE      (1 * MiB)
#define SN0_FLASH_LOCAL     0x1fc00000  /* node offset, reset vector view */
#define SN0_FLASH_HUB       0x10000000  /* node offset within Hub special */
#define SN0_LBOOT_SIZE      (256 * MiB) /* the Hub special view's real size */

/*
 * The directory and error-correction back doors, reached through Hub special
 * space above the boot flash windows. Real firmware's own memory and
 * directory diagnostics write a pattern through here and read it back to
 * test for stuck bits; a range that answers with anything other than what
 * was last written there fails that test on a healthy machine. We model no
 * coherence directory and no error correction, so the addresses carry no
 * meaning beyond that round trip, and each window is sized to the full
 * offset range the guest's own address macros can produce within it.
 */
#define SN0_BACKDOOR_ECC_HUB  0x80000000  /* node offset within Hub special */
#define SN0_BACKDOOR_ECC_SIZE (1 * GiB)
#define SN0_BACKDOOR_DIR_HUB  0xc0000000  /* node offset within Hub special */
#define SN0_BACKDOOR_DIR_SIZE (1 * GiB)

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

/* The firmware configuration block, at flash offset 0x60. */
#define SN0_CONFIG_OFFSET   0x60
#define SN0_CONFIG_MAGIC    0x69703237636f6e66ULL  /* "ip27conf" */

#endif /* HW_MIPS_SN0_H */
