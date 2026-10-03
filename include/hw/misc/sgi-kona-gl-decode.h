/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality (KONA) OpenGL doorbell command decoder.
 *
 * The table lists the decoded selector subset. The decoder holds no device
 * state, so the framing rules can be tested on the host and shared by the
 * device and its renderer.
 */
#ifndef HW_MISC_SGI_KONA_GL_DECODE_H
#define HW_MISC_SGI_KONA_GL_DECODE_H

#include "hw/misc/sgi-kona-texture.h"

#define SGI_KONA_GL_MAX_ARGS 8

typedef enum SGIKonaGLLayoutEvidence {
    /* The selector table records this layout as established. */
    SGI_KONA_GL_LAYOUT_ESTABLISHED,
    /* Counted from computed stores; one word per store unless shown wider. */
    SGI_KONA_GL_LAYOUT_INFERRED,
    /* The argument count is not settled; the decoder will not frame it. */
    SGI_KONA_GL_LAYOUT_UNRESOLVED,
} SGIKonaGLLayoutEvidence;

typedef struct SGIKonaGLSelectorInfo {
    uint32_t selector;
    const char *call;
    SGIKonaGLLayoutEvidence evidence;
    unsigned argc;
    /* Zero when argument 0 is computed; else the only values seen for it. */
    unsigned num_arg0_values;
    uint32_t arg0_values[2];
} SGIKonaGLSelectorInfo;

typedef enum SGIKonaGLDecodeResult {
    SGI_KONA_GL_DECODE_OK,
    /* The first word is not a selector in the table. */
    SGI_KONA_GL_DECODE_UNKNOWN_SELECTOR,
    /* A known selector whose argument count the evidence does not settle. */
    SGI_KONA_GL_DECODE_UNRESOLVED_LAYOUT,
    /* Fewer words are available than the packet needs. */
    SGI_KONA_GL_DECODE_TRUNCATED,
    /* The caller declared a packet length the table disagrees with. */
    SGI_KONA_GL_DECODE_LENGTH_MISMATCH,
    /* Argument 0 is a literal in every emitter and this one differs. */
    SGI_KONA_GL_DECODE_UNEXPECTED_ARGUMENT,
} SGIKonaGLDecodeResult;

typedef struct SGIKonaGLCommand {
    const SGIKonaGLSelectorInfo *info;  /* NULL for an unknown selector */
    uint32_t selector;
    unsigned argc;
    uint32_t args[SGI_KONA_GL_MAX_ARGS];
    /*
     * Words this packet occupies, selector included. After an unknown or
     * unresolved selector it is 1, so a caller can skip the word and
     * resynchronise; after truncation it is the length still needed.
     */
    unsigned length;
} SGIKonaGLCommand;

/* Pass as declared_len when the transport announces no packet length. */
#define SGI_KONA_GL_LENGTH_UNDECLARED 0

const SGIKonaGLSelectorInfo *sgi_kona_gl_lookup(uint32_t selector);

/*
 * Decode the packet starting at words[0]. declared_len is a word count
 * the transport announced for this packet, selector included, or
 * SGI_KONA_GL_LENGTH_UNDECLARED. cmd is filled for every result.
 */
SGIKonaGLDecodeResult sgi_kona_gl_decode(const uint32_t *words, size_t nwords,
                                         size_t declared_len,
                                         SGIKonaGLCommand *cmd);

const char *sgi_kona_gl_decode_result_name(SGIKonaGLDecodeResult result);

/* The generated table, exposed for tests that walk every entry. */
const SGIKonaGLSelectorInfo *sgi_kona_gl_selector_table(size_t *count);

/* Native return metadata does not establish a supported state value. */
typedef struct SGIKonaGLQueryInfo {
    uint32_t pname;
    uint8_t type;
    uint8_t count;
} SGIKonaGLQueryInfo;

const SGIKonaGLQueryInfo *sgi_kona_gl_query_lookup(uint32_t pname);
const SGIKonaGLQueryInfo *sgi_kona_gl_query_table(size_t *count);

/* Values are native representation bits, not client getter conversions. */
bool sgi_kona_gl_query_serialize(const SGIKonaGLQueryInfo *info,
                                 const uint64_t *values, unsigned count,
                                 uint8_t *out, size_t capacity, size_t *length);

/* Unknown flag bits refuse the record without changing either output. */
bool sgi_kona_gl_texture_transform(uint32_t flags, uint32_t scale,
                                   uint32_t bias, double scale_rgba[4],
                                   double bias_rgba[4]);

/*
 * Decode established repeat/ordinary-filter sampler fields. Level identity,
 * border colour and post-filter transforms remain with their owning records.
 * Unsupported encodings leave every parameter unchanged.
 */
/* Level-zero dimensions reconstructed from the emitted logarithms. */
void sgi_kona_gl_texture_dimensions(const uint32_t bind[5], uint32_t *width,
                                    uint32_t *height);

bool sgi_kona_gl_texture_sampler(const uint32_t bind[5],
                                 const uint32_t record[22],
                                 SGIKonaTextureParams *params);

#endif /* HW_MISC_SGI_KONA_GL_DECODE_H */
