/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_MGRAS_GL_H
#define HW_MISC_SGI_MGRAS_GL_H

#include "hw/misc/sgi-mgras.h"
#include "migration/vmstate.h"

/* Whether a main-port header is an OpenGL packet the model executes. */
bool sgi_mgras_gl_header(uint32_t header);
/* Execute one such packet once its payload has arrived. */
void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words);
void sgi_mgras_gl_init(SGIMgrasState *s);
void sgi_mgras_gl_cleanup(SGIMgrasState *s);
void sgi_mgras_gl_reset(SGIMgrasState *s);
/* The answer to the 0xa104 current-attribute query for index. */
uint32_t sgi_mgras_gl_attribute_answer(SGIMgrasState *s, uint32_t index);
/* The answer to an 0xa20c state query, from its argument words. */
uint32_t sgi_mgras_gl_query_answer(SGIMgrasState *s, const uint32_t *args,
                                   uint32_t nargs);
/* Select the GL state a completed context image names. */
void sgi_mgras_gl_context_loaded(SGIMgrasState *s);
/* The GE ports, through which the kernel copies GE external RAM. */
#define SGI_MGRAS_GE_PORTS      0x058000
#define SGI_MGRAS_GE_PORTS_END  0x058400
/* Each context's parser position. */
extern const VMStateDescription vmstate_sgi_mgras_gl_context_parser;
/* Each context's light or material readback, once one has been asked. */
extern const VMStateDescription vmstate_sgi_mgras_gl_readback;

/*
 * An inline data block's header: bit 31, the byte offset of its first byte in
 * its first word in bits 23:22, and its byte length in bits 19:0. It takes
 * (offset + length + 3) / 4 words.
 */
#define SGI_MGRAS_CMD_BLOCK_OFFSET 0x00c00000u
#define SGI_MGRAS_CMD_BLOCK_BYTES  0x000fffffu
/* The parser's class while the words of a block a GL packet owns arrive. */
#define SGI_MGRAS_CMD_TOKEN_GL_BLOCK (SGI_MGRAS_CMD_TOKEN_DISCARD - 1)
/*
 * Whether the inline block header continues the GL packet just executed,
 * glBitmap's image; if so it sets the parser to take the block's words
 * through sgi_mgras_gl_block_word(), one at a time after the parser has
 * counted each off.
 */
bool sgi_mgras_gl_block(SGIMgrasState *s, uint32_t header);
void sgi_mgras_gl_block_word(SGIMgrasState *s, uint32_t word);
/* A marker for a block under way. */
extern const VMStateDescription vmstate_sgi_mgras_gl_block;

/*
 * The parser's class while the words of a GE parameter load (a class-0
 * 0x80 or 0xcd packet) arrive; each goes to sgi_mgras_gl_ge_load_word()
 * after the parser has counted it off.
 */
#define SGI_MGRAS_CMD_TOKEN_GE_LOAD (SGI_MGRAS_CMD_TOKEN_DISCARD - 2)
/*
 * Whether a main-port header opens a GE parameter load; if so the parser
 * is set to stream its words.
 */
bool sgi_mgras_gl_ge_load(SGIMgrasState *s, uint32_t header);
void sgi_mgras_gl_ge_load_word(SGIMgrasState *s, uint32_t word);
/*
 * Whether the loaded context has a pixel draw waiting for its image, so
 * that a host-memory DMA transfer to the GE carries it; *take says whether
 * the transfer's rows rows of bpr bytes fit the draw. One that does not
 * ends the draw, logged, and the caller moves nothing.
 */
bool sgi_mgras_gl_pixels_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                             bool *take);
/* One row of that transfer, one or more image rows from its first byte. */
void sgi_mgras_gl_pixels_row(SGIMgrasState *s, const uint8_t *bytes,
                             uint32_t n);
/* End the draw after a transfer that failed partway. */
void sgi_mgras_gl_pixels_abort(SGIMgrasState *s);
/*
 * Whether the GE pixel program loaded is the three-unsigned-byte RGB one
 * (GL_RGB, GL_UNSIGNED_BYTE at the initial pixel transfer state).
 */
bool sgi_mgras_gl_pixels_rgb8(SGIMgrasState *s);
/*
 * Note a header in the loaded context for its waiting pixel draw: any
 * header but a zero word or an inline block ends the draw's claim on the
 * blocks that follow.
 */
void sgi_mgras_gl_pixels_header(SGIMgrasState *s, uint32_t header);
/* The GE parameters and each context's pixel draw, once either is used. */
extern const VMStateDescription vmstate_sgi_mgras_gl_pixels;
/* Each context's DMA mark on a waiting pixel draw, while one has it. */
extern const VMStateDescription vmstate_sgi_mgras_gl_pixel_flags;

/*
 * The kernel's context eviction (sgi-mgras-gl.c, "Context eviction"). A
 * DMA read with no raster box that fits it may be the read-out of a
 * context, which the 0xfc08 after it confirms; host is the address of its
 * first byte, rows and bpr its descriptor for the log line if nothing
 * claims it. A write kick with the GE port named, no pixel draw waiting
 * and an 0xfd04 count before it carries a context back in, which the
 * 0xfb08 after it places.
 */
void sgi_mgras_gl_eram_read_kicked(SGIMgrasState *s, uint64_t host,
                                   uint32_t rows, uint32_t bpr);
bool sgi_mgras_gl_eram_write_armed(SGIMgrasState *s);
void sgi_mgras_gl_eram_write_kicked(SGIMgrasState *s, uint64_t host);
/* Note a header for a pending ERAM transfer, which only its token claims. */
void sgi_mgras_gl_eram_header(SGIMgrasState *s, uint32_t header);
/* Whether migrated ERAM transfer state is in range. */
bool sgi_mgras_gl_eram_valid(SGIMgrasState *s);

/* Note GE-port traffic the per-context state cannot follow. */
void sgi_mgras_gl_ge_port_access(SGIMgrasState *s, hwaddr addr, bool write);
/* The command parser's position, to set aside and take up again. */
void sgi_mgras_cmd_parser_save(SGIMgrasState *s, SGIMgrasCmdParser *p);
void sgi_mgras_cmd_parser_load(SGIMgrasState *s, const SGIMgrasCmdParser *p);
/* Whether a migrated parser position is in range. */
bool sgi_mgras_cmd_parser_valid(uint32_t cls, uint32_t reg,
                                uint32_t words_remaining,
                                uint32_t payload_count);
/* Whether migrated GL state is in range. */
bool sgi_mgras_gl_post_load(SGIMgrasState *s);

#endif
