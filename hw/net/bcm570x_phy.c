/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Broadcom BCM570x integrated copper PHY. */
/*
 * The PHY has no address of its own on the bus: software reaches it by
 * writing a register number into the MAC's MI_COMM register and reading a
 * word back, so this device offers a plain read/write pair rather than a
 * MemoryRegion and the MAC calls them when it services a guest's management
 * access. That is the split the in-tree DP83840 model takes, and for the same
 * reason. The MAC is a different Broadcom part but the copper PHY's contract
 * is the ordinary MII/GMII one that OpenBSD's brgphy drives.
 *
 * Link, like the DP83840's, is modelled without a cable: out of reset the
 * board is negotiating, link down, autonegotiation incomplete, and the
 * link comes up on its own a short settle after the interface first reads
 * status (the path a poll-only driver takes), or at once when a guest
 * configures the interface with a control-register write (brgphy restarts
 * autonegotiation, so it takes this path). The reported link is also gated on
 * the MAC's netdev link, so a backend that goes down takes the carrier with
 * it.
 *
 * The identity is a decision, not a detail: it decodes to Broadcom's
 * organisational identifier and the BCM5701 model, which is the pair brgphy
 * matches to bind. Modelled from the public OpenBSD brgphy/mii sources and
 * the ordinary MII/GMII register contract they drive.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/bcm570x_phy.h"

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
 * Start the autonegotiation settle. A real PHY negotiates on its own as soon
 * as autonegotiation is enabled, which it is out of reset, so the settle
 * runs from reset and from the control-register write that restarts it, not
 * from the driver's first look at link.
 *
 * Waiting for that first look is what broke bring-up: the IRIX tg driver
 * reads link status only a handful of times while configuring the interface
 * and then stops for good, so arming on its first read guaranteed every one
 * of those reads saw a link that had not begun negotiating, and the carrier
 * arrived after the driver had given up watching for it.
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
         * driver on the route it trusts: it intersects the two words and
         * reads the media out. Not valid before autonegotiation completes.
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
         * settled result: autonegotiation complete (ACOMP), and the
         * resolution 1000baseT full duplex in the highest-common-denominator
         * field (matching the GTSR/EXTSR gigabit answer above). This is the
         * register the driver gates the up-path on, so arm the settle here
         * too. A poll that reads only this register still brings the link
         * up. Never the all-ones an absent PHY reads, even while negotiating,
         * so the driver does not take it for a bad read.
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
         * Reset and restart are self-clearing: a driver writes one, then
         * polls for it to go away. Reset returns the PHY to its cable-less
         * bench state, link down.
         */
        if (val & BCM570X_PHY_BMCR_RESET) {
            bcm570x_phy_reset(s);
            return;
        }

        /*
         * Isolating the part takes it off the link. A restart begins a fresh
         * negotiation, which the driver waits to observe completing, so it
         * cannot complete synchronously here or a driver polling for that
         * transition waits on one that already happened.
         *
         * Only a restart restarts it. A driver rewrites this register for
         * other reasons, checking the link, setting a mode, and treating
         * every write as a restart drops a working link each time, which the
         * driver sees as the carrier coming back and announces again and
         * again.
         */
        if (val & BCM570X_PHY_BMCR_ISOLATE) {
            s->autoneg_done = false;
            s->settle_armed = false;
            timer_del(s->settle_timer);
        } else if (val & BCM570X_PHY_BMCR_STARTNEG) {
            s->autoneg_done = false;
            s->settle_armed = false;
            timer_del(s->settle_timer);
        }

        s->bmcr = val & ~(BCM570X_PHY_BMCR_RESET | BCM570X_PHY_BMCR_STARTNEG);
        if (!(val & BCM570X_PHY_BMCR_ISOLATE)) {
            bcm570x_phy_arm_settle(s);
        }
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
        /* Out of reset autonegotiation is enabled, so the part starts it. */
        bcm570x_phy_arm_settle(s);
    }
    /*
     * link_down is the state of the far end, not a register, so a PHY reset
     * leaves it alone: the MAC drives it from the netdev.
     */
}

static void bcm570x_phy_init(Object *obj)
{
    Bcm570xPhyState *s = BCM570X_PHY(obj);

    s->settle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   bcm570x_phy_settle_done, s);
}

static void bcm570x_phy_finalize(Object *obj)
{
    Bcm570xPhyState *s = BCM570X_PHY(obj);

    timer_free(s->settle_timer);
}

static bool bcm570x_phy_compat_needed(void *opaque)
{
    return false;
}

const VMStateDescription vmstate_bcm570x_phy = {
    .name = "bcm570x-phy",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = bcm570x_phy_compat_needed,
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

static const TypeInfo bcm570x_phy_types[] = {
    {
        .name = TYPE_BCM570X_PHY,
        .parent = TYPE_OBJECT,
        .instance_size = sizeof(Bcm570xPhyState),
        .instance_init = bcm570x_phy_init,
        .instance_finalize = bcm570x_phy_finalize,
    },
};

DEFINE_TYPES(bcm570x_phy_types)
