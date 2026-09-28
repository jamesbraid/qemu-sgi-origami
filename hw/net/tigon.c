/*
 * Alteon Tigon 2 Gigabit Ethernet controller (SGI IRIS "eg" board).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * At power-on the host loads firmware into on-card SRAM and releases the
 * embedded CPU. Firmware uses shared-memory producer/consumer rings in host
 * DRAM and mailbox doorbells; this model implements that interface without
 * executing the downloaded firmware. Register offsets, ring and descriptor
 * layouts, and initialization follow the public NetBSD ti driver
 * (sys/dev/pci/if_ti.c, if_tireg.h), which implements the published Tigon
 * interface used by SGI eg.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/net/tigon.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/sgi-bridge.h"
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
#define TI_MISC_CONF            0x050
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
#define TI_GCR_STAT_TICKS       0x638
#define TI_GCR_TX_MAX_COAL_BD   0x63c
#define TI_GCR_RX_MAX_COAL_BD   0x640
#define TI_GCR_IFINDEX          0x65c
#define TI_GCR_IFMTU            0x660
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
#define TI_EV_STATS_UPDATED     0x04
#define TI_EV_LINKSTAT_CHANGED  0x06
#define TI_EV_CODE_GIG_LINK_UP  0x01

/*
 * Command word fields: opcode in bits 31:24, code in 23:12, index in 11:0.
 * OpenBSD sys/dev/ic/tireg.h:856-858 spells the three out as accessors and
 * its TI_DO_CMD packs them the same way (tireg.h:906-910); Linux's struct cmd
 * is the same three bitfields in that order (acenic.h:362-372).
 */
#define TI_CMD_OP(word)         (((word) >> 24) & 0xff)
#define TI_CMD_CODE(word)       (((word) >> 12) & 0xfff)
#define TI_CMD_IDX(word)        ((word) & 0xfff)

/* Command opcode: the driver asks the firmware to negotiate the link. */
#define TI_CMD_LINK_NEGOTIATION 0x0b
#define TI_CMD_SET_PROMISC_MODE 0x0a
#define TI_CMD_CODE_PROMISC_ENB 0x01
#define TI_CMD_CODE_PROMISC_DIS 0x02
#define TI_CMD_SET_MAC_ADDR      0x0c
/*
 * The command-path jumbo producer push. The public header marks it obsolete
 * in favour of the mailbox doorbell, but the IRIX eg driver drives its jumbo
 * ring through the command, so a card serving that guest must honour both.
 */
#define TI_CMD_SET_RX_JUMBO_PROD_IDX 0x10

/*
 * The statistics commands. The host asks the firmware to refresh the block
 * (both public drivers also send the general-communication update after
 * publishing the interface index and MTU, which is the same refresh under its
 * older name) or to zero the counters. The host-state command is how the
 * driver tells the card its protocol stack is up, which is the one thing the
 * card knows about the interface's administrative state.
 */
#define TI_CMD_HOST_STATE       0x01
#define TI_CMD_CODE_STACK_UP    0x01
#define TI_CMD_UPDATE_GENCOM    0x04
#define TI_CMD_CLR_STATS        0x0f
#define TI_CMD_RFRSH_STATS      0x11

/*
 * Link status reported up as a modelled fact: gigabit, full duplex. The
 * chip's own link-enable bit rides in LINK_STAT (if_tireg.h TI_LNK and
 * TI_GLNK).
 */
#define TI_GLNK_STAT_UP         0x000c0000  /* 1000MB | full-duplex */
#define TI_LNK_STAT_UP          0x400c0000  /* 1000MB|full-duplex|link-enb */

/* Ring control block host-ring residency flag (if_tireg.h). */
#define TI_RCB_FLAG_HOST_RING   0x0040

/* Buffer-descriptor flags and type (if_tireg.h). */
#define TI_BDFLAG_TCP_UDP_CKSUM 0x0001  /* insert L4 checksum on transmit */
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
/*
 * Smallest buffer that can hold one: below this the descriptor is not real.
 */
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
#define TI_GIB_REFRESH_STATS_PTR 0x488

/*
 * The firmware statistics block (struct ti_stats, if_tireg.h): 1 KB of
 * counters padded to a fixed size, laid out as the head of the general
 * information block and refreshed by DMA into whichever host address the
 * block's last field names. The constants below are word indices into it,
 * counting from the block's start. The first twelve words are the RFC 1643
 * ethernet-like counters, which this model leaves zero; the RFC 1213
 * interfaces group and the RFC 1573 64-bit counters follow.
 */
#define TI_STATS_SIZE           0x400
#define TI_STAT_IF_INDEX        12
#define TI_STAT_IF_MTU          14
#define TI_STAT_IF_SPEED        15
#define TI_STAT_IF_ADMIN_STATUS 16
#define TI_STAT_IF_OPER_STATUS  17
#define TI_STAT_IF_IN_DISCARDS  19
#define TI_STAT_IF_PHYS_ADDRESS 25
#define TI_STAT_HC_IN_OCTETS    36
#define TI_STAT_HC_IN_UCAST     38
#define TI_STAT_HC_IN_MULTICAST 40
#define TI_STAT_HC_IN_BROADCAST 42
#define TI_STAT_HC_OUT_OCTETS   44
#define TI_STAT_HC_OUT_UCAST    46
#define TI_STAT_HC_OUT_MULTICAST 48
#define TI_STAT_HC_OUT_BROADCAST 50

/* RFC 1213 interface states, spelled out in the public register header. */
#define TI_IF_STATUS_UP         1
#define TI_IF_STATUS_DOWN       2

/*
 * The modelled link, in the bits a second RFC 1213's ifSpeed is counted in.
 */
#define TIGON_LINK_BITS_PER_SECOND 1000000000u

/* The statistics-tick register counts the chip's microseconds. */
#define TIGON_STAT_TICK_NS      1000

/*
 * How often the model rewrites the block at the information block's own head.
 * The firmware keeps that copy current: the IRIX driver reads it without ever
 * asking for a refresh, and asks for its refreshed copy only every five
 * seconds, so a card that rewrote the head on that period alone would answer
 * the board utility with counters from before the traffic it is being asked
 * about. This is an approximation of "always current" chosen to be cheap: a
 * kilobyte at ten hertz, and only when a counter has moved.
 */
#define TIGON_STATS_LIVE_NS     100000000

#define TIGON_GCR_IDX(off)      (((off) - TI_GCR_BASE) >> 2)

/* Serial-EEPROM slave phases. */
enum {
    TIGON_EE_IDLE = 0,
    TIGON_EE_RX,     /* shifting a byte in from the master */
    TIGON_EE_READ,   /* shifting a byte out to the master */
};

/*
 * The EEPROM byte source. The driver reads only the station address, which
 * the public source pins at TI_EE_MAC_OFFSET + 2 (0x8e): the two bytes at
 * 0x8c/0x8d are zero padding before the six MAC bytes (if_ti.c comment at the
 * read call). Everything else reads zero.
 */
static uint8_t tigon_eeprom_byte(TigonState *s, uint16_t addr)
{
    if (addr >= TI_EE_MAC_OFFSET + 2 && addr < TI_EE_MAC_OFFSET + 2 + 6) {
        return s->conf.macaddr.a[addr - (TI_EE_MAC_OFFSET + 2)];
    }
    return 0;
}

/*
 * A 24Cxx serial EEPROM as an I2C slave, clocked by MISC_LOCAL_CTL writes.
 * The driver reads the MAC through this path before firmware download, so a
 * model that only served PAR0/PAR1 would return garbage for the driver's
 * actual read. Start/stop are data-line transitions while the clock is high;
 * bits are sampled on the rising edge and the slave presents read data and
 * acknowledges on the falling edge.
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

/* Deliver whatever has accumulated, and start counting again. */
static void tigon_coal_flush(TigonState *s, bool rx)
{
    uint32_t *count = rx ? &s->coal_rx_bds : &s->coal_tx_bds;
    QEMUTimer *timer = rx ? s->coal_rx_timer : s->coal_tx_timer;

    *count = 0;
    timer_del(timer);
    s->irq_asserted = true;
    tigon_update_irq(s);
}

static void tigon_coal_rx_timer(void *opaque)
{
    tigon_coal_flush(opaque, true);
}

static void tigon_coal_tx_timer(void *opaque)
{
    tigon_coal_flush(opaque, false);
}

/* Compare before adding so a restored near-limit count cannot wrap. */
static bool tigon_coal_count_reaches(uint32_t count, uint32_t completed,
                                     uint32_t threshold)
{
    return count >= threshold || completed >= threshold - count;
}

/*
 * A ring completion, which is what the driver's coalescing settings govern.
 * Events are not routed here: a link change or the firmware coming up is rare
 * and waited on, and holding one back for company would delay bring-up.
 *
 * The completion itself has already been written to its ring. Only the
 * interrupt is held, so a driver that polls still sees the work.
 */
static void tigon_completion_irq(TigonState *s, bool rx,
                                 uint32_t completed_bds)
{
    uint32_t *coal_bds = rx ? &s->coal_rx_bds : &s->coal_tx_bds;
    uint32_t max_bds = s->gcr[TIGON_GCR_IDX(rx ? TI_GCR_RX_MAX_COAL_BD
                                               : TI_GCR_TX_MAX_COAL_BD)];
    uint32_t ticks = s->gcr[TIGON_GCR_IDX(rx ? TI_GCR_RX_COAL_TICKS
                                             : TI_GCR_TX_COAL_TICKS)];
    QEMUTimer *timer = rx ? s->coal_rx_timer : s->coal_tx_timer;

    /*
     * Not asked for, or asked for without a deadline. A count with no time
     * limit would hold the last completions of a transfer back for an
     * interrupt that never comes, which is a stall rather than a saving.
     */
    if (max_bds <= 1 || ticks == 0 ||
        tigon_coal_count_reaches(*coal_bds, completed_bds, max_bds)) {
        tigon_coal_flush(s, rx);
        return;
    }
    *coal_bds += completed_bds;
    if (!timer_pending(timer)) {
        /* The unit the driver counts in is a microsecond. */
        timer_mod(timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)ticks * 1000);
    }
}


/*
 * Descriptor/GIB DMA byte order. The chip's own GCR OPMODE swap is a real,
 * guest-programmed feature: on a big-endian host the driver sets the
 * byte-swap-descriptor bit so the chip delivers host (big-endian) order, and
 * the register header's descriptor structs change field order under the same
 * condition. This model honours it rather than assuming a fixed order. The
 * swap bit selects big-endian PCI-DMA accessors, matching the "chip delivers
 * big-endian, Bridge transparent" path that this fork's other NIC models
 * behind the SGI Bridge also take.
 */
static bool tigon_bd_bigendian(TigonState *s)
{
    return (s->gcr[TIGON_GCR_IDX(TI_GCR_OPMODE)] & TI_OPMODE_BYTESWAP_BD) != 0;
}

enum {
    TIGON_MAX_BD_WORDS = 8,
};

static inline AddressSpace *tigon_descriptor_dma_as(TigonState *s,
                                                     const char *direction,
                                                     uint64_t addr,
                                                     uint32_t len)
{
    trace_tigon_descriptor_dma_route(direction, addr, len);
    return s->descriptor_as;
}

/*
 * Keep each descriptor in one ordered DMA request. Completion and producer
 * words use the single-word helper so a complete descriptor is visible first.
 */
static MemTxResult tigon_dma_read_words(TigonState *s, uint64_t addr,
                                        uint32_t *words, size_t count)
{
    bool bigendian = tigon_bd_bigendian(s);
    MemTxResult result;
    size_t bytes;
    size_t i;

    assert(count > 0 && count <= TIGON_MAX_BD_WORDS);
    bytes = count * sizeof(*words);
    result = dma_memory_read(tigon_descriptor_dma_as(s, "read", addr, bytes),
                             addr, words, bytes,
                             MEMTXATTRS_UNSPECIFIED);
    trace_tigon_dma("read", addr, bytes, result);
    if (result != MEMTX_OK) {
        memset(words, 0, bytes);
        return result;
    }

    for (i = 0; i < count; i++) {
        words[i] = bigendian ? be32_to_cpu(words[i])
                             : le32_to_cpu(words[i]);
    }
    return MEMTX_OK;
}

static MemTxResult tigon_dma_write_words(TigonState *s, uint64_t addr,
                                         const uint32_t *words, size_t count)
{
    bool bigendian = tigon_bd_bigendian(s);
    uint32_t raw[TIGON_MAX_BD_WORDS];
    MemTxResult result;
    size_t bytes;
    size_t i;

    assert(count > 0 && count <= ARRAY_SIZE(raw));
    bytes = count * sizeof(*words);
    for (i = 0; i < count; i++) {
        raw[i] = bigendian ? cpu_to_be32(words[i])
                           : cpu_to_le32(words[i]);
    }
    result = dma_memory_write(tigon_descriptor_dma_as(s, "write", addr,
                                                       bytes),
                              addr, raw, bytes,
                              MEMTXATTRS_UNSPECIFIED);
    trace_tigon_dma("write", addr, bytes, result);
    return result;
}

static MemTxResult tigon_dma_read32(TigonState *s, uint64_t addr,
                                    uint32_t *v)
{
    return tigon_dma_read_words(s, addr, v, 1);
}

static MemTxResult tigon_dma_write32(TigonState *s, uint64_t addr,
                                     uint32_t v)
{
    return tigon_dma_write_words(s, addr, &v, 1);
}

/*
 * A host DMA address in a descriptor or RCB is two 32-bit words, high then
 * low (struct ti_hostaddr), so read and write it as two words rather than one
 * 64-bit quantity; the word order is fixed regardless of the swap setting.
 */
static MemTxResult tigon_dma_read_addr(TigonState *s, uint64_t addr,
                                       uint64_t *v)
{
    uint32_t words[2];
    MemTxResult result;

    result = tigon_dma_read_words(s, addr, words, ARRAY_SIZE(words));
    if (result != MEMTX_OK) {
        return result;
    }
    *v = ((uint64_t)words[0] << 32) | words[1];
    return MEMTX_OK;
}

static MemTxResult tigon_dma_read_bd(TigonState *s, uint64_t addr,
                                     uint64_t *buf_addr, uint32_t *word)
{
    uint32_t words[3];
    MemTxResult result;

    result = tigon_dma_read_words(s, addr, words, ARRAY_SIZE(words));
    if (result == MEMTX_OK) {
        *buf_addr = ((uint64_t)words[0] << 32) | words[1];
        *word = words[2];
    }
    return result;
}

/*
 * Frame payload DMA. Payload is a byte stream, and on every correctly
 * programmed pairing its net transform is identity: the card applies no
 * byte-lane swap of its own to frame data, so the model moves it verbatim.
 * Behind the SGI Bridge that must not ride the byte-lane swap window. The
 * slot's swap is required for descriptors, which read as word values, and
 * per-word dispatch through the window was measured as this device's dominant
 * throughput cost, so payload takes the Bridge's plain DMA route: full
 * decode and node-qualified fabric routing, byte lanes alone skipped. On a
 * machine without the Bridge the device's own bus space is already plain.
 */
static MemTxResult tigon_dma_read_frame(TigonState *s, uint64_t addr,
                                        uint8_t *dst, uint32_t len)
{
    /*
     * Ordered descriptor reads precede this; ordered status writes publish
     * it.
     */
    return dma_memory_read_relaxed(s->payload_as, addr, dst, len);
}

static MemTxResult tigon_dma_write_frame(TigonState *s, uint64_t addr,
                                         const uint8_t *src, uint32_t len)
{
    /* Ordered return and producer writes publish these packet bytes. */
    return dma_memory_write_relaxed(s->payload_as, addr, src, len);
}

static MemTxResult tigon_dma_write_frame_iov(TigonState *s, uint64_t addr,
                                             const struct iovec *iov,
                                             int iovcnt, size_t len)
{
    MemTxResult result;
    size_t written = 0;
    int i;

    for (i = 0; i < iovcnt && written < len; i++) {
        size_t part = MIN(iov[i].iov_len, len - written);

        if (!part) {
            continue;
        }
        result = tigon_dma_write_frame(s, addr + written,
                                       iov[i].iov_base, part);
        if (result != MEMTX_OK) {
            return result;
        }
        written += part;
    }
    return written == len ? MEMTX_OK : MEMTX_ERROR;
}

/*
 * Parse the general information block once it is complete. The RCBs and
 * producer-pointer fields are filled by the driver in host DRAM before it
 * releases the CPU, so this runs at firmware-up, when both preconditions
 * hold, rather than at GENINFO_LO write time when the RCBs are not yet
 * populated.
 */
static MemTxResult tigon_parse_gib(TigonState *s)
{
    uint64_t g = s->gib_base;
    uint64_t tx_rcb = g + TI_GIB_TX_RCB;
    uint64_t ret_rcb = g + TI_GIB_RETURN_RCB;
    uint64_t ev_ring_base, ev_prodidx_ptr;
    uint64_t tx_ring_base, std_rx_ring_base, jumbo_rx_ring_base;
    uint64_t return_ring_base, return_prodidx_ptr, tx_considx_ptr;
    uint64_t refresh_stats_ptr;
    uint32_t tx_word, ret_word, return_ring_max;
    MemTxResult result;

    result = tigon_dma_read_addr(s, g + TI_GIB_EV_RCB, &ev_ring_base);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_EV_PRODIDX_PTR,
                                 &ev_prodidx_ptr);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, tx_rcb, &tx_ring_base);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read32(s, tx_rcb + 8, &tx_word);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_STD_RX_RCB,
                                 &std_rx_ring_base);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_JUMBO_RX_RCB,
                                 &jumbo_rx_ring_base);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, ret_rcb, &return_ring_base);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read32(s, ret_rcb + 8, &ret_word);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_RETURN_PRODIDX_PTR,
                                 &return_prodidx_ptr);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_TX_CONSIDX_PTR,
                                 &tx_considx_ptr);
    if (result != MEMTX_OK) {
        return result;
    }
    result = tigon_dma_read_addr(s, g + TI_GIB_REFRESH_STATS_PTR,
                                 &refresh_stats_ptr);
    if (result != MEMTX_OK) {
        return result;
    }

    return_ring_max = ret_word >> 16;
    if (return_ring_max == 0) {
        return_ring_max = TI_RETURN_RING_CNT;   /* 0 means 2048 */
    }
    s->ev_ring_base = ev_ring_base;
    s->ev_prodidx_ptr = ev_prodidx_ptr;
    s->tx_ring_base = tx_ring_base;
    s->tx_host_ring = (tx_word & TI_RCB_FLAG_HOST_RING) != 0;
    s->std_rx_ring_base = std_rx_ring_base;
    s->jumbo_rx_ring_base = jumbo_rx_ring_base;
    s->return_ring_base = return_ring_base;
    s->return_ring_max = return_ring_max;
    s->return_prodidx_ptr = return_prodidx_ptr;
    s->tx_considx_ptr = tx_considx_ptr;
    s->refresh_stats_ptr = refresh_stats_ptr;
    trace_tigon_gib(g, refresh_stats_ptr);
    return MEMTX_OK;
}

static MemTxResult tigon_post_event(TigonState *s, uint8_t event,
                                    uint16_t code, uint16_t idx)
{
    uint64_t slot = s->ev_ring_base + (uint64_t)s->event_prodidx * 8;
    uint32_t words[2] = {
        ((uint32_t)event << 24) |
        ((uint32_t)(code & 0xfff) << 12) | (idx & 0xfff),
        0,
    };
    uint32_t next = (s->event_prodidx + 1) % TI_EVENT_RING_CNT;
    MemTxResult result;

    result = tigon_dma_write_words(s, slot, words, ARRAY_SIZE(words));
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: event descriptor write at 0x%" PRIx64
                      " failed; event held\n", slot);
        return result;
    }
    result = tigon_dma_write32(s, s->ev_prodidx_ptr, next);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: event producer write at 0x%" PRIx64
                      " failed; event held\n", s->ev_prodidx_ptr);
        return result;
    }
    s->event_prodidx = next;
    s->irq_asserted = true;
    tigon_update_irq(s);
    return MEMTX_OK;
}

static void tigon_stats_put32(uint8_t *image, unsigned word, uint32_t v,
                              bool bigendian)
{
    if (bigendian) {
        stl_be_p(image + word * 4, v);
    } else {
        stl_le_p(image + word * 4, v);
    }
}

/*
 * A 64-bit counter is two words, most significant first. The word order is
 * fixed whichever way the host set the byte order, the same as the 64-bit
 * host addresses in a ring control block: written as one quantity in the
 * host's order instead, a big-endian host reads every count multiplied by
 * 2^32 (observed on the IRIX guest, which clears the descriptor byte swap).
 */
static void tigon_stats_put64(uint8_t *image, unsigned word, uint64_t v,
                              bool bigendian)
{
    tigon_stats_put32(image, word, v >> 32, bigendian);
    tigon_stats_put32(image, word + 1, v, bigendian);
}

/*
 * Assemble the statistics block and write it where the host asked for it. The
 * block is a struct in host DRAM, so it carries the byte order the host
 * selected for everything else the card writes there.
 *
 * Only the counters this model can observe are filled. The collision,
 * framing, carrier and internal-attention counters describe events the model
 * never generates, so leaving them zero reports what happened rather than
 * filling in a plausible number; the same goes for the firmware's own
 * command, event and ring bookkeeping further down the block.
 */
static MemTxResult tigon_publish_stats(TigonState *s, uint64_t addr)
{
    bool bigendian = tigon_bd_bigendian(s);
    bool link_up = s->gcr[TIGON_GCR_IDX(TI_GCR_GLINK_STAT)] != 0;
    uint8_t image[TI_STATS_SIZE];
    MemTxResult result;

    memset(image, 0, sizeof(image));
    /*
     * The interface's index and MTU are the host's own, handed to the card
     * through the register pair and reported straight back: the card has no
     * other source for either, and no public source says the firmware
     * rewrites what it was given.
     */
    tigon_stats_put32(image, TI_STAT_IF_INDEX,
                      s->gcr[TIGON_GCR_IDX(TI_GCR_IFINDEX)], bigendian);
    tigon_stats_put32(image, TI_STAT_IF_MTU,
                      s->gcr[TIGON_GCR_IDX(TI_GCR_IFMTU)], bigendian);
    tigon_stats_put32(image, TI_STAT_IF_SPEED,
                      link_up ? TIGON_LINK_BITS_PER_SECOND : 0, bigendian);
    tigon_stats_put32(image, TI_STAT_IF_ADMIN_STATUS,
                      s->stack_up ? TI_IF_STATUS_UP : TI_IF_STATUS_DOWN,
                      bigendian);
    tigon_stats_put32(image, TI_STAT_IF_OPER_STATUS,
                      link_up ? TI_IF_STATUS_UP : TI_IF_STATUS_DOWN,
                      bigendian);
    tigon_stats_put32(image, TI_STAT_IF_IN_DISCARDS, s->stat_rx_discards,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_IN_OCTETS, s->stat_rx_octets,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_IN_UCAST, s->stat_rx_ucast, bigendian);
    tigon_stats_put64(image, TI_STAT_HC_IN_MULTICAST, s->stat_rx_mcast,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_IN_BROADCAST, s->stat_rx_bcast,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_OUT_OCTETS, s->stat_tx_octets,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_OUT_UCAST, s->stat_tx_ucast,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_OUT_MULTICAST, s->stat_tx_mcast,
                      bigendian);
    tigon_stats_put64(image, TI_STAT_HC_OUT_BROADCAST, s->stat_tx_bcast,
                      bigendian);
    /*
     * The station address sits in the last six bytes of an eight-byte field,
     * behind two zero bytes, the same padding the serial EEPROM puts in
     * front of it, and is published as the two words the host's own address
     * registers already hold. The IRIX board utility prints bytes two through
     * seven of the field, so placing the address at the field's start instead
     * makes it print the address short by its first two octets followed by
     * two zeroes: 0:12:34:56:0:0 for a card at 52:54:00:12:34:56, observed on
     * run irix-20260913T170938-bf92af.
     */
    tigon_stats_put32(image, TI_STAT_IF_PHYS_ADDRESS,
                      ((uint32_t)s->station_addr.a[0] << 8) |
                      s->station_addr.a[1], bigendian);
    tigon_stats_put32(image, TI_STAT_IF_PHYS_ADDRESS + 1,
                      ((uint32_t)s->station_addr.a[2] << 24) |
                      ((uint32_t)s->station_addr.a[3] << 16) |
                      ((uint32_t)s->station_addr.a[4] << 8) |
                      s->station_addr.a[5], bigendian);

    result = dma_memory_write(tigon_descriptor_dma_as(s, "write", addr,
                                                      sizeof(image)),
                              addr, image, sizeof(image),
                              MEMTXATTRS_UNSPECIFIED);
    trace_tigon_dma("write", addr, sizeof(image), result);
    trace_tigon_stats(addr, s->stat_rx_ucast, s->stat_tx_ucast, result);
    return result;
}

/*
 * Publish the counters and say so on the event ring. A refresh the host asked
 * for is answered by the statistics-updated event, so a driver that waits for
 * the answer gets one.
 */
static void tigon_stats_refresh(TigonState *s)
{
    if (!s->firmware_up) {
        return;
    }
    /*
     * The block goes to both places the host named. The general information
     * block declares a statistics area at its own head, and its last field
     * names where a refreshed copy goes. Both public drivers make those one
     * address, so neither shows which the card fills; the IRIX guest gives
     * them separate buffers and reads its counters out of the head, so a card
     * that wrote only the refresh copy reported nothing to it.
     */
    if (s->gib_base && tigon_publish_stats(s, s->gib_base) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: statistics write at 0x%" PRIx64
                      " failed; counters held\n", s->gib_base);
        return;
    }
    if (s->refresh_stats_ptr && s->refresh_stats_ptr != s->gib_base &&
        tigon_publish_stats(s, s->refresh_stats_ptr) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: statistics write at 0x%" PRIx64
                      " failed; counters held\n", s->refresh_stats_ptr);
        return;
    }
    tigon_post_event(s, TI_EV_STATS_UPDATED, 0, 0);
}

/*
 * The clear command zeroes the counters. The head still holds the old ones,
 * so this leaves the block dirty: a host that clears and then reads the head
 * without asking for a refresh, which is how the IRIX board utility reads
 * it, has to see the zeroes.
 */
static void tigon_stats_clear(TigonState *s)
{
    s->stat_rx_octets = 0;
    s->stat_rx_ucast = 0;
    s->stat_rx_mcast = 0;
    s->stat_rx_bcast = 0;
    s->stat_tx_octets = 0;
    s->stat_tx_ucast = 0;
    s->stat_tx_mcast = 0;
    s->stat_tx_bcast = 0;
    s->stat_rx_discards = 0;
    s->stats_dirty = true;
}

/*
 * The refresh period the host programmed. Zero, the reset value, leaves the
 * firmware refreshing only when asked.
 */
static void tigon_stats_arm_timer(TigonState *s)
{
    uint32_t ticks = s->gcr[TIGON_GCR_IDX(TI_GCR_STAT_TICKS)];

    if (!ticks || !s->firmware_up) {
        timer_del(s->stats_timer);
        return;
    }
    timer_mod(s->stats_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (uint64_t)ticks * TIGON_STAT_TICK_NS);
}

static void tigon_stats_timer(void *opaque)
{
    TigonState *s = opaque;

    tigon_stats_refresh(s);
    tigon_stats_arm_timer(s);
}

/*
 * Keep the head current between the host's own refreshes. Every write that
 * moves a field this block publishes marks it dirty, and the set is closed:
 * the frame and octet counters, the discard count, the station address, the
 * host-state command's stack-up code, the link, and the three registers the
 * block reads, interface index, size and the gigabit link-status word. A
 * field added to the block has to be added here too, or the head carries
 * current counters over a stale interface section.
 */
static void tigon_stats_live_timer(void *opaque)
{
    TigonState *s = opaque;

    if (s->firmware_up && s->gib_base && s->stats_dirty &&
        tigon_publish_stats(s, s->gib_base) == MEMTX_OK) {
        s->stats_dirty = false;
    }
    timer_mod(s->stats_live_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              TIGON_STATS_LIVE_NS);
}

/*
 * A frame counts against the cast its destination address selects, the way
 * RFC 1213 separates them, and its octets count whichever cast it was.
 */
static void tigon_count_tx(TigonState *s, const uint8_t *frame, uint32_t len)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (len < sizeof(broadcast)) {
        return;     /* too short to name a destination: not a frame */
    }
    s->stat_tx_octets += len;
    s->stats_dirty = true;
    if (memcmp(frame, broadcast, sizeof(broadcast)) == 0) {
        s->stat_tx_bcast++;
    } else if (frame[0] & 1) {
        s->stat_tx_mcast++;
    } else {
        s->stat_tx_ucast++;
    }
}

/*
 * The same split on receive, from the cast the address filter already found.
 */
static void tigon_count_rx(TigonState *s, uint16_t flags, uint32_t len)
{
    s->stat_rx_octets += len;
    s->stats_dirty = true;
    if ((flags & TI_BDFLAG_BCAST_PKT) == TI_BDFLAG_BCAST_PKT) {
        s->stat_rx_bcast++;
    } else if (flags & TI_BDFLAG_MCAST_PKT) {
        s->stat_rx_mcast++;
    } else {
        s->stat_rx_ucast++;
    }
}

/*
 * Firmware-up fires only when both the CPU has been released and the GIB
 * address has been published: the firmware-up event lands on the event ring,
 * whose control block lives inside the GIB, so the model cannot post it until
 * the GIB is reachable.
 */
static void tigon_try_firmware_up(TigonState *s)
{
    if (s->firmware_up || !s->cpu_a_released || !s->gib_valid) {
        return;
    }
    if (tigon_parse_gib(s) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: general information block read at 0x%" PRIx64
                      " failed; firmware held\n", s->gib_base);
        return;
    }
    if (tigon_post_event(s, TI_EV_FIRMWARE_UP, 0, 0) != MEMTX_OK) {
        return;
    }
    s->firmware_up = true;
    tigon_stats_arm_timer(s);
    timer_mod(s->stats_live_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              TIGON_STATS_LIVE_NS);
    /*
     * Coming up is a transition from refusing frames to accepting them, so it
     * has to re-offer anything held, exactly as posting a buffer does. A
     * driver that posts its buffers before the handshake completes rings no
     * doorbell afterwards, and the held frame would wait for one forever.
     */
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

/*
 * Report the port up as a modelled fact. There is no SerDes and no
 * negotiation. Post the link-state event and set the link-status registers
 * the driver reads, in response to its link-negotiation request, so the event
 * lands after the driver is waiting for it rather than before (datasheet,
 * "Model vs hardware": link comes up when the interface is put into use).
 */
static void tigon_link_up(TigonState *s)
{
    s->gcr[TIGON_GCR_IDX(TI_GCR_GLINK_STAT)] = TI_GLNK_STAT_UP;
    s->gcr[TIGON_GCR_IDX(TI_GCR_LINK_STAT)] = TI_LNK_STAT_UP;
    s->stats_dirty = true;      /* ifSpeed and ifOperStatus follow the link */
    tigon_post_event(s, TI_EV_LINKSTAT_CHANGED, TI_EV_CODE_GIG_LINK_UP, 0);
}

/* The firmware's asynchronous answer to a link-negotiation command. */
static void tigon_link_timer(void *opaque)
{
    tigon_link_up((TigonState *)opaque);
}

/* A brief virtual delay stands in for link negotiation. */
#define TIGON_LINK_DELAY_NS 50000000

static void tigon_set_station_address(TigonState *s)
{
    uint32_t par0 = s->gcr[TIGON_GCR_IDX(TI_GCR_PAR0)];
    uint32_t par1 = s->gcr[TIGON_GCR_IDX(TI_GCR_PAR1)];
    uint8_t *m = s->station_addr.a;

    m[0] = par0 >> 8;
    m[1] = par0;
    m[2] = par1 >> 24;
    m[3] = par1 >> 16;
    m[4] = par1 >> 8;
    m[5] = par1;
    s->stats_dirty = true;      /* ifPhysAddress reports this address */
    qemu_format_nic_info_str(qemu_get_queue(s->nic), m);
}

/*
 * A shortage ends when a write actually posts a buffer. A write that moves
 * nothing is not the end of one: a driver whose receive handler rewrites the
 * index having found no memory to spare would otherwise reset the count on
 * every pass and never report the drought at all.
 */
static void tigon_report_rx_shortage_end(TigonState *s)
{
    if (s->jumbo_rx_prodidx == s->jumbo_rx_considx ||
        s->rx_shortage_drops == 0) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "tigon: jumbo ring posted again after %" PRIu64
                  " frames discarded for want of a buffer\n",
                  s->rx_shortage_drops);
    s->rx_shortage_drops = 0;
}

/* The same, for the standard ring's own drought. */
static void tigon_report_std_shortage_end(TigonState *s)
{
    if (s->std_rx_prodidx == s->std_rx_considx ||
        s->std_shortage_drops == 0) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "tigon: standard ring posted again after %" PRIu64
                  " frames discarded for want of a buffer\n",
                  s->std_shortage_drops);
    s->std_shortage_drops = 0;
}

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

        trace_tigon_command(TI_CMD_OP(cmd), TI_CMD_CODE(cmd), TI_CMD_IDX(cmd));
        if (s->firmware_up && TI_CMD_OP(cmd) == TI_CMD_LINK_NEGOTIATION) {
            timer_mod(s->link_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      TIGON_LINK_DELAY_NS);
        } else if (TI_CMD_OP(cmd) == TI_CMD_SET_PROMISC_MODE) {
            s->promisc = TI_CMD_CODE(cmd) == TI_CMD_CODE_PROMISC_ENB;
        } else if (TI_CMD_OP(cmd) == TI_CMD_SET_MAC_ADDR) {
            tigon_set_station_address(s);
        } else if (TI_CMD_OP(cmd) == TI_CMD_HOST_STATE) {
            s->stack_up = TI_CMD_CODE(cmd) == TI_CMD_CODE_STACK_UP;
            s->stats_dirty = true;      /* ifAdminStatus follows it */
        } else if (TI_CMD_OP(cmd) == TI_CMD_RFRSH_STATS ||
                   TI_CMD_OP(cmd) == TI_CMD_UPDATE_GENCOM) {
            tigon_stats_refresh(s);
        } else if (TI_CMD_OP(cmd) == TI_CMD_CLR_STATS) {
            tigon_stats_clear(s);
        } else if (TI_CMD_OP(cmd) == TI_CMD_SET_RX_JUMBO_PROD_IDX) {
            uint32_t idx = TI_CMD_IDX(cmd);

            if (idx >= TI_JUMBO_RX_RING_CNT) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "tigon: jumbo-rx producer index %u out of "
                              "range in command\n", idx);
            }
            s->jumbo_rx_prodidx = idx % TI_JUMBO_RX_RING_CNT;
            trace_tigon_rx_doorbell("jumbo-command", idx);
            tigon_report_rx_shortage_end(s);
            /*
             * The same release the mailbox performs. These two are the only
             * ways to post this ring and the IRIX eg driver uses the command,
             * so a completion refused for a full arrival line has to come
             * back either way; this path was not making that call.
             */
            sgi_net_rx_latency_ready(&s->rx_latency);
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        } else if (cmd != 0) {
            qemu_log_mask(LOG_UNIMP,
                          "tigon: unhandled firmware command 0x%02x "
                          "(word 0x%08x)\n", TI_CMD_OP(cmd), cmd);
        }
        s->cmd_considx = (s->cmd_considx + 1) % TIGON_CMD_RING_CNT;
    }

    /*
     * Command completion is signalled only by advancing the consumer index
     * the driver reads, never by an interrupt: commands have no host-memory
     * ring the ISR could acknowledge, so raising the level-triggered line
     * here would leave it asserted forever and storm the ISR. A command that
     * must notify the host (a link-state change) posts its own event, which
     * carries the interrupt.
     */
    s->gcr[TIGON_GCR_IDX(TI_GCR_CMDCONS_IDX)] = s->cmd_considx;
}

/*
 * Transmit doorbell. Walk the transmit ring from the saved consumer index to
 * the guest's producer index, assembling each frame from its descriptors'
 * fragments up to the end-of-frame flag, and hand it to the netdev. On Tigon
 * 2 the ring is host-resident (the host-ring flag); the Tigon-1 windowed-SRAM
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

static void tigon_tx_finish(TigonState *s, uint32_t completed_bds)
{
    if (completed_bds) {
        tigon_completion_irq(s, false, completed_bds);
    }

    s->defer_backend_rx = false;
    if (s->backend_rx_deferred) {
        s->backend_rx_deferred = false;
        qemu_bh_schedule(s->rx_flush_bh);
    }
}

static void tigon_tx(TigonState *s, uint32_t prod)
{
    uint8_t frame[TIGON_MAX_FRAME_SIZE];
    uint32_t frame_len = 0;
    uint16_t frame_cksum = 0;   /* checksum-offload flags across the frame */
    uint32_t cons = s->tx_considx;
    uint32_t completed_bds = 0;
    uint32_t frame_bds = 0;
    bool walked = false;

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

    while (cons != prod) {
        uint64_t desc = s->tx_ring_base + (uint64_t)cons * 16;
        uint64_t buf;
        uint32_t word;
        uint16_t len;
        uint16_t flags;

        if (tigon_dma_read_bd(s, desc, &buf, &word) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: transmit descriptor read at 0x%" PRIx64
                          " failed; ring walk stopped\n", desc);
            tigon_tx_finish(s, completed_bds);
            return;
        }
        len = word >> 16;
        flags = word & 0xffff;
        if (len > sizeof(frame) - frame_len) {
            len = sizeof(frame) - frame_len;
        }
        if (tigon_dma_read_frame(s, buf, frame + frame_len, len) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: transmit payload read at 0x%" PRIx64
                          " failed; ring walk stopped\n", buf);
            tigon_tx_finish(s, completed_bds);
            return;
        }
        walked = true;
        frame_bds++;
        frame_len += len;
        frame_cksum |= flags & (TI_BDFLAG_TCP_UDP_CKSUM | TI_BDFLAG_IP_CKSUM);
        cons = (cons + 1) % TI_TX_RING_CNT;

        if (flags & TI_BDFLAG_END) {
            /*
             * Checksum offload: IRIX's eg hands transmit frames with the L4
             * (and optionally IP) checksum field left zero and asks the card
             * to fill it via these descriptor flags. Compute it here, as the
             * firmware would, without it every TCP segment leaves with a
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
            if (tigon_dma_write32(s, s->tx_considx_ptr, cons) != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "tigon: transmit consumer write at 0x%" PRIx64
                              " failed; frame held\n", s->tx_considx_ptr);
                tigon_tx_finish(s, completed_bds);
                return;
            }
            /*
             * A backend may answer synchronously from qemu_send_packet().
             * Hold that receive until the internal consumer and completion
             * interrupt below have made this transmit whole.
             */
            s->defer_backend_rx = true;
            qemu_send_packet(qemu_get_queue(s->nic), frame, frame_len);
            tigon_count_tx(s, frame, frame_len);
            s->tx_considx = cons;
            completed_bds += frame_bds;
            frame_bds = 0;
            frame_len = 0;
            frame_cksum = 0;
        }
    }

    if (!walked) {
        if (tigon_dma_write32(s, s->tx_considx_ptr, cons) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: transmit consumer write at 0x%" PRIx64
                          " failed; ring walk stopped\n", s->tx_considx_ptr);
            tigon_tx_finish(s, completed_bds);
            return;
        }
        s->tx_considx = cons;
    }
    tigon_tx_finish(s, completed_bds);
}

static void tigon_reset_state(TigonState *s)
{
    /*
     * Modeled defaults, not confirmed silicon power-on-reset values: the
     * mailboxes and GCR indices are producer/consumer positions the driver
     * always re-initialises, and the embedded CPUs come up halted (datasheet
     * "On the concrete reset entries"). The SRAM backing store is
     * deliberately not cleared: a guest resetting mid-download should not
     * have to re-fetch firmware it already wrote.
     */
    if (s->link_timer) {
        timer_del(s->link_timer);
    }
    s->misc_host_ctl = 0;
    s->misc_local_ctl = 0;
    s->pci_state = 0;
    s->misc_conf = 0;
    s->win_base = 0;
    s->cpu_state_a = TI_CPUSTATE_HALT;
    s->cpu_pc_a = 0;
    s->cpu_ctl_b = TI_CPUSTATE_HALT;
    s->cpu_pc_b = 0;
    s->mb_hostintr = 0;
    s->coal_rx_bds = 0;
    s->coal_tx_bds = 0;
    if (s->coal_rx_timer) {
        timer_del(s->coal_rx_timer);
    }
    if (s->coal_tx_timer) {
        timer_del(s->coal_tx_timer);
    }
    if (s->stats_timer) {
        timer_del(s->stats_timer);
    }
    if (s->stats_live_timer) {
        timer_del(s->stats_live_timer);
    }
    if (s->rx_flush_bh) {
        qemu_bh_cancel(s->rx_flush_bh);
    }
    sgi_net_rx_latency_reset(&s->rx_latency);
    s->defer_backend_rx = false;
    s->backend_rx_deferred = false;
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
    s->refresh_stats_ptr = 0;
    s->stack_up = false;
    tigon_stats_clear(s);
    s->event_prodidx = 0;
    s->return_prodidx = 0;
    s->tx_considx = 0;
    s->std_rx_considx = 0;
    s->std_rx_prodidx = 0;
    s->jumbo_rx_considx = 0;
    s->jumbo_rx_prodidx = 0;
    s->rx_shortage_drops = 0;
    s->std_shortage_drops = 0;
    s->cmd_considx = 0;
    s->irq_asserted = false;
    memset(&s->eeprom, 0, sizeof(s->eeprom));
    s->eeprom.last_sda = true;   /* an idle serial line floats high */
    s->station_addr = s->conf.macaddr;
    if (s->nic) {
        qemu_format_nic_info_str(qemu_get_queue(s->nic),
                                 s->station_addr.a);
    }

    /*
     * Seed the MAC/station-address registers the firmware would present once
     * running, in the driver's big-endian pair format (if_ti.c ti_init2):
     * PAR0 carries bytes 0-1, PAR1 carries bytes 2-5.
     */
    {
        const uint8_t *m = s->station_addr.a;
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
    case TI_MISC_CONF:
        return s->misc_conf;
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
    case TI_MISC_CONF:
        s->misc_conf = val;
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
        trace_tigon_rx_doorbell("standard", (uint32_t)val);
        tigon_report_std_shortage_end(s);
        /*
         * A frame refused before a buffer existed can now be delivered, and a
         * completion the card could not write can be retried: posting a
         * buffer is what restarts placement, not the driver's next interrupt.
         */
        sgi_net_rx_latency_ready(&s->rx_latency);
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
        trace_tigon_rx_doorbell("jumbo", (uint32_t)val);
        tigon_report_rx_shortage_end(s);
        /* A frame too big for the standard ring can now be delivered. */
        sgi_net_rx_latency_ready(&s->rx_latency);
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
            case TI_GCR_IFINDEX:
            case TI_GCR_IFMTU:
            case TI_GCR_GLINK_STAT:
                /*
                 * The registers the statistics block reads. LINK_STAT is not
                 * one: the block takes its speed and operational state from
                 * the gigabit word alone.
                 */
                s->stats_dirty = true;
                break;
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
            case TI_GCR_STAT_TICKS:
                trace_tigon_coal_write("stat_ticks", (uint32_t)val);
                tigon_stats_arm_timer(s);
                break;
            default:
                break;
            }
            if (addr == TI_GCR_MASK_INTRS) {
                /*
                 * Zero permits, anything else holds the line down: the public
                 * driver writes zero here under "Turn interrupts on", and
                 * that write is the only direct evidence of the polarity. A
                 * pending cause is held rather than dropped, so unmasking
                 * delivers it. Note a non-zero write: if a guest ever makes
                 * one and then waits for an interrupt, this reading is where
                 * to look.
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
             * This window is little-endian, so the half at the lower offset
             * is the least significant one, whether the guest writes the
             * pair as two words or as a single 64-bit store that is split on
             * the way in. The public header's names read from the other
             * convention, so TI_GCR_GENINFO_HI is the offset of the *low*
             * half here; taking the names literally builds the address
             * backwards.
             *
             * IRIX's eg gives it as 0x00f80000 then 0xa3000000, which
             * assembled this way is 0xa300000000f80000: a target widget of
             * 0xa and an offset inside memory, matching what every working
             * device on this machine puts on the bus. Assembled the other way
             * it names widget 0 and an offset past the end of memory, and
             * every ring the card reads through it comes back zero.
             *
             * The address is completed by the write to the upper half, which
             * is where the card is handed a usable block.
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
 * The register file is little-endian on the PCI bus (the ISP1040 precedent
 * for a merchant LE-native controller behind this same Bridge); the memory
 * core and the Bridge's own register-space correction carry the byte order.
 * Each register is 32 bits: impl is pinned alongside valid because an
 * undeclared min of 1 would let a byte access reach a side-effecting doorbell
 * or the MISC_HOST_CTL reset/swap bits through a partial-word path.
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
 * downloaded firmware lands here and is read back but never executed. Byte
 * and halfword accesses are allowed since a download or diagnostic read may
 * be narrower than the register block's strict 32-bit rule.
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


/* A ones-complement sum, folded to 16 bits. */
static uint16_t tigon_ones_sum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/*
 * What the card summed over a received frame, for the driver to use instead
 * of summing the bytes again on the processor. Two 16-bit fields: the sum
 * over the IP header and the sum over what follows it.
 *
 * The sums are NOT inverted. The driver does that itself, and reads both
 * fields for any IP frame with nothing to gate them, so a well-formed header
 * must come back as 0xffff here. Reporting zero tells a driver of that shape
 * that every packet it receives is corrupt.
 *
 * Returns 0 for anything not a plain IPv4 frame, which is what the driver
 * ignores anyway.
 */
static uint32_t tigon_rx_checksums(const uint8_t *frame, size_t len)
{
    size_t ihl, ip_len;

    if (len < 14 + 20 || frame[12] != 0x08 || frame[13] != 0x00) {
        return 0;                       /* not IPv4 */
    }
    if ((frame[14] >> 4) != 4) {
        return 0;
    }
    ihl = (frame[14] & 0x0f) * 4;
    if (ihl < 20 || len < 14 + ihl) {
        return 0;
    }
    ip_len = ((size_t)frame[16] << 8) | frame[17];
    if (ip_len < ihl || len < 14 + ip_len) {
        return 0;                       /* truncated or lying about itself */
    }
    return ((uint32_t)tigon_ones_sum(frame + 14, ihl) << 16) |
           tigon_ones_sum(frame + 14 + ihl, ip_len - ihl);
}

static uint16_t tigon_ones_sum_iov(const struct iovec *iov, int iovcnt,
                                   size_t offset, size_t len)
{
    uint32_t sum = 0;
    uint8_t high = 0;
    bool have_high = false;
    int i;

    for (i = 0; i < iovcnt && len; i++) {
        const uint8_t *p = iov[i].iov_base;
        size_t part;

        if (offset >= iov[i].iov_len) {
            offset -= iov[i].iov_len;
            continue;
        }
        p += offset;
        part = MIN(iov[i].iov_len - offset, len);
        offset = 0;
        len -= part;
        while (part--) {
            if (!have_high) {
                high = *p++;
                have_high = true;
            } else {
                sum += ((uint32_t)high << 8) | *p++;
                have_high = false;
            }
        }
    }
    if (have_high) {
        sum += (uint32_t)high << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

static uint32_t tigon_rx_checksums_iov(const struct iovec *iov, int iovcnt,
                                       size_t len)
{
    uint8_t header[14 + 60];
    size_t ihl, ip_len;

    if (iovcnt == 1) {
        return tigon_rx_checksums(iov[0].iov_base, len);
    }
    if (len < 14 + 20 ||
        iov_to_buf(iov, iovcnt, 0, header, 14 + 20) != 14 + 20 ||
        header[12] != 0x08 || header[13] != 0x00 ||
        (header[14] >> 4) != 4) {
        return 0;
    }
    ihl = (header[14] & 0x0f) * 4;
    if (ihl < 20 || len < 14 + ihl ||
        iov_to_buf(iov, iovcnt, 0, header, 14 + ihl) != 14 + ihl) {
        return 0;
    }
    ip_len = ((size_t)header[16] << 8) | header[17];
    if (ip_len < ihl || len < 14 + ip_len) {
        return 0;
    }
    return ((uint32_t)tigon_ones_sum_iov(iov, iovcnt, 14, ihl) << 16) |
           tigon_ones_sum_iov(iov, iovcnt, 14 + ihl, ip_len - ihl);
}

static bool tigon_can_receive(NetClientState *nc)
{
    TigonState *s = TIGON(qemu_get_nic_opaque(nc));

    if (s->defer_backend_rx) {
        s->backend_rx_deferred = true;
        return false;
    }

    /* No room on the arrival line is the same answer as no buffer. */
    if (sgi_net_rx_latency_full(&s->rx_latency)) {
        return false;
    }

    /* Operational, and at least one standard receive buffer is posted. */
    return s->firmware_up &&
           (s->std_rx_considx != s->std_rx_prodidx ||
            s->jumbo_rx_considx != s->jumbo_rx_prodidx);
}

/*
 * Accept a frame for our unicast address, broadcast, or any multicast group,
 * or anything at all in promiscuous mode. The return value says whether to
 * keep the frame and sets the cast flag for its return descriptor.
 *
 * The exact group filter is not modelled, so every multicast frame is kept
 * and the stack sorts them out. That is the safe direction to be wrong in:
 * keeping a frame the driver did not ask for costs it one discard, while
 * dropping one it did ask for breaks the protocol carried on it and says
 * nothing anywhere. IPv6 neighbour discovery rides entirely on solicited-node
 * multicast, so dropping unsubscribed groups fails it silently.
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
    if (memcmp(dest, s->station_addr.a, 6) == 0) {
        *flags = TI_BDFLAG_UCAST_PKT;
        return true;
    }
    if (dest[0] & 1) {
        *flags = TI_BDFLAG_MCAST_PKT;   /* group bit: any multicast */
        return true;
    }
    if (s->promisc) {
        *flags = TI_BDFLAG_UCAST_PKT;
        return true;
    }
    return false;
}

static MemTxResult tigon_write_rx_return(TigonState *s, uint64_t addr,
                                         uint64_t buf_addr,
                                         uint32_t word2, uint32_t word3,
                                         uint32_t checksums)
{
    uint32_t words[TIGON_MAX_BD_WORDS] = {
        buf_addr >> 32,
        buf_addr & 0xffffffff,
        word2,
        word3,
        checksums,
        0,
        0,
        0,
    };

    return tigon_dma_write_words(s, addr, words, ARRAY_SIZE(words));
}

/*
 * Publish a receive whose descriptors are already written: the return-ring
 * producer the driver reads, and the completion interrupt. Held behind the
 * arrival floor by sgi_net_rx_latency_hold().
 */
static bool tigon_rx_publish(void *opaque, const void *record)
{
    TigonState *s = opaque;
    const TigonRxDone *done = record;

    if (tigon_dma_write32(s, s->return_prodidx_ptr,
                          done->return_prodidx) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: receive return producer write at 0x%" PRIx64
                      " failed; frame held\n", s->return_prodidx_ptr);
        return false;
    }
    tigon_completion_irq(s, true, 1);
    return true;
}

static ssize_t tigon_receive_packet(NetClientState *nc,
                                    const struct iovec *iov, int iovcnt,
                                    size_t size)
{
    TigonRxDone done;
    TigonState *s = TIGON(qemu_get_nic_opaque(nc));
    uint8_t dest[6];
    uint16_t flags = 0;
    uint64_t rx_desc, buf_addr, ret_desc;
    uint32_t rx_word, next_return_prod;
    uint16_t buf_idx, buf_len;
    size_t landed;
    bool jumbo;

    if (!s->firmware_up) {
        return size;   /* not operational: consumed, not queued */
    }
    if (iov_to_buf(iov, iovcnt, 0, dest, sizeof(dest)) != sizeof(dest) ||
        !tigon_rx_filter(s, dest, size, &flags)) {
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
     *
     * Either ring configured but holding no posted buffer discards the frame
     * and counts an input discard rather than mangling it into the other
     * ring's buffer. The two rings have independent control blocks and index
     * pairs, so a shortage on one cannot stop the other being served, while
     * parking the frame stops the back end and strands every frame behind it.
     *
     * Both halves of the disposition are the model's, not evidence: no public
     * source says whether real firmware discards at once or retains the frame
     * for a bounded refill, nor which of its counters moves. ifInDiscards is
     * RFC 1213's count of inbound packets discarded with no error found, one
     * reason for which is freeing buffer space, and is where this model
     * already counts its other receive drops. One bounded hold was built for
     * this device too, and the one that was measured, on the BCM570x, not
     * here, delivered fewer bytes than discarding at once; the Tigon
     * datasheet carries that as the stated limitation.
     */
    /*
     * A queued frame is re-offered straight to receive() without another
     * can_receive(), so the arrival line's room is checked here too. Zero is
     * the same flow control the ring checks below use.
     */
    if (sgi_net_rx_latency_full(&s->rx_latency)) {
        return 0;
    }
    jumbo = size > TIGON_STD_FRAME_SIZE && s->jumbo_rx_ring_base != 0;
    if (jumbo && s->jumbo_rx_considx == s->jumbo_rx_prodidx) {
        if (s->std_rx_considx == s->std_rx_prodidx) {
            return 0;  /* neither ring posted: genuine flow control */
        }
        /*
         * Reported once at each end of the shortage, never once per frame: a
         * ring left dry under load discards for as long as the drought lasts,
         * and a line each would spend the guest's own scheduling inside the
         * receive path rather than letting it post the buffer that ends this.
         */
        if (s->rx_shortage_drops == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: frame of %zu bytes needs the jumbo ring, "
                          "which has no posted buffer; discarding until it "
                          "is posted\n", size);
        }
        s->rx_shortage_drops++;
        trace_tigon_rx_shortage_drop("jumbo", (uint32_t)size);
        s->stat_rx_discards++;
        s->stats_dirty = true;
        return size;
    }
    if (!jumbo && s->std_rx_considx == s->std_rx_prodidx) {
        /*
         * The mirror, and the same disposition. The jumbo ring counts as
         * posted only if it could carry a frame at all, the condition size
         * selection applies above: a doorbell rung before the ring's control
         * block is published names buffers the card cannot reach.
         */
        if (s->jumbo_rx_ring_base == 0 ||
            s->jumbo_rx_considx == s->jumbo_rx_prodidx) {
            return 0;  /* neither ring posted: genuine flow control */
        }
        if (s->std_shortage_drops == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "tigon: frame of %zu bytes needs the standard ring, "
                          "which has no posted buffer; discarding until it "
                          "is posted\n", size);
        }
        s->std_shortage_drops++;
        trace_tigon_rx_shortage_drop("standard", (uint32_t)size);
        s->stat_rx_discards++;
        s->stats_dirty = true;
        return size;
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
    if (tigon_dma_read_bd(s, rx_desc, &buf_addr, &rx_word) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: receive descriptor read at 0x%" PRIx64
                      " failed; frame held\n", rx_desc);
        return 0;
    }
    /* Big-endian word: buffer index in the high half, length in the low. */
    buf_idx = rx_word >> 16;
    buf_len = rx_word & 0xffff;
    /*
     * A descriptor claiming a buffer too small to hold an ethernet frame is
     * not a buffer the guest posted. The ring is not where the card
     * believes it is. Writing the frame to whatever address sits beside that
     * length lands it in arbitrary guest memory, which faults the kernel a
     * long way from here and with nothing pointing back. Say so and drop the
     * frame instead. A buffer LARGER than any frame is not suspect: the
     * length is the buffer's capacity, and the IRIX driver posts 16 KB
     * allocations on its jumbo ring. An upper bound here silently dropped
     * every jumbo frame that guest ever received.
     */
    if (buf_len < TIGON_MIN_FRAME_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: %s-rx descriptor %u offers a %u-byte buffer "
                      "at 0x%" PRIx64 ", which is not a posted receive "
                      "buffer; dropping the frame\n",
                      jumbo ? "jumbo" : "standard", buf_idx, buf_len,
                      buf_addr);
        s->stat_rx_discards++;
        s->stats_dirty = true;
        return size;
    }

    /*
     * Never write past the buffer the guest sized: a frame larger than it is
     * clamped to its length, and the return descriptor reports what actually
     * landed. The standard ring's buffers hold a full frame in normal use;
     * this guards a guest that posts a short one.
     */
    landed = MIN(size, buf_len);
    if (tigon_dma_write_frame_iov(s, buf_addr, iov, iovcnt,
                                  landed) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: receive payload write at 0x%" PRIx64
                      " failed; frame dropped\n", buf_addr);
        s->stat_rx_discards++;
        s->stats_dirty = true;
        return size;
    }
    /* Post a receive-return descriptor naming the buffer and the length. */
    ret_desc = s->return_ring_base + (uint64_t)s->return_prodidx * 32;
    if (tigon_write_rx_return(s, ret_desc, buf_addr,
                              ((uint32_t)buf_idx << 16) |
                              (landed & 0xffff),
                              ((uint32_t)(jumbo
                                          ? TI_BDTYPE_RECV_JUMBO_BD
                                          : TI_BDTYPE_RECV_BD) << 16) |
                              flags | TI_BDFLAG_END |
                              (jumbo ? TI_BDFLAG_JUMBO_RING : 0),
                              tigon_rx_checksums_iov(iov, iovcnt,
                                                    size)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "tigon: receive return descriptor write at 0x%"
                      PRIx64 " failed; frame held\n", ret_desc);
        return 0;
    }

    tigon_count_rx(s, flags, (uint32_t)size);

    next_return_prod = (s->return_prodidx + 1) % s->return_ring_max;

    if (jumbo) {
        s->jumbo_rx_considx =
            (s->jumbo_rx_considx + 1) % TI_JUMBO_RX_RING_CNT;
    } else {
        s->std_rx_considx = (s->std_rx_considx + 1) % TI_STD_RX_RING_CNT;
    }
    s->return_prodidx = next_return_prod;

    /*
     * The frame and its return descriptor are in guest memory; the producer
     * index that lets the driver look at them, and the interrupt that tells
     * it to, wait for the frame's arrival deadline.
     */
    done.return_prodidx = next_return_prod;
    sgi_net_rx_latency_hold(&s->rx_latency, &done);

    return size;
}

static ssize_t tigon_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    const struct iovec iov = {
        .iov_base = (void *)buf,
        .iov_len = size,
    };

    return tigon_receive_packet(nc, &iov, 1, size);
}

static ssize_t tigon_receive_iov(NetClientState *nc, const struct iovec *iov,
                                 int iovcnt)
{
    size_t size = iov_size(iov, iovcnt);

    trace_tigon_receive_iov(iovcnt, size);
    return tigon_receive_packet(nc, iov, iovcnt, size);
}

static void tigon_rx_flush(void *opaque)
{
    TigonState *s = opaque;

    trace_tigon_rx_flush();
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
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
    .receive_iov = tigon_receive_iov,
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
    AddressSpace *plain;

    if (s->sram_size == 0) {
        error_setg(errp, "tigon: sram-size must be non-zero");
        return;
    }

    /*
     * PCI identity comes from instance properties rather than class-level
     * constants, so the SGI board is one property instantiation of the
     * generic Alteon part (hw/misc/sgi-bridge-dma-test.c uses the same
     * pattern).
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
    s->coal_rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    tigon_coal_rx_timer, s);
    s->coal_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    tigon_coal_tx_timer, s);
    s->stats_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tigon_stats_timer, s);
    s->stats_live_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       tigon_stats_live_timer, s);

    /*
     * One 16 KB non-prefetchable memory BAR (WINDATA, SRAM_DATA and the
     * mailboxes have read side effects). The register block is the first 2
     * KB; the 2 KB SRAM window sits at 0x800 as its own sub-region.
     */
    memory_region_init(&s->bar, OBJECT(s), "tigon", TIGON_BAR_SIZE);
    memory_region_init_io(&s->reg, OBJECT(s), &tigon_reg_ops, s,
                          "tigon-regs", TIGON_REG_SIZE);
    memory_region_init_io(&s->sram_win, OBJECT(s), &tigon_sram_ops, s,
                          "tigon-sram-window", TIGON_SRAM_WIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->reg);
    memory_region_add_subregion(&s->bar, TIGON_SRAM_WINDOW, &s->sram_win);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /*
     * The device cannot move buses after realize, so resolve both routes
     * once.
     */
    s->descriptor_as = pci_get_address_space(dev);
    plain = sgi_bridge_plain_dma_as(dev);
    s->payload_as = plain ? plain : pci_get_address_space(dev);
    trace_tigon_payload_dma_route(plain ? "bridge-plain" : "pci");

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->station_addr = s->conf.macaddr;
    s->nic = qemu_new_nic(&tigon_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), DEVICE(dev)->id,
                          &DEVICE(dev)->mem_reentrancy_guard, s);
    s->rx_flush_bh = qemu_bh_new_guarded(
        tigon_rx_flush, s, &DEVICE(dev)->mem_reentrancy_guard);
    if (!sgi_net_rx_latency_init(&s->rx_latency, s->rx_arrival_latency_us,
                                 sizeof(TigonRxDone), tigon_rx_publish,
                                 tigon_rx_flush, s, errp)) {
        return;
    }
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void tigon_exit(PCIDevice *dev)
{
    TigonState *s = TIGON(dev);

    sgi_net_rx_latency_cleanup(&s->rx_latency);
    qemu_bh_delete(s->rx_flush_bh);
    qemu_del_nic(s->nic);
    timer_free(s->link_timer);
    s->link_timer = NULL;
    timer_free(s->coal_rx_timer);
    s->coal_rx_timer = NULL;
    timer_free(s->coal_tx_timer);
    s->coal_tx_timer = NULL;
    timer_free(s->stats_timer);
    s->stats_timer = NULL;
    timer_free(s->stats_live_timer);
    s->stats_live_timer = NULL;
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
    TigonState *s = opaque;

    if (version_id < 2) {
        s->station_addr = s->conf.macaddr;
    }
    if (version_id < 3 && timer_pending(s->coal_rx_timer)) {
        uint64_t deadline = timer_expire_time_ns(s->coal_rx_timer);

        if (s->coal_tx_bds) {
            timer_mod(s->coal_tx_timer, deadline);
        }
        if (!s->coal_rx_bds) {
            timer_del(s->coal_rx_timer);
        }
    }
    /* Version 3 counted completed TX ring walks, not descriptors. */
    if (version_id < 4 && s->coal_tx_bds) {
        tigon_coal_flush(s, false);
    }
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->station_addr.a);
    tigon_update_irq(s);
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
    .version_id = 6,
    .minimum_version_id = 1,
    .post_load = tigon_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, TigonState),
        VMSTATE_UINT8_ARRAY_V(station_addr.a, TigonState, 6, 2),
        VMSTATE_UINT32(misc_host_ctl, TigonState),
        VMSTATE_UINT32(misc_local_ctl, TigonState),
        VMSTATE_UINT32(pci_state, TigonState),
        VMSTATE_UINT32(misc_conf, TigonState),
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
        VMSTATE_TIMER_PTR(coal_rx_timer, TigonState),
        VMSTATE_UINT32(coal_rx_bds, TigonState),
        VMSTATE_UINT32(coal_tx_bds, TigonState),
        VMSTATE_TIMER_PTR_V(coal_tx_timer, TigonState, 3),
        VMSTATE_STRUCT(eeprom, TigonState, 1, vmstate_tigon_eeprom,
                       TigonEeprom),
        VMSTATE_STRUCT(rx_latency, TigonState, 5,
                       vmstate_sgi_net_rx_latency, SGINetRxLatency),
        VMSTATE_UINT64_V(refresh_stats_ptr, TigonState, 6),
        VMSTATE_BOOL_V(stack_up, TigonState, 6),
        VMSTATE_UINT64_V(stat_rx_octets, TigonState, 6),
        VMSTATE_UINT64_V(stat_rx_ucast, TigonState, 6),
        VMSTATE_UINT64_V(stat_rx_mcast, TigonState, 6),
        VMSTATE_UINT64_V(stat_rx_bcast, TigonState, 6),
        VMSTATE_UINT64_V(stat_tx_octets, TigonState, 6),
        VMSTATE_UINT64_V(stat_tx_ucast, TigonState, 6),
        VMSTATE_UINT64_V(stat_tx_mcast, TigonState, 6),
        VMSTATE_UINT64_V(stat_tx_bcast, TigonState, 6),
        VMSTATE_UINT32_V(stat_rx_discards, TigonState, 6),
        VMSTATE_TIMER_PTR_V(stats_timer, TigonState, 6),
        VMSTATE_BOOL_V(stats_dirty, TigonState, 6),
        VMSTATE_TIMER_PTR_V(stats_live_timer, TigonState, 6),
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
    DEFINE_PROP_UINT32("rx-arrival-latency-us", TigonState,
                       rx_arrival_latency_us, SGI_NET_RX_LATENCY_DEFAULT_US),
};

static void tigon_instance_init(Object *obj)
{
    TigonState *s = TIGON(obj);

    /*
     * A board-identity default the property may override; the model serves
     * one media variant and does not negotiate, so this only labels the card.
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
