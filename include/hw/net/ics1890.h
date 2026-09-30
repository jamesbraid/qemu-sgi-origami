/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_NET_ICS1890_H
#define HW_NET_ICS1890_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_ICS1890 "ics1890"
OBJECT_DECLARE_SIMPLE_TYPE(ICS1890State, ICS1890)

struct ICS1890State {
    SysBusDevice parent_obj;
    uint8_t addr;
    uint16_t regs[32];
    uint8_t link_low;
    uint16_t powerdown_qpr;
    uint16_t powerdown_fault;
    bool link_down;
    bool autoneg_done;
    bool speed100;
    bool full_duplex;
    QEMUTimer *negotiation_timer;
};

uint16_t ics1890_read(ICS1890State *s, unsigned reg);
void ics1890_write(ICS1890State *s, unsigned reg, uint16_t val);
void ics1890_update_link(ICS1890State *s, bool link_down);
bool ics1890_in_loopback(ICS1890State *s);
bool ics1890_data_enabled(ICS1890State *s);

#endif
