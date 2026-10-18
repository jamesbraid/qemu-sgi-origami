/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_ODYSSEY_NATIVE_MESA_H
#define HW_MISC_SGI_ODYSSEY_NATIVE_MESA_H

#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/rasterizer.h"

typedef enum OdysseyNativeStoreResult {
    ODYSSEY_NATIVE_STORE_OK,
    ODYSSEY_NATIVE_STORE_UNCHANGED,
    ODYSSEY_NATIVE_STORE_PARTIAL,
    ODYSSEY_NATIVE_STORE_UNKNOWN,
} OdysseyNativeStoreResult;

typedef struct OdysseyNativeMesa {
    SGIBoardMemory *memory[2];
    const SGIPlane *plane[2];
    SGIRenderResource resources[2];
    SGIClipList clip;
    /* One view per backing, borrowed for one draw; no retained pixel cache. */
    void (*invalidate_scanout_all)(void *opaque);
    void *invalidate_opaque;
    OdysseyNativeStoreResult (*store)(void *opaque, SGIBoardMemory *memory,
                                     const SGIPlane *plane, uint32_t x,
                                     uint32_t y, uint32_t word);
    void *store_opaque;
    /*
     * Test-only: passed to the attempt as its staged-pixel check, so a
     * test can refuse a draw after readback and before any native write.
     */
    SGIRenderResult (*check_staged)(void *opaque,
                                    const SGIRenderResource *resource,
                                    const SGIRenderRect *rect,
                                    const uint32_t *slots,
                                    size_t stride_slots,
                                    size_t capacity_slots,
                                    const SGIRenderWriteCoverage *writes,
                                    Error **errp);
    void *check_staged_opaque;
} OdysseyNativeMesa;

/*
 * Where the device's Mesa draws went: drawn on Mesa, or refused and drawn
 * on the scalar path, counted by refusal reason. Host diagnostics for
 * measuring the remaining fallbacks; never migrated.
 */
typedef struct OdysseyMesaCounts {
    uint64_t committed;
    uint64_t fallbacks[SGI_RENDER_REFUSAL__MAX];
} OdysseyMesaCounts;

/* The device's scalar draw, which a refused draw runs instead. */
typedef SGIGLDrawResult (*OdysseyScalarDraw)(void *opaque,
                                             const SGIGLDraw *draw);

bool odyssey_native_mesa_bind(OdysseyNativeMesa *native,
                              SGIBoardMemory *buffer0, const SGIPlane *plane0,
                              SGIBoardMemory *buffer1, const SGIPlane *plane1,
                              void (*invalidate_scanout_all)(void *opaque),
                              void *invalidate_opaque, Error **errp);
SGIRasterTarget odyssey_native_mesa_target(OdysseyNativeMesa *native);
/*
 * Attempt one draw on Mesa for a frame that may mix in scalar draws. A
 * window state the adapter cannot map is refused before any native
 * access, with a reason as the shared renderer's own refusals carry.
 */
SGIRenderDrawResult odyssey_native_mesa_draw(OdysseyNativeMesa *native,
                                             SGIRenderer *renderer,
                                             const SGIRasterRules *rules,
                                             const SGIGLDraw *draw,
                                             unsigned selected_buffers,
                                             SGIRenderEye eye,
                                             bool compare_active,
                                             SGIRenderAttemptOutcome *outcome,
                                             Error **errp);
/*
 * Draw on Mesa, or draw the same draw with @scalar when Mesa refuses it,
 * and return the result of whichever path drew it. @refusal other than
 * SGI_RENDER_REFUSAL_NONE is the device's own refusal, made before
 * binding; @native is then unused. A refusal is counted and traced and
 * has no other effect; a Mesa failure is reported and returned, never
 * drawn again on the scalar path.
 */
SGIGLDrawResult odyssey_native_mesa_draw_or_scalar(
    OdysseyNativeMesa *native, SGIRenderer *renderer,
    const SGIRasterRules *rules, const SGIGLDraw *draw,
    unsigned selected_buffers, SGIRenderEye eye, bool compare_active,
    SGIRenderRefusal refusal, OdysseyScalarDraw scalar, void *scalar_opaque,
    OdysseyMesaCounts *counts);

#endif
