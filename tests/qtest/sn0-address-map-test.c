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
#include "qemu/units.h"
#include "libqtest.h"

/* Node zero's window, and the space the Hub answers uncached references in. */
#define NODE0           0x0000000000ULL
#define SPACE_HUB       0x10000000000ULL

/* The flash, in the two places a guest reaches it. */
#define FLASH_RESET     (NODE0 + 0x1fc00000)
#define FLASH_HUB       (SPACE_HUB + NODE0 + 0x10000000)

/*
 * The directory and error-correction back doors. Real firmware's own memory
 * and directory diagnostic writes a pattern through here and reads it back
 * to test for stuck bits; nothing about coherence or ECC is modelled, so the
 * only property that has to hold is the round trip itself.
 */
#define BACKDOOR_ECC    (SPACE_HUB + NODE0 + 0x80000000ULL)
#define BACKDOOR_DIR    (SPACE_HUB + NODE0 + 0xc0000000ULL)
#define BACKDOOR_SIZE   0x40000000ULL

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

/*
 * The Hub special view of the flash, LBOOT, is 256 MB decoding a 1 MB chip:
 * real firmware reaches its own code through offsets in this window well past
 * the flash's real size, and a real chip-select ignores the extra bits and
 * repeats. This is the case `test_flash_two_views` cannot reach, since offset
 * zero would pass even with a window sized to the flash alone.
 */
static void test_lboot_mirrors_the_flash(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t direct, mirrored;

    /* One repeat period past the flash's own size: the first wrap. */
    direct = qtest_readq(qts, FLASH_RESET + CONFIG_OFFSET);
    mirrored = qtest_readq(qts, FLASH_HUB + 0x100000 + CONFIG_OFFSET);
    g_assert_cmphex(mirrored, ==, direct);

    /* Deep into the window, the offset this project's own D1 trace hit. */
    direct = qtest_readl(qts, FLASH_RESET + 0x2a3c);
    mirrored = qtest_readl(qts, FLASH_HUB + 0xfc02a3c);
    g_assert_cmphex(mirrored, ==, direct);

    /* The last aligned double word of the 256 MB window still wraps. */
    direct = qtest_readq(qts, FLASH_RESET + (0x10000000 - 8) % 0x100000);
    mirrored = qtest_readq(qts, FLASH_HUB + 0x10000000 - 8);
    g_assert_cmphex(mirrored, ==, direct);

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

/* One bank's real stride, matching hw/mips/sn0.h's SN0_MEM_BANK_SIZE. */
#define MEM_BANK_SIZE   0x20000000ULL

/*
 * A config that spans two banks unevenly: 768 MiB is one full 512 MiB bank
 * plus a 256 MiB remainder in the second. -m 512M alone (test_node_memory,
 * above) and any single-bank size take the machine's other branch entirely
 * — one region, no loop — so neither one ever runs the per-bank alias loop
 * this checks. A size that is an exact multiple of two full banks would run
 * the loop but never exercise MIN(remaining, SN0_MEM_BANK_SIZE) actually
 * clamping to less than a full bank; an uneven second bank is the smallest
 * config where both that clamp and the second bank's alias offset into
 * machine->ram (bank_offset = ram_size - remaining, not i * SN0_MEM_BANK_SIZE)
 * have to be right for both writes below to read back correctly.
 */
static void test_multi_bank_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 768M");

    /* Bank 0 backed for its own full 512 MiB, to the last double word. */
    qtest_writeq(qts, NODE0 + MEM_BANK_SIZE - 8, 0xb0b0b0b0b0b0b0b0ULL);
    g_assert_cmphex(qtest_readq(qts, NODE0 + MEM_BANK_SIZE - 8), ==,
                    0xb0b0b0b0b0b0b0b0ULL);

    /* Bank 1 starts at its own base, one full bank stride up. */
    qtest_writeq(qts, NODE0 + MEM_BANK_SIZE, 0xb1b1b1b1b1b1b1b1ULL);
    g_assert_cmphex(qtest_readq(qts, NODE0 + MEM_BANK_SIZE), ==,
                    0xb1b1b1b1b1b1b1b1ULL);

    /*
     * Bank 1 backed for only its real 256 MiB (768 MiB total minus the 512
     * MiB bank 0 already took), to the last double word of that -- not the
     * full 512 MiB bank 1's own stride would otherwise allow.
     */
    qtest_writeq(qts, NODE0 + MEM_BANK_SIZE + 256 * MiB - 8,
                0xb2b2b2b2b2b2b2b2ULL);
    g_assert_cmphex(qtest_readq(qts, NODE0 + MEM_BANK_SIZE + 256 * MiB - 8),
                    ==, 0xb2b2b2b2b2b2b2b2ULL);

    /*
     * The three patterns above land in three different bytes of the one
     * underlying machine->ram object once bank_offset is right (0, 512 MiB
     * minus 8, and 768 MiB minus 8 into it) -- reading bank 0's pattern back
     * unchanged after writing bank 1's is what catches bank 1 having been
     * aliased onto the same bytes as bank 0 instead of its own.
     */
    g_assert_cmphex(qtest_readq(qts, NODE0 + MEM_BANK_SIZE - 8), ==,
                    0xb0b0b0b0b0b0b0b0ULL);

    qtest_quit(qts);
}

/*
 * Write a pattern at a handful of addresses across a back-door window and
 * confirm each reads back unchanged. The offsets are the start of the
 * window, one in the middle, and the last aligned double word, rather than
 * one address at the base, because a region sized wrong at the top is a
 * different bug from one that never held state at all.
 */
static void check_backdoor_round_trip(QTestState *qts, uint64_t base)
{
    static const uint64_t offsets[] = { 0x0, 0x12345678, BACKDOOR_SIZE - 8 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t addr = base + offsets[i];
        uint64_t pattern = 0xdaedbaefdaedbaefULL ^ offsets[i];

        qtest_writeq(qts, addr, pattern);
        g_assert_cmphex(qtest_readq(qts, addr), ==, pattern);
    }
}

static void test_directory_backdoor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_round_trip(qts, BACKDOOR_DIR);

    qtest_quit(qts);
}

static void test_ecc_backdoor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_round_trip(qts, BACKDOOR_ECC);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/address-map/config-block", NULL,
                        test_config_block);
    qtest_add_data_func("sn0/address-map/flash-two-views", NULL,
                        test_flash_two_views);
    qtest_add_data_func("sn0/address-map/lboot-mirrors-the-flash", NULL,
                        test_lboot_mirrors_the_flash);
    qtest_add_data_func("sn0/address-map/entry-points", NULL,
                        test_entry_points);
    qtest_add_data_func("sn0/address-map/flash-read-only", NULL,
                        test_flash_is_read_only);
    qtest_add_data_func("sn0/address-map/node-memory", NULL, test_node_memory);
    qtest_add_data_func("sn0/address-map/multi-bank-memory", NULL,
                        test_multi_bank_memory);
    qtest_add_data_func("sn0/address-map/directory-backdoor", NULL,
                        test_directory_backdoor);
    qtest_add_data_func("sn0/address-map/ecc-backdoor", NULL,
                        test_ecc_backdoor);

    return g_test_run();
}
