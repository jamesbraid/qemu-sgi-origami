/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shared-memory display for the SGI machines, protocol version 3. See
 * include/ui/sgi-display-shm.h and docs/system/mips/sgi/display-shm.rst.
 *
 * One reader at a time. QEMU owns a control block and a set of regions, each
 * one allocation with one handle. A region is a sealed memfd on Linux, a
 * POSIX shared-memory object on macOS and a pagefile-backed section on
 * Windows; QEMU maps each and the reader maps its own view. With
 * display-shm-gpu the scanout regions are host-visible Vulkan memory
 * instead (sgi-display-vk.c), which the reader imports into its own Vulkan
 * device; any region that cannot be allocated that way is shared memory. The
 * handle travels to the reader on the AF_UNIX stream socket (SCM_RIGHTS on
 * POSIX, DuplicateHandle on Windows). The socket also carries the reader's
 * binary input, which sgi-display-input.c turns into qemu_input_*() calls.
 *
 * A head that has not been converted to its own family publishes its
 * SGIScanout frame as family xrgb: the frame lives in a region, the
 * model's CPU resolve still runs, and the reader sees the lines it marked
 * in the head's dirty bitmap.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qemu/atomic.h"
#include "qemu/memfd.h"
#include "qemu/units.h"
#include "qemu/notify.h"
#include "qemu/queue.h"
#include "qom/object.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/display/sgi/scanout.h"
#include "ui/sgi-display-shm.h"
#include "ui/sgi-display-priv.h"
#include "ui/sgi-display-vk.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

/* Ask for the guest's own scanout rate: SGI modes run at 60 to 76 Hz. */
#define SGI_SHM_REFRESH_MS 13
/* A reader this far behind is not reading: drop it and let it reconnect. */
#define SGI_SHM_OUT_MAX 1024
#define SGI_SHM_IN_BYTES 2048
#define SGI_SHM_MSG_MAX 64
/*
 * The bytes of regions that queued REGION messages hold open. A reader that
 * stops reading while the guest changes mode pins one frame per change; past
 * this it is dropped, as one that lets the message count run out is.
 */
#define SGI_SHM_PIN_MAX (256 * MiB)

#ifdef _WIN32
#define SGI_SHM_HOST_BACKEND SGI_SHM_BACKEND_SHM_WIN32
#else
#define SGI_SHM_HOST_BACKEND SGI_SHM_BACKEND_SHM_FD
#endif

typedef struct SGIDisplay SGIDisplay;
typedef struct SGIDisplayHead SGIDisplayHead;

struct SGIDisplayRegion {
    QTAILQ_ENTRY(SGIDisplayRegion) next;
    uint32_t id;                /* 0 for the control block and local memory */
    uint32_t kind;
    uint8_t *ptr;
    size_t size;                /* what the reader maps; rounded up to 64 KiB */
    bool local;                 /* g_malloc0() memory, display-shm is off */
    uint32_t backend;           /* SGI_SHM_BACKEND_*, as REGION says */
    uint32_t memory_type;       /* the Vulkan backends' import parameters */
    uint64_t alloc_size;
    uint32_t flags;             /* SGI_SHM_REGION_* */
    SGIVkExport *vk;            /* the Vulkan allocation, or NULL for shm */
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

struct SGIDisplayHead {
    DisplayChangeListener dcl;
    SGIDisplay *d;
    SGIScanout *s;
    SGIShmHead *rec;
    unsigned idx;
    uint32_t family;
    uint32_t layout_gen;        /* the writer's own copies of what it */
    uint32_t state_gen;         /* publishes; the reader can write them */
    uint32_t cursor_gen;
    bool bound;
    bool presented;             /* lines marked since content_gen last moved */
    uint32_t width, height;     /* the mode QEMU last wrote, for the input */
    SGIDisplayRegion *cursor_region;
    uint8_t *cursor_image;
    int32_t cursor_x, cursor_y;
    bool cursor_on;
};

/* What a message carries to the reader besides its bytes. */
typedef struct SGIExport {
    int fd;                     /* POSIX: a dup to attach to the first byte */
    uint64_t handle;            /* Windows: a handle valid in the reader */
    uint64_t pin;               /* the bytes of region it keeps open */
} SGIExport;

#define SGI_NO_EXPORT ((SGIExport) { .fd = -1 })

typedef struct SGIDisplayOut {
    QSIMPLEQ_ENTRY(SGIDisplayOut) next;
    uint8_t buf[SGI_SHM_MSG_MAX];
    size_t len, off;
    SGIExport x;
} SGIDisplayOut;

struct SGIDisplay {
    char *name;
    char *sock_path;            /* the property, or NULL for the default */
    bool gpu_requested;
    uint8_t gpu_uuid[16];
    SGIVkDevice *vk;            /* the adapter regions are exported from */
    bool vk_tried;              /* opening it was tried, win or lose */
    SGIDisplayRegion *control;
    SGIShmHeader *hdr;
    QTAILQ_HEAD(, SGIDisplayRegion) regions;
    uint32_t next_region_id;
    unsigned nheads;
    int cursor_head;            /* the head the guest cursor last showed on */
    SGIDisplayHead heads[SGI_SHM_MAX_HEADS];

    int listen_fd;
    int client_fd;
    bool hello;                 /* the reader's HELLO arrived */
    uint8_t in_buf[SGI_SHM_IN_BYTES];
    size_t in_len;
    QSIMPLEQ_HEAD(, SGIDisplayOut) out;
    unsigned out_count;
    uint64_t out_pinned;        /* the pin of every export still queued */
    int pend_fd;                /* a connection that has not said HELLO */
    uint8_t pend_buf[sizeof(SGIShmMsgHello)];
    size_t pend_len;
    bool want_write;
    bool export_failed;         /* the failure to export was reported */
    bool test_pattern;          /* x-display-shm-test-pattern */
#ifdef _WIN32
    HANDLE reader_process;      /* the peer of client_fd */
    HANDLE pend_process;        /* the peer of pend_fd */
#endif
    QEMUBH *wake_bh;
    bool exited;
    Notifier done;
    Notifier exit;
};

static SGIDisplay *shm_display;

/* ---- Regions ---------------------------------------------------------- */

#ifndef _WIN32
static int region_fd_create(const char *label, size_t size, Error **errp)
{
    static unsigned serial;
    g_autofree char *name = NULL;
    int fd;

#ifdef CONFIG_LINUX
    fd = qemu_memfd_create(label, size, false, 0,
                           F_SEAL_SHRINK | F_SEAL_GROW, NULL);
    if (fd >= 0) {
        return fd;
    }
#endif
    /*
     * macOS, and a Linux without memfd: a POSIX shared-memory object whose
     * name goes away at once. The reader gets the handle, never the name.
     * The name stays under macOS's 31-character limit.
     */
    name = g_strdup_printf("/sgi-dsp-%u-%u", (unsigned)getpid(), serial++);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot create shared memory %s", name);
        return -1;
    }
    if (ftruncate(fd, size) < 0) {
        error_setg_errno(errp, errno, "cannot size shared memory %s", name);
        shm_unlink(name);
        close(fd);
        return -1;
    }
    shm_unlink(name);
    qemu_set_cloexec(fd);
    return fd;
}
#endif

static bool region_map(SGIDisplayRegion *r, const char *label, size_t size,
                       Error **errp)
{
    r->size = size;
    r->alloc_size = size;
    r->backend = SGI_SHM_HOST_BACKEND;
#ifdef _WIN32
    r->handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                   (DWORD)((uint64_t)size >> 32),
                                   (DWORD)(size & 0xffffffffu), NULL);
    if (!r->handle) {
        error_setg(errp, "cannot create the %s section: error %lu", label,
                   GetLastError());
        return false;
    }
    r->ptr = MapViewOfFile(r->handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!r->ptr) {
        error_setg(errp, "cannot map the %s section: error %lu", label,
                   GetLastError());
        CloseHandle(r->handle);
        r->handle = NULL;
        return false;
    }
#else
    void *p;

    r->fd = region_fd_create(label, size, errp);
    if (r->fd < 0) {
        return false;
    }
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (p == MAP_FAILED) {
        error_setg_errno(errp, errno, "cannot map the %s region", label);
        close(r->fd);
        r->fd = -1;
        return false;
    }
    r->ptr = p;
#endif
    return true;
}

static void region_unmap(SGIDisplayRegion *r)
{
    if (r->vk) {
        /* The export owns its mapping and its handle. */
        sgi_display_vk_free(r->vk);
        r->vk = NULL;
        r->ptr = NULL;
#ifdef _WIN32
        r->handle = NULL;
#else
        r->fd = -1;
#endif
        return;
    }
#ifdef _WIN32
    if (r->ptr) {
        UnmapViewOfFile(r->ptr);
    }
    if (r->handle) {
        CloseHandle(r->handle);
    }
    r->handle = NULL;
#else
    if (r->ptr) {
        munmap(r->ptr, r->size);
    }
    if (r->fd >= 0) {
        close(r->fd);
    }
    r->fd = -1;
#endif
    r->ptr = NULL;
}

/* ---- The socket ------------------------------------------------------- */

#ifdef _WIN32
/* Close a handle QEMU made in the reader's process and never delivered. */
static void remote_close(SGIDisplay *d, uint64_t handle)
{
    if (d->reader_process && handle) {
        DuplicateHandle(d->reader_process, (HANDLE)(uintptr_t)handle, NULL,
                        NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
}
#endif

/* Give back an export that never reached the reader. */
static void export_discard(SGIDisplay *d, const SGIExport *x)
{
#ifdef _WIN32
    remote_close(d, x->handle);
#else
    if (x->fd >= 0) {
        close(x->fd);
    }
#endif
}

/* Free a message; one that was not sent whole takes its export back. */
static void out_free(SGIDisplay *d, SGIDisplayOut *o)
{
    if (o->off < o->len) {
        export_discard(d, &o->x);
    }
    d->out_pinned -= o->x.pin;
    g_free(o);
}

static void pend_drop(SGIDisplay *d)
{
    if (d->pend_fd >= 0) {
        qemu_set_fd_handler(d->pend_fd, NULL, NULL, NULL);
        close(d->pend_fd);
        d->pend_fd = -1;
    }
    d->pend_len = 0;
#ifdef _WIN32
    if (d->pend_process) {
        CloseHandle(d->pend_process);
        d->pend_process = NULL;
    }
#endif
}

static void client_drop(SGIDisplay *d)
{
    SGIDisplayOut *o;

    if (d->client_fd >= 0) {
        qemu_set_fd_handler(d->client_fd, NULL, NULL, NULL);
        close(d->client_fd);
        d->client_fd = -1;
    }
    while ((o = QSIMPLEQ_FIRST(&d->out))) {
        QSIMPLEQ_REMOVE_HEAD(&d->out, next);
        out_free(d, o);
    }
    d->out_count = 0;
    d->out_pinned = 0;
    d->want_write = false;
    d->hello = false;
    d->in_len = 0;
#ifdef _WIN32
    if (d->reader_process) {
        CloseHandle(d->reader_process);
        d->reader_process = NULL;
    }
#endif
    sgi_display_input_reader_gone();
}

static ssize_t out_send(int sock, const SGIDisplayOut *o)
{
#ifdef _WIN32
    return send(sock, (const char *)o->buf + o->off, o->len - o->off, 0);
#else
    struct iovec iov = { .iov_base = (void *)(o->buf + o->off),
                         .iov_len = o->len - o->off };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;

    if (o->x.fd >= 0 && !o->off) {
        struct cmsghdr *c;

        memset(&ctl, 0, sizeof(ctl));
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof(ctl.buf);
        c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &o->x.fd, sizeof(int));
    }
    return sendmsg(sock, &mh, 0);
#endif
}

static void client_read(void *opaque);
static void out_flush(SGIDisplay *d);

static void client_writable(void *opaque)
{
    out_flush(opaque);
}

static void out_flush(SGIDisplay *d)
{
    while (d->client_fd >= 0 && !QSIMPLEQ_EMPTY(&d->out)) {
        SGIDisplayOut *o = QSIMPLEQ_FIRST(&d->out);
        ssize_t n = out_send(d->client_fd, o);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!d->want_write) {
                    d->want_write = true;
                    qemu_set_fd_handler(d->client_fd, client_read,
                                        client_writable, d);
                }
                return;
            }
            client_drop(d);
            return;
        }
#ifndef _WIN32
        if (n > 0 && o->x.fd >= 0) {
            /* The handle went with the first byte. */
            close(o->x.fd);
            o->x.fd = -1;
            d->out_pinned -= o->x.pin;
            o->x.pin = 0;
        }
#endif
        o->off += n;
        if (o->off < o->len) {
            continue;
        }
        QSIMPLEQ_REMOVE_HEAD(&d->out, next);
        d->out_count--;
        out_free(d, o);
    }
    if (d->client_fd >= 0 && d->want_write) {
        d->want_write = false;
        qemu_set_fd_handler(d->client_fd, client_read, NULL, d);
    }
}

/* Queue a message; it owns the export, if any, from here on. */
static void out_queue(SGIDisplay *d, const void *msg, size_t len,
                      SGIExport x)
{
    SGIDisplayOut *o;

    assert(len <= SGI_SHM_MSG_MAX);
    if (d->client_fd < 0 || !d->hello) {
        export_discard(d, &x);
        return;
    }
    if (d->out_count >= SGI_SHM_OUT_MAX) {
        export_discard(d, &x);
        warn_report("sgi-display-shm: the reader is not reading, dropping it");
        client_drop(d);
        return;
    }
    o = g_new0(SGIDisplayOut, 1);
    memcpy(o->buf, msg, len);
    o->len = len;
    o->x = x;
    QSIMPLEQ_INSERT_TAIL(&d->out, o, next);
    d->out_count++;
    d->out_pinned += x.pin;
    out_flush(d);
    if (d->client_fd >= 0 && d->out_pinned > SGI_SHM_PIN_MAX) {
        warn_report("sgi-display-shm: the reader is not reading and queued "
                    "regions hold %" PRIu64 " MiB open, dropping it",
                    d->out_pinned / MiB);
        client_drop(d);
    }
}

/*
 * A handle for the reader: a dup of the descriptor on POSIX, in x->fd; on
 * Windows a handle value already valid in the reader's process, in
 * x->handle. False when the reader cannot take it.
 */
static bool region_export(SGIDisplay *d, const SGIDisplayRegion *r,
                          SGIExport *x)
{
    *x = SGI_NO_EXPORT;
    x->pin = r->size;
#ifdef _WIN32
    {
        HANDLE dup;

        if (!d->reader_process ||
            !DuplicateHandle(GetCurrentProcess(), r->handle,
                             d->reader_process, &dup, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            error_report("sgi-display-shm: cannot give the reader a handle: "
                         "error %lu", GetLastError());
            return false;
        }
        x->handle = (uint64_t)(uintptr_t)dup;
    }
#else
    /*
     * Not qemu_dup_flags(): it ends in fcntl(F_SETFL), which a POSIX shared
     * memory descriptor on macOS refuses with ENOTTY. qemu_dup() already
     * sets close-on-exec.
     */
    x->fd = qemu_dup(r->fd);
    if (x->fd < 0) {
        /*
         * A reader that reconnects at once would hit this every time:
         * say it once.
         */
        if (!d->export_failed) {
            d->export_failed = true;
            error_report("sgi-display-shm: cannot duplicate a region "
                         "handle: %s", strerror(errno));
        }
        return false;
    }
#endif
    return true;
}

static void send_region(SGIDisplay *d, const SGIDisplayRegion *r)
{
    SGIShmMsgRegion m = {
        .hdr = { .type = SGI_SHM_MSG_REGION },
        .id = r->id, .kind = r->kind, .backend = r->backend,
        .memory_type = r->memory_type, .size = r->size,
        .alloc_size = r->alloc_size, .flags = r->flags,
    };
    SGIExport x;

    if (!region_export(d, r, &x)) {
        client_drop(d);
        return;
    }
    m.handle = x.handle;
    out_queue(d, &m, sizeof(m), x);
}

/*
 * The GPU backend when any live region is exported on the adapter, else the
 * host's shared memory. REGION names the backend of each region, which is the
 * one a reader acts on: a region whose export failed is shared memory in a
 * session that is otherwise Vulkan.
 */
static uint32_t welcome_backend(const SGIDisplay *d)
{
    const SGIDisplayRegion *r;

    QTAILQ_FOREACH(r, &d->regions, next) {
        if (r->vk) {
            return r->backend;
        }
    }
    return SGI_SHM_HOST_BACKEND;
}

static void send_welcome(SGIDisplay *d)
{
    SGIShmMsgWelcome m = {
        .hdr = { .type = SGI_SHM_MSG_WELCOME },
        .version = SGI_SHM_VERSION, .backend = welcome_backend(d),
        .nheads = qatomic_read(&d->hdr->nheads),
        .control_backend = SGI_SHM_HOST_BACKEND,
    };
    SGIExport x;

    if (m.backend != SGI_SHM_HOST_BACKEND) {
        memcpy(m.device_uuid, sgi_display_vk_device_uuid(d->vk),
               sizeof(m.device_uuid));
        memcpy(m.driver_uuid, sgi_display_vk_driver_uuid(d->vk),
               sizeof(m.driver_uuid));
    }
    if (!region_export(d, d->control, &x)) {
        client_drop(d);
        return;
    }
    m.control_handle = x.handle;
    out_queue(d, &m, sizeof(m), x);
}

static void send_header_only(SGIDisplay *d, uint16_t type, uint32_t seq)
{
    SGIShmMsgHeader m = { .type = type, .seq = seq };

    out_queue(d, &m, sizeof(m), SGI_NO_EXPORT);
}

/* WAKE goes from a bottom half, so any thread may ask for it. */
static void wake_bh(void *opaque)
{
    SGIDisplay *d = opaque;

    for (unsigned i = 0; i < d->nheads; i++) {
        SGIDisplayHead *h = &d->heads[i];
        SGIShmMsgWake m = { .hdr = { .type = SGI_SHM_MSG_WAKE },
                            .head = i };

        if (h->bound && qatomic_read(&h->rec->armed) == 1 &&
            qatomic_cmpxchg(&h->rec->armed, 1, 0) == 1) {
            out_queue(d, &m, sizeof(m), SGI_NO_EXPORT);
        }
    }
}

/*
 * Ask for a WAKE if the reader is armed. The full barrier orders the
 * generation store the caller just made before the load of armed: the
 * reader stores armed and then re-reads the generations, and without it
 * both sides can miss the other's store and the reader sleeps through a
 * change. One barrier a publish, not a pixel.
 */
static void head_wake(SGIDisplayHead *h)
{
    /* The caller's generation store before the load of armed below. */
    smp_mb();
    if (qatomic_read(&h->rec->armed) == 1) {
        qemu_bh_schedule(h->d->wake_bh);
    }
}

/* ---- Messages from the reader ---------------------------------------- */

static size_t msg_size(uint16_t type)
{
    switch (type) {
    case SGI_SHM_MSG_HELLO:
        return sizeof(SGIShmMsgHello);
    case SGI_SHM_MSG_INPUT:
        return sizeof(SGIShmMsgInput);
    default:
        return 0;
    }
}

/* The reader's HELLO has passed: tell it everything. */
static bool client_hello(SGIDisplay *d)
{
    SGIDisplayRegion *r;

    d->hello = true;
    send_welcome(d);
    QTAILQ_FOREACH(r, &d->regions, next) {
        if (d->client_fd < 0) {
            break;
        }
        send_region(d, r);
    }
    return d->client_fd >= 0;
}

static void client_read(void *opaque)
{
    SGIDisplay *d = opaque;
    bool ack = false;
    uint32_t ack_seq = 0;
    size_t off = 0;
    ssize_t n;

    n = recv(d->client_fd, (char *)d->in_buf + d->in_len,
             sizeof(d->in_buf) - d->in_len, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR)) {
        client_drop(d);
        return;
    }
    if (n < 0) {
        return;
    }
    d->in_len += n;

    /* Everything this read returned is applied in order, then one sync. */
    while (d->in_len - off >= sizeof(SGIShmMsgHeader)) {
        SGIShmMsgHeader hdr;
        size_t size;

        memcpy(&hdr, d->in_buf + off, sizeof(hdr));
        size = msg_size(hdr.type);
        if (!size) {
            warn_report("sgi-display-shm: unknown message type 0x%x from "
                        "the reader, closing", hdr.type);
            client_drop(d);
            return;
        }
        if (d->in_len - off < size) {
            break;
        }
        if (hdr.type == SGI_SHM_MSG_HELLO) {
            /* The first one was taken before this reader was accepted. */
            warn_report("sgi-display-shm: a second HELLO, closing");
            client_drop(d);
            return;
        } else {
            SGIShmMsgInput m;

            memcpy(&m, d->in_buf + off, sizeof(m));
            sgi_display_input_apply(&m);
            if (m.hdr.flags & SGI_SHM_MSGF_ACK) {
                ack = true;
                ack_seq = m.hdr.seq;
            }
        }
        off += size;
    }
    d->in_len -= off;
    memmove(d->in_buf, d->in_buf + off, d->in_len);
    sgi_display_input_sync();
    if (ack) {
        send_header_only(d, SGI_SHM_MSG_INPUT_ACK, ack_seq);
    }
}

/* Whether the process on the other end of fd is this one's user. */
static bool peer_is_self(int fd)
{
#if defined(CONFIG_LINUX)
    struct ucred cr;
    socklen_t len = sizeof(cr);

    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 &&
           cr.uid == geteuid();
#elif defined(CONFIG_DARWIN) || defined(CONFIG_BSD)
    uid_t uid;
    gid_t gid;

    return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid();
#else
    /* The socket's directory is the user's own: see shm_default_socket(). */
    return true;
#endif
}

#ifdef _WIN32
#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif

/*
 * The process on the other end of the socket, opened to duplicate handles
 * into. HELLO.pid is the reader's own word and is not used.
 */
static HANDLE peer_process(int fd)
{
    SOCKET sock = _get_osfhandle(fd);
    DWORD pid = 0, got = 0;
    HANDLE process;

    if (WSAIoctl(sock, SIO_AF_UNIX_GETPEERPID, NULL, 0, &pid, sizeof(pid),
                 &got, NULL, NULL) != 0 || !pid) {
        error_report("sgi-display-shm: cannot find the reader's process: "
                     "error %d", WSAGetLastError());
        return NULL;
    }
    process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) {
        error_report("sgi-display-shm: cannot open the reader, process "
                     "%lu: error %lu", pid, GetLastError());
    }
    return process;
}
#endif

/*
 * The first bytes of a new connection: a HELLO that passes, and nothing
 * before it. Only then does this reader replace the current one, so a
 * connection that says nothing, or the wrong thing, costs it nothing.
 */
static void pend_read(void *opaque)
{
    SGIDisplay *d = opaque;
    SGIShmMsgHello m;
    SGIShmMsgHeader hdr;
    ssize_t n;

    n = recv(d->pend_fd, (char *)d->pend_buf + d->pend_len,
             sizeof(d->pend_buf) - d->pend_len, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR)) {
        pend_drop(d);
        return;
    }
    if (n < 0) {
        return;
    }
    d->pend_len += n;
    if (d->pend_len < sizeof(hdr)) {
        return;
    }
    memcpy(&hdr, d->pend_buf, sizeof(hdr));
    if (hdr.type != SGI_SHM_MSG_HELLO) {
        warn_report("sgi-display-shm: a new reader's first message is 0x%x, "
                    "not HELLO, closing it", hdr.type);
        pend_drop(d);
        return;
    }
    if (d->pend_len < sizeof(m)) {
        return;
    }
    memcpy(&m, d->pend_buf, sizeof(m));
    if (m.version != SGI_SHM_VERSION) {
        warn_report("sgi-display-shm: the reader speaks version %u, this "
                    "is %u", m.version, SGI_SHM_VERSION);
        pend_drop(d);
        return;
    }
    /* Read no further than the HELLO: the rest is client_read()'s. */
    client_drop(d);
    d->client_fd = d->pend_fd;
    d->pend_fd = -1;
    d->pend_len = 0;
#ifdef _WIN32
    d->reader_process = d->pend_process;
    d->pend_process = NULL;
#endif
    qemu_set_fd_handler(d->client_fd, client_read, NULL, d);
    if (!client_hello(d)) {
        client_drop(d);
    }
}

static void client_accept(void *opaque)
{
    SGIDisplay *d = opaque;
    int fd = qemu_accept(d->listen_fd, NULL, NULL);

    if (fd < 0) {
        return;
    }
    if (!peer_is_self(fd)) {
        warn_report("sgi-display-shm: a connection from another user, "
                    "closing it");
        close(fd);
        return;
    }
    /* One connection waits for its HELLO at a time: the newest. */
    pend_drop(d);
    qemu_set_blocking(fd, false, NULL);
#ifdef _WIN32
    d->pend_process = peer_process(fd);
    if (!d->pend_process) {
        close(fd);
        return;
    }
#endif
    d->pend_fd = fd;
    qemu_set_fd_handler(fd, pend_read, NULL, d);
}

/* ---- Regions: the model-facing half ----------------------------------- */

/*
 * Open the adapter named by display-shm-gpu, once. NULL, with one warning,
 * when this host cannot export from it: every region is then shared memory.
 */
static SGIVkDevice *display_vk(SGIDisplay *d)
{
    Error *err = NULL;

    if (!d->gpu_requested || d->vk_tried) {
        return d->vk;
    }
    d->vk_tried = true;
    d->vk = sgi_display_vk_open(d->gpu_uuid, &err);
    if (!d->vk) {
        warn_report("sgi-display-shm: display-shm-gpu: %s; publishing "
                    "shared memory", error_get_pretty(err));
        error_free(err);
        return NULL;
    }
    info_report("sgi-display-shm: exporting scanout memory from %s",
                sgi_display_vk_device_name(d->vk));
    return d->vk;
}

/*
 * Back r with exportable Vulkan memory. False, with r untouched, when the
 * kind is not scanout memory or the adapter cannot give it: the caller then
 * maps shared memory instead.
 */
static bool region_map_vk(SGIDisplay *d, SGIDisplayRegion *r, size_t size)
{
    SGIVkDevice *vk;
    Error *err = NULL;
    SGIVkExport *x;

    /* The cursor image is small and the viewer copies it: not scanout. */
    if (r->kind == SGI_SHM_KIND_CURSOR) {
        return false;
    }
    vk = display_vk(d);
    if (!vk) {
        return false;
    }
    x = sgi_display_vk_alloc(vk, size, &err);
    if (!x) {
        warn_report_once("sgi-display-shm: cannot export a region from "
                         "the GPU (%s); it is shared memory instead",
                         error_get_pretty(err));
        error_free(err);
        return false;
    }
    r->vk = x;
    r->ptr = x->ptr;
    r->size = size;
    r->alloc_size = x->alloc_size;
    r->memory_type = x->memory_type;
    r->flags = x->dedicated ? SGI_SHM_REGION_DEDICATED : 0;
#ifdef _WIN32
    r->backend = SGI_SHM_BACKEND_VK_WIN32;
    r->handle = x->handle;
#else
    r->backend = SGI_SHM_BACKEND_VK_FD;
    r->fd = x->fd;
#endif
    return true;
}

void *sgi_display_region_alloc(Object *owner, const char *label,
                               uint64_t size, uint32_t kind,
                               SGIDisplayRegion **region)
{
    SGIDisplay *d = shm_display;
    SGIDisplayRegion *r = g_new0(SGIDisplayRegion, 1);
    g_autofree char *name = NULL;
    Error *err = NULL;
    size_t rounded = ROUND_UP(MAX(size, 1), SGI_SHM_REGION_ALIGN);

    r->kind = kind;
    if (!d || !d->name || d->exited) {
        r->local = true;
        r->size = size;
        r->ptr = g_malloc0(MAX(size, 1));
        *region = r;
        return r->ptr;
    }
#ifndef _WIN32
    r->fd = -1;
#endif
    name = g_strdup_printf("sgi-display-%s-%s", label,
                           owner ? object_get_typename(owner) : "anon");
    if (!region_map_vk(d, r, rounded) &&
        !region_map(r, name, rounded, &err)) {
        error_report_err(err);
        exit(1);
    }
    r->id = d->next_region_id++;
    QTAILQ_INSERT_TAIL(&d->regions, r, next);
    if (d->client_fd >= 0 && d->hello) {
        send_region(d, r);
    }
    *region = r;
    return r->ptr;
}

/*
 * Take the REGION for id out of the queue if none of it has been sent. The
 * descriptor or handle it holds goes with it, so the region's memory is
 * released when QEMU unmaps it and not when the reader catches up.
 */
static bool region_unsent_drop(SGIDisplay *d, uint32_t id)
{
    SGIDisplayOut *o;

    QSIMPLEQ_FOREACH(o, &d->out, next) {
        SGIShmMsgRegion m;

        if (o->off || o->len != sizeof(m)) {
            continue;
        }
        memcpy(&m, o->buf, sizeof(m));
        if (m.hdr.type == SGI_SHM_MSG_REGION && m.id == id) {
            QSIMPLEQ_REMOVE(&d->out, o, SGIDisplayOut, next);
            d->out_count--;
            out_free(d, o);
            return true;
        }
    }
    return false;
}

void sgi_display_region_free(SGIDisplayRegion *r)
{
    SGIDisplay *d = shm_display;

    if (!r) {
        return;
    }
    if (r->local) {
        g_free(r->ptr);
        g_free(r);
        return;
    }
    if (d && !d->exited) {
        SGIShmMsgRegionGone m = { .hdr = { .type = SGI_SHM_MSG_REGION_GONE },
                                  .id = r->id };

        QTAILQ_REMOVE(&d->regions, r, next);
        /* A reader that never heard of it need not hear it is gone. */
        if (!region_unsent_drop(d, r->id)) {
            out_queue(d, &m, sizeof(m), SGI_NO_EXPORT);
        }
    }
    region_unmap(r);
    g_free(r);
}

bool sgi_display_region_take_local(SGIDisplayRegion *r, void **mem)
{
    if (!r || !r->local) {
        return false;
    }
    *mem = r->ptr;
    g_free(r);
    return true;
}

/* ---- Heads ------------------------------------------------------------ */

static void layout_begin(SGIDisplayHead *h)
{
    qatomic_set(&h->rec->layout_gen, ++h->layout_gen);
    /* The odd generation is visible before any field below changes. */
    smp_mb();
}

static void layout_end(SGIDisplayHead *h)
{
    /* The fields are written before the generation turns even. */
    smp_wmb();
    qatomic_set(&h->rec->layout_gen, ++h->layout_gen);
    qatomic_inc(&h->rec->content_gen);
    head_wake(h);
}

static void dirty_mark(SGIShmHead *rec, int first, int count)
{
    int last = MIN(first + count, SGI_SHM_MAX_LINES);

    first = MAX(first, 0);
    while (first < last) {
        int word = first / 64, bit = first % 64;
        int n = MIN(64 - bit, last - first);
        uint64_t mask = (n == 64 ? ~0ull : (1ull << n) - 1) << bit;

        qatomic_or(&rec->dirty[word], mask);
        first += n;
    }
}

static void head_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    SGIDisplayHead *h = container_of(dcl, SGIDisplayHead, dcl);

    h->cursor_x = x;
    h->cursor_y = y;
    h->cursor_on = on;
    qatomic_set(&h->rec->cursor_x, x);
    qatomic_set(&h->rec->cursor_y, y);
    qatomic_set(&h->rec->cursor_on, on);
    if (on) {
        h->d->cursor_head = h->idx;
    }
    /* The position is visible before the generation that announces it. */
    smp_wmb();
    /* Two, so the generation stays even: odd means the image is changing. */
    h->cursor_gen += 2;
    qatomic_set(&h->rec->cursor_gen, h->cursor_gen);
    head_wake(h);
    sgi_display_input_cursor_moved(h->idx);
}

static void head_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    SGIDisplayHead *h = container_of(dcl, SGIDisplayHead, dcl);
    SGIShmHead *rec = h->rec;
    uint32_t w = c ? MIN(c->width, SGI_SHM_CURSOR_MAX) : 0;
    uint32_t ht = c ? MIN(c->height, SGI_SHM_CURSOR_MAX) : 0;

    /* The generation is odd while the image and its description change. */
    qatomic_set(&rec->cursor_gen, ++h->cursor_gen);
    /* The odd generation is visible before the image changes. */
    smp_mb();
    memset(h->cursor_image, 0, SGI_SHM_CURSOR_MAX * SGI_SHM_CURSOR_MAX * 4);
    for (uint32_t y = 0; y < ht; y++) {
        memcpy(h->cursor_image + (size_t)y * SGI_SHM_CURSOR_MAX * 4,
               c->data + (size_t)y * c->width, (size_t)w * 4);
    }
    qatomic_set(&rec->cursor_hot_x, c ? c->hot_x : 0);
    qatomic_set(&rec->cursor_hot_y, c ? c->hot_y : 0);
    qatomic_set(&rec->cursor_w, w);
    qatomic_set(&rec->cursor_h, ht);
    /* The image and its description are in place before it turns even. */
    smp_wmb();
    qatomic_set(&rec->cursor_gen, ++h->cursor_gen);
    head_wake(h);
}

static void head_refresh(DisplayChangeListener *dcl)
{
    qemu_console_hw_update(dcl->con);
}

static const DisplayChangeListenerOps head_ops = {
    .dpy_name = "sgi-display-shm",
    .dpy_refresh = head_refresh,
    .dpy_mouse_set = head_mouse_set,
    .dpy_cursor_define = head_cursor_define,
};

void sgi_display_head_bind(SGIScanout *s, uint32_t family,
                           uint32_t family_version)
{
    SGIDisplay *d = shm_display;
    SGIDisplayHead *h = s->dhead;

    if (!d || !d->name || d->exited) {
        return;
    }
    if (!h) {
        if (d->nheads == SGI_SHM_MAX_HEADS) {
            warn_report("sgi-display-shm: more than %d heads, the rest are "
                        "not published", SGI_SHM_MAX_HEADS);
            return;
        }
        h = &d->heads[d->nheads];
        h->d = d;
        h->s = s;
        h->idx = d->nheads;
        h->rec = (SGIShmHead *)((uint8_t *)d->control->ptr +
                                SGI_SHM_HEAD_OFFSET +
                                (size_t)h->idx * SGI_SHM_HEAD_BYTES);
        h->cursor_image = sgi_display_region_alloc(
            OBJECT(s->dev), "cursor", SGI_SHM_CURSOR_MAX *
            SGI_SHM_CURSOR_MAX * 4, SGI_SHM_KIND_CURSOR, &h->cursor_region);
        h->bound = true;
        s->dhead = h;
        layout_begin(h);
        h->rec->cursor_image.region = h->cursor_region->id;
        layout_end(h);
        h->dcl.update_interval = SGI_SHM_REFRESH_MS;
        d->nheads++;
        qatomic_set(&d->hdr->nheads, d->nheads);
        /* Registering defines the console's current cursor on the head. */
        qemu_console_register_listener(s->con, &h->dcl, &head_ops);
    }
    layout_begin(h);
    h->family = family;
    qatomic_set(&h->rec->family, family);
    qatomic_set(&h->rec->family_version, family_version);
    layout_end(h);
}

void sgi_display_head_unbind(SGIScanout *s)
{
    SGIDisplayHead *h = s->dhead;

    if (!h || !h->bound) {
        return;
    }
    if (!h->d->exited) {
        qemu_console_unregister_listener(&h->dcl);
    }
    layout_begin(h);
    for (unsigned i = 0; i < SGI_SHM_PLANES; i++) {
        memset(&h->rec->plane[i], 0, sizeof(h->rec->plane[i]));
    }
    for (unsigned i = 0; i < SGI_SHM_TABLES; i++) {
        memset(&h->rec->table[i], 0, sizeof(h->rec->table[i]));
    }
    memset(&h->rec->cursor_image, 0, sizeof(h->rec->cursor_image));
    qatomic_set(&h->rec->active, 0);
    layout_end(h);
    sgi_display_region_free(h->cursor_region);
    h->cursor_region = NULL;
    h->cursor_image = NULL;
    h->bound = false;
    s->dhead = NULL;
}

/* The layout section must be open. */
static void head_write_mode(SGIDisplayHead *h, uint32_t width,
                            uint32_t height, uint32_t refresh_mhz)
{
    h->width = width;
    h->height = height;
    qatomic_set(&h->rec->width, width);
    qatomic_set(&h->rec->height, height);
    qatomic_set(&h->rec->refresh_mhz, refresh_mhz);
    /* One bit a line: past SGI_SHM_MAX_LINES the bitmap cannot say. */
    qatomic_set(&h->rec->flags, height <= SGI_SHM_MAX_LINES ?
                SGI_SHM_HEAD_DIRTY_EXACT : 0);
    qatomic_set(&h->rec->active, width && height);
    dirty_mark(h->rec, 0, height);
}

void sgi_display_head_set_mode(SGIScanout *s, uint32_t width,
                               uint32_t height, uint32_t refresh_mhz)
{
    SGIDisplayHead *h = s->dhead;

    if (!h) {
        return;
    }
    layout_begin(h);
    head_write_mode(h, width, height, refresh_mhz);
    layout_end(h);
}

void sgi_display_head_set_frame(SGIScanout *s, SGIDisplayRegion *r,
                                uint32_t width, uint32_t height,
                                uint32_t refresh_mhz)
{
    SGIDisplayHead *h = s->dhead;
    SGIShmPlane *p;

    if (!h) {
        return;
    }
    p = &h->rec->plane[0];
    layout_begin(h);
    head_write_mode(h, width, height, refresh_mhz);
    p->region = r ? r->id : 0;
    p->format = r ? SGI_SHM_FMT_X8R8G8B8 : 0;
    p->offset = 0;
    p->width = r ? width : 0;
    p->height = r ? height : 0;
    p->stride = r ? width * 4 : 0;
    p->flags = 0;
    layout_end(h);
}

void sgi_display_head_guest_cursor(SGIScanout *s, int x, int y)
{
    SGIDisplayHead *h = s->dhead;

    if (!h) {
        return;
    }
    h->cursor_x = x;
    h->cursor_y = y;
    h->cursor_on = true;
    h->d->cursor_head = h->idx;
    qatomic_set(&h->rec->cursor_x, x);
    qatomic_set(&h->rec->cursor_y, y);
    sgi_display_input_cursor_moved(h->idx);
}

void sgi_display_head_set_plane(SGIScanout *s, unsigned i,
                                SGIDisplayRegion *r, uint64_t offset,
                                uint32_t format, uint32_t width,
                                uint32_t height, uint32_t stride,
                                uint32_t flags)
{
    SGIDisplayHead *h = s->dhead;
    SGIShmPlane *p;

    if (!h) {
        return;
    }
    assert(i < SGI_SHM_PLANES);
    assert(!r || (offset % SGI_SHM_PLANE_ALIGN == 0 && stride % 4 == 0));
    p = &h->rec->plane[i];
    layout_begin(h);
    p->region = r ? r->id : 0;
    p->format = r ? format : 0;
    p->offset = r ? offset : 0;
    p->width = r ? width : 0;
    p->height = r ? height : 0;
    p->stride = r ? stride : 0;
    p->flags = r ? flags : 0;
    dirty_mark(h->rec, 0, h->rec->height);
    layout_end(h);
}

void sgi_display_head_set_table(SGIScanout *s, unsigned i,
                                SGIDisplayRegion *r, uint64_t offset,
                                uint32_t size)
{
    SGIDisplayHead *h = s->dhead;
    SGIShmTable *t;

    if (!h) {
        return;
    }
    assert(i < SGI_SHM_TABLES);
    t = &h->rec->table[i];
    layout_begin(h);
    t->region = r ? r->id : 0;
    t->size = r ? size : 0;
    t->offset = r ? offset : 0;
    layout_end(h);
}

void *sgi_display_head_state_begin(SGIScanout *s)
{
    SGIDisplayHead *h = s->dhead;

    if (!h) {
        return NULL;
    }
    qatomic_set(&h->rec->state_gen, ++h->state_gen);
    /* The odd generation is visible before the registers change. */
    smp_mb();
    return h->rec->state;
}

void sgi_display_head_state_end(SGIScanout *s)
{
    SGIDisplayHead *h = s->dhead;

    if (!h) {
        return;
    }
    /* The registers are written before the generation turns even. */
    smp_wmb();
    qatomic_set(&h->rec->state_gen, ++h->state_gen);
    qatomic_inc(&h->rec->content_gen);
    head_wake(h);
}

void sgi_display_head_presented(SGIScanout *s, int first, int count)
{
    SGIDisplayHead *h = s->dhead;

    if (h && h->family == SGI_SHM_FAMILY_XRGB) {
        dirty_mark(h->rec, first, count);
        qatomic_set(&h->presented, true);
    }
}

bool sgi_display_head_exact(SGIScanout *s)
{
    SGIDisplayHead *h = s->dhead;

    return h && h->family == SGI_SHM_FAMILY_XRGB;
}

void sgi_display_head_invalidated(SGIScanout *s, int first, int count)
{
    SGIDisplayHead *h = s->dhead;

    if (h && h->family != SGI_SHM_FAMILY_XRGB) {
        dirty_mark(h->rec, first, count);
        qatomic_inc(&h->rec->content_gen);
        head_wake(h);
    }
}

void sgi_display_head_published(SGIScanout *s)
{
    SGIDisplayHead *h = s->dhead;

    if (h && qatomic_read(&h->presented) &&
        qatomic_xchg(&h->presented, false)) {
        /* The marks are visible before the generation that announces them. */
        smp_wmb();
        qatomic_inc(&h->rec->content_gen);
        head_wake(h);
    }
}

/* ---- What the input half asks of the heads ---------------------------- */

bool sgi_display_head_cursor(unsigned head, int32_t *x, int32_t *y, bool *on)
{
    SGIDisplay *d = shm_display;

    if (!d || head >= d->nheads || !d->heads[head].bound) {
        return false;
    }
    *x = d->heads[head].cursor_x;
    *y = d->heads[head].cursor_y;
    *on = d->heads[head].cursor_on;
    return true;
}

void sgi_display_head_mode(unsigned head, uint32_t *width, uint32_t *height)
{
    SGIDisplay *d = shm_display;

    if (!d || head >= d->nheads || !d->heads[head].bound) {
        *width = *height = 0;
        return;
    }
    *width = d->heads[head].width;
    *height = d->heads[head].height;
}

int sgi_display_cursor_head(void)
{
    return shm_display ? shm_display->cursor_head : -1;
}

unsigned sgi_display_head_count(void)
{
    return shm_display ? shm_display->nheads : 0;
}

/* ---- Start-up and exit ------------------------------------------------ */

/*
 * A directory only this user can use, for a default socket when
 * XDG_RUNTIME_DIR is unset. The temporary directory is shared: a socket
 * bound there by name could be pre-empted, or replaced, by anyone.
 */
static char *shm_private_dir(Error **errp)
{
#ifdef _WIN32
    /* The temporary directory is the user's own. */
    return g_strdup(g_get_tmp_dir());
#else
    g_autofree char *dir = g_strdup_printf("%s/sgi-display-%u",
                                           g_get_tmp_dir(),
                                           (unsigned)geteuid());
    struct stat st;

    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        error_setg_errno(errp, errno, "cannot create %s", dir);
        return NULL;
    }
    if (lstat(dir, &st) < 0) {
        error_setg_errno(errp, errno, "cannot examine %s", dir);
        return NULL;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022)) {
        error_setg(errp, "%s is not a directory that only this user can "
                   "write; set XDG_RUNTIME_DIR or display-shm-socket", dir);
        return NULL;
    }
    return g_steal_pointer(&dir);
#endif
}

static char *shm_default_socket(SGIDisplay *d, Error **errp)
{
    const char *xdg = g_getenv("XDG_RUNTIME_DIR");
    g_autofree char *dir = NULL;
    g_autofree char *file = g_strdup_printf("%s.sock", d->name);

    if (xdg && *xdg) {
        dir = g_strdup(xdg);
    } else {
        dir = shm_private_dir(errp);
        if (!dir) {
            return NULL;
        }
    }
    return g_build_filename(dir, file, NULL);
}

/*
 * Whether something is serving path. unix_listen() unlinks whatever is
 * there, which would take the socket from a running QEMU of the same name;
 * a stale one, left by a QEMU that was killed, refuses the connection.
 */
static bool shm_socket_in_use(const char *path, Error **errp)
{
    struct sockaddr_un un = { .sun_family = AF_UNIX };
    int fd, err;
#ifndef _WIN32
    struct stat st;

    if (lstat(path, &st) == 0 && !S_ISSOCK(st.st_mode)) {
        error_setg(errp, "%s exists and is not a socket", path);
        return true;
    }
#endif
    if (strlen(path) >= sizeof(un.sun_path)) {
        return false;           /* unix_listen() says so */
    }
    strcpy(un.sun_path, path);
    fd = qemu_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    qemu_set_blocking(fd, false, NULL);
    err = connect(fd, (struct sockaddr *)&un, sizeof(un)) < 0 ? errno : 0;
    close(fd);
    /* A full backlog answers EAGAIN: a live listener all the same. */
    if (!err || err == EAGAIN || err == EWOULDBLOCK) {
        error_setg(errp, "another process is serving %s", path);
        return true;
    }
    return false;
}

static void shm_exit(Notifier *n, void *data)
{
    SGIDisplay *d = container_of(n, SGIDisplay, exit);

    client_drop(d);
    if (d->listen_fd >= 0) {
        qemu_set_fd_handler(d->listen_fd, NULL, NULL, NULL);
        close(d->listen_fd);
        d->listen_fd = -1;
        unlink(d->sock_path);
    }
    pend_drop(d);
    /* The listeners stop with the process; they must not see a dead block. */
    for (unsigned i = 0; i < d->nheads; i++) {
        if (d->heads[i].bound) {
            qemu_console_unregister_listener(&d->heads[i].dcl);
        }
    }
    d->exited = true;
}

static void shm_machine_done(Notifier *n, void *data)
{
    SGIDisplay *d = container_of(n, SGIDisplay, done);
    Error *err = NULL;
#ifndef _WIN32
    mode_t old_umask;
#endif

    if (!d->name) {
        warn_report("sgi-display-shm: display-shm-socket or display-shm-gpu "
                    "is set without display-shm, so nothing is published");
        return;
    }
    if (!d->nheads) {
        warn_report("sgi-display-shm: this machine has no graphic head");
    }
    if (d->test_pattern) {
        if (!d->nheads) {
            error_report("x-display-shm-test-pattern needs a graphic head");
            exit(1);
        }
        sgi_display_pattern_start(d->heads[0].s);
    }
    /* Say now, not at the first region, why a GPU request fell back. */
    display_vk(d);
    d->exit.notify = shm_exit;
    qemu_add_exit_notifier(&d->exit);

    if (!d->sock_path) {
        d->sock_path = shm_default_socket(d, &err);
    }
    if (!d->sock_path || shm_socket_in_use(d->sock_path, &err)) {
        error_report_err(err);
        exit(1);
    }
#ifndef _WIN32
    /* Bind the socket for its owner only. Start-up is single threaded. */
    old_umask = umask(0077);
#endif
    d->listen_fd = unix_listen(d->sock_path, &err);
#ifndef _WIN32
    umask(old_umask);
#endif
    if (d->listen_fd < 0) {
        error_report_err(err);
        exit(1);
    }
    qemu_set_fd_handler(d->listen_fd, client_accept, NULL, d);
    /* Everything is in place: nothing reads the block before this. */
    smp_wmb();
    qatomic_set(&d->hdr->magic, SGI_SHM_MAGIC);
    info_report("sgi-display-shm: %u head(s), socket %s", d->nheads,
                d->sock_path);
}

static SGIDisplay *shm_get_display(void)
{
    if (!shm_display) {
        shm_display = g_new0(SGIDisplay, 1);
        shm_display->listen_fd = -1;
        shm_display->client_fd = -1;
        shm_display->pend_fd = -1;
        shm_display->next_region_id = 1;
        shm_display->cursor_head = -1;
        QTAILQ_INIT(&shm_display->regions);
        QSIMPLEQ_INIT(&shm_display->out);
        shm_display->done.notify = shm_machine_done;
        qemu_add_machine_init_done_notifier(&shm_display->done);
    }
    return shm_display;
}

static bool shm_control_create(SGIDisplay *d, Error **errp)
{
    SGIDisplayRegion *r = g_new0(SGIDisplayRegion, 1);

#ifndef _WIN32
    r->fd = -1;
#endif
    if (!region_map(r, "sgi-display-control", SGI_SHM_CONTROL_BYTES, errp)) {
        g_free(r);
        return false;
    }
    d->control = r;
    d->hdr = (SGIShmHeader *)r->ptr;
    d->hdr->version = SGI_SHM_VERSION;
    d->hdr->head_bytes = SGI_SHM_HEAD_BYTES;
    d->hdr->head_offset = SGI_SHM_HEAD_OFFSET;
    d->hdr->qemu_pid = getpid();
    d->wake_bh = qemu_bh_new(wake_bh, d);
    return true;
}

static char *shm_get_name(Object *obj, Error **errp)
{
    return g_strdup(shm_display && shm_display->name ? shm_display->name : "");
}

static void shm_set_name(Object *obj, const char *value, Error **errp)
{
    SGIDisplay *d = shm_get_display();

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
    /* The control block exists before any model asks for a region. */
    if (!shm_control_create(d, errp)) {
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
    SGIDisplay *d = shm_get_display();

    g_free(d->sock_path);
    d->sock_path = *value ? g_strdup(value) : NULL;
}

static char *shm_get_gpu(Object *obj, Error **errp)
{
    SGIDisplay *d = shm_display;
    GString *s = g_string_new(NULL);

    if (d && d->gpu_requested) {
        for (unsigned i = 0; i < 16; i++) {
            g_string_append_printf(s, "%02x", d->gpu_uuid[i]);
        }
    }
    return g_string_free(s, false);
}

static void shm_set_gpu(Object *obj, const char *value, Error **errp)
{
    SGIDisplay *d = shm_get_display();
    uint8_t uuid[16];

    if (strlen(value) != 32) {
        error_setg(errp, "display-shm-gpu takes the 32 hex digits of a "
                   "Vulkan deviceUUID");
        return;
    }
    for (unsigned i = 0; i < 16; i++) {
        int hi = g_ascii_xdigit_value(value[2 * i]);
        int lo = g_ascii_xdigit_value(value[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            error_setg(errp, "display-shm-gpu takes the 32 hex digits of a "
                       "Vulkan deviceUUID");
            return;
        }
        uuid[i] = hi << 4 | lo;
    }
    memcpy(d->gpu_uuid, uuid, sizeof(uuid));
    d->gpu_requested = true;
}

/*
 * Test only: "X,Y,ON" moves head 0's host cursor as a model would, through
 * the console, so a qtest can play the guest answering a pointer step.
 */
static void shm_set_test_cursor(Object *obj, const char *value, Error **errp)
{
    SGIDisplay *d = shm_get_display();
    int x, y, on;

    if (sscanf(value, "%d,%d,%d", &x, &y, &on) != 3 || !d->nheads ||
        !d->heads[0].bound) {
        error_setg(errp, "x-display-shm-cursor takes X,Y,ON for head 0");
        return;
    }
    qemu_console_set_mouse(d->heads[0].s->con, x, y, on);
}

static char *shm_get_test_cursor(Object *obj, Error **errp)
{
    return g_strdup("");
}

static bool shm_get_test_pattern(Object *obj, Error **errp)
{
    return shm_display && shm_display->test_pattern;
}

static void shm_set_test_pattern(Object *obj, bool value, Error **errp)
{
    shm_get_display()->test_pattern = value;
}

void sgi_display_shm_class_add_properties(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "x-display-shm-test-pattern",
                                   shm_get_test_pattern,
                                   shm_set_test_pattern);
    object_class_property_add_str(oc, "x-display-shm-cursor",
                                  shm_get_test_cursor, shm_set_test_cursor);
    object_class_property_add_str(oc, "display-shm", shm_get_name,
                                  shm_set_name);
    object_class_property_set_description(oc, "display-shm",
        "Publish the graphic heads to one external reader over a local "
        "socket (NAME.sock unless display-shm-socket says otherwise); NAME "
        "is 1 to 30 characters of A-Z a-z 0-9 . _ -; see "
        "docs/system/mips/sgi/display-shm.rst");
    object_class_property_add_str(oc, "display-shm-socket", shm_get_socket,
                                  shm_set_socket);
    object_class_property_set_description(oc, "display-shm-socket",
        "Path of the display-shm socket; default NAME.sock in "
        "$XDG_RUNTIME_DIR, or in the temporary directory when that is unset");
    object_class_property_add_str(oc, "display-shm-gpu", shm_get_gpu,
                                  shm_set_gpu);
    object_class_property_set_description(oc, "display-shm-gpu",
        "The Vulkan deviceUUID (32 hex digits) to export scanout memory "
        "on; without it, or when the export fails, regions are plain "
        "shared memory");
}
