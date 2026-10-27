/*
 * Synthetic SGI PROM distribution containers for machine tests.
 *
 * Wraps a test's own payload in the uncompressed single-segment container
 * docs/system/mips/sgi/prom-image.rst describes, so a test can prove a machine
 * input takes the container and not only a raw image. The layout is
 * repeated here rather than taken from the decoder, so a decoder that got
 * it wrong would not agree with itself. No SGI firmware bytes are used.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_PROM_CONTAINER_H
#define SGI_PROM_CONTAINER_H

#include "qemu/bswap.h"

#define SGI_PROM_CONTAINER_HEADER 4096

/* The container around @payload; *@size receives its length. */
static inline uint8_t *sgi_prom_container_wrap(const uint8_t *payload,
                                               size_t payload_size,
                                               size_t *size)
{
    uint8_t *data;
    uint64_t sum = 0;
    size_t i;

    *size = SGI_PROM_CONTAINER_HEADER + payload_size;
    data = g_malloc0(*size);
    memcpy(data + SGI_PROM_CONTAINER_HEADER, payload, payload_size);
    for (i = 0; i < payload_size; i++) {
        sum += payload[i];
    }
    stq_be_p(data + 0x40, 0x4a464b535743534dULL);     /* signature */
    stq_be_p(data + 0x48, 6);                         /* version */
    stq_be_p(data + 0x50, *size);                     /* file end */
    stl_be_p(data + 0x7c, 1);                         /* segments */
    stq_be_p(data + 0x90, 0x10);                      /* stored */
    stq_be_p(data + 0x98, SGI_PROM_CONTAINER_HEADER); /* payload offset */
    stq_be_p(data + 0xb0, payload_size);              /* decoded length */
    stq_be_p(data + 0xc0, sum);                       /* decoded sum */
    stq_be_p(data + 0xd0, payload_size);              /* storage extent */
    return data;
}

/* Write @payload's container to a new temporary file and return its path. */
static inline char *sgi_prom_container_write(const uint8_t *payload,
                                             size_t payload_size,
                                             const char *template)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *data = NULL;
    char *path = NULL;
    size_t size;
    int fd;

    data = sgi_prom_container_wrap(payload, payload_size, &size);
    fd = g_file_open_tmp(template, &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_true(g_file_set_contents(path, (const char *)data, size,
                                      &error));
    g_assert_no_error(error);
    return path;
}

#endif /* SGI_PROM_CONTAINER_H */
