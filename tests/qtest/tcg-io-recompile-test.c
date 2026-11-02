/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"

static char *image_path;

static void test_metadata(gconstpointer data)
{
    const char *options = data;
    g_autofree char *quoted = g_shell_quote(image_path);
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = qtest_initf("-M origin200 -m 128M -cpu R10000 -bios %s "
                      "-nic none -S -device tcg-io-recompile-fixture,id=meta %s",
                      quoted, options);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/meta',"
        "'property':'exercise','value':true}}");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autofree char *directory = NULL;
    uint8_t image[4096] = { 0 };

    g_test_init(&argc, &argv, NULL);
    directory = g_dir_make_tmp("tcg-io-recompile-XXXXXX", NULL);
    g_assert_nonnull(directory);
    image_path = g_build_filename(directory, "unused.bin", NULL);
    g_assert_true(g_file_set_contents(image_path, (char *)image,
                                      sizeof(image), NULL));
    qtest_add_data_func("/tcg/io-recompile/metadata/single",
                        "-accel tcg,thread=single", test_metadata);
    qtest_add_data_func("/tcg/io-recompile/metadata/multicpu",
                        "-smp 2 -accel tcg,thread=multi", test_metadata);
    qtest_add_data_func("/tcg/io-recompile/metadata/icount",
                        "-accel tcg,thread=single "
                        "-icount shift=3,align=off,sleep=off", test_metadata);
    int result = g_test_run();

    g_assert_cmpint(unlink(image_path), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_free(image_path);
    return result;
}
