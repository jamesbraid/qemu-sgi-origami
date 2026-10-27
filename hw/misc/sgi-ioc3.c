/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI IOC3 PCI base I/O: serial DMA, Ethernet, PS/2, MicroLAN and timekeeper.
 */
/*
 * One PCI device and BAR hold two 16550s, Ethernet buffer RAM and the byte
 * bus. Add-in cards may carry board identity beside the station address on
 * MicroLAN; board-* properties describe that population.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "hw/misc/sgi-board-record.h"
#include "hw/char/sgi-ioc-serial-dma.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "net/util.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "net/net.h"
#include "trace.h"

/* Word conversion keeps child DMA formats independent of PCI placement. */
static MemTxResult sgi_ioc3_pci_dma_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIIOC3State *s = opaque;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    assert(size <= sizeof(bytes));
    result = address_space_read(pci_get_address_space(PCI_DEVICE(s)),
                                addr ^ (4 - size), attrs, bytes, size);
    *value = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult sgi_ioc3_pci_dma_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIIOC3State *s = opaque;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_be_p(bytes, size, value);
    return address_space_write(pci_get_address_space(PCI_DEVICE(s)),
                               addr ^ (4 - size), attrs, bytes, size);
}

/* Bounds the stack a buffer write converts into before sending it on. */
#define SGI_IOC3_DMA_SCRATCH    1024

static void sgi_ioc3_dma_reverse_words(uint8_t *dst, const uint8_t *src,
                                       hwaddr len)
{
    hwaddr i;

    for (i = 0; i < len; i += 4) {
        stl_le_p(dst + i, ldl_be_p(src + i));
    }
}

/*
 * The whole words of a long transfer, converted and passed on as one
 * buffer. A word is what one aligned 4-byte access converts, so the bytes
 * match the per-access path; a partial word at either end is left to it.
 * The onward transfer is buffer-only: it moves what lies in RAM or what a
 * region beyond, such as the Bridge's placement, takes as a buffer, and
 * stops where an ordinary access would be needed. The words that moved
 * are consumed; the core makes today's per-access steps for the rest, so
 * whatever sits on the PCI side sees the accesses it saw before. Should an
 * onward region stop inside a word, the bytes of that word it did move are
 * moved again, unchanged, by the per-access step, even when no whole word
 * moved and the callback declines. A partial word cannot be consumed: its
 * moved bytes are the far end of the word on the device side. The memory
 * API allows the re-move; see docs/devel/memory.rst.
 */
static hwaddr sgi_ioc3_pci_dma_read_buffer(void *opaque, hwaddr addr,
                                           void *buffer, hwaddr len,
                                           MemTxAttrs attrs,
                                           MemTxResult *result)
{
    SGIIOC3State *s = opaque;
    AddressSpace *pci = pci_get_address_space(PCI_DEVICE(s));
    hwaddr span = QEMU_ALIGN_DOWN(len, 4);
    uint8_t scratch[SGI_IOC3_DMA_SCRATCH];
    hwaddr off, n, moved;
    MemTxResult part;

    if (addr & 3) {
        return 0;
    }
    for (off = 0; off < span; off += moved) {
        n = MIN(span - off, sizeof(scratch));
        moved = QEMU_ALIGN_DOWN(address_space_read_buffer_only(pci,
                                    addr + off, attrs, scratch, n, &part), 4);
        if (!moved) {
            break;
        }
        sgi_ioc3_dma_reverse_words((uint8_t *)buffer + off, scratch, moved);
        *result |= part;
        if (moved < n) {
            off += moved;
            break;
        }
    }
    if (off) {
        trace_sgi_ioc3_pci_dma_buffer(s, "read", addr, off);
    }
    return off;
}

static hwaddr sgi_ioc3_pci_dma_write_buffer(void *opaque, hwaddr addr,
                                            const void *buffer, hwaddr len,
                                            MemTxAttrs attrs,
                                            MemTxResult *result)
{
    SGIIOC3State *s = opaque;
    AddressSpace *pci = pci_get_address_space(PCI_DEVICE(s));
    hwaddr span = QEMU_ALIGN_DOWN(len, 4);
    uint8_t scratch[SGI_IOC3_DMA_SCRATCH];
    hwaddr off, n, moved;
    MemTxResult part;

    if (addr & 3) {
        return 0;
    }
    for (off = 0; off < span; off += moved) {
        n = MIN(span - off, sizeof(scratch));
        sgi_ioc3_dma_reverse_words(scratch, (const uint8_t *)buffer + off, n);
        moved = QEMU_ALIGN_DOWN(address_space_write_buffer_only(pci,
                                    addr + off, attrs, scratch, n, &part), 4);
        if (!moved) {
            break;
        }
        *result |= part;
        if (moved < n) {
            off += moved;
            break;
        }
    }
    if (off) {
        trace_sgi_ioc3_pci_dma_buffer(s, "write", addr, off);
    }
    return off;
}

static const MemoryRegionOps sgi_ioc3_pci_dma_ops = {
    .read_with_attrs = sgi_ioc3_pci_dma_read,
    .write_with_attrs = sgi_ioc3_pci_dma_write,
    .read_buffer = sgi_ioc3_pci_dma_read_buffer,
    .write_buffer = sgi_ioc3_pci_dma_write_buffer,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* The register view changes narrow placement, preserving transferred bytes. */
static MemTxResult sgi_ioc3_pci_bar_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIIOC3State *s = opaque;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    assert(size <= sizeof(bytes));
    result = address_space_read(&s->bar_as, addr ^ (4 - size),
                                attrs, bytes, size);
    *value = ldn_le_p(bytes, size);
    return result;
}

static MemTxResult sgi_ioc3_pci_bar_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIIOC3State *s = opaque;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_le_p(bytes, size, value);
    return address_space_write(&s->bar_as, addr ^ (4 - size),
                               attrs, bytes, size);
}

static const MemoryRegionOps sgi_ioc3_pci_bar_ops = {
    .read_with_attrs = sgi_ioc3_pci_bar_read,
    .write_with_attrs = sgi_ioc3_pci_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void sgi_ioc3_update_irq(SGIIOC3State *s)
{
    qemu_set_irq(s->superio_irq, !!(s->int_status & s->int_enable));
}

static void sgi_ioc3_serial_irq(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = port ? SGI_IOC3_INT_SERIAL_B : SGI_IOC3_INT_SERIAL_A;

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);

    /*
     * On every level, not assertion alone: IRIX's DMA-ring console receives
     * with the 16550 interrupts disabled entirely.
     */
    sgi_ioc_sdma_kick(&s->sdma_engine, port);
}

static void sgi_ioc3_sdma_tx_ready(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;

    sgi_ioc_sdma_set_tx_ready(&s->sdma_engine, port, level);
}

/* The ethernet child's GPIO level becomes this chip's PCI interrupt pin. */
static void sgi_ioc3_eth_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * The PS/2 controller's data-available line latches interrupt-status bit 22.
 * The controller asserts it for every character it takes off a port, and
 * drops it once a guest read leaves both its receive holdings empty; the
 * latch holds across that drain, and only a write-one-to-clear of the status
 * register acknowledges it, which is what both Linux's edge handler and
 * IRIX's acknowledge writes expect. Nothing here remembers the line: a byte
 * the controller is still holding when the guest acknowledges is signalled
 * again by the controller, not re-latched here.
 */
static void sgi_ioc3_kbc_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;

    if (level) {
        s->int_status |= SGI_IOC3_INT_KEYBOARD;
        sgi_ioc3_update_irq(s);
    }
}

/*
 * The parallel port's four sources. Its service interrupt is a level, like
 * the two serial pass-throughs: what quiets it is a write to the port's own
 * extended control register, so a write-one-to-clear here would drop an
 * interrupt this chip would never raise again. The three below it (one per
 * DMA context, then the DMA fault) latch on the port's pulse and the driver
 * clears each by writing its bit back.
 */
static void sgi_ioc3_parallel_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = 1u << (SGI_IOC3_INT_PARALLEL_SHIFT + n);

    if (n == SGI_IOC3_PAR_IRQ_SERVICE) {
        s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    } else if (level) {
        s->int_status |= bit;
    }
    sgi_ioc3_update_irq(s);
}

/*
 * The station-address record the guests read out of the part's first page.
 * This is image content, not device behaviour: the marker and the reversed
 * address are the record's own format, and so is the checksum at bytes 11 and
 * 12, which a DS2502-class part never computes; an embedded page checksum
 * is a software file-structure convention, while the part's own CRCs always
 * cover a whole page. It is stored inverted so a guest shifting the record
 * and its checksum through the generator lands on the fixed residue every
 * one-wire CRC-16 stream ends on.
 */
static void sgi_ioc3_onewire_build_page(SGIIOC3State *s,
                                        uint8_t page[DS2502_PAGE_SIZE])
{
    uint16_t crc16;
    unsigned int i;

    memset(page, 0, DS2502_PAGE_SIZE);   /* unconstrained by either guest */
    page[0] = 0x0a;                      /* the marker both guests require */
    for (i = 0; i < 6; i++) {
        page[10 - i] = s->eth.conf.macaddr.a[i];
    }
    crc16 = ~onewire_crc16(0, page, 11);
    page[11] = crc16 & 0xff;
    page[12] = crc16 >> 8;
}

/* One bit slot, reported finished inside the write the guest polls after. */
static void sgi_ioc3_onewire_slot(SGIIOC3State *s, uint32_t val)
{
    uint32_t pulse = val >> SGI_IOC3_ONEWIRE_PULSE_SHIFT;
    uint32_t sample = (val >> SGI_IOC3_ONEWIRE_SAMPLE_SHIFT) &
                      SGI_IOC3_ONEWIRE_SAMPLE_MASK;
    bool level;

    if (!(s->pin_control & SGI_IOC3_PIN_MICROLAN_ENABLE) ||
        (!s->board->onewire_identity && !s->board_part)) {
        /*
         * Two ways to an empty bus. Pin-control bit 21 routes the engine onto
         * its pin, and with the bit clear the bus is disconnected; and a
         * board with no ethernet and no board record carries no part, the
         * station-address part being wired up with the ethernet rather than
         * on its own. Either way no part answers: the wire reads high and the
         * slot advances no conversation.
         */
        level = true;
    } else {
        level = sgi_onewire_slot(s->onewire_bus, pulse, sample);
    }

    s->microlan_control = (val & SGI_IOC3_MICROLAN_TIMING) |
                          SGI_IOC3_MICROLAN_DONE |
                          (level ? SGI_IOC3_MICROLAN_LEVEL : 0);
}

static bool sgi_ioc3_int_out_periodic_has_no_low_phase(uint32_t active)
{
    return (active & SGI_IOC3_INT_OUT_MODE) ==
               SGI_IOC3_INT_OUT_PERIODIC &&
           (active & SGI_IOC3_INT_OUT_COUNT) + 1 <=
               SGI_IOC3_INT_OUT_PULSE_TICKS;
}

static void sgi_ioc3_int_out_hold_high(SGIIOC3State *s)
{
    /* Keep int_out's programmed readback; active becomes internally HIGH. */
    s->int_out_active = SGI_IOC3_INT_OUT_HIGH;
    s->int_out_state = true;
    s->int_out_pulse_end = false;
    s->int_out_square_started = false;
    timer_del(s->int_out_timer);
}

/*
 * Every rising edge of the output posts the output-pulse source, whatever
 * mode produced it; the mask only decides the pin.
 */
static void sgi_ioc3_int_out_drive(SGIIOC3State *s, bool level)
{
    if (level && !s->int_out_state) {
        s->int_status |= SGI_IOC3_INT_RT_OUTPUT_PULSE;
        sgi_ioc3_update_irq(s);
    }
    s->int_out_state = level;
}

/*
 * One transition due at @at. The next is scheduled from @at rather than from
 * whenever this runs, so a late transition keeps the output's phase.
 */
static void sgi_ioc3_int_out_step(SGIIOC3State *s, int64_t at)
{
    switch (s->int_out_active & SGI_IOC3_INT_OUT_MODE) {
    case SGI_IOC3_INT_OUT_LOW:
        sgi_ioc3_int_out_drive(s, false);
        s->int_out_pulse_end = false;
        break;
    case SGI_IOC3_INT_OUT_HIGH:
        sgi_ioc3_int_out_drive(s, true);
        s->int_out_pulse_end = false;
        break;
    case SGI_IOC3_INT_OUT_ONE_SHOT:
        if (!s->int_out_pulse_end) {
            sgi_ioc3_int_out_drive(s, true);
            s->int_out_pulse_end = true;
            timer_mod(s->int_out_timer, at + SGI_IOC3_INT_OUT_PULSE_TICKS *
                                             SGI_IOC3_INT_OUT_TICK_NS);
        } else {
            sgi_ioc3_int_out_drive(s, false);
            s->int_out_pulse_end = false;
        }
        break;
    case SGI_IOC3_INT_OUT_PERIODIC:
        if (sgi_ioc3_int_out_periodic_has_no_low_phase(s->int_out_active)) {
            sgi_ioc3_int_out_drive(s, true);
            sgi_ioc3_int_out_hold_high(s);
        } else if (!s->int_out_pulse_end) {
            sgi_ioc3_int_out_drive(s, true);
            s->int_out_pulse_end = true;
            timer_mod(s->int_out_timer, at + SGI_IOC3_INT_OUT_PULSE_TICKS *
                                             SGI_IOC3_INT_OUT_TICK_NS);
        } else {
            sgi_ioc3_int_out_drive(s, false);
            s->int_out_pulse_end = false;
            timer_mod(s->int_out_timer,
                      at + ((s->int_out_active & SGI_IOC3_INT_OUT_COUNT) + 1 -
                            SGI_IOC3_INT_OUT_PULSE_TICKS) *
                           SGI_IOC3_INT_OUT_TICK_NS);
        }
        break;
    case SGI_IOC3_INT_OUT_SQUARE:
        if (s->int_out_square_started) {
            sgi_ioc3_int_out_drive(s, !s->int_out_state);
        } else {
            s->int_out_square_started = true;
        }
        timer_mod(s->int_out_timer,
                  at + ((s->int_out_active & SGI_IOC3_INT_OUT_COUNT) + 1) *
                       SGI_IOC3_INT_OUT_TICK_NS);
        break;
    }
}

static void sgi_ioc3_int_out_expire(void *opaque)
{
    SGIIOC3State *s = opaque;

    sgi_ioc3_int_out_step(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * Transitions already due happen before a read, not whenever the main loop
 * next services the timer: the IO-8 board test samples a periodic output in a
 * tight loop and needs both levels.
 */
static void sgi_ioc3_int_out_catch_up(SGIIOC3State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    while (timer_pending(s->int_out_timer)) {
        int64_t due = timer_expire_time_ns(s->int_out_timer);

        if (due > now) {
            break;
        }
        timer_del(s->int_out_timer);
        sgi_ioc3_int_out_step(s, due);
    }
}

static bool sgi_ioc3_int_out_command_valid(uint32_t value)
{
    uint32_t mode = value & SGI_IOC3_INT_OUT_MODE;
    uint32_t count = value & SGI_IOC3_INT_OUT_COUNT;

    if (value & SGI_IOC3_INT_OUT_DIAG) {
        return false;
    }
    if (mode == SGI_IOC3_INT_OUT_LOW || mode == SGI_IOC3_INT_OUT_HIGH) {
        return true;
    }
    /*
     * The internal strobe preserves reset count zero and still emits a pulse.
     */
    if (mode == SGI_IOC3_INT_OUT_ONE_SHOT && count == 0) {
        return true;
    }

    return count >= SGI_IOC3_INT_OUT_MIN_COUNT &&
           count <= SGI_IOC3_INT_OUT_MAX_COUNT &&
           (mode == SGI_IOC3_INT_OUT_ONE_SHOT ||
            (mode == SGI_IOC3_INT_OUT_PERIODIC &&
             count >= SGI_IOC3_INT_OUT_PULSE_TICKS) ||
            mode == SGI_IOC3_INT_OUT_SQUARE);
}

/* Fitted board inputs remain independent of the software output latch. */
static void sgi_ioc3_update_gpio5(SGIIOC3State *s)
{
    /* Logical board request; the composition supplies the connection. */
    qemu_set_irq(s->gpio5_reset_request,
                 (s->pin_control & (1u << 5)) && !(s->pin_data & (1u << 5)));
}

static uint32_t sgi_ioc3_pin_data(SGIIOC3State *s)
{
    return (s->pin_data & ~s->pin_input_mask) | s->pin_input_values;
}

static uint64_t sgi_ioc3_main_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;

    if (addr >= SGI_IOC3_PIN_REG_FIRST && addr <= SGI_IOC3_PIN_REG_LAST &&
        (addr & 3) == 0) {
        unsigned int pin = (addr - SGI_IOC3_PIN_REG_FIRST) / 4;

        return (sgi_ioc3_pin_data(s) >> pin) & 1;
    }

    switch (addr) {
    case SGI_IOC3_INT_STATUS:
        sgi_ioc_sdma_catch_up(&s->sdma_engine);
        return s->int_status;
    case SGI_IOC3_INT_ENABLE_SET:
        /*
         * The set alias reads back the live mask; the clear alias does not.
         */
        return s->int_enable;
    case SGI_IOC3_CHIP_CONTROL:
        /*
         * Bit 22: arbiter idle, polled by the kernel's console setup. It
         * reads busy only while the serial DMA engine is inside a transfer,
         * the rule IOC4's serial idle bit follows too.
         */
        return s->chip_control |
               (sgi_ioc_sdma_idle(&s->sdma_engine) ?
                SGI_IOC3_CHIP_CTRL_ARB_IDLE : 0);
    case SGI_IOC3_INT_OUTPUT:
        sgi_ioc3_int_out_catch_up(s);
        return s->int_out |
               (s->int_out_state ? SGI_IOC3_INT_OUT_STATE : 0);
    case SGI_IOC3_MICROLAN_CONTROL:
        return s->microlan_control;
    case SGI_IOC3_PIN_CONTROL_SET:
        return s->pin_control;
    case SGI_IOC3_PIN_DATA:
        return sgi_ioc3_pin_data(s);
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
        sgi_ioc3_update_gpio5(s);
        return;
    }

    switch (addr) {
    case SGI_IOC3_INT_STATUS:
        /* An acknowledgement must not be undone by a late expiry. */
        sgi_ioc_sdma_catch_up(&s->sdma_engine);
        /* Write-one-to-clear; the level-driven bits ignore the write. */
        s->int_status &= ~(val & ~(SGI_IOC3_INT_PASSTHROUGH |
                                   SGI_IOC3_INT_PARALLEL_SERVICE |
                                   SGI_IOC3_INT_RX_HIGH_WATER_LEVEL |
                                   SGI_IOC3_INT_TX_EMPTY_LEVEL |
                                   SGI_IOC3_INT_RX_FULL_LEVEL));
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
         * The port offsets aim the serial DMA engines (sdma_aimed()); an
         * engine aimed here may have work waiting that no event will start.
         */
        trace_sgi_ioc3_chip_control_write(s, s->chip_control,
                                          val & SGI_IOC3_CHIP_CTRL_MASK);
        s->chip_control = val & SGI_IOC3_CHIP_CTRL_MASK;
        sgi_ioc_sdma_kick(&s->sdma_engine, 0);
        sgi_ioc_sdma_kick(&s->sdma_engine, 1);
        break;
    case SGI_IOC3_INT_OUTPUT: {
        uint32_t int_out = val & SGI_IOC3_INT_OUT_MASK;

        /*
         * Stored as written, as the IO-8 board test requires; a command with
         * no defined behaviour leaves the running output alone.
         */
        s->int_out = int_out;
        if (!sgi_ioc3_int_out_command_valid(int_out)) {
            break;
        }

        timer_del(s->int_out_timer);
        s->int_out_active = s->int_out & (SGI_IOC3_INT_OUT_MODE |
                                          SGI_IOC3_INT_OUT_COUNT);
        s->int_out_pulse_end = false;
        s->int_out_square_started = false;
        if ((s->int_out_active & SGI_IOC3_INT_OUT_MODE) ==
            SGI_IOC3_INT_OUT_ONE_SHOT) {
            s->int_out_state = false;
        }
        /*
         * A fixed level is there for the next read, which the IO-8 board test
         * checks; a timed mode starts after the MMIO write returns.
         */
        if ((int_out & SGI_IOC3_INT_OUT_MODE) == SGI_IOC3_INT_OUT_LOW ||
            (int_out & SGI_IOC3_INT_OUT_MODE) == SGI_IOC3_INT_OUT_HIGH) {
            sgi_ioc3_int_out_expire(s);
        } else {
            timer_mod(s->int_out_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
        }
        break;
    }
    case SGI_IOC3_MICROLAN_CONTROL:
        sgi_ioc3_onewire_slot(s, (uint32_t)val);
        break;
    case SGI_IOC3_PIN_CONTROL_SET:
        s->pin_control |= val;
        sgi_ioc3_update_gpio5(s);
        break;
    case SGI_IOC3_PIN_CONTROL_CLR:
        s->pin_control &= ~val;
        sgi_ioc3_update_gpio5(s);
        break;
    case SGI_IOC3_PIN_DATA:
        s->pin_data = val;
        sgi_ioc3_update_gpio5(s);
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
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The serial DMA engines. A ring's address is derived from its port and
 * direction; no register reads it back.
 */

/* The 16550's published layout; hw/char/serial.c's macros are private. */
#define UART_RBR_THR    0
#define UART_LCR        3
#define UART_MCR        4
#define UART_LSR        5
#define UART_MSR        6
#define UART_SCR        7

#define UART_LSR_DR     0x01
#define UART_LSR_OE     0x02
#define UART_LSR_PE     0x04
#define UART_LSR_FE     0x08
#define UART_LSR_BI     0x10

#define UART_MCR_DTR    0x01
#define UART_MCR_RTS    0x02
#define UART_MCR_OUT1   0x04
#define UART_MCR_OUT2   0x08
#define UART_MCR_LOOP   0x10
/* Auto-RTS/CTS enable, which the shared 16550 core has no bit for. */
#define UART_MCR_AFE    0x20

/* What a factory-test write to line status reaches: everything but bit 7. */
#define SGI_IOC3_UART_LSR_TEST_MASK 0x7f
/* And in modem status: the four change indicators, not the live levels. */
#define SGI_IOC3_UART_MSR_TEST_MASK 0x0f

#define UART_MSR_DCTS   0x01
#define UART_MSR_DDCD   0x08
#define UART_MSR_CTS    0x10
#define UART_MSR_DCD    0x80

#define UART_LCR_DLAB   0x80

/*
 * IOC3 overloads the scratch register: while a port's divisor latch is
 * selected, a write to its scratch register (offset 7) programs the 16550's
 * clock pre-divider instead of storing a scratch byte. The value written is
 * twice the divider wanted (the PROM and IRIX write 6, for a divide of 3)
 * and the reference clock the ordinary divisor latches then work from is the
 * crystal divided by that divider.
 *
 * The commonly quoted figure for this crystal is 22,000,000 Hz, but that
 * appears to be a rounded figure rather than the real part: SGI's manual
 * calls these 460,800-baud ports, and 22.1184 MHz / 3 / 16 lands exactly on
 * 460,800 and on the standard rates below it (divisor 48 gives exactly 9600),
 * where the rounded 22 MHz figure is half a percent off. A guest that only
 * ever reads back its own writes cannot tell the two figures apart; it
 * matters here because the emulated baud rate has to line up with a real host
 * serial port.
 */
#define SGI_IOC3_UART_CRYSTAL_HZ 22118400

/*
 * Offset 7 always stores into the 16550 core's own scratch byte first (see
 * the caller), so a guest reads back exactly what it wrote whether or not the
 * divisor latch is selected. This is the overload on top of that: while it is
 * selected, also derive the reference clock the port's baud rate is computed
 * from. A written 0 or 1 would divide by zero; no meaning is documented for
 * either, and no observed guest writes one, so they are read as
 * "leave the reference clock alone".
 */
static void sgi_ioc3_serial_program_predivider(SGIIOC3State *s,
                                               unsigned int port,
                                               uint8_t predivider)
{
    SerialState *serial = &s->serial[port];
    unsigned int divider = predivider / 2;

    if (divider == 0) {
        return;
    }

    s->serial_predivider[port] = predivider;
    serial->baudbase = SGI_IOC3_UART_CRYSTAL_HZ / divider / 16;

    /*
     * serial.c only recomputes the baud rate on a divisor-latch or
     * line-control write. Re-issue the port's current line control so a
     * pre-divider written after the divisor already latched takes effect at
     * once, rather than waiting on whatever next touches either register.
     */
    serial_io_ops.write(serial, UART_LCR, serial->lcr, 1);
}

/*
 * Every character the chip hands a port's UART to transmit, written to a
 * backend of its own. Loopback keeps its characters inside the port.
 */
static void sgi_ioc3_serial_record(SGIIOC3State *s, unsigned int port,
                                   uint8_t ch)
{
    if (!qemu_chr_fe_backend_connected(&s->serial_record[port])) {
        return;
    }
    if (s->serial[port].mcr & UART_MCR_LOOP) {
        return;
    }
    qemu_chr_fe_write_all(&s->serial_record[port], &ch, 1);
}

/* Byte-wide only; both guests place their own byte lanes; add none here. */
static uint64_t sgi_ioc3_serial_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3SerialAlias *handle = opaque;
    SerialState *serial = &handle->chip->serial[handle->port];
    bool is_rbr = addr == UART_RBR_THR && !(serial->lcr & UART_LCR_DLAB);
    uint64_t val = serial_io_ops.read(&handle->chip->serial[handle->port],
                                      addr, 1);

    if (addr == UART_MCR) {
        val |= handle->chip->serial_mcr_autoflow[handle->port];
    }

    /*
     * A factory-test write shows up in the read that follows it and no later
     * one: reading line status is what clears the error bits it names, so
     * nothing here outlives its own read.
     */
    if (addr == UART_LSR) {
        val |= handle->chip->serial_lsr_test[handle->port];
        handle->chip->serial_lsr_test[handle->port] = 0;
    }

    /* Modem status clears its change indicators on a read the same way. */
    if (addr == UART_MSR) {
        val |= handle->chip->serial_msr_test[handle->port];
        handle->chip->serial_msr_test[handle->port] = 0;
        if (sgi_ioc_sdma_flow_cts(&handle->chip->sdma_engine, handle->port)) {
            val |= UART_MSR_CTS;
        }
    }

    if (is_rbr) {
        trace_sgi_ioc3_serial_data_read(handle->chip, 'A' + handle->port,
                                        addr);
    } else {
        trace_sgi_ioc3_serial_read(handle->chip, 'A' + handle->port, addr,
                                   val);
    }
    return val;
}

static void sgi_ioc3_serial_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    SGIIOC3SerialAlias *handle = opaque;
    SerialState *serial = &handle->chip->serial[handle->port];
    bool is_thr = addr == UART_RBR_THR && !(serial->lcr & UART_LCR_DLAB);
    bool is_predivider = addr == UART_SCR && (serial->lcr & UART_LCR_DLAB);

    if (is_thr) {
        trace_sgi_ioc3_serial_data_write(handle->chip, 'A' + handle->port,
                                         addr);
    } else {
        trace_sgi_ioc3_serial_write(handle->chip, 'A' + handle->port,
                                    addr, val & 0xff);
    }

    /* The one modem-control bit the shared 16550 core masks away. */
    if (addr == UART_MCR) {
        handle->chip->serial_mcr_autoflow[handle->port] = val & UART_MCR_AFE;
    }

    /*
     * Line status takes a write for factory testing, which the shared core
     * has nowhere to put. Bit 7 is not one of them.
     */
    if (addr == UART_LSR) {
        handle->chip->serial_lsr_test[handle->port] =
            val & SGI_IOC3_UART_LSR_TEST_MASK;
    }

    if (addr == UART_MSR) {
        handle->chip->serial_msr_test[handle->port] =
            val & SGI_IOC3_UART_MSR_TEST_MASK;
    }

    serial_io_ops.write(serial, addr, val & 0xff, 1);

    if (is_thr) {
        sgi_ioc3_serial_record(handle->chip, handle->port, val & 0xff);
    } else if (is_predivider) {
        sgi_ioc3_serial_program_predivider(handle->chip, handle->port,
                                           val & 0xff);
    }
}

static const MemoryRegionOps sgi_ioc3_serial_ops = {
    .read = sgi_ioc3_serial_read,
    .write = sgi_ioc3_serial_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

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

static void sgi_ioc3_sdma_raise(void *opaque, unsigned port,
                                SGIIOCSerialDMASource src)
{
    SGIIOC3State *s = opaque;

    s->int_status |= SGI_IOC3_SERIAL_SOURCE(port, src);
    sgi_ioc3_update_irq(s);
}

static void sgi_ioc3_sdma_set_level(void *opaque, unsigned port,
                                    SGIIOCSerialDMASource src, bool level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = SGI_IOC3_SERIAL_SOURCE(port, src);

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);
}

/*
 * Chip control places each port's 16550 for its DMA engine. Aimed anywhere
 * else, the engine does not reach the port; what it reaches instead is not
 * modelled. The walk also drives a 16550 the board may not have fitted.
 */
static bool sgi_ioc3_sdma_reachable(void *opaque, unsigned port)
{
    SGIIOC3State *s = opaque;
    static const hwaddr uart[SGI_IOC3_SERIAL_PORTS] = {
        SGI_IOC3_SERIAL_A, SGI_IOC3_SERIAL_B,
    };
    unsigned int shift = port ? SGI_IOC3_CHIP_CTRL_PORT_B_SHIFT
                              : SGI_IOC3_CHIP_CTRL_PORT_A_SHIFT;
    uint32_t field = (s->chip_control >> shift) &
                     SGI_IOC3_CHIP_CTRL_PORT_FIELD;

    if (!s->board->serial[port]) {
        return false;
    }
    return field == (uart[port] & 0xfff) / 8;
}

static void sgi_ioc3_sdma_transmitted(void *opaque, unsigned port, uint8_t ch)
{
    sgi_ioc3_serial_record(opaque, port, ch);
}

/* IOC3's header defines the held counts; an IOC3 halt is not known. */
static const SGIIOCSerialDMAParams sgi_ioc3_sdma_params = {
    .name = "sgi-ioc3",
    .held_counts = true,
    .halt_on_dma_error = false,
};

static const SGIIOCSerialDMAOps sgi_ioc3_sdma_engine_ops = {
    .raise = sgi_ioc3_sdma_raise,
    .set_level = sgi_ioc3_sdma_set_level,
    .reachable = sgi_ioc3_sdma_reachable,
    .transmitted = sgi_ioc3_sdma_transmitted,
};

static uint64_t sgi_ioc3_sdma_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;
    hwaddr reg = SGI_IOC3_SDMA_BASE + addr;
    unsigned int port;
    hwaddr offset;

    if (sgi_ioc3_sdma_port_offset(reg, &port, &offset)) {
        return sgi_ioc_sdma_port_read(&s->sdma_engine, port, offset);
    }
    return sgi_ioc_sdma_base_read(&s->sdma_engine, 0,
                                  reg == SGI_IOC3_SDMA_RING_HI);
}

static void sgi_ioc3_sdma_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3State *s = opaque;
    hwaddr reg = SGI_IOC3_SDMA_BASE + addr;
    unsigned int port;
    hwaddr offset;

    if (sgi_ioc3_sdma_port_offset(reg, &port, &offset)) {
        sgi_ioc_sdma_port_write(&s->sdma_engine, port, offset, val);
        return;
    }

    sgi_ioc_sdma_base_write(&s->sdma_engine, 0, reg == SGI_IOC3_SDMA_RING_HI,
                            val);
}

static const MemoryRegionOps sgi_ioc3_sdma_ops = {
    .read = sgi_ioc3_sdma_read,
    .write = sgi_ioc3_sdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* A per-port page mapping one engine into user space. */
static uint64_t sgi_ioc3_sdma_alias_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SGIIOC3SerialAlias *alias = opaque;

    if (addr >= SGI_IOC_SDMA_PORT_REGS * 4) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: read of serial DMA alias offset 0x%"
                      HWADDR_PRIx " which is past the seven registers a page "
                      "exposes\n", addr);
        return 0;
    }

    return sgi_ioc_sdma_port_read(&alias->chip->sdma_engine, alias->port,
                                  addr);
}

static void sgi_ioc3_sdma_alias_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIIOC3SerialAlias *alias = opaque;

    if (addr >= SGI_IOC_SDMA_PORT_REGS * 4) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: write of 0x%08x to serial DMA alias offset 0x%"
                      HWADDR_PRIx " which is past the seven registers a page "
                      "exposes\n", (uint32_t)val, addr);
        return;
    }

    /*
     * Three of the seven are read-only through a page, which is the whole
     * point of exposing one to a user process: the two indexes the engine
     * advances and the 16550 shadow. The main block takes writes to the first
     * two.
     */
    if (addr == SGI_IOC_SDMA_TX_CONSUME ||
        addr == SGI_IOC_SDMA_RX_PRODUCE ||
        addr == SGI_IOC_SDMA_SHADOW) {
        sgi_ioc_sdma_write_refused(&alias->chip->sdma_engine, alias->port,
                                   addr, val);
        return;
    }

    sgi_ioc_sdma_port_write(&alias->chip->sdma_engine, alias->port, addr,
                            val);
}

static const MemoryRegionOps sgi_ioc3_sdma_alias_ops = {
    .read = sgi_ioc3_sdma_alias_read,
    .write = sgi_ioc3_sdma_alias_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The ethernet buffer RAM: 17 stored bits per slot, parity included, so a
 * diagnostic can place a wrong parity bit deliberately.
 */
static uint64_t sgi_ioc3_ssram_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;
    uint32_t word = s->ssram_word[addr / 4];

    if (sgi_ioc3_eth_ram_parity_enabled(&s->eth) && (ctpop32(word) & 1)) {
        word |= SGI_IOC3_SSRAM_PARITY_ERROR;
    }

    return word;
}

static void sgi_ioc3_ssram_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SGIIOC3State *s = opaque;
    uint32_t word = val & SGI_IOC3_SSRAM_STORED;

    /* With checking enabled, bit 17 requests generated stored parity. */
    if (sgi_ioc3_eth_ram_parity_enabled(&s->eth) &&
        (val & SGI_IOC3_SSRAM_PARITY_ERROR)) {
        word &= ~SGI_IOC3_SSRAM_PARITY;
        if (ctpop32(word & SGI_IOC3_SSRAM_DATA) & 1) {
            word |= SGI_IOC3_SSRAM_PARITY;
        }
    }
    s->ssram_word[addr / 4] = word;
}

static const MemoryRegionOps sgi_ioc3_ssram_ops = {
    .read = sgi_ioc3_ssram_read,
    .write = sgi_ioc3_ssram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The sub-devices each board fits, keyed by the subsystem identity a guest
 * reads out of configuration space. Mainline Linux's per-board setup
 * routines (drivers/mfd/ioc3.c, v7.2-rc7, commit db2ddb8) provide the
 * starting population. Its driver has no parallel child, so physical
 * parallel ports also follow the board manuals and OpenBSD attachment.
 * The IP29 System Board and the IP27 BaseIO6G share one Linux routine.
 */
static const struct {
    uint16_t subsystem_id;
    SGIIOC3Population board;
} sgi_ioc3_boards[] = {
    { SGI_IOC3_SUBSYS_IP27_BASEIO6G,
      { .eth = true, .serial = { true, true }, .kbc = true,
        .onewire_identity = true,
        .timekeeper = SGI_IOC3_TIMEKEEPER_M48T35 } },
    { SGI_IOC3_SUBSYS_IP27_MIO,
      { .serial = { true, true }, .kbc = true, .parallel = true } },
    { SGI_IOC3_SUBSYS_IP27_BASEIO,
      { .eth = true, .serial = { true, true },
        .onewire_identity = true,
        .timekeeper = SGI_IOC3_TIMEKEEPER_M48T35 } },
    { SGI_IOC3_SUBSYS_IP29_SYSBOARD,
      { .eth = true, .serial = { true, true }, .kbc = true,
        .parallel = true, .onewire_identity = true,
        .timekeeper = SGI_IOC3_TIMEKEEPER_M48T35 } },
    /*
     * The Octane's clock is a DS1685 at byte-bus devices 1 and 2, not the
     * M48T35 at device 0, and substituting the Timekeeper would hand a guest
     * a different register layout under the right name. That is why the kind
     * of clock is named here rather than a bare "has one".
     *
     * The front-panel LEDs on two of the general-purpose pins are still a
     * gap: this is the only board that carries them, and a guest that looks
     * finds nothing rather than the wrong thing.
     */
    { SGI_IOC3_SUBSYS_IP30_SYSBOARD,
      { .eth = true, .serial = { true, true }, .kbc = true,
        .parallel = true, .onewire_identity = true,
        .timekeeper = SGI_IOC3_TIMEKEEPER_DS1685 } },
    { SGI_IOC3_SUBSYS_MENET,
      { .eth = true, .serial = { true, true },
        .onewire_identity = true } },
    { SGI_IOC3_SUBSYS_MENET4,
      { .eth = true, .onewire_identity = true } },
};

/*
 * Everything the table does not name, the CAD DUO. This is a fall-through and
 * not a row: the driver runs it for any unmatched identity, the later SGI
 * boards it has no entry for included.
 */
static const SGIIOC3Population sgi_ioc3_cad_duo = {
    .eth = true, .kbc = true, .onewire_identity = true,
};

static const SGIIOC3Population *sgi_ioc3_board(uint16_t subsystem_id)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(sgi_ioc3_boards); i++) {
        if (sgi_ioc3_boards[i].subsystem_id == subsystem_id) {
            return &sgi_ioc3_boards[i].board;
        }
    }

    return &sgi_ioc3_cad_duo;
}

static bool sgi_ioc3_resolve_population(SGIIOC3State *s, Error **errp)
{
    SGIIOC3Population *population = &s->population;
    uint32_t unknown;

    if (s->timekeeper_kind > SGI_IOC3_TIMEKEEPER_DS1685) {
        error_setg(errp, "IOC3 timekeeper kind %u is unsupported",
                   s->timekeeper_kind);
        return false;
    }
    if (s->population_mask == SGI_IOC3_POPULATION_AUTO) {
        if (s->timekeeper_kind != SGI_IOC3_TIMEKEEPER_NONE) {
            error_setg(errp,
                       "IOC3 timekeeper kind %u requires an explicit population",
                       s->timekeeper_kind);
            return false;
        }
        s->board = sgi_ioc3_board(s->subsystem_id);
        return true;
    }

    memset(population, 0, sizeof(*population));
    s->board = population;
    unknown = s->population_mask & ~SGI_IOC3_POPULATION_ALL;
    if (unknown) {
        error_setg(errp, "IOC3 population has unknown bits 0x%x", unknown);
        return false;
    }
    if ((s->population_mask & SGI_IOC3_POPULATION_ONEWIRE_IDENTITY) &&
        !(s->population_mask & SGI_IOC3_POPULATION_ETHERNET)) {
        error_setg(errp, "IOC3 one-wire identity requires ethernet");
        return false;
    }

    population->eth = s->population_mask & SGI_IOC3_POPULATION_ETHERNET;
    population->serial[0] =
        s->population_mask & SGI_IOC3_POPULATION_SERIAL_A;
    population->serial[1] =
        s->population_mask & SGI_IOC3_POPULATION_SERIAL_B;
    population->kbc =
        s->population_mask & SGI_IOC3_POPULATION_KEYBOARD_MOUSE;
    population->parallel =
        s->population_mask & SGI_IOC3_POPULATION_PARALLEL;
    population->onewire_identity =
        s->population_mask & SGI_IOC3_POPULATION_ONEWIRE_IDENTITY;
    population->timekeeper = s->timekeeper_kind;
    return true;
}

static const hwaddr sgi_ioc3_serial_base[SGI_IOC3_SERIAL_PORTS] = {
    SGI_IOC3_SERIAL_A, SGI_IOC3_SERIAL_B,
};

static const char *const sgi_ioc3_serial_name[SGI_IOC3_SERIAL_PORTS] = {
    "sgi-ioc3.serial-a", "sgi-ioc3.serial-b",
};

static const char *const sgi_ioc3_sdma_alias_name[SGI_IOC3_SERIAL_PORTS] = {
    "sgi-ioc3.sdma-alias-a", "sgi-ioc3.sdma-alias-b",
};

static const hwaddr sgi_ioc3_sdma_alias_base[SGI_IOC3_SERIAL_PORTS] = {
    SGI_IOC3_SDMA_ALIAS_A, SGI_IOC3_SDMA_ALIAS_B,
};

static void sgi_ioc3_build(PCIDevice *dev, Error **errp)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    if ((s->pin_input_mask & ~0xffffu) ||
        (s->pin_input_values & ~s->pin_input_mask)) {
        error_setg(errp,
                   "GPIO inputs must fit the pin registers and input mask");
        return;
    }

    if (!sgi_ioc3_resolve_population(s, errp)) {
        return;
    }

    if (s->board->timekeeper != SGI_IOC3_TIMEKEEPER_DS1685 &&
        (s->ds1685.blk || s->ds1685.clock_blk)) {
        error_setg(errp, "DS1685 backing requires a board with a DS1685");
        return;
    }

    if (s->onewire_serial & ~SGI_IOC3_ONEWIRE_SERIAL_MASK) {
        error_setg(errp, "onewire-serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }

    /*
     * The board identity, over the class-wide default the PCI core has
     * already written. A guest reads the subsystem dword to decide which
     * sub-devices the board wires up, so it has to answer per instance.
     */
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, s->subsystem_id);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /* IRIX's configuration diagnostic fails the slot on a zero Status. */
    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);
    /*
     * Parity-error response lives in the Command half beside the enables the
     * PCI core already lets a guest write: io8bt writes 0x146 there and
     * expects it back.
     */
    pci_word_test_and_set_mask(dev->wmask + PCI_COMMAND, PCI_COMMAND_PARITY);
    /*
     * The latency timer is writable on this part; the PCI core leaves it
     * read-only by default, and io8bt writes 0xff00 over the cache-line-size
     * word and reads it back.
     */
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        /*
         * A child the board does not fit leaves the composition tree
         * entirely, rather than sitting in it unrealized: qdev asserts after
         * machine init that every device object it can reach is realized. Its
         * chardev-a/chardev-b alias goes first, same dangling-alias hazard as
         * the other conditionally-fitted children below.
         */
        if (!s->board->serial[i]) {
            object_property_del(OBJECT(dev),
                                i == 0 ? "chardev-a" : "chardev-b");
            object_unparent(OBJECT(serial));
            continue;
        }

        if (!qdev_realize(DEVICE(serial), NULL, errp)) {
            return;
        }

        s->serial_io_port[i].chip = s;
        s->serial_io_port[i].port = i;
        memory_region_init_io(&s->serial_io[i], OBJECT(dev),
                              &sgi_ioc3_serial_ops, &s->serial_io_port[i],
                              sgi_ioc3_serial_name[i], 8);
        memory_region_add_subregion(&s->bar, sgi_ioc3_serial_base[i],
                                    &s->serial_io[i]);

        serial->irq = qdev_get_gpio_in(DEVICE(dev), i);
        qdev_connect_gpio_out_named(
            DEVICE(serial), SERIAL_GPIO_TX_READY, 0,
            qdev_get_gpio_in_named(DEVICE(dev), "serial-tx-ready", i));
    }

    memory_region_init_io(&s->main, OBJECT(dev), &sgi_ioc3_main_ops, s,
                          "sgi-ioc3.main", SGI_IOC3_MAIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->main);

    /* All DMA masters share the chip's conversion into its PCI function. */
    memory_region_init_io(&s->pci_dma, OBJECT(dev), &sgi_ioc3_pci_dma_ops, s,
                          "sgi-ioc3.pci-dma", UINT64_MAX);
    s->pci_dma.disable_reentrancy_guard = true;
    s->dma_as = &s->dma_view_as;

    /*
     * The MicroLAN bus exists on every board; what varies is whether a part
     * hangs off it. Creating it unconditionally keeps one object graph and
     * one migration shape across board variants rather than two.
     *
     * It is not created so that an empty bus can answer: a board with no
     * ethernet carries no station-address part, and the slot handler answers
     * that case before it ever reaches the bus. The empty bus is never driven
     * from here.
     */
    s->onewire_bus = onewire_create_bus(DEVICE(dev), "onewire");

    if (s->board->eth) {
        object_property_set_link(OBJECT(&s->eth), "dma-memory",
                                 OBJECT(&s->pci_dma),
                                 &error_abort);
        bool configured = qemu_configure_nic_device(DEVICE(&s->eth), true,
                                                    NULL);

        if (s->board_mac) {
            MACAddr address;

            if (net_parse_macaddr(address.a, s->board_mac) < 0 ||
                !memcmp(address.a, "\0\0\0\0\0\0",
                        sizeof(address.a)) || (address.a[0] & 1)) {
                error_setg(errp, "IOC3 board MAC must be a unicast address");
                return;
            }
            if (configured && !s->eth.mac_generated &&
                memcmp(address.a, s->eth.conf.macaddr.a,
                       sizeof(address.a))) {
                error_setg(errp, "IOC3 NIC MAC conflicts with board identity");
                return;
            }
            s->eth.conf.macaddr = address;
            s->eth.mac_generated = false;
        }
        if (!qdev_realize(DEVICE(&s->eth), BUS(&s->ioc3_bus), errp)) {
            return;
        }
        memory_region_add_subregion(&s->bar, SGI_IOC3_ENET_BASE,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->eth), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 0,
                           qdev_get_gpio_in_named(DEVICE(dev), "eth-irq", 0));

    } else {
        /*
         * The instance-init aliases below reach into this child regardless of
         * population, so they must go before the child does: an alias left
         * standing over an unparented target is a dangling property, not an
         * absent one, and a deep introspection walk over a board that leaves
         * ethernet off reads through it into freed state.
         */
        object_property_del(OBJECT(dev), "netdev");
        object_property_del(OBJECT(dev), "mac");
        object_property_del(OBJECT(dev), "phy-addr");
        object_property_del(OBJECT(dev), "phy-dp83843");
        object_property_del(OBJECT(dev), "phy-ics1890");
        object_unparent(OBJECT(&s->eth));
    }

    if (s->board->onewire_identity) {
        uint8_t page[DS2502_PAGE_SIZE];
        DeviceState *part;

        /*
         * The station-address part, a DS2501: the 512-bit sibling of the
         * DS2502, which is what the family byte 0x91 (the custom bit over
         * base type 0x11) identifies. Its bytes use the fitted ethernet
         * address, so validation keeps that engine alongside the part.
         */
        sgi_ioc3_onewire_build_page(s, page);
        part = qdev_new(TYPE_ONEWIRE_DS2501);
        object_property_add_child(OBJECT(dev), "onewire-part", OBJECT(part));
        qdev_prop_set_uint8(part, "family", SGI_IOC3_ONEWIRE_FAMILY);
        qdev_prop_set_uint64(part, "serial", s->onewire_serial);
        ds2502_set_image(part, page, sizeof(page));
        if (!qdev_realize_and_unref(part, BUS(s->onewire_bus), errp)) {
            return;
        }
    }

    if (s->board_part || s->board_barcode || s->board_revision ||
        s->board_name) {
        const SGIBoardRecord record = {
            .barcode = s->board_barcode,
            .part = s->board_part,
            .revision = s->board_revision,
            .name = s->board_name,
        };

        /*
         * The board's own record, a second part on the same bus: a guest's
         * one-wire search finds both and tells them apart by family.
         */
        if (!sgi_board_record_attach(OBJECT(dev), "onewire-board-part",
                                     s->onewire_bus, &record,
                                     s->board_onewire_serial, "IOC3",
                                     errp)) {
            return;
        }
    }

    /* Chip register decode is independent of the board's external ports. */
    s->kbc_dev.ports_present = s->board->kbc;
    if (!qdev_realize(DEVICE(&s->kbc_dev), BUS(&s->ioc3_bus), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_KBC_BASE,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->kbc_dev), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->kbc_dev), 0,
                       qdev_get_gpio_in_named(DEVICE(dev), "kbc-irq", 0));

    /* On the chip's own bus, so a reset of the IOC3 reaches it. */
    if (s->board->parallel) {
        object_property_set_link(OBJECT(&s->parallel_dev), "dma-memory",
                                 OBJECT(&s->pci_dma),
                                 &error_abort);
        if (!qdev_realize(DEVICE(&s->parallel_dev), BUS(&s->ioc3_bus), errp)) {
            return;
        }
        memory_region_add_subregion(&s->bar, SGI_IOC3_PARALLEL_DMA_BASE,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->parallel_dev), 0));
        memory_region_add_subregion(&s->bar, SGI_IOC3_PARALLEL_ECP_BASE,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->parallel_dev), 1));
        for (i = 0; i < SGI_IOC3_PAR_IRQS; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->parallel_dev), i,
                               qdev_get_gpio_in_named(DEVICE(dev),
                                                      "parallel-irq", i));
        }
    } else {
        /* Same dangling-alias hazard as the ethernet block above. */
        object_property_del(OBJECT(dev), "parallel-chardev");
        object_unparent(OBJECT(&s->parallel_dev));
    }

    /*
     * The serial DMA engines belong to the ports: their walk drives a 16550
     * directly, so a board with no port has nothing behind these registers.
     */
    if (s->board->serial[0] || s->board->serial[1]) {
        memory_region_init_io(&s->sdma, OBJECT(dev), &sgi_ioc3_sdma_ops, s,
                              "sgi-ioc3.sdma", SGI_IOC3_SDMA_SIZE);
        memory_region_add_subregion(&s->bar, SGI_IOC3_SDMA_BASE, &s->sdma);
    }

    sgi_ioc_sdma_init(&s->sdma_engine, SGI_IOC3_SERIAL_PORTS,
                      &sgi_ioc3_sdma_params, &sgi_ioc3_sdma_engine_ops, s,
                      s->dma_as);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        s->sdma_alias_port[i].chip = s;
        s->sdma_alias_port[i].port = i;

        if (!s->board->serial[i]) {
            continue;
        }
        sgi_ioc_sdma_attach_uart(&s->sdma_engine, i, &s->serial[i]);

        memory_region_init_io(&s->sdma_alias[i], OBJECT(dev),
                              &sgi_ioc3_sdma_alias_ops,
                              &s->sdma_alias_port[i],
                              sgi_ioc3_sdma_alias_name[i],
                              SGI_IOC3_SDMA_ALIAS_SIZE);
        memory_region_add_subregion(&s->bar, sgi_ioc3_sdma_alias_base[i],
                                    &s->sdma_alias[i]);
    }

    /* The chip's own buffer RAM, which no board leaves off. */
    memory_region_init_io(&s->ssram, OBJECT(dev), &sgi_ioc3_ssram_ops, s,
                          "sgi-ioc3.ssram", SGI_IOC3_SSRAM_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SSRAM_BASE, &s->ssram);

    /*
     * On the chip's own bus, so a reset of the IOC3 reaches it. Whichever
     * clock this board does not fit is unparented rather than left realized
     * but unmapped, so it is absent from the object graph and from the
     * migration stream too. Remove the backend aliases before unparenting
     * their child, same as ethernet above.
     */
    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_M48T35) {
        if (!qdev_realize(DEVICE(&s->rtc), BUS(&s->ioc3_bus), errp)) {
            return;
        }
        memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV0,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->rtc), 0));
    } else {
        object_property_del(OBJECT(dev), "nvram-drive");
        object_property_del(OBJECT(dev), "nvram-clock-drive");
        object_unparent(OBJECT(&s->rtc));
    }

    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_DS1685) {
        if (!qdev_realize(DEVICE(&s->ds1685), BUS(&s->ioc3_bus), errp)) {
            return;
        }
        /*
         * Two selects, because the part is reached indirectly: a register
         * number goes to the first and the register itself answers on the
         * second.
         */
        memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV1,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->ds1685), 0));
        memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV2,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->ds1685), 1));
    } else {
        object_property_del(OBJECT(dev), "ds1685-drive");
        object_property_del(OBJECT(dev), "ds1685-clock-drive");
        object_unparent(OBJECT(&s->ds1685));
    }

    memory_region_init_io(&s->pci_bar, OBJECT(dev), &sgi_ioc3_pci_bar_ops, s,
                          "sgi-ioc3.pci-bar", SGI_IOC3_BAR_SIZE);
    s->pci_bar.disable_reentrancy_guard = true;
    address_space_init(&s->bar_as, &s->bar, "sgi-ioc3.registers");
    address_space_init(&s->dma_view_as, &s->pci_dma, "sgi-ioc3.dma");
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->pci_bar);

    /* The SuperIO line is board wiring, not the PCI pin. */
    qdev_init_gpio_out(DEVICE(dev), &s->superio_irq, 1);
    qdev_init_gpio_out_named(DEVICE(dev), &s->gpio5_reset_request,
                             "gpio5-reset-request", 1);
}

/*
 * An IOC3 added with -device realizes outside a board's build, and each
 * of its regions would commit on its own. Build it in one transaction.
 * The build realizes the serial ports, ethernet and timekeeper, maps
 * regions and wires IRQs; nothing in it accesses an address space.
 */
static void sgi_ioc3_realize(PCIDevice *dev, Error **errp)
{
    memory_region_transaction_begin();
    sgi_ioc3_build(dev, errp);
    memory_region_transaction_commit();
}

static void sgi_ioc3_exit(PCIDevice *dev)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    /*
     * Unrealize children or their reset handlers point at freed state. Only
     * the ones this board realized: unrealizing a device that never was
     * asserts.
     */
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        if (s->board->serial[i]) {
            qdev_unrealize(DEVICE(&s->serial[i]));
        }
    }
    sgi_ioc_sdma_finalize(&s->sdma_engine);
    timer_del(s->int_out_timer);
    if (s->board->eth) {
        qdev_unrealize(DEVICE(&s->eth));
    }
    qdev_unrealize(DEVICE(&s->kbc_dev));
    if (s->board->parallel) {
        qdev_unrealize(DEVICE(&s->parallel_dev));
    }
    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_M48T35) {
        qdev_unrealize(DEVICE(&s->rtc));
    }
    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_DS1685) {
        qdev_unrealize(DEVICE(&s->ds1685));
    }
    address_space_destroy(&s->dma_view_as);
    address_space_destroy(&s->bar_as);
}

static void sgi_ioc3_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    s->int_status = 0;
    s->int_enable = 0;
    sgi_ioc3_update_irq(s);

    sgi_ioc_sdma_reset(&s->sdma_engine);
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        /* Reset clears the rest of modem control, so it clears this too. */
        s->serial_mcr_autoflow[i] = 0;
        s->serial_lsr_test[i] = 0;
        s->serial_msr_test[i] = 0;
    }

    /* The buffer RAM is memory: reset does not clear it. */
    s->microlan_control = 0;
    s->pin_control = 0;
    s->pin_data = 0;
    sgi_ioc3_update_gpio5(s);
    s->chip_control = 0;
    s->int_out = 0;
    s->int_out_active = 0;
    s->int_out_state = false;
    s->int_out_pulse_end = false;
    s->int_out_square_started = false;
    timer_del(s->int_out_timer);
}

static void sgi_ioc3_init(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    qdev_init_gpio_in(DEVICE(obj), sgi_ioc3_serial_irq,
                      SGI_IOC3_SERIAL_PORTS);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_sdma_tx_ready,
                            "serial-tx-ready", SGI_IOC3_SERIAL_PORTS);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_eth_irq, "eth-irq", 1);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_kbc_irq, "kbc-irq", 1);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_parallel_irq, "parallel-irq",
                            SGI_IOC3_PAR_IRQS);

    s->int_out_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    sgi_ioc3_int_out_expire, s);

    qbus_init(&s->ioc3_bus, sizeof(s->ioc3_bus), TYPE_SGI_IOC3_BUS,
              DEVICE(obj), "ioc3-bus");

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        object_initialize_child(obj, "serial[*]", &s->serial[i], TYPE_SERIAL);
    }

    object_property_add_alias(obj, "chardev-a", OBJECT(&s->serial[0]),
                              "chardev");
    object_property_add_alias(obj, "chardev-b", OBJECT(&s->serial[1]),
                              "chardev");

    object_initialize_child(obj, "eth", &s->eth, TYPE_SGI_IOC3_ETH);

    object_property_add_alias(obj, "netdev", OBJECT(&s->eth), "netdev");
    object_property_add_alias(obj, "mac", OBJECT(&s->eth), "mac");
    object_property_add_alias(obj, "phy-addr", OBJECT(&s->eth), "phy-addr");
    object_property_add_alias(obj, "phy-ics1890", OBJECT(&s->eth),
                              "phy-ics1890");
    object_property_add_alias(obj, "phy-dp83843", OBJECT(&s->eth),
                              "phy-dp83843");

    object_initialize_child(obj, "kbc", &s->kbc_dev, TYPE_SGI_IOC3_KBC);

    object_initialize_child(obj, "parallel", &s->parallel_dev,
                            TYPE_SGI_IOC3_PARALLEL);

    object_property_add_alias(obj, "parallel-chardev",
                              OBJECT(&s->parallel_dev), "chardev");

    /*
     * Both clocks are initialized; realize keeps whichever this board fits
     * and unparents the other. A subsystem identity is a property, so which
     * one is wanted is not known until then.
     */
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_M48T35);
    object_initialize_child(obj, "ds1685", &s->ds1685, TYPE_DS1685);

    object_property_add_alias(obj, "nvram-drive", OBJECT(&s->rtc), "drive");
    object_property_add_alias(obj, "nvram-clock-drive",
                              OBJECT(&s->rtc), "clock-drive");
    object_property_add_alias(obj, "ds1685-drive",
                              OBJECT(&s->ds1685), "drive");
    object_property_add_alias(obj, "ds1685-clock-drive",
                              OBJECT(&s->ds1685), "clock-drive");
}

static void sgi_ioc3_finalize(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);

    timer_free(s->int_out_timer);
}

/* An absent port has no timer for vmstate_serial's subsection test. */
static bool sgi_ioc3_serial_a_needed(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;

    return s->board->serial[0];
}

static bool sgi_ioc3_serial_b_needed(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;

    return s->board->serial[1];
}

static int sgi_ioc3_post_load(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;
    unsigned int i;

    sgi_ioc3_update_irq(s);
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        sgi_ioc_sdma_kick(&s->sdma_engine, i);
        /*
         * baudbase is a realize-time property of the embedded SerialState,
         * not a migrated field, so vmstate_serial's own post_load (which
         * already ran, nested inside the field load above) computed the
         * port's baud rate from the 16550 core's stock default rather than
         * from the pre-divider this chip overloads the scratch register with.
         * Rederive it now that serial_predivider[] has loaded.
         * sgi_ioc3_serial_program_predivider() already treats 0 or 1 (never
         * programmed) as "leave it alone".
         */
        sgi_ioc3_serial_program_predivider(s, i, s->serial_predivider[i]);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 22,
    .minimum_version_id = 22,
    .post_load = sgi_ioc3_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(pin_input_mask, SGIIOC3State),
        VMSTATE_UINT32_EQUAL(pin_input_values, SGIIOC3State),
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC3State),
        VMSTATE_UINT32(int_status, SGIIOC3State),
        VMSTATE_UINT32(int_enable, SGIIOC3State),
        VMSTATE_STRUCT_TEST(serial[0], SGIIOC3State,
                            sgi_ioc3_serial_a_needed, 0,
                            vmstate_serial, SerialState),
        VMSTATE_STRUCT_TEST(serial[1], SGIIOC3State,
                            sgi_ioc3_serial_b_needed, 0,
                            vmstate_serial, SerialState),
        VMSTATE_UINT8_ARRAY(serial_predivider, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT8_ARRAY(serial_mcr_autoflow, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT8_ARRAY(serial_lsr_test, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT8_ARRAY(serial_msr_test, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT32(microlan_control, SGIIOC3State),
        VMSTATE_UINT32(pin_control, SGIIOC3State),
        VMSTATE_UINT32(pin_data, SGIIOC3State),
        VMSTATE_SGI_IOC_SDMA(sdma_engine, SGIIOC3State),
        VMSTATE_UINT32(chip_control, SGIIOC3State),
        VMSTATE_UINT32(int_out, SGIIOC3State),
        VMSTATE_UINT32(int_out_active, SGIIOC3State),
        VMSTATE_BOOL(int_out_state, SGIIOC3State),
        VMSTATE_BOOL(int_out_pulse_end, SGIIOC3State),
        VMSTATE_BOOL(int_out_square_started, SGIIOC3State),
        VMSTATE_TIMER_PTR(int_out_timer, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(ssram_word, SGIIOC3State, SGI_IOC3_SSRAM_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_ioc3_properties[] = {
    DEFINE_PROP_UINT32("gpio-input-mask", SGIIOC3State, pin_input_mask, 0),
    DEFINE_PROP_UINT32("gpio-input-values", SGIIOC3State, pin_input_values, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIIOC3State, onewire_serial,
                       SGI_IOC3_ONEWIRE_SERIAL),
    DEFINE_PROP_UINT16("subsystem-id", SGIIOC3State, subsystem_id,
                       SGI_IOC3_SUBSYSTEM_ID),
    DEFINE_PROP_UINT32("population", SGIIOC3State, population_mask,
                       SGI_IOC3_POPULATION_AUTO),
    DEFINE_PROP_UINT8("timekeeper-kind", SGIIOC3State, timekeeper_kind,
                      SGI_IOC3_TIMEKEEPER_NONE),
    DEFINE_PROP_CHR("record-a", SGIIOC3State, serial_record[0]),
    DEFINE_PROP_CHR("record-b", SGIIOC3State, serial_record[1]),
    DEFINE_PROP_STRING("board-part", SGIIOC3State, board_part),
    DEFINE_PROP_STRING("board-barcode", SGIIOC3State, board_barcode),
    DEFINE_PROP_STRING("board-revision", SGIIOC3State, board_revision),
    DEFINE_PROP_STRING("board-name", SGIIOC3State, board_name),
    DEFINE_PROP_STRING("board-mac", SGIIOC3State, board_mac),
    DEFINE_PROP_UINT64("board-onewire-serial", SGIIOC3State,
                       board_onewire_serial, SGI_IOC3_BOARD_ONEWIRE_SERIAL),
};

static void sgi_ioc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 base I/O controller";

    pc->realize = sgi_ioc3_realize;
    pc->exit = sgi_ioc3_exit;
    pc->vendor_id = SGI_IOC3_VENDOR_ID;
    pc->device_id = SGI_IOC3_DEVICE_ID;
    pc->revision = SGI_IOC3_REVISION;
    pc->class_id = SGI_IOC3_CLASS_ID;
    pc->subsystem_vendor_id = SGI_IOC3_VENDOR_ID;
    pc->subsystem_id = SGI_IOC3_SUBSYSTEM_ID;
    rc->phases.hold = sgi_ioc3_reset_hold;
    dc->vmsd = &vmstate_sgi_ioc3;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, sgi_ioc3_properties);
}

static const TypeInfo sgi_ioc3_types[] = {
    {
        .name = TYPE_SGI_IOC3,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIIOC3State),
        .instance_init = sgi_ioc3_init,
        .instance_finalize = sgi_ioc3_finalize,
        .class_init = sgi_ioc3_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
    {
        .name = TYPE_SGI_IOC3_BUS,
        .parent = TYPE_SYSTEM_BUS,
        .instance_size = sizeof(SGIIOC3BusState),
    },
};

DEFINE_TYPES(sgi_ioc3_types)
