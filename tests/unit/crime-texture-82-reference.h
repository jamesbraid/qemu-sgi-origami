/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef TESTS_CRIME_TEXTURE_82_REFERENCE_H
#define TESTS_CRIME_TEXTURE_82_REFERENCE_H
#include "hw/mips/sgi/crime-texture.h"
typedef struct Frozen82Texture Frozen82Texture;
Frozen82Texture *frozen82_texture_new(const uint64_t *p, const uint64_t *tlb,
                                      int x, int y, bool point,
                                      CrimeTextureRead read, void *opaque);
bool frozen82_texture_sample(Frozen82Texture *t, int x, int y, uint32_t *color);
void frozen82_texture_free(Frozen82Texture *t);
#ifdef CRIME_TEXTURE_TEST
struct CrimeRational;
bool frozen82_texture_test_environment(Frozen82Texture *t,
                                       const struct CrimeRational *texel,
                                       uint32_t *color);
void frozen82_texture_test_work(Frozen82Texture *t, unsigned fragments,
                                unsigned taps, unsigned bytes);
#endif
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Frozen82Texture, frozen82_texture_free)
#endif
