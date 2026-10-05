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
 * Every one-wire part is included, the Hub's module-identity responders
 * too. The scheme's values are repeated here as literals, not taken from
 * the header that defines them.
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
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

#define ONEWIRE_PREFIX     UINT64_C(0x51454d55)
#define UNIQUEWARE_MARKER  UINT64_C(0x454d55)
#define COMPONENT_LETTERS  "NUIFSBXCRMGPOD"
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
} TopologyExtra;

static const TopologyExtra topology_extras[] = {
    { "fuel", FUEL_IDENTITY },
    { "fuel-vpro", FUEL_IDENTITY },
    { "origin300-pci-expansion-configuration-storage-experiment",
      STORAGE_BINDING, true },
    { "origin300-pci-expansion-pending-register-experiment",
      STORAGE_BINDING, true },
    { "origin300-pci-expansion-unconnected-uart-experiment",
      STORAGE_BINDING, true },
};

typedef struct MachineCase {
    char *name;
    char *args;            /* -M and its options, -cpu, -smp, -m, -device */
    bool ip35;             /* takes the IP35 PROM size */
    bool storage;          /* binds the expansion configuration storage */
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

static QDict *qom_get(QTestState *qts, const char *path, const char *name)
{
    QDict *response = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': {'path': %s, 'property': %s}}",
        path, name);

    g_assert(qdict_haskey(response, "return"));
    return response;
}

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *name)
{
    g_autoptr(QDict) response = qom_get(qts, path, name);

    return qdict_get_int(response, "return");
}

/* NULL for a string property left unset. */
static char *qom_get_str(QTestState *qts, const char *path, const char *name)
{
    g_autoptr(QDict) response = qom_get(qts, path, name);
    QString *value = qobject_to(QString, qdict_get(response, "return"));

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

static void check_object(QTestState *qts, Inventory *inv, const char *path,
                         QList *properties)
{
    bool serial = false, family = false, l1 = false;
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(prop, "name");

        if (!strcmp(name, "serial") &&
            !strcmp(qdict_get_str(prop, "type"), "uint64")) {
            serial = true;
        } else if (!strcmp(name, "family")) {
            family = true;
        } else if (!strcmp(name, "identity-index")) {
            l1 = true;
        } else if (!strcmp(name, "board-barcode") ||
                   !strcmp(name, "board-record-barcode")) {
            check_text(inv, path, name, qom_get_str(qts, path, name), true);
        } else if (!strcmp(name, "model-serial")) {
            check_text(inv, path, name, qom_get_str(qts, path, name), true);
        }
    }

    if (serial && family) {
        uint64_t value = qom_get_uint(qts, path, "serial");
        uint64_t code = qom_get_uint(qts, path, "family");
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

    if (l1) {
        g_autofree char *board = qom_get_str(qts, path, "node-board-name");

        if (board && *board) {
            char *key = g_strdup_printf(
                "%" PRIu64, qom_get_uint(qts, path, "identity-index"));
            const char *other = g_hash_table_lookup(inv->l1_instances, key);

            if (other) {
                g_error("%s and %s build board areas for instance %s",
                        other, path, key);
            }
            g_hash_table_insert(inv->l1_instances, key, g_strdup(path));
        }
    }
}

static void walk(QTestState *qts, Inventory *inv, const char *path)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute': 'qom-list', 'arguments': {'path': %s}}", path);
    QList *properties = qdict_get_qlist(response, "return");
    const QListEntry *entry;

    check_object(qts, inv, path, properties);
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_has_prefix(qdict_get_str(prop, "type"), "child<")) {
            g_autofree char *child = g_strdup_printf(
                "%s/%s", path, qdict_get_str(prop, "name"));

            walk(qts, inv, child);
        }
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
    g_autofree char *storage = NULL;
    g_autofree char *drive = NULL;
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
    prom = blank_image(c->ip35 ? IP35_PROM_SIZE : IP27_FLASH_SIZE);
    if (c->storage) {
        storage = blank_image(EXPANSION_STORAGE);
        drive = g_strdup_printf("-drive if=none,id=cfg,file=%s,format=raw",
                                storage);
    }
    qts = qtest_initf("-M %s -bios %s %s -display none -S", c->args, prom,
                      drive ? drive : "");
    walk(qts, &inv, "/machine");
    qtest_quit(qts);

    /*
     * Every machine has something to check: one-wire parts, or on a Fuel,
     * whose identity lives in its L1, the system board's areas.
     */
    g_assert_cmpuint(inv.parts + g_hash_table_size(inv.l1_instances), >, 0);
    g_hash_table_destroy(inv.roms);
    g_hash_table_destroy(inv.barcodes);
    g_hash_table_destroy(inv.l1_instances);
    g_unlink(prom);
    if (storage) {
        g_unlink(storage);
    }
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
        c->storage = extra && extra->storage;
        c->args = g_strdup_printf(
            "%s%s -cpu %s -smp %" PRId64 " -m %" PRId64 "M",
            qdict_get_str(offering, "machine-options"),
            extra ? extra->machine_options : "",
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
    .args = (char *)"origin2000,topology=origin2000-rack -cpu R10000 "
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
    return g_test_run();
}
