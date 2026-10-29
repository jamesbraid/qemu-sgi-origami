/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Baseline: 82f35fa792268c7c0483e74abf592ec1ebf848e2.
 * Frozen sampler/math bodies use renamed symbols and relocated includes.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/mips/sgi/crime-rational.h"
#include "crime-rational-82-reference.h"
#include "crime-texture-82-reference.h"
#define CrimeTexture Frozen82Texture
#define crime_texture_new frozen82_texture_new
#define crime_texture_sample frozen82_texture_sample
#define crime_texture_free frozen82_texture_free
#define crime_texture_test_work frozen82_texture_test_work
#define crime_gcd128 frozen82_gcd128
#define crime_gcd64 frozen82_gcd64
#define crime_integer frozen82_integer
#define crime_integer_add frozen82_integer_add
#define crime_integer_bits frozen82_integer_bits
#define crime_integer_compare frozen82_integer_compare
#define crime_integer_divide frozen82_integer_divide
#define crime_integer_from128 frozen82_integer_from128
#define crime_integer_gcd frozen82_integer_gcd
#define crime_integer_low64 frozen82_integer_low64
#define crime_integer_multiply frozen82_integer_multiply
#define crime_integer_quotient frozen82_integer_quotient
#define crime_integer_shift frozen82_integer_shift
#define crime_integer_subtract frozen82_integer_subtract
#define crime_integer_words frozen82_integer_words
#define crime_product64 frozen82_product64
#define crime_rational frozen82_rational
#define crime_rational_add frozen82_rational_add
#define crime_rational_add64 frozen82_rational_add64
#define crime_rational_compare frozen82_rational_compare
#define crime_rational_equal_denominator frozen82_rational_equal_denominator
#define crime_rational_floor frozen82_rational_floor
#define crime_rational_is64 frozen82_rational_is64
#define crime_rational_multiply frozen82_rational_multiply
#define crime_rational_multiply64 frozen82_rational_multiply64
#define crime_rational_reduce frozen82_rational_reduce
#define crime_rational_subtract frozen82_rational_subtract
#define crime_rational_valid frozen82_rational_valid
#include "crime-texture-82-body.h"

#ifdef CRIME_TEXTURE_TEST
bool frozen82_texture_test_environment(Frozen82Texture *t,
                                       const CrimeRational *texel,
                                       uint32_t *color)
{
    return crime_texture_environment(t, texel, color);
}
#endif
