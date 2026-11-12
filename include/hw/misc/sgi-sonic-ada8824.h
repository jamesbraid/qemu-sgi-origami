/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic ADA8 class eight-channel converter box (the Lucid 8824 or a
 * sibling) that sits on ESSI0 of the Sonic HDSP card's DSP 2, and the
 * QEMU audio output behind it.
 *
 * The hardware reference is docs/system/mips/sgi/hardware/sonic-hdsp.rst
 * ("The converter box" and "Audio output").
 */

#ifndef HW_MISC_SGI_SONIC_ADA8824_H
#define HW_MISC_SGI_SONIC_ADA8824_H

#include "hw/core/qdev.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_SGI_SONIC_ADA8824 "sonic-ada8824"
OBJECT_DECLARE_SIMPLE_TYPE(SonicAda8824State, SGI_SONIC_ADA8824)

#define SONIC_ADA_RING_FRAMES   4096
#define SONIC_ADA_MAX_CHANNELS  8

struct SonicAda8824State {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    /* Properties. */
    uint32_t address;           /* the 5-bit box address on the link */
    uint32_t cmd2_reply;        /* the answer to command 2 */
    uint32_t audio_channels;    /* channels carried to the audiodev */
    AudioBackend *audio_be;

    /* The link. */
    uint32_t sample_rate;       /* the card's rate, set by the card */
    bool reply_valid;
    uint32_t reply;

    /* Audio: samples gather into frames, frames queue for the backend. */
    SWVoiceOut *voice;
    int32_t frame[SONIC_ADA_MAX_CHANNELS];
    uint32_t frame_pos;
    int32_t ring[SONIC_ADA_RING_FRAMES][SONIC_ADA_MAX_CHANNELS];
    uint32_t ring_head, ring_count;
    uint64_t frames_out;        /* frames handed to the ring */
    uint64_t frames_dropped;    /* frames lost to a full ring */
};

/* The card sets the rate it clocks the link at, before the first word. */
void sonic_ada8824_set_rate(SonicAda8824State *b, uint32_t hz);

/*
 * One word the DSP shifts out of ESSI0 (@wl significant bits, right
 * aligned), and the word the box shifts in for the same slot. rx returns
 * false when the box drives nothing.
 */
void sonic_ada8824_tx(SonicAda8824State *b, uint32_t word, unsigned wl);
bool sonic_ada8824_rx(SonicAda8824State *b, uint32_t *word, unsigned wl);

#endif /* HW_MISC_SGI_SONIC_ADA8824_H */
