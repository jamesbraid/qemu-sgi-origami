/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only stimulus for a MIPS CPU warm exception entry. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "accel/tcg/cpu-ops.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
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
    char *create_variant;
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

/* Synthetic reports exercise capture only, never a production ECC detector. */
static void mips_cpu_exception_fixture_secondary_report(Object *obj,
                                                       const char *value,
                                                       Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);
    g_auto(GStrv) words = g_strsplit(value, " ", -1);
    uint64_t fields[4];
    R12000SecondaryReport report;

    if (!DEVICE(obj)->realized || !s->cpu || g_strv_length(words) != 4) {
        error_setg(errp, "expected realized fixture and "
                   "path error cacheop syndrome");
        return;
    }
    for (unsigned int i = 0; i < 4; i++) {
        if (qemu_strtou64(words[i], NULL, 0, &fields[i]) ||
            fields[i] > UINT32_MAX) {
            error_setg(errp, "invalid secondary report field");
            return;
        }
    }
    if (fields[2] > 1) {
        error_setg(errp, "cacheop must be zero or one");
        return;
    }
    report = (R12000SecondaryReport) {
        .path = fields[0], .error = fields[1], .cache_op = fields[2],
        .syndrome = fields[3],
    };
    mips_r12000a_report_secondary_error(MIPS_CPU(s->cpu), &report, errp);
}

static char *mips_cpu_exception_fixture_syndrome(Object *obj, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);
    MIPSCPU *cpu = s->cpu ? MIPS_CPU(s->cpu) : NULL;

    return g_strdup_printf("%u", cpu && cpu->r10k_profile ?
                            cpu->r10k_profile->r12000a_syndrome : 0);
}

static bool mips_cpu_exception_fixture_captured(Object *obj, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);
    MIPSCPU *cpu = s->cpu ? MIPS_CPU(s->cpu) : NULL;

    return cpu && cpu->r10k_profile && cpu->r10k_profile->r12000a_captured;
}

static void mips_cpu_exception_fixture_cold_reset_work(CPUState *cs,
                                                      run_on_cpu_data data)
{
    cpu_reset(cs);
    cs->halted = 0;
}

static void mips_cpu_exception_fixture_cold_reset(void *opaque, int n,
                                                  int level)
{
    MipsCpuExceptionFixtureState *s = opaque;

    if (level) {
        async_run_on_cpu(s->cpu, mips_cpu_exception_fixture_cold_reset_work,
                          RUN_ON_CPU_NULL);
    }
}

/* Test scheduling only: both CPUs execute the same RAM instructions. */
typedef struct FixtureContext {
    uint64_t pc;
    uint64_t mailbox;
} FixtureContext;

static void fixture_context_work(CPUState *cs, run_on_cpu_data data)
{
    const FixtureContext *context = data.host_ptr;

    cpu_set_pc(cs, context->pc);
    MIPS_CPU(cs)->env.active_tc.gpr[26] = context->mailbox;
}

static void fixture_context_set(Object *obj, const char *value, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);
    g_auto(GStrv) words = g_strsplit(value, " ", -1);
    FixtureContext context;

    if (!DEVICE(obj)->realized || g_strv_length(words) != 2 ||
        qemu_strtou64(words[0], NULL, 0, &context.pc) ||
        qemu_strtou64(words[1], NULL, 0, &context.mailbox) ||
        ((context.pc | context.mailbox) & 3)) {
        error_setg(errp, "expected realized fixture and aligned PC/mailbox");
        return;
    }
    run_on_cpu(s->cpu, fixture_context_work, RUN_ON_CPU_HOST_PTR(&context));
}

static void fixture_halted_work(CPUState *cs, run_on_cpu_data data)
{
    cs->halted = data.host_int;
}

static void fixture_halted_set(Object *obj, bool value, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);

    if (!DEVICE(obj)->realized) {
        error_setg(errp, "CPU scheduling requires realized test fixture");
        return;
    }
    run_on_cpu(s->cpu, fixture_halted_work, RUN_ON_CPU_HOST_INT(value));
}

static char *fixture_translation_key(Object *obj, Error **errp)
{
    MipsCpuExceptionFixtureState *s = MIPS_CPU_EXCEPTION_FIXTURE(obj);
    TCGTBCPUState key;

    if (!DEVICE(obj)->realized || !s->cpu) {
        error_setg(errp, "translation key requires a realized CPU fixture");
        return NULL;
    }
    key = CPU_GET_CLASS(s->cpu)->tcg_ops->get_tb_cpu_state(s->cpu);
    return g_strdup_printf("%" PRIx64 " %u %u", (uint64_t)key.pc, key.flags,
                            s->cpu->tcg_cflags);
}

static bool fixture_create_variant(MipsCpuExceptionFixtureState *s,
                                    Error **errp)
{
    static const char *const inputs[] = {
        "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
        "r12000-scache-bytes", "r12000-scache-block-words",
    };
    MIPSCPU *reference = MIPS_CPU(s->cpu);
    DeviceState *device;
    Error *error = NULL;

    if (strcmp(object_get_typename(OBJECT(reference)),
               MIPS_CPU_TYPE_NAME("R12000"))) {
        error_setg(errp, "mixed-variant fixture requires R12000 reference CPU");
        return false;
    }
    device = qdev_new(object_get_typename(OBJECT(reference)));
    object_property_add_child(OBJECT(s), "variant-cpu", OBJECT(device));
    object_unref(OBJECT(device));
    for (unsigned int i = 0; i < ARRAY_SIZE(inputs); i++) {
        uint64_t value = object_property_get_uint(OBJECT(reference), inputs[i],
                                                   &error);
        if (error) {
            goto fail;
        }
        object_property_set_uint(OBJECT(device), inputs[i], value, &error);
        if (error) {
            goto fail;
        }
    }
    object_property_set_str(OBJECT(device), "r12000-variant", s->create_variant,
                            &error);
    if (error) {
        goto fail;
    }
    object_property_set_bool(OBJECT(device), "big-endian",
                             reference->is_big_endian, &error);
    if (error) {
        goto fail;
    }
    qdev_connect_clock_in(device, "clk-in", reference->clock);
    if (!qdev_realize(device, NULL, &error)) {
        goto fail;
    }
    s->cpu = CPU(device);
    cpu_mips_irq_init_cpu(MIPS_CPU(device));
    cpu_mips_clock_init(MIPS_CPU(device));
    return true;
fail:
    error_propagate(errp, error);
    return false;
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
    if (s->create_variant && !fixture_create_variant(s, errp)) {
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
    DEFINE_PROP_STRING("create-r12000-variant", MipsCpuExceptionFixtureState,
                       create_variant),
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
                            mips_cpu_exception_fixture_cold_reset,
                            "cold-reset", 1);
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

    object_class_property_add_str(oc, "ram-context", NULL, fixture_context_set);
    object_class_property_add_bool(oc, "test-halted", NULL, fixture_halted_set);
    object_class_property_add_str(oc, "translation-key",
                                  fixture_translation_key, NULL);
    object_class_property_add_str(oc, "secondary-report", NULL,
                                  mips_cpu_exception_fixture_secondary_report);
    object_class_property_add_str(oc, "secondary-syndrome",
                                  mips_cpu_exception_fixture_syndrome, NULL);
    object_class_property_add_bool(oc, "syndrome-captured",
                                   mips_cpu_exception_fixture_captured, NULL);
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
