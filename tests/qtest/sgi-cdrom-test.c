/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Every SGI machine that can carry a CD-ROM gives scsi-cd the SCSI-2 block
 * length behaviour of hw/mips/sgi/board-cdrom.c. Each machine class lists
 * the properties, and a CD made with -cdrom or with -device scsi-cd has
 * them, including one that sets its own power-on length. What the drive
 * then does is driven through each host adapter in that adapter's own
 * test.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qbool.h"
#include "qobject/qnum.h"
#include "sgi-test.h"

typedef struct CdProperty {
    const char *name;
    const char *value;
} CdProperty;

static const CdProperty props[] = {
    { "quirk_mode_sense_rom_use_dbd", "on" },
    { "quirk_mode_page_set_block_size", "on" },
    { "quirk_mode_page_vendor_specific_apple", "on" },
};

/* Every product class, including those a bare launch here cannot start. */
static const char *const products[] = {
    "octane", "octane2", "o2",
    "origin200", "origin2000", "onyx2",
    "origin300", "origin3000", "origin350", "onyx4", "onyx350",
    "tezro", "fuel",
};

static char *medium_path;
static char *flash_path;
static char *prom_path;

static void test_machine_classes(void)
{
    QTestState *qts = qtest_init("-M none");
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute': 'query-machines', 'arguments': {'compat-props': true}}");
    QList *machines = qdict_get_qlist(reply, "return");

    for (size_t i = 0; i < G_N_ELEMENTS(products); i++) {
        const QListEntry *e;
        QList *compat = NULL;

        if (!qtest_has_machine(products[i])) {
            continue;
        }
        QLIST_FOREACH_ENTRY(machines, e) {
            QDict *m = qobject_to(QDict, qlist_entry_obj(e));

            if (g_str_equal(qdict_get_str(m, "name"), products[i])) {
                compat = qdict_get_qlist(m, "compat-props");
            }
        }
        g_assert_nonnull(compat);
        for (size_t q = 0; q < G_N_ELEMENTS(props); q++) {
            bool found = false;

            QLIST_FOREACH_ENTRY(compat, e) {
                QDict *p = qobject_to(QDict, qlist_entry_obj(e));

                found |= g_str_equal(qdict_get_str(p, "qom-type"),
                                     "scsi-cd") &&
                         g_str_equal(qdict_get_str(p, "property"),
                                     props[q].name) &&
                         g_str_equal(qdict_get_str(p, "value"),
                                     props[q].value);
            }
            if (!found) {
                g_test_message("%s lacks scsi-cd.%s=%s", products[i],
                               props[q].name, props[q].value);
            }
            g_assert_true(found);
        }
    }
    qtest_quit(qts);
}

typedef struct CdLaunch {
    const char *machine;
    const char *options;    /* -M after the machine name */
    /* The rest of the command line; %s is the boot PROM image, if any. */
    const char *args;
    char **prom;
    /* What -device scsi-cd needs beyond the drive: bus and target. */
    const char *device;
    /* -cdrom reaches a bus on this machine. */
    bool cdrom;
} CdLaunch;

static const CdLaunch launches[] = {
    { "octane", "", "-m 128M -cpu R10000 -bios %s", &flash_path,
      "bus=scsi.1,scsi-id=4", true },
    { "octane2", "", "-bios %s", &flash_path, "bus=scsi.1,scsi-id=4", true },
    /* The O2 builds no bus -cdrom reaches. */
    { "o2", ",register-test-profile=on", "-m 64M -cpu R10000", NULL,
      "bus=aic0.0,scsi-id=4", false },
    { "origin200", "", "", NULL, "bus=scsi.0,scsi-id=4", true },
    { "origin2000", "", "", NULL, "bus=scsi.0,scsi-id=4", true },
    { "onyx2", "", "", NULL, "bus=scsi.0,scsi-id=4", true },
    /* These two refuse -cdrom: no adapter takes the legacy drive. */
    { "origin300", "", "-bios %s", &prom_path, "bus=scsi.0,scsi-id=4",
      false },
    { "origin3000", "", "-bios %s", &prom_path,
      "bus=scsi.0,scsi-id=4,port_wwn=0x2100000000000004,"
      "wwn=0x2000000000000004", false },
    { "fuel", "", "-bios %s", &prom_path, "bus=scsi.0,scsi-id=4", true },
};

/* The QOM path of the one block device attached to a device. */
static char *cd_path(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute': 'query-block'}");
    const QListEntry *e;
    char *path = NULL;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), e) {
        QDict *b = qobject_to(QDict, qlist_entry_obj(e));

        if (qdict_haskey(b, "qdev")) {
            g_assert_null(path);
            path = g_strdup(qdict_get_str(b, "qdev"));
        }
    }
    g_assert_nonnull(path);
    return path;
}

/* A CD's property, as qom-get returns it, in the form compat-props uses. */
static char *cd_property(QTestState *qts, const char *path, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute': 'qom-get', "
                                       "'arguments': {'path': %s, "
                                       "'property': %s}}", path, name);
    QObject *value = qdict_get(reply, "return");

    g_assert_nonnull(value);
    if (qobject_type(value) == QTYPE_QBOOL) {
        return g_strdup(qbool_get_bool(qobject_to(QBool, value)) ?
                        "on" : "off");
    }
    return g_strdup_printf("%" PRId64, qnum_get_int(qobject_to(QNum, value)));
}

/*
 * Start @l with one CD, by -cdrom or by -device scsi-cd with @extra
 * appended, and return the state and the CD's QOM path.
 */
static QTestState *start_cd(const CdLaunch *l, bool cdrom, const char *extra,
                            char **path)
{
    g_autofree char *args = NULL;
    g_autofree char *rest = NULL;
    g_autofree char *drive = NULL;
    g_autoptr(QDict) type = NULL;
    QTestState *qts;

    rest = g_strdup_printf(l->args, l->prom ? *l->prom : "");
    drive = cdrom ?
        g_strdup_printf("-cdrom %s", medium_path) :
        g_strdup_printf("-drive if=none,id=cd,file=%s,format=raw,"
                        "readonly=on -device scsi-cd,drive=cd,%s%s",
                        medium_path, l->device, extra);
    args = g_strdup_printf("-M %s%s %s -nodefaults -S %s", l->machine,
                           l->options, rest, drive);
    qts = qtest_init(args);
    *path = cd_path(qts);
    type = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                     "{'path': %s, 'property': 'type'}}", *path);
    g_assert_cmpstr(qdict_get_str(type, "return"), ==, "scsi-cd");
    return qts;
}

static void check_launch(const CdLaunch *l, bool cdrom)
{
    g_autofree char *path = NULL;
    QTestState *qts = start_cd(l, cdrom, "", &path);

    for (size_t q = 0; q < G_N_ELEMENTS(props); q++) {
        g_autofree char *value = cd_property(qts, path, props[q].name);

        if (!g_str_equal(value, props[q].value)) {
            g_test_message("%s %s: %s is %s, not %s", l->machine,
                           cdrom ? "-cdrom" : "-device scsi-cd",
                           props[q].name, value, props[q].value);
            g_assert_not_reached();
        }
    }
    qtest_quit(qts);
}

/*
 * A scsi-cd given its own power-on length keeps it, like the 512-byte
 * drives SGI sold in the Indy era, and keeps the SCSI-2 behaviour.
 */
static void test_own_length(const void *data)
{
    const CdLaunch *l = data;
    g_autofree char *path = NULL;
    g_autofree char *length = NULL;
    g_autofree char *select = NULL;
    QTestState *qts = start_cd(l, false, ",physical_block_size=512", &path);

    length = cd_property(qts, path, "physical_block_size");
    g_assert_cmpstr(length, ==, "512");
    select = cd_property(qts, path, "quirk_mode_page_set_block_size");
    g_assert_cmpstr(select, ==, "on");
    qtest_quit(qts);
}

static void test_device(const void *data)
{
    check_launch(data, false);
}

static void test_cdrom(const void *data)
{
    check_launch(data, true);
}

static char *zero_file(const char *name, size_t size)
{
    g_autofree uint8_t *zero = g_malloc0(size);
    char *path = NULL;
    int fd = g_file_open_tmp(name, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, zero, size), ==, size);
    close(fd);
    return path;
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    medium_path = zero_file("sgi-cdrom-XXXXXX.iso", 32 * 1024);
    /* What octane-test.c boots: no PROM code is needed to read QOM. */
    flash_path = zero_file("sgi-cdrom-flash-XXXXXX", 4);
    prom_path = zero_file("sgi-cdrom-prom-XXXXXX", PROM_SIZE);

    qtest_add_func("/sgi/cdrom/machine-classes", test_machine_classes);
    for (size_t i = 0; i < G_N_ELEMENTS(launches); i++) {
        const CdLaunch *l = &launches[i];
        g_autofree char *device = NULL;
        g_autofree char *cdrom = NULL;
        g_autofree char *own = NULL;

        if (!qtest_has_machine(l->machine)) {
            continue;
        }
        device = g_strdup_printf("/sgi/cdrom/%s/device", l->machine);
        qtest_add_data_func(device, l, test_device);
        own = g_strdup_printf("/sgi/cdrom/%s/own-length", l->machine);
        qtest_add_data_func(own, l, test_own_length);
        if (l->cdrom) {
            cdrom = g_strdup_printf("/sgi/cdrom/%s/cdrom", l->machine);
            qtest_add_data_func(cdrom, l, test_cdrom);
        }
    }
    ret = g_test_run();
    unlink(medium_path);
    unlink(flash_path);
    unlink(prom_path);
    g_free(medium_path);
    g_free(flash_path);
    g_free(prom_path);
    return ret;
}
