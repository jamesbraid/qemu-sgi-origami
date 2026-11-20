/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI OCTANE Digital Video option (Galileo 1.5 board), at the level where a
 * guest sees it present, enumerates it, opens it and sets up a path.
 *
 * Registers are reached through an option window of the IMPACT graphics
 * board (sgi-mgras.h); DMA and interrupts go through a GIO-mode Bridge on
 * the option's XIO board. Every fact, its grade and what is not modelled
 * is in docs/system/mips/sgi/hardware/galileo.rst.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/display/sgi/video-io.h"
#include "hw/misc/sgi-mgras.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "trace.h"

#define TYPE_SGI_GALILEO "sgi-galileo"
OBJECT_DECLARE_TYPE(GalState, GalClass,
                    SGI_GALILEO)

/* "Identity": board product ID, revisions and the board's part. */
#define GAL_PRODUCT_ID         0x11
#define GAL_BOARD_REVISION     0x24
#define GAL_TMI_REVISION       2
#define GAL_CSC_REVISION       0
#define GAL_BOARD_PART         "030-0931-003"
#define GAL_BOARD_NAME         "GALILEO15"
#define GAL_BOARD_REVISION_TXT "C"

/* "GIO window": slot 0's product ID word, and where the other probes land. */
#define GIO_ID_OFFSET           0x4
#define GIO_MODE                0x10
#define GIO_CHIP_ID             0x20
#define GIO_SLOT_STRIDE_1       0x400000
#define GIO_SLOT_STRIDE_2       0x500000
#define GIO_SLOT_STRIDE_3       0x600000

/* "Interrupts": GIO lines, which the model wires to Bridge pins alike. */
#define GIO_LINE_DMA            4
#define GIO_LINE_CC1            6

/* "Register blocks". */
enum { BLK_BLEND, BLK_CC1, BLK_CONTROL, BLK_CSC, BLK_COUNT };
static const hwaddr blk_base[BLK_COUNT] = {
    0x072800, 0x073000, 0x073400, 0x073800,
};
static const char *const blk_name[BLK_COUNT] = {
    "blender", "CC1", "control", "CSC"
};
/* Index and data direct registers; the blender's never match (see below). */
static const uint8_t blk_index_reg[BLK_COUNT] = { 0, 1, 0, 0 };
static const uint8_t blk_data_reg[BLK_COUNT] = { 8, 2, 1, 1 };
/* Highest indirect register; the control block also answers 240 to 255. */
static const uint16_t blk_last[BLK_COUNT] = { 38, 80, 39, 5 };
#define CONTROL_CROSSPOINT_FIRST 240
#define CONTROL_CROSSPOINT_LAST  255
#define CONTROL_DIRECT_REVISION  2
#define CONTROL_DIRECT_TMI_INDEX 6
#define CONTROL_DIRECT_TMI_DATA  7
#define CSC_DIRECT_REVISION      3
#define TMI_LAST                 149
#define TMI_REVISION_REG         147
#define TMI_STATUS_REG           129
#define TMI_STATUS_START         0x02
#define BLEND_DIRECT_DATA1       1
#define BLEND_DIRECT_DATA2       2
#define BLEND_DIRECT_ADDR_LOW    3
#define BLEND_DIRECT_ADDR_HIGH   4
#define BLEND_LAST2              3
#define CC1_FIELD_CONTROL        38
#define CC1_FIELD_ENABLES        0xa0
#define CONTROL_I2C_STATUS       37
#define CONTROL_I2C_BUSY         0x80
#define CONTROL_TIMER_STATUS     17
/* No field event is modelled: these bits never read set. */
#define CONTROL_TIMER_PENDING    0x51

/* "VGI1 channel registers": byte offsets within a channel's 0x1000 block. */
#define CH_SETUP        0x000
#define CH_INT_ENABLE   0x010
#define CH_HORIZONTAL   0x020
#define CH_VERTICAL     0x030
#define CH_STRIDE       0x040
#define CH_REG_100      0x100
#define CH_TRIGGER      0x200
#define CH_START_OFFSET 0x210
#define CH_PAGE_LIST    0x220
#define CH_STATUS       0x300
#define CH_REGS         (0x400 / 8)
#define CH_R(ch, off)   ((ch)->reg[(off) / 8])
#define CH_ST_FIELD     BIT(0)
#define CH_ST_CONSUMED  BIT(1)
#define CH_ST_DONE      BIT(3)
#define CH_ST_TRIGGER   BIT(4)
#define CH_ST_START     BIT(5)
#define CH_ST_ERROR     BIT(8)
#define CH_ST_EVENTS    0xffff
#define CH_ST_LINE_SHIFT 16
#define CH_ST_COUNT_SHIFT 33
#define CH_ST_COUNT_MASK  0x7fff
#define GAL_PAGE        16384
#define GAL_LIST_ENTRIES 2048
#define GAL_GIO_MASK    0xffffffffffULL
/* Approximation: the setup bits that stay set from set-up to abort. */
#define CH_SETUP_ACTIVE 0x0c

typedef struct GalReq {
    uint64_t start_offset, page_list;
} GalReq;

typedef struct GalChan {
    uint64_t reg[CH_REGS];
    GalReq next, cur;
    bool queued, active;
    bool ending;
} GalChan;

struct GalClass {
    SGIXioBoardClass parent_class;

    DeviceUnrealize parent_unrealize;
    ResettablePhases parent_phases;
};

struct GalState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint32_t field_period_ns;
    Object *out_sink;

    SGIBridgeState *bridge;
    SGIMgrasState *graphics;
    MemoryRegion gio;
    QEMUTimer *field_timer;

    GalChan ch[2];
    QEMUTimer *dma_timer;
    QEMUTimer *end_timer;
    uint32_t field_count, event_count;
    bool dma_irq;
    AddressSpace fabric_as;
    uint8_t direct[BLK_COUNT][8];
    uint8_t ind[BLK_COUNT][256];
    uint8_t tmi[TMI_LAST + 1];
    uint8_t blend2[BLEND_LAST2 + 1];
    bool tmi_start;
};

static void gal_unimp(const char *what, hwaddr addr, bool write)
{
    qemu_log_mask(LOG_UNIMP, "sgi-galileo: unimplemented %s %s at "
                  "0x%" HWADDR_PRIx "\n", what, write ? "write" : "read",
                  addr);
}

/* ---- Register blocks ---- */

static bool gal_locate(hwaddr addr, unsigned *blk, unsigned *sel)
{
    for (unsigned b = 0; b < BLK_COUNT; b++) {
        hwaddr off = addr - blk_base[b];

        if (addr >= blk_base[b] && off < SGI_MGRAS_OPTION_BLOCK_SIZE) {
            if ((off & 0x7f) != 8) {
                return false;
            }
            *blk = b;
            *sel = off >> 7;
            return true;
        }
    }
    return false;
}

static bool gal_ind_valid(unsigned blk, unsigned idx)
{
    return idx <= blk_last[blk] ||
           (blk == BLK_CONTROL && idx >= CONTROL_CROSSPOINT_FIRST &&
            idx <= CONTROL_CROSSPOINT_LAST);
}

/* Fields sit on a fixed grid of virtual time; a late callback keeps it. */
static int64_t gal_next_field(GalState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return (now / s->field_period_ns + 1) * s->field_period_ns;
}

static void gal_field_update(GalState *s)
{
    if (s->ind[BLK_CC1][CC1_FIELD_CONTROL] & CC1_FIELD_ENABLES) {
        if (!timer_pending(s->field_timer)) {
            timer_mod(s->field_timer, gal_next_field(s));
        }
    } else {
        timer_del(s->field_timer);
    }
}

/* A pulse, not a level; the real line type is unknown. */
static void gal_field_tick(void *opaque)
{
    GalState *s = opaque;

    qemu_irq_pulse(qdev_get_gpio_in(DEVICE(s->bridge), GIO_LINE_CC1));
    timer_mod(s->field_timer, gal_next_field(s));
}

static uint8_t gal_ind_read(GalState *s, unsigned blk, unsigned idx)
{
    if (!gal_ind_valid(blk, idx)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: %s indirect %u is "
                      "out of range\n", blk_name[blk], idx);
        return 0;
    }
    if (blk == BLK_CONTROL && idx == CONTROL_I2C_STATUS) {
        return s->ind[blk][idx] & ~CONTROL_I2C_BUSY;
    }
    if (blk == BLK_CONTROL && idx == CONTROL_TIMER_STATUS) {
        return s->ind[blk][idx] & ~CONTROL_TIMER_PENDING;
    }
    return s->ind[blk][idx];
}

static void gal_ind_write(GalState *s, unsigned blk, unsigned idx,
                           uint8_t value)
{
    if (!gal_ind_valid(blk, idx)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: %s indirect %u is "
                      "out of range\n", blk_name[blk], idx);
        return;
    }
    s->ind[blk][idx] = value;
    if (blk == BLK_CC1 && idx == CC1_FIELD_CONTROL) {
        gal_field_update(s);
    }
}

static uint8_t gal_option_read_reg(void *opaque, hwaddr addr)
{
    GalState *s = opaque;
    unsigned blk, sel;

    if (addr == SGI_MGRAS_OPTION_ID) {
        return GAL_PRODUCT_ID;
    }
    if (!gal_locate(addr, &blk, &sel)) {
        gal_unimp("option window register", addr, false);
        return 0;
    }
    if (blk == BLK_BLEND &&
        (sel == BLEND_DIRECT_DATA1 || sel == BLEND_DIRECT_DATA2)) {
        unsigned idx = s->direct[blk][BLEND_DIRECT_ADDR_LOW] |
                       s->direct[blk][BLEND_DIRECT_ADDR_HIGH] << 8;

        if (sel == BLEND_DIRECT_DATA2 && idx <= BLEND_LAST2) {
            return s->blend2[idx];
        }
        return sel == BLEND_DIRECT_DATA1 ? gal_ind_read(s, blk, idx) : 0;
    }
    if (sel == blk_data_reg[blk]) {
        return gal_ind_read(s, blk, s->direct[blk][blk_index_reg[blk]]);
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_TMI_DATA) {
        unsigned idx = s->direct[blk][CONTROL_DIRECT_TMI_INDEX];

        if (idx > TMI_LAST) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: TMI indirect %u "
                          "is out of range\n", idx);
            return 0;
        }
        if (idx == TMI_STATUS_REG) {
            /* A start pulse: set on one read, clear on the next. */
            s->tmi_start = !s->tmi_start;
            return s->tmi_start ? s->tmi[idx] | TMI_STATUS_START :
                                  s->tmi[idx] & ~TMI_STATUS_START;
        }
        return s->tmi[idx];
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_REVISION) {
        return GAL_BOARD_REVISION;
    }
    return s->direct[blk][sel];
}

static void gal_option_write_reg(void *opaque, hwaddr addr, uint8_t value)
{
    GalState *s = opaque;
    unsigned blk, sel;

    if (addr == SGI_MGRAS_OPTION_ID) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: write to the "
                      "identification byte\n");
        return;
    }
    if (!gal_locate(addr, &blk, &sel)) {
        gal_unimp("option window register", addr, true);
        return;
    }
    if (blk == BLK_BLEND &&
        (sel == BLEND_DIRECT_DATA1 || sel == BLEND_DIRECT_DATA2)) {
        unsigned idx = s->direct[blk][BLEND_DIRECT_ADDR_LOW] |
                       s->direct[blk][BLEND_DIRECT_ADDR_HIGH] << 8;

        if (sel == BLEND_DIRECT_DATA2 && idx <= BLEND_LAST2) {
            s->blend2[idx] = value;
        } else if (sel == BLEND_DIRECT_DATA1) {
            gal_ind_write(s, blk, idx, value);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: blender file 2 "
                          "register %u is out of range\n", idx);
        }
        return;
    }
    if (sel == blk_data_reg[blk]) {
        gal_ind_write(s, blk, s->direct[blk][blk_index_reg[blk]], value);
        return;
    }
    if (blk == BLK_CONTROL && sel == CONTROL_DIRECT_TMI_DATA) {
        unsigned idx = s->direct[blk][CONTROL_DIRECT_TMI_INDEX];

        if (idx > TMI_LAST) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: TMI indirect %u "
                          "is out of range\n", idx);
        } else if (idx != TMI_REVISION_REG) {
            s->tmi[idx] = value;
        }
        return;
    }
    if ((blk == BLK_CSC && sel == CSC_DIRECT_REVISION) ||
        (blk == BLK_CONTROL && sel == CONTROL_DIRECT_REVISION)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: write to a revision "
                      "register\n");
        return;
    }
    s->direct[blk][sel] = value;
}

static uint8_t gal_option_read(void *opaque, hwaddr addr)
{
    uint8_t value = gal_option_read_reg(opaque, addr);

    trace_sgi_galileo_option(addr, value, false);
    return value;
}

static void gal_option_write(void *opaque, hwaddr addr, uint8_t value)
{
    trace_sgi_galileo_option(addr, value, true);
    gal_option_write_reg(opaque, addr, value);
}

static const SGIMgrasOptionWindowOps gal_option_window_ops = {
    .read = gal_option_read,
    .write = gal_option_write,
};

/* ---- The GIO window ---- */

/* Bytes the Bridge's GIO translation reaches, one page at a time. */
static bool gal_dma_read(GalState *s, uint64_t gio, void *buf, size_t len)
{
    IOMMUMemoryRegion *iommu = &s->bridge->dma[0].iommu;
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_GET_CLASS(iommu);
    uint8_t *dst = buf;

    while (len) {
        IOMMUTLBEntry e = imrc->translate(iommu, gio, IOMMU_RO, 0);
        size_t n = MIN(len, e.addr_mask + 1 - (gio & e.addr_mask));

        if (e.perm == IOMMU_NONE ||
            address_space_read(&s->fabric_as,
                               e.translated_addr | (gio & e.addr_mask),
                               MEMTXATTRS_UNSPECIFIED, dst, n) != MEMTX_OK) {
            return false;
        }
        gio += n;
        dst += n;
        len -= n;
    }
    return true;
}

/*
 * Pixels are B G R, three bytes, lines back to back; stride counts four.
 * Studio-range BT.601 out; other formats are not read.
 */
static void gal_put_frame(GalState *s, const uint8_t *buf, size_t stride,
                          unsigned lines)
{
    SGIVideoFormat fmt = {
        .width = stride / 4, .height = lines, .rate_num = 30000,
        .rate_den = 1001, .fields = SGI_VIDEO_BFF, .depth = 8,
    };
    SGIVideoFrame *f;

    if (!s->out_sink || !sgi_video_format_valid(&fmt)) {
        return;
    }
    f = sgi_video_frame_new(&fmt);
    for (unsigned y = 0; y < lines; y++) {
        const uint8_t *row = buf + y * fmt.width * 3;

        for (unsigned x = 0; x < fmt.width; x++) {
            int r = row[x * 3 + 2], g = row[x * 3 + 1], b = row[x * 3];

            f->y[y * fmt.width + x] =
                16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
            if (!(x & 1)) {
                unsigned c = y * fmt.width / 2 + x / 2;

                f->cb[c] = 128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8);
                f->cr[c] = 128 + ((112 * r - 94 * g - 18 * b + 128) >> 8);
            }
        }
    }
    sgi_video_sink_put_frame(s->out_sink, f);
    sgi_video_frame_free(f);
}

/* One request: the page list's entries name 16 KiB GIO pages by number. */
static bool gal_request_run(GalState *s, unsigned c, const GalReq *rq)
{
    GalChan *ch = &s->ch[c];
    uint64_t vparam = CH_R(ch, CH_VERTICAL);
    uint32_t lines = (vparam >> 48 & 0xffff) + (vparam >> 16 & 0xffff);
    uint64_t stride = CH_R(ch, CH_STRIDE);
    g_autofree uint8_t *buf = NULL;
    uint8_t *dst;
    uint64_t off = rq->start_offset;
    size_t bytes, pages;

    if (off >= GAL_PAGE || stride > 4 * SGI_VIDEO_MAX_WIDTH ||
        lines > SGI_VIDEO_MAX_HEIGHT) {
        return false;
    }
    bytes = lines * (stride / 4) * 3;
    pages = (off + bytes + GAL_PAGE - 1) / GAL_PAGE;
    if (pages > GAL_LIST_ENTRIES) {
        return false;
    }
    buf = dst = g_malloc0(bytes);
    for (size_t i = 0; i < pages; i++) {
        uint8_t raw[8];
        uint64_t pfn, gio;
        size_t n = MIN(bytes - (dst - buf), GAL_PAGE - off);

        if (!gal_dma_read(s, (rq->page_list & GAL_GIO_MASK) + i * 8, raw, 8)) {
            return false;
        }
        pfn = ldq_be_p(raw) & 0xffffffff;
        gio = (pfn << 14) + off;
        trace_sgi_galileo_desc(c, i, pfn);
        if (!gal_dma_read(s, gio, dst, n)) {
            return false;
        }
        dst += n;
        off = 0;
    }
    gal_put_frame(s, buf, stride, lines);
    return true;
}

static GalChan *gal_chan(GalState *s, hwaddr addr, unsigned size)
{
    unsigned c = addr >> 12;

    if (size != 8 || c < 1 || c > 2 || (addr & 0xfff) >= 0x400 || (addr & 7)) {
        return NULL;
    }
    return &s->ch[c - 1];
}

static void gal_irq_update(GalState *s)
{
    bool level = false;

    for (unsigned c = 0; c < 2; c++) {
        level |= CH_R(&s->ch[c], CH_STATUS) & CH_R(&s->ch[c], CH_INT_ENABLE);
    }
    if (level != s->dma_irq) {
        s->dma_irq = level;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->bridge), GIO_LINE_DMA), level);
    }
}

static bool gal_chan_running(const GalChan *ch)
{
    return (CH_R(ch, CH_SETUP) & CH_SETUP_ACTIVE) &&
           (CH_R(ch, CH_INT_ENABLE) & CH_ST_FIELD);
}

static void gal_dma_update(GalState *s)
{
    if (gal_chan_running(&s->ch[0]) || gal_chan_running(&s->ch[1])) {
        if (!timer_pending(s->dma_timer)) {
            timer_mod(s->dma_timer, gal_next_field(s));
        }
    } else {
        timer_del(s->dma_timer);
    }
}

/* The status the guest reads: the events, and the field and line at them. */
static uint64_t gal_status(GalState *s, GalChan *ch)
{
    uint64_t events = CH_R(ch, CH_STATUS);
    uint64_t lines = CH_R(ch, CH_VERTICAL) >> 16 & 0x7ff;

    return events | ((events & CH_ST_DONE) ? lines << CH_ST_LINE_SHIFT : 0) |
           ((uint64_t)(s->event_count & 1) << 32) |
           ((uint64_t)(s->event_count & CH_ST_COUNT_MASK) <<
            CH_ST_COUNT_SHIFT);
}

/* The end of DMA lands between verticals, never on one. True if any is due. */
static bool gal_dma_end(GalState *s)
{
    bool due = false;

    for (unsigned c = 0; c < 2; c++) {
        GalChan *ch = &s->ch[c];

        if (!ch->ending) {
            continue;
        }
        due = true;
        if (CH_R(ch, CH_STATUS) & (CH_R(ch, CH_INT_ENABLE) | CH_ST_FIELD)) {
            continue;
        }
        if (!gal_request_run(s, c, &ch->cur)) {
            CH_R(ch, CH_STATUS) |= CH_ST_ERROR;
        }
        CH_R(ch, CH_STATUS) |= CH_ST_DONE;
        ch->active = ch->ending = false;
        s->event_count = s->field_count;
    }
    gal_irq_update(s);
    return due;
}

static void gal_dma_end_timer(void *opaque)
{
    GalState *s = opaque;

    gal_dma_end(s);
    if (s->ch[0].ending || s->ch[1].ending) {
        timer_mod(s->end_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                s->field_period_ns / 8);
    }
}

/* A request is a start vertical, a plain one, then its end: two fields. */
static void gal_dma_tick(void *opaque)
{
    GalState *s = opaque;

    timer_del(s->end_timer);
    if (gal_dma_end(s) || (CH_R(&s->ch[0], CH_STATUS) & CH_ST_DONE) ||
        (CH_R(&s->ch[1], CH_STATUS) & CH_ST_DONE)) {
        timer_mod(s->dma_timer, gal_next_field(s));
        return;
    }
    if (!gal_chan_running(&s->ch[0]) && !gal_chan_running(&s->ch[1])) {
        return;
    }
    s->field_count++;
    s->event_count = s->field_count;
    for (unsigned c = 0; c < 2; c++) {
        GalChan *ch = &s->ch[c];

        if (!gal_chan_running(ch)) {
            continue;
        }
        CH_R(ch, CH_STATUS) |= CH_ST_FIELD;
        if (ch->active) {
            ch->ending = true;
            timer_mod(s->end_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                    s->field_period_ns / 2);
        } else if (ch->queued) {
            ch->cur = ch->next;
            ch->queued = false;
            ch->active = true;
            CH_R(ch, CH_STATUS) |= CH_ST_START | CH_ST_CONSUMED;
            if (CH_R(ch, CH_TRIGGER) == 1) {
                CH_R(ch, CH_STATUS) |= CH_ST_TRIGGER;
            }
        }
    }
    gal_irq_update(s);
    timer_mod(s->dma_timer, gal_next_field(s));
}

static uint64_t gal_gio_read_reg(GalState *s, hwaddr addr, unsigned size)
{
    GalChan *ch = gal_chan(s, addr, size);

    if (ch) {
        switch (addr & 0xfff) {
        case CH_SETUP:
        case CH_INT_ENABLE:
        case CH_HORIZONTAL:
        case CH_VERTICAL:
        case CH_STRIDE:
        case CH_REG_100:
        case CH_TRIGGER:
        case CH_START_OFFSET:
        case CH_PAGE_LIST:
        case CH_STATUS:
            return (addr & 0xfff) == CH_STATUS ? gal_status(s, ch) :
                   CH_R(ch, addr & 0xfff);
        }
        gal_unimp("GIO channel register", addr, false);
        return 0;
    }
    /* The 64-bit identification register; its low word is the slot probe. */
    if ((addr == GIO_ID_OFFSET && size == 4) || (!addr && size == 8)) {
        return GAL_PRODUCT_ID;
    }
    if (!addr && size == 4) {
        return 0;
    }
    if (addr == GIO_CHIP_ID && size == 8) {
        return 0;
    }
    /* Slots 1 to 3 hold no board: the probe reads zero. */
    if ((addr == GIO_SLOT_STRIDE_1 + GIO_ID_OFFSET ||
         addr == GIO_SLOT_STRIDE_2 + GIO_ID_OFFSET ||
         addr == GIO_SLOT_STRIDE_3 + GIO_ID_OFFSET) && size == 4) {
        return 0;
    }
    gal_unimp("GIO window", addr, false);
    return 0;
}

static uint64_t gal_gio_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t value = gal_gio_read_reg(opaque, addr, size);

    trace_sgi_galileo_gio(addr, value, size, false);
    return value;
}

static void gal_gio_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    GalState *s = opaque;
    GalChan *ch = gal_chan(s, addr, size);

    trace_sgi_galileo_gio(addr, value, size, true);
    if (ch) {
        unsigned off = addr & 0xfff;

        switch (off) {
        case CH_SETUP:
        case CH_INT_ENABLE:
        case CH_HORIZONTAL:
        case CH_VERTICAL:
        case CH_STRIDE:
        case CH_REG_100:
        case CH_START_OFFSET:
            CH_R(ch, off) = value;
            break;
        case CH_TRIGGER:
            CH_R(ch, off) = value;
            if (value == 2) {
                ch->queued = ch->active = ch->ending = false;
            }
            break;
        case CH_PAGE_LIST:
            CH_R(ch, off) = value;
            if (ch->queued) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-galileo: request queued "
                              "over an unstarted one\n");
            }
            ch->next = (GalReq){ .start_offset = CH_R(ch, CH_START_OFFSET),
                                 .page_list = value };
            ch->queued = true;
            break;
        case CH_STATUS:
            CH_R(ch, off) &= ~(value & CH_ST_EVENTS);
            break;
        default:
            gal_unimp("GIO channel register", addr, true);
        }
        gal_irq_update(s);
        gal_dma_update(s);
        return;
    }
    /* The driver's presence path stores the system mode. */
    if (size == 8 && addr == GIO_MODE) {
        return;
    }
    gal_unimp("GIO window", addr, true);
}

static const MemoryRegionOps gal_gio_ops = {
    .read = gal_gio_read,
    .write = gal_gio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* ---- Lifecycle ---- */

static bool gal_build_widget(SGIXioBoardState *board, Error **errp)
{
    GalState *s = SGI_GALILEO(board);
    Object *obj = OBJECT(s);
    DeviceState *bridge;
    bool ambiguous = false;
    Object *gfx;
    uint64_t serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('O', board->slot);
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

    if (!s->fabric) {
        error_setg(errp, TYPE_SGI_GALILEO " needs the fabric its "
                   "interrupts travel");
        return false;
    }
    if (!s->graphics) {
        /* Without a "graphics" link, take the machine's only MGRAS. */
        gfx = object_resolve_path_type("", TYPE_SGI_MGRAS, &ambiguous);
        if (!gfx || ambiguous) {
            error_setg(errp, TYPE_SGI_GALILEO
                       " needs a graphics link or exactly one sgi-mgras that "
                       "is already realized");
            return false;
        }
        s->graphics = SGI_MGRAS(gfx);
    }

    memory_region_init_io(&s->gio, obj, &gal_gio_ops, s,
                          TYPE_SGI_GALILEO ".gio",
                          SGI_BRIDGE_GIO_WINDOW_SIZE);
    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(obj, "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    sgi_synthetic_barcode(barcode, 'O', serial);
    qdev_prop_set_string(bridge, "board-part", GAL_BOARD_PART);
    qdev_prop_set_string(bridge, "board-barcode", barcode);
    qdev_prop_set_string(bridge, "board-revision", GAL_BOARD_REVISION_TXT);
    qdev_prop_set_string(bridge, "board-name", GAL_BOARD_NAME);
    qdev_prop_set_uint64(bridge, "onewire-serial", serial);
    qdev_prop_set_bit(bridge, "gio-mode", true);
    sgi_xio_board_name_pci_bus(board, bridge, NULL);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    object_property_set_link(OBJECT(bridge), "gio-window", OBJECT(&s->gio),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    if (!sgi_mgras_attach_option_window(s->graphics, &gal_option_window_ops,
                                        s)) {
        error_setg(errp, "the graphics board's option window already has "
                   "a board");
        return false;
    }
    s->field_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gal_field_tick, s);
    s->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gal_dma_tick, s);
    s->end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gal_dma_end_timer, s);
    address_space_init(&s->fabric_as, s->fabric, "sgi-galileo-fabric");

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    board->link_reset = qdev_get_gpio_in_named(bridge, "link-reset", 0);
    board->link_chip = OBJECT(bridge);
    return true;
}

static void gal_unrealize(DeviceState *dev)
{
    GalClass *dc = SGI_GALILEO_GET_CLASS(dev);
    GalState *s = SGI_GALILEO(dev);

    dc->parent_unrealize(dev);
    if (s->graphics) {
        sgi_mgras_detach_option_window(s->graphics, s);
    }
    timer_free(s->field_timer);
    s->field_timer = NULL;
    if (s->dma_timer) {
        address_space_destroy(&s->fabric_as);
    }
    timer_free(s->dma_timer);
    s->dma_timer = NULL;
    timer_free(s->end_timer);
    s->end_timer = NULL;
}

/* A reset returns the registers to the model's zero state and stops fields. */
static void gal_reset_hold(Object *obj, ResetType type)
{
    GalClass *dc = SGI_GALILEO_GET_CLASS(obj);
    GalState *s = SGI_GALILEO(obj);

    if (dc->parent_phases.hold) {
        dc->parent_phases.hold(obj, type);
    }
    memset(s->direct, 0, sizeof(s->direct));
    memset(s->ind, 0, sizeof(s->ind));
    memset(s->tmi, 0, sizeof(s->tmi));
    memset(s->blend2, 0, sizeof(s->blend2));
    s->tmi_start = false;
    s->tmi[TMI_REVISION_REG] = GAL_TMI_REVISION;
    s->direct[BLK_CSC][CSC_DIRECT_REVISION] = GAL_CSC_REVISION;
    memset(s->ch, 0, sizeof(s->ch));
    if (s->field_timer) {
        timer_del(s->field_timer);
    }
    if (s->end_timer) {
        timer_del(s->end_timer);
    }
    s->field_count = s->event_count = 0;
    if (s->dma_timer) {
        timer_del(s->dma_timer);
    }
    s->dma_irq = false;
    if (s->bridge) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->bridge), GIO_LINE_DMA), 0);
    }
}

static int gal_post_load(void *opaque, int version_id)
{
    GalState *s = opaque;

    gal_field_update(s);
    /* The Bridge restores the pin; only the model's view of it is set. */
    s->dma_irq = false;
    for (unsigned c = 0; c < 2; c++) {
        s->dma_irq |= !!(CH_R(&s->ch[c], CH_STATUS) &
                         CH_R(&s->ch[c], CH_INT_ENABLE));
    }
    gal_dma_update(s);
    if (s->ch[0].ending || s->ch[1].ending) {
        timer_mod(s->end_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                s->field_period_ns / 2);
    }
    return 0;
}

static const VMStateDescription vmstate_gal_req = {
    .name = TYPE_SGI_GALILEO ".request",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(start_offset, GalReq),
        VMSTATE_UINT64(page_list, GalReq),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_gal_chan = {
    .name = TYPE_SGI_GALILEO ".channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(reg, GalChan, CH_REGS),
        VMSTATE_STRUCT(next, GalChan, 1, vmstate_gal_req, GalReq),
        VMSTATE_STRUCT(cur, GalChan, 1, vmstate_gal_req, GalReq),
        VMSTATE_BOOL(queued, GalChan),
        VMSTATE_BOOL(active, GalChan),
        VMSTATE_BOOL(ending, GalChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_gal = {
    .name = TYPE_SGI_GALILEO,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = gal_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_2DARRAY(direct, GalState, BLK_COUNT, 8),
        VMSTATE_UINT8_2DARRAY(ind, GalState, BLK_COUNT, 256),
        VMSTATE_UINT8_ARRAY(tmi, GalState, TMI_LAST + 1),
        VMSTATE_UINT8_ARRAY(blend2, GalState, BLEND_LAST2 + 1),
        VMSTATE_BOOL(tmi_start, GalState),
        VMSTATE_UINT32(field_count, GalState),
        VMSTATE_UINT32(event_count, GalState),
        VMSTATE_STRUCT_ARRAY(ch, GalState, 2, 0, vmstate_gal_chan, GalChan),
        VMSTATE_END_OF_LIST()
    },
};

static const Property gal_properties[] = {
    DEFINE_PROP_LINK("fabric", GalState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("graphics", GalState, graphics, TYPE_SGI_MGRAS,
                     SGIMgrasState *),
    DEFINE_PROP_LINK("out-sink", GalState, out_sink, TYPE_SGI_VIDEO_SINK,
                     Object *),
    /* The CC1 field interrupt period: an NTSC field by default. */
    DEFINE_PROP_UINT32("field-period-ns", GalState, field_period_ns,
                       16683333),
};

static void gal_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    GalClass *dvc = SGI_GALILEO_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    bc->build_widget = gal_build_widget;
    device_class_set_parent_unrealize(dc, gal_unrealize,
                                      &dvc->parent_unrealize);
    resettable_class_set_parent_phases(rc, NULL, gal_reset_hold, NULL,
                                       &dvc->parent_phases);
    dc->vmsd = &vmstate_gal;
    dc->desc = "SGI OCTANE Digital Video option (Galileo 1.5)";
    /* Fitted to an empty XIO slot it names (slot=widget-10). */
    dc->user_creatable = true;
    device_class_set_props(dc, gal_properties);
}

static const TypeInfo gal_types[] = {
    {
        .name = TYPE_SGI_GALILEO,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(GalState),
        .class_size = sizeof(GalClass),
        .class_init = gal_class_init,
    },
};

DEFINE_TYPES(gal_types)
