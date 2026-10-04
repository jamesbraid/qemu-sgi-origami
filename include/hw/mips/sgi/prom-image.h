/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI PROM image decoder: accepts a raw PROM image or SGI's version-6
 * single-segment distribution container, uncompressed or gzip. See
 * docs/specs/sgi-prom-image.rst for the accepted container subset.
 *
 * Copyright (c) 2026 James Braid
 */

#ifndef HW_MIPS_SGI_PROM_IMAGE_H
#define HW_MIPS_SGI_PROM_IMAGE_H

#include "qapi/error.h"

typedef struct SGIPromImage {
    /* The bytes as supplied, container header included. */
    GBytes *file;
    /* The decoded payload; the same bytes as @file for a raw image. */
    GBytes *payload;
} SGIPromImage;

void sgi_prom_image_free(SGIPromImage *image);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SGIPromImage, sgi_prom_image_free)

/*
 * Decode @file, refusing a payload longer than @payload_limit bytes:
 * the capacity of whatever the caller copies it into.
 */
SGIPromImage *sgi_prom_image_decode(GBytes *file, size_t payload_limit,
                                    Error **errp);

/*
 * Read and decode @path. A file too large to hold a @payload_limit-byte
 * payload in any accepted encoding is refused before it is read.
 */
SGIPromImage *sgi_prom_image_load(const char *path, size_t payload_limit,
                                  Error **errp);

#endif /* HW_MIPS_SGI_PROM_IMAGE_H */
