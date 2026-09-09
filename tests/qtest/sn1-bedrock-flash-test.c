/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 boot-flash adapter, on its own. The empty machine supplies an
 * address space and nothing else, so the two windows below are test
 * addresses rather than the boot-window addresses an Origin 300 uses.
 *
 * Every image here is synthetic. No test needs, or asserts anything about,
 * the contents of a real boot image.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define FLASH_PATH "/machine/peripheral/flash"
#define PRIMARY 0x10000000ULL
#define ALTERNATE 0x20000000ULL

#define FLASH_SIZE (16 * 1024 * 1024)
#define SECTOR_SIZE (64 * 1024)

/* A command or mode word at flash word index n arrives at bus offset 8n. */
#define WORD(n) ((uint64_t)(n) * 8)
/* A flash byte offset n is programmed through bus offset 4n, n even. */
#define BYTE(n) ((uint64_t)(n) * 4)

#define UNLOCK0 WORD(0x555)
#define UNLOCK1 WORD(0x2aa)
#define QUERY WORD(0x55)

/* CFI query values are 16 bits wide at twice the query index. */
#define QUERY_READ(i) ((uint64_t)(i) * 2)

static char *flash_image(const char *tag)
{
    char *path = g_strdup_printf("%s/sn1-bedrock-flash-%s-XXXXXX",
                                 g_get_tmp_dir(), tag);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, FLASH_SIZE), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void image_write(const char *path, uint64_t offset, const void *buffer,
                        size_t length)
{
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, buffer, length, offset), ==, (ssize_t)length);
    g_assert_cmpint(close(fd), ==, 0);
}

static void image_write_be64(const char *path, uint64_t offset, uint64_t value)
{
    uint8_t bytes[8];

    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = value >> (56 - 8 * i);
    }
    image_write(path, offset, bytes, sizeof(bytes));
}

/*
 * Programming only clears bits, so a sector a test means to program has to
 * start out erased. The images are otherwise left sparse.
 */
static void image_erase_sector(const char *path, uint64_t offset)
{
    g_autofree uint8_t *erased = g_malloc(SECTOR_SIZE);

    memset(erased, 0xff, SECTOR_SIZE);
    image_write(path, offset, erased, SECTOR_SIZE);
}

static QTestState *flash_start(const char *image, const char *options,
                               const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -S "
                                "-drive if=none,id=fl,file=%s,format=raw%s "
                                "-device sgi-bedrock-flash,id=flash,drive=fl "
                                "%s", image, options, extra);

    qtest_sysbus_mmio_map(s, FLASH_PATH, 0, PRIMARY);
    qtest_sysbus_mmio_map(s, FLASH_PATH, 1, ALTERNATE);
    return s;
}

static void flash_program(QTestState *s, uint64_t base, uint64_t offset,
                          uint64_t word)
{
    qtest_writeq(s, base + UNLOCK0, 0xaa);
    qtest_writeq(s, base + UNLOCK1, 0x55);
    qtest_writeq(s, base + UNLOCK0, 0xa0);
    qtest_writeq(s, base + BYTE(offset), word);
}

static char *sized_image(const char *tag, uint64_t size)
{
    char *path = g_strdup_printf("%s/sn1-bedrock-flash-%s-XXXXXX",
                                 g_get_tmp_dir(), tag);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void assert_qemu_rejects(const char *image, const char *message)
{
    g_autofree char *drive = g_strdup_printf("if=none,id=fl,file=%s,"
                                             "format=raw", image);
    g_autofree char *output = NULL;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-accel", "qtest",
        "-cpu", "R10000", "-S",
        "-display", "none", "-drive", drive,
        "-device", "sgi-bedrock-flash,id=flash,drive=fl", NULL
    };
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL, NULL,
                               &output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(output, message));
}

/*
 * The device takes exactly the published 16 MiB. It refuses a shorter or a
 * longer image, and refuses to come up with no image at all, rather than
 * inventing what an absent or mis-sized part would answer. Contents are its
 * own business: a run-of-the-mill image and one already carrying the result
 * of an earlier update are both accepted.
 */
static void test_backing_file(void)
{
    g_autofree char *small = sized_image("small", FLASH_SIZE / 2);
    g_autofree char *large = sized_image("large", FLASH_SIZE + 1);
    g_autofree char *good = flash_image("good");
    g_autofree char *output = NULL;
    const char *bare[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-accel", "qtest",
        "-cpu", "R10000", "-S",
        "-display", "none", "-device", "sgi-bedrock-flash,id=flash", NULL
    };
    int wait_status = 0;
    QTestState *s;

    assert_qemu_rejects(small, "requires 16777216 bytes");
    assert_qemu_rejects(large, "requires 16777216 bytes");
    assert_qemu_rejects("/nonexistent/sn1-bedrock-flash", "No such file");

    g_assert_true(g_spawn_sync(NULL, (char **)bare, NULL, 0, NULL, NULL, NULL,
                               &output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(output, "raw flash image"));

    s = flash_start(good, "", "");
    g_assert_cmphex(qtest_readq(s, PRIMARY), ==, 0);
    qtest_quit(s);

    g_assert_cmpint(unlink(small), ==, 0);
    g_assert_cmpint(unlink(large), ==, 0);
    g_assert_cmpint(unlink(good), ==, 0);
}

/*
 * A read reaches the byte at the same offset, at every width the bus
 * carries, out to the last byte of the part -- including an image whose
 * contents are already the result of an earlier update rather than a fresh
 * one.
 */
static void test_one_to_one_reads(void)
{
    g_autofree char *image = flash_image("reads");
    static const uint8_t head[8] = {
        0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79, 0x88
    };
    QTestState *s;

    image_write(image, 0, head, sizeof(head));
    image_write_be64(image, 0x10000, 0x1122334455667788ULL);
    image_write_be64(image, 0x1e0000, 0xfeedfacecafef00dULL);
    image_write_be64(image, 0x1f0000, 0x0badc0debaadf00dULL);
    image_write_be64(image, FLASH_SIZE - 8, 0x0123456789abcdefULL);

    s = flash_start(image, "", "");
    g_assert_cmphex(qtest_readb(s, PRIMARY), ==, 0x1f);
    g_assert_cmphex(qtest_readb(s, PRIMARY + 3), ==, 0x4c);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 2), ==, 0x3d4c);
    g_assert_cmphex(qtest_readl(s, PRIMARY + 4), ==, 0x5b6a7988);
    g_assert_cmphex(qtest_readq(s, PRIMARY), ==, 0x1f2e3d4c5b6a7988ULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x10000), ==,
                    0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x1f0000), ==,
                    0x0badc0debaadf00dULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + FLASH_SIZE - 8), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readb(s, PRIMARY + FLASH_SIZE - 1), ==, 0xef);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

/* Both software selections reach the same stored bytes. */
static void test_selections_alias(void)
{
    g_autofree char *image = flash_image("alias");
    QTestState *s;

    image_write_be64(image, 0x10000, 0x1122334455667788ULL);
    image_write_be64(image, 0x1e0000, 0xfeedfacecafef00dULL);
    image_write_be64(image, 0x1f0000, 0x0badc0debaadf00dULL);
    image_write_be64(image, FLASH_SIZE - 8, 0x0123456789abcdefULL);
    image_erase_sector(image, 0x20000);

    s = flash_start(image, "", "");
    g_assert_cmphex(qtest_readq(s, ALTERNATE + 0x10000), ==,
                    0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(s, ALTERNATE + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    g_assert_cmphex(qtest_readq(s, ALTERNATE + 0x1f0000), ==,
                    0x0badc0debaadf00dULL);
    g_assert_cmphex(qtest_readq(s, ALTERNATE + FLASH_SIZE - 8), ==,
                    0x0123456789abcdefULL);

    /* A word programmed through one selection is readable through the other. */
    flash_program(s, PRIMARY, 0x20000, 0x0f0f);
    g_assert_cmphex(qtest_readw(s, ALTERNATE + 0x20000), ==, 0x0f0f);
    flash_program(s, ALTERNATE, 0x20002, 0x33cc);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0x20002), ==, 0x33cc);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, PRIMARY + 0x20000), ==, 0x0f0f33cc);
    g_assert_cmphex(qtest_readl(s, ALTERNATE + 0x20000), ==, 0x0f0f33cc);
    qtest_quit(s);

    /* And it is in the image, so a fresh run reads it back. */
    s = flash_start(image, "", "");
    g_assert_cmphex(qtest_readl(s, PRIMARY + 0x20000), ==, 0x0f0f33cc);
    g_assert_cmphex(qtest_readl(s, ALTERNATE + 0x20000), ==, 0x0f0f33cc);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-flash/backing-file", test_backing_file);
    qtest_add_func("sn1/bedrock-flash/one-to-one-reads",
                   test_one_to_one_reads);
    qtest_add_func("sn1/bedrock-flash/selections-alias", test_selections_alias);
    return g_test_run();
}
