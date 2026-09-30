/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check R10000 processor identity, Config fields, and 64-entry TLB
 * size. See the MIPS R10000 User's Manual version 2.0. The 44-bit user
 * address width is covered by the XContext register ROM.
 */

#include "rom.h"

const char rom_name[] = "cpuid";

static uint32_t field(uint32_t word, int shift, int width)
{
    return (word >> shift) & ((1u << width) - 1);
}

/*
 * Config register field positions, from the R10000 manual's Config
 * register
 * field definitions (Table 14-15). The secondary cache clock divisor
 * occupies
 * bits 21:19, which the same table lists between the secondary cache
 * size and
 * the reserved bits.
 */
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
     * The implementation number of the R10000 is 0x09. The revision
     * number in
     * the low byte is deliberately not checked: the same manual section
     * warns
     * that a chip change need not change the revision number and a
     * revision
     * number change need not mean a chip change, so software must not
     * characterise the processor by it — and neither should a test.
     */
    rom_assert_eq(1, 0x09, field(prid, 8, 8));

    /*
     * The upper halfword is reserved and reads zero.
     */
    rom_assert_eq(2, 0, field(prid, 16, 16));

    config = rom_get_config();

    /*
     * Bit 31 is checked on its own because it is where a model built
     * from the
     * later MIPS32-style Config register would give itself away. There,
     * bit
     * 31
     * announces that a second Config register exists. On the R10000 it
     * is
     * simply the top bit of the primary instruction cache size field,
     * and
     * since that field holds 3 the bit reads zero.
     */
    rom_assert_eq(3, 0, field(config, 31, 1));

    /*
     * The Config register section states the primary cache size fields
     * are
     * hardwired to 011, and the manual's own annotation of that table
     * reads
     * those as 32 Kbytes each. The decoded size is 4 Kbytes shifted
     * left by
     * the
     * field value.
     */
    rom_assert_eq(4, 3, field(config, CFG_IC_SHIFT, 3));
    rom_assert_eq(5, 32 * 1024, 4096u << field(config, CFG_IC_SHIFT, 3));
    rom_assert_eq(6, 3, field(config, CFG_DC_SHIFT, 3));
    rom_assert_eq(7, 32 * 1024, 4096u << field(config, CFG_DC_SHIFT, 3));

    /*
     * The secondary cache size field encoding starts at 512 Kbytes for
     * zero
     * and
     * doubles per step, so the Origin 200's one megabyte of secondary
     * cache
     * is
     * field value 1.
     */
    rom_assert_eq(8, 1, field(config, CFG_SS_SHIFT, 3));
    rom_assert_eq(9, 1024 * 1024,
                  (512u * 1024) << field(config, CFG_SS_SHIFT, 3));

    /*
     * The secondary cache block size field selects a 16-word block when
     * clear
     * and a 32-word block when set, so a 128-byte secondary cache line
     * is
     * field
     * value 1. Both IRIX and Linux expect a 128-byte line on this
     * processor.
     */
    rom_assert_eq(10, 1, field(config, CFG_SB_SHIFT, 1));
    rom_assert_eq(11, 128, 64u << field(config, CFG_SB_SHIFT, 1));

    /*
     * The memory system endianness bit is set for big endian, which
     * every SGI
     * machine is.
     */
    rom_assert_eq(12, 1, field(config, CFG_BE_SHIFT, 1));

    /*
     * The processor to system clock divisor encoding gives division by
     * two
     * for
     * field value 3, which is the Origin 200's 180 MHz processor
     * feeding its
     * 90 MHz hub.
     */
    rom_assert_eq(13, 3, field(config, CFG_EC_SHIFT, 4));

    /*
     * The secondary cache clock divisor does NOT repeat the value
     * above, and
     * an
     * earlier version of this ROM asserted that it did. The two fields
     * have
     * different widths and different encodings. The manual's clock
     * section
     * gives the processor clock as the system clock times (divisor + 1)
     * over
     * two, and the secondary cache clock as the system clock times
     * (divisor +
     * 1) over (this field + 1) — so the ratio of processor clock to
     * secondary
     * cache clock is (this field + 1) / 2, and the field's five useful
     * values
     * give the 1, 1.5, 2, 2.5 and 3 the same section names in prose.
     *
     * Field value 2 is therefore a ratio of 1.5: a 120 MHz secondary
     * cache
     * behind a 180 MHz processor. That is what the board's own reset
     * mode
     * word
     * selects. The 3 asserted here before would be a ratio of 2, a 90
     * MHz
     * secondary cache, which is not this machine.
     */
    rom_assert_eq(14, 2, field(config, CFG_SC_SHIFT, 3));

    /*
     * The kseg0 cache algorithm holds 5, which the manual's cache
     * algorithm
     * table names cacheable coherent exclusive on write. This ROM
     * asserted 3,
     * cacheable noncoherent, on the reasoning that a single-processor
     * node
     * boots that way — but the board's reset mode word selects a
     * coherent
     * algorithm, which is what a machine whose whole architecture is
     * cache
     * coherence across nodes would be expected to do. One node is still
     * a
     * coherent node.
     */
    rom_assert_eq(15, 5, field(config, CFG_K0_SHIFT, 3));

    /*
     * Most of the remaining system interface fields are zero on this
     * board,
     * and
     * the reserved bits read zero because the manual says they do.
     */
    rom_assert_eq(16, 0, field(config, CFG_DN_SHIFT, 2));
    rom_assert_eq(17, 0, field(config, CFG_CT_SHIFT, 1));
    rom_assert_eq(18, 0, field(config, CFG_PE_SHIFT, 1));

    /*
     * The maximum outstanding requests field is the exception, and it
     * was
     * swept up in the "all zero" claim above. The manual's table for it
     * reads
     * one request for field value 0 through four requests for field
     * value 3,
     * so zero would mean the processor may have a single request
     * outstanding
     * at a time. The board asks for 3, the full four, which is both the
     * most
     * the processor supports and what a machine built for throughput
     * would
     * choose. Asserting zero here was asserting a crippled
     * configuration.
     */
    rom_assert_eq(19, 3, field(config, CFG_PM_SHIFT, 2));

    rom_assert_eq(20, 0, field(config, CFG_SK_SHIFT, 1));
    rom_assert_eq(21, 0, field(config, CFG_RES_SHIFT, 4));

    /*
     * The entry index is six bits wide, which is what makes 63 the
     * highest
     * entry a 64-entry buffer has and what a 48-entry R4000 cannot
     * hold. The
     * manual specifies the width of the field and says nothing about
     * what
     * writing to the bits above it does, so the check is on the
     * boundary
     * rather
     * than on any masking rule: index 63 must survive a write and read
     * back,
     * and index 64 must not appear because there is no such entry.
     */
    rom_set_index(0);
    rom_assert_eq(22, 0, rom_get_index());
    rom_set_index(0x3f);
    rom_assert_eq(23, 0x3f, rom_get_index());
    rom_set_index(0x40);
    rom_assert_eq(24, 0x3f, rom_get_index());

    /*
     * The number of wired entries is held in a field of the same width,
     * so 63
     * is writable there too, and a 48-entry buffer cannot represent it.
     */
    rom_set_wired(WIRED_FLOOR);
    rom_assert_eq(25, WIRED_FLOOR, rom_get_wired());
    rom_set_wired(0x3f);
    rom_assert_eq(26, 0x3f, rom_get_wired());

    /*
     * The replacement counter is specified only by its bounds: it never
     * selects
     * an entry below the wired count and never selects one past the
     * last
     * entry.
     * The exact sequence is deliberately not checked, because the
     * architecture
     * does not define it. What is checked is that the whole of the
     * specified
     * range is live — with the wired count at 32, a 64-entry buffer
     * must
     * eventually offer an entry at 48 or above, and over this many
     * samples a
     * counter that never did so would have to be confined to the lower
     * half
     * of
     * its range rather than merely unlucky.
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
