/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 *
 * One PCI device, one BAR covering everything: two 16550 serial ports and
 * their DMA engines, the ethernet block and its buffer RAM, a PS/2
 * controller, a one-wire ethernet-address part, and the byte bus's
 * timekeeper. The SuperIO window at 0x20000 holds byte-wide registers one
 * byte apart where the chip's other blocks are 32 bits wide.
 *
 * Unimplemented parts of the window are holes, not zeroes: an access there
 * reaches no device and is reported.
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

/*
 * The 16550 windows: byte-wide registers one byte apart. An access wider
 * than a byte is refused -- no source documents one, and the datasheet
 * records refusal as a choice. Both guests place their own byte lanes, so
 * no lane arithmetic may be added here.
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
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/*
 * The SuperIO line is a level: status AND enable, recomputed on every write
 * to either register and on every source change, never latched.
 */
static void sgi_ioc3_update_irq(SGIIOC3State *s)
{
    qemu_set_irq(s->superio_irq, !!(s->int_status & s->int_enable));
}

/*
 * A port's status bit follows the 16550's own line rather than latching;
 * only the guest's register read quiets it.
 */
static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port);

static void sgi_ioc3_serial_irq(void *opaque, int port, int level)
{
    SGIIOC3State *s = opaque;
    uint32_t bit = port ? SGI_IOC3_INT_SERIAL_B : SGI_IOC3_INT_SERIAL_A;

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
    sgi_ioc3_update_irq(s);

    /*
     * Run the DMA engine on every level, not on assertion alone: a masked
     * 16550 still delivers characters here with level 0, and IRIX's
     * DMA-ring console receives with the 16550 disabled entirely. With
     * ring DMA off the run is a no-op.
     */
    sgi_ioc3_sdma_run(s, port);
}

/*
 * The ethernet child has no PCI identity of its own; its GPIO level becomes
 * this chip's PCI interrupt pin.
 */
static void sgi_ioc3_eth_irq(void *opaque, int n, int level)
{
    SGIIOC3State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * The one-wire ethernet-address part behind the MicroLAN engine: reset, ROM
 * selection, function command, two-byte target address, answer shifted out
 * bit by bit. Protocol per docs/datasheets/ioc3.md, "The ethernet-address
 * part's read protocol". OpenBSD's owmac selects by Match ROM, so selection
 * compares a real identifier.
 */

/*
 * The standard one-wire CRC-8: reflected polynomial 0x8c, seed zero, sent
 * as computed (not inverted, unlike the CRC-16 below).
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
 * The standard one-wire CRC-16: reflected polynomial 0xa001, seed zero.
 * Linux's ioc3eth_get_mac_addr() checks the fixed residue 0xb001.
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
 * The 64-bit ROM identifier: family 0x91, serial top twelve bits 0x5e7
 * (both required -- see the datasheet section named above), CRC-8 over the
 * seven bytes.
 */
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
        /*
         * No page is ever redirected: status bytes of 0xff are what stop
         * owmem_redirect()'s loop on its first look.
         */
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

/*
 * Run one bit slot and leave its answer in the level bit. The pulse field
 * (bits 10 up) tells a reset from an ordinary slot; phase transitions
 * follow the slot sequence, not its timing -- a driver cannot tell instant
 * completion from the real microseconds.
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
         * Three slots per identity bit: the bit, its complement, the
         * guest's choice. The part stays selected afterwards; SGI's PROM
         * relies on that and sends no second ROM command.
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
        /*
         * Match ROM, LSB first; a part that does not match stops
         * answering until the next reset.
         */
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
        /*
         * Bit 22 is the read-only arbiter-idle status; the kernel's serial
         * console setup polls it, and this model's arbiter is always idle.
         */
        return s->chip_control | SGI_IOC3_CHIP_CTRL_ARB_IDLE;
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
         * Write-one-to-clear for latched sources. The pass-through and
         * receive high-water bits are levels and ignore the write: clearing
         * a still-asserted level would drop an interrupt never raised again.
         */
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
        /*
         * Stored, not acted on: every guest programs the port-offset fields
         * to the two offsets this chip already implements.
         */
        s->chip_control = val & SGI_IOC3_CHIP_CTRL_MASK;
        break;
    case SGI_IOC3_MICROLAN_CONTROL:
        /*
         * One one-wire time slot, performed synchronously: the timing is
         * not observable from a driver's read-after-write polling.
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
    .endianness = DEVICE_LITTLE_ENDIAN,
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
         * Held as written; the hardware-owned busy bits are never set, so
         * a driver polling for idle reads idle at once.
         */
        return s->kbc_control;

    case SGI_IOC3_KBC_KBD_RECEIVE:
    case SGI_IOC3_KBC_AUX_RECEIVE:
        /*
         * Nothing is plugged in. Zero clears every per-byte valid flag, so
         * this reads as a port that received nothing, not three zero bytes.
         */
        return 0;

    default:
        /* The transmit registers are write-only. */
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
        /* Accepted and dropped: an unplugged port, not a fault. */
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
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};
/*
 * The serial DMA engines. One base register covers all four rings -- port A
 * transmit, port A receive, port B transmit, port B receive, in that fixed
 * order, each of the size the base register's low bit selects -- so a
 * ring's address comes from its port and direction and no register reads
 * it back.
 */

/*
 * The 16550's published register layout; hw/char/serial.c's own
 * definitions are private to it.
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

#define UART_MCR_DTR    0x01
#define UART_MCR_RTS    0x02
#define UART_MCR_OUT1   0x04
#define UART_MCR_OUT2   0x08
#define UART_MCR_LOOP   0x10

#define UART_MSR_DCTS   0x01
#define UART_MSR_DDCD   0x08
#define UART_MSR_CTS    0x10
#define UART_MSR_DCD    0x80

/*
 * Split an offset into port and per-port register. The shared ring base
 * pair sits below port A's base and belongs to neither port.
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
 * The selected ring size is also every index register's pointer width: an
 * index addresses the ring in eight-byte units and cannot leave it.
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
 * One ring's address. The size-select bit lives inside the base register
 * but outside the address; unmasked, every ring lands one byte off.
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
 * OR into the live register, never write back a snapshot: a transmit walk
 * in loopback delivers to this chip's own receiver before returning, and a
 * pre-walk snapshot would destroy any source raised inside the walk.
 */
static void sgi_ioc3_raise_serial_source(SGIIOC3State *s, unsigned int port,
                                         unsigned int source)
{
    s->int_status |= SGI_IOC3_SERIAL_SOURCE(port, source);
    sgi_ioc3_update_irq(s);
}

/* A level source: set or cleared to match a condition, never latched. */
static void sgi_ioc3_set_serial_source(SGIIOC3State *s, unsigned int port,
                                       unsigned int source, bool level)
{
    uint32_t bit = SGI_IOC3_SERIAL_SOURCE(port, source);

    s->int_status = (s->int_status & ~bit) | (level ? bit : 0);
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
 * Post the packer as one receive-ring entry. Unfilled slots carry neither
 * validity flag, so a short entry says so.
 */
static bool sgi_ioc3_sdma_post(SGIIOC3State *s, unsigned int port,
                               uint32_t *produce, uint32_t *consume,
                               uint32_t mask)
{
    uint64_t ring = sgi_ioc3_sdma_ring(s, port, SGI_IOC3_SDMA_RING_RX);
    uint32_t next = (*produce + SGI_IOC3_SDMA_ENTRY_SIZE) & mask;

    /*
     * Full ring: refuse rather than overwrite an unconsumed entry. The
     * characters keep, in the packer and behind it the port's FIFO.
     */
    if (next == (*consume & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_RX_RING_FULL);
        return false;
    }

    /*
     * A refused fabric write must not advance the index: an index past an
     * entry that never reached memory points the driver at garbage.
     */
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

/*
 * Wind the receive timer: the programmed reload in 16 kHz ticks from now,
 * so it expires after that much receive idleness (the datasheet's account
 * of the reload).
 */
static void sgi_ioc3_sdma_rx_timer_start(SGIIOC3State *s, unsigned int port)
{
    uint32_t reload = *sgi_ioc3_sdma_port_reg(s, port, SGI_IOC3_SDMA_RX_TIMER)
                      & SGI_IOC3_SDMA_TIMER_RELOAD;

    timer_mod(s->sdma_rx_timer[port],
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)reload * SGI_IOC3_SDMA_TIMER_TICK_NS);
}

/*
 * Receive idleness ran out. A drained ring expires into silence: the
 * interrupt only means something while characters wait.
 */
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

    /*
     * A partial entry still packed means the ring was full; expiry is
     * another moment software may have made room by.
     */
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
 * The high-water source is a level, recomputed wherever the waiting count
 * moves: a latched raise storms IRIX's ring console, which never writes an
 * acknowledgement. The mark is in characters, not entries -- the firmware
 * programs 511 against 1 KB rings of four-character entries -- so waiting
 * is counted as published entries times four.
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
 * End of burst: post any short group now rather than waiting out the
 * receive timer -- this model's substitution; a guest polling the produce
 * index sees a short entry earlier than silicon would show one. The
 * interrupt is not hurried: delivery winds the timer and the timer's
 * expiry raises the source, the timing the datasheet describes.
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

        /*
         * A full packer goes to memory first; on refusal the drain stops
         * without dropping -- the characters keep in the packer and the
         * FIFO until software makes room.
         */
        if (s->sdma_packed[port] == SGI_IOC3_SDMA_ENTRY_CHARS &&
            !sgi_ioc3_sdma_post(s, port, produce, consume, mask)) {
            break;
        }

        lsr = serial_io_ops.read(serial, UART_LSR, 1);
        if (!(lsr & UART_LSR_DR)) {
            break;
        }

        /*
         * Reading LSR clears its error latches, so it is read before the
         * character it describes -- the other order reports the previous
         * character's errors against this one.
         */
        s->sdma_packer[port][s->sdma_packed[port]] =
            serial_io_ops.read(serial, UART_RBR_THR, 1);
        msr = serial_io_ops.read(serial, UART_MSR, 1);

        /*
         * Two separate validity flags: data-and-errors, modem-and-overrun.
         * Unfilled slots carry neither, which is how a driver reading a
         * whole entry knows where the characters stop.
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
         * An entry goes to memory at four characters, not before; the next
         * pass's post or the end-of-burst flush writes it.
         */
        ++s->sdma_packed[port];
    }
}

/*
 * Walk the transmit ring as far as software has handed over. Each control
 * byte types its data byte: character, modem-control write, or delay; the
 * type with both bits clear ends the entry, and sending past it puts
 * memory garbage on the wire after every short write.
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

        /*
         * A refused fetch stops the walk where it is: the stack bytes are
         * not an entry.
         */
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
                /* The entry ends here, and so does this slot's interrupt. */
                continue;
            case SGI_IOC3_SDMA_TX_CHARACTER:
                serial_io_ops.write(serial, UART_RBR_THR, entry[i], 1);
                /*
                 * In loopback the sent character sits in this port's own
                 * 16-byte receive FIFO; draining per character is what
                 * stops a 500-character ring overrunning it.
                 */
                sgi_ioc3_sdma_receive(s, port);
                break;
            case SGI_IOC3_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC3_SDMA_TX_DELAY:
                /* Nothing to pace against: the transmitter is instant. */
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
     * Raised on reaching the end of a walk, not on every idle engine, so
     * an empty ring does not raise it forever.
     */
    if (walked && (*consume & mask) == (*produce & mask)) {
        sgi_ioc3_raise_serial_source(s, port, SGI_IOC3_INT_TX_RING_EMPTY);
    }
}

static void sgi_ioc3_sdma_run(SGIIOC3State *s, unsigned int port)
{
    /*
     * One walk per port at a time: loopback re-enters through the port's
     * own interrupt, and a second walk would post the packer's half-filled
     * entry mid-burst.
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
 * The control register's reset bit is an action, not a setting: stored, it
 * would hold a driver's engine in reset for good. It resets the four
 * indices and nothing else -- the watermark and timer reload survive.
 */
static void sgi_ioc3_sdma_reset_port(SGIIOC3State *s, unsigned int port)
{
    unsigned int reg;

    for (reg = SGI_IOC3_SDMA_TX_PRODUCE; reg <= SGI_IOC3_SDMA_RX_CONSUME;
         reg += 4) {
        *sgi_ioc3_sdma_port_reg(s, port, reg) = 0;
    }

    /* And the characters the packer was holding for an entry it never wrote. */
    s->sdma_packed[port] = 0;
    memset(s->sdma_packer[port], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
    s->sdma_delivered[port] = false;

    /* Zeroing the consume index disarmed the receive timer, so it stops. */
    timer_del(s->sdma_rx_timer[port]);
}

/*
 * The 16550 shadow: a passive mirror of the port's status and modem lines,
 * assembled from the register file's fields directly -- the byte-wide read
 * ops clear the delta and error latches and this view must not. The
 * DMA-side console driver reads carrier from here; a shadow of zero hangs
 * the console at open.
 */
static uint32_t sgi_ioc3_sdma_shadow(SGIIOC3State *s, unsigned int port)
{
    SerialState *serial = &s->serial[port];
    uint8_t lsr = serial->lsr;
    uint8_t mcr = serial->mcr;
    uint8_t msr;
    uint32_t shadow;

    /*
     * In loopback the modem-control outputs wire back to the status
     * inputs, the same derivation the byte-wide modem-status read makes.
     */
    if (mcr & UART_MCR_LOOP) {
        msr = ((mcr & 0x0c) << 4) | ((mcr & 0x02) << 3) | ((mcr & 0x01) << 5);
    } else {
        msr = serial->msr;
    }

    /* The low byte mirrors the line-status register one for one. */
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

    /*
     * The produce index also reports how many characters the packer holds
     * for the entry it has not written yet.
     */
    if (offset == SGI_IOC3_SDMA_RX_PRODUCE) {
        val |= (uint32_t)s->sdma_packed[port] << SGI_IOC3_SDMA_HELD_SHIFT;
    }

    /*
     * The count field reads back the ticks left while the timer runs; an
     * idle timer's count is stated by no source, and zero is the choice.
     */
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

/* Write one port register; none of them keeps a whole word. */
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
        /* A disabled engine delivers nothing for the timer to announce. */
        if (!(val & SGI_IOC3_SDMA_ENABLE)) {
            timer_del(s->sdma_rx_timer[port]);
        }
        /*
         * The drain bit (27) is a command, not stored: IRIX polls the
         * register until it reads clear, and a stuck bit is the reading no
         * driver survives. Enabling is also a start -- the firmware hands
         * the transmit ring over before it enables the engine, so a walk
         * only on produce-index writes would never happen here.
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

        /*
         * Bit 31 arms the receive timer. Re-arming over a non-empty ring
         * winds it again -- a choice, stated by no source; prompting again
         * is the reading a driver can only be helped by.
         */
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
        /*
         * Discarded, and not logged: OpenBSD writes zero here on every
         * stock boot, and the chip's shadow is recomputed regardless.
         */
        return;

    case SGI_IOC3_SDMA_TX_CONSUME:
    case SGI_IOC3_SDMA_RX_PRODUCE:
        /*
         * The engine's side of the ring. Refused and traced, not a guest
         * error: the firmware writes both, zero, on every stock boot. The
         * datasheet records refusal as a hedge -- only a non-zero write
         * could tell refusing from accepting.
         */
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

/*
 * The alias pages: a second view of one port's seven registers, so a
 * privileged process can map one engine into user space without the rest
 * of the chip.
 */
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
 * The ethernet buffer RAM: 17 bits stored per slot, 16 of data plus the
 * parity bit beside them. The window's halves are separate storage --
 * a midpoint alias would fail the driver's size probe -- and parity is
 * stored, not computed, so a diagnostic can place a wrong one.
 */
static uint64_t sgi_ioc3_ssram_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3State *s = opaque;
    uint32_t word = s->ssram_word[addr / 4];

    /*
     * Even parity across the seventeen stored bits is correct; an odd
     * count reports the error when checking is enabled.
     */
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

    /* Only 48 bits fit the ROM identity; more would truncate silently. */
    if (s->onewire_serial & ~SGI_IOC3_ONEWIRE_SERIAL_MASK) {
        error_setg(errp, "onewire-serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }

    /*
     * The PCI interrupt pin belongs to the ethernet block; the SuperIO
     * line is separate, directly wired -- see superio_irq below.
     */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /*
     * PCI Status must reset to 0x0280 (medium DEVSEL, fast back-to-back):
     * IRIX's io_config_space diagnostic compares the whole dword and fails
     * the slot on a zero Status half. See docs/datasheets/ioc3.md.
     */
    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        if (!qdev_realize(DEVICE(serial), NULL, errp)) {
            return;
        }

        /* Eight byte-wide registers, so an eight-byte region. */
        memory_region_init_io(&s->serial_io[i], OBJECT(dev),
                              &sgi_ioc3_serial_ops, serial,
                              sgi_ioc3_serial_name[i], 8);
        memory_region_add_subregion(&s->bar, sgi_ioc3_serial_base[i],
                                    &s->serial_io[i]);

        /* Both ports' interrupts leave the chip on the one SuperIO line. */
        serial->irq = qdev_get_gpio_in(DEVICE(dev), i);
    }

    memory_region_init_io(&s->main, OBJECT(dev), &sgi_ioc3_main_ops, s,
                          "sgi-ioc3.main", SGI_IOC3_MAIN_SIZE);
    memory_region_add_subregion(&s->bar, 0, &s->main);

    /*
     * The ethernet child's DMA must go through this function's bus-master
     * view to reach the Bridge's translation (docs/datasheets/ioc3.md,
     * "DMA"), so the view is handed down before the child realizes.
     */
    object_property_set_link(OBJECT(&s->eth), "dma-memory",
                             OBJECT(&dev->bus_master_container_region),
                             &error_abort);
    /* The serial engines' rings go through the same space. */
    s->dma_as = pci_get_address_space(dev);
    /*
     * The backend is claimed here: the child carries the NIC properties
     * and nothing outside can name it on a command line.
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
     * The alias pages reach the same accessors; each page's port travels
     * in its opaque, since handlers cannot tell which region reached them.
     */
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

        /*
         * The receive timer counts guest time: receive idleness stops
         * with the machine and migrates with it.
         */
        s->sdma_rx_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           sgi_ioc3_sdma_rx_timer_expired,
                                           &s->sdma_alias_port[i]);
    }

    /* An Origin 200 populates the whole buffer-RAM window. */
    memory_region_init_io(&s->ssram, OBJECT(dev), &sgi_ioc3_ssram_ops, s,
                          "sgi-ioc3.ssram", SGI_IOC3_SSRAM_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC3_SSRAM_BASE, &s->ssram);

    /*
     * The byte bus's first device select: the timekeeper's own region. A
     * sysbus child rather than a bus-less plain device, which is never
     * reset; the same holds for the ethernet block and its transceiver.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    memory_region_add_subregion(&s->bar, SGI_IOC3_BYTEBUS_DEV0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rtc),
                                                       0));

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    /*
     * The SuperIO line is board wiring, not the PCI pin: a plain GPIO
     * output the machine connects to the Bridge pin the board wires.
     */
    qdev_init_gpio_out(DEVICE(dev), &s->superio_irq, 1);
}

static void sgi_ioc3_exit(PCIDevice *dev)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    /*
     * Unrealize the children: their reset handlers would otherwise point
     * at freed state after unplug.
     */
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        qdev_unrealize(DEVICE(&s->serial[i]));
        timer_free(s->sdma_rx_timer[i]);
    }
    qdev_unrealize(DEVICE(&s->eth));
    qdev_unrealize(DEVICE(&s->rtc));
}

static void sgi_ioc3_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    s->int_status = 0;
    s->int_enable = 0;
    sgi_ioc3_update_irq(s);

    /*
     * The serial DMA block resets to zero throughout; a zeroed consume
     * index is a disarmed receive timer, so both timers stop.
     */
    memset(s->sdma_reg, 0, sizeof(s->sdma_reg));
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        timer_del(s->sdma_rx_timer[i]);
        s->sdma_packed[i] = 0;
        memset(s->sdma_packer[i], 0, SGI_IOC3_SDMA_ENTRY_SIZE);
        s->sdma_delivered[i] = false;
    }

    s->kbc_control = 0;

    /*
     * The buffer RAM is memory, not registers: a reset line does not
     * empty it, so it is deliberately not cleared here. Everything else
     * resets to zero, per SGI's own register diagnostic.
     */
    s->microlan_control = 0;
    s->pin_control = 0;
    s->pin_data = 0;
    s->chip_control = 0;

    /* A reset mid-answer must not leave stale bits shifting out. */
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

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        object_initialize_child(obj, "serial[*]", &s->serial[i], TYPE_SERIAL);
    }

    object_property_add_alias(obj, "chardev-a", OBJECT(&s->serial[0]),
                              "chardev");
    object_property_add_alias(obj, "chardev-b", OBJECT(&s->serial[1]),
                              "chardev");

    object_initialize_child(obj, "eth", &s->eth, TYPE_SGI_IOC3_ETH);

    /*
     * Aliases let any machine name the children's backends; a real IOC3
     * is also sold as a plain PCI card, so pluggable is what the part is.
     */
    object_property_add_alias(obj, "netdev", OBJECT(&s->eth), "netdev");
    object_property_add_alias(obj, "mac", OBJECT(&s->eth), "mac");

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_M48T35);

    object_property_add_alias(obj, "nvram-drive", OBJECT(&s->rtc), "drive");
}

/* The interrupt line is derived state: re-drive it after load. */
static int sgi_ioc3_post_load(void *opaque, int version_id)
{
    SGIIOC3State *s = opaque;

    sgi_ioc3_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 12,
    .minimum_version_id = 12,
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
        VMSTATE_UINT8_2DARRAY(sdma_packer, SGIIOC3State,
                              SGI_IOC3_SERIAL_PORTS,
                              SGI_IOC3_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8_ARRAY(sdma_packed, SGIIOC3State,
                            SGI_IOC3_SERIAL_PORTS),
        VMSTATE_TIMER_PTR_ARRAY(sdma_rx_timer, SGIIOC3State,
                                SGI_IOC3_SERIAL_PORTS),
        VMSTATE_UINT32(kbc_control, SGIIOC3State),
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
};

DEFINE_TYPES(sgi_ioc3_types)
