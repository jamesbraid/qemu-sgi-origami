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

#define SGI_KONA_GL_MAX_ARGS 22

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
 * Decode repeat, ordinary-filter and sharpening sampler fields. Level identity,
 * border colour and post-filter transforms remain with their owning records.
 * Unsupported encodings leave every parameter unchanged.
 */
/* Level-zero dimensions reconstructed from the emitted logarithms. */
void sgi_kona_gl_texture_dimensions(const uint32_t bind[5], uint32_t *width,
                                    uint32_t *height);

bool sgi_kona_gl_texture_sampler(const uint32_t bind[5],
                                 const uint32_t record[22],
                                 SGIKonaTextureParams *params);
bool sgi_kona_gl_texture_sampler_1d(const uint32_t bind[5],
                                    const uint32_t record[22],
                                    SGIKonaTextureParams *params);

/* Validate native storage families, dimensional mode and transport. */
bool sgi_kona_gl_texture_native_layout(const uint32_t header[20]);
/* The record byte must agree with an independently decoded public format. */
bool sgi_kona_gl_texture_native_binding_layout(uint32_t base, uint32_t target,
                                               uint32_t record);

/* Volume records use fourteen mipmap words or four nonmipmap words. */
bool sgi_kona_gl_texture_volume_sampler(const uint32_t bind[5],
                                         const uint32_t *record, bool mipmapped,
                                         SGIKonaTextureParams *params);
void sgi_kona_gl_texture_volume_dimensions(const uint32_t bind[5],
                                            uint32_t *width, uint32_t *height,
                                            uint32_t *depth);

/* Decode supported native internal-format table indexes transactionally. */
bool sgi_kona_gl_texture_internal_format(uint32_t index, uint32_t *base,
                                         uint32_t *internal);

/* Native scalar types 0..6; zero bytes for an unknown format or type. */
uint32_t sgi_kona_gl_texture_pixel_bytes(uint32_t format, uint32_t type);
uint32_t sgi_kona_gl_native_pixel_bytes(uint32_t format, uint32_t type);

/* Raw indices retain integer values, including fractional FLOAT input. */
bool sgi_kona_gl_pixel_index(uint32_t type, const uint32_t *source,
                              bool swap_bytes, double *index);
bool sgi_kona_gl_pixel_index_transfer(double index, int32_t shift,
                                       int32_t offset, uint32_t *out);
bool sgi_kona_gl_pixel_index_round(double index, int32_t shift,
                                    int32_t offset, uint32_t *out);
bool sgi_kona_gl_pixel_index_pack(uint32_t type, uint32_t index,
                                   bool swap_bytes, uint8_t *out,
                                   size_t capacity);

/*
 * Components occupy transport-decoded words, most significant bytes first.
 * Integer inputs use OpenGL 1.2.1 table 2.6; FLOAT inputs use IEEE binary32.
 * Byte swapping reverses each multibyte component before conversion.
 * Produces full RRGGBBAA before destination-format conversion. Finite values
 * clamp to [0, 1] and round to eight bits for the current model storage
 * profile. Unsupported forms and nonfinite components leave rgba unchanged.
 */
bool sgi_kona_gl_texture_pixel_rgba(uint32_t format, uint32_t type,
                                    const uint32_t *source, bool swap_bytes,
                                    uint32_t *rgba);

/* Apply ordinary pixel transfer before the texture storage quantization. */
bool sgi_kona_gl_texture_pixel_transfer(uint32_t format, uint32_t type,
                                        const uint32_t *source, bool swap_bytes,
                                        const float scale[4],
                                        const float bias[4], uint32_t *rgba);

/* Preserve normalized 32-bit scalar precision before ordinary transfer. */
bool sgi_kona_gl_pixel_rgba_double(uint32_t format, uint32_t type,
                                   const uint32_t *source, bool swap_bytes,
                                   double rgba[4]);

/* Retain finite scalar values before pixel transfer, without quantization. */
bool sgi_kona_gl_pixel_rgba_float(uint32_t format, uint32_t type,
                                  const uint32_t *source, bool swap_bytes,
                                  float rgba[4]);

/*
 * The first 32 words hold 64 signed mantissa/exponent coefficients. Word 32
 * is transport padding. Invalid coefficient words leave values unchanged.
 */
bool sgi_kona_gl_texture_filter_curve(const uint32_t words[33],
                                      double values[64]);

/* Ordinary selector 0x6 PIO framing; source skips are applied by the sender. */
typedef struct SGIKonaGLBitmapLayout {
    uint32_t width, height;
    uint64_t stride, row_bytes, payload_words;
    uint32_t phase, bit_skip;
    bool strided;
} SGIKonaGLBitmapLayout;

/* Unsupported headers or unpack state leave layout unchanged. */
bool sgi_kona_gl_bitmap_pio_layout(const uint32_t header[9],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLBitmapLayout *layout);
typedef struct SGIKonaGLBitmapDMALayout {
    SGIKonaGLBitmapLayout bitmap;
    uint64_t total_bytes, line_bytes, pitch, span;
    uint32_t lines, source_phase;
} SGIKonaGLBitmapDMALayout;

/* Contiguous DMA is word-aligned; strided DMA requests exact row bytes. */
bool sgi_kona_gl_bitmap_dma_layout(const uint32_t header[9],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLBitmapDMALayout *layout);
/* Locate a raw source byte before residual bit skips and bit normalization. */
bool sgi_kona_gl_bitmap_pio_offset(const SGIKonaGLBitmapLayout *layout,
                                   uint32_t row, uint64_t byte,
                                   uint64_t *offset);
/* Normalize one byte to MSB-first; missing words leave output unchanged. */
bool sgi_kona_gl_bitmap_pio_byte(const SGIKonaGLBitmapLayout *layout,
                                 const uint32_t *words, size_t nwords,
                                 bool lsb_first, uint32_t row, uint32_t byte,
                                 uint8_t *out);

/* Pack finite colour to the seven native scalar types, transactionally. */
bool sgi_kona_gl_pixel_pack(uint32_t format, uint32_t type,
                            const float rgba[4], bool swap_bytes,
                            uint8_t *out, size_t capacity);

/* Preserve read transfer and luminance reduction before final narrowing. */
bool sgi_kona_gl_pixel_pack_double(uint32_t format, uint32_t type,
                                   const double rgba[4], bool swap_bytes,
                                   uint8_t *out, size_t capacity);

/* Raw DrawPixels rows include padding; pixel_bytes is zero for bitmaps. */
typedef struct SGIKonaGLPixelLayout {
    uint32_t width, height, pixel_bytes;
    uint64_t stride, row_bytes, payload_words;
    uint32_t phase;
    bool strided;
} SGIKonaGLPixelLayout;

/* Unsupported headers or overflowing spans leave layout unchanged. */
bool sgi_kona_gl_pixels_pio_layout(const uint32_t header[8],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLPixelLayout *layout);
typedef struct SGIKonaGLPixelDMALayout {
    SGIKonaGLPixelLayout pixels;
    uint64_t total_bytes, line_bytes, pitch, span;
    uint32_t lines, source_phase;
} SGIKonaGLPixelDMALayout;

/* Scalar DMA omits PIO prefixes; contiguous bitmap DMA keeps whole words. */
bool sgi_kona_gl_pixels_dma_layout(const uint32_t header[8],
                                   uint32_t row_length, uint32_t skip_rows,
                                   uint32_t skip_pixels, uint32_t alignment,
                                   SGIKonaGLPixelDMALayout *layout);
/* Locate a useful byte in the word-framed PIO staging buffer. */
bool sgi_kona_gl_pixels_pio_offset(const SGIKonaGLPixelLayout *layout,
                                   uint32_t row, uint64_t byte,
                                   uint64_t *offset);
/* Extract a useful row byte; missing words leave output unchanged. */
bool sgi_kona_gl_pixels_pio_byte(const SGIKonaGLPixelLayout *layout,
                                 const uint32_t *words, size_t nwords,
                                 uint32_t row, uint64_t byte, uint8_t *out);

#endif /* HW_MISC_SGI_KONA_GL_DECODE_H */
