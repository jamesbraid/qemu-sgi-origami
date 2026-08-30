/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Broadcom NetXtreme BCM570x ("Tigon3") Gigabit Ethernet controller.
 *
 * A generic PCI network controller that OpenBSD's bge, Linux's tg3 and IRIX's
 * tg all drive as a conventional descriptor-ring Gigabit MAC with an integrated
 * copper PHY. The SGI IRIS Release 2 Gigabit board is one instantiation of this
 * chip, distinguished only by its PCI identity (a Broadcom primary vendor/device
 * plus an SGI subsystem identity), port count and media -- all QOM properties,
 * not bespoke code. The single-port BCM5701 and the dual-port dual-MAC
 * BCM5704/5704S are `model` property variants of this one type.
 *
 * Modelled so far: the PCI device and its configurable four-tuple; the 64 KiB
 * BAR0 (a register file at 0x0000-0x7FFF and a 32 KiB on-chip SRAM window at
 * 0x8000-0xFFFF); the three ways the driver reaches chip state -- the BAR
 * directly, an indirect register window through PCI config 0x78/0x80, and an
 * indirect memory window through config 0x7C/0x84; the chip id in MISC_CTL; the
 * global reset with its on-chip firmware-ready handshake at SRAM 0x0B50; the
 * self-clearing *_MODE reset bits; the MII path to the integrated copper PHY
 * (a separate model) through the MI registers; the MAC-address mailbox in SRAM;
 * link status tied to the netdev; the transmit ring -- the send-ring RCB, the
 * producer mailbox, the descriptor/data DMA through the Bridge, and the status
 * block DMA'd to the host, with the DMA byte order settled here; and the receive
 * ring -- posted standard-ring buffers, the frame DMA'd in, a completed
 * descriptor written to the return ring, and the status block's receive
 * producer advanced; and the INTA interrupt -- the 0x0204 mailbox
 * mask/acknowledge protocol, the MISC_CTL global mask, and
 * PCISTATE.INTR_NOT_ACTIVE, driven as a level through the Bridge when a
 * status-block update is posted with interrupts unmasked; and the dual-port
 * BCM5704 -- one multifunction PCI device whose function 0 spawns an identical
 * MAC at function 1 of the same slot, each function an independent instance of
 * this same code. A write to a register with no modelled behaviour logs
 * LOG_UNIMP and is stored so a read-back still round-trips.
 *
 * Written from docs/datasheets/bcm570x.md (the reviewed register/DMA/interrupt
 * contract) and docs/library/nic-on-bridge-conventions.md (the shared byte-swap
 * and architecture convention), plus the public OpenBSD bge and in-tree QEMU
 * device sources; no IRIX vendor source was read.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/bcm570x_phy.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_BCM570X "bcm570x"
typedef struct Bcm570xState Bcm570xState;
DECLARE_INSTANCE_CHECKER(Bcm570xState, BCM570X, TYPE_BCM570X)

/* Default four-tuple: the single-port SGI IRIS Release 2 board (bcm5701). */
#define BCM570X_VENDOR_ID           0x14e4  /* Broadcom */
#define BCM570X_DEVICE_ID           0x1645  /* BCM5701 */
#define BCM570X_SUBSYS_VENDOR_ID    0x10a9  /* SGI */
#define BCM570X_SUBSYS_ID           0x8010

/*
 * The dual-port BCM5704 copper four-tuple, the model=bcm5704 default. The
 * SerDes variant is device 0x16a8 / subsystem 0x8014, selected by overriding
 * device-id and subsystem-id explicitly.
 */
#define BCM570X_DEVICE_ID_BCM5704   0x1648
#define BCM570X_SUBSYS_ID_BCM5704   0x8013

/*
 * BAR0 is 64 KiB: a register file at 0x0000-0x7FFF and a 32 KiB window into
 * on-chip SRAM at 0x8000-0xFFFF whose base within SRAM is set through config
 * 0x7C. Both are 32-bit-accessed. The register file and the SRAM are each
 * 32 KiB.
 */
#define BCM570X_REG_FILE_SIZE       0x8000u
#define BCM570X_SRAM_SIZE           0x8000u
#define BCM570X_BAR0_SIZE           0x10000u
#define BCM570X_REG_FILE_WORDS      (BCM570X_REG_FILE_SIZE / 4)
#define BCM570X_SRAM_WORDS          (BCM570X_SRAM_SIZE / 4)

/*
 * The indirect-access config registers (docs/datasheets/bcm570x.md, "three
 * access paths"). The base register selects an offset; a read or write of the
 * data register then reaches that offset in the register file (0x78/0x80) or in
 * on-chip SRAM (0x7C/0x84). The memory-window base, masked to a 32 KiB
 * boundary, also positions the BAR0 window at 0x8000.
 */
#define BCM570X_PCI_REG_BASEADDR    0x78
#define BCM570X_PCI_MEMWIN_BASEADDR 0x7c
#define BCM570X_PCI_REG_DATA        0x80
#define BCM570X_PCI_MEMWIN_DATA     0x84

/*
 * Config 0x68 (misc host control). Bits 31:16 read the ASIC revision -- the
 * driver takes its whole chip id from this field (bge_chipid = MISC_CTL >> 16,
 * ASIC revision = chipid >> 12), so it must decode to the presented part. The
 * low 16 control bits are writable and behaviourless for now.
 */
#define BCM570X_PCI_MISC_CTL        0x68
#define BCM570X_CHIPID_BCM5701      0x0105  /* B5 stepping; ASIC revision 0x0 */
#define BCM570X_CHIPID_BCM5704      0x2003  /* A3 stepping; ASIC revision 0x2 */

/*
 * The two MISC_CTL low bits that gate the interrupt. CLEAR_INTA is a
 * write-1-to-acknowledge that self-clears; MASK_PCI_INTR is a global mask the
 * driver holds set through reset and clears in bge_blockinit to let the line
 * assert. Both sit in the writable low half of MISC_CTL.
 */
#define BCM570X_MISCCTL_CLEAR_INTA      0x00000001u
#define BCM570X_MISCCTL_MASK_PCI_INTR   0x00000002u

/*
 * Config 0x70 (PCISTATE) reports the bus and the interrupt line. The chip runs
 * in conventional PCI behind the Bridge, so PCI_BUSMODE reads 1; the driver
 * branches its DMA-watermark setup on that and takes the plain-PCI arm.
 * INTR_NOT_ACTIVE follows the line -- set while the chip is not asserting INTA,
 * which is the driver's shared-interrupt spurious check (bge_intr). The chip
 * drives INTR_NOT_ACTIVE, so the register is presented read-only to the guest.
 */
#define BCM570X_PCI_PCISTATE            0x70
#define BCM570X_PCISTATE_INTR_NOT_ACTIVE 0x00000002u
#define BCM570X_PCISTATE_BUSMODE_PCI     0x00000004u

/* Register-file offsets with modelled behaviour. */
#define BCM570X_MAC_MODE            0x0400
#define BCM570X_HCC_MODE            0x3c00
#define BCM570X_MISC_CFG            0x6804

/*
 * Bit 0 of a *_MODE register is a self-clearing reset the driver sets and then
 * polls for clear. Bit 0 of MISC_CFG (RESET_CORE_CLOCKS) triggers the chip's
 * global reset instead.
 */
#define BCM570X_MODE_RESET          0x00000001u
#define BCM570X_MISCCFG_RESET_CORE_CLOCKS 0x00000001u

/*
 * The firmware-ready handshake. The driver writes the magic to on-chip SRAM
 * 0x0B50 before a global reset and polls there afterwards for its complement,
 * which is how the on-chip bootcode signals initialisation complete.
 */
#define BCM570X_SRAM_FW_MBOX        0x0b50
#define BCM570X_FW_MAGIC            0x4b657654u
#define BCM570X_FW_HANDSHAKE_DONE   (~BCM570X_FW_MAGIC)     /* 0xB49A89AB */

/*
 * The MII management registers, the only path to the integrated PHY. MI_COMM
 * carries a command: a READ or WRITE, the PHY address, the register number and
 * (for a write) the data; the chip clears BUSY when done and, for a read,
 * leaves the value in the low 16 bits. MI_STS is the auto-poll link latch and
 * MI_MODE (plain storage in the register file) carries the AUTOPOLL enable.
 */
#define BCM570X_MI_COMM             0x044c
#define BCM570X_MI_STS              0x0450
#define BCM570X_MICOMM_DATA         0x0000ffffu
#define BCM570X_MICOMM_REG          0x001f0000u     /* bits 20:16 */
#define BCM570X_MICOMM_REG_SHIFT    16
#define BCM570X_MICMD_WRITE         0x04000000u
#define BCM570X_MICMD_READ          0x08000000u
#define BCM570X_MICOMM_BUSY         0x20000000u
#define BCM570X_MISTS_LINK          0x00000001u

/*
 * The MAC-address mailbox in on-chip SRAM. When the high half of 0x0C14 reads
 * the tag 0x484B the driver takes the six address bytes from these two words,
 * which is the path bge_attach reads before it falls back to the EEPROM. The
 * on-chip bootcode leaves them there, so a reset re-populates them.
 */
#define BCM570X_SRAM_MAC_HI         0x0c14
#define BCM570X_SRAM_MAC_LO         0x0c18
#define BCM570X_MAC_TAG             0x484b

/*
 * The transmit ring. The driver writes the send-ring control block to on-chip
 * SRAM 0x0100 -- the ring's host-memory base as a 64-bit address, then a
 * max-length/flags word whose top half is the ring length in descriptors --
 * and rings the doorbell by writing the new producer descriptor index to the
 * mailbox at 0x0304. Each descriptor is 16 bytes: a 64-bit host buffer address,
 * a 16-bit length, and 16-bit flags whose END bit marks the last descriptor of
 * a frame.
 */
#define BCM570X_MBX_TX_PROD_LO      0x0304
#define BCM570X_SRAM_SEND_RING_RCB  0x0100
#define BCM570X_RCB_HOSTADDR_HI     0x00    /* offsets within the RCB */
#define BCM570X_RCB_HOSTADDR_LO     0x04
#define BCM570X_RCB_MAXLEN_FLAGS    0x08
#define BCM570X_TXBD_SIZE           16
#define BCM570X_TXBD_ADDR_HI        0x00    /* offsets within a tx descriptor */
#define BCM570X_TXBD_ADDR_LO        0x04
#define BCM570X_TXBD_LEN            0x08
#define BCM570X_TXBD_FLAGS          0x0a
#define BCM570X_TXBDFLAG_END        0x0004

/*
 * The host coalescing block DMAs the status block to the host address the
 * driver programs, only while it is enabled. The status block reports transmit
 * and receive progress the driver reads instead of a register: the transmit
 * consumer index sits at offset 16 and the receive producer index at 18, both
 * in the first of the sixteen index pairs.
 */
#define BCM570X_HCC_MODE_ENABLE     0x00000002u
#define BCM570X_HCC_STATUSBLK_ADDR_HI 0x3c38
#define BCM570X_HCC_STATUSBLK_ADDR_LO 0x3c3c
#define BCM570X_STATUS_BLK_SIZE     0x50
#define BCM570X_STATFLAG_UPDATED    0x00000001u
#define BCM570X_STATUS_TX_CONS_OFF  16
#define BCM570X_STATUS_RX_PROD_OFF  18

/*
 * The interrupt mailbox. Writing 1 masks the line and acknowledges the pending
 * status-block update; writing 0 re-enables it. The driver writes 1 on entry to
 * its handler and 0 on exit (bge_intr), and writes 0 once in bge_blockinit to
 * first enable host interrupts. Only the low 32 bits of the 64-bit mailbox are
 * used, and the chip acts on a write to the low half.
 */
#define BCM570X_MBX_IRQ0_LO         0x0204

/* The largest frame the transmit engine assembles across descriptors. */
#define BCM570X_MAX_FRAME           2048

/*
 * The receive rings. The host posts buffers into a standard ring in host
 * memory -- its base in the standard-ring control block CSRs -- and bumps the
 * producer mailbox 0x026C. The chip DMAs an incoming frame into the next posted
 * buffer, writes a completed descriptor into a return ring in host memory
 * (control block in SRAM 0x0200), and advances the receive producer index in
 * the status block; the driver acknowledges by writing the return-ring consumer
 * to 0x0284. A receive descriptor is 32 bytes: a 64-bit buffer address, then the
 * buffer index, the frame length, and flags whose END bit marks the frame end
 * and whose ERROR bit (with sub-flags in a separate word) marks a bad frame.
 */
#define BCM570X_MBX_RX_STD_PROD_LO  0x026c
#define BCM570X_MBX_RX_CONS0_LO     0x0284
#define BCM570X_RX_STD_RCB_HADDR_HI 0x2450
#define BCM570X_RX_STD_RCB_HADDR_LO 0x2454
#define BCM570X_RX_STD_RCB_MAXLEN_FLAGS 0x2458
#define BCM570X_SRAM_RX_RETURN_RCB  0x0200
#define BCM570X_RXBD_SIZE           32
#define BCM570X_RXBD_ADDR_HI        0x00    /* offsets within an rx descriptor */
#define BCM570X_RXBD_ADDR_LO        0x04
#define BCM570X_RXBD_IDX            0x08
#define BCM570X_RXBD_LEN            0x0a
#define BCM570X_RXBD_FLAGS          0x0e
#define BCM570X_RXBDFLAG_END        0x0004

struct Bcm570xState {
    PCIDevice parent_obj;

    MemoryRegion bar0;

    NICState *nic;
    NICConf conf;

    /* PCI identity, defaulting to the SGI board; overridable per instance. */
    char *model;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;

    /* The two indirect-window base registers (config 0x78 / 0x7C). */
    uint32_t reg_win_base;
    uint32_t mem_win_base;

    /* Backing storage for the register file and the on-chip SRAM. */
    uint32_t regfile[BCM570X_REG_FILE_WORDS];
    uint32_t sram[BCM570X_SRAM_WORDS];

    /* Transmit-ring engine and the status block it reports through. */
    uint16_t tx_cons;           /* the chip's send-ring consumer */
    uint16_t status_tx_cons;    /* what the status block reports (TX) */
    uint16_t status_rx_prod;    /* the return-ring producer the status reports */
    uint16_t rx_std_cons;       /* the chip's standard-ring consumer */
    bool tx_busy;               /* guards the send against netdev re-entrancy */

    /* Interrupt state: a mask set through the 0x0204 mailbox and a pending
     * latch set when a status-block update awaits the driver. The line is the
     * conjunction of the latch with neither mask (mailbox nor MISC_CTL global)
     * set; tx_busy above is transient and not migrated, these two are. */
    bool irq_masked;
    bool irq_pending;

    /* The integrated copper PHY, reached through the MI registers. */
    Bcm570xPhyState phy;
};

static uint32_t bcm570x_regfile_read(Bcm570xState *s, uint32_t off)
{
    return s->regfile[(off & (BCM570X_REG_FILE_SIZE - 1)) >> 2];
}

static void bcm570x_regfile_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    s->regfile[(off & (BCM570X_REG_FILE_SIZE - 1)) >> 2] = val;
}

static uint32_t bcm570x_sram_read(Bcm570xState *s, uint32_t off)
{
    return s->sram[(off & (BCM570X_SRAM_SIZE - 1)) >> 2];
}

static void bcm570x_sram_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    s->sram[(off & (BCM570X_SRAM_SIZE - 1)) >> 2] = val;
}

/* The chip id MISC_CTL[31:16] presents, from which the driver reads the part. */
static uint16_t bcm570x_chip_id(Bcm570xState *s)
{
    switch (s->device_id) {
    case 0x1648:    /* BCM5704 copper */
    case 0x16a8:    /* BCM5704S SerDes */
        return BCM570X_CHIPID_BCM5704;
    default:        /* BCM5701 single-port, the SGI board default */
        return BCM570X_CHIPID_BCM5701;
    }
}

/*
 * The interrupt is a level, not an edge: the chip asserts INTA while it has a
 * status-block update the driver has not taken (irq_pending) and neither mask
 * is set -- the interrupt mailbox (irq_masked) nor the global
 * MISC_CTL.MASK_PCI_INTR. PCISTATE.INTR_NOT_ACTIVE mirrors the inverse, so the
 * driver's shared-interrupt spurious check (bge_intr) reads true exactly when
 * the line is down. Recompute the config bit and drive the wire together so the
 * two never disagree; the Bridge follows this level to the Hub.
 */
static bool bcm570x_irq_active(Bcm570xState *s)
{
    uint32_t misc = pci_get_long(PCI_DEVICE(s)->config + BCM570X_PCI_MISC_CTL);

    return s->irq_pending && !s->irq_masked &&
           !(misc & BCM570X_MISCCTL_MASK_PCI_INTR);
}

static void bcm570x_update_irq(Bcm570xState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    bool active = bcm570x_irq_active(s);
    uint32_t pcistate = pci_get_long(pci->config + BCM570X_PCI_PCISTATE);

    if (active) {
        pcistate &= ~BCM570X_PCISTATE_INTR_NOT_ACTIVE;
    } else {
        pcistate |= BCM570X_PCISTATE_INTR_NOT_ACTIVE;
    }
    pci_set_long(pci->config + BCM570X_PCI_PCISTATE, pcistate);
    pci_set_irq(pci, active);
}

/*
 * Present the station address in the SRAM mailbox the driver reads first, with
 * the tag in the high half of 0x0C14. The on-chip bootcode leaves it there, so
 * every reset re-populates it.
 */
static void bcm570x_load_mac_mailbox(Bcm570xState *s)
{
    const uint8_t *m = s->conf.macaddr.a;

    bcm570x_sram_write(s, BCM570X_SRAM_MAC_HI,
                       ((uint32_t)BCM570X_MAC_TAG << 16) |
                       ((uint32_t)m[0] << 8) | m[1]);
    bcm570x_sram_write(s, BCM570X_SRAM_MAC_LO,
                       ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) |
                       ((uint32_t)m[4] << 8) | m[5]);
}

/*
 * A global reset (the driver's write of MISC_CFG.RESET_CORE_CLOCKS, by either
 * access path) returns the register file and the on-chip SRAM to their reset
 * values, leaves the firmware-ready complement in the SRAM handshake mailbox --
 * how the on-chip bootcode signals the driver it is up -- and re-populates the
 * MAC-address mailbox the bootcode also owns. The PCI config the driver saves
 * and restores by hand, the chip id in MISC_CTL among it, is untouched.
 */
static void bcm570x_global_reset(Bcm570xState *s)
{
    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->sram, 0, sizeof(s->sram));
    s->tx_cons = 0;
    s->status_tx_cons = 0;
    s->status_rx_prod = 0;
    s->rx_std_cons = 0;
    s->irq_masked = true;
    s->irq_pending = false;
    bcm570x_sram_write(s, BCM570X_SRAM_FW_MBOX, BCM570X_FW_HANDSHAKE_DONE);
    bcm570x_load_mac_mailbox(s);
    bcm570x_update_irq(s);
}

/*
 * DMA the status block to the host address the driver programmed through the
 * host-coalescing block, but only while that block is enabled -- the driver
 * gates it and reads the transmit/receive progress from there rather than a
 * register. The chip owns the block, so the model builds it whole each time.
 *
 * Byte order (settled here against the transmit round-trip qtest, per the
 * NIC-on-Bridge convention): the device delivers big-endian, and the Bridge is
 * a no-op for this slot's DMA (its DEV_SWAP bit set), the same shape the
 * in-tree IOC3 NIC uses. So the block is encoded big-endian and lands
 * big-endian in guest memory. The MODE_CTL DMA-swap and MISC_CTL word-swap the
 * driver programs are stored for read-back but applied as no-ops on this
 * big-endian target -- each swap layer modelled once, never doubled.
 */
static void bcm570x_dma_status_block(Bcm570xState *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t blk[BCM570X_STATUS_BLK_SIZE];
    uint64_t base;

    if (!(bcm570x_regfile_read(s, BCM570X_HCC_MODE) & BCM570X_HCC_MODE_ENABLE)) {
        return;
    }

    base = ((uint64_t)bcm570x_regfile_read(s, BCM570X_HCC_STATUSBLK_ADDR_HI)
            << 32) | bcm570x_regfile_read(s, BCM570X_HCC_STATUSBLK_ADDR_LO);

    memset(blk, 0, sizeof(blk));
    stl_be_p(blk, BCM570X_STATFLAG_UPDATED);
    stw_be_p(blk + BCM570X_STATUS_TX_CONS_OFF, s->status_tx_cons);
    stw_be_p(blk + BCM570X_STATUS_RX_PROD_OFF, s->status_rx_prod);
    pci_dma_write(pci, base, blk, sizeof(blk));

    /*
     * The block now carries work the driver has not taken; latch it and let the
     * line assert if interrupts are unmasked. Reached only past the enable gate
     * above, so a disabled host-coalescing block raises no interrupt either.
     */
    s->irq_pending = true;
    bcm570x_update_irq(s);
}

/*
 * Ring the transmit doorbell: send every descriptor from the chip's consumer up
 * to the producer index the driver just wrote, then report the new consumer
 * through the status block.
 *
 * The producer is a descriptor index the driver reduces modulo the ring length
 * itself (the ring holds 512 descriptors), so it is inherently descriptor-
 * aligned; the failure mode is an index past the ring, which -- left unguarded
 * -- makes the walk chase a consumer it never reaches and the mailbox write
 * never return. That is the IOC3 ethernet review's first finding, so an out-of-
 * range index logs a guest error and returns, and the walk is bounded by the
 * ring length regardless. A re-entrancy flag holds across the send so a peer
 * that calls synchronously back into the ring cannot corrupt its state.
 */
static void bcm570x_tx_kick(Bcm570xState *s, uint32_t prod_raw)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t prod = prod_raw & 0xffff;
    uint32_t maxlen_flags =
        bcm570x_sram_read(s, BCM570X_SRAM_SEND_RING_RCB +
                          BCM570X_RCB_MAXLEN_FLAGS);
    uint16_t ring_len = maxlen_flags >> 16;
    uint64_t ring_base;
    uint8_t frame[BCM570X_MAX_FRAME];
    unsigned int frame_len = 0;
    unsigned int walked = 0;

    if (ring_len == 0 || prod >= ring_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: transmit producer %u out of range "
                      "(ring length %u)\n", prod, ring_len);
        return;
    }
    if (s->tx_busy) {
        return;
    }

    ring_base = ((uint64_t)bcm570x_sram_read(s, BCM570X_SRAM_SEND_RING_RCB +
                                             BCM570X_RCB_HOSTADDR_HI) << 32) |
                bcm570x_sram_read(s, BCM570X_SRAM_SEND_RING_RCB +
                                  BCM570X_RCB_HOSTADDR_LO);

    s->tx_busy = true;
    while (s->tx_cons != prod && walked < ring_len) {
        uint8_t bd[BCM570X_TXBD_SIZE];
        uint64_t buf_addr;
        uint16_t len, flags;

        pci_dma_read(pci, ring_base + (uint64_t)s->tx_cons * BCM570X_TXBD_SIZE,
                     bd, sizeof(bd));
        buf_addr = ((uint64_t)ldl_be_p(bd + BCM570X_TXBD_ADDR_HI) << 32) |
                   ldl_be_p(bd + BCM570X_TXBD_ADDR_LO);
        len = lduw_be_p(bd + BCM570X_TXBD_LEN);
        flags = lduw_be_p(bd + BCM570X_TXBD_FLAGS);

        if (frame_len + len <= sizeof(frame)) {
            pci_dma_read(pci, buf_addr, frame + frame_len, len);
            frame_len += len;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: transmit frame exceeds %u bytes, dropped\n",
                          (unsigned int)sizeof(frame));
            frame_len = 0;
        }

        if (flags & BCM570X_TXBDFLAG_END) {
            if (frame_len) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, frame_len);
            }
            frame_len = 0;
        }

        s->tx_cons = (s->tx_cons + 1) % ring_len;
        walked++;
    }
    s->tx_busy = false;

    s->status_tx_cons = prod;
    bcm570x_dma_status_block(s);
}

/*
 * Register-file writes, reached identically from the BAR and from the indirect
 * register window, so a side effect fires whichever path the driver uses -- the
 * 5700/5701 issue the global reset through the indirect window, not the BAR.
 */
static void bcm570x_reg_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    off &= (BCM570X_REG_FILE_SIZE - 1);

    switch (off) {
    case BCM570X_MISC_CFG:
        if (val & BCM570X_MISCCFG_RESET_CORE_CLOCKS) {
            bcm570x_global_reset(s);
            return;
        }
        break;
    case BCM570X_MAC_MODE:
    case BCM570X_HCC_MODE:
        /* The reset bit self-clears; the rest of the write persists. */
        val &= ~BCM570X_MODE_RESET;
        break;
    case BCM570X_MBX_IRQ0_LO:
        /*
         * The interrupt mask. Writing 1 masks and acknowledges: it drops the
         * line and clears the pending latch, so re-enabling does not re-fire on
         * an update already taken. Writing 0 re-enables, and a later update
         * re-asserts. The driver reads completions from the status block, not
         * this register, so the stored value is only for read-back.
         */
        s->irq_masked = (val & 1) != 0;
        if (val & 1) {
            s->irq_pending = false;
        }
        bcm570x_regfile_write(s, off, val);
        bcm570x_update_irq(s);
        return;
    case BCM570X_MBX_TX_PROD_LO:
        /* Store the producer for read-back, then send up to it. */
        bcm570x_regfile_write(s, off, val);
        bcm570x_tx_kick(s, val);
        return;
    case BCM570X_MBX_RX_STD_PROD_LO: {
        /* Posting standard-ring buffers; guard the index like the TX one. */
        uint16_t std_len =
            bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_MAXLEN_FLAGS) >> 16;

        if (std_len == 0 || (val & 0xffff) >= std_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: receive standard producer %u out of range "
                          "(ring length %u)\n", val & 0xffff, std_len);
            return;
        }
        bcm570x_regfile_write(s, off, val);
        /* A newly posted buffer may release a frame the peer had queued. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    }
    case BCM570X_MBX_RX_CONS0_LO: {
        /* The driver's return-ring acknowledgement; same range guard. */
        uint16_t ret_len =
            bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                              BCM570X_RCB_MAXLEN_FLAGS) >> 16;

        if (ret_len == 0 || (val & 0xffff) >= ret_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: receive consumer %u out of range "
                          "(ring length %u)\n", val & 0xffff, ret_len);
            return;
        }
        bcm570x_regfile_write(s, off, val);
        return;
    }
    case BCM570X_MI_COMM: {
        /*
         * A PHY management access. The integrated PHY answers at any address
         * -- it is the only one on the internal bus and the driver reads a
         * fixed address -- so the address field is not decoded. A read leaves
         * the value in the low 16 bits with BUSY clear; a write forwards the
         * data. Either way the chip completes at once, so a poll for BUSY
         * clear succeeds on the first read.
         */
        unsigned reg = (val & BCM570X_MICOMM_REG) >> BCM570X_MICOMM_REG_SHIFT;

        if (val & BCM570X_MICMD_READ) {
            val = bcm570x_phy_read(&s->phy, reg);
        } else {
            if (val & BCM570X_MICMD_WRITE) {
                bcm570x_phy_write(&s->phy, reg, val & BCM570X_MICOMM_DATA);
            }
            val &= ~BCM570X_MICOMM_BUSY;
        }
        bcm570x_regfile_write(s, off, val);
        return;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm570x: write 0x%08x to register 0x%04x has no "
                      "modelled effect yet\n", val, off);
        break;
    }
    bcm570x_regfile_write(s, off, val);
}

/*
 * Register-file reads, reached identically from the BAR and the indirect
 * window. Most registers read back their stored value; MI_STS is derived from
 * the PHY's live link so the driver's auto-poll path sees the carrier.
 */
static uint32_t bcm570x_reg_read(Bcm570xState *s, uint32_t off)
{
    switch (off & (BCM570X_REG_FILE_SIZE - 1)) {
    case BCM570X_MI_STS:
        return bcm570x_phy_link_up(&s->phy) ? BCM570X_MISTS_LINK : 0;
    default:
        return bcm570x_regfile_read(s, off);
    }
}

/* SRAM offset the BAR0 window at 0x8000 exposes for a given BAR address. */
static uint32_t bcm570x_window_sram_off(Bcm570xState *s, hwaddr addr)
{
    uint32_t base = s->mem_win_base & ~(BCM570X_SRAM_SIZE - 1);

    return base + (uint32_t)(addr - BCM570X_REG_FILE_SIZE);
}

static uint64_t bcm570x_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm570xState *s = opaque;

    if (addr < BCM570X_REG_FILE_SIZE) {
        return bcm570x_reg_read(s, addr);
    }
    return bcm570x_sram_read(s, bcm570x_window_sram_off(s, addr));
}

static void bcm570x_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Bcm570xState *s = opaque;

    if (addr < BCM570X_REG_FILE_SIZE) {
        bcm570x_reg_write(s, addr, val);
        return;
    }
    bcm570x_sram_write(s, bcm570x_window_sram_off(s, addr), val);
}

/*
 * The register file and the SRAM are 32-bit-accessed. impl.min/max pin the
 * width so the memory core never chops a wide access into repeated calls
 * (which would run a register's side effects twice) nor widens a narrow one.
 *
 * The declared endianness is DEVICE_LITTLE_ENDIAN, the ISP1040 precedent for
 * these little-endian-native controllers behind the Bridge, now settled against
 * the transmit round-trip qtest. Register MMIO correction is the Bridge's and
 * is independent of the DMA DEV_SWAP path (a separate Layer-A concern): a guest
 * dword read of a register returns the value written, proven by the Task-1
 * round-trips. The DMA byte order is settled separately in bcm570x_tx_kick /
 * bcm570x_dma_status_block.
 */
static const MemoryRegionOps bcm570x_bar0_ops = {
    .read = bcm570x_bar0_read,
    .write = bcm570x_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * The two indirect config-space windows. A 32-bit write to the base register
 * (0x78 / 0x7C) records the offset; a 32-bit access to the data register
 * (0x80 / 0x84) then reaches that offset in the register file or in SRAM. Every
 * other config access takes the default path.
 */
static void bcm570x_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                 int len)
{
    Bcm570xState *s = BCM570X(dev);

    if (len == 4) {
        switch (addr) {
        case BCM570X_PCI_REG_BASEADDR:
            s->reg_win_base = val;
            return;
        case BCM570X_PCI_MEMWIN_BASEADDR:
            s->mem_win_base = val;
            return;
        case BCM570X_PCI_REG_DATA:
            bcm570x_reg_write(s, s->reg_win_base, val);
            return;
        case BCM570X_PCI_MEMWIN_DATA:
            bcm570x_sram_write(s, s->mem_win_base, val);
            return;
        }
    }
    pci_default_write_config(dev, addr, val, len);

    /*
     * MISC_CTL's low bits gate the interrupt: MASK_PCI_INTR is the global mask
     * and CLEAR_INTA is a write-1-to-acknowledge that self-clears. A write that
     * lands anywhere in that register acknowledges if asked and re-derives the
     * line -- this is how bge_blockinit's "set CLEAR_INTA, clear MASK_PCI_INTR"
     * enables the interrupt.
     */
    if (addr < BCM570X_PCI_MISC_CTL + 4 && addr + len > BCM570X_PCI_MISC_CTL) {
        uint32_t misc = pci_get_long(dev->config + BCM570X_PCI_MISC_CTL);

        if (misc & BCM570X_MISCCTL_CLEAR_INTA) {
            s->irq_pending = false;
            misc &= ~BCM570X_MISCCTL_CLEAR_INTA;
            pci_set_long(dev->config + BCM570X_PCI_MISC_CTL, misc);
        }
        bcm570x_update_irq(s);
    }
}

static uint32_t bcm570x_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    Bcm570xState *s = BCM570X(dev);

    if (len == 4) {
        switch (addr) {
        case BCM570X_PCI_REG_BASEADDR:
            return s->reg_win_base;
        case BCM570X_PCI_MEMWIN_BASEADDR:
            return s->mem_win_base;
        case BCM570X_PCI_REG_DATA:
            return bcm570x_reg_read(s, s->reg_win_base);
        case BCM570X_PCI_MEMWIN_DATA:
            return bcm570x_sram_read(s, s->mem_win_base);
        }
    }
    return pci_default_read_config(dev, addr, len);
}

/*
 * Whether the engine can take a frame: the receive rings must be set up and the
 * host-coalescing block enabled, and the host must have posted a standard-ring
 * buffer the chip has not yet used -- its producer mailbox ahead of the chip's
 * consumer. Shared by can_receive and receive so the two never disagree.
 */
static bool bcm570x_rx_ready(Bcm570xState *s)
{
    uint16_t std_len =
        bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_MAXLEN_FLAGS) >> 16;
    uint16_t ret_len =
        bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                          BCM570X_RCB_MAXLEN_FLAGS) >> 16;
    uint16_t std_prod =
        bcm570x_regfile_read(s, BCM570X_MBX_RX_STD_PROD_LO) & 0xffff;

    return std_len != 0 && ret_len != 0 &&
           (bcm570x_regfile_read(s, BCM570X_HCC_MODE) &
            BCM570X_HCC_MODE_ENABLE) &&
           std_prod != s->rx_std_cons;
}

static bool bcm570x_can_receive(NetClientState *nc)
{
    return bcm570x_rx_ready(qemu_get_nic_opaque(nc));
}

/*
 * The address filter: accept broadcast and multicast, and unicast addressed to
 * our station. The station address is the one handed through the SRAM mailbox
 * (the device's network configuration); a guest that reprograms the MAC filter
 * registers to a different address is not modelled here.
 */
static bool bcm570x_frame_accepted(Bcm570xState *s, const uint8_t *buf,
                                   size_t size)
{
    if (size < 6) {
        return false;
    }
    if (buf[0] & 0x01) {
        return true;
    }
    return memcmp(buf, s->conf.macaddr.a, 6) == 0;
}

/*
 * A frame from the backend. Not ready, or no buffer posted, returns 0 -- the
 * only answer that leaves the frame at the peer's queue head for later. A frame
 * the address filter drops returns the consumed count, never 0, so the peer is
 * not wedged waiting on a frame that will never be taken. Otherwise the frame is
 * DMA'd into the next posted standard-ring buffer, a completed descriptor is
 * written into the return ring, the receive producer advances, and the status
 * block reports it. The byte order reuses the transmit path's exactly:
 * descriptors big-endian, the payload a raw byte stream, the Bridge a no-op.
 */
static ssize_t bcm570x_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    Bcm570xState *s = qemu_get_nic_opaque(nc);
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t std_len =
        bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_MAXLEN_FLAGS) >> 16;
    uint16_t ret_len =
        bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                          BCM570X_RCB_MAXLEN_FLAGS) >> 16;
    uint64_t std_base, ret_base, buf_addr;
    uint8_t std_bd[BCM570X_RXBD_SIZE];
    uint8_t ret_bd[BCM570X_RXBD_SIZE];
    unsigned int idx;

    if (!bcm570x_rx_ready(s)) {
        return 0;
    }
    if (!bcm570x_frame_accepted(s, buf, size)) {
        return size;
    }
    if (size > BCM570X_MAX_FRAME) {
        size = BCM570X_MAX_FRAME;
    }

    std_base = ((uint64_t)bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_HADDR_HI)
                << 32) | bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_HADDR_LO);
    ret_base = ((uint64_t)bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                                            BCM570X_RCB_HOSTADDR_HI) << 32) |
               bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                                 BCM570X_RCB_HOSTADDR_LO);
    idx = s->rx_std_cons;

    /* The posted buffer's host address, from the standard-ring descriptor. */
    pci_dma_read(pci, std_base + (uint64_t)idx * BCM570X_RXBD_SIZE,
                 std_bd, sizeof(std_bd));
    buf_addr = ((uint64_t)ldl_be_p(std_bd + BCM570X_RXBD_ADDR_HI) << 32) |
               ldl_be_p(std_bd + BCM570X_RXBD_ADDR_LO);

    pci_dma_write(pci, buf_addr, buf, size);

    /* The completed descriptor into the return ring, big-endian. */
    memset(ret_bd, 0, sizeof(ret_bd));
    stl_be_p(ret_bd + BCM570X_RXBD_ADDR_HI, buf_addr >> 32);
    stl_be_p(ret_bd + BCM570X_RXBD_ADDR_LO, buf_addr & 0xffffffff);
    stw_be_p(ret_bd + BCM570X_RXBD_IDX, idx);
    stw_be_p(ret_bd + BCM570X_RXBD_LEN, size);
    stw_be_p(ret_bd + BCM570X_RXBD_FLAGS, BCM570X_RXBDFLAG_END);
    pci_dma_write(pci, ret_base + (uint64_t)s->status_rx_prod *
                  BCM570X_RXBD_SIZE, ret_bd, sizeof(ret_bd));

    s->rx_std_cons = (s->rx_std_cons + 1) % std_len;
    s->status_rx_prod = (s->status_rx_prod + 1) % ret_len;

    bcm570x_dma_status_block(s);
    return size;
}

/* Tie the PHY's carrier to the netdev: a backend that goes down drops link. */
static void bcm570x_set_link_status(NetClientState *nc)
{
    Bcm570xState *s = qemu_get_nic_opaque(nc);

    bcm570x_phy_set_link_down(&s->phy, nc->link_down);
}

static NetClientInfo net_bcm570x_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bcm570x_can_receive,
    .receive = bcm570x_receive,
    .link_status_changed = bcm570x_set_link_status,
};

static void bcm570x_realize(PCIDevice *dev, Error **errp)
{
    Bcm570xState *s = BCM570X(dev);
    DeviceState *ds = DEVICE(dev);
    uint8_t *pci_conf = dev->config;

    if (s->model && strcmp(s->model, "bcm5701") != 0 &&
        strcmp(s->model, "bcm5704") != 0) {
        error_setg(errp, "bcm570x: unknown model '%s' "
                   "(expected bcm5701 or bcm5704)", s->model);
        return;
    }

    /*
     * The model selects the identity. bcm5704 defaults to the copper dual-port
     * four-tuple; a caller leaving the id properties at their bcm5701 defaults
     * gets it, one wanting the SerDes variant overrides device-id and
     * subsystem-id. This runs before the ids reach config space and before the
     * chip id is derived from device-id below.
     */
    if (s->model && strcmp(s->model, "bcm5704") == 0) {
        if (s->device_id == BCM570X_DEVICE_ID) {
            s->device_id = BCM570X_DEVICE_ID_BCM5704;
        }
        if (s->subsystem_id == BCM570X_SUBSYS_ID) {
            s->subsystem_id = BCM570X_SUBSYS_ID_BCM5704;
        }
    }

    /*
     * The PCI four-tuple IRIX tg requires. The Broadcom primary vendor/device
     * sit at 0x00/0x02 and the SGI subsystem identity in the standard PCI
     * subsystem fields: subsystem vendor at 0x2C, subsystem id at 0x2E. (bge's
     * BGE_PCI_SUBSYS/BGE_PCI_SUBVID header macro names label these offsets
     * backwards, but if_bge.c reads them the standard way -- the low half of
     * the 0x2C dword is the subsystem vendor -- so standard placement is what
     * the silicon and IRIX both use.)
     */
    pci_config_set_vendor_id(pci_conf, s->vendor_id);
    pci_config_set_device_id(pci_conf, s->device_id);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, s->subsystem_vendor_id);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, s->subsystem_id);
    pci_conf[PCI_INTERRUPT_PIN] = 0x01;     /* INTA */

    /*
     * MISC_CTL[31:16] is the chip id, which the driver reads its whole identity
     * from. Keep it read-only across reset; leave the low 16 control bits
     * writable so the driver's byte-order and interrupt-mask writes stick.
     */
    pci_set_long(pci_conf + BCM570X_PCI_MISC_CTL,
                 (uint32_t)bcm570x_chip_id(s) << 16);
    pci_set_word(dev->wmask + BCM570X_PCI_MISC_CTL, 0xffff);

    /*
     * PCISTATE presents conventional-PCI bus mode and, in INTR_NOT_ACTIVE, the
     * inverse of the interrupt line the driver reads for its spurious check.
     * The chip drives it, so it stays read-only to the guest (no wmask); the
     * reset value has the line inactive.
     */
    pci_set_long(pci_conf + BCM570X_PCI_PCISTATE,
                 BCM570X_PCISTATE_BUSMODE_PCI |
                 BCM570X_PCISTATE_INTR_NOT_ACTIVE);

    memory_region_init_io(&s->bar0, OBJECT(s), &bcm570x_bar0_ops, s,
                          "bcm570x-bar0", BCM570X_BAR0_SIZE);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /*
     * Realize the PHY as a system-bus device so machine reset reaches it: a
     * bus-less device is never reset (see sgi-ioc3-eth.c for the same note).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->phy), errp)) {
        return;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_bcm570x_info, &s->conf,
                          object_get_typename(OBJECT(dev)), ds->id,
                          &ds->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    bcm570x_phy_set_link_down(&s->phy, qemu_get_queue(s->nic)->link_down);

    /*
     * The dual-port BCM5704 is one multifunction PCI device presenting two MAC
     * functions, not two devices and not a bridged card (datasheet: an
     * inference from the public BCM5704 architecture and the driver supporting
     * it). Model it as function 0 spawning an identical sibling at function 1 of
     * the same slot -- both are this same type running the same per-function
     * code, so the two share the implementation rather than forking it, and each
     * gets its own register file, SRAM, PHY, ring engine, status block and
     * interrupt. Only function 0 spawns, so the sibling at function 1 does not
     * recurse.
     *
     * The PCI core ran pci_init_multifunction for this function before this
     * realize, so set the multifunction capability and header bit by hand now:
     * without the capability on function 0 the core rejects registering a
     * function 1 as populating a single-function device. The sibling carries the
     * full four-tuple so a SerDes override on function 0 reaches function 1 too.
     */
    if (s->model && strcmp(s->model, "bcm5704") == 0 &&
        PCI_FUNC(dev->devfn) == 0) {
        PCIBus *bus = pci_get_bus(dev);
        PCIDevice *f1;

        dev->cap_present |= QEMU_PCI_CAP_MULTIFUNCTION;
        pci_conf[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;

        f1 = pci_new_multifunction(PCI_DEVFN(PCI_SLOT(dev->devfn), 1),
                                   TYPE_BCM570X);
        qdev_prop_set_string(&f1->qdev, "model", "bcm5704");
        qdev_prop_set_uint16(&f1->qdev, "vendor-id", s->vendor_id);
        qdev_prop_set_uint16(&f1->qdev, "device-id", s->device_id);
        qdev_prop_set_uint16(&f1->qdev, "subsystem-vendor-id",
                             s->subsystem_vendor_id);
        qdev_prop_set_uint16(&f1->qdev, "subsystem-id", s->subsystem_id);
        if (!pci_realize_and_unref(f1, bus, errp)) {
            return;
        }
    }
}

static void bcm570x_reset_hold(Object *obj, ResetType type)
{
    Bcm570xState *s = BCM570X(obj);

    /*
     * A cold reset zeroes the register file, the on-chip SRAM and the
     * indirect-window bases. The firmware-ready complement is arranged only by
     * the driver-triggered global reset (bcm570x_global_reset), which is the
     * one the driver writes the magic for and then polls; the chip id in
     * MISC_CTL lives in PCI config space and survives either.
     */
    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->sram, 0, sizeof(s->sram));
    s->reg_win_base = 0;
    s->mem_win_base = 0;
    s->tx_cons = 0;
    s->status_tx_cons = 0;
    s->status_rx_prod = 0;
    s->rx_std_cons = 0;
    s->tx_busy = false;
    s->irq_masked = true;
    s->irq_pending = false;
    bcm570x_load_mac_mailbox(s);
    bcm570x_update_irq(s);
}

/*
 * The migration stream carries the interrupt's cause and its masks but not the
 * wire itself. Re-derive the line on the destination from the restored state so
 * it asserts INTA exactly when the source did, and so PCISTATE.INTR_NOT_ACTIVE
 * and the wire agree. The PHY migrates as its own section (it is a realized
 * child device with its own VMStateDescription), so it is not enumerated here.
 */
static int bcm570x_post_load(void *opaque, int version_id)
{
    bcm570x_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_bcm570x = {
    .name = "bcm570x",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = bcm570x_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, Bcm570xState),
        VMSTATE_UINT32_ARRAY(regfile, Bcm570xState, BCM570X_REG_FILE_WORDS),
        VMSTATE_UINT32_ARRAY(sram, Bcm570xState, BCM570X_SRAM_WORDS),
        VMSTATE_UINT32(reg_win_base, Bcm570xState),
        VMSTATE_UINT32(mem_win_base, Bcm570xState),
        VMSTATE_UINT16(tx_cons, Bcm570xState),
        VMSTATE_UINT16(status_tx_cons, Bcm570xState),
        VMSTATE_UINT16(status_rx_prod, Bcm570xState),
        VMSTATE_UINT16(rx_std_cons, Bcm570xState),
        VMSTATE_BOOL_V(irq_masked, Bcm570xState, 2),
        VMSTATE_BOOL_V(irq_pending, Bcm570xState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm570x_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", Bcm570xState, vendor_id,
                       BCM570X_VENDOR_ID),
    DEFINE_PROP_UINT16("device-id", Bcm570xState, device_id,
                       BCM570X_DEVICE_ID),
    DEFINE_PROP_UINT16("subsystem-vendor-id", Bcm570xState,
                       subsystem_vendor_id, BCM570X_SUBSYS_VENDOR_ID),
    DEFINE_PROP_UINT16("subsystem-id", Bcm570xState, subsystem_id,
                       BCM570X_SUBSYS_ID),
    DEFINE_PROP_STRING("model", Bcm570xState, model),
    DEFINE_NIC_PROPERTIES(Bcm570xState, conf),
};

static void bcm570x_init(Object *obj)
{
    Bcm570xState *s = BCM570X(obj);

    object_initialize_child(obj, "phy", &s->phy, TYPE_BCM570X_PHY);
}

static void bcm570x_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = bcm570x_realize;
    pc->config_read = bcm570x_config_read;
    pc->config_write = bcm570x_config_write;
    pc->vendor_id = BCM570X_VENDOR_ID;
    pc->device_id = BCM570X_DEVICE_ID;
    pc->revision = 0x00;
    pc->class_id = PCI_CLASS_NETWORK_ETHERNET;

    rc->phases.hold = bcm570x_reset_hold;
    dc->vmsd = &vmstate_bcm570x;
    dc->desc = "Broadcom NetXtreme BCM570x Gigabit Ethernet";
    device_class_set_props(dc, bcm570x_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm570x_types[] = {
    {
        .name = TYPE_BCM570X,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(Bcm570xState),
        .instance_init = bcm570x_init,
        .class_init = bcm570x_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(bcm570x_types)
