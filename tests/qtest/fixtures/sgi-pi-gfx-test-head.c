/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A graphics head for register-contract qtests. Its "return" property
 * returns credits when a test says so. With "port" set it also answers
 * stores in a 4 KiB window at that physical address, placed over the
 * global map, and counts in "taken" those a processor interface marked as
 * counted; with "auto-return" it returns one credit for each of them at
 * once, as a head that consumes synchronously does. "port-width" 4 has
 * the memory core split a doubleword store into two callbacks, as it does
 * for any port that leaves its access width at the default. Real heads return
 * credits as their framers take words off their ports; they adopt the
 * interface on their own branches.
 *
 * With "pio" linked to a node's processor-facing I/O window, the port is
 * placed over that window instead, at "pio-offset", so a processor reaches
 * it on its own I/O path, through its node and the fabric, as it reaches a
 * board. A port load while "sleep-ms" is nonzero lets the big lock go for
 * that long first, as a board's load does while it waits out a drawing
 * slice.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/pi-gfx.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define TYPE_SGI_PI_GFX_TEST_HEAD "sgi-pi-gfx-test-head"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPIGfxTestHeadState, SGI_PI_GFX_TEST_HEAD)

#define TEST_HEAD_PORT_SIZE 0x1000

struct SGIPIGfxTestHeadState {
    DeviceState parent_obj;
    Object *pi;
    uint8_t widget;
    uint64_t port_base;
    bool auto_return;
    uint8_t port_width;
    uint32_t taken;
    MemoryRegion *pio;
    uint64_t pio_offset;
    uint32_t sleep_ms;
    MemoryRegion port;
};

static uint64_t test_head_port_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIPIGfxTestHeadState *s = opaque;
    uint32_t sleep_ms = qatomic_read(&s->sleep_ms);

    if (sleep_ms && bql_locked()) {
        bql_unlock();
        g_usleep(sleep_ms * 1000ULL);
        bql_lock();
    }
    return 0;
}

static void test_head_port_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    SGIPIGfxTestHeadState *s = opaque;

    if (!sgi_pi_gfx_store_counted()) {
        return;
    }
    s->taken++;
    if (s->auto_return && s->pi) {
        sgi_pi_gfx_return_credits(s->pi, s->widget, 1);
    }
}

static const MemoryRegionOps test_head_port_ops = {
    .read = test_head_port_read,
    .write = test_head_port_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static const MemoryRegionOps test_head_narrow_port_ops = {
    .read = test_head_port_read,
    .write = test_head_port_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void test_head_return(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    SGIPIGfxTestHeadState *s = SGI_PI_GFX_TEST_HEAD(obj);
    uint32_t credits;

    if (!visit_type_uint32(v, name, &credits, errp)) {
        return;
    }
    if (!s->pi) {
        error_setg(errp, "pi link is not set");
        return;
    }
    sgi_pi_gfx_return_credits(s->pi, s->widget, credits);
}

/* "owe" adds to, or with a negative count cancels, the node's debt. */
static void test_head_owe(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    SGIPIGfxTestHeadState *s = SGI_PI_GFX_TEST_HEAD(obj);
    int32_t credits;

    if (!visit_type_int32(v, name, &credits, errp)) {
        return;
    }
    if (!s->pi) {
        error_setg(errp, "pi link is not set");
        return;
    }
    sgi_pi_gfx_owe_credits(s->pi, s->widget, credits);
}

/* "repay" returns owed credits. */
static void test_head_repay(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    SGIPIGfxTestHeadState *s = SGI_PI_GFX_TEST_HEAD(obj);
    uint32_t credits;

    if (!visit_type_uint32(v, name, &credits, errp)) {
        return;
    }
    if (!s->pi) {
        error_setg(errp, "pi link is not set");
        return;
    }
    sgi_pi_gfx_repay_credits(s->pi, s->widget, credits);
}

static void test_head_init(Object *obj)
{
    SGIPIGfxTestHeadState *s = SGI_PI_GFX_TEST_HEAD(obj);

    object_property_add(obj, "return", "uint32", NULL, test_head_return,
                        NULL, NULL);
    object_property_add(obj, "owe", "int32", NULL, test_head_owe, NULL, NULL);
    object_property_add(obj, "repay", "uint32", NULL, test_head_repay,
                        NULL, NULL);
    object_property_add_uint32_ptr(obj, "taken", &s->taken,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "sleep-ms", &s->sleep_ms,
                                   OBJ_PROP_FLAG_READWRITE);
}

static void test_head_realize(DeviceState *dev, Error **errp)
{
    SGIPIGfxTestHeadState *s = SGI_PI_GFX_TEST_HEAD(dev);

    if (!s->port_base && !s->pio) {
        return;
    }
    if (s->port_width != 4 && s->port_width != 8) {
        error_setg(errp, "port-width is 4 or 8");
        return;
    }
    memory_region_init_io(&s->port, OBJECT(dev),
                          s->port_width == 4 ? &test_head_narrow_port_ops :
                                               &test_head_port_ops, s,
                          "sgi-pi-gfx-test-head.port", TEST_HEAD_PORT_SIZE);
    if (s->pio) {
        memory_region_add_subregion_overlap(s->pio, s->pio_offset, &s->port,
                                            10);
        return;
    }
    memory_region_add_subregion_overlap(get_system_memory(), s->port_base,
                                        &s->port, 10);
}

static const Property test_head_properties[] = {
    DEFINE_PROP_LINK("pi", SGIPIGfxTestHeadState, pi, TYPE_SGI_PI_GFX,
                     Object *),
    DEFINE_PROP_UINT8("widget", SGIPIGfxTestHeadState, widget, 0),
    DEFINE_PROP_UINT64("port", SGIPIGfxTestHeadState, port_base, 0),
    DEFINE_PROP_BOOL("auto-return", SGIPIGfxTestHeadState, auto_return,
                     false),
    DEFINE_PROP_UINT8("port-width", SGIPIGfxTestHeadState, port_width, 8),
    DEFINE_PROP_LINK("pio", SGIPIGfxTestHeadState, pio, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("pio-offset", SGIPIGfxTestHeadState, pio_offset, 0),
};

static void test_head_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI graphics head stand-in for credit qtests";
    dc->realize = test_head_realize;
    device_class_set_props(dc, test_head_properties);
}

static const TypeInfo test_head_type = {
    .name = TYPE_SGI_PI_GFX_TEST_HEAD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIPIGfxTestHeadState),
    .instance_init = test_head_init,
    .class_init = test_head_class_init,
};

static void test_head_register_types(void)
{
    type_register_static(&test_head_type);
}

type_init(test_head_register_types)
