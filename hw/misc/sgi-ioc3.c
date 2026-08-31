/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 *
 * One PCI device, one BAR: two 16550s and their DMA engines, the ethernet
 * block and its buffer RAM, PS/2, a one-wire ethernet-address part, and
 * the byte bus's timekeeper. See docs/specs/sgi-ioc3.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "net/net.h"
#include "trace.h"

/* Byte-wide only; both guests place their own byte lanes -- add none here. */
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
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void sgi_ioc3_update_irq(SGIIOC3State *s)
{
    qemu_set_irq(s->superio_irq, !!(s->int_status & s->int_enable));
}

static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port);

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

/* The ethernet child's GPIO level becomes this chip's PCI interrupt pin. */
static void sgi_ioc3_eth_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * The PS/2 controller's data-available line latches interrupt-status bit 22:
 * a queued byte sets it and it holds across a drain of the receive registers.
 * Only a write-one-to-clear of the status register acknowledges it, which is
 * what both Linux's edge handler and IRIX's acknowledge writes expect.
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
 * The one-wire ethernet-address part behind the MicroLAN engine; protocol
 * per the ioc3 datasheet (docs/datasheets/ioc3.md in the origami project
 * tree the fork is developed inside, not this repository).
 */

/* Standard one-wire CRC-8, sent as computed -- unlike the CRC-16 below. */
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

/* Standard one-wire CRC-16; stored inverted for the 0xb001 residue. */
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

static uint64_t sgi_ioc3_onewire_rom_id(uint64_t serial)
{
    uint8_t rom[7];
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

/* All three function commands share the CRC-8-of-echo framing. */
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
        /* 0xff status bytes stop owmem_redirect()'s loop on its first look. */
        memset(&s->onewire_out[1], 0xff, SGI_IOC3_ONEWIRE_STATUS_SIZE);
        s->onewire_out_len = 1 + SGI_IOC3_ONEWIRE_STATUS_SIZE;
        s->onewire_outbit = 0;
        s->onewire_phase = SGI_IOC3_ONEWIRE_ANSWER;
        return;
    }

    /* Both guests only ever ask for page zero, the only page held. */
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

/* One bit slot; phases follow the slot sequence, never its timing. */
static void sgi_ioc3_onewire_slot(SGIIOC3State *s, uint32_t val)
{
    uint32_t pulse = val >> SGI_IOC3_ONEWIRE_PULSE_SHIFT;
    bool level = true;

    if (!(s->pin_control & SGI_IOC3_PIN_MICROLAN_ENABLE)) {
        /*
         * Pin-control bit 21 routes the engine onto its pin; with it clear the
         * bus is disconnected. No part answers: the wire reads high and the
         * slot advances no conversation.
         */
        s->microlan_control = (val & SGI_IOC3_MICROLAN_TIMING) |
                              SGI_IOC3_MICROLAN_DONE |
                              SGI_IOC3_MICROLAN_LEVEL;
        return;
    }

    if (pulse >= SGI_IOC3_ONEWIRE_PULSE_RESET) {
        /* Reset: the part pulls the wire low and forgets the conversation. */
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
                /* Linux's w1 core: Skip ROM with one device registered. */
                s->onewire_command = 0;
                s->onewire_phase = SGI_IOC3_ONEWIRE_FUNCTION;
                break;
            case SGI_IOC3_ONEWIRE_ROM_MATCH:
                /* OpenBSD's owmac always selects by Match ROM. */
                s->onewire_match = 0;
                s->onewire_phase = SGI_IOC3_ONEWIRE_MATCH;
                break;
            default:
                s->onewire_phase = SGI_IOC3_ONEWIRE_IDLE;
                break;
            }
        }
    } else if (s->onewire_phase == SGI_IOC3_ONEWIRE_SEARCH) {
        /*
         * Three slots per identity bit. The part stays selected after the
         * search; SGI's PROM sends no second ROM command.
         */
        unsigned int step = s->onewire_bit % 3;
        unsigned int index = s->onewire_bit / 3;
        bool bit = (sgi_ioc3_onewire_rom_id(s->onewire_serial) >> index) & 1;

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
        /* A part that does not match stops answering until the next reset. */
        if (pulse < SGI_IOC3_ONEWIRE_PULSE_ZERO) {
            s->onewire_match |= 1ULL << s->onewire_bit;
        }
        if (++s->onewire_bit == 64) {
            s->onewire_bit = 0;
            s->onewire_command = 0;
            s->onewire_phase =
                (s->onewire_match == sgi_ioc3_onewire_rom_id(s->onewire_serial))
                ? SGI_IOC3_ONEWIRE_FUNCTION : SGI_IOC3_ONEWIRE_IDLE;
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
        /* Shifting the answer back, LSB of each byte first. */
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
        /* Bit 22: arbiter idle, polled by the kernel's console setup. */
        return s->chip_control | SGI_IOC3_CHIP_CTRL_ARB_IDLE;
    case SGI_IOC3_INT_OUTPUT:
        return s->int_out;
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
        /* Write-one-to-clear; the level-driven bits ignore the write. */
        s->int_status &= ~(val & ~(SGI_IOC3_INT_PASSTHROUGH |
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
        /* Stored: every guest programs the offsets already implemented. */
        s->chip_control = val & SGI_IOC3_CHIP_CTRL_MASK;
        break;
    case SGI_IOC3_INT_OUTPUT:
        /*
         * Stored configuration only. The interval timer and the pin drive
         * are not modelled; no guest on our path drives them.
         */
        s->int_out = val & SGI_IOC3_INT_OUT_MASK;
        break;
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
#define UART_MCR        4
#define UART_LSR        5
#define UART_MSR        6

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

#define UART_MSR_DCTS   0x01
#define UART_MSR_DDCD   0x08
#define UART_MSR_CTS    0x10
#define UART_MSR_DCD    0x80

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

        for (i = 0; i < SGI_IOC3_SDMA_ENTRY_CHARS; i++) {
            uint8_t control = entry[SGI_IOC3_SDMA_ENTRY_CHARS + i];

            switch (control & SGI_IOC3_SDMA_TX_TYPE) {
            case SGI_IOC3_SDMA_TX_NOTHING:
                continue;
            case SGI_IOC3_SDMA_TX_CHARACTER:
                serial_io_ops.write(serial, UART_RBR_THR, entry[i], 1);
                /* Drain per character or loopback overruns the 16-byte FIFO. */
                sgi_ioc3_sdma_receive(s, port);
                break;
            case SGI_IOC3_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC3_SDMA_TX_DELAY:
                break;
            }

            if (control & SGI_IOC3_SDMA_TX_INTERRUPT) {
                sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_DONE);
            }
        }

        *consume = (*consume + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;
        walked = true;
    }

    /* On reaching the end of a walk, not on every idle engine. */
    if (walked && (*consume & mask) == (*produce & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_RING_EMPTY);
    }
}

static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port)
{
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

    timer_del(s->sdma_rx_timer[port]);
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

    /* Ticks left while running; an idle timer's count reads zero. */
    if (offset == SGI_IOC3_SDMA_RX_TIMER &&
        timer_pending(s->sdma_rx_timer[port])) {
        int64_t left = timer_expire_time_ns(s->sdma_rx_timer[port]) -
                       qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint32_t ticks = MIN(MAX(left, 0) / SGI_IOC3_SDMA_TIMER_TICK_NS,
                             SGI_IOC3_SDMA_TIMER_RELOAD);

        val |= ticks << SGI_IOC3_SDMA_TIMER_COUNT_SHIFT;
    }

    return val;
}

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
        return;

    case SGI_IOC3_SDMA_SHADOW:
        /* Discarded silently: OpenBSD writes zero here on every boot. */
        return;

    case SGI_IOC3_SDMA_TX_CONSUME:
    case SGI_IOC3_SDMA_RX_PRODUCE:
        /* The engine's side; refused and traced -- firmware writes zeroes. */
        trace_sgi_ioc3_sdma_write_refused(
            (uint32_t)(SGI_IOC3_SDMA_PORT_A +
                       port * SGI_IOC3_SDMA_PORT_STRIDE + offset),
            (uint32_t)val);
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

    if (s->onewire_serial & ~SGI_IOC3_ONEWIRE_SERIAL_MASK) {
        error_setg(errp, "onewire-serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /* IRIX's configuration diagnostic fails the slot on a zero Status. */
    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        if (!qdev_realize(DEVICE(serial), NULL, errp)) {
            return;
        }

        memory_region_init_io(&s->serial_io[i], OBJECT(dev),
                              &sgi_ioc3_serial_ops, serial,
                              sgi_ioc3_serial_name[i], 8);
        memory_region_add_subregion(&s->bar, sgi_ioc3_serial_base[i],
                                    &s->serial_io[i]);

        serial->irq = qdev_get_gpio_in(DEVICE(dev), i);
    }

    memory_region_init_io(&s->main, OBJECT(dev), &sgi_ioc3_main_ops, s,
                          "sgi-ioc3.main", SGI_IOC3_MAIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->main);

    /* All child DMA goes through this function's bus-master view. */
    object_property_set_link(OBJECT(&s->eth), "dma-memory",
                             OBJECT(&dev->bus_master_container_region),
                             &error_abort);
    s->dma_as = pci_get_address_space(dev);
    qemu_configure_nic_device(DEVICE(&s->eth), true, NULL);
    if (!qdev_realize(DEVICE(&s->eth), BUS(&s->ioc3_bus), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_ENET_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->eth),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 0,
                       qdev_get_gpio_in_named(DEVICE(dev), "eth-irq", 0));

    /* On the chip's own bus, so a reset of the IOC3 reaches it. */
    if (!qdev_realize(DEVICE(&s->kbc_dev), BUS(&s->ioc3_bus), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_KBC_BASE,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->kbc_dev), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->kbc_dev), 0,
                       qdev_get_gpio_in_named(DEVICE(dev), "kbc-irq", 0));

    memory_region_init_io(&s->sdma, OBJECT(dev), &sgi_ioc3_sdma_ops, s,
                          "sgi-ioc3.sdma", SGI_IOC3_SDMA_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SDMA_BASE, &s->sdma);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        s->sdma_alias_port[i].chip = s;
        s->sdma_alias_port[i].port = i;
        memory_region_init_io(&s->sdma_alias[i], OBJECT(dev),
                              &sgi_ioc3_sdma_alias_ops,
                              &s->sdma_alias_port[i],
                              sgi_ioc3_sdma_alias_name[i],
                              SGI_IOC3_SDMA_ALIAS_SIZE);
        memory_region_add_subregion(&s->bar, sgi_ioc3_sdma_alias_base[i],
                                    &s->sdma_alias[i]);

        s->sdma_rx_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           sgi_ioc3_sdma_rx_timer_expired,
                                           &s->sdma_alias_port[i]);
    }

    memory_region_init_io(&s->ssram, OBJECT(dev), &sgi_ioc3_ssram_ops, s,
                          "sgi-ioc3.ssram", SGI_IOC3_SSRAM_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SSRAM_BASE, &s->ssram);

    /* On the chip's own bus, so a reset of the IOC3 reaches it. */
    if (!qdev_realize(DEVICE(&s->rtc), BUS(&s->ioc3_bus), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rtc),
                                                       0));

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /* The SuperIO line is board wiring, not the PCI pin. */
    qdev_init_gpio_out(DEVICE(dev), &s->superio_irq, 1);
}

static void sgi_ioc3_exit(PCIDevice *dev)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    /* Unrealize children or their reset handlers point at freed state. */
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        qdev_unrealize(DEVICE(&s->serial[i]));
        timer_free(s->sdma_rx_timer[i]);
    }
    qdev_unrealize(DEVICE(&s->eth));
    qdev_unrealize(DEVICE(&s->kbc_dev));
    qdev_unrealize(DEVICE(&s->rtc));
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
        s->sdma_packed[i] = 0;
        memset(s->sdma_packer[i], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
        s->sdma_delivered[i] = false;
    }

    /* The buffer RAM is memory: reset does not clear it. */
    s->microlan_control = 0;
    s->pin_control = 0;
    s->pin_data = 0;
    s->chip_control = 0;
    s->int_out = 0;

    s->onewire_phase = 0;
    s->onewire_bit = 0;
    s->onewire_command = 0;
    s->onewire_match = 0;
    s->onewire_args[0] = 0;
    s->onewire_args[1] = 0;
    s->onewire_argn = 0;
    memset(s->onewire_out, 0, sizeof(s->onewire_out));
    s->onewire_out_len = 0;
    s->onewire_outbit = 0;
}

static void sgi_ioc3_init(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    qdev_init_gpio_in(DEVICE(obj), sgi_ioc3_serial_irq,
                      SGI_IOC3_SERIAL_PORTS);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_eth_irq, "eth-irq", 1);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_kbc_irq, "kbc-irq", 1);

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

    object_initialize_child(obj, "kbc", &s->kbc_dev, TYPE_SGI_IOC3_KBC);

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_M48T35);

    object_property_add_alias(obj, "nvram-drive", OBJECT(&s->rtc), "drive");
}

static int sgi_ioc3_post_load(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;

    sgi_ioc3_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 14,
    .minimum_version_id = 14,
    .post_load = sgi_ioc3_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC3State),
        VMSTATE_UINT32(int_status, SGIIOC3State),
        VMSTATE_UINT32(int_enable, SGIIOC3State),
        VMSTATE_STRUCT_ARRAY(serial, SGIIOC3State, SGI_IOC3_SERIAL_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(microlan_control, SGIIOC3State),
        VMSTATE_UINT32(pin_control, SGIIOC3State),
        VMSTATE_UINT32(pin_data, SGIIOC3State),
        VMSTATE_UINT32_ARRAY(sdma_reg, SGIIOC3State, SGI_IOC3_SDMA_REGS),
        VMSTATE_UINT32(chip_control, SGIIOC3State),
        VMSTATE_UINT32(int_out, SGIIOC3State),
        VMSTATE_UINT8_2DARRAY(sdma_packer, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS,
                              SGI_IOC3_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8_ARRAY(sdma_packed, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_TIMER_PTR_ARRAY(sdma_rx_timer, SGIIOC3State,
                                SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT32_ARRAY(ssram_word, SGIIOC3State, SGI_IOC3_SSRAM_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_properties[] = {
    DEFINE_PROP_UINT64("onewire-serial", SGIIOC3State, onewire_serial,
                       SGI_IOC3_ONEWIRE_SERIAL),
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
