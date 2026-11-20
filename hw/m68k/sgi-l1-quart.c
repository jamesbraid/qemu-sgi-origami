/*
 * One channel of the L1 board's four-channel UART: a 16550-compatible part
 * with 64-byte FIFOs; l1-board.rst rates the depth and the trigger table.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"

#define TYPE_SGI_L1_QUART "sgi-l1-quart"
OBJECT_DECLARE_SIMPLE_TYPE(QuartState, SGI_L1_QUART)

#define FIFO_SIZE       64
#define LCR_DLAB        0x80
#define LCR_EFR         0xbf    /* the enhanced register bank */
#define IER_RX          0x01
#define IER_THRE        0x02
#define LSR_DR          0x01
#define LSR_IDLE        0x60    /* transmit holding and shift registers empty */
#define MSR_RESET       0xb0    /* carrier, data set and clear to send up */

struct QuartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;
    qemu_irq irq;
    QEMUTimer *timeout;
    uint32_t clock_hz;

    uint8_t rx[FIFO_SIZE];
    unsigned rx_head, rx_len;
    uint8_t ier, lcr, mcr, scr, dll, dlm;
    /* The enhanced bank behind LCR 0xbf: EFR, XON1, XON2, XOFF1, XOFF2. */
    uint8_t efr, xon[4];
    bool fifo, thre, timed_out;
    unsigned trigger;
};

/* FCR bits 7:6; only level 10, which reads 56, is seen in the firmware. */
static const unsigned trigger_levels[4] = { 8, 16, 56, 60 };

static unsigned quart_cause(QuartState *s)
{
    if ((s->ier & IER_RX) && s->rx_len >= s->trigger) {
        return 0x4;
    }
    if ((s->ier & IER_RX) && s->timed_out) {
        return 0xc;
    }
    if ((s->ier & IER_THRE) && s->thre) {
        return 0x2;
    }
    return 0x1;
}

static void quart_update_irq(QuartState *s)
{
    qemu_set_irq(s->irq, quart_cause(s) != 0x1);
}

/* Four character times at the programmed rate, with no byte arriving. */
static void quart_arm_timeout(QuartState *s)
{
    unsigned div = (s->dlm << 8 | s->dll) ?: 1;

    s->timed_out = false;
    if (s->rx_len && s->rx_len < s->trigger) {
        timer_mod(s->timeout, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  muldiv64(40ULL * 16 * div, NANOSECONDS_PER_SECOND,
                           s->clock_hz));
    } else {
        timer_del(s->timeout);
    }
}

static void quart_timeout(void *opaque)
{
    QuartState *s = opaque;

    s->timed_out = s->rx_len && s->rx_len < s->trigger;
    quart_update_irq(s);
}

static int quart_can_receive(void *opaque)
{
    QuartState *s = opaque;

    return s->fifo ? FIFO_SIZE - s->rx_len : !s->rx_len;
}

static void quart_receive(void *opaque, const uint8_t *buf, int size)
{
    QuartState *s = opaque;

    for (int i = 0; i < size && s->rx_len < FIFO_SIZE; i++) {
        s->rx[(s->rx_head + s->rx_len++) % FIFO_SIZE] = buf[i];
    }
    quart_arm_timeout(s);
    quart_update_irq(s);
}

static uint8_t quart_pop(QuartState *s)
{
    uint8_t val = 0;

    if (s->rx_len) {
        val = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % FIFO_SIZE;
        s->rx_len--;
        quart_arm_timeout(s);
        quart_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
    }
    return val;
}

static uint64_t quart_read(void *opaque, hwaddr addr, unsigned size)
{
    QuartState *s = opaque;
    unsigned cause;

    if (s->lcr == LCR_EFR && addr >= 2 && addr != 3) {
        return addr == 2 ? s->efr : s->xon[addr - 4];
    }
    switch (addr) {
    case 0:
        return s->lcr & LCR_DLAB ? s->dll : quart_pop(s);
    case 1:
        return s->lcr & LCR_DLAB ? s->dlm : s->ier;
    case 2:
        cause = quart_cause(s);
        if (cause == 0x2) {
            s->thre = false;
            quart_update_irq(s);
        }
        return cause | (s->fifo ? 0xc0 : 0);
    case 3:
        return s->lcr;
    case 4:
        return s->mcr;
    case 5:
        return LSR_IDLE | (s->rx_len ? LSR_DR : 0);
    case 6:
        return MSR_RESET;
    default:
        return s->scr;
    }
}

static void quart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    QuartState *s = opaque;
    uint8_t byte = val;

    if (s->lcr == LCR_EFR && addr >= 2 && addr != 3) {
        if (addr == 2) {
            s->efr = byte;
        } else {
            s->xon[addr - 4] = byte;
        }
        return;
    }
    switch (addr) {
    case 0:
        if (s->lcr & LCR_DLAB) {
            s->dll = byte;
        } else {
            s->thre = false;
            qemu_chr_fe_write_all(&s->chr, &byte, 1);
            s->thre = s->ier & IER_THRE;
        }
        break;
    case 1:
        if (s->lcr & LCR_DLAB) {
            s->dlm = byte;
        } else {
            /* Enabling the empty holding register's interrupt raises it. */
            s->thre |= !(s->ier & IER_THRE) && (byte & IER_THRE);
            s->ier = byte & 0x0f;
        }
        break;
    case 2:
        s->fifo = byte & 1;
        s->trigger = s->fifo ? trigger_levels[byte >> 6] : 1;
        if (byte & 2 || !s->fifo) {
            s->rx_len = 0;
        }
        quart_arm_timeout(s);
        break;
    case 3:
        s->lcr = byte;
        break;
    case 4:
        s->mcr = byte;
        if (byte & 0x10) {
            qemu_log_mask(LOG_UNIMP, "sgi-l1-quart: loopback is not "
                          "modelled\n");
        }
        break;
    case 7:
        s->scr = byte;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1-quart: write to read-only "
                      "register %u\n", (unsigned)addr);
    }
    quart_update_irq(s);
}

static const MemoryRegionOps quart_ops = {
    .read = quart_read,
    .write = quart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void quart_reset_hold(Object *obj, ResetType type)
{
    QuartState *s = SGI_L1_QUART(obj);

    s->rx_head = s->rx_len = 0;
    s->ier = s->lcr = s->mcr = s->dll = s->dlm = s->efr = 0;
    memset(s->xon, 0, sizeof(s->xon));
    s->fifo = s->timed_out = false;
    s->thre = false;
    s->trigger = 1;
    timer_del(s->timeout);
    quart_update_irq(s);
}

static void quart_realize(DeviceState *dev, Error **errp)
{
    QuartState *s = SGI_L1_QUART(dev);

    s->timeout = timer_new_ns(QEMU_CLOCK_VIRTUAL, quart_timeout, s);
    qemu_chr_fe_set_handlers(&s->chr, quart_can_receive, quart_receive, NULL,
                             NULL, s, NULL, true);
}

static void quart_init(Object *obj)
{
    QuartState *s = SGI_L1_QUART(obj);

    memory_region_init_io(&s->mmio, obj, &quart_ops, s, "sgi-l1-quart", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property quart_props[] = {
    DEFINE_PROP_CHR("chardev", QuartState, chr),
    DEFINE_PROP_UINT32("clock-hz", QuartState, clock_hz, 12000000),
};

static void quart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI L1 board four-channel UART, one channel";
    dc->realize = quart_realize;
    device_class_set_props(dc, quart_props);
    RESETTABLE_CLASS(oc)->phases.hold = quart_reset_hold;
}

static const TypeInfo quart_types[] = {
    {
        .name          = TYPE_SGI_L1_QUART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(QuartState),
        .instance_init = quart_init,
        .class_init    = quart_class_init,
    },
};

DEFINE_TYPES(quart_types)
