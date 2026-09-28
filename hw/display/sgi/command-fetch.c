/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics command fetch. See the header for the contract and for what
 * is known and not known about the first user's pool format.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "hw/display/sgi/command-fetch.h"
#include "trace.h"

static uint64_t sgi_fetch_max_words(const SGICommandFetch *f)
{
    return f->max_words ? f->max_words : SGI_FETCH_MAX_WORDS;
}

static unsigned sgi_fetch_max_segments(const SGICommandFetch *f)
{
    return f->max_segments ? f->max_segments : SGI_FETCH_MAX_SEGMENTS;
}

/*
 * A range may take at most what is left of the source's words and of the
 * fetch's own word cap, and no more than one segment can count. A range
 * of no words adds nothing, and in particular does not look up a page that
 * may not exist; any other range needs a segment under the segment cap.
 * All of this is checked before the range maps or allocates anything.
 */
static int sgi_fetch_check(SGICommandFetch *f, uint64_t nwords,
                           uint64_t *budget, const char *source)
{
    uint64_t max_words = sgi_fetch_max_words(f);
    unsigned max_segments = sgi_fetch_max_segments(f);

    if (nwords > *budget || nwords > UINT32_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: %" PRIu64 " more words "
                      "exceed the %s\n", nwords, source);
        return SGI_FAIL_BOUNDS;
    }
    if (f->nwords > max_words || nwords > max_words - f->nwords) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: %" PRIu64 " more words "
                      "exceed context %u's cap of %" PRIu64 "\n",
                      nwords, f->context, max_words);
        return SGI_FAIL_BOUNDS;
    }
    if (nwords && f->nsegments >= max_segments) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: context %u's list runs "
                      "past its cap of %u ranges\n", f->context,
                      max_segments);
        return SGI_FAIL_BOUNDS;
    }
    *budget -= nwords;
    return 0;
}

/*
 * Room for one more segment, and nwords words for it, without adding it.
 * The array doubles, so a list of many ranges costs few reallocations, and
 * never grows past the segment cap. The sizes are the guest's, so a host
 * that cannot supply them fails the invocation rather than QEMU.
 */
static int sgi_fetch_alloc(SGICommandFetch *f, uint64_t nwords,
                           uint32_t **words)
{
    if (f->nsegments == f->capacity) {
        uint64_t capacity = MIN(MAX(16, (uint64_t)f->capacity * 2),
                                sgi_fetch_max_segments(f));
        SGIFetchSegment *segments = g_try_renew(SGIFetchSegment,
                                                f->segments, capacity);

        if (!segments) {
            goto fail;
        }
        f->segments = segments;
        f->capacity = capacity;
    }
    *words = g_try_new(uint32_t, nwords);
    if (*words) {
        return 0;
    }
fail:
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: no host memory for context "
                  "%u's %" PRIu64 " more words\n", f->context, nwords);
    return SGI_FAIL_BOUNDS;
}

/*
 * Keep a range's words, read into words in memory order, as a new segment
 * in host order. sgi_fetch_alloc() made the room.
 */
static void sgi_fetch_take(SGICommandFetch *f, uint32_t *words,
                           uint64_t nwords, uint64_t addr, bool little_endian)
{
    for (uint64_t i = 0; i < nwords; i++) {
        words[i] = little_endian ? ldl_le_p(&words[i]) : ldl_be_p(&words[i]);
    }
    f->segments[f->nsegments++] = (SGIFetchSegment) {
        .words = words,
        .nwords = nwords,
        .addr = addr,
    };
    f->nwords += nwords;
}

static int sgi_fetch_add(SGICommandFetch *f, const SGIDMABus *bus,
                         const SGIDMAPageList *pl, uint64_t offset,
                         uint64_t nwords, uint64_t *budget)
{
    SGIDMAMap map = { };
    g_autofree uint32_t *words = NULL;
    int ret = sgi_fetch_check(f, nwords, budget, "pool");

    if (ret || !nwords) {
        return ret;
    }
    ret = sgi_fetch_alloc(f, nwords, &words);
    if (ret) {
        return ret;
    }
    ret = sgi_dma_map(bus, &map, pl, offset, nwords * 4,
                      DMA_DIRECTION_TO_DEVICE, f->trace_chunks, f->trace_seq);
    if (!ret && !sgi_dma_map_copy(&map, offset, words, nwords * 4, false)) {
        ret = SGI_FAIL_BOUNDS;
    }
    sgi_dma_unmap(&map);
    if (!ret) {
        sgi_fetch_take(f, g_steal_pointer(&words), nwords,
                       pl->addr[offset / pl->page_size] +
                       (offset & (pl->page_size - 1)), f->little_endian);
    }
    return ret;
}

static int sgi_fetch_begin(SGICommandFetch *f)
{
    /*
     * A fetch not yet finished is one being run, perhaps by the decode()
     * below us on the stack when a list invokes a list. The guest's words
     * choose that, so it fails the inner invocation and leaves the outer
     * one's segments, count and run alone.
     */
    if (f->open++) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: context %u's fetch is "
                      "still running a list\n", f->context);
        return SGI_FAIL_BUSY;
    }
    f->nwords = 0;
    return 0;
}

int sgi_fetch_map(SGICommandFetch *f, const SGIDMABus *bus,
                  const SGIDMAPageList *pl, uint64_t offset, uint64_t nwords)
{
    uint64_t budget;
    int ret = sgi_fetch_begin(f);

    if (ret) {
        return ret;
    }
    budget = sgi_dma_page_list_bytes(pl) / 4;
    ret = sgi_fetch_add(f, bus, pl, offset, nwords, &budget);
    /*
     * The budget bounds a next() that keeps asking; a range of no words
     * ends the fetch, so neither can loop.
     */
    while (!ret && f->next && f->next(bus->opaque, f, &offset, &nwords) &&
           nwords) {
        ret = sgi_fetch_add(f, bus, pl, offset, nwords, &budget);
    }
    return ret;
}

static int sgi_fetch_add_plane(SGICommandFetch *f, const SGIBoardMemory *m,
                               const SGIPlane *p, uint64_t offset,
                               uint64_t nwords, uint64_t *budget)
{
    g_autofree uint32_t *words = NULL;
    int ret = sgi_fetch_check(f, nwords, budget, "plane");

    if (ret || !nwords) {
        return ret;
    }
    if (offset > p->len || nwords > (p->len - offset) / 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: %" PRIu64 " words at "
                      "0x%" PRIx64 " are outside plane %u\n",
                      nwords, offset, p->id);
        return SGI_FAIL_BOUNDS;
    }
    ret = sgi_fetch_alloc(f, nwords, &words);
    if (ret) {
        return ret;
    }
    if (!sgi_board_memory_export(m, p->base + offset, nwords * 4, words)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: plane %u's words at 0x%"
                      PRIx64 " are not resolved\n", p->id, offset);
        return SGI_FAIL_BOUNDS;
    }
    sgi_fetch_take(f, g_steal_pointer(&words), nwords, p->base + offset,
                   p->packing == SGI_PACK_LE_RED_LOW ||
                   p->packing == SGI_PACK_LE_RED_HIGH);
    return 0;
}

int sgi_fetch_map_plane(SGICommandFetch *f, const SGIBoardMemory *m,
                        uint32_t plane, uint64_t offset, uint64_t nwords,
                        void *opaque)
{
    const SGIPlane *p = sgi_board_memory_plane(m, plane);
    uint64_t budget;
    int ret = sgi_fetch_begin(f);

    if (ret) {
        return ret;
    }
    if (!p) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: no plane %u\n", plane);
        return SGI_FAIL_BOUNDS;
    }
    if (sgi_plane_bytes_per_pixel(p->format) != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-fetch: plane %u is not a plane "
                      "of 32-bit words\n", plane);
        return SGI_FAIL_FORMAT;
    }
    budget = p->len / 4;
    ret = sgi_fetch_add_plane(f, m, p, offset, nwords, &budget);
    while (!ret && f->next && f->next(opaque, f, &offset, &nwords) && nwords) {
        ret = sgi_fetch_add_plane(f, m, p, offset, nwords, &budget);
    }
    return ret;
}

void sgi_fetch_run(SGICommandFetch *f, void *opaque)
{
    for (unsigned i = 0; i < f->nsegments; i++) {
        f->decode(opaque, f->context, f->segments[i].words,
                  f->segments[i].nwords);
    }
}

void sgi_fetch_finish(SGICommandFetch *f, int status, const SGIHeadOps *ops,
                      void *opaque)
{
    /*
     * The finish of a refused map only closes it. seq and cookie are the
     * running invocation's, so a record here would say that one failed;
     * the refused invocation's identity is the caller's to publish.
     */
    if (f->open > 1) {
        f->open--;
        return;
    }
    f->open = 0;
    trace_sgi_fetch(f->seq, f->context, f->nsegments, f->nwords, status);
    for (unsigned i = 0; i < f->nsegments; i++) {
        g_free(f->segments[i].words);
    }
    g_free(f->segments);
    f->segments = NULL;
    f->nsegments = 0;
    f->capacity = 0;
    if (status) {
        sgi_head_publish(ops, opaque, f->seq, SGI_DONE_FAILED, f->cookie,
                         status);
    } else {
        sgi_head_publish(ops, opaque, f->seq, SGI_DONE_CONSUMED, f->cookie,
                         f->nwords);
    }
}

int sgi_fetch(SGICommandFetch *f, const SGIDMABus *bus,
              const SGIDMAPageList *pl, uint64_t offset, uint64_t nwords)
{
    int ret = sgi_fetch_map(f, bus, pl, offset, nwords);

    if (!ret) {
        sgi_fetch_run(f, bus->opaque);
    }
    sgi_fetch_finish(f, ret, bus->ops, bus->opaque);
    return ret;
}
