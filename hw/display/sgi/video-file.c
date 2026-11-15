/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Video file sink (every frame a board plays out, as Y4M) and source (Y4M
 * or raw frames played in). See docs/system/mips/sgi/video-io.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/thread.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "exec/icount.h"
#include <poll.h>
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


/* ---- The file source ---- */

#define TYPE_SGI_VIDEO_FILE_SOURCE "sgi-video-file-source"
OBJECT_DECLARE_SIMPLE_TYPE(SGIVideoFileSource, SGI_VIDEO_FILE_SOURCE)

#define SRC_QUEUE 4
/* How long a waiting board gives a source that has no frame. */
#define SRC_WAIT_MS 10000

typedef enum SrcRead {
    SRC_OK,
    SRC_END,                    /* a clean end of file */
    SRC_ERR,                    /* corrupt, truncated or unreadable: logged */
    SRC_STOP,
} SrcRead;

struct SGIVideoFileSource {
    DeviceState parent_obj;

    char *path, *format, *rate, *fields, *timecode;
    uint32_t width, height, depth;
    bool loop, lockstep;

    int fd;
    off_t data_start;
    uint64_t stride, total;     /* bytes of a frame in the file; frames or 0 */
    bool y4m, seekable;
    SGIVideoFormat fmt;
    bool drop_frame;
    int64_t tc_start;

    QemuThread thread;
    QemuMutex lock;
    QemuCond wake;
    bool started, stop, eof, ended, starved;
    bool seek_pending;
    uint64_t seek_to;
    SGIVideoFrame *queue[SRC_QUEUE];
    unsigned head, count;
    SGIVideoFrame *cur;

    uint64_t delivered, repeated;
};

/* Read len bytes, polling so that a stop request is honoured on a FIFO. */
static SrcRead src_read(SGIVideoFileSource *s, void *buf, size_t len,
                        size_t *got)
{
    uint8_t *p = buf;

    *got = 0;
    while (*got < len) {
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
        ssize_t n;

        if (s->stop) {
            return SRC_STOP;
        }
        if (poll(&pfd, 1, 50) <= 0) {
            continue;
        }
        n = read(s->fd, p + *got, len - *got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                          ": cannot read %s: %s\n", s->path, strerror(errno));
            return SRC_ERR;
        }
        if (n == 0) {
            return SRC_END;
        }
        *got += n;
    }
    return SRC_OK;
}

static SrcRead src_read_frame(SGIVideoFileSource *s, SGIVideoFrame **out)
{
    size_t bytes = sgi_video_frame_bytes(&s->fmt), got;
    g_autofree uint8_t *buf = g_malloc(bytes);
    SrcRead r;

    if (s->y4m) {
        char line[6];

        r = src_read(s, line, sizeof(line), &got);
        if (r == SRC_END && !got) {
            return SRC_END;
        }
        if (r == SRC_END) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                          ": %s ends inside a frame marker\n", s->path);
            return SRC_ERR;
        }
        if (r != SRC_OK) {
            return r;
        }
        if (memcmp(line, "FRAME\n", sizeof(line))) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                          ": %s has a corrupt frame marker\n", s->path);
            return SRC_ERR;
        }
    }
    r = src_read(s, buf, bytes, &got);
    if (r == SRC_END) {
        if (!s->y4m && !got) {
            return SRC_END;
        }
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                      ": %s ends inside a frame (%zu of %zu bytes)\n",
                      s->path, got, bytes);
        return SRC_ERR;
    }
    if (r != SRC_OK) {
        return r;
    }
    *out = sgi_video_frame_new(&s->fmt);
    sgi_y4m_frame_get(*out, buf);
    return SRC_OK;
}

static void src_flush(SGIVideoFileSource *s)
{
    while (s->count) {
        sgi_video_frame_free(s->queue[s->head]);
        s->head = (s->head + 1) % SRC_QUEUE;
        s->count--;
    }
}

/* Reader only, lock held: put the file at frame n, in one seek. */
static void src_do_seek(SGIVideoFileSource *s)
{
    uint64_t n = s->seek_to;
    bool ok = true;

    src_flush(s);
    s->eof = false;
    s->starved = false;
    if (s->loop && s->total) {
        n %= s->total;
    }
    if (s->seekable) {
        if (s->total && n >= s->total) {
            s->eof = true;
        } else {
            ok = lseek(s->fd, s->data_start + n * s->stride, SEEK_SET) >= 0;
        }
    } else if (n) {
        ok = false;
    }
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                      ": cannot seek %s to frame %" PRIu64 "\n", s->path, n);
        s->eof = true;
    }
    s->seek_pending = false;
    qemu_cond_broadcast(&s->wake);
}

static void *src_thread(void *opaque)
{
    SGIVideoFileSource *s = opaque;
    bool fresh = false;         /* a frame since the last rewind */

    qemu_mutex_lock(&s->lock);
    while (!s->stop) {
        SGIVideoFrame *f = NULL;
        SrcRead r;

        if (s->seek_pending) {
            src_do_seek(s);
            fresh = true;
            continue;
        }
        if (s->eof || s->count == SRC_QUEUE) {
            qemu_cond_wait(&s->wake, &s->lock);
            continue;
        }
        qemu_mutex_unlock(&s->lock);
        r = src_read_frame(s, &f);
        if (r == SRC_END && s->loop && s->seekable && fresh &&
            lseek(s->fd, s->data_start, SEEK_SET) >= 0) {
            fresh = false;
            r = src_read_frame(s, &f);
        }
        qemu_mutex_lock(&s->lock);
        if (s->seek_pending || s->stop) {
            sgi_video_frame_free(f);
        } else if (r == SRC_OK) {
            fresh = true;
            s->starved = false;
            s->queue[(s->head + s->count++) % SRC_QUEUE] = f;
        } else if (r == SRC_END || r == SRC_ERR) {
            /* Either way the file has nothing more to give. */
            s->eof = true;
        }
        qemu_cond_broadcast(&s->wake);
    }
    qemu_mutex_unlock(&s->lock);
    return NULL;
}

static bool src_signal(Object *obj, SGIVideoFormat *fmt)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(obj);

    *fmt = s->fmt;
    return !s->ended;
}

/*
 * Realtime repeats the last frame when the reader is behind and counts it.
 * Under icount, or lockstep, the board waits for the frame, for a bounded
 * time. With the file's last frame gone and no loop the signal is lost.
 */
static const SGIVideoFrame *src_next_frame(Object *obj)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(obj);
    const SGIVideoFrame *ret = NULL;
    int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + SRC_WAIT_MS;

    qemu_mutex_lock(&s->lock);
    while (!s->count && !s->eof && !s->starved &&
           (s->lockstep || icount_enabled())) {
        int64_t left = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (left <= 0 || !qemu_cond_timedwait(&s->wake, &s->lock, left)) {
            qemu_log_mask(LOG_GUEST_ERROR, TYPE_SGI_VIDEO_FILE_SOURCE
                          ": no frame from %s for %d s, repeating\n",
                          s->path, SRC_WAIT_MS / 1000);
            s->starved = true;
        }
    }
    if (s->count) {
        sgi_video_frame_free(s->cur);
        s->cur = s->queue[s->head];
        s->head = (s->head + 1) % SRC_QUEUE;
        s->count--;
        s->delivered++;
        qemu_cond_broadcast(&s->wake);
        ret = s->cur;
    } else if (s->eof) {
        s->ended = true;
    } else if (s->cur) {
        s->repeated++;
        ret = s->cur;
    }
    qemu_mutex_unlock(&s->lock);
    return ret;
}

static uint64_t src_position(Object *obj)
{
    return SGI_VIDEO_FILE_SOURCE(obj)->delivered;
}

static void src_seek(Object *obj, uint64_t position)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(obj);

    qemu_mutex_lock(&s->lock);
    s->seek_to = position;
    s->seek_pending = true;
    s->delivered = position;
    s->ended = false;
    qemu_cond_broadcast(&s->wake);
    while (s->seek_pending) {
        qemu_cond_wait(&s->wake, &s->lock);
    }
    qemu_mutex_unlock(&s->lock);
}

static bool src_header(SGIVideoFileSource *s, Error **errp)
{
    if (!strcmp(s->format, "y4m")) {
        char line[1024];
        size_t got;
        unsigned i = 0;

        while (i < sizeof(line) - 1 &&
               src_read(s, line + i, 1, &got) == SRC_OK && line[i] != '\n') {
            i++;
        }
        line[i] = 0;
        s->y4m = true;
        s->data_start = i + 1;
        return sgi_y4m_parse_header(line, &s->fmt, errp);
    }
    if (strcmp(s->format, "raw")) {
        error_setg(errp, "format must be y4m or raw");
        return false;
    }
    s->fmt.width = s->width;
    s->fmt.height = s->height;
    s->fmt.depth = s->depth;
    s->fmt.fields = !strcmp(s->fields, "tff") ? SGI_VIDEO_TFF :
                    !strcmp(s->fields, "bff") ? SGI_VIDEO_BFF :
                    SGI_VIDEO_PROGRESSIVE;
    if (sscanf(s->rate, "%u/%u", &s->fmt.rate_num, &s->fmt.rate_den) != 2 ||
        !sgi_video_format_valid(&s->fmt)) {
        error_setg(errp, "raw video needs width, height, depth 8 or 10, an "
                   "even width and rate=num/den, within 4096 by 2304");
        return false;
    }
    return true;
}

static void src_realize(DeviceState *dev, Error **errp)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(dev);
    struct stat st;

    if (!s->path) {
        error_setg(errp, "sgi-video-file-source needs a path");
        return;
    }
    s->fd = qemu_open(s->path, O_RDONLY | O_BINARY, errp);
    if (s->fd < 0) {
        return;
    }
    if (!src_header(s, errp)) {
        goto fail;
    }
    if (s->timecode) {
        s->tc_start = sgi_tc_parse(s->timecode, &s->fmt, &s->drop_frame);
        if (s->tc_start < 0) {
            error_setg(errp, "timecode '%s' is not valid for this rate",
                       s->timecode);
            goto fail;
        }
    }
    s->stride = sgi_video_frame_bytes(&s->fmt) + (s->y4m ? 6 : 0);
    s->seekable = fstat(s->fd, &st) == 0 && S_ISREG(st.st_mode);
    if (s->seekable) {
        s->total = st.st_size > s->data_start ?
                   (st.st_size - s->data_start) / s->stride : 0;
    }
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->wake);
    qemu_thread_create(&s->thread, "sgi-video-src", src_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->started = true;
    return;
fail:
    close(s->fd);
    s->fd = -1;
}

static void src_unrealize(DeviceState *dev)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(dev);

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
    src_flush(s);
    sgi_video_frame_free(s->cur);
    s->cur = NULL;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static char *src_get_timecode_now(Object *obj, Error **errp)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(obj);
    char tc[12];

    sgi_tc_format(s->tc_start + (s->delivered ? s->delivered - 1 : 0),
                  &s->fmt, s->drop_frame, tc);
    return g_strdup(tc);
}

static const Property src_properties[] = {
    DEFINE_PROP_STRING("path", SGIVideoFileSource, path),
    DEFINE_PROP_STRING("format", SGIVideoFileSource, format),
    DEFINE_PROP_UINT32("width", SGIVideoFileSource, width, 0),
    DEFINE_PROP_UINT32("height", SGIVideoFileSource, height, 0),
    DEFINE_PROP_UINT32("depth", SGIVideoFileSource, depth, 10),
    DEFINE_PROP_STRING("rate", SGIVideoFileSource, rate),
    DEFINE_PROP_STRING("fields", SGIVideoFileSource, fields),
    DEFINE_PROP_BOOL("loop", SGIVideoFileSource, loop, false),
    DEFINE_PROP_BOOL("lockstep", SGIVideoFileSource, lockstep, false),
    DEFINE_PROP_STRING("timecode", SGIVideoFileSource, timecode),
};

static void src_init(Object *obj)
{
    SGIVideoFileSource *s = SGI_VIDEO_FILE_SOURCE(obj);

    s->fd = -1;
    s->format = g_strdup("y4m");
    s->rate = g_strdup("30000/1001");
    s->fields = g_strdup("progressive");
    object_property_add_uint64_ptr(obj, "frames", &s->delivered,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "repeated", &s->repeated,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "queued", &s->count,
                                   OBJ_PROP_FLAG_READ);
}

static void src_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIVideoSourceClass *sc = SGI_VIDEO_SOURCE_CLASS(oc);

    dc->realize = src_realize;
    dc->unrealize = src_unrealize;
    dc->user_creatable = true;
    dc->desc = "SGI video file source (Y4M or raw)";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, src_properties);
    object_class_property_add_str(oc, "timecode-now", src_get_timecode_now,
                                  NULL);
    sc->has_signal = src_signal;
    sc->next_frame = src_next_frame;
    sc->position = src_position;
    sc->seek = src_seek;
}

static const TypeInfo video_file_types[] = {
    {
        .name = TYPE_SGI_VIDEO_SINK,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIVideoSinkClass),
    },
    {
        .name = TYPE_SGI_VIDEO_SOURCE,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIVideoSourceClass),
    },
    {
        .name = TYPE_SGI_VIDEO_FILE_SOURCE,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIVideoFileSource),
        .instance_init = src_init,
        .class_init = src_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_SGI_VIDEO_SOURCE },
            { }
        },
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
