/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check R10000 identity, Config fields, and 64-entry TLB size. XContext
 * address-width behavior is checked separately.
 */

#include "rom.h"

const char rom_name[] = "cpuid";

static uint32_t field(uint32_t word, int shift, int width)
{
    return (word >> shift) & ((1u << width) - 1);
}

/* Config register field positions. */
#define CFG_K0_SHIFT   0        /* kseg0 cache algorithm, 3 bits */
#define CFG_DN_SHIFT   3        /* device number, 2 bits */
#define CFG_CT_SHIFT   5        /* coherent request target, 1 bit */
#define CFG_PE_SHIFT   6        /* eliminate requests enabled, 1 bit */
#define CFG_PM_SHIFT   7        /* maximum outstanding requests, 2 bits */
#define CFG_EC_SHIFT   9        /* processor to system clock divisor, 4 bits */
#define CFG_SB_SHIFT   13       /* secondary cache block size, 1 bit */
#define CFG_SK_SHIFT   14       /* secondary cache error correction, 1 bit */
#define CFG_BE_SHIFT   15       /* memory system endianness, 1 bit */
#define CFG_SS_SHIFT   16       /* secondary cache size, 3 bits */
/* Processor to secondary clock divisor, 3 bits. */
#define CFG_SC_SHIFT   19
#define CFG_RES_SHIFT  22       /* reserved, 4 bits, reads zero */
#define CFG_DC_SHIFT   26       /* primary data cache size, 3 bits */
#define CFG_IC_SHIFT   29       /* primary instruction cache size, 3 bits */

#define RANDOM_SAMPLES 256
#define WIRED_FLOOR    32

void rom_main(void)
{
    uint32_t prid, config, i, status;
    uint32_t seen_low = 0xffffffffu, seen_high = 0, distinct_seen = 0;
    uint32_t reached_upper_half = 0;
    uint32_t previous = 0xffffffffu;

    prid = rom_get_prid();

    /*
     * The implementation field identifies the R10000. The revision byte
     * is not a reliable processor identifier and is intentionally ignored.
     */
    rom_assert_eq(1, 0x09, field(prid, 8, 8));

    /*
     * The upper halfword is reserved and reads zero.
     */
    rom_assert_eq(2, 0, field(prid, 16, 16));

    config = rom_get_config();

    /*
     * Bit 31 is part of the instruction-cache size field here, not a
     * later-generation indicator for a second Config register.
     */
    rom_assert_eq(3, 0, field(config, 31, 1));

    /* Both primary cache size fields encode 32 KiB. */
    rom_assert_eq(4, 3, field(config, CFG_IC_SHIFT, 3));
    rom_assert_eq(5, 32 * 1024, 4096u << field(config, CFG_IC_SHIFT, 3));
    rom_assert_eq(6, 3, field(config, CFG_DC_SHIFT, 3));
    rom_assert_eq(7, 32 * 1024, 4096u << field(config, CFG_DC_SHIFT, 3));

    /* The Origin 200's 1 MiB secondary cache encodes as field value 1. */
    rom_assert_eq(8, 1, field(config, CFG_SS_SHIFT, 3));
    rom_assert_eq(9, 1024 * 1024,
                  (512u * 1024) << field(config, CFG_SS_SHIFT, 3));

    /* Field value 1 selects the 128-byte secondary-cache line. */
    rom_assert_eq(10, 1, field(config, CFG_SB_SHIFT, 1));
    rom_assert_eq(11, 128, 64u << field(config, CFG_SB_SHIFT, 1));

    /* The memory system is big-endian. */
    rom_assert_eq(12, 1, field(config, CFG_BE_SHIFT, 1));

    /* Field value 3 divides the 180 MHz CPU clock to the 90 MHz system bus. */
    rom_assert_eq(13, 3, field(config, CFG_EC_SHIFT, 4));

    /*
     * The secondary-cache divisor has a different encoding from the CPU
     * divisor. Value 2 gives a 120 MHz cache behind the 180 MHz processor.
     */
    rom_assert_eq(14, 2, field(config, CFG_SC_SHIFT, 3));

    /* The board reset selects coherent write-exclusive kseg0 caching. */
    rom_assert_eq(15, 5, field(config, CFG_K0_SHIFT, 3));

    /*
     * The remaining interface fields and reserved bits read zero on this
     * board.
     */
    rom_assert_eq(16, 0, field(config, CFG_DN_SHIFT, 2));
    rom_assert_eq(17, 0, field(config, CFG_CT_SHIFT, 1));
    rom_assert_eq(18, 0, field(config, CFG_PE_SHIFT, 1));

    /* The board enables the maximum four outstanding requests. */
    rom_assert_eq(19, 3, field(config, CFG_PM_SHIFT, 2));

    rom_assert_eq(20, 0, field(config, CFG_SK_SHIFT, 1));
    rom_assert_eq(21, 0, field(config, CFG_RES_SHIFT, 4));

    /*
     * Index 63 is the final entry of the 64-entry R10000 TLB; check the
     * boundary without assuming behavior for writes above the field width.
     */
    rom_set_index(0);
    rom_assert_eq(22, 0, rom_get_index());
    rom_set_index(0x3f);
    rom_assert_eq(23, 0x3f, rom_get_index());
    rom_set_index(0x40);
    rom_assert_eq(24, 0x3f, rom_get_index());

    /* Wired uses the same six-bit range and can represent 63 entries. */
    rom_set_wired(WIRED_FLOOR);
    rom_assert_eq(25, WIRED_FLOOR, rom_get_wired());
    rom_set_wired(0x3f);
    rom_assert_eq(26, 0x3f, rom_get_wired());

    /*
     * Random's sequence is implementation-defined; check its bounds and
     * that sampling reaches the upper half of the unwired entries.
     */
    rom_set_wired(WIRED_FLOOR);
    for (i = 0; i < RANDOM_SAMPLES; i++) {
        uint32_t r = rom_get_random();

        if (r < seen_low) {
            seen_low = r;
        }
        if (r > seen_high) {
            seen_high = r;
        }
        if (r != previous) {
            distinct_seen++;
            previous = r;
        }
        if (r >= 48) {
            reached_upper_half = 1;
        }
    }

    rom_assert(27, seen_low >= WIRED_FLOOR);
    rom_assert(28, seen_high <= 63);
    rom_assert(29, distinct_seen >= 2);
    rom_assert(30, reached_upper_half);

    /*
     * Leave the wired count as the startup code set it.
     */
    rom_set_wired(0);
    rom_assert_eq(31, 0, rom_get_wired());

    /*
     * Which Status register bits software may write is part of the
     * processor's
     * identity too, and two of them separate this processor from the
     * R4000.
     *
     * The manual's diagnostic status field figure (Figure 14-12) places
     * the
     * cache error disable at bit 16 and describes it in the
     * accompanying
     * table
     * (Table 14-11) as a control: "Specifies that cache parity or ECC
     * errors
     * cannot cause exceptions". The cache error section (section 9.6)
     * says
     * the
     * same from the other side: "Asserting the CP0 Status register DE
     * bit
     * suppresses the posting of future Cache Error exceptions".
     * Software
     * asserts it, so it has to stick.
     *
     * The same figure shows bits 24:23 as the field the table describes
     * as
     * "Reserved. Must be written as zeroes, and returns zeroes when
     * read".
     *
     * The bit at 18 is the cache-hit indicator the secondary Hit
     * operations
     * set and clear, and it is a software-writable one: the CACHE
     * instruction's own section says of it "This bit is readable and
     * writable by software", and the CACHE chapter repeats that "The CH
     * bit
     * can also be modified by a MTC0 instruction". So a write must
     * stick in
     * both directions.
     *
     * The bit at 17 is left alone deliberately: "Reserved in the
     * R10000, and
     * should be set to 0" is guidance to software rather than a
     * statement
     * about what a read returns.
     */
    status = rom_get_status();

    rom_set_status(status | ROM_ST_DE);
    rom_assert_eq(32, ROM_ST_DE, rom_get_status() & ROM_ST_DE);
    rom_set_status(status);
    rom_assert_eq(33, 0, rom_get_status() & ROM_ST_DE);

    rom_set_status(status | ROM_ST_DS_RESERVED);
    rom_assert_eq(34, 0, rom_get_status() & ROM_ST_DS_RESERVED);
    rom_set_status(status);

    /*
     * Writing the Config register must not disturb what the processor
     * is.
     * The manual's Config section hardwires both primary cache size
     * fields
     * and takes bits 24:0 from the reset mode bits; the kseg0 cache
     * algorithm is the one field it carries forward from the R4400 as
     * software's, and every MIPS kernel read-modify-writes the register
     * to
     * set exactly that field. A write mask built for another
     * processor's
     * layout zeroes the middle of this one — after which the caches
     * read as
     * absent until the next reset.
     */
    config = rom_get_config();

    rom_set_config(config);
    rom_assert_eq(35, config, rom_get_config());

    rom_set_config(config ^ 7u);
    rom_assert_eq(36, config ^ 7u, rom_get_config());

    /*
     * A write of every bit changes nothing beyond the kseg0 algorithm.
     */
    rom_set_config(0xffffffffu);
    rom_assert_eq(37, config | 7u, rom_get_config());

    rom_set_config(config);
    rom_assert_eq(38, config, rom_get_config());

    /*
     * A coprocessor 0 move with a nonzero select field is an undefined
     * encoding here: the manual's MFC0 format shows bits 10:0 as an
     * all-zero
     * field, and the R10000 has one Config register, not the later
     * architecture's numbered family. The model refuses the encoding
     * with a
     * reserved instruction exception — the same refusal every
     * MIPS32-selector
     * access gets on this processor, except the register 25 encodings
     * that
     * are the R10000's own performance counter instructions — so the
     * MIPS32-shaped Config1 QEMU keeps internally never reaches a
     * guest.
     * The target register is the second kernel scratch register, which
     * the
     * compiler never allocates, so nothing is disturbed if a regression
     * lets
     * the move execute.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x401b8001");        /* mfc0 $27, $16, select 1 */
    rom_assert_eq(39, 1, rom_exc.count);
    rom_assert_eq(40, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_set_status(status | ROM_ST_CH);
    rom_assert_eq(41, ROM_ST_CH, rom_get_status() & ROM_ST_CH);
    rom_set_status(status);
    rom_assert_eq(42, 0, rom_get_status() & ROM_ST_CH);

    /*
     * The CacheErr register is the opposite kind of register from the
     * ones
     * above: "a 32-bit read-only register", loaded by hardware when a
     * Cache
     * Error exception is taken and by nothing else. A write must
     * vanish. No
     * cache error has occurred in this run, so the register also still
     * holds
     * its post-reset contents — which is all a read here may assume,
     * not a
     * permanent zero.
     */
    rom_set_cacherr(0xffffffffu);
    rom_assert_eq(43, 0, rom_get_cacherr());
}
