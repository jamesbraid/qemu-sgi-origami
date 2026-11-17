/* SPDX-License-Identifier: BSD-3-Clause */
/* Stands in for the producer in unit tests that link a source without it. */
#include "qemu/osdep.h"
#include "ui/display-shm.h"

bool dshm_producer_present;

DShmHead *dshm_head_add(QemuConsole *con, const DShmSourceOps *ops,
                        void *opaque, const char *label)
{
    return NULL;
}

void dshm_head_remove(DShmHead *h)
{
}

void dshm_head_set_mode(DShmHead *h, uint32_t width, uint32_t height,
                        uint32_t refresh_mhz, uint32_t *ref, size_t stride)
{
}

void dshm_head_marked(DShmHead *h)
{
}

void dshm_head_sample_start(DShmHead *h)
{
}

void dshm_head_changed(DShmHead *h, int first, int count)
{
}

void dshm_head_produced(DShmHead *h, unsigned count)
{
}

void dshm_head_sample_end(DShmHead *h)
{
}

void dshm_head_retrace(DShmHead *h)
{
}

void dshm_head_retrace_stopped(DShmHead *h)
{
}

void dshm_head_set_steerable(DShmHead *h, bool steerable)
{
}

void dshm_head_set_cursor_embedded(DShmHead *h, bool embedded)
{
}

void dshm_head_cursor_move(DShmHead *h, int x, int y, bool on)
{
}

void dshm_head_guest_cursor(DShmHead *h, unsigned kind, int x, int y)
{
}

void dshm_head_mark(DShmHead *h, int first, int count)
{
}

void dshm_head_set_gpu(DShmHead *h, const DShmGpuOps *ops)
{
}

void dshm_head_gpu_ready(DShmHead *h)
{
}

uint32_t dshm_head_sample_marks_gpu(DShmHead *h, DShmResolveRow resolve,
                                    void *opaque)
{
    return 0;
}

void dshm_head_gpu_publish(DShmHead *h, uint32_t id)
{
}

void dshm_head_gpu_abort(DShmHead *h, uint32_t id)
{
}
