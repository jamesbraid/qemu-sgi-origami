/*
 * Register-contract facts about the per-CPU cache-as-RAM private window
 * (hw/mips/sgi/sn-promdata.c), parameterised over both Scalable Node
 * generations: sgi-sn0-node (Origin 200/2000) and sgi-sn1-node
 * (Origin 300).
 *
 * Covers placement, priority and memory-space wiring for the
 * flash-execution filter every generation installs, and for the
 * copy_local routing overlay at each generation's PROM RAM-copy address.
 *
 * qtest never executes guest code, so the private phase itself cannot
 * be armed here -- the same limitation sn0-migration-test.c's own
 * promdata case already documents. What a qtest can check is that
 * copy_local exists, sits at the right address and priority, and
 * survives a migration; that is exactly what commit d8d18c7aa0 silently
 * dropped for Origin 200 while moving the filter into this shared file,
 * so test_copy_local_window's Origin 200 case is also the regression
 * test for that finding: it fails outright (the region is simply absent
 * from the tree) against a build that carries the drop, and passes once
 * sn_promdata_init() installs copy_local again.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

/* This file's own copies, the way every sn0-*-test.c stands alone. */
#define PROMDATA_PADDR          UINT64_C(0x00100000)
#define PROMDATA_SIZE           UINT64_C(0x00008000)

/* hw/mips/sgi/sn0.c's own constant and origin200's flash_reset.size. */
#define PROM_COPY_PADDR         UINT64_C(0x01a00000)
#define SN0_FLASH_RESET_SIZE    UINT64_C(0x00100000)
#define SN1_PROM_COPY_SIZE      UINT64_C(0x00200000)

/* hw/mips/sn1-origin300-model.c's generated prom_size. */
#define SN1_PROM_SIZE           UINT64_C(0x1686a8)

typedef struct PromdataWindowCase {
    const char *label;
    const char *machine_args;
    bool needs_bios;
    const char *filter_region;
    const char *copy_region;
    uint64_t copy_size;
} PromdataWindowCase;

static const PromdataWindowCase promdata_window_cases[] = {
    {
        .label = "origin200",
        .machine_args = "-M origin200,synthesized-firmware=off -smp 2",
        .needs_bios = false,
        .filter_region = "sn0.node0.cpu0.promdata-filter",
        .copy_region = "sn0.node0.cpu0.prom-copy-local",
        .copy_size = SN0_FLASH_RESET_SIZE,
    },
    {
        .label = "origin300",
        .machine_args = "-M origin300 -cpu R14000 -smp 2",
        .needs_bios = true,
        .filter_region = "sn1.node0.cpu0.promdata-filter",
        .copy_region = "sn1.node0.cpu0.prom-copy-local",
        .copy_size = SN1_PROM_COPY_SIZE,
    },
};

static char *write_minimal_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(SN1_PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn-promdata-window-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, SN1_PROM_SIZE), ==,
                    (ssize_t)SN1_PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *start_case(const PromdataWindowCase *c,
                              const char *prom_path)
{
    if (c->needs_bios) {
        g_autofree char *quoted = g_shell_quote(prom_path);

        return qtest_initf("%s -bios %s", c->machine_args, quoted);
    }
    return qtest_init(c->machine_args);
}

/* Every generation installs the flash-execution filter the same way. */
static void test_filter_window(const void *data)
{
    const PromdataWindowCase *c = data;
    g_autofree char *prom_path = write_minimal_prom();
    QTestState *qts = start_case(c, prom_path);
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64 " (prio 2, i/o): %s",
        PROMDATA_PADDR, PROMDATA_PADDR + PROMDATA_SIZE - 1,
        c->filter_region);

    g_assert_nonnull(strstr(mtree, mapping));

    qtest_quit(qts);
    unlink(prom_path);
}

/*
 * copy_local is a disabled-by-default alias (sn-promdata.c only enables
 * it once a CPU is latched into the private phase), so the flat view
 * "-f" uses above never shows it; "-D" walks the raw tree and marks a
 * disabled region instead of hiding it.
 */
static void test_copy_local_window(const void *data)
{
    const PromdataWindowCase *c = data;
    g_autofree char *prom_path = write_minimal_prom();
    QTestState *qts = start_case(c, prom_path);
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -D");

    g_autofree char *mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64
        " (prio 2, container): alias %s @system "
        "%016" PRIx64 "-%016" PRIx64 " [disabled]",
        PROM_COPY_PADDR, PROM_COPY_PADDR + c->copy_size - 1,
        c->copy_region, PROM_COPY_PADDR,
        PROM_COPY_PADDR + c->copy_size - 1);

    g_assert_nonnull(strstr(mtree, mapping));

    qtest_quit(qts);
    unlink(prom_path);
}

/*
 * The array-level VMStateDescription wrapper each node type keeps
 * (vmstate_sn0_promdata / vmstate_sn1_node_promdata) calls
 * sn_promdata_apply_load() in its own post_load, which is what
 * re-derives copy_local's enabled state after a migration (the
 * "migration handling 19898fab4f had" the shared helper must keep).
 * Confirm the region -- present, disabled, same address and priority --
 * survives the round trip rather than being dropped or left mis-wired.
 */
static void test_copy_local_window_survives_migration(const void *data)
{
    const PromdataWindowCase *c = data;
    g_autofree char *prom_path = write_minimal_prom();
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    g_autofree char *mtree = NULL;
    g_autofree char *mapping = NULL;

    tmpfs = g_dir_make_tmp("sn-promdata-window-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = start_case(c, prom_path);

    if (c->needs_bios) {
        g_autofree char *quoted = g_shell_quote(prom_path);

        dest_args = g_strdup_printf("%s -bios %s -incoming %s",
                                    c->machine_args, quoted, uri);
    } else {
        dest_args = g_strdup_printf("%s -incoming %s", c->machine_args, uri);
    }
    to = qtest_init(dest_args);

    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }
        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }

    mtree = qtest_hmp(to, "info mtree -D");
    mapping = g_strdup_printf(
        "%016" PRIx64 "-%016" PRIx64
        " (prio 2, container): alias %s @system "
        "%016" PRIx64 "-%016" PRIx64 " [disabled]",
        PROM_COPY_PADDR, PROM_COPY_PADDR + c->copy_size - 1,
        c->copy_region, PROM_COPY_PADDR,
        PROM_COPY_PADDR + c->copy_size - 1);
    g_assert_nonnull(strstr(mtree, mapping));

    qtest_quit(to);
    qtest_quit(from);
    unlink(prom_path);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < G_N_ELEMENTS(promdata_window_cases); i++) {
        const PromdataWindowCase *c = &promdata_window_cases[i];
        g_autofree char *filter_name = g_strdup_printf(
            "sn-promdata-window/%s/filter-window", c->label);
        g_autofree char *copy_name = g_strdup_printf(
            "sn-promdata-window/%s/copy-local-window", c->label);
        g_autofree char *copy_migration_name = g_strdup_printf(
            "sn-promdata-window/%s/copy-local-window-survives-migration",
            c->label);

        qtest_add_data_func(filter_name, c, test_filter_window);
        qtest_add_data_func(copy_name, c, test_copy_local_window);
        qtest_add_data_func(copy_migration_name, c,
                            test_copy_local_window_survives_migration);
    }

    return g_test_run();
}
