/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * InfiniteReality (KONA) OpenGL doorbell command decoder.
 *
 * Framing follows the command grammar datasheet: the doorbell is a
 * single write-only address taking selector-then-argument packets
 * ("The userspace doorbell, process VA 0x4000"), and the subset decoded
 * here is its "OpenGL selector catalogue (3D-milestone subset)". Which
 * layouts are established, inferred or unresolved is recorded per entry
 * in the generated table. The doorbell accepts 64-bit stores as well as
 * 32-bit ones, so the caller splits each store into big-endian words
 * before decoding.
 *
 * No kick or length register for the doorbell is established; the
 * declared length exists for transports that do announce one, as the
 * kernel FIFO does ("The kernel FIFO push port, +0x7c").
 */
#include "qemu/osdep.h"
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
