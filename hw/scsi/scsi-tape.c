/*
 * SCSI sequential-access tape, backed by a SIMH tape image
 *
 * Copyright (c) 2026 Emmanuel Ugwu <emmanuelugwu121@gmail.com>
 *
 * The QOM lifecycle, property set and position state are the public QEMU
 * GSoC 2026 scsi-tape skeleton; the command set and the on-file format are
 * the origami fork's own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/memalign.h"
#include "qemu/hw-version.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/block/block.h"
#include "block/block-common.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define MAX_SERIAL_LEN           36
#define MAX_SERIAL_LEN_FOR_DEVID 20
#define TYPE_SCSI_TAPE           "scsi-tape"

/*
 * SIMH tape image: a record is a 4-byte little-endian length, the data
 * padded to an even length, and the length again so the medium can be
 * walked backwards; a tape mark is a zero length; SIMH's all-ones
 * end-of-medium marker follows the last entry and is read as end-of-data.
 * The marker is written because the block layer reports an image's length
 * rounded up to a sector, so the file's end cannot mark the data's end:
 * the zero padding past it would read as tape marks.
 */
#define TAPE_MARK          0x00000000u
#define TAPE_EOD           0xffffffffu
#define TAPE_HDR_LEN       4
#define TAPE_MAX_BLOCK     0x00ffffffu
#define TAPE_MIN_BLOCK     1
/* The most one command moves; bounds the buffer a guest can make us take. */
#define TAPE_MAX_TRANSFER  (2 * TAPE_MAX_BLOCK)

/* Fixed-format sense byte 2 flag bits (SSC). */
#define SENSE_FLAG_FILEMARK  0x80
#define SENSE_FLAG_EOM       0x40
#define SENSE_FLAG_ILI       0x20

#define TAPE_INQUIRY_LEN     36
#define TAPE_MODE_MAX        96

OBJECT_DECLARE_SIMPLE_TYPE(SCSITapeState, SCSI_TAPE)

typedef struct SCSITapeReq {
    SCSIRequest req;
    struct iovec iov;
    uint32_t buflen;
    /* A CHECK CONDITION that follows a partial data transfer. */
    bool check;
} SCSITapeReq;

struct SCSITapeState {
    SCSIDevice qdev;
    /* Byte offset of the current position in the backing image. */
    uint64_t pos;
    /* Records and marks passed since beginning-of-tape, for READ POSITION. */
    uint32_t block;
    uint32_t file_num;
    /* Zero selects variable-length records. */
    uint32_t block_size;
    uint8_t density;
    bool medium_loaded;
    bool prevent_removal;
    bool write_protect;
    char *vendor;
    char *version;
    char *serial;
    char *product;
    char *device_id;
};

typedef enum {
    TAPE_ENT_RECORD,
    TAPE_ENT_MARK,
    TAPE_ENT_EOD,
    TAPE_ENT_ERROR,
} TapeEntry;

static bool tape_medium_present(SCSITapeState *s)
{
    return s->medium_loaded && blk_is_inserted(s->qdev.conf.blk);
}

static void tape_rewind(SCSITapeState *s)
{
    s->pos = 0;
    s->block = 0;
    s->file_num = 0;
}

static uint32_t tape_pad(uint32_t len)
{
    return (len + 1) & ~1u;
}

/* -1 is the only failure a walk distinguishes: the image ends here. */
static int tape_read_u32(SCSITapeState *s, uint64_t off, uint32_t *val)
{
    uint8_t buf[TAPE_HDR_LEN];
    int64_t len = blk_getlength(s->qdev.conf.blk);

    if (len < 0 || off + TAPE_HDR_LEN > (uint64_t)len) {
        return -1;
    }
    if (blk_pread(s->qdev.conf.blk, off, TAPE_HDR_LEN, buf, 0) < 0) {
        return -EIO;
    }
    *val = ldl_le_p(buf);
    return 0;
}

/*
 * An image written elsewhere ends at its last entry with no end-of-data
 * marker, and the block layer pads what it reports past that end with
 * zeros, which read as tape marks. A zero header in the last sector with
 * nothing but zeros after it is that padding, not a mark.
 */
static bool tape_is_padding(SCSITapeState *s, uint64_t pos)
{
    uint8_t buf[BDRV_SECTOR_SIZE];
    int64_t len = blk_getlength(s->qdev.conf.blk);
    uint64_t rest;

    if (len < 0 || (uint64_t)len - pos > sizeof(buf)) {
        return false;
    }
    rest = len - pos;
    if (blk_pread(s->qdev.conf.blk, pos, rest, buf, 0) < 0) {
        return false;
    }
    return buffer_is_zero(buf, rest);
}

/* What lies at pos, and where the entry after it starts. */
static TapeEntry tape_entry_forward(SCSITapeState *s, uint64_t pos,
                                    uint32_t *reclen, uint64_t *next)
{
    uint32_t len;
    int ret = tape_read_u32(s, pos, &len);

    if (ret == -1) {
        return TAPE_ENT_EOD;
    }
    if (ret < 0) {
        return TAPE_ENT_ERROR;
    }
    if (len == TAPE_EOD) {
        return TAPE_ENT_EOD;
    }
    if (len == TAPE_MARK) {
        if (tape_is_padding(s, pos)) {
            return TAPE_ENT_EOD;
        }
        *next = pos + TAPE_HDR_LEN;
        return TAPE_ENT_MARK;
    }
    if (len > TAPE_MAX_BLOCK) {
        return TAPE_ENT_ERROR;
    }
    *reclen = len;
    *next = pos + TAPE_HDR_LEN + tape_pad(len) + TAPE_HDR_LEN;
    return TAPE_ENT_RECORD;
}

/* What lies just before pos, and where it starts. Never called at BOT. */
static TapeEntry tape_entry_backward(SCSITapeState *s, uint64_t pos,
                                     uint32_t *reclen, uint64_t *prev)
{
    uint32_t len, head;

    if (pos < TAPE_HDR_LEN || tape_read_u32(s, pos - TAPE_HDR_LEN, &len) < 0) {
        return TAPE_ENT_ERROR;
    }
    if (len == TAPE_MARK) {
        *prev = pos - TAPE_HDR_LEN;
        return TAPE_ENT_MARK;
    }
    if (len == TAPE_EOD || len > TAPE_MAX_BLOCK) {
        return TAPE_ENT_ERROR;
    }
    *prev = pos - TAPE_HDR_LEN - tape_pad(len) - TAPE_HDR_LEN;
    if (pos < TAPE_HDR_LEN + tape_pad(len) + TAPE_HDR_LEN ||
        tape_read_u32(s, *prev, &head) < 0 || head != len) {
        return TAPE_ENT_ERROR;
    }
    *reclen = len;
    return TAPE_ENT_RECORD;
}

/*
 * The image ends exactly where the last write ended: a write past the end
 * grows it, and a write short of the end drops everything after, as a real
 * tape does. A backend that cannot resize makes the failure loud rather
 * than leaving stale records for a later walk to find.
 */
static int tape_set_length(SCSITapeState *s, uint64_t end)
{
    int64_t len = blk_getlength(s->qdev.conf.blk);
    Error *err = NULL;

    if (len < 0) {
        return -EIO;
    }
    if ((uint64_t)len == end) {
        return 0;
    }
    if (blk_truncate(s->qdev.conf.blk, end, false, PREALLOC_MODE_OFF, 0,
                     &err) < 0) {
        /* A host-side failure; the guest sees a medium error. */
        warn_report("scsi-tape: cannot resize the image to the new "
                    "end-of-data: %s", error_get_pretty(err));
        error_free(err);
        return -EIO;
    }
    return 0;
}

/* End-of-data follows the last entry, so a walk never reaches padding. */
static int tape_seal(SCSITapeState *s, uint64_t end)
{
    uint8_t hdr[TAPE_HDR_LEN];

    stl_le_p(hdr, TAPE_EOD);
    if (blk_pwrite(s->qdev.conf.blk, end, TAPE_HDR_LEN, hdr, 0) < 0) {
        return -EIO;
    }
    return 0;
}

static int tape_write_record(SCSITapeState *s, const uint8_t *data,
                             uint32_t len)
{
    BlockBackend *blk = s->qdev.conf.blk;
    uint8_t hdr[TAPE_HDR_LEN];
    uint8_t pad = 0;
    uint64_t off = s->pos;
    uint64_t end = off + TAPE_HDR_LEN + tape_pad(len) + TAPE_HDR_LEN;

    if (tape_set_length(s, end + TAPE_HDR_LEN) < 0) {
        return -EIO;
    }
    stl_le_p(hdr, len);
    if (blk_pwrite(blk, off, TAPE_HDR_LEN, hdr, 0) < 0 ||
        blk_pwrite(blk, off + TAPE_HDR_LEN, len, data, 0) < 0) {
        return -EIO;
    }
    off += TAPE_HDR_LEN + len;
    if (len & 1) {
        if (blk_pwrite(blk, off, 1, &pad, 0) < 0) {
            return -EIO;
        }
        off++;
    }
    if (blk_pwrite(blk, off, TAPE_HDR_LEN, hdr, 0) < 0 ||
        tape_seal(s, end) < 0) {
        return -EIO;
    }
    s->pos = end;
    s->block++;
    return 0;
}

static int tape_write_mark(SCSITapeState *s)
{
    uint8_t hdr[TAPE_HDR_LEN];

    if (tape_set_length(s, s->pos + 2 * TAPE_HDR_LEN) < 0) {
        return -EIO;
    }
    stl_le_p(hdr, TAPE_MARK);
    if (blk_pwrite(s->qdev.conf.blk, s->pos, TAPE_HDR_LEN, hdr, 0) < 0 ||
        tape_seal(s, s->pos + TAPE_HDR_LEN) < 0) {
        return -EIO;
    }
    s->pos += TAPE_HDR_LEN;
    s->block++;
    s->file_num++;
    return 0;
}

/* Sense a completion will carry; the caller decides when it completes. */
static void tape_set_sense(SCSITapeReq *r, uint8_t key, uint8_t asc,
                           uint8_t ascq, uint8_t flags, int32_t info)
{
    SCSIRequest *req = &r->req;
    SCSISense sense = { .key = key, .asc = asc, .ascq = ascq };

    req->sense_len = scsi_build_sense(req->sense, sense);
    req->sense[2] |= flags;
    if (flags || info) {
        req->sense[0] |= 0x80;
        stl_be_p(req->sense + 3, (uint32_t)info);
    }
    r->check = true;
}

static void tape_check_sense(SCSITapeReq *r, uint8_t key, uint8_t asc,
                             uint8_t ascq, uint8_t flags, int32_t info)
{
    tape_set_sense(r, key, asc, ascq, flags, info);
    scsi_req_complete(&r->req, CHECK_CONDITION);
}

static void tape_check_condition(SCSITapeReq *r, SCSISense sense)
{
    scsi_req_build_sense(&r->req, sense);
    scsi_req_complete(&r->req, CHECK_CONDITION);
}

static void tape_filemark(SCSITapeReq *r, int32_t remaining)
{
    tape_set_sense(r, NO_SENSE, 0x00, 0x01, SENSE_FLAG_FILEMARK, remaining);
}

static void tape_end_of_data(SCSITapeReq *r, int32_t remaining)
{
    tape_set_sense(r, BLANK_CHECK, 0x00, 0x05, 0, remaining);
}

/*
 * The drivers disagree on beginning-of-medium: OpenBSD recognises it only
 * as BLANK CHECK 00/04 on a SPACE, and reads the NO SENSE + end-of-medium
 * form as a pending end-of-medium that fails the next transfer. OpenBSD is
 * the first guest tested, so its form is emitted; the datasheet records
 * the divergence as a per-guest decision.
 */
static void tape_beginning_of_medium(SCSITapeReq *r, int32_t remaining)
{
    tape_set_sense(r, BLANK_CHECK, 0x00, 0x04, 0, remaining);
}

static void tape_medium_error(SCSITapeReq *r)
{
    tape_set_sense(r, MEDIUM_ERROR, 0x11, 0x00, 0, 0);
}

static void scsi_tape_init_iovec(SCSITapeReq *r, size_t size)
{
    SCSITapeState *s = SCSI_TAPE(r->req.dev);

    if (!r->iov.iov_base) {
        r->buflen = size;
        r->iov.iov_base = blk_blockalign(s->qdev.conf.blk, r->buflen);
        /* Bytes a handler leaves untouched must not be host memory. */
        memset(r->iov.iov_base, 0, r->buflen);
    }
    r->iov.iov_len = MIN(r->buflen, size);
}

static uint32_t cdb_be24(const uint8_t *p)
{
    return (p[0] << 16) | (p[1] << 8) | p[2];
}

static int32_t cdb_se24(const uint8_t *p)
{
    uint32_t v = cdb_be24(p);

    return v & 0x800000 ? (int32_t)(v | 0xff000000u) : (int32_t)v;
}

static void tape_copy_out(uint8_t *out, uint32_t *len, uint32_t alloc)
{
    *len = MIN(*len, alloc);
}

static uint32_t tape_inquiry(SCSITapeState *s, SCSITapeReq *r,
                             const uint8_t *cdb, uint8_t *out)
{
    uint32_t alloc = lduw_be_p(cdb + 3);
    uint32_t len;

    if (cdb[1] & 0x01) {
        switch (cdb[2]) {
        case 0x00:
            out[0] = TYPE_TAPE;
            out[1] = 0x00;
            out[3] = 2;
            out[4] = 0x00;
            out[5] = 0x80;
            len = 6;
            break;
        case 0x80: {
            size_t slen = s->serial ? strlen(s->serial) : 0;

            out[0] = TYPE_TAPE;
            out[1] = 0x80;
            out[3] = slen;
            memcpy(out + 4, s->serial, slen);
            len = 4 + slen;
            break;
        }
        default:
            tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
            return 0;
        }
    } else {
        if (cdb[2] != 0) {
            tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
            return 0;
        }
        out[0] = TYPE_TAPE;
        out[1] = 0x80;
        out[2] = 0x02;
        out[3] = 0x02;
        out[4] = TAPE_INQUIRY_LEN - 5;
        strpadcpy((char *)out + 8, 8, s->vendor, ' ');
        strpadcpy((char *)out + 16, 16, s->product, ' ');
        strpadcpy((char *)out + 32, 4, s->version, ' ');
        len = TAPE_INQUIRY_LEN;
    }
    tape_copy_out(out, &len, alloc);
    return len;
}

static uint32_t tape_request_sense(SCSITapeReq *r, const uint8_t *cdb,
                                   uint8_t *out)
{
    uint32_t len = scsi_build_sense(out, SENSE_CODE(NO_SENSE));

    tape_copy_out(out, &len, cdb[4]);
    return len;
}

static uint32_t tape_read_block_limits(uint8_t *out)
{
    out[0] = 0;
    out[1] = (TAPE_MAX_BLOCK >> 16) & 0xff;
    out[2] = (TAPE_MAX_BLOCK >> 8) & 0xff;
    out[3] = TAPE_MAX_BLOCK & 0xff;
    stw_be_p(out + 4, TAPE_MIN_BLOCK);
    return 6;
}

static uint32_t tape_mode_page(uint8_t page, uint8_t *out)
{
    switch (page) {
    case 0x01:
        out[0] = 0x01;
        out[1] = 0x0a;
        return 12;
    case 0x0f:
        out[0] = 0x0f;
        out[1] = 0x0e;
        /* Decompression enabled, nothing else: no algorithm is modelled. */
        out[3] = 0x80;
        return 16;
    case 0x10:
        out[0] = 0x10;
        out[1] = 0x0e;
        /* Early-warning and end-of-data are reported, not buffered away. */
        out[10] = 0x18;
        return 16;
    case 0x11:
        /*
         * Medium partition page: one partition, not partitionable (maximum
         * additional partitions and additional partitions defined both
         * zero). A DAT driver reads it at open and fails the open if it is
         * missing, so a single-partition medium must still answer it.
         */
        out[0] = 0x11;
        out[1] = 0x06;
        return 8;
    default:
        return 0;
    }
}

static uint32_t tape_mode_sense(SCSITapeState *s, SCSITapeReq *r,
                                const uint8_t *cdb, uint8_t *out)
{
    bool dbd = cdb[1] & 0x08;
    uint8_t pc = cdb[2] >> 6;
    uint8_t page = cdb[2] & 0x3f;
    uint32_t len = 4;

    if (pc == 3) {
        tape_check_condition(r, SENSE_CODE(SAVING_PARAMS_NOT_SUPPORTED));
        return 0;
    }
    out[1] = 0x00;
    out[2] = (s->write_protect ? 0x80 : 0x00) | 0x10;
    out[3] = dbd ? 0 : 8;
    if (!dbd) {
        out[4] = s->density;
        out[9] = s->block_size >> 16;
        out[10] = s->block_size >> 8;
        out[11] = s->block_size;
        len += 8;
    }

    switch (page) {
    case 0x00:
        break;
    case 0x01:
    case 0x0f:
    case 0x10:
    case 0x11:
        len += tape_mode_page(page, out + len);
        break;
    case 0x3f:
        len += tape_mode_page(0x01, out + len);
        len += tape_mode_page(0x0f, out + len);
        len += tape_mode_page(0x10, out + len);
        len += tape_mode_page(0x11, out + len);
        break;
    default:
        tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return 0;
    }
    /*
     * Changeable values: the descriptor's density and block length, and
     * every page body, are settable; headers and reserved bytes are not.
     */
    if (pc == 1) {
        uint32_t off = 4;

        if (!dbd) {
            out[4] = 0xff;
            out[9] = out[10] = out[11] = 0xff;
            off += 8;
        }
        while (off + 2 <= len) {
            memset(out + off + 2, 0xff, out[off + 1]);
            off += 2 + out[off + 1];
        }
    }
    out[0] = len - 1;
    tape_copy_out(out, &len, cdb[4]);
    trace_scsi_tape_mode_sense(pc, page, dbd, cdb[4], len);
    return len;
}

static void tape_mode_select(SCSITapeState *s, SCSITapeReq *r,
                             const uint8_t *in, uint32_t len)
{
    uint32_t bd_len, off;

    if (len < 4) {
        tape_check_condition(r, SENSE_CODE(INVALID_PARAM_LEN));
        return;
    }
    bd_len = in[3];
    if (4 + bd_len > len) {
        tape_check_condition(r, SENSE_CODE(INVALID_PARAM_LEN));
        return;
    }
    if (bd_len == 8) {
        uint32_t bs = cdb_be24(in + 9);

        if (bs > TAPE_MAX_BLOCK) {
            tape_check_condition(r, SENSE_CODE(INVALID_PARAM));
            return;
        }
        if (in[4]) {
            s->density = in[4];
        }
        s->block_size = bs;
        /* The bus core sizes a fixed-mode transfer from this. */
        s->qdev.blocksize = bs ? bs : 512;
    } else if (bd_len != 0) {
        tape_check_condition(r, SENSE_CODE(INVALID_PARAM));
        return;
    }
    trace_scsi_tape_mode_select(len, bd_len, s->block_size, s->density);

    /*
     * Pages are accepted whole. None is modelled beyond what MODE SENSE
     * reports, so a guest's choice cannot be honoured; it is logged so the
     * scope can be revisited rather than rejected and mounted around.
     */
    for (off = 4 + bd_len; off + 2 <= len; off += 2 + in[off + 1]) {
        uint8_t page = in[off] & 0x3f;

        if (page != 0x01 && page != 0x0f && page != 0x10 && page != 0x11) {
            qemu_log_mask(LOG_UNIMP, "scsi-tape: MODE SELECT page 0x%02x "
                          "accepted but not modelled\n", page);
        }
    }
    scsi_req_complete(&r->req, GOOD);
}

static uint32_t tape_read_position(SCSITapeState *s, SCSITapeReq *r,
                                   const uint8_t *cdb, uint8_t *out)
{
    uint8_t action = cdb[1] & 0x1f;

    out[0] = s->pos == 0 ? 0x80 : 0x00;
    switch (action) {
    case 0x00:
        stl_be_p(out + 4, s->block);
        stl_be_p(out + 8, s->block);
        return 20;
    case 0x06:
        stq_be_p(out + 8, s->block);
        stq_be_p(out + 16, s->file_num);
        return 32;
    default:
        tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return 0;
    }
}

/*
 * READ(6): fixed mode moves whole blocks of the mode block length, variable
 * mode moves one record. A mark, end-of-data or a length mismatch ends the
 * transfer early and is reported through sense with the residue in the
 * information field, after whatever data was read is delivered.
 */
static uint32_t tape_read6(SCSITapeState *s, SCSITapeReq *r,
                           const uint8_t *cdb, uint8_t **outp)
{
    bool fixed = cdb[1] & 0x01;
    bool sili = cdb[1] & 0x02;
    uint32_t xfer = cdb_be24(cdb + 2);
    uint32_t total, count, got = 0, remaining;
    uint8_t *out;

    if (fixed) {
        if (s->block_size == 0 ||
            xfer > TAPE_MAX_TRANSFER / s->block_size) {
            tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
            return 0;
        }
        count = xfer;
        total = xfer * s->block_size;
    } else {
        /* No record exceeds the block limit, so the buffer need not. */
        count = 1;
        total = MIN(xfer, TAPE_MAX_BLOCK);
    }
    if (total == 0) {
        scsi_req_complete(&r->req, GOOD);
        return 0;
    }
    scsi_tape_init_iovec(r, total);
    out = r->iov.iov_base;
    *outp = out;
    remaining = fixed ? count : xfer;

    while (count--) {
        uint32_t reclen = 0, take;
        uint64_t next = 0;
        TapeEntry ent = tape_entry_forward(s, s->pos, &reclen, &next);

        if (ent == TAPE_ENT_EOD) {
            tape_end_of_data(r, remaining);
            break;
        }
        if (ent == TAPE_ENT_ERROR) {
            tape_medium_error(r);
            break;
        }
        if (ent == TAPE_ENT_MARK) {
            s->pos = next;
            s->block++;
            s->file_num++;
            tape_filemark(r, remaining);
            break;
        }

        take = fixed ? MIN(reclen, s->block_size) : MIN(reclen, total);
        if (blk_pread(s->qdev.conf.blk, s->pos + TAPE_HDR_LEN, take,
                      out + got, 0) < 0) {
            tape_medium_error(r);
            break;
        }
        got += take;
        s->pos = next;
        s->block++;

        if (fixed) {
            /* The wrong-length block does not count as transferred. */
            if (reclen != s->block_size) {
                tape_set_sense(r, NO_SENSE, 0x00, 0x00, SENSE_FLAG_ILI,
                               remaining);
                break;
            }
            remaining--;
        } else if (reclen != xfer && !(sili && reclen < xfer)) {
            tape_set_sense(r, NO_SENSE, 0x00, 0x00, SENSE_FLAG_ILI,
                           (int32_t)xfer - (int32_t)reclen);
        }
    }
    return got;
}

static void tape_write6(SCSITapeState *s, SCSITapeReq *r,
                        const uint8_t *in, uint32_t len)
{
    bool fixed = r->req.cmd.buf[1] & 0x01;
    uint32_t count = fixed ? cdb_be24(r->req.cmd.buf + 2) : 1;
    uint32_t reclen = fixed ? s->block_size : len;
    uint32_t off = 0;

    while (count--) {
        if (tape_write_record(s, in + off, reclen) < 0) {
            tape_check_sense(r, MEDIUM_ERROR, 0x0c, 0x00, 0,
                             fixed ? count + 1 : reclen);
            return;
        }
        off += reclen;
    }
    scsi_req_complete(&r->req, GOOD);
}

static void tape_space(SCSITapeState *s, SCSITapeReq *r, const uint8_t *cdb)
{
    uint8_t code = cdb[1] & 0x07;
    int32_t count = cdb_se24(cdb + 2);
    uint32_t remaining;
    uint32_t reclen;
    uint64_t at;
    TapeEntry ent;

    if (code != 0 && code != 1 && code != 3) {
        tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return;
    }
    if (code == 3) {
        for (;;) {
            ent = tape_entry_forward(s, s->pos, &reclen, &at);
            if (ent == TAPE_ENT_ERROR) {
                tape_medium_error(r);
                scsi_req_complete(&r->req, CHECK_CONDITION);
                return;
            }
            if (ent == TAPE_ENT_EOD) {
                break;
            }
            s->pos = at;
            s->block++;
            if (ent == TAPE_ENT_MARK) {
                s->file_num++;
            }
        }
        scsi_req_complete(&r->req, GOOD);
        return;
    }

    remaining = count < 0 ? -count : count;
    while (remaining) {
        if (count > 0) {
            ent = tape_entry_forward(s, s->pos, &reclen, &at);
            if (ent == TAPE_ENT_EOD) {
                tape_end_of_data(r, remaining);
                break;
            }
            if (ent == TAPE_ENT_ERROR) {
                tape_medium_error(r);
                break;
            }
            s->pos = at;
            s->block++;
            if (ent == TAPE_ENT_MARK) {
                s->file_num++;
                if (code == 0) {
                    tape_filemark(r, remaining);
                    break;
                }
                remaining--;
            } else {
                if (code == 0) {
                    remaining--;
                }
            }
        } else {
            if (s->pos == 0) {
                tape_beginning_of_medium(r, remaining);
                break;
            }
            ent = tape_entry_backward(s, s->pos, &reclen, &at);
            if (ent == TAPE_ENT_ERROR) {
                tape_medium_error(r);
                break;
            }
            s->pos = at;
            s->block--;
            if (ent == TAPE_ENT_MARK) {
                s->file_num--;
                if (code == 0) {
                    tape_filemark(r, remaining);
                    break;
                }
                remaining--;
            } else {
                if (code == 0) {
                    remaining--;
                }
            }
        }
    }
    scsi_req_complete(&r->req, r->check ? CHECK_CONDITION : GOOD);
}

static void tape_write_filemarks(SCSITapeState *s, SCSITapeReq *r,
                                 const uint8_t *cdb)
{
    uint32_t count = cdb_be24(cdb + 2);

    while (count) {
        if (tape_write_mark(s) < 0) {
            tape_check_sense(r, MEDIUM_ERROR, 0x0c, 0x00, 0, count);
            return;
        }
        count--;
    }
    scsi_req_complete(&r->req, GOOD);
}

/* Position to a logical object counted from beginning-of-tape. */
static void tape_locate(SCSITapeState *s, SCSITapeReq *r, const uint8_t *cdb)
{
    uint32_t target = ldl_be_p(cdb + 3);
    uint32_t reclen;
    uint64_t at;
    TapeEntry ent;

    tape_rewind(s);
    while (s->block < target) {
        ent = tape_entry_forward(s, s->pos, &reclen, &at);
        if (ent == TAPE_ENT_EOD) {
            tape_end_of_data(r, target - s->block);
            scsi_req_complete(&r->req, CHECK_CONDITION);
            return;
        }
        if (ent == TAPE_ENT_ERROR) {
            tape_medium_error(r);
            scsi_req_complete(&r->req, CHECK_CONDITION);
            return;
        }
        s->pos = at;
        s->block++;
        if (ent == TAPE_ENT_MARK) {
            s->file_num++;
        }
    }
    scsi_req_complete(&r->req, GOOD);
}

static void tape_load_unload(SCSITapeState *s, SCSITapeReq *r,
                             const uint8_t *cdb)
{
    /* Retension winds to the end and back; the image needs nothing. */
    if (cdb[4] & 0x02) {
        if (!tape_medium_present(s)) {
            tape_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return;
        }
        tape_rewind(s);
        scsi_req_complete(&r->req, GOOD);
        return;
    }
    if (cdb[4] & 0x01) {
        if (!blk_is_inserted(s->qdev.conf.blk)) {
            tape_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return;
        }
        s->medium_loaded = true;
    } else {
        if (s->prevent_removal) {
            tape_check_condition(r,
                                 SENSE_CODE(NOT_READY_REMOVAL_PREVENTED));
            return;
        }
        s->medium_loaded = false;
    }
    tape_rewind(s);
    scsi_req_complete(&r->req, GOOD);
}

static int32_t scsi_tape_send_command(SCSIRequest *req, uint8_t *cdb)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    SCSITapeState *s = SCSI_TAPE(req->dev);
    uint8_t *out;
    uint32_t len;

    trace_scsi_tape_command(cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5]);

    /* Commands that need no medium, or say so themselves. */
    switch (cdb[0]) {
    case INQUIRY:
        scsi_tape_init_iovec(r, TAPE_INQUIRY_LEN + MAX_SERIAL_LEN);
        len = tape_inquiry(s, r, cdb, r->iov.iov_base);
        goto data_in;
    case REQUEST_SENSE:
        scsi_tape_init_iovec(r, SCSI_SENSE_BUF_SIZE);
        len = tape_request_sense(r, cdb, r->iov.iov_base);
        goto data_in;
    case ALLOW_MEDIUM_REMOVAL:
        s->prevent_removal = cdb[4] & 0x01;
        scsi_req_complete(req, GOOD);
        return 0;
    case LOAD_UNLOAD:
        tape_load_unload(s, r, cdb);
        return 0;
    case TEST_UNIT_READY:
        if (!tape_medium_present(s)) {
            tape_check_condition(r, SENSE_CODE(NO_MEDIUM));
        } else {
            scsi_req_complete(req, GOOD);
        }
        return 0;
    case READ_BLOCK_LIMITS:
        scsi_tape_init_iovec(r, 6);
        len = tape_read_block_limits(r->iov.iov_base);
        goto data_in;
    case MODE_SENSE:
        scsi_tape_init_iovec(r, TAPE_MODE_MAX);
        memset(r->iov.iov_base, 0, TAPE_MODE_MAX);
        len = tape_mode_sense(s, r, cdb, r->iov.iov_base);
        goto data_in;
    case MODE_SELECT:
        len = cdb[4];
        if (len == 0) {
            scsi_req_complete(req, GOOD);
            return 0;
        }
        scsi_tape_init_iovec(r, len);
        return -len;
    default:
        break;
    }

    if (!tape_medium_present(s)) {
        tape_check_condition(r, SENSE_CODE(NO_MEDIUM));
        return 0;
    }

    switch (cdb[0]) {
    case REWIND:
        tape_rewind(s);
        scsi_req_complete(req, GOOD);
        return 0;
    case READ_POSITION:
        scsi_tape_init_iovec(r, 32);
        memset(r->iov.iov_base, 0, 32);
        len = tape_read_position(s, r, cdb, r->iov.iov_base);
        goto data_in;
    case READ_6:
        len = tape_read6(s, r, cdb, &out);
        if (len == 0) {
            if (r->check) {
                scsi_req_complete(req, CHECK_CONDITION);
            }
            /* Otherwise a zero-length read already completed. */
            return 0;
        }
        r->iov.iov_len = len;
        return len;
    case SPACE:
        tape_space(s, r, cdb);
        return 0;
    case LOCATE_10:
        tape_locate(s, r, cdb);
        return 0;
    case WRITE_6:
    case WRITE_FILEMARKS:
    case ERASE:
        if (s->write_protect) {
            tape_check_condition(r, SENSE_CODE(WRITE_PROTECTED));
            return 0;
        }
        break;
    default:
        tape_check_condition(r, SENSE_CODE(INVALID_OPCODE));
        return 0;
    }

    switch (cdb[0]) {
    case WRITE_6: {
        bool fixed = cdb[1] & 0x01;
        uint32_t xfer = cdb_be24(cdb + 2);

        if (fixed) {
            if (s->block_size == 0 ||
                xfer > TAPE_MAX_TRANSFER / s->block_size) {
                tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
                return 0;
            }
            len = xfer * s->block_size;
        } else {
            if (xfer > TAPE_MAX_BLOCK) {
                tape_check_condition(r, SENSE_CODE(INVALID_FIELD));
                return 0;
            }
            len = xfer;
        }
        if (len == 0) {
            scsi_req_complete(req, GOOD);
            return 0;
        }
        scsi_tape_init_iovec(r, len);
        return -len;
    }
    case WRITE_FILEMARKS:
        tape_write_filemarks(s, r, cdb);
        return 0;
    case ERASE:
        if (tape_set_length(s, s->pos + TAPE_HDR_LEN) < 0 ||
            tape_seal(s, s->pos) < 0) {
            tape_check_sense(r, MEDIUM_ERROR, 0x0c, 0x00, 0, 0);
            return 0;
        }
        scsi_req_complete(req, GOOD);
        return 0;
    default:
        g_assert_not_reached();
    }

data_in:
    if (len == 0) {
        /* Either the handler completed it, or nothing was asked for. */
        if (req->status == -1) {
            scsi_req_complete(req, GOOD);
        }
        return 0;
    }
    r->iov.iov_len = len;
    return len;
}

static void scsi_tape_read_data(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    int buflen = r->iov.iov_len;

    if (buflen) {
        r->iov.iov_len = 0;
        scsi_req_data(&r->req, buflen);
        return;
    }
    scsi_req_complete(&r->req, r->check ? CHECK_CONDITION : GOOD);
}

static void scsi_tape_write_data(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);
    SCSITapeState *s = SCSI_TAPE(req->dev);

    if (r->iov.iov_len) {
        int buflen = r->iov.iov_len;

        r->iov.iov_len = 0;
        scsi_req_data(&r->req, buflen);
        return;
    }

    switch (req->cmd.buf[0]) {
    case MODE_SELECT:
        tape_mode_select(s, r, r->iov.iov_base, r->buflen);
        break;
    case WRITE_6:
        tape_write6(s, r, r->iov.iov_base, r->buflen);
        break;
    default:
        g_assert_not_reached();
    }
}

static uint8_t *scsi_tape_get_buf(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);

    return r->iov.iov_base;
}

static void scsi_tape_free_request(SCSIRequest *req)
{
    SCSITapeReq *r = DO_UPCAST(SCSITapeReq, req, req);

    qemu_vfree(r->iov.iov_base);
}

static const SCSIReqOps scsi_tape_reqops = {
    .size         = sizeof(SCSITapeReq),
    .free_req     = scsi_tape_free_request,
    .send_command = scsi_tape_send_command,
    .read_data    = scsi_tape_read_data,
    .write_data   = scsi_tape_write_data,
    .get_buf      = scsi_tape_get_buf,
};

static SCSIRequest *scsi_tape_new_request(SCSIDevice *dev, uint32_t tag,
                                          uint32_t lun, uint8_t *buf,
                                          void *hba_private)
{
    return scsi_req_alloc(&scsi_tape_reqops, dev, tag, lun, hba_private);
}

static void scsi_tape_reset(DeviceState *dev)
{
    SCSITapeState *s = SCSI_TAPE(dev);

    scsi_device_purge_requests(&s->qdev, SENSE_CODE(RESET));
    tape_rewind(s);
    s->block_size = 0;
    s->qdev.blocksize = 512;
    s->density = 0;
    s->prevent_removal = false;
    s->medium_loaded = blk_is_inserted(s->qdev.conf.blk);
}

static void scsi_tape_realize(SCSIDevice *dev, Error **errp)
{
    SCSITapeState *s = SCSI_TAPE(dev);
    bool read_only;

    if (!s->qdev.conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }
    if (s->serial && strlen(s->serial) > MAX_SERIAL_LEN) {
        error_setg(errp, "The serial number can't be longer than %d "
                   "characters", MAX_SERIAL_LEN);
        return;
    }
    if (!s->device_id && s->serial) {
        if (strlen(s->serial) > MAX_SERIAL_LEN_FOR_DEVID) {
            error_setg(errp, "The serial number can't be longer than %d "
                       "characters when it is also used as the default for "
                       "device_id", MAX_SERIAL_LEN_FOR_DEVID);
            return;
        }
        s->device_id = g_strdup(s->serial);
    }

    read_only = !blk_supports_write_perm(s->qdev.conf.blk);
    if (!blkconf_apply_backend_options(&dev->conf, read_only, true, errp)) {
        return;
    }
    /* A write sets the image's length, so a writable tape must resize it. */
    if (!read_only) {
        uint64_t perm, shared;

        blk_get_perm(s->qdev.conf.blk, &perm, &shared);
        if (blk_set_perm(s->qdev.conf.blk, perm | BLK_PERM_RESIZE, shared,
                         errp) < 0) {
            return;
        }
    }
    s->write_protect = read_only;
    s->qdev.type = TYPE_TAPE;
    s->qdev.blocksize = 512;

    if (!s->vendor) {
        s->vendor = g_strdup("QEMU");
    }
    if (!s->product) {
        s->product = g_strdup("QEMU TAPE");
    }
    if (!s->version) {
        s->version = g_strdup(QEMU_HW_VERSION);
    }

    tape_rewind(s);
    s->medium_loaded = blk_is_inserted(s->qdev.conf.blk);
}

/* The bus core sizes fixed-mode transfers from this; keep it in step. */
static int scsi_tape_post_load(void *opaque, int version_id)
{
    SCSITapeState *s = opaque;

    s->qdev.blocksize = s->block_size ? s->block_size : 512;
    return 0;
}

static const VMStateDescription vmstate_scsi_tape = {
    .name = "scsi-tape",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = scsi_tape_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SCSI_DEVICE(qdev, SCSITapeState),
        VMSTATE_UINT64(pos, SCSITapeState),
        VMSTATE_UINT32(block, SCSITapeState),
        VMSTATE_UINT32(file_num, SCSITapeState),
        VMSTATE_UINT32(block_size, SCSITapeState),
        VMSTATE_UINT8(density, SCSITapeState),
        VMSTATE_BOOL(medium_loaded, SCSITapeState),
        VMSTATE_BOOL(prevent_removal, SCSITapeState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property scsi_tape_properties[] = {
    DEFINE_PROP_DRIVE_IOTHREAD("drive", SCSITapeState, qdev.conf.blk),
    DEFINE_BLOCK_PROPERTIES_BASE(SCSITapeState, qdev.conf),
    DEFINE_PROP_STRING("ver", SCSITapeState, version),
    DEFINE_PROP_STRING("serial", SCSITapeState, serial),
    DEFINE_PROP_STRING("vendor", SCSITapeState, vendor),
    DEFINE_PROP_STRING("product", SCSITapeState, product),
    DEFINE_PROP_STRING("device_id", SCSITapeState, device_id),
};

static void scsi_tape_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize   = scsi_tape_realize;
    sc->alloc_req = scsi_tape_new_request;
    device_class_set_legacy_reset(dc, scsi_tape_reset);
    dc->desc = "virtual SCSI tape";
    dc->vmsd = &vmstate_scsi_tape;
    device_class_set_props(dc, scsi_tape_properties);
}

static const TypeInfo scsi_tape_info = {
    .name          = TYPE_SCSI_TAPE,
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSITapeState),
    .class_init    = scsi_tape_class_init,
};

static void scsi_tape_register_types(void)
{
    type_register_static(&scsi_tape_info);
}

type_init(scsi_tape_register_types)
