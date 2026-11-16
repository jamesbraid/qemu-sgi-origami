/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The shared-memory display of the SGI machines, read as an external viewer
 * would: connect to the socket, say HELLO, take the control block and the
 * regions as passed handles, check them against the contract in
 * docs/system/mips/sgi/display-shm.rst at raw offsets, and send binary
 * input. Almost none of it needs a GPU: every region is plain shared memory,
 * and the display-shm-gpu tests at the end check the fallbacks without one.
 * The last test exports through a real Vulkan device when the host has one,
 * and imports the handle in a second process, which is this binary run with
 * --vk-helper. POSIX hosts only; the Windows handle passing has no reader
 * here.
 */
#include "qemu/osdep.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "prereq.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "ui/sgi-display-shm.h"
#include "ui/sgi-display-vk-abi.h"

#define WAIT_MS 20000
#define QUIET_MS 400
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "reset-mode=0x1aa683"
#define GPU_UUID "0123456789abcdef0123456789abcdef"

#define VPRO_SHORT 0x1b000000ULL
#define VPRO_PATH "/machine/module0/widget-11"
#define ODSY_BACKEND_SELECT 0x400000
#define ODSY_BACKEND_HDR(index, count) \
    (0x30000000u | ((uint32_t)(index) << 14) | (count))

static char *prom_path;
static char *sock_dir;
static char *sock_path;

static QTestState *start_with(const char *machine_extra)
{
    return qtest_initf("-M octane2,display-shm=sgi-dsp-%d,"
                       "display-shm-socket=%s%s -m 256M -cpu %s -bios %s -S "
                       "-display none -serial none -monitor none",
                       getpid(), sock_path, machine_extra, OCTANE2_CPU,
                       prom_path);
}

static QTestState *start(void)
{
    return start_with("");
}

/* ---- The wire --------------------------------------------------------- */

typedef struct Msg {
    uint8_t b[64];
    unsigned len;
    int fd;                     /* the handle that came with it, or -1 */
    bool eof;
} Msg;

static unsigned msg_len(unsigned type)
{
    switch (type) {
    case 0x81:
        return 64;
    case 0x82:
        return 56;
    case 0x83:
    case 0x84:
        return 16;
    case 0x85:
        return 8;
    default:
        g_assert_not_reached();
    }
}

static int connect_socket(void)
{
    gint64 end = g_get_monotonic_time() + WAIT_MS * 1000;
    struct sockaddr_un un = { .sun_family = AF_UNIX };

    g_assert_cmpuint(strlen(sock_path), <, sizeof(un.sun_path));
    strcpy(un.sun_path, sock_path);
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

static bool wait_readable(int fd, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };

    return poll(&p, 1, ms) == 1;
}

/*
 * One message from QEMU. The handle rides with the first byte, so the header
 * is read with recvmsg() and the rest with read(). False on a quiet socket,
 * or with m->eof set when QEMU closed it.
 */
static bool recv_msg(int sock, Msg *m, int ms)
{
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct iovec iov = { .iov_base = m->b, .iov_len = 8 };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = ctl.buf,
                         .msg_controllen = sizeof(ctl.buf) };
    struct cmsghdr *c;
    ssize_t n;
    unsigned got = 8;

    memset(m, 0, sizeof(*m));
    m->fd = -1;
    iov.iov_base = m->b;
    if (!wait_readable(sock, ms)) {
        return false;
    }
    n = recvmsg(sock, &mh, 0);
    if (n == 0) {
        m->eof = true;
        return false;
    }
    g_assert_cmpint(n, ==, 8);
    for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            g_assert_cmpuint(c->cmsg_len, ==, CMSG_LEN(sizeof(int)));
            memcpy(&m->fd, CMSG_DATA(c), sizeof(int));
        }
    }
    m->len = msg_len(lduw_le_p(m->b));
    while (got < m->len) {
        g_assert_true(wait_readable(sock, WAIT_MS));
        n = read(sock, m->b + got, m->len - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
    return true;
}

static void expect_msg(int sock, Msg *m, unsigned type)
{
    g_assert_true(recv_msg(sock, m, WAIT_MS));
    g_assert_cmphex(lduw_le_p(m->b), ==, type);
}

/* Whether QEMU has hung up: readable with end of file. */
static bool hung_up(int sock)
{
    Msg m;

    for (int i = 0; i < 50; i++) {
        if (recv_msg(sock, &m, 100)) {
            if (m.fd >= 0) {
                close(m.fd);
            }
            continue;
        }
        if (m.eof) {
            return true;
        }
    }
    return false;
}

static void send_all(int sock, const void *buf, size_t len)
{
    g_assert_cmpint(write(sock, buf, len), ==, len);
}

static void send_hello_ver(int sock, uint32_t version)
{
    uint8_t b[48] = {};

    stw_le_p(b, 0x0001);
    stl_le_p(b + 8, version);
    stl_le_p(b + 12, 0);
    stl_le_p(b + 16, getpid());
    send_all(sock, b, sizeof(b));
}

static void send_hello(int sock)
{
    send_hello_ver(sock, 3);
}

static void fill_input(uint8_t *b, uint32_t seq, bool ack, uint32_t kind,
                       uint32_t head, uint32_t code, uint32_t value,
                       int32_t x, int32_t y)
{
    memset(b, 0, 32);
    stw_le_p(b, 0x0002);
    stw_le_p(b + 2, ack ? 1 : 0);
    stl_le_p(b + 4, seq);
    stl_le_p(b + 8, kind);
    stl_le_p(b + 12, head);
    stl_le_p(b + 16, code);
    stl_le_p(b + 20, value);
    stl_le_p(b + 24, x);
    stl_le_p(b + 28, y);
}

/* ---- A reader's session ---------------------------------------------- */

typedef struct Region {
    uint32_t id, kind, backend, memory_type, flags;
    uint64_t size, alloc_size;
    uint8_t *ptr;               /* NULL for a handle that cannot be mapped */
    int fd;
} Region;

typedef struct Session {
    QTestState *qts;
    int sock;
    Msg welcome;
    uint8_t *ctl;
    int ctl_fd;
    Region regions[16];
    unsigned nregions;
} Session;

static uint8_t *head_raw(Session *s, unsigned h)
{
    return s->ctl + 4096 + 4096 * h;
}

static Region *find_region(Session *s, uint32_t id)
{
    for (unsigned i = 0; i < s->nregions; i++) {
        if (s->regions[i].id == id) {
            return &s->regions[i];
        }
    }
    return NULL;
}

static uint8_t *map_fd(int fd, size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    g_assert(p != MAP_FAILED);
    return p;
}

/* Take every REGION that follows WELCOME until the socket goes quiet. */
static void take_regions(Session *s)
{
    Msg m;

    while (recv_msg(s->sock, &m, QUIET_MS)) {
        Region *r;

        g_assert_cmphex(lduw_le_p(m.b), ==, 0x82);
        g_assert_cmpuint(s->nregions, <, G_N_ELEMENTS(s->regions));
        g_assert_cmpint(m.fd, >=, 0);
        r = &s->regions[s->nregions++];
        r->id = ldl_le_p(m.b + 8);
        r->kind = ldl_le_p(m.b + 12);
        r->backend = ldl_le_p(m.b + 16);
        r->memory_type = ldl_le_p(m.b + 20);
        r->size = ldq_le_p(m.b + 24);
        r->alloc_size = ldq_le_p(m.b + 32);
        r->flags = ldl_le_p(m.b + 40);
        r->fd = m.fd;
        /* An opaque GPU handle is the importer's, not for mmap(). */
        if (r->backend == SGI_SHM_BACKEND_SHM_FD) {
            r->ptr = map_fd(m.fd, r->size);
        }
    }
}

static void session_open(Session *s, QTestState *qts)
{
    memset(s, 0, sizeof(*s));
    s->qts = qts;
    s->sock = connect_socket();
    send_hello(s->sock);
    expect_msg(s->sock, &s->welcome, 0x81);
    g_assert_cmpint(s->welcome.fd, >=, 0);
    s->ctl_fd = s->welcome.fd;
    s->ctl = map_fd(s->ctl_fd, SGI_SHM_CONTROL_BYTES);
    take_regions(s);
}

static void session_close(Session *s)
{
    for (unsigned i = 0; i < s->nregions; i++) {
        if (s->regions[i].ptr) {
            munmap(s->regions[i].ptr, s->regions[i].size);
        }
        close(s->regions[i].fd);
    }
    munmap(s->ctl, SGI_SHM_CONTROL_BYTES);
    close(s->ctl_fd);
    close(s->sock);
}

/* Poll a head's 64-bit content_gen until it differs from old. */
static uint64_t wait_content(Session *s, unsigned h, uint64_t old)
{
    gint64 end = g_get_monotonic_time() + WAIT_MS * 1000;
    uint64_t *gen = (uint64_t *)(head_raw(s, h) + 32);

    while (qatomic_read(gen) == old) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(5000);
    }
    return qatomic_read(gen);
}

static uint64_t content_gen(Session *s, unsigned h)
{
    return qatomic_read((uint64_t *)(head_raw(s, h) + 32));
}

/* The reader's side of the bitmap: take every word and clear it. */
static uint64_t take_dirty(Session *s, unsigned h, uint64_t words[64])
{
    uint64_t any = 0;

    for (unsigned i = 0; i < 64; i++) {
        words[i] = qatomic_xchg((uint64_t *)(head_raw(s, h) + 1536 + 8 * i),
                                0);
        any |= words[i];
    }
    return any;
}

static bool zero_range(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/* ---- Screendump ------------------------------------------------------- */

/*
 * The pixels of a QMP screendump of the VPro's console (the machine's own
 * first console is a text one) as 0x00RRGGBB.
 */
static uint32_t *screendump(QTestState *qts, uint32_t *w, uint32_t *ht)
{
    g_autofree char *path = g_build_filename(sock_dir, "dump.ppm", NULL);
    g_autofree char *data = NULL;
    uint32_t *px;
    gsize len;
    unsigned maxval;
    int hdr_len = 0;

    qtest_qmp_assert_success(qts,
        "{'execute': 'screendump', 'arguments': {'filename': %s, "
        "'device': 'widget-11'}}", path);
    g_assert(g_file_get_contents(path, &data, &len, NULL));
    unlink(path);
    g_assert_cmpint(sscanf(data, "P6\n%u %u\n%u\n%n", w, ht, &maxval,
                           &hdr_len), >=, 3);
    g_assert_cmpuint(maxval, ==, 255);
    g_assert_cmpuint(len, ==, hdr_len + (gsize)*w * *ht * 3);
    px = g_new(uint32_t, (size_t)*w * *ht);
    for (size_t i = 0; i < (size_t)*w * *ht; i++) {
        const uint8_t *p = (uint8_t *)data + hdr_len + i * 3;

        px[i] = p[0] << 16 | p[1] << 8 | p[2];
    }
    return px;
}

/* ---- Tests ------------------------------------------------------------ */

/* The control block and the messages, at the offsets the page states. */
static void test_layout(void)
{
    QTestState *qts = start();
    Session s;
    const uint8_t *w, *raw, *hd;
    Region *frame, *cursor;
    uint32_t dw, dht, frame_id, cursor_id;
    uint32_t *dump;

    session_open(&s, qts);
    w = s.welcome.b;
    g_assert_cmpuint(s.welcome.len, ==, 64);
    g_assert_cmpuint(ldl_le_p(w + 4), ==, 0);       /* seq: zero */
    g_assert_cmpuint(ldl_le_p(w + 8), ==, 3);       /* version */
    g_assert_cmpuint(ldl_le_p(w + 12), ==, 3);      /* backend: SHM_FD */
    g_assert_cmpuint(ldl_le_p(w + 16), ==, 1);      /* nheads */
    g_assert_cmpuint(ldl_le_p(w + 20), ==, 3);      /* control_backend */
    g_assert_true(zero_range(w + 24, 16));          /* device_uuid */
    g_assert_true(zero_range(w + 40, 16));          /* driver_uuid */
    g_assert_cmpuint(ldq_le_p(w + 56), ==, 0);      /* handle: fd attached */

    /* The control block's header, 256 bytes. */
    raw = s.ctl;
    g_assert_cmpmem(raw, 4, "GSS3", 4);
    g_assert_cmphex(ldl_le_p(raw + 0), ==, 0x33535347);
    g_assert_cmpuint(ldl_le_p(raw + 4), ==, 3);
    g_assert_cmpuint(ldl_le_p(raw + 8), ==, 1);     /* nheads */
    g_assert_cmpuint(ldl_le_p(raw + 12), ==, 4096); /* head_bytes */
    g_assert_cmpuint(ldl_le_p(raw + 16), ==, 4096); /* head_offset */
    g_assert_cmpuint(ldl_le_p(raw + 20), ==, qtest_pid(qts));
    g_assert_true(zero_range(raw + 24, 232));
    g_assert_true(zero_range(raw + 256, 4096 - 256));
    g_assert_true(zero_range(raw + 4096 * 2, 65536 - 4096 * 2));

    /* Head 0. */
    hd = head_raw(&s, 0);
    g_assert_cmpuint(ldl_le_p(hd + 0), ==, 1);      /* active */
    g_assert_cmpuint(ldl_le_p(hd + 4), ==, 1);      /* family: xrgb */
    g_assert_cmpuint(ldl_le_p(hd + 8), ==, 1);      /* family_version */
    g_assert_cmpuint(ldl_le_p(hd + 12) & 1, ==, 0); /* layout_gen even */
    g_assert_cmpuint(ldl_le_p(hd + 16), ==, 1280);
    g_assert_cmpuint(ldl_le_p(hd + 20), ==, 1024);
    g_assert_cmpuint(ldl_le_p(hd + 28) & 1, ==, 1); /* dirty lines exact */
    g_assert_cmpuint(ldl_le_p(hd + 40) & 1, ==, 0); /* state_gen even */
    g_assert_cmpuint(ldl_le_p(hd + 44), ==, 0);     /* armed */
    g_assert_cmpuint(ldl_le_p(hd + 72), <=, 64);    /* cursor_w */
    g_assert_cmpuint(ldl_le_p(hd + 76), <=, 64);    /* cursor_h */

    /* plane[0] at 80: the frame, x8r8g8b8, offset 0, 5,120-byte rows. */
    frame_id = ldl_le_p(hd + 80);
    g_assert_cmpuint(frame_id, >=, 1);
    g_assert_cmpuint(ldl_le_p(hd + 84), ==, 1);     /* format */
    g_assert_cmpuint(ldq_le_p(hd + 88), ==, 0);     /* offset */
    g_assert_cmpuint(ldl_le_p(hd + 96), ==, 1280);
    g_assert_cmpuint(ldl_le_p(hd + 100), ==, 1024);
    g_assert_cmpuint(ldl_le_p(hd + 104), ==, 5120);
    g_assert_cmpuint(ldl_le_p(hd + 108), ==, 0);    /* flags */
    g_assert_true(zero_range(hd + 80 + 32, 7 * 32));    /* planes 1 to 7 */
    g_assert_true(zero_range(hd + 336, 4 * 16));        /* no tables */
    /* cursor_image at 400: its own region, offset 0. */
    cursor_id = ldl_le_p(hd + 400);
    g_assert_cmpuint(cursor_id, >=, 1);
    g_assert_cmpuint(ldl_le_p(hd + 404), ==, 0);
    g_assert_cmpuint(ldq_le_p(hd + 408), ==, 0);
    g_assert_true(zero_range(hd + 416, 96));            /* reserved */
    g_assert_true(zero_range(hd + 512, 1024));          /* no family state */
    g_assert_true(zero_range(hd + 2048, 2048));         /* reserved */
    /* Heads that do not exist are zero. */
    g_assert_true(zero_range(head_raw(&s, 1), 4096));

    /* The regions, each as a REGION message and a handle. */
    g_assert_cmpuint(s.nregions, ==, 2);
    frame = find_region(&s, frame_id);
    cursor = find_region(&s, cursor_id);
    g_assert_nonnull(frame);
    g_assert_nonnull(cursor);
    g_assert_cmpuint(frame->kind, ==, 1);
    g_assert_cmpuint(cursor->kind, ==, 4);
    g_assert_cmpuint(frame->backend, ==, 3);
    g_assert_cmpuint(frame->size, ==, 5 * 1024 * 1024);
    g_assert_cmpuint(cursor->size, ==, 65536);
    for (unsigned i = 0; i < s.nregions; i++) {
        struct stat st;

        g_assert_cmpuint(s.regions[i].size % 65536, ==, 0);
        g_assert_cmpint(fstat(s.regions[i].fd, &st), ==, 0);
        g_assert_cmpuint(st.st_size, ==, s.regions[i].size);
    }

    /* The frame is what the console shows. */
    dump = screendump(qts, &dw, &dht);
    g_assert_cmpuint(dw, ==, 1280);
    g_assert_cmpuint(dht, ==, 1024);
    for (size_t i = 0; i < (size_t)dw * dht; i++) {
        g_assert_cmphex(ldl_le_p(frame->ptr + i * 4) & 0xffffff, ==, dump[i]);
    }
    g_free(dump);

    session_close(&s);
    qtest_quit(qts);
}

/* REGION in full, at its raw offsets, and the handle's properties. */
static void test_region_message(void)
{
    QTestState *qts = start();
    int sock = connect_socket();
    Msg m;
    uint64_t size;
    int fd;

    send_hello(sock);
    expect_msg(sock, &m, 0x81);
    close(m.fd);
    expect_msg(sock, &m, 0x82);
    g_assert_cmpuint(m.len, ==, 56);
    g_assert_cmpuint(ldl_le_p(m.b + 4), ==, 0);     /* seq */
    g_assert_cmpuint(ldl_le_p(m.b + 8), >=, 1);     /* id */
    g_assert_true(ldl_le_p(m.b + 12) == 1 ||
                  ldl_le_p(m.b + 12) == 4);         /* kind: plane, cursor */
    g_assert_cmpuint(ldl_le_p(m.b + 16), ==, 3);    /* backend */
    g_assert_cmpuint(ldl_le_p(m.b + 20), ==, 0);    /* memory_type */
    size = ldq_le_p(m.b + 24);
    g_assert_cmpuint(size % 65536, ==, 0);
    g_assert_cmpuint(ldq_le_p(m.b + 32), ==, size); /* alloc_size */
    g_assert_cmpuint(ldl_le_p(m.b + 40), ==, 0);    /* flags */
    g_assert_cmpuint(ldl_le_p(m.b + 44), ==, 0);
    g_assert_cmpuint(ldq_le_p(m.b + 48), ==, 0);    /* handle: fd attached */
    fd = m.fd;
    g_assert_cmpint(fd, >=, 0);

#ifdef F_GET_SEALS
    /* Sealed against shrinking and growing: a reader's mapping stays valid. */
    {
        int seals = fcntl(fd, F_GET_SEALS);

        if (seals >= 0) {
            g_assert_cmphex(seals & (F_SEAL_SHRINK | F_SEAL_GROW), ==,
                            F_SEAL_SHRINK | F_SEAL_GROW);
            g_assert_cmpint(ftruncate(fd, size / 2), ==, -1);
        }
    }
#endif
    /* Two mappings of one handle share their bytes. */
    {
        uint8_t *a = map_fd(fd, size), *b = map_fd(fd, size);

        a[12345] = 0x5a;
        g_assert_cmphex(b[12345], ==, 0x5a);
        a[12345] = 0;
        munmap(a, size);
        munmap(b, size);
    }
    close(fd);
    close(sock);
    qtest_quit(qts);
}

#define ODSY_DISPMODE_RGB 0x005215a6u

static void backend_push(QTestState *qts, uint32_t word)
{
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, word);
}

/*
 * The screen (drawable 0) in an RGB mode with an identity colour table, so
 * a pixel written to board memory shows as its own colour. Out of reset the
 * screen is black whatever the memory holds.
 */
static void display_identity(QTestState *qts)
{
    backend_push(qts, ODSY_BACKEND_HDR(0x2900, 1));
    backend_push(qts, ODSY_DISPMODE_RGB);
    backend_push(qts, ODSY_BACKEND_HDR(0x1a00, 0x600));
    for (uint32_t i = 0; i < 0x600; i++) {
        uint32_t v = i < 0x200 ? i >> 2 :
                     i < 0x400 ? ((i - 0x200) >> 1) + 0x80 :
                     (i - 0x300) + 0x100;

        backend_push(qts, v << 20 | v << 10 | v);
    }
}

static void set_test_pixel(QTestState *qts, uint32_t x, uint32_t y,
                           uint32_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", VPRO_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", VPRO_PATH, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", VPRO_PATH, value);
}

/*
 * Only a line whose pixels changed is marked. The VPro invalidates every
 * line on any write, so this is the resolve's compare at work: one pixel
 * marks one line, the same pixel written again marks none and leaves
 * content_gen alone. An armed reader gets one WAKE.
 */
static void test_dirty(void)
{
    QTestState *qts = start();
    uint64_t words[64], gen;
    Session s;
    Msg m;

    session_open(&s, qts);
    display_identity(qts);
    /* Let the new colour table settle, then take its marks. */
    g_usleep(300 * 1000);
    take_dirty(&s, 0, words);
    gen = content_gen(&s, 0);
    g_usleep(300 * 1000);
    g_assert_cmpuint(content_gen(&s, 0), ==, gen);
    g_assert_cmphex(take_dirty(&s, 0, words), ==, 0);

    /* One pixel on line 100: line 100 and no other line is marked. */
    set_test_pixel(qts, 5, 100, 0x11223300);
    gen = wait_content(&s, 0, gen);
    g_usleep(100 * 1000);
    take_dirty(&s, 0, words);
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(words[i], ==, i == 100 / 64 ?
                                      1ull << (100 % 64) : 0);
    }
    /* The pixel is in the frame when the bit is. */
    {
        const Region *f = find_region(&s, ldl_le_p(head_raw(&s, 0) + 80));

        g_assert_cmphex(ldl_le_p(f->ptr + 100 * 5120 + 5 * 4) & 0xffffff,
                        ==, 0x112233);
    }

    /* The same pixel again: every line is re-resolved, none changed. */
    set_test_pixel(qts, 5, 100, 0x11223300);
    g_usleep(300 * 1000);
    g_assert_cmpuint(content_gen(&s, 0), ==, gen);
    g_assert_cmphex(take_dirty(&s, 0, words), ==, 0);

    /* Armed: the next change wakes the reader once and disarms the head. */
    stl_le_p(head_raw(&s, 0) + 44, 1);
    set_test_pixel(qts, 5, 100, 0x44556600);
    expect_msg(s.sock, &m, 0x84);
    g_assert_cmpuint(m.len, ==, 16);
    g_assert_cmpuint(ldl_le_p(m.b + 8), ==, 0);     /* head */
    g_assert_cmpuint(ldl_le_p(m.b + 12), ==, 0);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 44), ==, 0);
    wait_content(&s, 0, gen);
    g_assert_false(recv_msg(s.sock, &m, QUIET_MS));

    session_close(&s);
    qtest_quit(qts);
}

/* Move head 0's host cursor as a model would (see the machine property). */
static void guest_cursor(QTestState *qts, int x, int y, bool on)
{
    g_autofree char *value = g_strdup_printf("%d,%d,%d", x, y, on);

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': '/machine', "
        "'property': 'x-display-shm-cursor', 'value': %s}}", value);
}

/* A guest cursor move bumps cursor_gen by 2 and wakes an armed viewer. */
static void test_cursor_wake(void)
{
    QTestState *qts = start();
    uint32_t gen;
    Session s;
    Msg m;

    session_open(&s, qts);
    gen = ldl_le_p(head_raw(&s, 0) + 48);
    g_assert_cmpuint(gen & 1, ==, 0);

    /* Not armed: no WAKE, the generation still moves, by 2. */
    guest_cursor(qts, 10, 20, true);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 48), ==, gen + 2);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 52), ==, 1);
    g_assert_cmpint((int32_t)ldl_le_p(head_raw(&s, 0) + 56), ==, 10);
    g_assert_cmpint((int32_t)ldl_le_p(head_raw(&s, 0) + 60), ==, 20);
    g_assert_false(recv_msg(s.sock, &m, QUIET_MS));

    /* Armed: one WAKE for the head, and the head is disarmed. */
    stl_le_p(head_raw(&s, 0) + 44, 1);
    guest_cursor(qts, 11, 21, true);
    expect_msg(s.sock, &m, 0x84);
    g_assert_cmpuint(ldl_le_p(m.b + 8), ==, 0);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 44), ==, 0);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 48), ==, gen + 4);

    /* Hiding it is a change too. */
    guest_cursor(qts, 11, 21, false);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 48), ==, gen + 6);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 52), ==, 0);
    session_close(&s);
    qtest_quit(qts);
}

/*
 * A new mode replaces the frame's region: a new id, REGION_GONE for the old
 * one, the plane descriptor rewritten under an odd layout_gen.
 */
static void test_resize(void)
{
    QTestState *qts = start();
    uint32_t old_id, new_id, lg;
    uint64_t old_size;
    Region *old;
    Session s;
    Msg m;

    session_open(&s, qts);
    old_id = ldl_le_p(head_raw(&s, 0) + 80);
    old = find_region(&s, old_id);
    g_assert_nonnull(old);
    old_size = old->size;
    lg = ldl_le_p(head_raw(&s, 0) + 12);

    /* The channel's size and format words: 1280 x 720 in a 5-word record. */
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT,
                 ODSY_BACKEND_HDR(0x2802, 5));
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 1280);
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 720);
    for (int i = 0; i < 3; i++) {
        qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 0);
    }

    expect_msg(s.sock, &m, 0x82);
    new_id = ldl_le_p(m.b + 8);
    g_assert_cmpuint(new_id, >, old_id);
    g_assert_cmpuint(ldl_le_p(m.b + 12), ==, 1);
    g_assert_cmpuint(ldq_le_p(m.b + 24), ==,
                     ROUND_UP(1280 * 720 * 4, 65536));
    g_assert_cmpuint(ldq_le_p(m.b + 24), <, old_size);
    close(m.fd);
    expect_msg(s.sock, &m, 0x83);
    g_assert_cmpuint(m.len, ==, 16);
    g_assert_cmpuint(ldl_le_p(m.b + 8), ==, old_id);
    g_assert_cmpuint(ldl_le_p(m.b + 12), ==, 0);

    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 12) & 1, ==, 0);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 12), >, lg);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 16), ==, 1280);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 20), ==, 720);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 80), ==, new_id);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 96), ==, 1280);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 100), ==, 720);
    g_assert_cmpuint(ldl_le_p(head_raw(&s, 0) + 104), ==, 5120);

    /* A reader that kept the old mapping still has valid memory. */
    old->ptr[0] = 1;
    session_close(&s);
    qtest_quit(qts);
}

/* A reader that hangs up and comes back is told everything again. */
static void test_reconnect(void)
{
    QTestState *qts = start();
    Session a, b;
    uint32_t ids[2];

    session_open(&a, qts);
    g_assert_cmpuint(a.nregions, ==, 2);
    ids[0] = a.regions[0].id;
    ids[1] = a.regions[1].id;
    session_close(&a);

    session_open(&b, qts);
    /* The same regions, with the same ids: none was reused for another. */
    g_assert_cmpuint(b.nregions, ==, 2);
    g_assert_cmpuint(b.regions[0].id, ==, ids[0]);
    g_assert_cmpuint(b.regions[1].id, ==, ids[1]);
    g_assert_cmphex(ldl_le_p(b.ctl), ==, 0x33535347);
    g_assert_cmpuint(ldl_le_p(b.welcome.b + 16), ==, 1);
    session_close(&b);
    qtest_quit(qts);
}

/*
 * After a mode change a reconnecting reader gets only the live regions, the
 * replacement under its new id, and no id is handed out twice.
 */
static void test_reconnect_after_resize(void)
{
    QTestState *qts = start();
    uint32_t old_frame;
    Session a, b;
    Msg m;

    session_open(&a, qts);
    old_frame = ldl_le_p(head_raw(&a, 0) + 80);
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT,
                 ODSY_BACKEND_HDR(0x2802, 5));
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 1280);
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 720);
    for (int i = 0; i < 3; i++) {
        qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, 0);
    }
    expect_msg(a.sock, &m, 0x82);
    close(m.fd);
    expect_msg(a.sock, &m, 0x83);
    session_close(&a);

    session_open(&b, qts);
    g_assert_cmpuint(b.nregions, ==, 2);
    g_assert_null(find_region(&b, old_frame));
    g_assert_nonnull(find_region(&b, ldl_le_p(head_raw(&b, 0) + 80)));
    g_assert_cmpuint(ldl_le_p(head_raw(&b, 0) + 80), >, old_frame);
    g_assert_cmpuint(find_region(&b, ldl_le_p(head_raw(&b, 0) + 80))->size,
                     ==, ROUND_UP(1280 * 720 * 4, 65536));
    session_close(&b);
    qtest_quit(qts);
}

/* The reader's mistakes close the connection and nothing else. */
static void test_protocol_errors(void)
{
    QTestState *qts = start();
    uint8_t b[32];
    Msg m;
    int sock;

    /* An unknown type. */
    sock = connect_socket();
    memset(b, 0, sizeof(b));
    stw_le_p(b, 0x7777);
    send_all(sock, b, 8);
    g_assert_true(hung_up(sock));
    close(sock);

    /* Input before HELLO. */
    sock = connect_socket();
    fill_input(b, 1, true, SGI_SHM_INPUT_RELATIVE, 0, 0, 0, 1, 1);
    send_all(sock, b, sizeof(b));
    g_assert_true(hung_up(sock));
    close(sock);

    /* A HELLO for another version. */
    sock = connect_socket();
    send_hello_ver(sock, 2);
    g_assert_true(hung_up(sock));
    close(sock);

    /* A second HELLO. */
    sock = connect_socket();
    send_hello(sock);
    expect_msg(sock, &m, 0x81);
    close(m.fd);
    send_hello(sock);
    g_assert_true(hung_up(sock));
    close(sock);

    /* QEMU still serves the next reader. */
    sock = connect_socket();
    send_hello(sock);
    expect_msg(sock, &m, 0x81);
    close(m.fd);
    close(sock);
    qtest_quit(qts);
}

/*
 * A reader sees plain shared memory everywhere, and WELCOME and every REGION
 * say so, with the Vulkan fields zero.
 */
static void check_shm_only(Session *s)
{
    g_assert_cmpuint(ldl_le_p(s->welcome.b + 12), ==, 3);   /* backend */
    g_assert_cmpuint(ldl_le_p(s->welcome.b + 20), ==, 3);   /* control */
    g_assert_true(zero_range(s->welcome.b + 24, 32));       /* both UUIDs */
    g_assert_cmpuint(s->nregions, ==, 2);
    for (unsigned i = 0; i < s->nregions; i++) {
        g_assert_cmpuint(s->regions[i].backend, ==, 3);
        g_assert_cmpuint(s->regions[i].memory_type, ==, 0);
        g_assert_cmpuint(s->regions[i].alloc_size, ==, s->regions[i].size);
        g_assert_cmpuint(s->regions[i].flags, ==, 0);
    }
}

/*
 * A deviceUUID no adapter has: whatever the host's Vulkan, the answer is
 * shared memory, said in WELCOME and in every REGION. The property reads
 * back.
 */
static void test_fallback(void)
{
    QTestState *qts = start_with(",display-shm-gpu=" GPU_UUID);
    QDict *resp;
    Session s;

    resp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                     "{'path': '/machine', 'property': 'display-shm-gpu'}}");
    g_assert_cmpstr(qdict_get_str(resp, "return"), ==, GPU_UUID);
    qobject_unref(resp);

    session_open(&s, qts);
    check_shm_only(&s);
    session_close(&s);
    qtest_quit(qts);
}

/*
 * No Vulkan loader on the host, as in a container without a GPU: the loader
 * name the environment gives does not exist. The same answer, and QEMU
 * starts.
 */
static void test_fallback_no_loader(void)
{
    QTestState *qts;
    Session s;

    g_setenv("QEMU_SGI_VULKAN_LOADER", "/nonexistent/libvulkan.so.1", true);
    qts = start_with(",display-shm-gpu=" GPU_UUID);
    g_unsetenv("QEMU_SGI_VULKAN_LOADER");
    session_open(&s, qts);
    check_shm_only(&s);
    session_close(&s);
    qtest_quit(qts);
}

/* ---- The Vulkan export, with a real device -------------------------- */

/* Every SGI machine type offers all three properties, from one helper. */
static void test_properties_on_every_machine(void)
{
    static const char *const machines[] = {
        "octane", "octane2", "o2", "origin200", "origin2000", "onyx2",
        "origin300", "origin3000", "origin350", "onyx4", "onyx350", "tezro",
        "fuel",
    };
    QTestState *qts = qtest_init("-machine none");

    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        /* A build without every SGI family still checks the ones it has. */
        if (!qtest_has_machine(machines[i])) {
            continue;
        }
        g_autofree char *type = g_strdup_printf("%s-machine", machines[i]);
        QDict *resp = qtest_qmp(qts, "{'execute': 'qom-list-properties',"
                                "'arguments': {'typename': %s}}", type);
        QList *props = qdict_get_qlist(resp, "return");
        bool shm = false, sock = false, gpu = false;
        QListEntry *e;

        g_assert(props);
        QLIST_FOREACH_ENTRY(props, e) {
            const char *name = qdict_get_str(qobject_to(QDict, e->value),
                                             "name");

            shm |= g_str_equal(name, "display-shm");
            sock |= g_str_equal(name, "display-shm-socket");
            gpu |= g_str_equal(name, "display-shm-gpu");
        }
        g_assert_true(shm);
        g_assert_true(sock);
        g_assert_true(gpu);
        qobject_unref(resp);
    }
    qtest_quit(qts);
}

/* ---- Guest cursors that live in the model's pixels ------------------- */

#define HEAD_CURSOR_GEN     48
#define HEAD_CURSOR_ON      52
#define HEAD_CURSOR_X       56
#define HEAD_CURSOR_Y       60

#define ODSY_CURSOR_POSITION    0x2580
#define ODSY_CURSOR_CONTROL     0x2581

#define MGRAS_VC3_INDEXDATA     (0x1b000000ULL + 0x072038)
#define MGRAS_VC3_WORD(index, data) \
    (((uint32_t)(index) << 24) | ((uint32_t)(data) << 8))
#define MGRAS_CURSOR_ORIGIN     31

#define GBE_BASE                0x16000000ULL
#define GBE_VT(n)               (GBE_BASE + 0x10000 + (n) * 4)
#define GBE_CRS_0               (GBE_BASE + 0x70000)

static int32_t head_cursor_x(Session *s)
{
    return (int32_t)qatomic_read((uint32_t *)(head_raw(s, 0) +
                                              HEAD_CURSOR_X));
}

static int32_t head_cursor_y(Session *s)
{
    return (int32_t)qatomic_read((uint32_t *)(head_raw(s, 0) +
                                              HEAD_CURSOR_Y));
}

/*
 * A model that draws its cursor into its own pixels publishes only where the
 * guest put it: cursor_on stays 0 so a viewer shows no second cursor, and
 * there is no new generation, the cursor being in the pixels. gen is the
 * one binding the head left.
 */
static void assert_cursor_published_only_as_position(Session *s, uint32_t gen,
                                                     int x, int y)
{
    g_assert_cmpint(head_cursor_x(s), ==, x);
    g_assert_cmpint(head_cursor_y(s), ==, y);
    g_assert_cmpuint(ldl_le_p(head_raw(s, 0) + HEAD_CURSOR_ON), ==, 0);
    g_assert_cmpuint(ldl_le_p(head_raw(s, 0) + HEAD_CURSOR_GEN), ==, gen);
}

/* The VPro's position entry: the glyph's top-left pixel, x:y in 31:16, 15:0. */
static void test_vpro_cursor_position(void)
{
    QTestState *qts = start();
    uint32_t gen;
    Session s;

    session_open(&s, qts);
    gen = ldl_le_p(head_raw(&s, 0) + HEAD_CURSOR_GEN);
    assert_cursor_published_only_as_position(&s, gen, 0, 0);

    backend_push(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_POSITION, 1));
    backend_push(qts, 380u << 16 | 372);
    assert_cursor_published_only_as_position(&s, gen, 380, 372);

    /* Enabling it, or loading a glyph, moves nothing. */
    backend_push(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_CONTROL, 1));
    backend_push(qts, 1);
    assert_cursor_published_only_as_position(&s, gen, 380, 372);

    /* Each coordinate is a signed 16-bit value. */
    backend_push(qts, ODSY_BACKEND_HDR(ODSY_CURSOR_POSITION, 1));
    backend_push(qts, ((uint32_t)(uint16_t)-5) << 16 | 1023);
    assert_cursor_published_only_as_position(&s, gen, -5, 1023);
    session_close(&s);
    qtest_quit(qts);
}

static QTestState *start_octane_mgras(void)
{
    return qtest_initf("-M octane,display-shm=sgi-dsp-%d,"
                       "display-shm-socket=%s -m 128M -cpu R10000 -bios %s "
                       "-S -display none -serial none -monitor none "
                       "-device sgi-mgras,slot=widget-11,product=si",
                       getpid(), sock_path, prom_path);
}

/*
 * The IMPACT's VC3 registers 2 and 3 are the glyph's top-left pixel plus
 * 31. They are separate writes, so the position is published at the next
 * retrace, once, not as an x with the old y.
 */
static void test_impact_cursor_position(void)
{
    QTestState *qts = start_octane_mgras();
    uint32_t gen;
    Session s;

    /* The retrace timer is on the virtual clock, which only runs when going. */
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    session_open(&s, qts);
    gen = ldl_le_p(head_raw(&s, 0) + HEAD_CURSOR_GEN);
    assert_cursor_published_only_as_position(&s, gen, 0, 0);

    qtest_writel(qts, MGRAS_VC3_INDEXDATA,
                 MGRAS_VC3_WORD(2, 380 + MGRAS_CURSOR_ORIGIN));
    g_assert_cmpint(head_cursor_x(&s), ==, 0);
    qtest_writel(qts, MGRAS_VC3_INDEXDATA,
                 MGRAS_VC3_WORD(3, 372 + MGRAS_CURSOR_ORIGIN));
    g_assert_cmpint(head_cursor_y(&s), ==, 0);
    qtest_clock_step(qts, 100 * 1000 * 1000);
    assert_cursor_published_only_as_position(&s, gen, 380, 372);

    /* Near the top left the glyph's corner is off the screen. */
    qtest_writel(qts, MGRAS_VC3_INDEXDATA, MGRAS_VC3_WORD(2, 10));
    qtest_writel(qts, MGRAS_VC3_INDEXDATA, MGRAS_VC3_WORD(3, 0));
    qtest_clock_step(qts, 100 * 1000 * 1000);
    assert_cursor_published_only_as_position(&s, gen,
                                             10 - MGRAS_CURSOR_ORIGIN,
                                             -MGRAS_CURSOR_ORIGIN);
    session_close(&s);
    qtest_quit(qts);
}

/*
 * The O2's GBE places CRS_0's position by the timing registers: less the
 * 54-dot lead taken from VT_18, plus VT_18's signed Y start.
 */
static void test_gbe_cursor_position(void)
{
    QTestState *qts = qtest_initf(
        "-M o2,display-shm=sgi-dsp-%d,display-shm-socket=%s,"
        "register-test-profile=on,bank-profile=8x128m-synthetic,"
        "crime-profile=1.5,reset-low-ram-approx=on,"
        "functional-memory-transfer=on,prom-private-window=on,"
        "prom-sb64-reset=on,gbe11-registers=on,prom-id-word=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=238 -cpu R10000 -m 1G -display none "
        "-serial none -monitor none -S", getpid(), sock_path, prom_path);
    uint32_t gen;
    Session s;

    session_open(&s, qts);
    gen = ldl_le_p(head_raw(&s, 0) + HEAD_CURSOR_GEN);
    assert_cursor_published_only_as_position(&s, gen, 0, 0);

    /* No line period yet: nothing to place the cursor in. */
    qtest_writel(qts, GBE_CRS_0, 7u << 16 | 9);
    assert_cursor_published_only_as_position(&s, gen, 0, 0);

    /* 1344 dots by 806 lines, the cursor lead 54 dots before the start. */
    qtest_writel(qts, GBE_VT(1), 806u << 12 | 1344);
    qtest_writel(qts, GBE_VT(18), (1344 - 54));
    qtest_writel(qts, GBE_CRS_0, 100u << 16 | 200);
    assert_cursor_published_only_as_position(&s, gen, 200, 100);

    /* A negative position is signed, and VT_18's Y start moves it. */
    qtest_writel(qts, GBE_VT(18), 3u << 12 | (1344 - 54));
    qtest_writel(qts, GBE_CRS_0, ((uint32_t)(uint16_t)-4) << 16 |
                                 (uint16_t)-31);
    assert_cursor_published_only_as_position(&s, gen, -31, -1);
    session_close(&s);
    qtest_quit(qts);
}

/* ---- Input ------------------------------------------------------------ */

#define BRIDGE          0x1f000000ULL
#define CFG2            (BRIDGE + 0x020000 + 2 * 0x1000)
#define IOC3            (BRIDGE + 0x600000)
#define KBD_RECEIVE     (IOC3 + 0x0a0)
#define AUX_RECEIVE     (IOC3 + 0x0a4)
#define AUX_TRANSMIT    (IOC3 + 0x0ac)
#define RX_VALID(p)     (1u << (29 + (p)))

/* Bytes the receive register at @reg yields until it stays empty. */
static unsigned drain(QTestState *qts, uint64_t reg, uint8_t *out,
                      unsigned max)
{
    unsigned n = 0;

    for (int idle = 0; idle < 8 && n < max; ) {
        uint32_t v = qtest_readl(qts, reg);
        unsigned got = 0;

        for (int p = 2; p >= 0; p--) {
            if (v & RX_VALID(p) && n < max) {
                out[n++] = (v >> (p * 8)) & 0xff;
                got++;
            }
        }
        idle = got ? 0 : idle + 1;
        qtest_clock_step(qts, 2 * 1000 * 1000);
    }
    return n;
}

/* An Octane2 with the IOC3 reachable, the guest running and a reader in. */
static QTestState *input_start(int *sock)
{
    QTestState *qts = start();
    Msg m;

    qtest_writel(qts, CFG2 + 0x10, 0x10000000);
    qtest_writel(qts, CFG2 + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    *sock = connect_socket();
    send_hello(*sock);
    expect_msg(*sock, &m, 0x81);
    close(m.fd);
    /* The regions that follow WELCOME. */
    while (recv_msg(*sock, &m, QUIET_MS)) {
        g_assert_cmphex(lduw_le_p(m.b), ==, 0x82);
        close(m.fd);
    }
    return qts;
}

static void test_input_key(void)
{
    uint8_t buf[16], b[32];
    unsigned n;
    int sock;
    QTestState *qts = input_start(&sock);
    Msg m;

    /* Linux evdev KEY_A is 30: PS/2 set 2 make code 0x1c. */
    fill_input(b, 7, true, SGI_SHM_INPUT_KEY, 0, 30, 1, 0, 0);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(m.len, ==, 8);
    g_assert_cmpuint(ldl_le_p(m.b + 4), ==, 7);
    n = drain(qts, KBD_RECEIVE, buf, sizeof(buf));
    g_assert_cmpuint(n, >=, 1);
    g_assert_cmphex(buf[0], ==, 0x1c);

    /* Key up: the break sequence 0xf0, 0x1c. */
    fill_input(b, 8, false, SGI_SHM_INPUT_KEY, 0, 30, 0, 0, 0);
    send_all(sock, b, sizeof(b));
    n = drain(qts, KBD_RECEIVE, buf, sizeof(buf));
    g_assert_cmpuint(n, >=, 2);
    g_assert_cmphex(buf[0], ==, 0xf0);
    g_assert_cmphex(buf[1], ==, 0x1c);
    /* No ack without the flag. */
    g_assert_false(recv_msg(sock, &m, QUIET_MS));

    /* Codes outside 1..0x2ff are dropped and the connection survives. */
    fill_input(b, 9, true, SGI_SHM_INPUT_KEY, 0, 0x300, 1, 0, 0);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(ldl_le_p(m.b + 4), ==, 9);
    g_assert_cmpuint(drain(qts, KBD_RECEIVE, buf, sizeof(buf)), ==, 0);
    close(sock);
    qtest_quit(qts);
}

/* A viewer that drops with a key down must not leave it stuck. */
static void test_input_release_on_drop(void)
{
    uint8_t buf[16], b[32];
    int sock;
    QTestState *qts = input_start(&sock);
    Msg m;

    fill_input(b, 1, true, SGI_SHM_INPUT_KEY, 0, 30, 1, 0, 0);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(drain(qts, KBD_RECEIVE, buf, sizeof(buf)), >=, 1);
    close(sock);
    /* The break sequence arrives although the viewer sent no key up. */
    g_assert_cmpuint(drain(qts, KBD_RECEIVE, buf, sizeof(buf)), >=, 2);
    g_assert_cmphex(buf[0], ==, 0xf0);
    g_assert_cmphex(buf[1], ==, 0x1c);
    qtest_quit(qts);
}

static void enable_mouse(QTestState *qts)
{
    uint8_t buf[16];

    /* The mouse reports only after the guest enables streaming (0xf4). */
    qtest_writel(qts, AUX_TRANSMIT, 0xf4);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 1);
    g_assert_cmphex(buf[0], ==, 0xfa);
}

static void test_input_mouse(void)
{
    uint8_t buf[16], b[96];
    int sock;
    QTestState *qts = input_start(&sock);
    Msg m;

    enable_mouse(qts);

    /*
     * One write, three messages: one batch, one guest report, one ack with
     * the last sequence. x +5, y +3 (QEMU's y is down) and the left button.
     */
    fill_input(b, 1, true, SGI_SHM_INPUT_RELATIVE, 0, 0, 0, 5, 3);
    fill_input(b + 32, 2, false, SGI_SHM_INPUT_BUTTON, 0,
               0 /* INPUT_BUTTON_LEFT */, 1, 0, 0);
    fill_input(b + 64, 3, true, SGI_SHM_INPUT_RELATIVE, 0, 0, 0, 0, 0);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(ldl_le_p(m.b + 4), ==, 3);
    g_assert_false(recv_msg(sock, &m, QUIET_MS));
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 3);
    g_assert_cmphex(buf[0] & 0x09, ==, 0x09);   /* left button, always-1 */
    g_assert_cmphex(buf[1], ==, 5);
    g_assert_cmphex(buf[2], ==, 0xfd);          /* -3, PS/2 y is up */

    /* An unknown button is dropped. */
    fill_input(b, 4, true, SGI_SHM_INPUT_BUTTON, 0, 999, 1, 0, 0);
    send_all(sock, b, 32);
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 0);

    /* An unknown kind is dropped too. */
    fill_input(b, 5, true, 99, 0, 0, 0, 1, 1);
    send_all(sock, b, 32);
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 0);
    close(sock);
    qtest_quit(qts);
}

/*
 * An absolute position is a first relative step from the cursor QEMU last
 * saw (the origin before the guest shows one), one step per guest cursor
 * update after that. A head that does not exist is dropped.
 */
static void test_input_absolute(void)
{
    uint8_t buf[16], b[32];
    int sock;
    QTestState *qts = input_start(&sock);
    Msg m;

    enable_mouse(qts);
    fill_input(b, 1, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0, 7, 9);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 3);
    g_assert_cmphex(buf[1], ==, 7);
    g_assert_cmphex(buf[2], ==, 0xf7);          /* -9 */

    fill_input(b, 2, true, SGI_SHM_INPUT_ABSOLUTE, 3, 0, 0, 7, 9);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 0);
    close(sock);
    qtest_quit(qts);
}

/* ---- The absolute pointer against a simulated guest -------------------- */

/* Virtual time QEMU waits for a guest answer, and to call one complete. */
#define ANSWER_MS 250
#define SETTLE_MS 40
#define HEAD_W 1280
#define HEAD_H 1024

/*
 * The next mouse packet, as the QEMU pointer counts it carried (y down).
 * False when none starts in @wait_ms of virtual time. The controller
 * shifts the bytes in over time, so a started packet is read to its end.
 */
static bool next_packet(QTestState *qts, int *dx, int *dy, int wait_ms)
{
    uint8_t p[3];
    unsigned n = 0;

    for (int ms = 0; ; ms++) {
        uint32_t v = qtest_readl(qts, AUX_RECEIVE);

        for (int i = 2; i >= 0 && n < 3; i--) {
            if (v & RX_VALID(i)) {
                p[n++] = (v >> (i * 8)) & 0xff;
            }
        }
        if (n == 3 || (!n && ms >= wait_ms)) {
            break;
        }
        g_assert_cmpint(ms, <, wait_ms + 100);
        qtest_clock_step(qts, 1000 * 1000);
    }
    if (!n) {
        return false;
    }
    g_assert_cmpuint(n, ==, 3);
    g_assert_cmphex(p[0] & 0x08, ==, 0x08);
    /* At most 127 counts, so the 8-bit value is the whole delta. */
    *dx = (int8_t)p[1];
    *dy = -(int8_t)p[2];
    g_assert_cmpint(!!(p[0] & 0x10), ==, *dx < 0);
    g_assert_cmpint(!!(p[0] & 0x20), ==, *dy > 0);
    return true;
}

/*
 * How the simulated guest turns one packet's counts on one axis into
 * pixels. IRIX 6.5.30 on the VPro, observed with one packet at a time: up
 * to 4 counts move that many pixels, n counts above that 2n - 4.
 */
static int irix_accel(int n)
{
    int a = abs(n);

    return n < 0 ? -(a <= 4 ? a : 2 * a - 4) : (a <= 4 ? a : 2 * a - 4);
}

/* A guest that moves one pixel a count, as the PROM does. */
static int linear_accel(int n)
{
    return n;
}

/* A steeper guest: one count is one pixel, n above that 4n - 3. */
static int steep_accel(int n)
{
    int a = abs(n);

    return n < 0 ? -(a <= 1 ? a : 4 * a - 3) : (a <= 1 ? a : 4 * a - 3);
}

typedef struct SimGuest {
    QTestState *qts;
    int (*accel)(int n);
    /*
     * Answer every packet late and in parts: first the old position again,
     * then x alone, then y, SETTLE_MS / 2 apart.
     */
    bool lag;
    int max_x, max_y;           /* where the guest stops its cursor */
    int x, y;
    unsigned steps;             /* packets for the current target */
} SimGuest;

static void sim_report(SimGuest *g, int x, int y)
{
    guest_cursor(g->qts, x, y, true);
}

/* Play the guest until QEMU stops sending; returns the packets it took. */
static unsigned sim_run(SimGuest *g)
{
    int dx, dy;

    g->steps = 0;
    while (next_packet(g->qts, &dx, &dy, ANSWER_MS + SETTLE_MS)) {
        int nx = MAX(0, MIN(g->max_x, g->x + g->accel(dx)));
        int ny = MAX(0, MIN(g->max_y, g->y + g->accel(dy)));

        g->steps++;
        g_assert_cmpuint(g->steps, <=, 32);
        if (g->lag) {
            sim_report(g, g->x, g->y);
            qtest_clock_step(g->qts, SETTLE_MS / 2 * 1000 * 1000);
            sim_report(g, nx, g->y);
            qtest_clock_step(g->qts, SETTLE_MS / 2 * 1000 * 1000);
        }
        g->x = nx;
        g->y = ny;
        sim_report(g, nx, ny);
    }
    return g->steps;
}

static void sim_target(SimGuest *g, int sock, int x, int y)
{
    static uint32_t seq;
    uint8_t b[32];
    Msg m;

    fill_input(b, ++seq, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0, x, y);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    sim_run(g);
    g_test_message("target %d,%d: at %d,%d after %u steps", x, y, g->x,
                   g->y, g->steps);
}

/*
 * Every corner to the opposite one and to the middle, then far targets in
 * turn, as a viewer moving across the head would send them. The first
 * target of each pair also starts from wherever the last one left the
 * cursor, so nothing learnt on one target may spoil the next.
 */
static const int sim_targets[][2] = {
    { 0, 0 }, { 1279, 1023 }, { 640, 512 },
    { 1279, 0 }, { 0, 1023 }, { 640, 512 },
    { 1279, 1023 }, { 0, 0 }, { 640, 512 },
    { 0, 1023 }, { 1279, 0 }, { 640, 512 },
    { 380, 372 }, { 580, 512 }, { 680, 172 }, { 80, 72 }, { 880, 872 },
    { 5, 1018 }, { 1274, 3 }, { 641, 513 }, { 1, 1 },
};

static void sim_all_targets(int (*accel)(int n), bool lag)
{
    int sock;
    QTestState *qts = input_start(&sock);
    SimGuest g = { .qts = qts, .accel = accel, .lag = lag,
                   .max_x = HEAD_W - 1, .max_y = HEAD_H - 1,
                   .x = 640, .y = 512 };

    enable_mouse(qts);
    sim_report(&g, g.x, g.y);
    for (unsigned i = 0; i < ARRAY_SIZE(sim_targets); i++) {
        sim_target(&g, sock, sim_targets[i][0], sim_targets[i][1]);
        g_assert_cmpint(g.x, ==, sim_targets[i][0]);
        g_assert_cmpint(g.y, ==, sim_targets[i][1]);
    }
    close(sock);
    qtest_quit(qts);
}

/*
 * A guest that accelerates as IRIX does: a gain learnt on a large step is
 * twice the gain of a small one. Every target lands exactly, in one packet
 * per step, with nothing learnt on one target spoiling the next.
 */
static void test_input_absolute_accelerating(void)
{
    sim_all_targets(irix_accel, false);
}

/* Steeper still: four pixels a count above a threshold of one. */
static void test_input_absolute_steep(void)
{
    sim_all_targets(steep_accel, false);
}

/*
 * A guest whose answer lags the packet and comes in parts: first a report
 * that has not moved, then x, then y. QEMU waits for the whole answer
 * before the next step.
 */
static void test_input_absolute_lagging(void)
{
    sim_all_targets(irix_accel, true);
}

/*
 * A target the guest will not move its cursor to: IRIX on the VPro stops
 * the reported corner 8 pixels short of the right and bottom edges. The
 * move ends at the edge, on one axis while the other still closes in and
 * on both when both are held, and the next target is reached as usual.
 */
static void test_input_absolute_edge(void)
{
    int sock;
    QTestState *qts = input_start(&sock);
    SimGuest g = { .qts = qts, .accel = irix_accel,
                   .max_x = HEAD_W - 9, .max_y = HEAD_H - 9,
                   .x = 640, .y = 512 };

    enable_mouse(qts);
    sim_report(&g, g.x, g.y);
    sim_target(&g, sock, HEAD_W - 1, 300);
    g_assert_cmpint(g.x, ==, HEAD_W - 9);
    g_assert_cmpint(g.y, ==, 300);
    sim_target(&g, sock, HEAD_W - 1, HEAD_H - 1);
    g_assert_cmpint(g.x, ==, HEAD_W - 9);
    g_assert_cmpint(g.y, ==, HEAD_H - 9);
    g_assert_cmpuint(g.steps, <=, 8);
    sim_target(&g, sock, 100, 100);
    g_assert_cmpint(g.x, ==, 100);
    g_assert_cmpint(g.y, ==, 100);
    close(sock);
    qtest_quit(qts);
}

/*
 * The guest answers a step by moving its cursor; QEMU sizes the next step
 * from what that one did. The first step is 1:1 from (0, 0). A guest that
 * moves only to (3, 4) shows gains of 3/7 and 4/9, so the 4 and 5 still to
 * go take steps of 9 and 11. A relative message then ends the move: the
 * next answer sends nothing.
 */
static void test_input_absolute_loop(void)
{
    uint8_t buf[16], b[32];
    int sock, dx, dy;
    QTestState *qts = input_start(&sock);
    Msg m;

    enable_mouse(qts);
    fill_input(b, 1, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0, 7, 9);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_true(next_packet(qts, &dx, &dy, 0));
    g_assert_cmpint(dx, ==, 7);
    g_assert_cmpint(dy, ==, 9);

    /* Not complete until the cursor has been still for SETTLE_MS. */
    guest_cursor(qts, 3, 4, true);
    qtest_clock_step(qts, (SETTLE_MS - 1) * 1000 * 1000);
    g_assert_false(next_packet(qts, &dx, &dy, 0));
    g_assert_true(next_packet(qts, &dx, &dy, 2));
    g_assert_cmpint(dx, ==, 9);
    g_assert_cmpint(dy, ==, 11);

    /* A relative message replaces the target. */
    fill_input(b, 2, true, SGI_SHM_INPUT_RELATIVE, 0, 0, 0, 0, 0);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    guest_cursor(qts, 6, 8, true);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 0);
    g_assert_false(next_packet(qts, &dx, &dy, ANSWER_MS + SETTLE_MS));

    /* A step the guest never answers ends the move. */
    fill_input(b, 3, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0, 100, 8);
    send_all(sock, b, sizeof(b));
    expect_msg(sock, &m, 0x85);
    g_assert_true(next_packet(qts, &dx, &dy, 0));
    g_assert_cmpint(dx, ==, 94);
    g_assert_cmpint(dy, ==, 0);
    qtest_clock_step(qts, ANSWER_MS * 1000 * 1000);
    guest_cursor(qts, 20, 8, true);
    g_assert_false(next_packet(qts, &dx, &dy, ANSWER_MS + SETTLE_MS));
    close(sock);
    qtest_quit(qts);
}

/*
 * A viewer whose pointer keeps moving sends a new target before the guest
 * has answered the step in flight. QEMU holds the next step until that
 * answer, then sizes it for the newest target, rather than sending a packet
 * for every target from a cursor position that is already out of date.
 */
static void test_input_absolute_retarget(void)
{
    static const int moving[][2] = {
        { 600, 500 }, { 700, 520 }, { 800, 560 }, { 900, 640 }, { 1000, 300 },
    };
    int sock, dx, dy;
    QTestState *qts = input_start(&sock);
    SimGuest g = { .qts = qts, .accel = irix_accel,
                   .max_x = HEAD_W - 1, .max_y = HEAD_H - 1,
                   .x = 640, .y = 512 };
    uint8_t b[32];
    Msg m;

    enable_mouse(qts);
    sim_report(&g, g.x, g.y);
    for (unsigned i = 0; i < ARRAY_SIZE(moving); i++) {
        fill_input(b, i + 1, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0,
                   moving[i][0], moving[i][1]);
        send_all(sock, b, sizeof(b));
        expect_msg(sock, &m, 0x85);
    }
    /* One packet, for the first target, and no more until it is answered. */
    g_assert_true(next_packet(qts, &dx, &dy, 0));
    g_assert_cmpint(dx, ==, 600 - 640);
    g_assert_cmpint(dy, ==, 500 - 512);
    g_assert_false(next_packet(qts, &dx, &dy, SETTLE_MS - 1));
    g.x += irix_accel(dx);
    g.y += irix_accel(dy);
    sim_report(&g, g.x, g.y);
    /* The rest of the move closes on the last target. */
    sim_run(&g);
    g_assert_cmpint(g.x, ==, 1000);
    g_assert_cmpint(g.y, ==, 300);
    close(sock);
    qtest_quit(qts);
}

/*
 * A pointer moving at a steady speed is aimed at ahead of itself, by the
 * distance it covers while a step lands, and a pointer that stops is aimed
 * at exactly. Targets 10 ms apart at 1 px/ms make a lead of tens of pixels;
 * the step sized after the answer reaches past the newest target, and once
 * the targets stop the cursor ends on it.
 */
static void test_input_absolute_lead(void)
{
    int sock, dx, dy, last_dx = 0;
    QTestState *qts = input_start(&sock);
    SimGuest g = { .qts = qts, .accel = linear_accel,
                   .max_x = HEAD_W - 1, .max_y = HEAD_H - 1,
                   .x = 100, .y = 100 };
    uint8_t b[32];
    Msg m;

    enable_mouse(qts);
    sim_report(&g, g.x, g.y);
    for (unsigned i = 0; i < 4; i++) {
        fill_input(b, i + 1, true, SGI_SHM_INPUT_ABSOLUTE, 0, 0, 0,
                   200 + 10 * i, 100);
        send_all(sock, b, sizeof(b));
        expect_msg(sock, &m, 0x85);
        if (i < 3) {
            qtest_clock_step(qts, 10 * 1000 * 1000);
        }
    }
    /* The first step was for the first target, 100 px away. */
    g_assert_true(next_packet(qts, &dx, &dy, 0));
    g_assert_cmpint(dx, ==, 100);
    g.x += linear_accel(dx);
    sim_report(&g, g.x, g.y);
    /* The next is for where the pointer is going: past its newest target. */
    g_assert_true(next_packet(qts, &dx, &dy, ANSWER_MS));
    g_test_message("lead step %d for a target 30 px on (cursor at %d)", dx,
                   g.x);
    g_assert_cmpint(g.x + linear_accel(dx), >, 230);
    g.x += linear_accel(dx);
    sim_report(&g, g.x, g.y);
    /* Then the pointer is quiet and the cursor comes back to it. */
    qtest_clock_step(qts, 100 * 1000 * 1000);
    while (next_packet(qts, &dx, &dy, ANSWER_MS + SETTLE_MS)) {
        last_dx = dx;
        g.x += linear_accel(dx);
        sim_report(&g, g.x, g.y);
    }
    g_assert_cmpint(last_dx, !=, 0);
    g_assert_cmpint(g.x, ==, 230);
    close(sock);
    qtest_quit(qts);
}

/* The socket is removed when QEMU exits. */
static void test_cleanup_on_exit(void)
{
    QTestState *qts = start();
    Session s;

    session_open(&s, qts);
    session_close(&s);
    g_assert_cmpint(access(sock_path, F_OK), ==, 0);
    qtest_quit(qts);
    g_assert_cmpint(access(sock_path, F_OK), ==, -1);
}

/*
 * The helper process: this binary run as "--vk-helper list" or "--vk-helper
 * import ...". It speaks Vulkan the way a viewer does, through the loader
 * opened at run time, with the declarations the QEMU side uses.
 *
 * list
 *     One line for each device that can export and import an opaque handle
 *     for a storage buffer: its deviceUUID as 32 hex digits, a tab, its name.
 * import UUID FD MEMORY_TYPE ALLOC_SIZE DEDICATED
 *     Import descriptor FD as QEMU's REGION says (memory type, allocation
 *     size, dedicated or not) into the device UUID, map it, print "imported",
 *     and then answer lines on standard input: "frame SHA256 WIDTH HEIGHT
 *     STRIDE" waits up to 10 seconds for the mapped frame, as 0x00RRGGBB
 *     words, to hash to SHA256 and prints "ok", or "bad" with the hash it
 *     saw. End of input ends the process.
 */
typedef struct VkHelper {
    void *lib;
    VkInstance inst;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkEnumeratePhysicalDevices enumerate_devices;
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_ext;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr get_device_proc;
    VkPhysicalDevice pd[16];
    uint8_t uuid[16][16];
    char name[16][256];
    uint32_t npd;
} VkHelper;

static void *vk_sym(VkHelper *h, const char *name)
{
    void *p = (void *)h->get_instance_proc(h->inst, name);

    g_assert_nonnull(p);
    return p;
}

/* False when the host has no loader or no device: the test then skips. */
static bool vk_helper_open(VkHelper *h)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_MAKE_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    PFN_vkCreateInstance create_instance;
    VkPhysicalDevice all[16];
    uint32_t n = 16;
    const char *loader = g_getenv("QEMU_SGI_VULKAN_LOADER");

    memset(h, 0, sizeof(*h));
    h->lib = dlopen(loader && *loader ? loader : SGI_VK_LOADER,
                    RTLD_NOW | RTLD_LOCAL);
    if (!h->lib) {
        return false;
    }
    h->get_instance_proc = dlsym(h->lib, "vkGetInstanceProcAddr");
    g_assert_nonnull(h->get_instance_proc);
    create_instance = (void *)h->get_instance_proc(NULL, "vkCreateInstance");
    if (create_instance(&ici, NULL, &h->inst) != VK_SUCCESS) {
        return false;
    }
    h->enumerate_devices = vk_sym(h, "vkEnumeratePhysicalDevices");
    h->get_props2 = vk_sym(h, "vkGetPhysicalDeviceProperties2");
    h->get_external = vk_sym(h, "vkGetPhysicalDeviceExternalBufferProperties");
    h->enumerate_ext = vk_sym(h, "vkEnumerateDeviceExtensionProperties");
    h->create_device = vk_sym(h, "vkCreateDevice");
    h->get_device_proc = vk_sym(h, "vkGetDeviceProcAddr");
    if (h->enumerate_devices(h->inst, &n, all) < 0) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties id = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id,
        };
        VkPhysicalDeviceExternalBufferInfo ebi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .handleType = SGI_VK_HANDLE_TYPE,
        };
        VkExternalBufferProperties ebp = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
        };
        VkFlags need = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                       VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
        uint32_t next = 0;
        g_autofree VkExtensionProperties *ext = NULL;
        bool have_ext = false;

        h->get_props2(all[i], &p2);
        h->get_external(all[i], &ebi, &ebp);
        if ((ebp.externalMemoryFeatures & need) != need) {
            continue;
        }
        h->enumerate_ext(all[i], NULL, &next, NULL);
        ext = g_new0(VkExtensionProperties, next ? next : 1);
        h->enumerate_ext(all[i], NULL, &next, ext);
        for (uint32_t j = 0; j < next; j++) {
            have_ext |= !strcmp(ext[j].extensionName, SGI_VK_EXTENSION);
        }
        if (!have_ext || h->npd == G_N_ELEMENTS(h->pd)) {
            continue;
        }
        h->pd[h->npd] = all[i];
        memcpy(h->uuid[h->npd], id.deviceUUID, 16);
        pstrcpy(h->name[h->npd], sizeof(h->name[0]),
                (const char *)p2.properties + SGI_VK_PROPS_DEVICE_NAME_OFFSET);
        h->npd++;
    }
    return true;
}

static int vk_helper_list(void)
{
    VkHelper h;

    if (!vk_helper_open(&h)) {
        return 0;
    }
    for (uint32_t i = 0; i < h.npd; i++) {
        for (int j = 0; j < 16; j++) {
            printf("%02x", h.uuid[i][j]);
        }
        printf("\t%s\n", h.name[i]);
    }
    return 0;
}

static bool parse_uuid(const char *hex, uint8_t uuid[16])
{
    if (strlen(hex) < 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        int hi = g_ascii_xdigit_value(hex[2 * i]);
        int lo = g_ascii_xdigit_value(hex[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        uuid[i] = hi << 4 | lo;
    }
    return true;
}

static int vk_helper_import(char **args)
{
    VkHelper h;
    uint8_t want[16];
    uint32_t dev_index = UINT32_MAX;
    int fd = atoi(args[1]);
    unsigned long memory_type;
    uint64_t alloc_size, size;
    bool dedicated = atoi(args[4]);
    const char *exts[] = { SGI_VK_EXTENSION };
    const float prio = 1.0f;
    /* Queue family 0: every driver has one, and nothing is submitted. */
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts,
    };
    VkExternalMemoryBufferCreateInfo ebci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = SGI_VK_HANDLE_TYPE,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebci,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkImportMemoryFdInfoKHR imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = SGI_VK_HANDLE_TYPE, .fd = fd,
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
    };
    VkMemoryRequirements req;
    VkDevice dev;
    VkBuffer buf;
    VkDeviceMemory mem;
    void *ptr;
    char line[256];
    PFN_vkCreateBuffer create_buffer;
    PFN_vkGetBufferMemoryRequirements get_req;
    PFN_vkAllocateMemory allocate;
    PFN_vkBindBufferMemory bind;
    PFN_vkMapMemory map;

    g_assert_true(parse_uuid(args[0], want));
    g_assert_cmpint(qemu_strtoul(args[2], NULL, 0, &memory_type), ==, 0);
    g_assert_cmpint(qemu_strtou64(args[3], NULL, 0, &alloc_size), ==, 0);
    g_assert_cmpint(qemu_strtou64(args[5], NULL, 0, &size), ==, 0);
    bci.size = size;
    mai.allocationSize = alloc_size;
    mai.memoryTypeIndex = memory_type;
    g_assert_true(vk_helper_open(&h));
    for (uint32_t i = 0; i < h.npd; i++) {
        if (!memcmp(h.uuid[i], want, 16)) {
            dev_index = i;
        }
    }
    g_assert_cmpuint(dev_index, !=, UINT32_MAX);
    g_assert_cmpint(h.create_device(h.pd[dev_index], &dci, NULL, &dev), ==,
                    VK_SUCCESS);
    create_buffer = (void *)h.get_device_proc(dev, "vkCreateBuffer");
    get_req = (void *)h.get_device_proc(dev, "vkGetBufferMemoryRequirements");
    allocate = (void *)h.get_device_proc(dev, "vkAllocateMemory");
    bind = (void *)h.get_device_proc(dev, "vkBindBufferMemory");
    map = (void *)h.get_device_proc(dev, "vkMapMemory");

    g_assert_cmpint(create_buffer(dev, &bci, NULL, &buf), ==, VK_SUCCESS);
    get_req(dev, buf, &req);
    /* The exporter's memory type must be one this buffer may live in. */
    g_assert_cmphex(req.memoryTypeBits & (1u << memory_type), !=, 0);
    if (dedicated) {
        ded.buffer = buf;
        imp.pNext = &ded;
    }
    g_assert_cmpint(allocate(dev, &mai, NULL, &mem), ==, VK_SUCCESS);
    g_assert_cmpint(bind(dev, buf, mem, 0), ==, VK_SUCCESS);
    g_assert_cmpint(map(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr), ==, VK_SUCCESS);
    printf("imported\n");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin)) {
        char sha[65];
        unsigned w, ht, stride;
        gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        g_autofree char *seen = NULL;

        if (sscanf(line, "frame %64s %u %u %u", sha, &w, &ht, &stride) != 4) {
            printf("bad command\n");
            fflush(stdout);
            continue;
        }
        for (;;) {
            g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);

            for (unsigned y = 0; y < ht; y++) {
                const uint32_t *row = (const uint32_t *)
                    ((const uint8_t *)ptr + (size_t)y * stride);
                uint32_t px[8192];

                g_assert_cmpuint(w, <=, G_N_ELEMENTS(px));
                /* Another process may be writing it: read each word once. */
                for (unsigned x = 0; x < w; x++) {
                    px[x] = GUINT32_TO_LE(qatomic_read((uint32_t *)&row[x]) &
                                          0xffffff);
                }
                g_checksum_update(sum, (const guchar *)px, w * 4);
            }
            g_free(seen);
            seen = g_strdup(g_checksum_get_string(sum));
            if (!strcmp(seen, sha) || g_get_monotonic_time() > end) {
                break;
            }
            g_usleep(10000);
        }
        printf(!strcmp(seen, sha) ? "ok\n" : "bad %s\n", seen);
        fflush(stdout);
    }
    return 0;
}

static int vk_helper_main(char **argv, int argc)
{
    if (argc >= 3 && !strcmp(argv[2], "list")) {
        return vk_helper_list();
    }
    if (argc == 9 && !strcmp(argv[2], "import")) {
        return vk_helper_import(argv + 3);
    }
    fprintf(stderr, "usage: --vk-helper list | import UUID FD MEMORY_TYPE "
            "ALLOC_SIZE DEDICATED SIZE\n");
    return 2;
}

/* ---- The test, the QEMU side ------------------------------------------ */

static char *self_path;

/*
 * Run the helper and return what it printed. A helper that cannot run or
 * dies, a driver crash while listing devices say, fails the test: only a
 * clean run with nothing printed means there is no device.
 */
static char *helper_output(const char *arg1)
{
    char *argv[] = { self_path, (char *)"--vk-helper", (char *)arg1, NULL };
    char *out = NULL;
    GError *err = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                               &out, NULL, &status, &err));
    g_assert_no_error(err);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    return out;
}

/* The frame as the reader will hash it: 0x00RRGGBB words, little endian. */
static char *frame_sha(const uint32_t *px, size_t n)
{
    g_autofree uint32_t *le = g_new(uint32_t, n);

    for (size_t i = 0; i < n; i++) {
        le[i] = GUINT32_TO_LE(px[i]);
    }
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)le,
                                       n * 4);
}

/* Ask the helper whether the imported frame now hashes to what QEMU shows. */
static void expect_frame(FILE *to, FILE *from, QTestState *qts)
{
    char reply[128];
    uint32_t w, ht;
    g_autofree uint32_t *dump = screendump(qts, &w, &ht);
    g_autofree char *sha = frame_sha(dump, (size_t)w * ht);

    g_assert_cmpint(fprintf(to, "frame %s %u %u %u\n", sha, w, ht, w * 4), >,
                    0);
    fflush(to);
    g_assert_nonnull(fgets(reply, sizeof(reply), from));
    g_assert_cmpstr(reply, ==, "ok\n");
}

static char *screendump_sha(QTestState *qts)
{
    uint32_t w, ht;
    g_autofree uint32_t *dump = screendump(qts, &w, &ht);

    return frame_sha(dump, (size_t)w * ht);
}

/*
 * Make pixel (x, y) of the VPro's frame show colour. The head resolves a
 * colour index through the lookup RAM, which starts all zero, so the frame
 * is black until an entry is written: the pixel holds index, and the RAM
 * entry index holds colour. Waits until the console shows a frame that
 * differs from old_sha, so a reader has something to differ on.
 */
static void show_pixel(QTestState *qts, unsigned x, unsigned y,
                       uint32_t index, uint32_t colour, const char *old_sha)
{
    char *sha = NULL;

    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", VPRO_PATH, x);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", VPRO_PATH, y);
    qtest_qmp_assert_success(qts,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel', 'value': %u}}", VPRO_PATH, index);
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT,
                 ODSY_BACKEND_HDR(index, 1));
    qtest_writel(qts, VPRO_SHORT + ODSY_BACKEND_SELECT, colour);
    /* The console shows it within a few frames. */
    for (int i = 0; i < 100; i++) {
        sha = screendump_sha(qts);
        if (strcmp(sha, old_sha)) {
            break;
        }
        g_free(sha);
        sha = NULL;
        g_usleep(50000);
    }
    g_assert_nonnull(sha);
    g_free(sha);
}

/*
 * One adapter: QEMU exports the frame as an opaque handle, a second process
 * imports it into its own device, and reads what the VPro model wrote, then
 * what it wrote after the import.
 */
static void gpu_export_one(const char *uuid_hex)
{
    g_autofree char *extra = g_strdup_printf(",display-shm-gpu=%s", uuid_hex);
    QTestState *qts = start_with(extra);
    uint8_t uuid[16];
    Session s;
    Region *frame, *cursor;
    uint32_t frame_id, cursor_id;
    const uint8_t *hd;
    char *argv[10];
    g_autofree char *fd_arg = NULL, *type_arg = NULL, *alloc_arg = NULL;
    g_autofree char *size_arg = NULL, *before = NULL, *blank = NULL;
    GPid pid;
    int in_fd, out_fd, status;
    FILE *to, *from;
    char reply[128];
    GError *err = NULL;

    g_test_message("exporting from %s", uuid_hex);
    g_assert_true(parse_uuid(uuid_hex, uuid));
    blank = screendump_sha(qts);
    show_pixel(qts, 5, 100, 0x123, 0x3ff00ff0, blank);
    session_open(&s, qts);

    /* WELCOME names the backend and the adapter. */
    g_assert_cmpuint(ldl_le_p(s.welcome.b + 12), ==, SGI_SHM_BACKEND_VK_FD);
    g_assert_cmpuint(ldl_le_p(s.welcome.b + 20), ==, 3);    /* control */
    g_assert_cmpmem(s.welcome.b + 24, 16, uuid, 16);
    g_assert_false(zero_range(s.welcome.b + 40, 16));       /* driver_uuid */

    hd = head_raw(&s, 0);
    frame_id = ldl_le_p(hd + 80);
    cursor_id = ldl_le_p(hd + 400);
    frame = find_region(&s, frame_id);
    cursor = find_region(&s, cursor_id);
    g_assert_nonnull(frame);
    g_assert_nonnull(cursor);
    g_assert_cmpuint(s.nregions, ==, 2);
    /* The frame is exported; the cursor image, not scanout, is not. */
    g_assert_cmpuint(frame->backend, ==, SGI_SHM_BACKEND_VK_FD);
    g_assert_cmpuint(cursor->backend, ==, SGI_SHM_BACKEND_SHM_FD);
    g_assert_cmpuint(frame->size, ==, 5 * 1024 * 1024);
    g_assert_cmpuint(frame->size % 65536, ==, 0);
    g_assert_cmpuint(frame->alloc_size, >=, frame->size);
    g_assert_cmpuint(frame->memory_type, <, 32);
    g_assert_cmpuint(frame->flags & ~SGI_SHM_REGION_DEDICATED, ==, 0);
    g_assert_cmpint(frame->fd, >=, 0);

    fd_arg = g_strdup_printf("%d", frame->fd);
    type_arg = g_strdup_printf("%u", frame->memory_type);
    alloc_arg = g_strdup_printf("%" PRIu64, frame->alloc_size);
    size_arg = g_strdup_printf("%" PRIu64, frame->size);
    argv[0] = self_path;
    argv[1] = (char *)"--vk-helper";
    argv[2] = (char *)"import";
    argv[3] = (char *)uuid_hex;
    argv[4] = fd_arg;
    argv[5] = type_arg;
    argv[6] = alloc_arg;
    argv[7] = (char *)(frame->flags & SGI_SHM_REGION_DEDICATED ? "1" : "0");
    argv[8] = size_arg;
    argv[9] = NULL;
    g_spawn_async_with_pipes(NULL, argv, NULL,
        G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_DO_NOT_REAP_CHILD, NULL,
        NULL, &pid, &in_fd, &out_fd, NULL, &err);
    g_assert_no_error(err);
    to = fdopen(in_fd, "w");
    from = fdopen(out_fd, "r");
    g_assert_nonnull(fgets(reply, sizeof(reply), from));
    g_assert_cmpstr(reply, ==, "imported\n");

    /* What the model drew before the import, then after it. */
    before = screendump_sha(qts);
    expect_frame(to, from, qts);
    show_pixel(qts, 9, 200, 0x124, 0x2a55aa55, before);
    expect_frame(to, from, qts);

    fclose(to);
    fclose(from);
    g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    g_spawn_close_pid(pid);
    session_close(&s);
    qtest_quit(qts);
}

static void test_gpu_export(void)
{
    g_autofree char *list = helper_output("list");
    g_auto(GStrv) lines = NULL;
    const char *only = g_getenv("SGI_DISPLAY_VK_TEST_UUID");
    unsigned ran = 0;

    if (!list || !*list) {
        qtest_prereq_missing("VULKAN_EXPORT", "no Vulkan device can export "
                             "host-visible memory");
        return;
    }
    lines = g_strsplit(list, "\n", -1);
    for (int i = 0; lines[i] && *lines[i]; i++) {
        g_autofree char *uuid = g_strndup(lines[i], 32);

        if (only && *only && strcmp(only, uuid)) {
            continue;
        }
        g_test_message("device %s", lines[i]);
        gpu_export_one(uuid);
        ran++;
    }
    if (!ran) {
        qtest_prereq_missing("VULKAN_EXPORT", "SGI_DISPLAY_VK_TEST_UUID "
                             "names no device");
    }
}

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, ret;

    if (argc > 1 && !strcmp(argv[1], "--vk-helper")) {
        return vk_helper_main(argv, argc);
    }
    self_path = realpath(argv[0], NULL);
    g_assert_nonnull(self_path);
    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("sgi-display-shm-prom-XXXXXX", &prom_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, erased, sizeof(erased)), ==, sizeof(erased));
    close(fd);
    sock_dir = g_dir_make_tmp("sgi-display-shm-XXXXXX", NULL);
    g_assert(sock_dir);
    sock_path = g_build_filename(sock_dir, "d.sock", NULL);

    qtest_add_func("sgi-display-shm/layout", test_layout);
    qtest_add_func("sgi-display-shm/region-message", test_region_message);
    qtest_add_func("sgi-display-shm/dirty", test_dirty);
    qtest_add_func("sgi-display-shm/cursor-wake", test_cursor_wake);
    qtest_add_func("sgi-display-shm/vpro-cursor-position",
                   test_vpro_cursor_position);
    qtest_add_func("sgi-display-shm/impact-cursor-position",
                   test_impact_cursor_position);
    qtest_add_func("sgi-display-shm/gbe-cursor-position",
                   test_gbe_cursor_position);
    qtest_add_func("sgi-display-shm/resize", test_resize);
    qtest_add_func("sgi-display-shm/reconnect", test_reconnect);
    qtest_add_func("sgi-display-shm/reconnect-after-resize",
                   test_reconnect_after_resize);
    qtest_add_func("sgi-display-shm/protocol-errors", test_protocol_errors);
    qtest_add_func("sgi-display-shm/fallback", test_fallback);
    qtest_add_func("sgi-display-shm/fallback-no-loader",
                   test_fallback_no_loader);
    qtest_add_func("sgi-display-shm/gpu-export", test_gpu_export);
    qtest_add_func("sgi-display-shm/properties",
                   test_properties_on_every_machine);
    qtest_add_func("sgi-display-shm/input-key", test_input_key);
    qtest_add_func("sgi-display-shm/input-release-on-drop",
                   test_input_release_on_drop);
    qtest_add_func("sgi-display-shm/input-mouse", test_input_mouse);
    qtest_add_func("sgi-display-shm/input-absolute", test_input_absolute);
    qtest_add_func("sgi-display-shm/input-absolute-loop",
                   test_input_absolute_loop);
    qtest_add_func("sgi-display-shm/input-absolute-accelerating",
                   test_input_absolute_accelerating);
    qtest_add_func("sgi-display-shm/input-absolute-steep",
                   test_input_absolute_steep);
    qtest_add_func("sgi-display-shm/input-absolute-lagging",
                   test_input_absolute_lagging);
    qtest_add_func("sgi-display-shm/input-absolute-edge",
                   test_input_absolute_edge);
    qtest_add_func("sgi-display-shm/input-absolute-retarget",
                   test_input_absolute_retarget);
    qtest_add_func("sgi-display-shm/input-absolute-lead",
                   test_input_absolute_lead);
    qtest_add_func("sgi-display-shm/cleanup-on-exit", test_cleanup_on_exit);
    ret = g_test_run();

    unlink(prom_path);
    rmdir(sock_dir);
    free(self_path);
    return ret;
}
