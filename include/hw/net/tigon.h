/*
 * Alteon Tigon 2 Gigabit Ethernet controller (the SGI IRIS "eg" board).
 *
 * The chip is a generic Alteon part (also sold as the AceNIC, Netgear GA620,
 * 3Com 3c985 and DEC PN9000SX); its PCI identity is a set of QOM properties
 * rather than a baked-in type, so the SGI board is one property instantiation
 * of the same model. See docs/datasheets/tigon.md and docs/plans/tigon-model.md
 * in the origami project tree for the hardware contract this models.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_TIGON_H
#define HW_NET_TIGON_H

#include "hw/pci/pci_device.h"
#include "net/net.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_TIGON "tigon"
OBJECT_DECLARE_SIMPLE_TYPE(TigonState, TIGON)

/*
 * The register file and SRAM window are reached through one 16 KB memory BAR
 * (if_tireg.h: "All registers are accessed through a 16K shared memory
 * block"). The register block is the first 2 KB; the 2 KB SRAM window sits at
 * 0x800.
 */
#define TIGON_BAR_SIZE      0x4000
#define TIGON_REG_SIZE      0x800
#define TIGON_SRAM_WINDOW   0x800
#define TIGON_SRAM_WIN_SIZE 0x800

/* The general communication registers span 0x600..0x6FC: 64 dwords. */
#define TIGON_GCR_REGS      64
/* The command ring is NIC-resident at 0x700..0x7FC: 64 dwords. */
#define TIGON_CMD_RING_CNT  64

/* Compare before adding so a restored near-limit count cannot wrap. */
static inline bool tigon_coal_count_reaches(uint32_t count,
                                            uint32_t completed,
                                            uint32_t threshold)
{
    return count >= threshold || completed >= threshold - count;
}

/* The bit-banged serial-EEPROM (24Cxx I2C slave) that holds the MAC. */
typedef struct TigonEeprom {
    bool last_clk;      /* EE_CLK level at the previous MISC_LOCAL_CTL write */
    bool last_sda;      /* master data line (EE_DOUT while EE_TXEN) before */
    uint8_t state;      /* TIGON_EE_* phase */
    uint8_t bitcnt;     /* bit index within the byte in flight (0..8) */
    uint8_t shift;      /* byte shifting in (receive) or out (read) */
    uint8_t bytenum;    /* transaction byte: 0 control, 1 addr-hi, 2 addr-lo */
    bool read_mode;     /* the control byte selected a read (0xA1) */
    uint16_t addr;      /* latched EEPROM byte address */
    bool din;           /* level the slave drives back on EE_DIN */
} TigonEeprom;

struct TigonState {
    PCIDevice parent_obj;

    MemoryRegion bar;       /* 16 KB container BAR */
    MemoryRegion reg;       /* register block, 0..0x7FF */
    MemoryRegion sram_win;  /* 2 KB SRAM window, 0x800..0xFFF */

    NICState *nic;
    QEMUBH *rx_flush_bh;
    NICConf conf;
    MACAddr station_addr;    /* active filter; conf holds the reset address */
    AddressSpace *descriptor_as; /* immutable, borrowed PCI DMA route */
    AddressSpace *payload_as;    /* immutable, borrowed payload DMA route */

    /* PCI identity, configurable so the SGI board is one instantiation. */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint8_t revision;       /* PCI config revision byte */
    uint8_t chip_rev;       /* runtime rev nibble reported in MISC_HOST_CTL */
    uint8_t port_count;
    char *media;
    uint32_t sram_size;

    /* Low control registers (0x040..0x07C). Only writable bits are stored. */
    uint32_t misc_host_ctl;
    uint32_t misc_local_ctl;
    uint32_t pci_state;
    /*
     * Card SRAM timing configuration. Stored and returned, and nothing
     * acts on it: the bit the driver sets selects synchronous SRAM, and
     * this model's SRAM is an array with no timing to select. Kept so a
     * read-modify-write sees what it wrote rather than zero.
     */
    uint32_t misc_conf;
    uint32_t win_base;

    /* Embedded-CPU control latches. */
    uint32_t cpu_state_a;
    uint32_t cpu_pc_a;
    uint32_t cpu_ctl_b;
    uint32_t cpu_pc_b;

    /* Interrupt mailbox hold-off latch (MB_HOSTINTR). */
    uint32_t mb_hostintr;

    /* Plain backing store for the GCR block and NIC-resident command ring. */
    uint32_t gcr[TIGON_GCR_REGS];
    uint32_t cmd_ring[TIGON_CMD_RING_CNT];

    /* Firmware/ring model. */
    bool cpu_a_released;
    bool gib_valid;
    bool firmware_up;
    bool promisc;
    uint64_t gib_base;

    /* Ring residency parsed from the GIB at firmware-up. */
    uint64_t ev_ring_base;
    uint64_t ev_prodidx_ptr;
    uint64_t tx_ring_base;
    bool tx_host_ring;
    uint64_t std_rx_ring_base;
    /*
     * The jumbo receive ring. The driver sizes the frames it will accept by
     * what the card can take, so a card that never serves this ring keeps the
     * guest at the standard maximum however it is configured.
     */
    uint64_t jumbo_rx_ring_base;
    uint64_t return_ring_base;
    uint32_t return_ring_max;
    uint64_t return_prodidx_ptr;
    uint64_t tx_considx_ptr;

    /* Producer/consumer positions the model itself maintains. */
    uint32_t event_prodidx;
    uint32_t return_prodidx;
    uint32_t tx_considx;
    uint32_t std_rx_considx;
    uint32_t std_rx_prodidx;
    uint32_t jumbo_rx_considx;
    uint32_t jumbo_rx_prodidx;
    uint32_t cmd_considx;

    /* Transient ordering state for synchronous backend receive callbacks. */
    bool defer_backend_rx;
    bool backend_rx_deferred;

    /* Internal pending-interrupt latch. */
    bool irq_asserted;
    /* Last level traced, so the trace records edges rather than calls. */
    bool irq_line_traced;

    /*
     * Interrupt coalescing. The driver asks for completions to accumulate up
     * to a count or a time, whichever comes first, so that one interrupt
     * covers many. Counted per direction; the line is shared.
     */
    uint32_t coal_rx_bds;
    uint32_t coal_tx_bds;
    QEMUTimer *coal_rx_timer;
    QEMUTimer *coal_tx_timer;

    /*
     * Link negotiation is asynchronous on real hardware: the firmware answers a
     * link-negotiation command with a link-state event a short time later. A
     * timer reproduces that delay so the event lands after the driver has begun
     * waiting for it, rather than racing ahead of the wait.
     */
    QEMUTimer *link_timer;

    TigonEeprom eeprom;

    /* Card SRAM: the downloaded firmware lands here and is never executed. */
    uint8_t *sram;
};

#endif /* HW_NET_TIGON_H */
