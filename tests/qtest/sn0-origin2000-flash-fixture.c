/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Seed physical flash independently of the CPU address-space selector. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "system/qtest.h"
#include "hw/mips/sgi/sn0.h"

#define TYPE_SN0_FLASH_TEST "sgi-sn0-node-flash-test"
OBJECT_DECLARE_SIMPLE_TYPE(SN0FlashTestState, SN0_FLASH_TEST)

struct SN0FlashTestState {
    DeviceState parent_obj;
    uint32_t node;
    uint32_t word;
};

static void sn0_flash_test_realize(DeviceState *dev, Error **errp)
{
    SN0FlashTestState *s = SN0_FLASH_TEST(dev);
    g_autofree char *node_name = g_strdup_printf("node%u", s->node);
    Object *node_obj;
    SN0NodeState *node;
    MemoryRegion *chip;
    const hwaddr offset = 0x64;

    if (!qtest_enabled()) {
        error_setg(errp, TYPE_SN0_FLASH_TEST " requires the qtest accelerator");
        return;
    }
    if (!object_dynamic_cast(OBJECT(current_machine),
                             MACHINE_TYPE_NAME("origin2000"))) {
        error_setg(errp, TYPE_SN0_FLASH_TEST " requires Origin 2000");
        return;
    }
    /*
     * The machine's node array is private to the builder (sn-machine.c);
     * every node it builds is a QOM child named "node<index>", so a test
     * fixture outside that file reaches one by that name instead of a second
     * accessor API that would exist for this one caller.
     */
    node_obj = object_resolve_path_component(OBJECT(current_machine),
                                             node_name);
    if (!node_obj) {
        error_setg(errp, TYPE_SN0_FLASH_TEST
                   " needs a populated physical node");
        return;
    }
    node = SN0_NODE(node_obj);
    if (!node->flash) {
        error_setg(errp, TYPE_SN0_FLASH_TEST
                   " needs a populated physical node");
        return;
    }
    chip = sysbus_mmio_get_region(SYS_BUS_DEVICE(node->flash), 0);
    if (memory_region_size(chip) < offset + sizeof(s->word)) {
        error_setg(errp, TYPE_SN0_FLASH_TEST " flash is too small");
        return;
    }

    /* No reset callback: reset must preserve the original fixture itself. */
    stl_be_p((uint8_t *)memory_region_get_ram_ptr(chip) + offset, s->word);
    memory_region_set_dirty(chip, offset, sizeof(s->word));
}

static const Property sn0_flash_test_properties[] = {
    DEFINE_PROP_UINT32("node", SN0FlashTestState, node, UINT32_MAX),
    DEFINE_PROP_UINT32("word", SN0FlashTestState, word, 0),
};

static void sn0_flash_test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sn0_flash_test_realize;
    dc->hotpluggable = false;
    device_class_set_props(dc, sn0_flash_test_properties);
}

static const TypeInfo sn0_flash_test_type = {
    .name = TYPE_SN0_FLASH_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SN0FlashTestState),
    .class_init = sn0_flash_test_class_init,
};

static void sn0_flash_test_register_types(void)
{
    type_register_static(&sn0_flash_test_type);
}

type_init(sn0_flash_test_register_types)
