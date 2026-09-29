/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics board memory and planes. See the header for the model.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "hw/display/sgi/board-memory.h"
#include "trace.h"

void sgi_board_memory_init(SGIBoardMemory *m, uint64_t size)
{
    /* A family constant; the limit is the migration buffer's length. */
    assert(size && size <= UINT32_MAX);
    memset(m, 0, sizeof(*m));
    m->size = size;
    m->bytes_len = size;
    m->bytes = g_malloc0(size);
}

void sgi_board_memory_init_pages(SGIBoardMemory *m, uint32_t npages,
                                 uint32_t page_size)
{
    assert(npages && is_power_of_2(page_size));
    memset(m, 0, sizeof(*m));
    m->size = (uint64_t)npages * page_size;
    m->npages = npages;
    m->page_size = page_size;
    m->pages = g_new0(SGIBoardPage, npages);
}

void sgi_board_memory_set_page(SGIBoardMemory *m, uint32_t i, uint64_t guest,
                               void *host)
{
    assert(m->pages && i < m->npages);
    m->pages[i].guest = guest;
    m->pages[i].host = host;
}

void sgi_board_memory_cleanup(SGIBoardMemory *m)
{
    g_free(m->bytes);
    g_free(m->pages);
    memset(m, 0, sizeof(*m));
}

unsigned sgi_plane_bytes_per_pixel(SGIPlaneFormat format)
{
    switch (format) {
    case SGI_PLANE_CI8:
    case SGI_PLANE_S8:
    case SGI_PLANE_TAG:
    case SGI_PLANE_OVERLAY8:
        return 1;
    case SGI_PLANE_RGB12:
        return 2;
    case SGI_PLANE_RGB24:
    case SGI_PLANE_RGBA8:
    case SGI_PLANE_Z24:
    case SGI_PLANE_TEXTURE:
    case SGI_PLANE_RAW:
        return 4;
    }
    g_assert_not_reached();
}

uint32_t sgi_board_memory_add_plane(SGIBoardMemory *m, const SGIPlane *plane,
                                    Error **errp)
{
    unsigned bpp = sgi_plane_bytes_per_pixel(plane->format);
    uint64_t row = (uint64_t)plane->width * bpp;
    uint64_t need;
    SGIPlane *p;

    if (!plane->width || !plane->height) {
        error_setg(errp, "plane has no pixels");
        return SGI_PLANE_NONE;
    }
    if (plane->stride < row) {
        error_setg(errp, "plane stride %u is shorter than its %" PRIu64
                   "-byte rows", plane->stride, row);
        return SGI_PLANE_NONE;
    }
    need = (uint64_t)(plane->height - 1) * plane->stride + row;
    if (need > plane->len) {
        error_setg(errp, "plane needs %" PRIu64 " bytes but has %" PRIu64,
                   need, plane->len);
        return SGI_PLANE_NONE;
    }
    if (plane->base > m->size || plane->len > m->size - plane->base) {
        error_setg(errp, "plane 0x%" PRIx64 "+0x%" PRIx64 " is outside "
                   "0x%" PRIx64 " bytes of board memory",
                   plane->base, plane->len, m->size);
        return SGI_PLANE_NONE;
    }

    if (m->nplanes == SGI_PLANES_MAX) {
        error_setg(errp, "board memory already has %d planes",
                   SGI_PLANES_MAX);
        return SGI_PLANE_NONE;
    }
    p = &m->planes[m->nplanes];
    *p = *plane;
    p->id = m->nplanes++;
    trace_sgi_plane_add(p->id, p->base, p->len, p->width, p->height,
                        p->stride, p->format);
    return p->id;
}

const SGIPlane *sgi_board_memory_plane(const SGIBoardMemory *m, uint32_t id)
{
    return id < m->nplanes ? &m->planes[id] : NULL;
}

bool sgi_clip_contains(const SGIClipList *clip, int x, int y)
{
    if (!clip->n) {
        return !clip->empty;
    }
    for (uint32_t i = 0; i < MIN(clip->n, SGI_CLIP_MAX); i++) {
        const SGIClipRect *r = &clip->r[i];

        if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1) {
            return true;
        }
    }
    return false;
}

uint32_t sgi_logic_op(uint8_t op, uint32_t s, uint32_t d)
{
    switch (op & 0xf) {
    case SGI_LOGIC_CLEAR:
        return 0;
    case SGI_LOGIC_AND:
        return s & d;
    case SGI_LOGIC_AND_REVERSE:
        return s & ~d;
    case SGI_LOGIC_COPY:
        return s;
    case SGI_LOGIC_AND_INVERTED:
        return ~s & d;
    case SGI_LOGIC_NOOP:
        return d;
    case SGI_LOGIC_XOR:
        return s ^ d;
    case SGI_LOGIC_OR:
        return s | d;
    case SGI_LOGIC_NOR:
        return ~(s | d);
    case SGI_LOGIC_EQUIV:
        return ~(s ^ d);
    case SGI_LOGIC_INVERT:
        return ~d;
    case SGI_LOGIC_OR_REVERSE:
        return s | ~d;
    case SGI_LOGIC_COPY_INVERTED:
        return ~s;
    case SGI_LOGIC_OR_INVERTED:
        return ~s | d;
    case SGI_LOGIC_NAND:
        return ~(s & d);
    default: /* SGI_LOGIC_SET */
        return ~0u;
    }
}

static bool sgi_plane_is_le(const SGIPlane *p)
{
    return p->packing == SGI_PACK_LE_RED_LOW ||
           p->packing == SGI_PACK_LE_RED_HIGH;
}

uint32_t sgi_plane_decode(const SGIPlane *p, const uint8_t *bytes)
{
    switch (sgi_plane_bytes_per_pixel(p->format)) {
    case 1:
        return bytes[0];
    case 2:
        return sgi_plane_is_le(p) ? lduw_le_p(bytes) : lduw_be_p(bytes);
    default:
        return sgi_plane_is_le(p) ? ldl_le_p(bytes) : ldl_be_p(bytes);
    }
}

void sgi_plane_encode(const SGIPlane *p, uint32_t word, uint8_t *bytes)
{
    switch (sgi_plane_bytes_per_pixel(p->format)) {
    case 1:
        bytes[0] = word;
        break;
    case 2:
        if (sgi_plane_is_le(p)) {
            stw_le_p(bytes, word);
        } else {
            stw_be_p(bytes, word);
        }
        break;
    default:
        if (sgi_plane_is_le(p)) {
            stl_le_p(bytes, word);
        } else {
            stl_be_p(bytes, word);
        }
        break;
    }
}

/*
 * The host bytes behind [base, base + len), when they are contiguous on the
 * host: always for a device-backed memory, within one resolved page for a
 * page-backed one. NULL otherwise, and the caller goes byte-range by page.
 */
static uint8_t *sgi_board_memory_host(const SGIBoardMemory *m, uint64_t base,
                                      uint64_t len)
{
    uint64_t in_page;
    uint8_t *host;

    if (base > m->size || len > m->size - base) {
        return NULL;
    }
    if (m->bytes) {
        return m->bytes + base;
    }
    in_page = base & (m->page_size - 1);
    if (in_page + len > m->page_size) {
        return NULL;
    }
    host = m->pages[base / m->page_size].host;
    return host ? host + in_page : NULL;
}

static bool sgi_board_memory_copy(const SGIBoardMemory *m, uint64_t base,
                                  uint64_t len, uint8_t *buf, bool out)
{
    uint64_t done = 0;

    if (base > m->size || len > m->size - base) {
        return false;
    }
    if (m->bytes) {
        if (out) {
            memcpy(buf, m->bytes + base, len);
        } else {
            memcpy(m->bytes + base, buf, len);
        }
        return true;
    }
    /* Check every page first so a failed copy copies nothing. */
    for (uint64_t a = base & ~(uint64_t)(m->page_size - 1); a < base + len;
         a += m->page_size) {
        if (!m->pages[a / m->page_size].host) {
            return false;
        }
    }
    while (done < len) {
        uint64_t at = base + done;
        uint64_t n = MIN(len - done, m->page_size - (at & (m->page_size - 1)));
        uint8_t *host = sgi_board_memory_host(m, at, n);

        if (out) {
            memcpy(buf + done, host, n);
        } else {
            memcpy(host, buf + done, n);
        }
        done += n;
    }
    return true;
}

bool sgi_board_memory_export(const SGIBoardMemory *m, uint64_t base,
                             uint64_t len, void *dst)
{
    return sgi_board_memory_copy(m, base, len, dst, true);
}

bool sgi_board_memory_import(SGIBoardMemory *m, uint64_t base, uint64_t len,
                             const void *src)
{
    return sgi_board_memory_copy(m, base, len, (uint8_t *)src, false);
}

static uint64_t sgi_plane_offset(const SGIPlane *p, int x, int y)
{
    return p->base + (uint64_t)y * p->stride +
           (uint64_t)x * sgi_plane_bytes_per_pixel(p->format);
}

uint32_t sgi_plane_load(const SGIBoardMemory *m, const SGIPlane *p,
                        int x, int y)
{
    unsigned bpp = sgi_plane_bytes_per_pixel(p->format);
    uint8_t bytes[4];
    uint64_t at;
    uint8_t *host;

    if (!sgi_plane_contains(p, x, y)) {
        return 0;
    }
    at = sgi_plane_offset(p, x, y);
    host = sgi_board_memory_host(m, at, bpp);
    if (host) {
        return sgi_plane_decode(p, host);
    }
    if (!sgi_board_memory_export(m, at, bpp, bytes)) {
        return 0;
    }
    return sgi_plane_decode(p, bytes);
}

void sgi_plane_store(SGIBoardMemory *m, const SGIPlane *p, int x, int y,
                     uint32_t word)
{
    unsigned bpp = sgi_plane_bytes_per_pixel(p->format);
    uint8_t bytes[4];
    uint64_t at;
    uint8_t *host;

    if (!sgi_plane_contains(p, x, y)) {
        return;
    }
    at = sgi_plane_offset(p, x, y);
    host = sgi_board_memory_host(m, at, bpp);
    if (host) {
        sgi_plane_encode(p, word, host);
        return;
    }
    sgi_plane_encode(p, word, bytes);
    sgi_board_memory_import(m, at, bpp, bytes);
}

void sgi_plane_merge(SGIBoardMemory *m, const SGIPlane *p, int x, int y,
                     uint32_t src, uint8_t op, uint32_t mask)
{
    uint32_t dst = sgi_plane_load(m, p, x, y);
    uint32_t val = sgi_logic_op(op, src, dst);

    sgi_plane_store(m, p, x, y, (dst & ~mask) | (val & mask));
}

static const VMStateDescription vmstate_sgi_board_page = {
    .name = "sgi-board-page",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(guest, SGIBoardPage),
        VMSTATE_END_OF_LIST()
    },
};

static int sgi_board_memory_post_load(void *opaque, int version_id)
{
    SGIBoardMemory *m = opaque;

    for (uint32_t i = 0; i < m->npages; i++) {
        m->pages[i].host = NULL;
    }
    return 0;
}

const VMStateDescription vmstate_sgi_board_memory = {
    .name = "sgi-board-memory",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_board_memory_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(size, SGIBoardMemory),
        VMSTATE_UINT32_EQUAL(nplanes, SGIBoardMemory),
        /* Refuses a stream from a board of another size. */
        VMSTATE_UINT32_EQUAL(bytes_len, SGIBoardMemory),
        VMSTATE_VBUFFER_UINT32(bytes, SGIBoardMemory, 0, NULL, bytes_len),
        VMSTATE_UINT32_EQUAL(npages, SGIBoardMemory),
        VMSTATE_STRUCT_VARRAY_POINTER_UINT32(pages, SGIBoardMemory, npages,
                                             vmstate_sgi_board_page,
                                             SGIBoardPage),
        VMSTATE_END_OF_LIST()
    },
};
