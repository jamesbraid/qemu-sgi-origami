/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-kona-mesa-provider.h"

enum {
    KONA_MESA_FRONT_COLOUR,
    KONA_MESA_FRONT_INDEX,
    KONA_MESA_BACK_COLOUR,
    KONA_MESA_BACK_INDEX,
    KONA_MESA_FRONT_TAG,
    KONA_MESA_BACK_TAG,
    KONA_MESA_DEPTH,
    KONA_MESA_STENCIL,
    KONA_MESA_TEXTURE,
    KONA_MESA_RESOURCES,
};

typedef struct SGIKonaMesaBacking {
    uint32_t handle, image_id;
    bool native_fragment;
    bool touched;
    uint32_t *pixels;
    uint64_t id, generation;
} SGIKonaMesaBacking;

typedef struct SGIKonaMesaView {
    uint32_t handle, level, image_id;
    bool native_fragment;
    uint64_t id, generation;
    SGIKonaMesaBacking *backing;
} SGIKonaMesaView;

struct SGIKonaMesaProvider {
    SGIBoardMemory *memory;
    SGIKonaMesaDirtyOps dirty;
    void *dirty_opaque;
    uint32_t planes[6];
    uint8_t direct_tag;
    SGIRenderResource resources[KONA_MESA_RESOURCES];
    SGIKonaMesaTextureView texture;
    SGIKonaMesaView *texture_view;
    GHashTable *backings, *views;
    uint64_t next_id;
    bool texture_bound;
};

static unsigned board_backing(unsigned resource)
{
    static const uint8_t backing[] = { 0, 0, 1, 1, 2, 3, 4, 5 };

    return backing[resource];
}

static SGIRenderResource *resource_for_plane(SGIKonaMesaProvider *p,
                                              unsigned i)
{
    const SGIPlane *plane = sgi_board_memory_plane(
        p->memory, p->planes[board_backing(i)]);
    SGIRenderResource *resource = &p->resources[i];
    unsigned b = board_backing(i);

    *resource = (SGIRenderResource) {
        .backing_id = b + 1,
        .backing_generation = 1,
        .view_id = i + 1,
        .view_generation = 1,
        .role = i == KONA_MESA_FRONT_COLOUR || i == KONA_MESA_BACK_COLOUR ?
                SGI_RENDER_COLOUR :
                i == KONA_MESA_FRONT_INDEX || i == KONA_MESA_BACK_INDEX ?
                SGI_RENDER_INDEX :
                i == KONA_MESA_FRONT_TAG || i == KONA_MESA_BACK_TAG ?
                SGI_RENDER_TAG : i == KONA_MESA_DEPTH ? SGI_RENDER_DEPTH :
                SGI_RENDER_STENCIL,
        .target = p->planes[b],
        .buffer = i <= KONA_MESA_FRONT_INDEX ||
                  i == KONA_MESA_FRONT_TAG ? SGI_RENDER_FRONT :
                  i <= KONA_MESA_BACK_INDEX ||
                  i == KONA_MESA_BACK_TAG ? SGI_RENDER_BACK :
                  SGI_RENDER_BUFFER_NONE,
        .eye = SGI_RENDER_MONO,
        .width = plane->width,
        .height = plane->height,
        .depth = 1,
        .bottom_origin = plane->bottom_origin,
    };
    if (resource->role == SGI_RENDER_COLOUR) {
        resource->component_count = 3;
        resource->component[0] = (SGIRenderComponent) {
            SGI_RENDER_R, SGI_RENDER_UNORM, 8,
        };
        resource->component[1] = (SGIRenderComponent) {
            SGI_RENDER_G, SGI_RENDER_UNORM, 8,
        };
        resource->component[2] = (SGIRenderComponent) {
            SGI_RENDER_B, SGI_RENDER_UNORM, 8,
        };
    } else {
        resource->component_count = 1;
        resource->component[0] = (SGIRenderComponent) {
            SGI_RENDER_SCALAR,
            resource->role == SGI_RENDER_DEPTH ? SGI_RENDER_UNORM :
                                                 SGI_RENDER_UINT,
            resource->role == SGI_RENDER_INDEX ? 16 :
            resource->role == SGI_RENDER_DEPTH ? 24 : 8,
        };
    }
    return resource;
}

static bool valid_planes(SGIBoardMemory *memory,
                         const SGIKonaMesaPlanes *ids, Error **errp)
{
    const uint32_t plane_ids[] = {
        ids->front, ids->back, ids->front_tag, ids->back_tag,
        ids->depth, ids->stencil,
    };
    const SGIPlaneFormat formats[] = {
        SGI_PLANE_RGB24, SGI_PLANE_RGB24, SGI_PLANE_TAG,
        SGI_PLANE_TAG, SGI_PLANE_Z24, SGI_PLANE_S8,
    };
    const SGIPlane *front = sgi_board_memory_plane(memory, ids->front);

    if (!front || !front->width || !front->height) {
        error_setg(errp, "KONA Mesa front colour plane is unavailable");
        return false;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(plane_ids); i++) {
        const SGIPlane *plane = sgi_board_memory_plane(memory, plane_ids[i]);
        unsigned bpp = sgi_plane_bytes_per_pixel(formats[i]);

        if (!plane || plane->format != formats[i] ||
            plane->packing != (i < 2 ? SGI_PACK_LE_RED_HIGH :
                                       SGI_PACK_LE_RED_LOW) ||
            !plane->bottom_origin || plane->width != front->width ||
            plane->height != front->height ||
            (uint64_t)plane->width * bpp > plane->stride) {
            error_setg(errp, "KONA Mesa plane %u has an unsupported layout", i);
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            const SGIPlane *other = sgi_board_memory_plane(memory,
                                                             plane_ids[j]);

            if (plane_ids[i] == plane_ids[j]) {
                error_setg(errp, "KONA Mesa native planes overlap by ID");
                return false;
            }
            if (plane->base < other->base + other->len &&
                other->base < plane->base + plane->len) {
                error_setg(errp, "KONA Mesa native plane ranges overlap");
                return false;
            }
        }
    }
    return true;
}

SGIKonaMesaProvider *sgi_kona_mesa_provider_new(
    SGIBoardMemory *memory, const SGIKonaMesaPlanes *planes,
    const SGIKonaMesaDirtyOps *dirty, void *dirty_opaque, Error **errp)
{
    SGIKonaMesaProvider *p;

    if (!memory || !planes || !dirty || !dirty->plane || !dirty->texture) {
        error_setg(errp, "KONA Mesa provider needs native dirty callbacks");
        return NULL;
    }
    if (!valid_planes(memory, planes, errp)) {
        return NULL;
    }
    p = g_try_new0(SGIKonaMesaProvider, 1);
    if (!p) {
        error_setg(errp, "Cannot allocate KONA Mesa provider");
        return NULL;
    }
    p->memory = memory;
    p->dirty = *dirty;
    p->dirty_opaque = dirty_opaque;
    p->planes[0] = planes->front;
    p->planes[1] = planes->back;
    p->planes[2] = planes->front_tag;
    p->planes[3] = planes->back_tag;
    p->planes[4] = planes->depth;
    p->planes[5] = planes->stencil;
    p->direct_tag = planes->direct_tag;
    p->next_id = KONA_MESA_RESOURCES + 1;
    p->backings = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, g_free);
    p->views = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                      g_free, g_free);
    for (unsigned i = 0; i < KONA_MESA_TEXTURE; i++) {
        resource_for_plane(p, i);
    }
    return p;
}

void sgi_kona_mesa_provider_free(SGIKonaMesaProvider *p)
{
    if (p) {
        g_hash_table_destroy(p->views);
        g_hash_table_destroy(p->backings);
        g_free(p);
    }
}

const SGIRenderResource *sgi_kona_mesa_resources(
    const SGIKonaMesaProvider *p, size_t *count)
{
    *count = KONA_MESA_TEXTURE + !!p->texture_bound;
    return p->resources;
}

void sgi_kona_mesa_refresh_board_views(SGIKonaMesaProvider *p)
{
    if (!p) {
        return;
    }
    for (unsigned i = 0; i < KONA_MESA_TEXTURE; i++) {
        SGIRenderResource *resource = &p->resources[i];

        resource->backing_generation++;
        resource->view_generation++;
    }
}

void sgi_kona_mesa_resize_board_views(SGIKonaMesaProvider *p)
{
    if (!p) {
        return;
    }
    for (unsigned i = 0; i < KONA_MESA_TEXTURE; i++) {
        const SGIPlane *plane = sgi_board_memory_plane(
            p->memory, p->planes[board_backing(i)]);
        SGIRenderResource *resource = &p->resources[i];

        resource->width = plane->width;
        resource->height = plane->height;
        resource->backing_generation++;
        resource->view_generation++;
    }
}

static uint64_t backing_key(uint32_t handle, uint32_t image_id, bool native)
{
    return ((uint64_t)handle << 24) | ((uint64_t)native << 23) | image_id;
}

static uint64_t view_key(uint32_t handle, uint32_t level,
                         uint32_t image_id, bool native)
{
    return ((uint64_t)handle << 32) | ((uint64_t)native << 31) |
           ((uint64_t)level << 23) | image_id;
}

static void texture_backing_changed(SGIKonaMesaProvider *p,
                                     SGIKonaMesaBacking *backing);

static SGIKonaMesaBacking *texture_backing(SGIKonaMesaProvider *p,
                                            const SGIKonaMesaTextureView *view)
{
    uint64_t key = backing_key(view->handle, view->image_id,
                                view->native_fragment);
    SGIKonaMesaBacking *b = g_hash_table_lookup(p->backings, &key);

    if (!b) {
        uint64_t *stored_key = g_new(uint64_t, 1);

        b = g_new0(SGIKonaMesaBacking, 1);
        *stored_key = key;
        b->handle = view->handle;
        b->image_id = view->image_id;
        b->native_fragment = view->native_fragment;
        b->pixels = view->pixels;
        b->id = p->next_id++;
        b->generation = 1;
        g_hash_table_insert(p->backings, stored_key, b);
    } else if (b->pixels != view->pixels) {
        b->pixels = view->pixels;
        texture_backing_changed(p, b);
    }
    return b;
}

static SGIKonaMesaView *texture_view(SGIKonaMesaProvider *p,
                                      const SGIKonaMesaTextureView *view,
                                      SGIKonaMesaBacking *backing)
{
    uint64_t key = view_key(view->handle, view->level, view->image_id,
                            view->native_fragment);
    SGIKonaMesaView *v = g_hash_table_lookup(p->views, &key);

    if (!v) {
        uint64_t *stored_key = g_new(uint64_t, 1);

        v = g_new0(SGIKonaMesaView, 1);
        *stored_key = key;
        v->handle = view->handle;
        v->level = view->level;
        v->image_id = view->image_id;
        v->native_fragment = view->native_fragment;
        v->id = p->next_id++;
        v->generation = 1;
        v->backing = backing;
        g_hash_table_insert(p->views, stored_key, v);
    } else {
        v->generation++;
    }
    return v;
}

bool sgi_kona_mesa_bind_texture(SGIKonaMesaProvider *p,
                                const SGIKonaMesaTextureView *view,
                                Error **errp)
{
    SGIKonaMesaBacking *backing;
    SGIKonaMesaView *v;
    SGIRenderResource *r;

    if (!p || !view || !view->pixels || !view->width || !view->height ||
        !view->depth || view->level >= SGI_GL_TEX_LEVELS_MAX ||
        view->image_id >= (1u << 23) ||
        (view->coverage && !view->covered) ||
        (uint64_t)view->width * view->height >
        SIZE_MAX / sizeof(*view->pixels) / view->depth ||
        (uint64_t)view->border[0] * 2 >= view->width ||
        (uint64_t)view->border[1] * 2 >= view->height ||
        (uint64_t)view->border[2] * 2 >= view->depth) {
        error_setg(errp, "KONA Mesa texture view is invalid");
        return false;
    }
    backing = texture_backing(p, view);
    v = texture_view(p, view, backing);
    p->texture = *view;
    p->texture_view = v;
    p->texture_bound = true;
    r = &p->resources[KONA_MESA_TEXTURE];
    *r = (SGIRenderResource) {
        .backing_id = backing->id,
        .backing_generation = backing->generation,
        .view_id = v->id,
        .view_generation = v->generation,
        .role = SGI_RENDER_TEXTURE,
        .buffer = SGI_RENDER_BUFFER_NONE,
        .eye = SGI_RENDER_MONO,
        .width = view->width,
        .height = view->height,
        .depth = view->depth,
        .texture_level = view->level,
        .bottom_origin = view->bottom_origin,
        .component_count = 4,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
        },
    };
    memcpy(r->border, view->border, sizeof(r->border));
    return true;
}

void sgi_kona_mesa_unbind_texture(SGIKonaMesaProvider *p)
{
    if (p) {
        p->texture_bound = false;
        p->texture_view = NULL;
        memset(&p->texture, 0, sizeof(p->texture));
    }
}

void sgi_kona_mesa_touch_plane(SGIKonaMesaProvider *p, uint32_t id,
                               uint32_t y0, uint32_t y1)
{
    if (!p) {
        return;
    }
    for (unsigned i = 0; i < KONA_MESA_TEXTURE; i++) {
        SGIRenderResource *r = &p->resources[i];

        if (r->target == id) {
            r->backing_generation++;
            r->view_generation++;
        }
    }
    if (y0 <= y1 && sgi_board_memory_plane(p->memory, id)) {
        p->dirty.plane(p->dirty_opaque, id, y0, y1);
    }
}

static void texture_backing_changed(SGIKonaMesaProvider *p,
                                     SGIKonaMesaBacking *backing)
{
    GHashTableIter iter;
    gpointer value;

    backing->generation++;
    g_hash_table_iter_init(&iter, p->views);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaMesaView *v = value;

        if (v->backing == backing) {
            v->generation++;
            p->dirty.texture(p->dirty_opaque, v->handle, v->level,
                             v->image_id);
        }
    }
    if (p->texture_bound && p->texture_view->backing == backing) {
        p->resources[KONA_MESA_TEXTURE].backing_generation =
            backing->generation;
        p->resources[KONA_MESA_TEXTURE].view_generation =
            p->texture_view->generation;
    }
}

void sgi_kona_mesa_touch_texture(SGIKonaMesaProvider *p,
                                 uint32_t handle, int level)
{
    GHashTableIter iter;
    gpointer value;

    if (!p) {
        return;
    }
    g_hash_table_iter_init(&iter, p->views);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaMesaView *v = value;

        if (v->handle == handle && (level < 0 || v->level == level)) {
            v->backing->touched = true;
        }
    }
    g_hash_table_iter_init(&iter, p->backings);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        SGIKonaMesaBacking *b = value;

        if (b->touched) {
            b->touched = false;
            texture_backing_changed(p, b);
        }
    }
    if (p->texture_bound && p->texture.handle == handle &&
        (level < 0 || p->texture.level == level)) {
        sgi_kona_mesa_unbind_texture(p);
    }
}

static int resource_index(SGIKonaMesaProvider *p,
                          const SGIRenderResource *resource)
{
    if (!p || !resource) {
        return -1;
    }
    for (unsigned i = 0; i < KONA_MESA_RESOURCES; i++) {
        if (resource == &p->resources[i]) {
            return i == KONA_MESA_TEXTURE && !p->texture_bound ? -1 : i;
        }
    }
    return -1;
}

static bool board_word(SGIKonaMesaProvider *p, unsigned i, uint32_t x,
                       uint32_t y, uint32_t *word)
{
    const SGIPlane *plane = sgi_board_memory_plane(p->memory,
                              p->planes[board_backing(i)]);
    unsigned bpp = sgi_plane_bytes_per_pixel(plane->format);
    uint64_t offset = plane->base + (uint64_t)y * plane->stride +
                      (uint64_t)x * bpp;
    uint8_t bytes[4];

    if (!sgi_board_memory_export(p->memory, offset, bpp, bytes)) {
        return false;
    }
    *word = sgi_plane_decode(plane, bytes);
    return true;
}

static bool board_store(SGIKonaMesaProvider *p, unsigned i, uint32_t x,
                        uint32_t y, uint32_t word)
{
    const SGIPlane *plane = sgi_board_memory_plane(p->memory,
                              p->planes[board_backing(i)]);
    unsigned bpp = sgi_plane_bytes_per_pixel(plane->format);
    uint64_t offset = plane->base + (uint64_t)y * plane->stride +
                      (uint64_t)x * bpp;
    uint8_t bytes[4];

    sgi_plane_encode(plane, word, bytes);
    return sgi_board_memory_import(p->memory, offset, bpp, bytes);
}

static bool texture_word(SGIKonaMesaProvider *p, uint32_t slice,
                         uint32_t x, uint32_t y, uint32_t *word)
{
    const SGIKonaMesaTextureView *view = &p->texture;
    size_t at = ((size_t)slice * view->height + y) * view->width + x;

    if (view->coverage && !(view->coverage[at / 8] & (1u << (at % 8)))) {
        return false;
    }
    *word = view->pixels[at];
    return true;
}

static void texture_store(SGIKonaMesaProvider *p, uint32_t slice,
                          uint32_t x, uint32_t y, uint32_t word)
{
    SGIKonaMesaTextureView *view = &p->texture;
    size_t at = ((size_t)slice * view->height + y) * view->width + x;

    view->pixels[at] = word;
    if (view->coverage && !(view->coverage[at / 8] & (1u << (at % 8)))) {
        view->coverage[at / 8] |= 1u << (at % 8);
        (*view->covered)++;
    }
}

static uint32_t unpack_slot(unsigned i, uint32_t word, unsigned channel)
{
    if (i == KONA_MESA_FRONT_COLOUR || i == KONA_MESA_BACK_COLOUR) {
        return (word >> (16 - 8 * channel)) & 0xff;
    }
    if (i == KONA_MESA_TEXTURE) {
        return (word >> (24 - 8 * channel)) & 0xff;
    }
    return i == KONA_MESA_FRONT_INDEX || i == KONA_MESA_BACK_INDEX ?
           word & 0xffff : i == KONA_MESA_DEPTH ? word & 0xffffff :
           word & 0xff;
}

static uint32_t pack_slots(unsigned i, const uint32_t *slots)
{
    if (i == KONA_MESA_FRONT_COLOUR || i == KONA_MESA_BACK_COLOUR) {
        return slots[0] << 16 | slots[1] << 8 | slots[2];
    }
    if (i == KONA_MESA_TEXTURE) {
        return slots[0] << 24 | slots[1] << 16 |
               slots[2] << 8 | slots[3];
    }
    return slots[0];
}

static SGIRenderResult kona_mesa_snapshot(void *opaque,
    const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    SGIKonaMesaProvider *p = opaque;
    int i = resource_index(p, resource);
    if (i < 0) {
        error_setg(errp, "KONA Mesa resource is no longer bound");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint64_t y = rect->y0; y <= rect->y1; y++) {
        for (uint64_t x = rect->x0; x <= rect->x1; x++) {
            uint32_t word;
            size_t at = (size_t)(y - rect->y0) * stride_slots +
                        (x - rect->x0) * resource->component_count;

            if (!(i == KONA_MESA_TEXTURE ?
                  texture_word(p, slice, x, y, &word) :
                  board_word(p, i, x, y, &word))) {
                error_setg(errp, "KONA Mesa native snapshot failed at "
                           "(%" PRIu64 ", %" PRIu64 ")", x, y);
                return SGI_RENDER_RESOURCE_ERROR;
            }
            for (unsigned c = 0; c < resource->component_count; c++) {
                slots[at + c] = unpack_slot(i, word, c);
            }
        }
    }
    (void)capacity_slots;
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult kona_mesa_commit(void *opaque,
    const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    SGIKonaMesaProvider *p = opaque;
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    int i = resource_index(p, resource);
    size_t width = (uint64_t)rect->x1 - rect->x0 + 1;

    if (i < 0) {
        error_setg(errp, "KONA Mesa resource is no longer bound");
        return result;
    }
    for (uint64_t y = rect->y0; y <= rect->y1; y++) {
        for (uint64_t x = rect->x0; x <= rect->x1; x++) {
            size_t at = (size_t)(y - rect->y0) * stride_slots +
                        (x - rect->x0) * resource->component_count;
            uint32_t word = pack_slots(i, &slots[at]);

            if (i == KONA_MESA_TEXTURE) {
                texture_store(p, slice, x, y, word);
            } else if (!board_store(p, i, x, y, word)) {
                if (result.complete_pixels) {
                    uint32_t last_y = rect->y0 +
                                      (result.complete_pixels - 1) / width;

                    sgi_kona_mesa_touch_plane(p, p->planes[board_backing(i)],
                                               rect->y0, last_y);
                }
                error_setg(errp, "KONA Mesa native commit failed at "
                           "(%" PRIu64 ", %" PRIu64 ")", x, y);
                return result;
            }
            result.complete_pixels++;
        }
    }
    if (i == KONA_MESA_TEXTURE) {
        texture_backing_changed(p, p->texture_view->backing);
    } else {
        sgi_kona_mesa_touch_plane(p, p->planes[board_backing(i)],
                                   rect->y0, rect->y1);
    }
    result.status = SGI_RENDER_OK;
    (void)capacity_slots;
    return result;
}

static SGIRenderCommitResult kona_mesa_commit_covered(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    SGIKonaMesaProvider *p = opaque;
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    int i = resource_index(p, resource);
    uint32_t tag_plane, colour_last_y = 0, tag_last_y = 0;
    bool colour_changed = false, tag_changed = false;

    if (i != KONA_MESA_FRONT_COLOUR && i != KONA_MESA_BACK_COLOUR) {
        error_setg(errp, "KONA Mesa covered commit needs a colour bank");
        result.status = SGI_RENDER_UNSUPPORTED;
        return result;
    }
    tag_plane = p->planes[i == KONA_MESA_FRONT_COLOUR ? 2 : 3];
    for (uint64_t y = rect->y0; y <= rect->y1; y++) {
        for (uint64_t x = rect->x0; x <= rect->x1; x++) {
            size_t row = y - rect->y0;
            size_t col = x - rect->x0;
            size_t at = row * stride_slots + col * resource->component_count;

            if (!writes->pixel[row * writes->stride_bytes + col]) {
                result.complete_pixels++;
                continue;
            }
            if (!board_store(p, i, x, y, pack_slots(i, &slots[at]))) {
                error_setg(errp, "KONA Mesa colour commit failed at "
                           "(%" PRIu64 ", %" PRIu64 ")", x, y);
                goto failed;
            }
            colour_changed = true;
            colour_last_y = y;
            if (!board_store(p, i == KONA_MESA_FRONT_COLOUR ?
                             KONA_MESA_FRONT_TAG : KONA_MESA_BACK_TAG,
                             x, y, p->direct_tag)) {
                result.partial_pixel = true;
                error_setg(errp, "KONA Mesa tag commit failed at "
                           "(%" PRIu64 ", %" PRIu64 ")", x, y);
                goto failed;
            }
            tag_changed = true;
            tag_last_y = y;
            result.complete_pixels++;
        }
    }
    result.status = SGI_RENDER_OK;

failed:
    if (colour_changed) {
        sgi_kona_mesa_touch_plane(p, p->planes[board_backing(i)],
                                   rect->y0, colour_last_y);
    }
    if (tag_changed) {
        sgi_kona_mesa_touch_plane(p, tag_plane, rect->y0, tag_last_y);
    }
    (void)slice;
    (void)capacity_slots;
    return result;
}

const SGIRenderResourceOps *sgi_kona_mesa_resource_ops(void)
{
    static const SGIRenderResourceOps ops = {
        .snapshot = kona_mesa_snapshot,
        .commit = kona_mesa_commit,
        .commit_covered = kona_mesa_commit_covered,
    };

    return &ops;
}
