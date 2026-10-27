/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics: the geometry of an OpenGL selection hit. See the header
 * for the contract.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/display/sgi/gl-select.h"
#include "hw/display/sgi/rasterizer.h"

static bool sgi_gl_select_clip_vertex(const SGIGLDraw *d,
                                      const float *vertex,
                                      SGIVolumeVertex *out)
{
    double object[4], eye[4] = { 0 };

    *out = (SGIVolumeVertex){ 0 };

    for (int i = 0; i < 4; i++) {
        object[i] = vertex[SGI_GL_V_POSITION + i];
        if (!isfinite(object[i])) {
            return false;
        }
    }
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            eye[row] += d->state->modelview[col * 4 + row] * object[col];
        }
        if (!isfinite(eye[row])) {
            return false;
        }
    }
    sgi_gl_eye_to_clip(d->state, eye, out->clip);
    if (!isfinite(out->clip[0]) || !isfinite(out->clip[1]) ||
        !isfinite(out->clip[2]) || !isfinite(out->clip[3])) {
        return false;
    }
    memcpy(out->eye, eye, sizeof(eye));
    return true;
}

static bool sgi_gl_select_point_depth(const SGIGLDraw *d,
                                      const SGIVolumeVertex *vertex,
                                      double *depth)
{
    double window[3];

    if (!sgi_volume_contains(vertex)) {
        return false;
    }
    sgi_gl_clip_to_window(d->state, vertex->clip, window);
    *depth = d->state->depth_range[0] +
             window[2] * (d->state->depth_range[1] -
                          d->state->depth_range[0]);
    return isfinite(*depth);
}

static bool sgi_gl_select_vertex_depth(const SGIGLDraw *d,
                                       const SGIVolumeVertex *vertex,
                                       double *depth)
{
    double window[3];

    sgi_gl_clip_to_window(d->state, vertex->clip, window);
    *depth = d->state->depth_range[0] +
             window[2] * (d->state->depth_range[1] -
                          d->state->depth_range[0]);
    return isfinite(*depth);
}

static bool sgi_gl_select_line_depth(const SGIGLDraw *d,
                                     SGIVolumeVertex a,
                                     SGIVolumeVertex b,
                                     double *minimum, double *maximum)
{
    double da, db;

    if (!sgi_volume_clip_segment(&a, &b) ||
        !sgi_gl_select_vertex_depth(d, &a, &da) ||
        !sgi_gl_select_vertex_depth(d, &b, &db)) {
        return false;
    }
    *minimum = MIN(da, db);
    *maximum = MAX(da, db);
    return true;
}

/* Clip a convex polygon as a triangle fan, retaining its projected winding. */
static bool sgi_gl_select_polygon_depth(const SGIGLDraw *d,
                                        uint32_t first,
                                        const unsigned *order,
                                        unsigned nvertices,
                                        double *minimum,
                                        double *maximum,
                                        double *area)
{
    bool hit = false;

    *area = 0;
    for (unsigned tri = 1; tri + 1 < nvertices; tri++) {
        SGIVolumeVertex in[3], clipped[SGI_VOLUME_TRIANGLE_MAX];
        double window[SGI_VOLUME_TRIANGLE_MAX][3];
        unsigned indices[3] = { 0, tri, tri + 1 };
        unsigned count;

        for (unsigned i = 0; i < ARRAY_SIZE(in); i++) {
            unsigned vertex_index = order ? order[indices[i]] : indices[i];
            const float *vertex = d->vertices +
                                  (first + vertex_index) * SGI_GL_VERTEX_FLOATS;

            if (!sgi_gl_select_clip_vertex(d, vertex, &in[i])) {
                return false;
            }
        }
        count = sgi_volume_clip_triangle(in, clipped);
        if (count < 3) {
            continue;
        }
        for (unsigned i = 0; i < count; i++) {
            double depth;

            sgi_gl_clip_to_window(d->state, clipped[i].clip, window[i]);
            if (!sgi_gl_select_vertex_depth(d, &clipped[i], &depth)) {
                return false;
            }
            *minimum = hit ? MIN(*minimum, depth) : depth;
            *maximum = hit ? MAX(*maximum, depth) : depth;
            hit = true;
        }
        for (unsigned i = 0; i < count; i++) {
            const double *a = window[i];
            const double *b = window[(i + 1) % count];

            *area += a[0] * b[1] - b[0] * a[1];
        }
    }
    return hit;
}

static bool sgi_gl_select_polygon_culled(const SGIGLDraw *d,
                                         double area)
{
    bool front = d->state->front_face == SGI_GL_CW ? area < 0 : area > 0;

    return d->state->cull_face &&
           (d->state->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
            d->state->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK));
}

static void sgi_gl_select_quad(const SGIGLDraw *d, uint32_t first, bool strip,
                               SGIGLSelectHit *hit, void *opaque)
{
    static const unsigned strip_order[] = { 0, 1, 3, 2 };
    const unsigned *order = strip ? strip_order : NULL;
    double minimum = 0, maximum = 0, area;

    if (sgi_gl_select_polygon_depth(d, first, order, 4,
                                    &minimum, &maximum, &area) &&
        !sgi_gl_select_polygon_culled(d, area)) {
        hit(opaque, minimum, maximum);
    }
}

SGIGLSelectResult sgi_gl_select_draw(const SGIGLDraw *d, SGIGLSelectHit *hit,
                                     void *opaque)
{
    if (!d->count || !d->vertices || d->state->window_matrix_given ||
        d->state->clip_planes) {
        return SGI_GL_SELECT_UNSUPPORTED_STATE;
    }
    switch (d->mode) {
    case SGI_GL_POINTS:
        for (uint32_t i = 0; i < d->count; i++) {
            const float *vertex = d->vertices + i * SGI_GL_VERTEX_FLOATS;
            SGIVolumeVertex point;
            double depth;

            if (sgi_gl_select_clip_vertex(d, vertex, &point) &&
                sgi_gl_select_point_depth(d, &point, &depth)) {
                hit(opaque, depth, depth);
            }
        }
        break;
    case SGI_GL_LINES:
        if (d->count % 2) {
            break;
        }
        for (uint32_t i = 0; i < d->count; i += 2) {
            SGIVolumeVertex line[2];
            double minimum = 0, maximum = 0;

            if (sgi_gl_select_clip_vertex(
                    d, d->vertices + i * SGI_GL_VERTEX_FLOATS, &line[0]) &&
                sgi_gl_select_clip_vertex(
                    d, d->vertices + (i + 1) * SGI_GL_VERTEX_FLOATS,
                    &line[1]) &&
                sgi_gl_select_line_depth(d, line[0], line[1],
                                         &minimum, &maximum)) {
                hit(opaque, minimum, maximum);
            }
        }
        break;
    case SGI_GL_TRIANGLES:
        if (d->count % 3) {
            break;
        }
        for (uint32_t i = 0; i < d->count; i += 3) {
            double minimum = 0, maximum = 0, area;

            if (sgi_gl_select_polygon_depth(d, i, NULL, 3,
                                            &minimum, &maximum, &area) &&
                !sgi_gl_select_polygon_culled(d, area)) {
                hit(opaque, minimum, maximum);
            }
        }
        break;
    case SGI_GL_QUADS:
        if (d->count % 4) {
            break;
        }
        for (uint32_t i = 0; i < d->count; i += 4) {
            sgi_gl_select_quad(d, i, false, hit, opaque);
        }
        break;
    case SGI_GL_QUAD_STRIP:
        if (d->count < 4 || d->count % 2) {
            break;
        }
        for (uint32_t i = 0; i + 3 < d->count; i += 2) {
            sgi_gl_select_quad(d, i, true, hit, opaque);
        }
        break;
    case SGI_GL_POLYGON: {
        double minimum = 0, maximum = 0, area;

        if (d->count >= 3 &&
            sgi_gl_select_polygon_depth(d, 0, NULL, d->count,
                                        &minimum, &maximum, &area) &&
            !sgi_gl_select_polygon_culled(d, area)) {
            hit(opaque, minimum, maximum);
        }
        break;
    }
    default:
        return SGI_GL_SELECT_UNSUPPORTED_MODE;
    }
    return SGI_GL_SELECT_OK;
}
