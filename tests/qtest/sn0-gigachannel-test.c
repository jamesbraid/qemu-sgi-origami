/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register-contract tests for the Origin 200 GIGAchannel expansion cabinet
 * as the origin200-gigachannel topology builds it, and for the converted
 * module in loopback (origin200-gigachannel-loopback), from
 * docs/datasheets/sgi-gigachannel.md in the Origami repository.
 *
 * The IP27 PROM finds the cabinet by reading widget 0's identity through
 * the node's Hub: the Crossbow's part number there marks the node, a
 * Bridge's does not. With the cabinet the IP29 Bridge sits on Crossbow link
 * 9 and the cabinet's own Bridge, revision 4 with four PCI openings, on
 * link 8; the Hub is on link A, and links B to F are empty XIO positions.
 * In loopback there is no Crossbow and the IP29 Bridge answers the plain
 * direct path, widget 0 included. The cabinet Bridge's external-flash port
 * carries the cabinet's system-controller status word on its second chip
 * select, and on its base chip select the BASEIO PROM flash part when a
 * drive named sgi-gigachannel-flash0 supplies its image; with none, nothing.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define XTALK_BASE UINT64_C(0x20000000000)
#define XTALK_REG_DATA 4
#define XTALK_WIDGET_SIZE UINT64_C(0x1000000)
#define WIDGET_ID 0x0000
#define XTALK_ID_PART(v) (((v) >> 12) & 0xffff)
#define XTALK_ID_REVISION(v) (((v) >> 28) & 0xf)
#define CROSSBOW_PART 0x0000
#define BRIDGE_PART 0xc002

#define XBOW_LINK_STATUS(w) (0x0110 + ((w) - 8) * 0x40)
#define XBOW_LINK_AUX(w) (0x0138 + ((w) - 8) * 0x40)
#define XBOW_AUX_FAILURE_MODE (1u << 6)
#define XBOW_AUX_PRESENT (1u << 5)
#define XBOW_LINK_ALIVE (1u << 31)
#define XBOW_LINK_MAX_RETRY (1u << 5)

#define CABINET_WIDGET 8
#define IP29_WIDGET 9
#define HUB_WIDGET 10
#define CABINET_LINKS ((1u << CABINET_WIDGET) | (1u << IP29_WIDGET) | \
                       (1u << HUB_WIDGET))

#define BRIDGE_CFG0(w, dev) ((uint64_t)(w) * XTALK_WIDGET_SIZE + 0x020000 + \
                             (dev) * 0x1000)
#define IOC3_ID_PAIR 0x000310a9u
#define IP29_IOC3_DEVICE 2
#define CABINET_BUS "pci.gigachannel1"

/*
 * The cabinet Bridge's external-flash window: the BASEIO PROM part, when
 * one is fitted, on the base chip select, the cabinet's system-controller
 * status word on the other.
 */
#define FLASH_WINDOW 0xc00000
#define FLASH_SELECT_SIZE 0x200000
#define STATUS_WORD (FLASH_WINDOW + FLASH_SELECT_SIZE)
#define STATUS_RPS_FITTED 0x0040
#define BRIDGE_CONTROL 0x0020
#define BRIDGE_CONTROL_FLASH_WRITE_EN (1u << 31)

/*
 * The fitted part: 1 MB, answering the AMD autoselect sequence the IP27
 * PROM's loader writes as 16-bit stores at word addresses 0x5555 and
 * 0x2aaa with manufacturer 0x01 and device 0x22da.
 */
#define FLASH_PART_SIZE 0x100000
#define FLASH_UNLOCK0 (2 * 0x5555)
#define FLASH_UNLOCK1 (2 * 0x2aaa)
#define FLASH_ID_MANUFACTURER 0x0001
#define FLASH_ID_DEVICE 0x22da

#define GIGACHANNEL "-M origin200,topology=origin200-gigachannel"
#define LOOPBACK "-M origin200,topology=origin200-gigachannel-loopback"
#define PLAIN "-M origin200"

/* A 32-bit crosstalk read through CPU 0's own Hub. */
static uint32_t xtalk_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/* A widget register, which answers in the low word of its doubleword. */
static uint32_t widget_read(QTestState *qts, uint64_t offset)
{
    return xtalk_read(qts, offset + XTALK_REG_DATA);
}

static uint32_t widget_id(QTestState *qts, unsigned int widget)
{
    return widget_read(qts, widget * XTALK_WIDGET_SIZE + WIDGET_ID);
}

static QTestState *start(const char *machine, const char *extra)
{
    return qtest_initf("%s -display none -d unimp,guest_errors %s",
                       machine, extra);
}

/*
 * The whole of the PROM's cabinet detection: widget 0 answers the
 * Crossbow's part with the cabinet attached, and a Bridge's part on the
 * plain machine and in loopback.
 */
static void test_widget0_marks_the_cabinet(void)
{
    QTestState *qts = start(GIGACHANNEL, "");

    g_assert_cmphex(XTALK_ID_PART(widget_id(qts, 0)), ==, CROSSBOW_PART);
    qtest_quit(qts);

    qts = start(PLAIN, "");
    g_assert_cmphex(XTALK_ID_PART(widget_id(qts, 0)), ==, BRIDGE_PART);
    qtest_quit(qts);

    qts = start(LOOPBACK, "");
    g_assert_cmphex(XTALK_ID_PART(widget_id(qts, 0)), ==, BRIDGE_PART);
    qtest_quit(qts);
}

/*
 * Links 8, 9 and A carry a widget from reset, so the PROM's link walk
 * finds them; every other port reads presence clear and the failed
 * initialization state an empty XIO slot shows (docs/datasheets/xbow.md,
 * link auxiliary status). One of links B to F is the spare widget a
 * `-device` XIO card claims, and it stays empty until one does.
 */
static void test_crossbow_links(void)
{
    QTestState *qts = start(GIGACHANNEL, "");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': '/machine/crossbow0',"
        "'property': 'startup-links-alive'}}");
    unsigned int widget;

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmphex(qdict_get_int(response, "return"), ==, CABINET_LINKS);
    for (widget = 8; widget < 16; widget++) {
        uint32_t aux = widget_read(qts, XBOW_LINK_AUX(widget));
        uint32_t status = widget_read(qts, XBOW_LINK_STATUS(widget));

        if (CABINET_LINKS & (1u << widget)) {
            g_assert_cmphex(aux & XBOW_AUX_PRESENT, ==, XBOW_AUX_PRESENT);
            g_assert_cmphex(status & XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
        } else {
            g_assert_cmphex(aux, ==, XBOW_AUX_FAILURE_MODE);
            g_assert_cmphex(status, ==, XBOW_LINK_MAX_RETRY);
        }
    }
    qtest_quit(qts);
}

/*
 * Widget 9 is the module's own IP29 Bridge, its IOC3 at PCI device 2;
 * widget 8 is the cabinet's Bridge, revision 4, with nothing fitted.
 */
static void test_bridges_at_their_links(void)
{
    QTestState *qts = start(GIGACHANNEL, "");
    uint32_t ip29 = widget_id(qts, IP29_WIDGET);
    uint32_t cabinet = widget_id(qts, CABINET_WIDGET);

    g_assert_cmphex(XTALK_ID_PART(ip29), ==, BRIDGE_PART);
    g_assert_cmphex(XTALK_ID_PART(cabinet), ==, BRIDGE_PART);
    g_assert_cmpuint(XTALK_ID_REVISION(cabinet), ==, 4);
    g_assert_cmphex(xtalk_read(qts, BRIDGE_CFG0(IP29_WIDGET,
                                                IP29_IOC3_DEVICE)), ==,
                    IOC3_ID_PAIR);
    qtest_quit(qts);
}

/* A 16-bit crosstalk read through CPU 0's own Hub. */
static uint16_t xtalk_read16(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint16_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE + offset,
                                    bytes, sizeof(bytes)));
    return lduw_be_p(bytes);
}

/* A 16-bit crosstalk write through CPU 0's own Hub. */
static void xtalk_write16(QTestState *qts, uint64_t offset, uint16_t value)
{
    uint8_t bytes[sizeof(uint16_t)];

    stw_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, XTALK_BASE + offset,
                                     bytes, sizeof(bytes)));
}

/* The QEMU log a run wrote to its own -D file, once the run has quit. */
typedef struct LoggedRun {
    char *dir;
    char *log;
} LoggedRun;

static QTestState *start_logged(LoggedRun *run, const char *extra)
{
    g_autofree char *args = NULL;

    run->dir = g_dir_make_tmp("sn0-gigachannel-XXXXXX", NULL);
    g_assert_nonnull(run->dir);
    run->log = g_build_filename(run->dir, "qemu.log", NULL);
    args = g_strdup_printf("-D %s %s", run->log, extra);
    return start(GIGACHANNEL, args);
}

static char *finish_logged(LoggedRun *run)
{
    char *contents = NULL;

    g_assert_true(g_file_get_contents(run->log, &contents, NULL, NULL));
    unlink(run->log);
    rmdir(run->dir);
    g_free(run->log);
    g_free(run->dir);
    return contents;
}

#define NO_PART_AT_STATUS "sgi-bridge: flash read at window offset " \
                          "0x200000 with no part there"

/*
 * The cabinet's status word answers on the cabinet Bridge's second flash
 * chip select, where the BASEIO PROM and the IRIX kernel read it: zero by
 * default, a cabinet with no redundant supply and no event, or whatever
 * "system-controller-status" says. A qtest read of an address nothing
 * answers also returns zero, so the Bridge's own log shows the read was
 * answered rather than refused.
 */
static void test_status_word(void)
{
    uint64_t status = CABINET_WIDGET * XTALK_WIDGET_SIZE + STATUS_WORD;
    LoggedRun run;
    g_autofree char *log = NULL;
    QTestState *qts = start_logged(&run, "");

    g_assert_cmphex(xtalk_read16(qts, status), ==, 0);
    qtest_quit(qts);
    log = finish_logged(&run);
    g_assert_null(strstr(log, NO_PART_AT_STATUS));

    qts = start(GIGACHANNEL, "-global sgi-gigachannel."
                "system-controller-status=0x40");
    g_assert_cmphex(xtalk_read16(qts, status), ==, STATUS_RPS_FITTED);
    qtest_quit(qts);
}

/*
 * No source describes a write to the status word: one is logged and
 * dropped, and the word keeps its value.
 */
static void test_status_word_write_dropped(void)
{
    uint64_t status = CABINET_WIDGET * XTALK_WIDGET_SIZE + STATUS_WORD;
    uint64_t control = CABINET_WIDGET * XTALK_WIDGET_SIZE + BRIDGE_CONTROL +
                       XTALK_REG_DATA;
    LoggedRun run;
    g_autofree char *log = NULL;
    uint8_t bytes[4];
    QTestState *qts = start_logged(&run, "-global sgi-gigachannel."
                                   "system-controller-status=0x40");

    /* With the flash write enable set, so the Bridge passes it on. */
    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE + control,
                                    bytes, sizeof(bytes)));
    stl_be_p(bytes, ldl_be_p(bytes) | BRIDGE_CONTROL_FLASH_WRITE_EN);
    g_assert_true(qtest_memwrite_cpu(qts, 0, XTALK_BASE + control,
                                     bytes, sizeof(bytes)));
    xtalk_write16(qts, status, 0x1234);
    g_assert_cmphex(xtalk_read16(qts, status), ==, STATUS_RPS_FITTED);
    qtest_quit(qts);
    log = finish_logged(&run);
    g_assert_nonnull(strstr(log, "sgi-gigachannel: write of 0x1234 to the "
                            "system-controller status word"));
}

/*
 * Without a drive no flash part is fitted on the base chip select, where
 * the IP27 PROM looks for its BASEIO PROM, and the IP29 Bridge now on link
 * 9 carries no status word: both fail the way an unpopulated widget
 * address does, which a qtest read shows only as the Bridge's own log line.
 */
static void test_flash_selects_elsewhere_empty(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-gigachannel-XXXXXX", NULL);
    g_autofree char *log = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *extra = g_strdup_printf("-D %s", log);
    g_autofree char *contents = NULL;
    QTestState *qts;

    qts = start(GIGACHANNEL, extra);
    xtalk_read(qts, CABINET_WIDGET * XTALK_WIDGET_SIZE + FLASH_WINDOW);
    xtalk_read(qts, IP29_WIDGET * XTALK_WIDGET_SIZE + STATUS_WORD);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "sgi-bridge: flash read at window "
                            "offset 0x0 with no part there"));
    g_assert_nonnull(strstr(contents, "sgi-bridge: flash read at window "
                            "offset 0x200000 with no part there"));
    unlink(log);
    rmdir(dir);
}

/* Every image byte a function of its offset, so each lane is distinct. */
static uint8_t flash_pattern(uint32_t offset)
{
    return (uint8_t)(offset * 7 + (offset >> 8) * 13 + 0x5a);
}

static uint32_t flash_pattern_word(uint32_t offset)
{
    return (uint32_t)flash_pattern(offset) << 24 |
           (uint32_t)flash_pattern(offset + 1) << 16 |
           (uint32_t)flash_pattern(offset + 2) << 8 |
           flash_pattern(offset + 3);
}

/*
 * With a drive named sgi-gigachannel-flash0 the cabinet Bridge's base chip
 * select answers the part the IP27 PROM's loader accepts, at the widget 8
 * window it reads on a GIGAchannel: the image's bytes, big-endian, then
 * with the flash write enable set, the autoselect identity. The status
 * word on the other chip select answers as before.
 */
static void test_flash_part_fitted(void)
{
    uint64_t window = CABINET_WIDGET * XTALK_WIDGET_SIZE + FLASH_WINDOW;
    uint64_t control = CABINET_WIDGET * XTALK_WIDGET_SIZE + BRIDGE_CONTROL +
                       XTALK_REG_DATA;
    g_autofree char *dir = g_dir_make_tmp("sn0-gigachannel-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "flash.raw", NULL);
    g_autofree uint8_t *bytes = g_malloc(FLASH_PART_SIZE);
    g_autofree char *extra = NULL;
    GError *error = NULL;
    uint8_t word[4];
    uint32_t offset;
    QTestState *qts;

    for (offset = 0; offset < FLASH_PART_SIZE; offset++) {
        bytes[offset] = flash_pattern(offset);
    }
    g_assert_true(g_file_set_contents(image, (const char *)bytes,
                                      FLASH_PART_SIZE, &error));
    g_assert_no_error(error);
    extra = g_strdup_printf("-drive if=none,id=sgi-gigachannel-flash0,"
                            "file=%s,format=raw -global sgi-gigachannel."
                            "system-controller-status=0x40", image);
    qts = start(GIGACHANNEL, extra);

    g_assert_cmphex(xtalk_read(qts, window), ==, flash_pattern_word(0));
    g_assert_cmphex(xtalk_read(qts, window + 0x12344), ==,
                    flash_pattern_word(0x12344));
    g_assert_cmphex(xtalk_read(qts, window + FLASH_PART_SIZE - 4), ==,
                    flash_pattern_word(FLASH_PART_SIZE - 4));

    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE + control,
                                    word, sizeof(word)));
    stl_be_p(word, ldl_be_p(word) | BRIDGE_CONTROL_FLASH_WRITE_EN);
    g_assert_true(qtest_memwrite_cpu(qts, 0, XTALK_BASE + control,
                                     word, sizeof(word)));
    xtalk_write16(qts, window + FLASH_UNLOCK0, 0xaa);
    xtalk_write16(qts, window + FLASH_UNLOCK1, 0x55);
    xtalk_write16(qts, window + FLASH_UNLOCK0, 0x90);
    g_assert_cmphex(xtalk_read16(qts, window), ==, FLASH_ID_MANUFACTURER);
    g_assert_cmphex(xtalk_read16(qts, window + 2), ==, FLASH_ID_DEVICE);
    xtalk_write16(qts, window, 0xf0);
    g_assert_cmphex(xtalk_read(qts, window), ==, flash_pattern_word(0));

    g_assert_cmphex(xtalk_read16(qts, CABINET_WIDGET * XTALK_WIDGET_SIZE +
                                 STATUS_WORD), ==, STATUS_RPS_FITTED);
    qtest_quit(qts);

    unlink(image);
    rmdir(dir);
}

/* In loopback the IP29 Bridge is back at widget 8 on the direct path. */
static void test_loopback_takes_the_plain_path(void)
{
    QTestState *qts = start(LOOPBACK, "");
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': '/machine/crossbow0'}}");

    g_assert_true(qdict_haskey(response, "error"));
    g_assert_cmphex(xtalk_read(qts, BRIDGE_CFG0(CABINET_WIDGET,
                                                IP29_IOC3_DEVICE)), ==,
                    IOC3_ID_PAIR);
    qtest_quit(qts);
}

/* A card in each of the cabinet's four PCI openings enumerates at io7. */
static void test_card_enumerates_in_each_cabinet_slot(void)
{
    unsigned int device;

    for (device = 0; device < 4; device++) {
        g_autofree char *extra = g_strdup_printf(
            "-device sgi-ioc3,bus=" CABINET_BUS ",addr=%u", device);
        QTestState *qts = start(GIGACHANNEL, extra);

        g_assert_cmphex(xtalk_read(qts, BRIDGE_CFG0(CABINET_WIDGET, device)),
                        ==, IOC3_ID_PAIR);
        qtest_quit(qts);
    }
}

/* The cabinet has four PCI openings: a card at device 4 is refused. */
static void test_fifth_cabinet_slot_refused(void)
{
    const char *const args[] = {
        qtest_qemu_binary(NULL), "-M",
        "origin200,topology=origin200-gigachannel",
        "-display", "none", "-accel", "qtest", "-qmp", "stdio",
        "-device", "sgi-ioc3,bus=" CABINET_BUS ",addr=4", NULL,
    };

    assert_launch_rejected(args, "reserved");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sn0/gigachannel/widget0-marks-the-cabinet",
                   test_widget0_marks_the_cabinet);
    qtest_add_func("sn0/gigachannel/crossbow-links", test_crossbow_links);
    qtest_add_func("sn0/gigachannel/bridges-at-their-links",
                   test_bridges_at_their_links);
    qtest_add_func("sn0/gigachannel/status-word", test_status_word);
    qtest_add_func("sn0/gigachannel/status-word-write-dropped",
                   test_status_word_write_dropped);
    qtest_add_func("sn0/gigachannel/flash-selects-elsewhere-empty",
                   test_flash_selects_elsewhere_empty);
    qtest_add_func("sn0/gigachannel/flash-part-fitted",
                   test_flash_part_fitted);
    qtest_add_func("sn0/gigachannel/loopback-takes-the-plain-path",
                   test_loopback_takes_the_plain_path);
    qtest_add_func("sn0/gigachannel/card-enumerates-in-each-cabinet-slot",
                   test_card_enumerates_in_each_cabinet_slot);
    qtest_add_func("sn0/gigachannel/fifth-cabinet-slot-refused",
                   test_fifth_cabinet_slot_refused);
    return g_test_run();
}
