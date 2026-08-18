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
 * The link is always up. There is no cable in the machine model and nothing
 * that would drive a change, so the transceiver reports a good link from reset
 * and autonegotiation finishes the moment it is asked for. Both drivers poll
 * with timeouts long enough that they cannot tell this from a real part, and a
 * model that made them wait would only be slower.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/net/dp83840.h"
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
#define DP83840_ADVERTISE_RESET (DP83840_LINK_100_FDX | DP83840_LINK_100_HDX | \
                                 DP83840_LINK_10_FDX | DP83840_LINK_10_HDX | \
                                 DP83840_LINK_SELECTOR)

/*
 * The status word, derived on every read rather than stored. Two of its bits
 * depend on state a guest can change -- whether autonegotiation has completed,
 * and it can restart one -- so a stored copy would be a second place for the
 * answer to live and a second place for it to go stale.
 *
 * The link bit is worth a word of its own. On real hardware it latches low, so
 * both drivers read this register twice and take the union of the two reads to
 * find out whether the link has been down since they last looked. A model
 * reporting the live level satisfies that: the union of two identical answers
 * is that answer. What a model must not do is latch the bit and then fail to
 * clear it, because the second read would report a link that is not there.
 */
static uint16_t dp83840_status(DP83840State *s)
{
    uint16_t status = DP83840_STAT_EXTENDED | DP83840_STAT_CAN_AUTONEG |
                      DP83840_STAT_LINK |
                      DP83840_STAT_100_FDX | DP83840_STAT_100_HDX |
                      DP83840_STAT_10_FDX | DP83840_STAT_10_HDX;

    if (s->autoneg_done) {
        status |= DP83840_STAT_AUTONEG_OK;
    }

    return status;
}

uint16_t dp83840_read(DP83840State *s, int reg)
{
    switch (reg) {
    case DP83840_CONTROL:
        return s->control;

    case DP83840_STATUS:
        return dp83840_status(s);

    case DP83840_IDENTITY_HIGH:
        return DP83840_IDENTITY_HIGH_VALUE;

    case DP83840_IDENTITY_LOW:
        return DP83840_IDENTITY_LOW_VALUE;

    case DP83840_ADVERTISE:
        return s->advertise;

    case DP83840_PARTNER:
        /*
         * What the far end offers. It advertises exactly what this end does,
         * which is what puts the driver on the route it trusts: it intersects
         * the two words and reads the media out of the result, so a partner
         * offering nothing in common would leave the guest with a link and no
         * usable media.
         */
        return DP83840_ADVERTISE_RESET;

    case DP83840_EXPANSION:
        /*
         * The far end can autonegotiate. This single bit chooses which of two
         * routes the driver takes to a media type, and the other one ends at a
         * register whose duplex reporting the driver's own comment distrusts.
         */
        return DP83840_EXP_PARTNER_AUTONEG;

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
        return DP83840_ADDRESS;

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
    case DP83840_CONTROL:
        /*
         * Reset and restart are self-clearing: a driver writes one, then polls
         * for it to go away, and a model that left either set would hold the
         * driver in a loop until its timeout expired. Both take effect here and
         * are gone before the guest can look.
         */
        if (val & DP83840_CTRL_RESET) {
            dp83840_reset(s);
            return;
        }

        if (val & DP83840_CTRL_RESTART) {
            s->autoneg_done = true;
        }

        /*
         * Asking for autonegotiation completes it, for the same reason: the
         * driver will not believe anything it reads until the completion bit is
         * set, and reports no media at all in the meantime -- a working link
         * that looks dead.
         */
        if (val & DP83840_CTRL_AUTONEG) {
            s->autoneg_done = true;
        }

        s->control = val & ~(DP83840_CTRL_RESET | DP83840_CTRL_RESTART);
        return;

    case DP83840_ADVERTISE:
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

    case DP83840_STATUS:
    case DP83840_IDENTITY_HIGH:
    case DP83840_IDENTITY_LOW:
    case DP83840_PARTNER:
    case DP83840_EXPANSION:
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
    return (s->control & DP83840_CTRL_LOOPBACK) ||
           (s->loopback_bypass & (DP83840_LBREMR_LB_MASK |
                                  DP83840_LBREMR_10BT_LOOPBACK));
}

void dp83840_reset(DP83840State *s)
{
    /*
     * Out of reset the part is autonegotiating and advertising everything it
     * can do, and its registers hold the values its own specification gives.
     *
     * The completion flag is the one place this model departs from those
     * values, and it is a departure with nothing behind it either way. Real
     * silicon comes up with autonegotiation incomplete and the link down, and
     * SGI's power-on diagnostic warns about both -- but that diagnostic reads
     * these registers long before it resets anything, so there is no interval
     * a model could complete in that the diagnostic would not already have
     * looked at. Reporting a settled link costs those two warnings, which the
     * diagnostic does not fail the board for; reporting an unsettled one costs
     * every guest its media, because both drivers report none until this flag
     * is set. See docs/datasheets/dp83840.md.
     */
    s->control = DP83840_CONTROL_RESET_VALUE;
    s->advertise = DP83840_ADVERTISE_RESET;
    s->pcs_config = DP83840_PCS_RESET_VALUE;
    s->loopback_bypass = DP83840_LBREMR_RESET_VALUE;
    s->tenbt_config = DP83840_10BT_RESET_VALUE;
    s->autoneg_done = true;
}

static void dp83840_reset_hold(Object *obj, ResetType type)
{
    dp83840_reset(DP83840(obj));
}

static const VMStateDescription vmstate_dp83840 = {
    .name = "dp83840",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(control, DP83840State),
        VMSTATE_UINT16(advertise, DP83840State),
        VMSTATE_UINT16(pcs_config, DP83840State),
        VMSTATE_UINT16(loopback_bypass, DP83840State),
        VMSTATE_UINT16(tenbt_config, DP83840State),
        VMSTATE_BOOL(autoneg_done, DP83840State),
        VMSTATE_END_OF_LIST()
    }
};

static void dp83840_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "DP83840 10/100 ethernet transceiver";
    dc->vmsd = &vmstate_dp83840;
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
