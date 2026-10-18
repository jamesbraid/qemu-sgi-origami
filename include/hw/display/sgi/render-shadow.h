/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_DISPLAY_SGI_RENDER_SHADOW_H
#define HW_DISPLAY_SGI_RENDER_SHADOW_H

#include "hw/display/sgi/rasterizer.h"

/*
 * Shadow comparison: a test mode that measures the Mesa renderer against
 * the scalar path on real guest work. The scalar path stays authoritative
 * and is the only path that writes native storage. For each draw the
 * family:
 *
 * 1. calls sgi_render_shadow_capture() before the scalar draw, which
 *    copies every resource of its private target from the native one;
 * 2. runs the scalar draw against native storage as usual;
 * 3. calls sgi_render_shadow_draw(), or sgi_render_shadow_raster() for a
 *    decoded raster operation, which attempts the same work on Mesa
 *    against the private copy, through the private target's own commit
 *    callbacks, and compares the result with native storage.
 *
 * Write coverage: the shadow records each pixel Mesa's commits write. A
 * family that also records the pixels its scalar store callbacks write
 * during step 2, one byte a pixel per resource in the resource's row
 * order, passes them as scalar_writes; a pixel one path wrote and the
 * other did not is then a mismatch even when the stored values agree,
 * as for a write with the colour already held, or a tag or other side
 * effect of a store. Without masks only contents are compared, the
 * outcome says so, and such draws are counted apart.
 *
 * The private target lists the same resources, in the same order, as the
 * native one, with callbacks over the family's private host storage: a
 * snapshot, a dense commit for the capture and a covered commit for the
 * Mesa attempt. Neither call writes native storage, latches anything the
 * scalar path depends on, or reports anything to the guest.
 */

typedef enum SGIRenderShadowResult {
    SGI_RENDER_SHADOW_MATCH,
    SGI_RENDER_SHADOW_MISMATCH,
    SGI_RENDER_SHADOW_REFUSED,      /* Mesa refused; nothing compared */
    SGI_RENDER_SHADOW_FAILED,       /* Mesa or a callback failed */
    SGI_RENDER_SHADOW_DISABLED,     /* no shadow renderer this session */
} SGIRenderShadowResult;

typedef struct SGIRenderShadowCounts {
    uint64_t matches, mismatches, refusals, failures;
    /* Matches and mismatches compared without scalar write masks. */
    uint64_t without_coverage;
} SGIRenderShadowCounts;

typedef struct SGIRenderShadowOutcome {
    SGIRenderShadowResult result;
    SGIRenderSignature signature;
    SGIRenderRefusal refusal;       /* REFUSED: why */
    SGIRenderResult status;         /* FAILED: the cause */
    /*
     * MISMATCH: the first differing pixel, in resource order then rows
     * then columns, with each path's component slots; or, with
     * phase_differs, the two final stipple phases.
     */
    size_t resource;
    uint32_t x, y;
    uint8_t components;
    uint32_t scalar[4], mesa[4];
    bool scalar_wrote, mesa_wrote;  /* at that pixel, with coverage */
    bool coverage_compared;         /* write masks were compared */
    bool phase_differs;
    uint32_t scalar_phase, mesa_phase;
} SGIRenderShadowOutcome;

typedef struct SGIRenderShadow SGIRenderShadow;

/*
 * A shadow session with its own renderer. If the renderer cannot be
 * created the session is disabled, which sgi_render_shadow_disabled()
 * reports with the reason, and every draw returns DISABLED.
 */
SGIRenderShadow *sgi_render_shadow_new(void);
void sgi_render_shadow_free(SGIRenderShadow *shadow);
/* The reason the session is disabled, or NULL while it runs. */
const char *sgi_render_shadow_disabled(SGIRenderShadow *shadow);

/* Step 1: copy each private resource from its native one, full extent. */
SGIRenderResult sgi_render_shadow_capture(SGIRenderShadow *shadow,
                                          const SGIRasterTarget *native,
                                          const SGIRasterTarget *private,
                                          Error **errp);

/*
 * Step 3, after the scalar draw: attempt draw on Mesa against private,
 * with the mixed-frame flag clear and reports suppressed, and compare
 * every resource but textures with native. A borrowed stipple phase is
 * not touched: the attempt counts from phase_before, the value before the
 * scalar draw, in its own copy, which is compared with the scalar draw's
 * final *draw->line_phase. A failure recreates the shadow renderer for the
 * next draw. scalar_writes is NULL, or one mask per native resource, NULL
 * for any not recorded; coverage is compared only when every non-texture
 * resource has one. Counts the result under the draw's signature.
 */
SGIRenderShadowResult sgi_render_shadow_draw(SGIRenderShadow *shadow,
                                             const SGIRasterRules *rules,
                                             const SGIRasterTarget *native,
                                             const SGIRasterTarget *private,
                                             const SGIGLDraw *draw,
                                             uint32_t phase_before,
                                             const uint8_t *const
                                             *scalar_writes,
                                             SGIRenderShadowOutcome *out);
/* The same for a decoded raster operation, such as CLEAR. */
SGIRenderShadowResult sgi_render_shadow_raster(SGIRenderShadow *shadow,
                                               const SGIRasterRules *rules,
                                               const SGIRasterTarget *native,
                                               const SGIRasterTarget *private,
                                               const SGIRasterOp *op,
                                               const uint8_t *const
                                               *scalar_writes,
                                               SGIRenderShadowOutcome *out);

/* The counts for one signature's text; all zero if it was never seen. */
SGIRenderShadowCounts sgi_render_shadow_counts(SGIRenderShadow *shadow,
                                               const char *signature);
/* The totals over every signature. */
SGIRenderShadowCounts sgi_render_shadow_totals(SGIRenderShadow *shadow);
/* Call fn for each signature seen, in no fixed order. */
void sgi_render_shadow_foreach(SGIRenderShadow *shadow,
                               void (*fn)(const char *signature,
                                          const SGIRenderShadowCounts *counts,
                                          void *opaque),
                               void *opaque);
/* Test-only: the session's current renderer, NULL when disabled. */
SGIRenderer *sgi_render_shadow_test_renderer(SGIRenderShadow *shadow);

#endif
