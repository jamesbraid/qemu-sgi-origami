/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Broadcom NetXtreme BCM570x integrated copper PHY, reached through the
 * MAC's MII management registers.
 */

#ifndef HW_NET_BCM570X_PHY_H
#define HW_NET_BCM570X_PHY_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM570X_PHY "bcm570x-phy"
OBJECT_DECLARE_SIMPLE_TYPE(Bcm570xPhyState, BCM570X_PHY)

/* The standard MII management registers this model answers. */
#define BCM570X_PHY_BMCR        0x00    /* control */
#define BCM570X_PHY_BMSR        0x01    /* status */
#define BCM570X_PHY_IDR1        0x02    /* identity high */
#define BCM570X_PHY_IDR2        0x03    /* identity low */
#define BCM570X_PHY_ANAR        0x04    /* autoneg advertisement */
#define BCM570X_PHY_ANLPAR      0x05    /* autoneg link partner */
#define BCM570X_PHY_GTCR        0x09    /* 1000baseT control */
#define BCM570X_PHY_GTSR        0x0a    /* 1000baseT status */
#define BCM570X_PHY_EXTSR       0x0f    /* extended status */
#define BCM570X_PHY_AUXSTS      0x19    /* Broadcom auxiliary status summary */

/*
 * The identity brgphy binds the BCM5701 on: the "xxBROADCOM" OUI 0x000818 and
 * model 0x11, encoded per IEEE 802.3-22.2.4.3.1 into the two id words --
 * id1 = OUI >> 6, id2 top six bits = OUI & 0x3f, then model << 4, then the
 * revision nibble. The revision (0 here) is not part of the match.
 */
#define BCM570X_PHY_IDR1_VALUE  0x0020
#define BCM570X_PHY_IDR2_BCM5701 0x6110

/* BMCR (control) bits. */
#define BCM570X_PHY_BMCR_RESET    0x8000    /* self-clearing */
#define BCM570X_PHY_BMCR_LOOP     0x4000
#define BCM570X_PHY_BMCR_SPEED100 0x2000
#define BCM570X_PHY_BMCR_AUTOEN   0x1000
#define BCM570X_PHY_BMCR_ISOLATE  0x0400
#define BCM570X_PHY_BMCR_STARTNEG 0x0200    /* self-clearing */
#define BCM570X_PHY_BMCR_FDX      0x0100
#define BCM570X_PHY_BMCR_SPEED1000 0x0040

/* BMSR (status) bits. */
#define BCM570X_PHY_BMSR_100TXFDX   0x4000
#define BCM570X_PHY_BMSR_100TXHDX   0x2000
#define BCM570X_PHY_BMSR_10TFDX     0x1000
#define BCM570X_PHY_BMSR_10THDX     0x0800
#define BCM570X_PHY_BMSR_EXTSTAT    0x0100
#define BCM570X_PHY_BMSR_ANEGDONE   0x0020
#define BCM570X_PHY_BMSR_CANAUTONEG 0x0008
#define BCM570X_PHY_BMSR_LINK       0x0004
#define BCM570X_PHY_BMSR_EXTCAP     0x0001

/* ANAR / ANLPAR bits. */
#define BCM570X_PHY_ANAR_100TXFDX   0x0100
#define BCM570X_PHY_ANAR_100TXHDX   0x0080
#define BCM570X_PHY_ANAR_10TFDX     0x0040
#define BCM570X_PHY_ANAR_10THDX     0x0020
#define BCM570X_PHY_ANAR_CSMA       0x0001  /* 802.3 selector */
#define BCM570X_PHY_ANLPAR_ACK      0x4000

/* 1000baseT control / status / extended-status bits. */
#define BCM570X_PHY_GTCR_ADV_1000TFDX 0x0200
#define BCM570X_PHY_GTSR_MASTER       0x4000
#define BCM570X_PHY_GTSR_LOCALRXOK    0x2000
#define BCM570X_PHY_GTSR_REMOTERXOK   0x1000
#define BCM570X_PHY_GTSR_LP_1000TFDX  0x0800
#define BCM570X_PHY_EXTSR_1000TFDX    0x2000
#define BCM570X_PHY_EXTSR_1000THDX    0x1000

/*
 * Auxiliary status summary (BRGPHY_MII_AUXSTS) bits. ACOMP is the driver's
 * autonegotiation-complete gate; the AN_RES field holds the negotiated
 * highest-common-denominator result, 0x0700 being 1000baseT full duplex. A
 * brgphy copper link-up reads this register for the settled speed and duplex.
 */
#define BCM570X_PHY_AUXSTS_ACOMP      0x8000
#define BCM570X_PHY_AUXSTS_1000TFDX   0x0700

/* Reset values: autonegotiating, advertising 10/100 and 1000baseT full. */
#define BCM570X_PHY_BMCR_RESET_VALUE (BCM570X_PHY_BMCR_AUTOEN | \
                                      BCM570X_PHY_BMCR_FDX | \
                                      BCM570X_PHY_BMCR_SPEED1000)
#define BCM570X_PHY_ANAR_RESET_VALUE (BCM570X_PHY_ANAR_CSMA | \
                                      BCM570X_PHY_ANAR_10THDX | \
                                      BCM570X_PHY_ANAR_10TFDX | \
                                      BCM570X_PHY_ANAR_100TXHDX | \
                                      BCM570X_PHY_ANAR_100TXFDX)
#define BCM570X_PHY_GTCR_RESET_VALUE BCM570X_PHY_GTCR_ADV_1000TFDX

/*
 * How long autonegotiation takes to settle once the interface first reads
 * link status, for a driver that only polls. QEMU's e1000 and the in-tree
 * DP83840 model time it the same way; two seconds clears any polling driver's
 * timeout with room to spare.
 */
#define BCM570X_PHY_AUTONEG_SETTLE_NS (2 * NANOSECONDS_PER_SECOND)

struct Bcm570xPhyState {
    SysBusDevice parent_obj;

    /* Writable registers, held as the words a driver reads back. */
    uint16_t bmcr;
    uint16_t anar;
    uint16_t gtcr;

    /*
     * Whether autonegotiation has completed. False out of reset; goes true a
     * settle after the interface first reads link status, or at once when a
     * guest writes the control register to configure the interface.
     */
    bool autoneg_done;

    /* The far-end link, driven from the MAC's netdev; no cable, no carrier. */
    bool link_down;

    QEMUTimer *settle_timer;
    bool settle_armed;
};

void bcm570x_phy_reset(Bcm570xPhyState *s);
uint16_t bcm570x_phy_read(Bcm570xPhyState *s, int reg);
void bcm570x_phy_write(Bcm570xPhyState *s, int reg, uint16_t val);
bool bcm570x_phy_link_up(Bcm570xPhyState *s);
void bcm570x_phy_set_link_down(Bcm570xPhyState *s, bool link_down);

#endif
