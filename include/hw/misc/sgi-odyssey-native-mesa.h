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
} OdysseyNativeMesa;

bool odyssey_native_mesa_bind(OdysseyNativeMesa *native,
                              SGIBoardMemory *buffer0, const SGIPlane *plane0,
                              SGIBoardMemory *buffer1, const SGIPlane *plane1,
                              void (*invalidate_scanout_all)(void *opaque),
                              void *invalidate_opaque, Error **errp);
SGIRasterTarget odyssey_native_mesa_target(OdysseyNativeMesa *native);
SGIRenderResult odyssey_native_mesa_draw(OdysseyNativeMesa *native,
                                         SGIRenderer *renderer,
                                         const SGIRasterRules *rules,
                                         const SGIGLDraw *draw,
                                         unsigned selected_buffers,
                                         SGIRenderEye eye, bool compare_active,
                                         Error **errp);

#endif
