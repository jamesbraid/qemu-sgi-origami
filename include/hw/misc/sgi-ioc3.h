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
 * The reset value of the combined revision-and-class register is
 * 0xFF000000: class 0xFF ("does not fit any defined class"), sub-class 0x00,
 * revision 0x00. SGI's own diagnostic checks exactly this value on a fresh
 * chip and treats the low nibble as unwritable.
 */
#define SGI_IOC3_CLASS_ID       0xFF00
#define SGI_IOC3_REVISION       0x00
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
 * MicroLAN (one-wire) control and the pin block it shares the window with.
 * Both guests route the MicroLAN engine onto pin 8 as their first act on the
 * chip, so this has to decode and answer before any one-wire transaction can
 * even be attempted — see docs/datasheets/ioc3.md's "GPIO and MicroLAN"
 * section for the full register semantics this models.
 *
 * The bus itself answers silently: every slot completes (never leaving a
 * guest polling forever) but no part responds, which is a known, already
 * -documented interim rather than a guess — see that datasheet section's
 * "the bus must answer" discussion, and task #13, which owns fitting the
 * real ethernet-address part this chip should present. A slot that never
 * finishes is what this file has to rule out; what the slot reports when
 * it does finish is a separate, larger piece of work.
 */
#define SGI_IOC3_MICROLAN_CONTROL 0x30
#define SGI_IOC3_MICROLAN_LEVEL (1u << 0)   /* sampled from the bus */
#define SGI_IOC3_MICROLAN_DONE  (1u << 1)   /* hardware sets this when done */
#define SGI_IOC3_MICROLAN_TIMING (~(SGI_IOC3_MICROLAN_LEVEL | \
                                    SGI_IOC3_MICROLAN_DONE))

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
 * far enough to decide it does not want them.
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
 * The two serial ports' bits, which are pass-throughs: they follow the 16550's
 * own interrupt level rather than latching, so a write cannot clear them and
 * what really quiets one is the register read the guest's serial driver does.
 */
#define SGI_IOC3_INT_SERIAL_A   (1u << 6)
#define SGI_IOC3_INT_SERIAL_B   (1u << 15)
#define SGI_IOC3_INT_PASSTHROUGH (SGI_IOC3_INT_SERIAL_A | SGI_IOC3_INT_SERIAL_B)

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
     * The ethernet block: registers, engine, network backend and
     * transceiver, all of it in hw/net/sgi-ioc3-eth.c now. A child of this
     * chip because that is the topology -- the block has no address of its
     * own, reached only through a window inside this chip's BAR -- the same
     * reason the real-time clock below is a child rather than a machine
     * device the chip merely forwards to.
     */
    SGIIOC3EthState eth;

    /*
     * The serial DMA block, held the same way and for the same reason: nearly
     * every register is storage, and the few that are not are the ones a
     * driver cannot place because hardware owns them. No ring is walked and no
     * character moves through here -- the ports themselves are driven through
     * their 16550 windows, which is where both guests actually send bytes.
     */
    MemoryRegion sdma;
    uint32_t sdma_reg[SGI_IOC3_SDMA_REGS];

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
