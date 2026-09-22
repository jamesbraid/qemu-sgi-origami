/*
 * The Origin 300 IO-8 board's IOC3 Ethernet transceiver, at the MII
 * address the BASEIO PROM's diagnostic hard-codes: 1, not the Origin
 * 200's 31. hw/net/dp83840.c keeps 31 as its default; hw/mips/sgi/sn1.c
 * threads this board's address through sn1-model.h's per-device
 * descriptor into the eth child's "phy-addr" property, a "sgi-ioc3" ->
 * "sgi-ioc3-eth" -> "dp83840" alias chain whose last hop already backs
 * Origin 200's own default (tests/qtest/sn0-ioc3-test.c).
 *
 * A real Origin 300's OpenBSD capture confirms the address: "nsphyter0
 * at iec0 phy 1: DP83843 10/100 PHY" (docs/datasheets/dp83840.md). The
 * board's transceiver is also a different part, a DP83843 rather than
 * the Origin 200's DP83840A, selected the same way through a
 * "phy-dp83843" alias chain; this file pins its identity register and
 * that register 0x17 (its Loopback Register there) reads back what is
 * written. The behaviour that register selects -- turning a frame back
 * without reaching the backend -- is tests/qtest/sgi-ioc3-eth-test.c's,
 * reached through a plain sgi-ioc3 device rather than this board's full
 * crosstalk wiring. The rest of the transceiver's own register contract
 * -- reset, autonegotiation -- belongs to sn0-ioc3-test.c and is not
 * repeated here; this file pins only where and as what the IO-8 board's
 * copy answers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define XTALK_WIDGET(w) (SPACE_XTALK + (uint64_t)(w) * UINT64_C(0x1000000))

/* Head F carries the IO-8 board, and the IOC3 is device 4 on it. */
#define IO8_WIDGET 0xf
#define IO8_IOC3_SLOT 4
#define IO8_IOC3_CFG (XTALK_WIDGET(IO8_WIDGET) + 0x20000 + \
                      IO8_IOC3_SLOT * 0x1000)

/*
 * The XBridge datasheet's device-window table gives slots 2-7 one
 * megabyte each from 0x600000, which puts slot 4 at 0x800000 (the same
 * table tests/qtest/sn1-io8-scsi-test.c cites for slot 1's window).
 */
#define IO8_IOC3_WINDOW (XTALK_WIDGET(IO8_WIDGET) + 0x800000)

/*
 * The window's target comes from the head's device register in units of
 * a megabyte. Nothing else on this bus is enabled while the test runs,
 * so the address is free.
 */
#define ADAPTER_MMIO 0x10000000

#define XBRIDGE_DEVICE(n) (0x200 + (n) * 8)
#define XBRIDGE_DEVICE_MEMORY (1U << 12)

/* The ENET block's MII management registers (include/hw/net/sgi-ioc3-eth.h). */
#define MII_CONTROL (IO8_IOC3_WINDOW + 0x148)
#define MII_DATA_R  (IO8_IOC3_WINDOW + 0x14c)
#define MII_DATA_W  (IO8_IOC3_WINDOW + 0x150)
#define MII_READ    (1u << 10)

/* What the BASEIO PROM's MII helpers hard-code, and Origin 200's default. */
#define PHY_ADDRESS       1
#define ORIGIN200_ADDRESS 31

static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-io8-ioc3-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint16_t phy_read(QTestState *qts, unsigned int phy, unsigned int reg)
{
    qtest_writel(qts, MII_CONTROL, MII_READ | (phy << 5) | reg);
    return qtest_readl(qts, MII_DATA_R) & 0xffff;
}

static void phy_write(QTestState *qts, unsigned int phy, unsigned int reg,
                      uint16_t val)
{
    qtest_writel(qts, MII_DATA_W, val);
    qtest_writel(qts, MII_CONTROL, (phy << 5) | reg);
}

/*
 * IOC3 declares itself self-ordered to the XBridge head (SGI_BRIDGE_
 * BYTE_LANE_SELF_ORDERED in hw/misc/sgi-ioc3.c), so the device register's
 * swap bit is never consulted for it; only the memory-space and window
 * bits matter here.
 */
/*
 * The head's own registers are a big-endian 64-bit file; a 32-bit access
 * reaches the low word at a plus-four offset (see
 * tests/qtest/sn1-io8-scsi-test.c's xtalk_reg_write). PCI config-space
 * cycles decode separately and take no such correction.
 */
static void xtalk_reg_write(QTestState *qts, unsigned int widget,
                            uint64_t offset, uint32_t value)
{
    qtest_writel(qts, XTALK_WIDGET(widget) + offset + 4, value);
}

static QTestState *machine_start(void)
{
    g_autofree char *prom = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(MACHINE_ARGS " -bios %s", prom);

    qtest_writel(qts, IO8_IOC3_CFG + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, IO8_IOC3_CFG + PCI_COMMAND, PCI_COMMAND_MEMORY);
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_DEVICE(IO8_IOC3_SLOT),
                    XBRIDGE_DEVICE_MEMORY | (ADAPTER_MMIO >> 20));
    return qts;
}

static void test_transceiver_identity(const void *data)
{
    QTestState *qts = machine_start();

    /*
     * PHYIDR1 is the OUI's upper bits, the same on every National
     * Semiconductor PHY this project models, so it stays 0x2000 on both
     * parts. PHYIDR2 is this board's own: 0x5c10 is the DP83843's, not the
     * Origin 200 DP83840A's 0x5c01 (docs/datasheets/dp83840.md). The
     * transceiver's own register contract otherwise belongs to
     * sn0-ioc3-test.c.
     */
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 2), ==, 0x2000);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 3), ==, 0x5c10);

    qtest_quit(qts);
}

/*
 * Read-after-write only: the loopback behaviour this register selects on
 * the DP83843 belongs to tests/qtest/sgi-ioc3-eth-test.c, which reaches the
 * register through a plain sgi-ioc3 device rather than this board's full
 * crosstalk wiring.
 */
static void test_register_0x17_reads_back_a_written_value(const void *data)
{
    QTestState *qts = machine_start();

    phy_write(qts, PHY_ADDRESS, 0x17, 0x20);
    g_assert_cmphex(phy_read(qts, PHY_ADDRESS, 0x17), ==, 0x20);

    qtest_quit(qts);
}

static void test_origin200_address_is_silent(const void *data)
{
    QTestState *qts = machine_start();

    /* The Origin 200's address answers nothing on this board. */
    g_assert_cmphex(phy_read(qts, ORIGIN200_ADDRESS, 2), ==, 0xffff);

    qtest_quit(qts);
}

static void test_transceiver_answers_at_one_address(const void *data)
{
    QTestState *qts = machine_start();
    unsigned int phy, answered = 0;

    /*
     * Every address: both IRIX and the BASEIO PROM stop at the first
     * that answers, so a model answering everywhere would be found at
     * address zero and no boot notices. The count is the assertion.
     */
    for (phy = 0; phy < 32; phy++) {
        if (phy_read(qts, phy, 2) != 0xffff) {
            g_assert_cmpuint(phy, ==, PHY_ADDRESS);
            answered++;
        }
    }
    g_assert_cmpuint(answered, ==, 1);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();

    qtest_add_data_func("origin300/io8-ioc3/transceiver-identity", NULL,
                        test_transceiver_identity);
    qtest_add_data_func("origin300/io8-ioc3/origin200-address-is-silent",
                        NULL, test_origin200_address_is_silent);
    qtest_add_data_func(
        "origin300/io8-ioc3/transceiver-answers-at-one-address", NULL,
        test_transceiver_answers_at_one_address);
    qtest_add_data_func("origin300/io8-ioc3/register-0x17-reads-back", NULL,
                        test_register_0x17_reads_back_a_written_value);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_free(prom_path);
    return result;
}
