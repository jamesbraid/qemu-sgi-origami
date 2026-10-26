/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI IOC3/IOC4 serial ring DMA engine.
 *
 * Both chips put the same engine in front of each 16550-compatible UART:
 * seven 32-bit registers per port, rings of eight-byte entries in host
 * memory, and a 64-bit ring base shared by a pair of ports. The chip owns its
 * register window, its interrupt status words and the UART instances; this
 * object owns the per-port registers, the rings and the timers.
 */

#ifndef HW_CHAR_SGI_IOC_SERIAL_DMA_H
#define HW_CHAR_SGI_IOC_SERIAL_DMA_H

#include "exec/hwaddr.h"
#include "hw/char/serial.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"

#define SGI_IOC_SDMA_MAX_PORTS      4
#define SGI_IOC_SDMA_MAX_PAIRS      (SGI_IOC_SDMA_MAX_PORTS / 2)

/* A port's seven registers, by byte offset within its block. */
#define SGI_IOC_SDMA_CONTROL        0x00
#define SGI_IOC_SDMA_TX_PRODUCE     0x04
#define SGI_IOC_SDMA_TX_CONSUME     0x08
#define SGI_IOC_SDMA_RX_PRODUCE     0x0c
#define SGI_IOC_SDMA_RX_CONSUME     0x10
#define SGI_IOC_SDMA_RX_TIMER       0x14
#define SGI_IOC_SDMA_SHADOW         0x18
#define SGI_IOC_SDMA_PORT_REGS      7
#define SGI_IOC_SDMA_PORT_STRIDE    (SGI_IOC_SDMA_PORT_REGS * 4)

/* Ring base, low word: bit 0 selects 4 KiB rings, the rest is address. */
#define SGI_IOC_SDMA_RING_4K        (1u << 0)
#define SGI_IOC_SDMA_RING_ADDRESS   0xfffff000u
#define SGI_IOC_SDMA_RING_1K_SIZE   0x400
#define SGI_IOC_SDMA_RING_4K_SIZE   0x1000

/* Within a pair's region: first port TX, RX, then second port TX, RX. */
#define SGI_IOC_SDMA_RING_TX        0
#define SGI_IOC_SDMA_RING_RX        1
#define SGI_IOC_SDMA_RING_DIRS      2

/* Control. */
#define SGI_IOC_SDMA_HIGH_WATER     0x1ffu
#define SGI_IOC_SDMA_TX_TIMER_RUN   (1u << 16)
#define SGI_IOC_SDMA_FLOW_CONTROL   (1u << 17)
#define SGI_IOC_SDMA_POST_ON_CD     (1u << 18)
#define SGI_IOC_SDMA_POST_ON_CTS    (1u << 19)
#define SGI_IOC_SDMA_QUAD_SPEED     (1u << 20)
#define SGI_IOC_SDMA_NO_DIVIDER     (1u << 21)
#define SGI_IOC_SDMA_RX_TO_MEMORY   (1u << 27)
#define SGI_IOC_SDMA_ENABLE         (1u << 28)
#define SGI_IOC_SDMA_PAUSE          (1u << 29)
#define SGI_IOC_SDMA_PAUSED         (1u << 30)
#define SGI_IOC_SDMA_RESET          (1u << 31)

/* Status and commands: these never read back as written. */
#define SGI_IOC_SDMA_CTRL_UNSTORED  (SGI_IOC_SDMA_TX_TIMER_RUN | \
                                     SGI_IOC_SDMA_PAUSED | \
                                     SGI_IOC_SDMA_RX_TO_MEMORY | \
                                     SGI_IOC_SDMA_RESET)

/* Index registers. */
#define SGI_IOC_SDMA_ARM_RX_TIMER   (1u << 31)
#define SGI_IOC_SDMA_HELD_SHIFT     24

/* Receive timer: reload in bits 0-11, live count in bits 16-27. */
#define SGI_IOC_SDMA_TIMER_RELOAD   0xfffu
#define SGI_IOC_SDMA_TIMER_COUNT_SHIFT 16
#define SGI_IOC_SDMA_TIMER_TICK_NS  62500   /* one 16 kHz tick */

/* A ring entry: four data bytes, then one status or control byte each. */
#define SGI_IOC_SDMA_ENTRY_SIZE     8
#define SGI_IOC_SDMA_ENTRY_CHARS    4

/* Transmit control byte. */
#define SGI_IOC_SDMA_TX_TYPE        0xc0
#define SGI_IOC_SDMA_TX_NOTHING     0x00
#define SGI_IOC_SDMA_TX_CHARACTER   0x40
#define SGI_IOC_SDMA_TX_MODEM       0x80
#define SGI_IOC_SDMA_TX_DELAY       0xc0
#define SGI_IOC_SDMA_TX_INTERRUPT   0x20
#define SGI_IOC_SDMA_TX_UNDEFINED   0x1f

/* Receive status byte. */
#define SGI_IOC_SDMA_RX_OVERRUN     0x01
#define SGI_IOC_SDMA_RX_PARITY      0x02
#define SGI_IOC_SDMA_RX_FRAMING     0x04
#define SGI_IOC_SDMA_RX_BREAK       0x08
#define SGI_IOC_SDMA_RX_CTS         0x10
#define SGI_IOC_SDMA_RX_CD          0x20
#define SGI_IOC_SDMA_RX_MODEM_OK    0x40
#define SGI_IOC_SDMA_RX_DATA_OK     0x80

/* The shadow: the 16550's line status, modem status and modem control. */
#define SGI_IOC_SDMA_SHADOW_LSR_MASK  0x000000ffu
#define SGI_IOC_SDMA_SHADOW_DELTA_CTS (1u << 16)
#define SGI_IOC_SDMA_SHADOW_DELTA_CD  (1u << 19)
#define SGI_IOC_SDMA_SHADOW_CTS       (1u << 20)
#define SGI_IOC_SDMA_SHADOW_CD        (1u << 23)
#define SGI_IOC_SDMA_SHADOW_DTR       (1u << 24)
#define SGI_IOC_SDMA_SHADOW_RTS       (1u << 25)
#define SGI_IOC_SDMA_SHADOW_OUT1      (1u << 26)
#define SGI_IOC_SDMA_SHADOW_OUT2      (1u << 27)
#define SGI_IOC_SDMA_SHADOW_LOOPBACK  (1u << 28)

/*
 * A port's interrupt sources, numbered as IOC3 lays them out in its status
 * word. IOC4 places all but the DMA error the same way, eight to a port.
 */
typedef enum SGIIOCSerialDMASource {
    SGI_IOC_SDMA_SRC_TX_EMPTY = 0,
    SGI_IOC_SDMA_SRC_RX_FULL = 1,
    SGI_IOC_SDMA_SRC_RX_HIGH = 2,
    SGI_IOC_SDMA_SRC_RX_TIMER = 3,
    SGI_IOC_SDMA_SRC_DELTA_CD = 4,
    SGI_IOC_SDMA_SRC_DELTA_CTS = 5,
    SGI_IOC_SDMA_SRC_TX_EXPLICIT = 7,
    SGI_IOC_SDMA_SRC_DMA_ERROR = 8,
} SGIIOCSerialDMASource;

/* What differs between the chips, fixed per chip. */
typedef struct SGIIOCSerialDMAParams {
    /*
     * The produce index reports the packer's held count in bits 24-26.
     * IOC3's header defines the field; IOC4's does not, so whether IOC4
     * has it is not known.
     */
    bool held_counts;
    /*
     * A failed transfer stops the port until a channel reset: IOC4's header
     * calls its error sources "serial halted by mem err". Whether an IOC3
     * port halts is not known.
     */
    bool halt_on_dma_error;
} SGIIOCSerialDMAParams;

/* How the engine reaches the chip around it. */
typedef struct SGIIOCSerialDMAOps {
    /* Latch a source; the chip's write-one-to-clear acknowledges it. */
    void (*raise)(void *opaque, unsigned port, SGIIOCSerialDMASource src);
    /* Drive a source that follows a level and ignores acknowledgement. */
    void (*set_level)(void *opaque, unsigned port, SGIIOCSerialDMASource src,
                      bool level);
    /* The port's UART is fitted and the engine is aimed at it. */
    bool (*reachable)(void *opaque, unsigned port);
    /* A character went to the UART for transmission. Optional. */
    void (*transmitted)(void *opaque, unsigned port, uint8_t ch);
    /*
     * A transfer failed at addr, receive or transmit. Optional: without it
     * the engine raises the port's DMA error source.
     */
    void (*dma_error)(void *opaque, unsigned port, bool rx, hwaddr addr);
} SGIIOCSerialDMAOps;

typedef struct SGIIOCSerialDMA SGIIOCSerialDMA;

typedef struct SGIIOCSerialDMAPort {
    SGIIOCSerialDMA *engine;
    unsigned index;
    SerialState *uart;

    uint32_t reg[SGI_IOC_SDMA_PORT_REGS];

    /* The 16550 core's character handlers, which the engine wraps. */
    IOCanReadHandler *saved_can_read;
    IOReadHandler *saved_read;
    IOEventHandler *saved_event;
    BackendChangeHandler *saved_be_change;
    void *saved_opaque;

    /* Re-entry guard for loopback walks; only true inside a call. */
    bool busy;

    /* Stopped by a failed transfer, until a channel reset. */
    bool halted;

    /* The UART's holding-register level, and the entry being sent. */
    bool tx_ready;
    bool tx_fetched;
    uint8_t tx_entry[SGI_IOC_SDMA_ENTRY_SIZE];
    uint8_t tx_slot;

    /*
     * A transmit delay in progress: when it ends, or 0 with none, and
     * whether it owes an explicit interrupt then.
     */
    int64_t tx_delay_due;
    bool tx_delay_irq;
    QEMUTimer *tx_delay_timer;

    /*
     * Whether a pause holds the timers, when it began, and when the receive
     * timer it froze would have run out.
     */
    bool paused;
    int64_t paused_at;
    int64_t rx_frozen_due;

    /* A ring entry under construction, and its held count. */
    uint8_t packer[SGI_IOC_SDMA_ENTRY_SIZE];
    uint8_t packed;
    /* The last post was turned away for lack of ring room. */
    bool post_refused;

    /* The receive timer's arm, kept apart from the consume pointer. */
    bool armed;

    /* A drain command written and not yet done. */
    bool drain_pending;

    QEMUTimer *rx_timer;
    /* The receive timer's count while it is not running. */
    uint16_t rx_idle_count;

    /*
     * A looped-back character still on the line: when it reaches the
     * receiver, or 0 with none in flight. The timer is only its wake-up.
     */
    int64_t loop_due;
    QEMUTimer *loop_timer;
} SGIIOCSerialDMAPort;

struct SGIIOCSerialDMA {
    const SGIIOCSerialDMAParams *params;
    const SGIIOCSerialDMAOps *ops;
    void *opaque;
    AddressSpace *dma_as;
    uint32_t nports;

    /* Transfers under way, for the chip's idle status; only inside a call. */
    unsigned in_flight;

    /* Per pair of ports: ring base high and low words, as written. */
    uint32_t base_hi[SGI_IOC_SDMA_MAX_PAIRS];
    uint32_t base_lo[SGI_IOC_SDMA_MAX_PAIRS];

    SGIIOCSerialDMAPort port[SGI_IOC_SDMA_MAX_PORTS];
};

void sgi_ioc_sdma_init(SGIIOCSerialDMA *d, unsigned nports,
                       const SGIIOCSerialDMAParams *params,
                       const SGIIOCSerialDMAOps *ops, void *opaque,
                       AddressSpace *dma_as);
void sgi_ioc_sdma_attach_uart(SGIIOCSerialDMA *d, unsigned port,
                              SerialState *uart);
void sgi_ioc_sdma_finalize(SGIIOCSerialDMA *d);
void sgi_ioc_sdma_reset(SGIIOCSerialDMA *d);

uint32_t sgi_ioc_sdma_base_read(SGIIOCSerialDMA *d, unsigned pair, bool high);
void sgi_ioc_sdma_base_write(SGIIOCSerialDMA *d, unsigned pair, bool high,
                             uint32_t val);

uint32_t sgi_ioc_sdma_port_read(SGIIOCSerialDMA *d, unsigned port,
                                hwaddr offset);
void sgi_ioc_sdma_port_write(SGIIOCSerialDMA *d, unsigned port,
                             hwaddr offset, uint32_t val);

/* Something changed that may let the engine move. */
void sgi_ioc_sdma_kick(SGIIOCSerialDMA *d, unsigned port);
/* The UART's transmit holding register emptied or filled. */
void sgi_ioc_sdma_set_tx_ready(SGIIOCSerialDMA *d, unsigned port, bool level);
/* Apply loopback arrivals and timer expiries already due. */
void sgi_ioc_sdma_catch_up(SGIIOCSerialDMA *d);

/* No transfer is under way: what a chip's DMA idle status reports. */
bool sgi_ioc_sdma_idle(SGIIOCSerialDMA *d);

/* Clear-to-send as the port reports it, which flow control can force. */
bool sgi_ioc_sdma_flow_cts(SGIIOCSerialDMA *d, unsigned port);

extern const VMStateDescription vmstate_sgi_ioc_sdma;

#define VMSTATE_SGI_IOC_SDMA(_field, _state) \
    VMSTATE_STRUCT(_field, _state, 0, vmstate_sgi_ioc_sdma, SGIIOCSerialDMA)

#endif
