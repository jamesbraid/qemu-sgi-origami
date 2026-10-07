/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_ODYSSEY_PLANES_H
#define HW_MISC_SGI_ODYSSEY_PLANES_H

#include "hw/display/sgi/rasterizer.h"

/* This format describes synthetic host bytes, not Odyssey board memory. */
typedef enum OdysseyPlaneFormat {
    ODYSSEY_PLANE_SYNTH_RGBA8,
} OdysseyPlaneFormat;

typedef bool (*OdysseyPlanePredicate)(void *opaque, uint32_t x, uint32_t y);
typedef void (*OdysseyPlaneMarkWritten)(void *opaque, uint32_t x, uint32_t y);

/* The caller assigns targets for one drawable; plane coordinates never flip. */
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
    uint64_t backing_id, backing_generation;
    uint64_t view_id, view_generation;
    uint8_t channel_mask;
    bool dirty;
    void *opaque;
    OdysseyPlanePredicate is_written;
    OdysseyPlanePredicate owns_pixel;
    OdysseyPlaneMarkWritten mark_written;
    /* Set by the provider for one synchronous submission. */
    uint8_t *submitted_bytes;
    uint64_t submitted_backing_generation, submitted_view_generation;
    uint64_t expected_backing_generation, expected_view_generation;
} OdysseyPlaneView;

typedef struct OdysseyPlanes {
    OdysseyPlaneView *views;
    SGIRenderResource *resources;
    size_t count;
    bool initialized;
    bool active;
} OdysseyPlanes;

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

#endif
