/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI PROM image decoder. The accepted container subset, measured from
 * the processor and I/O PROMs distributed with IRIX 6.5 and 6.5.30, is
 * described in docs/system/mips/sgi/prom-image.rst; anything else carrying the
 * container signature is refused rather than copied as a raw image.
 *
 * Copyright (c) 2026 James Braid
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <zlib.h>
#include "qemu/bswap.h"
#include "hw/mips/sgi/prom-image.h"

#define SGI_PROM_SIGNATURE      0x4a464b535743534dULL
#define SGI_PROM_VERSION        6
#define SGI_PROM_HEADER_SIZE    4096

/* Big-endian header fields, by byte offset. */
#define SGI_PROM_SIGNATURE_OFFSET       0x40
#define SGI_PROM_VERSION_OFFSET         0x48
#define SGI_PROM_END_OFFSET             0x50
#define SGI_PROM_SEGMENTS_OFFSET        0x7c
#define SGI_PROM_FLAGS_OFFSET           0x90
#define SGI_PROM_PAYLOAD_OFFSET         0x98
#define SGI_PROM_DECODED_LENGTH_OFFSET  0xb0
#define SGI_PROM_STORED_LENGTH_OFFSET   0xb8
#define SGI_PROM_DECODED_SUM_OFFSET     0xc0
#define SGI_PROM_STORED_SUM_OFFSET      0xc8
#define SGI_PROM_EXTENT_OFFSET          0xd0

#define SGI_PROM_FLAGS_STORED   0x10
#define SGI_PROM_FLAGS_GZIP     0x13

/*
 * Zero bytes accepted after a gzip segment. The observed images end zero
 * and six bytes after theirs; this bound is a compatibility policy, not
 * a documented padding rule.
 */
#define SGI_PROM_MAX_TAIL       7

/* A gzip member's header and trailer outweigh zlib's by this much. */
#define SGI_PROM_GZIP_FRAMING   12

void sgi_prom_image_free(SGIPromImage *image)
{
    if (image) {
        g_clear_pointer(&image->file, g_bytes_unref);
        g_clear_pointer(&image->payload, g_bytes_unref);
        g_free(image);
    }
}

static uint64_t sgi_prom_byte_sum(const uint8_t *data, size_t size)
{
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

/* Inflate exactly one gzip member into exactly @decoded_size bytes. */
static GBytes *sgi_prom_inflate(const uint8_t *data, size_t size,
                                size_t decoded_size, Error **errp)
{
    g_autofree uint8_t *decoded = NULL;
    z_stream stream = { 0 };
    bool complete;
    int ret;

    if (size > UINT_MAX || decoded_size > UINT_MAX) {
        error_setg(errp, "SGI PROM gzip segment exceeds decoder limits");
        return NULL;
    }
    decoded = g_malloc(decoded_size);
    stream.next_in = (Bytef *)data;
    stream.avail_in = size;
    stream.next_out = decoded;
    stream.avail_out = decoded_size;
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        error_setg(errp, "cannot initialize the SGI PROM gzip decoder");
        return NULL;
    }
    ret = inflate(&stream, Z_FINISH);
    complete = ret == Z_STREAM_END && stream.total_out == decoded_size &&
               stream.avail_in == 0;
    inflateEnd(&stream);
    if (!complete) {
        error_setg(errp, "invalid SGI PROM gzip segment or decoded length");
        return NULL;
    }
    return g_bytes_new_take(g_steal_pointer(&decoded), decoded_size);
}

static GBytes *sgi_prom_decode_container(GBytes *file, size_t payload_limit,
                                         Error **errp)
{
    const uint8_t *data;
    size_t size, stored_size, i;
    uint64_t decoded_length, flags;
    g_autoptr(GBytes) payload = NULL;
    const uint8_t *decoded;
    size_t decoded_size;

    data = g_bytes_get_data(file, &size);
    if (size < SGI_PROM_HEADER_SIZE) {
        error_setg(errp, "truncated SGI PROM container header");
        return NULL;
    }
    if (ldq_be_p(data + SGI_PROM_VERSION_OFFSET) != SGI_PROM_VERSION) {
        error_setg(errp, "unsupported SGI PROM container version");
        return NULL;
    }
    if (ldq_be_p(data + SGI_PROM_END_OFFSET) != size) {
        error_setg(errp, "SGI PROM container declared end does not match "
                   "its size");
        return NULL;
    }
    if (ldl_be_p(data + SGI_PROM_SEGMENTS_OFFSET) != 1) {
        error_setg(errp, "unsupported SGI PROM container segment count");
        return NULL;
    }
    if (ldq_be_p(data + SGI_PROM_PAYLOAD_OFFSET) != SGI_PROM_HEADER_SIZE) {
        error_setg(errp, "unsupported SGI PROM container payload offset");
        return NULL;
    }
    flags = ldq_be_p(data + SGI_PROM_FLAGS_OFFSET);
    if (flags != SGI_PROM_FLAGS_STORED && flags != SGI_PROM_FLAGS_GZIP) {
        error_setg(errp, "unsupported SGI PROM container flags");
        return NULL;
    }
    decoded_length = ldq_be_p(data + SGI_PROM_DECODED_LENGTH_OFFSET);
    if (!decoded_length) {
        error_setg(errp, "invalid SGI PROM container decoded length");
        return NULL;
    }
    if (decoded_length > payload_limit) {
        error_setg(errp, "SGI PROM container decoded length exceeds flash "
                   "capacity (%zu bytes)", payload_limit);
        return NULL;
    }

    if (flags == SGI_PROM_FLAGS_STORED) {
        if (ldq_be_p(data + SGI_PROM_STORED_LENGTH_OFFSET) ||
            ldq_be_p(data + SGI_PROM_STORED_SUM_OFFSET) ||
            ldq_be_p(data + SGI_PROM_EXTENT_OFFSET) != decoded_length) {
            error_setg(errp, "unsupported SGI PROM uncompressed metadata");
            return NULL;
        }
        if (decoded_length != size - SGI_PROM_HEADER_SIZE) {
            error_setg(errp, "invalid SGI PROM container stored length");
            return NULL;
        }
        payload = g_bytes_new_from_bytes(file, SGI_PROM_HEADER_SIZE,
                                         decoded_length);
    } else {
        uint64_t stored_length =
            ldq_be_p(data + SGI_PROM_STORED_LENGTH_OFFSET);

        if (!stored_length || stored_length > size - SGI_PROM_HEADER_SIZE) {
            error_setg(errp, "invalid SGI PROM container stored length");
            return NULL;
        }
        stored_size = stored_length;
        if (size - SGI_PROM_HEADER_SIZE - stored_size > SGI_PROM_MAX_TAIL) {
            error_setg(errp, "unsupported SGI PROM container trailing bytes");
            return NULL;
        }
        for (i = SGI_PROM_HEADER_SIZE + stored_size; i < size; i++) {
            if (data[i]) {
                error_setg(errp, "nonzero SGI PROM container trailing bytes");
                return NULL;
            }
        }
        if (sgi_prom_byte_sum(data + SGI_PROM_HEADER_SIZE, stored_size) !=
            ldq_be_p(data + SGI_PROM_STORED_SUM_OFFSET)) {
            error_setg(errp, "SGI PROM stored payload checksum mismatch");
            return NULL;
        }
        payload = sgi_prom_inflate(data + SGI_PROM_HEADER_SIZE, stored_size,
                                   decoded_length, errp);
        if (!payload) {
            return NULL;
        }
    }

    decoded = g_bytes_get_data(payload, &decoded_size);
    if (sgi_prom_byte_sum(decoded, decoded_size) !=
        ldq_be_p(data + SGI_PROM_DECODED_SUM_OFFSET)) {
        error_setg(errp, "SGI PROM decoded payload checksum mismatch");
        return NULL;
    }
    return g_steal_pointer(&payload);
}

SGIPromImage *sgi_prom_image_decode(GBytes *file, size_t payload_limit,
                                    Error **errp)
{
    g_autoptr(SGIPromImage) image = g_new0(SGIPromImage, 1);
    const uint8_t *data;
    size_t size;

    data = g_bytes_get_data(file, &size);
    image->file = g_bytes_ref(file);
    if (size < SGI_PROM_SIGNATURE_OFFSET + 8 ||
        ldq_be_p(data + SGI_PROM_SIGNATURE_OFFSET) != SGI_PROM_SIGNATURE) {
        if (size > payload_limit) {
            error_setg(errp, "PROM image exceeds flash capacity (%zu bytes)",
                       payload_limit);
            return NULL;
        }
        image->payload = g_bytes_ref(file);
        return g_steal_pointer(&image);
    }
    image->payload = sgi_prom_decode_container(file, payload_limit, errp);
    if (!image->payload) {
        return NULL;
    }
    return g_steal_pointer(&image);
}

/*
 * The largest file that can still decode to @payload_limit bytes: the
 * header, the worst-case gzip expansion of an incompressible payload and
 * the accepted zero tail.
 */
static uint64_t sgi_prom_file_limit(size_t payload_limit)
{
    return SGI_PROM_HEADER_SIZE + compressBound(payload_limit) +
           SGI_PROM_GZIP_FRAMING + SGI_PROM_MAX_TAIL;
}

SGIPromImage *sgi_prom_image_load(const char *path, size_t payload_limit,
                                  Error **errp)
{
    ERRP_GUARD();
    g_autoptr(GError) gerr = NULL;
    g_autofree char *contents = NULL;
    GBytes *file;
    GStatBuf st;
    gsize length;
    SGIPromImage *image;

    if (g_stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "cannot read PROM image '%s'", path);
        return NULL;
    }
    if ((uint64_t)st.st_size > sgi_prom_file_limit(payload_limit)) {
        error_setg(errp, "PROM image '%s' exceeds flash capacity "
                   "(%zu bytes): no encoding of a %" PRId64 "-byte file "
                   "fits", path, payload_limit, (int64_t)st.st_size);
        return NULL;
    }
    if (!g_file_get_contents(path, &contents, &length, &gerr)) {
        error_setg(errp, "cannot read PROM image '%s': %s", path,
                   gerr->message);
        return NULL;
    }
    file = g_bytes_new_take(g_steal_pointer(&contents), length);
    image = sgi_prom_image_decode(file, payload_limit, errp);
    g_bytes_unref(file);
    if (!image) {
        error_prepend(errp, "'%s': ", path);
    }
    return image;
}
