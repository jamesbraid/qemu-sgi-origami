/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Display protocol version 4: the wire. Every integer is little endian and
 * the producer refuses to start on a big-endian host, so these structures
 * are the bytes. Offsets include the 8-byte message header. The contract is
 * docs/interop/display-shm.rst; this file is its C form.
 */
#ifndef UI_DISPLAY_SHM_PROTO_H
#define UI_DISPLAY_SHM_PROTO_H

#include "qemu/compiler.h"

#define DSHM_MAJOR              4
#define DSHM_MINOR              0
#define DSHM_MSG_MAX            131072
#define DSHM_REGION_ALIGN       65536
#define DSHM_STRIDE_ALIGN       256
#define DSHM_STATE_MAGIC        0x50534451u     /* "QDSP" in memory */
#define DSHM_FEEDBACK_MAGIC     0x42464451u     /* "QDFB" in memory */
#define DSHM_FEEDBACK_HEADS     511
#define DSHM_MAX_SLOTS          16
#define DSHM_MAX_LEASES         16
#define DSHM_MAX_CREDIT         4
#define DSHM_MAX_RECTS          64
#define DSHM_MAX_DIM            16384   /* a head's width and height */
#define DSHM_CURSOR_MAX         128
#define DSHM_HANDLE_WINDOW      32

/* Header flags. */
#define DSHM_F_MUST             (1u << 0)
#define DSHM_F_HANDLE           (1u << 1)

/* Viewer to producer. */
#define DSHM_MSG_HELLO          0x0001
#define DSHM_MSG_REQUEST        0x0002
#define DSHM_MSG_IMPORT_FAILED  0x0003
#define DSHM_MSG_HANDLES_SEEN   0x0004
#define DSHM_MSG_SET_STORAGE    0x0005
#define DSHM_MSG_INPUT_CLAIM    0x0010
#define DSHM_MSG_KEY            0x0011
#define DSHM_MSG_BUTTON         0x0012
#define DSHM_MSG_REL            0x0013
#define DSHM_MSG_ABS            0x0014
#define DSHM_MSG_SYNC           0x0015
#define DSHM_MSG_INPUT_RESET    0x0016
/* Producer to viewer. */
#define DSHM_MSG_WELCOME        0x0081
#define DSHM_MSG_REGION         0x0082
#define DSHM_MSG_REGION_GONE    0x0083
#define DSHM_MSG_READY          0x0084
#define DSHM_MSG_HEAD           0x0085
#define DSHM_MSG_FRAME          0x0086
#define DSHM_MSG_CURSOR_IMAGE   0x0087
#define DSHM_MSG_CURSOR_NOTIFY  0x0088
#define DSHM_MSG_INPUT_ACK      0x0089
#define DSHM_MSG_INPUT_STATE    0x008A
#define DSHM_MSG_ORPHANS        0x008B
/* Either way. */
#define DSHM_MSG_ERROR          0x00F0

/* Extension types: the index WELCOME assigned, then the local number. */
#define DSHM_EXT_TYPE(index, local) (((index) << 8) | (local))

#define DSHM_ERR_VERSION        1
#define DSHM_ERR_PROTOCOL       2
#define DSHM_ERR_UNSUPPORTED    3
#define DSHM_ERR_PERMISSION     4
#define DSHM_ERR_LIMIT          5
#define DSHM_ERR_SHUTDOWN       7
#define DSHM_ERR_INTERNAL       8

/* Core features. */
#define DSHM_FEAT_INPUT         (1u << 0)
#define DSHM_FEAT_RW_PIXELS     (1u << 1)
#define DSHM_FEAT_LEDS          (1u << 2)
#define DSHM_FEAT_CURSOR_OVERLAY (1u << 3)
#define DSHM_FEAT_TEST          (1u << 4)
#define DSHM_FEAT_ALL           0x1fu

/* Storage classes. */
#define DSHM_STORAGE_SHM        1

/* Region kinds. */
#define DSHM_KIND_STATE         1
#define DSHM_KIND_FEEDBACK      2
#define DSHM_KIND_SLOT          3

#define DSHM_ACCESS_READ        1u
#define DSHM_ACCESS_WRITE       2u

/* HEAD flags. */
#define DSHM_HEAD_PRESENT       (1u << 0)
#define DSHM_HEAD_TOO_LARGE     (1u << 1)
#define DSHM_HEAD_UNSUPPORTED   (1u << 2)
#define DSHM_HEAD_CURSOR_OUT    (1u << 3)

#define DSHM_POINTER_ABSOLUTE   1
#define DSHM_POINTER_RELATIVE   2
#define DSHM_POINTER_STEERED    3

#define DSHM_FOURCC_XR24        0x34325258u
#define DSHM_FOURCC_XB24        0x34324258u
#define DSHM_FOURCC_AR24        0x34325241u

/* REQUEST kinds and flags. */
#define DSHM_REQ_FRAME          (1u << 0)
#define DSHM_REQ_CURSOR         (1u << 1)
#define DSHM_REQ_SAMPLE_NOW     (1u << 0)

/* FRAME damage kinds and flags. */
#define DSHM_DAMAGE_WHOLE       0
#define DSHM_DAMAGE_RECTS       1
#define DSHM_DAMAGE_LINES       2
#define DSHM_FRAME_CURSOR_EMBEDDED (1u << 0)

/* IMPORT_FAILED reasons. */
#define DSHM_IMPORT_CLASS       1
#define DSHM_IMPORT_ADAPTER     2
#define DSHM_IMPORT_DEVICE_LOST 3
#define DSHM_IMPORT_OOM         4
#define DSHM_IMPORT_TOO_LARGE   5
#define DSHM_IMPORT_NEEDS_WRITE 6
#define DSHM_IMPORT_REPRESENTATION 7

typedef struct DShmHdr {
    uint16_t type;
    uint16_t flags;
    uint32_t length;
} DShmHdr;

typedef struct DShmHello {              /* 120 bytes, tail: extension offers */
    DShmHdr hdr;
    uint16_t major;
    uint16_t minor;
    uint32_t features;
    uint8_t name[32];
    uint32_t storage[8];
    uint32_t credit;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t prev_viewer_id;
    uint32_t max_slot_bytes;
    uint32_t n_ext;
    uint64_t prev_handles_taken;
    uint64_t prev_session_id;
} DShmHello;

typedef struct DShmOffer {              /* 40 bytes, then payload padded to 8 */
    uint8_t name[32];
    uint16_t version_min;
    uint16_t version_max;
    uint32_t payload_len;
} DShmOffer;

typedef struct DShmRequest {            /* 32 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t kinds;
    uint64_t frame_seen;
    uint32_t cursor_seen;
    uint32_t flags;
} DShmRequest;

typedef struct DShmImportFailed {       /* 16 bytes */
    DShmHdr hdr;
    uint32_t region;
    uint32_t reason;
} DShmImportFailed;

typedef struct DShmHandlesSeen {        /* 16 bytes */
    DShmHdr hdr;
    uint64_t count;
} DShmHandlesSeen;

typedef struct DShmSetStorage {         /* 48 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t reserved;
    uint32_t storage[8];
} DShmSetStorage;

typedef struct DShmInputClaim {         /* 16 bytes */
    DShmHdr hdr;
    uint32_t flags;
    uint32_t reserved;
} DShmInputClaim;

typedef struct DShmKey {                /* KEY and BUTTON, 24 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t code;
    uint32_t down;
    uint32_t reserved;
} DShmKey;

typedef struct DShmMotion {             /* REL and ABS, 24 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t reserved;
    int32_t x;
    int32_t y;
} DShmMotion;

typedef struct DShmSync {               /* 16 bytes */
    DShmHdr hdr;
    uint32_t flags;
    uint32_t serial;
} DShmSync;

typedef struct DShmWelcome {            /* 80 bytes, tail: extension accepts */
    DShmHdr hdr;
    uint16_t major;
    uint16_t minor;
    uint32_t features;
    uint64_t session_id;
    uint32_t viewer_id;
    uint32_t state_region;
    uint32_t feedback_region;
    uint32_t credit;
    uint32_t max_viewers;
    uint32_t n_ext;
    uint8_t name[32];
} DShmWelcome;

typedef struct DShmAccept {             /* 40 bytes, then payload padded to 8 */
    uint8_t name[32];
    uint16_t version;
    uint8_t index;
    uint8_t reserved;
    uint32_t payload_len;
} DShmAccept;

typedef struct DShmRegionMsg {          /* 48 bytes with no payload */
    DShmHdr hdr;
    uint32_t id;
    uint32_t kind;
    uint32_t storage;
    uint32_t access;
    uint64_t size;
    uint32_t payload_len;
    uint32_t reserved;
    /* payload, then the u64 handle in the last 8 bytes */
} DShmRegionMsg;

typedef struct DShmRegionGone {         /* 16 bytes */
    DShmHdr hdr;
    uint32_t id;
    uint32_t reserved;
} DShmRegionGone;

typedef struct DShmHeadMsg {            /* 160 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t flags;
    uint32_t mode_serial;
    uint32_t source_index;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    uint32_t fourcc;
    uint32_t stride;
    uint32_t n_slots;
    uint32_t slot_region[DSHM_MAX_SLOTS];
    uint32_t pointer_mode;
    uint32_t credit;
    uint8_t label[32];
    uint64_t reserved;
} DShmHeadMsg;

typedef struct DShmFrame {              /* 72 bytes, then damage */
    DShmHdr hdr;
    uint32_t head;
    uint32_t region;
    uint64_t serial;
    uint32_t mode_serial;
    uint16_t damage_kind;
    uint16_t damage_count;
    uint64_t sample_ns;
    uint64_t publish_ns;
    uint64_t guest_ns;
    uint16_t lease;
    uint16_t frame_flags;
    uint32_t reserved;
    uint64_t lease_seq;
} DShmFrame;

typedef struct DShmCursorImage {        /* 40 bytes, then pixels */
    DShmHdr hdr;
    uint32_t head;
    uint32_t image_serial;
    uint32_t width;
    uint32_t height;
    int32_t hot_x;
    int32_t hot_y;
    uint32_t fourcc;
    uint32_t stride;
} DShmCursorImage;

typedef struct DShmCursorNotify {       /* 16 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t cursor_seq;
} DShmCursorNotify;

typedef struct DShmInputAck {           /* 16 bytes */
    DShmHdr hdr;
    uint32_t serial;
    uint32_t reserved;
} DShmInputAck;

typedef struct DShmInputState {         /* 16 bytes */
    DShmHdr hdr;
    uint32_t flags;
    uint32_t leds;
} DShmInputState;

typedef struct DShmError {              /* 24 bytes, then text */
    DShmHdr hdr;
    uint32_t code;
    uint16_t major_min;
    uint16_t major_max;
    uint32_t text_len;
    uint32_t reserved;
} DShmError;

/* The state region. */
typedef struct DShmStateHeader {        /* 64 bytes */
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint64_t session_id;
    uint32_t head_offset;
    uint32_t head_stride;
    uint32_t head_capacity;
    uint8_t reserved[36];
} DShmStateHeader;

typedef struct DShmStateHead {          /* 64 bytes */
    uint32_t cursor_seq;
    uint32_t cursor_flags;
    int32_t cursor_x;
    int32_t cursor_y;
    uint32_t cursor_image_serial;
    uint32_t frame_seq;
    uint64_t frame_serial;
    uint64_t sample_ns;
    uint64_t publish_ns;
    uint64_t guest_ns;
    uint32_t skipped;
    uint32_t reserved;
} DShmStateHead;

/* The feedback region. */
typedef struct DShmFeedbackHeader {     /* 64 bytes */
    uint32_t magic;
    uint16_t major;
    uint16_t reserved0;
    uint32_t head_offset;
    uint32_t head_stride;
    uint32_t head_capacity;
    uint8_t reserved[44];
} DShmFeedbackHeader;

typedef struct DShmFeedbackHead {       /* 128 bytes */
    uint64_t released[DSHM_MAX_LEASES];
} DShmFeedbackHead;

/* qemu.pointer-steering, version 1. */
#define DSHM_STEER_BLOCK_BYTES  32
#define DSHM_STEER_STEER_DONE   0x01
typedef struct DShmSteerBlock {         /* 32 bytes */
    uint32_t seq;
    uint32_t kind;                      /* 0 none, 1 hot spot, 2 glyph corner */
    int32_t x;
    int32_t y;
    uint32_t moving;
    uint8_t reserved[12];
} DShmSteerBlock;

typedef struct DShmSteerDone {          /* 24 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t reason;
    int32_t x;
    int32_t y;
} DShmSteerDone;

/* qemu.test, version 1. */
#define DSHM_TEST_STIMULUS      0x01
#define DSHM_TEST_INPUT_ECHO    0x02
#define DSHM_TEST_STATS_REQUEST 0x03
#define DSHM_TEST_STATS         0x04
#define DSHM_TEST_CONTROL       0x05
#define DSHM_TEST_CONTROL_DONE  0x06

typedef struct DShmStimulus {           /* 32 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t kind;
    uint64_t seed;
    uint32_t step;
    uint32_t arg;
} DShmStimulus;

typedef struct DShmControl {            /* 24 bytes */
    DShmHdr hdr;
    uint32_t op;
    uint32_t head;
    uint32_t target_viewer;
    uint32_t arg;
} DShmControl;

typedef struct DShmControlDone {        /* 16 bytes */
    DShmHdr hdr;
    uint32_t op;
    uint32_t result;
} DShmControlDone;

typedef struct DShmInputEcho {          /* 40 bytes */
    DShmHdr hdr;
    uint32_t head;
    uint32_t kind;
    uint32_t code;
    int32_t a;
    int32_t b;
    uint32_t seq;
    uint64_t reserved;
} DShmInputEcho;

/* 96 bytes: ten u64 counters, then the u32 sets and max_queued. */
typedef struct DShmStats {
    DShmHdr hdr;
    uint64_t samples;
    uint64_t fills;
    uint64_t lines_produced;
    uint64_t lines_stored;
    uint64_t lines_copied;
    uint64_t bytes_copied;
    uint64_t publishes;
    uint64_t frames_sent;
    uint64_t wakeups;
    uint64_t retained_bytes;
    uint32_t sets;
    uint32_t max_queued;
} DShmStats;

#define DSHM_CHECK(type, field, off) \
    QEMU_BUILD_BUG_ON(offsetof(type, field) != (off))

QEMU_BUILD_BUG_ON(sizeof(DShmHdr) != 8);
QEMU_BUILD_BUG_ON(sizeof(DShmHello) != 120);
DSHM_CHECK(DShmHello, major, 8);
DSHM_CHECK(DShmHello, features, 12);
DSHM_CHECK(DShmHello, name, 16);
DSHM_CHECK(DShmHello, storage, 48);
DSHM_CHECK(DShmHello, credit, 80);
DSHM_CHECK(DShmHello, max_width, 84);
DSHM_CHECK(DShmHello, max_height, 88);
DSHM_CHECK(DShmHello, prev_viewer_id, 92);
DSHM_CHECK(DShmHello, max_slot_bytes, 96);
DSHM_CHECK(DShmHello, n_ext, 100);
DSHM_CHECK(DShmHello, prev_handles_taken, 104);
DSHM_CHECK(DShmHello, prev_session_id, 112);
QEMU_BUILD_BUG_ON(sizeof(DShmOffer) != 40);
QEMU_BUILD_BUG_ON(sizeof(DShmRequest) != 32);
DSHM_CHECK(DShmRequest, head, 8);
DSHM_CHECK(DShmRequest, kinds, 12);
DSHM_CHECK(DShmRequest, frame_seen, 16);
DSHM_CHECK(DShmRequest, cursor_seen, 24);
DSHM_CHECK(DShmRequest, flags, 28);
QEMU_BUILD_BUG_ON(sizeof(DShmImportFailed) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmHandlesSeen) != 16);
DSHM_CHECK(DShmHandlesSeen, count, 8);
QEMU_BUILD_BUG_ON(sizeof(DShmSetStorage) != 48);
DSHM_CHECK(DShmSetStorage, storage, 16);
QEMU_BUILD_BUG_ON(sizeof(DShmInputClaim) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmKey) != 24);
DSHM_CHECK(DShmKey, code, 12);
DSHM_CHECK(DShmKey, down, 16);
QEMU_BUILD_BUG_ON(sizeof(DShmMotion) != 24);
DSHM_CHECK(DShmMotion, x, 16);
DSHM_CHECK(DShmMotion, y, 20);
QEMU_BUILD_BUG_ON(sizeof(DShmSync) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmWelcome) != 80);
DSHM_CHECK(DShmWelcome, session_id, 16);
DSHM_CHECK(DShmWelcome, viewer_id, 24);
DSHM_CHECK(DShmWelcome, state_region, 28);
DSHM_CHECK(DShmWelcome, feedback_region, 32);
DSHM_CHECK(DShmWelcome, credit, 36);
DSHM_CHECK(DShmWelcome, max_viewers, 40);
DSHM_CHECK(DShmWelcome, n_ext, 44);
DSHM_CHECK(DShmWelcome, name, 48);
QEMU_BUILD_BUG_ON(sizeof(DShmAccept) != 40);
QEMU_BUILD_BUG_ON(sizeof(DShmRegionMsg) != 40);
DSHM_CHECK(DShmRegionMsg, size, 24);
DSHM_CHECK(DShmRegionMsg, payload_len, 32);
QEMU_BUILD_BUG_ON(sizeof(DShmRegionGone) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmHeadMsg) != 160);
DSHM_CHECK(DShmHeadMsg, mode_serial, 16);
DSHM_CHECK(DShmHeadMsg, width, 24);
DSHM_CHECK(DShmHeadMsg, refresh_mhz, 32);
DSHM_CHECK(DShmHeadMsg, stride, 40);
DSHM_CHECK(DShmHeadMsg, n_slots, 44);
DSHM_CHECK(DShmHeadMsg, slot_region, 48);
DSHM_CHECK(DShmHeadMsg, pointer_mode, 112);
DSHM_CHECK(DShmHeadMsg, credit, 116);
DSHM_CHECK(DShmHeadMsg, label, 120);
QEMU_BUILD_BUG_ON(sizeof(DShmFrame) != 72);
DSHM_CHECK(DShmFrame, serial, 16);
DSHM_CHECK(DShmFrame, mode_serial, 24);
DSHM_CHECK(DShmFrame, damage_kind, 28);
DSHM_CHECK(DShmFrame, sample_ns, 32);
DSHM_CHECK(DShmFrame, publish_ns, 40);
DSHM_CHECK(DShmFrame, guest_ns, 48);
DSHM_CHECK(DShmFrame, lease, 56);
DSHM_CHECK(DShmFrame, frame_flags, 58);
DSHM_CHECK(DShmFrame, lease_seq, 64);
QEMU_BUILD_BUG_ON(sizeof(DShmCursorImage) != 40);
QEMU_BUILD_BUG_ON(sizeof(DShmCursorNotify) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmInputAck) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmInputState) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmError) != 24);
QEMU_BUILD_BUG_ON(sizeof(DShmStateHeader) != 64);
DSHM_CHECK(DShmStateHeader, head_offset, 16);
QEMU_BUILD_BUG_ON(sizeof(DShmStateHead) != 64);
DSHM_CHECK(DShmStateHead, cursor_image_serial, 16);
DSHM_CHECK(DShmStateHead, frame_seq, 20);
DSHM_CHECK(DShmStateHead, frame_serial, 24);
DSHM_CHECK(DShmStateHead, skipped, 56);
QEMU_BUILD_BUG_ON(sizeof(DShmFeedbackHeader) != 64);
QEMU_BUILD_BUG_ON(sizeof(DShmFeedbackHead) != 128);
QEMU_BUILD_BUG_ON(sizeof(DShmSteerBlock) != 32);
QEMU_BUILD_BUG_ON(sizeof(DShmSteerDone) != 24);
QEMU_BUILD_BUG_ON(sizeof(DShmStimulus) != 32);
QEMU_BUILD_BUG_ON(sizeof(DShmControl) != 24);
QEMU_BUILD_BUG_ON(sizeof(DShmControlDone) != 16);
QEMU_BUILD_BUG_ON(sizeof(DShmInputEcho) != 40);
QEMU_BUILD_BUG_ON(sizeof(DShmStats) != 96);
DSHM_CHECK(DShmStats, retained_bytes, 80);
DSHM_CHECK(DShmStats, sets, 88);
DSHM_CHECK(DShmStats, max_queued, 92);

#endif
