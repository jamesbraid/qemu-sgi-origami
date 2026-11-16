/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The scanout test links hw/display/sgi/scanout.c without the shared-memory
 * display (ui/sgi-display-shm.c), which is what a machine runs without the
 * display-shm property: frames are plain heap memory and no head is
 * published.
 */
#include "qemu/osdep.h"
#include "ui/sgi-display-shm.h"

struct SGIDisplayRegion {
    void *ptr;
};

void *sgi_display_region_alloc(Object *owner, const char *label,
                               uint64_t size, uint32_t kind,
                               SGIDisplayRegion **region)
{
    *region = g_new0(SGIDisplayRegion, 1);
    (*region)->ptr = g_malloc0(MAX(size, 1));
    return (*region)->ptr;
}

void sgi_display_region_free(SGIDisplayRegion *region)
{
    if (region) {
        g_free(region->ptr);
        g_free(region);
    }
}

bool sgi_display_region_take_local(SGIDisplayRegion *region, void **mem)
{
    *mem = region->ptr;
    g_free(region);
    return true;
}

void sgi_display_head_bind(SGIScanout *s, uint32_t family,
                           uint32_t family_version)
{
}

void sgi_display_head_unbind(SGIScanout *s)
{
}

void sgi_display_head_set_frame(SGIScanout *s, SGIDisplayRegion *r,
                                uint32_t width, uint32_t height,
                                uint32_t refresh_mhz)
{
}

void sgi_display_head_guest_cursor(SGIScanout *s, int x, int y)
{
}

void sgi_display_head_presented(SGIScanout *s, int first, int count)
{
}

void sgi_display_head_invalidated(SGIScanout *s, int first, int count)
{
}

void sgi_display_head_published(SGIScanout *s)
{
}

bool sgi_display_head_exact(SGIScanout *s)
{
    return false;
}
