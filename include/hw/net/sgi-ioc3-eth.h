/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI IOC3 Ethernet MAC register and device definitions. */
/*
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The MAC is SGI's
 * own design with no vendor document; the IOC3 register behavior is
 * documented in docs/specs/sgi-ioc3.rst.
 */

#ifndef HW_NET_SGI_IOC3_ETH_H
#define HW_NET_SGI_IOC3_ETH_H

#include "hw/core/sysbus.h"
#include "hw/net/dp83840.h"
#include "hw/net/net-rx-latency.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_ETH "sgi-ioc3-eth"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3EthState, SGI_IOC3_ETH)

/* Offsets from the block's own base inside the parent's window. */
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

/* MAC control; the offset field counts sixteen-bit units. */
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

/* Interrupt sources, receive low, transmit high. */
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

/* The receive ring: 512 buffer addresses, indexed by byte offset. */
#define SGI_IOC3_ENET_RX_RING_ENTRIES 512
#define SGI_IOC3_ENET_RX_RING_SHIFT   3
#define SGI_IOC3_ENET_RX_RING_SIZE    (SGI_IOC3_ENET_RX_RING_ENTRIES << \
                                       SGI_IOC3_ENET_RX_RING_SHIFT)
#define SGI_IOC3_ENET_RX_BASE_MASK    0xfffff000u
#define SGI_IOC3_ENET_RX_INDEX_MASK   (SGI_IOC3_ENET_RX_RING_SIZE - 1)

/*
 * The threshold is a low watermark on available buffers; the timer is the
 * per-frame source, and its arm bit is a command, not storage.
 */
#define SGI_IOC3_ENET_RCSR_THRESH_MASK 0x1ffu
#define SGI_IOC3_ENET_RTR_RELOAD_MASK  0x7ffu
#define SGI_IOC3_ENET_RPIR_ARM         (1u << 31)

/* A receive buffer's two-word header, before the frame data. */
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

/* Inline 128-byte descriptors; the base's low bit selects the count. */
#define SGI_IOC3_ENET_TX_DESC_SIZE       128
#define SGI_IOC3_ENET_TX_RING_SMALL      128
#define SGI_IOC3_ENET_TX_RING_LARGE      512
#define SGI_IOC3_ENET_TX_RING_SEL        (1u << 0)
#define SGI_IOC3_ENET_TX_BASE_MASK       0xffffc000u
#define SGI_IOC3_ENET_TX_CONSUME_IDLE    (1u << 31)
#define SGI_IOC3_ENET_TX_CONSUME_MASK    0xffffu

#define SGI_IOC3_ENET_TXD_BYTECNT_MASK   0x000007ffu
#define SGI_IOC3_ENET_TXD_INT_DONE       (1u << 12)
#define SGI_IOC3_ENET_TXD_DATA_VALID     (1u << 16)
#define SGI_IOC3_ENET_TXD_BUF1_VALID     (1u << 17)
#define SGI_IOC3_ENET_TXD_BUF2_VALID     (1u << 18)
#define SGI_IOC3_ENET_TXD_DOCHECKSUM     (1u << 19)
#define SGI_IOC3_ENET_TXD_DATA_LEN_MASK  0x0000007fu
#define SGI_IOC3_ENET_TXD_BUF1_LEN_MASK  0x0007ff00u
#define SGI_IOC3_ENET_TXD_BUF1_LEN_SHIFT 8
#define SGI_IOC3_ENET_TXD_BUF2_LEN_MASK  0x7ff00000u
#define SGI_IOC3_ENET_TXD_BUF2_LEN_SHIFT 20
/* The inline area holds 104 bytes; its length field can name 127. */
#define SGI_IOC3_ENET_TXD_DATA_OFFSET    24
#define SGI_IOC3_ENET_TXD_SIZE           SGI_IOC3_ENET_TX_DESC_SIZE
#define SGI_IOC3_ENET_TXD_DATA_SIZE      (SGI_IOC3_ENET_TXD_SIZE - \
                                          SGI_IOC3_ENET_TXD_DATA_OFFSET)

/* The eleven-bit byte-count field's limit. */
#define SGI_IOC3_ENET_FRAME_MAX          2048

#define SGI_IOC3_ENET_MII_READ      (1u << 10)
#define SGI_IOC3_ENET_MII_BUSY      (1u << 11)

/* All-ones, or a driver's scan stops at the wrong address. */
#define SGI_IOC3_ENET_MII_ABSENT    0xffffu

#define SGI_IOC3_ENET_MII_REG_MASK  0x1fu
#define SGI_IOC3_ENET_MII_PHY_SHIFT 5
#define SGI_IOC3_ENET_MII_PHY_MASK  0x1fu

struct SGIIOC3EthState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    bool irq_asserted;

    /* The parent's bus-master view, linked before realize. */
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    uint32_t enet_reg[SGI_IOC3_ENET_REGS];
    /*
     * Sources raised while the same INT_STATUS bit was already set and
     * unacknowledged: the guest's next write-one-to-clear of that bit must
     * turn into a real edge for this one too rather than silently clearing
     * both (see sgi_ioc3_eth_raise_int()).
     */
    uint32_t int_deferred;

    NICConf conf;
    /* Set with mac when the address came from the default generator. */
    bool mac_generated;
    NICState *nic;
    QEMUBH *rx_flush_bh;
    uint8_t frame[SGI_IOC3_ENET_FRAME_MAX];

    /*
     * Backend replies wait until the transmit walk has published its state.
     */
    bool defer_backend_rx;
    /* can_receive() refused ingress while backend replies were deferred. */
    bool backend_rx_deferred;

    DP83840State phy;

    uint16_t mii_data;
    uint16_t mii_data_w;

    /* Apart from its index: an index-only write must not disarm. */
    bool rx_timer_armed;

    /*
     * Where the engine will write next, which runs ahead of the RX_CONSUME
     * register whenever the delay line is holding a completion: the frame is
     * already in the guest's buffer, but the index that tells the guest so is
     * published only once the arrival deadline passes.
     */
    uint32_t rx_consume_staged;

    /* The modelled minimum between a frame arriving and its completion. */
    uint32_t rx_arrival_latency_us;
    SGINetRxLatency rx_latency;
};

/* One receive whose buffer is written and whose completion is still held. */
typedef struct SGIIOC3EthRxDone {
    uint64_t buf_addr;
    uint32_t word0;
    uint32_t consume;
} SGIIOC3EthRxDone;

bool sgi_ioc3_eth_ram_parity_enabled(SGIIOC3EthState *s);

#endif
