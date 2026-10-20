/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE 2.0 Ethernet register and packet DMA contracts. */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-wait.h"
#include "sgi-migrate.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <glib/gstdio.h>

#define ETH_BASE UINT64_C(0x1f280000)
#define ETH_REPEAT (ETH_BASE + 0x7fe00)
#define MAC_CONTROL 0x00
#define DMA_CONTROL 0x10
#define STATION 0xa0
#define MDIO_DATA 0x60
#define MDIO_ADDRESS 0x68
#define MDIO_READ 0x70
#define RX_DELAY 0x18
#define TX_INT_ALIAS 0x20
#define RX_INT_ALIAS 0x28
#define STATION_SECONDARY 0xa8
#define MULTICAST_MASK 0xb0
#define TX_POINTER 0x30
#define TX_BASE 0xb8
#define INT_STATUS 0x08
#define RX_FIFO_INFO 0x40
#define RX_FIFO_PORT 0x100
#define CRIME_STATUS UINT64_C(0x14000010)
#define CRIME_ENABLE UINT64_C(0x14000018)
#define RAM_FULL UINT64_C(0x40000000)
#define RAM_NO_ECC UINT64_C(0x80000000)

static void test_register_reset_and_alias(void)
{
    QTestState *qts = qtest_init("-M o2-test -nic none");

    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL), ==,
                    UINT64_C(0x20000001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==, 0);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    g_assert_cmphex(qtest_readq(qts, ETH_REPEAT + STATION), ==,
                    UINT64_C(0x02a14c7395e6));
    qtest_quit(qts);
}

static void test_selected_word_control_mdio(void)
{
    QTestState *qts = qtest_init("-M o2-test,phy-ics1890=on,phy-address=8 "
                                 "-nic none");

    g_assert_cmphex(qtest_readl(qts, ETH_BASE + MAC_CONTROL + 4), ==,
                    UINT32_C(0x20000001));
    qtest_writel(qts, ETH_BASE + MAC_CONTROL + 4, 0x0a);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL), ==,
                    UINT64_C(0x2000000a));
    qtest_writel(qts, ETH_BASE + MAC_CONTROL + 4, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL), ==,
                    UINT64_C(0x20000001));

    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4, 4U << 12);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + DMA_CONTROL + 4), ==,
                    4U << 12);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==,
                    4U << 12);
    qtest_writel(qts, ETH_BASE + TX_INT_ALIAS + 4, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==,
                    (4U << 12) | 1);
    qtest_writel(qts, ETH_BASE + TX_INT_ALIAS + 4, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==,
                    4U << 12);
    qtest_writel(qts, ETH_BASE + RX_DELAY + 4, 0x2a);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_DELAY), ==, 0x2a);

    qtest_writel(qts, ETH_BASE + MDIO_ADDRESS + 4, (8U << 5) | 2);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_ADDRESS), ==,
                    (8U << 5) | 2);
    qtest_writel(qts, ETH_BASE + MDIO_READ + 4, UINT32_MAX);
    g_assert_true(qtest_readl(qts, ETH_BASE + MDIO_DATA + 4) & (1U << 16));
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + MDIO_DATA + 4), ==, 0x0015);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_DATA), ==, 0x0015);

    qtest_writel(qts, ETH_BASE + MDIO_ADDRESS + 4, 8U << 5);
    qtest_writel(qts, ETH_BASE + MDIO_DATA + 4, 0x2100);
    qtest_clock_step(qts, 1000);
    qtest_writel(qts, ETH_BASE + MDIO_READ + 4, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + MDIO_DATA + 4), ==, 0x2100);

    qtest_writel(qts, ETH_BASE + MDIO_ADDRESS + 4, (7U << 5) | 2);
    qtest_writel(qts, ETH_BASE + MDIO_READ + 4, 0);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + MDIO_DATA + 4), ==, 0xffff);
    qtest_quit(qts);
}

static void test_selected_word_status_ack(void)
{
    QTestState *qts = qtest_init("-M o2-test -nic none");

    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 0);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 (1U << 15) | (1U << 9) | (1U << 4));
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 5));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writeq(qts, CRIME_ENABLE, 8);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 0);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 5));
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1U << 5);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 5));

    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 (1U << 15) | (1U << 9));
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1U << 5);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 5));
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

static void test_selected_pointer_fifo_pio(void)
{
    QTestState *qts = qtest_init("-M o2-test -nic none");

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 1U << 2);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 0x57);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0x57);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 4, 0x1234);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0x57);

    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4,
                 RAM_FULL + 0x8123);
    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 0xfc,
                 RAM_FULL + 0x9007);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x20002));
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 6), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x20002));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT + 0xf8), ==,
                    RAM_FULL + 0x8000);
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 6), ==, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT), ==,
                    RAM_FULL + 0x9000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x20200));
    qtest_quit(qts);
}

static void test_tx_pointer_readback(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-tx-pointer-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts = qtest_initf("-M o2-test -nic none -d unimp -D %s",
                                  log_path);

    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0);

    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 0x2d);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0x2d);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0x2d);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==,
                    0x2d);

    qtest_writew(qts, ETH_BASE + TX_POINTER + 4, 0x1234);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 2, 0x1234);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 14, 0x1234);
    qtest_writeb(qts, ETH_BASE + TX_POINTER + 6, 0x12);
    qtest_writel(qts, ETH_BASE + TX_POINTER + 4, 0x12345678);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0x2d);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0x2d);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==,
                    0x2d);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 2), ==, 0);

    qtest_writeq(qts, ETH_BASE + TX_POINTER, 0x19);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0x19);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==,
                    0x19);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0x32 size 2"));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0x3e size 2"));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0x36 size 1"));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0x34 size 4"));
    g_assert_nonnull(strstr(log_text, "unsupported read at 0x32 size 2"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_selected_fifo_generation(void)
{
    QTestState *qts = qtest_init("-M o2-test -nic none");

    for (unsigned i = 0; i < 16; i++) {
        qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4 + (i % 2) * 0xf8,
                     RAM_FULL + 0x8000 + i * 0x1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x100010));
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 6), ==, 0);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT), ==,
                        RAM_FULL + 0x8000 + i * 0x1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x101000));
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 6), ==,
                    0x10);
    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4,
                 RAM_FULL + 0x18000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x111001));
    qtest_quit(qts);
}

static void test_selected_width_refusals(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-eth-word-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts = qtest_initf("-M o2-test -nic none -d unimp -D %s",
                                  log_path);

    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writel(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeb(qts, ETH_BASE + MAC_CONTROL + 4, 0);
    qtest_writew(qts, ETH_BASE + MAC_CONTROL + 4, 0);
    qtest_writel(qts, ETH_REPEAT + MAC_CONTROL + 4, 0);
    qtest_writel(qts, ETH_BASE + RX_INT_ALIAS + 4, UINT32_MAX);
    qtest_writel(qts, ETH_BASE + STATION + 4, 0);
    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL), ==,
                    UINT64_C(0x20000001));
    g_assert_cmphex(qtest_readq(qts, ETH_REPEAT + MAC_CONTROL), ==,
                    UINT64_C(0x20000001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + STATION), ==,
                    UINT64_C(0x02a14c7395e6));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + INT_STATUS + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 6), ==, 0);
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 5), ==, 0);
    qtest_writel(qts, ETH_BASE + 0x7c, 0x1234);
    qtest_writeq(qts, ETH_BASE + 0x78, 0x0456);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0x0 size 4"));
    g_assert_nonnull(strstr(log_text,
                            "unsupported write at 0x7fe04 size 4"));
    g_assert_nonnull(strstr(log_text,
                            "unsupported read at 0xc size 4"));
    g_assert_nonnull(strstr(log_text,
                            "collision backoff seed 0x234"));
    g_assert_nonnull(strstr(log_text,
                            "collision backoff seed 0x456"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static uint16_t mdio_read(QTestState *qts, unsigned phy, unsigned reg)
{
    qtest_writeq(qts, ETH_BASE + MDIO_ADDRESS, (phy << 5) | reg);
    qtest_writeq(qts, ETH_BASE + MDIO_READ, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_DATA) & (1U << 16),
                    ==, 1U << 16);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_DATA) & (1U << 16),
                    ==, 0);
    return qtest_readq(qts, ETH_BASE + MDIO_DATA) & 0xffff;
}

static void mdio_write(QTestState *qts, unsigned phy, unsigned reg,
                       uint16_t value)
{
    qtest_writeq(qts, ETH_BASE + MDIO_ADDRESS, (phy << 5) | reg);
    qtest_writeq(qts, ETH_BASE + MDIO_DATA, value);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_DATA) & (1U << 16),
                    ==, 1U << 16);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MDIO_DATA) & (1U << 16),
                    ==, 0);
}

static void test_mdio_selected_address(void)
{
    QTestState *qts = qtest_init("-M o2-test -nic none");

    g_assert_cmphex(mdio_read(qts, 8, 2), ==, 0xffff);
    qtest_quit(qts);

    qts = qtest_init("-M o2-test,phy-ics1890=on,phy-address=8 "
                     "-nic none");
    g_assert_cmphex(mdio_read(qts, 8, 2), ==, 0x0015);
    g_assert_cmphex(mdio_read(qts, 8, 3), ==, 0xf423);
    g_assert_cmphex(mdio_read(qts, 7, 2), ==, 0xffff);
    mdio_write(qts, 7, 0, 0x2100);
    g_assert_cmphex(mdio_read(qts, 8, 0), ==, 0x3000);
    mdio_write(qts, 8, 0, 0x2100);
    g_assert_cmphex(mdio_read(qts, 8, 0), ==, 0x2100);
    qtest_quit(qts);

    qts = qtest_init("-M o2-test,phy-ics1890=on,phy-address=0 -nic none");
    g_assert_cmphex(mdio_read(qts, 0, 0), ==, 0x3400);
    g_assert_cmphex(mdio_read(qts, 1, 2), ==, 0xffff);
    qtest_quit(qts);
}

static void socket_timeout(int fd)
{
    struct timeval timeout = { .tv_sec = 10 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)), ==, 0);
}

static void test_tx_inline_raw(void)
{
    static const uint8_t command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0x00, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x3c
    };
    uint8_t frame[60], got[60], landed[8];
    uint32_t wire_length;
    const uint64_t descriptor = RAM_FULL + 0x4000;
    QTestState *qts;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i * 7 + 3;
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
                      "mac=02:a1:4c:73:95:e6", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, descriptor, command, sizeof(command));
    qtest_memwrite(qts, descriptor + 68, frame, sizeof(frame));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);

    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(frame));
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(qts, descriptor, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_concat_cross_page(void)
{
    static const uint8_t command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x03, 0x3a, 0x00, 0x95
    };
    static const uint8_t pointer[8] = {
        0x00, 0x00, 0x00, 0x4f, 0x40, 0x00, 0xaf, 0xf8
    };
    static const uint8_t status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x96
    };
    const uint64_t descriptor = RAM_FULL + 0x6000;
    const uint64_t payload = RAM_FULL + 0xaff8;
    uint8_t frame[150], got[150], landed[8];
    uint32_t wire_length;
    QTestState *qts;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i ^ 0x5a;
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
                      "mac=02:a1:4c:73:95:e6", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 8);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, descriptor, command, sizeof(command));
    qtest_memwrite(qts, descriptor + 8, pointer, sizeof(pointer));
    qtest_memwrite(qts, descriptor + 58, frame, 70);
    qtest_memwrite(qts, payload, frame + 70, 80);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(frame));
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(qts, descriptor, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0x10001);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 3, ==, 3);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 3);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 1);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writel(qts, ETH_BASE + TX_INT_ALIAS + 4, 0);
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 3);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_two_concat_and_hole(void)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x07, 0x38, 0, 0x95
    };
    static const uint8_t pointer1[8] = {
        0, 0, 0, 0x0f, 0x40, 0, 0xaf, 0xf0
    };
    static const uint8_t pointer2[8] = {
        0, 0, 0, 0x3d, 0x40, 0, 0xb0, 0
    };
    static const uint8_t pointer2_bad[8] = {
        0, 0, 0, 0x3d, 0x48, 0, 0x10, 0
    };
    static const uint8_t done[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x96
    };
    const uint64_t descriptor = RAM_FULL + 0x4000;
    const uint64_t payload1 = RAM_FULL + 0xaff0;
    const uint64_t payload2 = RAM_FULL + 0xb000;
    uint8_t frame[150], entry[128] = { 0 }, got[150], landed[8];
    uint8_t guard[16], wire_guard[16];
    uint32_t wire_length;
    QTestState *qts;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i * 7 + 0x31;
    }
    memset(guard, 0xcc, sizeof(guard));
    memcpy(entry, command, sizeof(command));
    memcpy(entry + 8, pointer1, sizeof(pointer1));
    memcpy(entry + 16, pointer2, sizeof(pointer2));
    memcpy(entry + 56, frame, 72);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 8);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, descriptor, entry, sizeof(entry));
    qtest_memwrite(qts, payload1 - sizeof(guard), guard, sizeof(guard));
    qtest_memwrite(qts, payload1, frame + 72, 16);
    qtest_memwrite(qts, payload2, frame + 88, 62);
    qtest_memwrite(qts, payload2 + 62, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4, 3);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 1);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(frame));
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(qts, descriptor, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), done, sizeof(done));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 3, ==, 3);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writel(qts, ETH_BASE + TX_INT_ALIAS + 4, 0);
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 3);
    g_assert_false(qtest_get_irq(qts, 0));
    memcpy(entry + 16, pointer2_bad, sizeof(pointer2_bad));
    qtest_memwrite(qts, descriptor + 128, entry, sizeof(entry));
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 2);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 3));
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_memread(qts, descriptor + 128, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), command, sizeof(command));
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);
    qtest_memread(qts, payload1 - sizeof(guard), wire_guard,
                  sizeof(wire_guard));
    g_assert_cmpmem(wire_guard, sizeof(wire_guard), guard, sizeof(guard));
    qtest_memread(qts, payload2 + 62, wire_guard, sizeof(wire_guard));
    g_assert_cmpmem(wire_guard, sizeof(wire_guard), guard, sizeof(guard));
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1U << 3);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 3));
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void socket_send_frame(int fd, const uint8_t *frame, size_t length)
{
    uint32_t header = htonl(length);

    g_assert_cmpint(send(fd, &header, sizeof(header), 0), ==,
                    sizeof(header));
    g_assert_cmpint(send(fd, frame, length, 0), ==, length);
}

static void wait_fifo_count(QTestState *qts, unsigned count)
{
    SgiWait wait;

    sgi_wait_start(&wait, "MACE receive FIFO count %u", count);
    while ((qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31) != count) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
}

static void wait_crime_ethernet_irq(QTestState *qts)
{
    SgiWait wait;

    sgi_wait_start(&wait, "MACE Ethernet CRIME bit 3");
    while (!(qtest_readq(qts, CRIME_STATUS) & 8)) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
}

static void test_rx_filter_and_raw_dma(void)
{
    static const uint8_t station[6] = {
        0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6
    };
    static const uint8_t source[6] = {
        0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc
    };
    static const uint8_t expected_status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x41
    };
    static const uint8_t expected_fcs[4] = { 0x8e, 0x07, 0x70, 0x45 };
    static const uint8_t broadcast_fcs[4] = { 0xc4, 0x5d, 0x6b, 0xcb };
    const uint64_t rx0 = RAM_FULL + 0x8000;
    const uint64_t rx1 = RAM_FULL + 0x9000;
    uint8_t frame[61], reversed[61], broadcast[61];
    uint8_t old_buffer[256], landed[64], status[8], fcs[4];
    g_autofree char *dir = g_dir_make_tmp("mace-eth-rx-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts;
    int sv[2];

    memset(old_buffer, 0xa5, sizeof(old_buffer));
    memcpy(frame, station, sizeof(station));
    memcpy(frame + 6, source, sizeof(source));
    frame[12] = 0x08;
    frame[13] = 0;
    for (unsigned i = 14; i < sizeof(frame); i++) {
        frame[i] = (i - 14) * 7 + 3;
    }
    memcpy(reversed, frame, sizeof(frame));
    for (unsigned i = 0; i < 6; i++) {
        reversed[i] = station[5 - i];
    }
    memcpy(broadcast, frame, sizeof(frame));
    memset(broadcast, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
                      "mac=02:a1:4c:73:95:e6 -d unimp -D %s",
                      sv[1], log_path);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, rx0, old_buffer, sizeof(old_buffer));
    qtest_memwrite(qts, rx1, old_buffer, sizeof(old_buffer));
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, rx0);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT + 0xf8, rx1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x20002));
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | (1U << 9) | (2U << 4));
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);

    socket_send_frame(sv[0], reversed, sizeof(reversed));
    socket_send_frame(sv[0], broadcast, sizeof(broadcast));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_crime_ethernet_irq(qts);
    qtest_memread(qts, rx0, status, sizeof(status));
    /* The partial-checksum field has no cleared byte-pairing rule yet. */
    g_assert_cmpmem(status, 2, expected_status, 2);
    g_assert_cmpmem(status + 4, 4, expected_status + 4, 4);
    g_assert_cmphex(status[2] | status[3], ==, 0);
    qtest_memread(qts, rx0 + 32, landed, 2);
    g_assert_cmphex(landed[0] | landed[1], ==, 0);
    qtest_memread(qts, rx0 + 34, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));
    qtest_memread(qts, rx0 + 95, fcs, sizeof(fcs));
    g_assert_cmpmem(fcs, sizeof(fcs), expected_fcs, sizeof(expected_fcs));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x20101));
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1U << 5);
    socket_send_frame(sv[0], broadcast, sizeof(broadcast));
    wait_fifo_count(qts, 0);
    qtest_memread(qts, rx1 + 34, landed, sizeof(broadcast));
    g_assert_cmpmem(landed, sizeof(broadcast), broadcast,
                    sizeof(broadcast));
    qtest_memread(qts, rx1 + 95, fcs, sizeof(fcs));
    g_assert_cmpmem(fcs, sizeof(fcs), broadcast_fcs,
                    sizeof(broadcast_fcs));
    qtest_memread(qts, rx1 + 2, status, 2);
    g_assert_cmphex(status[0] | status[1], ==, 0);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 1U << 5);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | (1U << 9));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 1U << 5);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    {
        const char *needle = "RX partial checksum is unsupported";
        unsigned reports = 0;
        const char *cursor = log_text;

        while ((cursor = strstr(cursor, needle))) {
            reports++;
            cursor += strlen(needle);
        }
        g_assert_cmpuint(reports, ==, 1);
    }
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_fifo_dma_wrap(void)
{
    static const uint64_t base = RAM_FULL + 0x8000;
    uint8_t frames[16][60], old_buffer[256], landed[60], status[8];
    uint8_t guard[16];
    QTestState *qts;
    int sv[2];

    memset(old_buffer, 0xa5, sizeof(old_buffer));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    mdio_write(qts, 8, 0, 0x2100);
    for (unsigned slot = 0; slot < 16; slot++) {
        uint64_t address = base + slot * 0x1000;

        qtest_memwrite(qts, address, old_buffer, sizeof(old_buffer));
        qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4 +
                     (slot % 2) * 0xf8, address);
        frames[slot][0] = 0x02;
        frames[slot][1] = 0x20;
        frames[slot][2] = 0x30;
        frames[slot][3] = 0x40;
        frames[slot][4] = 0x50;
        frames[slot][5] = slot;
        for (unsigned i = 6; i < sizeof(frames[slot]); i++) {
            frames[slot][i] = slot * 13 + i * 5;
        }
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x100010));
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 (1U << 15) | (1U << 12) | (1U << 9) | (16U << 4));
    g_assert_false(qtest_get_irq(qts, 0));
    socket_send_frame(sv[0], frames[0], sizeof(frames[0]));
    wait_crime_ethernet_irq(qts);
    qtest_memread(qts, base, status, sizeof(status));
    g_assert_true(status[0] & 0x80);
    g_assert_cmphex(status[7], ==, 64);
    qtest_memread(qts, base + 10, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frames[0], sizeof(frames[0]));

    for (unsigned slot = 1; slot < 16; slot++) {
        socket_send_frame(sv[0], frames[slot], sizeof(frames[slot]));
    }
    wait_fifo_count(qts, 0);
    for (unsigned slot = 0; slot < 16; slot++) {
        uint64_t address = base + slot * 0x1000;

        qtest_memread(qts, address, status, sizeof(status));
        g_assert_true(status[0] & 0x80);
        g_assert_cmphex(status[4] >> 3, ==, slot);
        g_assert_cmphex(status[7], ==, 64);
        qtest_memread(qts, address + 10, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), frames[slot],
                        sizeof(frames[slot]));
        qtest_memread(qts, address + 74, guard, sizeof(guard));
        g_assert_cmpmem(guard, sizeof(guard), old_buffer,
                        sizeof(guard));
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x101000));
    g_assert_cmphex(qtest_readb(qts, ETH_BASE + RX_FIFO_INFO + 6), ==,
                    0x10);
    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4, base + 0x10000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x111001));
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1U << 5);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 (1U << 15) | (1U << 12) | (1U << 9) | (1U << 4));
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1U << 5);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void rx_sequence_frame(uint8_t frame[60], unsigned sequence)
{
    static const uint8_t station[6] = {
        0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6
    };

    memcpy(frame, station, sizeof(station));
    frame[6] = 0x12;
    frame[7] = 0x34;
    frame[8] = 0x56;
    frame[9] = 0x78;
    frame[10] = sequence >> 8;
    frame[11] = sequence;
    frame[12] = 0x88;
    frame[13] = 0xb5;
    for (unsigned i = 14; i < 60; i++) {
        frame[i] = sequence * 19 + i * 5;
    }
}

static void test_rx_sustained_fifo_replenish(void)
{
    static const uint64_t refill_info[2] = {
        UINT64_C(0x100010), UINT64_C(0x001010)
    };
    static const uint64_t drained_info[2] = {
        0, UINT64_C(0x101000)
    };
    const uint64_t base = RAM_FULL + 0x80000;
    uint8_t frames[16][60], old_buffer[256], landed[60], status[8];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-sustained-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts;
    int sv[2];

    memset(old_buffer, 0xa5, sizeof(old_buffer));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
                      "mac=02:a1:4c:73:95:e6 -d guest_errors -D %s",
                      sv[1], log_path);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));

    for (unsigned sent = 0; sent < 524;) {
        unsigned count = MIN(16U, 524U - sent);
        unsigned done = sent + count;

        for (unsigned i = 0; i < count; i++) {
            uint64_t address = base + i * 0x1000;

            qtest_memwrite(qts, address, old_buffer, sizeof(old_buffer));
            qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, address);
            rx_sequence_frame(frames[i], sent + i);
        }
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                        count == 16 ? refill_info[(sent / 16) & 1] :
                                      UINT64_C(0x0c000c));
        for (unsigned i = 0; i < count; i++) {
            socket_send_frame(sv[0], frames[i], sizeof(frames[i]));
        }
        wait_fifo_count(qts, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                        done == 524 ? UINT64_C(0x0c0c00) :
                                      drained_info[(done / 16) & 1]);
        for (unsigned i = 0; i < count; i++) {
            uint64_t address = base + i * 0x1000;
            unsigned sequence = sent + i;

            qtest_memread(qts, address, status, sizeof(status));
            g_assert_cmphex(status[0], ==, 0x80);
            g_assert_cmphex(status[1] | status[2] | status[3], ==, 0);
            g_assert_cmphex(status[4], ==,
                            ((sequence & 31) << 3) | 0x04);
            g_assert_cmphex(status[5] | status[6], ==, 0);
            g_assert_cmphex(status[7], ==, 64);
            qtest_memread(qts, address + 34, landed, sizeof(landed));
            g_assert_cmpmem(landed, sizeof(landed), frames[i],
                            sizeof(frames[i]));
        }
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0xc0, ==,
                        0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) &
                        (1U << 15), ==, 1U << 15);
        sent = done;
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_null(strstr(log_text, "RX address FIFO overrun"));
    g_assert_null(strstr(log_text, "RX DMA failed"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_unfitted_crime_bank(void)
{
    static const uint8_t tx_command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x3b
    };
    static const uint8_t tx_pointer[8] = {
        0x00, 0x00, 0x00, 0x3b, 0x48, 0x00, 0x10, 0x00
    };
    const uint64_t descriptor = RAM_FULL + 0x4000;
    const uint64_t invalid_rx = RAM_FULL + 0x08008000;
    const uint64_t valid_rx = RAM_FULL + 0x8000;
    const uint64_t guard_address = RAM_FULL + 0xa000;
    g_autofree char *dir = g_dir_make_tmp("mace-eth-bank-XXXXXX", NULL);
    g_autofree char *tx_log = g_build_filename(dir, "tx.log", NULL);
    g_autofree char *rx_log = g_build_filename(dir, "rx.log", NULL);
    g_autofree char *log_text = NULL;
    uint8_t frame[60], descriptor_after[8], guard[8];
    uint8_t guard_after[8], received[60], rx_status[8];
    QTestState *qts;
    SgiWait wait;
    int sv[2];
    struct timeval timeout = { .tv_sec = 1 };
    uint32_t word;

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i + 0x31;
    }
    memcpy(frame, (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    memcpy(guard, (uint8_t[]) { 0xa1, 0xb2, 0xc3, 0xd4,
                                0xe5, 0xf6, 0x17, 0x28 }, sizeof(guard));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpint(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO,
                               &timeout, sizeof(timeout)), ==, 0);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "phy-ics1890=on,phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                      "-d guest_errors -D %s", sv[1], tx_log);
    qtest_writeq(qts, UINT64_C(0x14000208), 0x100);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, descriptor, tx_command, sizeof(tx_command));
    qtest_memwrite(qts, descriptor + 8, tx_pointer, sizeof(tx_pointer));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    qtest_memread(qts, descriptor, descriptor_after,
                  sizeof(descriptor_after));
    g_assert_cmpmem(descriptor_after, sizeof(descriptor_after), tx_command,
                    sizeof(tx_command));
    errno = 0;
    g_assert_cmpint(recv(sv[0], &word, sizeof(word), MSG_WAITALL), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(tx_log, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "TX DMA failed at 0x48001000"));
    g_clear_pointer(&log_text, g_free);
    close(sv[0]);
    close(sv[1]);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "phy-ics1890=on,phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                      "-d guest_errors,unimp -D %s", sv[1], rx_log);
    qtest_writeq(qts, UINT64_C(0x14000208), 0x100);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, guard_address, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, invalid_rx);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE RX stops on unfitted CRIME bank");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 1);
    qtest_memread(qts, guard_address, guard_after, sizeof(guard_after));
    g_assert_cmpmem(guard_after, sizeof(guard_after), guard,
                    sizeof(guard));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT), ==,
                    invalid_rx);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, valid_rx);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_fifo_count(qts, 0);
    qtest_memread(qts, valid_rx, rx_status, sizeof(rx_status));
    g_assert_cmphex(rx_status[0], ==, 0x80);
    g_assert_cmphex(rx_status[2] | rx_status[3], ==, 0);
    qtest_memread(qts, valid_rx + 34, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), frame, sizeof(frame));
    qtest_memread(qts, guard_address, guard_after, sizeof(guard_after));
    g_assert_cmpmem(guard_after, sizeof(guard_after), guard,
                    sizeof(guard));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(rx_log, &log_text, NULL, NULL));
    {
        const char *failure = strstr(log_text,
                                     "RX DMA failed at 0x48008000");
        const char *warning = strstr(log_text,
            "RX partial checksum is unsupported");

        g_assert_nonnull(failure);
        g_assert_nonnull(warning);
        g_assert_true(failure < warning);
        g_assert_null(strstr(warning + 1,
                             "RX partial checksum is unsupported"));
    }
    g_assert_cmpint(g_remove(tx_log), ==, 0);
    g_assert_cmpint(g_remove(rx_log), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_no_ecc_dma_alias(void)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t done[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t descriptor = RAM_NO_ECC + 0x4000;
    const uint64_t rx_buffer = RAM_NO_ECC + 0x8000;
    uint8_t frame[60], got[60], landed[60], status[8];
    uint8_t old_buffer[256], guard[16];
    uint32_t wire_length;
    QTestState *qts;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i * 9 + 0x23;
    }
    memset(old_buffer, 0xa5, sizeof(old_buffer));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, descriptor, command, sizeof(command));
    qtest_memwrite(qts, descriptor + 68, frame, sizeof(frame));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4, 3);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 1);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(frame));
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(qts, descriptor, status, sizeof(status));
    g_assert_cmpmem(status, sizeof(status), done, sizeof(done));
    qtest_memread(qts, RAM_FULL + 0x4000, status, sizeof(status));
    g_assert_cmpmem(status, sizeof(status), done, sizeof(done));

    qtest_memwrite(qts, rx_buffer, old_buffer, sizeof(old_buffer));
    qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 4, rx_buffer);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_fifo_count(qts, 0);
    qtest_memread(qts, rx_buffer + 34, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    qtest_memread(qts, RAM_FULL + 0x8000 + 34, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    qtest_memread(qts, rx_buffer, status, sizeof(status));
    g_assert_true(status[0] & 0x80);
    qtest_memread(qts, rx_buffer + 98, guard, sizeof(guard));
    g_assert_cmpmem(guard, sizeof(guard), old_buffer, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void low_dma_descriptor(uint8_t entry[128], uint8_t seed)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };

    memset(entry, 0xa5, 128);
    memcpy(entry, command, sizeof(command));
    for (unsigned i = 0; i < 60; i++) {
        entry[68 + i] = seed + i * 7;
    }
}

static void test_low_dma_default_failure(void)
{
    const uint64_t base = UINT64_C(0x01100000);
    g_autofree char *dir = g_dir_make_tmp("mace-low-dma-off-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    uint8_t entry[128], after[128];
    QTestState *qts;
    uint32_t wire_length;
    int sv[2];

    low_dma_descriptor(entry, 0x21);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "phy-ics1890=on,phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                      "-d guest_errors -D %s", sv[1], log_path);
    g_assert_false(qtest_qom_get_bool(qts, "/machine",
                                       "mace-eth-low-dma-compat"));
    qtest_writeq(qts, UINT64_C(0x14000208), 0x100);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, RAM_FULL + base, entry, sizeof(entry));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    qtest_memread(qts, RAM_FULL + base, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), entry, sizeof(entry));
    errno = 0;
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "TX DMA failed at 0x1100000"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void low_dma_assert_views(QTestState *qts, uint64_t low,
                                 const uint8_t *expected, size_t size)
{
    static const uint64_t views[] = { 0, RAM_FULL, RAM_NO_ECC };
    g_autofree uint8_t *got = g_malloc(size);

    for (unsigned i = 0; i < G_N_ELEMENTS(views); i++) {
        qtest_memread(qts, views[i] + low, got, size);
        g_assert_cmpmem(got, size, expected, size);
    }
}

static void low_dma_assert_packet(int fd, const uint8_t expected[60])
{
    uint32_t wire_length;
    uint8_t got[60];

    g_assert_cmpint(recv(fd, &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
    g_assert_cmpint(recv(fd, got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), expected, 60);
}

static void test_low_dma_shared_ram(void)
{
    static const uint8_t done[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t tx = UINT64_C(0x01100000);
    const uint64_t rx = UINT64_C(0x01200000);
    uint8_t entry[2][128], old_second[128], guard[16];
    uint8_t frame[60], rx_bytes[128], byte_guard[3];
    QTestState *qts;
    int sv[2];

    low_dma_descriptor(entry[0], 0x31);
    low_dma_descriptor(old_second, 0x76);
    low_dma_descriptor(entry[1], 0x92);
    memset(guard, 0xa5, sizeof(guard));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "mace-eth-low-dma-compat=on,phy-ics1890=on,"
                      "phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    g_assert_true(qtest_qom_get_bool(qts, "/machine",
                                      "mace-eth-low-dma-compat"));
    qtest_writeq(qts, UINT64_C(0x14000208), 0x100);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, RAM_FULL + tx - sizeof(guard), guard,
                   sizeof(guard));
    qtest_memwrite(qts, RAM_FULL + tx, entry[0], sizeof(entry[0]));
    qtest_memwrite(qts, RAM_FULL + tx + 128, old_second,
                   sizeof(old_second));
    qtest_memwrite(qts, RAM_FULL + tx + 256, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, tx);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 1);
    low_dma_assert_packet(sv[0], entry[0] + 68);
    low_dma_assert_views(qts, tx, done, sizeof(done));

    qtest_memwrite(qts, RAM_FULL + tx + 128, entry[1], sizeof(entry[1]));
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 2);
    low_dma_assert_packet(sv[0], entry[1] + 68);
    low_dma_assert_views(qts, tx + 128, done, sizeof(done));
    low_dma_assert_views(qts, tx - sizeof(guard), guard, sizeof(guard));
    low_dma_assert_views(qts, tx + 256, guard, sizeof(guard));

    byte_guard[0] = 0x61;
    byte_guard[1] = 0x62;
    byte_guard[2] = 0x63;
    qtest_memwrite(qts, RAM_FULL + tx + 0x17f, byte_guard,
                   sizeof(byte_guard));
    qtest_writeb(qts, tx + 0x180, 0x99);
    byte_guard[1] = 0x99;
    low_dma_assert_views(qts, tx + 0x17f, byte_guard,
                         sizeof(byte_guard));

    memset(rx_bytes, 0xa5, sizeof(rx_bytes));
    qtest_memwrite(qts, RAM_FULL + rx - sizeof(guard), guard,
                   sizeof(guard));
    qtest_memset(qts, RAM_FULL + rx, 0xa5, 4096);
    qtest_memwrite(qts, RAM_FULL + rx + 4096, guard, sizeof(guard));
    memset(frame, 0x5a, sizeof(frame));
    memcpy(frame, (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, rx);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | (1U << 9));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_fifo_count(qts, 0);
    qtest_memread(qts, RAM_FULL + rx, rx_bytes, sizeof(rx_bytes));
    g_assert_true(rx_bytes[0] & 0x80);
    g_assert_cmphex(rx_bytes[7], ==, 64);
    g_assert_cmpmem(rx_bytes + 34, sizeof(frame), frame, sizeof(frame));
    low_dma_assert_views(qts, rx, rx_bytes, sizeof(rx_bytes));
    low_dma_assert_views(qts, rx - sizeof(guard), guard, sizeof(guard));
    low_dma_assert_views(qts, rx + 4096, guard, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_low_dma_holes(void)
{
    const uint64_t bank_hole = UINT64_C(0x08000000);
    const uint64_t outside_low = UINT64_C(0x10000000);
    g_autofree char *dir = g_dir_make_tmp("mace-low-dma-hole-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    uint8_t entry[128], after[128], frame[60], guard[16];
    uint8_t rx_boundary[128];
    QTestState *qts;
    SgiWait wait;
    uint32_t wire_length;
    int sv[2];

    low_dma_descriptor(entry, 0x43);
    memset(guard, 0xa5, sizeof(guard));
    memset(rx_boundary, 0xa5, sizeof(rx_boundary));
    memset(frame, 0x5a, sizeof(frame));
    memcpy(frame, (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "mace-eth-low-dma-compat=on,phy-ics1890=on,"
                      "phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                      "-d guest_errors -D %s", sv[1], log_path);
    qtest_writeq(qts, UINT64_C(0x14000208), 0x100);
    qtest_writeq(qts, UINT64_C(0x14000218), 0x108);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, RAM_FULL + bank_hole - sizeof(guard),
                   guard, sizeof(guard));
    qtest_memwrite(qts, RAM_FULL + outside_low, entry, sizeof(entry));
    qtest_memwrite(qts, RAM_FULL + outside_low + 0x8000, rx_boundary,
                   sizeof(rx_boundary));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, bank_hole);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 2);

    qtest_writeq(qts, ETH_BASE + INT_STATUS, 8);
    qtest_writeq(qts, ETH_BASE + TX_BASE, outside_low);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 2);
    qtest_memread(qts, RAM_FULL + bank_hole - sizeof(guard),
                  after, sizeof(guard));
    g_assert_cmpmem(after, sizeof(guard), guard, sizeof(guard));
    qtest_memread(qts, RAM_FULL + outside_low, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), entry, sizeof(entry));
    errno = 0;
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);

    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, bank_hole + 0x8000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE low RX rejects an unpopulated bank");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 1);
    qtest_readq(qts, ETH_BASE + RX_FIFO_PORT);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, outside_low + 0x8000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE low RX rejects the frame-buffer aperture");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    qtest_memread(qts, RAM_FULL + outside_low + 0x8000,
                  after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), rx_boundary,
                    sizeof(rx_boundary));
    qtest_memread(qts, RAM_FULL + bank_hole - sizeof(guard),
                  after, sizeof(guard));
    g_assert_cmpmem(after, sizeof(guard), guard, sizeof(guard));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "TX DMA failed at 0x8000000"));
    g_assert_nonnull(strstr(log_text, "TX DMA failed at 0x10000000"));
    g_assert_nonnull(strstr(log_text, "RX DMA failed at 0x8008000"));
    g_assert_nonnull(strstr(log_text, "RX DMA failed at 0x10008000"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_link_transition_irq(void)
{
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    mdio_write(qts, 8, 0, 0x2100);
    mdio_read(qts, 8, 1); /* Clear the startup low-link latch. */
    g_assert_true(mdio_read(qts, 8, 1) & 4);
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, (1U << 7) | (1U << 3) |
                 (1U << 1));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL) & 0x8b,
                    ==, 0x8a);
    qtest_qmp_assert_success(qts,
        "{'execute':'set_link','arguments':{'name':'n0','up':false}}");
    g_assert_false(mdio_read(qts, 8, 1) & 4);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 4);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_qmp_assert_success(qts,
        "{'execute':'set_link','arguments':{'name':'n0','up':true}}");
    mdio_read(qts, 8, 1);
    g_assert_true(mdio_read(qts, 8, 1) & 4);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 4);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_unlisted_widths_and_reset(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-eth-width-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts = qtest_initf("-M o2-test -nic none -d unimp "
                                  "-D %s", log_path);

    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writel(qts, ETH_BASE + STATION, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, ETH_BASE + STATION), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + STATION), ==,
                    UINT64_C(0x02a14c7395e6));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + 0x90), ==, 0);
    qtest_writeq(qts, ETH_BASE + 0x90, UINT64_MAX);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, (1U << 2) | (1U << 4));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (1U << 11));
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MAC_CONTROL), ==,
                    UINT64_C(0x20000001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS), ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "unsupported write at 0xa0 size 4"));
    g_assert_nonnull(strstr(log_text, "unsupported read at 0x90 size 8"));
    g_assert_nonnull(strstr(log_text, "MAC loopback/SIA is unsupported"));
    g_assert_nonnull(strstr(log_text, "RX gathering is unsupported"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_rx_fifo_underflow(void)
{
    uint8_t frame[60];
    QTestState *qts;
    SgiWait wait;
    int sv[2];

    memset(frame, 0x5a, sizeof(frame));
    memcpy(frame, (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 1U << 15);
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE RX FIFO underflow event");
    while (!(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 6))) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 1U << 6);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static QTestState *mace_standalone_start(const char *extra)
{
    QTestState *qts = qtest_initf(
        "-M none -cpu R10000 -S -nic none "
        "-device sgi-mace,id=mace,phy-ics1890=on,phy-address=8 %s",
        extra);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 0,
                          UINT64_C(0x1f000000));
    qtest_irq_intercept_out_named(qts, "/machine/peripheral/mace",
                                  "crime");
    return qts;
}

static void test_migrate_pending_mdio_and_fifo(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-eth-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = mace_standalone_start("");
    QTestState *to = mace_standalone_start("-incoming defer");
    int64_t source_clock;

    qtest_writeq(from, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(from, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    qtest_writeq(from, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(from, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (1U << 9) | (2U << 4));
    g_assert_true(qtest_get_irq(from, 3));
    qtest_writeq(from, ETH_BASE + MDIO_ADDRESS, (8U << 5) | 2);
    qtest_writeq(from, ETH_BASE + MDIO_READ, 0);
    source_clock = qtest_clock_step(from, 1);
    g_assert_true(qtest_readq(from, ETH_BASE + MDIO_DATA) & (1U << 16));

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    qtest_clock_set(to, source_clock);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    g_assert_cmphex(qtest_readq(to, ETH_BASE + STATION), ==,
                    UINT64_C(0x02a14c7395e6));
    g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x10001));
    g_assert_true(qtest_readq(to, ETH_BASE + INT_STATUS) & (1U << 5));
    g_assert_true(qtest_get_irq(to, 3));
    g_assert_true(qtest_readq(to, ETH_BASE + MDIO_DATA) & (1U << 16));
    qtest_clock_step(to, 999);
    g_assert_cmphex(qtest_readq(to, ETH_BASE + MDIO_DATA), ==, 0x0015);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_control_aliases(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-eth-filter-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts = qtest_initf("-M o2-test -nic none -d unimp "
                                  "-D %s", log_path);

    qtest_writeq(qts, ETH_BASE + TX_INT_ALIAS, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==, 1);
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, (1U << 9) | (2U << 4));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL), ==,
                    (1U << 9) | (2U << 4) | 1);
    qtest_writeq(qts, ETH_BASE + RX_DELAY, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_DELAY), ==, 0);
    qtest_writeq(qts, ETH_BASE + STATION_SECONDARY,
                 UINT64_C(0x5a46bd8732e1));
    qtest_writeq(qts, ETH_BASE + MULTICAST_MASK,
                 UINT64_C(0x8000000000000001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + STATION_SECONDARY), ==,
                    UINT64_C(0x5a46bd8732e1));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + MULTICAST_MASK), ==,
                    UINT64_C(0x8000000000000001));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text,
                            "secondary station filtering is unsupported"));
    g_assert_nonnull(strstr(log_text,
                            "multicast hash filtering is unsupported"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_tx_ring_wrap(void)
{
    static const uint8_t command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0x00, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x3c
    };
    const uint64_t base = RAM_FULL + 0xc000;
    uint8_t ring[8192] = { 0 }, guard[16], got[60], landed[16];
    uint32_t wire_length;
    QTestState *qts;
    int sv[2];

    for (unsigned slot = 0; slot < 64; slot++) {
        memcpy(ring + slot * 128, command, sizeof(command));
        for (unsigned i = 0; i < sizeof(got); i++) {
            ring[slot * 128 + 68 + i] = slot + 3 * i;
        }
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, base - sizeof(guard), guard, sizeof(guard));
    qtest_memwrite(qts, base, ring, sizeof(ring));
    qtest_memwrite(qts, base + sizeof(ring), guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 63);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==,
                    UINT64_C(0x3f003f));
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
    for (unsigned slot = 0; slot < 64; slot++) {
        g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                             MSG_WAITALL), ==, sizeof(wire_length));
        g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
        g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                        sizeof(got));
        g_assert_cmpmem(got, sizeof(got), ring + slot * 128 + 68,
                        sizeof(got));
    }
    qtest_memread(qts, base, landed, sizeof(status));
    g_assert_cmpmem(landed, sizeof(status), status, sizeof(status));
    qtest_memread(qts, base + 63 * 128, landed, sizeof(status));
    g_assert_cmpmem(landed, sizeof(status), status, sizeof(status));
    qtest_memread(qts, base - sizeof(guard), landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), guard, sizeof(guard));
    qtest_memread(qts, base + sizeof(ring), landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), guard, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void tx_sequence_frame(uint8_t frame[60], unsigned sequence)
{
    memset(frame, 0xff, 6);
    frame[6] = 0x02;
    frame[7] = 0xa1;
    frame[8] = 0x4c;
    frame[9] = 0x73;
    frame[10] = sequence >> 8;
    frame[11] = sequence;
    frame[12] = 0x88;
    frame[13] = 0xb5;
    for (unsigned i = 14; i < 60; i++) {
        frame[i] = sequence * 17 + i * 7;
    }
}

static void test_tx_pointer_outside_ring(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-tx-pointer-range-XXXXXX",
                                       NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                      "-d unimp,guest_errors -D %s", sv[1], log_path);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 64);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x18, ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2, ==, 2);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text, "TX pointer outside active ring"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static uint64_t tx_pointer_aliases(QTestState *qts)
{
    uint64_t pointer = qtest_readq(qts, ETH_BASE + TX_POINTER);

    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==,
                    pointer);
    return pointer;
}

static void expect_tx_frame(int fd, const uint8_t frame[60])
{
    uint32_t wire_length;
    uint8_t got[60];

    g_assert_cmpint(recv(fd, &wire_length, sizeof(wire_length), MSG_WAITALL),
                    ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
    g_assert_cmpint(recv(fd, got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, 60);
}

static void expect_no_tx_frame(int fd)
{
    uint32_t wire_length;

    g_assert_cmpint(recv(fd, &wire_length, sizeof(wire_length),
                         MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);
}

static QTestState *start_tx_after_frames(int sv[2], unsigned completed)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t base = RAM_FULL + 0x40000;
    uint8_t entry[128], frame[60], landed[8];
    QTestState *qts;

    g_assert_cmpuint(completed, >, 0);
    g_assert_cmpuint(completed, <=, 63);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    for (unsigned i = 0; i < completed; i++) {
        memset(entry, 0, sizeof(entry));
        memcpy(entry, command, sizeof(command));
        tx_sequence_frame(frame, i);
        memcpy(entry + 68, frame, sizeof(frame));
        qtest_memwrite(qts, base + i * 128, entry, sizeof(entry));
        qtest_writeq(qts, ETH_BASE + TX_POINTER, i + 1);
        expect_tx_frame(sv[0], frame);
        qtest_memread(qts, base + i * 128, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
    }
    g_assert_cmphex(tx_pointer_aliases(qts), ==,
                    ((uint64_t)completed << 16) | completed);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 2);
    return qts;
}

static void test_tx_mac_reset_keeps_queue(void)
{
    int sv[2];
    QTestState *qts = start_tx_after_frames(sv, 1);

    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 0);
    expect_no_tx_frame(sv[0]);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_dma_disable_keeps_queue(void)
{
    int sv[2];
    QTestState *qts = start_tx_after_frames(sv, 1);

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 0);
    expect_no_tx_frame(sv[0]);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_driver_reinitialization(void)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t restarted_base = RAM_FULL + 0x60000;
    uint8_t entry[128] = { 0 }, frame[60], landed[8];
    int sv[2];
    QTestState *qts = start_tx_after_frames(sv, 1);

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    tx_pointer_aliases(qts);
    qtest_writeq(qts, ETH_BASE + TX_BASE, restarted_base);
    tx_pointer_aliases(qts);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RAM_FULL + 0x50000 + i * 0x1000);
    }
    tx_pointer_aliases(qts);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 0);
    g_assert_cmphex(tx_pointer_aliases(qts) & 0x1ff, ==, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    tx_pointer_aliases(qts);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    g_assert_cmphex(tx_pointer_aliases(qts), ==, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2, ==, 2);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 0);
    expect_no_tx_frame(sv[0]);

    memcpy(entry, command, sizeof(command));
    tx_sequence_frame(frame, 1);
    memcpy(entry + 68, frame, sizeof(frame));
    qtest_memwrite(qts, restarted_base, entry, sizeof(entry));
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    expect_tx_frame(sv[0], frame);
    qtest_memread(qts, restarted_base, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
    g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 2);
    expect_no_tx_frame(sv[0]);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_reinitialization_without_producer_zero(void)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t restarted_base = RAM_FULL + 0x60000;

    for (unsigned mode = 0; mode < 2; mode++) {
        uint8_t entry[128] = { 0 }, fresh[60], stale[60], landed[8];
        int sv[2];
        QTestState *qts = start_tx_after_frames(sv, 15);

        tx_sequence_frame(fresh, 601);
        tx_sequence_frame(stale, 600);
        memcpy(entry, command, sizeof(command));
        memcpy(entry + 68, fresh, sizeof(fresh));
        qtest_memwrite(qts, restarted_base, entry, sizeof(entry));
        memcpy(entry + 68, stale, sizeof(stale));
        qtest_memwrite(qts, restarted_base + 15 * 128,
                       entry, sizeof(entry));

        if (mode == 0) {
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
        } else {
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
        }
        qtest_writeq(qts, ETH_BASE + TX_BASE, restarted_base);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
        g_assert_cmphex(tx_pointer_aliases(qts), ==, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a,
                        ==, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2, ==, 2);
        expect_no_tx_frame(sv[0]);

        qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
        expect_tx_frame(sv[0], fresh);
        qtest_memread(qts, restarted_base, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
        qtest_memread(qts, restarted_base + 15 * 128,
                      landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), command, sizeof(command));
        g_assert_cmphex(tx_pointer_aliases(qts), ==, UINT64_C(0x10001));
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a,
                        ==, 2);
        expect_no_tx_frame(sv[0]);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_tx_reinitialization_migrates(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        g_autofree char *dir = g_dir_make_tmp("mace-tx-reinit-XXXXXX", NULL);
        g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = mace_standalone_start("");
        QTestState *to = mace_standalone_start("-incoming defer");

        qtest_writeq(from, ETH_BASE + MAC_CONTROL, 0);
        qtest_writeq(from, ETH_BASE + DMA_CONTROL, 2);
        if (mode == 0) {
            qtest_writeq(from, ETH_BASE + DMA_CONTROL, 0);
            qtest_writeq(from, ETH_BASE + MAC_CONTROL, 1);
        } else {
            qtest_writeq(from, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(from, ETH_BASE + DMA_CONTROL, 0);
        }
        qtest_writeq(from, ETH_BASE + TX_BASE, RAM_FULL + 0x40000);
        qtest_writeq(from, ETH_BASE + TX_POINTER, 1);
        qtest_writeq(from, ETH_BASE + MAC_CONTROL, 0);
        g_assert_cmphex(tx_pointer_aliases(from), ==, 1);

        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        qtest_qmp_assert_success(to, "{'execute':'cont'}");
        g_assert_cmphex(tx_pointer_aliases(to), ==, 1);
        qtest_writeq(to, ETH_BASE + DMA_CONTROL, 2);
        g_assert_cmphex(tx_pointer_aliases(to), ==, 0);
        qtest_quit(to);
        qtest_quit(from);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_tx_sustained_ring(void)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0x01, 0x44, 0, 0x3b
    };
    static const uint8_t status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x3c
    };
    const uint64_t base = RAM_FULL + 0x40000;
    const uint64_t restarted_base = RAM_FULL + 0x60000;
    uint8_t frames[63][60], entry[128], got[60], landed[8];
    uint8_t guard[16], guard_after[16];
    uint64_t pointer;
    uint32_t wire_length;
    unsigned read, next;
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    memset(guard, 0xa5, sizeof(guard));
    qtest_memwrite(qts, base - sizeof(guard), guard, sizeof(guard));
    qtest_memwrite(qts, base + 8192, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);

    for (unsigned sent = 0; sent < 524;) {
        unsigned count = MIN(63U, 524U - sent);

        count = MIN(count, 64U - (sent & 63));
        if (sent == 64) {
            count = 1;
        }
        for (unsigned i = 0; i < count; i++) {
            unsigned slot = (sent + i) & 63;

            memset(entry, 0, sizeof(entry));
            memcpy(entry, command, sizeof(command));
            tx_sequence_frame(frames[i], sent + i);
            memcpy(entry + 68, frames[i], sizeof(frames[i]));
            qtest_memwrite(qts, base + slot * 128, entry, sizeof(entry));
        }
        next = (sent + count) & 63;
        qtest_writeq(qts, ETH_BASE + TX_POINTER, next);
        for (unsigned i = 0; i < count; i++) {
            unsigned slot = (sent + i) & 63;

            g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                                 MSG_WAITALL), ==, sizeof(wire_length));
            g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
            g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                            sizeof(got));
            g_assert_cmpmem(got, sizeof(got), frames[i], sizeof(frames[i]));
            qtest_memread(qts, base + slot * 128, landed, sizeof(landed));
            g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
        }
        sent += count;
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==,
                        ((uint64_t)next << 16) | next);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 2);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2, ==, 2);
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 2);
        if (sent == 64) {
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a,
                            ==, 0);
            g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                                 MSG_DONTWAIT), ==, -1);
            g_assert_cmpint(errno, ==, EAGAIN);
        }
        if (sent == 65) {
            g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                                 MSG_DONTWAIT), ==, -1);
            g_assert_cmpint(errno, ==, EAGAIN);
        }
    }

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + TX_BASE, restarted_base);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RAM_FULL + 0x50000 + i * 0x1000);
    }
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    pointer = qtest_readq(qts, ETH_BASE + TX_POINTER);
    read = (pointer >> 16) & 63;
    next = (read + 1) & 63;

    g_assert_cmphex(pointer & 63, ==, read);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_BASE), ==,
                    restarted_base);
    memset(entry, 0, sizeof(entry));
    memcpy(entry, command, sizeof(command));
    tx_sequence_frame(frames[0], 524);
    memcpy(entry + 68, frames[0], sizeof(frames[0]));
    qtest_memwrite(qts, restarted_base + (read & 63) * 128,
                   entry, sizeof(entry));
    qtest_writeq(qts, ETH_BASE + TX_POINTER, next);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frames[0], sizeof(frames[0]));
    qtest_memread(qts, restarted_base + (read & 63) * 128,
                  landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), status, sizeof(status));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==,
                    ((uint64_t)next << 16) | next);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x1a, ==, 2);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2, ==, 2);
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);
    qtest_memread(qts, base - sizeof(guard), guard_after, sizeof(guard_after));
    g_assert_cmpmem(guard_after, sizeof(guard_after), guard, sizeof(guard));
    qtest_memread(qts, base + 8192, guard_after, sizeof(guard_after));
    g_assert_cmpmem(guard_after, sizeof(guard_after), guard, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_large_ring(unsigned size_bits)
{
    static const uint8_t command[8] = {
        0, 0, 0, 0, 0, 0x18, 0, 0x63
    };
    static const uint8_t status[8] = {
        0x80, 0, 0, 0, 0, 0x80, 0, 0x64
    };
    const uint64_t base = RAM_FULL + 0x20000;
    const unsigned entries = 64U << size_bits;
    const size_t ring_bytes = entries * 128;
    g_autofree uint8_t *ring = g_malloc0(ring_bytes);
    uint8_t guard[16], landed[16], got[100], vector[8];
    uint32_t wire_length;
    QTestState *qts;
    int sv[2];

    memset(guard, 0xa5, sizeof(guard));
    for (unsigned slot = 0; slot < entries; slot++) {
        uint8_t *entry = ring + slot * 128;

        memcpy(entry, command, sizeof(command));
        for (unsigned i = 0; i < sizeof(got); i++) {
            entry[24 + i] = slot * 11 + i * 3;
        }
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_memwrite(qts, base - sizeof(guard), guard, sizeof(guard));
    qtest_memwrite(qts, base, ring, ring_bytes);
    qtest_memwrite(qts, base + ring_bytes, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4,
                 3 | (size_bits << 2));
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, entries - 1);
    g_assert_cmphex(qtest_readw(qts, ETH_BASE + TX_POINTER + 4), ==,
                    entries - 1);
    qtest_writew(qts, ETH_BASE + TX_POINTER + 6, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);

    for (unsigned slot = 0; slot < entries; slot++) {
        g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                             MSG_WAITALL), ==, sizeof(wire_length));
        g_assert_cmpuint(ntohl(wire_length), ==, sizeof(got));
        g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                        sizeof(got));
        g_assert_cmpmem(got, sizeof(got), ring + slot * 128 + 24,
                        sizeof(got));
    }
    qtest_memread(qts, base, vector, sizeof(vector));
    g_assert_cmpmem(vector, sizeof(vector), status, sizeof(status));
    qtest_memread(qts, base + (entries - 1) * 128,
                  vector, sizeof(vector));
    g_assert_cmpmem(vector, sizeof(vector), status, sizeof(status));
    qtest_memread(qts, base - sizeof(guard), landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), guard, sizeof(guard));
    qtest_memread(qts, base + ring_bytes, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), guard, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_ring_wrap_16k(void)
{
    test_tx_large_ring(1);
}

static void test_tx_ring_wrap_64k(void)
{
    test_tx_large_ring(3);
}

static void test_queued_packets_after_phy_negotiation(void)
{
    static const uint8_t command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0x00, 0x3b
    };
    static const uint8_t expected_status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x3c
    };
    const uint64_t descriptor = RAM_FULL + 0x4000;
    const uint64_t rx_buffer = RAM_FULL + 0x8000;
    uint8_t outgoing[60], incoming[60], received[60], status[8];
    uint32_t wire_length;
    QTestState *qts;
    SgiWait wait;
    int sv[2];

    for (unsigned i = 0; i < sizeof(outgoing); i++) {
        outgoing[i] = i * 5 + 7;
        incoming[i] = i * 11 + 9;
    }
    memcpy(incoming,
           (uint8_t[]) { 0x02, 0xa1, 0x4c, 0x73, 0x95, 0xe6 }, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_memwrite(qts, descriptor, command, sizeof(command));
    qtest_memwrite(qts, descriptor + 68, outgoing, sizeof(outgoing));
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, rx_buffer);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    socket_send_frame(sv[0], incoming, sizeof(incoming));
    qtest_clock_step(qts, 99999999);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 1);
    qtest_clock_step(qts, 1);

    sgi_wait_start(&wait, "MACE queued TX and RX after PHY negotiation");
    while ((qtest_readq(qts, ETH_BASE + TX_POINTER) >> 16) != 1 ||
           (qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31) != 0) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 3, ==, 3);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_memread(qts, descriptor, status, sizeof(status));
    g_assert_cmpmem(status, sizeof(status), expected_status,
                    sizeof(expected_status));
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpint(recv(sv[0], &wire_length, sizeof(wire_length),
                         MSG_WAITALL), ==, sizeof(wire_length));
    g_assert_cmpuint(ntohl(wire_length), ==, sizeof(outgoing));
    g_assert_cmpint(recv(sv[0], received, sizeof(received),
                         MSG_WAITALL), ==, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), outgoing,
                    sizeof(outgoing));
    qtest_memread(qts, rx_buffer + 34, received, sizeof(received));
    g_assert_cmpmem(received, sizeof(received), incoming,
                    sizeof(incoming));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_queued_tx_irq_after_phy_negotiation(void)
{
    static const uint8_t command[8] = {
        0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0x00, 0x3b
    };
    static const uint8_t expected_status[8] = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x3c
    };
    const uint64_t descriptor = RAM_FULL + 0x4000;
    uint8_t frame[60], status[8];
    QTestState *qts;
    SgiWait wait;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i * 13 + 1;
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 8);
    qtest_memwrite(qts, descriptor, command, sizeof(command));
    qtest_memwrite(qts, descriptor + 68, frame, sizeof(frame));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, descriptor);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 1);
    qtest_clock_step(qts, 100000000);
    sgi_wait_start(&wait, "MACE TX after PHY negotiation");
    while ((qtest_readq(qts, ETH_BASE + TX_POINTER) >> 16) != 1) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1);
    }
    qtest_memread(qts, descriptor, status, sizeof(status));
    g_assert_cmpmem(status, sizeof(status), expected_status,
                    sizeof(expected_status));
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 3, ==, 3);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_empty_reassert(void)
{
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    qtest_writeq(qts, CRIME_ENABLE, 8);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writel(qts, ETH_BASE + DMA_CONTROL + 4, 3);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writel(qts, ETH_BASE + TX_INT_ALIAS + 4, 0);
    qtest_writel(qts, ETH_BASE + INT_STATUS + 4, 1);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 1);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace/ethernet/register-reset-alias",
                   test_register_reset_and_alias);
    qtest_add_func("/sgi/mace/ethernet/selected-word-control-mdio",
                   test_selected_word_control_mdio);
    qtest_add_func("/sgi/mace/ethernet/selected-word-status-ack",
                   test_selected_word_status_ack);
    qtest_add_func("/sgi/mace/ethernet/selected-pointer-fifo-pio",
                   test_selected_pointer_fifo_pio);
    qtest_add_func("/sgi/mace/ethernet/selected-fifo-generation",
                   test_selected_fifo_generation);
    qtest_add_func("/sgi/mace/ethernet/selected-width-refusals",
                   test_selected_width_refusals);
    qtest_add_func("/sgi/mace/ethernet/mdio-selected-address",
                   test_mdio_selected_address);
    qtest_add_func("/sgi/mace/ethernet/tx-inline-raw", test_tx_inline_raw);
    qtest_add_func("/sgi/mace/ethernet/tx-concat-cross-page",
                   test_tx_concat_cross_page);
    qtest_add_func("/sgi/mace/ethernet/tx-two-concat-hole",
                   test_tx_two_concat_and_hole);
    qtest_add_func("/sgi/mace/ethernet/rx-filter-raw-dma",
                   test_rx_filter_and_raw_dma);
    qtest_add_func("/sgi/mace/ethernet/rx-fifo-dma-wrap",
                   test_rx_fifo_dma_wrap);
    qtest_add_func("/sgi/mace/ethernet/rx-sustained-replenish",
                   test_rx_sustained_fifo_replenish);
    qtest_add_func("/sgi/mace/ethernet/unfitted-crime-bank",
                   test_unfitted_crime_bank);
    qtest_add_func("/sgi/mace/ethernet/no-ecc-dma-alias",
                   test_no_ecc_dma_alias);
    qtest_add_func("/sgi/mace/ethernet/low-dma-default-failure",
                   test_low_dma_default_failure);
    qtest_add_func("/sgi/mace/ethernet/low-dma-shared-ram",
                   test_low_dma_shared_ram);
    qtest_add_func("/sgi/mace/ethernet/low-dma-holes",
                   test_low_dma_holes);
    qtest_add_func("/sgi/mace/ethernet/link-transition-irq",
                   test_link_transition_irq);
    qtest_add_func("/sgi/mace/ethernet/unlisted-widths-reset",
                   test_unlisted_widths_and_reset);
    qtest_add_func("/sgi/mace/ethernet/rx-fifo-underflow",
                   test_rx_fifo_underflow);
    qtest_add_func("/sgi/mace/ethernet/migrate-pending-mdio-fifo",
                   test_migrate_pending_mdio_and_fifo);
    qtest_add_func("/sgi/mace/ethernet/control-aliases",
                   test_control_aliases);
    qtest_add_func("/sgi/mace/ethernet/tx-ring-wrap", test_tx_ring_wrap);
    qtest_add_func("/sgi/mace/ethernet/tx-pointer-outside-ring",
                   test_tx_pointer_outside_ring);
    qtest_add_func("/sgi/mace/ethernet/tx-mac-reset-keeps-queue",
                   test_tx_mac_reset_keeps_queue);
    qtest_add_func("/sgi/mace/ethernet/tx-dma-disable-keeps-queue",
                   test_tx_dma_disable_keeps_queue);
    qtest_add_func("/sgi/mace/ethernet/tx-driver-reinitialization",
                   test_tx_driver_reinitialization);
    qtest_add_func("/sgi/mace/ethernet/tx-reinitialization-without-zero",
                   test_tx_reinitialization_without_producer_zero);
    qtest_add_func("/sgi/mace/ethernet/tx-reinitialization-migrates",
                   test_tx_reinitialization_migrates);
    qtest_add_func("/sgi/mace/ethernet/tx-sustained-ring",
                   test_tx_sustained_ring);
    qtest_add_func("/sgi/mace/ethernet/tx-pointer-readback",
                   test_tx_pointer_readback);
    qtest_add_func("/sgi/mace/ethernet/tx-ring-wrap-16k",
                   test_tx_ring_wrap_16k);
    qtest_add_func("/sgi/mace/ethernet/tx-ring-wrap-64k",
                   test_tx_ring_wrap_64k);
    qtest_add_func("/sgi/mace/ethernet/queued-phy-negotiation",
                   test_queued_packets_after_phy_negotiation);
    qtest_add_func("/sgi/mace/ethernet/queued-tx-irq",
                   test_queued_tx_irq_after_phy_negotiation);
    qtest_add_func("/sgi/mace/ethernet/tx-empty-reassert",
                   test_tx_empty_reassert);
    return g_test_run();
}
