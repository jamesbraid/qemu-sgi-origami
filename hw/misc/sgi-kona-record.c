/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality/KONA stream recorder.
 *
 * See include/hw/misc/sgi-kona-record.h for the records and
 * docs/system/mips/sgi/hardware/kona.rst for what each one means.
 *
 * The pipe reaches this from three places: the processors' loads and
 * stores (under the head's access lock), its work thread (DMA) and the
 * main loop (reset, exit). One lock keeps each record whole and the file
 * in the order the records were made.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "system/system.h"
#include "hw/misc/sgi-kona-record.h"

/* Flush this often, so a recording from a killed process still reads. */
#define SGI_KONA_REC_FLUSH_EVERY 4096

struct SGIKonaRecorder {
    QemuMutex lock;
    FILE *file;
    bool failed;
    Notifier exit;
    bool exit_added;
    uint64_t records;
    /* A load equal to the one before it, with nothing between, is dropped. */
    bool have_last_load;
    uint8_t last_load_size;
    uint32_t last_load_off;
    uint64_t last_load_val;
    uint64_t pixel_offset;
    uint32_t pixel_pitch;
    uint32_t pixel_row;
};

/*
 * Under the lock. The first failed write is reported and the recording
 * stops there, so a damaged file reads as cut off: it gets no end record.
 */
static void rec_put(SGIKonaRecorder *r, const void *buf, size_t len)
{
    if (r->file && !r->failed && fwrite(buf, 1, len, r->file) != len) {
        r->failed = true;
        error_report("sgi-kona: record file write failed: %s; recording"
                     " stopped", strerror(errno));
    }
}

static void rec_put8(SGIKonaRecorder *r, uint8_t v)
{
    rec_put(r, &v, 1);
}

static void rec_put32(SGIKonaRecorder *r, uint32_t v)
{
    uint8_t b[4];

    stl_le_p(b, v);
    rec_put(r, b, sizeof(b));
}

static void rec_put64(SGIKonaRecorder *r, uint64_t v)
{
    uint8_t b[8];

    stq_le_p(b, v);
    rec_put(r, b, sizeof(b));
}

/* Under the lock. Buffered write errors show here and in ferror(). */
static void rec_flush(SGIKonaRecorder *r)
{
    if (r->file && !r->failed && (fflush(r->file) || ferror(r->file))) {
        r->failed = true;
        error_report("sgi-kona: record file write failed: %s; recording"
                     " stopped", strerror(errno));
    }
}

/* Under the lock, once per record after its last byte. */
static void rec_done(SGIKonaRecorder *r)
{
    r->records++;
    if (r->records % SGI_KONA_REC_FLUSH_EVERY == 0) {
        rec_flush(r);
    }
}

static void rec_access(SGIKonaRecorder *r, uint8_t tag, unsigned size,
                       uint32_t off, uint64_t val)
{
    rec_put8(r, tag);
    rec_put8(r, size);
    rec_put32(r, off);
    rec_put64(r, val);
    rec_done(r);
}

static void rec_exit(Notifier *n, void *data)
{
    SGIKonaRecorder *r = container_of(n, SGIKonaRecorder, exit);

    r->exit_added = false;
    sgi_kona_record_finish(r);
}

SGIKonaRecorder *sgi_kona_record_open(const char *path, uint32_t part,
                                      uint32_t revision, uint32_t rm_count,
                                      uint32_t texture_mb,
                                      const char *product, Error **errp)
{
    SGIKonaRecorder *r = g_new0(SGIKonaRecorder, 1);
    size_t n = product ? MIN(strlen(product), 255) : 0;
    static const char magic[8] = SGI_KONA_REC_MAGIC;

    r->file = fopen(path, "wb");
    if (!r->file) {
        error_setg_errno(errp, errno, "sgi-kona: cannot create record file"
                         " '%s'", path);
        g_free(r);
        return NULL;
    }
    qemu_mutex_init(&r->lock);
    rec_put(r, magic, sizeof(magic));
    rec_put32(r, SGI_KONA_REC_VERSION);
    rec_put8(r, SGI_KONA_REC_CONFIG);
    rec_put32(r, part);
    rec_put32(r, revision);
    rec_put32(r, rm_count);
    rec_put32(r, texture_mb);
    rec_put8(r, n);
    rec_put(r, product, n);
    rec_done(r);
    rec_flush(r);
    if (r->failed) {
        fclose(r->file);
        qemu_mutex_destroy(&r->lock);
        error_setg(errp, "sgi-kona: cannot write record file '%s'", path);
        g_free(r);
        return NULL;
    }

    r->exit.notify = rec_exit;
    qemu_add_exit_notifier(&r->exit);
    r->exit_added = true;
    return r;
}

void sgi_kona_record_finish(SGIKonaRecorder *r)
{
    QEMU_LOCK_GUARD(&r->lock);

    if (!r->file) {
        return;
    }
    if (!r->failed) {
        rec_put8(r, SGI_KONA_REC_END);
        rec_put64(r, r->records);
    }
    rec_flush(r);
    if (fclose(r->file) && !r->failed) {
        error_report("sgi-kona: record file write failed: %s",
                     strerror(errno));
    }
    r->file = NULL;
}

void sgi_kona_record_free(SGIKonaRecorder *r)
{
    if (!r) {
        return;
    }
    sgi_kona_record_finish(r);
    if (r->exit_added) {
        notifier_remove(&r->exit);
    }
    qemu_mutex_destroy(&r->lock);
    g_free(r);
}

void sgi_kona_record_store(SGIKonaRecorder *r, unsigned size, uint32_t off,
                           uint64_t val)
{
    QEMU_LOCK_GUARD(&r->lock);

    if (r->file) {
        r->have_last_load = false;
        rec_access(r, SGI_KONA_REC_STORE, size, off, val);
    }
}

void sgi_kona_record_load(SGIKonaRecorder *r, unsigned size, uint32_t off,
                          uint64_t val)
{
    QEMU_LOCK_GUARD(&r->lock);

    if (!r->file) {
        return;
    }
    if (r->have_last_load && r->last_load_size == size &&
        r->last_load_off == off && r->last_load_val == val) {
        return;
    }
    r->have_last_load = true;
    r->last_load_size = size;
    r->last_load_off = off;
    r->last_load_val = val;
    rec_access(r, SGI_KONA_REC_LOAD, size, off, val);
}

static void rec_dma(SGIKonaRecorder *r, uint8_t tag, unsigned src,
                    uint64_t addr, const void *buf, uint32_t len)
{
    if (!r->file) {
        return;
    }
    r->have_last_load = false;
    rec_put8(r, tag);
    rec_put8(r, src);
    rec_put64(r, addr);
    rec_put32(r, len);
    rec_put(r, buf, len);
    rec_done(r);
}

void sgi_kona_record_dma_read(SGIKonaRecorder *r, unsigned src,
                              uint64_t addr, const void *buf, uint32_t len)
{
    QEMU_LOCK_GUARD(&r->lock);

    rec_dma(r, SGI_KONA_REC_DMA_READ, src, addr, buf, len);
}

void sgi_kona_record_dma_write(SGIKonaRecorder *r, unsigned src,
                               uint64_t addr, const void *buf, uint32_t len)
{
    QEMU_LOCK_GUARD(&r->lock);

    rec_dma(r, SGI_KONA_REC_DMA_WRITE, src, addr, buf, len);
}

void sgi_kona_record_interrupt(SGIKonaRecorder *r, unsigned kind,
                               uint8_t vector, bool delivered)
{
    QEMU_LOCK_GUARD(&r->lock);

    if (!r->file) {
        return;
    }
    r->have_last_load = false;
    rec_put8(r, SGI_KONA_REC_INTERRUPT);
    rec_put8(r, kind);
    rec_put8(r, vector);
    rec_put8(r, delivered);
    rec_done(r);
}

void sgi_kona_record_mark(SGIKonaRecorder *r, uint32_t kind, uint32_t arg)
{
    QEMU_LOCK_GUARD(&r->lock);

    if (!r->file) {
        return;
    }
    r->have_last_load = false;
    rec_put8(r, SGI_KONA_REC_MARK);
    rec_put32(r, kind);
    rec_put32(r, arg);
    rec_done(r);
    rec_flush(r);
}

void sgi_kona_record_pixels_begin(SGIKonaRecorder *r, uint64_t offset,
                                  uint32_t pitch)
{
    QEMU_LOCK_GUARD(&r->lock);

    r->pixel_offset = offset;
    r->pixel_pitch = pitch;
    r->pixel_row = 0;
}

void sgi_kona_record_pixels_row(SGIKonaRecorder *r, bool write,
                                const void *buf, uint32_t len)
{
    QEMU_LOCK_GUARD(&r->lock);
    uint64_t addr = r->pixel_offset +
                    (uint64_t)r->pixel_row++ * r->pixel_pitch;

    rec_dma(r, write ? SGI_KONA_REC_DMA_WRITE : SGI_KONA_REC_DMA_READ,
            SGI_KONA_REC_SRC_PIXELS, addr, buf, len);
}
