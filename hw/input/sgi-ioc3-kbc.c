/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI IOC3 PS/2 keyboard and mouse controller. */
/*
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The block is a
 * bit-level PS/2 line engine, not an 8042
 * clone: it wraps two generic ps2 devices and presents SGI's five-register
 * interface: a control/status register,
 * a receive register per port and a transmit register per port.
 *
 * Each port's controller holds three received characters. The clamp bits in
 * the control register hold a port's
 * clock line low once it has taken one, or three, of them, which stops the
 * device with the rest still in it; with
 * no clamp armed there is nothing to stop it with and a character that
 * arrives with the slots full is lost. So the
 * two ps2 devices stand in for what is still on the far side of the wire, and
 * the holdings are what this controller
 * has actually received.
 *
 * A real device clocks each character out over about a millisecond, so
 * characters reach a holding one character time apart, not all at once when
 * the host queues them (see SGI_IOC3_KBC_CHAR_NS). A per-port timer on the
 * virtual clock paces the transfers, and a device a clamp has stopped needs a
 * character time after the release before it sends again.
 *
 * See docs/system/mips/sgi/hardware/ioc3.rst for the IOC3 register contract.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/hwaddr.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/input/sgi-ioc3-kbc.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ui/input.h"
#include "trace.h"

static PS2State *sgi_ioc3_kbc_ps2(SGIIOC3KBCState *s, unsigned int port)
{
    return port == SGI_IOC3_KBC_PORT_AUX ? PS2_DEVICE(&s->mouse)
                                         : PS2_DEVICE(&s->kbd);
}

/*
 * How many characters this port may take before its clock line is held low.
 * Zero means no clamp is armed, and then
 * the controller has no way to stop the device: a character that arrives with
 * the three slots full is lost. Arming
 * both clamps is not a combination any driver writes or the documented
 * behavior describes, so the tighter one is
 * taken.
 */
static unsigned int sgi_ioc3_kbc_clamp(SGIIOC3KBCState *s, unsigned int port)
{
    bool aux = port == SGI_IOC3_KBC_PORT_AUX;

    if (s->control & (aux ? SGI_IOC3_KBC_CTRL_M_CLAMP_1
                          : SGI_IOC3_KBC_CTRL_K_CLAMP_1)) {
        return 1;
    }
    if (s->control & (aux ? SGI_IOC3_KBC_CTRL_M_CLAMP_3
                          : SGI_IOC3_KBC_CTRL_K_CLAMP_3)) {
        return SGI_IOC3_KBC_RX_SLOTS;
    }
    return 0;
}

static bool sgi_ioc3_kbc_holding(SGIIOC3KBCState *s)
{
    return s->port[SGI_IOC3_KBC_PORT_KBD].count != 0 ||
           s->port[SGI_IOC3_KBC_PORT_AUX].count != 0;
}

/*
 * The controller's output is the data-available line, and the IOC3 latches it
 * into interrupt-status bit 22, which holds across a drain of the receive
 * registers until the guest writes a one to it. The line is asserted for
 * every character the controller moves into a holding, and falls once a read
 * leaves both holdings empty. Model approximation: the silicon may pulse per
 * character or hold a level over the holding, and neither public driver tells
 * them apart. Per character means an assertion always stands for a character
 * the guest has not read.
 */
static void sgi_ioc3_kbc_arrived(SGIIOC3KBCState *s)
{
    qemu_set_irq(s->irq, 1);
}

static void sgi_ioc3_kbc_drained(SGIIOC3KBCState *s)
{
    if (sgi_ioc3_kbc_holding(s)) {
        return;
    }
    qemu_set_irq(s->irq, 0);
}

/*
 * Move what the device has sent into this port's holding, at the device's
 * pace: one character, then nothing more until a character time later, when
 * the port's timer calls back in. A clamped port that has reached its limit
 * stops there with its clock line low and the rest still in the device; an
 * unclamped one loses the surplus (each loss still takes its character time)
 * and says so through the overflow flag.
 *
 * Nothing lands in the call that releases a clamp: the device restarts a
 * character time after the release, so its interrupt follows the guest's
 * acknowledge of the read that released it. Landing inside the read would
 * put the interrupt before that acknowledge, which clears the latch and
 * strands the character. Model approximation: the spacing is one character
 * time and not a measured value.
 */
static void sgi_ioc3_kbc_fill(SGIIOC3KBCState *s, unsigned int port)
{
    SGIIOC3KBCPort *p = &s->port[port];
    PS2State *ps2;
    unsigned int clamp = sgi_ioc3_kbc_clamp(s, port);
    int64_t now;
    bool arrived = false;

    if (!s->ports_present || s->filling) {
        return;
    }
    ps2 = sgi_ioc3_kbc_ps2(s, port);
    if (ps2_queue_empty(ps2)) {
        return;
    }
    s->filling = true;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (clamp && p->count >= clamp) {
        p->stopped = true;
        timer_del(p->timer);
    } else {
        if (p->stopped) {
            p->stopped = false;
            p->ready_at = MAX(p->ready_at, now + SGI_IOC3_KBC_CHAR_NS);
        }
        if (now >= p->ready_at) {
            uint8_t byte = ps2_read_data(ps2);

            p->ready_at = now + SGI_IOC3_KBC_CHAR_NS;
            if (p->count >= SGI_IOC3_KBC_RX_SLOTS) {
                trace_sgi_ioc3_kbc_overflow(port, byte);
                p->overflow = true;
            } else {
                p->data[p->count++] = byte;
                arrived = true;
            }
        }
        if (ps2_queue_empty(ps2)) {
            timer_del(p->timer);
        } else if (clamp && p->count >= clamp) {
            p->stopped = true;
            timer_del(p->timer);
        } else {
            timer_mod(p->timer, p->ready_at);
        }
    }

    s->filling = false;
    if (arrived) {
        sgi_ioc3_kbc_arrived(s);
    }
}

/* The device has had its character time: let the next one land. */
static void sgi_ioc3_kbc_kbd_tick(void *opaque)
{
    sgi_ioc3_kbc_fill(opaque, SGI_IOC3_KBC_PORT_KBD);
}

static void sgi_ioc3_kbc_aux_tick(void *opaque)
{
    sgi_ioc3_kbc_fill(opaque, SGI_IOC3_KBC_PORT_AUX);
}

/* A ps2 device raises its line when it has something to send. */
static void sgi_ioc3_kbc_set_irq(void *opaque, int n, int level)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);

    trace_sgi_ioc3_kbc_irq(n, level);
    if (level) {
        sgi_ioc3_kbc_fill(s, n);
    }
}

/*
 * Report what this port is holding, oldest character in the highest slot, and
 * empty the holding: the register packs characters since the last read, so
 * the read clears the characters, their flags and the overflow flag together.
 */
static uint32_t sgi_ioc3_kbc_receive(SGIIOC3KBCState *s, unsigned int port)
{
    SGIIOC3KBCPort *p = &s->port[port];
    uint32_t val = 0;
    unsigned int i;

    for (i = 0; i < p->count; i++) {
        unsigned int slot = (SGI_IOC3_KBC_RX_SLOTS - 1) - i;

        val |= SGI_IOC3_KBC_RX_DATA(slot, p->data[i]);
        val |= SGI_IOC3_KBC_RX_VALID(slot);
        /*
         * The per-character error flag (bit 24+slot) stays clear: the ps2
         * layer delivers clean bytes, so nothing
         * received is ever in error.
         */
    }

    if (p->count && port == SGI_IOC3_KBC_PORT_AUX) {
        val |= SGI_IOC3_KBC_RX_SOURCE_AUX;
    }
    if (p->overflow) {
        val |= SGI_IOC3_KBC_RX_OVERFLOW;
    }

    p->count = 0;
    p->overflow = false;
    sgi_ioc3_kbc_drained(s);

    /*
     * The clamp releases with the holding. The fill restarts the stopped
     * device a character time from now, never inside this read.
     */
    sgi_ioc3_kbc_fill(s, port);
    return val;
}

static uint64_t sgi_ioc3_kbc_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);
    uint64_t val;

    switch (addr) {
    case SGI_IOC3_KBC_CONTROL:
        /*
         * Write-pending and the other hardware-status bits read clear: a
         * synchronous transmit completes within the
         * register write, so a driver's post-transmit poll finishes on its
         * first look.
         */
        val = s->control;
        break;

    case SGI_IOC3_KBC_KBD_RECEIVE:
        val = sgi_ioc3_kbc_receive(s, SGI_IOC3_KBC_PORT_KBD);
        break;

    case SGI_IOC3_KBC_AUX_RECEIVE:
        val = sgi_ioc3_kbc_receive(s, SGI_IOC3_KBC_PORT_AUX);
        break;

    default:
        /* The transmit registers are write-only. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-kbc: read of write-only register 0x%"
                      HWADDR_PRIx "\n", addr);
        val = 0;
        break;
    }

    trace_sgi_ioc3_kbc_read(addr, val);
    return val;
}

static void sgi_ioc3_kbc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);

    trace_sgi_ioc3_kbc_write(addr, val, size);
    switch (addr) {
    case SGI_IOC3_KBC_CONTROL:
        s->control = val & SGI_IOC3_KBC_CONTROL_MASK;
        if (val & (SGI_IOC3_KBC_CTRL_EMULATION |
                   SGI_IOC3_KBC_CTRL_FAST_CLOCK)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc3-kbc: emulation/fast-clock effects "
                          "are not modeled\n");
        }
        /* Releasing a clamp lets a stopped device start sending again. */
        sgi_ioc3_kbc_fill(s, SGI_IOC3_KBC_PORT_KBD);
        sgi_ioc3_kbc_fill(s, SGI_IOC3_KBC_PORT_AUX);
        return;

    case SGI_IOC3_KBC_KBD_TRANSMIT:
    case SGI_IOC3_KBC_AUX_TRANSMIT:
        if (!s->ports_present) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc3-kbc: transmit on an unwired port\n");
            return;
        }
        if (addr == SGI_IOC3_KBC_KBD_TRANSMIT) {
            ps2_write_keyboard(&s->kbd, val & 0xff);
        } else {
            ps2_write_mouse(&s->mouse, val & 0xff);
        }
        return;

    default:
        /* The receive registers are read-only. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-kbc: write of 0x%" PRIx64 " to read-only "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
}

static const MemoryRegionOps sgi_ioc3_kbc_ops = {
    .read = sgi_ioc3_kbc_read,
    .write = sgi_ioc3_kbc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_ioc3_kbc_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(dev);

    if (!s->ports_present) {
        return;
    }
    if (!s->ports_initialized) {
        object_initialize_child(OBJECT(s), "kbd", &s->kbd, TYPE_PS2_KBD_DEVICE);
        object_initialize_child(OBJECT(s), "mouse", &s->mouse,
                                TYPE_PS2_MOUSE_DEVICE);
        s->ports_initialized = true;
    }
    if (!qdev_realize(DEVICE(&s->kbd), BUS(&s->kbc_bus), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->mouse), BUS(&s->kbc_bus), errp)) {
        return;
    }

    s->port[SGI_IOC3_KBC_PORT_KBD].timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_ioc3_kbc_kbd_tick, s);
    s->port[SGI_IOC3_KBC_PORT_AUX].timer =
        timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_ioc3_kbc_aux_tick, s);

    /* Each port's data-available line feeds the one shared interrupt. */
    qdev_connect_gpio_out(DEVICE(&s->kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-input", 0));
    qdev_connect_gpio_out(DEVICE(&s->mouse), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-input", 1));
}

static void sgi_ioc3_kbc_unrealize(DeviceState *dev)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(dev);

    if (s->ports_present) {
        unsigned int port;

        for (port = 0; port < SGI_IOC3_KBC_PORTS; port++) {
            timer_free(s->port[port].timer);
            s->port[port].timer = NULL;
        }
        qdev_unrealize(DEVICE(&s->kbd));
        qdev_unrealize(DEVICE(&s->mouse));
    }
}

static void sgi_ioc3_kbc_init(Object *obj)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    qbus_init(&s->kbc_bus, sizeof(s->kbc_bus), TYPE_SGI_IOC3_KBC_BUS,
             DEVICE(obj), "kbc-bus");

    memory_region_init_io(&s->mmio, obj, &sgi_ioc3_kbc_ops, s, "sgi-ioc3.kbc",
                          SGI_IOC3_KBC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_kbc_set_irq, "ps2-input", 2);
}

static void sgi_ioc3_kbc_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(obj);
    unsigned int port;

    s->control = 0;
    for (port = 0; port < SGI_IOC3_KBC_PORTS; port++) {
        SGIIOC3KBCPort *p = &s->port[port];

        memset(p->data, 0, sizeof(p->data));
        p->count = 0;
        p->overflow = false;
        p->stopped = false;
        p->ready_at = 0;
        if (p->timer) {
            timer_del(p->timer);
        }
    }
    qemu_set_irq(s->irq, 0);
}

/*
 * A holding longer than the register can report would index past the slots,
 * so a stream that claims one is refused
 * rather than trusted.
 */
static int sgi_ioc3_kbc_post_load(void *opaque, int version_id)
{
    SGIIOC3KBCState *s = opaque;
    unsigned int port;

    for (port = 0; port < SGI_IOC3_KBC_PORTS; port++) {
        if (s->port[port].count > SGI_IOC3_KBC_RX_SLOTS) {
            return -EINVAL;
        }
    }
    return 0;
}

static bool sgi_ioc3_kbc_timer_needed(void *opaque)
{
    SGIIOC3KBCPort *p = opaque;

    return p->timer && timer_pending(p->timer);
}

static const VMStateDescription vmstate_sgi_ioc3_kbc_port_timer = {
    .name = "sgi-ioc3-kbc/port/timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_ioc3_kbc_timer_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, SGIIOC3KBCPort),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_ioc3_kbc_port = {
    .name = "sgi-ioc3-kbc/port",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(data, SGIIOC3KBCPort, SGI_IOC3_KBC_RX_SLOTS),
        VMSTATE_UINT8(count, SGIIOC3KBCPort),
        VMSTATE_BOOL(overflow, SGIIOC3KBCPort),
        VMSTATE_BOOL_V(stopped, SGIIOC3KBCPort, 2),
        VMSTATE_INT64_V(ready_at, SGIIOC3KBCPort, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_ioc3_kbc_port_timer,
        NULL
    }
};

/*
 * The holdings and the pacing state carry everything that decides the next
 * arrival: the latched source bit travels in the chip's own state, the
 * characters still in the devices travel in theirs, and each port's timer
 * travels in a subsection while one is pending, so nothing is re-derived
 * after a load.
 */
static const VMStateDescription vmstate_sgi_ioc3_kbc = {
    .name = "sgi-ioc3-kbc",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sgi_ioc3_kbc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, SGIIOC3KBCState),
        VMSTATE_STRUCT_ARRAY(port, SGIIOC3KBCState, SGI_IOC3_KBC_PORTS, 1,
                             vmstate_sgi_ioc3_kbc_port, SGIIOC3KBCPort),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Host input only: point QEMU's unbound key events at this controller's
 * keyboard, or away from it. Nothing the
 * guest can see changes, and the routing lives in the input layer's handler
 * list rather than here, so there is
 * nothing to migrate or reset.
 */
static void sgi_ioc3_kbc_set_activate_keyboard(Object *obj, bool value,
                                               Error **errp)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(obj);
    QemuInputHandlerState *hs = s->ports_initialized
                                ? PS2_DEVICE(&s->kbd)->hs : NULL;
    g_autofree char *path = NULL;

    if (!hs) {
        error_setg(errp, "the keyboard has no input handler until realized");
        return;
    }

    if (value) {
        qemu_input_handler_activate(hs);
    } else {
        qemu_input_handler_deactivate(hs);
    }

    path = object_get_canonical_path(obj);
    trace_sgi_ioc3_kbc_activate_keyboard(path, value);
}

static void sgi_ioc3_kbc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 PS/2 keyboard and mouse controller";
    dc->realize = sgi_ioc3_kbc_realize;
    dc->unrealize = sgi_ioc3_kbc_unrealize;
    dc->vmsd = &vmstate_sgi_ioc3_kbc;
    /* Instantiated only by the IOC3, which wires its bus and interrupt. */
    dc->user_creatable = false;
    rc->phases.hold = sgi_ioc3_kbc_reset_hold;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    object_class_property_add_bool(oc, "activate-keyboard", NULL,
                                   sgi_ioc3_kbc_set_activate_keyboard);
    object_class_property_set_description(oc, "activate-keyboard",
        "Set true to send host key events to this controller's keyboard, "
        "false to move it behind every other keyboard");
}

static const TypeInfo sgi_ioc3_kbc_types[] = {
    {
        .name = TYPE_SGI_IOC3_KBC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3KBCState),
        .instance_init = sgi_ioc3_kbc_init,
        .class_init = sgi_ioc3_kbc_class_init,
    },
    {
        .name = TYPE_SGI_IOC3_KBC_BUS,
        .parent = TYPE_SYSTEM_BUS,
        .instance_size = sizeof(SGIIOC3KBCBusState),
    },
};

DEFINE_TYPES(sgi_ioc3_kbc_types)
