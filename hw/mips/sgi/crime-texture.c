/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/int128.h"
#include "crime-rational.h"
#include "crime-texture.h"

/* Retain sampled bytes before destination effects. */
typedef struct CrimeTexturePage {
    uint64_t identity;
    uint8_t *data, *present;
} CrimeTexturePage;

struct CrimeTexture {
    uint64_t tlb[28];
    CrimeTexturePage pages[112];
    unsigned page_count, snapshot_bytes, taps, fragments, bytes, type, function;
    uint32_t environment, border;
    int64_t plane[3][3];
    int anchor[2];
    unsigned u, v, largest, flags;
    bool point;
    uint32_t mode;
    CrimeTextureRead read;
    void *opaque;
};

static bool crime_texture_read(CrimeTexture *t, unsigned entry, unsigned offset,
                                uint32_t *value)
{
    uint16_t raw = t->tlb[entry / 4] >> (48 - 16 * (entry % 4));
    uint8_t data[4];
    uint64_t identity;
    CrimeTexturePage *page = NULL;
    unsigned local;

    if (!(raw & 0x8000) ||
        !t->read(t->opaque, ((uint64_t)(raw & 0x7fff) << 16) + offset,
                 t->bytes, &identity, data)) {
        return false;
    }
    local = identity & 65535;
    if (local + t->bytes > 65536) {
        return false;
    }
    for (unsigned i = 0; i < t->page_count; i++) {
        if (t->pages[i].identity == (identity >> 16)) {
            page = &t->pages[i];
            break;
        }
    }
    if (!page) {
        if (t->page_count == ARRAY_SIZE(t->pages)) {
            return false;
        }
        page = &t->pages[t->page_count++];
        page->identity = identity >> 16;
        page->data = g_try_malloc(65536);
        page->present = g_try_malloc0(8192);
        if (!page->data || !page->present) {
            return false;
        }
    }
    for (unsigned b = 0; b < t->bytes; b++) {
        unsigned bit = local + b;

        if (!(page->present[bit / 8] & (1U << (bit % 8)))) {
            if (++t->snapshot_bytes > CRIME_TEXTURE_MAX_SNAPSHOT) {
                return false;
            }
            page->data[bit] = data[b];
            page->present[bit / 8] |= 1U << (bit % 8);
        }
        data[b] = page->data[bit];
    }
    *value = t->bytes == 4 ? ldl_be_p(data) : lduw_be_p(data);
    return true;
}

CrimeTexture *crime_texture_new(const uint64_t *p, const uint64_t *tlb,
                                int anchor_x, int anchor_y, bool point,
                                CrimeTextureRead read, void *opaque)
{
    CrimeTexture *t;

    uint32_t mode = p[0x110 / 4];
    unsigned type = (mode >> 20) & 15, depth = (mode >> 18) & 3;
    bool format = ((type == 1 || type == 2) &&
                   (depth == 1 || depth == 2)) ||
                  ((type == 4 || (type >= 5 && type <= 8)) && depth == 1);

    uint64_t format_word = p[0x118 / 4];
    unsigned us = (format_word >> 44) & 15;
    unsigned vs = (format_word >> 40) & 15;
    unsigned u, v, d;

    if (us > 10 || vs > 10) {
        return NULL;
    }
    u = 10 - us;
    v = 10 - vs;
    d = MAX(u, v);
    if (!format || ((mode & 3) == 1 && type >= 5) ||
        ((format_word >> 36) & 15) != u ||
        ((format_word >> 32) & 15) != v ||
        ((format_word >> 16) & 65535) != (1U << (u + 4)) - 1 ||
        (format_word & 65535) != (1U << (v + 4)) - 1 ||
        ((mode >> 14) & 15) != 10 - d || ((mode >> 10) & 15) != d ||
        ((mode >> 7) & 7) > 5 ||
        (p[0x134 / 4] & ~3U)) {
        return NULL;
    }
    if (point) {
        const unsigned derivatives[] = {0x138, 0x140, 0x148, 0x150,
                                         0x158, 0x15c};
        for (unsigned i = 0; i < ARRAY_SIZE(derivatives); i++) {
            if (p[derivatives[i] / 4]) {
                return NULL;
            }
        }
    }
    t = g_try_new0(CrimeTexture, 1);
    if (t) {
        memcpy(t->tlb, tlb, sizeof(t->tlb));
        t->mode = mode;
        t->type = type;
        t->bytes = 1U << depth;
        t->function = mode & 3;
        t->environment = p[0x168 / 4];
        t->border = p[0x160 / 4];
        t->u = u;
        t->v = v;
        t->largest = d;
        t->flags = p[0x134 / 4];
        t->point = point;
        t->anchor[0] = anchor_x;
        t->anchor[1] = anchor_y;
        t->plane[0][0] = p[0x120 / 4];
        t->plane[0][1] = p[0x138 / 4];
        t->plane[0][2] = p[0x140 / 4];
        t->plane[1][0] = p[0x128 / 4];
        t->plane[1][1] = p[0x148 / 4];
        t->plane[1][2] = p[0x150 / 4];
        t->plane[2][0] = (int32_t)p[0x130 / 4];
        t->plane[2][1] = (int32_t)p[0x158 / 4];
        t->plane[2][2] = (int32_t)p[0x15c / 4];
        t->read = read;
        t->opaque = opaque;
    }
    return t;
}

static void crime_texture_components(const CrimeTexture *t, uint32_t raw,
                                      CrimeRational *components)
{
    for (unsigned c = 0; c < 4; c++) {
        unsigned value, denominator;
        if (t->type == 4) {
            value = (raw >> (12 - 4 * c)) & 15;
            denominator = 15;
        } else if (t->type >= 5) {
            value = (t->type == 6 || c != 3) ? (raw >> 8) & 255 : raw & 255;
            denominator = 255;
        } else if (t->bytes == 4) {
            value = (raw >> (24 - 8 * c)) & 255;
            denominator = 255;
        } else {
            value = c == 3 ? (raw >> 15) & 1 : (raw >> (10 - 5 * c)) & 31;
            denominator = c == 3 ? 1 : 31;
        }
        components[c] = crime_rational(value, denominator);
    }
}

static bool crime_texture_environment(const CrimeTexture *t,
                                       const CrimeRational *texel,
                                       uint32_t *color)
{
    CrimeRational one = crime_rational(1, 1), zero = crime_rational(0, 1);
    CrimeRational scale = crime_rational(255, 1), half = crime_rational(1, 2);
    uint32_t output = 0;

    for (unsigned c = 0; c < 4; c++) {
        unsigned shift = 24 - 8 * c;
        CrimeRational f = crime_rational((*color >> shift) & 255, 255);
        CrimeRational e = crime_rational((t->environment >> shift) & 255, 255);
        CrimeRational result, inverse, left, right;
        bool present = c == 3 ? t->type != 1 && t->type != 7 : t->type != 5;
        int64_t byte;

        if (!present || (t->function == 1 && c == 3)) {
            result = f;
        } else if (t->function == 3 ||
                   (t->function == 1 && t->type == 1)) {
            result = texel[c];
        } else if (t->function == 0 ||
                   (t->function == 2 && c == 3 && t->type != 6)) {
            if (!crime_rational_multiply(&result, &f, &texel[c])) {
                return false;
            }
        } else {
            const CrimeRational *weight = &texel[t->function == 1 ? 3 : c];
            const CrimeRational *second = t->function == 1 ? &texel[c] : &e;
            if (!crime_rational_subtract(&inverse, &one, weight) ||
                !crime_rational_multiply(&left, &f, &inverse) ||
                !crime_rational_multiply(&right, second, weight) ||
                !crime_rational_add(&result, &left, &right)) {
                return false;
            }
        }
        if (crime_rational_compare(&result, &zero) < 0) {
            result = zero;
        } else if (crime_rational_compare(&result, &one) > 0) {
            result = one;
        }
        if (!crime_rational_multiply(&result, &result, &scale) ||
            !crime_rational_add(&result, &result, &half) ||
            !crime_rational_floor(&result, &byte)) {
            return false;
        }
        output |= (uint32_t)byte << shift;
    }
    *color = output;
    return true;
}

static Int128 crime_texture_product(int64_t a, int64_t b)
{
    uint64_t low, high;
    muls64(&low, &high, a, b);
    return int128_make128(low, high);
}

static bool crime_texture_coordinates(const CrimeTexture *t, int x, int y,
                                       CrimeRational *coordinate)
{
    int64_t extracted[3];
    CrimeRational one = crime_rational(1, 1);

    for (unsigned i = 0; i < 3; i++) {
        Int128 value = int128_makes64(t->plane[i][0]);
        int64_t lo = i == 2 ? INT32_MIN : INT64_MIN;
        int64_t hi = i == 2 ? INT32_MAX : INT64_MAX;

        if (!t->point) {
            value = int128_add(value, crime_texture_product(
                t->plane[i][1], (int64_t)x - t->anchor[0]));
            value = int128_add(value, crime_texture_product(
                t->plane[i][2], (int64_t)y - t->anchor[1]));
        }
        if (int128_lt(value, int128_makes64(lo)) ||
            int128_gt(value, int128_makes64(hi))) {
            return false;
        }
        extracted[i] = int128_getlo(int128_rshift(value, 12));
    }
    if (!extracted[2]) {
        return false;
    }
    for (unsigned i = 0; i < 2; i++) {
        int64_t n = extracted[2] < 0 ? -extracted[i] : extracted[i];
        uint64_t d = extracted[2] < 0 ? -extracted[2] : extracted[2];
        CrimeRational quotient = crime_rational(n, d);
        CrimeRational scale = crime_rational(1,
            1U << (14 - 4 * ((t->flags >> i) & 1)));

        if (!crime_rational_multiply(&coordinate[i], &quotient, &scale) ||
            !crime_rational_subtract(&coordinate[i], &coordinate[i], &one)) {
            return false;
        }
    }
    return true;
}

static bool crime_texture_wrap(CrimeRational *value, unsigned size,
                                unsigned mode)
{
    CrimeRational zero = crime_rational(0, 1), one = crime_rational(1, 1);
    CrimeRational half_texel = crime_rational(1, 2 * size), lo, hi;
    int64_t whole;

    if (mode == 1) {
        if (!crime_rational_floor(value, &whole)) {
            return false;
        }
        lo = crime_rational(whole, 1);
        return crime_rational_subtract(value, value, &lo);
    }
    lo = zero;
    hi = one;
    if (mode == 2) {
        lo = half_texel;
        if (!crime_rational_subtract(&hi, &one, &half_texel)) {
            return false;
        }
    } else if (mode == 3) {
        lo = half_texel;
        lo.negative = true;
        if (!crime_rational_add(&hi, &one, &half_texel)) {
            return false;
        }
    }
    if (crime_rational_compare(value, &lo) < 0) {
        *value = lo;
    } else if (crime_rational_compare(value, &hi) > 0) {
        *value = hi;
    }
    return true;
}

static void crime_texture_border(const CrimeTexture *t, CrimeRational *value)
{
    for (unsigned c = 0; c < 4; c++) {
        unsigned shift = 24 - 8 * c;
        if (t->type == 6 || (c != 3 && (t->type == 7 || t->type == 8))) {
            shift = 24;
        }
        value[c] = crime_rational((t->border >> shift) & 255, 255);
    }
}

static bool crime_texture_tap(CrimeTexture *t, unsigned level, int64_t x,
                               int64_t y, CrimeRational *value)
{
    static const unsigned first[] = {93, 93, 93, 93, 92, 93,
                                     92, 88, 80, 64, 0};
    static const unsigned packed[] = {0xfc00, 0xf800, 0xf000, 0xe000,
                                      0xc000, 0x8000, 0, 0, 0, 0, 0};
    unsigned width = 1U << MAX((int)t->u - (int)level, 0);
    unsigned height = 1U << MAX((int)t->v - (int)level, 0);
    unsigned d = t->largest - level;
    unsigned wrap_x = (t->mode >> 4) & 3, wrap_y = (t->mode >> 2) & 3;
    unsigned byte_x, entry, local_x, local_y, offset;
    uint32_t raw;

    if (++t->taps > CRIME_TEXTURE_MAX_TAPS) {
        return false;
    }
    if (wrap_x == 1) {
        x = (x % width + width) % width;
    }
    if (wrap_y == 1) {
        y = (y % height + height) % height;
    }
    if (x < 0 || x >= width || y < 0 || y >= height) {
        crime_texture_border(t, value);
        return true;
    }
    byte_x = x * t->bytes;
    entry = first[d] + (y / 128) * (d == 10 ? 8 : 4) + byte_x / 512;
    local_x = byte_x % 512;
    local_y = y % 128;
    if (t->mode & (1U << 24)) {
        offset = ((local_y & 0x4c) << 9) + ((local_y & 0x30) << 5) +
                 ((local_y & 3) << 3) + ((local_x & 0x180) << 6) +
                 ((local_x & 0x78) << 2) + (local_x & 7);
    } else {
        offset = local_y * 512 + local_x;
    }
    offset += packed[d];
    if (entry >= 112 || offset + t->bytes > 65536 ||
        !crime_texture_read(t, entry, offset, &raw)) {
        return false;
    }
    crime_texture_components(t, raw, value);
    return true;
}

static Int128 crime_texture_weight_product(uint64_t a, uint64_t b)
{
    uint64_t low, high;
    mulu64(&low, &high, a, b);
    return int128_make128(low, high);
}

static Int128 crime_texture_weight_scale(Int128 value, uint64_t scale)
{
    uint64_t low, carry;
    mulu64(&low, &carry, int128_getlo(value), scale);
    return int128_make128(low, (uint64_t)int128_gethi(value) * scale + carry);
}

static bool crime_texture_linear128(CrimeTexture *t, unsigned level,
                                     const int64_t index[2],
                                     CrimeRational weights[2][2],
                                     CrimeRational *result)
{
    /* lcm(1, 15, 31, 255) covers every admitted component and border. */
    const uint64_t component_one = 7905;
    uint64_t denominator[2], numerator[2], weight[2][2];
    Int128 sum[4] = { 0 }, common;

    for (unsigned axis = 0; axis < 2; axis++) {
        const CrimeRational *w = &weights[axis][1];
        denominator[axis] = (uint64_t)w->denominator.word[1] << 32 |
                            w->denominator.word[0];
        numerator[axis] = (uint64_t)w->numerator.word[1] << 32 |
                          w->numerator.word[0];
        weight[axis][0] = denominator[axis] - numerator[axis];
        weight[axis][1] = numerator[axis];
    }
    common = crime_texture_weight_scale(crime_texture_weight_product(
        denominator[0], denominator[1]), component_one);
    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            CrimeRational tap[4];
            Int128 w = crime_texture_weight_product(weight[0][col],
                                                     weight[1][row]);
            if (!int128_nz(w)) {
                continue;
            }
            if (!crime_texture_tap(t, level, index[0] + col, index[1] + row,
                                   tap)) {
                return false;
            }
            for (unsigned c = 0; c < 4; c++) {
                unsigned n = tap[c].numerator.word[0];
                unsigned d = tap[c].denominator.word[0];
                g_assert(d && !(component_one % d) && n <= d);
                sum[c] = int128_add(sum[c], crime_texture_weight_scale(
                    w, n * (component_one / d)));
            }
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        Int128 divisor = crime_gcd128(sum[c], common);
        result[c] = (CrimeRational) {
            .numerator = crime_integer_from128(int128_divu(sum[c], divisor)),
            .denominator = crime_integer_from128(int128_divu(common, divisor)),
        };
    }
    return true;
}

static bool crime_texture_level(CrimeTexture *t,
                                 const CrimeRational *coordinate,
                                 unsigned level, bool linear,
                                 CrimeRational *result)
{
    CrimeRational weights[2][2];
    CrimeRational one = crime_rational(1, 1), half = crime_rational(1, 2);
    int64_t index[2];

    for (unsigned axis = 0; axis < 2; axis++) {
        unsigned log = axis ? t->v : t->u;
        unsigned size = 1U << MAX((int)log - (int)level, 0);
        unsigned wrap = (t->mode >> (axis ? 2 : 4)) & 3;
        CrimeRational value = coordinate[axis], scale = crime_rational(size, 1);
        CrimeRational integer;

        if (!crime_texture_wrap(&value, size, wrap) ||
            !crime_rational_multiply(&value, &value, &scale) ||
            (linear && !crime_rational_subtract(&value, &value, &half)) ||
            !crime_rational_floor(&value, &index[axis])) {
            return false;
        }
        integer = crime_rational(index[axis], 1);
        if (!crime_rational_subtract(&weights[axis][1], &value, &integer) ||
            !crime_rational_subtract(&weights[axis][0], &one,
                                     &weights[axis][1])) {
            return false;
        }
        if (!linear && wrap != 1 && index[axis] == size &&
            !crime_integer_bits(&weights[axis][1].numerator)) {
            index[axis]--;
        }
    }
    if (!linear) {
        /* A normalized tap times one plus zero has no arithmetic failure. */
        return crime_texture_tap(t, level, index[0], index[1], result);
    }
    {
        unsigned du = crime_integer_bits(&weights[0][1].denominator);
        unsigned dv = crime_integer_bits(&weights[1][1].denominator);

        /*
         * Every old reduced denominator divides D=7905*Du*Dv. Convex
         * component products and positive partial sums are at most D.
         * D<2^127 proves all old signed512 checks pass, so native exact
         * accumulation preserves failures as well as values. Decide
         * before taps: no retry, prefetch or changed validation order.
         */
        if (du <= 64 && dv <= 64 && du + dv + 13 <= 127) {
            return crime_texture_linear128(t, level, index, weights, result);
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        result[c] = crime_rational(0, 1);
    }
    for (unsigned row = 0; row < (linear ? 2 : 1); row++) {
        for (unsigned col = 0; col < (linear ? 2 : 1); col++) {
            CrimeRational tap[4], weight = one;
            if (linear &&
                !crime_rational_multiply(&weight, &weights[0][col],
                                         &weights[1][row])) {
                return false;
            }
            if (!crime_integer_bits(&weight.numerator)) {
                continue;
            }
            if (!crime_texture_tap(t, level, index[0] + col, index[1] + row,
                                   tap)) {
                return false;
            }
            for (unsigned c = 0; c < 4; c++) {
                if (!crime_rational_multiply(&tap[c], &tap[c], &weight) ||
                    !crime_rational_add(&result[c], &result[c], &tap[c])) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool crime_texture_footprint(const CrimeTexture *t, int x, int y,
                                     const CrimeRational *coordinate,
                                     CrimeRational *squared)
{
    CrimeRational next[2], delta[2], candidate, square;
    *squared = crime_rational(0, 1);
    if (t->point) {
        *squared = crime_rational(1, 1);
        return true;
    }
    for (unsigned axis = 0; axis < 2; axis++) {
        if (!crime_texture_coordinates(t, x + !axis, y + !!axis, next)) {
            return false;
        }
        candidate = crime_rational(0, 1);
        for (unsigned component = 0; component < 2; component++) {
            CrimeRational size = crime_rational(
                1U << (component ? t->v : t->u), 1);
            if (!crime_rational_subtract(&delta[component], &next[component],
                                         &coordinate[component]) ||
                !crime_rational_multiply(&delta[component],
                                         &delta[component], &size) ||
                !crime_rational_multiply(&square, &delta[component],
                                         &delta[component]) ||
                !crime_rational_add(&candidate, &candidate, &square)) {
                return false;
            }
        }
        if (crime_rational_compare(&candidate, squared) > 0) {
            *squared = candidate;
        }
    }
    return true;
}

static bool crime_texture_lod(const CrimeTexture *t,
                               const CrimeRational *squared, int *lod)
{
    CrimeInteger shifted, one = crime_integer(1);
    CrimeRational scale = crime_rational(1, 1), normalized;
    int exponent, shift;
    int64_t mantissa;
    unsigned fraction = 0;

    if (!crime_integer_bits(&squared->numerator)) {
        *lod = INT_MIN / 2;
        return true;
    }
    exponent = (int)crime_integer_bits(&squared->numerator) -
                   crime_integer_bits(&squared->denominator);
    if (exponent >= 0) {
        if (!crime_integer_shift(&shifted, &squared->denominator, exponent)) {
            return false;
        }
        exponent -= crime_integer_compare(&squared->numerator, &shifted) < 0;
    } else {
        if (!crime_integer_shift(&shifted, &squared->numerator, -exponent)) {
            return false;
        }
        exponent -= crime_integer_compare(&shifted, &squared->denominator) < 0;
    }
    if (exponent > 2 * ((int)t->largest + 1)) {
        *lod = 256 * (t->largest + 1);
        return true;
    }
    shift = 31 - exponent;
    if (shift >= 0) {
        if (!crime_integer_shift(&scale.numerator, &one, shift)) {
            return false;
        }
    } else if (!crime_integer_shift(&scale.denominator, &one, -shift)) {
        return false;
    }
    if (!crime_rational_valid(&scale) ||
        !crime_rational_multiply(&normalized, squared, &scale) ||
        !crime_rational_floor(&normalized, &mantissa)) {
        return false;
    }
    for (unsigned i = 0; i < 7; i++) {
        uint64_t next = (uint64_t)mantissa * mantissa >> 31;
        fraction <<= 1;
        if (next >= (UINT64_C(1) << 32)) {
            next >>= 1;
            fraction |= 1;
        }
        mantissa = next;
    }
    *lod = 128 * exponent + (int)fraction;
    return true;
}

bool crime_texture_sample(CrimeTexture *t, int x, int y, uint32_t *color)
{
    CrimeRational texel[4], next_texel[4], coordinate[2], squared;
    unsigned minimum = (t->mode >> 7) & 7, magnification = (t->mode >> 6) & 1;
    unsigned level = 0, next = 0, weight = 0;
    bool linear;
    int lod, transition = magnification &&
                         (minimum == 2 || minimum == 4) ? 128 : 0;

    if (++t->fragments > CRIME_TEXTURE_MAX_FRAGMENTS ||
        !crime_texture_coordinates(t, x, y, coordinate) ||
        !crime_texture_footprint(t, x, y, coordinate, &squared) ||
        !crime_texture_lod(t, &squared, &lod)) {
        return false;
    }
    if (lod <= transition) {
        linear = magnification;
    } else if (minimum < 2) {
        linear = minimum == 1;
    } else if (minimum == 2 || minimum == 3) {
        level = lod <= 128 ? 0 : MIN((lod + 127) / 256, t->largest);
        next = level;
        linear = minimum == 3;
    } else {
        lod = MAX(lod, 0);
        level = MIN(lod / 256, t->largest);
        next = MIN(level + 1, t->largest);
        weight = next == level ? 0 : lod % 256;
        linear = minimum == 5;
    }
    if (!crime_texture_level(t, coordinate, level, linear, texel)) {
        return false;
    }
    if (weight) {
        CrimeRational w = crime_rational(weight, 256);
        CrimeRational inverse = crime_rational(256 - weight, 256);
        if (!crime_texture_level(t, coordinate, next, linear, next_texel)) {
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (!crime_rational_multiply(&texel[c], &texel[c], &inverse) ||
                !crime_rational_multiply(&next_texel[c], &next_texel[c], &w) ||
                !crime_rational_add(&texel[c], &texel[c], &next_texel[c])) {
                return false;
            }
        }
    }
    return crime_texture_environment(t, texel, color);
}

void crime_texture_free(CrimeTexture *t)
{
    if (t) {
        for (unsigned i = 0; i < t->page_count; i++) {
            g_free(t->pages[i].data);
            g_free(t->pages[i].present);
        }
        g_free(t);
    }
}

#ifdef CRIME_TEXTURE_TEST
void crime_texture_test_work(CrimeTexture *t, unsigned fragments,
                             unsigned taps, unsigned snapshot_bytes)
{
    t->fragments = fragments;
    t->taps = taps;
    t->snapshot_bytes = snapshot_bytes;
}
#endif
