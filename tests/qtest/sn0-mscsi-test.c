/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register-contract tests for the SGI MSCSI XIO board: a Bridge and four
 * QLogic ISP1040 SCSI controllers (docs/datasheets/sgi-mscsi.md in the
 * Origami tree). The board is the "mscsi" preset of sgi-xio-baseio, built
 * from the existing Bridge and ISP1040 models.
 *
 * Two placements run the same checks. A `-device sgi-xio-baseio,
 * preset=mscsi` board attaches at an Origin 2000's empty XIO slot IO4,
 * crossbow0 widget 11, as the MENET's own test does. The
 * origin200-gigachannel-xio topology fits two boards in the GIGAchannel
 * cabinet, at io2 (Crossbow widget 11) and io4 (widget 13), with a MENET
 * at io3 (widget 12) between them: the XIO population of a captured
 * one-module system (docs/datasheets/sgi-gigachannel.md in the Origami
 * tree, "Crossbow link assignment").
 *
 * The contract, from a real IP27 PROM inventory of an MSCSI board and
 * real IRIX inventories:
 *   - the board's Bridge is revision 3;
 *   - ISP1040s (0x1077:0x1020), PCI revision 5, at PCI devices 0-3, and
 *     nothing else: no IOC3, so no console, record, clock or MediaIO
 *     backend.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include <sys/wait.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-test.h"

#define XTALK_REG_DATA      4
#define XTALK_WIDGET_SIZE   UINT64_C(0x1000000)
#define WIDGET_ID           0x0000
#define XTALK_ID_PART(v)    (((v) >> 12) & 0xffff)
#define XTALK_ID_REVISION(v) (((v) >> 28) & 0xf)
#define BRIDGE_PART         0xc002
#define MSCSI_BRIDGE_REVISION 3

#define MSCSI_ISPS          4
#define ISP1040_ID_PAIR     0x10201077u
#define ISP1040_REVISION    5
#define PCI_REVISION        0x08

/* IO4, the empty XIO slot a `-device` board takes: crossbow0 widget 11. */
#define DEVICE_WIDGET       11

typedef struct {
    const char *args;
    unsigned int widget;
    const char *board_path;
} MscsiCase;

#define DEVICE_ARGS \
    "-M origin2000 -display none -device sgi-xio-baseio,preset=mscsi," \
    "slot=io4"

static const MscsiCase device_case = {
    DEVICE_ARGS, DEVICE_WIDGET, "/machine/module0/slot11/baseio",
};

#define PLACED_ARGS \
    "-M origin200,topology=origin200-gigachannel-xio -display none"
/* The cabinet's io2 and io4 positions: Crossbow links B and D. */
#define IO2_WIDGET          11
#define IO3_WIDGET          12
#define IO4_WIDGET          13
/* The Hub (A), the IP29 Bridge (9) and the cabinet Bridge (8). */
#define CABINET_LINKS       0x0700

static const MscsiCase io2_case = {
    PLACED_ARGS, IO2_WIDGET, "/machine/module1/slot11/baseio",
};
static const MscsiCase io4_case = {
    PLACED_ARGS, IO4_WIDGET, "/machine/module1/slot13/baseio",
};

/* A 32-bit crosstalk read through CPU 0's own Hub. */
static uint32_t xtalk_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, SPACE_XTALK + offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static uint64_t widget_base(const MscsiCase *c)
{
    return (uint64_t)c->widget * XTALK_WIDGET_SIZE;
}

static uint32_t config_read(QTestState *qts, const MscsiCase *c,
                            unsigned int device, unsigned int offset)
{
    return xtalk_read(qts, widget_base(c) + 0x020000 + device * 0x1000 +
                           offset);
}

/* True when the QOM object at `path` has a child property named `child`. */
static bool qom_has_child(QTestState *qts, const char *path,
                          const char *child)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': %s}}", path);
    QList *list;
    QListEntry *entry;

    g_assert_false(qdict_haskey(response, "error"));
    list = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(prop, "name"), child)) {
            return true;
        }
    }
    return false;
}

/* The board's own Bridge answers at its widget, revision 3. */
static void test_bridge_revision(const void *data)
{
    const MscsiCase *c = data;
    QTestState *qts = qtest_init(c->args);
    uint32_t id = xtalk_read(qts, widget_base(c) + WIDGET_ID +
                                  XTALK_REG_DATA);

    g_assert_cmphex(XTALK_ID_PART(id), ==, BRIDGE_PART);
    g_assert_cmpuint(XTALK_ID_REVISION(id), ==, MSCSI_BRIDGE_REVISION);
    qtest_quit(qts);
}

/* Four ISP1040s at devices 0-3, PCI revision 5. */
static void test_isps_enumerate(const void *data)
{
    const MscsiCase *c = data;
    QTestState *qts = qtest_init(c->args);
    unsigned int d;

    for (d = 0; d < MSCSI_ISPS; d++) {
        g_assert_cmphex(config_read(qts, c, d, 0), ==, ISP1040_ID_PAIR);
        g_assert_cmphex(config_read(qts, c, d, PCI_REVISION) & 0xff, ==,
                        ISP1040_REVISION);
    }
    qtest_quit(qts);
}

/* The board builds the four controllers and nothing else. */
static void test_population(const void *data)
{
    const MscsiCase *c = data;
    QTestState *qts = qtest_init(c->args);
    unsigned int d;

    for (d = 0; d < MSCSI_ISPS; d++) {
        g_autofree char *child = g_strdup_printf("scsi%u", d);

        g_assert_true(qom_has_child(qts, c->board_path, child));
    }
    g_assert_false(qom_has_child(qts, c->board_path, "scsi4"));
    g_assert_false(qom_has_child(qts, c->board_path, "ioc3"));
    qtest_quit(qts);
}

/*
 * Run QEMU with args and require it to refuse them with message: an error
 * exit, not a signal. Each caller passes -accel qtest, which stands in for
 * the PROM the machine otherwise demands before it builds any board.
 */
static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output, message));
}

/* An MSCSI in IO1 would replace the module's BaseIO. */
static void test_mscsi_is_not_a_baseio_preset(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000,baseio-preset=mscsi",
        "-accel", "qtest", "-display", "none", NULL
    };

    assert_qemu_rejects(args, "baseio-preset 'mscsi' is not a BaseIO card");
}

/*
 * The board has no IOC3, so an id for a console, record, MediaIO port or
 * clock is refused rather than dropped.
 */
static void test_mscsi_refuses_backends_it_cannot_take(const void *data)
{
    static const char *const props[] = {
        "chardev-a", "chardev-b", "record-a", "record-b",
        "mio-chardev-a", "mio-chardev-b",
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(props); i++) {
        g_autofree char *device = g_strdup_printf(
            "sgi-xio-baseio,preset=mscsi,slot=io4,"
            "%s=spare", props[i]);
        g_autofree char *message = g_strdup_printf(
            "preset 'mscsi' fits nothing to take %s 'spare'", props[i]);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", "origin2000", "-accel", "qtest",
            "-display", "none",
            "-chardev", "null,id=spare", "-device", device, NULL
        };

        assert_qemu_rejects(args, message);
    }
    {
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", "origin2000", "-accel", "qtest",
            "-display", "none",
            "-drive", "if=none,id=spare,driver=null-co",
            "-device", "sgi-xio-baseio,preset=mscsi,slot=io4,"
            "nvram-drive=spare", NULL
        };

        assert_qemu_rejects(args,
                            "preset 'mscsi' fits nothing to take nvram-drive");
    }
}

/*
 * The populated cabinet's three boards are alive from reset, and io5 and
 * io6 (links E and F) stay empty.
 */
static void test_placed_widgets_are_alive_at_startup(const void *data)
{
    QTestState *qts = qtest_init(PLACED_ARGS);
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': '/machine/crossbow0',"
        "'property': 'startup-links-alive'}}");

    g_assert_false(qdict_haskey(response, "error"));
    g_assert_cmphex(qdict_get_int(response, "return"), ==,
                    CABINET_LINKS | (1u << IO2_WIDGET) | (1u << IO3_WIDGET) |
                    (1u << IO4_WIDGET));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/mscsi/bridge-revision", &device_case,
                        test_bridge_revision);
    qtest_add_data_func("sn0/mscsi/isps-enumerate", &device_case,
                        test_isps_enumerate);
    qtest_add_data_func("sn0/mscsi/population", &device_case,
                        test_population);
    qtest_add_data_func("sn0/mscsi/not-a-baseio-preset", NULL,
                        test_mscsi_is_not_a_baseio_preset);
    qtest_add_data_func("sn0/mscsi/refuses-backends-it-cannot-take", NULL,
                        test_mscsi_refuses_backends_it_cannot_take);
    qtest_add_data_func("sn0/mscsi/placed/widgets-alive-at-startup", NULL,
                        test_placed_widgets_are_alive_at_startup);
    qtest_add_data_func("sn0/mscsi/placed/io2/bridge-revision", &io2_case,
                        test_bridge_revision);
    qtest_add_data_func("sn0/mscsi/placed/io2/isps-enumerate", &io2_case,
                        test_isps_enumerate);
    qtest_add_data_func("sn0/mscsi/placed/io2/population", &io2_case,
                        test_population);
    qtest_add_data_func("sn0/mscsi/placed/io4/bridge-revision", &io4_case,
                        test_bridge_revision);
    qtest_add_data_func("sn0/mscsi/placed/io4/isps-enumerate", &io4_case,
                        test_isps_enumerate);
    return g_test_run();
}
