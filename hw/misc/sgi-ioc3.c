/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the serial console first.
 *
 * The chip is one PCI device with a single base address register covering
 * everything it has, and the console lives in a window near the top of that
 * range which is unlike the rest of the chip. The SuperIO window at offset
 * 0x20000 holds byte-wide registers a single byte apart, where the chip's own
 * blocks are thirty-two bits wide. Getting that stride wrong is the most
 * likely way to produce a console that emits nothing, so the two ports declare
 * their access width explicitly rather than inheriting a default.
 *
 * Only the two serial ports are modelled here. The rest of the chip's window
 * is deliberately left as a hole, so an access to something unimplemented
 * reaches no device and is reported, rather than decoding and returning a
 * plausible zero. On a machine with one node, one processor and no I/O, zero
 * is the right answer to a great many questions, which is exactly what makes
 * answering all of them dangerous.
 *
 * The register file itself is QEMU's, wrapped rather than reimplemented. What
 * this file supplies is the machine's view of it: the byte stride, the byte
 * width, and the endianness.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"

/*
 * The ports' access contract, which is the whole reason this wrapper exists
 * rather than using the register file's own region directly.
 *
 * Three things differ from what that region declares. The machine is
 * big-endian where the register file assumes the little-endian PCI I/O space
 * it was written for; the registers are one byte apart rather than at the
 * four-byte stride the chip's other blocks use; and an access wider than a
 * byte is refused.
 *
 * That last one is a choice, and the datasheet records it as a choice rather
 * than a finding. Nothing in the sources or in either guest says what the
 * hardware does with a wider access, and no correct driver issues one, so no
 * observation settles it. Refusing is the option that cannot be wrong quietly:
 * the memory system reports the attempt, and the report can only ever fire on
 * a real defect. Silently narrowing, which is what the register file's own
 * wrapper does, is the one option with nothing to recommend it, because it is
 * an unknown presented as known.
 *
 * The endianness is declared and then left alone. A big-endian processor
 * reaching a little-endian part needs its byte lanes placed, and both guests
 * expect that: one does it in its bus accessors, the other in its driver. The
 * device must not do its own lane arithmetic on top, because that cancels out
 * against one guest and doubles against the other, which is the worst failure
 * available here.
 */
static uint64_t sgi_ioc3_serial_read(void *opaque, hwaddr addr, unsigned size)
{
    return serial_io_ops.read(opaque, addr, 1);
}

static void sgi_ioc3_serial_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    serial_io_ops.write(opaque, addr, val & 0xff, 1);
}

static const MemoryRegionOps sgi_ioc3_serial_ops = {
    .read = sgi_ioc3_serial_read,
    .write = sgi_ioc3_serial_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/*
 * The SuperIO line follows the intersection of the status register and the
 * enable mask, and drops when that intersection empties. So it is recomputed on
 * every write to either register and on every change to a source, rather than
 * raised when a source asserts — a line raised on the event and never
 * recomputed stays up after software has serviced everything it can see, which
 * a guest experiences as taking the same interrupt forever.
 */
static void sgi_ioc3_update_irq(SGIIOC3State *s)
{
    qemu_set_irq(s->superio_irq, !!(s->int_status & s->int_enable));
}

/*
 * A serial port's interrupt is a pass-through: the status bit follows the
 * 16550's own line rather than latching, so it goes up and down with the port
 * and no write to the status register can clear it. What really quiets it is
 * the register read the guest's own serial driver performs.
 */
static void sgi_ioc3_serial_irq(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = port ? SGI_IOC3_INT_SERIAL_B : SGI_IOC3_INT_SERIAL_A;

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);
}

static uint64_t sgi_ioc3_main_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;

    if (addr >= SGI_IOC3_PIN_REG_FIRST && addr <= SGI_IOC3_PIN_REG_LAST &&
        (addr & 3) == 0) {
        unsigned int pin = (addr - SGI_IOC3_PIN_REG_FIRST) / 4;

        return (s->pin_data >> pin) & 1;
    }

    switch (addr) {
    case SGI_IOC3_INT_STATUS:
        return s->int_status;
    case SGI_IOC3_INT_ENABLE_SET:
        /* The set alias reads back the live mask; the clear alias does not. */
        return s->int_enable;
    case SGI_IOC3_MICROLAN_CONTROL:
        return s->microlan_control;
    case SGI_IOC3_PIN_CONTROL_SET:
        /* Same shape as the interrupt-enable pair: the set alias reads back
         * the live mask, the clear alias does not. */
        return s->pin_control;
    case SGI_IOC3_PIN_DATA:
        return s->pin_data;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: read of main register 0x%" HWADDR_PRIx
                      " which this model does not implement\n", addr);
        return 0;
    }
}

static void sgi_ioc3_main_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3State *s = opaque;

    if (addr >= SGI_IOC3_PIN_REG_FIRST && addr <= SGI_IOC3_PIN_REG_LAST &&
        (addr & 3) == 0) {
        unsigned int pin = (addr - SGI_IOC3_PIN_REG_FIRST) / 4;
        uint32_t bit = 1u << pin;

        s->pin_data = (s->pin_data & ~bit) | ((val & 1) ? bit : 0);
        return;
    }

    switch (addr) {
    case SGI_IOC3_INT_STATUS:
        /*
         * Writing a one acknowledges a latched source. The pass-through bits
         * are not latched and ignore the write — clearing a still-asserted
         * level would drop an interrupt this model would never raise again,
         * and a guest's handler writes back everything it serviced, including
         * the serial bits, on every interrupt.
         */
        s->int_status &= ~(val & ~SGI_IOC3_INT_PASSTHROUGH);
        sgi_ioc3_update_irq(s);
        break;
    case SGI_IOC3_INT_ENABLE_SET:
        s->int_enable |= val;
        sgi_ioc3_update_irq(s);
        break;
    case SGI_IOC3_INT_ENABLE_CLR:
        s->int_enable &= ~val;
        sgi_ioc3_update_irq(s);
        break;
    case SGI_IOC3_MICROLAN_CONTROL:
        /*
         * A one-wire time slot: the guest supplies how long to hold the bus
         * low and how long the remainder of the slot lasts, and hardware
         * performs it. This model performs it synchronously — real timing
         * is not observable from a guest driver's own read-after-write
         * polling loop, so completing the slot in the write itself satisfies
         * both OpenBSD's and Linux's drivers without reproducing microsecond
         * delays neither one can tell apart from an instant one.
         *
         * No part answers on this bus yet (task #13), which for every slot
         * type here means the bus stays at its idle high level: an unasked
         * reset sees no presence pulse, and a read slot sees no device
         * pulling the line low. Reporting that level is what "no part
         * responds" means at the register a guest actually reads, and it is
         * what keeps a guest's own presence check correctly negative rather
         * than silently positive.
         */
        s->microlan_control = (val & SGI_IOC3_MICROLAN_TIMING) |
                              SGI_IOC3_MICROLAN_LEVEL | SGI_IOC3_MICROLAN_DONE;
        break;
    case SGI_IOC3_PIN_CONTROL_SET:
        s->pin_control |= val;
        break;
    case SGI_IOC3_PIN_CONTROL_CLR:
        s->pin_control &= ~val;
        break;
    case SGI_IOC3_PIN_DATA:
        s->pin_data = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: write of 0x%" PRIx64 " to main register 0x%"
                      HWADDR_PRIx " which this model does not implement\n",
                      val, addr);
        break;
    }
}

static const MemoryRegionOps sgi_ioc3_main_ops = {
    .read = sgi_ioc3_main_read,
    .write = sgi_ioc3_main_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static uint64_t sgi_ioc3_kbc_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;

    switch (SGI_IOC3_KBC_BASE + addr) {
    case SGI_IOC3_KBC_CONTROL:
        /*
         * Held as written. The bits hardware owns in this register report a
         * write still in progress and a state machine not yet idle, and this
         * model never sets either, so a driver that writes a byte and then
         * polls for the engine to go quiet reads quiet on its first look
         * rather than waiting out a timeout that would never end.
         */
        return s->kbc_control;

    case SGI_IOC3_KBC_KBD_RECEIVE:
    case SGI_IOC3_KBC_AUX_RECEIVE:
        /*
         * No keyboard and no mouse are plugged in. Each register packs up to
         * three received bytes, each with its own valid flag in the top bits,
         * and zero clears every one of those flags -- so this reads as a port
         * that has received nothing, which is true, rather than as three bytes
         * of zero.
         */
        return 0;

    default:
        /*
         * The two transmit registers are write-only. Reading one is a guest
         * asking what it last sent, which the hardware does not keep.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3: read of write-only PS/2 register 0x%03x\n",
                      (unsigned)(SGI_IOC3_KBC_BASE + addr));
        return 0;
    }
}

static void sgi_ioc3_kbc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    SGIIOC3State *s = opaque;

    switch (SGI_IOC3_KBC_BASE + addr) {
    case SGI_IOC3_KBC_CONTROL:
        s->kbc_control = val;
        return;

    case SGI_IOC3_KBC_KBD_TRANSMIT:
    case SGI_IOC3_KBC_AUX_TRANSMIT:
        /*
         * Accepted and dropped. There is nothing on the other end of either
         * port to receive a byte, and the write completing is what a driver
         * needs; a device that refused the write would be reporting a fault
         * where the real one reports an unplugged port.
         */
        return;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3: write to read-only PS/2 register 0x%03x\n",
                      (unsigned)(SGI_IOC3_KBC_BASE + addr));
        return;
    }
}

static const MemoryRegionOps sgi_ioc3_kbc_ops = {
    .read = sgi_ioc3_kbc_read,
    .write = sgi_ioc3_kbc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Which of a port's seven serial DMA registers hardware owns. The two index
 * registers software advances are writable and the two hardware advances are
 * not, so the pair a driver could confuse are on opposite sides of this: the
 * transmit produce index is software's and the transmit consume index is the
 * engine's. The 16550 shadow is a mirror of lines the port drives, so nothing
 * software writes belongs in it either.
 *
 * The shared ring base pair below both ports is writable, which is why an
 * offset before port A's base is not tested against these.
 */
static bool sgi_ioc3_sdma_is_read_only(hwaddr addr)
{
    hwaddr reg = SGI_IOC3_SDMA_BASE + addr;
    hwaddr offset;

    if (reg >= SGI_IOC3_SDMA_PORT_B) {
        offset = reg - SGI_IOC3_SDMA_PORT_B;
    } else if (reg >= SGI_IOC3_SDMA_PORT_A) {
        offset = reg - SGI_IOC3_SDMA_PORT_A;
    } else {
        return false;
    }

    return offset == SGI_IOC3_SDMA_TX_CONSUME ||
           offset == SGI_IOC3_SDMA_RX_PRODUCE ||
           offset == SGI_IOC3_SDMA_SHADOW;
}

static uint64_t sgi_ioc3_sdma_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;

    /*
     * Every register reads back what is stored, and the hardware-owned ones
     * stay at zero because nothing advances them: no ring is walked, so the
     * engine is permanently where it started. A driver reading them sees an
     * idle channel, which is what an idle channel looks like.
     */
    return s->sdma_reg[addr / 4];
}

static void sgi_ioc3_sdma_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3State *s = opaque;

    if (sgi_ioc3_sdma_is_read_only(addr)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3: write to read-only serial DMA register "
                      "0x%03x\n", (unsigned)(SGI_IOC3_SDMA_BASE + addr));
        return;
    }

    s->sdma_reg[addr / 4] = val;
}

static const MemoryRegionOps sgi_ioc3_sdma_ops = {
    .read = sgi_ioc3_sdma_read,
    .write = sgi_ioc3_sdma_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Read the collision and deferral counters and zero them in the same step,
 * which is what the hardware does and what makes reading them a way to clear
 * them. Separate from the read handler because the clearing is the part worth
 * naming: a caller that only wanted the value would be using the wrong verb.
 */
static uint32_t sgi_ioc3_take_counters(SGIIOC3State *s)
{
    unsigned int index = (SGI_IOC3_ENET_COUNTERS - SGI_IOC3_ENET_BASE) / 4;
    uint32_t counters = s->enet_reg[index];

    s->enet_reg[index] = 0;
    return counters;
}

static uint64_t sgi_ioc3_enet_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;
    unsigned int index = addr / 4;

    switch (SGI_IOC3_ENET_BASE + addr) {
    case SGI_IOC3_ENET_COUNTERS:
        /*
         * Collisions and deferrals, cleared by the act of reading them. Both
         * guests read this once while resetting the block for no reason other
         * than to zero it, so a model that returned the count without clearing
         * would leave a driver's own counters climbing on a machine that has
         * never sent a frame.
         */
        return sgi_ioc3_take_counters(s);

    case SGI_IOC3_ENET_BUFFER_INDEX:
        /*
         * Where the block's own engines sit in the buffer memory. Nothing is
         * moving, so every one of the four positions is zero, and this is read
         * only rather than storage because software cannot place them.
         */
        return 0;

    case SGI_IOC3_ENET_MII_CONTROL:
        /*
         * Never busy. The read a guest asked for finished before it could
         * look, so the poll after it stops on the first pass.
         */
        return s->enet_reg[index] & ~SGI_IOC3_ENET_MII_BUSY;

    case SGI_IOC3_ENET_MII_DATA_R:
        /* No physical-layer chip answers; an absent one reads all-ones. */
        return SGI_IOC3_ENET_MII_ABSENT;

    default:
        return s->enet_reg[index];
    }
}

static void sgi_ioc3_enet_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3State *s = opaque;
    unsigned int index = addr / 4;

    switch (SGI_IOC3_ENET_BASE + addr) {
    case SGI_IOC3_ENET_INT_STATUS:
        /*
         * Written to acknowledge: a one clears the source it names rather than
         * setting it. This is the same contract as the SuperIO interrupt
         * register, and the reason to state it twice is that modelling this one
         * as ordinary storage produces the same failure on the ethernet line
         * that it produced on the serial one -- a driver writes back the bits
         * it just read, the write sets them instead of clearing them, and the
         * source it was acknowledging never goes away.
         */
        s->enet_reg[index] &= ~(uint32_t)val;
        return;

    case SGI_IOC3_ENET_COUNTERS:
    case SGI_IOC3_ENET_BUFFER_INDEX:
    case SGI_IOC3_ENET_MII_DATA_R:
        /* Read-only. A guest writing here is not told it worked. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3: write to read-only ethernet register 0x%03x\n",
                      (unsigned)(SGI_IOC3_ENET_BASE + addr));
        return;

    default:
        s->enet_reg[index] = val;
        return;
    }
}

static const MemoryRegionOps sgi_ioc3_enet_ops = {
    .read = sgi_ioc3_enet_read,
    .write = sgi_ioc3_enet_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The ethernet buffer RAM. Every slot answers and keeps what it is given, and
 * the two halves of the window are separate storage, which is the whole of
 * what a driver's size probe measures: it writes a pattern at the first slot
 * and the complement at the halfway slot and reads both back, so a window
 * aliasing at its midpoint reports half the RAM.
 *
 * Seventeen bits are kept because seventeen bits exist -- sixteen of data and
 * the parity bit stored beside them. The parity bit is storage like the rest
 * rather than something computed: this model moves no frames, so there is
 * nothing to compute it over, and a diagnostic write is how software would
 * place a deliberately wrong one anyway.
 */
static uint64_t sgi_ioc3_ssram_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;

    return s->ssram_word[addr / 4];
}

static void sgi_ioc3_ssram_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SGIIOC3State *s = opaque;

    s->ssram_word[addr / 4] = val & SGI_IOC3_SSRAM_STORED;
}

static const MemoryRegionOps sgi_ioc3_ssram_ops = {
    .read = sgi_ioc3_ssram_read,
    .write = sgi_ioc3_ssram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const hwaddr sgi_ioc3_serial_base[SGI_IOC3_SERIAL_PORTS] = {
    SGI_IOC3_SERIAL_A, SGI_IOC3_SERIAL_B,
};

static const char *const sgi_ioc3_serial_name[SGI_IOC3_SERIAL_PORTS] = {
    "sgi-ioc3.serial-a", "sgi-ioc3.serial-b",
};

static void sgi_ioc3_realize(PCIDevice *dev, Error **errp)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    /*
     * The chip's own PCI interrupt pin belongs to the ethernet block, which
     * this model does not implement — nothing ever asserts it — but the
     * config-space byte is cheap to get right and a guest is entitled to
     * read what a real chip reports there. The SuperIO line a guest actually
     * cares about on this machine is not this pin at all; see superio_irq.
     */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        if (!qdev_realize(DEVICE(serial), NULL, errp)) {
            return;
        }

        /*
         * Eight registers of one byte each, so the region is eight bytes wide
         * and the register file's own bound of eight offsets cannot be passed.
         */
        memory_region_init_io(&s->serial_io[i], OBJECT(dev),
                              &sgi_ioc3_serial_ops, serial,
                              sgi_ioc3_serial_name[i], 8);
        memory_region_add_subregion(&s->bar, sgi_ioc3_serial_base[i],
                                    &s->serial_io[i]);

        /*
         * The port's own interrupt reaches this chip rather than the machine:
         * it becomes a pass-through bit here and leaves the chip on the one
         * SuperIO line, which is what makes the two ports share a line and
         * share a handler.
         */
        serial->irq = qdev_get_gpio_in(DEVICE(dev), i);
    }

    /*
     * Only the interrupt block of the main window is implemented, so the region
     * covers those registers and stops. Everything else in the main window is a
     * hole rather than a zero, which is what an access to a register nobody has
     * written should get.
     */
    memory_region_init_io(&s->main, OBJECT(dev), &sgi_ioc3_main_ops, s,
                          "sgi-ioc3.main", SGI_IOC3_MAIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->main);

    /*
     * The ethernet block, its own window so that the unmodelled parallel port,
     * PS/2 and serial DMA registers between it and the main one stay a hole.
     */
    memory_region_init_io(&s->enet, OBJECT(dev), &sgi_ioc3_enet_ops, s,
                          "sgi-ioc3.enet", SGI_IOC3_ENET_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_ENET_BASE, &s->enet);

    /* The PS/2 controller, below the serial DMA block. */
    memory_region_init_io(&s->kbc, OBJECT(dev), &sgi_ioc3_kbc_ops, s,
                          "sgi-ioc3.kbc", SGI_IOC3_KBC_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_KBC_BASE, &s->kbc);

    /* The serial DMA block, immediately below the ethernet one. */
    memory_region_init_io(&s->sdma, OBJECT(dev), &sgi_ioc3_sdma_ops, s,
                          "sgi-ioc3.sdma", SGI_IOC3_SDMA_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SDMA_BASE, &s->sdma);

    /*
     * The ethernet buffer RAM, the whole window rather than part of it: an
     * Origin 200 populates all of it, so there is no unpopulated remainder
     * here whose behaviour would have to be decided.
     */
    memory_region_init_io(&s->ssram, OBJECT(dev), &sgi_ioc3_ssram_ops, s,
                          "sgi-ioc3.ssram", SGI_IOC3_SSRAM_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SSRAM_BASE, &s->ssram);

    /*
     * The byte bus's first device select, and the clock behind it. The chip
     * decodes the select and the part answers, so what is mapped here is the
     * clock's own region rather than anything this file implements.
     *
     * It is realized as a system-bus device rather than a plain one because a
     * plain device realized with no bus is never reset -- its registers decode
     * and answer perfectly while every reset value stays at whatever the
     * allocation left there, which is a defect nothing warns about.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rtc),
                                                       0));

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /*
     * The SuperIO line is not this chip's own PCI interrupt pin: the
     * contract's ratified pin for it is the base I/O board's own wiring, not
     * something the Bridge's per-slot PCI routing produces from any pin this
     * chip could assert. So it stays a plain qdev GPIO output — a real wire a
     * guest cannot see in configuration space — for the machine to connect
     * directly to the Bridge pin the board wires it to, the same shape the
     * previous direct-mapped chip already gave it and unaffected by becoming
     * a PCI device otherwise.
     */
    qdev_init_gpio_out(DEVICE(dev), &s->superio_irq, 1);
}

static void sgi_ioc3_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3State *s = SGI_IOC3(obj);

    /*
     * Nothing enabled and nothing pending. The enable mask resetting to zero is
     * why a guest sees no interrupt from this chip until it asks for one, and
     * the pass-through bits come back as soon as a port asserts again.
     */
    s->int_status = 0;
    s->int_enable = 0;
    sgi_ioc3_update_irq(s);

    /*
     * The ethernet block resets to zero throughout, which the datasheet gives
     * for every register in it. That matters most for the media access
     * control register, whose transmit and receive enables sit in it: a block
     * that came up enabled would be one no driver had asked to run.
     */
    memset(s->enet_reg, 0, sizeof(s->enet_reg));

    /*
     * The serial DMA block resets to zero throughout, indices included, which
     * is what puts both engines back at the start of their rings.
     */
    memset(s->sdma_reg, 0, sizeof(s->sdma_reg));

    /* Both PS/2 ports come up with their lines released and nothing sent. */
    s->kbc_control = 0;

    /*
     * The ethernet buffer RAM is deliberately not among these. It is memory
     * rather than a register file, and a reset line does not empty memory --
     * what a chip holds there after a reset is what it held before one. Its
     * contents at power-on are whatever the parts came up with, and zero is
     * as good a stand-in for that as anything, which is what allocating the
     * array gives us for free.
     */

    /*
     * SGI's own register diagnostic lists every register in the chip
     * resetting to zero apart from the PCI identity and revision words,
     * and these three are no exception.
     */
    s->microlan_control = 0;
    s->pin_control = 0;
    s->pin_data = 0;
}

static void sgi_ioc3_init(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    qdev_init_gpio_in(DEVICE(obj), sgi_ioc3_serial_irq,
                      SGI_IOC3_SERIAL_PORTS);

    /*
     * The ports are children of the chip rather than devices in their own
     * right, because that is what they are: one chip, one base address
     * register, two register files inside it. Making them separate devices
     * would give each its own window and lose the containment that decides
     * where they answer.
     */
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        object_initialize_child(obj, "serial[*]", &s->serial[i], TYPE_SERIAL);
    }

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_M48T35);
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 8,
    .minimum_version_id = 8,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC3State),
        VMSTATE_UINT32_V(int_status, SGIIOC3State, 2),
        VMSTATE_UINT32_V(int_enable, SGIIOC3State, 2),
        VMSTATE_STRUCT_ARRAY(serial, SGIIOC3State, SGI_IOC3_SERIAL_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(microlan_control, SGIIOC3State),
        VMSTATE_UINT32(pin_control, SGIIOC3State),
        VMSTATE_UINT32(pin_data, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(enet_reg, SGIIOC3State, SGI_IOC3_ENET_REGS),
        VMSTATE_UINT32_ARRAY(sdma_reg, SGIIOC3State, SGI_IOC3_SDMA_REGS),
        VMSTATE_UINT32(kbc_control, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(ssram_word, SGIIOC3State, SGI_IOC3_SSRAM_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_properties[] = {
    DEFINE_PROP_CHR("chardev-a", SGIIOC3State, serial[0].chr),
    DEFINE_PROP_CHR("chardev-b", SGIIOC3State, serial[1].chr),
};

static void sgi_ioc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 base I/O controller";

    pc->realize = sgi_ioc3_realize;
    pc->vendor_id = SGI_IOC3_VENDOR_ID;
    pc->device_id = SGI_IOC3_DEVICE_ID;
    pc->revision = SGI_IOC3_REVISION;
    pc->class_id = SGI_IOC3_CLASS_ID;
    pc->subsystem_vendor_id = SGI_IOC3_VENDOR_ID;
    pc->subsystem_id = SGI_IOC3_SUBSYSTEM_ID;
    rc->phases.hold = sgi_ioc3_reset_hold;
    dc->vmsd = &vmstate_sgi_ioc3;
    device_class_set_props(dc, sgi_ioc3_properties);
}

static const TypeInfo sgi_ioc3_types[] = {
    {
        .name = TYPE_SGI_IOC3,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIIOC3State),
        .instance_init = sgi_ioc3_init,
        .class_init = sgi_ioc3_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(sgi_ioc3_types)
