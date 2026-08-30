/*
 * Alteon Tigon 2 (the SGI "eg" gigabit board) behind the Origin 200 Bridge.
 *
 * The Tigon is an add-in PCI card, so unlike the base I/O board's soldered
 * adapters it is instantiated with -device at a free slot and reached through
 * that slot's own Bridge decode -- the same way tests/qtest/sn0-bridge-test.c
 * hosts its probe card. Everything here runs on the machine's real
 * crosstalk-to-PCI topology so the register contract, the firmware-mediated
 * ring engine, and the descriptor byte order are all proven on the fabric the
 * card actually sits on rather than a generic PCI bus that has no Bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/* A fixed station address so PAR0/PAR1 and the EEPROM read are deterministic. */
#define TIGON_MAC       "52:54:00:12:34:56"
static const uint8_t tigon_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

#define MACHINE_WITH_BRIDGE  \
    "-M origin200,io-bridge=on -device tigon,addr=3,mac=" TIGON_MAC

/*
 * The card's slot. Devices 0 and 1 are the base I/O board's SCSI adapters and
 * device 2 is IOC3; 3 is the first free slot, and its widget window is the
 * first of the six 1 MB windows (sn0-bridge-test.c's own window table).
 */
#define TIGON_SLOT      3
#define WINDOW_TIGON    (BRIDGE_BASE + 0x700000)

/* PCI type 0 configuration space through the Bridge. */
#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define PCI_VENDOR_DEVICE   0x00
#define PCI_COMMAND         0x04
#define PCI_REV_CLASS       0x08
#define PCI_BAR0            0x10
#define PCI_INTR_LINE       0x3c

#define PCI_COMMAND_MEMORY  0x0002
#define PCI_COMMAND_MASTER  0x0004

/* The Bridge's own per-slot device register, low half of its 8-byte slot. */
#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE    (1u << 12)

/* Where the card's BAR is parked in PCI memory space (1 MB aligned). */
#define ADAPTER_MMIO        0x10000000u

/* The card's identity, from docs/datasheets/tigon.md. */
#define TIGON_VENDOR_ID     0x10a9
#define TIGON_DEVICE_ID     0x0009
#define TIGON_CLASS_CODE    0x020000
#define TIGON_CHIP_REV      0x6         /* Tigon 2 rev nibble */

/* Register offsets within the card's own window (if_tireg.h). */
#define TI_MISC_HOST_CTL    0x040
#define TI_MISC_LOCAL_CTL   0x044
#define TI_GCR_PAR0         0x600
#define TI_GCR_PAR1         0x604
#define TI_MHC_RESET        0x00000008
#define TI_MHC_CHIP_REV_MASK 0xf0000000

/* Serial-EEPROM bit-bang lines in MISC_LOCAL_CTL. */
#define TI_MLC_EE_CLK       0x00100000
#define TI_MLC_EE_TXEN      0x00200000
#define TI_MLC_EE_DOUT      0x00400000
#define TI_MLC_EE_DIN       0x00800000
#define TI_EE_MAC_OFFSET    0x8c

/* Interrupt and CPU control. */
#define TI_MHC_INTSTATE     0x00000001
#define TI_CPU_STATE        0x140

/* GCR registers exercised by the ring model. */
#define TI_GCR_GENINFO_HI   0x608
#define TI_GCR_GENINFO_LO   0x60c
#define TI_GCR_OPMODE       0x618
#define TI_OPMODE_BYTESWAP_BD   0x00000002
#define TI_OPMODE_WORDSWAP_BD   0x00000004
#define TI_OPMODE_BYTESWAP_DATA 0x00000010

/*
 * A general information block and its rings, laid out in low guest RAM. The
 * Bridge carries a DMA address below its translated window straight through to
 * system memory, so the card reaches these at the same address the test wrote
 * them, with no address-translation setup.
 */
#define GIB_PHYS            0x00200000u
#define GIB_EV_RCB          0x400
#define GIB_TX_RCB          0x420
#define GIB_STD_RX_RCB      0x430
#define GIB_RETURN_RCB      0x460
#define GIB_EV_PROD_PTR     0x470
#define GIB_RETURN_PROD_PTR 0x478
#define GIB_TX_CONS_PTR     0x480
#define GIB_SIZE            0x490

#define EVENT_RING_PHYS     0x00210000u
#define EV_PROD_PHYS        0x00220000u
#define TX_CONS_PHYS        0x00230000u
#define RETURN_RING_PHYS    0x00240000u
#define RETURN_PROD_PHYS    0x00250000u
#define STD_RX_RING_PHYS    0x00260000u
#define TX_RING_PHYS        0x00270000u
#define RX_BUF_PHYS         0x00280000u
#define TX_BUF_PHYS         0x00290000u

#define TI_RCB_FLAG_HOST_RING   0x0040
#define TI_EV_FIRMWARE_UP       0x01

/* Write a ring control block's 64-bit host address (two words, hi then lo). */
static void write_rcb_addr(QTestState *qts, uint32_t rcb, uint32_t phys)
{
    qtest_writel(qts, rcb + 0, 0);
    qtest_writel(qts, rcb + 4, phys);
}

/*
 * A 4-byte access needs no byte-lane correction -- sgi_bridge_window_correct()
 * moves only the low two bits and leaves a full-word access at its natural
 * offset -- so a 32-bit register is reached at WINDOW + offset directly.
 */
static void aim_window_at_bar(QTestState *qts)
{
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void test_config_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* Vendor and device, the SGI Tigon pair, in one 32-bit config read. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_VENDOR_DEVICE), ==,
                    TIGON_VENDOR_ID | (TIGON_DEVICE_ID << 16));

    /* Class code (bytes 0x09-0x0B): Ethernet network controller. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_REV_CLASS) >> 8, ==,
                    TIGON_CLASS_CODE);

    /* One interrupt pin, INTA. */
    g_assert_cmphex((qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_INTR_LINE) >> 8) &
                    0xff, ==, 0x01);

    qtest_quit(qts);
}

static void test_one_memory_bar(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* BAR0 is a 32-bit, non-prefetchable memory BAR: low nibble all clear. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0), ==,
                    0xffffc000u);

    /* There is no second BAR. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4), ==, 0);

    qtest_quit(qts);
}

static void test_misc_host_ctl_chip_rev(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* The chip-revision nibble is the driver's Tigon 1 vs 2 runtime test. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    /* The RESET bit is accepted and leaves the read-only rev nibble intact. */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_RESET);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    qtest_quit(qts);
}

/*
 * The station-address paths. The driver reads the MAC before any firmware is
 * downloaded, so both routes must answer independently of firmware state: the
 * bit-banged serial EEPROM (MISC_LOCAL_CTL) that the driver reads at attach,
 * and the GCR PAR0/PAR1 registers real firmware would present once running.
 *
 * The EEPROM helpers below replay the exact 24Cxx I2C sequence the NetBSD "ti"
 * driver's ti_eeprom_getbyte()/EEPROM_START()/EEPROM_STOP() macros drive
 * (if_ti.c, if_tireg.h): a start condition, an 8-bit control byte, two address
 * bytes for a write, a stop/start, an 8-bit read control byte, then eight bits
 * clocked out on EE_DIN.
 */
static uint32_t ee_read(QTestState *qts)
{
    return qtest_readl(qts, WINDOW_TIGON + TI_MISC_LOCAL_CTL);
}

static void ee_write(QTestState *qts, uint32_t v)
{
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_LOCAL_CTL, v);
}

static void ee_setbit(QTestState *qts, uint32_t bit)
{
    ee_write(qts, ee_read(qts) | bit);
}

static void ee_clrbit(QTestState *qts, uint32_t bit)
{
    ee_write(qts, ee_read(qts) & ~bit);
}

static void ee_start(QTestState *qts)
{
    ee_setbit(qts, TI_MLC_EE_CLK);
    ee_setbit(qts, TI_MLC_EE_DOUT);
    ee_setbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_DOUT);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static void ee_stop(QTestState *qts)
{
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_DOUT);
    ee_setbit(qts, TI_MLC_EE_CLK);
    ee_setbit(qts, TI_MLC_EE_TXEN);
    ee_setbit(qts, TI_MLC_EE_DOUT);
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static void ee_putbyte(QTestState *qts, uint8_t byte)
{
    int i;

    ee_setbit(qts, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        if (byte & i) {
            ee_setbit(qts, TI_MLC_EE_DOUT);
        } else {
            ee_clrbit(qts, TI_MLC_EE_DOUT);
        }
        ee_setbit(qts, TI_MLC_EE_CLK);
        ee_clrbit(qts, TI_MLC_EE_CLK);
    }
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    /* The ninth clock is the acknowledge; the driver reads and ignores it. */
    ee_setbit(qts, TI_MLC_EE_CLK);
    (void)ee_read(qts);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static uint8_t ee_getbyte(QTestState *qts, uint16_t addr)
{
    uint8_t byte = 0;
    int i;

    ee_start(qts);
    ee_putbyte(qts, 0xa0);              /* write control byte */
    ee_putbyte(qts, (addr >> 8) & 0xff);
    ee_putbyte(qts, addr & 0xff);
    ee_stop(qts);
    ee_start(qts);
    ee_putbyte(qts, 0xa1);             /* read control byte */

    ee_clrbit(qts, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        ee_setbit(qts, TI_MLC_EE_CLK);
        if (ee_read(qts) & TI_MLC_EE_DIN) {
            byte |= i;
        }
        ee_clrbit(qts, TI_MLC_EE_CLK);
    }
    ee_stop(qts);

    return byte;
}

static void test_station_addr_par(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* PAR0 = bytes 0-1, PAR1 = bytes 2-5, in the driver's big-endian pair. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_PAR0), ==,
                    ((uint32_t)tigon_mac[0] << 8) | tigon_mac[1]);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_PAR1), ==,
                    ((uint32_t)tigon_mac[2] << 24) |
                    ((uint32_t)tigon_mac[3] << 16) |
                    ((uint32_t)tigon_mac[4] << 8) | tigon_mac[5]);

    qtest_quit(qts);
}

static void test_station_addr_eeprom(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    int i;

    aim_window_at_bar(qts);

    /*
     * The six MAC bytes sit at TI_EE_MAC_OFFSET + 2, not at TI_EE_MAC_OFFSET:
     * the manual's stated offset is two zero pad bytes before the address
     * (if_ti.c comment at the read call). Confirm both facts.
     */
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET + 2 + i), ==,
                        tigon_mac[i]);
    }
    g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET), ==, 0);
    g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET + 1), ==, 0);

    qtest_quit(qts);
}

/*
 * Put the card in a state where its DMA lands big-endian in guest RAM: bus
 * master enabled, and OPMODE's descriptor/data byte-swap bits set, which is
 * what the driver programs on a big-endian host. The rings sit in low RAM the
 * Bridge carries straight through, so no address-translation setup is needed.
 */
static void enable_dma_big_endian(QTestState *qts)
{
    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE,
                 TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                 TI_OPMODE_BYTESWAP_DATA);
}

static void build_gib(QTestState *qts)
{
    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, EVENT_RING_PHYS, 0, 0x800);
    qtest_memset(qts, EV_PROD_PHYS, 0, 8);
    qtest_memset(qts, TX_CONS_PHYS, 0, 8);
    qtest_memset(qts, RETURN_RING_PHYS, 0, 0x4000);
    qtest_memset(qts, RETURN_PROD_PHYS, 0, 8);
    qtest_memset(qts, STD_RX_RING_PHYS, 0, 0x800);
    qtest_memset(qts, TX_RING_PHYS, 0, 0x800);

    write_rcb_addr(qts, GIB_PHYS + GIB_EV_RCB, EVENT_RING_PHYS);
    /* Tigon 2 puts the transmit ring in host DRAM (the host-ring flag). */
    write_rcb_addr(qts, GIB_PHYS + GIB_TX_RCB, TX_RING_PHYS);
    qtest_writel(qts, GIB_PHYS + GIB_TX_RCB + 8,
                 (512u << 16) | TI_RCB_FLAG_HOST_RING);
    write_rcb_addr(qts, GIB_PHYS + GIB_STD_RX_RCB, STD_RX_RING_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_RCB, RETURN_RING_PHYS);
    qtest_writel(qts, GIB_PHYS + GIB_RETURN_RCB + 8, (2048u << 16));
    write_rcb_addr(qts, GIB_PHYS + GIB_EV_PROD_PTR, EV_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_PROD_PTR, RETURN_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_TX_CONS_PTR, TX_CONS_PHYS);
}

static void publish_gib(QTestState *qts)
{
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_HI, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_LO, GIB_PHYS);
}

static void release_cpu(QTestState *qts)
{
    qtest_writel(qts, WINDOW_TIGON + TI_CPU_STATE, 0);
}

static bool irq_asserted(QTestState *qts)
{
    return (qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
            TI_MHC_INTSTATE) != 0;
}

static void test_firmware_up_requires_gib(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);

    /* Releasing the CPU without publishing the GIB must not post firmware-up. */
    release_cpu(qts);
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 0);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
}

static void test_firmware_up_posts_event(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);
    publish_gib(qts);
    release_cpu(qts);

    /* Both conditions met: a firmware-up event and the interrupt. */
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS), ==,
                    (uint32_t)TI_EV_FIRMWARE_UP << 24);
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 1);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/tigon/config-identity", NULL,
                        test_config_identity);
    qtest_add_data_func("/sn0/tigon/one-memory-bar", NULL,
                        test_one_memory_bar);
    qtest_add_data_func("/sn0/tigon/misc-host-ctl-chip-rev", NULL,
                        test_misc_host_ctl_chip_rev);
    qtest_add_data_func("/sn0/tigon/station-addr-par", NULL,
                        test_station_addr_par);
    qtest_add_data_func("/sn0/tigon/station-addr-eeprom", NULL,
                        test_station_addr_eeprom);
    qtest_add_data_func("/sn0/tigon/firmware-up-requires-gib", NULL,
                        test_firmware_up_requires_gib);
    qtest_add_data_func("/sn0/tigon/firmware-up-posts-event", NULL,
                        test_firmware_up_posts_event);

    return g_test_run();
}
