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
 * The identity a guest's config-space read gets. OpenBSD binds its driver on
 * the vendor/device pair alone and reads nothing else here; mainline Linux
 * additionally reads the subsystem identity, or — for a Bridge-attached chip
 * like this one — the synthetic value the Bridge substitutes from the
 * board's own one-wire line, so this register's own contents only matter on
 * a path that does not go through the Bridge.
 */
#define SGI_IOC3_VENDOR_ID      0x10A9
#define SGI_IOC3_DEVICE_ID      0x0003
/*
 * The selected Origin 200 IOC3 reports class 0xFF ("does not fit any defined
 * class"), sub-class 0x00, and PCI revision 0x01.  A diagnostic table for an
 * earlier revision-0 part records 0xFF000000; that is not this machine's
 * fitted identity.
 */
#define SGI_IOC3_CLASS_ID       0xFF00
#define SGI_IOC3_REVISION       0x01
/* The Origin 200's IP29 System Board, the only board this machine models. */
#define SGI_IOC3_SUBSYSTEM_ID   0xC303

/*
 * One base address register covers the whole chip, and every offset in the
 * datasheet is measured from it. The highest thing in the map is the fourth
 * byte-bus device select at 0x0e0000 with a 32 KB window behind it, so a
 * megabyte is the smallest power of two that covers the chip.
 */
#define SGI_IOC3_BAR_SIZE       (1 * MiB)

/* The two serial ports, at their offsets in the byte-wide SuperIO window. */
#define SGI_IOC3_SERIAL_PORTS   2
#define SGI_IOC3_SERIAL_A       0x020178
#define SGI_IOC3_SERIAL_B       0x020170

/*
 * The SuperIO interrupt block, in the chip's main register window.
 *
 * The chip presents two interrupt outputs rather than one — an ethernet line
 * and a SuperIO line — and each has its own status and enable pair. This is the
 * SuperIO pair, and the register-level reason the two outputs are separate is
 * that this status register has no ethernet bit in it at all. The ethernet
 * pair lives in the ethernet block's own window and drives the chip's
 * ordinary PCI interrupt pin rather than a wire of its own, unlike this one.
 *
 * The enable mask has two write aliases over one register, so that a guest can
 * turn a source on or off without a read-modify-write.
 */
#define SGI_IOC3_INT_STATUS     0x1c
#define SGI_IOC3_INT_ENABLE_SET 0x20
#define SGI_IOC3_INT_ENABLE_CLR 0x24

/*
 * The chip control register. Its two seven-bit fields tell the serial DMA
 * engines where the ports live inside the SuperIO window, each as that port's
 * byte offset divided by eight, and four more bits set the byte bus strobe
 * length. The firmware writes it on every boot and no guest reads those fields
 * back, so a model that dropped them would produce an identical transcript --
 * which is why the qtest asserts the firmware's own value rather than a boot
 * doing it.
 */
#define SGI_IOC3_CHIP_CONTROL   0x28
/*
 * Bits 19-21 report which internal master wants the PCI bus and bit 22 reports
 * the PCI arbiter idle. Both are diagnostic status the hardware answers, so the
 * writable mask stops below them and a write cannot store them. The model has
 * no serial-DMA engine driving PCI traffic, so the arbiter is always idle and
 * the read reports it. On our machine's boot the kernel's serial console setup
 * looped on this register and did not proceed until bit 22 read idle.
 */
#define SGI_IOC3_CHIP_CTRL_MASK 0x0007ffffu
#define SGI_IOC3_CHIP_CTRL_ARB_IDLE 0x00400000u

/*
 * MicroLAN (one-wire) control and the pin block it shares the window with.
 * Both guests route the MicroLAN engine onto pin 8 as their first act on the
 * chip, so this has to decode and answer before any one-wire transaction can
 * even be attempted — see docs/datasheets/ioc3.md's "GPIO and MicroLAN"
 * section for the full register semantics this models.
 *
 * The bus carries one part, the ethernet-address memory both guests need to
 * bring up a usable station address — see that datasheet's "The
 * ethernet-address part's read protocol" section for the transaction this
 * register drives and the one-wire state machine below implements.
 */
#define SGI_IOC3_MICROLAN_CONTROL 0x30
#define SGI_IOC3_MICROLAN_LEVEL (1u << 0)   /* sampled from the bus */
#define SGI_IOC3_MICROLAN_DONE  (1u << 1)   /* hardware sets this when done */
#define SGI_IOC3_MICROLAN_TIMING (~(SGI_IOC3_MICROLAN_LEVEL | \
                                    SGI_IOC3_MICROLAN_DONE))

/*
 * A write's pulse field (bits 10 and up, microseconds holding the bus low)
 * is what tells a reset slot from an ordinary read/write-one/write-zero
 * slot apart; nothing here tries to reproduce microseconds, only to bin a
 * guest's own timings the way a real slot would land. See the datasheet
 * section named above for both guests' actual timing constants, all of
 * which fall well clear of this boundary.
 */
#define SGI_IOC3_ONEWIRE_PULSE_SHIFT  10
#define SGI_IOC3_ONEWIRE_PULSE_RESET  400 /* at or above: a reset */
#define SGI_IOC3_ONEWIRE_PULSE_ZERO   40  /* at or above (below reset): a zero */

/* ROM-selection commands, and the two guests' different ways of using them:
 * Linux's w1 core sends Skip ROM whenever only one device is registered,
 * which is always true of this bus; OpenBSD's owmac driver always sends
 * Match ROM instead, never Skip ROM. Both must work.
 */
#define SGI_IOC3_ONEWIRE_ROM_SEARCH   0xf0
#define SGI_IOC3_ONEWIRE_ROM_SKIP     0xcc
#define SGI_IOC3_ONEWIRE_ROM_MATCH    0x55

/* Function commands once a part is selected. The two memory-read commands
 * are the same operation under two names -- OpenBSD's owmac issues the
 * plain one, Linux's w1_ds2502_read_page the CRC-named one -- and both are
 * answered identically. Read Status is OpenBSD-only, for the redirection
 * table its owmac_read_redirect() reads before trusting a page.
 */
#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY      0xf0
#define SGI_IOC3_ONEWIRE_FN_READ_MEMORY_CRC  0xc3
#define SGI_IOC3_ONEWIRE_FN_READ_STATUS      0xaa

/* The part's fixed identity: family 0x91 (the custom bit set over type
 * 0x11) with the serial number's top twelve bits 0x5e7, both required by
 * OpenBSD's owmac_match() and observed on real hardware. The remaining
 * serial bits are this model's own choice, not a transcription of a real
 * unit's serial number -- see the datasheet section named above.
 */
#define SGI_IOC3_ONEWIRE_FAMILY  0x91
#define SGI_IOC3_ONEWIRE_SERIAL  0x5e7000000001ULL

/* The serial is the low forty-eight bits of the ROM identity; a value that
 * does not fit is rejected at realize the same way the Bridge's is. */
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
 * The pin control register has two write-only aliases over one word, the
 * same shape as the interrupt-enable pair above: a guest sets or clears
 * individual bits without a read-modify-write. Bit 21 is the one both
 * guests' one-wire drivers set immediately — it routes the MicroLAN engine
 * onto pin 8, and nothing on that bus works until it is.
 */
#define SGI_IOC3_PIN_CONTROL_SET 0x34
#define SGI_IOC3_PIN_CONTROL_CLR 0x38
#define SGI_IOC3_PIN_MICROLAN_ENABLE (1u << 21)

#define SGI_IOC3_PIN_DATA       0x3c
#define SGI_IOC3_PINS           9

/*
 * One register per pin, at 0x40 plus four times the pin number, so a guest
 * can drive or read a single pin without a read-modify-write of the data
 * register above. Each is an alias onto one bit of that same register
 * rather than storage of its own — reading returns that bit alone, and
 * writing changes only that bit, which is what "without a read-modify-
 * write" means for the guest even though this model still does one
 * internally.
 */
#define SGI_IOC3_PIN_REG(n)     (0x40 + 4 * (n))
#define SGI_IOC3_PIN_REG_FIRST  SGI_IOC3_PIN_REG(0)
#define SGI_IOC3_PIN_REG_LAST   SGI_IOC3_PIN_REG(SGI_IOC3_PINS - 1)

#define SGI_IOC3_MAIN_SIZE      (SGI_IOC3_PIN_REG_LAST + 4)

/*
 * The PS/2 keyboard and mouse controller: a control and status register and
 * then a receive and a transmit register for each of the two ports.
 *
 * It is a bit-level engine rather than a command-level 8042 clone, so there is
 * no command protocol to answer -- what a guest needs during attach is that
 * the registers decode. Nothing is plugged in, so both receive registers hold
 * no valid bytes, which is what an empty port reads as.
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
 * The serial DMA block: a ring base shared by both ports, then seven registers
 * for each port four bytes apart. It ends where the ethernet block begins, so
 * the two windows meet without a gap between them.
 *
 * A guest reaches this while attaching its serial child even when it goes on
 * to drive the ports through the byte-wide 16550 windows instead, which is
 * what both of ours do -- the registers have to answer before a driver gets
 * far enough to decide it does not want them. The firmware's own base I/O
 * diagnostic goes further and drives the engines, in loopback, on both ports.
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
 * The ring base pair. The low word carries a 4 KB-aligned base in bits 12 up
 * and, in its bottom bit, which of the two ring sizes is selected -- so the
 * size bit sits inside the register but outside the address, and a model
 * or-ing the whole word into its arithmetic lands one ring-size-select bit
 * away from where the guest put its rings.
 */
#define SGI_IOC3_SDMA_RING_4K       (1u << 0)
#define SGI_IOC3_SDMA_RING_ADDRESS  0xfffff000u
#define SGI_IOC3_SDMA_RING_1K_SIZE  0x400
#define SGI_IOC3_SDMA_RING_4K_SIZE  0x1000

/*
 * One base covers four rings in a fixed order -- port A transmit, port A
 * receive, port B transmit, port B receive -- so a ring's address comes from
 * its port and direction and there is no register anywhere that reads it
 * back.
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
 * Three of the control register's bits are not settings the guest stores. Two
 * are hardware's own read-backs -- the transmit timer running, and the pause
 * having taken effect -- and the third is a command that acts and is gone.
 */
#define SGI_IOC3_SDMA_CTRL_UNSTORED (SGI_IOC3_SDMA_TX_TIMER_RUN | \
                                     SGI_IOC3_SDMA_PAUSED | \
                                     SGI_IOC3_SDMA_RESET)

/*
 * The index registers address the ring in eight-byte units, so the pointer
 * occupies bits three upward and its width follows the ring size. The two
 * hardware advances report how many bytes are still inside the engine
 * alongside the pointer, and the receive consume index carries the bit that
 * arms the receive timer.
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
 * A ring entry is eight bytes carrying four characters: four data bytes, then
 * four status-or-control bytes, one for each of them. Software can test all
 * four validity flags with a single word read of the status half, which is why
 * the two halves are grouped rather than interleaved.
 */
#define SGI_IOC3_SDMA_ENTRY_SIZE    8
#define SGI_IOC3_SDMA_ENTRY_CHARS   4

/*
 * A transmit entry's control byte: a two-bit type saying what the data byte
 * beside it is, and -- independent of the type -- a request for an interrupt
 * once that byte has gone out.
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
 * The 16550 shadow register: a read-only mirror of the port's own status and
 * modem lines, so software driving the DMA rings never has to reach the
 * byte-wide window. The low byte mirrors the 16550 line-status register one for
 * one -- data-ready, the error latches, transmit-holding-empty and
 * transmit-shift-empty -- while the modem levels and the driven outputs sit
 * higher up. The DMA-side console driver reads carrier-detect and
 * clear-to-send from here rather than from the byte-wide modem-status register,
 * so a shadow that did not mirror them would report the line dead. These are
 * the shadow's own bit positions, distinct from the receive-entry status byte
 * above, which carries clear-to-send and carrier at different bits.
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
 * The alias pages, one per port, each exposing that port's seven registers and
 * nothing else so a privileged process can map them into user space without
 * the rest of the chip. They are additional views of the same state rather
 * than storage, and three registers are read-only through them where they are
 * not through the main block.
 */
#define SGI_IOC3_SDMA_ALIAS_A       0x08000
#define SGI_IOC3_SDMA_ALIAS_B       0x0c000
#define SGI_IOC3_SDMA_ALIAS_SIZE    (16 * KiB)

/*
 * The ethernet block sits behind SGI_IOC3_ENET_BASE (hw/net/sgi-ioc3-eth.h)
 * in the chip's window, but the block itself -- registers, engine, network
 * backend and transceiver -- is its own device now (hw/net/sgi-ioc3-eth.c,
 * docs/component-taxonomy.md). What stays here is the one thing a sibling
 * block still needs: where the serial DMA window ends, which is where the
 * ethernet one begins, so this is derived from that device's own base
 * rather than restating the offset.
 */
#define SGI_IOC3_SDMA_SIZE          (SGI_IOC3_ENET_BASE - SGI_IOC3_SDMA_BASE)
#define SGI_IOC3_SDMA_REGS          (SGI_IOC3_SDMA_SIZE / 4)

/*
 * The ethernet buffer RAM, reached through the diagnostic window that gives
 * each of its sixteen-bit words a thirty-two-bit slot of its own: bits 0-15
 * are the data, bit 16 is the parity bit stored beside them, and nothing owns
 * the bits above. So the window's quarter-megabyte of address space is 65,536
 * slots holding 65,536 sixteen-bit words -- 128 KB of RAM in 256 KB of
 * address space, which is the whole of what an Origin 200 populates.
 *
 * Address space and RAM being different quantities is the thing to hold on to
 * here, because every driver sizing this RAM prints the RAM figure and every
 * header states the address figure. A chip with the smaller RAM answers over
 * the first half of the window only, so the halves being separate storage is
 * the entire content of the size probe: a window that aliased at its midpoint
 * would report half the RAM while looking, from every other angle, like
 * memory that works.
 */
#define SGI_IOC3_SSRAM_BASE     0x40000
#define SGI_IOC3_SSRAM_SIZE     0x40000
#define SGI_IOC3_SSRAM_WORDS    (SGI_IOC3_SSRAM_SIZE / 4)

#define SGI_IOC3_SSRAM_DATA     0x0000ffffu
#define SGI_IOC3_SSRAM_PARITY   0x00010000u
#define SGI_IOC3_SSRAM_STORED   (SGI_IOC3_SSRAM_DATA | SGI_IOC3_SSRAM_PARITY)

/*
 * The checker's answer, which is generated on every read rather than stored:
 * set when the seventeen stored bits do not carry even parity. SGI's power-on
 * diagnostic writes deliberately bad parity and reads this bit back to decide
 * whether the chip's parity logic works, so a window that only stored what was
 * written fails the board.
 */
#define SGI_IOC3_SSRAM_PARITY_ERROR 0x00020000u

/*
 * The byte bus: four device selects for byte-wide parts outside the chip. The
 * Origin 200 wires only the first, and what hangs off it is the machine's
 * real-time clock -- a Timekeeper, directly addressed, with a flat 32 KB
 * window. The other three selects are other machines' wiring and stay a hole
 * here rather than answering.
 *
 * The window belongs to this chip and the part behind it is a device of its
 * own, which is what the bus is: this model decodes the select and forwards,
 * and the clock answers.
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
 * The two ports' pass-through bits, which are the odd ones out among the
 * eighteen: they follow the 16550's own interrupt level rather than latching,
 * so a write cannot clear them and what really quiets one is the register read
 * the guest's serial driver does.
 */
#define SGI_IOC3_INT_SERIAL_A \
    SGI_IOC3_SERIAL_SOURCE(0, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_SERIAL_B \
    SGI_IOC3_SERIAL_SOURCE(1, SGI_IOC3_INT_PASSTHROUGH_SOURCE)
#define SGI_IOC3_INT_PASSTHROUGH (SGI_IOC3_INT_SERIAL_A | SGI_IOC3_INT_SERIAL_B)

/*
 * What one alias page's handlers are given: the chip, and which port's seven
 * registers that page exposes. The receive timers' callbacks are handed the
 * same pair for the same reason -- neither can tell which port reached it
 * from anything but its opaque.
 */
typedef struct SGIIOC3SerialAlias {
    SGIIOC3State *chip;
    unsigned int port;
} SGIIOC3SerialAlias;

struct SGIIOC3State {
    PCIDevice parent_obj;

    /*
     * The chip's whole window. Everything modelled is a subregion of this, and
     * everything not modelled is a hole rather than a zero: an access to an
     * offset we have not implemented reaches no device and is reported, which
     * is what a guest asking an unanswerable question should get.
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
     * The MicroLAN engine and the pin block it shares a window with. Only
     * pin_control's bit 21 (MicroLAN routed to pin 8) and microlan_control's
     * own completion have any behaviour modelled yet; the rest is plain
     * storage so a guest reading back what it wrote sees what it wrote.
     */
    uint32_t microlan_control;
    uint32_t pin_control;
    uint32_t pin_data;

    /*
     * The board identity this part reports: the low forty-eight bits of the
     * one-wire ROM. A property so the machine can give each node's base I/O
     * board its own laser-programmed serial -- the same identity the Bridge
     * carries and for the same reason, that the firmware must not merge two
     * physical nodes whose parts happen to answer alike. Configuration, not
     * migrated state, so it is not in the VMState below.
     */
    uint64_t onewire_serial;

    /*
     * The one-wire transaction state machine sitting behind the MicroLAN
     * engine above, driving the ethernet-address part -- see
     * docs/datasheets/ioc3.md's "The ethernet-address part's read
     * protocol". Not part of this device's migrated state, the same
     * choice the Bridge's identical machinery already makes: a
     * transaction caught mid-slot by a migration is one a guest retries
     * from a reset, and every field here reads as "idle" at its zero
     * value.
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
     * The ethernet block: registers, engine, network backend and
     * transceiver, all of it in hw/net/sgi-ioc3-eth.c now. A child of this
     * chip because that is the topology -- the block has no address of its
     * own, reached only through a window inside this chip's BAR -- the same
     * reason the real-time clock below is a child rather than a machine
     * device the chip merely forwards to.
     */
    SGIIOC3EthState eth;

    /*
     * The serial DMA block's register file. Nearly every register is
     * storage, and the few that are not are the ones a driver cannot place
     * because hardware owns them: the engines advance the two hardware-owned
     * indices in here as they walk their rings.
     */
    MemoryRegion sdma;
    uint32_t sdma_reg[SGI_IOC3_SDMA_REGS];

    /*
     * The two alias pages, and the per-port opaque each one hands its
     * handlers. A page's own handlers cannot tell which region they were
     * reached through, so the port travels in the opaque rather than being
     * recovered from the address.
     */
    MemoryRegion sdma_alias[SGI_IOC3_SERIAL_PORTS];
    SGIIOC3SerialAlias sdma_alias_port[SGI_IOC3_SERIAL_PORTS];

    /*
     * Whether this port's engine is already on the stack. A transmit walk in
     * loopback hands the port's own receiver a character inside the register
     * write that sends it, and the register file answers by raising the
     * port's interrupt, which reaches this chip and would start a second walk
     * underneath the first -- one that would post the packer's half-filled
     * entry in the middle of the burst that is still filling it. Reading a
     * character out of the port does the same thing one layer down. Not
     * migrated: it is only ever true while a call is in progress, and no
     * migration happens inside one.
     */
    bool sdma_busy[SGI_IOC3_SERIAL_PORTS];

    /*
     * The receive packer: a ring entry under construction, and how many of
     * its four characters are in it. An entry reaches memory once four
     * characters have been packed into it, which is what the produce index's
     * own held-character count is reporting -- an engine writing an entry per
     * character would hold nothing and give a driver four times the entries
     * for the same text.
     */
    uint8_t sdma_packer[SGI_IOC3_SERIAL_PORTS][SGI_IOC3_SDMA_ENTRY_SIZE];
    uint8_t sdma_packed[SGI_IOC3_SERIAL_PORTS];

    /*
     * Whether anything has reached the receive ring since the sources for it
     * were last raised. The two a delivery prompts a driver with are raised
     * at the end of a burst rather than per entry, so this is what tells an
     * idle port from one that has just filled the ring.
     */
    bool sdma_delivered[SGI_IOC3_SERIAL_PORTS];

    /*
     * Each port's receive timer: armed through bit 31 of the receive consume
     * index, counting receive idleness at 16 kHz from the reload the guest
     * programmed, and raising the receive-timer source on expiry while
     * characters are waiting. The deadline lives in the timer itself and
     * migrates with it; the count a guest reads back is derived from it.
     */
    QEMUTimer *sdma_rx_timer[SGI_IOC3_SERIAL_PORTS];

    /*
     * Where the serial engines read and write their rings. It is the chip's
     * own PCI address space, so every transfer goes through the Bridge's
     * translation the way the guest programmed it -- the ethernet block is
     * handed the same space for the same reason.
     */
    AddressSpace *dma_as;

    /*
     * The chip control register, which is storage here: its port-offset
     * fields are a multiplexer on real silicon, and the firmware and both
     * guests program them to the two offsets this model already implements.
     */
    uint32_t chip_control;

    /*
     * The PS/2 controller. Only its control and status register holds anything
     * a guest wrote; the receive pair reads empty because no device is plugged
     * in, and the transmit pair takes bytes nothing carries away.
     */
    MemoryRegion kbc;
    uint32_t kbc_control;

    /*
     * The ethernet buffer RAM, one array entry per sixteen-bit word so that a
     * word and the slot it answers in stay the same thing. This is memory
     * rather than registers: the block's own engines would fill it with
     * received frames on a chip that moved any, and what a guest does with it
     * here is size it. Held as storage that keeps seventeen bits, which is
     * what the chip has, rather than as a plain thirty-two-bit array, because
     * the width is invisible to a boot -- every driver writes values that fit
     * -- and so is only ever checked by a test.
     */
    MemoryRegion ssram;
    uint32_t ssram_word[SGI_IOC3_SSRAM_WORDS];

    /*
     * The real-time clock on the byte bus's first device select. It is a child
     * of this chip rather than a device the machine wires up, because that is
     * the topology: the bus belongs to the controller and a guest reaches the
     * clock only through this chip's window.
     */
    M48T35State rtc;
};

#endif
