/*
 * Alteon Tigon 2 Gigabit Ethernet controller (the SGI IRIS "eg" board).
 *
 * The card does almost nothing at power-on: the host driver downloads firmware
 * into on-card SRAM, releases the embedded CPU, and thereafter talks to the
 * running firmware through a shared-memory producer/consumer ring interface in
 * host DRAM poked by a few mailbox doorbells. We do not execute the downloaded
 * firmware; we model what the operational firmware does as the driver observes
 * it (docs/datasheets/tigon.md, docs/plans/tigon-model.md).
 *
 * Register offsets, ring/descriptor layouts and the init sequence come from the
 * public NetBSD "ti" driver (refs/netbsd-src/sys/dev/pci/if_ti.c, if_tireg.h),
 * which implements the same published Tigon host interface the SGI eg driver is
 * built to. This is a clean-room implementation: no SGI/IRIX source was read.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/net/tigon.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/checksum.h"

#define TIGON_PCI_VENDOR_ID     0x10a9   /* Silicon Graphics */
#define TIGON_PCI_DEVICE_ID     0x0009   /* SGI Tigon */

/* Register offsets inside the BAR (if_tireg.h). */
#define TI_MISC_HOST_CTL        0x040
#define TI_MISC_LOCAL_CTL       0x044
#define TI_PCI_STATE            0x05c
#define TI_WINBASE              0x068
#define TI_WINDATA              0x06c
#define TI_CPU_STATE            0x140
#define TI_CPU_PROGRAM_COUNTER  0x144
#define TI_SRAM_ADDR            0x154
#define TI_SRAM_DATA            0x158
#define TI_CPU_CTL_B            0x240
#define TI_CPU_PROGRAM_COUNTER_B 0x244
#define TI_MB_HOSTINTR          0x504
#define TI_MB_CMDPROD_IDX       0x50c
#define TI_MB_SENDPROD_IDX      0x514
#define TI_MB_STDRXPROD_IDX     0x51c
#define TI_MB_JUMBORXPROD_IDX   0x524
#define TI_MB_MINIRXPROD_IDX    0x52c
#define TI_GCR_BASE             0x600
#define TI_GCR_END              0x6fc
#define TI_GCR_GENINFO_HI       0x608
#define TI_GCR_GENINFO_LO       0x60c
#define TI_GCR_OPMODE           0x618
#define TI_GCR_EVENTCONS_IDX    0x628
#define TI_GCR_CMDCONS_IDX      0x62c
#define TI_GCR_RX_COAL_TICKS    0x630
#define TI_GCR_TX_COAL_TICKS    0x634
#define TI_GCR_TX_MAX_COAL_BD   0x63c
#define TI_GCR_RX_MAX_COAL_BD   0x640
#define TI_GCR_MASK_INTRS       0x664
#define TI_GCR_GLINK_STAT       0x668
#define TI_GCR_LINK_STAT        0x66c
#define TI_GCR_RXRETURNCONS_IDX 0x680
#define TI_GCR_CMDRING          0x700

/* MISC_HOST_CTL bits. */
#define TI_MHC_INTSTATE         0x00000001  /* interrupt state (read-only) */
#define TI_MHC_CLEARINT         0x00000002  /* write to clear the interrupt */
#define TI_MHC_RESET            0x00000008  /* write to reset the chip */
#define TI_MHC_MASK_INTS        0x00000040  /* mask interrupts */
#define TI_MHC_CHIP_REV_MASK    0xf0000000  /* chip revision (read-only) */

/* MISC_LOCAL_CTL: the bit-banged serial-EEPROM lines. */
#define TI_MLC_EE_CLK           0x00100000
#define TI_MLC_EE_TXEN          0x00200000  /* master drives the data line */
#define TI_MLC_EE_DOUT          0x00400000  /* data out from host to EEPROM */
#define TI_MLC_EE_DIN           0x00800000  /* data in from EEPROM to host */
#define TI_EE_MAC_OFFSET        0x8c

/* CPU state bits. */
#define TI_CPUSTATE_HALT        0x00010000

/* GCR registers reached by symbolic offset. */
#define TI_GCR_PAR0             0x600
#define TI_GCR_PAR1             0x604

/* GCR OPMODE: the descriptor/data DMA byte-swap control (if_tireg.h). */
#define TI_OPMODE_BYTESWAP_BD   0x00000002
#define TI_OPMODE_WORDSWAP_BD   0x00000004
#define TI_OPMODE_BYTESWAP_DATA 0x00000010

/* Ring sizes and the firmware-up event (if_tireg.h). */
#define TI_EVENT_RING_CNT       256
#define TI_RETURN_RING_CNT      2048
#define TI_STD_RX_RING_CNT      512
#define TI_JUMBO_RX_RING_CNT    256
#define TI_TX_RING_CNT          512
#define TI_EV_FIRMWARE_UP       0x01
#define TI_EV_LINKSTAT_CHANGED  0x06
#define TI_EV_CODE_GIG_LINK_UP  0x01

/* Command opcode: the driver asks the firmware to negotiate the link. */
#define TI_CMD_LINK_NEGOTIATION 0x0b
#define TI_CMD_SET_PROMISC_MODE 0x0a
#define TI_CMD_CODE_PROMISC_ENB 0x01
#define TI_CMD_CODE_PROMISC_DIS 0x02

/*
 * Link status reported up as a modelled fact: gigabit, full duplex. The chip's
 * own link-enable bit rides in LINK_STAT (if_tireg.h TI_LNK and TI_GLNK).
 */
#define TI_GLNK_STAT_UP         0x000c0000  /* 1000MB | full-duplex */
#define TI_LNK_STAT_UP          0x400c0000  /* 1000MB | full-duplex | link-enb */

/* Ring control block host-ring residency flag (if_tireg.h). */
#define TI_RCB_FLAG_HOST_RING   0x0040

/* Buffer-descriptor flags and type (if_tireg.h). */
#define TI_BDFLAG_TCP_UDP_CKSUM 0x0001  /* insert the L4 checksum on transmit */
#define TI_BDFLAG_IP_CKSUM      0x0002  /* insert the IP header checksum */
#define TI_BDFLAG_END           0x0004
#define TI_BDFLAG_UCAST_PKT     0x0020
#define TI_BDFLAG_MCAST_PKT     0x0040
#define TI_BDFLAG_BCAST_PKT     0x0060
#define TI_BDTYPE_RECV_BD       0x0002
#define TI_BDTYPE_RECV_JUMBO_BD 0x0003
#define TI_BDFLAG_JUMBO_RING    0x0010

/* Largest frame the model assembles (jumbo, with slack). */
#define TIGON_MAX_FRAME_SIZE    9600
/* Smallest buffer that can hold one: below this the descriptor is not real. */
#define TIGON_MIN_FRAME_SIZE    60
/* Largest frame the standard ring's buffers are sized for. */
#define TIGON_STD_FRAME_SIZE    1518

/*
 * Field offsets inside the general information block (struct ti_gib): a 1 KB
 * statistics area, then seven 16-byte ring control blocks, then four 8-byte
 * host-memory producer/consumer pointers.
 */
#define TI_GIB_EV_RCB           0x400
#define TI_GIB_TX_RCB           0x420
#define TI_GIB_STD_RX_RCB       0x430
#define TI_GIB_JUMBO_RX_RCB     0x440
#define TI_GIB_RETURN_RCB       0x460
#define TI_GIB_EV_PRODIDX_PTR   0x470
#define TI_GIB_RETURN_PRODIDX_PTR 0x478
#define TI_GIB_TX_CONSIDX_PTR   0x480

#define TIGON_GCR_IDX(off)      (((off) - TI_GCR_BASE) >> 2)

/* Serial-EEPROM slave phases. */
enum {
    TIGON_EE_IDLE = 0,
    TIGON_EE_RX,     /* shifting a byte in from the master */
    TIGON_EE_READ,   /* shifting a byte out to the master */
};

/*
 * The EEPROM byte source. The driver reads only the station address, which the
 * public source pins at TI_EE_MAC_OFFSET + 2 (0x8e): the two bytes at 0x8c/0x8d
 * are zero padding before the six MAC bytes (if_ti.c comment at the read call).
 * Everything else reads zero.
 */
static uint8_t tigon_eeprom_byte(TigonState *s, uint16_t addr)
{
    if (addr >= TI_EE_MAC_OFFSET + 2 && addr < TI_EE_MAC_OFFSET + 2 + 6) {
        return s->conf.macaddr.a[addr - (TI_EE_MAC_OFFSET + 2)];
    }
    return 0;
}

/*
 * A 24Cxx serial EEPROM as an I2C slave, clocked by MISC_LOCAL_CTL writes. The
 * driver reads the MAC through this path before firmware download, so a model
 * that only served PAR0/PAR1 would return garbage for the driver's actual read.
 * Start/stop are data-line transitions while the clock is high; bits are
 * sampled on the rising edge and the slave presents read data and acknowledges
 * on the falling edge.
 */
static void tigon_eeprom_step(TigonState *s, uint32_t val)
{
    TigonEeprom *e = &s->eeprom;
    bool clk = val & TI_MLC_EE_CLK;
    bool txen = val & TI_MLC_EE_TXEN;
    bool sda = txen ? !!(val & TI_MLC_EE_DOUT) : true;

    if (clk && e->last_clk) {
        if (e->last_sda && !sda) {          /* data falling: START */
            e->state = TIGON_EE_RX;
            e->bitcnt = 0;
            e->shift = 0;
            e->bytenum = 0;
            e->read_mode = false;
            e->din = false;
        } else if (!e->last_sda && sda) {   /* data rising: STOP */
            e->state = TIGON_EE_IDLE;
            e->din = false;
        }
    }

    if (clk && !e->last_clk) {               /* rising edge */
        if (e->state == TIGON_EE_RX) {
            if (e->bitcnt < 8) {
                e->shift = (e->shift << 1) | (sda ? 1 : 0);
                e->bitcnt++;
                if (e->bitcnt == 8) {
                    switch (e->bytenum) {
                    case 0:
                        /* 0xA1 selects a read, 0xA0 a write. */
                        e->read_mode = e->shift & 1;
                        break;
                    case 1:
                        e->addr = (uint16_t)e->shift << 8;
                        break;
                    case 2:
                        e->addr = (e->addr & 0xff00) | e->shift;
                        break;
                    }
                    e->din = false;   /* acknowledge low */
                }
            } else if (e->bitcnt == 8) {
                /* This is the acknowledge clock, one pulse after the byte. */
                e->bitcnt = 9;
            }
        }
    }

    if (!clk && e->last_clk) {                /* falling edge */
        if (e->state == TIGON_EE_RX && e->bitcnt == 9) {
            /* End of the acknowledge clock: set up the next byte or read. */
            if (e->read_mode) {
                e->state = TIGON_EE_READ;
                e->bitcnt = 0;
                e->shift = tigon_eeprom_byte(s, e->addr);
                e->din = (e->shift >> 7) & 1;
            } else {
                e->bytenum++;
                e->bitcnt = 0;
                e->shift = 0;
            }
        } else if (e->state == TIGON_EE_READ) {
            e->bitcnt++;
            if (e->bitcnt < 8) {
                e->din = (e->shift >> (7 - e->bitcnt)) & 1;
            }
        }
    }

    e->last_clk = clk;
    e->last_sda = sda;
    s->misc_local_ctl = val & ~TI_MLC_EE_DIN;
}

static bool tigon_irq_line(TigonState *s)
{
    /*
     * One level-triggered INTA. There is no per-cause status register: the
     * "status" is the set of producer indices the firmware wrote into host
     * memory, so the model keeps an internal pending latch and gates delivery
     * on the interrupt-mailbox hold-off and the MISC_HOST_CTL mask bit.
     */
    return s->irq_asserted && s->mb_hostintr == 0 &&
           s->gcr[TIGON_GCR_IDX(TI_GCR_MASK_INTRS)] == 0 &&
           !(s->misc_host_ctl & TI_MHC_MASK_INTS);
}

static void tigon_update_irq(TigonState *s)
{
    bool level = tigon_irq_line(s);

    /*
     * Trace only transitions. The Bridge forwards to the Hub on a rising edge
     * and never on a held level, so how often this line actually falls and
     * rises again decides whether a condition raised during a handler is seen
     * at all. That is not answerable by reading the code: it depends on the
     * order the guest's driver does things.
     */
    if (level != s->irq_line_traced) {
        s->irq_line_traced = level;
        trace_tigon_irq_line(level, s->irq_asserted, s->mb_hostintr);
    }
    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * Deliver whatever has accumulated, and start counting again.
 */
static void tigon_coal_flush(TigonState *s)
{
    s->coal_rx_bds = 0;
    s->coal_tx_bds = 0;
    timer_del(s->coal_timer);
    s->irq_asserted = true;
    tigon_update_irq(s);
}

static void tigon_coal_timer(void *opaque)
{
    tigon_coal_flush(opaque);
}

/*
 * A ring completion, which is what the driver's coalescing settings govern.
 * Events are not routed here: a link change or the firmware coming up is rare
 * and waited on, and holding one back for company would delay bring-up.
 *
 * The completion itself has already been written to its ring. Only the
 * interrupt is held, so a driver that polls still sees the work.
 */
static void tigon_completion_irq(TigonState *s, bool rx)
{
    uint32_t max_bds = s->gcr[TIGON_GCR_IDX(rx ? TI_GCR_RX_MAX_COAL_BD
                                               : TI_GCR_TX_MAX_COAL_BD)];
    uint32_t ticks = s->gcr[TIGON_GCR_IDX(rx ? TI_GCR_RX_COAL_TICKS
                                             : TI_GCR_TX_COAL_TICKS)];
    uint32_t count = rx ? ++s->coal_rx_bds : ++s->coal_tx_bds;

    /*
     * Not asked for, or asked for without a deadline. A count with no time
     * limit would hold the last completions of a transfer back for an
     * interrupt that never comes, which is a stall rather than a saving.
     */
    if (max_bds <= 1 || ticks == 0 || count >= max_bds) {
        tigon_coal_flush(s);
        return;
    }
    if (!timer_pending(s->coal_timer)) {
        /* The unit the driver counts in is a microsecond. */
        timer_mod(s->coal_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)ticks * 1000);
    }
}


/*
 * Descriptor/GIB DMA byte order. The chip's own GCR OPMODE swap is a real,
 * guest-programmed feature: on a big-endian host the driver sets the
 * byte-swap-descriptor bit so the chip delivers host (big-endian) order, and
 * the register header's descriptor structs change field order under the same
 * condition. We honour it rather than assume a fixed order -- the swap bit
 * selects big-endian PCI-DMA accessors, matching the "chip delivers
 * big-endian, Bridge transparent" path. See docs/library/nic-on-bridge-
 * conventions.md and docs/plans/tigon-model.md, "Byte-swap".
 */
static bool tigon_bd_bigendian(TigonState *s)
{
    return (s->gcr[TIGON_GCR_IDX(TI_GCR_OPMODE)] & TI_OPMODE_BYTESWAP_BD) != 0;
}

static uint32_t tigon_dma_ld32(TigonState *s, uint64_t addr)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t v = 0;

    if (tigon_bd_bigendian(s)) {
        ldl_be_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    } else {
        ldl_le_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    }
    return v;
}

static void tigon_dma_st32(TigonState *s, uint64_t addr, uint32_t v)
{
    PCIDevice *pci = PCI_DEVICE(s);

    if (tigon_bd_bigendian(s)) {
        stl_be_pci_dma(pci, addr, v, MEMTXATTRS_UNSPECIFIED);
    } else {
        stl_le_pci_dma(pci, addr, v, MEMTXATTRS_UNSPECIFIED);
    }
}

/*
 * A host DMA address in a descriptor or RCB is two 32-bit words, high then low
 * (struct ti_hostaddr), so read and write it as two words rather than one
 * 64-bit quantity -- the word order is fixed regardless of the swap setting.
 */
static uint64_t tigon_dma_ld_addr(TigonState *s, uint64_t addr)
{
    return ((uint64_t)tigon_dma_ld32(s, addr) << 32) |
           tigon_dma_ld32(s, addr + 4);
}

static void tigon_dma_st_addr(TigonState *s, uint64_t addr, uint64_t v)
{
    tigon_dma_st32(s, addr, v >> 32);
    tigon_dma_st32(s, addr + 4, v & 0xffffffff);
}

/*
 * Frame payload DMA. A raw pci_dma_read/write does not compensate for the
 * Bridge's within-word byte-lane swap (docs/datasheets/bridge.md, "Byte
 * lanes on the DMA path"): tigon_dma_ld32()/tigon_dma_st32() already compose
 * correctly against that swap for descriptor and GIB words, so the payload
 * is moved the same way, one aligned word at a time. The swap correction
 * only holds at natural alignment, so an unaligned start or a trailing
 * partial word still goes through a full aligned word access and only the
 * bytes actually wanted are picked out of (or folded into) it.
 */
/*
 * Reverse the bytes of every aligned four-byte group in place. This is the
 * byte-lane swap the card applies when the descriptor byte-swap is off, done
 * over a whole buffer rather than one word at a time.
 */
static void tigon_swap_words(uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 0; i + 4 <= len; i += 4) {
        uint8_t b0 = p[i], b1 = p[i + 1];

        p[i] = p[i + 3];
        p[i + 1] = p[i + 2];
        p[i + 2] = b1;
        p[i + 3] = b0;
    }
}

static void tigon_dma_read_frame(TigonState *s, uint64_t addr, uint8_t *dst,
                                 uint32_t len)
{
    uint8_t staging[TIGON_MAX_FRAME_SIZE + 8];
    uint64_t first = addr & ~3ULL;
    uint32_t skew, span;

    /*
     * With the descriptor byte-swap on, the word load assembles the bytes in
     * memory order and the old loop wrote them straight back out in that
     * order: the identity, so the payload moves in a single transfer.
     */
    if (tigon_bd_bigendian(s)) {
        pci_dma_read(PCI_DEVICE(s), addr, dst, len);
        return;
    }

    /*
     * With it off -- which is how IRIX runs the card -- each aligned word
     * arrives byte-reversed. That is still one transfer plus a swap over the
     * result, rather than a transfer per word: a full-size frame costs 375 of
     * them otherwise, and this is the path the guest actually uses.
     */
    skew = addr - first;
    span = skew + len;
    span = (span + 3) & ~3u;
    if (span > sizeof(staging)) {
        /*
         * Longer than any frame the card assembles: fall back rather than
         * overrun the staging buffer.
         */
        while (len) {
            uint32_t off = addr & 3;
            uint32_t word = tigon_dma_ld32(s, addr & ~3ULL);
            uint32_t take = MIN(4 - off, len);
            uint32_t i;

            for (i = 0; i < take; i++) {
                dst[i] = word >> (24 - 8 * (off + i));
            }
            addr += take;
            dst += take;
            len -= take;
        }
        return;
    }

    pci_dma_read(PCI_DEVICE(s), first, staging, span);
    tigon_swap_words(staging, span);
    memcpy(dst, staging + skew, len);
}

static void tigon_dma_write_frame(TigonState *s, uint64_t addr,
                                  const uint8_t *src, uint32_t len)
{
    uint8_t staging[TIGON_MAX_FRAME_SIZE + 8];
    uint64_t first = addr & ~3ULL;
    uint32_t skew, span;

    /* The same identity as the read side: one transfer, not one per word. */
    if (tigon_bd_bigendian(s)) {
        pci_dma_write(PCI_DEVICE(s), addr, src, len);
        return;
    }

    skew = addr - first;
    span = skew + len;
    span = (span + 3) & ~3u;
    if (span > sizeof(staging)) {
        while (len) {
            uint64_t word_addr = addr & ~3ULL;
            uint32_t off = addr & 3;
            uint32_t take = MIN(4 - off, len);
            uint32_t word = take < 4 ? tigon_dma_ld32(s, word_addr) : 0;
            uint32_t i;

            for (i = 0; i < take; i++) {
                uint32_t shift = 24 - 8 * (off + i);
                word = (word & ~(0xffu << shift)) | ((uint32_t)src[i] << shift);
            }
            tigon_dma_st32(s, word_addr, word);
            addr += take;
            src += take;
            len -= take;
        }
        return;
    }

    /*
     * Read back only when the range does not cover whole words: the bytes
     * outside it belong to the guest and have to survive the write, which is
     * what the old loop's read-modify-write was for.
     */
    if (skew || (span != skew + len)) {
        pci_dma_read(PCI_DEVICE(s), first, staging, span);
        tigon_swap_words(staging, span);
    }
    memcpy(staging + skew, src, len);
    tigon_swap_words(staging, span);
    pci_dma_write(PCI_DEVICE(s), first, staging, span);
}

/*
 * Parse the general information block once it is complete. The RCBs and
 * producer-pointer fields are filled by the driver in host DRAM before it
 * releases the CPU, so this runs at firmware-up, when both preconditions hold,
 * rather than at GENINFO_LO write time when the RCBs are not yet populated.
 */
static void tigon_parse_gib(TigonState *s)
{
    uint64_t g = s->gib_base;
    uint64_t tx_rcb = g + TI_GIB_TX_RCB;
    uint64_t ret_rcb = g + TI_GIB_RETURN_RCB;
    uint32_t ret_word;

    s->ev_ring_base = tigon_dma_ld_addr(s, g + TI_GIB_EV_RCB);
    s->ev_prodidx_ptr = tigon_dma_ld_addr(s, g + TI_GIB_EV_PRODIDX_PTR);

    s->tx_ring_base = tigon_dma_ld_addr(s, tx_rcb);
    s->tx_host_ring =
        (tigon_dma_ld32(s, tx_rcb + 8) & TI_RCB_FLAG_HOST_RING) != 0;

    s->std_rx_ring_base = tigon_dma_ld_addr(s, g + TI_GIB_STD_RX_RCB);
    s->jumbo_rx_ring_base =
        tigon_dma_ld_addr(s, g + TI_GIB_JUMBO_RX_RCB);

    s->return_ring_base = tigon_dma_ld_addr(s, ret_rcb);
    ret_word = tigon_dma_ld32(s, ret_rcb + 8);
    s->return_ring_max = ret_word >> 16;
    if (s->return_ring_max == 0) {
        s->return_ring_max = TI_RETURN_RING_CNT;   /* 0 means 2048 */
    }
    s->return_prodidx_ptr = tigon_dma_ld_addr(s, g + TI_GIB_RETURN_PRODIDX_PTR);
    s->tx_considx_ptr = tigon_dma_ld_addr(s, g + TI_GIB_TX_CONSIDX_PTR);
}

static void tigon_post_event(TigonState *s, uint8_t event, uint16_t code,
                             uint16_t idx)
{
    uint64_t slot = s->ev_ring_base + (uint64_t)s->event_prodidx * 8;
    uint32_t eventx = ((uint32_t)event << 24) |
                      ((uint32_t)(code & 0xfff) << 12) | (idx & 0xfff);

    tigon_dma_st32(s, slot, eventx);
    tigon_dma_st32(s, slot + 4, 0);   /* reserved word */
    s->event_prodidx = (s->event_prodidx + 1) % TI_EVENT_RING_CNT;
    tigon_dma_st32(s, s->ev_prodidx_ptr, s->event_prodidx);
    s->irq_asserted = true;
    tigon_update_irq(s);
}

/*
 * Firmware-up fires only when both the CPU has been released and the GIB
 * address has been published: the firmware-up event lands on the event ring,
 * whose control block lives inside the GIB, so the model cannot post it until
 * the GIB is reachable (docs/plans/tigon-model.md, review item 2).
 */
static void tigon_try_firmware_up(TigonState *s)
{
    if (s->firmware_up || !s->cpu_a_released || !s->gib_valid) {
        return;
    }
    tigon_parse_gib(s);
    s->firmware_up = true;
    tigon_post_event(s, TI_EV_FIRMWARE_UP, 0, 0);
    /*
     * Coming up is a transition from refusing frames to accepting them, so it
     * has to re-offer anything held, exactly as posting a buffer does. A
     * driver that posts its buffers before the handshake completes rings no
     * doorbell afterwards, and the held frame would wait for one forever.
     */
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

/*
 * Report the port up as a modelled fact -- there is no SerDes and no
 * negotiation. Post the link-state event and set the link-status registers the
 * driver reads, in response to its link-negotiation request, so the event lands
 * after the driver is waiting for it rather than before (datasheet, "Model vs
 * hardware": link comes up when the interface is put into use).
 */
static void tigon_link_up(TigonState *s)
{
    s->gcr[TIGON_GCR_IDX(TI_GCR_GLINK_STAT)] = TI_GLNK_STAT_UP;
    s->gcr[TIGON_GCR_IDX(TI_GCR_LINK_STAT)] = TI_LNK_STAT_UP;
    tigon_post_event(s, TI_EV_LINKSTAT_CHANGED, TI_EV_CODE_GIG_LINK_UP, 0);
}

/* The firmware's asynchronous answer to a link-negotiation command. */
static void tigon_link_timer(void *opaque)
{
    tigon_link_up((TigonState *)opaque);
}

/* A brief virtual delay stands in for link negotiation. */
#define TIGON_LINK_DELAY_NS 50000000

/*
 * Command doorbell. The firmware drains the NIC-resident command ring and
 * advances the command consumer index the driver polls after posting a
 * command; without that advance the driver's post-command wait never returns.
 * Most commands need no interpreting for basic traffic (readiness is
 * firmware-up plus a posted receive buffer); the link-negotiation request is
 * answered with a link-up event. The producer index is reduced into range so
 * an out-of-range doorbell cannot loop.
 */
static void tigon_run_commands(TigonState *s, uint32_t prod)
{
    if (prod >= TIGON_CMD_RING_CNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: command producer index %u out of range\n", prod);
        prod %= TIGON_CMD_RING_CNT;
    }

    while (s->cmd_considx != prod) {
        uint32_t cmd = s->cmd_ring[s->cmd_considx];

        if (s->firmware_up && (cmd >> 24) == TI_CMD_LINK_NEGOTIATION) {
            timer_mod(s->link_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      TIGON_LINK_DELAY_NS);
        } else if ((cmd >> 24) == TI_CMD_SET_PROMISC_MODE) {
            s->promisc = ((cmd >> 16) & 0xff) == TI_CMD_CODE_PROMISC_ENB;
        }
        s->cmd_considx = (s->cmd_considx + 1) % TIGON_CMD_RING_CNT;
    }

    /*
     * Command completion is signalled only by advancing the consumer index the
     * driver reads, never by an interrupt: commands have no host-memory ring the
     * ISR could acknowledge, so raising the level-triggered line here would leave
     * it asserted forever and storm the ISR. A command that must notify the host
     * (a link-state change) posts its own event, which carries the interrupt.
     */
    s->gcr[TIGON_GCR_IDX(TI_GCR_CMDCONS_IDX)] = s->cmd_considx;
}

/*
 * Transmit doorbell. Walk the transmit ring from the saved consumer index to
 * the guest's producer index, assembling each frame from its descriptors'
 * fragments up to the end-of-frame flag, and hand it to the netdev. On Tigon 2
 * the ring is host-resident (the host-ring flag); the Tigon-1 windowed-SRAM
 * path is not modelled. The producer index is reduced into range so an
 * out-of-range mailbox write cannot make the walk loop forever.
 */

/*
 * Whether a frame is safe to hand to net_checksum_calculate(). The guest
 * composes these bytes, so a frame flagged for offload need not be the IPv4
 * packet the flag claims. The helper reads the IP header length and the IP
 * total length and trusts both against the buffer it was given: a header
 * length below the 20-byte minimum underflows its remaining-length arithmetic
 * and walks off the end of the frame. Check here, at the point where the
 * guest's bytes enter, that the frame really is IPv4 and that its own two
 * lengths fit inside it.
 */
static bool tigon_frame_offloadable(const uint8_t *frame, uint32_t len)
{
    static const uint32_t eth_hdr_len = 14;
    uint32_t ihl, ip_len;

    /* Ethernet header plus the smallest possible IPv4 header. */
    if (len < eth_hdr_len + 20) {
        return false;
    }
    /* Only IPv4 is offloaded: anything else is not the packet claimed. */
    if (frame[12] != 0x08 || frame[13] != 0x00) {
        return false;
    }
    if ((frame[eth_hdr_len] >> 4) != 4) {
        return false;
    }

    ihl = (frame[eth_hdr_len] & 0x0f) * 4;
    ip_len = ((uint32_t)frame[eth_hdr_len + 2] << 8) | frame[eth_hdr_len + 3];

    /* The header must be at least the minimum and fit inside the packet, */
    if (ihl < 20 || ip_len < ihl) {
        return false;
    }
    /* and the packet must fit inside the frame the card is sending. */
    return eth_hdr_len + ip_len <= len;
}

static void tigon_tx(TigonState *s, uint32_t prod)
{
    uint8_t frame[TIGON_MAX_FRAME_SIZE];
    uint32_t frame_len = 0;
    uint16_t frame_cksum = 0;   /* checksum-offload flags across the frame */

    if (!s->firmware_up) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: transmit doorbell before firmware-up\n");
        return;
    }
    if (!s->tx_host_ring) {
        qemu_log_mask(LOG_UNIMP,
                      "tigon: Tigon-1 NIC-resident transmit ring not "
                      "modelled\n");
        return;
    }

    if (prod >= TI_TX_RING_CNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: transmit producer index %u out of range\n", prod);
        prod %= TI_TX_RING_CNT;
    }

    while (s->tx_considx != prod) {
        uint64_t desc = s->tx_ring_base + (uint64_t)s->tx_considx * 16;
        uint64_t buf = tigon_dma_ld_addr(s, desc);
        uint32_t word = tigon_dma_ld32(s, desc + 8);
        uint16_t len = word >> 16;      /* big-endian: length high, flags low */
        uint16_t flags = word & 0xffff;

        if (len > sizeof(frame) - frame_len) {
            len = sizeof(frame) - frame_len;
        }
        /* Payload undoes the same Bridge byte-lane swap the descriptors do. */
        tigon_dma_read_frame(s, buf, frame + frame_len, len);
        frame_len += len;
        frame_cksum |= flags & (TI_BDFLAG_TCP_UDP_CKSUM | TI_BDFLAG_IP_CKSUM);
        s->tx_considx = (s->tx_considx + 1) % TI_TX_RING_CNT;

        if (flags & TI_BDFLAG_END) {
            /*
             * Checksum offload: IRIX's eg hands transmit frames with the L4
             * (and optionally IP) checksum field left zero and asks the card
             * to fill it via these descriptor flags. Compute it here, as the
             * firmware would -- without it every TCP segment leaves with a
             * zero checksum and the peer drops it (UDP's optional checksum
             * happens to survive, which is why a UDP transfer worked but TCP
             * never completed a handshake). Same offload the on-board IOC3
             * honours in hw/net/sgi-ioc3-eth.c.
             */
            int csum = 0;
            if (frame_cksum & TI_BDFLAG_TCP_UDP_CKSUM) {
                csum |= CSUM_TCP | CSUM_UDP;
            }
            if (frame_cksum & TI_BDFLAG_IP_CKSUM) {
                csum |= CSUM_IP;
            }
            if (csum && tigon_frame_offloadable(frame, frame_len)) {
                net_checksum_calculate(frame, frame_len, csum);
            }
            qemu_send_packet(qemu_get_queue(s->nic), frame, frame_len);
            frame_len = 0;
            frame_cksum = 0;
        }
    }

    tigon_dma_st32(s, s->tx_considx_ptr, s->tx_considx);
    tigon_completion_irq(s, false);
}

static void tigon_reset_state(TigonState *s)
{
    /*
     * Modeled defaults, not confirmed silicon power-on-reset values: the
     * mailboxes and GCR indices are producer/consumer positions the driver
     * always re-initialises, and the embedded CPUs come up halted (datasheet
     * "On the concrete reset entries"). The SRAM backing store is deliberately
     * not cleared: a guest resetting mid-download should not have to re-fetch
     * firmware it already wrote.
     */
    if (s->link_timer) {
        timer_del(s->link_timer);
    }
    s->misc_host_ctl = 0;
    s->misc_local_ctl = 0;
    s->pci_state = 0;
    s->win_base = 0;
    s->cpu_state_a = TI_CPUSTATE_HALT;
    s->cpu_pc_a = 0;
    s->cpu_ctl_b = TI_CPUSTATE_HALT;
    s->cpu_pc_b = 0;
    s->mb_hostintr = 0;
    s->coal_rx_bds = 0;
    s->coal_tx_bds = 0;
    if (s->coal_timer) {
        timer_del(s->coal_timer);
    }
    memset(s->gcr, 0, sizeof(s->gcr));
    memset(s->cmd_ring, 0, sizeof(s->cmd_ring));
    s->cpu_a_released = false;
    s->gib_valid = false;
    s->firmware_up = false;
    s->promisc = false;
    s->gib_base = 0;
    s->ev_ring_base = 0;
    s->ev_prodidx_ptr = 0;
    s->tx_ring_base = 0;
    s->tx_host_ring = false;
    s->std_rx_ring_base = 0;
    s->jumbo_rx_ring_base = 0;
    s->return_ring_base = 0;
    s->return_ring_max = 0;
    s->return_prodidx_ptr = 0;
    s->tx_considx_ptr = 0;
    s->event_prodidx = 0;
    s->return_prodidx = 0;
    s->tx_considx = 0;
    s->std_rx_considx = 0;
    s->std_rx_prodidx = 0;
    s->jumbo_rx_considx = 0;
    s->jumbo_rx_prodidx = 0;
    s->cmd_considx = 0;
    s->irq_asserted = false;
    memset(&s->eeprom, 0, sizeof(s->eeprom));
    s->eeprom.last_sda = true;   /* an idle serial line floats high */

    /*
     * Seed the MAC/station-address registers the firmware would present once
     * running, in the driver's big-endian pair format (if_ti.c ti_init2): PAR0
     * carries bytes 0-1, PAR1 carries bytes 2-5.
     */
    {
        const uint8_t *m = s->conf.macaddr.a;
        s->gcr[TIGON_GCR_IDX(TI_GCR_PAR0)] = ((uint32_t)m[0] << 8) | m[1];
        s->gcr[TIGON_GCR_IDX(TI_GCR_PAR1)] = ((uint32_t)m[2] << 24) |
                                             ((uint32_t)m[3] << 16) |
                                             ((uint32_t)m[4] << 8) | m[5];
    }
    tigon_update_irq(s);
}

static void tigon_misc_host_ctl_write(TigonState *s, uint32_t val)
{
    if (val & TI_MHC_RESET) {
        tigon_reset_state(s);
        return;
    }
    if (val & TI_MHC_CLEARINT) {
        s->irq_asserted = false;
    }
    /* The chip-revision nibble and interrupt-state bit are read-only. */
    s->misc_host_ctl = val & ~(TI_MHC_CHIP_REV_MASK | TI_MHC_INTSTATE |
                               TI_MHC_RESET | TI_MHC_CLEARINT);
    tigon_update_irq(s);
}

static uint64_t tigon_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    TigonState *s = opaque;

    switch (addr) {
    case TI_MISC_HOST_CTL:
        return ((uint32_t)s->chip_rev << 28) | s->misc_host_ctl |
               (tigon_irq_line(s) ? TI_MHC_INTSTATE : 0);
    case TI_MISC_LOCAL_CTL:
        return s->misc_local_ctl | (s->eeprom.din ? TI_MLC_EE_DIN : 0);
    case TI_PCI_STATE:
        return s->pci_state;
    case TI_WINBASE:
        return s->win_base;
    case TI_CPU_STATE:
        return s->cpu_state_a;
    case TI_CPU_PROGRAM_COUNTER:
        return s->cpu_pc_a;
    case TI_CPU_CTL_B:
        return s->cpu_ctl_b;
    case TI_CPU_PROGRAM_COUNTER_B:
        return s->cpu_pc_b;
    case TI_MB_HOSTINTR:
        return s->mb_hostintr;
    default:
        if (addr >= TI_GCR_BASE && addr <= TI_GCR_END) {
            return s->gcr[TIGON_GCR_IDX(addr)];
        }
        if (addr >= TI_GCR_CMDRING &&
            addr < TI_GCR_CMDRING + TIGON_CMD_RING_CNT * 4) {
            return s->cmd_ring[(addr - TI_GCR_CMDRING) >> 2];
        }
        qemu_log_mask(LOG_UNIMP, "tigon: read from unimplemented register "
                      "0x%03" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void tigon_reg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    TigonState *s = opaque;

    switch (addr) {
    case TI_MISC_HOST_CTL:
        tigon_misc_host_ctl_write(s, val);
        return;
    case TI_MISC_LOCAL_CTL:
        tigon_eeprom_step(s, val);
        return;
    case TI_PCI_STATE:
        s->pci_state = val;
        return;
    case TI_WINBASE:
        s->win_base = val;
        return;
    case TI_CPU_STATE:
        s->cpu_state_a = val;
        /* Clearing the halt bit releases embedded CPU A (ti_gibinit). */
        if (!(val & TI_CPUSTATE_HALT)) {
            s->cpu_a_released = true;
            tigon_try_firmware_up(s);
        }
        return;
    case TI_CPU_PROGRAM_COUNTER:
        s->cpu_pc_a = val;
        return;
    case TI_CPU_CTL_B:
        s->cpu_ctl_b = val;
        return;
    case TI_CPU_PROGRAM_COUNTER_B:
        s->cpu_pc_b = val;
        return;
    case TI_MB_HOSTINTR:
        /*
         * The mailbox write is the driver's interrupt acknowledge (if_ti.c
         * ti_intr: 1 = ack+mask, 0 = re-enable); IRIX's eg acks only with 0
         * and never advances EVENTCONS_IDX. The value is the delivery mask
         * (see tigon_irq_line).
         */
        /*
         * A write of 1 masks and acknowledges; the following 0 re-enables
         * (if_ti.c ti_intr). IRIX's eg only ever writes 0, and that write is
         * its acknowledge. Clear the pending latch on an acknowledge, but not
         * on the mask-to-unmask re-enable, so a completion that arrived while
         * the line was held off is still delivered once it is unmasked.
         */
        if (!(s->mb_hostintr && (val & 1) == 0)) {
            s->irq_asserted = false;
        }
        s->mb_hostintr = val & 1;
        tigon_update_irq(s);
        return;
    case TI_MB_SENDPROD_IDX:
        tigon_tx(s, val);
        return;
    case TI_MB_STDRXPROD_IDX:
        if (val >= TI_STD_RX_RING_CNT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: standard-rx producer index %u out of "
                          "range\n", (uint32_t)val);
        }
        s->std_rx_prodidx = val % TI_STD_RX_RING_CNT;
        /* A frame refused before a buffer existed can now be delivered. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    case TI_MB_CMDPROD_IDX:
        tigon_run_commands(s, val);
        return;
    case TI_MB_JUMBORXPROD_IDX:
        if (val >= TI_JUMBO_RX_RING_CNT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: jumbo-rx producer index %u out of range\n",
                          (uint32_t)val);
        }
        s->jumbo_rx_prodidx = val % TI_JUMBO_RX_RING_CNT;
        /* A frame too big for the standard ring can now be delivered. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    case TI_WINDATA:
    case TI_SRAM_ADDR:
    case TI_SRAM_DATA:
    case TI_MB_MINIRXPROD_IDX:
        qemu_log_mask(LOG_UNIMP, "tigon: write 0x%08x to unimplemented "
                      "register 0x%03" HWADDR_PRIx "\n", (uint32_t)val, addr);
        return;
    default:
        if (addr >= TI_GCR_BASE && addr <= TI_GCR_END) {
            s->gcr[TIGON_GCR_IDX(addr)] = val;
            switch (addr) {
            case TI_GCR_RX_COAL_TICKS:
                trace_tigon_coal_write("rx_coal_ticks", (uint32_t)val);
                break;
            case TI_GCR_TX_COAL_TICKS:
                trace_tigon_coal_write("tx_coal_ticks", (uint32_t)val);
                break;
            case TI_GCR_RX_MAX_COAL_BD:
                trace_tigon_coal_write("rx_max_coal_bds", (uint32_t)val);
                break;
            case TI_GCR_TX_MAX_COAL_BD:
                trace_tigon_coal_write("tx_max_coal_bds", (uint32_t)val);
                break;
            default:
                break;
            }
            if (addr == TI_GCR_MASK_INTRS) {
                /*
                 * Zero permits, anything else holds the line down: the public
                 * driver writes zero here under "Turn interrupts on", and that
                 * write is the only direct evidence of the polarity. A pending
                 * cause is held rather than dropped, so unmasking delivers it.
                 * Note a non-zero write: if a guest ever makes one and then
                 * waits for an interrupt, this reading is where to look.
                 */
                if (val != 0) {
                    qemu_log_mask(LOG_UNIMP,
                                  "tigon: interrupt mask set to 0x%08x, read "
                                  "as masking; only a zero write is attested\n",
                                  (uint32_t)val);
                }
                tigon_update_irq(s);
                return;
            }
            /*
             * The two general-information registers hold one 64-bit address.
             * This window is little-endian, so the half at the lower offset is
             * the least significant one -- whether the guest writes the pair as
             * two words or as a single 64-bit store that is split on the way
             * in. The public header's names read from the other convention, so
             * TI_GCR_GENINFO_HI is the offset of the *low* half here; taking
             * the names literally builds the address backwards.
             *
             * IRIX's eg gives it as 0x00f80000 then 0xa3000000, which assembled
             * this way is 0xa300000000f80000: a target widget of 0xa and an
             * offset inside memory, matching what every working device on this
             * machine puts on the bus. Assembled the other way it names widget
             * 0 and an offset past the end of memory, and every ring the card
             * reads through it comes back zero.
             *
             * The address is completed by the write to the upper half, which is
             * where the card is handed a usable block.
             */
            if (addr == TI_GCR_GENINFO_HI || addr == TI_GCR_GENINFO_LO) {
                uint32_t low = s->gcr[TIGON_GCR_IDX(TI_GCR_GENINFO_HI)];
                uint32_t high = s->gcr[TIGON_GCR_IDX(TI_GCR_GENINFO_LO)];

                s->gib_base = ((uint64_t)high << 32) | low;
                if (addr == TI_GCR_GENINFO_LO) {
                    s->gib_valid = s->gib_base != 0;
                    tigon_try_firmware_up(s);
                }
            }
            return;
        }
        if (addr >= TI_GCR_CMDRING &&
            addr < TI_GCR_CMDRING + TIGON_CMD_RING_CNT * 4) {
            s->cmd_ring[(addr - TI_GCR_CMDRING) >> 2] = val;
            return;
        }
        qemu_log_mask(LOG_UNIMP, "tigon: write 0x%08x to unimplemented "
                      "register 0x%03" HWADDR_PRIx "\n", (uint32_t)val, addr);
        return;
    }
}

/*
 * The register file is little-endian on the PCI bus (the ISP1040 precedent for
 * a merchant LE-native controller behind this same Bridge); the memory core
 * and the Bridge's own register-space correction carry the byte order. Each
 * register is 32 bits: impl is pinned alongside valid because an undeclared
 * min of 1 would let a byte access reach a side-effecting doorbell or the
 * MISC_HOST_CTL reset/swap bits through a partial-word path.
 */
static const MemoryRegionOps tigon_reg_ops = {
    .read = tigon_reg_read,
    .write = tigon_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * The 2 KB SRAM window at 0x800, positioned over card SRAM by WINBASE. The
 * downloaded firmware lands here and is read back but never executed. Byte and
 * halfword accesses are allowed since a download or diagnostic read may be
 * narrower than the register block's strict 32-bit rule.
 */
static uint64_t tigon_sram_read(void *opaque, hwaddr addr, unsigned size)
{
    TigonState *s = opaque;
    uint32_t off = (s->win_base + addr) % s->sram_size;

    if (off + size > s->sram_size) {
        return 0;
    }
    return ldn_le_p(s->sram + off, size);
}

static void tigon_sram_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    TigonState *s = opaque;
    uint32_t off = (s->win_base + addr) % s->sram_size;

    if (off + size > s->sram_size) {
        return;
    }
    stn_le_p(s->sram + off, size, val);
}

static const MemoryRegionOps tigon_sram_ops = {
    .read = tigon_sram_read,
    .write = tigon_sram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static bool tigon_can_receive(NetClientState *nc)
{
    TigonState *s = TIGON(qemu_get_nic_opaque(nc));

    /* Operational, and at least one standard receive buffer is posted. */
    return s->firmware_up &&
           (s->std_rx_considx != s->std_rx_prodidx ||
            s->jumbo_rx_considx != s->jumbo_rx_prodidx);
}

/*
 * Accept a frame for our unicast address or broadcast, or anything in
 * promiscuous mode. Multicast hash filtering is not modelled yet; the return
 * value says whether to keep the frame and sets the cast flag for its return
 * descriptor.
 */
static bool tigon_rx_filter(TigonState *s, const uint8_t *dest, size_t size,
                            uint16_t *flags)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (size < sizeof(broadcast)) {
        return false;   /* runt: too short to carry a destination address */
    }
    if (memcmp(dest, broadcast, 6) == 0) {
        *flags = TI_BDFLAG_BCAST_PKT;
        return true;
    }
    if (memcmp(dest, s->conf.macaddr.a, 6) == 0) {
        *flags = TI_BDFLAG_UCAST_PKT;
        return true;
    }
    if (s->promisc) {
        *flags = (dest[0] & 1) ? TI_BDFLAG_MCAST_PKT : TI_BDFLAG_UCAST_PKT;
        return true;
    }
    return false;
}

static ssize_t tigon_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    TigonState *s = TIGON(qemu_get_nic_opaque(nc));
    uint16_t flags = 0;
    uint64_t rx_desc, buf_addr, ret_desc;
    uint32_t rx_word;
    uint16_t buf_idx, buf_len;
    size_t landed;
    bool jumbo;

    if (!s->firmware_up) {
        return size;   /* not operational: consumed, not queued */
    }
    if (!tigon_rx_filter(s, buf, size, &flags)) {
        /*
         * Filtered frames are consumed, never returned as 0: a 0 would wedge
         * the backend's queue head behind a frame no retry can ever pass.
         */
        return size;
    }
    /*
     * A frame larger than the standard ring's buffers belongs in the jumbo
     * ring, which the driver posts separately and only once it has been told
     * a larger frame size. Choosing by size is what lets a guest raise its
     * frame size at all: served only from the standard ring, the card can
     * never carry one.
     */
    jumbo = size > TIGON_STD_FRAME_SIZE && s->jumbo_rx_ring_base != 0 &&
            s->jumbo_rx_considx != s->jumbo_rx_prodidx;
    if (!jumbo && s->std_rx_considx == s->std_rx_prodidx) {
        return 0;      /* genuine no-buffer flow control */
    }
    /*
     * Which ring carried the frame is not visible from a throughput figure,
     * because a large frame still arrives out of the standard ring when the
     * driver posted nothing to the jumbo one. Trace it, so the question of
     * whether a given guest uses the jumbo ring has an answer that is not a
     * guess.
     */
    trace_tigon_rx_ring(jumbo ? "jumbo" : "standard", (uint32_t)size,
                        jumbo
                        ? (s->jumbo_rx_prodidx - s->jumbo_rx_considx)
                          % TI_JUMBO_RX_RING_CNT
                        : (s->std_rx_prodidx - s->std_rx_considx)
                          % TI_STD_RX_RING_CNT);

    /* Take the next posted buffer from whichever ring carries this frame. */
    if (jumbo) {
        rx_desc = s->jumbo_rx_ring_base + (uint64_t)s->jumbo_rx_considx * 32;
    } else {
        rx_desc = s->std_rx_ring_base + (uint64_t)s->std_rx_considx * 32;
    }
    buf_addr = tigon_dma_ld_addr(s, rx_desc);
    /* Big-endian word: buffer index in the high half, length in the low. */
    rx_word = tigon_dma_ld32(s, rx_desc + 8);
    buf_idx = rx_word >> 16;
    buf_len = rx_word & 0xffff;
    if (jumbo) {
        s->jumbo_rx_considx =
            (s->jumbo_rx_considx + 1) % TI_JUMBO_RX_RING_CNT;
    } else {
        s->std_rx_considx = (s->std_rx_considx + 1) % TI_STD_RX_RING_CNT;
    }

    /*
     * A descriptor claiming a buffer too small to hold an ethernet frame, or
     * larger than the biggest frame the card assembles, is not a buffer the
     * guest posted -- the ring is not where the card believes it is. Writing
     * the frame to whatever address sits beside that length lands it in
     * arbitrary guest memory, which faults the kernel a long way from here and
     * with nothing pointing back. Say so and drop the frame instead.
     */
    if (buf_len < TIGON_MIN_FRAME_SIZE || buf_len > TIGON_MAX_FRAME_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: %s-rx descriptor %u offers a %u-byte buffer "
                      "at 0x%" PRIx64 ", which is not a posted receive "
                      "buffer; dropping the frame\n",
                      jumbo ? "jumbo" : "standard", buf_idx, buf_len,
                      buf_addr);
        return size;
    }

    /*
     * Never write past the buffer the guest sized: a frame larger than it is
     * clamped to its length, and the return descriptor reports what actually
     * landed. The standard ring's buffers hold a full frame in normal use;
     * this guards a guest that posts a short one.
     */
    landed = MIN(size, buf_len);
    /* Payload undoes the same Bridge byte-lane swap the descriptors do. */
    tigon_dma_write_frame(s, buf_addr, buf, landed);

    /* Post a receive-return descriptor naming the buffer and the length. */
    ret_desc = s->return_ring_base + (uint64_t)s->return_prodidx * 32;
    tigon_dma_st_addr(s, ret_desc, buf_addr);
    tigon_dma_st32(s, ret_desc + 8,
                   ((uint32_t)buf_idx << 16) | (landed & 0xffff));
    tigon_dma_st32(s, ret_desc + 12,
                   ((uint32_t)(jumbo ? TI_BDTYPE_RECV_JUMBO_BD
                                     : TI_BDTYPE_RECV_BD) << 16) |
                   flags | TI_BDFLAG_END |
                   (jumbo ? TI_BDFLAG_JUMBO_RING : 0));
    tigon_dma_st32(s, ret_desc + 16, 0);   /* ip / tcp-udp checksum */
    tigon_dma_st32(s, ret_desc + 20, 0);   /* error flags / vlan tag */
    tigon_dma_st32(s, ret_desc + 24, 0);   /* reserved */
    tigon_dma_st32(s, ret_desc + 28, 0);   /* opaque cookie */

    s->return_prodidx = (s->return_prodidx + 1) % s->return_ring_max;
    tigon_dma_st32(s, s->return_prodidx_ptr, s->return_prodidx);
    tigon_completion_irq(s, true);

    return size;
}

static void tigon_net_cleanup(NetClientState *nc)
{
    TigonState *s = qemu_get_nic_opaque(nc);

    s->nic = NULL;
}

static NetClientInfo tigon_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = tigon_can_receive,
    .receive = tigon_receive,
    .cleanup = tigon_net_cleanup,
};

static void tigon_reset_hold(Object *obj, ResetType type)
{
    TigonState *s = TIGON(obj);

    tigon_reset_state(s);
}

static void tigon_realize(PCIDevice *dev, Error **errp)
{
    TigonState *s = TIGON(dev);

    if (s->sram_size == 0) {
        error_setg(errp, "tigon: sram-size must be non-zero");
        return;
    }

    /*
     * PCI identity comes from instance properties rather than class-level
     * constants, so the SGI board is one property instantiation of the generic
     * Alteon part (hw/misc/sgi-bridge-dma-test.c uses the same pattern).
     */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);
    pci_config_set_revision(dev->config, s->revision);
    pci_config_set_class(dev->config, PCI_CLASS_NETWORK_ETHERNET);
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, s->subsystem_vendor_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, s->subsystem_id);
    dev->config[PCI_INTERRUPT_PIN] = 0x01;   /* INTA */

    s->sram = g_malloc0(s->sram_size);
    s->link_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tigon_link_timer, s);
    s->coal_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tigon_coal_timer, s);

    /*
     * One 16 KB non-prefetchable memory BAR (WINDATA, SRAM_DATA and the
     * mailboxes have read side effects). The register block is the first 2 KB;
     * the 2 KB SRAM window sits at 0x800 as its own sub-region.
     */
    memory_region_init(&s->bar, OBJECT(s), "tigon", TIGON_BAR_SIZE);
    memory_region_init_io(&s->reg, OBJECT(s), &tigon_reg_ops, s,
                          "tigon-regs", TIGON_REG_SIZE);
    memory_region_init_io(&s->sram_win, OBJECT(s), &tigon_sram_ops, s,
                          "tigon-sram-window", TIGON_SRAM_WIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->reg);
    memory_region_add_subregion(&s->bar, TIGON_SRAM_WINDOW, &s->sram_win);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&tigon_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), DEVICE(dev)->id,
                          &DEVICE(dev)->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void tigon_exit(PCIDevice *dev)
{
    TigonState *s = TIGON(dev);

    qemu_del_nic(s->nic);
    timer_free(s->link_timer);
    s->link_timer = NULL;
    timer_free(s->coal_timer);
    s->coal_timer = NULL;
    g_free(s->sram);
    s->sram = NULL;
}

static const VMStateDescription vmstate_tigon_eeprom = {
    .name = "tigon/eeprom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(last_clk, TigonEeprom),
        VMSTATE_BOOL(last_sda, TigonEeprom),
        VMSTATE_UINT8(state, TigonEeprom),
        VMSTATE_UINT8(bitcnt, TigonEeprom),
        VMSTATE_UINT8(shift, TigonEeprom),
        VMSTATE_UINT8(bytenum, TigonEeprom),
        VMSTATE_BOOL(read_mode, TigonEeprom),
        VMSTATE_UINT16(addr, TigonEeprom),
        VMSTATE_BOOL(din, TigonEeprom),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Re-drive the interrupt line from the migrated pending latch and its gating
 * bits, since the physical INTx level is not itself part of the stream.
 */
static int tigon_post_load(void *opaque, int version_id)
{
    tigon_update_irq(opaque);
    return 0;
}

/*
 * The rings live in guest memory and migrate with it; only the model's own
 * indices into them, the parsed ring residency, and the firmware/interrupt
 * latches need saving. The SRAM backing array is deliberately not migrated:
 * it holds a never-executed firmware image a fresh download reproduces.
 */
static const VMStateDescription vmstate_tigon = {
    .name = "tigon",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tigon_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, TigonState),
        VMSTATE_UINT32(misc_host_ctl, TigonState),
        VMSTATE_UINT32(misc_local_ctl, TigonState),
        VMSTATE_UINT32(pci_state, TigonState),
        VMSTATE_UINT32(win_base, TigonState),
        VMSTATE_UINT32(cpu_state_a, TigonState),
        VMSTATE_UINT32(cpu_pc_a, TigonState),
        VMSTATE_UINT32(cpu_ctl_b, TigonState),
        VMSTATE_UINT32(cpu_pc_b, TigonState),
        VMSTATE_UINT32(mb_hostintr, TigonState),
        VMSTATE_UINT32_ARRAY(gcr, TigonState, TIGON_GCR_REGS),
        VMSTATE_UINT32_ARRAY(cmd_ring, TigonState, TIGON_CMD_RING_CNT),
        VMSTATE_BOOL(cpu_a_released, TigonState),
        VMSTATE_BOOL(gib_valid, TigonState),
        VMSTATE_BOOL(firmware_up, TigonState),
        VMSTATE_BOOL(promisc, TigonState),
        VMSTATE_UINT64(gib_base, TigonState),
        VMSTATE_UINT64(ev_ring_base, TigonState),
        VMSTATE_UINT64(ev_prodidx_ptr, TigonState),
        VMSTATE_UINT64(tx_ring_base, TigonState),
        VMSTATE_BOOL(tx_host_ring, TigonState),
        VMSTATE_UINT64(std_rx_ring_base, TigonState),
        VMSTATE_UINT64(jumbo_rx_ring_base, TigonState),
        VMSTATE_UINT64(return_ring_base, TigonState),
        VMSTATE_UINT32(return_ring_max, TigonState),
        VMSTATE_UINT64(return_prodidx_ptr, TigonState),
        VMSTATE_UINT64(tx_considx_ptr, TigonState),
        VMSTATE_UINT32(event_prodidx, TigonState),
        VMSTATE_UINT32(return_prodidx, TigonState),
        VMSTATE_UINT32(tx_considx, TigonState),
        VMSTATE_UINT32(std_rx_considx, TigonState),
        VMSTATE_UINT32(std_rx_prodidx, TigonState),
        VMSTATE_UINT32(jumbo_rx_considx, TigonState),
        VMSTATE_UINT32(jumbo_rx_prodidx, TigonState),
        VMSTATE_UINT32(cmd_considx, TigonState),
        VMSTATE_BOOL(irq_asserted, TigonState),
        VMSTATE_TIMER_PTR(link_timer, TigonState),
        VMSTATE_TIMER_PTR(coal_timer, TigonState),
        VMSTATE_UINT32(coal_rx_bds, TigonState),
        VMSTATE_UINT32(coal_tx_bds, TigonState),
        VMSTATE_STRUCT(eeprom, TigonState, 1, vmstate_tigon_eeprom,
                       TigonEeprom),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tigon_properties[] = {
    DEFINE_NIC_PROPERTIES(TigonState, conf),
    DEFINE_PROP_UINT16("vendor-id", TigonState, vendor_id, TIGON_PCI_VENDOR_ID),
    DEFINE_PROP_UINT16("device-id", TigonState, device_id, TIGON_PCI_DEVICE_ID),
    DEFINE_PROP_UINT16("subsystem-vendor-id", TigonState,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-id", TigonState, subsystem_id, 0),
    DEFINE_PROP_UINT8("revision", TigonState, revision, 0),
    DEFINE_PROP_UINT8("chip-rev", TigonState, chip_rev, 0x6),
    DEFINE_PROP_UINT8("port-count", TigonState, port_count, 1),
    DEFINE_PROP_STRING("media", TigonState, media),
    DEFINE_PROP_SIZE32("sram-size", TigonState, sram_size, 1 * MiB),
};

static void tigon_instance_init(Object *obj)
{
    TigonState *s = TIGON(obj);

    /*
     * A board-identity default the property may override; the model serves one
     * media variant and does not negotiate, so this only labels the card.
     */
    s->media = g_strdup("copper");
}

static void tigon_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = tigon_realize;
    pc->exit = tigon_exit;
    pc->vendor_id = TIGON_PCI_VENDOR_ID;
    pc->device_id = TIGON_PCI_DEVICE_ID;
    pc->class_id = PCI_CLASS_NETWORK_ETHERNET;
    rc->phases.hold = tigon_reset_hold;
    dc->vmsd = &vmstate_tigon;
    dc->desc = "Alteon Tigon 2 Gigabit Ethernet (SGI eg)";
    device_class_set_props(dc, tigon_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo tigon_types[] = {
    {
        .name = TYPE_TIGON,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(TigonState),
        .instance_init = tigon_instance_init,
        .class_init = tigon_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(tigon_types)
