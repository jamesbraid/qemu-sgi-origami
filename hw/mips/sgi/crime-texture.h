/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_CRIME_TEXTURE_H
#define HW_MIPS_SGI_CRIME_TEXTURE_H

#define CRIME_TEXTURE_MAX_FRAGMENTS 4194304U
#define CRIME_TEXTURE_MAX_TAPS 33554432U
#define CRIME_TEXTURE_MAX_SNAPSHOT 7340032U

typedef struct CrimeTexture CrimeTexture;
typedef bool (*CrimeTextureRead)(void *opaque, uint64_t address, unsigned size,
                                 uint64_t *identity, uint8_t *data);

CrimeTexture *crime_texture_new(const uint64_t *parameter, const uint64_t *tlb,
                                int anchor_x, int anchor_y, bool point,
                                CrimeTextureRead read, void *opaque);
bool crime_texture_sample(CrimeTexture *texture, int x, int y, uint32_t *color);
void crime_texture_free(CrimeTexture *texture);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CrimeTexture, crime_texture_free)

#ifdef CRIME_TEXTURE_TEST
/* Unit-only boundary setup; absent from the device build. */
void crime_texture_test_work(CrimeTexture *texture, unsigned fragments,
                             unsigned taps, unsigned snapshot_bytes);
#endif

#endif
