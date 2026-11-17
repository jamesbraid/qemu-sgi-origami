/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The display-shm producer's socket: listening, peer checks, framing, the
 * output queue with its handle window, and the session set-up of
 * docs/interop/display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "ui/display-shm-priv.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

static void viewer_read(void *opaque);
static void viewer_writable(void *opaque);
static void viewer_release_bh(void *opaque);
static void viewer_unref(DShmViewer *v);

/* ---- The output queue ------------------------------------------------- */

void dshm_out_free(DShmViewer *v, DShmOut *o)
{
#ifdef _WIN32
    dshm_win_discard(v, o);
#else
    if (DSHM_HANDLE_OK(o->h)) {
        close(o->h);
    }
#endif
    g_free(o->saved_damage);
    g_free(o->buf);
    g_free(o);
}

void dshm_out_remove(DShmViewer *v, DShmOut *o)
{
    if (v->partial == o) {
        v->partial = NULL;
    }
    if (o->urgent) {
        v->urgent_queued--;
    }
    QTAILQ_REMOVE(&v->out, o, next);
    v->out_bytes -= o->len;
    dshm_out_free(v, o);
}

static void viewer_update_handler(DShmViewer *v)
{
    if (v->fd < 0) {
        return;
    }
    qemu_set_fd_handler(v->fd, viewer_read,
                        v->want_write ? viewer_writable : NULL, v);
}

static bool out_sendable(DShmViewer *v, DShmOut *o)
{
    if (o->off) {
        return true;
    }
    if (v->hold_writes && !o->urgent) {
        return false;
    }
    if (DSHM_HANDLE_OK(o->h) &&
        v->handles_started - v->handles_seen >= DSHM_HANDLE_WINDOW) {
        return false;
    }
    return true;
}

static DShmOut *out_pick(DShmViewer *v)
{
    DShmOut *o;

    if (v->partial) {
        return v->partial;
    }
    /* Nothing half sent: start the first that may; the stream is ordered. */
    if (!v->hold_writes) {
        o = QTAILQ_FIRST(&v->out);
        return o && out_sendable(v, o) ? o : NULL;
    }
    /* Held: only urgent messages go, and with none queued there is no walk. */
    if (!v->urgent_queued) {
        return NULL;
    }
    QTAILQ_FOREACH(o, &v->out, next) {
        if (o->urgent) {
            return out_sendable(v, o) ? o : NULL;
        }
    }
    return NULL;
}

static ssize_t out_send(DShmViewer *v, const DShmOut *o)
{
#ifdef _WIN32
    return send(v->fd, (const char *)o->buf + o->off, o->len - o->off, 0);
#else
    struct iovec iov = { .iov_base = (void *)(o->buf + o->off),
                         .iov_len = o->len - o->off };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;

    if (DSHM_HANDLE_OK(o->h) && !o->off) {
        struct cmsghdr *c;

        memset(&ctl, 0, sizeof(ctl));
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof(ctl.buf);
        c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &o->h, sizeof(int));
    }
    return sendmsg(v->fd, &mh, MSG_NOSIGNAL);
#endif
}

void dshm_flush(DShmViewer *v)
{
    DShmOut *o;

    while (!v->closing && !v->doomed && (o = out_pick(v))) {
        bool first = !o->off;
        ssize_t n;

#ifdef _WIN32
        if (first && DSHM_HANDLE_OK(o->h) && !dshm_win_stage(v, o)) {
            dshm_viewer_doom(v, DSHM_ERR_INTERNAL, "cannot pass a handle");
            return;
        }
#endif
        n = out_send(v, o);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!v->want_write) {
                    v->want_write = true;
                    viewer_update_handler(v);
                }
                return;
            }
            dshm_viewer_doom(v, 0, NULL);
            return;
        }
        if (n > 0 && first) {
            if (DSHM_HANDLE_OK(o->h)) {
#ifdef _WIN32
                dshm_win_started(v, o);
#else
                close(o->h);
                o->h = DSHM_NO_HANDLE;
                v->handles_started++;
#endif
            }
            if (o->type == DSHM_MSG_FRAME) {
                dshm_frame_started(v, o);
            }
        }
        o->off += n;
        if (o->off < o->len) {
            v->partial = o;
            continue;
        }
        dshm_out_remove(v, o);
    }
    if (!v->closing && !v->doomed && v->want_write &&
        (QTAILQ_EMPTY(&v->out) || !out_pick(v))) {
        v->want_write = false;
        viewer_update_handler(v);
    }
}

static void viewer_writable(void *opaque)
{
    dshm_flush(opaque);
}

DShmOut *dshm_queue_ext(DShmViewer *v, const void *msg, size_t len,
                        bool urgent)
{
    DShmOut *o;

    if (v->closing || v->doomed) {
        return NULL;
    }
    assert(len >= sizeof(DShmHdr) && !(len % 8));
    if (v->out_bytes + len > DSHM_OUT_MAX_BYTES) {
        dshm_viewer_doom(v, DSHM_ERR_LIMIT, "output queue full");
        return NULL;
    }
    o = g_new0(DShmOut, 1);
    o->buf = g_memdup2(msg, len);
    o->len = len;
    o->h = DSHM_NO_HANDLE;
    o->urgent = urgent;
    v->urgent_queued += urgent;
    o->type = lduw_le_p(o->buf);
    stl_le_p(o->buf + 4, len);
    QTAILQ_INSERT_TAIL(&v->out, o, next);
    v->out_bytes += len;
    return o;
}

static void handle_drop(DShmHandle h)
{
    if (DSHM_HANDLE_OK(h)) {
#ifdef _WIN32
        CloseHandle(h);
#else
        close(h);
#endif
    }
}

/* Queue msg; a duplicate handle, if given, is owned by the queue and sent. */
DShmOut *dshm_queue(DShmViewer *v, const void *msg, size_t len, DShmHandle h,
                    bool tail_handle)
{
    DShmOut *o;

    if (v->closing || v->doomed) {
        handle_drop(h);
        return NULL;
    }
    o = dshm_queue_ext(v, msg, len, false);
    if (!o) {
        handle_drop(h);
        return NULL;
    }
    if (DSHM_HANDLE_OK(h)) {
        o->h = h;
        stw_le_p(o->buf + 2, lduw_le_p(o->buf + 2) | DSHM_F_HANDLE);
        if (tail_handle) {
            stq_le_p(o->buf + len - 8, 0);
        }
    }
    return o;
}

/* ---- Closing ---------------------------------------------------------- */

/* A closing viewer stays in the table, skipped, until the loop is clear. */
static void viewer_unlist(DShmViewer *v)
{
    DisplayShm *d = v->d;

    for (unsigned i = 0; i < d->n_pending; i++) {
        if (d->pending[i] == v) {
            d->pending[i] = d->pending[--d->n_pending];
            return;
        }
    }
}

/* Close, sending ERROR first if the stream is at a message boundary. */
void dshm_viewer_close(DShmViewer *v, uint32_t code, const char *text)
{
    DShmOut *o;

    if (v->closing) {
        return;
    }
    if (code) {
        bool mid = false;

        QTAILQ_FOREACH(o, &v->out, next) {
            mid |= o->off != 0;
        }
        if (!mid) {
            size_t tl = text ? strlen(text) : 0;
            size_t len = ROUND_UP(sizeof(DShmError) + tl, 8);
            g_autofree uint8_t *buf = g_malloc0(len);
            DShmError *e = (DShmError *)buf;

            e->hdr.type = DSHM_MSG_ERROR;
            e->hdr.length = len;
            e->code = code;
            e->text_len = tl;
            if (code == DSHM_ERR_VERSION) {
                e->major_min = e->major_max = DSHM_MAJOR;
            }
            memcpy(buf + sizeof(DShmError), text, tl);
            /* Best effort: the viewer may read it or may not. */
            if (send(v->fd, (const char *)buf, len,
                     MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
                /* a closing connection has nothing to wait for */
            }
        }
    }
    v->closing = true;
    qemu_set_fd_handler(v->fd, NULL, NULL, NULL);
#ifdef _WIN32
    /* A plain close is not seen by the peer on a Windows AF_UNIX socket. */
    shutdown(v->fd, SD_BOTH);
#endif
    close(v->fd);
    v->fd = -1;
    viewer_unlist(v);
    v->partial = NULL;
    while ((o = QTAILQ_FIRST(&v->out))) {
        QTAILQ_REMOVE(&v->out, o, next);
        v->out_bytes -= o->len;
        v->urgent_queued -= o->urgent;
        /* A FRAME dropped here was never sent: its lease goes with the rest. */
        dshm_out_free(v, o);
    }
    if (v->hello) {
        dshm_viewer_leave(v);
#ifdef _WIN32
        dshm_win_depart(v);
#endif
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), viewer_release_bh, v);
}

static void viewer_unref(DShmViewer *v)
{
    if (--v->refs) {
        return;
    }
    g_hash_table_destroy(v->announced);
#ifdef _WIN32
    dshm_win_viewer_free(v);
#endif
    g_free(v->in);
    g_free(v->vh);
    g_free(v);
}

static void viewer_release_bh(void *opaque)
{
    DShmViewer *v = opaque;
    DisplayShm *d = v->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        if (d->viewers[i] == v) {
            d->viewers[i] = d->viewers[--d->n_viewers];
            break;
        }
    }
    viewer_unref(v);
}

static void viewer_doom_bh(void *opaque)
{
    DShmViewer *v = opaque;

    if (!v->closing) {
        dshm_viewer_close(v, v->doom_code, v->doom_code ? v->doom_text : NULL);
    }
    viewer_unref(v);
}

/* Close from the main loop, for callers walking tables a close changes. */
void dshm_viewer_doom(DShmViewer *v, uint32_t code, const char *text)
{
    if (v->closing || v->doomed) {
        return;
    }
    v->doomed = true;
    v->doom_code = code;
    snprintf(v->doom_text, sizeof(v->doom_text), "%s", text ? text : "");
    v->refs++;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), viewer_doom_bh, v);
}

/* ---- Regions as the viewer sees them ---------------------------------- */

#ifdef _WIN32
/* The slot's shared fence as a REGION of its own; the slot's names it. */
static bool region_announce_fence(DShmViewer *v, DShmRegion *r)
{
    DShmRegionMsg m = { .hdr = { .type = DSHM_MSG_REGION,
                                 .flags = DSHM_F_MUST },
                        .id = r->fence_id, .kind = DSHM_KIND_FENCE,
                        .storage = DSHM_STORAGE_WIN32,
                        .access = DSHM_ACCESS_READ | DSHM_ACCESS_WRITE };
    uint8_t buf[sizeof(m) + 8] = { };
    HANDLE h = dshm_region_dup_fence(r);
    DShmOut *o;

    if (!h) {
        return false;
    }
    memcpy(buf, &m, sizeof(m));
    o = dshm_queue(v, buf, sizeof(buf), h, true);
    if (!o) {
        return false;
    }
    o->region = r->fence_id;
    g_hash_table_add(v->announced, GUINT_TO_POINTER(r->fence_id));
    return true;
}
#endif

bool dshm_region_announce(DShmViewer *v, DShmRegion *r, uint32_t access)
{
    DShmRegionMsg m = { .hdr = { .type = DSHM_MSG_REGION,
                                 .flags = DSHM_F_MUST },
                        .id = r->id, .kind = r->kind,
                        .storage = DSHM_STORAGE_SHM, .access = access,
                        .size = r->size };
    uint8_t buf[sizeof(m) + sizeof(DShmGpuRegion) + 8] = { };
    size_t len = sizeof(m) + 8;
    DShmOut *o;
    DShmHandle h = dshm_region_dup(r, access);

    if (!DSHM_HANDLE_OK(h)) {
        return false;
    }
    if (r->gpu) {
        const DisplayShmGpuSlot *g = &r->gpu_slot;
#ifdef _WIN32
        DShmWin32Region p = { .fourcc = g->fourcc, .flags = DSHM_GPU_WRITTEN,
                              .adapter_luid = g->adapter_luid,
                              .fence_region = r->fence_id,
                              .stride = g->stride, .width = g->width,
                              .height = g->height };

        /* the fence goes first, so the viewer has it when the slot names it */
        if (r->fence_id &&
            !g_hash_table_contains(v->announced,
                                   GUINT_TO_POINTER(r->fence_id)) &&
            !region_announce_fence(v, r)) {
            handle_drop(h);
            return false;
        }
#else
        DShmGpuRegion p = { .fourcc = g->fourcc, .flags = DSHM_GPU_WRITTEN,
                            .modifier = g->modifier, .stride = g->stride,
                            .width = g->width, .height = g->height };
#endif

        m.storage = DSHM_GPU_CLASS;
        m.payload_len = sizeof(p);
        memcpy(buf + sizeof(m), &p, sizeof(p));
        len += sizeof(p);
    }
    memcpy(buf, &m, sizeof(m));
    o = dshm_queue(v, buf, len, h, true);
    if (!o) {
        return false;
    }
    o->region = r->id;
    g_hash_table_add(v->announced, GUINT_TO_POINTER(r->id));
    return true;
}

#ifdef _WIN32
/* The slot's fence goes with it, the same way. */
static void region_forget_fence(DShmViewer *v, const DShmRegion *r)
{
    bool announced = g_hash_table_remove(v->announced,
                                         GUINT_TO_POINTER(r->fence_id));
    bool unsent = false;
    DShmOut *o, *next;

    QTAILQ_FOREACH_SAFE(o, &v->out, next, next) {
        if (!o->off && o->type == DSHM_MSG_REGION && o->region == r->fence_id) {
            unsent = true;
            dshm_out_remove(v, o);
        }
    }
    if (announced && !unsent) {
        DShmRegionGone g = { .hdr = { .type = DSHM_MSG_REGION_GONE },
                             .id = r->fence_id };

        dshm_queue(v, &g, sizeof(g), DSHM_NO_HANDLE, false);
    }
}
#endif

/* Cancel an unsent REGION and what names it, or send REGION_GONE. */
void dshm_region_forget(DShmViewer *v, DShmRegion *r)
{
    DECLARE_BITMAP(heads, DSHM_FEEDBACK_HEADS);
    DShmOut *o, *next;
    bool announced, unsent = false;

    if (v->in_forget || v->closing) {
        /* a cancelled FRAME freed its slot, which is what we are doing */
        return;
    }
    v->in_forget = true;
    announced = g_hash_table_remove(v->announced, GUINT_TO_POINTER(r->id));
    bitmap_zero(heads, DSHM_FEEDBACK_HEADS);
    QTAILQ_FOREACH_SAFE(o, &v->out, next, next) {
        if (o->off) {
            continue;
        }
        if (o->type == DSHM_MSG_REGION && o->region == r->id) {
            unsent = true;
            dshm_out_remove(v, o);
        } else if (o->type == DSHM_MSG_FRAME && o->region == r->id) {
            dshm_frame_cancel(v, o);
            dshm_out_remove(v, o);
        } else if (o->type == DSHM_MSG_HEAD) {
            const DShmHeadMsg *hm = (const DShmHeadMsg *)o->buf;
            bool hit = false;

            for (unsigned i = 0; i < hm->n_slots; i++) {
                hit |= hm->slot_region[i] == r->id;
            }
            if (hit) {
                if (hm->head < DSHM_FEEDBACK_HEADS) {
                    set_bit(hm->head, heads);
                }
                dshm_out_remove(v, o);
            }
        }
    }
    if (announced && !unsent) {
        DShmRegionGone g = { .hdr = { .type = DSHM_MSG_REGION_GONE },
                             .id = r->id };

        dshm_queue(v, &g, sizeof(g), DSHM_NO_HANDLE, false);
    }
#ifdef _WIN32
    if (r->fence_id) {
        region_forget_fence(v, r);
    }
#endif
    for (unsigned h = find_first_bit(heads, DSHM_FEEDBACK_HEADS);
         h < DSHM_FEEDBACK_HEADS && !v->closing;
         h = find_next_bit(heads, DSHM_FEEDBACK_HEADS, h + 1)) {
        DShmHead *hd = dshm_head(v->d, h);

        if (hd) {
            dshm_head_send_to(v, hd);
        }
    }
    v->in_forget = false;
}

unsigned dshm_live_viewers(DisplayShm *d)
{
    unsigned n = 0;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        n += !d->viewers[i]->closing;
    }
    return n;
}

DShmViewer *dshm_viewer_by_id(DisplayShm *d, uint32_t id)
{
    for (unsigned i = 0; i < d->n_viewers; i++) {
        if (d->viewers[i]->id == id && !d->viewers[i]->closing) {
            return d->viewers[i];
        }
    }
    return NULL;
}

/* ---- Messages in ------------------------------------------------------ */

static size_t core_min_len(uint16_t type)
{
    switch (type) {
    case DSHM_MSG_HELLO:
        return sizeof(DShmHello);
    case DSHM_MSG_REQUEST:
        return sizeof(DShmRequest);
    case DSHM_MSG_IMPORT_FAILED:
        return sizeof(DShmImportFailed);
    case DSHM_MSG_HANDLES_SEEN:
        return sizeof(DShmHandlesSeen);
    case DSHM_MSG_SET_STORAGE:
        return sizeof(DShmSetStorage);
    case DSHM_MSG_INPUT_CLAIM:
        return sizeof(DShmInputClaim);
    case DSHM_MSG_KEY:
    case DSHM_MSG_BUTTON:
        return sizeof(DShmKey);
    case DSHM_MSG_REL:
    case DSHM_MSG_ABS:
        return sizeof(DShmMotion);
    case DSHM_MSG_SYNC:
        return sizeof(DShmSync);
    case DSHM_MSG_INPUT_RESET:
        return sizeof(DShmHdr);
    case DSHM_MSG_ERROR:
        return sizeof(DShmError);
    default:
        return 0;
    }
}

/* The offer of name with version 1, or NULL; *bad if the offers overrun. */
static const uint8_t *offer_find(const uint8_t *offers, size_t left,
                                 unsigned n, const char *name, bool *bad)
{
    size_t off = 0;
    const uint8_t *found = NULL;

    for (unsigned i = 0; i < n; i++) {
        DShmOffer of;
        size_t total;

        if (left - off < sizeof(of)) {
            *bad = true;
            return NULL;
        }
        memcpy(&of, offers + off, sizeof(of));
        total = sizeof(of) + ROUND_UP((size_t)of.payload_len, 8);
        if (total > left - off) {
            *bad = true;
            return NULL;
        }
        if (!strncmp((const char *)of.name, name, sizeof(of.name)) &&
            of.version_min <= 1 && of.version_max >= 1) {
            found = offers + off;
        }
        off += total;
    }
    return found;
}

static bool offers_have(const uint8_t *offers, size_t left, unsigned n,
                        const char *name, bool *bad)
{
    return offer_find(offers, left, n, name, bad) != NULL;
}


static void viewer_hello(DShmViewer *v, const DShmHdr *hdr,
                         const uint8_t *msg)
{
    DisplayShm *d = v->d;
    DShmHello h;
    DShmWelcome w = { .hdr = { .type = DSHM_MSG_WELCOME,
                               .flags = DSHM_F_MUST } };
    g_autofree uint8_t *wb = NULL;
    size_t wlen = sizeof(w), pos;
    const uint8_t *offers = msg + sizeof(h);
    size_t left = hdr->length - sizeof(h);
    bool test_ok, steer_ok, gpu_ok = false, bad = false;
    unsigned idx = 0, n_acc = 0, gpu_idx = 0;
    Error *err = NULL;

    memcpy(&h, msg, sizeof(h));
    if (h.major != DSHM_MAJOR) {
        dshm_viewer_close(v, DSHM_ERR_VERSION, "version");
        return;
    }
    if (h.credit < 1 || h.credit > DSHM_MAX_CREDIT ||
        h.n_ext > 64) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "HELLO fields");
        return;
    }
    {
        bool has_shm = false, term = false;

        for (unsigned i = 0; i < 8 && !term; i++) {
            if (!h.storage[i]) {
                term = true;
            }
            has_shm |= h.storage[i] == DSHM_STORAGE_SHM;
        }
        if (!has_shm || !term) {
            dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "HELLO storage list");
            return;
        }
    }
    test_ok = offers_have(offers, left, h.n_ext, DSHM_EXT_TEST, &bad);
    steer_ok = offers_have(offers, left, h.n_ext, DSHM_EXT_STEER, &bad);
    if (DSHM_GPU_CLASS) {
        gpu_ok = offers_have(offers, left, h.n_ext, DSHM_GPU_EXT, &bad) &&
                 dshm_storage_has(h.storage, DSHM_GPU_CLASS);
    }
    if (bad) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "extension offers overrun");
        return;
    }
    /* closed viewers hold table slots until the main loop is clear of them */
    if (dshm_live_viewers(d) >= d->max_viewers ||
        d->n_viewers >= DSHM_VIEWERS_MAX) {
        dshm_viewer_close(v, DSHM_ERR_LIMIT, "too many viewers");
        return;
    }
    /* From here the viewer is a viewer. */
    for (unsigned i = 0; i < d->n_pending; i++) {
        if (d->pending[i] == v) {
            d->pending[i] = d->pending[--d->n_pending];
            break;
        }
    }
    d->viewers[d->n_viewers++] = v;
    v->hello = true;
    v->id = d->next_viewer_id++;
    memcpy(v->name, h.name, 32);
    v->name[32] = 0;
    v->features = h.features & DSHM_FEAT_ALL;
    if (!d->x_test) {
        v->features &= ~DSHM_FEAT_TEST;
    }
    memcpy(v->storage, h.storage, sizeof(v->storage));
    v->credit = h.credit;
    v->max_width = h.max_width ? h.max_width : UINT32_MAX;
    v->max_height = h.max_height ? h.max_height : UINT32_MAX;
    v->ext_test = d->x_test && (v->features & DSHM_FEAT_TEST) && test_ok;
    v->ext_steer = steer_ok && dshm_steer_wanted(d);
    if (v->ext_test) {
        v->test_idx = ++idx;
        n_acc++;
    }
    if (v->ext_steer) {
        v->steer_idx = ++idx;
        n_acc++;
    }
    v->ext_gpu = gpu_ok;
    if (v->ext_gpu) {
        gpu_idx = ++idx;
        n_acc++;
    }

    /* The feedback region is this viewer's own. */
    v->feedback = dshm_region_new(d, DSHM_KIND_FEEDBACK, DSHM_REGION_ALIGN,
                                  true, &err);
    if (!v->feedback) {
        error_report_err(err);
        dshm_viewer_close(v, DSHM_ERR_LIMIT, "no memory for feedback");
        return;
    }
    v->fbh = (DShmFeedbackHeader *)v->feedback->ptr;
    v->fbh->magic = DSHM_FEEDBACK_MAGIC;
    v->fbh->major = DSHM_MAJOR;
    v->fbh->head_offset = sizeof(DShmFeedbackHeader);
    v->fbh->head_stride = sizeof(DShmFeedbackHead);
    v->fbh->head_capacity = d->head_capacity;

    w.major = DSHM_MAJOR;
    w.minor = MIN(h.minor, DSHM_MINOR);
    w.features = v->features;
    w.session_id = d->session_id;
    w.viewer_id = v->id;
    w.state_region = d->state->id;
    w.feedback_region = v->feedback->id;
    w.credit = v->credit;
    w.max_viewers = d->max_viewers;
    w.n_ext = n_acc;
    snprintf((char *)w.name, sizeof(w.name), "qemu display-shm 4.0");
    wlen += n_acc * sizeof(DShmAccept);
    if (v->ext_steer) {
        wlen += 8;
    }
    if (v->ext_gpu) {
        wlen += sizeof(DShmGpuAccept);
    }
    wb = g_malloc0(wlen);
    memcpy(wb, &w, sizeof(w));
    pos = sizeof(w);
    if (v->ext_test) {
        DShmAccept a = { .version = 1, .index = v->test_idx };

        strcpy((char *)a.name, DSHM_EXT_TEST);
        memcpy(wb + pos, &a, sizeof(a));
        pos += sizeof(a);
    }
    if (v->ext_steer) {
        DShmAccept a = { .version = 1, .index = v->steer_idx,
                         .payload_len = 8 };

        strcpy((char *)a.name, DSHM_EXT_STEER);
        memcpy(wb + pos, &a, sizeof(a));
        pos += sizeof(a);
        stl_le_p(wb + pos, d->steer_block_offset);
        stl_le_p(wb + pos + 4, DSHM_STEER_BLOCK_BYTES);
        pos += 8;
    }
    if (v->ext_gpu) {
        DShmAccept a = { .version = 1, .index = gpu_idx,
                         .payload_len = sizeof(DShmGpuAccept) };

        strcpy((char *)a.name, DSHM_GPU_EXT);
        memcpy(wb + pos, &a, sizeof(a));
        pos += sizeof(a);
        stl_le_p(wb + pos, DSHM_ALLOC_GPU);
        pos += sizeof(DShmGpuAccept);
    }
    dshm_queue(v, wb, wlen, DSHM_NO_HANDLE, false);
#ifdef _WIN32
    if (!dshm_win_hello(v, &h)) {
        return;
    }
#endif
    dshm_region_announce(v, d->state, DSHM_ACCESS_READ);
    dshm_region_announce(v, v->feedback, DSHM_ACCESS_READ | DSHM_ACCESS_WRITE);
    dshm_viewer_join(v);
    dshm_flush(v);
}

static bool msg_ok_before_hello(uint16_t type)
{
    return type == DSHM_MSG_HELLO || type == DSHM_MSG_ERROR;
}

void dshm_message(DShmViewer *v, const DShmHdr *hdr, const uint8_t *msg)
{
    size_t min = core_min_len(hdr->type);
    unsigned idx = hdr->type >> 8;

    if (hdr->flags & DSHM_F_HANDLE) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "handle from a viewer");
        return;
    }
    if (!v->hello && !msg_ok_before_hello(hdr->type)) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "message before HELLO");
        return;
    }
    if (v->hello && hdr->type == DSHM_MSG_HELLO) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "second HELLO");
        return;
    }
    if (idx) {
        if (idx == v->test_idx && v->ext_test) {
            dshm_test_message(v, hdr->type & 0xff, msg, hdr->length);
            return;
        }
        goto unknown;
    }
    if (!min) {
        goto unknown;
    }
    if (hdr->length < min) {
        dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "short message");
        return;
    }
    switch (hdr->type) {
    case DSHM_MSG_HELLO:
        viewer_hello(v, hdr, msg);
        return;
    case DSHM_MSG_ERROR:
        dshm_viewer_close(v, 0, NULL);
        return;
    case DSHM_MSG_HANDLES_SEEN: {
        DShmHandlesSeen m;

        memcpy(&m, msg, sizeof(m));
        if (m.count < v->handles_seen || m.count > v->handles_started) {
            dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "HANDLES_SEEN");
            return;
        }
        v->handles_seen = m.count;
        dshm_flush(v);
        return;
    }
    case DSHM_MSG_REQUEST: {
        DShmRequest m;

        memcpy(&m, msg, sizeof(m));
        if (v->ready && m.head < v->n_vh && v->vh[m.head] &&
            v->d->heads[m.head]->present) {
            dshm_vh_request(v->vh[m.head], &m);
        }
        return;
    }
    case DSHM_MSG_IMPORT_FAILED: {
        DShmImportFailed m;

        memcpy(&m, msg, sizeof(m));
        dshm_import_failed(v, m.region, m.reason);
        return;
    }
    case DSHM_MSG_SET_STORAGE: {
        DShmSetStorage m;

        memcpy(&m, msg, sizeof(m));
        for (unsigned i = 0; i < v->n_vh; i++) {
            if (v->vh[i] && (m.head == 0xffffffffu || m.head == i)) {
                dshm_vh_set_storage(v->vh[i], m.storage);
            }
        }
        return;
    }
    default:
        dshm_input_message(v, hdr, msg);
        return;
    }
unknown:
    if (hdr->flags & DSHM_F_MUST) {
        dshm_viewer_close(v, DSHM_ERR_UNSUPPORTED, "unsupported message");
    }
}

static void viewer_read(void *opaque)
{
    DShmViewer *v = opaque;
    DisplayShm *d = v->d;
#ifndef _WIN32
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * 8)];
    } ctl;
    struct iovec iov;
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = ctl.buf,
                         .msg_controllen = sizeof(ctl.buf) };
#endif
    size_t off = 0;
    ssize_t n;

#ifdef _WIN32
    n = recv(v->fd, (char *)v->in + v->in_len, DSHM_MSG_MAX - v->in_len, 0);
#else
    iov.iov_base = v->in + v->in_len;
    iov.iov_len = DSHM_MSG_MAX - v->in_len;
    n = recvmsg(v->fd, &mh, MSG_CMSG_CLOEXEC);
#endif
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR)) {
        dshm_viewer_close(v, 0, NULL);
        return;
    }
    if (n < 0) {
        return;
    }
#ifndef _WIN32
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);

            for (size_t i = 0; i < cnt; i++) {
                int fd;

                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(fd));
                close(fd);
            }
        }
    }
#endif
    v->in_len += n;
    while (!v->closing && v->in_len - off >= sizeof(DShmHdr)) {
        DShmHdr hdr;

        memcpy(&hdr, v->in + off, sizeof(hdr));
        if (!v->hello && hdr.type == DSHM_MSG_HELLO && !hdr.length) {
            warn_report("display-shm: version 3 viewer, closing");
            dshm_viewer_close(v, 0, NULL);
            return;
        }
        if (hdr.length < sizeof(hdr) || hdr.length > DSHM_MSG_MAX ||
            (hdr.length % 8)) {
            dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "bad message length");
            return;
        }
        if (v->in_len - off < hdr.length) {
            break;
        }
        dshm_message(v, &hdr, v->in + off);
        off += hdr.length;
    }
    if (!v->closing) {
        v->in_len -= off;
        memmove(v->in, v->in + off, v->in_len);
    }
    dshm_viewer_refresh_gone(v);
    dshm_eval_flagged_all(d);
}

/* ---- Listening -------------------------------------------------------- */

#ifndef _WIN32
static bool peer_ok(DisplayShm *d, int fd)
{
#ifdef CONFIG_LINUX
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        return false;
    }
    if (cred.uid == geteuid()) {
        return true;
    }
    if (d->allow_uid && *d->allow_uid) {
        g_auto(GStrv) ids = g_strsplit(d->allow_uid, ",", -1);

        for (unsigned i = 0; ids[i]; i++) {
            if ((uid_t)g_ascii_strtoull(ids[i], NULL, 10) == cred.uid) {
                return true;
            }
        }
    }
    return false;
#else
    uid_t euid;
    gid_t egid;

    if (getpeereid(fd, &euid, &egid) < 0) {
        return false;
    }
    return euid == geteuid();
#endif
}
#endif /* !_WIN32 */

static void accept_ready(void *opaque)
{
    DisplayShm *d = opaque;
    int fd = qemu_accept(d->listen_fd, NULL, NULL);
    DShmViewer *v;
    bool ok;

    if (fd < 0) {
        return;
    }
    qemu_set_blocking(fd, false, NULL);
    v = g_new0(DShmViewer, 1);
    v->refs = 1;
    v->d = d;
    v->fd = fd;
#ifdef _WIN32
    ok = dshm_win_peer(v);
#else
    ok = peer_ok(d, fd);
#endif
    if (!ok) {
        const DShmError e = { .hdr = { .type = DSHM_MSG_ERROR,
                                       .length = sizeof(DShmError) },
                              .code = DSHM_ERR_PERMISSION };

        if (send(fd, (const char *)&e, sizeof(e),
                 MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            /* refused either way */
        }
        close(fd);
        v->refs = 0;
#ifdef _WIN32
        dshm_win_viewer_free(v);
#endif
        g_free(v);
        return;
    }
    if (d->n_pending >= DSHM_PENDING_MAX) {
        close(fd);
#ifdef _WIN32
        dshm_win_viewer_free(v);
#endif
        g_free(v);
        return;
    }
    v->in = g_malloc(DSHM_MSG_MAX);
    v->announced = g_hash_table_new(NULL, NULL);
    QTAILQ_INIT(&v->out);
    d->pending = g_renew(DShmViewer *, d->pending, d->n_pending + 1);
    d->pending[d->n_pending++] = v;
    qemu_set_fd_handler(fd, viewer_read, NULL, v);
}

static bool dir_ok(const char *path, Error **errp)
{
    g_autofree char *dir = g_path_get_dirname(path);
    struct stat st;

    if (stat(dir, &st) < 0 && errno == ENOENT) {
#ifdef _WIN32
        if (g_mkdir_with_parents(dir, 0700) < 0) {
#else
        if (mkdir(dir, 0700) < 0) {
#endif
            error_setg_errno(errp, errno, "cannot create %s", dir);
            return false;
        }
    }
    if (stat(dir, &st) < 0) {
        error_setg_errno(errp, errno, "cannot stat %s", dir);
        return false;
    }
#ifdef _WIN32
    /* No owner or mode bits here: the peer's token is checked at accept. */
    if (!S_ISDIR(st.st_mode)) {
#else
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 022)) {
#endif
        error_setg(errp, "display-shm: %s must be a directory owned by this "
                   "user that no one else can write", dir);
        return false;
    }
    return true;
}

void dshm_transport_start(DisplayShm *d, Error **errp)
{
    if (d->listen_fd_prop >= 0) {
#ifdef _WIN32
        error_setg(errp, "display-shm: fd is not supported on Windows, "
                   "use path");
        return;
#endif
        d->listen_fd = d->listen_fd_prop;
        qemu_set_blocking(d->listen_fd, false, NULL);
    } else {
        int probe;
#ifndef _WIN32
        struct stat st;
#endif

        if (!d->path || !*d->path) {
            error_setg(errp, "display-shm needs fd or path");
            return;
        }
        if (!dir_ok(d->path, errp)) {
            return;
        }
        probe = unix_connect(d->path, NULL);
        if (probe >= 0) {
            close(probe);
            error_setg(errp, "display-shm: %s is in use", d->path);
            return;
        }
#ifdef _WIN32
        /* Nothing answered the probe: a file left by a crash blocks bind. */
        unlink(d->path);
#else
        if (lstat(d->path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            unlink(d->path);
        }
#endif
        d->listen_fd = unix_listen(d->path, errp);
        if (d->listen_fd < 0) {
            return;
        }
#ifndef _WIN32
        if (chmod(d->path, 0600) < 0) {
            error_setg_errno(errp, errno, "cannot restrict %s", d->path);
            close(d->listen_fd);
            d->listen_fd = -1;
            return;
        }
#endif
        d->bound_path = g_strdup(d->path);
    }
    qemu_set_fd_handler(d->listen_fd, accept_ready, NULL, d);
}

void dshm_transport_stop(DisplayShm *d)
{
    if (d->listen_fd >= 0) {
        qemu_set_fd_handler(d->listen_fd, NULL, NULL, NULL);
        close(d->listen_fd);
        d->listen_fd = -1;
        if (d->bound_path) {
            unlink(d->bound_path);
        }
    }
    for (unsigned i = 0; i < d->n_viewers; i++) {
        dshm_viewer_close(d->viewers[i], DSHM_ERR_SHUTDOWN, "exiting");
    }
    while (d->n_pending) {
        dshm_viewer_close(d->pending[0], 0, NULL);
    }
}
