/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/misc/sgi-l1-sn2.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-char.h"
#include "qapi/qapi-types-char.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "monitor/monitor.h"
#include "net/net.h"
#include "qemu/audio.h"
#include "system/blockdev.h"
#include "system/cpus.h"
#include "system/rtc.h"
#include "system/runstate.h"

/* The device must not reach monitor, audio, net or block services. */
int monitor_fd_param(Monitor *mon, const char *fdname, Error **errp)
{
    g_assert_not_reached();
}

/* hw/core/qdev-properties-system.c defines every property kind together. */
AudioBackend *audio_be_by_name(const char *name, Error **errp)
{
    g_assert_not_reached();
}

const char *audio_be_get_id(AudioBackend *be)
{
    g_assert_not_reached();
}

int qemu_find_net_clients_except(const char *id, NetClientState **ncs,
                                 NetClientDriver type, int max)
{
    return 0;
}

void blockdev_auto_del(BlockBackend *blk)
{
    g_assert_not_reached();
}

/* The RTC clock reads 2026-11-17 12:34:56 UTC plus the L1's offset. */
void qemu_get_timedate(struct tm *tm, time_t offset)
{
    time_t t = 1794918896 + offset;

    gmtime_r(&t, tm);
}

/* Reset and shutdown requests, counted instead of acted on. */
static unsigned reset_requests, shutdown_requests;

void qemu_system_reset_request(ShutdownCause reason)
{
    reset_requests++;
}

void qemu_system_shutdown_request(ShutdownCause reason)
{
    shutdown_requests++;
}

/* Fixed vectors: no PPP address/control prefix, FCS complemented, low first. */
static const uint8_t identity[] = {
    0x7e, 0x02, 0x0f, 0xff, 0xf0, 0x06, 0x00, 0x03, 0x00,
    0x96, 0x92, 0x7e,
};
static const uint8_t typed[] = {
    0x7e, 0x1f, 0x0f, 0xff, 0xf0, 0x06, 0x00, 0x06, 0x03,
    0x00, 0x12, 0x34, 0x7d, 0x5e, 0x7d, 0x5d, 0x01, 0x68,
    0x69, 0x00, 0x83, 0x7d, 0x5e, 0x7d, 0x5d, 0x00, 0x6d,
    0x35, 0x7e,
};

typedef struct Fixture {
    DeviceState *dev;
    CharFrontend uart;
    GByteArray *rx;
    int room;
    bool limited;
    char *dir;
    char *path;
} Fixture;

static int can_read(void *opaque)
{
    Fixture *f = opaque;

    return f->room;
}

static void receive(void *opaque, const uint8_t *buf, int len)
{
    Fixture *f = opaque;

    g_byte_array_append(f->rx, buf, len);
    if (f->limited) {
        f->room = MAX(0, f->room - len);
    }
}

/* A small room stands for a 16550 receive FIFO the guest has not drained. */
static void set_room(Fixture *f, int room)
{
    f->room = room;
    f->limited = room < 4096;
}

static void start_log(Fixture *f)
{
    f->dir = g_dir_make_tmp("test-sgi-sn2-l1.XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->path = g_build_filename(f->dir, "log", NULL);
    qemu_set_log_filename_flags(f->path, LOG_UNIMP | LOG_GUEST_ERROR,
                                &error_abort);
}

static bool two_cpus;

static void setup_node(Fixture *f, bool placed)
{
    f->rx = g_byte_array_new();
    f->room = 4096;
    f->dev = qdev_new(TYPE_SGI_L1_SN2);
    if (two_cpus) {
        qdev_prop_set_uint32(f->dev, "cpus", 2);
    }
    if (placed) {
        qdev_prop_set_uint32(f->dev, "rack", 3);
        qdev_prop_set_uint32(f->dev, "bay", 2);
        qdev_prop_set_uint32(f->dev, "partition", 5);
    }
    qdev_realize(f->dev, NULL, &error_abort);
    device_cold_reset(f->dev);
    qemu_chr_fe_init(&f->uart, sgi_l1_sn2_link(SGI_L1_SN2(f->dev)),
                     &error_abort);
    qemu_chr_fe_set_handlers(&f->uart, can_read, receive, NULL, NULL,
                             f, NULL, true);
    g_assert_true(qemu_chr_fe_backend_open(&f->uart));
    start_log(f);
}

static void setup(Fixture *f, gconstpointer data)
{
    setup_node(f, false);
}

static void setup_placed(Fixture *f, gconstpointer data)
{
    setup_node(f, true);
}

static void setup_two_cpus(Fixture *f, gconstpointer data)
{
    two_cpus = true;
    setup_node(f, false);
    two_cpus = false;
}

static char *read_log(Fixture *f)
{
    char *text = NULL;

    qemu_set_log_filename_flags(NULL, 0, &error_abort);
    drain_call_rcu();
    g_assert_true(g_file_get_contents(f->path, &text, NULL, NULL));
    return text;
}

static void teardown(Fixture *f, gconstpointer data)
{
    qemu_set_log_filename_flags(NULL, 0, &error_abort);
    drain_call_rcu();
    /* Every reply must have been checked by its test. */
    g_assert_cmpuint(f->rx->len, ==, 0);
    g_byte_array_free(f->rx, true);
    qdev_unrealize(f->dev);
    g_assert_false(qemu_chr_fe_backend_open(&f->uart));
    qemu_chr_fe_deinit(&f->uart, false);
    object_unparent(OBJECT(f->dev));
    object_unref(OBJECT(f->dev));
    g_remove(f->path);
    g_rmdir(f->dir);
    g_free(f->path);
    g_free(f->dir);
}

static void send_bytes(Fixture *f, const uint8_t *bytes, unsigned len)
{
    g_assert_cmpint(qemu_chr_fe_write_all(&f->uart, bytes, len), ==, len);
}

static void expect_count(const char *text, const char *needle, unsigned count)
{
    unsigned n = 0;

    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) {
        n++;
    }
    g_assert_cmpuint(n, ==, count);
}

/* Test encoder uses the non-reflected polynomial and reverses each byte. */
static uint8_t reverse_byte(uint8_t v)
{
    v = (v >> 4) | (v << 4);
    v = ((v & 0xcc) >> 2) | ((v & 0x33) << 2);
    return ((v & 0xaa) >> 1) | ((v & 0x55) << 1);
}

static void append_escaped(GByteArray *wire, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        uint8_t escape = 0x7d;

        g_byte_array_append(wire, &escape, 1);
        byte ^= 0x20;
    }
    g_byte_array_append(wire, &byte, 1);
}

static GByteArray *encode(const uint8_t *payload, unsigned len)
{
    GByteArray *wire = g_byte_array_new();
    uint8_t flag = 0x7e;
    uint16_t crc = 0xffff;

    g_byte_array_append(wire, &flag, 1);
    for (unsigned i = 0; i < len; i++) {
        crc ^= reverse_byte(payload[i]) << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
        }
        append_escaped(wire, payload[i]);
    }
    append_escaped(wire, reverse_byte(crc >> 8) ^ 0xff);
    append_escaped(wire, reverse_byte(crc) ^ 0xff);
    g_byte_array_append(wire, &flag, 1);
    return wire;
}

static void send_packet(Fixture *f, const uint8_t *p, unsigned len)
{
    g_autoptr(GByteArray) wire = encode(p, len);

    send_bytes(f, wire->data, wire->len);
}

static void expect_wire(Fixture *f, const uint8_t *wire, unsigned len)
{
    g_assert_cmpuint(f->rx->len, >=, len);
    g_assert_cmpmem(f->rx->data, len, wire, len);
    g_byte_array_remove_range(f->rx, 0, len);
}

static void expect_reply(Fixture *f, const uint8_t *payload, unsigned len)
{
    g_autoptr(GByteArray) wire = encode(payload, len);

    expect_wire(f, wire->data, wire->len);
}

static void expect_status(Fixture *f, int32_t status)
{
    uint8_t reply[6] = { 0x25 };

    stl_be_p(reply + 1, status);
    expect_reply(f, reply, sizeof(reply));
}

static unsigned put_int(uint8_t *p, uint32_t value)
{
    p[0] = 0;
    stl_be_p(p + 1, value);
    return 5;
}

static unsigned put_string(uint8_t *p, const char *text)
{
    p[0] = 1;
    strcpy((char *)p + 1, text);
    return strlen(text) + 2;
}

/* A task-6 request as the PROM builds it: destination 0x0ffff006. */
static void send_request(Fixture *f, uint8_t index, uint16_t code,
                         unsigned count, const uint8_t *args, unsigned len)
{
    uint8_t p[130] = { index, 0x0f, 0xff, 0xf0, 0x06 };

    stw_be_p(p + 5, code);
    p[7] = count;
    memcpy(p + 8, args, len);
    send_packet(f, p, 8 + len);
}

/* The reply to the identity vector's request 0x0003 at index 2. */
static void expect_identity_reply(Fixture *f)
{
    static const uint8_t reply[] = { 0x22, 0, 0, 0, 0, 2,
                                     0, 0, 0, 0, 0x41, 0, 0, 0, 0, 'O' };

    expect_reply(f, reply, sizeof(reply));
}

/* Three arguments to an EEPROM read, which needs four: the L1 says -100. */
static void expect_typed_reply(Fixture *f)
{
    static const uint8_t reply[] = { 0x3f, 0xff, 0xff, 0xff, 0x9c, 0 };

    expect_reply(f, reply, sizeof(reply));
}

static void test_vectors(Fixture *f, gconstpointer data)
{
    static const uint8_t payload[] = { 2, 15, 255, 240, 6, 0, 3, 0 };
    g_autoptr(GByteArray) wire = encode(payload, sizeof(payload));
    g_autofree char *log = NULL;

    g_assert_cmpmem(wire->data, wire->len, identity, sizeof(identity));
    send_bytes(f, identity, sizeof(identity));
    /* Feed an escape pair across separate writes, as a UART does. */
    for (unsigned i = 0; i < sizeof(typed); i++) {
        send_bytes(f, typed + i, 1);
    }
    log = read_log(f);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
    expect_typed_reply(f);
    expect_count(log, "invalid", 0);
}

static void test_crc_and_recovery(Fixture *f, gconstpointer data)
{
    uint8_t bad[sizeof(identity)];
    g_autofree char *log = NULL;

    memcpy(bad, identity, sizeof(bad));
    bad[9] ^= 1;
    send_bytes(f, bad, sizeof(bad));
    /* The closing flag also opens the next packet. */
    send_bytes(f, identity + 1, sizeof(identity) - 1);
    log = read_log(f);
    expect_count(log, "invalid frame", 1);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
}

static void test_malformed_arguments(Fixture *f, gconstpointer data)
{
    uint8_t p[14] = { 0, 15, 255, 240, 6, 0, 6, 1 };
    g_autofree char *log = NULL;

    send_packet(f, p, 6); /* short header */
    send_packet(f, p, 8); /* missing type */
    p[8] = 0;
    send_packet(f, p, 12); /* short integer */
    p[8] = 1;
    memset(p + 9, 'x', 5);
    send_packet(f, p, sizeof(p)); /* unterminated string */
    p[8] = 0x86;
    send_packet(f, p, sizeof(p)); /* short indirect */
    p[8] = 2;
    send_packet(f, p, sizeof(p)); /* unknown type */
    p[7] = 0;
    send_packet(f, p, 9); /* trailing byte */
    send_bytes(f, identity, sizeof(identity));
    log = read_log(f);
    expect_count(log, "short request", 1);
    expect_count(log, "malformed arguments", 6);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
}

/* The PROM's third request has no argument count: header, address, code. */
static void test_request_without_a_count(Fixture *f, gconstpointer data)
{
    static const uint8_t observed[] = {
        0x7e, 0x05, 0x0f, 0xff, 0xf0, 0x06, 0x00, 0x03, 0x69, 0x27, 0x7e,
    };
    static const uint8_t reply[] = { 0x25, 0, 0, 0, 0, 2,
                                     0, 0, 0, 0, 0x41, 0, 0, 0, 0, 'O' };
    static const uint8_t p[] = { 5, 0x0f, 0xff, 0xf0, 0x06, 0x00 };
    g_autoptr(GByteArray) wire = encode(observed + 1, 7);
    g_autofree char *log = NULL;

    g_assert_cmpmem(wire->data, wire->len, observed, sizeof(observed));
    send_bytes(f, observed, sizeof(observed));
    expect_reply(f, reply, sizeof(reply));
    send_packet(f, p, sizeof(p));  /* one byte short of the code */
    log = read_log(f);
    expect_count(log, "short request", 1);
    expect_count(log, "unsupported request", 0);
}

static void test_length_limit(Fixture *f, gconstpointer data)
{
    /* Console text (class 2), as the PROM sends it: 128 payload bytes fit. */
    uint8_t p[129] = { 0x40 };
    g_autofree char *log = NULL;

    memset(p + 1, 'x', sizeof(p) - 1);
    send_packet(f, p, 128);
    send_packet(f, p, 129);
    send_bytes(f, identity, sizeof(identity));
    log = read_log(f);
    expect_count(log, "unsupported class=2", 1);
    expect_count(log, "invalid frame", 1);
    expect_identity_reply(f);
}

static void test_flags(Fixture *f, gconstpointer data)
{
    static const uint8_t noise[] = { 0x7d, 1, 2, 0x7e, 0x7e, 0x7d, 0x7e,
                                     1, 0x7e };
    g_autofree char *log = NULL;

    send_bytes(f, noise, sizeof(noise));
    send_bytes(f, identity, sizeof(identity));
    log = read_log(f);
    expect_count(log, "invalid frame", 2);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
}

static void test_classes_and_chunks(Fixture *f, gconstpointer data)
{
    uint8_t p[] = { 0x42, 'h', 'i' };
    uint8_t chunk[] = { 0, 15, 255, 240, 6, 0x30, 1, 0xff };
    g_autofree char *log = NULL;

    send_packet(f, p, sizeof(p));
    send_packet(f, chunk, sizeof(chunk));
    send_bytes(f, identity, sizeof(identity));
    log = read_log(f);
    g_assert_nonnull(strstr(log, "unsupported class=2 index=2 length=3"));
    g_assert_nonnull(strstr(log, "unsupported chunk code=0x3001"));
    expect_count(log, "malformed", 0);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
}

/* An SN2 L1 with a ringbuf console, and its node UART end taking replies. */
static Chardev *console_setup(Fixture *f, const char *id, ChardevRingbuf *ring,
                              ChardevBackend *backend)
{
    Chardev *chr;

    backend->u.ringbuf.data = ring;
    chr = qemu_chardev_new(id, TYPE_CHARDEV_RINGBUF, backend, NULL,
                           &error_abort);
    f->rx = g_byte_array_new();
    f->dev = qdev_new(TYPE_SGI_L1_SN2);
    qdev_prop_set_chr(f->dev, "chardev", chr);
    qdev_realize(f->dev, NULL, &error_abort);
    device_cold_reset(f->dev);
    qemu_chr_fe_init(&f->uart, sgi_l1_sn2_link(SGI_L1_SN2(f->dev)),
                     &error_abort);
    qemu_chr_fe_set_handlers(&f->uart, can_read, receive, NULL, NULL,
                             f, NULL, true);
    return chr;
}

/* A console frame (class 2) goes to the chardev as plain text. */
static void test_console_text(void)
{
    uint8_t frame[] = { 0x40, 'P', 'O', 'D', '>', ' ' };
    ChardevRingbuf ring = { .has_size = true, .size = 64 };
    ChardevBackend backend = { .type = CHARDEV_BACKEND_KIND_RINGBUF };
    Chardev *chr;
    Fixture f = { .room = 4096 };
    g_autofree char *text = NULL;

    chr = console_setup(&f, "sn2-console", &ring, &backend);
    static const uint8_t typed_a[] = { 0x7e, 0x00, 0x61, 0xc8, 0x7d, 0x5d,
                                       0x7e };

    send_packet(&f, frame, sizeof(frame));
    text = qmp_ringbuf_read("sn2-console", 64, false, 0, &error_abort);
    g_assert_cmpstr(text, ==, "POD> ");
    g_assert_cmpuint(f.rx->len, ==, 0);
    /* A typed character goes to the node as one frame with the same index. */
    qemu_chr_be_write(chr, (uint8_t *)"a", 1);
    g_assert_cmpuint(f.rx->len, ==, sizeof(typed_a));
    g_assert_cmpmem(f.rx->data, f.rx->len, typed_a, sizeof(typed_a));
    g_byte_array_set_size(f.rx, 0);
    /* Output on index 5 retargets the input; a reset takes it back to 0. */
    frame[0] = 0x45;
    send_packet(&f, frame, sizeof(frame));
    qemu_chr_be_write(chr, (uint8_t *)"a", 1);
    g_assert_cmpmem(f.rx->data + 1, 1, "\x05", 1);
    g_byte_array_set_size(f.rx, 0);
    device_cold_reset(f.dev);
    qemu_chr_be_write(chr, (uint8_t *)"a", 1);
    g_assert_cmpmem(f.rx->data, f.rx->len, typed_a, sizeof(typed_a));
    g_byte_array_set_size(f.rx, 0);
    g_byte_array_free(f.rx, true);
    qdev_unrealize(f.dev);
    qemu_chr_fe_deinit(&f.uart, false);
    object_unparent(OBJECT(f.dev));
    object_unparent(OBJECT(chr));
}

/* Console text reaches the chardev unchanged for every byte value. */
static void test_console_bytes(void)
{
    ChardevRingbuf ring = { .has_size = true, .size = 512 };
    ChardevBackend backend = { .type = CHARDEV_BACKEND_KIND_RINGBUF };
    Chardev *chr;
    Fixture f = { .room = 4096 };
    g_autofree char *text = NULL;
    g_autofree guchar *raw = NULL;
    gsize raw_len;

    chr = console_setup(&f, "sn2-bytes", &ring, &backend);

    for (unsigned base = 0; base < 256; base += 64) {
        uint8_t frame[65] = { 0x40 };

        for (unsigned i = 0; i < 64; i++) {
            frame[1 + i] = base + i;
        }
        send_packet(&f, frame, sizeof(frame));
    }
    text = qmp_ringbuf_read("sn2-bytes", 512, true, DATA_FORMAT_BASE64,
                            &error_abort);
    raw = g_base64_decode(text, &raw_len);
    g_assert_cmpuint(raw_len, ==, 256);
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmpuint(raw[i], ==, i);
    }
    g_byte_array_free(f.rx, true);
    qdev_unrealize(f.dev);
    qemu_chr_fe_deinit(&f.uart, false);
    object_unparent(OBJECT(f.dev));
    object_unparent(OBJECT(chr));
}

/* The PROM's full frames, 127 text bytes each, all arrive: 4 KiB of text. */
static void test_console_full_frames(void)
{
    ChardevRingbuf ring = { .has_size = true, .size = 8192 };
    ChardevBackend backend = { .type = CHARDEV_BACKEND_KIND_RINGBUF };
    Chardev *chr;
    Fixture f = { .room = 4096 };
    g_autofree char *text = NULL;
    GString *want = g_string_new(NULL);

    chr = console_setup(&f, "sn2-full", &ring, &backend);

    for (unsigned n = 0; n < 33; n++) {
        uint8_t frame[128] = { 0x44 };

        for (unsigned i = 0; i < 127; i++) {
            frame[1 + i] = 'a' + (n + i) % 26;
            g_string_append_c(want, frame[1 + i]);
        }
        send_packet(&f, frame, sizeof(frame));
    }
    text = qmp_ringbuf_read("sn2-full", 8192, false, 0, &error_abort);
    g_assert_cmpstr(text, ==, want->str);
    g_assert_cmpuint(want->len, ==, 33 * 127);
    g_string_free(want, true);
    g_byte_array_free(f.rx, true);
    qdev_unrealize(f.dev);
    qemu_chr_fe_deinit(&f.uart, false);
    object_unparent(OBJECT(f.dev));
    object_unparent(OBJECT(chr));
}

/* Ctrl-T enters the operator's L1 mode, where "pwr d" shuts the machine down. */
static void test_operator_mode(void)
{
    ChardevRingbuf ring = { .has_size = true, .size = 64 };
    ChardevBackend backend = { .type = CHARDEV_BACKEND_KIND_RINGBUF };
    Chardev *chr;
    Fixture f = { .room = 4096 };
    unsigned downs = shutdown_requests;
    static const uint8_t typed_a[] = { 0x7e, 0x00, 0x61, 0xc8, 0x7d, 0x5d,
                                       0x7e };
    g_autofree char *log = NULL;

    chr = console_setup(&f, "sn2-operator", &ring, &backend);
    start_log(&f);
    /* Ctrl-T, Ctrl-D leaves the mode, and the next byte is typed input. */
    qemu_chr_be_write(chr, (uint8_t *)"\x14\x04" "a", 3);
    g_assert_cmpmem(f.rx->data, f.rx->len, typed_a, sizeof(typed_a));
    g_byte_array_set_size(f.rx, 0);
    g_assert_cmpuint(shutdown_requests, ==, downs);
    /* Backspace edits the line; "pwr d" asks for the shutdown. */
    qemu_chr_be_write(chr, (uint8_t *)"\x14pwr x\x7f" "d\r", 9);
    g_assert_cmpuint(f.rx->len, ==, 0);
    g_assert_cmpuint(shutdown_requests, ==, downs + 1);
    /* Any other line is logged, and the console is typed into again. */
    qemu_chr_be_write(chr, (uint8_t *)"\x14" "help\r", 6);
    g_assert_cmpuint(shutdown_requests, ==, downs + 1);
    log = read_log(&f);
    expect_count(log, "operator command \"help\" is not modelled", 1);
    qemu_chr_be_write(chr, (uint8_t *)"a", 1);
    g_assert_cmpmem(f.rx->data, f.rx->len, typed_a, sizeof(typed_a));
    g_byte_array_set_size(f.rx, 0);
    g_remove(f.path);
    g_rmdir(f.dir);
    g_free(f.path);
    g_free(f.dir);
    g_byte_array_free(f.rx, true);
    qdev_unrealize(f.dev);
    qemu_chr_fe_deinit(&f.uart, false);
    object_unparent(OBJECT(f.dev));
    object_unparent(OBJECT(chr));
}

static void test_display(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    static const uint8_t bad[] = { 0x25, 0xff, 0xff, 0xff, 0x9c, 0 };
    uint8_t args[32];
    unsigned n = put_string(args, "Prom starting...");

    /* The wire vector is fixed by hand, apart from the test encoder. */
    send_request(f, 5, 0x1005, 1, args, n);
    expect_wire(f, ack, sizeof(ack));
    send_request(f, 5, 0x1005, 0, NULL, 0);
    expect_reply(f, bad, sizeof(bad));
}

/* The kernel opens the event subchannel with the number it chose, 19. */
/* Reboot request: the reply, then a reset request; anything else is logged. */
static void test_reboot(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x32, 0, 0, 0, 0, 0 };
    uint8_t p[40] = { 0x12, 0x0f, 0xff, 0xf0, 0x03, 0x00, 0x00, 1 };
    unsigned n = put_string(p + 8, "* rst");
    unsigned resets = reset_requests;
    g_autofree char *log = NULL;

    send_packet(f, p, 8 + n);
    expect_reply(f, ack, sizeof(ack));
    g_assert_cmpuint(reset_requests, ==, resets + 1);
    n = put_string(p + 8, "p 3 rst");
    send_packet(f, p, 8 + n);
    expect_reply(f, ack, sizeof(ack));
    g_assert_cmpuint(reset_requests, ==, resets + 2);
    /* The L2 probe before it fails: this machine has no L2. */
    {
        static const uint8_t no_l2[] = { 0x32, 0xff, 0xff, 0xff, 0xff, 0 };
        uint8_t q[40] = { 0x12, 0x1f, 0xff, 0xf0, 0x03, 0x00, 0x00, 1 };

        n = put_string(q + 8, "version");
        send_packet(f, q, 8 + n);
        expect_reply(f, no_l2, sizeof(no_l2));
        g_assert_cmpuint(reset_requests, ==, resets + 2);
    }
    /* An L2 probe that is not the "version" command has no known reply. */
    {
        uint8_t q[40] = { 0x12, 0x1f, 0xff, 0xf0, 0x03, 0x00, 0x00, 1 };

        n = put_string(q + 8, "status");
        send_packet(f, q, 8 + n);
    }
    /* An unknown command string has no known reply. */
    n = put_string(p + 8, "version");
    send_packet(f, p, 8 + n);
    g_assert_cmpuint(reset_requests, ==, resets + 2);
    log = read_log(f);
    expect_count(log, "unsupported request", 2);
}

/* Get time reads the RTC clock; set time is accepted and changes nothing. */
static void test_time(Fixture *f, gconstpointer data)
{
    static const uint8_t get[] = {
        0x25, 0, 0, 0, 0, 6,
        0, 0, 0, 0x07, 0xea, 0, 0, 0, 0, 11, 0, 0, 0, 0, 17,
        0, 0, 0, 0, 12, 0, 0, 0, 0, 34, 0, 0, 0, 0, 56,
    };
    static const uint8_t ok[] = { 0x25, 0, 0, 0, 0, 0 };
    uint8_t args[30];

    send_request(f, 5, 0x200e, 0, NULL, 0);
    expect_reply(f, get, sizeof(get));
    put_int(args, 2027);
    put_int(args + 5, 1);
    put_int(args + 10, 2);
    put_int(args + 15, 3);
    put_int(args + 20, 4);
    put_int(args + 25, 5);
    send_request(f, 5, 0x200d, 6, args, sizeof(args));
    expect_reply(f, ok, sizeof(ok));
    send_request(f, 5, 0x200e, 0, NULL, 0);
    expect_reply(f, get, sizeof(get));
}

static void test_event_subchannel(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    static const uint8_t bad[] = { 0x25, 0xff, 0xff, 0xff, 0x9c, 0 };
    uint8_t args[5];

    put_int(args, 19);
    send_request(f, 5, 0x1007, 1, args, sizeof(args));
    expect_wire(f, ack, sizeof(ack));
    send_request(f, 5, 0x1007, 0, NULL, 0);
    expect_reply(f, bad, sizeof(bad));
}

/* A reply that cannot be queued is dropped and logged. */
static void test_reply_queue_full(Fixture *f, gconstpointer data)
{
    uint8_t args[5];
    unsigned n = put_int(args, 19);
    g_autofree char *log = NULL;

    set_room(f, 0);
    /* Each acknowledgement is ten bytes: 102 fit in the 1024-byte queue. */
    for (int i = 0; i < 103; i++) {
        send_request(f, 5, 0x1007, 1, args, n);
    }
    log = read_log(f);
    expect_count(log, "reply queue full", 1);
}

static void test_version_report_estimate(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    uint8_t args[15];
    g_autofree char *log = NULL;

    /* The PROM's report: 1, then its version 5.04 as 5 and 4. */
    put_int(args, 1);
    put_int(args + 5, 5);
    put_int(args + 10, 4);
    send_request(f, 5, 0x100b, 3, args, sizeof(args));
    expect_wire(f, ack, sizeof(ack));
    /* The reply takes the request's index: another processor's slot. */
    send_request(f, 9, 0x100b, 3, args, sizeof(args));
    expect_reply(f, (const uint8_t[]){ 0x29, 0, 0, 0, 0, 0 }, 6);
    send_request(f, 5, 0x100b, 2, args, 10);
    expect_status(f, -100);
    log = read_log(f);
    expect_count(log, "answered by approximation", 2);
}

static void test_fifo_flow(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    uint8_t args[32];
    unsigned n = put_string(args, "Init memory...");

    set_room(f, 4);
    send_request(f, 5, 0x1005, 1, args, n);
    g_assert_cmpuint(f->rx->len, ==, 4);
    set_room(f, 4);
    qemu_chr_fe_accept_input(&f->uart);
    g_assert_cmpuint(f->rx->len, ==, 8);
    set_room(f, 4096);
    qemu_chr_fe_accept_input(&f->uart);
    expect_wire(f, ack, sizeof(ack));
}

static void test_pending_reply_reset(Fixture *f, gconstpointer data)
{
    uint8_t args[32];
    unsigned n = put_string(args, "Init I/O...");

    set_room(f, 0);
    send_request(f, 5, 0x1005, 1, args, n);
    device_cold_reset(f->dev);
    set_room(f, 4096);
    qemu_chr_fe_accept_input(&f->uart);
    g_assert_cmpuint(f->rx->len, ==, 0);
}

static void test_plain_requests(Fixture *f, gconstpointer data)
{
    static const struct {
        uint16_t code;
        uint8_t reply[26];
        unsigned len;
    } cases[] = {
        { 0x0003, { 0x25, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x41, 0, 0, 0, 0, 'O' }, 16 },
        { 0x000e, { 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, 11 },
        { 0x0013, { 0x25, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O' }, 16 },
        { 0x0009, { 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, 11 },
        { 0x0001, { 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, 11 },
        { 0x0015, { 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, 11 },
        { 0x0012, { 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 }, 11 },
        { 0x0008, { 0x25, 0xff, 0xff, 0xff, 0x98, 0 }, 6 },
        /* One node, one processor, present and enabled. */
        { 0x0010, { 0x25, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                    0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, 26 },
    };
    uint8_t arg[5];
    g_autofree char *log = NULL;

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        send_request(f, 5, cases[i].code, 0, NULL, 0);
        expect_reply(f, cases[i].reply, cases[i].len);
    }
    /* The PROM sends no arguments; any other shape is not answered. */
    send_request(f, 5, 0x0003, 1, arg, put_int(arg, 1));
    log = read_log(f);
    expect_count(log, "unsupported request", 1);
}

/* The SH_SCRATCH2 word the machine loads: rack, bay, slab and partition. */
static void test_node_identity(void)
{
    g_assert_cmphex(sgi_l1_sn2_identity_word(1, 1, 0), ==, 0x0410006c);
    g_assert_cmphex(sgi_l1_sn2_identity_word(3, 2, 5), ==, 0x0c20056c);
}

/* The module and partition replies follow the properties. */
static void test_node_identity_placed(Fixture *f, gconstpointer data)
{
    static const uint8_t module[] = {
        0x25, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0xc2, 0, 0, 0, 0, 'O' };
    static const uint8_t partition[] = {
        0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0, 5 };

    send_request(f, 5, 0x0003, 0, NULL, 0);
    expect_reply(f, module, sizeof(module));
    send_request(f, 5, 0x0009, 0, NULL, 0);
    expect_reply(f, partition, sizeof(partition));
}

/* Two processors: the presence and enable words have slices 0 and 2 set. */
static void test_two_cpus(Fixture *f, gconstpointer data)
{
    static const uint8_t presence[] = {
        0x25, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 5, 0, 0, 0, 0, 5 };

    send_request(f, 5, 0x0010, 0, NULL, 0);
    expect_reply(f, presence, sizeof(presence));
}

/* The datasheet's Table 21, bytes 0 to 71, for the 512 MB module. */
static const uint8_t spd[72] = {
        0x80, 0x08, 0x07, 0x0d, 0x0b, 0x01, 0x48, 0x00,
        0x04, 0x50, 0x70, 0x02, 0x82, 0x08, 0x08, 0x01,
        0x0e, 0x04, 0x1c, 0x01, 0x02, 0x20, 0xc0, 0x60,
        0x70, 0x75, 0x75, 0x3c, 0x28, 0x3c, 0x28, 0x80,
        0x60, 0x60, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x37, 0x46, 0x30, 0x28, 0x50, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xd3,
        0x2c, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void send_eeprom(Fixture *f, uint32_t id, uint32_t selector,
                        uint32_t offset, uint32_t length)
{
    uint8_t args[20];

    put_int(args, id);
    put_int(args + 5, selector);
    put_int(args + 10, offset);
    put_int(args + 15, length);
    send_request(f, 5, 0x0006, 4, args, sizeof(args));
}

/* A status-0 reply: bytes left, then the SPD bytes from offset. */
static void expect_spd(Fixture *f, uint32_t offset, uint32_t length,
                       uint32_t left)
{
    uint8_t reply[6 + 5 + 1 + 64] = { 0x25, 0, 0, 0, 0, length ? 2 : 1, 0 };
    unsigned n = 12;

    stl_be_p(reply + 7, left);
    if (length) {
        reply[11] = 0x80 | length;
        for (unsigned i = 0; i < length; i++) {
            reply[n++] = offset + i < sizeof(spd) ? spd[offset + i] : 0;
        }
    } else {
        n = 11;
    }
    expect_reply(f, reply, n);
}

static void test_spd(Fixture *f, gconstpointer data)
{
    /* The PROM's order: bank-major, slot ascending. */
    static const uint8_t ids[] = { 8, 16, 9, 17, 10, 18, 11, 19,
                                   12, 20, 13, 21, 14, 22, 15, 23 };
    uint8_t sum = 0;

    /* The L1 refuses a record whose byte 63 is not the sum of 0 to 62. */
    for (unsigned i = 0; i < 63; i++) {
        sum += spd[i];
    }
    g_assert_cmpuint(sum, ==, spd[63]);
    /* The checksum byte on the wire, so the device's copy is checked too. */
    send_eeprom(f, 8, 4, 56, 8);
    expect_spd(f, 56, 8, 192);
    for (unsigned i = 0; i < sizeof(ids); i++) {
        send_eeprom(f, ids[i], 4, 0, 48);
        if (i < 4) {
            expect_spd(f, 0, 48, 208);
        } else {
            expect_status(f, -104);
        }
    }
}

static void test_eeprom_ranges(Fixture *f, gconstpointer data)
{
    send_eeprom(f, 8, 4, 40, 16);
    expect_spd(f, 40, 16, 200);
    /* A read past the end is cut and leaves nothing. */
    send_eeprom(f, 8, 4, 250, 16);
    expect_spd(f, 250, 6, 0);
    send_eeprom(f, 8, 4, 64, 64);
    expect_spd(f, 64, 64, 128);
    /* Length 0 sends the count and no data. */
    send_eeprom(f, 8, 4, 0, 0);
    expect_spd(f, 0, 0, 256);
    send_eeprom(f, 8, 4, 256, 1);
    expect_status(f, -105);
    send_eeprom(f, 8, 5, 0, 48);
    expect_status(f, -105);
    send_eeprom(f, 8, 0, 0, 48);
    expect_status(f, -104);
    /* An empty slot is -104 whatever the selector. */
    send_eeprom(f, 10, 5, 0, 48);
    expect_status(f, -104);
}

/* The node's FRU: a common header and an empty board area, each sum 0. */
static const uint8_t fru_head[] = {
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfe,
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xc0,
    0xc0, 0xc0, 0xc0, 0xc1, 0x00, 0x00, 0x00, 0x7c,
};

/* The node's chassis area: a common header and an empty chassis area. */
static const uint8_t fru_chassis[] = {
    0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xfe,
    0x01, 0x01, 0x00, 0xc0, 0xc0, 0xc1, 0x00, 0xbd,
};

static void expect_fru_window_of(Fixture *f, const uint8_t *image,
                                 unsigned image_len, uint32_t offset,
                                 uint32_t length)
{
    uint8_t reply[6 + 5 + 1 + 64] = { 0x25, 0, 0, 0, 0, 2, 0 };
    unsigned n = 12;
    uint32_t end = MIN(offset + length, 512);

    stl_be_p(reply + 7, 512 - end);
    reply[11] = 0x80 | (end - offset);
    for (uint32_t i = offset; i < end; i++) {
        reply[n++] = i < image_len ? image[i] : 0;
    }
    expect_reply(f, reply, n);
}

static void expect_fru_window(Fixture *f, uint32_t offset, uint32_t length)
{
    expect_fru_window_of(f, fru_head, sizeof(fru_head), offset, length);
}

static void test_board_area(Fixture *f, gconstpointer data)
{
    g_autofree char *log = NULL;
    uint8_t sum = 0;

    /* The image is a valid FRU: header and board area each sum to zero. */
    for (unsigned i = 0; i < 8; i++) {
        sum += fru_head[i];
    }
    g_assert_cmpuint(sum, ==, 0);
    sum = 0;
    for (unsigned i = 8; i < 24; i++) {
        sum += fru_head[i];
    }
    g_assert_cmpuint(sum, ==, 0);

    /* The node answers in 64-byte windows, then a short last one. */
    for (uint32_t offset = 0; offset < 512; offset += 64) {
        send_eeprom(f, 1, 2, offset, 64);
        expect_fru_window(f, offset, 64);
    }
    send_eeprom(f, 1, 2, 500, 64);
    expect_fru_window(f, 500, 64);
    send_eeprom(f, 1, 2, 512, 64);
    expect_status(f, -105);
    /* No riser is fitted, so the I/O brick's record is absent. */
    send_eeprom(f, 98, 2, 0, 64);
    expect_status(f, -104);
    /* The chassis area (selector 1) is an empty, format-valid one. */
    sum = 0;
    for (unsigned i = 0; i < 8; i++) {
        sum += fru_chassis[i];
    }
    g_assert_cmpuint(sum, ==, 0);
    sum = 0;
    for (unsigned i = 8; i < 16; i++) {
        sum += fru_chassis[i];
    }
    g_assert_cmpuint(sum, ==, 0);
    send_eeprom(f, 1, 1, 0, 64);
    expect_fru_window_of(f, fru_chassis, sizeof(fru_chassis), 0, 64);
    send_eeprom(f, 1, 1, 512, 64);
    expect_status(f, -105);
    /* Other selectors have no known contents. */
    send_eeprom(f, 98, 1, 0, 64);
    log = read_log(f);
    expect_count(log, "unsupported request", 1);
}

/* A bad cpu count fails to realize, and an unrealized controller frees its link. */
static void test_realize_failures(void)
{
    DeviceState *first = qdev_new(TYPE_SGI_L1_SN2);
    DeviceState *dev = qdev_new(TYPE_SGI_L1_SN2);
    g_autofree char *label = NULL;
    Error *err = NULL;

    qdev_prop_set_uint32(dev, "cpus", 3);
    g_assert_false(qdev_realize(dev, NULL, &err));
    g_assert_nonnull(err);
    error_free(err);
    object_unref(OBJECT(dev));

    qdev_realize(first, NULL, &error_abort);
    label = g_strdup(sgi_l1_sn2_link(SGI_L1_SN2(first))->label);
    g_assert_nonnull(qemu_chr_find(label));
    qdev_unrealize(first);
    object_unparent(OBJECT(first));
    object_unref(OBJECT(first));
    g_assert_null(qemu_chr_find(label));
}

static void test_reset(Fixture *f, gconstpointer data)
{
    g_autofree char *log = NULL;

    send_bytes(f, typed, 13); /* between escape and escaped byte */
    device_cold_reset(f->dev);
    send_bytes(f, typed + 13, sizeof(typed) - 13);
    send_bytes(f, identity, sizeof(identity));
    log = read_log(f);
    expect_count(log, "unsupported request", 0);
    expect_identity_reply(f);
    expect_count(log, "invalid", 0);
}

static void test_migration(Fixture *f, gconstpointer data)
{
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(f->dev)->vmsd;
    QIOChannelBuffer *buffer = qio_channel_buffer_new(1024);
    QEMUFile *file = qemu_file_new_output(QIO_CHANNEL(buffer));
    g_autoptr(GByteArray) saved = g_byte_array_new();
    g_autofree char *log = NULL;
    unsigned split = GPOINTER_TO_UINT(data);

    send_bytes(f, typed, split);
    g_assert_cmpint(vmstate_save_state(file, vmsd, f->dev, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(file), ==, 0);
    g_byte_array_append(saved, buffer->data, buffer->usage);
    g_byte_array_append(saved, (const uint8_t *)"", 1);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    device_cold_reset(f->dev);
    buffer = qio_channel_buffer_new(saved->len);
    memcpy(buffer->data, saved->data, saved->len);
    buffer->usage = saved->len;
    file = qemu_file_new_input(QIO_CHANNEL(buffer));
    g_assert_cmpint(vmstate_load_state(file, vmsd, f->dev, vmsd->version_id,
                                       &error_abort), ==, 0);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    send_bytes(f, typed + split, sizeof(typed) - split);
    log = read_log(f);
    expect_count(log, "unsupported request", 0);
    expect_typed_reply(f);
    expect_count(log, "invalid", 0);
}

/* Save the device's state, reset it and load the state back. */
static void migrate(Fixture *f)
{
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(f->dev)->vmsd;
    QIOChannelBuffer *buffer = qio_channel_buffer_new(2048);
    QEMUFile *file = qemu_file_new_output(QIO_CHANNEL(buffer));
    g_autoptr(GByteArray) saved = g_byte_array_new();

    g_assert_cmpint(vmstate_save_state(file, vmsd, f->dev, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(file), ==, 0);
    g_byte_array_append(saved, buffer->data, buffer->usage);
    g_byte_array_append(saved, (const uint8_t *)"", 1);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    device_cold_reset(f->dev);
    buffer = qio_channel_buffer_new(saved->len);
    memcpy(buffer->data, saved->data, saved->len);
    buffer->usage = saved->len;
    file = qemu_file_new_input(QIO_CHANNEL(buffer));
    g_assert_cmpint(vmstate_load_state(file, vmsd, f->dev, vmsd->version_id,
                                       &error_abort), ==, 0);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
}

/* The operator's escape (Ctrl-T) leaves the console in a mode a stream keeps. */
static void test_migration_in_operator_mode(void)
{
    ChardevRingbuf ring = { .has_size = true, .size = 64 };
    ChardevBackend backend = { .type = CHARDEV_BACKEND_KIND_RINGBUF };
    Chardev *chr;
    Fixture f = { .room = 4096 };
    unsigned downs = shutdown_requests;

    chr = console_setup(&f, "sn2-operator-migrate", &ring, &backend);
    qemu_chr_be_write(chr, (uint8_t *)"\x14", 1);
    migrate(&f);
    /* The mode survived: the next line is an operator command. */
    qemu_chr_be_write(chr, (uint8_t *)"pwr d\r", 6);
    g_assert_cmpuint(shutdown_requests, ==, downs + 1);
    g_byte_array_free(f.rx, true);
    qdev_unrealize(f.dev);
    qemu_chr_fe_deinit(&f.uart, false);
    object_unparent(OBJECT(f.dev));
    object_unparent(OBJECT(chr));
}

/* A full frame in flight: the class byte, 127 text bytes and two checks. */
static void test_migration_full_frame(Fixture *f, gconstpointer data)
{
    uint8_t frame[1 + 130];

    frame[0] = 0x7e;
    memset(frame + 1, 0x41, 130);
    send_bytes(f, frame, sizeof(frame));
    migrate(f);
}

static void test_pending_reply_migration(Fixture *f, gconstpointer data)
{
    static const uint8_t ack[] = { 0x7e, 0x25, 0, 0, 0, 0, 0, 0x68, 0x66,
                                   0x7e };
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(f->dev)->vmsd;
    QIOChannelBuffer *buffer = qio_channel_buffer_new(2048);
    QEMUFile *file = qemu_file_new_output(QIO_CHANNEL(buffer));
    g_autoptr(GByteArray) saved = g_byte_array_new();
    uint8_t args[32];
    unsigned n = put_string(args, "Running");

    set_room(f, 0);
    send_request(f, 5, 0x1005, 1, args, n);
    g_assert_cmpint(vmstate_save_state(file, vmsd, f->dev, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(file), ==, 0);
    g_byte_array_append(saved, buffer->data, buffer->usage);
    g_byte_array_append(saved, (const uint8_t *)"", 1);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    device_cold_reset(f->dev);
    buffer = qio_channel_buffer_new(saved->len);
    memcpy(buffer->data, saved->data, saved->len);
    buffer->usage = saved->len;
    file = qemu_file_new_input(QIO_CHANNEL(buffer));
    g_assert_cmpint(vmstate_load_state(file, vmsd, f->dev, vmsd->version_id,
                                       &error_abort), ==, 0);
    qemu_fclose(file);
    object_unref(OBJECT(buffer));
    set_room(f, 4096);
    qemu_chr_fe_accept_input(&f->uart);
    expect_wire(f, ack, sizeof(ack));
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    module_call_init(MODULE_INIT_QOM);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
#define ADD(name, fn) \
    g_test_add("/sgi-sn2-l1/" name, Fixture, NULL, setup, fn, teardown)
    g_test_add("/sgi-sn2-l1/node-identity-placed", Fixture, NULL,
               setup_placed, test_node_identity_placed, teardown);
    g_test_add_func("/sgi-sn2-l1/node-identity", test_node_identity);
    g_test_add("/sgi-sn2-l1/two-cpus", Fixture, NULL, setup_two_cpus,
               test_two_cpus, teardown);
    ADD("vectors", test_vectors);
    ADD("crc-and-recovery", test_crc_and_recovery);
    ADD("malformed-arguments", test_malformed_arguments);
    ADD("request-without-a-count", test_request_without_a_count);
    ADD("length-limit", test_length_limit);
    ADD("flags", test_flags);
    ADD("classes-and-chunks", test_classes_and_chunks);
    g_test_add_func("/sgi-sn2-l1/console-text", test_console_text);
    g_test_add_func("/sgi-sn2-l1/console-bytes", test_console_bytes);
    g_test_add_func("/sgi-sn2-l1/operator-mode", test_operator_mode);
    g_test_add_func("/sgi-sn2-l1/migration-in-operator-mode",
                    test_migration_in_operator_mode);
    g_test_add_func("/sgi-sn2-l1/console-full-frames",
                    test_console_full_frames);
    ADD("reset", test_reset);
    ADD("display", test_display);
    ADD("version-report-estimate", test_version_report_estimate);
    ADD("plain-requests", test_plain_requests);
    ADD("spd", test_spd);
    ADD("eeprom-ranges", test_eeprom_ranges);
    ADD("board-area", test_board_area);
    ADD("event-subchannel", test_event_subchannel);
    ADD("reboot", test_reboot);
    ADD("time", test_time);
    ADD("reply-queue-full", test_reply_queue_full);
    ADD("fifo-flow", test_fifo_flow);
    ADD("pending-reply-reset", test_pending_reply_reset);
    ADD("pending-reply-migration", test_pending_reply_migration);
    ADD("migration-full-frame", test_migration_full_frame);
#undef ADD
    g_test_add_func("/sgi-sn2-l1/realize-failures", test_realize_failures);
    for (unsigned i = 1; i < sizeof(typed); i++) {
        g_autofree char *name = g_strdup_printf("/sgi-sn2-l1/migration/%u", i);

        g_test_add(name, Fixture, GUINT_TO_POINTER(i), setup,
                   test_migration, teardown);
    }
    return g_test_run();
}
