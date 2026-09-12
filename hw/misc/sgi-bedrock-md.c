/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-md.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

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
 * Bits 58:56 are write-only in the public definition; reading them back
 * and resetting them to zero is model policy.
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

static void md_selftest_command(SGIBedrockMDState *s, uint64_t value);
static void md_selftest_complete(void *opaque);

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
    case 0x40010: case 0x40018: case 0x40020: case 0x40028:
    case 0x40030: case 0x40038: case 0x40058:
    case 0x60010: case 0x60018: case 0x60020: case 0x60028:
    case 0x60030: case 0x60038:
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

    switch (offset) {
    case 0: return s->current_cell;
    case 0x8: return s->memory_config;
    case 0x60: return MD_REPLY_QUEUE;
    case 0x20000: return s->refresh;
    case 0x20048: return s->selftest_status;
    case 0x40000: return s->directory_config;
    case 0x40050: return MD_REQUEST_QUEUE;
    }
    if (md_storage(offset, &index, &mask)) {
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
    }
    if (md_storage(offset, &index, &mask)) {
        s->storage[index] = value & mask;
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

static bool md_is_protection(hwaddr offset)
{
    return offset < MD_PROTECTION_EXTENT &&
           (offset & MD_PROTECTION_SELECTOR_MASK) == MD_PROTECTION_SELECTOR;
}

static bool md_protection_translate(const SGIBedrockMDState *s,
                                    hwaddr offset, unsigned *index)
{
    unsigned software_bank = offset / MD_DIRECTORY_BANK_SIZE;
    uint64_t node_size;
    uint64_t bank_base = software_bank * MD_MEMORY_BANK_SIZE;
    uint64_t bank_size;
    uint64_t within;

    if (!s->node_mr) {
        return false;
    }
    node_size = memory_region_size(s->node_mr);
    if (bank_base >= node_size) {
        return false;
    }
    bank_size = MIN(node_size - bank_base, MD_MEMORY_BANK_SIZE);
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
        return false;
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

static uint64_t md_directory_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (md_is_protection(offset)) {
        if (!md_protection_translate(s, offset, &index)) {
            return 0;
        }
        return s->protection[index];
    }
    if (!md_directory_translate(s, offset, &index, NULL)) {
        index = md_directory_compat_get(s, offset);
        return s->directory_compat_values[index] &
               ((offset & 8) ? UINT64_MAX : md_directory_mask(s));
    }
    if (offset & 8) {
        return s->page_counters[index];
    }
    return s->directory[index] & md_directory_mask(s);
}

static void md_directory_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;

    if (md_is_protection(offset)) {
        if (md_protection_translate(s, offset, &index)) {
            s->protection[index] = value;
        }
        return;
    }
    if (!md_directory_translate(s, offset, &index, NULL)) {
        index = md_directory_compat_get(s, offset);
        s->directory_compat_values[index] =
            value & ((offset & 8) ? UINT64_MAX : md_directory_mask(s));
        return;
    }
    if (offset & 8) {
        s->page_counters[index] = value;
    } else {
        s->directory[index] = value & md_directory_mask(s);
    }
}

static const MemoryRegionOps md_directory_ops = {
    .read = md_directory_read,
    .write = md_directory_write,
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
 * Fitted ECC bytes follow the same software-to-locational bank mapping as
 * memory. The carrier packs byte addresses in reverse order within each
 * doubleword, so undo that transform before applying the fitted bound.
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
        return s->ecc[index];
    }
    return lduw_be_p(&s->ecc[index]);
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
        stw_be_p(&s->ecc[index], value);
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

static bool md_selftest_bank_fits(const SGIBedrockMDState *s,
                                  unsigned locational_bank)
{
    uint64_t dense_base = 0;
    uint64_t raw_size;
    unsigned size_code;

    if (!s->node_mr || !md_memory_bank_enabled(s, locational_bank)) {
        return false;
    }

    for (unsigned bank = 0; bank < locational_bank; bank++) {
        uint64_t bank_size;

        if (!md_memory_bank_enabled(s, bank)) {
            continue;
        }
        size_code = md_memory_size_code(s, bank);
        if (size_code < 1 || size_code > 5) {
            return false;
        }
        bank_size = md_memory_bank_size(size_code);
        if (dense_base > UINT64_MAX - bank_size) {
            return false;
        }
        dense_base += bank_size;
    }

    size_code = md_memory_size_code(s, locational_bank);
    if (size_code < 1 || size_code > 5) {
        return false;
    }
    raw_size = memory_region_size(s->node_mr);
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
    s->selftest_status = 0;
}

static void md_selftest_command(SGIBedrockMDState *s, uint64_t value)
{
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

    s->selftest_running = true;
    s->selftest_selector = extract64(value, 4, 3);
    s->selftest_phase = s->storage[MD_SELFTEST_DATA] & 0x80;
    s->selftest_result = md_selftest_bank_fits(
        s, md_memory_remap_bank(s, s->selftest_selector));
    s->selftest_status = 0;
    timer_mod_ns(s->selftest_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 MD_SELFTEST_INTERVAL_NS);
}

static void md_selftest_complete(void *opaque)
{
    SGIBedrockMDState *s = opaque;

    if (!s->selftest_running) {
        return;
    }
    s->selftest_running = false;
    s->selftest_status = MD_SELFTEST_DONE |
                         (s->selftest_result ? MD_SELFTEST_PASSED : 0);
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

    /* The linked RAM densely packs enabled locational banks in bank order. */
    for (unsigned bank = 0; bank < locational_bank; bank++) {
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

static MemTxResult md_memory_read(void *opaque, hwaddr offset, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIBedrockMDState *s = opaque;
    hwaddr raw_offset;
    MemTxResult result;

    result = md_memory_translate(s, offset, size, false, &raw_offset);
    if (result != MEMTX_OK) {
        *value = 0;
        return result;
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
    hwaddr raw_offset;
    MemTxResult result;

    result = md_memory_translate(s, offset, size, true, &raw_offset);
    if (result != MEMTX_OK) {
        return result;
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

static void md_reset_registers(SGIBedrockMDState *s)
{
    md_selftest_cancel(s);
    s->current_cell = UINT64_MAX;
    s->memory_config = 0;
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

    if (s->directory_compat_count >
        SGI_BEDROCK_MD_DIRECTORY_COMPAT_ENTRIES) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_md = {
    .name = TYPE_SGI_BEDROCK_MD, .version_id = 6, .minimum_version_id = 1,
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
        VMSTATE_UINT8_V(selftest_status, SGIBedrockMDState, 5),
        VMSTATE_TIMER_PTR_V(selftest_timer, SGIBedrockMDState, 5),
        VMSTATE_END_OF_LIST()
    },
};

static void md_init(Object *obj)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(obj);

    s->selftest_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     md_selftest_complete, s);
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
    memory_region_init_io(&s->memory_mmio, obj, &md_memory_ops, s,
                          TYPE_SGI_BEDROCK_MD "-memory",
                          SGI_BEDROCK_MD_MEMORY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->memory_mmio);
    qdev_init_gpio_in_named(DEVICE(obj), md_soft_reset_input, "soft-reset", 1);
}

static void md_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(dev);
    uint64_t node_size;

    s->protection_entries = SGI_BEDROCK_MD_PROTECTION_ENTRIES;
    s->protection = g_new0(uint64_t, s->protection_entries);
    if (!s->node_mr) {
        return;
    }
    if (!memory_region_is_ram(s->node_mr)) {
        error_setg(errp, TYPE_SGI_BEDROCK_MD
                   " 'node-memory' must link a RAM region");
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
    address_space_init(&s->node_as, s->node_mr,
                       TYPE_SGI_BEDROCK_MD "-node-memory");
    s->node_as_initialized = true;
    s->directory_entries = node_size / 128;
    s->ecc_entries = node_size / 8;
    s->directory = g_new0(uint64_t, s->directory_entries);
    s->page_counters = g_new0(uint64_t, s->directory_entries);
    s->ecc = g_new0(uint8_t, s->ecc_entries);
}

static void md_unrealize(DeviceState *dev)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(dev);

    timer_del(s->selftest_timer);
    if (s->node_as_initialized) {
        address_space_destroy(&s->node_as);
        s->node_as_initialized = false;
    }
    g_clear_pointer(&s->directory, g_free);
    g_clear_pointer(&s->page_counters, g_free);
    g_clear_pointer(&s->protection, g_free);
    g_clear_pointer(&s->ecc, g_free);
    s->directory_entries = 0;
    s->protection_entries = 0;
    s->ecc_entries = 0;
}

static const Property md_properties[] = {
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
    if (s->node_as_initialized) {
        address_space_destroy(&s->node_as);
    }
    g_clear_pointer(&s->directory, g_free);
    g_clear_pointer(&s->page_counters, g_free);
    g_clear_pointer(&s->protection, g_free);
    g_clear_pointer(&s->ecc, g_free);
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
