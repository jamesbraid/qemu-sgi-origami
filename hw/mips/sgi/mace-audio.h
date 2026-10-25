/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_AUDIO_H
#define HW_MIPS_SGI_MACE_AUDIO_H

#include "migration/vmstate.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "system/memory.h"

enum {
    MACE_AUDIO_LINE,
    MACE_AUDIO_AUX1,
    MACE_AUDIO_AUX2,
    MACE_AUDIO_AUX3,
    MACE_AUDIO_MIC,
    MACE_AUDIO_MONO,
    MACE_AUDIO_SUM,
    MACE_AUDIO_INPUTS,
};

typedef struct MACEAudioChannel {
    uint16_t control;
    uint16_t read;
    uint16_t write;
    uint8_t data[32];
    uint8_t count;
    uint8_t cursor;
    uint8_t fault;
    uint32_t msc;
    uint32_t ust;
} MACEAudioChannel;

typedef struct MACEAudioClock {
    int64_t time;
    int64_t progress;
    uint64_t rate;
    int64_t next;
    int64_t phase_next;
    uint64_t work_index;
    uint16_t phase;
    uint16_t phase_latch;
} MACEAudioClock;

typedef struct MACEAudioConverter {
    uint64_t rate;
    uint8_t edges;
    bool active;
    int16_t fifo[8];
    uint8_t head;
    uint8_t count;
    bool requested;
    double previous[2];
    double sample[2];
    int64_t sample_time;
    double output_previous[2];
    double output[2];
    int64_t output_time;
    uint64_t converted;
    uint16_t applied_gain;
    uint16_t pending_gain;
    uint16_t gain_wait;
} MACEAudioConverter;

typedef struct MACEAudio {
    MACEAudioChannel channel[3];
    MACEAudioClock clock[4];
    MACEAudioConverter converter[4];
    uint16_t reg[32];
    uint32_t control;
    uint32_t command;
    uint16_t mask;
    uint16_t response;
    uint32_t latched_command;
    uint16_t pending_response;
    uint16_t current_response;
    bool response_pending;
    bool transaction_valid;
    bool playback_valid[2];
    bool capture_valid[2];
    uint16_t capture_slot[2];
    uint8_t group;
    uint8_t serial_stage;
    uint8_t frame_slots;
    uint32_t sclk;
    int64_t next_serial;
    uint64_t serial_fraction;
    int64_t reset_start;
    int64_t init_deadline;
    int64_t power_deadline;
    uint8_t power_state;
    bool calibrated;
    bool presence;
    bool pwrdwn;
    bool pwrdwn_pending;
    uint64_t frames;
    int64_t reference_ready;
    int64_t next_host;
    uint64_t host_fraction;
    double input[MACE_AUDIO_INPUTS][2];
    double line_negative[2];
    double line[2][2];
    double headphone[2];
    double mono;
    bool pdmnft;
    bool volume[2];
    uint32_t fixture_policy;
    QEMUTimer *timer;
    AddressSpace *as;
    uint32_t *ring_base;
    uint32_t (*get_ust)(void *opaque, int64_t event_ns);
    void (*update_irq)(void *opaque, uint32_t status);
    void (*update_ttl)(void *opaque, unsigned outputs);
    void *opaque;
    AudioBackend *backend;
    SWVoiceOut *voice_out;
    SWVoiceIn *voice_in;
    int16_t host_output[4096];
    uint16_t host_head;
    uint16_t host_count;
} MACEAudio;

void mace_audio_init(MACEAudio *s, AddressSpace *as, uint32_t *ring_base,
                     uint32_t (*get_ust)(void *, int64_t),
                     void (*update_irq)(void *, uint32_t), void *opaque);
bool mace_audio_realize(MACEAudio *s, Error **errp);
void mace_audio_reset(MACEAudio *s);
void mace_audio_cleanup(MACEAudio *s);
bool mace_audio_base_allowed(MACEAudio *s);
uint64_t mace_audio_read(MACEAudio *s, hwaddr offset, unsigned size);
void mace_audio_write(MACEAudio *s, hwaddr offset, uint64_t value,
                      unsigned size);
uint64_t mace_audio_timestamp_read(MACEAudio *s, unsigned channel,
                                   unsigned half, unsigned size);
void mace_audio_timestamp_write(MACEAudio *s, unsigned channel,
                                unsigned half, unsigned size, uint64_t value);
void mace_audio_volume(MACEAudio *s, unsigned button, bool level);
void mace_audio_set_input(MACEAudio *s, unsigned pin, int32_t value);
void mace_audio_pdmnft(MACEAudio *s, bool level);
void mace_audio_pwrdwn(MACEAudio *s, bool level);
void mace_audio_sync(MACEAudio *s);
int mace_audio_post_load(MACEAudio *s);
bool mace_audio_frequency(MACEAudio *s, unsigned generator, bool bit,
                          uint64_t *numerator, uint64_t *denominator);
extern const MemoryRegionOps mace_audio_ops;
extern const VMStateDescription vmstate_mace_audio;

#endif
