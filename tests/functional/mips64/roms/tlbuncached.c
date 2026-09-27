/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A translated uncached page reaches the space its entry names.
 *
 * An unmapped reference takes its uncached attribute from the address,
 * and a
 * translated one takes it from the entry that translated it. The R10000
 * manual's uncached-attribute section says both in one breath: "For
 * unmapped
 * accesses, the uncached attribute is sourced from VA[58:57]. For
 * mapped
 * accesses, the uncached attribute is sourced from the TLB Uncached
 * Attribute
 * field. The TLB Uncached Attribute field may be initialized in 64-bit
 * mode
 * using bits 63:62 of the CP0 EntryLo0 and EntryLo1 registers." Its
 * EntryLo
 * section confirms where that field lives and that a TLB write carries
 * it:
 * "Two additional bits for the mapped space's uncached attribute can be
 * loaded
 * into bits 63:62 of the EntryLo register, which are then written into
 * the
 * TLB
 * with a TLB Write."
 *
 * Why this is worth a ROM of its own: when the mapped path ignores the
 * attribute, every translated uncached page reaches memory whatever it
 * names.
 * There is no exception and no log — a driver mapping a register window
 * reads
 * node memory and believes it. Silent wrong data is the failure this
 * guards
 * against, so the check has to be that an access lands somewhere
 * identifiable
 * rather than merely that it completes.
 *
 * The three spaces used here are identifiable because the bare machine
 * puts
 * something different in each at the offsets mapped. Memory holds
 * whatever
 * was
 * planted in it. The Hub's special space holds the boot flash, so a
 * mapping
 * that names it reads the same bytes the reset vector's own view of the
 * flash
 * reads. The routine 64 MiB population does not back ordinary memory at
 * the
 * flash offset, so a mapping that wrongly reached memory takes an
 * exception;
 * the ROM's exception checks catch it. The crosstalk space holds the
 * Hub's own registers, whose network-interface status register reports
 * M mode
 * and node zero.
 *
 * Note the flash is not checked against the configuration block this
 * machine
 * synthesizes, because a test ROM is handed to the machine as its
 * firmware
 * and
 * therefore *is* the flash content. Comparing two views of whatever the
 * flash
 * holds is the check that survives that.
 *
 * The R10000 alone: on the R4000 those two bits are the read-inhibit
 * and
 * execute-inhibit bits, and setting them makes a page unreadable rather
 * than
 * naming a space.
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

    /*
     * The attribute that means memory. This is the one an operating
     * system
     * has
     * to use for an uncached view of ordinary memory, and the one a
     * model
     * that
     * ignored the attribute would appear to implement for every value.
     */
    rom_exc_reset();
    map_one(MEMORY_PA, ROM_UC_MEMORY);
    rom_assert_eq(1, PLANTED, load64(VIRT));
    rom_assert_eq(2, 0, rom_exc.count);

    /*
     * The attribute that means the Hub's special space, where the same
     * physical
     * offset is the boot flash rather than memory. The ordinary-memory
     * view
     * at
     * this offset is absent on the routine 64 MiB population, so an
     * ignored
     * attribute faults rather than returning a plausible memory value.
     */
    flash_view = *(volatile uint64_t *)(unsigned long)(FLASH_RESET_VIEW +
                                                       FLASH_PROBE_OFFSET);

    rom_exc_reset();
    map_one(FLASH_PA, ROM_UC_HUB);
    rom_assert_eq(3, flash_view, load64(VIRT + FLASH_PROBE_OFFSET));
    rom_assert_eq(4, 0, rom_exc.count);

    /*
     * The attribute that means crosstalk space, where the same offset
     * is the
     * Hub's own register window. The check is two views of one
     * register: the
     * translated read must see what the unmapped crosstalk view sees.
     * The
     * register's value itself carries board identity and is pinned by
     * the
     * machine-model tests, not here.
     */
    rom_exc_reset();
    map_one(HUB_REGS_PA, ROM_UC_XTALK);
    rom_assert_eq(5, load64(HUB_REGS_VIEW + NI_STATUS_OFFSET),
                  load64(VIRT + NI_STATUS_OFFSET));
    rom_assert_eq(6, 0, rom_exc.count);

    /*
     * Reading the entry back reports the attribute that was written,
     * which is
     * what a guest saving and restoring a translation depends on.
     */
    rom_set_index(0);
    __asm__ __volatile__("tlbr" : : : "memory");
    rom_assert_eq(7, ROM_UC_XTALK,
                  (rom_get_entrylo0() >> ROM_ENTRYLO_UC_SHIFT) & 3);
    rom_assert_eq(8, ROM_UC_XTALK,
                  (rom_get_entrylo1() >> ROM_ENTRYLO_UC_SHIFT) & 3);

    /*
     * A cacheable entry drives no attribute at all, so it reaches
     * memory
     * whatever those two bits hold. That is what lets a ROM that runs
     * against
     * more than one processor map a page without naming a space.
     */
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
