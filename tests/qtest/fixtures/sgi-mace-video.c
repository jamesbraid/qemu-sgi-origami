/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Timed, decoded D1 signals; this fixture identifies no fitted video module. */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/mace-video.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/qtest.h"

#define TYPE_MACE_VIDEO_TEST "sgi-mace-video-test"
OBJECT_DECLARE_SIMPLE_TYPE(MACEVideoTest, MACE_VIDEO_TEST)

struct MACEVideoTest {
    SysBusDevice parent_obj;
    MACEVideo video;
    MACEVideoPort port[3];
    MemoryRegion timers;
    MemoryRegion fixture;
    MemoryRegion dma;
    MemoryRegion native;
    MemoryRegion no_ecc;
    MemoryRegion *ram;
    uint64_t mapped_bytes;
    uint32_t synthetic_revision;
    int64_t epoch;
    AddressSpace as;
    qemu_irq irq[3];
    MACEVideoEvent event;
    uint32_t seed;
    uint32_t output_count;
    uint64_t output_time[8192];
    uint64_t output_position[8192];
    uint32_t output_sample[8192];
    uint16_t output_flags[8192];
};

static uint32_t fixture_ust(void *opaque, int64_t ns)
{
    MACEVideoTest *s = opaque;

    return MAX(ns - s->epoch, 0) / 960;
}

static void fixture_irq(void *opaque, unsigned levels)
{
    MACEVideoTest *s = opaque;

    for (unsigned i = 0; i < 3; i++) {
        qemu_set_irq(s->irq[i], !!(levels & (1U << i)));
    }
}

static void fixture_emit(void *opaque, const MACEVideoEvent *e)
{
    MACEVideoTest *s = opaque;
    unsigned i;

    if (e->type != MACE_VIDEO_SAMPLE) {
        return;
    }
    i = s->output_count++;
    g_assert(i < ARRAY_SIZE(s->output_time));
    s->output_time[i] = e->time;
    s->output_position[i] = (uint64_t)e->x << 32 | e->y;
    s->output_sample[i] = e->sample;
    s->output_flags[i] = e->port | e->field << 8;
}

static uint64_t fixture_read(void *opaque, hwaddr addr, unsigned size)
{
    MACEVideoTest *s = opaque;

    mace_video_sync(&s->video);
    if (addr >= 0x1000 && addr < 0x41000) {
        unsigned i = (addr - 0x1000) / 32;

        switch (addr & 31) {
        case 0:
            return s->output_time[i];
        case 8:
            return s->output_position[i];
        case 16:
            return s->output_sample[i];
        case 24:
            return s->output_flags[i];
        }
    }
    if (addr == 0x58) {
        return s->output_count;
    }
    if (addr == 0x30) {
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    return 0;
}

static uint32_t fixture_pattern(unsigned seed, unsigned x, unsigned y)
{
    unsigned p = x & ~1U;

    return ((0x40 + ((seed + p + y) % 31)) << 24) |
           ((0x20 + ((seed * 3 + p * 5 + y * 7) % 173)) << 16) |
           ((0xa0 + ((seed + p * 3 + y * 2) % 47)) << 8) |
           (0x30 + ((seed * 7 + p * 11 + y * 13) % 163));
}

static void fixture_field(MACEVideoTest *s)
{
    MACEVideoEvent e = s->event;
    unsigned pixels = e.x, lines = e.y;
    int64_t origin = e.time;

    g_assert(pixels && pixels <= 4096 && lines && lines <= 2048);
    e.type = MACE_VIDEO_LOCK;
    g_assert(mace_video_enqueue(&s->video, &e));
    e.type = MACE_VIDEO_SOF;
    e.period_num = 100;
    e.period_den = 1;
    g_assert(mace_video_enqueue(&s->video, &e));
    for (unsigned y = 0; y < lines; y++) {
        e.type = MACE_VIDEO_LINE;
        e.time = origin + (int64_t)y * pixels * 100;
        e.x = pixels;
        e.y = y;
        g_assert(mace_video_enqueue(&s->video, &e));
        for (unsigned x = 0; x < pixels; x++) {
            e.type = MACE_VIDEO_SAMPLE;
            e.time = origin + ((int64_t)y * pixels + x + 1) * 100;
            e.x = x;
            e.sample = fixture_pattern(s->seed, x, y);
            g_assert(mace_video_enqueue(&s->video, &e));
        }
    }
    e.type = MACE_VIDEO_EOF;
    e.time = origin + (int64_t)lines * pixels * 100;
    e.x = pixels;
    e.y = lines;
    g_assert(mace_video_enqueue(&s->video, &e));
}

static void fixture_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    MACEVideoTest *s = opaque;
    MACEVideoEvent *e = &s->event;

    switch (addr) {
    case 0:
        e->time = value;
        break;
    case 8:
        e->type = value & 15;
        e->port = (value >> 4) & 7;
        e->field = (value >> 8) & 1;
        break;
    case 0x10:
        e->x = value;
        break;
    case 0x18:
        e->y = value;
        break;
    case 0x20:
        e->sample = value;
        break;
    case 0x28:
        g_assert(mace_video_enqueue(&s->video, e));
        break;
    case 0x38:
        e->sequence = value;
        break;
    case 0x40:
        mace_video_fifo_fault(&s->video, value);
        break;
    case 0x48:
        e->period_num = value;
        break;
    case 0x50:
        e->period_den = value;
        break;
    case 0x68:
        fixture_field(s);
        break;
    case 0x70:
        s->seed = value;
        break;
    }
}

static const MemoryRegionOps fixture_ops = {
    .read = fixture_read,
    .write = fixture_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void fixture_reset(Object *obj, ResetType type)
{
    MACEVideoTest *s = MACE_VIDEO_TEST(obj);

    mace_video_reset(&s->video);
    s->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->output_count = 0;
}

static int fixture_pre_save(void *opaque)
{
    mace_video_sync(&MACE_VIDEO_TEST(opaque)->video);
    return 0;
}

static int fixture_post_load(void *opaque, int version)
{
    MACEVideoTest *s = opaque;

    if (s->output_count > ARRAY_SIZE(s->output_time)) {
        return -EINVAL;
    }
    return mace_video_post_load(&s->video);
}

static const VMStateDescription vmstate_fixture_event = {
    .name = "sgi-mace-video-test/programmed-event",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(time, MACEVideoEvent),
        VMSTATE_UINT32(x, MACEVideoEvent),
        VMSTATE_UINT32(y, MACEVideoEvent),
        VMSTATE_UINT32(sample, MACEVideoEvent),
        VMSTATE_UINT64(sequence, MACEVideoEvent),
        VMSTATE_UINT64(period_num, MACEVideoEvent),
        VMSTATE_UINT64(period_den, MACEVideoEvent),
        VMSTATE_UINT8(port, MACEVideoEvent),
        VMSTATE_UINT8(field, MACEVideoEvent),
        VMSTATE_UINT8(type, MACEVideoEvent),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_fixture = {
    .name = TYPE_MACE_VIDEO_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = fixture_pre_save,
    .post_load = fixture_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(mapped_bytes, MACEVideoTest),
        VMSTATE_INT64(epoch, MACEVideoTest),
        VMSTATE_UINT32(seed, MACEVideoTest),
        VMSTATE_STRUCT(event, MACEVideoTest, 0, vmstate_fixture_event,
                       MACEVideoEvent),
        VMSTATE_UINT32(output_count, MACEVideoTest),
        VMSTATE_UINT64_ARRAY(output_time, MACEVideoTest, 8192),
        VMSTATE_UINT64_ARRAY(output_position, MACEVideoTest, 8192),
        VMSTATE_UINT32_ARRAY(output_sample, MACEVideoTest, 8192),
        VMSTATE_UINT16_ARRAY(output_flags, MACEVideoTest, 8192),
        VMSTATE_STRUCT(video, MACEVideoTest, 0, vmstate_mace_video, MACEVideo),
        VMSTATE_END_OF_LIST()
    },
};

static void fixture_realize(DeviceState *dev, Error **errp)
{
    MACEVideoTest *s = MACE_VIDEO_TEST(dev);
    uint64_t size;

    if (s->synthetic_revision > 15) {
        error_setg(errp, "video fixture requires an explicit synthetic "
                         "four-bit revision");
        return;
    }
    s->video.revision = s->synthetic_revision;
    if (!qtest_enabled() || !s->ram || !memory_region_is_ram(s->ram) ||
        memory_region_is_rom(s->ram)) {
        error_setg(errp, "video fixture requires qtest and writable RAM");
        return;
    }
    size = s->mapped_bytes ?: memory_region_size(s->ram);
    if (size > memory_region_size(s->ram)) {
        error_setg(errp, "video fixture mapping exceeds its RAM");
        return;
    }
    memory_region_init_alias(&s->native, OBJECT(dev), "video-test-native",
                             s->ram, 0, size);
    memory_region_init_alias(&s->no_ecc, OBJECT(dev), "video-test-no-ecc",
                             s->ram, 0, size);
    memory_region_add_subregion(&s->dma, 0x40000000, &s->native);
    memory_region_add_subregion(&s->dma, 0x80000000, &s->no_ecc);
    vmstate_register_ram(s->ram, dev);
}

static void fixture_unrealize(DeviceState *dev)
{
    MACEVideoTest *s = MACE_VIDEO_TEST(dev);

    mace_video_cleanup(&s->video);
    vmstate_unregister_ram(s->ram, dev);
}

static void fixture_init(Object *obj)
{
    MACEVideoTest *s = MACE_VIDEO_TEST(obj);

    memory_region_init(&s->dma, obj, "video-test-dma", 1ULL << 32);
    address_space_init(&s->as, &s->dma, "video-test-dma");
    qdev_init_gpio_out(DEVICE(obj), s->irq, 3);
    mace_video_init(&s->video, &s->as, fixture_ust, fixture_irq, s);
    s->video.emit = fixture_emit;
    for (unsigned i = 0; i < 3; i++) {
        MACEVideoPort *p = &s->port[i];

        p->video = &s->video;
        p->channel = i;
        memory_region_init_io(&p->mmio, obj, &mace_video_ops, p,
                              "video-test-channel", 0x80000);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &p->mmio);
    }
    memory_region_init_io(&s->timers, obj, &mace_video_timer_ops,
                          &s->video, "video-test-timers", 24);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->timers);
    memory_region_init_io(&s->fixture, obj, &fixture_ops, s,
                          "video-test-signal", 0x41000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->fixture);
}

static void fixture_finalize(Object *obj)
{
    MACEVideoTest *s = MACE_VIDEO_TEST(obj);

    mace_video_cleanup(&s->video);
    address_space_destroy(&s->as);
}

static const Property fixture_properties[] = {
    DEFINE_PROP_UINT32("synthetic-revision", MACEVideoTest,
                       synthetic_revision, UINT32_MAX),
    DEFINE_PROP_UINT64("mapped-bytes", MACEVideoTest, mapped_bytes, 0),
    DEFINE_PROP_LINK("ram", MACEVideoTest, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void fixture_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->user_creatable = true;
    dc->realize = fixture_realize;
    dc->unrealize = fixture_unrealize;
    dc->vmsd = &vmstate_fixture;
    device_class_set_props(dc, fixture_properties);
    RESETTABLE_CLASS(oc)->phases.hold = fixture_reset;
}

static const TypeInfo fixture_type = {
    .name = TYPE_MACE_VIDEO_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACEVideoTest),
    .instance_init = fixture_init,
    .instance_finalize = fixture_finalize,
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}
type_init(fixture_register_types)
