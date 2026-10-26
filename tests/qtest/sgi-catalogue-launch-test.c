/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Launch every offering query-sgi-machines lists from the catalogue's own
 * data and nothing else, the way a launcher does: the offering's
 * machine-options, one block node per storage item bound through the
 * machine property of the same name, its default -cpu, its -smp and its
 * default memory, with -nodefaults. A topology with graphics slots is
 * launched again with a pipe in its first slot, adding the graphics
 * record's machine-properties. Any input the machine needs that the
 * catalogue does not describe fails here, by topology, as does a SCSI
 * adapter the machine builds and the offering does not list.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

static QDict *catalogue;
static char *directory;

typedef struct Launch {
    QDict *offering;
    /* With a pipe from the topology's graphics record. */
    bool pipe;
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
 * Run QEMU with @args (NULL-terminated) and the QMP @commands on its
 * standard input, after qmp_capabilities and before quit. Returns its
 * exit status; @out and @err hold what it printed.
 */
static int run_qemu(GPtrArray *args, const char *commands, char **out,
                    char **err)
{
    g_autofree char *input = g_strdup_printf(
        "{\"execute\":\"qmp_capabilities\"}\n%s"
        "{\"execute\":\"quit\"}\n", commands);
    struct sigaction ignore = { .sa_handler = SIG_IGN }, previous;
    GPid pid;
    int input_fd, output_fd, error_fd, status;
    GIOChannel *channel;
    gsize length;
    ssize_t written;

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
    g_assert_cmpint(sigaction(SIGPIPE, &previous, NULL), ==, 0);
    close(input_fd);
    channel = g_io_channel_unix_new(output_fd);
    g_assert_cmpint(g_io_channel_read_to_end(channel, out, &length, NULL),
                    ==, G_IO_STATUS_NORMAL);
    g_io_channel_unref(channel);
    close(output_fd);
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
            g_test_message("%s lists %s, which it does not build", machine,
                           qdict_get_str(adapter, "bus"));
        }
        g_assert_nonnull(strstr(out, line));
    }
    if (strstr(out, "dev: qlogic-isp") && qlist_empty(adapters)) {
        g_test_message("%s builds a QLogic SCSI adapter it does not list",
                       machine);
        g_assert_false(qlist_empty(adapters));
    }
}

static void test_launch(const void *opaque)
{
    const Launch *l = opaque;
    QDict *o = l->offering;
    const char *topology = qdict_get_str(o, "topology");
    int64_t nodes = qdict_get_int(o, "nodes");
    QDict *memory = qdict_get_qdict(o, "memory-per-node-mib");
    QDict *graphics = qdict_get_qdict(topology_record(topology), "graphics");
    QDict *machine_properties = graphics ?
        qdict_get_qdict(graphics, "machine-properties") : NULL;
    /*
     * A processor that lists required properties is an opt-in experiment:
     * the catalogue names the -cpu properties a launch must choose but
     * gives no values, so QEMU must refuse a launch made without them,
     * asking for the experiment's opt-in.
     */
    bool opt_in = qdict_haskey(qdict_get_qdict(o, "processor"),
                               "required-properties");
    /*
     * An Origin 2000 with a pipe needs a keyboard for its X server, which
     * only the BaseIO-G's MediaIO carries: the catalogue launch must
     * build it, whatever the catalogue's record says.
     */
    bool mediaio = l->pipe &&
                   g_str_equal(qdict_get_str(o, "product"), "origin2000");
    g_autofree char *tag = g_strdup_printf(
        "%s-%" PRId64 "-%s", topology, nodes, l->pipe ? "pipe" : "bare");
    g_autoptr(GString) machine = g_string_new(
        qdict_get_str(o, "machine-options"));
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *out = NULL, *err = NULL;
    const QListEntry *e;
    int status;

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
    g_ptr_array_add(args, g_strdup("-M"));
    g_ptr_array_add(args, g_strdup(machine->str));
    g_ptr_array_add(args, g_strdup("-cpu"));
    g_ptr_array_add(args, g_strdup(qdict_get_str(o, "default-cpu-model")));
    g_ptr_array_add(args, g_strdup("-smp"));
    g_ptr_array_add(args, g_strdup_printf("%" PRId64,
                                          qdict_get_int(o, "smp")));
    g_ptr_array_add(args, g_strdup("-m"));
    g_ptr_array_add(args, g_strdup_printf(
        "%" PRId64 "M", qdict_get_int(memory, "default") * nodes));
    for (const char *const *word = (const char *const[]) {
             "-nodefaults", "-display", "none", "-S", "-accel", "qtest",
             "-qmp", "stdio", NULL }; *word; word++) {
        g_ptr_array_add(args, g_strdup(*word));
    }
    g_ptr_array_add(args, NULL);

    status = run_qemu(args, mediaio ?
        "{\"execute\":\"qom-get\",\"arguments\":{\"path\":"
        "\"/machine/module0/io1/baseio/mio-ioc3/kbc\","
        "\"property\":\"type\"}}\n" QTREE : QTREE, &out, &err);
    for (guint i = 0; i < files->len; i++) {
        unlink(g_ptr_array_index(files, i));
    }
    if (opt_in) {
        g_assert_cmpint(status, !=, 0);
        g_assert_nonnull(strstr(err, "requires ip53-experimental=on"));
        return;
    }
    if (status) {
        g_test_message("%s refused: %s", machine->str, err);
    }
    g_assert_cmpint(status, ==, 0);
    check_scsi_adapters(o, machine->str, out);
    /* The qom-get answers with the controller's type, not an error. */
    if (mediaio) {
        if (strstr(out, "\"error\"")) {
            g_test_message("%s has no MediaIO keyboard controller: %s",
                           machine->str, out);
        }
        g_assert_null(strstr(out, "\"error\""));
    }
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
     * One launch per -M value: the processor-option offerings of one
     * population differ only in the processor module, and the shared
     * population is what fails on a missing input.
     */
    QLIST_FOREACH_ENTRY(qdict_get_qlist(catalogue, "offerings"), e) {
        QDict *o = qobject_to(QDict, qlist_entry_obj(e));
        const char *options = qdict_get_str(o, "machine-options");
        const char *topology = qdict_get_str(o, "topology");
        QDict *graphics;

        if (g_hash_table_contains(seen, options) ||
            !machine_built(qts, qdict_get_str(o, "product"))) {
            continue;
        }
        g_hash_table_add(seen, (gpointer)options);
        graphics = qdict_get_qdict(topology_record(topology), "graphics");
        for (int pipe = 0; pipe <= (graphics != NULL); pipe++) {
            Launch *l = g_new0(Launch, 1);
            g_autofree char *path = g_strdup_printf(
                "/sgi-catalogue-launch/%s/%s", options,
                pipe ? "pipe" : "bare");

            l->offering = o;
            l->pipe = pipe;
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
