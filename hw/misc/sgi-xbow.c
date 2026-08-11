/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow — the crosstalk crossbar, as far as a guest has to see it.
 *
 * The chip is an eight-port switch that routes crosstalk packets by the widget
 * number they name, and its own registers answer as widget 0. It translates
 * nothing and reorders nothing, so on a machine with one node board and one I/O
 * bridge it is a point-to-point link with a register file on it, and a guest
 * that never read those registers would not notice its absence.
 *
 * Except that both guests do read them, first, and decide what kind of machine
 * this is from the answer. That is the whole of what this model owes them
 * today: widget 0 has to answer its identification register, and answer with
 * the right identity. An Origin 200 does not report a crossbar there — it
 * reports the identity of the I/O bridge — and a guest reading a crossbar's own
 * part number instead takes a different probe path, consults the configuration
 * graph differently, and prints different things. So the identity is a property
 * the machine sets rather than a constant here, and this file stays the
 * Crossbow's, which is where the rest of the switch goes when a machine that
 * has one needs it.
 *
 * Everything not implemented raises a bus error rather than answering zero. A
 * register file that answers plausibly for registers nobody wrote is how a
 * missing mechanism stays invisible until a guest fails somewhere unrelated.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

/*
 * Every register in this chip is 32 bits sitting in the low half of an
 * eight-byte slot, so on this big-endian machine the data is at the slot's
 * offset plus four and that is the address the guests' own headers name. The
 * upper half of the slot reads zero.
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
 * Which addresses this model answers. The chip has many more registers, and a
 * guest asking for one of those is asking a real question we have not answered,
 * so it is told rather than served a zero.
 *
 * The upper half of a register's eight-byte slot is not on this list, and that
 * is deliberate rather than an omission. It holds no register — the data is in
 * the low half — so a model returning zero there would be answering for an
 * address the chip has nothing at, which is the one thing a device here may not
 * do. It is also the address a reader gets by forgetting the plus-four, so
 * making it faulty rather than plausible is what turns that mistake into a
 * message instead of a zero.
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
    /*
     * The identification register is read-only, and this chip treats a write to
     * a read-only register as an error condition rather than as something to
     * ignore quietly.
     */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbow: write of 0x%" PRIx64 " to the read-only "
                  "identification register at 0x%" HWADDR_PRIx "\n", val, addr);
}

/*
 * The chip's own rule, and it is stricter than a model would otherwise be:
 * every register is reached by a crosstalk double-word packet, so an access of
 * any other width is an error condition rather than an access to serve. Both
 * the maximum and the minimum are declared, because leaving either unset makes
 * the memory system reshape an access rather than reject it.
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

/*
 * Nothing in this model changes after reset, so there is no state to carry. The
 * description exists anyway, because a device without one silently drops out of
 * a snapshot and the next field added here would go missing without a word.
 */
static const VMStateDescription vmstate_sgi_xbow = {
    .name = "sgi-xbow",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The defaults are a real Crossbow's own identity, which is counter-intuitive
 * and correct: the chip reports part number zero and manufacturer zero, and a
 * model that invented a plausible non-zero part would be read as an unknown
 * widget. A machine whose widget 0 is not a Crossbow says so by setting these.
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
