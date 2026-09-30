/* SPDX-License-Identifier: GPL-2.0-or-later */
/* R10000 reserved and unavailable instruction exceptions. */

#include "rom.h"

const char rom_name[] = "isadecode";

void rom_main(void)
{
    uint32_t status;

    /* All four BC0 condition encodings are Reserved Instruction. */
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

    /* The rs=0x0c encoding is also Reserved Instruction. */
    rom_exc_reset();
    ROM_TRY(".word 0x41800000");
    rom_assert_eq(9, 1, rom_exc.count);
    rom_assert_eq(10, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /* Unsupported MDMX and SPECIAL2 encodings must trap. */
    rom_exc_reset();
    ROM_TRY(".word 0x78000000");        /* the MDMX major, 0x1e */
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    ROM_TRY("li $26, 6\n\t"
            ".word 0x735ad802");
    rom_assert_eq(13, 1, rom_exc.count);
    rom_assert_eq(14, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));

    /* CU2 must be writable to test both COP2 exception classes. */
    status = rom_get_status();
    rom_set_status(status | ROM_ST_CU2);
    rom_assert_eq(15, ROM_ST_CU2, rom_get_status() & ROM_ST_CU2);
    rom_set_status(status & ~ROM_ST_CU2);
    rom_assert_eq(16, 0, rom_get_status() & ROM_ST_CU2);

    /* With CU2 clear, each COP2 opcode reports unit 2 unusable. */
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

    /* With CU2 set, those encodings are Reserved Instruction. */
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

    /* WAIT is undefined on R10000; accepting it would halt the ROM. */
    rom_exc_reset();
    ROM_TRY(".word 0x42000020");
    rom_assert_eq(42, 1, rom_exc.count);
    rom_assert_eq(43, ROM_EXC_RI, ROM_CAUSE_EXC(rom_exc.cause));
}
