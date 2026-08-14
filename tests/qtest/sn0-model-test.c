/*
 * The Origin 200 machine, checked against the model it is supposed to be.
 *
 * The architecture contract says the configuration graph the firmware
 * synthesises and the devices the machine instantiates come from one topology
 * and cannot disagree. Until now nothing checked that: it held as long as
 * whoever edited the machine file remembered to edit the firmware builder to
 * match, and the failure it guards against is a guest that boots, discovers a
 * machine slightly unlike the one that was built, and dies somewhere unrelated
 * with nothing in the log.
 *
 * This is that check. Both descriptions are read back out of a running machine
 * and compared against the model they are both supposed to be projections of:
 *
 *   - the configuration graph, read straight out of node memory, field by
 *     field, against what tools/gen-klconfig generates from the model;
 *   - the object graph, walked over the monitor, against the devices the model
 *     says this machine has.
 *
 * The expectations are generated rather than written here, so this file cannot
 * drift from the model by being edited: see sn0-model.h, which
 * tools/check-model-header proves is what regenerating produces.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

#include "sn0-model.h"

/*
 * The graph sits in node memory, and the memory space is carrier window zero,
 * so a node offset is the address the memory system sees unchanged.
 */
static uint64_t read_field(QTestState *qts, uint64_t at, unsigned width)
{
    switch (width) {
    case 1:
        return qtest_readb(qts, at);
    case 2:
        return qtest_readw(qts, at);
    case 4:
        return qtest_readl(qts, at);
    case 8:
        return qtest_readq(qts, at);
    default:
        g_assert_not_reached();
    }
}

/*
 * Every field the model says the graph carries, read back from where the
 * machine actually put it. A mismatch names the record and field rather than
 * the address, because an address alone sends the reader hunting.
 */
static void test_config_graph(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sn0_model_graph); i++) {
        const SN0ModelField *f = &sn0_model_graph[i];
        uint64_t got = read_field(qts, f->offset, f->width);
        uint64_t want = f->value;
        const SN0ModelDivergence *excused = NULL;
        size_t d;

        /*
         * A field the model and the machine are known to disagree about. The
         * exception lives in the model, with a reason and an owner, and is
         * checked in both directions: if the machine has stopped diverging the
         * entry is stale and should go, so that too is a failure.
         */
        for (d = 0; sn0_model_divergences[d].field != NULL; d++) {
            if (sn0_model_divergences[d].offset == f->offset) {
                excused = &sn0_model_divergences[d];
                want = excused->machine_value;
                break;
            }
        }

        if (got != want) {
            g_test_message("%s at %#" PRIx64 ": %s says %#" PRIx64
                           ", machine has %#" PRIx64 "%s%s",
                           f->field, f->offset,
                           excused ? "the known divergence" : "model",
                           want, got,
                           excused ? " — owner: " : "",
                           excused ? excused->owner : "");
        }
        g_assert_cmphex(got, ==, want);
    }

    qtest_quit(qts);
}

/*
 * Every byte of the graph region, not only the fields the model names. A field
 * the model forgets and the machine also leaves zero would agree trivially, and
 * prove nothing; comparing the whole region instead catches anything the
 * machine writes that the model does not account for.
 *
 * Where the two are known to differ, the model says so and says who owns the
 * decision. Those exceptions are honoured here and are also checked in the
 * other direction, so an exception that has stopped being true fails the test
 * rather than sitting in the file forever.
 */
static void test_graph_whole_region(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);
    g_autofree uint8_t *got = g_malloc0(SN0_MODEL_GRAPH_SPAN);
    g_autofree uint8_t *want = g_memdup2(sn0_model_graph_image,
                                         SN0_MODEL_GRAPH_SPAN);
    g_autofree bool *excused = g_malloc0(SN0_MODEL_GRAPH_SPAN * sizeof(bool));
    size_t i;
    uint64_t off;

    qtest_memread(qts, SN0_MODEL_GRAPH_BASE, got, SN0_MODEL_GRAPH_SPAN);

    /*
     * A read that quietly returned nothing would make every comparison below
     * succeed against a zeroed buffer. The graph's signature is the cheapest
     * proof that the read worked at all, and this test is worthless without it.
     */
    g_assert_cmpuint(got[4], ==, 0xbe);

    for (i = 0; sn0_model_divergences[i].field != NULL; i++) {
        const SN0ModelDivergence *d = &sn0_model_divergences[i];
        size_t at = d->offset - SN0_MODEL_GRAPH_BASE;
        uint64_t machine = 0;
        unsigned b;

        for (b = 0; b < d->width; b++) {
            machine = (machine << 8) | got[at + b];
            excused[at + b] = true;
        }
        /*
         * The exception has to still be earning its place. If the machine has
         * stopped diverging, the entry is stale and the model should drop it.
         */
        if (machine != d->machine_value) {
            g_test_message("the model excuses %s as %#" PRIx64
                           " (owner: %s), but the machine now has %#" PRIx64
                           " — delete the known_divergences entry",
                           d->field, d->machine_value, d->owner, machine);
        }
        g_assert_cmphex(machine, ==, d->machine_value);
    }

    for (off = 0; off < SN0_MODEL_GRAPH_SPAN; off++) {
        if (excused[off] || got[off] == want[off]) {
            continue;
        }
        g_test_message("graph byte at %#" PRIx64 ": model says %#x, "
                       "machine has %#x — a byte the model does not account for",
                       SN0_MODEL_GRAPH_BASE + off, want[off], got[off]);
        g_assert_cmphex(got[off], ==, want[off]);
    }

    /* Everything past the populated span must be zero, in chunks. */
    for (off = SN0_MODEL_GRAPH_SPAN; off < SN0_MODEL_GRAPH_SIZE; off += 4096) {
        size_t len = MIN(4096, SN0_MODEL_GRAPH_SIZE - off);
        g_autofree uint8_t *tail = g_malloc0(len);
        size_t j;

        qtest_memread(qts, SN0_MODEL_GRAPH_BASE + off, tail, len);
        for (j = 0; j < len; j++) {
            if (tail[j] != 0) {
                g_test_message("graph byte at %#" PRIx64 " is %#x; the model "
                               "says the region ends at %#x",
                               SN0_MODEL_GRAPH_BASE + off + j, tail[j],
                               SN0_MODEL_GRAPH_SPAN);
            }
            g_assert_cmpuint(tail[j], ==, 0);
        }
    }

    qtest_quit(qts);
}

/* Collect every "type" in the object graph under one path, recursively. */
static void collect_types(QTestState *qts, const char *path, GHashTable *seen,
                          int depth)
{
    QDict *response;
    QList *children;
    const QListEntry *entry;

    /*
     * The tree is shallow. Running out of depth means it is not the tree this
     * test thinks it is, which is a failure rather than a reason to stop and
     * check whatever was collected so far.
     */
    g_assert_cmpint(depth, <=, 8);

    response = qtest_qmp(qts, "{ 'execute': 'qom-list',"
                              " 'arguments': { 'path': %s } }", path);
    /*
     * A path that came out of qom-list must answer qom-list. Treating a failure
     * here as "nothing to add" would let a missing subtree pass silently, which
     * is exactly the kind of hole that makes a green test meaningless.
     */
    if (!qdict_haskey(response, "return")) {
        g_test_message("qom-list failed for '%s'", path);
    }
    g_assert_true(qdict_haskey(response, "return"));
    children = qdict_get_qlist(response, "return");

    for (entry = qlist_first(children); entry; entry = qlist_next(entry)) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_try_str(child, "name");
        const char *type = qdict_get_try_str(child, "type");
        g_autofree char *sub = NULL;

        if (!name || !type) {
            continue;
        }
        /*
         * A link is a reference to an object that lives elsewhere in the tree,
         * so following it would walk the same objects twice and could loop.
         */
        if (g_str_has_prefix(type, "link<")) {
            continue;
        }
        if (g_str_has_prefix(type, "child<")) {
            g_autofree char *bare = g_strndup(type + 6, strlen(type) - 7);

            g_hash_table_add(seen, g_strdup(bare));
            sub = g_strdup_printf("%s/%s", path, name);
            collect_types(qts, sub, seen, depth + 1);
        }
    }
    qobject_unref(response);
}

/*
 * Every device the model says is built, found in the machine's own object
 * graph. This is the half of the contract the configuration graph cannot see:
 * the guest is told what the firmware says, and this is what the emulator
 * actually instantiated.
 */
static void test_object_graph(const void *data)
{
    QTestState *qts = qtest_initf("-M %s -m %dM",
                                  SN0_MODEL_MACHINE, SN0_MODEL_RAM_MB);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    QDict *response;
    const char *root;
    size_t i;

    /*
     * The machine itself, first. Without this the test would accept any machine
     * that happened to contain the right device types among its children.
     */
    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine',"
                              " 'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    root = qdict_get_str(response, "return");
    g_assert_cmpstr(root, ==, SN0_MODEL_MACHINE_TYPE);
    qobject_unref(response);

    collect_types(qts, "/machine", seen, 0);

    for (i = 0; i < ARRAY_SIZE(sn0_model_devices); i++) {
        const SN0ModelDevice *d = &sn0_model_devices[i];

        if (!g_hash_table_contains(seen, d->qom_type)) {
            g_test_message("the model says this machine has %s as '%s', "
                           "and the object graph has no such type",
                           d->name, d->qom_type);
        }
        g_assert_true(g_hash_table_contains(seen, d->qom_type));
    }

    g_hash_table_destroy(seen);
    qtest_quit(qts);
}

static QDict *find_qom_type(QList *types, const char *name)
{
    const QListEntry *entry;

    for (entry = qlist_first(types); entry; entry = qlist_next(entry)) {
        QDict *type = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(type, "name"), name)) {
            return type;
        }
    }
    return NULL;
}

/*
 * Product identity and shared architecture are separate QOM boundaries. The
 * command line still selects origin200, but its implementation derives from
 * an abstract SN0 machine and contains a node object that owns the Hub.
 */
static void test_architecture_boundaries(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");
    QDict *response;
    QList *types;
    QDict *type;

    response = qtest_qmp(qts, "{ 'execute': 'qom-list-types',"
                              " 'arguments': { 'implements': 'machine',"
                              "                  'abstract': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    types = qdict_get_qlist(response, "return");
    type = find_qom_type(types, "sn0-machine");
    g_assert_nonnull(type);
    g_assert_true(qdict_get_bool(type, "abstract"));
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-list-types',"
                              " 'arguments': { 'implements': 'sn0-machine',"
                              "                  'abstract': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    types = qdict_get_qlist(response, "return");
    type = find_qom_type(types, SN0_MODEL_MACHINE_TYPE);
    g_assert_nonnull(type);
    g_assert_false(qdict_haskey(type, "abstract") &&
                   qdict_get_bool(type, "abstract"));
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sn0-node");
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/hub',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sgi-hub");
    qobject_unref(response);

    response = qtest_qmp(qts, "{ 'execute': 'qom-get',"
                              " 'arguments': { 'path': '/machine/node0/cpu0',"
                              "                  'property': 'type' } }");
    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==,
                    "R10000-mips64-cpu");
    qobject_unref(response);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/model/config-graph", NULL, test_config_graph);
    qtest_add_data_func("sn0/model/graph-whole-region", NULL,
                        test_graph_whole_region);
    qtest_add_data_func("sn0/model/object-graph", NULL, test_object_graph);
    qtest_add_data_func("sn0/model/architecture-boundaries", NULL,
                        test_architecture_boundaries);

    return g_test_run();
}
