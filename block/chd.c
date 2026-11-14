/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * CHD (MAME "compressed hunks of data") hard-disk images.
 *
 * The container is read by MAME's own CHD code (subprojects/libchdman), so
 * every codec MAME writes reads here the way it does in MAME and chdman. A
 * hunk is decoded when a request first touches it and kept in a small cache.
 *
 * MAME's code is synchronous: it calls back for each read and waits for the
 * answer. Everything it does here runs in the calling coroutine, which simply
 * yields inside the callback while the block layer fetches the bytes. One
 * CoMutex keeps a second request out while the first is parked there.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu-chd-shim.h"

#define CHD_MAGIC "MComprHD"
#define CHD_V5_HEADER_BYTES 124
#define CHD_DEFAULT_CACHE (32 * MiB)

/* The bytes of the CHD file, as MAME sees them. */
typedef struct ChdIO {
    BdrvChild *child;
    int64_t length;
} ChdIO;

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
    ret = bdrv_pread(io->child, offset, len, buf, 0);
    if (ret < 0) {
        return ret;
    }
    *actual = len;
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
    .length = chd_io_length,
};

typedef struct BDRVChdState {
    CoMutex lock;

    ChdShimFile *chd;
    ChdIO io;

    uint32_t hunk_bytes;
    uint32_t hunk_count;

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

static int chd_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVChdState *s = bs->opaque;
    QemuOpts *opts;
    ChdShimInfo info;
    uint32_t cyls, heads, secs, bps;
    uint64_t cache_bytes;
    int64_t file_len;
    int ret, err;

    GLOBAL_STATE_CODE();

    opts = qemu_opts_create(&chd_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto out;
    }
    cache_bytes = qemu_opt_get_size(opts, "cache-size", CHD_DEFAULT_CACHE);

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        goto out;
    }
    ret = bdrv_apply_auto_read_only(bs, "CHD images can only be read", errp);
    if (ret < 0) {
        goto out;
    }

    qemu_co_mutex_init(&s->lock);

    file_len = bdrv_getlength(bs->file->bs);
    if (file_len < 0) {
        ret = file_len;
        error_setg_errno(errp, -ret, "Could not get the size of the CHD");
        goto out;
    }
    ret = chd_check_header(bs->file, file_len, errp);
    if (ret < 0) {
        goto out;
    }
    s->io.child = bs->file;
    s->io.length = file_len;

    err = chd_shim_open(&s->chd, &chd_io_ops, &s->io, false, NULL);
    if (err) {
        error_setg(errp, "Not a usable CHD: %s", chd_shim_strerror(err));
        ret = -EINVAL;
        goto out;
    }
    chd_shim_get_info(s->chd, &info);
    if (!info.is_hd || chd_shim_hd_geometry(s->chd, &cyls, &heads, &secs,
                                            &bps) || bps != BDRV_SECTOR_SIZE) {
        error_setg(errp, "CHD is not a 512-byte-sector hard disk image");
        ret = -ENOTSUP;
        goto fail;
    }
    if (info.has_parent) {
        error_setg(errp, "CHD is a diff and needs its parent");
        ret = -ENOTSUP;
        goto fail;
    }

    s->hunk_bytes = info.hunk_bytes;
    s->hunk_count = info.hunk_count;
    bs->total_sectors = info.logical_bytes / BDRV_SECTOR_SIZE;

    cache_bytes = MAX(cache_bytes, s->hunk_bytes);
    s->cache_slots = MIN(cache_bytes / s->hunk_bytes, s->hunk_count);
    s->cache_slots = MAX(s->cache_slots, 1);
    s->cache_tag = g_try_new0(uint32_t, s->cache_slots);
    s->cache_data = g_try_malloc((size_t)s->cache_slots * s->hunk_bytes);
    if (!s->cache_tag || !s->cache_data) {
        error_setg(errp, "Could not allocate the hunk cache");
        g_free(s->cache_tag);
        g_free(s->cache_data);
        ret = -ENOMEM;
        goto fail;
    }

    ret = 0;
    goto out;

fail:
    chd_shim_close(s->chd);
    s->chd = NULL;
out:
    qemu_opts_del(opts);
    return ret;
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

    while (done < bytes) {
        uint64_t pos = offset + done;
        uint32_t hunk = pos / s->hunk_bytes;
        uint32_t in_hunk = pos % s->hunk_bytes;
        uint32_t n = MIN(s->hunk_bytes - in_hunk, bytes - done);
        uint32_t slot = hunk % s->cache_slots;
        uint8_t *data = s->cache_data + (size_t)slot * s->hunk_bytes;

        if (s->cache_tag[slot] != hunk + 1) {
            int err;

            qemu_co_mutex_lock(&s->lock);
            if (s->cache_tag[slot] != hunk + 1) {
                /* the slot's old hunk is gone from here on, even on failure */
                s->cache_tag[slot] = 0;
                err = chd_shim_read_hunk(s->chd, hunk, data);
                if (err) {
                    error_report("chd: reading hunk %u: %s", hunk,
                                 chd_shim_strerror(err));
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

static void chd_close(BlockDriverState *bs)
{
    BDRVChdState *s = bs->opaque;

    chd_shim_close(s->chd);
    g_free(s->cache_data);
    g_free(s->cache_tag);
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
    .bdrv_child_perm        = bdrv_default_perms,
    .bdrv_refresh_limits    = chd_refresh_limits,
    .bdrv_co_preadv         = chd_co_preadv,
    .bdrv_co_get_info       = chd_co_get_info,
    .is_format              = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
