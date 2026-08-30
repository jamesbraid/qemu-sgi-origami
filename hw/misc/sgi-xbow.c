/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow crosstalk crossbar.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * An eight-port switch routing crosstalk packets by widget number, its own
 * registers answering as widget 0. Only the identification register is
 * implemented; guests read it first and decide the machine type from the
 * answer. The identity is a machine property, not a constant: an Origin 200
 * reports the I/O bridge's identity at widget 0, not a crossbar's.
 * Unimplemented registers raise a bus error rather than answering zero.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

/*
 * Registers are 32 bits in the low half of an eight-byte slot: the data sits
 * at the slot's offset plus four, the address the guests' own headers name.
 */
#define XBOW_IDENTIFICATION     0x0000
#define XBOW_REG_DATA           4

/* Identification: revision, part, manufacturer, and a bit that is set. */
#define XBOW_ID_REVISION_SHIFT  28
#define XBOW_ID_PART_SHIFT      12
#define XBOW_ID_MANUFACTURER_SHIFT 1
#define XBOW_ID_ONE             1

static uint32_t sgi_xbow_identification(SGIXbowState *s)
{
    return (s->revision << XBOW_ID_REVISION_SHIFT) |
           (s->part << XBOW_ID_PART_SHIFT) |
           (s->manufacturer << XBOW_ID_MANUFACTURER_SHIFT) |
           XBOW_ID_ONE;
}

/*
 * A slot's upper half holds no register, so it faults like any unimplemented
 * address -- which also catches a reader who forgot the plus-four.
 */
static bool sgi_xbow_implemented(hwaddr addr)
{
    return addr == XBOW_IDENTIFICATION + XBOW_REG_DATA;
}

static bool sgi_xbow_accepts(void *opaque, hwaddr addr, unsigned size,
                             bool is_write, MemTxAttrs attrs)
{
    if (!sgi_xbow_implemented(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbow: %s of register 0x%" HWADDR_PRIx " which this "
                      "model does not implement; raising a bus error rather "
                      "than answering\n", is_write ? "write" : "read", addr);
        return false;
    }
    return true;
}

static uint64_t sgi_xbow_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIXbowState *s = opaque;

    switch (addr) {
    case XBOW_IDENTIFICATION + XBOW_REG_DATA:
        return sgi_xbow_identification(s);
    default:
        /* Everything else was refused before it reached here. */
        g_assert_not_reached();
    }
}

static void sgi_xbow_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    /* The chip errors on a write to its read-only register. */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbow: write of 0x%" PRIx64 " to the read-only "
                  "identification register at 0x%" HWADDR_PRIx "\n", val, addr);
}

/*
 * Every register access is a crosstalk double-word; any other width is an
 * error. Both bounds are declared so the core rejects instead of reshaping.
 */
static const MemoryRegionOps sgi_xbow_ops = {
    .read = sgi_xbow_read,
    .write = sgi_xbow_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.accepts = sgi_xbow_accepts,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_xbow_realize(DeviceState *dev, Error **errp)
{
    SGIXbowState *s = SGI_XBOW(dev);

    if (!s->span) {
        error_setg(errp, "the crossbar needs the size of the crosstalk space "
                         "its widget owns");
        return;
    }

    memory_region_init_io(&s->regs, OBJECT(dev), &sgi_xbow_ops, s,
                          "sgi-xbow.regs", s->span);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
}

/* No state to carry yet; the description keeps the device in snapshots. */
static const VMStateDescription vmstate_sgi_xbow = {
    .name = "sgi-xbow",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

/*
 * A real Crossbow reports part 0, manufacturer 0: zero is the identity,
 * not an unset default. Machines override these for a different widget 0.
 */
static const Property sgi_xbow_properties[] = {
    DEFINE_PROP_UINT64("span", SGIXbowState, span, 0),
    DEFINE_PROP_UINT32("revision", SGIXbowState, revision, 1),
    DEFINE_PROP_UINT32("part", SGIXbowState, part, 0),
    DEFINE_PROP_UINT32("manufacturer", SGIXbowState, manufacturer, 0),
};

static void sgi_xbow_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI Crossbow crosstalk crossbar";
    dc->realize = sgi_xbow_realize;
    dc->vmsd = &vmstate_sgi_xbow;
    device_class_set_props(dc, sgi_xbow_properties);
    /* The span is board wiring, so -device could only fail at realize. */
    dc->user_creatable = false;
}

static const TypeInfo sgi_xbow_types[] = {
    {
        .name = TYPE_SGI_XBOW,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIXbowState),
        .class_init = sgi_xbow_class_init,
    },
};

DEFINE_TYPES(sgi_xbow_types)
