/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics DMA: guest pages to host chunks, and pixel transfers
 * between those chunks and a plane. See the header for the contract.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "hw/display/sgi/pixel-dma.h"
#include "trace.h"

static bool sgi_dma_compose(const SGIDMABus *bus, uint64_t addr, hwaddr *out)
{
    if (bus->ops && bus->ops->dma_address) {
        return bus->ops->dma_address(bus->opaque, addr, out);
    }
    *out = addr;
    return true;
}

static void sgi_dma_trace(const SGIDMABus *bus, bool trace, uint64_t seq,
                          uint64_t addr, const void *bytes, uint32_t len)
{
    if (trace && bus->ops && bus->ops->dma_chunk) {
        bus->ops->dma_chunk(bus->opaque, seq, addr, bytes, len);
    }
}

bool sgi_dma_read(const SGIDMABus *bus, uint64_t addr, void *buf,
                  uint32_t len, bool trace, uint64_t trace_seq)
{
    uint8_t *dst = buf;
    uint32_t done = 0;

    /*
     * No page list here, so chunks break at 4 KiB boundaries of the device
     * address: a traced chunk is then never longer than the contract allows
     * and never crosses a page of any size the families use.
     */
    while (done < len) {
        uint64_t at = addr + done;
        uint32_t n = MIN(len - done,
                         SGI_DMA_CHUNK_MAX - (at & (SGI_DMA_CHUNK_MAX - 1)));
        MemTxResult res;
        hwaddr bus_addr;

        if (!sgi_dma_compose(bus, at, &bus_addr)) {
            return false;
        }
        res = dma_memory_read(bus->as, bus_addr, dst + done, n,
                              MEMTXATTRS_UNSPECIFIED);
        if (res != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: read of %u bytes at "
                          "0x%" HWADDR_PRIx " failed (result %d)\n",
                          n, bus_addr, res);
            return false;
        }
        sgi_dma_trace(bus, trace, trace_seq, at, dst + done, n);
        done += n;
    }
    return true;
}

/*
 * A transfer's chunk count follows sizes the guest chose, so running out
 * of host memory fails the transfer rather than QEMU.
 */
static SGIDMAChunk *sgi_dma_map_append(SGIDMAMap *map)
{
    if (map->nchunks == map->capacity) {
        unsigned capacity;
        SGIDMAChunk *chunks;

        if (map->capacity > UINT_MAX / 2) {
            return NULL;
        }
        capacity = MAX(16, map->capacity * 2);
        chunks = g_try_renew(SGIDMAChunk, map->chunks, capacity);
        if (!chunks) {
            return NULL;
        }
        map->chunks = chunks;
        map->capacity = capacity;
    }
    return &map->chunks[map->nchunks++];
}

uint64_t sgi_dma_page_list_bytes(const SGIDMAPageList *pl)
{
    if (pl->page_size < SGI_DMA_MIN_PAGE_SIZE ||
        !is_power_of_2(pl->page_size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: page size %u is not a "
                      "power of two of at least %u\n", pl->page_size,
                      SGI_DMA_MIN_PAGE_SIZE);
        return 0;
    }
    return (uint64_t)pl->npages * pl->page_size;
}

int sgi_dma_map(const SGIDMABus *bus, SGIDMAMap *map,
                const SGIDMAPageList *pl, uint64_t offset, uint64_t len,
                DMADirection dir, bool trace, uint64_t trace_seq)
{
    uint64_t total = sgi_dma_page_list_bytes(pl);
    uint64_t pos = offset;
    unsigned first = map->nchunks;

    assert(!map->nchunks || (map->as == bus->as && map->dir == dir));
    map->as = bus->as;
    map->dir = dir;

    if (offset > total || len > total - offset) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: bytes 0x%" PRIx64 "+0x%"
                      PRIx64 " are outside a %u-page buffer\n",
                      offset, len, pl->npages);
        return SGI_FAIL_BOUNDS;
    }

    while (pos < offset + len) {
        uint64_t page = pl->addr[pos / pl->page_size];
        uint64_t in_page = pos & (pl->page_size - 1);
        uint64_t addr = page + in_page;
        /*
         * The cut at the page's end is what keeps a chunk off guest memory
         * the driver did not hand over: the next page of the buffer is
         * wherever the list says, not the physically next one.
         */
        dma_addr_t n = MIN(MIN(offset + len - pos, pl->page_size - in_page),
                           SGI_DMA_CHUNK_MAX);
        SGIDMAChunk *c;
        hwaddr bus_addr;
        void *host;

        if (page & (pl->page_size - 1)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: page 0x%" PRIx64 " is "
                          "not aligned to its %u-byte size\n", page,
                          pl->page_size);
            return SGI_FAIL_BOUNDS;
        }
        if (!sgi_dma_compose(bus, addr, &bus_addr)) {
            return SGI_FAIL_BUS;
        }
        c = sgi_dma_map_append(map);
        if (!c) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: no host memory for "
                          "chunk %u of 0x%" PRIx64 " bytes\n",
                          map->nchunks, len);
            return SGI_FAIL_BOUNDS;
        }
        host = dma_memory_map(bus->as, bus_addr, &n, dir,
                              MEMTXATTRS_UNSPECIFIED);
        *c = (SGIDMAChunk) {
            .host = host,
            .len = n,
            .pos = pos,
            .addr = addr,
            .bus_addr = bus_addr,
        };
        if (!host || !n) {
            /* Not RAM, or no map to be had: copy through a bounce buffer. */
            if (host) {
                dma_memory_unmap(bus->as, host, n, dir, 0);
            }
            n = MIN(MIN(offset + len - pos, pl->page_size - in_page),
                    SGI_DMA_CHUNK_MAX);
            c->host = g_try_malloc0(n);
            if (!c->host) {
                map->nchunks--;
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: no host memory "
                              "for a %u-byte bounce buffer\n", (unsigned)n);
                return SGI_FAIL_BOUNDS;
            }
            c->len = n;
            c->bounce = true;
            trace_sgi_dma_bounce(addr, n);
            /*
             * Only a source is read. A buffer the head writes starts empty
             * and only the bytes it wrote go back, so a target that is not
             * RAM sees the writes the head made and no read of its own.
             */
            if (dir == DMA_DIRECTION_TO_DEVICE &&
                dma_memory_read(bus->as, bus_addr, c->host, n,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: read of %u bytes "
                              "at 0x%" HWADDR_PRIx " failed\n",
                              (unsigned)n, bus_addr);
                return SGI_FAIL_BUS;
            }
        }
        if (dir == DMA_DIRECTION_TO_DEVICE) {
            sgi_dma_trace(bus, trace, trace_seq, addr, c->host, c->len);
        }
        pos += c->len;
    }
    trace_sgi_dma_map(trace_seq, dir, offset, len, map->nchunks - first);
    return 0;
}

static uint64_t sgi_dma_map_copy_from(SGIDMAMap *map, unsigned start,
                                      uint64_t pos, uint8_t *p, uint64_t len,
                                      bool to_guest)
{
    uint64_t done = 0;

    for (unsigned i = start; i < map->nchunks && done < len; i++) {
        SGIDMAChunk *c = &map->chunks[i];
        uint64_t at = pos + done;
        uint64_t n;

        if (at < c->pos || at >= c->pos + c->len) {
            continue;
        }
        n = MIN(len - done, c->pos + c->len - at);
        if (to_guest) {
            uint32_t from = at - c->pos;
            uint32_t to = from + n;

            memcpy((uint8_t *)c->host + from, p + done, n);
            if (c->written_end) {
                from = MIN(from, c->written_start);
                to = MAX(to, c->written_end);
            }
            c->written_start = from;
            c->written_end = to;
        } else {
            memcpy(p + done, (uint8_t *)c->host + (at - c->pos), n);
        }
        done += n;
        map->cursor = i;
    }
    return done;
}

bool sgi_dma_map_copy(SGIDMAMap *map, uint64_t pos, void *buf, uint64_t len,
                      bool to_guest)
{
    unsigned start = map->cursor < map->nchunks &&
                     map->chunks[map->cursor].pos <= pos ? map->cursor : 0;

    /*
     * Rows are copied in the order their chunks were mapped, so starting
     * at the chunk the previous copy ended in walks the list about once
     * per transfer instead of once per row. A range the walk from there
     * does not cover is looked for again from the start.
     */
    if (sgi_dma_map_copy_from(map, start, pos, buf, len, to_guest) == len) {
        return true;
    }
    return start &&
           sgi_dma_map_copy_from(map, 0, pos, buf, len, to_guest) == len;
}

void sgi_dma_unmap(SGIDMAMap *map)
{
    for (unsigned i = 0; i < map->nchunks; i++) {
        SGIDMAChunk *c = &map->chunks[i];

        uint32_t start = c->written_start;
        uint32_t len = c->written_end - start;

        /*
         * Whatever the head wrote reaches the guest and its dirty log, even
         * when the transfer failed later on: the guest's memory then shows
         * how far the transfer got. A map's written length counts from its
         * start, so a direct chunk is released with everything up to the
         * last byte written.
         */
        if (!c->bounce) {
            dma_memory_unmap(map->as, c->host, c->len, map->dir,
                             c->written_end);
            continue;
        }
        if (len) {
            MemTxResult res = dma_memory_write(map->as, c->bus_addr + start,
                                               (uint8_t *)c->host + start,
                                               len, MEMTXATTRS_UNSPECIFIED);

            if (res != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: write of %u bytes "
                              "at 0x%" HWADDR_PRIx " failed (result %d)\n",
                              len, c->bus_addr + start, res);
            }
        }
        g_free(c->host);
    }
    g_free(map->chunks);
    memset(map, 0, sizeof(*map));
}

static uint64_t sgi_dma_pixels_row_bytes(const SGIPixelDMA *d)
{
    return (uint64_t)d->w * d->guest_bpp;
}

/*
 * Every size here can come from a guest's descriptor, so the transfer must
 * fit its plane and its page list before anything is mapped or allocated:
 * no wider or taller than the plane, at most SGI_DMA_MAX_ROWS rows, no
 * more bytes moved than the list holds, and the last row's end inside the
 * list. The plane then bounds the bytes moved, and with pages no smaller
 * than a chunk it bounds the chunks too, to each row's bytes over
 * SGI_DMA_CHUNK_MAX plus two. run() checks the plane again, since that is
 * the plane it writes.
 */
static int sgi_dma_pixels_check(const SGIPixelDMA *d,
                                const SGIDMAPageList *pl,
                                const SGIBoardMemory *m)
{
    const SGIPlane *p = sgi_board_memory_plane(m, d->plane);
    uint64_t total = sgi_dma_page_list_bytes(pl);
    uint64_t row = sgi_dma_pixels_row_bytes(d);
    uint64_t moved, end;

    if (!p) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: no plane %u\n", d->plane);
        return SGI_FAIL_BOUNDS;
    }
    if ((uint32_t)d->w > p->width || (uint32_t)d->h > p->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: %dx%d pixels exceed plane "
                      "%u's %ux%u\n", d->w, d->h, d->plane, p->width,
                      p->height);
        return SGI_FAIL_BOUNDS;
    }
    if (d->h > SGI_DMA_MAX_ROWS ||
        umul64_overflow(row, d->h, &moved) || moved > total ||
        umul64_overflow(d->pitch, d->h - 1, &end) ||
        uadd64_overflow(end, row, &end) ||
        uadd64_overflow(end, d->offset, &end) || end > total) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: %dx%d pixels of %u bytes, "
                      "pitch %u, from 0x%" PRIx64 " do not fit a %u-page "
                      "buffer\n", d->w, d->h, d->guest_bpp, d->pitch,
                      d->offset, pl->npages);
        return SGI_FAIL_BOUNDS;
    }
    /*
     * Overlapping rows are fine to read, but written back through bounce
     * buffers the later chunk's copy of the shared bytes would win.
     */
    if (d->dir == SGI_DMA_FROM_PLANE && d->h > 1 && d->pitch < row) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: pitch %u overlaps %" PRIu64
                      "-byte rows written to guest memory\n", d->pitch, row);
        return SGI_FAIL_BOUNDS;
    }
    return 0;
}

int sgi_dma_pixels_map(SGIPixelDMA *d, const SGIDMABus *bus,
                       const SGIDMAPageList *pl, const SGIBoardMemory *m)
{
    DMADirection dir = d->dir == SGI_DMA_TO_PLANE ? DMA_DIRECTION_TO_DEVICE :
                                                    DMA_DIRECTION_FROM_DEVICE;
    uint64_t row = sgi_dma_pixels_row_bytes(d);
    int ret;

    memset(&d->map, 0, sizeof(d->map));
    if (d->w <= 0 || d->h <= 0 || !d->guest_bpp) {
        return 0;
    }
    ret = sgi_dma_pixels_check(d, pl, m);
    if (ret) {
        return ret;
    }
    /* Rows that follow one another map as one range, in fewer chunks. */
    if (d->pitch == row) {
        return sgi_dma_map(bus, &d->map, pl, d->offset, row * d->h, dir,
                           d->trace_chunks, d->trace_seq);
    }
    for (int32_t r = 0; r < d->h; r++) {
        ret = sgi_dma_map(bus, &d->map, pl,
                          d->offset + (uint64_t)r * d->pitch, row, dir,
                          d->trace_chunks, d->trace_seq);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

int sgi_dma_pixels_run(SGIPixelDMA *d, SGIBoardMemory *m,
                       const SGIHeadOps *ops, void *opaque)
{
    const SGIPlane *p = sgi_board_memory_plane(m, d->plane);
    const SGIPlane *tag = NULL;
    uint64_t row = sgi_dma_pixels_row_bytes(d);
    g_autofree uint8_t *bytes = NULL;
    g_autofree uint32_t *words = NULL;
    unsigned pbpp;

    if (d->w <= 0 || d->h <= 0 || !d->guest_bpp) {
        return 0;
    }
    if (!p) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: no plane %u\n", d->plane);
        return SGI_FAIL_BOUNDS;
    }
    if (d->tag_plane != SGI_PLANE_NONE) {
        tag = sgi_board_memory_plane(m, d->tag_plane);
        if (!tag) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: no tag plane %u\n",
                          d->tag_plane);
            return SGI_FAIL_BOUNDS;
        }
    }
    /* A row wider or a transfer taller than the plane has no destination. */
    if ((uint32_t)d->w > p->width || (uint32_t)d->h > p->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: %dx%d pixels exceed plane "
                      "%u's %ux%u\n", d->w, d->h, d->plane, p->width,
                      p->height);
        return SGI_FAIL_BOUNDS;
    }
    pbpp = sgi_plane_bytes_per_pixel(p->format);
    if (!ops->convert_row && d->guest_bpp != pbpp) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: %u-byte guest pixels to "
                      "plane %u's %u-byte pixels need a conversion\n",
                      d->guest_bpp, d->plane, pbpp);
        return SGI_FAIL_FORMAT;
    }

    bytes = g_malloc(row);
    words = g_new(uint32_t, d->w);
    for (int32_t r = 0; r < d->h; r++) {
        uint64_t pos = d->offset + (uint64_t)r * d->pitch;
        int y = d->y + r;

        if (d->dir == SGI_DMA_TO_PLANE) {
            if (!sgi_dma_map_copy(&d->map, pos, bytes, row, false)) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: row %d at 0x%"
                              PRIx64 " was not mapped\n", r, pos);
                return SGI_FAIL_BOUNDS;
            }
            if (ops->convert_row) {
                ops->convert_row(opaque, d->conversion, true, bytes,
                                 (uint8_t *)words, d->w);
            } else {
                for (int32_t i = 0; i < d->w; i++) {
                    words[i] = sgi_plane_decode(p, bytes + i * pbpp);
                }
            }
            for (int32_t i = 0; i < d->w; i++) {
                int x = d->x + i;

                if (!sgi_plane_contains(p, x, y) ||
                    !sgi_clip_contains(&d->clip, x, y)) {
                    continue;
                }
                sgi_plane_merge(m, p, x, y, words[i], d->logic_op, d->mask);
                if (tag) {
                    sgi_plane_store(m, tag, x, y, d->tag);
                }
            }
        } else {
            for (int32_t i = 0; i < d->w; i++) {
                words[i] = sgi_plane_load(m, p, d->x + i, y);
            }
            if (ops->convert_row) {
                ops->convert_row(opaque, d->conversion, false,
                                 (const uint8_t *)words, bytes, d->w);
            } else {
                for (int32_t i = 0; i < d->w; i++) {
                    sgi_plane_encode(p, words[i], bytes + i * pbpp);
                }
            }
            if (!sgi_dma_map_copy(&d->map, pos, bytes, row, true)) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-dma: row %d at 0x%"
                              PRIx64 " was not mapped\n", r, pos);
                return SGI_FAIL_BOUNDS;
            }
        }
    }
    return 0;
}

void sgi_dma_pixels_finish(SGIPixelDMA *d, int status, const SGIHeadOps *ops,
                           void *opaque)
{
    uint64_t moved = d->w > 0 && d->h > 0 ?
                     sgi_dma_pixels_row_bytes(d) * d->h : 0;

    sgi_dma_unmap(&d->map);
    trace_sgi_dma_pixels(d->seq, d->dir, d->plane, d->x, d->y, d->w, d->h,
                         status);
    if (status) {
        sgi_head_publish(ops, opaque, d->seq, SGI_DONE_FAILED, d->cookie,
                         status);
    } else {
        sgi_head_publish(ops, opaque, d->seq, SGI_DONE_WRITTEN, d->cookie,
                         moved);
    }
}

int sgi_dma_pixels(SGIPixelDMA *d, const SGIDMABus *bus,
                   const SGIDMAPageList *pl, SGIBoardMemory *m)
{
    int ret = sgi_dma_pixels_map(d, bus, pl, m);

    if (!ret) {
        ret = sgi_dma_pixels_run(d, m, bus->ops, bus->opaque);
    }
    sgi_dma_pixels_finish(d, ret, bus->ops, bus->opaque);
    return ret;
}
