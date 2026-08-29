/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DP83840 ethernet transceiver, as the Origin 200's base I/O controller
 * drives it.
 */

#ifndef HW_NET_DP83840_H
#define HW_NET_DP83840_H

#include "hw/core/sysbus.h"
#include "hw/net/mii.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_DP83840 "dp83840"
OBJECT_DECLARE_SIMPLE_TYPE(DP83840State, DP83840)

/*
 * The registers this part has beyond the standard management set. The
 * standard registers, their bits and this part's own two identifier words
 * are hw/net/mii.h's vocabulary (MII_BMCR and the rest, DP83840_PHYID1 and
 * DP83840_PHYID2); only what mii.h has no name for is defined here.
 */
#define DP83840_PCS_CONFIG      0x17
#define DP83840_LOOPBACK_BYPASS 0x18
#define DP83840_ADDRESS_STATUS  0x19
#define DP83840_10BT_CONFIG     0x1c

/*
 * Reset values, taken from the part's own published specification. SGI's
 * power-on transceiver test compares against the same numbers through its own
 * masks, so a public part specification and a vendor diagnostic agree on them
 * independently -- which is what makes them the part's values rather than one
 * reader's inference from one firmware.
 *
 * The control register comes up autonegotiating at a hundred megabits in full
 * duplex. The configuration register comes up with NRZI coding enabled and the
 * hundred-megabit good-link override in its normal position. The ten-megabit
 * configuration register comes up transmitting link pulses over twisted pair
 * with heartbeat and jabber enabled.
 */
#define DP83840_CONTROL_RESET_VALUE  (MII_BMCR_SPEED100 | \
                                      MII_BMCR_AUTOEN | \
                                      MII_BMCR_FD)
#define DP83840_PCS_RESET_VALUE      0x8040
#define DP83840_10BT_RESET_VALUE     0x80b9

/*
 * The loopback and bypass register's reset value. Four of its bits are latched
 * from package pins at reset rather than fixed, and this board's wiring for
 * them is not observed anywhere we can read -- so they are modelled as the
 * normal operating state, no bypass and no loopback, which is the only state a
 * working board can be in. The two bits that are fixed in the specification,
 * bad-start-of-stream detection and transmit-disable-during-loopback, are set.
 */
#define DP83840_LBREMR_RESET_VALUE   0x8020

/*
 * The loopback selections in that register, which are separate from the
 * control register's own. Bits 9 and 8 choose a hundred-megabit loopback --
 * the setting that turns the frame round at the twisted-pair transceiver
 * beyond this part is one of them -- and bit 11 loops the ten-megabit encoder.
 * Any of them puts the station in loopback as far as the controller is
 * concerned, which is all the controller needs to know.
 */
#define DP83840_LBREMR_LB_MASK       0x0300
#define DP83840_LBREMR_10BT_LOOPBACK 0x0800

/*
 * How long autonegotiation takes to settle once the interface starts looking
 * for a link. QEMU's e1000 models the same delay with a virtual-clock timer;
 * the DP83840's own datasheet gives roughly 1.5-2.3 seconds against a capable
 * partner, and two seconds is used here. Any value a polling driver's generous
 * timeout tolerates would serve it, but the choice also has to clear the
 * firmware diagnostic's reset-value sweep: that reads status, then the
 * link-partner and expansion registers about a tenth of a second of virtual
 * time later (measured), and all three must find the link still settling. Two
 * seconds leaves that a wide margin even when host load stretches the sweep.
 */
#define DP83840_AUTONEG_SETTLE_NS (2 * NANOSECONDS_PER_SECOND)

struct DP83840State {
    SysBusDevice parent_obj;

    /*
     * The management address the part answers on, an "addr" property the
     * controller asks for when it routes a request and the address-status
     * register reports back. It defaults to 31, which is where the board
     * wires it: both drivers walk all thirty-two management addresses and
     * take the first that answers, so nothing forces this on the model --
     * but every one of the thirty-one real Origin 200 captures in the
     * boot-log library prints the transceiver at address 31, on the second
     * controller as well as the first, so it is the board's wiring rather
     * than one machine's accident. A model answering everywhere is found
     * at zero, and the guest prints the address it found.
     */
    uint8_t addr;

    /*
     * The writable registers, held as the words a driver reads back. Everything
     * else this part answers is either constant or derived, and deriving it on
     * each read is what keeps the derived words from disagreeing with the state
     * they are derived from.
     */
    uint16_t control;
    uint16_t advertise;
    uint16_t pcs_config;

    /*
     * The two part-specific registers only SGI's power-on diagnostic reaches.
     * Neither guest driver reads or writes either, so nothing but that
     * diagnostic and the register suite sees them at all -- they are here
     * because the firmware's transceiver test fails the board without them,
     * not because a driver wanted them.
     */
    uint16_t loopback_bypass;
    uint16_t tenbt_config;

    /*
     * Whether a link has come up. False out of reset -- a cable-less board on
     * a bench, which is exactly what SGI's power-on diagnostic reads before it
     * has driven this part -- and gated on it are the link and completion bits
     * in the status word and whether the partner and expansion registers answer
     * non-zero. It goes true a settling delay after the interface first reads
     * link status, or at once if a guest writes the control register to force
     * the link up. See dp83840_read(), dp83840_write() and dp83840_reset().
     */
    bool autoneg_done;

    /*
     * Whether the backend reports its link down -- the operator's set_link,
     * the model's stand-in for the cable being pulled. Kept apart from
     * autoneg_done because the two answer different questions: one is
     * whether negotiation has run its course, the other whether there is a
     * wire to negotiate over. The status word's link and completion bits
     * need both.
     */
    bool link_down;

    /*
     * The autonegotiation settling clock and whether it has been started since
     * reset. A real part negotiates on its own once autonegotiation is enabled,
     * which it is at reset; this models that as a delay armed the first time
     * the interface reads link status. Arming on the read rather than at reset
     * is deliberate: our wall-clock device timing reaches SGI's power-on
     * diagnostic tens of seconds of virtual time in, so a reset-armed timer
     * would already have fired and the diagnostic's single reset-value probe
     * would see a live link. Armed on the read, that one probe sees the link
     * still settling -- the bench state it expects -- while a driver that polls
     * waits the delay out and gets its carrier.
     */
    QEMUTimer *settle_timer;
    bool settle_armed;
};

void dp83840_reset(DP83840State *s);
void dp83840_update_link(DP83840State *s, bool link_down);
bool dp83840_in_loopback(DP83840State *s);
uint16_t dp83840_read(DP83840State *s, int reg);
void dp83840_write(DP83840State *s, int reg, uint16_t val);

#endif
