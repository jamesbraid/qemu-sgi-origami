/*
 * Whether a QEMU under test has drawn in bands of rows on several threads.
 *
 * The shared SGI rasterizer starts its band workers, threads named
 * "sgi-raster/N", the first time a draw into a target that allows it
 * (SGIRasterTarget.concurrent_pixels) is large enough to split, and keeps
 * them until the process ends. A family's test draws something large in
 * a QEMU of its own and then looks for those threads, which shows that
 * the family's target opted in. It reads the threads' names from Linux's
 * /proc, so it skips where that is absent, and where the host has a
 * single processor, on which the rasterizer draws on one thread.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_RASTER_BANDS_H
#define SGI_RASTER_BANDS_H

#include "libqtest.h"

/* The band workers the QEMU of @qts has started; -1 if it cannot tell. */
static inline int sgi_raster_band_workers(QTestState *qts)
{
    g_autofree char *dir_path = NULL;
    GDir *dir;
    const char *task;
    int workers = 0;

    if (g_get_num_processors() < 2) {
        return -1;
    }
    dir_path = g_strdup_printf("/proc/%d/task", (int)qtest_pid(qts));
    dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        return -1;
    }
    while ((task = g_dir_read_name(dir))) {
        g_autofree char *comm_path = g_strdup_printf("%s/%s/comm", dir_path,
                                                     task);
        g_autofree char *comm = NULL;

        if (g_file_get_contents(comm_path, &comm, NULL, NULL) &&
            g_str_has_prefix(comm, "sgi-raster/")) {
            workers++;
        }
    }
    g_dir_close(dir);
    return workers;
}

#endif /* SGI_RASTER_BANDS_H */
