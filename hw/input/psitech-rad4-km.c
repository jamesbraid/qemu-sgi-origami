/* SPDX-License-Identifier: BSD-3-Clause */
/* PsiTech RAD4-KM keyboard and mouse controller. */
/*
 * See include/hw/input/psitech-rad4-km.h for the register contract this file
 * implements and the disclosed
 * approximations (chip identity, and the undriven board-level interrupt gate
 * at driver offset 0x78).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/input/psitech-rad4-km.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "trace.h"

static void rad4_km_write_mr(Rad4KMChannel *chan, uint8_t val)
{
    if (chan->mr_ptr == 0) {
        chan->mr1 = val;
        chan->mr_ptr = 1;
    } else {
        /* No established auto-wrap; MR2 stays selected until CR resets it. */
        chan->mr2 = val;
    }
}

static void rad4_km_write_cr(Rad4KMChannel *chan, uint8_t val)
{
    uint8_t misc = (val >> RAD4_KM_CR_MISC_SHIFT) & RAD4_KM_CR_MISC_MASK;

    switch (val & RAD4_KM_CR_RX_MASK) {
    case 0:
        break;
    case RAD4_KM_CR_RX_ENABLE:
        chan->rx_enabled = true;
        break;
    case RAD4_KM_CR_RX_DISABLE:
        chan->rx_enabled = false;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4-km: reserved CR Rx field (val 0x%02x)\n",
                      val);
        break;
    }

    switch (val & RAD4_KM_CR_TX_MASK) {
    case 0:
        break;
    case RAD4_KM_CR_TX_ENABLE:
        chan->tx_enabled = true;
        break;
    case RAD4_KM_CR_TX_DISABLE:
        chan->tx_enabled = false;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4-km: reserved CR Tx field (val 0x%02x)\n",
                      val);
        break;
    }

    switch (misc) {
    case 0:
        break;
    case RAD4_KM_CR_MISC_RESET_MR:
        chan->mr_ptr = 0;
        break;
    case RAD4_KM_CR_MISC_RESET_RX:
    case RAD4_KM_CR_MISC_RESET_TX:
    case RAD4_KM_CR_MISC_RESET_ERROR:
    case RAD4_KM_CR_MISC_RESET_BRK:
        /*
         * Established as issued at driver init, but this model has no
         * separate receive FIFO, error-status or
         * break-change state for these commands to act on beyond what the ps2
         * core devices already own, so no
         * register bit changes here.
         *
         * Real SCN2681-family silicon additionally disables the receiver on
         * Reset Receiver and the transmitter on
         * Reset Transmitter, independent of the same command byte's own Rx/Tx
         * enable field above. This model does not clear rx_enabled/tx_enabled
         * here, so it is untested and
         * possibly divergent for a future guest access
         * that issues one of these MISC commands with Rx/Tx left enabled in
         * the same byte. The driver's own init
         * sequence (0x1a, then 0x20, 0x30, 0x40, 0x50) issues these commands
         * only after 0x1a has already disabled
         * both, so the byte's own Rx/Tx field already reads disabled here,
         * not a divergence from any exercised
         * guest behavior today.
         */
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4-km: unimplemented CR MISC command %u\n",
                      misc);
        break;
    }
}

static uint8_t rad4_km_read_sr(Rad4KMChannel *chan, PS2State *ps2)
{
    uint8_t val = 0;

    if (chan->rx_enabled && !ps2_queue_empty(ps2)) {
        val |= RAD4_KM_SR_RXRDY;
    }
    if (chan->tx_enabled) {
        /* Transmit is synchronous in this model: always ready once
         * enabled. */
        val |= RAD4_KM_SR_TXRDY;
    }
    return val;
}

static uint8_t rad4_km_read_isr(PsitechRad4KMState *s)
{
    uint8_t val = 0;

    if (s->chan_a.tx_enabled) {
        val |= RAD4_KM_ISR_TXRDY_A;
    }
    if (s->chan_a.rx_enabled && !ps2_queue_empty(PS2_DEVICE(&s->kbd))) {
        val |= RAD4_KM_ISR_RXRDY_A;
    }
    if (s->chan_b.tx_enabled) {
        val |= RAD4_KM_ISR_TXRDY_B;
    }
    if (s->chan_b.rx_enabled && !ps2_queue_empty(PS2_DEVICE(&s->mouse))) {
        val |= RAD4_KM_ISR_RXRDY_B;
    }
    return val;
}

/*
 * IMR gating which ISR bits reach the chip's own interrupt output is the
 * ordinary SCN2681/SCC68681 DUART family
 * contract (see the file header): a public, chip-family fact, unlike the
 * board-level gate at driver offset 0x78
 * this model does not invent. ISR's own read behavior above stays the raw,
 * unmasked value. Only this aggregate output is masked.
 */
static void rad4_km_update_irq(PsitechRad4KMState *s)
{
    qemu_set_irq(s->irq, !!(rad4_km_read_isr(s) & s->imr));
}

/*
 * Either ps2 core device's own IRQ output changes only its queue-empty state
 * (ps2_read_data()/ps2_write_*() already
 * call ps2_raise_irq()/ ps2_lower_irq()); this model recomputes the chip-wide
 * masked condition from scratch on
 * every edge rather than tracking each source bit incrementally, matching
 * rad4_km_read_isr()'s own always-recompute
 * style.
 */
static void rad4_km_ps2_irq(void *opaque, int n, int level)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(opaque);

    rad4_km_update_irq(s);
}

static uint8_t rad4_km_read_data(PS2State *ps2)
{
    if (ps2_queue_empty(ps2)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "psitech-rad4-km: data register read with an empty "
                      "queue\n");
        return 0;
    }
    return ps2_read_data(ps2);
}

static uint64_t rad4_km_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(opaque);
    unsigned index = addr / RAD4_KM_REG_STRIDE;
    uint64_t val = 0;

    switch (index) {
    case RAD4_KM_REG_SRA_CSRA:
        val = rad4_km_read_sr(&s->chan_a, PS2_DEVICE(&s->kbd));
        break;

    case RAD4_KM_REG_RHRA_THRA:
        val = rad4_km_read_data(PS2_DEVICE(&s->kbd));
        break;

    case RAD4_KM_REG_SRB_CSRB:
        val = rad4_km_read_sr(&s->chan_b, PS2_DEVICE(&s->mouse));
        break;

    case RAD4_KM_REG_RHRB_THRB:
        val = rad4_km_read_data(PS2_DEVICE(&s->mouse));
        break;

    case RAD4_KM_REG_ISR_IMR:
        val = rad4_km_read_isr(s);
        break;

    case RAD4_KM_REG_MRA:
    case RAD4_KM_REG_MRB:
    case RAD4_KM_REG_CRA:
    case RAD4_KM_REG_CRB:
    case RAD4_KM_REG_ACR:
    case RAD4_KM_REG_OPCR:
    case RAD4_KM_REG_CTUR:
    case RAD4_KM_REG_CTLR:
    case RAD4_KM_REG_IVR:
    case RAD4_KM_REG_OPR_SET:
    case RAD4_KM_REG_OPR_RESET:
        /* No Established read behavior at this index; never guess one. */
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4-km: read of unimplemented register "
                      "index 0x%x\n", index);
        break;

    default:
        g_assert_not_reached();
    }

    trace_psitech_rad4_km_read(addr, val);
    return val;
}

static void rad4_km_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(opaque);
    unsigned index = addr / RAD4_KM_REG_STRIDE;
    uint8_t byte = val & 0xff;

    trace_psitech_rad4_km_write(addr, val, size);
    switch (index) {
    case RAD4_KM_REG_MRA:
        rad4_km_write_mr(&s->chan_a, byte);
        return;

    case RAD4_KM_REG_MRB:
        rad4_km_write_mr(&s->chan_b, byte);
        return;

    case RAD4_KM_REG_SRA_CSRA:
        /* CSRA: clock select, functionally inert; no baud timing is
         * modeled. */
        s->chan_a.csr = byte;
        return;

    case RAD4_KM_REG_SRB_CSRB:
        s->chan_b.csr = byte;
        return;

    case RAD4_KM_REG_CRA:
        rad4_km_write_cr(&s->chan_a, byte);
        rad4_km_update_irq(s);
        return;

    case RAD4_KM_REG_CRB:
        rad4_km_write_cr(&s->chan_b, byte);
        rad4_km_update_irq(s);
        return;

    case RAD4_KM_REG_RHRA_THRA:
        ps2_write_keyboard(&s->kbd, byte);
        return;

    case RAD4_KM_REG_RHRB_THRB:
        ps2_write_mouse(&s->mouse, byte);
        return;

    case RAD4_KM_REG_ACR:
        /* Established as a single zero-write at init; stored, inert. */
        s->acr = byte;
        return;

    case RAD4_KM_REG_ISR_IMR:
        s->imr = byte;
        rad4_km_update_irq(s);
        return;

    case RAD4_KM_REG_OPCR:
        /* Established as a single zero-write at init; stored, inert. */
        s->opcr = byte;
        return;

    case RAD4_KM_REG_OPR_SET:
        s->output_port |= byte;
        return;

    case RAD4_KM_REG_OPR_RESET:
        s->output_port &= ~byte;
        return;

    case RAD4_KM_REG_CTUR:
    case RAD4_KM_REG_CTLR:
    case RAD4_KM_REG_IVR:
        /* Confirmed never accessed by the driver; never guess a behavior. */
        qemu_log_mask(LOG_UNIMP,
                      "psitech-rad4-km: write of 0x%02x to unimplemented "
                      "register index 0x%x\n", byte, index);
        return;

    default:
        g_assert_not_reached();
    }
}

static const MemoryRegionOps rad4_km_ops = {
    .read = rad4_km_read,
    .write = rad4_km_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rad4_km_init(Object *obj)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "kbd", &s->kbd, TYPE_PS2_KBD_DEVICE);
    object_initialize_child(obj, "mouse", &s->mouse, TYPE_PS2_MOUSE_DEVICE);

    memory_region_init_io(&s->mmio, obj, &rad4_km_ops, s, "psitech-rad4-km",
                          RAD4_KM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    qdev_init_gpio_in(DEVICE(obj), rad4_km_ps2_irq, 2);
    sysbus_init_irq(sbd, &s->irq);
}

static void rad4_km_realize(DeviceState *dev, Error **errp)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->kbd), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mouse), errp)) {
        return;
    }

    /*
     * Each ps2 core device's own IRQ output feeds this chip's aggregate ISR/
     * IMR condition (rad4_km_ps2_irq()); see
     * the file header and include/hw/input/psitech-rad4-km.h for why this
     * model wires that much and no further,
     * leaving the board-level interrupt gate at driver offset 0x78 unwired
     * rather than guessing its destination.
     */
    qdev_connect_gpio_out(DEVICE(&s->kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in(dev, 0));
    qdev_connect_gpio_out(DEVICE(&s->mouse), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in(dev, 1));
}

static void rad4_km_reset_hold(Object *obj, ResetType type)
{
    PsitechRad4KMState *s = PSITECH_RAD4_KM(obj);

    memset(&s->chan_a, 0, sizeof(s->chan_a));
    memset(&s->chan_b, 0, sizeof(s->chan_b));
    s->acr = 0;
    s->imr = 0;
    s->opcr = 0;
    s->output_port = 0;

    rad4_km_update_irq(s);
}

static const VMStateDescription vmstate_rad4_km_channel = {
    .name = "psitech-rad4-km-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(mr_ptr, Rad4KMChannel),
        VMSTATE_UINT8(mr1, Rad4KMChannel),
        VMSTATE_UINT8(mr2, Rad4KMChannel),
        VMSTATE_UINT8(csr, Rad4KMChannel),
        VMSTATE_BOOL(rx_enabled, Rad4KMChannel),
        VMSTATE_BOOL(tx_enabled, Rad4KMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_psitech_rad4_km = {
    .name = "psitech-rad4-km",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(chan_a, PsitechRad4KMState, 0,
                      vmstate_rad4_km_channel, Rad4KMChannel),
        VMSTATE_STRUCT(chan_b, PsitechRad4KMState, 0,
                      vmstate_rad4_km_channel, Rad4KMChannel),
        VMSTATE_UINT8(acr, PsitechRad4KMState),
        VMSTATE_UINT8(imr, PsitechRad4KMState),
        VMSTATE_UINT8(opcr, PsitechRad4KMState),
        VMSTATE_UINT8(output_port, PsitechRad4KMState),
        VMSTATE_END_OF_LIST()
    }
};

static void rad4_km_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "PsiTech RAD4-KM keyboard/mouse controller";
    dc->realize = rad4_km_realize;
    dc->vmsd = &vmstate_psitech_rad4_km;
    /* Instantiated only by psitech-rad4, which wires it onto its BAR 3. */
    dc->user_creatable = false;
    rc->phases.hold = rad4_km_reset_hold;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo psitech_rad4_km_types[] = {
    {
        .name = TYPE_PSITECH_RAD4_KM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PsitechRad4KMState),
        .instance_init = rad4_km_init,
        .class_init = rad4_km_class_init,
    },
};

DEFINE_TYPES(psitech_rad4_km_types)
