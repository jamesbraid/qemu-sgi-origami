/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test TLB conflict behavior and verify that a replacement translation
 * is used instead of a stale cached mapping.
 *
 * Source: R10000 User's Manual, version 2.0.
 */

#include "rom.h"

const char rom_name[] = "tlbconflict";

/*
 * Physical pages nothing else in a ROM uses: the harness keeps its data
 * below
 * physical 0x200000 and its stack just under that.
 */
#define PHYS_A      0x00300000ull
#define PHYS_A_ODD  0x00301000ull
#define PHYS_B      0x00302000ull
#define PHYS_B_ODD  0x00303000ull
#define PHYS_C      0x00304000ull
#define PHYS_C_ODD  0x00305000ull
#define PHYS_BIG    0x02000000ull

/*
 * Virtual pages in the user segment, which is translated from kernel
 * mode.
 */
#define VIRT_X    0x10000000ull
#define VIRT_Y    0x12000000ull
#define VIRT_Z    0x14000000ull
#define VIRT_W    0x16000000ull
#define VIRT_R    0x18000000ull
#define VIRT_BIG  0x20000000ull
/*
 * A page inside the 16 Mbyte pair mapped at VIRT_BIG.
 */
#define VIRT_IN   (VIRT_BIG + 0x4000)

#define ASID_MINE  7
#define ASID_OTHER 8

#define MAP_RW (ROM_ENTRYLO_V | ROM_ENTRYLO_D | ROM_ENTRYLO_G)

/*
 * Interrupt mask bit 7 of the Status register, the timer's mask.
 */
#define ROM_ST_IM7 (1u << 15)

/*
 * The EJTAG debug register's debug-mode bit, which register 23 would
 * reach
 * on a processor that has one.
 */
#define EJTAG_DM (1u << 30)

/*
 * The same arming distance and poll bound the timer ROM uses.
 */
#define TIMER_TARGET (1u << 22)
#define POLL_LIMIT   50000000

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

/*
 * The uncached window onto a physical address, for planting a value
 * where a
 * translated access should later find it.
 */
static volatile uint32_t *phys(uint64_t paddr)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    return (volatile uint32_t *)(unsigned long)(0xffffffffa0000000ull + paddr);
}

static uint32_t load32(uint64_t vaddr)
{
    uint32_t v;

    __asm__ __volatile__("lw %0, 0(%1)" : "=r"(v) : "r"(vaddr) : "memory");
    return v;
}

static void tlb_write_index(int index, uint64_t entryhi, uint64_t lo0,
                            uint64_t lo1, uint32_t mask)
{
    rom_set_pagemask(mask);
    rom_set_entryhi(entryhi);
    rom_set_entrylo0(lo0);
    rom_set_entrylo1(lo1);
    rom_set_index(index);
    __asm__ __volatile__("tlbwi" : : : "memory");
}

static void tlb_read_index(int index)
{
    rom_set_index(index);
    __asm__ __volatile__("tlbr" : : : "memory");
}

static uint32_t tlb_probe(uint64_t entryhi)
{
    rom_set_entryhi(entryhi);
    __asm__ __volatile__("tlbp" : : : "memory");
    return rom_get_index();
}

static uint32_t status_ts(void)
{
    return rom_get_status() & ROM_ST_TS;
}

static void clear_ts(void)
{
    rom_set_status(rom_get_status() & ~ROM_ST_TS);
}

/*
 * Unmask by writing the Status register, then wait, bounded, for the
 * delivery; the resume address is armed past the wait. The pattern is
 * the
 * intdeliver ROM's.
 */
#define UNMASK_AND_WAIT(unmasked)                                             \
    do {                                                                      \
        uint64_t bound__ = 1000000;                                           \
        uint32_t st__ = (unmasked);                                           \
        __asm__ __volatile__(                                                 \
            ".set push\n\t.set noreorder\n\t"                                 \
            "lui $26, %%hi(9f)\n\t"                                           \
            "daddiu $26, $26, %%lo(9f)\n\t"                                   \
            "sd $26, 0(%[res])\n\t"                                           \
            "mtc0 %[st], $12\n"                                               \
            "1:\tbeqz %[n], 9f\n\t"                                           \
            " daddiu %[n], %[n], -1\n\t"                                      \
            "b 1b\n\t"                                                        \
            " nop\n"                                                          \
            "9:\t.set pop"                                                    \
            : [n] "+r"(bound__)                                               \
            : [res] "r"(&rom_exc.resume), [st] "r"(st__)                      \
            : "$26", "memory");                                               \
    } while (0)

/*
 * Arm the timer and wait for the line itself, so IP7 is a pending
 * level. A
 * host stall across the two arming writes lets Count pass Compare
 * before
 * Compare lands; that lost window is detected and re-armed, and the
 * wait
 * polls the line rather than the counter, the interrupt-delivery ROM's
 * shape. Holds whether or not the model matches a behind Compare write.
 */
static void raise_timer_line(void)
{
    int i, tries;

    for (tries = 0; tries < 8; tries++) {
        rom_set_count(0);
        rom_set_compare(TIMER_TARGET);
        if (rom_get_count() >= TIMER_TARGET) {
            continue;
        }
        for (i = 0; i < POLL_LIMIT && !(rom_get_cause() & ROM_CAUSE_IP7);
             i++) {
        }
        return;
    }
}

void rom_main(void)
{
    uint64_t lo0_a = rom_entrylo(PHYS_A, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo1_a = rom_entrylo(PHYS_A_ODD, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo0_b = rom_entrylo(PHYS_B, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo1_b = rom_entrylo(PHYS_B_ODD, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo0_c = rom_entrylo(PHYS_C, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint64_t lo1_c = rom_entrylo(PHYS_C_ODD, MAP_RW, ROM_CACHE_NONCOHERENT);
    uint32_t st, probe, base;

    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(0);
    rom_set_entryhi(0);
    rom_exc_reset();

    /*
     * The bit starts clear, and stays clear across writes that conflict
     * with
     * nothing.
     */
    rom_assert_eq(1, 0, status_ts());

    *phys(PHYS_A) = 0x0a11ce00u;
    *phys(PHYS_A_ODD) = 0x0a11ce01u;
    *phys(PHYS_B) = 0xb0bca700u;
    *phys(PHYS_B_ODD) = 0xb0bca701u;

    tlb_write_index(2, VIRT_X, lo0_a, lo1_a, ROM_PAGEMASK_4K);
    rom_assert_eq(2, 0, status_ts());
    rom_assert_eq(3, 0x0a11ce00u, load32(VIRT_X));

    /*
     * The same virtual page written at another index: the write itself
     * must
     * set the TS bit, translation must reach the new entry's physical
     * page -
     * not whichever entry a lookup happens to find first - and the old
     * entry
     * must read back invalidated, no longer answering a probe.
     */
    tlb_write_index(9, VIRT_X, lo0_b, lo1_b, ROM_PAGEMASK_4K);
    rom_assert_eq(4, ROM_ST_TS, status_ts());
    rom_assert_eq(5, 0xb0bca700u, load32(VIRT_X));
    rom_assert_eq(6, 0xb0bca701u, load32(VIRT_X + 0x1000));
    tlb_read_index(2);
    rom_assert_eq(7, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    rom_assert_eq(8, 0, rom_get_entrylo1() & ROM_ENTRYLO_V);
    rom_assert(9, rom_get_entryhi() != VIRT_X);
    probe = tlb_probe(VIRT_X);
    rom_assert_eq(10, 0, probe & 0x80000000u);
    rom_assert_eq(11, 9, probe & 0x3f);
    tlb_read_index(9);
    rom_assert_eq(12, VIRT_X, rom_get_entryhi());

    /*
     * Software owns the bit both ways: a written zero clears it, a
     * written
     * one sets it.
     */
    st = rom_get_status();
    rom_set_status(st & ~ROM_ST_TS);
    rom_assert_eq(13, 0, status_ts());
    rom_set_status(st | ROM_ST_TS);
    rom_assert_eq(14, ROM_ST_TS, status_ts());
    clear_ts();
    rom_assert_eq(15, 0, status_ts());

    /*
     * A write that overlaps nothing sets nothing and disturbs nothing.
     */
    tlb_write_index(4, VIRT_Y, lo0_c, lo1_c, ROM_PAGEMASK_4K);
    rom_assert_eq(16, 0, status_ts());
    probe = tlb_probe(VIRT_X);
    rom_assert_eq(17, 9, probe & 0x3f);

    /*
     * Address space identifiers separate entries the way globality
     * joins
     * them: the same page number under a second identifier is not a
     * conflict,
     * but a global entry for that page conflicts with both, since it
     * would
     * match whatever the current identifier.
     */
    tlb_write_index(6, VIRT_Z | ASID_MINE,
                    rom_entrylo(PHYS_A, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_A_ODD, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_assert_eq(18, 0, status_ts());
    tlb_write_index(26, VIRT_Z | ASID_OTHER,
                    rom_entrylo(PHYS_B, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_B_ODD, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_assert_eq(19, 0, status_ts());
    tlb_write_index(27, VIRT_Z, lo0_c, lo1_c, ROM_PAGEMASK_4K);
    rom_assert_eq(20, ROM_ST_TS, status_ts());
    tlb_read_index(6);
    rom_assert_eq(21, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    tlb_read_index(26);
    rom_assert_eq(22, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    tlb_read_index(27);
    rom_assert_eq(23, VIRT_Z, rom_get_entryhi());
    clear_ts();

    /*
     * The manual says even the Wired area: an entry below the wired
     * floor is
     * protected from random replacement, not from the conflict rule.
     */
    *phys(PHYS_A) = 0x01dead00u;
    *phys(PHYS_B) = 0x0eb0b500u;
    rom_set_wired(8);
    tlb_write_index(3, VIRT_W, lo0_a, lo1_a, ROM_PAGEMASK_4K);
    rom_assert_eq(24, 0x01dead00u, load32(VIRT_W));
    rom_assert_eq(25, 0, status_ts());
    tlb_write_index(20, VIRT_W, lo0_b, lo1_b, ROM_PAGEMASK_4K);
    rom_assert_eq(26, ROM_ST_TS, status_ts());
    tlb_read_index(3);
    rom_assert_eq(27, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    rom_assert_eq(28, 0x0eb0b500u, load32(VIRT_W));
    rom_set_wired(0);
    clear_ts();

    /*
     * Overlap is judged under each entry's own page mask, not by equal
     * page
     * numbers: a 16 Mbyte pair covers the 4 Kbyte page inside it, so
     * writing
     * it is a conflict, and the load lands where the big pages say.
     */
    *phys(PHYS_C) = 0x5ca1ab00u;
    *phys(PHYS_BIG + 0x4000) = 0xb16b16b1u;
    tlb_write_index(5, VIRT_IN, lo0_c, lo1_c, ROM_PAGEMASK_4K);
    rom_assert_eq(29, 0x5ca1ab00u, load32(VIRT_IN));
    rom_assert_eq(30, 0, status_ts());
    tlb_write_index(30, VIRT_BIG,
                    rom_entrylo(PHYS_BIG, MAP_RW, ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_BIG + 0x1000000, MAP_RW,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_16M);
    rom_assert_eq(31, ROM_ST_TS, status_ts());
    tlb_read_index(5);
    rom_assert_eq(32, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    rom_assert_eq(33, 0xb16b16b1u, load32(VIRT_IN));
    clear_ts();

    /*
     * The rule holds for the random write as well. An entry for the
     * page
     * under a foreign identifier sits in the wired area; a load misses
     * past
     * it and the harness's refill handler installs a global entry with
     * a
     * random write, which can only pick an index above the floor. That
     * write
     * must invalidate the wired entry and set the bit, exactly as the
     * indexed one did.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_wired(11);
    *phys(PHYS_B) = 0xfab0b0b0u;
    tlb_write_index(10, VIRT_R | ASID_OTHER,
                    rom_entrylo(PHYS_A, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    rom_entrylo(PHYS_A_ODD, ROM_ENTRYLO_V | ROM_ENTRYLO_D,
                                ROM_CACHE_NONCOHERENT),
                    ROM_PAGEMASK_4K);
    rom_assert_eq(34, 0, status_ts());
    rom_set_entryhi(ASID_MINE);
    rom_exc_reset();
    rom_exc.entrylo0 = lo0_b;
    rom_exc.entrylo1 = lo1_b;
    rom_exc.pagemask = ROM_PAGEMASK_4K;
    rom_exc.refill = 1;
    rom_assert_eq(35, 0xfab0b0b0u, load32(VIRT_R));
    rom_exc.refill = 0;
    rom_assert_eq(36, 1, rom_exc.count);
    rom_assert_eq(37, ROM_ST_TS, status_ts());
    tlb_read_index(10);
    rom_assert_eq(38, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);
    rom_set_wired(0);
    clear_ts();

    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_entryhi(0);

    /*
     * Writing register 23 must not enter a debug mode this processor
     * does
     * not have: a pending timer interrupt, unmasked afterwards, is
     * still
     * delivered. On a model that dispatches register 23 to the EJTAG
     * debug
     * register, the debug-mode bit masks all interrupts and the wait
     * below
     * runs out with nothing delivered.
     */
    rom_exc_reset();
    base = rom_get_status() & ~(ROM_ST_IE | ROM_ST_IM7);
    rom_set_status(base);
    rom_assert_eq(39, 0, rom_get_status() &
                  (ROM_ST_IE | ROM_ST_EXL | ROM_ST_ERL));
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(40, 0, rom_get_cause() & ROM_CAUSE_IP7);
    raise_timer_line();
    rom_assert_eq(41, ROM_CAUSE_IP7, rom_get_cause() & ROM_CAUSE_IP7);
    rom_assert_eq(42, 0, rom_exc.count);

    __asm__ __volatile__("mtc0 %0, $23" : : "r"(EJTAG_DM));

    rom_exc.status_set = base;
    UNMASK_AND_WAIT(base | ROM_ST_IE | ROM_ST_IM7);
    rom_assert_eq(43, 1, rom_exc.count);
    rom_assert_eq(44, ROM_EXC_INT, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Leave nothing pending and nothing open behind.
     */
    rom_set_status(base);
    rom_set_compare(rom_get_count() + 0x40000000u);
    rom_assert_eq(45, 0, rom_get_cause() & ROM_CAUSE_IP7);

    /*
     * Reading back a conflict-invalidated entry loads the PageMask
     * register
     * with the entry's own stale mask, the way reading any entry does;
     * a
     * guest that sets PageMask once and relies on read-backs restoring
     * the
     * one size it ever writes must not inherit a zero mask. The
     * sentinel
     * below proves the read-back wrote the register rather than leaving
     * it
     * standing.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);
    clear_ts();
    tlb_write_index(6, VIRT_Y, lo0_a, lo1_a, ROM_PAGEMASK_16K);
    tlb_write_index(21, VIRT_Y, lo0_b, lo1_b, ROM_PAGEMASK_16K);
    rom_assert_eq(46, ROM_ST_TS, status_ts());
    rom_set_pagemask(ROM_PAGEMASK_16M);
    tlb_read_index(6);
    rom_assert_eq(47, ROM_PAGEMASK_16K, rom_get_pagemask());
    rom_assert_eq(48, 0, rom_get_entrylo0() & ROM_ENTRYLO_V);

    /*
     * The read-back's other three destinations. The manual does not
     * give the
     * bit image an invalidated entry reads back as, only that the entry
     * no
     * longer matches; the model presents zeroes, so a guest that dumps
     * entries and writes the dump back gets an entry that maps nothing
     * rather than a resurrected stale translation. These pin that
     * committed
     * presentation - the full destination set of the read-back - so it
     * cannot drift piecemeal.
     */
    rom_assert_eq(49, 0, rom_get_entryhi());
    rom_assert_eq(50, 0, rom_get_entrylo0());
    rom_assert_eq(51, 0, rom_get_entrylo1());
    rom_set_pagemask(ROM_PAGEMASK_4K);
    clear_ts();
}
