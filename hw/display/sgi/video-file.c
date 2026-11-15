/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Video file sink: every frame a board plays out, as Y4M, plus optional
 * program audio as WAV. See docs/system/mips/sgi/video-io.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "qemu/log.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "exec/icount.h"
#include "hw/display/sgi/video-io.h"

#define TYPE_SGI_VIDEO_FILE_SINK "sgi-video-file-sink"
OBJECT_DECLARE_SIMPLE_TYPE(SGIVideoFileSink, SGI_VIDEO_FILE_SINK)

#define SINK_QUEUE 8
#define WAV_RATE 48000
#define WAV_CHANNELS 2

struct SGIVideoFileSink {
    DeviceState parent_obj;

    char *path;
    char *audio_path;
    int fd, audio_fd;
    uint64_t audio_bytes;

    QemuThread thread;
    QemuMutex lock;
    QemuCond wake;              /* the queue changed */
    bool started, stop;
    SGIVideoFrame *queue[SINK_QUEUE];
    unsigned head, count;
    SGIVideoFormat fmt;         /* of the header; valid once header is set */
    bool header;

    uint64_t frames, dropped, written;
};

static bool sink_write(int fd, const void *buf, size_t len)
{
    return qemu_write_full(fd, buf, len) == len;
}

/* Write one frame; a frame whose format differs from the header's is lost. */
static bool sink_write_frame(SGIVideoFileSink *s, const SGIVideoFrame *f)
{
    g_autofree uint8_t *buf = NULL;
    size_t len;

    if (!s->header) {
        g_autofree char *h = sgi_y4m_header(&f->fmt);

        s->fmt = f->fmt;
        s->header = true;
        if (!sink_write(s->fd, h, strlen(h))) {
            return false;
        }
    } else if (memcmp(&s->fmt, &f->fmt, sizeof(s->fmt))) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SINK
                      ": frame format changed mid-stream, frame dropped\n");
        return false;
    }
    buf = g_malloc(6 + sgi_video_frame_bytes(&f->fmt));
    len = sgi_y4m_frame_put(f, buf);
    return sink_write(s->fd, buf, len);
}

static void *sink_thread(void *opaque)
{
    SGIVideoFileSink *s = opaque;

    qemu_mutex_lock(&s->lock);
    for (;;) {
        SGIVideoFrame *f;
        bool ok;

        while (!s->count && !s->stop) {
            qemu_cond_wait(&s->wake, &s->lock);
        }
        if (!s->count) {
            break;
        }
        f = s->queue[s->head];
        qemu_mutex_unlock(&s->lock);
        ok = sink_write_frame(s, f);
        sgi_video_frame_free(f);
        qemu_mutex_lock(&s->lock);
        s->head = (s->head + 1) % SINK_QUEUE;
        s->count--;
        if (ok) {
            s->written++;
        } else {
            s->dropped++;
        }
        qemu_cond_broadcast(&s->wake);
    }
    qemu_mutex_unlock(&s->lock);
    return NULL;
}

/*
 * Realtime drops a frame when the writer is behind and counts it; under
 * icount the run is the clock, so the board waits for room instead.
 */
static void sink_put_frame(Object *obj, const SGIVideoFrame *f)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(obj);

    qemu_mutex_lock(&s->lock);
    s->frames++;
    while (s->count == SINK_QUEUE && icount_enabled()) {
        qemu_cond_wait(&s->wake, &s->lock);
    }
    if (s->count == SINK_QUEUE) {
        s->dropped++;
    } else {
        s->queue[(s->head + s->count++) % SINK_QUEUE] = sgi_video_frame_dup(f);
        qemu_cond_broadcast(&s->wake);
    }
    qemu_mutex_unlock(&s->lock);
}

static void sink_put_audio(Object *obj, const int16_t *pcm, size_t frames)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(obj);
    size_t bytes = frames * WAV_CHANNELS * sizeof(int16_t);
    g_autofree uint8_t *le = NULL;

    if (s->audio_fd < 0) {
        return;
    }
    le = g_malloc(bytes);
    for (size_t i = 0; i < frames * WAV_CHANNELS; i++) {
        stw_le_p(le + 2 * i, pcm[i]);
    }
    if (sink_write(s->audio_fd, le, bytes)) {
        s->audio_bytes += bytes;
    }
}

static void sink_realize(DeviceState *dev, Error **errp)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(dev);
    uint8_t wav[SGI_WAV_HEADER_BYTES];

    if (!s->path) {
        error_setg(errp, "sgi-video-file-sink needs a path");
        return;
    }
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->wake);
    s->fd = qemu_create(s->path, O_WRONLY | O_TRUNC | O_BINARY, 0666, errp);
    if (s->fd < 0) {
        return;
    }
    if (s->audio_path) {
        s->audio_fd = qemu_create(s->audio_path, O_WRONLY | O_TRUNC | O_BINARY,
                                  0666, errp);
        if (s->audio_fd < 0) {
            return;
        }
        sgi_wav_header(wav, WAV_RATE, WAV_CHANNELS, 0);
        if (!sink_write(s->audio_fd, wav, sizeof(wav))) {
            error_setg_errno(errp, errno, "cannot write %s", s->audio_path);
            return;
        }
    }
    qemu_thread_create(&s->thread, "sgi-video-sink", sink_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->started = true;
}

static void sink_init(Object *obj)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(obj);

    s->fd = s->audio_fd = -1;
    object_property_add_uint64_ptr(obj, "frames", &s->frames,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "dropped", &s->dropped,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "written", &s->written,
                                   OBJ_PROP_FLAG_READ);
}

static void sink_unrealize(DeviceState *dev)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(dev);

    if (s->started) {
        qemu_mutex_lock(&s->lock);
        s->stop = true;
        qemu_cond_broadcast(&s->wake);
        qemu_mutex_unlock(&s->lock);
        qemu_thread_join(&s->thread);
    }
    if (s->audio_fd >= 0) {
        uint8_t wav[SGI_WAV_HEADER_BYTES];

        sgi_wav_header(wav, WAV_RATE, WAV_CHANNELS, s->audio_bytes);
        if (lseek(s->audio_fd, 0, SEEK_SET) == 0) {
            sink_write(s->audio_fd, wav, sizeof(wav));
        }
        close(s->audio_fd);
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    qemu_cond_destroy(&s->wake);
    qemu_mutex_destroy(&s->lock);
}

static const Property sink_properties[] = {
    DEFINE_PROP_STRING("path", SGIVideoFileSink, path),
    DEFINE_PROP_STRING("audio", SGIVideoFileSink, audio_path),
};

static void sink_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIVideoSinkClass *sc = SGI_VIDEO_SINK_CLASS(oc);

    dc->realize = sink_realize;
    dc->unrealize = sink_unrealize;
    dc->user_creatable = true;
    dc->desc = "SGI video file sink (Y4M and WAV)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, sink_properties);
    sc->put_frame = sink_put_frame;
    sc->put_audio = sink_put_audio;
}

static const TypeInfo video_file_types[] = {
    {
        .name = TYPE_SGI_VIDEO_SINK,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIVideoSinkClass),
    },
    {
        .name = TYPE_SGI_VIDEO_FILE_SINK,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIVideoFileSink),
        .instance_init = sink_init,
        .class_init = sink_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_VIDEO_SINK },
            { }
        },
    },
};

DEFINE_TYPES(video_file_types)
