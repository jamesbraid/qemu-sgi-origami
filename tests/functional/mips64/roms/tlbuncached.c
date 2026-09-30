/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test that a translated uncached page reaches the space selected by
 * its TLB Uncached Attribute field. The R10000 manual distinguishes
 * that field from the address-sourced attribute used for unmapped
 * references.
 *
 * Source: MIPS R10000 User's Manual, version 2.0, uncached-attribute
 * and EntryLo sections.
 */

#include "rom.h"

const char rom_name[] = "tlbuncached";

/*
 * A virtual page in the user segment, translated from kernel mode too.
 */
#define VIRT 0x14000000ull

/*
 * What each space holds at the physical offset mapped below.
 */
#define MEMORY_PA   0x00500000ull       /* plain node memory */
/* The boot flash in Hub special space. */
#define FLASH_PA    0x10000000ull
/* Hub registers in crosstalk space. */
#define HUB_REGS_PA 0x01600000ull

#define FLASH_PROBE_OFFSET  0x68        /* any offset inside the flash */
/* The register at the crosstalk offset. */
#define NI_STATUS_OFFSET    0x0000

/*
 * The unmapped uncached view of the same Hub register window.
 */
#define HUB_REGS_VIEW       0x9200000001600000ull

/*
 * The uncached compatibility view of the flash, which the reset vector
 * uses.
 */
#define FLASH_RESET_VIEW    0xffffffffbfc00000ull

#define PLANTED  0x0123456789abcdefull

static void map_one(uint64_t paddr, int uc)
{
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(VIRT);
    rom_set_entrylo0(rom_entrylo_in(paddr,
                                    ROM_ENTRYLO_V | ROM_ENTRYLO_D |
                                    ROM_ENTRYLO_G,
                                    ROM_CACHE_UNCACHED, uc));
    rom_set_entrylo1(rom_entrylo_in(paddr + 0x1000,
                                    ROM_ENTRYLO_V | ROM_ENTRYLO_D |
                                    ROM_ENTRYLO_G,
                                    ROM_CACHE_UNCACHED, uc));
    rom_set_index(0);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_entryhi(0);
}

static uint64_t load64(uint64_t vaddr)
{
    uint64_t v;

    __asm__ __volatile__("ld %0, 0(%1)" : "=r"(v) : "r"(vaddr) : "memory");
    return v;
}

void rom_main(void)
{
    /*
     * Volatile preserves accesses through the uncached physical-memory
     * mapping.
     */
    volatile uint64_t *plant;
    uint64_t flash_view;

    rom_set_status(rom_get_status() | ROM_ST_KX);

    /*
     * Something recognisable in memory to find through the mapping.
     */
    plant = (volatile uint64_t *)(unsigned long)(0xffffffffa0000000ull +
                                                 MEMORY_PA);
    *plant = PLANTED;

/* Attribute 0 selects ordinary memory; ignoring the field would mimic it. */
    rom_exc_reset();
    map_one(MEMORY_PA, ROM_UC_MEMORY);
    rom_assert_eq(1, PLANTED, load64(VIRT));
    rom_assert_eq(2, 0, rom_exc.count);

/* Attribute 1 selects Hub special space (boot flash), distinct from RAM. */
    flash_view = *(volatile uint64_t *)(unsigned long)(FLASH_RESET_VIEW +
                                                       FLASH_PROBE_OFFSET);

    rom_exc_reset();
    map_one(FLASH_PA, ROM_UC_HUB);
    rom_assert_eq(3, flash_view, load64(VIRT + FLASH_PROBE_OFFSET));
    rom_assert_eq(4, 0, rom_exc.count);

/* Attribute 2 selects crosstalk; compare the translated and unmapped views. */
    rom_exc_reset();
    map_one(HUB_REGS_PA, ROM_UC_XTALK);
    rom_assert_eq(5, load64(HUB_REGS_VIEW + NI_STATUS_OFFSET),
                  load64(VIRT + NI_STATUS_OFFSET));
    rom_assert_eq(6, 0, rom_exc.count);

/* EntryLo readback must preserve the uncached attribute. */
    rom_set_index(0);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(7, ROM_UC_XTALK,
                  (rom_get_entrylo0() >> ROM_ENTRYLO_UC_SHIFT) & 3);
    rom_assert_eq(8, ROM_UC_XTALK,
                  (rom_get_entrylo1() >> ROM_ENTRYLO_UC_SHIFT) & 3);

/* Cacheable entries ignore the uncached attribute and reach ordinary memory. */
    rom_exc_reset();
    rom_tlb_park(ROM_TLB_ENTRIES);
    rom_set_pagemask(ROM_PAGEMASK_4K);
    rom_set_entryhi(VIRT);
    rom_set_entrylo0(rom_entrylo_in(MEMORY_PA,
                                    ROM_ENTRYLO_V | ROM_ENTRYLO_D |
                                    ROM_ENTRYLO_G,
                                    ROM_CACHE_NONCOHERENT, ROM_UC_HUB));
    rom_set_entrylo1(rom_entrylo_in(MEMORY_PA + 0x1000,
                                    ROM_ENTRYLO_V | ROM_ENTRYLO_D |
                                    ROM_ENTRYLO_G,
                                    ROM_CACHE_NONCOHERENT, ROM_UC_HUB));
    rom_set_index(0);
    __asm__ __volatile__("tlbwi" : : : "memory");
    rom_set_entryhi(0);
    rom_assert_eq(9, PLANTED, load64(VIRT));
    rom_assert_eq(10, 0, rom_exc.count);
}
