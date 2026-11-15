/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The shared-memory display of the SGI machines, read as an external viewer
 * would: map the segment, check its header against the contract in
 * docs/system/mips/sgi/display-shm.rst, take a frame through the generation
 * protocol, and compare it with the same console's QMP screendump and with
 * the frame message on the socket. POSIX hosts only: the Windows mapping
 * has no reader here.
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
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "reset-mode=0x1aa683"

static char *prom_path;

static char *shm_name;
static char *sock_dir;
static char *sock_path;

static QTestState *start(const char *machine)
{
    return qtest_initf("-M %s,display-shm=%s,display-shm-socket=%s -m 256M "
                       "-cpu %s -bios %s -S -display none -serial none "
                       "-monitor none", machine, shm_name, sock_path,
                       OCTANE2_CPU, prom_path);
}

/* Map the segment once QEMU has written its magic. */
static SGIShmHeader *map_segment(void)
{
    g_autofree char *name = g_strdup_printf("/%s", shm_name);
    gint64 end = g_get_monotonic_time() + WAIT_MS * 1000;

    for (;;) {
        int fd = shm_open(name, O_RDWR, 0);

        if (fd >= 0) {
            struct stat st;
            void *p;

            g_assert_cmpint(fstat(fd, &st), ==, 0);
            if (st.st_size >= SGI_SHM_HEADER_BYTES) {
                p = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
                close(fd);
                g_assert(p != MAP_FAILED);
                if (qatomic_read(&((SGIShmHeader *)p)->magic) ==
                    SGI_SHM_MAGIC) {
                    return p;
                }
                munmap(p, st.st_size);
            } else {
                close(fd);
            }
        }
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(20000);
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

static void read_msg(int fd, SGIShmMsg *m)
{
    size_t got = 0;

    while (got < sizeof(*m)) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        ssize_t n;

        g_assert_cmpint(poll(&p, 1, WAIT_MS), ==, 1);
        n = read(fd, (char *)m + got, sizeof(*m) - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
}

/* A copy of the newest frame, taken with the reader's algorithm. */
static uint8_t *take_frame(SGIShmHeader *hdr, unsigned h, uint32_t *w,
                           uint32_t *ht, uint64_t *seq)
{
    SGIShmHead *hh = &hdr->head[h];

    for (int tries = 0; tries < 1000; tries++) {
        unsigned s = qatomic_read(&hh->latest);
        uint32_t g0, g1;
        uint8_t *copy;

        g_assert_cmpuint(s, <, SGI_SHM_SLOTS);
        qatomic_set(&hh->reading, s + 1);
        /* The claim must be visible before the generation is read. */
        smp_mb();
        g0 = qatomic_read(&hh->slot[s].gen);
        if (g0 & 1) {
            continue;
        }
        *w = qatomic_read(&hh->slot[s].width);
        *ht = qatomic_read(&hh->slot[s].height);
        *seq = qatomic_read(&hh->slot[s].seq);
        g_assert_cmpuint(*w, >, 0);
        g_assert_cmpuint(*w, <=, SGI_SHM_MAX_DIM);
        g_assert_cmpuint(*ht, >, 0);
        g_assert_cmpuint(*ht, <=, SGI_SHM_MAX_DIM);
        copy = g_memdup2((uint8_t *)hdr + hdr->data_offset +
                         ((size_t)h * SGI_SHM_SLOTS + s) * hdr->slot_bytes,
                         (size_t)*w * *ht * 4);
        /* The copy must finish before the generation is read again. */
        smp_mb();
        g1 = qatomic_read(&hh->slot[s].gen);
        qatomic_set(&hh->reading, 0);
        if (g0 == g1) {
            return copy;
        }
        g_free(copy);
    }
    g_assert_not_reached();
}

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

static void test_octane2(void)
{
    SGIShmHeader *hdr;
    QTestState *qts;
    SGIShmMsg m;
    uint32_t w, ht, dw, dht;
    uint64_t seq;
    uint8_t *frame;
    uint32_t *dump;
    int fd;

    qts = start("octane2");
    hdr = map_segment();

    /* The page's offsets, written out rather than taken from the header. */
    {
        const uint8_t *raw = (const uint8_t *)hdr;

        g_assert_cmphex(ldl_le_p(raw + 0), ==, 0x48535347);
        g_assert_cmpmem(raw, 4, "GSSH", 4);
        g_assert_cmpuint(ldl_le_p(raw + 4), ==, 2);
        g_assert_cmpuint(ldl_le_p(raw + 8), ==, 1);
        g_assert_cmpuint(ldl_le_p(raw + 12), ==, 16 * 1024 * 1024);
        g_assert_cmpuint(ldq_le_p(raw + 16), ==, 4096);
        g_assert_cmpuint(ldq_le_p(raw + 24), ==, 4096 + 3 * 16 * 1024 * 1024);
        g_assert_cmpuint(ldl_le_p(raw + 64 + 0), ==, 1);    /* active */
    }
    g_assert_cmpuint(hdr->version, ==, SGI_SHM_VERSION);
    g_assert_cmpuint(hdr->nheads, ==, 1);
    g_assert_cmpuint(hdr->slot_bytes, ==, SGI_SHM_SLOT_BYTES);
    g_assert_cmpuint(hdr->data_offset, ==, SGI_SHM_HEADER_BYTES);
    g_assert_cmpuint(hdr->total_bytes, ==,
                     SGI_SHM_HEADER_BYTES +
                     (uint64_t)SGI_SHM_SLOTS * SGI_SHM_SLOT_BYTES);

    /* A reader connecting gets a full-frame message for each active head. */
    fd = connect_socket();
    read_msg(fd, &m);
    g_assert_cmpuint(m.type, ==, SGI_SHM_MSG_FRAME);
    g_assert_cmpuint(m.head, ==, 0);
    g_assert_cmpuint(m.flags & SGI_SHM_FLAG_FULL, ==, SGI_SHM_FLAG_FULL);
    g_assert_cmpuint(m.y0, ==, 0);
    g_assert_cmpuint(m.y1, ==, m.height);
    g_assert_cmpuint(m.reserved, ==, 0);
    g_assert_cmpuint(m.seq, >=, 1);
    {
        /* The message's offsets from the page, read from the raw bytes. */
        const uint8_t *raw = (const uint8_t *)&m;

        g_assert_cmpuint(ldl_le_p(raw + 0), ==, 1);
        g_assert_cmpuint(ldq_le_p(raw + 8), ==, m.seq);
        g_assert_cmpuint(ldl_le_p(raw + 20), ==, 0);
        g_assert_cmpuint(ldl_le_p(raw + 24), ==, m.height);
        g_assert_cmpuint(ldl_le_p(raw + 28), ==, m.width);
        g_assert_cmpuint(ldl_le_p(raw + 36) & 3, ==, 3);
        g_assert_cmpuint(sizeof(m), ==, 56);
    }

    g_assert_cmpuint(qatomic_read(&hdr->head[0].active), ==, 1);
    frame = take_frame(hdr, 0, &w, &ht, &seq);
    g_assert_cmpuint(w, ==, m.width);
    g_assert_cmpuint(ht, ==, m.height);
    g_assert_cmpuint(seq, >=, m.seq);

    /* The slot holds what the console shows. */
    dump = screendump(qts, &dw, &dht);
    g_assert_cmpuint(dw, ==, w);
    g_assert_cmpuint(dht, ==, ht);
    for (size_t i = 0; i < (size_t)w * ht; i++) {
        g_assert_cmphex(ldl_le_p(frame + i * 4) & 0xffffff, ==, dump[i]);
    }
    g_free(dump);
    g_free(frame);

    /* Unused fields stay zero. */
    for (unsigned i = 0; i < sizeof(hdr->reserved); i++) {
        g_assert_cmpuint(hdr->reserved[i], ==, 0);
    }
    for (unsigned i = hdr->nheads; i < SGI_SHM_MAX_HEADS; i++) {
        g_assert_cmpuint(hdr->head[i].active, ==, 0);
    }

    close(fd);
    munmap(hdr, hdr->total_bytes);
    qtest_quit(qts);
}

/* Every SGI machine type offers both properties, from the one shared helper. */
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
        bool shm = false, sock = false;
        QListEntry *e;

        g_assert(props);
        QLIST_FOREACH_ENTRY(props, e) {
            const char *name = qdict_get_str(qobject_to(QDict, e->value),
                                             "name");

            shm |= g_str_equal(name, "display-shm");
            sock |= g_str_equal(name, "display-shm-socket");
        }
        g_assert_true(shm);
        g_assert_true(sock);
        qobject_unref(resp);
    }
    qtest_quit(qts);
}

/*
 * A viewer's input goes over QMP input-send-event. Check that a key and a
 * relative mouse move with a button press reach the Octane2's IOC3 keyboard
 * and mouse receive registers while display-shm is publishing.
 */
#define BRIDGE          0x1f000000ULL
#define CFG2            (BRIDGE + 0x020000 + 2 * 0x1000)
#define IOC3            (BRIDGE + 0x600000)
#define KBD_RECEIVE     (IOC3 + 0x0a0)
#define AUX_RECEIVE     (IOC3 + 0x0a4)
#define AUX_TRANSMIT    (IOC3 + 0x0ac)
#define RX_VALID(p)     (1u << (29 + (p)))

static void send_input(QTestState *qts, const char *events)
{
    g_autofree char *cmd = g_strdup_printf(
        "{'execute': 'input-send-event', 'arguments': {'events': %s}}",
        events);
    QDict *resp;

    qtest_qmp_send_raw(qts, "%s", cmd);
    resp = qtest_qmp_receive(qts);
    g_assert(qdict_haskey(resp, "return"));
    qobject_unref(resp);
}

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

static void test_input(void)
{
    QTestState *qts = start("octane2");
    uint8_t buf[16];
    unsigned n;

    qtest_writel(qts, CFG2 + 0x10, 0x10000000);
    qtest_writel(qts, CFG2 + 0x04, 0x00000002);
    qtest_writel(qts, BRIDGE + 0x200 + 8 * 2 + 4, 0x1100);
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");

    /* Key "a" down: PS/2 set 2 make code 0x1c. */
    send_input(qts, "[{'type': 'key', 'data': {'down': true, 'key': "
                    "{'type': 'qcode', 'data': 'a'}}}]");
    n = drain(qts, KBD_RECEIVE, buf, sizeof(buf));
    g_assert_cmpuint(n, >=, 1);
    g_assert_cmphex(buf[0], ==, 0x1c);

    /* The mouse reports only after the guest enables streaming (0xf4). */
    qtest_writel(qts, AUX_TRANSMIT, 0xf4);
    n = drain(qts, AUX_RECEIVE, buf, sizeof(buf));
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmphex(buf[0], ==, 0xfa);

    /* x +5, y -3 (QEMU's y is down), left button: a three-byte packet. */
    send_input(qts, "[{'type': 'rel', 'data': {'axis': 'x', 'value': 5}},"
                    " {'type': 'rel', 'data': {'axis': 'y', 'value': 3}},"
                    " {'type': 'btn', 'data': {'down': true, "
                    "'button': 'left'}}]");
    n = drain(qts, AUX_RECEIVE, buf, sizeof(buf));
    g_assert_cmpuint(n, >=, 3);
    g_assert_cmphex(buf[0] & 0x09, ==, 0x09);   /* left button, always-1 */
    g_assert_cmphex(buf[1], ==, 5);
    g_assert_cmphex(buf[2], ==, 0xfd);          /* -3, PS/2 y is up */
    qtest_quit(qts);
}

/* A reader that reconnects gets every head repainted again. */
static void test_reconnect(void)
{
    QTestState *qts = start("octane2");
    SGIShmHeader *hdr = map_segment();
    SGIShmMsg m1, m2;
    int fd = connect_socket();

    read_msg(fd, &m1);
    close(fd);
    fd = connect_socket();
    read_msg(fd, &m2);
    g_assert_cmpuint(m2.type, ==, SGI_SHM_MSG_FRAME);
    g_assert_cmpuint(m2.flags & SGI_SHM_FLAG_FULL, ==, SGI_SHM_FLAG_FULL);
    g_assert_cmpuint(m2.width, ==, m1.width);
    g_assert_cmpuint(m2.height, ==, m1.height);
    g_assert_cmpuint(m2.seq, >, m1.seq);
    close(fd);
    munmap(hdr, hdr->total_bytes);
    qtest_quit(qts);
}

static void test_cleanup_on_exit(void)
{
    g_autofree char *name = g_strdup_printf("/%s", shm_name);
    QTestState *qts = start("octane2");
    SGIShmHeader *hdr = map_segment();

    munmap(hdr, hdr->total_bytes);
    qtest_quit(qts);
    /* QEMU removes the segment and the socket when it exits. */
    g_assert_cmpint(shm_open(name, O_RDWR, 0), ==, -1);
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
    shm_name = g_strdup_printf("sgi-dshm-%d", getpid());
    sock_dir = g_dir_make_tmp("sgi-display-shm-XXXXXX", NULL);
    g_assert(sock_dir);
    sock_path = g_build_filename(sock_dir, "d.sock", NULL);

    qtest_add_func("sgi-display-shm/octane2", test_octane2);
    qtest_add_func("sgi-display-shm/properties",
                   test_properties_on_every_machine);
    qtest_add_func("sgi-display-shm/input", test_input);
    qtest_add_func("sgi-display-shm/reconnect", test_reconnect);
    qtest_add_func("sgi-display-shm/cleanup-on-exit", test_cleanup_on_exit);
    ret = g_test_run();

    unlink(prom_path);
    rmdir(sock_dir);
    return ret;
}
