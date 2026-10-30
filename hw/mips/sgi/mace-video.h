/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_VIDEO_H
#define HW_MIPS_SGI_MACE_VIDEO_H

#include "migration/vmstate.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "system/memory.h"

typedef enum MACEVideoEventType {
    MACE_VIDEO_LOSS,
    MACE_VIDEO_LOCK,
    MACE_VIDEO_SAMPLE,
    MACE_VIDEO_EOF,
    MACE_VIDEO_SOF,
    MACE_VIDEO_LINE,
} MACEVideoEventType;

/*
 * Ports 0/1 are selected primary/secondary inputs, 2/3 internal E/F loopback,
 * and 4 an external timing reference. External producers cannot submit 2/3.
 * SOF x/y describe the raster; LINE x is line length and y is its index.
 * SAMPLE contains the UYVY pair containing pixel x, whose sample is due now.
 * Sequence identifies one field, including callbacks left behind after abort.
 * SOF period_num/period_den is the rational pixel period in nanoseconds.
 */
typedef struct MACEVideoEvent {
    int64_t time;
    uint32_t x;
    uint32_t y;
    uint32_t sample;
    uint64_t sequence;
    uint64_t period_num;
    uint64_t period_den;
    uint64_t generation;
    uint64_t ticks;
    int64_t origin;
    uint8_t port;
    uint8_t field;
    uint8_t type;
    QTAILQ_ENTRY(MACEVideoEvent) next;
} MACEVideoEvent;

typedef QTAILQ_HEAD(, MACEVideoEvent) MACEVideoEventList;

typedef struct MACEVideoActive {
    uint16_t page[32];
    uint8_t unit[8];
    uint32_t width;
    uint32_t lines;
    uint32_t first_x;
    uint32_t first_y;
    uint32_t black_end;
    uint32_t cursor;
    uint32_t row;
    uint32_t column;
    uint8_t count;
    bool interleaved;
    bool valid;
} MACEVideoActive;

typedef struct MACEVideoChannel {
    uint64_t reg[24];
    MACEVideoActive active;
    uint64_t sequence;
    bool halted;
    uint32_t ust;
    uint32_t msc;
    bool field_open;
    uint8_t field;
} MACEVideoChannel;

typedef struct MACEVideoOutput {
    int64_t origin;
    uint64_t base_ticks;
    uint64_t generation;
    uint64_t sequence;
    uint64_t next_sequence;
    uint32_t period_num;
    uint32_t period_den;
    uint32_t pixels;
    uint32_t lines;
    uint32_t blank;
    uint32_t leading;
    uint32_t trailing;
    uint32_t image_x;
    uint32_t image_y;
    bool locked;
} MACEVideoOutput;

typedef struct MACEVideo {
    MACEVideoChannel channel[3];
    uint8_t revision;
    MACEVideoOutput output;
    AddressSpace *as;
    uint32_t (*get_ust)(void *opaque, int64_t event_ns);
    void (*update_irq)(void *opaque, unsigned levels);
    void (*emit)(void *opaque, const MACEVideoEvent *event);
    void *opaque;
    QEMUTimer *timer;
    bool lock[5];
    MACEVideoEventList pending;
} MACEVideo;

typedef struct MACEVideoPort {
    MACEVideo *video;
    unsigned channel;
    MemoryRegion mmio;
} MACEVideoPort;

void mace_video_init(MACEVideo *s, AddressSpace *as,
                     uint32_t (*get_ust)(void *, int64_t),
                     void (*update_irq)(void *, unsigned), void *opaque);
bool mace_video_enqueue(MACEVideo *s, const MACEVideoEvent *event);
void mace_video_fifo_fault(MACEVideo *s, unsigned channel);
void mace_video_cleanup(MACEVideo *s);
void mace_video_reset(MACEVideo *s);
void mace_video_sync(MACEVideo *s);
int mace_video_post_load(MACEVideo *s);
extern const MemoryRegionOps mace_video_ops;
extern const MemoryRegionOps mace_video_timer_ops;
extern const VMStateDescription vmstate_mace_video;

#endif
