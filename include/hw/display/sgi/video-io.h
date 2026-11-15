/* SPDX-License-Identifier: BSD-3-Clause */
/* Where a board's video goes: a sink takes each frame it plays out. */
#ifndef HW_DISPLAY_SGI_VIDEO_IO_H
#define HW_DISPLAY_SGI_VIDEO_IO_H

#include "qom/object.h"
#include "hw/display/sgi/video-port.h"

#define TYPE_SGI_VIDEO_SINK "sgi-video-sink"

typedef struct SGIVideoSinkClass SGIVideoSinkClass;
DECLARE_CLASS_CHECKERS(SGIVideoSinkClass, SGI_VIDEO_SINK, TYPE_SGI_VIDEO_SINK)

struct SGIVideoSinkClass {
    InterfaceClass parent;

    /* One played-out frame; the sink copies it. Under the big lock. */
    void (*put_frame)(Object *sink, const SGIVideoFrame *f);
};

static inline void sgi_video_sink_put_frame(Object *sink,
                                            const SGIVideoFrame *f)
{
    SGI_VIDEO_SINK_GET_CLASS(sink)->put_frame(sink, f);
}

#define TYPE_SGI_VIDEO_SOURCE "sgi-video-source"

typedef struct SGIVideoSourceClass SGIVideoSourceClass;
DECLARE_CLASS_CHECKERS(SGIVideoSourceClass, SGI_VIDEO_SOURCE,
                       TYPE_SGI_VIDEO_SOURCE)

struct SGIVideoSourceClass {
    InterfaceClass parent;

    /* The source's raster and rate; false when it has no signal. */
    bool (*has_signal)(Object *src, SGIVideoFormat *fmt);
    /* The frame for the board's next frame time (a repeat when behind). */
    const SGIVideoFrame *(*next_frame)(Object *src);
    /* Frames handed out so far, and a seek back to such a count. */
    uint64_t (*position)(Object *src);
    void (*seek)(Object *src, uint64_t position);
};

static inline bool sgi_video_source_signal(Object *src, SGIVideoFormat *fmt)
{
    return SGI_VIDEO_SOURCE_GET_CLASS(src)->has_signal(src, fmt);
}

static inline const SGIVideoFrame *sgi_video_source_next(Object *src)
{
    return SGI_VIDEO_SOURCE_GET_CLASS(src)->next_frame(src);
}

#endif
