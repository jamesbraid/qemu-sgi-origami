/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics board memory and planes: plane bounds, packing, both
 * backings, logic ops and masks, clip lists, and migration.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "io/channel-buffer.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "hw/display/sgi/board-memory.h"

static SGIPlane plane(uint64_t base, uint32_t w, uint32_t h, uint32_t stride,
                      SGIPlaneFormat format, SGIPacking packing)
{
    return (SGIPlane) {
        .base = base,
        .len = (uint64_t)(h - 1) * stride +
               (uint64_t)w * sgi_plane_bytes_per_pixel(format),
        .width = w,
        .height = h,
        .stride = stride,
        .format = format,
        .packing = packing,
        .evidence = "unit test",
    };
}

static void test_plane_bounds(void)
{
    SGIBoardMemory m;
    SGIPlane p = plane(0, 16, 8, 64, SGI_PLANE_RAW, SGI_PACK_BE_RED_LOW);
    SGIPlane bad;
    const SGIPlane *first;
    Error *err = NULL;

    sgi_board_memory_init(&m, 1024);

    bad = p;
    bad.width = 0;
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &bad, &err), ==,
                     SGI_PLANE_NONE);
    g_assert(err);
    error_free(err);
    err = NULL;

    bad = p;
    bad.stride = 60;            /* 16 pixels of 4 bytes need 64 */
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &bad, &err), ==,
                     SGI_PLANE_NONE);
    error_free(err);
    err = NULL;

    bad = p;
    bad.len -= 1;               /* the last pixel's last byte is missing */
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &bad, &err), ==,
                     SGI_PLANE_NONE);
    error_free(err);
    err = NULL;

    bad = p;
    bad.base = 1024 - bad.len + 1;
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &bad, &err), ==,
                     SGI_PLANE_NONE);
    error_free(err);
    err = NULL;

    g_assert_cmpuint(m.nplanes, ==, 0);
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &p, &error_abort), ==, 0);
    /* A second plane over the same bytes: planes may overlap. */
    p.format = SGI_PLANE_TAG;
    p.stride = 64;
    p.len = 7 * 64 + 16;
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &p, &error_abort), ==, 1);
    g_assert_nonnull(sgi_board_memory_plane(&m, 1));
    g_assert_null(sgi_board_memory_plane(&m, 2));
    g_assert_cmpuint(sgi_board_memory_plane(&m, 1)->id, ==, 1);

    /* The table is fixed: earlier plane pointers survive later adds. */
    first = sgi_board_memory_plane(&m, 0);
    while (m.nplanes < SGI_PLANES_MAX) {
        sgi_board_memory_add_plane(&m, &p, &error_abort);
    }
    g_assert_true(first == sgi_board_memory_plane(&m, 0));
    g_assert_cmpuint(sgi_board_memory_add_plane(&m, &p, &err), ==,
                     SGI_PLANE_NONE);
    error_free(err);

    sgi_board_memory_cleanup(&m);
}

static void test_bytes_per_pixel(void)
{
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_CI8), ==, 1);
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_RGB12), ==, 2);
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_RGB24), ==, 4);
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_Z24), ==, 4);
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_TAG), ==, 1);
    g_assert_cmpuint(sgi_plane_bytes_per_pixel(SGI_PLANE_RAW), ==, 4);
}

static void test_packing(void)
{
    SGIBoardMemory m;
    SGIPlane be = plane(0, 4, 2, 16, SGI_PLANE_RAW, SGI_PACK_BE_RED_HIGH);
    SGIPlane le = plane(32, 4, 2, 16, SGI_PLANE_RAW, SGI_PACK_LE_RED_LOW);
    SGIPlane w16 = plane(64, 4, 2, 8, SGI_PLANE_RGB12, SGI_PACK_BE_RED_LOW);
    const SGIPlane *pbe, *ple, *p16;
    static const uint8_t be_bytes[] = { 0x11, 0x22, 0x33, 0x44 };
    static const uint8_t le_bytes[] = { 0x44, 0x33, 0x22, 0x11 };
    static const uint8_t w16_bytes[] = { 0x0a, 0xbc };

    sgi_board_memory_init(&m, 128);
    pbe = sgi_board_memory_plane(&m,
                                 sgi_board_memory_add_plane(&m, &be,
                                                            &error_abort));
    ple = sgi_board_memory_plane(&m,
                                 sgi_board_memory_add_plane(&m, &le,
                                                            &error_abort));
    p16 = sgi_board_memory_plane(&m,
                                 sgi_board_memory_add_plane(&m, &w16,
                                                            &error_abort));

    sgi_plane_store(&m, pbe, 1, 1, 0x11223344);
    g_assert_cmpmem(m.bytes + 16 + 4, 4, be_bytes, 4);
    g_assert_cmphex(sgi_plane_load(&m, pbe, 1, 1), ==, 0x11223344);

    sgi_plane_store(&m, ple, 1, 1, 0x11223344);
    g_assert_cmpmem(m.bytes + 32 + 16 + 4, 4, le_bytes, 4);
    g_assert_cmphex(sgi_plane_load(&m, ple, 1, 1), ==, 0x11223344);

    sgi_plane_store(&m, p16, 3, 0, 0xabc);
    g_assert_cmpmem(m.bytes + 64 + 6, 2, w16_bytes, 2);
    g_assert_cmphex(sgi_plane_load(&m, p16, 3, 0), ==, 0xabc);

    sgi_board_memory_cleanup(&m);
}

static void test_out_of_plane(void)
{
    SGIBoardMemory m;
    SGIPlane p = plane(16, 4, 4, 4, SGI_PLANE_CI8, SGI_PACK_BE_RED_LOW);
    const SGIPlane *pp;
    uint8_t zero[64] = { 0 };

    sgi_board_memory_init(&m, 64);
    pp = sgi_board_memory_plane(&m,
                                sgi_board_memory_add_plane(&m, &p,
                                                           &error_abort));
    /* Each of these would land on a neighbour's byte if it were stored. */
    sgi_plane_store(&m, pp, 4, 0, 0xff);
    sgi_plane_store(&m, pp, -1, 1, 0xff);
    sgi_plane_store(&m, pp, 0, 4, 0xff);
    sgi_plane_store(&m, pp, 0, -1, 0xff);
    g_assert_cmpmem(m.bytes, 64, zero, 64);

    sgi_plane_store(&m, pp, 3, 3, 0x5a);
    g_assert_cmpuint(m.bytes[16 + 15], ==, 0x5a);
    g_assert_cmpuint(sgi_plane_load(&m, pp, 3, 3), ==, 0x5a);
    g_assert_cmpuint(sgi_plane_load(&m, pp, 4, 3), ==, 0);
    g_assert_cmpuint(sgi_plane_load(&m, pp, 3, 4), ==, 0);

    g_assert_false(sgi_board_memory_export(&m, 60, 5, zero));
    g_assert_false(sgi_board_memory_import(&m, UINT64_MAX, 2, zero));
    g_assert_true(sgi_board_memory_export(&m, 60, 4, zero));

    sgi_board_memory_cleanup(&m);
}

/*
 * A page-backed memory: four 16-byte pages resolved to scattered host
 * buffers, in an order unrelated to their guest addresses. A 20-byte row
 * stride puts rows, and some words, across page boundaries.
 */
static void test_page_backed(void)
{
    SGIBoardMemory m;
    uint8_t host[4][16];
    SGIPlane p = plane(0, 5, 3, 20, SGI_PLANE_RAW, SGI_PACK_BE_RED_LOW);
    const SGIPlane *pp;
    static const unsigned order[4] = { 2, 0, 3, 1 };
    uint8_t out[8];

    memset(host, 0, sizeof(host));
    sgi_board_memory_init_pages(&m, 4, 16);
    g_assert_cmpuint(m.size, ==, 64);
    for (unsigned i = 0; i < 4; i++) {
        sgi_board_memory_set_page(&m, i, 0x10000 * (i + 1), host[order[i]]);
    }
    pp = sgi_board_memory_plane(&m,
                                sgi_board_memory_add_plane(&m, &p,
                                                           &error_abort));

    /* Row 0, pixel 3: bytes 12..15, the end of page 0. */
    sgi_plane_store(&m, pp, 3, 0, 0xa1a2a3a4);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[2][12]), ==, 0xa1a2a3a4);
    /* Row 1, pixel 0: bytes 20..23, inside page 1. */
    sgi_plane_store(&m, pp, 0, 1, 0xb1b2b3b4);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[0][4]), ==, 0xb1b2b3b4);
    /* Row 2, pixel 3: bytes 52..55, page 3. */
    sgi_plane_store(&m, pp, 3, 2, 0xc1c2c3c4);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[1][4]), ==, 0xc1c2c3c4);
    g_assert_cmphex(sgi_plane_load(&m, pp, 3, 0), ==, 0xa1a2a3a4);
    g_assert_cmphex(sgi_plane_load(&m, pp, 0, 1), ==, 0xb1b2b3b4);

    /* A range across pages 0 and 1 comes back whole. */
    g_assert_true(sgi_board_memory_export(&m, 12, 8, out));
    g_assert_cmphex((uint32_t)ldl_be_p(out), ==, 0xa1a2a3a4);
    g_assert_cmphex((uint32_t)ldl_be_p(out + 4), ==, 0);

    /* A word that straddles pages goes to both. */
    g_assert_true(sgi_board_memory_import(&m, 14, 4,
                                          (uint8_t []) { 1, 2, 3, 4 }));
    g_assert_cmpuint(host[2][14], ==, 1);
    g_assert_cmpuint(host[2][15], ==, 2);
    g_assert_cmpuint(host[0][0], ==, 3);
    g_assert_cmpuint(host[0][1], ==, 4);

    /* An unresolved page: nothing crosses it, in either direction. */
    sgi_board_memory_set_page(&m, 1, 0x20000, NULL);
    g_assert_false(sgi_board_memory_export(&m, 12, 8, out));
    g_assert_false(sgi_board_memory_import(&m, 12, 8, out));
    g_assert_cmpuint(host[2][12], ==, 0xa1);
    g_assert_cmphex(sgi_plane_load(&m, pp, 0, 1), ==, 0);
    sgi_plane_store(&m, pp, 0, 1, 0xdeadbeef);
    g_assert_cmphex((uint32_t)ldl_be_p(&host[0][4]), ==, 0xb1b2b3b4);

    sgi_board_memory_cleanup(&m);
}

static void test_logic_ops(void)
{
    /* OpenGL's table for s = 1100 and d = 1010, in GL_CLEAR + op order. */
    static const uint32_t want[16] = {
        0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
        0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf,
    };

    for (unsigned op = 0; op < 16; op++) {
        g_assert_cmphex(sgi_logic_op(op, 0xc, 0xa) & 0xf, ==, want[op]);
    }
}

static void test_merge_mask(void)
{
    SGIBoardMemory m;
    SGIPlane p = plane(0, 2, 1, 8, SGI_PLANE_RAW, SGI_PACK_LE_RED_LOW);
    const SGIPlane *pp;

    sgi_board_memory_init(&m, 8);
    pp = sgi_board_memory_plane(&m,
                                sgi_board_memory_add_plane(&m, &p,
                                                           &error_abort));
    sgi_plane_store(&m, pp, 0, 0, 0x12345678);
    sgi_plane_merge(&m, pp, 0, 0, 0xffffffff, SGI_LOGIC_XOR, 0x00ff00ff);
    g_assert_cmphex(sgi_plane_load(&m, pp, 0, 0), ==, 0x12cb5687);
    sgi_plane_merge(&m, pp, 1, 0, 0xcafef00d, SGI_LOGIC_COPY, 0xffff0000);
    g_assert_cmphex(sgi_plane_load(&m, pp, 1, 0), ==, 0xcafe0000);
    sgi_board_memory_cleanup(&m);
}

static void test_clip(void)
{
    SGIClipList none = { 0 };
    SGIClipList two = {
        .n = 2,
        .r = { { 0, 0, 3, 3 }, { 10, 10, 10, 12 } },
    };
    SGIClipList empty = { .empty = true };
    SGIClipList one = {
        .n = 1,
        .empty = true,
        .r = { { 0, 0, 3, 3 } },
    };

    g_assert_true(sgi_clip_contains(&none, -100, 100));
    /* An empty list keeps nothing; with a rectangle the flag is ignored. */
    g_assert_false(sgi_clip_contains(&empty, 0, 0));
    g_assert_false(sgi_clip_contains(&empty, -100, 100));
    g_assert_true(sgi_clip_contains(&one, 3, 3));
    g_assert_false(sgi_clip_contains(&one, 4, 3));
    g_assert_true(sgi_clip_contains(&two, 3, 3));
    g_assert_false(sgi_clip_contains(&two, 4, 3));
    g_assert_true(sgi_clip_contains(&two, 10, 12));
    g_assert_false(sgi_clip_contains(&two, 10, 13));
    g_assert_false(sgi_clip_contains(&two, 9, 10));
}

typedef struct TestHead {
    uint32_t regs[2];
    SGIBoardMemory mem;
} TestHead;

DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_test_head_mem, "test-head/mem",
                                TestHead, mem);

/* The family's own description, with the shared state as a subsection. */
static const VMStateDescription vmstate_test_head = {
    .name = "test-head",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, TestHead, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_test_head_mem,
        NULL
    },
};

/* The saved stream, copied out before closing the file frees it. */
static GByteArray *save_head(TestHead *h)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(4096);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    GByteArray *stream;

    g_assert_cmpint(vmstate_save_state(f, &vmstate_test_head, h, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(f), ==, 0);
    stream = g_byte_array_new();
    g_byte_array_append(stream, bioc->data, bioc->usage);
    /*
     * What follows a device in a real stream: loading peeks one byte past
     * the device for another subsection, and a stream that ends there is
     * an error.
     */
    g_byte_array_append(stream, (const guint8 *)"", 1);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    return stream;
}

static int load_head(GByteArray *stream, TestHead *h)
{
    QIOChannelBuffer *bioc = qio_channel_buffer_new(stream->len);
    QEMUFile *f;
    Error *err = NULL;
    int ret;

    memcpy(bioc->data, stream->data, stream->len);
    bioc->usage = stream->len;
    f = qemu_file_new_input(QIO_CHANNEL(bioc));
    ret = vmstate_load_state(f, &vmstate_test_head, h, 1, &err);
    if (err) {
        g_test_message("load: %s", error_get_pretty(err));
    }
    error_free(err);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    return ret;
}

static void test_migration_device_backed(void)
{
    TestHead src = { .regs = { 1, 2 } }, dst = { 0 }, other = { 0 };
    SGIPlane p = plane(0, 4, 4, 4, SGI_PLANE_CI8, SGI_PACK_BE_RED_LOW);
    GByteArray *stream;

    sgi_board_memory_init(&src.mem, 256);
    sgi_board_memory_init(&dst.mem, 256);
    sgi_board_memory_init(&other.mem, 128);
    sgi_board_memory_add_plane(&src.mem, &p, &error_abort);
    sgi_board_memory_add_plane(&dst.mem, &p, &error_abort);
    sgi_board_memory_add_plane(&other.mem, &p, &error_abort);
    for (unsigned i = 0; i < 256; i++) {
        src.mem.bytes[i] = i * 7;
    }

    stream = save_head(&src);
    g_assert_cmpint(load_head(stream, &dst), ==, 0);
    g_assert_cmpmem(dst.mem.bytes, 256, src.mem.bytes, 256);
    g_assert_cmpuint(dst.regs[1], ==, 2);
    /* A board of another size refuses the stream. */
    g_assert_cmpint(load_head(stream, &other), !=, 0);

    g_byte_array_unref(stream);
    sgi_board_memory_cleanup(&src.mem);
    sgi_board_memory_cleanup(&dst.mem);
    sgi_board_memory_cleanup(&other.mem);
}

static void test_migration_page_backed(void)
{
    TestHead src = { 0 }, dst = { 0 };
    uint8_t host[2][16];
    GByteArray *stream;

    sgi_board_memory_init_pages(&src.mem, 2, 16);
    sgi_board_memory_init_pages(&dst.mem, 2, 16);
    sgi_board_memory_set_page(&src.mem, 0, 0x4000, host[0]);
    sgi_board_memory_set_page(&src.mem, 1, 0x8000, host[1]);
    sgi_board_memory_set_page(&dst.mem, 0, 0x1111, host[1]);

    stream = save_head(&src);
    g_assert_cmpint(load_head(stream, &dst), ==, 0);
    /* Guest addresses travel; host pointers are the family's to redo. */
    g_assert_cmphex(dst.mem.pages[0].guest, ==, 0x4000);
    g_assert_cmphex(dst.mem.pages[1].guest, ==, 0x8000);
    g_assert_null(dst.mem.pages[0].host);
    g_assert_null(dst.mem.pages[1].host);

    g_byte_array_unref(stream);
    sgi_board_memory_cleanup(&src.mem);
    sgi_board_memory_cleanup(&dst.mem);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi-memory/plane-bounds", test_plane_bounds);
    g_test_add_func("/sgi-memory/bytes-per-pixel", test_bytes_per_pixel);
    g_test_add_func("/sgi-memory/packing", test_packing);
    g_test_add_func("/sgi-memory/out-of-plane", test_out_of_plane);
    g_test_add_func("/sgi-memory/page-backed", test_page_backed);
    g_test_add_func("/sgi-memory/logic-ops", test_logic_ops);
    g_test_add_func("/sgi-memory/merge-mask", test_merge_mask);
    g_test_add_func("/sgi-memory/clip", test_clip);
    g_test_add_func("/sgi-memory/migration-device-backed",
                    test_migration_device_backed);
    g_test_add_func("/sgi-memory/migration-page-backed",
                    test_migration_page_backed);
    return g_test_run();
}
