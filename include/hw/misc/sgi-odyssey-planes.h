/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_ODYSSEY_PLANES_H
#define HW_MISC_SGI_ODYSSEY_PLANES_H

#include "hw/display/sgi/rasterizer.h"

/* This format describes synthetic host bytes, not Odyssey board memory. */
typedef enum OdysseyPlaneFormat {
    ODYSSEY_PLANE_SYNTH_RGBA8,
    ODYSSEY_PLANE_SYNTH_RGBA4,
    ODYSSEY_PLANE_SYNTH_RGB5_A1,
    ODYSSEY_PLANE_SYNTH_RGB10_A2,
    ODYSSEY_PLANE_SYNTH_RGBA12_A12,
    ODYSSEY_PLANE_SYNTH_CI8,
    ODYSSEY_PLANE_SYNTH_CI12,
} OdysseyPlaneFormat;

typedef bool (*OdysseyPlanePredicate)(void *opaque, uint32_t x, uint32_t y);
typedef void (*OdysseyPlaneMarkWritten)(void *opaque, uint32_t x, uint32_t y);

typedef struct OdysseyPlaneRawBinding {
    OdysseyPlaneFormat format;
    SGIRenderRole role;
    SGIColourTarget target;
    SGIRenderBuffer buffer;
    SGIRenderEye eye;
    uint32_t width, height;
    size_t stride_bytes, capacity_bytes;
    uint64_t backing_id, view_id;
    bool bottom_origin;
} OdysseyPlaneRawBinding;

/* The caller assigns targets; resource rows follow bottom_origin. */
typedef struct OdysseyPlaneView {
    uint8_t *bytes;
    size_t capacity_bytes;
    size_t stride_bytes;
    uint32_t width, height;
    OdysseyPlaneFormat format;
    SGIRenderRole role;
    SGIColourTarget target;
    SGIRenderBuffer buffer;
    SGIRenderEye eye;
    bool bottom_origin;
    /* Top-first backing rows and callbacks; false keeps plane row order. */
    bool storage_top_origin;
    uint64_t backing_id, backing_generation;
    uint64_t view_id, view_generation;
    uint8_t channel_mask;
    uint16_t index_mask;
    bool dirty;
    void *opaque;
    /* Written pixels are readable; ownership only limits writes. */
    OdysseyPlanePredicate is_written;
    OdysseyPlanePredicate owns_pixel;
    OdysseyPlaneMarkWritten mark_written;
    /* Set by the provider for one synchronous submission. */
    uint8_t *submitted_bytes;
    bool submitted_storage_top_origin;
    uint64_t submitted_backing_generation, submitted_view_generation;
    uint64_t expected_backing_generation, expected_view_generation;
    OdysseyPlaneRawBinding submitted_raw;
} OdysseyPlaneView;

typedef struct OdysseyPlanes {
    OdysseyPlaneView *views;
    SGIRenderResource *resources;
    size_t count;
    bool initialized;
    bool active;
} OdysseyPlanes;

typedef enum OdysseyPlaneRawStatus {
    ODYSSEY_PLANE_RAW_OK,
    ODYSSEY_PLANE_RAW_INVALID,
    ODYSSEY_PLANE_RAW_UNSUPPORTED,
    ODYSSEY_PLANE_RAW_NO_MEMORY,
    ODYSSEY_PLANE_RAW_RESOURCE,
    ODYSSEY_PLANE_RAW_HOST,
    ODYSSEY_PLANE_RAW_NOT_RUN,
} OdysseyPlaneRawStatus;

typedef struct OdysseyPlaneRawRef {
    size_t view_index;
    SGIRenderBuffer buffer;
    SGIRenderEye eye;
    uint64_t backing_generation, view_generation;
} OdysseyPlaneRawRef;

typedef struct OdysseyPlaneRawWrite {
    OdysseyPlaneRawRef ref;
    SGIRenderRect rect;
    const uint32_t *slots;
    size_t stride_slots, capacity_slots;
    const SGIRenderWriteCoverage *coverage;
} OdysseyPlaneRawWrite;

typedef struct OdysseyPlaneRawResult {
    OdysseyPlaneRawStatus status;
    size_t complete_pixels;
    bool extent_known;
    bool partial_pixel;
    uint8_t component_mask;
} OdysseyPlaneRawResult;

/* Initialize a zeroed provider once; a new submission needs fresh descriptors. */
bool odyssey_planes_init(OdysseyPlanes *planes, OdysseyPlaneView *views,
                         SGIRenderResource *resources, size_t count,
                         Error **errp);
bool odyssey_planes_refresh(OdysseyPlanes *planes,
                            SGIRenderResource *new_resources, Error **errp);
void odyssey_planes_end_submission(OdysseyPlanes *planes);
/* Exchange logical front/back bindings for both eyes without copying pixels. */
bool odyssey_planes_swap_bindings(OdysseyPlanes *planes, Error **errp);
const SGIRenderResourceOps *odyssey_planes_resource_ops(void);

/* Raw slots contain unsigned R,G,B,A scalars or one unsigned index. */
bool odyssey_planes_raw_init(OdysseyPlanes *planes, OdysseyPlaneView *views,
                             size_t count, Error **errp);
OdysseyPlaneRawStatus odyssey_planes_raw_snapshot(
    OdysseyPlanes *planes, const OdysseyPlaneRawRef *ref,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp);
OdysseyPlaneRawStatus odyssey_planes_raw_commit_many(
    OdysseyPlanes *planes, const OdysseyPlaneRawWrite *writes, size_t count,
    OdysseyPlaneRawResult *results, Error **errp);
OdysseyPlaneRawStatus odyssey_planes_raw_copy(
    OdysseyPlanes *planes, const OdysseyPlaneRawRef *source,
    const SGIRenderRect *source_rect, const OdysseyPlaneRawRef *destination,
    const SGIRenderRect *destination_rect, OdysseyPlaneRawResult *result,
    Error **errp);

#endif
