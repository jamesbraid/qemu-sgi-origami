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
