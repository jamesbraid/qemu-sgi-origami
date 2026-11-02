/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1 hub: a stand-in for the L2's USB host controller and hubs.
 *
 * The L2's own programs run under qemu-ppc with the sgi-l2-shim plugin,
 * which answers their opens and ioctls on the L1 device nodes. This process
 * is the other end of those nodes. It accepts one stream connection per L1
 * model (the model's "l2" chardev), gives each the lowest free slot, and
 * moves router segments between that connection and the slot's node:
 * one segment per message on the node side, kind byte plus segment on the
 * L1 side. Everything that crosses is logged with a timestamp.
 *
 * The slot's USB location is synthetic: a topology-aware assignment waits
 * for the rack topology (the real locations are not established).
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>
#include "hw/misc/sgi-l1-l2-link.h"
#include "hw/misc/sgi-l2-hub.h"

#define MAX_QUEUED_SEGMENTS 256
#define MAX_L1_TX_BYTES     (1 << 20)

typedef enum {
    CONN_L1,        /* an L1 model's link */
    CONN_CONTROL,   /* a program's open of the control node */
    CONN_SLOT,      /* a program's open of a slot node */
    CONN_PENDING,   /* accepted, request not read yet */
} ConnType;

typedef struct Conn {
    ConnType type;
    int fd;
    int slot;
    GByteArray *rx;     /* stream bytes from an L1 */
    GByteArray *tx;     /* bytes not yet written to an L1 */
} Conn;

typedef struct Slot {
    Conn *l1;           /* NULL when no L1 is attached */
    Conn *node;         /* the open node, or NULL */
    GQueue pending;     /* segments from the L1 before the node opened */
    uint8_t record[SGI_L2_HUB_RECORD_LEN];
} Slot;

static Slot slots[SGI_L2_HUB_SLOTS];
static GPtrArray *conns;
static FILE *logfile;
static bool stop;

static void hub_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void hub_log(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(logfile, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min,
            tm.tm_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fputc('\n', logfile);
    fflush(logfile);
}

static char *hex(const uint8_t *data, size_t len)
{
    GString *s = g_string_new(NULL);

    for (size_t i = 0; i < len; i++) {
        g_string_append_printf(s, "%02x", data[i]);
    }
    return g_string_free(s, false);
}

static void hub_log_segment(const char *dir, int slot, const uint8_t *seg,
                            size_t len)
{
    g_autofree char *bytes = hex(seg, len);

    hub_log("segment %s slot %d len %zu %s", dir, slot, len, bytes);
}

static int make_listener(const char *dir, const char *name, int type)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    g_autofree char *path = g_build_filename(dir, name, NULL);
    int fd;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "qemu-sgi-l2-hub: path too long: %s\n", path);
        exit(1);
    }
    strcpy(addr.sun_path, path);
    unlink(path);
    fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
        listen(fd, 16)) {
        fprintf(stderr, "qemu-sgi-l2-hub: %s: %s\n", path, strerror(errno));
        exit(1);
    }
    return fd;
}

static Conn *conn_new(ConnType type, int fd)
{
    Conn *c = g_new0(Conn, 1);

    c->type = type;
    c->fd = fd;
    c->slot = -1;
    if (type == CONN_L1) {
        c->rx = g_byte_array_new();
        c->tx = g_byte_array_new();
    }
    g_ptr_array_add(conns, c);
    return c;
}

/* The synthetic USB location: bus 1, root hub port 1, hub port slot + 1. */
static void make_record(int slot, uint8_t *record)
{
    record[0] = 1;
    record[1] = 2;
    record[2] = slot + 2;
    record[3] = 1;
    record[4] = slot + 1;
    memset(record + 5, 0, SGI_L2_HUB_RECORD_LEN - 5);
}

static void send_reply(int fd, int err, const uint8_t *record)
{
    uint8_t reply[SGI_L2_HUB_REPLY_LEN] = { err };

    if (record) {
        memcpy(reply + 1, record, SGI_L2_HUB_RECORD_LEN);
    }
    if (send(fd, reply, sizeof(reply), MSG_NOSIGNAL) < 0) {
        hub_log("reply failed: %s", strerror(errno));
    }
}

static void broadcast_map(void)
{
    uint8_t map[SGI_L2_HUB_SLOTS];
    g_autofree char *bytes = NULL;

    for (int i = 0; i < SGI_L2_HUB_SLOTS; i++) {
        map[i] = slots[i].l1 != NULL;
    }
    bytes = hex(map, sizeof(map));
    hub_log("slot map %s", bytes);
    for (guint i = 0; i < conns->len; i++) {
        Conn *c = g_ptr_array_index(conns, i);

        if (c->type == CONN_CONTROL) {
            send(c->fd, map, sizeof(map), MSG_NOSIGNAL);
        }
    }
}

static void conn_free(Conn *c)
{
    g_ptr_array_remove(conns, c);
    close(c->fd);
    if (c->rx) {
        g_byte_array_free(c->rx, true);
        g_byte_array_free(c->tx, true);
    }
    g_free(c);
}

static void node_closed(Conn *c)
{
    Slot *s = &slots[c->slot];

    s->node = NULL;
    hub_log("node close slot %d (the program resets the device)", c->slot);
    conn_free(c);
}

static void l1_detach(Conn *c)
{
    Slot *s = &slots[c->slot];
    uint8_t *seg;

    hub_log("detach slot %d", c->slot);
    s->l1 = NULL;
    while ((seg = g_queue_pop_head(&s->pending))) {
        g_free(seg);
    }
    if (s->node) {
        /* The program sees a read of 0 and, soon, the new slot map. */
        shutdown(s->node->fd, SHUT_RDWR);
    }
    conn_free(c);
    broadcast_map();
}

/* Send queued segments to an open node until it would block. */
static void flush_node(Slot *s, int slot)
{
    uint8_t *seg;

    while (s->node && (seg = g_queue_peek_head(&s->pending))) {
        if (send(s->node->fd, seg, lduw_be_p(seg),
                 MSG_DONTWAIT | MSG_NOSIGNAL) < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                hub_log("deliver slot %d failed: %s, segment dropped", slot,
                        strerror(errno));
                g_free(g_queue_pop_head(&s->pending));
                continue;
            }
            return;
        }
        g_free(g_queue_pop_head(&s->pending));
    }
}

/*
 * Segments for a slot wait in order until its node is open and has room, so
 * a program that reads slowly delays only its own slot. The length is the
 * segment's own first two bytes.
 */
static void deliver_to_node(Slot *s, int slot, const uint8_t *seg, size_t len)
{
    if (g_queue_get_length(&s->pending) >= MAX_QUEUED_SEGMENTS) {
        hub_log("slot %d: queue full, segment dropped", slot);
        return;
    }
    if (!s->node) {
        hub_log("slot %d: node not open, segment queued", slot);
    }
    g_queue_push_tail(&s->pending, g_memdup2(seg, len));
    flush_node(s, slot);
}

/* Bytes for an L1 wait in its link's buffer, so a stalled L1 delays itself. */
static void l1_flush(Conn *c)
{
    while (c->tx->len) {
        ssize_t n = write(c->fd, c->tx->data, c->tx->len);

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                hub_log("slot %d: write to the L1 failed: %s", c->slot,
                        strerror(errno));
                g_byte_array_set_size(c->tx, 0);
            }
            return;
        }
        g_byte_array_remove_range(c->tx, 0, n);
    }
}

static void l1_send(Conn *c, const uint8_t *seg, size_t len)
{
    uint8_t kind = SGI_L1_L2_KIND_SEGMENT;

    if (c->tx->len + 1 + len > MAX_L1_TX_BYTES) {
        hub_log("slot %d: the L1 is not reading, segment dropped", c->slot);
        return;
    }
    g_byte_array_append(c->tx, &kind, 1);
    g_byte_array_append(c->tx, seg, len);
    l1_flush(c);
}

static void handle_l1_data(Conn *c)
{
    uint8_t buf[2048];
    ssize_t n = read(c->fd, buf, sizeof(buf));
    Slot *s = &slots[c->slot];

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (n <= 0) {
        l1_detach(c);
        return;
    }
    g_byte_array_append(c->rx, buf, n);
    while (c->rx->len >= 3) {
        uint8_t kind = c->rx->data[0];
        size_t len = lduw_be_p(c->rx->data + 1);

        if (kind != SGI_L1_L2_KIND_SEGMENT || len < SGI_L1_L2_SEGMENT_MIN ||
            len > SGI_L1_L2_SEGMENT_MAX) {
            hub_log("slot %d: bad link message kind %u length %zu, closing",
                    c->slot, kind, len);
            l1_detach(c);
            return;
        }
        if (c->rx->len < 1 + len) {
            return;
        }
        hub_log_segment("L1->L2", c->slot, c->rx->data + 1, len);
        deliver_to_node(s, c->slot, c->rx->data + 1, len);
        g_byte_array_remove_range(c->rx, 0, 1 + len);
    }
}

static void handle_node_data(Conn *c)
{
    uint8_t buf[SGI_L2_HUB_SEGMENT_MAX + 1];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    Slot *s = &slots[c->slot];

    if (n <= 0) {
        node_closed(c);
        return;
    }
    if (n > SGI_L2_HUB_SEGMENT_MAX) {
        hub_log("slot %d: oversized write, dropped", c->slot);
        return;
    }
    if (n < SGI_L1_L2_SEGMENT_MIN) {
        /* Too short to hold the length field that is stamped next. */
        hub_log("slot %d: short write, dropped", c->slot);
        return;
    }
    if (lduw_be_p(buf) != n) {
        /* The kernel driver stamps the transfer length over bytes 0-1. */
        hub_log("slot %d: length field %u stamped to %zd", c->slot,
                lduw_be_p(buf), n);
        stw_be_p(buf, n);
    }
    hub_log_segment("L2->L1", c->slot, buf, n);
    if (!s->l1) {
        hub_log("slot %d: no L1 attached, segment dropped", c->slot);
        return;
    }
    if (n > SGI_L1_L2_SEGMENT_MAX) {
        hub_log("slot %d: segment above the link maximum, dropped", c->slot);
        return;
    }
    l1_send(s->l1, buf, n);
}

static void handle_request(Conn *c)
{
    uint8_t req[SGI_L2_HUB_REQUEST_LEN + 1];
    ssize_t n = recv(c->fd, req, sizeof(req), 0);
    int slot;

    if (n != SGI_L2_HUB_REQUEST_LEN) {
        if (n > 0) {
            hub_log("request of %zd bytes, closing", n);
        }
        conn_free(c);
        return;
    }
    slot = req[1];
    switch (req[0]) {
    case SGI_L2_HUB_OP_CONTROL:
        hub_log("open control node");
        c->type = CONN_CONTROL;
        send_reply(c->fd, 0, NULL);
        broadcast_map();
        return;
    case SGI_L2_HUB_OP_OPEN:
    case SGI_L2_HUB_OP_QUERY:
        if (slot >= SGI_L2_HUB_SLOTS || !slots[slot].l1) {
            hub_log("%s slot %d: no device",
                    req[0] == SGI_L2_HUB_OP_OPEN ? "open" : "query", slot);
            send_reply(c->fd, slot >= SGI_L2_HUB_SLOTS ? EINVAL : ENODEV,
                       NULL);
            conn_free(c);
            return;
        }
        if (req[0] == SGI_L2_HUB_OP_QUERY) {
            send_reply(c->fd, 0, slots[slot].record);
            conn_free(c);
            return;
        }
        if (slots[slot].node) {
            hub_log("open slot %d: busy", slot);
            send_reply(c->fd, EBUSY, NULL);
            conn_free(c);
            return;
        }
        hub_log("open slot %d", slot);
        c->type = CONN_SLOT;
        c->slot = slot;
        slots[slot].node = c;
        send_reply(c->fd, 0, slots[slot].record);
        flush_node(&slots[slot], slot);
        return;
    case SGI_L2_HUB_OP_RESET:
        hub_log("reset slot %d ioctl 0x53%02x", slot, req[2]);
        send_reply(c->fd, slot < SGI_L2_HUB_SLOTS && slots[slot].l1 ? 0 :
                   ENODEV, NULL);
        conn_free(c);
        return;
    default:
        hub_log("unknown request op 0x%02x, closing", req[0]);
        conn_free(c);
    }
}

static void accept_l1(int listener)
{
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    Conn *c;
    int slot;

    if (fd < 0) {
        return;
    }
    for (slot = 0; slot < SGI_L2_HUB_SLOTS && slots[slot].l1; slot++) {
    }
    if (slot == SGI_L2_HUB_SLOTS) {
        hub_log("L1 link refused: all %d slots in use", SGI_L2_HUB_SLOTS);
        close(fd);
        return;
    }
    c = conn_new(CONN_L1, fd);
    c->slot = slot;
    slots[slot].l1 = c;
    make_record(slot, slots[slot].record);
    hub_log("attach slot %d", slot);
    broadcast_map();
}

static void on_signal(int sig)
{
    stop = true;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *logpath = NULL;
    int l1_listener, dev_listener;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) {
            dir = argv[++i];
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logpath = argv[++i];
        } else {
            fprintf(stderr, "usage: %s --dir DIR [--log FILE]\n", argv[0]);
            return 2;
        }
    }
    if (!dir) {
        fprintf(stderr, "usage: %s --dir DIR [--log FILE]\n", argv[0]);
        return 2;
    }
    logfile = logpath ? fopen(logpath, "a") : stderr;
    if (!logfile) {
        fprintf(stderr, "qemu-sgi-l2-hub: %s: %s\n", logpath, strerror(errno));
        return 1;
    }
    sigaction(SIGPIPE, &(struct sigaction){ .sa_handler = SIG_IGN }, NULL);
    sigaction(SIGTERM, &(struct sigaction){ .sa_handler = on_signal }, NULL);
    sigaction(SIGINT, &(struct sigaction){ .sa_handler = on_signal }, NULL);
    conns = g_ptr_array_new();
    l1_listener = make_listener(dir, SGI_L2_HUB_L1_SOCKET, SOCK_STREAM);
    dev_listener = make_listener(dir, SGI_L2_HUB_DEV_SOCKET, SOCK_SEQPACKET);
    hub_log("ready in %s", dir);

    while (!stop) {
        g_autofree struct pollfd *fds = g_new(struct pollfd, conns->len + 2);
        g_autofree Conn **who = g_new(Conn *, conns->len);
        guint n = conns->len;

        fds[0] = (struct pollfd){ .fd = l1_listener, .events = POLLIN };
        fds[1] = (struct pollfd){ .fd = dev_listener, .events = POLLIN };
        for (guint i = 0; i < n; i++) {
            who[i] = g_ptr_array_index(conns, i);
            fds[i + 2] = (struct pollfd){ .fd = who[i]->fd, .events = POLLIN };
            if (who[i]->type == CONN_L1 && who[i]->tx->len) {
                fds[i + 2].events |= POLLOUT;
            } else if (who[i]->type == CONN_SLOT &&
                       !g_queue_is_empty(&slots[who[i]->slot].pending)) {
                fds[i + 2].events |= POLLOUT;
            }
        }
        if (poll(fds, n + 2, -1) < 0) {
            continue;
        }
        if (fds[0].revents) {
            accept_l1(l1_listener);
        }
        if (fds[1].revents) {
            int fd = accept4(dev_listener, NULL, NULL, SOCK_CLOEXEC);

            if (fd >= 0) {
                conn_new(CONN_PENDING, fd);
            }
        }
        for (guint i = 0; i < n; i++) {
            Conn *c = who[i];

            /* A handler can free other connections; check it is still live. */
            if (!fds[i + 2].revents || !g_ptr_array_find(conns, c, NULL)) {
                continue;
            }
            switch (c->type) {
            case CONN_L1:
                if (fds[i + 2].revents & POLLOUT) {
                    l1_flush(c);
                }
                if (fds[i + 2].revents & ~POLLOUT) {
                    handle_l1_data(c);
                }
                break;
            case CONN_SLOT:
                if (fds[i + 2].revents & POLLOUT) {
                    flush_node(&slots[c->slot], c->slot);
                }
                if (fds[i + 2].revents & ~POLLOUT) {
                    handle_node_data(c);
                }
                break;
            case CONN_PENDING:
                handle_request(c);
                break;
            case CONN_CONTROL:
                /* The program only reads it; any event here is a close. */
                {
                    uint8_t drop[64];
                    ssize_t r = recv(c->fd, drop, sizeof(drop), MSG_DONTWAIT);

                    if (r == 0 || (r < 0 && errno != EAGAIN)) {
                        hub_log("control node closed");
                        conn_free(c);
                    }
                }
                break;
            }
        }
    }
    hub_log("stopping");
    return 0;
}
