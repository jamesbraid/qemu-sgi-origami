/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/misc/sgi-l1.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define BEDROCK_REMOTE_REGISTERS_SIZE 0x8000000
#define BEDROCK_L1_UART_OFFSET 0x80
/* The remote form of the hub's register window: offsets 22:0 under bit 23. */
#define BEDROCK_IO_REGISTERS_SIZE 0x800000
#define BEDROCK_PI_MODIFY_OFFSET 0x90
#define BEDROCK_PI1_OFFSET 0x200000

bool sgi_bedrock_hub_registers_permitted(SGIBedrockState *s)
{
    return s->initiator_region < 64 &&
           (s->md.current_cell & (UINT64_C(1) << s->initiator_region));
}

/* True when the window refuses the access, with the reason logged. */
static bool bedrock_remote_refused(SGIBedrockState *s, hwaddr addr,
                                   unsigned size, const char *access)
{
    if (!s->l1_uart_as_initialized) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: remote register window %s "
                      "at 0x%" HWADDR_PRIx " with no L1 controller linked\n",
                      access, addr);
        return true;
    }
    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: remote register window "
                      "%s at 0x%" HWADDR_PRIx " denied for region %u\n",
                      access, addr, (unsigned)s->initiator_region);
        return true;
    }
    if (addr < BEDROCK_L1_UART_OFFSET ||
        addr + size > BEDROCK_L1_UART_OFFSET + SGI_L1_SIZE) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: remote register window %s "
                      "at 0x%" HWADDR_PRIx " is not decoded\n", access, addr);
        return true;
    }
    return false;
}

static MemTxResult bedrock_remote_read(void *opaque, hwaddr addr,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    uint8_t buffer[8];
    MemTxResult result;

    if (bedrock_remote_refused(s, addr, size, "read")) {
        return MEMTX_DECODE_ERROR;
    }
    result = address_space_read(&s->l1_uart_as,
                                addr - BEDROCK_L1_UART_OFFSET,
                                attrs, buffer, size);
    if (result != MEMTX_OK) {
        return result;
    }
    switch (size) {
    case 1:
        *value = buffer[0];
        break;
    case 2:
        *value = lduw_be_p(buffer);
        break;
    case 4:
        *value = ldl_be_p(buffer);
        break;
    case 8:
        *value = ldq_be_p(buffer);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult bedrock_remote_write(void *opaque, hwaddr addr,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    uint8_t buffer[8];

    if (bedrock_remote_refused(s, addr, size, "write")) {
        return MEMTX_DECODE_ERROR;
    }
    switch (size) {
    case 1:
        buffer[0] = value;
        break;
    case 2:
        stw_be_p(buffer, value);
        break;
    case 4:
        stl_be_p(buffer, value);
        break;
    case 8:
        stq_be_p(buffer, value);
        break;
    default:
        return MEMTX_DECODE_ERROR;
    }
    return address_space_write(&s->l1_uart_as,
                               addr - BEDROCK_L1_UART_OFFSET,
                               attrs, buffer, size);
}

static const MemoryRegionOps bedrock_remote_ops = {
    .read_with_attrs = bedrock_remote_read,
    .write_with_attrs = bedrock_remote_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static MemTxResult bedrock_hub_special_read(void *opaque, hwaddr addr,
                                            uint64_t *value, unsigned size,
                                            MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    MemoryRegionSection section;
    MemTxResult result;

    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: hub register read at "
                      "0x%" HWADDR_PRIx " denied for region %u\n", addr,
                      (unsigned)s->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    section = memory_region_find(&s->mmio, addr, size);
    if (!section.mr || section.mr == &s->mmio ||
        int128_lt(section.size, int128_make64(size))) {
        if (section.mr) {
            memory_region_unref(section.mr);
        }
        return MEMTX_DECODE_ERROR;
    }
    result = memory_region_dispatch_read(section.mr,
                                         section.offset_within_region, value,
                                         size_memop(size) | MO_BE, attrs);
    memory_region_unref(section.mr);
    return result;
}

static MemTxResult bedrock_hub_special_write(void *opaque, hwaddr addr,
                                             uint64_t value, unsigned size,
                                             MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;
    MemoryRegionSection section;
    MemTxResult result;

    if (!sgi_bedrock_hub_registers_permitted(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock: hub register write at "
                      "0x%" HWADDR_PRIx " denied for region %u\n", addr,
                      (unsigned)s->initiator_region);
        return MEMTX_DECODE_ERROR;
    }
    section = memory_region_find(&s->mmio, addr, size);
    if (!section.mr || section.mr == &s->mmio ||
        int128_lt(section.size, int128_make64(size))) {
        if (section.mr) {
            memory_region_unref(section.mr);
        }
        return MEMTX_DECODE_ERROR;
    }
    result = memory_region_dispatch_write(section.mr,
                                          section.offset_within_region, value,
                                          size_memop(size) | MO_BE, attrs);
    memory_region_unref(section.mr);
    return result;
}

static const MemoryRegionOps bedrock_hub_special_ops = {
    .read_with_attrs = bedrock_hub_special_read,
    .write_with_attrs = bedrock_hub_special_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static MemTxResult bedrock_io_registers_read(void *opaque, hwaddr addr,
                                             uint64_t *value, unsigned size,
                                             MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock: crosstalk read of hub register "
                  "0x%" HWADDR_PRIx " is not supported\n", addr);
    *value = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult bedrock_io_registers_write(void *opaque, hwaddr addr,
                                              uint64_t value, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBedrockState *s = opaque;

    switch (addr) {
    case BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[0], value, s->local_region);
        return MEMTX_OK;
    case BEDROCK_PI1_OFFSET + BEDROCK_PI_MODIFY_OFFSET:
        sgi_bedrock_pi_io_modify(&s->pi[1], value, s->local_region);
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock: crosstalk write of 0x%"
                      PRIx64 " to hub register 0x%" HWADDR_PRIx
                      " is not supported\n", value, addr);
        return MEMTX_DECODE_ERROR;
    }
}

static const MemoryRegionOps bedrock_io_registers_ops = {
    .read_with_attrs = bedrock_io_registers_read,
    .write_with_attrs = bedrock_io_registers_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void bedrock_forward_irq(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->hardware_irq[n], level);
}

static void bedrock_forward_nmi(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->nmi[n], level);
}

static void bedrock_forward_cpu_enable(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->cpu_enable[n], level);
}

static void bedrock_lb_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_lb_error(&s->pi[0], level);
    sgi_bedrock_pi_set_lb_error(&s->pi[1], level);
}

static void bedrock_ni_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_ni_error(&s->pi[0], level);
    sgi_bedrock_pi_set_ni_error(&s->pi[1], level);
}

static void bedrock_xb_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_xb_error(&s->pi[0], level);
    sgi_bedrock_pi_set_xb_error(&s->pi[1], level);
}

static void bedrock_local_reset(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    if (level) {
        device_cold_reset(DEVICE(&s->pi[0]));
        device_cold_reset(DEVICE(&s->pi[1]));
        device_cold_reset(DEVICE(&s->io));
        sgi_bedrock_md_soft_reset(&s->md);
        device_cold_reset(DEVICE(&s->lb));
        device_cold_reset(DEVICE(&s->ni));
        device_cold_reset(DEVICE(&s->xb));
    }
}

static void bedrock_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(dev);
    unsigned mask;

    if (s->processor_count > 4) {
        error_setg(errp, "Bedrock processor count exceeds four slots");
        return;
    }
    mask = s->processor_mask ? s->processor_mask :
        (1U << s->processor_count) - 1;
    if ((mask & ~0xfU) || ctpop32(mask) != s->processor_count) {
        error_setg(errp, "Bedrock processor mask must select exactly %u "
                   "of four slots", s->processor_count);
        return;
    }
    if (s->cpu_enable_initial_mask & ~mask) {
        error_setg(errp, "Bedrock initial CPU enable mask selects an "
                   "empty slot");
        return;
    }
    s->processor_mask = mask;
    if (!s->dma_mr) {
        error_setg(errp, TYPE_SGI_BEDROCK " 'dma-memory' link not set");
        return;
    }
    if (s->l1_uart_mr && s->initiator_region >= 64) {
        error_setg(errp, "initiator-region must be set below 64 when "
                   "l1-uart is linked");
        return;
    }
    if (s->revision >= 16) {
        error_setg(errp, "revision must fit four bits");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-bedrock-dma");
    s->dma_as_initialized = true;
    if (s->l1_uart_mr) {
        address_space_init(&s->l1_uart_as, s->l1_uart_mr,
                           "sgi-bedrock-l1-uart");
        s->l1_uart_as_initialized = true;
    }
    qdev_prop_set_uint8(DEVICE(&s->io), "revision", s->revision);
    qdev_prop_set_uint8(DEVICE(&s->io), "local-region", s->local_region);
    qdev_prop_set_uint16(DEVICE(&s->io), "inbound-widget-mask",
                         s->inbound_widget_mask);
    qdev_prop_set_uint8(DEVICE(&s->io), "inbound-device", s->inbound_device);
    qdev_prop_set_uint64(DEVICE(&s->io), "device-mask-reset",
                         s->device_mask_reset);
    sgi_bedrock_io_set_dma_as(&s->io, &s->dma_as);
    for (unsigned i = 0; i < ARRAY_SIZE(s->widget_mr); i++) {
        if (s->widget_mr[i]) {
            g_autofree char *name = g_strdup_printf("widget-%u", i);

            if (!object_property_set_link(OBJECT(&s->io), name,
                                          OBJECT(s->widget_mr[i]), errp)) {
                return;
            }
        }
    }
    if (!object_property_set_link(OBJECT(&s->io), "hub-registers",
                                  OBJECT(&s->io_registers), errp)) {
        return;
    }
    if (s->node_mr) {
        if (!object_property_set_link(OBJECT(&s->md), "node-memory",
                                      OBJECT(s->node_mr), errp)) {
            return;
        }
    }
    qdev_prop_set_uint8(DEVICE(&s->md), "installed-bank-mask",
                        s->installed_memory_bank_mask);
    for (unsigned bank = 0; bank < 8; bank++) {
        g_autofree char *property =
            g_strdup_printf("installed-bank%u-mib", bank);

        qdev_prop_set_uint16(DEVICE(&s->md), property,
                            s->installed_memory_bank_mib[bank]);
    }
    for (unsigned i = 0; i < 2; i++) {
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "interface", i);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "cpu-base", s->cpu_base);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "processor-count",
                            s->processor_count);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "processor-mask",
                            s->processor_mask);
        qdev_prop_set_uint8(DEVICE(&s->pi[i]), "cpu-enable-initial",
                           (s->cpu_enable_initial_mask >> (2 * i)) & 3);
        if (!qdev_realize(DEVICE(&s->pi[i]), sysbus_get_default(), errp)) {
            return;
        }
        memory_region_add_subregion(&s->mmio, i * 0x200000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pi[i]), 0));
        for (unsigned line = 0;
             line < SGI_BEDROCK_PI_PROCESSORS * SGI_BEDROCK_PI_HW_IRQS;
             line++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "hardware-irq",
                                        line,
                qdev_get_gpio_in_named(dev, "pi-hardware-irq",
                    i * SGI_BEDROCK_PI_PROCESSORS *
                    SGI_BEDROCK_PI_HW_IRQS + line));
        }
        for (unsigned cpu = 0; cpu < SGI_BEDROCK_PI_PROCESSORS; cpu++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "nmi", cpu,
                qdev_get_gpio_in_named(dev, "pi-nmi",
                    i * SGI_BEDROCK_PI_PROCESSORS + cpu));
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "cpu-enable", cpu,
                qdev_get_gpio_in_named(dev, "pi-cpu-enable",
                    i * SGI_BEDROCK_PI_PROCESSORS + cpu));
        }
    }
    qdev_prop_set_uint32(DEVICE(&s->lb), "revision", s->revision);
    /*
     * The local block's vector engine reads a hub register for a routed read;
     * the node's own register window is where a local route resolves.
     */
    if (!object_property_set_link(OBJECT(&s->lb), "hub-registers",
                                  OBJECT(&s->hub_window), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->io), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->lb), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->ni), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->md), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->xb), sysbus_get_default(), errp)) {
        return;
    }
    memory_region_add_subregion(&s->mmio, 0x400000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 0));
    memory_region_add_subregion(&s->mmio, 0x600000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->lb), 0));
    memory_region_add_subregion(&s->mmio, 0x680000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->ni), 0));
    memory_region_add_subregion(&s->mmio, 0x700000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->xb), 0));
    memory_region_add_subregion(&s->mmio, 0x780000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "error", 0,
        qdev_get_gpio_in_named(dev, "lb-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->xb), "error", 0,
        qdev_get_gpio_in_named(dev, "xb-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "soft-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "error", 0,
        qdev_get_gpio_in_named(dev, "ni-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "local-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
}

static const Property bedrock_properties[] = {
    DEFINE_PROP_UINT32("processor-count", SGIBedrockState,
                       processor_count, 1),
    DEFINE_PROP_UINT32("processor-mask", SGIBedrockState,
                       processor_mask, 0),
    DEFINE_PROP_UINT32("cpu-enable-initial-mask", SGIBedrockState,
                       cpu_enable_initial_mask, 0),
    /*
     * Index of this node's first processor, so each interface can name its
     * own pair and tell the reading processor A from B.
     */
    DEFINE_PROP_UINT32("cpu-base", SGIBedrockState, cpu_base, 0),
    DEFINE_PROP_UINT8("revision", SGIBedrockState, revision, 2),
    DEFINE_PROP_UINT8("local-region", SGIBedrockState, local_region, 0),
    DEFINE_PROP_UINT8("installed-bank-mask", SGIBedrockState,
                      installed_memory_bank_mask, UINT8_MAX),
    DEFINE_PROP_UINT16("installed-bank0-mib", SGIBedrockState,
                       installed_memory_bank_mib[0], 0),
    DEFINE_PROP_UINT16("installed-bank1-mib", SGIBedrockState,
                       installed_memory_bank_mib[1], 0),
    DEFINE_PROP_UINT16("installed-bank2-mib", SGIBedrockState,
                       installed_memory_bank_mib[2], 0),
    DEFINE_PROP_UINT16("installed-bank3-mib", SGIBedrockState,
                       installed_memory_bank_mib[3], 0),
    DEFINE_PROP_UINT16("installed-bank4-mib", SGIBedrockState,
                       installed_memory_bank_mib[4], 0),
    DEFINE_PROP_UINT16("installed-bank5-mib", SGIBedrockState,
                       installed_memory_bank_mib[5], 0),
    DEFINE_PROP_UINT16("installed-bank6-mib", SGIBedrockState,
                       installed_memory_bank_mib[6], 0),
    DEFINE_PROP_UINT16("installed-bank7-mib", SGIBedrockState,
                       installed_memory_bank_mib[7], 0),
    DEFINE_PROP_UINT16("inbound-widget-mask", SGIBedrockState,
                       inbound_widget_mask, (1U << 0xe) | (1U << 0xf)),
    DEFINE_PROP_UINT8("inbound-device", SGIBedrockState, inbound_device, 0),
    DEFINE_PROP_UINT64("device-mask-reset", SGIBedrockState,
                       device_mask_reset, UINT64_MAX),
    DEFINE_PROP_LINK("dma-memory", SGIBedrockState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("node-memory", SGIBedrockState, node_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("l1-uart", SGIBedrockState, l1_uart_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT8("initiator-region", SGIBedrockState,
                      initiator_region, UINT8_MAX),
#define BEDROCK_WIDGET_PROP(_n) \
    DEFINE_PROP_LINK("widget-" #_n, SGIBedrockState, widget_mr[_n], \
                     TYPE_MEMORY_REGION, MemoryRegion *)
    BEDROCK_WIDGET_PROP(0), BEDROCK_WIDGET_PROP(1),
    BEDROCK_WIDGET_PROP(2), BEDROCK_WIDGET_PROP(3),
    BEDROCK_WIDGET_PROP(4), BEDROCK_WIDGET_PROP(5),
    BEDROCK_WIDGET_PROP(6), BEDROCK_WIDGET_PROP(7),
    BEDROCK_WIDGET_PROP(8), BEDROCK_WIDGET_PROP(9),
    BEDROCK_WIDGET_PROP(10), BEDROCK_WIDGET_PROP(11),
    BEDROCK_WIDGET_PROP(12), BEDROCK_WIDGET_PROP(13),
    BEDROCK_WIDGET_PROP(14), BEDROCK_WIDGET_PROP(15),
#undef BEDROCK_WIDGET_PROP
};

static void bedrock_init(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    DeviceState *dev = DEVICE(obj);

    object_initialize_child(obj, "pi0", &s->pi[0], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "pi1", &s->pi[1], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "io", &s->io, TYPE_SGI_BEDROCK_IO);
    object_initialize_child(obj, "md", &s->md, TYPE_SGI_BEDROCK_MD);
    object_initialize_child(obj, "lb", &s->lb, TYPE_SGI_BEDROCK_LB);
    object_initialize_child(obj, "ni", &s->ni, TYPE_SGI_BEDROCK_NI);
    object_initialize_child(obj, "xb", &s->xb, TYPE_SGI_BEDROCK_XB);
    memory_region_init(&s->mmio, obj, TYPE_SGI_BEDROCK, SGI_BEDROCK_SIZE);
    /*
     * The vector engine reads a hub register by its window offset. This alias
     * is never mapped, so it presents the window at offset zero however the
     * machine places mmio itself.
     */
    memory_region_init_alias(&s->hub_window, obj,
                             TYPE_SGI_BEDROCK "-hub-window", &s->mmio, 0,
                             SGI_BEDROCK_SIZE);
    memory_region_init_io(&s->hub_special_registers, obj,
                          &bedrock_hub_special_ops, s,
                          TYPE_SGI_BEDROCK "-hub-special-registers",
                          SGI_BEDROCK_SIZE);
    /* The checked view forwards into this device's own register tree. */
    s->hub_special_registers.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->io), 2));
    memory_region_init_io(&s->remote_registers, obj, &bedrock_remote_ops, s,
                          TYPE_SGI_BEDROCK "-remote-registers",
                          BEDROCK_REMOTE_REGISTERS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->remote_registers);
    memory_region_init_io(&s->io_registers, obj, &bedrock_io_registers_ops,
                          s, TYPE_SGI_BEDROCK "-io-registers",
                          BEDROCK_IO_REGISTERS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->io_registers);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 2));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->md), 3));
    qdev_init_gpio_out_named(dev, s->hardware_irq, "hardware-irq",
                             ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_out_named(dev, s->nmi, "nmi", ARRAY_SIZE(s->nmi));
    qdev_init_gpio_out_named(dev, s->cpu_enable, "cpu-enable",
                             ARRAY_SIZE(s->cpu_enable));
    qdev_init_gpio_in_named(dev, bedrock_forward_irq, "pi-hardware-irq",
                            ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_in_named(dev, bedrock_forward_nmi, "pi-nmi",
                            ARRAY_SIZE(s->nmi));
    qdev_init_gpio_in_named(dev, bedrock_forward_cpu_enable, "pi-cpu-enable",
                            ARRAY_SIZE(s->cpu_enable));
    qdev_init_gpio_in_named(dev, bedrock_lb_error, "lb-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_ni_error, "ni-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_xb_error, "xb-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_local_reset, "local-reset", 1);
}

static void bedrock_finalize(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);

    if (s->dma_as_initialized) {
        address_space_destroy(&s->dma_as);
    }
    if (s->l1_uart_as_initialized) {
        address_space_destroy(&s->l1_uart_as);
    }
}

static void bedrock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI Bedrock node controller";
    dc->realize = bedrock_realize;
    dc->user_creatable = true;
    device_class_set_props(dc, bedrock_properties);
}

static const TypeInfo bedrock_info = {
    .name = TYPE_SGI_BEDROCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockState),
    .instance_init = bedrock_init,
    .instance_finalize = bedrock_finalize,
    .class_init = bedrock_class_init,
};

static void bedrock_register_types(void)
{
    type_register_static(&bedrock_info);
}

type_init(bedrock_register_types)
