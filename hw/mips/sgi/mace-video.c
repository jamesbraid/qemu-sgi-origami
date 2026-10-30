/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * MACE revision-2 decoded video and linear YUV8 DMA backend.
 * Sources supply labelled virtual-time fields, lines and pixel pairs.
 * Memory service is synchronous coherent RAM; staging is not a physical FIFO.
 * Raw D1 decoding, physical frontends/wiring, filters, tiled/mipmap storage,
 * ECC and asynchronous finite buffering are not implemented.
 * The isolated qtest fixture requires an explicit synthetic revision.
 */

#include "qemu/osdep.h"
#include "mace-video.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/rcu.h"
#include "qemu/host-utils.h"
#include "system/dma.h"

enum {
    VIDEO_OUTPUT_SOF = 16,
    VIDEO_OUTPUT_LINE,
    VIDEO_OUTPUT_SAMPLE,
    VIDEO_OUTPUT_EOF,
};

static void video_queue(MACEVideo *s, const MACEVideoEvent *event);
static void video_event(MACEVideo *s, const MACEVideoEvent *event);
static bool output_raster(MACEVideoChannel *c, unsigned bank,
                           MACEVideoOutput *r);

static void video_irq(MACEVideo *s);

static void video_abort(MACEVideoChannel *c, uint32_t cause)
{
    c->reg[1] |= cause;
    c->active.valid = false;
    c->active.count = 0;
    c->halted = true;
}

static bool input_sync(MACEVideo *s, unsigned channel)
{
    uint64_t cfg = s->channel[channel].reg[2];
    unsigned source = (cfg >> 2) & 3;

    return (cfg & 2) && s->lock[source] &&
        (source >= 2 || (s->channel[2].reg[11] & (1U << (source * 2))));
}

static bool video_span(MACEVideoActive *a, uint32_t offset)
{
    uint64_t last;

    if (!a->width || !a->lines) {
        return false;
    }
    last = (uint64_t)offset + (uint64_t)(a->lines - 1) * a->width *
           (a->interleaved ? 2 : 1) + a->width - 1;
    return last < 32 * 65536;
}

static bool input_geometry(MACEVideoChannel *c, unsigned bank,
                            MACEVideoActive *a)
{
    uint64_t h = c->reg[bank ? 9 : 6];
    uint64_t v = c->reg[bank ? 10 : 7];
    unsigned hs = h & 0x3ff, he = (h >> 10) & 0x3ff;
    unsigned vs = v & 0x3ff, ve = (v >> 20) & 0x3ff;
    unsigned black = (v >> 10) & 0x3ff;

    if (((c->reg[2] >> 10) & 7) != 2 || (c->reg[2] & 0xe3f0) ||
        (h >> 38) || (v >> 38) || he < hs || ve < vs || !vs ||
        ((hs + 2) & 1) || ((he - hs + 1) & 3) ||
        (black && (black < vs || black > ve))) {
        return false;
    }
    a->width = c->reg[5] & 0xff8;
    a->lines = ve - vs + 1;
    a->first_x = hs + 2;
    a->first_y = vs;
    a->black_end = black;
    a->interleaved = c->reg[2] & (1U << 16);
    return a->width && a->width == 2 * (he - hs + 1);
}

static bool video_dma(MACEVideo *s, uint32_t addr, uint8_t *data,
                       unsigned bytes, bool write)
{
    hwaddr current = addr, remain = bytes;
    bool valid = addr >= 0x40000000 && addr < 0xc0000000 &&
                 bytes <= 0xc0000000ULL - addr;
    MemTxResult result;

    RCU_READ_LOCK_GUARD();
    while (valid && remain) {
        hwaddr translated, length = remain;
        MemoryRegion *mr = address_space_translate(s->as, current, &translated,
                &length, write, MEMTXATTRS_UNSPECIFIED);

        if (!length || !memory_region_is_ram(mr) ||
            memory_region_is_rom(mr) ||
            !address_space_access_valid(s->as, current, length, write,
                                        MEMTXATTRS_UNSPECIFIED)) {
            uint8_t ignored = 0;

            if (!memory_region_is_ram(mr)) {
                /* Reach the decoder's error responder without partial RAM. */
                if (write) {
                    dma_memory_write(s->as, current, &ignored, 1,
                                     MEMTXATTRS_UNSPECIFIED);
                } else {
                    dma_memory_read(s->as, current, &ignored, 1,
                                    MEMTXATTRS_UNSPECIFIED);
                }
            }
            valid = false;
            break;
        }
        current += length;
        remain -= length;
    }
    if (!valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-video: inaccessible DMA at 0x%08x size %u\n",
                      addr, bytes);
        return false;
    }
    result = write ? dma_memory_write(s->as, addr, data, bytes,
                                      MEMTXATTRS_UNSPECIFIED) :
                     dma_memory_read(s->as, addr, data, bytes,
                                     MEMTXATTRS_UNSPECIFIED);
    return result == MEMTX_OK;
}

static bool video_unit(MACEVideo *s, MACEVideoChannel *c, bool write)
{
    MACEVideoActive *a = &c->active;
    unsigned page = a->cursor >> 16;
    uint32_t addr;

    if (page >= 32 || !a->page[page]) {
        video_abort(c, 1U << 4);
        return false;
    }
    addr = ((uint32_t)a->page[page] << 16) | (a->cursor & 0xffff);
    if (!video_dma(s, addr, a->unit, 8, write)) {
        video_abort(c, 1U << 8);
        return false;
    }
    return true;
}

static void video_admit(MACEVideo *s, unsigned channel,
                         const MACEVideoEvent *e)
{
    MACEVideoChannel *c = &s->channel[channel];
    MACEVideoActive next = { 0 };
    uint8_t table[64];
    uint32_t descriptor = c->reg[3];
    unsigned selector = descriptor & 3;

    if (!(c->reg[0] & c->reg[2] & 1) || c->halted || c->active.valid ||
        !(descriptor & 4)) {
        return;
    }
    if (channel < 2) {
        if ((selector == 2 && e->field) || (selector == 3 && !e->field)) {
            return;
        }
        if (!input_geometry(c, e->field, &next) ||
            next.first_x + next.width / 2 > e->x || e->y < 2 ||
            next.first_y + next.lines > e->y - 1) {
            goto unsupported;
        }
    } else {
        MACEVideoOutput *r = &s->output;
        unsigned cfg = c->reg[2];

        next.width = c->reg[5] & 0xff8;
        next.lines = (c->reg[5] >> 12) & 0x3ff;
        next.interleaved = cfg & (1U << 20);
        next.first_x = r->image_x;
        next.first_y = r->image_y;
        if ((descriptor & 1) != e->field || !next.width || !next.lines ||
            next.first_x + next.width / 2 > r->pixels ||
            next.first_y + next.lines > r->trailing) {
            goto unsupported;
        }
    }
    next.cursor = c->reg[4];
    if (!video_span(&next, next.cursor)) {
        goto unsupported;
    }
    c->reg[3] &= ~4ULL;
    if (!video_dma(s, descriptor & 0xffffffc0U, table, 64, false)) {
        video_abort(c, 1U << 8);
        return;
    }
    for (unsigned i = 0; i < 32; i++) {
        next.page[i] = lduw_be_p(table + 2 * i);
    }
    for (unsigned i = 0; i < 8; i++) {
        c->reg[16 + i] = ldq_be_p(table + 8 * i);
    }
    next.valid = true;
    c->active = next;
    return;

unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace-video: unsupported channel %u field geometry\n",
                  channel);
}

static void input_sample(MACEVideo *s, MACEVideoChannel *c,
                          const MACEVideoEvent *e)
{
    MACEVideoActive *a = &c->active;
    uint16_t sample;

    if (!a->valid || e->x < a->first_x ||
        e->x >= a->first_x + a->width / 2 || e->y < a->first_y ||
        e->y >= a->first_y + a->lines) {
        return;
    }
    if (e->y != a->first_y + a->row ||
        e->x != a->first_x + a->column / 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-video: missing or out-of-order input pixel\n");
        video_abort(c, 0);
        return;
    }
    sample = e->y < a->black_end ? 0x8010 :
             e->x & 1 ? e->sample : e->sample >> 16;
    stw_be_p(a->unit + a->count, sample);
    a->count += 2;
    a->column += 2;
    if (a->count < 8) {
        return;
    }
    if (!video_unit(s, c, true)) {
        return;
    }
    a->count = 0;
    a->cursor += 8;
    if (a->column == a->width) {
        a->column = 0;
        a->row++;
        if (a->row < a->lines && a->interleaved) {
            a->cursor += a->width;
        }
    }
    if (a->row == a->lines) {
        a->valid = false;
        c->reg[1] |= 4;
    }
}

static unsigned event_priority(unsigned type)
{
    switch (type) {
    case MACE_VIDEO_LOSS:
        return 0;
    case MACE_VIDEO_LOCK:
        return 1;
    case MACE_VIDEO_LINE:
    case VIDEO_OUTPUT_LINE:
        return 2;
    case MACE_VIDEO_SAMPLE:
    case VIDEO_OUTPUT_SAMPLE:
        return 3;
    case MACE_VIDEO_EOF:
    case VIDEO_OUTPUT_EOF:
        return 4;
    default:
        return 5;
    }
}

static void video_schedule(MACEVideo *s)
{
    MACEVideoEvent *e = QTAILQ_FIRST(&s->pending);

    if (e) {
        timer_mod_ns(s->timer, e->time);
    } else {
        timer_del(s->timer);
    }
}

bool mace_video_enqueue(MACEVideo *s, const MACEVideoEvent *event)
{
    /* Endpoints submit complete timestamp groups before their deadline. */
    if (event->time <= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) ||
        (event->port != 0 && event->port != 1 && event->port != 4) ||
        event->field > 1 || event->type > MACE_VIDEO_LINE) {
        return false;
    }
    video_queue(s, event);
    return true;
}

static void video_queue(MACEVideo *s, const MACEVideoEvent *event)
{
    MACEVideoEvent *e, *before;

    e = g_new(MACEVideoEvent, 1);
    *e = *event;
    before = QTAILQ_LAST(&s->pending);
    if (before && (e->time > before->time ||
        (e->time == before->time && event_priority(e->type) >=
                                  event_priority(before->type)))) {
        QTAILQ_INSERT_TAIL(&s->pending, e, next);
        video_schedule(s);
        return;
    }
    if (before && e->time == before->time) {
        while (before && e->time == before->time &&
               event_priority(e->type) < event_priority(before->type)) {
            before = QTAILQ_PREV(before, next);
        }
        if (before) {
            QTAILQ_INSERT_AFTER(&s->pending, before, e, next);
        } else {
            QTAILQ_INSERT_HEAD(&s->pending, e, next);
        }
        video_schedule(s);
        return;
    }
    QTAILQ_FOREACH(before, &s->pending, next) {
        if (e->time < before->time ||
            (e->time == before->time && event_priority(e->type) <
                                      event_priority(before->type))) {
            QTAILQ_INSERT_BEFORE(before, e, next);
            video_schedule(s);
            return;
        }
    }
    QTAILQ_INSERT_TAIL(&s->pending, e, next);
    video_schedule(s);

}

static void video_clear_events(MACEVideo *s)
{
    MACEVideoEvent *e;

    while ((e = QTAILQ_FIRST(&s->pending))) {
        QTAILQ_REMOVE(&s->pending, e, next);
        g_free(e);
    }
}

void mace_video_cleanup(MACEVideo *s)
{
    video_clear_events(s);
    if (s->timer) {
        timer_free(s->timer);
        s->timer = NULL;
    }
}

static bool output_raster(MACEVideoChannel *c, unsigned bank,
                           MACEVideoOutput *r)
{
    uint64_t h = c->reg[bank ? 8 : 6];
    uint64_t v = c->reg[bank ? 9 : 7];

    r->pixels = 2 * (((h >> 10) & 0x3ff) + 1);
    r->blank = 2 * ((h & 0x3ff) + 2);
    r->image_x = 2 * (((h >> 20) & 0x3ff) + 1);
    r->leading = (v & 0x3ff) + 1;
    r->trailing = ((v >> 10) & 0x3ff) + 1;
    r->lines = ((v >> 20) & 0x3ff) + 1;
    r->image_y = ((v >> 30) & 0x3ff) + 1;
    return !(h >> 40) && (h & 0x3ff) != 0x3ff &&
           r->blank <= r->image_x && r->image_x < r->pixels &&
           r->leading <= r->image_y && r->image_y < r->trailing &&
           r->trailing <= r->lines;
}

static bool output_deadline(int64_t origin, uint64_t ticks,
                            uint32_t num, uint32_t den, int64_t *deadline)
{
    uint64_t low, high, rem;

    if (!den || origin < 0) {
        return false;
    }
    mulu64(&low, &high, ticks, num);
    rem = divu128(&low, &high, (uint64_t)den * 2);
    if (high || low > (uint64_t)(INT64_MAX - origin) ||
        (rem && low == (uint64_t)(INT64_MAX - origin))) {
        return false;
    }
    *deadline = origin + low + !!rem;
    return true;
}

static void output_queue(MACEVideo *s, unsigned type, unsigned x, unsigned y)
{
    MACEVideoOutput *o = &s->output;
    MACEVideoEvent e = {
        .type = type, .x = x, .y = y, .sequence = o->sequence,
        .generation = o->generation,
    };
    uint64_t index = type == VIDEO_OUTPUT_EOF ?
                     (uint64_t)o->lines * o->pixels :
                     (uint64_t)y * o->pixels + x;

    if (output_deadline(o->origin, o->base_ticks + index * 2,
                        o->period_num, o->period_den, &e.time)) {
        video_queue(s, &e);
    }
}

static void output_emit(MACEVideo *s, MACEVideoEvent *e)
{
    MACEVideoChannel *c = &s->channel[2];

    for (unsigned port = 0; port < 2; port++) {
        MACEVideoEvent loop = *e;

        if (((c->reg[2] >> (1 + 2 * port)) & 3) != 2) {
            continue;
        }
        e->port = 2 + port;
        if (s->emit) {
            s->emit(s->opaque, e);
        }
        loop.port = 2 + port;
        if (loop.type == MACE_VIDEO_SOF || loop.type == MACE_VIDEO_LINE) {
            loop.x -= 2;
        } else if (loop.type == MACE_VIDEO_SAMPLE) {
            if (loop.x < 2) {
                continue;
            }
            loop.x -= 2;
        }
        video_event(s, &loop);
    }
}

static void output_loss(MACEVideo *s, int64_t time, bool reference)
{
    MACEVideoChannel *c = &s->channel[2];
    MACEVideoEvent e = { .time = time, .type = MACE_VIDEO_LOSS };

    s->output.generation++;
    if (reference && s->output.locked) {
        c->reg[1] |= 8;
        if (c->active.valid) {
            video_abort(c, 8);
        }
    }
    c->field_open = false;
    s->output.locked = false;
    output_emit(s, &e);
}

static void output_clock(MACEVideo *s, const MACEVideoEvent *reference)
{
    MACEVideoChannel *c = &s->channel[2];
    MACEVideoOutput *o = &s->output;
    MACEVideoEvent e = {
        .type = VIDEO_OUTPUT_SOF, .field = 0, .origin =
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .period_num = 2000, .period_den = 27,
        .generation = o->generation, .sequence = ++o->next_sequence,
    };
    MACEVideoOutput r = { 0 };
    unsigned ref = (c->reg[2] >> 6) & 3;

    if (reference) {
        if (!reference->period_num || !reference->period_den ||
            reference->period_num > UINT32_MAX ||
            reference->period_den > UINT32_MAX ||
            reference->period_num * 3 < reference->period_den * 200 ||
            !output_raster(c, reference->field, &r) ||
            r.pixels != reference->x || r.lines != reference->y) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-video: unsupported reference timing\n");
            return;
        }
        e.origin = reference->time;
        e.period_num = reference->period_num;
        e.period_den = reference->period_den;
        e.field = reference->field;
        e.ticks = (ref == 1 ? 10 : 15) + c->reg[10];
    }
    if (output_deadline(e.origin, e.ticks, e.period_num, e.period_den,
                        &e.time)) {
        video_queue(s, &e);
    }
}

static void output_event(MACEVideo *s, const MACEVideoEvent *event)
{
    MACEVideoChannel *c = &s->channel[2];
    MACEVideoOutput *o = &s->output;
    MACEVideoEvent e = *event;
    MACEVideoActive *a = &c->active;

    if (!(c->reg[2] & 32) || e.generation != o->generation) {
        return;
    }
    e.field = c->field;
    if (event->type == VIDEO_OUTPUT_SOF) {
        int64_t end;

        if (!output_raster(c, event->field, o) ||
            !output_deadline(event->origin, event->ticks +
                              (uint64_t)o->pixels * o->lines * 2,
                              event->period_num, event->period_den, &end)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-video: unsupported output raster\n");
            return;
        }
        if (a->valid) {
            video_abort(c, 0);
        }
        o->origin = event->origin;
        o->period_num = event->period_num;
        o->period_den = event->period_den;
        o->base_ticks = event->ticks;
        o->sequence = event->sequence;
        o->locked = true;
        c->sequence = event->sequence;
        c->field_open = true;
        c->field = event->field;
        c->ust = s->get_ust(s->opaque, event->time);
        c->reg[1] |= 2;
        s->lock[2] = ((c->reg[2] >> 1) & 3) == 2;
        s->lock[3] = ((c->reg[2] >> 3) & 3) == 2;
        e.field = c->field;
        e.type = MACE_VIDEO_SOF;
        e.x = o->pixels;
        e.y = o->lines;
        video_admit(s, 2, &e);
        output_emit(s, &e);
        e.type = MACE_VIDEO_LINE;
        e.y = 0;
        output_emit(s, &e);
        output_queue(s, VIDEO_OUTPUT_LINE, 0, 1);
        output_queue(s, VIDEO_OUTPUT_EOF, 0, 0);
        return;
    }
    if (!c->field_open || e.sequence != o->sequence) {
        return;
    }
    if (event->type == VIDEO_OUTPUT_LINE) {
        e.type = MACE_VIDEO_LINE;
        e.x = o->pixels;
        output_emit(s, &e);
        if (e.y >= o->leading && e.y < o->trailing) {
            output_queue(s, VIDEO_OUTPUT_SAMPLE, o->blank, e.y);
        }
        if (e.y + 1 < o->lines) {
            output_queue(s, VIDEO_OUTPUT_LINE, 0, e.y + 1);
        }
    } else if (event->type == VIDEO_OUTPUT_SAMPLE) {
        bool image = a->valid && e.y >= a->first_y &&
            e.y < a->first_y + a->lines && e.x >= a->first_x &&
            e.x < a->first_x + a->width / 2;

        e.type = MACE_VIDEO_SAMPLE;
        e.sample = 0x80108010U;
        if (image && !a->count) {
            if (video_unit(s, c, false)) {
                a->count = 4;
            } else {
                image = false;
            }
        }
        if (image) {
            e.sample = ldl_be_p(a->unit + ((4 - a->count) / 2) * 4);
        }
        output_emit(s, &e);
        if (image) {
            a->count--;
            a->column += 2;
            if (!a->count) {
                a->cursor += 8;
            }
            if (a->column == a->width) {
                a->column = 0;
                a->row++;
                if (a->row < a->lines && a->interleaved) {
                    a->cursor += a->width;
                }
            }
            if (a->row == a->lines) {
                a->valid = false;
                c->reg[1] |= 4;
            }
        }
        if (e.x + 1 < o->pixels) {
            output_queue(s, VIDEO_OUTPUT_SAMPLE, e.x + 1, e.y);
        }
    } else if (event->type == VIDEO_OUTPUT_EOF) {
        c->msc += ((c->msc + 1) & 1) == c->field ? 1 : 2;
        c->field_open = false;
        if (a->valid) {
            video_abort(c, 0);
        }
        e.type = MACE_VIDEO_EOF;
        e.y = o->lines;
        output_emit(s, &e);
        if (!((c->reg[2] >> 6) & 3)) {
            e.type = VIDEO_OUTPUT_SOF;
            e.field ^= 1;
            e.origin = o->origin;
            e.period_num = o->period_num;
            e.period_den = o->period_den;
            e.ticks = o->base_ticks + (uint64_t)o->pixels * o->lines * 2;
            e.sequence = ++o->next_sequence;
            video_queue(s, &e);
        }
    }
}

static void video_event(MACEVideo *s, const MACEVideoEvent *e)
{
    bool was_sync[2] = { input_sync(s, 0), input_sync(s, 1) };

    if (e->type <= MACE_VIDEO_LOCK) {
        s->lock[e->port] = e->type == MACE_VIDEO_LOCK;
    }
    if (e->port < 2 || e->port == 4) {
        MACEVideoChannel *out = &s->channel[2];
        unsigned ref = (out->reg[2] >> 6) & 3;
        unsigned port = ref == 1 ? 4 : ref == 2 ? 0 : 1;
        bool enabled = port == 4 ||
            (s->channel[2].reg[11] & (1U << (2 * port)));

        if ((out->reg[2] & 32) && ref && port == e->port) {
            if (e->type == MACE_VIDEO_LOSS) {
                output_loss(s, e->time, true);
            } else if (e->type == MACE_VIDEO_SOF && s->lock[port] && enabled) {
                output_clock(s, e);
            }
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        MACEVideoChannel *c = &s->channel[i];

        if (((c->reg[2] >> 2) & 3) != e->port) {
            continue;
        }
        if (e->type == MACE_VIDEO_LOSS) {
            if (was_sync[i]) {
                c->reg[1] |= 8;
            }
            if (c->active.valid) {
                video_abort(c, 8);
            }
            c->field_open = false;
        } else if (input_sync(s, i)) {
            if (e->type == MACE_VIDEO_SOF) {
                if (c->active.valid) {
                    video_abort(c, 0);
                }
                c->ust = s->get_ust(s->opaque, e->time);
                c->field = e->field;
                c->sequence = e->sequence;
                c->field_open = true;
                c->reg[1] |= 2;
                video_admit(s, i, e);
            } else if (c->field_open && c->sequence == e->sequence) {
                if (e->type == MACE_VIDEO_SAMPLE && c->field == e->field) {
                    input_sample(s, c, e);
                } else if (e->type == MACE_VIDEO_LINE && c->active.valid &&
                           e->x < c->active.first_x + c->active.width / 2 &&
                           e->y >= c->active.first_y &&
                           e->y < c->active.first_y + c->active.lines) {
                    video_abort(c, 1U << 5);
                } else if (e->type == MACE_VIDEO_EOF) {
                    c->msc += ((c->msc + 1) & 1) == c->field ? 1 : 2;
                    c->field_open = false;
                    if (c->active.valid) {
                        video_abort(c, e->y && e->y <= c->active.first_y +
                                      c->active.lines - 1 ? 1U << 6 : 0);
                    }
                }
            }
        }
    }
    video_irq(s);
}

void mace_video_sync(MACEVideo *s)
{
    MACEVideoEvent *e;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while ((e = QTAILQ_FIRST(&s->pending)) && e->time <= now) {
        QTAILQ_REMOVE(&s->pending, e, next);
        if (e->type >= VIDEO_OUTPUT_SOF) {
            output_event(s, e);
            video_irq(s);
        } else {
            video_event(s, e);
        }
        g_free(e);
    }
    video_schedule(s);
}

void mace_video_fifo_fault(MACEVideo *s, unsigned channel)
{
    g_assert(channel < 3);
    mace_video_sync(s);
    video_abort(&s->channel[channel], 1U << 7);
    video_irq(s);
}

static void video_timer(void *opaque)
{
    mace_video_sync(opaque);
}

static void video_irq(MACEVideo *s)
{
    unsigned levels = 0;

    for (unsigned i = 0; i < 3; i++) {
        levels |= !!(s->channel[i].reg[0] & s->channel[i].reg[1] &
                      (i < 2 ? 0x3fe : 0x19e)) << i;
    }
    s->update_irq(s->opaque, levels);
}

static MemTxResult timer_access(MACEVideo *s, hwaddr addr, uint64_t *value,
                                 unsigned size, bool write)
{
    MACEVideoChannel *c;

    if ((size != 4 && size != 8) || (addr & (size - 1)) || addr >= 24 ||
        (addr & 7) + size > 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-video: invalid timer access at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return MEMTX_ERROR;
    }
    mace_video_sync(s);
    c = &s->channel[addr / 8];
    if (write) {
        if (size == 8) {
            c->ust = *value >> 32;
            c->msc = *value;
        } else if (addr & 4) {
            c->msc = *value;
        } else {
            c->ust = *value;
        }
    } else if (size == 8) {
        *value = (uint64_t)c->ust << 32 | c->msc;
    } else {
        *value = addr & 4 ? c->msc : c->ust;
    }
    return MEMTX_OK;
}

static MemTxResult video_timer_read(void *opaque, hwaddr addr, uint64_t *value,
                                    unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return timer_access(opaque, addr, value, size, false);
}

static MemTxResult video_timer_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    return timer_access(opaque, addr, &value, size, true);
}

const MemoryRegionOps mace_video_timer_ops = {
    .read_with_attrs = video_timer_read,
    .write_with_attrs = video_timer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static const uint64_t video_masks[2][12] = {
    { 0x3ff, 0, 0x1ffff, 0xffffffc7, 0xfff8, 0xff8,
      0x1ffffffffff, 0x7fffffffff, 0xff,
      0x1ffffffffff, 0x7fffffffff, 0xff },
    { 0x19f, 0, 0x3ffffff, 0xffffffc5, 0xfff8, 0x3ffff8,
      0x3ffffffffff, 0xffffffffff, 0x3ffffffffff,
      0xffffffffff, 0x7ff, 0x7fffff },
};

static bool video_register(unsigned channel, hwaddr addr, unsigned size)
{
    if (size != 8 || (addr & 7) ||
        !(addr <= 0x58 || (addr >= 0x80 && addr <= 0xb8))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-video: unsupported channel %u access "
                      "at 0x%" HWADDR_PRIx " size %u\n", channel, addr, size);
        return false;
    }
    return true;
}

static MemTxResult video_read(void *opaque, hwaddr addr, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    MACEVideoPort *p = opaque;
    MACEVideo *s = p->video;
    MACEVideoChannel *c = &s->channel[p->channel];

    *value = 0;
    if (!video_register(p->channel, addr, size)) {
        return MEMTX_ERROR;
    }
    mace_video_sync(s);
    *value = c->reg[addr / 8];
    if (p->channel == 2 && addr == 0x58) {
        *value |= (uint64_t)s->revision << 32;
    }
    if (addr == 8) {
        *value |= c->active.valid ? 1 : 0;
        *value |= p->channel < 2 && input_sync(s, p->channel) ? 0x400 : 0;
        c->reg[1] = 0;
        video_irq(s);
    }
    return MEMTX_OK;
}

static bool video_configuration(MACEVideoChannel *c, unsigned channel,
                                 uint32_t value)
{
    uint32_t old = c->reg[2];
    uint32_t changed = old ^ value;
    uint32_t clock = channel < 2 ? 2 : 32;
    MACEVideoActive ignored = { 0 };
    MACEVideoOutput raster = { 0 };

    if (!changed) {
        return true;
    }
    if (c->active.valid) {
        return false;
    }
    if (changed == 1 && !(value & 1)) {
        return true;
    }
    if ((old & 1) || ((value & 1) && changed != 1)) {
        return false;
    }
    if ((value & 1) && !(value & clock)) {
        return false;
    }
    if ((value & 1) && channel < 2 &&
        (!input_geometry(c, 0, &ignored) || !input_geometry(c, 1, &ignored))) {
        return false;
    }
    if (channel == 2 && (value & 33)) {
        unsigned source_e = (value >> 1) & 3;
        unsigned source_f = (value >> 3) & 3;

        if (!output_raster(c, 0, &raster) ||
            !output_raster(c, 1, &raster) ||
            (value & 0x3c03f00) ||
            (source_e != 0 && source_e != 2) ||
            (source_f != 0 && source_f != 2)) {
            return false;
        }
        if (((value >> 6) & 3) && (!c->reg[10] ||
            c->reg[10] > 2 * raster.pixels)) {
            return false;
        }
        if ((value & 1) && (((value >> 14) & 7) != 2 ||
            (value & 0x2e0000) || (!source_e && !source_f))) {
            return false;
        }
    }
    if (changed == clock || changed == 1) {
        return true;
    }
    if (channel < 2 && (changed & 0x3fc) && ((old | value) & clock)) {
        return false;
    }
    return channel < 2 || !((old | value) & clock);
}

static bool video_bank(MACEVideoChannel *c, unsigned channel, unsigned reg)
{
    unsigned bank;

    if (!(c->reg[2] & 1)) {
        return true;
    }
    bank = channel < 2 ? reg >= 9 : reg >= 8;
    return c->field_open && c->field != bank;
}

static MemTxResult video_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    MACEVideoPort *p = opaque;
    MACEVideo *s = p->video;
    MACEVideoChannel *c = &s->channel[p->channel];
    unsigned reg = addr / 8;
    uint64_t mask, old;

    if (!video_register(p->channel, addr, size)) {
        return MEMTX_ERROR;
    }
    mace_video_sync(s);
    if (addr == 8) {
        return MEMTX_OK;
    }
    mask = reg < 12 ? video_masks[p->channel == 2][reg] : UINT64_MAX;
    old = c->reg[reg];

    value &= mask;
    if (p->channel == 2 && reg == 2 && value != old) {
        for (unsigned i = 0; i < 2; i++) {
            if (((s->channel[i].reg[2] >> 2) & 3) >= 2 &&
                s->channel[i].active.valid) {
                goto unsupported;
            }
        }
    }
    if (p->channel == 2 && reg == 10 && (c->reg[2] & 32)) {
        goto unsupported;
    }
    if (reg == 2 && !video_configuration(c, p->channel, value)) {
        goto unsupported;
    }
    if (reg >= 6 && reg <= (p->channel < 2 ? 11 : 9) &&
        !video_bank(c, p->channel, reg)) {
        goto unsupported;
    }
    if (p->channel == 2 && reg == 11) {
        uint64_t changed = c->reg[11] ^ value;
        unsigned ref = (c->reg[2] >> 6) & 3;
        bool consuming = c->active.valid || c->field_open;

        for (unsigned i = 0; i < 2; i++) {
            unsigned source = (s->channel[i].reg[2] >> 2) & 3;

            if (source < 2 && (s->channel[i].active.valid ||
                s->channel[i].field_open) &&
                (changed & (3U << (source * 2)))) {
                goto unsupported;
            }
        }
        for (unsigned i = 0; i < 2; i++) {
            consuming |= s->channel[i].field_open &&
                         ((s->channel[i].reg[2] >> 2) & 3) >= 2;
        }
        if (ref >= 2 && consuming && (changed & (3U << (2 * (ref - 2))))) {
            goto unsupported;
        }
        if (changed & ~15ULL) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-video: shared audio/GBE/GPI signals "
                          "are not connected\n");
        }
    }
    if (reg == 0 && !(value & 1)) {
        memset(&c->active, 0, sizeof(c->active));
        c->halted = false;
    }
    if (reg == 2 && p->channel < 2 && !(value & 2)) {
        c->field_open = false;
    }
    c->reg[reg] = value;
    if (p->channel == 2 && reg == 2 && ((old ^ value) & 32)) {
        if (value & 32) {
            s->output.generation++;
            if (!((value >> 6) & 3)) {
                output_clock(s, NULL);
            }
        } else {
            output_loss(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), false);
        }
    }
    video_irq(s);
    return MEMTX_OK;

unsupported:
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace-video: unsafe or unsupported channel %u "
                  "configuration at 0x%" HWADDR_PRIx "\n", p->channel, addr);
    return MEMTX_ERROR;
}

const MemoryRegionOps mace_video_ops = {
    .read_with_attrs = video_read,
    .write_with_attrs = video_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

void mace_video_init(MACEVideo *s, AddressSpace *as,
                     uint32_t (*get_ust)(void *, int64_t),
                     void (*update_irq)(void *, unsigned), void *opaque)
{
    s->as = as;
    s->get_ust = get_ust;
    s->update_irq = update_irq;
    s->opaque = opaque;
    QTAILQ_INIT(&s->pending);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, video_timer, s);
}

void mace_video_reset(MACEVideo *s)
{
    video_clear_events(s);
    timer_del(s->timer);
    memset(s->channel, 0, sizeof(s->channel));
    memset(&s->output, 0, sizeof(s->output));
    s->lock[2] = s->lock[3] = false;
    video_irq(s);
}

int mace_video_post_load(MACEVideo *s)
{
    MACEVideoEvent *e, *previous = NULL;

    for (unsigned i = 0; i < 3; i++) {
        MACEVideoChannel *c = &s->channel[i];
        MACEVideoActive *a = &c->active;

        for (unsigned r = 0; r < 12; r++) {
            uint64_t mask = r == 1 ? (i < 2 ? 0x3fe : 0x19e) :
                            video_masks[i == 2][r];

            if (c->reg[r] & ~mask) {
                return -EINVAL;
            }
        }
        if (c->field > 1 || (c->halted && a->valid) ||
            (a->valid && (!(c->reg[0] & c->reg[2] & 1) || !a->width ||
            (a->width & 7) || a->width > 4088 || !a->lines ||
            a->lines > 1023 || a->row >= a->lines ||
            a->column >= a->width || (a->column & 1) ||
            a->cursor >= 0x200000 || (a->cursor & 7) ||
            (i < 2 ? a->count > 6 || (a->count & 1) : a->count > 4)))) {
            return -EINVAL;
        }
    }
    QTAILQ_FOREACH(e, &s->pending, next) {
        bool generated = e->type >= VIDEO_OUTPUT_SOF &&
                         e->type <= VIDEO_OUTPUT_EOF;

        if (e->time < 0 || e->field > 1 ||
            (!generated && (e->type > MACE_VIDEO_LINE ||
             (e->port != 0 && e->port != 1 && e->port != 4))) ||
            (previous && (e->time < previous->time ||
             (e->time == previous->time && event_priority(e->type) <
                                         event_priority(previous->type))))) {
            return -EINVAL;
        }
        previous = e;
    }
    if (s->output.locked && (!(s->channel[2].reg[2] & 32) ||
        !s->output.period_num || !s->output.period_den ||
        !s->output.pixels || s->output.pixels > 2048 ||
        !s->output.lines || s->output.lines > 1024 ||
        s->output.origin < 0)) {
        return -EINVAL;
    }
    video_schedule(s);
    video_irq(s);
    return 0;
}

static const VMStateDescription vmstate_video_active = {
    .name = "sgi-mace-video/active",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(page, MACEVideoActive, 32),
        VMSTATE_UINT8_ARRAY(unit, MACEVideoActive, 8),
        VMSTATE_UINT32(width, MACEVideoActive),
        VMSTATE_UINT32(lines, MACEVideoActive),
        VMSTATE_UINT32(first_x, MACEVideoActive),
        VMSTATE_UINT32(first_y, MACEVideoActive),
        VMSTATE_UINT32(black_end, MACEVideoActive),
        VMSTATE_UINT32(cursor, MACEVideoActive),
        VMSTATE_UINT32(row, MACEVideoActive),
        VMSTATE_UINT32(column, MACEVideoActive),
        VMSTATE_UINT8(count, MACEVideoActive),
        VMSTATE_BOOL(interleaved, MACEVideoActive),
        VMSTATE_BOOL(valid, MACEVideoActive),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_video_channel = {
    .name = "sgi-mace-video/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(reg, MACEVideoChannel, 24),
        VMSTATE_UINT32(ust, MACEVideoChannel),
        VMSTATE_UINT32(msc, MACEVideoChannel),
        VMSTATE_STRUCT(active, MACEVideoChannel, 0, vmstate_video_active,
                       MACEVideoActive),
        VMSTATE_UINT64(sequence, MACEVideoChannel),
        VMSTATE_BOOL(halted, MACEVideoChannel),
        VMSTATE_BOOL(field_open, MACEVideoChannel),
        VMSTATE_UINT8(field, MACEVideoChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int video_pre_load(void *opaque)
{
    MACEVideo *s = opaque;

    timer_del(s->timer);
    video_clear_events(s);
    return 0;
}

static const VMStateDescription vmstate_video_event = {
    .name = "sgi-mace-video/event",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(time, MACEVideoEvent),
        VMSTATE_UINT32(x, MACEVideoEvent),
        VMSTATE_UINT32(y, MACEVideoEvent),
        VMSTATE_UINT32(sample, MACEVideoEvent),
        VMSTATE_UINT64(sequence, MACEVideoEvent),
        VMSTATE_UINT64(period_num, MACEVideoEvent),
        VMSTATE_UINT64(period_den, MACEVideoEvent),
        VMSTATE_UINT64(generation, MACEVideoEvent),
        VMSTATE_UINT64(ticks, MACEVideoEvent),
        VMSTATE_INT64(origin, MACEVideoEvent),
        VMSTATE_UINT8(port, MACEVideoEvent),
        VMSTATE_UINT8(field, MACEVideoEvent),
        VMSTATE_UINT8(type, MACEVideoEvent),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_video_output = {
    .name = "sgi-mace-video/output",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(origin, MACEVideoOutput),
        VMSTATE_UINT64(base_ticks, MACEVideoOutput),
        VMSTATE_UINT64(generation, MACEVideoOutput),
        VMSTATE_UINT64(sequence, MACEVideoOutput),
        VMSTATE_UINT64(next_sequence, MACEVideoOutput),
        VMSTATE_UINT32(period_num, MACEVideoOutput),
        VMSTATE_UINT32(period_den, MACEVideoOutput),
        VMSTATE_UINT32(pixels, MACEVideoOutput),
        VMSTATE_UINT32(lines, MACEVideoOutput),
        VMSTATE_UINT32(blank, MACEVideoOutput),
        VMSTATE_UINT32(leading, MACEVideoOutput),
        VMSTATE_UINT32(trailing, MACEVideoOutput),
        VMSTATE_UINT32(image_x, MACEVideoOutput),
        VMSTATE_UINT32(image_y, MACEVideoOutput),
        VMSTATE_BOOL(locked, MACEVideoOutput),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_mace_video = {
    .name = "sgi-mace-video",
    .pre_load = video_pre_load,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(revision, MACEVideo),
        VMSTATE_STRUCT(output, MACEVideo, 0, vmstate_video_output,
                       MACEVideoOutput),
        VMSTATE_BOOL_ARRAY(lock, MACEVideo, 5),
        VMSTATE_QTAILQ_V(pending, MACEVideo, 1, vmstate_video_event,
                        MACEVideoEvent, next),
        VMSTATE_STRUCT_ARRAY(channel, MACEVideo, 3, 0,
                             vmstate_video_channel, MACEVideoChannel),
        VMSTATE_END_OF_LIST()
    },
};
