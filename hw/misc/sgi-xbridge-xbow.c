/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * XBridge widget 0: crossbar-style per-port status and control, with one
 * reader-specific MMIO view per port.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/misc/sgi-xbridge-xbow.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qapi/error.h"

#define REG_COUNT SGI_XBRIDGE_XBOW_REG_COUNT
#define REG(s, off) ((s)->reg[(off) / 8])

#define LINK_COUNT SGI_XBRIDGE_XBOW_LINKS
#define LINK_FIRST 8
#define LINK_BASE 0x100
#define LINK_STRIDE 0x40
#define LINK_REG(n, off) (LINK_BASE + ((n) - LINK_FIRST) * LINK_STRIDE + (off))

#define PART_NUMBER 0xd000

/* Widget 0 status: the reader's port, then the error conditions. */
#define STATUS_WIDGET0_REQUEST (1U << 23)
#define STATUS_SOURCE_SHIFT 6
#define STATUS_ACCESS_ERROR (1U << 5)
#define STATUS_CONDITIONS 0x3f

/* Widget 0 control: enables sit at the same bit as their condition. */
#define CONTROL_WRITABLE 0xfe
#define CONTROL_RESET 0x80

/* Interrupt destination upper: vector, target and address; bit 19 fixed. */
#define DEST_WRITABLE 0xff07ffff
#define DEST_FIXED (1U << 19)
#define DEST_RESET 0x00080000

/* Link control: the force-bad-packet one-shot at bit 23 always reads zero. */
#define LINK_CONTROL_WRITABLE 0xbf7f01ff
#define LINK_CONTROL_RESET 0x00080000

/*
 * "The majority of the Crossbow Subsection of XBridge operates off single
 * 100 Mhz clock known as the core_clock" (XBridge specification rev 0.5,
 * printed page 86): the unit of the link reset one-shot's two cycles
 * (page 70) and, inferred, of the 1024 after a first reset pattern in
 * which the link layer ignores the rest (page 122).
 */
#define CORE_CYCLE_NS 10

#define LINK_STATUS_ALIVE (1U << 31)
#define LINK_STATUS_MAX_RETRY (1U << 5)
#define LINK_AUX_FAILURE_MODE (1U << 6)
#define LINK_AUX_PRESENT (1U << 5)
#define LINK_AUX_EIGHT_BIT (1U << 4)

static bool xbow_link_present(SGIXBridgeXbowState *s, unsigned link)
{
    return s->links_present & (1U << (link - LINK_FIRST));
}

void sgi_xbridge_xbow_set_link_present(SGIXBridgeXbowState *s,
                                       unsigned int widget, bool present)
{
    uint8_t bit = 1U << (widget - LINK_FIRST);

    g_assert(widget >= LINK_FIRST && widget < LINK_FIRST + LINK_COUNT);
    s->links_present = present ? (s->links_present | bit)
                               : (s->links_present & ~bit);
}

static void xbow_packet(SGIXBridgeXbowState *s, bool set)
{
    SGIXioIntDest dest = sgi_xio_int_dest(REG(s, 0x30), REG(s, 0x38));
    MemTxResult result;

    if (!s->target[dest.widget]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge-xbow: interrupt target %x is absent\n",
                      dest.widget);
        return;
    }
    result = sgi_xio_send_interrupt(&s->target_as[dest.widget], dest.address,
                                    dest.vector, set);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge-xbow: interrupt destination refused write\n");
    }
}

/* One assertion is outstanding at a time; later conditions only accumulate. */
static void xbow_condition(SGIXBridgeXbowState *s, uint32_t condition)
{
    s->status |= condition;
    if ((REG(s, 0x20) & condition) && !(s->status & STATUS_WIDGET0_REQUEST)) {
        s->status |= STATUS_WIDGET0_REQUEST;
        xbow_packet(s, true);
    }
}

static void xbow_access_error(SGIXBridgeXbowState *s, hwaddr addr,
                              unsigned size, bool write)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbridge-xbow: %u-byte %s at 0x%" HWADDR_PRIx
                  " is a register access error\n",
                  size, write ? "write" : "read", addr);
    xbow_condition(s, STATUS_ACCESS_ERROR);
}

static uint32_t xbow_status(SGIXBridgeXbowState *s, unsigned port)
{
    return s->status | (port << STATUS_SOURCE_SHIFT);
}

static uint32_t xbow_status_clear(SGIXBridgeXbowState *s, unsigned port)
{
    uint32_t value = xbow_status(s, port);
    bool asserted = s->status & STATUS_WIDGET0_REQUEST;

    s->status &= ~(STATUS_CONDITIONS | STATUS_WIDGET0_REQUEST);
    if (asserted) {
        xbow_packet(s, false);
    }
    return value;
}

static uint32_t xbow_link_status(SGIXBridgeXbowState *s, unsigned link)
{
    /* An empty port fails initialization and stays there. */
    return xbow_link_present(s, link) ? LINK_STATUS_ALIVE
                                      : LINK_STATUS_MAX_RETRY;
}

static uint32_t xbow_link_aux(SGIXBridgeXbowState *s, unsigned link)
{
    uint32_t value = xbow_link_present(s, link) ? LINK_AUX_PRESENT
                                                : LINK_AUX_FAILURE_MODE;

    if (s->links_8bit & (1U << (link - LINK_FIRST))) {
        value |= LINK_AUX_EIGHT_BIT;
    }
    return value;
}

static bool xbow_decoded(hwaddr off)
{
    return off < 0x300 && (off < 0xd0 || off > 0xf8);
}

static uint64_t xbow_read(void *opaque, hwaddr addr, unsigned size)
{
    struct XbowPort *port = opaque;
    SGIXBridgeXbowState *s = port->xbow;
    hwaddr off = addr & ~7;

    if (size != 4 || (addr & 7) != 4 || !xbow_decoded(off)) {
        xbow_access_error(s, addr, size, false);
        return 0;
    }
    if (off >= LINK_BASE) {
        unsigned link = LINK_FIRST + (off - LINK_BASE) / LINK_STRIDE;

        switch ((off - LINK_BASE) % LINK_STRIDE) {
        case 0x00:
            /* Nothing is queued, so a flush completes at once. */
            return 0;
        case 0x10:
        case 0x28:
            return xbow_link_status(s, link);
        case 0x30:
            /* Write-only; the data the chip answers is not specified. */
            xbow_access_error(s, addr, size, false);
            return 0;
        case 0x38:
            return xbow_link_aux(s, link);
        default:
            return REG(s, off);
        }
    }
    switch (off) {
    case 0x08:
        return xbow_status(s, port->port);
    case 0x50:
        return xbow_status_clear(s, port->port);
    case 0xc8:
        return REG(s, 0xc0);
    default:
        return REG(s, off);
    }
}

static void xbow_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    struct XbowPort *port = opaque;
    SGIXBridgeXbowState *s = port->xbow;
    hwaddr off = addr & ~7;

    if (size != 4 || (addr & 7) != 4 || !xbow_decoded(off)) {
        xbow_access_error(s, addr, size, true);
        return;
    }
    if (off >= LINK_BASE) {
        unsigned link = LINK_FIRST + (off - LINK_BASE) / LINK_STRIDE;

        switch ((off - LINK_BASE) % LINK_STRIDE) {
        case 0x08:
            REG(s, off) = value & LINK_CONTROL_WRITABLE;
            return;
        case 0x18:
        case 0x20:
            REG(s, off) = value;
            return;
        case 0x30:
            /*
             * The one-shot resets the widget down this link alone, the
             * way the Crossbow's does. Which pattern it sends is stated
             * two ways: the register's own page says it makes the LLP
             * "assert the remote link reset signal" (printed page 70),
             * and the reset chapter says it asserts SendWarmReset (page
             * 122); the Crossbow datasheet leaves the same conflict in
             * its own specification open. Either way it goes down this
             * one link, and the modelled widgets, the bridge heads,
             * reset alike for both, so the model sends the one line it
             * has (inferred). What ends the link's initialization
             * afterwards is not stated, so a present link reports alive
             * throughout, the policy the Crossbow model applies to the
             * links a machine selects.
             */
            qemu_irq_pulse(s->warm_reset[link - LINK_FIRST]);
            return;
        default:
            xbow_access_error(s, addr, size, true);
            return;
        }
    }
    switch (off) {
    case 0x20:
        REG(s, off) = value & CONTROL_WRITABLE;
        break;
    case 0x28:
        REG(s, off) = value & 0xfffff;
        break;
    case 0x30:
        REG(s, off) = (value & DEST_WRITABLE) | DEST_FIXED;
        break;
    case 0x38:
        REG(s, off) = value;
        break;
    case 0x40:
        /* The write rearms error logging; the data is discarded. */
        REG(s, 0x40) = 0;
        REG(s, 0x10) = 0;
        REG(s, 0x18) = 0;
        break;
    case 0x48:
        REG(s, off) = value & 0x03ffffff;
        break;
    case 0x58:
        REG(s, off) = value & 0x3f;
        break;
    case 0x60:
    case 0x68:
        REG(s, off) = value & 0x00700000;
        break;
    case 0x70:
        if (value & 0xffc00) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-xbridge-xbow: one-wire pulse not modeled\n");
        }
        REG(s, off) = (value & 0xffffc) | 2;
        break;
    case 0x78 ... 0xb8:
        REG(s, off) = value & 0xff;
        break;
    case 0xc0:
        if (!REG(s, 0xc0)) {
            REG(s, 0xc0) = value & 0xff;
        }
        break;
    case 0xc8:
        REG(s, 0xc0) = 0;
        break;
    default:
        /* Identification, status, error addresses and the clearing alias. */
        xbow_access_error(s, addr, size, true);
        break;
    }
}

static const MemoryRegionOps xbow_ops = {
    .read = xbow_read,
    .write = xbow_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void xbow_reset_core(SGIXBridgeXbowState *s)
{
    unsigned i;

    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    REG(s, 0x00) = (s->revision << 28) | (PART_NUMBER << 12) | 1;
    REG(s, 0x20) = CONTROL_RESET;
    REG(s, 0x28) = 0xfffff;
    REG(s, 0x30) = DEST_RESET;
    REG(s, 0x48) = 0x03ff1810;
    REG(s, 0x58) = 2;
    REG(s, 0x70) = 2;
    REG(s, 0x78) = 0xff;
    for (i = 0; i < LINK_COUNT; i++) {
        REG(s, 0x80 + i * 8) = 0xff;
        REG(s, LINK_REG(LINK_FIRST + i, 0x08)) = LINK_CONTROL_RESET;
    }
}

/*
 * A warm reset arriving on a port resets the core and goes out on every
 * port whose fence admits the source, the source's own included, using
 * the fences as they were programmed before the core reset. Returning the
 * fences to all ones on that reset is the model's choice: the change list
 * says the fences "are reset only by power on reset" (printed page 27)
 * while the reset chapter treats an accepted warm reset as a power-on
 * reset of the core (page 122), and which holds is unknown.
 */
static void xbow_warm_reset(void *opaque, int line, int level)
{
    SGIXBridgeXbowState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint8_t fence[LINK_COUNT];
    unsigned i;

    if (!level) {
        return;
    }
    /*
     * "These reset loops are disabled by the LLP which after detecting the
     * first reset pattern, will ignore all subsequent reset requests for
     * 1024 cycles" (printed page 122): the resets that come back as this
     * one spreads, or another port's inside the window, do nothing. The
     * page gives the window to each port's LLP; the model keeps one for
     * the chip, and a pattern the widget 0 fence rejects still opens it,
     * since the LLP detected it (both inferred).
     */
    if (!sgi_xio_llp_reset_accepted(&s->warm_reset_quiet_until, now,
                                    CORE_CYCLE_NS)) {
        return;
    }
    if (!(REG(s, 0x78) & (1U << line))) {
        return;
    }
    for (i = 0; i < LINK_COUNT; i++) {
        fence[i] = REG(s, 0x80 + i * 8);
    }
    xbow_reset_core(s);
    /*
     * Every link whose fence admits the source port forwards it, the
     * source's own link included. A fence names the ports its link accepts
     * a warm reset from, and its own port's bit resets set (printed page
     * 27): "for normal operation a link 8 should always be enabled to
     * accept a warm reset from itself" (page 81).
     */
    for (i = 0; i < LINK_COUNT; i++) {
        if (fence[i] & (1U << line)) {
            qemu_irq_pulse(s->warm_reset[i]);
        }
    }
}

static void xbow_reset_hold(Object *obj, ResetType type)
{
    SGIXBridgeXbowState *s = SGI_XBRIDGE_XBOW(obj);

    xbow_reset_core(s);
    s->warm_reset_quiet_until = 0;
}

static void xbow_realize(DeviceState *dev, Error **errp)
{
    SGIXBridgeXbowState *s = SGI_XBRIDGE_XBOW(dev);
    unsigned i;

    if (!s->revision || s->revision > 15) {
        error_setg(errp, "XBridge crossbar revision must be 1 to 15");
        return;
    }
    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("sgi-xbridge-xbow.target-%x",
                                                i);

        if (s->target[i]) {
            address_space_init(&s->target_as[i], s->target[i], name);
        }
    }
    for (i = 0; i < LINK_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("sgi-xbridge-xbow.port-%x",
                                                LINK_FIRST + i);

        s->port[i].xbow = s;
        s->port[i].port = LINK_FIRST + i;
        memory_region_init_io(&s->view[i], OBJECT(s), &xbow_ops, &s->port[i],
                              name, 0x1000000);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->view[i]);
    }
    qdev_init_gpio_in_named(dev, xbow_warm_reset, "warm-reset", LINK_COUNT);
    qdev_init_gpio_out_named(dev, s->warm_reset, "warm-reset-out", LINK_COUNT);
}

static void xbow_unrealize(DeviceState *dev)
{
    SGIXBridgeXbowState *s = SGI_XBRIDGE_XBOW(dev);
    unsigned i;

    for (i = 0; i < 16; i++) {
        if (s->target[i]) {
            address_space_destroy(&s->target_as[i]);
        }
    }
}

static const VMStateDescription vmstate_xbow = {
    .name = TYPE_SGI_XBRIDGE_XBOW,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIXBridgeXbowState, REG_COUNT),
        VMSTATE_UINT32(status, SGIXBridgeXbowState),
        VMSTATE_INT64_V(warm_reset_quiet_until, SGIXBridgeXbowState, 2),
        VMSTATE_END_OF_LIST()
    }
};

/* Which links have a widget attached and which run eight bits wide. */
static const Property xbow_properties[] = {
    DEFINE_PROP_UINT8("links-present", SGIXBridgeXbowState, links_present, 0),
    DEFINE_PROP_UINT8("links-8bit", SGIXBridgeXbowState, links_8bit, 0),
    DEFINE_PROP_UINT32("revision", SGIXBridgeXbowState, revision, 2),
};

static void xbow_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI XBridge crossbar";
    dc->user_creatable = true;
    dc->realize = xbow_realize;
    dc->unrealize = xbow_unrealize;
    dc->vmsd = &vmstate_xbow;
    rc->phases.hold = xbow_reset_hold;
    device_class_set_props(dc, xbow_properties);
}

static void xbow_init(Object *obj)
{
    SGIXBridgeXbowState *s = SGI_XBRIDGE_XBOW(obj);
    unsigned i;

    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("target-%x", i);

        object_property_add_link(obj, name, TYPE_MEMORY_REGION,
                                 (Object **)&s->target[i],
                                 qdev_prop_allow_set_link_before_realize, 0);
    }
}

static const TypeInfo xbow_types[] = {
    {
        .name = TYPE_SGI_XBRIDGE_XBOW,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIXBridgeXbowState),
        .instance_init = xbow_init,
        .class_init = xbow_class_init,
    },
};

DEFINE_TYPES(xbow_types)
