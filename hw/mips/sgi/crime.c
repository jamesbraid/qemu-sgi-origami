/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI CRIME processor interfaces and MACE interrupts.
 * The default 1.5 interface has an explicit legacy software alternative.
 * An opt-in standalone profile decodes eight logical 128 MiB RAM banks.
 * An opt-in synchronous linear transfer approximation uses healthy RAM.
 * The legacy profile includes synchronous 2D and bounded GL color/SZ commands.
 * Physical ECC, watchdog and VICE are not modeled.
 * Unsupported register operations fail visibly.
 */
#include "qemu/osdep.h"
#include "crime-texture.h"
#include "hw/mips/sgi/crime.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "fpu/softfloat.h"
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
#define CRIME_LATCH_MASK 0x41cf0000U
#define CRIME_LEGACY_LATCH_MASK 0x81cf0000U
#define CRIME_IP2_MASK 0x8fc00038U
#define CRIME_IP3_MASK 0x000fff07U
#define CRIME_IP4_MASK (1U << 28)
#define CRIME_IP5_MASK ((1U << 6) | (1U << 29))
#define CRIME_IP6_MASK 0x40300080U
#define CRIME_ID_REVISION 0xb1U
#define CRIME_TIMER_HZ 66666666U

#define CRIME_CLIP_TLB 0x10016e0
#define CRIME_LINEAR_TLB 0x1001700
#define CRIME_TRANSFER 0x1003000
#define CRIME_TLB_ENTRY_MASK 0x800fffffU
#define CRIME_TRANSFER_PARAMETERS 10

typedef struct CrimeTransferSpan {
    uint64_t physical;
    uint64_t offset;
    unsigned length;
} CrimeTransferSpan;

typedef struct CrimeTransferSuppression {
    unsigned validity_clear;
    unsigned outside_mask;
} CrimeTransferSuppression;

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
                                  uint64_t *length,
                                  CrimeTransferSuppression *suppressed)
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

        /* Raw storage is retained; classification is a command policy. */
        if (!(entry & 0x80000000U) || (entry & ~CRIME_TLB_ENTRY_MASK)) {
            if (!suppressed) {
                return false;
            }
            if (!(entry & 0x80000000U)) {
                suppressed->validity_clear++;
            } else {
                suppressed->outside_mask++;
            }
        } else {
            if (!crime_transfer_resolve(s, physical, bytes, &spans[*count])) {
                return false;
            }
            (*count)++;
        }
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

static MemTxResult crime_tiled_commit(SGICrimeState *s, hwaddr offset,
                                       const uint32_t *parameter);

static MemTxResult crime_clip_commit(SGICrimeState *s, hwaddr offset,
                                       const uint32_t *p)
{
    unsigned x0 = p[6] >> 16, y0 = p[6] & 0xffff;
    unsigned x1 = p[7] >> 16, y1 = p[7] & 0xffff;
    unsigned width, height;
    g_autofree CrimeTransferSpan *spans = NULL;
    uint8_t fill[512];

    if ((p[0] & ~0xffU) || p[1] != UINT32_MAX || p[9] != 1 ||
        x0 > x1 || y0 > y1 || x1 >= 512 || y1 >= 2048) {
        return crime_transfer_fail(offset, "unsupported CLIP clear");
    }
    width = x1 - x0 + 1;
    height = y1 - y0 + 1;
    spans = g_new(CrimeTransferSpan, height);
    for (unsigned row = 0; row < height; row++) {
        unsigned y = y0 + row, index = y / 128;
        uint64_t word = s->transfer.clip[index / 4];
        uint16_t entry = word >> (48 - 16 * (index % 4));
        uint64_t physical = ((uint64_t)(entry & 0x7fff) << 16) +
                            (y % 128) * 512 + x0;

        if (!(entry & 0x8000) ||
            !crime_transfer_resolve(s, physical, width, &spans[row])) {
            return crime_transfer_fail(offset, "invalid CLIP destination");
        }
        for (unsigned earlier = 0; earlier < row; earlier++) {
            if (crime_transfer_overlap(&spans[row], &spans[earlier])) {
                return crime_transfer_fail(offset,
                                           "CLIP destination aliases itself");
            }
        }
    }
    if (p[0] & 1) {
        crime_healthy_memory_notice(s);
    }
    /* MMIO holds the BQL across preflight and these ordinary RAM writes. */
    memset(fill, p[3] & 0xff, width);
    for (unsigned row = 0; row < height; row++) {
        if (address_space_write(&address_space_memory, spans[row].physical,
                                MEMTXATTRS_UNSPECIFIED, fill, width) !=
            MEMTX_OK) {
            return crime_transfer_fail(offset,
                                       "validated CLIP RAM write failed");
        }
    }
    return MEMTX_OK;
}

static MemTxResult crime_transfer_commit(SGICrimeState *s, hwaddr offset,
                                          unsigned index, uint32_t value)
{
    uint32_t parameter[CRIME_TRANSFER_PARAMETERS];
    CrimeTransferSpan source[32], destination[32];
    CrimeTransferSuppression suppressed = { 0 };
    unsigned source_count = 0, destination_count;
    uint64_t source_length, destination_length;
    uint32_t mode, src, dst;
    bool copy;
    g_autofree uint8_t *buffer = NULL;
    unsigned position;

    memcpy(parameter, s->transfer.parameter, sizeof(parameter));
    if (index < CRIME_TRANSFER_PARAMETERS) {
        parameter[index] = value;
    }
    mode = parameter[0];
    copy = mode & (1U << 11);
    src = (mode >> 5) & 7;
    dst = (mode >> 2) & 7;
    if (s->legacy_software && (dst < 3 || dst == 6)) {
        MemTxResult result = dst == 6 ?
            crime_clip_commit(s, offset, parameter) :
            crime_tiled_commit(s, offset, parameter);

        if (result == MEMTX_OK && index < CRIME_TRANSFER_PARAMETERS) {
            s->transfer.parameter[index] = value;
        }
        return result;
    }
    if ((mode & ~0x8ffU) || (dst != 4 && dst != 5) ||
        (copy && src != 4 && src != 5) || parameter[1] != UINT32_MAX) {
        return crime_transfer_fail(offset, "unsupported transfer mode or mask");
    }
    if (!crime_transfer_spans(s, dst - 4, parameter[6], parameter[7],
                              destination, &destination_count,
                              &destination_length, copy ? NULL : &suppressed)) {
        return crime_transfer_fail(offset,
                                   "invalid destination translation or bounds");
    }
    if (copy &&
        (!crime_transfer_spans(s, src - 4, parameter[4], parameter[5],
                               source, &source_count, &source_length, NULL) ||
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
        if (index < CRIME_TRANSFER_PARAMETERS) {
            s->transfer.parameter[index] = value;
        }
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
    if (copy) {
        if (index < CRIME_TRANSFER_PARAMETERS) {
            s->transfer.parameter[index] = value;
        }
    } else if (suppressed.validity_clear || suppressed.outside_mask) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: LINEAR CLEAR suppressed %u destination pages "
                      "(validity-bit-clear=%u, outside-mask=%u)\n",
                      suppressed.validity_clear + suppressed.outside_mask,
                      suppressed.validity_clear, suppressed.outside_mask);
    }
    return MEMTX_OK;
}

static bool crime_renderer_touches(hwaddr offset, unsigned size,
                                    hwaddr start, unsigned length)
{
    return offset < start + length && start < offset + size;
}

/* The admitted state is sparse; padding is not another writable register. */
typedef struct CrimeRenderRegister {
    unsigned offset;
    unsigned size;
    bool readable;
    bool native_only;
} CrimeRenderRegister;

static const CrimeRenderRegister crime_render_registers[] = {
    { 0x000, 4, true }, { 0x008, 4, true },
    { 0x010, 4, true }, { 0x018, 4, true },
    { 0x020, 8, true }, { 0x028, 8, true }, { 0x030, 8, true },
    { 0x038, 8, true }, { 0x040, 8, true }, { 0x048, 8, true },
    { 0x050, 4, true }, { 0x058, 4, true },
    { 0x060, 4, false }, { 0x070, 4, false }, { 0x074, 4, false },
    { 0x078, 4, false }, { 0x0a0, 4, false },
    { 0x0a8, 4, false }, { 0x0ac, 4, false },
    { 0x0b0, 4, false }, { 0x0b4, 4, false },
    { 0x0c0, 4, true }, { 0x0c4, 4, true },
    { 0x0d0, 4, false }, { 0x0d8, 4, true },
    { 0x1b0, 4, true }, { 0x1b8, 4, true },
    /* Parameter retention does not admit additional START commands. */
    { 0x080, 4, false, true }, { 0x084, 4, false, true },
    { 0x088, 4, false, true }, { 0x08c, 4, false, true },
    { 0x090, 4, false, true }, { 0x094, 4, false, true },
    { 0x0e0, 4, false, true }, { 0x0e4, 4, false, true },
    { 0x0e8, 4, false, true }, { 0x0ec, 4, false, true },
    { 0x0f0, 4, false, true }, { 0x0f4, 4, false, true },
    { 0x0f8, 4, false, true }, { 0x0fc, 4, false, true },
    { 0x100, 4, false, true }, { 0x104, 4, false, true },
    { 0x108, 4, false, true }, { 0x10c, 4, false, true },
    { 0x110, 4, true, true }, { 0x118, 8, true, true },
    { 0x120, 8, false, true }, { 0x128, 8, false, true },
    { 0x130, 4, false, true }, { 0x134, 4, false, true },
    { 0x138, 8, false, true },
    { 0x140, 8, false, true }, { 0x148, 8, false, true },
    { 0x150, 8, false, true }, { 0x158, 4, false, true },
    { 0x15c, 4, false, true }, { 0x160, 4, true, true },
    { 0x168, 4, true, true }, { 0x170, 4, true, true },
    { 0x178, 4, false, true }, { 0x180, 4, false, true },
    { 0x188, 4, false, true }, { 0x190, 4, false, true },
    { 0x194, 4, false, true }, { 0x198, 4, true, true },
    { 0x1a0, 4, true, true }, { 0x1a8, 4, true, true },
    { 0x1c0, 4, true, true }, { 0x1c8, 8, false, true },
    { 0x1d0, 8, false, true }, { 0x1d8, 8, false, true },
    { 0x1e0, 4, true, true }, { 0x1e8, 4, true, true },
};

static const CrimeRenderRegister *crime_render_register(unsigned offset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(crime_render_registers); i++) {
        if (crime_render_registers[i].offset == offset) {
            return &crime_render_registers[i];
        }
    }
    return NULL;
}

static bool crime_render_texture_start(unsigned offset)
{
    switch (offset) {
    case 0x120: case 0x128: case 0x130:
    case 0x138: case 0x140: case 0x148: case 0x150:
    case 0x158: case 0x15c:
        return true;
    default:
        return false;
    }
}

static bool crime_render_state_access(hwaddr offset, unsigned size)
{
    if (crime_renderer_touches(offset, size, 0x1001000, 0x800)) {
        return true;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(crime_render_registers); i++) {
        const CrimeRenderRegister *reg = &crime_render_registers[i];

        if (crime_renderer_touches(offset, size, 0x1002000 + reg->offset,
                                    reg->size)) {
            return true;
        }
    }
    return false;
}

typedef struct CrimeRenderPixel {
    uint64_t source;
    uint64_t destination;
    uint64_t source_id;
    uint64_t destination_id;
    uint32_t mask;
    uint32_t color;
    unsigned x, y;
    uint64_t sz_address, sz_identity;
    uint32_t sz_value, sz_mask;
} CrimeRenderPixel;

typedef enum CrimeRenderTranslation {
    CRIME_RENDER_TRANSLATION_ERROR,
    CRIME_RENDER_INVALID_TILE,
    CRIME_RENDER_MAPPED,
} CrimeRenderTranslation;

/* Resolve through the populated-bank decoder before coherent DMA. */
static CrimeRenderTranslation crime_framebuffer_translate(SGICrimeState *s,
                                                           unsigned bank,
                                                           uint64_t byte_x,
                                                           unsigned y,
                                                           unsigned bytes,
                                                           uint64_t *physical,
                                                           uint64_t *identity)
{
    uint64_t index = (uint64_t)(y / 128) * 16 + byte_x / 512;
    uint64_t word, address;
    uint16_t entry;
    CrimeTransferSpan span;

    if (bank >= 3 || byte_x >= 8192 || y >= 2048 || index >= 256) {
        return CRIME_RENDER_TRANSLATION_ERROR;
    }
    word = s->render.tlb[bank * 64 + index / 4];
    entry = word >> (48 - 16 * (index % 4));
    if (!(entry & 0x8000)) {
        return CRIME_RENDER_INVALID_TILE;
    }
    address = ((uint64_t)(entry & 0x7fff) << 16) +
              (y % 128) * 512 + byte_x % 512;
    if (!crime_transfer_resolve(s, address, bytes, &span)) {
        return CRIME_RENDER_TRANSLATION_ERROR;
    }
    *physical = address;
    *identity = span.offset;
    return CRIME_RENDER_MAPPED;
}

static CrimeRenderTranslation crime_render_translate(SGICrimeState *s,
                                                      unsigned bank,
                                                      unsigned x, unsigned y,
                                                      unsigned bytes,
                                                      unsigned stride,
                                                      uint64_t *physical,
                                                      uint64_t *identity)
{
    if (x >= 2048) {
        return CRIME_RENDER_TRANSLATION_ERROR;
    }
    return crime_framebuffer_translate(s, bank,
                                        (uint64_t)x * stride + stride - bytes,
                                        y, bytes, physical, identity);
}

static CrimeRenderTranslation crime_render_linear(SGICrimeState *s,
                                                   unsigned bank,
                                                   uint32_t address,
                                                   uint64_t *physical,
                                                   uint64_t *identity)
{
    CrimeTransferSpan span;
    unsigned count;
    uint64_t length;

    if (!crime_transfer_spans(s, bank - 4, address, address, &span,
                              &count, &length, NULL)) {
        return CRIME_RENDER_TRANSLATION_ERROR;
    }
    *physical = span.physical;
    *identity = span.offset;
    return CRIME_RENDER_MAPPED;
}

static int crime_render_compare_destination(const void *a, const void *b,
                                             void *opaque)
{
    const CrimeRenderPixel *pixels = opaque;
    uint64_t x = pixels[*(const uint32_t *)a].destination_id;
    uint64_t y = pixels[*(const uint32_t *)b].destination_id;

    return (x > y) - (x < y);
}

static uint32_t crime_render_byte_mask(const CrimeRenderPixel *p,
                                        unsigned byte, unsigned bytes)
{
    return (p->mask >> (8 * (bytes - 1 - byte))) & 0xff;
}

/* Sorting identities must not change the programmed retirement order. */
static bool crime_render_validate(CrimeRenderPixel *pixels, unsigned count,
                                   unsigned bytes, bool check_overlap,
                                   bool *overlap)
{
    g_autofree uint32_t *order = NULL;

    unsigned enabled = 0;

    *overlap = false;
    if (!count) {
        return true;
    }
    order = g_try_new(uint32_t, count);
    if (!order) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        if (pixels[i].mask) {
            order[enabled++] = i;
        }
    }
    g_qsort_with_data(order, enabled, sizeof(*order),
                      crime_render_compare_destination, pixels);
    for (unsigned i = 0; i < enabled; i++) {
        CrimeRenderPixel *p = &pixels[order[i]];

        for (unsigned j = i; j > 0; j--) {
            CrimeRenderPixel *q = &pixels[order[j - 1]];

            if (q->destination_id + bytes <= p->destination_id) {
                break;
            }
            for (unsigned b = 0; b < bytes; b++) {
                uint64_t address = p->destination_id + b;

                if (crime_render_byte_mask(p, b, bytes) &&
                    address >= q->destination_id &&
                    address < q->destination_id + bytes &&
                    crime_render_byte_mask(q, address - q->destination_id,
                                            bytes)) {
                    return false;
                }
            }
        }
        if (check_overlap) {
            unsigned lo = 0, hi = enabled;

            while (lo < hi) {
                unsigned mid = lo + (hi - lo) / 2;
                uint64_t destination = pixels[order[mid]].destination_id;

                if (destination < p->source_id) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo < enabled && pixels[order[lo]].destination_id ==
                              p->source_id) {
                *overlap = true;
            }
        }
    }
    return true;
}

static uint32_t crime_render_load_pixel(const uint8_t *data, unsigned bytes)
{
    switch (bytes) {
    case 1:
        return data[0];
    case 2:
        return lduw_be_p(data);
    default:
        return ldl_be_p(data);
    }
}

static void crime_render_store_pixel(uint8_t *data, unsigned bytes,
                                      uint32_t value)
{
    switch (bytes) {
    case 1:
        data[0] = value;
        break;
    case 2:
        stw_be_p(data, value);
        break;
    default:
        stl_be_p(data, value);
        break;
    }
}

static uint32_t crime_render_rop(unsigned operation, uint32_t source,
                                  uint32_t destination)
{
    switch (operation) {
    case 0: return 0;
    case 1: return source & destination;
    case 2: return source & ~destination;
    case 3: return source;
    case 4: return ~source & destination;
    case 5: return destination;
    case 6: return source ^ destination;
    case 7: return source | destination;
    case 8: return ~(source | destination);
    case 9: return ~(source ^ destination);
    case 10: return ~destination;
    case 11: return source | ~destination;
    case 12: return ~source;
    case 13: return ~source | destination;
    case 14: return ~(source & destination);
    default: return UINT32_MAX;
    }
}

static MemTxResult crime_render_execute(hwaddr offset,
                                         CrimeRenderPixel *pixels,
                                         unsigned count, unsigned bytes,
                                         bool copy, unsigned rop,
                                         bool read_destination)
{
    for (unsigned i = 0; i < count; i++) {
        CrimeRenderPixel *p = &pixels[i];
        uint8_t source_data[4], destination_data[4];
        uint32_t source = p->color, destination = 0, result;

        if (copy) {
            if (address_space_read(&address_space_memory, p->source,
                                   MEMTXATTRS_UNSPECIFIED, source_data,
                                   bytes) != MEMTX_OK) {
                return crime_transfer_fail(offset, "validated source failed");
            }
            source = crime_render_load_pixel(source_data, bytes);
        }
        if (read_destination) {
            if (address_space_read(&address_space_memory, p->destination,
                                   MEMTXATTRS_UNSPECIFIED, destination_data,
                                   bytes) != MEMTX_OK) {
                return crime_transfer_fail(offset,
                                           "validated destination failed");
            }
            destination = crime_render_load_pixel(destination_data, bytes);
        }
        result = crime_render_rop(rop, source, destination);
        result = (destination & ~p->mask) | (result & p->mask);
        crime_render_store_pixel(destination_data, bytes, result);
        if (address_space_write(&address_space_memory, p->destination,
                                MEMTXATTRS_UNSPECIFIED, destination_data,
                                bytes) != MEMTX_OK) {
            return crime_transfer_fail(offset, "validated RAM write failed");
        }
    }
    return MEMTX_OK;
}

static bool crime_render_mode(uint32_t mode, unsigned *bank, unsigned *bytes,
                                unsigned *stride)
{
    unsigned storage_depth = (mode >> 8) & 3;
    unsigned pixel_type = (mode >> 4) & 15;
    unsigned pixel_depth = (mode >> 2) & 3;

    *bank = (mode >> 10) & 7;
    if ((*bank >= 3 && *bank != 4 && *bank != 5) || (mode & 3)) {
        return false;
    }
    *bytes = 1U << pixel_depth;
    *stride = 1U << storage_depth;
    if (*bank >= 4) {
        return pixel_type == 0 && pixel_depth == 0 && storage_depth == 0;
    }
    return (pixel_type == 0 && pixel_depth == 0 &&
            (storage_depth == 0 || storage_depth == 2)) ||
           (pixel_type == 2 && storage_depth == pixel_depth &&
            (pixel_depth == 1 || pixel_depth == 2));
}

typedef struct CrimeRenderClip {
    unsigned xmin, ymin, xmax, ymax;
} CrimeRenderClip;

static CrimeRenderClip crime_render_clip(uint64_t value)
{
    return (CrimeRenderClip) {
        .xmin = value >> 48, .ymin = (value >> 32) & 0xffff,
        .xmax = (value >> 16) & 0xffff, .ymax = value & 0xffff,
    };
}

static bool crime_render_inside(const CrimeRenderClip *clip, int x, int y)
{
    return x >= clip->xmin && x < clip->xmax &&
           y >= clip->ymin && y < clip->ymax;
}

typedef struct CrimeRenderGeometry {
    unsigned opcode, samples, steps, width, thickness, capacity;
    int x0, y0, x1, y1, dx, dy, lower;
    bool xmajor;
    int64_t vertex[3][2];
    int shade_anchor[2];
} CrimeRenderGeometry;


static int64_t crime_render_edge(const int64_t a[2], const int64_t b[2],
                                  int64_t x, int64_t y)
{
    return (b[0] - a[0]) * (y - a[1]) -
           (b[1] - a[1]) * (x - a[0]);
}


static bool crime_render_gl_field(uint64_t raw, int64_t *field)
{
    if (raw <= 0x0007ffff) {
        *field = raw;
    } else if (raw >= 0x00800000 && raw <= 0x0087ffff) {
        *field = raw - 0x00800000;
    } else if (raw >= 0x48000000 && raw <= 0x4807ffff) {
        *field = raw - 0x48000000;
    } else {
        return false;
    }
    return true;
}

#define CRIME_LINE_SCALE (INT64_C(1) << 20)

typedef struct CrimeGLLine {
    int64_t vertex[2][2];
    int64_t p[2], q[2], u, v, j14, k0, slope;
    float64 normal;
    unsigned width;
    bool xmajor, aa, empty;
} CrimeGLLine;

static int64_t crime_line_floor(int64_t value, int64_t divisor)
{
    return value / divisor - (value % divisor < 0);
}

static uint64_t crime_line_gcd(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t r = a % b;

        a = b;
        b = r;
    }
    return a;
}

static Int128 crime_line_product(int64_t a, int64_t b)
{
    uint64_t low, high;

    muls64(&low, &high, a, b);
    return int128_make128(low, high);
}

static bool crime_line_constant(Int128 value, int64_t *result)
{
    if (!int128_gt(value, int128_makes64(-(INT64_C(1) << 54))) ||
        !int128_lt(value, int128_makes64(INT64_C(1) << 54))) {
        return false;
    }
    *result = (int64_t)int128_getlo(value);
    return true;
}

static bool crime_gl_line_state(const uint64_t *p, CrimeGLLine *line)
{
    uint32_t primitive = p[0x60 / 4];
    uint32_t aa = p[0x190 / 4], cov = p[0x194 / 4];
    int64_t dx, dy;
    float_status fs = { 0 };

    if ((primitive >> 24) != 1 || (primitive & 0x00f80000) ||
        !(p[0x18 / 4] & 0x400000)) {
        return false;
    }
    for (unsigned v = 0; v < 2; v++) {
        for (unsigned axis = 0; axis < 2; axis++) {
            int64_t field;
            int16_t origin = p[0x58 / 4] >> (axis ? 0 : 16);

            if (!crime_render_gl_field(p[(0x80 + 8 * v + 4 * axis) / 4],
                                       &field)) {
                return false;
            }
            line->vertex[v][axis] = field + 64 * (int64_t)origin;
        }
    }
    dx = line->vertex[1][0] - line->vertex[0][0];
    dy = line->vertex[1][1] - line->vertex[0][1];
    line->xmajor = llabs(dx) >= llabs(dy);
    line->aa = p[0x18 / 4] & 0x1000;
    line->empty = !dx && !dy;
    line->width = MAX(1, ((primitive & 0xffff) + 16) / 32);
    if (!line->aa) {
        return true;
    }
    line->slope = (int16_t)(aa >> 16);
    if (line->slope < -16384 || line->slope > 16384 ||
        (cov & ~0x7f7fU) || !(cov & 127) || (cov & 127) > 64 ||
        !((cov >> 8) & 127) || ((cov >> 8) & 127) > 64) {
        return false;
    }
    if (line->empty) {
        return true;
    }
    {
        unsigned major = line->xmajor ? 0 : 1, minor = 1 - major;
        int64_t a = line->vertex[0][major], b = line->vertex[1][major];
        int d = b > a ? 1 : -1;
        int16_t origin = p[0x58 / 4] >> (minor ? 0 : 16);
        int64_t k1, start, end, reference, base, k, remainder;
        int64_t delta[2], divisor, r;
        unsigned cs = (cov >> 8) & 127, ce = cov & 127;

        line->k0 = d * crime_line_floor(d * a + 32, 64);
        k1 = d * (-crime_line_floor(-(d * b + 32), 64) - 1);
        start = 64 * line->k0 + d * (32 - (int)cs);
        end = 64 * k1 + d * ((int)ce - 32);
        if (d * (end - start) <= 0) {
            return false;
        }
        reference = line->vertex[0][minor] * 16384 +
                    line->slope * (64 * line->k0 - a);
        base = 64 * ((aa & 65535) + 16384 * (int64_t)origin);
        k = crime_line_floor(reference - base, 4194304);
        remainder = reference - base - k * 4194304;
        k += remainder * 2 > 4194304;
        line->j14 = (aa & 65535) + 16384 * (int64_t)origin + 65536 * k;
        line->p[major] = 16384 * start;
        line->q[major] = 16384 * end;
        line->p[minor] = 64 * line->j14 +
                         line->slope * (start - 64 * line->k0);
        line->q[minor] = 64 * line->j14 +
                         line->slope * (end - 64 * line->k0);
        for (unsigned axis = 0; axis < 2; axis++) {
            if (llabs(line->p[axis]) >= (INT64_C(1) << 38) ||
                llabs(line->q[axis]) >= (INT64_C(1) << 38)) {
                return false;
            }
        }
        if (line->p[0] > line->q[0] ||
            (line->p[0] == line->q[0] && line->p[1] > line->q[1])) {
            for (unsigned axis = 0; axis < 2; axis++) {
                int64_t t = line->p[axis];

                line->p[axis] = line->q[axis];
                line->q[axis] = t;
            }
        }
        delta[0] = line->q[0] - line->p[0];
        delta[1] = line->q[1] - line->p[1];
        divisor = crime_line_gcd(llabs(delta[0]), llabs(delta[1]));
        line->u = delta[0] / divisor;
        line->v = delta[1] / divisor;
        if (llabs(line->u) > 16384 || llabs(line->v) > 16384 ||
            !crime_line_constant(int128_add(
                crime_line_product(line->u, line->u),
                crime_line_product(line->v, line->v)),
                                 &r) || r <= 0 || r > (INT64_C(1) << 29)) {
            return false;
        }
        set_float_rounding_mode(float_round_nearest_even, &fs);
        line->normal = float64_sqrt(int64_to_float64(r, &fs), &fs);
    }
    return true;
}

typedef struct CrimeLinePoint {
    float64 x, y;
} CrimeLinePoint;

typedef struct CrimeLinePlane {
    float64 x, y, constant;
    bool skip;
} CrimeLinePlane;

static float64 crime_line_evaluate(const CrimeLinePlane *plane,
                                     CrimeLinePoint p, float_status *fs)
{
    float64 a = float64_mul(plane->x, p.x, fs);
    float64 b = float64_mul(plane->y, p.y, fs);

    return float64_add(float64_add(a, b, fs), plane->constant, fs);
}

static void crime_line_cap(Int128 constant, int64_t u, int64_t v,
                             bool *skip, bool *empty)
{
    Int128 lo = INT128_MAX, hi = INT128_MIN;

    for (unsigned corner = 0; corner < 4; corner++) {
        int64_t x = corner & 1 ? CRIME_LINE_SCALE / 2 : -CRIME_LINE_SCALE / 2;
        int64_t y = corner & 2 ? CRIME_LINE_SCALE / 2 : -CRIME_LINE_SCALE / 2;
        Int128 value = int128_sub(
            int128_add(crime_line_product(u, x), crime_line_product(v, y)),
            constant);

        lo = int128_min(lo, value);
        hi = int128_max(hi, value);
    }
    *skip = int128_nonneg(lo);
    *empty = int128_le(hi, int128_zero());
}

static bool crime_gl_line_area(const CrimeGLLine *line, int x, int y,
                                 float64 *area)
{
    int64_t a[2], b[2], ka, kb, h;
    Int128 ia, ib, ih;
    CrimeLinePlane planes[4] = { 0 };
    CrimeLinePoint polygon[12], next[12];
    unsigned count = 4;
    float_status fs = { 0 };
    float64 u, v, fa, fb, fh, total = float64_zero;
    bool empty;

    *area = float64_zero;
    if (line->empty) {
        return true;
    }
    if (x <= -(1 << 18) || x >= (1 << 18) ||
        y <= -(1 << 18) || y >= (1 << 18)) {
        return false;
    }
    set_float_rounding_mode(float_round_nearest_even, &fs);
    for (unsigned axis = 0; axis < 2; axis++) {
        int64_t center = (int64_t)(axis ? y : x) * CRIME_LINE_SCALE;

        a[axis] = line->p[axis] - center;
        b[axis] = line->q[axis] - center;
        if (llabs(a[axis]) >= (INT64_C(1) << 39) ||
            llabs(b[axis]) >= (INT64_C(1) << 39)) {
            return false;
        }
    }
    ia = int128_add(crime_line_product(line->u, a[0]),
                    crime_line_product(line->v, a[1]));
    ib = int128_add(crime_line_product(line->u, b[0]),
                    crime_line_product(line->v, b[1]));
    ih = int128_sub(crime_line_product(line->u, a[1]),
                    crime_line_product(line->v, a[0]));
    if (!crime_line_constant(ia, &ka) || !crime_line_constant(ib, &kb) ||
        !crime_line_constant(ih, &h)) {
        return false;
    }
    crime_line_cap(ia, line->u, line->v, &planes[0].skip, &empty);
    if (empty) {
        return true;
    }
    crime_line_cap(int128_neg(ib), -line->u, -line->v, &planes[1].skip, &empty);
    if (empty) {
        return true;
    }
    u = int64_to_float64(line->u, &fs);
    v = int64_to_float64(line->v, &fs);
    fa = int64_to_float64_scalbn(ka, -20, &fs);
    fb = int64_to_float64_scalbn(kb, -20, &fs);
    fh = int64_to_float64_scalbn(h, -20, &fs);
    planes[0].x = u;
    planes[0].y = v;
    planes[0].constant = float64_chs(fa);
    planes[1].x = float64_chs(u);
    planes[1].y = float64_chs(v);
    planes[1].constant = fb;
    planes[2].x = v;
    planes[2].y = float64_chs(u);
    planes[2].constant = float64_add(line->normal, fh, &fs);
    planes[3].x = float64_chs(v);
    planes[3].y = u;
    planes[3].constant = float64_sub(line->normal, fh, &fs);
    polygon[0] = (CrimeLinePoint) { float64_chs(float64_half),
                                   float64_chs(float64_half) };
    polygon[1] = (CrimeLinePoint) { float64_half, float64_chs(float64_half) };
    polygon[2] = (CrimeLinePoint) { float64_half, float64_half };
    polygon[3] = (CrimeLinePoint) { float64_chs(float64_half), float64_half };
    for (unsigned plane = 0; plane < 4 && count; plane++) {
        unsigned n = 0;
        CrimeLinePoint previous = polygon[count - 1];
        float64 fp = crime_line_evaluate(&planes[plane], previous, &fs);
        bool previous_inside = float64_le(float64_zero, fp, &fs);

        if (planes[plane].skip) {
            continue;
        }
        for (unsigned i = 0; i < count; i++) {
            CrimeLinePoint current = polygon[i];
            float64 fc = crime_line_evaluate(&planes[plane], current, &fs);
            bool inside = float64_le(float64_zero, fc, &fs);

            if (previous_inside != inside) {
                float64 t = float64_div(fp, float64_sub(fp, fc, &fs), &fs);
                float64 dx = float64_sub(current.x, previous.x, &fs);
                float64 dy = float64_sub(current.y, previous.y, &fs);

                if (n >= ARRAY_SIZE(next)) {
                    return false;
                }
                next[n++] = (CrimeLinePoint) {
                    float64_add(previous.x, float64_mul(t, dx, &fs), &fs),
                    float64_add(previous.y, float64_mul(t, dy, &fs), &fs),
                };
            }
            if (inside) {
                if (n >= ARRAY_SIZE(next)) {
                    return false;
                }
                next[n++] = current;
            }
            previous = current;
            fp = fc;
            previous_inside = inside;
        }
        memcpy(polygon, next, n * sizeof(*polygon));
        count = n;
    }
    for (unsigned i = 1; i + 1 < count; i++) {
        float64 ax = float64_sub(polygon[i].x, polygon[0].x, &fs);
        float64 ay = float64_sub(polygon[i].y, polygon[0].y, &fs);
        float64 bx = float64_sub(polygon[i + 1].x, polygon[0].x, &fs);
        float64 by = float64_sub(polygon[i + 1].y, polygon[0].y, &fs);
        float64 cross = float64_sub(float64_mul(ax, by, &fs),
                                    float64_mul(ay, bx, &fs), &fs);

        total = float64_add(total, float64_abs(cross), &fs);
    }
    total = float64_div(total, int64_to_float64(2, &fs), &fs);
    if (float64_is_any_nan(total) || float64_is_infinity(total) ||
        (get_float_exception_flags(&fs) &
         (float_flag_invalid | float_flag_divbyzero | float_flag_overflow))) {
        return false;
    }
    *area = float64_lt(total, float64_zero, &fs) ? float64_zero :
            float64_lt(float64_one, total, &fs) ? float64_one : total;
    return true;
}

/* Coefficients of 1, epsilon and epsilon squared, with positive denominator. */
typedef struct CrimeLineInterval {
    int64_t n[3], d;
} CrimeLineInterval;

static int crime_line_interval_compare(const CrimeLineInterval *a,
                                         const CrimeLineInterval *b)
{
    for (unsigned i = 0; i < 3; i++) {
        int64_t difference = a->n[i] * b->d - b->n[i] * a->d;

        if (difference) {
            return difference > 0 ? 1 : -1;
        }
    }
    return 0;
}

static bool crime_gl_line_diamond(const CrimeGLLine *line, int x, int y)
{
    CrimeLineInterval lower = { .n = { 0, 0, 0 }, .d = 1 };
    CrimeLineInterval upper = { .n = { 1, 0, 0 }, .d = 1 };
    int64_t ax = line->vertex[0][0] - 64 * (int64_t)x;
    int64_t ay = 64 * (int64_t)y - line->vertex[0][1];
    int64_t bx = line->vertex[1][0] - 64 * (int64_t)x;
    int64_t by = 64 * (int64_t)y - line->vertex[1][1];
    bool final_inside = true;

    if (line->xmajor) {
        ay -= 32 * (line->width - 1);
        by -= 32 * (line->width - 1);
    } else {
        ax -= 32 * (line->width - 1);
        bx -= 32 * (line->width - 1);
    }
    for (unsigned plane = 0; plane < 4; plane++) {
        int64_t sx = plane & 1 ? 1 : -1;
        int64_t sy = plane & 2 ? 1 : -1;
        int64_t da = sx * ax + sy * ay;
        int64_t db = sx * bx + sy * by;
        CrimeLineInterval bound = { .n = { 32 - da, 64 * sx, 64 * sy },
                                    .d = db - da };
        bool end_positive = 32 - db > 0 ||
                            (32 - db == 0 && sx > 0);

        final_inside &= end_positive;
        if (!bound.d) {
            if (!(32 - da > 0 || (32 - da == 0 && sx > 0))) {
                return false;
            }
            continue;
        }
        if (bound.d < 0) {
            bound.d = -bound.d;
            for (unsigned i = 0; i < 3; i++) {
                bound.n[i] = -bound.n[i];
            }
            if (crime_line_interval_compare(&lower, &bound) < 0) {
                lower = bound;
            }
        } else if (crime_line_interval_compare(&upper, &bound) > 0) {
            upper = bound;
        }
    }
    return !final_inside && crime_line_interval_compare(&lower, &upper) < 0;
}

static bool crime_render_triangle(const uint64_t *p, CrimeRenderGeometry *g)
{
    uint32_t primitive = p[0x60 / 4];
    int64_t area, xmin, xmax, ymin, ymax;

    if (primitive & 0x00f80000) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned axis = 0; axis < 2; axis++) {
            uint64_t raw = p[(0x80 + 8 * i + 4 * axis) / 4];
            int16_t origin = p[0x58 / 4] >> (axis ? 0 : 16);
            int64_t field;

            if (!crime_render_gl_field(raw, &field)) {
                return false;
            }
            g->vertex[i][axis] = field + (int64_t)origin * 64;
        }
    }
    /* Loaded starts refer to the original traversal-selected integer sample. */
    for (unsigned axis = 0; axis < 2; axis++) {
        int64_t selected = g->vertex[primitive & 0x20000 ? 0 : 2][axis];

        g->shade_anchor[axis] = selected / 64 - (selected % 64 < 0);
    }
    /* The bounded six-bit transport requires 64-bit edge products. */
    area = crime_render_edge(g->vertex[0], g->vertex[1],
                              g->vertex[2][0], g->vertex[2][1]);
    if (area < 0) {
        for (unsigned axis = 0; axis < 2; axis++) {
            int64_t v = g->vertex[1][axis];

            g->vertex[1][axis] = g->vertex[2][axis];
            g->vertex[2][axis] = v;
        }
    }
    xmin = MAX(MIN(g->vertex[0][0],
                   MIN(g->vertex[1][0], g->vertex[2][0])), 0);
    ymin = MAX(MIN(g->vertex[0][1],
                   MIN(g->vertex[1][1], g->vertex[2][1])), 0);
    xmax = MIN(MAX(g->vertex[0][0],
                   MAX(g->vertex[1][0], g->vertex[2][0])), 2047 * 64);
    ymax = MIN(MAX(g->vertex[0][1],
                   MAX(g->vertex[1][1], g->vertex[2][1])), 2047 * 64);
    if (!area || xmin > xmax || ymin > ymax) {
        g->samples = g->capacity = 0;
        return true;
    }
    /* Clipping before division leaves nonnegative signed floor/ceil inputs. */
    xmin = DIV_ROUND_UP(xmin, 64);
    ymin = DIV_ROUND_UP(ymin, 64);
    xmax /= 64;
    ymax /= 64;
    if (xmin > xmax || ymin > ymax) {
        g->samples = g->capacity = 0;
        return true;
    }
    g->dx = primitive & 0x10000 ? -1 : 1;
    g->dy = primitive & 0x20000 ? 1 : -1;
    g->x0 = g->dx > 0 ? xmin : xmax;
    g->y0 = g->dy > 0 ? ymin : ymax;
    g->width = xmax - xmin + 1;
    g->samples = g->capacity = g->width * (ymax - ymin + 1);
    g->thickness = 1;
    return true;
}

static bool crime_render_triangle_inside(const CrimeRenderGeometry *g,
                                          int x, int y)
{
    for (unsigned i = 0; i < 3; i++) {
        const int64_t *a = g->vertex[i], *b = g->vertex[(i + 1) % 3];
        int64_t dx = b[0] - a[0], dy = b[1] - a[1];
        int64_t edge = crime_render_edge(a, b, (int64_t)x * 64,
                                          (int64_t)y * 64);

        if (edge < 0 || (!edge && !(dy < 0 || (!dy && dx > 0)))) {
            return false;
        }
    }
    return true;
}

static bool crime_render_gl_rectangle(const uint64_t *p, CrimeRenderGeometry *g)
{
    uint32_t primitive = p[0x60 / 4];
    int xy[2][2];
    int xmin, xmax, ymin, ymax;

    if ((primitive & ~0x30000U) != 0x03000000) {
        return false;
    }
    for (unsigned v = 0; v < 2; v++) {
        for (unsigned axis = 0; axis < 2; axis++) {
            int64_t field;
            int16_t origin = p[0x58 / 4] >> (axis ? 0 : 16);

            if (!crime_render_gl_field(p[(0x80 + 8 * v + 4 * axis) / 4],
                                       &field) || (field & 63)) {
                return false;
            }
            xy[v][axis] = field / 64 + origin;
        }
    }
    g->dx = primitive & 0x10000 ? -1 : 1;
    g->dy = primitive & 0x20000 ? 1 : -1;
    if (g->dx * (xy[1][0] - xy[0][0]) < 0 ||
        g->dy * (xy[1][1] - xy[0][1]) < 0) {
        return false;
    }
    xmin = MAX(MIN(xy[0][0], xy[1][0]), 0);
    ymin = MAX(MIN(xy[0][1], xy[1][1]), 0);
    xmax = MIN(MAX(xy[0][0], xy[1][0]), 2047);
    ymax = MIN(MAX(xy[0][1], xy[1][1]), 2047);
    if (p[0x18 / 4] & 0x40000) {
        if (xy[0][1] != xy[1][1]) {
            return false;
        }
        /* Stipple advances from the programmed start, before clipping. */
        g->x0 = xy[0][0];
        g->y0 = xy[0][1];
        g->width = abs(xy[1][0] - xy[0][0]) + 1;
        g->samples = g->width;
        g->capacity = ymin <= ymax && xmin <= xmax ? xmax - xmin + 1 : 0;
        return true;
    }
    if (xmin > xmax || ymin > ymax) {
        g->samples = g->capacity = 0;
        return true;
    }
    g->x0 = g->dx > 0 ? xmin : xmax;
    g->y0 = g->dy > 0 ? ymin : ymax;
    g->x1 = xy[1][0];
    g->y1 = xy[1][1];
    g->width = xmax - xmin + 1;
    g->samples = g->capacity = g->width * (ymax - ymin + 1);
    return true;
}

static bool crime_render_gl_point(const uint64_t *p, CrimeRenderGeometry *g)
{
    int sample[2];

    if (p[0x60 / 4] & ~0x70000U) {
        return false;
    }
    for (unsigned axis = 0; axis < 2; axis++) {
        int64_t field, coordinate;
        int16_t origin = p[0x58 / 4] >> (axis ? 0 : 16);

        if (!crime_render_gl_field(p[(0x80 + 4 * axis) / 4], &field)) {
            return false;
        }
        coordinate = field + 64 * (int64_t)origin;
        /* Framebuffer-Y half ties select the smaller sample index. */
        sample[axis] = axis ? -crime_line_floor(32 - coordinate, 64) :
                              crime_line_floor(coordinate + 32, 64);
    }
    g->x0 = sample[0];
    g->y0 = sample[1];
    g->samples = g->capacity = sample[0] >= 0 && sample[0] < 2048 &&
                               sample[1] >= 0 && sample[1] < 2048;
    return true;
}

static bool crime_render_geometry(const uint64_t *p, CrimeRenderGeometry *g)
{
    uint32_t primitive = p[0x60 / 4];

    g->opcode = primitive >> 24;
    if (!g->opcode && (p[0x18 / 4] & 0x400000)) {
        return crime_render_gl_point(p, g);
    }
    if (g->opcode == 1 && (p[0x18 / 4] & 0x400000)) {
        return !(primitive & 0x00f80000);
    }
    if (g->opcode == 2) {
        return crime_render_triangle(p, g);
    }
    if (g->opcode == 3 && (p[0x18 / 4] & 0x400000)) {
        return crime_render_gl_rectangle(p, g);
    }
    g->x0 = p[0x70 / 4] >> 16;
    g->y0 = p[0x70 / 4] & 0xffff;
    g->x1 = p[0x74 / 4] >> 16;
    g->y1 = p[0x74 / 4] & 0xffff;
    if (g->opcode <= 1) {
        if (primitive & 0x00f80000) {
            return false;
        }
        g->samples = g->capacity = g->thickness = 1;
        if (!g->opcode) {
            return true;
        }
    } else if ((primitive & ~0x30000U) != 0x3000000 ||
               g->x0 >= 2048 || g->y0 >= 2048 ||
               g->x1 >= 2048 || g->y1 >= 2048) {
        return false;
    }
    if (g->opcode == 1) {
        unsigned ax = abs(g->x1 - g->x0), ay = abs(g->y1 - g->y0);

        g->steps = MAX(ax, ay);
        g->samples = g->steps + 1 - !!(primitive & 0x40000);
        g->thickness = MAX(1, DIV_ROUND_UP(primitive & 0xffff, 32));
        g->lower = -((int)(g->thickness - 1) / 2);
        g->xmajor = ax >= ay;
        g->capacity = MIN(g->samples, 2048U) * g->thickness;
        if (!g->steps) {
            g->capacity *= g->thickness;
        }
    } else {
        g->dx = primitive & 0x10000 ? -1 : 1;
        g->dy = primitive & 0x20000 ? 1 : -1;
        if ((g->x1 - g->x0) * g->dx < 0 ||
            (g->y1 - g->y0) * g->dy < 0) {
            return false;
        }
        g->width = abs(g->x1 - g->x0) + 1;
        g->samples = g->capacity = g->width * (abs(g->y1 - g->y0) + 1);
        g->thickness = 1;
    }
    return true;
}

static void crime_render_sample(const CrimeRenderGeometry *g, unsigned i,
                                 int *x, int *y)
{
    if (g->opcode >= 2) {
        *x = g->x0 + g->dx * (int)(i % g->width);
        *y = g->y0 + g->dy * (int)(i / g->width);
    } else if (g->opcode == 1 && g->steps) {
        /* Selected floor interpolation; skip-last keeps this denominator. */
        *x = ((int64_t)(g->steps - i) * g->x0 + (int64_t)i * g->x1) /
             g->steps;
        *y = ((int64_t)(g->steps - i) * g->y0 + (int64_t)i * g->y1) /
             g->steps;
    } else {
        *x = g->x0;
        *y = g->y0;
    }
}

static bool crime_render_alpha_pass(uint32_t color, uint32_t test)
{
    uint8_t alpha = color, reference = test;

    switch (test >> 8) {
    case 0: return false;
    case 1: return alpha < reference;
    case 2: return alpha == reference;
    case 3: return alpha <= reference;
    case 4: return alpha > reference;
    case 5: return alpha != reference;
    case 6: return alpha >= reference;
    case 7: return true;
    default:
        g_assert_not_reached();
    }
}

typedef struct CrimeColorMode {
    unsigned bank, bytes, format;
    uint32_t mask;
} CrimeColorMode;

typedef struct CrimeColorPixel {
    uint64_t source[4], destination[4];
    uint32_t mask, result;
} CrimeColorPixel;

typedef struct CrimeColorByte {
    uint64_t identity;
    bool enabled;
} CrimeColorByte;

static bool crime_color_mode(uint32_t value, CrimeColorMode *mode)
{
    unsigned depth = (value >> 2) & 3;
    unsigned type = (value >> 4) & 15;

    mode->bank = (value >> 10) & 7;
    if ((value & 3) || (mode->bank >= 3 && mode->bank != 4 &&
                              mode->bank != 5) ||
        ((value >> 8) & 3) != depth) {
        return false;
    }
    if (type == 1 && depth == 0) {
        mode->format = 0;
    } else if (type == 2 && depth == 1) {
        mode->format = 1;
    } else if (type == 2 && depth == 2) {
        mode->format = 2;
    } else if (type == 1 && depth == 2) {
        mode->format = 3;
    } else {
        return false;
    }
    mode->bytes = 1U << depth;
    mode->mask = mode->format == 3 ? 0xffffff00U :
                 UINT32_MAX >> (32 - 8 * mode->bytes);
    return true;
}

static uint32_t crime_color_convert(uint32_t value, unsigned source,
                                     unsigned destination)
{
    unsigned r, g, b, a = 255;

    if (source == 0) {
        r = value >> 5;
        g = (value >> 2) & 7;
        b = value & 3;
        r = (r << 5) | (r << 2) | (r >> 1);
        g = (g << 5) | (g << 2) | (g >> 1);
        b *= 0x55;
    } else if (source == 1) {
        r = (value >> 10) & 31;
        g = (value >> 5) & 31;
        b = value & 31;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        a = value & 0x8000 ? 255 : 0;
    } else {
        r = value >> 24;
        g = (value >> 16) & 255;
        b = (value >> 8) & 255;
        if (source == 2) {
            a = value & 255;
        }
    }
    switch (destination) {
    case 0:
        return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
    case 1:
        return ((a >> 7) << 15) | ((r >> 3) << 10) |
               ((g >> 3) << 5) | (b >> 3);
    default:
        return (r << 24) | (g << 16) | (b << 8) |
               (destination == 2 ? a : 0);
    }
}

static int crime_color_compare(const void *a, const void *b)
{
    const CrimeColorByte *x = a, *y = b;

    return (x->identity > y->identity) - (x->identity < y->identity);
}

static bool crime_color_envelope(uint64_t first, uint64_t pitch,
                                  unsigned width, unsigned height,
                                  unsigned bytes)
{
    uint64_t last = first + (height - 1) * pitch + (uint64_t)width * bytes - 1;

    return last <= UINT32_MAX && (last >> 12) - (first >> 12) < 32;
}

static MemTxResult crime_render_color(SGICrimeState *s, hwaddr offset,
                                       const uint64_t *p,
                                       const CrimeRenderGeometry *geometry)
{
    CrimeColorMode src, dst;
    CrimeRenderClip scissor = crime_render_clip(p[0x48 / 4]), screen[5];
    uint32_t draw = p[0x18 / 4], clip = p[0x10 / 4];
    unsigned width = geometry->width, height = geometry->samples / width;
    unsigned screen_count = 0, count = 0, operation;
    unsigned sx = p[0xa0 / 4] >> 16, sy = p[0xa0 / 4] & 0xffff;
    uint64_t source_pitch = 0, destination_pitch;
    g_autofree CrimeColorPixel *pixels = NULL;
    g_autoptr(GArray) sources =
        g_array_new(false, false, sizeof(CrimeColorByte));
    g_autoptr(GArray) destinations =
        g_array_new(false, false, sizeof(CrimeColorByte));

    if (!crime_color_mode(p[0], &src) || !crime_color_mode(p[8 / 4], &dst) ||
        (src.bank < 4 && dst.bank < 4) || (draw & ~0x3002f8U) ||
        geometry->opcode != 3 || (clip & ~0x3ffU) ||
        ((clip & 31) & ~((clip >> 5) & 31))) {
        return crime_transfer_fail(offset, "unsupported color transfer");
    }
    operation = draw & 0x200 ? p[0x1b0 / 4] : 3;
    if (operation > 15) {
        return crime_transfer_fail(offset,
                                       "unsupported color raster operation");
    }
    for (unsigned n = 0; n < 5; n++) {
        if (clip & (1U << (5 + n))) {
            screen[screen_count] = crime_render_clip(p[(0x20 + 8 * n) / 4]);
            if (screen[screen_count].xmin > screen[screen_count].xmax ||
                screen[screen_count].ymin > screen[screen_count].ymax) {
                return crime_transfer_fail(offset, "invalid color screen mask");
            }
            screen_count++;
        }
    }
    if (screen_count > 1 || ((draw & 0x100000) &&
        (scissor.xmin > scissor.xmax || scissor.ymin > scissor.ymax))) {
        return crime_transfer_fail(offset, "unsupported color clipping");
    }
    if (src.bank >= 4) {
        if (p[0xa8 / 4] != src.bytes || p[0xac / 4] < src.bytes ||
            p[0xac / 4] >= 0x10000000) {
            return crime_transfer_fail(offset, "unsupported color source step");
        }
        source_pitch = (uint64_t)(width - 1) * src.bytes + p[0xac / 4];
        if (!crime_color_envelope(p[0xa0 / 4], source_pitch, width, height,
                                  src.bytes)) {
            return crime_transfer_fail(offset, "invalid color source envelope");
        }
    } else if (sx >= 2048 || sy >= 2048 || sx + width > 2048 ||
               sy + height > 2048 || p[0xa8 / 4] != 1 || p[0xac / 4] != 1) {
        return crime_transfer_fail(offset, "invalid color tiled source");
    }
    destination_pitch = (uint64_t)width * dst.bytes;
    if (dst.bank >= 4 && (p[0xb4 / 4] != dst.bytes ||
        !crime_color_envelope(p[0xb0 / 4], destination_pitch, width, height,
                              dst.bytes))) {
        return crime_transfer_fail(offset,
                                       "invalid color destination envelope");
    }
    pixels = g_new(CrimeColorPixel, geometry->samples);
    for (unsigned i = 0; i < geometry->samples; i++) {
        CrimeColorPixel *pixel = &pixels[count];
        uint64_t source = p[0xa0 / 4] + (i / width) * source_pitch +
                          (uint64_t)(i % width) * src.bytes;
        uint64_t destination = p[0xb0 / 4] + (uint64_t)i * dst.bytes;
        uint32_t mask = dst.mask & (draw & 0x80 ? p[0x1b8 / 4] : UINT32_MAX);
        int x, y;
        bool screen_pass = !screen_count;

        crime_render_sample(geometry, i, &x, &y);
        for (unsigned n = 0; n < screen_count; n++) {
            screen_pass |= crime_render_inside(&screen[n], x, y) ==
                           !!(clip & 31);
        }
        if (!screen_pass || ((draw & 0x100000) &&
            !crime_render_inside(&scissor, x, y))) {
            continue;
        }
        for (unsigned b = 0; b < dst.bytes; b++) {
            unsigned lane = (dst.bank >= 4 ? destination + b :
                             (uint64_t)x * dst.bytes + b) & 3;
            if (!(draw & (1U << (6 - lane)))) {
                mask &= ~(0xffU << (8 * (dst.bytes - 1 - b)));
            }
        }
        if (!mask) {
            continue;
        }
        pixel->mask = mask;
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
            const CrimeColorMode *mode = endpoint ? &dst : &src;
            for (unsigned b = 0; b < mode->bytes; b++) {
                CrimeColorByte byte = { 0 };
                uint64_t *physical = endpoint ? &pixel->destination[b] :
                                                &pixel->source[b];
                CrimeRenderTranslation translation;
                if (mode->bank >= 4) {
                    translation = crime_render_linear(s, mode->bank,
                        (endpoint ? destination : source) + b,
                        physical, &byte.identity);
                } else {
                    unsigned tx = endpoint ? x : sx + x -
                        MIN(geometry->x0, geometry->x1);
                    unsigned ty = endpoint ? y : sy + y -
                        MIN(geometry->y0, geometry->y1);
                    translation = crime_framebuffer_translate(s, mode->bank,
                        (uint64_t)tx * mode->bytes + b, ty, 1,
                        physical, &byte.identity);
                }
                if (translation != CRIME_RENDER_MAPPED) {
                    return crime_transfer_fail(offset, "invalid color mapping");
                }
                byte.enabled = endpoint &&
                    ((mask >> (8 * (mode->bytes - 1 - b))) & 255);
                g_array_append_val(endpoint ? destinations : sources, byte);
            }
        }
        count++;
    }
    g_array_sort(destinations, crime_color_compare);
    for (unsigned i = 1; i < destinations->len; i++) {
        CrimeColorByte *a = &g_array_index(destinations, CrimeColorByte, i - 1);
        CrimeColorByte *b = &g_array_index(destinations, CrimeColorByte, i);
        if (a->identity == b->identity && a->enabled && b->enabled) {
            return crime_transfer_fail(offset, "color destination aliases");
        }
    }
    for (unsigned i = 0; i < sources->len; i++) {
        if (bsearch(&g_array_index(sources, CrimeColorByte, i),
                    destinations->data, destinations->len,
                    sizeof(CrimeColorByte), crime_color_compare)) {
            return crime_transfer_fail(offset,
                                       "color source overlaps destination");
        }
    }
    /* The BQL keeps ordinary RAM mappings stable through staging and commit. */
    for (unsigned i = 0; i < count; i++) {
        CrimeColorPixel *pixel = &pixels[i];
        uint8_t source_data[4], destination_data[4];
        uint32_t source, destination;
        for (unsigned b = 0; b < src.bytes; b++) {
            if (address_space_read(&address_space_memory, pixel->source[b],
                MEMTXATTRS_UNSPECIFIED, &source_data[b], 1) != MEMTX_OK) {
                return crime_transfer_fail(offset,
                                       "validated color source failed");
            }
        }
        for (unsigned b = 0; b < dst.bytes; b++) {
            if (address_space_read(&address_space_memory, pixel->destination[b],
                MEMTXATTRS_UNSPECIFIED, &destination_data[b], 1) != MEMTX_OK) {
                return crime_transfer_fail(offset,
                                           "validated color destination failed");
            }
        }
        source = crime_color_convert(crime_render_load_pixel(source_data,
                                     src.bytes), src.format, dst.format);
        destination = crime_render_load_pixel(destination_data, dst.bytes);
        pixel->result = (destination & ~pixel->mask) |
                       (crime_render_rop(operation, source, destination) &
                        pixel->mask);
    }
    for (unsigned i = 0; i < count; i++) {
        CrimeColorPixel *pixel = &pixels[i];
        uint8_t data[4];
        crime_render_store_pixel(data, dst.bytes, pixel->result);
        for (unsigned b = 0; b < dst.bytes; b++) {
            if ((pixel->mask >> (8 * (dst.bytes - 1 - b))) & 255) {
                if (address_space_write(&address_space_memory,
                    pixel->destination[b], MEMTXATTRS_UNSPECIFIED,
                    &data[b], 1) != MEMTX_OK) {
                    return crime_transfer_fail(offset,
                                               "validated color write failed");
                }
            }
        }
    }
    return MEMTX_OK;
}

typedef struct CrimeTriangleOutput {
    unsigned bank, bytes, format, shift;
    uint32_t mask;
} CrimeTriangleOutput;

static bool crime_triangle_output(uint32_t raw, CrimeTriangleOutput *o)
{
    unsigned type = (raw >> 4) & 15;
    unsigned depth = (raw >> 2) & 3;
    unsigned storage = (raw >> 8) & 3;
    bool split = raw & 2;

    o->bank = (raw >> 10) & 7;
    o->shift = 0;
    if (o->bank >= 3) {
        return false;
    }
    if (depth == 2 && storage == 2 && !split &&
        (type == 1 || type == 2)) {
        o->format = type == 2 ? 2 : 3;
        o->mask = type == 2 ? UINT32_MAX : 0xffffff00U;
    } else if (type == 2 && depth == 1 &&
               ((storage == 1 && !split) || storage == 2)) {
        o->format = 1;
        o->shift = storage == 2 && split && (raw & 1) ? 16 : 0;
        o->mask = 0xffffU << o->shift;
    } else {
        return false;
    }
    o->bytes = 1U << storage;
    return true;
}

/* Selected model precision, preserving exact five-bit destination values. */
#define CRIME_COLOR_ONE INT64_C(7905)
#define CRIME_COLOR_DEN (CRIME_COLOR_ONE * CRIME_COLOR_ONE)

static int64_t crime_blend_factor(unsigned selector, unsigned channel,
                                   const int64_t *source,
                                   const int64_t *destination,
                                   const int64_t *constant, bool source_factor)
{
    switch (selector) {
    case 0:
        return 0;
    case 1:
        return CRIME_COLOR_ONE;
    case 2:
        return source_factor ? destination[channel] : source[channel];
    case 3:
        return CRIME_COLOR_ONE -
               (source_factor ? destination[channel] : source[channel]);
    case 4:
        return source[3];
    case 5:
        return CRIME_COLOR_ONE - source[3];
    case 6:
        return destination[3];
    case 7:
        return CRIME_COLOR_ONE - destination[3];
    case 8:
        return constant[channel];
    case 9:
        return CRIME_COLOR_ONE - constant[channel];
    case 10:
        return constant[3];
    case 11:
        return CRIME_COLOR_ONE - constant[3];
    case 12:
        return channel == 3 ? CRIME_COLOR_ONE :
               MIN(source[3], CRIME_COLOR_ONE - destination[3]);
    default:
        g_assert_not_reached();
    }
}

static uint32_t crime_color_pack(const CrimeTriangleOutput *o,
                                  const uint64_t *p, const int64_t src[4],
                                  uint32_t old, unsigned x, unsigned y)
{
    static const unsigned rank[4][4] = {
        { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
        { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
    };
    int64_t dst[4], constant[4];
    uint32_t draw = p[0x18 / 4], func = p[0x1a8 / 4];
    uint32_t selected = old >> o->shift, packed = 0;
    unsigned operation = (func >> 8) & 15;

    for (unsigned k = 0; k < 4; k++) {
        unsigned shift = 24 - 8 * k;

        constant[k] = ((p[0x1a0 / 4] >> shift) & 255) * 31;
        if (o->format == 1) {
            dst[k] = k == 3 ? ((selected >> 15) & 1) * CRIME_COLOR_ONE :
                     ((selected >> (10 - 5 * k)) & 31) * 255;
        } else {
            dst[k] = o->format == 3 && k == 3 ? CRIME_COLOR_ONE :
                     ((selected >> shift) & 255) * 31;
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        int64_t n = src[k] * CRIME_COLOR_ONE, q;

        if (draw & 0x400) {
            int64_t a = src[k] * crime_blend_factor((func >> 4) & 15,
                k, src, dst, constant, true);
            int64_t b = dst[k] * crime_blend_factor(func & 15,
                k, src, dst, constant, false);

            switch (operation) {
            case 0:
                n = a + b;
                break;
            case 1:
                n = MIN(src[k], dst[k]) * CRIME_COLOR_ONE;
                break;
            case 2:
                n = MAX(src[k], dst[k]) * CRIME_COLOR_ONE;
                break;
            case 3:
                n = a - b;
                break;
            case 4:
                n = b - a;
                break;
            default:
                g_assert_not_reached();
            }
            n = CLAMP(n, 0, CRIME_COLOR_DEN);
        }
        if (o->format == 1 && k < 3) {
            int64_t scaled = 31 * n;

            q = scaled / CRIME_COLOR_DEN;
            if ((draw & 0x100) &&
                (scaled % CRIME_COLOR_DEN) * 32 >
                (2 * rank[y & 3][x & 3] + 1) * CRIME_COLOR_DEN) {
                q++;
            }
            packed |= MIN(q, 31) << (10 - 5 * k);
        } else if (o->format == 1) {
            packed |= MIN(1, 2 * n / CRIME_COLOR_DEN) << 15;
        } else {
            q = (255 * n + CRIME_COLOR_DEN / 2) / CRIME_COLOR_DEN;
            packed |= q << (24 - 8 * k);
        }
    }
    return packed << o->shift;
}

static uint32_t crime_triangle_pack(const CrimeTriangleOutput *o,
                                     const uint64_t *p, uint32_t source,
                                     uint32_t old, unsigned x, unsigned y)
{
    int64_t normalized[4];

    for (unsigned k = 0; k < 4; k++) {
        normalized[k] = ((source >> (24 - 8 * k)) & 255) * 31;
    }
    return crime_color_pack(o, p, normalized, old, x, y);
}

typedef struct CrimeGLTransferPixel {
    uint64_t source[4], destination[4];
    uint32_t mask;
    uint16_t x, y;
} CrimeGLTransferPixel;

static bool crime_gl_transfer_mode(uint32_t raw, CrimeTriangleOutput *o,
                                    bool source)
{
    unsigned bank = (raw >> 10) & 7;

    if (bank == 4 || bank == 5) {
        if (((raw >> 4) & 15) != 2 || ((raw >> 8) & 3) != 2 ||
            ((raw >> 2) & 3) != 2 || (raw & 2)) {
            return false;
        }
        *o = (CrimeTriangleOutput) {
            .bank = bank, .bytes = 4, .format = 2, .mask = UINT32_MAX,
        };
        return true;
    }
    return crime_triangle_output(raw, o) && (!source || o->format != 3);
}

static bool crime_gl_transfer_geometry(const uint64_t *p,
                                        CrimeRenderGeometry *g)
{
    uint32_t primitive = p[0x60 / 4];
    int *points[2][2] = { {&g->x0, &g->y0}, {&g->x1, &g->y1} };
    uint64_t samples;

    for (unsigned v = 0; v < 2; v++) {
        for (unsigned a = 0; a < 2; a++) {
            int64_t field;
            int16_t origin = p[0x58 / 4] >> (a ? 0 : 16);

            if (!crime_render_gl_field(p[(0x80 + 8 * v + 4 * a) / 4],
                                       &field) || (field & 63)) {
                return false;
            }
            *points[v][a] = field / 64 + origin;
        }
    }
    g->dx = primitive & 0x10000 ? -1 : 1;
    g->dy = primitive & 0x20000 ? 1 : -1;
    if ((g->x1 - g->x0) * g->dx < 0 ||
        (g->y1 - g->y0) * g->dy < 0) {
        return false;
    }
    g->width = abs(g->x1 - g->x0) + 1;
    samples = (uint64_t)g->width * (abs(g->y1 - g->y0) + 1);
    if (samples > 4194304) {
        return false;
    }
    g->samples = samples;
    return true;
}

static void crime_gl_transfer_sift(uint64_t *ids, unsigned root, unsigned count)
{
    uint64_t value = ids[root];

    while (root < count / 2) {
        unsigned child = 2 * root + 1;

        if (child + 1 < count && ids[child] < ids[child + 1]) {
            child++;
        }
        if (value >= ids[child]) {
            break;
        }
        ids[root] = ids[child];
        root = child;
    }
    ids[root] = value;
}

/* In-place heapsort keeps alias metadata inside the command's memory bound. */
static void crime_gl_transfer_sort(uint64_t *ids, unsigned count)
{
    for (unsigned root = count / 2; root > 0; root--) {
        crime_gl_transfer_sift(ids, root - 1, count);
    }
    for (unsigned end = count; end > 1; end--) {
        uint64_t value = ids[0];

        ids[0] = ids[end - 1];
        ids[end - 1] = value;
        crime_gl_transfer_sift(ids, 0, end - 1);
    }
}

static void crime_gl_transfer_read(const uint64_t *physical, unsigned bytes,
                                    uint8_t *data)
{
    for (unsigned b = 0; b < bytes; b++) {
        MemTxResult result = address_space_read(&address_space_memory,
            physical[b], MEMTXATTRS_UNSPECIFIED, &data[b], 1);

        /* Preflight and the BQL guarantee ordinary RAM at each byte. */
        g_assert(result == MEMTX_OK);
    }
}

static MemTxResult crime_render_gl_transfer(SGICrimeState *s, hwaddr offset,
                                             const uint64_t *p,
                                             unsigned index, unsigned words)
{
    CrimeTriangleOutput src, dst;
    CrimeRenderGeometry g = { 0 };
    CrimeRenderClip scissor = crime_render_clip(p[0x48 / 4]), screen = { 0 };
    uint32_t draw = p[0x18 / 4], clip = p[0x10 / 4];
    uint32_t primitive = p[0x60 / 4], ignored = primitive & 0x00f80000U;
    unsigned height, kx = 1, ky = 1, count = 0, identities = 0, operation;
    int sx = 0, sy = 0, hx = 1, hy = 1;
    int ox = (int16_t)(p[0x58 / 4] >> 16), oy = (int16_t)p[0x58 / 4];
    uint64_t source_pitch = 0, destination_pitch = 0, metadata;
    bool upload, download, has_screen = false, inside = clip & 31;
    g_autofree CrimeGLTransferPixel *pixels = NULL;
    g_autofree uint64_t *destination_ids = NULL;

    if (!crime_gl_transfer_mode(p[0], &src, true) ||
        !crime_gl_transfer_mode(p[8 / 4], &dst, false) ||
        (src.bank >= 4 && dst.bank >= 4) || (draw & ~0x7107f8U) ||
        !crime_gl_transfer_geometry(p, &g) || (clip & ~0x3ffU) ||
        ((clip & 31) & ~((clip >> 5) & 31)) ||
        ctpop32((clip >> 5) & 31) > 1) {
        return crime_transfer_fail(offset, "unsupported GL pixel transfer");
    }
    upload = src.bank >= 4;
    download = dst.bank >= 4;
    height = g.samples / g.width;
    if (words && ((index == 0x190 / 4 || index == 0x194 / 4) ||
        ((index == 0xb0 / 4 || index == 0xb4 / 4) && !download))) {
        return crime_transfer_fail(offset, "unsupported GL transfer START");
    }
    operation = draw & 0x200 ? p[0x1b0 / 4] : 3;
    if (operation > 15 || ((draw & 0x400) &&
        ((p[0x1a8 / 4] & ~0xfffULL) ||
         ((p[0x1a8 / 4] >> 8) & 15) > 4 ||
         ((p[0x1a8 / 4] >> 4) & 15) > 12 ||
         (p[0x1a8 / 4] & 15) > 11))) {
        return crime_transfer_fail(offset,
                                   "unsupported GL transfer color state");
    }
    for (unsigned n = 0; n < 5; n++) {
        if (clip & (1U << (5 + n))) {
            screen = crime_render_clip(p[(0x20 + 8 * n) / 4]);
            has_screen = true;
        }
    }
    if ((has_screen && (screen.xmin > screen.xmax ||
                        screen.ymin > screen.ymax)) ||
        ((draw & 0x100000) && (scissor.xmin > scissor.xmax ||
                              scissor.ymin > scissor.ymax))) {
        return crime_transfer_fail(offset, "invalid GL transfer clipping");
    }
    if (upload) {
        uint32_t xstep = p[0xa8 / 4], ystep = p[0xac / 4];
        unsigned columns, rows, advance = ystep & 0x1ffff;

        if ((xstep & 0x0ffe0000U) || (ystep & 0x0ffe0000U) ||
            (xstep & 0x1ffff) != 4 || advance < 4) {
            return crime_transfer_fail(offset, "unsupported GL upload steps");
        }
        kx = (xstep >> 28) + 1;
        ky = (ystep >> 28) + 1;
        columns = DIV_ROUND_UP(g.width, kx);
        rows = DIV_ROUND_UP(height, ky);
        source_pitch = (uint64_t)(columns - 1) * 4 + advance;
        if (!crime_color_envelope(p[0xa0 / 4], source_pitch,
                                  columns, rows, 4)) {
            return crime_transfer_fail(offset, "invalid GL upload envelope");
        }
    } else {
        int last_x, last_y;
        uint32_t xstep = p[0xa8 / 4], ystep = p[0xac / 4];

        if ((p[0xa0 / 4] & ~0x07ff07ffULL) ||
            (xstep != 1 && (download || xstep != 0x7ff)) ||
            (ystep != 1 && (download || ystep != 0x7ff))) {
            return crime_transfer_fail(offset, "unsupported GL tiled source");
        }
        hx = xstep == 1 ? 1 : -1;
        hy = ystep == 1 ? 1 : -1;
        sx = 4096 + (int)((p[0xa0 / 4] >> 16) & 0x7ff) +
             (int16_t)(p[0x50 / 4] >> 16);
        sy = 4096 + (int)(p[0xa0 / 4] & 0x7ff) + (int16_t)p[0x50 / 4];
        last_x = sx + g.dx * hx * (int)(g.width - 1);
        last_y = sy + g.dy * hy * (int)(height - 1);
        if (MIN(sx, last_x) < 0 || MAX(sx, last_x) >= 2048 ||
            MIN(sy, last_y) < 0 || MAX(sy, last_y) >= 2048) {
            return crime_transfer_fail(offset, "invalid GL source coordinates");
        }
    }
    if (download) {
        if (p[0xb4 / 4] < 4 || p[0xb4 / 4] > 0x1ffff) {
            return crime_transfer_fail(offset,
                                   "unsupported GL download stride");
        }
        destination_pitch = (uint64_t)(g.width - 1) * 4 + p[0xb4 / 4];
        if (!crime_color_envelope(p[0xb0 / 4], destination_pitch,
                                  g.width, height, 4)) {
            return crime_transfer_fail(offset, "invalid GL download envelope");
        }
    }
    if (ignored) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: GL transfer primitive raw 0x%08x "
                      "ignores reserved bits 0x%08x\n", primitive, ignored);
    }
    metadata = (uint64_t)g.samples *
               (sizeof(*pixels) + dst.bytes * sizeof(*destination_ids));
    if (metadata > 512 * MiB) {
        return crime_transfer_fail(offset, "GL transfer plan exceeds limit");
    }
    pixels = g_try_new(CrimeGLTransferPixel, g.samples);
    destination_ids = g_try_new(uint64_t, (size_t)g.samples * dst.bytes);
    if (!pixels || !destination_ids) {
        return crime_transfer_fail(offset,
                                   "GL transfer plan allocation failed");
    }
    for (unsigned r = 0; r < height; r++) {
        int y = g.y0 + g.dy * (int)r;

        for (unsigned c = 0; c < g.width; c++) {
            int x = g.x0 + g.dx * (int)c;
            uint64_t source = p[0xa0 / 4] + (r / ky) * source_pitch +
                              (uint64_t)(c / kx) * 4;
            uint64_t destination = p[0xb0 / 4] + r * destination_pitch +
                                   (uint64_t)c * 4;
            uint32_t mask = dst.mask &
                (draw & 0x80 ? p[0x1b8 / 4] : UINT32_MAX);
            CrimeGLTransferPixel *pixel = &pixels[count];

            if (x < 0 || x >= 2048 || y < 0 || y >= 2048 ||
                (has_screen && crime_render_inside(&screen, x, y) != inside) ||
                ((draw & 0x100000) &&
                 !crime_render_inside(&scissor, x - ox, y - oy))) {
                continue;
            }
            for (unsigned b = 0; b < dst.bytes; b++) {
                unsigned lane = (download ? destination + b :
                                 (uint64_t)x * dst.bytes + b) & 3;

                if (!(draw & (1U << (6 - lane)))) {
                    mask &= ~(0xffU << (8 * (dst.bytes - 1 - b)));
                }
            }
            if (!mask) {
                continue;
            }
            pixel->mask = mask;
            pixel->x = x;
            pixel->y = y;
            for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
                const CrimeTriangleOutput *o = endpoint ? &dst : &src;
                int tx = endpoint ? x : sx + g.dx * hx * (int)c;
                int ty = endpoint ? y : sy + g.dy * hy * (int)r;

                for (unsigned b = 0; b < o->bytes; b++) {
                    uint64_t identity, *physical = endpoint ?
                        &pixel->destination[b] : &pixel->source[b];
                    CrimeRenderTranslation mapped;

                    if (o->bank >= 4) {
                        mapped = crime_render_linear(s, o->bank,
                            (endpoint ? destination : source) + b,
                            physical, &identity);
                    } else {
                        mapped = crime_framebuffer_translate(s, o->bank,
                            (uint64_t)tx * o->bytes + b, ty, 1,
                            physical, &identity);
                    }
                    if (mapped != CRIME_RENDER_MAPPED ||
                        identity > UINT64_MAX / 2) {
                        return crime_transfer_fail(offset,
                                                   "invalid GL transfer mapping");
                    }
                    if (endpoint) {
                        bool enabled =
                            (mask >> (8 * (dst.bytes - 1 - b))) & 255;

                        destination_ids[identities++] = identity * 2 + enabled;
                    }
                }
            }
            count++;
        }
    }
    crime_gl_transfer_sort(destination_ids, identities);
    for (unsigned i = 1; i < identities; i++) {
        uint64_t a = destination_ids[i - 1], b = destination_ids[i];

        if (a / 2 == b / 2 && (a & b & 1)) {
            return crime_transfer_fail(offset,
                                   "GL transfer destination aliases");
        }
    }
    if (upload || download) {
        for (unsigned i = 0; i < count; i++) {
            for (unsigned b = 0; b < src.bytes; b++) {
                CrimeTransferSpan span;
                unsigned lo = 0, hi = identities;

                if (!crime_transfer_resolve(s, pixels[i].source[b], 1, &span)) {
                    return crime_transfer_fail(offset,
                                               "invalid GL transfer source RAM");
                }
                while (lo < hi) {
                    unsigned mid = lo + (hi - lo) / 2;

                    if (destination_ids[mid] / 2 < span.offset) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo < identities && destination_ids[lo] / 2 == span.offset) {
                    return crime_transfer_fail(offset,
                                               "GL linear transfer overlaps");
                }
            }
        }
    }
    /* Stable RAM and bounded pure color arithmetic cannot fail here. */
    for (unsigned i = 0; i < count; i++) {
        CrimeGLTransferPixel *pixel = &pixels[i];
        uint8_t source_data[4], destination_data[4];
        int64_t normalized[4];
        uint32_t source, old, packed, result;

        crime_gl_transfer_read(pixel->source, src.bytes, source_data);
        crime_gl_transfer_read(pixel->destination, dst.bytes, destination_data);
        source = crime_render_load_pixel(source_data, src.bytes) >> src.shift;
        old = crime_render_load_pixel(destination_data, dst.bytes);
        for (unsigned k = 0; k < 4; k++) {
            normalized[k] = src.format == 1 ?
                (k == 3 ? ((source >> 15) & 1) * CRIME_COLOR_ONE :
                          ((source >> (10 - 5 * k)) & 31) * 255) :
                ((source >> (24 - 8 * k)) & 255) * 31;
        }
        packed = crime_color_pack(&dst, p, normalized, old, pixel->x, pixel->y);
        result = (old & ~pixel->mask) |
                 (crime_render_rop(operation, packed, old) & pixel->mask);
        crime_render_store_pixel(destination_data, dst.bytes, result);
        for (unsigned b = 0; b < dst.bytes; b++) {
            if ((pixel->mask >> (8 * (dst.bytes - 1 - b))) & 255) {
                MemTxResult tx = address_space_write(&address_space_memory,
                    pixel->destination[b], MEMTXATTRS_UNSPECIFIED,
                    &destination_data[b], 1);

                g_assert(tx == MEMTX_OK);
            }
        }
    }
    return MEMTX_OK;
}

#define CRIME_Z_LIMIT (INT64_C(1) << 36)

typedef struct CrimeRenderSZ {
    bool depth, stencil, write_depth, tag;
    uint32_t control, mode, mask;
    int64_t start, dx, dy;
} CrimeRenderSZ;

static bool crime_render_compare(unsigned function, uint32_t a, uint32_t b)
{
    switch (function) {
    case 0:
        return false;
    case 1:
        return a < b;
    case 2:
        return a == b;
    case 3:
        return a <= b;
    case 4:
        return a > b;
    case 5:
        return a != b;
    case 6:
        return a >= b;
    case 7:
        return true;
    default:
        g_assert_not_reached();
    }
}

static bool crime_render_sz_state(const uint64_t *p, unsigned opcode,
                                    CrimeRenderSZ *sz)
{
    uint32_t draw = p[0x18 / 4];

    sz->depth = draw & 4;
    sz->stencil = draw & 1;
    sz->write_depth = sz->depth && (draw & 2);
    sz->control = p[0x1c0 / 4];
    sz->mode = p[0x1e0 / 4];
    sz->mask = p[0x1e8 / 4];
    sz->tag = (sz->depth || sz->stencil) && (sz->control & 0x1000000);
    if (!sz->depth && !sz->stencil) {
        return true;
    }
    if (opcode > 3 || (draw & 0x200000) ||
        !(draw & 0x400000)) {
        return false;
    }
    if (sz->depth) {
        sz->start = (int64_t)p[0x1c8 / 4];
        sz->dx = (int64_t)p[0x1d0 / 4];
        sz->dy = (int64_t)p[0x1d8 / 4];
        if ((sz->control & 0xf0000000U) ||
            sz->start < -CRIME_Z_LIMIT || sz->start > CRIME_Z_LIMIT ||
            sz->dx < -CRIME_Z_LIMIT || sz->dx > CRIME_Z_LIMIT ||
            sz->dy < -CRIME_Z_LIMIT || sz->dy > CRIME_Z_LIMIT ||
            ((opcode == 0 || opcode == 3) && (sz->dx || sz->dy))) {
            return false;
        }
    }
    if (sz->stencil && ((sz->mode & 0x8000) || (sz->mask & ~0xffU) ||
        ((sz->mode >> 8) & 15) > 5 || ((sz->mode >> 4) & 15) > 5 ||
        (sz->mode & 15) > 5)) {
        return false;
    }
    if (sz->tag) {
        unsigned comparison = (sz->mode >> 12) & 15;
        unsigned depth_fail = (sz->mode >> 4) & 15;

        if (!sz->depth || !sz->stencil ||
            (comparison != 5 && comparison != 7) ||
            ((sz->mode >> 16) & 255) != 255 ||
            ((sz->mode >> 8) & 15) != 0 ||
            (depth_fail != 0 && depth_fail != 2) ||
            (sz->mode & 15) != 2 ||
            sz->mask != (sz->write_depth ? 255 : 0)) {
            return false;
        }
    }
    return true;
}

static bool crime_render_sz_depth(const CrimeRenderSZ *sz,
                                    const CrimeRenderGeometry *g,
                                    int x, int y, uint32_t *depth)
{
    int64_t z = sz->start;

    if (!sz->depth) {
        *depth = 0;
        return true;
    }
    if (g->opcode == 2) {
        z += sz->dx * (x - g->shade_anchor[0]) +
             sz->dy * (y - g->shade_anchor[1]);
    }
    if (z < -CRIME_Z_LIMIT || z > CRIME_Z_LIMIT) {
        return false;
    }
    z = z / 4096 - (z % 4096 < 0);
    *depth = CLAMP(z, 0, 0xffffff);
    return true;
}

static uint32_t crime_render_stencil(unsigned operation, unsigned old,
                                       unsigned reference)
{
    switch (operation) {
    case 0:
        return old;
    case 1:
        return 0;
    case 2:
        return reference;
    case 3:
        return MIN(old + 1, 255);
    case 4:
        return old ? old - 1 : 0;
    case 5:
        return old ^ 255;
    default:
        g_assert_not_reached();
    }
}

static bool crime_render_sz_sample(SGICrimeState *s, const CrimeRenderSZ *sz,
                                     unsigned x, unsigned y, uint32_t depth,
                                     CrimeRenderPixel *pixel, bool *color_pass)
{
    uint8_t data[4];
    uint32_t old, stencil, reference = sz->mode >> 24;
    uint32_t stored_depth, next_stencil, mask = 0;
    unsigned operation;
    bool stencil_pass, depth_pass;

    if (crime_render_translate(s, 2, x, y, 4, 4,
        &pixel->sz_address, &pixel->sz_identity) != CRIME_RENDER_MAPPED ||
        address_space_read(&address_space_memory, pixel->sz_address,
                           MEMTXATTRS_UNSPECIFIED, data, 4) != MEMTX_OK) {
        return false;
    }
    old = ldl_be_p(data);
    stencil = old >> 24;
    stored_depth = old & 0xffffff;
    pixel->sz_mask = 0;
    pixel->sz_value = old;
    if (sz->tag) {
        bool predicate = ((sz->mode >> 12) & 7) == 7 || stencil != reference;
        bool materialize = ((sz->mode >> 4) & 15) == 2;
        uint32_t effective = predicate ? sz->control & 0xffffff : stored_depth;

        depth_pass = crime_render_compare((sz->control >> 25) & 7,
                                           depth, effective);
        *color_pass = depth_pass && (predicate || !materialize);
        if (sz->write_depth && (predicate || !materialize) &&
            (depth_pass || (predicate && materialize))) {
            pixel->sz_mask = UINT32_MAX;
            pixel->sz_value = reference << 24 |
                              (depth_pass ? depth : effective);
        }
        return true;
    }
    stencil_pass = !sz->stencil || crime_render_compare(
        (sz->mode >> 12) & 7, reference & (sz->mode >> 16),
        stencil & (sz->mode >> 16));
    depth_pass = !sz->depth || crime_render_compare(
        (sz->control >> 25) & 7, depth, stored_depth);
    *color_pass = stencil_pass && depth_pass;
    operation = !stencil_pass ? (sz->mode >> 8) & 15 :
                !depth_pass ? (sz->mode >> 4) & 15 : sz->mode & 15;
    next_stencil = stencil;
    if (sz->stencil && operation) {
        mask = sz->mask << 24;
        next_stencil = crime_render_stencil(operation, stencil, reference);
    }
    if (*color_pass && sz->write_depth) {
        mask |= 0xffffff;
    }
    pixel->sz_mask = mask;
    pixel->sz_value = (old & ~mask) |
        (((next_stencil << 24) | depth) & mask);
    return true;
}

static int crime_render_compare_sz(const void *a, const void *b, void *opaque)
{
    const CrimeRenderPixel *pixels = opaque;
    uint64_t x = pixels[*(const uint32_t *)a].sz_identity;
    uint64_t y = pixels[*(const uint32_t *)b].sz_identity;

    return (x > y) - (x < y);
}

static bool crime_render_sz_aliases(CrimeRenderPixel *pixels, unsigned count,
                                      unsigned bytes)
{
    g_autofree uint32_t *sz_order = NULL;
    g_autofree uint32_t *color_order = NULL;
    unsigned colors = 0, z = 0, c = 0;

    if (!count) {
        return true;
    }
    sz_order = g_try_new(uint32_t, count);
    color_order = g_try_new(uint32_t, count);
    if (!sz_order || !color_order) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        sz_order[i] = i;
        if (pixels[i].mask) {
            color_order[colors++] = i;
        }
    }
    g_qsort_with_data(sz_order, count, sizeof(*sz_order),
                      crime_render_compare_sz, pixels);
    g_qsort_with_data(color_order, colors, sizeof(*color_order),
                      crime_render_compare_destination, pixels);
    for (unsigned i = 1; i < count; i++) {
        CrimeRenderPixel *a = &pixels[sz_order[i - 1]];
        CrimeRenderPixel *b = &pixels[sz_order[i]];

        if (a->sz_identity + 4 > b->sz_identity && (a->sz_mask || b->sz_mask)) {
            return false;
        }
    }
    while (z < count && c < colors) {
        uint64_t sz = pixels[sz_order[z]].sz_identity;
        uint64_t color = pixels[color_order[c]].destination_id;

        if (sz + 4 <= color) {
            z++;
        } else if (color + bytes <= sz) {
            c++;
        } else {
            return false;
        }
    }
    return true;
}

static MemTxResult crime_render_execute_plan(hwaddr offset,
                                           CrimeRenderPixel *pixels,
                                           unsigned count, unsigned bytes,
                                           const CrimeTriangleOutput *o,
                                           const uint64_t *p,
                                           unsigned operation, bool sz)
{
    /* Read and stage every result before the first destination write. */
    for (unsigned i = 0; i < count; i++) {
        CrimeRenderPixel *pixel = &pixels[i];
        uint8_t data[4];
        uint32_t old, source, result;

        if (!pixel->mask) {
            continue;
        }
        if (address_space_read(&address_space_memory, pixel->destination,
                               MEMTXATTRS_UNSPECIFIED, data,
                               bytes) != MEMTX_OK) {
            return crime_transfer_fail(offset,
                                       "validated triangle read failed");
        }
        old = crime_render_load_pixel(data, bytes);
        source = o ? crime_triangle_pack(o, p, pixel->color, old,
                                          pixel->x, pixel->y) : pixel->color;
        result = crime_render_rop(operation, source, old);
        pixel->color = (old & ~pixel->mask) | (result & pixel->mask);
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t data[4];

        if (pixels[i].mask) {
            crime_render_store_pixel(data, bytes, pixels[i].color);
            if (address_space_write(&address_space_memory,
                pixels[i].destination, MEMTXATTRS_UNSPECIFIED, data,
                bytes) != MEMTX_OK) {
                return crime_transfer_fail(offset,
                                           "validated color write failed");
            }
        }
        if (sz && pixels[i].sz_mask) {
            stl_be_p(data, pixels[i].sz_value);
            if (address_space_write(&address_space_memory,
                pixels[i].sz_address, MEMTXATTRS_UNSPECIFIED, data,
                4) != MEMTX_OK) {
                return crime_transfer_fail(offset, "validated SZ write failed");
            }
        }
    }
    return MEMTX_OK;
}

/* Modular iteration and ideal signed clamp are functional policies. */
static bool crime_triangle_shade(const uint64_t *p,
                                  const CrimeRenderGeometry *g,
                                  int x, int y, uint32_t *color)
{
    static const unsigned dx[] = { 0xf0, 0xf4, 0x100, 0x104 };
    static const unsigned dy[] = { 0xf8, 0xfc, 0x108, 0x10c };

    *color = 0;
    for (unsigned k = 0; k < 4; k++) {
        int64_t start = sextract32(p[(0xe0 + 4 * k) / 4], 0, 21);
        int64_t a = sextract32(p[dx[k] / 4], 0, 21);
        int64_t b = sextract32(p[dy[k] / 4], 0, 21);
        int64_t value = start + a * (x - g->shade_anchor[0]) +
                        b * (y - g->shade_anchor[1]);
        int64_t component;

        value = sextract64((uint64_t)value, 0, 21);
        component = value / 4096 - (value % 4096 < 0);
        *color |= CLAMP(component, 0, 255) << (24 - 8 * k);
    }
    return true;
}

typedef struct CrimeLineFragment {
    int x, y;
    uint32_t color, depth;
} CrimeLineFragment;

typedef struct CrimeLineFragments {
    CrimeLineFragment *data;
    unsigned len, capacity;
} CrimeLineFragments;

static void crime_line_fragments_clear(CrimeLineFragments *fragments)
{
    g_free(fragments->data);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(CrimeLineFragments, crime_line_fragments_clear)

static bool crime_gl_line_depth(const CrimeRenderSZ *sz,
                                const CrimeGLLine *line, int x, int y,
                                uint32_t *depth)
{
    int64_t n, z;

    *depth = 0;
    if (!sz->depth) {
        return true;
    }
    n = 64 * sz->start + sz->dx * (64 * x - line->vertex[0][0]) +
        sz->dy * (64 * y - line->vertex[0][1]);
    z = crime_line_floor(n, 64);
    if (z < -CRIME_Z_LIMIT || z > CRIME_Z_LIMIT) {
        return false;
    }
    *depth = CLAMP(crime_line_floor(z, 4096), 0, 0xffffff);
    return true;
}

static bool crime_gl_line_color(const uint64_t *p, const CrimeGLLine *line,
                                  int x, int y, unsigned coverage,
                                  uint32_t *color)
{
    static const unsigned dx[] = { 0xf0, 0xf4, 0x100, 0x104 };
    static const unsigned dy[] = { 0xf8, 0xfc, 0x108, 0x10c };

    if (x <= -(1 << 18) || x >= (1 << 18) ||
        y <= -(1 << 18) || y >= (1 << 18)) {
        return false;
    }
    *color = p[0xd0 / 4];
    if (p[0x18 / 4] & 0x10000) {
        *color = 0;
        for (unsigned k = 0; k < 4; k++) {
            int64_t start = sextract32(p[(0xe0 + 4 * k) / 4], 0, 21);
            int64_t a = sextract32(p[dx[k] / 4], 0, 21);
            int64_t b = sextract32(p[dy[k] / 4], 0, 21);
            int64_t n = 64 * start + a * (64 * x - line->vertex[0][0]) +
                        b * (64 * y - line->vertex[0][1]);
            int64_t value = crime_line_floor(n, 64);
            int64_t component;

            if (value < -(1 << 20) || value >= (1 << 20)) {
                return false;
            }
            component = CLAMP(crime_line_floor(value, 4096), 0, 255);
            *color |= component << (24 - 8 * k);
        }
    }
    if (line->aa) {
        *color = (*color & 0xffffff00U) | ((*color & 255) * coverage / 256);
    }
    return true;
}

static bool crime_gl_line_emit(const uint64_t *p, const CrimeGLLine *line,
                                 const CrimeRenderSZ *sz,
                                 CrimeLineFragments *fragments,
                                 int x, int y, unsigned coverage, bool enabled)
{
    CrimeLineFragment fragment = { .x = x, .y = y };

    if (!crime_gl_line_color(p, line, x, y, coverage, &fragment.color) ||
        !crime_gl_line_depth(sz, line, x, y, &fragment.depth)) {
        return false;
    }
    if (enabled && x >= 0 && y >= 0 && x < 2048 && y < 2048) {
        if (fragments->len >= 2048 * 2048) {
            return false;
        }
        if (fragments->len == fragments->capacity) {
            unsigned capacity = fragments->capacity ?
                                fragments->capacity * 2 : 64;
            CrimeLineFragment *data = g_try_realloc_n(
                fragments->data, capacity, sizeof(*data));

            if (!data) {
                return false;
            }
            fragments->data = data;
            fragments->capacity = capacity;
        }
        fragments->data[fragments->len++] = fragment;
    }
    return true;
}

static bool crime_gl_line_band(const uint64_t *p, const CrimeGLLine *line,
                                 const CrimeRenderSZ *sz,
                                 CrimeLineFragments *fragments,
                                 int major, int first, int last, bool enabled)
{
    int step = line->xmajor ? -1 : 1;

    for (int minor = line->xmajor ? last : first;
         minor >= first && minor <= last; minor += step) {
        int x = line->xmajor ? major : minor;
        int y = line->xmajor ? minor : major;

        if (!crime_gl_line_emit(p, line, sz, fragments, x, y, 255, enabled)) {
            return false;
        }
    }
    return true;
}

static bool crime_gl_line_generate(const uint64_t *p, const CrimeGLLine *line,
                                     const CrimeRenderSZ *sz,
                                     CrimeLineFragments *fragments)
{
    unsigned major_axis = line->xmajor ? 0 : 1, minor_axis = 1 - major_axis;
    int first, last;
    float_status fs = { 0 };

    if (line->empty) {
        return true;
    }
    set_float_rounding_mode(float_round_nearest_even, &fs);
    if (line->aa) {
        first = crime_line_floor(MIN(line->p[major_axis], line->q[major_axis]),
                                  CRIME_LINE_SCALE) - 2;
        last = crime_line_floor(MAX(line->p[major_axis], line->q[major_axis]),
                                 CRIME_LINE_SCALE) + 2;
        for (int major = first; major <= last; major++) {
            int center = crime_line_floor(line->j14 +
                                           line->slope * (major - line->k0),
                                           16384);

            /* The normal band and pixel square fit this minor range. */
            for (int minor = center - 4; minor <= center + 4; minor++) {
                int x = line->xmajor ? major : minor;
                int y = line->xmajor ? minor : major;
                float64 area;
                unsigned coverage;

                if (!crime_gl_line_area(line, x, y, &area)) {
                    return false;
                }
                if (!float64_lt(float64_zero, area, &fs)) {
                    continue;
                }
                coverage = MIN(255, float64_to_uint32_round_to_zero(
                    float64_mul(area, int64_to_float64(256, &fs), &fs), &fs));
                if (!crime_gl_line_emit(p, line, sz, fragments, x, y,
                                         coverage, true)) {
                    return false;
                }
            }
        }
    } else {
        int64_t a = line->vertex[0][major_axis];
        int64_t b = line->vertex[1][major_axis];
        int64_t delta = b - a;
        int64_t minor_delta = line->vertex[1][minor_axis] -
                              line->vertex[0][minor_axis];
        int64_t minor_start = line->vertex[0][minor_axis] +
            (line->xmajor ? 32 : -32) * (int64_t)(line->width - 1);
        bool stipple = p[0x18 / 4] & 0x80000;
        unsigned mode = p[0xc0 / 4];
        unsigned start_bit = (mode >> 24) & 31, bit = start_bit;
        unsigned last_bit = (mode >> 16) & 31;
        unsigned repeat = (mode >> 8) & 255, max_repeat = mode & 255;
        int step = delta > 0 ? 1 : -1;

        first = crime_line_floor(MIN(a, b), 64) - 1;
        last = crime_line_floor(MAX(a, b), 64) + 1;
        for (int major = delta > 0 ? first : last;
             major >= first && major <= last; major += step) {
            int64_t numerator = minor_start * delta +
                                minor_delta * (64 * major - a);
            int64_t denominator = delta * 64;
            int center, lo = INT_MAX, hi = INT_MIN;
            bool enabled = !stipple || (p[0xc4 / 4] & (1U << (31 - bit)));

            if (denominator < 0) {
                numerator = -numerator;
                denominator = -denominator;
            }
            center = crime_line_floor(numerator, denominator);
            for (int minor = center - 2; minor <= center + 2; minor++) {
                int x = line->xmajor ? major : minor;
                int y = line->xmajor ? minor : major;
                int begin, end;

                if (!crime_gl_line_diamond(line, x, y)) {
                    continue;
                }
                begin = line->xmajor ? minor - (int)line->width + 1 : minor;
                end = line->xmajor ? minor : minor + (int)line->width - 1;
                if (lo != INT_MAX && begin > hi + 1) {
                    if (!crime_gl_line_band(p, line, sz, fragments,
                                            major, lo, hi, enabled)) {
                        return false;
                    }
                    lo = INT_MAX;
                }
                lo = MIN(lo, begin);
                hi = MAX(hi, end);
            }
            if (lo != INT_MAX &&
                !crime_gl_line_band(p, line, sz, fragments, major, lo, hi,
                                     enabled)) {
                return false;
            }
            if (lo != INT_MAX && stipple) {
                if (repeat < max_repeat) {
                    repeat++;
                } else {
                    repeat = 0;
                    bit = bit == last_bit ? start_bit : bit + 1;
                }
            }
        }
    }
    return !(get_float_exception_flags(&fs) &
             (float_flag_invalid | float_flag_divbyzero | float_flag_overflow));
}

bool sgi_crime_line_coverage(SGICrimeState *s, int x, int y, uint64_t *bits)
{
    CrimeGLLine line = { 0 };
    float64 area;

    if (!s->legacy_software || !s->functional_memory_transfer ||
        !crime_gl_line_state(s->render.parameter, &line) || !line.aa ||
        !crime_gl_line_area(&line, x, y, &area)) {
        return false;
    }
    *bits = float64_val(area);
    return true;
}

static bool crime_texture_ram(void *opaque, uint64_t address, unsigned size,
                               uint64_t *identity, uint8_t *data)
{
    CrimeTransferSpan span;

    if (!crime_transfer_resolve(opaque, address, size, &span) ||
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, data, size) != MEMTX_OK) {
        return false;
    }
    *identity = span.offset;
    return true;
}

static MemTxResult crime_render_de_commit(SGICrimeState *s, hwaddr offset,
                                           unsigned index, uint64_t value,
                                           unsigned words)
{
    uint64_t p[128], linear_first = 0, linear_pitch = 0, linear_last = 0;
    uint32_t draw, clip, plane, stipple_mode;
    unsigned destination_bank, source_bank = 0, bytes, stride;
    unsigned source_bytes, source_stride, screen_count = 0;
    unsigned count = 0, suppressed = 0, operation;
    unsigned bit = 0, repeat = 0, first = 0, last = 0, max_repeat = 0;
    int sx, sy, destination_x, destination_y;
    bool copy, stipple, overlap, inside, upload, download, transfer;
    bool alpha, alpha_pass, triangle, sz_active, gl_rectangle;
    bool native_gl, gl_line, gl_point, gl_span;
    CrimeRenderClip scissor, screen[5];
    CrimeRenderGeometry geometry = { 0 };
    CrimeTriangleOutput output = { 0 };
    CrimeRenderSZ sz = { 0 };
    CrimeGLLine line = { 0 };
    g_auto(CrimeLineFragments) line_fragments = { 0 };
    g_autoptr(CrimeTexture) texture = NULL;
    g_autofree CrimeRenderPixel *pixels = NULL;
    g_autoptr(GArray) bounded_pixels = NULL;
    MemTxResult result;

    memcpy(p, s->render.parameter, sizeof(p));
    if (words) {
        p[index] = words == 2 ? value >> 32 : value;
    }
    if (words == 2) {
        p[index + 1] = (uint32_t)value;
    }
    draw = p[0x18 / 4];
    clip = p[0x10 / 4];
    copy = draw & 0x200000;
    for (unsigned role = 0; role < 2; role++) {
        uint32_t raw = p[role ? 8 / 4 : 0];
        uint32_t ignored = raw & 0xffffe000U;

        if ((role || copy) && ignored &&
            !(s->render_mode_reported & (1U << role))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: %s buffer mode raw 0x%08x "
                          "ignores reserved upper bits 0x%08x\n",
                          role ? "destination" : "source", raw, ignored);
            s->render_mode_reported |= 1U << role;
        }
    }
    if (s->legacy_software && (p[0x60 / 4] >> 24) == 3 &&
        (draw & 0x600000) == 0x600000) {
        result = crime_render_gl_transfer(s, offset, p, index, words);
        if (result == MEMTX_OK) {
            for (unsigned i = 0; i < words; i++) {
                s->render.parameter[index + i] = p[index + i];
            }
        }
        return result;
    }
    sz_active = draw & 5;
    alpha = draw & 0x800;
    if (alpha && (p[0x198 / 4] & ~0x7ffULL)) {
        return crime_transfer_fail(offset, "unsupported alpha comparison");
    }
    triangle = (p[0x60 / 4] >> 24) == 2;
    gl_rectangle = (p[0x60 / 4] >> 24) == 3 && (draw & 0x400000);
    gl_line = (p[0x60 / 4] >> 24) == 1 && (draw & 0x400000);
    gl_point = !(p[0x60 / 4] >> 24) && (draw & 0x400000);
    native_gl = triangle || gl_rectangle || gl_line || gl_point;
    gl_span = gl_rectangle && (draw & 0x40000);
    if ((draw & ~(gl_line ? (draw & 0x1000 ? 0x511ff8U : 0x590fffU) :
                 gl_rectangle ? 0x551fffU : gl_point ? 0x511fffU :
                    triangle ? (0x5b97ffU | (sz_active ? 0x800U : 0)) :
                               0x3e0affU)) ||
        (native_gl ? (!(draw & 0x400000) ||
                      !crime_triangle_output(p[0x8 / 4], &output)) :
                    (p[0x50 / 4] ||
                     (copy ? p[0x58 / 4] != 0 :
                             (p[0x58 / 4] & 0x80008000U)))) ||
        !crime_render_geometry(p, &geometry)) {
        return crime_transfer_fail(offset, "unsupported primitive state");
    }
    if (draw & 0x8000) {
        if (!triangle || geometry.samples > CRIME_TEXTURE_MAX_FRAGMENTS) {
            return crime_transfer_fail(offset, "unsupported texture primitive");
        }
        texture = crime_texture_new(p, s->render.texture,
                                     geometry.shade_anchor[0],
                                     geometry.shade_anchor[1], false,
                                     crime_texture_ram, s);
        if (!texture) {
            return crime_transfer_fail(offset, "unsupported texture state");
        }
    }
    if (gl_line && (draw & 0x80007) && words &&
        (index == 0x190 / 4 || index == 0x194 / 4 ||
         crime_render_texture_start(index * 4))) {
        return crime_transfer_fail(offset,
                                   "unsupported composed GL line START");
    }
    if (words && (index == 0x190 / 4 || index == 0x194 / 4) && !gl_line) {
        return crime_transfer_fail(offset, "AA START requires native GL line");
    }
    if (((draw & 7) && (copy || !native_gl)) ||
        !crime_render_sz_state(p, geometry.opcode, &sz)) {
        return crime_transfer_fail(offset, "unsupported depth/stencil state");
    }
    if (native_gl && (draw & 0x400) &&
        ((p[0x1a8 / 4] & ~0xfffULL) ||
         ((p[0x1a8 / 4] >> 8) & 15) > 4 ||
         ((p[0x1a8 / 4] >> 4) & 15) > 12 ||
         (p[0x1a8 / 4] & 15) > 11)) {
        return crime_transfer_fail(offset, "unsupported blend function");
    }
    destination_x = native_gl ? (int16_t)(p[0x58 / 4] >> 16) :
                               (int)(p[0x58 / 4] >> 16);
    destination_y = native_gl ? (int16_t)p[0x58 / 4] :
                               (int)(p[0x58 / 4] & 0xffff);
    stipple = (geometry.opcode == 1 && (draw & 0x80000)) ||
              (geometry.opcode == 3 && (draw & 0x40000));
    if (alpha && (copy || (stipple && !gl_line && !gl_span) ||
                  (!gl_line && !gl_rectangle && !gl_point &&
                   (p[0x8 / 4] & 0x3ffU) != 0x228))) {
        return crime_transfer_fail(offset, "unsupported alpha primitive");
    }
    if (copy && (geometry.opcode != 3 || stipple)) {
        return crime_transfer_fail(offset,
                                   "unsupported stipple or primitive copy");
    }
    if (stipple) {
        stipple_mode = p[0xc0 / 4];
        first = bit = (stipple_mode >> 24) & 31;
        last = (stipple_mode >> 16) & 31;
        repeat = (stipple_mode >> 8) & 255;
        max_repeat = stipple_mode & 255;
        if ((stipple_mode & 0xe0e00000U) || first > last ||
            repeat > max_repeat) {
            return crime_transfer_fail(offset, "invalid stipple mode");
        }
    }
    if (copy && ((p[0] | p[8 / 4]) & 0xf0) &&
        (((p[0] >> 10) & 7) >= 4 || ((p[8 / 4] >> 10) & 7) >= 4)) {
        if (words && (index == 0xb0 / 4 || index == 0xb4 / 4) &&
            ((p[8 / 4] >> 10) & 7) < 4) {
            return crime_transfer_fail(offset,
                                       "destination START requires download");
        }
        result = crime_render_color(s, offset, p, &geometry);
        if (result == MEMTX_OK) {
            for (unsigned i = 0; i < words; i++) {
                s->render.parameter[index + i] = p[index + i];
            }
        }
        return result;
    }
    if (native_gl) {
        destination_bank = output.bank;
        bytes = stride = output.bytes;
    } else if (!crime_render_mode(p[0x8 / 4], &destination_bank,
                                  &bytes, &stride)) {
        return crime_transfer_fail(offset, "unsupported destination mode");
    }
    if (copy &&
        !crime_render_mode(p[0], &source_bank, &source_bytes, &source_stride)) {
        return crime_transfer_fail(offset, "unsupported rectangle source");
    }
    upload = copy && source_bank >= 4 && destination_bank < 3;
    download = copy && source_bank < 3 && destination_bank >= 4;
    transfer = upload || download;
    if (transfer) {
        if (bytes != 1 || source_bytes != 1 || (draw & ~0x3002f8U)) {
            return crime_transfer_fail(offset, "unsupported indexed transfer");
        }
    } else if (destination_bank >= 4 || (copy && source_bank >= 4) ||
               (copy && (source_bytes != bytes || source_stride != stride ||
                         p[0xa8 / 4] != 1 || p[0xac / 4] != 1))) {
        return crime_transfer_fail(offset, "unsupported rectangle layout");
    }
    if (words && (index == 0xb0 / 4 || index == 0xb4 / 4) && !download) {
        return crime_transfer_fail(offset,
                                   "destination START requires download");
    }
    operation = draw & 0x200 ? p[0x1b0 / 4] : 3;
    if ((draw & 0x200) && p[0x1b0 / 4] > 15) {
        return crime_transfer_fail(offset, "unsupported raster operation");
    }
    inside = clip & 31;
    if ((clip & ~(native_gl ? 0x7ffU : 0x3ffU)) ||
        ((clip & 31) & ~((clip >> 5) & 31))) {
        return crime_transfer_fail(offset, "unsupported clip mode");
    }
    for (unsigned n = 0; n < 5; n++) {
        if (clip & (1U << (5 + n))) {
            CrimeRenderClip box = crime_render_clip(p[(0x20 + 8 * n) / 4]);

            if (box.xmin > box.xmax || box.ymin > box.ymax) {
                return crime_transfer_fail(offset, "invalid screen mask");
            }
            screen[screen_count++] = box;
        }
    }
    if (screen_count > 1 &&
        ((clip & 31) != ((clip >> 5) & 31) || transfer)) {
        return crime_transfer_fail(offset, "unsupported screen mask union");
    }
    scissor = crime_render_clip(p[0x48 / 4]);
    if ((draw & 0x100000) &&
        (scissor.xmin > scissor.xmax || scissor.ymin > scissor.ymax)) {
        return crime_transfer_fail(offset, "invalid scissor");
    }
    sx = p[0xa0 / 4] >> 16;
    sy = p[0xa0 / 4] & 0xffff;
    if (download) {
        if (sx >= 2048 || sy >= 2048 || sx + geometry.width > 2048 ||
            sy + geometry.samples / geometry.width > 2048 ||
            p[0xa8 / 4] != 1 || p[0xac / 4] != 1 || p[0xb4 / 4] != 1) {
            return crime_transfer_fail(offset, "invalid indexed download");
        }
        linear_first = p[0xb0 / 4];
        linear_pitch = geometry.width;
        linear_last = linear_first + geometry.samples - 1;
    } else if (upload) {
        if (p[0xa8 / 4] != 1 || !p[0xac / 4] || p[0xac / 4] >= 0x10000000) {
            return crime_transfer_fail(offset,
                                       "unsupported upload source step");
        }
        linear_first = p[0xa0 / 4];
        linear_pitch = geometry.width - 1 + p[0xac / 4];
        linear_last = linear_first +
            (geometry.samples / geometry.width - 1) * linear_pitch +
            geometry.width - 1;
    } else if (copy && (sx >= 2048 || sy >= 2048 ||
                        sx + geometry.x1 - geometry.x0 < 0 ||
                        sx + geometry.x1 - geometry.x0 >= 2048 ||
                        sy + geometry.y1 - geometry.y0 < 0 ||
                        sy + geometry.y1 - geometry.y0 >= 2048)) {
        return crime_transfer_fail(offset, "invalid source rectangle");
    }
    if (transfer && (linear_last > UINT32_MAX ||
        (linear_last >> 12) - (linear_first >> 12) >= 32)) {
        return crime_transfer_fail(offset, "invalid indexed linear envelope");
    }
    plane = draw & 0x80 ? p[0x1b8 / 4] : UINT32_MAX;
    plane &= native_gl ? output.mask : UINT32_MAX >> (32 - 8 * bytes);
    alpha_pass = !alpha || crime_render_alpha_pass(p[0xd0 / 4], p[0x198 / 4]);
    if (gl_line) {
        if (!crime_gl_line_state(p, &line) ||
            !crime_gl_line_generate(p, &line, &sz, &line_fragments)) {
            return crime_transfer_fail(offset, "invalid GL line producer");
        }
        geometry.samples = geometry.capacity = line_fragments.len;
    }
    if (geometry.opcode <= 1 && !native_gl) {
        bounded_pixels = g_array_new(false, false, sizeof(CrimeRenderPixel));
    } else {
        pixels = g_try_new(CrimeRenderPixel, geometry.capacity);
        if (geometry.capacity && !pixels) {
            return crime_transfer_fail(offset, "primitive allocation failed");
        }
    }
    for (unsigned i = 0; i < geometry.samples; i++) {
        int cx, cy;
        unsigned stripe_x = 1, stripe_y = 1, first_col = 0, first_row = 0;
        uint32_t color = p[0xd0 / 4], fragment_depth = 0;
        uint32_t linear = transfer ? linear_first +
            (i / geometry.width) * linear_pitch + i % geometry.width : 0;
        bool foreground = gl_line || !stipple ||
                          (p[0xc4 / 4] & (1U << (31 - bit)));

        if (stipple && !gl_line) {
            if (repeat < max_repeat) {
                repeat++;
            } else {
                repeat = 0;
                bit = bit == last ? first : bit + 1;
            }
        }
        if (!foreground) {
            if (!(draw & 0x20000)) {
                continue;
            }
            color = p[0xd8 / 4];
        }
        if (gl_line) {
            CrimeLineFragment *fragment = &line_fragments.data[i];

            cx = fragment->x;
            cy = fragment->y;
            color = fragment->color;
            fragment_depth = fragment->depth;
            alpha_pass = !alpha ||
                         crime_render_alpha_pass(color, p[0x198 / 4]);
        } else {
            crime_render_sample(&geometry, i, &cx, &cy);
        }
        if (triangle) {
            if (!crime_render_triangle_inside(&geometry, cx, cy)) {
                continue;
            }
            if ((draw & 0x10000) &&
                !crime_triangle_shade(p, &geometry, cx, cy, &color)) {
                return crime_transfer_fail(offset, "shade arithmetic overflow");
            }
            if (texture && !crime_texture_sample(texture, cx, cy, &color)) {
                return crime_transfer_fail(offset, "invalid texture sample");
            }
            alpha_pass = !alpha ||
                         crime_render_alpha_pass(color, p[0x198 / 4]);
        }
        if (sz_active && !gl_line && !crime_render_sz_depth(&sz, &geometry,
                                                 cx, cy, &fragment_depth)) {
            return crime_transfer_fail(offset, "depth arithmetic overflow");
        }
        if (geometry.opcode == 1 && !gl_line) {
            if (!geometry.steps || !geometry.xmajor) {
                stripe_x = geometry.thickness;
                cx += geometry.lower;
            }
            if (!geometry.steps || geometry.xmajor) {
                stripe_y = geometry.thickness;
                cy += geometry.lower;
            }
        }
        if (geometry.opcode <= 1 && !native_gl) {
            int64_t left = (int64_t)cx + destination_x;
            int64_t top = (int64_t)cy + destination_y;

            if (left >= 2048 || top >= 2048 ||
                left + stripe_x <= 0 || top + stripe_y <= 0) {
                continue;
            }
            first_col = MAX(0LL, -left);
            first_row = MAX(0LL, -top);
            stripe_x = MIN((int64_t)stripe_x, 2048 - left);
            stripe_y = MIN((int64_t)stripe_y, 2048 - top);
        }
        for (unsigned row = first_row; row < stripe_y; row++) {
            int local_y = cy + (int)row;
            int y = local_y + (native_gl ? 0 : destination_y);

            for (unsigned col = first_col; col < stripe_x; col++) {
                int local_x = cx + (int)col;
                int x = local_x + (native_gl ? 0 : destination_x);
                uint32_t mask = plane;
                CrimeRenderPixel bounded_pixel = { 0 };
                CrimeRenderPixel *pixel;
                CrimeRenderTranslation translation;
                bool screen_pass = !screen_count;

                for (unsigned n = 0; n < screen_count; n++) {
                    screen_pass |= crime_render_inside(&screen[n], x, y) ==
                                   inside;
                }
                if (x < 0 || x >= 2048 || y < 0 || y >= 2048 ||
                    ((draw & 0x100000) &&
                     !crime_render_inside(&scissor,
                         native_gl ? x - destination_x : local_x,
                         native_gl ? y - destination_y : local_y)) ||
                    !screen_pass) {
                    continue;
                }
                for (unsigned b = 0; b < bytes; b++) {
                    unsigned lane = download ? (linear & 3) :
                        (x * stride + stride - bytes + b) & 3;

                    if (!(draw & (1U << (6 - lane)))) {
                        mask &= ~(0xffU << (8 * (bytes - 1 - b)));
                    }
                }
                if ((!mask && !sz_active) || !alpha_pass) {
                    continue;
                }
                pixel = bounded_pixels ? &bounded_pixel : &pixels[count];
                *pixel = (CrimeRenderPixel) { 0 };
                if (sz_active) {
                    bool color_pass;

                    if (!crime_render_sz_sample(s, &sz, x, y, fragment_depth,
                                                 pixel, &color_pass)) {
                        return crime_transfer_fail(offset,
                                                   "invalid SZ mapping");
                    }
                    if (!color_pass) {
                        mask = 0;
                    }
                }
                if (!mask) {
                    translation = CRIME_RENDER_MAPPED;
                } else if (download) {
                    translation = crime_render_linear(
                        s, destination_bank, linear, &pixel->destination,
                        &pixel->destination_id);
                } else {
                    translation = crime_render_translate(
                        s, destination_bank, x, y, bytes, stride,
                        &pixel->destination, &pixel->destination_id);
                }

                if (!copy && translation == CRIME_RENDER_INVALID_TILE) {
                    suppressed++;
                    if (!sz_active) {
                        continue;
                    }
                    mask = 0;
                    translation = CRIME_RENDER_MAPPED;
                }
                if (translation != CRIME_RENDER_MAPPED) {
                    return crime_transfer_fail(offset,
                                               "invalid primitive mapping");
                }
                if (copy) {
                    if (upload) {
                        translation = crime_render_linear(
                            s, source_bank, linear, &pixel->source,
                            &pixel->source_id);
                    } else {
                        int origin_x = download ?
                            MIN(geometry.x0, geometry.x1) : geometry.x0;
                        int origin_y = download ?
                            MIN(geometry.y0, geometry.y1) : geometry.y0;

                        translation = crime_render_translate(
                            s, source_bank, sx + x - origin_x,
                            sy + y - origin_y, source_bytes, source_stride,
                            &pixel->source, &pixel->source_id);
                    }
                }
                if (copy && translation != CRIME_RENDER_MAPPED) {
                    return crime_transfer_fail(offset,
                                               "invalid primitive mapping");
                }
                pixel->mask = mask;
                pixel->color = color;
                pixel->x = x;
                pixel->y = y;
                if (bounded_pixels) {
                    g_array_append_val(bounded_pixels, bounded_pixel);
                }
                count++;
            }
        }
    }
    if (bounded_pixels) {
        pixels = (CrimeRenderPixel *)g_array_free(
            g_steal_pointer(&bounded_pixels), false);
    }
    if (!crime_render_validate(pixels, count, bytes, transfer, &overlap) ||
        (transfer && overlap)) {
        return crime_transfer_fail(offset,
                                   "primitive memory validation failed");
    }
    if (sz_active && !crime_render_sz_aliases(pixels, count, bytes)) {
        return crime_transfer_fail(offset, "SZ memory validation failed");
    }
    /* MMIO holds the BQL, keeping prevalidated RAM mappings stable. */
    result = native_gl || sz_active ?
        crime_render_execute_plan(offset, pixels, count, bytes,
                                   native_gl ? &output : NULL, p,
                                   operation, sz_active) :
        crime_render_execute(offset, pixels, count, bytes, copy,
                              operation, true);
    if (result == MEMTX_OK) {
        for (unsigned i = 0; i < words; i++) {
            s->render.parameter[index + i] = p[index + i];
        }
        if (suppressed) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: %s suppressed %u invalid destination "
                          "fragments\n", geometry.opcode == 0 ? "point" :
                          geometry.opcode == 1 ? "line" :
                          triangle ? "triangle" : "rectangle",
                          suppressed);
        }
    }
    return result;
}

static void crime_render_record_tile(uint64_t *bases, bool *seen,
                                      unsigned index, uint64_t identity,
                                      unsigned x, unsigned y, bool *unique)
{
    uint64_t base = identity - (y % 128) * 512 - x % 512;

    if (!seen[index]) {
        for (unsigned i = 0; i < 256; i++) {
            if (seen[i] && bases[i] == base) {
                *unique = false;
            }
        }
        seen[index] = true;
        bases[index] = base;
    } else if (bases[index] != base) {
        *unique = false;
    }
}

static MemTxResult crime_tiled_commit(SGICrimeState *s, hwaddr offset,
                                       const uint32_t *p)
{
    unsigned mode = p[0], source_bank = (mode >> 5) & 7;
    unsigned destination_bank = (mode >> 2) & 7;
    bool copy = mode & 0x800, stipple = mode & 0x400;
    bool overlap, unique = true;
    unsigned depth = (mode >> 8) & 3, pixel_bytes = 1U << depth;
    int dy = p[9] == 1 ? 1 : -1;
    int dx0 = p[6] >> 16, dx1 = p[7] >> 16;
    int dy0 = p[6] & 0xffff, dy1 = p[7] & 0xffff;
    int sx0 = p[4] >> 16, sx1 = p[5] >> 16;
    int sy0 = p[4] & 0xffff, sy1 = p[5] & 0xffff;
    unsigned width, height, count = 0, enabled = 0;
    uint64_t bases[256] = { 0 };
    bool seen[256] = { false };
    g_autofree CrimeRenderPixel *pixels = NULL;

    if ((mode & ~0xfffU) || depth == 3 || destination_bank >= 3 ||
        (copy && (source_bank >= 3 || depth || stipple)) ||
        (!copy && (depth != 2 || stipple) &&
         (dx0 % pixel_bytes || (dx1 + 1) % pixel_bytes)) ||
        (p[9] != 1 && (!copy || p[9] != UINT32_MAX)) ||
        (copy && p[8] != p[9]) ||
        dx0 >= 8192 || dx1 >= 8192 || dy0 >= 2048 || dy1 >= 2048 ||
        dx1 < dx0 || (dy1 - dy0) * dy < 0) {
        return crime_transfer_fail(offset, "unsupported tiled transfer");
    }
    width = dx1 - dx0 + 1;
    height = abs(dy1 - dy0) + 1;
    if (copy &&
        (sx0 >= 8192 || sx1 >= 8192 || sy0 >= 2048 || sy1 >= 2048 ||
         sx1 < sx0 || (sy1 - sy0) * dy < 0 ||
         sx1 - sx0 + 1 != width || abs(sy1 - sy0) + 1 != height)) {
        return crime_transfer_fail(offset, "invalid tiled source bounds");
    }
    pixels = g_new(CrimeRenderPixel, width * height);
    for (unsigned row = 0; row < height; row++) {
        unsigned y = dy0 + dy * (int)row;
        unsigned sy = sy0 + dy * (int)row;

        for (unsigned col = 0; col < width; col++) {
            unsigned x = dx0 + col, sx = sx0 + col;
            CrimeRenderPixel *pixel = &pixels[count++];

            pixel->mask = 0xff;
            pixel->color = (p[3] >> (8 * (3 - (x & 3)))) & 0xff;
            if (crime_framebuffer_translate(s, destination_bank, x, y, 1,
                                            &pixel->destination,
                                            &pixel->destination_id) !=
                CRIME_RENDER_MAPPED ||
                (copy &&
                 crime_framebuffer_translate(s, source_bank, sx, sy, 1,
                                             &pixel->source,
                                             &pixel->source_id) !=
                 CRIME_RENDER_MAPPED)) {
                return crime_transfer_fail(offset, "invalid tiled mapping");
            }
            if (copy && source_bank == destination_bank) {
                crime_render_record_tile(bases, seen, (y / 128) * 16 + x / 512,
                                           pixel->destination_id,
                                           x, y, &unique);
                crime_render_record_tile(bases, seen,
                                           (sy / 128) * 16 + sx / 512,
                                           pixel->source_id, sx, sy, &unique);
            }
        }
    }
    if (!crime_render_validate(pixels, count, 1, copy, &overlap) ||
        (overlap && (source_bank != destination_bank ||
                     sx0 != dx0 || sx1 != dx1 || !unique))) {
        return crime_transfer_fail(offset,
                                   "unsupported tiled alias or overlap");
    }
    if ((mode & 1) || (copy && (mode & 2))) {
        crime_healthy_memory_notice(s);
    }
    /* A disabled byte still participates in full-rectangle preflight. */
    for (unsigned i = 0; i < count; i++) {
        unsigned x = dx0 + i % width;

        unsigned bit = ((x - (dx0 & ~31U)) / pixel_bytes) & 31;

        if ((p[1] & (0x80000000U >> (x & 31))) &&
            (!stipple || (p[2] & (0x80000000U >> bit)))) {
            pixels[enabled++] = pixels[i];
        }
    }
    return crime_render_execute(offset, pixels, enabled, 1, copy, 3, false);
}

static uint64_t *crime_render_translation_word(SGICrimeState *s,
                                                 hwaddr offset)
{
    if (offset < 0x1001000 || offset >= 0x1001800) {
        return NULL;
    }
    if (offset < 0x1001600) {
        return &s->render.tlb[(offset - 0x1001000) / 8];
    }
    if (offset < 0x10016e0) {
        return &s->render.texture[(offset - 0x1001600) / 8];
    }
    if (offset < CRIME_LINEAR_TLB) {
        return &s->transfer.clip[(offset - CRIME_CLIP_TLB) / 8];
    }
    return &s->transfer.tlb[(offset - CRIME_LINEAR_TLB) / 8];
}

static MemTxResult crime_render_state_read(SGICrimeState *s, hwaddr offset,
                                            uint64_t *value, unsigned size)
{
    const CrimeRenderRegister *reg;
    uint64_t *word = crime_render_translation_word(s, offset);

    if (word && size == 8 && !(offset & 7)) {
        *value = *word;
        return MEMTX_OK;
    }
    reg = crime_render_register(offset - 0x1002000);
    if (reg && reg->readable && size == reg->size) {
        *value = s->render.parameter[reg->offset / 4];
        return MEMTX_OK;
    }
    return crime_transfer_fail(offset, "invalid renderer state read");
}

static MemTxResult crime_render_state_write(SGICrimeState *s, hwaddr offset,
                                             uint64_t value, unsigned size)
{
    const CrimeRenderRegister *reg;
    uint64_t *word = crime_render_translation_word(s, offset);

    if (word && size == 8 && !(offset & 7)) {
        *word = value;
        return MEMTX_OK;
    }
    reg = crime_render_register(offset - 0x1002000);
    if (reg && size == reg->size) {
        s->render.parameter[reg->offset / 4] = value;
        return MEMTX_OK;
    }
    if (reg && reg->size == 4 && size == 8 && !(offset & 7)) {
        unsigned index = reg->offset / 4;

        /* Selected big-endian transport policy, not silicon lane behavior. */
        switch (reg->offset) {
        case 0x000: case 0x008: case 0x010: case 0x018:
        case 0x050: case 0x058: case 0x060: case 0x078:
        case 0x0d0: case 0x0d8: case 0x1b0: case 0x1b8:
            s->render.parameter[index] = value >> 32;
            return MEMTX_OK;
        case 0x070:
        case 0x080: case 0x088: case 0x090:
        case 0x0e0: case 0x0e8: case 0x0f0: case 0x0f8:
        case 0x100: case 0x108: case 0x190:
        case 0x0a8:
        case 0x0c0: {
            const CrimeRenderRegister *second =
                crime_render_register(reg->offset + 4);

            if (!second || second->size != 4) {
                break;
            }
            /* Validate both members before publishing either logical word. */
            s->render.parameter[index] = value >> 32;
            s->render.parameter[second->offset / 4] = (uint32_t)value;
            return MEMTX_OK;
        }
        default:
            break;
        }
    }
    return crime_transfer_fail(offset, "invalid renderer state write");
}

static bool crime_renderer_must_stop(hwaddr offset, unsigned size, bool write)
{
    offset -= 0x1000000;
    /* Bad setup must stop before a later command can use stale state. */
    return crime_renderer_touches(offset, size, 0x16e0, 0x20) ||
           crime_renderer_touches(offset, size, 0x1ee0, 0x20) ||
           crime_renderer_touches(offset, size, 0x1700, 0x100) ||
           crime_renderer_touches(offset, size, 0x2800, 0x800) ||
           crime_renderer_touches(offset, size, 0x3000, 0x80) ||
           crime_renderer_touches(offset, size, 0x3800, 0x800) ||
           crime_renderer_touches(offset, size, 0x4000, 0x1000) ||
           (write &&
            (crime_renderer_touches(offset, size, 0x400, 4) ||
             crime_renderer_touches(offset, size, 0x21f0, 4) ||
             crime_renderer_touches(offset, size, 0x21f8, 4)));
}

static MemTxResult crime_renderer_unsupported(SGICrimeState *s, hwaddr offset,
                                              unsigned size, bool write,
                                              uint64_t value)
{
    if (s->legacy_software && s->functional_memory_transfer &&
        offset >= 0x1000000 && offset + size <= 0x1005000 &&
        (!crime_renderer_must_stop(offset, size, write) ||
         (!write && offset == CRIME_TRANSFER + 0x70 && size == 4))) {
        if (write) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: unsupported renderer %u-byte write "
                          "at 0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                          size, (hwaddr)(0x14000000ULL + offset), value);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: unsupported renderer %u-byte read "
                          "at 0x%" HWADDR_PRIx "\n", size,
                          (hwaddr)(0x14000000ULL + offset));
        }
        return MEMTX_ERROR;
    }
    if (write) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: rejected renderer %u-byte write at "
                      "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n", size,
                      (hwaddr)(0x14000000ULL + offset), value);
    }
    return crime_transfer_fail(offset, write ?
                               "unsupported transfer register write" :
                               "unsupported transfer register read");
}

static MemTxResult crime_ifb_access(SGICrimeState *s, hwaddr offset,
                                    uint64_t *value, unsigned size, bool write)
{
    uint64_t *word;
    unsigned index;

    if ((size != 4 && size != 8) || (offset & (size - 1)) ||
        offset < 0x1000000 || offset + size > 0x1000400) {
        return crime_renderer_unsupported(s, offset, size, write,
                                           write ? *value : 0);
    }
    index = (offset - 0x1000000) / 8;
    word = index < 64 ? &s->render.ifb_data[index] :
                        &s->render.ifb_address[index - 64];
    if (write) {
        if (size == 8) {
            *word = *value;
        } else {
            unsigned shift = offset & 4 ? 0 : 32;
            uint64_t mask = (uint64_t)UINT32_MAX << shift;

            *word = (*word & ~mask) | ((uint64_t)(uint32_t)*value << shift);
        }
    } else {
        *value = size == 8 ? *word :
                 offset & 4 ? (uint32_t)*word : *word >> 32;
    }
    return MEMTX_OK;
}

static void crime_ifb_record(SGICrimeState *s, hwaddr offset,
                             uint64_t value, unsigned size)
{
    unsigned head = s->render.ifb_head;
    unsigned width = size == 8 ? 3 : offset & 4 ? 1 : 2;
    uint32_t header = ((offset & 0x7f8) >> 3) |
                      (((offset >> 12) & 7) << 8) | (width << 11) |
                      (!!(offset & 0x800) << 13);

    g_assert(size == 4 || size == 8);
    s->render.ifb_data[head] = size == 8 ? value :
        (uint64_t)(uint32_t)value << (offset & 4 ? 0 : 32);
    s->render.ifb_address[head] = (uint64_t)header << 32;
    s->render.ifb_head = (head + 1) & 63;
}

static MemTxResult crime_transfer_read(SGICrimeState *s, hwaddr offset,
                                       uint64_t *value, unsigned size)
{
    unsigned index;

    if (s->legacy_software &&
        crime_renderer_touches(offset, size, 0x1000000, 0x400)) {
        return crime_ifb_access(s, offset, value, size, false);
    }
    if (s->legacy_software && offset == 0x1000400 && size == 4) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: undefined interface-buffer diagnostic "
                      "%u-byte read at 0x%" HWADDR_PRIx "\n", size,
                      (hwaddr)(0x14000000ULL + offset));
        *value = 0;
        return MEMTX_OK;
    }
    if (s->legacy_software && crime_render_state_access(offset, size)) {
        return crime_render_state_read(s, offset, value, size);
    }
    if (offset >= CRIME_LINEAR_TLB && offset < CRIME_LINEAR_TLB + 256 &&
        size == 8 && !(offset & 7)) {
        *value = s->transfer.tlb[(offset - CRIME_LINEAR_TLB) / 8];
        return MEMTX_OK;
    }
    if (offset == CRIME_RENDER_STATUS && size == 4) {
        unsigned head = s->legacy_software ? s->render.ifb_head : 0;

        *value = CRIME_EMPTY_RENDER_STATUS | head | (head << 6) | (head << 12);
        return MEMTX_OK;
    }
    if (offset >= CRIME_TRANSFER && offset < CRIME_TRANSFER + 0x50 &&
        size == 4 &&
        crime_transfer_parameter(offset - CRIME_TRANSFER, &index) &&
        index != 6 && index != 7) {
        *value = s->transfer.parameter[index];
        return MEMTX_OK;
    }
    return crime_renderer_unsupported(s, offset, size, false, 0);
}

static MemTxResult crime_transfer_write(SGICrimeState *s, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    unsigned index;

    if (s->legacy_software &&
        crime_renderer_touches(offset, size, 0x1000000, 0x400)) {
        return crime_ifb_access(s, offset, &value, size, true);
    }
    if (s->legacy_software) {
        const CrimeRenderRegister *reg =
            crime_render_register(offset - 0x1002800);

        if (offset == 0x1000400 && size == 4) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: opaque empty interface-buffer control "
                          "(unimplemented fields) %u-byte write at "
                          "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n", size,
                          (hwaddr)(0x14000000ULL + offset), value);
            return MEMTX_OK;
        }
        if (offset == 0x1000408 && size == 4 && !value) {
            /*
             * IRIX writes zero here first in renderer initialization and
             * error recovery. Its role, a renderer and interface-buffer
             * reset or enable, is inferred; with nothing ever queued there
             * is nothing to discard, so it is accepted without effect.
             */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: renderer 4-byte zero write at "
                          "0x%" HWADDR_PRIx " accepted as a no-op "
                          "(inferred reset or enable role not modeled)\n",
                          (hwaddr)(0x14000000ULL + offset));
            return MEMTX_OK;
        }
        if (offset == CRIME_TRANSFER + 0x10 && size == 4) {
            s->transfer.parameter[2] = value;
            return MEMTX_OK;
        }
        if (offset == CRIME_RENDER_STATUS + 8 && size == 4) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: opaque engine-status-bank "
                          "(undefined semantics) %u-byte write at "
                          "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n", size,
                          (hwaddr)(0x14000000ULL + offset), value);
            return MEMTX_OK;
        }
        if (crime_render_state_access(offset, size)) {
            return crime_render_state_write(s, offset, value, size);
        }
        if (reg && reg->offset >= 0x080 && reg->offset <= 0x094 &&
            (size == 4 || (size == 8 && !(offset & 7)))) {
            const uint64_t *p = s->render.parameter;
            unsigned opcode = p[0x60 / 4] >> 24;
            unsigned source_bank = (p[0] >> 10) & 7;
            bool point = !opcode && (p[0x18 / 4] & 0x400000);
            bool upload = opcode == 3 &&
                (p[0x18 / 4] & 0x600000) == 0x600000 &&
                (source_bank == 4 || source_bank == 5);

            if (point || upload) {
                return crime_render_de_commit(s, offset, reg->offset / 4,
                                               value, size / 4);
            }
        }
        if (reg && (!reg->native_only ||
                    (reg->offset >= 0xe0 && reg->offset <= 0x10c) ||
                    (reg->offset >= 0x1c0 && reg->offset <= 0x1e8) ||
                    reg->offset == 0x190 || reg->offset == 0x194 ||
                    crime_render_texture_start(reg->offset))) {
            bool paired = size == 8 &&
                (reg->offset == 0x070 || reg->offset == 0x0a8 ||
                 reg->offset == 0x0c0 || reg->offset == 0x190);

            if (size == reg->size || paired) {
                return crime_render_de_commit(s, offset, reg->offset / 4,
                                               value, paired ? 2 : 1);
            }
        }
        if (size == 4) {
            if (offset == 0x10021f0 || offset == CRIME_TRANSFER + 0x70 ||
                ((!value) && (offset == 0x10021f8 ||
                              offset == CRIME_TRANSFER + 0x78))) {
                return MEMTX_OK;
            }
            if (offset == 0x10029f0) {
                return crime_render_de_commit(s, offset, 0, 0, 0);
            }
            if (offset == CRIME_TRANSFER + 0x870) {
                return crime_transfer_commit(s, offset,
                                               CRIME_TRANSFER_PARAMETERS, 0);
            }
            if (offset == CRIME_TRANSFER + 0x810) {
                uint32_t mode = s->transfer.parameter[0];

                if ((mode & 0x800) || ((mode >> 2) & 7) >= 3) {
                    return crime_transfer_fail(offset,
                                               "stipple START requires tiled clear");
                }
                return crime_transfer_commit(s, offset, 2, value);
            }
            if (offset >= CRIME_TRANSFER + 0x800 &&
                offset < CRIME_TRANSFER + 0x850 &&
                crime_transfer_parameter(offset - CRIME_TRANSFER - 0x800,
                                           &index)) {
                return crime_transfer_commit(s, offset, index, value);
            }
        }
    }
    if (offset >= CRIME_LINEAR_TLB && offset < CRIME_LINEAR_TLB + 256 &&
        size == 8 && !(offset & 7)) {
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
    return crime_renderer_unsupported(s, offset, size, true, value);
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

void sgi_crime_update_mace_inputs(DeviceState *dev, uint16_t mask,
                                  uint16_t value)
{
    SGICrimeState *s = SGI_CRIME(dev);

    s->inputs = (s->inputs & ~(uint32_t)mask) | (value & mask);
    crime_update_hardware(s);
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
    if (s->legacy_software && offset == 0x003c && size == 4) {
        *value = crime_timer_value(s);
        return MEMTX_OK;
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
    /* Legacy compatibility reads extend the documented doubleword interface. */
    if (s->legacy_software && size == 4 && !(offset & 3) &&
        offset >= 0x208 && offset <= 0x244) {
        unsigned bank = (offset - 0x208) / 8;
        uint64_t control = s->eight_128m_banks ? s->bank_control[bank] : bank;

        *value = offset & 4 ? (uint32_t)control : control >> 32;
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
        *value = s->ecc_replacement;
        break;
    case 0x0058:
    case 0x0268:
        if (!s->legacy_software) {
            goto unsupported;
        }
        /* fall through */
    case 0x0260:
        if (s->legacy_software) {
            /*
             * Approximation: no VICE error source, syndrome or check-bit
             * storage is modeled, and zero is not what hardware claims.
             */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: error register 0x%" HWADDR_PRIx
                          " read as zero (approximation)\n",
                          (hwaddr)(0x14000000ULL + offset));
            *value = 0;
            break;
        }
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
        MemTxResult result = crime_transfer_write(s, offset, value, size);

        if (result == MEMTX_OK && s->legacy_software &&
            offset >= 0x1001000 && offset < 0x1004000) {
            crime_ifb_record(s, offset - 0x1000000, value, size);
        }
        return result;
    }
    if (size != 8 || offset & 7) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: invalid %u-byte write at 0x%" HWADDR_PRIx
                      "\n", size, offset);
        return (s->functional_memory_transfer || s->legacy_software) ?
            crime_transfer_fail(offset, "invalid register width or alignment") :
            MEMTX_ERROR;
    }
    if (offset == 0x200 && (value & 2)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: unsupported ECC replacement request "
                      "at 0x14000200 value 0x%" PRIx64 "\n", value);
        return (s->functional_memory_transfer || s->legacy_software) ?
            crime_transfer_fail(offset, "unsupported ECC replacement") :
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
            if (!value) {
                return MEMTX_OK;
            }
            break;
        case 0x0250:
            if (!value) {
                /* The error address latch is kept. */
                s->memory_error_status = 0;
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
        case 0x0050:
            if (!value) {
                /* Public evidence leaves the legacy enable bits unknown. */
                qemu_log_mask(LOG_UNIMP,
                              "sgi-crime: legacy CPU error enable "
                              "%u-byte zero write at 0x14000050 "
                              "accepted as a no-op approximation\n", size);
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
            if (!value) {
                return MEMTX_OK;
            }
            break;
        default:
            break;
        }
    }
    switch (offset) {
    case 0x0270:
        if (value <= UINT8_MAX) {
            s->ecc_replacement = value;
            return MEMTX_OK;
        }
        break;
    case 0x0018:
        if (s->legacy_software && (value >> 32)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: ignoring legacy interrupt-mask upper "
                          "bits 0x%" PRIx32 "\n", (uint32_t)(value >> 32));
            value = (uint32_t)value;
        }
        if (!(value >> 32)) {
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
    memset(&s->render, 0, sizeof(s->render));
    s->healthy_memory_reported = false;
    s->render_mode_reported = 0;
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
    s->ecc_replacement = 0;
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

    if (!s->transfer_state_received || !s->profile_state_received ||
        !s->render_state_received || !s->ecc_state_received) {
        return -EINVAL;
    }
    if ((incoming->hardware & software_mask) ||
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
        profile->memory_error_status != s->incoming_memory_error_status ||
        profile->memory_error_address != s->incoming_memory_error_address) {
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
    s->transfer = s->incoming_transfer;
    s->ecc_replacement = s->incoming_ecc_replacement;
    s->render = s->incoming_render;
    s->healthy_memory_reported = false;
    s->render_mode_reported = 0;
    s->timer_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    crime_update_irq(s);
    return 0;
}

static bool crime_transfer_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;

    s->incoming_transfer = s->transfer;
    s->incoming_memory_error_status = s->memory_error_status;
    s->incoming_memory_error_address = s->memory_error_address;
    return true;
}

static int crime_transfer_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;
    CrimeTransferState *incoming = &s->incoming_transfer;

    if (incoming->ecc_enable > 1 ||
        (!s->legacy_software && incoming->parameter[2]) ||
        (!s->functional_memory_transfer && incoming->ecc_enable) ||
        (s->incoming_memory_error_status &
         ~(CRIME_ERROR_READ | CRIME_ERROR_WRITE | CRIME_ERROR_RMW)) ||
        s->incoming_memory_error_address > 0x3fffffff) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->tlb); i++) {
        if (!s->functional_memory_transfer && incoming->tlb[i]) {
            return -EINVAL;
        }
    }
    if (!s->functional_memory_transfer) {
        for (unsigned i = 0; i < ARRAY_SIZE(incoming->parameter); i++) {
            if (incoming->parameter[i]) {
                return -EINVAL;
            }
        }
    }
    if (!s->legacy_software || !s->functional_memory_transfer) {
        for (unsigned i = 0; i < ARRAY_SIZE(incoming->clip); i++) {
            if (incoming->clip[i]) {
                return -EINVAL;
            }
        }
    }
    s->transfer_state_received = true;
    return 0;
}

static const VMStateDescription vmstate_crime_transfer = {
    .name = TYPE_SGI_CRIME "/linear-transfer",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save_errp = crime_transfer_pre_save,
    .post_load = crime_transfer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(transfer_policy, SGICrimeState),
        VMSTATE_UINT8_EQUAL(interface_revision, SGICrimeState),
        VMSTATE_UINT64_ARRAY(incoming_transfer.tlb, SGICrimeState, 32),
        VMSTATE_UINT32_ARRAY(incoming_transfer.parameter, SGICrimeState, 10),
        VMSTATE_UINT64(incoming_transfer.ecc_enable, SGICrimeState),
        VMSTATE_UINT32(incoming_memory_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_memory_error_address, SGICrimeState),
        VMSTATE_UINT64_ARRAY(incoming_transfer.clip, SGICrimeState, 4),
        VMSTATE_END_OF_LIST()
    },
};

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
    .pre_save_errp = crime_profile_pre_save,
    .post_load = crime_profile_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(profile_id, SGICrimeState),
        VMSTATE_UINT8_EQUAL(transfer_policy, SGICrimeState),
        VMSTATE_UINT8_EQUAL(configuration, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.inputs, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.latched, SGICrimeState),
        VMSTATE_UINT64(incoming_profile.cpu_error_address, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.cpu_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.memory_error_status, SGICrimeState),
        VMSTATE_UINT32(incoming_profile.memory_error_address, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static bool crime_render_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;

    s->incoming_render = s->render;
    return true;
}

static int crime_render_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;
    CrimeRenderState *incoming = &s->incoming_render;
    bool active = s->legacy_software && s->functional_memory_transfer;

    for (unsigned i = 0; i < ARRAY_SIZE(incoming->tlb); i++) {
        if (!active && incoming->tlb[i]) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->texture); i++) {
        if (!active && incoming->texture[i]) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->parameter); i++) {
        const CrimeRenderRegister *reg = crime_render_register(i * 4);
        uint64_t value = incoming->parameter[i];

        if ((!active && value) || (!reg && value) ||
            (reg && reg->size == 4 && value >> 32)) {
            return -EINVAL;
        }
    }
    if (incoming->ifb_head >= 64 || (!active && incoming->ifb_head)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->ifb_data); i++) {
        if (!active && (incoming->ifb_data[i] || incoming->ifb_address[i])) {
            return -EINVAL;
        }
    }
    s->render_state_received = true;
    return 0;
}

static const VMStateDescription vmstate_crime_render = {
    .name = TYPE_SGI_CRIME "/renderer",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save_errp = crime_render_pre_save,
    .post_load = crime_render_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(incoming_render.tlb, SGICrimeState, 192),
        VMSTATE_UINT64_ARRAY(incoming_render.parameter, SGICrimeState, 128),
        VMSTATE_UINT64_ARRAY(incoming_render.texture, SGICrimeState, 28),
        VMSTATE_UINT64_ARRAY(incoming_render.ifb_data, SGICrimeState, 64),
        VMSTATE_UINT64_ARRAY(incoming_render.ifb_address, SGICrimeState, 64),
        VMSTATE_UINT8(incoming_render.ifb_head, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static bool crime_ecc_pre_save(void *opaque, Error **errp)
{
    SGICrimeState *s = opaque;

    s->incoming_ecc_replacement = s->ecc_replacement;
    return true;
}

static int crime_ecc_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;

    if (s->incoming_ecc_replacement > UINT8_MAX) {
        return -EINVAL;
    }
    s->ecc_state_received = true;
    return 0;
}

static const VMStateDescription vmstate_crime_ecc = {
    .name = TYPE_SGI_CRIME "/ecc-replacement",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save_errp = crime_ecc_pre_save,
    .post_load = crime_ecc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(incoming_ecc_replacement, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static int crime_pre_load(void *opaque)
{
    SGICrimeState *s = opaque;

    s->transfer_state_received = false;
    s->profile_state_received = false;
    s->render_state_received = false;
    s->ecc_state_received = false;
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
        &vmstate_crime_render,
        &vmstate_crime_ecc,
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
    s->configuration = s->empty_renderer_status | (s->prom_id_word << 1) |
                       (s->eight_128m_banks << 2) |
                       (s->reset_low_ram_approx << 3);
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
