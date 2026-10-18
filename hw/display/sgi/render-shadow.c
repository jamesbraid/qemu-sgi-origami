/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/sgi/render-shadow.h"
#include "ui/sgi-renderer.h"
#include "trace.h"

struct SGIRenderShadow {
    SGIRenderer *renderer;
    char *disabled;
    GHashTable *counts;         /* signature text -> SGIRenderShadowCounts */
    SGIRenderShadowCounts totals;
};

static void sgi_render_shadow_start(SGIRenderShadow *shadow)
{
    Error *err = NULL;

    if (sgi_renderer_create(&shadow->renderer, &err) !=
        SGI_RENDER_CREATE_OK) {
        shadow->renderer = NULL;
        shadow->disabled = g_strdup(err ? error_get_pretty(err) :
                                    "SGI shadow renderer is unavailable");
        trace_sgi_render_shadow_disabled(shadow->disabled);
    }
    error_free(err);
}

SGIRenderShadow *sgi_render_shadow_new(void)
{
    SGIRenderShadow *shadow = g_new0(SGIRenderShadow, 1);

    shadow->counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_free);
    sgi_render_shadow_start(shadow);
    return shadow;
}

void sgi_render_shadow_free(SGIRenderShadow *shadow)
{
    if (shadow) {
        sgi_renderer_free(shadow->renderer);
        g_hash_table_destroy(shadow->counts);
        g_free(shadow->disabled);
        g_free(shadow);
    }
}

const char *sgi_render_shadow_disabled(SGIRenderShadow *shadow)
{
    return shadow->disabled;
}

SGIRenderer *sgi_render_shadow_test_renderer(SGIRenderShadow *shadow)
{
    return shadow->renderer;
}

/* One resource's full extent, as its snapshot callback returns it. */
static uint32_t *sgi_render_shadow_read(const SGIRasterTarget *target,
                                        const SGIRenderResource *res,
                                        size_t *nslots,
                                        SGIRenderResult *status,
                                        Error **errp)
{
    SGIRenderRect rect = { .x1 = res->width - 1, .y1 = res->height - 1 };
    uint32_t *slots;

    if (!res->width || !res->height || !res->component_count ||
        res->depth != 1 ||
        res->width > SIZE_MAX / res->height / res->component_count /
                     sizeof(*slots)) {
        error_setg(errp, "SGI shadow resource has no comparable extent");
        *status = SGI_RENDER_RESOURCE_ERROR;
        return NULL;
    }
    *nslots = (size_t)res->width * res->height * res->component_count;
    slots = g_try_new(uint32_t, *nslots);
    if (!slots) {
        error_setg(errp, "Cannot allocate SGI shadow comparison buffer");
        *status = SGI_RENDER_OOM;
        return NULL;
    }
    *status = sgi_render_resource_snapshot(target, res, 0, &rect, slots,
                                           res->width * res->component_count,
                                           *nslots, errp);
    if (*status != SGI_RENDER_OK) {
        g_free(slots);
        return NULL;
    }
    return slots;
}

/* The private target must describe the native one's resources. */
static bool sgi_render_shadow_pair(const SGIRasterTarget *native,
                                   const SGIRasterTarget *private,
                                   Error **errp)
{
    if (!native->resources || !private->resources ||
        native->resource_count != private->resource_count ||
        !private->resource_ops || !private->resource_ops->snapshot ||
        !private->resource_ops->commit ||
        !private->resource_ops->commit_covered) {
        error_setg(errp, "SGI shadow target does not pair with the native "
                   "target");
        return false;
    }
    for (size_t i = 0; i < native->resource_count; i++) {
        const SGIRenderResource *a = &native->resources[i];
        const SGIRenderResource *b = &private->resources[i];

        if (a->role != b->role || a->target != b->target ||
            a->width != b->width || a->height != b->height ||
            a->depth != b->depth || a->bottom_origin != b->bottom_origin ||
            a->component_count != b->component_count ||
            memcmp(a->component, b->component, sizeof(a->component))) {
            error_setg(errp, "SGI shadow resource %zu differs from its "
                       "native resource", i);
            return false;
        }
    }
    return true;
}

SGIRenderResult sgi_render_shadow_capture(SGIRenderShadow *shadow,
                                          const SGIRasterTarget *native,
                                          const SGIRasterTarget *private,
                                          Error **errp)
{
    if (!sgi_render_shadow_pair(native, private, errp)) {
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (size_t i = 0; i < native->resource_count; i++) {
        const SGIRenderResource *res = &private->resources[i];
        SGIRenderRect rect = { .x1 = res->width - 1, .y1 = res->height - 1 };
        g_autofree uint32_t *slots = NULL;
        SGIRenderCommitResult committed;
        SGIRenderResult status;
        size_t nslots;

        slots = sgi_render_shadow_read(native, &native->resources[i],
                                       &nslots, &status, errp);
        if (!slots) {
            return status;
        }
        committed = sgi_render_resource_commit(private, res, 0, &rect, slots,
                                               res->width *
                                               res->component_count,
                                               nslots, errp);
        if (committed.status != SGI_RENDER_OK) {
            return committed.status;
        }
    }
    return SGI_RENDER_OK;
}

static void sgi_render_shadow_count(SGIRenderShadow *shadow,
                                    const SGIRenderShadowOutcome *out)
{
    SGIRenderShadowCounts *counts = g_hash_table_lookup(
        shadow->counts, out->signature.text);

    if (!counts) {
        counts = g_new0(SGIRenderShadowCounts, 1);
        g_hash_table_insert(shadow->counts, g_strdup(out->signature.text),
                            counts);
    }
    if ((out->result == SGI_RENDER_SHADOW_MATCH ||
         out->result == SGI_RENDER_SHADOW_MISMATCH) &&
        !out->coverage_compared) {
        counts->without_coverage++;
        shadow->totals.without_coverage++;
    }
    switch (out->result) {
    case SGI_RENDER_SHADOW_MATCH:
        counts->matches++;
        shadow->totals.matches++;
        break;
    case SGI_RENDER_SHADOW_MISMATCH:
        counts->mismatches++;
        shadow->totals.mismatches++;
        break;
    case SGI_RENDER_SHADOW_REFUSED:
        counts->refusals++;
        shadow->totals.refusals++;
        break;
    case SGI_RENDER_SHADOW_FAILED:
        counts->failures++;
        shadow->totals.failures++;
        break;
    case SGI_RENDER_SHADOW_DISABLED:
        break;
    }
}

/*
 * A failed attempt may have latched the renderer, so the next draw gets a
 * new one; a failed family callback leaves it as it is.
 */
static void sgi_render_shadow_fail(SGIRenderShadow *shadow,
                                   SGIRenderShadowOutcome *out,
                                   SGIRenderResult status, Error *err,
                                   bool renderer_failed)
{
    out->result = SGI_RENDER_SHADOW_FAILED;
    out->status = status;
    trace_sgi_render_shadow_failed(out->signature.text, status,
                                   err ? error_get_pretty(err) : "");
    if (renderer_failed) {
        sgi_renderer_free(shadow->renderer);
        shadow->renderer = NULL;
        sgi_render_shadow_start(shadow);
    }
}

/*
 * The private target as the Mesa attempt sees it: the family's callbacks,
 * with every pixel each commit writes recorded per resource first.
 */
typedef struct SGIRenderShadowRecorder {
    const SGIRasterTarget *private;
    uint8_t **written;          /* per resource, one byte a pixel */
} SGIRenderShadowRecorder;

static uint8_t *sgi_render_shadow_written(SGIRenderShadowRecorder *rec,
                                          const SGIRenderResource *res,
                                          const SGIRenderResource **orig)
{
    size_t i = res - rec->private->resources;

    assert(i < rec->private->resource_count);
    *orig = &rec->private->resources[i];
    return rec->written[i];
}

static SGIRenderResult sgi_render_shadow_rec_snapshot(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    SGIRenderShadowRecorder *rec = opaque;

    return rec->private->resource_ops->snapshot(rec->private->opaque, res,
                                                slice, rect, slots,
                                                stride_slots, capacity_slots,
                                                errp);
}

static SGIRenderCommitResult sgi_render_shadow_rec_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    SGIRenderShadowRecorder *rec = opaque;
    const SGIRenderResource *orig;
    uint8_t *written = sgi_render_shadow_written(rec, res, &orig);

    for (uint32_t y = rect->y0; written && y <= rect->y1; y++) {
        memset(written + (size_t)y * orig->width + rect->x0, 1,
               rect->x1 - rect->x0 + 1);
    }
    return rec->private->resource_ops->commit(rec->private->opaque, res,
                                              slice, rect, slots,
                                              stride_slots, capacity_slots,
                                              errp);
}

static SGIRenderCommitResult sgi_render_shadow_rec_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    SGIRenderShadowRecorder *rec = opaque;
    const SGIRenderResource *orig;
    uint8_t *written = sgi_render_shadow_written(rec, res, &orig);

    for (uint32_t y = rect->y0; written && y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (writes->pixel[(size_t)(y - rect->y0) * writes->stride_bytes +
                              x - rect->x0]) {
                written[(size_t)y * orig->width + x] = 1;
            }
        }
    }
    return rec->private->resource_ops->commit_covered(
        rec->private->opaque, res, slice, rect, slots, stride_slots,
        capacity_slots, writes, errp);
}

static const SGIRenderResourceOps sgi_render_shadow_rec_ops = {
    .snapshot = sgi_render_shadow_rec_snapshot,
    .commit = sgi_render_shadow_rec_commit,
    .commit_covered = sgi_render_shadow_rec_commit_covered,
};

/*
 * Find the first pixel where the two resources' slots differ, or, with
 * both write masks, where one path wrote and the other did not.
 */
static bool sgi_render_shadow_differs(SGIRenderShadowOutcome *out, size_t i,
                                      const SGIRenderResource *res,
                                      const uint32_t *scalar,
                                      const uint32_t *mesa,
                                      const uint8_t *scalar_written,
                                      const uint8_t *mesa_written)
{
    unsigned n = res->component_count;

    for (uint32_t y = 0; y < res->height; y++) {
        for (uint32_t x = 0; x < res->width; x++) {
            size_t pixel = (size_t)y * res->width + x;
            size_t at = pixel * n;
            bool s = scalar_written && scalar_written[pixel];
            bool m = mesa_written && mesa_written[pixel];

            if (!memcmp(&scalar[at], &mesa[at], n * sizeof(*scalar)) &&
                (!scalar_written || s == m)) {
                continue;
            }
            out->resource = i;
            out->x = x;
            out->y = y;
            out->components = MIN(n, 4);
            memcpy(out->scalar, &scalar[at], out->components *
                   sizeof(*scalar));
            memcpy(out->mesa, &mesa[at], out->components * sizeof(*mesa));
            out->scalar_wrote = s;
            out->mesa_wrote = m;
            return true;
        }
    }
    return false;
}

/*
 * Attempt a draw or raster operation, whichever is given, against the
 * private copy, then compare. phase is the attempt's own stipple phase.
 */
static void sgi_render_shadow_run(SGIRenderShadow *shadow,
                                  const SGIRasterRules *rules,
                                  const SGIRasterTarget *native,
                                  const SGIRasterTarget *private,
                                  const SGIGLDraw *draw,
                                  const SGIRasterOp *op,
                                  const uint8_t *const *scalar_writes,
                                  SGIRenderShadowOutcome *out)
{
    static const SGIRenderAttemptOptions opts = { .quiet = true };
    SGIRenderShadowRecorder rec = { .private = private };
    SGIRasterTarget recorded;
    SGIRenderAttemptOutcome attempt;
    SGIRenderDrawResult result;
    Error *err = NULL;
    bool coverage = scalar_writes != NULL;

    if (!sgi_render_shadow_pair(native, private, &err)) {
        sgi_render_shadow_fail(shadow, out, SGI_RENDER_RESOURCE_ERROR, err,
                               false);
        goto done;
    }
    /* Coverage is compared only when every compared resource has a mask. */
    for (size_t i = 0; coverage && i < native->resource_count; i++) {
        coverage = native->resources[i].role == SGI_RENDER_TEXTURE ||
                   scalar_writes[i];
    }
    rec.written = g_new0(uint8_t *, native->resource_count);
    for (size_t i = 0; i < native->resource_count; i++) {
        const SGIRenderResource *res = &private->resources[i];

        rec.written[i] = g_try_malloc0((size_t)res->width * res->height);
        if (!rec.written[i]) {
            error_setg(&err, "Cannot allocate SGI shadow write mask");
            sgi_render_shadow_fail(shadow, out, SGI_RENDER_OOM, err, false);
            goto done;
        }
    }
    recorded = *private;
    recorded.opaque = &rec;
    recorded.resource_ops = &sgi_render_shadow_rec_ops;
    result = draw ? sgi_render_draw_attempt(shadow->renderer, rules,
                                            &recorded, draw, &opts, &attempt,
                                            &err) :
                    sgi_render_raster_attempt(shadow->renderer, rules,
                                              &recorded, op, &opts, &attempt,
                                              &err);
    switch (result) {
    case SGI_RENDER_DRAW_OK:
        break;
    case SGI_RENDER_DRAW_REFUSED:
        out->result = SGI_RENDER_SHADOW_REFUSED;
        out->refusal = attempt.reason;
        trace_sgi_render_shadow_refused(out->signature.text,
                                        sgi_render_refusal_name(
                                            attempt.reason));
        goto done;
    default:
        sgi_render_shadow_fail(shadow, out, attempt.status, err, true);
        goto done;
    }
    out->result = SGI_RENDER_SHADOW_MATCH;
    out->coverage_compared = coverage;
    for (size_t i = 0; i < native->resource_count; i++) {
        const SGIRenderResource *res = &native->resources[i];
        g_autofree uint32_t *scalar = NULL;
        g_autofree uint32_t *mesa = NULL;
        SGIRenderResult status;
        size_t nslots;

        if (res->role == SGI_RENDER_TEXTURE) {
            continue;
        }
        scalar = sgi_render_shadow_read(native, res, &nslots, &status, &err);
        mesa = scalar ? sgi_render_shadow_read(private,
                                               &private->resources[i],
                                               &nslots, &status, &err) :
                        NULL;
        if (!mesa) {
            sgi_render_shadow_fail(shadow, out, status, err, false);
            goto done;
        }
        if (sgi_render_shadow_differs(out, i, res, scalar, mesa,
                                      coverage ? scalar_writes[i] : NULL,
                                      coverage ? rec.written[i] : NULL)) {
            out->result = SGI_RENDER_SHADOW_MISMATCH;
            trace_sgi_render_shadow_mismatch(out->signature.text, i, out->x,
                                             out->y);
            break;
        }
    }
done:
    for (size_t i = 0; rec.written && i < native->resource_count; i++) {
        g_free(rec.written[i]);
    }
    g_free(rec.written);
    error_free(err);
}

SGIRenderShadowResult sgi_render_shadow_draw(SGIRenderShadow *shadow,
                                             const SGIRasterRules *rules,
                                             const SGIRasterTarget *native,
                                             const SGIRasterTarget *private,
                                             const SGIGLDraw *draw,
                                             uint32_t phase_before,
                                             const uint8_t *const
                                             *scalar_writes,
                                             SGIRenderShadowOutcome *out)
{
    SGIGLDraw copy = *draw;
    uint32_t phase = phase_before;

    memset(out, 0, sizeof(*out));
    sgi_render_draw_signature(rules, native, draw, &out->signature);
    if (!shadow->renderer) {
        out->result = SGI_RENDER_SHADOW_DISABLED;
        return out->result;
    }
    copy.line_phase = draw->line_phase ? &phase : NULL;
    sgi_render_shadow_run(shadow, rules, native, private, &copy, NULL,
                          scalar_writes, out);
    if (out->result == SGI_RENDER_SHADOW_MATCH && draw->line_phase &&
        phase != *draw->line_phase) {
        out->result = SGI_RENDER_SHADOW_MISMATCH;
        out->phase_differs = true;
        out->scalar_phase = *draw->line_phase;
        out->mesa_phase = phase;
        trace_sgi_render_shadow_phase_mismatch(out->signature.text,
                                               out->scalar_phase, phase);
    }
    sgi_render_shadow_count(shadow, out);
    return out->result;
}

SGIRenderShadowResult sgi_render_shadow_raster(SGIRenderShadow *shadow,
                                               const SGIRasterRules *rules,
                                               const SGIRasterTarget *native,
                                               const SGIRasterTarget *private,
                                               const SGIRasterOp *op,
                                               const uint8_t *const
                                               *scalar_writes,
                                               SGIRenderShadowOutcome *out)
{
    memset(out, 0, sizeof(*out));
    sgi_render_raster_signature(rules, native, op, &out->signature);
    if (!shadow->renderer) {
        out->result = SGI_RENDER_SHADOW_DISABLED;
        return out->result;
    }
    sgi_render_shadow_run(shadow, rules, native, private, NULL, op,
                          scalar_writes, out);
    sgi_render_shadow_count(shadow, out);
    return out->result;
}

SGIRenderShadowCounts sgi_render_shadow_counts(SGIRenderShadow *shadow,
                                               const char *signature)
{
    SGIRenderShadowCounts *counts = g_hash_table_lookup(shadow->counts,
                                                        signature);

    return counts ? *counts : (SGIRenderShadowCounts) { 0 };
}

SGIRenderShadowCounts sgi_render_shadow_totals(SGIRenderShadow *shadow)
{
    return shadow->totals;
}

void sgi_render_shadow_foreach(SGIRenderShadow *shadow,
                               void (*fn)(const char *signature,
                                          const SGIRenderShadowCounts *counts,
                                          void *opaque),
                               void *opaque)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, shadow->counts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        fn(key, value, opaque);
    }
}
