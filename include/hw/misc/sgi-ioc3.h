/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 */

#ifndef HW_MISC_SGI_IOC3_H
#define HW_MISC_SGI_IOC3_H

#include "hw/core/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/char/serial.h"
#include "hw/rtc/m48t35.h"
#include "hw/net/sgi-ioc3-eth.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3 "sgi-ioc3"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3State, SGI_IOC3)

/*
 * OpenBSD binds on the vendor/device pair alone; Linux also reads the
 * subsystem identity, which the Bridge substitutes from the board's
 * one-wire line, so the stored value matters only off-Bridge.
 */
#define SGI_IOC3_VENDOR_ID      0x10A9
#define SGI_IOC3_DEVICE_ID      0x0003
/*
 * Class 0xFF ("no defined class"), revision 1 -- the fitted Origin 200
 * part, not the revision-0 identity an older diagnostic table records.
 */
#define SGI_IOC3_CLASS_ID       0xFF00
#define SGI_IOC3_REVISION       0x01
/* The Origin 200's IP29 System Board, the only board this machine models. */
#define SGI_IOC3_SUBSYSTEM_ID   0xC303

/*
 * One BAR covers the chip; the highest occupant is the fourth byte-bus
 * select at 0x0e0000 plus its 32 KB window, so 1 MB is the smallest cover.
 */
#define SGI_IOC3_BAR_SIZE       (1 * MiB)

/* The two serial ports, at their offsets in the byte-wide SuperIO window. */
#define SGI_IOC3_SERIAL_PORTS   2
#define SGI_IOC3_SERIAL_A       0x020178
#define SGI_IOC3_SERIAL_B       0x020170

/*
 * The SuperIO interrupt pair. The chip has two interrupt outputs: this
 * status register carries no ethernet bit at all -- the ethernet pair lives
 * in that block's window and drives the PCI pin instead. The enable mask
 * has set and clear write aliases so a guest avoids a read-modify-write.
 */
#define SGI_IOC3_INT_STATUS     0x1c
#define SGI_IOC3_INT_ENABLE_SET 0x20
#define SGI_IOC3_INT_ENABLE_CLR 0x24

/*
 * Two seven-bit fields place the ports in the SuperIO window (byte offset
 * divided by eight); four more bits set the byte-bus strobe length.
 */
#define SGI_IOC3_CHIP_CONTROL   0x28
/*
 * Bits 19-22 are hardware-answered diagnostic status outside the writable
 * mask; the kernel's serial console setup polls bit 22 until it reads idle.
 */
#define SGI_IOC3_CHIP_CTRL_MASK 0x0007ffffu
#define SGI_IOC3_CHIP_CTRL_ARB_IDLE 0x00400000u

/*
 * MicroLAN (one-wire) control; the bus carries one part, the
 * ethernet-address memory. Register semantics and the read protocol:
 * docs/datasheets/ioc3.md and docs/specs/sgi-ioc3.rst.
 */
#define SGI_IOC3_MICROLAN_CONTROL 0x30
#define SGI_IOC3_MICROLAN_LEVEL (1u << 0)   /* sampled from the bus */
#define SGI_IOC3_MICROLAN_DONE  (1u << 1)   /* hardware sets this when done */
#define SGI_IOC3_MICROLAN_TIMING (~(SGI_IOC3_MICROLAN_LEVEL | \
                                    SGI_IOC3_MICROLAN_DONE))

/*
 * The pulse field (bits 10 up, microseconds low) tells slot kinds apart by
 * threshold; both guests' timing constants fall well clear of the bounds.
 */
#define SGI_IOC3_ONEWIRE_PULSE_SHIFT  10
#define SGI_IOC3_ONEWIRE_PULSE_RESET  400 /* at or above: a reset */
#define SGI_IOC3_ONEWIRE_PULSE_ZERO   40  /* at or above (below reset): a zero */

/*
 * ROM-selection commands. Linux's w1 core sends Skip ROM on a one-device
 * bus; OpenBSD's owmac always sends Match ROM. Both must work.
 */
#define SGI_IOC3_ONEWIRE_ROM_SEARCH   0xf0
#define SGI_IOC3_ONEWIRE_ROM_SKIP     0xcc
#define SGI_IOC3_ONEWIRE_ROM_MATCH    0x55

/*
 * Function commands. The two memory reads are one operation under two
 * names (OpenBSD plain, Linux CRC-named); Read Status serves the
 * redirection table owmac reads before trusting a page.
 */
#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY      0xf0
#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY_CRC  0xc3
#define SGI_IOC3_ONEWIRE_FN_READ_STATUS      0xaa

/*
 * Family 0x91 with serial top bits 0x5e7, both required by OpenBSD's
 * owmac_match(); the remaining serial bits are this model's own choice.
 */
#define SGI_IOC3_ONEWIRE_FAMILY  0x91
#define SGI_IOC3_ONEWIRE_SERIAL  0x5e7000000001ULL

/* Low 48 bits of the ROM identity; an oversize value fails realize. */
#define SGI_IOC3_ONEWIRE_SERIAL_MASK 0xffffffffffffULL

#define SGI_IOC3_ONEWIRE_PAGE_SIZE   32
#define SGI_IOC3_ONEWIRE_STATUS_SIZE 4
#define SGI_IOC3_ONEWIRE_ANSWER_SIZE (1 + SGI_IOC3_ONEWIRE_PAGE_SIZE + 1)

/* States the one-wire transaction sits in between a reset and the next one. */
#define SGI_IOC3_ONEWIRE_IDLE      0  /* nothing asked since the last reset */
#define SGI_IOC3_ONEWIRE_COMMAND   1  /* taking the eight bits of a ROM command */
#define SGI_IOC3_ONEWIRE_SEARCH    2  /* answering a search for parts */
#define SGI_IOC3_ONEWIRE_MATCH     3  /* taking the 64 bits of a Match ROM */
#define SGI_IOC3_ONEWIRE_FUNCTION  4  /* selected; taking a function command */
#define SGI_IOC3_ONEWIRE_OFFSET    5  /* taking the two bytes of a target address */
#define SGI_IOC3_ONEWIRE_ANSWER    6  /* shifting a prepared answer back out */

/*
 * Set/clear write aliases over one word, like the interrupt enables.
 * Bit 21 routes the MicroLAN engine onto pin 8; nothing on that bus works
 * until both guests' drivers set it, which they do first.
 */
#define SGI_IOC3_PIN_CONTROL_SET 0x34
#define SGI_IOC3_PIN_CONTROL_CLR 0x38
#define SGI_IOC3_PIN_MICROLAN_ENABLE (1u << 21)

#define SGI_IOC3_PIN_DATA       0x3c
#define SGI_IOC3_PINS           9

/*
 * One register per pin, each an alias onto one bit of the data register:
 * reads return that bit alone and writes change only it.
 */
#define SGI_IOC3_PIN_REG(n)     (0x40 + 4 * (n))
#define SGI_IOC3_PIN_REG_FIRST  SGI_IOC3_PIN_REG(0)
#define SGI_IOC3_PIN_REG_LAST   SGI_IOC3_PIN_REG(SGI_IOC3_PINS - 1)

#define SGI_IOC3_MAIN_SIZE      (SGI_IOC3_PIN_REG_LAST + 4)

/*
 * The PS/2 controller: a bit-level engine, not an 8042 clone, so there is
 * no command protocol -- attach only needs the registers to decode.
 */
#define SGI_IOC3_KBC_CONTROL        0x09c
#define SGI_IOC3_KBC_KBD_RECEIVE    0x0a0
#define SGI_IOC3_KBC_AUX_RECEIVE    0x0a4
#define SGI_IOC3_KBC_KBD_TRANSMIT   0x0a8
#define SGI_IOC3_KBC_AUX_TRANSMIT   0x0ac

#define SGI_IOC3_KBC_BASE           SGI_IOC3_KBC_CONTROL
#define SGI_IOC3_KBC_SIZE           (SGI_IOC3_KBC_AUX_TRANSMIT + 4 - \
                                     SGI_IOC3_KBC_BASE)
#define SGI_IOC3_KBC_REGS           (SGI_IOC3_KBC_SIZE / 4)

/*
 * The serial DMA block: a shared ring base, then seven registers per port.
 * The registers must answer during serial attach even for guests that then
 * drive the ports through the byte windows; the firmware's base I/O
 * diagnostic drives the engines themselves, in loopback.
 */
#define SGI_IOC3_SDMA_RING_HI       0x0b0
#define SGI_IOC3_SDMA_RING_LO       0x0b4
#define SGI_IOC3_SDMA_PORT_A        0x0b8
#define SGI_IOC3_SDMA_PORT_B        0x0d4

/* Offsets within one port's seven registers, from that port's base. */
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

/*
 * The low word carries the 4 KB-aligned base in bits 12 up and the ring
 * size select in bit 0 -- inside the register but outside the address.
 */
#define SGI_IOC3_SDMA_RING_4K       (1u << 0)
#define SGI_IOC3_SDMA_RING_ADDRESS  0xfffff000u
#define SGI_IOC3_SDMA_RING_1K_SIZE  0x400
#define SGI_IOC3_SDMA_RING_4K_SIZE  0x1000

/*
 * One base, four rings in fixed order (A tx, A rx, B tx, B rx): a ring's
 * address comes from port and direction and no register reads it back.
 */
#define SGI_IOC3_SDMA_RING_TX       0
#define SGI_IOC3_SDMA_RING_RX       1
#define SGI_IOC3_SDMA_RING_DIRS     2

/* The port control register. */
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

/*
 * Not stored: two hardware read-backs (timer running, pause taken) and two
 * commands that act and are gone (channel reset, receive drain -- which
 * IRIX polls to read back clear).
 */
#define SGI_IOC3_SDMA_CTRL_UNSTORED (SGI_IOC3_SDMA_TX_TIMER_RUN | \
                                     SGI_IOC3_SDMA_PAUSED | \
                                     SGI_IOC3_SDMA_RX_TO_MEMORY | \
                                     SGI_IOC3_SDMA_RESET)

/*
 * Indices address the ring in eight-byte units (pointer from bit 3, width
 * following ring size); the hardware-advanced pair also report the bytes
 * still inside the engine, and the rx consume index arms the timer.
 */
#define SGI_IOC3_SDMA_ARM_RX_TIMER  (1u << 31)
#define SGI_IOC3_SDMA_HELD_SHIFT    24
#define SGI_IOC3_SDMA_TX_HELD       (0xfu << SGI_IOC3_SDMA_HELD_SHIFT)
#define SGI_IOC3_SDMA_RX_HELD       (0x7u << SGI_IOC3_SDMA_HELD_SHIFT)

/* The receive timer: a twelve-bit reload, and the count read back above it. */
#define SGI_IOC3_SDMA_TIMER_RELOAD  0xfffu
#define SGI_IOC3_SDMA_TIMER_COUNT   0x0fff0000u
#define SGI_IOC3_SDMA_TIMER_COUNT_SHIFT 16

/* One tick of the receive timer's 16 kHz clock, in nanoseconds. */
#define SGI_IOC3_SDMA_TIMER_TICK_NS 62500

/*
 * Eight bytes, four characters: data bytes then their status/control
 * bytes, grouped so one word read tests all four validity flags.
 */
#define SGI_IOC3_SDMA_ENTRY_SIZE    8
#define SGI_IOC3_SDMA_ENTRY_CHARS   4

/*
 * A transmit control byte: a two-bit type for the data byte beside it,
 * plus an independent per-byte interrupt request.
 */
#define SGI_IOC3_SDMA_TX_TYPE       0xc0
#define SGI_IOC3_SDMA_TX_NOTHING    0x00
#define SGI_IOC3_SDMA_TX_CHARACTER  0x40
#define SGI_IOC3_SDMA_TX_MODEM      0x80
#define SGI_IOC3_SDMA_TX_DELAY      0xc0
#define SGI_IOC3_SDMA_TX_INTERRUPT  0x20

/* A receive entry's status byte. */
#define SGI_IOC3_SDMA_RX_OVERRUN    0x01
#define SGI_IOC3_SDMA_RX_PARITY     0x02
#define SGI_IOC3_SDMA_RX_FRAMING    0x04
#define SGI_IOC3_SDMA_RX_BREAK      0x08
#define SGI_IOC3_SDMA_RX_CTS        0x10
#define SGI_IOC3_SDMA_RX_CD         0x20
#define SGI_IOC3_SDMA_RX_MODEM_OK   0x40
#define SGI_IOC3_SDMA_RX_DATA_OK    0x80

/*
 * The 16550 shadow: a read-only mirror of the port's status and modem
 * lines. The DMA-side console driver reads carrier and clear-to-send from
 * here, not the byte window; the bit positions are the shadow's own,
 * distinct from the receive-entry status byte's.
 */
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

/*
 * Alias pages: one port's seven registers and nothing else, mappable into
 * user space; three registers are read-only through them.
 */
#define SGI_IOC3_SDMA_ALIAS_A       0x08000
#define SGI_IOC3_SDMA_ALIAS_B       0x0c000
#define SGI_IOC3_SDMA_ALIAS_SIZE    (16 * KiB)

/*
 * The serial DMA window ends where the ethernet block's begins, so the
 * size derives from that device's own base rather than restating it.
 */
#define SGI_IOC3_SDMA_SIZE          (SGI_IOC3_ENET_BASE - SGI_IOC3_SDMA_BASE)
#define SGI_IOC3_SDMA_REGS          (SGI_IOC3_SDMA_SIZE / 4)

/*
 * The ethernet buffer RAM: each 16-bit word in a 32-bit slot, data in bits
 * 0-15 and the stored parity bit at 16 -- 128 KB of RAM in 256 KB of
 * address space. The halves are separate storage; a window aliasing at its
 * midpoint reports half the RAM to the driver's size probe.
 */
#define SGI_IOC3_SSRAM_BASE     0x40000
#define SGI_IOC3_SSRAM_SIZE     0x40000
#define SGI_IOC3_SSRAM_WORDS    (SGI_IOC3_SSRAM_SIZE / 4)

#define SGI_IOC3_SSRAM_DATA     0x0000ffffu
#define SGI_IOC3_SSRAM_PARITY   0x00010000u
#define SGI_IOC3_SSRAM_STORED   (SGI_IOC3_SSRAM_DATA | SGI_IOC3_SSRAM_PARITY)

/*
 * Generated per read, not stored: set when the seventeen stored bits are
 * not even parity. The power-on diagnostic writes bad parity deliberately.
 */
#define SGI_IOC3_SSRAM_PARITY_ERROR 0x00020000u

/*
 * The byte bus's four device selects. The Origin 200 wires only the first
 * (the Timekeeper clock, a 32 KB window); the other three stay holes.
 */
#define SGI_IOC3_BYTEBUS_DEV0   0x80000

/*
 * A serial port owns nine consecutive bits of the SuperIO interrupt status
 * register, and port B's nine sit directly above port A's.
 */
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

/*
 * The pass-through bits follow the 16550's own level rather than latching:
 * a write cannot clear one; the guest's own register reads quiet it.
 */
#define SGI_IOC3_INT_SERIAL_A \
    SGI_IOC3_SERIAL_SOURCE(0, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_SERIAL_B \
    SGI_IOC3_SERIAL_SOURCE(1, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_PASSTHROUGH (SGI_IOC3_INT_SERIAL_A | SGI_IOC3_INT_SERIAL_B)

/*
 * The high-water bits are levels too: each follows whether the mark's
 * worth of characters is waiting now. IRIX's ring console never
 * acknowledges it and depends on that.
 */
#define SGI_IOC3_INT_RX_HIGH_WATER_LEVEL \
    (SGI_IOC3_SERIAL_SOURCE(0, SGI_IOC3_INT_RX_HIGH_WATER) | \
     SGI_IOC3_SERIAL_SOURCE(1, SGI_IOC3_INT_RX_HIGH_WATER))

/*
 * What an alias page's handlers and the receive timers are given: neither
 * can tell which port reached it from anything but its opaque.
 */
typedef struct SGIIOC3SerialAlias {
    SGIIOC3State *chip;
    unsigned int port;
} SGIIOC3SerialAlias;

struct SGIIOC3State {
    PCIDevice parent_obj;

    /*
     * The whole window; anything not modelled is a hole that reports,
     * never a zero.
     */
    MemoryRegion bar;

    SerialState serial[SGI_IOC3_SERIAL_PORTS];
    MemoryRegion serial_io[SGI_IOC3_SERIAL_PORTS];

    /* The main window's interrupt registers, and the line they drive. */
    MemoryRegion main;
    qemu_irq superio_irq;
    uint32_t int_status;
    uint32_t int_enable;

    /*
     * MicroLAN and pin block. Only pin 8 routing and the engine's own
     * completion have behavior; the rest is storage.
     */
    uint32_t microlan_control;
    uint32_t pin_control;
    uint32_t pin_data;

    /*
     * The low 48 bits of the one-wire ROM, a property so each node's board
     * gets its own serial -- the firmware must not merge two nodes whose
     * parts answer alike. Configuration, not migrated state.
     */
    uint64_t onewire_serial;

    /*
     * The one-wire transaction state machine. Not migrated, the Bridge's
     * choice for its identical machinery: a transaction caught mid-slot is
     * one the guest retries from a reset, and zero reads as idle.
     */
    uint8_t onewire_phase;
    uint8_t onewire_bit;
    uint8_t onewire_command;
    uint64_t onewire_match;
    uint8_t onewire_args[2];
    uint8_t onewire_argn;
    uint8_t onewire_out[SGI_IOC3_ONEWIRE_ANSWER_SIZE];
    uint8_t onewire_out_len;
    uint16_t onewire_outbit;

    /*
     * The ethernet block (hw/net/sgi-ioc3-eth.c), a child because that is
     * the topology: it has no address of its own outside this BAR.
     */
    SGIIOC3EthState eth;

    /*
     * The serial DMA register file; the engines advance the two
     * hardware-owned indices in here as they walk their rings.
     */
    MemoryRegion sdma;
    uint32_t sdma_reg[SGI_IOC3_SDMA_REGS];

    /* The alias pages and the per-port opaque their handlers receive. */
    MemoryRegion sdma_alias[SGI_IOC3_SERIAL_PORTS];
    SGIIOC3SerialAlias sdma_alias_port[SGI_IOC3_SERIAL_PORTS];

    /*
     * Whether this port's engine is already on the stack: a loopback walk
     * re-enters through the port's own interrupt, and a second walk would
     * post the packer's half-filled entry mid-burst. Only ever true inside
     * a call, so not migrated.
     */
    bool sdma_busy[SGI_IOC3_SERIAL_PORTS];

    /*
     * The receive packer: a ring entry under construction and its held
     * count -- what the produce index's held-character field reports.
     */
    uint8_t sdma_packer[SGI_IOC3_SERIAL_PORTS][SGI_IOC3_SDMA_ENTRY_SIZE];
    uint8_t sdma_packed[SGI_IOC3_SERIAL_PORTS];

    /*
     * Whether anything reached the receive ring since its sources were
     * last raised: the delivery sources fire per burst, not per entry.
     */
    bool sdma_delivered[SGI_IOC3_SERIAL_PORTS];

    /*
     * Per-port receive idle timer, armed through bit 31 of the rx consume
     * index; the deadline migrates in the timer, the read-back count is
     * derived from it.
     */
    QEMUTimer *sdma_rx_timer[SGI_IOC3_SERIAL_PORTS];

    /*
     * The chip's own PCI address space, so ring transfers go through the
     * Bridge's translation; the ethernet block gets the same space.
     */
    AddressSpace *dma_as;

    /*
     * Storage: the port-offset fields are a multiplexer on real silicon,
     * and every guest programs the two offsets this model implements.
     */
    uint32_t chip_control;

    /* PS/2: only control/status stores; nothing is plugged in. */
    MemoryRegion kbc;
    uint32_t kbc_control;

    /*
     * The buffer RAM, one entry per 16-bit word, storing the seventeen
     * bits the chip has; the width is invisible to a boot and only a test
     * checks it.
     */
    MemoryRegion ssram;
    uint32_t ssram_word[SGI_IOC3_SSRAM_WORDS];

    /*
     * The clock on the byte bus's first select, a child because the bus
     * belongs to this controller.
     */
    M48T35State rtc;
};

#endif
