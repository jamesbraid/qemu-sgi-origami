/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IP30 HEART standalone processor and memory controller profile.
 * Memory population is explicit; this does not identify an Octane DIMM layout.
 * The Crossbow widget accepts interrupt packets and mapped-RAM DMA.
 * Compare and trigger are not modeled.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/mips/sgi-xtalk-port.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"

#define HEART_REG_SIZE 0x60008
#define HEART_RAM_BASE 0x20000000ULL
#define HEART_UNIT (32 * MiB)
#define HEART_COUNT_MASK ((1ULL << 52) - 1)
#define HEART_LEVEL0_MASK 0x000000000000ffffULL
#define HEART_LEVEL1_MASK 0x00000000ffff0000ULL
#define HEART_LEVEL2_MASK 0x0003ffff00000000ULL
#define HEART_WIDGET_SIZE 0x1000
#define HEART_PACKET_SIZE (1ULL << 48)
#define HEART_RAM_PACKET_SIZE ((HEART_MAX_UNITS + 0x1ffULL) * HEART_UNIT)

#define HEART_ONEWIRE_DONE 2
#define HEART_ONEWIRE_LEVEL 1

static void heart_onewire_complete(void *opaque)
{
    SGIHeartState *s = opaque;

    s->onewire_busy = false;
    s->onewire_control |= HEART_ONEWIRE_DONE;
    if (s->onewire_level) {
        s->onewire_control |= HEART_ONEWIRE_LEVEL;
    }
}

static void heart_onewire_refresh(SGIHeartState *s)
{
    if (s->onewire_busy &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->onewire_deadline_ns) {
        timer_del(s->onewire_timer);
        heart_onewire_complete(s);
    }
}

static void heart_onewire_start(SGIHeartState *s, uint64_t command)
{
    uint32_t pulse = (command >> 10) & 0x3ff;
    uint32_t sample = (command >> 2) & 0xff;

    if (s->onewire_busy) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-heart: one-wire command replaced busy slot\n");
    }
    timer_del(s->onewire_timer);
    s->onewire_control = command & ~(HEART_ONEWIRE_DONE | HEART_ONEWIRE_LEVEL);
    s->onewire_level = sgi_onewire_slot(s->onewire_bus, pulse, sample);
    s->onewire_busy = true;
    /* Register units are modeled as microseconds; silicon timing is unknown. */
    s->onewire_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                             (int64_t)(pulse + sample) * 1000;
    timer_mod(s->onewire_timer, s->onewire_deadline_ns);
}

static void heart_update_irq(SGIHeartState *s)
{
    uint64_t enabled = s->pending & s->mask[0];

    qemu_set_irq(s->processor[0], !!(enabled & HEART_LEVEL0_MASK));
    qemu_set_irq(s->processor[1], !!(enabled & HEART_LEVEL1_MASK));
    qemu_set_irq(s->processor[2], !!(enabled & HEART_LEVEL2_MASK));
}

static MemTxResult heart_widget_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;

    if ((offset == 0x00 && size == 8) ||
        (offset == 0x04 && size == 4)) {
        *value = ((uint32_t)s->widget_revision << 28) |
                 (0xc001U << 12) | (0x0036U << 1);
        return MEMTX_OK;
    }
    if (offset == 0x24 && size == 4) {
        *value = s->widget_control;
        return MEMTX_OK;
    }
    if (offset == 0x60 && size == 8) {
        *value = 0;
        return MEMTX_OK;
    }
    if (offset == 0x68 && size == 8) {
        *value = s->widget_error_mask;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported widget %u-byte read "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult heart_widget_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;

    if (offset == 0x24 && size == 4) {
        s->widget_control = value;
        return MEMTX_OK;
    }
    if (offset == 0x44 && size == 4 && value == 0) {
        return MEMTX_OK;
    }
    if (offset == 0x60 && size == 8) {
        /* No widget error producer exists in this model. */
        return MEMTX_OK;
    }
    if (offset == 0x68 && size == 8) {
        s->widget_error_mask = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported widget %u-byte write "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps heart_widget_ops = {
    .read_with_attrs = heart_widget_read,
    .write_with_attrs = heart_widget_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

/*
 * Validate one QEMU memory operation before writing any byte. The packet
 * callback sees at most eight bytes; a larger PCI DMA may issue several
 * operations and can fail after an earlier one has completed.
 */
static bool heart_dma_mapped(SGIHeartState *s, hwaddr address, unsigned size)
{
    for (unsigned byte = 0; byte < size; byte++) {
        hwaddr at = address + byte;
        bool found = false;

        if (at < address) {
            return false;
        }
        for (int bank = HEART_BANKS - 1; bank >= 0; bank--) {
            uint32_t d = s->descriptor[bank];
            hwaddr base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
            hwaddr span = (uint64_t)((d >> 16 & 0x3f) + 1) * HEART_UNIT;

            if ((d & 0x80000000U) && at >= base && at - base < span) {
                found = s->capacity[bank] != 0;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static MemTxResult heart_packet_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;
    uint8_t bytes[8];

    if (offset >= HEART_RAM_BASE && heart_dma_mapped(s, offset, size)) {
        MemTxResult result = address_space_read(&address_space_memory, offset,
                                                attrs, bytes, size);

        if (result == MEMTX_OK) {
            *value = ldn_be_p(bytes, size);
        }
        return result;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported packet %u-byte read "
                  "at 0x%" HWADDR_PRIx "\n", size, offset);
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult heart_packet_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIHeartState *s = opaque;
    unsigned vector = value & 0xff;
    uint8_t bytes[8];

    if (offset >= HEART_RAM_BASE && heart_dma_mapped(s, offset, size)) {
        stn_be_p(bytes, size, value);
        return address_space_write(&address_space_memory, offset, attrs,
                                   bytes, size);
    }
    if (offset != 0x80 || size != 8 || (value & ~0x1ffULL) || vector >= 64) {
        qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported packet %u-byte "
                      "write at 0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                      size, offset, value);
        return MEMTX_ERROR;
    }
    if (value & 0x100) {
        s->pending |= 1ULL << vector;
    } else {
        s->pending &= ~(1ULL << vector);
    }
    heart_update_irq(s);
    return MEMTX_OK;
}

static const MemoryRegionOps heart_packet_ops = {
    .read_with_attrs = heart_packet_read,
    .write_with_attrs = heart_packet_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult heart_packet_ram_read(void *opaque, hwaddr offset,
                                         uint64_t *value, unsigned size,
                                         MemTxAttrs attrs)
{
    return heart_packet_read(opaque, offset + HEART_RAM_BASE,
                             value, size, attrs);
}

static MemTxResult heart_packet_ram_write(void *opaque, hwaddr offset,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    return heart_packet_write(opaque, offset + HEART_RAM_BASE,
                              value, size, attrs);
}

static const MemoryRegionOps heart_packet_ram_ops = {
    .read_with_attrs = heart_packet_ram_read,
    .write_with_attrs = heart_packet_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static uint64_t heart_counter(SGIHeartState *s)
{
    uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                       s->counter_epoch_ns;

    return (s->saved_count + elapsed / 80) & HEART_COUNT_MASK;
}

static uint64_t heart_absent_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void heart_absent_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
}

static const MemoryRegionOps heart_absent_ops = {
    .read = heart_absent_read,
    .write = heart_absent_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t heart_bank_offset(uint32_t descriptor, uint64_t capacity,
                                  uint64_t offset)
{
    /* Functional probe projection; physical HEART wiring is not established. */
    if ((descriptor & 0x803f0000U) == 0x803f0000U &&
        ((descriptor >> 22) & 7) == 6) {
        switch (capacity) {
        case 64 * MiB:
            return (offset & (32 * MiB - 1)) |
                   ((offset & (64 * MiB)) >> 1);
        case 128 * MiB:
            return (offset & (32 * MiB - 1)) |
                   ((offset & (64 * MiB)) >> 1) |
                   ((offset & (1 * GiB)) >> 4);
        case 256 * MiB:
            return (offset & (128 * MiB - 1)) |
                   ((offset & (256 * MiB)) >> 1);
        }
    }
    return offset % capacity;
}

static void heart_map_bank(SGIHeartState *s, unsigned bank)
{
    uint32_t d = s->descriptor[bank];
    MemoryRegion *memory = s->cpu_memory ?: get_system_memory();
    uint64_t base = HEART_RAM_BASE + (uint64_t)(d & 0x1ff) * HEART_UNIT;
    unsigned units = (d >> 16 & 0x3f) + 1;

    memory_region_transaction_begin();
    for (unsigned unit = 0; unit < HEART_MAX_UNITS; unit++) {
        if (s->mapped[bank][unit]) {
            memory_region_del_subregion(memory, &s->views[bank][unit]);
            s->mapped[bank][unit] = false;
        }
    }
    if (d & 0x80000000U) {
        for (unsigned unit = 0; unit < units; unit++) {
            if (s->capacity[bank]) {
                memory_region_set_alias_offset(&s->views[bank][unit],
                    heart_bank_offset(d, s->capacity[bank],
                                      (uint64_t)unit * HEART_UNIT));
            }
            memory_region_add_subregion_overlap(memory,
                base + (uint64_t)unit * HEART_UNIT,
                &s->views[bank][unit], bank + 1);
            s->mapped[bank][unit] = true;
        }
    }
    memory_region_transaction_commit();
}

static void heart_map_all(SGIHeartState *s)
{
    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        heart_map_bank(s, bank);
    }
}

static uint64_t heart_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIHeartState *s = opaque;

    if (offset >= 0x20 && offset < 0x40) {
        unsigned n = (offset - 0x20) / 4;

        if (size == 4) {
            return s->descriptor[n];
        }
        if (size == 8 && !(n & 1)) {
            return ((uint64_t)s->descriptor[n] << 32) |
                   s->descriptor[n + 1];
        }
    }
    if (offset == 0xb8 && size == 8) {
        heart_onewire_refresh(s);
        return s->onewire_control;
    }
    if (offset == 0xbc && size == 4) {
        heart_onewire_refresh(s);
        return s->onewire_control;
    }
    if (size == 8) {
        switch (offset) {
        case 0x00: return s->mode;
        case 0x08: return s->sdram_mode;
        case 0x10: return s->mem_refresh;
        case 0x18: return s->mem_req_arb;
        case 0x80: return 1ULL << 4;
        case 0x10000 ... 0x10018:
            if (!(offset & 7)) {
                return s->mask[(offset - 0x10000) / 8];
            }
            break;
        case 0x10030: return s->pending;
        case 0x10038: return s->pending & s->mask[0];
        case 0x20000: return heart_counter(s);
        case 0x50000: return 0;
        default:
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported %u-byte read at 0x%"
                  HWADDR_PRIx "\n", size, offset);
    return 0;
}

static void heart_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    SGIHeartState *s = opaque;

    if (offset >= 0x20 && offset < 0x40) {
        unsigned n = (offset - 0x20) / 4;

        if (size == 4) {
            s->descriptor[n] = value;
            if (n < HEART_BANKS) {
                heart_map_bank(s, n);
            }
            return;
        }
        if (size == 8 && !(n & 1)) {
            s->descriptor[n] = value >> 32;
            s->descriptor[n + 1] = value;
            for (unsigned i = n; i < n + 2 && i < HEART_BANKS; i++) {
                heart_map_bank(s, i);
            }
            return;
        }
    }
    if (offset == 0xb8 && size == 8) {
        heart_onewire_start(s, value);
        return;
    }
    if (offset == 0xbc && size == 4) {
        heart_onewire_start(s, value);
        return;
    }
    if (size == 8) {
        switch (offset) {
        case 0x00:
            s->mode = value;
            return;
        case 0x08:
            s->sdram_mode = value;
            return;
        case 0x10:
            s->mem_refresh = value;
            return;
        case 0x18:
            s->mem_req_arb = value;
            return;
        case 0x10000 ... 0x10018:
            if (!(offset & 7)) {
                s->mask[(offset - 0x10000) / 8] = value;
                heart_update_irq(s);
                return;
            }
            break;
        case 0x10020:
            s->pending |= value;
            heart_update_irq(s);
            return;
        case 0x10028:
            s->pending &= ~value;
            heart_update_irq(s);
            return;
        default:
            break;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-heart: unsupported %u-byte write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n", size, offset,
                  value);
}

static const MemoryRegionOps heart_ops = {
    .read = heart_read,
    .write = heart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 8 },
};

static void heart_reset_enter(Object *obj, ResetType type)
{
    SGIHeartState *s = SGI_HEART(obj);

    s->mode = 0;
    s->sdram_mode = 0;
    s->mem_refresh = 0;
    s->mem_req_arb = 0;
    memset(s->descriptor, 0, sizeof(s->descriptor));
    memset(s->mask, 0, sizeof(s->mask));
    s->pending = 0;
    s->saved_count = 0;
    s->widget_control = 0;
    s->widget_error_mask = 0;
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->onewire_timer);
    s->onewire_control = 0;
    s->onewire_level = false;
    s->onewire_busy = false;
    s->onewire_deadline_ns = 0;
}

static void heart_reset_hold(Object *obj, ResetType type)
{
    SGIHeartState *s = SGI_HEART(obj);

    heart_map_all(s);
    heart_update_irq(s);
}

static int heart_pre_save(void *opaque)
{
    SGIHeartState *s = opaque;

    s->saved_count = heart_counter(s);
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->onewire_remaining_ns = s->onewire_busy ?
        MAX(0, s->onewire_deadline_ns - s->counter_epoch_ns) : 0;
    return 0;
}

static int heart_post_load(void *opaque, int version_id)
{
    SGIHeartState *s = opaque;

    s->saved_count &= HEART_COUNT_MASK;
    s->counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->onewire_timer);
    if (s->onewire_busy) {
        s->onewire_deadline_ns = s->counter_epoch_ns +
                                 s->onewire_remaining_ns;
        timer_mod(s->onewire_timer, s->onewire_deadline_ns);
    }
    heart_map_all(s);
    heart_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_heart = {
    .name = TYPE_SGI_HEART,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = heart_pre_save,
    .post_load = heart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(mode, SGIHeartState),
        VMSTATE_UINT64(sdram_mode, SGIHeartState),
        VMSTATE_UINT64(mem_refresh, SGIHeartState),
        VMSTATE_UINT64(mem_req_arb, SGIHeartState),
        VMSTATE_UINT32_ARRAY(descriptor, SGIHeartState, 8),
        VMSTATE_UINT64_ARRAY(mask, SGIHeartState, 4),
        VMSTATE_UINT64(pending, SGIHeartState),
        VMSTATE_UINT64(saved_count, SGIHeartState),
        VMSTATE_UINT32(widget_control, SGIHeartState),
        VMSTATE_UINT32(widget_error_mask, SGIHeartState),
        VMSTATE_UINT64(onewire_control, SGIHeartState),
        VMSTATE_INT64(onewire_remaining_ns, SGIHeartState),
        VMSTATE_BOOL(onewire_level, SGIHeartState),
        VMSTATE_BOOL(onewire_busy, SGIHeartState),
        VMSTATE_END_OF_LIST()
    },
};

static void heart_realize(DeviceState *dev, Error **errp)
{
    SGIHeartState *s = SGI_HEART(dev);

    for (unsigned bank = 0; bank < HEART_BANKS; bank++) {
        uint64_t capacity = s->capacity[bank];
        g_autofree char *bank_name = g_strdup_printf("heart.bank%u", bank);

        if (capacity != 0 && capacity != 64 * MiB &&
            capacity != 128 * MiB && capacity != 256 * MiB) {
            error_setg(errp,
                       "HEART bank %u capacity must be 0, 64M, 128M or 256M",
                       bank);
            return;
        }
        if (capacity && !memory_region_init_ram(&s->backing[bank],
                OBJECT(dev), bank_name, capacity, errp)) {
            return;
        }
        for (unsigned unit = 0; unit < HEART_MAX_UNITS; unit++) {
            g_autofree char *name = g_strdup_printf("heart.bank%u.unit%u",
                                                     bank, unit);

            if (capacity) {
                memory_region_init_alias(&s->views[bank][unit], OBJECT(dev),
                    name, &s->backing[bank],
                    ((uint64_t)unit * HEART_UNIT) % capacity, HEART_UNIT);
            } else {
                memory_region_init_io(&s->views[bank][unit], OBJECT(dev),
                    &heart_absent_ops, s, name, HEART_UNIT);
            }
        }
    }
}

static const Property heart_properties[] = {
    DEFINE_PROP_LINK("cpu-memory", SGIHeartState, cpu_memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("bank0-size", SGIHeartState, capacity[0], 0),
    DEFINE_PROP_UINT64("bank1-size", SGIHeartState, capacity[1], 0),
    DEFINE_PROP_UINT64("bank2-size", SGIHeartState, capacity[2], 0),
    DEFINE_PROP_UINT64("bank3-size", SGIHeartState, capacity[3], 0),
    DEFINE_PROP_UINT8("widget-revision", SGIHeartState, widget_revision, 0),
};

static void heart_init(Object *obj)
{
    SGIHeartState *s = SGI_HEART(obj);

    memory_region_init_io(&s->registers, obj, &heart_ops, s,
                          TYPE_SGI_HEART, HEART_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    memory_region_init_io(&s->widget, obj, &heart_widget_ops, s,
                          "sgi-heart.widget", HEART_WIDGET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->widget);
    memory_region_init(&s->packet, obj, "sgi-heart.packet",
                       HEART_PACKET_SIZE);
    memory_region_init_io(&s->packet_control, obj, &heart_packet_ops, s,
                          "sgi-heart.packet-control", HEART_WIDGET_SIZE);
    memory_region_add_subregion(&s->packet, 0, &s->packet_control);
    memory_region_init_io(&s->packet_ram, obj, &heart_packet_ram_ops, s,
                          "sgi-heart.packet-ram", HEART_RAM_PACKET_SIZE);
    memory_region_add_subregion(&s->packet, HEART_RAM_BASE, &s->packet_ram);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->packet);
    qdev_init_gpio_out_named(DEVICE(obj), s->processor, "processor", 3);
    s->onewire_bus = onewire_create_bus(DEVICE(obj), "onewire");
    s->onewire_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   heart_onewire_complete, s);
}

static void heart_finalize(Object *obj)
{
    SGIHeartState *s = SGI_HEART(obj);

    timer_free(s->onewire_timer);
}

static const char *heart_xtalk_describe(Object *port, unsigned int index)
{
    return "HEART widget";
}

static MemoryRegion *heart_xtalk_cpu_face(Object *port, unsigned int index)
{
    return &SGI_HEART(port)->widget;
}

static MemoryRegion *heart_xtalk_packet_face(Object *port,
                                             unsigned int index)
{
    return &SGI_HEART(port)->packet;
}

static void heart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIXtalkPortClass *pc = SGI_XTALK_PORT_CLASS(klass);

    dc->desc = "SGI IP30 HEART standalone functional profile";
    dc->realize = heart_realize;
    dc->vmsd = &vmstate_heart;
    dc->user_creatable = true;
    device_class_set_props(dc, heart_properties);
    rc->phases.enter = heart_reset_enter;
    rc->phases.hold = heart_reset_hold;
    pc->describe = heart_xtalk_describe;
    pc->cpu_face = heart_xtalk_cpu_face;
    pc->packet_face = heart_xtalk_packet_face;
}

static const TypeInfo heart_info = {
    .name = TYPE_SGI_HEART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIHeartState),
    .instance_init = heart_init,
    .instance_finalize = heart_finalize,
    .class_init = heart_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XTALK_PORT },
        { },
    },
};

static void heart_register_types(void)
{
    type_register_static(&heart_info);
}

type_init(heart_register_types)
