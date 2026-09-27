/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DP83840 ethernet transceiver, reached through the IOC3's management
 * registers (the lan9118-phy shape). Out of reset the link is down; it
 * comes up a settle after the first status read, or at once on a
 * control-register write. The "dp83843" property switches the model to
 * the Origin 300 IO-8 board's DP83843 part, with its own identity,
 * loopback selection and reset register values. See
 * docs/specs/sgi-ioc3.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/dp83840.h"
#include "hw/net/mii.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "trace.h"

/* Both ends offer everything; what a real machine negotiates is unrecorded. */
#define DP83840_ADVERTISE_RESET (MII_ANAR_TXFD | MII_ANAR_TX | \
                                 MII_ANAR_10FD | MII_ANAR_10 | \
                                 MII_ANAR_CSMACD)

/* The link bit latches low on hardware; the live level satisfies both reads. */
static uint16_t dp83840_status(DP83840State *s)
{
    uint16_t status = MII_BMSR_EXTCAP | MII_BMSR_AUTONEG |
                      MII_BMSR_100TX_FD | MII_BMSR_100TX_HD |
                      MII_BMSR_10T_FD | MII_BMSR_10T_HD;

    if (s->dp83843) {
        status |= MII_BMSR_MFPS;
    }
    if (s->autoneg_done && !s->link_down) {
        status |= MII_BMSR_LINK_ST | MII_BMSR_AN_COMP;
    }

    return status;
}

void dp83840_update_link(DP83840State *s, bool link_down)
{
    trace_dp83840_update_link(link_down ? "down" : "up");
    s->link_down = link_down;
}

static void dp83840_settle_done(void *opaque)
{
    DP83840State *s = opaque;

    s->autoneg_done = true;
}

/*
 * The settle arms on the first status read: armed at reset it elapses
 * before SGI's diagnostic probes (which must find the link down), and
 * armed on control writes it never fires for Linux's phylib.
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

    /* The identity binds OpenBSD's DP83840-specific driver. */
    case MII_PHYID1:
        return DP83840_PHYID1;

    case MII_PHYID2:
        return s->dp83843 ? DP83843_PHYID2 : DP83840_PHYID2;

    case MII_ANAR:
        return s->advertise;

    case MII_ANLPAR:
        /* The far end mirrors our advertisement; invalid until negotiated. */
        return s->autoneg_done ? DP83840_ADVERTISE_RESET : 0;

    case MII_ANER:
        return (s->dp83843 ? MII_ANER_NP : 0) |
               (s->autoneg_done ? MII_ANER_NWAY : 0);

    case DP83840_PCS_CONFIG:
        return s->pcs_config;

    case DP83840_LOOPBACK_BYPASS:
    case DP83840_10BT_CONFIG:
        /* Storage; SGI's power-on transceiver test requires read-back. */
        return reg == DP83840_LOOPBACK_BYPASS ? s->loopback_bypass
                                              : s->tenbt_config;

    case DP83840_ADDRESS_STATUS:
        return s->addr;

    default:
        /* Zero: all-ones reads as an absent transceiver. */
        qemu_log_mask(LOG_UNIMP,
                      "dp83840: read of register %d which this model does not "
                      "implement\n", reg);
        return 0;
    }
}

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
        /* Reset and restart self-clear, or a polling driver hangs. */
        if (val & MII_BMCR_RESET) {
            dp83840_reset(s);
            return;
        }

        /* No cable to negotiate against: a configuring write links at once. */
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
 * Three switches select loopback on the DP83840A; the diagnostic uses more
 * than one. The DP83843's register 0x17 (DP83840_PCS_CONFIG's address,
 * plain storage on the DP83840A) replaces register 0x18 as the second
 * switch: its Loopback Register carries the twister/PMD and 10BASE-T ENDEC
 * loopback selects instead.
 */
bool dp83840_in_loopback(DP83840State *s)
{
    if (s->control & MII_BMCR_LOOPBACK) {
        return true;
    }
    if (s->dp83843) {
        return (s->pcs_config & (DP83843_LBR_TW_LBEN |
                                 DP83843_LBR_10_ENDEC_LB)) != 0;
    }
    return (s->loopback_bypass & (DP83840_LBREMR_LB_MASK |
                                  DP83840_LBREMR_10BT_LOOPBACK)) != 0;
}

void dp83840_reset(DP83840State *s)
{
    trace_dp83840_reset();

    /* Link down: SGI's diagnostic probes the not-yet-settled state. */
    s->control = DP83840_CONTROL_RESET_VALUE;
    s->advertise = DP83840_ADVERTISE_RESET;
    /* The DP83843 selects the normal copper strap configuration here. */
    s->pcs_config = s->dp83843 ? 0 : DP83840_PCS_RESET_VALUE;
    s->loopback_bypass = s->dp83843 ? 0 : DP83840_LBREMR_RESET_VALUE;
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
    DEFINE_PROP_BOOL("dp83843", DP83840State, dp83843, false),
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
