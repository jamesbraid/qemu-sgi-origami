/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_DISPLAY_SGI_X_GC_H
#define HW_DISPLAY_SGI_X_GC_H

#define SGI_X_GC_WORDS 13

typedef enum SGIXGCTarget {
    SGI_X_GC_UNSUPPORTED,
    SGI_X_GC_MAIN_CI8,
    SGI_X_GC_MAIN_INDEX16,
    SGI_X_GC_MAIN_RGB8,
    SGI_X_GC_OVERLAY0,
    SGI_X_GC_OVERLAY1,
    SGI_X_GC_POPUP,
    SGI_X_GC_DID,
} SGIXGCTarget;

typedef enum SGIXGCStatus {
    SGI_X_GC_OK,
    SGI_X_GC_BAD_FIELDS,
    SGI_X_GC_BAD_COLOR,
    SGI_X_GC_BAD_MASK,
    SGI_X_GC_MIXED_DID,
    SGI_X_GC_DEFERRED,
} SGIXGCStatus;

typedef struct SGIXGC {
    uint32_t raw[SGI_X_GC_WORDS];
    SGIXGCTarget target;
    uint32_t foreground;
    uint32_t background;
    uint32_t mask;
    uint32_t width_mask;
    uint8_t gx;
    uint8_t first_selector;
    uint8_t second_selector;
    bool opaque_or_tiled;
} SGIXGC;

/* GX numbers are shared with GL enums after subtracting GL_CLEAR. */
uint32_t sgi_x_logic_op(unsigned gx, uint32_t source, uint32_t destination);

/* RGB8 accepts zero or full alpha, with all field-gap bits zero. */
SGIXGCStatus sgi_x_gc_decode(const uint32_t words[SGI_X_GC_WORDS],
                            SGIXGC *decoded);
/* Preserve the full native main-plane index and mask. */
SGIXGCStatus sgi_x_gc_decode_main_index(const uint32_t words[SGI_X_GC_WORDS],
                                       SGIXGC *decoded);
uint32_t sgi_x_gc_apply(const SGIXGC *gc, uint32_t source,
                        uint32_t destination);
bool sgi_x_gc_noop(const SGIXGC *gc);
bool sgi_x_gc_needs_destination(const SGIXGC *gc);

#endif
