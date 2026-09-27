/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * R14000 revision 2.4 identity and the reviewed register state that
 * differs
 * from a renamed R10000.
 *
 * Oracle: docs/datasheets/r14000.md at Origami commit 14dbec407f.  The
 * processor and FPU identifiers are bounded R14000 compatibility
 * choices.
 * The cache fields, TLB population, masks, and reset values below are
 * the
 * reviewed R14000 facts or explicit R12000A compatibility policy
 * recorded by
 * that contract.  Config bits 24 through 0 are supplied by the machine,
 * so
 * this ROM neither asserts nor manufactures a board mode word.
 */

#include "rom.h"

const char rom_name[] = "r14000_identity";

#define CFG_DC_SHIFT 26
#define CFG_IC_SHIFT 29

static uint32_t field(uint32_t word, int shift, int width)
{
    return (word >> shift) & ((1u << width) - 1);
}

static uint32_t cfc1_fcr0(void)
{
    uint32_t v;

    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "cfc1 %0, $0\n\t.set pop" : "=r"(v));
    return v;
}

static uint32_t cfc1_fcr31(void)
{
    uint32_t v;

    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "cfc1 %0, $31\n\t.set pop" : "=r"(v));
    return v;
}

static void ctc1_fcr31(uint32_t v)
{
    __asm__ __volatile__(".set push\n\t.set mips4\n\t.set hardfloat\n\t"
                         "ctc1 %0, $31\n\t.set pop" : : "r"(v));
}

void rom_main(void)
{
    uint32_t config = rom_get_config();
    uint32_t status = rom_get_status();

    rom_assert_eq(1, 0x00000f24u, rom_get_prid());
    rom_set_status(status | ROM_ST_CU1);
    rom_assert_eq(2, 0x00000900u, cfc1_fcr0());

    /*
     * Both 32 KiB primary caches use the family Config layout.
     */
    rom_assert_eq(3, 3, field(config, CFG_IC_SHIFT, 3));
    rom_assert_eq(4, 3, field(config, CFG_DC_SHIFT, 3));

    /*
     * Sixty-four entries make 63 the highest writable Index and Wired.
     */
    rom_set_index(0x3f);
    rom_assert_eq(5, 0x3f, rom_get_index());
    rom_set_index(0x40);
    rom_assert_eq(6, 0x3f, rom_get_index());
    rom_set_wired(0x3f);
    rom_assert_eq(7, 0x3f, rom_get_wired());
    rom_set_wired(0);

    rom_assert_eq(8, 0, rom_get_framemask());
    rom_set_framemask(0xffffffffu);
    rom_assert_eq(9, 0x0000ffffu, rom_get_framemask());
    rom_set_framemask(0);

    rom_set_pagemask(0xffffffffu);
    rom_assert_eq(10, 0x01ffe000u, rom_get_pagemask());
    rom_set_pagemask(0);

    rom_set_lladdr(0x89abcdefu);
    rom_assert_eq(11, 0x89abcdefu, rom_get_lladdr());
    rom_set_taglo(0xffffffffu);
    rom_assert_eq(12, 0xffffffffu, rom_get_taglo());
    rom_set_ecc(0xffffffffu);
    rom_assert_eq(13, 0x000003ffu, rom_get_ecc());

    /*
     * Only K0 may change; every machine-owned reset bit stays intact.
     */
    rom_set_config(config ^ 7u);
    rom_assert_eq(14, config ^ 7u, rom_get_config());
    rom_set_config(0xffffffffu);
    rom_assert_eq(15, config | 7u, rom_get_config());
    rom_set_config(config);

    /*
     * R12000A compatibility policy selects the eight-condition FCR31.
     */
    rom_assert_eq(16, 0, cfc1_fcr31());
    ctc1_fcr31(0xff800003u);
    rom_assert_eq(17, 0xff800003u, cfc1_fcr31());
    ctc1_fcr31(0x007c0000u);
    rom_assert_eq(18, 0, cfc1_fcr31());
    ctc1_fcr31(0);

    /*
     * The selected Status mask retains controls and drops reserved
     * bits.
     */
    rom_set_status(status | ROM_ST_DE | ROM_ST_CH | ROM_ST_DS_RESERVED);
    rom_assert_eq(19, ROM_ST_DE | ROM_ST_CH,
                  rom_get_status() &
                  (ROM_ST_DE | ROM_ST_CH | ROM_ST_DS_RESERVED));
    rom_set_status(status & ~ROM_ST_CU1);
}
