/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Where a board's video goes: a sink takes each frame the board plays out.
 * A board holds the sink as a link property and calls these through the
 * object, so a file today and another backend later plug in unchanged.
 */
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
    /* Interleaved 16-bit PCM, frames * channels samples. */
    void (*put_audio)(Object *sink, const int16_t *pcm, size_t frames);
};

static inline void sgi_video_sink_put_frame(Object *sink,
                                            const SGIVideoFrame *f)
{
    SGI_VIDEO_SINK_GET_CLASS(sink)->put_frame(sink, f);
}

static inline void sgi_video_sink_put_audio(Object *sink, const int16_t *pcm,
                                            size_t frames)
{
    SGI_VIDEO_SINK_GET_CLASS(sink)->put_audio(sink, pcm, frames);
}

#endif
