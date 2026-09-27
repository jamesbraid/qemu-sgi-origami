/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bus error contract: a read that nothing answers faults, a write
 * posts.
 *
 * Oracle: the R10000 User's Manual version 2.0, the Bus Error exception
 * description. Its cause is exhaustive and every request kind in it is
 * a
 * read: "A Bus Error exception occurs when a processor block read,
 * upgrade,
 * or double/single/partial-word read request receives an external ERR
 * completion response, or a processor double/single/partial-word read
 * request receives an external ACK completion response where the
 * associated
 * external double/single/partial-word data response contains an
 * uncorrectable error." An uncached store leaves the processor as a
 * double/single/partial-word write request, which the System interface
 * chapter shows as one address cycle and one data cycle with no
 * completion
 * response at all: the write is posted, and whatever becomes of it is
 * the
 * memory system's to report, never a precise exception at the store.
 *
 * Why this is worth a ROM of its own: firmware leans on the asymmetry
 * from
 * the other side. The PROM's Bridge self-test deliberately writes to an
 * address nothing implements and expects to keep running, taking the
 * report
 * through the I/O system's error registers; a model that raises a
 * precise
 * bus error at the store hands its handler an exception real hardware
 * never
 * delivers. The load half is pinned beside it so losing the store
 * exception
 * can never quietly widen into losing the bus error itself.
 *
 * For the faulting load, what is pinned is what the manual pins: the
 * DBE
 * code — "Bus error exception (data reference: load or store)", code 7
 * in
 * the Cause register's exception code table — the recorded program
 * counter,
 * and the clear branch-delay bit. BadVAddr is deliberately not pinned:
 * the
 * BadVAddr description sets it for TLB and Address Error exceptions and
 * "otherwise, the architecture leaves this register undefined"; the Bus
 * Error description's servicing text computes the address from the
 * instruction instead.
 *
 * The probed address is the second node's Hub through the node window,
 * on
 * the suite's one-node machine: nothing decodes there, so a read is
 * refused
 * by the machine rather than answered. The foldback ROM leans on the
 * same
 * fact from the other direction, probing that address to detect a
 * one-node
 * machine by its bus error. If a second node ever becomes part of the
 * default suite machine, the load assertions here fail loudly and this
 * ROM
 * must move its hole.
 *
 * The R10000 alone: the posted-write half is that processor's System
 * interface contract. QEMU's R4000 model raises a precise bus error for
 * a
 * failed store, and this ROM asserts that the R10000 model must not.
 */

#include "rom.h"

const char rom_name[] = "buserror";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define ATTR_XTALK      1               /* the attribute that means crosstalk */
#define HUB_NODE        0x01800000ull   /* "the Hub of the node I name" */
#define NODE(n)         ((uint64_t)(n) << 32)

/*
 * A Hub scratch register offset, real on any node that exists — the
 * fault
 * comes from the node being absent, not from the offset being wrong.
 */
#define IO_SCRATCH      0x400150ull

/*
 * Nothing on a one-node machine: the second node's Hub.
 */
#define HOLE \
    XKPHYS(ROM_CACHE_UNCACHED, ATTR_XTALK, \
           NODE(1) + HUB_NODE + IO_SCRATCH)

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/*
 * Run a block with a return address armed, and also report the address
 * of the
 * block's first instruction so a test can say where the exception must
 * be
 * recorded. The label the return address is taken from sits after the
 * block,
 * so a fault anywhere inside it continues at the same place.
 */
#define TRY_AT(first, insns, ...)                                             \
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                    \
                         "lui $26, %%hi(9f)\n\t"                              \
                         "daddiu $26, $26, %%lo(9f)\n\t"                      \
                         "sd $26, 0(%[res])\n\t"                              \
                         "lui %[at], %%hi(8f)\n\t"                            \
                         "daddiu %[at], %[at], %%lo(8f)\n"                    \
                         "8:\t" insns "\n"                                    \
                         "9:\t.set pop"                                       \
                         : [at] "=&r"(first), ##__VA_ARGS__                   \
                         : [res] "r"(&rom_exc.resume),                        \
                           [p] "r"((unsigned long)HOLE)                       \
                         : "$26", "memory")

void rom_main(void)
{
    uint64_t first, junk;

    /*
     * 64-bit kernel addressing on: the hole is reached through the
     * 64-bit
     * unmapped space. Without this the access takes an address error
     * and the
     * ROM would be asserting about the wrong exception entirely.
     */
    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * A doubleword store into the hole is posted and completes: no
     * exception, and the ROM is still running to say so.
     */
    rom_exc_reset();
    TRY_AT(first, "sd $0, 0(%[p])");
    rom_assert_eq(1, 0, rom_exc.count);

    /*
     * A word store is a partial-word write request, posted the same
     * way.
     */
    rom_exc_reset();
    TRY_AT(first, "sw $0, 0(%[p])");
    rom_assert_eq(2, 0, rom_exc.count);

    /*
     * The doubleword load from the very same address is a read request
     * and
     * must still fault: one bus error, through the general vector, the
     * DBE
     * code, the load itself recorded, and the branch-delay bit clear.
     */
    rom_exc_reset();
    TRY_AT(first, "ld %[j], 0(%[p])", [j] "=&r"(junk));
    rom_assert_eq(3, 1, rom_exc.count);
    rom_assert_eq(4, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(5, ROM_EXC_DBE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(6, 0, rom_exc.cause & ROM_CAUSE_BD);
    rom_assert_eq(7, first, rom_exc.epc);

    /*
     * The word load too, so the width the stores used is covered on the
     * read
     * side as well.
     */
    rom_exc_reset();
    TRY_AT(first, "lw %[j], 0(%[p])", [j] "=&r"(junk));
    rom_assert_eq(8, 1, rom_exc.count);
    rom_assert_eq(9, ROM_EXC_DBE, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(10, first, rom_exc.epc);
}
