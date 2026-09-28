/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock memory/directory unit: bank mapping, ECC, directory state and
 * atomic memory operations. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-md.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/bitmap.h"
#include "qemu/module.h"
#include "hw/core/cpu.h"
#include "trace.h"

enum {
    MD_ARB, MD_MIGRATION, MD_PERF_SELECT0, MD_PERF_SELECT1,
    MD_PERF_COUNTER0, MD_PERF_COUNTER1, MD_JUNK_TIMING,
    MD_SELFTEST_DATA, MD_ISSUE_DEBUG, MD_MIGRATION_DIFFERENCE,
    MD_MIGRATION_VALUE, MD_ECC_CONFIG, MD_DIRECTORY_DEBUG, MD_MEMORY_DEBUG,
};

#define MD_REPLY_QUEUE UINT64_C(0x001818180c0c0c00)
#define MD_REQUEST_QUEUE UINT64_C(0x0000000004040400)
#define MD_COUNTER_MASK ((UINT64_C(1) << 41) - 1)
#define MD_PROTECTION_EXTENT 0x40000000
#define MD_PROTECTION_SELECTOR_MASK 0x7c8
#define MD_PROTECTION_SELECTOR 0x408
#define MD_MEMORY_BANK_SIZE UINT64_C(0x40000000)
#define MD_DIRECTORY_BANK_SIZE (MD_MEMORY_BANK_SIZE / 8)
#define MD_ECC_BANK_SIZE (MD_MEMORY_BANK_SIZE / 8)
#define MD_ECC_ADMITTED_HALFWORD_LOW 0x04000006
#define MD_ECC_ADMITTED_HALFWORD_HIGH 0x08000006
/*
 * Bits 58:56 are write-only in the public definition; reading them back and
 * resetting them to zero is model policy.
*/
#define MD_MEMORY_CONFIG_MASK (UINT64_C(0xfbfbfbfb) | \
    (UINT64_C(3) << 32) | (UINT64_C(0xf) << 44) | \
    (UINT64_C(0x3f) << 48) | (UINT64_C(7) << 56) | \
    (UINT64_C(3) << 60))
#define MD_PERF_SELECT_MASK (UINT64_C(3) | (UINT64_C(0xf) << 4) | \
    (UINT64_C(0x7f) << 8) | (UINT64_C(0xf) << 16) | \
    (UINT64_C(0xffffffffff) << 20) | (UINT64_C(1) << 63))
#define MD_THRESHOLD_MASK (UINT64_C(0x7fff) | (UINT64_C(7) << 32) | \
    (UINT64_C(7) << 35))
#define MD_SELFTEST_INTERVAL_NS 1000
#define MD_SELFTEST_COMMAND_MASK 0x71
#define MD_SELFTEST_COMMAND_BITS 0x7
#define MD_SELFTEST_PASSED 0x1
#define MD_SELFTEST_DONE 0x2
/* Self-test data: write control, initial pattern zero, no other control. */
#define MD_SELFTEST_DATA_WRITE_ZERO 0x80
/* Memory error register: valid and overrun pairs, syndrome and address. */
#define MD_MEMORY_ERROR_READ_CE_VALID (UINT64_C(1) << 52)
#define MD_MEMORY_ERROR_READ_CE_OVERRUN (UINT64_C(1) << 53)
#define MD_MEMORY_ERROR_WRITE_UCE_VALID (UINT64_C(1) << 56)
#define MD_MEMORY_ERROR_READ_UCE_VALID (UINT64_C(1) << 60)
#define MD_MEMORY_ERROR_READ_UCE_OVERRUN (UINT64_C(1) << 61)
#define MD_MEMORY_ERROR_SYNDROME_SHIFT 40
#define MD_MEMORY_ERROR_SYNDROME_MASK \
    (UINT64_C(0xff) << MD_MEMORY_ERROR_SYNDROME_SHIFT)
#define MD_MEMORY_ERROR_ADDRESS_MASK (((UINT64_C(1) << 30) - 1) << 3)
#define MD_ECC_DATA_BITS 64
#define MD_ECC_CHECK_BITS 8
/* Directory error register: UCE, AE, CE and ACE valid and overrun pairs. */
#define MD_DIRECTORY_ERROR_UCE_VALID (UINT64_C(1) << 63)
#define MD_DIRECTORY_ERROR_CE_VALID (UINT64_C(1) << 61)
#define MD_DIRECTORY_ERROR_UCE_OVERRUN (UINT64_C(1) << 59)
#define MD_DIRECTORY_ERROR_CE_OVERRUN (UINT64_C(1) << 57)
#define MD_DIRECTORY_ERROR_SYNDROME_SHIFT 40
#define MD_DIRECTORY_ERROR_SYNDROME_MASK \
    (UINT64_C(0x7f) << MD_DIRECTORY_ERROR_SYNDROME_SHIFT)
#define MD_DIRECTORY_ERROR_ADDRESS_MASK MD_MEMORY_ERROR_ADDRESS_MASK
/* The directory backdoor's node offset: seven eighths of the 8 GiB node. */
#define MD_DIRECTORY_HSPEC_BASE UINT64_C(0x1c0000000)
#define MD_DIRECTORY_PREMIUM_CHECK_MASK \
    ((UINT64_C(0x3f) << 32) | (UINT64_C(1) << 48))
#define MD_DIRECTORY_STANDARD_CHECK_MASK (UINT64_C(0x3f) << 16)
#define MD_DIRECTORY_LINE 128

static void md_selftest_command(SGIBedrockMDState *s, uint64_t value);
static void md_selftest_complete(void *opaque);
static void md_selftest_sync(SGIBedrockMDState *s);
static void md_update_memory_aliases(SGIBedrockMDState *s);
static void md_ecc_injections_place(SGIBedrockMDState *s);
static void md_directory_injections_place(SGIBedrockMDState *s);
static void md_retired_flush_all(SGIBedrockMDState *s);
static void md_node_memory_written(SGIBedrockMDState *s, uint64_t raw,
                                   unsigned size);
static uint64_t md_directory_word_written(SGIBedrockMDState *s,
                                          uint64_t value, hwaddr offset,
                                          const uint32_t *index);
static void md_watch_line(SGIBedrockMDState *s, uint32_t index);
static void md_watched_pages_place(SGIBedrockMDState *s);

static bool md_storage(hwaddr offset, unsigned *index, uint64_t *mask)
{
    switch (offset) {
    case 0x10:
        *index = MD_ARB;
        *mask = 0xff;
        return true;
    case 0x18:
        *index = MD_MIGRATION;
        *mask = 0x3ff | (UINT64_C(0xff) << 16) | (UINT64_C(1) << 32);
        return true;
    case 0x10000:
        *index = MD_PERF_SELECT0;
        *mask = MD_PERF_SELECT_MASK;
        return true;
    case 0x10008:
        *index = MD_PERF_SELECT1;
        *mask = MD_PERF_SELECT_MASK;
        return true;
    case 0x10010:
        *index = MD_PERF_COUNTER0;
        *mask = MD_COUNTER_MASK;
        return true;
    case 0x10018:
        *index = MD_PERF_COUNTER1;
        *mask = MD_COUNTER_MASK;
        return true;
    case 0x20008:
        *index = MD_JUNK_TIMING;
        *mask = UINT64_C(0xffffffffffff);
        return true;
    case 0x20038:
        *index = MD_SELFTEST_DATA;
        *mask = 0xff;
        return true;
    case 0x20060:
        *index = MD_ISSUE_DEBUG;
        *mask = 3;
        return true;
    case 0x40040:
        *index = MD_MIGRATION_DIFFERENCE;
        *mask = MD_THRESHOLD_MASK | (UINT64_C(1) << 63);
        return true;
    case 0x40048:
        *index = MD_MIGRATION_VALUE;
        *mask = MD_THRESHOLD_MASK | (UINT64_C(3) << 62);
        return true;
    case 0x60000:
        *index = MD_ECC_CONFIG;
        *mask = 1;
        return true;
    case 0x40060:
        *index = MD_DIRECTORY_DEBUG;
        *mask = 3;
        return true;
    case 0x60040:
        *index = MD_MEMORY_DEBUG;
        *mask = 1;
        return true;
    default:
        return false;
    }
}

static bool md_idle(hwaddr offset)
{
    switch (offset) {
    case 0x20: case 0x28: case 0x40: case 0x48: case 0x58:
    case 0x20010: case 0x20018: case 0x20020: case 0x20028:
    case 0x20030: case 0x20040: case 0x20048:
    case 0x40020: case 0x40028:
    case 0x40030: case 0x40038: case 0x40058:
    case 0x60020: case 0x60028: case 0x60030: case 0x60038:
        return true;
    default:
        return false;
    }
}

static uint64_t md_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;
    uint64_t mask;
    uint64_t value;

    switch (offset) {
    case 0: return s->current_cell;
    case 0x8: return s->memory_config;
    case 0x60: return MD_REPLY_QUEUE;
    case 0x20000: return s->refresh;
    case 0x20040:
        md_selftest_sync(s);
        return s->selftest_error_address;
    case 0x20048:
        md_selftest_sync(s);
        return s->selftest_status;
    case 0x40000: return s->directory_config;
    case 0x40050: return MD_REQUEST_QUEUE;
    case 0x60010: return s->memory_error;
    case 0x60018:
        /* Read-to-clear is model policy; the header names no mechanism. */
        value = s->memory_error;
        s->memory_error = 0;
        return value;
    case 0x40010: return s->directory_error;
    case 0x40018:
        value = s->directory_error;
        s->directory_error = 0;
        return value;
    }
    if (md_storage(offset, &index, &mask)) {
        /*
         * Performance counter 0 is plain storage, but the stock PROM uses it
         * as a software lock word at the second SMP rendezvous; traced so
         * that rendezvous can be followed by processor.
         */
        if (index == MD_PERF_COUNTER0) {
            trace_sgi_bedrock_md_perf0_read(
                current_cpu ? current_cpu->cpu_index : -1, s->storage[index]);
        }
        return s->storage[index];
    }
    if (md_idle(offset)) {
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unimplemented read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void md_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;
    uint64_t mask;

    switch (offset) {
    case 0:
        s->current_cell = value;
        return;
    case 0x8:
        s->memory_config = value & MD_MEMORY_CONFIG_MASK;
        md_update_memory_aliases(s);
        return;
    case 0x20000:
        s->refresh = value & ((UINT64_C(1) << 63) | 0xfff); return;
    case 0x40000:
        s->directory_config = value & 3;
        return;
    case 0x60:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: reply credit reset unsupported\n");
        return;
    case 0x40050:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: request queue resize unsupported\n");
        return;
    case 0x20030:
        md_selftest_command(s, value);
        return;
    case 0x40010:
    case 0x40018:
    case 0x60010:
    case 0x60018:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-md: error register "
                      "write at 0x%" HWADDR_PRIx " ignored\n", offset);
        return;
    }
    if (md_storage(offset, &index, &mask)) {
        s->storage[index] = value & mask;
        if (index == MD_PERF_COUNTER0) {
            trace_sgi_bedrock_md_perf0_write(
                current_cpu ? current_cpu->cpu_index : -1, s->storage[index]);
        }
        return;
    }
    if (md_idle(offset)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unsupported write at 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unimplemented write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n", offset, value);
}

/* Every register is 64 bits; refusing narrower accesses is model policy. */
static const MemoryRegionOps md_ops = {
    .read = md_read, .write = md_write, .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static uint64_t md_directory_mask(const SGIBedrockMDState *s)
{
    return s->directory_config & 1 ? UINT64_MAX : UINT32_MAX;
}

static unsigned md_memory_size_code(const SGIBedrockMDState *s,
                                    unsigned locational_bank)
{
    return extract64(s->memory_config, (locational_bank / 2) * 8 + 4, 4);
}

static bool md_memory_bank_enabled(const SGIBedrockMDState *s,
                                   unsigned locational_bank)
{
    return s->memory_config &
           (UINT64_C(1) << ((locational_bank / 2) * 8 +
                            (locational_bank & 1)));
}

static uint64_t md_memory_bank_size(unsigned size_code)
{
    return UINT64_C(0x2000000) << size_code;
}

static bool md_has_installed_bank_sizes(const SGIBedrockMDState *s)
{
    for (unsigned bank = 0; bank < 8; bank++) {
        if (s->installed_memory_bank_mib[bank]) {
            return true;
        }
    }
    return false;
}

static bool md_installed_bank(const SGIBedrockMDState *s, unsigned bank,
                              uint64_t *base, uint64_t *size)
{
    uint64_t dense_base = 0;

    if (!(s->installed_memory_bank_mask & (1U << bank)) ||
        !s->installed_memory_bank_mib[bank]) {
        return false;
    }
    for (unsigned earlier = 0; earlier < bank; earlier++) {
        if (s->installed_memory_bank_mask & (1U << earlier)) {
            dense_base += (uint64_t)s->installed_memory_bank_mib[earlier]
                          << 20;
        }
    }
    *base = dense_base;
    *size = (uint64_t)s->installed_memory_bank_mib[bank] << 20;
    return true;
}

static void md_update_memory_aliases(SGIBedrockMDState *s)
{
    uint64_t raw_size;

    if (!s->memory_alias_initialized || !md_has_installed_bank_sizes(s)) {
        return;
    }
    raw_size = memory_region_size(s->node_mr);
    memory_region_transaction_begin();
    for (unsigned software_bank = 0; software_bank < 8; software_bank++) {
        unsigned locational_bank = software_bank ^
                                   extract64(s->memory_config, 32, 2);
        unsigned size_code = md_memory_size_code(s, locational_bank);
        uint64_t bank_base;
        uint64_t bank_size;

        memory_region_set_enabled(&s->memory_alias[software_bank], false);
        s->bank_alias_size[software_bank] = 0;
        if (!md_memory_bank_enabled(s, locational_bank) ||
            size_code == 0 || size_code > 5 ||
            !md_installed_bank(s, locational_bank, &bank_base, &bank_size)) {
            continue;
        }
        bank_size = MIN(bank_size, md_memory_bank_size(size_code));
        if (bank_base >= raw_size) {
            continue;
        }
        bank_size = MIN(bank_size, raw_size - bank_base);
        memory_region_set_alias_offset(&s->memory_alias[software_bank],
                                       bank_base);
        memory_region_set_size(&s->memory_alias[software_bank], bank_size);
        memory_region_set_enabled(&s->memory_alias[software_bank], true);
        s->bank_alias_base[software_bank] = bank_base;
        s->bank_alias_size[software_bank] = bank_size;
    }
    /* A retired overlay's place follows the old mapping. */
    md_retired_flush_all(s);
    md_ecc_injections_place(s);
    md_directory_injections_place(s);
    md_watched_pages_place(s);
    memory_region_transaction_commit();
}

static bool md_is_protection(hwaddr offset)
{
    return offset < MD_PROTECTION_EXTENT &&
           (offset & MD_PROTECTION_SELECTOR_MASK) == MD_PROTECTION_SELECTOR;
}

static bool md_protection_translate(const SGIBedrockMDState *s,
                                    hwaddr offset, unsigned *index)
{
    unsigned software_bank = offset / MD_DIRECTORY_BANK_SIZE;
    unsigned locational_bank = software_bank ^
                               extract64(s->memory_config, 32, 2);
    uint64_t node_size;
    uint64_t bank_base = software_bank * MD_MEMORY_BANK_SIZE;
    uint64_t bank_size;
    uint64_t within;

    if (!s->node_mr) {
        return false;
    }
    node_size = memory_region_size(s->node_mr);
    if (md_has_installed_bank_sizes(s)) {
        if (!md_installed_bank(s, locational_bank, &bank_base, &bank_size)) {
            return false;
        }
        if (bank_base >= node_size) {
            return false;
        }
        bank_size = MIN(bank_size, node_size - bank_base);
        goto translate;
    }
    if (bank_base >= node_size) {
        return false;
    }
    bank_size = MIN(node_size - bank_base, MD_MEMORY_BANK_SIZE);
translate:
    within = ((uint64_t)((offset % MD_DIRECTORY_BANK_SIZE) >> 11) << 14) |
             (((offset >> 4) & 3) << 7);
    within %= bank_size;
    *index = ((bank_base + within) >> 14) * 4 + ((within >> 7) & 3);
    return *index < s->protection_entries;
}

static bool md_directory_translate(const SGIBedrockMDState *s, hwaddr offset,
                                   unsigned *index, bool *configured)
{
    unsigned software_bank = offset / MD_DIRECTORY_BANK_SIZE;
    unsigned selector = extract64(s->memory_config, 32, 2);
    unsigned locational_bank = software_bank ^ selector;
    unsigned size_code;
    uint64_t within;
    uint64_t raw = 0;

    if (configured) {
        *configured = false;
    }
    if (!md_memory_bank_enabled(s, locational_bank)) {
        return false;
    }
    size_code = md_memory_size_code(s, locational_bank);
    if (size_code == 0 || size_code > 5) {
        return false;
    }

    /* One 16-byte carrier slot describes one 128-byte memory line. */
    within = ((offset % MD_DIRECTORY_BANK_SIZE) & ~UINT64_C(0xf)) << 3;
    if (within >= md_memory_bank_size(size_code)) {
        /* Beyond the declared bank: outside configured memory. */
        return false;
    }
    if (md_has_installed_bank_sizes(s)) {
        uint64_t installed_size;

        if (!md_installed_bank(s, locational_bank, &raw, &installed_size) ||
            within >= installed_size) {
            /*
             * Enabled bank, valid size code, but this carrier has no fitted
             * backing (an absent bank, or above a partly-fitted
             * bank's installed capacity). Firmware walks these
             * provisionally-enabled carriers during its directory retention
             * probe; admit them to the stock-PROM directory compatibility
             * store instead of raising a bus error.
            */
            if (configured) {
                *configured = true;
            }
            return false;
        }
        goto translated;
    }

    /* The backing arrays follow the raw RAM's dense locational-bank order. */
    for (unsigned bank = 0; bank < locational_bank; bank++) {
        unsigned earlier_code;

        if (!md_memory_bank_enabled(s, bank)) {
            continue;
        }
        earlier_code = md_memory_size_code(s, bank);
        if (earlier_code > 5) {
            return false;
        }
        if (earlier_code != 0) {
            raw += md_memory_bank_size(earlier_code);
        }
    }
translated:
    raw += within;
    if (configured) {
        *configured = true;
    }
    *index = raw / 128;
    return *index < s->directory_entries;
}

static int md_directory_compat_index(const SGIBedrockMDState *s,
                                     hwaddr offset)
{
    for (unsigned i = 0; i < s->directory_compat_count; i++) {
        if (s->directory_compat_offsets[i] == offset) {
            return i;
        }
    }
    return -1;
}

static unsigned md_directory_compat_get(SGIBedrockMDState *s,
                                        hwaddr offset)
{
    int index = md_directory_compat_index(s, offset);

    if (index >= 0) {
        return index;
    }
    g_assert(s->directory_compat_count <
             SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES);
    index = s->directory_compat_count++;
    s->directory_compat_offsets[index] = offset;
    s->directory_compat_values[index] = 0;
    return index;
}

static bool md_directory_accepts(void *opaque, hwaddr offset, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    const char *access = is_write ? "write" : "read";
    bool configured;
    unsigned index;

    if (md_is_protection(offset)) {
        if (size != 8 || offset % 8 != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bedrock-md: %u-byte protection %s at offset "
                          "0x%" HWADDR_PRIx " is not an aligned 64-bit "
                          "carrier word\n", size, access, offset);
            return false;
        }
        return true;
    }
    if (!s->directory) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: directory %s without fitted "
                      "node memory\n", access);
        return false;
    }
    if (size != 8 || offset % 8 != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: %u-byte directory %s at offset 0x%"
                      HWADDR_PRIx " is not an aligned 64-bit carrier word\n",
                      size, access, offset);
        return false;
    }
    if (!md_directory_translate(s, offset, &index, &configured)) {
        if (!configured) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bedrock-md: directory %s at offset 0x%"
                          HWADDR_PRIx " is outside configured memory\n",
                          access, offset);
            return false;
        }
        if (md_directory_compat_index(s, offset) >= 0 ||
            s->directory_compat_count <
            SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES) {
            return true;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: directory %s at offset 0x%"
                      HWADDR_PRIx " rejected: compatibility store is full\n",
                      access, offset);
        return false;
    }
    return true;
}

/*
 * A debugger's or monitor's read of a directory word is not software
 * looking at the state, so it does not start following the line.
 */
static MemTxResult md_directory_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (md_is_protection(offset)) {
        *value = md_protection_translate(s, offset, &index) ?
                 s->protection[index] : 0;
        return MEMTX_OK;
    }
    if (!md_directory_translate(s, offset, &index, NULL)) {
        index = md_directory_compat_get(s, offset);
        *value = s->directory_compat_values[index] &
                 ((offset & 8) ? UINT64_MAX : md_directory_mask(s));
        return MEMTX_OK;
    }
    if (offset & 8) {
        *value = s->page_counters[index];
        return MEMTX_OK;
    }
    if (!attrs.debug) {
        md_watch_line(s, index);
    }
    *value = s->directory[index] & md_directory_mask(s);
    return MEMTX_OK;
}

static MemTxResult md_directory_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (md_is_protection(offset)) {
        if (md_protection_translate(s, offset, &index)) {
            s->protection[index] = value;
        }
        return MEMTX_OK;
    }
    if (!md_directory_translate(s, offset, &index, NULL)) {
        index = md_directory_compat_get(s, offset);
        if (!(offset & 8)) {
            value = md_directory_word_written(s, value, offset, NULL);
        }
        s->directory_compat_values[index] =
            value & ((offset & 8) ? UINT64_MAX : md_directory_mask(s));
        return MEMTX_OK;
    }
    if (offset & 8) {
        s->page_counters[index] = value;
    } else {
        s->directory[index] =
            md_directory_word_written(s, value, offset, &index) &
            md_directory_mask(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps md_directory_ops = {
    .read_with_attrs = md_directory_read,
    .write_with_attrs = md_directory_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
        .accepts = md_directory_accepts,
    },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

/*
 * The directory ECC code's column tables, one column per word bit, as the
 * Origin 300's directory diagnostic uses
 * them. A premium word keeps seven check bits at 37:32 and 48, a standard
 * word six at 21:16, and those positions'
 * columns are the unit vectors.
*/
static const uint8_t md_directory_premium_column[64] = {
    0x1f, 0x2f, 0x37, 0x3b, 0x3d, 0x3e, 0x07, 0x0b,
    0x0d, 0x0e, 0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c,
    0x7f, 0x4f, 0x57, 0x5b, 0x5d, 0x5e, 0x67, 0x6b,
    0x6d, 0x6e, 0x73, 0x75, 0x76, 0x79, 0x7a, 0x7c,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x23, 0x25,
    0x26, 0x29, 0x2a, 0x2c, 0x31, 0x32, 0x34, 0x38,
    0x40, 0x43, 0x45, 0x46, 0x49, 0x4a, 0x4c, 0x51,
    0x52, 0x54, 0x58, 0x61, 0x62, 0x64, 0x68, 0x70,
};

static const uint8_t md_directory_standard_column[32] = {
    0x1f, 0x2f, 0x37, 0x3b, 0x3d, 0x3e, 0x07, 0x0b,
    0x0d, 0x0e, 0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x23, 0x25,
    0x26, 0x29, 0x2a, 0x2c, 0x31, 0x32, 0x34, 0x38,
};

static bool md_directory_premium(const SGIBedrockMDState *s)
{
    return s->directory_config & 1;
}

static uint64_t md_directory_check_mask(bool premium)
{
    return premium ? MD_DIRECTORY_PREMIUM_CHECK_MASK :
                     MD_DIRECTORY_STANDARD_CHECK_MASK;
}

/* Zero for a codeword; otherwise the column of the bit in error, or none. */
static uint8_t md_directory_syndrome(uint64_t word, bool premium)
{
    const uint8_t *column = premium ? md_directory_premium_column :
                                      md_directory_standard_column;
    uint8_t syndrome = 0;

    if (!premium) {
        word = (uint32_t)word;
    }
    while (word) {
        syndrome ^= column[ctz64(word)];
        word &= word - 1;
    }
    return syndrome;
}

static int md_directory_syndrome_bit(uint8_t syndrome, bool premium)
{
    const uint8_t *column = premium ? md_directory_premium_column :
                                      md_directory_standard_column;
    unsigned bits = premium ? 64 : 32;

    for (unsigned bit = 0; bit < bits; bit++) {
        if (column[bit] == syndrome) {
            return bit;
        }
    }
    return -1;
}

uint64_t sgi_bedrock_md_directory_encode(uint64_t word, bool premium)
{
    uint64_t mask = md_directory_check_mask(premium);
    uint8_t check;

    word &= ~mask;
    check = md_directory_syndrome(word, premium);
    if (premium) {
        return word | ((uint64_t)(check & 0x3f) << 32) |
               ((uint64_t)(check >> 6) << 48);
    }
    return word | ((uint64_t)check << 16);
}

enum {
    MD_DIRECTORY_ERROR_CE,
    MD_DIRECTORY_ERROR_UCE,
};

/* Same rules as the memory error record: UCE above CE. */
static void md_directory_error_latch(SGIBedrockMDState *s, int kind,
                                     uint64_t address, uint8_t syndrome)
{
    uint64_t valid = kind == MD_DIRECTORY_ERROR_UCE ?
                     MD_DIRECTORY_ERROR_UCE_VALID : MD_DIRECTORY_ERROR_CE_VALID;
    uint64_t overrun = kind == MD_DIRECTORY_ERROR_UCE ?
                       MD_DIRECTORY_ERROR_UCE_OVERRUN :
                       MD_DIRECTORY_ERROR_CE_OVERRUN;
    uint64_t recorded = MD_DIRECTORY_ERROR_UCE_VALID |
                        (kind == MD_DIRECTORY_ERROR_CE ?
                         MD_DIRECTORY_ERROR_CE_VALID : 0);

    if (s->directory_error & recorded) {
        s->directory_error |= overrun;
    } else {
        s->directory_error &= ~(MD_DIRECTORY_ERROR_ADDRESS_MASK |
                                MD_DIRECTORY_ERROR_SYNDROME_MASK);
        s->directory_error |= (address & MD_DIRECTORY_ERROR_ADDRESS_MASK) |
                              ((uint64_t)syndrome <<
                               MD_DIRECTORY_ERROR_SYNDROME_SHIFT);
    }
    s->directory_error |= valid;
    /*
     * The unit's error interrupts are the memory ones: the Origin 300's
     * directory diagnostic waits for the correctable
     * one and then reads this register's correctable bit, and waits for
     * error-interrupt bit 24 before reading its
     * uncorrectable bit.
    */
    if (!s->bedrock) {
        return;
    }
    if (kind == MD_DIRECTORY_ERROR_CE) {
        sgi_bedrock_memory_correctable_error(s->bedrock);
    } else {
        sgi_bedrock_memory_uncorrectable_error(s->bedrock);
    }
}

/*
 * Placing or removing an overlay is a memory map change, and each one
 * rebuilds the flat view of every address space. The stock PROM's memory
 * test marks and clears the same few hundred doublewords and lines over
 * and over: one four-processor Origin 300 boot placed 6630 overlays and
 * removed as many, for 310 distinct doublewords and lines, and the
 * rebuilds stretched the test from about ten seconds to two to four
 * minutes on a loaded host. So lifting a mark retires its slot rather than
 * removing its overlay: a retired slot's overlay stays placed and passes
 * every access through as the memory beneath would, marking the same
 * doubleword or line again reactivates it without a map change, and an
 * overlay is removed once it has idled for a second, together with any
 * other that has by then, in one change. None of this is visible to the
 * guest.
 */
#define MD_RETIRED_IDLE_NS NANOSECONDS_PER_SECOND

static void md_retired_arm(SGIBedrockMDState *s)
{
    if (!timer_pending(s->retired_timer)) {
        timer_mod(s->retired_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                    MD_RETIRED_IDLE_NS);
    }
}

/*
 * Every write to node memory through the memory unit, a fetch-and-op among
 * them, asks whether it covers a marked
 * line, so the lookup goes through a table keyed by line rather than a scan
 * of all the slots: nmem's fetch-and-op
 * test otherwise spends most of its time in the scan.
*/
static SGIBedrockMDDirectoryInjection *
md_directory_injection_find(SGIBedrockMDState *s, uint32_t index)
{
    return g_hash_table_lookup(s->directory_injection_by_index, &index);
}

static void md_directory_injection_activate(
    SGIBedrockMDState *s, SGIBedrockMDDirectoryInjection *injection,
    uint32_t index)
{
    injection->active = true;
    injection->index = index;
    g_hash_table_insert(s->directory_injection_by_index, &injection->index,
                        injection);
}

static void md_directory_injection_deactivate(
    SGIBedrockMDState *s, SGIBedrockMDDirectoryInjection *injection)
{
    if (injection->active) {
        g_hash_table_remove(s->directory_injection_by_index,
                            &injection->index);
        injection->active = false;
    }
}

/*
 * Place an overlay over the memory the raw offset @raw backs, at the
 * software-bank address the bank aliases give it, or leave it unplaced when
 * no enabled bank reaches it. A bank-select change moves it. A checked
 * doubleword sits above a checked line, and a watched line above both,
 * since its own accesses go through the checks of either.
 */
static bool md_overlay_place(SGIBedrockMDState *s, MemoryRegion *mr,
                             uint64_t raw, uint64_t *address, bool *placed,
                             int priority)
{
    if (*placed) {
        memory_region_del_subregion(&s->ecc_overlay, mr);
        *placed = false;
    }
    for (unsigned bank = 0; bank < 8; bank++) {
        if (s->bank_alias_size[bank] &&
            raw >= s->bank_alias_base[bank] &&
            raw - s->bank_alias_base[bank] < s->bank_alias_size[bank]) {
            *address = bank * MD_MEMORY_BANK_SIZE +
                       (raw - s->bank_alias_base[bank]);
            memory_region_add_subregion_overlap(&s->ecc_overlay, *address,
                                                mr, priority);
            *placed = true;
            return true;
        }
    }
    return false;
}

static void md_directory_injection_unplace(
    SGIBedrockMDDirectoryInjection *injection)
{
    SGIBedrockMDState *s = injection->md;

    if (injection->placed) {
        if (!injection->active) {
            g_hash_table_remove(s->directory_retired_by_index,
                                &injection->index);
        }
        memory_region_del_subregion(&s->ecc_overlay, &injection->mr);
        injection->placed = false;
    }
}

/* Lift a line's mark, keeping its overlay placed for the next one. */
static void md_directory_injection_retire(
    SGIBedrockMDState *s, SGIBedrockMDDirectoryInjection *injection)
{
    md_directory_injection_deactivate(s, injection);
    if (injection->placed) {
        injection->retired_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g_hash_table_insert(s->directory_retired_by_index, &injection->index,
                            injection);
        md_retired_arm(s);
    }
}

static void md_directory_injection_place(
    SGIBedrockMDState *s, SGIBedrockMDDirectoryInjection *injection)
{
    md_overlay_place(s, &injection->mr,
                     (uint64_t)injection->index * MD_DIRECTORY_LINE,
                     &injection->address, &injection->placed, 0);
}

static void md_directory_injections_place(SGIBedrockMDState *s)
{
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        if (s->directory_injection[i].active) {
            md_directory_injection_place(s, &s->directory_injection[i]);
        }
    }
}

static void md_directory_injection_clear(SGIBedrockMDState *s,
                                         uint32_t index)
{
    SGIBedrockMDDirectoryInjection *injection =
        md_directory_injection_find(s, index);

    if (injection) {
        md_directory_injection_retire(s, injection);
    }
}

static void md_directory_injections_clear(SGIBedrockMDState *s)
{
    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        md_directory_injection_unplace(&s->directory_injection[i]);
        md_directory_injection_deactivate(s, &s->directory_injection[i]);
    }
    memory_region_transaction_commit();
}

/* A slot neither marked nor placed, removing every retired overlay if none. */
static SGIBedrockMDDirectoryInjection *
md_directory_injection_free(SGIBedrockMDState *s)
{
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
            SGIBedrockMDDirectoryInjection *injection =
                &s->directory_injection[i];

            if (!injection->active && !injection->placed) {
                return injection;
            }
        }
        md_retired_flush_all(s);
    }
    return NULL;
}

static void md_directory_injection_add(SGIBedrockMDState *s, uint32_t index,
                                       hwaddr offset)
{
    SGIBedrockMDDirectoryInjection *injection =
        md_directory_injection_find(s, index);

    if (injection) {
        injection->offset = offset;
        return;
    }
    injection = g_hash_table_lookup(s->directory_retired_by_index, &index);
    if (injection) {
        g_hash_table_remove(s->directory_retired_by_index, &index);
        md_directory_injection_activate(s, injection, index);
        injection->offset = offset;
        return;
    }
    injection = md_directory_injection_free(s);
    if (injection) {
        md_directory_injection_activate(s, injection, index);
        injection->offset = offset;
        md_directory_injection_place(s, injection);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: directory injection table "
                  "full; reads of line %u are not checked\n", index);
}

/*
 * A backdoor write of the +0 word: with directory ECC ignored the word is
 * stored as written and a non-codeword
 * starts checking its line; otherwise the check field is regenerated.
*/
static uint64_t md_directory_word_written(SGIBedrockMDState *s,
                                          uint64_t value, hwaddr offset,
                                          const uint32_t *index)
{
    bool premium = md_directory_premium(s);

    if (s->directory_config & 2) {
        if (index) {
            if (md_directory_syndrome(value, premium)) {
                md_directory_injection_add(s, *index, offset);
            } else {
                md_directory_injection_clear(s, *index);
            }
        }
        return value;
    }
    if (index) {
        md_directory_injection_clear(s, *index);
    }
    return sgi_bedrock_md_directory_encode(value, premium);
}

/* Read a checked line: @offset and @size name the bytes wanted from it. */
static MemTxResult md_directory_checked_read(
    SGIBedrockMDDirectoryInjection *injection, hwaddr offset,
    uint64_t *value, unsigned size, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = injection->md;
    uint64_t raw = (uint64_t)injection->index * MD_DIRECTORY_LINE + offset;
    bool premium = md_directory_premium(s);
    MemTxResult result;

    *value = 0;
    if (!(s->directory_config & 2)) {
        uint64_t word = s->directory[injection->index];
        uint8_t syndrome = md_directory_syndrome(word, premium);
        uint64_t address = MD_DIRECTORY_HSPEC_BASE + injection->offset;

        if (syndrome) {
            int bit = md_directory_syndrome_bit(syndrome, premium);

            if (bit < 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-bedrock-md: uncorrectable directory ECC "
                              "error for line %u (word 0x%016" PRIx64
                              "), syndrome 0x%02x\n", injection->index,
                              word, syndrome);
                md_directory_error_latch(s, MD_DIRECTORY_ERROR_UCE, address,
                                         syndrome);
                return MEMTX_ERROR;
            }
            /* The corrected word is written back: the backdoor shows it. */
            s->directory[injection->index] = word ^ (UINT64_C(1) << bit);
            md_directory_injection_clear(s, injection->index);
            md_directory_error_latch(s, MD_DIRECTORY_ERROR_CE, address,
                                     syndrome);
        }
    }
    switch (size) {
    case 1:
        *value = address_space_ldub(&s->node_as, raw, attrs, &result);
        break;
    case 2:
        *value = address_space_lduw_be(&s->node_as, raw, attrs, &result);
        break;
    case 4:
        *value = address_space_ldl_be(&s->node_as, raw, attrs, &result);
        break;
    default:
        *value = address_space_ldq_be(&s->node_as, raw, attrs, &result);
        break;
    }
    return result;
}

/*
 * An access to a retired slot's overlay does what it would do with the
 * overlay gone: a marked line beneath is still checked and still rewritten
 * by a write, and anything else is plain node memory.
 */
static MemTxResult md_retired_read(SGIBedrockMDState *s, uint64_t raw,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    SGIBedrockMDDirectoryInjection *line =
        md_directory_injection_find(s, raw / MD_DIRECTORY_LINE);
    MemTxResult result;

    if (line) {
        return md_directory_checked_read(line, raw % MD_DIRECTORY_LINE, value,
                                         size, attrs);
    }
    switch (size) {
    case 1:
        *value = address_space_ldub(&s->node_as, raw, attrs, &result);
        break;
    case 2:
        *value = address_space_lduw_be(&s->node_as, raw, attrs, &result);
        break;
    case 4:
        *value = address_space_ldl_be(&s->node_as, raw, attrs, &result);
        break;
    default:
        *value = address_space_ldq_be(&s->node_as, raw, attrs, &result);
        break;
    }
    return result;
}

static MemTxResult md_retired_write(SGIBedrockMDState *s, uint64_t raw,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    MemTxResult result;

    switch (size) {
    case 1:
        address_space_stb(&s->node_as, raw, value, attrs, &result);
        break;
    case 2:
        address_space_stw_be(&s->node_as, raw, value, attrs, &result);
        break;
    case 4:
        address_space_stl_be(&s->node_as, raw, value, attrs, &result);
        break;
    default:
        address_space_stq_be(&s->node_as, raw, value, attrs, &result);
        break;
    }
    if (result == MEMTX_OK) {
        md_node_memory_written(s, raw, size);
    }
    return result;
}

static MemTxResult md_directory_overlay_read(void *opaque, hwaddr offset,
                                             uint64_t *value, unsigned size,
                                             MemTxAttrs attrs)
{
    SGIBedrockMDDirectoryInjection *injection = opaque;

    if (!injection->active) {
        return md_retired_read(injection->md,
                               (uint64_t)injection->index *
                               MD_DIRECTORY_LINE + offset,
                               value, size, attrs);
    }
    return md_directory_checked_read(injection, offset, value, size, attrs);
}

/*
 * A write to any part of a marked line rewrites its directory entry, as the
 * unit does on every access it serves:
 * the stored word keeps its bits and gets a fresh check field, so the line is
 * no longer checked. The stock PROM
 * leaves non-codewords behind its directory walk with checking on, then
 * writes and reads inside such lines while it
 * copies itself to memory.
*/
static void md_directory_line_written(SGIBedrockMDState *s, uint32_t index)
{
    if (md_directory_injection_find(s, index)) {
        s->directory[index] =
            sgi_bedrock_md_directory_encode(s->directory[index],
                                            md_directory_premium(s));
        md_directory_injection_clear(s, index);
    }
}

static MemTxResult md_directory_overlay_write(void *opaque, hwaddr offset,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBedrockMDDirectoryInjection *injection = opaque;
    SGIBedrockMDState *s = injection->md;
    uint64_t raw = (uint64_t)injection->index * MD_DIRECTORY_LINE + offset;
    MemTxResult result;

    switch (size) {
    case 1:
        address_space_stb(&s->node_as, raw, value, attrs, &result);
        break;
    case 2:
        address_space_stw_be(&s->node_as, raw, value, attrs, &result);
        break;
    case 4:
        address_space_stl_be(&s->node_as, raw, value, attrs, &result);
        break;
    default:
        address_space_stq_be(&s->node_as, raw, value, attrs, &result);
        break;
    }
    if (result == MEMTX_OK) {
        md_node_memory_written(s, raw, size);
    }
    return result;
}

static const MemoryRegionOps md_directory_overlay_ops = {
    .read_with_attrs = md_directory_overlay_read,
    .write_with_attrs = md_directory_overlay_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * The memory ECC code's column table: the syndrome each of the 64 data bits
 * produces alone, then the eight check
 * bits' unit vectors. The Origin 300 memory diagnostic injects one single-bit
 * error per table entry. Every column
 * has odd weight and no two differ by a column, so a single-bit error is
 * named by its
 * syndrome and a double-bit error is detected and not corrected.
*/
static const uint8_t md_ecc_column[MD_ECC_DATA_BITS + MD_ECC_CHECK_BITS] = {
    0x13, 0x23, 0x43, 0x83, 0x2f, 0xf1, 0x0d, 0x07,
    0xd0, 0x70, 0x4f, 0xf8, 0x61, 0x62, 0x64, 0x68,
    0x1c, 0x2c, 0x4c, 0x8c, 0x15, 0x25, 0x45, 0x85,
    0x19, 0x29, 0x49, 0x89, 0x1a, 0x2a, 0x4a, 0x8a,
    0x51, 0x52, 0x54, 0x58, 0x91, 0x92, 0x94, 0x98,
    0xa1, 0xa2, 0xa4, 0xa8, 0x31, 0x32, 0x34, 0x38,
    0x16, 0x26, 0x46, 0x86, 0x1f, 0xf2, 0x0b, 0x0e,
    0xb0, 0xe0, 0x8f, 0xf4, 0xc1, 0xc2, 0xc4, 0xc8,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

uint8_t sgi_bedrock_md_ecc_compute(uint64_t data)
{
    uint8_t ecc = 0;

    while (data) {
        ecc ^= md_ecc_column[ctz64(data)];
        data &= data - 1;
    }
    return ecc;
}

/* The bit a nonzero syndrome names: a data bit, 64 + a check bit, or -1. */
static int md_ecc_syndrome_bit(uint8_t syndrome)
{
    for (unsigned bit = 0; bit < ARRAY_SIZE(md_ecc_column); bit++) {
        if (md_ecc_column[bit] == syndrome) {
            return bit;
        }
    }
    return -1;
}

enum {
    MD_MEMORY_ERROR_READ_CE,
    MD_MEMORY_ERROR_WRITE_UCE,
    MD_MEMORY_ERROR_READ_UCE,
};

/*
 * Latch an error the way the header describes: each kind's valid bit is set
 * whenever it occurs; the address and
 * syndrome belong to the earliest error of the highest priority so far, and a
 * kind that finds one of equal or
 * higher priority already recorded sets its overrun bit instead.
*/
static void md_memory_error_latch(SGIBedrockMDState *s, int kind,
                                  uint64_t address, uint8_t syndrome)
{
    static const uint64_t valid[] = {
        MD_MEMORY_ERROR_READ_CE_VALID, MD_MEMORY_ERROR_WRITE_UCE_VALID,
        MD_MEMORY_ERROR_READ_UCE_VALID,
    };
    uint64_t recorded = 0;

    for (int higher = kind; higher <= MD_MEMORY_ERROR_READ_UCE; higher++) {
        recorded |= valid[higher];
    }
    if (kind != MD_MEMORY_ERROR_READ_CE &&
        (s->memory_error & (MD_MEMORY_ERROR_WRITE_UCE_VALID |
                            MD_MEMORY_ERROR_READ_UCE_VALID)) &&
        ((s->memory_error ^ address) & MD_MEMORY_ERROR_ADDRESS_MASK & ~0x7f)) {
        s->memory_error |= UINT64_C(1) << 63;
    }
    if (s->memory_error & recorded) {
        s->memory_error |= valid[kind] << 1;
    } else {
        s->memory_error &= ~(MD_MEMORY_ERROR_ADDRESS_MASK |
                             MD_MEMORY_ERROR_SYNDROME_MASK);
        s->memory_error |= (address & MD_MEMORY_ERROR_ADDRESS_MASK) |
                           ((uint64_t)syndrome <<
                            MD_MEMORY_ERROR_SYNDROME_SHIFT);
    }
    s->memory_error |= valid[kind];
    if (!s->bedrock) {
        return;
    }
    if (kind == MD_MEMORY_ERROR_READ_CE) {
        sgi_bedrock_memory_correctable_error(s->bedrock);
    } else {
        sgi_bedrock_memory_uncorrectable_error(s->bedrock);
    }
}

/* Keyed by doubleword for the same reason as the directory's marked lines. */
static SGIBedrockMDECCInjection *md_ecc_injection_find(SGIBedrockMDState *s,
                                                       uint64_t raw)
{
    return g_hash_table_lookup(s->ecc_injection_by_raw, &raw);
}

static void md_ecc_injection_activate(SGIBedrockMDState *s,
                                      SGIBedrockMDECCInjection *injection,
                                      uint64_t raw)
{
    injection->active = true;
    injection->raw = raw;
    g_hash_table_insert(s->ecc_injection_by_raw, &injection->raw, injection);
}

static void md_ecc_injection_deactivate(SGIBedrockMDState *s,
                                        SGIBedrockMDECCInjection *injection)
{
    if (injection->active) {
        g_hash_table_remove(s->ecc_injection_by_raw, &injection->raw);
        injection->active = false;
    }
}

static void md_ecc_injection_unplace(SGIBedrockMDECCInjection *injection)
{
    SGIBedrockMDState *s = injection->md;

    if (injection->placed) {
        if (!injection->active) {
            g_hash_table_remove(s->ecc_retired_by_raw, &injection->raw);
        }
        memory_region_del_subregion(&s->ecc_overlay, &injection->mr);
        injection->placed = false;
    }
}

/* Lift a doubleword's mark, keeping its overlay placed for the next one. */
static void md_ecc_injection_retire(SGIBedrockMDState *s,
                                    SGIBedrockMDECCInjection *injection)
{
    md_ecc_injection_deactivate(s, injection);
    if (injection->placed) {
        injection->retired_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g_hash_table_insert(s->ecc_retired_by_raw, &injection->raw, injection);
        md_retired_arm(s);
    }
}

/*
 * A raw doubleword shows through whichever software bank's alias covers it;
 * the overlay goes at that software-bank
 * address so the processor's read of the doubleword is the one checked. A
 * bank-select change moves it.
*/
static void md_ecc_injection_place(SGIBedrockMDState *s,
                                   SGIBedrockMDECCInjection *injection)
{
    md_overlay_place(s, &injection->mr, injection->raw * 8,
                     &injection->address, &injection->placed, 1);
}

static void md_ecc_injections_place(SGIBedrockMDState *s)
{
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        if (s->ecc_injection[i].active) {
            md_ecc_injection_place(s, &s->ecc_injection[i]);
        }
    }
}

static void md_ecc_injection_clear(SGIBedrockMDState *s, uint64_t raw)
{
    SGIBedrockMDECCInjection *injection = md_ecc_injection_find(s, raw);

    if (injection) {
        md_ecc_injection_retire(s, injection);
    }
}

static void md_ecc_injections_clear(SGIBedrockMDState *s)
{
    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        md_ecc_injection_unplace(&s->ecc_injection[i]);
        md_ecc_injection_deactivate(s, &s->ecc_injection[i]);
    }
    memory_region_transaction_commit();
}

/* A slot neither marked nor placed, removing every retired overlay if none. */
static SGIBedrockMDECCInjection *md_ecc_injection_free(SGIBedrockMDState *s)
{
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
            SGIBedrockMDECCInjection *injection = &s->ecc_injection[i];

            if (!injection->active && !injection->placed) {
                return injection;
            }
        }
        md_retired_flush_all(s);
    }
    return NULL;
}

/*
 * Remove the retired overlays idle since @before, all of them for
 * INT64_MAX, in one map change. Returns when the next one comes due, or
 * INT64_MAX when none is left.
 */
static int64_t md_retired_flush(SGIBedrockMDState *s, int64_t before)
{
    int64_t next = INT64_MAX;

    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        SGIBedrockMDECCInjection *injection = &s->ecc_injection[i];

        if (injection->active || !injection->placed) {
            continue;
        }
        if (before == INT64_MAX || injection->retired_ns <= before) {
            md_ecc_injection_unplace(injection);
        } else {
            next = MIN(next, injection->retired_ns + MD_RETIRED_IDLE_NS);
        }
    }
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        SGIBedrockMDDirectoryInjection *line = &s->directory_injection[i];

        if (line->active || !line->placed) {
            continue;
        }
        if (before == INT64_MAX || line->retired_ns <= before) {
            md_directory_injection_unplace(line);
        } else {
            next = MIN(next, line->retired_ns + MD_RETIRED_IDLE_NS);
        }
    }
    memory_region_transaction_commit();
    return next;
}

static void md_retired_flush_all(SGIBedrockMDState *s)
{
    md_retired_flush(s, INT64_MAX);
}

static void md_retired_expire(void *opaque)
{
    SGIBedrockMDState *s = opaque;
    int64_t next = md_retired_flush(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                                       MD_RETIRED_IDLE_NS);

    if (next != INT64_MAX) {
        timer_mod(s->retired_timer, next);
    }
}

static void md_ecc_injection_add(SGIBedrockMDState *s, uint64_t raw)
{
    SGIBedrockMDECCInjection *injection = md_ecc_injection_find(s, raw);

    if (injection) {
        return;
    }
    injection = g_hash_table_lookup(s->ecc_retired_by_raw, &raw);
    if (injection) {
        g_hash_table_remove(s->ecc_retired_by_raw, &raw);
        md_ecc_injection_activate(s, injection, raw);
        return;
    }
    injection = md_ecc_injection_free(s);
    if (injection) {
        md_ecc_injection_activate(s, injection, raw);
        md_ecc_injection_place(s, injection);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: ECC injection table full; "
                  "reads of doubleword 0x%" PRIx64 " are not checked\n",
                  raw * 8);
}

/*
 * The ECC byte a backdoor read returns: the byte written through the backdoor
 * while it disagrees with the data,
 * otherwise the code's own value for the data, as a controller that
 * regenerates ECC on every write holds.
*/
static uint8_t md_ecc_byte(SGIBedrockMDState *s, uint64_t raw)
{
    MemTxResult result;
    uint64_t data;

    if (md_ecc_injection_find(s, raw)) {
        return s->ecc[raw];
    }
    data = address_space_ldq_be(&s->node_as, raw * 8, MEMTXATTRS_UNSPECIFIED,
                                &result);
    return result == MEMTX_OK ? sgi_bedrock_md_ecc_compute(data) : 0;
}

/* A backdoor write that disagrees with the data starts checking it. */
static void md_ecc_written(SGIBedrockMDState *s, uint64_t raw)
{
    MemTxResult result;
    uint64_t data = address_space_ldq_be(&s->node_as, raw * 8,
                                         MEMTXATTRS_UNSPECIFIED, &result);

    if (result != MEMTX_OK) {
        return;
    }
    if (sgi_bedrock_md_ecc_compute(data) != s->ecc[raw]) {
        md_ecc_injection_add(s, raw);
    } else {
        md_ecc_injection_clear(s, raw);
    }
}

/*
 * Read a checked doubleword: @address is the software-bank address the error
 * record names, @offset and @size the
 * bytes wanted from it.
*/
static MemTxResult md_ecc_checked_read(SGIBedrockMDECCInjection *injection,
                                       uint64_t address, hwaddr offset,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIBedrockMDState *s = injection->md;
    MemTxResult result;
    uint64_t data = address_space_ldq_be(&s->node_as, injection->raw * 8,
                                         attrs, &result);

    *value = 0;
    if (result != MEMTX_OK) {
        return result;
    }
    if (!(s->storage[MD_ECC_CONFIG] & 1)) {
        uint64_t raw = injection->raw;
        uint8_t syndrome = s->ecc[raw] ^ sgi_bedrock_md_ecc_compute(data);

        if (syndrome) {
            int bit = md_ecc_syndrome_bit(syndrome);

            if (bit < 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-bedrock-md: uncorrectable memory ECC "
                              "error at 0x%09" PRIx64 ", syndrome 0x%02x, "
                              "%s read\n", address, syndrome,
                              attrs.mips_uncached ? "uncached" : "cached");
                md_memory_error_latch(s, MD_MEMORY_ERROR_READ_UCE, address,
                                      syndrome);
                /*
                 * An uncached processor read fails, which the load reports as
                 * a bus error: the R10000 family posts one when a
                 * double, single or partial-word response carries the
                 * uncorrectable-error indication, and the Origin 300's memory
                 * diagnostic reads each uncorrectable syndrome uncached under
                 * a bus-error handler. A cached read is a block read,
                 * for which the processor posts a Cache Error exception
                 * instead, and the public SN1 header gives the processor
                 * interface separate error bits for an uncorrectable error on
                 * an uncached read and during a cached load. The model
                 * does not raise the Cache Error exception: a cached read
                 * completes with the word as stored and the error reaches
                 * software only through the record and its interrupt. The
                 * attribute marks only a processor's uncached or
                 * uncached-accelerated reference, so a DMA engine's read
                 * completes too.
                */
                if (attrs.mips_uncached) {
                    return MEMTX_ERROR;
                }
            } else {
                /*
                 * The corrected word and its check byte are written back, so
                 * the next read and a backdoor read both see the
                 * codeword. The Origin 300's memory diagnostic requires the
                 * check byte to be right after the correcting read (its
                 * section 2 compares it).
                */
                if (bit < MD_ECC_DATA_BITS) {
                    data ^= UINT64_C(1) << bit;
                    address_space_stq_be(&s->node_as, raw * 8, data, attrs,
                                         &result);
                    if (result != MEMTX_OK) {
                        return result;
                    }
                }
                s->ecc[raw] = sgi_bedrock_md_ecc_compute(data);
                md_ecc_injection_clear(s, raw);
                md_memory_error_latch(s, MD_MEMORY_ERROR_READ_CE, address,
                                      syndrome);
            }
        }
    }
    *value = extract64(data, (8 - offset - size) * 8, size * 8);
    return MEMTX_OK;
}

static MemTxResult md_ecc_overlay_read(void *opaque, hwaddr offset,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIBedrockMDECCInjection *injection = opaque;

    if (!injection->active) {
        return md_retired_read(injection->md, injection->raw * 8 + offset,
                               value, size, attrs);
    }
    return md_ecc_checked_read(injection, injection->address, offset, value,
                               size, attrs);
}

/* A write regenerates the ECC, so the doubleword no longer needs checking. */
static MemTxResult md_ecc_overlay_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIBedrockMDECCInjection *injection = opaque;
    SGIBedrockMDState *s = injection->md;
    uint64_t raw = injection->raw;
    MemTxResult result;
    uint64_t data;

    if (!injection->active) {
        return md_retired_write(s, raw * 8 + offset, value, size, attrs);
    }
    data = address_space_ldq_be(&s->node_as, raw * 8, attrs, &result);
    if (result != MEMTX_OK) {
        return result;
    }
    data = deposit64(data, (8 - offset - size) * 8, size * 8, value);
    address_space_stq_be(&s->node_as, raw * 8, data, attrs, &result);
    if (result != MEMTX_OK) {
        return result;
    }
    s->ecc[raw] = sgi_bedrock_md_ecc_compute(data);
    md_ecc_injection_clear(s, raw);
    md_node_memory_written(s, raw * 8, 8);
    return MEMTX_OK;
}

static const MemoryRegionOps md_ecc_overlay_ops = {
    .read_with_attrs = md_ecc_overlay_read,
    .write_with_attrs = md_ecc_overlay_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

/*
 * Installed ECC bytes follow the same software-to-locational bank mapping as
 * memory. The carrier packs byte
 * addresses in reverse order within each doubleword, so undo that transform
 * before applying the installed bound.
*/
static bool md_ecc_translate(const SGIBedrockMDState *s, hwaddr offset,
                             unsigned size, unsigned *index)
{
    unsigned software_bank = offset / MD_ECC_BANK_SIZE;
    unsigned selector = extract64(s->memory_config, 32, 2);
    unsigned locational_bank = software_bank ^ selector;
    unsigned size_code;
    uint64_t within;
    uint64_t raw = 0;

    if (!s->ecc || !md_memory_bank_enabled(s, locational_bank)) {
        return false;
    }
    size_code = md_memory_size_code(s, locational_bank);
    if (size_code == 0 || size_code > 5) {
        return false;
    }

    within = (offset % MD_ECC_BANK_SIZE) ^ (size == 1 ? 7 : 6);
    if (md_has_installed_bank_sizes(s)) {
        uint64_t installed_size;

        if (!md_installed_bank(s, locational_bank, &raw, &installed_size)) {
            return false;
        }
        installed_size /= 8;
        if (within >= MIN(md_memory_bank_size(size_code) / 8,
                          installed_size) ||
            size > MIN(md_memory_bank_size(size_code) / 8,
                       installed_size) - within) {
            return false;
        }
        raw /= 8;
        goto translated;
    }
    if (within >= md_memory_bank_size(size_code) / 8 ||
        size > md_memory_bank_size(size_code) / 8 - within) {
        return false;
    }

    for (unsigned bank = 0; bank < locational_bank; bank++) {
        unsigned earlier_code;

        if (!md_memory_bank_enabled(s, bank)) {
            continue;
        }
        earlier_code = md_memory_size_code(s, bank);
        if (earlier_code > 5) {
            return false;
        }
        if (earlier_code != 0) {
            raw += md_memory_bank_size(earlier_code) / 8;
        }
    }
translated:
    raw += within;
    if (raw >= s->ecc_entries || size > s->ecc_entries - raw) {
        return false;
    }
    *index = raw;
    return true;
}

static const char *md_ecc_refusal(const SGIBedrockMDState *s, hwaddr offset,
                                  unsigned size)
{
    unsigned index;

    if (size != 1 && size != 2) {
        return "unsupported width";
    }
    if (size == 2 && (offset & 1)) {
        return "unaligned halfword";
    }
    if (md_ecc_translate(s, offset, size, &index) ||
        (size == 2 && (offset == MD_ECC_ADMITTED_HALFWORD_LOW ||
                       offset == MD_ECC_ADMITTED_HALFWORD_HIGH))) {
        return NULL;
    }
    return "outside fitted memory";
}

static void md_ecc_log(hwaddr offset, unsigned size, bool is_write,
                       const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-bedrock-md: ECC backdoor %s offset 0x%08"
                  HWADDR_PRIx " size %u rejected: %s\n",
                  is_write ? "write" : "read", offset, size, reason);
}

static bool md_ecc_accepts(void *opaque, hwaddr offset, unsigned size,
                           bool is_write, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    const char *reason = md_ecc_refusal(s, offset, size);

    if (reason) {
        md_ecc_log(offset, size, is_write, reason);
        return false;
    }
    return true;
}

static uint64_t md_ecc_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (!md_ecc_translate(s, offset, size, &index)) {
        g_assert(size == 2 &&
                 (offset == MD_ECC_ADMITTED_HALFWORD_LOW ||
                  offset == MD_ECC_ADMITTED_HALFWORD_HIGH));
        if (offset == MD_ECC_ADMITTED_HALFWORD_LOW) {
            return s->ecc_admitted_halfword_low;
        }
        return s->ecc_admitted_halfword;
    }
    if (size == 1) {
        return md_ecc_byte(s, index);
    }
    /*
     * The halfword carrier at 6 modulo 8 holds the even doubleword's byte in
     * its low half and the odd doubleword's in
     * its high half: plain big-endian byte addressing of the ^7 byte
     * carriers, and the byte position the Origin 300's
     * memory diagnostic selects.
    */
    return ((uint16_t)md_ecc_byte(s, index + 1) << 8) | md_ecc_byte(s, index);
}

static void md_ecc_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (!md_ecc_translate(s, offset, size, &index)) {
        g_assert(size == 2 &&
                 (offset == MD_ECC_ADMITTED_HALFWORD_LOW ||
                  offset == MD_ECC_ADMITTED_HALFWORD_HIGH));
        if (offset == MD_ECC_ADMITTED_HALFWORD_LOW) {
            s->ecc_admitted_halfword_low = value;
            return;
        }
        s->ecc_admitted_halfword = value;
        return;
    }
    if (size == 1) {
        s->ecc[index] = value;
    } else {
        s->ecc[index] = value;
        s->ecc[index + 1] = value >> 8;
    }
    for (unsigned byte = 0; byte < size; byte++) {
        md_ecc_written(s, index + byte);
    }
}

/*
 * The scrub carrier at four eighths of the node (BDECC_SCRUB in the public
 * SN1 address header) indexes the ECC
 * bytes the way the entry carrier does. A byte or halfword read returns the
 * byte; an aligned 64-bit store scrubs
 * the 64-byte block whose eight bytes the carrier word holds, checking and
 * correcting each doubleword the way a
 * read would. The Origin 300's memory diagnostic stores zero here before
 * reading a corrected word back and reads
 * the check byte through it.
*/
static bool md_ecc_scrub_accepts(void *opaque, hwaddr offset, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    const char *reason;

    if (!is_write) {
        return md_ecc_accepts(opaque, offset, size, false, attrs);
    }
    if (size != 8) {
        reason = "unsupported width";
    } else if (offset % 8) {
        reason = "unaligned doubleword";
    } else {
        unsigned index;

        reason = md_ecc_translate(s, offset ^ 7, 1, &index) ? NULL :
                 "outside fitted memory";
    }
    if (reason) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: ECC scrub write offset 0x%08"
                      HWADDR_PRIx " size %u rejected: %s\n", offset, size,
                      reason);
        return false;
    }
    return true;
}

static void md_ecc_scrub_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    SGIBedrockMDState *s = opaque;
    uint64_t block = (offset / MD_ECC_BANK_SIZE) * MD_MEMORY_BANK_SIZE +
                     (offset % MD_ECC_BANK_SIZE) * 8;

    /* Doubleword j of the block has its byte at carrier offset 7 - j. */
    for (unsigned j = 0; j < 8; j++) {
        SGIBedrockMDECCInjection *injection;
        unsigned index;
        uint64_t data;

        if (!md_ecc_translate(s, offset + (7 - j), 1, &index)) {
            continue;
        }
        injection = md_ecc_injection_find(s, index);
        if (injection) {
            md_ecc_checked_read(injection, block + j * 8, 0, &data, 8,
                                MEMTXATTRS_UNSPECIFIED);
        }
    }
}

static const MemoryRegionOps md_ecc_scrub_ops = {
    .read = md_ecc_read,
    .write = md_ecc_scrub_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
        .accepts = md_ecc_scrub_accepts,
    },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

uint64_t sgi_bedrock_md_ecc_store_value(hwaddr offset, uint64_t value,
                                        unsigned size)
{
    uint64_t memory_address;

    if (size != 2) {
        return value;
    }

    memory_address = (offset ^ 6) * 8;
    switch (memory_address & 0x30) {
    case 0x10:
        return value >> 48;
    case 0x20:
        return value >> 32;
    case 0x30:
        return value >> 16;
    default:
        return value;
    }
}

static const MemoryRegionOps md_ecc_ops = {
    .read = md_ecc_read,
    .write = md_ecc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
        .accepts = md_ecc_accepts,
    },
    .impl = { .min_access_size = 1, .max_access_size = 2, .unaligned = true },
};

/* Bank select maps in either direction because XOR is its own inverse. */
static unsigned md_memory_remap_bank(const SGIBedrockMDState *s,
                                     unsigned bank)
{
    return bank ^ extract64(s->memory_config, 32, 2);
}

/*
 * Whether the selected bank is fitted and backed, and if so the raw range its
 * configured size covers: @base and
 * @size.
*/
static bool md_selftest_bank_fits(const SGIBedrockMDState *s,
                                  unsigned selector,
                                  unsigned configured_bank,
                                  uint64_t *base, uint64_t *size)
{
    uint64_t dense_base = 0;
    uint64_t raw_size;
    unsigned size_code;

    if (!s->node_mr || !md_memory_bank_enabled(s, configured_bank)) {
        return false;
    }

    size_code = md_memory_size_code(s, configured_bank);
    if (size_code < 1 || size_code > 5) {
        return false;
    }
    *size = md_memory_bank_size(size_code);
    if (md_has_installed_bank_sizes(s)) {
        uint64_t installed_size;

        if (!md_installed_bank(s, selector, &dense_base, &installed_size) ||
            md_memory_bank_size(size_code) > installed_size) {
            return false;
        }
        raw_size = memory_region_size(s->node_mr);
        *base = dense_base;
        return dense_base <= raw_size &&
               installed_size <= raw_size - dense_base;
    }

    for (unsigned bank = 0; bank < configured_bank; bank++) {
        unsigned earlier_code;
        uint64_t bank_size;

        if (!md_memory_bank_enabled(s, bank)) {
            continue;
        }
        earlier_code = md_memory_size_code(s, bank);
        if (earlier_code < 1 || earlier_code > 5) {
            return false;
        }
        bank_size = md_memory_bank_size(earlier_code);
        if (dense_base > UINT64_MAX - bank_size) {
            return false;
        }
        dense_base += bank_size;
    }

    raw_size = memory_region_size(s->node_mr);
    *base = dense_base;
    return dense_base <= raw_size &&
           md_memory_bank_size(size_code) <= raw_size - dense_base;
}

static void md_selftest_cancel(SGIBedrockMDState *s)
{
    timer_del(s->selftest_timer);
    s->selftest_running = false;
    s->selftest_selector = 0;
    s->selftest_phase = false;
    s->selftest_result = false;
    s->selftest_error_address = 0;
    s->selftest_status = 0;
}

/*
 * A write phase with initial pattern zero and no nibble, byte or cycle
 * control writes zero through the whole bank,
 * check bytes included, so it leaves no doubleword the ECC backdoor had
 * marked; like any data write it rewrites the
 * entry of each marked directory line. The stock PROM runs one over each bank
 * before and after its memory test; the
 * test leaves check bytes that disagree with their data behind it, and this
 * fill is what clears them before a
 * diagnostic reads the bank. The other patterns' transformation rules are not
 * published, so their phases leave
 * memory as it is.
*/
static void md_selftest_fill(SGIBedrockMDState *s, uint64_t base,
                             uint64_t size)
{
    if (address_space_set(&s->node_as, base, 0, size,
                          MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-md: self-test fill of "
                      "raw 0x%09" PRIx64 " size 0x%09" PRIx64 " failed\n",
                      base, size);
    }
    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        SGIBedrockMDECCInjection *injection = &s->ecc_injection[i];

        if (injection->active && injection->raw >= base / 8 &&
            injection->raw - base / 8 < size / 8) {
            s->ecc[injection->raw] = 0;
            md_ecc_injection_unplace(injection);
            md_ecc_injection_deactivate(s, injection);
        }
    }
    /* A data write rewrites the entry of every marked line it covers. */
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        SGIBedrockMDDirectoryInjection *line = &s->directory_injection[i];

        if (line->active &&
            (uint64_t)line->index >= base / MD_DIRECTORY_LINE &&
            line->index - base / MD_DIRECTORY_LINE < size / MD_DIRECTORY_LINE) {
            md_directory_line_written(s, line->index);
        }
    }
    memory_region_transaction_commit();
}

static void md_selftest_command(SGIBedrockMDState *s, uint64_t value)
{
    uint64_t base = 0;
    uint64_t size = 0;
    int64_t start_ns;

    if (value == 0x4 || value == 0x2) {
        md_selftest_cancel(s);
        return;
    }
    if ((value & MD_SELFTEST_COMMAND_BITS) != 0x1 ||
        value & ~MD_SELFTEST_COMMAND_MASK) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: unsupported self-test command "
                      "0x%" PRIx64 "\n", value);
        return;
    }

    /*
     * The interval runs from the start command, not from the end of the fill
     * below: the fill is part of the test, and
     * its host cost must not push the completion past a deadline firmware
     * sampled before the start.
    */
    start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->selftest_running = true;
    s->selftest_selector = extract64(value, 4, 3);
    s->selftest_phase = s->storage[MD_SELFTEST_DATA] & 0x80;
    s->selftest_result =
        (s->installed_memory_bank_mask & (1U << s->selftest_selector)) &&
        md_selftest_bank_fits(s, s->selftest_selector,
                              md_memory_remap_bank(
                                  s, s->selftest_selector),
                              &base, &size);
    if (s->selftest_result &&
        s->storage[MD_SELFTEST_DATA] == MD_SELFTEST_DATA_WRITE_ZERO) {
        md_selftest_fill(s, base, size);
    }
    s->selftest_error_address = 0;
    s->selftest_status = 0;
    timer_mod_ns(s->selftest_timer, start_ns + MD_SELFTEST_INTERVAL_NS);
}

static void md_selftest_complete(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    if (!s->selftest_running) {
        return;
    }
    s->selftest_running = false;
    s->selftest_error_address = s->selftest_result ? 0 : UINT64_C(1) << 48;
    s->selftest_status = MD_SELFTEST_DONE |
                         (s->selftest_result ? MD_SELFTEST_PASSED : 0);
}

/*
 * A read at or after the completion time sees the result even if the main
 * loop has not yet run the timer. Otherwise
 * a processor polling status could find the test still running after its own
 * clock had passed the completion, for
 * as long as the host kept the main loop from running.
*/
static void md_selftest_sync(SGIBedrockMDState *s)
{
    if (s->selftest_running &&
        timer_expired(s->selftest_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        timer_del(s->selftest_timer);
        md_selftest_complete(s);
    }
}

static MemTxResult md_memory_translate(SGIBedrockMDState *s, hwaddr offset,
                                       unsigned size, bool is_write,
                                       hwaddr *raw_offset)
{
    const char *access = is_write ? "write" : "read";
    unsigned software_bank = offset / MD_MEMORY_BANK_SIZE;
    unsigned locational_bank = md_memory_remap_bank(s, software_bank);
    unsigned size_code;
    uint64_t bank_size;
    uint64_t within = offset % MD_MEMORY_BANK_SIZE;
    uint64_t raw = 0;
    uint64_t raw_size;

    if (!s->node_mr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " rejected: node memory is not linked\n",
                      access, offset);
        return MEMTX_DECODE_ERROR;
    }
    if (!md_memory_bank_enabled(s, locational_bank)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " rejected: locational bank %u is "
                      "disabled\n", access, offset, locational_bank);
        return MEMTX_DECODE_ERROR;
    }

    size_code = md_memory_size_code(s, locational_bank);
    if (size_code == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " rejected: locational bank %u has size "
                      "code 0\n", access, offset, locational_bank);
        return MEMTX_DECODE_ERROR;
    }
    if (size_code > 5) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " rejected: locational bank %u has "
                      "unsupported size code %u\n", access, offset,
                      locational_bank, size_code);
        return MEMTX_DECODE_ERROR;
    }

    bank_size = md_memory_bank_size(size_code);
    if (md_has_installed_bank_sizes(s)) {
        uint64_t installed_size;

        if (!md_installed_bank(s, locational_bank, &raw, &installed_size)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bedrock-md: memory %s at offset 0x%08"
                          HWADDR_PRIx " rejected: locational bank %u is not "
                          "installed\n", access, offset, locational_bank);
            return MEMTX_DECODE_ERROR;
        }
        bank_size = MIN(bank_size, installed_size);
    }
    if (within >= bank_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " is beyond locational bank %u size 0x%08"
                      PRIx64 "\n", access, offset, locational_bank,
                      bank_size);
        return MEMTX_DECODE_ERROR;
    }
    if (size > bank_size - within) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " size %u crosses locational bank %u size "
                      "0x%08" PRIx64 "\n", access, offset, size,
                      locational_bank, bank_size);
        return MEMTX_DECODE_ERROR;
    }

    /* The linked RAM densely packs locational banks in bank order. */
    for (unsigned bank = 0;
         !md_has_installed_bank_sizes(s) && bank < locational_bank; bank++) {
        unsigned earlier_code;

        if (!md_memory_bank_enabled(s, bank)) {
            continue;
        }
        earlier_code = md_memory_size_code(s, bank);
        if (earlier_code > 5) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bedrock-md: memory %s at offset 0x%08"
                          HWADDR_PRIx " rejected: locational bank %u follows "
                          "bank %u with unsupported size code %u\n", access,
                          offset, locational_bank, bank, earlier_code);
            return MEMTX_DECODE_ERROR;
        }
        if (earlier_code != 0) {
            raw += md_memory_bank_size(earlier_code);
        }
    }
    raw += within;
    raw_size = memory_region_size(s->node_mr);
    if (raw >= raw_size || size > raw_size - raw) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: memory %s at offset 0x%08"
                      HWADDR_PRIx " rejected: locational bank %u exceeds raw "
                      "backing size 0x%08" PRIx64 "\n", access, offset,
                      locational_bank, raw_size);
        return MEMTX_DECODE_ERROR;
    }

    *raw_offset = raw;
    return MEMTX_OK;
}

/* A checked doubleword reached through the decoder rather than an alias. */
static SGIBedrockMDECCInjection *md_ecc_injection_within(SGIBedrockMDState *s,
                                                         hwaddr raw,
                                                         unsigned size)
{
    if (raw / 8 != (raw + size - 1) / 8) {
        return NULL;
    }
    return md_ecc_injection_find(s, raw / 8);
}

/*
 * Node memory was written outside the overlays' own paths, by the decoder or
 * a fetch-and-op store: the write
 * regenerated the ECC of the doublewords it covered and rewrote the directory
 * entry of every line it touched.
*/
static void md_node_memory_written(SGIBedrockMDState *s, uint64_t raw,
                                   unsigned size)
{
    uint64_t last = raw + size - 1;

    for (uint64_t index = raw / 8; index <= last / 8; index++) {
        md_ecc_injection_clear(s, index);
    }
    for (uint64_t line = raw / MD_DIRECTORY_LINE;
         line <= last / MD_DIRECTORY_LINE; line++) {
        md_directory_line_written(s, line);
    }
}

static MemTxResult md_memory_read(void *opaque, hwaddr offset, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    SGIBedrockMDECCInjection *injection;
    SGIBedrockMDDirectoryInjection *line;
    hwaddr raw_offset;
    MemTxResult result;

    result = md_memory_translate(s, offset, size, false, &raw_offset);
    if (result != MEMTX_OK) {
        *value = 0;
        return result;
    }
    injection = md_ecc_injection_within(s, raw_offset, size);
    if (injection) {
        return md_ecc_checked_read(injection, offset & ~UINT64_C(7),
                                   raw_offset % 8, value, size, attrs);
    }
    line = md_directory_injection_find(s, raw_offset / MD_DIRECTORY_LINE);
    if (line && raw_offset % MD_DIRECTORY_LINE + size <= MD_DIRECTORY_LINE) {
        return md_directory_checked_read(line, raw_offset % MD_DIRECTORY_LINE,
                                         value, size, attrs);
    }
    switch (size) {
    case 1:
        *value = address_space_ldub(&s->node_as, raw_offset, attrs, &result);
        break;
    case 2:
        *value = address_space_lduw_be(&s->node_as, raw_offset, attrs,
                                       &result);
        break;
    case 4:
        *value = address_space_ldl_be(&s->node_as, raw_offset, attrs, &result);
        break;
    case 8:
        *value = address_space_ldq_be(&s->node_as, raw_offset, attrs, &result);
        break;
    default:
        g_assert_not_reached();
    }
    return result;
}

static MemTxResult md_memory_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    SGIBedrockMDECCInjection *injection;
    hwaddr raw_offset;
    MemTxResult result;

    result = md_memory_translate(s, offset, size, true, &raw_offset);
    if (result != MEMTX_OK) {
        return result;
    }
    injection = md_ecc_injection_within(s, raw_offset, size);
    if (injection) {
        return md_ecc_overlay_write(injection, raw_offset % 8, value, size,
                                    attrs);
    }
    switch (size) {
    case 1:
        address_space_stb(&s->node_as, raw_offset, value, attrs, &result);
        break;
    case 2:
        address_space_stw_be(&s->node_as, raw_offset, value, attrs, &result);
        break;
    case 4:
        address_space_stl_be(&s->node_as, raw_offset, value, attrs, &result);
        break;
    case 8:
        address_space_stq_be(&s->node_as, raw_offset, value, attrs, &result);
        break;
    default:
        g_assert_not_reached();
    }
    if (result == MEMTX_OK) {
        md_node_memory_written(s, raw_offset, size);
    }
    return result;
}

static const MemoryRegionOps md_memory_ops = {
    .read_with_attrs = md_memory_read,
    .write_with_attrs = md_memory_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

/*
 * Directory state on processor requests.
 *
 * The unit rewrites a line's directory word as it serves each processor
 * request: a read miss, a read-exclusive or upgrade, a write-back. The
 * transitions are the Origin directory protocol's (the SGI Origin ccNUMA
 * paper, ISCA 1997, section 3.2): a read of an unowned line, or of one the
 * reader already owns, grants it exclusive; a read of a shared line adds
 * the reader; a read of a line another processor owns shares it between
 * the two; a read-exclusive or upgrade makes the requester the owner and
 * invalidates every other copy; an owner's write-back unowns the line. A
 * clean exclusive or shared copy is dropped without telling the directory.
 * The processors' requests are the R10000's (R10000 User's Manual version
 * 2.0, section 4.6 and Table 4-2), and depend on the reference's cache
 * algorithm, which each reference carries: under coherent exclusive-on-
 * write (5) a load miss reads shared and a store miss reads exclusive;
 * under coherent exclusive (4) a load miss reads exclusive too; a store to
 * a shared copy upgrades; noncoherent (3) and uncached (2, 7) references
 * make no coherent request. An access with no algorithm is a device's --
 * the block transfer engine's, say -- which the unit serves as an I/O
 * requester (MD_PTR_OWNER_IO in the public SN1 header), not a processor;
 * what it does to the word then is not modelled, and it changes none.
 *
 * Each processor's copy of a line is kept here, apart from the pages
 * being watched, because the directory word does not hold it: whether the
 * owner's copy is dirty, which decides whether a flush writes the line
 * back and unowns it or drops it silently, and whether a processor has
 * silently dropped a clean or shared copy, which decides whether a hit
 * cache instruction finds it, are processor cache state the directory
 * never records. A line first seen takes its copies from its word; after
 * that the copies persist across the page's overlay coming and going, and
 * only a reset discards them.
 *
 * Limits, each a departure from the hardware a guest could observe:
 *
 * - Loads and stores change the directory only on watched pages. A page
 *   is watched from a backdoor read of the word of a line a processor has
 *   named in a secondary-cache instruction, in a free slot or the least
 *   recently used one; a directory walk, which names nothing, watches
 *   nothing. Cache instructions and the copies they act on are followed
 *   on every line the model has kept copies for, watched or not.
 * - No copy is ever displaced by capacity: a processor keeps a line until
 *   a cache instruction or another processor's request takes it.
 * - An index instruction acts on every kept line in the named set,
 *   whichever way holds it, and reaches only the issuing node's unit and
 *   its peer's.
 * - A sharer bit covers both processors behind it; a copy taken from
 *   such a word is given to each of them that is fitted.
 * - A line first seen owned takes a clean copy for its owner, so a flush
 *   drops it without a write-back.
 * - A line followed by more than 16 holders is left alone.
 * - Requests complete at once; the busy and wait states never appear.
 * - Watched pages keep their overlay until displaced, and the processor
 *   runs each access to one as a device access.
 *
 * Words in any state the protocol above does not produce -- busy, wait,
 * poisoned, coarse or sparse, owned by an I/O device, or naming a
 * processor or interface the machine does not have -- are left alone. So
 * is a line whose word is marked with a check error while the directory
 * checks ECC; with the ignore-ECC bit set nothing is checked, so such a
 * line changes like any other and its check field is kept as written.
 */
/* The unit watches whole 4 KiB pages: one overlay covers 32 lines. */
#define MD_WATCH_PAGE 4096
#define MD_WATCH_PAGE_LINES (MD_WATCH_PAGE / MD_DIRECTORY_LINE)

enum {
    MD_CACHE_INVALID,
    MD_CACHE_SHARED,
    MD_CACHE_CLEAN,
    MD_CACHE_DIRTY,
};

enum {
    MD_REQUEST_READ,
    MD_REQUEST_READ_EXCLUSIVE,
    MD_REQUEST_WRITEBACK,
};

enum {
    MD_FORMAT_UNOWNED,
    MD_FORMAT_POINTER,
    MD_FORMAT_SHARED_FINE,
};

/* A decoded directory word, the parts of it the protocol above touches. */
typedef struct MDDirectoryEntry {
    unsigned format;
    uint16_t owner;
    uint64_t sharers;
} MDDirectoryEntry;

/*
 * A shared-fine vector bit stands for a processor interface: interface
 * one of node n is bit 2n + 1 across the tags. Inferred from the Origin
 * 300's directory diagnostic, which the memory-directory datasheet
 * records.
 */
static unsigned md_pointer_interface(uint16_t pointer)
{
    return (pointer >> 3) * 2 + ((pointer >> 2) & 1);
}

static uint64_t md_locprot_bit(bool premium)
{
    return UINT64_C(1) << (premium ? 38 : 22);
}

static bool md_shared_group(uint64_t *sharers, unsigned tag, uint64_t vector,
                            unsigned width)
{
    if (!vector) {
        return true;
    }
    if (tag * width >= 64) {
        return false;
    }
    *sharers |= vector << (tag * width);
    return true;
}

/* The public SN1 standard and premium layouts (hubmd_next.h, v2.5.40). */
static bool md_directory_decode(uint64_t word, bool premium,
                                MDDirectoryEntry *entry)
{
    unsigned width = premium ? 16 : 8;
    unsigned tag1, tag2;
    uint64_t vector1, vector2;
    bool tag2valid;

    entry->sharers = 0;
    entry->owner = 0;
    entry->format = word & 3;
    switch (entry->format) {
    case MD_FORMAT_UNOWNED:
        return true;
    case MD_FORMAT_POINTER:
        if (extract64(word, 2, 3) != 0) {
            return false;
        }
        entry->owner = extract64(word, 8, 8);
        if (premium) {
            entry->owner |= extract64(word, 22, 3) << 8;
        }
        /* Device bit 1 names an I/O owner (MD_PTR_OWNER_IO, hubmd_next.h). */
        return !(entry->owner & 2);
    case MD_FORMAT_SHARED_FINE:
        tag1 = extract64(word, 2, 3);
        tag2 = extract64(word, 5, 3);
        vector1 = extract64(word, 8, 8);
        if (premium) {
            tag1 |= extract64(word, 22, 2) << 3;
            tag2 |= extract64(word, 54, 2) << 3;
            vector1 |= extract64(word, 24, 8) << 8;
            vector2 = extract64(word, 40, 8) | extract64(word, 56, 8) << 8;
            tag2valid = extract64(word, 39, 1);
        } else {
            vector2 = extract64(word, 24, 8);
            tag2valid = extract64(word, 23, 1);
        }
        return md_shared_group(&entry->sharers, tag1, vector1, width) &&
               (!tag2valid ||
                md_shared_group(&entry->sharers, tag2, vector2, width));
    default:
        return false;
    }
}

/*
 * The word for @entry, keeping only the old word's local protection bit.
 * The priority, second pointer and critical-word fields a pointer entry
 * also carries, and a shared entry's unused second tag, are written zero.
 * That is model policy: the public header names the fields but not what
 * the unit puts in them, and the Origin 300's directory diagnostic, the
 * one reader of transitions, checks none of them.
 */
static bool md_directory_compose(const MDDirectoryEntry *entry, bool premium,
                                 uint64_t old, uint64_t *word)
{
    unsigned width = premium ? 16 : 8;
    uint64_t vector[2] = { 0, 0 };
    unsigned tag[2] = { 0, 0 };
    unsigned groups = 0;

    *word = old & md_locprot_bit(premium);
    switch (entry->format) {
    case MD_FORMAT_UNOWNED:
        break;
    case MD_FORMAT_POINTER:
        *word |= MD_FORMAT_POINTER |
                 (uint64_t)(entry->owner & 0xff) << 8;
        if (premium) {
            *word |= (uint64_t)((entry->owner >> 8) & 7) << 22;
        }
        break;
    case MD_FORMAT_SHARED_FINE:
        for (unsigned bit = 0; bit < 64; bit++) {
            unsigned group = bit / width;

            if (!(entry->sharers & (UINT64_C(1) << bit))) {
                continue;
            }
            if (groups && tag[groups - 1] == group) {
                vector[groups - 1] |= UINT64_C(1) << (bit % width);
                continue;
            }
            if (groups == 2) {
                return false;
            }
            tag[groups] = group;
            vector[groups++] = UINT64_C(1) << (bit % width);
        }
        *word |= MD_FORMAT_SHARED_FINE | (uint64_t)(tag[0] & 7) << 2 |
                 (uint64_t)(tag[1] & 7) << 5 | (vector[0] & 0xff) << 8;
        if (premium) {
            *word |= (uint64_t)(tag[0] >> 3) << 22 |
                     ((vector[0] >> 8) & 0xff) << 24 |
                     (uint64_t)(groups == 2) << 39 |
                     (vector[1] & 0xff) << 40 |
                     (uint64_t)(tag[1] >> 3) << 54 |
                     ((vector[1] >> 8) & 0xff) << 56;
        } else {
            *word |= (uint64_t)(groups == 2) << 23 |
                     (vector[1] & 0xff) << 24;
        }
        break;
    default:
        g_assert_not_reached();
    }
    return true;
}

/*
 * The processors' copies of line @index, created empty when @add. They
 * outlive the line's slot: only a reset discards them.
 */
static SGIBedrockMDLineState *md_line_state(SGIBedrockMDState *s,
                                            uint32_t index, bool add)
{
    SGIBedrockMDLineState *state =
        g_hash_table_lookup(s->line_states, &index);

    if (!state && add) {
        state = g_new0(SGIBedrockMDLineState, 1);
        state->index = index;
        g_hash_table_insert(s->line_states, &state->index, state);
        qatomic_set(&s->line_state_total, g_hash_table_size(s->line_states));
    }
    return state;
}

static SGIBedrockMDHolder *md_line_holder(SGIBedrockMDState *s,
                                          uint32_t index, uint16_t pointer,
                                          bool add)
{
    SGIBedrockMDLineState *state = md_line_state(s, index, add);

    if (!state) {
        return NULL;
    }
    for (unsigned i = 0; i < state->holders; i++) {
        if (state->holder[i].pointer == pointer) {
            return &state->holder[i];
        }
    }
    if (!add || state->holders == SGI_BEDROCK_MD_LINE_HOLDERS) {
        return NULL;
    }
    state->holder[state->holders].pointer = pointer;
    state->holder[state->holders].state = MD_CACHE_INVALID;
    return &state->holder[state->holders++];
}

/*
 * Whether every processor and interface @entry names is fitted. A word
 * naming one that is not came from software or a stale state, not from
 * the protocol, and the model does not guess what the unit would do.
 */
static bool md_directory_entry_present(SGIBedrockMDState *s,
                                       const MDDirectoryEntry *entry)
{
    if (!s->pointer_present) {
        return true;
    }
    if (entry->format == MD_FORMAT_POINTER) {
        return s->pointer_present(s->requester_opaque, entry->owner);
    }
    for (unsigned bit = 0; bit < 64; bit++) {
        uint16_t interface = (bit >> 1) << 3 | (bit & 1) << 2;

        if ((entry->sharers & (UINT64_C(1) << bit)) &&
            !s->pointer_present(s->requester_opaque, interface) &&
            !s->pointer_present(s->requester_opaque, interface | 1)) {
            return false;
        }
    }
    return true;
}

/* One line of a watched page: its raw index and software-bank address. */
typedef struct MDLine {
    uint32_t index;
    uint64_t address;
} MDLine;

static MDLine md_page_line(const SGIBedrockMDWatchedPage *page,
                           uint64_t offset)
{
    return (MDLine) {
        .index = page->index + offset / MD_DIRECTORY_LINE,
        .address = page->address +
                   (offset & ~(uint64_t)(MD_DIRECTORY_LINE - 1)),
    };
}

/* The directory backdoor offset of a watched line's word. */
static hwaddr md_line_directory_offset(const MDLine *line)
{
    return (line->address / MD_MEMORY_BANK_SIZE) * MD_DIRECTORY_BANK_SIZE +
           ((line->address % MD_MEMORY_BANK_SIZE) >> 3);
}

/*
 * Serve one request from @pointer against the line's directory word and
 * the other processors' copies, and return the state the requester's copy
 * is granted. A word the protocol does not produce keeps its value.
 */
static uint8_t md_directory_request(SGIBedrockMDState *s,
                                    const MDLine *line,
                                    uint16_t pointer, int request,
                                    bool prefetch)
{
    bool premium = md_directory_premium(s);
    uint64_t old = s->directory[line->index];
    uint64_t requester = UINT64_C(1) << md_pointer_interface(pointer);
    MDDirectoryEntry entry;
    SGIBedrockMDLineState *state;
    uint8_t granted;
    uint64_t word;

    if (!md_directory_decode(old, premium, &entry) ||
        md_pointer_interface(pointer) >= 64 ||
        !md_directory_entry_present(s, &entry)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: directory word 0x%016"
                      PRIx64 " of line %u is in a state the model does not "
                      "follow; left unchanged\n", old, line->index);
        return request == MD_REQUEST_READ ? MD_CACHE_SHARED :
                                            MD_CACHE_CLEAN;
    }
    switch (request) {
    case MD_REQUEST_READ:
        if (entry.format == MD_FORMAT_SHARED_FINE) {
            entry.sharers |= requester;
            granted = MD_CACHE_SHARED;
        } else if (entry.format == MD_FORMAT_POINTER &&
                   entry.owner != pointer) {
            SGIBedrockMDHolder *owner = md_line_holder(s, line->index,
                                                       entry.owner,
                                                       false);

            if (owner && owner->state != MD_CACHE_INVALID) {
                owner->state = MD_CACHE_SHARED;
            }
            entry.sharers = requester |
                            UINT64_C(1) << md_pointer_interface(entry.owner);
            entry.format = MD_FORMAT_SHARED_FINE;
            granted = MD_CACHE_SHARED;
        } else if (prefetch && entry.format == MD_FORMAT_UNOWNED) {
            /*
             * A read with the prefetch cause is served shared. Inferred
             * from the Origin 300's directory diagnostic alone.
             */
            entry.sharers = requester;
            entry.format = MD_FORMAT_SHARED_FINE;
            granted = MD_CACHE_SHARED;
        } else {
            entry.format = MD_FORMAT_POINTER;
            entry.owner = pointer;
            granted = MD_CACHE_CLEAN;
        }
        break;
    case MD_REQUEST_READ_EXCLUSIVE:
        state = md_line_state(s, line->index, false);
        for (unsigned i = 0; state && i < state->holders; i++) {
            if (state->holder[i].pointer != pointer) {
                state->holder[i].state = MD_CACHE_INVALID;
            }
        }
        entry.format = MD_FORMAT_POINTER;
        entry.owner = pointer;
        granted = MD_CACHE_CLEAN;
        break;
    case MD_REQUEST_WRITEBACK:
        if (entry.format == MD_FORMAT_POINTER && entry.owner == pointer) {
            entry.format = MD_FORMAT_UNOWNED;
        }
        granted = MD_CACHE_INVALID;
        break;
    default:
        g_assert_not_reached();
    }
    if (!md_directory_compose(&entry, premium, old, &word)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: sharers of line %u span "
                      "more than two tags; directory word left unchanged\n",
                      line->index);
        return granted;
    }
    /*
     * Written as a backdoor write is: with the ignore-ECC bit set the old
     * check field is kept and a word that is then not a codeword starts
     * being checked; otherwise the check field is regenerated.
     */
    if (s->directory_config & 2) {
        word |= old & md_directory_check_mask(premium);
    }
    s->directory[line->index] =
        md_directory_word_written(s, word, md_line_directory_offset(line),
                                  &line->index) & md_directory_mask(s);
    trace_sgi_bedrock_md_directory_request(line->index, pointer, request,
                                           old, s->directory[line->index]);
    return granted;
}

/*
 * With the directory's ignore-ECC bit set the unit checks no word, so a
 * line whose word is not a codeword is followed like any other; with the
 * bit clear such a line is left to the check-error path.
 */
static bool md_line_frozen(SGIBedrockMDState *s, uint32_t index)
{
    return !(s->directory_config & 2) &&
           md_directory_injection_find(s, index);
}

/*
 * A cached load or store reached the watched line. The requester is the
 * directory pointer the reference carries in its requester_id; a device's
 * access carries no cache algorithm and is left alone below.
 */
static void md_watched_access(SGIBedrockMDState *s,
                              SGIBedrockMDWatchedPage *page, hwaddr offset,
                              bool is_write, MemTxAttrs attrs)
{
    MDLine one = md_page_line(page, offset);
    const MDLine *line = &one;
    SGIBedrockMDHolder *holder;
    uint16_t pointer;

    /*
     * Only a processor's coherent cached reference, algorithm 4 or 5, is a
     * coherent request. Uncached references (2, 7) bypass the caches, a
     * noncoherent one (3) needs no coherency check (R10000 User's Manual
     * version 2.0, section 4.6 and Table 4-2), and an access with no
     * algorithm is a device's -- the block transfer engine's, say -- which
     * the unit serves as an I/O requester, not as a processor.
     */
    if ((attrs.mips_cca != 4 && attrs.mips_cca != 5) || attrs.debug ||
        md_line_frozen(s, line->index)) {
        return;
    }
    /* The processor's directory pointer, from its reference. */
    pointer = attrs.requester_id;
    page->last_use = ++s->use_clock;
    holder = md_line_holder(s, line->index, pointer, true);
    if (!holder) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: line %u has more holders "
                      "than the model follows\n", line->index);
        return;
    }
    if (!is_write) {
        if (holder->state != MD_CACHE_INVALID) {
            return;
        }
        /*
         * An instruction fetch arrives here as a read too and is served
         * as its algorithm's load miss. That is inferred: the manual
         * gives the request for load and store misses only (R10000 User's
         * Manual version 2.0, section 4.4, "Descriptions of the Cache
         * Algorithms"), never for an instruction miss.
         */
        if (attrs.mips_cca == 4) {
            /* Under coherent exclusive a load miss reads exclusive. */
            md_directory_request(s, line, pointer, MD_REQUEST_READ_EXCLUSIVE,
                                 false);
            holder->state = MD_CACHE_CLEAN;
            return;
        }
        holder->state = md_directory_request(s, line, pointer,
                                             MD_REQUEST_READ, false);
        return;
    }
    switch (holder->state) {
    case MD_CACHE_DIRTY:
        return;
    case MD_CACHE_CLEAN:
        /* A store to a clean exclusive copy dirties it with no request. */
        break;
    default:
        /* A miss asks to read exclusive, a shared copy to upgrade. */
        md_directory_request(s, line, pointer, MD_REQUEST_READ_EXCLUSIVE,
                             false);
        break;
    }
    holder->state = MD_CACHE_DIRTY;
}

static MemTxResult md_watched_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    SGIBedrockMDWatchedPage *page = opaque;
    MemTxResult result = md_memory_read(page->md, page->address + offset,
                                        value, size, attrs);

    if (result == MEMTX_OK && page->active) {
        md_watched_access(page->md, page, offset, false, attrs);
    }
    return result;
}

static MemTxResult md_watched_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    SGIBedrockMDWatchedPage *page = opaque;
    MemTxResult result = md_memory_write(page->md, page->address + offset,
                                         value, size, attrs);

    if (result == MEMTX_OK && page->active) {
        md_watched_access(page->md, page, offset, true, attrs);
    }
    return result;
}

static const MemoryRegionOps md_watched_ops = {
    .read_with_attrs = md_watched_read,
    .write_with_attrs = md_watched_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void md_watched_page_unplace(SGIBedrockMDState *s,
                                    SGIBedrockMDWatchedPage *line)
{
    if (line->placed) {
        memory_region_del_subregion(&s->ecc_overlay, &line->mr);
        line->placed = false;
    }
}

static void md_watched_count_update(SGIBedrockMDState *s)
{
    uint32_t count = 0;

    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        count += s->watched[i].active;
    }
    qatomic_set(&s->watched_count, count);
}

static void md_watched_pages_place(SGIBedrockMDState *s)
{
    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        SGIBedrockMDWatchedPage *line = &s->watched[i];

        if (line->active &&
            !md_overlay_place(s, &line->mr,
                              (uint64_t)line->index * MD_DIRECTORY_LINE,
                              &line->address, &line->placed, 2)) {
            line->active = false;
        }
    }
    md_watched_count_update(s);
}

/*
 * Stop following @line: remove its overlay and nothing else. The directory
 * word and the processors' copies stay exactly as they are; nothing on the
 * hardware happens when the model stops watching. The caller commits the
 * map change.
 */
static void md_watched_page_release(SGIBedrockMDState *s,
                                    SGIBedrockMDWatchedPage *line)
{
    line->active = false;
    md_watched_page_unplace(s, line);
}

static void md_watched_pages_clear(SGIBedrockMDState *s)
{
    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        md_watched_page_unplace(s, &s->watched[i]);
        s->watched[i].active = false;
    }
    memory_region_transaction_commit();
    md_watched_count_update(s);
}

/*
 * The copies a line followed for the first time must already have, read
 * from its directory word: the owner a pointer names holds the line
 * exclusive, and each processor on an interface a sharer bit names holds
 * it shared -- the vector cannot say which of an interface's two
 * processors has it, so both are taken to. Whether the owner's copy is
 * dirty the word does not say. It is taken to be clean, the one choice
 * that never changes the word by itself: a clean copy is dropped silently
 * where a dirty one would be written back, and a write-back the model
 * cannot know happened is a change hardware might not have made. A word
 * the protocol does not produce gives no copies.
 */
static void md_line_state_from_word(SGIBedrockMDState *s, uint32_t index)
{
    MDDirectoryEntry entry;
    SGIBedrockMDHolder *holder;

    if (md_line_state(s, index, false) || !s->pointer_present ||
        !md_directory_decode(s->directory[index], md_directory_premium(s),
                             &entry) ||
        !md_directory_entry_present(s, &entry)) {
        return;
    }
    if (entry.format == MD_FORMAT_POINTER) {
        holder = md_line_holder(s, index, entry.owner, true);
        if (holder) {
            holder->state = MD_CACHE_CLEAN;
        }
        return;
    }
    for (unsigned bit = 0; bit < 64; bit++) {
        uint16_t interface = (bit >> 1) << 3 | (bit & 1) << 2;

        if (!(entry.sharers & (UINT64_C(1) << bit))) {
            continue;
        }
        for (uint16_t slot = 0; slot < 2; slot++) {
            if (s->pointer_present(s->requester_opaque, interface | slot)) {
                holder = md_line_holder(s, index, interface | slot, true);
                if (holder) {
                    holder->state = MD_CACHE_SHARED;
                }
            }
        }
    }
}

/*
 * A reset, or a stream being loaded, discards every line's copies and
 * every naming.
 */
static void md_line_states_clear(SGIBedrockMDState *s)
{
    g_hash_table_remove_all(s->line_states);
    qatomic_set(&s->line_state_total, 0);
    if (s->named_lines) {
        bitmap_zero(s->named_lines, s->directory_entries);
    }
}

/* The raw line the software-bank address @address falls in, if backed. */
static bool md_address_line(SGIBedrockMDState *s, uint64_t address,
                            uint32_t *index)
{
    unsigned bank = address / MD_MEMORY_BANK_SIZE;
    uint64_t within = address % MD_MEMORY_BANK_SIZE;

    if (bank >= 8 || within >= s->bank_alias_size[bank]) {
        return false;
    }
    *index = (s->bank_alias_base[bank] + within) / MD_DIRECTORY_LINE;
    return *index < s->directory_entries;
}

void sgi_bedrock_md_name_line(SGIBedrockMDState *s, uint64_t address)
{
    uint32_t index;

    if (!s->named_lines || !md_address_line(s, address, &index)) {
        return;
    }
    /*
     * Test before setting. This is a timing measure, not a correctness
     * rule: the bit ends up the same either way. The firmware's
     * cache-flush loops name the same lines over and over from every
     * processor, and on an eight-processor two-chassis pair an atomic
     * write every time slowed them enough that the second node missed the
     * firmware's global barrier ("Global barrier failed at line 5300",
     * then a General Exception on node 0), as did taking the lock here.
     * The one probe run with the test passed its boot.
     */
    if (!test_bit(index, s->named_lines)) {
        set_bit_atomic(index, s->named_lines);
    }
}

bool sgi_bedrock_md_following(SGIBedrockMDState *s)
{
    return qatomic_read(&s->watched_count) != 0 ||
           qatomic_read(&s->line_state_total) != 0;
}

/* The software-bank address the bank aliases give raw line @index. */
static bool md_line_address(SGIBedrockMDState *s, uint32_t index,
                            uint64_t *address)
{
    uint64_t raw = (uint64_t)index * MD_DIRECTORY_LINE;

    for (unsigned bank = 0; bank < 8; bank++) {
        if (s->bank_alias_size[bank] &&
            raw >= s->bank_alias_base[bank] &&
            raw - s->bank_alias_base[bank] < s->bank_alias_size[bank]) {
            *address = bank * MD_MEMORY_BANK_SIZE +
                       (raw - s->bank_alias_base[bank]);
            return true;
        }
    }
    return false;
}

/* The watched page holding the line at software-bank address @address. */
static SGIBedrockMDWatchedPage *md_watched_page_at(SGIBedrockMDState *s,
                                                   uint64_t address)
{
    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        SGIBedrockMDWatchedPage *page = &s->watched[i];

        if (page->active &&
            page->address == (address & ~(uint64_t)(MD_WATCH_PAGE - 1))) {
            return page;
        }
    }
    return NULL;
}

/*
 * A secondary-cache instruction acting on @pointer's copy of @line: a
 * dirty copy is written back and a clean or shared one dropped without a
 * word to the directory; Hit Invalidate drops even a dirty one silently
 * (R10000 User's Manual, the processor block write and eliminate
 * protocols). That this system sends no eliminate requests is inferred
 * from the Origin 300's directory diagnostic. Returns whether the
 * processor held the line.
 */
static bool md_cache_instruction(SGIBedrockMDState *s, const MDLine *line,
                                 uint16_t pointer,
                                 SGIBedrockMDProcessorRequest request)
{
    SGIBedrockMDHolder *holder = md_line_holder(s, line->index, pointer,
                                                false);
    bool held;

    if (!holder || holder->state == MD_CACHE_INVALID) {
        return false;
    }
    held = true;
    if (request != SGI_BEDROCK_MD_HIT_INVALIDATE &&
        holder->state == MD_CACHE_DIRTY) {
        md_directory_request(s, line, pointer, MD_REQUEST_WRITEBACK, false);
    }
    holder->state = MD_CACHE_INVALID;
    return held;
}

/* Ascending software-bank address, the order an index instruction takes. */
static gint md_line_address_compare(gconstpointer a, gconstpointer b)
{
    const MDLine *x = a, *y = b;

    return x->address < y->address ? -1 : x->address > y->address;
}

/*
 * The followed lines an index instruction for @address reaches: every line
 * with copies whose address, by the first bank alias that maps it, agrees
 * with @address in the set bits @index_mask selects, unless its checks are
 * frozen. In ascending address order.
 *
 * The set bits are contiguous from the line size up, so the lines of one set
 * sit one set span apart in each bank, and there are far fewer of them than
 * lines followed once a boot has run a while: firmware flush loops issue
 * index instructions by the thousand, and each one walking every followed
 * line under the lock would cost time next to the firmware's timing-bound
 * barriers. So the lines of the set are looked up by address when that is
 * the shorter list, and the followed lines are walked otherwise.
 */
static GArray *md_index_lines(SGIBedrockMDState *s, uint64_t address,
                              uint64_t index_mask)
{
    GArray *lines = g_array_new(false, false, sizeof(MDLine));
    uint64_t span = index_mask + MD_DIRECTORY_LINE;
    uint64_t set = address & index_mask;
    uint64_t candidates = 0;
    MDLine line;

    if (is_power_of_2(span) && !(index_mask & (MD_DIRECTORY_LINE - 1))) {
        for (unsigned bank = 0; bank < 8; bank++) {
            candidates += DIV_ROUND_UP(s->bank_alias_size[bank], span);
        }
    } else {
        candidates = UINT64_MAX;
    }

    if (candidates < g_hash_table_size(s->line_states)) {
        for (unsigned bank = 0; bank < 8; bank++) {
            uint64_t start = bank * MD_MEMORY_BANK_SIZE;

            for (uint64_t within = (set - start) & (span - 1);
                 within < s->bank_alias_size[bank]; within += span) {
                uint64_t raw = s->bank_alias_base[bank] + within;
                uint64_t canonical;

                line.index = raw / MD_DIRECTORY_LINE;
                line.address = start + within;
                if (raw / MD_DIRECTORY_LINE >= s->directory_entries ||
                    !md_line_state(s, line.index, false) ||
                    !md_line_address(s, line.index, &canonical) ||
                    canonical != line.address ||
                    md_line_frozen(s, line.index)) {
                    continue;
                }
                g_array_append_val(lines, line);
            }
        }
    } else {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, s->line_states);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            SGIBedrockMDLineState *state = value;

            line.index = state->index;
            if (md_line_address(s, line.index, &line.address) &&
                !((line.address ^ address) & index_mask) &&
                !md_line_frozen(s, line.index)) {
                g_array_append_val(lines, line);
            }
        }
        g_array_sort(lines, md_line_address_compare);
    }
    return lines;
}

/*
 * A processor's secondary-cache instruction acts on its copies whether or
 * not the unit is watching their pages, as the processor's cache would:
 * the copies are kept apart from the pages, and following them only on a
 * watched page would leave a dirty copy to be written back later than the
 * processor wrote it. A hit instruction names the line at @address; an
 * index instruction names a set, and acts on every line the processor
 * holds whose address agrees with @address in the set bits @index_mask
 * selects. A prefetch is a request for the line like a load or store, so
 * it is served only on a watched page. A prefetch with a load hint asks to
 * read, with a store hint to read exclusive, and a copy already held needs
 * neither.
 */
bool sgi_bedrock_md_processor_request(SGIBedrockMDState *s, uint16_t pointer,
                                      SGIBedrockMDProcessorRequest request,
                                      uint64_t address, uint64_t index_mask)
{
    SGIBedrockMDWatchedPage *page;
    SGIBedrockMDHolder *holder;
    MDLine line;
    bool found = false;

    if (request == SGI_BEDROCK_MD_INDEX_WRITEBACK_INVALIDATE) {
        g_autoptr(GArray) lines = md_index_lines(s, address, index_mask);

        for (guint i = 0; i < lines->len; i++) {
            MDLine *one = &g_array_index(lines, MDLine, i);

            found |= md_cache_instruction(s, one, pointer, request);
            page = md_watched_page_at(s, one->address);
            if (page) {
                page->last_use = ++s->use_clock;
            }
        }
        return found;
    }
    if (!md_address_line(s, address, &line.index)) {
        return false;
    }
    line.address = address & ~(uint64_t)(MD_DIRECTORY_LINE - 1);
    if (md_line_frozen(s, line.index)) {
        return false;
    }
    page = md_watched_page_at(s, line.address);
    if (page) {
        page->last_use = ++s->use_clock;
    }
    switch (request) {
    case SGI_BEDROCK_MD_HIT_WRITEBACK_INVALIDATE:
    case SGI_BEDROCK_MD_HIT_INVALIDATE:
        return md_cache_instruction(s, &line, pointer, request);
    case SGI_BEDROCK_MD_PREFETCH_LOAD:
    case SGI_BEDROCK_MD_PREFETCH_STORE:
        if (!page) {
            return false;
        }
        holder = md_line_holder(s, line.index, pointer, true);
        if (!holder) {
            return false;
        }
        if (request == SGI_BEDROCK_MD_PREFETCH_LOAD) {
            if (holder->state == MD_CACHE_INVALID) {
                holder->state = md_directory_request(s, &line, pointer,
                                                     MD_REQUEST_READ, true);
            }
        } else if (holder->state != MD_CACHE_CLEAN &&
                   holder->state != MD_CACHE_DIRTY) {
            md_directory_request(s, &line, pointer,
                                 MD_REQUEST_READ_EXCLUSIVE, false);
            holder->state = MD_CACHE_CLEAN;
        }
        return false;
    default:
        g_assert_not_reached();
    }
}

/*
 * Software read line @index's directory word through the backdoor. A word
 * on a watched page keeps the page recently used. Otherwise, if a
 * processor has named the line in a cache instruction since it last let a
 * page be watched, watch its page from here, in a free slot or the least
 * recently used one; only the overlay moves, and the words and copies of
 * the page it leaves stay as they are. A word nobody has named -- a
 * directory walk's -- watches nothing, and a naming lets one page be
 * watched once, so a walk over lines firmware once flushed watches none
 * of their pages again.
 */
static void md_watch_line(SGIBedrockMDState *s, uint32_t line_index)
{
    uint32_t index = line_index & ~(uint32_t)(MD_WATCH_PAGE_LINES - 1);
    SGIBedrockMDWatchedPage *slot = NULL;

    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        SGIBedrockMDWatchedPage *page = &s->watched[i];

        if (page->active && page->index == index) {
            /* Any word read on a watched page keeps it recently used. */
            page->last_use = ++s->use_clock;
            if (s->named_lines) {
                clear_bit_atomic(line_index, s->named_lines);
            }
            return;
        }
        if (!slot || (slot->active &&
                      (!page->active || page->last_use < slot->last_use))) {
            slot = page;
        }
    }
    if (!s->named_lines || !test_bit(line_index, s->named_lines)) {
        return;
    }
    /* A naming is spent on the page it watches. */
    clear_bit_atomic(line_index, s->named_lines);
    memory_region_transaction_begin();
    md_watched_page_release(s, slot);
    slot->index = index;
    slot->last_use = ++s->use_clock;
    slot->active = md_overlay_place(s, &slot->mr,
                                    (uint64_t)index * MD_DIRECTORY_LINE,
                                    &slot->address, &slot->placed, 2);
    memory_region_transaction_commit();
    for (uint32_t line = index;
         slot->active && line < index + MD_WATCH_PAGE_LINES &&
         line < s->directory_entries; line++) {
        if (!md_line_frozen(s, line)) {
            md_line_state_from_word(s, line);
        }
    }
    md_watched_count_update(s);
    trace_sgi_bedrock_md_watch_page(index, slot->address, slot->active);
}

/*
 * Memory-special space: a fetch-and-op variable is the first doubleword of
 * each 64-byte block of node memory, and
 * the offset inside the block names the atomic operation. Only aligned 64-bit
 * and 32-bit accesses reach the
 * variable; anything else, and an offset with no operation, is refused as a
 * bus error.
*/
static bool md_fetchop_variable(SGIBedrockMDState *s, hwaddr offset,
                                unsigned size, bool is_write, hwaddr *raw)
{
    const char *access = is_write ? "write" : "read";
    const char *reason = NULL;

    if (size != 8 && size != 4) {
        reason = "not a 64-bit or 32-bit access";
    } else if (offset % size) {
        reason = "unaligned";
    }
    if (reason) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-md: fetch-and-op %s at offset 0x%09"
                      HWADDR_PRIx " size %u rejected: %s\n",
                      access, offset, size, reason);
        return false;
    }
    return md_memory_translate(s, offset & ~(SGI_BEDROCK_MD_FETCHOP_BLOCK - 1),
                               8, is_write, raw) == MEMTX_OK;
}

static void md_fetchop_refuse_operation(hwaddr offset, bool is_write)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-bedrock-md: fetch-and-op %s at offset 0x%09"
                  HWADDR_PRIx " rejected: no such operation\n",
                  is_write ? "write" : "read", offset);
}

/*
 * A 32-bit access names one word of the variable with address bit 2 (the high
 * word at 0, the low word at 4, in the
 * big-endian order the memory holds) and operates on that word alone, as a
 * 32-bit variable; the operation is still
 * the block offset's, bits 5:3. A 64-bit access operates on the whole
 * doubleword.
*/
static unsigned md_fetchop_shift(hwaddr offset, unsigned size)
{
    return size == 8 || offset & 4 ? 0 : 32;
}

static uint64_t md_fetchop_mask(unsigned size)
{
    return size == 8 ? UINT64_MAX : UINT32_MAX;
}

static uint64_t md_fetchop_get(hwaddr offset, unsigned size, uint64_t variable)
{
    return (variable >> md_fetchop_shift(offset, size)) &
           md_fetchop_mask(size);
}

static uint64_t md_fetchop_put(hwaddr offset, unsigned size,
                               uint64_t variable, uint64_t operand)
{
    unsigned shift = md_fetchop_shift(offset, size);
    uint64_t mask = md_fetchop_mask(size) << shift;

    return (variable & ~mask) | ((operand << shift) & mask);
}

static MemTxResult md_fetchop_read(void *opaque, hwaddr offset,
                                   uint64_t *value, unsigned size,
                                   MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    MemTxResult result = MEMTX_OK;
    hwaddr raw;
    uint64_t old;
    uint64_t operand;

    *value = 0;
    if (!md_fetchop_variable(s, offset, size, false, &raw)) {
        return MEMTX_DECODE_ERROR;
    }
    old = address_space_ldq_be(&s->node_as, raw, attrs, &result);
    if (result != MEMTX_OK) {
        return result;
    }
    operand = md_fetchop_get(offset, size, old);
    switch (offset % SGI_BEDROCK_MD_FETCHOP_BLOCK & ~7) {
    case 0:
        *value = operand;
        return MEMTX_OK;
    case 8:
        operand++;
        break;
    case 16:
        operand--;
        break;
    case 24:
        operand = 0;
        break;
    default:
        md_fetchop_refuse_operation(offset, false);
        return MEMTX_DECODE_ERROR;
    }
    address_space_stq_be(&s->node_as, raw,
                         md_fetchop_put(offset, size, old, operand), attrs,
                         &result);
    if (result == MEMTX_OK) {
        md_node_memory_written(s, raw, 8);
        *value = md_fetchop_get(offset, size, old);
    }
    return result;
}

/* The stored data reaches the addressed word only; the other is unchanged. */
static MemTxResult md_fetchop_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    MemTxResult result = MEMTX_OK;
    hwaddr raw;
    uint64_t old;
    uint64_t operand;

    if (!md_fetchop_variable(s, offset, size, true, &raw)) {
        return MEMTX_DECODE_ERROR;
    }
    old = address_space_ldq_be(&s->node_as, raw, attrs, &result);
    if (result != MEMTX_OK) {
        return result;
    }
    operand = md_fetchop_get(offset, size, old);
    switch (offset % SGI_BEDROCK_MD_FETCHOP_BLOCK & ~7) {
    case 0:
        operand = value;
        break;
    case 8:
        operand++;
        break;
    case 16:
        operand--;
        break;
    case 24:
        operand &= value;
        break;
    case 32:
        operand |= value;
        break;
    default:
        md_fetchop_refuse_operation(offset, true);
        return MEMTX_DECODE_ERROR;
    }
    address_space_stq_be(&s->node_as, raw,
                         md_fetchop_put(offset, size, old, operand), attrs,
                         &result);
    if (result == MEMTX_OK) {
        md_node_memory_written(s, raw, 8);
    }
    return result;
}

static const MemoryRegionOps md_fetchop_ops = {
    .read_with_attrs = md_fetchop_read,
    .write_with_attrs = md_fetchop_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void md_reset_registers(SGIBedrockMDState *s)
{
    md_selftest_cancel(s);
    timer_del(s->retired_timer);
    memory_region_transaction_begin();
    md_ecc_injections_clear(s);
    md_directory_injections_clear(s);
    md_watched_pages_clear(s);
    md_line_states_clear(s);
    s->use_clock = 0;
    s->memory_error = 0;
    s->directory_error = 0;
    s->current_cell = UINT64_MAX;
    s->memory_config = 0;
    md_update_memory_aliases(s);
    memory_region_transaction_commit();
    s->refresh = 0;
    s->directory_config = 0;
    memset(s->storage, 0, sizeof(s->storage));
}

void sgi_bedrock_md_soft_reset(SGIBedrockMDState *s)
{
    uint64_t memory_config = s->memory_config;
    uint64_t refresh = s->refresh & ((UINT64_C(1) << 63) | 0xfff);
    uint64_t directory_config = s->directory_config;

    md_reset_registers(s);
    s->memory_config = memory_config;
    md_update_memory_aliases(s);
    s->refresh = refresh;
    s->directory_config = directory_config;
}

static void md_soft_reset_input(void *opaque, int n, int level)
{
    if (level) {
        sgi_bedrock_md_soft_reset(opaque);
    }
}

static void md_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(obj);

    md_reset_registers(s);
    if (s->directory) {
        memset(s->directory, 0,
               s->directory_entries * sizeof(*s->directory));
        memset(s->page_counters, 0,
               s->directory_entries * sizeof(*s->page_counters));
    }
    memset(s->directory_compat_offsets, 0,
           sizeof(s->directory_compat_offsets));
    memset(s->directory_compat_values, 0,
           sizeof(s->directory_compat_values));
    s->directory_compat_count = 0;
    if (s->ecc) {
        memset(s->ecc, 0, s->ecc_entries * sizeof(*s->ecc));
    }
    s->ecc_admitted_halfword_low = 0;
    s->ecc_admitted_halfword = 0;
    memset(s->protection, 0,
           s->protection_entries * sizeof(*s->protection));
}

static int md_post_load(void *opaque, int version_id)
{
    SGIBedrockMDState *s = opaque;

    /*
     * The lookup tables are derived from the slots, which the stream set.
     * Empty them first so a rejected stream leaves
     * no key pointing at a slot the stream rewrote.
    */
    g_hash_table_remove_all(s->ecc_injection_by_raw);
    g_hash_table_remove_all(s->directory_injection_by_index);
    g_hash_table_remove_all(s->ecc_retired_by_raw);
    g_hash_table_remove_all(s->directory_retired_by_index);
    timer_del(s->retired_timer);
    /* The stream carries marks, not retired overlays: none survives it. */
    md_retired_flush_all(s);
    /* The stream carries which lines are watched, not where they sit. */
    memory_region_transaction_begin();
    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        md_watched_page_unplace(s, &s->watched[i]);
        if (s->watched[i].active &&
            s->watched[i].index >= s->directory_entries) {
            memory_region_transaction_commit();
            return -EINVAL;
        }
    }
    memory_region_transaction_commit();
    if (s->line_state_count < 0) {
        s->line_state_count = 0;
        return -EINVAL;
    }
    for (int32_t i = 0; i < s->line_state_count; i++) {
        SGIBedrockMDLineState *state = &s->line_state_array[i];

        if (state->index >= s->directory_entries ||
            state->holders > SGI_BEDROCK_MD_LINE_HOLDERS ||
            g_hash_table_contains(s->line_states, &state->index)) {
            g_clear_pointer(&s->line_state_array, g_free);
            s->line_state_count = 0;
            return -EINVAL;
        }
        state = g_memdup2(state, sizeof(*state));
        g_hash_table_insert(s->line_states, &state->index, state);
        qatomic_set(&s->line_state_total,
                    g_hash_table_size(s->line_states));
    }
    g_clear_pointer(&s->line_state_array, g_free);
    s->line_state_count = 0;
    if (s->directory_compat_count >
        SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        SGIBedrockMDECCInjection *injection = &s->ecc_injection[i];

        if (injection->active &&
            !g_hash_table_insert(s->ecc_injection_by_raw, &injection->raw,
                                 injection)) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        SGIBedrockMDDirectoryInjection *line = &s->directory_injection[i];

        if (line->active &&
            !g_hash_table_insert(s->directory_injection_by_index,
                                 &line->index, line)) {
            return -EINVAL;
        }
    }
    md_update_memory_aliases(s);
    return 0;
}

static const VMStateDescription vmstate_md_ecc_injection = {
    .name = TYPE_SGI_BEDROCK_MD "-ecc-injection",
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(raw, SGIBedrockMDECCInjection),
        VMSTATE_BOOL(active, SGIBedrockMDECCInjection),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_md_directory_injection = {
    .name = TYPE_SGI_BEDROCK_MD "-directory-injection",
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(offset, SGIBedrockMDDirectoryInjection),
        VMSTATE_UINT32(index, SGIBedrockMDDirectoryInjection),
        VMSTATE_BOOL(active, SGIBedrockMDDirectoryInjection),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_md_holder = {
    .name = TYPE_SGI_BEDROCK_MD "-holder",
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(pointer, SGIBedrockMDHolder),
        VMSTATE_UINT8(state, SGIBedrockMDHolder),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_md_watched_page = {
    .name = TYPE_SGI_BEDROCK_MD "-watched-page",
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(index, SGIBedrockMDWatchedPage),
        VMSTATE_BOOL(active, SGIBedrockMDWatchedPage),
        VMSTATE_UINT64(last_use, SGIBedrockMDWatchedPage),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_md_line_state = {
    .name = TYPE_SGI_BEDROCK_MD "-line-state",
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(index, SGIBedrockMDLineState),
        VMSTATE_UINT8(holders, SGIBedrockMDLineState),
        VMSTATE_STRUCT_ARRAY(holder, SGIBedrockMDLineState,
                             SGI_BEDROCK_MD_LINE_HOLDERS, 1,
                             vmstate_md_holder, SGIBedrockMDHolder),
        VMSTATE_END_OF_LIST()
    },
};

static bool md_watched_needed(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    return s->watched_count != 0;
}

static const VMStateDescription vmstate_md_watched = {
    .name = TYPE_SGI_BEDROCK_MD "/watched",
    .version_id = 1, .minimum_version_id = 1,
    .needed = md_watched_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(watched, SGIBedrockMDState,
                             SGI_BEDROCK_MD_WATCHED_PAGES, 1,
                             vmstate_md_watched_page,
                             SGIBedrockMDWatchedPage),
        VMSTATE_UINT64(use_clock, SGIBedrockMDState),
        VMSTATE_END_OF_LIST()
    },
};

static bool md_line_states_needed(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    return g_hash_table_size(s->line_states) != 0 ||
           (s->named_lines &&
            !bitmap_empty(s->named_lines, s->directory_entries));
}

/* The per-line copies travel flattened into an array. */
static int md_line_states_pre_save(void *opaque)
{
    SGIBedrockMDState *s = opaque;
    GHashTableIter iter;
    gpointer value;
    uint32_t i = 0;

    g_free(s->line_state_array);
    s->line_state_count = g_hash_table_size(s->line_states);
    s->line_state_array = g_new(SGIBedrockMDLineState, s->line_state_count);
    g_hash_table_iter_init(&iter, s->line_states);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        s->line_state_array[i++] = *(SGIBedrockMDLineState *)value;
    }
    return 0;
}

static void md_line_states_post_save(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    g_clear_pointer(&s->line_state_array, g_free);
    s->line_state_count = 0;
}

static const VMStateDescription vmstate_md_line_states = {
    .name = TYPE_SGI_BEDROCK_MD "/line-states",
    .version_id = 1, .minimum_version_id = 1,
    .needed = md_line_states_needed,
    .pre_save = md_line_states_pre_save,
    .post_save = md_line_states_post_save,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(line_state_count, SGIBedrockMDState),
        VMSTATE_STRUCT_VARRAY_ALLOC(line_state_array, SGIBedrockMDState,
                                    line_state_count, 1,
                                    vmstate_md_line_state,
                                    SGIBedrockMDLineState),
        VMSTATE_BITMAP(named_lines, SGIBedrockMDState, 1, named_line_bits),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The watched lines, the flushed-line record and the per-line copies travel
 * in subsections sent only when there are any, so a stream without them
 * must leave none.
 */
static int md_pre_load(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    md_watched_pages_clear(s);
    md_line_states_clear(s);
    g_clear_pointer(&s->line_state_array, g_free);
    s->line_state_count = 0;
    s->use_clock = 0;
    return 0;
}

static const VMStateDescription vmstate_md = {
    .name = TYPE_SGI_BEDROCK_MD, .version_id = 8, .minimum_version_id = 1,
    .pre_load = md_pre_load,
    .post_load = md_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(current_cell, SGIBedrockMDState),
        VMSTATE_UINT64(memory_config, SGIBedrockMDState),
        VMSTATE_UINT64(refresh, SGIBedrockMDState),
        VMSTATE_UINT64(directory_config, SGIBedrockMDState),
        VMSTATE_UINT64_ARRAY(storage, SGIBedrockMDState,
                             SGI_BEDROCK_MD_STORAGE),
        VMSTATE_VARRAY_UINT32(directory, SGIBedrockMDState,
                              directory_entries, 2,
                              vmstate_info_uint64, uint64_t),
        VMSTATE_VARRAY_UINT32(page_counters, SGIBedrockMDState,
                              directory_entries, 3,
                              vmstate_info_uint64, uint64_t),
        VMSTATE_VARRAY_UINT32(protection, SGIBedrockMDState,
                              protection_entries, 4,
                              vmstate_info_uint64, uint64_t),
        VMSTATE_UINT64_ARRAY_V(directory_compat_offsets, SGIBedrockMDState,
                              SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES, 5),
        VMSTATE_UINT64_ARRAY_V(directory_compat_values, SGIBedrockMDState,
                              SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES, 5),
        VMSTATE_UINT8_V(directory_compat_count, SGIBedrockMDState, 5),
        VMSTATE_VARRAY_UINT32(ecc, SGIBedrockMDState, ecc_entries, 5,
                              vmstate_info_uint8, uint8_t),
        VMSTATE_UINT16_V(ecc_admitted_halfword, SGIBedrockMDState, 5),
        VMSTATE_UINT16_V(ecc_admitted_halfword_low, SGIBedrockMDState, 6),
        VMSTATE_BOOL_V(selftest_running, SGIBedrockMDState, 5),
        VMSTATE_UINT8_V(selftest_selector, SGIBedrockMDState, 5),
        VMSTATE_BOOL_V(selftest_phase, SGIBedrockMDState, 5),
        VMSTATE_BOOL_V(selftest_result, SGIBedrockMDState, 5),
        VMSTATE_UINT64_V(selftest_error_address, SGIBedrockMDState, 7),
        VMSTATE_UINT8_V(selftest_status, SGIBedrockMDState, 5),
        VMSTATE_TIMER_PTR_V(selftest_timer, SGIBedrockMDState, 5),
        VMSTATE_UINT64_V(memory_error, SGIBedrockMDState, 8),
        VMSTATE_STRUCT_ARRAY(ecc_injection, SGIBedrockMDState,
                             SGI_BEDROCK_MD_ECC_INJECTIONS, 8,
                             vmstate_md_ecc_injection,
                             SGIBedrockMDECCInjection),
        VMSTATE_UINT64_V(directory_error, SGIBedrockMDState, 8),
        VMSTATE_STRUCT_ARRAY(directory_injection, SGIBedrockMDState,
                             SGI_BEDROCK_MD_DIRECTORY_INJECTIONS, 8,
                             vmstate_md_directory_injection,
                             SGIBedrockMDDirectoryInjection),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_md_watched,
        &vmstate_md_line_states,
        NULL
    },
};

static void md_init(Object *obj)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(obj);

    s->selftest_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     md_selftest_complete, s);
    s->ecc_injection_by_raw = g_hash_table_new(g_int64_hash, g_int64_equal);
    s->line_states = g_hash_table_new_full(g_int_hash, g_int_equal, NULL,
                                           g_free);
    s->directory_injection_by_index = g_hash_table_new(g_int_hash,
                                                       g_int_equal);
    s->ecc_retired_by_raw = g_hash_table_new(g_int64_hash, g_int64_equal);
    s->directory_retired_by_index = g_hash_table_new(g_int_hash, g_int_equal);
    s->retired_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, md_retired_expire, s);
    memory_region_init_io(&s->mmio, obj, &md_ops, s, TYPE_SGI_BEDROCK_MD,
                          SGI_BEDROCK_MD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    memory_region_init_io(&s->directory_mmio, obj, &md_directory_ops, s,
                          TYPE_SGI_BEDROCK_MD "-directory",
                          SGI_BEDROCK_MD_DIRECTORY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->directory_mmio);
    memory_region_init_io(&s->ecc_mmio, obj, &md_ecc_ops, s,
                          TYPE_SGI_BEDROCK_MD "-ecc",
                          SGI_BEDROCK_MD_ECC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ecc_mmio);
    memory_region_init_io(&s->ecc_scrub_mmio, obj, &md_ecc_scrub_ops, s,
                          TYPE_SGI_BEDROCK_MD "-ecc-scrub",
                          SGI_BEDROCK_MD_ECC_SIZE);
    memory_region_init(&s->memory_mr, obj, TYPE_SGI_BEDROCK_MD "-memory",
                       SGI_BEDROCK_MD_MEMORY_SIZE);
    memory_region_init_io(&s->memory_mmio, obj, &md_memory_ops, s,
                          TYPE_SGI_BEDROCK_MD "-memory-decoder",
                          SGI_BEDROCK_MD_MEMORY_SIZE);
    memory_region_add_subregion(&s->memory_mr, 0, &s->memory_mmio);
    /* Checked doublewords sit above the bank aliases inside the window. */
    memory_region_init(&s->ecc_overlay, obj, TYPE_SGI_BEDROCK_MD "-ecc-overlay",
                       SGI_BEDROCK_MD_MEMORY_SIZE);
    memory_region_add_subregion_overlap(&s->memory_mr, 0, &s->ecc_overlay, 2);
    for (unsigned i = 0; i < SGI_BEDROCK_MD_ECC_INJECTIONS; i++) {
        g_autofree char *name =
            g_strdup_printf(TYPE_SGI_BEDROCK_MD "-ecc-injection%u", i);

        s->ecc_injection[i].md = s;
        memory_region_init_io(&s->ecc_injection[i].mr, obj,
                              &md_ecc_overlay_ops, &s->ecc_injection[i],
                              name, 8);
    }
    for (unsigned i = 0; i < SGI_BEDROCK_MD_DIRECTORY_INJECTIONS; i++) {
        g_autofree char *name =
            g_strdup_printf(TYPE_SGI_BEDROCK_MD "-directory-injection%u", i);

        s->directory_injection[i].md = s;
        memory_region_init_io(&s->directory_injection[i].mr, obj,
                              &md_directory_overlay_ops,
                              &s->directory_injection[i], name,
                              MD_DIRECTORY_LINE);
    }
    for (unsigned i = 0; i < SGI_BEDROCK_MD_WATCHED_PAGES; i++) {
        g_autofree char *name =
            g_strdup_printf(TYPE_SGI_BEDROCK_MD "-watched-page%u", i);

        s->watched[i].md = s;
        memory_region_init_io(&s->watched[i].mr, obj, &md_watched_ops,
                              &s->watched[i], name, MD_WATCH_PAGE);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->memory_mr);
    memory_region_init_io(&s->fetchop_mmio, obj, &md_fetchop_ops, s,
                          TYPE_SGI_BEDROCK_MD "-fetch-and-op",
                          SGI_BEDROCK_MD_FETCHOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->fetchop_mmio);
    /* Region 5: the ECC scrub carrier, four eighths of the node. */
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ecc_scrub_mmio);
    qdev_init_gpio_in_named(DEVICE(obj), md_soft_reset_input, "soft-reset", 1);
}

static void md_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(dev);
    MemoryRegion *ram;
    uint64_t node_size, offset = 0;

    s->protection_entries = SGI_BEDROCK_MD_PROTECTION_ENTRIES;
    s->protection = g_new0(uint64_t, s->protection_entries);
    if (!s->node_mr) {
        return;
    }
    /*
     * A node of a several-node machine links its own share of the machine's
     * RAM, an alias of it; one node links the RAM
     * itself. Find the RAM under any aliases and the share's offset into it.
    */
    ram = s->node_mr;
    while (ram->alias) {
        offset += ram->alias_offset;
        ram = ram->alias;
    }
    if (!memory_region_is_ram(ram)) {
        error_setg(errp, TYPE_SGI_BEDROCK_MD
                   " 'node-memory' must link a RAM region or an alias of one");
        return;
    }
    node_size = memory_region_size(s->node_mr);
    if (node_size % 128 != 0) {
        error_setg(errp, TYPE_SGI_BEDROCK_MD
                   " node memory size must be a multiple of 128 bytes");
        return;
    }
    if (node_size > (uint64_t)SGI_BEDROCK_MD_DIRECTORY_SIZE * 8) {
        error_setg(errp, TYPE_SGI_BEDROCK_MD
                   " node memory exceeds the directory aperture");
        return;
    }
    /*
     * Root the address space at an unplaced alias of that RAM, not at
     * node_mr: rendering an address space adds its root
     * region's own address (render_memory_region() in system/memory.c), and a
     * node's share sits at the node's
     * placement, 2^33 for a second node. An unplaced alias of node_mr itself
     * would not do either, since the memory core
     * looks through an alias that covers its target whole from offset zero
     * when it picks the root it renders.
    */
    memory_region_init_alias(&s->node_root, OBJECT(s),
                             TYPE_SGI_BEDROCK_MD "-node-root", ram,
                             offset, node_size);
    address_space_init(&s->node_as, &s->node_root,
                       TYPE_SGI_BEDROCK_MD "-node-memory");
    s->node_as_initialized = true;
    for (unsigned bank = 0; bank < 8; bank++) {
        g_autofree char *name =
            g_strdup_printf(TYPE_SGI_BEDROCK_MD "-memory-bank%u", bank);

        memory_region_init_alias(&s->memory_alias[bank], OBJECT(s), name,
                                 s->node_mr, 0, 1);
        memory_region_set_enabled(&s->memory_alias[bank], false);
        memory_region_set_unmergeable(&s->memory_alias[bank], true);
        memory_region_add_subregion_overlap(&s->memory_mr,
                                            bank * MD_MEMORY_BANK_SIZE,
                                            &s->memory_alias[bank], 1);
    }
    s->memory_alias_initialized = true;
    md_update_memory_aliases(s);
    s->directory_entries = node_size / 128;
    s->ecc_entries = node_size / 8;
    s->directory = g_new0(uint64_t, s->directory_entries);
    s->page_counters = g_new0(uint64_t, s->directory_entries);
    s->ecc = g_new0(uint8_t, s->ecc_entries);
    s->named_lines = bitmap_new(s->directory_entries);
    s->named_line_bits = s->directory_entries;
}

static void md_unrealize(DeviceState *dev)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(dev);

    timer_del(s->selftest_timer);
    timer_del(s->retired_timer);
    md_ecc_injections_clear(s);
    md_directory_injections_clear(s);
    md_watched_pages_clear(s);
    if (s->memory_alias_initialized) {
        for (unsigned bank = 0; bank < 8; bank++) {
            memory_region_del_subregion(&s->memory_mr,
                                        &s->memory_alias[bank]);
        }
        s->memory_alias_initialized = false;
    }
    if (s->node_as_initialized) {
        address_space_destroy(&s->node_as);
        s->node_as_initialized = false;
    }
    g_clear_pointer(&s->directory, g_free);
    g_clear_pointer(&s->page_counters, g_free);
    g_clear_pointer(&s->protection, g_free);
    g_clear_pointer(&s->ecc, g_free);
    g_clear_pointer(&s->named_lines, g_free);
    s->directory_entries = 0;
    s->protection_entries = 0;
    s->ecc_entries = 0;
}

static const Property md_properties[] = {
    DEFINE_PROP_UINT8("installed-bank-mask", SGIBedrockMDState,
                      installed_memory_bank_mask, UINT8_MAX),
    DEFINE_PROP_UINT16("installed-bank0-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[0], 0),
    DEFINE_PROP_UINT16("installed-bank1-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[1], 0),
    DEFINE_PROP_UINT16("installed-bank2-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[2], 0),
    DEFINE_PROP_UINT16("installed-bank3-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[3], 0),
    DEFINE_PROP_UINT16("installed-bank4-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[4], 0),
    DEFINE_PROP_UINT16("installed-bank5-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[5], 0),
    DEFINE_PROP_UINT16("installed-bank6-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[6], 0),
    DEFINE_PROP_UINT16("installed-bank7-mib", SGIBedrockMDState,
                       installed_memory_bank_mib[7], 0),
    DEFINE_PROP_LINK("node-memory", SGIBedrockMDState, node_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void md_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock memory and directory interface";
    dc->vmsd = &vmstate_md;
    dc->user_creatable = true;
    dc->realize = md_realize;
    dc->unrealize = md_unrealize;
    rc->phases.enter = md_reset_enter;
    device_class_set_props(dc, md_properties);
}

static void md_finalize(Object *obj)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(obj);

    timer_free(s->selftest_timer);
    timer_free(s->retired_timer);
    g_hash_table_destroy(s->ecc_injection_by_raw);
    g_hash_table_destroy(s->directory_injection_by_index);
    g_hash_table_destroy(s->line_states);
    g_free(s->line_state_array);
    g_hash_table_destroy(s->ecc_retired_by_raw);
    g_hash_table_destroy(s->directory_retired_by_index);
    if (s->node_as_initialized) {
        address_space_destroy(&s->node_as);
    }
    g_clear_pointer(&s->directory, g_free);
    g_clear_pointer(&s->page_counters, g_free);
    g_clear_pointer(&s->protection, g_free);
    g_clear_pointer(&s->ecc, g_free);
    g_clear_pointer(&s->named_lines, g_free);
}

static const TypeInfo md_info = {
    .name = TYPE_SGI_BEDROCK_MD, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockMDState), .instance_init = md_init,
    .instance_finalize = md_finalize, .class_init = md_class_init,
};

static void md_register_types(void)
{
    type_register_static(&md_info);
}
type_init(md_register_types)
