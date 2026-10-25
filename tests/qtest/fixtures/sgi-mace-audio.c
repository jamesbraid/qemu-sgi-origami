/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic codec pins and inspection; no firmware or physical jack claims. */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi/mace-audio.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/qtest.h"

#define TYPE_MACE_AUDIO_TEST "sgi-mace-audio-test"
OBJECT_DECLARE_SIMPLE_TYPE(MACEAudioTest, MACE_AUDIO_TEST)

struct MACEAudioTest {
    SysBusDevice parent_obj;
    MACEAudio audio;
    MemoryRegion audio_regs;
    MemoryRegion fixture_regs;
    MemoryRegion dma;
    MemoryRegion native;
    MemoryRegion no_ecc;
    MemoryRegion *ram;
    uint64_t mapped_bytes;
    AddressSpace as;
    qemu_irq irq;
    uint32_t base;
    uint32_t status;
    uint32_t mask;
};

static uint32_t fixture_ust(void *opaque, int64_t event_ns)
{
    return event_ns / 960;
}

static void fixture_irq(void *opaque, uint32_t status)
{
    MACEAudioTest *s = opaque;

    s->status = status;
    qemu_set_irq(s->irq, !!(s->status & s->mask));
}

static uint64_t fixture_read(void *opaque, hwaddr offset, unsigned size)
{
    MACEAudioTest *s = opaque;
    MACEAudio *a = &s->audio;

    mace_audio_sync(a);
    if (offset >= 0x500 && offset < 0x600) {
        MACEAudioClock *c = &a->clock[(offset - 0x500) / 0x40];

        switch (offset & 0x3f) {
        case 0:
            return c->next;
        case 8:
            return c->progress +
                (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - c->time) *
                (int64_t)c->rate * 3;
        case 16:
            return c->rate;
        case 24:
            return c->phase_next;
        case 32:
            return c->work_index;
        }
    }
    if (offset >= 0x600 && offset < 0x680) {
        MACEAudioConverter *c = &a->converter[(offset - 0x600) / 0x20];

        return offset & 8 ? c->sample_time : c->converted;
    }
    if (offset >= 0x700 && offset < 0x760) {
        unsigned generator = (offset - 0x700) / 0x20;
        uint64_t numerator, denominator;

        if (!mace_audio_frequency(a, generator, !(offset & 16),
                                  &numerator, &denominator)) {
            return offset & 8 ? 0 : UINT64_MAX;
        }
        return offset & 8 ? denominator : numerator;
    }
    if (offset >= 0x200 && offset < 0x300) {
        return a->reg[(offset - 0x200) / 8];
    }
    if (offset >= 0x400 && offset < 0x418) {
        return mace_audio_timestamp_read(a, (offset - 0x400) / 8,
                                         (offset & 4) != 0, size);
    }
    switch (offset) {
    case 0x100:
    case 0x108:
    case 0x110:
    case 0x118:
        return (int32_t)a->line[(offset - 0x100) / 16][(offset / 8) & 1];
    case 0x120:
    case 0x128:
        return (int32_t)a->headphone[(offset / 8) & 1];
    case 0x130:
        return (int32_t)a->mono;
    case 0x150:
        return a->frames;
    case 0x158:
        return a->sclk;
    case 0x160:
        return a->group * 8 + a->serial_stage;
    case 0x168:
        return a->channel[0].fault | a->channel[1].fault << 8 |
               a->channel[2].fault << 16;
    case 0x170:
        return a->converter[0].edges | a->converter[1].edges << 8 |
               a->converter[2].edges << 16 | a->converter[3].edges << 24;
    case 0x178:
        return a->next_serial;
    case 0x180:
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    case 0x188:
        return s->base;
    case 0x190:
        return s->status;
    case 0x198:
        return s->mask;
    case 0x300:
        return a->converter[0].count | a->converter[1].count << 8 |
               a->converter[2].count << 16 | a->converter[3].count << 24;
    case 0x308:
        return a->converter[2].requested | a->converter[3].requested << 1;
    case 0x310:
        return a->converter[2].applied_gain |
               (uint32_t)a->converter[3].applied_gain << 16;
    case 0x318:
    case 0x320:
    case 0x328:
        return a->clock[(offset - 0x318) / 8].phase;
    case 0x330:
    case 0x338:
    case 0x340:
        return a->clock[(offset - 0x330) / 8].phase_latch;
    case 0x350:
        return (!!a->voice_out) | (!!a->voice_in << 1);
    case 0x358:
        return a->host_count;
    case 0x348:
        return a->converter[2].gain_wait |
               (uint32_t)a->converter[3].gain_wait << 16;
    default:
        return 0;
    }
}

static void fixture_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MACEAudioTest *s = opaque;
    MACEAudio *a = &s->audio;

    mace_audio_sync(a);
    if (offset < 0x90 && !(offset & 7)) {
        if (offset < 0x70) {
            mace_audio_set_input(a, offset / 8, value);
        }
        return;
    }
    if (offset >= 0x400 && offset < 0x418) {
        mace_audio_timestamp_write(a, (offset - 0x400) / 8,
                                    (offset & 4) != 0, size, value);
        return;
    }
    switch (offset) {
    case 0x90:
    case 0x98:
        mace_audio_set_input(a, 14 + ((offset / 8) & 1), value);
        break;
    case 0x138:
        mace_audio_pdmnft(a, value & 1);
        break;
    case 0x140:
        mace_audio_pwrdwn(a, value & 1);
        break;
    case 0x148:
        mace_audio_volume(a, 0, value & 1);
        mace_audio_volume(a, 1, value & 2);
        break;
    case 0x188:
        if (((s->base ^ value) & 0xffff8000U) &&
            !mace_audio_base_allowed(a)) {
            break;
        }
        s->base = value & 0xffff8001U;
        break;
    case 0x198:
        s->mask = value & 255;
        fixture_irq(s, s->status);
        break;
    }
}

static const MemoryRegionOps fixture_ops = {
    .read = fixture_read,
    .write = fixture_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

static void fixture_reset(Object *obj, ResetType type)
{
    MACEAudioTest *s = MACE_AUDIO_TEST(obj);

    s->base = 1;
    s->mask = 0;
    mace_audio_reset(&s->audio);
}

static int fixture_pre_save(void *opaque)
{
    mace_audio_sync(&MACE_AUDIO_TEST(opaque)->audio);
    return 0;
}

static int fixture_post_load(void *opaque, int version)
{
    return mace_audio_post_load(&MACE_AUDIO_TEST(opaque)->audio);
}

static const VMStateDescription vmstate_fixture = {
    .name = TYPE_MACE_AUDIO_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = fixture_pre_save,
    .post_load = fixture_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_EQUAL(mapped_bytes, MACEAudioTest),
        VMSTATE_UINT32(base, MACEAudioTest),
        VMSTATE_UINT32(status, MACEAudioTest),
        VMSTATE_UINT32(mask, MACEAudioTest),
        VMSTATE_STRUCT(audio, MACEAudioTest, 0, vmstate_mace_audio, MACEAudio),
        VMSTATE_END_OF_LIST()
    },
};

static void fixture_realize(DeviceState *dev, Error **errp)
{
    MACEAudioTest *s = MACE_AUDIO_TEST(dev);

    if (!qtest_enabled() || !s->ram || !memory_region_is_ram(s->ram) ||
        memory_region_is_rom(s->ram)) {
        error_setg(errp, "audio fixture requires qtest and writable RAM");
        return;
    }
    if (s->mapped_bytes > memory_region_size(s->ram)) {
        error_setg(errp, "audio fixture mapping exceeds its RAM");
        return;
    }
    uint64_t size = s->mapped_bytes ?: memory_region_size(s->ram);

    memory_region_init_alias(&s->native, OBJECT(dev), "audio-test-native",
                             s->ram, 0, size);
    memory_region_init_alias(&s->no_ecc, OBJECT(dev), "audio-test-no-ecc",
                             s->ram, 0, size);
    memory_region_add_subregion(&s->dma, 0x40000000, &s->native);
    memory_region_add_subregion(&s->dma, 0x80000000, &s->no_ecc);
    if (!mace_audio_realize(&s->audio, errp)) {
        return;
    }
    vmstate_register_ram(s->ram, dev);
}

static void fixture_unrealize(DeviceState *dev)
{
    MACEAudioTest *s = MACE_AUDIO_TEST(dev);

    mace_audio_cleanup(&s->audio);
    vmstate_unregister_ram(s->ram, dev);
}

static void fixture_init(Object *obj)
{
    MACEAudioTest *s = MACE_AUDIO_TEST(obj);

    memory_region_init(&s->dma, obj, "audio-test-dma", 1ULL << 32);
    address_space_init(&s->as, &s->dma, "audio-test-dma");
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    mace_audio_init(&s->audio, &s->as, &s->base,
                     fixture_ust, fixture_irq, s);
    memory_region_init_io(&s->audio_regs, obj, &mace_audio_ops, &s->audio,
                          "audio-test-registers", 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->audio_regs);
    memory_region_init_io(&s->fixture_regs, obj, &fixture_ops, s,
                          "audio-test-pins", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->fixture_regs);
}

static void fixture_finalize(Object *obj)
{
    MACEAudioTest *s = MACE_AUDIO_TEST(obj);

    mace_audio_cleanup(&s->audio);
    address_space_destroy(&s->as);
}

static const Property fixture_properties[] = {
    DEFINE_AUDIO_PROPERTIES(MACEAudioTest, audio.backend),
    DEFINE_PROP_UINT64("mapped-bytes", MACEAudioTest, mapped_bytes, 0),
    DEFINE_PROP_LINK("ram", MACEAudioTest, ram, TYPE_MEMORY_REGION,
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
    .name = TYPE_MACE_AUDIO_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACEAudioTest),
    .instance_init = fixture_init,
    .instance_finalize = fixture_finalize,
    .class_init = fixture_class_init,
};

static void fixture_register_types(void)
{
    type_register_static(&fixture_type);
}
type_init(fixture_register_types)
