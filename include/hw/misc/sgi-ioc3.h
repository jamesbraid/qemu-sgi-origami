/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 */

#ifndef HW_MISC_SGI_IOC3_H
#define HW_MISC_SGI_IOC3_H

#include "hw/core/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/char/serial.h"
#include "hw/rtc/ds1685.h"
#include "hw/rtc/m48t35.h"
#include "hw/net/sgi-ioc3-eth.h"
#include "hw/input/sgi-ioc3-kbc.h"
#include "hw/char/sgi-ioc3-parallel.h"
#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3 "sgi-ioc3"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3State, SGI_IOC3)

/*
 * The chip's own bus: its sub-devices (ethernet, timekeeper, PS/2) realize onto
 * it rather than the system bus, so a reset of the IOC3 reaches them and their
 * migration path names the chip that owns them. Same shape as the macio
 * controller.
 */
#define TYPE_SGI_IOC3_BUS "sgi-ioc3-bus"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3BusState, SGI_IOC3_BUS)

struct SGIIOC3BusState {
    /*< private >*/
    BusState parent_obj;
};

#define SGI_IOC3_VENDOR_ID      0x10A9
#define SGI_IOC3_DEVICE_ID      0x0003
#define SGI_IOC3_CLASS_ID       0xFF00  /* no defined class */
#define SGI_IOC3_REVISION       0x01    /* the fitted Origin 200 part */

/*
 * The board the chip is fitted to, named by the subsystem identity in its
 * PCI configuration space. A guest reads the subsystem dword and decides
 * from it which sub-devices the board wires up, so this identity is not
 * decoration: it selects a population. The vendor half is SGI's on every
 * board and never varies.
 */
#define SGI_IOC3_SUBSYS_IP27_BASEIO6G   0xC300
#define SGI_IOC3_SUBSYS_IP27_MIO        0xC301
#define SGI_IOC3_SUBSYS_IP27_BASEIO     0xC302
#define SGI_IOC3_SUBSYS_IP29_SYSBOARD   0xC303  /* the Origin 200's board */
#define SGI_IOC3_SUBSYS_IP30_SYSBOARD   0xC304
#define SGI_IOC3_SUBSYS_MENET           0xC305
#define SGI_IOC3_SUBSYS_MENET4          0xC306

#define SGI_IOC3_SUBSYSTEM_ID   SGI_IOC3_SUBSYS_IP29_SYSBOARD

#define SGI_IOC3_BAR_SIZE       (1 * MiB)

/* The serial ports, in the byte-wide SuperIO window. */
#define SGI_IOC3_SERIAL_PORTS   2
#define SGI_IOC3_SERIAL_A       0x020178
#define SGI_IOC3_SERIAL_B       0x020170

/*
 * The SuperIO interrupt pair: no ethernet bit here -- that block drives
 * the PCI pin. The enable mask has set and clear write aliases.
 */
#define SGI_IOC3_INT_STATUS     0x1c
#define SGI_IOC3_INT_ENABLE_SET 0x20
#define SGI_IOC3_INT_ENABLE_CLR 0x24

#define SGI_IOC3_CHIP_CONTROL   0x28
/* Bit 22 is arbiter-idle status, polled by the kernel's console setup. */
#define SGI_IOC3_CHIP_CTRL_MASK 0x0007ffffu
#define SGI_IOC3_CHIP_CTRL_ARB_IDLE 0x00400000u

/*
 * Interrupt output timer configuration. The interval count (bits 0-15),
 * the mode (bits 16-18) and the diagnostic bit (30) are writable; the
 * pin-state readback (bit 31) and the reserved field are not stored.
 */
#define SGI_IOC3_INT_OUTPUT     0x2c
#define SGI_IOC3_INT_OUT_MASK   0x4007ffffu

/* MicroLAN (one-wire) control; protocol in docs/specs/sgi-ioc3.rst. */
#define SGI_IOC3_MICROLAN_CONTROL 0x30
#define SGI_IOC3_MICROLAN_LEVEL (1u << 0)   /* sampled from the bus */
#define SGI_IOC3_MICROLAN_DONE  (1u << 1)   /* hardware sets this when done */
#define SGI_IOC3_MICROLAN_TIMING (~(SGI_IOC3_MICROLAN_LEVEL | \
                                    SGI_IOC3_MICROLAN_DONE))

/* Slot kinds by pulse-width threshold; both guests fall well clear. */
#define SGI_IOC3_ONEWIRE_PULSE_SHIFT  10
#define SGI_IOC3_ONEWIRE_PULSE_RESET  400 /* at or above: a reset */
#define SGI_IOC3_ONEWIRE_PULSE_ZERO   40  /* at or above (below reset): a zero */

/* ROM selection: Linux sends Skip, OpenBSD Match; both must work. */
#define SGI_IOC3_ONEWIRE_ROM_SEARCH   0xf0
#define SGI_IOC3_ONEWIRE_ROM_SKIP     0xcc
#define SGI_IOC3_ONEWIRE_ROM_MATCH    0x55

#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY      0xf0
#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY_CRC  0xc3
#define SGI_IOC3_ONEWIRE_FN_READ_STATUS      0xaa

/* Family and serial top bits are required by OpenBSD's owmac_match(). */
#define SGI_IOC3_ONEWIRE_FAMILY  0x91
#define SGI_IOC3_ONEWIRE_SERIAL  0x5e7000000001ULL

#define SGI_IOC3_ONEWIRE_SERIAL_MASK 0xffffffffffffULL

#define SGI_IOC3_ONEWIRE_PAGE_SIZE   32
#define SGI_IOC3_ONEWIRE_STATUS_SIZE 4
#define SGI_IOC3_ONEWIRE_ANSWER_SIZE (1 + SGI_IOC3_ONEWIRE_PAGE_SIZE + 1)

/* One-wire transaction phases, reset to reset. */
#define SGI_IOC3_ONEWIRE_IDLE      0
#define SGI_IOC3_ONEWIRE_COMMAND   1
#define SGI_IOC3_ONEWIRE_SEARCH    2
#define SGI_IOC3_ONEWIRE_MATCH     3
#define SGI_IOC3_ONEWIRE_FUNCTION  4
#define SGI_IOC3_ONEWIRE_OFFSET    5
#define SGI_IOC3_ONEWIRE_ANSWER    6

/* Bit 21 routes the MicroLAN engine onto pin 8; drivers set it first. */
#define SGI_IOC3_PIN_CONTROL_SET 0x34
#define SGI_IOC3_PIN_CONTROL_CLR 0x38
#define SGI_IOC3_PIN_MICROLAN_ENABLE (1u << 21)

#define SGI_IOC3_PIN_DATA       0x3c
#define SGI_IOC3_PINS           9

/* One register per pin, an alias onto one bit of the data register. */
#define SGI_IOC3_PIN_REG(n)     (0x40 + 4 * (n))
#define SGI_IOC3_PIN_REG_FIRST  SGI_IOC3_PIN_REG(0)
#define SGI_IOC3_PIN_REG_LAST   SGI_IOC3_PIN_REG(SGI_IOC3_PINS - 1)

#define SGI_IOC3_MAIN_SIZE      (SGI_IOC3_PIN_REG_LAST + 4)

/*
 * The PS/2 controller: a bit-level engine, not an 8042 clone. Its window
 * sits here; the registers and behaviour live in hw/input/sgi-ioc3-kbc.c.
 */
#define SGI_IOC3_KBC_BASE           0x09c

/* The serial DMA block: a shared ring base, then seven registers per port. */
#define SGI_IOC3_SDMA_RING_HI       0x0b0
#define SGI_IOC3_SDMA_RING_LO       0x0b4
#define SGI_IOC3_SDMA_PORT_A        0x0b8
#define SGI_IOC3_SDMA_PORT_B        0x0d4

#define SGI_IOC3_SDMA_CONTROL       0x00
#define SGI_IOC3_SDMA_TX_PRODUCE    0x04
#define SGI_IOC3_SDMA_TX_CONSUME    0x08
#define SGI_IOC3_SDMA_RX_PRODUCE    0x0c
#define SGI_IOC3_SDMA_RX_CONSUME    0x10
#define SGI_IOC3_SDMA_RX_TIMER      0x14
#define SGI_IOC3_SDMA_SHADOW        0x18

#define SGI_IOC3_SDMA_BASE          SGI_IOC3_SDMA_RING_HI
#define SGI_IOC3_SDMA_PORT_REGS     7
#define SGI_IOC3_SDMA_PORT_STRIDE  (SGI_IOC3_SDMA_PORT_B - SGI_IOC3_SDMA_PORT_A)

/* The size select is in the register but outside the address. */
#define SGI_IOC3_SDMA_RING_4K       (1u << 0)
#define SGI_IOC3_SDMA_RING_ADDRESS  0xfffff000u
#define SGI_IOC3_SDMA_RING_1K_SIZE  0x400
#define SGI_IOC3_SDMA_RING_4K_SIZE  0x1000

/* Four rings in fixed order: A tx, A rx, B tx, B rx. */
#define SGI_IOC3_SDMA_RING_TX       0
#define SGI_IOC3_SDMA_RING_RX       1
#define SGI_IOC3_SDMA_RING_DIRS     2

#define SGI_IOC3_SDMA_HIGH_WATER    0x1ffu
#define SGI_IOC3_SDMA_TX_TIMER_RUN  (1u << 16)
#define SGI_IOC3_SDMA_FLOW_CONTROL  (1u << 17)
#define SGI_IOC3_SDMA_POST_ON_CD    (1u << 18)
#define SGI_IOC3_SDMA_POST_ON_CTS   (1u << 19)
#define SGI_IOC3_SDMA_QUAD_SPEED    (1u << 20)
#define SGI_IOC3_SDMA_NO_DIVIDER    (1u << 21)
#define SGI_IOC3_SDMA_RX_TO_MEMORY  (1u << 27)
#define SGI_IOC3_SDMA_ENABLE        (1u << 28)
#define SGI_IOC3_SDMA_PAUSE         (1u << 29)
#define SGI_IOC3_SDMA_PAUSED        (1u << 30)
#define SGI_IOC3_SDMA_RESET         (1u << 31)

/* Hardware read-backs and act-and-clear commands; never stored. */
#define SGI_IOC3_SDMA_CTRL_UNSTORED (SGI_IOC3_SDMA_TX_TIMER_RUN | \
                                     SGI_IOC3_SDMA_PAUSED | \
                                     SGI_IOC3_SDMA_RX_TO_MEMORY | \
                                     SGI_IOC3_SDMA_RESET)

/* Indices address the ring in entries; rx consume bit 31 arms the timer. */
#define SGI_IOC3_SDMA_ARM_RX_TIMER  (1u << 31)
#define SGI_IOC3_SDMA_HELD_SHIFT    24
#define SGI_IOC3_SDMA_TX_HELD       (0xfu << SGI_IOC3_SDMA_HELD_SHIFT)
#define SGI_IOC3_SDMA_RX_HELD       (0x7u << SGI_IOC3_SDMA_HELD_SHIFT)

#define SGI_IOC3_SDMA_TIMER_RELOAD  0xfffu
#define SGI_IOC3_SDMA_TIMER_COUNT   0x0fff0000u
#define SGI_IOC3_SDMA_TIMER_COUNT_SHIFT 16

#define SGI_IOC3_SDMA_TIMER_TICK_NS 62500   /* one 16 kHz tick */

/* Eight bytes per entry: four data bytes, then their status/control. */
#define SGI_IOC3_SDMA_ENTRY_SIZE    8
#define SGI_IOC3_SDMA_ENTRY_CHARS   4

/* A transmit control byte types the data byte beside it. */
#define SGI_IOC3_SDMA_TX_TYPE       0xc0
#define SGI_IOC3_SDMA_TX_NOTHING    0x00
#define SGI_IOC3_SDMA_TX_CHARACTER  0x40
#define SGI_IOC3_SDMA_TX_MODEM      0x80
#define SGI_IOC3_SDMA_TX_DELAY      0xc0
#define SGI_IOC3_SDMA_TX_INTERRUPT  0x20

#define SGI_IOC3_SDMA_RX_OVERRUN    0x01
#define SGI_IOC3_SDMA_RX_PARITY     0x02
#define SGI_IOC3_SDMA_RX_FRAMING    0x04
#define SGI_IOC3_SDMA_RX_BREAK      0x08
#define SGI_IOC3_SDMA_RX_CTS        0x10
#define SGI_IOC3_SDMA_RX_CD         0x20
#define SGI_IOC3_SDMA_RX_MODEM_OK   0x40
#define SGI_IOC3_SDMA_RX_DATA_OK    0x80

/* The 16550 shadow's own bit positions, not the status byte's. */
#define SGI_IOC3_SHADOW_LSR_MASK    0x000000ffu
#define SGI_IOC3_SHADOW_DELTA_CTS   (1u << 16)
#define SGI_IOC3_SHADOW_DELTA_CD    (1u << 19)
#define SGI_IOC3_SHADOW_CTS         (1u << 20)
#define SGI_IOC3_SHADOW_CD          (1u << 23)
#define SGI_IOC3_SHADOW_DTR         (1u << 24)
#define SGI_IOC3_SHADOW_RTS         (1u << 25)
#define SGI_IOC3_SHADOW_OUT1        (1u << 26)
#define SGI_IOC3_SHADOW_OUT2        (1u << 27)
#define SGI_IOC3_SHADOW_LOOPBACK    (1u << 28)

/* Per-port alias pages; three registers are read-only through them. */
#define SGI_IOC3_SDMA_ALIAS_A       0x08000
#define SGI_IOC3_SDMA_ALIAS_B       0x0c000
#define SGI_IOC3_SDMA_ALIAS_SIZE    (16 * KiB)

#define SGI_IOC3_SDMA_SIZE          (SGI_IOC3_ENET_BASE - SGI_IOC3_SDMA_BASE)
#define SGI_IOC3_SDMA_REGS          (SGI_IOC3_SDMA_SIZE / 4)

/*
 * The buffer RAM: 16 data bits plus a stored parity bit per 32-bit slot;
 * a window aliasing at its midpoint fails the driver's size probe.
 */
#define SGI_IOC3_SSRAM_BASE     0x40000
#define SGI_IOC3_SSRAM_SIZE     0x40000
#define SGI_IOC3_SSRAM_WORDS    (SGI_IOC3_SSRAM_SIZE / 4)

#define SGI_IOC3_SSRAM_DATA     0x0000ffffu
#define SGI_IOC3_SSRAM_PARITY   0x00010000u
#define SGI_IOC3_SSRAM_STORED   (SGI_IOC3_SSRAM_DATA | SGI_IOC3_SSRAM_PARITY)

/* Generated per read: the diagnostic writes bad parity deliberately. */
#define SGI_IOC3_SSRAM_PARITY_ERROR 0x00020000u

/*
 * The byte-bus selects, and which of them answer is a board's business. A
 * Timekeeper is one directly-addressed window on the first; a DS1685 is
 * reached indirectly, through an address port on the second and a data port
 * on the third. No board fits both clocks, and the fourth select is always
 * a hole.
 */
#define SGI_IOC3_BYTEBUS_DEV0   0x80000
#define SGI_IOC3_BYTEBUS_DEV1   0xa0000
#define SGI_IOC3_BYTEBUS_DEV2   0xc0000

/* Each port owns nine consecutive interrupt-status bits, A then B. */
#define SGI_IOC3_INT_PORT_STRIDE 9
#define SGI_IOC3_INT_TX_RING_EMPTY 0
#define SGI_IOC3_INT_RX_RING_FULL  1
#define SGI_IOC3_INT_RX_HIGH_WATER 2
#define SGI_IOC3_INT_RX_TIMER      3
#define SGI_IOC3_INT_CD_CHANGED    4
#define SGI_IOC3_INT_CTS_CHANGED   5
#define SGI_IOC3_INT_PASSTHROUGH_SOURCE 6
#define SGI_IOC3_INT_TX_DONE       7
#define SGI_IOC3_INT_PCI_ERROR     8

#define SGI_IOC3_SERIAL_SOURCE(port, source) \
    (1u << ((port) * SGI_IOC3_INT_PORT_STRIDE + (source)))

/* Pass-through bits follow the 16550's level; a write cannot clear one. */
#define SGI_IOC3_INT_SERIAL_A \
    SGI_IOC3_SERIAL_SOURCE(0, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_SERIAL_B \
    SGI_IOC3_SERIAL_SOURCE(1, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_PASSTHROUGH (SGI_IOC3_INT_SERIAL_A | SGI_IOC3_INT_SERIAL_B)

/* Levels too: IRIX's ring console never acknowledges them. */
#define SGI_IOC3_INT_RX_HIGH_WATER_LEVEL \
    (SGI_IOC3_SERIAL_SOURCE(0, SGI_IOC3_INT_RX_HIGH_WATER) | \
     SGI_IOC3_SERIAL_SOURCE(1, SGI_IOC3_INT_RX_HIGH_WATER))

/*
 * The parallel port's four consecutive sources: its service interrupt,
 * one per DMA context, then a DMA fault. The service bit is a level like
 * the two serial pass-throughs, quieted by a write to the port's own
 * extended control register rather than to the status register here; the
 * other three latch and a write-one-to-clear acknowledges them.
 */
#define SGI_IOC3_INT_PARALLEL_SHIFT 18
#define SGI_IOC3_INT_PARALLEL_SERVICE (1u << SGI_IOC3_INT_PARALLEL_SHIFT)

/* The one shared keyboard-and-mouse source, driven by the PS/2 controller. */
#define SGI_IOC3_INT_KEYBOARD       (1u << 22)

/*
 * Which sub-devices a board fits. One-wire is not a member here: the
 * station-address part sits behind the MicroLAN engine and is registered as
 * part of ethernet setup, so a board with no ethernet has no part on the bus
 * either. The buffer RAM, the serial DMA registers and the main register
 * block belong to the chip rather than to a board, and every board has them.
 */
/*
 * Which clock, rather than whether one: the two parts are not alternatives
 * behind one name. A Timekeeper is a flat window and a DS1685 is an index
 * and a data port, so a board that names the wrong one hands its guest a
 * different register layout at the address it looked for.
 */
typedef enum SGIIOC3ClockKind {
    SGI_IOC3_CLOCK_NONE = 0,
    SGI_IOC3_CLOCK_M48T35,      /* byte-bus device 0, directly addressed */
    SGI_IOC3_CLOCK_DS1685,      /* byte-bus devices 1 and 2, indirect */
} SGIIOC3ClockKind;

typedef struct SGIIOC3Population {
    bool eth;
    bool serial[SGI_IOC3_SERIAL_PORTS];
    bool kbc;
    bool parallel;
    SGIIOC3ClockKind clock;
} SGIIOC3Population;

/* Handlers learn their port only from this opaque. */
typedef struct SGIIOC3SerialAlias {
    SGIIOC3State *chip;
    unsigned int port;
} SGIIOC3SerialAlias;

struct SGIIOC3State {
    PCIDevice parent_obj;

    /* The chip's own bus; the sub-devices below realize onto it. */
    SGIIOC3BusState ioc3_bus;

    /* A property: which board this chip is on, presented in config space. */
    uint16_t subsystem_id;

    /* What that board fits, resolved once in realize. */
    const SGIIOC3Population *board;

    MemoryRegion bar;

    SerialState serial[SGI_IOC3_SERIAL_PORTS];
    MemoryRegion serial_io[SGI_IOC3_SERIAL_PORTS];

    MemoryRegion main;
    qemu_irq superio_irq;
    uint32_t int_status;
    uint32_t int_enable;

    uint32_t microlan_control;
    uint32_t pin_control;
    uint32_t pin_data;

    /* A property: the firmware must not merge nodes that answer alike. */
    uint64_t onewire_serial;

    /* Not migrated: a transaction caught mid-slot retries from a reset. */
    uint8_t onewire_phase;
    uint8_t onewire_bit;
    uint8_t onewire_command;
    uint64_t onewire_match;
    uint8_t onewire_args[2];
    uint8_t onewire_argn;
    uint8_t onewire_out[SGI_IOC3_ONEWIRE_ANSWER_SIZE];
    uint8_t onewire_out_len;
    uint16_t onewire_outbit;

    SGIIOC3EthState eth;

    MemoryRegion sdma;
    uint32_t sdma_reg[SGI_IOC3_SDMA_REGS];

    MemoryRegion sdma_alias[SGI_IOC3_SERIAL_PORTS];
    SGIIOC3SerialAlias sdma_alias_port[SGI_IOC3_SERIAL_PORTS];

    /* Re-entry guard for loopback walks; only true inside a call. */
    bool sdma_busy[SGI_IOC3_SERIAL_PORTS];

    /* A ring entry under construction, and its held count. */
    uint8_t sdma_packer[SGI_IOC3_SERIAL_PORTS][SGI_IOC3_SDMA_ENTRY_SIZE];
    uint8_t sdma_packed[SGI_IOC3_SERIAL_PORTS];

    /* Delivery sources fire per burst, not per entry. */
    bool sdma_delivered[SGI_IOC3_SERIAL_PORTS];

    QEMUTimer *sdma_rx_timer[SGI_IOC3_SERIAL_PORTS];

    AddressSpace *dma_as;   /* ring transfers go through the Bridge */

    uint32_t chip_control;
    uint32_t int_out;

    SGIIOC3KBCState kbc_dev;

    SGIIOC3ParallelState parallel_dev;

    MemoryRegion ssram;
    uint32_t ssram_word[SGI_IOC3_SSRAM_WORDS];

    /* One of these is realized, or neither; never both. */
    M48T35State rtc;
    DS1685State ds1685;
};

#endif
