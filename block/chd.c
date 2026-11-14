/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * CHD (MAME "compressed hunks of data") hard-disk images.
 *
 * The container is read and written by MAME's own CHD code
 * (subprojects/libchdman), so this driver does what the iris emulator does
 * with the same files:
 *
 *  - an uncompressed CHD is written in place;
 *  - a compressed one is never written: guest writes go to an uncompressed
 *    "<image>.diff.chd" beside it, and a diff that is already there is
 *    reattached;
 *  - "overlay" sends an uncompressed image's writes to a diff as well;
 *  - a hunk's map entry reaches the disk only after the hunk's data has been
 *    synced, so a crash never leaves a map entry pointing at data that is not
 *    there.
 *
 * MAME's code is synchronous: it calls back for each read and write and waits
 * for the answer. Everything it does here runs in the calling coroutine, which
 * simply yields inside the callback while the block layer fetches the bytes.
 * One CoMutex keeps a second request out while the first is parked there.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/module.h"
#include "qemu/memalign.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "system/block-backend.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qobject/qdict.h"
#include "qemu-chd-shim.h"

#define CHD_MAGIC "MComprHD"
#define CHD_V5_HEADER_BYTES 124
#define CHD_DEFAULT_CACHE (32 * MiB)
#define CHD_DIFF_SUFFIX ".diff.chd"

/* ------------------------------------------------------------------ */
/* The bytes of one CHD file, as MAME sees them. */

typedef struct ChdHeldWrite {
    uint64_t offset;
    size_t len;
    uint8_t *data;
} ChdHeldWrite;

typedef struct ChdIO {
    BdrvChild *child;       /* the node's file or diff child, or ... */
    BlockBackend *blk;      /* ... the temporary file a diff is made in */
    int64_t length;
    /*
     * The map of an uncompressed version 5 file we may write: MAME appends a
     * new hunk and then writes its 4-byte map entry. Those writes wait in
     * "held" until chd_io_sync() has put the data on the disk.
     */
    uint64_t map_start, map_end;
    GArray *held;
} ChdIO;

static bool chd_io_overlaps_map(ChdIO *io, uint64_t offset, size_t len)
{
    return offset < io->map_end && offset + len > io->map_start;
}

static int coroutine_mixed_fn TSA_NO_TSA
chd_io_pread(ChdIO *io, uint64_t offset, size_t len, void *buf)
{
    return io->blk ? blk_pread(io->blk, offset, len, buf, 0)
                   : bdrv_pread(io->child, offset, len, buf, 0);
}

static int coroutine_mixed_fn TSA_NO_TSA
chd_io_pwrite(ChdIO *io, uint64_t offset, size_t len, const void *buf)
{
    if (io->blk) {
        /* a BlockBackend, unlike a node, will not write past the end */
        if (offset + len > io->length) {
            int ret = blk_truncate(io->blk, offset + len, false,
                                   PREALLOC_MODE_OFF, 0, NULL);

            if (ret < 0) {
                return ret;
            }
        }
        return blk_pwrite(io->blk, offset, len, buf, 0);
    }
    return bdrv_pwrite(io->child, offset, len, buf, 0);
}

static int coroutine_mixed_fn TSA_NO_TSA chd_io_flush(ChdIO *io)
{
    return io->blk ? blk_flush(io->blk) : bdrv_flush(io->child->bs);
}

/* These run in the middle of MAME's code, in or out of a coroutine. */
static int coroutine_mixed_fn TSA_NO_TSA
chd_io_read_at(void *opaque, uint64_t offset, void *buf, size_t len,
               size_t *actual)
{
    ChdIO *io = opaque;
    int ret;

    if (offset >= io->length) {
        *actual = 0;
        return 0;
    }
    len = MIN(len, io->length - offset);
    ret = chd_io_pread(io, offset, len, buf);
    if (ret < 0) {
        return ret;
    }
    *actual = len;

    if (io->held) {
        for (guint i = 0; i < io->held->len; i++) {
            ChdHeldWrite *w = &g_array_index(io->held, ChdHeldWrite, i);
            uint64_t lo = MAX(offset, w->offset);
            uint64_t hi = MIN(offset + len, w->offset + w->len);

            if (lo < hi) {
                memcpy((uint8_t *)buf + (lo - offset),
                       w->data + (lo - w->offset), hi - lo);
            }
        }
    }
    return 0;
}

static int coroutine_mixed_fn TSA_NO_TSA
chd_io_write_at(void *opaque, uint64_t offset, const void *buf, size_t len)
{
    ChdIO *io = opaque;

    if (io->held && chd_io_overlaps_map(io, offset, len)) {
        ChdHeldWrite w = {
            .offset = offset, .len = len, .data = g_memdup2(buf, len),
        };

        g_array_append_val(io->held, w);
        io->length = MAX(io->length, offset + len);
        return 0;
    }
    if (chd_io_pwrite(io, offset, len, buf) < 0) {
        return -EIO;
    }
    io->length = MAX(io->length, offset + len);
    return 0;
}

static int chd_io_length(void *opaque, uint64_t *len)
{
    ChdIO *io = opaque;

    *len = io->length;
    return 0;
}

static const ChdShimIO chd_io_ops = {
    .read_at = chd_io_read_at,
    .write_at = chd_io_write_at,
    .length = chd_io_length,
    .release = NULL,    /* the owner frees the ChdIO, after a last sync */
};

/*
 * Make every write so far durable: the data, then the map entries that point
 * at it. A crash between the two leaves an unreferenced hunk at the end of
 * the file, which nothing reads.
 */
static int coroutine_mixed_fn TSA_NO_TSA chd_io_sync(ChdIO *io)
{
    int ret = chd_io_flush(io);

    if (ret < 0 || !io->held || io->held->len == 0) {
        return ret;
    }
    for (guint i = 0; i < io->held->len; i++) {
        ChdHeldWrite *w = &g_array_index(io->held, ChdHeldWrite, i);

        ret = chd_io_pwrite(io, w->offset, w->len, w->data);
        if (ret < 0) {
            return ret;
        }
    }
    ret = chd_io_flush(io);
    if (ret < 0) {
        return ret;
    }
    for (guint i = 0; i < io->held->len; i++) {
        g_free(g_array_index(io->held, ChdHeldWrite, i).data);
    }
    g_array_set_size(io->held, 0);
    return 0;
}

static ChdIO *chd_io_new(BdrvChild *child, int64_t length, bool writable_v5)
{
    ChdIO *io = g_new0(ChdIO, 1);

    io->child = child;
    io->length = length;
    if (writable_v5) {
        uint8_t h[CHD_V5_HEADER_BYTES];
        uint64_t logical, map;
        uint32_t hunk;

        /* the caller has already checked this is an uncompressed v5 file */
        if (bdrv_pread(child, 0, sizeof(h), h, 0) < 0) {
            g_free(io);
            return NULL;
        }
        logical = ldq_be_p(h + 32);
        map = ldq_be_p(h + 40);
        hunk = ldl_be_p(h + 56);
        if (hunk) {
            io->map_start = map;
            io->map_end = map + DIV_ROUND_UP(logical, hunk) * 4;
        }
        io->held = g_array_new(false, true, sizeof(ChdHeldWrite));
    }
    return io;
}

static void chd_io_free(ChdIO *io)
{
    if (!io) {
        return;
    }
    if (io->held) {
        for (guint i = 0; i < io->held->len; i++) {
            g_free(g_array_index(io->held, ChdHeldWrite, i).data);
        }
        g_array_free(io->held, true);
    }
    g_free(io);
}

/* ------------------------------------------------------------------ */

typedef struct BDRVChdState {
    CoMutex lock;

    ChdShimFile *base;      /* the image; read-only when a diff is in use */
    ChdShimFile *top;       /* what guest I/O reaches: the diff, or the image */
    ChdIO *base_io;
    ChdIO *top_io;          /* top's file; NULL if top is the read-only image */
    BdrvChild *diff;        /* the diff file, or NULL */

    bool overlay;           /* option: diff an uncompressed image too */
    char *base_path;        /* absolute */
    char *diff_path;

    uint32_t hunk_bytes;
    uint32_t hunk_count;
    uint64_t logical_bytes;
    bool top_has_parent;
    bool writable;          /* guest writes are allowed */

    /* decoded hunks, direct-mapped on the hunk number */
    uint32_t cache_slots;
    uint32_t *cache_tag;    /* hunk + 1, 0 for empty */
    uint8_t *cache_data;
} BDRVChdState;

static QemuOptsList chd_runtime_opts = {
    .name = "chd",
    .head = QTAILQ_HEAD_INITIALIZER(chd_runtime_opts.head),
    .desc = {
        {
            .name = "overlay",
            .type = QEMU_OPT_BOOL,
            .help = "Keep guest writes in the .diff.chd file, also for an "
                    "uncompressed image",
        },
        {
            .name = "cache-size",
            .type = QEMU_OPT_SIZE,
            .help = "Bytes of decoded hunks to keep (default 32 MiB)",
        },
        { /* end of list */ }
    },
};

static int chd_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    if (buf_size >= 8 && !memcmp(buf, CHD_MAGIC, 8)) {
        return 100;
    }
    return 0;
}

/* ---- creating a diff ---- */

/* Make a rename in the directory of path durable; NTFS journals it itself. */
static void chd_sync_dir(const char *path)
{
#ifndef _WIN32
    g_autofree char *dir = g_path_get_dirname(path);
    int dfd = qemu_open_old(dir, O_RDONLY);

    if (dfd >= 0) {
        fsync(dfd);
        qemu_close(dfd);
    }
#endif
}

/*
 * Build an empty diff under another name and rename it into place once it is
 * on the disk, so that a crash never leaves a half-made diff where the next
 * start would reattach it.
 */
static int chd_create_diff_file(ChdShimFile *base, const char *diff_path,
                                Error **errp)
{
    g_autofree char *tmp = g_strconcat(diff_path, ".new", NULL);
    QDict *opts = qdict_new();
    BlockBackend *blk;
    ChdShimFile *diff = NULL;
    ChdIO *io;
    int fd, ret;

    fd = qemu_create(tmp, O_RDWR | O_BINARY | O_TRUNC, 0644, errp);
    if (fd < 0) {
        qobject_unref(opts);
        return -errno;
    }
    qemu_close(fd);

    qdict_put_str(opts, "driver", "file");
    blk = blk_new_open(tmp, NULL, opts, BDRV_O_RDWR | BDRV_O_RESIZE, errp);
    if (!blk) {
        qemu_unlink(tmp);
        return -EIO;
    }

    io = g_new0(ChdIO, 1);
    io->blk = blk;
    ret = chd_shim_create_diff(&diff, &chd_io_ops, io, base);
    if (ret) {
        error_setg(errp, "Could not create %s: %s", diff_path,
                   chd_shim_strerror(ret));
        ret = -EIO;
    } else {
        chd_shim_close(diff);   /* writes the header and map out */
        ret = blk_flush(blk);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Could not sync %s", tmp);
        }
    }
    chd_io_free(io);
    blk_unref(blk);

    if (ret == 0 && g_rename(tmp, diff_path) < 0) {
        error_setg_errno(errp, errno, "Could not move %s into place", tmp);
        ret = -errno;
    }
    if (ret < 0) {
        qemu_unlink(tmp);
    } else {
        /* guest writes are acknowledged after this, so the diff must last */
        chd_sync_dir(diff_path);
    }
    return ret;
}

/* ------------------------------------------------------------------ */

static void chd_cache_invalidate(BDRVChdState *s, uint32_t first, uint32_t last)
{
    for (uint32_t h = first; h <= last; h++) {
        uint32_t slot = h % s->cache_slots;

        if (s->cache_tag[slot] == h + 1) {
            s->cache_tag[slot] = 0;
        }
    }
}

/*
 * MAME trusts a version 5 header: a unit size of zero divides by zero in its
 * open, and an absurd size aborts in a bounds check. Refuse what is plainly
 * wrong here, so a damaged file is an error and not a crash. This does not
 * harden MAME's parser: do not open CHDs from untrusted sources.
 */
static int chd_check_header(BdrvChild *file, int64_t file_len, Error **errp)
{
    uint8_t h[CHD_V5_HEADER_BYTES];
    uint64_t logical, map, hunks;
    uint32_t hunk, unit;
    bool compressed = false;
    int ret;

    if (file_len < CHD_V5_HEADER_BYTES) {
        return 0;       /* not a v5 file; MAME says what is wrong with it */
    }
    ret = bdrv_pread(file, 0, sizeof(h), h, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "Could not read the CHD header");
        return ret;
    }
    if (memcmp(h, CHD_MAGIC, 8) || ldl_be_p(h + 8) != CHD_V5_HEADER_BYTES ||
        ldl_be_p(h + 12) != 5) {
        return 0;
    }
    for (int i = 16; i < 32; i++) {
        compressed |= h[i] != 0;
    }
    logical = ldq_be_p(h + 32);
    map = ldq_be_p(h + 40);
    hunk = ldl_be_p(h + 56);
    unit = ldl_be_p(h + 60);
    if (!hunk || !unit || hunk % unit) {
        error_setg(errp, "CHD header is damaged");
        return -EINVAL;
    }
    /* not DIV_ROUND_UP: a damaged size can wrap it */
    hunks = logical / hunk + (logical % hunk != 0);
    if (hunks > UINT32_MAX || map >= file_len ||
        (!compressed && hunks * 4 > file_len - map)) {
        error_setg(errp, "CHD header is damaged");
        return -EINVAL;
    }
    return 0;
}

/*
 * The path the diff and the fold work beside, or NULL when the file child is
 * not a plain file. Symlinks are resolved: a fold renames over the path, and
 * renaming over a link would replace the link and leave the image stale.
 */
static char *chd_base_path(BlockDriverState *file_bs)
{
    if (strcmp(file_bs->drv->format_name, "file")) {
        return NULL;
    }
#ifndef _WIN32
    {
        char *real = realpath(file_bs->filename, NULL);

        if (real) {
            return real;
        }
    }
#endif
    return g_canonicalize_filename(file_bs->filename, NULL);
}

static int chd_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVChdState *s = bs->opaque;
    QemuOpts *opts;
    ChdShimInfo info;
    uint32_t cyls, heads, secs, bps;
    int64_t file_len;
    uint64_t cache_bytes;
    bool want_write, diff_exists, use_diff;
    int ret, err;

    GLOBAL_STATE_CODE();

    opts = qemu_opts_create(&chd_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto out_opts;
    }
    s->overlay = qemu_opt_get_bool(opts, "overlay", false);
    cache_bytes = qemu_opt_get_size(opts, "cache-size", CHD_DEFAULT_CACHE);

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        goto out_opts;
    }
    want_write = bs->open_flags & BDRV_O_RDWR;
    bs->supported_write_flags = BDRV_REQ_FUA;

    qemu_co_mutex_init(&s->lock);

    file_len = bdrv_getlength(bs->file->bs);
    if (file_len < 0) {
        ret = file_len;
        error_setg_errno(errp, -ret, "Could not get the size of the CHD");
        goto out_opts;
    }
    ret = chd_check_header(bs->file, file_len, errp);
    if (ret < 0) {
        goto out_opts;
    }
    s->base_path = chd_base_path(bs->file->bs);
    s->diff_path = s->base_path ?
        g_strconcat(s->base_path, CHD_DIFF_SUFFIX, NULL) : NULL;

    s->base_io = chd_io_new(bs->file, file_len, false);
    err = chd_shim_open(&s->base, &chd_io_ops, s->base_io, false, NULL);
    if (err) {
        error_setg(errp, "Not a usable CHD: %s", chd_shim_strerror(err));
        ret = -EINVAL;
        goto fail;
    }
    chd_shim_get_info(s->base, &info);
    if (!info.is_hd || chd_shim_hd_geometry(s->base, &cyls, &heads, &secs,
                                            &bps) || bps != BDRV_SECTOR_SIZE) {
        error_setg(errp, "CHD is not a 512-byte-sector hard disk image");
        ret = -ENOTSUP;
        goto fail;
    }
    if (info.has_parent) {
        error_setg(errp, "CHD is a diff and needs its parent; open the parent "
                   "and its %s file instead", CHD_DIFF_SUFFIX);
        ret = -ENOTSUP;
        goto fail;
    }

    s->hunk_bytes = info.hunk_bytes;
    s->hunk_count = info.hunk_count;
    s->logical_bytes = info.logical_bytes;
    bs->total_sectors = info.logical_bytes / BDRV_SECTOR_SIZE;
    s->top = s->base;

    /*
     * An existing diff is reattached, even to read it only, or the image
     * would read as it was before the diff's changes. Otherwise an
     * uncompressed image is written in place (unless overlay asks for a diff)
     * and a compressed one gets a fresh diff.
     */
    diff_exists = s->diff_path &&
                  g_file_test(s->diff_path, G_FILE_TEST_EXISTS);
    use_diff = diff_exists || (want_write && (s->overlay || info.version != 5 ||
                                              info.compressed));
    if (use_diff && !s->diff_path) {
        error_setg(errp, "This CHD is not on a plain file, so there is no "
                   "place for its diff");
        ret = -ENOTSUP;
        goto fail;
    }
    s->writable = want_write;

    if (want_write && !use_diff) {
        /* reopen the image for writing, with held map entries */
        chd_shim_close(s->base);
        s->base = s->top = NULL;
        chd_io_free(s->base_io);
        s->base_io = chd_io_new(bs->file, file_len, true);
        err = s->base_io ?
            chd_shim_open(&s->base, &chd_io_ops, s->base_io, true, NULL) : -1;
        if (err) {
            error_setg(errp, "Could not open the CHD for writing%s%s",
                       err > 0 ? ": " : "",
                       err > 0 ? chd_shim_strerror(err) : "");
            ret = -EIO;
            goto fail;
        }
        s->top = s->base;
        s->top_io = s->base_io;
    } else if (use_diff) {
        int64_t diff_len;
        ChdIO *dio;

        if (!diff_exists) {
            /*
             * A diff names its parent by SHA-1. MAME records none in an
             * uncompressed CHD that chdman made, and a diff of such a file
             * cannot be opened again.
             */
            if (buffer_is_zero(info.sha1, sizeof(info.sha1))) {
                error_setg(errp, "CHD records no SHA-1, so a diff cannot name "
                           "it as its parent");
                ret = -ENOTSUP;
                goto fail;
            }
            ret = chd_create_diff_file(s->base, s->diff_path, errp);
            if (ret < 0) {
                goto fail;
            }
        }
        qdict_put_str(options, "diff.driver", "file");
        qdict_put_str(options, "diff.filename", s->diff_path);
        /*
         * The driver reads the diff's header and map as well as its data, so
         * it is an image child: qemu-img info opens without I/O, and only a
         * metadata child is exempt from that.
         */
        s->diff = bdrv_open_child(NULL, options, "diff", bs, &child_of_bds,
                                  BDRV_CHILD_DATA | BDRV_CHILD_METADATA,
                                  false, errp);
        if (!s->diff) {
            ret = -EIO;
            goto fail;
        }
        diff_len = bdrv_getlength(s->diff->bs);
        dio = chd_io_new(s->diff, diff_len, want_write);
        err = dio ? chd_shim_open(&s->top, &chd_io_ops, dio, want_write,
                                  s->base) : -1;
        if (err) {
            error_setg(errp, "Could not open %s%s%s", s->diff_path,
                       err > 0 ? ": " : "",
                       err > 0 ? chd_shim_strerror(err) : "");
            chd_io_free(dio);
            ret = -EIO;
            goto fail;
        }
        s->top_io = dio;
        s->top_has_parent = true;
    }

    cache_bytes = MAX(cache_bytes, s->hunk_bytes);
    s->cache_slots = MIN(cache_bytes / s->hunk_bytes, s->hunk_count);
    s->cache_slots = MAX(s->cache_slots, 1);
    s->cache_tag = g_try_new0(uint32_t, s->cache_slots);
    s->cache_data = g_try_malloc((size_t)s->cache_slots * s->hunk_bytes);
    if (!s->cache_tag || !s->cache_data) {
        error_setg(errp, "Could not allocate the hunk cache");
        ret = -ENOMEM;
        goto fail;
    }

    ret = 0;
    goto out_opts;

fail:
    if (s->top && s->top != s->base) {
        chd_shim_close(s->top);
    }
    if (s->base) {
        chd_shim_close(s->base);
    }
    if (s->top_io && s->top_io != s->base_io) {
        chd_io_free(s->top_io);
    }
    chd_io_free(s->base_io);
    s->base = s->top = NULL;
    s->base_io = s->top_io = NULL;
    g_free(s->base_path);
    g_free(s->diff_path);
    g_free(s->cache_tag);
    g_free(s->cache_data);
    s->base_path = s->diff_path = NULL;
    s->cache_tag = NULL;
    s->cache_data = NULL;
out_opts:
    qemu_opts_del(opts);
    return ret;
}

/*
 * A hunk written for the first time is appended to the file, so a writable
 * file needs permission to grow as well as to change.
 */
static void chd_child_perm(BlockDriverState *bs, BdrvChild *c,
                           BdrvChildRole role, BlockReopenQueue *reopen_queue,
                           uint64_t perm, uint64_t shared,
                           uint64_t *nperm, uint64_t *nshared)
{
    bdrv_default_perms(bs, c, role, reopen_queue, perm, shared, nperm,
                       nshared);
    if (perm & BLK_PERM_WRITE) {
        *nperm |= BLK_PERM_RESIZE;
    }
}

static void chd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    BDRVChdState *s = bs->opaque;

    bs->bl.request_alignment = BDRV_SECTOR_SIZE;
    bs->bl.opt_transfer = s->hunk_bytes;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    int64_t done = 0;
    int ret = 0;

    while (done < bytes) {
        uint64_t pos = offset + done;
        uint32_t hunk = pos / s->hunk_bytes;
        uint32_t in_hunk = pos % s->hunk_bytes;
        uint32_t n = MIN(s->hunk_bytes - in_hunk, bytes - done);
        uint32_t slot = hunk % s->cache_slots;
        uint8_t *data = s->cache_data + (size_t)slot * s->hunk_bytes;

        if (s->cache_tag[slot] != hunk + 1) {
            qemu_co_mutex_lock(&s->lock);
            if (s->cache_tag[slot] != hunk + 1) {
                /* the slot's old hunk is gone from here on, even on failure */
                s->cache_tag[slot] = 0;
                ret = chd_shim_read_hunk(s->top, hunk, data);
                if (ret) {
                    error_report("chd: reading hunk %u: %s", hunk,
                                 chd_shim_strerror(ret));
                    qemu_co_mutex_unlock(&s->lock);
                    return -EIO;
                }
                s->cache_tag[slot] = hunk + 1;
            }
            /* No yield between here and the copy: the slot stays ours. */
            qemu_iovec_from_buf(qiov, done, data + in_hunk, n);
            qemu_co_mutex_unlock(&s->lock);
        } else {
            qemu_iovec_from_buf(qiov, done, data + in_hunk, n);
        }
        done += n;
    }
    return 0;
}

/*
 * MAME's write_hunk stores nothing for an all-zero hunk that the file does not
 * hold yet, because such a hunk reads as zeroes anyway. In a diff it would
 * read from the parent instead, and the zeroes would be lost. An uncompressed
 * version 5 map entry has no value that means zero, so the only way to keep
 * them is to hold the hunk: copy it into the diff first, and let the write
 * overwrite it in place.
 */
static int coroutine_fn
chd_claim_zeroed_hunks(BDRVChdState *s, uint64_t offset, const uint8_t *data,
                       uint64_t bytes)
{
    uint64_t end = offset + bytes;
    uint8_t *buf = g_try_malloc(s->hunk_bytes);
    int ret = 0;

    if (!buf) {
        return -ENOMEM;
    }
    for (uint64_t hunk = offset / s->hunk_bytes;
         hunk < DIV_ROUND_UP(end, s->hunk_bytes); hunk++) {
        uint64_t start = hunk * s->hunk_bytes;
        uint64_t lo = MAX(offset, start) - start;
        uint64_t hi = MIN(end, start + s->hunk_bytes) - start;
        uint32_t codec, comp;

        if (!buffer_is_zero(data + (start + lo - offset), hi - lo)) {
            continue;
        }
        if (chd_shim_hunk_info(s->top, hunk, &codec, &comp)) {
            ret = -EIO;
            break;
        }
        /*
         * A hunk the diff holds has stored bytes; one it leaves to the
         * parent has none.
         */
        if (comp != 0) {
            continue;
        }
        if (chd_shim_read_hunk(s->top, hunk, buf)) {
            ret = -EIO;
            break;
        }
        /*
         * Non-zero bytes the write keeps from the parent make MAME store the
         * hunk itself. It is only when the write leaves the hunk all zero
         * over data that the parent has that the zeroes would be lost.
         */
        if (!buffer_is_zero(buf + lo, hi - lo) && buffer_is_zero(buf, lo) &&
            buffer_is_zero(buf + hi, s->hunk_bytes - hi)) {
            if (chd_shim_write_hunk(s->top, hunk, buf)) {
                ret = -EIO;
                break;
            }
        }
    }
    g_free(buf);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
               QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVChdState *s = bs->opaque;
    uint8_t *buf;
    int ret;

    if (!s->writable) {
        return -EPERM;
    }
    buf = g_try_malloc(bytes);
    if (!buf) {
        return -ENOMEM;
    }
    qemu_iovec_to_buf(qiov, 0, buf, bytes);

    qemu_co_mutex_lock(&s->lock);
    ret = 0;
    if (s->top_has_parent) {
        ret = chd_claim_zeroed_hunks(s, offset, buf, bytes);
    }
    if (ret == 0 && chd_shim_write_bytes(s->top, offset, buf, bytes)) {
        ret = -EIO;
    }
    /* also after a failure, which may have written part of the range */
    chd_cache_invalidate(s, offset / s->hunk_bytes,
                         (offset + bytes - 1) / s->hunk_bytes);
    if (ret == 0) {
        /*
         * A write that allocated a hunk left a map entry waiting for its
         * data: sync now, so that a kill never keeps a later write and loses
         * an earlier one.
         */
        if ((s->top_io->held && s->top_io->held->len) ||
            (flags & BDRV_REQ_FUA)) {
            ret = chd_io_sync(s->top_io);
        }
    }
    qemu_co_mutex_unlock(&s->lock);
    g_free(buf);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK chd_co_flush_to_disk(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;
    int ret = 0;

    if (s->writable) {
        qemu_co_mutex_lock(&s->lock);
        ret = chd_io_sync(s->top_io);
        qemu_co_mutex_unlock(&s->lock);
    }
    return ret;
}

static void chd_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;

    if (s->writable) {
        bdrv_graph_rdlock_main_loop();
        chd_io_sync(s->top_io);
        bdrv_graph_rdunlock_main_loop();
    }
    if (s->top && s->top != s->base) {
        chd_shim_close(s->top);
    }
    if (s->writable) {
        bdrv_graph_rdlock_main_loop();
        chd_io_sync(s->top_io);     /* closing may have written the header */
        bdrv_graph_rdunlock_main_loop();
    }
    chd_shim_close(s->base);
    if (s->writable && s->top_io == s->base_io) {
        bdrv_graph_rdlock_main_loop();
        chd_io_sync(s->base_io);
        bdrv_graph_rdunlock_main_loop();
    }
    if (s->top_io != s->base_io) {
        chd_io_free(s->top_io);
    }
    chd_io_free(s->base_io);
    g_free(s->cache_data);
    g_free(s->cache_tag);
    g_free(s->base_path);
    g_free(s->diff_path);
}

static int coroutine_fn GRAPH_RDLOCK
chd_co_get_info(BlockDriverState *bs, BlockDriverInfo *bdi)
{
    BDRVChdState *s = bs->opaque;

    bdi->cluster_size = s->hunk_bytes;
    return 0;
}

static BlockDriver bdrv_chd = {
    .format_name            = "chd",
    .instance_size          = sizeof(BDRVChdState),
    .bdrv_probe             = chd_probe,
    .bdrv_open              = chd_open,
    .bdrv_close             = chd_close,
    .bdrv_child_perm        = chd_child_perm,
    .bdrv_refresh_limits    = chd_refresh_limits,
    .bdrv_co_preadv         = chd_co_preadv,
    .bdrv_co_pwritev        = chd_co_pwritev,
    .bdrv_co_flush_to_disk  = chd_co_flush_to_disk,
    .bdrv_co_get_info       = chd_co_get_info,
    .is_format              = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
