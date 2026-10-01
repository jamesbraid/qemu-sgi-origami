/*
 * BlockBackend tests
 *
 * Copyright (c) 2017 Kevin Wolf <kwolf@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "block/block.h"
#include "system/block-backend.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qobject/qdict.h"
#include "qemu/units.h"

static void test_drain_aio_error_flush_cb(void *opaque, int ret)
{
    bool *completed = opaque;

    g_assert(ret == -ENOMEDIUM);
    *completed = true;
}

static void test_drain_aio_error(void)
{
    BlockBackend *blk = blk_new(qemu_get_aio_context(),
                                BLK_PERM_ALL, BLK_PERM_ALL);
    BlockAIOCB *acb;
    bool completed = false;

    acb = blk_aio_flush(blk, test_drain_aio_error_flush_cb, &completed);
    g_assert(acb != NULL);
    g_assert(completed == false);

    blk_drain(blk);
    g_assert(completed == true);

    blk_unref(blk);
}

static void test_drain_all_aio_error(void)
{
    BlockBackend *blk = blk_new(qemu_get_aio_context(),
                                BLK_PERM_ALL, BLK_PERM_ALL);
    BlockAIOCB *acb;
    bool completed = false;

    acb = blk_aio_flush(blk, test_drain_aio_error_flush_cb, &completed);
    g_assert(acb != NULL);
    g_assert(completed == false);

    blk_drain_all();
    g_assert(completed == true);

    blk_unref(blk);
}


static QDict *full_raw_options(const char *filename)
{
    QDict *options = qdict_new();

    qdict_put_str(options, "driver", "raw");
    qdict_put_str(options, "file.driver", "file");
    qdict_put_str(options, "file.filename", filename);
    return options;
}

static void test_full_raw_file_length(void)
{
    g_autofree char *filename = NULL;
    int fd = g_file_open_tmp("qemu-block-length-XXXXXX", &filename, NULL);
    int64_t lengths[] = {16 * MiB - 1, 16 * MiB, 16 * MiB + 1};
    BlockBackend *blk;
    size_t i;

    g_assert_cmpint(fd, >=, 0);
    for (i = 0; i < G_N_ELEMENTS(lengths); i++) {
        g_assert_cmpint(ftruncate(fd, lengths[i]), ==, 0);
        blk = blk_new_open(NULL, NULL, full_raw_options(filename), 0,
                           &error_abort);
        g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, lengths[i]);
        g_assert_cmpint(blk_getlength(blk), ==,
                        ROUND_UP(lengths[i], BDRV_SECTOR_SIZE));
        /* Query the opened file again after a caller changes its size. */
        g_assert_cmpint(ftruncate(fd, lengths[i] + 7), ==, 0);
        g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, lengths[i] + 7);
        blk_unref(blk);
    }
    close(fd);
    g_assert_cmpint(unlink(filename), ==, 0);
}

static void test_full_raw_file_length_unsupported(void)
{
    g_autofree char *filename = NULL;
    g_autofree char *directory = NULL;
    int fd = g_file_open_tmp("qemu-block-length-XXXXXX", &filename, NULL);
    QDict *options;
    BlockBackend *blk;
    Error *err = NULL;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 16 * MiB + 512), ==, 0);
    close(fd);

    blk = blk_new(qemu_get_aio_context(), 0, BLK_PERM_ALL);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOMEDIUM);
    blk_unref(blk);

    options = full_raw_options(filename);
    qdict_put_int(options, "offset", 0);
    blk = blk_new_open(NULL, NULL, options, 0, &error_abort);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    options = full_raw_options(filename);
    qdict_put_int(options, "offset", 512);
    blk = blk_new_open(NULL, NULL, options, 0, &error_abort);
    g_assert_cmpint(blk_getlength(blk), ==, 16 * MiB);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    options = full_raw_options(filename);
    qdict_put_int(options, "size", 16 * MiB);
    blk = blk_new_open(NULL, NULL, options, 0, &error_abort);
    g_assert_cmpint(blk_getlength(blk), ==, 16 * MiB);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    options = qdict_new();
    qdict_put_str(options, "driver", "null-co");
    blk = blk_new_open(NULL, NULL, options, 0, &error_abort);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    qdict_put_str(options, "file.driver", "null-co");
    blk = blk_new_open(NULL, NULL, options, 0, &error_abort);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    options = qdict_new();
    qdict_put_str(options, "driver", "copy-on-read");
    qdict_put_str(options, "file.driver", "raw");
    qdict_put_str(options, "file.file.driver", "file");
    qdict_put_str(options, "file.file.filename", filename);
    blk = blk_new_open(NULL, NULL, options, BDRV_O_RDWR, &error_abort);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    blk = blk_new_open(NULL, NULL, full_raw_options(filename),
                       BDRV_O_RDWR | BDRV_O_SNAPSHOT, &error_abort);
    g_assert_cmpint(blk_get_full_raw_file_length(blk), ==, -ENOTSUP);
    blk_unref(blk);

    directory = g_dir_make_tmp("qemu-block-directory-XXXXXX", NULL);
    g_assert_nonnull(directory);
    blk = blk_new_open(NULL, NULL, full_raw_options(directory), 0, &err);
    g_assert_null(blk);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpint(rmdir(directory), ==, 0);
    g_assert_cmpint(unlink(filename), ==, 0);
}

int main(int argc, char **argv)
{
    bdrv_init();
    qemu_init_main_loop(&error_abort);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/block-backend/drain_aio_error", test_drain_aio_error);
    g_test_add_func("/block-backend/drain_all_aio_error",
                    test_drain_all_aio_error);

    g_test_add_func("/block-backend/full_raw_file_length",
                    test_full_raw_file_length);

    g_test_add_func("/block-backend/full_raw_file_length_unsupported",
                    test_full_raw_file_length_unsupported);

    return g_test_run();
}
