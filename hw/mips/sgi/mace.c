/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI MACE 2.0 platform registers. */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi/mace.h"
#include "hw/mips/sgi/mace-timer.h"
#include "hw/rtc/ds1685.h"
#include "hw/char/serial.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define MACE_APERTURE_SIZE 0x400000
#define MACE_RING_BASE     0x310000
#define MACE_UART_A        0x390000
#define MACE_UART_B        0x398000
#define MACE_ISTAT         0x310010
#define MACE_IMASK         0x310018
#define MACE_UST           0x340000
#define MACE_COMPARE0      0x340008
#define MACE_TIMER_NS      960
#define MACE_TIMER_BIT(i)  (1U << (13 + (i)))
#define MACE_RING_WRITABLE 0xffff8001U

typedef struct MACEUARTPort {
    SGIMACEState *mace;
    MemoryRegion mmio;
    unsigned index;
} MACEUARTPort;

struct SGIMACEState {
    SysBusDevice parent_obj;
    MemoryRegion aperture;
    MemoryRegion regs;
    DS1685State rtc;
    SerialState uart[2];
    MACEUARTPort uart_port[2];
    qemu_irq crime[16];
    qemu_irq isa_reset;
    QEMUTimer *timer;
    int64_t epoch_ns;
    int64_t saved_elapsed_ns;
    uint32_t ring_base;
    uint32_t istat;
    uint32_t imask;
    uint32_t compare[3];
    uint64_t compare_due[3];
    bool compare_programmed[3];
};

static uint64_t mace_elapsed_ticks(SGIMACEState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch_ns;

    return elapsed > 0 ? elapsed / MACE_TIMER_NS : 0;
}

static void mace_update_irqs(SGIMACEState *s)
{
    uint32_t active = s->istat & s->imask;

    qemu_set_irq(s->crime[4], (active & 0xffff0000U) != 0);
    qemu_set_irq(s->crime[5], (active & 0x0000ff00U) != 0);
    qemu_set_irq(s->crime[6], (active & 0x000000ffU) != 0);
}

static void mace_rtc_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;

    s->istat = (s->istat & ~(1U << 8)) | (level ? (1U << 8) : 0);
    mace_update_irqs(s);
}

static void mace_uart_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;
    uint32_t bit = 1U << (20 + 6 * n);

    s->istat = (s->istat & ~bit) | (level ? bit : 0);
    mace_update_irqs(s);
}

static void mace_schedule_compare(SGIMACEState *s)
{
    int64_t next = INT64_MAX;
    unsigned i;

    timer_del(s->timer);
    for (i = 0; i < ARRAY_SIZE(s->compare); i++) {
        int64_t deadline;

        if (!s->compare_programmed[i] || (s->istat & MACE_TIMER_BIT(i)) ||
            s->compare_due[i] == UINT64_MAX) {
            continue;
        }
        deadline = s->epoch_ns + s->compare_due[i] * MACE_TIMER_NS;
        next = MIN(next, deadline);
    }
    if (next != INT64_MAX) {
        timer_mod(s->timer, next);
    }
}

static void mace_compare_fire(void *opaque)
{
    SGIMACEState *s = opaque;
    uint64_t current = mace_elapsed_ticks(s);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(s->compare); i++) {
        if (s->compare_programmed[i] && !(s->istat & MACE_TIMER_BIT(i)) &&
            sgi_mace_compare_reached(s->compare_due[i], current)) {
            s->istat |= MACE_TIMER_BIT(i);
        }
    }
    mace_update_irqs(s);
    mace_schedule_compare(s);
}

static int mace_compare_index(hwaddr offset)
{
    if (offset >= MACE_COMPARE0 && offset <= MACE_COMPARE0 + 16 &&
        !(offset & 7)) {
        return (offset - MACE_COMPARE0) / 8;
    }
    if (offset >= 0x350000 && offset < 0x380000 && !(offset & 7)) {
        return (offset - 0x350000) / 0x10000;
    }
    return -1;
}

static uint64_t mace_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIMACEState *s = opaque;
    int index = mace_compare_index(offset);

    if (size == 8) {
        if (index >= 0) {
            if (!s->compare_programmed[index]) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: compare reset value is unspecified\n");
            }
            return s->compare[index];
        }
        switch (offset) {
        case MACE_RING_BASE:
            return s->ring_base;
        case MACE_ISTAT:
            return s->istat;
        case MACE_IMASK:
            return s->imask;
        case MACE_UST:
            return (uint32_t)mace_elapsed_ticks(s);
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace: read offset 0x%" HWADDR_PRIx
                  " size %u\n", offset, size);
    return 0;
}

static void mace_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    SGIMACEState *s = opaque;
    int index = mace_compare_index(offset);

    if (size == 8) {
        if (index >= 0) {
            s->compare[index] = value;
            s->compare_due[index] = sgi_mace_compare_due_tick(
                mace_elapsed_ticks(s), s->compare[index]);
            s->compare_programmed[index] = true;
            if (s->compare_due[index] == UINT64_MAX) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: compare equal to current UST is unsupported\n");
            }
            s->istat &= ~MACE_TIMER_BIT(index);
            mace_update_irqs(s);
            mace_schedule_compare(s);
            return;
        }
        switch (offset) {
        case MACE_RING_BASE: {
            bool assert_reset = !(s->ring_base & 1) && (value & 1);

            s->ring_base = value & MACE_RING_WRITABLE;
            if (assert_reset) {
                serial_reset_device(&s->uart[0]);
                serial_reset_device(&s->uart[1]);
            }
            serial_set_rx_discard(&s->uart[0], s->ring_base & 1);
            serial_set_rx_discard(&s->uart[1], s->ring_base & 1);
            qemu_set_irq(s->isa_reset, s->ring_base & 1);
            return;
        }
        case MACE_IMASK:
            s->imask = value;
            mace_update_irqs(s);
            return;
        case MACE_ISTAT:
            if (value & 0x0000e000U) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace: timer status clears through compare writes\n");
            }
            if (value & ~0x0000e000U) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: peripheral status write is unsupported\n");
            }
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace: write offset 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 "\n", offset, size, value);
}

static uint64_t mace_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    MACEUARTPort *port = opaque;
    SGIMACEState *s = port->mace;
    unsigned reg = offset >> 8;

    if (s->ring_base & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: UART read while external reset is asserted\n");
        return 0;
    }
    if (reg == 7 && (s->uart[port->index].lcr & 0x80)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: TI UART IR/clock register is unsupported\n");
        return 0;
    }
    return serial_io_ops.read(&s->uart[port->index], reg, 1);
}

static void mace_uart_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MACEUARTPort *port = opaque;
    SGIMACEState *s = port->mace;
    unsigned reg = offset >> 8;

    if (s->ring_base & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: UART write while external reset is asserted\n");
        return;
    }
    if (reg == 7 && (s->uart[port->index].lcr & 0x80)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: TI UART IR/clock register is unsupported\n");
        return;
    }
    serial_io_ops.write(&s->uart[port->index], reg, value, 1);
}

static const MemoryRegionOps mace_uart_ops = {
    .read = mace_uart_read,
    .write = mace_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static const MemoryRegionOps mace_ops = {
    .read = mace_read,
    .write = mace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void mace_reset_hold(Object *obj, ResetType type)
{
    SGIMACEState *s = SGI_MACE(obj);
    unsigned i;

    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->saved_elapsed_ns = 0;
    s->ring_base = 1;
    s->istat = 0;
    s->imask = 0;
    memset(s->compare, 0, sizeof(s->compare));
    for (i = 0; i < ARRAY_SIZE(s->compare_due); i++) {
        s->compare_due[i] = UINT64_MAX;
    }
    memset(s->compare_programmed, 0, sizeof(s->compare_programmed));
    timer_del(s->timer);
    for (i = 0; i < ARRAY_SIZE(s->crime); i++) {
        qemu_set_irq(s->crime[i], 0);
    }
    serial_set_rx_discard(&s->uart[0], true);
    serial_set_rx_discard(&s->uart[1], true);
    qemu_set_irq(s->isa_reset, 1);
}

static int mace_pre_save(void *opaque)
{
    SGIMACEState *s = opaque;

    s->saved_elapsed_ns = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                              s->epoch_ns, 0);
    return 0;
}

static int mace_post_load(void *opaque, int version_id)
{
    SGIMACEState *s = opaque;

    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                  s->saved_elapsed_ns;
    mace_update_irqs(s);
    serial_set_rx_discard(&s->uart[0], s->ring_base & 1);
    serial_set_rx_discard(&s->uart[1], s->ring_base & 1);
    qemu_set_irq(s->isa_reset, s->ring_base & 1);
    mace_schedule_compare(s);
    return 0;
}

static const VMStateDescription vmstate_mace = {
    .name = TYPE_SGI_MACE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mace_pre_save,
    .post_load = mace_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(saved_elapsed_ns, SGIMACEState),
        VMSTATE_UINT32(ring_base, SGIMACEState),
        VMSTATE_UINT32(istat, SGIMACEState),
        VMSTATE_UINT32(imask, SGIMACEState),
        VMSTATE_UINT32_ARRAY(compare, SGIMACEState, 3),
        VMSTATE_UINT64_ARRAY(compare_due, SGIMACEState, 3),
        VMSTATE_BOOL_ARRAY(compare_programmed, SGIMACEState, 3),
        VMSTATE_STRUCT_ARRAY(uart, SGIMACEState, 2, 0, vmstate_serial,
                             SerialState),
        VMSTATE_END_OF_LIST()
    },
};

static void mace_init(Object *obj)
{
    SGIMACEState *s = SGI_MACE(obj);

    memory_region_init(&s->aperture, obj, "sgi-mace", MACE_APERTURE_SIZE);
    memory_region_init_io(&s->regs, obj, &mace_ops, s, "sgi-mace-regs",
                          MACE_APERTURE_SIZE);
    memory_region_add_subregion(&s->aperture, 0, &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->aperture);
    qdev_init_gpio_out_named(DEVICE(obj), s->crime, "crime", 16);
    qdev_init_gpio_out_named(DEVICE(obj), &s->isa_reset, "isa-reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_rtc_irq, "rtc", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_uart_irq, "uart", 2);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_DS1685);
    object_initialize_child(obj, "uart-a", &s->uart[0], TYPE_SERIAL);
    object_initialize_child(obj, "uart-b", &s->uart[1], TYPE_SERIAL);
    object_property_add_alias(obj, "chardev-a", OBJECT(&s->uart[0]),
                              "chardev");
    object_property_add_alias(obj, "chardev-b", OBJECT(&s->uart[1]),
                              "chardev");
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mace_compare_fire, s);
}

static void mace_finalize(Object *obj)
{
    SGIMACEState *s = SGI_MACE(obj);

    timer_free(s->timer);
}

static void mace_realize(DeviceState *dev, Error **errp)
{
    SGIMACEState *s = SGI_MACE(dev);
    unsigned i;

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(&s->aperture, 0x3a0000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rtc), 2), 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       qdev_get_gpio_in_named(dev, "rtc", 0));

    for (i = 0; i < ARRAY_SIZE(s->uart); i++) {
        MACEUARTPort *port = &s->uart_port[i];

        qdev_prop_set_uint32(DEVICE(&s->uart[i]), "baudbase", 115200);
        serial_set_rx_discard(&s->uart[i], true);
        s->uart[i].irq = qdev_get_gpio_in_named(dev, "uart", i);
        if (!qdev_realize(DEVICE(&s->uart[i]), NULL, errp)) {
            return;
        }
        port->mace = s;
        port->index = i;
        memory_region_init_io(&port->mmio, OBJECT(dev), &mace_uart_ops,
                              port, i ? "sgi-mace-uart-b" : "sgi-mace-uart-a",
                              0x800);
        memory_region_add_subregion_overlap(&s->aperture,
                                            i ? MACE_UART_B : MACE_UART_A,
                                            &port->mmio, 1);
    }
}

static void mace_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI MACE 2.0 I/O controller";
    dc->realize = mace_realize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_mace;
    rc->phases.hold = mace_reset_hold;
}

static const TypeInfo mace_type_info = {
    .name = TYPE_SGI_MACE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMACEState),
    .instance_init = mace_init,
    .instance_finalize = mace_finalize,
    .class_init = mace_class_init,
};

static void mace_register_types(void)
{
    type_register_static(&mace_type_info);
}

type_init(mace_register_types)
