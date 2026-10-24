/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE 2.0 Ethernet register and packet DMA contracts. */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
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
#define RX_REINIT_OLD_BASE (RAM_FULL + 0x80000)
#define RX_REINIT_NEW_BASE (RAM_FULL + 0xa0000)

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

static uint64_t eth_status(QTestState *qts, uint64_t address)
{
    uint8_t bytes[8];
    uint64_t value = 0;

    qtest_memread(qts, address, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

static void wait_rx_status(QTestState *qts, uint64_t address)
{
    SgiWait wait;

    sgi_wait_start(&wait, "MACE RX valid status");
    while (!(eth_status(qts, address) & (UINT64_C(1) << 63))) {
        sgi_wait_pause(&wait, 1000);
        qtest_clock_step(qts, 0);
    }
}

static QTestState *rx_interval_start(int sv[2], unsigned count, bool gather)
{
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    for (unsigned i = 0; i < count; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RAM_FULL + 0x8000 + 4096 * i);
    }
    qtest_writeq(qts, ETH_BASE + RX_DELAY, 2);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (1U << 9) | (count << 4) |
                 (4U << 12) | (gather ? 1U << 11 : 0));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 0xff);
    return qts;
}

static void test_rx_interval(void)
{
    uint8_t frame[60] = { 0 };
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, false);

    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 7);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_clock_step(qts, 30690);
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x9000);
    qtest_writeq(qts, ETH_BASE + RX_DELAY, 63);
    qtest_clock_step(qts, 30689);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, 8U << 4);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_clock_step(qts, 10000000);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_interval_refill(void)
{
    uint8_t frame[60] = { 0 };
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, false);

    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x18000);
    qtest_clock_step(qts, 61380);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_interval_early(void)
{
    uint8_t frame[60] = { 0 };
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 4, false);

    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 3);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, 3U << 4);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
    qtest_clock_step(qts, 61380);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_gather_interval(void)
{
    uint8_t frame[60] = { 0 }, second[60], landed[60];
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, true);

    memset(second, 0x5b, sizeof(second));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 8);
    qtest_clock_step(qts, 30690);
    socket_send_frame(sv[0], second, sizeof(second));
    wait_rx_status(qts, RAM_FULL + 0x8100);
    g_assert_cmpuint((eth_status(qts, RAM_FULL + 0x8100) >> 27) & 31, ==, 1);
    qtest_memread(qts, RAM_FULL + 0x8122, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), second, sizeof(second));
    g_assert_cmphex(eth_status(qts, RAM_FULL + 0x9000), ==, 0);
    qtest_clock_step(qts, 30689);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 7);
    qtest_clock_step(qts, 61380);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 7);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_gather_disable(void)
{
    uint8_t frame[60] = { 0 };
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, true);

    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 7);
    qtest_clock_step(qts, 61380);
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x9000);
    g_assert_cmphex(eth_status(qts, RAM_FULL + 0x8100), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_interval_migration(void)
{
    for (unsigned gather = 0; gather < 2; gather++) {
        for (unsigned canceled = 0; canceled < 2; canceled++) {
            g_autofree char *dir = g_dir_make_tmp("mace-rx-timer-XXXXXX", NULL);
            g_autofree char *socket = g_build_filename(dir, "migration.sock",
                                                       NULL);
            g_autofree char *uri = g_strdup_printf("unix:%s", socket);
            uint8_t frame[60] = { 0 }, first[128], restored[128];
            int sv[2], dest_sv[2];
            QTestState *from = rx_interval_start(sv, 8, gather);
            QTestState *to;
            int64_t clock;

            g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, dest_sv),
                            ==, 0);
            to = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 -S "
                             "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                             "-incoming defer", dest_sv[1]);
            socket_send_frame(sv[0], frame, sizeof(frame));
            wait_rx_status(from, RAM_FULL + 0x8000);
            qtest_memread(from, RAM_FULL + 0x8000, first, sizeof(first));
            clock = qtest_clock_step(from, 61379);
            if (canceled) {
                qtest_writeq(from, ETH_BASE + DMA_CONTROL, 0);
            }
            qtest_qmp_assert_success(from, "{'execute':'stop'}");
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
            sgi_migrate_stopped(from, uri);
            sgi_wait_migration(to, "completed");
            qtest_clock_set(to, clock);
            qtest_qmp_assert_success(to, "{'execute':'cont'}");
            qtest_memread(to, RAM_FULL + 0x8000, restored, sizeof(restored));
            g_assert_cmpmem(first, sizeof(first), restored, sizeof(restored));
            if (gather && !canceled) {
                socket_send_frame(dest_sv[0], frame, sizeof(frame));
                wait_rx_status(to, RAM_FULL + 0x8100);
                g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31,
                                 ==, 8);
            }
            g_assert_false(qtest_readq(to, ETH_BASE + INT_STATUS) & 32);
            qtest_clock_step(to, 1);
            g_assert_cmpint(!!(qtest_readq(to, ETH_BASE + INT_STATUS) & 32),
                            ==, !canceled);
            g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31,
                             ==, 7);
            qtest_writeq(to, ETH_BASE + INT_STATUS, 32);
            qtest_clock_step(to, 1000000);
            g_assert_false(qtest_readq(to, ETH_BASE + INT_STATUS) & 32);
            g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31,
                             ==, 7);
            qtest_quit(to);
            qtest_quit(from);
            close(sv[0]);
            close(sv[1]);
            close(dest_sv[0]);
            close(dest_sv[1]);
            g_unlink(socket);
            g_rmdir(dir);
        }
    }
}

static void test_rx_gather_capacity(void)
{
    uint8_t frame[60] = { 0 };
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, true);

    for (unsigned packet = 0; packet < 33; packet++) {
        uint64_t status = RAM_FULL + 0x8000 +
                          (packet / 10) * 4096 + (packet % 10) * 256;

        frame[59] = packet;
        socket_send_frame(sv[0], frame, sizeof(frame));
        wait_rx_status(qts, status);
        g_assert_cmpuint((eth_status(qts, status) >> 27) & 31, ==, packet & 31);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==,
                         8 - ((packet + 1) / 10));
    }
    qtest_clock_step(qts, 61380);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 4);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_gather_offset_change(void)
{
    uint8_t frame[1514] = { 0 }, sentinel[8192], guard[32];
    int sv[2];
    QTestState *qts = rx_interval_start(sv, 8, true);
    SgiWait wait;

    memset(sentinel, 0x55, sizeof(sentinel));
    qtest_memwrite(qts, RAM_FULL + 0x8000, sentinel, sizeof(sentinel));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (1U << 11) | (1U << 12) | (8U << 4));
    for (unsigned packet = 0; packet < 10; packet++) {
        socket_send_frame(sv[0], frame, 60);
        wait_rx_status(qts, RAM_FULL + 0x8000 + packet * 256);
    }
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 8);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (1U << 11) | (7U << 12) | (8U << 4));
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE receive after live offset change");
    while (((qtest_readq(qts, ETH_BASE + INT_STATUS) >> 25) & 31) != 11) {
        sgi_wait_pause(&wait, 1000);
        qtest_clock_step(qts, 0);
    }
    g_assert_true(eth_status(qts, RAM_FULL + 0x9000) & (UINT64_C(1) << 63));
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 6);
    qtest_memread(qts, RAM_FULL + 0x8fe0, guard, sizeof(guard));
    g_assert_cmpmem(guard, sizeof(guard), sentinel, sizeof(guard));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_gather_limits(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        uint8_t frame[1514] = { 0 };
        unsigned length = mode == 0 ? sizeof(frame) : 60;
        unsigned count = mode == 3 ? 3 : 8;
        int sv[2];
        QTestState *qts = rx_interval_start(sv, count, mode != 2);

        if (mode == 1) {
            qtest_writeq(qts, ETH_BASE + RX_DELAY, 0);
        }
        socket_send_frame(sv[0], frame, length);
        wait_rx_status(qts, RAM_FULL + 0x8000);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==,
                         count - 1);
        socket_send_frame(sv[0], frame, length);
        wait_rx_status(qts, RAM_FULL + 0x9000);
        g_assert_cmphex(eth_status(qts, RAM_FULL + 0x8100), ==, 0);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==,
                         count - 2);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_checksum_vectors(void)
{
    static const uint16_t sums[] = {
        0x44b8, 0x1002, 0xdd53, 0x6627, 0x1a8d, 0x3be7
    };
    static const uint8_t fcs[][4] = {
        { 0xee, 0x7f, 0xec, 0xb0 }, { 0x0a, 0xb0, 0x6f, 0xba },
        { 0x32, 0x25, 0x05, 0x6a }, { 0x83, 0xa6, 0xde, 0xdb },
        { 0x08, 0x89, 0x12, 0x04 }, { 0xf4, 0x65, 0x47, 0x81 },
    };
    uint8_t frame[1514], landed[1600], expected[8];
    QTestState *qts;
    int sv[2];

    for (unsigned i = 0; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    for (unsigned i = 0; i < G_N_ELEMENTS(sums); i++) {
        unsigned length = i < 4 ? 60 + i : i == 4 ? 60 : 1514;
        uint64_t buffer = RAM_FULL + 0x8000 + i * 4096;
        uint64_t vector = (UINT64_C(1) << 63) |
                          ((uint64_t)sums[i] << 32) |
                          ((uint64_t)i << 27) | (i == 5 ? 1U << 20 : 0) |
                          (i == 4 ? 1U << 26 : 0) |
                          (length + 4);

        if (i >= 4) {
            memset(frame, i == 4 ? 0 : 0xff, length);
        }
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, buffer);
        socket_send_frame(sv[0], frame, length);
        wait_fifo_count(qts, 0);
        for (unsigned j = 0; j < sizeof(expected); j++) {
            expected[j] = vector >> (56 - j * 8);
        }
        qtest_memread(qts, buffer, landed, sizeof(landed));
        g_assert_cmpmem(landed, 8, expected, 8);
        g_assert_cmphex(landed[32] | landed[33], ==, 0);
        g_assert_cmpmem(landed + 34, length, frame, length);
        g_assert_cmpmem(landed + 34 + length, 4, fcs[i], 4);
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
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
    g_assert_cmpmem(status, 2, expected_status, 2);
    g_assert_cmpmem(status + 4, 4, expected_status + 4, 4);
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
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 1U << 5);
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | (1U << 9));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 1U << 5);
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    {
        g_assert_null(strstr(log_text, "RX partial checksum is unsupported"));
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

static const struct {
    uint16_t checksum;
    uint8_t fcs[4];
} rx_progress_vectors[] = {
    { 0x679a, { 0x13, 0x02, 0x45, 0x90 } },
    { 0xd557, { 0x10, 0x26, 0x01, 0x74 } },
    { 0x04e6, { 0x8b, 0xb3, 0x00, 0xbf } },
    { 0x0a20, { 0xa0, 0x2d, 0x3c, 0xc9 } },
    { 0xacc8, { 0x7d, 0x26, 0x4d, 0xc3 } },
    { 0x5a6f, { 0x64, 0x61, 0x5f, 0x78 } },
    { 0x7d35, { 0x17, 0x4e, 0x1a, 0x9c } },
    { 0xe6bf, { 0x1f, 0xb2, 0xc7, 0x0c } },
    { 0x7c89, { 0x8e, 0x9b, 0x39, 0x37 } },
    { 0x3a52, { 0xfc, 0x15, 0xd4, 0xcf } },
    { 0xd760, { 0x12, 0xaf, 0xa5, 0x8f } },
    { 0xd9e6, { 0x62, 0xbd, 0xa1, 0x4f } },
    { 0x8579, { 0x0c, 0xb1, 0xee, 0x37 } },
    { 0x82b3, { 0x00, 0xed, 0x42, 0x80 } },
    { 0xcf5c, { 0xd6, 0xa9, 0x03, 0xb7 } },
    { 0x3277, { 0x73, 0x4c, 0x15, 0x79 } },
    { 0x23b0, { 0xa0, 0xaa, 0x24, 0x9e } },
    { 0xa702, { 0x07, 0x6a, 0x8c, 0x7b } },
    { 0x3610, { 0xd1, 0x40, 0x9c, 0xfc } },
    { 0x716d, { 0x24, 0x3f, 0xd0, 0xa4 } },
    { 0xb3a0, { 0x20, 0xf9, 0x61, 0x68 } },
    { 0x8c3c, { 0x4b, 0xa9, 0x5a, 0x9e } },
    { 0xeba5, { 0x71, 0x0c, 0xdf, 0xee } },
    { 0x4c11, { 0xa2, 0xd5, 0x59, 0xdb } },
    { 0x7225, { 0x79, 0xef, 0xf2, 0x1e } },
    { 0x1cad, { 0x93, 0x9c, 0xcd, 0x41 } },
    { 0xfa79, { 0xad, 0xdb, 0xdc, 0x19 } },
    { 0x8a16, { 0x41, 0x0d, 0x23, 0xcf } },
    { 0xc163, { 0x7a, 0x88, 0x6b, 0xeb } },
    { 0xf4e0, { 0xba, 0xde, 0xaa, 0x5c } },
    { 0x4254, { 0x6b, 0x3c, 0x92, 0xbc } },
    { 0xd6cb, { 0x9c, 0xfd, 0x40, 0xbd } },
    { 0xbc42, { 0x07, 0x8e, 0x06, 0xee } },
    { 0xc384, { 0xd6, 0x64, 0x8a, 0xa2 } },
};

#define RX_PROGRESS_BASE (RAM_FULL + 0x80000)

static void rx_progress_assert(QTestState *qts, unsigned read, unsigned write,
                               unsigned count, unsigned sequence)
{
    uint64_t expected = ((uint64_t)(write & 31) << 16) |
                        ((read & 31) << 8) | count;
    uint64_t status = qtest_readq(qts, ETH_BASE + INT_STATUS);

    for (unsigned alias = 0; alias < 3; alias++) {
        uint64_t info = qtest_readq(qts, ETH_BASE + RX_FIFO_INFO + alias * 8);

        g_assert_cmphex(info, ==, expected);
        g_assert_cmphex(status & 0x1f00, ==, info & 0x1f00);
        g_assert_cmpuint((info >> 8) & 15, ==, read % 16);
        g_assert_cmpuint((info >> 12) & 1, ==, (read / 16) % 2);
    }
    g_assert_cmpuint((status >> 25) & 31, ==, sequence % 32);
}

static void rx_progress_post(QTestState *qts, unsigned slot)
{
    uint8_t cleared[128] = { 0 };
    uint64_t address = RX_PROGRESS_BASE + slot * 4096;

    qtest_memwrite(qts, address, cleared, sizeof(cleared));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, address);
}

static void rx_progress_enable(QTestState *qts)
{
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12) | (1U << 9) | (16U << 4));
}

static QTestState *rx_progress_start(int sv[2])
{
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    rx_progress_assert(qts, 0, 0, 0, 0);
    for (unsigned slot = 0; slot < 16; slot++) {
        rx_progress_post(qts, slot);
    }
    rx_progress_assert(qts, 0, 16, 16, 0);
    rx_progress_enable(qts);
    return qts;
}

static void rx_progress_complete(QTestState *qts, int fd, unsigned done,
                                 unsigned sequence, unsigned marker)
{
    uint8_t frame[60], landed[128];
    uint64_t address = RX_PROGRESS_BASE + (done - 1) * 4096;
    uint64_t expected = (UINT64_C(1) << 63) |
                        ((uint64_t)rx_progress_vectors[marker].checksum << 32) |
                        ((uint64_t)(sequence % 32) << 27) | (1U << 26) | 64;

    rx_sequence_frame(frame, marker);
    socket_send_frame(fd, frame, sizeof(frame));
    wait_fifo_count(qts, 15);
    g_assert_cmphex(eth_status(qts, address), ==, expected);
    qtest_memread(qts, address, landed, sizeof(landed));
    g_assert_cmphex(landed[32] | landed[33], ==, 0);
    g_assert_cmpmem(landed + 34, sizeof(frame), frame, sizeof(frame));
    g_assert_cmpmem(landed + 94, 4, rx_progress_vectors[marker].fcs, 4);
    rx_progress_assert(qts, done, done + 15, 15, sequence + 1);
    rx_progress_post(qts, done + 15);
    rx_progress_assert(qts, done, done + 16, 16, sequence + 1);
}

static void test_rx_progress(void)
{
    int sv[2];
    QTestState *qts = rx_progress_start(sv);

    for (unsigned done = 1; done <= 33; done++) {
        rx_progress_complete(qts, sv[0], done, done - 1, done - 1);
        if (done == 17) {
            uint64_t status = qtest_readq(qts, ETH_BASE + INT_STATUS);

            for (unsigned i = 0; i < 3; i++) {
                g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS), ==,
                                status);
            }
            qtest_writeq(qts, ETH_BASE + INT_STATUS, 0);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS), ==,
                            status);
            qtest_writeq(qts, ETH_BASE + INT_STATUS, 0x1f00);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS), ==,
                            status);
            g_assert_cmphex(status & 0xff, !=, 0);
            qtest_writeq(qts, ETH_BASE + INT_STATUS, status & 0xff);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0xff,
                            ==, 0);
            rx_progress_assert(qts, done, done + 16, 16, done);
        }
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_progress_reset(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        int sv[2];
        QTestState *qts = rx_progress_start(sv);
        unsigned sequence = mode == 0 ? 0 : 17;

        for (unsigned done = 1; done <= 17; done++) {
            rx_progress_complete(qts, sv[0], done, done - 1, done - 1);
        }
        if (mode == 0) {
            qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
        } else {
            if (mode == 3) {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
            }
            if (mode != 2) {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
                rx_progress_assert(qts, 17, 33, 16, 17);
            }
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
            if (mode == 2) {
                rx_progress_assert(qts, 17, 33, 16, 17);
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
            }
        }
        rx_progress_assert(qts, 0, 0, 0, sequence);
        for (unsigned slot = 0; slot < 16; slot++) {
            rx_progress_post(qts, slot);
        }
        rx_progress_assert(qts, 0, 16, 16, sequence);
        rx_progress_enable(qts);
        rx_progress_complete(qts, sv[0], 1, sequence, 33);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_progress_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-rx-progress-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t before[33][128], after[128];
    int sv[2], dest_sv[2];
    QTestState *from = rx_progress_start(sv);
    QTestState *to;

    for (unsigned done = 1; done <= 17; done++) {
        rx_progress_complete(from, sv[0], done, done - 1, done - 1);
    }
    for (unsigned slot = 0; slot < 33; slot++) {
        qtest_memread(from, RX_PROGRESS_BASE + slot * 4096, before[slot], 128);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, dest_sv), ==, 0);
    to = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 -S "
                     "-nic socket,id=n0,fd=%d,model=sgi-mace-eth "
                     "-incoming defer", dest_sv[1]);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    rx_progress_assert(to, 17, 33, 16, 17);
    for (unsigned slot = 0; slot < 33; slot++) {
        qtest_memread(to, RX_PROGRESS_BASE + slot * 4096, after, sizeof(after));
        g_assert_cmpmem(before[slot], sizeof(after), after, sizeof(after));
    }
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    for (unsigned done = 18; done <= 33; done++) {
        rx_progress_complete(to, dest_sv[0], done, done - 1, done - 1);
    }
    qtest_quit(to);
    qtest_quit(from);
    close(sv[0]);
    close(sv[1]);
    close(dest_sv[0]);
    close(dest_sv[1]);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
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
            g_assert_cmphex(status[1], ==, 0);
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

static QTestState *start_rx_full_fifo(int sv[2], bool dma_enabled,
                                      const char *log_path,
                                      uint8_t old_snapshot[21][128])
{
    uint8_t old_buffer[128], frame[60];
    QTestState *qts;

    memset(old_buffer, 0xa5, sizeof(old_buffer));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth,"
                      "mac=02:a1:4c:73:95:e6 -d guest_errors -D %s",
                      sv[1], log_path);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
    for (unsigned i = 0; i < 21; i++) {
        qtest_memwrite(qts, RX_REINIT_OLD_BASE + i * 0x1000,
                       old_buffer, sizeof(old_buffer));
    }
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RX_REINIT_OLD_BASE + i * 0x1000);
    }
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    if (dma_enabled) {
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
        for (unsigned i = 0; i < 5; i++) {
            rx_sequence_frame(frame, 20 + i);
            socket_send_frame(sv[0], frame, sizeof(frame));
        }
        wait_fifo_count(qts, 11);
    } else {
        for (unsigned i = 0; i < 5; i++) {
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT), ==,
                            RX_REINIT_OLD_BASE + i * 0x1000);
        }
    }
    for (unsigned i = 16; i < 21; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RX_REINIT_OLD_BASE + i * 0x1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    for (unsigned i = 0; i < 21; i++) {
        qtest_memread(qts, RX_REINIT_OLD_BASE + i * 0x1000,
                      old_snapshot[i], sizeof(old_snapshot[i]));
    }
    return qts;
}

static void test_rx_full_fifo_reinitialization(unsigned mode)
{
    static const uint8_t expected_fcs[8][4] = {
        { 0x37, 0xb6, 0xe8, 0x31 }, { 0x38, 0x81, 0x6c, 0x50 },
        { 0x29, 0x21, 0x56, 0xb4 }, { 0x4e, 0xa4, 0xb6, 0x14 },
        { 0x79, 0xdc, 0x65, 0x23 }, { 0x2b, 0x44, 0xd7, 0x91 },
        { 0x4f, 0x52, 0x6a, 0xbe }, { 0x98, 0x16, 0x72, 0xc9 },
    };
    uint8_t frames[8][60], old_snapshot[21][128], old_after[128];
    uint8_t new_buffer[128], status[8], padding[2], landed[60], fcs[4];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-reinit-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, mode != 2, log_path,
                                          old_snapshot);

    if (mode == 1) {
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x150510));
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    } else {
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x150510));
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);

    memset(new_buffer, 0xa5, sizeof(new_buffer));
    for (unsigned i = 0; i < 16; i++) {
        uint64_t address = RX_REINIT_NEW_BASE + i * 0x1000;

        qtest_memwrite(qts, address, new_buffer, sizeof(new_buffer));
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, address);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x100010));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    for (unsigned i = 0; i < 8; i++) {
        rx_sequence_frame(frames[i], 40 + i);
        socket_send_frame(sv[0], frames[i], sizeof(frames[i]));
    }
    wait_fifo_count(qts, 8);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x100808));
    for (unsigned i = 0; i < 8; i++) {
        uint64_t address = RX_REINIT_NEW_BASE + i * 0x1000;
        uint8_t expected_status[8] = {
            0x80, 0, 0, 0,
            (((mode == 2 ? 0 : 5) + i) << 3) | 0x04,
            0, 0, 64
        };

        qtest_memread(qts, address, status, sizeof(status));
        g_assert_cmpmem(status, 2, expected_status, 2);
        g_assert_cmpmem(status + 4, 4, expected_status + 4, 4);
        qtest_memread(qts, address + 32, padding, sizeof(padding));
        g_assert_cmphex(padding[0] | padding[1], ==, 0);
        qtest_memread(qts, address + 34, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), frames[i], sizeof(frames[i]));
        qtest_memread(qts, address + 94, fcs, sizeof(fcs));
        g_assert_cmpmem(fcs, sizeof(fcs), expected_fcs[i],
                        sizeof(expected_fcs[i]));
    }
    for (unsigned i = 0; i < 21; i++) {
        qtest_memread(qts, RX_REINIT_OLD_BASE + i * 0x1000,
                      old_after, sizeof(old_after));
        g_assert_cmpmem(old_after, sizeof(old_after), old_snapshot[i],
                        sizeof(old_snapshot[i]));
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0xc0, ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_null(strstr(log_text, "RX address FIFO overrun"));
    g_assert_null(strstr(log_text, "RX DMA failed"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_stop_then_reset(void)
{
    test_rx_full_fifo_reinitialization(0);
}

static void test_rx_reinit_reset_then_stop(void)
{
    test_rx_full_fifo_reinitialization(1);
}

static void test_rx_reinit_already_disabled(void)
{
    test_rx_full_fifo_reinitialization(2);
}

static void test_rx_reinit_after_rx_progress(void)
{
    static const uint8_t expected_status[8] = {
        0x80, 0, 0, 0, (5 << 3) | 0x04, 0, 0, 64
    };
    uint8_t old_snapshot[21][128], frame[60], status[8], landed[60];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-reinit-progress-XXXXXX",
                                           NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, true, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    rx_sequence_frame(frame, 25);
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_fifo_count(qts, 15);
    qtest_memread(qts, RX_REINIT_OLD_BASE + 5 * 0x1000,
                  status, sizeof(status));
    g_assert_cmpmem(status, 2, expected_status, 2);
    g_assert_cmpmem(status + 4, 4, expected_status + 4, 4);
    qtest_memread(qts, RX_REINIT_OLD_BASE + 5 * 0x1000 + 34,
                  landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);
    for (unsigned i = 0; i < 16; i++) {
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                     RX_REINIT_NEW_BASE + i * 0x1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x100010));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_null(strstr(log_text, "RX address FIFO overrun"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_core_only(void)
{
    uint8_t old_snapshot[21][128];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-core-only-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, true, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    qtest_quit(qts);
    g_remove(log_path);
    g_rmdir(dir);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_reenable_discards_pulse(void)
{
    uint8_t old_snapshot[21][128];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-reenable-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, true, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    qtest_quit(qts);
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_dma_only(void)
{
    uint8_t old_snapshot[21][128];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-dma-only-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, false, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    qtest_quit(qts);
    g_remove(log_path);
    g_rmdir(dir);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_other_ring_size(void)
{
    uint8_t old_snapshot[21][128];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-ring-size-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, false, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 1U << 2);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    0);
    qtest_quit(qts);
    g_remove(log_path);
    g_rmdir(dir);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_reinit_active_16k_then_zero(void)
{
    uint8_t old_snapshot[21][128];
    g_autofree char *dir = g_dir_make_tmp("mace-rx-active-16k-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    int sv[2];
    QTestState *qts = start_rx_full_fifo(sv, false, log_path, old_snapshot);

    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (1U << 2));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    UINT64_C(0x150510));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==,
                    0);
    qtest_quit(qts);
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
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, valid_rx);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_fifo_count(qts, 0);
    qtest_memread(qts, valid_rx, rx_status, sizeof(rx_status));
    g_assert_cmphex(rx_status[0], ==, 0x80);
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
        g_assert_nonnull(failure);
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
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, outside_low);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
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
    g_assert_true(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE low RX rejects an unpopulated bank");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 5000);
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 1);
    qtest_readq(qts, ETH_BASE + RX_FIFO_PORT);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, outside_low + 0x8000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (4U << 12));
    g_assert_true(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
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
    g_assert_null(strstr(log_text, "RX gathering is unsupported"));
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
    g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & (1U << 7));
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

static void test_rx_reinit_pending_migrates(void)
{
    for (unsigned mode = 0; mode < 3; mode++) {
        g_autofree char *dir = g_dir_make_tmp("mace-rx-reinit-migrate-XXXXXX",
                                               NULL);
        g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        QTestState *from = mace_standalone_start("");
        QTestState *to = mace_standalone_start("-incoming defer");

        qtest_writeq(from, ETH_BASE + MAC_CONTROL, 0);
        for (unsigned i = 0; i < 16; i++) {
            qtest_writeq(from, ETH_BASE + RX_FIFO_PORT,
                         RX_REINIT_OLD_BASE + i * 0x1000);
        }
        for (unsigned i = 0; i < 5; i++) {
            g_assert_cmphex(qtest_readq(from, ETH_BASE + RX_FIFO_PORT), ==,
                            RX_REINIT_OLD_BASE + i * 0x1000);
            qtest_writeq(from, ETH_BASE + RX_FIFO_PORT,
                         RX_REINIT_OLD_BASE + (16 + i) * 0x1000);
        }
        g_assert_cmphex(qtest_readq(from, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x150510));
        qtest_writeq(from, ETH_BASE + DMA_CONTROL,
                     (1U << 15) | (mode == 2 ? 1U << 2 : 0));
        if (mode != 1) {
            qtest_writeq(from, ETH_BASE + DMA_CONTROL, 0);
        } else {
            qtest_writeq(from, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(from, ETH_BASE + MAC_CONTROL, 0);
        }
        g_assert_cmphex(qtest_readq(from, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x150510));

        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        qtest_qmp_assert_success(to, "{'execute':'cont'}");
        g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x150510));
        if (mode != 1) {
            qtest_writeq(to, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(to, ETH_BASE + MAC_CONTROL, 0);
        } else {
            qtest_writeq(to, ETH_BASE + DMA_CONTROL, 0);
        }
        g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==, 0);
        for (unsigned i = 0; i < 16; i++) {
            qtest_writeq(to, ETH_BASE + RX_FIFO_PORT,
                         RX_REINIT_NEW_BASE + i * 0x1000);
        }
        g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==,
                        UINT64_C(0x100010));
        qtest_quit(to);
        qtest_quit(from);
        g_unlink(socket);
        g_rmdir(dir);
    }
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

static void store_be64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = value >> (56 - i * 8);
    }
}

static void test_tx_pointer_combinations(void)
{
    const uint64_t base = RAM_FULL + 0x20000;
    const uint64_t pointers[] = {
        RAM_FULL + 0xaff8, RAM_FULL + 0xb100, RAM_FULL + 0xb200
    };
    const unsigned lengths[] = { 13, 17, 19 };
    uint8_t descriptor[128], expected[128], landed[128], payload[19];
    uint32_t wire_length;
    int sv[2];
    QTestState *qts;
    unsigned producer = 0;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + TX_BASE, base);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    for (unsigned inlined = 0; inlined < 2; inlined++) {
        for (unsigned valid = inlined ? 0 : 1; valid < 8; valid++) {
            unsigned total = inlined ? 64 : 0;
            uint64_t command;

            memset(descriptor, 0, sizeof(descriptor));
            for (unsigned i = 0; i < total; i++) {
                descriptor[64 + i] = expected[i] = i * 7 + valid;
            }
            for (unsigned slot = 0; slot < 3; slot++) {
                if (!(valid & (1U << slot))) {
                    continue;
                }
                for (unsigned i = 0; i < lengths[slot]; i++) {
                    payload[i] = 0x73 + slot * 31 + i * 11 + valid;
                }
                memcpy(expected + total, payload, lengths[slot]);
                qtest_memwrite(qts, pointers[slot], payload, lengths[slot]);
                store_be64(descriptor + 8 + 8 * slot,
                            ((uint64_t)(lengths[slot] - 1) << 32) |
                            pointers[slot]);
                total += lengths[slot];
            }
            command = (total - 1) | (inlined ? 64U << 16 : 0) |
                      (1U << 24) | (valid << 25);
            store_be64(descriptor, command);
            qtest_memwrite(qts, base + producer * 128, descriptor, 128);
            qtest_writeq(qts, ETH_BASE + TX_POINTER, ++producer);
            g_assert_cmpint(recv(sv[0], &wire_length, 4, MSG_WAITALL), ==, 4);
            g_assert_cmpuint(ntohl(wire_length), ==, total);
            g_assert_cmpint(recv(sv[0], landed, total, MSG_WAITALL), ==, total);
            g_assert_cmpmem(landed, total, expected, total);
            g_assert_cmphex(eth_status(qts, base + (producer - 1) * 128), ==,
                            (UINT64_C(1) << 63) | (1U << 23) | total);
        }
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_all_ring_reinitialization(void)
{
    for (unsigned bits = 0; bits < 4; bits++) {
        for (unsigned order = 0; order < 2; order++) {
            uint8_t entry[128] = { 0 }, frame[60], got[60];
            uint8_t old[128], after[128];
            const uint64_t base = RAM_FULL + 0x20000;
            unsigned dma = (bits << 2) | (1U << 15) | (4U << 12) | 3;
            int sv[2];
            uint32_t length;
            QTestState *qts;

            g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
            socket_timeout(sv[0]);
            qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                              "-nic socket,id=n0,fd=%d,model=sgi-mace-eth",
                              sv[1]);
            mdio_write(qts, 8, 0, 0x2100);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
            qtest_writeq(qts, ETH_BASE + TX_BASE, base);
            for (unsigned i = 0; i < 16; i++) {
                qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                             RAM_FULL + 0x8000 + i * 4096);
            }
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, dma);
            for (unsigned i = 0; i < 5; i++) {
                memset(frame, 0x21 + i, sizeof(frame));
                socket_send_frame(sv[0], frame, sizeof(frame));
            }
            wait_fifo_count(qts, 11);
            for (unsigned i = 0; i < 5; i++) {
                qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                             RAM_FULL + 0x8000 + i * 4096);
            }
            store_be64(entry, (68U << 16) | 59);
            memcpy(entry + 68, frame, sizeof(frame));
            qtest_memwrite(qts, base, entry, sizeof(entry));
            qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
            g_assert_cmpint(recv(sv[0], &length, 4, MSG_WAITALL), ==, 4);
            g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                            sizeof(got));
            qtest_writeq(qts, ETH_BASE + RX_DELAY, 2);
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, dma | (1U << 11));
            socket_send_frame(sv[0], frame, sizeof(frame));
            wait_rx_status(qts, RAM_FULL + 0xd000);
            qtest_memread(qts, RAM_FULL + 0xd000, old, sizeof(old));
            if (!order) {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, bits << 2);
            }
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
            if (order) {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, bits << 2);
            }
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);
            qtest_writeq(qts, ETH_BASE + TX_BASE, base + 0x10000);
            for (unsigned i = 0; i < 16; i++) {
                qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                             RAM_FULL + 0x50000 + i * 4096);
            }
            qtest_writeq(qts, ETH_BASE + RX_DELAY, 0);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, dma);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==, 0);
            memset(frame, 0x7b, sizeof(frame));
            memcpy(entry + 68, frame, sizeof(frame));
            qtest_memwrite(qts, base + 0x10000, entry, sizeof(entry));
            qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
            g_assert_cmpint(recv(sv[0], &length, 4, MSG_WAITALL), ==, 4);
            g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                            sizeof(got));
            g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
            for (unsigned i = 0; i < 16; i++) {
                memset(frame, 0x50 + i, sizeof(frame));
                socket_send_frame(sv[0], frame, sizeof(frame));
            }
            wait_fifo_count(qts, 0);
            for (unsigned i = 0; i < 16; i++) {
                qtest_memread(qts, RAM_FULL + 0x50000 + i * 4096 + 34,
                              got, sizeof(got));
                memset(frame, 0x50 + i, sizeof(frame));
                g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
            }
            qtest_clock_step(qts, 61380);
            qtest_memread(qts, RAM_FULL + 0xd000, after, sizeof(after));
            g_assert_cmpmem(after, sizeof(after), old, sizeof(old));
            qtest_quit(qts);
            close(sv[0]);
            close(sv[1]);
        }
    }
}

static void test_tx_all_ring_sizes(void)
{
    for (unsigned bits = 0; bits < 4; bits++) {
        unsigned entries = 64U << bits;
        uint8_t descriptor[128] = { 0 }, frame[60], got[60];
        uint32_t length;
        int sv[2];
        QTestState *qts;
        const uint64_t base = RAM_FULL + 0x20000;

        g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
        socket_timeout(sv[0]);
        qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                          "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
        mdio_write(qts, 8, 0, 0x2100);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        qtest_writeq(qts, ETH_BASE + TX_BASE, base);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3 | (bits << 2));
        for (unsigned packet = 0; packet < entries + 2; packet++) {
            unsigned index = packet % entries;
            unsigned next = (index + 1) % entries;
            uint64_t pointer = ((uint64_t)next << 16) | next;

            tx_sequence_frame(frame, packet);
            store_be64(descriptor, (UINT64_C(1) << 24) | (68U << 16) | 59);
            memcpy(descriptor + 68, frame, sizeof(frame));
            qtest_memwrite(qts, base + index * 128, descriptor, 128);
            qtest_writeq(qts, ETH_BASE + TX_POINTER, next);
            g_assert_cmpint(recv(sv[0], &length, 4, MSG_WAITALL), ==, 4);
            g_assert_cmpuint(ntohl(length), ==, sizeof(frame));
            g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                            sizeof(got));
            g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
            g_assert_cmphex(eth_status(qts, base + index * 128), ==,
                            (UINT64_C(1) << 63) | (1U << 23) | 60);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==,
                            pointer);
            g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER + 8), ==,
                            pointer);
            if (bits < 3) {
                qtest_writeq(qts, ETH_BASE + TX_POINTER, entries);
                g_assert_cmphex(qtest_readq(qts, ETH_BASE + TX_POINTER), ==,
                                pointer);
            }
        }
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3 | (((bits + 1) & 3) << 2));
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 12, ==,
                        bits << 2);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_tx_reserved_descriptor(void)
{
    uint8_t descriptor[128], landed[128], got[64];
    const uint64_t base = RAM_FULL + 0x20000;
    const uint64_t payload = RAM_FULL + 0xa000;
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf("-M o2-test,phy-ics1890=on,phy-address=8 "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    mdio_write(qts, 8, 0, 0x2100);
    for (unsigned mode = 0; mode < 9; mode++) {
        uint64_t command = (1U << 25) | 59;
        uint64_t pointer = (UINT64_C(59) << 32) | payload;

        memset(descriptor, 0, sizeof(descriptor));
        switch (mode) {
        case 0:
            command = (UINT64_C(1) << 40) | (68U << 16) | 59;
            break;
        case 1:
            pointer |= UINT64_C(1) << 48;
            break;
        case 2:
            pointer |= 1;
            break;
        case 3:
            pointer = (UINT64_C(19) << 32) | payload;
            break;
        case 4:
            pointer = (UINT64_C(60) << 32) | payload;
            break;
        case 5:
            command = (4U << 25) | (16U << 16) | 149;
            break;
        case 6:
            command = (127U << 16) | 59;
            break;
        case 7:
            command = (1U << 25) | 2048;
            pointer = (UINT64_C(2048) << 32) | payload;
            break;
        case 8:
            command = (3U << 25) | 59;
            pointer = (UINT64_C(29) << 32) | 0xf0000000;
            store_be64(descriptor + 16,
                        (UINT64_C(1) << 48) | (UINT64_C(29) << 32) | payload);
            break;
        }
        store_be64(descriptor, command);
        store_be64(descriptor + 8, pointer);
        qtest_memwrite(qts, base, descriptor, sizeof(descriptor));
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        qtest_writeq(qts, ETH_BASE + TX_BASE, base);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 0xff);
        qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
        g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x18, ==, 16);
        g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_DONTWAIT), ==, -1);
        g_assert_cmpint(errno, ==, EAGAIN);
        qtest_memread(qts, base, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), descriptor, sizeof(descriptor));
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

#define FAULT_MEMORY UINT64_C(0x100000000)

static void fault_set(QTestState *qts, const char *property, uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/memtx',"
        "'property':%s,'value':%" PRIu64 "}}", property, value);
}

static QTestState *fault_eth_start_socket(int sv[2], bool incoming, bool legacy,
                                         int type)
{
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, type, 0, sv), ==, 0);
    socket_timeout(sv[0]);
    qts = qtest_initf(
        "-M none -cpu R10000 -nic none "
        "-device memory-transaction-test,id=memtx,fault-memory=on "
        "-device %s,id=mace,phy-ics1890=on,phy-address=8,"
        "ram=/machine/peripheral/memtx/test-memory[0] "
        "-nic socket,id=n0,fd=%d,model=sgi-mace-eth %s",
        legacy ? "sgi-mace-rx-threshold-test" : "sgi-mace", sv[1],
        incoming ? "-incoming defer" : "");
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/mace", 0, 0x1f000000);
    if (!incoming) {
        mdio_write(qts, 8, 0, 0x2100);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    }
    return qts;
}

static QTestState *fault_eth_start_mode(int sv[2], bool incoming, bool legacy)
{
    return fault_eth_start_socket(sv, incoming, legacy, SOCK_STREAM);
}

static QTestState *fault_eth_start_full(int sv[2], bool incoming)
{
    return fault_eth_start_mode(sv, incoming, false);
}

static QTestState *fault_eth_start(int sv[2])
{
    return fault_eth_start_full(sv, false);
}

static void test_dma_fault_memory(void)
{
    uint8_t before[16], after[16];
    int sv[2];
    QTestState *qts = fault_eth_start(sv);

    memset(before, 0x59, sizeof(before));
    qtest_memwrite(qts, FAULT_MEMORY, before, sizeof(before));
    fault_set(qts, "fault-start", 8);
    fault_set(qts, "fault-end", 15);
    fault_set(qts, "fault-mode", 2);
    g_assert_cmpint(memtx_writeq(qts, FAULT_MEMORY + 8, 0), ==, MEMTX_ERROR);
    fault_set(qts, "fault-mode", 0);
    qtest_memread(qts, FAULT_MEMORY, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    g_assert_cmpint(memtx_writeq(qts, FAULT_MEMORY, 0x12345678), ==, MEMTX_OK);
    {
        uint64_t value;

        g_assert_cmpint(memtx_readq(qts, FAULT_MEMORY, &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0x12345678);
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_dma_faults(void)
{
    for (unsigned phase = 0; phase < 3; phase++) {
        uint8_t frame[60] = { 0 }, sentinel[512], landed[512];
        int sv[2];
        QTestState *qts = fault_eth_start(sv);
        SgiWait wait;
        uint64_t first;

        memset(sentinel, 0x5a, sizeof(sentinel));
        qtest_memwrite(qts, FAULT_MEMORY + 0x8000, sentinel, sizeof(sentinel));
        for (unsigned i = 0; i < 8; i++) {
            qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                         RAM_FULL + 0x8000 + 4096 * i);
        }
        qtest_writeq(qts, ETH_BASE + RX_DELAY, 2);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                     (1U << 15) | (1U << 11) | (4U << 12) | (8U << 4));
        socket_send_frame(sv[0], frame, sizeof(frame));
        wait_rx_status(qts, FAULT_MEMORY + 0x8000);
        first = eth_status(qts, FAULT_MEMORY + 0x8000);
        fault_set(qts, "fault-start", phase == 0 ? 0x8122 :
                  phase == 1 ? 0x8100 : 0x8160);
        fault_set(qts, "fault-end", phase == 0 ? 0x815d :
                  phase == 1 ? 0x8107 : 0x8163);
        fault_set(qts, "fault-mode", 2);
        socket_send_frame(sv[0], frame, sizeof(frame));
        sgi_wait_start(&wait, "MACE RX fault stops DMA");
        while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
            sgi_wait_pause(&wait, 1000);
            qtest_clock_step(qts, 0);
        }
        fault_set(qts, "fault-mode", 0);
        qtest_memread(qts, FAULT_MEMORY + 0x8000, landed, sizeof(landed));
        g_assert_cmphex(eth_status(qts, FAULT_MEMORY + 0x8000), ==, first);
        g_assert_cmpmem(landed + 0x100, 8, sentinel, 8);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 8);
        qtest_clock_step(qts, 61380);
        g_assert_false(qtest_readq(qts, ETH_BASE + INT_STATUS) & 32);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                     (1U << 15) | (1U << 11) | (4U << 12));
        g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
        g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 0xff);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_PORT), ==,
                        RAM_FULL + 0x8000);
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x7000);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
        g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
        if (phase == 0) {
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
        }
        if (phase < 2) {
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
            if (phase == 1) {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
            }
        } else {
            qtest_system_reset(qts);
            mdio_write(qts, 8, 0, 0x2100);
        }
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO), ==, 0);
        {
            uint8_t preserved[512];

            qtest_memread(qts, FAULT_MEMORY + 0x8000,
                          preserved, sizeof(preserved));
            g_assert_cmpmem(preserved, sizeof(preserved),
                            landed, sizeof(landed));
        }
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x1000);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
        g_assert_true(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
        socket_send_frame(sv[0], frame, sizeof(frame));
        wait_fifo_count(qts, 0);
        g_assert_true(eth_status(qts, FAULT_MEMORY + 0x1000) &
                      (UINT64_C(1) << 63));
        g_assert_cmpuint((eth_status(qts, FAULT_MEMORY + 0x1000) >> 27) & 31,
                         ==, phase == 2 ? 0 : 1);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_fault_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-rx-fault-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t frame[60] = { 0 }, before[128], after[128];
    int sv[2], dest_sv[2];
    QTestState *from = fault_eth_start(sv);
    QTestState *to = fault_eth_start_full(dest_sv, true);
    SgiWait wait;
    int64_t clock;

    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(from, ETH_BASE + RX_FIFO_PORT,
                     RAM_FULL + 0x8000 + 4096 * i);
    }
    qtest_writeq(from, ETH_BASE + RX_DELAY, 2);
    qtest_writeq(from, ETH_BASE + DMA_CONTROL,
                 (1U << 15) | (1U << 11) | (4U << 12) | (8U << 4));
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(from, FAULT_MEMORY + 0x8000);
    qtest_memread(from, FAULT_MEMORY + 0x8000, before, sizeof(before));
    fault_set(from, "fault-start", 0x8100);
    fault_set(from, "fault-end", 0x8107);
    fault_set(from, "fault-mode", 2);
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "MACE RX status fault before migration");
    while (qtest_readq(from, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 1000);
        qtest_clock_step(from, 0);
    }
    fault_set(from, "fault-mode", 0);
    clock = qtest_clock_step(from, 1);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    qtest_clock_set(to, clock);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_writeq(to, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    g_assert_false(qtest_readq(to, ETH_BASE + DMA_CONTROL) & (1U << 15));
    qtest_clock_step(to, 61380);
    g_assert_false(qtest_readq(to, ETH_BASE + INT_STATUS) & 32);
    g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31, ==, 8);
    qtest_memread(to, FAULT_MEMORY + 0x8000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), before, sizeof(before));
    qtest_writeq(to, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(to, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(to, ETH_BASE + MAC_CONTROL, 0);
    g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==, 0);
    qtest_writeq(to, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x1000);
    qtest_writeq(to, ETH_BASE + DMA_CONTROL, (1U << 15) | (4U << 12));
    socket_send_frame(dest_sv[0], frame, sizeof(frame));
    wait_fifo_count(to, 0);
    g_assert_true(eth_status(to, FAULT_MEMORY + 0x1000) & (UINT64_C(1) << 63));
    qtest_quit(to);
    qtest_quit(from);
    close(sv[0]);
    close(sv[1]);
    close(dest_sv[0]);
    close(dest_sv[1]);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_tx_status_fault(void)
{
    uint8_t descriptor[128] = { 0 }, got[60], frame[60];
    uint32_t length;
    int sv[2];
    QTestState *qts = fault_eth_start(sv);

    memset(frame, 0x37, sizeof(frame));
    store_be64(descriptor, (68U << 16) | 59);
    memcpy(descriptor + 68, frame, sizeof(frame));
    qtest_memwrite(qts, FAULT_MEMORY + 0x2000, descriptor, sizeof(descriptor));
    fault_set(qts, "fault-start", 0x2000);
    fault_set(qts, "fault-end", 0x2007);
    fault_set(qts, "fault-mode", 2);
    qtest_writeq(qts, ETH_BASE + TX_BASE, RAM_FULL + 0x2000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_cmpint(recv(sv[0], &length, 4, MSG_WAITALL), ==, 4);
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_WAITALL), ==,
                    sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & 2);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    fault_set(qts, "fault-mode", 0);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 8);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 3);
    g_assert_cmpint(recv(sv[0], got, sizeof(got), MSG_DONTWAIT), ==, -1);
    g_assert_cmpint(errno, ==, EAGAIN);
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
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

static uint64_t rx_retirement_control(unsigned threshold, bool gather)
{
    return (1U << 15) | (1U << 9) | (4U << 12) | (threshold << 4) |
           (gather ? 1U << 11 : 0);
}

static void rx_retirement_config(QTestState *qts, unsigned count,
                                 unsigned delay, bool gather)
{
    uint8_t guard[4096];

    memset(guard, 0x59, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    for (unsigned i = 0; i < count; i++) {
        unsigned offset = (0x8000 + 4096 * i) & 0xffff;

        qtest_memwrite(qts, FAULT_MEMORY + offset, guard, sizeof(guard));
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + offset);
    }
    qtest_writeq(qts, ETH_BASE + RX_DELAY, delay);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 rx_retirement_control(count, gather));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 0xff);
}

static QTestState *rx_retirement_start(int sv[2], unsigned count,
                                      unsigned delay, bool gather)
{
    QTestState *qts = fault_eth_start_mode(sv, false, true);

    qtest_irq_intercept_out_named(qts, "/machine/peripheral/mace", "crime");
    rx_retirement_config(qts, count, delay, gather);
    return qts;
}

static void rx_retirement_packet(QTestState *qts, int fd, unsigned offset,
                                 unsigned marker)
{
    uint8_t frame[60], landed[128];
    uint64_t vector;

    rx_sequence_frame(frame, marker);
    socket_send_frame(fd, frame, sizeof(frame));
    wait_rx_status(qts, FAULT_MEMORY + offset);
    qtest_memread(qts, FAULT_MEMORY + offset, landed, sizeof(landed));
    vector = eth_status(qts, FAULT_MEMORY + offset);
    g_assert_cmphex(vector >> 63, ==, 1);
    g_assert_cmpuint(vector & 0xffff, ==, 64);
    g_assert_cmphex(landed[32], ==, 0);
    g_assert_cmphex(landed[33], ==, 0);
    g_assert_cmpmem(landed + 34, sizeof(frame), frame, sizeof(frame));
    g_assert_cmpuint(marker, <, G_N_ELEMENTS(rx_progress_vectors));
    g_assert_cmphex((vector >> 32) & 0xffff, ==,
                    rx_progress_vectors[marker].checksum);
    g_assert_cmpmem(landed + 94, 4, rx_progress_vectors[marker].fcs, 4);
    for (unsigned i = 98; i < sizeof(landed); i++) {
        g_assert_cmphex(landed[i], ==, 0x59);
    }
}

static bool rx_threshold_event(QTestState *qts)
{
    return qtest_readq(qts, ETH_BASE + INT_STATUS) & 32;
}

static void rx_host_offer(int fd, unsigned marker)
{
    uint8_t frame[60];

    rx_sequence_frame(frame, marker);
    g_assert_cmpint(send(fd, frame, sizeof(frame), 0), ==, sizeof(frame));
}

static void rx_host_packet(QTestState *qts, unsigned offset, unsigned marker,
                           unsigned sequence)
{
    uint8_t frame[60], landed[128];
    uint64_t vector;

    rx_sequence_frame(frame, marker);
    wait_rx_status(qts, FAULT_MEMORY + offset);
    qtest_memread(qts, FAULT_MEMORY + offset, landed, sizeof(landed));
    vector = eth_status(qts, FAULT_MEMORY + offset);
    g_assert_cmpuint((vector >> 27) & 31, ==, sequence & 31);
    g_assert_cmpuint(vector & 0xffff, ==, 64);
    g_assert_cmphex((vector >> 32) & 0xffff, ==,
                    rx_progress_vectors[marker].checksum);
    g_assert_cmpmem(landed + 34, sizeof(frame), frame, sizeof(frame));
    g_assert_cmpmem(landed + 94, 4, rx_progress_vectors[marker].fcs, 4);
    for (unsigned i = 8; i < 32; i++) {
        g_assert_cmphex(landed[i], ==, 0x59);
    }
    g_assert_cmphex(landed[32], ==, 0);
    g_assert_cmphex(landed[33], ==, 0);
    for (unsigned i = 98; i < sizeof(landed); i++) {
        g_assert_cmphex(landed[i], ==, 0x59);
    }
}

static void test_rx_host_admission(void)
{
    for (unsigned grouped = 0; grouped < 2; grouped++) {
        int sv[2];
        QTestState *qts = fault_eth_start_socket(sv, false, true, SOCK_DGRAM);
        uint8_t guard[128], before[sizeof(guard)];
        uint64_t control = rx_retirement_control(2, false);

        memset(guard, 0x59, sizeof(guard));
        rx_retirement_config(qts, 2, 0, false);
        for (unsigned i = 0; i < 4; i++) {
            rx_host_offer(sv[0], i);
            if (!grouped && i < 2) {
                rx_host_packet(qts, 0x8000 + 4096 * i, i, i);
            }
        }
        rx_host_packet(qts, 0x8000, 0, 0);
        rx_host_packet(qts, 0x9000, 1, 1);
        wait_fifo_count(qts, 0);

        for (unsigned i = 2; i < 34; i++) {
            unsigned offset = 0xa000 + (i & 1) * 4096;
            unsigned port = (i - 2) % 16;
            uint64_t status;

            qtest_memwrite(qts, FAULT_MEMORY + offset, guard, sizeof(guard));
            if (i >= 4) {
                rx_host_offer(sv[0], i);
            }
            /* This flush retries the held offer without a readiness query. */
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
            qtest_clock_step(qts, 0);
            status = qtest_readq(qts, ETH_BASE + INT_STATUS);
            g_assert_cmpuint((status >> 25) & 31, ==, i & 31);
            g_assert_cmphex(status & 0xc0, ==, 0);
            g_assert_cmphex(status & 32, ==, 32);
            g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                             ==, 0);
            qtest_memread(qts, FAULT_MEMORY + offset, before, sizeof(before));
            g_assert_cmpmem(before, sizeof(before), guard, sizeof(guard));
            if (i < 18) {
                qtest_writel(qts, ETH_BASE + RX_FIFO_PORT + 8 * port + 4,
                             RAM_FULL + offset);
            } else {
                qtest_writeq(qts, ETH_REPEAT + RX_FIFO_PORT + 8 * port,
                             RAM_FULL + offset);
            }
            rx_host_packet(qts, offset, i, i);
            wait_fifo_count(qts, 0);
        }
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_host_gather_admission(void)
{
    for (unsigned delayed = 0; delayed < 2; delayed++) {
        int sv[2];
        QTestState *qts = fault_eth_start_socket(sv, false, true, SOCK_DGRAM);
        uint8_t guard[128];
        unsigned consumed = delayed ? 13 : 4;
        uint64_t control = rx_retirement_control(4, true);

        memset(guard, 0x59, sizeof(guard));
        rx_retirement_config(qts, 4, delayed ? 2 : 0, true);
        for (unsigned i = 0; i <= consumed; i++) {
            rx_host_offer(sv[0], i);
        }
        for (unsigned i = 0; i < consumed; i++) {
            unsigned offset = delayed && i < 10 ? 0x8000 + i * 256 :
                              0x8000 + (delayed ? i - 9 : i) * 4096;

            rx_host_packet(qts, offset, i, i);
        }
        wait_fifo_count(qts, 0);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
        qtest_clock_step(qts, 0);
        g_assert_cmpuint((qtest_readq(qts, ETH_BASE + INT_STATUS) >> 25) & 31,
                         ==, consumed);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0xc0, ==, 0);
        qtest_memwrite(qts, FAULT_MEMORY + 0xc000, guard, sizeof(guard));
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0xc000);
        rx_host_packet(qts, 0xc000, consumed, consumed);
        wait_fifo_count(qts, 0);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_host_nonpacket_drops(void)
{
    for (unsigned kind = 0; kind < 3; kind++) {
        int sv[2];
        QTestState *qts = fault_eth_start_socket(sv, false, true, SOCK_DGRAM);
        uint8_t frame[1515], guard[128];
        unsigned length = kind == 0 ? 60 : kind == 1 ? 14 : sizeof(frame);

        memset(guard, 0x59, sizeof(guard));
        memset(frame, 0, sizeof(frame));
        rx_sequence_frame(frame, 0);
        if (kind == 0) {
            frame[0] = 4;
        }
        rx_retirement_config(qts, 0, 0, false);
        qtest_writeq(qts, ETH_BASE + STATION, UINT64_C(0x02a14c7395e6));
        qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 0);
        g_assert_cmpint(send(sv[0], frame, length, 0), ==, length);
        sgi_wait_socket_read(sv[0]);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                     rx_retirement_control(0, false));
        qtest_clock_step(qts, 0);
        g_assert_cmpuint((qtest_readq(qts, ETH_BASE + INT_STATUS) >> 25) & 31,
                         ==, 0);
        g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0xc0, ==, 0);
        qtest_memwrite(qts, FAULT_MEMORY + 0x8000, guard, sizeof(guard));
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
        rx_host_offer(sv[0], 0);
        rx_host_packet(qts, 0x8000, 0, 0);
        wait_fifo_count(qts, 0);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_host_control_transitions(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        int sv[2];
        QTestState *qts = fault_eth_start_socket(sv, false, true, SOCK_DGRAM);
        uint8_t guard[128], landed[sizeof(guard)];
        uint64_t control = rx_retirement_control(1, false);
        unsigned sequence = mode == 1 ? 0 : 1;

        memset(guard, 0x59, sizeof(guard));
        rx_retirement_config(qts, 1, 0, false);
        rx_host_offer(sv[0], 0);
        rx_host_packet(qts, 0x8000, 0, 0);
        wait_fifo_count(qts, 0);
        qtest_memwrite(qts, FAULT_MEMORY + 0x9000, guard, sizeof(guard));
        rx_host_offer(sv[0], 1);
        sgi_wait_socket_read(sv[0]);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
        qtest_clock_step(qts, 0);

        if (mode == 0) {
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
        } else if (mode == 1) {
            qtest_system_reset(qts);
            mdio_write(qts, 8, 0, 0x2100);
            qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
        } else if (mode == 2) {
            qtest_qmp_assert_success(qts,
                "{'execute':'set_link','arguments':{'name':'n0','up':false}}");
            qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
        } else {
            /* The normal NIC destruction must complete the pending sender. */
            qtest_qmp_assert_success(qts,
                "{'execute':'qom-set','arguments':"
                "{'path':'/machine/peripheral/mace/eth',"
                "'property':'realized','value':false}}");
            qtest_quit(qts);
            close(sv[0]);
            close(sv[1]);
            continue;
        }
        qtest_clock_step(qts, 0);
        qtest_memread(qts, FAULT_MEMORY + 0x9000, landed, sizeof(landed));
        g_assert_cmpmem(landed, sizeof(landed), guard, sizeof(guard));
        if (mode == 2) {
            qtest_qmp_assert_success(qts,
                "{'execute':'set_link','arguments':{'name':'n0','up':true}}");
        }
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x9000);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
        rx_host_offer(sv[0], 2);
        rx_host_packet(qts, 0x9000, 2, sequence);
        wait_fifo_count(qts, 0);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_host_dma_fault(void)
{
    int sv[2];
    QTestState *qts = fault_eth_start_socket(sv, false, true, SOCK_DGRAM);
    uint8_t guard[128], partial[sizeof(guard)], after[sizeof(guard)];
    uint64_t control = rx_retirement_control(1, false);
    SgiWait wait;

    memset(guard, 0x59, sizeof(guard));
    rx_retirement_config(qts, 0, 0, false);
    qtest_memwrite(qts, FAULT_MEMORY + 0x9000, guard, sizeof(guard));
    rx_host_offer(sv[0], 0);
    sgi_wait_socket_read(sv[0]);
    fault_set(qts, "fault-start", 0x9022);
    fault_set(qts, "fault-end", 0x905d);
    fault_set(qts, "fault-mode", 2);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x9000);
    sgi_wait_start(&wait, "MACE deferred offer reaches RX DMA fault");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 1000);
        qtest_clock_step(qts, 0);
    }
    fault_set(qts, "fault-mode", 0);
    qtest_memread(qts, FAULT_MEMORY + 0x9000, partial, sizeof(partial));
    g_assert_false(eth_status(qts, FAULT_MEMORY + 0x9000) >> 63);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
    rx_host_offer(sv[0], 1);
    sgi_wait_socket_read(sv[0]);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
    qtest_clock_step(qts, 0);
    qtest_memread(qts, FAULT_MEMORY + 0x9000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), partial, sizeof(partial));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 1);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    qtest_memwrite(qts, FAULT_MEMORY + 0xa000, guard, sizeof(guard));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0xa000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, control);
    rx_host_offer(sv[0], 2);
    rx_host_packet(qts, 0xa000, 2, 0);
    wait_fifo_count(qts, 0);
    qtest_memread(qts, FAULT_MEMORY + 0x9000, after, sizeof(after));
    g_assert_cmpmem(after, sizeof(after), partial, sizeof(partial));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_retirement(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        int sv[2];
        QTestState *qts = fault_eth_start_mode(sv, false, legacy);
        uint8_t before[3 * 4096], after[sizeof(before)];

        qtest_irq_intercept_out_named(qts, "/machine/peripheral/mace", "crime");
        rx_retirement_config(qts, 8, 0, false);
        g_assert_false(rx_threshold_event(qts));
        for (unsigned i = 0; i < 3; i++) {
            rx_retirement_packet(qts, sv[0], 0x8000 + 4096 * i, i);
        }
        g_assert_true(rx_threshold_event(qts));
        g_assert_true(qtest_get_irq(qts, 3));
        qtest_memread(qts, FAULT_MEMORY + 0x8000, before, sizeof(before));
        for (unsigned i = 0; i < 3; i++) {
            qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT,
                         RAM_FULL + 0x8000 + 4096 * i);
            g_assert_cmpint(rx_threshold_event(qts), ==, i < 2 || !legacy);
        }
        g_assert_cmpint(qtest_get_irq(qts, 3), ==, !legacy);
        qtest_memread(qts, FAULT_MEMORY + 0x8000, after, sizeof(after));
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        rx_retirement_packet(qts, sv[0], 0xb000, 3);
        g_assert_true(rx_threshold_event(qts));
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 0);
        g_assert_true(rx_threshold_event(qts));
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 8);
        g_assert_true(rx_threshold_event(qts));
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
        g_assert_cmpint(rx_threshold_event(qts), ==, !legacy);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_threshold_legacy_machine(void)
{
    uint8_t frame[60], landed[60];
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M o2-test,bank-profile=8x128m-synthetic,"
                      "crime-profile=legacy-software,phy-ics1890=on,"
                      "phy-address=8 -m 1G "
                      "-nic socket,id=n0,fd=%d,model=sgi-mace-eth", sv[1]);
    qtest_irq_intercept_out_named(qts, "/machine/crime", "cpu-ip");
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(qts, 0x14000208 + 8 * bank, 0x100 | (bank << 2));
    }
    mdio_write(qts, 8, 0, 0x2100);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, 3U << 5);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(1, false));
    g_assert_false(rx_threshold_event(qts));
    rx_sequence_frame(frame, 0);
    socket_send_frame(sv[0], frame, sizeof(frame));
    wait_rx_status(qts, RAM_FULL + 0x8000);
    qtest_memread(qts, RAM_FULL + 0x8022, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    g_assert_true(rx_threshold_event(qts));
    g_assert_true(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writeq(qts, CRIME_ENABLE, 8);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeq(qts, CRIME_ENABLE, 0);
    g_assert_true(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    g_assert_false(rx_threshold_event(qts));
    g_assert_false(qtest_readq(qts, CRIME_STATUS) & 8);
    qtest_writeq(qts, CRIME_ENABLE, 8);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_aliases_and_masks(void)
{
    int sv[2];
    QTestState *qts = rx_retirement_start(sv, 8, 0, false);

    rx_retirement_packet(qts, sv[0], 0x8000, 0);
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, 8U << 4);
    g_assert_true(rx_threshold_event(qts));
    g_assert_false(qtest_get_irq(qts, 3));
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, 7U << 4);
    g_assert_false(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, (8U << 4) | (1U << 9));
    g_assert_false(rx_threshold_event(qts));
    g_assert_false(qtest_get_irq(qts, 3));
    rx_retirement_packet(qts, sv[0], 0x9000, 1);
    g_assert_true(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 rx_retirement_control(8, false) & ~(1U << 15));
    g_assert_true(rx_threshold_event(qts));
    g_assert_false(qtest_get_irq(qts, 3));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 rx_retirement_control(6, false) & ~(1U << 15));
    g_assert_false(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(8, false));
    g_assert_false(rx_threshold_event(qts));
    rx_retirement_packet(qts, sv[0], 0xa000, 2);
    g_assert_true(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
    g_assert_false(rx_threshold_event(qts));
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                     (8U << 4) | (enabled ? 1U << 9 : 0));
        g_assert_false(rx_threshold_event(qts));
    }
    rx_retirement_packet(qts, sv[0], 0xb000, 3);
    g_assert_true(rx_threshold_event(qts));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(4, false));
    g_assert_false(rx_threshold_event(qts));
    qtest_system_reset(qts);
    g_assert_false(rx_threshold_event(qts));
    mdio_write(qts, 8, 0, 0x2100);
    rx_retirement_config(qts, 1, 0, false);
    g_assert_false(rx_threshold_event(qts));
    rx_retirement_packet(qts, sv[0], 0x8000, 0);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    g_assert_false(rx_threshold_event(qts));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_field_values(void)
{
    static const unsigned counts[] = { 0, 3, 4, 16 };
    int sv[2];
    QTestState *qts = rx_retirement_start(sv, 0, 0, false);

    for (unsigned c = 0; c < G_N_ELEMENTS(counts); c++) {
        for (unsigned threshold = 0; threshold < 32; threshold++) {
            unsigned count = counts[c];
            unsigned posted = count == 16 ? 16 : count + 1;

            qtest_system_reset(qts);
            mdio_write(qts, 8, 0, 0x2100);
            rx_retirement_config(qts, posted, 0, false);
            qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                         (31U << 4) | (1U << 9));
            g_assert_false(rx_threshold_event(qts));
            rx_retirement_packet(qts, sv[0], 0x8000, 0);
            if (count == 16) {
                qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
            }
            g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                             ==, count);
            g_assert_true(rx_threshold_event(qts));
            if (threshold & 1) {
                qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                             (threshold << 4) | (1U << 9));
            } else {
                qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                             rx_retirement_control(threshold, false));
            }
            g_assert_cmpint(rx_threshold_event(qts), ==, threshold != count);
            qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                         (count << 4) | (1U << 9));
            g_assert_false(rx_threshold_event(qts));
            qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                         (((count + 1) & 31) << 4) | (1U << 9));
            g_assert_false(rx_threshold_event(qts));
        }
    }
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_pending_refill(void)
{
    static const unsigned early_counts[] = { 1, 4, 5 };

    for (unsigned unequal = 0; unequal < 2; unequal++) {
        int sv[2];
        QTestState *qts = rx_retirement_start(sv, 8, 2, false);

        rx_retirement_packet(qts, sv[0], 0x8000, 0);
        g_assert_false(rx_threshold_event(qts));
        qtest_clock_step(qts, 30690);
        qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
        if (unequal) {
            qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                         (7U << 4) | (1U << 9));
        }
        qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
        qtest_writeq(qts, ETH_BASE + RX_DELAY, 63);
        qtest_clock_step(qts, 30689);
        g_assert_false(rx_threshold_event(qts));
        qtest_clock_step(qts, 1);
        g_assert_cmpint(rx_threshold_event(qts), ==, unequal);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 8);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
    for (unsigned c = 0; c < G_N_ELEMENTS(early_counts); c++) {
        unsigned count = early_counts[c];
        int sv[2];
        QTestState *qts = rx_retirement_start(sv, count, 1, false);

        rx_retirement_packet(qts, sv[0], 0x8000, 0);
        g_assert_cmpint(rx_threshold_event(qts), ==, count < 5);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                         ==, count - 1);
        if (count == 5) {
            qtest_clock_step(qts, 30689);
            g_assert_false(rx_threshold_event(qts));
            qtest_clock_step(qts, 1);
            g_assert_true(rx_threshold_event(qts));
        }
        qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                     ((count - 1) << 4) | (1U << 9));
        qtest_clock_step(qts, 30690);
        g_assert_false(rx_threshold_event(qts));
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_threshold_gather_retirement(void)
{
    for (unsigned unequal = 0; unequal < 2; unequal++) {
        int sv[2];
        QTestState *qts = rx_retirement_start(sv, 8, 0, false);
        uint8_t before[4096], after[sizeof(before)];

        rx_retirement_packet(qts, sv[0], 0x8000, 0);
        g_assert_true(rx_threshold_event(qts));
        qtest_writeq(qts, ETH_BASE + RX_DELAY, 2);
        qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                     rx_retirement_control(8, true));
        rx_retirement_packet(qts, sv[0], 0x9000, 1);
        g_assert_true(rx_threshold_event(qts));
        qtest_memread(qts, FAULT_MEMORY + 0x9000, before, sizeof(before));
        qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, (7U << 4) | (1U << 9));
        g_assert_false(rx_threshold_event(qts));
        if (unequal) {
            qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS,
                         (8U << 4) | (1U << 9));
        } else {
            qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
        }
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                         ==, unequal ? 7 : 8);
        qtest_clock_step(qts, 61379);
        g_assert_false(rx_threshold_event(qts));
        qtest_clock_step(qts, 1);
        g_assert_cmpint(rx_threshold_event(qts), ==, unequal);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                         ==, unequal ? 6 : 7);
        qtest_clock_step(qts, 61380);
        g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31,
                         ==, unequal ? 6 : 7);
        qtest_memread(qts, FAULT_MEMORY + 0x9000, after, sizeof(after));
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmphex(eth_status(qts, FAULT_MEMORY + 0xa000) >> 63, ==, 0);
        qtest_quit(qts);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_rx_threshold_other_causes(void)
{
    int sv[2];
    QTestState *qts = rx_retirement_start(sv, 1, 0, false);

    rx_retirement_packet(qts, sv[0], 0x8000, 0);
    qtest_writeq(qts, ETH_BASE + MAC_CONTROL, (3U << 5) | (1U << 7));
    qtest_qmp_assert_success(qts,
        "{'execute':'set_link','arguments':{'name':'n0','up':false}}");
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x24, ==, 0x24);
    qtest_writeq(qts, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
    g_assert_cmphex(qtest_readq(qts, ETH_BASE + INT_STATUS) & 0x24, ==, 4);
    g_assert_true(qtest_get_irq(qts, 3));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 32);
    g_assert_true(qtest_get_irq(qts, 3));
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 4);
    g_assert_false(qtest_get_irq(qts, 3));
    qtest_qmp_assert_success(qts,
        "{'execute':'set_link','arguments':{'name':'n0','up':true}}");
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(2, false));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 rx_retirement_control(1, false) | 3);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 1);
    g_assert_false(rx_threshold_event(qts));
    g_assert_true(qtest_get_irq(qts, 3));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_fault_retirement(void)
{
    int sv[2];
    QTestState *qts = rx_retirement_start(sv, 8, 0, false);
    uint8_t before[4096], after[sizeof(before)], frame[60] = { 0 };
    SgiWait wait;

    rx_retirement_packet(qts, sv[0], 0x8000, 0);
    qtest_writeq(qts, ETH_BASE + RX_DELAY, 2);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(8, true));
    rx_retirement_packet(qts, sv[0], 0x9000, 1);
    qtest_memread(qts, FAULT_MEMORY + 0x9000, before, sizeof(before));
    qtest_writeq(qts, ETH_BASE + TX_BASE, 0xf0000000);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL,
                 rx_retirement_control(8, true) | 3);
    qtest_writeq(qts, ETH_BASE + TX_POINTER, 1);
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    fault_set(qts, "fault-start", 0x9100);
    fault_set(qts, "fault-end", 0x9107);
    fault_set(qts, "fault-mode", 2);
    socket_send_frame(sv[0], frame, sizeof(frame));
    sgi_wait_start(&wait, "legacy RX threshold status fault");
    while (qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15)) {
        sgi_wait_pause(&wait, 1000);
        qtest_clock_step(qts, 0);
    }
    fault_set(qts, "fault-mode", 0);
    g_assert_cmphex(eth_status(qts, FAULT_MEMORY + 0x9100) >> 63, ==, 0);
    qtest_writeq(qts, ETH_BASE + RX_INT_ALIAS, (7U << 4) | (1U << 9));
    g_assert_false(rx_threshold_event(qts));
    g_assert_true(qtest_readq(qts, ETH_BASE + INT_STATUS) & 8);
    g_assert_true(qtest_get_irq(qts, 3));
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(8, true));
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
    qtest_clock_step(qts, 613800);
    g_assert_false(rx_threshold_event(qts));
    qtest_memread(qts, FAULT_MEMORY + 0x9000, after, sizeof(after));
    /* The failed packet may have payload writes in the next bucket. */
    g_assert_cmpmem(before, 256, after, 256);
    g_assert_cmpuint(qtest_readq(qts, ETH_BASE + RX_FIFO_INFO) & 31, ==, 7);
    qtest_writeq(qts, ETH_BASE + INT_STATUS, 8);
    qtest_writeq(qts, ETH_BASE + DMA_CONTROL, rx_retirement_control(8, true));
    g_assert_false(qtest_readq(qts, ETH_BASE + DMA_CONTROL) & (1U << 15));
    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

static void test_rx_threshold_migration(void)
{
    for (unsigned mode = 0; mode < 4; mode++) {
        g_autofree char *dir = g_dir_make_tmp("mace-rx-retire-XXXXXX", NULL);
        g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
        g_autofree char *uri = g_strdup_printf("unix:%s", socket);
        int sv[2], dest_sv[2];
        QTestState *from = rx_retirement_start(sv, 8, 2, mode == 3);
        QTestState *to = fault_eth_start_mode(dest_sv, true, true);
        uint8_t before[4096], after[sizeof(before)];
        uint64_t fifo, events;
        int64_t clock;

        qtest_irq_intercept_out_named(to, "/machine/peripheral/mace", "crime");
        rx_retirement_packet(from, sv[0], 0x8000, 0);
        if (mode < 2) {
            qtest_clock_step(from, 61380);
            g_assert_true(rx_threshold_event(from));
            if (mode == 1) {
                qtest_writeq(from, ETH_BASE + RX_FIFO_PORT, RAM_FULL + 0x8000);
                g_assert_false(rx_threshold_event(from));
            }
            clock = qtest_clock_step(from, 0);
        } else {
            if (mode == 3) {
                qtest_writeq(from, ETH_BASE + RX_INT_ALIAS, 8U << 4);
            }
            clock = qtest_clock_step(from, 30690);
        }
        fifo = qtest_readq(from, ETH_BASE + RX_FIFO_INFO);
        events = qtest_readq(from, ETH_BASE + INT_STATUS);
        qtest_memread(from, FAULT_MEMORY + 0x8000, before, sizeof(before));
        qtest_qmp_assert_success(from, "{'execute':'stop'}");
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        qtest_clock_set(to, clock);
        qtest_qmp_assert_success(to, "{'execute':'cont'}");
        g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==, fifo);
        g_assert_cmphex(qtest_readq(to, ETH_BASE + INT_STATUS), ==, events);
        g_assert_cmpint(qtest_get_irq(to, 3), ==, mode == 0);
        if (mode >= 2) {
            qtest_clock_step(to, 30689);
            g_assert_false(rx_threshold_event(to));
            qtest_clock_step(to, 1);
            g_assert_true(rx_threshold_event(to));
            g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31,
                             ==, 7);
            g_assert_cmpint(qtest_get_irq(to, 3), ==, mode == 2);
        }
        qtest_clock_step(to, 61380);
        qtest_memread(to, FAULT_MEMORY + 0x8000, after, sizeof(after));
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        g_assert_cmpuint(qtest_readq(to, ETH_BASE + RX_FIFO_INFO) & 31,
                         ==, mode == 1 ? 8 : 7);
        qtest_quit(to);
        qtest_quit(from);
        close(sv[0]);
        close(sv[1]);
        close(dest_sv[0]);
        close(dest_sv[1]);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_rx_threshold_migration_mismatch(void)
{
    g_autofree char *dir = g_dir_make_tmp("mace-rx-policy-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    int sv[2], dest_sv[2];
    QTestState *from = rx_retirement_start(sv, 8, 2, true);
    QTestState *to = fault_eth_start_mode(dest_sv, true, false);
    SgiWait wait;

    rx_retirement_packet(from, sv[0], 0x8000, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_start(&wait, "MACE RX threshold destination policy rejection");
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");

        if (!strcmp(qdict_get_str(reply, "status"), "failed")) {
            break;
        }
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_false(rx_threshold_event(to));
    g_assert_cmphex(qtest_readq(to, ETH_BASE + RX_FIFO_INFO), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    close(sv[0]);
    close(sv[1]);
    close(dest_sv[0]);
    close(dest_sv[1]);
    g_unlink(socket);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace/ethernet/rx-host-admission",
                   test_rx_host_admission);
    qtest_add_func("/sgi/mace/ethernet/rx-host-gather-admission",
                   test_rx_host_gather_admission);
    qtest_add_func("/sgi/mace/ethernet/rx-host-nonpacket-drops",
                   test_rx_host_nonpacket_drops);
    qtest_add_func("/sgi/mace/ethernet/rx-host-control-transitions",
                   test_rx_host_control_transitions);
    qtest_add_func("/sgi/mace/ethernet/rx-host-dma-fault",
                   test_rx_host_dma_fault);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-retirement",
                   test_rx_threshold_retirement);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-legacy-machine",
                   test_rx_threshold_legacy_machine);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-aliases-masks",
                   test_rx_threshold_aliases_and_masks);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-field-values",
                   test_rx_threshold_field_values);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-pending-refill",
                   test_rx_threshold_pending_refill);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-gather-retirement",
                   test_rx_threshold_gather_retirement);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-other-causes",
                   test_rx_threshold_other_causes);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-fault-retirement",
                   test_rx_threshold_fault_retirement);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-migration",
                   test_rx_threshold_migration);
    qtest_add_func("/sgi/mace/ethernet/rx-threshold-migration-mismatch",
                   test_rx_threshold_migration_mismatch);
    qtest_add_func("/sgi/mace/ethernet/rx-fault-migration",
                   test_rx_fault_migration);
    qtest_add_func("/sgi/mace/ethernet/rx-gather-offset-change",
                   test_rx_gather_offset_change);
    qtest_add_func("/sgi/mace/ethernet/rx-gather-limits",
                   test_rx_gather_limits);
    qtest_add_func("/sgi/mace/ethernet/rx-gather-capacity",
                   test_rx_gather_capacity);
    qtest_add_func("/sgi/mace/ethernet/all-ring-reinitialization",
                   test_all_ring_reinitialization);
    qtest_add_func("/sgi/mace/ethernet/tx-pointer-combinations",
                   test_tx_pointer_combinations);
    qtest_add_func("/sgi/mace/ethernet/rx-interval-migration",
                   test_rx_interval_migration);
    qtest_add_func("/sgi/mace/ethernet/tx-status-fault",
                   test_tx_status_fault);
    qtest_add_func("/sgi/mace/ethernet/rx-dma-faults",
                   test_rx_dma_faults);
    qtest_add_func("/sgi/mace/ethernet/dma-fault-memory",
                   test_dma_fault_memory);
    qtest_add_func("/sgi/mace/ethernet/tx-all-ring-sizes",
                   test_tx_all_ring_sizes);
    qtest_add_func("/sgi/mace/ethernet/tx-reserved-descriptor",
                   test_tx_reserved_descriptor);
    qtest_add_func("/sgi/mace/ethernet/rx-gather-disable",
                   test_rx_gather_disable);
    qtest_add_func("/sgi/mace/ethernet/rx-gather-interval",
                   test_rx_gather_interval);
    qtest_add_func("/sgi/mace/ethernet/rx-interval-early",
                   test_rx_interval_early);
    qtest_add_func("/sgi/mace/ethernet/rx-interval-refill",
                   test_rx_interval_refill);
    qtest_add_func("/sgi/mace/ethernet/rx-interval",
                   test_rx_interval);
    qtest_add_func("/sgi/mace/ethernet/rx-checksum-vectors",
                   test_rx_checksum_vectors);
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
    qtest_add_func("/sgi/mace/ethernet/tx-inline-raw",
                   test_tx_inline_raw);
    qtest_add_func("/sgi/mace/ethernet/tx-concat-cross-page",
                   test_tx_concat_cross_page);
    qtest_add_func("/sgi/mace/ethernet/tx-two-concat-hole",
                   test_tx_two_concat_and_hole);
    qtest_add_func("/sgi/mace/ethernet/rx-filter-raw-dma",
                   test_rx_filter_and_raw_dma);
    qtest_add_func("/sgi/mace/ethernet/rx-progress", test_rx_progress);
    qtest_add_func("/sgi/mace/ethernet/rx-progress-reset",
                   test_rx_progress_reset);
    qtest_add_func("/sgi/mace/ethernet/rx-progress-migration",
                   test_rx_progress_migration);
    qtest_add_func("/sgi/mace/ethernet/rx-fifo-dma-wrap",
                   test_rx_fifo_dma_wrap);
    qtest_add_func("/sgi/mace/ethernet/rx-sustained-replenish",
                   test_rx_sustained_fifo_replenish);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-stop-then-reset",
                   test_rx_reinit_stop_then_reset);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-reset-then-stop",
                   test_rx_reinit_reset_then_stop);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-after-rx-progress",
                   test_rx_reinit_after_rx_progress);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-already-disabled",
                   test_rx_reinit_already_disabled);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-core-only",
                   test_rx_reinit_core_only);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-reenable-discards-pulse",
                   test_rx_reinit_reenable_discards_pulse);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-dma-only",
                   test_rx_reinit_dma_only);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-other-ring-size",
                   test_rx_reinit_other_ring_size);
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-active-16k-then-zero",
                   test_rx_reinit_active_16k_then_zero);
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
    qtest_add_func("/sgi/mace/ethernet/rx-reinit-pending-migrates",
                   test_rx_reinit_pending_migrates);
    qtest_add_func("/sgi/mace/ethernet/control-aliases",
                   test_control_aliases);
    qtest_add_func("/sgi/mace/ethernet/tx-ring-wrap",
                   test_tx_ring_wrap);
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
