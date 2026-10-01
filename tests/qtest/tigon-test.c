/*
 * Alteon Tigon 2 (SGI "eg") device contract on a Bridge head.
 * Tests PCI identity, firmware rings, DMA byte order, and interrupts.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The "none" machine holds one Bridge and adjacent transfer memory: the card
 * uses its native head, with no Origin 200 node, crossbar, or fabric. The
 * populated machine's per-slot DEV_SWAP correction is covered separately in
 * tests/qtest/sn0-tigon-test.c. Tests cover the register contract, firmware
 * transmit/receive rings in host DRAM, producer-index safety, and interrupt
 * controls. Fixed byte-layout assertions check descriptor/data DMA polarity;
 * a self-round-trip would pass with either swap direction. Register offsets,
 * ring and descriptor layouts, and swap controls come from Tigon
 * documentation and are restated independently of the model.
 */

#include "qemu/osdep.h"
#include "libqos/libqos-sgi-bridge.h"
#include "libqtest.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_regs.h"
#include "hw/net/e1000_regs.h"

#include <sys/socket.h>
#include <netinet/in.h>

/* A fixed station address so PAR0/PAR1 and the EEPROM read are known. */
#define TIGON_MAC       "52:54:00:12:34:56"
static const uint8_t tigon_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

/* The card's identity. */
#define TIGON_VENDOR_ID     0x10a9
#define TIGON_DEVICE_ID     0x0009
#define TIGON_CLASS_CODE    0x020000
#define TIGON_CHIP_REV      0x6         /* Tigon 2 rev nibble */
#define TIGON_BAR0_SIZE     0x4000

/* Register offsets within BAR0 (if_tireg.h). */
#define TI_MISC_HOST_CTL    0x040
#define TI_MISC_LOCAL_CTL   0x044
#define TI_CPU_STATE        0x140
#define TI_MB_HOSTINTR      0x504

/*
 * The device's default arrival-to-completion floor is in nanoseconds.
 * Completion waits must advance virtual time and run the main loop.
 */
#define RX_ARRIVAL_LATENCY_NS  200000
#define TI_MB_SENDPROD_IDX  0x514
#define TI_MB_STDRXPROD_IDX 0x51c
#define TI_GCR_PAR0         0x600
#define TI_GCR_PAR1         0x604
#define TI_GCR_GENINFO_HI   0x608
#define TI_GCR_GENINFO_LO   0x60c
#define TI_GCR_OPMODE       0x618
#define TI_GCR_EVENTCONS_IDX 0x628
#define TI_GCR_RXRETURNCONS_IDX 0x680
#define TI_GCR_CMDRING      0x700
#define TI_MB_CMDPROD_IDX   0x50c
#define TI_CMD_RFRSH_STATS  0x11
/* Word index of ifHCOutBroadcastPkts in the block at the GIB's own head. */
#define STAT_HC_OUT_BROADCAST (50 * 4)

/* MISC_HOST_CTL bits. */
#define TI_MHC_INTSTATE      0x00000001
#define TI_MHC_CLEARINT      0x00000002
#define TI_MHC_RESET         0x00000008
#define TI_MHC_BYTE_SWAP_ENB 0x00000010
#define TI_MHC_WORD_SWAP_ENB 0x00000020
#define TI_MHC_MASK_INTS     0x00000040
#define TI_MHC_CHIP_REV_MASK 0xf0000000

/* Serial-EEPROM bit-bang lines in MISC_LOCAL_CTL. */
#define TI_MLC_EE_CLK       0x00100000
#define TI_MLC_EE_TXEN      0x00200000
#define TI_MLC_EE_DOUT      0x00400000
#define TI_MLC_EE_DIN       0x00800000
#define TI_EE_MAC_OFFSET    0x8c

/* GCR OPMODE: the descriptor/data DMA byte-swap control (if_tireg.h). */
#define TI_OPMODE_BYTESWAP_BD   0x00000002
#define TI_OPMODE_WORDSWAP_BD   0x00000004
#define TI_OPMODE_BYTESWAP_DATA 0x00000010

/* Ring flags, descriptor type, and the firmware-up event. */
#define TI_RCB_FLAG_HOST_RING   0x0040
#define TI_BDFLAG_END           0x0004
#define TI_BDTYPE_RECV_BD       0x0002
#define TI_EV_FIRMWARE_UP       0x01

/* Intel 82574L used to originate a packet from two transmit descriptors. */
#define E1000E_VENDOR_ID        0x8086
#define E1000E_DEVICE_ID        0x10d3
#define E1000E_TX_RING_LEN      0x1000

/* Ring sizes the RCBs advertise (if_tireg.h). */
#define EVENT_RING_CNT      256
#define RETURN_RING_CNT     2048
#define STD_RX_RING_CNT     512
#define TX_RING_CNT         512

/*
 * Field offsets inside the general information block (struct ti_gib): a 1 KB
 * statistics area, then the ring control blocks, then the host-memory
 * producer/consumer pointers. Restated from the public host-interface layout.
 */
#define GIB_EV_RCB          0x400
#define GIB_TX_RCB          0x420
#define GIB_STD_RX_RCB      0x430
#define GIB_RETURN_RCB      0x460
#define GIB_EV_PROD_PTR     0x470
#define GIB_RETURN_PROD_PTR 0x478
#define GIB_TX_CONS_PTR     0x480
#define GIB_SIZE            0x490

typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar bar;
    bool dma_be;                /* the DMA byte order the guest set in OPMODE */
    /* Guest-DRAM regions, allocated at bring-up. */
    uint64_t gib, ev_ring, ev_prod, tx_cons, ret_ring, ret_prod,
             std_rx_ring, tx_ring, rx_buf, tx_buf;
} TigonTest;

static void save_dev(QPCIDevice *dev, int devfn, void *data)
{
    *(QPCIDevice **)data = dev;
}

static QPCIDevice *find_tigon(QPCIBus *bus)
{
    QPCIDevice *dev = NULL;

    qpci_device_foreach(bus, TIGON_VENDOR_ID, TIGON_DEVICE_ID, save_dev, &dev);
    return dev;
}

static QPCIDevice *find_e1000e(QPCIBus *bus)
{
    QPCIDevice *dev = NULL;

    qpci_device_foreach(bus, E1000E_VENDOR_ID, E1000E_DEVICE_ID,
                        save_dev, &dev);
    return dev;
}

static void tigon_boot_cmd(TigonTest *t, const char *cmd)
{
    uint64_t barsize;

    memset(t, 0, sizeof(*t));
    t->qs = qtest_sgi_bridge_boot("%s", cmd);
    t->dev = find_tigon(t->qs->pcibus);
    g_assert(t->dev != NULL);

    t->bar = qpci_iomap(t->dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, TIGON_BAR0_SIZE);

    qpci_device_enable(t->dev);
}

static void tigon_boot(TigonTest *t)
{
    tigon_boot_cmd(t, "-device tigon,mac=" TIGON_MAC);
}

static void tigon_stop(TigonTest *t)
{
    g_free(t->dev);
    qtest_shutdown(t->qs);
}

static uint32_t reg_rd(TigonTest *t, uint64_t off)
{
    return qpci_io_readl(t->dev, t->bar, off);
}

static void reg_wr(TigonTest *t, uint64_t off, uint32_t val)
{
    qpci_io_writel(t->dev, t->bar, off, val);
}

static bool irq_pending(TigonTest *t)
{
    return (reg_rd(t, TI_MISC_HOST_CTL) & TI_MHC_INTSTATE) != 0;
}

/*
 * OPMODE selects guest-memory byte order for descriptor/GIB DMA. Read and
 * write it explicitly; qtest_writel's target-endian correction would hide
 * swap polarity.
 */
static void dma_stl(TigonTest *t, uint64_t addr, uint32_t val)
{
    uint8_t b[4];

    if (t->dma_be) {
        b[0] = val >> 24; b[1] = val >> 16; b[2] = val >> 8; b[3] = val;
    } else {
        b[0] = val; b[1] = val >> 8; b[2] = val >> 16; b[3] = val >> 24;
    }
    qtest_memwrite(t->qs->qts, addr, b, 4);
}

static uint32_t dma_ldl(TigonTest *t, uint64_t addr)
{
    uint8_t b[4];

    qtest_memread(t->qs->qts, addr, b, 4);
    if (t->dma_be) {
        return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
               ((uint32_t)b[2] << 8) | b[3];
    }
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

/* A 64-bit host DMA address is two words, high then low (ti_hostaddr). */
static void dma_st_hostaddr(TigonTest *t, uint64_t at, uint64_t addr)
{
    dma_stl(t, at, addr >> 32);
    dma_stl(t, at + 4, addr & 0xffffffff);
}

/*
 * Build GIB and rings in guest DRAM and set host-appropriate DMA order. Leave
 * the GIB address unpublished and CPU held to test firmware-up preconditions
 * separately.
 */
static void tigon_setup_rings(TigonTest *t, bool dma_be)
{
    QTestState *q = t->qs->qts;

    t->dma_be = dma_be;
    reg_wr(t, TI_GCR_OPMODE, dma_be ?
           (TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
            TI_OPMODE_BYTESWAP_DATA) : 0);

    t->gib         = qmalloc(t->qs, GIB_SIZE);
    t->ev_ring     = qmalloc(t->qs, EVENT_RING_CNT * 8);
    t->ev_prod     = qmalloc(t->qs, 8);
    t->tx_cons     = qmalloc(t->qs, 8);
    t->ret_ring    = qmalloc(t->qs, RETURN_RING_CNT * 32);
    t->ret_prod    = qmalloc(t->qs, 8);
    t->std_rx_ring = qmalloc(t->qs, STD_RX_RING_CNT * 32);
    t->tx_ring     = qmalloc(t->qs, TX_RING_CNT * 16);
    t->rx_buf      = qmalloc(t->qs, 2048);
    t->tx_buf      = qmalloc(t->qs, 2048);

    qtest_memset(q, t->gib, 0, GIB_SIZE);
    qtest_memset(q, t->ev_ring, 0, EVENT_RING_CNT * 8);
    qtest_memset(q, t->ev_prod, 0, 8);
    qtest_memset(q, t->tx_cons, 0, 8);
    qtest_memset(q, t->ret_ring, 0, RETURN_RING_CNT * 32);
    qtest_memset(q, t->ret_prod, 0, 8);
    qtest_memset(q, t->std_rx_ring, 0, STD_RX_RING_CNT * 32);
    qtest_memset(q, t->tx_ring, 0, TX_RING_CNT * 16);

    /* Event ring control block and the host slot for its producer index. */
    dma_st_hostaddr(t, t->gib + GIB_EV_RCB, t->ev_ring);
    dma_st_hostaddr(t, t->gib + GIB_EV_PROD_PTR, t->ev_prod);

    /* Tigon 2 keeps the transmit ring in host DRAM (the host-ring flag). */
    dma_st_hostaddr(t, t->gib + GIB_TX_RCB, t->tx_ring);
    dma_stl(t, t->gib + GIB_TX_RCB + 8,
            ((uint32_t)TX_RING_CNT << 16) | TI_RCB_FLAG_HOST_RING);
    dma_st_hostaddr(t, t->gib + GIB_TX_CONS_PTR, t->tx_cons);

    dma_st_hostaddr(t, t->gib + GIB_STD_RX_RCB, t->std_rx_ring);

    dma_st_hostaddr(t, t->gib + GIB_RETURN_RCB, t->ret_ring);
    dma_stl(t, t->gib + GIB_RETURN_RCB + 8, (uint32_t)RETURN_RING_CNT << 16);
    dma_st_hostaddr(t, t->gib + GIB_RETURN_PROD_PTR, t->ret_prod);
}

/* Publish the GIB address and release embedded CPU A -> firmware-up. */
static void tigon_publish(TigonTest *t)
{
    /*
     * The little-endian register view puts the low half at the lower offset.
     */
    reg_wr(t, TI_GCR_GENINFO_HI, t->gib & 0xffffffff);
    reg_wr(t, TI_GCR_GENINFO_LO, t->gib >> 32);
    reg_wr(t, TI_CPU_STATE, 0);   /* clear the halt bit */
}

static void tigon_operational(TigonTest *t, bool dma_be)
{
    tigon_setup_rings(t, dma_be);
    tigon_publish(t);
}

/* ---- config identity and the BAR ---- */

static void test_config_identity(void)
{
    TigonTest t;

    tigon_boot(&t);

    g_assert_cmphex(qpci_config_readw(t.dev, PCI_VENDOR_ID), ==,
                    TIGON_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(t.dev, PCI_DEVICE_ID), ==,
                    TIGON_DEVICE_ID);
    /* Class code sits in the top three bytes of the class/revision word. */
    g_assert_cmphex(qpci_config_readl(t.dev, PCI_CLASS_REVISION) >> 8, ==,
                    TIGON_CLASS_CODE);
    /* One interrupt pin, INTA. */
    g_assert_cmphex(qpci_config_readb(t.dev, PCI_INTERRUPT_PIN), ==, 0x01);

    tigon_stop(&t);
}

static void test_bar0_shape(void)
{
    QOSState *qs = qtest_sgi_bridge_boot("-device tigon,mac=" TIGON_MAC);
    QPCIDevice *dev = find_tigon(qs->pcibus);

    g_assert(dev != NULL);

    /*
     * BAR0 is a single 32-bit, non-prefetchable memory BAR: bit 0 clear
     * (memory), bits 2:1 clear (32-bit), bit 3 clear (non-prefetchable), and
     * the size mask leaves the low 14 bits (16 KB) zero.
     */
    qpci_config_writel(dev, PCI_BASE_ADDRESS_0, 0xffffffffu);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_0), ==,
                    0xffffc000u);

    /* There is no second BAR. */
    qpci_config_writel(dev, PCI_BASE_ADDRESS_1, 0xffffffffu);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_1), ==, 0);

    g_free(dev);
    qtest_shutdown(qs);
}

/* ---- register contract ---- */

static void test_chip_rev_and_reset(void)
{
    TigonTest t;

    tigon_boot(&t);

    /* The chip-revision nibble is the driver's Tigon 1 vs 2 runtime test. */
    g_assert_cmphex(reg_rd(&t, TI_MISC_HOST_CTL) & TI_MHC_CHIP_REV_MASK, ==,
                    (uint32_t)TIGON_CHIP_REV << 28);

    /*
     * The RESET bit is accepted and leaves the read-only rev nibble intact.
     */
    reg_wr(&t, TI_MISC_HOST_CTL, TI_MHC_RESET);
    g_assert_cmphex(reg_rd(&t, TI_MISC_HOST_CTL) & TI_MHC_CHIP_REV_MASK, ==,
                    (uint32_t)TIGON_CHIP_REV << 28);

    tigon_stop(&t);
}

/*
 * Register-swap bits and descriptor-swap OPMODE read back as programmed.
 * Byte-order tests separately check descriptor-swap behavior.
 */
static void test_register_config_roundtrip(void)
{
    TigonTest t;

    tigon_boot(&t);

    reg_wr(&t, TI_MISC_HOST_CTL, TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB);
    g_assert_cmphex(reg_rd(&t, TI_MISC_HOST_CTL) &
                    (TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB), ==,
                    TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB);

    reg_wr(&t, TI_GCR_OPMODE,
           TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
           TI_OPMODE_BYTESWAP_DATA);
    g_assert_cmphex(reg_rd(&t, TI_GCR_OPMODE), ==,
                    TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                    TI_OPMODE_BYTESWAP_DATA);

    tigon_stop(&t);
}

/* ---- station address, both routes ---- */

static void test_station_addr_par(void)
{
    TigonTest t;

    tigon_boot(&t);

    /* PAR0 = bytes 0-1, PAR1 = bytes 2-5, in the driver's big-endian pair. */
    g_assert_cmphex(reg_rd(&t, TI_GCR_PAR0), ==,
                    ((uint32_t)tigon_mac[0] << 8) | tigon_mac[1]);
    g_assert_cmphex(reg_rd(&t, TI_GCR_PAR1), ==,
                    ((uint32_t)tigon_mac[2] << 24) |
                    ((uint32_t)tigon_mac[3] << 16) |
                    ((uint32_t)tigon_mac[4] << 8) | tigon_mac[5]);

    tigon_stop(&t);
}

/*
 * Attach reads the serial EEPROM before firmware download. Helpers use the
 * NetBSD ti driver's exact 24Cxx I2C sequence from
 * ti_eeprom_getbyte()/EEPROM_START()/EEPROM_STOP().
 */
static uint32_t ee_read(TigonTest *t)
{
    return reg_rd(t, TI_MISC_LOCAL_CTL);
}

static void ee_write(TigonTest *t, uint32_t v)
{
    reg_wr(t, TI_MISC_LOCAL_CTL, v);
}

static void ee_setbit(TigonTest *t, uint32_t bit)
{
    ee_write(t, ee_read(t) | bit);
}

static void ee_clrbit(TigonTest *t, uint32_t bit)
{
    ee_write(t, ee_read(t) & ~bit);
}

static void ee_start(TigonTest *t)
{
    ee_setbit(t, TI_MLC_EE_CLK);
    ee_setbit(t, TI_MLC_EE_DOUT);
    ee_setbit(t, TI_MLC_EE_TXEN);
    ee_clrbit(t, TI_MLC_EE_DOUT);
    ee_clrbit(t, TI_MLC_EE_CLK);
}

static void ee_stop(TigonTest *t)
{
    ee_clrbit(t, TI_MLC_EE_TXEN);
    ee_clrbit(t, TI_MLC_EE_DOUT);
    ee_setbit(t, TI_MLC_EE_CLK);
    ee_setbit(t, TI_MLC_EE_TXEN);
    ee_setbit(t, TI_MLC_EE_DOUT);
    ee_clrbit(t, TI_MLC_EE_TXEN);
    ee_clrbit(t, TI_MLC_EE_CLK);
}

static void ee_putbyte(TigonTest *t, uint8_t byte)
{
    int i;

    ee_setbit(t, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        if (byte & i) {
            ee_setbit(t, TI_MLC_EE_DOUT);
        } else {
            ee_clrbit(t, TI_MLC_EE_DOUT);
        }
        ee_setbit(t, TI_MLC_EE_CLK);
        ee_clrbit(t, TI_MLC_EE_CLK);
    }
    ee_clrbit(t, TI_MLC_EE_TXEN);
    /* The ninth clock is the acknowledge; the driver reads and ignores it. */
    ee_setbit(t, TI_MLC_EE_CLK);
    (void)ee_read(t);
    ee_clrbit(t, TI_MLC_EE_CLK);
}

static uint8_t ee_getbyte(TigonTest *t, uint16_t addr)
{
    uint8_t byte = 0;
    int i;

    ee_start(t);
    ee_putbyte(t, 0xa0);                /* write control byte */
    ee_putbyte(t, (addr >> 8) & 0xff);
    ee_putbyte(t, addr & 0xff);
    ee_stop(t);
    ee_start(t);
    ee_putbyte(t, 0xa1);                /* read control byte */

    ee_clrbit(t, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        ee_setbit(t, TI_MLC_EE_CLK);
        if (ee_read(t) & TI_MLC_EE_DIN) {
            byte |= i;
        }
        ee_clrbit(t, TI_MLC_EE_CLK);
    }
    ee_stop(t);

    return byte;
}

static void test_station_addr_eeprom(void)
{
    TigonTest t;
    int i;

    tigon_boot(&t);

    /*
     * The six MAC bytes sit at TI_EE_MAC_OFFSET + 2, not at TI_EE_MAC_OFFSET:
     * the manual's stated offset is two zero pad bytes before the address.
     */
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(ee_getbyte(&t, TI_EE_MAC_OFFSET + 2 + i), ==,
                        tigon_mac[i]);
    }
    g_assert_cmphex(ee_getbyte(&t, TI_EE_MAC_OFFSET), ==, 0);
    g_assert_cmphex(ee_getbyte(&t, TI_EE_MAC_OFFSET + 1), ==, 0);

    tigon_stop(&t);
}

/* ---- firmware-up handshake ---- */

static void test_firmware_up_requires_gib(void)
{
    TigonTest t;

    tigon_boot(&t);
    tigon_setup_rings(&t, false);

    /* Releasing the CPU with no GIB published must not post firmware-up. */
    reg_wr(&t, TI_CPU_STATE, 0);
    g_assert_cmphex(dma_ldl(&t, t.ev_ring), ==, 0);
    g_assert_cmphex(dma_ldl(&t, t.ev_prod), ==, 0);
    g_assert_false(irq_pending(&t));

    tigon_stop(&t);
}

static void test_firmware_up_posts_event(void)
{
    TigonTest t;

    tigon_boot(&t);
    tigon_operational(&t, false);

    /* Both preconditions met: a firmware-up event and the interrupt. */
    g_assert_cmphex(dma_ldl(&t, t.ev_ring), ==,
                    (uint32_t)TI_EV_FIRMWARE_UP << 24);
    g_assert_cmphex(dma_ldl(&t, t.ev_prod), ==, 1);
    g_assert_true(irq_pending(&t));

    tigon_stop(&t);
}

/* ---- transmit and receive over a socket netdev ---- */

static void net_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static size_t net_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

static void net_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

/* Give the main loop passes to read the socket and deliver a frame. */
static uint32_t wait_return_prod(TigonTest *t)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (dma_ldl(t, t->ret_prod) != 0) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(t->qs->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    return dma_ldl(t, t->ret_prod);
}

static void tigon_boot_netdev_args(TigonTest *t, int fd, const char *args)
{
    char *cmd = g_strdup_printf(
        "-netdev socket,fd=%d,id=n0 "
        "-device tigon,netdev=n0,mac=" TIGON_MAC " %s",
        fd, args ? args : "");

    tigon_boot_cmd(t, cmd);
    g_free(cmd);
}

static void tigon_boot_netdev(TigonTest *t, int fd)
{
    tigon_boot_netdev_args(t, fd, NULL);
}

/* A minimal well-formed ethernet frame the test chooses byte for byte. */
static const uint8_t test_frame[60] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,      /* broadcast dest */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* source = our MAC */
    0x08, 0x06,                              /* ethertype ARP */
    0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
};

static void test_transmit(void)
{
    int sv[2];
    TigonTest t;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    tigon_boot_netdev(&t, sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    tigon_operational(&t, false);

    /* One transmit descriptor: a single fragment marked end-of-frame. */
    qtest_memwrite(t.qs->qts, t.tx_buf, test_frame, sizeof(test_frame));
    dma_st_hostaddr(&t, t.tx_ring + 0, t.tx_buf);
    dma_stl(&t, t.tx_ring + 8,
            ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    dma_stl(&t, t.tx_ring + 12, 0);

    /* Ring the send doorbell: producer index 1. */
    reg_wr(&t, TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));

    /* The transmit consumer index advanced in host memory. */
    g_assert_cmphex(dma_ldl(&t, t.tx_cons), ==, 1);

    tigon_stop(&t);
    close(sv[0]);
}

static void tigon_migrate(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }
        g_assert_true(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static char *tigon_migration_directory(void)
{
    g_autofree char *cwd = g_get_current_dir();
    char *directory = g_build_filename(cwd,
                                       ".tigon-migration-XXXXXX", NULL);

    g_assert_nonnull(g_mkdtemp(directory));
    return directory;
}

static void tigon_migration_cleanup(const char *directory, const char *socket)
{
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* Current migration retains an operational ring engine and guest RAM. */
static void test_operational_ring_migration(void)
{
    g_autofree char *directory = tigon_migration_directory();
    g_autofree char *socket = g_build_filename(directory, "migration.sock",
                                                NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    int from_sv[2], to_sv[2];
    TigonTest from, to;
    uint8_t got[sizeof(test_frame)];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    tigon_boot_netdev(&from, from_sv[1]);
    close(from_sv[1]);
    net_recv_timeout(from_sv[0], 5);
    tigon_operational(&from, false);

    /* One frame before the migration, so a counter has something to keep. */
    qtest_memwrite(from.qs->qts, from.tx_buf, test_frame, sizeof(test_frame));
    dma_st_hostaddr(&from, from.tx_ring, from.tx_buf);
    dma_stl(&from, from.tx_ring + 8,
            ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    dma_stl(&from, from.tx_ring + 12, 0);
    reg_wr(&from, TI_MB_SENDPROD_IDX, 1);
    len = net_recv_frame(from_sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(test_frame));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    tigon_boot_netdev_args(&to, to_sv[1], incoming);
    close(to_sv[1]);
    net_recv_timeout(to_sv[0], 5);
    qtest_qmp_assert_success(to.qs->qts,
        "{ 'execute': 'migrate-set-parameters', "
        "  'arguments': { 'announce-rounds': 0 } }");

    tigon_migrate(from.qs->qts, uri);

    /* The ring locations live in device state; their contents live in RAM. */
    to.dma_be = from.dma_be;
    to.gib = from.gib;
    to.ev_ring = from.ev_ring;
    to.ev_prod = from.ev_prod;
    to.tx_cons = from.tx_cons;
    to.ret_ring = from.ret_ring;
    to.ret_prod = from.ret_prod;
    to.std_rx_ring = from.std_rx_ring;
    to.tx_ring = from.tx_ring;
    to.rx_buf = from.rx_buf;
    to.tx_buf = from.tx_buf;

    g_assert_cmphex(dma_ldl(&to, to.ev_ring), ==,
                    (uint32_t)TI_EV_FIRMWARE_UP << 24);
    g_assert_cmphex(dma_ldl(&to, to.ev_prod), ==, 1);
    g_assert_cmphex(reg_rd(&to, TI_GCR_OPMODE), ==, 0);

    /* The consumer index crossed with the device: this is the second slot. */
    qtest_memwrite(to.qs->qts, to.tx_buf, test_frame, sizeof(test_frame));
    dma_st_hostaddr(&to, to.tx_ring + 16, to.tx_buf);
    dma_stl(&to, to.tx_ring + 24,
            ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    dma_stl(&to, to.tx_ring + 28, 0);
    reg_wr(&to, TI_MB_SENDPROD_IDX, 2);

    len = net_recv_frame(to_sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));
    g_assert_cmphex(dma_ldl(&to, to.tx_cons), ==, 2);

    /*
     * Two broadcast frames straddle migration; destination counters must show
     * both. Each 64-bit counter has two words, most significant first.
     */
    reg_wr(&to, TI_GCR_CMDRING, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    reg_wr(&to, TI_MB_CMDPROD_IDX, 1);
    g_assert_cmphex(dma_ldl(&to, to.gib + STAT_HC_OUT_BROADCAST), ==, 0);
    g_assert_cmphex(dma_ldl(&to, to.gib + STAT_HC_OUT_BROADCAST + 4), ==, 2);

    tigon_stop(&to);
    tigon_stop(&from);
    close(to_sv[0]);
    close(from_sv[0]);
    tigon_migration_cleanup(directory, socket);
}

/* A standard receive-buffer descriptor: address, index and length. */
static void post_std_rx_buffer(TigonTest *t, unsigned slot, uint64_t phys,
                               uint16_t idx, uint32_t bytes)
{
    uint64_t desc = t->std_rx_ring + (uint64_t)slot * 32;

    dma_st_hostaddr(t, desc + 0, phys);
    dma_stl(t, desc + 8, ((uint32_t)idx << 16) | (bytes & 0xffff));
    dma_stl(t, desc + 12, (uint32_t)TI_BDTYPE_RECV_BD << 16);
}

static const uint8_t rx_frame_ours[64] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* dest = our MAC */
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,      /* source */
    0x08, 0x00,                              /* ethertype IP */
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

static const uint8_t rx_frame_other[64] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x99,      /* dest = a different unicast */
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x08, 0x00,
    0x99, 0x98, 0x97, 0x96,
};

static void test_receive(void)
{
    int sv[2];
    TigonTest t;
    uint8_t landed[64];
    uint32_t word2;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    tigon_boot_netdev(&t, sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    tigon_operational(&t, false);

    qtest_memset(t.qs->qts, t.rx_buf, 0, 2048);
    post_std_rx_buffer(&t, 0, t.rx_buf, 0, 2048);
    reg_wr(&t, TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    /* The return producer index advances once the frame is delivered. */
    g_assert_cmphex(wait_return_prod(&t), ==, 1);

    /* Return descriptor names the buffer index and the received length. */
    word2 = dma_ldl(&t, t.ret_ring + 8);
    g_assert_cmphex(word2 >> 16, ==, 0);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));

    /* The frame landed in the posted buffer, byte for byte. */
    qtest_memread(t.qs->qts, t.rx_buf, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_true(irq_pending(&t));

    tigon_stop(&t);
    close(sv[0]);
}

/*
 * The card writes a received frame and return descriptor immediately, but
 * publishes the host-memory producer index and interrupt only after the
 * virtual-clock arrival deadline. The descriptor is invisible until indexed;
 * check it is written while the producer remains unchanged. Without the
 * floor, a transmit and reply complete in one virtual instant, unlike wire,
 * peer, or DMA latency.
 */
static void test_receive_waits_for_its_arrival_deadline(void)
{
    int sv[2];
    TigonTest t;
    uint8_t landed[sizeof(rx_frame_ours)];
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    tigon_boot_netdev(&t, sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    tigon_operational(&t, false);

    /* Drop the firmware-up interrupt so the line starts quiet. */
    reg_wr(&t, TI_MISC_HOST_CTL, TI_MHC_CLEARINT);
    g_assert_false(irq_pending(&t));

    qtest_memset(t.qs->qts, t.rx_buf, 0, 2048);
    post_std_rx_buffer(&t, 0, t.rx_buf, 0, 2048);
    reg_wr(&t, TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    /*
     * Wait for the card to take the frame, which it shows by writing it into
     * the buffer. Nothing here advances the virtual clock, so the arrival
     * deadline cannot have passed.
     */
    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(t.qs->qts, t.rx_buf, landed, sizeof(landed));
        if (memcmp(landed, rx_frame_ours, sizeof(landed)) == 0) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmpmem(landed, sizeof(landed),
                    rx_frame_ours, sizeof(rx_frame_ours));

    /* The return descriptor is written; the producer and the line are not. */
    g_assert_cmphex(dma_ldl(&t, t.ret_ring + 8) & 0xffff, ==,
                    sizeof(rx_frame_ours));
    g_assert_cmphex(dma_ldl(&t, t.ret_prod), ==, 0);
    g_assert_false(irq_pending(&t));

    /* Still nothing one nanosecond short of the deadline. */
    qtest_clock_step(t.qs->qts, RX_ARRIVAL_LATENCY_NS - 1);
    g_assert_cmphex(dma_ldl(&t, t.ret_prod), ==, 0);
    g_assert_false(irq_pending(&t));

    /* And both at once past it. */
    qtest_clock_step(t.qs->qts, 1);
    g_assert_cmphex(dma_ldl(&t, t.ret_prod), ==, 1);
    g_assert_true(irq_pending(&t));

    tigon_stop(&t);
    close(sv[0]);
}

/*
 * Compare flat delivery with a two-descriptor split inside IPv4. libqos can
 * originate a scattered frame only with an Intel PCI Express card. Bridge is
 * conventional PCI and mips64-softmmu has no PCI Express, so that target
 * cannot run the fragmented half without comparing a frame against itself.
 */
static void test_receive_iov_matches_flat(void)
{
    static const uint8_t frame[60] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x08, 0x00,
        0x45, 0x00, 0x00, 0x2e, 0x12, 0x34, 0x00, 0x00,
        0x40, 0x11, 0xab, 0xcd, 0xc0, 0x00, 0x02, 0x01,
        0xc6, 0x33, 0x64, 0x02,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a,
    };
    g_autofree char *trace_path = NULL;
    g_autofree char *trace = NULL;
    QPCIBar e1000e_bar;
    QPCIDevice *e1000e;
    TigonTest flat, fragmented;
    struct e1000_tx_desc tx_desc[4] = { };
    uint8_t flat_payload[2][sizeof(frame)];
    uint8_t fragmented_payload[2][sizeof(frame)];
    uint8_t flat_return[64], fragmented_return[64];
    g_autofree char *cmd = NULL;
    uint64_t flat_buf2, fragmented_buf2, tx_ring, tx_buf, barsize;
    int flat_sv[2], trace_fd, attempt;

    if (!qtest_has_device("e1000e")) {
        g_test_skip("no PCI Express endpoint on this machine to originate a "
                    "fragmented frame");
        return;
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, flat_sv), ==, 0);
    tigon_boot_netdev(&flat, flat_sv[1]);
    close(flat_sv[1]);
    tigon_operational(&flat, false);
    flat_buf2 = qmalloc(flat.qs, 2048);
    post_std_rx_buffer(&flat, 0, flat.rx_buf, 7, 2048);
    post_std_rx_buffer(&flat, 1, flat_buf2, 8, 2048);
    reg_wr(&flat, TI_MB_STDRXPROD_IDX, 2);
    net_send_frame(flat_sv[0], frame, sizeof(frame));
    net_send_frame(flat_sv[0], frame, sizeof(frame));
    for (attempt = 0; attempt < 200 && dma_ldl(&flat, flat.ret_prod) != 2;
         attempt++) {
        g_usleep(5000);
        qtest_clock_step(flat.qs->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(dma_ldl(&flat, flat.ret_prod), ==, 2);
    qtest_memread(flat.qs->qts, flat.rx_buf, flat_payload,
                  sizeof(flat_payload[0]));
    qtest_memread(flat.qs->qts, flat_buf2, flat_payload[1],
                  sizeof(flat_payload[1]));
    qtest_memread(flat.qs->qts, flat.ret_ring, flat_return,
                  sizeof(flat_return));
    g_assert_true(irq_pending(&flat));

    trace_fd = g_file_open_tmp("tigon-receive-iov-XXXXXX", &trace_path, NULL);
    g_assert_cmpint(trace_fd, >=, 0);
    close(trace_fd);
    cmd = g_strdup_printf(
        "-netdev hubport,id=tigon-net,hubid=0 "
        "-device tigon,netdev=tigon-net,mac=" TIGON_MAC " "
        "-netdev hubport,id=e1000e-net,hubid=0 "
        "-device e1000e,netdev=e1000e-net "
        "-trace tigon_receive_iov -D %s", trace_path);
    tigon_boot_cmd(&fragmented, cmd);
    tigon_operational(&fragmented, false);
    fragmented_buf2 = qmalloc(fragmented.qs, 2048);
    post_std_rx_buffer(&fragmented, 0, fragmented.rx_buf, 7, 2048);
    post_std_rx_buffer(&fragmented, 1, fragmented_buf2, 8, 2048);
    reg_wr(&fragmented, TI_MB_STDRXPROD_IDX, 2);

    e1000e = find_e1000e(fragmented.qs->pcibus);
    g_assert_nonnull(e1000e);
    e1000e_bar = qpci_iomap(e1000e, 0, &barsize);
    qpci_device_enable(e1000e);
    tx_ring = qmalloc(fragmented.qs, E1000E_TX_RING_LEN);
    tx_buf = qmalloc(fragmented.qs, sizeof(frame));
    qtest_memwrite(fragmented.qs->qts, tx_buf, frame, sizeof(frame));
    tx_desc[0].buffer_addr = cpu_to_le64(tx_buf);
    tx_desc[0].lower.data = cpu_to_le32(35);
    tx_desc[1].buffer_addr = cpu_to_le64(tx_buf + 35);
    tx_desc[1].lower.data = cpu_to_le32(E1000_TXD_CMD_RS |
                                       E1000_TXD_CMD_EOP |
                                       (sizeof(frame) - 35));
    tx_desc[2] = tx_desc[0];
    tx_desc[3] = tx_desc[1];
    qtest_memwrite(fragmented.qs->qts, tx_ring, tx_desc, sizeof(tx_desc));
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDBAL, tx_ring);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDBAH, tx_ring >> 32);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDLEN, E1000E_TX_RING_LEN);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDH, 0);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDT, 0);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TCTL, E1000_TCTL_EN);
    qpci_io_writel(e1000e, e1000e_bar, E1000_TDT, 4);
    for (attempt = 0;
         attempt < 200 && dma_ldl(&fragmented, fragmented.ret_prod) != 2;
         attempt++) {
        g_usleep(5000);
        qtest_clock_step(fragmented.qs->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(dma_ldl(&fragmented, fragmented.ret_prod), ==, 2);
    qtest_memread(fragmented.qs->qts, fragmented.rx_buf,
                  fragmented_payload[0], sizeof(fragmented_payload[0]));
    qtest_memread(fragmented.qs->qts, fragmented_buf2,
                  fragmented_payload[1], sizeof(fragmented_payload[1]));
    qtest_memread(fragmented.qs->qts, fragmented.ret_ring,
                  fragmented_return, sizeof(fragmented_return));
    g_assert_true(irq_pending(&fragmented));
    g_assert_cmpmem(fragmented_payload[0], sizeof(fragmented_payload[0]),
                    frame, sizeof(frame));
    g_assert_cmpmem(fragmented_payload[1], sizeof(fragmented_payload[1]),
                    frame, sizeof(frame));
    g_assert_cmpmem(fragmented_payload, sizeof(fragmented_payload),
                    flat_payload, sizeof(flat_payload));
    g_assert_cmpmem(fragmented_return, sizeof(fragmented_return),
                    flat_return, sizeof(flat_return));
    g_assert_cmphex(dma_ldl(&fragmented, fragmented.ret_ring + 16), ==,
                    0x2f79a9b6);
    g_assert_cmphex(dma_ldl(&fragmented, fragmented.ret_ring + 48), ==,
                    0x2f79a9b6);
    g_free(e1000e);
    tigon_stop(&fragmented);
    g_assert_cmpint(g_file_get_contents(trace_path, &trace, NULL, NULL), ==,
                    true);
    g_assert_nonnull(strstr(trace, "tigon_receive_iov "));
    g_assert_nonnull(strstr(trace, "60 bytes"));

    tigon_stop(&flat);
    close(flat_sv[0]);
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
}

static void test_receive_filters_other(void)
{
    int sv[2];
    TigonTest t;
    uint8_t landed[64];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    tigon_boot_netdev(&t, sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    tigon_operational(&t, false);

    qtest_memset(t.qs->qts, t.rx_buf, 0, 2048);
    post_std_rx_buffer(&t, 0, t.rx_buf, 0, 2048);
    post_std_rx_buffer(&t, 1, t.rx_buf + 0x1000, 1, 2048);
    reg_wr(&t, TI_MB_STDRXPROD_IDX, 2);

    /*
     * The backend reads a foreign-station frame before one addressed to us.
     * Only the latter reaches the ring; bad filtering makes producer 2 and
     * replaces the buffer with the wrong frame.
     */
    net_send_frame(sv[0], rx_frame_other, sizeof(rx_frame_other));
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_cmphex(wait_return_prod(&t), ==, 1);

    qtest_memread(t.qs->qts, t.rx_buf, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    tigon_stop(&t);
    close(sv[0]);
}

static void test_receive_clamps_to_buffer(void)
{
    static const uint8_t big_frame[128] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,   /* dest = our MAC */
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x08, 0x00, 0xa1, 0xa2, 0xa3, 0xa4,
    };
    int sv[2];
    TigonTest t;
    uint8_t guard[64];
    uint32_t word2;
    int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    tigon_boot_netdev(&t, sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    tigon_operational(&t, false);

    /* A 64-byte buffer, with the 64 bytes past it seeded to catch a spill. */
    qtest_memset(t.qs->qts, t.rx_buf, 0, 128);
    qtest_memset(t.qs->qts, t.rx_buf + 64, 0xa5, 64);
    post_std_rx_buffer(&t, 0, t.rx_buf, 0, 64);
    reg_wr(&t, TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], big_frame, sizeof(big_frame));
    g_assert_cmphex(wait_return_prod(&t), ==, 1);

    /* The reported length is clamped to the buffer, not the frame. */
    word2 = dma_ldl(&t, t.ret_ring + 8);
    g_assert_cmphex(word2 & 0xffff, ==, 64);

    /* Nothing landed past the buffer: the seeded bytes are intact. */
    qtest_memread(t.qs->qts, t.rx_buf + 64, guard, sizeof(guard));
    for (i = 0; i < (int)sizeof(guard); i++) {
        g_assert_cmphex(guard[i], ==, 0xa5);
    }

    tigon_stop(&t);
    close(sv[0]);
}

/* ---- ring producer-index safety ---- */

/*
 * Out-of-range mailbox producer indices must reduce into the ring instead of
 * spinning toward an unreachable value. Four billion would hang without the
 * guard; after writing it, the test requires the machine to answer.
 */
static void test_producer_index_safety(void)
{
    TigonTest t;

    tigon_boot(&t);
    tigon_operational(&t, false);

    reg_wr(&t, TI_MB_SENDPROD_IDX, 0xffffffffu);
    reg_wr(&t, TI_MB_SENDPROD_IDX, 0x40000000u);
    reg_wr(&t, TI_MB_STDRXPROD_IDX, 0xffffffffu);

    /* The walks terminated: the register file still answers. */
    g_assert_cmphex(reg_rd(&t, TI_MISC_HOST_CTL) & TI_MHC_CHIP_REV_MASK, ==,
                    (uint32_t)TIGON_CHIP_REV << 28);

    tigon_stop(&t);
}

/* ---- interrupt controls ---- */

static void test_interrupt_mask_ack(void)
{
    TigonTest t;

    tigon_boot(&t);
    tigon_operational(&t, false);
    g_assert_true(irq_pending(&t));

    /* The mailbox write acknowledges and holds off the current interrupt. */
    reg_wr(&t, TI_MB_HOSTINTR, 1);
    g_assert_false(irq_pending(&t));
    reg_wr(&t, TI_MB_HOSTINTR, 0);
    g_assert_false(irq_pending(&t));

    tigon_stop(&t);

    tigon_boot(&t);
    tigon_operational(&t, false);
    g_assert_true(irq_pending(&t));

    /* The chip-level mask suppresses delivery without acknowledging it. */
    reg_wr(&t, TI_MISC_HOST_CTL, TI_MHC_MASK_INTS);
    g_assert_false(irq_pending(&t));
    reg_wr(&t, TI_MISC_HOST_CTL, 0);
    g_assert_true(irq_pending(&t));

    /* Clear-interrupt drops the pending latch for good. */
    reg_wr(&t, TI_MISC_HOST_CTL, TI_MHC_CLEARINT);
    g_assert_false(irq_pending(&t));

    tigon_stop(&t);
}

/* The mailbox write acknowledges independently of ring-consumer progress. */
static void test_interrupt_hostintr_ack(void)
{
    TigonTest t;

    tigon_boot(&t);
    tigon_operational(&t, false);
    g_assert_true(irq_pending(&t));

    /* Advancing the consumer index alone does not acknowledge the line. */
    reg_wr(&t, TI_GCR_EVENTCONS_IDX, 1);
    g_assert_true(irq_pending(&t));

    reg_wr(&t, TI_MB_HOSTINTR, 0);
    g_assert_false(irq_pending(&t));

    tigon_stop(&t);
}

/* ---- byte order, pinned by golden fixed-byte-layout ---- */

/*
 * OPMODE swap selects raw guest-memory byte positions for event 0x01000000
 * and producer index 1. Exact byte assertions catch reversed polarity; a
 * self-round-trip passes either orientation. On the generic host, no Bridge
 * alters device writes before they reach guest memory.
 */
static void byte_order_golden(bool dma_be)
{
    TigonTest t;
    uint8_t seen[4];

    tigon_boot(&t);
    tigon_operational(&t, dma_be);

    /* The event word 0x01000000: 0x01 in byte 0 big-endian, byte 3 little. */
    qtest_memread(t.qs->qts, t.ev_ring, seen, 4);
    if (dma_be) {
        g_assert_cmphex(seen[0], ==, 0x01);
        g_assert_cmphex(seen[1], ==, 0x00);
        g_assert_cmphex(seen[2], ==, 0x00);
        g_assert_cmphex(seen[3], ==, 0x00);
    } else {
        g_assert_cmphex(seen[0], ==, 0x00);
        g_assert_cmphex(seen[1], ==, 0x00);
        g_assert_cmphex(seen[2], ==, 0x00);
        g_assert_cmphex(seen[3], ==, 0x01);
    }

    /* The producer index 1: 0x01 in byte 3 big-endian, byte 0 little. */
    qtest_memread(t.qs->qts, t.ev_prod, seen, 4);
    if (dma_be) {
        g_assert_cmphex(seen[0], ==, 0x00);
        g_assert_cmphex(seen[1], ==, 0x00);
        g_assert_cmphex(seen[2], ==, 0x00);
        g_assert_cmphex(seen[3], ==, 0x01);
    } else {
        g_assert_cmphex(seen[0], ==, 0x01);
        g_assert_cmphex(seen[1], ==, 0x00);
        g_assert_cmphex(seen[2], ==, 0x00);
        g_assert_cmphex(seen[3], ==, 0x00);
    }

    tigon_stop(&t);
}

static void test_byte_order_little_endian(void)
{
    byte_order_golden(false);
}

static void test_byte_order_big_endian(void)
{
    byte_order_golden(true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /*
     * Require Tigon in generic qemu-system-x86_64. A quiet skip would let a
     * Kconfig regression produce an empty green run.
     */
    g_assert_true(qtest_has_device("tigon"));

    qtest_add_func("tigon/config-identity", test_config_identity);
    qtest_add_func("tigon/bar0-shape", test_bar0_shape);
    qtest_add_func("tigon/chip-rev-and-reset", test_chip_rev_and_reset);
    qtest_add_func("tigon/register-config-roundtrip",
                   test_register_config_roundtrip);
    qtest_add_func("tigon/station-addr-par", test_station_addr_par);
    qtest_add_func("tigon/station-addr-eeprom", test_station_addr_eeprom);
    qtest_add_func("tigon/firmware-up-requires-gib",
                   test_firmware_up_requires_gib);
    qtest_add_func("tigon/firmware-up-posts-event",
                   test_firmware_up_posts_event);
    qtest_add_func("tigon/transmit", test_transmit);
    qtest_add_func("tigon/operational-ring-migration",
                   test_operational_ring_migration);
    qtest_add_func("tigon/receive", test_receive);
    qtest_add_func("tigon/receive-waits-for-arrival-deadline",
                   test_receive_waits_for_its_arrival_deadline);
    qtest_add_func("tigon/receive-iov-matches-flat",
                   test_receive_iov_matches_flat);
    qtest_add_func("tigon/receive-filters-other", test_receive_filters_other);
    qtest_add_func("tigon/receive-clamps-to-buffer",
                   test_receive_clamps_to_buffer);
    qtest_add_func("tigon/producer-index-safety", test_producer_index_safety);
    qtest_add_func("tigon/interrupt-mask-ack", test_interrupt_mask_ack);
    qtest_add_func("tigon/interrupt-hostintr-ack",
                   test_interrupt_hostintr_ack);
    qtest_add_func("tigon/byte-order-little-endian",
                   test_byte_order_little_endian);
    qtest_add_func("tigon/byte-order-big-endian", test_byte_order_big_endian);

    return g_test_run();
}
