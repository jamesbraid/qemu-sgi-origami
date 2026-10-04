/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI PROM image decoder, over synthetic containers only: no SGI firmware
 * bytes appear here.
 *
 * Copyright (c) 2026 James Braid
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include <zlib.h>
#include "qemu/bswap.h"
#include "hw/mips/sgi/prom-image.h"

#define PAYLOAD_SIZE 32
#define BUFFER_SIZE 8192
#define HEADER_SIZE 4096

static const uint8_t plain[PAYLOAD_SIZE] = {
    0, 7, 14, 21, 28, 35, 42, 49, 56, 63, 70, 77, 84, 91, 98, 105,
    112, 119, 126, 133, 140, 147, 154, 161, 168, 175, 182, 189,
    196, 203, 210, 217,
};

static uint64_t byte_sum(const uint8_t *data, size_t size)
{
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

/*
 * Build a version-6 single-segment container around @plain, stored or
 * gzip, followed by @padding zero bytes. Fields the decoder treats as
 * metadata only (revision, name, addresses) get arbitrary values.
 */
static size_t make_image(uint8_t *data, bool gzip, size_t padding)
{
    size_t stored_size = sizeof(plain);
    size_t file_size;

    memset(data, 0, BUFFER_SIZE);
    if (gzip) {
        z_stream stream = { 0 };

        g_assert_cmpint(deflateInit2(&stream, Z_DEFAULT_COMPRESSION,
                                     Z_DEFLATED, MAX_WBITS + 16, 8,
                                     Z_DEFAULT_STRATEGY), ==, Z_OK);
        stream.next_in = (Bytef *)plain;
        stream.avail_in = sizeof(plain);
        stream.next_out = data + HEADER_SIZE;
        stream.avail_out = BUFFER_SIZE - HEADER_SIZE;
        g_assert_cmpint(deflate(&stream, Z_FINISH), ==, Z_STREAM_END);
        stored_size = stream.total_out;
        g_assert_cmpint(deflateEnd(&stream), ==, Z_OK);
    } else {
        memcpy(data + HEADER_SIZE, plain, sizeof(plain));
    }
    file_size = HEADER_SIZE + stored_size + padding;
    stl_be_p(data + 0x3c, 156);
    stq_be_p(data + 0x40, 0x4a464b535743534dULL);
    stq_be_p(data + 0x48, 6);
    stq_be_p(data + 0x50, file_size);
    stl_be_p(data + 0x7c, 1);
    memcpy(data + 0x80, "synthetic-image", 16);
    stq_be_p(data + 0x90, gzip ? 0x13 : 0x10);
    stq_be_p(data + 0x98, HEADER_SIZE);
    stq_be_p(data + 0xa0, 0x12345678);
    stq_be_p(data + 0xa8, 0x87654321);
    stq_be_p(data + 0xb0, sizeof(plain));
    stq_be_p(data + 0xb8, gzip ? stored_size : 0);
    stq_be_p(data + 0xc0, byte_sum(plain, sizeof(plain)));
    stq_be_p(data + 0xc8,
             gzip ? byte_sum(data + HEADER_SIZE, stored_size) : 0);
    stq_be_p(data + 0xd0, gzip ? 1024 : sizeof(plain));
    return file_size;
}

static SGIPromImage *decode(const uint8_t *data, size_t size, size_t limit,
                            Error **errp)
{
    g_autoptr(GBytes) file = g_bytes_new(data, size);

    return sgi_prom_image_decode(file, limit, errp);
}

static void reject(const uint8_t *data, size_t size, size_t limit,
                   const char *message)
{
    Error *err = NULL;
    g_autoptr(SGIPromImage) image = decode(data, size, limit, &err);

    g_assert_null(image);
    g_assert_nonnull(err);
    if (!strstr(error_get_pretty(err), message)) {
        g_test_message("expected '%s' in '%s'", message,
                       error_get_pretty(err));
        g_assert_not_reached();
    }
    error_free(err);
}

static void assert_payload(const SGIPromImage *image)
{
    size_t size;
    const uint8_t *payload = g_bytes_get_data(image->payload, &size);

    g_assert_cmpmem(payload, size, plain, sizeof(plain));
}

/* Both encodings decode to the same payload and keep the whole file. */
static void test_wrapped(void)
{
    uint8_t data[BUFFER_SIZE];
    size_t file_size;
    int gzip;

    for (gzip = 0; gzip <= 1; gzip++) {
        g_autoptr(SGIPromImage) image = NULL;

        file_size = make_image(data, gzip, 0);
        image = decode(data, file_size, sizeof(plain), &error_abort);
        assert_payload(image);
        g_assert_cmpmem(g_bytes_get_data(image->file, NULL),
                        g_bytes_get_size(image->file), data, file_size);
    }
}

static void test_malformed(void)
{
    static const struct {
        size_t offset;
        uint64_t value;
        const char *message;
    } cases[] = {
        { 0x48, 7, "version" },
        { 0x90, 0x11, "flags" },
        { 0x98, UINT64_MAX, "payload offset" },
        { 0xb0, UINT64_MAX, "decoded length" },
        { 0xb0, 0, "decoded length" },
        { 0xb8, 1, "metadata" },
        { 0xc0, 0, "decoded payload checksum" },
        { 0xc8, 1, "metadata" },
        { 0xd0, 1, "metadata" },
        { 0x50, UINT64_MAX, "declared end" },
    };
    uint8_t data[BUFFER_SIZE];
    size_t i, size;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        size = make_image(data, false, 0);
        stq_be_p(data + cases[i].offset, cases[i].value);
        reject(data, size, sizeof(plain), cases[i].message);
    }
    size = make_image(data, false, 0);
    stl_be_p(data + 0x7c, 2);
    reject(data, size, sizeof(plain), "segment count");
    stl_be_p(data + 0x7c, 0);
    reject(data, size, sizeof(plain), "segment count");
    size = make_image(data, false, 0);
    reject(data, size, sizeof(plain) - 1, "exceeds flash capacity");
}

static void test_truncated(void)
{
    uint8_t data[BUFFER_SIZE];
    size_t size, file_size;

    file_size = make_image(data, false, 0);
    for (size = 0x48; size < HEADER_SIZE; size++) {
        reject(data, size, sizeof(plain), "truncated");
    }
    reject(data, file_size - 1, sizeof(plain), "declared end");
    stq_be_p(data + 0x50, file_size - 1);
    reject(data, file_size - 1, sizeof(plain), "stored length");
}

static void test_gzip(void)
{
    uint8_t data[BUFFER_SIZE];
    size_t size, stored_size, padding;

    for (padding = 0; padding <= 7; padding++) {
        g_autoptr(SGIPromImage) image = NULL;

        size = make_image(data, true, padding);
        image = decode(data, size, sizeof(plain), &error_abort);
        assert_payload(image);
    }
    size = make_image(data, true, 8);
    reject(data, size, sizeof(plain), "trailing");
    size = make_image(data, true, 1);
    data[size - 1] = 1;
    reject(data, size, sizeof(plain), "trailing");
    size = make_image(data, true, 0);
    stq_be_p(data + 0xb8, UINT64_MAX);
    reject(data, size, sizeof(plain), "stored length");
    size = make_image(data, true, 0);
    stq_be_p(data + 0xb8, 0);
    reject(data, size, sizeof(plain), "stored length");
    size = make_image(data, true, 0);
    stq_be_p(data + 0xc8, 0);
    reject(data, size, sizeof(plain), "stored payload checksum");
    size = make_image(data, true, 0);
    stq_be_p(data + 0xc0, 0);
    reject(data, size, sizeof(plain), "decoded payload checksum");

    /* A declared length either side of the member's own. */
    size = make_image(data, true, 0);
    stq_be_p(data + 0xb0, sizeof(plain) - 1);
    reject(data, size, sizeof(plain), "gzip");
    size = make_image(data, true, 0);
    stq_be_p(data + 0xb0, sizeof(plain) + 1);
    reject(data, size, sizeof(plain) + 1, "gzip");

    /* A corrupted member whose outer checksum was kept consistent. */
    size = make_image(data, true, 0);
    stored_size = ldq_be_p(data + 0xb8);
    data[HEADER_SIZE + stored_size - 8] ^= 1;
    stq_be_p(data + 0xc8, byte_sum(data + HEADER_SIZE, stored_size));
    reject(data, size, sizeof(plain), "gzip");

    /* Two concatenated members. */
    size = make_image(data, true, 0);
    stored_size = ldq_be_p(data + 0xb8);
    memcpy(data + size, data + HEADER_SIZE, stored_size);
    stq_be_p(data + 0xb8, stored_size * 2);
    stq_be_p(data + 0x50, size + stored_size);
    stq_be_p(data + 0xc8, byte_sum(data + HEADER_SIZE, stored_size * 2));
    reject(data, size + stored_size, sizeof(plain), "gzip");

    /* A member cut short by one byte. */
    size = make_image(data, true, 0);
    stq_be_p(data + 0xb8, size - HEADER_SIZE - 1);
    stq_be_p(data + 0x50, size - 1);
    stq_be_p(data + 0xc8,
             byte_sum(data + HEADER_SIZE, size - HEADER_SIZE - 1));
    reject(data, size - 1, sizeof(plain), "gzip");
}

/* Anything without the signature is a raw image, kept byte for byte. */
static void test_raw(void)
{
    g_autoptr(SGIPromImage) image = decode(plain, sizeof(plain),
                                           sizeof(plain), &error_abort);
    g_autoptr(SGIPromImage) empty = decode(NULL, 0, 0, &error_abort);

    assert_payload(image);
    g_assert_true(g_bytes_equal(image->file, image->payload));
    g_assert_cmpuint(g_bytes_get_size(empty->payload), ==, 0);
    reject(plain, sizeof(plain), sizeof(plain) - 1, "exceeds flash capacity");
}

static void test_files(void)
{
    uint8_t data[BUFFER_SIZE];
    size_t size;
    g_autofree char *dir = g_dir_make_tmp("sgi-prom-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "synthetic.img", NULL);
    g_autofree uint8_t *large = NULL;
    g_autoptr(SGIPromImage) image = NULL;
    Error *err = NULL;

    g_assert_nonnull(dir);
    size = make_image(data, true, 6);
    g_assert_true(g_file_set_contents(path, (const char *)data, size, NULL));
    image = sgi_prom_image_load(path, sizeof(plain), &error_abort);
    assert_payload(image);
    g_clear_pointer(&image, sgi_prom_image_free);

    /* A decoder refusal names the file. */
    image = sgi_prom_image_load(path, sizeof(plain) - 1, &err);
    g_assert_null(image);
    g_assert_nonnull(strstr(error_get_pretty(err), path));
    g_assert_nonnull(strstr(error_get_pretty(err), "exceeds flash capacity"));
    error_free(err);
    err = NULL;

    /* A file no encoding could fit is refused before it is read. */
    large = g_malloc0(BUFFER_SIZE);
    g_assert_true(g_file_set_contents(path, (const char *)large, BUFFER_SIZE,
                                      NULL));
    image = sgi_prom_image_load(path, 1, &err);
    g_assert_null(image);
    g_assert_nonnull(strstr(error_get_pretty(err), "exceeds flash capacity"));
    /* The decoder's own refusal would not say this. */
    g_assert_nonnull(strstr(error_get_pretty(err), "no encoding of a"));
    error_free(err);
    err = NULL;

    g_assert_cmpint(g_unlink(path), ==, 0);
    image = sgi_prom_image_load(path, sizeof(plain), &err);
    g_assert_null(image);
    g_assert_nonnull(strstr(error_get_pretty(err), "cannot read"));
    error_free(err);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-prom-image/wrapped", test_wrapped);
    g_test_add_func("/sgi-prom-image/malformed", test_malformed);
    g_test_add_func("/sgi-prom-image/truncated", test_truncated);
    g_test_add_func("/sgi-prom-image/gzip", test_gzip);
    g_test_add_func("/sgi-prom-image/raw", test_raw);
    g_test_add_func("/sgi-prom-image/files", test_files);
    return g_test_run();
}
