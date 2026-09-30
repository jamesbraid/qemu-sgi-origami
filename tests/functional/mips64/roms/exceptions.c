/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Check exception vectors, recorded state, codes, and return behavior. */

#include "rom.h"

const char rom_name[] = "exceptions";

/* Uncached storage for deliberately misaligned accesses. */
static uint64_t scratch[4];

/* Record the block start and resume after it, regardless of faulting op. */
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
                           [p] "r"(&scratch[0]), [one] "r"(1ull)              \
                         : "$26", "memory")

#define VEC_BOOTSTRAP_GENERAL (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)
#define VEC_BOOTSTRAP_REFILL  (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

void rom_main(void)
{
    uint64_t first, junk, reached;
    uint32_t status;

    /*
     * A word load from an address that is not a multiple of four is an
     * address
     * error on a load. The recorded program counter is the load itself
     * and
     * the
     * branch-delay bit is clear, because the load is not in a delay
     * slot.
     */
    rom_exc_reset();
    TRY_AT(first, "lw %[j], 1(%[p])", [j] "=&r"(junk));
    rom_assert_eq(1, 1, rom_exc.count);
    rom_assert_eq(2, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(3, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(4, 0, rom_exc.cause & ROM_CAUSE_BD);
    rom_assert_eq(5, first, rom_exc.epc);
    rom_assert_eq(6, (uint64_t)(unsigned long)&scratch[0] + 1,
                  rom_exc.badvaddr);

    /*
     * The store form reports an address error on a store instead.
     */
    rom_exc_reset();
    TRY_AT(first, "sw $0, 2(%[p])");
    rom_assert_eq(7, 1, rom_exc.count);
    rom_assert_eq(8, ROM_EXC_ADES, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(9, first, rom_exc.epc);
    rom_assert_eq(10, (uint64_t)(unsigned long)&scratch[0] + 2,
                  rom_exc.badvaddr);

    /*
     * A doubleword load needs eight-byte alignment, so an address that
     * is a
     * multiple of four but not of eight still faults.
     */
    rom_exc_reset();
    TRY_AT(first, "ld %[j], 4(%[p])", [j] "=&r"(junk));
    rom_assert_eq(11, 1, rom_exc.count);
    rom_assert_eq(12, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));

    /* A delay-slot fault records the branch EPC and sets BD. */
    rom_exc_reset();
    TRY_AT(first,
           "beq $0, $0, 7f\n\t"
           " lw %[j], 1(%[p])\n"
           "7:\tnop",
           [j] "=&r"(junk));
    rom_assert_eq(13, 1, rom_exc.count);
    rom_assert_eq(14, ROM_EXC_ADEL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(15, ROM_CAUSE_BD, rom_exc.cause & ROM_CAUSE_BD);
    rom_assert_eq(16, first, rom_exc.epc);

    /*
     * A not-taken branch-likely nullifies its delay slot; no fault is
     * expected.
     */
    rom_exc_reset();
    TRY_AT(first,
           "beql $0, %[one], 7f\n\t"
           " lw %[j], 1(%[p])\n"
           "7:\tnop",
           [j] "=&r"(junk));
    rom_assert_eq(17, 0, rom_exc.count);

    /*
     * A breakpoint, a system call, a trap and an integer overflow all
     * reach
     * the
     * general entry point, each with its own exception code.
     */
    rom_exc_reset();
    TRY_AT(first, "break");
    rom_assert_eq(18, ROM_EXC_BP, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(19, first, rom_exc.epc);

    rom_exc_reset();
    TRY_AT(first, "syscall");
    rom_assert_eq(20, ROM_EXC_SYS, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    TRY_AT(first, "teq $0, $0");
    rom_assert_eq(21, ROM_EXC_TR, ROM_CAUSE_EXC(rom_exc.cause));

    /*
     * Adding one to the largest positive word overflows a signed add,
     * which
     * traps; the unsigned form of the same addition does not.
     */
    rom_exc_reset();
    TRY_AT(first,
           "lui %[j], 0x7fff\n\t"
           "ori %[j], %[j], 0xffff\n\t"
           "add %[j], %[j], %[one]",
           [j] "=&r"(junk));
    rom_assert_eq(22, 1, rom_exc.count);
    rom_assert_eq(23, ROM_EXC_OV, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    TRY_AT(first,
           "lui %[j], 0x7fff\n\t"
           "ori %[j], %[j], 0xffff\n\t"
           "addu %[j], %[j], %[one]",
           [j] "=&r"(junk));
    rom_assert_eq(24, 0, rom_exc.count);

    /*
     * Clearing the boot-exception-vector bit moves every entry point
     * from the
     * copies this ROM carries to the copies the startup code placed in
     * low
     * memory. The recorded entry point identifier loses its bootstrap
     * marker.
     */
    status = rom_get_status();
    rom_set_status(status & ~ROM_ST_BEV);
    rom_exc_reset();
    TRY_AT(first, "lw %[j], 1(%[p])", [j] "=&r"(junk));
    rom_assert_eq(25, 1, rom_exc.count);
    rom_assert_eq(26, ROM_VEC_GENERAL, rom_exc.vector);
    rom_set_status(status);

    /*
     * And restoring it puts them back.
     */
    rom_exc_reset();
    TRY_AT(first, "lw %[j], 1(%[p])", [j] "=&r"(junk));
    rom_assert_eq(27, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);

    /*
     * The unmapped address uses the refill vector with EXL clear and the
     * general vector with EXL set. Park the TLB to exclude stale hits.
     */
    rom_tlb_park(ROM_TLB_ENTRIES);

    rom_exc_reset();
    TRY_AT(first,
           "lui %[j], 0x0010\n\t"
           "lw %[j], 0(%[j])",
           [j] "=&r"(junk));
    rom_assert_eq(28, 1, rom_exc.count);
    rom_assert_eq(29, VEC_BOOTSTRAP_REFILL, rom_exc.vector);
    rom_assert_eq(30, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));

    rom_exc_reset();
    rom_set_status(status | ROM_ST_EXL);
    TRY_AT(first,
           "lui %[j], 0x0010\n\t"
           "lw %[j], 0(%[j])",
           [j] "=&r"(junk));
    /*
     * The handler's return path cleared the exception-level bit for us.
     */
    rom_assert_eq(31, 1, rom_exc.count);
    rom_assert_eq(32, VEC_BOOTSTRAP_GENERAL, rom_exc.vector);
    rom_assert_eq(33, ROM_EXC_TLBL, ROM_CAUSE_EXC(rom_exc.cause));
    rom_assert_eq(34, 0, rom_get_status() & ROM_ST_EXL);
    rom_set_status(status);

    /*
     * With ERL set, return uses ErrorEPC and clears ERL. The marker
     * distinguishes reaching the staged target from falling through.
     */
    __asm__ __volatile__(".set push\n\t.set noreorder\n\t"
                         "move %[r], $0\n\t"
                         "lui $26, %%hi(1f)\n\t"
                         "daddiu $26, $26, %%lo(1f)\n\t"
                         "dmtc0 $26, $30\n\t"       /* ErrorEPC */
                         "mfc0 $26, $12\n\t"        /* Status */
                         "ori $26, $26, 4\n\t"      /* error level */
                         "mtc0 $26, $12\n\t"
                         "eret\n\t"
                         "ori %[r], %[r], 2\n"
                         "1:\tori %[r], %[r], 1\n\t"
                         ".set pop"
                         : [r] "=&r"(reached)
                         :
                         : "$26");
    rom_assert_eq(35, 1, reached);
    rom_assert_eq(36, 0, rom_get_status() & ROM_ST_ERL);

    /*
     * A handled miss with EXL already set must preserve EPC; the handler
     * installs its own resume target, so this sentinel is never executed.
     */
    rom_set_epc(0x00000000abcd1230ull);
    rom_exc_reset();
    rom_set_status(status | ROM_ST_EXL);
    TRY_AT(first,
           "lui %[j], 0x0010\n\t"
           "lw %[j], 0(%[j])",
           [j] "=&r"(junk));
    rom_assert_eq(37, 1, rom_exc.count);
    rom_assert_eq(38, 0x00000000abcd1230ull, rom_exc.epc);
    rom_set_status(status);

    (void)junk;
}
