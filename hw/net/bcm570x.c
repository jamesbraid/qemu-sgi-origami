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
 * global reset with its on-chip firmware-ready handshake -- the SRAM 0x0B50
 * mailbox and the firmware-progress register at 0x5034 the driver polls; the
 * self-clearing *_MODE reset bits, including the buffer-manager enable request
 * the IRIX driver polls for clear rather than set; the MII path to the
 * integrated copper PHY (a separate model) through the MI registers; the
 * MAC-address mailbox in SRAM;
 * link status tied to the netdev; the transmit ring -- the send-ring RCB, the
 * producer mailbox, the descriptor/data DMA to host memory, and the status
 * block DMA'd to the host, with the DMA byte order settled here; and the receive
 * ring -- posted standard-ring buffers, the frame DMA'd in, a completed
 * descriptor written to the return ring, and the status block's receive
 * producer advanced; and the INTA interrupt -- the 0x0204 mailbox
 * mask/acknowledge protocol, the MISC_CTL global mask, and
 * PCISTATE.INTR_NOT_ACTIVE, driven as an INTA level when a
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
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "net/checksum.h"
#include "hw/pci/pci_device.h"
#include "standard-headers/linux/virtio_net.h"
#include "hw/net/net_tx_pkt.h"
#include "hw/pci-host/sgi-bridge.h"
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
/*
 * The low 256 bytes of BAR0 are the PCI configuration registers again: the part
 * presents them in both spaces, and a driver reaches MISC_CTL and PCISTATE
 * through whichever is convenient. Reaching them through the BAR has to land on
 * the same state, or a driver that acknowledges its interrupt there is writing
 * into a register file nothing reads.
 */
#define BCM570X_PCI_CFG_ALIAS       0x100

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
#define BCM570X_MISCCTL_ENDIAN_BYTESWAP 0x00000004u
#define BCM570X_MISCCTL_ENDIAN_WORDSWAP 0x00000008u

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
/*
 * The MAC's status word. Its change bits are latches the driver clears by
 * writing them back, so storing what it wrote leaves every bit it tried to
 * clear reading as set -- the driver then sees the same condition it just
 * acknowledged, and keeps handling it.
 */
#define BCM570X_MAC_STATUS          0x0404
/*
 * MAC address filter slot 0. The chip loads the station address here from the
 * serial EEPROM at power-on, so a driver may read it back straight from these
 * registers instead of the SRAM mailbox: the high word holds the first two
 * address bytes, the low word the remaining four.
 */
#define BCM570X_MAC_ADDR_0_HIGH     0x0410
#define BCM570X_MAC_ADDR_0_LOW      0x0414
#define BCM570X_MAC_ADDR_STRIDE     8
#define BCM570X_HCC_MODE            0x3c00
/*
 * How often the host-coalescing block DMAs the status block on its own, in
 * microseconds. The part posts one whether or not traffic moved, and the IRIX
 * driver leans on that: its watchdog tears the interface down and re-runs
 * bring-up whenever a whole second passes with no interrupt, so a model that
 * only posts on traffic is torn down once a second forever. Capped below the
 * driver's one-second patience rather than trusted to the programmed value.
 */
#define BCM570X_HCC_STATS_TICKS     0x3c28
#define BCM570X_STATUS_TICK_MAX_NS  (250 * SCALE_MS)
#define BCM570X_BUFMGR_MODE         0x4400
#define BCM570X_MISC_CFG            0x6804

/*
 * Bit 0 of a *_MODE register is a self-clearing reset the driver sets and then
 * polls for clear. Bit 0 of MISC_CFG (RESET_CORE_CLOCKS) triggers the chip's
 * global reset instead.
 */
#define BCM570X_MODE_RESET          0x00000001u
#define BCM570X_MISCCFG_RESET_CORE_CLOCKS 0x00000001u

/*
 * The buffer-manager enable bit diverges from the public drivers. tg3 and bge
 * set BUFMGR_MODE.ENABLE and poll for it to stay SET; the IRIX tg driver sets it
 * and instead polls for it to CLEAR, reading the drop as the chip's
 * acknowledgement that the buffer manager has started. So the model self-clears
 * ENABLE -- an enable-request the chip completes at once -- while ATTN_ENABLE and
 * the rest of the write persist.
 */
#define BCM570X_BUFMGR_MODE_ENABLE  0x00000002u

/*
 * The firmware-ready handshake. The driver writes the magic to on-chip SRAM
 * 0x0B50 and polls there for its complement, which is how the on-chip bootcode
 * signals initialisation complete. The mailbox returns the complement of the
 * last value written, so the magic reads back ready whether the driver writes
 * it before the reset (bge/tg3) or after it (IRIX tg).
 */
#define BCM570X_SRAM_FW_MBOX        0x0b50
#define BCM570X_FW_MAGIC            0x4b657654u
#define BCM570X_FW_HANDSHAKE_DONE   (~BCM570X_FW_MAGIC)     /* 0xB49A89AB */

/*
 * The firmware-progress register (RX RISC block; RX_CPU_HWBKPT at 0x5034 in
 * the public tg3 names). The IRIX driver polls it after a reset and fails the
 * moment it changes from the all-ones init to a value it does not recognise; a
 * bare zero reads as such a value. The chip holds it at the all-ones "no
 * progress yet" sentinel so the driver reads "unchanged" and keeps polling the
 * mailbox.
 */
#define BCM570X_FW_PROGRESS         0x5034
#define BCM570X_FW_PROGRESS_INIT    0xffffffffu

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
/*
 * Flag bits within an RCB's Max_Len/Flags word (PG105 Table 27, "Flag
 * Fields", page 91). Only the jumbo ring's Extended RX Enable and Disable
 * Ring bits have a modelled effect; the others (mini ring, etc.) are not
 * used on this machine.
 */
#define BCM570X_RCB_FLAG_EXT_RX_ENABLE 0x0001  /* use the 64-byte jumbo BD */
#define BCM570X_RCB_FLAG_DISABLE_RING  0x0002  /* ring not in use */
#define BCM570X_TXBD_SIZE           16
#define BCM570X_TXBD_ADDR_HI        0x00    /* offsets within a tx descriptor */
#define BCM570X_TXBD_ADDR_LO        0x04
/* len_flags word at 0x08: length in the high 16 bits (0x0a), flags low (0x08). */
#define BCM570X_TXBD_FLAGS          0x08
#define BCM570X_TXBD_LEN            0x0a
/*
 * The last word of a transmit descriptor carries the maximum segment size in
 * its high half. A non-zero value is the driver asking the card to cut one
 * large buffer into segments of that size -- what IRIX advertises on the
 * interface as L2TCPSEG -- rather than handing down one segment per frame. The
 * work saved is the guest's, which is where the time goes on an emulated CPU.
 */
#define BCM570X_TXBD_MSS            0x0e
/*
 * The offload the driver asks for in a transmit descriptor. It hands the frame
 * over with the checksum fields still placeholders when it sets these, so a
 * model that ignores them puts frames on the wire that every receiver discards
 * without a word. ICMP and ARP hide it: the guest computes those itself.
 */
#define BCM570X_TXBDFLAG_TCP_UDP_CKSUM 0x0001
#define BCM570X_TXBDFLAG_IP_CKSUM   0x0002
#define BCM570X_TXBDFLAG_END        0x0004
#define BCM570X_TXBDFLAG_INSERT_SRC_ADDR 0x1000
#define BCM570X_TXBDFLAG_CHOOSE_SRC_ADDR 0x6000
#define BCM570X_TXBDFLAG_SRC_ADDR_SHIFT  13

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
#define BCM570X_HCC_RX_COAL_TICKS   0x3c08  /* receive coalescing tick budget */
#define BCM570X_HCC_TX_COAL_TICKS   0x3c0c  /* transmit coalescing tick budget */
#define BCM570X_HCC_RX_MAX_COAL_BDS 0x3c10  /* receive coalescing frame count */
#define BCM570X_HCC_TX_MAX_COAL_BDS 0x3c14  /* transmit coalescing frame count */
#define BCM570X_STATUS_BLK_SIZE     0x50
#define BCM570X_STATFLAG_UPDATED    0x00000001u
/* Status-block idx[0]: rx_producer at 0x10, tx_consumer at 0x12 (tg3 layout). */
#define BCM570X_STATUS_RX_PROD_OFF  16
#define BCM570X_STATUS_TX_CONS_OFF  18
#define BCM570X_STATUS_TAG_OFF      4

/*
 * The interrupt mailbox. Writing 1 masks the line and acknowledges the pending
 * status-block update; writing 0 re-enables it. The driver writes 1 on entry to
 * its handler and 0 on exit (bge_intr), and writes 0 once in bge_blockinit to
 * first enable host interrupts. Only the low 32 bits of the 64-bit mailbox are
 * used, and the chip acts on a write to the low half.
 */
#define BCM570X_MBX_IRQ0_LO         0x0204

/*
 * The largest frame the engines carry, transmit or receive. The part does jumbo
 * frames, and the figure here matches what the other two Origin adapters carry
 * so that a guest configured for jumbo behaves the same whichever card it is
 * given. It is a model limit, not the hardware's.
 */
#define BCM570X_MAX_FRAME           9600

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
/*
 * The standard receive ring holds a fixed number of descriptors. Its control
 * block's length field is the size of the buffer each descriptor points at --
 * an MTU, not a count -- while the return ring's really is a ring length. Taking
 * the buffer size for a ring length wraps this ring somewhere the driver does
 * not, and the two disagree about which descriptors are posted: the card walks
 * on into slots the driver has not filled and reads whatever they held.
 */
#define BCM570X_RX_STD_RING_LEN     512u
/*
 * The jumbo receive ring. A frame too big for the standard ring's buffers rides
 * a ring of its own, whose buffers the driver sizes for the larger MTU, and the
 * completion is marked so the driver recycles from the ring the buffer came
 * from. Without it a jumbo frame has nowhere big enough to go, and writing one
 * into a standard-ring buffer runs off the end of what the driver set aside --
 * which corrupts whatever the guest had after it.
 *
 * The jumbo RCB's Max_Len/Flags word (0x2448) is not consulted for its
 * length: the manual says Max_Len there is unused for jumbo rings, ignored
 * by hardware in favor of the standard ring's own Max_Len (PG105 page 91,
 * Table 341, page 438). A traced IRIX 6.5.30 boot at MTU 9000 confirms the
 * driver leaves it at 0 and never writes it. What does matter is Disable
 * Ring (bit 1): the ring is available only while that bit is clear and its
 * host base is nonzero. Extended RX Enable (bit 0) asks for 64-byte
 * descriptors this model does not parse; while set, the ring is treated the
 * same as disabled.
 */
#define BCM570X_MBX_RX_JUMBO_PROD_LO 0x0274
#define BCM570X_RX_JUMBO_RCB_HADDR_HI 0x2440
#define BCM570X_RX_JUMBO_RCB_HADDR_LO 0x2444
#define BCM570X_RX_JUMBO_RCB_MAXLEN_FLAGS 0x2448
#define BCM570X_RX_JUMBO_RING_LEN   256u
#define BCM570X_SRAM_RX_RETURN_RCB  0x0200
#define BCM570X_RXBD_SIZE           32
#define BCM570X_RXBD_ADDR_HI        0x00    /* offsets within an rx descriptor */
#define BCM570X_RXBD_ADDR_LO        0x04
/*
 * A completed descriptor packs the ring index and length into one word and the
 * type and flags into the next, using the transmit ring's scalar layout. The
 * length includes the four-byte FCS the driver strips; the END flag marks the
 * last descriptor of a frame. The opaque word is copied back from the posted
 * buffer so the driver maps the frame to the buffer it supplied.
 */
#define BCM570X_RXBD_IDX_LEN        0x08    /* ring index (31:16), length (15:0) */
#define BCM570X_RXBD_TYPE_FLAGS     0x0c    /* type (31:16), flags (15:0) */
#define BCM570X_RXBD_CSUM           0x10 /* ip sum (31:16), tcp/udp sum (15:0) */
#define BCM570X_RXBD_OPAQUE         0x1c
#define BCM570X_RXBDFLAG_TCP_UDP_CKSUM 0x0001
#define BCM570X_RXBDFLAG_JUMBO_RING 0x0020
#define BCM570X_RXBDFLAG_IP_CKSUM   0x0002
#define BCM570X_RXBDFLAG_END        0x0004
#define BCM570X_ETH_FCS_LEN         4
#define BCM570X_ETH_MIN_FRAME       60

struct Bcm570xState {
    PCIDevice parent_obj;

    /*
     * The plain DMA route offered by an SGI Bridge, or NULL elsewhere.
     * Big-endian scalar accessors on this route preserve the descriptor and
     * status layout of little-endian accessors through the Bridge swap route.
     * Frame payload is copied verbatim as a bounded model behavior for the
     * measured BCM5701 configuration. That payload choice does not establish
     * the card's hardware transformation with its DMA-swap controls clear.
     * Off-Bridge devices retain the existing little-endian scalar view and
     * payload handling.
     */
    AddressSpace *plain_dma;

    MemoryRegion bar0;

    NICState *nic;
    NICConf conf;
    struct NetTxPkt *tx_pkt;
    bool has_vnet_hdr;
    QEMUBH *rx_flush_bh;
    bool rx_flush_pending;

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
    uint16_t rx_jumbo_cons;     /* the chip's jumbo-ring consumer */
    bool tx_busy;               /* guards the send against netdev re-entrancy */
    bool backend_rx_deferred;

    /* Interrupt state: a mask set through the 0x0204 mailbox and a pending
     * latch set when a status-block update awaits the driver. The line is the
     * conjunction of the latch with neither mask (mailbox nor MISC_CTL global)
     * set. tx_busy and backend_rx_deferred are transient; the remaining state
     * is migrated.
     */
    bool irq_masked;
    bool irq_pending;
    bool status_kick_done;  /* bring-up status update posted once past enable */
    uint8_t status_tag;         /* monotonic per status-block update, delivered in the block */

    /* Receive interrupt coalescing: frames accepted since the last status-block
     * DMA, and the timer that flushes a partial group so the tail of a burst is
     * never stranded past the driver's programmed tick budget. */
    uint16_t rx_coal_pending;
    uint16_t tx_coal_pending;
    QEMUTimer tx_coal_timer;
    QEMUTimer status_tick_timer;
    QEMUTimer coal_timer;

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
    uint32_t o = off & (BCM570X_SRAM_SIZE - 1);

    return s->sram[o >> 2];
}

static void bcm570x_sram_write(Bcm570xState *s, uint32_t off, uint32_t val)
{
    off &= (BCM570X_SRAM_SIZE - 1);

    /*
     * The firmware mailbox stands in for the on-chip bootcode: it reads
     * back the one's-complement of whatever was last written, so the
     * driver's magic reads as the ready value at once, no bootcode to run.
     */
    if (off == BCM570X_SRAM_FW_MBOX) {
        val = ~val;
    }
    s->sram[off >> 2] = val;
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
 * Present the station address the two ways a driver may read it at attach: the
 * SRAM mailbox at 0x0C14, tagged 0x484B in its high half, which bge_attach
 * reads first; and MAC address filter slot 0, which the chip loads from the
 * serial EEPROM at power-on and a driver may read straight from. The on-chip
 * bootcode owns both, so every reset re-populates them.
 */
static void bcm570x_load_station_address(Bcm570xState *s)
{
    const uint8_t *m = s->conf.macaddr.a;
    uint32_t hi = ((uint32_t)m[0] << 8) | m[1];
    uint32_t lo = ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) |
                  ((uint32_t)m[4] << 8) | m[5];

    bcm570x_sram_write(s, BCM570X_SRAM_MAC_HI,
                       ((uint32_t)BCM570X_MAC_TAG << 16) | hi);
    bcm570x_sram_write(s, BCM570X_SRAM_MAC_LO, lo);
    bcm570x_regfile_write(s, BCM570X_MAC_ADDR_0_HIGH, hi);
    bcm570x_regfile_write(s, BCM570X_MAC_ADDR_0_LOW, lo);
}

/* Discard frames tied to the old rings and reopen backend delivery. */
static void bcm570x_reset_rx_queue(Bcm570xState *s)
{
    if (s->nic) {
        NetClientState *nc = qemu_get_queue(s->nic);

        /*
         * The purge helper first retries the queue. With reset registers that
         * delivery is refused and sets receive_disabled again before the
         * packet is purged, so clear the gate explicitly afterwards.
         */
        qemu_flush_or_purge_queued_packets(nc, true);
        nc->receive_disabled = 0;
    }
    s->rx_flush_pending = false;
    s->backend_rx_deferred = false;
}

/*
 * A global reset (the driver's write of MISC_CFG.RESET_CORE_CLOCKS, by either
 * access path) returns the register file and the on-chip SRAM to their reset
 * values, re-arms the SRAM handshake mailbox to the firmware-ready complement
 * -- how the on-chip bootcode signals the driver it is up -- and re-populates
 * the MAC-address mailbox the bootcode also owns. Writing the magic through the
 * complementing mailbox leaves the ready value, so a driver that wrote the
 * magic before the reset (bge/tg3) still reads ready afterwards. The trigger
 * bit is not written back, so MISC_CFG bit 0 self-clears for the driver's poll.
 * The PCI config the driver saves and restores by hand, the chip id in MISC_CTL
 * among it, is untouched.
 */
static void bcm570x_global_reset(Bcm570xState *s)
{
    memset(s->regfile, 0, sizeof(s->regfile));
    memset(s->sram, 0, sizeof(s->sram));
    s->tx_cons = 0;
    s->status_tx_cons = 0;
    s->status_rx_prod = 0;
    s->rx_std_cons = 0;
    s->rx_jumbo_cons = 0;
    s->irq_masked = true;
    s->irq_pending = false;
    s->status_kick_done = false;
    s->status_tag = 0;
    s->rx_coal_pending = 0;
    s->tx_coal_pending = 0;
    if (s->rx_flush_bh) {
        qemu_bh_cancel(s->rx_flush_bh);
    }
    bcm570x_reset_rx_queue(s);
    timer_del(&s->coal_timer);
    timer_del(&s->tx_coal_timer);
    timer_del(&s->status_tick_timer);
    bcm570x_sram_write(s, BCM570X_SRAM_FW_MBOX, BCM570X_FW_MAGIC);
    bcm570x_load_station_address(s);
    bcm570x_update_irq(s);
}

/*
 * The driver word-swaps its register/SRAM host-address writes for the
 * big-endian host via ENDIAN_WORDSWAP; honour it at composition so the two
 * 32-bit halves form the intended 64-bit DMA address (datasheet "Byte order").
 */
static uint64_t bcm570x_compose_hostaddr(Bcm570xState *s, uint32_t hi,
                                         uint32_t lo)
{
    uint32_t misc = pci_get_long(PCI_DEVICE(s)->config + BCM570X_PCI_MISC_CTL);

    if (misc & BCM570X_MISCCTL_ENDIAN_WORDSWAP) {
        return ((uint64_t)lo << 32) | hi;
    }
    return ((uint64_t)hi << 32) | lo;
}

/*
 * The existing off-Bridge payload path applies swaps to whole 32-bit words
 * at their physical alignment. This describes the current model, not an
 * established hardware transformation for the MODE_CTL DMA-swap controls.
 */
static void bcm570x_swap_span(uint8_t *p, unsigned int span,
                              bool bswap, bool wswap)
{
    unsigned int i;

    for (i = 0; i + 4 <= span; i += 4) {
        uint8_t *w = p + i;
        uint8_t t;

        if (bswap) {
            t = w[0]; w[0] = w[1]; w[1] = t;
            t = w[2]; w[2] = w[3]; w[3] = t;
        }
        if (wswap) {
            t = w[0]; w[0] = w[2]; w[2] = t;
            t = w[1]; w[1] = w[3]; w[3] = t;
        }
    }
}

/*
 * One ordered nonframe DMA transfer, on whichever route the machine gives us:
 * Bridge's plain space when there is one, the slot's own (swap-corrected)
 * space otherwise. See plain_dma for the scalar and payload distinction.
 */
static MemTxResult bcm570x_dma_read_buf(Bcm570xState *s, uint64_t addr,
                                        void *buf, unsigned int len)
{
    if (s->plain_dma) {
        return dma_memory_read(s->plain_dma, addr, buf, len,
                               MEMTXATTRS_UNSPECIFIED);
    }
    return pci_dma_read(PCI_DEVICE(s), addr, buf, len);
}

static MemTxResult bcm570x_dma_write_buf(Bcm570xState *s, uint64_t addr,
                                         const void *buf, unsigned int len)
{
    if (s->plain_dma) {
        return dma_memory_write(s->plain_dma, addr, buf, len,
                                MEMTXATTRS_UNSPECIFIED);
    }
    return pci_dma_write(PCI_DEVICE(s), addr, buf, len);
}

/*
 * DMA'd values. On the plain route memory holds the driver's own big-endian
 * layout, whose 16-bit fields sit in the other half of each 32-bit word from
 * where the swap-corrected view puts them -- the byte-lane halfword
 * correction, offset xor 2. On the slot route the swap has already turned
 * the layout little-endian at the model's own offsets.
 */
static uint32_t bcm570x_dma_ld32(Bcm570xState *s, const uint8_t *p)
{
    return s->plain_dma ? ldl_be_p(p) : ldl_le_p(p);
}

static uint16_t bcm570x_dma_ld16(Bcm570xState *s, const uint8_t *p)
{
    if (s->plain_dma) {
        return lduw_be_p((const uint8_t *)((uintptr_t)p ^ 2));
    }
    return lduw_le_p(p);
}

static void bcm570x_dma_st32(Bcm570xState *s, uint8_t *p, uint32_t v)
{
    if (s->plain_dma) {
        stl_be_p(p, v);
    } else {
        stl_le_p(p, v);
    }
}

static void bcm570x_dma_st16(Bcm570xState *s, uint8_t *p, uint16_t v)
{
    if (s->plain_dma) {
        stw_be_p((uint8_t *)((uintptr_t)p ^ 2), v);
    } else {
        stw_le_p(p, v);
    }
}

static bool bcm570x_frame_swap(Bcm570xState *s, bool *bswap, bool *wswap)
{
    uint32_t misc = pci_get_long(PCI_DEVICE(s)->config + BCM570X_PCI_MISC_CTL);

    if (s->plain_dma) {
        /*
         * Verbatim payload is the bounded Bridge-machine model behavior.
         * The card's clear-data hardware transformation remains unknown.
         */
        *bswap = false;
        *wswap = false;
        return false;
    }

    /*
     * Preserve the existing off-Bridge interpretation of MISC_CTL: the
     * word-swap bit requests both byte and word swaps. The reviewed MODE_CTL
     * fallback is a separate model change.
     */
    *bswap = (misc & BCM570X_MISCCTL_ENDIAN_WORDSWAP) != 0;
    *wswap = *bswap;
    return *bswap;
}

/*
 * Read a frame fragment from host memory, applying the model's payload swap.
 * The ordered descriptor fetch immediately before this read establishes the
 * guest's payload writes, so the packet copy itself needs no second barrier.
 * Swapped payloads reverse whole 32-bit words at their physical alignment.
 */
static MemTxResult bcm570x_frame_dma_read(PCIDevice *pci, Bcm570xState *s,
                                          uint64_t phys, uint8_t *dst,
                                          unsigned int len)
{
    bool bswap, wswap;
    uint64_t astart;
    unsigned int lead, span;
    uint8_t tmp[BCM570X_MAX_FRAME + 8];
    MemTxResult result;

    if (!bcm570x_frame_swap(s, &bswap, &wswap)) {
        return dma_memory_read_relaxed(s->plain_dma ? s->plain_dma :
                                       pci_get_address_space(pci),
                                       phys, dst, len);
    }
    astart = phys & ~(uint64_t)3;
    lead = (unsigned int)(phys - astart);
    span = (lead + len + 3) & ~3u;
    if (span > sizeof(tmp)) {
        return dma_memory_read_relaxed(pci_get_address_space(pci),
                                       phys, dst, len);
    }
    result = dma_memory_read_relaxed(pci_get_address_space(pci),
                                     astart, tmp, span);
    if (result != MEMTX_OK) {
        return result;
    }
    bcm570x_swap_span(tmp, span, bswap, wswap);
    memcpy(dst, tmp + lead, len);
    return MEMTX_OK;
}

/*
 * The ordered completed-descriptor write after this payload copy publishes it
 * to the guest and supplies the required barrier on either DMA route. Swapped
 * payloads reverse the word-aligned span whole, including harmless padding the
 * driver does not read.
 */
static MemTxResult bcm570x_frame_dma_write(PCIDevice *pci, Bcm570xState *s,
                                           uint64_t phys, const uint8_t *src,
                                           unsigned int len)
{
    bool bswap, wswap;
    uint64_t astart;
    unsigned int lead, span;
    uint8_t tmp[BCM570X_MAX_FRAME + 8];

    if (!bcm570x_frame_swap(s, &bswap, &wswap)) {
        return dma_memory_write_relaxed(s->plain_dma ? s->plain_dma :
                                        pci_get_address_space(pci),
                                        phys, src, len);
    }
    astart = phys & ~(uint64_t)3;
    lead = (unsigned int)(phys - astart);
    span = (lead + len + 3) & ~3u;
    if (span > sizeof(tmp)) {
        return dma_memory_write_relaxed(pci_get_address_space(pci),
                                        phys, src, len);
    }
    memset(tmp, 0, span);
    memcpy(tmp + lead, src, len);
    bcm570x_swap_span(tmp, span, bswap, wswap);
    return dma_memory_write_relaxed(pci_get_address_space(pci),
                                    astart, tmp, span);
}

/*
 * DMA the status block to the host address the driver programmed through the
 * host-coalescing block, but only while that block is enabled -- the driver
 * gates it and reads the transmit/receive progress from there rather than a
 * register. The chip owns the block, so the model builds it whole each time.
 *
 * Scalar fields use big-endian accessors on the Bridge's plain DMA route and
 * little-endian accessors elsewhere. The MODE_CTL DMA-swap control is stored
 * for readback. Host address composition retains its MISC_CTL handling.
 */
static bool bcm570x_post_status_block(Bcm570xState *s, bool raise)
{
    uint8_t blk[BCM570X_STATUS_BLK_SIZE];
    uint64_t base;
    uint8_t next_tag;

    if (!(bcm570x_regfile_read(s, BCM570X_HCC_MODE) & BCM570X_HCC_MODE_ENABLE)) {
        return false;
    }

    base = bcm570x_compose_hostaddr(s,
               bcm570x_regfile_read(s, BCM570X_HCC_STATUSBLK_ADDR_HI),
               bcm570x_regfile_read(s, BCM570X_HCC_STATUSBLK_ADDR_LO));

    memset(blk, 0, sizeof(blk));
    next_tag = s->status_tag + 1;
    bcm570x_dma_st32(s, blk, BCM570X_STATFLAG_UPDATED);
    bcm570x_dma_st32(s, blk + BCM570X_STATUS_TAG_OFF, next_tag);
    bcm570x_dma_st16(s, blk + BCM570X_STATUS_TX_CONS_OFF, s->status_tx_cons);
    bcm570x_dma_st16(s, blk + BCM570X_STATUS_RX_PROD_OFF, s->status_rx_prod);
    if (bcm570x_dma_write_buf(s, base, blk, sizeof(blk)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: status block write at 0x%" PRIx64
                      " failed; update not posted\n", base);
        return false;
    }
    s->status_tag = next_tag;

    /*
     * The block now carries an update the driver has not taken. The line asserts
     * while that pending latch is set and interrupts are unmasked, and drops when
     * the driver acknowledges by writing the interrupt mailbox. The monotonic tag
     * is delivered in the block for the driver's own use, not to gate the line.
     * Reached only past the enable gate above, so a disabled host-coalescing
     * block raises no interrupt either. Returns whether the update reached the
     * host, which the bring-up shot needs to know not to spend itself on a
     * dropped update.
     */
    if (raise) {
        s->irq_pending = true;
        bcm570x_update_irq(s);
    }
    return true;
}

/* An update the driver is meant to be interrupted for. */
static bool bcm570x_dma_status_block(Bcm570xState *s)
{
    return bcm570x_post_status_block(s, true);
}


static void bcm570x_arm_status_tick(Bcm570xState *s);

/*
 * The coalescing block's own periodic status update. It carries whatever the
 * rings have and raises the line, which is what keeps the driver's watchdog
 * satisfied between bursts of traffic.
 */
static void bcm570x_status_tick(void *opaque)
{
    Bcm570xState *s = opaque;

    if (!(bcm570x_regfile_read(s, BCM570X_HCC_MODE) &
          BCM570X_HCC_MODE_ENABLE)) {
        return;
    }
    bcm570x_dma_status_block(s);
    bcm570x_arm_status_tick(s);
}

static void bcm570x_arm_status_tick(Bcm570xState *s)
{
    uint32_t us = bcm570x_regfile_read(s, BCM570X_HCC_STATS_TICKS);
    uint64_t ns = us ? (uint64_t)us * 1000 : BCM570X_STATUS_TICK_MAX_NS;

    timer_mod(&s->status_tick_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              MIN(ns, BCM570X_STATUS_TICK_MAX_NS));
}

/* The driver programs both coalescing tick budgets in microseconds. */
#define BCM570X_COAL_TICK_NS 1000

/* Retry later instead of spinning on an address the guest has not repaired. */
static void bcm570x_arm_coal_retry(QEMUTimer *timer, uint32_t ticks)
{
    uint64_t ns = ticks ? (uint64_t)ticks * BCM570X_COAL_TICK_NS :
                  BCM570X_COAL_TICK_NS;

    timer_mod(timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              MIN(ns, BCM570X_STATUS_TICK_MAX_NS));
}

/*
 * Flush a coalesced receive group: report every frame accumulated since the last
 * update through one status-block DMA and interrupt, and disarm the tick timer.
 * The status block always carries the current receive producer, so one update
 * tells the driver about the whole group.
 */
static void bcm570x_rx_coal_flush(Bcm570xState *s)
{
    uint32_t ticks;

    if (s->rx_coal_pending == 0) {
        return;
    }
    if (bcm570x_dma_status_block(s)) {
        s->rx_coal_pending = 0;
        timer_del(&s->coal_timer);
        return;
    }

    ticks = bcm570x_regfile_read(s, BCM570X_HCC_RX_COAL_TICKS);
    bcm570x_arm_coal_retry(&s->coal_timer, ticks);
}

static void bcm570x_coal_timer(void *opaque)
{
    bcm570x_rx_coal_flush(opaque);
}

/*
 * A received frame reached its buffer. Batch its status-block update the way the
 * host-coalescing block does: raise one interrupt per RX_MAX_COAL_BDS frames
 * (0x3c10) instead of one per frame, and arm the RX_COAL_TICKS (0x3c08) timer so
 * a partial final group still flushes within the driver's tick budget. A
 * max-BDs of zero or one is the un-coalesced case the model shipped with -- one
 * update per frame -- so a driver that programs neither register is unchanged.
 */
/*
 * The transmit side of the same arrangement. The driver programs how many
 * completed descriptors, or how long, it is prepared to wait before being told
 * about them, and reporting each one as it happens costs an interrupt per frame
 * for a saving the driver did not ask for.
 */
static void bcm570x_tx_coal_flush(Bcm570xState *s)
{
    uint32_t ticks;

    if (s->tx_coal_pending == 0) {
        return;
    }
    if (bcm570x_dma_status_block(s)) {
        s->tx_coal_pending = 0;
        timer_del(&s->tx_coal_timer);
        return;
    }

    ticks = bcm570x_regfile_read(s, BCM570X_HCC_TX_COAL_TICKS);
    bcm570x_arm_coal_retry(&s->tx_coal_timer, ticks);
}

static void bcm570x_tx_coal_timer(void *opaque)
{
    bcm570x_tx_coal_flush(opaque);
}

/*
 * Count the descriptors this doorbell completed and report the group once it
 * is as large as the driver asked for. The tick timer is what stops the tail
 * of a burst waiting for a descriptor that never comes: a count on its own
 * would hold the last few completions back for an interrupt that never
 * arrives, which is a stall wearing a saving's clothes.
 */
static void bcm570x_tx_coalesce(Bcm570xState *s,
                                unsigned int completed_descs)
{
    uint32_t max_bds = bcm570x_regfile_read(s, BCM570X_HCC_TX_MAX_COAL_BDS);
    uint32_t ticks = bcm570x_regfile_read(s, BCM570X_HCC_TX_COAL_TICKS);

    if (completed_descs == 0) {
        /*
         * A doorbell at the current consumer completes nothing. Bring-up still
         * leans on the status update arriving, so report it rather than folding
         * it into a group that may never fill.
         */
        bcm570x_dma_status_block(s);
        return;
    }
    s->tx_coal_pending += completed_descs;
    if (max_bds <= 1 || s->tx_coal_pending >= max_bds) {
        bcm570x_tx_coal_flush(s);
    } else if (ticks != 0 && !timer_pending(&s->tx_coal_timer)) {
        timer_mod(&s->tx_coal_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)ticks * BCM570X_COAL_TICK_NS);
    } else if (ticks == 0) {
        bcm570x_tx_coal_flush(s);
    }
}

static void bcm570x_rx_coalesce(Bcm570xState *s)
{
    uint32_t max_bds = bcm570x_regfile_read(s, BCM570X_HCC_RX_MAX_COAL_BDS);
    uint32_t ticks = bcm570x_regfile_read(s, BCM570X_HCC_RX_COAL_TICKS);

    s->rx_coal_pending++;
    if (max_bds <= 1 || s->rx_coal_pending >= max_bds) {
        bcm570x_rx_coal_flush(s);
    } else if (ticks != 0 && !timer_pending(&s->coal_timer)) {
        timer_mod(&s->coal_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)ticks * BCM570X_COAL_TICK_NS);
    } else if (ticks == 0) {
        bcm570x_rx_coal_flush(s);
    }
}

static bool bcm570x_rx_ready(Bcm570xState *s);

/*
 * Take the frames the peer still holds. The drain runs from a bottom half so
 * nothing is delivered underneath the register write that made room: the
 * guest resumes first, and the completion and its status update land the way
 * an unsolicited frame would.
 */
static void bcm570x_rx_flush(void *opaque)
{
    Bcm570xState *s = opaque;

    s->rx_flush_pending = false;
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

/*
 * Hold the frame at the peer's queue head. Whatever refused it -- an empty
 * producer ring, a full return ring, a DMA that failed on a bad address --
 * is repaired by the guest through a receive-ready register, and that write
 * is what retries it.
 */
static void bcm570x_defer_rx_flush(Bcm570xState *s)
{
    s->rx_flush_pending = true;
}

static void bcm570x_retry_rx_flush(Bcm570xState *s)
{
    if (s->rx_flush_pending && bcm570x_rx_ready(s)) {
        qemu_bh_schedule(s->rx_flush_bh);
    }
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
/*
 * Finish the checksums the driver left to the card. The lengths in the frame
 * were composed by the guest, so an IPv4 header shorter than its own minimum,
 * or lengths reaching past what arrived, would walk the helper off the end of
 * the buffer: decline the offload rather than trust them.
 */
static void bcm570x_tx_offload_checksum(uint8_t *frame, unsigned int len,
                                        uint16_t flags)
{
    unsigned int ihl, total;
    int csum = 0;

    if (len < 14 + 20 || lduw_be_p(frame + 12) != 0x0800) {
        return;
    }
    if ((frame[14] >> 4) != 4) {
        return;
    }
    ihl = (frame[14] & 0x0f) * 4;
    total = lduw_be_p(frame + 16);
    if (ihl < 20 || total < ihl || 14 + total > len) {
        return;
    }

    if (flags & BCM570X_TXBDFLAG_IP_CKSUM) {
        csum |= CSUM_IP;
    }
    if (flags & BCM570X_TXBDFLAG_TCP_UDP_CKSUM) {
        csum |= CSUM_TCP | CSUM_UDP;
    }
    net_checksum_calculate(frame, len, csum);
}

static void bcm570x_tx_replace_source(Bcm570xState *s, uint8_t *frame,
                                      unsigned int len, uint16_t flags)
{
    unsigned int slot;
    uint32_t hi, lo;

    if (!(flags & BCM570X_TXBDFLAG_INSERT_SRC_ADDR) || len < 12) {
        return;
    }

    slot = (flags & BCM570X_TXBDFLAG_CHOOSE_SRC_ADDR) >>
           BCM570X_TXBDFLAG_SRC_ADDR_SHIFT;
    hi = bcm570x_regfile_read(s, BCM570X_MAC_ADDR_0_HIGH +
                              slot * BCM570X_MAC_ADDR_STRIDE);
    lo = bcm570x_regfile_read(s, BCM570X_MAC_ADDR_0_LOW +
                              slot * BCM570X_MAC_ADDR_STRIDE);
    frame[6] = hi >> 8;
    frame[7] = hi;
    stl_be_p(frame + 8, lo);
}

typedef struct Bcm570xTxLsoLayout {
    unsigned int ihl;
    unsigned int thl;
    unsigned int hdr;
    unsigned int payload;
} Bcm570xTxLsoLayout;

static bool bcm570x_tx_lso_layout(const uint8_t *frame, unsigned int len,
                                  unsigned int mss,
                                  Bcm570xTxLsoLayout *layout)
{
    unsigned int ihl, thl, hdr, payload, ip_len;

    if (len < 14 + 20 + 20 || lduw_be_p(frame + 12) != 0x0800 ||
        (frame[14] >> 4) != 4 || frame[23] != 6) {
        return false;
    }
    ihl = (frame[14] & 0x0f) * 4;
    if (ihl < 20 || 14 + ihl + 20 > len) {
        return false;
    }
    ip_len = lduw_be_p(frame + 16);
    if ((lduw_be_p(frame + 20) & (IP_OFFMASK | IP_MF)) != 0 ||
        ip_len < ihl + 20 || 14 + ip_len != len) {
        return false;
    }
    thl = (frame[14 + ihl + 12] >> 4) * 4;
    hdr = 14 + ihl + thl;
    if (thl < 20 || ihl + thl > ip_len || mss == 0 ||
        hdr + mss > BCM570X_MAX_FRAME) {
        return false;
    }
    payload = ip_len - ihl - thl;
    if (payload <= mss) {
        return false;
    }

    if (layout) {
        layout->ihl = ihl;
        layout->thl = thl;
        layout->hdr = hdr;
        layout->payload = payload;
    }
    return true;
}

static void bcm570x_tx_pkt_release(void *context, void *base, size_t len)
{
    /* The fragment points at bcm570x_tx_kick()'s frame buffer. */
}

static bool bcm570x_tx_vnet_segment(Bcm570xState *s, uint8_t *frame,
                                    unsigned int len, unsigned int mss)
{
    struct virtio_net_hdr *vhdr;
    Bcm570xTxLsoLayout layout;
    bool sent = false;

    if (!s->has_vnet_hdr ||
        !bcm570x_tx_lso_layout(frame, len, mss, &layout)) {
        return false;
    }

    net_tx_pkt_reset(s->tx_pkt, bcm570x_tx_pkt_release, NULL);
    if (!net_tx_pkt_add_raw_fragment(s->tx_pkt, frame, len) ||
        !net_tx_pkt_parse(s->tx_pkt) ||
        !net_tx_pkt_build_vheader(s->tx_pkt, true, true, mss)) {
        goto out;
    }
    vhdr = net_tx_pkt_get_vhdr(s->tx_pkt);
    if ((vhdr->gso_type & ~VIRTIO_NET_HDR_GSO_ECN) !=
            VIRTIO_NET_HDR_GSO_TCPV4 ||
        vhdr->hdr_len != layout.hdr) {
        goto out;
    }

    net_tx_pkt_update_ip_checksums(s->tx_pkt);
    sent = net_tx_pkt_send(s->tx_pkt, qemu_get_queue(s->nic));

out:
    net_tx_pkt_reset(s->tx_pkt, bcm570x_tx_pkt_release, NULL);
    return sent;
}

static void bcm570x_send_packet(Bcm570xState *s, const uint8_t *frame,
                                size_t len)
{
    struct virtio_net_hdr vhdr = { };
    struct iovec iov[] = {
        {
            .iov_base = &vhdr,
            .iov_len = sizeof(vhdr),
        }, {
            .iov_base = (void *)frame,
            .iov_len = len,
        },
    };

    if (s->has_vnet_hdr) {
        qemu_sendv_packet(qemu_get_queue(s->nic), iov, G_N_ELEMENTS(iov));
    } else {
        qemu_send_packet(qemu_get_queue(s->nic), frame, len);
    }
}

/*
 * Cut one oversized TCP buffer into segments the peer will accept. The guest
 * hands down a single frame holding many segments' worth of payload; each piece
 * goes out with its own copy of the headers, its own sequence number, and the
 * finishing flags only on the last. Transmitting the buffer whole instead puts
 * a frame on the wire no receiver takes, and the sender then waits out a
 * retransmission timeout for every one of them.
 *
 * Anything that does not look like the IPv4 TCP frame this is for -- a short
 * header, lengths that do not fit inside what arrived, a segment size that
 * would not divide it -- is sent unchanged rather than guessed at.
 */
static void bcm570x_tx_segment(Bcm570xState *s, uint8_t *frame,
                               unsigned int len, unsigned int mss,
                               uint16_t flags)
{
    Bcm570xTxLsoLayout layout;
    unsigned int ihl, thl, hdr, payload, off, ip_id;
    uint8_t seg[BCM570X_MAX_FRAME];
    uint32_t seq;

    if (!bcm570x_tx_lso_layout(frame, len, mss, &layout)) {
        goto send_whole;
    }
    ihl = layout.ihl;
    thl = layout.thl;
    hdr = layout.hdr;
    payload = layout.payload;

    seq = ldl_be_p(frame + 14 + ihl + 4);
    ip_id = lduw_be_p(frame + 18);

    for (off = 0; off < payload; off += mss) {
        unsigned int piece = MIN(mss, payload - off);
        bool last = (off + piece >= payload);

        memcpy(seg, frame, hdr);
        memcpy(seg + hdr, frame + hdr + off, piece);

        stw_be_p(seg + 16, ihl + thl + piece);      /* IP total length */
        stw_be_p(seg + 18, ip_id + (off / mss));    /* a fresh identifier */
        stl_be_p(seg + 14 + ihl + 4, seq + off);    /* sequence number */
        if (!last) {
            /* Only the final piece finishes the sender's message. */
            seg[14 + ihl + 13] &= ~0x09;            /* clear FIN and PSH */
        }
        stw_be_p(seg + 24, 0);                      /* checksums to recompute */
        stw_be_p(seg + 14 + ihl + 16, 0);
        net_checksum_calculate(seg, hdr + piece, CSUM_IP | CSUM_TCP);

        bcm570x_send_packet(s, seg, hdr + piece);
    }
    return;

send_whole:
    if (flags & (BCM570X_TXBDFLAG_IP_CKSUM | BCM570X_TXBDFLAG_TCP_UDP_CKSUM)) {
        bcm570x_tx_offload_checksum(frame, len, flags);
    }
    bcm570x_send_packet(s, frame, len);
}

static void bcm570x_tx_rollback_to_frame(Bcm570xState *s,
                                         uint16_t frame_start,
                                         unsigned int completed_descs)
{
    s->tx_cons = frame_start;
    s->tx_busy = false;
    if (completed_descs) {
        s->status_tx_cons = frame_start;
        bcm570x_tx_coalesce(s, completed_descs);
    }
    if (s->backend_rx_deferred) {
        s->backend_rx_deferred = false;
        qemu_bh_schedule(s->rx_flush_bh);
    }
}

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
    unsigned int completed_descs = 0;
    uint16_t frame_flags = 0;
    uint16_t frame_start;
    unsigned int frame_mss = 0;
    bool frame_dropped = false;

    if (ring_len == 0 || prod >= ring_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: transmit producer %u out of range "
                      "(ring length %u)\n", prod, ring_len);
        return;
    }
    if (s->tx_busy) {
        return;
    }

    ring_base = bcm570x_compose_hostaddr(s,
                    bcm570x_sram_read(s, BCM570X_SRAM_SEND_RING_RCB +
                                      BCM570X_RCB_HOSTADDR_HI),
                    bcm570x_sram_read(s, BCM570X_SRAM_SEND_RING_RCB +
                                      BCM570X_RCB_HOSTADDR_LO));

    s->tx_busy = true;
    frame_start = s->tx_cons;
    while (s->tx_cons != prod && walked < ring_len) {
        uint8_t bd[BCM570X_TXBD_SIZE] = { 0 };
        uint64_t buf_addr;
        uint16_t len, flags;

        if (bcm570x_dma_read_buf(s,
                ring_base + (uint64_t)s->tx_cons * BCM570X_TXBD_SIZE,
                bd, sizeof(bd)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: transmit descriptor %u read failed; "
                          "ring walk stopped\n", s->tx_cons);
            bcm570x_tx_rollback_to_frame(s, frame_start, completed_descs);
            return;
        }
        buf_addr = ((uint64_t)bcm570x_dma_ld32(s, bd + BCM570X_TXBD_ADDR_HI)
                    << 32) |
                   bcm570x_dma_ld32(s, bd + BCM570X_TXBD_ADDR_LO);
        len = bcm570x_dma_ld16(s, bd + BCM570X_TXBD_LEN);
        flags = bcm570x_dma_ld16(s, bd + BCM570X_TXBD_FLAGS);

        if (!frame_dropped && frame_len + len <= sizeof(frame)) {
            if (bcm570x_frame_dma_read(pci, s, buf_addr, frame + frame_len,
                                       len) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bcm570x: transmit payload read at 0x%" PRIx64
                              " failed; frame left ready to retry\n", buf_addr);
                bcm570x_tx_rollback_to_frame(s, frame_start,
                                             completed_descs);
                return;
            }
            frame_len += len;
        } else if (!frame_dropped) {
            /*
             * The frame overran the buffer. Drop the whole frame -- stay in the
             * dropping state across the remaining fragments so a later fragment
             * is not mistaken for a fresh, in-range packet -- and resume at the
             * next END.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: transmit frame exceeds %u bytes, dropped\n",
                          (unsigned int)sizeof(frame));
            frame_dropped = true;
        }

        frame_flags |= flags;
        if (frame_mss == 0) {
            frame_mss = bcm570x_dma_ld16(s, bd + BCM570X_TXBD_MSS);
        }
        if (flags & BCM570X_TXBDFLAG_END) {
            if (!frame_dropped && frame_len) {
                if (frame_mss == 0) {
                    bcm570x_tx_replace_source(s, frame, frame_len,
                                              frame_flags);
                }
                if (!bcm570x_tx_vnet_segment(s, frame, frame_len, frame_mss)) {
                    bcm570x_tx_segment(s, frame, frame_len, frame_mss,
                                       frame_flags);
                }
            }
            frame_len = 0;
            frame_flags = 0;
            frame_mss = 0;
            frame_dropped = false;
        }

        s->tx_cons = (s->tx_cons + 1) % ring_len;
        walked++;
        if (flags & BCM570X_TXBDFLAG_END) {
            frame_start = s->tx_cons;
            completed_descs = walked;
        }
    }
    if (s->tx_cons != frame_start) {
        bcm570x_tx_rollback_to_frame(s, frame_start, completed_descs);
        return;
    }
    s->tx_busy = false;

    s->status_tx_cons = prod;
    bcm570x_tx_coalesce(s, completed_descs);
    /* Publish transmit completion before releasing a synchronous reply. */
    if (s->backend_rx_deferred) {
        s->backend_rx_deferred = false;
        qemu_bh_schedule(s->rx_flush_bh);
    }
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
        /* The reset bit self-clears; the rest of the write persists. */
        val &= ~BCM570X_MODE_RESET;
        break;
    case BCM570X_MAC_STATUS:
        /* Write one to clear: the bits written are the ones acknowledged. */
        bcm570x_regfile_write(s, off, bcm570x_regfile_read(s, off) & ~val);
        return;
    case BCM570X_HCC_MODE:
        val &= ~BCM570X_MODE_RESET;
        bcm570x_regfile_write(s, off, val);
        if (val & BCM570X_HCC_MODE_ENABLE) {
            bcm570x_arm_status_tick(s);
            bcm570x_retry_rx_flush(s);
        } else {
            timer_del(&s->status_tick_timer);
        }
        return;
    case BCM570X_BUFMGR_MODE:
        /*
         * The reset bit self-clears like any *_MODE register, and so does the
         * ENABLE request the IRIX driver polls for clear (its divergence from
         * tg3/bge). ATTN_ENABLE and the rest persist for read-back.
         */
        val &= ~(BCM570X_MODE_RESET | BCM570X_BUFMGR_MODE_ENABLE);
        break;
    case BCM570X_MBX_IRQ0_LO:
        /*
         * Writing 1 masks and acknowledges the current update. A later update
         * posted while masked remains pending, so writing 0 to re-enable makes
         * the level reassert. The IRIX driver also uses a write-0-only runtime
         * acknowledgement; when already unmasked, retain that observed path by
         * clearing the pending update.
         */
        if (val & 1) {
            s->irq_pending = false;
            s->irq_masked = true;
        } else {
            if (s->irq_masked) {
                s->irq_masked = false;
            } else {
                s->irq_pending = false;
            }
            bcm570x_retry_rx_flush(s);
        }
        bcm570x_regfile_write(s, off, val);
        bcm570x_update_irq(s);
        return;
    case BCM570X_MBX_TX_PROD_LO:
        /* Store the producer for read-back, then send up to it. */
        bcm570x_regfile_write(s, off, val);
        bcm570x_tx_kick(s, val);
        return;
    case BCM570X_RX_JUMBO_RCB_MAXLEN_FLAGS:
        /*
         * Max_Len (bits 31-16) is unused for the jumbo ring and never read
         * back for it; only Disable Ring and Extended RX Enable do anything,
         * and both are read directly out of the stored word where needed.
         * The extended 64-byte descriptor format is not modelled -- flag it
         * once, here at the point the guest asks for it, rather than on
         * every frame that finds the ring unavailable because of it.
         */
        if (val & BCM570X_RCB_FLAG_EXT_RX_ENABLE) {
            qemu_log_mask(LOG_UNIMP,
                          "bcm570x: extended jumbo receive descriptors "
                          "(0x2448 bit 0) are not modelled; the jumbo ring "
                          "is treated as unavailable\n");
        }
        break;
    case BCM570X_MBX_RX_JUMBO_PROD_LO: {
        /* Posting jumbo-ring buffers; guard the index like the standard one. */
        if ((val & 0xffff) >= BCM570X_RX_JUMBO_RING_LEN) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: receive jumbo producer %u out of range "
                          "(ring length %u)\n", val & 0xffff,
                          BCM570X_RX_JUMBO_RING_LEN);
            return;
        }
        bcm570x_regfile_write(s, off, val);
        /* Room where there was none: take anything the peer had waiting. */
        bcm570x_retry_rx_flush(s);
        return;
    }
    case BCM570X_MBX_RX_STD_PROD_LO: {
        /* Posting standard-ring buffers; guard the index like the TX one. */
        uint16_t std_len =
            bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_MAXLEN_FLAGS) >> 16;

        if (std_len == 0 || (val & 0xffff) >= BCM570X_RX_STD_RING_LEN) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: receive standard producer %u out of range "
                          "(ring length %u)\n", val & 0xffff,
                          BCM570X_RX_STD_RING_LEN);
            return;
        }
        bcm570x_regfile_write(s, off, val);
        /*
         * Bring-up completes with the driver enabling the engines, unmasking
         * interrupts, and posting receive buffers; only then does it wait for
         * the host-coalescing engine's first status-block DMA and interrupt
         * before declaring the interface running. With no traffic the runtime
         * status paths (transmit doorbell, received frame) never fire, so post
         * one status update here, on the first standard-ring doorbell past the
         * enable. One shot suffices; the runtime paths carry it afterwards.
         *
         * The shot is spent only when the update actually reaches the host. The
         * driver posts its first buffers before it enables the host-coalescing
         * block, so an unconditional shot lands while that block is still off,
         * is dropped, and leaves nothing to retry: the driver waits for an
         * update that never comes and re-runs bring-up about once a second.
         * Keeping the flag clear until a real update lands retries on the next
         * doorbell, which is past the enable.
         */

        /* Room where there was none: take anything the peer had waiting. */
        bcm570x_retry_rx_flush(s);
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
        /*
         * Taking a completion frees the entry it sat in, and the back end may be
         * holding a frame this device refused while the ring was full. Nothing
         * else will offer it again: a refusal leaves the frame queued until the
         * device says it is ready, and the only other place that says so is the
         * standard-ring doorbell -- which a driver that is draining rather than
         * refilling need not write. Without this the interface takes one ring's
         * worth and then goes quiet for good, which looks like the link dying
         * partway through a transfer.
         */
        bcm570x_retry_rx_flush(s);
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
    uint32_t o = off & (BCM570X_REG_FILE_SIZE - 1);
    uint32_t val;

    switch (o) {
    case BCM570X_MI_STS:
        val = bcm570x_phy_link_up(&s->phy) ? BCM570X_MISTS_LINK : 0;
        break;
    case BCM570X_FW_PROGRESS:
        val = BCM570X_FW_PROGRESS_INIT;
        break;
    default:
        val = bcm570x_regfile_read(s, off);
        break;
    }

    return val;
}

static uint32_t bcm570x_config_read(PCIDevice *dev, uint32_t addr, int len);
static void bcm570x_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                 int len);

/* SRAM offset the BAR0 window at 0x8000 exposes for a given BAR address. */
static uint32_t bcm570x_window_sram_off(Bcm570xState *s, hwaddr addr)
{
    uint32_t base = s->mem_win_base & ~(BCM570X_SRAM_SIZE - 1);

    return base + (uint32_t)(addr - BCM570X_REG_FILE_SIZE);
}

static uint64_t bcm570x_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    Bcm570xState *s = opaque;

    if (size == 1) {
        uint32_t val = bcm570x_reg_read(s, addr & ~3);

        return extract32(val, (addr & 3) * 8, 8);
    }
    if (addr < BCM570X_PCI_CFG_ALIAS) {
        return bcm570x_config_read(PCI_DEVICE(s), addr, size);
    }
    if (addr < BCM570X_REG_FILE_SIZE) {
        return bcm570x_reg_read(s, addr);
    }
    return bcm570x_sram_read(s, bcm570x_window_sram_off(s, addr));
}

static void bcm570x_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Bcm570xState *s = opaque;

    if (addr < BCM570X_PCI_CFG_ALIAS) {
        bcm570x_config_write(PCI_DEVICE(s), addr, val, size);
        return;
    }
    if (addr < BCM570X_REG_FILE_SIZE) {
        bcm570x_reg_write(s, addr, val);
        return;
    }
    bcm570x_sram_write(s, bcm570x_window_sram_off(s, addr), val);
}

static bool bcm570x_bar0_accepts(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    if (size == 4) {
        return !(addr & 3);
    }

    return size == 1 && !is_write &&
           addr >= BCM570X_MAC_ADDR_0_HIGH &&
           addr < BCM570X_MAC_ADDR_0_LOW + 4;
}

/*
 * The register file and the SRAM are 32-bit-accessed. impl.min/max pin the
 * width so the memory core never chops a wide access into repeated calls
 * (which would run a register's side effects twice). The only narrow access
 * the chip accepts is a read from the first station-address pair.
 *
 * The declared endianness is DEVICE_LITTLE_ENDIAN: the part is little-endian
 * native (the ISP1040 precedent for these controllers), so the register file
 * presents its native order and any host bridge composes the endianness for
 * the guest. A guest dword read of a register returns the value written,
 * pinned by the device's golden byte-layout qtest on a generic host. The DMA
 * byte order is handled separately in bcm570x_tx_kick and
 * bcm570x_dma_status_block.
 */
static const MemoryRegionOps bcm570x_bar0_ops = {
    .read = bcm570x_bar0_read,
    .write = bcm570x_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .accepts = bcm570x_bar0_accepts,
    },
    .impl = {
        .min_access_size = 1,
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
 * Whether the jumbo ring can take a frame at all, independent of whether one
 * is posted: it needs a host base and must not be disabled (PG105 Table 341,
 * page 438). Extended RX Enable asks for the 64-byte descriptor format this
 * model does not parse, so a ring asking for it is treated the same as a
 * disabled one -- oversize frames are dropped rather than misread as the
 * plain 32-byte format. Max_Len (bits 31-16) plays no part in this: the
 * manual says hardware ignores it for the jumbo ring.
 */
static bool bcm570x_jumbo_ring_enabled(Bcm570xState *s)
{
    uint32_t flags =
        bcm570x_regfile_read(s, BCM570X_RX_JUMBO_RCB_MAXLEN_FLAGS);
    uint64_t jumbo_base = bcm570x_compose_hostaddr(s,
                     bcm570x_regfile_read(s, BCM570X_RX_JUMBO_RCB_HADDR_HI),
                     bcm570x_regfile_read(s, BCM570X_RX_JUMBO_RCB_HADDR_LO));

    return jumbo_base != 0 &&
           !(flags & BCM570X_RCB_FLAG_DISABLE_RING) &&
           !(flags & BCM570X_RCB_FLAG_EXT_RX_ENABLE);
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

    /*
     * A completion the driver has not read yet must not be overwritten. The
     * driver publishes how far it has consumed the return ring; when the next
     * completion would land on that entry the ring is full, and a frame is
     * refused rather than written over one still owed to the driver -- which it
     * would otherwise follow to a buffer already recycled, faulting somewhere
     * that looks nothing like this device and only once the ring has wrapped.
     */
    uint16_t ret_cons =
        bcm570x_regfile_read(s, BCM570X_MBX_RX_CONS0_LO) & 0xffff;

    /*
     * Either ring having a buffer posted is enough to take a frame; which one
     * it goes to depends on how big it turns out to be, which is not known
     * until it arrives.
     */
    uint16_t jumbo_prod =
        bcm570x_regfile_read(s, BCM570X_MBX_RX_JUMBO_PROD_LO) & 0xffff;
    bool posted = (std_len != 0 && std_prod != s->rx_std_cons) ||
                  (bcm570x_jumbo_ring_enabled(s) &&
                   jumbo_prod != s->rx_jumbo_cons);

    return ret_len != 0 && posted &&
           (bcm570x_regfile_read(s, BCM570X_HCC_MODE) &
            BCM570X_HCC_MODE_ENABLE) &&
           (uint16_t)((s->status_rx_prod + 1) % ret_len) != ret_cons;
}

static bool bcm570x_can_receive(NetClientState *nc)
{
    Bcm570xState *s = qemu_get_nic_opaque(nc);

    if (s->tx_busy) {
        s->backend_rx_deferred = true;
        return false;
    }
    /*
     * Let an offered frame reach bcm570x_receive(), which returns zero and
     * records the deferred flush when the hardware is not ready. can_receive
     * is also called speculatively, so recording work here would let an empty
     * poll consume a later interrupt acknowledgement.
     */
    return true;
}

/*
 * The address filter: accept broadcast and multicast, and unicast addressed to
 * our station. The station address is the one handed through the SRAM mailbox
 * (the device's network configuration); a guest that reprograms the MAC filter
 * registers to a different address is not modelled here.
 */
/*
 * The address the part filters on is the one in MAC address filter slot 0, which
 * the driver programs -- not the address the command line gave the device. The
 * two are the same only until a driver installs its own, and IRIX tg installs
 * the system Ethernet address over whatever the card came up with. Filtering on
 * the command-line address instead dropped every reply the peer sent back to the
 * address the guest had actually announced, silently, since a rejected frame is
 * indistinguishable from one that never arrived.
 */
static bool bcm570x_frame_accepted(Bcm570xState *s, const uint8_t *buf,
                                   size_t size)
{
    uint32_t hi = bcm570x_regfile_read(s, BCM570X_MAC_ADDR_0_HIGH);
    uint32_t lo = bcm570x_regfile_read(s, BCM570X_MAC_ADDR_0_LOW);
    uint8_t addr[6];

    if (size < 6) {
        return false;
    }
    if (buf[0] & 0x01) {       /* broadcast and multicast */
        return true;
    }

    addr[0] = hi >> 8;
    addr[1] = hi;
    addr[2] = lo >> 24;
    addr[3] = lo >> 16;
    addr[4] = lo >> 8;
    addr[5] = lo;

    return memcmp(buf, addr, sizeof(addr)) == 0;
}

/* The one's-complement sum of a span, folded. A field that checks out sums
 * with its own checksum to 0xffff, which is the value the part reports. */
static uint16_t bcm570x_ones_sum(const uint8_t *buf, unsigned int len,
                                 uint32_t seed)
{
    uint32_t sum = net_checksum_add_cont(len, (uint8_t *)buf, 0) + seed;

    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/*
 * Check an arriving frame's checksums and report what they came to, so the
 * driver can pass the frame up without reading the payload a second time. The
 * part reports the sums it computed rather than a verdict; a field that checks
 * out sums to 0xffff. Anything not plainly IPv4 TCP or UDP is left unclaimed --
 * reporting a sum for something never summed would have the stack skip its own
 * pass on the strength of it.
 */
static uint32_t bcm570x_rx_offload_checksum(const uint8_t *frame,
                                            unsigned int len,
                                            uint16_t *flags)
{
    unsigned int ihl, total, l4len;
    uint16_t ip_sum, l4_sum;
    uint32_t pseudo;
    uint8_t proto;

    if (len < 34 || lduw_be_p(frame + 12) != 0x0800 ||
        (frame[14] >> 4) != 4) {
        return 0;
    }
    ihl = (frame[14] & 0x0f) * 4;
    total = lduw_be_p(frame + 16);
    if (ihl < 20 || total < ihl || 14 + total > len) {
        return 0;
    }

    ip_sum = bcm570x_ones_sum(frame + 14, ihl, 0);
    *flags |= BCM570X_RXBDFLAG_IP_CKSUM;

    proto = frame[23];
    if (proto != 6 && proto != 17) {
        return (uint32_t)ip_sum << 16;
    }

    /* The pseudo-header the transport sum is taken over, then the segment. */
    l4len = total - ihl;
    pseudo = 0;
    pseudo += lduw_be_p(frame + 26);
    pseudo += lduw_be_p(frame + 28);
    pseudo += lduw_be_p(frame + 30);
    pseudo += lduw_be_p(frame + 32);
    pseudo += proto;
    pseudo += l4len;
    l4_sum = bcm570x_ones_sum(frame + 14 + ihl, l4len, pseudo);
    *flags |= BCM570X_RXBDFLAG_TCP_UDP_CKSUM;

    return ((uint32_t)ip_sum << 16) | l4_sum;
}

/*
 * A frame from the backend. Not ready, or no buffer posted, returns 0 -- the
 * only answer that leaves the frame at the peer's queue head for later. A frame
 * the address filter drops returns the consumed count, never 0, so the peer is
 * not wedged waiting on a frame that will never be taken. Otherwise the frame is
 * DMA'd into the next posted standard-ring buffer, a completed descriptor is
 * written into the return ring, the receive producer advances, and the status
 * block reports it. Descriptor byte order and payload handling use the same
 * DMA helpers as the transmit path.
 */
static ssize_t bcm570x_receive(NetClientState *nc, const uint8_t *buf,
                               size_t size)
{
    Bcm570xState *s = qemu_get_nic_opaque(nc);
    size_t orig_size = size;
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t ret_len =
        bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                          BCM570X_RCB_MAXLEN_FLAGS) >> 16;
    uint64_t std_base, ret_base, buf_addr;
    uint8_t std_bd[BCM570X_RXBD_SIZE];
    uint8_t ret_bd[BCM570X_RXBD_SIZE];
    uint8_t padded[BCM570X_ETH_MIN_FRAME];
    unsigned int idx;
    uint32_t framelen;
    uint16_t rx_flags = BCM570X_RXBDFLAG_END;
    uint32_t csum;
    uint16_t std_size, std_prod, jumbo_prod;
    uint64_t jumbo_base;
    bool on_jumbo = false;
    bool jumbo_enabled;

    if (s->has_vnet_hdr) {
        if (size <= sizeof(struct virtio_net_hdr)) {
            return orig_size;
        }
        buf += sizeof(struct virtio_net_hdr);
        size -= sizeof(struct virtio_net_hdr);
    }

    if (!bcm570x_rx_ready(s)) {
        bcm570x_defer_rx_flush(s);
        return 0;
    }
    if (!bcm570x_frame_accepted(s, buf, size)) {
        return orig_size;
    }
    if (size > BCM570X_MAX_FRAME) {
        /*
         * Drop it rather than deliver part of it. Handing the guest a frame cut
         * short leaves it holding something that looks whole and is not, and
         * nothing anywhere says so -- the failure then surfaces wherever the
         * truncated payload is finally parsed, which is nowhere near here.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: received frame of %zu bytes exceeds the %u the "
                      "model carries, dropped\n",
                      size, (unsigned int)BCM570X_MAX_FRAME);
        return orig_size;
    }

    /*
     * Pad a short frame to the 60-byte Ethernet minimum. A frame on the wire is
     * padded by its sender, but a host backend can hand us a runt, and the
     * driver rejects a descriptor whose length falls below the minimum.
     */
    framelen = size;
    if (framelen < BCM570X_ETH_MIN_FRAME) {
        memcpy(padded, buf, framelen);
        memset(padded + framelen, 0, BCM570X_ETH_MIN_FRAME - framelen);
        buf = padded;
        framelen = BCM570X_ETH_MIN_FRAME;
    }

    /*
     * Pick the ring by what the frame will fit in. The standard ring's Max_Len
     * is the driver's statement of how much room its buffers hold; a frame
     * larger than that goes to the jumbo ring instead, whenever the jumbo
     * ring is enabled -- its own Max_Len plays no part (see the jumbo ring
     * comment above). A frame too big for the standard ring while the jumbo
     * ring is disabled or unconfigured is dropped rather than written past
     * the end of somebody's buffer.
     */
    std_size = bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_MAXLEN_FLAGS) >> 16;
    std_prod =
        bcm570x_regfile_read(s, BCM570X_MBX_RX_STD_PROD_LO) & 0xffff;
    jumbo_prod =
        bcm570x_regfile_read(s, BCM570X_MBX_RX_JUMBO_PROD_LO) & 0xffff;
    jumbo_enabled = bcm570x_jumbo_ring_enabled(s);
    jumbo_base = bcm570x_compose_hostaddr(s,
                     bcm570x_regfile_read(s, BCM570X_RX_JUMBO_RCB_HADDR_HI),
                     bcm570x_regfile_read(s, BCM570X_RX_JUMBO_RCB_HADDR_LO));

    if (framelen <= std_size) {
        if (std_prod == s->rx_std_cons) {
            bcm570x_defer_rx_flush(s);
            return 0;
        }
        std_base = bcm570x_compose_hostaddr(s,
                       bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_HADDR_HI),
                       bcm570x_regfile_read(s, BCM570X_RX_STD_RCB_HADDR_LO));
        idx = s->rx_std_cons;
    } else if (jumbo_enabled) {
        if (jumbo_prod == s->rx_jumbo_cons) {
            bcm570x_defer_rx_flush(s);
            return 0;
        }
        std_base = jumbo_base;
        idx = s->rx_jumbo_cons;
        rx_flags |= BCM570X_RXBDFLAG_JUMBO_RING;
        on_jumbo = true;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: received frame of %u bytes exceeds the "
                      "%u-byte standard buffer and the jumbo ring is "
                      "disabled or unconfigured, dropped\n",
                      framelen, std_size);
        return orig_size;
    }

    ret_base = bcm570x_compose_hostaddr(s,
                   bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                                     BCM570X_RCB_HOSTADDR_HI),
                   bcm570x_sram_read(s, BCM570X_SRAM_RX_RETURN_RCB +
                                     BCM570X_RCB_HOSTADDR_LO));

    /* The posted buffer's host address, from the chosen ring's descriptor. */
    memset(std_bd, 0, sizeof(std_bd));
    if (bcm570x_dma_read_buf(s,
                            std_base + (uint64_t)idx * BCM570X_RXBD_SIZE,
                            std_bd, sizeof(std_bd)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: receive descriptor %u read failed; "
                      "frame queued without consuming it\n", idx);
        bcm570x_defer_rx_flush(s);
        return 0;
    }
    buf_addr = ((uint64_t)bcm570x_dma_ld32(s, std_bd + BCM570X_RXBD_ADDR_HI)
                << 32) |
               bcm570x_dma_ld32(s, std_bd + BCM570X_RXBD_ADDR_LO);

    if (on_jumbo) {
        /*
         * The jumbo ring carries no ring-wide buffer size (its RCB's
         * Max_Len is unused); the driver states each buffer's size in the
         * descriptor it posts. A frame too big for that one descriptor is
         * dropped, mirroring the standard path's oversize drop above: a
         * log and a consumed-but-undelivered frame, with the descriptor
         * left for a frame that does fit it.
         */
        uint16_t buf_len =
            bcm570x_dma_ld32(s, std_bd + BCM570X_RXBD_IDX_LEN) & 0xffff;

        if (framelen > buf_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm570x: received frame of %u bytes exceeds the "
                          "%u-byte jumbo buffer posted at descriptor %u, "
                          "dropped\n", framelen, buf_len, idx);
            return orig_size;
        }
    }

    if (bcm570x_frame_dma_write(pci, s, buf_addr, buf,
                                framelen) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: receive payload write at 0x%" PRIx64
                      " failed; frame queued without consuming descriptor "
                      "%u\n", buf_addr, idx);
        bcm570x_defer_rx_flush(s);
        return 0;
    }
    csum = bcm570x_rx_offload_checksum(buf, framelen, &rx_flags);

    /* The completed descriptor uses the selected DMA route's scalar layout. */
    memset(ret_bd, 0, sizeof(ret_bd));
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_ADDR_HI, buf_addr >> 32);
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_ADDR_LO, buf_addr & 0xffffffff);
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_IDX_LEN,
             ((uint32_t)idx << 16) | ((framelen + BCM570X_ETH_FCS_LEN) & 0xffff));
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_TYPE_FLAGS, rx_flags);
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_CSUM, csum);
    bcm570x_dma_st32(s, ret_bd + BCM570X_RXBD_OPAQUE,
                     bcm570x_dma_ld32(s, std_bd + BCM570X_RXBD_OPAQUE));

    if (bcm570x_dma_write_buf(s,
                             ret_base + (uint64_t)s->status_rx_prod *
                             BCM570X_RXBD_SIZE, ret_bd,
                             sizeof(ret_bd)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x: receive completion %u write failed; "
                      "frame queued and descriptor not consumed\n",
                      s->status_rx_prod);
        bcm570x_defer_rx_flush(s);
        return 0;
    }

    if (on_jumbo) {
        s->rx_jumbo_cons =
            (s->rx_jumbo_cons + 1) % BCM570X_RX_JUMBO_RING_LEN;
    } else {
        s->rx_std_cons = (s->rx_std_cons + 1) % BCM570X_RX_STD_RING_LEN;
    }
    s->status_rx_prod = (s->status_rx_prod + 1) % ret_len;

    bcm570x_rx_coalesce(s);
    return orig_size;
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

static void bcm570x_cleanup_net(Bcm570xState *s)
{
    if (s->rx_flush_bh) {
        qemu_bh_delete(s->rx_flush_bh);
        s->rx_flush_bh = NULL;
    }
    if (s->nic) {
        net_tx_pkt_reset(s->tx_pkt, bcm570x_tx_pkt_release, NULL);
        net_tx_pkt_uninit(s->tx_pkt);
        s->tx_pkt = NULL;
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
}

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

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_bcm570x_info, &s->conf,
                          object_get_typename(OBJECT(dev)), ds->id,
                          &ds->mem_reentrancy_guard, s);
    s->has_vnet_hdr = qemu_has_vnet_hdr(qemu_get_queue(s->nic)->peer);
    if (s->has_vnet_hdr) {
        qemu_set_vnet_hdr_len(qemu_get_queue(s->nic)->peer,
                              sizeof(struct virtio_net_hdr));
    }
    net_tx_pkt_init(&s->tx_pkt, 1);
    s->rx_flush_bh = qemu_bh_new_guarded(bcm570x_rx_flush, s,
                                          &ds->mem_reentrancy_guard);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    bcm570x_phy_set_link_down(&s->phy, qemu_get_queue(s->nic)->link_down);

    /*
     * Version 6 streams carried the PHY as a standalone qdev section. Keep a
     * load-only compatibility registration for that section while version 7
     * and later carry the same state inside the MAC section.
     */
    if (vmstate_register_with_alias_id(VMSTATE_IF(dev),
                                       VMSTATE_INSTANCE_ID_ANY,
                                       &vmstate_bcm570x_phy, &s->phy,
                                       -1, 0, errp) < 0) {
        bcm570x_cleanup_net(s);
        return;
    }

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
            vmstate_unregister(VMSTATE_IF(dev), &vmstate_bcm570x_phy,
                               &s->phy);
            bcm570x_cleanup_net(s);
            return;
        }
    }

    /*
     * The DMA route decides the model's regime; see plain_dma. Resolved once
     * here -- the device cannot move buses after realize.
     */
    s->plain_dma = sgi_bridge_plain_dma_as(dev);
}

static void bcm570x_exit(PCIDevice *dev)
{
    Bcm570xState *s = BCM570X(dev);

    vmstate_unregister(VMSTATE_IF(dev), &vmstate_bcm570x_phy, &s->phy);
    bcm570x_cleanup_net(s);
    timer_del(&s->coal_timer);
    timer_del(&s->tx_coal_timer);
    timer_del(&s->status_tick_timer);
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
    s->rx_jumbo_cons = 0;
    s->tx_busy = false;
    s->irq_masked = true;
    s->irq_pending = false;
    s->status_kick_done = false;
    s->status_tag = 0;
    s->rx_coal_pending = 0;
    s->tx_coal_pending = 0;
    if (s->rx_flush_bh) {
        qemu_bh_cancel(s->rx_flush_bh);
    }
    bcm570x_reset_rx_queue(s);
    timer_del(&s->coal_timer);
    timer_del(&s->tx_coal_timer);
    timer_del(&s->status_tick_timer);
    bcm570x_phy_reset(&s->phy);
    bcm570x_load_station_address(s);
    bcm570x_update_irq(s);
}

/*
 * The migration stream carries the interrupt's cause and its masks but not the
 * wire itself. Re-derive the line on the destination from the restored state so
 * it asserts INTA exactly when the source did, and so PCISTATE.INTR_NOT_ACTIVE
 * and the wire agree. The PHY state is nested in this device's migration
 * section.
 */
static int bcm570x_post_load(void *opaque, int version_id)
{
    Bcm570xState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* The PHY migrates its cable state; NetClientState does not. */
    qemu_get_queue(s->nic)->link_down = s->phy.link_down;
    if (version_id < 6) {
        s->tx_coal_pending = 0;
    }
    bcm570x_update_irq(s);
    /*
     * Version 6 carries the timers themselves. Older streams carried only the
     * receive pending count, so report that count immediately. Rebuild any
     * missing timer from the restored register state so a malformed or older
     * stream cannot strand live work.
     */
    if (s->rx_coal_pending != 0 && !timer_pending(&s->coal_timer)) {
        uint32_t ticks = bcm570x_regfile_read(s, BCM570X_HCC_RX_COAL_TICKS);

        timer_mod(&s->coal_timer, version_id < 6 ? now :
                  now + (int64_t)ticks * BCM570X_COAL_TICK_NS);
    }
    if (s->tx_coal_pending != 0 && !timer_pending(&s->tx_coal_timer)) {
        uint32_t ticks = bcm570x_regfile_read(s, BCM570X_HCC_TX_COAL_TICKS);

        timer_mod(&s->tx_coal_timer,
                  now + (int64_t)ticks * BCM570X_COAL_TICK_NS);
    }
    if ((bcm570x_regfile_read(s, BCM570X_HCC_MODE) &
         BCM570X_HCC_MODE_ENABLE) && !timer_pending(&s->status_tick_timer)) {
        bcm570x_arm_status_tick(s);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm570x = {
    .name = "bcm570x",
    .version_id = 9,
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
        VMSTATE_UINT16(rx_jumbo_cons, Bcm570xState),
        VMSTATE_BOOL_V(irq_masked, Bcm570xState, 2),
        VMSTATE_BOOL_V(irq_pending, Bcm570xState, 2),
        VMSTATE_BOOL_V(status_kick_done, Bcm570xState, 3),
        VMSTATE_UINT8_V(status_tag, Bcm570xState, 3),
        VMSTATE_UINT16_V(rx_coal_pending, Bcm570xState, 4),
        /* Version 5 carried two superseded DMA-warning latches. */
        VMSTATE_UNUSED_V(5, 2),
        VMSTATE_UINT16_V(tx_coal_pending, Bcm570xState, 6),
        VMSTATE_TIMER_V(coal_timer, Bcm570xState, 6),
        VMSTATE_TIMER_V(tx_coal_timer, Bcm570xState, 6),
        VMSTATE_TIMER_V(status_tick_timer, Bcm570xState, 6),
        VMSTATE_STRUCT(phy, Bcm570xState, 7, vmstate_bcm570x_phy,
                       Bcm570xPhyState),
        /* Version 8 carried the superseded receive-unmask latch. */
        VMSTATE_UNUSED_V(8, 1),
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
    timer_init_ns(&s->coal_timer, QEMU_CLOCK_VIRTUAL, bcm570x_coal_timer, s);
    timer_init_ns(&s->tx_coal_timer, QEMU_CLOCK_VIRTUAL,
                  bcm570x_tx_coal_timer, s);
    timer_init_ns(&s->status_tick_timer, QEMU_CLOCK_VIRTUAL,
                  bcm570x_status_tick, s);
}

static void bcm570x_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = bcm570x_realize;
    pc->exit = bcm570x_exit;
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
