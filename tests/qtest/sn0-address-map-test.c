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

/* The default synthesized graph's eight plain-megabyte bank counts. */
#define GRAPH_BANK_SIZES    0x453c

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

static void check_ram(QTestState *qts, uint64_t addr, uint64_t pattern)
{
    qtest_writeq(qts, addr, pattern);
    g_assert_cmphex(qtest_readq(qts, addr), ==, pattern);
}

static void check_not_ram(QTestState *qts, uint64_t addr)
{
    uint64_t before = qtest_readq(qts, addr);

    qtest_writeq(qts, addr, ~before);
    g_assert_cmphex(qtest_readq(qts, addr), ==, before);
}

/*
 * An IP27 bank of at most 128 MiB occupies the first 128 MiB slot. A larger
 * bank is interleaved evenly over all four slots on a 128 MiB stride.
 */
static void test_128m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 128M");

    check_ram(qts, NODE0 + 0x1000, 0x0123456789abcdefULL);
    check_ram(qts, NODE0 + 128 * MiB - 8, 0x1281281281281281ULL);
    check_not_ram(qts, NODE0 + 128 * MiB);

    qtest_quit(qts);
}

/* One bank's stride, matching hw/mips/sn0.h's SN0_MEM_BANK_SIZE. */
#define MEM_BANK_SIZE   0x20000000ULL
#define MEM_SLOT_SIZE   0x08000000ULL

/*
 * A 256 MiB bank is four 64 MiB runs, not one contiguous run. This is the
 * layout both supported guests derive from the graph's per-bank size.
 */
static void test_256m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 256M");
    unsigned int slot;

    for (slot = 0; slot < 4; slot++) {
        check_ram(qts, NODE0 + slot * MEM_SLOT_SIZE,
                  0x2560000000000000ULL + slot);
        check_ram(qts, NODE0 + slot * MEM_SLOT_SIZE + 64 * MiB - 8,
                  0x256fffffffffff00ULL + slot);
        check_not_ram(qts, NODE0 + slot * MEM_SLOT_SIZE + 64 * MiB);
    }

    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 2), ==, 0);

    qtest_quit(qts);
}

/*
 * The explicit 512 MiB configuration is two 256 MiB banks. That keeps the
 * flash and synthesized ARCS regions in bank zero's final gap instead of
 * advertising their addresses as RAM.
 */
static void test_512m_memory(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 512M");
    unsigned int bank, slot;

    for (bank = 0; bank < 2; bank++) {
        for (slot = 0; slot < 4; slot++) {
            check_ram(qts, NODE0 + bank * MEM_BANK_SIZE +
                      slot * MEM_SLOT_SIZE,
                      0x5120000000000000ULL + bank * 4 + slot);
        }
    }

    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 2), ==, 256);
    g_assert_cmpuint(qtest_readw(qts, NODE0 + GRAPH_BANK_SIZES + 4), ==, 0);
    check_not_ram(qts, NODE0 + 0x1f800000);
    check_not_ram(qts, NODE0 + 0x1fc00000);
    check_not_ram(qts, NODE0 + 0x1fd00000);

    qtest_quit(qts);
}

/*
 * Hub-special offset zero is a 256 MiB view of the reading processor's local
 * node memory.  It must preserve the fitted bank's sparse slot layout rather
 * than turn the whole window into RAM.  The first address is the physical page
 * to which the real PROM maps its uncached post-relocation instruction stream.
 */
static void test_hub_local_memory_alias(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    uint64_t prom_copy = 0x01a0a520;

    qtest_writeq(qts, prom_copy, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(qts, SPACE_HUB + prom_copy), ==,
                    0x0123456789abcdefULL);
    qtest_writeq(qts, SPACE_HUB + prom_copy, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(qts, prom_copy), ==,
                    0xfedcba9876543210ULL);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 256M");
    check_ram(qts, SPACE_HUB + 0x1000, 0x1000000000000001ULL);
    check_not_ram(qts, SPACE_HUB + 64 * MiB);
    check_ram(qts, SPACE_HUB + 128 * MiB + 0x1000,
              0x1000000000000002ULL);
    check_not_ram(qts, SPACE_HUB + 192 * MiB);
    qtest_quit(qts);
}

static void test_invalid_memory_size(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin200", "-m", "96M",
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL
    };
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output,
                           "cannot be populated from IP29 memory banks"));
}

/*
 * One bank's stride within a back-door window: an even eight-way split of
 * the 1 GB window, matching hw/mips/sn0.h's SN0_BACKDOOR_BANK_STRIDE.
 */
#define BACKDOOR_BANK_STRIDE   0x8000000ULL  /* 128 MiB */

/*
 * The firmware's own classification pattern: 0x55 repeated across a
 * doubleword. Real firmware's presence probe writes this to an entry,
 * writes zero eight bytes further on (never compared against anything,
 * there only to disturb the shared data lines), and reads the first
 * location back -- premium format if the low 48 bits survived, standard
 * format if only the low 16 did, absent if neither did.
 */
#define BACKDOOR_PATTERN       0x5555555555555555ULL
#define BACKDOOR_PATTERN_LOW16 0x5555ULL
#define BACKDOOR_PATTERN_LOW48 0x0000555555555555ULL

/*
 * A populated entry's real storage is only as wide as this machine's
 * directory format: the low sixteen bits of each eight-byte-aligned entry
 * round-trip, and the other six do not survive being written, whatever
 * they were written as. Checked at the bank's own base, one entry into the
 * middle of it, and its last aligned entry -- rather than one address at
 * the base -- because a populated region sized wrong at its own top is a
 * different bug from one that never held state at all. The pattern is
 * folded with the bank index so that two banks written in the same test
 * cannot pass by aliasing onto each other's bytes.
 */
static void check_backdoor_entry(QTestState *qts, uint64_t base,
                                 unsigned int bank)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    static const uint64_t offsets[] = { 0x0, 0x1234560,
                                        BACKDOOR_BANK_STRIDE - 8 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(offsets); i++) {
        uint64_t addr = bank_base + offsets[i];
        uint64_t written = (0xdaedbaefdaedba00ULL ^ offsets[i]) + bank;
        uint64_t readback;

        qtest_writeq(qts, addr, written);
        readback = qtest_readq(qts, addr);

        g_assert_cmphex(readback & 0xffffULL, ==, written & 0xffffULL);
        g_assert_cmphex(readback & ~0xffffULL, ==, 0);
    }
}

/*
 * Real firmware's own presence probe, run against a bank this machine
 * populates: write the pattern to the entry, write zero eight bytes on,
 * read the entry back. Assert the classification the probe reaches --
 * standard format, this machine's own, per SGI's manual stating the
 * Origin 200 carries no premium directory memory -- not the value that
 * produces it. The low sixteen bits are the part the contract fixes; the
 * rest is only required to disagree with the wide mask, so this checks
 * that disagreement rather than a specific value.
 */
static void check_backdoor_standard_format(QTestState *qts, uint64_t addr)
{
    uint64_t readback;

    qtest_writeq(qts, addr, BACKDOOR_PATTERN);
    qtest_writeq(qts, addr + 8, 0);
    readback = qtest_readq(qts, addr);

    g_assert_cmphex(readback & 0xffffULL, ==, BACKDOOR_PATTERN_LOW16);
    g_assert_cmphex(readback & 0xffffffffffffULL, !=, BACKDOOR_PATTERN_LOW48);
}

/*
 * Real firmware's own presence probe, run against a bank this machine does
 * not populate: write the pattern, write zero eight bytes on, read back.
 * It calls a bank present only when the pattern survives; for a bank that
 * does not exist, real hardware has nothing behind either write to land
 * in, so the low sixteen bits -- the part either format needs to survive
 * -- must not come back as the pattern wrote them. Assert the
 * classification the probe reaches, not what the location actually reads
 * as: that value is this model's own arbitrary choice (recorded beside
 * sn0_backdoor_absent_ops in hw/mips/sn0.c), and asserting it would freeze
 * a modelling choice as though it were a contract.
 */
static void check_backdoor_absent(QTestState *qts, uint64_t addr)
{
    uint64_t readback;

    qtest_writeq(qts, addr, BACKDOOR_PATTERN);
    qtest_writeq(qts, addr + 8, 0);
    readback = qtest_readq(qts, addr);

    g_assert_cmphex(readback & 0xffffULL, !=, BACKDOOR_PATTERN_LOW16);
}

/*
 * The back door is the ordinary memory address shifted right by two.  A
 * populated bank therefore has one quarter as much unique directory or ECC
 * storage as ordinary memory, repeated across its fixed 128 MiB share of the
 * back-door window.  Firmware relies on this aliasing to size the bank.
 */
static void check_backdoor_bank_span(QTestState *qts, uint64_t base,
                                     unsigned int bank, uint64_t span)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    uint64_t first = bank_base + 0x1000;
    uint64_t distinct = first + span / 2;
    uint64_t alias = first + span;

    qtest_writeq(qts, first, 0x0000000000001111ULL);
    qtest_writeq(qts, distinct, 0x0000000000002222ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x1111);

    qtest_writeq(qts, alias, 0x0000000000003333ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x3333);
}

static void check_backdoor_bank_distinct(QTestState *qts, uint64_t base,
                                         unsigned int bank, uint64_t offset)
{
    uint64_t bank_base = base + (uint64_t)bank * BACKDOOR_BANK_STRIDE;
    uint64_t first = bank_base + 0x1000;

    qtest_writeq(qts, first, 0x0000000000004444ULL);
    qtest_writeq(qts, first + offset, 0x0000000000005555ULL);
    g_assert_cmphex(qtest_readq(qts, first) & 0xffffULL, ==, 0x4444);
}

static void test_backdoor_tracks_bank_size(const void *data)
{
    QTestState *qts;

    qts = qtest_init("-M origin200");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 16 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 16 * MiB);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 128M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 32 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 32 * MiB);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 256M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 64 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 64 * MiB);
    qtest_quit(qts);

    qts = qtest_init("-M origin200 -m 768M");
    check_backdoor_bank_span(qts, BACKDOOR_DIR, 0, 64 * MiB);
    check_backdoor_bank_span(qts, BACKDOOR_ECC, 0, 64 * MiB);
    check_backdoor_bank_distinct(qts, BACKDOOR_DIR, 1, 64 * MiB);
    check_backdoor_bank_distinct(qts, BACKDOOR_ECC, 1, 64 * MiB);
    qtest_quit(qts);
}

/*
 * The default machine populates one 64 MiB bank from a matched DIMM pair.
 * Bank 0 answers in standard directory format; bank 1 and the top of the
 * window remain absent.
 */
static void test_directory_backdoor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_entry(qts, BACKDOOR_DIR, 0);
    check_backdoor_standard_format(qts, BACKDOOR_DIR);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_DIR + BACKDOOR_SIZE - 8);

    qtest_quit(qts);
}

static void test_ecc_backdoor(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    check_backdoor_entry(qts, BACKDOOR_ECC, 0);
    check_backdoor_standard_format(qts, BACKDOOR_ECC);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_BANK_STRIDE);
    check_backdoor_absent(qts, BACKDOOR_ECC + BACKDOOR_SIZE - 8);

    qtest_quit(qts);
}

/*
 * The same 768 MiB configuration as test_multi_bank_memory: bank 0 full,
 * bank 1 partial, both genuinely populated, bank 2 and beyond not. What this
 * adds beyond that test is proof the back door tracks how many banks a
 * configuration actually has rather than hard-coding "bank 0 only" --
 * bank 1's own share of the back door has to round-trip here although it
 * does not on the default machine above, and bank 0's pattern has to
 * survive bank 1 being written, which a model aliasing every populated
 * bank onto the same bytes would not show.
 */
static void check_backdoor_multi_bank(QTestState *qts, uint64_t base)
{
    const uint64_t isolation_pattern = 0x00000000000012f0ULL;

    check_backdoor_entry(qts, base, 0);
    check_backdoor_standard_format(qts, base);
    check_backdoor_entry(qts, base, 1);
    check_backdoor_standard_format(qts, base + BACKDOOR_BANK_STRIDE);

    /* Bank 0's own entry, written last so nothing after this touches it,
     * must survive bank 1 being written -- which a model aliasing every
     * populated bank onto the same bytes would not show. */
    qtest_writeq(qts, base, isolation_pattern);
    check_backdoor_entry(qts, base, 1);
    g_assert_cmphex(qtest_readq(qts, base) & 0xffffULL, ==,
                    isolation_pattern & 0xffffULL);

    check_backdoor_absent(qts, base + 2 * BACKDOOR_BANK_STRIDE);
}

static void test_backdoor_tracks_populated_banks(const void *data)
{
    QTestState *qts = qtest_init("-M origin200 -m 768M");

    check_backdoor_multi_bank(qts, BACKDOOR_DIR);
    check_backdoor_multi_bank(qts, BACKDOOR_ECC);

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
    qtest_add_data_func("sn0/address-map/memory-128m", NULL,
                        test_128m_memory);
    qtest_add_data_func("sn0/address-map/memory-256m", NULL,
                        test_256m_memory);
    qtest_add_data_func("sn0/address-map/memory-512m", NULL,
                        test_512m_memory);
    qtest_add_data_func("sn0/address-map/hub-local-memory-alias", NULL,
                        test_hub_local_memory_alias);
    qtest_add_data_func("sn0/address-map/invalid-memory-size", NULL,
                        test_invalid_memory_size);
    qtest_add_data_func("sn0/address-map/directory-backdoor", NULL,
                        test_directory_backdoor);
    qtest_add_data_func("sn0/address-map/ecc-backdoor", NULL,
                        test_ecc_backdoor);
    qtest_add_data_func("sn0/address-map/backdoor-tracks-populated-banks",
                        NULL, test_backdoor_tracks_populated_banks);
    qtest_add_data_func("sn0/address-map/backdoor-tracks-bank-size", NULL,
                        test_backdoor_tracks_bank_size);

    return g_test_run();
}
