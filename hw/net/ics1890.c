/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ICS1890 revision 3. Register tables: ICS1890RevG 10/21/97, pp17-35.
 * Reviewed contract: Origami docs/datasheets/ics1890.md. Packet networking
 * approximates a peer advertising all four supported modes, with a 100ms
 * virtual negotiation delay. Electrical test modes remain unsupported.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/net/ics1890.h"
#include "hw/net/mii.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "trace.h"

#define ICS_ECR 16
#define ICS_QPR 17
#define ICS_TENBT 18
#define ICS_ECR2 19
#define ICS_PEER 0x01e1
#define ICS_NEGOTIATION_NS (NANOSECONDS_PER_SECOND / 10)

static bool ics1890_powerdown(ICS1890State *s)
{
    return s->regs[MII_BMCR] & MII_BMCR_PDOWN;
}

bool ics1890_data_enabled(ICS1890State *s)
{
    return !(s->regs[MII_BMCR] & (MII_BMCR_PDOWN | MII_BMCR_ISOLATE));
}

bool ics1890_in_loopback(ICS1890State *s)
{
    return ics1890_data_enabled(s) &&
           (s->regs[MII_BMCR] & MII_BMCR_LOOPBACK);
}

static bool ics1890_live_link(ICS1890State *s)
{
    if (ics1890_powerdown(s)) {
        return false;
    }
    if (s->regs[MII_BMCR] & MII_BMCR_AUTOEN) {
        return s->autoneg_done && !s->link_down;
    }
    return !s->link_down || (!s->speed100 && (s->regs[ICS_TENBT] & 2));
}

static void ics1890_set_link_low(ICS1890State *s)
{
    if (!ics1890_powerdown(s)) {
        s->link_low = 3;
    }
}

static bool ics1890_can_negotiate(ICS1890State *s)
{
    uint16_t adv = s->regs[MII_ANAR];

    return !ics1890_powerdown(s) && !s->link_down &&
           (s->regs[MII_BMCR] & MII_BMCR_AUTOEN) &&
           (adv & MII_ANAR_SELECT) == 1 &&
           !(adv & MII_ANAR_RFAULT) && (adv & ICS_PEER & 0x01e0);
}

static void ics1890_negotiation_done(void *opaque)
{
    ICS1890State *s = opaque;
    uint16_t common = s->regs[MII_ANAR] & ICS_PEER;

    if (!ics1890_can_negotiate(s)) {
        return;
    }
    s->autoneg_done = true;
    s->speed100 = common & (MII_ANAR_TXFD | MII_ANAR_TX);
    s->full_duplex = common & (s->speed100 ? MII_ANAR_TXFD : MII_ANAR_10FD);
    s->regs[MII_ANLPAR] = ICS_PEER;
    s->regs[MII_ANER] |= 3;
    s->regs[ICS_QPR] |= 0x3800;
    trace_ics1890_negotiation(s->speed100, s->full_duplex);
}

static void ics1890_configure(ICS1890State *s)
{
    timer_del(s->negotiation_timer);
    if (ics1890_powerdown(s)) {
        return;
    }
    s->autoneg_done = false;
    s->regs[MII_ANLPAR] = 0;
    s->regs[MII_ANER] &= ~1;
    s->speed100 = s->regs[MII_BMCR] & MII_BMCR_SPEED100;
    s->full_duplex = s->regs[MII_BMCR] & MII_BMCR_FD;
    ics1890_set_link_low(s);
    if (ics1890_can_negotiate(s)) {
        timer_mod(s->negotiation_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ICS_NEGOTIATION_NS);
    }
}

void ics1890_update_link(ICS1890State *s, bool link_down)
{
    if (s->link_down == link_down) {
        return;
    }
    s->link_down = link_down;
    ics1890_configure(s);
}

static uint16_t ics1890_quick_poll_status(ICS1890State *s)
{
    return (s->speed100 ? 0x8000 : 0) |
           (s->full_duplex ? 0x4000 : 0) |
           (s->autoneg_done ? 0x0010 : 0) |
           (s->link_down ? 0x0008 : 0);
}

static uint16_t ics1890_transmitted_fault(ICS1890State *s)
{
    /* An up synthetic peer supplies receive activity (RevG p35). */
    return (s->regs[MII_ANAR] & MII_ANAR_RFAULT) ||
           ((s->regs[ICS_ECR2] & 0x10) && s->link_down) ? 0x0400 : 0;
}

static bool ics1890_implemented(unsigned reg)
{
    return reg <= MII_ANER || (reg >= ICS_ECR && reg <= ICS_ECR2);
}

uint16_t ics1890_read(ICS1890State *s, unsigned reg)
{
    uint16_t val;

    if (!ics1890_implemented(reg)) {
        qemu_log_mask(LOG_UNIMP, "ics1890: read of unimplemented register %u\n",
                      reg);
        return 0xffff;
    }
    val = s->regs[reg];
    switch (reg) {
    case MII_BMSR:
        val |= s->autoneg_done ? MII_BMSR_AN_COMP : 0;
        if (ics1890_live_link(s) && !(s->link_low & 1)) {
            val |= MII_BMSR_LINK_ST;
        }
        s->link_low &= ~1;
        s->regs[reg] &= ~0x0012;
        break;
    case MII_ANER:
        s->regs[reg] &= ~0x0012;
        break;
    case ICS_QPR:
        val |= ics1890_powerdown(s) ? s->powerdown_qpr :
                                     ics1890_quick_poll_status(s);
        if (ics1890_live_link(s) && !(s->link_low & 2)) {
            val |= 1;
        }
        s->link_low &= ~2;
        s->regs[reg] &= ~0x3fe6;
        if (s->autoneg_done && !ics1890_powerdown(s)) {
            s->regs[reg] |= 0x3800;
        }
        break;
    case ICS_TENBT:
        s->regs[reg] &= ~0x4000;
        break;
    case ICS_ECR2:
        val |= ics1890_powerdown(s) ? s->powerdown_fault :
                                     ics1890_transmitted_fault(s);
        break;
    }
    trace_ics1890_read(val, reg);
    return val;
}

static void ics1890_reset(ICS1890State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MII_BMCR] = 0x3000 | (s->addr ? 0 : MII_BMCR_ISOLATE);
    s->regs[MII_BMSR] = 0x7809;
    s->regs[MII_PHYID1] = 0x0015;
    s->regs[MII_PHYID2] = 0xf423;
    s->regs[MII_ANAR] = ICS_PEER;
    s->regs[ICS_ECR] = (s->addr << 6) | 8;
    s->regs[ICS_TENBT] = 0x0010;
    s->regs[ICS_ECR2] = 0x4001;
    s->link_low = 3;
    s->powerdown_qpr = 0;
    s->powerdown_fault = 0;
    ics1890_configure(s);
}

void ics1890_write(ICS1890State *s, unsigned reg, uint16_t val)
{
    bool override = s->regs[ICS_ECR] & 0x8000;
    uint16_t mask = 0;
    uint16_t old;

    trace_ics1890_write(val, reg);
    /* Every addressed write consumes override, including unsupported ones. */
    s->regs[ICS_ECR] &= ~0x8000;
    if (!ics1890_implemented(reg)) {
        qemu_log_mask(LOG_UNIMP,
                      "ics1890: write to unimplemented register %u\n",
                      reg);
        return;
    }
    old = s->regs[reg];
    switch (reg) {
    case MII_BMCR:
        if (val & MII_BMCR_RESET) {
            ics1890_reset(s);
            return;
        }
        s->regs[reg] = val & 0xfd80;
        if (val & MII_BMCR_CTST) {
            qemu_log_mask(LOG_UNIMP, "ics1890: collision electrical test\n");
        }
        if (!(old & MII_BMCR_PDOWN) && ics1890_powerdown(s)) {
            s->powerdown_qpr = ics1890_quick_poll_status(s);
            s->powerdown_fault = ics1890_transmitted_fault(s);
            s->regs[MII_BMSR] &= ~0x0016;
            s->regs[MII_ANER] &= ~0x0012;
            s->regs[ICS_QPR] &= ~0x3fe7;
            s->regs[ICS_TENBT] &= ~0x4000;
            s->link_low = 3;
        } else if ((old & MII_BMCR_PDOWN) && !ics1890_powerdown(s)) {
            s->regs[MII_BMSR] &= ~0x0012;
            s->regs[MII_ANER] &= ~0x0010;
            s->regs[ICS_QPR] &= ~0x07e6;
        }
        if (old != s->regs[reg] ||
            ((val & MII_BMCR_ANRESTART) && (val & MII_BMCR_AUTOEN))) {
            ics1890_configure(s);
        }
        return;
    case MII_BMSR:
        mask = override ? 0x7f80 : 0;
        break;
    case MII_PHYID1:
    case MII_PHYID2:
        mask = override ? 0xffff : 0;
        break;
    case MII_ANAR:
        mask = override ? 0x3dff : 0x21e0;
        break;
    case MII_ANER:
        mask = override ? 0xffe0 : 0;
        break;
    case ICS_ECR:
        mask = 0x002d | (override ? 0 : 0x8000);
        if (val & 0x7812) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ics1890: reserved extended-control bits\n");
        }
        if ((val & 0x25) || !(val & 8)) {
            qemu_log_mask(LOG_UNIMP, "ics1890: electrical encoding test\n");
        }
        break;
    case ICS_TENBT:
        mask = 0x002f;
        if ((val & 0xbfc0) || !(val & 0x10)) {
            qemu_log_mask(LOG_GUEST_ERROR, "ics1890: reserved 10BASE-T bits\n");
        }
        if (val & 0x2d) {
            qemu_log_mask(LOG_UNIMP, "ics1890: analog 10BASE-T controls\n");
        }
        break;
    case ICS_ECR2:
        mask = 0x0011;
        if (val & 0x1fee) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ics1890: reserved extended-control-2 bits\n");
        }
        break;
    }
    s->regs[reg] = (old & ~mask) | (val & mask);
    if (reg == MII_ANAR && old != s->regs[reg]) {
        ics1890_configure(s);
    }
}

static void ics1890_reset_hold(Object *obj, ResetType type)
{
    ics1890_reset(ICS1890(obj));
}

static void ics1890_realize(DeviceState *dev, Error **errp)
{
    ICS1890State *s = ICS1890(dev);

    if (s->addr > 31) {
        error_setg(errp, "ics1890: PHY address must be at most 31");
        return;
    }
    s->negotiation_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ics1890_negotiation_done, s);
}

static void ics1890_unrealize(DeviceState *dev)
{
    ICS1890State *s = ICS1890(dev);

    timer_free(s->negotiation_timer);
    s->negotiation_timer = NULL;
}

static const VMStateDescription vmstate_ics1890 = {
    .name = "ics1890",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, ICS1890State, 32),
        VMSTATE_UINT8(link_low, ICS1890State),
        VMSTATE_UINT16(powerdown_qpr, ICS1890State),
        VMSTATE_UINT16(powerdown_fault, ICS1890State),
        VMSTATE_BOOL(link_down, ICS1890State),
        VMSTATE_BOOL(autoneg_done, ICS1890State),
        VMSTATE_BOOL(speed100, ICS1890State),
        VMSTATE_BOOL(full_duplex, ICS1890State),
        VMSTATE_TIMER_PTR(negotiation_timer, ICS1890State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ics1890_properties[] = {
    DEFINE_PROP_UINT8("addr", ICS1890State, addr, 1),
};

static void ics1890_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "ICS1890 10/100 Ethernet transceiver";
    dc->realize = ics1890_realize;
    dc->unrealize = ics1890_unrealize;
    dc->vmsd = &vmstate_ics1890;
    dc->user_creatable = false;
    device_class_set_props(dc, ics1890_properties);
    rc->phases.hold = ics1890_reset_hold;
}

static const TypeInfo ics1890_type = {
    .name = TYPE_ICS1890,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ICS1890State),
    .class_init = ics1890_class_init,
};

static void ics1890_register_types(void)
{
    type_register_static(&ics1890_type);
}

type_init(ics1890_register_types)
