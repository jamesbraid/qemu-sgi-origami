/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-md.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

enum {
    MD_ARB, MD_MIGRATION, MD_PERF_SELECT0, MD_PERF_SELECT1,
    MD_PERF_COUNTER0, MD_PERF_COUNTER1, MD_JUNK_TIMING,
    MD_SELFTEST_DATA, MD_ISSUE_DEBUG, MD_MIGRATION_DIFFERENCE,
    MD_MIGRATION_VALUE, MD_ECC_CONFIG, MD_DIRECTORY_DEBUG, MD_MEMORY_DEBUG,
};

#define MD_REPLY_QUEUE UINT64_C(0x001818180c0c0c00)
#define MD_REQUEST_QUEUE UINT64_C(0x0000000004040400)
#define MD_COUNTER_MASK ((UINT64_C(1) << 41) - 1)
/*
 * Bits 58:56 are write-only in the public definition; reading them back
 * and resetting them to zero is model policy.
 */
#define MD_MEMORY_CONFIG_MASK (UINT64_C(0xfbfbfbfb) | \
    (UINT64_C(3) << 32) | (UINT64_C(0xf) << 44) | \
    (UINT64_C(0x3f) << 48) | (UINT64_C(7) << 56) | \
    (UINT64_C(3) << 60))
#define MD_PERF_SELECT_MASK (UINT64_C(3) | (UINT64_C(0xf) << 4) | \
    (UINT64_C(0x7f) << 8) | (UINT64_C(0xf) << 16) | \
    (UINT64_C(0xffffffffff) << 20) | (UINT64_C(1) << 63))
#define MD_THRESHOLD_MASK (UINT64_C(0x7fff) | (UINT64_C(7) << 32) | \
    (UINT64_C(7) << 35))

static bool md_storage(hwaddr offset, unsigned *index, uint64_t *mask)
{
    switch (offset) {
    case 0x10:
        *index = MD_ARB;
        *mask = 0xff;
        return true;
    case 0x18:
        *index = MD_MIGRATION;
        *mask = 0x3ff | (UINT64_C(0xff) << 16) | (UINT64_C(1) << 32);
        return true;
    case 0x10000:
        *index = MD_PERF_SELECT0;
        *mask = MD_PERF_SELECT_MASK;
        return true;
    case 0x10008:
        *index = MD_PERF_SELECT1;
        *mask = MD_PERF_SELECT_MASK;
        return true;
    case 0x10010:
        *index = MD_PERF_COUNTER0;
        *mask = MD_COUNTER_MASK;
        return true;
    case 0x10018:
        *index = MD_PERF_COUNTER1;
        *mask = MD_COUNTER_MASK;
        return true;
    case 0x20008:
        *index = MD_JUNK_TIMING;
        *mask = UINT64_C(0xffffffffffff);
        return true;
    case 0x20038:
        *index = MD_SELFTEST_DATA;
        *mask = 0xff;
        return true;
    case 0x20060:
        *index = MD_ISSUE_DEBUG;
        *mask = 3;
        return true;
    case 0x40040:
        *index = MD_MIGRATION_DIFFERENCE;
        *mask = MD_THRESHOLD_MASK | (UINT64_C(1) << 63);
        return true;
    case 0x40048:
        *index = MD_MIGRATION_VALUE;
        *mask = MD_THRESHOLD_MASK | (UINT64_C(3) << 62);
        return true;
    case 0x60000:
        *index = MD_ECC_CONFIG;
        *mask = 1;
        return true;
    case 0x40060:
        *index = MD_DIRECTORY_DEBUG;
        *mask = 3;
        return true;
    case 0x60040:
        *index = MD_MEMORY_DEBUG;
        *mask = 1;
        return true;
    default:
        return false;
    }
}

static bool md_idle(hwaddr offset)
{
    switch (offset) {
    case 0x20: case 0x28: case 0x40: case 0x48: case 0x58:
    case 0x20010: case 0x20018: case 0x20020: case 0x20028:
    case 0x20030: case 0x20040: case 0x20048:
    case 0x40010: case 0x40018: case 0x40020: case 0x40028:
    case 0x40030: case 0x40038: case 0x40058:
    case 0x60010: case 0x60018: case 0x60020: case 0x60028:
    case 0x60030: case 0x60038:
        return true;
    default:
        return false;
    }
}

static uint64_t md_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;
    uint64_t mask;

    switch (offset) {
    case 0: return s->current_cell;
    case 0x8: return s->memory_config;
    case 0x60: return MD_REPLY_QUEUE;
    case 0x20000: return s->refresh;
    case 0x40000: return s->directory_config;
    case 0x40050: return MD_REQUEST_QUEUE;
    }
    if (md_storage(offset, &index, &mask)) {
        return s->storage[index];
    }
    if (md_idle(offset)) {
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unimplemented read at 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void md_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockMDState *s = opaque;
    unsigned index;
    uint64_t mask;

    switch (offset) {
    case 0:
        s->current_cell = value;
        return;
    case 0x8:
        s->memory_config = value & MD_MEMORY_CONFIG_MASK;
        return;
    case 0x20000:
        s->refresh = value & ((UINT64_C(1) << 63) | 0xfff); return;
    case 0x40000:
        s->directory_config = value & 3;
        return;
    case 0x60:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: reply credit reset unsupported\n");
        return;
    case 0x40050:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: request queue resize unsupported\n");
        return;
    case 0x20030:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-md: self-test command unsupported\n");
        return;
    }
    if (md_storage(offset, &index, &mask)) {
        s->storage[index] = value & mask;
        return;
    }
    if (md_idle(offset)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unsupported write at 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-bedrock-md: unimplemented write at 0x%"
                  HWADDR_PRIx " value 0x%" PRIx64 "\n", offset, value);
}

/* Every register is 64 bits; refusing narrower accesses is model policy. */
static const MemoryRegionOps md_ops = {
    .read = md_read, .write = md_write, .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void md_cold_reset(SGIBedrockMDState *s)
{
    s->current_cell = UINT64_MAX;
    s->memory_config = 0;
    s->refresh = 0;
    s->directory_config = 0;
    memset(s->storage, 0, sizeof(s->storage));
}

void sgi_bedrock_md_soft_reset(SGIBedrockMDState *s)
{
    uint64_t memory_config = s->memory_config;
    uint64_t refresh = s->refresh & ((UINT64_C(1) << 63) | 0xfff);
    uint64_t directory_config = s->directory_config;

    md_cold_reset(s);
    s->memory_config = memory_config;
    s->refresh = refresh;
    s->directory_config = directory_config;
}

static void md_soft_reset_input(void *opaque, int n, int level)
{
    if (level) {
        sgi_bedrock_md_soft_reset(opaque);
    }
}

static void md_reset_enter(Object *obj, ResetType type)
{
    md_cold_reset(SGI_BEDROCK_MD(obj));
}

static const VMStateDescription vmstate_md = {
    .name = TYPE_SGI_BEDROCK_MD, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(current_cell, SGIBedrockMDState),
        VMSTATE_UINT64(memory_config, SGIBedrockMDState),
        VMSTATE_UINT64(refresh, SGIBedrockMDState),
        VMSTATE_UINT64(directory_config, SGIBedrockMDState),
        VMSTATE_UINT64_ARRAY(storage, SGIBedrockMDState,
                             SGI_BEDROCK_MD_STORAGE),
        VMSTATE_END_OF_LIST()
    },
};

static void md_init(Object *obj)
{
    SGIBedrockMDState *s = SGI_BEDROCK_MD(obj);

    memory_region_init_io(&s->mmio, obj, &md_ops, s, TYPE_SGI_BEDROCK_MD,
                          SGI_BEDROCK_MD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_in_named(DEVICE(obj), md_soft_reset_input, "soft-reset", 1);
}

static void md_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock memory and directory interface";
    dc->vmsd = &vmstate_md;
    dc->user_creatable = true;
    rc->phases.enter = md_reset_enter;
}

static const TypeInfo md_info = {
    .name = TYPE_SGI_BEDROCK_MD, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockMDState), .instance_init = md_init,
    .class_init = md_class_init,
};

static void md_register_types(void)
{
    type_register_static(&md_info);
}
type_init(md_register_types)
