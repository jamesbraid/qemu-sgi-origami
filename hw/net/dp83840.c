/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DP83840 ethernet transceiver, as the Origin 200's base I/O controller drives
 * it.
 *
 * The part has no address of its own. Software reaches it by writing a
 * five-bit transceiver address and a five-bit register number into the
 * controller's management control register and reading a word back, so this
 * device offers a plain pair of functions rather than a MemoryRegion and the
 * controller calls them when it services a guest's management access. That is
 * the shape QEMU's own lan9118-phy already has, for the same reason.
 *
 * What is modelled is what the two drivers we run require, which is a good
 * deal less than the part does -- see docs/datasheets/dp83840.md, which also
 * records why answering with the real identity is a decision rather than a
 * detail: it binds OpenBSD's specific transceiver driver instead of its
 * generic one, and widens the contract by two registers belonging to this part
 * alone.
 *
 * The link takes a moment to come up, the way real autonegotiation does. Out
 * of reset this is a board still negotiating -- link down, autonegotiation
 * incomplete -- which is the state SGI's power-on diagnostic reads and checks
 * against the part's documented reset values. The link then comes up on its
 * own a short settle after the interface first reads link status, so a driver
 * that leaves autonegotiation enabled and only polls (Linux's phylib) gets its
 * carrier without writing anything; a driver that forces the link up with a
 * control-register write (the PROM's network boot, OpenBSD) gets it at once.
 * The settle is a virtual-clock delay the way QEMU's e1000 times
 * autonegotiation, but what starts it is this model's own choice -- see
 * dp83840_status_read() for the argument. See also dp83840_reset() and
 * dp83840_write().
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/dp83840.h"
#include "hw/net/mii.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"

/*
 * What the station advertises, and it is a choice rather than a finding. The
 * part is a hundred-megabit transceiver and full duplex is what a modern link
 * would settle on, so that is what both ends offer here. Nothing observed says
 * what a real Origin 200 negotiates on a given network -- the captures print
 * the transceiver but not the media it settled on -- and the datasheet records
 * this as open for that reason.
 */
#define DP83840_ADVERTISE_RESET (MII_ANAR_TXFD | MII_ANAR_TX | \
                                 MII_ANAR_10FD | MII_ANAR_10 | \
                                 MII_ANAR_CSMACD)

/*
 * The status word, derived on every read rather than stored. Two of its bits
 * depend on state a guest can change -- whether the link has come up, and it
 * can take it back down with a reset -- so a stored copy would be a second
 * place for the answer to live and a second place for it to go stale.
 *
 * The link bit is worth a word of its own. On real hardware it latches low, so
 * both drivers read this register twice and take the union of the two reads to
 * find out whether the link has been down since they last looked. A model
 * reporting the live level satisfies that: the union of two identical answers
 * is that answer. What a model must not do is latch the bit and then fail to
 * clear it, because the second read would report a link that is not there.
 *
 * The link bit and the completion bit come up clear and go together: neither
 * is real until autonegotiation has settled. A single reset-value probe sees
 * them clear -- the bench state SGI's power-on diagnostic expects -- while a
 * driver that polls sees them set once the settle elapses. See
 * dp83840_status_read() for how the settle is armed. A backend whose link
 * is down takes both away again whatever the negotiation state, because
 * there is no wire for either to be true of.
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

/*
 * The backend's link state, handed in by the controller that owns the
 * network client -- the same split lan9118_phy_update_link has. Nothing
 * else to recompute here: the status word is derived on every read, so
 * recording the state is the whole job.
 */
void dp83840_update_link(DP83840State *s, bool link_down)
{
    s->link_down = link_down;
}

/* The settle elapsed: autonegotiation has completed and the link is up. */
static void dp83840_settle_done(void *opaque)
{
    DP83840State *s = opaque;

    s->autoneg_done = true;
}

/*
 * Read the status register, arming the autonegotiation settle the first time
 * the interface looks at link status. Arming on a status read is not what
 * upstream models do -- e1000 and its relatives start their settle from link
 * events and control-register writes -- and the departure is deliberate. Our
 * wall-clock device timing places SGI's power-on diagnostic tens of virtual
 * seconds into the run, so a settle armed at reset would have elapsed long
 * before the diagnostic's reset-value probe, which must find the link still
 * down; and Linux's phylib never writes the control register at all, so a
 * write-armed settle would leave it polling forever. The window has to open
 * when the diagnostic starts probing, and the first status read is that
 * moment. The read itself still returns the not-yet-linked state; a caller
 * that reads once (the diagnostic) sees the link settling, and one that
 * polls (a driver) sees it come up when the delay elapses. A guest that
 * forces the link up with a control-register write does not go through here
 * -- see dp83840_write().
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

uint16_t dp83840_read(DP83840State *s, int reg)
{
    switch (reg) {
    case MII_BMCR:
        return s->control;

    case MII_BMSR:
        return dp83840_status_read(s);

    /*
     * The real identity: National Semiconductor, model zero, revision one.
     * Answering with it is a decision rather than a detail -- the pair is
     * what OpenBSD's table matches to bind its DP83840-specific transceiver
     * driver instead of its generic one, and the revision is what its
     * console line prints. The high word also satisfies mainline Linux's
     * cruder scan, which accepts anything neither all-zeroes nor all-ones.
     */
    case MII_PHYID1:
        return DP83840_PHYID1;

    case MII_PHYID2:
        return DP83840_PHYID2;

    case MII_ANAR:
        return s->advertise;

    case MII_ANLPAR:
        /*
         * What the far end offers. It advertises exactly what this end does,
         * which is what puts the driver on the route it trusts: it intersects
         * the two words and reads the media out of the result, so a partner
         * offering nothing in common would leave the guest with a link and no
         * usable media. Not valid before autonegotiation completes -- the
         * part's own datasheet says so -- so this reads zero until then.
         */
        return s->autoneg_done ? DP83840_ADVERTISE_RESET : 0;

    case MII_ANER:
        /*
         * The far end can autonegotiate. This single bit chooses which of two
         * routes the driver takes to a media type, and the other one ends at a
         * register whose duplex reporting the driver's own comment distrusts.
         * Same validity rule as the partner register above.
         */
        return s->autoneg_done ? MII_ANER_NWAY : 0;

    case DP83840_PCS_CONFIG:
        return s->pcs_config;

    case DP83840_LOOPBACK_BYPASS:
    case DP83840_10BT_CONFIG:
        /*
         * Storage, and read back is the whole contract. SGI's power-on
         * transceiver test writes the loopback selects here and requires them
         * to read back, then writes them away again and requires that too;
         * nothing else in the machine looks at either register.
         */
        return reg == DP83840_LOOPBACK_BYPASS ? s->loopback_bypass
                                              : s->tenbt_config;

    case DP83840_ADDRESS_STATUS:
        /*
         * Read only on the route taken when the far end cannot autonegotiate,
         * which this model's far end can -- so nothing our guests do reaches
         * this. It answers anyway, reporting a hundred-megabit link by leaving
         * the ten-megabit bit clear, and its own address in the low bits,
         * which is the one field in it that has to agree with something else.
         */
        return s->addr;

    default:
        /*
         * Every other register this part has. Answering zero rather than
         * all-ones matters: all-ones is what an absent transceiver reads as,
         * and a driver scanning for one takes it as an empty address.
         */
        qemu_log_mask(LOG_UNIMP,
                      "dp83840: read of register %d which this model does not "
                      "implement\n", reg);
        return 0;
    }
}

void dp83840_write(DP83840State *s, int reg, uint16_t val)
{
    switch (reg) {
    case MII_BMCR:
        /*
         * Reset and restart are self-clearing: a driver writes one, then polls
         * for it to go away, and a model that left either set would hold the
         * driver in a loop until its timeout expired. Both take effect here and
         * are gone before the guest can look.
         *
         * Reset returns the part to its cable-less bench state, link down.
         */
        if (val & MII_BMCR_RESET) {
            dp83840_reset(s);
            return;
        }

        /*
         * Any other write to this register is a guest configuring the interface
         * to carry traffic -- restarting autonegotiation, or forcing a speed and
         * duplex. That brings the link up at once: there is no cable to
         * negotiate against, so we stand in as a cooperative far end and the
         * negotiation succeeds immediately. This is the path the PROM's network
         * boot and OpenBSD take. Isolating the part takes it back off the link,
         * and leaves the settle free to re-arm on the next status read.
         *
         * Linux's phylib does not reach here: it leaves autonegotiation enabled
         * from reset and polls status without rewriting the control register, so
         * its link comes up through the settle armed in dp83840_status_read().
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
        /*
         * Read-modify-written once when the driver changes media, and never
         * read back for a decision. Storage that returns what was written is
         * the whole contract, and is what lets the driver's read-modify-write
         * see the bits it set a moment earlier.
         */
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
 * Whether the transceiver is looping the transmit path back to receive. The
 * controller asks so that it can turn a frame round without putting it on the
 * wire.
 *
 * Three switches select it and the diagnostic uses more than one: the control
 * register's own loopback bit, and two fields of the loopback and bypass
 * register -- the hundred-megabit loopback selects, one of which turns the
 * frame round at the twisted-pair transceiver beyond this part, and the
 * ten-megabit encoder loopback. This model does not distinguish where the
 * frame turns round, because nothing downstream of it is modelled: the
 * controller sees the same frame come back either way.
 */
bool dp83840_in_loopback(DP83840State *s)
{
    return (s->control & MII_BMCR_LOOPBACK) ||
           (s->loopback_bypass & (DP83840_LBREMR_LB_MASK |
                                  DP83840_LBREMR_10BT_LOOPBACK));
}

void dp83840_reset(DP83840State *s)
{
    /*
     * Out of reset the part is autonegotiating and advertising everything it
     * can do, and its registers hold the values its own specification gives.
     *
     * The link is not one of those values. Real silicon comes up with the link
     * down and autonegotiation incomplete, then negotiates on its own and
     * brings the link up a short time later. SGI's power-on diagnostic reads the
     * status, link-partner and expansion registers early, as pure reads before
     * it has driven the part, and expects that not-yet-settled state: no link,
     * no completed autonegotiation, no partner. A model that came up with the
     * link already live answered those reads wrong, which is what the
     * diagnostic's three reset-value warnings reported. So the link stays down
     * out of reset and comes up a settle after the interface first reads status
     * -- see dp83840_status_read() -- or at once if a guest forces it up with a
     * control-register write -- see dp83840_write(). See docs/datasheets/dp83840.md.
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
    dc->realize = dp83840_realize;
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
