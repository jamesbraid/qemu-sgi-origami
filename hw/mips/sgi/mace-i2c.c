/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * MACE revision-2 private untimed I2C profile. DATA completes one synchronous
 * endpoint byte; CONTROL never prefetches. This functional approximation has
 * no repeated-START request, wire timing, active line sampling or overrides.
 * Ordinary O2 admission and production endpoint wiring remain unqualified.
 */
#include "qemu/osdep.h"
#include "mace-i2c.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define CONFIG_RESET 1
#define CONFIG_FAST 2
#define CONFIG_OVERRIDE 0x0c
#define CONFIG_RELEASED 0x30
#define CONTROL_NONIDLE 1
#define CONTROL_RECEIVE 2
#define CONTROL_HOLD 4
#define STATUS_BUSY 0x10
#define STATUS_NACK 0x20
#define STATUS_ERROR 0x80

static int mace_i2c_stop(MACEI2C *s)
{
    int ret = 0;

    if (s->owned) {
        if (s->pending_ack) {
            s->pending_ack = false;
            ret = i2c_master_nack(s->bus, 0);
        }
        if (i2c_master_end_transfer(s->bus, 0) < 0) {
            ret = -EPERM;
        }
    }
    s->owned = false;
    s->address_acked = false;
    s->receive = false;
    s->address = 0;
    return ret;
}

void mace_i2c_reset(MACEI2C *s, bool cold)
{
    if (!s->enabled) {
        return;
    }
    if (s->busy) {
        s->reset_pending = true;
        s->cold_pending |= cold;
        return;
    }
    s->busy = true;
    if (mace_i2c_stop(s) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace-i2c: reset ownership failure\n");
    }
    s->control = s->data = 0;
    s->nack = s->error = false;
    if (cold || s->cold_pending) {
        s->config = 0;
    }
    s->reset_pending = s->cold_pending = s->busy = false;
}

static void mace_i2c_boundary(MACEI2C *s)
{
    s->busy = false;
    if (s->reset_pending) {
        bool cold = s->cold_pending;

        s->reset_pending = s->cold_pending = false;
        mace_i2c_reset(s, cold);
    }
}

static MemTxResult mace_i2c_failure(MACEI2C *s, const char *reason, int ret)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mace-i2c: %s (%d)\n", reason, ret);
    s->error = true;
    return MEMTX_ERROR;
}

static MemTxResult mace_i2c_misuse(MACEI2C *s)
{
    s->busy = true;
    mace_i2c_stop(s);
    mace_i2c_failure(s, "DATA protocol misuse", -EINVAL);
    mace_i2c_boundary(s);
    return MEMTX_ERROR;
}

static MemTxResult mace_i2c_data(MACEI2C *s, bool read, uint8_t *byte)
{
    bool hold = s->control & CONTROL_HOLD;
    int ret;

    if (s->error) {
        return mace_i2c_failure(s, "DATA blocked by sticky error", -EIO);
    }
    if (i2c_bus_validate_untimed_private(s->bus, NULL) < 0) {
        return mace_i2c_failure(s, "incompatible untimed endpoint", -ENOTSUP);
    }
    if (read != !!(s->control & CONTROL_RECEIVE) ||
        (read && (!s->owned || !s->address_acked || !s->receive ||
                  !s->address)) ||
        (!read && s->owned && (!s->address_acked || s->receive))) {
        return mace_i2c_misuse(s);
    }
    s->busy = true;
    if (read) {
        /* Continuing the receive implicitly ACKs the preceding byte. */
        s->pending_ack = false;
        ret = i2c_master_recv(s->bus, 0, byte);
        if (!ret) {
            s->pending_ack = true;
        }
    } else if (!s->owned) {
        ret = i2c_bus_start_resolved(s->bus, 0);
        if (!ret) {
            s->owned = true;
            s->address = *byte >> 1;
            s->receive = *byte & 1;
            ret = i2c_master_start_transfer(s->bus, 0, s->address, s->receive);
            s->address_acked = ret == 0;
        }
    } else {
        ret = i2c_master_send(s->bus, 0, *byte);
    }
    if (ret < 0) {
        mace_i2c_failure(s, ret == -EINVAL ? "DATA protocol misuse" :
                                          "byte API failure", ret);
        mace_i2c_stop(s);
    } else {
        s->data = *byte;
        if (!read) {
            s->nack = ret != 0;
        }
        if (!hold && mace_i2c_stop(s) < 0) {
            ret = -EPERM;
            mace_i2c_failure(s, "STOP ownership failure", ret);
        }
    }
    mace_i2c_boundary(s);
    return ret < 0 ? MEMTX_ERROR : MEMTX_OK;
}

static bool mace_i2c_valid(void *opaque, hwaddr offset, unsigned size,
                            bool write, MemTxAttrs attrs)
{
    return size == 8 && !(offset & 7) && offset <= 0x18;
}

static MemTxResult mace_i2c_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    MACEI2C *s = opaque;
    uint8_t byte;
    MemTxResult ret;

    if (!s->enabled) {
        return MEMTX_ERROR;
    }
    switch (offset) {
    case 0:
    case 8:
        if (s->owned || s->busy || i2c_bus_busy(s->bus) ||
            i2c_bus_validate_untimed_private(s->bus, NULL) < 0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-i2c: sampled inputs unavailable\n");
            return MEMTX_ERROR;
        }
        *value = s->config | CONFIG_RELEASED;
        return MEMTX_OK;
    case 0x10:
        *value = s->control | (s->owned ? CONTROL_NONIDLE : 0) |
                 (s->busy ? STATUS_BUSY : 0) | (s->nack ? STATUS_NACK : 0) |
                 (s->error ? STATUS_ERROR : 0);
        return MEMTX_OK;
    case 0x18:
        ret = mace_i2c_data(s, true, &byte);
        if (ret == MEMTX_OK) {
            *value = byte;
        }
        return ret;
    default:
        g_assert_not_reached();
    }
}

static MemTxResult mace_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    MACEI2C *s = opaque;
    uint8_t byte = value;
    bool reset;
    int ret;

    if (!s->enabled) {
        return MEMTX_ERROR;
    }
    switch (offset) {
    case 0:
    case 8:
        if (byte & CONFIG_OVERRIDE) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace-i2c: physical override unsupported\n");
            s->error = true;
            return MEMTX_ERROR;
        }
        reset = !(s->config & CONFIG_RESET) && (byte & CONFIG_RESET);
        s->config = byte & (CONFIG_RESET | CONFIG_FAST);
        if (reset) {
            mace_i2c_reset(s, false);
        }
        return MEMTX_OK;
    case 0x10:
        s->control = byte & (CONTROL_RECEIVE | CONTROL_HOLD);
        if (!(byte & CONTROL_NONIDLE) ||
            (s->pending_ack && (byte & CONTROL_RECEIVE) &&
             !(byte & CONTROL_HOLD))) {
            s->busy = true;
            ret = mace_i2c_stop(s);
            if (ret < 0) {
                mace_i2c_failure(s, "force-idle ownership failure", ret);
            }
            mace_i2c_boundary(s);
            return ret < 0 ? MEMTX_ERROR : MEMTX_OK;
        }
        return MEMTX_OK;
    case 0x18:
        return mace_i2c_data(s, false, &byte);
    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps mace_i2c_ops = {
    .read_with_attrs = mace_i2c_read,
    .write_with_attrs = mace_i2c_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8,
        .accepts = mace_i2c_valid,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static void mace_i2c_realize(DeviceState *dev, Error **errp)
{
    MACEI2C *s = SGI_MACE_I2C(dev);

    s->bus = i2c_init_bus_type(TYPE_I2C_WIRE, dev, "i2c");
    if (i2c_bus_enable_untimed_private(s->bus, errp) < 0) {
        return;
    }
    s->enabled = 1;
}

void mace_i2c_prepare_unrealize(MACEI2C *s, Error **errp)
{
    if (!s->enabled) {
        return;
    }
    if (s->busy) {
        error_setg(errp, "MACE I2C cannot unrealize during a byte callback");
        return;
    }
    mace_i2c_reset(s, true);
    s->enabled = 0;
}

static int mace_i2c_state_valid(void *opaque)
{
    MACEI2C *s = opaque;
    I2cWire *bus;

    if (!s->enabled) {
        return 0;
    }
    if (s->busy || s->reset_pending || s->cold_pending ||
        (s->config & ~3) || (s->control & ~6) || s->address > 0x7f ||
        (s->pending_ack && (!s->owned || !s->receive || !s->address_acked)) ||
        (!s->owned && (s->address || s->receive || s->address_acked)) ||
        i2c_bus_validate_untimed_private(s->bus, NULL) < 0) {
        return -EINVAL;
    }
    bus = I2C_WIRE(s->bus);
    if (s->owned != (bus->owner == 0) ||
        s->owned != bus->address_valid ||
        (s->owned && (s->address != bus->requested_address ||
                      s->receive != bus->master_recv)) ||
        s->address_acked != !QLIST_EMPTY(&s->bus->current_devs)) {
        return -EINVAL;
    }
    return 0;
}

static int mace_i2c_post_load(void *opaque, int version)
{
    return mace_i2c_state_valid(opaque);
}

static const VMStateDescription vmstate_mace_i2c = {
    .name = "sgi-mace-i2c-untimed",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mace_i2c_state_valid,
    .post_load = mace_i2c_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(enabled, MACEI2C),
        VMSTATE_UINT8(config, MACEI2C),
        VMSTATE_UINT8(control, MACEI2C),
        VMSTATE_UINT8(data, MACEI2C),
        VMSTATE_UINT8(address, MACEI2C),
        VMSTATE_BOOL(owned, MACEI2C),
        VMSTATE_BOOL(address_acked, MACEI2C),
        VMSTATE_BOOL(receive, MACEI2C),
        VMSTATE_BOOL(nack, MACEI2C),
        VMSTATE_BOOL(error, MACEI2C),
        VMSTATE_BOOL(pending_ack, MACEI2C),
        VMSTATE_END_OF_LIST()
    },
};

static void mace_i2c_instance_init(Object *obj)
{
    MACEI2C *s = SGI_MACE_I2C(obj);

    memory_region_init_io(&s->regs, obj, &mace_i2c_ops, s,
                          "sgi-mace-i2c-untimed", 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->regs);
}

static void mace_i2c_pre_unrealize(DeviceState *dev, Error **errp)
{
    mace_i2c_prepare_unrealize(SGI_MACE_I2C(dev), errp);
}

static void mace_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->user_creatable = false;
    dc->realize = mace_i2c_realize;
    dc->pre_unrealize = mace_i2c_pre_unrealize;
    dc->vmsd = &vmstate_mace_i2c;
}

static const TypeInfo mace_i2c_type = {
    .name = TYPE_SGI_MACE_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACEI2C),
    .instance_init = mace_i2c_instance_init,
    .class_init = mace_i2c_class_init,
};

static void mace_i2c_register_types(void)
{
    type_register_static(&mace_i2c_type);
}
type_init(mace_i2c_register_types)
