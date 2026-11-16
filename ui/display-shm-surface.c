/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Surface heads: consoles of 2D devices that finish a hardware update before
 * qemu_console_hw_update() returns. Any other graphic console is unsupported.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "ui/display-shm-priv.h"
#include "ui/surface.h"

typedef struct SurfaceHead {
    DShmHead *h;
    DisplaySurface *surface;
    bool updated;               /* a rectangle arrived since the last poll */
    uint64_t interval;
    uint64_t base;
} SurfaceHead;

#define POLL_MAX_MS 250

static const char *const allowed[] = {
    "VGA", "secondary-vga", "cirrus-vga", "isa-cirrus-vga", "bochs-display",
    "ramfb", "virtio-vga", "virtio-gpu-pci", "virtio-gpu-device",
};

bool dshm_surface_wanted(QemuConsole *con)
{
    Object *dev = object_property_get_link(OBJECT(con), "device", NULL);
    const char *name;

    if (!dev || qemu_console_has_gl(con)) {
        return false;
    }
    name = object_get_typename(dev);
    if (strstr(name, "-gl") || strstr(name, "rutabaga") ||
        strstr(name, "virgl")) {
        return false;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(allowed); i++) {
        if (object_dynamic_cast(dev, allowed[i])) {
            return true;
        }
    }
    return false;
}

static void surface_resolve(void *opaque, unsigned y, uint32_t *dst)
{
    SurfaceHead *sh = opaque;
    DisplaySurface *ds = sh->surface;
    uint32_t w = sh->h->width;

    if (!ds || y >= (unsigned)surface_height(ds)) {
        memset(dst, 0, w * 4);
        return;
    }
    if (surface_format(ds) == PIXMAN_x8r8g8b8 ||
        surface_format(ds) == PIXMAN_a8r8g8b8) {
        memcpy(dst, surface_data(ds) + (size_t)y * surface_stride(ds),
               MIN(w, (uint32_t)surface_width(ds)) * 4);
    } else {
        pixman_image_t *to = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, 1,
                                                      dst, w * 4);

        pixman_image_composite(PIXMAN_OP_SRC, ds->image, NULL, to,
                               0, y, 0, 0, 0, 0, w, 1);
        pixman_image_unref(to);
    }
}

static void surface_sample(void *opaque)
{
    SurfaceHead *sh = opaque;

    /* The device's own update runs to its end here, marking what changed. */
    qemu_console_hw_update(sh->h->con);
    dshm_head_sample_marks(sh->h, surface_resolve, sh);
}

static const DShmSourceOps surface_ops = { .sample = surface_sample };

static void surface_gfx_update(DisplayChangeListener *dcl, int x, int y,
                               int w, int h)
{
    DShmHead *head = container_of(dcl, DShmHead, dcl);
    SurfaceHead *sh = head->opaque;

    sh->updated = true;
    sh->interval = sh->base;
    dshm_head_mark(head, y, h);
}

static void surface_gfx_switch(DisplayChangeListener *dcl,
                               DisplaySurface *new_surface)
{
    DShmHead *head = container_of(dcl, DShmHead, dcl);
    SurfaceHead *sh = head->opaque;

    sh->surface = new_surface;
    sh->updated = true;
    if (!new_surface) {
        dshm_head_set_mode_owned(head, 0, 0, 0);
        return;
    }
    dshm_head_set_mode_owned(head, surface_width(new_surface),
                             surface_height(new_surface), 0);
}

/* Poll while somebody waits, backing off when nothing changes. */
static void surface_refresh(DisplayChangeListener *dcl)
{
    DShmHead *head = container_of(dcl, DShmHead, dcl);
    SurfaceHead *sh = head->opaque;

    if (!dshm_head_demand(head)) {
        qemu_console_listener_set_refresh(dcl, GUI_REFRESH_INTERVAL_IDLE);
        return;
    }
    sh->updated = false;
    qemu_console_hw_update(head->con);
    if (!sh->updated) {
        sh->interval = MIN(sh->interval * 2, (uint64_t)POLL_MAX_MS);
    }
    qemu_console_listener_set_refresh(dcl, sh->interval);
}

static const DisplayChangeListenerOps surface_dcl_ops = {
    .dpy_name = "display-shm",
    .dpy_refresh = surface_refresh,
    .dpy_gfx_update = surface_gfx_update,
    .dpy_gfx_switch = surface_gfx_switch,
    .dpy_mouse_set = dshm_dcl_mouse_set,
    .dpy_cursor_define = dshm_dcl_cursor_define,
};

void dshm_surface_attach(DisplayShm *d, QemuConsole *con)
{
    SurfaceHead *sh;
    DShmHead *h;

    if (!dshm_surface_wanted(con)) {
        h = dshm_head_new(d, con, NULL, NULL, "unsupported");
        h->unsupported = true;
        h->seen_con = con;
        h->con = NULL;
        dshm_head_announce(h);
        return;
    }
    sh = g_new0(SurfaceHead, 1);
    sh->base = 16;
    sh->interval = sh->base;
    h = dshm_head_new(d, con, &surface_ops, sh, "surface");
    sh->h = h;
    h->dcl_ops = &surface_dcl_ops;
    dshm_head_announce(h);
}
