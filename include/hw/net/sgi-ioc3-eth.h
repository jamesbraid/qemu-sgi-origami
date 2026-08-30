/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the ethernet MAC engine.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c), mapped into that
 * chip's PCI BAR at the ethernet block's offset. Its media access
 * controller is SGI's own design rather than a catalogue part, so the
 * register table in the IOC3 datasheet kept in the Origami project tree
 * (docs/datasheets/ioc3.md there, not in this QEMU tree) is the whole
 * specification and there is no vendor document behind it to consult.
 */

#ifndef HW_NET_SGI_IOC3_ETH_H
#define HW_NET_SGI_IOC3_ETH_H

#include "hw/core/sysbus.h"
#include "hw/net/dp83840.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_ETH "sgi-ioc3-eth"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3EthState, SGI_IOC3_ETH)

/*
 * Offsets are measured from the block's own base, which is where the parent
 * chip maps this device's MMIO region -- see SGI_IOC3_ENET_BASE in
 * hw/misc/sgi-ioc3.h for where that base sits in the chip's own window.
 */
#define SGI_IOC3_ENET_MAC_CONTROL   0x0f0
#define SGI_IOC3_ENET_INT_STATUS    0x0f4
#define SGI_IOC3_ENET_INT_ENABLE    0x0f8
#define SGI_IOC3_ENET_RX_CONTROL    0x0fc
#define SGI_IOC3_ENET_RX_RING_HI    0x100
#define SGI_IOC3_ENET_RX_RING_LO    0x104
#define SGI_IOC3_ENET_RX_BARRIER    0x108
#define SGI_IOC3_ENET_RX_CONSUME    0x10c
#define SGI_IOC3_ENET_RX_PRODUCE    0x110
#define SGI_IOC3_ENET_RX_TIMER      0x114
#define SGI_IOC3_ENET_TX_CONTROL    0x118
#define SGI_IOC3_ENET_RANDOM_SEED   0x11c
#define SGI_IOC3_ENET_COUNTERS      0x120
#define SGI_IOC3_ENET_BUFFER_INDEX  0x124
#define SGI_IOC3_ENET_TX_RING_HI    0x128
#define SGI_IOC3_ENET_TX_RING_LO    0x12c
#define SGI_IOC3_ENET_TX_CONSUME    0x130
#define SGI_IOC3_ENET_TX_PRODUCE    0x134
#define SGI_IOC3_ENET_MAC_ADDR_HI   0x138
#define SGI_IOC3_ENET_MAC_ADDR_LO   0x13c
#define SGI_IOC3_ENET_HASH_HI       0x140
#define SGI_IOC3_ENET_HASH_LO       0x144
#define SGI_IOC3_ENET_MII_CONTROL   0x148
#define SGI_IOC3_ENET_MII_DATA_R    0x14c
#define SGI_IOC3_ENET_MII_DATA_W    0x150

#define SGI_IOC3_ENET_BASE          SGI_IOC3_ENET_MAC_CONTROL
#define SGI_IOC3_ENET_SIZE          (SGI_IOC3_ENET_MII_DATA_W + 4 - \
                                     SGI_IOC3_ENET_BASE)
#define SGI_IOC3_ENET_REGS          (SGI_IOC3_ENET_SIZE / 4)

/*
 * MAC control bits. The offset field is buffer-start to frame-start in
 * sixteen-bit units -- SGI's own driver states the unit.
 */
#define SGI_IOC3_ENET_MCR_PROMISC   (1u << 1)
#define SGI_IOC3_ENET_MCR_PAD_EN    (1u << 2)
#define SGI_IOC3_ENET_MCR_RXOFF_MASK   0x1f8u
#define SGI_IOC3_ENET_MCR_RXOFF_SHIFT  3
#define SGI_IOC3_ENET_MCR_RAM_PARITY   (1u << 9)
#define SGI_IOC3_ENET_MCR_TX_DMA_EN (1u << 13)
#define SGI_IOC3_ENET_MCR_TX_EN     (1u << 14)
#define SGI_IOC3_ENET_MCR_RX_DMA_EN (1u << 15)
#define SGI_IOC3_ENET_MCR_RX_EN     (1u << 16)
#define SGI_IOC3_ENET_MCR_LOOPBACK  (1u << 17)
#define SGI_IOC3_ENET_MCR_ARB_DIAG_IDLE (1u << 21)
#define SGI_IOC3_ENET_MCR_RESET     (1u << 31)

/*
 * Interrupt sources, receive low, transmit high. The engine raises only a
 * few; the rest exist so a whole-word write-back is not refused.
 */
#define SGI_IOC3_ENET_INT_RX_TIMER      (1u << 0)
#define SGI_IOC3_ENET_INT_RX_THRESHOLD  (1u << 1)
#define SGI_IOC3_ENET_INT_RX_OVERFLOW   (1u << 2)
#define SGI_IOC3_ENET_INT_RX_BUF_OFLOW  (1u << 3)
#define SGI_IOC3_ENET_INT_RX_MEM_ERR    (1u << 4)
#define SGI_IOC3_ENET_INT_RX_PARITY     (1u << 5)
#define SGI_IOC3_ENET_INT_TX_EMPTY      (1u << 16)
#define SGI_IOC3_ENET_INT_TX_RETRY      (1u << 17)
#define SGI_IOC3_ENET_INT_TX_DEFER      (1u << 18)
#define SGI_IOC3_ENET_INT_TX_LATECOLL   (1u << 19)
#define SGI_IOC3_ENET_INT_TX_OVERSIZE   (1u << 20)
#define SGI_IOC3_ENET_INT_TX_UNDERFLOW  (1u << 21)
#define SGI_IOC3_ENET_INT_TX_COMPLETE   (1u << 22)
#define SGI_IOC3_ENET_INT_TX_COLL_WRAP  (1u << 23)
#define SGI_IOC3_ENET_INT_TX_DEFER_WRAP (1u << 24)
#define SGI_IOC3_ENET_INT_TX_MEM_ERR    (1u << 25)
#define SGI_IOC3_ENET_INT_TX_PARITY     (1u << 26)

/*
 * The receive ring: 512 eight-byte buffer addresses, indexed by byte
 * offset -- the indices use the same unit.
 */
#define SGI_IOC3_ENET_RX_RING_ENTRIES 512
#define SGI_IOC3_ENET_RX_RING_SHIFT   3
#define SGI_IOC3_ENET_RX_RING_SIZE    (SGI_IOC3_ENET_RX_RING_ENTRIES << \
                                       SGI_IOC3_ENET_RX_RING_SHIFT)
#define SGI_IOC3_ENET_RX_BASE_MASK    0xfffff000u
#define SGI_IOC3_ENET_RX_INDEX_MASK   (SGI_IOC3_ENET_RX_RING_SIZE - 1)

/*
 * The two delivery-notice sources. The threshold is a low watermark on
 * buffers still available, counted downward (Linux sets 15 of 64; OpenBSD
 * sets its whole count) -- a once-per-frame raise satisfies only OpenBSD.
 * The timer is the per-frame source, armed through bit 31 of the produce
 * index; arming is a command, not storage -- SGI's driver writes the index
 * without the bit and an armed timer must stay armed.
 */
#define SGI_IOC3_ENET_RCSR_THRESH_MASK 0x1ffu
#define SGI_IOC3_ENET_RTR_RELOAD_MASK  0x7ffu
#define SGI_IOC3_ENET_RPIR_ARM         (1u << 31)

/*
 * A receive buffer's two-word header, before the frame data; bit positions
 * from SGI's own header, agreeing with the datasheet's prose.
 */
#define SGI_IOC3_ENET_RXH_VALID       (1u << 31)
#define SGI_IOC3_ENET_RXH_BYTECNT_MASK   0x07ff0000u
#define SGI_IOC3_ENET_RXH_BYTECNT_SHIFT  16
#define SGI_IOC3_ENET_RXE_LEN_LO_SHIFT   12
#define SGI_IOC3_ENET_RXE_LEN_HI_SHIFT   16
#define SGI_IOC3_ENET_RXE_MULTICAST   (1u << 26)
#define SGI_IOC3_ENET_RXE_BROADCAST   (1u << 27)
#define SGI_IOC3_ENET_RXE_LONGEVENT   (1u << 28)
#define SGI_IOC3_ENET_RXE_BADPKT      (1u << 29)
#define SGI_IOC3_ENET_RXE_GOODPKT     (1u << 30)

/*
 * The transmit ring holds 128-byte descriptors inline; the base register's
 * low bit selects the count. The consume index carries an idle flag beside
 * its byte offset -- OpenBSD masks it off before dividing.
 */
#define SGI_IOC3_ENET_TX_DESC_SIZE       128
#define SGI_IOC3_ENET_TX_RING_SMALL      128
#define SGI_IOC3_ENET_TX_RING_LARGE      512
#define SGI_IOC3_ENET_TX_RING_SEL        (1u << 0)
#define SGI_IOC3_ENET_TX_BASE_MASK       0xffffc000u
#define SGI_IOC3_ENET_TX_CONSUME_IDLE    (1u << 31)
#define SGI_IOC3_ENET_TX_CONSUME_MASK    0xffffu

/*
 * A transmit descriptor's header words and buffer pointers, per the
 * datasheet.
 */
#define SGI_IOC3_ENET_TXD_BYTECNT_MASK   0x000007ffu
#define SGI_IOC3_ENET_TXD_INT_DONE       (1u << 12)
#define SGI_IOC3_ENET_TXD_DATA_VALID     (1u << 16)
#define SGI_IOC3_ENET_TXD_BUF1_VALID     (1u << 17)
#define SGI_IOC3_ENET_TXD_BUF2_VALID     (1u << 18)
/* Insert the transport checksum into the assembled frame. */
#define SGI_IOC3_ENET_TXD_DOCHECKSUM     (1u << 19)
#define SGI_IOC3_ENET_TXD_DATA_LEN_MASK  0x0000007fu
#define SGI_IOC3_ENET_TXD_BUF1_LEN_MASK  0x0007ff00u
#define SGI_IOC3_ENET_TXD_BUF1_LEN_SHIFT 8
#define SGI_IOC3_ENET_TXD_BUF2_LEN_MASK  0x7ff00000u
#define SGI_IOC3_ENET_TXD_BUF2_LEN_SHIFT 20
/*
 * The inline data area: the last 104 bytes of the 128, both drivers
 * agreeing independently. The seven-bit length field can name up to 127
 * bytes for an area that holds 104; the engine takes the smaller.
 */
#define SGI_IOC3_ENET_TXD_DATA_OFFSET    24
#define SGI_IOC3_ENET_TXD_SIZE           SGI_IOC3_ENET_TX_DESC_SIZE
#define SGI_IOC3_ENET_TXD_DATA_SIZE      (SGI_IOC3_ENET_TXD_SIZE - \
                                          SGI_IOC3_ENET_TXD_DATA_OFFSET)

/* The eleven-bit byte-count field's own limit on an assembled frame. */
#define SGI_IOC3_ENET_FRAME_MAX          2048

/*
 * MII control: bit 10 starts a read, bit 11 is busy -- never set here,
 * since a read completes before the guest can look.
 */
#define SGI_IOC3_ENET_MII_READ      (1u << 10)
#define SGI_IOC3_ENET_MII_BUSY      (1u << 11)

/*
 * An address with no transceiver reads all-ones, or a driver's scan of the
 * thirty-two addresses stops at the wrong one.
 */
#define SGI_IOC3_ENET_MII_ABSENT    0xffffu

/* The request layout: register number low, transceiver address above. */
#define SGI_IOC3_ENET_MII_REG_MASK  0x1fu
#define SGI_IOC3_ENET_MII_PHY_SHIFT 5
#define SGI_IOC3_ENET_MII_PHY_MASK  0x1fu

struct SGIIOC3EthState {
    SysBusDevice parent_obj;

    /*
     * The register window inside the parent's BAR, and the level line that
     * drives the chip's ordinary PCI interrupt pin through the parent.
     */
    MemoryRegion mmio;
    qemu_irq irq;

    /*
     * The DMA view, a "dma-memory" link set before realize: the parent
     * hands down its PCI function's bus-master view so every transfer goes
     * through the Bridge's translation.
     */
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    /*
     * The registers as one array: most are plain storage, and the few that
     * drive the engine are reached by named offsets into it.
     */
    uint32_t enet_reg[SGI_IOC3_ENET_REGS];

    /*
     * The backend, and the scratch buffer a descriptor's pieces are
     * assembled into for one qemu_send_packet call; receive writes straight
     * into the guest's buffer.
     */
    NICConf conf;
    /*
     * Set with the mac property when the address came from the default
     * generator -- the one case realize may replace with SGI's prefix.
     */
    bool mac_generated;
    NICState *nic;
    uint8_t frame[SGI_IOC3_ENET_FRAME_MAX];

    /*
     * The transceiver: a child because the management registers are the
     * only way to it.
     */
    DP83840State phy;

    /*
     * A management transfer's two halves -- the fetched read word and the
     * loaded write word -- separate registers on the chip.
     */
    uint16_t mii_data;
    uint16_t mii_data_w;

    /*
     * Receive-timer arming, apart from the index it is written through: a
     * later index-only write must not disarm what an earlier write armed.
     */
    bool rx_timer_armed;
};

bool sgi_ioc3_eth_ram_parity_enabled(SGIIOC3EthState *s);

#endif
