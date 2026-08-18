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
#include "system/dma.h"
#include "net/net.h"
#include "trace.h"

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
static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port);

static void sgi_ioc3_serial_irq(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = port ? SGI_IOC3_INT_SERIAL_B : SGI_IOC3_INT_SERIAL_A;

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);

    /*
     * The port asserting is also the one moment this model can see that a
     * character has arrived from outside, so it is where the engine gets to
     * move it. That covers a guest whose own serial driver has the 16550's
     * receive interrupt enabled; one that disables the 16550 entirely and
     * receives through the rings alone would leave characters in the port's
     * FIFO until something else reached the engine, and no guest on this
     * machine's path does that -- the firmware's diagnostic runs both ports
     * in loopback, where every character is delivered inside the write that
     * sent it. An IRIX guest driving the rings is what would settle whether
     * the gap is worth a poll of its own.
     */
    if (level) {
        sgi_ioc3_sdma_run(s, port);
    }
}

/*
 * The ethernet block's interrupt reaches this chip's ordinary PCI interrupt
 * pin rather than a wire of its own, unlike the SuperIO line above. The
 * block itself is a child device with no PCI identity to raise that pin
 * with directly, so this is what turns a level on its GPIO output into a
 * call to pci_set_irq on the chip the guest actually sees.
 */
static void sgi_ioc3_eth_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * The one-wire ethernet-address part behind the MicroLAN engine above. See
 * docs/datasheets/ioc3.md's "The ethernet-address part's read protocol" for
 * the command bytes, the CRC conventions and which guest relies on what --
 * this is the model built against that section, not a re-derivation of it.
 *
 * The state machine's shape mirrors the Bridge's board-identity one-wire
 * (hw/pci-host/sgi-bridge.c): a reset, a ROM-selection phase, a function
 * command, a two-byte target address, then an answer shifted back out one
 * bit at a time. Two things differ from the Bridge's part. This bus's guest
 * — OpenBSD's owmac — addresses the part with a Match ROM rather than a
 * Skip ROM, so selection has to compare a real identifier rather than
 * assume the only part on the bus. And the part answers two different
 * function commands for the same read, one framed to a shorter, unchecked
 * response and one to a longer, checked one, rather than the Bridge's
 * single extended-read command.
 */

/*
 * The Dallas/Maxim one-wire eight-bit CRC: polynomial x^8+x^5+x^4+1,
 * reflected form 0x8c, seeded at zero and sent as computed -- unlike the
 * sixteen-bit CRC below, this one is not sent inverted. Confirmed bit-for-
 * bit against both guests' own lookup tables (OpenBSD's crc8_table in
 * sys/dev/onewire/onewire_subr.c, Linux's w1_crc8_table in
 * drivers/w1/w1_io.c), which are identical table for table; this is a
 * public one-wire-standard algorithm computed bit-serially rather than
 * either table reproduced.
 */
static uint8_t sgi_ioc3_onewire_crc8(uint8_t crc, const uint8_t *data,
                                     unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/*
 * The standard sixteen-bit one-wire CRC (polynomial 0x8005, reflected
 * 0xa001, seeded at zero) -- the same algorithm the Bridge's board-identity
 * part already implements, reused here for the ethernet-address page's own
 * embedded check. Confirmed against Linux's crc16() (include/linux/crc16.h
 * states the same polynomial and seed) and against the fixed residue
 * 0xb001 its ioc3eth_get_mac_addr() requires.
 */
static uint16_t sgi_ioc3_onewire_crc16(uint16_t crc, const uint8_t *data,
                                       unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/*
 * The part's fixed sixty-four-bit ROM identifier: family 0x91, the serial
 * number's required top twelve bits 0x5e7 followed by this model's own
 * choice for the rest, and a CRC-8 computed over those seven bytes rather
 * than hand-derived -- see the datasheet section named above for which
 * parts of this are required and which are a modelling choice.
 */
static uint64_t sgi_ioc3_onewire_rom_id(void)
{
    uint8_t rom[7];
    uint64_t serial = SGI_IOC3_ONEWIRE_SERIAL;
    unsigned int i;
    uint8_t crc;

    rom[0] = SGI_IOC3_ONEWIRE_FAMILY;
    for (i = 0; i < 6; i++) {
        rom[1 + i] = (serial >> (8 * i)) & 0xff;
    }
    crc = sgi_ioc3_onewire_crc8(0, rom, sizeof(rom));

    return (uint64_t)rom[0] | ((uint64_t)rom[1] << 8) |
           ((uint64_t)rom[2] << 16) | ((uint64_t)rom[3] << 24) |
           ((uint64_t)rom[4] << 32) | ((uint64_t)rom[5] << 40) |
           ((uint64_t)rom[6] << 48) | ((uint64_t)crc << 56);
}

/*
 * Assemble what the part sends back for the function command now held in
 * onewire_command, once the two-byte target address has arrived. The
 * status read and the two memory reads share the same framing -- a CRC-8
 * of the three-byte echo, then the requested bytes -- and differ only in
 * which bytes follow and whether a trailing CRC-8 is appended.
 */
static void sgi_ioc3_onewire_answer(SGIIOC3State *s)
{
    uint8_t echo[3];
    uint8_t page[SGI_IOC3_ONEWIRE_PAGE_SIZE];
    uint16_t crc16;
    unsigned int i;

    echo[0] = s->onewire_command;
    echo[1] = s->onewire_args[0];
    echo[2] = s->onewire_args[1];
    s->onewire_out[0] = sgi_ioc3_onewire_crc8(0, echo, sizeof(echo));

    if (s->onewire_command == SGI_IOC3_ONEWIRE_FN_READ_STATUS) {
        /*
         * No page of this model's memory is ever redirected, so every
         * status byte reads 0xff -- which is what tells owmem_redirect()'s
         * loop to stop on its first look and hand owmac_read_page() the
         * page it asked for, rather than an address this model actually
         * remaps. OpenBSD only ever asks for offset 0x0001; every other
         * address answering the same way is this model's choice, not a
         * second observed fact.
         */
        memset(&s->onewire_out[1], 0xff, SGI_IOC3_ONEWIRE_STATUS_SIZE);
        s->onewire_out_len = 1 + SGI_IOC3_ONEWIRE_STATUS_SIZE;
        s->onewire_outbit = 0;
        s->onewire_phase = SGI_IOC3_ONEWIRE_ANSWER;
        return;
    }

    /*
     * Both memory-read commands answer the same page regardless of the
     * address requested: OpenBSD's owmac and Linux's w1_ds2502_read_page
     * both always ask for page zero, the only page this part needs to
     * hold.
     */
    page[0] = 0x0a;                       /* the fixed marker both guests require */
    memset(&page[1], 0, 4);               /* unconstrained by either guest */
    for (i = 0; i < 6; i++) {
        page[10 - i] = s->eth.conf.macaddr.a[i];
    }
    crc16 = ~sgi_ioc3_onewire_crc16(0, page, 11);
    page[11] = crc16 & 0xff;
    page[12] = crc16 >> 8;
    memset(&page[13], 0, SGI_IOC3_ONEWIRE_PAGE_SIZE - 13);

    memcpy(&s->onewire_out[1], page, sizeof(page));
    s->onewire_out[1 + sizeof(page)] =
        sgi_ioc3_onewire_crc8(0, page, sizeof(page));

    s->onewire_out_len = SGI_IOC3_ONEWIRE_ANSWER_SIZE;
    s->onewire_outbit = 0;
    s->onewire_phase = SGI_IOC3_ONEWIRE_ANSWER;
}

/*
 * Run one bit slot on the one-wire bus and leave its answer in the level
 * bit sgi_ioc3_main_write packs back into the register. The pulse field
 * (bits 10 and up of the write) tells a reset from an ordinary slot; every
 * other phase transition below follows the sequence of slots rather than
 * their timing, the same design the Bridge's identical machinery uses and
 * for the same reason -- nothing a guest driver does can tell this model's
 * instant completion apart from the real microseconds.
 */
static void sgi_ioc3_onewire_slot(SGIIOC3State *s, uint32_t val)
{
    uint32_t pulse = val >> SGI_IOC3_ONEWIRE_PULSE_SHIFT;
    bool level = true;

    if (pulse >= SGI_IOC3_ONEWIRE_PULSE_RESET) {
        /* A reset. The part holds the wire down, which a guest reads as
         * presence, and forgets whatever conversation was in progress.
         */
        s->onewire_phase = SGI_IOC3_ONEWIRE_COMMAND;
        s->onewire_bit = 0;
        s->onewire_command = 0;
        s->onewire_outbit = 0;
        level = false;
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_COMMAND) {
        /* A ROM command, least-significant bit first. */
        if (pulse < SGI_IOC3_ONEWIRE_PULSE_ZERO) {
            s->onewire_command |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            switch (s->onewire_command) {
            case SGI_IOC3_ONEWIRE_ROM_SEARCH:
                s->onewire_phase = SGI_IOC3_ONEWIRE_SEARCH;
                break;
            case SGI_IOC3_ONEWIRE_ROM_SKIP:
                /* One part on this bus, and the guest says so rather than
                 * naming it -- Linux's w1 core takes this path whenever
                 * only one device is registered.
                 */
                s->onewire_command = 0;
                s->onewire_phase = SGI_IOC3_ONEWIRE_FUNCTION;
                break;
            case SGI_IOC3_ONEWIRE_ROM_MATCH:
                /* OpenBSD's owmac always addresses the part this way,
                 * never by Skip ROM -- see the datasheet section named
                 * above.
                 */
                s->onewire_match = 0;
                s->onewire_phase = SGI_IOC3_ONEWIRE_MATCH;
                break;
            default:
                s->onewire_phase = SGI_IOC3_ONEWIRE_IDLE;
                break;
            }
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_SEARCH) {
        /* A search asks for each bit of the identity three slots at a
         * time: the bit, its complement, then the guest's choice. With a
         * single part on the bus every choice matches what was offered.
         * The selected part remains ready for a function command; SGI's
         * PROM relies on that and sends no second ROM command.
         */
        unsigned int step = s->onewire_bit % 3;
        unsigned int index = s->onewire_bit / 3;
        bool bit = (sgi_ioc3_onewire_rom_id() >> index) & 1;

        if (step == 0) {
            level = bit;
        } else if (step == 1) {
            level = !bit;
        }

        if (++s->onewire_bit == 64 * 3) {
            s->onewire_bit = 0;
            s->onewire_command = 0;
            s->onewire_phase = SGI_IOC3_ONEWIRE_FUNCTION;
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_MATCH) {
        /* The sixty-four bits of a Match ROM, least-significant bit
         * first. A part that does not match stops answering until the
         * next reset, real one-wire semantics for the general case of
         * more than one part on a bus.
         */
        if (pulse < SGI_IOC3_ONEWIRE_PULSE_ZERO) {
            s->onewire_match |= 1ULL << s->onewire_bit;
        }
        if (++s->onewire_bit == 64) {
            s->onewire_bit = 0;
            s->onewire_command = 0;
            s->onewire_phase = (s->onewire_match == sgi_ioc3_onewire_rom_id()) ?
                               SGI_IOC3_ONEWIRE_FUNCTION : SGI_IOC3_ONEWIRE_IDLE;
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_FUNCTION) {
        /* The function asked of the selected part, a bit at a time. */
        if (pulse < SGI_IOC3_ONEWIRE_PULSE_ZERO) {
            s->onewire_command |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            s->onewire_argn = 0;
            s->onewire_args[0] = 0;
            s->onewire_args[1] = 0;
            switch (s->onewire_command) {
            case SGI_IOC3_ONEWIRE_FN_READ_MEMORY:
            case SGI_IOC3_ONEWIRE_FN_READ_MEMORY_CRC:
            case SGI_IOC3_ONEWIRE_FN_READ_STATUS:
                s->onewire_phase = SGI_IOC3_ONEWIRE_OFFSET;
                break;
            default:
                s->onewire_phase = SGI_IOC3_ONEWIRE_IDLE;
                break;
            }
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_OFFSET) {
        /* Two bytes of target address, low first, still one slot per bit. */
        if (pulse < SGI_IOC3_ONEWIRE_PULSE_ZERO) {
            s->onewire_args[s->onewire_argn] |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            if (++s->onewire_argn == 2) {
                sgi_ioc3_onewire_answer(s);
            }
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_ANSWER) {
        /* Shifting the answer back, least-significant bit of each byte
         * first. The guest reads with the same short slot it uses to
         * write a one, so there is nothing to tell apart here: whatever
         * it does, the part is sending.
         */
        unsigned int byte = s->onewire_outbit / 8;

        level = (s->onewire_out[byte] >> (s->onewire_outbit % 8)) & 1;
        if (++s->onewire_outbit == (unsigned int)s->onewire_out_len * 8) {
            s->onewire_outbit = 0;
            s->onewire_phase = SGI_IOC3_ONEWIRE_IDLE;
        }
    }

    s->microlan_control = (val & SGI_IOC3_MICROLAN_TIMING) |
                          SGI_IOC3_MICROLAN_DONE |
                          (level ? SGI_IOC3_MICROLAN_LEVEL : 0);
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
    case SGI_IOC3_CHIP_CONTROL:
        return s->chip_control;
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
    case SGI_IOC3_CHIP_CONTROL:
        /*
         * Where the serial DMA engines are told the two ports live inside the
         * SuperIO window, plus the byte bus strobe length. It is storage here
         * and not a multiplexer: the firmware and both guests program the
         * fields to the two offsets this chip already implements, so a model
         * resolving a port through them and one ignoring them answer alike on
         * every path any of them takes. Storing it is what the register
         * contract asks for; acting on it would add a failure mode nothing
         * exercises.
         */
        s->chip_control = val & SGI_IOC3_CHIP_CTRL_MASK;
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
         * What answers is the ethernet-address part's transaction state
         * machine below, driven one slot at a time.
         */
        sgi_ioc3_onewire_slot(s, (uint32_t)val);
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
 * The two serial ports' DMA engines.
 *
 * Each port has an engine that moves characters between a pair of memory
 * rings and the port's own 16550, and one base register covers all four
 * rings: port A transmit, port A receive, port B transmit, port B receive,
 * in that fixed order, each of the size the base register's low bit selects.
 * So a ring's address comes from its port and its direction and there is no
 * register that reads it back -- which is why every ring address this file
 * computes is asserted in the qtest by reading the memory the engine wrote.
 *
 * Nothing here is on the console's path. The firmware and both guests drive
 * these ports through the byte-wide 16550 windows with ring DMA switched off,
 * and an engine only ever walks a ring once a guest has set the enable bit,
 * so a console that works stays working whatever this code does. What does
 * drive the engines is the firmware's own base I/O diagnostic, which puts
 * both ports in loopback and hands each one a full ring.
 */

/*
 * The handful of 16550 register bits the engine needs. The register file's
 * own definitions are private to hw/char/serial.c, so these are restated here
 * rather than reached for -- they are the 16550's published layout, not that
 * file's invention, and this device already reaches the register file only
 * through its published ops.
 */
#define UART_RBR_THR    0
#define UART_MCR        4
#define UART_LSR        5
#define UART_MSR        6

#define UART_LSR_DR     0x01
#define UART_LSR_OE     0x02
#define UART_LSR_PE     0x04
#define UART_LSR_FE     0x08
#define UART_LSR_BI     0x10

#define UART_MSR_CTS    0x10
#define UART_MSR_DCD    0x80

/*
 * Which of a port's seven serial DMA registers hardware owns, as an offset
 * from that port's base. The two index registers software advances are
 * writable and the two hardware advances are not, so the pair a driver could
 * confuse are on opposite sides of this: the transmit produce index is
 * software's and the transmit consume index is the engine's. The 16550
 * shadow is a mirror of lines the port drives, so nothing software writes
 * belongs in it either -- but unlike the two indices, a write to it is not a
 * sign of driver confusion about who owns what: see the write handler below.
 *
 * The shared ring base pair below both ports is writable, which is why an
 * offset before port A's base reports itself as belonging to neither port.
 */
static bool sgi_ioc3_sdma_port_offset(hwaddr reg, unsigned int *port,
                                      hwaddr *offset)
{
    if (reg >= SGI_IOC3_SDMA_PORT_B) {
        *port = 1;
        *offset = reg - SGI_IOC3_SDMA_PORT_B;
        return true;
    } else if (reg >= SGI_IOC3_SDMA_PORT_A) {
        *port = 0;
        *offset = reg - SGI_IOC3_SDMA_PORT_A;
        return true;
    }

    return false;
}

/* Where one of a port's seven registers lives in the block's storage. */
static uint32_t *sgi_ioc3_sdma_port_reg(SGIIOC3State *s, unsigned int port,
                                        hwaddr offset)
{
    hwaddr reg = SGI_IOC3_SDMA_PORT_A + port * SGI_IOC3_SDMA_PORT_STRIDE +
                 offset;

    return &s->sdma_reg[(reg - SGI_IOC3_SDMA_BASE) / 4];
}

static uint32_t sgi_ioc3_sdma_control(SGIIOC3State *s, unsigned int port)
{
    return *sgi_ioc3_sdma_port_reg(s, port, SGI_IOC3_SDMA_CONTROL);
}

/*
 * The ring size the base register currently selects, and with it the width of
 * every index register's pointer field. The two are the same fact: an index
 * addresses the ring in eight-byte units and cannot leave it, so the pointer
 * is exactly as wide as the ring is long.
 */
static uint32_t sgi_ioc3_sdma_ring_size(SGIIOC3State *s)
{
    uint32_t lo = s->sdma_reg[(SGI_IOC3_SDMA_RING_LO - SGI_IOC3_SDMA_BASE) / 4];

    return (lo & SGI_IOC3_SDMA_RING_4K) ? SGI_IOC3_SDMA_RING_4K_SIZE
                                        : SGI_IOC3_SDMA_RING_1K_SIZE;
}

static uint32_t sgi_ioc3_sdma_pointer_mask(SGIIOC3State *s)
{
    return (sgi_ioc3_sdma_ring_size(s) - 1) & ~(SGI_IOC3_SDMA_ENTRY_SIZE - 1);
}

/*
 * One ring's address. The four rings are one allocation four ring-sizes long
 * and are told apart by port and direction alone, and the base register's
 * size-select bit is inside the register but outside the address -- masking
 * it off here is the whole of that, and getting it wrong puts every ring one
 * byte away from where the guest built it.
 */
static uint64_t sgi_ioc3_sdma_ring(SGIIOC3State *s, unsigned int port,
                                   unsigned int direction)
{
    uint32_t hi = s->sdma_reg[(SGI_IOC3_SDMA_RING_HI - SGI_IOC3_SDMA_BASE) / 4];
    uint32_t lo = s->sdma_reg[(SGI_IOC3_SDMA_RING_LO - SGI_IOC3_SDMA_BASE) / 4];
    uint64_t base = ((uint64_t)hi << 32) | (lo & SGI_IOC3_SDMA_RING_ADDRESS);
    unsigned int index = port * SGI_IOC3_SDMA_RING_DIRS + direction;

    return base + (uint64_t)index * sgi_ioc3_sdma_ring_size(s);
}

/*
 * Raise one of a port's nine interrupt sources. It is or-ed into whatever the
 * register already holds rather than written over a value read earlier,
 * because the register belongs to no one engine: a transmit walk in loopback
 * delivers characters to the same chip's receiver before it returns, so any
 * source raised inside the walk would be destroyed by a write-back of a
 * pre-walk snapshot -- invisibly, since every register involved would still
 * hold a legal value afterwards.
 */
static void sgi_ioc3_raise_serial_source(SGIIOC3State *s, unsigned int port,
                                         unsigned int source)
{
    s->int_status |= SGI_IOC3_SERIAL_SOURCE(port, source);
    sgi_ioc3_update_irq(s);
}

/* Whether the port's engine may move anything at all right now. */
static bool sgi_ioc3_sdma_running(SGIIOC3State *s, unsigned int port)
{
    uint32_t control = sgi_ioc3_sdma_control(s, port);

    return (control & SGI_IOC3_SDMA_ENABLE) &&
           !(control & SGI_IOC3_SDMA_PAUSE);
}

/*
 * Take whatever the port's receiver is holding and pack it into the receive
 * ring, four characters to an entry, with each character's own status byte
 * beside it.
 */
/*
 * Put the packer's contents into the receive ring as one entry and start it
 * again. Slots the engine did not fill carry neither validity flag, so an
 * entry written short says so rather than leaving stale characters behind it.
 */
static void sgi_ioc3_sdma_post(SGIIOC3State *s, unsigned int port,
                               uint32_t *produce, uint32_t *consume,
                               uint32_t mask)
{
    uint64_t ring = sgi_ioc3_sdma_ring(s, port, SGI_IOC3_SDMA_RING_RX);
    uint32_t next = (*produce + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;

    /*
     * The ring is full when the next entry the engine would fill is the one
     * software has not consumed yet. Characters are dropped rather than
     * written over an entry a driver has not read, and the source says so.
     */
    if (next == (*consume & mask)) {
        s->sdma_packed[port] = 0;
        memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_RING_FULL);
        return;
    }

    dma_memory_write(s->dma_as, ring + (*produce & mask),
                     s->sdma_packer[port], SGI_IOC3_SDMA_ENTRY_SIZE,
                     MEMTXATTRS_UNSPECIFIED);
    *produce = next;
    s->sdma_packed[port] = 0;
    memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
    s->sdma_delivered[port] = true;
}

/*
 * The end of a burst. A real engine keeps a short group in its packer until
 * either a fourth character arrives or the receive timer runs out of idle
 * time, and this model has no clock of its own to run that timer against --
 * so what stands in for the timer expiring is the moment the engine has
 * nothing left to do, which is the same event the timer exists to detect.
 * That substitution is this model's, not the hardware's: a guest that armed
 * the timer expecting a short group to sit until it expired gets it
 * immediately instead. Nothing on this machine's path can tell the two apart,
 * and the project's ioc3-serial-dma notebook names what would.
 *
 * Then the two sources a delivery prompts a driver with. The receive timer is
 * armed through bit 31 of the consume index; the threshold is a different
 * thing, a supply warning that fires once the characters waiting for software
 * reach the mark the control register sets. The mark is in characters rather
 * than entries -- the firmware's diagnostic programs 511 against 1 KB rings,
 * which hold 128 entries of four characters, so a mark counted in entries
 * could never be reached and a mark counted in characters is one short of
 * full, which is what a mark set out of the way looks like.
 */
static void sgi_ioc3_sdma_flush(SGIIOC3State *s, unsigned int port)
{
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);
    uint32_t control = sgi_ioc3_sdma_control(s, port);
    uint32_t waiting;

    if (s->sdma_packed[port]) {
        sgi_ioc3_sdma_post(s, port, produce, consume, mask);
    }

    if (!s->sdma_delivered[port]) {
        return;
    }
    s->sdma_delivered[port] = false;

    if (*consume & SGI_IOC3_SDMA_ARM_RX_TIMER) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_TIMER);
    }

    waiting = ((*produce - *consume) & mask) / SGI_IOC3_SDMA_ENTRY_SIZE *
              SGI_IOC3_SDMA_ENTRY_CHARS;
    if ((control & SGI_IOC3_SDMA_HIGH_WATER) &&
        waiting >= (control & SGI_IOC3_SDMA_HIGH_WATER)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_HIGH_WATER);
    }
}

static void sgi_ioc3_sdma_receive(SGIIOC3State *s, unsigned int port)
{
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);
    SerialState *serial = &s->serial[port];

    while (sgi_ioc3_sdma_running(s, port)) {
        uint8_t lsr = serial_io_ops.read(serial, UART_LSR, 1);
        uint8_t msr;
        uint8_t status;

        if (!(lsr & UART_LSR_DR)) {
            break;
        }

        /*
         * Reading the line status register is what clears its error latches,
         * so the value read above is the one that describes the character
         * read here, and reading them in the other order would report the
         * previous character's errors against this one.
         */
        s->sdma_packer[port][s->sdma_packed[port]] =
            serial_io_ops.read(serial, UART_RBR_THR, 1);
        msr = serial_io_ops.read(serial, UART_MSR, 1);

        /*
         * The status byte's two validity flags are what a driver actually
         * tests, and they are separate: one says the data byte and the error
         * fields mean something, the other says the modem and overrun fields
         * do. A slot the engine did not fill carries neither, which is how a
         * driver reading a whole entry knows where the characters stop.
         */
        status = SGI_IOC3_SDMA_RX_DATA_OK | SGI_IOC3_SDMA_RX_MODEM_OK;
        if (lsr & UART_LSR_OE) {
            status |= SGI_IOC3_SDMA_RX_OVERRUN;
        }
        if (lsr & UART_LSR_PE) {
            status |= SGI_IOC3_SDMA_RX_PARITY;
        }
        if (lsr & UART_LSR_FE) {
            status |= SGI_IOC3_SDMA_RX_FRAMING;
        }
        if (lsr & UART_LSR_BI) {
            status |= SGI_IOC3_SDMA_RX_BREAK;
        }
        if (msr & UART_MSR_CTS) {
            status |= SGI_IOC3_SDMA_RX_CTS;
        }
        if (msr & UART_MSR_DCD) {
            status |= SGI_IOC3_SDMA_RX_CD;
        }
        s->sdma_packer[port][SGI_IOC3_SDMA_ENTRY_CHARS +
                             s->sdma_packed[port]] = status;

        /*
         * An entry goes to memory once four characters have been packed into
         * it, and not before. That is what the packer is: the produce index
         * reports how many bytes it is holding precisely because a driver
         * would otherwise see an entry per character, and the count is the
         * difference between an engine that packs and one that flushes as it
         * goes.
         */
        if (++s->sdma_packed[port] == SGI_IOC3_SDMA_ENTRY_CHARS) {
            sgi_ioc3_sdma_post(s, port, produce, consume, mask);
        }
    }
}

/*
 * Walk the transmit ring from where the engine got to as far as software has
 * handed over, and send what each entry carries.
 *
 * An entry's four control bytes are typed one per data byte: a character for
 * the transmitter, a byte for the port's modem control register, or a delay.
 * The type both bits clear means the slot holds nothing, which is how a
 * driver with fewer than four characters ends an entry -- an engine sending
 * all four regardless puts three bytes of whatever was in memory on the wire
 * after every short write.
 */
static void sgi_ioc3_sdma_transmit(SGIIOC3State *s, unsigned int port)
{
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_TX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_TX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);
    SerialState *serial = &s->serial[port];
    uint64_t ring = sgi_ioc3_sdma_ring(s, port, SGI_IOC3_SDMA_RING_TX);
    bool walked = false;

    while (sgi_ioc3_sdma_running(s, port) &&
           (*consume & mask) != (*produce & mask)) {
        uint8_t entry[SGI_IOC3_SDMA_ENTRY_SIZE];
        unsigned int i;

        dma_memory_read(s->dma_as, ring + (*consume & mask), entry,
                        sizeof(entry), MEMTXATTRS_UNSPECIFIED);

        for (i = 0; i < SGI_IOC3_SDMA_ENTRY_CHARS; i++) {
            uint8_t control = entry[SGI_IOC3_SDMA_ENTRY_CHARS + i];

            switch (control & SGI_IOC3_SDMA_TX_TYPE) {
            case SGI_IOC3_SDMA_TX_NOTHING:
                /* The entry ends here, and so does this slot's interrupt. */
                continue;
            case SGI_IOC3_SDMA_TX_CHARACTER:
                serial_io_ops.write(serial, UART_RBR_THR, entry[i], 1);
                /*
                 * With the port in loopback the character it just sent is
                 * already sitting in its own receiver, and the receive FIFO
                 * is sixteen bytes deep against a ring the diagnostic fills
                 * five hundred characters at a time. Draining per character
                 * is what keeps the two in step; letting the FIFO run would
                 * lose characters to overrun and report it as the engine's.
                 */
                sgi_ioc3_sdma_receive(s, port);
                break;
            case SGI_IOC3_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC3_SDMA_TX_DELAY:
                /*
                 * A pause in milliseconds before the next byte. Nothing in
                 * this model has a rate for it to pace against -- the
                 * transmitter here is instantaneous -- so the delay is
                 * honoured by being unnecessary rather than by waiting.
                 */
                break;
            }

            if (control & SGI_IOC3_SDMA_TX_INTERRUPT) {
                sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_DONE);
            }
        }

        *consume = (*consume + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;
        walked = true;
    }

    /*
     * The queue draining is its own source, separate from the per-entry
     * completion above, and a driver may enable either without the other. It
     * is raised on reaching the end of a walk rather than on every idle
     * engine, so an enabled engine with an empty ring does not raise it
     * forever.
     */
    if (walked && (*consume & mask) == (*produce & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_RING_EMPTY);
    }
}

/*
 * Both directions, in the order the hardware has them: characters go out, and
 * anything the port has taken in comes back. The receive pass runs even when
 * the transmit ring was empty, because a port with nothing to send still has
 * a receiver.
 */
static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port)
{
    /*
     * One walk at a time per port. Every way into this function from inside
     * itself goes through the port's own interrupt: sending a character in
     * loopback delivers it, and reading one out empties the receive FIFO,
     * and the register file raises or lowers its line on both. A second walk
     * starting there would post the packer's half-filled entry in the middle
     * of the burst still filling it.
     */
    if (s->sdma_busy[port]) {
        return;
    }
    s->sdma_busy[port] = true;

    sgi_ioc3_sdma_transmit(s, port);
    sgi_ioc3_sdma_receive(s, port);
    sgi_ioc3_sdma_flush(s, port);

    s->sdma_busy[port] = false;
}

/*
 * Put one port's channels back at the start of their rings. This is the
 * control register's top bit, which is an action rather than a setting: the
 * firmware's diagnostic writes it, then writes the register again without it,
 * so a model storing it would work by luck -- but a driver that wrote it once
 * and moved on would leave the engine held in reset for good.
 */
static void sgi_ioc3_sdma_reset_port(SGIIOC3State *s, unsigned int port)
{
    /*
     * The four indices and nothing else. What the bit resets is the channels,
     * so the settings a driver made before it -- the watermark, the receive
     * timer's reload -- are still the settings it meant afterwards.
     */
    unsigned int reg;

    for (reg = SGI_IOC3_SDMA_TX_PRODUCE; reg <= SGI_IOC3_SDMA_RX_CONSUME;
         reg += 4) {
        *sgi_ioc3_sdma_port_reg(s, port, reg) = 0;
    }

    /* And the characters the packer was holding for an entry it never wrote. */
    s->sdma_packed[port] = 0;
    memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
    s->sdma_delivered[port] = false;
}

/*
 * One of a port's seven registers, read. Six of them read back the state they
 * hold; the control register additionally answers two things hardware owns --
 * the transmit timer running, which nothing in this model starts, and the
 * pause having taken effect, which it always has, since this engine has no
 * transfer in flight to finish first.
 */
static uint32_t sgi_ioc3_sdma_read_port(SGIIOC3State *s, unsigned int port,
                                        hwaddr offset)
{
    uint32_t val = *sgi_ioc3_sdma_port_reg(s, port, offset);

    if (offset == SGI_IOC3_SDMA_CONTROL && (val & SGI_IOC3_SDMA_PAUSE)) {
        val |= SGI_IOC3_SDMA_PAUSED;
    }

    /*
     * The receive produce index reports alongside its pointer how many
     * characters the packer is holding for an entry it has not written yet,
     * which is the difference a driver can see between an engine that packs
     * four to an entry and one that writes an entry per character.
     */
    if (offset == SGI_IOC3_SDMA_RX_PRODUCE) {
        val |= (uint32_t)s->sdma_packed[port] << SGI_IOC3_SDMA_HELD_SHIFT;
    }

    return val;
}

/*
 * One of a port's seven registers, written. What each one keeps is the
 * register's own business and none of them keeps a whole word: the control
 * register drops three bits it does not store, the indices keep a pointer as
 * wide as the ring, and two indices and the shadow are not software's to
 * write at all.
 */
static void sgi_ioc3_sdma_write_port(SGIIOC3State *s, unsigned int port,
                                     hwaddr offset, uint32_t val)
{
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);

    switch (offset) {
    case SGI_IOC3_SDMA_CONTROL:
        *sgi_ioc3_sdma_port_reg(s, port, offset) =
            val & ~SGI_IOC3_SDMA_CTRL_UNSTORED;
        if (val & SGI_IOC3_SDMA_RESET) {
            sgi_ioc3_sdma_reset_port(s, port);
        }
        /*
         * Enabling is a start, not only a setting. The diagnostic hands the
         * transmit ring over before it programs the ring base and before it
         * enables the engine at all, so an engine that walked only when
         * software advanced the produce index would never walk here.
         */
        sgi_ioc3_sdma_run(s, port);
        return;

    case SGI_IOC3_SDMA_TX_PRODUCE:
        *sgi_ioc3_sdma_port_reg(s, port, offset) = val & mask;
        sgi_ioc3_sdma_run(s, port);
        return;

    case SGI_IOC3_SDMA_RX_CONSUME:
        *sgi_ioc3_sdma_port_reg(s, port, offset) =
            val & (mask | SGI_IOC3_SDMA_ARM_RX_TIMER);
        sgi_ioc3_sdma_run(s, port);
        return;

    case SGI_IOC3_SDMA_RX_TIMER:
        *sgi_ioc3_sdma_port_reg(s, port, offset) =
            val & SGI_IOC3_SDMA_TIMER_RELOAD;
        return;

    case SGI_IOC3_SDMA_SHADOW:
        /*
         * OpenBSD writes zero here on every boot, alongside the port's
         * control register, to put the port into pass-through mode before
         * attaching its 16550 driver, and the firmware's diagnostic writes
         * the modem lines it has just set through the byte-wide window. The
         * real chip discards both the same way: the shadow is hardware's own
         * mirror of the lines the port drives, recomputed regardless of what
         * software last stored. Logging it would fire on every stock boot for
         * a write that is neither a driver bug nor observable.
         */
        return;

    case SGI_IOC3_SDMA_TX_CONSUME:
    case SGI_IOC3_SDMA_RX_PRODUCE:
        /*
         * These two are the engine's side of the ring. The datasheet's
         * contract is that no driver writes them and that a write here is a
         * real anomaly -- and the firmware's own base I/O diagnostic writes
         * both of them, zero, on every stock boot, which is a finding for
         * the datasheet rather than a change to make here. It writes them
         * immediately after resetting the channels, when they already hold
         * zero, so refusing the write costs the diagnostic nothing.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3: write to read-only serial DMA register "
                      "0x%03x\n",
                      (unsigned)(SGI_IOC3_SDMA_PORT_A +
                                 port * SGI_IOC3_SDMA_PORT_STRIDE + offset));
        return;
    }
}

static uint64_t sgi_ioc3_sdma_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;
    hwaddr reg = SGI_IOC3_SDMA_BASE + addr;
    unsigned int port;
    hwaddr offset;
    uint32_t val;

    if (sgi_ioc3_sdma_port_offset(reg, &port, &offset)) {
        val = sgi_ioc3_sdma_read_port(s, port, offset);
    } else {
        val = s->sdma_reg[addr / 4];
    }

    trace_sgi_ioc3_sdma_read((uint32_t)reg, val);
    return val;
}

static void sgi_ioc3_sdma_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3State *s = opaque;
    hwaddr reg = SGI_IOC3_SDMA_BASE + addr;
    unsigned int port;
    hwaddr offset;

    trace_sgi_ioc3_sdma_write((uint32_t)reg, (uint32_t)val);

    if (sgi_ioc3_sdma_port_offset(reg, &port, &offset)) {
        sgi_ioc3_sdma_write_port(s, port, offset, val);
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
 * The ethernet buffer RAM. Every slot answers and keeps what it is given, and
 * the two halves of the window are separate storage, which is the whole of
 * what a driver's size probe measures: it writes a pattern at the first slot
 * and the complement at the halfway slot and reads both back, so a window
 * aliasing at its midpoint reports half the RAM.
 *
 * Seventeen bits are kept because seventeen bits exist -- sixteen of data and
 * the parity bit stored beside them. The parity bit is storage like the rest
 * rather than something computed: no frame data ever lands in this window --
 * the ethernet engine's rings and buffers live in ordinary guest memory, not
 * here -- so there is nothing to compute a parity bit over, and a diagnostic
 * write is how software would place a deliberately wrong one anyway.
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
     * The chip's own PCI interrupt pin belongs to the ethernet block. That
     * block is a child device now (hw/net/sgi-ioc3-eth.c) with a GPIO output
     * of its own; sgi_ioc3_eth_irq below is what turns a level on that wire
     * into a call to this chip's own pci_set_irq, since the child has no PCI
     * identity to call it with directly. The SuperIO line a guest also cares
     * about on this machine is a different, directly wired line and is not
     * this pin at all; see superio_irq.
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
     * The ethernet block. Its DMA has to go through this chip's own PCI
     * function to reach the Bridge's address translation, so the address
     * space is handed down before the child realizes -- see
     * docs/datasheets/ioc3.md's "DMA" section for why every transfer there
     * must go through it rather than the plain system address space.
     */
    s->eth.dma_as = pci_get_address_space(dev);
    /*
     * And the serial engines' own, which is the same space for the same
     * reason: their rings sit in guest memory reached through the Bridge's
     * translation, so the transfer has to go through this chip's PCI address
     * space rather than around it.
     */
    s->dma_as = pci_get_address_space(dev);
    /*
     * And its network backend, claimed here rather than by the machine that
     * plugs this chip in. The block is a child device carrying the NIC
     * properties itself, so nothing outside can name it on a command line --
     * which left every machine but the Origin unable to give this chip a
     * network at all, and made the whole ethernet unreachable on any board
     * used to cross-check it away from the SGI fabric.
     */
    qemu_configure_nic_device(DEVICE(&s->eth), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eth), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_ENET_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->eth),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 0,
                       qdev_get_gpio_in_named(DEVICE(dev), "eth-irq", 0));

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
     * allocation left there, which is a defect nothing warns about. The same
     * is true of the ethernet block above and its own child, the transceiver.
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
     * The ethernet block is a child device now and resets on its own --
     * resettable children reset in the same transaction their parent does,
     * which is the whole point of using the resettable phases API rather
     * than a plain reset method here.
     */

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
    s->chip_control = 0;
}

static void sgi_ioc3_init(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    qdev_init_gpio_in(DEVICE(obj), sgi_ioc3_serial_irq,
                      SGI_IOC3_SERIAL_PORTS);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_eth_irq, "eth-irq", 1);

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

    object_initialize_child(obj, "eth", &s->eth, TYPE_SGI_IOC3_ETH);

    /*
     * The ethernet block carries the network properties, and it is a child
     * nobody outside can name, so without these aliases the only way to give
     * this chip a backend is for the board to claim one on its behalf. That
     * left the ethernet unreachable on every machine but the Origin -- and
     * the whole point of plugging this card into a machine with none of the
     * SGI fabric under it is to find out which defects belong to the device.
     * A real IOC3 is also sold as a plain PCI card, so being pluggable is
     * what the part does rather than a concession to testing.
     */
    object_property_add_alias(obj, "netdev", OBJECT(&s->eth), "netdev");
    object_property_add_alias(obj, "mac", OBJECT(&s->eth), "mac");

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_M48T35);
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 11,
    .minimum_version_id = 11,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC3State),
        VMSTATE_UINT32_V(int_status, SGIIOC3State, 2),
        VMSTATE_UINT32_V(int_enable, SGIIOC3State, 2),
        VMSTATE_STRUCT_ARRAY(serial, SGIIOC3State, SGI_IOC3_SERIAL_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(microlan_control, SGIIOC3State),
        VMSTATE_UINT32(pin_control, SGIIOC3State),
        VMSTATE_UINT32(pin_data, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(sdma_reg, SGIIOC3State, SGI_IOC3_SDMA_REGS),
        VMSTATE_UINT32_V(chip_control, SGIIOC3State, 11),
        VMSTATE_UINT8_2DARRAY_V(sdma_packer, SGIIOC3State,
                                SGI_IOC3_SERIAL_PORTS,
                                SGI_IOC3_SDMA_ENTRY_SIZE, 11),
        VMSTATE_UINT8_ARRAY_V(sdma_packed, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS, 11),
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
