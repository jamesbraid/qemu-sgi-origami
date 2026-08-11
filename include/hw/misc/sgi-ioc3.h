/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 */

#ifndef HW_MISC_SGI_IOC3_H
#define HW_MISC_SGI_IOC3_H

#include "hw/core/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/char/serial.h"
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
 * that this status register has no ethernet bit in it at all. Only the SuperIO
 * line is modelled: the ethernet block does not exist here.
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
 * The ethernet block. Its media access controller is SGI's own design rather
 * than a catalogue part, so the datasheet's register table is the whole
 * specification and there is no vendor document behind it to consult.
 *
 * This is a window of its own rather than an extension of the main one,
 * because the offsets between the pin registers and this block belong to the
 * parallel port, the PS/2 controller and the serial DMA engines, none of which
 * are modelled. Widening the main window would turn those into zeros; leaving
 * a hole keeps an access to them reported, which is this device's stated rule.
 */
#define SGI_IOC3_ENET_MAC_CONTROL   0x0f0
#define SGI_IOC3_ENET_INT_STATUS    0x0f4
#define SGI_IOC3_ENET_INT_ENABLE    0x0f8
#define SGI_IOC3_ENET_RX_CONTROL    0x0fc
#define SGI_IOC3_ENET_RX_RING_HI    0x100
#define SGI_IOC3_ENET_RX_RING_LO    0x104
#define SGI_IOC3_ENET_RX_BARRIER    0x108
#define SGI_IOC3_ENET_RX_CONSUME    0x10c
#define SGI_IOC3_ENET_RX_PRODUCE    0x110
#define SGI_IOC3_ENET_RX_TIMER      0x114
#define SGI_IOC3_ENET_TX_CONTROL    0x118
#define SGI_IOC3_ENET_RANDOM_SEED   0x11c
#define SGI_IOC3_ENET_COUNTERS      0x120
#define SGI_IOC3_ENET_BUFFER_INDEX  0x124
#define SGI_IOC3_ENET_TX_RING_HI    0x128
#define SGI_IOC3_ENET_TX_RING_LO    0x12c
#define SGI_IOC3_ENET_TX_CONSUME    0x130
#define SGI_IOC3_ENET_TX_PRODUCE    0x134
#define SGI_IOC3_ENET_MAC_ADDR_HI   0x138
#define SGI_IOC3_ENET_MAC_ADDR_LO   0x13c
#define SGI_IOC3_ENET_HASH_HI       0x140
#define SGI_IOC3_ENET_HASH_LO       0x144
#define SGI_IOC3_ENET_MII_CONTROL   0x148
#define SGI_IOC3_ENET_MII_DATA_R    0x14c
#define SGI_IOC3_ENET_MII_DATA_W    0x150

#define SGI_IOC3_ENET_BASE          SGI_IOC3_ENET_MAC_CONTROL

/* The serial DMA block runs from its ring base up to where the ethernet
 * block starts, which is how the two windows meet exactly.
 */
#define SGI_IOC3_SDMA_SIZE          (SGI_IOC3_ENET_BASE - SGI_IOC3_SDMA_BASE)
#define SGI_IOC3_SDMA_REGS          (SGI_IOC3_SDMA_SIZE / 4)

#define SGI_IOC3_ENET_SIZE          (SGI_IOC3_ENET_MII_DATA_W + 4 - \
                                     SGI_IOC3_ENET_BASE)
#define SGI_IOC3_ENET_REGS          (SGI_IOC3_ENET_SIZE / 4)

/* Bit 10 of the media-independent interface control register starts a read,
 * and bit 11 reads back as busy while one is in flight. Nothing here takes
 * time, so a read is complete by the time the guest can look and busy never
 * reads set -- a driver that polls it therefore stops on its first look
 * rather than running its timeout out.
 */
#define SGI_IOC3_ENET_MII_READ      (1u << 10)
#define SGI_IOC3_ENET_MII_BUSY      (1u << 11)

/*
 * No physical-layer chip answers. A driver scans all thirty-two addresses
 * reading identifier registers, and an absent one reads as all-ones on a real
 * bus, so that is what a read returns.
 */
#define SGI_IOC3_ENET_MII_ABSENT    0xffffu

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
     * The ethernet block, held as one array indexed by register rather than as
     * two dozen named fields. Almost every register in it is plain storage a
     * driver writes and reads back, and the few that are not are handled by
     * name at their offsets, so naming the rest individually would add bulk
     * without adding meaning. The block moves no packets: it exists so that a
     * guest configuring the controller finds registers where the hardware has
     * them, which is what its attach needs and all that is claimed here.
     */
    MemoryRegion enet;
    uint32_t enet_reg[SGI_IOC3_ENET_REGS];

    /*
     * The serial DMA block, held the same way and for the same reason: nearly
     * every register is storage, and the few that are not are the ones a
     * driver cannot place because hardware owns them. No ring is walked and no
     * character moves through here -- the ports themselves are driven through
     * their 16550 windows, which is where both guests actually send bytes.
     */
    MemoryRegion sdma;
    uint32_t sdma_reg[SGI_IOC3_SDMA_REGS];
};

#endif
