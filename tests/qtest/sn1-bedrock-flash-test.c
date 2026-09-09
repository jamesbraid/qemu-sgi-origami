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

/*
 * The two boot spaces, at their machine-bus addresses. A command or
 * programming store is relative to these; an ordinary read is relative to the
 * software read address it lands in, which is why the two disagree at
 * SECOND_READ.
 */
#define HUB 0x10000000000ULL
#define LOCAL (HUB + 0x18000000ULL)
#define REMOTE (HUB + 0x38000000ULL)

/* One PROM stride into a boot space: the second software read address. */
#define SECOND_READ 0x02000000ULL
#define PRIMARY LOCAL
#define ALTERNATE (LOCAL + SECOND_READ)

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

/*
 * The whole backing is commandable: a store at window offset 4n reaches
 * backing byte n, and 4 * 16 MiB fits inside a 128 MiB boot space. The last
 * word begins at backing byte LAST_WORD and the first store with no backing
 * word behind it is at window offset WRITE_LIMIT.
 */
#define LAST_WORD (FLASH_SIZE - 2)
#define LAST_SECTOR (FLASH_SIZE - SECTOR_SIZE)
#define WRITE_LIMIT (4ULL * FLASH_SIZE)
/*
 * A sector for the erase-bounds case: far above the quarter of the backing
 * the superseded mapping could reach, and with a neighbour on each side so
 * the bounds have something either way to check.
 */
#define HIGH_SECTOR (FLASH_SIZE - 2 * SECTOR_SIZE)

static char *sized_image(const char *tag, off_t size)
{
    char *path = g_strdup_printf("%s/sn1-bedrock-flash-%s-XXXXXX",
                                 g_get_tmp_dir(), tag);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *flash_image(const char *tag)
{
    return sized_image(tag, FLASH_SIZE);
}

static void image_write(const char *path, off_t offset, const void *buffer,
                        size_t length)
{
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, buffer, length, offset), ==, (ssize_t)length);
    g_assert_cmpint(close(fd), ==, 0);
}

static void image_write_be64(const char *path, off_t offset,
                             uint64_t value)
{
    uint8_t bytes[8];

    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = value >> (56 - 8 * i);
    }
    image_write(path, offset, bytes, sizeof(bytes));
}

static void image_read(const char *path, off_t offset, void *buffer,
                       size_t length)
{
    int fd = open(path, O_RDONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, buffer, length, offset), ==, (ssize_t)length);
    g_assert_cmpint(close(fd), ==, 0);
}

/*
 * Programming only clears bits, so a sector a test means to program has to
 * start out erased. The images are otherwise left sparse.
 */
static void image_erase_sector(const char *path, off_t offset)
{
    g_autofree uint8_t *erased = g_malloc(SECTOR_SIZE);

    memset(erased, 0xff, SECTOR_SIZE);
    image_write(path, offset, erased, SECTOR_SIZE);
}

static QTestState *flash_start(const char *image, const char *options,
                               const char *extra)
{
    /*
     * The machine runs rather than being held stopped: erase completes on a
     * virtual-clock timer, and that clock only advances while it does.
     */
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-drive if=none,id=fl,file=%s,format=raw%s "
                                "-device sgi-bedrock-flash,id=flash,drive=fl "
                                "%s", image, options, extra);

    qtest_sysbus_mmio_map(s, FLASH_PATH, 0, LOCAL);
    qtest_sysbus_mmio_map(s, FLASH_PATH, 1, REMOTE);
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

static void flash_reset_to_array(QTestState *s, uint64_t base)
{
    qtest_writeq(s, base + UNLOCK0, 0xf0);
}

static void flash_enter_query(QTestState *s, uint64_t base)
{
    qtest_writeq(s, base + QUERY, 0x98);
}

static void flash_erase_sector(QTestState *s, uint64_t base, uint64_t offset)
{
    qtest_writeq(s, base + UNLOCK0, 0xaa);
    qtest_writeq(s, base + UNLOCK1, 0x55);
    qtest_writeq(s, base + UNLOCK0, 0x80);
    qtest_writeq(s, base + UNLOCK0, 0xaa);
    qtest_writeq(s, base + UNLOCK1, 0x55);
    qtest_writeq(s, base + BYTE(offset), 0x30);
    /* The window for further sector commands, then the erase itself. */
    qtest_clock_step_next(s);
    qtest_clock_step_next(s);
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
    flash_program(s, LOCAL, 0x20000, 0x0f0f);
    g_assert_cmphex(qtest_readw(s, ALTERNATE + 0x20000), ==, 0x0f0f);
    flash_program(s, REMOTE, 0x20002, 0x33cc);
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

/*
 * The query command rides the same 64-bit store as any other command, at
 * eight times its flash word address, and the answers come back as 16-bit
 * values at twice the query index. Identifiers are a separate mode the
 * firmware never enters, and nothing here depends on them.
 */
static void test_query_mode(void)
{
    g_autofree char *image = flash_image("query");
    QTestState *s;

    /* Array contents the query answers cannot be confused with. */
    image_write_be64(image, 0x20, 0xdeadbeefdeadbeefULL);
    image_write_be64(image, 0x28, 0xdeadbeefdeadbeefULL);

    s = flash_start(image, "", "");
    /*
     * The offset the published byte-write description would put this command
     * at: eight times the flash byte rather than eight times the flash word.
     * It is a well-formed store at an aligned address, so the adapter
     * forwards it, and the flash must not read it as the query command.
     */
    qtest_writeq(s, LOCAL + 0x550, 0x98);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x20), ==, 0xdeadbeefdeadbeefULL);

    qtest_writeq(s, LOCAL + QUERY, 0x98);
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x10)), ==, 'Q');
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x11)), ==, 'R');
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x12)), ==, 'Y');
    /* The AMD-compatible primary command set. */
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x13)), ==, 0x0002);
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x14)), ==, 0x0000);
    /* Both selections are the same device, so both are in query mode. */
    g_assert_cmphex(qtest_readw(s, ALTERNATE + QUERY_READ(0x10)), ==, 'Q');

    flash_reset_to_array(s, LOCAL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x20), ==, 0xdeadbeefdeadbeefULL);
    g_assert_cmphex(qtest_readq(s, ALTERNATE + 0x20), ==,
                    0xdeadbeefdeadbeefULL);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * Programming reaches the start, the middle and the last word of a sector,
 * puts the two bytes of each word down in that order, and survives the run
 * that wrote it.
 */
static void test_program(void)
{
    g_autofree char *image = flash_image("program");
    uint8_t stored[4];
    QTestState *s;

    image_erase_sector(image, 0);
    image_erase_sector(image, LAST_SECTOR);
    image_write_be64(image, SECTOR_SIZE, 0x8899aabbccddeeffULL);

    s = flash_start(image, "", "");
    flash_program(s, LOCAL, 0, 0x1234);
    /* Polling the programmed word sees the new data, so the write is done. */
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0), ==, 0x1234);
    g_assert_cmphex(qtest_readb(s, PRIMARY + 0), ==, 0x12);
    g_assert_cmphex(qtest_readb(s, PRIMARY + 1), ==, 0x34);

    flash_program(s, LOCAL, SECTOR_SIZE / 2, 0xa55a);
    g_assert_cmphex(qtest_readw(s, PRIMARY + SECTOR_SIZE / 2), ==, 0xa55a);
    flash_program(s, LOCAL, SECTOR_SIZE - 2, 0x0f1e);
    g_assert_cmphex(qtest_readw(s, PRIMARY + SECTOR_SIZE - 2), ==, 0x0f1e);
    /* Programming the last word of a sector does not touch the next one. */
    g_assert_cmphex(qtest_readw(s, PRIMARY + SECTOR_SIZE), ==, 0x8899);

    /*
     * The transform at both ends of the backing: the lowest store reaches
     * backing byte zero and the highest reaches the last word. Reads are
     * one-to-one, so finding each word back at its backing offset pins 4n.
     */
    QEMU_BUILD_BUG_ON(BYTE(2) != 8);
    QEMU_BUILD_BUG_ON(BYTE(LAST_WORD) != WRITE_LIMIT - 8);
    flash_program(s, LOCAL, LAST_WORD, 0x9876);
    g_assert_cmphex(qtest_readw(s, PRIMARY + LAST_WORD), ==,
                    0x9876);
    qtest_quit(s);

    image_read(image, 0, stored, sizeof(stored));
    g_assert_cmphex(stored[0], ==, 0x12);
    g_assert_cmphex(stored[1], ==, 0x34);
    g_assert_cmphex(stored[2], ==, 0xff);

    s = flash_start(image, "", "");
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0), ==, 0x1234);
    g_assert_cmphex(qtest_readw(s, PRIMARY + SECTOR_SIZE / 2), ==, 0xa55a);
    g_assert_cmphex(qtest_readw(s, PRIMARY + SECTOR_SIZE - 2), ==, 0x0f1e);
    g_assert_cmphex(qtest_readw(s, PRIMARY + LAST_WORD), ==,
                    0x9876);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * An erase clears one 64 KiB sector and leaves the sectors either side of it
 * alone, whether it is the first sector or one further up.
 *
 * The upper sector is the last one in the backing, reached through the other
 * boot space. Both are commandable because a store at window offset 4n has
 * the whole 16 MiB behind it inside a 128 MiB space.
 */
static void test_sector_erase(void)
{
    g_autofree char *image = flash_image("erase");
    const off_t high_sector = HIGH_SECTOR;
    QTestState *s;

    image_write_be64(image, 0, 0x1111111111111111ULL);
    image_write_be64(image, SECTOR_SIZE - 8, 0x2222222222222222ULL);
    image_write_be64(image, SECTOR_SIZE, 0x3333333333333333ULL);
    image_write_be64(image, high_sector - 8, 0x4444444444444444ULL);
    image_write_be64(image, high_sector, 0x5555555555555555ULL);
    image_write_be64(image, high_sector + SECTOR_SIZE - 8,
                     0x6666666666666666ULL);
    image_write_be64(image, high_sector + SECTOR_SIZE, 0x7777777777777777ULL);

    s = flash_start(image, "", "");
    flash_erase_sector(s, LOCAL, 0);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PRIMARY + SECTOR_SIZE - 8), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PRIMARY + SECTOR_SIZE), ==,
                    0x3333333333333333ULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + high_sector), ==,
                    0x5555555555555555ULL);

    /* Commandable, and out of reach of the mapping this one replaced. */
    QEMU_BUILD_BUG_ON(BYTE(HIGH_SECTOR) >= WRITE_LIMIT);
    QEMU_BUILD_BUG_ON(HIGH_SECTOR < FLASH_SIZE / 4);
    flash_erase_sector(s, REMOTE, high_sector);
    g_assert_cmphex(qtest_readq(s, PRIMARY + high_sector - 8), ==,
                    0x4444444444444444ULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + high_sector), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PRIMARY + high_sector + SECTOR_SIZE - 8), ==,
                    UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PRIMARY + high_sector + SECTOR_SIZE), ==,
                    0x7777777777777777ULL);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

static char *log_file(const char *tag)
{
    char *path = g_strdup_printf("%s/sn1-bedrock-flash-%s-XXXXXX.log",
                                 g_get_tmp_dir(), tag);
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static unsigned count_lines(const char *text, const char *needle)
{
    unsigned found = 0;

    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) {
        found++;
    }
    return found;
}

/*
 * The whole 16 MiB is commandable through either boot space. Program the
 * first word, the midpoint word and the last word, and check each landed at
 * the backing offset a quarter of its window offset. The last word carries
 * two distinct bytes so a swapped final lane shows up, and its second byte is
 * the highest backing byte there is.
 *
 * The first store with no backing word behind it must be refused before any
 * flash cycle, which is what separates an exclusive bound from an inclusive
 * one: the refused offset is exactly four times the backing size.
 */
static void check_aperture(const char *tag, uint64_t base)
{
    g_autofree char *image = flash_image(tag);
    g_autofree char *log = log_file(tag);
    g_autofree char *found = NULL;
    g_autofree char *extra = NULL;
    uint8_t stored[2];
    QTestState *s;

    image_erase_sector(image, 0);
    image_erase_sector(image, FLASH_SIZE / 2);
    image_erase_sector(image, LAST_SECTOR);

    extra = g_strdup_printf("-d guest_errors,trace:pflash_io_write -D %s", log);
    s = flash_start(image, "", extra);

    flash_program(s, base, 0, 0x1122);
    flash_program(s, base, FLASH_SIZE / 2, 0x3344);
    flash_program(s, base, LAST_WORD, 0x5566);

    /* Read back through the first software read address, one-to-one. */
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0), ==, 0x1122);
    g_assert_cmphex(qtest_readw(s, PRIMARY + FLASH_SIZE / 2), ==, 0x3344);
    g_assert_cmphex(qtest_readw(s, PRIMARY + LAST_WORD), ==, 0x5566);
    /* Both lanes of the last word, the second being the highest byte. */
    g_assert_cmphex(qtest_readb(s, PRIMARY + LAST_WORD), ==, 0x55);
    g_assert_cmphex(qtest_readb(s, PRIMARY + FLASH_SIZE - 1), ==, 0x66);

    /* One past the last backing word: refused, no cycle, nothing stored. */
    qtest_writeq(s, base + WRITE_LIMIT, 0xaa);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "past the last backing word"));
    /* Three programs of four cycles each, and not one more. */
    g_assert_cmpuint(count_lines(found, "pflash_io_write"), ==, 12);

    image_read(image, FLASH_SIZE - 2, stored, sizeof(stored));
    g_assert_cmphex(stored[0], ==, 0x55);
    g_assert_cmphex(stored[1], ==, 0x66);

    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void test_local_aperture(void)
{
    check_aperture("local-aperture", LOCAL);
}

static void test_remote_aperture(void)
{
    check_aperture("remote-aperture", REMOTE);
}

/*
 * One address, two meanings. At a stride into the boot space an ordinary read
 * starts the second software read address at backing byte zero, while a store
 * reaches the middle of the backing. Anything that decodes writes from the
 * read address instead -- the superseded mapping, and the 4 MiB ceiling that
 * came with it -- fails here.
 */
static void test_access_type_split(void)
{
    g_autofree char *image = flash_image("split");
    QTestState *s;

    image_erase_sector(image, 0);
    image_erase_sector(image, FLASH_SIZE / 2);
    image_write_be64(image, 0, 0xfeedfacecafef00dULL);

    s = flash_start(image, "", "");

    /* The read at that address starts the second view, at backing zero. */
    g_assert_cmphex(qtest_readq(s, LOCAL + SECOND_READ), ==,
                    qtest_readq(s, PRIMARY + 0));

    /* The store at the same address reaches the midpoint word instead. */
    flash_program(s, LOCAL, FLASH_SIZE / 2, 0x0f1e);
    g_assert_cmphex(qtest_readw(s, PRIMARY + FLASH_SIZE / 2), ==, 0x0f1e);
    /* Backing zero, which a per-view decoder would have hit, is untouched. */
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0), ==, 0xfeedfacecafef00dULL);
    g_assert_cmphex(qtest_readq(s, ALTERNATE + 0), ==, 0xfeedfacecafef00dULL);
    /* And the midpoint is visible through the second read address too. */
    g_assert_cmphex(qtest_readw(s, ALTERNATE + FLASH_SIZE / 2), ==, 0x0f1e);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * The one place where the width of a flash cycle is visible. A 64-bit store
 * has to reach the flash as a single two-byte cycle; split into two one-byte
 * cycles it would be two commands, and the firmware's programming sequence
 * would fall apart. The CFI device's own trace says which happened.
 *
 * A store with anything set above the flash word is not something the
 * firmware ever sends, and the board's answer to it is unknown. Refuse it
 * outright: say so, start no cycle, and leave the data alone.
 */
static void test_cfi_cycle_width(void)
{
    g_autofree char *image = flash_image("cycle");
    g_autofree char *good_log = log_file("cycle-good");
    g_autofree char *wide_log = log_file("cycle-wide");
    g_autofree char *found = NULL;
    g_autofree char *extra = NULL;
    uint8_t stored[2];
    QTestState *s;

    image_erase_sector(image, 0);

    extra = g_strdup_printf("-d trace:pflash_io_write -D %s", good_log);
    s = flash_start(image, "", extra);
    flash_program(s, LOCAL, 0x100, 0x1234);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(good_log, &found, NULL, NULL));
    g_assert_cmpuint(count_lines(found, "pflash_io_write"), ==, 4);
    g_assert_cmpuint(count_lines(found, "size:1"), ==, 0);
    g_assert_nonnull(strstr(found, "offset:0x0aaa size:2 value:0x00aa"));
    g_assert_nonnull(strstr(found, "offset:0x0554 size:2 value:0x0055"));
    g_assert_nonnull(strstr(found, "offset:0x0aaa size:2 value:0x00a0"));
    g_assert_nonnull(strstr(found, "offset:0x0100 size:2 value:0x1234"));
    g_clear_pointer(&found, g_free);

    g_free(extra);
    extra = g_strdup_printf("-d guest_errors,trace:pflash_io_write -D %s",
                            wide_log);
    s = flash_start(image, "", extra);
    qtest_writeq(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(s, LOCAL + UNLOCK1, 0x55);
    qtest_writeq(s, LOCAL + UNLOCK0, 0xa0);
    qtest_writeq(s, LOCAL + BYTE(0x200), 0x15678);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(wide_log, &found, NULL, NULL));
    /* The three unlock cycles reached the flash; the data cycle did not. */
    g_assert_cmpuint(count_lines(found, "pflash_io_write"), ==, 3);
    g_assert_null(strstr(found, "value:0x5678"));
    g_assert_nonnull(strstr(found, "sets bits above the 16-bit flash word"));

    image_read(image, 0x200, stored, sizeof(stored));
    g_assert_cmphex(stored[0], ==, 0xff);
    g_assert_cmphex(stored[1], ==, 0xff);

    g_assert_cmpint(unlink(good_log), ==, 0);
    g_assert_cmpint(unlink(wide_log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * The firmware sends whole 64-bit stores. Anything narrower is outside that
 * interface and the board's answer to it is unknown, so the adapter refuses
 * it rather than picking a meaning: no cycle reaches the flash, and the array
 * keeps what it had.
 */
static void test_narrow_writes(void)
{
    g_autofree char *image = flash_image("narrow");
    g_autofree char *log = log_file("narrow");
    g_autofree char *found = NULL;
    g_autofree char *extra = NULL;
    static const uint8_t expected[8] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    };
    uint8_t head[8];
    QTestState *s;

    image_erase_sector(image, 0);
    image_write_be64(image, 0, 0x0123456789abcdefULL);

    extra = g_strdup_printf("-d unimp,trace:pflash_io_write,"
                            "trace:pflash_data_write -D %s", log);
    s = flash_start(image, "", extra);

    /* Outside any command sequence: nothing should reach the flash at all. */
    qtest_writeb(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writew(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writel(s, LOCAL + UNLOCK0, 0xaa);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0), ==, 0x0123456789abcdefULL);

    /*
     * The one that would corrupt data: a program sequence opened with three
     * proper stores, then its payload sent narrow. Forwarded, that clears the
     * word; refused, the word keeps what it had. Refusing issues no cycle, so
     * the flash is left waiting for that payload and reads status rather than
     * the array -- the image below is where the damage would show.
     */
    qtest_writeq(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(s, LOCAL + UNLOCK1, 0x55);
    qtest_writeq(s, LOCAL + UNLOCK0, 0xa0);
    qtest_writew(s, LOCAL + BYTE(0), 0x0000);
    qtest_quit(s);

    image_read(image, 0, head, sizeof(head));
    g_assert_cmpmem(head, sizeof(head), expected, sizeof(expected));

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    /* The three stores opening the sequence, and nothing else. */
    g_assert_cmpuint(count_lines(found, "pflash_io_write"), ==, 3);
    g_assert_cmpuint(count_lines(found, "data offset:"), ==, 0);
    g_assert_cmpuint(count_lines(found, "outside the 64-bit store"), ==, 4);
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * A reset returns the flash to plain array reads from wherever it was, and
 * an unfinished command sequence does not survive it: a data or erase cycle
 * arriving after the reset is a stray write, not the completion of what was
 * started before.
 */
static void test_reset_states(void)
{
    g_autofree char *image = flash_image("reset");
    QTestState *s;

    image_erase_sector(image, 0);
    image_write_be64(image, 0x20, 0xdeadbeefdeadbeefULL);
    image_write_be64(image, SECTOR_SIZE, 0x3333333333333333ULL);

    s = flash_start(image, "", "");

    /* Stored bytes survive a reset taken during ordinary reads. */
    flash_program(s, LOCAL, 0x400, 0xc3c3);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0x400), ==, 0xc3c3);

    /* Query mode does not. */
    flash_enter_query(s, LOCAL);
    g_assert_cmphex(qtest_readw(s, PRIMARY + QUERY_READ(0x10)), ==, 'Q');
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x20), ==, 0xdeadbeefdeadbeefULL);

    /* Nor does a program sequence waiting for its data. */
    qtest_writeq(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(s, LOCAL + UNLOCK1, 0x55);
    qtest_writeq(s, LOCAL + UNLOCK0, 0xa0);
    qtest_system_reset(s);
    qtest_writeq(s, LOCAL + BYTE(0x402), 0x0f0f);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0x402), ==, 0xffff);

    /* Nor an erase sequence waiting for its sector. */
    qtest_writeq(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(s, LOCAL + UNLOCK1, 0x55);
    qtest_writeq(s, LOCAL + UNLOCK0, 0x80);
    qtest_writeq(s, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(s, LOCAL + UNLOCK1, 0x55);
    qtest_system_reset(s);
    qtest_writeq(s, LOCAL + BYTE(SECTOR_SIZE), 0x30);
    g_assert_cmphex(qtest_readq(s, PRIMARY + SECTOR_SIZE), ==,
                    0x3333333333333333ULL);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0x400), ==, 0xc3c3);
    qtest_quit(s);
    g_assert_cmpint(unlink(image), ==, 0);
}

/*
 * A read-only image still boots and reads, and neither programming nor
 * erasing changes a byte of it. Both attempts say so: the bytes coming back
 * unchanged look exactly like a part that programmed what it was given, so
 * the complaint is the only thing that separates the two.
 */
static void test_read_only(void)
{
    g_autofree char *image = flash_image("readonly");
    g_autofree char *log = log_file("readonly");
    g_autofree char *found = NULL;
    g_autofree char *extra = NULL;
    uint8_t before[16], after[16];
    QTestState *s;

    image_erase_sector(image, 0);
    image_write_be64(image, 0x400, 0x0123456789abcdefULL);
    image_write_be64(image, SECTOR_SIZE, 0x3333333333333333ULL);
    image_read(image, 0x400, before, sizeof(before));

    extra = g_strdup_printf("-d guest_errors -D %s", log);
    s = flash_start(image, ",readonly=on", extra);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x400), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0), ==, UINT64_MAX);

    flash_program(s, LOCAL, 0x100, 0x1234);
    g_assert_cmphex(qtest_readw(s, PRIMARY + 0x100), ==, 0xffff);
    flash_program(s, LOCAL, 0x400, 0x0000);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x400), ==,
                    0x0123456789abcdefULL);

    flash_erase_sector(s, LOCAL, 0);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x400), ==,
                    0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + SECTOR_SIZE), ==,
                    0x3333333333333333ULL);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "sgi-bedrock-flash: word program ignored: "
                            "backing store is read-only"));
    g_assert_nonnull(strstr(found, "sgi-bedrock-flash: sector erase ignored: "
                            "backing store is read-only"));

    image_read(image, 0x400, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                              "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

typedef struct {
    char *directory;
    char *socket;
    char *uri;
    char *source;
    char *destination;
    QTestState *from;
    QTestState *to;
} FlashMigration;

/*
 * The destination starts from an image that differs from the source's, so
 * reading the source's bytes on the destination shows they travelled rather
 * than having been there all along.
 */
static void migration_start(FlashMigration *m, const char *tag)
{
    GError *error = NULL;
    g_autofree char *pattern = g_strdup_printf("sn1-bedrock-flash-%s-XXXXXX",
                                               tag);

    m->directory = g_dir_make_tmp(pattern, &error);
    g_assert_no_error(error);
    m->socket = g_build_filename(m->directory, "migration.sock", NULL);
    m->uri = g_strdup_printf("unix:%s", m->socket);
    m->source = flash_image(tag);
    m->destination = flash_image(tag);
    image_erase_sector(m->source, 0);
    image_write_be64(m->source, 0x1e0000, 0xfeedfacecafef00dULL);
    image_write_be64(m->destination, 0x1e0000, 0x1111111111111111ULL);
    m->from = flash_start(m->source, "", "");
    m->to = flash_start(m->destination, "", "-incoming defer");
}

static void migration_run(FlashMigration *m)
{
    qtest_qmp_assert_success(m->to,
        "{'execute': 'migrate-incoming', 'arguments': {'uri': %s}}", m->uri);
    qtest_qmp_assert_success(m->from,
        "{'execute': 'migrate', 'arguments': {'uri': %s}}", m->uri);
    wait_for_migration(m->from);
}

static void migration_finish(FlashMigration *m)
{
    qtest_quit(m->from);
    qtest_quit(m->to);
    /* QEMU removes the migration socket on its way out. */
    unlink(m->socket);
    g_assert_cmpint(unlink(m->source), ==, 0);
    g_assert_cmpint(unlink(m->destination), ==, 0);
    g_assert_cmpint(rmdir(m->directory), ==, 0);
    g_free(m->directory);
    g_free(m->socket);
    g_free(m->uri);
    g_free(m->source);
    g_free(m->destination);
}

/* Ordinary reads: the stored bytes cross to the destination. */
static void test_migration_stored_bytes(void)
{
    FlashMigration m;

    migration_start(&m, "migrate-bytes");
    flash_program(m.from, LOCAL, 0x400, 0xc3c3);
    migration_run(&m);
    g_assert_cmphex(qtest_readw(m.to, PRIMARY + 0x400), ==, 0xc3c3);
    g_assert_cmphex(qtest_readq(m.to, PRIMARY + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    g_assert_cmphex(qtest_readq(m.to, ALTERNATE + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    migration_finish(&m);
}

/* Query mode crosses too, and the destination can leave it. */
static void test_migration_query_mode(void)
{
    FlashMigration m;

    migration_start(&m, "migrate-query");
    flash_enter_query(m.from, LOCAL);
    migration_run(&m);
    g_assert_cmphex(qtest_readw(m.to, PRIMARY + QUERY_READ(0x10)), ==, 'Q');
    g_assert_cmphex(qtest_readw(m.to, PRIMARY + QUERY_READ(0x13)), ==, 0x0002);
    flash_reset_to_array(m.to, LOCAL);
    g_assert_cmphex(qtest_readq(m.to, PRIMARY + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    migration_finish(&m);
}

/* A command sequence half issued on the source finishes on the destination. */
static void test_migration_partial_command(void)
{
    FlashMigration m;

    migration_start(&m, "migrate-partial");
    qtest_writeq(m.from, LOCAL + UNLOCK0, 0xaa);
    qtest_writeq(m.from, LOCAL + UNLOCK1, 0x55);
    qtest_writeq(m.from, LOCAL + UNLOCK0, 0xa0);
    migration_run(&m);
    qtest_writeq(m.to, LOCAL + BYTE(0x600), 0x5aa5);
    g_assert_cmphex(qtest_readw(m.to, PRIMARY + 0x600), ==, 0x5aa5);
    migration_finish(&m);
}

static void assert_writes_stay_in_first_sector(const char *log)
{
    const char *needle = "data offset:0x";
    unsigned seen = 0;

    for (const char *p = strstr(log, needle); p; p = strstr(p + 1, needle)) {
        uint64_t offset = g_ascii_strtoull(p + strlen(needle), NULL, 16);

        g_assert_cmphex(offset, <, SECTOR_SIZE);
        seen++;
    }
    g_assert_cmpuint(seen, ==, SECTOR_SIZE / 2);
}

/*
 * The access pattern the firmware uses to update its own configuration:
 * read the first sector, change one 80-byte block inside it, erase the
 * sector, program the whole 64 KiB back, and read it to check. The sector
 * data here is generated, and the block is an arbitrary distinct run of
 * bytes.
 *
 * What matters is the reach of the operation. One erase, at the first
 * sector; one program cycle per word, all inside it; and the rest of the
 * part, including the two offsets the firmware keeps its logs at, untouched.
 */
static void test_first_sector_rewrite(void)
{
    g_autofree char *image = flash_image("rewrite");
    g_autofree char *log = log_file("rewrite");
    g_autofree char *found = NULL;
    g_autofree char *extra = NULL;
    g_autofree uint8_t *seed = g_malloc(SECTOR_SIZE);
    g_autofree uint8_t *expected = g_malloc(SECTOR_SIZE);
    g_autofree uint8_t *sector = g_malloc(SECTOR_SIZE);
    uint8_t block[0x50];
    QTestState *s;

    for (unsigned i = 0; i < SECTOR_SIZE; i++) {
        seed[i] = (i * 7 + 0x31) & 0xff;
    }
    for (unsigned i = 0; i < sizeof(block); i++) {
        block[i] = 0xa0 + i;
    }
    memcpy(expected, seed, SECTOR_SIZE);
    memcpy(expected + 0x60, block, sizeof(block));

    image_write(image, 0, seed, SECTOR_SIZE);
    image_write_be64(image, SECTOR_SIZE, 0x1111111111111111ULL);
    image_write_be64(image, 0x1e0000, 0xfeedfacecafef00dULL);
    image_write_be64(image, 0x1f0000, 0x0badc0debaadf00dULL);
    image_write_be64(image, FLASH_SIZE - 8, 0x0123456789abcdefULL);

    extra = g_strdup_printf("-d trace:pflash_sector_erase_start,"
                            "trace:pflash_data_write -D %s", log);
    s = flash_start(image, "", extra);

    qtest_memread(s, PRIMARY, sector, SECTOR_SIZE);
    g_assert_cmpmem(sector, SECTOR_SIZE, seed, SECTOR_SIZE);
    memcpy(sector + 0x60, block, sizeof(block));

    flash_erase_sector(s, LOCAL, 0);
    for (unsigned i = 0; i < SECTOR_SIZE; i += 2) {
        flash_program(s, LOCAL, i, (sector[i] << 8) | sector[i + 1]);
    }

    qtest_memread(s, PRIMARY, sector, SECTOR_SIZE);
    g_assert_cmpmem(sector, SECTOR_SIZE, expected, SECTOR_SIZE);
    g_assert_cmphex(qtest_readq(s, PRIMARY + SECTOR_SIZE), ==,
                    0x1111111111111111ULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x1e0000), ==,
                    0xfeedfacecafef00dULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + 0x1f0000), ==,
                    0x0badc0debaadf00dULL);
    g_assert_cmphex(qtest_readq(s, PRIMARY + FLASH_SIZE - 8), ==,
                    0x0123456789abcdefULL);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_cmpuint(count_lines(found, "start sector erase"), ==, 1);
    g_assert_nonnull(strstr(found, "start sector erase at: 0x0000-0xffff"));
    assert_writes_stay_in_first_sector(found);

    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(unlink(image), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-flash/backing-file", test_backing_file);
    qtest_add_func("sn1/bedrock-flash/one-to-one-reads",
                   test_one_to_one_reads);
    qtest_add_func("sn1/bedrock-flash/selections-alias", test_selections_alias);
    qtest_add_func("sn1/bedrock-flash/query-mode", test_query_mode);
    qtest_add_func("sn1/bedrock-flash/program", test_program);
    qtest_add_func("sn1/bedrock-flash/sector-erase", test_sector_erase);
    qtest_add_func("sn1/bedrock-flash/local-aperture",
                   test_local_aperture);
    qtest_add_func("sn1/bedrock-flash/remote-aperture",
                   test_remote_aperture);
    qtest_add_func("sn1/bedrock-flash/access-type-split",
                   test_access_type_split);
    qtest_add_func("sn1/bedrock-flash/cfi-cycle-width", test_cfi_cycle_width);
    qtest_add_func("sn1/bedrock-flash/narrow-writes", test_narrow_writes);
    qtest_add_func("sn1/bedrock-flash/reset-states", test_reset_states);
    qtest_add_func("sn1/bedrock-flash/read-only", test_read_only);
    qtest_add_func("sn1/bedrock-flash/migration-stored-bytes",
                   test_migration_stored_bytes);
    qtest_add_func("sn1/bedrock-flash/migration-query-mode",
                   test_migration_query_mode);
    qtest_add_func("sn1/bedrock-flash/migration-partial-command",
                   test_migration_partial_command);
    qtest_add_func("sn1/bedrock-flash/first-sector-rewrite",
                   test_first_sector_rewrite);
    return g_test_run();
}
