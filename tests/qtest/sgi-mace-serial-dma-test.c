/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic register tests for the reviewed MACE serial DMA policy. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-wait.h"
#include "sgi-migrate.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include "qobject/qdict.h"
#include "memtx.h"

#define BASE 0x1f318000ULL
#define RING 0x1f310000ULL

typedef enum SerialProfile {
    SERIAL_STANDALONE,
    SERIAL_O2_LITERAL,
    SERIAL_O2_LEGACY,
    SERIAL_OFFSET_STANDALONE,
} SerialProfile;

typedef struct SerialFixture {
    QTestState *qts;
    char *dir;
    char *ram;
    char *path[2];
    int fd;
    int uart[2];
} SerialFixture;

static uint64_t uart_address(unsigned port)
{
    return 0x1f390000ULL + port * 0x8000;
}

static SerialFixture fixture_start_config(const char *extra,
                                           SerialProfile profile)
{
    SerialFixture t = { 0 };
    bool o2 = profile == SERIAL_O2_LITERAL || profile == SERIAL_O2_LEGACY;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };

    t.dir = g_dir_make_tmp("serial-dma-XXXXXX", NULL);
    g_assert_nonnull(t.dir);
    t.ram = g_build_filename(t.dir, "ram", NULL);
    t.fd = g_open(t.ram, O_CREAT | O_EXCL | O_RDWR, 0600);
    g_assert_cmpint(t.fd, >=, 0);
    g_assert_cmpint(ftruncate(t.fd, (o2 ? 1024 : 64) * 1024 * 1024), ==, 0);
    for (unsigned p = 0; p < 2; p++) {
        t.path[p] = g_strdup_printf("%s/%u", t.dir, p);
    }
    if (o2) {
        t.qts = qtest_initf(
            "-M o2,register-test-profile=on,"
            "bank-profile=8x128m-synthetic,crime-profile=%s,"
            "memory-backend=dma,mace-eth-low-dma-compat=on -m 1G "
            "-cpu R10000 -nodefaults %s "
            "-object memory-backend-file,id=dma,size=1G,mem-path=%s,share=on "
            "-chardev socket,id=a,path=%s,server=on,wait=off "
            "-chardev socket,id=b,path=%s,server=on,wait=off "
            "-global sgi-mace.chardev-a=a -global sgi-mace.chardev-b=b "
            "-device sgi-serial-peer,id=peer-a,uart=/machine/mace/uart-a "
            "-device sgi-serial-peer,id=peer-b,uart=/machine/mace/uart-b",
            profile == SERIAL_O2_LEGACY ? "legacy-software" : "1.5", extra,
            t.ram, t.path[0], t.path[1]);
    } else {
        t.qts = qtest_initf("-M none -cpu R10000 %s "
            "-object memory-backend-file,id=dma,size=64M,mem-path=%s,share=on "
            "-chardev socket,id=a,path=%s,server=on,wait=off "
            "-chardev socket,id=b,path=%s,server=on,wait=off "
            "-device %s,id=mace,ram=/objects/dma/dma[0],"
            "chardev-a=a,chardev-b=b "
            "-device sgi-serial-peer,id=peer-a,uart=/machine/peripheral/mace/uart-a,"
            "ram=/objects/dma/dma[0] "
            "-device sgi-serial-peer,id=peer-b,uart=/machine/peripheral/mace/uart-b",
            extra, t.ram, t.path[0], t.path[1],
            profile == SERIAL_OFFSET_STANDALONE ?
            "sgi-mace-serial-offset-test" : "sgi-mace");
        qtest_sysbus_mmio_map(t.qts, "/machine/peripheral/mace", 0, 0x1f000000);
        qtest_irq_intercept_out_named(t.qts, "/machine/peripheral/mace",
                                      "crime");
    }
    qtest_writeq(t.qts, RING, 0x40000000);
    for (unsigned p = 0; p < 2; p++) {
        t.uart[p] = socket(AF_UNIX, SOCK_STREAM, 0);
        g_assert_cmpint(t.uart[p], >=, 0);
        g_strlcpy(addr.sun_path, t.path[p], sizeof(addr.sun_path));
        g_assert_cmpint(connect(t.uart[p], (struct sockaddr *)&addr,
                                sizeof(addr)), ==, 0);
        qtest_writeb(t.qts, uart_address(p) + 0x300, 0x80);
        qtest_writeb(t.qts, uart_address(p), 1);
        qtest_writeb(t.qts, uart_address(p) + 0x300, 3);
        qtest_writeb(t.qts, uart_address(p) + 0x200, 1);
    }
    return t;
}

static SerialFixture fixture_start_extra(const char *extra)
{
    return fixture_start_config(extra, SERIAL_STANDALONE);
}

static SerialFixture fixture_start(void)
{
    return fixture_start_extra("");
}

static void fixture_stop(SerialFixture *t)
{
    for (unsigned p = 0; p < 2; p++) {
        close(t->uart[p]);
    }
    qtest_quit(t->qts);
    close(t->fd);
    g_unlink(t->ram);
    for (unsigned p = 0; p < 2; p++) {
        g_unlink(t->path[p]);
        g_free(t->path[p]);
    }
    g_assert_cmpint(g_rmdir(t->dir), ==, 0);
    g_free(t->dir);
    g_free(t->ram);
}

static void packet_pair(uint8_t *block, unsigned pair, uint8_t command,
                        uint8_t byte)
{
    unsigned i = pair / 4 * 8 + pair % 4;

    block[i] = command;
    block[i + 4] = byte;
}

static void uart_expect(SerialFixture *t, unsigned port,
                        const uint8_t *bytes, unsigned count)
{
    uint8_t actual[4096];
    unsigned got = 0;
    SgiWait wait;

    g_assert_cmpuint(count, <=, sizeof(actual));
    sgi_wait_start(&wait, "serial DMA output");
    while (got < count) {
        ssize_t n = recv(t->uart[port], actual + got, count - got,
                         MSG_DONTWAIT);

        if (n > 0) {
            got += n;
        } else {
            g_assert_true(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            sgi_wait_pause(&wait, 1000);
        }
    }
    g_assert_cmpmem(actual, count, bytes, count);
}

static void uart_empty(SerialFixture *t, unsigned port)
{
    uint8_t byte;

    g_assert_cmpint(recv(t->uart[port], &byte, 1, MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void ring_write(SerialFixture *t, unsigned port, unsigned offset,
                       const uint8_t *data, unsigned count)
{
    g_assert_cmpint(pwrite(t->fd, data, count, 0x4000 + port * 0x2000 + offset),
                    ==, count);
}

static void pointer_wait(SerialFixture *t, uint64_t reg, unsigned expected)
{
    SgiWait wait;

    sgi_wait_start(&wait, "serial DMA pointer");
    while (qtest_readq(t->qts, reg) != expected) {
        sgi_wait_pause(&wait, 1000);
    }
}

static void peer_set(SerialFixture *t, unsigned port,
                      const char *property, uint64_t value)
{
    g_autofree char *path = g_strdup_printf("/machine/peripheral/peer-%c",
                                           'a' + port);

    qtest_qmp_assert_success(t->qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%" PRIu64 "}}",
        path, property, value);
}

static void test_registers(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device sgi-mace,id=mace");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, 0x1f000000);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        /* Reset and 32-byte publication are selected compatibility policy. */
        g_assert_cmphex(qtest_readq(s, base), ==, 0x400);
        g_assert_cmphex(qtest_readq(s, base + 0x20), ==, 0x400);
        for (unsigned i = 1; i < 8; i++) {
            if (i != 4) {
                g_assert_cmphex(qtest_readq(s, base + 8 * i), ==, 0);
            }
        }
        qtest_writeq(s, base, 0);
        qtest_writeq(s, base + 0x10, 0x3f);
        g_assert_cmphex(qtest_readq(s, base + 0x10), ==, 0x20);
        g_assert_cmphex(qtest_readq(s, base + 0x18), ==, 0x20);
        qtest_writeq(s, base + 0x10, 0);
        g_assert_cmphex(qtest_readq(s, base + 0x10), ==, 0x20);
        qtest_writeq(s, base + 0x30, 0x40);
        g_assert_cmphex(qtest_readq(s, base + 0x30), ==, 0);
        qtest_writeq(s, base + 8, 0x40);
        g_assert_cmphex(qtest_readq(s, base + 8), ==, 0);
        qtest_writeq(s, base + 0x28, 0x20);
        g_assert_cmphex(qtest_readq(s, base + 0x28), ==, 0);
        qtest_writeq(s, base, 0x400);
        g_assert_cmphex(qtest_readq(s, base + 0x18), ==, 0);
    }
    qtest_writeq(s, BASE + 0x4000, 0);
    qtest_writeq(s, BASE + 0x4010, 64);
    qtest_writeq(s, BASE, 0x400);
    g_assert_cmphex(qtest_readq(s, BASE + 0x4000), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x4010), ==, 64);
    qtest_quit(s);
}

static void test_base_validation(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device sgi-mace,id=mace");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, 0x1f000000);
    qtest_writeq(s, RING, 0x40000001);
    qtest_writeq(s, BASE, 0);
    qtest_writeq(s, RING, 0x40008000);
    g_assert_cmphex(qtest_readq(s, RING), ==, 0x40000001);
    qtest_writeq(s, RING, 0x40000000);
    g_assert_cmphex(qtest_readq(s, RING), ==, 0x40000000);
    qtest_writeq(s, BASE, 0x400);
    qtest_writeq(s, RING, 0x40008001);
    g_assert_cmphex(qtest_readq(s, RING), ==, 0x40008001);
    qtest_quit(s);
}

static bool threshold(unsigned selector, unsigned blocks)
{
    switch (selector) {
    case 0: return false;
    case 1: return blocks < 32;
    case 2: return blocks < 64;
    case 3: return blocks < 96;
    case 4: return blocks == 0;
    case 5: return blocks != 0;
    case 6: return blocks == 127;
    case 7: return blocks != 127;
    default:
        g_assert_not_reached();
    }
}

static void test_tx_thresholds(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device sgi-mace,id=mace");
    const unsigned depths[] = {
        0, 1, 31, 32, 33, 63, 64, 65, 95, 96, 97, 126, 127
    };

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, 0x1f000000);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;
        uint32_t bit = 1U << (21 + p * 6);

        for (unsigned j = 0; j < G_N_ELEMENTS(depths); j++) {
            qtest_writeq(s, base, 0x400);
            qtest_writeq(s, base, 0);
            qtest_writeq(s, base + 0x10, depths[j] * 32);
            for (unsigned sel = 0; sel < 8; sel++) {
                qtest_writeq(s, base, 0x200 | sel * 32);
                g_assert_cmpint(!!(qtest_readq(s, RING + 0x10) & bit), ==,
                                threshold(sel, depths[j]));
                qtest_writeq(s, RING + 0x10, 0);
                g_assert_cmpint(!!(qtest_readq(s, RING + 0x10) & bit), ==,
                                threshold(sel, depths[j]));
                qtest_writeq(s, base, sel * 32);
                g_assert_cmphex(qtest_readq(s, RING + 0x10) & bit, ==, 0);
            }
        }
        qtest_writeq(s, base, 0x400);
    }
    qtest_quit(s);
}

static void test_tx_packet_counts(void)
{
    SerialFixture t = fixture_start();

    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        for (unsigned count = 0; count <= 16; count++) {
            uint8_t block[32] = { 0 }, bytes[16];

            qtest_writeq(t.qts, base, 0x400);
            qtest_writeq(t.qts, base, 0);
            qtest_writeb(t.qts, uart_address(p) + 0x200, count & 1);
            for (unsigned i = 0; i < 16; i++) {
                bytes[i] = 0x31 + i * 7;
                packet_pair(block, i, i < count ? 0x40 : 0, bytes[i]);
            }
            ring_write(&t, p, 0, block, 32);
            qtest_writeq(t.qts, base + 0x10, 32);
            /* Bank selection cannot redirect DMA writes into the divisor. */
            qtest_writeb(t.qts, uart_address(p) + 0x300, 0x83);
            qtest_writeq(t.qts, base, 0x200);
            pointer_wait(&t, base + 8, 32);
            uart_expect(&t, p, bytes, count);
            uart_empty(&t, p);
            g_assert_cmphex(qtest_readb(t.qts, uart_address(p)), ==, 1);
            qtest_writeb(t.qts, uart_address(p) + 0x300, 3);
        }
    }
    fixture_stop(&t);
}

static void test_delay_freeze(void)
{
    SerialFixture t = fixture_start();
    uint8_t block[32] = { 0 };
    const uint8_t first = 'a', second = 'Z';

    packet_pair(block, 0, 0x40, first);
    packet_pair(block, 1, 0xe0, 10);
    packet_pair(block, 2, 0x40, second);
    packet_pair(block, 3, 0x80, 3);
    for (unsigned pause = 0; pause < 2; pause++) {
        qtest_writeq(t.qts, BASE, 0x400);
        qtest_writeq(t.qts, BASE, 0);
        ring_write(&t, 0, 0, block, 32);
        qtest_writeq(t.qts, BASE + 0x10, 32);
        qtest_writeq(t.qts, BASE, 0x200);
        uart_expect(&t, 0, &first, 1);
        qtest_clock_step(t.qts, 4 * 1000000);
        if (pause) {
            qtest_writeq(t.qts, RING, 0x40000001);
        } else {
            qtest_writeq(t.qts, BASE, 0);
        }
        qtest_clock_step(t.qts, 20 * 1000000);
        g_assert_cmphex(qtest_readq(t.qts, RING + 0x10) & (1U << 22), ==, 0);
        g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 0);
        uart_empty(&t, 0);
        if (pause) {
            qtest_writeq(t.qts, RING, 0x40000000);
        } else {
            qtest_writeq(t.qts, BASE, 0x200);
        }
        qtest_clock_step(t.qts, 5 * 1000000);
        uart_empty(&t, 0);
        qtest_clock_step(t.qts, 1000000);
        pointer_wait(&t, BASE + 8, 32);
        uart_expect(&t, 0, &second, 1);
        g_assert_cmphex(qtest_readb(t.qts, uart_address(0) + 0x400), ==, 3);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
        qtest_writeq(t.qts, RING + 0x10, UINT32_MAX);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
        qtest_writeq(t.qts, RING + 0x10, ~(1U << 22));
        g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
    }
    fixture_stop(&t);
}

static void test_rx_packet(void)
{
    SerialFixture t = fixture_start();

    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        for (unsigned count = 1; count <= 4; count++) {
            uint8_t actual[32], expected[32], data[] = { 'R', 'x', 0x93, 0x17 };

            for (unsigned i = 0; i < 4; i++) {
                memset(expected + i * 8, 0, 4);
                memset(expected + i * 8 + 4, 0xff, 4);
            }
            for (unsigned i = 0; i < count; i++) {
                packet_pair(expected, i, 0x80, data[i]);
            }
            qtest_writeq(t.qts, base + 0x20, 0x400);
            qtest_writeq(t.qts, base + 0x20, 0);
            qtest_writeb(t.qts, uart_address(p) + 0x200, count == 1 ? 0 : 0xc7);
            g_assert_cmpint(write(t.uart[p], data, count), ==, count);
            sgi_wait_socket_read(t.uart[p]);
            /* RX ignores the PIO bank selector and interrupt enable. */
            qtest_writeb(t.qts, uart_address(p) + 0x300, 0x83);
            qtest_writeq(t.qts, base + 0x20, 0x200);
            pointer_wait(&t, base + 0x30, 32);
            g_assert_cmpint(pread(t.fd, actual, 32, 0x5000 + p * 0x2000),
                            ==, 32);
            g_assert_cmpmem(actual, 32, expected, 32);
            g_assert_cmphex(qtest_readb(t.qts, uart_address(p)), ==, 1);
            qtest_writeb(t.qts, uart_address(p) + 0x300, 3);
            qtest_writeq(t.qts, base + 0x28, 32);
            g_assert_cmphex(qtest_readq(t.qts, base + 0x38), ==, 0);
        }
    }
    fixture_stop(&t);
}

static void test_dma_faults(void)
{
    SerialFixture t = fixture_start();
    const uint32_t bases[] = { 0, 0x44000000, 0x84000000 };

    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        qtest_writeq(t.qts, BASE, 0x400);
        qtest_writeq(t.qts, RING, bases[i]);
        qtest_writeq(t.qts, BASE, 0);
        qtest_writeq(t.qts, BASE + 0x10, 32);
        qtest_writeq(t.qts, BASE, 0x200);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 23));
        g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 0);
        qtest_writeq(t.qts, RING + 0x10, 0);
        qtest_writeq(t.qts, BASE, 0);
        qtest_writeq(t.qts, BASE, 0x200);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 23));
        uart_empty(&t, 0);
    }
    fixture_stop(&t);
}

static void test_rx_errors(void)
{
    SerialFixture t = fixture_start();

    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        for (unsigned error = 2; error <= 0x10; error <<= 1) {
            uint8_t actual[32];

            qtest_writeq(t.qts, base + 0x20, 0x400);
            qtest_writeq(t.qts, base + 0x20, 0);
            peer_set(&t, p, "receive", 0xa7);
            peer_set(&t, p, "line-errors", error);
            qtest_writeq(t.qts, base + 0x20, 0x200);
            pointer_wait(&t, base + 0x30, 32);
            g_assert_cmpint(pread(t.fd, actual, 32, 0x5000 + p * 0x2000),
                            ==, 32);
            g_assert_cmphex(actual[0], ==, 0x80 | (error >> 1));
            g_assert_cmphex(actual[4], ==, 0xa7);
            if (error == 2) {
                g_assert_true(qtest_readq(t.qts, RING + 0x10) &
                              (1U << (25 + p * 6)));
                peer_set(&t, p, "receive", 0x35);
                qtest_writeq(t.qts, base + 0x28, 32);
                qtest_writeq(t.qts, base + 0x20, 0);
                qtest_writeq(t.qts, base + 0x20, 0x200);
                g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 32);
                /* DMA reset preserves the UART's queued byte. */
                qtest_writeq(t.qts, base + 0x20, 0x400);
                qtest_writeq(t.qts, base + 0x20, 0x200);
                pointer_wait(&t, base + 0x30, 32);
            }
            /* The core exposes parity/framing as persistent injected state. */
            qtest_writeq(t.qts, base + 0x20, 0x400);
            qtest_writeq(t.qts, RING, 0x40000001);
            qtest_writeq(t.qts, RING, 0x40000000);
        }
    }
    fixture_stop(&t);
}

static void test_modem_pause(void)
{
    SerialFixture t = fixture_start();
    uint8_t block[32] = { 0 };
    const uint8_t output = 'M';

    packet_pair(block, 0, 0xe0, 10);
    packet_pair(block, 1, 0x40, output);
    ring_write(&t, 0, 0, block, 32);
    qtest_writeb(t.qts, uart_address(0) + 0x100, 9);
    qtest_writeq(t.qts, BASE, 0);
    qtest_writeq(t.qts, BASE + 0x10, 32);
    qtest_writeq(t.qts, BASE, 0x200);
    qtest_clock_step(t.qts, 4 * 1000000);
    qtest_writeq(t.qts, BASE + 0x20, 0x200);
    peer_set(&t, 0, "modem-status", 0xb1);
    peer_set(&t, 0, "receive", 0x51);
    g_assert_cmphex(qtest_readq(t.qts, BASE + 0x30), ==, 0);
    qtest_writeq(t.qts, RING + 0x18, 1U << 20);
    g_assert_true(qtest_get_irq(t.qts, 4));
    qtest_clock_step(t.qts, 20 * 1000000);
    uart_empty(&t, 0);
    g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 0);
    qtest_writeq(t.qts, RING + 0x18, 0);
    qtest_writeq(t.qts, RING + 0x18, UINT32_MAX);
    qtest_clock_step(t.qts, 1000000);
    uart_empty(&t, 0);
    g_assert_cmphex(qtest_readb(t.qts, uart_address(0) + 0x600), ==, 0xb1);
    qtest_clock_step(t.qts, 5 * 1000000);
    uart_empty(&t, 0);
    qtest_clock_step(t.qts, 1000000);
    pointer_wait(&t, BASE + 8, 32);
    uart_expect(&t, 0, &output, 1);
    fixture_stop(&t);
}

static void test_rx_full_wrap(void)
{
    SerialFixture t = fixture_start();
    uint8_t guard[0x4000], actual[0x4000];

    memset(guard, 0x69, sizeof(guard));
    g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0), ==, sizeof(guard));
    g_assert_cmpint(pwrite(t.fd, guard, 4096, 0x4000), ==, 4096);
    g_assert_cmpint(pwrite(t.fd, guard, 4096, 0x6000), ==, 4096);
    g_assert_cmpint(pwrite(t.fd, guard, 32, 0x8000), ==, 32);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;
        uint8_t block[32];

        qtest_writeb(t.qts, uart_address(p) + 0x100, 1);
        qtest_writeq(t.qts, base + 0x20, 0x200);
        for (unsigned i = 0; i < 127; i++) {
            peer_set(&t, p, "receive", i);
            pointer_wait(&t, base + 0x30, (i + 1) * 32);
        }
        g_assert_cmphex(qtest_readq(t.qts, base + 0x38), ==, 0xfe0);
        peer_set(&t, p, "receive", 0xe3);
        g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << (20 + 6 * p)));
        qtest_writeq(t.qts, base + 0x20, 0);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << (20 + 6 * p)));
        qtest_writeq(t.qts, base + 0x20, 0x200);
        g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << (20 + 6 * p)));
        g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 0xfe0);
        /* Free exactly one block; the pending byte publishes through wrap. */
        qtest_writeq(t.qts, base + 0x28, 32);
        pointer_wait(&t, base + 0x30, 0);
        g_assert_cmpint(pread(t.fd, block, 32, 0x5fe0 + p * 0x2000), ==, 32);
        g_assert_cmphex(block[0], ==, 0x80);
        g_assert_cmphex(block[4], ==, 0xe3);
        peer_set(&t, p, "receive", 0x72);
        qtest_writeq(t.qts, base + 0x28, 64);
        pointer_wait(&t, base + 0x30, 32);
        g_assert_cmpint(pread(t.fd, block, 32, 0x5000 + p * 0x2000), ==, 32);
        g_assert_cmphex(block[4], ==, 0x72);
        qtest_writeq(t.qts, base + 0x20, 0x400);
    }
    g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0), ==, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));
    for (unsigned p = 0; p < 2; p++) {
        g_assert_cmpint(pread(t.fd, actual, 4096, 0x4000 + p * 0x2000),
                        ==, 4096);
        g_assert_cmpmem(actual, 4096, guard, 4096);
    }
    g_assert_cmpint(pread(t.fd, actual, 32, 0x8000), ==, 32);
    g_assert_cmpmem(actual, 32, guard, 32);
    fixture_stop(&t);
}

static void test_tx_backpressure(void)
{
    SerialFixture t = fixture_start();
    uint8_t expected[127 * 16];
    uint8_t ring[4096] = { 0 };
    SgiWait wait;

    for (unsigned i = 0; i < G_N_ELEMENTS(expected); i++) {
        expected[i] = i * 13 + 17;
        packet_pair(ring + i / 16 * 32, i % 16, 0x40, expected[i]);
    }
    ring_write(&t, 0, 0, ring, sizeof(ring));
    qtest_writeq(t.qts, BASE, 0);
    qtest_writeq(t.qts, BASE + 0x10, 0xfe0);
    qtest_writeq(t.qts, BASE, 0x200);
    sgi_wait_start(&wait, "real UART backend backpressure");
    for (;;) {
        QDict *reply = qtest_qmp(t.qts,
            "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/peer-a',"
            "'property':'transmit-blocked'}}");
        bool blocked = qdict_get_bool(reply, "return");

        qobject_unref(reply);
        if (blocked) {
            break;
        }
        sgi_wait_pause(&wait, 1000);
    }
    qtest_writeq(t.qts, BASE, 0);
    {
        unsigned held = qtest_readq(t.qts, BASE + 8);

        qtest_clock_step(t.qts, 1000000);
        g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, held);
    }
    qtest_writeq(t.qts, BASE, 0x200);
    uart_expect(&t, 0, expected, sizeof(expected));
    pointer_wait(&t, BASE + 8, 0xfe0);
    uart_empty(&t, 0);
    /* Cross the ring end using legal publication while DMA is enabled. */
    memset(ring, 0, sizeof(ring));
    packet_pair(ring, 0, 0x40, 'w');
    ring_write(&t, 0, 0xfe0, ring, 32);
    packet_pair(ring, 0, 0x40, 'r');
    ring_write(&t, 0, 0, ring, 32);
    qtest_writeq(t.qts, BASE + 0x10, 32);
    pointer_wait(&t, BASE + 8, 32);
    uart_expect(&t, 0, (const uint8_t *)"wr", 2);
    fixture_stop(&t);
}

static void test_no_ecc_view(void)
{
    SerialFixture t = fixture_start();
    uint8_t block[32] = { 0 }, actual[32];

    qtest_writeq(t.qts, RING, 0x80000000);
    packet_pair(block, 0, 0x40, 'N');
    ring_write(&t, 1, 0, block, 32);
    qtest_writeq(t.qts, BASE + 0x4000, 0);
    qtest_writeq(t.qts, BASE + 0x4010, 32);
    qtest_writeq(t.qts, BASE + 0x4000, 0x200);
    pointer_wait(&t, BASE + 0x4008, 32);
    uart_expect(&t, 1, (const uint8_t *)"N", 1);
    qtest_writeq(t.qts, BASE + 0x20, 0x200);
    peer_set(&t, 0, "receive", 'E');
    pointer_wait(&t, BASE + 0x30, 32);
    g_assert_cmpint(pread(t.fd, actual, 32, 0x5000), ==, 32);
    g_assert_cmphex(actual[4], ==, 'E');
    fixture_stop(&t);
}

static void test_low_ethernet_alias_isolation(void)
{
    QTestState *s;
    uint8_t block[32] = { 0 };

    if (!qtest_has_machine("o2")) {
        g_test_skip("O2 test machine is unavailable");
        return;
    }
    s = qtest_init("-M o2,register-test-profile=on,"
        "bank-profile=8x128m-synthetic,"
                              "mace-eth-low-dma-compat=on -m 1G -cpu R10000 "
                              "-nodefaults");

    packet_pair(block, 0, 0x20, 0);
    qtest_memwrite(s, 0x40004000, block, 32);
    qtest_writeq(s, RING, 0);
    qtest_writeq(s, BASE, 0);
    qtest_writeq(s, BASE + 0x10, 32);
    qtest_writeq(s, BASE, 0x200);
    g_assert_true(qtest_readq(s, RING + 0x10) & (1U << 23));
    g_assert_false(qtest_readq(s, RING + 0x10) & (1U << 22));
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
    qtest_quit(s);
}

static void test_migrate_delay(void)
{
    for (unsigned mode = 0; mode < 5; mode++) {
        SerialFixture from = fixture_start();
        SerialFixture to = fixture_start_extra("-incoming defer");
        uint8_t block[32] = { 0 };
        g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", path);

        packet_pair(block, 0, 0x40, 'A');
        packet_pair(block, 1, 0xe0, 10);
        packet_pair(block, 2, 0x40, 'B');
        ring_write(&from, 0, 0, block, 32);
        qtest_writeq(from.qts, BASE, 0);
        qtest_writeq(from.qts, BASE + 0x10, 32);
        qtest_writeq(from.qts, BASE, 0x200);
        uart_expect(&from, 0, (const uint8_t *)"A", 1);
        qtest_clock_step(from.qts, 4 * 1000000);
        if (mode == 1) {
            qtest_writeq(from.qts, BASE, 0);
        } else if (mode == 2) {
            qtest_writeq(from.qts, RING, 0x40000001);
        } else if (mode >= 3) {
            qtest_writeb(from.qts, uart_address(0) + 0x100, 8);
            peer_set(&from, 0, "modem-status", 0xb1);
        }
        qtest_qmp_assert_success(to.qts,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from.qts, uri);
        sgi_wait_migration(to.qts, "completed");
        if (mode) {
            qtest_clock_step(to.qts, 20 * 1000000);
            uart_empty(&to, 0);
            g_assert_cmphex(qtest_readq(to.qts, BASE + 8), ==, 0);
        }
        if (mode == 1) {
            qtest_writeq(to.qts, BASE, 0x200);
        } else if (mode == 2) {
            qtest_writeq(to.qts, RING, 0x40000000);
        } else if (mode == 4) {
            qtest_writeb(to.qts, uart_address(0) + 0x100, 0);
        } else if (mode == 3) {
            g_assert_cmphex(qtest_readb(to.qts, uart_address(0) + 0x600),
                            ==, 0xb1);
        }
        qtest_clock_step(to.qts, 5 * 1000000);
        uart_empty(&to, 0);
        qtest_clock_step(to.qts, 1000000);
        pointer_wait(&to, BASE + 8, 32);
        uart_expect(&to, 0, (const uint8_t *)"B", 1);
        uart_empty(&to, 0);
        if (mode == 4) {
            g_assert_cmphex(qtest_readb(to.qts, uart_address(0) + 0x600),
                            ==, 0xb1);
        }
        g_assert_true(qtest_readq(to.qts, RING + 0x10) & (1U << 22));
        fixture_stop(&to);
        g_unlink(path);
        fixture_stop(&from);
    }
}

static void test_migrate_rx_full(void)
{
    SerialFixture from = fixture_start();
    SerialFixture to = fixture_start_extra("-incoming defer");
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);
    uint8_t block[32];

    qtest_writeq(from.qts, BASE + 0x20, 0x200);
    for (unsigned i = 0; i < 127; i++) {
        peer_set(&from, 0, "receive", i);
        pointer_wait(&from, BASE + 0x30, (i + 1) * 32);
    }
    peer_set(&from, 0, "receive", 0xe7);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmphex(qtest_readq(to.qts, BASE + 0x38), ==, 0xfe0);
    g_assert_cmpint(pread(to.fd, block, 32, 0x5000), ==, 32);
    g_assert_cmphex(block[0], ==, 0x80);
    g_assert_cmphex(block[4], ==, 0);
    qtest_writeq(to.qts, BASE + 0x28, 32);
    pointer_wait(&to, BASE + 0x30, 0);
    g_assert_cmpint(pread(to.fd, block, 32, 0x5fe0), ==, 32);
    g_assert_cmphex(block[0], ==, 0x80);
    g_assert_cmphex(block[4], ==, 0xe7);
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

static void test_access_widths(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE
        "-M none -cpu R10000 -device sgi-mace,id=mace");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/mace", 0, 0x1f000000);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        g_assert_cmpuint(memtx_writeb(s, base, 0), ==, MEMTX_DECODE_ERROR);
        g_assert_cmpuint(memtx_writew(s, base, 0), ==, MEMTX_DECODE_ERROR);
        g_assert_cmpuint(memtx_writel(s, base, 0), ==, MEMTX_DECODE_ERROR);
        g_assert_cmphex(qtest_readq(s, base), ==, 0x400);
    }
    qtest_quit(s);
}

static void test_rx_thresholds(void)
{
    SerialFixture t = fixture_start();

    qtest_writeq(t.qts, BASE + 0x20, 0x200);
    for (unsigned depth = 0; depth <= 127; depth++) {
        if (depth) {
            peer_set(&t, 0, "receive", depth);
            pointer_wait(&t, BASE + 0x30, depth * 32);
        }
        for (unsigned sel = 0; sel < 8; sel++) {
            bool expected = sel >= 1 && sel <= 3 ? depth >= sel * 32 :
                            threshold(sel, depth);

            qtest_writeq(t.qts, BASE + 0x20, 0x200 | sel * 32);
            g_assert_cmpint(!!(qtest_readq(t.qts, RING + 0x10) & (1U << 24)),
                            ==, expected);
            qtest_writeq(t.qts, RING + 0x18, 1U << 24);
            g_assert_cmpint(qtest_get_irq(t.qts, 4), ==, expected);
            qtest_writeq(t.qts, RING + 0x18, 0);
            g_assert_false(qtest_get_irq(t.qts, 4));
        }
    }
    fixture_stop(&t);
}

static void test_pair_events_and_pio(void)
{
    SerialFixture t = fixture_start();
    uint8_t block[32] = { 0 };

    packet_pair(block, 0, 0x20, 0);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + 0x4000 * p;

        ring_write(&t, p, 0, block, 32);
        qtest_writeq(t.qts, base, 0);
        qtest_writeq(t.qts, base + 0x10, 32);
        qtest_writeq(t.qts, base, 0x200);
        pointer_wait(&t, base + 8, 32);
    }
    g_assert_cmphex(qtest_readq(t.qts, RING + 0x10) & 0x10400000,
                    ==, 0x10400000);
    qtest_writeq(t.qts, RING + 0x10, ~(1U << 22));
    g_assert_cmphex(qtest_readq(t.qts, RING + 0x10) & 0x10400000, ==, 1U << 28);
    qtest_writeq(t.qts, RING + 0x18, 1U << 28);
    g_assert_true(qtest_get_irq(t.qts, 4));
    qtest_writeq(t.qts, RING + 0x10, ~(1U << 28));
    g_assert_false(qtest_get_irq(t.qts, 4));
    qtest_writeq(t.qts, BASE, 0);
    qtest_writeb(t.qts, uart_address(0), 'P');
    uart_expect(&t, 0, (const uint8_t *)"P", 1);
    qtest_writeq(t.qts, BASE, 0x200);
    g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 32);
    uart_empty(&t, 0);
    fixture_stop(&t);
}

static void test_rx_overrun_stops(void)
{
    for (unsigned full = 0; full < 2; full++) {
        SerialFixture t = fixture_start();

        qtest_writeq(t.qts, BASE + 0x20, 0x200);
        if (full) {
            for (unsigned i = 0; i < 127; i++) {
                peer_set(&t, 0, "receive", i);
                pointer_wait(&t, BASE + 0x30, (i + 1) * 32);
            }
        }
        if (full) {
            peer_set(&t, 0, "receive", 0x57);
        }
        peer_set(&t, 0, "line-errors", 2);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 25));
        g_assert_cmphex(qtest_readq(t.qts, BASE + 0x30), ==, full ? 0xfe0 : 0);
        peer_set(&t, 0, "receive", 0x37);
        if (full) {
            qtest_writeq(t.qts, BASE + 0x28, 32);
        }
        qtest_writeq(t.qts, RING + 0x18, UINT32_MAX);
        qtest_writeq(t.qts, BASE + 0x20, 0);
        qtest_writeq(t.qts, BASE + 0x20, 0x200);
        g_assert_cmphex(qtest_readq(t.qts, BASE + 0x30), ==, full ? 0xfe0 : 0);
        fixture_stop(&t);
    }
}

static void test_migrate_faults(void)
{
    SerialFixture from = fixture_start();
    SerialFixture to = fixture_start_extra("-incoming defer");
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);

    qtest_writeq(from.qts, RING, 0);
    qtest_writeq(from.qts, BASE, 0);
    qtest_writeq(from.qts, BASE + 0x10, 32);
    qtest_writeq(from.qts, BASE, 0x200);
    qtest_writeq(from.qts, BASE + 0x20, 0x200);
    peer_set(&from, 0, "receive", 0x47);
    g_assert_cmphex(qtest_readq(from.qts, BASE + 0x30), ==, 0);
    g_assert_false(qtest_readq(from.qts, RING + 0x10) & (1U << 25));
    qtest_writeq(from.qts, BASE + 0x4020, 0x200);
    peer_set(&from, 1, "line-errors", 2);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_true(qtest_readq(to.qts, RING + 0x10) & (1U << 23));
    g_assert_true(qtest_readq(to.qts, RING + 0x10) & (1U << 31));
    for (unsigned offset = 0; offset <= 0x20; offset += 0x20) {
        qtest_writeq(to.qts, BASE + offset, 0);
        qtest_writeq(to.qts, BASE + offset, 0x200);
    }
    peer_set(&to, 0, "receive", 0x63);
    g_assert_cmphex(qtest_readq(to.qts, BASE + 8), ==, 0);
    g_assert_cmphex(qtest_readq(to.qts, BASE + 0x30), ==, 0);
    uart_empty(&to, 0);
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

static void test_migrate_blocked_tx(void)
{
    SerialFixture from = fixture_start();
    SerialFixture to = fixture_start_extra("-incoming defer");
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);
    uint8_t expected[127 * 16], ring[4096] = { 0 };
    int emitted;
    SgiWait wait;

    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = 31 + i * 17;
        packet_pair(ring + i / 16 * 32, i % 16,
                    i ? 0x40 : 0x60, expected[i]);
    }
    ring_write(&from, 0, 0, ring, sizeof(ring));
    qtest_writeq(from.qts, BASE, 0);
    qtest_writeq(from.qts, BASE + 0x10, 0xfe0);
    qtest_writeq(from.qts, BASE, 0x200);
    sgi_wait_start(&wait, "UART blocked before migration");
    for (;;) {
        QDict *reply = qtest_qmp(from.qts,
            "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/peer-a',"
            "'property':'transmit-blocked'}}");
        bool blocked = qdict_get_bool(reply, "return");

        qobject_unref(reply);
        if (blocked) {
            break;
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmpint(ioctl(from.uart[0], FIONREAD, &emitted), ==, 0);
    g_assert_cmpint(emitted, >, 0);
    g_assert_cmpint(emitted, <, sizeof(expected));
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    {
        uint8_t migrated[32];

        g_assert_cmpint(pread(to.fd, migrated, 32, 0x4020), ==, 32);
        g_assert_cmpmem(migrated, 32, ring + 32, 32);
    }
    g_assert_true(qtest_readq(to.qts, RING + 0x10) & (1U << 22));
    qtest_writeq(to.qts, RING + 0x10, ~(1U << 22));
    uart_expect(&to, 0, expected + emitted, sizeof(expected) - emitted);
    pointer_wait(&to, BASE + 8, 0xfe0);
    g_assert_false(qtest_readq(to.qts, RING + 0x10) & (1U << 22));
    uart_empty(&to, 0);
    uart_expect(&from, 0, expected, emitted);
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

static void test_rx_long_input(void)
{
    SerialFixture t = fixture_start();
    uint8_t bytes[40];
    unsigned consumed = 0, position = 0;
    SgiWait wait;

    for (unsigned i = 0; i < sizeof(bytes); i++) {
        bytes[i] = 0x23 + 5 * i;
    }
    qtest_writeb(t.qts, uart_address(0) + 0x200, 0xc7);
    qtest_writeq(t.qts, BASE + 0x20, 0x200);
    g_assert_cmpint(write(t.uart[0], bytes, sizeof(bytes)), ==, sizeof(bytes));
    sgi_wait_start(&wait, "long receive input publication");
    while (consumed < sizeof(bytes)) {
        uint8_t block[32];

        if (qtest_readq(t.qts, BASE + 0x30) == position) {
            sgi_wait_pause(&wait, 1000);
            continue;
        }
        g_assert_cmpint(pread(t.fd, block, 32, 0x5000 + position), ==, 32);
        for (unsigned i = 0; i < 16; i++) {
            unsigned index = i / 4 * 8 + i % 4;

            if (block[index] & 0x80) {
                g_assert_cmpuint(consumed, <, sizeof(bytes));
                g_assert_cmphex(block[index], ==, 0x80);
                g_assert_cmphex(block[index + 4], ==, bytes[consumed++]);
            } else {
                g_assert_cmphex(block[index + 4], ==, 0xff);
            }
        }
        position = (position + 32) & 0xfe0;
        qtest_writeq(t.qts, BASE + 0x28, position);
    }
    g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << 25));
    fixture_stop(&t);
}

static void test_delay_zero_cancel(void)
{
    SerialFixture t = fixture_start();
    uint8_t block[32] = { 0 };

    packet_pair(block, 0, 0xe0, 0);
    ring_write(&t, 1, 0, block, 32);
    qtest_writeq(t.qts, BASE + 0x4000, 0);
    qtest_writeq(t.qts, BASE + 0x4010, 32);
    qtest_writeq(t.qts, BASE + 0x4000, 0x200);
    pointer_wait(&t, BASE + 0x4008, 32);
    g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 28));
    packet_pair(block, 0, 0xe0, 10);
    ring_write(&t, 0, 0, block, 32);
    qtest_writeq(t.qts, BASE, 0);
    qtest_writeq(t.qts, BASE + 0x10, 32);
    qtest_writeq(t.qts, BASE, 0x200);
    qtest_clock_step(t.qts, 4000000);
    qtest_writeq(t.qts, BASE, 0x400);
    qtest_clock_step(t.qts, 20000000);
    g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
    g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 0);
    g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 28));
    fixture_stop(&t);
}

static void test_modem_event_time(void)
{
    for (unsigned transient = 0; transient < 2; transient++) {
        SerialFixture t = fixture_start();
        uint8_t block[32] = { 0 };

        packet_pair(block, 0, 0xe0, 10);
        packet_pair(block, 1, 0x40, 'T');
        ring_write(&t, 0, 0, block, 32);
        qtest_writeb(t.qts, uart_address(0) + 0x100, 8);
        qtest_writeq(t.qts, BASE, 0);
        qtest_writeq(t.qts, BASE + 0x10, 32);
        qtest_writeq(t.qts, BASE, 0x200);
        qtest_clock_step(t.qts, 4000000);
        peer_set(&t, 0, transient ? "modem-transient-pause" :
                 "modem-status-and-advance", (20 << 8) | 0xb1);
        if (!transient) {
            g_assert_cmphex(qtest_readb(t.qts, uart_address(0) + 0x600),
                            ==, 0xb1);
        }
        qtest_clock_step(t.qts, 5000000);
        g_assert_cmphex(qtest_readq(t.qts, BASE + 8), ==, 0);
        g_assert_false(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
        uart_empty(&t, 0);
        qtest_clock_step(t.qts, 1000000);
        pointer_wait(&t, BASE + 8, 32);
        uart_expect(&t, 0, (const uint8_t *)"T", 1);
        g_assert_true(qtest_readq(t.qts, RING + 0x10) & (1U << 22));
        fixture_stop(&t);
    }
}

static void test_uart_gpio_levels(void)
{
    SerialFixture from = fixture_start();
    SerialFixture to = fixture_start_extra("-incoming defer");
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);
    const uint32_t both = (1U << 20) | (1U << 26);

    qtest_writeq(from.qts, RING + 0x18, both);
    for (unsigned p = 0; p < 2; p++) {
        uint32_t bit = 1U << (20 + p * 6);

        qtest_set_irq_in(from.qts, "/machine/peripheral/mace", "uart", p, 1);
        g_assert_true(qtest_readq(from.qts, RING + 0x10) & bit);
        g_assert_true(qtest_get_irq(from.qts, 4));
        /* Idle DMA service must preserve the asserted UART input wire. */
        qtest_writeq(from.qts, BASE + p * 0x4000, 0);
        qtest_writeq(from.qts, BASE + p * 0x4000, 0x400);
        qtest_writeq(from.qts, RING + 0x10, 0);
        g_assert_true(qtest_readq(from.qts, RING + 0x10) & bit);
    }
    qtest_writeq(from.qts, RING + 0x18, 0);
    g_assert_false(qtest_get_irq(from.qts, 4));
    g_assert_cmphex(qtest_readq(from.qts, RING + 0x10) & both, ==, both);
    qtest_writeq(from.qts, RING + 0x18, both);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmphex(qtest_readq(to.qts, RING + 0x10) & both, ==, both);
    g_assert_true(qtest_get_irq(to.qts, 4));
    qtest_set_irq_in(to.qts, "/machine/peripheral/mace", "uart", 0, 0);
    g_assert_cmphex(qtest_readq(to.qts, RING + 0x10) & both, ==, 1U << 26);
    g_assert_true(qtest_get_irq(to.qts, 4));
    qtest_qmp_assert_success(to.qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(to.qts, RING + 0x10) & both, ==, 0);
    g_assert_false(qtest_get_irq(to.qts, 4));
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

static void serial_reset(SerialFixture *t)
{
    for (unsigned p = 0; p < 2; p++) {
        qtest_writeq(t->qts, BASE + p * 0x4000, 0x400);
        qtest_writeq(t->qts, BASE + p * 0x4000 + 0x20, 0x400);
    }
}

static void serial_banks(SerialFixture *t, uint64_t control)
{
    /* Equal selectors leave only bank zero's decoded range populated. */
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(t->qts, 0x14000208 + bank * 8, control);
    }
}

static void test_ring_address_classes(void)
{
    static const uint32_t offsets[] = {
        0, 0x00118000, 0x08008000, 0x1f310000, 0x20008000, 0x3fff8000,
    };
    static const uint32_t classes[] = { 0, 0x40000000, 0x80000000 };

    if (!qtest_has_machine("o2")) {
        g_test_skip("O2 test machine is unavailable");
        return;
    }
    for (unsigned profile = 0; profile < 2; profile++) {
        SerialFixture t = fixture_start_config("",
            profile ? SERIAL_O2_LEGACY : SERIAL_O2_LITERAL);

        for (unsigned o = 0; o < G_N_ELEMENTS(offsets); o++) {
            uint32_t offset = offsets[o];
            off_t backing = offset & 0x07ffffff;
            uint8_t expected[0x8020], actual[sizeof(expected)];

            serial_banks(&t, 0x100 | (offset >> 25 & 0x1c));
            for (unsigned a = 0; a < G_N_ELEMENTS(classes); a++) {
                uint32_t raw = classes[a] + offset;
                bool succeeds = profile || a;

                serial_reset(&t);
                memset(expected, 0xa5, sizeof(expected));
                for (unsigned p = 0; p < 2; p++) {
                    uint8_t *block = expected + 0x4000 + p * 0x2000;

                    memset(block, 0, 32);
                    packet_pair(block, 0, 0x60, 0x35 + p * 0x47);
                    packet_pair(block, 1, 0x40, 0xe2 - p * 0x31);
                }
                g_assert_cmpint(pwrite(t.fd, expected, sizeof(expected),
                                        backing), ==, sizeof(expected));
                qtest_writeq(t.qts, RING, raw);
                g_assert_cmphex(qtest_readq(t.qts, RING), ==, raw);
                for (unsigned p = 0; p < 2; p++) {
                    uint64_t base = BASE + p * 0x4000;
                    uint8_t bytes[] = { 0x35 + p * 0x47, 0xe2 - p * 0x31 };

                    qtest_writeq(t.qts, base, 0);
                    qtest_writeq(t.qts, base + 0x10, 32);
                    qtest_writeq(t.qts, base, 0x200);
                    qtest_writeq(t.qts, base + 0x20, 0x2a0);
                    peer_set(&t, p, "receive", 0x91 - p * 0x56);
                    if (succeeds) {
                        uint8_t *rx = expected + 0x5000 + p * 0x2000;

                        pointer_wait(&t, base + 8, 32);
                        pointer_wait(&t, base + 0x30, 32);
                        uart_expect(&t, p, bytes, sizeof(bytes));
                        for (unsigned word = 0; word < 4; word++) {
                            memset(rx + word * 8, 0, 4);
                            memset(rx + word * 8 + 4, 0xff, 4);
                        }
                        rx[0] = 0x80;
                        rx[4] = 0x91 - p * 0x56;
                        g_assert_true(qtest_readq(t.qts, RING + 0x10) &
                                      (1U << (22 + 6 * p)));
                        g_assert_true(qtest_readq(t.qts, RING + 0x10) &
                                      (1U << (24 + 6 * p)));
                    } else {
                        g_assert_cmphex(qtest_readq(t.qts, base + 8), ==, 0);
                        g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 0);
                        g_assert_true(qtest_readq(t.qts, RING + 0x10) &
                                      (1U << (23 + 6 * p)));
                        g_assert_false(qtest_readq(t.qts, RING + 0x10) &
                                       (1U << (22 + 6 * p)));
                        uart_empty(&t, p);
                    }
                }
                g_assert_cmpint(pread(t.fd, actual, sizeof(actual), backing),
                                ==, sizeof(actual));
                /* Includes audio/scratch pages and unused serial blocks. */
                g_assert_cmpmem(actual, sizeof(actual), expected,
                                sizeof(expected));
                g_assert_cmphex(qtest_readq(t.qts, RING), ==, raw);
                qtest_writeq(t.qts, RING, raw ^ 0x8000);
                g_assert_cmphex(qtest_readq(t.qts, RING), ==, raw);
            }
        }
        fixture_stop(&t);
    }
}

static void test_ring_address_faults(void)
{
    static const uint32_t bases[] = {
        0, 0x40000000, 0x80000000, 0xc0000000, 0xffff8000,
    };

    if (!qtest_has_machine("o2")) {
        g_test_skip("O2 test machine is unavailable");
        return;
    }
    SerialFixture t = fixture_start_config("", SERIAL_O2_LEGACY);

    for (unsigned fault = 0; fault < 3; fault++) {
        /* A hole, a wrong fitted geometry, then a reserved address class. */
        for (unsigned a = fault == 2 ? 3 : 0; a < G_N_ELEMENTS(bases); a++) {
            uint8_t guard[0x8000], actual[sizeof(guard)];

            serial_reset(&t);
            serial_banks(&t, fault == 0 ? 0x104 : fault == 1 ? 0 : 0x100);
            memset(guard, 0x59, sizeof(guard));
            g_assert_cmpint(pwrite(t.fd, guard, sizeof(guard), 0),
                            ==, sizeof(guard));
            qtest_writeq(t.qts, RING, bases[a]);
            for (unsigned p = 0; p < 2; p++) {
                uint64_t base = BASE + p * 0x4000;

                qtest_writeq(t.qts, base, 0);
                qtest_writeq(t.qts, base + 0x10, 0xfe0);
                qtest_writeq(t.qts, base, 0x200);
                qtest_writeq(t.qts, base + 0x20, 0x200);
                peer_set(&t, p, "receive", 0x37 + p * 0x41);
                g_assert_cmphex(qtest_readq(t.qts, base + 8), ==, 0);
                g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 0);
                g_assert_true(qtest_readq(t.qts, RING + 0x10) &
                              (1U << (23 + p * 6)));
                g_assert_false(qtest_readq(t.qts, RING + 0x10) &
                               (1U << (25 + p * 6)));
                qtest_writeq(t.qts, base, 0);
                qtest_writeq(t.qts, base, 0x200);
                qtest_writeq(t.qts, base + 0x20, 0);
                qtest_writeq(t.qts, base + 0x20, 0x200);
                g_assert_cmphex(qtest_readq(t.qts, base + 8), ==, 0);
                g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 0);
                uart_empty(&t, p);
            }
            serial_banks(&t, 0x100);
            for (unsigned p = 0; p < 2; p++) {
                uint64_t base = BASE + p * 0x4000;

                qtest_writeq(t.qts, base, 0);
                qtest_writeq(t.qts, base, 0x200);
                qtest_writeq(t.qts, base + 0x20, 0);
                qtest_writeq(t.qts, base + 0x20, 0x200);
                g_assert_cmphex(qtest_readq(t.qts, base + 8), ==, 0);
                g_assert_cmphex(qtest_readq(t.qts, base + 0x30), ==, 0);
                uart_empty(&t, p);
            }
            g_assert_cmpint(pread(t.fd, actual, sizeof(actual), 0),
                            ==, sizeof(actual));
            g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));
        }
    }
    fixture_stop(&t);
}

static void test_low_ring_wrap(void)
{
    if (!qtest_has_machine("o2")) {
        g_test_skip("O2 test machine is unavailable");
        return;
    }
    SerialFixture t = fixture_start_config("", SERIAL_O2_LEGACY);
    uint8_t ring[4096] = { 0 }, actual[32];
    static const uint32_t bases[] = {
        0, 0x3fff8000, 0x7fff8000, 0xbfff8000,
    };

    for (unsigned a = 0; a < G_N_ELEMENTS(bases); a++) {
        off_t backing = a ? 0x07ff8000 : 0;

        serial_reset(&t);
        serial_banks(&t, a ? 0x11c : 0x100);
        qtest_writeq(t.qts, RING, bases[a]);
        for (unsigned p = 0; p < 2; p++) {
            uint64_t base = BASE + p * 0x4000;
            uint8_t bytes[] = { 0x29 + p, 0xd1 - p };

            memset(ring, 0, sizeof(ring));
            packet_pair(ring, 0, 0x60, bytes[0]);
            packet_pair(ring + 0xfe0, 15, 0x60, bytes[1]);
            g_assert_cmpint(pwrite(t.fd, ring, sizeof(ring),
                                    backing + 0x4000 + p * 0x2000),
                            ==, sizeof(ring));
            qtest_writeq(t.qts, base, 0);
            qtest_writeq(t.qts, base + 0x10, 0xfe0);
            qtest_writeq(t.qts, base, 0x200);
            pointer_wait(&t, base + 8, 0xfe0);
            qtest_writeq(t.qts, base + 0x10, 0);
            pointer_wait(&t, base + 8, 0);
            uart_expect(&t, p, bytes, sizeof(bytes));
            uart_empty(&t, p);
            qtest_writeq(t.qts, base + 0x20, 0x200);
            for (unsigned i = 0; i < 127; i++) {
                peer_set(&t, p, "receive", i ^ (p ? 0xb3 : 0x59));
                pointer_wait(&t, base + 0x30, (i + 1) * 32);
            }
            qtest_writeq(t.qts, base + 0x28, 32);
            peer_set(&t, p, "receive", 0xe7 - p);
            pointer_wait(&t, base + 0x30, 0);
            g_assert_cmpint(pread(t.fd, actual, 32,
                                   backing + 0x5fe0 + p * 0x2000), ==, 32);
            g_assert_cmphex(actual[0], ==, 0x80);
            g_assert_cmphex(actual[4], ==, 0xe7 - p);
        }
    }
    qtest_system_reset(t.qts);
    serial_banks(&t, 0x100);
    qtest_writeq(t.qts, RING, 0);
    ring_write(&t, 0, 0, ring, 32);
    qtest_writeq(t.qts, BASE, 0);
    qtest_writeq(t.qts, BASE + 0x10, 32);
    qtest_writeq(t.qts, BASE, 0x200);
    pointer_wait(&t, BASE + 8, 32);
    uart_expect(&t, 0, (const uint8_t[]){ 0x2a }, 1);
    fixture_stop(&t);
}

static void test_migrate_low_ring(void)
{
    SerialFixture from = fixture_start_config("", SERIAL_OFFSET_STANDALONE);
    SerialFixture to = fixture_start_config("-incoming defer",
                                           SERIAL_OFFSET_STANDALONE);
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);
    uint8_t block[32] = { 0 }, actual[32];

    qtest_writeq(from.qts, RING, 0);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        memset(block, 0, sizeof(block));
        packet_pair(block, 0, 0x40, 0x39 + p);
        packet_pair(block, 1, 0xe0, 10);
        packet_pair(block, 2, 0x40, 0xa3 - p);
        ring_write(&from, p, 0, block, sizeof(block));
        qtest_writeq(from.qts, base, 0);
        qtest_writeq(from.qts, base + 0x10, 32);
        qtest_writeq(from.qts, base, 0x200);
        uart_expect(&from, p, (const uint8_t[]){ 0x39 + p }, 1);
        qtest_writeq(from.qts, base, 0);
        qtest_writeq(from.qts, base + 0x20, 0x200);
        peer_set(&from, p, "receive", 0x72 + p);
        pointer_wait(&from, base + 0x30, 32);
    }
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmphex(qtest_readq(to.qts, RING), ==, 0);
    qtest_clock_step(to.qts, 20 * 1000000);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        uart_empty(&to, p);
        g_assert_cmphex(qtest_readq(to.qts, base + 0x30), ==, 32);
        g_assert_cmpint(pread(to.fd, actual, 32, 0x5000 + p * 0x2000), ==, 32);
        g_assert_cmphex(actual[4], ==, 0x72 + p);
        qtest_writeq(to.qts, base, 0x200);
    }
    qtest_clock_step(to.qts, 9 * 1000000);
    uart_empty(&to, 0);
    uart_empty(&to, 1);
    qtest_clock_step(to.qts, 1000000);
    for (unsigned p = 0; p < 2; p++) {
        uint64_t base = BASE + p * 0x4000;

        pointer_wait(&to, base + 8, 32);
        uart_expect(&to, p, (const uint8_t[]){ 0xa3 - p }, 1);
        uart_empty(&to, p);
        g_assert_cmphex(qtest_readq(to.qts, base + 0x30), ==, 32);
        g_assert_true(qtest_readq(to.qts, RING + 0x10) &
                      (1U << (22 + p * 6)));
    }
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

static void test_migrate_ring_policy_mismatch(void)
{
    SerialFixture from = fixture_start_config("", SERIAL_OFFSET_STANDALONE);
    SerialFixture to = fixture_start_extra("-incoming defer");
    g_autofree char *path = g_build_filename(from.dir, "migration", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", path);
    uint8_t block[32] = { 0 };
    SgiWait wait;

    packet_pair(block, 0, 0xc0, 10);
    packet_pair(block, 1, 0x40, 0x93);
    ring_write(&from, 0, 0, block, sizeof(block));
    qtest_writeq(from.qts, RING, 0);
    qtest_writeq(from.qts, BASE, 0);
    qtest_writeq(from.qts, BASE + 0x10, 32);
    qtest_writeq(from.qts, BASE, 0x200);
    qtest_writeq(from.qts, BASE, 0);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from.qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_start(&wait, "serial ring policy mismatch rejection");
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp_assert_success_ref(to.qts,
            "{'execute':'query-migrate'}");

        if (!strcmp(qdict_get_str(reply, "status"), "failed")) {
            break;
        }
        sgi_wait_pause(&wait, 1000);
    }
    uart_empty(&to, 0);
    g_assert_cmphex(qtest_readq(to.qts, BASE + 8), ==, 0);
    fixture_stop(&to);
    g_unlink(path);
    fixture_stop(&from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace-serial-dma/ring-address-classes",
                   test_ring_address_classes);
    qtest_add_func("/sgi/mace-serial-dma/ring-address-faults",
                   test_ring_address_faults);
    qtest_add_func("/sgi/mace-serial-dma/low-ring-wrap", test_low_ring_wrap);
    qtest_add_func("/sgi/mace-serial-dma/migrate-low-ring",
                   test_migrate_low_ring);
    qtest_add_func("/sgi/mace-serial-dma/migrate-ring-policy-mismatch",
                   test_migrate_ring_policy_mismatch);
    qtest_add_func("/sgi/mace-serial-dma/registers", test_registers);
    qtest_add_func("/sgi/mace-serial-dma/base-validation",
                   test_base_validation);
    qtest_add_func("/sgi/mace-serial-dma/tx-thresholds", test_tx_thresholds);
    qtest_add_func("/sgi/mace-serial-dma/tx-packet-counts",
                   test_tx_packet_counts);
    qtest_add_func("/sgi/mace-serial-dma/delay-freeze", test_delay_freeze);
    qtest_add_func("/sgi/mace-serial-dma/rx-packet", test_rx_packet);
    qtest_add_func("/sgi/mace-serial-dma/dma-faults", test_dma_faults);
    qtest_add_func("/sgi/mace-serial-dma/rx-errors", test_rx_errors);
    qtest_add_func("/sgi/mace-serial-dma/modem-pause", test_modem_pause);
    qtest_add_func("/sgi/mace-serial-dma/rx-full-wrap", test_rx_full_wrap);
    qtest_add_func("/sgi/mace-serial-dma/tx-backpressure",
                   test_tx_backpressure);
    qtest_add_func("/sgi/mace-serial-dma/no-ecc-view", test_no_ecc_view);
    qtest_add_func("/sgi/mace-serial-dma/low-ethernet-alias-isolation",
                   test_low_ethernet_alias_isolation);
    qtest_add_func("/sgi/mace-serial-dma/migrate-delay", test_migrate_delay);
    qtest_add_func("/sgi/mace-serial-dma/migrate-rx-full",
                   test_migrate_rx_full);
    qtest_add_func("/sgi/mace-serial-dma/access-widths", test_access_widths);
    qtest_add_func("/sgi/mace-serial-dma/rx-thresholds", test_rx_thresholds);
    qtest_add_func("/sgi/mace-serial-dma/pair-events-pio",
                   test_pair_events_and_pio);
    qtest_add_func("/sgi/mace-serial-dma/rx-overrun-stops",
                   test_rx_overrun_stops);
    qtest_add_func("/sgi/mace-serial-dma/migrate-faults", test_migrate_faults);
    qtest_add_func("/sgi/mace-serial-dma/migrate-blocked-tx",
                   test_migrate_blocked_tx);
    qtest_add_func("/sgi/mace-serial-dma/rx-long-input", test_rx_long_input);
    qtest_add_func("/sgi/mace-serial-dma/delay-zero-cancel",
                   test_delay_zero_cancel);
    qtest_add_func("/sgi/mace-serial-dma/modem-event-time",
                   test_modem_event_time);
    qtest_add_func("/sgi/mace-serial-dma/uart-gpio-levels",
                   test_uart_gpio_levels);
    return g_test_run();
}
