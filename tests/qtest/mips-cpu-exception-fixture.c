/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only stimulus for a MIPS CPU warm exception entry. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "system/qtest.h"
#include "system/address-spaces.h"
#include "system/tcg.h"
#include "cpu.h"

#define TYPE_MIPS_CPU_EXCEPTION_FIXTURE "mips-cpu-exception-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(MipsCpuExceptionFixtureState,
                           MIPS_CPU_EXCEPTION_FIXTURE)

struct MipsCpuExceptionFixtureState {
    DeviceState parent_obj;
    CPUState *cpu;
    qemu_irq cache_error_complete;
    uint32_t cpu_index;
    MemoryRegion attribute_probe;
    uint64_t attribute_probe_base;
    MemoryRegion upper_probe;
    uint64_t upper_probe_base;
};

/* Observe the attributes of an executed CPU load, including cached lookups. */
static MemTxResult mips_cpu_attribute_probe_read(void *opaque, hwaddr addr,
                                                uint64_t *value,
                                                unsigned int size,
                                                MemTxAttrs attrs)
{
    *value = attrs.mips_cca;
    return MEMTX_OK;
}

static const MemoryRegionOps mips_cpu_attribute_probe_ops = {
    .read_with_attrs = mips_cpu_attribute_probe_read,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mips_cpu_exception_fixture_external_irq(void *opaque, int n,
                                                    int level)
{
    MipsCpuExceptionFixtureState *s = opaque;

    qemu_set_irq(MIPS_CPU(s->cpu)->env.irq[n + 2], level);
}

static void mips_cpu_exception_fixture_soft_reset_work(CPUState *cs,
                                                       run_on_cpu_data data)
{
    /* Match the Hub's warm entry on the selected CPU thread. */
    cs->exception_index = EXCP_SRESET;
    CPU_GET_CLASS(cs)->tcg_ops->do_interrupt(cs);
    cs->halted = 0;
}

static void mips_cpu_exception_fixture_soft_reset(void *opaque, int n,
                                                  int level)
{
    MipsCpuExceptionFixtureState *s = opaque;

    if (level) {
        async_run_on_cpu(s->cpu,
                         mips_cpu_exception_fixture_soft_reset_work,
                         RUN_ON_CPU_NULL);
    }
}

static void mips_cpu_exception_fixture_cache_error_work(CPUState *cs,
                                                        run_on_cpu_data data)
{
    MipsCpuExceptionFixtureState *s = data.host_ptr;

    cs->exception_index = EXCP_CACHE;
    CPU_GET_CLASS(cs)->tcg_ops->do_interrupt(cs);
    /* Let qtest distinguish a completed injection from an unchanged mailbox. */
    qemu_irq_pulse(s->cache_error_complete);
}

static void mips_cpu_exception_fixture_cache_error(void *opaque, int n,
                                                   int level)
{
    MipsCpuExceptionFixtureState *s = opaque;

    if (level) {
        async_run_on_cpu(s->cpu,
                         mips_cpu_exception_fixture_cache_error_work,
                         RUN_ON_CPU_HOST_PTR(s));
    }
}

static void mips_cpu_exception_fixture_realize(DeviceState *dev, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(dev);

    if (!qtest_driver() || !tcg_enabled()) {
        error_setg(errp, TYPE_MIPS_CPU_EXCEPTION_FIXTURE
                   " requires qtest with TCG");
        return;
    }
    s->cpu = qemu_get_cpu(s->cpu_index);
    if (!s->cpu) {
        error_setg(errp, TYPE_MIPS_CPU_EXCEPTION_FIXTURE
                   " cannot find CPU %u", s->cpu_index);
        return;
    }
    if (s->attribute_probe_base != UINT64_MAX) {
        memory_region_init_io(&s->attribute_probe, OBJECT(s),
                               &mips_cpu_attribute_probe_ops, s,
                               "mips-cpu-attribute-probe", 0x1000);
        memory_region_add_subregion(get_system_memory(),
                                     s->attribute_probe_base,
                                     &s->attribute_probe);
    }
    if (s->upper_probe_base != UINT64_MAX) {
        memory_region_init_io(&s->upper_probe, OBJECT(s),
                               &mips_cpu_attribute_probe_ops, s,
                               "mips-cpu-upper-address-probe", 0x1000);
        memory_region_add_subregion(get_system_memory(), s->upper_probe_base,
                                     &s->upper_probe);
    }
}

static const Property mips_cpu_exception_fixture_properties[] = {
    DEFINE_PROP_UINT32("cpu-index", MipsCpuExceptionFixtureState,
                       cpu_index, 0),
    DEFINE_PROP_UINT64("attribute-probe-base", MipsCpuExceptionFixtureState,
                       attribute_probe_base, UINT64_MAX),
    DEFINE_PROP_UINT64("upper-probe-base", MipsCpuExceptionFixtureState,
                       upper_probe_base, UINT64_MAX),
};

static void mips_cpu_exception_fixture_instance_init(Object *obj)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);

    qdev_init_gpio_in_named(DEVICE(obj),
                            mips_cpu_exception_fixture_external_irq,
                            "external-irq", 6);
    qdev_init_gpio_in_named(DEVICE(obj),
                            mips_cpu_exception_fixture_soft_reset,
                            "soft-reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj),
                            mips_cpu_exception_fixture_cache_error,
                            "cache-error", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->cache_error_complete,
                             "cache-error-complete", 1);
}

static void mips_cpu_exception_fixture_class_init(ObjectClass *oc,
                                                  const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mips_cpu_exception_fixture_realize;
    dc->hotpluggable = false;
    device_class_set_props(dc, mips_cpu_exception_fixture_properties);
}

static const TypeInfo mips_cpu_exception_fixture_type = {
    .name = TYPE_MIPS_CPU_EXCEPTION_FIXTURE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MipsCpuExceptionFixtureState),
    .instance_init = mips_cpu_exception_fixture_instance_init,
    .class_init = mips_cpu_exception_fixture_class_init,
};

static void mips_cpu_exception_fixture_register_types(void)
{
    type_register_static(&mips_cpu_exception_fixture_type);
}

type_init(mips_cpu_exception_fixture_register_types)
