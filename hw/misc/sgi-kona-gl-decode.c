/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality (KONA) OpenGL doorbell command decoder.
 *
 * The doorbell takes selector-first packets. The table lists the decoded
 * subset and marks each layout established, inferred, or unresolved. The
 * caller splits 64-bit stores into big-endian words before decoding.
 *
 * No doorbell kick or length register is modeled. The declared length is
 * available for transports that provide one.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/misc/sgi-kona-gl-decode.h"

#include "sgi-kona-gl-selectors.c.inc"

const SGIKonaGLSelectorInfo *sgi_kona_gl_selector_table(size_t *count)
{
    *count = ARRAY_SIZE(sgi_kona_gl_selectors);
    return sgi_kona_gl_selectors;
}

const SGIKonaGLSelectorInfo *sgi_kona_gl_lookup(uint32_t selector)
{
    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_gl_selectors); i++) {
        if (sgi_kona_gl_selectors[i].selector == selector) {
            return &sgi_kona_gl_selectors[i];
        }
    }
    return NULL;
}

static bool arg0_expected(const SGIKonaGLSelectorInfo *info, uint32_t value)
{
    if (!info->num_arg0_values) {
        return true;
    }
    for (unsigned i = 0; i < info->num_arg0_values; i++) {
        if (info->arg0_values[i] == value) {
            return true;
        }
    }
    return false;
}

SGIKonaGLDecodeResult sgi_kona_gl_decode(const uint32_t *words, size_t nwords,
                                         size_t declared_len,
                                         SGIKonaGLCommand *cmd)
{
    const SGIKonaGLSelectorInfo *info;
    unsigned length;

    memset(cmd, 0, sizeof(*cmd));
    if (!nwords) {
        cmd->length = 1;
        return SGI_KONA_GL_DECODE_TRUNCATED;
    }

    cmd->selector = words[0];
    cmd->length = 1;
    info = sgi_kona_gl_lookup(words[0]);
    cmd->info = info;
    if (!info) {
        return SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR;
    }
    if (info->evidence == SGI_KONA_GL_LAYOUT_UNRESOLVED) {
        return SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT;
    }

    assert(info->argc <= SGI_KONA_GL_MAX_ARGS);
    length = 1 + info->argc;
    cmd->length = length;
    cmd->argc = info->argc;

    if (declared_len != SGI_KONA_GL_LENGTH_UNDECLARED &&
        declared_len != length) {
        return SGI_KONA_GL_DECODE_LENGTH_MISMATCH;
    }
    if (nwords < length) {
        return SGI_KONA_GL_DECODE_TRUNCATED;
    }

    memcpy(cmd->args, &words[1], info->argc * sizeof(uint32_t));
    if (info->argc && !arg0_expected(info, cmd->args[0])) {
        return SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT;
    }
    return SGI_KONA_GL_DECODE_OK;
}

const char *sgi_kona_gl_decode_result_name(SGIKonaGLDecodeResult result)
{
    static const char *const names[] = {
        [SGI_KONA_GL_DECODE_OK] = "ok",
        [SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR] = "unknown selector",
        [SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT] = "unresolved layout",
        [SGI_KONA_GL_DECODE_TRUNCATED] = "truncated",
        [SGI_KONA_GL_DECODE_LENGTH_MISMATCH] = "length mismatch",
        [SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT] = "unexpected argument",
    };

    if (result >= ARRAY_SIZE(names) || !names[result]) {
        return "invalid";
    }
    return names[result];
}

/* Cleared native query descriptors; state support is checked separately. */
static const SGIKonaGLQueryInfo sgi_kona_gl_queries[] = {
    { 0x0b10, 0x04, 1 },
    { 0x0b11, 0x03, 1 },
    { 0x0b14, 0x04, 6 },
    { 0x0b20, 0x04, 1 },
    { 0x0b21, 0x03, 1 },
    { 0x0b24, 0x04, 1 },
    { 0x0b25, 0x04, 1 },
    { 0x0b26, 0x04, 1 },
    { 0x0b40, 0x04, 2 },
    { 0x0b41, 0x04, 1 },
    { 0x0b42, 0x04, 1 },
    { 0x0b44, 0x04, 1 },
    { 0x0b45, 0x04, 1 },
    { 0x0b46, 0x04, 1 },
    { 0x0b50, 0x04, 1 },
    { 0x0b51, 0x04, 1 },
    { 0x0b52, 0x04, 1 },
    { 0x0b53, 0x13, 4 },
    { 0x0b54, 0x04, 1 },
    { 0x0b55, 0x04, 1 },
    { 0x0b56, 0x04, 1 },
    { 0x0b57, 0x04, 1 },
    { 0x0b60, 0x04, 1 },
    { 0x0b61, 0x03, 1 },
    { 0x0b62, 0x03, 1 },
    { 0x0b63, 0x03, 1 },
    { 0x0b64, 0x03, 1 },
    { 0x0b65, 0x04, 1 },
    { 0x0b66, 0x13, 4 },
    { 0x0b70, 0x12, 2 },
    { 0x0b71, 0x04, 1 },
    { 0x0b72, 0x04, 1 },
    { 0x0b73, 0x12, 1 },
    { 0x0b74, 0x04, 1 },
    { 0x0b80, 0x13, 4 },
    { 0x0b90, 0x04, 1 },
    { 0x0b91, 0x04, 1 },
    { 0x0b92, 0x04, 1 },
    { 0x0b93, 0x04, 1 },
    { 0x0b94, 0x04, 1 },
    { 0x0b95, 0x04, 1 },
    { 0x0b96, 0x04, 1 },
    { 0x0b97, 0x04, 1 },
    { 0x0b98, 0x04, 1 },
    { 0x0ba0, 0x04, 1 },
    { 0x0ba1, 0x04, 1 },
    { 0x0ba2, 0x04, 4 },
    { 0x0ba3, 0x04, 1 },
    { 0x0ba4, 0x04, 1 },
    { 0x0ba5, 0x04, 1 },
    { 0x0ba6, 0x03, 16 },
    { 0x0ba7, 0x03, 16 },
    { 0x0ba8, 0x03, 16 },
    { 0x0bb0, 0x04, 1 },
    { 0x0bc0, 0x04, 1 },
    { 0x0bc1, 0x04, 1 },
    { 0x0bc2, 0x13, 1 },
    { 0x0bd0, 0x04, 1 },
    { 0x0be0, 0x04, 1 },
    { 0x0be1, 0x04, 1 },
    { 0x0be2, 0x04, 1 },
    { 0x0bf0, 0x04, 1 },
    { 0x0bf1, 0x04, 1 },
    { 0x0bf2, 0x04, 1 },
    { 0x0c01, 0x04, 1 },
    { 0x0c02, 0x04, 1 },
    { 0x0c10, 0x04, 4 },
    { 0x0c11, 0x04, 1 },
    { 0x0c20, 0x03, 1 },
    { 0x0c21, 0x04, 1 },
    { 0x0c22, 0x13, 4 },
    { 0x0c23, 0x04, 4 },
    { 0x0c50, 0x04, 1 },
    { 0x0c51, 0x04, 1 },
    { 0x0c52, 0x04, 1 },
    { 0x0c53, 0x04, 1 },
    { 0x0c54, 0x04, 1 },
    { 0x0c60, 0x04, 1 },
    { 0x0c61, 0x04, 1 },
    { 0x0c62, 0x04, 1 },
    { 0x0c63, 0x04, 1 },
    { 0x0cb0, 0x04, 1 },
    { 0x0cb1, 0x04, 1 },
    { 0x0cb2, 0x04, 1 },
    { 0x0cb3, 0x04, 1 },
    { 0x0cb4, 0x04, 1 },
    { 0x0cb5, 0x04, 1 },
    { 0x0cb6, 0x04, 1 },
    { 0x0cb7, 0x04, 1 },
    { 0x0cb8, 0x04, 1 },
    { 0x0cb9, 0x04, 1 },
    { 0x0d70, 0x04, 1 },
    { 0x2a00, 0x04, 1 },
    { 0x2a01, 0x04, 1 },
    { 0x2a02, 0x04, 1 },
    { 0x3000, 0x04, 1 },
    { 0x3001, 0x04, 1 },
    { 0x3002, 0x04, 1 },
    { 0x3003, 0x04, 1 },
    { 0x3004, 0x04, 1 },
    { 0x3005, 0x04, 1 },
    { 0x4000, 0x04, 1 },
    { 0x4001, 0x04, 1 },
    { 0x4002, 0x04, 1 },
    { 0x4003, 0x04, 1 },
    { 0x4004, 0x04, 1 },
    { 0x4005, 0x04, 1 },
    { 0x4006, 0x04, 1 },
    { 0x4007, 0x04, 1 },
    { 0x8005, 0x13, 4 },
    { 0x8009, 0x04, 1 },
    { 0x8010, 0x04, 1 },
    { 0x8011, 0x04, 1 },
    { 0x8012, 0x04, 1 },
    { 0x8024, 0x04, 1 },
    { 0x802e, 0x04, 1 },
    { 0x8037, 0x04, 1 },
    { 0x8038, 0x03, 1 },
    { 0x8039, 0x03, 1 },
    { 0x8094, 0x04, 1 },
    { 0x809d, 0x04, 1 },
    { 0x809e, 0x04, 1 },
    { 0x809f, 0x04, 1 },
    { 0x80a0, 0x04, 1 },
    { 0x80aa, 0x03, 1 },
    { 0x80ab, 0x04, 1 },
    { 0x80ac, 0x04, 1 },
    { 0x80b1, 0x03, 16 },
    { 0x80b2, 0x04, 1 },
    { 0x80b4, 0x03, 1 },
    { 0x80b5, 0x03, 1 },
    { 0x80b6, 0x03, 1 },
    { 0x80b7, 0x03, 1 },
    { 0x80b8, 0x03, 1 },
    { 0x80b9, 0x03, 1 },
    { 0x80ba, 0x03, 1 },
    { 0x80bb, 0x03, 1 },
    { 0x80bc, 0x04, 1 },
    { 0x80d0, 0x04, 1 },
    { 0x80d1, 0x04, 1 },
    { 0x80d2, 0x04, 1 },
    { 0x8126, 0x03, 1 },
    { 0x8127, 0x03, 1 },
    { 0x8128, 0x03, 1 },
    { 0x8129, 0x03, 3 },
    { 0x812b, 0x04, 1 },
    { 0x8148, 0x00, 1 },
    { 0x8149, 0x04, 1 },
    { 0x814a, 0x03, 3 },
    { 0x814b, 0x03, 3 },
    { 0x817d, 0x04, 1 },
    { 0x817e, 0x03, 4 },
    { 0x8181, 0x04, 1 },
    { 0x8183, 0x04, 1 },
    { 0x8196, 0x04, 1 },
    { 0x8198, 0x04, 1 },
    { 0x8199, 0x03, 4 },
    { 0x81c4, 0x04, 1 },
    { 0x81c7, 0x04, 1 },
};

const SGIKonaGLQueryInfo *sgi_kona_gl_query_lookup(uint32_t pname)
{
    for (unsigned i = 0; i < ARRAY_SIZE(sgi_kona_gl_queries); i++) {
        if (sgi_kona_gl_queries[i].pname == pname) {
            return &sgi_kona_gl_queries[i];
        }
    }
    return NULL;
}

const SGIKonaGLQueryInfo *sgi_kona_gl_query_table(size_t *count)
{
    *count = ARRAY_SIZE(sgi_kona_gl_queries);
    return sgi_kona_gl_queries;
}

bool sgi_kona_gl_query_serialize(const SGIKonaGLQueryInfo *info,
                                 const uint64_t *values, unsigned count,
                                 uint8_t *out, size_t capacity, size_t *length)
{
    unsigned width;
    size_t bytes;

    *length = 0;
    if (!info || count != info->count) {
        return false;
    }
    switch (info->type) {
    case 0x00:
        width = 1;
        break;
    case 0x03:
    case 0x04:
    case 0x13:
        width = 4;
        break;
    case 0x12:
        width = 8;
        break;
    default:
        return false;
    }
    bytes = (size_t)count * width;
    if (bytes > capacity) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        switch (width) {
        case 1:
            out[i] = values[i];
            break;
        case 4:
            stl_be_p(out + 4 * i, values[i]);
            break;
        case 8:
            stq_be_p(out + 8 * i, values[i]);
            break;
        }
    }
    *length = bytes;
    return true;
}

/* RGBA channels occupy the bytes from most to least significant. */
bool sgi_kona_gl_texture_transform(uint32_t flags, uint32_t scale,
                                   uint32_t bias, double scale_rgba[4],
                                   double bias_rgba[4])
{
    if (flags & ~0xfu) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        unsigned shift = 24 - 8 * k;
        bool enabled = flags & (8u >> k);

        scale_rgba[k] = enabled ? ((scale >> shift) & 0xff) / 255.0 : 1.0;
        bias_rgba[k] = enabled ? ((bias >> shift) & 0xff) / 255.0 : 0.0;
    }
    return true;
}

void sgi_kona_gl_texture_dimensions(const uint32_t bind[5], uint32_t *width,
                                    uint32_t *height)
{
    *width = 1u << (bind[3] & 15);
    *height = 1u << ((bind[3] >> 4) & 15);
}

/* Native min selectors group spatial and mipmap interpolation separately. */
bool sgi_kona_gl_texture_sampler(const uint32_t bind[5],
                                 const uint32_t record[22],
                                 SGIKonaTextureParams *params)
{
    static const uint32_t min_filter[8] = {
        SGI_GL_NEAREST, SGI_GL_LINEAR, 0,
        SGI_GL_NEAREST_MIPMAP_NEAREST, SGI_GL_NEAREST_MIPMAP_LINEAR,
        SGI_GL_LINEAR_MIPMAP_NEAREST, SGI_GL_LINEAR_MIPMAP_LINEAR, 0,
    };
    SGIKonaTextureParams next = *params;
    unsigned min = (bind[2] >> 2) & 7;
    unsigned mag = (bind[2] >> 5) & 7;
    unsigned lo = record[2] & 0x3ff;
    unsigned hi = (record[2] >> 10) & 0x3ff;
    unsigned scale_s = record[1] & 0xff;
    unsigned scale_t = (record[1] >> 8) & 0xff;

    /*
     * Non-repeat S/T collapse three public clamp modes into the same bits.
     * Their border storage is not established by this sampler record.
     */
    if (!min_filter[min] || (mag > 1 && (mag < 3 || mag > 5)) ||
        (bind[3] & 0x7000) ||
        !scale_s || !scale_t) {
        return false;
    }
    next.min_filter = min_filter[min];
    next.mag_filter = mag ? SGI_GL_LINEAR : SGI_GL_NEAREST;
    next.filter_curve_slot = 0;
    next.detail_level = 0;
    next.detail_mode = SGI_GL_MODULATE;
    if (mag >= 3) {
        switch (bind[2] & 0x0c000000) {
        case 0:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_SGIS :
                                        SGI_GL_LINEAR_DETAIL_SGIS;
            break;
        case 0x04000000:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_COLOR_SGIS :
                                        SGI_GL_LINEAR_DETAIL_COLOR_SGIS;
            break;
        case 0x08000000:
            next.mag_filter = mag == 5 ? SGI_GL_LINEAR_SHARPEN_ALPHA_SGIS :
                                        SGI_GL_LINEAR_DETAIL_ALPHA_SGIS;
            break;
        default:
            return false;
        }
        next.filter_curve_slot = bind[1] & 0xfff;
        /* Categorical negative bounds cannot evaluate a filter curve. */
        if ((lo && lo < 64) || hi < 64) {
            return false;
        }
    }
    if (mag == 3 || mag == 4) {
        unsigned detail_level = (bind[2] >> 21) & 7;

        if (detail_level > 4) {
            return false;
        }
        next.detail_level = -(int)detail_level;
        next.detail_mode = mag == 3 ? SGI_GL_ADD : SGI_GL_MODULATE;
    }
    next.wrap_s = next.wrap_t = SGI_GL_REPEAT;
    next.lod_scale_s = scale_s / 16.0;
    next.lod_scale_t = scale_t / 16.0;
    if (min >= 3 || mag >= 3) {
        /*
         * Positive effective LOD bounds encode trunc(64 * (LOD + 1)).
         * Below 64 the base-zero producer uses a quantized exponential.
         * Its exact negative lambda cannot be recovered. For ordinary
         * filters every negative lambda selects magnification, so retain
         * that decision with categorical bounds rather than invert it.
         * Nonzero base levels use another metric and remain unsupported.
         */
        if (params->base_level || (record[2] & 0xfff00000) || lo > hi) {
            return false;
        }
        next.min_lod = lo >= 64 ? lo / 64.0 - 1.0 : -1000.0;
        next.max_lod = hi >= 64 ? hi / 64.0 - 1.0 : -1.0;
        if (next.min_lod > next.max_lod) {
            return false;
        }
    }
    *params = next;
    return true;
}

void sgi_kona_gl_texture_volume_dimensions(const uint32_t bind[5],
                                            uint32_t *width, uint32_t *height,
                                            uint32_t *depth)
{
    sgi_kona_gl_texture_dimensions(bind, width, height);
    *depth = 1u << ((bind[3] >> 8) & 15);
}

bool sgi_kona_gl_texture_volume_sampler(const uint32_t bind[5],
                                         const uint32_t *record, bool mipmapped,
                                         SGIKonaTextureParams *params)
{
    SGIKonaTextureParams next = *params;
    uint32_t ordinary[22] = { 0, 0x00101010 };
    unsigned min = (bind[2] >> 2) & 7, mag = (bind[2] >> 5) & 7;

    if ((bind[1] >> 28) != 3 || params->base_level || mag > 1 ||
        (mipmapped ? min < 3 || min > 6 : min > 1 || min != mag)) {
        return false;
    }
    if (mipmapped) {
        memcpy(ordinary, record, 4 * sizeof(uint32_t));
        next.lod_scale_r = ((record[1] >> 16) & 255) / 16.0;
        if (next.lod_scale_r == 0) {
            return false;
        }
    } else {
        /* The short record omits LOD state; equal filters do not need it. */
        next.lod_scale_r = 1;
    }
    next.wrap_r = SGI_GL_REPEAT;
    if (!sgi_kona_gl_texture_sampler(bind, ordinary, &next)) {
        return false;
    }
    *params = next;
    return true;
}

bool sgi_kona_gl_texture_internal_format(uint32_t index, uint32_t *base,
                                         uint32_t *internal)
{
    static const struct {
        uint32_t base, internal;
    } formats[] = {
        [0] = { SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        [1] = { SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        [2] = { SGI_GL_RGBA, SGI_GL_RGBA },
        [3] = { SGI_GL_LUMINANCE, 0x8042 },
        [6] = { SGI_GL_LUMINANCE, 0x803f },
        [7] = { SGI_GL_ALPHA, 0x803b },
        [9] = { SGI_GL_LUMINANCE_ALPHA, 0x8043 },
        [10] = { SGI_GL_LUMINANCE, 0x8040 },
        [11] = { SGI_GL_ALPHA, 0x803c },
        [13] = { SGI_GL_LUMINANCE_ALPHA, 0x8044 },
        [14] = { SGI_GL_LUMINANCE_ALPHA, 0x8045 },
        [15] = { SGI_GL_LUMINANCE, SGI_GL_LUMINANCE },
        [16] = { SGI_GL_LUMINANCE, 0x8041 },
        [17] = { SGI_GL_ALPHA, SGI_GL_ALPHA },
        [18] = { SGI_GL_ALPHA, 0x803d },
        [21] = { SGI_GL_LUMINANCE_ALPHA, SGI_GL_LUMINANCE_ALPHA },
        [23] = { SGI_GL_ALPHA, 0x803e },
        [33] = { SGI_GL_RGB, 0x8051 },
        [34] = { SGI_GL_RGBA, 0x8058 },
        [37] = { SGI_GL_RGB, SGI_GL_RGB },
        [38] = { SGI_GL_RGB, SGI_GL_RGB },
        [41] = { SGI_GL_RGBA, SGI_GL_RGBA },
    };

    if (index >= ARRAY_SIZE(formats) || !formats[index].base) {
        return false;
    }
    *base = formats[index].base;
    *internal = formats[index].internal;
    return true;
}

uint32_t sgi_kona_gl_texture_pixel_bytes(uint32_t format, uint32_t type)
{
    static const uint8_t widths[] = { 1, 1, 2, 2, 4, 4, 4 };
    uint32_t components;

    switch (format) {
    case 3:                     /* ALPHA */
    case 6:                     /* LUMINANCE */
        components = 1;
        break;
    case 4:                     /* RGB */
        components = 3;
        break;
    case 5:                     /* RGBA */
        components = 4;
        break;
    case 7:                     /* LUMINANCE_ALPHA */
        components = 2;
        break;
    default:
        return 0;
    }
    return type < ARRAY_SIZE(widths) ? components * widths[type] : 0;
}

bool sgi_kona_gl_texture_pixel_rgba(uint32_t format, uint32_t type,
                                    const uint32_t *source, bool swap_bytes,
                                    uint32_t *rgba)
{
    uint32_t components = sgi_kona_gl_texture_pixel_bytes(format, 1);
    uint32_t values[4] = { 0, 0, 0, 255 };
    uint32_t r, g, b, a;

    if (!sgi_kona_gl_texture_pixel_bytes(format, type)) {
        return false;
    }
    for (uint32_t i = 0; i < components; i++) {
        double component;
        uint32_t raw;

        switch (type) {
        case 0:
        case 1:
            raw = (source[0] >> (24 - i * 8)) & 255;
            component = type == 1 ? raw / 255.0 :
                        (2.0 * (int8_t)raw + 1) / 255.0;
            break;
        case 2:
        case 3:
            raw = (source[i / 2] >> (16 - (i % 2) * 16)) & 65535;
            if (swap_bytes) {
                raw = bswap16(raw);
            }
            component = type == 3 ? raw / 65535.0 :
                        (2.0 * (int16_t)raw + 1) / 65535.0;
            break;
        case 4:
        case 5:
            raw = swap_bytes ? bswap32(source[i]) : source[i];
            component = type == 5 ? raw / 4294967295.0 :
                        (2.0 * (int32_t)raw + 1) / 4294967295.0;
            break;
        case 6: {
            float value;

            raw = swap_bytes ? bswap32(source[i]) : source[i];
            /* Reject both NaNs and infinities before host FP arithmetic. */
            if ((raw & 0x7f800000) == 0x7f800000) {
                return false;
            }
            memcpy(&value, &raw, sizeof(value));
            component = value;
            break;
        }
        default:
            g_assert_not_reached();
        }
        values[i] = (uint32_t)(CLAMP(component, 0.0, 1.0) * 255.0 + 0.5);
    }
    r = values[0];
    g = values[1];
    b = values[2];
    a = values[3];
    switch (format) {
    case 3:
        a = r;
        r = g = b = 0;
        break;
    case 4:
        a = 255;
        break;
    case 6:
        g = b = r;
        a = 255;
        break;
    case 7:
        a = g;
        g = b = r;
        break;
    }
    *rgba = (r << 24) | (g << 16) | (b << 8) | a;
    return true;
}

bool sgi_kona_gl_texture_filter_curve(const uint32_t words[33],
                                      double values[64])
{
    double decoded[64];

    if (!words || !values) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t first = words[2 * i], last = words[2 * i + 1];
        uint32_t code[4] = {
            first & 0x3ff, (first >> 10) & 0x3ff,
            (first >> 20) & 0x3ff, (first >> 30) | (last << 2),
        };

        if (last & ~0xffu) {
            return false;
        }
        for (unsigned j = 0; j < 4; j++) {
            int mantissa = (int8_t)(code[j] >> 2);

            decoded[4 * i + j] = mantissa * (1 << (code[j] & 3)) / 128.0;
        }
    }
    memcpy(values, decoded, sizeof(decoded));
    return true;
}
