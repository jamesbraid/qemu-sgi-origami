/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The shared-memory display of the SGI machines, read as an external viewer
 * would: connect to the socket, say HELLO, take the control block and the
 * regions as passed handles, check them against the contract in
 * docs/system/mips/sgi/display-shm.rst at raw offsets, and send binary
 * input. None of it needs a GPU: every region is plain shared memory.
 * POSIX hosts only; the Windows handle passing has no reader here.
 */
#include "qemu/osdep.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "ui/sgi-display-shm.h"

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
    uint32_t id, kind, backend;
    uint64_t size;
    uint8_t *ptr;
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
        r->size = ldq_le_p(m.b + 24);
        r->fd = m.fd;
        r->ptr = map_fd(m.fd, r->size);
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
        munmap(s->regions[i].ptr, s->regions[i].size);
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
 * GPU memory is a separate backend this build lacks, so asking for it falls
 * back to shared memory and WELCOME says so. The property reads back.
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
    g_assert_cmpuint(ldl_le_p(s.welcome.b + 12), ==, 3);
    g_assert_cmpuint(ldl_le_p(s.welcome.b + 20), ==, 3);
    g_assert_true(zero_range(s.welcome.b + 24, 32));
    g_assert_cmpuint(s.nregions, ==, 2);
    for (unsigned i = 0; i < s.nregions; i++) {
        g_assert_cmpuint(s.regions[i].backend, ==, 3);
    }
    session_close(&s);
    qtest_quit(qts);
}

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

/*
 * The guest answers a step by moving its cursor; QEMU learns the gain from
 * the answer and sends the rest. The first step is 1:1 from the origin
 * (7, 9). The guest, accelerating, moves only to (3, 4), a gain of 3/7 and
 * 4/9, so the 4 and 5 still to go take steps of 9 and 11.
 */
static void test_input_absolute_loop(void)
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
    g_assert_cmphex(buf[2], ==, 0xf7);

    guest_cursor(qts, 3, 4, true);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 3);
    g_assert_cmphex(buf[1], ==, 9);
    g_assert_cmphex(buf[2], ==, 0xf5);              /* -11 */

    /* On target: the next answer ends the loop and sends nothing. */
    guest_cursor(qts, 7, 9, true);
    g_assert_cmpuint(drain(qts, AUX_RECEIVE, buf, sizeof(buf)), ==, 0);
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

int main(int argc, char **argv)
{
    const uint8_t erased[] = { 0xff, 0xff, 0xff, 0xff };
    int fd, ret;

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
    qtest_add_func("sgi-display-shm/resize", test_resize);
    qtest_add_func("sgi-display-shm/reconnect", test_reconnect);
    qtest_add_func("sgi-display-shm/reconnect-after-resize",
                   test_reconnect_after_resize);
    qtest_add_func("sgi-display-shm/protocol-errors", test_protocol_errors);
    qtest_add_func("sgi-display-shm/fallback", test_fallback);
    qtest_add_func("sgi-display-shm/properties",
                   test_properties_on_every_machine);
    qtest_add_func("sgi-display-shm/input-key", test_input_key);
    qtest_add_func("sgi-display-shm/input-release-on-drop",
                   test_input_release_on_drop);
    qtest_add_func("sgi-display-shm/input-mouse", test_input_mouse);
    qtest_add_func("sgi-display-shm/input-absolute", test_input_absolute);
    qtest_add_func("sgi-display-shm/input-absolute-loop",
                   test_input_absolute_loop);
    qtest_add_func("sgi-display-shm/cleanup-on-exit", test_cleanup_on_exit);
    ret = g_test_run();

    unlink(prom_path);
    rmdir(sock_dir);
    return ret;
}
