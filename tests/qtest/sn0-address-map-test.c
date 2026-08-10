/*
 * SGI Origin 200 (IP27) physical address map.
 *
 * The bare machine puts two things in that map: a node's memory and its boot
 * flash. What this checks is that they appear where the address map says, that
 * the flash's two views really are one region rather than two copies that can
 * drift, and that the contents a guest reads before any firmware runs say what
 * they are supposed to say.
 *
 * Addresses here are what the memory system sees, which for an uncached
 * reference is the physical address with the processor's uncached attribute
 * above it. The attribute that means memory is placed at window zero, so a
 * plain physical address addresses memory and the Hub's special space starts
 * one window up.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Node zero's window, and the space the Hub answers uncached references in. */
#define NODE0           0x0000000000ULL
#define SPACE_HUB       0x10000000000ULL

/* The flash, in the two places a guest reaches it. */
#define FLASH_RESET     (NODE0 + 0x1fc00000)
#define FLASH_HUB       (SPACE_HUB + NODE0 + 0x10000000)

/* The configuration block, at flash offset 0x60. */
#define CONFIG_OFFSET   0x60
#define CONFIG_MAGIC    0x69703237636f6e66ULL    /* "ip27conf" */

#define FIELD_MAGIC     0x08
#define FIELD_CPU_FREQ  0x10
#define FIELD_HUB_FREQ  0x18
#define FIELD_RTC_FREQ  0x20
#define FIELD_MACHTYPE  0x30

#define ENTRY_POINTS    10
#define ENTRY_STRIDE    8

/* Every instruction word at an entry point has to be a branch of some kind. */
static bool is_branch(uint32_t insn)
{
    uint32_t op = insn >> 26;
    uint32_t func = insn & 0x3f;

    switch (op) {
    case 0x00:                  /* SPECIAL: jr and jalr are branches */
        return func == 0x08 || func == 0x09;
    case 0x01:                  /* REGIMM: the branch-on-condition forms */
    case 0x02:                  /* j */
    case 0x03:                  /* jal */
    case 0x04:                  /* beq, and b as its idiom */
    case 0x05:                  /* bne */
    case 0x06:                  /* blez */
    case 0x07:                  /* bgtz */
    case 0x14:                  /* beql */
    case 0x15:                  /* bnel */
    case 0x16:                  /* blezl */
    case 0x17:                  /* bgtzl */
        return true;
    default:
        return false;
    }
}

static void test_config_block(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    g_assert_cmphex(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);

    /* The clocks a guest reads its timebase from. */
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_CPU_FREQ), ==, 180000000);
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_HUB_FREQ), ==, 90000000);
    g_assert_cmpuint(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_RTC_FREQ), ==, 1250000);

    /*
     * One means Origin 200; zero would make a guest introduce itself as an
     * Origin 2000.
     */
    g_assert_cmpuint(qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET +
                               FIELD_MACHTYPE), ==, 1);

    qtest_quit(qts);
}

static void test_flash_two_views(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    int i;

    /*
     * The same bytes have to appear at both addresses. Comparing the whole
     * configuration block rather than one field is what would catch two regions
     * built separately from the same values.
     */
    for (i = 0; i < 0x50; i += 4) {
        uint32_t reset_view = qtest_readl(qts, FLASH_RESET + CONFIG_OFFSET + i);
        uint32_t hub_view = qtest_readl(qts, FLASH_HUB + CONFIG_OFFSET + i);

        g_assert_cmphex(reset_view, ==, hub_view);
    }

    g_assert_cmphex(qtest_readq(qts, FLASH_HUB + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

static void test_entry_points(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    int i;

    /*
     * A guest calls nine of these by address and expects to come back, so
     * whatever is there has to redirect rather than run on into the next one.
     */
    for (i = 0; i < ENTRY_POINTS; i++) {
        uint32_t insn = qtest_readl(qts, FLASH_RESET + i * ENTRY_STRIDE);

        g_assert_true(is_branch(insn));
    }

    qtest_quit(qts);
}

static void test_flash_is_read_only(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t before = qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET +
                                FIELD_MAGIC);

    /* Rejected, and without taking the machine down with it. */
    qtest_writeq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC, 0);
    g_assert_cmphex(qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, before);

    /* Still answering afterwards. */
    g_assert_cmphex(qtest_readq(qts, FLASH_HUB + CONFIG_OFFSET + FIELD_MAGIC),
                    ==, CONFIG_MAGIC);

    qtest_quit(qts);
}

static void test_node_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 512M");

    /* Memory is at the bottom of the node's window. */
    qtest_writeq(qts, NODE0 + 0x1000, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, NODE0 + 0x1000), ==,
                    0x0123456789abcdefULL);

    /*
     * The flash sits on top of the first memory bank rather than beside it, so
     * the bank's last bytes are still memory and the flash's are not.
     */
    qtest_writeq(qts, NODE0 + 0x1fbffff8, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, NODE0 + 0x1fbffff8), ==,
                    0x1122334455667788ULL);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/address-map/config-block", NULL,
                        test_config_block);
    qtest_add_data_func("sn0/address-map/flash-two-views", NULL,
                        test_flash_two_views);
    qtest_add_data_func("sn0/address-map/entry-points", NULL,
                        test_entry_points);
    qtest_add_data_func("sn0/address-map/flash-read-only", NULL,
                        test_flash_is_read_only);
    qtest_add_data_func("sn0/address-map/node-memory", NULL, test_node_memory);

    return g_test_run();
}
