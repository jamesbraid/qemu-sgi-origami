/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics board memory and the planes over it.
 *
 * SGIBoardMemory is one head's pixel and buffer memory, laid out as the
 * guest's allocator lays it out. It has one of two backings, which the
 * family chooses:
 *
 *   device-backed   a byte array of the size the family's descriptor
 *                   gives, owned here (MGRAS, InfiniteReality);
 *   page-backed     guest RAM: a list of fixed-size guest pages the driver
 *                   pinned, which the family resolves to host pointers
 *                   under the big lock (Odyssey).
 *
 * An SGIPlane is a typed window over that memory: format, size, stride,
 * packing and row origin. Planes may overlap (a tag plane beside a
 * polymorphic colour plane, two banks over one depth plane). A plane is
 * addressed in its own coordinates: pixel (x, y) is at byte
 *
 *     base + y * stride + x * sgi_plane_bytes_per_pixel(format)
 *
 * whatever the row origin. bottom_origin says only how memory rows reach
 * the monitor: when it is set, plane row 0 is the bottom display line, and
 * sgi_scanout_invalidate_plane() maps rows accordingly.
 *
 * A plane word is the native value the family writes, assembled from the
 * plane's bytes in the byte order its packing names. The red-low or
 * red-high half of the packing is a label for the family's resolve and
 * conversion hooks; nothing here reorders channels.
 *
 * Out-of-plane loads read zero and out-of-plane stores are dropped: the
 * shared code cannot know whether an access outside a plane is the guest's
 * error or the family's, so the family checks and logs before it asks.
 */
#ifndef HW_DISPLAY_SGI_BOARD_MEMORY_H
#define HW_DISPLAY_SGI_BOARD_MEMORY_H

#include "migration/vmstate.h"

#define SGI_PLANE_NONE UINT32_MAX
#define SGI_PLANES_MAX 32

typedef enum SGIPlaneFormat {
    SGI_PLANE_CI8,          /* 1 byte: colour index */
    SGI_PLANE_RGB12,        /* 2 bytes: 12-bit colour in a 16-bit word */
    SGI_PLANE_RGB24,        /* 4 bytes: 24-bit colour in a 32-bit word */
    SGI_PLANE_RGBA8,        /* 4 bytes */
    SGI_PLANE_Z24,          /* 4 bytes: 24-bit depth in a 32-bit word */
    SGI_PLANE_S8,           /* 1 byte: stencil */
    SGI_PLANE_TAG,          /* 1 byte: per-pixel tag */
    SGI_PLANE_OVERLAY8,     /* 1 byte */
    SGI_PLANE_TEXTURE,      /* 4 bytes */
    SGI_PLANE_RAW,          /* 4 bytes: a word the family interprets */
} SGIPlaneFormat;

typedef enum SGIPacking {
    SGI_PACK_LE_RED_LOW,
    SGI_PACK_LE_RED_HIGH,
    SGI_PACK_BE_RED_LOW,
    SGI_PACK_BE_RED_HIGH,
} SGIPacking;

typedef struct SGIPlane {
    uint32_t id;                /* index in the board's plane table */
    uint64_t base, len;         /* bytes within the board memory */
    uint32_t width, height, stride;
    SGIPlaneFormat format;
    SGIPacking packing;         /* what the model writes, labelled */
    bool bottom_origin;         /* plane row 0 is the bottom display line */
    const char *evidence;       /* where the layout comes from */
} SGIPlane;

typedef struct SGIBoardPage {
    void *host;                 /* NULL until the family resolves it */
    uint64_t guest;             /* the page's guest address */
} SGIBoardPage;

typedef struct SGIBoardMemory {
    uint64_t size;
    uint8_t *bytes;             /* device-backed, else NULL */
    uint32_t bytes_len;         /* size when device-backed, else 0 */
    SGIBoardPage *pages;        /* page-backed, else NULL */
    uint32_t npages;
    uint32_t page_size;
    /* A fixed table, so a plane pointer stays valid for the memory's life. */
    SGIPlane planes[SGI_PLANES_MAX];
    uint32_t nplanes;
} SGIBoardMemory;

/*
 * Up to 16 inclusive rectangles, in plane coordinates. With n 0 the list
 * is no clip, keeping every pixel, unless empty is set: then it keeps no
 * pixel, as for a window that other windows wholly cover. A zeroed list
 * is no clip.
 */
#define SGI_CLIP_MAX 16

typedef struct SGIClipRect {
    int32_t x0, y0, x1, y1;
} SGIClipRect;

typedef struct SGIClipList {
    uint32_t n;
    uint32_t empty;             /* 0 or 1; with n 0, 1 keeps nothing */
    SGIClipRect r[SGI_CLIP_MAX];
} SGIClipList;

/* The sixteen logic ops, in OpenGL's order (GL_CLEAR + op). */
enum {
    SGI_LOGIC_CLEAR, SGI_LOGIC_AND, SGI_LOGIC_AND_REVERSE, SGI_LOGIC_COPY,
    SGI_LOGIC_AND_INVERTED, SGI_LOGIC_NOOP, SGI_LOGIC_XOR, SGI_LOGIC_OR,
    SGI_LOGIC_NOR, SGI_LOGIC_EQUIV, SGI_LOGIC_INVERT, SGI_LOGIC_OR_REVERSE,
    SGI_LOGIC_COPY_INVERTED, SGI_LOGIC_OR_INVERTED, SGI_LOGIC_NAND,
    SGI_LOGIC_SET,
};

/* A zeroed device-backed memory of size bytes, at most 4 GiB - 1. */
void sgi_board_memory_init(SGIBoardMemory *m, uint64_t size);
/* A page-backed memory of npages unresolved pages of page_size bytes. */
void sgi_board_memory_init_pages(SGIBoardMemory *m, uint32_t npages,
                                 uint32_t page_size);
/* Page i is guest address guest, reached at host (NULL: unresolved). */
void sgi_board_memory_set_page(SGIBoardMemory *m, uint32_t i, uint64_t guest,
                               void *host);
void sgi_board_memory_cleanup(SGIBoardMemory *m);

unsigned sgi_plane_bytes_per_pixel(SGIPlaneFormat format);

/*
 * Declare a plane. Its id is assigned here and returned; the rest of
 * *plane is copied. Fails, returning SGI_PLANE_NONE, when the plane does
 * not fit the memory, its rows do not fit its stride and length, or the
 * table already holds SGI_PLANES_MAX planes.
 */
uint32_t sgi_board_memory_add_plane(SGIBoardMemory *m, const SGIPlane *plane,
                                    Error **errp);
const SGIPlane *sgi_board_memory_plane(const SGIBoardMemory *m, uint32_t id);

static inline bool sgi_plane_contains(const SGIPlane *p, int x, int y)
{
    return x >= 0 && y >= 0 && (uint32_t)x < p->width &&
           (uint32_t)y < p->height;
}

bool sgi_clip_contains(const SGIClipList *clip, int x, int y);
uint32_t sgi_logic_op(uint8_t op, uint32_t src, uint32_t dst);

uint32_t sgi_plane_load(const SGIBoardMemory *m, const SGIPlane *p,
                        int x, int y);
void sgi_plane_store(SGIBoardMemory *m, const SGIPlane *p, int x, int y,
                     uint32_t word);
/*
 * Store op(src, dst) at (x, y) through the write mask: bits set in mask
 * take the result, the others keep the plane's word.
 */
void sgi_plane_merge(SGIBoardMemory *m, const SGIPlane *p, int x, int y,
                     uint32_t src, uint8_t op, uint32_t mask);

/* A plane word to and from its stored bytes, in the plane's packing. */
uint32_t sgi_plane_decode(const SGIPlane *p, const uint8_t *bytes);
void sgi_plane_encode(const SGIPlane *p, uint32_t word, uint8_t *bytes);

/*
 * Copy len bytes at base out of, or into, board memory. Returns false,
 * copying nothing, when the range is outside the memory or crosses a page
 * the family has not resolved.
 */
bool sgi_board_memory_export(const SGIBoardMemory *m, uint64_t base,
                             uint64_t len, void *dst);
bool sgi_board_memory_import(SGIBoardMemory *m, uint64_t base, uint64_t len,
                             const void *src);

/*
 * Migration. The shared state is a subsection of the family's own
 * description, so adopting it does not change the family's version:
 *
 *     DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_foo_mem, "foo/board-memory",
 *                                     FooState, mem);
 *     ... .subsections = (const VMStateDescription * const []) {
 *             &vmstate_foo_mem, NULL },
 *
 * The name must begin with the family description's name and a slash.
 * It carries the bytes of a device-backed memory, or the guest addresses
 * of a page-backed one. Planes are realize-time configuration and only
 * their count is checked. The bytes are one non-iterative blob, so
 * downtime grows with the board's size. Loading a page-backed memory
 * leaves every page unresolved: the family resolves them again in its
 * own post_load, which runs after this one.
 */
extern const VMStateDescription vmstate_sgi_board_memory;

#define VMSTATE_SGI_BOARD_MEMORY(_field, _state)                         \
    VMSTATE_STRUCT(_field, _state, 0, vmstate_sgi_board_memory,          \
                   SGIBoardMemory)

#define DEFINE_VMSTATE_SGI_BOARD_MEMORY(_vmsd, _name, _state, _field)    \
    static const VMStateDescription _vmsd = {                            \
        .name = (_name),                                                 \
        .version_id = 1,                                                 \
        .minimum_version_id = 1,                                         \
        .fields = (const VMStateField[]) {                               \
            VMSTATE_SGI_BOARD_MEMORY(_field, _state),                    \
            VMSTATE_END_OF_LIST()                                        \
        },                                                               \
    }

#endif
