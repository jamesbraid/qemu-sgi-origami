/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef TESTS_CRIME_TEXTURE_361_REFERENCE_H
#define TESTS_CRIME_TEXTURE_361_REFERENCE_H
#include "hw/mips/sgi/crime-texture.h"
typedef struct PreviousTexture PreviousTexture;
PreviousTexture *previous_texture_new(const uint64_t *p, const uint64_t *tlb,
                                      int x, int y, bool point,
                                      CrimeTextureRead read, void *opaque);
bool previous_texture_sample(PreviousTexture *t, int x, int y, uint32_t *color);
void previous_texture_free(PreviousTexture *t);
#ifdef CRIME_TEXTURE_TEST
void previous_texture_test_work(PreviousTexture *t, unsigned fragments,
                                unsigned taps, unsigned bytes);
#endif
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PreviousTexture, previous_texture_free)
#endif
