/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shared-memory display for the SGI machines. See ui/sgi-display-shm.h and
 * docs/system/mips/sgi/display-shm.rst.
 *
 * One reader at a time, no input channel (input goes through QMP
 * input-send-event). Shared memory is POSIX shm_open on Linux and macOS and
 * a named file mapping on Windows; the notification socket is AF_UNIX on all
 * three (Windows 10 and later).
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/atomic.h"
#include "qemu/notify.h"
#include "qom/object.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/sgi-display-shm.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

/* Ask for the guest's own scanout rate: SGI modes run at 60 to 76 Hz. */
#define SGI_SHM_REFRESH_MS 13

typedef struct ShmDisplay ShmDisplay;

typedef struct ShmHead {
    DisplayChangeListener dcl;
    ShmDisplay *d;
    unsigned idx;
    DisplaySurface *surface;
    uint32_t width, height;
    /*
     * The writer's own copy of what it publishes. The reader maps the
     * segment read-write, so nothing QEMU indexes or branches on is read
     * back from it, except the one field the reader owns, reading.
     */
    bool active;
    uint32_t latest;
    uint64_t seq;
    int32_t cursor_x, cursor_y;
    uint32_t cursor_on;
    uint32_t gen[SGI_SHM_SLOTS];
    /* Rows of the surface not yet compared with the latest slot. */
    uint32_t pend_y0, pend_y1;
    /* Rows each slot lacks against the newest frame. */
    uint32_t stale_y0[SGI_SHM_SLOTS], stale_y1[SGI_SHM_SLOTS];
    bool resized;
    bool warned_format;
} ShmHead;

struct ShmDisplay {
    char *name;
    char *sock_path;            /* the property, or NULL for the default */
    SGIShmHeader *hdr;
    uint8_t *base;
    size_t total;
#ifdef _WIN32
    HANDLE mapping;
#endif
    int listen_fd;
    int client_fd;
    bool resync;                /* a message was dropped; send state again */
    bool created;
    unsigned nheads;
    ShmHead heads[SGI_SHM_MAX_HEADS];
    Notifier done;
    Notifier exit;
};

static ShmDisplay *shm_display;

static uint8_t *shm_slot(ShmDisplay *d, unsigned head, unsigned slot)
{
    return d->base + SGI_SHM_HEADER_BYTES +
           ((size_t)head * SGI_SHM_SLOTS + slot) * SGI_SHM_SLOT_BYTES;
}

static void shm_drop_client(ShmDisplay *d)
{
    if (d->client_fd >= 0) {
        qemu_set_fd_handler(d->client_fd, NULL, NULL, NULL);
        close(d->client_fd);
        d->client_fd = -1;
    }
}

/* Returns false when the message did not go out. */
static bool shm_send(ShmDisplay *d, const SGIShmMsg *m)
{
    ssize_t n;

    if (d->client_fd < 0) {
        return true;
    }
    n = send(d->client_fd, (const void *)m, sizeof(*m), 0);
    if (n == (ssize_t)sizeof(*m)) {
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                  errno == EINTR)) {
        /* Socket full: the reader resyncs from the next full message. */
        d->resync = true;
        return false;
    }
    /*
     * A broken connection, or a partial message that would desynchronise
     * the stream: drop the reader, which reconnects and gets a full repaint.
     */
    shm_drop_client(d);
    return false;
}

static void shm_msg_frame(ShmHead *h, SGIShmMsg *m, uint32_t y0, uint32_t y1,
                          uint32_t flags)
{
    *m = (SGIShmMsg) {
        .type = SGI_SHM_MSG_FRAME, .head = h->idx,
        .seq = h->seq, .slot = h->latest, .y0 = y0, .y1 = y1,
        .width = h->width, .height = h->height, .flags = flags,
        .cursor_x = h->cursor_x, .cursor_y = h->cursor_y,
        .cursor_on = h->cursor_on,
    };
}

static void shm_full_repaint(ShmHead *h)
{
    h->pend_y0 = 0;
    h->pend_y1 = h->height;
    for (unsigned s = 0; s < SGI_SHM_SLOTS; s++) {
        h->stale_y0[s] = 0;
        h->stale_y1[s] = h->height;
    }
    h->resized = true;
}

static void shm_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *s)
{
    ShmHead *h = container_of(dcl, ShmHead, dcl);
    SGIShmHead *hh = &h->d->hdr->head[h->idx];
    int w = surface_width(s), ht = surface_height(s);
    pixman_format_code_t fmt = surface_format(s);
    SGIShmMsg m;

    h->surface = s;
    if (w <= 0 || ht <= 0 || w > SGI_SHM_MAX_DIM || ht > SGI_SHM_MAX_DIM ||
        (fmt != PIXMAN_x8r8g8b8 && fmt != PIXMAN_a8r8g8b8)) {
        if (!h->warned_format) {
            warn_report("sgi-display-shm: head %u surface %dx%d format 0x%x "
                        "cannot be published", h->idx, w, ht, (unsigned)fmt);
            h->warned_format = true;
        }
        if (h->active) {
            h->active = false;
            qatomic_set(&hh->active, 0);
            h->width = h->height = 0;
            shm_msg_frame(h, &m, 0, 0, SGI_SHM_FLAG_INACTIVE);
            shm_send(h->d, &m);
        }
        return;
    }
    h->width = w;
    h->height = ht;
    h->active = true;
    qatomic_set(&hh->active, 1);
    shm_full_repaint(h);
}

static void shm_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int ht)
{
    ShmHead *h = container_of(dcl, ShmHead, dcl);
    uint32_t y0 = MAX(y, 0), y1 = MAX(y + ht, 0);

    if (h->pend_y1 <= h->pend_y0) {
        h->pend_y0 = y0;
        h->pend_y1 = y1;
    } else {
        h->pend_y0 = MIN(h->pend_y0, y0);
        h->pend_y1 = MAX(h->pend_y1, y1);
    }
}

static void shm_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    ShmHead *h = container_of(dcl, ShmHead, dcl);
    SGIShmHead *hh = &h->d->hdr->head[h->idx];
    SGIShmMsg m = { .type = SGI_SHM_MSG_CURSOR, .head = h->idx,
                    .seq = h->seq, .slot = h->latest,
                    .width = h->width, .height = h->height,
                    .cursor_x = x, .cursor_y = y, .cursor_on = on };

    h->cursor_x = x;
    h->cursor_y = y;
    h->cursor_on = on;
    qatomic_set(&hh->cursor_x, x);
    qatomic_set(&hh->cursor_y, y);
    qatomic_set(&hh->cursor_on, on);
    shm_send(h->d, &m);
}

static void shm_publish(ShmHead *h)
{
    ShmDisplay *d = h->d;
    SGIShmHead *hh = &d->hdr->head[h->idx];
    DisplaySurface *s = h->surface;
    unsigned width = h->width, height = h->height;
    unsigned latest = h->latest;
    unsigned reading = qatomic_read(&hh->reading);
    unsigned slot, t;
    uint32_t y0, y1, cy0 = height, cy1 = 0;
    const uint8_t *src;
    uint8_t *lat;
    SGIShmMsg m;

    if (!s || !h->active) {
        return;
    }
    if (h->pend_y1 <= h->pend_y0 && !h->resized) {
        return;
    }
    y0 = MIN(h->pend_y0, height);
    y1 = MIN(h->pend_y1, height);
    src = surface_data(s);
    lat = shm_slot(d, h->idx, latest);

    /* Find the rows that really changed against the newest frame. */
    if (h->resized) {
        cy0 = 0;
        cy1 = height;
    } else {
        for (uint32_t y = y0; y < y1; y++) {
            if (memcmp(src + (size_t)y * surface_stride(s),
                       lat + (size_t)y * width * 4, (size_t)width * 4)) {
                cy0 = MIN(cy0, y);
                cy1 = y + 1;
            }
        }
    }
    h->pend_y0 = h->pend_y1 = 0;
    if (cy1 <= cy0) {
        return;
    }

    /* A slot that is neither the newest nor held by the reader. */
    for (slot = 0; slot < SGI_SHM_SLOTS; slot++) {
        if (slot != latest && slot + 1 != reading) {
            break;
        }
    }
    assert(slot < SGI_SHM_SLOTS);

    /* Rows to bring this slot up to date: what it lacks plus the change. */
    if (h->resized) {
        y0 = 0;
        y1 = height;
    } else {
        y0 = cy0;
        y1 = cy1;
        if (h->stale_y1[slot] > h->stale_y0[slot]) {
            y0 = MIN(y0, h->stale_y0[slot]);
            y1 = MAX(y1, h->stale_y1[slot]);
        }
    }
    qatomic_set(&hh->slot[slot].gen, ++h->gen[slot]);   /* odd */
    smp_mb();
    qatomic_set(&hh->slot[slot].width, width);
    qatomic_set(&hh->slot[slot].height, height);
    for (uint32_t y = y0; y < y1; y++) {
        memcpy(shm_slot(d, h->idx, slot) + (size_t)y * width * 4,
               src + (size_t)y * surface_stride(s), (size_t)width * 4);
    }
    /* The rows must be visible before the generation turns even. */
    smp_mb();
    h->seq++;
    qatomic_set(&hh->seq, h->seq);
    qatomic_set(&hh->slot[slot].seq, h->seq);
    qatomic_set(&hh->slot[slot].gen, ++h->gen[slot]);   /* even */
    /* The slot must be stable before a reader can be pointed at it. */
    smp_mb();
    h->latest = slot;
    qatomic_set(&hh->latest, slot);

    h->stale_y0[slot] = h->stale_y1[slot] = 0;
    for (t = 0; t < SGI_SHM_SLOTS; t++) {
        if (t == slot) {
            continue;
        }
        if (h->resized) {
            h->stale_y0[t] = 0;
            h->stale_y1[t] = height;
        } else if (h->stale_y1[t] <= h->stale_y0[t]) {
            h->stale_y0[t] = cy0;
            h->stale_y1[t] = cy1;
        } else {
            h->stale_y0[t] = MIN(h->stale_y0[t], cy0);
            h->stale_y1[t] = MAX(h->stale_y1[t], cy1);
        }
    }

    shm_msg_frame(h, &m, cy0, cy1,
                  h->resized ? SGI_SHM_FLAG_SIZE | SGI_SHM_FLAG_FULL : 0);
    h->resized = false;
    shm_send(d, &m);
}

/* Tell the reader everything again after a dropped message. */
static void shm_resend_state(ShmDisplay *d)
{
    d->resync = false;
    for (unsigned i = 0; i < d->nheads && d->client_fd >= 0; i++) {
        ShmHead *h = &d->heads[i];
        SGIShmMsg m;

        if (!h->active) {
            shm_msg_frame(h, &m, 0, 0, SGI_SHM_FLAG_INACTIVE);
        } else if (h->seq) {
            shm_msg_frame(h, &m, 0, h->height, SGI_SHM_FLAG_FULL |
                          (h->resized ? SGI_SHM_FLAG_SIZE : 0));
        } else {
            m = (SGIShmMsg) { .type = SGI_SHM_MSG_CURSOR, .head = i,
                              .width = h->width, .height = h->height,
                              .cursor_x = h->cursor_x,
                              .cursor_y = h->cursor_y,
                              .cursor_on = h->cursor_on };
        }
        if (!shm_send(d, &m)) {
            return;
        }
    }
}

static void shm_refresh(DisplayChangeListener *dcl)
{
    ShmHead *h = container_of(dcl, ShmHead, dcl);

    qemu_console_hw_update(h->dcl.con);
    shm_publish(h);
    if (h->d->resync && h->idx == 0) {
        shm_resend_state(h->d);
    }
}

static const DisplayChangeListenerOps shm_ops = {
    .dpy_name = "sgi-display-shm",
    .dpy_refresh = shm_refresh,
    .dpy_gfx_update = shm_gfx_update,
    .dpy_gfx_switch = shm_gfx_switch,
    .dpy_mouse_set = shm_mouse_set,
};

/* The reader sends nothing; reading detects that it went away. */
static void shm_client_read(void *opaque)
{
    ShmDisplay *d = opaque;
    char buf[64];
    ssize_t n = recv(d->client_fd, buf, sizeof(buf), 0);

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR)) {
        shm_drop_client(d);
    }
}

static void shm_accept(void *opaque)
{
    ShmDisplay *d = opaque;
    int fd = accept(d->listen_fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    shm_drop_client(d);
    qemu_set_blocking(fd, false, NULL);
    d->client_fd = fd;
    qemu_set_fd_handler(fd, shm_client_read, NULL, d);
    /* A new reader gets every active head repainted once. */
    for (unsigned i = 0; i < d->nheads; i++) {
        ShmHead *h = &d->heads[i];

        if (h->active) {
            shm_full_repaint(h);
        }
    }
}

static char *shm_default_socket(ShmDisplay *d)
{
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    g_autofree char *file = g_strdup_printf("%s.sock", d->name);

    if (!dir || !*dir) {
        dir = g_get_tmp_dir();
    }
    return g_build_filename(dir, file, NULL);
}

static bool shm_segment_create(ShmDisplay *d, Error **errp)
{
#ifdef _WIN32
    g_autofree char *name = g_strdup_printf("Local\\%s", d->name);

    d->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                    PAGE_READWRITE,
                                    (DWORD)((uint64_t)d->total >> 32),
                                    (DWORD)(d->total & 0xffffffffu), name);
    if (!d->mapping) {
        error_setg(errp, "cannot create file mapping %s: error %lu", name,
                   GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        error_setg(errp, "file mapping %s already exists", name);
        CloseHandle(d->mapping);
        d->mapping = NULL;
        return false;
    }
    d->base = MapViewOfFile(d->mapping, FILE_MAP_ALL_ACCESS, 0, 0, d->total);
    if (!d->base) {
        error_setg(errp, "cannot map %s: error %lu", name, GetLastError());
        return false;
    }
#else
    g_autofree char *name = g_strdup_printf("/%s", d->name);
    void *p;
    int fd;

    /* Replace a segment a crashed run left behind. */
    shm_unlink(name);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot create shared memory %s", name);
        return false;
    }
    d->created = true;
    if (ftruncate(fd, d->total) < 0) {
        error_setg_errno(errp, errno, "cannot size shared memory %s", name);
        close(fd);
        return false;
    }
    p = mmap(NULL, d->total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        error_setg_errno(errp, errno, "cannot map shared memory %s", name);
        return false;
    }
    d->base = p;
#endif
    return true;
}

static void shm_segment_destroy(ShmDisplay *d)
{
#ifdef _WIN32
    if (d->base) {
        UnmapViewOfFile(d->base);
    }
    if (d->mapping) {
        CloseHandle(d->mapping);
    }
    d->mapping = NULL;
#else
    if (d->created) {
        g_autofree char *name = g_strdup_printf("/%s", d->name);

        shm_unlink(name);
    }
    if (d->base) {
        munmap(d->base, d->total);
    }
#endif
    d->base = NULL;
    d->hdr = NULL;
    d->created = false;
}

static void shm_exit(Notifier *n, void *data)
{
    ShmDisplay *d = container_of(n, ShmDisplay, exit);

    shm_drop_client(d);
    if (d->listen_fd >= 0) {
        qemu_set_fd_handler(d->listen_fd, NULL, NULL, NULL);
        close(d->listen_fd);
        d->listen_fd = -1;
        unlink(d->sock_path);
    }
    /* The listeners stop with the process; they must not see a dead segment. */
    for (unsigned i = 0; i < d->nheads; i++) {
        qemu_console_unregister_listener(&d->heads[i].dcl);
    }
    d->nheads = 0;
    shm_segment_destroy(d);
}

static void shm_machine_done(Notifier *n, void *data)
{
    ShmDisplay *d = container_of(n, ShmDisplay, done);
    Error **errp = &error_fatal;
    Error *err = NULL;
    unsigned nheads = 0;
    QemuConsole *cons[SGI_SHM_MAX_HEADS];
#ifndef _WIN32
    mode_t old_umask;
#endif

    if (!d->name) {
        warn_report("sgi-display-shm: display-shm-socket is set without "
                    "display-shm, so nothing is published");
        return;
    }

    /* The heads are the graphic consoles that exist once the board is built. */
    for (unsigned i = 0; ; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);

        if (!con) {
            break;
        }
        if (!qemu_console_is_graphic(con)) {
            continue;
        }
        if (nheads == SGI_SHM_MAX_HEADS) {
            warn_report("sgi-display-shm: more than %d graphic consoles, "
                        "the rest are not published", SGI_SHM_MAX_HEADS);
            break;
        }
        cons[nheads++] = con;
    }
    if (!nheads) {
        warn_report("sgi-display-shm: this machine has no graphic console");
    }

    d->total = SGI_SHM_HEADER_BYTES +
               (size_t)nheads * SGI_SHM_SLOTS * SGI_SHM_SLOT_BYTES;
    d->exit.notify = shm_exit;
    qemu_add_exit_notifier(&d->exit);
    if (!shm_segment_create(d, errp)) {
        return;
    }
    d->hdr = (SGIShmHeader *)d->base;
    d->hdr->version = SGI_SHM_VERSION;
    d->hdr->nheads = nheads;
    d->hdr->slot_bytes = SGI_SHM_SLOT_BYTES;
    d->hdr->data_offset = SGI_SHM_HEADER_BYTES;
    d->hdr->total_bytes = d->total;

    if (!d->sock_path) {
        d->sock_path = shm_default_socket(d);
    }
#ifndef _WIN32
    /* Bind the socket for its owner only, without changing the process. */
    old_umask = umask(0077);
#endif
    d->listen_fd = unix_listen(d->sock_path, &err);
#ifndef _WIN32
    umask(old_umask);
#endif
    if (d->listen_fd < 0) {
        error_propagate(errp, err);
        return;
    }
    qemu_set_fd_handler(d->listen_fd, shm_accept, NULL, d);

    for (unsigned i = 0; i < nheads; i++) {
        ShmHead *h = &d->heads[i];

        h->d = d;
        h->idx = i;
        h->dcl.update_interval = SGI_SHM_REFRESH_MS;
        d->nheads++;
        qemu_console_register_listener(cons[i], &h->dcl, &shm_ops);
    }
    /* Everything is in place: let a reader that polls the segment in. */
    smp_wmb();
    qatomic_set(&d->hdr->magic, SGI_SHM_MAGIC);
    info_report("sgi-display-shm: %u head(s), segment %s, socket %s", nheads,
                d->name, d->sock_path);
}

static ShmDisplay *shm_get_display(void)
{
    if (!shm_display) {
        shm_display = g_new0(ShmDisplay, 1);
        shm_display->listen_fd = -1;
        shm_display->client_fd = -1;
        shm_display->done.notify = shm_machine_done;
        qemu_add_machine_init_done_notifier(&shm_display->done);
    }
    return shm_display;
}

static char *shm_get_name(Object *obj, Error **errp)
{
    return g_strdup(shm_display && shm_display->name ? shm_display->name : "");
}

static void shm_set_name(Object *obj, const char *value, Error **errp)
{
    ShmDisplay *d = shm_get_display();

    if (d->name) {
        error_setg(errp, "display-shm is set once");
        return;
    }
    if (HOST_BIG_ENDIAN) {
        error_setg(errp, "display-shm needs a little-endian host");
        return;
    }
    if (!*value || strlen(value) > SGI_SHM_NAME_MAX ||
        strspn(value, "abcdefghijklmnopqrstuvwxyz"
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
        strlen(value)) {
        error_setg(errp, "display-shm takes 1 to %d characters of "
                   "A-Z a-z 0-9 . _ -", SGI_SHM_NAME_MAX);
        return;
    }
    d->name = g_strdup(value);
}

static char *shm_get_socket(Object *obj, Error **errp)
{
    return g_strdup(shm_display && shm_display->sock_path ?
                    shm_display->sock_path : "");
}

static void shm_set_socket(Object *obj, const char *value, Error **errp)
{
    ShmDisplay *d = shm_get_display();

    g_free(d->sock_path);
    d->sock_path = *value ? g_strdup(value) : NULL;
}

void sgi_display_shm_class_add_properties(ObjectClass *oc)
{
    object_class_property_add_str(oc, "display-shm", shm_get_name,
                                  shm_set_name);
    object_class_property_set_description(oc, "display-shm",
        "Publish the graphic consoles through the shared-memory segment "
        "NAME (1 to 30 characters of A-Z a-z 0-9 . _ -) and tell a reader "
        "about frames over a local socket; see docs/system/mips/sgi/"
        "display-shm.rst");
    object_class_property_add_str(oc, "display-shm-socket", shm_get_socket,
                                  shm_set_socket);
    object_class_property_set_description(oc, "display-shm-socket",
        "Path of the display-shm socket; default NAME.sock in "
        "$XDG_RUNTIME_DIR, or in the temporary directory when that is unset");
}
