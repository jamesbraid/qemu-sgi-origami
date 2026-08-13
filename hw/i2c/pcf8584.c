/*
 * Philips PCF8584 I2C-bus controller
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A third-party bus controller with a public interface, modelled from the
 * register contract in docs/datasheets/pcf8584-elsc.md.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/pcf8584.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/*
 * Which register the A0-low address selects. ESO picks between the working
 * data register and the three configuration registers, but ES2 keeps
 * selecting either way: with the serial interface enabled it still chooses
 * between the data register and the interrupt vector, and only ES1 stops
 * meaning anything. Getting that asymmetry wrong is invisible against
 * firmware that never sets ES2 while running, which is the reason to model it
 * rather than collapse it.
 */
typedef enum {
    PCF8584_REG_OWN_ADDRESS,
    PCF8584_REG_CLOCK,
    PCF8584_REG_VECTOR,
    PCF8584_REG_DATA,
} PCF8584Reg;

static PCF8584Reg pcf8584_selected(PCF8584State *s)
{
    if ((s->control & PCF8584_CTL_ES2) && !(s->control & PCF8584_CTL_ES1)) {
        return PCF8584_REG_VECTOR;
    }
    if (s->control & PCF8584_CTL_ESO) {
        return (s->control & PCF8584_CTL_ES2) ? PCF8584_REG_VECTOR
                                              : PCF8584_REG_DATA;
    }
    if (s->control & PCF8584_CTL_ES1) {
        return PCF8584_REG_CLOCK;
    }
    return PCF8584_REG_OWN_ADDRESS;
}

static void pcf8584_update_irq(PCF8584State *s)
{
    /*
     * The interrupt follows the pending-interrupt bit, which is active low in
     * the chip's own naming: PIN clear means an operation completed. No
     * firmware this machine runs enables it, so this exists to keep the line
     * honest rather than because anything drives it today.
     */
    bool level = (s->control & PCF8584_CTL_ENI) &&
                 !(s->status & PCF8584_STA_PIN);

    qemu_set_irq(s->irq, level);
}

/* Mark the byte transfer finished, recording whether the far end acked. */
static void pcf8584_complete(PCF8584State *s, bool nacked)
{
    s->status &= ~PCF8584_STA_PIN;
    if (nacked) {
        s->status |= PCF8584_STA_LRB;
    } else {
        s->status &= ~PCF8584_STA_LRB;
    }
    pcf8584_update_irq(s);
}

static void pcf8584_stop(PCF8584State *s)
{
    if (s->transfer) {
        i2c_end_transfer(s->bus);
    }
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    s->status |= PCF8584_STA_BNB;
}

/*
 * Issue a start condition using addr as the address-and-direction byte. Used
 * for a first start, where the address reached the data register before the
 * control write, and for a repeated start, where it arrives after it.
 */
static void pcf8584_start(PCF8584State *s, uint8_t addr)
{
    bool recv = addr & 1;
    int rc;

    s->recv = recv;
    s->status &= ~PCF8584_STA_BNB;

    rc = i2c_start_transfer(s->bus, addr >> 1, recv);
    s->transfer = true;

    if (rc) {
        /*
         * Nobody answered. This is a normal outcome rather than an error: the
         * firmware deliberately addresses an invalid address after every
         * completed transaction, precisely to be refused.
         */
        pcf8584_complete(s, true);
        return;
    }

    /*
     * No fetch here even when recv is set. The Philips datasheet attaches
     * the transfer to the *read* of the data register, not to the start:
     * the driver's first read after a read-direction address returns
     * whatever the register already holds -- the address-and-direction
     * byte just written to generate this start -- and that read is what
     * moves the first real byte in. pcf8584_read_data() already implements
     * exactly that pattern (hold, then fetch); the register simply keeps
     * its current value here rather than being written to.
     */
    pcf8584_complete(s, false);
}

static void pcf8584_write_data(PCF8584State *s, uint8_t val)
{
    int rc;

    if (s->start_pending) {
        s->start_pending = false;
        pcf8584_start(s, val);
        return;
    }

    if (!s->transfer) {
        /*
         * Not an error: this is the address byte being staged ahead of the
         * control write that will generate the start.
         */
        s->data = val;
        return;
    }

    if (s->recv) {
        qemu_log_mask(LOG_GUEST_ERROR, "pcf8584: data written during a "
                      "receive transfer, ignored\n");
        return;
    }

    s->status |= PCF8584_STA_PIN;
    rc = i2c_send(s->bus, val);
    s->data = val;
    pcf8584_complete(s, rc != 0);
}

static uint8_t pcf8584_read_data(PCF8584State *s)
{
    uint8_t val = s->data;

    if (s->transfer && s->recv) {
        /*
         * Hand back the byte already held, then fetch the next one. A model
         * that fetches first and returns the fetched byte is off by one for
         * the whole packet, and the symptom reads as a protocol fault rather
         * than an off-by-one.
         */
        if (!(s->control & PCF8584_CTL_ACK)) {
            /* ACK cleared means the driver wants this to be the last byte. */
            i2c_nack(s->bus);
        }
        s->data = i2c_recv(s->bus);
        pcf8584_complete(s, false);
    }

    return val;
}

static void pcf8584_write_control(PCF8584State *s, uint8_t val)
{
    uint8_t prev = s->control;

    s->control = val;

    if (val & PCF8584_CTL_PIN) {
        /*
         * PIN set by the host is the reset and idle request: the chip reports
         * itself busy until an operation completes. It is written on its own
         * to select a configuration register, so it must not disturb a
         * transfer by itself.
         */
        s->status |= PCF8584_STA_PIN;
        s->status &= ~(PCF8584_STA_BER | PCF8584_STA_LAB | PCF8584_STA_AAS);
    }

    if (val & PCF8584_CTL_STA) {
        if (s->transfer) {
            /* Repeated start: the address byte has not been written yet. */
            s->start_pending = true;
            s->status |= PCF8584_STA_PIN;
        } else {
            /* First start: the address is already staged in the register. */
            s->status |= PCF8584_STA_PIN;
            pcf8584_start(s, s->data);
        }
        return;
    }

    if (val & PCF8584_CTL_STO) {
        pcf8584_stop(s);
        s->status |= PCF8584_STA_PIN;
        pcf8584_update_irq(s);
        return;
    }

    if ((prev ^ val) & PCF8584_CTL_ENI) {
        pcf8584_update_irq(s);
    }
}

static uint64_t pcf8584_read(void *opaque, hwaddr addr, unsigned size)
{
    PCF8584State *s = PCF8584(opaque);

    if (addr >= PCF8584_A0_STRIDE) {
        /*
         * With the serial interface disabled this address is write-only in the
         * chip's own documentation; real parts still answer a read with the
         * control bits plus the pending-interrupt state, and answering is more
         * useful to a guest than inventing a value.
         */
        if (s->control & PCF8584_CTL_ESO) {
            return s->status;
        }
        return (s->control & ~PCF8584_STA_PIN) | (s->status & PCF8584_STA_PIN);
    }

    switch (pcf8584_selected(s)) {
    case PCF8584_REG_OWN_ADDRESS:
        return s->own_address;
    case PCF8584_REG_CLOCK:
        return s->clock;
    case PCF8584_REG_VECTOR:
        return s->vector;
    case PCF8584_REG_DATA:
        return pcf8584_read_data(s);
    default:
        g_assert_not_reached();
    }
}

static void pcf8584_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    PCF8584State *s = PCF8584(opaque);

    if (addr >= PCF8584_A0_STRIDE) {
        pcf8584_write_control(s, val & 0xff);
        return;
    }

    switch (pcf8584_selected(s)) {
    case PCF8584_REG_OWN_ADDRESS:
        /*
         * Seven bits, right-justified: the chip shifts this left by one onto
         * the wire. Stored as written so that firmware's own read-back
         * liveness check sees exactly what it wrote.
         */
        s->own_address = val & 0x7f;
        break;
    case PCF8584_REG_CLOCK:
        s->clock = val & 0x1f;
        break;
    case PCF8584_REG_VECTOR:
        s->vector = val & 0xff;
        break;
    case PCF8584_REG_DATA:
        pcf8584_write_data(s, val & 0xff);
        break;
    default:
        g_assert_not_reached();
    }
}

/*
 * The data register's read has a side effect: it advances the receive
 * pipeline. So impl is declared rather than left to default, which would cap
 * it at four bytes and split the eight-byte accesses this machine's firmware
 * issues into two calls, running that side effect twice with nothing in the
 * result to show for it.
 */
static const MemoryRegionOps pcf8584_ops = {
    .read = pcf8584_read,
    .write = pcf8584_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The system controller reaches every node's chip the way a real
 * multi-master I2C bus has to: it addresses nobody and broadcasts. QEMU's
 * I2C core already delivers a broadcast to every child of the bus a
 * transfer's address names as the broadcast address (hw/i2c/core.c's default
 * match_and_add adds any slave once broadcast is set, regardless of its own
 * address), so this chip only needs a slave-side receiver sitting on its own
 * bus to catch one. The receiver carries no state of its own to migrate --
 * the byte it captures is written straight into the chip's data register,
 * which is already part of the chip's own vmstate.
 */
#define TYPE_PCF8584_BCAST_RX "pcf8584-broadcast-rx"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8584BcastRxState, PCF8584_BCAST_RX)

struct PCF8584BcastRxState {
    I2CSlave parent_obj;
    PCF8584State *chip;
};

static int pcf8584_bcast_rx_send(I2CSlave *dev, uint8_t data)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(dev);

    rx->chip->data = data;
    return 0;
}

static void pcf8584_bcast_rx_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->send = pcf8584_bcast_rx_send;
}

static const TypeInfo pcf8584_bcast_rx_type = {
    .name          = TYPE_PCF8584_BCAST_RX,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCF8584BcastRxState),
    .class_init    = pcf8584_bcast_rx_class_init,
};

static void pcf8584_reset_hold(Object *obj, ResetType type)
{
    PCF8584State *s = PCF8584(obj);

    s->own_address = 0;
    s->clock = 0;
    s->vector = 0;
    s->data = 0;
    s->control = 0;
    /* Out of reset the chip is idle and the bus is free. */
    s->status = PCF8584_STA_PIN | PCF8584_STA_BNB;
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    pcf8584_update_irq(s);
}

static void pcf8584_realize(DeviceState *dev, Error **errp)
{
    PCF8584State *s = PCF8584(dev);
    PCF8584BcastRxState *rx;

    memory_region_init_io(&s->iomem, OBJECT(dev), &pcf8584_ops, s,
                          "pcf8584", PCF8584_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->bus = i2c_init_bus(dev, "i2c");

    rx = PCF8584_BCAST_RX(i2c_slave_create_simple(s->bus,
                                                   TYPE_PCF8584_BCAST_RX, 0));
    rx->chip = s;
}

static const VMStateDescription vmstate_pcf8584 = {
    .name = "pcf8584",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(own_address, PCF8584State),
        VMSTATE_UINT8(clock, PCF8584State),
        VMSTATE_UINT8(vector, PCF8584State),
        VMSTATE_UINT8(data, PCF8584State),
        VMSTATE_UINT8(control, PCF8584State),
        VMSTATE_UINT8(status, PCF8584State),
        VMSTATE_BOOL(transfer, PCF8584State),
        VMSTATE_BOOL(recv, PCF8584State),
        VMSTATE_BOOL(start_pending, PCF8584State),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf8584_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Philips PCF8584 I2C-bus controller";
    dc->realize = pcf8584_realize;
    dc->vmsd = &vmstate_pcf8584;
    rc->phases.hold = pcf8584_reset_hold;
}

static const TypeInfo pcf8584_types[] = {
    {
        .name          = TYPE_PCF8584,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PCF8584State),
        .class_init    = pcf8584_class_init,
    },
};

DEFINE_TYPES(pcf8584_types)

static void pcf8584_bcast_rx_register(void)
{
    type_register_static(&pcf8584_bcast_rx_type);
}

type_init(pcf8584_bcast_rx_register)
