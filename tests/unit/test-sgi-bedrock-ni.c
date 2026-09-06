/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-ni-regs.h"

static uint64_t read_reg(SGIBedrockNIRegs *s, uint64_t offset)
{
    uint64_t value;

    g_assert_true(sgi_bedrock_ni_regs_read(s, offset, &value));
    return value;
}

static void test_controls(void)
{
    SGIBedrockNIRegs s = { 0 };
    static const struct {
        uint64_t offset;
        uint64_t mask;
    } controls[] = {
        { 0x10, 1 }, { 0x18, 7 }, { 0x20, 0x1ff },
        { 0x30, 0x7efffffffULL }, { 0x38, 0xffff },
    };

    for (size_t i = 0; i < ARRAY_SIZE(controls); i++) {
        uint64_t offset = controls[i].offset;
        uint64_t mask = controls[i].mask;

        g_assert_cmpint(sgi_bedrock_ni_regs_write(&s, offset, UINT64_MAX),
                        ==, SGI_BEDROCK_NI_WRITTEN);
        g_assert_cmphex(read_reg(&s, offset), ==, mask);
        sgi_bedrock_ni_regs_write(&s, offset, 0xaaaaaaaaaaaaaaaaULL);
        g_assert_cmphex(read_reg(&s, offset), ==,
                        mask & 0xaaaaaaaaaaaaaaaaULL);
        sgi_bedrock_ni_regs_write(&s, offset, 0);
        g_assert_cmphex(read_reg(&s, offset), ==, 0);
    }
}

static void test_reset_request(void)
{
    SGIBedrockNIRegs s = { 0 };

    for (unsigned enable = 0; enable < 2; enable++) {
        sgi_bedrock_ni_regs_write(&s, 0x10, enable);
        for (unsigned command = 0; command < 8; command++) {
            g_assert_cmpint(sgi_bedrock_ni_regs_write(&s, 8, command), ==,
                command & 4 ? SGI_BEDROCK_NI_LOCAL_RESET :
                              SGI_BEDROCK_NI_WRITTEN);
        }
    }
}

static void test_routing_storage(void)
{
    SGIBedrockNIRegs s = { 0 };

    for (unsigned i = 0; i < 128; i++) {
        sgi_bedrock_ni_regs_write(&s, 0x1000 + i * 8, 0xe0 | (i % 32));
    }
    sgi_bedrock_ni_regs_write(&s, 0x2000, 0xf3);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(read_reg(&s, 0x1000 + i * 8), ==, i % 32);
    }
    g_assert_cmphex(read_reg(&s, 0x2000), ==, 0x13);
}

static void test_disconnected(void)
{
    SGIBedrockNIRegs s = { 0 };

    sgi_bedrock_ni_regs_write(&s, 0, UINT64_MAX);
    sgi_bedrock_ni_regs_write(&s, 0x18, 7);
    g_assert_cmphex(read_reg(&s, 0), ==, 0);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 0);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 0);
    g_assert_cmphex(read_reg(&s, 0x120), ==, 0);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 0);
}

static void test_error_clear(void)
{
    /* Fixture state, not an emulated link event or generated error count. */
    SGIBedrockNIRegs s = { .errors = 1ULL << 52 };

    g_assert_cmphex(read_reg(&s, 0x100), ==, 1ULL << 52);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 1ULL << 52);
    g_assert_cmphex(read_reg(&s, 0x120), ==, 1ULL << 52);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 0);
}

static void test_reset_retention(void)
{
    /* Seed retained state as a migration load could; no link event is faked. */
    SGIBedrockNIRegs s = { .sideband = 0xabcde };

    sgi_bedrock_ni_regs_write(&s, 0x38, 0xa55a);
    sgi_bedrock_ni_regs_reset(&s);
    g_assert_cmphex(read_reg(&s, 0x118), ==, 0xabcde);
    /* Unknown control reset values are not hardware assertions. */
}

static void test_read_only_writes(void)
{
    /* Seeded fixture state; no link event is faked. */
    SGIBedrockNIRegs s = {
        .errors = 1ULL << 30, .header = { 0x11, 0x22 }, .sideband = 0x33,
    };
    static const uint64_t offsets[] = { 0, 0x100, 0x108, 0x110, 0x118, 0x120 };

    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_cmpint(sgi_bedrock_ni_regs_write(&s, offsets[i], UINT64_MAX),
                        ==, SGI_BEDROCK_NI_READ_ONLY);
    }
    g_assert_cmphex(read_reg(&s, 0), ==, 0);
    g_assert_cmphex(read_reg(&s, 0x100), ==, 1ULL << 30);
    g_assert_cmphex(read_reg(&s, 0x108), ==, 0x11);
    g_assert_cmphex(read_reg(&s, 0x110), ==, 0x22);
    g_assert_cmphex(read_reg(&s, 0x118), ==, 0x33);
}

/*
 * Port-error bits 52:24 raise the interrupt and the byte counters below
 * them do not. Each word is fixture state naming one bit, not a link event.
 */
static void test_error_level(void)
{
    static const struct {
        uint64_t errors;
        bool level;
    } cases[] = {
        { 0, false },
        { 1ULL << 52, true },   /* link reset in, the top interrupting bit */
        { 1ULL << 24, true },   /* tail timeout count, the bottom one */
        { 1ULL << 53, false },  /* reserved, above the field */
        { 1ULL << 23, false },  /* retry count, a byte counter below it */
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        SGIBedrockNIRegs s = { .errors = cases[i].errors };

        g_assert_cmpint(sgi_bedrock_ni_regs_error_level(&s), ==,
                        cases[i].level);
    }
}

static void test_unimplemented(void)
{
    SGIBedrockNIRegs s = { 0 };
    uint64_t value;
    static const uint64_t offsets[] = { 1, 0x28, 0x40, 0x1400, 0x2008 };

    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_false(sgi_bedrock_ni_regs_read(&s, offsets[i], &value));
        g_assert_cmpint(sgi_bedrock_ni_regs_write(&s, offsets[i], 1),
                        ==, SGI_BEDROCK_NI_UNIMPLEMENTED);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-bedrock-ni/controls", test_controls);
    g_test_add_func("/sgi-bedrock-ni/reset-request", test_reset_request);
    g_test_add_func("/sgi-bedrock-ni/routing-storage", test_routing_storage);
    g_test_add_func("/sgi-bedrock-ni/disconnected", test_disconnected);
    g_test_add_func("/sgi-bedrock-ni/error-clear", test_error_clear);
    g_test_add_func("/sgi-bedrock-ni/reset-retention", test_reset_retention);
    g_test_add_func("/sgi-bedrock-ni/read-only-writes", test_read_only_writes);
    g_test_add_func("/sgi-bedrock-ni/error-level", test_error_level);
    g_test_add_func("/sgi-bedrock-ni/unimplemented", test_unimplemented);
    return g_test_run();
}
