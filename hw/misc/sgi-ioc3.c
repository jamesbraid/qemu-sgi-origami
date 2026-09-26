/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 *
 * One PCI device, one BAR: two 16550s and their DMA engines, the ethernet
 * block and its buffer RAM, PS/2, a one-wire ethernet-address part, and
 * the byte bus's timekeeper. See docs/specs/sgi-ioc3.rst.
 *
 * An add-in card can also keep its board-identity record on the MicroLAN,
 * beside the station-address part; the "board-*" properties fit it.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "hw/misc/sgi-board-record.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "net/net.h"
#include "trace.h"

static void sgi_ioc3_update_irq(SGIIOC3State *s)
{
    qemu_set_irq(s->superio_irq, !!(s->int_status & s->int_enable));
}

static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port);
static void sgi_ioc3_sdma_rx_timer_catch_up(SGIIOC3State *s);

static void sgi_ioc3_serial_irq(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = port ? SGI_IOC3_INT_SERIAL_B : SGI_IOC3_INT_SERIAL_A;

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);

    /*
     * On every level, not assertion alone: IRIX's DMA-ring console
     * receives with the 16550 interrupts disabled entirely.
     */
    sgi_ioc3_sdma_run(s, port);
}

static void sgi_ioc3_sdma_tx_ready(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;

    s->sdma_tx_ready[port] = level;
    if (level) {
        sgi_ioc3_sdma_run(s, port);
    }
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
 * interrupt this chip would never raise again. The three below it -- one per
 * DMA context, then the DMA fault -- latch on the port's pulse and the
 * driver clears each by writing its bit back.
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
 * address are the record's own format, and so is the checksum at bytes 11
 * and 12, which a DS2502-class part never computes -- an embedded page
 * checksum is a software file-structure convention, while the part's own
 * CRCs always cover a whole page. It is stored inverted so a guest shifting
 * the record and its checksum through the generator lands on the fixed
 * residue every one-wire CRC-16 stream ends on.
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
         * on its own. Either way no part answers: the wire reads high and
         * the slot advances no conversation.
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
 * mode produced it (docs/datasheets/ioc3.md); the mask only decides the pin.
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
 * One transition due at @at. The next is scheduled from @at rather than
 * from whenever this runs, so a late transition keeps the output's phase.
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
 * next services the timer: the IO-8 board test samples a periodic output in
 * a tight loop and needs both levels.
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
        sgi_ioc3_sdma_rx_timer_catch_up(s);
        return s->int_status;
    case SGI_IOC3_INT_ENABLE_SET:
        /* The set alias reads back the live mask; the clear alias does not. */
        return s->int_enable;
    case SGI_IOC3_CHIP_CONTROL:
        /* Bit 22: arbiter idle, polled by the kernel's console setup. */
        return s->chip_control | SGI_IOC3_CHIP_CTRL_ARB_IDLE;
    case SGI_IOC3_INT_OUTPUT:
        sgi_ioc3_int_out_catch_up(s);
        return s->int_out |
               (s->int_out_state ? SGI_IOC3_INT_OUT_STATE : 0);
    case SGI_IOC3_MICROLAN_CONTROL:
        return s->microlan_control;
    case SGI_IOC3_PIN_CONTROL_SET:
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
        /* An acknowledgement must not be undone by a late expiry. */
        sgi_ioc3_sdma_rx_timer_catch_up(s);
        /* Write-one-to-clear; the level-driven bits ignore the write. */
        s->int_status &= ~(val & ~(SGI_IOC3_INT_PASSTHROUGH |
                                   SGI_IOC3_INT_PARALLEL_SERVICE |
                                   SGI_IOC3_INT_RX_HIGH_WATER_LEVEL));
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
        s->chip_control = val & SGI_IOC3_CHIP_CTRL_MASK;
        sgi_ioc3_sdma_run(s, 0);
        sgi_ioc3_sdma_run(s, 1);
        break;
    case SGI_IOC3_INT_OUTPUT: {
        uint32_t int_out = val & SGI_IOC3_INT_OUT_MASK;

        /*
         * Stored as written, as the IO-8 board test requires; a command
         * with no defined behaviour leaves the running output alone.
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
         * A fixed level is there for the next read, which the IO-8 board
         * test checks; a timed mode starts after the MMIO write returns.
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
 * docs/datasheets/ioc3.md, "IOC3 overloads the scratch register": while a
 * port's divisor latch is selected, a write to its scratch register (offset
 * 7) programs the 16550's clock pre-divider instead of storing a scratch
 * byte. The value written is twice the divider wanted -- the PROM and IRIX
 * write 6, for a divide of 3 -- and the reference clock the ordinary
 * divisor latches then work from is the crystal divided by that divider.
 *
 * The datasheet states the header's crystal as 22,000,000 Hz, but argues,
 * as an inference rather than a stated fact, that the real part is a
 * 22.1184 MHz crystal: SGI's manual calls these 460,800-baud ports, and
 * 22.1184 MHz / 3 / 16 lands exactly on 460,800 and on the standard rates
 * below it (divisor 48 gives exactly 9600), where the header's rounded
 * figure is half a percent off. A guest that only ever reads back its own
 * writes cannot tell the two figures apart; it matters here because the
 * emulated baud rate has to line up with a real host serial port.
 */
#define SGI_IOC3_UART_CRYSTAL_HZ 22118400

/*
 * Offset 7 always stores into the 16550 core's own scratch byte first (see
 * the caller), so a guest reads back exactly what it wrote whether or not
 * the divisor latch is selected. This is the overload on top of that: while
 * it is selected, also derive the reference clock the port's baud rate is
 * computed from. A written 0 or 1 would divide by zero; the datasheet
 * describes no meaning for either, and no capture shows one written, so
 * they are read as "leave the reference clock alone".
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
     * pre-divider written after the divisor already latched takes effect
     * at once, rather than waiting on whatever next touches either
     * register.
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

/*
 * docs/datasheets/ioc3.md, "Clear-to-send in loopback under hardware flow
 * control": looped back with the engine's hardware flow control on, the
 * port reports clear-to-send asserted even with request-to-send clear. The
 * board test observes the level, not the mechanism behind it, so this
 * supplies the level alone and leaves the modem-control register as
 * software wrote it.
 */
static bool sgi_ioc3_serial_flow_cts(SGIIOC3State *s, unsigned int port)
{
    hwaddr control = SGI_IOC3_SDMA_PORT_A + port * SGI_IOC3_SDMA_PORT_STRIDE +
                     SGI_IOC3_SDMA_CONTROL;

    return (s->serial[port].mcr & UART_MCR_LOOP) &&
           (s->sdma_reg[(control - SGI_IOC3_SDMA_BASE) / 4] &
            SGI_IOC3_SDMA_FLOW_CONTROL);
}

/* Byte-wide only; both guests place their own byte lanes -- add none here. */
static uint64_t sgi_ioc3_serial_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3SerialAlias *handle = opaque;
    uint64_t val = serial_io_ops.read(&handle->chip->serial[handle->port],
                                      addr, 1);

    if (addr == UART_MCR) {
        val |= handle->chip->serial_mcr_autoflow[handle->port];
    }

    /*
     * A factory-test write shows up in the read that follows it and no
     * later one: reading line status is what clears the error bits it
     * names, so nothing here outlives its own read.
     */
    if (addr == UART_LSR) {
        val |= handle->chip->serial_lsr_test[handle->port];
        handle->chip->serial_lsr_test[handle->port] = 0;
    }

    /* Modem status clears its change indicators on a read the same way. */
    if (addr == UART_MSR) {
        val |= handle->chip->serial_msr_test[handle->port];
        handle->chip->serial_msr_test[handle->port] = 0;
        if (sgi_ioc3_serial_flow_cts(handle->chip, handle->port)) {
            val |= UART_MSR_CTS;
        }
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

/* The selected ring size is also every index register's pointer width. */
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

/* The size-select bit is not address: unmasked, every ring is a byte off. */
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
 * OR into the live register: a loopback transmit walk re-enters the
 * receiver, and writing back a pre-walk snapshot would lose its sources.
 */
static void sgi_ioc3_raise_serial_source(SGIIOC3State *s, unsigned int port,
                                         unsigned int source)
{
    s->int_status |= SGI_IOC3_SERIAL_SOURCE(port, source);
    sgi_ioc3_update_irq(s);
}

static void sgi_ioc3_set_serial_source(SGIIOC3State *s, unsigned int port,
                                       unsigned int source, bool level)
{
    uint32_t bit = SGI_IOC3_SERIAL_SOURCE(port, source);

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);
}

static bool sgi_ioc3_sdma_running(SGIIOC3State *s, unsigned int port)
{
    uint32_t control = sgi_ioc3_sdma_control(s, port);

    return (control & SGI_IOC3_SDMA_ENABLE) &&
           !(control & SGI_IOC3_SDMA_PAUSE);
}

/* Post the packer as one entry; unfilled slots carry no validity flags. */
static bool sgi_ioc3_sdma_post(SGIIOC3State *s, unsigned int port,
                               uint32_t *produce, uint32_t *consume,
                               uint32_t mask)
{
    uint64_t ring = sgi_ioc3_sdma_ring(s, port, SGI_IOC3_SDMA_RING_RX);
    uint32_t next = (*produce + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;

    /* Full: refuse; the characters keep in the packer and the FIFO. */
    if (next == (*consume & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_RING_FULL);
        return false;
    }

    /* A refused write must not advance the index past a missing entry. */
    if (dma_memory_write(s->dma_as, ring + (*produce & mask),
                         s->sdma_packer[port], SGI_IOC3_SDMA_ENTRY_SIZE,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_PCI_ERROR);
        return false;
    }
    *produce = next;
    s->sdma_packed[port] = 0;
    memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
    s->sdma_delivered[port] = true;
    return true;
}

/* The reload is receive idleness, in 16 kHz ticks from the last delivery. */
static void sgi_ioc3_sdma_rx_timer_start(SGIIOC3State *s, unsigned int port)
{
    uint32_t reload = *sgi_ioc3_sdma_port_reg(s, port, SGI_IOC3_SDMA_RX_TIMER)
                      & SGI_IOC3_SDMA_TIMER_RELOAD;

    /* Once it has run, the count it held from the reload write is spent. */
    s->sdma_rx_idle_count[port] = 0;
    timer_mod(s->sdma_rx_timer[port],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)reload * SGI_IOC3_SDMA_TIMER_TICK_NS);
}

/* A drained ring expires into silence. */
static void sgi_ioc3_sdma_rx_timer_expired(void *opaque)
{
    SGIIOC3SerialAlias *handle = opaque;
    SGIIOC3State *s = handle->chip;
    unsigned int port = handle->port;
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);

    s->sdma_rx_idle_count[port] = 0;

    /* A still-packed entry means the ring was full; retry now. */
    if (s->sdma_packed[port] &&
        sgi_ioc3_sdma_post(s, port, produce, consume, mask)) {
        s->sdma_delivered[port] = false;
    }

    if ((*consume & SGI_IOC3_SDMA_ARM_RX_TIMER) &&
        ((*produce ^ *consume) & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_TIMER);
    }
}

/*
 * An expiry already due happens before an access, not whenever the main
 * loop next services the timer. The count field is read off the clock, so
 * without this a guest can read a spent count while the source it raises
 * is still clear: the IO-8 board test loads a two-tick reload, waits 10 ms
 * and requires the source set (docs/datasheets/ioc3.md).
 */
static void sgi_ioc3_sdma_rx_timer_catch_up(SGIIOC3State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned int port;

    for (port = 0; port < SGI_IOC3_SERIAL_PORTS; port++) {
        QEMUTimer *timer = s->sdma_rx_timer[port];

        if (timer_pending(timer) && timer_expire_time_ns(timer) <= now) {
            timer_del(timer);
            sgi_ioc3_sdma_rx_timer_expired(&s->sdma_alias_port[port]);
        }
    }
}

/*
 * A level, recomputed wherever the waiting count moves: a latched raise
 * storms IRIX's ring console. The mark counts characters, not entries.
 */
static void sgi_ioc3_sdma_update_high_water(SGIIOC3State *s, unsigned int port)
{
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);
    uint32_t high_water = sgi_ioc3_sdma_control(s, port) &
                          SGI_IOC3_SDMA_HIGH_WATER;
    uint32_t waiting = ((*produce - *consume) & mask) /
                       SGI_IOC3_SDMA_ENTRY_SIZE * SGI_IOC3_SDMA_ENTRY_CHARS;

    sgi_ioc3_set_serial_source(s, port, SGI_IOC3_INT_RX_HIGH_WATER,
                               high_water && waiting >= high_water);
}

/*
 * End of burst: a short group posts now rather than on timer expiry; the
 * interrupt still waits for the timer, the timing drivers observe.
 */
static void sgi_ioc3_sdma_flush(SGIIOC3State *s, unsigned int port)
{
    uint32_t *produce = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_PRODUCE);
    uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port,
                                               SGI_IOC3_SDMA_RX_CONSUME);
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);

    if (s->sdma_packed[port]) {
        sgi_ioc3_sdma_post(s, port, produce, consume, mask);
    }

    sgi_ioc3_sdma_update_high_water(s, port);

    if (!s->sdma_delivered[port]) {
        return;
    }
    s->sdma_delivered[port] = false;

    if (*consume & SGI_IOC3_SDMA_ARM_RX_TIMER) {
        sgi_ioc3_sdma_rx_timer_start(s, port);
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
        uint8_t lsr;
        uint8_t msr;
        uint8_t status;

        /* On refusal the drain stops without dropping anything. */
        if (s->sdma_packed[port] == SGI_IOC3_SDMA_ENTRY_CHARS &&
            !sgi_ioc3_sdma_post(s, port, produce, consume, mask)) {
            break;
        }

        lsr = serial_io_ops.read(serial, UART_LSR, 1);
        if (!(lsr & UART_LSR_DR)) {
            break;
        }

        /* LSR before RBR: reading LSR clears the latches it describes. */
        s->sdma_packer[port][s->sdma_packed[port]] =
            serial_io_ops.read(serial, UART_RBR_THR, 1);
        msr = serial_io_ops.read(serial, UART_MSR, 1);
        if (sgi_ioc3_serial_flow_cts(s, port)) {
            msr |= UART_MSR_CTS;
        }

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

        ++s->sdma_packed[port];
    }
}

/*
 * Each control byte types its data byte; the empty type ends the entry,
 * and sending past it puts memory garbage on the wire.
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

        /* A refused fetch stops the walk: the stack bytes are not an entry. */
        if (dma_memory_read(s->dma_as, ring + (*consume & mask), entry,
                            sizeof(entry), MEMTXATTRS_UNSPECIFIED) !=
            MEMTX_OK) {
            sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_PCI_ERROR);
            break;
        }

        for (i = s->sdma_tx_slot[port];
             i < SGI_IOC3_SDMA_ENTRY_CHARS; i++) {
            uint8_t control = entry[SGI_IOC3_SDMA_ENTRY_CHARS + i];
            uint8_t type = control & SGI_IOC3_SDMA_TX_TYPE;

            /* Wherever it sits: the slots behind it are not sent. */
            if (type == SGI_IOC3_SDMA_TX_NOTHING) {
                break;
            }

            switch (type) {
            case SGI_IOC3_SDMA_TX_CHARACTER:
                if (!s->sdma_tx_ready[port]) {
                    return;
                }
                serial_io_ops.write(serial, UART_RBR_THR, entry[i], 1);
                sgi_ioc3_serial_record(s, port, entry[i]);
                /* Drain per character or loopback overruns the 16-byte FIFO. */
                sgi_ioc3_sdma_receive(s, port);
                break;
            case SGI_IOC3_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC3_SDMA_TX_DELAY:
                break;
            }

            s->sdma_tx_slot[port] = i + 1;

            if (control & SGI_IOC3_SDMA_TX_INTERRUPT) {
                sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_DONE);
            }
        }

        s->sdma_tx_slot[port] = 0;
        *consume = (*consume + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;
        walked = true;
    }

    /* On reaching the end of a walk, not on every idle engine. */
    if (walked && (*consume & mask) == (*produce & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_RING_EMPTY);
    }
}

/*
 * Chip control places each port's 16550 for its DMA engine
 * (docs/datasheets/ioc3.md). Aimed anywhere else, the engine does not reach
 * the port; what it reaches instead is not modelled.
 */
static bool sgi_ioc3_sdma_aimed(SGIIOC3State *s, unsigned int port)
{
    static const hwaddr uart[SGI_IOC3_SERIAL_PORTS] = {
        SGI_IOC3_SERIAL_A, SGI_IOC3_SERIAL_B,
    };
    unsigned int shift = port ? SGI_IOC3_CHIP_CTRL_PORT_B_SHIFT
                              : SGI_IOC3_CHIP_CTRL_PORT_A_SHIFT;
    uint32_t field = (s->chip_control >> shift) &
                     SGI_IOC3_CHIP_CTRL_PORT_FIELD;

    return field == (uart[port] & 0xfff) / 8;
}

static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port)
{
    /* The walk drives a 16550 the board may not have fitted. */
    if (!s->board->serial[port]) {
        return;
    }

    if (!sgi_ioc3_sdma_aimed(s, port)) {
        return;
    }

    /* Loopback re-enters; a second walk would post a half-filled entry. */
    if (s->sdma_busy[port]) {
        return;
    }
    s->sdma_busy[port] = true;

    sgi_ioc3_sdma_transmit(s, port);
    sgi_ioc3_sdma_receive(s, port);
    sgi_ioc3_sdma_flush(s, port);

    s->sdma_busy[port] = false;
}

/* Resets the four indices and nothing else; the settings survive. */
static void sgi_ioc3_sdma_reset_port(SGIIOC3State *s, unsigned int port)
{
    unsigned int reg;

    for (reg = SGI_IOC3_SDMA_TX_PRODUCE; reg <= SGI_IOC3_SDMA_RX_CONSUME;
         reg += 4) {
        *sgi_ioc3_sdma_port_reg(s, port, reg) = 0;
    }

    s->sdma_packed[port] = 0;
    memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
    s->sdma_delivered[port] = false;
    s->sdma_tx_slot[port] = 0;

    timer_del(s->sdma_rx_timer[port]);
    s->sdma_rx_idle_count[port] = 0;
}

/*
 * A passive mirror read from the register file's fields: the byte-wide
 * read ops clear the latches and this view must not.
 */
static uint32_t sgi_ioc3_sdma_shadow(SGIIOC3State *s, unsigned int port)
{
    SerialState *serial = &s->serial[port];
    uint8_t lsr = serial->lsr;
    uint8_t mcr = serial->mcr;
    uint8_t msr;
    uint32_t shadow;

    /* In loopback the modem-control outputs wire back to status. */
    if (mcr & UART_MCR_LOOP) {
        msr = ((mcr & 0x0c) << 4) | ((mcr & 0x02) << 3) | ((mcr & 0x01) << 5);
    } else {
        msr = serial->msr;
    }
    if (sgi_ioc3_serial_flow_cts(s, port)) {
        msr |= UART_MSR_CTS;
    }

    shadow = lsr & SGI_IOC3_SHADOW_LSR_MASK;

    if (msr & UART_MSR_DCTS) {
        shadow |= SGI_IOC3_SHADOW_DELTA_CTS;
    }
    if (msr & UART_MSR_DDCD) {
        shadow |= SGI_IOC3_SHADOW_DELTA_CD;
    }
    if (msr & UART_MSR_CTS) {
        shadow |= SGI_IOC3_SHADOW_CTS;
    }
    if (msr & UART_MSR_DCD) {
        shadow |= SGI_IOC3_SHADOW_CD;
    }
    if (mcr & UART_MCR_DTR) {
        shadow |= SGI_IOC3_SHADOW_DTR;
    }
    if (mcr & UART_MCR_RTS) {
        shadow |= SGI_IOC3_SHADOW_RTS;
    }
    if (mcr & UART_MCR_OUT1) {
        shadow |= SGI_IOC3_SHADOW_OUT1;
    }
    if (mcr & UART_MCR_OUT2) {
        shadow |= SGI_IOC3_SHADOW_OUT2;
    }
    if (mcr & UART_MCR_LOOP) {
        shadow |= SGI_IOC3_SHADOW_LOOPBACK;
    }
    return shadow;
}

static uint32_t sgi_ioc3_sdma_read_port(SGIIOC3State *s, unsigned int port,
                                        hwaddr offset)
{
    uint32_t val;

    sgi_ioc3_sdma_rx_timer_catch_up(s);

    if (offset == SGI_IOC3_SDMA_SHADOW) {
        return sgi_ioc3_sdma_shadow(s, port);
    }

    val = *sgi_ioc3_sdma_port_reg(s, port, offset);

    if (offset == SGI_IOC3_SDMA_CONTROL && (val & SGI_IOC3_SDMA_PAUSE)) {
        val |= SGI_IOC3_SDMA_PAUSED;
    }

    /* The produce index also reports the packer's held count. */
    if (offset == SGI_IOC3_SDMA_RX_PRODUCE) {
        val |= (uint32_t)s->sdma_packed[port] << SGI_IOC3_SDMA_HELD_SHIFT;
    }

    /*
     * Ticks left while running. Idle, the count is whatever a reload write
     * last loaded, until the timer runs or the channel resets.
     */
    if (offset == SGI_IOC3_SDMA_RX_TIMER) {
        uint32_t ticks = s->sdma_rx_idle_count[port];

        if (timer_pending(s->sdma_rx_timer[port])) {
            int64_t left = timer_expire_time_ns(s->sdma_rx_timer[port]) -
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            ticks = MIN(MAX(left, 0) / SGI_IOC3_SDMA_TIMER_TICK_NS,
                        SGI_IOC3_SDMA_TIMER_RELOAD);
        }
        val |= ticks << SGI_IOC3_SDMA_TIMER_COUNT_SHIFT;
    }

    return val;
}

static void sgi_ioc3_sdma_write_port(SGIIOC3State *s, unsigned int port,
                                     hwaddr offset, uint32_t val)
{
    uint32_t mask = sgi_ioc3_sdma_pointer_mask(s);

    /* A write that stops or rewinds the timer comes after an expiry due. */
    sgi_ioc3_sdma_rx_timer_catch_up(s);

    switch (offset) {
    case SGI_IOC3_SDMA_CONTROL:
        *sgi_ioc3_sdma_port_reg(s, port, offset) =
            val & ~SGI_IOC3_SDMA_CTRL_UNSTORED;
        if (val & SGI_IOC3_SDMA_RESET) {
            sgi_ioc3_sdma_reset_port(s, port);
        }
        if (!(val & SGI_IOC3_SDMA_ENABLE)) {
            timer_del(s->sdma_rx_timer[port]);
        }
        /*
         * The drain bit is a command IRIX polls to zero, and enabling is a
         * start: the firmware hands the ring over before enabling.
         */
        sgi_ioc3_sdma_run(s, port);
        return;

    case SGI_IOC3_SDMA_TX_PRODUCE:
        *sgi_ioc3_sdma_port_reg(s, port, offset) = val & mask;
        sgi_ioc3_sdma_run(s, port);
        return;

    case SGI_IOC3_SDMA_RX_CONSUME: {
        uint32_t *consume = sgi_ioc3_sdma_port_reg(s, port, offset);
        uint32_t *produce;

        *consume = val & (mask | SGI_IOC3_SDMA_ARM_RX_TIMER);
        sgi_ioc3_sdma_run(s, port);

        /* Bit 31 arms the timer; re-arming over a non-empty ring rewinds. */
        produce = sgi_ioc3_sdma_port_reg(s, port, SGI_IOC3_SDMA_RX_PRODUCE);
        if (!(*consume & SGI_IOC3_SDMA_ARM_RX_TIMER)) {
            timer_del(s->sdma_rx_timer[port]);
        } else if ((*produce ^ *consume) & mask) {
            sgi_ioc3_sdma_rx_timer_start(s, port);
        }
        return;
    }

    case SGI_IOC3_SDMA_RX_TIMER:
        *sgi_ioc3_sdma_port_reg(s, port, offset) =
            val & SGI_IOC3_SDMA_TIMER_RELOAD;
        /* The write loads the count as well; a running timer keeps its own. */
        s->sdma_rx_idle_count[port] = val & SGI_IOC3_SDMA_TIMER_RELOAD;
        return;

    case SGI_IOC3_SDMA_SHADOW:
        /* Discarded silently: OpenBSD writes zero here on every boot. */
        return;

    /*
     * The engine advances these two, and software can still place them:
     * the board test writes each and reads it back (docs/datasheets/ioc3.md).
     * Only the pointer field stores, so a write cannot forge the held
     * counts the engine reports in the bits above it.
     */
    case SGI_IOC3_SDMA_TX_CONSUME:
    case SGI_IOC3_SDMA_RX_PRODUCE:
        *sgi_ioc3_sdma_port_reg(s, port, offset) = val & mask;
        sgi_ioc3_sdma_run(s, port);
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

    if (addr >= SGI_IOC3_SDMA_PORT_REGS * 4) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: read of serial DMA alias offset 0x%"
                      HWADDR_PRIx " which is past the seven registers a page "
                      "exposes\n", addr);
        return 0;
    }

    return sgi_ioc3_sdma_read_port(alias->chip, alias->port, addr);
}

static void sgi_ioc3_sdma_alias_write(void *opaque, hwaddr addr, uint64_t val,
                                      unsigned size)
{
    SGIIOC3SerialAlias *alias = opaque;

    if (addr >= SGI_IOC3_SDMA_PORT_REGS * 4) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc3: write of 0x%08x to serial DMA alias offset 0x%"
                      HWADDR_PRIx " which is past the seven registers a page "
                      "exposes\n", (uint32_t)val, addr);
        return;
    }

    /*
     * Three of the seven are read-only through a page, which is the whole
     * point of exposing one to a user process: the two indexes the engine
     * advances and the 16550 shadow (docs/datasheets/ioc3.md). The main
     * block takes writes to the first two.
     */
    if (addr == SGI_IOC3_SDMA_TX_CONSUME ||
        addr == SGI_IOC3_SDMA_RX_PRODUCE ||
        addr == SGI_IOC3_SDMA_SHADOW) {
        trace_sgi_ioc3_sdma_write_refused(
            (uint32_t)(SGI_IOC3_SDMA_ALIAS_A +
                       alias->port * (SGI_IOC3_SDMA_ALIAS_B -
                                      SGI_IOC3_SDMA_ALIAS_A) + addr),
            (uint32_t)val);
        return;
    }

    sgi_ioc3_sdma_write_port(alias->chip, alias->port, addr, val);
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

    s->ssram_word[addr / 4] = val & SGI_IOC3_SSRAM_STORED;
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
 * reads out of configuration space. Taken from mainline Linux's per-board
 * setup routines (drivers/mfd/ioc3.c, v7.2-rc7, commit db2ddb8), which are
 * the clearest published statement of how much board-to-board variation this
 * chip sees. The IP29 System Board and the IP27 BaseIO6G share one routine
 * there and so share a row here.
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
        .onewire_identity = true,
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

static void sgi_ioc3_realize(PCIDevice *dev, Error **errp)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    if (!sgi_ioc3_resolve_population(s, errp)) {
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
     * Parity-error response lives in the Command half beside the enables
     * the PCI core already lets a guest write (docs/datasheets/ioc3.md):
     * io8bt writes 0x146 there and expects it back.
     */
    pci_word_test_and_set_mask(dev->wmask + PCI_COMMAND, PCI_COMMAND_PARITY);
    /*
     * The latency timer is writable on this part (docs/datasheets/ioc3.md);
     * the PCI core leaves it read-only by default, and io8bt writes 0xff00
     * over the cache-line-size word and reads it back.
     */
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        /*
         * A child the board does not fit leaves the composition tree
         * entirely, rather than sitting in it unrealized: qdev asserts after
         * machine init that every device object it can reach is realized.
         * Its chardev-a/chardev-b alias goes first, same dangling-alias
         * hazard as the other conditionally-fitted children below.
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

    /* All child DMA goes through this function's bus-master view. */
    s->dma_as = pci_get_address_space(dev);

    /*
     * The MicroLAN bus exists on every board; what varies is whether a part
     * hangs off it. Creating it unconditionally keeps one object graph and
     * one migration shape across board variants rather than two.
     *
     * It is not created so that an empty bus can answer: a board with no
     * ethernet carries no station-address part, and the slot handler
     * answers that case before it ever reaches the bus. The empty bus is
     * never driven from here.
     */
    s->onewire_bus = onewire_create_bus(DEVICE(dev), "onewire");

    if (s->board->eth) {
        object_property_set_link(OBJECT(&s->eth), "dma-memory",
                                 OBJECT(&dev->bus_master_container_region),
                                 &error_abort);
        qemu_configure_nic_device(DEVICE(&s->eth), true, NULL);
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
         * The instance-init aliases below reach into this child regardless
         * of population, so they must go before the child does: an alias
         * left standing over an unparented target is a dangling property,
         * not an absent one, and a deep introspection walk over a board
         * that leaves ethernet off reads through it into freed state.
         */
        object_property_del(OBJECT(dev), "netdev");
        object_property_del(OBJECT(dev), "mac");
        object_property_del(OBJECT(dev), "phy-addr");
        object_property_del(OBJECT(dev), "phy-dp83843");
        object_unparent(OBJECT(&s->eth));
    }

    if (s->board->onewire_identity) {
        uint8_t page[DS2502_PAGE_SIZE];
        DeviceState *part;

        /*
         * The station-address part, a DS2501: the 512-bit sibling of the
         * DS2502, which is what the family byte 0x91 -- the custom bit over
         * base type 0x11 -- identifies. Its bytes use the fitted ethernet
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

    /* On the chip's own bus, so a reset of the IOC3 reaches it. */
    if (s->board->kbc) {
        if (!qdev_realize(DEVICE(&s->kbc_dev), BUS(&s->ioc3_bus), errp)) {
            return;
        }
        memory_region_add_subregion(&s->bar, SGI_IOC3_KBC_BASE,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(&s->kbc_dev), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->kbc_dev), 0,
                           qdev_get_gpio_in_named(DEVICE(dev), "kbc-irq", 0));
    } else {
        object_unparent(OBJECT(&s->kbc_dev));
    }

    /* On the chip's own bus, so a reset of the IOC3 reaches it. */
    if (s->board->parallel) {
        object_property_set_link(OBJECT(&s->parallel_dev), "dma-memory",
                                 OBJECT(&dev->bus_master_container_region),
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

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        s->sdma_alias_port[i].chip = s;
        s->sdma_alias_port[i].port = i;

        /*
         * For both ports whatever the board fits. The reset path deletes
         * every timer and the migration format carries the array whole, so
         * leaving one unmade would make an absent port a null dereference
         * rather than a quiet nothing.
         */
        s->sdma_rx_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           sgi_ioc3_sdma_rx_timer_expired,
                                           &s->sdma_alias_port[i]);

        if (!s->board->serial[i]) {
            continue;
        }

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
     * migration stream too. The M48T35 alone is reached by an alias, so
     * only its unparent needs one deleted first, same as ethernet above.
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
        object_unparent(OBJECT(&s->ds1685));
    }

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /* The SuperIO line is board wiring, not the PCI pin. */
    qdev_init_gpio_out(DEVICE(dev), &s->superio_irq, 1);
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
        timer_free(s->sdma_rx_timer[i]);
    }
    timer_del(s->int_out_timer);
    if (s->board->eth) {
        qdev_unrealize(DEVICE(&s->eth));
    }
    if (s->board->kbc) {
        qdev_unrealize(DEVICE(&s->kbc_dev));
    }
    if (s->board->parallel) {
        qdev_unrealize(DEVICE(&s->parallel_dev));
    }
    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_M48T35) {
        qdev_unrealize(DEVICE(&s->rtc));
    }
    if (s->board->timekeeper == SGI_IOC3_TIMEKEEPER_DS1685) {
        qdev_unrealize(DEVICE(&s->ds1685));
    }
}

static void sgi_ioc3_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    s->int_status = 0;
    s->int_enable = 0;
    sgi_ioc3_update_irq(s);

    memset(s->sdma_reg, 0, sizeof(s->sdma_reg));
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        timer_del(s->sdma_rx_timer[i]);
        s->sdma_rx_idle_count[i] = 0;
        s->sdma_tx_slot[i] = 0;
        s->sdma_packed[i] = 0;
        memset(s->sdma_packer[i], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
        s->sdma_delivered[i] = false;
        /* Reset clears the rest of modem control, so it clears this too. */
        s->serial_mcr_autoflow[i] = 0;
        s->serial_lsr_test[i] = 0;
        s->serial_msr_test[i] = 0;
    }

    /* The buffer RAM is memory: reset does not clear it. */
    s->microlan_control = 0;
    s->pin_control = 0;
    s->pin_data = 0;
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
}

static void sgi_ioc3_finalize(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);

    timer_free(s->int_out_timer);
}

/*
 * The 16550s ride in this device's own migration stream rather than being
 * children with a stream each, so a board that fits none has to leave them
 * out. Saving them anyway walks a serial that was never realized, whose
 * receive-timeout timer is a null pointer the subsection test dereferences.
 * A board that fits them writes exactly the field it always did, so the
 * format is unchanged wherever it was ever produced.
 */
static bool sgi_ioc3_serial_needed(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;

    return s->board->serial[0] || s->board->serial[1];
}

static int sgi_ioc3_pre_load(void *opaque)
{
    SGIIOC3State *s = opaque;

    memset(s->sdma_tx_slot, 0, sizeof(s->sdma_tx_slot));
    return 0;
}

static int sgi_ioc3_post_load(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;
    unsigned int i;

    if (version_id < 15) {
        s->int_out_active = 0;
        s->int_out_state = false;
        s->int_out_pulse_end = false;
        s->int_out_square_started = false;
        timer_del(s->int_out_timer);
    } else if (sgi_ioc3_int_out_periodic_has_no_low_phase(
                   s->int_out_active) &&
               (s->int_out_state || s->int_out_pulse_end)) {
        /*
         * Early version 15 streams can contain the count-two periodic mode.
         * It has no low phase, so retain its readback and asserted level but
         * discard the redundant timer. A not-yet-fired timer keeps its
         * migrated deadline and makes the same conversion in its callback.
         */
        sgi_ioc3_int_out_hold_high(s);
    }
    sgi_ioc3_update_irq(s);
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        sgi_ioc3_sdma_run(s, i);
        /*
         * baudbase is a realize-time property of the embedded SerialState,
         * not a migrated field, so vmstate_serial's own post_load -- which
         * already ran, nested inside the field load above -- computed the
         * port's baud rate from the 16550 core's stock default rather than
         * from the pre-divider this chip overloads the scratch register
         * with. Rederive it now that serial_predivider[] has loaded.
         * sgi_ioc3_serial_program_predivider() already treats 0 or 1 (never
         * programmed, or absent on a pre-17 stream) as "leave it alone".
         */
        sgi_ioc3_serial_program_predivider(s, i, s->serial_predivider[i]);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 21,
    .minimum_version_id = 14,
    .pre_load = sgi_ioc3_pre_load,
    .post_load = sgi_ioc3_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC3State),
        VMSTATE_UINT32(int_status, SGIIOC3State),
        VMSTATE_UINT32(int_enable, SGIIOC3State),
        VMSTATE_STRUCT_ARRAY_TEST(serial, SGIIOC3State, SGI_IOC3_SERIAL_PORTS,
                                  sgi_ioc3_serial_needed, 0, vmstate_serial,
                                  SerialState),
        VMSTATE_UINT8_ARRAY_V(serial_predivider, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS, 17),
        VMSTATE_UINT8_ARRAY_V(serial_mcr_autoflow, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS, 18),
        VMSTATE_UINT8_ARRAY_V(serial_lsr_test, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS, 19),
        VMSTATE_UINT8_ARRAY_V(serial_msr_test, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS, 20),
        VMSTATE_UINT16_ARRAY_V(sdma_rx_idle_count, SGIIOC3State,
                               SGI_IOC3_SERIAL_PORTS, 21),
        VMSTATE_UINT32(microlan_control, SGIIOC3State),
        VMSTATE_UINT32(pin_control, SGIIOC3State),
        VMSTATE_UINT32(pin_data, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(sdma_reg, SGIIOC3State, SGI_IOC3_SDMA_REGS),
        VMSTATE_UINT32(chip_control, SGIIOC3State),
        VMSTATE_UINT32(int_out, SGIIOC3State),
        VMSTATE_UINT32_V(int_out_active, SGIIOC3State, 15),
        VMSTATE_BOOL_V(int_out_state, SGIIOC3State, 15),
        VMSTATE_BOOL_V(int_out_pulse_end, SGIIOC3State, 15),
        VMSTATE_BOOL_V(int_out_square_started, SGIIOC3State, 15),
        VMSTATE_TIMER_PTR_V(int_out_timer, SGIIOC3State, 15),
        VMSTATE_UINT8_2DARRAY(sdma_packer, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS,
                              SGI_IOC3_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8_ARRAY(sdma_packed, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT8_ARRAY_V(sdma_tx_slot, SGIIOC3State,
                             SGI_IOC3_SERIAL_PORTS, 16),
        VMSTATE_TIMER_PTR_ARRAY(sdma_rx_timer, SGIIOC3State,
                                SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT32_ARRAY(ssram_word, SGIIOC3State, SGI_IOC3_SSRAM_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_properties[] = {
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
    DEFINE_PROP_UINT64("board-onewire-serial", SGIIOC3State,
                       board_onewire_serial, SGI_IOC3_BOARD_ONEWIRE_SERIAL),
};

/*
 * Read-only, because it is a fact about this model rather than a setting:
 * the ethernet engine reaches its descriptors and buffers with explicit
 * big-endian accessors and the serial blocks place their own byte lanes, so
 * the chip already sees guest memory the way the guest wrote it. A bridge
 * that corrected the bytes on top would reverse every word this device
 * reads. True in whichever slot a board fits the chip into.
 */
static bool sgi_ioc3_byte_lane_self_ordered(Object *obj, Error **errp)
{
    return true;
}

static void sgi_ioc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 base I/O controller";

    object_class_property_add_bool(oc, SGI_BRIDGE_BYTE_LANE_SELF_ORDERED,
                                   sgi_ioc3_byte_lane_self_ordered, NULL);

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
