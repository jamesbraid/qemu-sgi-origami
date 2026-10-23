/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only stimulus for a MIPS CPU warm exception entry. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "system/qtest.h"
#include "system/tcg.h"
#include "cpu.h"

#define TYPE_MIPS_CPU_EXCEPTION_FIXTURE "mips-cpu-exception-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(MipsCpuExceptionFixtureState,
                           MIPS_CPU_EXCEPTION_FIXTURE)

struct MipsCpuExceptionFixtureState {
    DeviceState parent_obj;
    CPUState *cpu;
    uint32_t cpu_index;
};

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
    }
}

static const Property mips_cpu_exception_fixture_properties[] = {
    DEFINE_PROP_UINT32("cpu-index", MipsCpuExceptionFixtureState,
                       cpu_index, 0),
};

static void mips_cpu_exception_fixture_instance_init(Object *obj)
{
    qdev_init_gpio_in_named(DEVICE(obj),
                            mips_cpu_exception_fixture_soft_reset,
                            "soft-reset", 1);
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
