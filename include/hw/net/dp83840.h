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
 * Only what hw/net/mii.h has no name for: the standard registers, their
 * bits and this part's identifier words are mii.h's vocabulary.
 */
#define DP83840_PCS_CONFIG      0x17
#define DP83840_LOOPBACK_BYPASS 0x18
#define DP83840_ADDRESS_STATUS  0x19
#define DP83840_10BT_CONFIG     0x1c

/*
 * Reset values from the part's published specification; SGI's power-on
 * transceiver test compares the same numbers independently.
 */
#define DP83840_CONTROL_RESET_VALUE  (MII_BMCR_SPEED100 | \
                                      MII_BMCR_AUTOEN | \
                                      MII_BMCR_FD)
#define DP83840_PCS_RESET_VALUE      0x8040
#define DP83840_10BT_RESET_VALUE     0x80b9

/*
 * Four of this register's bits latch from package pins at reset and the
 * board's wiring for them is unobserved; modelled as normal operation --
 * no bypass, no loopback -- with the specification's two fixed bits set.
 */
#define DP83840_LBREMR_RESET_VALUE   0x8020

/*
 * Loopback selections separate from the control register's own: bits 9-8
 * are the hundred-megabit loopbacks, bit 11 the ten-megabit encoder's.
 * Any of them is loopback as far as the controller is concerned.
 */
#define DP83840_LBREMR_LB_MASK       0x0300
#define DP83840_LBREMR_10BT_LOOPBACK 0x0800

/*
 * Autonegotiation settle delay: the part's specification gives 1.5-2.3
 * seconds. Two seconds must also outlast the firmware diagnostic's
 * reset-value sweep (~0.1 s of virtual time, measured), which needs to
 * find the link still settling across all three registers it reads.
 */
#define DP83840_AUTONEG_SETTLE_NS (2 * NANOSECONDS_PER_SECOND)

struct DP83840State {
    SysBusDevice parent_obj;

    /*
     * Management address, default 31: every real Origin 200 capture prints
     * the transceiver there. A model answering everywhere is found at zero.
     */
    uint8_t addr;

    /*
     * The writable registers; everything else is constant or derived on
     * each read, so derived words cannot disagree with their sources.
     */
    uint16_t control;
    uint16_t advertise;
    uint16_t pcs_config;

    /*
     * Reached only by SGI's power-on diagnostic, which fails the board
     * without them; no guest driver touches either.
     */
    uint16_t loopback_bypass;
    uint16_t tenbt_config;

    /*
     * Whether negotiation has completed. False out of reset (the bench
     * state the power-on diagnostic expects); gates the status word's link
     * and completion bits and the partner/expansion registers.
     */
    bool autoneg_done;

    /*
     * Backend link state (set_link, the pulled cable). Separate from
     * autoneg_done: one is whether negotiation ran, the other whether
     * there is a wire to negotiate over.
     */
    bool link_down;

    /*
     * The settle delay is armed on the first link-status read, not at
     * reset: the power-on diagnostic runs tens of virtual seconds in, and
     * a reset-armed timer would already have fired when its single probe
     * expects a link still settling.
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
