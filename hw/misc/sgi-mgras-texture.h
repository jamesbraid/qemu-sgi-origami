/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_MGRAS_TEXTURE_H
#define HW_MISC_SGI_MGRAS_TEXTURE_H

#include "hw/misc/sgi-mgras.h"
#include "hw/display/sgi/rasterizer.h"
#include "migration/vmstate.h"

/*
 * The raster register records packet, 0x7c08, whose inline block the
 * texture path reads, and the parser payload slots it keeps its place in
 * while the block's words arrive.
 */
#define SGI_MGRAS_TEX_RECORDS_OP    0x7c
#define SGI_MGRAS_TEX_RECORD_SLOTS  8

/* Whether a main-port header is one of the texture path's packets. */
bool sgi_mgras_texture_header(uint32_t header);
/*
 * Execute a texture packet, or a 0xec10 table write to a texture table;
 * false for a packet the GL path keeps for itself.
 */
bool sgi_mgras_texture_execute(SGIMgrasState *s, uint32_t header,
                               const uint32_t *payload, uint32_t words);
/*
 * Whether an inline block carries the records of the 0x7c08 packet just
 * executed; if so it sets the parser to take the block's words through
 * sgi_mgras_texture_record_word().
 */
bool sgi_mgras_texture_records(SGIMgrasState *s, uint32_t header);
void sgi_mgras_texture_record_word(SGIMgrasState *s, uint32_t word);
/* Whether a pixel draw's 0x8d20 payload is a texture load, taken here. */
bool sgi_mgras_texture_load_start(SGIMgrasState *s, const uint32_t *payload);
/*
 * Whether a waiting texture load takes a DMA transfer of rows rows of bpr
 * bytes; *take says whether the rows should be delivered.
 */
bool sgi_mgras_texture_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                           bool *take);
/* Whether one row of that transfer went to the texture load. */
bool sgi_mgras_texture_row(SGIMgrasState *s, const uint8_t *bytes,
                           uint32_t n);
/* End a texture load whose transfer failed partway. */
void sgi_mgras_texture_abort(SGIMgrasState *s);
/*
 * The image the loaded context draws with, filled into *image, or NULL
 * when texturing is off or its state is one the model does not draw.
 */
const SGIGLTexImage *sgi_mgras_texture_image(SGIMgrasState *s,
                                             SGIGLTexImage *image);
bool sgi_mgras_texture_pixel(SGIMgrasState *s, const float rgba[4],
                             float out[4]);
/*
 * Whether a DMA kick, a read when read, is the one an open TRAM save or
 * restore (or a 0x9d10 transfer the model does not take) was waiting for:
 * if so it was taken here and has moved its bytes.
 */
bool sgi_mgras_texture_tram_kick(SGIMgrasState *s, uint32_t kick_byte,
                                 bool read);
/* Whether a 0xda00 closes a TRAM save's read, taken here. */
bool sgi_mgras_texture_tram_read_done(SGIMgrasState *s);
/* Whether the board's TRAM transfer state is in range. */
bool sgi_mgras_texture_transfer_valid(const SGIMgrasState *s);
/*
 * A 0x7f08 register pair the kernel's TRAM save or restore sends, taken
 * outside any GL context's texture state; false for any other register.
 */
bool sgi_mgras_texture_kernel_record(SGIMgrasState *s, uint32_t reg,
                                     uint32_t value);
/* Clear TRAM and its transfer state, as a board reset does. */
void sgi_mgras_texture_board_reset(SGIMgrasState *s);
/* Forget one context's texture state. */
void sgi_mgras_texture_reset(SGIMgrasTexture *t);
/* Whether migrated texture state is in range. */
bool sgi_mgras_texture_valid(const SGIMgrasTexture *t);
/* Every context's texture state, once any has some. */
extern const VMStateDescription vmstate_sgi_mgras_texture;
/* TRAM's bytes, once a block has been written. */
extern const VMStateDescription vmstate_sgi_mgras_tram;

#endif
