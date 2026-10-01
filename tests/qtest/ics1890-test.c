/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* ICS1890 register contract through IOC3 management, without guest code. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-sgi-bridge.h"
#include "sgi-migrate.h"
#include <sys/socket.h>

#define NEGOTIATION_NS 100000000
#define RAM 0x02000000

typedef struct PHYTest {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar;
} PHYTest;

static PHYTest *start_addr(const char *extra, int fd, unsigned addr)
{
    PHYTest *t = g_new0(PHYTest, 1);
    g_autofree char *backend = fd >= 0 ?
        g_strdup_printf("-netdev socket,id=n0,fd=%d", fd) :
        g_strdup("-netdev socket,id=n0,listen=127.0.0.1:0");

    t->qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD
        "-display none %s -device sgi-ioc3,id=ioc3,bus=pci,addr=04.0,"
        "netdev=n0,phy-ics1890=on,phy-addr=%u %s", backend, addr, extra);
    t->bus = qpci_init_sgi_bridge(t->qts, NULL);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(4, 0));
    t->bar = qpci_iomap(t->dev, 0, NULL);
    qpci_device_enable(t->dev);
    return t;
}

static PHYTest *start(const char *extra, int fd)
{
    return start_addr(extra, fd, 1);
}

static void stop(PHYTest *t)
{
    g_free(t->dev);
    qpci_free_sgi_bridge(t->bus);
    qtest_quit(t->qts);
    g_free(t);
}

static void system_reset(PHYTest *t)
{
    qtest_system_reset(t->qts);
    g_free(t->dev);
    qpci_free_sgi_bridge(t->bus);
    t->bus = qpci_init_sgi_bridge(t->qts, NULL);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(4, 0));
    t->bar = qpci_iomap(t->dev, 0, NULL);
    qpci_device_enable(t->dev);
}

static void wr(PHYTest *t, unsigned reg, uint32_t val)
{
    qpci_io_writel(t->dev, t->bar, reg, val);
}

static uint32_t rd(PHYTest *t, unsigned reg)
{
    return qpci_io_readl(t->dev, t->bar, reg);
}

static uint16_t mread_addr(PHYTest *t, unsigned addr, unsigned reg)
{
    wr(t, 0x148, (addr << 5) | 0x400 | reg);
    return rd(t, 0x14c);
}

static uint16_t mread(PHYTest *t, unsigned reg)
{
    return mread_addr(t, 1, reg);
}

static void mwrite_addr(PHYTest *t, unsigned addr, unsigned reg, uint16_t val)
{
    wr(t, 0x150, val);
    wr(t, 0x148, (addr << 5) | reg);
}

static void mwrite(PHYTest *t, unsigned reg, uint16_t val)
{
    mwrite_addr(t, 1, reg, val);
}

static void link_set(PHYTest *t, bool up)
{
    qtest_qmp_assert_success(t->qts,
        "{'execute':'set_link','arguments':{'name':'n0','up':%i}}", up);
}

static void test_identity(void)
{
    PHYTest *t = start("", -1);

    for (unsigned addr = 0; addr < 32; addr++) {
        g_assert_cmphex(mread_addr(t, addr, 2), ==,
                       addr == 1 ? 0x0015 : 0xffff);
        g_assert_cmphex(mread_addr(t, addr, 3), ==,
                       addr == 1 ? 0xf423 : 0xffff);
    }
    g_assert_cmphex(mread(t, 0), ==, 0x3000);
    g_assert_cmphex(mread(t, 1), ==, 0x7809);
    g_assert_cmphex(mread(t, 4), ==, 0x01e1);
    g_assert_cmphex(mread(t, 5), ==, 0);
    g_assert_cmphex(mread(t, 6), ==, 0);
    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    g_assert_cmphex(mread(t, 18), ==, 0x0010);
    g_assert_cmphex(mread(t, 19), ==, 0x4001);
    mwrite(t, 0, 0x2000);
    mwrite_addr(t, 31, 0, 0x8000);
    g_assert_cmphex(mread(t, 0), ==, 0x2000);
    mwrite(t, 0, 0x8000);
    stop(t);
}

static void test_masks_override_reset(void)
{
    PHYTest *t = start("", -1);
    static const struct { unsigned reg; uint16_t reset, mask; } cases[] = {
        { 1, 0x7809, 0x7f80 }, { 2, 0x0015, 0xffff },
        { 3, 0xf423, 0xffff }, { 4, 0x01e1, 0x3dff },
        { 6, 0, 0xffe0 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        unsigned reg = cases[i].reg;
        uint16_t ordinary = reg == 4 ? 0x21e0 : 0;

        mwrite(t, reg, 0xffff);
        g_assert_cmphex(mread(t, reg), ==, cases[i].reset | ordinary);
        mwrite(t, 16, 0x8048);
        mread(t, 16); /* Reads must leave override armed. */
        mwrite_addr(t, 31, 2, 0); /* Wrong address cannot consume it. */
        mwrite(t, reg, 0);
        g_assert_cmphex(mread(t, reg), ==,
                       cases[i].reset & ~cases[i].mask);
        mwrite(t, reg, 0xffff);
        g_assert_cmphex(mread(t, reg), ==,
                       (cases[i].reset & ~cases[i].mask) | ordinary);
        mwrite(t, 0, 0x8000);
    }
    mwrite(t, 16, 0x8048);
    mwrite(t, 5, 0); /* An addressed read-only write consumes override. */
    mwrite(t, 2, 0);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    mwrite(t, 16, 0x8048);
    mwrite(t, 16, 0x8048); /* Consume rather than re-arm. */
    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    mwrite(t, 2, 0);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    mwrite(t, 16, 0x8048);
    mwrite(t, 2, 0x1234);
    g_assert_cmphex(mread(t, 2), ==, 0x1234);
    mwrite(t, 18, 0xffff);
    g_assert_cmphex(mread(t, 18), ==, 0x003f);
    mwrite(t, 19, 0xffff);
    g_assert_cmphex(mread(t, 19) & ~0x0400, ==, 0x4011);
    mwrite(t, 16, 0xffff);
    g_assert_cmphex(mread(t, 16), ==, 0x807d);
    mwrite(t, 0, 0xffff);
    g_assert_cmphex(mread(t, 0), ==, 0x3000);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    g_assert_cmphex(mread(t, 18), ==, 0x0010);
    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    mwrite(t, 0, 0x007f);
    g_assert_cmphex(mread(t, 0), ==, 0);
    system_reset(t);
    g_assert_cmphex(mread(t, 0), ==, 0x3000);
    stop(t);
}

static void test_extended_control_reserved_read_bit(void)
{
    PHYTest *t = start("", -1);
    static const struct { uint16_t write, read; } cases[] = {
        { 0, 0x0050 }, { 0x0010, 0x0050 }, { 0x7812, 0x0050 },
        { 0x002d, 0x007d }, { 0x3333, 0x0071 }, { 0x5aa5, 0x0075 },
        { 0x7fff, 0x007d },
    };

    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        mwrite(t, 16, cases[i].write);
        g_assert_cmphex(mread(t, 16), ==, cases[i].read);
    }
    mwrite(t, 16, 0x802d);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(mread(t, 16), ==, 0x807d);
    }
    mwrite_addr(t, 31, 16, 0);
    g_assert_cmphex(mread(t, 16), ==, 0x807d);
    /* Override changes ordinary controls, but cannot store reserved bits. */
    mwrite(t, 16, 0xffff);
    g_assert_cmphex(mread(t, 16), ==, 0x007d);
    mwrite(t, 2, 0);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    mwrite(t, 16, 0x8000);
    g_assert_cmphex(mread(t, 16), ==, 0x8050);
    mwrite(t, 16, 0);
    g_assert_cmphex(mread(t, 16), ==, 0x0050);
    mwrite(t, 16, 0x802d);
    g_assert_cmphex(mread(t, 16), ==, 0x807d);
    mwrite(t, 2, 0x1234);
    g_assert_cmphex(mread(t, 2), ==, 0x1234);
    g_assert_cmphex(mread(t, 16), ==, 0x007d);
    mwrite(t, 0, 0x8000);
    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    mwrite(t, 16, 0x802d);
    system_reset(t);
    g_assert_cmphex(mread(t, 16), ==, 0x0058);
    stop(t);
}

static void test_extended_control_address(void)
{
    static const unsigned addresses[] = { 0, 7, 31 };

    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        unsigned addr = addresses[i];
        PHYTest *t = start_addr("", -1, addr);

        for (unsigned other = 0; other < 32; other++) {
            g_assert_cmphex(mread_addr(t, other, 16), ==,
                           other == addr ? (addr << 6) | 0x18 : 0xffff);
        }
        mwrite_addr(t, addr, 16, 0xffff);
        g_assert_cmphex(mread_addr(t, addr, 16), ==,
                       (addr << 6) | 0x803d);
        mwrite_addr(t, addr, 16, 0);
        g_assert_cmphex(mread_addr(t, addr, 16), ==, (addr << 6) | 0x10);
        stop(t);
    }
}

static void test_negotiation(void)
{
    PHYTest *t = start("", -1);

    link_set(t, false);
    link_set(t, true);
    qtest_clock_step(t->qts, NEGOTIATION_NS - 1);
    for (unsigned i = 0; i < 100; i++) {
        g_assert_cmphex(mread(t, 1) & 0x20, ==, 0);
        g_assert_cmphex(mread(t, 17) & 0x10, ==, 0);
    }
    qtest_clock_step(t->qts, 1);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0x24);
    g_assert_cmphex(mread(t, 17) & 0xf811, ==, 0xf811);
    g_assert_cmphex(mread(t, 5), ==, 0x01e1);
    g_assert_cmphex(mread(t, 6), ==, 3);
    g_assert_cmphex(mread(t, 6), ==, 1);
    link_set(t, false);
    link_set(t, true);
    qtest_clock_step(t->qts, NEGOTIATION_NS);
    g_assert_cmphex(mread(t, 1) & 4, ==, 0);
    g_assert_cmphex(mread(t, 1) & 4, ==, 4);
    g_assert_cmphex(mread(t, 17) & 1, ==, 0);
    g_assert_cmphex(mread(t, 17) & 1, ==, 1);
    for (unsigned mode = 0x100; mode >= 0x20; mode >>= 1) {
        mwrite(t, 4, mode | 1);
        qtest_clock_step(t->qts, NEGOTIATION_NS);
        unsigned speed = mode >= 0x80 ? 0x8000 : 0;
        unsigned duplex = mode == 0x100 || mode == 0x40 ? 0x4000 : 0;
        g_assert_cmphex(mread(t, 17) & 0xc010, ==, speed | duplex | 0x10);
    }
    mwrite(t, 0, 0x2000);
    mread(t, 1); /* Release prior low-link history. */
    g_assert_cmphex(mread(t, 1) & 4, ==, 4);
    mwrite(t, 0, 0x2200); /* Restart has no effect in forced-speed mode. */
    g_assert_cmphex(mread(t, 0), ==, 0x2000);
    g_assert_cmphex(mread(t, 1) & 4, ==, 4);
    mwrite(t, 0, 0x3000);
    mwrite(t, 4, 1); /* No common technology. */
    qtest_clock_step(t->qts, NEGOTIATION_NS * 2);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0);
    mwrite(t, 16, 0x8048);
    mwrite(t, 4, 0x01e2); /* Wrong selector. */
    qtest_clock_step(t->qts, NEGOTIATION_NS * 2);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0);
    mwrite(t, 16, 0x8048);
    mwrite(t, 4, 0x21e1); /* Local fault must prevent completion. */
    qtest_clock_step(t->qts, NEGOTIATION_NS * 2);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0);
    mwrite(t, 4, 0x01e1);
    mwrite(t, 0, 0x3200);
    g_assert_cmphex(mread(t, 0), ==, 0x3000);
    qtest_clock_step(t->qts, NEGOTIATION_NS);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0x20);
    mwrite(t, 0, 0x3200);
    qtest_clock_step(t->qts, NEGOTIATION_NS / 2);
    mwrite(t, 0, 0x8000);
    qtest_clock_step(t->qts, NEGOTIATION_NS / 2);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0);
    qtest_clock_step(t->qts, NEGOTIATION_NS / 2);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0x20);
    stop(t);
}

static void test_powerdown(void)
{
    PHYTest *t = start("", -1);
    uint16_t retained;

    link_set(t, true);
    qtest_clock_step(t->qts, NEGOTIATION_NS);
    mwrite(t, 0, 0x3800);
    g_assert_cmphex(mread(t, 1) & 0x16, ==, 0);
    g_assert_cmphex(mread(t, 6) & 0x12, ==, 0);
    g_assert_cmphex(mread(t, 17) & 0x3fe7, ==, 0);
    g_assert_cmphex(mread(t, 18) & 0x4000, ==, 0);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0x20);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    mwrite(t, 16, 0x8048);
    mwrite(t, 2, 0x1234);
    g_assert_cmphex(mread(t, 2), ==, 0x1234);
    retained = mread(t, 17) & ~0x3fe7;
    link_set(t, false);
    g_assert_cmphex(mread(t, 17) & ~0x3fe7, ==, retained);
    link_set(t, true);
    qtest_clock_step(t->qts, NEGOTIATION_NS * 2);
    g_assert_cmphex(mread(t, 17) & 0x3fe7, ==, 0);
    mwrite(t, 0, 0x3000);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0);
    qtest_clock_step(t->qts, NEGOTIATION_NS - 1);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0);
    qtest_clock_step(t->qts, 1);
    g_assert_cmphex(mread(t, 1) & 0x20, ==, 0x20);
    mwrite(t, 0, 0x3200);
    qtest_clock_step(t->qts, NEGOTIATION_NS / 2);
    mwrite(t, 0, 0x3800);
    qtest_clock_step(t->qts, NEGOTIATION_NS * 2);
    g_assert_cmphex(mread(t, 1) & 0x24, ==, 0);
    mwrite(t, 18, 0x12); /* Link-loss inhibit cannot defeat power-down. */
    g_assert_cmphex(mread(t, 1) & 4, ==, 0);
    stop(t);
}

static void store32(PHYTest *t, uint64_t addr, uint32_t val)
{
    uint8_t buf[] = { val >> 24, val >> 16, val >> 8, val };
    qtest_memwrite(t->qts, addr, buf, sizeof(buf));
}

static void packet_setup(PHYTest *t)
{
    wr(t, 0xf0, (1 << 1) | (33 << 3) | (0xf << 13));
    wr(t, 0x100, 0);
    wr(t, 0x104, RAM + 0x8000);
    wr(t, 0x128, 0);
    wr(t, 0x12c, RAM);
    store32(t, RAM + 0x8000, 0);
    store32(t, RAM + 0x8004, RAM + 0xc000);
    wr(t, 0x110, 0x80000008);
}

static void transmit(PHYTest *t, unsigned slot, const uint8_t *frame,
                     size_t len)
{
    uint64_t desc = RAM + slot * 128;

    store32(t, desc, len | (1 << 16));
    store32(t, desc + 4, len);
    qtest_memwrite(t->qts, desc + 24, frame, len);
    wr(t, 0x134, (slot + 1) * 128);
}

static void socket_timeout(int fd, unsigned seconds)
{
    struct timeval tv = { .tv_sec = seconds };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static void socket_expect_frame(int fd, const uint8_t *frame, size_t len)
{
    uint32_t header;
    uint8_t got[60];

    socket_timeout(fd, 10);
    g_assert_cmpint(recv(fd, &header, sizeof(header), MSG_WAITALL), ==,
                    sizeof(header));
    g_assert_cmpuint(ntohl(header), ==, len);
    g_assert_cmpuint(len, <=, sizeof(got));
    g_assert_cmpint(recv(fd, got, len, MSG_WAITALL), ==, len);
    g_assert_cmpmem(got, len, frame, len);
}

static void socket_expect_no_frame(int fd)
{
    uint8_t byte;

    socket_timeout(fd, 1);
    g_assert_cmpint(recv(fd, &byte, sizeof(byte), 0), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void test_loopback(gconstpointer data)
{
    uint16_t control = GPOINTER_TO_UINT(data);
    bool loops = (control & 0x4000) && !(control & 0x0c00);
    int sv[2];
    uint8_t frame[60], got[60];
    PHYTest *t;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    t = start("", sv[1]);
    close(sv[1]);
    link_set(t, false);
    packet_setup(t);
    mwrite(t, 0, control);
    mwrite(t, 18, 0x10); /* This reserved bit must never select loopback. */
    memset(frame, 0x55, sizeof(frame));
    memset(frame, 0xff, 6);
    transmit(t, 0, frame, sizeof(frame));
    qtest_clock_step(t->qts, 1000000);
    g_assert_cmphex(rd(t, 0x10c), ==, loops ? 8 : 0);
    if (loops) {
        qtest_memread(t->qts, RAM + 0xc000 + 66, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    }
    socket_expect_no_frame(sv[0]);
    stop(t);
    close(sv[0]);
}

static void test_connected_packet_control(gconstpointer data)
{
    uint16_t control = GPOINTER_TO_UINT(data);
    bool loops = (control & 0x4000) && !(control & 0x0c00);
    int sv[2];
    uint8_t frame[60], got[60];
    PHYTest *t;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    t = start("", sv[1]);
    close(sv[1]);
    link_set(t, true);
    packet_setup(t);

    /* Successful output on each side makes socket silence meaningful. */
    mwrite(t, 0, 0x2000);
    memset(frame, 0x11, sizeof(frame));
    memset(frame, 0xff, 6);
    transmit(t, 0, frame, sizeof(frame));
    socket_expect_frame(sv[0], frame, sizeof(frame));
    g_assert_cmphex(rd(t, 0x10c), ==, 0);

    mwrite(t, 0, control);
    g_assert_cmphex(mread(t, 0), ==, control);
    g_assert_cmphex(mread(t, 2), ==, 0x0015);
    g_assert_cmphex(mread(t, 3), ==, 0xf423);
    memset(frame + 6, 0x22, sizeof(frame) - 6);
    transmit(t, 1, frame, sizeof(frame));
    qtest_clock_step(t->qts, 1000000);
    g_assert_cmphex(rd(t, 0x10c), ==, loops ? 8 : 0);
    if (loops) {
        qtest_memread(t->qts, RAM + 0xc000 + 66, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    }
    socket_expect_no_frame(sv[0]);

    mwrite(t, 0, 0x2000);
    g_assert_cmphex(mread(t, 0), ==, 0x2000);
    memset(frame + 6, 0x33, sizeof(frame) - 6);
    transmit(t, 2, frame, sizeof(frame));
    socket_expect_frame(sv[0], frame, sizeof(frame));
    g_assert_cmphex(rd(t, 0x10c), ==, loops ? 8 : 0);
    stop(t);
    close(sv[0]);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("ics1890-migration-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    PHYTest *from = start("-S", -1);
    PHYTest *to = start("-S -incoming defer", -1);
    int64_t source_clock;

    link_set(from, true);
    source_clock = qtest_clock_step(from->qts, NEGOTIATION_NS / 2);
    mwrite(from, 16, 0x802d);
    g_assert_cmphex(mread(from, 16), ==, 0x807d);
    qtest_qmp_assert_success(to->qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from->qts, uri);
    sgi_wait_migration_status(to->qts, "completed", 30);
    /* qtest's test-owned clock is not migrated with absolute device timers. */
    qtest_clock_set(to->qts, source_clock);
    /* Resume timer callbacks. The qtest accelerator executes no guest code. */
    qtest_qmp_assert_success(to->qts, "{'execute':'cont'}");
    g_assert_cmphex(mread(to, 16), ==, 0x807d);
    mwrite(to, 2, 0x1234);
    g_assert_cmphex(mread(to, 2), ==, 0x1234);
    g_assert_cmphex(mread(to, 16), ==, 0x007d);
    g_assert_cmphex(mread(to, 1) & 0x20, ==, 0);
    qtest_clock_step(to->qts, NEGOTIATION_NS / 2 - 1);
    g_assert_cmphex(mread(to, 1) & 0x20, ==, 0);
    qtest_clock_step(to->qts, 1);
    g_assert_cmphex(mread(to, 1) & 0x20, ==, 0x20);
    mwrite(to, 0, 0x8000);
    g_assert_cmphex(mread(to, 2), ==, 0x0015);
    g_assert_cmphex(mread(to, 16), ==, 0x0058);
    stop(to);
    stop(from);
    unlink(socket);
    rmdir(dir);
}

static void test_conflicting_selection(void)
{
    if (g_test_subprocess()) {
        PHYTest *t = start("-global sgi-ioc3.phy-dp83843=on", -1);
        stop(t);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*cannot select ICS1890 and DP83843 together*");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ics1890/identity", test_identity);
    qtest_add_func("/ics1890/masks-override-reset", test_masks_override_reset);
    qtest_add_func("/ics1890/extended-control-reserved-read-bit",
                   test_extended_control_reserved_read_bit);
    qtest_add_func("/ics1890/extended-control-address",
                   test_extended_control_address);
    qtest_add_func("/ics1890/negotiation", test_negotiation);
    qtest_add_func("/ics1890/powerdown", test_powerdown);
    qtest_add_data_func("/ics1890/loopback-10", GUINT_TO_POINTER(0x4000),
                       test_loopback);
    qtest_add_data_func("/ics1890/loopback-100", GUINT_TO_POINTER(0x6000),
                       test_loopback);
    qtest_add_data_func("/ics1890/loopback-isolated", GUINT_TO_POINTER(0x4400),
                       test_loopback);
    qtest_add_data_func("/ics1890/loopback-powered-down",
                       GUINT_TO_POINTER(0x4800), test_loopback);
    qtest_add_data_func("/ics1890/reserved-bit-is-not-loopback",
                       GUINT_TO_POINTER(0), test_loopback);
    qtest_add_data_func("/ics1890/connected-loopback-10",
                       GUINT_TO_POINTER(0x4000), test_connected_packet_control);
    qtest_add_data_func("/ics1890/connected-loopback-100",
                       GUINT_TO_POINTER(0x6000), test_connected_packet_control);
    qtest_add_data_func("/ics1890/connected-loopback-isolated",
                       GUINT_TO_POINTER(0x4400), test_connected_packet_control);
    qtest_add_data_func("/ics1890/connected-loopback-powered-down",
                       GUINT_TO_POINTER(0x4800), test_connected_packet_control);
    qtest_add_func("/ics1890/conflicting-selection",
                   test_conflicting_selection);
    qtest_add_func("/ics1890/migration", test_migration);
    return g_test_run();
}
