/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise host-PC metadata lifetime without executing generated code. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "exec/tb-flush.h"
#include "tcg/tcg.h"
#include "accel/tcg/internal-common.h"
#include "accel/tcg/tb-jmp-cache.h"
#include "accel/tcg/tb-context.h"

#define TYPE_TCG_IO_RECOMPILE_FIXTURE "tcg-io-recompile-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(TCGIOLookupFixture, TCG_IO_RECOMPILE_FIXTURE)

struct TCGIOLookupFixture {
    DeviceState parent_obj;
    uintptr_t old_pc;
    MemoryRegion mmio;
    uint64_t mmio_base;
    uint64_t reads;
    uint64_t acks;
    uint32_t irq_after;
    CPUState *cpu;
};

static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned int size)
{
    TCGIOLookupFixture *s = opaque;

    g_assert_cmphex(addr, ==, 0);
    s->reads++;
    if (s->reads == s->irq_after) {
        qemu_set_irq(MIPS_CPU(s->cpu)->env.irq[2], 1);
    }
    return s->reads;
}

static void mmio_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned int size)
{
    TCGIOLookupFixture *s = opaque;

    g_assert_cmphex(addr, ==, 4);
    s->acks++;
    qemu_set_irq(MIPS_CPU(s->cpu)->env.irq[2], 0);
}

static const MemoryRegionOps mmio_ops = {
    .read = mmio_read,
    .write = mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool stopped_test(Error **errp)
{
    if (!qtest_driver() || !tcg_enabled() || runstate_is_running()) {
        error_setg(errp, "metadata control requires stopped qtest with TCG");
        return false;
    }
    return true;
}

static void reset_io(Object *obj, bool value, Error **errp)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(obj);

    if (!stopped_test(errp)) {
        return;
    }
    if (value && s->cpu) {
        s->reads = s->acks = 0;
        qemu_set_irq(MIPS_CPU(s->cpu)->env.irq[2], 0);
    }
}

static void flush(Object *obj, bool value, Error **errp)
{
    if (stopped_test(errp) && value) {
        tb_flush__exclusive_or_serial();
    }
}

static void queue_flush(Object *obj, bool value, Error **errp)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(obj);

    if (!qtest_driver() || !tcg_enabled() ||
        !runstate_is_running() || !s->cpu) {
        error_setg(errp, "queued flush requires running qtest with TCG");
        return;
    }
    if (value) {
        queue_tb_flush(s->cpu);
    }
}

static char *tb_counters(Object *obj, Error **errp)
{
    return g_strdup_printf("%u %u",
        qatomic_read(&tb_ctx.tb_phys_invalidate_count),
        qatomic_read(&tb_ctx.tb_flush_count));
}

static bool metadata_present(Object *obj, Error **errp)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(obj);

    return stopped_test(errp) && s->cpu && s->cpu->tb_jmp_cache->io_tb;
}

static void realize(DeviceState *dev, Error **errp)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(dev);

    if (s->mmio_base == UINT64_MAX) {
        return;
    }
    if (!stopped_test(errp)) {
        return;
    }
    s->cpu = qemu_get_cpu(0);
    if (!s->cpu) {
        error_setg(errp, "MMIO fixture requires CPU zero");
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &mmio_ops, s,
                           "tcg-io-recompile-mmio", 0x1000);
    memory_region_add_subregion(get_system_memory(), s->mmio_base, &s->mmio);
}

static void instance_init(Object *obj)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(obj);

    object_property_add_uint64_ptr(obj, "reads", &s->reads, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "acks", &s->acks, OBJ_PROP_FLAG_READ);
}

static const Property properties[] = {
    DEFINE_PROP_UINT64("mmio-base", TCGIOLookupFixture,
                       mmio_base, UINT64_MAX),
    DEFINE_PROP_UINT32("irq-after", TCGIOLookupFixture, irq_after, 0),
};

static TranslationBlock *allocate_tb(size_t size)
{
    TranslationBlock *tb;

    qemu_thread_jit_write();
    tb = tcg_tb_alloc(tcg_ctx);
    g_assert_nonnull(tb);
    memset(tb, 0, sizeof(*tb));
    qemu_spin_init(&tb->jmp_lock);
    tb->page_addr[0] = tb->page_addr[1] = -1;
    tb->tc.ptr = tcg_splitwx_to_rx(tcg_ctx->code_gen_ptr);
    tb->tc.size = size;
    memset(tcg_ctx->code_gen_ptr, 0, size);
    tcg_ctx->code_gen_ptr += size;
    tcg_tb_insert(tb);
    qemu_thread_jit_execute();
    return tb;
}

static void check_lookup(CPUState *cpu, TranslationBlock *expected,
                         uintptr_t pc)
{
    g_assert_true(tcg_tb_lookup(pc) == expected);
    g_assert_true(cpu_io_recompile_tb_lookup(cpu, pc) == expected);
}

static void populate(CPUState *cpu, run_on_cpu_data data)
{
    TCGIOLookupFixture *s = data.host_ptr;
    TranslationBlock *a = allocate_tb(64);
    TranslationBlock *b = allocate_tb(64);
    uintptr_t apc = (uintptr_t)a->tc.ptr + 4;
    uintptr_t bpc = (uintptr_t)b->tc.ptr + 4;
    void *rollback;
    TranslationBlock *discard, *replacement;

    check_lookup(cpu, a, apc);
    check_lookup(cpu, a, apc);
    check_lookup(cpu, b, bpc);
    check_lookup(cpu, a, apc);
    check_lookup(cpu, b, bpc);
    /* In-flight restoration must still find an invalidated block. */
    tb_phys_invalidate(b, -1);
    g_assert_true(tb_cflags(b) & CF_INVALID);
    check_lookup(cpu, b, bpc);
    check_lookup(cpu, b, bpc + 4);
    check_lookup(cpu, NULL, (uintptr_t)b->tc.ptr + b->tc.size);
    check_lookup(cpu, NULL, 1);
    /* Preserve the original split-WX lookup behavior for either alias. */
    check_lookup(cpu, tcg_tb_lookup(apc - tcg_splitwx_diff),
                  apc - tcg_splitwx_diff);
    check_lookup(cpu, b, bpc);

    /* A duplicate translation is discarded before it can execute. */
    rollback = tcg_ctx->code_gen_ptr;
    discard = allocate_tb(64);
    tcg_tb_remove(discard);
    tcg_ctx->code_gen_ptr = rollback;
    replacement = allocate_tb(128);
    g_assert_true(replacement == discard);
    check_lookup(cpu, replacement, (uintptr_t)replacement->tc.ptr + 4);
    check_lookup(cpu, b, bpc);
    if (cpu->cpu_index == 0) {
        s->old_pc = bpc;
    }
}

static void reuse(CPUState *cpu, run_on_cpu_data data)
{
    TCGIOLookupFixture *s = data.host_ptr;
    TranslationBlock *tb = allocate_tb(4096);

    if (cpu->cpu_index == 0) {
        /* The old return address now belongs to a different-sized TB. */
        g_assert_cmpuint(s->old_pc, >=, (uintptr_t)tb->tc.ptr);
        g_assert_cmpuint(s->old_pc, <,
                          (uintptr_t)tb->tc.ptr + tb->tc.size);
        check_lookup(cpu, tb, s->old_pc);
    } else {
        check_lookup(cpu, tb, (uintptr_t)tb->tc.ptr + 4);
    }
}

static void exercise(Object *obj, bool value, Error **errp)
{
    TCGIOLookupFixture *s = TCG_IO_RECOMPILE_FIXTURE(obj);
    CPUState *cpu;

    if (!value || !qtest_driver() || !tcg_enabled() || runstate_is_running()) {
        error_setg(errp, "metadata exercise requires stopped qtest with TCG");
        return;
    }
    tb_flush__exclusive_or_serial();
    CPU_FOREACH(cpu) {
        g_assert_null(cpu->tb_jmp_cache->io_tb);
        run_on_cpu(cpu, populate, RUN_ON_CPU_HOST_PTR(s));
    }
    CPU_FOREACH(cpu) {
        g_assert_nonnull(cpu->tb_jmp_cache->io_tb);
    }
    tb_flush__exclusive_or_serial();
    CPU_FOREACH(cpu) {
        g_assert_null(cpu->tb_jmp_cache->io_tb);
        run_on_cpu(cpu, reuse, RUN_ON_CPU_HOST_PTR(s));
    }
    tb_flush__exclusive_or_serial();
    CPU_FOREACH(cpu) {
        g_assert_null(cpu->tb_jmp_cache->io_tb);
    }
}

static void class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    object_class_property_add_bool(oc, "exercise", NULL, exercise);
    object_class_property_add_bool(oc, "reset-io", NULL, reset_io);
    object_class_property_add_bool(oc, "flush", NULL, flush);
    object_class_property_add_bool(oc, "queue-flush", NULL, queue_flush);
    object_class_property_add_str(oc, "tb-counters", tb_counters, NULL);
    object_class_property_add_bool(oc, "metadata-present",
                                   metadata_present, NULL);
    dc->realize = realize;
    device_class_set_props(dc, properties);
    dc->hotpluggable = false;
}

static const TypeInfo fixture_type = {
    .name = TYPE_TCG_IO_RECOMPILE_FIXTURE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TCGIOLookupFixture),
    .class_init = class_init,
    .instance_init = instance_init,
};

static void register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(register_types)
