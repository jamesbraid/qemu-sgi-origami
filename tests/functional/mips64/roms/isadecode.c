/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that reserved and unavailable R10000 instructions trap with the
 * expected exception state. See the R10000 User's Manual.
 */

#include "rom.h"

const char rom_name[] = "isadecode";

void rom_main(void)
{
    uint32_t status;

    /*
     * The branch-on-coprocessor-0 encodings, all four condition forms.
     * The model this pins decoded the slot as another architecture's
     * per-thread register moves: an offset whose bits 15:11 are nonzero
     * named a general register to write, and the companion slot at
     * rs = 0x0C could write coprocessor 0 state. Here every form is a
     * Reserved Instruction exception.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x41000000");        /* bc0f, offset 0 */
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0x4101d800");        /* bc0t, offset bits 15:11 = 27 */
    rom_assert_eq(3, 1, rom_exc.count);
    rom_assert_eq(4, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0x41020000");        /* bc0fl */
    rom_assert_eq(5, 1, rom_exc.count);
    rom_assert_eq(6, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0x41030000");        /* bc0tl */
    rom_assert_eq(7, 1, rom_exc.count);
    rom_assert_eq(8, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * The rs = 0x0C slot, whose foreign decode wrote CP0 state.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x41800000");
    rom_assert_eq(9, 1, rom_exc.count);
    rom_assert_eq(10, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Undefined major opcodes are the cause list's first entry. The
     * model ignored the MDMX major outright and executed the MIPS32
     * multiply from the SPECIAL2 major; the R10000 has neither.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x78000000");        /* the MDMX major, 0x1e */
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * SPECIAL2 MUL, spelled as $27 = $26 * $26 so an execution stays
     * inside the scratch registers.
     */
    rom_exc_reset();
    ROM_TRY("li $26, 6\n\t"
            ".word 0x735ad802");
    rom_assert_eq(13, 1, rom_exc.count);
    rom_assert_eq(14, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Coprocessor 2: defined, absent, and the one refusal that depends
     * on state. CU2 is an ordinary read/write usability bit, and the
     * five COP2-family opcodes always trap — Coprocessor Unusable
     * naming unit 2 while CU2 is clear, Reserved Instruction once it is
     * set. A model whose Status mask pins CU2 to zero can never show
     * the second half, and a guest restoring a saved Status loses the
     * bit on the way through.
     */
    status = rom_get_status();
    rom_set_status(status | ROM_ST_CU2);
    rom_assert_eq(15, ROM_ST_CU2, rom_get_status() & ROM_ST_CU2);
    rom_set_status(status & ~ROM_ST_CU2);
    rom_assert_eq(16, 0, rom_get_status() & ROM_ST_CU2);

    /*
     * CU2 clear: every opcode of the family is Coprocessor Unusable.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x481b0000");        /* MFC2 $27, $0 */
    rom_assert_eq(17, 1, rom_exc.count);
    rom_assert_eq(18, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(19, 2, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);

    rom_exc_reset();
    ROM_TRY(".word 0xc8000000");        /* LWC2 */
    rom_assert_eq(20, 1, rom_exc.count);
    rom_assert_eq(21, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(22, 2, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);

    rom_exc_reset();
    ROM_TRY(".word 0xd8000000");        /* LDC2 */
    rom_assert_eq(23, 1, rom_exc.count);
    rom_assert_eq(24, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(25, 2, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);

    rom_exc_reset();
    ROM_TRY(".word 0xe8000000");        /* SWC2 */
    rom_assert_eq(26, 1, rom_exc.count);
    rom_assert_eq(27, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(28, 2, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);

    rom_exc_reset();
    ROM_TRY(".word 0xf8000000");        /* SDC2 */
    rom_assert_eq(29, 1, rom_exc.count);
    rom_assert_eq(30, ROM_EXC_CPU, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(31, 2, (rom_exc.cause >> ROM_CAUSE_CE_SHIFT) & 3);

    /*
     * CU2 set: the same words become Reserved Instruction.
     */
    rom_set_status(status | ROM_ST_CU2);

    rom_exc_reset();
    ROM_TRY(".word 0x481b0000");
    rom_assert_eq(32, 1, rom_exc.count);
    rom_assert_eq(33, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0xc8000000");
    rom_assert_eq(34, 1, rom_exc.count);
    rom_assert_eq(35, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0xd8000000");
    rom_assert_eq(36, 1, rom_exc.count);
    rom_assert_eq(37, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0xe8000000");
    rom_assert_eq(38, 1, rom_exc.count);
    rom_assert_eq(39, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY(".word 0xf8000000");
    rom_assert_eq(40, 1, rom_exc.count);
    rom_assert_eq(41, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_set_status(status & ~ROM_ST_CU2);

    /*
     * WAIT (COP0 function 0x20). The R10000's COP0 instruction set does
     * not define it; a model that inherits it from later architectures
     * halts here instead of refusing, and this routine never returns.
     */
    rom_exc_reset();
    ROM_TRY(".word 0x42000020");
    rom_assert_eq(42, 1, rom_exc.count);
    rom_assert_eq(43, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
}
