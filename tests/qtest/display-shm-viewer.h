/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A C viewer for the display protocol version 4, for qtests: connect, take
 * the regions as passed descriptors, map them, read frames and send input.
 * It reads the wire from the documented offsets and imports nothing from
 * the producer's headers except the layout constants, so a producer bug in
 * a structure cannot hide in a viewer that shares it.
 */
#ifndef TESTS_QTEST_DISPLAY_SHM_VIEWER_H
#define TESTS_QTEST_DISPLAY_SHM_VIEWER_H

#include "qemu/osdep.h"
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#endif
#include "qemu/bswap.h"
#include "ui/display-shm-proto.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define V_WAIT_MS   10000
#define V_QUIET_MS  300
#define V_HEADS     8
#define V_REGIONS   128
#define V_MAX_PENDING 40000

/* A passed descriptor, or on Windows a section handle valid in this process */
#ifdef _WIN32
typedef HANDLE VHandle;
#define V_NO_HANDLE NULL
#define V_HANDLE_OK(h) ((h) != NULL)
#else
typedef int VHandle;
#define V_NO_HANDLE (-1)
#define V_HANDLE_OK(h) ((h) >= 0)
#endif

typedef struct VMsg {
    uint16_t type, flags;
    uint32_t len;
    uint8_t *d;
    VHandle h;
} VMsg;

typedef struct VRegion {
    uint32_t id, kind, access, storage;
    uint32_t fourcc, gflags, gstride;   /* a dma-buf's payload */
    uint64_t size;
    VHandle h;
    uint8_t *map;               /* mapped read-only on demand */
    bool gone;
} VRegion;

typedef struct VHead {
    bool seen, present;
    uint32_t flags, mode_serial, width, height, mhz, fourcc, stride;
    uint32_t n_slots, slot[16], pointer_mode, credit;
    uint32_t heads_seen;
} VHead;

typedef struct VFrame {
    uint32_t head, region;
    uint64_t serial;
    uint32_t mode_serial;
    uint16_t damage_kind, damage_count;
    uint64_t sample_ns, publish_ns, guest_ns;
    uint16_t lease, flags;
    uint64_t lease_seq;
    uint8_t damage[2048];
    size_t damage_len;
} VFrame;

typedef struct Viewer {
    int fd;
    uint32_t id, granted_credit, features, credit_asked;
    uint64_t session;
    uint32_t state_region, feedback_region, max_viewers;
    VRegion reg[V_REGIONS];
    VHead head[V_HEADS];
#ifndef _WIN32
    int fdq[256];
    unsigned fdq_n;
#endif
    VMsg pending[V_MAX_PENDING];
    unsigned n_pending;
    uint8_t *in;
    size_t in_len;
    uint8_t *state;
    uint8_t *feedback;
    size_t state_size;
    unsigned test_idx, steer_idx, steer_block_off;
    uint32_t gpu_alloc;         /* linux.dmabuf accepted: the allocator */
    uint64_t handles_parsed;
    uint64_t handles_taken;     /* trailing fields read, Windows */
    uint32_t last_orphans;      /* entries of the last ORPHANS, Windows */
    bool closed;
    bool ready;
} Viewer;

static inline uint32_t le32(const uint8_t *p) { return ldl_le_p(p); }
static inline uint64_t le64(const uint8_t *p) { return ldq_le_p(p); }
static inline uint16_t le16(const uint8_t *p) { return lduw_le_p(p); }

static inline int v_connect_fd(const char *path)
{
    gint64 end = g_get_monotonic_time() + V_WAIT_MS * 1000;
    struct sockaddr_un un = { .sun_family = AF_UNIX };

    g_assert_cmpuint(strlen(path), <, sizeof(un.sun_path));
    strcpy(un.sun_path, path);
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);

        g_assert_cmpint(fd, >=, 0);
        if (connect(fd, (struct sockaddr *)&un, sizeof(un)) == 0) {
            return fd;
        }
        close(fd);
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(20000);
    }
}

static inline void v_send(Viewer *v, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = send(v->fd, (const void *)p, len, MSG_NOSIGNAL);

        g_assert_cmpint(n, >, 0);
        p += n;
        len -= n;
    }
}

/* A message with the header filled in. */
static inline void v_send_msg(Viewer *v, uint16_t type, uint16_t flags,
                       const void *body, size_t body_len)
{
    size_t len = 8 + ROUND_UP(body_len, 8);
    uint8_t *b = g_malloc0(len);

    stw_le_p(b, type);
    stw_le_p(b + 2, flags);
    stl_le_p(b + 4, len);
    if (body_len) {
        memcpy(b + 8, body, body_len);
    }
    v_send(v, b, len);
    g_free(b);
}

static inline void v_handle_close(VHandle h)
{
    if (V_HANDLE_OK(h)) {
#ifdef _WIN32
        CloseHandle(h);
#else
        close(h);
#endif
    }
}

static inline void v_msg_free(VMsg *m)
{
    g_free(m->d);
    v_handle_close(m->h);
    m->d = NULL;
    m->h = V_NO_HANDLE;
}

static inline void v_unmap(VRegion *r)
{
    if (r->map) {
#ifdef _WIN32
        UnmapViewOfFile(r->map);
#else
        munmap(r->map, r->size);
#endif
        r->map = NULL;
    }
}

static inline VRegion *v_region(Viewer *v, uint32_t id)
{
    for (unsigned i = 0; i < V_REGIONS; i++) {
        if (v->reg[i].id == id && !v->reg[i].gone) {
            return &v->reg[i];
        }
    }
    return NULL;
}

static inline uint8_t *v_map(Viewer *v, uint32_t id, bool writable)
{
    VRegion *r = v_region(v, id);

    g_assert_nonnull(r);
    if (!r->map) {
#ifdef _WIN32
        void *p = MapViewOfFile(r->h, writable ? FILE_MAP_READ |
                                FILE_MAP_WRITE : FILE_MAP_READ, 0, 0,
                                r->size);

        g_assert(p);
#else
        void *p = mmap(NULL, r->size, writable ? PROT_READ | PROT_WRITE :
                       PROT_READ, MAP_SHARED, r->h, 0);

        g_assert(p != MAP_FAILED);
#endif
        r->map = p;
    }
    return r->map;
}

static inline void v_apply_head(Viewer *v, const uint8_t *d)
{
    uint32_t h = le32(d + 8);
    VHead *vh;

    g_assert_cmpuint(h, <, V_HEADS);
    vh = &v->head[h];
    vh->seen = true;
    vh->flags = le32(d + 12);
    vh->present = vh->flags & 1;
    vh->mode_serial = le32(d + 16);
    vh->width = le32(d + 24);
    vh->height = le32(d + 28);
    vh->mhz = le32(d + 32);
    vh->fourcc = le32(d + 36);
    vh->stride = le32(d + 40);
    vh->n_slots = le32(d + 44);
    for (unsigned i = 0; i < 16; i++) {
        vh->slot[i] = le32(d + 48 + 4 * i);
    }
    vh->pointer_mode = le32(d + 112);
    vh->credit = le32(d + 116);
}

/* Whether the socket has something to read within ms. */
static inline bool v_readable(Viewer *v, int ms)
{
#ifdef _WIN32
    WSAPOLLFD p = { .fd = _get_osfhandle(v->fd), .events = POLLIN };

    return WSAPoll(&p, 1, ms) == 1;
#else
    struct pollfd p = { .fd = v->fd, .events = POLLIN };

    return poll(&p, 1, ms) == 1;
#endif
}

/* Read more bytes, collecting passed descriptors. 0: nothing, -1: closed. */
static inline int v_fill(Viewer *v, int ms)
{
    ssize_t n;
#ifndef _WIN32
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * 32)];
    } ctl;
    struct iovec iov = { .iov_base = v->in + v->in_len,
                         .iov_len = DSHM_MSG_MAX - v->in_len };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = ctl.buf,
                         .msg_controllen = sizeof(ctl.buf) };
#endif

    if (v->closed) {
        return -1;
    }
    if (!v_readable(v, ms)) {
        return 0;
    }
#ifdef _WIN32
    n = recv(v->fd, (char *)v->in + v->in_len, DSHM_MSG_MAX - v->in_len, 0);
    if (n <= 0) {
        v->closed = true;
        return -1;
    }
#else
    n = recvmsg(v->fd, &mh, MSG_CMSG_CLOEXEC);
    if (n <= 0) {
        v->closed = true;
        return -1;
    }
    g_assert_false(mh.msg_flags & MSG_CTRUNC);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);

            for (size_t i = 0; i < cnt; i++) {
                g_assert_cmpuint(v->fdq_n, <, ARRAY_SIZE(v->fdq));
                memcpy(&v->fdq[v->fdq_n++], CMSG_DATA(c) + i * sizeof(int),
                       sizeof(int));
            }
        }
    }
#endif
    v->in_len += n;
    return 1;
}

/* One whole message off the stream; applies REGION, HEAD and REGION_GONE. */
static inline bool v_parse(Viewer *v, VMsg *m)
{
    uint32_t len;

    if (v->in_len < 8) {
        return false;
    }
    len = le32(v->in + 4);
    g_assert_cmpuint(len, >=, 8);
    g_assert_cmpuint(len, <=, DSHM_MSG_MAX);
    g_assert_cmpuint(len % 8, ==, 0);
    if (v->in_len < len) {
        return false;
    }
    m->type = le16(v->in);
    m->flags = le16(v->in + 2);
    m->len = len;
    m->d = g_memdup2(v->in, len);
    m->h = V_NO_HANDLE;
    memmove(v->in, v->in + len, v->in_len - len);
    v->in_len -= len;
    if (m->flags & DSHM_F_HANDLE) {
        g_assert_cmpuint(len, >=, 16);
#ifdef _WIN32
        /* taken when the field is read; the value is ours to close */
        m->h = (HANDLE)(uintptr_t)le64(m->d + len - 8);
        g_assert_true(V_HANDLE_OK(m->h));
        v->handles_taken++;
#else
        g_assert_cmpuint(v->fdq_n, >, 0);
        m->h = v->fdq[0];
        memmove(v->fdq, v->fdq + 1, --v->fdq_n * sizeof(int));
        g_assert_cmpuint(le64(m->d + len - 8), ==, 0);
#endif
        v->handles_parsed++;
    }
    switch (m->type) {
    case DSHM_MSG_REGION: {
        VRegion *r = NULL;

        for (unsigned i = 0; i < V_REGIONS && !r; i++) {
            r = !v->reg[i].id || v->reg[i].gone ? &v->reg[i] : NULL;
        }
        g_assert_nonnull(r);
        g_assert_true(V_HANDLE_OK(m->h));
        v_unmap(r);
        v_handle_close(r->h);
        *r = (VRegion) { .id = le32(m->d + 8), .kind = le32(m->d + 12),
                         .storage = le32(m->d + 16),
                         .access = le32(m->d + 20),
                         .size = le64(m->d + 24), .h = m->h };
        if (le32(m->d + 32) >= 32) {
            r->fourcc = le32(m->d + 40);
            r->gflags = le32(m->d + 44);
            r->gstride = le32(m->d + 60);
        }
        m->h = V_NO_HANDLE;
        break;
    }
    case DSHM_MSG_REGION_GONE: {
        VRegion *r = v_region(v, le32(m->d + 8));

        if (r) {
            r->gone = true;
        }
        break;
    }
    case DSHM_MSG_HEAD:
        v_apply_head(v, m->d);
        break;
    default:
        break;
    }
    return true;
}

static inline void v_stash(Viewer *v, const VMsg *m)
{
    g_assert_cmpuint(v->n_pending, <, V_MAX_PENDING);
    v->pending[v->n_pending++] = *m;
}

/* Read until one more message is pending, or ms pass. */
static inline bool v_pump(Viewer *v, int ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    VMsg m = { };

    for (;;) {
        gint64 left;
        int r;

        if (v_parse(v, &m)) {
            v_stash(v, &m);
            return true;
        }
        if (v->closed) {
            return false;
        }
        left = MAX((end - g_get_monotonic_time()) / 1000, 0);
        r = v_fill(v, left);
        if (r < 0 || (r == 0 && g_get_monotonic_time() >= end)) {
            return false;
        }
    }
}

/*
 * The next message of a type, in order; others stay for later. False when
 * none arrives in ms.
 */
static inline bool v_take(Viewer *v, uint16_t type, VMsg *out, int ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

    for (;;) {
        gint64 left;

        for (unsigned i = 0; i < v->n_pending; i++) {
            if (v->pending[i].type == type) {
                *out = v->pending[i];
                memmove(&v->pending[i], &v->pending[i + 1],
                        (--v->n_pending - i) * sizeof(VMsg));
                return true;
            }
        }
        left = (end - g_get_monotonic_time()) / 1000;
        if (left <= 0 || v->closed) {
            return false;
        }
        v_pump(v, left);
    }
}

static inline void v_drain(Viewer *v, int ms)
{
    while (v_pump(v, ms)) {
    }
}

static inline void v_discard(Viewer *v, uint16_t type)
{
    for (unsigned i = 0; i < v->n_pending;) {
        if (v->pending[i].type == type) {
            v_msg_free(&v->pending[i]);
            memmove(&v->pending[i], &v->pending[i + 1],
                    (--v->n_pending - i) * sizeof(VMsg));
        } else {
            i++;
        }
    }
}

static inline void v_send_handles_seen(Viewer *v)
{
    uint8_t b[8];

    stq_le_p(b, v->handles_parsed);
    v_send_msg(v, DSHM_MSG_HANDLES_SEEN, 0, b, 8);
}

static inline unsigned v_steer_ext(Viewer *v, unsigned local)
{
    return (v->steer_idx << 8) | local;
}

typedef struct VOpts {
    uint32_t features;
    uint32_t credit;
    uint32_t storage0, storage1;
    bool test, steer, dmabuf;
    uint32_t max_w, max_h;
    const char *name;
    uint32_t prev_id;           /* the last connection, for ORPHANS */
    uint64_t prev_taken, prev_session;
} VOpts;

/* HELLO, WELCOME, regions, heads, READY. */
static inline Viewer *v_open(const char *path, VOpts o)
{
    Viewer *v = g_new0(Viewer, 1);
    uint8_t hello[120 + 80 + 48] = { };
    size_t len = 120, off;
    unsigned n_ext = 0;
    VMsg m = { };

    v->in = g_malloc(DSHM_MSG_MAX);
    for (unsigned i = 0; i < V_REGIONS; i++) {
        v->reg[i].h = V_NO_HANDLE;
    }
    v->fd = v_connect_fd(path);
    v->credit_asked = o.credit ? o.credit : 1;
    v->features = o.features | (o.test ? DSHM_FEAT_TEST : 0);
    stw_le_p(hello + 8, 4);
    stw_le_p(hello + 10, 0);
    stl_le_p(hello + 12, v->features);
    snprintf((char *)hello + 16, 32, "%s", o.name ? o.name : "qtest");
    stl_le_p(hello + 48, o.storage0 ? o.storage0 : 1);
    stl_le_p(hello + 52, o.storage1);
    stl_le_p(hello + 80, v->credit_asked);
    stl_le_p(hello + 84, o.max_w);
    stl_le_p(hello + 88, o.max_h);
    stl_le_p(hello + 92, o.prev_id);
    stq_le_p(hello + 104, o.prev_taken);
    stq_le_p(hello + 112, o.prev_session);
    off = 120;
    if (o.test) {
        snprintf((char *)hello + off, 32, "qemu.test");
        stw_le_p(hello + off + 32, 1);
        stw_le_p(hello + off + 34, 1);
        off += 40;
        n_ext++;
    }
    if (o.steer) {
        snprintf((char *)hello + off, 32, "qemu.pointer-steering");
        stw_le_p(hello + off + 32, 1);
        stw_le_p(hello + off + 34, 1);
        off += 40;
        n_ext++;
    }
    if (o.dmabuf) {
        snprintf((char *)hello + off, 32, "linux.dmabuf");
        stw_le_p(hello + off + 32, 1);
        stw_le_p(hello + off + 34, 1);
        stl_le_p(hello + off + 36, 8);
        off += 48;
        n_ext++;
    }
    stl_le_p(hello + 100, n_ext);
    len = off;
    stw_le_p(hello, DSHM_MSG_HELLO);
    stw_le_p(hello + 2, DSHM_F_MUST);
    stl_le_p(hello + 4, len);
    v_send(v, hello, len);

    g_assert_true(v_take(v, DSHM_MSG_WELCOME, &m, V_WAIT_MS));
    v->session = le64(m.d + 16);
    v->id = le32(m.d + 24);
    v->state_region = le32(m.d + 28);
    v->feedback_region = le32(m.d + 32);
    v->granted_credit = le32(m.d + 36);
    v->max_viewers = le32(m.d + 40);
    {
        unsigned n = le32(m.d + 44);
        size_t pos = 80;

        for (unsigned i = 0; i < n; i++) {
            const uint8_t *a = m.d + pos;
            uint32_t plen = le32(a + 36);

            if (!strcmp((const char *)a, "qemu.test")) {
                v->test_idx = a[34];
            } else if (!strcmp((const char *)a, "qemu.pointer-steering")) {
                v->steer_idx = a[34];
                v->steer_block_off = le32(a + 40);
            } else if (!strcmp((const char *)a, "linux.dmabuf")) {
                v->gpu_alloc = le32(a + 40);
            }
            pos += 40 + ROUND_UP(plen, 8);
        }
    }
    v_msg_free(&m);
    g_assert_true(v_take(v, DSHM_MSG_READY, &m, V_WAIT_MS));
    v_msg_free(&m);
    v->state = v_map(v, v->state_region, false);
    v->state_size = v_region(v, v->state_region)->size;
    v->feedback = v_map(v, v->feedback_region, true);
    v->ready = true;
    /* the tables are built; what set them up need not be taken again */
    v_discard(v, DSHM_MSG_HEAD);
    v_discard(v, DSHM_MSG_REGION);
    v_send_handles_seen(v);
    return v;
}

/*
 * Windows does not tell the peer about a close while unread data is queued,
 * so say goodbye and drain first.
 */
static inline void v_fd_close(int fd)
{
#ifdef _WIN32
    gint64 end = g_get_monotonic_time() + 500000;
    char buf[512];
    bool drained = false;

    shutdown(fd, SD_SEND);
    while (g_get_monotonic_time() < end) {
        fd_set rf;
        struct timeval tv = { .tv_usec = 50000 };

        FD_ZERO(&rf);
        FD_SET((SOCKET)_get_osfhandle(fd), &rf);
        if (select(0, &rf, NULL, NULL, &tv) > 0 &&
            recv(fd, buf, sizeof(buf), 0) <= 0) {
            drained = true;
            break;
        }
    }
    if (!drained) {
        g_printerr("display-shm viewer: no close from the producer in "
                   "500 ms\n");
    }
#endif
    close(fd);
}

static inline void v_close(Viewer *v)
{
    if (v->fd >= 0) {
        v_fd_close(v->fd);
    }
    for (unsigned i = 0; i < V_REGIONS; i++) {
        v_unmap(&v->reg[i]);
        v_handle_close(v->reg[i].h);
    }
    for (unsigned i = 0; i < v->n_pending; i++) {
        v_msg_free(&v->pending[i]);
    }
#ifndef _WIN32
    for (unsigned i = 0; i < v->fdq_n; i++) {
        close(v->fdq[i]);
    }
#endif
    g_free(v->in);
    g_free(v);
}

/* ---- Requests, frames, releases ---------------------------------------- */

static inline void v_request(Viewer *v, unsigned head, uint32_t kinds,
                      uint64_t frame_seen, uint32_t cursor_seen,
                      uint32_t flags)
{
    uint8_t b[24] = { };

    stl_le_p(b, head);
    stl_le_p(b + 4, kinds);
    stq_le_p(b + 8, frame_seen);
    stl_le_p(b + 16, cursor_seen);
    stl_le_p(b + 20, flags);
    v_send_msg(v, DSHM_MSG_REQUEST, DSHM_F_MUST, b, sizeof(b));
}

static inline void v_frame_parse(const VMsg *m, VFrame *f)
{
    const uint8_t *d = m->d;

    memset(f, 0, sizeof(*f));
    f->head = le32(d + 8);
    f->region = le32(d + 12);
    f->serial = le64(d + 16);
    f->mode_serial = le32(d + 24);
    f->damage_kind = le16(d + 28);
    f->damage_count = le16(d + 30);
    f->sample_ns = le64(d + 32);
    f->publish_ns = le64(d + 40);
    f->guest_ns = le64(d + 48);
    f->lease = le16(d + 56);
    f->flags = le16(d + 58);
    f->lease_seq = le64(d + 64);
    f->damage_len = m->len - 72;
    g_assert_cmpuint(f->damage_len, <=, sizeof(f->damage));
    memcpy(f->damage, d + 72, f->damage_len);
}

static inline bool v_frame(Viewer *v, VFrame *f, int ms)
{
    VMsg m = { };

    if (!v_take(v, DSHM_MSG_FRAME, &m, ms)) {
        return false;
    }
    v_frame_parse(&m, f);
    v_msg_free(&m);
    return true;
}

static inline void v_release(Viewer *v, const VFrame *f)
{
    uint64_t *rel = (uint64_t *)(v->feedback + 64 + 128 * f->head);

    __atomic_store_n(&rel[f->lease], f->lease_seq, __ATOMIC_RELEASE);
}

/* The slot's pixels at (x, y): the X byte is undefined. */
static inline uint32_t v_px(Viewer *v, const VFrame *f, unsigned x, unsigned y)
{
    uint8_t *m = v_map(v, f->region, false);
    uint32_t stride = v->head[f->head].stride;

    return ldl_le_p(m + (size_t)y * stride + x * 4) & 0xffffff;
}

/* A dma-buf slot's pixels; a memfd in these tests, so it maps. */
static inline uint32_t v_gpu_px(Viewer *v, const VFrame *f, unsigned x,
                                unsigned y)
{
    uint8_t *m = v_map(v, f->region, false);
    uint32_t stride = v_region(v, f->region)->gstride;

    return ldl_le_p(m + (size_t)y * stride + x * 4) & 0xffffff;
}

/* Whether (x, y) is inside the frame's damage. */
static inline bool v_damaged(const VFrame *f, unsigned x, unsigned y)
{
    switch (f->damage_kind) {
    case DSHM_DAMAGE_WHOLE:
        return true;
    case DSHM_DAMAGE_RECTS:
        for (unsigned i = 0; i < f->damage_count; i++) {
            const uint8_t *r = f->damage + 16 * i;

            if (x >= le32(r) && x < le32(r) + le32(r + 8) &&
                y >= le32(r + 4) && y < le32(r + 4) + le32(r + 12)) {
                return true;
            }
        }
        return false;
    default:
        return f->damage[y / 8] & (1 << (y % 8));
    }
}

/* ---- The state region -------------------------------------------------- */

typedef struct VStateFrame {
    uint64_t serial, sample_ns, publish_ns, guest_ns;
    uint32_t skipped;
} VStateFrame;

static inline VStateFrame v_state_frame(Viewer *v, unsigned head)
{
    const uint8_t *r = v->state + 64 + 64 * head;
    VStateFrame f;
    uint32_t s1, s2;

    for (;;) {
        s1 = __atomic_load_n((const uint32_t *)(r + 20), __ATOMIC_ACQUIRE);
        if (s1 & 1) {
            continue;
        }
        f.serial = __atomic_load_n((const uint64_t *)(r + 24),
                                   __ATOMIC_RELAXED);
        f.sample_ns = __atomic_load_n((const uint64_t *)(r + 32),
                                      __ATOMIC_RELAXED);
        f.publish_ns = __atomic_load_n((const uint64_t *)(r + 40),
                                       __ATOMIC_RELAXED);
        f.guest_ns = __atomic_load_n((const uint64_t *)(r + 48),
                                     __ATOMIC_RELAXED);
        f.skipped = __atomic_load_n((const uint32_t *)(r + 56),
                                    __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n((const uint32_t *)(r + 20), __ATOMIC_RELAXED);
        if (s1 == s2) {
            return f;
        }
    }
}

typedef struct VStateCursor {
    uint32_t seq, flags, image_serial;
    int32_t x, y;
} VStateCursor;

static inline VStateCursor v_state_cursor(Viewer *v, unsigned head)
{
    const uint8_t *r = v->state + 64 + 64 * head;
    VStateCursor c;
    uint32_t s1, s2;

    for (;;) {
        s1 = __atomic_load_n((const uint32_t *)r, __ATOMIC_ACQUIRE);
        if (s1 & 1) {
            continue;
        }
        c.flags = __atomic_load_n((const uint32_t *)(r + 4),
                                  __ATOMIC_RELAXED);
        c.x = __atomic_load_n((const int32_t *)(r + 8), __ATOMIC_RELAXED);
        c.y = __atomic_load_n((const int32_t *)(r + 12), __ATOMIC_RELAXED);
        c.image_serial = __atomic_load_n((const uint32_t *)(r + 16),
                                         __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n((const uint32_t *)r, __ATOMIC_RELAXED);
        if (s1 == s2) {
            c.seq = s1;
            return c;
        }
    }
}

/* ---- qemu.test ---------------------------------------------------------- */

static inline unsigned v_ext(Viewer *v, unsigned local)
{
    return (v->test_idx << 8) | local;
}

static inline void v_stimulus(Viewer *v, unsigned head, uint32_t kind,
                       uint64_t seed, uint32_t step, uint32_t arg)
{
    uint8_t b[24] = { };

    stl_le_p(b, head);
    stl_le_p(b + 4, kind);
    stq_le_p(b + 8, seed);
    stl_le_p(b + 16, step);
    stl_le_p(b + 20, arg);
    v_send_msg(v, v_ext(v, DSHM_TEST_STIMULUS), 0, b, sizeof(b));
}

static inline void v_pattern(Viewer *v, unsigned head, uint32_t pattern,
                      uint64_t seed, uint32_t step)
{
    v_stimulus(v, head, 1, seed, step, pattern);
}

/* Returns the result: 0 done, 1 nothing to act on. */
static inline uint32_t v_control(Viewer *v, unsigned op, unsigned head,
                          unsigned target, unsigned arg)
{
    uint8_t b[16] = { };
    VMsg m = { };
    uint32_t r;

    stl_le_p(b, op);
    stl_le_p(b + 4, head);
    stl_le_p(b + 8, target);
    stl_le_p(b + 12, arg);
    v_send_msg(v, v_ext(v, DSHM_TEST_CONTROL), 0, b, sizeof(b));
    g_assert_true(v_take(v, v_ext(v, DSHM_TEST_CONTROL_DONE), &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, op);
    r = le32(m.d + 12);
    v_msg_free(&m);
    return r;
}

typedef struct VStats {
    uint64_t samples, fills, lines_produced, lines_stored, lines_copied;
    uint64_t bytes_copied, publishes, frames_sent, wakeups, retained;
    uint32_t sets, max_queued;
} VStats;

static inline VStats v_stats(Viewer *v)
{
    VMsg m = { };
    VStats s;

    v_send_msg(v, v_ext(v, DSHM_TEST_STATS_REQUEST), 0, NULL, 0);
    g_assert_true(v_take(v, v_ext(v, DSHM_TEST_STATS), &m, V_WAIT_MS));
    g_assert_cmpuint(m.len, ==, 96);
    s = (VStats) {
        .samples = le64(m.d + 8), .fills = le64(m.d + 16),
        .lines_produced = le64(m.d + 24), .lines_stored = le64(m.d + 32),
        .lines_copied = le64(m.d + 40), .bytes_copied = le64(m.d + 48),
        .publishes = le64(m.d + 56), .frames_sent = le64(m.d + 64),
        .wakeups = le64(m.d + 72), .sets = le32(m.d + 88),
        .max_queued = le32(m.d + 92), .retained = le64(m.d + 80),
    };
    v_msg_free(&m);
    return s;
}

/* The picture the stimulus defines, written independently of the producer. */
static inline uint64_t t_fnv(uint64_t seed, uint32_t y, uint32_t s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t b[16];

    stq_le_p(b, seed);
    stl_le_p(b + 8, y);
    stl_le_p(b + 12, s);
    for (unsigned i = 0; i < 16; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static inline uint32_t t_pixel(unsigned pattern, uint64_t seed, uint32_t step,
                        unsigned x, unsigned y, unsigned w, unsigned h)
{
    uint32_t p = 0;

    switch (pattern) {
    case 1:
        p = (seed * 2654435761ull + step) & 0xffffff;
        break;
    case 2:
        p = ((y + (uint64_t)step * 8) % h) < 8 ? 0xffffff : 0;
        break;
    case 3: {
        uint32_t r = y % 7;

        if (step >= r) {
            uint32_t s = step - ((step - r) % 7);

            p = t_fnv(seed, y, s) & 0xffffff;
        }
        break;
    }
    }
    if (y == 0 && x < 8) {
        p = (step >> (4 * x)) & 0xf;
    }
    return p;
}

static inline void v_check_picture(Viewer *v, const VFrame *f, unsigned pattern,
                            uint64_t seed, uint32_t step)
{
    const VHead *h = &v->head[f->head];

    for (unsigned y = 0; y < h->height; y++) {
        for (unsigned x = 0; x < h->width; x++) {
            g_assert_cmphex(v_px(v, f, x, y), ==,
                            t_pixel(pattern, seed, step, x, y, h->width,
                                    h->height));
        }
    }
}

/* The step a frame's marker line carries. */
static inline uint32_t v_frame_step(Viewer *v, const VFrame *f)
{
    uint32_t step = 0;

    for (unsigned i = 0; i < 8; i++) {
        step |= (v_px(v, f, i, 0) & 0xf) << (4 * i);
    }
    return step;
}

/* ---- Input echo -------------------------------------------------------- */

typedef struct VEcho {
    uint32_t head, kind, code, seq;
    int32_t a, b;
} VEcho;

static inline bool v_echo(Viewer *v, VEcho *e, int ms)
{
    VMsg m = { };

    if (!v_take(v, v_ext(v, DSHM_TEST_INPUT_ECHO), &m, ms)) {
        return false;
    }
    g_assert_cmpuint(m.len, ==, 40);
    *e = (VEcho) { .head = le32(m.d + 8), .kind = le32(m.d + 12),
                   .code = le32(m.d + 16), .a = (int32_t)le32(m.d + 20),
                   .b = (int32_t)le32(m.d + 24), .seq = le32(m.d + 28) };
    v_msg_free(&m);
    return true;
}

static inline void v_key(Viewer *v, unsigned head, unsigned code, unsigned down)
{
    uint8_t b[16] = { };

    stl_le_p(b, head);
    stl_le_p(b + 4, code);
    stl_le_p(b + 8, down);
    v_send_msg(v, DSHM_MSG_KEY, 0, b, sizeof(b));
}

static inline void v_button(Viewer *v, unsigned head, unsigned button,
                            unsigned down)
{
    uint8_t b[16] = { };

    stl_le_p(b, head);
    stl_le_p(b + 4, button);
    stl_le_p(b + 8, down);
    v_send_msg(v, DSHM_MSG_BUTTON, 0, b, sizeof(b));
}

static inline void v_motion(Viewer *v, uint16_t type, unsigned head, int32_t x,
                     int32_t y)
{
    uint8_t b[16] = { };

    stl_le_p(b, head);
    stl_le_p(b + 8, x);
    stl_le_p(b + 12, y);
    v_send_msg(v, type, 0, b, sizeof(b));
}

static inline void v_sync(Viewer *v, bool ack, uint32_t serial)
{
    uint8_t b[8] = { };

    stl_le_p(b, ack);
    stl_le_p(b + 4, serial);
    v_send_msg(v, DSHM_MSG_SYNC, 0, b, sizeof(b));
}

#endif
