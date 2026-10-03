/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality/KONA's DMA engine, on the shared pixel DMA.
 *
 * See include/hw/misc/sgi-kona-dma.h for the packet this decodes, what is
 * established and what is approximated.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/visitor.h"
#include "migration/vmstate.h"
#include "exec/hwaddr.h"
#include "hw/display/sgi/pixel-dma.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-kona-dma.h"
#include "hw/misc/sgi-kona-ports.h"
#include "trace.h"

/*
 * The page size the packet's address split implies: word 1 carries the
 * address above bit 12 and word 2 the 12 bits below. That the pipe's
 * device address space has 4 KiB pages is inferred from that split.
 */
#define SGI_KONA_DMA_PAGE_SIZE 4096

/*
 * The most pages one packet's buffer may span, the model's bound: 64 MiB,
 * the largest list plane, so a whole-list copy fits, and far more than a
 * full framebuffer's 5 MiB. A packet whose lines and gaps span more is
 * refused before its page list is built, since every size in it is the
 * guest's.
 */
#define SGI_KONA_DMA_MAX_PAGES ((uint32_t)(64 * MiB / SGI_KONA_DMA_PAGE_SIZE))

/* convert_row() conversions: guest big-endian words, or tag-resolved. */
#define SGI_KONA_DMA_CONV_WORDS   0
#define SGI_KONA_DMA_CONV_RESOLVE 1

/*
 * Compose the fabric address the DMA engine's guest-memory transfers issue
 * on s->fabric_as. The packet names no destination widget, so the address
 * goes to the one fixed widget fabric_hub_widget names; see the struct
 * field comment on SGIKonaState's fabric_hub_widget.
 */
static bool sgi_kona_dma_address(void *opaque, uint64_t address,
                                 hwaddr *fabric)
{
    SGIKonaState *s = opaque;

    return sgi_xio_compose("sgi-kona", s->fabric_hub_widget, address, fabric);
}

/*
 * Guest bytes are big-endian 32-bit words and each is stored whole, top
 * byte included. Read back, a word goes out as stored, or for a
 * framebuffer readback as the pixel resolves by its tag (see
 * sgi_kona_fb_read()), which is why the reverse direction moves one
 * display line per transfer: this hook is not told where the words came
 * from, so the engine names the line in convert_x and convert_y.
 */
static void sgi_kona_dma_convert_row(void *opaque, uint8_t conversion,
                                     bool to_plane, const uint8_t *src,
                                     uint8_t *dst, unsigned pixels)
{
    SGIKonaState *s = opaque;
    SGIKonaDma *d = &s->dma;

    for (unsigned i = 0; i < pixels; i++) {
        if (to_plane) {
            ((uint32_t *)dst)[i] = ldl_be_p(src + i * 4);
        } else if (conversion == SGI_KONA_DMA_CONV_RESOLVE) {
            stl_be_p(dst + i * 4,
                     sgi_kona_fb_read(s, d->convert_x + i, d->convert_y));
        } else {
            stl_be_p(dst + i * 4, ((const uint32_t *)src)[i]);
        }
    }
}

/*
 * Every completion the engine publishes goes to the head's publish
 * (sgi_kona_publish()), which maps it onto the mailbox and cause
 * register. The engine records the last one for the qtest.
 */
static void sgi_kona_dma_publish(void *opaque, const SGICompletion *c)
{
    SGIKonaState *s = opaque;

    s->dma.test_last_kind = c->kind;
    s->dma.test_last_cookie = c->cookie;
    s->dma.test_last_payload = c->payload;
    s->dma.test_completions++;
    sgi_kona_publish(s, c);
}

static const SGIHeadOps sgi_kona_dma_head_ops = {
    .publish = sgi_kona_dma_publish,
    .convert_row = sgi_kona_dma_convert_row,
    .dma_address = sgi_kona_dma_address,
};

/* A packet the engine refuses before any transfer: one FAILED record. */
static void sgi_kona_dma_refuse(SGIKonaState *s, uint64_t cookie,
                                SGIFailure why)
{
    sgi_head_publish(&sgi_kona_dma_head_ops, s, s->dma.seq++,
                     SGI_DONE_FAILED, cookie, why);
}

/*
 * One transfer item, through the shared layer's three steps. A packet
 * may take several items; only its last, or one that fails, carries the
 * packet's cookie, so the sender sees one completion per packet. Returns
 * 0 or the item's SGIFailure.
 */
static int sgi_kona_dma_item(SGIKonaState *s, SGIPixelDMA *t,
                             const SGIDMAPageList *pl, uint64_t cookie,
                             bool last)
{
    const SGIDMABus bus = {
        .as = &s->fabric_as,
        .ops = &sgi_kona_dma_head_ops,
        .opaque = s,
    };
    int ret;

    t->seq = s->dma.seq++;
    ret = sgi_dma_pixels_map(t, &bus, pl, &s->mem);
    if (!ret) {
        ret = sgi_dma_pixels_run(t, &s->mem, &sgi_kona_dma_head_ops, s);
    }
    t->cookie = ret || last ? cookie : SGI_KONA_DMA_COOKIE_NONE;
    sgi_dma_pixels_finish(t, ret, &sgi_kona_dma_head_ops, s);
    if (!ret && t->dir == SGI_DMA_TO_PLANE && t->plane == s->plane_front) {
        sgi_scanout_invalidate_plane(&s->scanout,
                                     sgi_board_memory_plane(&s->mem,
                                                            t->plane),
                                     t->y, t->y + t->h - 1);
    }
    return ret;
}

/*
 * The framebuffer end: line r of the buffer is display line top + r,
 * from column x0, where x-test-dma-fb-offset names top and x0. The
 * plane is bottom-origin, so successive lines go down the plane's rows
 * and each is its own item.
 */
static void sgi_kona_dma_framebuffer(SGIKonaState *s,
                                     const SGIDMAPageList *pl,
                                     uint32_t in_page, uint32_t lines,
                                     uint32_t per_line, uint64_t pitch,
                                     uint64_t cookie)
{
    SGIKonaDma *d = &s->dma;
    uint32_t top = d->test_fb_offset / SGI_KONA_FB_WIDTH;
    uint32_t x0 = d->test_fb_offset % SGI_KONA_FB_WIDTH;
    uint32_t w = per_line / 4;

    /*
     * A line no wider than the plane that the placement pushes past its
     * right edge, or lines past its bottom, are the model's to refuse:
     * the shared layer bounds a transfer by the plane's size, not by
     * where the family placed it, and would drop those pixels silently.
     * A line wider than the plane goes on to the shared layer, which
     * refuses it.
     */
    if (top >= SGI_KONA_FB_HEIGHT || lines > SGI_KONA_FB_HEIGHT - top ||
        (w <= SGI_KONA_FB_WIDTH && x0 + w > SGI_KONA_FB_WIDTH)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma of %u lines of %u pixels at "
                      "framebuffer offset %u runs off the framebuffer; "
                      "refusing\n", lines, w, d->test_fb_offset);
        /*
         * The placement is the model's own, so the refusal carries no
         * cookie: a cookie would set the fatal cause bit, on which the
         * driver kills the pipe, for a failure the hardware may not
         * have. A sender that waits times out instead.
         */
        sgi_kona_dma_refuse(s, SGI_KONA_DMA_COOKIE_NONE, SGI_FAIL_BOUNDS);
        return;
    }

    for (uint32_t r = 0; r < lines; r++) {
        SGIPixelDMA t = {
            .dir = d->test_reverse ? SGI_DMA_FROM_PLANE : SGI_DMA_TO_PLANE,
            .plane = s->plane_front,
            .x = x0,
            .y = SGI_KONA_FB_HEIGHT - 1 - (top + r),
            .w = w,
            .h = 1,
            .pitch = per_line,
            .guest_bpp = 4,
            .conversion = d->test_reverse ? SGI_KONA_DMA_CONV_RESOLVE :
                                            SGI_KONA_DMA_CONV_WORDS,
            .logic_op = SGI_LOGIC_COPY,
            .mask = UINT32_MAX,
            .tag_plane = s->plane_front_tag,
            .tag = SGI_KONA_TAG_DIRECT,
            .offset = in_page + r * pitch,
        };

        d->convert_x = t.x;
        d->convert_y = t.y;
        if (sgi_kona_dma_item(s, &t, pl, cookie, r + 1 == lines)) {
            return;
        }
    }
    trace_sgi_kona_dma_transfer(pl->addr[0] + in_page, lines * per_line,
                                d->test_fb_offset, d->test_reverse);
}

/*
 * total bytes at buffer offset `offset` of a page list, into the list
 * plane at pipe-memory byte address dest. The plane's rows are
 * SGI_KONA_LIST_ROW_BYTES long, so the bytes go as up to three items:
 * the rest of a first row, whole rows, and the start of a last. The
 * model refuses a range that is not whole words inside the plane, since
 * the shared layer bounds a transfer by the plane's size, not by where
 * the family placed it.
 */
static void sgi_kona_dma_to_list(SGIKonaState *s, const SGIDMAPageList *pl,
                                 uint64_t offset, uint64_t dest,
                                 uint64_t total, uint64_t cookie)
{
    const SGIPlane *list = sgi_board_memory_plane(&s->mem, s->plane_list);
    uint64_t done = 0;

    if (dest % 4 || total % 4 || dest > list->len ||
        total > list->len - dest) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: 0x%" PRIx64 " bytes at pipe-memory 0x%"
                      PRIx64 " are not whole words inside the 0x%" PRIx64
                      "-byte list plane; refusing\n", total, dest,
                      list->len);
        sgi_kona_dma_refuse(s, cookie, SGI_FAIL_BOUNDS);
        return;
    }

    while (done < total) {
        uint64_t at = dest + done;
        uint32_t col = (at % SGI_KONA_LIST_ROW_BYTES) / 4;
        uint64_t left = total - done;
        uint32_t rows, w;
        SGIPixelDMA t;

        if (col || left < SGI_KONA_LIST_ROW_BYTES) {
            w = MIN(left, SGI_KONA_LIST_ROW_BYTES - col * 4) / 4;
            rows = 1;
        } else {
            w = SGI_KONA_LIST_ROW_BYTES / 4;
            rows = left / SGI_KONA_LIST_ROW_BYTES;
        }
        t = (SGIPixelDMA) {
            .dir = SGI_DMA_TO_PLANE,
            .plane = s->plane_list,
            .x = col,
            .y = at / SGI_KONA_LIST_ROW_BYTES,
            .w = w,
            .h = rows,
            .pitch = w * 4,
            .guest_bpp = 4,
            .conversion = SGI_KONA_DMA_CONV_WORDS,
            .logic_op = SGI_LOGIC_COPY,
            .mask = UINT32_MAX,
            .tag_plane = SGI_PLANE_NONE,
            .offset = offset + done,
        };
        done += (uint64_t)w * 4 * rows;
        if (sgi_kona_dma_item(s, &t, pl, cookie, done == total)) {
            return;
        }
    }
    trace_sgi_kona_dma_transfer(pl->addr[offset / pl->page_size] +
                                offset % pl->page_size, total, dest, false);
}

/*
 * The list end of a packet: its bytes, one line of them, at the pending
 * record's pipe-memory address. kona_dmawrite's one sender sends the
 * record's count as the packet's total and per-line count with no gap, so
 * the model refuses anything else rather than guess how a pipe joins
 * lines or reconciles counts.
 */
static void sgi_kona_dma_list(SGIKonaState *s, const SGIDMAPageList *pl,
                              uint32_t in_page, uint32_t lines,
                              uint32_t total, uint64_t cookie)
{
    SGIKonaDma *d = &s->dma;

    if (lines != 1 || d->record_bytes != total) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma of %u lines, 0x%x bytes, for a list "
                      "write of 0x%x bytes; refusing\n", lines, total,
                      d->record_bytes);
        sgi_kona_dma_refuse(s, cookie, SGI_FAIL_BOUNDS);
        return;
    }
    sgi_kona_dma_to_list(s, pl, in_page, d->record_dest, total, cookie);
}

/*
 * The packet, words 1 to 6. Every size is the guest's: the per-line count
 * must be whole words and divide the total, and the buffer the lines and
 * gaps span must fit SGI_KONA_DMA_MAX_PAGES, or the packet is refused
 * with one FAILED completion. A transfer the shared layer refuses (a line
 * wider than its plane, say) completes FAILED the same way.
 */
static void sgi_kona_dma_packet_run(SGIKonaState *s, const uint32_t *w)
{
    SGIKonaDma *d = &s->dma;
    bool record = d->record_pending;
    bool texture = !record && sgi_kona_texture_dma_waiting(s);
    uint32_t total = w[2], per_line = w[3], gap = w[4];
    uint64_t cookie = SGI_KONA_DMA_COOKIE_NONE;
    g_autofree uint64_t *pages = NULL;
    SGIDMAPageList pl;
    uint64_t io, pitch, span;
    uint32_t in_page, lines;

    d->record_pending = false;

    if (w[5] == SGI_KONA_DMA_DONE_WORD) {
        cookie = SGI_KONA_DMA_COOKIE(SGI_KONA_INT_CAUSE_MAILBOX, w[5]);
    } else if (w[5]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma word 6 is 0x%x, not 0 or 0x%x; no "
                      "completion posted\n", w[5], SGI_KONA_DMA_DONE_WORD);
    }

    if ((w[1] & ~0xfffu) != SGI_KONA_DMA_ADDR_TAG) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma word 2 (0x%08x) carries tag 0x%x, "
                      "expected 0x%x; refusing\n", w[1], w[1] & ~0xfffu,
                      SGI_KONA_DMA_ADDR_TAG);
        sgi_kona_dma_refuse(s, cookie, SGI_FAIL_BUS);
        return;
    }
    io = ((uint64_t)w[0] << 12) | (w[1] & 0xfff);
    in_page = io & (SGI_KONA_DMA_PAGE_SIZE - 1);

    if (!per_line || (!texture && per_line % 4) ||
        !total || total % per_line) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma of 0x%x bytes in lines of 0x%x is not "
                      "whole lines of whole words; refusing\n", total,
                      per_line);
        sgi_kona_dma_refuse(s, cookie, SGI_FAIL_BOUNDS);
        return;
    }
    lines = total / per_line;
    pitch = (uint64_t)per_line + gap;
    span = in_page + (uint64_t)(lines - 1) * pitch + per_line;
    if (span > (uint64_t)SGI_KONA_DMA_MAX_PAGES * SGI_KONA_DMA_PAGE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma of %u lines at a pitch of 0x%" PRIx64
                      " spans more than %u pages; refusing\n", lines, pitch,
                      SGI_KONA_DMA_MAX_PAGES);
        sgi_kona_dma_refuse(s, cookie, SGI_FAIL_BOUNDS);
        return;
    }

    /* The buffer is contiguous in the device address space. */
    pl.npages = DIV_ROUND_UP(span, SGI_KONA_DMA_PAGE_SIZE);
    pl.page_size = SGI_KONA_DMA_PAGE_SIZE;
    pages = g_new(uint64_t, pl.npages);
    for (uint32_t i = 0; i < pl.npages; i++) {
        pages[i] = io - in_page + (uint64_t)i * SGI_KONA_DMA_PAGE_SIZE;
    }
    pl.addr = pages;

    if (record) {
        sgi_kona_dma_list(s, &pl, in_page, lines, total, cookie);
    } else if (texture) {
        /* Publish only after the complete source was read and staged. */
        if (sgi_kona_texture_image_dma(s, io, lines, per_line, pitch) &&
            cookie != SGI_KONA_DMA_COOKIE_NONE) {
            sgi_head_publish(&sgi_kona_dma_head_ops, s, d->seq++,
                             SGI_DONE_WRITTEN, cookie, total);
        }
    } else if (sgi_kona_x_pixel_dma_waiting(s)) {
        /*
         * The X server's image: its bytes feed the open pixel write
         * through the map RAM, and the completion follows the last byte
         * read. A packet that cannot be read completes nothing.
         */
        if (sgi_kona_x_image_dma(s, io, lines, per_line, pitch) &&
            cookie != SGI_KONA_DMA_COOKIE_NONE) {
            sgi_head_publish(&sgi_kona_dma_head_ops, s, d->seq++,
                             SGI_DONE_WRITTEN, cookie, total);
        }
    } else if (d->test_framebuffer) {
        sgi_kona_dma_framebuffer(s, &pl, in_page, lines, per_line, pitch,
                                 cookie);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: dma packet with no list record and no X"
                      " pixel or texture write waiting; no pixels or completion\n");
    }
}

/*
 * 0x1ed page tagged-offset length: the DMA pool kona_dmapool pinned, which
 * list invocations 0x16e and 0x171 and the pull 0x167 read by byte offset.
 * kona_dmapool requires base and length to be multiples of
 * SGI_KONA_DMA_POOL_PAGE, and kona_dmapool_update_pipe sends a length of
 * 1 for a context with no pool (seen in a traced guest run), so a
 * range that is not whole pages leaves no pool registered, traced, not
 * logged. A word 2 without the 0x9000 tag, or a pool longer than the
 * model's bound, is logged as well. The pool is one range of the device
 * address space, as its single address says, and how the pipe resolves
 * its base against the offsets is unknown: the model adds them.
 */
static void sgi_kona_dma_register_pool(SGIKonaState *s, const uint32_t *args)
{
    SGIKonaDma *d = &s->dma;
    uint64_t base = ((uint64_t)args[0] << 12) | (args[1] & 0xfff);

    d->pool_valid = false;
    if ((args[1] & ~0xfffu) != SGI_KONA_DMA_ADDR_TAG ||
        args[2] > SGI_KONA_DMA_POOL_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: dma pool 0x%x 0x%x of 0x%x bytes is not "
                      "tagged, or is longer than 0x%x bytes; no pool\n",
                      args[0], args[1], args[2], SGI_KONA_DMA_POOL_MAX);
    } else if (!(base % SGI_KONA_DMA_POOL_PAGE) && args[2] &&
               !(args[2] % SGI_KONA_DMA_POOL_PAGE)) {
        d->pool_valid = true;
        d->pool_base = base;
        d->pool_len = args[2];
    }
    trace_sgi_kona_dma_pool(base, args[2], d->pool_valid);
}

/* The pool as a page list; pages must be freed by the caller. */
static SGIDMAPageList sgi_kona_dma_pool_pages(SGIKonaState *s,
                                              uint64_t **pages)
{
    SGIKonaDma *d = &s->dma;
    SGIDMAPageList pl = {
        .npages = d->pool_len / SGI_KONA_DMA_POOL_PAGE,
        .page_size = SGI_KONA_DMA_POOL_PAGE,
    };

    *pages = g_new(uint64_t, pl.npages);
    for (uint32_t i = 0; i < pl.npages; i++) {
        (*pages)[i] = d->pool_base + (uint64_t)i * SGI_KONA_DMA_POOL_PAGE;
    }
    pl.addr = *pages;
    return pl;
}

/*
 * A fetched list's words go through the command page's own decoder, the
 * one the GL client's doorbell words reach: a compiled list holds the
 * words immediate mode writes there ("Display lists, vertex arrays and
 * the DMA pool"). The model renders no GL, so the selectors it frames
 * are consumed, not drawn.
 */
static void sgi_kona_dma_fetch_decode(void *opaque, uint32_t context,
                                      const uint32_t *words, unsigned n)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < n; i++) {
        sgi_kona_command_word(s, words[i]);
    }
}

/*
 * The invocation's later ranges, the pairs of a 0x170 or 0x171 after its
 * first. How the pipe runs a list's ranges is the packet's own word, so
 * this walks them in order.
 */
static bool sgi_kona_dma_fetch_next(void *opaque, const SGICommandFetch *f,
                                    uint64_t *offset, uint64_t *nwords)
{
    SGIKonaState *s = opaque;
    SGIKonaDma *d = &s->dma;

    if (!d->nranges) {
        return false;
    }
    *offset = d->ranges[0];
    *nwords = d->ranges[1];
    d->ranges += 2;
    d->nranges--;
    return true;
}

/*
 * Run n (offset, word count) ranges as one list: from the pool, or from
 * pipe memory, the list plane. One CONSUMED completion carries the words
 * fed to the decoder. What the driver observes at a list's end is
 * unknown, so the completion posts nothing: its cookie is
 * SGI_KONA_DMA_COOKIE_NONE. A list that invokes a list finds the fetch
 * running and fails with SGI_FAIL_BUSY, published here under its own
 * sequence number; the outer list goes on.
 */
static void sgi_kona_dma_invoke(SGIKonaState *s, bool pool,
                                const uint32_t *pairs, unsigned n)
{
    SGIKonaDma *d = &s->dma;
    SGICommandFetch *f = &d->fetch;
    uint64_t seq = d->seq++;
    g_autofree uint64_t *pages = NULL;
    int ret;

    if (pool && !d->pool_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: list invoked from the pool with no pool "
                      "registered\n");
        sgi_head_publish(&sgi_kona_dma_head_ops, s, seq, SGI_DONE_FAILED,
                         SGI_KONA_DMA_COOKIE_NONE, SGI_FAIL_BOUNDS);
        return;
    }
    if (!f->open) {
        f->context = s->context_current < 0 ? 0 : s->context_current;
        f->decode = sgi_kona_dma_fetch_decode;
        f->next = sgi_kona_dma_fetch_next;
        f->little_endian = false;
        f->max_words = SGI_KONA_DMA_LIST_MAX_WORDS;
        f->max_segments = SGI_KONA_DMA_LIST_MAX_RANGES;
        f->seq = seq;
        f->cookie = SGI_KONA_DMA_COOKIE_NONE;
        d->ranges = pairs + 2;
        d->nranges = n - 1;
    }
    if (pool) {
        SGIDMAPageList pl = sgi_kona_dma_pool_pages(s, &pages);
        const SGIDMABus bus = {
            .as = &s->fabric_as,
            .ops = &sgi_kona_dma_head_ops,
            .opaque = s,
        };

        ret = sgi_fetch_map(f, &bus, &pl, pairs[0], pairs[1]);
    } else {
        ret = sgi_fetch_map_plane(f, &s->mem, s->plane_list, pairs[0],
                                  pairs[1], s);
    }
    if (!ret) {
        sgi_fetch_run(f, s);
    }
    sgi_fetch_finish(f, ret, &sgi_kona_dma_head_ops, s);
    if (ret == SGI_FAIL_BUSY) {
        sgi_head_publish(&sgi_kona_dma_head_ops, s, seq, SGI_DONE_FAILED,
                         SGI_KONA_DMA_COOKIE_NONE, SGI_FAIL_BUSY);
    }
}

/*
 * 0x167 dest src bytes 0: __glSdramPushIn's copy of a list block from the
 * pool into pipe memory, as a pixel transfer into the list plane. What
 * its fourth word does is unknown; the one sender sends 0.
 */
static void sgi_kona_dma_pull(SGIKonaState *s, const uint32_t *args)
{
    g_autofree uint64_t *pages = NULL;
    SGIDMAPageList pl;

    if (!s->dma.pool_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: list pull with no pool registered\n");
        sgi_kona_dma_refuse(s, SGI_KONA_DMA_COOKIE_NONE, SGI_FAIL_BOUNDS);
        return;
    }
    pl = sgi_kona_dma_pool_pages(s, &pages);
    sgi_kona_dma_to_list(s, &pl, args[1], args[0], args[2],
                         SGI_KONA_DMA_COOKIE_NONE);
}

/*
 * 0x16f dest src length: __glSdramMemmove, the pipe-memory block
 * allocator's move, within the list plane. That the length counts bytes
 * is inferred from its caller's other sizes; overlapping ranges move as
 * memmove() does.
 */
static void sgi_kona_dma_move(SGIKonaState *s, const uint32_t *args)
{
    const SGIPlane *list = sgi_board_memory_plane(&s->mem, s->plane_list);
    uint32_t dest = args[0], src = args[1], len = args[2];
    g_autofree uint8_t *buf = NULL;

    if (dest > list->len || len > list->len - dest ||
        src > list->len || len > list->len - src) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: list move of 0x%x bytes from 0x%x to 0x%x "
                      "leaves the 0x%" PRIx64 "-byte list plane; dropped\n",
                      len, src, dest, list->len);
        return;
    }
    buf = g_malloc(len);
    sgi_board_memory_export(&s->mem, list->base + src, len, buf);
    sgi_board_memory_import(&s->mem, list->base + dest, len, buf);
}

bool sgi_kona_dma_calls_open(SGIKonaState *s)
{
    return s->dma.calls_need != 0;
}

void sgi_kona_dma_calls_drop(SGIKonaState *s)
{
    s->dma.calls_need = 0;
    s->dma.calls_have = 0;
}

void sgi_kona_dma_calls_word(SGIKonaState *s, uint32_t word)
{
    SGIKonaDma *d = &s->dma;
    uint32_t words[2 * SGI_KONA_DMA_LIST_MAX_RANGES];
    unsigned n;

    d->calls_words[d->calls_have++] = word;
    if (d->calls_have < d->calls_need) {
        return;
    }
    /* The words are copied out: a list they invoke may open another. */
    n = d->calls_need / 2;
    memcpy(words, d->calls_words, d->calls_need * sizeof(uint32_t));
    sgi_kona_dma_calls_drop(s);
    sgi_kona_dma_invoke(s, d->calls_token == SGI_KONA_CMD_LIST_CALLS_POOL,
                        words, n);
}

/*
 * 0x170 n or 0x171 n: n pairs follow. An n outside 1 to the packet's
 * limit is logged and opens nothing, so the pairs are read as fresh
 * words.
 */
static void sgi_kona_dma_calls_open_n(SGIKonaState *s, uint32_t token,
                                      uint32_t n)
{
    SGIKonaDma *d = &s->dma;
    uint32_t max = token == SGI_KONA_CMD_LIST_CALLS ?
                   SGI_KONA_LIST_CALLS_MAX : SGI_KONA_LIST_CALLS_POOL_MAX;

    if (!n || n > max) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: list invocation 0x%x names %u ranges, not "
                      "1 to %u\n", token, n, max);
        return;
    }
    d->calls_token = token;
    d->calls_need = 2 * n;
    d->calls_have = 0;
}

void sgi_kona_dma_command(SGIKonaState *s, uint32_t selector,
                          const uint32_t *args)
{
    SGIKonaDma *d = &s->dma;

    switch (selector) {
    case SGI_KONA_DMA_PACKET:
        sgi_kona_dma_packet_run(s, args);
        break;
    case SGI_KONA_CMD_DMA_POOL:
        sgi_kona_dma_register_pool(s, args);
        break;
    case SGI_KONA_CMD_LIST_CALL:
        sgi_kona_dma_invoke(s, false, args, 1);
        break;
    case SGI_KONA_CMD_LIST_CALL_POOL:
        sgi_kona_dma_invoke(s, true, args, 1);
        break;
    case SGI_KONA_CMD_LIST_CALLS:
    case SGI_KONA_CMD_LIST_CALLS_POOL:
        sgi_kona_dma_calls_open_n(s, selector, args[0]);
        break;
    case SGI_KONA_CMD_LIST_PULL:
        sgi_kona_dma_pull(s, args);
        break;
    case SGI_KONA_CMD_LIST_MOVE:
        sgi_kona_dma_move(s, args);
        break;
    case SGI_KONA_DMA_LIST_WRITE:
        if (d->record_pending) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: dma list write 0x%x bytes at 0x%x "
                          "replaces one no packet took\n", args[1],
                          args[0]);
        }
        d->record_pending = true;
        d->record_dest = args[0];
        d->record_bytes = args[1];
        break;
    default:
        g_assert_not_reached();
    }
}

static void sgi_kona_dma_packet(void *opaque, unsigned port_id,
                                uint32_t selector, const uint32_t *args,
                                unsigned argc)
{
    SGIKonaState *s = opaque;

    sgi_kona_port_consumed(s, 1 + argc);

    switch (selector) {
    case SGI_KONA_DMA_PACKET:
    case SGI_KONA_DMA_LIST_WRITE:
    case SGI_KONA_CMD_DMA_POOL:
        sgi_kona_dma_command(s, selector, args);
        break;
    default:
        /*
         * The kernel FIFO table's selectors (sgi_kona_fifo_selectors[])
         * belong to other mechanisms (KONAsetdisplaymode, select-feed):
         * framed so the port stays in step, not modelled.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: dma fifo selector 0x%03x (argc %u) framed, "
                      "not modelled\n", selector, argc);
        break;
    }
}

static void sgi_kona_dma_unsupported(void *opaque, unsigned port_id,
                                     uint32_t word)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-kona: dma fifo word 0x%08x is not a selector the "
                  "port frames; dropped\n", word);
}

static const SGICommandPortOps sgi_kona_dma_ops = {
    .packet = sgi_kona_dma_packet,
    .unsupported = sgi_kona_dma_unsupported,
    /*
     * The kick's literals bracket a packet (0x30000 before the selector,
     * 0x10000 before word 1, 0 after word 6). The words are framed by
     * their selector, so the kick changes nothing; the port keeps the
     * value written.
     */
    .kick = NULL,
    /*
     * Not consulted: SGI_KONA_CREDIT reads sgi_kona_credit(), which counts
     * the words this port's framer holds with every other port's.
     */
    .credit = NULL,
};

void sgi_kona_dma_init(SGIKonaDma *d, SGIKonaState *kona)
{
    static const SGICommandPortSelector own[] = {
        { SGI_KONA_DMA_PACKET, SGI_KONA_DMA_PACKET_WORDS },
        { SGI_KONA_DMA_LIST_WRITE, SGI_KONA_DMA_LIST_WRITE_WORDS },
        { SGI_KONA_CMD_DMA_POOL, 3 },
    };

    assert(ARRAY_SIZE(own) + sgi_kona_fifo_num_selectors <=
           SGI_KONA_DMA_MAX_SELECTORS);
    d->num_selectors = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(own); i++) {
        d->selectors[d->num_selectors++] = own[i];
    }
    for (unsigned i = 0; i < sgi_kona_fifo_num_selectors; i++) {
        d->selectors[d->num_selectors++] = sgi_kona_fifo_selectors[i];
    }
    sgi_command_port_init(&d->fifo_port, &sgi_kona_dma_ops, kona,
                          SGI_KONA_PORT_FIFO, d->selectors,
                          d->num_selectors);
}

void sgi_kona_dma_reset(SGIKonaDma *d)
{
    sgi_command_port_reset(&d->fifo_port);
    d->record_pending = false;
    d->record_dest = 0;
    d->record_bytes = 0;
    d->pool_valid = false;
    d->pool_base = 0;
    d->pool_len = 0;
    sgi_kona_dma_calls_drop(container_of(d, SGIKonaState, dma));
    d->test_last_kind = 0;
    d->test_last_cookie = 0;
    d->test_last_payload = 0;
    d->test_completions = 0;
}

/*
 * object_property_add_bool() has no direct-pointer convenience form (unlike
 * object_property_add_uint32_ptr()), so the boolean test controls need
 * getter/setter pairs; both recover SGIKonaDma through SGI_KONA(obj), the
 * same convention hw/misc/sgi-mgras.c's own get-only x-test-first-ack/
 * x-test-second-ack properties use.
 */
static bool sgi_kona_dma_test_get_framebuffer(Object *obj, Error **errp)
{
    return SGI_KONA(obj)->dma.test_framebuffer;
}

static void sgi_kona_dma_test_set_framebuffer(Object *obj, bool value,
                                             Error **errp)
{
    SGI_KONA(obj)->dma.test_framebuffer = value;
}

static bool sgi_kona_dma_test_get_reverse(Object *obj, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);

    return s->dma.test_reverse;
}

static void sgi_kona_dma_test_set_reverse(Object *obj, bool value,
                                          Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);

    s->dma.test_reverse = value;
}

/*
 * Host-only qtest hooks. Matches hw/misc/sgi-kona.c's own x-test-pixel-x/
 * x-test-pixel-y/x-test-pixel precedent: plain uint32_t/bool properties,
 * excluded from migration. The last completion's kind, cookie and payload
 * are read-only.
 */
void sgi_kona_dma_test_props_init(Object *obj, SGIKonaDma *d)
{
    object_property_add_bool(obj, "x-test-dma-framebuffer",
                             sgi_kona_dma_test_get_framebuffer,
                             sgi_kona_dma_test_set_framebuffer);
    object_property_add_bool(obj, "x-test-dma-reverse",
                             sgi_kona_dma_test_get_reverse,
                             sgi_kona_dma_test_set_reverse);
    object_property_add_uint32_ptr(obj, "x-test-dma-fb-offset",
                                   &d->test_fb_offset,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-dma-completions",
                                   &d->test_completions, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "x-test-dma-last-kind",
                                   &d->test_last_kind, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "x-test-dma-last-cookie",
                                   &d->test_last_cookie, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "x-test-dma-last-payload",
                                   &d->test_last_payload, OBJ_PROP_FLAG_READ);
}

/*
 * The pending list-write record and the item sequence, which arrived with
 * the shared pixel DMA. The port's framing state is in the device's main
 * section. Always sent: a binary from before this subsection refuses
 * every stream this one saves, and one from before it loads here with no
 * record pending.
 *
 * Version 2 adds the DMA pool and a 0x170 or 0x171 still taking its
 * pairs. A version 1 source had neither, so its destination loads with
 * no pool and no invocation open.
 */
static bool sgi_kona_dma_needed(void *opaque)
{
    return true;
}

static int sgi_kona_dma_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;
    SGIKonaDma *d = &s->dma;
    uint32_t max = d->calls_token == SGI_KONA_CMD_LIST_CALLS ?
                   2 * SGI_KONA_LIST_CALLS_MAX :
                   2 * SGI_KONA_LIST_CALLS_POOL_MAX;

    if (d->calls_need &&
        ((d->calls_token != SGI_KONA_CMD_LIST_CALLS &&
          d->calls_token != SGI_KONA_CMD_LIST_CALLS_POOL) ||
         d->calls_need > max || d->calls_need % 2 ||
         d->calls_have >= d->calls_need)) {
        return -EINVAL;
    }
    if (d->pool_valid &&
        (d->pool_base % SGI_KONA_DMA_POOL_PAGE || !d->pool_len ||
         d->pool_len % SGI_KONA_DMA_POOL_PAGE ||
         d->pool_len > SGI_KONA_DMA_POOL_MAX)) {
        return -EINVAL;
    }
    return 0;
}

const VMStateDescription vmstate_sgi_kona_dma = {
    .name = TYPE_SGI_KONA "/dma",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = sgi_kona_dma_needed,
    .post_load = sgi_kona_dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(dma.record_pending, SGIKonaState),
        VMSTATE_UINT32(dma.record_dest, SGIKonaState),
        VMSTATE_UINT32(dma.record_bytes, SGIKonaState),
        VMSTATE_UINT64(dma.seq, SGIKonaState),
        VMSTATE_BOOL_V(dma.pool_valid, SGIKonaState, 2),
        VMSTATE_UINT64_V(dma.pool_base, SGIKonaState, 2),
        VMSTATE_UINT32_V(dma.pool_len, SGIKonaState, 2),
        VMSTATE_UINT32_V(dma.calls_token, SGIKonaState, 2),
        VMSTATE_UINT32_V(dma.calls_need, SGIKonaState, 2),
        VMSTATE_UINT32_V(dma.calls_have, SGIKonaState, 2),
        VMSTATE_UINT32_ARRAY_V(dma.calls_words, SGIKonaState,
                               2 * SGI_KONA_DMA_LIST_MAX_RANGES, 2),
        VMSTATE_END_OF_LIST()
    },
};
