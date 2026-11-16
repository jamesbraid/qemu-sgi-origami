/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A frame code on head 0 at 30 Hz of virtual time, to check publication with
 * no guest. Enabled by x-display-shm-test-pattern; see display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/display/sgi/scanout.h"
#include "ui/sgi-display-shm.h"
#include "ui/sgi-display-priv.h"
#include "trace.h"

#define PAT_WIDTH 720
#define PAT_HEIGHT 486
#define PAT_RATE 30
#define PAT_REFRESH_MHZ (PAT_RATE * 1000)
#define PAT_BLOCK 16
#define PAT_CODE_X 16
#define PAT_CODE_Y 16
#define PAT_BITS 32

typedef struct SGIPattern {
    SGIScanout *scanout;
    uint32_t *pixels;
    int64_t epoch;              /* virtual time of frame 0 */
    uint64_t frame;
    QEMUTimer *timer;
} SGIPattern;

static SGIPattern pattern;

/* Marker 10110010, then the 16-bit frame number, then its 8-bit check. */
static const uint8_t pat_marker[8] = { 1, 0, 1, 1, 0, 0, 1, 0 };

static unsigned pat_check(unsigned n)
{
    return ((n >> 8) ^ n ^ 0xa5) & 0xff;
}

static void pat_draw_code(SGIPattern *p, unsigned n)
{
    for (unsigned i = 0; i < PAT_BITS; i++) {
        unsigned bit;

        if (i < 8) {
            bit = pat_marker[i];
        } else if (i < 24) {
            bit = (n >> (23 - i)) & 1;
        } else {
            bit = (pat_check(n) >> (31 - i)) & 1;
        }
        for (unsigned y = 0; y < PAT_BLOCK; y++) {
            uint32_t *row = p->pixels + (PAT_CODE_Y + y) * PAT_WIDTH +
                            PAT_CODE_X + i * PAT_BLOCK;

            for (unsigned x = 0; x < PAT_BLOCK; x++) {
                row[x] = bit ? 0xffffff : 0;
            }
        }
    }
    sgi_display_head_presented(p->scanout, PAT_CODE_Y, PAT_BLOCK);
}

static void pat_tick(void *opaque)
{
    SGIPattern *p = opaque;

    pat_draw_code(p, p->frame & 0xffff);
    sgi_display_head_published(p->scanout);
    if (trace_event_get_state_backends(TRACE_SGI_DISPLAY_PATTERN_FRAME)) {
        trace_sgi_display_pattern_frame(p->frame & 0xffff,
                                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                        get_clock());
    }
    p->frame++;
    timer_mod(p->timer, p->epoch +
              muldiv64(p->frame, NANOSECONDS_PER_SECOND, PAT_RATE));
}

void sgi_display_pattern_start(SGIScanout *s)
{
    SGIPattern *p = &pattern;
    SGIDisplayRegion *region;

    p->scanout = s;
    p->pixels = sgi_display_region_alloc(OBJECT(s->dev), "test-pattern",
                                         (uint64_t)PAT_WIDTH * PAT_HEIGHT * 4,
                                         SGI_SHM_KIND_PLANE, &region);
    /* The pattern replaces whatever the head's model published. */
    sgi_display_head_bind(s, SGI_SHM_FAMILY_XRGB, 1);
    sgi_display_head_set_frame(s, region, PAT_WIDTH, PAT_HEIGHT,
                               PAT_REFRESH_MHZ);
    p->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pat_tick, p);
    /* Frame 0 is up before anything runs; the timer draws the rest. */
    pat_tick(p);
}
