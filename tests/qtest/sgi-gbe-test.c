/* SPDX-License-Identifier: BSD-3-Clause */
/* GBE 1.1 register contract and opt-in synthetic one-tile scanout. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define GBE_PATH "/machine/peripheral/gbe"
#define GBE_BASE UINT64_C(0x16000000)
#define GBE_REG(offset) (GBE_BASE + (offset))

#define CTRLSTAT     0x000000
#define DEVICE_ID    0x000014
#define VT_0         0x010000
#define VT_1         0x010004
#define VT_4         0x010010
#define VT_5         0x010014
#define OVR_0        0x020000
#define OVR_1        0x020004
#define OVR_2        0x020008
#define FRM_0        0x030000
#define FRM_1        0x030004
#define FRM_2        0x030008
#define FRM_3        0x03000c
#define DID          0x040000
#define DID_SHADOW   0x040004
#define WID(n)       (0x048000 + (n) * 4)
#define CMAP(n)      (0x050000 + (n) * 4)
#define GMAP(n)      (0x060000 + (n) * 4)
#define CRS_0        0x070000
#define CRS_1        0x070004
#define CRS_CM(n)    (0x070004 + (n) * 4)

#define SYNTHETIC_LIST 0x40010000u
#define SYNTHETIC_TILE 0x40020000u
#define SYNTHETIC_FRAME_NS 16667000

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
                                  "-device sgi-o2-gbe,id=gbe %s", extra);

    qtest_sysbus_mmio_map(qts, GBE_PATH, 0, GBE_BASE);
    return qts;
}

static uint32_t rd(QTestState *qts, unsigned offset)
{
    return qtest_readl(qts, GBE_REG(offset));
}

static void wr(QTestState *qts, unsigned offset, uint32_t value)
{
    qtest_writel(qts, GBE_REG(offset), value);
}

static void test_identity_and_reset(void)
{
    static const struct {
        unsigned offset;
        uint32_t mask;
    } zero_reset[] = {
        { VT_1, 0x00ffffff }, { VT_4, 0x00ffffff },
        { VT_5, 0x00ffffff }, { OVR_0, 0x00003fff },
        { OVR_1, 0xffffffe1 }, { OVR_2, 0xffffffe1 },
        { FRM_0, 0x0000ffff }, { FRM_1, 0xffff0000 },
        { FRM_2, 0xffffffe1 }, { FRM_3, 0xffffffe1 },
        { DID, 0x0001ffff }, { DID_SHADOW, 0x0001ffff },
        { CRS_0, 0xffffffff }, { CRS_1, 0x00000003 },
    };
    static const struct {
        unsigned offset;
        uint32_t value;
        uint32_t mask;
    } writable[] = {
        { VT_1, 0x0056789a, 0x00ffffff },
        { VT_4, 0x00123456, 0x00ffffff },
        { VT_5, 0x00654321, 0x00ffffff },
        { OVR_0, 0x00001fff, 0x00001fff },
        { FRM_0, 0x00003fff, 0x00007fff },
        { FRM_1, 0x12340000, 0xffff0000 },
        { CRS_0, 0x12345678, 0xffffffff },
    };
    QTestState *qts = start("");

    g_assert_cmphex(rd(qts, CTRLSTAT) & 0x03ffffcf, ==, 0x03ffffc1);
    g_assert_cmphex(rd(qts, DEVICE_ID), ==, 0x666);
    g_assert_cmphex(rd(qts, VT_0) & 0x80ffffff, ==, 0x80000000);
    for (size_t i = 0; i < G_N_ELEMENTS(zero_reset); i++) {
        g_assert_cmphex(rd(qts, zero_reset[i].offset) & zero_reset[i].mask,
                        ==, 0);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(writable); i++) {
        wr(qts, writable[i].offset, writable[i].value);
        g_assert_cmphex(rd(qts, writable[i].offset) & writable[i].mask,
                        ==, writable[i].value & writable[i].mask);
    }
    wr(qts, VT_0, 0x80ffffff);
    g_assert_cmphex(rd(qts, VT_0) & 0x80ffffff, ==, 0x80ffffff);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(rd(qts, CTRLSTAT) & 0x03ffffcf, ==, 0x03ffffc1);
    g_assert_cmphex(rd(qts, DEVICE_ID), ==, 0x666);
    g_assert_cmphex(rd(qts, VT_0) & 0x80ffffff, ==, 0x80000000);
    for (size_t i = 0; i < G_N_ELEMENTS(zero_reset); i++) {
        g_assert_cmphex(rd(qts, zero_reset[i].offset) & zero_reset[i].mask,
                        ==, 0);
    }
    qtest_quit(qts);
}

static void test_tables_and_geometry(void)
{
    QTestState *qts = start("");

    /* The published tables specify no reset image for these entries. */
    wr(qts, WID(0), UINT32_MAX);
    wr(qts, WID(31), 0x1234);
    g_assert_cmphex(rd(qts, WID(0)) & 0x1fff, ==, 0x1fff);
    g_assert_cmphex(rd(qts, WID(31)) & 0x1fff, ==, 0x1234);
    wr(qts, CMAP(0), UINT32_MAX);
    wr(qts, CMAP(4607), 0x12345678);
    wr(qts, GMAP(255), 0x87654321);
    wr(qts, CRS_CM(1), 0x12345678);
    wr(qts, CRS_CM(3), 0x87654321);
    g_assert_cmphex(rd(qts, CMAP(0)) & 0xffffff00, ==, 0xffffff00);
    g_assert_cmphex(rd(qts, CMAP(4607)) & 0xffffff00, ==, 0x12345600);
    g_assert_cmphex(rd(qts, GMAP(255)) & 0xffffff00, ==, 0x87654300);
    g_assert_cmphex(rd(qts, CRS_CM(1)) & 0xffffff00, ==, 0x12345600);
    g_assert_cmphex(rd(qts, CRS_CM(3)) & 0xffffff00, ==, 0x87654300);

    wr(qts, OVR_0, 0x1234);
    wr(qts, FRM_0, 0x3234);
    wr(qts, FRM_1, 0x12340000);
    wr(qts, CRS_0, 0x12345678);
    g_assert_cmphex(rd(qts, OVR_0) & 0x1fff, ==, 0x1234);
    g_assert_cmphex(rd(qts, FRM_0) & 0x7fff, ==, 0x3234);
    g_assert_cmphex(rd(qts, FRM_1) & 0xffff0000, ==, 0x12340000);
    g_assert_cmphex(rd(qts, CRS_0), ==, 0x12345678);
    qtest_quit(qts);
}

static void test_unsupported_accesses(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-invalid-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "guest-errors.txt", NULL);
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts = start(extra);

    wr(qts, OVR_0, 0x1234);
    qtest_writeb(qts, GBE_REG(OVR_0), 0xff);
    g_assert_cmphex(rd(qts, OVR_0) & 0x3fff, ==, 0x1234);
    qtest_writew(qts, GBE_REG(OVR_0), 0xffff);
    g_assert_cmphex(rd(qts, OVR_0) & 0x3fff, ==, 0x1234);
    qtest_writel(qts, GBE_REG(OVR_0 + 1), UINT32_MAX);
    g_assert_cmphex(rd(qts, OVR_0) & 0x3fff, ==, 0x1234);
    (void)qtest_readb(qts, GBE_REG(OVR_0));
    (void)rd(qts, 0x010018); /* Timing row unsupported in this subset. */
    wr(qts, 0x078000, UINT32_MAX); /* Glyph start is disputed. */
    g_assert_cmphex(rd(qts, OVR_0) & 0x3fff, ==, 0x1234);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "sgi-o2-gbe: unsupported access"));
    g_assert_nonnull(strstr(contents, "(1 bytes)"));
    g_assert_nonnull(strstr(contents, "(2 bytes)"));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_unsupported_64bit_access(void)
{
    QTestState *qts = qtest_initf(MEMTX_TEST_DEVICE "-M none -cpu R10000 -S "
                                  "-device sgi-o2-gbe,id=gbe");
    uint64_t wide;

    qtest_sysbus_mmio_map(qts, GBE_PATH, 0, GBE_BASE);
    wr(qts, VT_1, 0x0056789a);
    g_assert_cmpint(memtx_readq(qts, GBE_REG(CTRLSTAT), &wide),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(qts, GBE_REG(DEVICE_ID), &wide),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, GBE_REG(VT_1),
                                 UINT64_C(0x123456789abcdef0)), !=, MEMTX_OK);
    g_assert_cmphex(rd(qts, VT_1) & 0x00ffffff, ==, 0x0056789a);
    g_assert_cmphex(rd(qts, CTRLSTAT) & 0xf, ==, 1);
    g_assert_cmphex(rd(qts, DEVICE_ID), ==, 0x666);
    qtest_quit(qts);
}

static void test_unsupported_controls(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-controls-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "unimplemented.txt", NULL);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts = start(extra);

    wr(qts, VT_0, 0x80012345);
    wr(qts, VT_0, 0);
    g_assert_cmphex(rd(qts, VT_0) & 0x80ffffff, ==, 0x80012345);
    wr(qts, OVR_1, 0x12345000);
    wr(qts, OVR_2, 0x12345000);
    wr(qts, FRM_2, 0x12345000);
    wr(qts, FRM_3, 0x12345000);
    wr(qts, DID, 0x1234);
    wr(qts, DID_SHADOW, 0x1234);
    wr(qts, OVR_1, 1);
    wr(qts, FRM_2, 1);
    wr(qts, DID, 0x10000);
    wr(qts, CRS_1, 1);
    wr(qts, CRS_1, 2);
    g_assert_cmphex(rd(qts, OVR_1) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, OVR_2) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, FRM_2) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, FRM_3) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, DID) & 0x1ffff, ==, 0);
    g_assert_cmphex(rd(qts, DID_SHADOW) & 0x1ffff, ==, 0);
    g_assert_cmphex(rd(qts, CRS_1) & 3, ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "VT_0 clock is unsupported"));
    g_assert_nonnull(strstr(contents, "OVR_1 scanout control is unsupported"));
    g_assert_nonnull(strstr(contents, "OVR_2 shadow copy is unsupported"));
    g_assert_nonnull(strstr(contents, "FRM_2 scanout control is unsupported"));
    g_assert_nonnull(strstr(contents, "FRM_3 shadow copy is unsupported"));
    g_assert_nonnull(strstr(contents, "DID control is unsupported"));
    g_assert_nonnull(strstr(contents, "DID_SHADOW copy is unsupported"));
    g_assert_nonnull(strstr(contents,
                            "cursor scanout and crosshair are unsupported"));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    wr(from, VT_1, 0x0056789a);
    wr(from, FRM_0, 0x00004567);
    wr(from, OVR_0, 0x1234);
    wr(from, FRM_1, 0x12340000);
    wr(from, WID(31), 0x1234);
    wr(from, CMAP(4607), 0x12345600);
    wr(from, GMAP(255), 0x87654300);
    wr(from, CRS_CM(3), 0xaabbcc00);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(rd(to, VT_1) & 0xffffff, ==, 0x56789a);
    g_assert_cmphex(rd(to, FRM_0) & 0xffff, ==, 0x4567);
    g_assert_cmphex(rd(to, OVR_0) & 0x1fff, ==, 0x1234);
    g_assert_cmphex(rd(to, FRM_1) & 0xffff0000, ==, 0x12340000);
    g_assert_cmphex(rd(to, WID(31)) & 0x1fff, ==, 0x1234);
    g_assert_cmphex(rd(to, CMAP(4607)) & 0xffffff00, ==, 0x12345600);
    g_assert_cmphex(rd(to, GMAP(255)) & 0xffffff00, ==, 0x87654300);
    g_assert_cmphex(rd(to, CRS_CM(3)) & 0xffffff00, ==, 0xaabbcc00);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static QTestState *start_single_tile(const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -m 64M -display none "
        "-device sgi-o2-gbe,id=gbe,synthetic-single-tile=on,"
        "ram=/objects/ram/ram[0] %s", extra);

    qtest_sysbus_mmio_map(qts, GBE_PATH, 0, GBE_BASE);
    return qts;
}

static void fill_ram(QTestState *qts, uint32_t offset, uint32_t pixel)
{
    g_autofree uint8_t *tile = g_malloc(64 * KiB);

    for (size_t i = 0; i < 64 * KiB; i += 4) {
        stl_be_p(tile + i, pixel);
    }
    qtest_memwrite(qts, offset, tile, 64 * KiB);
}

static uint32_t single_tile_pixel(QTestState *qts, const char *dir,
                                  const char *name, unsigned x, unsigned y)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);
    g_autofree char *contents = NULL;
    gsize size = 0;
    static const char header[] = "P6\n128 128\n255\n";
    const uint8_t *pixel;

    qtest_qmp_assert_success(qts,
        "{'execute':'screendump','arguments':{'filename':%s}}", path);
    g_assert_true(g_file_get_contents(path, &contents, &size, NULL));
    g_assert_cmpuint(size, ==, strlen(header) + 128 * 128 * 3);
    g_assert_cmpmem(contents, strlen(header), header, strlen(header));
    pixel = (const uint8_t *)contents + strlen(header) + (y * 128 + x) * 3;
    g_assert_cmpint(g_unlink(path), ==, 0);
    return (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
}

static uint64_t single_tile_frames(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" GBE_PATH "',"
        "'property':'x-test-frame-count'}}");

    return qdict_get_int(reply, "return");
}

static void prepare_single_tile(QTestState *qts, uint16_t entry)
{
    uint8_t desc[2];

    fill_ram(qts, SYNTHETIC_LIST - 0x40000000u, 0x000000ff);
    fill_ram(qts, SYNTHETIC_TILE - 0x40000000u, 0x00ff0000);
    stw_be_p(desc, entry);
    qtest_memwrite(qts, SYNTHETIC_LIST - 0x40000000u, desc, sizeof(desc));
    wr(qts, FRM_0, 0x4010); /* 128 RGB8 pixels: 512 bytes per row. */
    wr(qts, FRM_1, 128u << 16);
    wr(qts, FRM_2, SYNTHETIC_LIST | 1);
}

static void test_single_tile_frozen_then_present(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-tile-XXXXXX", NULL);
    QTestState *qts = start_single_tile("");

    prepare_single_tile(qts, SYNTHETIC_TILE >> 16);
    g_assert_cmphex(rd(qts, FRM_0) & 0x7fff, ==, 0x4010);
    g_assert_cmphex(rd(qts, FRM_1) & 0xffff0000, ==, 128u << 16);
    g_assert_cmphex(rd(qts, FRM_2) & 0xffffffe1, ==,
                    SYNTHETIC_LIST | 1);
    g_assert_cmphex(rd(qts, WID(0)) & 0x1fff, ==, 5u << 2);
    qtest_clock_step(qts, 4 * SYNTHETIC_FRAME_NS);
    g_assert_cmpuint(single_tile_frames(qts), ==, 0);
    g_assert_cmphex(rd(qts, VT_0) & 0x80000000u, ==, 0x80000000u);
    g_assert_cmphex(single_tile_pixel(qts, dir, "frozen.ppm", 10, 10),
                    !=, 0x00ff0000);
    wr(qts, VT_0, 0);
    g_assert_cmphex(rd(qts, VT_0) & 0x80000000u, ==, 0);
    qtest_clock_step(qts, SYNTHETIC_FRAME_NS - 1);
    g_assert_cmpuint(single_tile_frames(qts), ==, 0);
    g_assert_cmphex(single_tile_pixel(qts, dir, "before.ppm", 10, 10),
                    !=, 0x00ff0000);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(single_tile_frames(qts), ==, 1);
    g_assert_cmphex(single_tile_pixel(qts, dir, "after.ppm", 10, 10),
                    ==, 0x00ff0000);
    qtest_quit(qts);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_single_tile_out_of_fit(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-fit-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "guest-errors.txt", NULL);
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts = start_single_tile(extra);

    prepare_single_tile(qts, 0x4402);
    wr(qts, VT_0, 0);
    qtest_clock_step(qts, SYNTHETIC_FRAME_NS);
    g_assert_cmpuint(single_tile_frames(qts), ==, 1);
    g_assert_cmphex(single_tile_pixel(qts, dir, "out-of-fit.ppm", 10, 10),
                    ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents,
                            "tile address 0x44020000 outside the linked "
                            "RAM fit"));
    g_assert_cmpint(g_unlink(log), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_single_tile_zero_entry(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-zero-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "guest-errors.txt", NULL);
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts = start_single_tile(extra);

    prepare_single_tile(qts, 0);
    wr(qts, VT_0, 0);
    qtest_clock_step(qts, SYNTHETIC_FRAME_NS);
    g_assert_cmphex(single_tile_pixel(qts, dir, "zero.ppm", 10, 10),
                    !=, 0x00ff0000);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents,
                            "tile address 0x00000000 outside the linked "
                            "RAM fit or zero"));
    g_assert_cmpint(g_unlink(log), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_single_tile_rejects_other_controls(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-control-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "unimplemented.txt", NULL);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts = start_single_tile(extra);

    wr(qts, FRM_2, 0);
    wr(qts, FRM_3, SYNTHETIC_LIST | 1);
    wr(qts, OVR_1, SYNTHETIC_LIST | 1);
    wr(qts, DID, 0x10000);
    wr(qts, CRS_1, 1);
    g_assert_cmphex(rd(qts, FRM_2) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, FRM_3) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, OVR_1) & 0xffffffe1, ==, 0);
    g_assert_cmphex(rd(qts, DID) & 0x1ffff, ==, 0);
    g_assert_cmphex(rd(qts, CRS_1) & 3, ==, 0);
    wr(qts, FRM_0, 0x4030);
    wr(qts, FRM_1, 128u << 16);
    wr(qts, FRM_2, SYNTHETIC_LIST | 1);
    wr(qts, VT_0, 0);
    g_assert_cmphex(rd(qts, VT_0) & 0x80000000u, ==, 0x80000000u);
    qtest_clock_step(qts, SYNTHETIC_FRAME_NS);
    g_assert_cmpuint(single_tile_frames(qts), ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "FRM_2 scanout control is unsupported"));
    g_assert_nonnull(strstr(contents, "FRM_3 shadow copy is unsupported"));
    g_assert_nonnull(strstr(contents, "OVR_1 scanout control is unsupported"));
    g_assert_nonnull(strstr(contents, "DID control is unsupported"));
    g_assert_nonnull(strstr(contents, "cursor scanout and crosshair"));
    g_assert_cmpint(g_unlink(log), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_single_tile_reset_and_migration_block(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-gbe-reset-XXXXXX", NULL);
    g_autoptr(QDict) migrate = NULL;
    QTestState *qts = start_single_tile("");

    prepare_single_tile(qts, SYNTHETIC_TILE >> 16);
    wr(qts, VT_0, 0);
    qtest_clock_step(qts, SYNTHETIC_FRAME_NS);
    g_assert_cmpuint(single_tile_frames(qts), ==, 1);
    g_assert_cmphex(single_tile_pixel(qts, dir, "before-reset.ppm", 10, 10),
                    ==, 0x00ff0000);
    migrate = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(migrate, "error"),
                                         "desc"),
                            "synthetic single-tile scanout does not support "
                            "migration"));
    qtest_system_reset(qts);
    g_assert_cmphex(rd(qts, VT_0) & 0x80000000u, ==, 0x80000000u);
    g_assert_cmphex(rd(qts, FRM_2) & 0xffffffe1, ==, 0);
    g_assert_cmpuint(single_tile_frames(qts), ==, 0);
    qtest_clock_step(qts, 2 * SYNTHETIC_FRAME_NS);
    g_assert_cmpuint(single_tile_frames(qts), ==, 0);
    g_assert_cmphex(single_tile_pixel(qts, dir, "after-reset.ppm", 10, 10),
                    ==, 0);
    qtest_quit(qts);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-gbe/identity-reset", test_identity_and_reset);
    qtest_add_func("/sgi-gbe/tables-geometry", test_tables_and_geometry);
    qtest_add_func("/sgi-gbe/unsupported", test_unsupported_accesses);
    qtest_add_func("/sgi-gbe/unsupported-64bit",
                   test_unsupported_64bit_access);
    qtest_add_func("/sgi-gbe/unsupported-controls", test_unsupported_controls);
    qtest_add_func("/sgi-gbe/migration", test_migration);
    qtest_add_func("/sgi-gbe/synthetic-single-tile",
                   test_single_tile_frozen_then_present);
    qtest_add_func("/sgi-gbe/synthetic-out-of-fit",
                   test_single_tile_out_of_fit);
    qtest_add_func("/sgi-gbe/synthetic-zero-entry",
                   test_single_tile_zero_entry);
    qtest_add_func("/sgi-gbe/synthetic-rejects-other-controls",
                   test_single_tile_rejects_other_controls);
    qtest_add_func("/sgi-gbe/synthetic-reset-migration-block",
                   test_single_tile_reset_and_migration_block);
    return g_test_run();
}
