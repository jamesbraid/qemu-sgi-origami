/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The graphics-head selection, -M <machine>,heads.<position>=<id>
 * (docs/specs/sgi-sn.rst, Graphics heads).
 *
 * The catalogue's own graphics records drive the device cases: every id a
 * topology offers is fitted at the first position offering it, and every
 * position is fitted once, so a catalogue change is tested as it lands.
 * Each refusal template is checked word for word, the dotted option is
 * checked beside -nographic (which sets the core machine property
 * graphics= the selection could otherwise collide with), and a command line
 * without heads must build exactly what it built before.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qjson.h"
#include "qobject/qstring.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

/* Where the Fuel's built-in V12 sits, named by its catalogue label. */
#define VPRO_FUEL_PATH "/machine/module0/graphics-slot"

static char *octane_flash;
static char *sn1_prom;
static QDict *catalogue;

/* How to launch each topology that has graphics positions. */
typedef struct Launch {
    const char *topology;
    const char *machine;
    const char *extra;
    enum { NO_BIOS, OCTANE_FLASH, SN1_PROM } bios;
} Launch;

static const Launch launches[] = {
    { "origin200", "origin200", "", NO_BIOS },
    { "origin200-dual", "origin200,topology=origin200-dual",
      "-smp 4 -m 128M", NO_BIOS },
    { "origin200-gigachannel", "origin200,topology=origin200-gigachannel",
      "", NO_BIOS },
    { "origin200-gigachannel-loopback",
      "origin200,topology=origin200-gigachannel-loopback", "", NO_BIOS },
    { "origin2000-deskside", "origin2000,topology=origin2000-deskside",
      "-smp 8", NO_BIOS },
    { "origin2000-rack", "origin2000", "-smp 8", NO_BIOS },
    { "onyx2-deskside", "onyx2,topology=onyx2-deskside",
      "-smp 4 -m 128M", NO_BIOS },
    { "onyx2-rack", "onyx2", "", NO_BIOS },
    { "octane", "octane", "-m 128M -cpu R10000", OCTANE_FLASH },
    { "octane2", "octane2", "-m 256M -cpu " OCTANE2_CPU, OCTANE_FLASH },
    { "fuel", "fuel,topology=fuel", "-cpu R14000 -smp 1 -m 512M",
      SN1_PROM },
    { "fuel-vpro", "fuel,topology=fuel-vpro", "-cpu R14000 -smp 1 -m 512M",
      SN1_PROM },
    { "origin300-v12-direct", "origin300,topology=origin300-v12-direct",
      "-cpu R14000 -smp 2", SN1_PROM },
    { "origin300-vbrick", "origin300,topology=origin300-vbrick",
      "-cpu R14000 -smp 2", SN1_PROM },
};

static const Launch *launch_for(const char *topology)
{
    for (size_t i = 0; i < G_N_ELEMENTS(launches); i++) {
        if (g_str_equal(launches[i].topology, topology)) {
            return &launches[i];
        }
    }
    g_error("no launch recipe for topology %s", topology);
}

static const char *bios_path(const Launch *l)
{
    return l->bios == OCTANE_FLASH ? octane_flash :
           l->bios == SN1_PROM ? sn1_prom : NULL;
}

/* Start @l's machine with @heads appended to its -M value. */
static QTestState *start(const Launch *l, const char *heads,
                         const char *more)
{
    const char *bios = bios_path(l);
    g_autofree char *quoted = bios ? g_shell_quote(bios) : NULL;

    return qtest_initf("-M %s%s -display none -S %s %s%s %s", l->machine,
                       heads, l->extra, bios ? "-bios " : "",
                       bios ? quoted : "", more);
}

/* The same launch as an argument vector, expecting a refusal. */
static void assert_refused(const Launch *l, const char *heads,
                           const char *message)
{
    const char *bios = bios_path(l);
    g_autofree char *machine = g_strdup_printf("%s%s", l->machine, heads);
    g_auto(GStrv) extra = g_strsplit(l->extra, " ", -1);
    g_autoptr(GPtrArray) args = g_ptr_array_new();

    g_ptr_array_add(args, (gpointer)qtest_qemu_binary(NULL));
    g_ptr_array_add(args, (gpointer)"-M");
    g_ptr_array_add(args, machine);
    for (char **word = extra; *word; word++) {
        if (**word) {
            g_ptr_array_add(args, *word);
        }
    }
    if (bios) {
        g_ptr_array_add(args, (gpointer)"-bios");
        g_ptr_array_add(args, (gpointer)bios);
    }
    g_ptr_array_add(args, (gpointer)"-accel");
    g_ptr_array_add(args, (gpointer)"qtest");
    g_ptr_array_add(args, (gpointer)"-display");
    g_ptr_array_add(args, (gpointer)"none");
    g_ptr_array_add(args, (gpointer)"-S");
    g_ptr_array_add(args, (gpointer)"-qmp");
    g_ptr_array_add(args, (gpointer)"stdio");
    g_ptr_array_add(args, NULL);
    assert_launch_rejected((const char *const *)args->pdata, message);
}

static QDict *topology_record(const char *name)
{
    QList *topologies = qdict_get_qlist(catalogue, "topologies");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(topologies, e) {
        QDict *t = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(t, "name"), name)) {
            return t;
        }
    }
    g_error("no topology %s in the catalogue", name);
}

static QDict *position_record(QDict *topology, const char *name)
{
    QList *positions = qdict_get_qlist(qdict_get_qdict(topology, "graphics"),
                                       "positions");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(positions, e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(p, "name"), name)) {
            return p;
        }
    }
    g_error("no position %s", name);
}

static QDict *option_record(QDict *position, const char *id)
{
    QList *options = qdict_get_qlist(position, "options");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(options, e) {
        QDict *o = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(qdict_get_qdict(o, "configuration"),
                                      "id"), id)) {
            return o;
        }
    }
    return NULL;
}

static const char *option_id(QDict *option)
{
    return qdict_get_str(qdict_get_qdict(option, "configuration"), "id");
}

static const char *option_family(QDict *option)
{
    return qdict_get_str(qdict_get_qdict(option, "configuration"), "family");
}

/* qom-get @path's @property as a string, or NULL when there is none. */
static char *qom_string(QTestState *qts, const char *path,
                        const char *property)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    QObject *value = qdict_get(response, "return");
    g_autoptr(GString) text = NULL;

    if (!value) {
        return NULL;
    }
    if (qobject_type(value) == QTYPE_QSTRING) {
        return g_strdup(qstring_get_str(qobject_to(QString, value)));
    }
    text = qobject_to_json(value);
    return g_strdup(text->str);
}

/* The QOM path and property that show which head @position holds. */
static void head_probe(const char *topology, const char *family,
                       const char *position, char **path,
                       const char **property)
{
    bool ip30 = g_str_has_prefix(topology, "octane");

    if (g_str_equal(family, "mgras")) {
        *path = g_strdup(ip30 ? "/machine/module0/mgras" : position);
        *property = "board";
    } else if (g_str_equal(family, "odyssey")) {
        g_assert_true(ip30);
        *path = g_strdup("/machine/module0/odyssey");
        *property = "profile";
    } else if (g_str_equal(family, "psitech")) {
        *path = g_strdup_printf("/machine/head-%s", position);
        *property = "device-id";
    } else {
        g_assert_cmpstr(family, ==, "kona");
        *path = g_strdup(position);
        *property = "board-part";
    }
}

static void assert_head(QTestState *qts, const char *topology,
                        QDict *position, QDict *option)
{
    const char *name = qdict_get_str(position, "name");
    const char *id = option_id(option);
    const char *family = option_family(option);
    g_autofree char *path = NULL;
    g_autofree char *value = NULL;
    const char *property;

    /*
     * A built-in VPro is the topology's own board record, named by the
     * record rather than the position; selecting it by name changes
     * nothing, so the launch succeeding is the check.
     */
    if (qdict_get_bool(position, "built-in") &&
        g_str_equal(family, "odyssey")) {
        return;
    }
    head_probe(topology, family, name, &path, &property);
    value = qom_string(qts, path, property);
    g_assert_nonnull(value);
    if (g_str_equal(family, "mgras")) {
        g_assert_cmpstr(value, ==, id);
        if (g_str_has_prefix(topology, "octane")) {
            g_autofree char *slot = qom_string(qts, path, "slot");

            g_assert_cmpstr(slot, ==, name + strlen("widget-"));
        }
    } else if (g_str_equal(family, "odyssey")) {
        g_assert_cmpstr(value, ==, id);
    } else if (g_str_equal(family, "psitech")) {
        g_autofree char *addr = qom_string(qts, path, "addr");
        unsigned device = atoi(name + strlen(name) - 1);

        g_assert_cmpstr(value, ==, g_str_equal(id, "rad4-km") ? "261" :
                        "260");
        g_assert_cmpint(atoi(addr), ==, device << 3);
    } else if (g_str_equal(family, "kona")) {
        g_assert_cmpstr(value, ==, qdict_get_str(option, "assembly"));
    }
}

/* Fit @option at @position of @topology and check the device it builds. */
static void fit(const char *topology, QDict *position, QDict *option)
{
    const Launch *l = launch_for(topology);
    g_autofree char *heads = g_strdup_printf(",heads.%s=%s",
                                             qdict_get_str(position, "name"),
                                             option_id(option));
    QTestState *qts = start(l, heads, "");

    assert_head(qts, topology, position, option);
    qtest_quit(qts);
}

/*
 * Every id a topology offers, at the first position offering it, and every
 * modelled position with the first head it offers.
 */
static void test_offered(gconstpointer data)
{
    const char *topology = data;
    QDict *t = topology_record(topology);
    QList *positions = qdict_get_qlist(qdict_get_qdict(t, "graphics"),
                                       "positions");
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    const QListEntry *pe, *oe;

    QLIST_FOREACH_ENTRY(positions, pe) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(pe));
        bool position_done = false;

        if (!qdict_get_bool(p, "modelled")) {
            continue;
        }
        QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "options"), oe) {
            QDict *o = qobject_to(QDict, qlist_entry_obj(oe));

            if (!qdict_get_bool(o, "selectable") ||
                g_str_equal(option_family(o), "none")) {
                continue;
            }
            if (!g_hash_table_contains(seen, option_id(o))) {
                g_hash_table_add(seen, (gpointer)option_id(o));
                fit(topology, p, o);
                position_done = true;
            } else if (!position_done) {
                fit(topology, p, o);
                position_done = true;
            }
        }
    }
}

/* Every record keeps the generator's rules. */
static void test_catalogue_rules(void)
{
    QList *topologies = qdict_get_qlist(catalogue, "topologies");
    const QListEntry *te, *pe, *oe;
    unsigned graphics = 0;

    QLIST_FOREACH_ENTRY(topologies, te) {
        QDict *t = qobject_to(QDict, qlist_entry_obj(te));
        QDict *g = qdict_get_qdict(t, "graphics");
        bool headless = true;

        if (!g) {
            continue;
        }
        graphics++;
        QLIST_FOREACH_ENTRY(qdict_get_qlist(g, "positions"), pe) {
            QDict *p = qobject_to(QDict, qlist_entry_obj(pe));
            bool modelled = qdict_get_bool(p, "modelled");
            unsigned defaults = 0;

            g_assert_cmpint(qdict_haskey(p, "reason"), ==, !modelled);
            QLIST_FOREACH_ENTRY(qdict_get_qlist(p, "options"), oe) {
                QDict *o = qobject_to(QDict, qlist_entry_obj(oe));
                bool selectable = qdict_get_bool(o, "selectable");

                g_assert_cmpint(qdict_haskey(o, "fidelity"), ==, selectable);
                g_assert_cmpint(qdict_haskey(o, "refused"), ==, !selectable);
                if (qdict_get_bool(o, "default")) {
                    defaults++;
                    g_assert_true(selectable);
                    if (!g_str_equal(option_family(o), "none")) {
                        headless = false;
                    }
                }
            }
            g_assert_cmpint(defaults, ==, modelled ? 1 : 0);
        }
        g_assert_cmpint(qdict_get_bool(g, "headless"), ==, headless);
    }
    g_assert_cmpint(graphics, ==, G_N_ELEMENTS(launches));
}

/* The records the launcher reads for the machines it presets. */
static void test_catalogue_records(void)
{
    QDict *octane2 = topology_record("octane2");
    QDict *w11 = position_record(octane2, "widget-11");
    QDict *v12 = option_record(w11, "v12");
    QDict *deskside = position_record(topology_record("onyx2-deskside"),
                                      "io4");
    QDict *rack = position_record(topology_record("onyx2-rack"), "io3");
    QDict *giga = position_record(topology_record("origin200-gigachannel"),
                                  "io6");
    QDict *o2k = position_record(topology_record("origin2000-rack"), "io3");
    QDict *octane = position_record(topology_record("octane"), "widget-11");
    QList *requires = qdict_get_qlist(v12, "requires");
    QDict *crossbow = qobject_to(QDict, qlist_peek(requires));

    g_assert_true(qdict_get_bool(v12, "selectable"));
    g_assert_cmpstr(qdict_get_str(v12, "fidelity"), ==, "implemented");
    g_assert_cmpstr(qdict_get_str(crossbow, "kind"), ==, "crossbow-revision");
    g_assert_true(qdict_get_bool(crossbow, "met"));
    g_assert_cmpstr(qdict_get_str(option_record(w11, "si"), "pairing"), ==,
                    "inferred");
    g_assert_false(qdict_get_bool(option_record(octane, "v12"),
                                  "selectable"));
    g_assert_true(qdict_get_bool(qdict_get_qdict(octane2, "graphics"),
                                 "headless"));

    g_assert_true(qdict_get_bool(deskside, "built-in"));
    g_assert_true(qdict_get_bool(option_record(deskside, "ir2-ge14-4-rm2"),
                                 "default"));
    g_assert_cmpstr(qdict_get_str(option_record(deskside, "ir2-ge14-4-rm2"),
                                  "fidelity"), ==, "approximated");
    g_assert_true(qdict_get_bool(option_record(rack, "ir3-ge16-4-rm1"),
                                 "default"));
    g_assert_false(qdict_get_bool(option_record(rack, "ir3-ge16-4-rm1-gvo"),
                                  "selectable"));

    g_assert_true(qdict_get_bool(option_record(giga, "si-t"), "selectable"));
    g_assert_false(qdict_get_bool(option_record(giga, "mxi"), "selectable"));
    g_assert_cmpstr(qdict_get_str(option_record(o2k, "ir3-ge16-4-rm1"),
                                  "refused"), ==,
                    "An Origin 2000 with an InfiniteReality pipe is an "
                    "Onyx2; use onyx2-rack.");
}

/* Each refusal template, word for word. */
static void test_refusals(void)
{
    assert_refused(launch_for("origin200"), ",heads.io6=si",
                   "origin200: topology 'origin200' has no graphics position "
                   "'io6'; its positions are pci-5, pci-6, pci-7");
    assert_refused(launch_for("origin200-gigachannel"), ",heads.io6=vpro",
                   "origin200: topology 'origin200-gigachannel' offers no "
                   "graphics 'vpro' at io6; it offers none, si, si-t, esi, "
                   "ssi, essi");
    assert_refused(launch_for("origin2000-rack"), ",heads.io3=ir3-ge16-4-rm1",
                   "origin2000: topology 'origin2000-rack' does not build "
                   "InfiniteReality3 (ir3-ge16-4-rm1) at io3: An Origin 2000 "
                   "with an InfiniteReality pipe is an Onyx2; use "
                   "onyx2-rack.");
    assert_refused(launch_for("origin200-gigachannel"),
                   ",heads.io6=ir3-ge16-4-rm1",
                   "origin200: topology 'origin200-gigachannel' does not "
                   "build InfiniteReality3 (ir3-ge16-4-rm1) at io6: SGI did "
                   "not offer InfiniteReality on the Origin 200; the known "
                   "GIGAchannel pipe is an owner conversion the model does "
                   "not build.");
    assert_refused(launch_for("octane"), ",heads.widget-11=v12",
                   "octane: topology 'octane' does not build VPro V12 (v12) "
                   "at widget-11: VPro V10 and V12 need Crossbow 1.4, the ER "
                   "power supply 060-0035-00x and PROM 4.5 (007-4210-003); "
                   "this Octane presents Crossbow revision code 4, below the "
                   "1.4 required, and the SR supply.");
    assert_refused(launch_for("octane"), ",heads.pci-cage=rad4",
                   "octane: topology 'octane' cannot fit graphics at "
                   "pci-cage: the model has no Octane PCI cage");
    assert_refused(launch_for("origin2000-rack"), ",heads.io3=si,heads.io5=si",
                   "origin2000: topology 'origin2000-rack' builds at most 1 "
                   "graphics head(s); io3 and io5 both select one");
    assert_refused(launch_for("octane"),
                   ",graphics-board=si,heads.widget-11=si",
                   "octane: heads.widget-11 and graphics-board both select a "
                   "graphics board; use heads.widget-11");
    assert_refused(launch_for("onyx2-deskside"), ",heads.io4=ir4-ge16-4-rm2",
                   "onyx2: topology 'onyx2-deskside' does not build "
                   "InfiniteReality4 (ir4-ge16-4-rm2) at io4: the model "
                   "presents one configuration");
    assert_refused(launch_for("origin200-dual"), ",heads.m1-pci-5=rad4",
                   "origin200: topology 'origin200-dual' cannot fit graphics "
                   "at m1-pci-5: the model fits PCI heads on the first module "
                   "only");
}

/*
 * -nographic sets the core machine property graphics=off in the same
 * dictionary the dotted heads. keys build; both must survive together.
 */
static void test_nographic(void)
{
    const Launch *l = launch_for("origin200-gigachannel");

    for (int nographic = 0; nographic < 2; nographic++) {
        QTestState *qts = start(l, ",heads.io6=si-t",
                                nographic ? "-nographic -serial none "
                                "-monitor none -parallel none" : "");
        g_autofree char *board = qom_string(qts, "io6", "board");
        g_autofree char *graphics = qom_string(qts, "/machine", "graphics");

        g_assert_cmpstr(board, ==, "si-t");
        g_assert_cmpstr(graphics, ==, nographic ? "false" : "true");
        qtest_quit(qts);
    }
}

/* With no heads, every topology builds what it built before. */
static void test_absent(void)
{
    QTestState *qts;
    g_autofree char *slot = NULL;
    g_autofree char *part = NULL;
    g_autofree char *type = NULL;

    qts = start(launch_for("origin200-gigachannel"), "", "");
    g_assert_null(qom_string(qts, "io6", "board"));
    qtest_quit(qts);

    qts = start(launch_for("origin200"), "", "");
    g_assert_null(qom_string(qts, "/machine/head-pci-5", "device-id"));
    qtest_quit(qts);

    qts = start(launch_for("octane"), "", "");
    g_assert_null(qom_string(qts, "/machine/module0/mgras", "board"));
    g_assert_null(qom_string(qts, "/machine/module0/odyssey", "profile"));
    qtest_quit(qts);

    /* graphics-board= still builds its board with no heads. */
    qts = start(launch_for("octane"), ",graphics-board=si,graphics-widget=10",
                "");
    slot = qom_string(qts, "/machine/module0/mgras", "slot");
    g_assert_cmpstr(slot, ==, "10");
    qtest_quit(qts);

    qts = start(launch_for("onyx2-deskside"), "", "");
    part = qom_string(qts, "io4", "board-part");
    g_assert_cmpstr(part, ==, "030-1129-002");
    qtest_quit(qts);

    qts = start(launch_for("fuel-vpro"), "", "");
    type = qom_string(qts, VPRO_FUEL_PATH, "type");
    g_assert_cmpstr(type, ==, "sgi-odyssey");
    qtest_quit(qts);
}

/* A built-in position takes its own default by name, as with no heads. */
static void test_built_in_by_name(void)
{
    static const char *const cases[][2] = {
        { "onyx2-deskside", "io4" },
        { "onyx2-rack", "io3" },
        { "fuel-vpro", "widget-13" },
        { "origin300-v12-direct", "widget-11" },
        { "origin300-vbrick", "widget-11" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        QDict *p = position_record(topology_record(cases[i][0]), cases[i][1]);
        QList *options = qdict_get_qlist(p, "options");
        QDict *d = qobject_to(QDict, qlist_peek(options));
        const Launch *l = launch_for(cases[i][0]);
        g_autofree char *heads = NULL;
        QTestState *qts;

        g_assert_true(qdict_get_bool(d, "default"));
        heads = g_strdup_printf(",heads.%s=%s", cases[i][1], option_id(d));
        qts = start(l, heads, "");
        if (g_str_equal(option_family(d), "kona")) {
            assert_head(qts, cases[i][0], p, d);
        }
        qtest_quit(qts);
    }
}

static char *temporary(const char *name, gsize size, uint8_t fill)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc(size);
    char *path = NULL;
    int fd = g_file_open_tmp(name, &path, &error);

    g_assert_no_error(error);
    memset(image, fill, size);
    g_assert_cmpint(write(fd, image, size), ==, (ssize_t)size);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

int main(int argc, char **argv)
{
    QTestState *qts;
    QDict *reply;
    int result;

    g_test_init(&argc, &argv, NULL);
    octane_flash = temporary("sgi-heads-flash-XXXXXX", 4, 0xff);
    sn1_prom = temporary("sgi-heads-prom-XXXXXX.bin", PROM_SIZE, 0);

    qts = qtest_init("-M none");
    reply = qtest_qmp(qts, "{'execute':'query-sgi-machines'}");
    catalogue = qobject_ref(qdict_get_qdict(reply, "return"));
    qobject_unref(reply);
    qtest_quit(qts);

    qtest_add_func("/sgi-heads/catalogue/rules", test_catalogue_rules);
    qtest_add_func("/sgi-heads/catalogue/records", test_catalogue_records);
    qtest_add_func("/sgi-heads/refusals", test_refusals);
    qtest_add_func("/sgi-heads/nographic", test_nographic);
    qtest_add_func("/sgi-heads/absent", test_absent);
    qtest_add_func("/sgi-heads/built-in-by-name", test_built_in_by_name);
    for (size_t i = 0; i < G_N_ELEMENTS(launches); i++) {
        g_autofree char *path = g_strdup_printf("/sgi-heads/offered/%s",
                                                launches[i].topology);

        qtest_add_data_func(path, launches[i].topology, test_offered);
    }
    result = g_test_run();
    qobject_unref(catalogue);
    unlink(octane_flash);
    unlink(sn1_prom);
    g_free(octane_flash);
    g_free(sn1_prom);
    return result;
}
