/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics pipes against the machine catalogue (docs/specs/sgi-sn.rst,
 * Graphics pipes): one -device per pipe, product= and the product's
 * settings, each machine's default pipe, -vga none, and every refusal the
 * catalogue's records give a reason for.
 *
 * The catalogue's own records drive the default and offer cases, so a
 * record that changes is tested as it lands; the record cases pin what the
 * first implementation lists, word for word where a launcher shows it.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "sgi-launch.h"
#include "sgi-test.h"

#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

/* The IP53 PIC fabric's Crossbow link registers for widget w. */
#define IP53_XBOW_LINK_STATUS(w) (0x100 + ((w) - 8) * 0x40 + 0x10 + 4)
#define IP53_XBOW_LINK_AUX(w) (0x100 + ((w) - 8) * 0x40 + 0x38 + 4)
#define IP53_XBOW_LINK_ALIVE (1u << 31)
#define IP53_XBOW_AUX_PRESENT (1u << 5)

static char *octane_flash;
static char *sn1_prom;
static char *ip53_data;
static char *ip53_clock;
static QDict *catalogue;

/* How to launch each topology that has a graphics record. */
typedef struct Launch {
    const char *topology;
    const char *machine;
    const char *extra;
    enum { NO_BIOS, OCTANE_FLASH, SN1_PROM } bios;
    /* Where its default pipe sits, or NULL for a machine with none. */
    const char *default_path;
    /* Whether it needs the IP53 IO9's DS1742 storage. */
    bool ip53_io9;
} Launch;

static const Launch launches[] = {
    { "origin200", "origin200", "", NO_BIOS, NULL },
    { "origin200-dual", "origin200,topology=origin200-dual",
      "-smp 4 -m 128M", NO_BIOS, NULL },
    { "origin200-gigachannel", "origin200,topology=origin200-gigachannel",
      "", NO_BIOS, NULL },
    { "origin200-gigachannel-loopback",
      "origin200,topology=origin200-gigachannel-loopback", "", NO_BIOS,
      NULL },
    { "origin2000-deskside", "origin2000,topology=origin2000-deskside",
      "-smp 8", NO_BIOS, NULL },
    { "origin2000-rack", "origin2000", "-smp 8", NO_BIOS, NULL },
    { "onyx2-deskside", "onyx2,topology=onyx2-deskside",
      "-smp 4 -m 128M", NO_BIOS, "/machine/module0/io4" },
    { "onyx2-rack", "onyx2", "", NO_BIOS, "/machine/module0/io3" },
    { "octane", "octane", "-m 128M -cpu R10000", OCTANE_FLASH,
      "/machine/module0/widget-11" },
    { "octane2", "octane2", "-m 256M -cpu " OCTANE2_CPU, OCTANE_FLASH,
      "/machine/module0/widget-11" },
    { "fuel", "fuel", "-cpu R14000 -smp 1 -m 512M",
      SN1_PROM, "/machine/module0/graphics-slot" },
    { "origin300-v12-direct", "origin300,topology=origin300-v12-direct",
      "-cpu R14000 -smp 2", SN1_PROM, "/machine/module0/rear-panel" },
    { "origin300-vbrick", "origin300,topology=origin300-vbrick",
      "-cpu R14000 -smp 2", SN1_PROM, "/machine/module1/vbrick-v12" },
    { "onyx350-compat", "onyx350,topology=onyx350-compat",
      "-cpu R16000,r16000-experimental=on,r16000-reset-mode=0x009caf85,"
      "r16000-scache-bytes=0x800000 -smp 4 -m 8G", SN1_PROM, NULL, true },
    { "tezro-tower-multi", "tezro,topology=tezro-tower-multi",
      "-cpu R16000,r16000-experimental=on,r16000-reset-mode=0x009bad85,"
      "r16000-scache-bytes=0x400000 -smp 4 -m 4G", SN1_PROM, NULL, true },
};

static const Launch origin300 = {
    "origin300", "origin300", "-cpu R14000 -smp 2", SN1_PROM, NULL,
};

static const Launch origin2000_one_node = {
    "origin2000-rack", "origin2000,nodes=1", "-smp 2 -m 64M", NO_BIOS, NULL,
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

/* The IP53 IO9's DS1742 data and clock drives, for @l if it needs them. */
static char *ip53_storage(const Launch *l)
{
    return l->ip53_io9 ?
        g_strdup_printf("-drive if=none,id=sgi-ds1742-data-io9,file=%s,"
                        "format=raw -drive if=none,id=sgi-ds1742-clock-io9,"
                        "file=%s,format=raw", ip53_data, ip53_clock) :
        g_strdup("");
}

/* Start @l's machine with @more on its command line. */
static QTestState *start(const Launch *l, const char *more)
{
    const char *bios = bios_path(l);
    g_autofree char *quoted = bios ? g_shell_quote(bios) : NULL;
    g_autofree char *storage = ip53_storage(l);

    return qtest_initf("-M %s -display none -S %s %s%s %s %s", l->machine,
                       l->extra, bios ? "-bios " : "", bios ? quoted : "",
                       storage, more);
}

/* The same launch as an argument vector, expecting refusal @message. */
static void assert_refused(const Launch *l, const char *more,
                           const char *message)
{
    const char *bios = bios_path(l);
    g_autofree char *storage = ip53_storage(l);
    g_autofree char *line = g_strdup_printf("%s %s %s", l->extra, storage,
                                            more);
    g_auto(GStrv) words = g_strsplit(line, " ", -1);
    g_autoptr(GPtrArray) args = g_ptr_array_new();

    g_ptr_array_add(args, (gpointer)qtest_qemu_binary(NULL));
    g_ptr_array_add(args, (gpointer)"-M");
    g_ptr_array_add(args, (gpointer)l->machine);
    for (char **word = words; *word; word++) {
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

static QDict *find(QList *list, const char *key, const char *value)
{
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(list, e) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(d, key), value)) {
            return d;
        }
    }
    return NULL;
}

static QDict *topology_record(const char *name)
{
    QDict *t = find(qdict_get_qlist(catalogue, "topologies"), "name", name);

    g_assert_nonnull(t);
    return t;
}

static QDict *graphics_record(const char *topology)
{
    return qdict_get_qdict(topology_record(topology), "graphics");
}

static QDict *slot_record(const char *topology, const char *slot)
{
    QDict *s = find(qdict_get_qlist(graphics_record(topology), "slots"),
                    "name", slot);

    g_assert_nonnull(s);
    return s;
}

static QDict *product_record(QDict *slot, const char *family,
                             const char *product)
{
    QDict *f = find(qdict_get_qlist(slot, "families"), "family", family);

    return f ? find(qdict_get_qlist(f, "products"), "product", product) :
           NULL;
}

/* "slot=io4" or "bus=pci,addr=5": how -device names @slot. */
static char *placement(QDict *slot)
{
    GString *text = g_string_new(NULL);
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(slot, "placement"), e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));

        g_string_append_printf(text, "%s%s=%s", text->len ? "," : "",
                               qdict_get_str(p, "name"),
                               qdict_get_str(p, "value"));
    }
    return g_string_free(text, false);
}

/* The refusal reason @slot gives for @product of @family, or NULL. */
static const char *refusal_reason(QDict *slot, const char *family,
                                  const char *product, const char *setting,
                                  const char *value)
{
    QList *refusals = qdict_get_qlist(slot, "refusals");
    const QListEntry *e;

    if (!refusals) {
        return NULL;
    }
    QLIST_FOREACH_ENTRY(refusals, e) {
        QDict *r = qobject_to(QDict, qlist_entry_obj(e));
        QList *settings = qdict_get_qlist(r, "settings");
        QDict *first = settings ?
            qobject_to(QDict, qlist_peek(settings)) : NULL;

        if (!g_str_equal(qdict_get_str(r, "family"), family) ||
            (qdict_haskey(r, "product") &&
             (!product || !g_str_equal(qdict_get_str(r, "product"),
                                       product))) ||
            !!first != !!setting ||
            (first && (!g_str_equal(qdict_get_str(first, "name"), setting) ||
                       !g_str_equal(qdict_get_str(first, "value"), value)))) {
            continue;
        }
        return qdict_get_str(r, "reason");
    }
    return NULL;
}

/* qom-get @path's @property as text, or NULL when there is none. */
static char *qom_text(QTestState *qts, const char *path, const char *property)
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

static char *qom_type(QTestState *qts, const char *path)
{
    return qom_text(qts, path, "type");
}

/* The pipe at @path is @product of @device with every setting in @pipe. */
static void assert_pipe(QTestState *qts, const char *path,
                        const char *device, QDict *pipe)
{
    g_autofree char *type = qom_type(qts, path);
    g_autofree char *product = qom_text(qts, path, "product");
    const QListEntry *e;

    g_assert_cmpstr(type, ==, device);
    g_assert_cmpstr(product, ==, qdict_get_str(pipe, "product"));
    QLIST_FOREACH_ENTRY(qdict_get_qlist(pipe, "settings"), e) {
        QDict *s = qobject_to(QDict, qlist_entry_obj(e));
        g_autofree char *value = qom_text(qts, path,
                                          qdict_get_str(s, "name"));

        g_assert_cmpstr(value, ==, qdict_get_str(s, "value"));
    }
}

/*
 * Where the pipe -device id=pipe created sits: a PCI card under the
 * peripherals, an XIO board under the module its slot is in.
 */
static char *pipe_path(QTestState *qts, const char *device)
{
    if (g_str_equal(device, "psitech-rad4")) {
        return g_strdup("/machine/peripheral/pipe");
    }
    for (unsigned module = 0; module < 4; module++) {
        g_autofree char *path = g_strdup_printf("/machine/module%u/pipe",
                                                module);
        g_autofree char *type = qom_type(qts, path);

        if (type) {
            g_assert_cmpstr(type, ==, device);
            return g_steal_pointer(&path);
        }
    }
    g_error("no %s pipe in any module", device);
}

static const char *device_for(const char *family)
{
    return g_str_equal(family, "mgras") ? "sgi-mgras" :
           g_str_equal(family, "odyssey") ? "sgi-odyssey" :
           g_str_equal(family, "kona") ? "sgi-kona" : "psitech-rad4";
}

/*
 * What the first implementation lists: which topologies a launcher offers
 * without graphics and why not the rest, each default, the refusals a
 * launcher shows, and nothing QEMU does not build.
 */
static void test_catalogue_records(void)
{
    static const struct {
        const char *topology;
        bool headless;
        const char *reason;
    } headless[] = {
        { "origin200", true, NULL },
        { "origin200-dual", true, NULL },
        { "origin200-gigachannel", true, NULL },
        { "origin200-gigachannel-loopback", true, NULL },
        { "origin2000-deskside", true, NULL },
        { "origin2000-rack", true, NULL },
        { "octane", true, NULL },
        { "octane2", true, NULL },
        { "onyx2-deskside", false,
          "SGI sold no Onyx2 without an InfiniteReality pipe, and an Onyx2 "
          "with no pipe is an Origin 2000, so use origin2000-deskside or "
          "origin2000-rack" },
        { "onyx2-rack", false,
          "SGI sold no Onyx2 without an InfiniteReality pipe, and an Onyx2 "
          "with no pipe is an Origin 2000, so use origin2000-deskside or "
          "origin2000-rack" },
        { "onyx350-compat", true, NULL },
        { "tezro-tower-multi", true, NULL },
        { "fuel", false,
          "every Fuel has its VPro graphics slot at widget 13" },
        { "origin300-v12-direct", false,
          "this topology exists to fit its V12, so use origin300 for no "
          "graphics" },
        { "origin300-vbrick", false,
          "this topology exists to fit its V12, so use origin300 for no "
          "graphics" },
    };
    static const char *const unbuilt[] = {
        "v6", "v8", "v10", "ir4", "ir", "reality", "rad4c-km", "rad1",
    };
    const QListEntry *e;
    QDict *slot, *product;

    for (size_t i = 0; i < G_N_ELEMENTS(headless); i++) {
        QDict *g = graphics_record(headless[i].topology);

        g_assert_nonnull(g);
        g_assert_cmpint(qdict_get_int(g, "max-pipes"), ==, 1);
        g_assert_cmpint(qdict_get_bool(g, "headless"), ==,
                        headless[i].headless);
        if (headless[i].reason) {
            g_assert_cmpstr(qdict_get_str(g, "headless-refused"), ==,
                            headless[i].reason);
        } else {
            g_assert_false(qdict_haskey(g, "headless-refused"));
        }
    }
    /* The Origin 300's XIO connector is a cable port, not a slot. */
    g_assert_null(qdict_get(topology_record("origin300"), "graphics"));

    /* Nothing QEMU does not build appears, offered or refused. */
    QLIST_FOREACH_ENTRY(qdict_get_qlist(catalogue, "topologies"), e) {
        QDict *t = qobject_to(QDict, qlist_entry_obj(e));
        QDict *g = qdict_get_qdict(t, "graphics");
        g_autoptr(GString) text = NULL;

        if (!g) {
            continue;
        }
        text = qobject_to_json(QOBJECT(g));
        for (size_t i = 0; i < G_N_ELEMENTS(unbuilt); i++) {
            g_autofree char *quoted = g_strdup_printf("\"%s\"", unbuilt[i]);

            g_assert_null(strstr(text->str, quoted));
        }
        g_assert_null(strstr(text->str, "gvo"));
        g_assert_null(strstr(text->str, "\"changeable\": true"));
    }

    /* The deskside's default is whole-tetra's configuration. */
    slot = slot_record("onyx2-deskside", "io4");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(slot, "default"),
                                  "product"), ==, "ir2");
    product = product_record(slot, "kona", "ir2");
    g_assert_cmpstr(qdict_get_str(product, "name"), ==, "InfiniteReality2");
    g_assert_cmpstr(qdict_get_str(product, "assembly"), ==, "030-1129-002");
    g_assert_nonnull(product_record(slot, "kona", "ir2e"));
    g_assert_null(product_record(slot, "kona", "ir3"));
    g_assert_cmpstr(refusal_reason(slot, "kona", NULL, "rm-count", "4"), ==,
                    "a pipe with no Ktown is an Onyx2 deskside pipe, which "
                    "holds at most two raster managers");
    g_assert_cmpstr(refusal_reason(slot, "kona", "ir3", NULL, NULL), ==,
                    "no manual or capture places an InfiniteReality3 in an "
                    "Onyx2 deskside");

    /*
     * The rack's default is true-wombat's with a plain DG5-2 in place of
     * its DG5-2 with GVO, which is not modelled.
     */
    slot = slot_record("onyx2-rack", "io3");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(slot, "default"),
                                  "product"), ==, "ir3");
    g_assert_null(product_record(slot, "kona", "ir2"));
    g_assert_nonnull(refusal_reason(slot, "kona", "ir2", NULL, NULL));
    {
        QDict *ir3 = product_record(slot, "kona", "ir3");
        QDict *display = find(qdict_get_qlist(ir3, "settings"), "name",
                              "display");
        QList *values = qdict_get_qlist(display, "values");

        g_assert_cmpstr(qdict_get_str(display, "default"), ==, "dg5-2");
        g_assert_cmpstr(qdict_get_str(find(values, "value", "dg5-2"),
                                      "evidence"), ==,
                        "030-1055-001, fancy-akita");
        g_assert_nonnull(find(values, "value", "dg5-8"));
        g_assert_cmpint(qlist_size(values), ==, 2);
    }

    slot = slot_record("octane", "widget-11");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(slot, "default"),
                                  "product"), ==, "si");
    g_assert_cmpstr(refusal_reason(slot, "odyssey", NULL, NULL, NULL), ==,
                    "VPro V12 needs Crossbow 1.4, the ER power supply "
                    "060-0035-00x and PROM 4.5 (007-4210-003), and this "
                    "Octane presents Crossbow revision code 4 and the SR "
                    "supply");
    slot = slot_record("octane2", "widget-11");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(slot, "default"),
                                  "product"), ==, "v12");
    g_assert_nonnull(product_record(slot, "mgras", "si"));
    g_assert_null(product_record(slot_record("octane2", "widget-9"),
                                 "odyssey", "v12"));

    slot = slot_record("origin2000-rack", "io3");
    g_assert_cmpstr(refusal_reason(slot, "kona", NULL, NULL, NULL), ==,
                    "an Origin 2000 with an InfiniteReality pipe is an "
                    "Onyx2, so use onyx2-rack");
    g_assert_cmpint(qdict_get_int(qdict_get_qdict(
        slot_record("origin2000-rack", "io7"), "requires"), "min-nodes"),
        ==, 2);
    g_assert_false(qdict_haskey(slot, "requires"));
    slot = slot_record("origin200-gigachannel", "io6");
    g_assert_cmpstr(refusal_reason(slot, "kona", NULL, NULL, NULL), ==,
                    "SGI did not offer InfiniteReality on the Origin 200, "
                    "and the known GIGAchannel pipe is an owner conversion "
                    "the model does not build");
    g_assert_cmpstr(qdict_get_str(product_record(
        slot_record("origin200", "pci-5"), "psitech", "rad4"), "fidelity"),
        ==, "validated");
    {
        g_autofree char *where = placement(slot_record("origin200-dual",
                                                       "m0-pci-6"));

        g_assert_cmpstr(where, ==, "bus=pci,addr=6");
    }
}

/*
 * A machine that ships with graphics fits its catalogue default, with the
 * product and every setting the record gives; a server machine fits none.
 */
static void test_default(const void *data)
{
    const Launch *l = data;
    QDict *g = graphics_record(l->topology);
    QTestState *qts = start(l, "");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(g, "slots"), e) {
        QDict *slot = qobject_to(QDict, qlist_entry_obj(e));
        QDict *pipe = qdict_get_qdict(slot, "default");

        if (pipe) {
            g_assert_nonnull(l->default_path);
            assert_pipe(qts, l->default_path,
                        device_for(qdict_get_str(pipe, "family")), pipe);
        }
    }
    /* A machine with no default pipe leaves every XIO slot empty. */
    QLIST_FOREACH_ENTRY(qdict_get_qlist(g, "slots"), e) {
        QDict *slot = qobject_to(QDict, qlist_entry_obj(e));
        g_autofree char *path = g_strdup_printf("/machine/module0/%s",
                                                qdict_get_str(slot, "name"));
        g_autofree char *type = NULL;

        if (l->default_path || qdict_haskey(slot, "requires") ||
            !g_str_has_prefix(qdict_get_str(slot, "name"), "io")) {
            continue;
        }
        type = qom_type(qts, path);
        g_assert_null(type);
    }
    qtest_quit(qts);
}

/*
 * Any SGI graphics -device replaces the default, whichever slot it names:
 * an Octane with an SI in widget 9 leaves widget 11 empty, and a bare
 * -device in the default's own slot takes the slot's default settings.
 */
static void test_device_replaces_default(void)
{
    QTestState *qts;
    g_autofree char *type = NULL;
    g_autofree char *other = NULL;
    QDict *pipe;

    qts = start(launch_for("octane"),
                "-device sgi-mgras,slot=widget-9,product=essi");
    type = qom_type(qts, "/machine/module0/widget-9");
    g_assert_cmpstr(type, ==, "sgi-mgras");
    other = qom_type(qts, "/machine/module0/widget-11");
    g_assert_null(other);
    qtest_quit(qts);

    qts = start(launch_for("octane2"),
                "-device sgi-mgras,slot=widget-11,product=si-t");
    g_free(type);
    type = qom_text(qts, "/machine/module0/widget-11", "product");
    g_assert_cmpstr(type, ==, "si-t");
    qtest_quit(qts);

    /* fancy-akita's pipe: GE14-2, two RMs and its own display generator. */
    qts = start(launch_for("onyx2-deskside"),
                "-device sgi-kona,slot=io4,product=ir2,geometry=ge14-2,"
                "rm-count=2,display=dg5-2");
    g_free(type);
    type = qom_text(qts, "/machine/module0/io4", "geometry");
    g_assert_cmpstr(type, ==, "ge14-2");
    g_free(type);
    type = qom_text(qts, "/machine/module0/io4", "display");
    g_assert_cmpstr(type, ==, "dg5-2");
    g_free(type);
    type = qom_text(qts, "/machine/module0/io4", "board-part");
    g_assert_cmpstr(type, ==, "030-1129-002");
    qtest_quit(qts);

    /* A bare -device sgi-kona,slot=io4 is the deskside's ir2. */
    pipe = qdict_get_qdict(slot_record("onyx2-deskside", "io4"), "default");
    qts = start(launch_for("onyx2-deskside"), "-device sgi-kona,slot=io4");
    assert_pipe(qts, "/machine/module0/io4", "sgi-kona", pipe);
    qtest_quit(qts);

    /* The rack's pipe takes four RMs, behind its Ktown. */
    qts = start(launch_for("onyx2-rack"),
                "-device sgi-kona,slot=io3,product=ir3,rm-count=4");
    g_free(type);
    type = qom_text(qts, "/machine/module0/io3", "rm-count");
    g_assert_cmpstr(type, ==, "4");
    g_free(type);
    type = qom_text(qts, "/machine/module0/io3", "other-identity-parts");
    g_assert_cmpstr(type, ==, "[\"030-1067-001\"]");
    qtest_quit(qts);
}

/*
 * -vga none skips the default on every topology, whatever the catalogue's
 * headless flag tells a launcher.
 */
static void test_vga_none(void)
{
    static const char *const topologies[] = {
        "octane", "octane2", "origin2000-rack", "origin200",
        "onyx2-deskside", "onyx2-rack", "fuel", "origin300-v12-direct",
        "origin300-vbrick",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(topologies); i++) {
        const Launch *l = launch_for(topologies[i]);
        QTestState *qts = start(l, "-vga none");
        g_autofree char *type = qom_type(qts, l->default_path ?
                                         l->default_path :
                                         "/machine/module0/widget-11");

        g_assert_null(type);
        qtest_quit(qts);
    }
    /* -nodefaults drops the default too. */
    {
        QTestState *qts = start(launch_for("octane2"), "-nodefaults");
        g_autofree char *type = qom_type(qts, "/machine/module0/widget-11");

        g_assert_null(type);
        qtest_quit(qts);
    }
    /* A pipe -device beside -vga none is still a pipe. */
    {
        QTestState *qts = start(launch_for("onyx2-rack"),
                                "-vga none -device sgi-kona,slot=io3");
        g_autofree char *type = qom_type(qts, "/machine/module0/io3");

        g_assert_cmpstr(type, ==, "sgi-kona");
        qtest_quit(qts);
    }
}

/* Every other -vga value is refused on every SGI machine. */
static void test_vga_other_refused(void)
{
    static const char *const topologies[] = {
        "octane", "onyx2-deskside", "origin200", "fuel",
    };

    for (size_t i = 0; i < G_N_ELEMENTS(topologies); i++) {
        assert_refused(launch_for(topologies[i]), "-vga std",
                       "-vga std is not a display an SGI machine takes; use "
                       "-vga none for no graphics, or a graphics -device "
                       "such as sgi-mgras");
    }
}

/* Each refusal carries what the design's table says it does. */
static void test_refusals(void)
{
    /* No slot given: the topology's graphics slots. */
    assert_refused(launch_for("octane"), "-device sgi-mgras",
                   "sgi-mgras needs slot= naming one of the graphics slots "
                   "of topology 'octane': widget-11 (slot=widget-11), "
                   "widget-9 (slot=widget-9), widget-10 (slot=widget-10), "
                   "widget-12 (slot=widget-12)");
    assert_refused(launch_for("origin200"), "-device psitech-rad4,bus=pci",
                   "psitech-rad4 needs bus= and addr= naming one of the "
                   "graphics slots of topology 'origin200': pci-5 "
                   "(bus=pci,addr=5), pci-6 (bus=pci,addr=6), pci-7 "
                   "(bus=pci,addr=7)");
    /* A slot the record does not list. */
    assert_refused(launch_for("origin200"),
                   "-device psitech-rad4,bus=pci,addr=3",
                   "topology 'origin200' has no graphics slot at "
                   "bus=pci,addr=3; its graphics slots are pci-5 "
                   "(bus=pci,addr=5)");
    assert_refused(launch_for("origin200-gigachannel"),
                   "-device sgi-mgras,slot=io3",
                   "topology 'origin200-gigachannel' has no graphics slot "
                   "io3; its graphics slots are io6 (slot=io6)");
    /* A family the slot does not take, with the record's reason. */
    assert_refused(launch_for("origin2000-rack"),
                   "-device sgi-kona,slot=io3",
                   "graphics slot io3 of topology 'origin2000-rack' takes "
                   "sgi-mgras, not sgi-kona: an Origin 2000 with an "
                   "InfiniteReality pipe is an Onyx2, so use onyx2-rack");
    /*
     * ... including at a slot whose refusals name a setting the requested
     * family does not have.
     */
    assert_refused(launch_for("onyx2-deskside"),
                   "-device sgi-mgras,slot=io4",
                   "graphics slot io4 of topology 'onyx2-deskside' takes "
                   "sgi-kona, not sgi-mgras");
    assert_refused(launch_for("onyx2-deskside"),
                   "-device sgi-odyssey,slot=io4",
                   "graphics slot io4 of topology 'onyx2-deskside' takes "
                   "sgi-kona, not sgi-odyssey");
    assert_refused(launch_for("origin200-gigachannel"),
                   "-device sgi-kona,slot=io6",
                   "takes sgi-mgras, not sgi-kona: SGI did not offer "
                   "InfiniteReality on the Origin 200, and the known "
                   "GIGAchannel pipe is an owner conversion the model does "
                   "not build");
    /* A product the slot does not offer, with the record's reason. */
    assert_refused(launch_for("onyx2-deskside"),
                   "-device sgi-kona,slot=io4,product=ir3",
                   "graphics slot io4 of topology 'onyx2-deskside' does not "
                   "offer sgi-kona product=ir3; it offers ir2 and ir2e: no "
                   "manual or capture places an InfiniteReality3 in an Onyx2 "
                   "deskside");
    assert_refused(launch_for("onyx2-rack"),
                   "-device sgi-kona,slot=io3,product=ir2",
                   "does not offer sgi-kona product=ir2; it offers ir2e and "
                   "ir3: no manual or capture places a GE14 InfiniteReality2 "
                   "in an Onyx2 rack");
    /* Unmodelled hardware is refused as not offered, with no reason. */
    assert_refused(launch_for("onyx2-rack"),
                   "-device sgi-kona,slot=io3,product=ir4",
                   "does not offer sgi-kona product=ir4; it offers ir2e and "
                   "ir3");
    assert_refused(launch_for("octane2"),
                   "-device sgi-odyssey,slot=widget-11,product=v8",
                   "does not offer sgi-odyssey product=v8; it offers v12");
    assert_refused(launch_for("onyx2-rack"),
                   "-device sgi-kona,slot=io3,display=dg5-2-gvo",
                   "does not offer sgi-kona product=ir3 with "
                   "display=dg5-2-gvo; it offers dg5-2 and dg5-8");
    /* A setting value the slot does not offer, with the record's reason. */
    assert_refused(launch_for("onyx2-deskside"),
                   "-device sgi-kona,slot=io4,product=ir2,rm-count=4",
                   "does not offer sgi-kona product=ir2 with rm-count=4; it "
                   "offers 1 and 2: a pipe with no Ktown is an Onyx2 "
                   "deskside pipe, which holds at most two raster managers");
    assert_refused(launch_for("onyx2-deskside"),
                   "-device sgi-kona,slot=io4,product=ir2e,geometry=ge14-4",
                   "does not offer sgi-kona product=ir2e with "
                   "geometry=ge14-4; it offers ge16-4");
    /* More pipes than the topology builds. */
    assert_refused(launch_for("octane"),
                   "-device sgi-mgras,slot=widget-9 "
                   "-device sgi-mgras,slot=widget-10",
                   "topology 'octane' builds at most 1 graphics pipe, and "
                   "widget-9 already holds one");
    /* A slot this machine's node boards do not fit. */
    assert_refused(&origin2000_one_node, "-device sgi-mgras,slot=io7",
                   "graphics slot io7 of topology 'origin2000-rack' exists "
                   "from nodes=2, and this machine fits 1");
    /* A topology with no graphics record. */
    assert_refused(&origin300, "-device sgi-odyssey,slot=widget-11",
                   "topology 'origin300' has no graphics slots, so it takes "
                   "no sgi-odyssey");
}

/*
 * Every product each slot offers is built there, and every value each
 * setting offers: one launch per product at the first slot offering it,
 * one per slot with that slot's first product, and one per further
 * setting value.
 */
static void test_offered(const void *data)
{
    const Launch *l = data;
    QDict *g = graphics_record(l->topology);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal, g_free,
                                                       NULL);
    const QListEntry *s, *f, *p, *v;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(g, "slots"), s) {
        QDict *slot = qobject_to(QDict, qlist_entry_obj(s));
        g_autofree char *where = placement(slot);
        bool slot_tried = false;

        QLIST_FOREACH_ENTRY(qdict_get_qlist(slot, "families"), f) {
            QDict *family = qobject_to(QDict, qlist_entry_obj(f));
            const char *device = qdict_get_str(family, "device");

            QLIST_FOREACH_ENTRY(qdict_get_qlist(family, "products"), p) {
                QDict *product = qobject_to(QDict, qlist_entry_obj(p));
                const char *name = qdict_get_str(product, "product");
                g_autofree char *key = g_strdup_printf("%s/%s", device,
                                                       name);
                QTestState *qts;
                g_autofree char *base = NULL;

                if (slot_tried && g_hash_table_contains(seen, key)) {
                    continue;
                }
                slot_tried = true;
                g_hash_table_add(seen, g_steal_pointer(&key));
                base = g_strdup_printf("-device %s,id=pipe,%s,product=%s",
                                       device, where, name);
                qts = start(l, base);
                {
                    g_autofree char *path = pipe_path(qts, device);
                    g_autofree char *given = qom_text(qts, path, "product");

                    g_assert_cmpstr(given, ==, name);
                }
                qtest_quit(qts);
                QLIST_FOREACH_ENTRY(qdict_get_qlist(product, "settings"), v) {
                    QDict *setting = qobject_to(QDict, qlist_entry_obj(v));
                    const QListEntry *value;

                    QLIST_FOREACH_ENTRY(qdict_get_qlist(setting, "values"),
                                        value) {
                        QDict *one = qobject_to(QDict,
                                                qlist_entry_obj(value));
                        g_autofree char *line = NULL;

                        if (g_str_equal(qdict_get_str(one, "value"),
                                        qdict_get_str(setting, "default"))) {
                            continue;
                        }
                        line = g_strdup_printf("%s,%s=%s", base,
                                               qdict_get_str(setting, "name"),
                                               qdict_get_str(one, "value"));
                        qts = start(l, line);
                        qtest_quit(qts);
                    }
                }
            }
        }
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

/*
 * The quad Tezro and the Onyx 350 take an optional V12 in graphics slot
 * widget-12 on the IP53 PIC fabric and fit none by default: the port then
 * reports no widget. A V12 -device there answers its identity at widget
 * 12, shows present and alive on the PXBOW port across a reset, and
 * raises its interrupt through the node's Bedrock PI.
 */
static void test_ip53_v12(void)
{
    static const char *const topologies[] = {
        "onyx350-compat", "tezro-tower-multi",
    };
    const uint64_t vpro = SPACE_IO + UINT64_C(12) * UINT64_C(0x1000000);
    const uint64_t bedrock = SPACE_IO + UINT64_C(0x01800000);
    const uint64_t bit = UINT64_C(1) << 5;

    for (size_t i = 0; i < G_N_ELEMENTS(topologies); i++) {
        const Launch *l = launch_for(topologies[i]);
        QTestState *qts = start(l, "");
        g_autofree char *type = qom_type(qts, "/machine/module0/widget-12");
        g_autofree char *product = NULL;

        g_assert_null(type);
        g_assert_cmphex(qtest_readl(qts, vpro + 0x0004), !=,
                        UINT32_C(0xac013047));
        g_assert_cmphex(qtest_readl(qts, SPACE_IO + IP53_XBOW_LINK_AUX(12)) &
                        IP53_XBOW_AUX_PRESENT, ==, 0);
        g_assert_cmphex(qtest_readl(qts,
                                    SPACE_IO + IP53_XBOW_LINK_STATUS(12)) &
                        IP53_XBOW_LINK_ALIVE, ==, 0);
        qtest_quit(qts);

        qts = start(l, "-device sgi-odyssey,slot=widget-12,product=v12");
        product = qom_text(qts, "/machine/module0/widget-12", "product");
        g_assert_cmpstr(product, ==, "v12");
        g_assert_cmphex(qtest_readl(qts, vpro + 0x0004), ==,
                        UINT32_C(0xac013047));
        g_assert_cmphex(qtest_readl(qts, vpro + 0x0024) & 0xf, ==, 12);
        for (int reset = 0; reset < 2; reset++) {
            if (reset) {
                qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
            }
            g_assert_cmphex(qtest_readl(qts,
                                        SPACE_IO + IP53_XBOW_LINK_AUX(12)) &
                            IP53_XBOW_AUX_PRESENT, ==, IP53_XBOW_AUX_PRESENT);
            g_assert_cmphex(qtest_readl(qts,
                                        SPACE_IO + IP53_XBOW_LINK_STATUS(12)) &
                            IP53_XBOW_LINK_ALIVE, ==, IP53_XBOW_LINK_ALIVE);
        }
        qtest_writel(qts, vpro + 0x0034,
                     (5u << 24) | (10u << 16) |
                     (uint32_t)(((UINT64_C(1) << 47) |
                                 UINT64_C(0x01800000) | 0x90) >> 32));
        qtest_writel(qts, vpro + 0x003c,
                     (uint32_t)((UINT64_C(1) << 47) |
                                UINT64_C(0x01800000) | 0x90));
        g_assert_cmphex(qtest_readq(qts, bedrock + 0x98) & bit, ==, 0);
        qtest_writel(qts, vpro + 0x1034, 0x200);
        g_assert_cmphex(qtest_readq(qts, bedrock + 0x98) & bit, ==, bit);
        qtest_writel(qts, vpro + 0x101c, 0x200);
        g_assert_cmphex(qtest_readq(qts, bedrock + 0x98) & bit, ==, 0);
        qtest_quit(qts);
    }
}

int main(int argc, char **argv)
{
    QTestState *qts;
    QDict *reply;
    int result;

    g_test_init(&argc, &argv, NULL);
    octane_flash = temporary("sgi-pipes-flash-XXXXXX", 4, 0xff);
    sn1_prom = temporary("sgi-pipes-prom-XXXXXX.bin", PROM_SIZE, 0);
    ip53_data = temporary("sgi-pipes-ip53-data-XXXXXX", 0x800, 0);
    ip53_clock = temporary("sgi-pipes-ip53-clock-XXXXXX", 16, 0);

    qts = qtest_init("-M none");
    reply = qtest_qmp(qts, "{'execute':'query-sgi-machines'}");
    catalogue = qobject_ref(qdict_get_qdict(reply, "return"));
    qobject_unref(reply);
    qtest_quit(qts);

    qtest_add_func("/sgi-graphics-pipes/catalogue", test_catalogue_records);
    qtest_add_func("/sgi-graphics-pipes/device-replaces-default",
                   test_device_replaces_default);
    qtest_add_func("/sgi-graphics-pipes/vga-none", test_vga_none);
    qtest_add_func("/sgi-graphics-pipes/vga-other", test_vga_other_refused);
    qtest_add_func("/sgi-graphics-pipes/refusals", test_refusals);
    qtest_add_func("/sgi-graphics-pipes/ip53-v12", test_ip53_v12);
    for (size_t i = 0; i < G_N_ELEMENTS(launches); i++) {
        g_autofree char *path = g_strdup_printf(
            "/sgi-graphics-pipes/default/%s", launches[i].topology);
        g_autofree char *offered = g_strdup_printf(
            "/sgi-graphics-pipes/offered/%s", launches[i].topology);

        qtest_add_data_func(path, &launches[i], test_default);
        qtest_add_data_func(offered, &launches[i], test_offered);
    }
    result = g_test_run();
    qobject_unref(catalogue);
    unlink(octane_flash);
    unlink(sn1_prom);
    unlink(ip53_data);
    unlink(ip53_clock);
    g_free(octane_flash);
    g_free(sn1_prom);
    g_free(ip53_data);
    g_free(ip53_clock);
    return result;
}
