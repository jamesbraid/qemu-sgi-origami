/*
 * Origin 2000 BaseIO flash behind the Bridge external-flash window
 * (hw/pci-host/sgi-bridge.c, sgi_bridge_flash_ops; hw/mips/sgi/xio-baseio.c).
 * The IP27 PROM loader enables flash writes, sends AMD autoselect with 16-bit
 * stores at word addresses 0x5555/0x2aaa, reads 16-bit IDs at offsets 0/2,
 * resets with 0xf0, then copies bytes. BaseIO uses node 0, widget 8, offset
 * 0xc0_0000 in its 16 MB window; Origin 200 has no fitted flash and fails
 * there. A bare Bridge fixture checks the optional second chip select 2 MB
 * into the window with a stand-in part.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define ORIGIN2000_MACHINE "-M origin2000 -smp 8 -display none"
#define ORIGIN200_MACHINE "-M origin200 -m 128M -display none"

/* Position-independent crosstalk space and the per-widget small window. */
#define IO_SPACE UINT64_C(0x20000000000)
#define XIO_WIDGET_SIZE UINT64_C(0x1000000)
#define BASEIO_WIDGET 8
#define BRIDGE_BASE (IO_SPACE + BASEIO_WIDGET * XIO_WIDGET_SIZE)

/* Registers are 32 bits in the low half of an eight-byte slot. */
#define REG_DATA 4
#define BRIDGE_CONTROL 0x020
#define CONTROL_FLASH_WRITE_EN (1u << 31)

/* The window: two 2 MB chip selects, the part on the first. */
#define FLASH_WINDOW 0xc00000
#define FLASH_SELECT_SIZE (2 * 1024 * 1024)
#define PART_SIZE (1024 * 1024)

/* The AMD command set as the 16-bit port sees it: byte offset = 2 * word. */
#define UNLOCK0_OFFSET (2 * 0x5555)
#define UNLOCK1_OFFSET (2 * 0x2aaa)
#define CMD_UNLOCK0 0xaa
#define CMD_UNLOCK1 0x55
#define CMD_AUTOSELECT 0x90
#define CMD_PROGRAM 0xa0
#define CMD_RESET 0xf0

/* What the BaseIO card fits: AMD 0x01 / 0x22da, the PROM's "type 2". */
#define ID_MANUFACTURER 0x0001
#define ID_DEVICE 0x22da

/* An erased image span gives a program cycle bits to clear. */
#define ERASED_OFFSET 0x80000
#define ERASED_LENGTH 0x100

static char *directory;
static char *image;
static char *logfile;

/*
 * A crosstalk read the window refuses is a bus error to a processor and
 * nothing to a qtest read, whose server drops the transaction result; the
 * refusal is observed through the model's own guest-error log instead.
 */
static void wait_for_log_marker(const char *marker)
{
    unsigned int i;

    for (i = 0; i < 100; i++) {
        g_autofree char *log = NULL;

        if (g_file_get_contents(logfile, &log, NULL, NULL) &&
            strstr(log, marker)) {
            return;
        }
        g_usleep(2000);
    }
    g_error("timed out waiting for QEMU log marker: %s", marker);
}

static void truncate_log(void)
{
    GError *error = NULL;

    g_assert_true(g_file_set_contents(logfile, "", 0, &error));
    g_assert_no_error(error);
}

/* Every byte of the image is a function of its offset: distinct per lane. */
static uint8_t pattern(uint32_t offset)
{
    return (uint8_t)(offset * 7 + (offset >> 8) * 13 + 0x5a);
}

static uint64_t pattern_word(uint32_t offset, unsigned size)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        value = (value << 8) | pattern(offset + i);
    }
    return value;
}

static void write_image(void)
{
    g_autofree uint8_t *bytes = g_malloc(PART_SIZE);
    GError *error = NULL;
    uint32_t offset;

    for (offset = 0; offset < PART_SIZE; offset++) {
        bytes[offset] = pattern(offset);
    }
    memset(bytes + ERASED_OFFSET, 0xff, ERASED_LENGTH);
    g_assert_true(g_file_set_contents(image, (const char *)bytes, PART_SIZE,
                                      &error));
    g_assert_no_error(error);
}

static QTestState *start_origin2000(bool backed)
{
    truncate_log();
    if (backed) {
        return qtest_initf(ORIGIN2000_MACHINE
                           " -drive if=none,id=sgi-baseio-flash0,file=%s,"
                           "format=raw -d guest_errors -D %s", image,
                           logfile);
    }
    return qtest_initf(ORIGIN2000_MACHINE " -d guest_errors -D %s", logfile);
}

static uint64_t flash_read(QTestState *qts, uint32_t offset, unsigned size)
{
    uint8_t bytes[8] = { 0 };

    g_assert_true(qtest_memread_cpu(qts, 0, BRIDGE_BASE + FLASH_WINDOW +
                                    offset, bytes + 8 - size, size));
    return ldq_be_p(bytes);
}

static void flash_write(QTestState *qts, uint32_t offset, uint64_t value,
                        unsigned size)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, BRIDGE_BASE + FLASH_WINDOW +
                                     offset, bytes + 8 - size, size));
}

static uint32_t control_read(QTestState *qts)
{
    uint8_t bytes[4];

    g_assert_true(qtest_memread_cpu(qts, 0, BRIDGE_BASE + BRIDGE_CONTROL +
                                    REG_DATA, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void control_write(QTestState *qts, uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, BRIDGE_BASE + BRIDGE_CONTROL +
                                     REG_DATA, bytes, sizeof(bytes)));
}

static void set_write_enable(QTestState *qts, bool enabled)
{
    uint32_t control = control_read(qts);

    control = enabled ? control | CONTROL_FLASH_WRITE_EN :
                        control & ~CONTROL_FLASH_WRITE_EN;
    control_write(qts, control);
    g_assert_cmphex(control_read(qts) & CONTROL_FLASH_WRITE_EN, ==,
                    enabled ? CONTROL_FLASH_WRITE_EN : 0);
}

/* The loader's own command sequence: three 16-bit stores. */
static void unlock(QTestState *qts, uint16_t command)
{
    flash_write(qts, UNLOCK0_OFFSET, CMD_UNLOCK0, 2);
    flash_write(qts, UNLOCK1_OFFSET, CMD_UNLOCK1, 2);
    flash_write(qts, UNLOCK0_OFFSET, command, 2);
}

/* Every read width returns the image's own bytes, big-endian, in order. */
static void test_read_every_width(void)
{
    QTestState *qts = start_origin2000(true);
    static const uint32_t offsets[] = { 0, 0x40, 0x12345, 0xffff8 };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_assert_cmphex(flash_read(qts, offsets[i], 1), ==,
                        pattern_word(offsets[i], 1));
        g_assert_cmphex(flash_read(qts, offsets[i] + 1, 1), ==,
                        pattern_word(offsets[i] + 1, 1));
        g_assert_cmphex(flash_read(qts, offsets[i], 2), ==,
                        pattern_word(offsets[i], 2));
        g_assert_cmphex(flash_read(qts, offsets[i], 4), ==,
                        pattern_word(offsets[i], 4));
        g_assert_cmphex(flash_read(qts, offsets[i], 8), ==,
                        pattern_word(offsets[i], 8));
    }
    qtest_quit(qts);
}

/*
 * The part decodes only its own address lines, so it repeats across the
 * 2 MB chip select; the other chip select has nothing on it.
 */
static void test_chip_select_decode(void)
{
    QTestState *qts = start_origin2000(true);

    g_assert_cmphex(flash_read(qts, PART_SIZE + 0x40, 8), ==,
                    pattern_word(0x40, 8));
    g_assert_cmphex(flash_read(qts, FLASH_SELECT_SIZE + 0x40, 8), ==, 0);
    wait_for_log_marker("sgi-bridge: flash read at window offset 0x200040 "
                        "with no part there");
    qtest_quit(qts);
}

/*
 * With write enable clear (its reset state), the command sequence
 * never reaches the part: reads keep returning the image. With it set,
 * autoselect answers the two identities at word addresses 0 and 1, and
 * the reset command returns the part to the image.
 */
static void test_write_enable_gates_autoselect(void)
{
    QTestState *qts = start_origin2000(true);

    g_assert_cmphex(control_read(qts) & CONTROL_FLASH_WRITE_EN, ==, 0);
    unlock(qts, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, pattern_word(0, 2));
    g_assert_cmphex(flash_read(qts, 2, 2), ==, pattern_word(2, 2));

    set_write_enable(qts, true);
    unlock(qts, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, ID_MANUFACTURER);
    g_assert_cmphex(flash_read(qts, 2, 2), ==, ID_DEVICE);
    /* The loader's own byte copies see the identity's lanes, not data. */
    g_assert_cmphex(flash_read(qts, 2, 1), ==, ID_DEVICE >> 8);
    g_assert_cmphex(flash_read(qts, 3, 1), ==, (ID_DEVICE & 0xff));

    flash_write(qts, 0, CMD_RESET, 2);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, pattern_word(0, 2));
    g_assert_cmphex(flash_read(qts, 2, 2), ==, pattern_word(2, 2));

    /* Clearing the enable again shuts the command path. */
    set_write_enable(qts, false);
    unlock(qts, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, pattern_word(0, 2));
    qtest_quit(qts);
}

/* The port takes 16-bit writes only: other widths never reach the part. */
static void test_write_width(void)
{
    QTestState *qts = start_origin2000(true);
    static const unsigned widths[] = { 1, 4, 8 };
    size_t i;

    set_write_enable(qts, true);
    for (i = 0; i < ARRAY_SIZE(widths); i++) {
        flash_write(qts, UNLOCK0_OFFSET, CMD_UNLOCK0, widths[i]);
        flash_write(qts, UNLOCK1_OFFSET, CMD_UNLOCK1, widths[i]);
        flash_write(qts, UNLOCK0_OFFSET, CMD_AUTOSELECT, widths[i]);
        g_assert_cmphex(flash_read(qts, 0, 2), ==, pattern_word(0, 2));
        g_assert_cmphex(flash_read(qts, 2, 2), ==, pattern_word(2, 2));
    }
    /* Not a stuck state machine: the 16-bit sequence still works after. */
    unlock(qts, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, ID_MANUFACTURER);
    flash_write(qts, 0, CMD_RESET, 2);
    qtest_quit(qts);
}

/* A program cycle lands one 16-bit word in the image, big-endian. */
static void test_program_word(void)
{
    QTestState *qts = start_origin2000(true);

    g_assert_cmphex(flash_read(qts, ERASED_OFFSET, 4), ==, 0xffffffffULL);
    set_write_enable(qts, true);
    unlock(qts, CMD_PROGRAM);
    flash_write(qts, ERASED_OFFSET, 0x1234, 2);
    g_assert_cmphex(flash_read(qts, ERASED_OFFSET, 2), ==, 0x1234);
    g_assert_cmphex(flash_read(qts, ERASED_OFFSET, 1), ==, 0x12);
    g_assert_cmphex(flash_read(qts, ERASED_OFFSET + 1, 1), ==, 0x34);
    g_assert_cmphex(flash_read(qts, ERASED_OFFSET, 4), ==, 0x1234ffffULL);
    /* The repeat above the part's own lines sees the same word. */
    g_assert_cmphex(flash_read(qts, PART_SIZE + ERASED_OFFSET, 2), ==,
                    0x1234);
    qtest_quit(qts);
}

/*
 * A card built without an image still fits the part: the loader's probe
 * identifies it and then reads a blank image. Zeros are the model's own
 * blank, stated in xio-baseio.c, not the erased chip's 0xff.
 */
static void test_unbacked_part(void)
{
    QTestState *qts = start_origin2000(false);

    g_assert_cmphex(flash_read(qts, 0, 8), ==, 0);
    g_assert_cmphex(flash_read(qts, 0x40, 8), ==, 0);
    set_write_enable(qts, true);
    unlock(qts, CMD_AUTOSELECT);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, ID_MANUFACTURER);
    g_assert_cmphex(flash_read(qts, 2, 2), ==, ID_DEVICE);
    flash_write(qts, 0, CMD_RESET, 2);
    g_assert_cmphex(flash_read(qts, 0, 2), ==, 0);
    qtest_quit(qts);
}

/*
 * A part on the second chip select: the fixture's Bridge mapped at a fixed
 * address, a 64 KiB part whose bytes hold their own offsets.
 */
#define FIXTURE_BRIDGE_BASE UINT64_C(0x10000000)
#define SELECT1_PART_SIZE 0x10000

static QTestState *start_fixture_select1(void)
{
    QTestState *qts;

    truncate_log();
    qts = qtest_initf("-M none -cpu R10000 -S -d guest_errors -D %s "
                      "-device sgi-board-test-fixture,id=wrap,"
                      "board-type=sgi-baseio,preset=ip29,"
                      "flash-select1-size=%u", logfile, SELECT1_PART_SIZE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/wrap/chip", 0,
                          FIXTURE_BRIDGE_BASE);
    return qts;
}

static uint64_t fixture_flash_read(QTestState *qts, uint32_t offset,
                                   unsigned size)
{
    uint8_t bytes[8] = { 0 };

    qtest_memread(qts, FIXTURE_BRIDGE_BASE + FLASH_WINDOW + offset,
                  bytes + 8 - size, size);
    return ldq_be_p(bytes);
}

/*
 * The second chip select answers with its own part, a read of any width
 * as 16-bit cycles, repeating above the part's own lines; the first chip
 * select, with nothing on it, fails.
 */
static void test_second_chip_select(void)
{
    QTestState *qts = start_fixture_select1();

    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE + 0x40, 2), ==,
                    0x4041);
    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE + 0x41, 1), ==,
                    0x41);
    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE + 0x10, 8), ==,
                    UINT64_C(0x1011121314151617));
    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE +
                                       SELECT1_PART_SIZE + 0x40, 2), ==,
                    0x4041);
    fixture_flash_read(qts, 0x40, 8);
    wait_for_log_marker("sgi-bridge: flash read at window offset 0x40 "
                        "with no part there");
    qtest_quit(qts);
}

/* Writes reach the second chip select's part only with the enable set. */
static void test_second_chip_select_write_enable(void)
{
    QTestState *qts = start_fixture_select1();
    uint64_t control = FIXTURE_BRIDGE_BASE + BRIDGE_CONTROL + REG_DATA;
    uint8_t word[2] = { 0xbe, 0xef };

    qtest_memwrite(qts, FIXTURE_BRIDGE_BASE + FLASH_WINDOW +
                   FLASH_SELECT_SIZE + 0x80, word, sizeof(word));
    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE + 0x80, 2), ==,
                    0x8081);
    qtest_writel(qts, control,
                 qtest_readl(qts, control) | CONTROL_FLASH_WRITE_EN);
    qtest_memwrite(qts, FIXTURE_BRIDGE_BASE + FLASH_WINDOW +
                   FLASH_SELECT_SIZE + 0x80, word, sizeof(word));
    g_assert_cmphex(fixture_flash_read(qts, FLASH_SELECT_SIZE + 0x80, 2), ==,
                    0xbeef);
    qtest_quit(qts);
}

/* The Origin 200 onboard Bridge fits no part: the window still fails. */
static void test_origin200_unpopulated(void)
{
    QTestState *qts;

    truncate_log();
    qts = qtest_initf(ORIGIN200_MACHINE " -d guest_errors -D %s", logfile);
    g_assert_cmphex(flash_read(qts, 0x40, 8), ==, 0);
    wait_for_log_marker("sgi-bridge: flash read at window offset 0x40 "
                        "with no part there");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sn0-bridge-flash-XXXXXX", NULL);
    g_assert_nonnull(directory);
    image = g_build_filename(directory, "baseio-flash.raw", NULL);
    logfile = g_build_filename(directory, "qemu.log", NULL);
    write_image();

    qtest_add_func("sn0/bridge/flash/read-every-width",
                   test_read_every_width);
    qtest_add_func("sn0/bridge/flash/chip-select-decode",
                   test_chip_select_decode);
    qtest_add_func("sn0/bridge/flash/write-enable-gates-autoselect",
                   test_write_enable_gates_autoselect);
    qtest_add_func("sn0/bridge/flash/write-width", test_write_width);
    qtest_add_func("sn0/bridge/flash/program-word", test_program_word);
    qtest_add_func("sn0/bridge/flash/unbacked-part", test_unbacked_part);
    qtest_add_func("sn0/bridge/flash/second-chip-select",
                   test_second_chip_select);
    qtest_add_func("sn0/bridge/flash/second-chip-select-write-enable",
                   test_second_chip_select_write_enable);
    qtest_add_func("sn0/bridge/flash/origin200-unpopulated",
                   test_origin200_unpopulated);
    ret = g_test_run();

    unlink(image);
    unlink(logfile);
    rmdir(directory);
    g_free(logfile);
    g_free(image);
    g_free(directory);
    return ret;
}
