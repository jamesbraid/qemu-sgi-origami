/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI IP30 HEART standalone processor and memory controller profile.
 * Memory population is explicit; this does not identify an Octane DIMM layout.
 * The Crossbow widget accepts interrupt packets and mapped-RAM DMA.
 * Compare and trigger are not modeled.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "trace.h"

#define HEART_REG_SIZE 0x60008
#define HEART_RAM_BASE 0x20000000ULL
#define HEART_UNIT (32 * MiB)
#define HEART_COUNT_MASK ((1ULL << 52) - 1)
#define HEART_LEVEL0_MASK 0x000000000000ffffULL
#define HEART_LEVEL1_MASK 0x00000000ffff0000ULL
#define HEART_LEVEL2_MASK 0x0003ffff00000000ULL
#define HEART_LEVEL3_MASK 0x0004000000000000ULL
#define HEART_LEVEL4_MASK 0xfff8000000000000ULL
#define HEART_WIDGET_SIZE 0x1000
#define HEART_PACKET_SIZE (1ULL << 48)
#define HEART_RAM_PACKET_SIZE ((HEART_MAX_UNITS + 0x1ffULL) * HEART_UNIT)

#define HEART_ECC_REPORT (1ULL << 0)
#define HEART_ECC_DOUBLE_REPORT (1ULL << 1)
#define HEART_INTERRUPT_ENABLE (1ULL << 13)
#define HEART_ECC_ENABLE (1ULL << 15)
#define HEART_ECC_GENERATE (1ULL << 19)
#define HEART_ECC_DOUBLE (1ULL << 20)
#define HEART_ECC_FORCE_WRITE (1ULL << 21)
#define HEART_ECC_LOCK (1ULL << 25)
#define HEART_ECC_GENERATION (HEART_ECC_GENERATE | HEART_ECC_DOUBLE)
/* Bit zero tags double-bit poison; backing-word keys are eight-byte aligned. */
#define HEART_ECC_KEY(word) ((word) & ~7ULL)
#define HEART_ECC_PENDING (1ULL << 63)

static void heart_ecc_map(SGIHeartState *s);
static bool heart_ecc_dma_allowed(SGIHeartState *s, hwaddr address,
                                  unsigned size, bool write);

#define HEART_ONEWIRE_DONE 2
#define HEART_ONEWIRE_LEVEL 1

static void heart_onewire_complete(void *opaque)
{
    SGIHeartState *s = opaque;

    s->onewire_busy = false;
    s->onewire_control |= HEART_ONEWIRE_DONE;
    if (s->onewire_level) {
        s->onewire_control |= HEART_ONEWIRE_LEVEL;
    }
}

static void heart_onewire_refresh(SGIHeartState *s)
{
    if (s->onewire_busy &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->onewire_deadline_ns) {
        timer_del(s->onewire_timer);
        heart_onewire_complete(s);
    }
}

static void heart_onewire_start(SGIHeartState *s, uint64_t command)
{
    uint32_t pulse = (command >> 10) & 0x3ff;
    uint32_t sample = (command >> 2) & 0xff;

    if (s->onewire_busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-heart: one-wire command replaced busy slot\n");
    }
    timer_del(s->onewire_timer);
    s->onewire_control = command & ~(HEART_ONEWIRE_DONE | HEART_ONEWIRE_LEVEL);
    s->onewire_level = sgi_onewire_slot(s->onewire_bus, pulse, sample);
    s->onewire_busy = true;
    /* Register units are modeled as microseconds; silicon timing is unknown. */
    s->onewire_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             (int64_t)(pulse + sample) * 1000;
    timer_mod(s->onewire_timer, s->onewire_deadline_ns);
}

static void heart_update_irq(SGIHeartState *s)
{
    uint64_t enabled = s->pending & s->mask[0];

    trace_sgi_heart_irq(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->pending,
                        s->mask[0], enabled,
                        !!(enabled & HEART_LEVEL0_MASK),
                        !!(enabled & HEART_LEVEL1_MASK),
                        !!(enabled & HEART_LEVEL2_MASK));
    qemu_set_irq(s->processor[0], !!(enabled & HEART_LEVEL0_MASK));
    qemu_set_irq(s->processor[1], !!(enabled & HEART_LEVEL1_MASK));
    qemu_set_irq(s->processor[2], !!(enabled & HEART_LEVEL2_MASK));
    qemu_set_irq(s->processor[3], !!(enabled & HEART_LEVEL3_MASK));
    if (!(s->mode & HEART_INTERRUPT_ENABLE)) {
        enabled &= ~HEART_ECC_PENDING;
    }
    qemu_set_irq(s->processor[4], !!(enabled & HEART_LEVEL4_MASK));
}

static MemTxResult heart_widget_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;

    if ((offset == 0x00 && size == 8) ||
        (offset == 0x04 && size == 4)) {
        *value = s->widget_id_64 ? s->widget_id_64 :
                 ((uint32_t)s->widget_revision << 28) |
                 (0xc001U << 12) | (0x0036U << 1);
        return MEMTX_OK;
    }
    if (offset == 0x08 && size == 8 && s->widget_revision == 6 &&
        s->widget_status_f_measured) {
        *value = 0x60;
        return MEMTX_OK;
    }
    if (offset == 0x24 && size == 4) {
        *value = s->widget_control;
        return MEMTX_OK;
    }
    if ((offset == 0x28 && size == 8) ||
        (offset == 0x2c && size == 4)) {
        *value = s->widget_request_timeout;
        return MEMTX_OK;
    }
    if (size == 4 && (offset == 0x14 || offset == 0x1c ||
                      offset == 0x44)) {
        *value = 0;
        return MEMTX_OK;
    }
    if (size == 8 && (offset == 0x60 || offset == 0x70 ||
                      offset == 0x78 || offset == 0x90)) {
        *value = 0;
        return MEMTX_OK;
    }
    if (offset == 0x68 && size == 8) {
        *value = s->widget_error_mask;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported widget %u-byte read "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult heart_widget_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;

    if (offset == 0x24 && size == 4) {
        s->widget_control = value;
        return MEMTX_OK;
    }
    if ((offset == 0x28 && size == 8) ||
        (offset == 0x2c && size == 4)) {
        s->widget_request_timeout = value;
        return MEMTX_OK;
    }
    if (offset == 0x44 && size == 4 && value == 0) {
        return MEMTX_OK;
    }
    if (offset == 0x60 && size == 8) {
        /* No widget error producer exists in this model. */
        return MEMTX_OK;
    }
    if (offset == 0x68 && size == 8) {
        s->widget_error_mask = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported widget %u-byte write "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps heart_widget_ops = {
    .read_with_attrs = heart_widget_read,
    .write_with_attrs = heart_widget_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

/*
 * Validate one QEMU memory operation before writing any byte. The packet
 * callback sees at most eight bytes; a larger PCI DMA may issue several
 * operations and can fail after an earlier one has completed.
 */
static bool heart_dma_mapped(SGIHeartState *s, hwaddr address, unsigned size)
{
    for (unsigned byte = 0; byte < size; byte++) {
        hwaddr at = address + byte;
        bool found = false;

        if (at < address) {
            return false;
        }
        for (int bank = HEART_BANKS - 1; bank >= 0; bank--) {
            uint32_t d = s->descriptor[bank];
            hwaddr base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
            hwaddr span = (uint64_t)((d >> 16 & 0x3f) + 1) * HEART_UNIT;

            if ((d & 0x80000000U) && at >= base && at - base < span) {
                found = s->capacity[bank] != 0;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static MemTxResult heart_packet_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;
    uint8_t bytes[8];

    if (offset >= HEART_RAM_BASE && heart_dma_mapped(s, offset, size)) {
        if (!heart_ecc_dma_allowed(s, offset, size, false)) {
            *value = 0;
            return MEMTX_ERROR;
        }
        MemTxResult result = address_space_read(&address_space_memory, offset,
                                                attrs, bytes, size);

        if (result == MEMTX_OK) {
            *value = ldn_be_p(bytes, size);
        }
        return result;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported packet %u-byte read "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult heart_packet_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;
    unsigned vector = value & 0xff;
    uint8_t bytes[8];
    uint64_t old_pending;

    if (offset >= HEART_RAM_BASE && heart_dma_mapped(s, offset, size)) {
        if (!heart_ecc_dma_allowed(s, offset, size, true)) {
            return MEMTX_ERROR;
        }
        stn_be_p(bytes, size, value);
        return address_space_write(&address_space_memory, offset, attrs,
                                   bytes, size);
    }
    if (offset != 0x80 || size != 8 || (value & ~0x1ffULL) || vector >= 64) {
        qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported packet %u-byte "
                      "write at 0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                      size, offset, value);
        return MEMTX_ERROR;
    }
    old_pending = s->pending;
    if (value & 0x100) {
        s->pending |= 1ULL << vector;
    } else {
        s->pending &= ~(1ULL << vector);
    }
    trace_sgi_heart_packet(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), value,
                           old_pending, s->pending, s->mask[0]);
    heart_update_irq(s);
    return MEMTX_OK;
}

static const MemoryRegionOps heart_packet_ops = {
    .read_with_attrs = heart_packet_read,
    .write_with_attrs = heart_packet_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult heart_packet_ram_read(void *opaque, hwaddr offset,
                                         uint64_t *value, unsigned size,
                                         MemTxAttrs attrs)
{
    return heart_packet_read(opaque, offset + HEART_RAM_BASE,
                             value, size, attrs);
}

static MemTxResult heart_packet_ram_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    return heart_packet_write(opaque, offset + HEART_RAM_BASE,
                              value, size, attrs);
}

static const MemoryRegionOps heart_packet_ram_ops = {
    .read_with_attrs = heart_packet_ram_read,
    .write_with_attrs = heart_packet_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static uint64_t heart_counter(SGIHeartState *s)
{
    uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                       s->counter_epoch_ns;

    return (s->saved_count + elapsed / 80) & HEART_COUNT_MASK;
}

static uint64_t heart_absent_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void heart_absent_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
}

static const MemoryRegionOps heart_absent_ops = {
    .read = heart_absent_read,
    .write = heart_absent_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t heart_bank_offset(uint32_t descriptor, uint64_t capacity,
                                  uint64_t offset)
{
    /* Functional probe projection; physical HEART wiring is not established. */
    if ((descriptor & 0x803f0000U) == 0x803f0000U &&
        ((descriptor >> 22) & 7) == 6) {
        switch (capacity) {
        case 64 * MiB:
            return (offset & (32 * MiB - 1)) |
                   ((offset & (64 * MiB)) >> 1);
        case 128 * MiB:
            return (offset & (32 * MiB - 1)) |
                   ((offset & (64 * MiB)) >> 1) |
                   ((offset & (1 * GiB)) >> 4);
        case 256 * MiB:
            return (offset & (128 * MiB - 1)) |
                   ((offset & (256 * MiB)) >> 1);
        }
    }
    return offset % capacity;
}

/* A key identifies a word in RAM backing, independent of its CPU aliases. */
static int heart_ram_byte(SGIHeartState *s, hwaddr address,
                           uint64_t *key, uint8_t **byte)
{
    for (int bank = HEART_BANKS - 1; bank >= 0; bank--) {
        uint32_t d = s->descriptor[bank];
        hwaddr base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
        hwaddr span = (uint64_t)(((d >> 16) & 0x3f) + 1) * HEART_UNIT;

        if ((d & 0x80000000U) && address >= base && address - base < span) {
            uint64_t offset;

            if (!s->capacity[bank]) {
                return 0;
            }
            offset = heart_bank_offset(d, s->capacity[bank], address - base);
            *key = ((uint64_t)bank << 32) | (offset & ~7ULL);
            *byte = (uint8_t *)memory_region_get_ram_ptr(&s->backing[bank]) +
                    offset;
            return 1;
        }
    }
    return -1;
}

static unsigned heart_ecc_find(SGIHeartState *s, uint64_t key)
{
    unsigned low = 0, high = s->ecc_word_count;

    while (low < high) {
        unsigned middle = low + (high - low) / 2;

        if (HEART_ECC_KEY(s->ecc_words[middle]) < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

static unsigned heart_ecc_poisoned(SGIHeartState *s, uint64_t key)
{
    unsigned index = heart_ecc_find(s, key);

    if (index < s->ecc_word_count &&
        HEART_ECC_KEY(s->ecc_words[index]) == key) {
        return (s->ecc_words[index] & 1) ? 2 : 1;
    }
    return 0;
}

static void heart_ecc_set(SGIHeartState *s, uint64_t key, unsigned fault)
{
    unsigned index = heart_ecc_find(s, key);
    bool found = index < s->ecc_word_count &&
                 HEART_ECC_KEY(s->ecc_words[index]) == key;
    uint64_t word = key | (fault == 2);

    if (found && fault) {
        s->ecc_words[index] = word;
        return;
    }
    if (!found && !fault) {
        return;
    }
    if (fault) {
        if (s->ecc_word_count == s->ecc_word_capacity) {
            s->ecc_word_capacity = MAX(32, s->ecc_word_capacity * 2);
            s->ecc_words = g_renew(uint64_t, s->ecc_words,
                                   s->ecc_word_capacity);
        }
        memmove(&s->ecc_words[index + 1], &s->ecc_words[index],
                (s->ecc_word_count - index) * sizeof(uint64_t));
        s->ecc_words[index] = word;
        s->ecc_word_count++;
    } else {
        s->ecc_word_count--;
        memmove(&s->ecc_words[index], &s->ecc_words[index + 1],
                (s->ecc_word_count - index) * sizeof(uint64_t));
    }
    heart_ecc_map(s);
}

static MemTxResult heart_ecc_reject(hwaddr address, unsigned size,
                                   const char *operation)
{
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported ECC %u-byte %s "
                  "at 0x%" HWADDR_PRIx "\n", size, operation, address);
    return MEMTX_ERROR;
}

static bool heart_ecc_dma_allowed(SGIHeartState *s, hwaddr address,
                                  unsigned size, bool write)
{
    if (write && (s->mode & HEART_ECC_GENERATION)) {
        heart_ecc_reject(address, size, "DMA injection");
        return false;
    }
    for (unsigned i = 0; i < size; i++) {
        uint64_t key;
        uint8_t *byte;

        if (heart_ram_byte(s, address + i, &key, &byte) > 0 &&
            heart_ecc_poisoned(s, key)) {
            heart_ecc_reject(address, size, "poisoned DMA");
            return false;
        }
    }
    return true;
}

typedef struct HeartECCRegion {
    MemoryRegion memory;
    bool mapped;
} HeartECCRegion;

typedef struct HeartECCRange {
    hwaddr base;
    hwaddr end;
} HeartECCRange;

static MemTxResult heart_ecc_access(SGIHeartState *s, hwaddr offset,
                                   uint64_t *value, unsigned size, bool write)
{
    hwaddr address = HEART_RAM_BASE + offset;
    uint8_t *bytes[8], data[8];
    uint64_t keys[8] = { 0 };
    int mapped[8];
    unsigned poisoned = 0;
    uint64_t generation = s->mode & HEART_ECC_GENERATION;
    bool inject = write && generation;
    bool force = write && (s->mode & HEART_ECC_FORCE_WRITE);
    uint64_t required = HEART_ECC_ENABLE |
                        (generation == HEART_ECC_DOUBLE ?
                         HEART_ECC_DOUBLE_REPORT : HEART_ECC_REPORT);

    for (unsigned i = 0; i < size; i++) {
        mapped[i] = heart_ram_byte(s, address + i, &keys[i], &bytes[i]);
        if (mapped[i] > 0) {
            poisoned = MAX(poisoned, heart_ecc_poisoned(s, keys[i]));
        }
    }
    if (inject || poisoned || force) {
        if (size != 8 || (address & 7) || keys[0] != keys[7] ||
            mapped[0] <= 0 || mapped[7] <= 0 ||
            (s->mode & HEART_ECC_LOCK) ||
            (inject && (force || generation == HEART_ECC_GENERATION ||
                        (s->mode & required) != required)) ||
            (!write && ((s->mode &
                         (HEART_ECC_DOUBLE | HEART_ECC_FORCE_WRITE)) ||
                        (poisoned == 2 && generation)))) {
            return heart_ecc_reject(address, size, write ? "write" : "read");
        }
    }
    for (unsigned i = 0; i < size; i++) {
        if (mapped[i] < 0) {
            return MEMTX_DECODE_ERROR;
        }
    }
    if (write) {
        stn_be_p(data, size, *value);
        for (unsigned i = 0; i < size; i++) {
            if (!mapped[i]) {
                continue;
            }
            *bytes[i] = data[i];
            memory_region_set_dirty(&s->backing[keys[i] >> 32],
                (uint32_t)keys[i] + ((address + i) & 7), 1);
        }
        if (inject || poisoned) {
            unsigned fault = inject ?
                             (generation == HEART_ECC_DOUBLE ? 2 : 1) : 0;

            heart_ecc_set(s, keys[0], fault);
        }
    } else {
        for (unsigned i = 0; i < size; i++) {
            data[i] = mapped[i] ? *bytes[i] : 0;
        }
        *value = ldn_be_p(data, size);
        if (poisoned) {
            if (!(s->mode & HEART_ECC_ENABLE)) {
                *value ^= poisoned == 2 ? 3 : 1;
            } else if (s->mode & (poisoned == 2 ? HEART_ECC_DOUBLE_REPORT :
                                  HEART_ECC_REPORT)) {
                unsigned requester = current_cpu ? current_cpu->cpu_index : 0;

                s->ecc_address = (address & ((1ULL << 34) - 1)) |
                    ((poisoned == 2 ? 0x30ULL : 0x13ULL) << 34) |
                    ((uint64_t)(requester & 3) << 43);
                s->ecc_data = *value ^ (poisoned == 2 ? 3 : 1);
                s->ecc_cause |= poisoned == 2 ? 2 : 1;
                s->pending |= HEART_ECC_PENDING;
                heart_update_irq(s);
            }
            if (poisoned == 2 && (s->mode & HEART_ECC_ENABLE)) {
                return MEMTX_ERROR;
            }
        }
    }
    return MEMTX_OK;
}

static MemTxResult heart_ecc_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    return heart_ecc_access(opaque, offset, value, size, false);
}

static MemTxResult heart_ecc_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    return heart_ecc_access(opaque, offset, &value, size, true);
}

static const MemoryRegionOps heart_ecc_ops = {
    .read_with_attrs = heart_ecc_read,
    .write_with_attrs = heart_ecc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static int heart_ecc_range_compare(const void *a, const void *b)
{
    const HeartECCRange *left = a, *right = b;

    return left->base < right->base ? -1 : left->base > right->base;
}

static void heart_ecc_map(SGIHeartState *s)
{
    MemoryRegion *memory = s->cpu_memory ?: get_system_memory();
    g_autoptr(GArray) ranges = g_array_new(false, false, sizeof(HeartECCRange));

    memory_region_transaction_begin();
    for (unsigned i = 0; i < s->ecc_regions->len; i++) {
        HeartECCRegion *region = g_ptr_array_index(s->ecc_regions, i);

        if (region->mapped) {
            memory_region_del_subregion(memory, &region->memory);
            region->mapped = false;
        }
    }
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        uint32_t d = s->descriptor[bank];
        hwaddr base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
        unsigned units = ((d >> 16) & 0x3f) + 1;

        if (!(d & 0x80000000U) || !s->capacity[bank]) {
            continue;
        }
        if (s->mode & (HEART_ECC_GENERATION | HEART_ECC_FORCE_WRITE)) {
            HeartECCRange range = { base, base + units * HEART_UNIT };

            g_array_append_val(ranges, range);
            continue;
        }
        for (unsigned unit = 0; unit < units; unit++) {
            uint64_t backing = heart_bank_offset(d, s->capacity[bank],
                                                unit * HEART_UNIT);
            unsigned index = heart_ecc_find(s,
                ((uint64_t)bank << 32) | backing);

            while (index < s->ecc_word_count &&
                   (s->ecc_words[index] >> 32) == bank &&
                   (uint32_t)HEART_ECC_KEY(s->ecc_words[index]) -
                   backing < HEART_UNIT) {
                hwaddr address = base + unit * HEART_UNIT +
                                  (uint32_t)HEART_ECC_KEY(s->ecc_words[index]) -
                                  backing;
                uint64_t key;
                uint8_t *byte;

                if (heart_ram_byte(s, address, &key, &byte) > 0 &&
                    key == HEART_ECC_KEY(s->ecc_words[index])) {
                    /* Avoid splitting a poisoned access before rejection. */
                    HeartECCRange range = { MAX(HEART_RAM_BASE, address - 8),
                                            address + 16 };

                    g_array_append_val(ranges, range);
                }
                index++;
            }
        }
    }
    g_array_sort(ranges, heart_ecc_range_compare);
    for (unsigned i = 0; i < ranges->len;) {
        HeartECCRange range = g_array_index(ranges, HeartECCRange, i++);
        HeartECCRegion *region;

        while (i < ranges->len &&
               g_array_index(ranges, HeartECCRange, i).base <= range.end) {
            range.end = MAX(range.end,
                           g_array_index(ranges, HeartECCRange, i++).end);
        }
        region = NULL;
        for (unsigned j = 0; j < s->ecc_regions->len; j++) {
            HeartECCRegion *candidate = g_ptr_array_index(s->ecc_regions, j);

            if (!candidate->mapped) {
                region = candidate;
                break;
            }
        }
        if (!region) {
            region = g_new0(HeartECCRegion, 1);
            memory_region_init_alias(&region->memory, OBJECT(s),
                                     "heart.ecc-alias", &s->ecc_memory,
                                     range.base - HEART_RAM_BASE,
                                     range.end - range.base);
            g_ptr_array_add(s->ecc_regions, region);
        } else {
            memory_region_set_alias_offset(&region->memory,
                                           range.base - HEART_RAM_BASE);
            memory_region_set_size(&region->memory, range.end - range.base);
        }
        memory_region_add_subregion_overlap(memory, range.base,
                                             &region->memory, HEART_BANKS + 1);
        region->mapped = true;
    }
    memory_region_transaction_commit();
}

static void heart_map_bank(SGIHeartState *s, unsigned bank)
{
    uint32_t d = s->descriptor[bank];
    MemoryRegion *memory = s->cpu_memory ?: get_system_memory();
    uint64_t base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
    unsigned units = (d >> 16 & 0x3f) + 1;

    memory_region_transaction_begin();
    for (unsigned unit = 0; unit < HEART_MAX_UNITS; unit++) {
        if (s->mapped[bank][unit]) {
            memory_region_del_subregion(memory, &s->views[bank][unit]);
            s->mapped[bank][unit] = false;
        }
    }
    if (d & 0x80000000U) {
        for (unsigned unit = 0; unit < units; unit++) {
            if (s->capacity[bank]) {
                memory_region_set_alias_offset(&s->views[bank][unit],
                    heart_bank_offset(d, s->capacity[bank],
                                      (uint64_t)unit * HEART_UNIT));
            }
            memory_region_add_subregion_overlap(memory,
                base + (uint64_t)unit * HEART_UNIT,
                &s->views[bank][unit], bank + 1);
            s->mapped[bank][unit] = true;
        }
    }
    memory_region_transaction_commit();
}

static void heart_map_all(SGIHeartState *s)
{
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        heart_map_bank(s, bank);
    }
    heart_ecc_map(s);
}

static uint64_t heart_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIHeartState *s = opaque;

    if (offset == 0x00 || offset == 0x98 || offset == 0xa0 ||
        offset == 0x10040) {
        trace_sgi_heart_piu_access(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                   false, offset, size,
                                   size != 8 ? 0 :
                                   offset == 0x00 ? s->mode :
                                   offset == 0x98 ? s->ecc_address :
                                   offset == 0xa0 ? s->ecc_data : s->ecc_cause);
    }

    if (offset >= 0x20 && offset < 0x40) {
        unsigned n = (offset - 0x20) / 4;

        if (size == 4) {
            return s->descriptor[n];
        }
        if (size == 8 && !(n & 1)) {
            return ((uint64_t)s->descriptor[n] << 32) |
                   s->descriptor[n + 1];
        }
    }
    if (offset == 0xb8 && size == 8) {
        heart_onewire_refresh(s);
        return s->onewire_control;
    }
    if (offset == 0xbc && size == 4) {
        heart_onewire_refresh(s);
        return s->onewire_control;
    }
    if (size == 8) {
        switch (offset) {
        case 0x00: return s->mode;
        case 0x08: return s->sdram_mode;
        case 0x10: return s->mem_refresh;
        case 0x18: return s->mem_req_arb;
        case 0x50: return s->flow_cell[0];
        case 0x58: return s->flow_cell[1];
        case 0x80: return 0x18;
        case 0x98: return s->ecc_address;
        case 0xa0: return s->ecc_data;
        case 0x10040: return s->ecc_cause;
        case 0xb0: return s->clock_divider;
        case 0x10000 ... 0x10018:
            if (!(offset & 7)) {
                return s->mask[(offset - 0x10000) / 8];
            }
            break;
        case 0x10030: return s->pending;
        case 0x10038: return s->pending & s->mask[0];
        case 0x20000: return heart_counter(s);
        case 0x30000: return s->compare;
        case 0x50000: return 0;
        default:
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported %u-byte read at 0x%"
                  HWADDR_PRIx "\n", size, offset);
    return 0;
}

static MemTxResult heart_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;

    if (offset == 0x00 || offset == 0x98 || offset == 0xa0 ||
        offset == 0x10040) {
        trace_sgi_heart_piu_access(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                   true, offset, size, value);
    }

    if (offset >= 0x20 && offset < 0x40) {
        unsigned n = (offset - 0x20) / 4;

        if (s->ecc_word_count) {
            return heart_ecc_reject(offset, size, "descriptor remap");
        }

        if (size == 4) {
            s->descriptor[n] = value;
            if (n < HEART_BANKS) {
                heart_map_bank(s, n);
            }
            heart_ecc_map(s);
            return MEMTX_OK;
        }
        if (size == 8 && !(n & 1)) {
            s->descriptor[n] = value >> 32;
            s->descriptor[n + 1] = value;
            for (unsigned i = n; i < n + 2 && i < HEART_BANKS; i++) {
                heart_map_bank(s, i);
            }
            heart_ecc_map(s);
            return MEMTX_OK;
        }
    }
    if (offset == 0xb8 && size == 8) {
        heart_onewire_start(s, value);
        return MEMTX_OK;
    }
    if (offset == 0xbc && size == 4) {
        heart_onewire_start(s, value);
        return MEMTX_OK;
    }
    if (size == 8) {
        switch (offset) {
        case 0x00:
            s->mode = value;
            heart_ecc_map(s);
            heart_update_irq(s);
            return MEMTX_OK;
        case 0x10040:
            s->ecc_cause &= ~value;
            return MEMTX_OK;
        case 0x08:
            s->sdram_mode = value;
            return MEMTX_OK;
        case 0x10:
            s->mem_refresh = value;
            return MEMTX_OK;
        case 0x18:
            s->mem_req_arb = value;
            return MEMTX_OK;
        case 0x50:
            s->flow_cell[0] = value;
            return MEMTX_OK;
        case 0x58:
            s->flow_cell[1] = value;
            return MEMTX_OK;
        case 0xb0:
            s->clock_divider = value;
            return MEMTX_OK;
        case 0x30000:
            s->compare = value & 0xffffff;
            return MEMTX_OK;
        case 0x10000 ... 0x10018:
            if (!(offset & 7)) {
                uint64_t old = s->mask[(offset - 0x10000) / 8];

                s->mask[(offset - 0x10000) / 8] = value;
                trace_sgi_heart_mask_write(
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    (offset - 0x10000) / 8, old, value, s->pending);
                heart_update_irq(s);
                return MEMTX_OK;
            }
            break;
        case 0x10020:
            trace_sgi_heart_pending_write(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                          offset, value, s->pending,
                                          s->pending | value, s->mask[0]);
            s->pending |= value;
            heart_update_irq(s);
            return MEMTX_OK;
        case 0x10028:
            trace_sgi_heart_pending_write(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                          offset, value, s->pending,
                                          s->pending & ~value, s->mask[0]);
            s->pending &= ~value;
            heart_update_irq(s);
            return MEMTX_OK;
        default:
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported %u-byte write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n", size, offset,
                  value);
    return MEMTX_OK;
}

static const MemoryRegionOps heart_ops = {
    .read = heart_read,
    .write_with_attrs = heart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void heart_reset_enter(Object *obj, ResetType type)
{
    SGIHeartState *s = SGI_HEART(obj);

    s->ecc_word_count = 0;
    s->ecc_cause = 0;
    s->ecc_address = 0;
    s->ecc_data = 0;
    s->mode = 0;
    s->sdram_mode = 0;
    s->mem_refresh = 0;
    s->mem_req_arb = 0;
    memset(s->flow_cell, 0, sizeof(s->flow_cell));
    s->clock_divider = 0;
    s->compare = 0;
    memset(s->descriptor, 0, sizeof(s->descriptor));
    memset(s->mask, 0, sizeof(s->mask));
    s->pending = 0;
    s->saved_count = 0;
    s->widget_control = 0;
    s->widget_request_timeout = 0;
    s->widget_error_mask = 0;
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->onewire_timer);
    s->onewire_control = 0;
    s->onewire_level = false;
    s->onewire_busy = false;
    s->onewire_deadline_ns = 0;
}

static void heart_reset_hold(Object *obj, ResetType type)
{
    SGIHeartState *s = SGI_HEART(obj);

    heart_map_all(s);
    heart_update_irq(s);
}

static int heart_pre_save(void *opaque)
{
    SGIHeartState *s = opaque;

    s->saved_count = heart_counter(s);
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->onewire_remaining_ns = s->onewire_busy ?
        MAX(0, s->onewire_deadline_ns - s->counter_epoch_ns) : 0;
    return 0;
}

static int heart_pre_load(void *opaque)
{
    SGIHeartState *s = opaque;

    g_clear_pointer(&s->ecc_words, g_free);
    s->ecc_word_count = 0;
    s->ecc_word_capacity = 0;
    return 0;
}

static int heart_post_load(void *opaque, int version_id)
{
    SGIHeartState *s = opaque;

    s->ecc_word_capacity = s->ecc_word_count;
    for (unsigned i = 0; i < s->ecc_word_count; i++) {
        unsigned bank = s->ecc_words[i] >> 32;
        uint32_t offset = HEART_ECC_KEY(s->ecc_words[i]);

        if (bank >= HEART_BANKS || offset >= s->capacity[bank] ||
            (s->ecc_words[i] & 6) ||
            (i && HEART_ECC_KEY(s->ecc_words[i - 1]) >=
                  HEART_ECC_KEY(s->ecc_words[i]))) {
            return -EINVAL;
        }
    }
    s->saved_count &= HEART_COUNT_MASK;
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->onewire_timer);
    if (s->onewire_busy) {
        s->onewire_deadline_ns = s->counter_epoch_ns +
                                 s->onewire_remaining_ns;
        timer_mod(s->onewire_timer, s->onewire_deadline_ns);
    }
    heart_map_all(s);
    heart_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_heart = {
    .name = TYPE_SGI_HEART,
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = heart_pre_save,
    .pre_load = heart_pre_load,
    .post_load = heart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ecc_word_count, SGIHeartState),
        VMSTATE_VARRAY_UINT32_ALLOC(ecc_words, SGIHeartState, ecc_word_count,
                                   0, vmstate_info_uint64, uint64_t),
        VMSTATE_UINT64(ecc_cause, SGIHeartState),
        VMSTATE_UINT64(ecc_address, SGIHeartState),
        VMSTATE_UINT64(ecc_data, SGIHeartState),
        VMSTATE_UINT64(mode, SGIHeartState),
        VMSTATE_UINT64(sdram_mode, SGIHeartState),
        VMSTATE_UINT64(mem_refresh, SGIHeartState),
        VMSTATE_UINT64(mem_req_arb, SGIHeartState),
        VMSTATE_UINT64_ARRAY(flow_cell, SGIHeartState, 2),
        VMSTATE_UINT64(clock_divider, SGIHeartState),
        VMSTATE_UINT32(compare, SGIHeartState),
        VMSTATE_UINT32_ARRAY(descriptor, SGIHeartState, 8),
        VMSTATE_UINT64_ARRAY(mask, SGIHeartState, 4),
        VMSTATE_UINT64(pending, SGIHeartState),
        VMSTATE_UINT64(saved_count, SGIHeartState),
        VMSTATE_UINT32(widget_control, SGIHeartState),
        VMSTATE_UINT32(widget_request_timeout, SGIHeartState),
        VMSTATE_UINT32(widget_error_mask, SGIHeartState),
        VMSTATE_UINT64(onewire_control, SGIHeartState),
        VMSTATE_INT64(onewire_remaining_ns, SGIHeartState),
        VMSTATE_BOOL(onewire_level, SGIHeartState),
        VMSTATE_BOOL(onewire_busy, SGIHeartState),
        VMSTATE_END_OF_LIST()
    },
};

static void heart_realize(DeviceState *dev, Error **errp)
{
    SGIHeartState *s = SGI_HEART(dev);

    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        uint64_t capacity = s->capacity[bank];
        g_autofree char *bank_name = g_strdup_printf("heart.bank%u", bank);

        if (capacity != 0 && capacity != 64 * MiB &&
            capacity != 128 * MiB && capacity != 256 * MiB) {
            error_setg(errp,
                       "HEART bank %u capacity must be 0, 64M, 128M or 256M",
                       bank);
            return;
        }
        if (capacity && !memory_region_init_ram(&s->backing[bank],
                OBJECT(dev), bank_name, capacity, errp)) {
            return;
        }
        for (unsigned unit = 0; unit < HEART_MAX_UNITS; unit++) {
            g_autofree char *name = g_strdup_printf("heart.bank%u.unit%u",
                                                     bank, unit);

            if (capacity) {
                memory_region_init_alias(&s->views[bank][unit], OBJECT(dev),
                    name, &s->backing[bank],
                    ((uint64_t)unit * HEART_UNIT) % capacity, HEART_UNIT);
            } else {
                memory_region_init_io(&s->views[bank][unit], OBJECT(dev),
                    &heart_absent_ops, s, name, HEART_UNIT);
            }
        }
    }
}

static const Property heart_properties[] = {
    DEFINE_PROP_LINK("cpu-memory", SGIHeartState, cpu_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("bank0-size", SGIHeartState, capacity[0], 0),
    DEFINE_PROP_UINT64("bank1-size", SGIHeartState, capacity[1], 0),
    DEFINE_PROP_UINT64("bank2-size", SGIHeartState, capacity[2], 0),
    DEFINE_PROP_UINT64("bank3-size", SGIHeartState, capacity[3], 0),
    DEFINE_PROP_UINT8("widget-revision", SGIHeartState, widget_revision, 0),
    DEFINE_PROP_UINT64("widget-id-64", SGIHeartState, widget_id_64, 0),
    DEFINE_PROP_BOOL("widget-status-f-measured", SGIHeartState,
                     widget_status_f_measured, false),
};

static void heart_init(Object *obj)
{
    SGIHeartState *s = SGI_HEART(obj);

    s->ecc_regions = g_ptr_array_new();
    /* Old flat views retain this fixed address origin when aliases move. */
    memory_region_init_io(&s->ecc_memory, obj, &heart_ecc_ops, s,
                          "heart.ecc", HEART_RAM_PACKET_SIZE + 8);
    memory_region_init_io(&s->registers, obj, &heart_ops, s,
                          TYPE_SGI_HEART, HEART_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    memory_region_init_io(&s->widget, obj, &heart_widget_ops, s,
                          "sgi-heart.widget", HEART_WIDGET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->widget);
    memory_region_init(&s->packet, obj, "sgi-heart.packet",
                       HEART_PACKET_SIZE);
    memory_region_init_io(&s->packet_control, obj, &heart_packet_ops, s,
                          "sgi-heart.packet-control", HEART_WIDGET_SIZE);
    memory_region_add_subregion(&s->packet, 0, &s->packet_control);
    memory_region_init_io(&s->packet_ram, obj, &heart_packet_ram_ops, s,
                          "sgi-heart.packet-ram", HEART_RAM_PACKET_SIZE);
    memory_region_add_subregion(&s->packet, HEART_RAM_BASE, &s->packet_ram);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->packet);
    qdev_init_gpio_out_named(DEVICE(obj), s->processor, "processor", 5);
    s->onewire_bus = onewire_create_bus(DEVICE(obj), "onewire");
    s->onewire_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   heart_onewire_complete, s);
}

static void heart_finalize(Object *obj)
{
    SGIHeartState *s = SGI_HEART(obj);

    timer_free(s->onewire_timer);
    g_free(s->ecc_words);
    for (unsigned i = 0; i < s->ecc_regions->len; i++) {
        HeartECCRegion *region = g_ptr_array_index(s->ecc_regions, i);

        g_free(region);
    }
    g_ptr_array_free(s->ecc_regions, true);
}

static const char *heart_xtalk_describe(Object *port, unsigned int index)
{
    return "HEART widget";
}

static MemoryRegion *heart_xtalk_cpu_face(Object *port, unsigned int index)
{
    return &SGI_HEART(port)->widget;
}

static MemoryRegion *heart_xtalk_packet_face(Object *port,
                                             unsigned int index)
{
    return &SGI_HEART(port)->packet;
}

static void heart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(klass);

    dc->desc = "SGI IP30 HEART standalone functional profile";
    dc->realize = heart_realize;
    dc->vmsd = &vmstate_heart;
    dc->user_creatable = true;
    device_class_set_props(dc, heart_properties);
    rc->phases.enter = heart_reset_enter;
    rc->phases.hold = heart_reset_hold;
    pc->describe = heart_xtalk_describe;
    pc->cpu_face = heart_xtalk_cpu_face;
    pc->packet_face = heart_xtalk_packet_face;
}

static const TypeInfo heart_info = {
    .name = TYPE_SGI_HEART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIHeartState),
    .instance_init = heart_init,
    .instance_finalize = heart_finalize,
    .class_init = heart_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XIO_PORT },
        { },
    },
};

static void heart_register_types(void)
{
    type_register_static(&heart_info);
}

type_init(heart_register_types)
