/*
 * Philips PCF8584 I2C-bus controller
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Modelled from the register contract in the Origami project tree's
 * controller datasheet; see also docs/specs/sgi-elsc.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/pcf8584.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"

/*
 * Which register the A0-low address selects. ES2 keeps selecting even with
 * the serial interface enabled; only ES1 stops meaning anything.
 */
typedef enum {
    PCF8584_REG_OWN_ADDRESS,
    PCF8584_REG_CLOCK,
    PCF8584_REG_VECTOR,
    PCF8584_REG_DATA,
} PCF8584Reg;

static uint64_t pcf8584_diagnostic_state(PCF8584State *s)
{
    return ((uint64_t)s->control << 56) |
           ((uint64_t)s->status << 48) |
           ((uint64_t)s->data << 40) |
           ((uint64_t)s->cur_addr7 << 32) |
           ((uint64_t)s->transfer << 3) |
           ((uint64_t)s->recv << 2) |
           ((uint64_t)s->start_pending << 1) |
           s->start_address_staged;
}

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
    /* PIN is active low in the chip's naming: clear means completed. */
    bool level = (s->control & PCF8584_CTL_ENI) &&
                 !(s->status & PCF8584_STS_PIN);

    qemu_set_irq(s->irq, level);
}

/* Mark the byte transfer finished, recording whether the far end acked. */
static void pcf8584_complete(PCF8584State *s, bool nacked)
{
    s->status &= ~PCF8584_STS_PIN;
    if (nacked) {
        s->status |= PCF8584_STS_LRB;
    } else {
        s->status &= ~PCF8584_STS_LRB;
    }
    pcf8584_update_irq(s);
}

static void pcf8584_stop(PCF8584State *s)
{
    trace_pcf8584_stop();
    if (s->transfer) {
        i2c_end_transfer(s->bus);
    }
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    s->start_address_staged = false;
    s->status |= PCF8584_STS_BB;
}

/* Issue a start with addr as the address-and-direction byte. */
static void pcf8584_start(PCF8584State *s, uint8_t addr)
{
    bool recv = addr & 1;
    uint8_t addr7 = addr >> 1;
    int rc;

    s->start_address_staged = false;

    /*
     * The core rescans only when nothing is matched, so an address change
     * must end the old match here; unconditionally would deliver I2C_FINISH
     * to a slave still mid-command.
     */
    if (s->transfer && addr7 != s->cur_addr7) {
        i2c_end_transfer(s->bus);
        s->transfer = false;
    }
    s->cur_addr7 = addr7;

    s->recv = recv;
    s->status &= ~PCF8584_STS_BB;

    rc = i2c_start_transfer(s->bus, addr7, recv);
    if (rc && s->transfer) {
        /*
         * A refused same-address repeated start is left open by the core for
         * the caller to end; held open it keeps i2c_bus_busy() true forever.
         */
        i2c_end_transfer(s->bus);
    }
    s->transfer = rc == 0;
    trace_pcf8584_start(addr7, recv, rc == 0);

    if (rc) {
        /* Normal: firmware probes an invalid address in order to be refused. */
        pcf8584_complete(s, true);
        return;
    }

    /* No fetch for recv: the transfer attaches to the read, not the start. */
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

    if (s->status & PCF8584_STS_BB) {
        /* Bus free: the address byte staged ahead of the control write. */
        s->data = val;
        s->start_address_staged = true;
        return;
    }

    if (s->recv) {
        qemu_log_mask(LOG_GUEST_ERROR, "pcf8584: data written during a "
                      "receive transfer, ignored\n");
        return;
    }

    s->status |= PCF8584_STS_PIN;
    if (s->transfer) {
        rc = i2c_send(s->bus, val);
    } else {
        /*
         * The start was refused. i2c_send() over the core's empty matched
         * list would report success, so the refusal is remembered instead.
         */
        rc = -1;
    }
    trace_pcf8584_send(val, rc == 0);
    s->data = val;
    pcf8584_complete(s, rc != 0);
}

static uint8_t pcf8584_read_data(PCF8584State *s)
{
    uint8_t val = s->data;

    if (s->transfer && s->recv) {
        /* Held byte first, then fetch: fetch-first is off by one all packet. */
        if (!(s->control & PCF8584_CTL_ACK)) {
            /* ACK cleared means the driver wants this to be the last byte. */
            trace_pcf8584_nack();
            i2c_nack(s->bus);
        }
        s->data = i2c_recv(s->bus);
        trace_pcf8584_recv(s->data);
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
         * PIN from the host is the idle request. It is also written alone to
         * select a configuration register, so it must not disturb a transfer.
         */
        s->status |= PCF8584_STS_PIN;
        s->status &= ~(PCF8584_STS_BER | PCF8584_STS_LAB | PCF8584_STS_AAS);
    }

    if (val & PCF8584_CTL_STA) {
        if (!(s->status & PCF8584_STS_BB)) {
            /* Repeated start: the address byte has not been written yet. */
            s->start_pending = true;
            s->status |= PCF8584_STS_PIN;
        } else {
            /* First start: the address is already staged in the register. */
            s->status |= PCF8584_STS_PIN;
            pcf8584_start(s, s->data);
        }
        return;
    }

    if (val & PCF8584_CTL_STO) {
        pcf8584_stop(s);
        s->status |= PCF8584_STS_PIN;
        pcf8584_update_irq(s);
        return;
    }

    if ((prev ^ val) & PCF8584_CTL_ENI) {
        pcf8584_update_irq(s);
    }
}

static uint64_t pcf8584_read_internal(void *opaque, hwaddr addr,
                                      unsigned size)
{
    PCF8584State *s = PCF8584(opaque);

    if (addr >= PCF8584_A0_STRIDE) {
        /*
         * Write-only in the documentation with the serial interface off;
         * real parts answer with the control bits plus the PIN state.
         */
        if (s->control & PCF8584_CTL_ESO) {
            return s->status;
        }
        return (s->control & ~PCF8584_STS_PIN) | (s->status & PCF8584_STS_PIN);
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

static void pcf8584_write_internal(void *opaque, hwaddr addr, uint64_t val,
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
         * Seven bits, right-justified; stored as written so the firmware's
         * read-back liveness check sees what it wrote.
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

static uint64_t pcf8584_read(void *opaque, hwaddr addr, unsigned size)
{
    PCF8584State *s = PCF8584(opaque);
    uint64_t before;
    uint64_t value;
    uint64_t after;

    if (likely(!s->diagnostic)) {
        value = pcf8584_read_internal(opaque, addr, size);
        trace_pcf8584_read(addr, value, size);
        return value;
    }

    before = pcf8584_diagnostic_state(s);
    value = pcf8584_read_internal(opaque, addr, size);
    after = pcf8584_diagnostic_state(s);

    trace_pcf8584_read(addr, value, size);
    device_diagnostic_record(s->diagnostic, "pcf8584", "mmio-read",
                             addr, value, size, true, before, after);
    return value;
}

static void pcf8584_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    PCF8584State *s = PCF8584(opaque);
    uint64_t before;
    uint64_t after;

    trace_pcf8584_write(addr, val, size);

    if (likely(!s->diagnostic)) {
        pcf8584_write_internal(opaque, addr, val, size);
        return;
    }

    before = pcf8584_diagnostic_state(s);
    pcf8584_write_internal(opaque, addr, val, size);
    after = pcf8584_diagnostic_state(s);
    device_diagnostic_record(s->diagnostic, "pcf8584", "mmio-write",
                             addr, val, size, true, before, after);
}

/*
 * impl.max is 8: the default of 4 splits the firmware's eight-byte accesses
 * and runs the data register's read side effect twice.
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
 * The chip's slave half: catches broadcasts and directed hits on S0',
 * reported through AAS. No state of its own to migrate.
 */
#define TYPE_PCF8584_BCAST_RX "pcf8584-broadcast-rx"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8584BcastRxState, PCF8584_BCAST_RX)

struct PCF8584BcastRxState {
    I2CSlave parent_obj;
    PCF8584State *chip;
};

/* Consulted at match time, so reset and migration never copy it here. */
static bool pcf8584_bcast_rx_match_and_add(I2CSlave *candidate,
                                           uint8_t address, bool broadcast,
                                           I2CNodeList *current_devs)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(candidate);
    I2CNode *node;

    if (!broadcast && address != rx->chip->own_address) {
        return false;
    }

    node = g_new(struct I2CNode, 1);
    node->elt = candidate;
    QLIST_INSERT_HEAD(current_devs, node, next);
    return true;
}

static int pcf8584_bcast_rx_event(I2CSlave *dev, enum i2c_event event)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(dev);

    /* Addressed as slave: a directed hit on S0' or the general call. */
    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        rx->chip->status |= PCF8584_STS_AAS;
    }
    return 0;
}

static int pcf8584_bcast_rx_send(I2CSlave *dev, uint8_t data)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(dev);

    /*
     * A staged first-start address is preserved until START consumes it,
     * or a delayed START would target an arbitration byte.
     */
    if (!rx->chip->start_address_staged) {
        rx->chip->data = data;
    }
    return 0;
}

static void pcf8584_bcast_rx_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->match_and_add = pcf8584_bcast_rx_match_and_add;
    sc->event = pcf8584_bcast_rx_event;
    sc->send = pcf8584_bcast_rx_send;
}

static void pcf8584_reset_hold(Object *obj, ResetType type)
{
    PCF8584State *s = PCF8584(obj);

    s->own_address = 0;
    s->clock = 0;
    s->vector = 0;
    s->data = 0;
    s->control = 0;
    /* Out of reset the chip is idle and the bus is free. */
    s->status = PCF8584_STS_PIN | PCF8584_STS_BB;
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    s->start_address_staged = false;
    s->cur_addr7 = 0;
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
    .version_id = 2,
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
        VMSTATE_BOOL_V(start_address_staged, PCF8584State, 2),
        VMSTATE_UINT8(cur_addr7, PCF8584State),
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
    {
        .name          = TYPE_PCF8584_BCAST_RX,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(PCF8584BcastRxState),
        .class_init    = pcf8584_bcast_rx_class_init,
    },
};

DEFINE_TYPES(pcf8584_types)
