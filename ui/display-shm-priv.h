/* SPDX-License-Identifier: BSD-3-Clause */
/* Internals shared by the display-shm producer's files. */
#ifndef UI_DISPLAY_SHM_PRIV_H
#define UI_DISPLAY_SHM_PRIV_H

#include "qemu/bitmap.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "hw/core/qdev.h"
#include "ui/console.h"
#include "ui/display-shm.h"
#include "ui/display-shm-proto.h"

#define TYPE_DISPLAY_SHM "display-shm"
OBJECT_DECLARE_SIMPLE_TYPE(DisplayShm, DISPLAY_SHM)

#define DSHM_VIEWERS_MAX        8
#define DSHM_OUT_MAX_BYTES      (4u << 20)
#define DSHM_PENDING_MAX        8
#define DSHM_STEER_QUEUE        64
#define DSHM_REPORT_MAX         256
#define DSHM_EXT_STEER          "qemu.pointer-steering"
#define DSHM_EXT_TEST           "qemu.test"

typedef struct DShmViewer DShmViewer;
typedef struct DShmVH DShmVH;
typedef struct DShmSet DShmSet;
typedef struct DShmSlot DShmSlot;
typedef struct DShmOut DShmOut;

/* One shared allocation with one handle. */
typedef struct DShmRegion {
    uint32_t id;
    uint32_t kind;
    uint32_t access;            /* DSHM_ACCESS_* for the viewer's handle */
    size_t size;
    uint8_t *ptr;
    int fd;
    unsigned refs;
} DShmRegion;

struct DShmSlot {
    DShmRegion *r;
    DShmSet *set;
    unsigned long *stale;       /* lines the reference frame has changed */
    unsigned lent;              /* leases naming it */
    bool retired;
};

struct DShmSet {
    QTAILQ_ENTRY(DShmSet) next;
    DShmHead *h;
    bool writable;
    bool current;               /* matches the head's mode */
    uint32_t width, height, stride;
    size_t slot_bytes;
    unsigned viewers;
    unsigned credits;           /* the sum over its viewers */
    DShmSlot *slot[DSHM_MAX_SLOTS];
    unsigned n;
    bool list_changed;          /* a slot left; announce the new list */
};

typedef struct DShmReq {
    bool frame, cursor;
    uint64_t frame_seen;
    uint32_t cursor_seen;
    bool sample_now;
    uint64_t bound;             /* sample number, 0 when unbound */
} DShmReq;

/* One viewer's view of one head. */
struct DShmVH {
    DShmViewer *v;
    DShmHead *h;
    DShmSet *set;
    unsigned credit;
    uint32_t mode_serial;
    struct {
        bool used;
        bool started;           /* the FRAME's first byte has gone out */
        uint64_t seq;
        DShmSlot *slot;
    } lease[DSHM_MAX_LEASES];
    uint64_t next_seq;
    uint64_t last_lent;
    bool reset;                 /* frame_seen and last_lent read as 0 */
    DShmReq req;
    unsigned long *damage;
    bool damage_all;
    DShmOut *queued;            /* the one unsent FRAME */
    bool too_large;
    bool lost_once;             /* device-lost re-creation used */
    bool nothing;               /* the set could not be allocated */
    bool needs_eval;
    uint64_t set_gen;           /* one more for every re-home or mode change */
    bool over_logged[DSHM_MAX_LEASES];
};

/* A queued message. */
struct DShmOut {
    QTAILQ_ENTRY(DShmOut) next;
    uint8_t *buf;
    size_t len, off;
    int fd;
    uint16_t type;
    bool urgent;                /* sent even while writes are held */
    uint32_t region;            /* REGION, FRAME: the region it names */
    DShmVH *vh;                 /* FRAME */
    unsigned lease;
    uint64_t lease_seq;
    uint64_t gen;
    uint64_t saved_last_lent;
    DShmReq saved_req;
    unsigned long *saved_damage;
    bool saved_damage_all;
    DShmSlot *slot;
};

struct DShmViewer {
    DisplayShm *d;
    int fd;
    uint32_t id;
    bool hello, ready, closing;
    bool doomed;                /* a close is scheduled */
    unsigned refs;              /* the viewer's life, and each bottom half */
    uint32_t doom_code;
    char doom_text[48];
    char name[33];
    uint32_t features;
    uint32_t storage[8];
    unsigned credit;            /* granted */
    uint32_t max_width, max_height;
    uint8_t *in;
    size_t in_len;
    QTAILQ_HEAD(, DShmOut) out;
    size_t out_bytes;
    bool want_write;
    bool hold_writes;
    DShmOut *partial;           /* the message being sent, if any */
    unsigned urgent_queued;     /* urgent messages waiting */
    bool in_forget;             /* dshm_region_forget() is running */
    uint64_t handles_started, handles_seen;
    GHashTable *announced;      /* region ids this viewer was told of */
    DShmRegion *feedback;
    DShmFeedbackHeader *fbh;
    DShmVH **vh;                /* by head number */
    unsigned n_vh;
    unsigned wholly_queued_max;
    unsigned test_idx, steer_idx;
    /* input */
    uint8_t keys_down[(0x2ff + 8) / 8];
    uint8_t key_head[0x300];    /* the head each held key went to */
    uint8_t button_head[16];
    uint64_t buttons_down;
    uint32_t wheel_dropped;     /* wheel buttons whose press was dropped */
    uint64_t sync_pending;
    bool ext_test, ext_steer;
};

struct DShmHead {
    DisplayShm *d;
    unsigned idx;
    bool present;
    bool pending_add;           /* added before the machine was ready */
    bool unsupported;
    QemuConsole *con;
    QemuConsole *seen_con;      /* the console of an unsupported head */
    const DShmSourceOps *ops;
    void *opaque;
    char label[32];
    DisplayChangeListener dcl;
    const DisplayChangeListenerOps *dcl_ops;
    bool dcl_on;
    /* mode and reference frame */
    uint32_t width, height, mhz, stride;
    uint32_t *ref;
    size_t ref_stride;
    uint32_t *own;              /* the reference frame the producer owns */
    bool ref_valid;
    bool need_first;
    uint64_t n;                 /* the newest serial */
    uint64_t sample_ns, publish_ns, guest_ns;
    bool unsampled;
    unsigned long *marks;       /* a core-owned source's marked lines */
    unsigned long *changed;     /* lines that differed in this sample */
    bool changed_any;
    uint64_t sample_seq, completed_seq;
    int64_t sample_start_ns;
    bool in_sample;             /* the producer asked for this sample */
    bool sample_open;           /* between sample_start and sample_end */
    int64_t cur_ns;
    bool hold_armed, held;
    bool steerable;
    bool cursor_embedded;
    bool waiting_retrace;
    QEMUBH *bh;
    unsigned skipped;
    /* cursor */
    bool cursor_visible;
    int32_t cursor_x, cursor_y;
    uint32_t cursor_seq, cursor_image_serial;
    uint32_t cursor_w, cursor_h;
    int32_t cursor_hot_x, cursor_hot_y;
    uint32_t *cursor_pixels;
    /* what the guest drew, for steering */
    uint32_t fb_kind;
    int32_t fb_x, fb_y;
    QTAILQ_HEAD(, DShmSet) sets;
    /* the test source */
    void *test;
};

typedef struct DShmSteerQ {
    bool key;
    unsigned code;
    bool down;
    unsigned head;
} DShmSteerQ;

typedef struct DShmStatsCounters {
    uint64_t samples, fills, lines_produced, lines_stored, lines_copied;
    uint64_t bytes_copied, publishes, frames_sent, wakeups;
} DShmStatsCounters;

struct DisplayShm {
    DeviceState parent_obj;
    /* properties */
    int32_t listen_fd_prop;
    char *path;
    uint32_t max_viewers;
    uint32_t head_capacity;
    char *allow_uid;
    char *vulkan, *udmabuf;
    char *steering;
    uint64_t max_head_bytes;
    uint32_t max_sets;
    uint64_t max_retained;
    char *retire;
    char *shm_prefix;
    bool x_test;

    uint64_t session_id;
    int listen_fd;
    char *bound_path;
    bool ready;
    Notifier done;
    Notifier exit;
    DShmHead **pre;             /* sources added before the machine was ready */
    unsigned n_pre;
    GHashTable *seen_cons;      /* consoles that have a head */
    DShmRegion *state;
    uint32_t steer_block_offset;
    uint32_t next_region_id;
    uint32_t next_viewer_id;
    DShmHead **heads;
    unsigned n_heads;
    DShmViewer *viewers[DSHM_VIEWERS_MAX];
    unsigned n_viewers;
    DShmViewer **pending;       /* connections before their HELLO */
    unsigned n_pending;
    uint64_t retained;
    DShmStatsCounters stats;
    bool vm_handler;
    VMChangeStateEntry *vm_entry;
    /* input */
    DShmViewer *owner;
    unsigned report_n;
    unsigned report_head;
    bool acc_on;
    unsigned acc_head;
    int64_t acc_x, acc_y;
    int64_t window_ns;
    int64_t rel_used[2];
    int64_t wheel_used;
    uint32_t echo_seq;
    uint32_t leds;
    /* steering */
    DShmSteerQ steerq[DSHM_STEER_QUEUE];
    unsigned steerq_n;
    int64_t steerq_since;
    QEMUTimer *steerq_timer;
    void *steer;
    void *test;
};

/* display-shm.c */
DShmHead *dshm_head(DisplayShm *d, unsigned idx);
int64_t dshm_now_ns(void);
void dshm_head_send_to(DShmViewer *v, DShmHead *h);
void dshm_viewer_join(DShmViewer *v);
void dshm_viewer_leave(DShmViewer *v);
void dshm_vh_request(DShmVH *vh, const DShmRequest *m);
void dshm_vh_set_storage(DShmVH *vh, const uint32_t *storage);
void dshm_vh_eval(DShmVH *vh);
void dshm_head_changed_geometry(DShmHead *h);
uint32_t dshm_head_pointer_mode(DShmHead *h, DShmViewer *v);
DShmHead *dshm_head_new(DisplayShm *d, QemuConsole *con,
                        const DShmSourceOps *ops, void *opaque,
                        const char *label);
void dshm_head_announce(DShmHead *h);
void dshm_head_do_sample(DShmHead *h);
void dshm_stats_fill(DisplayShm *d, DShmViewer *v, DShmStats *s);
void dshm_head_hold_release(DShmHead *h);
unsigned dshm_vh_retire_queued(DShmVH *vh);
bool dshm_head_demand(DShmHead *h);
void dshm_head_state_cursor_notify(DShmHead *h);
void dshm_import_failed(DShmViewer *v, uint32_t region, uint32_t reason);
void dshm_frame_started(DShmViewer *v, DShmOut *o);
void dshm_frame_cancel(DShmViewer *v, DShmOut *o);
void dshm_viewer_eval_flagged(DShmViewer *v);
void dshm_eval_flagged_all(DisplayShm *d);
void dshm_viewer_refresh_gone(DShmViewer *v);
void dshm_head_mark_bits(DShmHead *h, int first, int count);
void dshm_dcl_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on);
void dshm_dcl_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c);

/* display-shm-storage.c */
DShmRegion *dshm_region_new(DisplayShm *d, uint32_t kind, size_t size,
                            bool writable_by_viewer, Error **errp);
void dshm_region_ref(DShmRegion *r);
void dshm_region_unref(DisplayShm *d, DShmRegion *r);
int dshm_region_dup(const DShmRegion *r);

/* display-shm-transport.c */
void dshm_transport_start(DisplayShm *d, Error **errp);
void dshm_transport_stop(DisplayShm *d);
DShmOut *dshm_queue(DShmViewer *v, const void *msg, size_t len, int fd,
                    bool tail_handle);
DShmOut *dshm_queue_ext(DShmViewer *v, const void *msg, size_t len,
                        bool urgent);
void dshm_viewer_close(DShmViewer *v, uint32_t code, const char *text);
void dshm_viewer_doom(DShmViewer *v, uint32_t code, const char *text);
void dshm_out_remove(DShmViewer *v, DShmOut *o);
void dshm_out_free(DShmOut *o);
void dshm_flush(DShmViewer *v);
bool dshm_region_announce(DShmViewer *v, DShmRegion *r, uint32_t access);
void dshm_region_forget(DShmViewer *v, DShmRegion *r);
DShmViewer *dshm_viewer_by_id(DisplayShm *d, uint32_t id);
unsigned dshm_live_viewers(DisplayShm *d);
void dshm_message(DShmViewer *v, const DShmHdr *hdr, const uint8_t *msg);

/* display-shm-input.c */
void dshm_input_message(DShmViewer *v, const DShmHdr *hdr,
                        const uint8_t *msg);
void dshm_input_release(DShmViewer *v);
void dshm_input_claim_on_join(DShmViewer *v);
void dshm_input_ack_state(DShmViewer *v);
void dshm_inject_rel(DShmHead *h, int dx, int dy);
void dshm_inject_key(DShmHead *h, unsigned code, bool down);
void dshm_inject_button(DShmHead *h, unsigned button, bool down);
void dshm_inject_abs(DShmHead *h, int x, int y);
void dshm_inject_sync(DShmHead *h);
void dshm_inject_steer(DShmHead *h, int dx, int dy);
void dshm_input_apply_deferred(DShmViewer *v, unsigned head, bool key,
                               unsigned code, bool down);
void dshm_echo(DisplayShm *d, uint32_t head, uint32_t kind, uint32_t code,
               int32_t a, int32_t b);
void dshm_input_end_report(DisplayShm *d);

/* display-shm-steer.c */
bool dshm_steer_wanted(DisplayShm *d);
void dshm_steer_abs(DShmViewer *v, DShmHead *h, int x, int y);
bool dshm_steer_defer(DShmViewer *v, unsigned head, bool key, unsigned code,
                      bool down);
void dshm_steer_cancel(DisplayShm *d, bool drop);
void dshm_steer_cursor_moved(DShmHead *h);
void dshm_steer_init(DisplayShm *d);
void dshm_steer_head_changed(DShmHead *h);

/* display-shm-test.c */
void dshm_test_init(DisplayShm *d);
void dshm_test_head_add(DisplayShm *d);
void dshm_test_message(DShmViewer *v, unsigned local, const uint8_t *msg,
                       size_t len);
void dshm_test_viewer_gone(DShmViewer *v);

/* display-shm-surface.c */
bool dshm_surface_wanted(QemuConsole *con);
void dshm_surface_attach(DisplayShm *d, QemuConsole *con);
void dshm_rescan(DisplayShm *d);

#endif
