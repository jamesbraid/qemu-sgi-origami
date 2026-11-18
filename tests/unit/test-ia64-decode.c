/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Intel 245319-002, IA-64 SDM volume 3, revision 1.1 (July 2000).
 * Encodings below are assembled from the cited formats, not donor fixtures.
 * These tests decode instructions; they do not execute them.
 */

#include "qemu/osdep.h"
#include "target/ia64/decode/decode.h"

static void assert_instruction(const Ia64Instruction *insn, Ia64Opcode opcode,
                               IA64SlotUnit unit, uint64_t raw, unsigned qp,
                               unsigned slot)
{
    g_assert_true(insn->valid);
    g_assert_cmpint(insn->encoding_class, ==, IA64_ENCODING_DEFINED);
    g_assert_false(insn->placement_illegal);
    g_assert_false(insn->reserved_field);
    g_assert_cmpint(insn->opcode, ==, opcode);
    g_assert_cmpint(insn->unit, ==, unit);
    g_assert_cmphex(insn->raw, ==, raw);
    g_assert_cmpuint(insn->qp, ==, qp);
    g_assert_cmpuint(insn->slot, ==, slot);
    g_assert_cmphex(insn->address, ==, 0x2000);
}

static void test_bundle_fields(void)
{
    /* Figure 4-1, p. 4-1: template 4:0; slots 45:5, 86:46, 127:87. */
    for (unsigned bit = 0; bit < 128; bit++) {
        uint64_t low = bit < 64 ? UINT64_C(1) << bit : 0;
        uint64_t high = bit >= 64 ? UINT64_C(1) << (bit - 64) : 0;

        g_assert_cmpuint(ia64_bundle_template_code(low), ==,
                         bit < 5 ? 1U << bit : 0);
        for (unsigned slot = 0; slot < 3; slot++) {
            unsigned start = 5 + slot * 41;
            uint64_t expected = bit >= start && bit < start + 41
                                ? UINT64_C(1) << (bit - start) : 0;

            g_assert_cmphex(ia64_bundle_slot(low, high, slot), ==, expected);
        }
    }
    for (unsigned slot = 0; slot < 3; slot++) {
        g_assert_cmphex(ia64_bundle_slot(UINT64_MAX, UINT64_MAX, slot), ==,
                        UINT64_C(0x1ffffffffff));
    }
}

static void test_template_units(void)
{
    /* Table 4-2, p. 4-2. Stop-line placement is not asserted here. */
    static const IA64SlotUnit units[16][3] = {
        { IA64_UNIT_M, IA64_UNIT_I, IA64_UNIT_I },
        { IA64_UNIT_M, IA64_UNIT_I, IA64_UNIT_I },
        { IA64_UNIT_M, IA64_UNIT_L, IA64_UNIT_X },
        { IA64_UNIT_RESERVED, IA64_UNIT_RESERVED, IA64_UNIT_RESERVED },
        { IA64_UNIT_M, IA64_UNIT_M, IA64_UNIT_I },
        { IA64_UNIT_M, IA64_UNIT_M, IA64_UNIT_I },
        { IA64_UNIT_M, IA64_UNIT_F, IA64_UNIT_I },
        { IA64_UNIT_M, IA64_UNIT_M, IA64_UNIT_F },
        { IA64_UNIT_M, IA64_UNIT_I, IA64_UNIT_B },
        { IA64_UNIT_M, IA64_UNIT_B, IA64_UNIT_B },
        { IA64_UNIT_RESERVED, IA64_UNIT_RESERVED, IA64_UNIT_RESERVED },
        { IA64_UNIT_B, IA64_UNIT_B, IA64_UNIT_B },
        { IA64_UNIT_M, IA64_UNIT_M, IA64_UNIT_B },
        { IA64_UNIT_RESERVED, IA64_UNIT_RESERVED, IA64_UNIT_RESERVED },
        { IA64_UNIT_M, IA64_UNIT_F, IA64_UNIT_B },
        { IA64_UNIT_RESERVED, IA64_UNIT_RESERVED, IA64_UNIT_RESERVED },
    };

    for (unsigned code = 0; code < 32; code++) {
        const IA64TemplateInfo *info = ia64_template_info(code);

        g_assert_cmpint(info->defined, ==,
                        units[code / 2][0] != IA64_UNIT_RESERVED);
        for (unsigned slot = 0; slot < 3; slot++) {
            g_assert_cmpint(info->units[slot], ==, units[code / 2][slot]);
        }
    }
}

static void test_add_register(void)
{
    /* A1, p. 4-9: opcode 8, x4=0, x2b selects add or add-with-one. */
    static const unsigned regs[] = { 0, 1, 31, 32, 63, 64, 127 };
    static const IA64SlotUnit units[] = { IA64_UNIT_M, IA64_UNIT_I };

    for (unsigned u = 0; u < G_N_ELEMENTS(units); u++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(regs); i++) {
            unsigned r1 = regs[i];
            unsigned r2 = regs[(i + 1) % G_N_ELEMENTS(regs)];
            unsigned r3 = regs[(i + 2) % G_N_ELEMENTS(regs)];

            for (unsigned qp = 0; qp < 64; qp++) {
                for (unsigned carry = 0; carry < 2; carry++) {
                    uint64_t raw = (UINT64_C(8) << 37) |
                                   ((uint64_t)carry << 27) |
                                   ((uint64_t)r3 << 20) |
                                   ((uint64_t)r2 << 13) |
                                   ((uint64_t)r1 << 6) | qp;
                    Ia64Instruction insn = ia64_decode_insn(units[u], raw,
                                                           0x2000, u);

                    assert_instruction(&insn, carry ? IA64_OP_ADD_ONE
                                                    : IA64_OP_ADD,
                                       units[u], raw, qp, u);
                    g_assert_cmpuint(insn.operands.integer.destination, ==, r1);
                    g_assert_cmpuint(insn.operands.integer.source1, ==, r2);
                    g_assert_cmpuint(insn.operands.integer.source2, ==, r3);
                }
            }
        }
    }
}

static void test_add_immediate(void)
{
    /* A4, p. 4-10; immediate formation, Table 4-70, p. 4-79. */
    static const IA64SlotUnit units[] = { IA64_UNIT_M, IA64_UNIT_I };

    for (unsigned u = 0; u < G_N_ELEMENTS(units); u++) {
        for (int imm = -8192; imm <= 8191; imm++) {
            uint64_t field = (unsigned)imm & 0x3fff;
            uint64_t raw = (UINT64_C(8) << 37) | (UINT64_C(2) << 34) |
                           ((field >> 13) << 36) |
                           (((field >> 7) & 0x3f) << 27) |
                           (UINT64_C(127) << 20) |
                           ((field & 0x7f) << 13) | (UINT64_C(32) << 6) | 17;
            Ia64Instruction insn = ia64_decode_insn(units[u], raw, 0x2000, u);

            assert_instruction(&insn, IA64_OP_ADDS, units[u], raw, 17, u);
            g_assert_cmpuint(insn.operands.integer.destination, ==, 32);
            g_assert_cmpuint(insn.operands.integer.source2, ==, 127);
            g_assert_cmpint(insn.operands.integer.immediate, ==, imm);
        }
    }
}

static void test_branch_displacement(void)
{
    /* B1, p. 4-63; Table 4-70, p. 4-80: signed 21-bit field times 16. */
    static const int32_t displacements[] = {
        -16777216, -8388608, -32, -16, 0, 16, 32, 8388608, 16777200,
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(displacements); i++) {
        uint32_t field = (displacements[i] / 16) & 0x1fffff;

        for (unsigned qp = 0; qp < 64; qp++) {
            uint64_t raw = (UINT64_C(4) << 37) |
                           ((uint64_t)(field >> 20) << 36) |
                           ((uint64_t)(field & 0xfffff) << 13) | qp;
            Ia64Instruction insn = ia64_decode_insn(IA64_UNIT_B, raw,
                                                   0x2000, 2);

            assert_instruction(&insn, IA64_OP_BR_COND, IA64_UNIT_B, raw, qp, 2);
            g_assert_cmpint(insn.operands.branch.displacement, ==,
                            displacements[i]);
        }
    }
}

static void test_break_immediate(void)
{
    /* I19/M37/B9/F15, pp. 4-28/4-55/4-67/4-76; BREAK, p. 2-17. */
    static const IA64SlotUnit units[] = {
        IA64_UNIT_M, IA64_UNIT_I, IA64_UNIT_B, IA64_UNIT_F,
    };
    static const uint32_t immediates[] = {
        0, 1, 0xfffff, 0x100000, 0x100001, 0x100002,
        0x100003, 0x100004, 0x1fffff,
    };

    for (unsigned u = 0; u < G_N_ELEMENTS(units); u++) {
        unsigned slot = units[u] == IA64_UNIT_B ? 2 : 0;

        for (unsigned i = 0; i < G_N_ELEMENTS(immediates); i++) {
            for (unsigned qp = 0; qp < 64; qp++) {
                uint64_t raw = ((uint64_t)(immediates[i] >> 20) << 36) |
                               ((uint64_t)(immediates[i] & 0xfffff) << 6) | qp;
                Ia64Instruction insn = ia64_decode_insn(units[u], raw,
                                                       0x2000, slot);

                assert_instruction(&insn, IA64_OP_BREAK, units[u], raw, qp,
                                   slot);
                g_assert_cmpint(insn.operands.system.immediate, ==,
                                units[u] == IA64_UNIT_B ? 0 : immediates[i]);
            }
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ia64/intel-245319-002/bundle-fields", test_bundle_fields);
    g_test_add_func("/ia64/intel-245319-002/template-units",
                    test_template_units);
    g_test_add_func("/ia64/intel-245319-002/add-register", test_add_register);
    g_test_add_func("/ia64/intel-245319-002/add-immediate", test_add_immediate);
    g_test_add_func("/ia64/intel-245319-002/branch-displacement",
                    test_branch_displacement);
    g_test_add_func("/ia64/intel-245319-002/break-immediate",
                    test_break_immediate);
    return g_test_run();
}
