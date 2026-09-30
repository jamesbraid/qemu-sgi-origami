/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Check that untaken branch-likely instructions nullify their delay
 * slots and that the and-link forms still set the return address.
 * See the MIPS64 instruction set reference, MD00087 revision 6.06.
 */

#include "rom.h"

const char rom_name[] = "branchlikely";

#define BL_TAKEN     3
#define BL_NULLIFIED 0

/*
 * Two-register conditional branches: beq, bne and their likely forms.
 */
#define PROBE2(mnemonic, va, vb)                                              \
    ({                                                                        \
        uint64_t res__;                                                       \
        uint64_t a__ = (va), b__ = (vb);                                      \
        __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                \
                             "move %[r], $0\n\t"                              \
                             mnemonic " %[a], %[b], 3f\n\t"                   \
                             " ori %[r], %[r], 1\n\t"                         \
                             "b 4f\n\t"                                       \
                             " nop\n"                                         \
                             "3:\tori %[r], %[r], 2\n"                        \
                             "4:\t.set pop"                                   \
                             : [r] "=&r"(res__)                               \
                             : [a] "r"(a__), [b] "r"(b__));                   \
        res__;                                                                \
    })

/*
 * One-register conditional branches: blez, bgtz, bltz, bgez and their
 * likely
 * forms.
 */
#define PROBE1(mnemonic, va)                                                  \
    ({                                                                        \
        uint64_t res__;                                                       \
        uint64_t a__ = (va);                                                  \
        __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                \
                             "move %[r], $0\n\t"                              \
                             mnemonic " %[a], 3f\n\t"                         \
                             " ori %[r], %[r], 1\n\t"                         \
                             "b 4f\n\t"                                       \
                             " nop\n"                                         \
                             "3:\tori %[r], %[r], 2\n"                        \
                             "4:\t.set pop"                                   \
                             : [r] "=&r"(res__)                               \
                             : [a] "r"(a__));                                 \
        res__;                                                                \
    })

/*
 * The and-link forms, which also report where they left the return
 * address.
 * Register 31 is saved and put back because the caller's return address
 * lives
 * there. The expected link is the address of the instruction after the
 * delay
 * slot, which the probe captures as a label so the check does not
 * depend on
 * how the compiler laid the block out.
 */
#define PROBE_LINK(mnemonic, va, link_out, want_out)                          \
    ({                                                                        \
        uint64_t res__, save__;                                               \
        uint64_t a__ = (va);                                                  \
        __asm__ __volatile__(".set push\n\t.set noreorder\n\t"                \
                             "move %[r], $0\n\t"                              \
                             "move %[sv], $31\n\t"                            \
                             "lui %[want], %%hi(2f)\n\t"                      \
                             "daddiu %[want], %[want], %%lo(2f)\n\t"          \
                             mnemonic " %[a], 3f\n\t"                         \
                             " ori %[r], %[r], 1\n"                           \
                             "2:\tb 4f\n\t"                                   \
                             " nop\n"                                         \
                             "3:\tori %[r], %[r], 2\n"                        \
                             "4:\tmove %[lk], $31\n\t"                        \
                             "move $31, %[sv]\n\t"                            \
                             ".set pop"                                       \
                             : [r] "=&r"(res__), [lk] "=&r"(link_out),        \
                               [want] "=&r"(want_out), [sv] "=&r"(save__)     \
                             : [a] "r"(a__)                                   \
                             : "$31");                                        \
        res__;                                                                \
    })

void rom_main(void)
{
    uint64_t link, want;

    /*
     * An ordinary branch executes its delay slot either way: falling
     * through
     * gives 1, not 0, which is what the likely forms are contrasted
     * against.
     */
    rom_assert_eq(1, 1, PROBE2("beq", 1, 2));
    rom_assert_eq(2, BL_TAKEN, PROBE2("beq", 7, 7));
    rom_assert_eq(3, 1, PROBE2("bne", 7, 7));
    rom_assert_eq(4, BL_TAKEN, PROBE2("bne", 1, 2));

    /*
     * Branch on equal likely.
     */
    rom_assert_eq(5, BL_NULLIFIED, PROBE2("beql", 1, 2));
    rom_assert_eq(6, BL_TAKEN, PROBE2("beql", 7, 7));

    /*
     * Branch on not equal likely.
     */
    rom_assert_eq(7, BL_NULLIFIED, PROBE2("bnel", 7, 7));
    rom_assert_eq(8, BL_TAKEN, PROBE2("bnel", 1, 2));

    /*
     * The comparisons against zero are 64-bit, so a value whose low 32
     * bits
     * look negative but whose sign bit is clear counts as greater than
     * zero.
     */
    rom_assert_eq(9, BL_NULLIFIED, PROBE1("blezl", 1));
    rom_assert_eq(10, BL_TAKEN, PROBE1("blezl", 0));
    rom_assert_eq(11, BL_TAKEN, PROBE1("blezl", ~0ull));

    rom_assert_eq(12, BL_NULLIFIED, PROBE1("bgtzl", 0));
    rom_assert_eq(13, BL_TAKEN, PROBE1("bgtzl", 1));
    rom_assert_eq(14, BL_TAKEN, PROBE1("bgtzl", 0x0000000080000000ull));

    rom_assert_eq(15, BL_NULLIFIED, PROBE1("bltzl", 0));
    rom_assert_eq(16, BL_TAKEN, PROBE1("bltzl", ~0ull));
    rom_assert_eq(17, BL_NULLIFIED, PROBE1("bltzl", 0x0000000080000000ull));

    rom_assert_eq(18, BL_NULLIFIED, PROBE1("bgezl", ~0ull));
    rom_assert_eq(19, BL_TAKEN, PROBE1("bgezl", 0));

    /*
     * The and-link likely forms nullify the delay slot but link
     * regardless.
     */
    rom_assert_eq(20, BL_NULLIFIED, PROBE_LINK("bltzall", 0, link, want));
    rom_assert_eq(21, want, link);
    rom_assert_eq(22, BL_TAKEN, PROBE_LINK("bltzall", ~0ull, link, want));
    rom_assert_eq(23, want, link);

    rom_assert_eq(24, BL_NULLIFIED, PROBE_LINK("bgezall", ~0ull, link, want));
    rom_assert_eq(25, want, link);
    rom_assert_eq(26, BL_TAKEN, PROBE_LINK("bgezall", 0, link, want));
    rom_assert_eq(27, want, link);
}
