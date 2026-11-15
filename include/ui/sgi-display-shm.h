/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shared-memory display for the SGI machines.
 *
 * The machine property "display-shm=NAME" publishes every graphic console
 * through a shared-memory segment and tells one reader about new frames over
 * a local stream socket. The contract an external reader implements against
 * is docs/system/mips/sgi/display-shm.rst; this header is its C form and the
 * two must change together.
 *
 * Every integer is little endian. The writer refuses to start on a
 * big-endian host.
 */
#ifndef UI_SGI_DISPLAY_SHM_H
#define UI_SGI_DISPLAY_SHM_H

#include "qom/object.h"

#define SGI_SHM_MAGIC           0x48535347u     /* "GSSH" in memory */
#define SGI_SHM_VERSION         2
#define SGI_SHM_MAX_HEADS       8
#define SGI_SHM_SLOTS           3
#define SGI_SHM_MAX_DIM         2048
#define SGI_SHM_SLOT_BYTES      (SGI_SHM_MAX_DIM * SGI_SHM_MAX_DIM * 4)
#define SGI_SHM_HEADER_BYTES    4096
#define SGI_SHM_NAME_MAX        30

/* One slot's bookkeeping: 24 bytes. */
typedef struct SGIShmSlotInfo {
    uint64_t seq;               /* frame sequence this slot holds */
    uint32_t gen;               /* odd while the writer is inside the slot */
    uint32_t width;             /* valid while gen is even and unchanged */
    uint32_t height;
    uint32_t reserved;          /* zero */
} SGIShmSlotInfo;

/* One head's state: 128 bytes. */
typedef struct SGIShmHead {
    uint32_t active;            /* 1 while the head has a usable surface */
    uint32_t latest;            /* slot holding the newest frame */
    uint32_t reading;           /* slot + 1 the reader holds, 0 for none */
    uint32_t cursor_on;
    int32_t cursor_x;
    int32_t cursor_y;
    uint64_t seq;               /* sequence of the newest frame, 0 for none */
    SGIShmSlotInfo slot[SGI_SHM_SLOTS];
    uint8_t reserved[128 - 32 - SGI_SHM_SLOTS * 24];
} SGIShmHead;

/* The header at offset 0 of the segment: 64 bytes, then the heads. */
typedef struct SGIShmHeader {
    uint32_t magic;             /* written last; zero while initialising */
    uint32_t version;
    uint32_t nheads;
    uint32_t slot_bytes;
    uint64_t data_offset;       /* of head 0 slot 0 */
    uint64_t total_bytes;       /* size of the whole segment */
    uint8_t reserved[64 - 32];
    SGIShmHead head[SGI_SHM_MAX_HEADS];
} SGIShmHeader;

QEMU_BUILD_BUG_ON(sizeof(SGIShmSlotInfo) != 24);
QEMU_BUILD_BUG_ON(sizeof(SGIShmHead) != 128);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHead, slot) != 32);
QEMU_BUILD_BUG_ON(offsetof(SGIShmHeader, head) != 64);
QEMU_BUILD_BUG_ON(sizeof(SGIShmHeader) > SGI_SHM_HEADER_BYTES);

/* A message on the socket: 56 bytes, no padding. */
typedef struct SGIShmMsg {
    uint32_t type;              /* SGI_SHM_MSG_* */
    uint32_t head;
    uint64_t seq;               /* newest frame sequence of the head */
    uint32_t slot;              /* slot holding it (a hint) */
    uint32_t y0, y1;            /* rows [y0, y1) changed since last message */
    uint32_t width, height;
    uint32_t flags;             /* SGI_SHM_FLAG_* */
    int32_t cursor_x, cursor_y;
    uint32_t cursor_on;
    uint32_t reserved;          /* zero */
} SGIShmMsg;

QEMU_BUILD_BUG_ON(sizeof(SGIShmMsg) != 56);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsg, seq) != 8);
QEMU_BUILD_BUG_ON(offsetof(SGIShmMsg, cursor_x) != 40);

#define SGI_SHM_MSG_FRAME       1
#define SGI_SHM_MSG_CURSOR      2

#define SGI_SHM_FLAG_SIZE       (1u << 0)   /* the frame size changed */
#define SGI_SHM_FLAG_FULL       (1u << 1)   /* every row counts as changed */
#define SGI_SHM_FLAG_INACTIVE   (1u << 2)   /* the head lost its surface */

/*
 * Add the "display-shm" and "display-shm-socket" properties to a machine
 * class. Boards call sgi_machine_class_add_display_shm() instead.
 */
void sgi_display_shm_class_add_properties(ObjectClass *oc);

#endif
