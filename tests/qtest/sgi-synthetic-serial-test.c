/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Every catalogued SGI machine's synthetic serials carry the emulator's
 * marker and are unique within the machine.
 *
 * A serial no real part supplied is invented, and an inventory listing
 * from the emulator must never pass for a physical machine's. For each
 * catalogued topology this walks the whole QOM tree and checks:
 *
 *  - every one-wire part's 48-bit ROM serial carries "QEMU" in its top 32
 *    bits, or, on a UniqueWare part, "EMU" below the 12-bit customer code,
 *    and no two parts share a ROM serial, even under different family
 *    codes;
 *  - every board-record barcode and module serial a device was given reads
 *    "EMU", a component letter and digits, and no two barcodes match;
 *  - every L1 that builds a node board's identity areas names its own
 *    instance, so no two nodes' board-area serials match.
 *
 * Every one-wire part is included, the module controllers' MODULEID and
 * midplane parts too. The scheme's values are repeated here as literals,
 * not taken from the header that defines them.
 *
 * The topologies come from the catalogue the binary carries
 * (query-sgi-machines), so a new one is checked without editing this file.
 * Each runs its largest offering: the most nodes, then the most
 * processors. A few need launch inputs the catalogue does not describe;
 * those are listed below, and an entry naming no catalogue topology fails
 * the test. One more case fits two MGRAS boards and a KONA pipe by slot
 * name, so per-slot graphics serials are checked as well.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-o2-product-fixture.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

#define ONEWIRE_PREFIX     UINT64_C(0x51454d55)
#define UNIQUEWARE_MARKER  UINT64_C(0x454d55)
#define COMPONENT_LETTERS  "NUIFLYSBXCRMGPOD"
#define IP35_PROM_SIZE     0x1686a8
#define IP27_FLASH_SIZE    (1024 * 1024)
#define EXPANSION_STORAGE  (16 * 1024 * 1024)

#define FUEL_IDENTITY \
    ",board-id-word=0x4abc,ioc3-subsystem-id=0xc30a,l1-type-code=0x41," \
    "bedrock-revision=0x2"
#define STORAGE_BINDING ",expansion-storage.0=expansion-enclosure=cfg"
#define BOOT_FLASH_IP35 (16 * 1024 * 1024)

/* Launch inputs a topology needs that its catalogue offering omits. */
typedef struct TopologyExtra {
    const char *topology;
    const char *machine_options;   /* appended to -M */
    bool storage;                  /* binds the expansion configuration */
    bool io9;                      /* needs IO9 timekeeper backends */
    bool unmarked;                 /* exposes no synthetic identity */
} TopologyExtra;

static const TopologyExtra topology_extras[] = {
    { "fuel", FUEL_IDENTITY },
    { "origin350", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "onyx350-compat", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "onyx4-host-serial-compat", ",ip53-experimental=on,"
      "nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-rack", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-tower-multi", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-tower-one", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "origin350-ip59", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "onyx350-ip59", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "onyx4-host-ip59", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-rack-ip59", ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-tower-multi-ip59",
      ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "tezro-tower-one-ip59",
      ",ip53-experimental=on,nvram0=sgi-ds1742-data-io9,"
      "nvram0-clock=sgi-ds1742-clock-io9", .io9 = true },
    { "origin300-numalink-pci-expansion",
      STORAGE_BINDING, true },
    { "origin300-pci-expansion", STORAGE_BINDING, true },
    /*
     * The Origin 3000 bricks carry no one-wire part and their L1
     * profiles synthesize their records, so nothing here is marked.
     */
    { "origin3200", "", .unmarked = true },
    { "origin3400", "", .unmarked = true },
    { "origin3900", "", .unmarked = true },
    /* The Origin 3200's bricks again, with a V12 beside them. */
    { "onyx3000", "", .unmarked = true },
};

typedef struct MachineCase {
    char *name;
    char *args;            /* -M and its options, -cpu, -smp, -m, -device */
    bool ip35;             /* takes the IP35 PROM size */
    bool ip32;             /* has explicit caller-owned ROM and identity */
    bool storage;          /* binds the expansion configuration storage */
    bool io9;              /* needs IO9 timekeeper backends */
    bool unmarked;         /* exposes no synthetic identity */
} MachineCase;

/* What one machine's walk collected. */
typedef struct Inventory {
    GHashTable *roms;          /* ROM serial -> path */
    GHashTable *barcodes;      /* barcode -> path */
    GHashTable *l1_instances;  /* identity-index -> path */
    unsigned int parts;
} Inventory;

static bool onewire_serial_marked(uint64_t serial)
{
    return (serial >> 16) == ONEWIRE_PREFIX ||
           ((serial >> 12) & 0xffffff) == UNIQUEWARE_MARKER;
}

static bool text_serial_marked(const char *text)
{
    size_t length = strlen(text);

    if (length < 6 || strncmp(text, "EMU", 3) ||
        !strchr(COMPONENT_LETTERS, text[3])) {
        return false;
    }
    for (size_t i = 4; i < length; i++) {
        if (!g_ascii_isdigit(text[i])) {
            return false;
        }
    }
    return true;
}

/*
 * The value of a property that qom-list-get listed. It skips a property
 * whose getter fails, so a missing value is an error here, as qom-get's
 * would have been.
 */
static QObject *prop_value(QDict *prop, const char *path)
{
    if (!qdict_haskey(prop, "value")) {
        g_error("%s property %s cannot be read", path,
                qdict_get_str(prop, "name"));
    }
    return qdict_get(prop, "value");
}

static uint64_t prop_uint(QDict *prop, const char *path)
{
    return qnum_get_uint(qobject_to(QNum, prop_value(prop, path)));
}

/* NULL for a string property left unset. */
static char *prop_str(QDict *prop, const char *path)
{
    QString *value = qobject_to(QString, prop_value(prop, path));

    return value ? g_strdup(qstring_get_str(value)) : NULL;
}

static void check_text(Inventory *inv, const char *path, const char *name,
                       char *text, bool unique)
{
    if (!text || !*text) {
        g_free(text);
        return;
    }
    g_test_message("%s %s %s", path, name, text);
    g_assert_true(text_serial_marked(text));
    if (unique) {
        const char *other = g_hash_table_lookup(inv->barcodes, text);

        if (other) {
            g_error("%s and %s share barcode %s", other, path, text);
        }
        g_hash_table_insert(inv->barcodes, text, g_strdup(path));
    } else {
        g_free(text);
    }
}

static QDict *find_prop(QList *properties, const char *name)
{
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

        if (!strcmp(qdict_get_str(prop, "name"), name)) {
            return prop;
        }
    }
    return NULL;
}

static void check_object(Inventory *inv, const char *path,
                         QList *properties)
{
    QDict *serial = find_prop(properties, "serial");
    QDict *family = find_prop(properties, "family");
    QDict *identity = find_prop(properties, "identity-index");
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(prop, "name");

        if (!strcmp(name, "board-barcode") ||
            !strcmp(name, "board-record-barcode") ||
            !strcmp(name, "model-serial")) {
            check_text(inv, path, name, prop_str(prop, path), true);
        }
    }

    if (serial && family && !strcmp(qdict_get_str(serial, "type"),
                                    "uint64")) {
        uint64_t value = prop_uint(serial, path);
        uint64_t code = prop_uint(family, path);
        /*
         * Keyed on the serial alone, not family and serial: the scheme
         * gives no two parts one serial, whatever their family codes.
         */
        char *key = g_strdup_printf("%012" PRIx64, value);
        const char *other = g_hash_table_lookup(inv->roms, key);

        g_test_message("%s family 0x%02" PRIx64 " serial 0x%012" PRIx64,
                       path, code, value);
        g_assert_true(onewire_serial_marked(value));
        if (other) {
            g_error("%s and %s share ROM serial %s", other, path, key);
        }
        g_hash_table_insert(inv->roms, key, g_strdup(path));
        inv->parts++;
    }

    if (identity) {
        QDict *board_prop = find_prop(properties, "node-board-name");
        g_autofree char *board = board_prop ? prop_str(board_prop, path)
                                            : NULL;

        if (board && *board) {
            char *key = g_strdup_printf("%" PRIu64,
                                        prop_uint(identity, path));
            const char *other = g_hash_table_lookup(inv->l1_instances, key);

            if (other) {
                g_error("%s and %s build board areas for instance %s",
                        other, path, key);
            }
            g_hash_table_insert(inv->l1_instances, key, g_strdup(path));
        }
    }
}

/*
 * Memory regions and interrupt lines make up nearly all of a machine's QOM
 * objects. Neither carries a part serial, a barcode or a board identity,
 * and every request byte costs the emulator two main-loop wakeups, so the
 * walk does not ask for their properties.
 */
static bool is_leaf_child(const char *type)
{
    return !strcmp(type, "child<memory-region>") ||
           !strcmp(type, "child<irq>");
}

/*
 * Check the whole QOM tree, one level per request. Asking for every
 * object's properties and values at once, instead of listing an object
 * and then reading each property it carries, costs a few round trips a
 * machine rather than thousands: each round trip waits for the emulator
 * to be scheduled, so the walk's time followed the host's load.
 */
static void walk(QTestState *qts, Inventory *inv, const char *root)
{
    g_autoptr(GPtrArray) level = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(level, g_strdup(root));
    while (level->len) {
        g_autoptr(GPtrArray) next = g_ptr_array_new_with_free_func(g_free);
        QList *paths = qlist_new();
        g_autoptr(QDict) response = NULL;
        QList *results;
        const QListEntry *entry;
        guint i = 0;

        for (guint n = 0; n < level->len; n++) {
            qlist_append_str(paths, g_ptr_array_index(level, n));
        }
        response = qtest_qmp(qts, "{'execute': 'qom-list-get', "
                             "'arguments': {'paths': %p}}", paths);
        g_assert_true(qdict_haskey(response, "return"));
        results = qdict_get_qlist(response, "return");
        g_assert_cmpuint(qlist_size(results), ==, level->len);
        QLIST_FOREACH_ENTRY(results, entry) {
            const char *path = g_ptr_array_index(level, i++);
            QList *properties = qdict_get_qlist(
                qobject_to(QDict, qlist_entry_obj(entry)), "properties");
            const QListEntry *p;

            check_object(inv, path, properties);
            QLIST_FOREACH_ENTRY(properties, p) {
                QDict *prop = qobject_to(QDict, qlist_entry_obj(p));

                if (g_str_has_prefix(qdict_get_str(prop, "type"),
                                     "child<") &&
                    !is_leaf_child(qdict_get_str(prop, "type"))) {
                    g_ptr_array_add(next, g_strdup_printf(
                        "%s/%s", path, qdict_get_str(prop, "name")));
                }
            }
        }
        g_ptr_array_free(level, TRUE);
        level = g_steal_pointer(&next);
    }
}

static char *blank_image(size_t size)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sgi-synthetic-serial-XXXXXX.bin", &path,
                             &error);

    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, size), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_machine_serials(const void *data)
{
    const MachineCase *c = data;
    g_autofree char *machine = g_strndup(c->args, strcspn(c->args, ", "));
    g_autofree char *prom = NULL;
    g_autofree char *prom_option = NULL;
    /* Caller bytes yield the marked ROM serial 0x51454d550001. */
    O2ProductTest product = { .serial = "0100554d4551" };
    g_autofree char *storage = NULL;
    g_autofree char *drive = NULL;
    g_autofree char *io9_data = NULL;
    g_autofree char *io9_clock = NULL;
    g_autofree char *io9_drives = NULL;
    Inventory inv = {
        .roms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      g_free),
        .barcodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_free),
        .l1_instances = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free),
    };
    QTestState *qts;

    if (!qtest_has_machine(machine)) {
        g_test_skip("machine not built in");
        return;
    }
    if (c->ip32) {
        o2_product_prepare(&product);
        prom_option = g_strdup_printf("-M %s",
                                      strchr(product.machine, ',') + 1);
    } else {
        prom = blank_image(c->ip35 ? IP35_PROM_SIZE : IP27_FLASH_SIZE);
        prom_option = g_strdup_printf("-bios %s", prom);
    }
    if (c->storage) {
        storage = blank_image(EXPANSION_STORAGE);
        drive = g_strdup_printf("-drive if=none,id=cfg,file=%s,format=raw",
                                storage);
    }
    if (c->io9) {
        io9_data = blank_image(0x8000);
        io9_clock = blank_image(16);
        io9_drives = g_strdup_printf(
            "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
            "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw",
            io9_data, io9_clock);
    }
    qts = qtest_initf("-M %s %s %s %s -display none -S", c->args, prom_option,
                      drive ? drive : "", io9_drives ? io9_drives : "");
    walk(qts, &inv, "/machine");
    if (c->ip32) {
        g_autoptr(QDict) reply = qtest_qmp(qts,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':'identity-test-serial'}}");

        g_assert_cmpstr(qdict_get_str(reply, "return"), ==, product.serial);
    }
    qtest_quit(qts);

    /*
     * Earlier profiles expose a synthetic one-wire or L1 board identity.
     * An IP53 IO9 profile can expose neither; the walk still checks any
     * matching identity properties it does expose. IP32's identity is
     * caller-provided rather than synthesized; check its input above.
     */
    if (!c->io9 && !c->unmarked) {
        g_assert_cmpuint(inv.parts + g_hash_table_size(inv.l1_instances),
                         >, 0);
    }
    g_hash_table_destroy(inv.roms);
    g_hash_table_destroy(inv.barcodes);
    g_hash_table_destroy(inv.l1_instances);
    if (c->ip32) {
        o2_product_cleanup(&product);
    } else {
        g_unlink(prom);
    }
    if (io9_data) {
        g_unlink(io9_data);
        g_unlink(io9_clock);
    }
    if (storage) {
        g_unlink(storage);
    }
}

/*
 * A machine property that cannot be read answers with an error, and a
 * property dump skips it. The "routes" diagnostic once dereferenced the
 * Hub array a Bedrock topology does not have and killed the emulator, so
 * dumping a two-node Origin 300's properties crashed it.
 */
static void test_routes_unavailable_on_bedrock(void)
{
    g_autofree char *prom = NULL;
    g_autoptr(QDict) response = NULL;
    QTestState *qts;

    if (!qtest_has_machine("origin300")) {
        g_test_skip("machine not built in");
        return;
    }
    prom = blank_image(IP35_PROM_SIZE);
    qts = qtest_initf("-M origin300,topology=origin300-two-chassis,nodes=2 "
                      "-cpu R14000 -smp 8 -m 1024M -bios %s -display none -S",
                      prom);
    response = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                         "{'path': '/machine', 'property': 'routes'}}");
    g_assert_true(qdict_haskey(response, "error"));
    qtest_quit(qts);
    g_unlink(prom);
}

static const TopologyExtra *topology_extra(const char *topology)
{
    for (size_t i = 0; i < G_N_ELEMENTS(topology_extras); i++) {
        if (g_str_equal(topology_extras[i].topology, topology)) {
            return &topology_extras[i];
        }
    }
    return NULL;
}

/* Whether @a has more nodes, or as many and more processors, than @b. */
static bool larger_offering(QDict *a, QDict *b)
{
    int64_t a_nodes = qdict_get_int(a, "nodes");
    int64_t b_nodes = qdict_get_int(b, "nodes");

    return a_nodes > b_nodes ||
           (a_nodes == b_nodes &&
            qdict_get_int(a, "smp") > qdict_get_int(b, "smp"));
}

static bool boot_flash_is_ip35(QDict *offering)
{
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(offering, "storage"), entry) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(item, "name"), "node0-flash")) {
            return qdict_get_int(item, "size") == BOOT_FLASH_IP35;
        }
    }
    return false;
}

static gint compare_names(gconstpointer a, gconstpointer b, gpointer unused)
{
    return strcmp(a, b);
}

/* One case per catalogue topology, from its largest offering. */
static GPtrArray *catalogue_cases(void)
{
    QTestState *qts = qtest_init("-M none");
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute': 'query-sgi-machines'}");
    QList *offerings = qdict_get_qlist(qdict_get_qdict(reply, "return"),
                                       "offerings");
    g_autoptr(GHashTable) largest = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GList) topologies = NULL;
    GPtrArray *cases = g_ptr_array_new();
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(offerings, entry) {
        QDict *offering = qobject_to(QDict, qlist_entry_obj(entry));
        const char *topology = qdict_get_str(offering, "topology");
        QDict *best = g_hash_table_lookup(largest, topology);

        if (!best || larger_offering(offering, best)) {
            g_hash_table_insert(largest, (gpointer)topology, offering);
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(topology_extras); i++) {
        if (!g_hash_table_contains(largest, topology_extras[i].topology)) {
            g_error("launch extra names topology %s, which the catalogue "
                    "does not offer", topology_extras[i].topology);
        }
    }
    topologies = g_list_sort_with_data(g_hash_table_get_keys(largest),
                                       compare_names, NULL);
    for (GList *t = topologies; t; t = t->next) {
        QDict *offering = g_hash_table_lookup(largest, t->data);
        const TopologyExtra *extra = topology_extra(t->data);
        QDict *memory = qdict_get_qdict(offering, "memory-per-node-mib");
        MachineCase *c = g_new0(MachineCase, 1);

        c->name = g_strdup(t->data);
        c->ip35 = boot_flash_is_ip35(offering);
        c->ip32 = g_str_equal(qdict_get_str(offering, "product"), "o2");
        c->storage = extra && extra->storage;
        c->io9 = extra && extra->io9;
        c->unmarked = extra && extra->unmarked;
        c->args = g_strdup_printf(
            "%s%s -cpu %s -smp %" PRId64 " -m %" PRId64 "M",
            qdict_get_str(offering, "machine-options"),
            extra ? extra->machine_options : "",
            c->ip32 && g_str_equal(c->name, "o2plus") ? O2PLUS_CPU :
                qdict_get_str(offering, "default-cpu-model"),
            qdict_get_int(offering, "smp"),
            qdict_get_int(memory, "default") *
            qdict_get_int(offering, "nodes"));
        g_ptr_array_add(cases, c);
    }
    qtest_quit(qts);
    g_assert_cmpuint(cases->len, >, 0);
    return cases;
}

/*
 * Graphics boards fitted by slot name: each takes its own instance from
 * the slot, so two MGRAS boards and a KONA pipe in one machine do not share
 * a part serial or a barcode.
 */
static MachineCase slot_graphics_case = {
    .name = (char *)"origin2000-slot-graphics",
    .args = (char *)"origin2000,topology=origin2000-rack,"
            "x-uncatalogued-graphics=on -cpu R10000 "
            "-smp 8 -m 256M "
            "-device sgi-mgras,slot=io2,board-record=on "
            "-device sgi-mgras,slot=io5,board-record=on "
            "-device sgi-kona,slot=io4",
};

int main(int argc, char **argv)
{
    GPtrArray *cases;

    g_test_init(&argc, &argv, NULL);
    cases = catalogue_cases();
    g_ptr_array_add(cases, &slot_graphics_case);
    for (guint i = 0; i < cases->len; i++) {
        MachineCase *c = g_ptr_array_index(cases, i);
        g_autofree char *path = g_strdup_printf(
            "sgi/synthetic-serial/%s", c->name);

        qtest_add_data_func(path, c, test_machine_serials);
    }
    qtest_add_func("sgi/synthetic-serial/routes-unavailable-on-bedrock",
                   test_routes_unavailable_on_bedrock);
    return g_test_run();
}
