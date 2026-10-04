/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Attach each SGI machine's writable stores as block nodes through the
 * machine property named for the catalogue's storage item, and check that a
 * store persists across QEMU processes, that a wrongly sized or unknown
 * backing is refused, and that the legacy -drive forms still work.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-launch.h"

#define SN0_FLASH_SIZE (1024 * 1024)
#define SN0_FLASH_BASE UINT64_C(0x1fc00000)
#define SN0_FLASH_MARK 0x100
#define SN0_FLASH_PROBE 0x8000

/* Origin 300's 16 MiB two-position flash and the PROM image -bios takes. */
#define SN1_FLASH_SIZE (16 * 1024 * 1024)
#define SN1_PROM_SIZE 0x1686a8
#define SN1_PROM_RESET UINT64_C(0x1fc00000)
#define SN1_FLASH_MARK 0x2000
#define SN1_MACHINE "-cpu R14000 -smp 2"

static char *directory;

static char *store_path(const char *name)
{
    return g_strdup_printf("%s/%s.raw", directory, name);
}

/* An erased store of @size bytes with @mark at SN0_FLASH_MARK. */
static char *make_store(const char *name, size_t size, uint8_t mark)
{
    g_autofree char *bytes = g_malloc(size);
    char *path = store_path(name);

    memset(bytes, 0xff, size);
    if (size > SN0_FLASH_MARK) {
        bytes[SN0_FLASH_MARK] = mark;
    }
    g_assert_true(g_file_set_contents(path, bytes, size, NULL));
    return path;
}

static uint8_t file_byte(const char *path, size_t offset)
{
    g_autofree char *bytes = NULL;
    gsize length;

    g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    g_assert_cmpuint(offset, <, length);
    return bytes[offset];
}

/* "-blockdev ..." for a raw file node named @node. */
static char *blockdev(const char *node, const char *path)
{
    return g_strdup_printf("driver=raw,node-name=%s,file.driver=file,"
                           "file.filename=%s", node, path);
}

static uint8_t sn0_flash_read(QTestState *qts, unsigned int cpu,
                              uint32_t offset)
{
    uint8_t value;

    g_assert_true(qtest_memread_cpu(qts, cpu, SN0_FLASH_BASE + offset,
                                    &value, sizeof(value)));
    return value;
}

static void sn0_flash_write(QTestState *qts, unsigned int cpu,
                            uint32_t offset, uint8_t value)
{
    g_assert_true(qtest_memwrite_cpu(qts, cpu, SN0_FLASH_BASE + offset,
                                     &value, sizeof(value)));
}

/* The AMD program sequence the node's Am29F080-class part answers. */
static void sn0_flash_program(QTestState *qts, unsigned int cpu,
                              uint32_t offset, uint8_t value)
{
    sn0_flash_write(qts, cpu, 0x555, 0xaa);
    sn0_flash_write(qts, cpu, 0x2aa, 0x55);
    sn0_flash_write(qts, cpu, 0x555, 0xa0);
    sn0_flash_write(qts, cpu, offset, value);
}

static QTestState *start_origin200_flash(const char *path)
{
    g_autofree char *node = blockdev("flash", path);

    return qtest_initf("-M origin200,node0-flash=flash -blockdev %s", node);
}

/*
 * The node flash reads its file, not a seeded image; a programmed byte
 * reaches the file and the next QEMU process reads it back.
 */
static void test_sn0_node_flash_persists(void)
{
    g_autofree char *path = make_store("o200-node0", SN0_FLASH_SIZE, 0x69);
    QTestState *qts = start_origin200_flash(path);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x69);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0xff);
    sn0_flash_program(qts, 0, SN0_FLASH_PROBE, 0x5a);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0x5a);
    qtest_quit(qts);

    g_assert_cmphex(file_byte(path, SN0_FLASH_PROBE), ==, 0x5a);
    qts = start_origin200_flash(path);
    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_PROBE), ==, 0x5a);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* Each node's property backs that node's own part. */
static void test_sn0_node_flash_per_node(void)
{
    g_autofree char *path0 = make_store("o2k-node0", SN0_FLASH_SIZE, 0x10);
    g_autofree char *path1 = make_store("o2k-node1", SN0_FLASH_SIZE, 0x11);
    g_autofree char *node0 = blockdev("n0", path0);
    g_autofree char *node1 = blockdev("n1", path1);
    QTestState *qts = qtest_initf(
        "-M origin2000,topology=origin2000-deskside,nodes=2,"
        "node0-flash=n0,node1-flash=n1 -smp 4 -m 512M "
        "-blockdev %s -blockdev %s", node0, node1);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x10);
    g_assert_cmphex(sn0_flash_read(qts, 2, SN0_FLASH_MARK), ==, 0x11);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path0), ==, 0);
    g_assert_cmpint(g_unlink(path1), ==, 0);
}

/* The legacy pflash drive still backs the node flash. */
static void test_sn0_node_flash_legacy_drive(void)
{
    g_autofree char *path = make_store("o200-legacy", SN0_FLASH_SIZE, 0x42);
    QTestState *qts = qtest_initf("-M origin200 "
                                  "-drive if=pflash,file=%s,format=raw",
                                  path);

    g_assert_cmphex(sn0_flash_read(qts, 0, SN0_FLASH_MARK), ==, 0x42);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void assert_rejected(const char *machine, const char *extra_option,
                            const char *extra_value, const char *message)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-display", "none", "-S",
        "-qmp", "stdio", extra_option, extra_value, NULL,
    };

    assert_launch_rejected(args, message);
}

static void test_sn0_node_flash_refusals(void)
{
    g_autofree char *small = make_store("o200-small", SN0_FLASH_SIZE / 2, 0);
    g_autofree char *right = make_store("o200-right", SN0_FLASH_SIZE, 0);
    g_autofree char *small_node = blockdev("flash", small);
    g_autofree char *right_node = blockdev("flash", right);
    g_autofree char *legacy = g_strdup_printf("if=pflash,file=%s,format=raw",
                                              right);
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M", "origin200,node0-flash=flash",
        "-display", "none", "-S", "-qmp", "stdio",
        "-blockdev", right_node, "-drive", legacy, NULL,
    };

    assert_rejected("origin200,node0-flash=flash", "-blockdev", small_node,
                    "origin200: node0-flash is 524288 bytes; it must be "
                    "exactly 1048576");
    assert_rejected("origin200,node0-flash=missing", "-blockdev", right_node,
                    "origin200: node0-flash names 'missing', which is no "
                    "block node or drive");
    assert_rejected("origin200,node1-flash=flash", "-blockdev", right_node,
                    "origin200: topology 'origin200' with 1 node board has "
                    "no node1-flash");
    assert_launch_rejected(both, "node0-flash and -drive if=pflash,unit=0 "
                           "both back node 0's flash");
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(right), ==, 0);
}

/* One block node cannot back two stores. */
static void test_shared_node_refused(void)
{
    g_autofree char *path = make_store("shared", SN0_FLASH_SIZE, 0);
    g_autofree char *node = blockdev("flash", path);

    const char *args[] = {
        qtest_qemu_binary(NULL), "-M",
        "origin2000,topology=origin2000-deskside,nodes=2,"
        "node0-flash=flash,node1-flash=flash", "-smp", "4", "-m", "512M",
        "-display", "none", "-S", "-qmp", "stdio", "-blockdev", node, NULL,
    };

    assert_launch_rejected(args, "origin2000: node0-flash and node1-flash "
                           "both name 'flash'");
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* A zero-filled stand-in for the IP35 PROM -bios requires. */
static char *make_sn1_prom(void)
{
    g_autofree char *bytes = g_malloc0(SN1_PROM_SIZE);
    char *path = store_path("ip35-prom");

    g_assert_true(g_file_set_contents(path, bytes, SN1_PROM_SIZE, NULL));
    return path;
}

/* A 16 MiB flash image with @mark at SN1_FLASH_MARK of position zero. */
static char *make_sn1_flash(const char *name, size_t size, uint8_t mark)
{
    g_autofree char *bytes = g_malloc(size);
    char *path = store_path(name);

    memset(bytes, 0xff, size);
    bytes[SN1_FLASH_MARK] = mark;
    g_assert_true(g_file_set_contents(path, bytes, size, NULL));
    return path;
}

/* The reset view answers from the bound flash, not the -bios image. */
static void test_sn1_node_flash_bound(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *path = make_sn1_flash("o300-node0", SN1_FLASH_SIZE,
                                           0x35);
    g_autofree char *node = blockdev("flash", path);
    QTestState *qts = qtest_initf("-M origin300,node0-flash=flash "
                                  SN1_MACHINE " -bios %s -blockdev %s",
                                  prom, node);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x35);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* A protocol node backs the flash as well as a raw one, as on the Hub. */
static void test_sn1_node_flash_protocol_node(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *path = make_sn1_flash("o300-file", SN1_FLASH_SIZE,
                                           0x36);
    QTestState *qts = qtest_initf("-M origin300,node0-flash=flash "
                                  SN1_MACHINE " -bios %s -blockdev "
                                  "driver=file,node-name=flash,filename=%s",
                                  prom, path);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x36);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_sn1_node_flash_legacy_drive(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *path = make_sn1_flash("o300-legacy", SN1_FLASH_SIZE,
                                           0x53);
    QTestState *qts = qtest_initf("-M origin300 " SN1_MACHINE " -bios %s "
                                  "-drive if=pflash,file=%s,format=raw",
                                  prom, path);

    g_assert_cmphex(qtest_readb(qts, SN1_PROM_RESET + SN1_FLASH_MARK), ==,
                    0x53);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_sn1_node_flash_refusals(void)
{
    g_autofree char *prom = make_sn1_prom();
    g_autofree char *small = make_sn1_flash("o300-small", SN1_FLASH_SIZE / 2,
                                            0);
    g_autofree char *right = make_sn1_flash("o300-right", SN1_FLASH_SIZE, 0);
    g_autofree char *small_node = blockdev("flash", small);
    g_autofree char *right_node = blockdev("flash", right);
    g_autofree char *legacy = g_strdup_printf("if=pflash,file=%s,format=raw",
                                              right);
    const char *wrong_size[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-bios", prom, "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", small_node, NULL,
    };
    const char *both[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-bios", prom, "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", right_node, "-drive", legacy,
        NULL,
    };
    g_autofree char *stray = g_strdup_printf("if=pflash,unit=1,file=%s,"
                                             "format=raw", right);
    const char *extra[] = {
        qtest_qemu_binary(NULL), "-M", "origin300,node0-flash=flash",
        "-cpu", "R14000", "-smp", "2", "-bios", prom, "-display", "none",
        "-S", "-qmp", "stdio", "-blockdev", right_node, "-drive", stray,
        NULL,
    };

    assert_launch_rejected(wrong_size, "origin300: node0-flash is 8388608 "
                           "bytes; it must be exactly 16777216");
    assert_launch_rejected(both, "origin300: node0-flash and -drive "
                           "if=pflash,unit=0 both back node 0's flash");
    assert_launch_rejected(extra, "origin300 accepts one 16 MiB pflash "
                           "image per node");
    g_assert_cmpint(g_unlink(small), ==, 0);
    g_assert_cmpint(g_unlink(right), ==, 0);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sgi-machine-storage-XXXXXX", &error);
    g_assert_no_error(error);

    qtest_add_func("/sgi-machine-storage/sn0-node-flash/persists",
                   test_sn0_node_flash_persists);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/per-node",
                   test_sn0_node_flash_per_node);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/legacy-drive",
                   test_sn0_node_flash_legacy_drive);
    qtest_add_func("/sgi-machine-storage/sn0-node-flash/refusals",
                   test_sn0_node_flash_refusals);
    qtest_add_func("/sgi-machine-storage/shared-node-refused",
                   test_shared_node_refused);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/bound",
                   test_sn1_node_flash_bound);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/protocol-node",
                   test_sn1_node_flash_protocol_node);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/legacy-drive",
                   test_sn1_node_flash_legacy_drive);
    qtest_add_func("/sgi-machine-storage/sn1-node-flash/refusals",
                   test_sn1_node_flash_refusals);
    ret = g_test_run();
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(directory);
    return ret;
}
