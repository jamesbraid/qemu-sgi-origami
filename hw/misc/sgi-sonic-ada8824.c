/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic ADA8 class converter box on ESSI0 of the Sonic HDSP card's DSP 2.
 *
 * The box answers the 16-bit command words DSP 2 sends on the link and
 * carries the 24-bit words DSP 2 sends in audio mode to a QEMU audiodev.
 * The card (sgi-sonic-hdsp-dsp.c) calls in with one word per ESSI slot.
 *
 * Hardware reference: docs/system/mips/sgi/hardware/sonic-hdsp.rst
 * ("The converter box" and "Audio output").
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/sgi-sonic-ada8824.h"

/* A per-word condition would flood the log: say it once per site. */
#define LOG_UNIMP_ONCE(...) \
    do { \
        static bool once_; \
        if (!once_) { \
            once_ = true; \
            qemu_log_mask(LOG_UNIMP, __VA_ARGS__); \
        } \
    } while (0)

#define CMD_ADDRESS(w)  ((w) & 0x1f)
#define CMD_COMMAND(w)  (((w) >> 11) & 0x1f)

#define CMD_IDENTIFY    1       /* answers 288: the library's ADA8 test */
#define CMD_VERSION     2
#define CMD_BOARD       7
#define ADA8_ID         288

void sonic_ada8824_set_rate(SonicAda8824State *b, uint32_t hz)
{
    b->sample_rate = hz;
}

/* ------------------------------------------------------------------ */
/* Audio                                                              */
/* ------------------------------------------------------------------ */

static void ada_drain(SonicAda8824State *b)
{
    size_t bytes = b->audio_channels * sizeof(int32_t);

    while (b->voice && b->ring_count) {
        size_t n = audio_be_write(b->audio_be, b->voice,
                                  b->ring[b->ring_head], bytes);

        if (n < bytes) {
            return;             /* the backend is full: try at its callback */
        }
        b->ring_head = (b->ring_head + 1) % SONIC_ADA_RING_FRAMES;
        b->ring_count--;
    }
}

static void ada_audio_cb(void *opaque, int avail)
{
    ada_drain(opaque);
}

static bool ada_audio_open(SonicAda8824State *b)
{
    struct audsettings as = {
        .freq = b->sample_rate,
        .nchannels = b->audio_channels,
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = HOST_BIG_ENDIAN,
    };

    if (!b->audio_be) {
        return false;
    }
    if (!b->voice) {
        b->voice = audio_be_open_out(b->audio_be, NULL, "sonic-ada8824", b,
                                     ada_audio_cb, &as);
        if (!b->voice) {
            return false;
        }
        audio_be_set_active_out(b->audio_be, b->voice, true);
    }
    return true;
}

static void ada_audio_word(SonicAda8824State *b, uint32_t word)
{
    /* 24 significant bits, carried left aligned in a signed 32-bit sample. */
    b->frame[b->frame_pos++] = (int32_t)(word << 8);
    if (b->frame_pos < b->audio_channels) {
        return;
    }
    b->frame_pos = 0;
    b->frames_out++;
    if (b->ring_count == SONIC_ADA_RING_FRAMES) {
        b->frames_dropped++;
        return;
    }
    memcpy(b->ring[(b->ring_head + b->ring_count) % SONIC_ADA_RING_FRAMES],
           b->frame, sizeof(b->frame));
    b->ring_count++;
    if (ada_audio_open(b)) {
        ada_drain(b);
    }
}

/* ------------------------------------------------------------------ */
/* The link                                                           */
/* ------------------------------------------------------------------ */

static void ada_command(SonicAda8824State *b, uint32_t word)
{
    b->reply_valid = false;
    if (CMD_ADDRESS(word) != b->address) {
        return;             /* not ours: the box drives no reply */
    }
    b->reply_valid = true;
    switch (CMD_COMMAND(word)) {
    case CMD_IDENTIFY:
        b->reply = ADA8_ID;
        break;
    case CMD_VERSION:
        b->reply = b->cmd2_reply;
        break;
    case CMD_BOARD:
        b->reply = 0;
        break;
    default:
        LOG_UNIMP_ONCE("sonic-ada8824: command %u at address %u not "
                       "modelled\n", CMD_COMMAND(word), b->address);
        b->reply = 0;
        break;
    }
}

void sonic_ada8824_tx(SonicAda8824State *b, uint32_t word, unsigned wl)
{
    switch (wl) {
    case 16:
        ada_command(b, word & 0xffff);
        break;
    case 24:
        ada_audio_word(b, word & 0xffffff);
        break;
    default:
        LOG_UNIMP_ONCE("sonic-ada8824: %u-bit word not modelled\n", wl);
        break;
    }
}

bool sonic_ada8824_rx(SonicAda8824State *b, uint32_t *word, unsigned wl)
{
    if (wl == 24) {
        *word = 0;          /* the box's input side delivers silence */
        return true;
    }
    if (wl == 16 && b->reply_valid) {
        *word = b->reply & 0xffff;
        b->reply_valid = false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

static void ada_realize(DeviceState *dev, Error **errp)
{
    SonicAda8824State *b = SGI_SONIC_ADA8824(dev);

    if (b->address > 0x1f) {
        error_setg(errp, "sonic-ada8824: address must be 0 to 31");
        return;
    }
    if (!b->audio_channels || b->audio_channels > SONIC_ADA_MAX_CHANNELS) {
        error_setg(errp, "sonic-ada8824: audio-channels must be 1 to %d",
                   SONIC_ADA_MAX_CHANNELS);
        return;
    }
    if (b->audio_be && !audio_be_check(&b->audio_be, errp)) {
        return;
    }
    if (!b->sample_rate) {
        b->sample_rate = 48000;
    }
}

static void ada_unrealize(DeviceState *dev)
{
    SonicAda8824State *b = SGI_SONIC_ADA8824(dev);

    if (b->voice) {
        audio_be_close_out(b->audio_be, b->voice);
        b->voice = NULL;
    }
}

static ResettablePhases ada_parent_phases;

static void ada_reset_hold(Object *obj, ResetType type)
{
    SonicAda8824State *b = SGI_SONIC_ADA8824(obj);

    if (ada_parent_phases.hold) {
        ada_parent_phases.hold(obj, type);
    }
    b->reply_valid = false;
    b->reply = 0;
    b->frame_pos = 0;
    b->ring_head = b->ring_count = 0;
}

static void ada_instance_init(Object *obj)
{
    SonicAda8824State *b = SGI_SONIC_ADA8824(obj);

    object_property_add_uint64_ptr(obj, "frames-out", &b->frames_out,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "frames-dropped", &b->frames_dropped,
                                   OBJ_PROP_FLAG_READ);
}

static int ada_post_load(void *opaque, int version_id)
{
    SonicAda8824State *b = opaque;

    if (b->frame_pos >= SONIC_ADA_MAX_CHANNELS) {
        return -EINVAL;
    }
    /* Frames queued for the backend are not migrated: the stream restarts. */
    b->ring_head = b->ring_count = 0;
    return 0;
}

static const VMStateDescription vmstate_sonic_ada8824 = {
    .name = "sonic-ada8824",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ada_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(reply_valid, SonicAda8824State),
        VMSTATE_UINT32(reply, SonicAda8824State),
        VMSTATE_INT32_ARRAY(frame, SonicAda8824State, SONIC_ADA_MAX_CHANNELS),
        VMSTATE_UINT32(frame_pos, SonicAda8824State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ada_properties[] = {
    DEFINE_PROP_UINT32("address", SonicAda8824State, address, 1),
    DEFINE_PROP_UINT32("x-cmd2-reply", SonicAda8824State, cmd2_reply, 0x0100),
    DEFINE_PROP_UINT32("audio-channels", SonicAda8824State, audio_channels, 2),
    DEFINE_AUDIO_PROPERTIES(SonicAda8824State, audio_be),
};

static void ada_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ada_realize;
    dc->unrealize = ada_unrealize;
    dc->vmsd = &vmstate_sonic_ada8824;
    resettable_class_set_parent_phases(rc, NULL, ada_reset_hold, NULL,
                                       &ada_parent_phases);
    device_class_set_props(dc, ada_properties);
    dc->desc = "Sonic ADA8 class converter box (Lucid 8824)";
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo ada_info = {
    .name = TYPE_SGI_SONIC_ADA8824,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SonicAda8824State),
    .instance_init = ada_instance_init,
    .class_init = ada_class_init,
};

static void sonic_ada8824_register_types(void)
{
    type_register_static(&ada_info);
}

type_init(sonic_ada8824_register_types)
