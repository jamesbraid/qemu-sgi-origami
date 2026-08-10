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
#define SN0_MEM_BANKS       8

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
 * The boot flash: one megabyte, and the guest sees it two ways. The processor's
 * reset vector reaches it as node-local physical 0x1fc00000 through the
 * compatibility segments, and the Hub special space reaches the same bytes at
 * the node's base plus 0x10000000. Both views are real hardware behaviour
 * rather than a software convention, so they are one region and an alias.
 */
#define SN0_FLASH_SIZE      (1 * MiB)
#define SN0_FLASH_LOCAL     0x1fc00000  /* node offset, reset vector view */
#define SN0_FLASH_HUB       0x10000000  /* node offset within Hub special */

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
#define SN0_HW_DIR_CONFIG_GRAPH 1
#define SN0_HW_DIR_GDA          3
#define SN0_CONFIG_GRAPH        0x4000
#define SN0_CONFIG_GRAPH_SIZE   0xc000

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

/* Component types, and the flag that stops a consumer skipping one. */
#define SN0_COMP_PROCESSOR      1
#define SN0_COMP_HUB            2
#define SN0_COMP_MEMORY         3
#define SN0_COMP_BRIDGE         5
#define SN0_COMP_IO_CONTROLLER  6
#define SN0_COMP_MODULE_SERIAL  26
#define SN0_COMP_ENABLED        1

/*
 * The console's serial port, as an offset within the base-I/O widget. A guest
 * splits this address with two different masks and they mean different things,
 * which is the trap: the low twenty bits are the register offset that has to
 * reach hardware, while only the low twelve select which generation of I/O
 * controller it believes it has, and at or above 0x380 it reads a configuration
 * register this machine does not have.
 *
 * So both 0x178 and 0x20178 pass the generation check identically, because they
 * share their low twelve bits — and only one of them is a register. The I/O
 * controller puts its serial block in a window at 0x20000 with port A at 0x178
 * inside it, so 0x20178 is the address of the port's first 16550 register and
 * 0x178 is the discriminator value that identifies it.
 *
 * This machine had the discriminator here. The guest identified its console
 * correctly, drove an address that answers nothing, and took a bus error on a
 * byte store of 0x03 to offset three — setting 8N1 on a port that was not
 * there — with nothing in the failure naming the console.
 */
#define SN0_CONSOLE_PORT_OFFSET 0x20178
#define SN0_CONSOLE_PORT_MASK   0xfffff  /* the part that reaches hardware */
#define SN0_CONSOLE_WIDGET      8
#define SN0_CONSOLE_BAUD        9600
#define SN0_GDA                 0x11000
#define SN0_GDA_SIZE            0x400
#define SN0_GDA_SIGNATURE       0x58464552  /* "XFER" */
#define SN0_GDA_VERSION         2
#define SN0_GDA_NODE_TABLE      0x80
/*
 * The table is 256 bytes of signed sixteen-bit node identifiers, so 128 of
 * them, and the width is the whole point: a guest indexes it with a stride of
 * two and reads each entry with a signed halfword load, then skips the entry if
 * it is negative. Written a byte per entry instead, the first entry reads as
 * 0x00ff, which is positive, so nothing is skipped and the guest goes looking
 * for a node numbered 255.
 */
#define SN0_GDA_NODES           128
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
