/* SPDX-License-Identifier: BSD-3-Clause */
/* Real address-space mappings for the graphics completion-word qtests. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/display/sgi/pixel-dma.h"
#include "hw/misc/sgi-dma-word-test.h"
#include "hw/misc/sgi-odyssey.h"
#include "system/address-spaces.h"
#include "system/qtest.h"

#define TYPE_SGI_DMA_WORD_TEST "sgi-dma-word-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIWordTestState, SGI_DMA_WORD_TEST)

struct SGIWordTestState {
    DeviceState parent_obj;
    SysBusDevice *odyssey;
    bool native;
    MemoryRegion native_exposed;
    MemoryRegion control;
    MemoryRegion irq_sink;
    uint32_t irq_writes;
    uint32_t irq_addr;
    uint32_t irq_size;
    uint64_t irq_value;
    uint8_t irq_snapshot[0x2000];
    MemoryRegion root;
    MemoryRegion backing;
    MemoryRegion windows[SGI_DMA_WORD_TEST_WINDOWS];
    MemoryRegion readonly_half;
    MemoryRegion readonly_byte;
    MemoryRegion readonly_payload;
    MemoryRegion different_byte;
    MemoryRegion different_half;
    MemoryRegion hostile;
    MemoryRegion exposed;
    MemoryRegion backing_exposed;
    AddressSpace as;
    uint64_t target;
    uint64_t composed;
    bool compose_override;
    uint32_t value;
    uint32_t result;
    uint32_t mmio_reads;
    uint32_t mmio_writes;
};

static bool word_test_compose(void *opaque, uint64_t addr, hwaddr *out)
{
    SGIWordTestState *s = opaque;

    if (addr < SGI_DMA_WORD_TEST_DEVICE_BASE) {
        return false;
    }
    *out = s->compose_override ? s->composed :
           addr - SGI_DMA_WORD_TEST_DEVICE_BASE;
    return true;
}

static const SGIHeadOps word_test_head_ops = {
    .dma_address = word_test_compose,
};

static MemTxResult word_test_hostile_read(void *opaque, hwaddr addr,
                                         uint64_t *value, unsigned size,
                                         MemTxAttrs attrs)
{
    SGIWordTestState *s = opaque;

    s->mmio_reads++;
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult word_test_hostile_write(void *opaque, hwaddr addr,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    SGIWordTestState *s = opaque;

    s->mmio_writes++;
    return MEMTX_ERROR;
}

static const MemoryRegionOps word_test_hostile_ops = {
    .read_with_attrs = word_test_hostile_read,
    .write_with_attrs = word_test_hostile_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t word_test_irq_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void word_test_irq_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    SGIWordTestState *s = opaque;

    s->irq_writes++;
    s->irq_addr = addr;
    s->irq_size = size;
    s->irq_value = value;
    memcpy(s->irq_snapshot, memory_region_get_ram_ptr(&s->backing),
           sizeof(s->irq_snapshot));
}

static const MemoryRegionOps word_test_irq_ops = {
    .read = word_test_irq_read,
    .write = word_test_irq_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static uint64_t word_test_control_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    SGIWordTestState *s = opaque;

    if (addr >= SGI_DMA_WORD_TEST_IRQ_SNAPSHOT &&
        addr < SGI_DMA_WORD_TEST_IRQ_SNAPSHOT + sizeof(s->irq_snapshot)) {
        return ldl_be_p(s->irq_snapshot + addr -
                        SGI_DMA_WORD_TEST_IRQ_SNAPSHOT);
    }
    if (s->odyssey && addr >= SGI_DMA_WORD_TEST_PATTERN &&
        addr < SGI_DMA_WORD_TEST_PATTERN + 32 * sizeof(uint32_t)) {
        return SGI_ODYSSEY(s->odyssey)->pattern_buffer[
            (addr - SGI_DMA_WORD_TEST_PATTERN) / sizeof(uint32_t)];
    }
    switch (addr) {
    case SGI_DMA_WORD_TEST_IRQ_WRITES:
        return s->irq_writes;
    case SGI_DMA_WORD_TEST_IRQ_ADDR:
        return s->irq_addr;
    case SGI_DMA_WORD_TEST_IRQ_SIZE:
        return s->irq_size;
    case SGI_DMA_WORD_TEST_IRQ_VALUE:
        return s->irq_value;
    case SGI_DMA_WORD_TEST_RESULT:
        return s->result;
    case SGI_DMA_WORD_TEST_MMIO_READS:
        return s->mmio_reads;
    case SGI_DMA_WORD_TEST_MMIO_WRITES:
        return s->mmio_writes;
    default:
        return 0;
    }
}

static void word_test_control_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    SGIWordTestState *s = opaque;
    SGIDMABus bus = {
        .as = &s->as,
        .ops = &word_test_head_ops,
        .opaque = s,
    };

    switch (addr) {
    case SGI_DMA_WORD_TEST_ADDR_HIGH:
        s->target = (s->target & UINT32_MAX) | (value << 32);
        break;
    case SGI_DMA_WORD_TEST_ADDR_LOW:
        s->target = (s->target & ~UINT64_C(0xffffffff)) | value;
        break;
    case SGI_DMA_WORD_TEST_VALUE:
        s->value = value;
        break;
    case SGI_DMA_WORD_TEST_GO:
        if (value == 2 || value == 3) {
            MemTxResult result;
            hwaddr target;

            if (!word_test_compose(s, s->target, &target)) {
                s->result = MEMTX_ERROR;
                break;
            }
            if (value == 2) {
                address_space_stl_be(&s->as, target, s->value,
                                     MEMTXATTRS_UNSPECIFIED, &result);
            } else {
                address_space_stw_be(&s->as, target, s->value,
                                     MEMTXATTRS_UNSPECIFIED, &result);
            }
            s->result = result;
        } else if (value == 4) {
            s->result = sgi_odyssey_publish_be16(&bus, s->target, s->value);
        } else {
            s->result = sgi_odyssey_publish_be32(&bus, s->target, s->value);
        }
        break;
    case SGI_DMA_WORD_TEST_COMPOSE_HIGH:
        s->composed = (s->composed & UINT32_MAX) | (value << 32);
        break;
    case SGI_DMA_WORD_TEST_COMPOSE_LOW:
        s->composed = (s->composed & ~UINT64_C(0xffffffff)) | value;
        break;
    case SGI_DMA_WORD_TEST_COMPOSE_OVERRIDE:
        s->compose_override = value != 0;
        break;
    case SGI_DMA_WORD_TEST_BACKING_READONLY:
        memory_region_set_readonly(&s->backing, value != 0);
        break;
    }
}

static const MemoryRegionOps word_test_control_ops = {
    .read = word_test_control_read,
    .write = word_test_control_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void word_test_realize(DeviceState *dev, Error **errp)
{
    SGIWordTestState *s = SGI_DMA_WORD_TEST(dev);
    unsigned i;

    if (!qtest_enabled()) {
        error_setg(errp, "sgi-dma-word-test requires qtest");
        return;
    }
    if (!memory_region_init_ram_flags_nomigrate(&s->backing, OBJECT(s),
                                                "sgi-word-backing", 0x2000,
                                                0, errp)) {
        return;
    }
    memory_region_init(&s->root, OBJECT(s), "sgi-word-address-space", 0x20000);
    for (i = 0; i < ARRAY_SIZE(s->windows); i++) {
        g_autofree char *name = g_strdup_printf("sgi-word-window-%u", i);
        uint64_t length = i == SGI_DMA_WORD_TEST_SHORT ?
                          SGI_DMA_WORD_TEST_COUNTER + 2 :
                          i == SGI_DMA_WORD_TEST_HALF_SHORT ?
                          SGI_DMA_WORD_TEST_COUNTER + 1 : 0x1000;
        uint64_t offset = i == SGI_DMA_WORD_TEST_UNALIGNED_HOST ? 1 : 0;

        memory_region_init_alias(&s->windows[i], OBJECT(s), name,
                                 &s->backing, offset, length);
        if (i == SGI_DMA_WORD_TEST_READONLY) {
            memory_region_set_readonly(&s->windows[i], true);
        }
        memory_region_add_subregion(&s->root, i * SGI_DMA_WORD_TEST_STRIDE,
                                    &s->windows[i]);
    }
    /* The cache can extend through contiguous backing despite permissions. */
    memory_region_init_alias(&s->readonly_half, OBJECT(s), "sgi-word-ro-half",
                             &s->backing, SGI_DMA_WORD_TEST_COUNTER + 2, 2);
    memory_region_set_readonly(&s->readonly_half, true);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_SPLIT_READONLY * SGI_DMA_WORD_TEST_STRIDE +
        SGI_DMA_WORD_TEST_COUNTER + 2, &s->readonly_half, 1);
    memory_region_init_alias(&s->different_half, OBJECT(s),
                             "sgi-word-different-half", &s->backing, 0x1000, 2);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_SPLIT_BACKING * SGI_DMA_WORD_TEST_STRIDE +
        SGI_DMA_WORD_TEST_COUNTER + 2, &s->different_half, 1);
    memory_region_init_alias(&s->readonly_byte, OBJECT(s), "sgi-half-ro-byte",
                             &s->backing, SGI_DMA_WORD_TEST_COUNTER + 1, 1);
    memory_region_set_readonly(&s->readonly_byte, true);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_HALF_SPLIT_READONLY *
        SGI_DMA_WORD_TEST_STRIDE + SGI_DMA_WORD_TEST_COUNTER + 1,
        &s->readonly_byte, 1);
    memory_region_init_alias(&s->different_byte, OBJECT(s),
                             "sgi-half-different-byte", &s->backing, 0x1000, 1);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_HALF_SPLIT_BACKING *
        SGI_DMA_WORD_TEST_STRIDE + SGI_DMA_WORD_TEST_COUNTER + 1,
        &s->different_byte, 1);
    memory_region_init_alias(&s->readonly_payload, OBJECT(s),
                             "sgi-query-ro-payload", &s->backing, 0x928, 4);
    memory_region_set_readonly(&s->readonly_payload, true);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_PAYLOAD_READONLY *
        SGI_DMA_WORD_TEST_STRIDE + 0x928, &s->readonly_payload, 1);
    memory_region_init_io(&s->hostile, OBJECT(s), &word_test_hostile_ops, s,
                          "sgi-word-hostile", 4);
    memory_region_add_subregion_overlap(
        &s->root, SGI_DMA_WORD_TEST_MMIO * SGI_DMA_WORD_TEST_STRIDE +
        SGI_DMA_WORD_TEST_COUNTER, &s->hostile, 1);
    if (s->native) {
        s->odyssey = SYS_BUS_DEVICE(qdev_new("sgi-odyssey"));
        object_property_set_uint(OBJECT(s->odyssey), "module", 0, &error_abort);
        object_property_set_str(OBJECT(s->odyssey), "slot", "15", &error_abort);
        object_property_set_link(OBJECT(s->odyssey), "fabric",
                                 OBJECT(get_system_memory()), &error_abort);
        if (!sysbus_realize(s->odyssey, errp)) {
            object_unref(OBJECT(s->odyssey));
            s->odyssey = NULL;
            return;
        }
    }
    address_space_init(&s->as, &s->root, "sgi-word-dma");
    memory_region_init_alias(&s->exposed, OBJECT(s), "sgi-word-observation",
                             &s->root, 0, 0x20000);
    memory_region_init_alias(&s->backing_exposed, OBJECT(s),
                             "sgi-word-full-backing", &s->backing, 0, 0x2000);
    memory_region_add_subregion(get_system_memory(), SGI_DMA_WORD_TEST_BACKING,
                                &s->backing_exposed);
    memory_region_init_io(&s->control, OBJECT(s), &word_test_control_ops, s,
                          "sgi-word-control", 0x2400);
    memory_region_add_subregion(get_system_memory(), SGI_DMA_WORD_TEST_MEMORY,
                                &s->exposed);
    memory_region_add_subregion(get_system_memory(), SGI_DMA_WORD_TEST_CONTROL,
                                &s->control);
    memory_region_init_io(&s->irq_sink, OBJECT(s), &word_test_irq_ops, s,
                          "sgi-query-irq-sink", 0x100);
    memory_region_add_subregion(get_system_memory(), SGI_DMA_WORD_TEST_IRQ_SINK,
                                &s->irq_sink);
    if (s->odyssey) {
        MemoryRegion *regs = sysbus_mmio_get_region(s->odyssey, 0);

        memory_region_init_alias(&s->native_exposed, OBJECT(s),
                                 "sgi-query-native-mmio", regs, 0,
                                 memory_region_size(regs));
        memory_region_add_subregion(get_system_memory(),
                                    SGI_DMA_WORD_TEST_NATIVE,
                                    &s->native_exposed);
    }
}

static void word_test_unrealize(DeviceState *dev)
{
    SGIWordTestState *s = SGI_DMA_WORD_TEST(dev);

    memory_region_del_subregion(get_system_memory(), &s->control);
    memory_region_del_subregion(get_system_memory(), &s->irq_sink);
    memory_region_del_subregion(get_system_memory(), &s->exposed);
    memory_region_del_subregion(get_system_memory(), &s->backing_exposed);
    if (s->odyssey) {
        memory_region_del_subregion(get_system_memory(), &s->native_exposed);
        qdev_unrealize(DEVICE(s->odyssey));
        object_unref(OBJECT(s->odyssey));
    }
    address_space_destroy(&s->as);
}

static const Property word_test_properties[] = {
    DEFINE_PROP_BOOL("native", SGIWordTestState, native, false),
};

static void word_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, word_test_properties);
    dc->realize = word_test_realize;
    dc->unrealize = word_test_unrealize;
}

static const TypeInfo word_test_info = {
    .name = TYPE_SGI_DMA_WORD_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIWordTestState),
    .class_init = word_test_class_init,
};

static void word_test_register_types(void)
{
    type_register_static(&word_test_info);
}

type_init(word_test_register_types)
