/* SPDX-License-Identifier: BSD-3-Clause */
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

/* Only what hw/net/mii.h has no name for. */
#define DP83840_PCS_CONFIG      0x17
#define DP83840_LOOPBACK_BYPASS 0x18
#define DP83840_ADDRESS_STATUS  0x19
#define DP83840_10BT_CONFIG     0x1c

/* The specification's reset values; SGI's transceiver test compares them. */
#define DP83840_CONTROL_RESET_VALUE  (MII_BMCR_SPEED100 | \
                                      MII_BMCR_AUTOEN | \
                                      MII_BMCR_FD)
#define DP83840_PCS_RESET_VALUE      0x8040
#define DP83840_10BT_RESET_VALUE     0x80b9

/* Pin-latched bits unobserved; modelled as normal operation. */
#define DP83840_LBREMR_RESET_VALUE   0x8020

/* Loopback selections apart from the control register's own bit. */
#define DP83840_LBREMR_LB_MASK       0x0300
#define DP83840_LBREMR_10BT_LOOPBACK 0x0800

/*
 * National Semiconductor DP83843 "PHYTER": the Origin 300 IO-8 board's
 * transceiver, selected by the "dp83843" property instead of the default
 * DP83840A. Register 0x17 (DP83840_PCS_CONFIG's address, plain storage on
 * the DP83840A) is its Loopback Register, and register 0x18 (the DP83840A's
 * LBREMR) is its 10BTSCR, plain storage with no loopback function.
 */
#define DP83843_PHYID2          0x5c10
#define DP83843_ANNPTR          0x07
#define DP83843_ANNPTR_RW_MASK  0xb7ff
#define DP83843_ANNPTR_TOGGLE   0x0800
#define DP83843_ANNPTR_RESET    0x2801
#define DP83843_LBR_10_ENDEC_LB 0x0010
#define DP83843_LBR_TW_LBEN     0x0020

/*
 * In the specification's 1.5-2.3 s range, and long enough that the
 * firmware diagnostic's sweep still finds the link settling.
 */
#define DP83840_AUTONEG_SETTLE_NS (2 * NANOSECONDS_PER_SECOND)

struct DP83840State {
    SysBusDevice parent_obj;

    /* Default 31, where every real capture prints the transceiver. */
    uint8_t addr;

    /* False: DP83840A (Origin 200). True: DP83843 (Origin 300 IO-8). */
    bool dp83843;

    uint16_t control;
    uint16_t advertise;
    uint16_t next_page;
    uint16_t pcs_config;

    /* Only SGI's power-on diagnostic reaches these; it fails without them. */
    uint16_t loopback_bypass;
    uint16_t tenbt_config;

    bool autoneg_done;

    bool link_down;     /* the cable, apart from the negotiation */

    /* Armed on the first status read; see dp83840_status_read(). */
    QEMUTimer *settle_timer;
    bool settle_armed;
};

void dp83840_reset(DP83840State *s);
void dp83840_update_link(DP83840State *s, bool link_down);
bool dp83840_in_loopback(DP83840State *s);
uint16_t dp83840_read(DP83840State *s, int reg);
void dp83840_write(DP83840State *s, int reg, uint16_t val);

#endif
