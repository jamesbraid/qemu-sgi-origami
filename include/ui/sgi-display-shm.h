/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shared-memory display and binary input for the SGI machines, protocol
 * version 3.
 *
 * The machine property "display-shm=NAME" publishes every SGI graphic head
 * to one external reader. The reader connects to a local stream socket,
 * receives a control block and the heads' pixel regions as passed handles,
 * and maps them. The contract a reader implements against is
 * docs/system/mips/sgi/display-shm.rst; this header is its C form and the
 * two must change together.
 *
 * Every integer is little endian. QEMU refuses to start with the property
 * on a big-endian host.
 */
#ifndef UI_SGI_DISPLAY_SHM_H
#define UI_SGI_DISPLAY_SHM_H

#include "qom/object.h"

#define SGI_SHM_MAGIC           0x33535347u     /* "GSS3" in memory */
#define SGI_SHM_VERSION         3
#define SGI_SHM_MAX_HEADS       8
#define SGI_SHM_NAME_MAX        30

/* Every region size is a multiple of this, and region 0 is this long. */
#define SGI_SHM_REGION_ALIGN    65536
#define SGI_SHM_CONTROL_BYTES   65536
#define SGI_SHM_HEAD_BYTES      4096
#define SGI_SHM_HEAD_OFFSET     4096
#define SGI_SHM_MAX_LINES       4096    /* lines the dirty bitmap covers */
#define SGI_SHM_CURSOR_MAX      64
#define SGI_SHM_PLANES          8
#define SGI_SHM_TABLES          4
#define SGI_SHM_STATE_BYTES     1024
#define SGI_SHM_PLANE_ALIGN     256     /* plane offset multiple */

/* Region backends. */
#define SGI_SHM_BACKEND_VK_FD       1
#define SGI_SHM_BACKEND_VK_WIN32    2
#define SGI_SHM_BACKEND_SHM_FD      3
#define SGI_SHM_BACKEND_SHM_WIN32   4

/* Region kinds. */
#define SGI_SHM_KIND_PLANE      1
#define SGI_SHM_KIND_TABLES     2
#define SGI_SHM_KIND_TILE_POOL  3
#define SGI_SHM_KIND_CURSOR     4

/* Head families. */
#define SGI_SHM_FAMILY_XRGB     1
#define SGI_SHM_FAMILY_ODYSSEY  2
#define SGI_SHM_FAMILY_MGRAS    3
#define SGI_SHM_FAMILY_KONA     4
#define SGI_SHM_FAMILY_GBE      5
#define SGI_SHM_FAMILY_RAD      6

/* Plane formats. */
#define SGI_SHM_FMT_X8R8G8B8    1
#define SGI_SHM_FMT_RGBA8       2
#define SGI_SHM_FMT_U8          3
#define SGI_SHM_FMT_U16         4
#define SGI_SHM_FMT_U32         5
#define SGI_SHM_FMT_TILE_POOL   6

/* Plane flags. */
#define SGI_SHM_PLANE_BIG_ENDIAN    (1u << 0)
#define SGI_SHM_PLANE_BOTTOM_UP     (1u << 1)

/* Head flags. */
#define SGI_SHM_HEAD_DIRTY_EXACT    (1u << 0)
#define SGI_SHM_HEAD_RECOMPOSE      (1u << 1)

/* Plane descriptor: 32 bytes. */
typedef struct SGIShmPlane {
    uint32_t region;            /* 0 for an unused plane */
    uint32_t format;
    uint64_t offset;            /* in the region, a multiple of 256 */
    uint32_t width;
    uint32_t height;
    uint32_t stride;            /* bytes, a multiple of 4 */
    uint32_t flags;
} SGIShmPlane;

/* Table descriptor: 16 bytes. */
typedef struct SGIShmTable {
    uint32_t region;
    uint32_t size;
    uint64_t offset;
} SGIShmTable;

/* Cursor image descriptor: 16 bytes. 64x64 ARGB, rows 256 bytes apart. */
typedef struct SGIShmCursorImage {
    uint32_t region;
    uint32_t reserved;
    uint64_t offset;
} SGIShmCursorImage;

/* One head's record in the control block: 4,096 bytes. */
typedef struct SGIShmHead {
    uint32_t active;
    uint32_t family;
    uint32_t family_version;
    uint32_t layout_gen;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    uint32_t flags;
    uint64_t content_gen;
    uint32_t state_gen;
    uint32_t armed;
    uint32_t cursor_gen;
    uint32_t cursor_on;
    int32_t cursor_x;
    int32_t cursor_y;
    int32_t cursor_hot_x;
    int32_t cursor_hot_y;
    uint32_t cursor_w;
    uint32_t cursor_h;
    SGIShmPlane plane[SGI_SHM_PLANES];
    SGIShmTable table[SGI_SHM_TABLES];
    SGIShmCursorImage cursor_image;
    uint8_t reserved0[96];
    uint8_t state[SGI_SHM_STATE_BYTES];
    uint64_t dirty[SGI_SHM_MAX_LINES / 64];
    uint8_t reserved1[2048];
} SGIShmHead;

/* The control block's header at offset 0: 256 bytes. */
typedef struct SGIShmHeader {
    uint32_t magic;             /* written last */
    uint32_t version;
    uint32_t nheads;
    uint32_t head_bytes;
    uint32_t head_offset;
    uint32_t qemu_pid;
    uint8_t reserved[232];
} SGIShmHeader;

QEMU_BUILD_BUG_ON(sizeof(SGIShmPlane) != 32);
QEMU_BUILD_BUG_ON(sizeof(SGIShmTable) != 16);
QEMU_BUILD_BUG_ON(sizeof(SGIShmCursorImage) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, region) != 0);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, format) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, offset) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, width) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, height) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, stride) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmPlane, flags) != 28);
QEMU_BUILD_BUG_ON(offsetof(SGIShmTable, region) != 0);
QEMU_BUILD_BUG_ON(offsetof(SGIShmTable, size) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmTable, offset) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmCursorImage, region) != 0);
QEMU_BUILD_BUG_ON(offsetof(SGIShmCursorImage, reserved) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmCursorImage, offset) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, magic) != 0);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, version) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, nheads) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, head_bytes) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, head_offset) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, active) != 0);
QEMU_BUILD_BUG_ON(sizeof(SGIShmHeader) != 256);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, qemu_pid) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, reserved) != 24);
QEMU_BUILD_BUG_ON(sizeof(SGIShmHead) != SGI_SHM_HEAD_BYTES);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, family) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, family_version) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, layout_gen) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, width) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, height) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, refresh_mhz) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, flags) != 28);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, content_gen) != 32);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, state_gen) != 40);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, armed) != 44);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_gen) != 48);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_on) != 52);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_x) != 56);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_y) != 60);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_hot_x) != 64);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_hot_y) != 68);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_w) != 72);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_h) != 76);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, plane) != 80);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, table) != 336);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, cursor_image) != 400);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, reserved0) != 416);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, state) != 512);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, dirty) != 1536);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, reserved1) != 2048);
QEMU_BUILD_BUG_ON(SGI_SHM_HEAD_OFFSET + SGI_SHM_MAX_HEADS * SGI_SHM_HEAD_BYTES
                  > SGI_SHM_CONTROL_BYTES);

/* Socket messages. Each starts with this 8-byte header. */
typedef struct SGIShmMsgHeader {
    uint16_t type;
    uint16_t flags;             /* SGI_SHM_MSGF_* */
    uint32_t seq;
} SGIShmMsgHeader;

#define SGI_SHM_MSGF_ACK        (1u << 0)   /* INPUT: send an INPUT_ACK */

/* Reader to QEMU. */
#define SGI_SHM_MSG_HELLO       0x0001
#define SGI_SHM_MSG_INPUT       0x0002
/* QEMU to reader. */
#define SGI_SHM_MSG_WELCOME     0x0081
#define SGI_SHM_MSG_REGION      0x0082
#define SGI_SHM_MSG_REGION_GONE 0x0083
#define SGI_SHM_MSG_WAKE        0x0084
#define SGI_SHM_MSG_INPUT_ACK   0x0085

#define SGI_SHM_CAP_VULKAN      (1u << 0)
#define SGI_SHM_CAP_METAL       (1u << 1)
#define SGI_SHM_CAP_WAKE        (1u << 2)

typedef struct SGIShmMsgHello {         /* 48 bytes */
    SGIShmMsgHeader hdr;
    uint32_t version;
    uint32_t caps;
    uint32_t pid;
    uint32_t reserved0;
    uint8_t device_uuid[16];
    uint64_t reserved1;
} SGIShmMsgHello;

#define SGI_SHM_INPUT_KEY       1
#define SGI_SHM_INPUT_BUTTON    2
#define SGI_SHM_INPUT_RELATIVE  3
#define SGI_SHM_INPUT_ABSOLUTE  4

typedef struct SGIShmMsgInput {         /* 32 bytes */
    SGIShmMsgHeader hdr;
    uint32_t kind;
    uint32_t head;
    uint32_t code;
    uint32_t value;
    int32_t x;
    int32_t y;
} SGIShmMsgInput;

typedef struct SGIShmMsgWelcome {       /* 64 bytes */
    SGIShmMsgHeader hdr;
    uint32_t version;
    uint32_t backend;
    uint32_t nheads;
    uint32_t control_backend;
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
    uint64_t control_handle;
} SGIShmMsgWelcome;

#define SGI_SHM_REGION_DEDICATED    (1u << 0)

typedef struct SGIShmMsgRegion {        /* 56 bytes */
    SGIShmMsgHeader hdr;
    uint32_t id;
    uint32_t kind;
    uint32_t backend;
    uint32_t memory_type;
    uint64_t size;
    uint64_t alloc_size;
    uint32_t flags;
    uint32_t reserved;
    uint64_t handle;
} SGIShmMsgRegion;

typedef struct SGIShmMsgRegionGone {    /* 16 bytes */
    SGIShmMsgHeader hdr;
    uint32_t id;
    uint32_t reserved;
} SGIShmMsgRegionGone;

typedef struct SGIShmMsgWake {          /* 16 bytes */
    SGIShmMsgHeader hdr;
    uint32_t head;
    uint32_t reserved;
} SGIShmMsgWake;

QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgHeader) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHeader, type) != 0);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHeader, flags) != 2);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHeader, seq) != 4);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, version) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, reserved0) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, version) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, reserved) != 44);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegionGone, id) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWake, head) != 8);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgHello) != 48);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, caps) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, pid) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, device_uuid) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgHello, reserved1) != 40);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgInput) != 32);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, kind) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, head) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, code) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, value) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, x) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgInput, y) != 28);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgWelcome) != 64);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, backend) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, nheads) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, control_backend) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, device_uuid) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, driver_uuid) != 40);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgWelcome, control_handle) != 56);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgRegion) != 56);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, id) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, kind) != 12);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, backend) != 16);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, memory_type) != 20);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, size) != 24);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, alloc_size) != 32);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, flags) != 40);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsgRegion, handle) != 48);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgRegionGone) != 16);
QEMU_BUILD_BUG_ON(sizeof(SGIShmMsgWake) != 16);

/*
 * Add the "display-shm", "display-shm-socket" and "display-shm-gpu"
 * properties to a machine class. Boards call
 * sgi_machine_class_add_display_shm() instead.
 */
void sgi_display_shm_class_add_properties(ObjectClass *oc);

/* ---- Device-model API ------------------------------------------------- */

typedef struct SGIScanout SGIScanout;
typedef struct SGIDisplayRegion SGIDisplayRegion;

/*
 * Zeroed storage for a scanout plane or table of size bytes: a region the
 * reader can map when display-shm is on, g_malloc0() when it is off. Either
 * way *region is a handle to give back to sgi_display_region_free(), with
 * nothing else to free. It does not fail: a host that cannot create the
 * region ends QEMU, since the property asked for it.
 */
void *sgi_display_region_alloc(Object *owner, const char *label,
                               uint64_t size, uint32_t kind,
                               SGIDisplayRegion **region);
/* Release the storage; the pointer it returned is dead. NULL is fine. */
void sgi_display_region_free(SGIDisplayRegion *region);

/*
 * Bind a scanout's head record. sgi_scanout_init_head() binds every head as
 * the generic xrgb family; a model that publishes its own family calls this
 * again once its planes exist. A no-op when display-shm is off.
 */
void sgi_display_head_bind(SGIScanout *s, uint32_t family,
                           uint32_t family_version);
/* The head's mode: size and retrace rate in millihertz, 0 when unknown. */
void sgi_display_head_set_mode(SGIScanout *s, uint32_t width,
                               uint32_t height, uint32_t refresh_mhz);
/*
 * The mode and plane 0 as the generic x8r8g8b8 frame r, in one layout
 * update. r NULL means the head has no frame.
 */
void sgi_display_head_set_frame(SGIScanout *s, SGIDisplayRegion *r,
                                uint32_t width, uint32_t height,
                                uint32_t refresh_mhz);
/*
 * A scanout that draws its own cursor reports where the guest put it, so
 * absolute input can find it. Nothing is shown from this: cursor_on stays 0.
 */
void sgi_display_head_guest_cursor(SGIScanout *s, int x, int y);
void sgi_display_head_set_plane(SGIScanout *s, unsigned i,
                                SGIDisplayRegion *r, uint64_t offset,
                                uint32_t format, uint32_t width,
                                uint32_t height, uint32_t stride,
                                uint32_t flags);
void sgi_display_head_set_table(SGIScanout *s, unsigned i,
                                SGIDisplayRegion *r, uint64_t offset,
                                uint32_t size);
/*
 * Family registers: begin makes state_gen odd and returns the 1,024-byte
 * block, end makes it even and bumps content_gen.
 */
void *sgi_display_head_state_begin(SGIScanout *s);
void sgi_display_head_state_end(SGIScanout *s);

/*
 * Called by the scanout, not by models. Display lines first .. first +
 * count - 1 changed in the resolved frame (presented) or in the model's
 * planes (invalidated). Only the matching family acts: xrgb marks lines
 * once the resolve has run, every other family when the model says its
 * planes changed. Both are safe from any thread; published() is main loop
 * only and bumps content_gen after a batch.
 */
void sgi_display_head_presented(SGIScanout *s, int first, int count);
/* Whether the head is a published xrgb frame, which wants exact dirty lines. */
bool sgi_display_head_exact(SGIScanout *s);
void sgi_display_head_invalidated(SGIScanout *s, int first, int count);
void sgi_display_head_published(SGIScanout *s);
/* The scanout's console went away: the head shows nothing. */
void sgi_display_head_unbind(SGIScanout *s);

#endif
