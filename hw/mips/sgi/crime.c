/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI CRIME processor interfaces and MACE interrupts.
 * The default 1.5 interface has an explicit legacy software alternative.
 * An opt-in standalone profile decodes eight logical 128 MiB RAM banks.
 * An opt-in synchronous linear transfer approximation uses healthy RAM.
 * The legacy functional profile includes synchronous tiled 2D rendering.
 * Physical ECC, watchdog, 3D rendering and VICE are not modeled.
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
#include "qemu/bswap.h"
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
    { 0x0c0, 4, true }, { 0x0c4, 4, true },
    { 0x0d0, 4, false }, { 0x0d8, 4, true },
    { 0x1b0, 4, true }, { 0x1b8, 4, true },
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

static bool crime_render_state_access(hwaddr offset, unsigned size)
{
    if (crime_renderer_touches(offset, size, 0x1001000, 0x600)) {
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
                                                      uint64_t *physical,
                                                      uint64_t *identity)
{
    if (x >= 2048) {
        return CRIME_RENDER_TRANSLATION_ERROR;
    }
    return crime_framebuffer_translate(s, bank, (uint64_t)x * bytes, y, bytes,
                                        physical, identity);
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
    g_autofree uint32_t *order = g_new(uint32_t, count);

    *overlap = false;
    if (!count) {
        return true;
    }
    for (unsigned i = 0; i < count; i++) {
        order[i] = i;
    }
    g_qsort_with_data(order, count, sizeof(*order),
                      crime_render_compare_destination, pixels);
    for (unsigned i = 0; i < count; i++) {
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
            unsigned lo = 0, hi = count;

            while (lo < hi) {
                unsigned mid = lo + (hi - lo) / 2;
                uint64_t destination = pixels[order[mid]].destination_id;

                if (destination < pixels[i].source_id) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo < count && pixels[order[lo]].destination_id ==
                              pixels[i].source_id) {
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

static bool crime_render_mode(uint32_t mode, unsigned *bank, unsigned *bytes)
{
    *bank = mode >> 10;
    if (*bank >= 3) {
        return false;
    }
    switch (mode & 0x3ff) {
    case 0:
        *bytes = 1;
        return true;
    case 0x124:
        *bytes = 2;
        return true;
    case 0x228:
        *bytes = 4;
        return true;
    default:
        return false;
    }
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
} CrimeRenderGeometry;

static bool crime_render_geometry(const uint64_t *p, CrimeRenderGeometry *g)
{
    uint32_t primitive = p[0x60 / 4];

    g->opcode = primitive >> 24;
    g->x0 = p[0x70 / 4] >> 16;
    g->y0 = p[0x70 / 4] & 0xffff;
    if (g->x0 >= 2048 || g->y0 >= 2048) {
        return false;
    }
    if (g->opcode <= 1) {
        if (primitive & 0x00f80000) {
            return false;
        }
        g->samples = g->capacity = g->thickness = 1;
        if (!g->opcode) {
            return true;
        }
    } else if ((primitive & ~0x30000U) != 0x3000000) {
        return false;
    }
    g->x1 = p[0x74 / 4] >> 16;
    g->y1 = p[0x74 / 4] & 0xffff;
    if (g->x1 >= 2048 || g->y1 >= 2048) {
        return false;
    }
    if (g->opcode == 1) {
        unsigned ax = abs(g->x1 - g->x0), ay = abs(g->y1 - g->y0);

        g->steps = MAX(ax, ay);
        g->samples = g->steps + 1 - !!(primitive & 0x40000);
        g->thickness = MAX(1, DIV_ROUND_UP(primitive & 0xffff, 32));
        g->lower = -((int)(g->thickness - 1) / 2);
        g->xmajor = ax >= ay;
        g->capacity = g->samples * g->thickness;
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
    if (g->opcode == 3) {
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

static MemTxResult crime_render_de_commit(SGICrimeState *s, hwaddr offset,
                                           unsigned index, uint64_t value,
                                           unsigned words)
{
    uint64_t p[128];
    uint32_t draw, clip, plane, stipple_mode;
    unsigned destination_bank, source_bank = 0, bytes, source_bytes;
    unsigned count = 0, suppressed = 0, operation;
    unsigned bit = 0, repeat = 0, first = 0, last = 0, max_repeat = 0;
    int sx, sy;
    bool copy, stipple, overlap, screen_enabled = false, inside = false;
    CrimeRenderClip scissor, screen = { 0 };
    CrimeRenderGeometry geometry = { 0 };
    g_autofree CrimeRenderPixel *pixels = NULL;
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
    if ((draw & ~0x3e02f8U) || p[0x50 / 4] || p[0x58 / 4] ||
        !crime_render_geometry(p, &geometry)) {
        return crime_transfer_fail(offset, "unsupported primitive state");
    }
    stipple = (geometry.opcode == 1 && (draw & 0x80000)) ||
              (geometry.opcode == 3 && (draw & 0x40000));
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
    if (!crime_render_mode(p[0x8 / 4], &destination_bank, &bytes)) {
        return crime_transfer_fail(offset, "unsupported destination mode");
    }
    if (copy &&
        (!crime_render_mode(p[0], &source_bank, &source_bytes) ||
         source_bytes != bytes || p[0xa8 / 4] != 1 || p[0xac / 4] != 1)) {
        return crime_transfer_fail(offset, "unsupported rectangle source");
    }
    operation = draw & 0x200 ? p[0x1b0 / 4] : 3;
    if ((draw & 0x200) && p[0x1b0 / 4] > 15) {
        return crime_transfer_fail(offset, "unsupported raster operation");
    }
    if (clip & ~0x3ffU) {
        return crime_transfer_fail(offset, "unsupported clip mode");
    }
    for (unsigned n = 0; n < 5; n++) {
        unsigned enable = 1U << (9 - n), select = 1U << (4 - n);

        if (clip & enable) {
            if (screen_enabled || (clip & ~(enable | select))) {
                return crime_transfer_fail(offset, "multiple screen masks");
            }
            screen_enabled = true;
            inside = clip & select;
            screen = crime_render_clip(p[(0x20 + 8 * n) / 4]);
        }
    }
    if ((clip && !screen_enabled) ||
        (screen_enabled && (screen.xmin > screen.xmax ||
                            screen.ymin > screen.ymax))) {
        return crime_transfer_fail(offset, "invalid screen mask");
    }
    scissor = crime_render_clip(p[0x48 / 4]);
    if ((draw & 0x100000) &&
        (scissor.xmin > scissor.xmax || scissor.ymin > scissor.ymax)) {
        return crime_transfer_fail(offset, "invalid scissor");
    }
    sx = p[0xa0 / 4] >> 16;
    sy = p[0xa0 / 4] & 0xffff;
    if (copy && (sx >= 2048 || sy >= 2048 ||
                 sx + geometry.x1 - geometry.x0 < 0 ||
                 sx + geometry.x1 - geometry.x0 >= 2048 ||
                 sy + geometry.y1 - geometry.y0 < 0 ||
                 sy + geometry.y1 - geometry.y0 >= 2048)) {
        return crime_transfer_fail(offset, "invalid source rectangle");
    }
    plane = draw & 0x80 ? p[0x1b8 / 4] : UINT32_MAX;
    plane &= UINT32_MAX >> (32 - 8 * bytes);
    pixels = g_new(CrimeRenderPixel, geometry.capacity);
    for (unsigned i = 0; i < geometry.samples; i++) {
        int cx, cy;
        unsigned stripe_x = 1, stripe_y = 1;
        uint32_t color = p[0xd0 / 4];
        bool foreground = !stipple || (p[0xc4 / 4] & (1U << (31 - bit)));

        if (stipple) {
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
        crime_render_sample(&geometry, i, &cx, &cy);
        if (geometry.opcode == 1) {
            if (!geometry.steps || !geometry.xmajor) {
                stripe_x = geometry.thickness;
                cx += geometry.lower;
            }
            if (!geometry.steps || geometry.xmajor) {
                stripe_y = geometry.thickness;
                cy += geometry.lower;
            }
        }
        for (unsigned row = 0; row < stripe_y; row++) {
            int y = cy + (int)row;

            for (unsigned col = 0; col < stripe_x; col++) {
                int x = cx + (int)col;
                uint32_t mask = plane;
                CrimeRenderPixel *pixel;
                CrimeRenderTranslation translation;

                if (x < 0 || x >= 2048 || y < 0 || y >= 2048 ||
                    ((draw & 0x100000) &&
                     !crime_render_inside(&scissor, x, y)) ||
                    (screen_enabled &&
                     crime_render_inside(&screen, x, y) != inside)) {
                    continue;
                }
                for (unsigned b = 0; b < bytes; b++) {
                    if (!(draw & (1U << (6 - ((x * bytes + b) & 3))))) {
                        mask &= ~(0xffU << (8 * (bytes - 1 - b)));
                    }
                }
                if (!mask) {
                    continue;
                }
                pixel = &pixels[count];
                translation = crime_render_translate(
                    s, destination_bank, x, y, bytes,
                    &pixel->destination, &pixel->destination_id);

                if (!copy && translation == CRIME_RENDER_INVALID_TILE) {
                    suppressed++;
                    continue;
                }
                if (translation != CRIME_RENDER_MAPPED ||
                    (copy && crime_render_translate(
                        s, source_bank, sx + x - geometry.x0,
                        sy + y - geometry.y0, bytes,
                        &pixel->source, &pixel->source_id) !=
                        CRIME_RENDER_MAPPED)) {
                    return crime_transfer_fail(offset,
                                               "invalid primitive mapping");
                }
                pixel->mask = mask;
                pixel->color = color;
                count++;
            }
        }
    }
    if (!crime_render_validate(pixels, count, bytes, false, &overlap)) {
        return crime_transfer_fail(offset, "destination aliases itself");
    }
    /* MMIO holds the BQL, keeping prevalidated RAM mappings stable. */
    result = crime_render_execute(offset, pixels, count, bytes, copy,
                                   operation, true);
    if (result == MEMTX_OK) {
        for (unsigned i = 0; i < words; i++) {
            s->render.parameter[index + i] = p[index + i];
        }
        if (suppressed) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: %s suppressed %u invalid destination "
                          "fragments\n", geometry.opcode == 0 ? "point" :
                          geometry.opcode == 1 ? "line" : "rectangle",
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
    bool copy = mode & 0x800, overlap, unique = true;
    int dy = p[9] == 1 ? 1 : -1;
    int dx0 = p[6] >> 16, dx1 = p[7] >> 16;
    int dy0 = p[6] & 0xffff, dy1 = p[7] & 0xffff;
    int sx0 = p[4] >> 16, sx1 = p[5] >> 16;
    int sy0 = p[4] & 0xffff, sy1 = p[5] & 0xffff;
    unsigned width, height, count = 0, enabled = 0;
    uint64_t bases[256] = { 0 };
    bool seen[256] = { false };
    g_autofree CrimeRenderPixel *pixels = NULL;

    if ((mode & ~0x8ffU) || destination_bank >= 3 ||
        (copy && source_bank >= 3) ||
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

        if (p[1] & (0x80000000U >> (x & 31))) {
            pixels[enabled++] = pixels[i];
        }
    }
    return crime_render_execute(offset, pixels, enabled, 1, copy, 3, false);
}

static MemTxResult crime_render_state_read(SGICrimeState *s, hwaddr offset,
                                            uint64_t *value, unsigned size)
{
    const CrimeRenderRegister *reg;

    if (offset >= 0x1001000 && offset < 0x1001600 &&
        size == 8 && !(offset & 7)) {
        *value = s->render.tlb[(offset - 0x1001000) / 8];
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

    if (offset >= 0x1001000 && offset < 0x1001600 &&
        size == 8 && !(offset & 7)) {
        s->render.tlb[(offset - 0x1001000) / 8] = value;
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

static MemTxResult crime_transfer_read(SGICrimeState *s, hwaddr offset,
                                       uint64_t *value, unsigned size)
{
    unsigned index;

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
    return crime_renderer_unsupported(s, offset, size, false, 0);
}

static MemTxResult crime_transfer_write(SGICrimeState *s, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    unsigned index;

    if (s->legacy_software) {
        const CrimeRenderRegister *reg =
            crime_render_register(offset - 0x1002800);

        if (offset >= CRIME_CLIP_TLB && offset < CRIME_CLIP_TLB + 32 &&
            size == 8 && !(offset & 7)) {
            s->transfer.clip[(offset - CRIME_CLIP_TLB) / 8] = value;
            return MEMTX_OK;
        }
        if (offset == 0x1000400 && size == 4) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-crime: opaque empty interface-buffer control "
                          "(unimplemented fields) %u-byte write at "
                          "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n", size,
                          (hwaddr)(0x14000000ULL + offset), value);
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
        if (reg) {
            bool paired = size == 8 &&
                (reg->offset == 0x070 || reg->offset == 0x0a8 ||
                 reg->offset == 0x0c0);

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
        *value = s->ecc_replacement;
        break;
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
        case 0x0250:
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
    for (unsigned i = 0; i < ARRAY_SIZE(incoming->parameter); i++) {
        const CrimeRenderRegister *reg = crime_render_register(i * 4);
        uint64_t value = incoming->parameter[i];

        if ((!active && value) || (!reg && value) ||
            (reg && reg->size == 4 && value >> 32)) {
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
