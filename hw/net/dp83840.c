/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DP83840 ethernet transceiver, driven by the Origin 200's base I/O
 * controller.
 *
 * The part has no bus address: the controller reaches it through its
 * management registers, so this device is a pair of read/write functions
 * rather than a MemoryRegion (the lan9118-phy shape). Only what the guests'
 * drivers use is modelled; the DP83840 datasheet in the Origami project tree
 * (docs/datasheets/dp83840.md) covers the rest.
 *
 * Out of reset the link is down and autonegotiation incomplete, the state
 * SGI's power-on diagnostic expects. The link comes up a virtual-clock
 * settle after the first status read (Linux's phylib polls without ever
 * writing the control register), or at once when a driver forces it with a
 * control-register write (the PROM's network boot, OpenBSD).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/dp83840.h"
#include "hw/net/mii.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "trace.h"

/*
 * A choice, not a finding: nothing observed records what a real Origin 200
 * negotiates, so both ends offer everything (the datasheet marks this open).
 */
#define DP83840_ADVERTISE_RESET (MII_ANAR_TXFD | MII_ANAR_TX | \
                                 MII_ANAR_10FD | MII_ANAR_10 | \
                                 MII_ANAR_CSMACD)

/*
 * On real hardware the link bit latches low and drivers read the register
 * twice, taking the union. Reporting the live level satisfies that; latching
 * it set would make the second read report a link that is not there.
 */
static uint16_t dp83840_status(DP83840State *s)
{
    uint16_t status = MII_BMSR_EXTCAP | MII_BMSR_AUTONEG |
                      MII_BMSR_100TX_FD | MII_BMSR_100TX_HD |
                      MII_BMSR_10T_FD | MII_BMSR_10T_HD;

    if (s->autoneg_done && !s->link_down) {
        status |= MII_BMSR_LINK_ST | MII_BMSR_AN_COMP;
    }

    return status;
}

/* Backend link state, handed in by the controller (lan9118_phy split). */
void dp83840_update_link(DP83840State *s, bool link_down)
{
    trace_dp83840_update_link(link_down ? "down" : "up");
    s->link_down = link_down;
}

/* The settle elapsed: autonegotiation has completed and the link is up. */
static void dp83840_settle_done(void *opaque)
{
    DP83840State *s = opaque;

    s->autoneg_done = true;
}

/*
 * The settle arms on the first status read, not at reset or on control
 * writes the way e1000 does it, and the departure is deliberate: a settle
 * armed at reset elapses long before SGI's diagnostic probes the reset
 * values (which must find the link down), and Linux's phylib never writes
 * the control register at all, so a write-armed settle never fires for it.
 */
static uint16_t dp83840_status_read(DP83840State *s)
{
    if (!s->settle_armed && !s->autoneg_done &&
        (s->control & MII_BMCR_AUTOEN)) {
        s->settle_armed = true;
        timer_mod(s->settle_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  DP83840_AUTONEG_SETTLE_NS);
    }
    return dp83840_status(s);
}

static uint16_t dp83840_read_impl(DP83840State *s, int reg)
{
    switch (reg) {
    case MII_BMCR:
        return s->control;

    case MII_BMSR:
        return dp83840_status_read(s);

    /* The real identity binds OpenBSD's DP83840-specific driver. */
    case MII_PHYID1:
        return DP83840_PHYID1;

    case MII_PHYID2:
        return DP83840_PHYID2;

    case MII_ANAR:
        return s->advertise;

    case MII_ANLPAR:
        /*
         * The far end mirrors our advertisement so the driver's
         * intersection yields a usable media; invalid until negotiated.
         */
        return s->autoneg_done ? DP83840_ADVERTISE_RESET : 0;

    case MII_ANER:
        /*
         * The far end can autonegotiate; this bit picks the driver's
         * trusted route to a media type. Same validity rule as ANLPAR.
         */
        return s->autoneg_done ? MII_ANER_NWAY : 0;

    case DP83840_PCS_CONFIG:
        return s->pcs_config;

    case DP83840_LOOPBACK_BYPASS:
    case DP83840_10BT_CONFIG:
        /* Storage; SGI's power-on transceiver test requires read-back. */
        return reg == DP83840_LOOPBACK_BYPASS ? s->loopback_bypass
                                              : s->tenbt_config;

    case DP83840_ADDRESS_STATUS:
        /* Unreached (our far end autonegotiates); answers anyway. */
        return s->addr;

    default:
        /* Zero, not all-ones: all-ones reads as an absent transceiver. */
        qemu_log_mask(LOG_UNIMP,
                      "dp83840: read of register %d which this model does not "
                      "implement\n", reg);
        return 0;
    }
}

/* One trace point for every case above, derived answers included. */
uint16_t dp83840_read(DP83840State *s, int reg)
{
    uint16_t val = dp83840_read_impl(s, reg);

    trace_dp83840_read(val, reg);
    return val;
}

void dp83840_write(DP83840State *s, int reg, uint16_t val)
{
    trace_dp83840_write(val, reg);

    switch (reg) {
    case MII_BMCR:
        /* Reset and restart self-clear, or the polling driver hangs. */
        if (val & MII_BMCR_RESET) {
            dp83840_reset(s);
            return;
        }

        /*
         * Any other write is a guest configuring the link, and there is no
         * cable to negotiate against, so it comes up at once (the PROM and
         * OpenBSD path). Isolation takes it back down and lets the settle
         * re-arm on the next status read.
         */
        if (val & MII_BMCR_ISOLATE) {
            s->autoneg_done = false;
            s->settle_armed = false;
            timer_del(s->settle_timer);
        } else {
            s->autoneg_done = true;
            s->settle_armed = true;
            timer_del(s->settle_timer);
        }

        s->control = val & ~(MII_BMCR_RESET | MII_BMCR_ANRESTART);
        return;

    case MII_ANAR:
        s->advertise = val;
        return;

    case DP83840_PCS_CONFIG:
        s->pcs_config = val;
        return;

    case DP83840_LOOPBACK_BYPASS:
        s->loopback_bypass = val;
        return;

    case DP83840_10BT_CONFIG:
        s->tenbt_config = val;
        return;

    case MII_BMSR:
    case MII_PHYID1:
    case MII_PHYID2:
    case MII_ANLPAR:
    case MII_ANER:
    case DP83840_ADDRESS_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "dp83840: write of 0x%04x to read-only register %d\n",
                      val, reg);
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "dp83840: write of 0x%04x to register %d which this "
                      "model does not implement\n", val, reg);
        return;
    }
}

/*
 * Three switches select loopback and the diagnostic uses more than one;
 * the controller sees the same frame come back whichever is set.
 */
bool dp83840_in_loopback(DP83840State *s)
{
    return (s->control & MII_BMCR_LOOPBACK) ||
           (s->loopback_bypass & (DP83840_LBREMR_LB_MASK |
                                  DP83840_LBREMR_10BT_LOOPBACK));
}

void dp83840_reset(DP83840State *s)
{
    trace_dp83840_reset();

    /*
     * The specification's reset values, with the link down: SGI's power-on
     * diagnostic reads status, partner and expansion before driving the
     * part and expects the not-yet-settled state (datasheet records the
     * probes). The link comes up through dp83840_status_read()'s settle or
     * a control-register write.
     */
    s->control = DP83840_CONTROL_RESET_VALUE;
    s->advertise = DP83840_ADVERTISE_RESET;
    s->pcs_config = DP83840_PCS_RESET_VALUE;
    s->loopback_bypass = DP83840_LBREMR_RESET_VALUE;
    s->tenbt_config = DP83840_10BT_RESET_VALUE;
    s->autoneg_done = false;
    s->settle_armed = false;
    if (s->settle_timer) {
        timer_del(s->settle_timer);
    }
}

static void dp83840_reset_hold(Object *obj, ResetType type)
{
    dp83840_reset(DP83840(obj));
}

static void dp83840_realize(DeviceState *dev, Error **errp)
{
    DP83840State *s = DP83840(dev);

    s->settle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dp83840_settle_done, s);
}

static void dp83840_unrealize(DeviceState *dev)
{
    DP83840State *s = DP83840(dev);

    timer_free(s->settle_timer);
    s->settle_timer = NULL;
}

static const VMStateDescription vmstate_dp83840 = {
    .name = "dp83840",
    .version_id = 4,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(control, DP83840State),
        VMSTATE_UINT16(advertise, DP83840State),
        VMSTATE_UINT16(pcs_config, DP83840State),
        VMSTATE_UINT16(loopback_bypass, DP83840State),
        VMSTATE_UINT16(tenbt_config, DP83840State),
        VMSTATE_BOOL(autoneg_done, DP83840State),
        VMSTATE_BOOL(settle_armed, DP83840State),
        VMSTATE_TIMER_PTR(settle_timer, DP83840State),
        VMSTATE_BOOL_V(link_down, DP83840State, 4),
        VMSTATE_END_OF_LIST()
    }
};

static const Property dp83840_properties[] = {
    DEFINE_PROP_UINT8("addr", DP83840State, addr, 31),
};

static void dp83840_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "DP83840 10/100 ethernet transceiver";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->realize = dp83840_realize;
    dc->unrealize = dp83840_unrealize;
    dc->vmsd = &vmstate_dp83840;
    device_class_set_props(dc, dp83840_properties);
    rc->phases.hold = dp83840_reset_hold;
}

static const TypeInfo dp83840_types[] = {
    {
        .name = TYPE_DP83840,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DP83840State),
        .class_init = dp83840_class_init,
    },
};

DEFINE_TYPES(dp83840_types)
