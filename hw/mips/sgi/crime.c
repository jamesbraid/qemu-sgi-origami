/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI CRIME processor interfaces and MACE interrupts.
 * The default 1.5 interface has an explicit legacy software alternative.
 * An opt-in standalone profile decodes eight logical 128 MiB RAM banks.
 * An opt-in synchronous linear transfer approximation uses healthy RAM.
 * Physical ECC, watchdog, rasterization, GBE and VICE are not modeled.
 * Unsupported register operations fail visibly.
 */
#include "qemu/osdep.h"
#include "hw/mips/sgi/crime.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/runstate.h"

#define CRIME_REGISTER_SIZE 0x2000000
#define CRIME_RENDER_STATUS 0x01004000
#define CRIME_EMPTY_RENDER_STATUS 0x1e000000U
#define CRIME_FITTED_RAM (64 * MiB)
#define CRIME_OPTIONAL_RAM (128 * MiB)
#define CRIME_BANK_SIZE (128 * MiB)
#define CRIME_SMALL_BANK_SIZE (32 * MiB)
#define CRIME_BANKED_RAM (1 * GiB)
#define CRIME_LOW_RAM_SIZE (256 * MiB)
#define CRIME_HIGH_RAM 0x20000000ULL
#define CRIME_HIGH_RAM_SIZE (512 * MiB)
#define CRIME_BANK_FIELDS 0x11fU
#define CRIME_BANK_64_MBIT 0x100U
#define CRIME_ERROR_READ (1U << 25)
#define CRIME_ERROR_WRITE (1U << 26)
#define CRIME_ERROR_RMW (1U << 27)
#define CRIME_FULL_RAM 0x40000000ULL
#define CRIME_NO_ECC_RAM 0x80000000ULL
#define CRIME_CARRIER_SPAN (CRIME_NO_ECC_RAM + CRIME_BANKED_RAM)
#define CRIME_MACE_MASK 0xffffU
#define CRIME_SOFTWARE_MASK 0x30000000U
#define CRIME_LEGACY_SOFTWARE_MASK 0x70000000U
#define CRIME_RENDER_IRQ_MASK 0x0fc00000U
#define CRIME_LATCH_MASK 0x41cf0000U
#define CRIME_LEGACY_LATCH_MASK 0x81cf0000U
#define CRIME_IP2_MASK 0x8fc00038U
#define CRIME_IP3_MASK 0x000fff07U
#define CRIME_IP4_MASK (1U << 28)
#define CRIME_IP5_MASK ((1U << 6) | (1U << 29))
#define CRIME_IP6_MASK 0x40300080U
#define CRIME_ID_REVISION 0xb1U
#define CRIME_TIMER_HZ 66666666U

#define CRIME_LINEAR_TLB 0x1001700
#define CRIME_TRANSFER 0x1003000
#define CRIME_TLB_ENTRY_MASK 0x800fffffU
#define CRIME_TRANSFER_PARAMETERS 10

typedef struct CrimeTransferSpan {
    uint64_t physical;
    uint64_t offset;
    unsigned length;
} CrimeTransferSpan;

static MemTxResult crime_transfer_fail(hwaddr offset, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-crime: rejected access at 0x%" HWADDR_PRIx ": %s\n",
                  (hwaddr)(0x14000000ULL + offset), reason);
    vm_stop(RUN_STATE_INTERNAL_ERROR);
    return MEMTX_ERROR;
}

static void crime_healthy_memory_notice(SGICrimeState *s)
{
    if (!s->healthy_memory_reported) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: healthy-memory approximation: coherent "
                      "RAM without ECC check bits or correction\n");
        s->healthy_memory_reported = true;
    }
}

static bool crime_tlb_word_valid(uint64_t word)
{
    return !(((uint32_t)word | (uint32_t)(word >> 32)) & ~CRIME_TLB_ENTRY_MASK);
}

static bool crime_transfer_parameter(unsigned offset, unsigned *index)
{
    switch (offset) {
    case 0x00: case 0x08: case 0x18: case 0x20: case 0x28:
    case 0x30: case 0x38: case 0x40: case 0x48:
        *index = offset / 8;
        return true;
    default:
        return false;
    }
}

/* Resolve the existing decoder's FlatView without invoking its error MMIO. */
static bool crime_transfer_resolve(SGICrimeState *s, uint64_t physical,
                                    unsigned length, CrimeTransferSpan *span)
{
    uint64_t available;
    MemoryRegionSection section;
    bool valid;

    if (!(physical < CRIME_LOW_RAM_SIZE ||
          (physical >= CRIME_HIGH_RAM && physical < CRIME_FULL_RAM) ||
          (physical >= CRIME_FULL_RAM && physical < 0xc0000000ULL))) {
        return false;
    }
    section = memory_region_find(get_system_memory(), physical, length);
    available = int128_get64(section.size);
    valid = section.mr == s->ram && !section.readonly && available >= length;
    if (valid) {
        span->physical = physical;
        span->offset = section.offset_within_region;
        span->length = length;
    }
    memory_region_unref(section.mr);
    return valid;
}

static bool crime_transfer_spans(SGICrimeState *s, unsigned bank,
                                  uint32_t first, uint32_t last,
                                  CrimeTransferSpan *spans, unsigned *count,
                                  uint64_t *length)
{
    uint64_t position = first;

    if (last < first) {
        return false;
    }
    *length = (uint64_t)last - first + 1;
    if (*length > 0x20000 - (first & 0xfff)) {
        return false;
    }
    *count = 0;
    while (position <= last) {
        unsigned entry_index = (position >> 12) & 31;
        uint64_t word = s->transfer.tlb[bank * 16 + entry_index / 2];
        uint32_t entry = entry_index & 1 ? word : word >> 32;
        unsigned bytes = MIN((uint64_t)(4096 - (position & 0xfff)),
                             (uint64_t)last - position + 1);
        uint64_t physical = ((uint64_t)(entry & 0xfffff) << 12) |
                            (position & 0xfff);

        if (!(entry & 0x80000000U) ||
            !crime_transfer_resolve(s, physical, bytes, &spans[*count])) {
            return false;
        }
        (*count)++;
        position += bytes;
    }
    return true;
}

static bool crime_transfer_overlap(const CrimeTransferSpan *a,
                                    const CrimeTransferSpan *b)
{
    return a->offset < b->offset + b->length &&
           b->offset < a->offset + a->length;
}

static MemTxResult crime_transfer_commit(SGICrimeState *s, hwaddr offset,
                                          unsigned index, uint32_t value)
{
    uint32_t parameter[CRIME_TRANSFER_PARAMETERS];
    CrimeTransferSpan source[32], destination[32];
    unsigned source_count = 0, destination_count;
    uint64_t source_length, destination_length;
    uint32_t mode, src, dst;
    bool copy;
    g_autofree uint8_t *buffer = NULL;
    unsigned position;

    memcpy(parameter, s->transfer.parameter, sizeof(parameter));
    parameter[index] = value;
    mode = parameter[0];
    copy = mode & (1U << 11);
    src = (mode >> 5) & 7;
    dst = (mode >> 2) & 7;
    if ((mode & ~0x8ffU) || (dst != 4 && dst != 5) ||
        (copy && src != 4 && src != 5) || parameter[1] != UINT32_MAX) {
        return crime_transfer_fail(offset, "unsupported transfer mode or mask");
    }
    if (!crime_transfer_spans(s, dst - 4, parameter[6], parameter[7],
                              destination, &destination_count,
                              &destination_length)) {
        return crime_transfer_fail(offset,
                                           "invalid destination translation or bounds");
    }
    if (copy &&
        (!crime_transfer_spans(s, src - 4, parameter[4], parameter[5],
                               source, &source_count, &source_length) ||
         source_length != destination_length)) {
        return crime_transfer_fail(offset,
                                           "invalid source translation or bounds");
    }
    for (unsigned i = 0; i < destination_count; i++) {
        for (unsigned j = 0; j < i; j++) {
            if (crime_transfer_overlap(&destination[i], &destination[j])) {
                return crime_transfer_fail(offset,
                                           "destination aliases itself");
            }
        }
        for (unsigned j = 0; j < source_count; j++) {
            if (crime_transfer_overlap(&destination[i], &source[j])) {
                return crime_transfer_fail(offset,
                                           "source and destination overlap");
            }
        }
    }
    if ((mode & 1) || (copy && (mode & 2))) {
        crime_healthy_memory_notice(s);
    }
    /* MMIO holds the BQL, so the validated RAM mappings remain stable. */
    buffer = g_malloc(destination_length);
    if (copy) {
        position = 0;
        for (unsigned i = 0; i < source_count; i++) {
            CrimeTransferSpan *span = &source[i];

            if (address_space_read(&address_space_memory, span->physical,
                                   MEMTXATTRS_UNSPECIFIED, buffer + position,
                                   span->length) != MEMTX_OK) {
                return crime_transfer_fail(offset, "validated RAM read failed");
            }
            position += span->length;
        }
    } else {
        memset(buffer, parameter[3] & 0xff, destination_length);
    }
    position = 0;
    for (unsigned i = 0; i < destination_count; i++) {
        CrimeTransferSpan *span = &destination[i];

        if (address_space_write(&address_space_memory, span->physical,
                                MEMTXATTRS_UNSPECIFIED, buffer + position,
                                span->length) != MEMTX_OK) {
            return crime_transfer_fail(offset, "validated RAM write failed");
        }
        position += span->length;
    }
    s->transfer.parameter[index] = value;
    return MEMTX_OK;
}

static MemTxResult crime_transfer_read(SGICrimeState *s, hwaddr offset,
                                       uint64_t *value, unsigned size)
{
    unsigned index;

    if (offset >= CRIME_LINEAR_TLB && offset < CRIME_LINEAR_TLB + 256 &&
        size == 8 && !(offset & 7)) {
        *value = s->transfer.tlb[(offset - CRIME_LINEAR_TLB) / 8];
        return MEMTX_OK;
    }
    if (offset == CRIME_RENDER_STATUS && size == 4) {
        *value = CRIME_EMPTY_RENDER_STATUS;
        return MEMTX_OK;
    }
    if (offset >= CRIME_TRANSFER && offset < CRIME_TRANSFER + 0x50 &&
        size == 4 &&
        crime_transfer_parameter(offset - CRIME_TRANSFER, &index) &&
        index != 6 && index != 7) {
        *value = s->transfer.parameter[index];
        return MEMTX_OK;
    }
    return crime_transfer_fail(offset, "unsupported transfer register read");
}

static MemTxResult crime_transfer_write(SGICrimeState *s, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    unsigned index;

    if (offset >= CRIME_LINEAR_TLB && offset < CRIME_LINEAR_TLB + 256 &&
        size == 8 && !(offset & 7)) {
        if (!crime_tlb_word_valid(value)) {
            return crime_transfer_fail(offset, "reserved translation bits");
        }
        s->transfer.tlb[(offset - CRIME_LINEAR_TLB) / 8] = value;
        return MEMTX_OK;
    }
    if ((offset == CRIME_TRANSFER + 0x800 ||
         offset == CRIME_TRANSFER + 0x838) && size == 4) {
        index = (offset - CRIME_TRANSFER - 0x800) / 8;
        return crime_transfer_commit(s, offset, index, value);
    }
    if (offset >= CRIME_TRANSFER && offset < CRIME_TRANSFER + 0x50 &&
        size == 4 &&
        crime_transfer_parameter(offset - CRIME_TRANSFER, &index)) {
        s->transfer.parameter[index] = value;
        return MEMTX_OK;
    }
    return crime_transfer_fail(offset, "unsupported transfer register write");
}

static uint32_t crime_software_mask(const SGICrimeState *s)
{
    return s->legacy_software ? CRIME_LEGACY_SOFTWARE_MASK :
                                CRIME_SOFTWARE_MASK;
}

static uint32_t crime_latch_mask(const SGICrimeState *s)
{
    return s->legacy_software ? CRIME_LEGACY_LATCH_MASK : CRIME_LATCH_MASK;
}

static bool crime_enable_valid(const SGICrimeState *s, uint64_t value)
{
    return !(value >> 32) &&
           (!s->functional_memory_transfer || !(value & CRIME_RENDER_IRQ_MASK));
}

static void crime_update_irq(SGICrimeState *s)
{
    uint32_t pending = (s->hardware | s->software) & s->enable;

    if (s->legacy_software) {
        for (unsigned i = 0; i < ARRAY_SIZE(s->cpu_ip); i++) {
            qemu_set_irq(s->cpu_ip[i], i == 0 && pending);
        }
        return;
    }
    qemu_set_irq(s->cpu_ip[0], !!(pending & CRIME_IP2_MASK));
    qemu_set_irq(s->cpu_ip[1], !!(pending & CRIME_IP3_MASK));
    qemu_set_irq(s->cpu_ip[2], !!(pending & CRIME_IP4_MASK));
    qemu_set_irq(s->cpu_ip[3], !!(pending & CRIME_IP5_MASK));
    qemu_set_irq(s->cpu_ip[4], !!(pending & CRIME_IP6_MASK));
}

static void crime_update_hardware(SGICrimeState *s)
{
    uint32_t latch_mask = crime_latch_mask(s);

    s->latched |= s->inputs & latch_mask;
    s->hardware = (s->inputs & ~crime_software_mask(s) & ~latch_mask) |
                  s->latched;
    crime_update_irq(s);
}

static void crime_mace_input(void *opaque, int n, int level)
{
    SGICrimeState *s = opaque;

    if (level) {
        s->inputs |= 1U << n;
    } else {
        s->inputs &= ~(1U << n);
    }
    crime_update_hardware(s);
}

static void crime_external_input(void *opaque, int n, int level)
{
    crime_mace_input(opaque, n + 16, level);
}

static void crime_timer_snapshot(SGICrimeState *s, int64_t now,
                                 uint32_t *count, uint32_t *fraction)
{
    int64_t elapsed_ns = now - s->timer_epoch_ns;
    uint64_t whole_seconds;
    uint64_t partial;

    if (elapsed_ns < 0) {
        elapsed_ns = 0;
    }
    whole_seconds = elapsed_ns / NANOSECONDS_PER_SECOND;
    partial = (elapsed_ns % NANOSECONDS_PER_SECOND) * CRIME_TIMER_HZ +
              s->timer_fraction;
    *count = s->timer_base + whole_seconds * CRIME_TIMER_HZ +
             partial / NANOSECONDS_PER_SECOND;
    *fraction = partial % NANOSECONDS_PER_SECOND;
}

static uint32_t crime_timer_value(SGICrimeState *s)
{
    uint32_t count, fraction;

    crime_timer_snapshot(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                         &count, &fraction);
    return count;
}

static void crime_update_bank_mapping(SGICrimeState *s)
{
    if (!s->alias_profile || !s->ram_aliases_initialized) {
        return;
    }

    memory_region_transaction_begin();
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        memory_region_set_enabled(&s->bank_ram[bank], false);
        memory_region_set_enabled(&s->wrong_geometry[bank], false);
    }
    memory_region_set_enabled(&s->reset_low_ram, false);
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        uint16_t control = s->bank_control[bank];

        if (control & CRIME_BANK_64_MBIT) {
            hwaddr address = ((control >> 2) & 7) * CRIME_BANK_SIZE;

            memory_region_set_address(&s->bank_ram[bank], address);
            memory_region_set_enabled(&s->bank_ram[bank], true);
        } else {
            hwaddr address = (control & 0x1f) * CRIME_SMALL_BANK_SIZE;

            memory_region_set_address(&s->wrong_geometry[bank], address);
            memory_region_set_enabled(&s->wrong_geometry[bank],
                                      bank != 0 || !s->reset_low_ram_active);
        }
    }
    memory_region_set_enabled(&s->reset_low_ram, s->reset_low_ram_active);
    memory_region_transaction_commit();
}

static MemTxResult crime_unsupported_geometry(void *opaque, hwaddr offset)
{
    const uint16_t *control = opaque;
    hwaddr address = (*control & 0x1f) * CRIME_SMALL_BANK_SIZE + offset;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-crime: unsupported 16 Mbit bank geometry at "
                  "RAM offset 0x%" HWADDR_PRIx "\n", address);
    return MEMTX_ERROR;
}

static MemTxResult crime_unsupported_geometry_read(void *opaque, hwaddr offset,
                                                   uint64_t *value,
                                                   unsigned size,
                                                   MemTxAttrs attrs)
{
    *value = 0;
    return crime_unsupported_geometry(opaque, offset);
}

static MemTxResult crime_unsupported_geometry_write(void *opaque,
                                                    hwaddr offset,
                                                    uint64_t value,
                                                    unsigned size,
                                                    MemTxAttrs attrs)
{
    return crime_unsupported_geometry(opaque, offset);
}

static const MemoryRegionOps crime_wrong_geometry_ops = {
    .read_with_attrs = crime_unsupported_geometry_read,
    .write_with_attrs = crime_unsupported_geometry_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult crime_invalid_ram_access(SGICrimeState *s, hwaddr address,
                                             bool write)
{
    s->memory_error_status |= write ? CRIME_ERROR_WRITE : CRIME_ERROR_READ;
    s->memory_error_address = address & 0x3fffffff;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-crime: unmatched RAM %s at offset 0x%" HWADDR_PRIx
                  "\n", write ? "write" : "read", address);
    return MEMTX_ERROR;
}

static MemTxResult crime_invalid_ram_read(void *opaque, hwaddr address,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    *value = 0;
    return crime_invalid_ram_access(opaque, address, false);
}

static MemTxResult crime_invalid_ram_write(void *opaque, hwaddr address,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    return crime_invalid_ram_access(opaque, address, true);
}

static const MemoryRegionOps crime_invalid_ram_ops = {
    .read_with_attrs = crime_invalid_ram_read,
    .write_with_attrs = crime_invalid_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult crime_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    SGICrimeState *s = opaque;

    if (s->functional_memory_transfer && offset >= 0x1000000) {
        return crime_transfer_read(s, offset, value, size);
    }
    if (s->empty_renderer_status && offset == CRIME_RENDER_STATUS &&
        size == 4) {
        *value = CRIME_EMPTY_RENDER_STATUS;
        return MEMTX_OK;
    }
    if (s->prom_id_word && offset == 0x0004 && size == 4) {
        *value = s->legacy_software ? 0xa1 : CRIME_ID_REVISION;
        return MEMTX_OK;
    }
    if (size != 8 || offset & 7) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: invalid %u-byte read at 0x%" HWADDR_PRIx
                      "\n", size, offset);
        return (s->functional_memory_transfer || s->legacy_software) ?
            crime_transfer_fail(offset, "invalid register width or alignment") :
            MEMTX_ERROR;
    }
    if (offset >= 0x208 && offset <= 0x240) {
        unsigned bank = (offset - 0x208) / 8;

        *value = s->eight_128m_banks ? s->bank_control[bank] : bank;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x0000:
        *value = s->legacy_software ? 0xa1 : CRIME_ID_REVISION;
        break;
    case 0x0008:
        *value = s->legacy_software ? 0x100 : 0x120;
        break;
    case 0x0010:
        *value = s->hardware | s->software;
        break;
    case 0x0018:
        *value = s->enable;
        break;
    case 0x0020:
        *value = s->software;
        break;
    case 0x0028:
        *value = s->hardware;
        break;
    case 0x0030: /* Watchdog remains disabled. */
        *value = 0;
        break;
    case 0x0040:
        *value = s->legacy_software ? s->cpu_error_address : 0;
        break;
    case 0x0048:
        if (!s->legacy_software) {
            goto unsupported;
        }
        *value = s->cpu_error_status;
        break;
    case 0x4048:
        if (s->legacy_software) {
            goto unsupported;
        }
        *value = 0;
        break;
    case 0x0038:
        *value = crime_timer_value(s);
        break;
    case 0x0200:
        *value = s->functional_memory_transfer ? s->transfer.ecc_enable : 0;
        break;
    case 0x0270:
        if (s->legacy_software) {
            *value = 0;
            break;
        }
        /* fall through */
    case 0x0260:
        if (s->functional_memory_transfer) {
            *value = 0;
            break;
        }
        goto unsupported;
    case 0x0250:
        if (s->eight_128m_banks || s->functional_memory_transfer ||
            s->legacy_software) {
            *value = s->memory_error_status;
            break;
        }
        goto unsupported;
    case 0x0258:
        if (s->eight_128m_banks || s->functional_memory_transfer ||
            s->legacy_software) {
            *value = s->memory_error_address;
            break;
        }
        goto unsupported;
    default:
    unsupported:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: unsupported read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return (s->functional_memory_transfer || s->legacy_software) ?
            crime_transfer_fail(offset, "unsupported register read") :
            MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult crime_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    SGICrimeState *s = opaque;

    if (s->functional_memory_transfer && offset >= 0x1000000) {
        return crime_transfer_write(s, offset, value, size);
    }
    if (size != 8 || offset & 7) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: invalid %u-byte write at 0x%" HWADDR_PRIx
                      "\n", size, offset);
        return (s->functional_memory_transfer || s->legacy_software) ?
            crime_transfer_fail(offset, "invalid register width or alignment") :
            MEMTX_ERROR;
    }
    if (offset >= 0x208 && offset <= 0x240 && s->eight_128m_banks) {
        unsigned bank = (offset - 0x208) / 8;

        if (!(value & ~(uint64_t)CRIME_BANK_FIELDS)) {
            s->bank_control[bank] = value;
            if (s->reset_low_ram_active) {
                s->reset_low_ram_active = false;
                qemu_log_mask(LOG_UNIMP,
                              "sgi-crime: reset low-RAM approximation ended "
                              "by bank %u control write at register offset "
                              "0x%" HWADDR_PRIx
                              " value 0x%" PRIx64 "\n",
                              bank, offset, value);
            }
            crime_update_bank_mapping(s);
            return MEMTX_OK;
        }
    }
    if (s->legacy_software) {
        switch (offset) {
        case 0x0008:
            if (!value || value == 0x100) {
                return MEMTX_OK;
            }
            break;
        case 0x0010:
        case 0x0030:
        case 0x0250:
        case 0x0270:
            if (!value) {
                return MEMTX_OK;
            }
            break;
        case 0x0200:
            if (!s->functional_memory_transfer && !value) {
                return MEMTX_OK;
            }
            break;
        case 0x0048:
            if (!value) {
                s->cpu_error_status = 0;
                return MEMTX_OK;
            }
            break;
        default:
            break;
        }
    }
    if (s->functional_memory_transfer) {
        switch (offset) {
        case 0x0200:
            if (value <= 1) {
                s->transfer.ecc_enable = value;
                if (value) {
                    crime_healthy_memory_notice(s);
                }
                return MEMTX_OK;
            }
            break;
        case 0x0250:
        case 0x0270:
            if (!value) {
                return MEMTX_OK;
            }
            break;
        default:
            break;
        }
    }
    switch (offset) {
    case 0x0018:
        if (crime_enable_valid(s, value)) {
            s->enable = value;
            crime_update_irq(s);
            return MEMTX_OK;
        }
        break;
    case 0x0020:
        if (!(value & ~(uint64_t)crime_software_mask(s))) {
            s->software = value;
            crime_update_irq(s);
            return MEMTX_OK;
        }
        break;
    case 0x0028:
        if (!(value >> 32)) {
            s->latched &= value;
            crime_update_hardware(s);
            return MEMTX_OK;
        }
        break;
    case 0x0038:
        if (s->legacy_software || !(value >> 32)) {
            s->timer_base = value;
            s->timer_fraction = 0;
            s->timer_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            return MEMTX_OK;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-crime: unsupported write at 0x%" HWADDR_PRIx
                  " value 0x%" PRIx64 "\n", offset, value);
    return (s->functional_memory_transfer || s->legacy_software) ?
        crime_transfer_fail(offset, "unsupported register write") : MEMTX_ERROR;
}

static const MemoryRegionOps crime_ops = {
    .read_with_attrs = crime_read,
    .write_with_attrs = crime_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static void crime_reset_enter(Object *obj, ResetType type)
{
    SGICrimeState *s = SGI_CRIME(obj);

    memset(&s->transfer, 0, sizeof(s->transfer));
    s->healthy_memory_reported = false;
    s->latched = 0;
    s->cpu_error_status = 0;
    s->cpu_error_address = 0;
    s->enable = 0;
    s->software = 0;
    s->timer_base = 0;
    s->timer_fraction = 0;
    s->timer_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->memory_error_status = 0;
    s->memory_error_address = 0;
    s->reset_low_ram_active = s->reset_low_ram_approx;
    if (s->reset_low_ram_active) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: reset low-RAM approximation active for "
                      "decoder offsets [0, 0x%" HWADDR_PRIx ")\n",
                      (hwaddr)CRIME_SMALL_BANK_SIZE);
    }
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        s->bank_control[bank] = bank;
    }
}

static void crime_reset_hold(Object *obj, ResetType type)
{
    SGICrimeState *s = SGI_CRIME(obj);

    crime_update_bank_mapping(s);
    crime_update_hardware(s);
}

static bool crime_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t count, fraction;

    crime_timer_snapshot(s, now, &count, &fraction);
    s->timer_base = count;
    s->timer_fraction = fraction;
    s->timer_epoch_ns = now;
    s->incoming.hardware = s->hardware;
    s->incoming.enable = s->enable;
    s->incoming.software = s->software;
    s->incoming.timer_base = s->timer_base;
    s->incoming.timer_fraction = s->timer_fraction;
    return true;
}

static int crime_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;
    CrimeMigrationState *incoming = &s->incoming;
    CrimeProfileState *profile = &s->incoming_profile;
    uint32_t latch_mask = crime_latch_mask(s);
    uint32_t software_mask = crime_software_mask(s);
    uint32_t hardware;

    if ((s->functional_memory_transfer && !s->transfer_state_received) ||
        !s->profile_state_received) {
        return -EINVAL;
    }
    if ((incoming->hardware & software_mask) ||
        !crime_enable_valid(s, incoming->enable) ||
        (incoming->software & ~software_mask)) {
        return -EINVAL;
    }
    if (incoming->timer_fraction >= NANOSECONDS_PER_SECOND) {
        return -EINVAL;
    }
    profile->latched |= profile->inputs & latch_mask;
    hardware = (profile->inputs & ~software_mask & ~latch_mask) |
               profile->latched;
    if (hardware != incoming->hardware ||
        (s->functional_memory_transfer &&
         (profile->memory_error_status != s->incoming_memory_error_status ||
          profile->memory_error_address != s->incoming_memory_error_address))) {
        return -EINVAL;
    }
    s->inputs = profile->inputs;
    s->latched = profile->latched;
    s->cpu_error_address = profile->cpu_error_address;
    s->cpu_error_status = profile->cpu_error_status;
    s->memory_error_status = profile->memory_error_status;
    s->memory_error_address = profile->memory_error_address;
    s->hardware = hardware;
    s->enable = incoming->enable;
    s->software = incoming->software;
    s->timer_base = incoming->timer_base;
    s->timer_fraction = incoming->timer_fraction;
    if (s->functional_memory_transfer) {
        s->transfer = s->incoming_transfer;
        s->healthy_memory_reported = false;
    }
    s->timer_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    crime_update_irq(s);
    return 0;
}

static bool crime_transfer_needed(void *opaque)
{
    return SGI_CRIME(opaque)->functional_memory_transfer;
}

static bool crime_transfer_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;

    s->incoming_transfer = s->transfer;
    s->incoming_memory_error_status = s->memory_error_status;
    s->incoming_memory_error_address = s->memory_error_address;
    return true;
}

static int crime_transfer_pre_load(void *opaque)
{
    return SGI_CRIME(opaque)->functional_memory_transfer ? 0 : -EINVAL;
}

static int crime_transfer_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;
    CrimeTransferState *incoming = &s->incoming_transfer;

    if (incoming->ecc_enable > 1 || incoming->parameter[2] ||
        (s->incoming_memory_error_status &
         ~(CRIME_ERROR_READ | CRIME_ERROR_WRITE | CRIME_ERROR_RMW)) ||
        s->incoming_memory_error_address > 0x3fffffff) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->tlb); i++) {
        if (!crime_tlb_word_valid(incoming->tlb[i])) {
            return -EINVAL;
        }
    }
    s->transfer_state_received = true;
    return 0;
}

static const VMStateDescription vmstate_crime_transfer = {
    .name = TYPE_SGI_CRIME "/linear-transfer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = crime_transfer_needed,
    .pre_save_errp = crime_transfer_pre_save,
    .pre_load = crime_transfer_pre_load,
    .post_load = crime_transfer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(transfer_policy, SGICrimeState),
        VMSTATE_UINT8_EQUAL(interface_revision, SGICrimeState),
        VMSTATE_UINT64_ARRAY(incoming_transfer.tlb, SGICrimeState, 32),
        VMSTATE_UINT32_ARRAY(incoming_transfer.parameter, SGICrimeState, 10),
        VMSTATE_UINT64(incoming_transfer.ecc_enable, SGICrimeState),
        VMSTATE_UINT32(incoming_memory_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_memory_error_address, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static bool crime_profile_needed(void *opaque)
{
    return true;
}

static bool crime_profile_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;
    CrimeProfileState *profile = &s->incoming_profile;

    profile->inputs = s->inputs;
    profile->latched = s->latched;
    profile->cpu_error_address = s->cpu_error_address;
    profile->cpu_error_status = s->cpu_error_status;
    profile->memory_error_status = s->memory_error_status;
    profile->memory_error_address = s->memory_error_address;
    return true;
}

static int crime_profile_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;
    CrimeProfileState *profile = &s->incoming_profile;

    if ((profile->latched & ~crime_latch_mask(s)) ||
        (profile->cpu_error_status & ~7U) ||
        (profile->memory_error_status &
         ~(CRIME_ERROR_READ | CRIME_ERROR_WRITE | CRIME_ERROR_RMW)) ||
        profile->memory_error_address > 0x3fffffff ||
        (!s->legacy_software &&
         (profile->cpu_error_status || profile->cpu_error_address))) {
        return -EINVAL;
    }
    s->profile_state_received = true;
    return 0;
}

static const VMStateDescription vmstate_crime_profile = {
    .name = TYPE_SGI_CRIME "/profile",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = crime_profile_needed,
    .pre_save_errp = crime_profile_pre_save,
    .post_load = crime_profile_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(profile_id, SGICrimeState),
        VMSTATE_UINT8_EQUAL(transfer_policy, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.inputs, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.latched, SGICrimeState),
        VMSTATE_UINT64(incoming_profile.cpu_error_address, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.cpu_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.memory_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.memory_error_address, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static int crime_pre_load(void *opaque)
{
    SGICrimeState *s = opaque;

    s->transfer_state_received = false;
    s->profile_state_received = false;
    return 0;
}

static const VMStateDescription vmstate_crime = {
    .name = TYPE_SGI_CRIME,
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save_errp = crime_pre_save,
    .pre_load = crime_pre_load,
    .post_load = crime_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(incoming.hardware, SGICrimeState),
        VMSTATE_UINT32(incoming.enable, SGICrimeState),
        VMSTATE_UINT32(incoming.software, SGICrimeState),
        VMSTATE_UINT32(incoming.timer_base, SGICrimeState),
        VMSTATE_UINT32(incoming.timer_fraction, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_crime_transfer,
        &vmstate_crime_profile,
        NULL
    },
};

static const VMStateDescription vmstate_crime_populated_banks = {
    .name = "sgi-crime-populated-banks",
    .unmigratable = true,
};

static void crime_realize(DeviceState *dev, Error **errp)
{
    SGICrimeState *s = SGI_CRIME(dev);
    uint64_t ram_size;

    s->profile_id = s->legacy_software ? 1 : 0;
    s->transfer_policy = s->functional_memory_transfer ? 1 : 0;
    if (s->functional_memory_transfer && s->empty_renderer_status) {
        error_setg(errp, "sgi-crime functional-memory-transfer conflicts with "
                   "empty-renderer-status");
        return;
    }
    if (s->functional_memory_transfer && !s->ram) {
        error_setg(errp, "sgi-crime functional-memory-transfer requires RAM");
        return;
    }
    if (s->interface_revision != 1) {
        error_setg(errp, "sgi-crime supports only interface revision 1");
        return;
    }
    if (s->reset_low_ram_approx && !s->eight_128m_banks) {
        error_setg(errp, "sgi-crime reset low-RAM approximation requires "
                   "eight 128 MiB banks");
        return;
    }
    if (s->decoded_carrier && !s->eight_128m_banks) {
        error_setg(errp, "sgi-crime decoded carrier requires eight "
                   "128 MiB banks");
        return;
    }
    if (!s->decoded_carrier && s->decoded_base) {
        error_setg(errp, "sgi-crime decoded base requires a carrier");
        return;
    }
    if (!s->ram) {
        if (s->eight_128m_banks) {
            error_setg(errp, "sgi-crime eight 128 MiB banks require RAM");
        }
        return;
    }
    ram_size = memory_region_size(s->ram);
    if (!memory_region_is_ram(s->ram) ||
        (s->eight_128m_banks && ram_size != CRIME_BANKED_RAM) ||
        (!s->eight_128m_banks && ram_size != CRIME_FITTED_RAM &&
         ram_size != CRIME_OPTIONAL_RAM)) {
        if (s->eight_128m_banks) {
            error_setg(errp, "sgi-crime requires a 1 GiB RAM region for "
                       "eight 128 MiB banks");
        } else {
            error_setg(errp, "sgi-crime requires a 64 or 128 MiB RAM region "
                       "for the synthetic fit");
        }
        return;
    }
    if (memory_region_is_rom(s->ram)) {
        error_setg(errp, "sgi-crime requires writable RAM");
        return;
    }
    if (s->decoded_carrier) {
        if (s->decoded_carrier->alias ||
            s->decoded_carrier->terminates) {
            error_setg(errp, "sgi-crime decoded carrier must be a "
                       "memory container");
            return;
        }
        if (s->decoded_base > UINT64_MAX - CRIME_CARRIER_SPAN ||
            memory_region_size(s->decoded_carrier) <
            s->decoded_base + CRIME_CARRIER_SPAN) {
            error_setg(errp, "sgi-crime decoded carrier does not cover "
                       "all RAM views");
            return;
        }
    }
    if (s->ram_aliases_initialized &&
        (s->ram != s->alias_target ||
         s->eight_128m_banks != s->alias_profile ||
         s->decoded_carrier != s->alias_carrier ||
         s->decoded_base != s->alias_base)) {
        error_setg(errp, "sgi-crime RAM backing, bank profile or decoded "
                   "carrier cannot change after realize");
        return;
    }
    if (s->eight_128m_banks) {
        error_setg(&s->migration_blocker,
                   "CRIME eight 128 MiB banks do not support migration");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
        if (vmstate_register_with_alias_id(
                VMSTATE_IF(dev), VMSTATE_INSTANCE_ID_ANY,
                &vmstate_crime_populated_banks, s, -1, 0, errp) < 0) {
            migrate_del_blocker(&s->migration_blocker);
            return;
        }
        s->bank_migration_marker_registered = true;
    }
    if (!s->ram_aliases_initialized) {
        MemoryRegion *target = s->ram;
        uint64_t size = ram_size;

        if (s->eight_128m_banks) {
            memory_region_init(&s->decoded_ram, OBJECT(dev),
                               "sgi-crime.decoded-ram", CRIME_BANKED_RAM);
            memory_region_init_io(&s->invalid_ram, OBJECT(dev),
                                  &crime_invalid_ram_ops, s,
                                  "sgi-crime.invalid-ram", CRIME_BANKED_RAM);
            memory_region_add_subregion_overlap(&s->decoded_ram, 0,
                                                &s->invalid_ram, -1);
            memory_region_init_alias(&s->reset_low_ram, OBJECT(dev),
                                     "sgi-crime.reset-low-ram", s->ram, 0,
                                     CRIME_SMALL_BANK_SIZE);
            memory_region_set_enabled(&s->reset_low_ram, false);
            memory_region_add_subregion_overlap(&s->decoded_ram, 0,
                                                &s->reset_low_ram,
                                                SGI_CRIME_BANK_COUNT);
            for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
                g_autofree char *name =
                    g_strdup_printf("sgi-crime.bank%u", bank);
                g_autofree char *guard_name =
                    g_strdup_printf("sgi-crime.bank%u-geometry", bank);

                memory_region_init_alias(&s->bank_ram[bank], OBJECT(dev),
                                         name, s->ram,
                                         bank * CRIME_BANK_SIZE,
                                         CRIME_BANK_SIZE);
                memory_region_set_enabled(&s->bank_ram[bank], false);
                memory_region_add_subregion_overlap(&s->decoded_ram, 0,
                                                    &s->bank_ram[bank],
                                                    SGI_CRIME_BANK_COUNT -
                                                    bank);
                memory_region_init_io(&s->wrong_geometry[bank], OBJECT(dev),
                                      &crime_wrong_geometry_ops,
                                      &s->bank_control[bank], guard_name,
                                      CRIME_SMALL_BANK_SIZE);
                memory_region_set_enabled(&s->wrong_geometry[bank], false);
                memory_region_add_subregion_overlap(
                    &s->decoded_ram, 0, &s->wrong_geometry[bank],
                    SGI_CRIME_BANK_COUNT - bank);
                s->bank_control[bank] = bank;
            }
            target = &s->decoded_ram;
            size = CRIME_BANKED_RAM;
            memory_region_init_alias(&s->high_ram, OBJECT(dev),
                                     "sgi-crime.high-ram", target,
                                     CRIME_HIGH_RAM_SIZE,
                                     CRIME_HIGH_RAM_SIZE);
            if (s->decoded_carrier) {
                memory_region_init_alias(&s->carrier_low_ram, OBJECT(dev),
                                         "sgi-crime.carrier-low-ram",
                                         target, 0, CRIME_LOW_RAM_SIZE);
                memory_region_init_alias(&s->carrier_high_ram, OBJECT(dev),
                                         "sgi-crime.carrier-high-ram",
                                         target, CRIME_HIGH_RAM_SIZE,
                                         CRIME_HIGH_RAM_SIZE);
                memory_region_init_alias(&s->carrier_full_ram, OBJECT(dev),
                                         "sgi-crime.carrier-full-ram",
                                         target, 0, CRIME_BANKED_RAM);
                memory_region_init_alias(&s->carrier_no_ecc_ram, OBJECT(dev),
                                         "sgi-crime.carrier-no-ecc-ram",
                                         target, 0, CRIME_BANKED_RAM);
                s->carrier_aliases_initialized = true;
            }
        }
        memory_region_init_alias(&s->low_ram, OBJECT(dev),
                                 "sgi-crime.low-ram", target, 0,
                                 s->eight_128m_banks ?
                                 CRIME_LOW_RAM_SIZE : size);
        memory_region_init_alias(&s->full_ram, OBJECT(dev),
                                 "sgi-crime.full-ram", target, 0, size);
        memory_region_init_alias(&s->no_ecc_ram, OBJECT(dev),
                                 "sgi-crime.no-ecc-ram", target, 0, size);
        s->alias_target = s->ram;
        s->alias_profile = s->eight_128m_banks;
        s->alias_carrier = s->decoded_carrier;
        s->alias_base = s->decoded_base;
        s->ram_aliases_initialized = true;
    }
    memory_region_add_subregion(get_system_memory(), 0, &s->low_ram);
    if (s->eight_128m_banks) {
        memory_region_add_subregion(get_system_memory(), CRIME_HIGH_RAM,
                                    &s->high_ram);
    }
    memory_region_add_subregion(get_system_memory(), CRIME_FULL_RAM,
                                &s->full_ram);
    memory_region_add_subregion(get_system_memory(), CRIME_NO_ECC_RAM,
                                &s->no_ecc_ram);
    if (s->decoded_carrier) {
        memory_region_add_subregion(s->decoded_carrier, s->decoded_base,
                                    &s->carrier_low_ram);
        memory_region_add_subregion(s->decoded_carrier,
                                    s->decoded_base + CRIME_HIGH_RAM,
                                    &s->carrier_high_ram);
        memory_region_add_subregion(s->decoded_carrier,
                                    s->decoded_base + CRIME_FULL_RAM,
                                    &s->carrier_full_ram);
        memory_region_add_subregion(s->decoded_carrier,
                                    s->decoded_base + CRIME_NO_ECC_RAM,
                                    &s->carrier_no_ecc_ram);
    }
    s->ram_aliases_mapped = true;
}

static void crime_unrealize(DeviceState *dev)
{
    SGICrimeState *s = SGI_CRIME(dev);

    if (s->carrier_aliases_initialized) {
        if (s->carrier_no_ecc_ram.container) {
            memory_region_del_subregion(s->carrier_no_ecc_ram.container,
                                        &s->carrier_no_ecc_ram);
        }
        if (s->carrier_full_ram.container) {
            memory_region_del_subregion(s->carrier_full_ram.container,
                                        &s->carrier_full_ram);
        }
        if (s->carrier_high_ram.container) {
            memory_region_del_subregion(s->carrier_high_ram.container,
                                        &s->carrier_high_ram);
        }
        if (s->carrier_low_ram.container) {
            memory_region_del_subregion(s->carrier_low_ram.container,
                                        &s->carrier_low_ram);
        }
    }
    if (s->ram_aliases_mapped) {
        memory_region_del_subregion(get_system_memory(), &s->no_ecc_ram);
        memory_region_del_subregion(get_system_memory(), &s->full_ram);
        if (s->alias_profile) {
            memory_region_del_subregion(get_system_memory(), &s->high_ram);
        }
        memory_region_del_subregion(get_system_memory(), &s->low_ram);
        s->ram_aliases_mapped = false;
    }
    if (s->bank_migration_marker_registered) {
        vmstate_unregister(VMSTATE_IF(dev), &vmstate_crime_populated_banks, s);
        s->bank_migration_marker_registered = false;
    }
    migrate_del_blocker(&s->migration_blocker);
}

static void crime_check_decoded_carrier(const Object *obj,
                                        const char *name, Object *value,
                                        Error **errp)
{
    SGICrimeState *s = SGI_CRIME((Object *)obj);

    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime %s cannot change after initial "
                   "realization", name);
    }
}

static void crime_get_decoded_base(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t value = SGI_CRIME(obj)->decoded_base;

    visit_type_uint64(v, name, &value, errp);
}

static void crime_set_decoded_base(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    SGICrimeState *s = SGI_CRIME(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime %s cannot change after initial "
                   "realization", name);
        return;
    }
    s->decoded_base = value;
}

static bool crime_get_reset_low_ram_approx(Object *obj, Error **errp)
{
    return SGI_CRIME(obj)->reset_low_ram_approx;
}

static void crime_set_reset_low_ram_approx(Object *obj, bool value,
                                           Error **errp)
{
    SGICrimeState *s = SGI_CRIME(obj);

    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime reset-low-ram-approx cannot change "
                   "after initial realization");
        return;
    }
    s->reset_low_ram_approx = value;
}

static bool crime_get_empty_renderer_status(Object *obj, Error **errp)
{
    return SGI_CRIME(obj)->empty_renderer_status;
}

static void crime_set_empty_renderer_status(Object *obj, bool value,
                                            Error **errp)
{
    SGICrimeState *s = SGI_CRIME(obj);

    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime empty-renderer-status cannot change "
                   "after initial realization");
        return;
    }
    s->empty_renderer_status = value;
}

static bool crime_get_functional_memory_transfer(Object *obj, Error **errp)
{
    return SGI_CRIME(obj)->functional_memory_transfer;
}

static void crime_set_functional_memory_transfer(Object *obj, bool value,
                                                 Error **errp)
{
    SGICrimeState *s = SGI_CRIME(obj);

    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime functional-memory-transfer cannot change "
                   "after initial realization");
        return;
    }
    s->functional_memory_transfer = value;
}

static char *crime_get_profile(Object *obj, Error **errp)
{
    return g_strdup(SGI_CRIME(obj)->legacy_software ?
                    "legacy-software" : "1.5");
}

static void crime_set_profile(Object *obj, const char *value, Error **errp)
{
    SGICrimeState *s = SGI_CRIME(obj);

    if (qdev_is_realized(DEVICE(obj)) || s->ram_aliases_initialized) {
        error_setg(errp, "sgi-crime crime-profile cannot change after initial "
                   "realization");
        return;
    }
    if (strcmp(value, "1.5") && strcmp(value, "legacy-software")) {
        error_setg(errp, "crime-profile must be 1.5 or legacy-software");
        return;
    }
    s->legacy_software = !strcmp(value, "legacy-software");
}

static void crime_init(Object *obj)
{
    SGICrimeState *s = SGI_CRIME(obj);

    memory_region_init_io(&s->registers, obj, &crime_ops, s,
                          "sgi-crime.registers", CRIME_REGISTER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    qdev_init_gpio_in_named(DEVICE(obj), crime_mace_input, "mace", 16);
    qdev_init_gpio_in_named(DEVICE(obj), crime_external_input, "external", 16);
    qdev_init_gpio_out_named(DEVICE(obj), s->cpu_ip, "cpu-ip", 5);
    object_property_add_link(obj, "decoded-carrier", TYPE_MEMORY_REGION,
                             (Object **)&s->decoded_carrier,
                             crime_check_decoded_carrier,
                             OBJ_PROP_LINK_STRONG);
    object_property_add(obj, "decoded-base", "uint64",
                        crime_get_decoded_base, crime_set_decoded_base,
                        NULL, NULL);
    object_property_add_bool(obj, "reset-low-ram-approx",
                             crime_get_reset_low_ram_approx,
                             crime_set_reset_low_ram_approx);
    object_property_add_str(obj, "crime-profile", crime_get_profile,
                            crime_set_profile);
    object_property_add_bool(obj, "functional-memory-transfer",
                             crime_get_functional_memory_transfer,
                             crime_set_functional_memory_transfer);
    object_property_add_bool(obj, "empty-renderer-status",
                             crime_get_empty_renderer_status,
                             crime_set_empty_renderer_status);
}

static const Property crime_properties[] = {
    DEFINE_PROP_UINT8("interface-revision", SGICrimeState,
                      interface_revision, 1),
    DEFINE_PROP_LINK("ram", SGICrimeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("eight-128m-banks", SGICrimeState, eight_128m_banks,
                     false),
    DEFINE_PROP_BOOL("prom-id-word", SGICrimeState, prom_id_word, false),
};

static void crime_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI CRIME processor interface";
    dc->realize = crime_realize;
    dc->unrealize = crime_unrealize;
    dc->vmsd = &vmstate_crime;
    dc->user_creatable = true;
    device_class_set_props(dc, crime_properties);
    rc->phases.enter = crime_reset_enter;
    rc->phases.hold = crime_reset_hold;
}

static const TypeInfo crime_info = {
    .name = TYPE_SGI_CRIME,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGICrimeState),
    .instance_init = crime_init,
    .class_init = crime_class_init,
};

static void crime_register_types(void)
{
    type_register_static(&crime_info);
}

type_init(crime_register_types)
