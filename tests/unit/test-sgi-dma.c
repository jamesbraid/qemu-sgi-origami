/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics DMA and command fetch: guest pages resolved to host chunks,
 * the chunk trace contract, pixel transfers both ways, completions, and a
 * display-list fetch over the page format measured on MGRAS.
 *
 * Guest memory here is a sparse set of 4 KiB pages behind the three
 * address-space entry points the DMA helpers reach (map, unmap, rw). A map
 * runs to the end of the fake's own 4 KiB page, so a page list with smaller
 * pages shows whether a chunk stops at the list's page. The head composes a
 * device address into a bus address by adding BUS_OFFSET, the way a head
 * adds a widget number, so a test can tell the address a trace names from
 * the address the bus saw.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/dma.h"
#include "hw/display/sgi/command-fetch.h"
#include "hw/display/sgi/pixel-dma.h"

#define PAGE 4096
#define BUS_OFFSET 0x80000000000ull
/* Device addresses in [HOLE, HOLE + PAGE) read and write but do not map. */
#define HOLE 0x700000ull
/* Device addresses in [BAD, BAD + PAGE) fail every transaction. */
#define BAD 0x710000ull

static GHashTable *ram;
static int maps_outstanding;
static int maps_made;
static uint64_t unmapped_written;
static int hole_reads;
static AddressSpace fake_as;

static uint8_t *ram_page(uint64_t addr)
{
    uint64_t key = addr / PAGE;
    uint8_t *page = g_hash_table_lookup(ram, &key);

    if (!page) {
        uint64_t *k = g_new(uint64_t, 1);

        *k = key;
        page = g_malloc0(PAGE);
        g_hash_table_insert(ram, k, page);
    }
    return page;
}

static void ram_write(uint64_t addr, const void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ram_page(addr + i)[(addr + i) % PAGE] = ((const uint8_t *)buf)[i];
    }
}

static uint8_t ram_byte(uint64_t addr)
{
    return ram_page(addr)[addr % PAGE];
}

void *address_space_map(AddressSpace *as, hwaddr addr, hwaddr *plen,
                        bool is_write, MemTxAttrs attrs)
{
    uint64_t dev = addr - BUS_OFFSET;

    g_assert(as == &fake_as);
    if (addr < BUS_OFFSET || (dev >= HOLE && dev < BAD + PAGE)) {
        *plen = 0;
        return NULL;
    }
    *plen = MIN(*plen, PAGE - dev % PAGE);
    maps_outstanding++;
    maps_made++;
    return ram_page(dev) + dev % PAGE;
}

void address_space_unmap(AddressSpace *as, void *buffer, hwaddr len,
                         bool is_write, hwaddr access_len)
{
    maps_outstanding--;
    if (is_write) {
        unmapped_written += access_len;
    }
}

MemTxResult address_space_rw(const AddressSpace *as, hwaddr addr,
                             MemTxAttrs attrs, void *buf, hwaddr len,
                             bool is_write)
{
    uint64_t dev = addr - BUS_OFFSET;

    if (addr < BUS_OFFSET || (dev >= BAD && dev < BAD + PAGE)) {
        return MEMTX_DECODE_ERROR;
    }
    if (!is_write && dev < HOLE + PAGE && dev + len > HOLE) {
        hole_reads++;
    }
    for (hwaddr i = 0; i < len; i++) {
        uint8_t *b = &ram_page(dev + i)[(dev + i) % PAGE];

        if (is_write) {
            *b = ((uint8_t *)buf)[i];
        } else {
            ((uint8_t *)buf)[i] = *b;
        }
    }
    return MEMTX_OK;
}

typedef struct Chunk {
    uint64_t seq, addr;
    uint32_t len;
    uint8_t bytes[SGI_DMA_CHUNK_MAX];
} Chunk;

typedef struct Head {
    GArray *chunks;             /* of Chunk */
    GArray *done;               /* of SGICompletion */
    GArray *decoded;            /* of uint32_t */
    unsigned decode_calls;
    unsigned next_calls;
    uint64_t next_offset, next_words;
    unsigned next_limit;
} Head;

static bool head_dma_address(void *opaque, uint64_t addr, hwaddr *bus)
{
    *bus = addr + BUS_OFFSET;
    return true;
}

static void head_dma_chunk(void *opaque, uint64_t seq, uint64_t addr,
                           const uint8_t *bytes, uint32_t len)
{
    Head *h = opaque;
    Chunk c = { .seq = seq, .addr = addr, .len = len };

    g_assert_cmpuint(len, <=, SGI_DMA_CHUNK_MAX);
    memcpy(c.bytes, bytes, len);
    g_array_append_val(h->chunks, c);
}

static void head_publish(void *opaque, const SGICompletion *c)
{
    Head *h = opaque;

    g_array_append_val(h->done, *c);
}

/*
 * A family conversion: guest bytes are one-byte indices, plane words carry
 * the index in bits 7:0 under a fixed tag in 31:24.
 */
static void head_convert_row(void *opaque, uint8_t conversion, bool to_plane,
                             const uint8_t *src, uint8_t *dst,
                             unsigned pixels)
{
    g_assert_cmpuint(conversion, ==, 7);
    for (unsigned i = 0; i < pixels; i++) {
        if (to_plane) {
            ((uint32_t *)dst)[i] = 0xab000000 | src[i];
        } else {
            dst[i] = ((const uint32_t *)src)[i];
        }
    }
}

static const SGIHeadOps head_ops = {
    .publish = head_publish,
    .dma_address = head_dma_address,
    .dma_chunk = head_dma_chunk,
};

static const SGIHeadOps head_ops_convert = {
    .publish = head_publish,
    .convert_row = head_convert_row,
    .dma_address = head_dma_address,
    .dma_chunk = head_dma_chunk,
};

static Head *head_new(void)
{
    Head *h = g_new0(Head, 1);

    h->chunks = g_array_new(false, true, sizeof(Chunk));
    h->done = g_array_new(false, true, sizeof(SGICompletion));
    h->decoded = g_array_new(false, true, sizeof(uint32_t));
    return h;
}

static void head_free(Head *h)
{
    g_array_free(h->chunks, true);
    g_array_free(h->done, true);
    g_array_free(h->decoded, true);
    g_free(h);
}

static void setup(void)
{
    if (ram) {
        g_hash_table_destroy(ram);
    }
    ram = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    maps_outstanding = 0;
    maps_made = 0;
    unmapped_written = 0;
    hole_reads = 0;
}

static SGIDMABus bus_for(Head *h, const SGIHeadOps *ops)
{
    return (SGIDMABus) { .as = &fake_as, .ops = ops, .opaque = h };
}

/* A 64x16 plane of big-endian 32-bit words in a device-backed memory. */
static void plane_setup(SGIBoardMemory *m, SGIPlaneFormat format)
{
    SGIPlane p = {
        .base = 0, .width = 64, .height = 16,
        .stride = 64 * sgi_plane_bytes_per_pixel(format),
        .format = format, .packing = SGI_PACK_BE_RED_LOW,
        .evidence = "unit test",
    };

    p.len = p.stride * p.height;
    sgi_board_memory_init(m, 2 * p.len);
    sgi_board_memory_add_plane(m, &p, &error_abort);
    /* A tag plane after it. */
    p.base = p.len;
    p.format = SGI_PLANE_TAG;
    p.stride = 64;
    p.len = 64 * 16;
    sgi_board_memory_add_plane(m, &p, &error_abort);
}

/*
 * A page list read from guest memory is traced like any source: one chunk
 * at the device address, not the bus address, with the given sequence.
 * A read across a 4 KiB boundary is two chunks.
 */
static void test_read_traced(void)
{
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    uint8_t want[64], got[64];
    Chunk *c;

    setup();
    for (unsigned i = 0; i < sizeof(want); i++) {
        want[i] = i ^ 0x5a;
    }
    ram_write(0x4d74018, want, 32);
    g_assert_true(sgi_dma_read(&bus, 0x4d74018, got, 32, true, 41));
    g_assert_cmpmem(got, 32, want, 32);
    g_assert_cmpuint(h->chunks->len, ==, 1);
    c = &g_array_index(h->chunks, Chunk, 0);
    g_assert_cmphex(c->addr, ==, 0x4d74018);
    g_assert_cmpuint(c->len, ==, 32);
    g_assert_cmpuint(c->seq, ==, 41);
    g_assert_cmpmem(c->bytes, 32, want, 32);

    ram_write(0x10ff0, want, 64);
    g_assert_true(sgi_dma_read(&bus, 0x10ff0, got, 64, true, 42));
    g_assert_cmpmem(got, 64, want, 64);
    g_assert_cmpuint(h->chunks->len, ==, 3);
    c = &g_array_index(h->chunks, Chunk, 1);
    g_assert_cmphex(c->addr, ==, 0x10ff0);
    g_assert_cmpuint(c->len, ==, 16);
    c = &g_array_index(h->chunks, Chunk, 2);
    g_assert_cmphex(c->addr, ==, 0x11000);
    g_assert_cmpuint(c->len, ==, 48);

    /* Untraced: the bytes arrive and the hook hears nothing. */
    g_assert_true(sgi_dma_read(&bus, 0x4d74018, got, 32, false, 43));
    g_assert_cmpuint(h->chunks->len, ==, 3);

    g_assert_false(sgi_dma_read(&bus, BAD, got, 8, true, 44));
    head_free(h);
}

/*
 * A transfer to a plane whose rows cross guest pages, over a page list
 * whose pages are out of order: each row lands whole and the chunks each
 * name one page.
 */
static void test_rows_across_pages(void)
{
    static const uint64_t pages[3] = { 0x30000, 0x10000, 0x20000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 3, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 4, .y = 2, .w = 8, .h = 3,
        .pitch = 40, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
        .trace_chunks = true, .trace_seq = 77,
        .seq = 5, .cookie = 0xc0ffee,
        .offset = PAGE - 48,    /* row 1 starts 8 bytes before page 1 */
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 8; i++) {
            uint64_t pos = d.offset + r * 40 + i * 4;
            uint8_t word[4];

            stl_be_p(word, 0x1000 * (r + 1) + i);
            for (int b = 0; b < 4; b++) {
                ram_write(pages[(pos + b) / PAGE] + (pos + b) % PAGE,
                          &word[b], 1);
            }
        }
    }

    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 8; i++) {
            g_assert_cmphex(sgi_plane_load(&m, p, 4 + i, 2 + r), ==,
                            0x1000 * (r + 1) + i);
        }
        g_assert_cmphex(sgi_plane_load(&m, p, 3, 2 + r), ==, 0);
        g_assert_cmphex(sgi_plane_load(&m, p, 12, 2 + r), ==, 0);
    }
    /* Row 0 in page 0; row 1 split 8 + 24 at the boundary; row 2 page 1. */
    g_assert_cmpuint(h->chunks->len, ==, 4);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 0).addr, ==,
                    0x30000 + PAGE - 48);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 1).addr, ==,
                    0x30000 + PAGE - 8);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 1).len, ==, 8);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 2).addr, ==, 0x10000);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 2).len, ==, 24);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 3).addr, ==,
                    0x10000 + 32);
    for (unsigned i = 0; i < h->chunks->len; i++) {
        g_assert_cmpuint(g_array_index(h->chunks, Chunk, i).seq, ==, 77);
    }
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * Contiguous rows are one range cut into chunks of at most 4096 bytes that
 * never cross a page, and the chunks carry exactly the source bytes.
 */
static void test_chunking(void)
{
    static const uint64_t pages[4] = { 0x40000, 0x41000, 0x42000, 0x43000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 4, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 64, .h = 16,
        .pitch = 256, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
        .trace_chunks = true, .trace_seq = 9,
        .offset = 100,
    };
    static const uint32_t want_len[2] = { PAGE - 100, 100 };
    uint64_t total = 0;

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    for (uint64_t pos = 0; pos < 4 * PAGE; pos++) {
        uint8_t b = pos * 13 + 1;

        ram_write(pages[pos / PAGE] + pos % PAGE, &b, 1);
    }

    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    /* 16 rows of 256 bytes from offset 100: 3996 to the page's end, 100. */
    g_assert_cmpuint(h->chunks->len, ==, 2);
    for (unsigned i = 0; i < h->chunks->len; i++) {
        Chunk *c = &g_array_index(h->chunks, Chunk, i);
        uint64_t pos = 100 + total;

        g_assert_cmpuint(c->len, ==, want_len[i]);
        g_assert_cmphex(c->addr, ==, pages[pos / PAGE] + pos % PAGE);
        for (uint32_t b = 0; b < c->len; b++) {
            g_assert_cmpuint(c->bytes[b], ==, (uint8_t)((pos + b) * 13 + 1));
        }
        total += c->len;
    }
    g_assert_cmpuint(total, ==, 16 * 256);
    /* Bytes 100..103 of the pattern: (pos * 13 + 1) & 0xff. */
    g_assert_cmphex(sgi_plane_load(&m, p, 0, 0), ==, 0x15222f3c);
    sgi_board_memory_cleanup(&m);

    /* Two 4096-byte rows from offset 100: 3996, a whole page, then 100. */
    g_array_set_size(h->chunks, 0);
    sgi_board_memory_init(&m, 2 * 4096);
    sgi_board_memory_add_plane(&m, &(SGIPlane) {
        .width = 1024, .height = 2, .stride = 4096, .len = 2 * 4096,
        .format = SGI_PLANE_RAW, .packing = SGI_PACK_BE_RED_LOW,
    }, &error_abort);
    d.w = 1024;
    d.h = 2;
    d.pitch = 1024 * 4;
    d.offset = 100;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmpuint(h->chunks->len, ==, 3);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 0).len, ==, PAGE - 100);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 1).len, ==, PAGE);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 2).len, ==, 100);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 1).addr, ==, 0x41000);
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/* The completion carries the family's cookie, the sequence and the bytes. */
static void test_completion(void)
{
    static const uint64_t pages[1] = { 0x50000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 1, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 4, .h = 2, .pitch = 16, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
        .seq = 12, .cookie = 0x1234abcd,
    };
    SGICompletion *c;

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmpuint(h->done->len, ==, 1);
    c = &g_array_index(h->done, SGICompletion, 0);
    g_assert_cmpuint(c->kind, ==, SGI_DONE_WRITTEN);
    g_assert_cmpuint(c->seq, ==, 12);
    g_assert_cmphex(c->cookie, ==, 0x1234abcd);
    g_assert_cmpuint(c->payload, ==, 32);
    /* Nothing traced when the family did not ask. */
    g_assert_cmpuint(h->chunks->len, ==, 0);

    /* Past the end of the page list: FAILED, same cookie, bounds. */
    d.offset = PAGE - 16;
    d.seq = 13;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);
    c = &g_array_index(h->done, SGICompletion, 1);
    g_assert_cmpuint(c->kind, ==, SGI_DONE_FAILED);
    g_assert_cmpuint(c->seq, ==, 13);
    g_assert_cmphex(c->cookie, ==, 0x1234abcd);
    g_assert_cmpuint(c->payload, ==, SGI_FAIL_BOUNDS);
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * A page that is not RAM is copied through a bounce buffer, traced the
 * same way; a page whose transactions fail stops the transfer.
 */
static void test_bounce_and_bus_error(void)
{
    static const uint64_t pages[2] = { HOLE, BAD };
    SGIDMAPageList pl = { .addr = pages, .npages = 2, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 2, .h = 1, .pitch = 8, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE, .trace_chunks = true,
        .cookie = 1,
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    ram_write(HOLE, (uint8_t []) { 1, 2, 3, 4, 5, 6, 7, 8 }, 8);
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmphex(sgi_plane_load(&m, p, 0, 0), ==, 0x01020304);
    g_assert_cmphex(sgi_plane_load(&m, p, 1, 0), ==, 0x05060708);
    g_assert_cmpuint(h->chunks->len, ==, 1);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 0).addr, ==, HOLE);

    d.offset = PAGE;
    d.cookie = 2;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BUS);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 1).payload, ==,
                     SGI_FAIL_BUS);
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * Plane to guest: mapped pages are written in place and released with the
 * bytes written; a bounced page is written back at the end.
 */
static void test_from_plane(void)
{
    static const uint64_t pages[2] = { 0x60000, HOLE };
    SGIDMAPageList pl = { .addr = pages, .npages = 2, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_FROM_PLANE, .plane = 0,
        .x = 1, .y = 3, .w = 4, .h = 1, .pitch = 16, .guest_bpp = 4,
        .tag_plane = SGI_PLANE_NONE, .trace_chunks = true,
        .offset = PAGE - 8,
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    for (int i = 0; i < 4; i++) {
        sgi_plane_store(&m, p, 1 + i, 3, 0xa0b0c0d0 + i);
    }
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmpuint(ram_byte(0x60000 + PAGE - 8), ==, 0xa0);
    g_assert_cmpuint(ram_byte(0x60000 + PAGE - 1), ==, 0xd1);
    g_assert_cmpuint(ram_byte(HOLE), ==, 0xa0);
    g_assert_cmpuint(ram_byte(HOLE + 7), ==, 0xd3);
    g_assert_cmpuint(unmapped_written, ==, 8);
    g_assert_cmpint(hole_reads, ==, 0);
    /* The head wrote guest memory; nothing it read, nothing traced. */
    g_assert_cmpuint(h->chunks->len, ==, 0);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==,
                     16);
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/* Logic op, write mask, clip and tag apply per pixel on the way in. */
static void test_raster_state(void)
{
    static const uint64_t pages[1] = { 0x70000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 1, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p, *tag;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 4, .h = 1, .pitch = 16, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_XOR, .mask = 0x0000ffff,
        .tag_plane = 1, .tag = 0x2c,
        .clip = { .n = 1, .r = { { 1, 0, 2, 0 } } },
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    tag = sgi_board_memory_plane(&m, 1);
    for (int i = 0; i < 4; i++) {
        uint8_t w[4];

        stl_be_p(w, 0xffffffff);
        ram_write(0x70000 + i * 4, w, 4);
        sgi_plane_store(&m, p, i, 0, 0x12345678);
    }
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmphex(sgi_plane_load(&m, p, 0, 0), ==, 0x12345678);
    g_assert_cmphex(sgi_plane_load(&m, p, 1, 0), ==, 0x1234a987);
    g_assert_cmphex(sgi_plane_load(&m, p, 2, 0), ==, 0x1234a987);
    g_assert_cmphex(sgi_plane_load(&m, p, 3, 0), ==, 0x12345678);
    g_assert_cmpuint(sgi_plane_load(&m, tag, 0, 0), ==, 0);
    g_assert_cmpuint(sgi_plane_load(&m, tag, 1, 0), ==, 0x2c);
    g_assert_cmpuint(sgi_plane_load(&m, tag, 2, 0), ==, 0x2c);
    g_assert_cmpuint(sgi_plane_load(&m, tag, 3, 0), ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * The family's convert_row() turns guest bytes into plane words both ways;
 * without it a guest pixel of another size than the plane's is refused.
 */
static void test_conversion(void)
{
    static const uint64_t pages[1] = { 0x80000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 1, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops_convert);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0, .conversion = 7,
        .x = 0, .y = 0, .w = 3, .h = 1, .pitch = 3, .guest_bpp = 1,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    ram_write(0x80000, (uint8_t []) { 0x11, 0x22, 0x33 }, 3);
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmphex(sgi_plane_load(&m, p, 2, 0), ==, 0xab000033);

    d.dir = SGI_DMA_FROM_PLANE;
    d.offset = 16;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmpuint(ram_byte(0x80000 + 16 + 1), ==, 0x22);

    bus.ops = &head_ops;
    d.dir = SGI_DMA_TO_PLANE;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_FORMAT);
    g_assert_cmpint(maps_outstanding, ==, 0);

    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * Every size in a transfer can come from a guest. Transfers that do not fit
 * their page list fail before anything is mapped, whatever the arithmetic
 * would wrap to, and a descriptor naming a million rows over one page
 * fails at once rather than mapping a million chunks.
 */
static void test_bounds(void)
{
    static const uint64_t pages[1] = { 0xa0000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 1, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    SGIPixelDMA base = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 1, .h = 1, .pitch = 4, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
    };
    SGIPixelDMA d;

    setup();
    plane_setup(&m, SGI_PLANE_RAW);

    /* Pitch 0: every row is the same four bytes, a million times. */
    d = base;
    d.pitch = 0;
    d.h = 1000000;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);
    g_assert_cmpint(maps_made, ==, 0);

    d = base;
    d.h = INT32_MAX;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);
    d = base;
    d.w = INT32_MAX;
    d.guest_bpp = 255;
    d.pitch = UINT32_MAX;
    d.h = 2;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    /* Rows of 2^34 bytes, 2^30 of them: the product wraps to zero. */
    d = base;
    d.w = 1 << 30;
    d.guest_bpp = 16;
    d.h = 1 << 30;
    d.pitch = 0;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    /* An offset that wraps the end past zero. */
    d = base;
    d.offset = UINT64_MAX - 2;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    /* Overlapping rows written to guest memory. */
    d = base;
    d.dir = SGI_DMA_FROM_PLANE;
    d.w = 4;
    d.h = 2;
    d.pitch = 8;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    /* A row wider than the plane fits the list but not the plane. */
    d = base;
    d.w = 65;
    d.pitch = 65 * 4;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    /* A bad page size and an unaligned page fail rather than abort. */
    d = base;
    pl.page_size = 3;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);
    pl.page_size = PAGE;
    pl.addr = (const uint64_t []) { 0xa0010 };
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, SGI_FAIL_BOUNDS);

    for (unsigned i = 0; i < h->done->len; i++) {
        SGICompletion *c = &g_array_index(h->done, SGICompletion, i);

        g_assert_cmpuint(c->kind, ==, SGI_DONE_FAILED);
        g_assert_cmpuint(c->payload, ==, SGI_FAIL_BOUNDS);
    }
    g_assert_cmpuint(h->done->len, ==, 9);
    g_assert_cmpint(maps_outstanding, ==, 0);
    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * The fake bus maps up to its own 4 KiB page, so with 1 KiB list pages a
 * chunk that ran on past its list page would read the physically next
 * kilobyte instead of the page the list names next.
 */
static void test_page_cut(void)
{
    static const uint64_t pages[2] = { 0xb0800, 0xb0000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 2, .page_size = 1024 };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    const SGIPlane *p;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 0, .y = 0, .w = 64, .h = 8, .pitch = 256, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE, .trace_chunks = true,
    };

    setup();
    plane_setup(&m, SGI_PLANE_RAW);
    p = sgi_board_memory_plane(&m, 0);
    for (uint64_t a = 0xb0000; a < 0xb1000; a += 4) {
        uint8_t w[4];

        stl_be_p(w, (uint32_t)a);
        ram_write(a, w, 4);
    }
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    /* Rows 0-3 are list page 0, rows 4-7 list page 1. */
    g_assert_cmphex(sgi_plane_load(&m, p, 0, 0), ==, 0xb0800);
    g_assert_cmphex(sgi_plane_load(&m, p, 63, 3), ==, 0xb0bfc);
    g_assert_cmphex(sgi_plane_load(&m, p, 0, 4), ==, 0xb0000);
    g_assert_cmphex(sgi_plane_load(&m, p, 63, 7), ==, 0xb03fc);
    g_assert_cmpuint(h->chunks->len, ==, 2);
    g_assert_cmpuint(g_array_index(h->chunks, Chunk, 0).len, ==, 1024);
    g_assert_cmphex(g_array_index(h->chunks, Chunk, 1).addr, ==, 0xb0000);
    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * What the head wrote reaches the guest even when the transfer stops part
 * way, and nothing else does: a written direct chunk is released with the
 * length up to its last written byte, so dirty logging sees it, and a
 * written bounce chunk writes back only the bytes the head wrote. A bounce
 * chunk for guest memory the head writes is never read first: behind it
 * may be a register, not RAM. Unwritten chunks are neither.
 */
static void test_partial_write(void)
{
    static const uint64_t pages[3] = { 0xc0000, HOLE, 0xc1000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 3, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIDMAMap map = { };
    uint8_t ones[8];

    setup();
    memset(ones, 0x11, sizeof(ones));
    ram_write(HOLE + 15, (uint8_t []) { 0x77 }, 1);
    ram_write(HOLE + 24, (uint8_t []) { 0x77 }, 1);
    g_assert_cmpint(sgi_dma_map(&bus, &map, &pl, 0, 3 * PAGE,
                                DMA_DIRECTION_FROM_DEVICE, false, 0), ==, 0);
    g_assert_cmpuint(map.nchunks, ==, 3);
    g_assert_true(map.chunks[1].bounce);
    g_assert_true(sgi_dma_map_copy(&map, 0, ones, 8, true));
    g_assert_true(sgi_dma_map_copy(&map, PAGE + 16, ones, 8, true));
    sgi_dma_unmap(&map);
    g_assert_cmpint(hole_reads, ==, 0);
    g_assert_cmpuint(unmapped_written, ==, 8);
    g_assert_cmpuint(ram_byte(0xc0000), ==, 0x11);
    g_assert_cmpuint(ram_byte(HOLE + 15), ==, 0x77);
    g_assert_cmpuint(ram_byte(HOLE + 16), ==, 0x11);
    g_assert_cmpuint(ram_byte(HOLE + 23), ==, 0x11);
    g_assert_cmpuint(ram_byte(HOLE + 24), ==, 0x77);
    g_assert_cmpint(maps_outstanding, ==, 0);
    head_free(h);
}

/*
 * A plane in page-backed memory, as a head whose pixels live in guest RAM
 * has: the transfer lands through the memory's own page list.
 */
static void test_page_backed_plane(void)
{
    static const uint64_t pages[1] = { 0xd0000 };
    SGIDMAPageList pl = { .addr = pages, .npages = 1, .page_size = PAGE };
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIBoardMemory m;
    uint8_t host[4][64];
    SGIPlane p = {
        .width = 16, .height = 4, .stride = 64, .len = 256,
        .format = SGI_PLANE_RAW, .packing = SGI_PACK_BE_RED_HIGH,
    };
    const SGIPlane *pp;
    SGIPixelDMA d = {
        .dir = SGI_DMA_TO_PLANE, .plane = 0,
        .x = 14, .y = 1, .w = 4, .h = 2, .pitch = 16, .guest_bpp = 4,
        .logic_op = SGI_LOGIC_COPY, .mask = ~0u,
        .tag_plane = SGI_PLANE_NONE,
    };

    setup();
    memset(host, 0, sizeof(host));
    sgi_board_memory_init_pages(&m, 4, 64);
    for (unsigned i = 0; i < 4; i++) {
        sgi_board_memory_set_page(&m, i, 0x100000 + i * 0x4000, host[3 - i]);
    }
    pp = sgi_board_memory_plane(&m,
                                sgi_board_memory_add_plane(&m, &p,
                                                           &error_abort));
    for (unsigned i = 0; i < 8; i++) {
        uint8_t w[4];

        stl_be_p(w, 0xfeed0000 + i);
        ram_write(0xd0000 + i * 4, w, 4);
    }
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    /* Row 1 is memory page 1, host[2]; columns 16 and up are clipped. */
    g_assert_cmphex((uint32_t)ldl_be_p(&host[2][14 * 4]), ==, 0xfeed0000);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[2][15 * 4]), ==, 0xfeed0001);
    g_assert_cmphex(sgi_plane_load(&m, pp, 15, 2), ==, 0xfeed0005);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[1][15 * 4]), ==, 0xfeed0005);
    g_assert_cmphex(sgi_plane_load(&m, pp, 0, 3), ==, 0);

    /* And back out to the guest. */
    d.dir = SGI_DMA_FROM_PLANE;
    d.offset = 64;
    d.pitch = 16;
    g_assert_cmpint(sgi_dma_pixels(&d, &bus, &pl, &m), ==, 0);
    g_assert_cmphex((uint32_t)ldl_be_p((uint8_t []) {
                        ram_byte(0xd0040), ram_byte(0xd0041),
                        ram_byte(0xd0042), ram_byte(0xd0043) }), ==,
                    0xfeed0000);
    g_assert_cmpint(maps_outstanding, ==, 0);
    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * Command fetch over the list pool measured on an SI board, as the list
 * probe measurements in Origami's libGLcore analysis notes record it: a
 * 256-triangle list called once, the stock client and kernel driving the
 * model. The page list sat at physical 0x4d74018 and held 4 KiB frame
 * numbers 0x224bc to 0x224bf first; the invocation named pool offset 0 and
 * 0x3fa words. The words placed below are the dump's at the positions the
 * test reads:
 * page 0's header and first list words, its last list words and the zeros
 * after them, and page 1's header and first list words. The rest of each
 * page is a numbered filler, so an out-of-order or misaligned fetch shows.
 */
static const uint64_t pool_frames[4] = { 0x224bc, 0x224bd, 0x224be, 0x224bf };

static void pool_setup(void)
{
    static const uint32_t page0_head[16] = {
        0x00004000, 0x00001000, 0x000003f9, 0x00001a00,
        0x2000020c, 0x3f800000, 0x3f800000, 0x00000000,
        0x40000008, 0xbf800000, 0xbf800000, 0x40000008,
        0xbf640000, 0xbf780000, 0x40000008, 0xbf740000,
    };
    static const uint32_t page0_tail[8] = {   /* words 1012 .. 1019 */
        0x40000008, 0x3f3c0000, 0xbef00000, 0x40000008,
        0x3f2c0000, 0xbec00000, 0x00000000, 0x00000000,
    };
    static const uint32_t page1_head[8] = {
        0x00000000, 0x00002000, 0x000003f9, 0x2000020c,
        0x3f800000, 0x3f800000, 0x00000000, 0x40000008,
    };
    uint8_t w[8];

    for (unsigned pg = 0; pg < 4; pg++) {
        for (unsigned i = 0; i < PAGE / 4; i++) {
            stl_be_p(w, 0x5a000000 | (pg << 16) | i);
            ram_write((pool_frames[pg] << 12) + i * 4, w, 4);
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        stl_be_p(w, page0_head[i]);
        ram_write((pool_frames[0] << 12) + i * 4, w, 4);
    }
    for (unsigned i = 0; i < 8; i++) {
        stl_be_p(w, page0_tail[i]);
        ram_write((pool_frames[0] << 12) + (1012 + i) * 4, w, 4);
        stl_be_p(w, page1_head[i]);
        ram_write((pool_frames[1] << 12) + i * 4, w, 4);
    }
    for (unsigned i = 0; i < 4; i++) {
        stq_be_p(w, pool_frames[i]);
        ram_write(0x4d74018 + i * 8, w, 8);
    }
}

static void head_decode(void *opaque, uint32_t context, const uint32_t *words,
                        unsigned n)
{
    Head *h = opaque;

    g_assert_cmpuint(context, ==, 3);
    h->decode_calls++;
    g_array_append_vals(h->decoded, words, n);
}

static bool head_next(void *opaque, const SGICommandFetch *f,
                      uint64_t *offset, uint64_t *nwords)
{
    Head *h = opaque;

    if (h->next_calls++ >= h->next_limit) {
        return false;
    }
    *offset = h->next_offset;
    *nwords = h->next_words;
    return true;
}

/* The family's side: read the page list, frames to device addresses. */
static void pool_page_list(Head *h, uint64_t addrs[4])
{
    SGIDMABus bus = bus_for(h, &head_ops);
    uint8_t raw[32];

    g_assert_true(sgi_dma_read(&bus, 0x4d74018, raw, sizeof(raw), true, 100));
    for (unsigned i = 0; i < 4; i++) {
        addrs[i] = ldq_be_p(raw + i * 8) << 12;
    }
}

static void test_fetch_measured_pool(void)
{
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    uint64_t addrs[4];
    SGIDMAPageList pl = { .addr = addrs, .npages = 4, .page_size = PAGE };
    SGICommandFetch f = {
        .context = 3, .decode = head_decode,
        .trace_chunks = true, .trace_seq = 100,
        .seq = 21, .cookie = 0xfe,
    };
    SGICompletion *c;
    Chunk *ch;

    setup();
    pool_setup();
    pool_page_list(h, addrs);
    g_assert_cmphex(addrs[0], ==, 0x224bc000);
    g_assert_cmphex(addrs[3], ==, 0x224bf000);

    g_assert_cmpint(sgi_fetch(&f, &bus, &pl, 0, 0x3fa), ==, 0);
    g_assert_cmpuint(h->decode_calls, ==, 1);
    g_assert_cmpuint(h->decoded->len, ==, 0x3fa);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 0), ==, 0x4000);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 1), ==, 0x1000);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 2), ==, 0x3f9);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 3), ==, 0x1a00);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 16), ==, 0x5a000010);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 1017), ==,
                    0xbec00000);

    /* The page list, then the pool words, each traced at its own address. */
    g_assert_cmpuint(h->chunks->len, ==, 2);
    ch = &g_array_index(h->chunks, Chunk, 0);
    g_assert_cmphex(ch->addr, ==, 0x4d74018);
    g_assert_cmpuint(ch->len, ==, 32);
    ch = &g_array_index(h->chunks, Chunk, 1);
    g_assert_cmphex(ch->addr, ==, 0x224bc000);
    g_assert_cmpuint(ch->len, ==, 0x3fa * 4);
    g_assert_cmpuint(ch->seq, ==, 100);

    g_assert_cmpuint(h->done->len, ==, 1);
    c = &g_array_index(h->done, SGICompletion, 0);
    g_assert_cmpuint(c->kind, ==, SGI_DONE_CONSUMED);
    g_assert_cmpuint(c->payload, ==, 0x3fa);
    g_assert_cmphex(c->cookie, ==, 0xfe);
    g_assert_cmpuint(c->seq, ==, 21);
    g_assert_null(f.segments);
    g_assert_cmpint(maps_outstanding, ==, 0);
    head_free(h);
}

/*
 * How the board reaches later pages is the family's open question: the
 * fetch asks next() and follows whatever range it names, and the one
 * completion counts every word the grammar consumed.
 */
static void test_fetch_next(void)
{
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    uint64_t addrs[4];
    SGIDMAPageList pl = { .addr = addrs, .npages = 4, .page_size = PAGE };
    SGICommandFetch f = {
        .context = 3, .decode = head_decode, .next = head_next,
        .cookie = 0xab,
    };

    setup();
    pool_setup();
    pool_page_list(h, addrs);
    h->next_limit = 1;
    h->next_offset = PAGE;
    h->next_words = 8;
    g_assert_cmpint(sgi_fetch(&f, &bus, &pl, 0, 0x3fa), ==, 0);
    g_assert_cmpuint(h->decode_calls, ==, 2);
    g_assert_cmpuint(h->decoded->len, ==, 0x3fa + 8);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 0x3fa + 1), ==,
                    0x2000);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 0x3fa + 3), ==,
                    0x2000020c);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==,
                     0x3fa + 8);

    /* A next() that never stops runs out of pool, not forever. */
    g_array_set_size(h->decoded, 0);
    g_array_set_size(h->done, 0);
    h->decode_calls = 0;
    h->next_calls = 0;
    h->next_limit = UINT_MAX;
    h->next_offset = 0;
    h->next_words = 1024;
    g_assert_cmpint(sgi_fetch(&f, &bus, &pl, 0, 0x3fa), ==, SGI_FAIL_BOUNDS);
    g_assert_cmpuint(h->decode_calls, ==, 0);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).kind, ==,
                     SGI_DONE_FAILED);

    /* A range of no words ends the fetch. */
    g_array_set_size(h->done, 0);
    h->next_calls = 0;
    h->next_words = 0;
    g_assert_cmpint(sgi_fetch(&f, &bus, &pl, 0, 4), ==, 0);
    g_assert_cmpuint(h->next_calls, ==, 1);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==, 4);
    g_assert_cmpint(maps_outstanding, ==, 0);
    head_free(h);
}

/* A list held in a plane of words, in the plane's byte order. */
static void test_fetch_plane(void)
{
    Head *h = head_new();
    SGIBoardMemory m;
    SGIPlane p = {
        .base = 64, .len = 256, .width = 64, .height = 1, .stride = 256,
        .format = SGI_PLANE_RAW, .packing = SGI_PACK_LE_RED_LOW,
    };
    SGICommandFetch f = { .context = 3, .decode = head_decode };
    uint32_t id;

    sgi_board_memory_init(&m, 512);
    id = sgi_board_memory_add_plane(&m, &p, &error_abort);
    stl_le_p(m.bytes + 64 + 8, 0x01020304);
    g_assert_cmpint(sgi_fetch_map_plane(&f, &m, id, 8, 2, h), ==, 0);
    sgi_fetch_run(&f, h);
    sgi_fetch_finish(&f, 0, &head_ops, h);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 0), ==, 0x01020304);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==, 2);

    g_assert_cmpint(sgi_fetch_map_plane(&f, &m, id, 252, 2, h), ==,
                    SGI_FAIL_BOUNDS);
    sgi_fetch_finish(&f, SGI_FAIL_BOUNDS, &head_ops, h);

    /*
     * A list invoked as several ranges of the plane is one fetch: next()
     * names the ranges after the first, and one completion counts them all.
     */
    g_array_set_size(h->decoded, 0);
    g_array_set_size(h->done, 0);
    stl_le_p(m.bytes + 64 + 100, 0xaabbccdd);
    f.next = head_next;
    h->decode_calls = 0;
    h->next_calls = 0;
    h->next_limit = 2;
    h->next_offset = 100;
    h->next_words = 1;
    g_assert_cmpint(sgi_fetch_map_plane(&f, &m, id, 8, 2, h), ==, 0);
    sgi_fetch_run(&f, h);
    sgi_fetch_finish(&f, 0, &head_ops, h);
    g_assert_cmpuint(h->decode_calls, ==, 3);
    g_assert_cmpuint(h->decoded->len, ==, 4);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 2), ==, 0xaabbccdd);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 3), ==, 0xaabbccdd);
    g_assert_cmpuint(h->done->len, ==, 1);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==, 4);

    /* The plane's words bound a next() that keeps asking. */
    h->next_calls = 0;
    h->next_limit = UINT_MAX;
    g_assert_cmpint(sgi_fetch_map_plane(&f, &m, id, 0, 1, h), ==,
                    SGI_FAIL_BOUNDS);
    sgi_fetch_finish(&f, SGI_FAIL_BOUNDS, &head_ops, h);
    sgi_board_memory_cleanup(&m);
    head_free(h);
}

/*
 * A range of no words names no page: at the pool's very end, or over an
 * empty page list, it reads nothing and completes with nothing consumed.
 */
static void test_fetch_empty(void)
{
    Head *h = head_new();
    uint64_t addrs[1] = { 0xe0000 };
    SGIDMABus bus = bus_for(h, &head_ops);
    SGIDMAPageList pl = { .addr = addrs, .npages = 1, .page_size = PAGE };
    SGIDMAPageList none = { .addr = NULL, .npages = 0, .page_size = PAGE };
    SGICommandFetch f = { .context = 3, .decode = head_decode };

    setup();
    g_assert_cmpint(sgi_fetch(&f, &bus, &pl, PAGE, 0), ==, 0);
    g_assert_cmpint(sgi_fetch(&f, &bus, &none, 0, 0), ==, 0);
    g_assert_cmpuint(h->decode_calls, ==, 0);
    g_assert_cmpuint(h->done->len, ==, 2);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 1).kind, ==,
                     SGI_DONE_CONSUMED);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 1).payload, ==, 0);
    g_assert_cmpint(sgi_fetch(&f, &bus, &none, 0, 1), ==, SGI_FAIL_BOUNDS);
    g_assert_cmpint(maps_made, ==, 0);
    head_free(h);
}

/*
 * A list invoked from inside a list gets a fetch of its own, and both
 * complete with their own counts. Mapping the outer fetch again from inside
 * its own decode() fails that inner invocation with BUSY and leaves the
 * outer one to finish its own words.
 */
static SGICommandFetch *nested_outer;
static const SGIDMABus *nested_bus;
static const SGIDMAPageList *nested_pl;
static bool nested_reuse;

static void nested_decode(void *opaque, uint32_t context,
                          const uint32_t *words, unsigned n)
{
    Head *h = opaque;
    SGICommandFetch inner = { .context = 3, .decode = head_decode,
                              .cookie = 0x2 };

    head_decode(opaque, context, words, n);
    if (h->decode_calls != 1) {
        return;
    }
    if (nested_reuse) {
        g_assert_cmpint(sgi_fetch(nested_outer, nested_bus, nested_pl, PAGE,
                                  5), ==, SGI_FAIL_BUSY);
        return;
    }
    g_assert_cmpint(sgi_fetch(&inner, nested_bus, nested_pl, PAGE, 5), ==, 0);
}

static Head *run_nested(bool reuse)
{
    Head *h = head_new();
    SGIDMABus bus = bus_for(h, &head_ops);
    uint64_t addrs[4];
    SGIDMAPageList pl = { .addr = addrs, .npages = 4, .page_size = PAGE };
    SGICommandFetch outer = { .context = 3, .decode = nested_decode,
                              .next = head_next, .cookie = 0x1 };

    setup();
    pool_setup();
    pool_page_list(h, addrs);
    /* Two segments, so a run cut short after the first shows. */
    h->next_offset = 2 * PAGE;
    h->next_words = 3;
    h->next_limit = 1;
    nested_outer = &outer;
    nested_bus = &bus;
    nested_pl = &pl;
    nested_reuse = reuse;
    g_assert_cmpint(sgi_fetch(&outer, &bus, &pl, 0, 4), ==, 0);
    g_assert_cmpuint(h->done->len, ==, 2);
    g_assert_cmphex(g_array_index(h->done, SGICompletion, 1).cookie, ==, 0x1);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 1).kind, ==,
                     SGI_DONE_CONSUMED);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 1).payload, ==, 7);

    /* Finished, the outer fetch takes a new list. */
    g_assert_cmpint(sgi_fetch(&outer, &bus, &pl, PAGE, 5), ==, 0);
    g_assert_cmpuint(h->done->len, ==, 3);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 2).payload, ==, 5);
    g_assert_null(outer.segments);
    return h;
}

static void test_fetch_nested(void)
{
    Head *h = run_nested(false);

    g_assert_cmpuint(h->decoded->len, ==, 17);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 4), ==, 0);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 5), ==, 0x2000);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 9), ==, 0x5a020000);
    g_assert_cmphex(g_array_index(h->done, SGICompletion, 0).cookie, ==, 0x2);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==, 5);
    head_free(h);
}

static void test_fetch_reused(void)
{
    Head *h = run_nested(true);

    /* The outer list's seven words, then the second list's five. */
    g_assert_cmpuint(h->decoded->len, ==, 12);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 4), ==, 0x5a020000);
    g_assert_cmphex(g_array_index(h->decoded, uint32_t, 7), ==, 0);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).kind, ==,
                     SGI_DONE_FAILED);
    g_assert_cmpuint(g_array_index(h->done, SGICompletion, 0).payload, ==,
                     SGI_FAIL_BUSY);
    head_free(h);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-dma/read-traced", test_read_traced);
    g_test_add_func("/sgi-dma/rows-across-pages", test_rows_across_pages);
    g_test_add_func("/sgi-dma/chunking", test_chunking);
    g_test_add_func("/sgi-dma/completion", test_completion);
    g_test_add_func("/sgi-dma/bounce-and-bus-error",
                    test_bounce_and_bus_error);
    g_test_add_func("/sgi-dma/from-plane", test_from_plane);
    g_test_add_func("/sgi-dma/raster-state", test_raster_state);
    g_test_add_func("/sgi-dma/conversion", test_conversion);
    g_test_add_func("/sgi-dma/bounds", test_bounds);
    g_test_add_func("/sgi-dma/page-cut", test_page_cut);
    g_test_add_func("/sgi-dma/partial-write", test_partial_write);
    g_test_add_func("/sgi-dma/page-backed-plane", test_page_backed_plane);
    g_test_add_func("/sgi-fetch/measured-pool", test_fetch_measured_pool);
    g_test_add_func("/sgi-fetch/next", test_fetch_next);
    g_test_add_func("/sgi-fetch/plane", test_fetch_plane);
    g_test_add_func("/sgi-fetch/empty", test_fetch_empty);
    g_test_add_func("/sgi-fetch/nested", test_fetch_nested);
    g_test_add_func("/sgi-fetch/reused", test_fetch_reused);
    return g_test_run();
}
