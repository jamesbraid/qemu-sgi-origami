/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Launch every offering query-sgi-machines lists, by its id, from its
 * catalogue data: machine-options, storage bindings, processor, CPU count
 * and memory.
 * The IP32 recipe supplies original synthetic ROM and identity inputs and
 * the selected CPU tuple. All launches use -nodefaults. A topology with
 * graphics slots is launched again with a pipe in its first slot and the
 * graphics record's machine-properties. Any other unadvertised required
 * input fails here, as does an unlisted SCSI adapter the machine builds.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-o2-product-fixture.h"
#include "sgi-test.h"
#include "sgi-l1-l2-stub.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

static QDict *catalogue;
static char *directory;

typedef struct Launch {
    QDict *offering;
    /* With a pipe from the topology's graphics record. */
    bool pipe;
    /*
     * With a backend on every listed network, each by a -net nic naming
     * no model, rather than one on the default network only.
     */
    bool all_networks;
} Launch;

static QDict *topology_record(const char *name)
{
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(catalogue, "topologies"), e) {
        QDict *t = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(t, "name"), name)) {
            return t;
        }
    }
    g_assert_not_reached();
}

/* A zeroed file for storage item @name of @size bytes, named per test. */
static char *make_store(const char *tag, const char *name, uint64_t size)
{
    char *path = g_strdup_printf("%s/%s-%s.raw", directory, tag, name);
    g_autofree char *zeroes = g_malloc0(size);

    g_assert_true(g_file_set_contents(path, zeroes, size, NULL));
    return path;
}

/* "slot=io3" or "bus=pci,addr=5" from a slot's placement list. */
static void append_properties(GString *out, QList *properties)
{
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(properties, e) {
        QDict *p = qobject_to(QDict, qlist_entry_obj(e));

        g_string_append_printf(out, ",%s=%s", qdict_get_str(p, "name"),
                               qdict_get_str(p, "value"));
    }
}

/*
 * The first slot of @graphics that exists at @nodes, and the -device for
 * its first family's first product, which takes every setting's default.
 */
static char *first_pipe(QDict *graphics, int64_t nodes)
{
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(graphics, "slots"), e) {
        QDict *slot = qobject_to(QDict, qlist_entry_obj(e));
        QDict *requires = qdict_get_qdict(slot, "requires");
        QDict *family = qobject_to(QDict, qlist_peek(
            qdict_get_qlist(slot, "families")));
        QDict *product = qobject_to(QDict, qlist_peek(
            qdict_get_qlist(family, "products")));
        GString *device;

        if (requires && qdict_haskey(requires, "min-nodes") &&
            qdict_get_int(requires, "min-nodes") > nodes) {
            continue;
        }
        device = g_string_new(qdict_get_str(family, "device"));
        append_properties(device, qdict_get_qlist(slot, "placement"));
        g_string_append_printf(device, ",product=%s",
                               qdict_get_str(product, "product"));
        return g_string_free(device, false);
    }
    return NULL;
}

/*
 * Whether @line is the reply to a QMP command, as opposed to the greeting
 * or an event.
 */
static bool is_reply(const char *line)
{
    return g_str_has_prefix(line, "{\"return\"") ||
           g_str_has_prefix(line, "{\"error\"");
}

/*
 * Run QEMU with @args (NULL-terminated) and the QMP @commands, one per
 * line, after qmp_capabilities and before quit. Returns its exit status;
 * @out and @err hold what it printed.
 *
 * quit goes out only once every reply has been read. QEMU drops the
 * monitor output still queued when it exits, so a quit sent with the
 * commands loses the tail of a long reply (the device tree of an Origin 3800
 * is over 64 KiB) whenever this process reads the pipe slower than QEMU
 * fills it.
 */
static int run_qemu(GPtrArray *args, const char *commands, char **out,
                    char **err)
{
    static const char quit[] = "{\"execute\":\"quit\"}\n";
    g_autofree char *input = g_strdup_printf(
        "{\"execute\":\"qmp_capabilities\"}\n%s", commands);
    g_autoptr(GString) text = g_string_new(NULL);
    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
    unsigned int expected = 0, replies = 0;
    GPid pid;
    int input_fd, output_fd, error_fd, status;
    GIOChannel *channel;
    gsize length;
    ssize_t written;

    for (const char *c = input; *c; c++) {
        expected += *c == '\n';
    }
    g_assert_true(g_spawn_async_with_pipes(NULL, (char **)args->pdata, NULL,
                    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &input_fd,
                    &output_fd, &error_fd, NULL));
    /*
     * A refused launch exits before it reads its input, so a short write
     * is no failure; the exit status is the result.
     */
    sigemptyset(&ignore.sa_mask);
    g_assert_cmpint(sigaction(SIGPIPE, &ignore, &previous), ==, 0);
    written = qemu_write_full(input_fd, input, strlen(input));
    g_assert_cmpint(written, <=, strlen(input));
    channel = g_io_channel_unix_new(output_fd);
    while (replies < expected) {
        g_autofree char *line = NULL;
        GIOStatus st = g_io_channel_read_line(channel, &line, &length, NULL,
                                              NULL);

        if (st == G_IO_STATUS_EOF) {
            break;
        }
        g_assert_cmpint(st, ==, G_IO_STATUS_NORMAL);
        g_string_append_len(text, line, length);
        replies += is_reply(line);
    }
    written = qemu_write_full(input_fd, quit, strlen(quit));
    g_assert_cmpint(written, <=, strlen(quit));
    g_assert_cmpint(sigaction(SIGPIPE, &previous, NULL), ==, 0);
    close(input_fd);
    {
        g_autofree char *rest = NULL;

        g_assert_cmpint(g_io_channel_read_to_end(channel, &rest, &length,
                                                 NULL),
                        ==, G_IO_STATUS_NORMAL);
        g_string_append_len(text, rest, length);
    }
    g_io_channel_unref(channel);
    close(output_fd);
    *out = g_string_free(g_steal_pointer(&text), false);
    channel = g_io_channel_unix_new(error_fd);
    g_assert_cmpint(g_io_channel_read_to_end(channel, err, &length, NULL),
                    ==, G_IO_STATUS_NORMAL);
    g_io_channel_unref(channel);
    close(error_fd);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    g_spawn_close_pid(pid);
    g_assert_true(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* The device tree, which names every SCSI bus the machine built. */
#define QTREE "{\"execute\":\"human-monitor-command\",\"arguments\":" \
    "{\"command-line\":\"info qtree\"}}\n"

/*
 * A launcher attaches disks only to the SCSI buses the offering lists, so
 * each listed bus must exist, and a machine that builds a QLogic adapter
 * must list at least one. @out holds the QTREE reply, whose text escapes
 * each line end as \r\n.
 */
static void check_scsi_adapters(QDict *o, const char *machine,
                                const char *out)
{
    QList *adapters = qdict_get_qlist(o, "scsi-adapters");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(adapters, e) {
        QDict *adapter = qobject_to(QDict, qlist_entry_obj(e));
        g_autofree char *line = g_strdup_printf(
            "bus: %s\\r\\n", qdict_get_str(adapter, "bus"));

        if (!strstr(out, line)) {
            g_printerr("%s lacks expected SCSI bus %s\nQMP stdout:\n%s\n",
                       machine, qdict_get_str(adapter, "bus"), out);
        }
        g_assert_nonnull(strstr(out, line));
    }
    if (strstr(out, "dev: qlogic-isp") && qlist_empty(adapters)) {
        g_test_message("%s builds a QLogic SCSI adapter it does not list",
                       machine);
        g_assert_false(qlist_empty(adapters));
    }
}

/* Each NIC, its model, its address and its backend. */
#define NETWORK "{\"execute\":\"human-monitor-command\",\"arguments\":" \
    "{\"command-line\":\"info network\"}}\n"

/*
 * Bind a user-mode backend to the offering's default network the way a
 * launcher does from the catalogue alone: a -net nic naming the entry's
 * model, and where the address is derived, the derived default through
 * the entry's option, which QEMU must accept. With @all, bind every
 * listed network instead, in catalogue order, each by a -net nic naming
 * no model, which QEMU hands to the onboard NICs in that same order.
 */
static void append_networks(GPtrArray *args, QDict *o, bool all)
{
    const QListEntry *e;
    unsigned int i = 0;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "networks"), e) {
        QDict *n = qobject_to(QDict, qlist_entry_obj(e));
        QDict *mac = qdict_get_qdict(n, "mac");

        if (all) {
            g_ptr_array_add(args, g_strdup("-netdev"));
            g_ptr_array_add(args,
                            g_strdup_printf("user,id=catalogue-net%u", i));
            g_ptr_array_add(args, g_strdup("-net"));
            g_ptr_array_add(args,
                            g_strdup_printf("nic,netdev=catalogue-net%u", i));
            i++;
            continue;
        }
        if (!qdict_get_bool(n, "default")) {
            continue;
        }
        g_ptr_array_add(args, g_strdup("-netdev"));
        g_ptr_array_add(args, g_strdup("user,id=catalogue-net"));
        g_ptr_array_add(args, g_strdup("-net"));
        g_ptr_array_add(args, qdict_get_bool(mac, "derived") ?
            g_strdup_printf("nic,model=%s,netdev=catalogue-net,%s=%s",
                            qdict_get_str(n, "nic-model"),
                            qdict_get_str(n, "nic-mac-option"),
                            qdict_get_str(mac, "default-address")) :
            g_strdup_printf("nic,model=%s,netdev=catalogue-net",
                            qdict_get_str(n, "nic-model")));
    }
}

/*
 * Every onboard Ethernet the offering lists exists with the model it
 * names and, where derived, its address, and the default one carries
 * the backend bound above. With @all, each derived NIC carries the
 * backend bound for its own catalogue position. @out holds the NETWORK
 * reply, whose text escapes each line end as \r\n and a backslash as
 * \\.
 */
static void check_networks(QDict *o, const char *machine, const char *out,
                           bool all)
{
    const QListEntry *e;
    unsigned int i = 0;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "networks"), e) {
        QDict *n = qobject_to(QDict, qlist_entry_obj(e));
        QDict *mac = qdict_get_qdict(n, "mac");
        bool derived = qdict_get_bool(mac, "derived");
        g_autofree char *nic = derived ?
            g_strdup_printf("model=%s,macaddr=%s",
                            qdict_get_str(n, "nic-model"),
                            qdict_get_str(mac, "default-address")) :
            g_strdup_printf("model=%s,", qdict_get_str(n, "nic-model"));
        g_autofree char *bound = all && derived ?
            g_strdup_printf("%s\\r\\n \\\\ catalogue-net%u: ", nic, i) :
            g_strdup(nic);

        if (!strstr(out, bound)) {
            g_test_message("%s has no NIC %s in %s", machine, bound, out);
        }
        g_assert_nonnull(strstr(out, bound));
        i++;
    }
    if (!all && !qlist_empty(qdict_get_qlist(o, "networks"))) {
        g_assert_nonnull(strstr(out, "catalogue-net: index="));
    }
}

/*
 * The offering's processor part, CPU count and memory, as the catalogue
 * lists them.
 */
static void append_cpu_memory(GPtrArray *args, QDict *o)
{
    int64_t nodes = qdict_get_int(o, "nodes");
    QDict *memory = qdict_get_qdict(o, "memory-per-node-mib");
    int64_t ram_mib;

    g_ptr_array_add(args, g_strdup("-cpu"));
    /* The bare part: the O2+ board fits its module's values itself. */
    g_ptr_array_add(args, g_strdup(qdict_get_str(o, "default-cpu-model")));
    g_ptr_array_add(args, g_strdup("-smp"));
    g_ptr_array_add(args, g_strdup_printf("%" PRId64,
                                          qdict_get_int(o, "smp")));
    /*
     * From 16 GiB up (the Origin 3400 and 3800 offerings) a builder host
     * cannot back one plain -m reservation, so those launches map their
     * RAM unreserved.
     */
    ram_mib = qdict_get_int(memory, "default") * nodes;
    if (ram_mib >= 16 * 1024) {
        g_autofree char *ram =
            g_strdup_printf(SGI_UNRESERVED_RAM("%" PRId64 "M"), ram_mib);
        g_auto(GStrv) words = g_strsplit(ram, " ", 0);

        for (char **w = words; *w; w++) {
            g_ptr_array_add(args, g_strdup(*w));
        }
    } else {
        g_ptr_array_add(args, g_strdup("-m"));
        g_ptr_array_add(args, g_strdup_printf("%" PRId64 "M", ram_mib));
    }
}

static void test_launch(const void *opaque)
{
    const Launch *l = opaque;
    QDict *o = l->offering;
    const char *topology = qdict_get_str(o, "topology");
    int64_t nodes = qdict_get_int(o, "nodes");
    QDict *graphics = qdict_get_qdict(topology_record(topology), "graphics");
    QDict *machine_properties = graphics ?
        qdict_get_qdict(graphics, "machine-properties") : NULL;
    /*
     * Every offering launches as the catalogue gives it, with no -cpu
     * properties: an IP53 runtime profile and the Origin 3900's Cx-brick
     * node board both fit their R16000s themselves.
     */
    bool ip32 = g_str_equal(qdict_get_str(o, "product"), "o2");
    /*
     * An Origin 2000 with a pipe needs a keyboard for its X server, which
     * only the BaseIO-G's MediaIO carries: the catalogue launch must
     * build it, whatever the catalogue's record says.
     */
    bool mediaio = l->pipe &&
                   g_str_equal(qdict_get_str(o, "product"), "origin2000");
    g_autofree char *tag = g_strdup_printf(
        "%s-%s", qdict_get_str(o, "id"), l->pipe ? "pipe" : "bare");
    g_autoptr(GString) machine = g_string_new(
        qdict_get_str(o, "machine-options"));
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *out = NULL, *err = NULL;
    const QListEntry *e;
    int status;
    O2ProductTest product = { 0 };

    if (ip32) {
        o2_product_prepare(&product);
        g_string_append(machine, strchr(product.machine, ','));
    }
    g_ptr_array_add(args, g_strdup(qtest_qemu_binary(NULL)));
    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "storage"), e) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(e));
        const char *name = qdict_get_str(item, "name");
        char *path = make_store(tag, name, qdict_get_int(item, "size"));

        g_string_append_printf(machine, ",%s=%s", name, name);
        g_ptr_array_add(args, g_strdup("-blockdev"));
        g_ptr_array_add(args, g_strdup_printf(
            "driver=raw,node-name=%s,file.driver=file,file.filename=%s%s",
            name, path, qdict_get_bool(item, "read-only") ?
            ",read-only=on" : ""));
        g_ptr_array_add(files, path);
    }
    if (l->pipe) {
        char *device = first_pipe(graphics, nodes);

        g_assert_nonnull(device);
        if (machine_properties) {
            append_properties(machine, qdict_get_qlist(machine_properties,
                                                       "properties"));
        }
        g_ptr_array_add(args, g_strdup("-device"));
        g_ptr_array_add(args, device);
    }
    /*
     * The machine derives its own address when mac= is not given, so a
     * launcher that wants the catalogue's listed addresses names the
     * first one, and every derived NIC follows by its offset.
     */
    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "networks"), e) {
        QDict *n = qobject_to(QDict, qlist_entry_obj(e));
        QDict *mac = qdict_get_qdict(n, "mac");

        if (qdict_get_bool(mac, "derived") &&
            qdict_get_int(mac, "offset") == 0) {
            g_string_append_printf(machine, ",%s=%s",
                                   qdict_get_str(mac, "machine-property"),
                                   qdict_get_str(mac, "default-address"));
            break;
        }
    }
    g_ptr_array_add(args, g_strdup("-M"));
    g_ptr_array_add(args, g_strdup(machine->str));
    append_cpu_memory(args, o);
    for (const char *const *word = (const char *const[]) {
             "-nodefaults", "-display", "none", "-S", "-accel", "qtest",
             "-qmp", "stdio", NULL }; *word; word++) {
        g_ptr_array_add(args, g_strdup(*word));
    }
    append_networks(args, o, l->all_networks);
    g_ptr_array_add(args, NULL);

    status = run_qemu(args, mediaio ?
        "{\"execute\":\"qom-get\",\"arguments\":{\"path\":"
        "\"/machine/module0/io1/baseio/mio-ioc3/kbc\","
        "\"property\":\"type\"}}\n" QTREE NETWORK : QTREE NETWORK,
        &out, &err);
    for (guint i = 0; i < files->len; i++) {
        unlink(g_ptr_array_index(files, i));
    }
    if (ip32) {
        o2_product_cleanup(&product);
    }
    if (status) {
        g_test_message("%s refused: %s", machine->str, err);
    }
    g_assert_cmpint(status, ==, 0);
    check_scsi_adapters(o, machine->str, out);
    check_networks(o, machine->str, out, l->all_networks);
    /* The qom-get answers with the controller's type, not an error. */
    if (mediaio) {
        if (strstr(out, "\"error\"")) {
            g_test_message("%s has no MediaIO keyboard controller: %s",
                           machine->str, out);
        }
        g_assert_null(strstr(out, "\"error\""));
    }
}

/*
 * The l2-link lines of an offering, applied as a launcher would with an L2
 * peer for each: every C-brick L1 reaches the peer on its own line and no
 * other. An optional link (the Origin 3200's) is backed as if its setting
 * were on.
 */
static void test_l2_links(const void *opaque)
{
    const Launch *l = opaque;
    QDict *o = l->offering;
    g_autoptr(GString) machine = g_string_new(
        qdict_get_str(o, "machine-options"));
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) stubs = g_ptr_array_new();
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *command = NULL;
    /* Any image will do: the L1s connect before the PROM runs. */
    g_autofree char *prom = make_store(qdict_get_str(o, "id"), "prom",
                                       1476264);
    const QListEntry *e;
    QTestState *qts;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "serial-lines"), e) {
        QDict *line = qobject_to(QDict, qlist_entry_obj(e));
        L2Stub *stub;

        if (!g_str_equal(qdict_get_str(line, "kind"), "l2-link")) {
            continue;
        }
        stub = l2_stub_listen();
        g_ptr_array_add(stubs, stub);
        g_ptr_array_add(paths, g_strdup_printf(
            "/machine/node%" PRId64 "/l1", qdict_get_int(line, "node")));
        g_ptr_array_add(args, g_strdup("-chardev"));
        g_ptr_array_add(args, g_strdup_printf(
            "socket,id=%s,path=%s", qdict_get_str(line, "chardev"),
            stub->path));
        g_string_append_printf(machine, ",%s=%" PRId64 "=%s",
                               qdict_get_str(line, "property"),
                               qdict_get_int(line, "controller"),
                               qdict_get_str(line, "chardev"));
    }
    g_assert_cmpuint(stubs->len, >, 0);
    g_ptr_array_add(args, g_strdup("-M"));
    g_ptr_array_add(args, g_strdup(machine->str));
    append_cpu_memory(args, o);
    g_ptr_array_add(args, g_strdup("-bios"));
    g_ptr_array_add(args, g_strdup(prom));
    g_ptr_array_add(args, g_strdup("-nodefaults"));
    g_ptr_array_add(args, g_strdup("-display"));
    g_ptr_array_add(args, g_strdup("none"));
    g_ptr_array_add(args, NULL);
    command = g_strjoinv(" ", (char **)args->pdata);
    qts = qtest_init(command);
    for (guint i = 0; i < stubs->len; i++) {
        l2_stub_accept(g_ptr_array_index(stubs, i));
    }
    for (guint i = 0; i < stubs->len; i++) {
        const char *path = g_ptr_array_index(paths, i);

        for (unsigned n = 0; n < 10000 &&
             !qtest_qom_get_bool(qts, path, "l2-link-up"); n++) {
            g_usleep(1000);
        }
        g_assert_true(qtest_qom_get_bool(qts, path, "l2-link-up"));
    }
    qtest_quit(qts);
    unlink(prom);
    for (guint i = 0; i < stubs->len; i++) {
        l2_stub_free(g_ptr_array_index(stubs, i));
    }
}

/* How many l2-link lines the offering lists. */
static unsigned offering_l2_links(QDict *o)
{
    const QListEntry *e;
    unsigned count = 0;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(o, "serial-lines"), e) {
        QDict *line = qobject_to(QDict, qlist_entry_obj(e));

        count += g_str_equal(qdict_get_str(line, "kind"), "l2-link");
    }
    return count;
}

/* Whether this binary builds @product, so a configured-out one is skipped. */
static bool machine_built(QTestState *qts, const char *product)
{
    g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute':'query-machines'}");
    const QListEntry *e;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(reply, "return"), e) {
        QDict *m = qobject_to(QDict, qlist_entry_obj(e));

        if (g_str_equal(qdict_get_str(m, "name"), product)) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) piped = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GPtrArray) launches = g_ptr_array_new_with_free_func(g_free);
    const QListEntry *e;
    QTestState *qts;
    QDict *reply;
    int result;

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("sgi-catalogue-launch-XXXXXX", NULL);
    g_assert_nonnull(directory);

    qts = qtest_init("-M none");
    reply = qtest_qmp(qts, "{'execute':'query-sgi-machines'}");
    catalogue = qobject_ref(qdict_get_qdict(reply, "return"));
    qobject_unref(reply);

    /*
     * One launch per offering id. An id is unique, so the processor
     * options and populations that share a -M value each launch, and an
     * offering that needs more than its -M value to launch (the four
     * Origin 300 splits of two totals) cannot be missed. A pipe goes in
     * once per topology, on its first offering: what a pipe needs is the
     * topology's, not the population's.
     */
    QLIST_FOREACH_ENTRY(qdict_get_qlist(catalogue, "offerings"), e) {
        QDict *o = qobject_to(QDict, qlist_entry_obj(e));
        const char *id = qdict_get_str(o, "id");
        const char *topology = qdict_get_str(o, "topology");
        QDict *graphics;

        g_assert_false(g_hash_table_contains(seen, id));
        g_hash_table_add(seen, (gpointer)id);
        g_assert_nonnull(topology_record(topology));
        if (!machine_built(qts, qdict_get_str(o, "product"))) {
            continue;
        }
        graphics = qdict_get_qdict(topology_record(topology), "graphics");
        for (int pipe = 0; pipe <= (graphics != NULL); pipe++) {
            if (pipe) {
                if (g_hash_table_contains(piped, topology)) {
                    continue;
                }
                g_hash_table_add(piped, (gpointer)topology);
            }
            Launch *l = g_new0(Launch, 1);
            g_autofree char *path = g_strdup_printf(
                "/sgi-catalogue-launch/%s/%s", id, pipe ? "pipe" : "bare");

            l->offering = o;
            l->pipe = pipe;
            g_ptr_array_add(launches, l);
            g_strdelimit(path + strlen("/sgi-catalogue-launch/"), ",=",
                         '-');
            qtest_add_data_func(path, l, test_launch);
        }
        /* Every offering that lists links launches with them all backed. */
        if (offering_l2_links(o)) {
            Launch *l = g_new0(Launch, 1);
            g_autofree char *path = g_strdup_printf(
                "/sgi-catalogue-launch/%s/l2-links", id);

            l->offering = o;
            g_ptr_array_add(launches, l);
            g_strdelimit(path + strlen("/sgi-catalogue-launch/"), ",=",
                         '-');
            qtest_add_data_func(path, l, test_l2_links);
        }
        if (qlist_size(qdict_get_qlist(o, "networks")) > 1) {
            Launch *l = g_new0(Launch, 1);
            g_autofree char *path = g_strdup_printf(
                "/sgi-catalogue-launch/%s/networks", id);

            l->offering = o;
            l->all_networks = true;
            g_ptr_array_add(launches, l);
            g_strdelimit(path + strlen("/sgi-catalogue-launch/"), ",=",
                         '-');
            qtest_add_data_func(path, l, test_launch);
        }
    }
    qtest_quit(qts);

    result = g_test_run();
    qobject_unref(catalogue);
    g_rmdir(directory);
    g_free(directory);
    return result;
}
