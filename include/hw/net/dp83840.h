/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DP83840 ethernet transceiver, as the Origin 200's base I/O controller
 * drives it.
 */

#ifndef HW_NET_DP83840_H
#define HW_NET_DP83840_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_DP83840 "dp83840"
OBJECT_DECLARE_SIMPLE_TYPE(DP83840State, DP83840)

/*
 * Where the board puts it. Both drivers walk all thirty-two management
 * addresses and take the first that answers, so nothing forces this on the
 * model -- but every one of the thirty-one real Origin 200 captures in the
 * boot-log library prints the transceiver at address 31, on the second
 * controller as well as the first, so it is the board's wiring rather than one
 * machine's accident. A model answering everywhere is found at zero, and the
 * guest prints the address it found.
 */
#define DP83840_ADDRESS         31

/* The management registers the two drivers we run actually touch. */
#define DP83840_CONTROL         0x00
#define DP83840_STATUS          0x01
#define DP83840_IDENTITY_HIGH   0x02
#define DP83840_IDENTITY_LOW    0x03
#define DP83840_ADVERTISE       0x04
#define DP83840_PARTNER         0x05
#define DP83840_EXPANSION       0x06
#define DP83840_PCS_CONFIG      0x17
#define DP83840_LOOPBACK_BYPASS 0x18
#define DP83840_ADDRESS_STATUS  0x19
#define DP83840_10BT_CONFIG     0x1c

/*
 * The identity, as the two sixteen-bit words a driver reads rather than as a
 * description of them. They decode to National Semiconductor's organisational
 * identifier, model zero and revision one, which is the pair OpenBSD's table
 * matches to bind its transceiver driver and the revision its console line
 * prints. The high word also satisfies mainline Linux's cruder scan, which
 * accepts anything that is neither all-zeroes nor all-ones.
 */
#define DP83840_IDENTITY_HIGH_VALUE 0x2000
#define DP83840_IDENTITY_LOW_VALUE  0x5c01

/* Control register bits the drivers write. */
#define DP83840_CTRL_RESET      0x8000  /* self-clearing */
#define DP83840_CTRL_LOOPBACK   0x4000
#define DP83840_CTRL_SPEED_100  0x2000
#define DP83840_CTRL_AUTONEG    0x1000
#define DP83840_CTRL_ISOLATE    0x0400
#define DP83840_CTRL_RESTART    0x0200  /* self-clearing */
#define DP83840_CTRL_DUPLEX     0x0100

/* Status register bits the drivers read. */
#define DP83840_STAT_100_FDX    0x4000
#define DP83840_STAT_100_HDX    0x2000
#define DP83840_STAT_10_FDX     0x1000
#define DP83840_STAT_10_HDX     0x0800
#define DP83840_STAT_AUTONEG_OK 0x0020
#define DP83840_STAT_CAN_AUTONEG 0x0008
#define DP83840_STAT_LINK       0x0004
#define DP83840_STAT_EXTENDED   0x0001

/*
 * Advertisement and link-partner bits. The status path intersects the two
 * words and reads the media out of the result, so the partner has to advertise
 * something the station also advertises or the guest concludes there is no
 * usable media.
 */
#define DP83840_LINK_100_FDX    0x0100
#define DP83840_LINK_100_HDX    0x0080
#define DP83840_LINK_10_FDX     0x0040
#define DP83840_LINK_10_HDX     0x0020
#define DP83840_LINK_SELECTOR   0x0001  /* 802.3 */

/*
 * The expansion register's one bit that matters: whether the far end can
 * autonegotiate. It chooses which of two routes the driver takes to a media
 * type, and only one of them is a route the driver's own comment trusts.
 */
#define DP83840_EXP_PARTNER_AUTONEG 0x0001

/* The address-and-status register: its low bits report the part's own address. */
#define DP83840_ADDR_MASK       0x001f
#define DP83840_ADDR_10MBIT     0x0040

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
#define DP83840_CONTROL_RESET_VALUE  (DP83840_CTRL_SPEED_100 | \
                                      DP83840_CTRL_AUTONEG | \
                                      DP83840_CTRL_DUPLEX)
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

struct DP83840State {
    SysBusDevice parent_obj;

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
     * has written this part -- and true once a guest configures the control
     * register to bring the interface up. See dp83840_write() and
     * dp83840_reset(). Gates the link and completion bits in the status word
     * and whether the partner and expansion registers answer non-zero.
     */
    bool autoneg_done;
};

void dp83840_reset(DP83840State *s);
bool dp83840_in_loopback(DP83840State *s);
uint16_t dp83840_read(DP83840State *s, int reg);
void dp83840_write(DP83840State *s, int reg, uint16_t val);

#endif
