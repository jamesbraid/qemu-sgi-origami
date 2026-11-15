/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Video file sink: every frame a board plays out, as Y4M.
 * See docs/system/mips/sgi/video-io.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/thread.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "exec/icount.h"
#include "hw/display/sgi/video-io.h"

#define TYPE_SGI_VIDEO_FILE_SINK "sgi-video-file-sink"
OBJECT_DECLARE_SIMPLE_TYPE(SGIVideoFileSink, SGI_VIDEO_FILE_SINK)

#define SINK_QUEUE 8
/* How long a waiting board gives a stuck writer before it drops a frame. */
#define SINK_WAIT_MS 10000

struct SGIVideoFileSink {
    DeviceState parent_obj;

    char *path;
    bool lockstep;
    int fd;

    QemuThread thread;
    QemuMutex lock;
    QemuCond wake;              /* the queue changed */
    bool started, stop;
    SGIVideoFrame *queue[SINK_QUEUE];
    unsigned head, count;
    SGIVideoFormat fmt;         /* of the header; valid once header is set */
    bool header, failed, stuck;

    uint64_t frames, dropped, written;
};

static bool sink_write(SGIVideoFileSink *s, const void *buf, size_t len)
{
    if (qemu_write_full(s->fd, buf, len) == len) {
        return true;
    }
    if (!s->failed) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SINK
                      ": cannot write %s: %s; later frames are dropped\n",
                      s->path, strerror(errno));
        s->failed = true;
    }
    return false;
}

/* Write one frame; a frame whose format differs from the header's is lost. */
static bool sink_write_frame(SGIVideoFileSink *s, const SGIVideoFrame *f)
{
    g_autofree uint8_t *buf = NULL;
    size_t len;

    if (s->failed) {
        return false;
    }
    if (!s->header) {
        g_autofree char *h = sgi_y4m_header(&f->fmt);

        s->fmt = f->fmt;
        s->header = true;
        if (!sink_write(s, h, strlen(h))) {
            return false;
        }
    } else if (memcmp(&s->fmt, &f->fmt, sizeof(s->fmt))) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SINK
                      ": frame format changed mid-stream, frame dropped\n");
        return false;
    }
    buf = g_malloc(6 + sgi_video_frame_bytes(&f->fmt));
    len = sgi_y4m_frame_put(f, buf);
    return sink_write(s, buf, len);
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
        s->stuck = false;
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
 * Realtime drops a frame when the writer is behind and counts it. Under
 * icount, or lockstep, the board waits for room, for a bounded time.
 */
static void sink_put_frame(Object *obj, const SGIVideoFrame *f)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(obj);
    int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + SINK_WAIT_MS;

    qemu_mutex_lock(&s->lock);
    s->frames++;
    /* After one timeout the writer is stuck until it next finishes a frame. */
    while (s->count == SINK_QUEUE && !s->stuck &&
           (s->lockstep || icount_enabled())) {
        int64_t left = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (left <= 0 || !qemu_cond_timedwait(&s->wake, &s->lock, left)) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SINK
                          ": writer stuck for %d s, frame dropped\n",
                          SINK_WAIT_MS / 1000);
            s->stuck = true;
            break;
        }
    }
    if (s->count == SINK_QUEUE) {
        s->dropped++;
    } else {
        s->queue[(s->head + s->count++) % SINK_QUEUE] = sgi_video_frame_dup(f);
        qemu_cond_broadcast(&s->wake);
    }
    qemu_mutex_unlock(&s->lock);
}

static void sink_realize(DeviceState *dev, Error **errp)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(dev);

    if (!s->path) {
        error_setg(errp, "sgi-video-file-sink needs a path");
        return;
    }
    s->fd = qemu_create(s->path, O_WRONLY | O_TRUNC | O_BINARY, 0666, errp);
    if (s->fd < 0) {
        return;
    }
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->wake);
    qemu_thread_create(&s->thread, "sgi-video-sink", sink_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->started = true;
}

static void sink_init(Object *obj)
{
    SGIVideoFileSink *s = SGI_VIDEO_FILE_SINK(obj);

    s->fd = -1;
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
        qemu_cond_destroy(&s->wake);
        qemu_mutex_destroy(&s->lock);
        s->started = false;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static const Property sink_properties[] = {
    DEFINE_PROP_STRING("path", SGIVideoFileSink, path),
    DEFINE_PROP_BOOL("lockstep", SGIVideoFileSink, lockstep, false),
};

static void sink_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIVideoSinkClass *sc = SGI_VIDEO_SINK_CLASS(oc);

    dc->realize = sink_realize;
    dc->unrealize = sink_unrealize;
    dc->user_creatable = true;
    dc->desc = "SGI video file sink (Y4M)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, sink_properties);
    sc->put_frame = sink_put_frame;
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
