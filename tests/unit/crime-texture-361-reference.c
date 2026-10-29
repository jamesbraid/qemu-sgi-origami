/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Frozen361 sampler and arithmetic for complete old/new comparisons. */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "hw/mips/sgi/crime-rational.h"
#include "crime-rational-361-reference.h"
#include "crime-texture-361-reference.h"
#define CrimeTexture PreviousTexture
#define crime_texture_new previous_texture_new
#define crime_texture_sample previous_texture_sample
#define crime_texture_free previous_texture_free
#define crime_texture_test_work previous_texture_test_work
#define crime_integer previous_integer
#define crime_integer_bits previous_integer_bits
#define crime_integer_compare previous_integer_compare
#define crime_integer_shift previous_integer_shift
#define crime_rational previous_rational
#define crime_rational_add previous_rational_add
#define crime_rational_compare previous_rational_compare
#define crime_rational_floor previous_rational_floor
#define crime_rational_multiply previous_rational_multiply
#define crime_rational_subtract previous_rational_subtract
#define crime_rational_valid previous_rational_valid
#include "crime-texture-361-body.h"
