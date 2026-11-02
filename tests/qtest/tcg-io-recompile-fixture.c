/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise host-PC metadata lifetime without executing generated code. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/tcg.h"
#include "exec/tb-flush.h"
#include "tcg/tcg.h"
#include "accel/tcg/internal-common.h"
#include "accel/tcg/tb-jmp-cache.h"

#define TYPE_TCG_IO_RECOMPILE_FIXTURE "tcg-io-recompile-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(TCGIOLookupFixture, TCG_IO_RECOMPILE_FIXTURE)

struct TCGIOLookupFixture {
    DeviceState parent_obj;
    uintptr_t old_pc;
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
    dc->hotpluggable = false;
}

static const TypeInfo fixture_type = {
    .name = TYPE_TCG_IO_RECOMPILE_FIXTURE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(TCGIOLookupFixture),
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(register_types)
