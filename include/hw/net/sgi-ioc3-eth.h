/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the ethernet MAC engine.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c), mapped into that
 * chip's PCI BAR at the ethernet block's offset. Its media access
 * controller is SGI's own design rather than a catalogue part, so
 * docs/datasheets/ioc3.md's register table is the whole specification and
 * there is no vendor document behind it to consult.
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
 * MAC control register bits. The offset field holds the distance from a
 * receive buffer's start to where the frame begins, in sixteen-bit units --
 * SGI's own driver comments the field this way when it programs it, which is
 * where the halving comes from; the datasheet's byte-offset description and
 * this scale agree once that unit is known.
 */
#define SGI_IOC3_ENET_MCR_DUPLEX    (1u << 0)
#define SGI_IOC3_ENET_MCR_PROMISC   (1u << 1)
#define SGI_IOC3_ENET_MCR_PAD_EN    (1u << 2)
#define SGI_IOC3_ENET_MCR_RXOFF_MASK   0x1f8u
#define SGI_IOC3_ENET_MCR_RXOFF_SHIFT  3
#define SGI_IOC3_ENET_MCR_RAM_PARITY   (1u << 9)
#define SGI_IOC3_ENET_MCR_BAD_PARITY   (1u << 11)
#define SGI_IOC3_ENET_MCR_BUFSIZ    (1u << 12)
#define SGI_IOC3_ENET_MCR_TX_DMA_EN (1u << 13)
#define SGI_IOC3_ENET_MCR_TX_EN     (1u << 14)
#define SGI_IOC3_ENET_MCR_RX_DMA_EN (1u << 15)
#define SGI_IOC3_ENET_MCR_RX_EN     (1u << 16)
#define SGI_IOC3_ENET_MCR_LOOPBACK  (1u << 17)
#define SGI_IOC3_ENET_MCR_RESET     (1u << 31)

/*
 * Interrupt sources, receive in the low half and transmit in the high half.
 * Only the two this engine actually raises are named; the rest exist so a
 * guest's write-back of the whole status word does not trip the SuperIO
 * register's read-only complaint transplanted onto this one, and so a bit
 * position is available if a later pass models the condition it reports.
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
 * The receive ring: a fixed 512-entry array of eight-byte buffer addresses,
 * indexed by a byte offset rather than an entry number -- the entry number is
 * the offset shifted right by three, which is the same shift the offset's own
 * producer and consumer indices use.
 */
#define SGI_IOC3_ENET_RX_RING_ENTRIES 512
#define SGI_IOC3_ENET_RX_RING_SHIFT   3
#define SGI_IOC3_ENET_RX_RING_SIZE    (SGI_IOC3_ENET_RX_RING_ENTRIES << \
                                       SGI_IOC3_ENET_RX_RING_SHIFT)
#define SGI_IOC3_ENET_RX_BASE_MASK    0xfffff000u
#define SGI_IOC3_ENET_RX_INDEX_MASK   (SGI_IOC3_ENET_RX_RING_SIZE - 1)

/*
 * What prompts a driver when a frame is delivered, which is two separate
 * sources rather than one.
 *
 * The threshold in the receive control register is a low watermark on the
 * buffers still available to the engine, counted downward. Linux's own
 * comment calls it that, and it sets 15 against the 64 buffers it supplies,
 * asking to hear when its supply runs low. OpenBSD sets its whole available
 * count, which puts the watermark where the very first delivered frame
 * crosses it. One register, two intentions -- so raising this source once
 * per frame regardless of what is programmed here satisfies OpenBSD by
 * accident and misreports for every other watermark.
 *
 * The receive timer is the per-frame source. It is armed through bit 31 of
 * the produce index and loaded with an idle timeout, and both drivers load
 * zero and say what they mean by it themselves -- OpenBSD's comment is "Set
 * up RX timer to interrupt immediately upon reception", Linux's is
 * "Interrupt immediately". Linux enables no other receive source at all and
 * dispatches its receive handler on this one alone.
 *
 * Arming is a command rather than storage: SGI's own driver writes the
 * produce index without bit 31 from its interrupt path, so a write lacking
 * the bit has to leave an armed timer armed.
 */
#define SGI_IOC3_ENET_RCSR_THRESH_MASK 0x1ffu
#define SGI_IOC3_ENET_RTR_RELOAD_MASK  0x7ffu
#define SGI_IOC3_ENET_RPIR_ARM         (1u << 31)

/*
 * A receive buffer's own two-word header, immediately before the frame data
 * the offset field above places. Bit positions come from SGI's own header,
 * cross-checked against the datasheet's prose account of the same fields --
 * the two agree, and the split length field is why the datasheet describes
 * the frame length as arriving "in two pieces".
 */
#define SGI_IOC3_ENET_RXH_VALID       (1u << 31)
#define SGI_IOC3_ENET_RXH_BYTECNT_MASK   0x07ff0000u
#define SGI_IOC3_ENET_RXH_BYTECNT_SHIFT  16
#define SGI_IOC3_ENET_RXE_CRC         (1u << 0)
#define SGI_IOC3_ENET_RXE_FRAMING     (1u << 1)
#define SGI_IOC3_ENET_RXE_CODE        (1u << 2)
#define SGI_IOC3_ENET_RXE_PREAMBLE    (1u << 3)
#define SGI_IOC3_ENET_RXE_LEN_LO_MASK    0x00007000u
#define SGI_IOC3_ENET_RXE_LEN_LO_SHIFT   12
#define SGI_IOC3_ENET_RXE_LEN_HI_MASK    0x03ff0000u
#define SGI_IOC3_ENET_RXE_LEN_HI_SHIFT   16
#define SGI_IOC3_ENET_RXE_MULTICAST   (1u << 26)
#define SGI_IOC3_ENET_RXE_BROADCAST   (1u << 27)
#define SGI_IOC3_ENET_RXE_LONGEVENT   (1u << 28)
#define SGI_IOC3_ENET_RXE_BADPKT      (1u << 29)
#define SGI_IOC3_ENET_RXE_GOODPKT     (1u << 30)
#define SGI_IOC3_ENET_RXE_CARRIER     (1u << 31)

/*
 * The transmit ring holds descriptors inline rather than pointers, 128 bytes
 * each; the low bit of the base register picks how many of them there are.
 * The consume index carries an idle flag in the same word as its byte offset
 * -- OpenBSD's driver masks that bit off before dividing by descriptor size,
 * which is the tell that it is not part of the count.
 */
#define SGI_IOC3_ENET_TX_DESC_SIZE       128
#define SGI_IOC3_ENET_TX_DESC_SHIFT      7
#define SGI_IOC3_ENET_TX_RING_SMALL      128
#define SGI_IOC3_ENET_TX_RING_LARGE      512
#define SGI_IOC3_ENET_TX_RING_SEL        (1u << 0)
#define SGI_IOC3_ENET_TX_BASE_MASK       0xffffc000u
#define SGI_IOC3_ENET_TX_CONSUME_IDLE    (1u << 31)
#define SGI_IOC3_ENET_TX_CONSUME_MASK    0xffffu

/* A transmit descriptor's two header words and its two out-of-line buffer
 * pointers. Field positions are the datasheet's own, given here as named
 * bits rather than repeated as bare numbers in the engine that uses them.
 */
#define SGI_IOC3_ENET_TXD_BYTECNT_MASK   0x000007ffu
#define SGI_IOC3_ENET_TXD_INT_DONE       (1u << 12)
#define SGI_IOC3_ENET_TXD_DATA_VALID     (1u << 16)
#define SGI_IOC3_ENET_TXD_BUF1_VALID     (1u << 17)
#define SGI_IOC3_ENET_TXD_BUF2_VALID     (1u << 18)
#define SGI_IOC3_ENET_TXD_DATA_LEN_MASK  0x0000007fu
#define SGI_IOC3_ENET_TXD_BUF1_LEN_MASK  0x0007ff00u
#define SGI_IOC3_ENET_TXD_BUF1_LEN_SHIFT 8
#define SGI_IOC3_ENET_TXD_BUF2_LEN_MASK  0x7ff00000u
#define SGI_IOC3_ENET_TXD_BUF2_LEN_SHIFT 20
/*
 * Where the descriptor's own inline data area starts, and how much of the
 * descriptor is left for it: two four-byte header words and two eight-byte
 * buffer pointers come first, so the area is the last 104 bytes of the 128.
 * Both drivers state the same two numbers independently -- OpenBSD computes
 * the offset as two words plus two pointers and the size as what remains,
 * and Linux's own transmit descriptor declares its inline array 104 bytes
 * long -- which is what makes them the descriptor's layout rather than one
 * driver's convention.
 *
 * The length field above holds seven bits, so a guest can name up to 127
 * inline bytes in an area that holds 104. The engine takes the smaller.
 */
#define SGI_IOC3_ENET_TXD_DATA_OFFSET    24
#define SGI_IOC3_ENET_TXD_SIZE           SGI_IOC3_ENET_TX_DESC_SIZE
#define SGI_IOC3_ENET_TXD_DATA_SIZE      (SGI_IOC3_ENET_TXD_SIZE - \
                                          SGI_IOC3_ENET_TXD_DATA_OFFSET)

/* The largest frame this engine assembles before handing it to the network
 * backend: the eleven-bit byte count field's own limit, which no descriptor
 * can exceed regardless of how its buffers are split.
 */
#define SGI_IOC3_ENET_FRAME_MAX          2048

/* Bit 10 of the media-independent interface control register starts a read,
 * and bit 11 reads back as busy while one is in flight. Nothing here takes
 * time, so a read is complete by the time the guest can look and busy never
 * reads set -- a driver that polls it therefore stops on its first look
 * rather than running its timeout out.
 */
#define SGI_IOC3_ENET_MII_READ      (1u << 10)
#define SGI_IOC3_ENET_MII_BUSY      (1u << 11)

/*
 * An address with no transceiver on it reads as all-ones, which is what a
 * driver scanning the thirty-two addresses takes as "nothing here". The board
 * has exactly one transceiver and every other address must answer this way, or
 * the scan stops at the wrong one.
 */
#define SGI_IOC3_ENET_MII_ABSENT    0xffffu

/*
 * How the management control register carries a request: the register number
 * in the low five bits and the transceiver address above it. A read is asked
 * for explicitly; a write is what the register means without that bit.
 */
#define SGI_IOC3_ENET_MII_REG_MASK  0x1fu
#define SGI_IOC3_ENET_MII_PHY_SHIFT 5
#define SGI_IOC3_ENET_MII_PHY_MASK  0x1fu

struct SGIIOC3EthState {
    SysBusDevice parent_obj;

    /* The register window a guest reaches through the parent chip's BAR,
     * and the level line this block's own interrupt pair drives -- the
     * chip's ordinary PCI interrupt pin, not a wire of its own, so the
     * parent connects this output to a handler that raises that pin
     * rather than to any GPIO a guest could see.
     */
    MemoryRegion mmio;
    qemu_irq irq;

    /*
     * The parent PCI function's own DMA address space, handed down once at
     * realize. This block sits behind the same Bridge the parent's BAR
     * does, and every DMA master in the chip must go through that
     * translation -- see docs/datasheets/ioc3.md's "DMA" section.
     */
    AddressSpace *dma_as;

    /*
     * The register block, held as one array indexed by register rather than
     * as two dozen named fields. Most registers in it are plain storage a
     * driver writes and reads back; the ring bases, indices, MAC control and
     * interrupt pair are also stored here but additionally drive the engine
     * below through named accesses at their offsets, so naming the rest
     * individually would add bulk without adding meaning.
     */
    uint32_t enet_reg[SGI_IOC3_ENET_REGS];

    /*
     * The network backend and the scratch buffer a transmit descriptor's
     * buffers are assembled into before handing the whole frame to
     * qemu_send_packet in one call. Receiving needs no equivalent buffer: a
     * received frame is written straight into the guest-supplied buffer the
     * receive ring names, since unlike transmit there is only ever one
     * destination for the bytes.
     */
    NICConf conf;
    NICState *nic;
    uint8_t frame[SGI_IOC3_ENET_FRAME_MAX];

    /*
     * The transceiver on the far end of the management registers. It is a
     * child of this device because that is the only way to it: no address of
     * its own, reached by a guest writing an address and a register number
     * here.
     */
    DP83840State phy;

    /*
     * The two halves of a management transfer. The word a read request
     * fetched, held until the guest reads the data register, and the word a
     * guest loaded before issuing a write. They are separate registers on the
     * chip and separate here, because a driver loads one and reads the other
     * without the two ever meeting.
     */
    uint16_t mii_data;
    uint16_t mii_data_w;

    /*
     * Whether the receive timer has been armed. Held apart from the produce
     * index it is written through, because the bit is a command and the
     * index is storage: a later write of the index alone must not disarm
     * what an earlier write armed.
     */
    bool rx_timer_armed;
};

#endif
