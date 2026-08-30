/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Broadcom NetXtreme BCM570x integrated copper PHY.
 *
 * The PHY has no address of its own on the bus: software reaches it by writing
 * a register number into the MAC's MI_COMM register and reading a word back, so
 * this device offers a plain read/write pair rather than a MemoryRegion and the
 * MAC calls them when it services a guest's management access. That is the
 * split the in-tree DP83840 model takes, and for the same reason. The MAC is a
 * different Broadcom part but the copper PHY's contract is the ordinary
 * MII/GMII one that OpenBSD's brgphy drives.
 *
 * Link, like the DP83840's, is modelled without a cable: out of reset the
 * board is negotiating -- link down, autonegotiation incomplete -- and the link
 * comes up on its own a short settle after the interface first reads status
 * (the path a poll-only driver takes), or at once when a guest configures the
 * interface with a control-register write (brgphy restarts autonegotiation, so
 * it takes this path). The reported link is also gated on the MAC's netdev
 * link, so a backend that goes down takes the carrier with it.
 *
 * The identity is a decision, not a detail: it decodes to Broadcom's
 * organisational identifier and the BCM5701 model, which is the pair brgphy
 * matches to bind. Written from docs/datasheets/bcm570x.md and the public
 * OpenBSD brgphy/mii sources; no IRIX vendor source was read.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/bcm570x_phy.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"

bool bcm570x_phy_link_up(Bcm570xPhyState *s)
{
    return s->autoneg_done && !s->link_down;
}

void bcm570x_phy_set_link_down(Bcm570xPhyState *s, bool link_down)
{
    s->link_down = link_down;
}

/*
 * The status word, derived on every read. The link and completion bits depend
 * on state a guest and the backend can change, so a stored copy would be a
 * second place for the answer to go stale.
 */
static uint16_t bcm570x_phy_status(Bcm570xPhyState *s)
{
    uint16_t status = BCM570X_PHY_BMSR_EXTCAP | BCM570X_PHY_BMSR_CANAUTONEG |
                      BCM570X_PHY_BMSR_EXTSTAT |
                      BCM570X_PHY_BMSR_100TXFDX | BCM570X_PHY_BMSR_100TXHDX |
                      BCM570X_PHY_BMSR_10TFDX | BCM570X_PHY_BMSR_10THDX;

    if (bcm570x_phy_link_up(s)) {
        status |= BCM570X_PHY_BMSR_LINK | BCM570X_PHY_BMSR_ANEGDONE;
    }
    return status;
}

/* The settle elapsed: autonegotiation has completed. */
static void bcm570x_phy_settle_done(void *opaque)
{
    Bcm570xPhyState *s = opaque;

    s->autoneg_done = true;
}

/*
 * Arm the autonegotiation settle the first time the interface looks at link. A
 * real PHY negotiates on its own once autonegotiation is enabled, which it is
 * at reset; this models that as a delay started on the first link-status read,
 * so a driver that only polls gets its carrier without writing anything. Shared
 * by the basic status register and the auxiliary status summary, since a copper
 * link-up may poll either for completion.
 */
static void bcm570x_phy_arm_settle(Bcm570xPhyState *s)
{
    if (!s->settle_armed && !s->autoneg_done &&
        (s->bmcr & BCM570X_PHY_BMCR_AUTOEN)) {
        s->settle_armed = true;
        timer_mod(s->settle_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  BCM570X_PHY_AUTONEG_SETTLE_NS);
    }
}

static uint16_t bcm570x_phy_status_read(Bcm570xPhyState *s)
{
    bcm570x_phy_arm_settle(s);
    return bcm570x_phy_status(s);
}

uint16_t bcm570x_phy_read(Bcm570xPhyState *s, int reg)
{
    bool up = bcm570x_phy_link_up(s);

    switch (reg) {
    case BCM570X_PHY_BMCR:
        return s->bmcr;

    case BCM570X_PHY_BMSR:
        return bcm570x_phy_status_read(s);

    case BCM570X_PHY_IDR1:
        return BCM570X_PHY_IDR1_VALUE;

    case BCM570X_PHY_IDR2:
        return BCM570X_PHY_IDR2_BCM5701;

    case BCM570X_PHY_ANAR:
        return s->anar;

    case BCM570X_PHY_ANLPAR:
        /*
         * The far end advertises exactly what this end does, which puts the
         * driver on the route it trusts: it intersects the two words and reads
         * the media out. Not valid before autonegotiation completes.
         */
        return up ? (s->anar | BCM570X_PHY_ANLPAR_ACK) : 0;

    case BCM570X_PHY_GTCR:
        return s->gtcr;

    case BCM570X_PHY_GTSR:
        /*
         * The 1000baseT resolution the driver reads to settle on gigabit: the
         * link partner is 1000baseT-full capable, both receivers are OK, and
         * this end won the master/slave arbitration. Zero until negotiated.
         */
        return up ? (BCM570X_PHY_GTSR_MASTER | BCM570X_PHY_GTSR_LOCALRXOK |
                     BCM570X_PHY_GTSR_REMOTERXOK |
                     BCM570X_PHY_GTSR_LP_1000TFDX) : 0;

    case BCM570X_PHY_EXTSR:
        return BCM570X_PHY_EXTSR_1000TFDX | BCM570X_PHY_EXTSR_1000THDX;

    case BCM570X_PHY_AUXSTS:
        /*
         * The auxiliary status summary a brgphy copper link-up reads for the
         * settled result: autonegotiation complete (ACOMP), and the resolution
         * 1000baseT full duplex in the highest-common-denominator field
         * (matching the GTSR/EXTSR gigabit answer above). This is the register
         * the driver gates the up-path on, so arm the settle here too -- a poll
         * that reads only this register still brings the link up. Never the
         * all-ones an absent PHY reads, even while negotiating, so the driver
         * does not take it for a bad read.
         */
        bcm570x_phy_arm_settle(s);
        return up ? (BCM570X_PHY_AUXSTS_ACOMP | BCM570X_PHY_AUXSTS_1000TFDX) : 0;

    default:
        /*
         * Zero rather than all-ones: all-ones is what an absent PHY reads as,
         * and a driver scanning for one would take it as an empty address.
         */
        qemu_log_mask(LOG_UNIMP,
                      "bcm570x-phy: read of register %d which this model does "
                      "not implement\n", reg);
        return 0;
    }
}

void bcm570x_phy_write(Bcm570xPhyState *s, int reg, uint16_t val)
{
    switch (reg) {
    case BCM570X_PHY_BMCR:
        /*
         * Reset and restart are self-clearing: a driver writes one, then polls
         * for it to go away. Reset returns the PHY to its cable-less bench
         * state, link down.
         */
        if (val & BCM570X_PHY_BMCR_RESET) {
            bcm570x_phy_reset(s);
            return;
        }

        /*
         * Any other write configures the interface to carry traffic --
         * restarting autonegotiation, or forcing a speed and duplex. There is
         * no cable to negotiate against, so we stand in as a cooperative far
         * end and the negotiation succeeds at once. Isolating the part takes it
         * off the link and leaves the settle free to re-arm on the next status
         * read.
         */
        if (val & BCM570X_PHY_BMCR_ISOLATE) {
            s->autoneg_done = false;
            s->settle_armed = false;
            timer_del(s->settle_timer);
        } else {
            s->autoneg_done = true;
            s->settle_armed = true;
            timer_del(s->settle_timer);
        }

        s->bmcr = val & ~(BCM570X_PHY_BMCR_RESET | BCM570X_PHY_BMCR_STARTNEG);
        return;

    case BCM570X_PHY_ANAR:
        s->anar = val;
        return;

    case BCM570X_PHY_GTCR:
        s->gtcr = val;
        return;

    case BCM570X_PHY_BMSR:
    case BCM570X_PHY_IDR1:
    case BCM570X_PHY_IDR2:
    case BCM570X_PHY_ANLPAR:
    case BCM570X_PHY_GTSR:
    case BCM570X_PHY_EXTSR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm570x-phy: write of 0x%04x to read-only register %d\n",
                      val, reg);
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "bcm570x-phy: write of 0x%04x to register %d which this "
                      "model does not implement\n", val, reg);
        return;
    }
}

void bcm570x_phy_reset(Bcm570xPhyState *s)
{
    s->bmcr = BCM570X_PHY_BMCR_RESET_VALUE;
    s->anar = BCM570X_PHY_ANAR_RESET_VALUE;
    s->gtcr = BCM570X_PHY_GTCR_RESET_VALUE;
    s->autoneg_done = false;
    s->settle_armed = false;
    if (s->settle_timer) {
        timer_del(s->settle_timer);
    }
    /*
     * link_down is the state of the far end, not a register, so a PHY reset
     * leaves it alone: the MAC drives it from the netdev.
     */
}

static void bcm570x_phy_reset_hold(Object *obj, ResetType type)
{
    bcm570x_phy_reset(BCM570X_PHY(obj));
}

static void bcm570x_phy_realize(DeviceState *dev, Error **errp)
{
    Bcm570xPhyState *s = BCM570X_PHY(dev);

    s->settle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   bcm570x_phy_settle_done, s);
}

static const VMStateDescription vmstate_bcm570x_phy = {
    .name = "bcm570x-phy",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(bmcr, Bcm570xPhyState),
        VMSTATE_UINT16(anar, Bcm570xPhyState),
        VMSTATE_UINT16(gtcr, Bcm570xPhyState),
        VMSTATE_BOOL(autoneg_done, Bcm570xPhyState),
        VMSTATE_BOOL(link_down, Bcm570xPhyState),
        VMSTATE_BOOL(settle_armed, Bcm570xPhyState),
        VMSTATE_TIMER_PTR(settle_timer, Bcm570xPhyState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm570x_phy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "Broadcom NetXtreme BCM570x copper PHY";
    dc->realize = bcm570x_phy_realize;
    dc->vmsd = &vmstate_bcm570x_phy;
    rc->phases.hold = bcm570x_phy_reset_hold;
}

static const TypeInfo bcm570x_phy_types[] = {
    {
        .name = TYPE_BCM570X_PHY,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Bcm570xPhyState),
        .class_init = bcm570x_phy_class_init,
    },
};

DEFINE_TYPES(bcm570x_phy_types)
