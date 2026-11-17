/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The conformance rows of docs/interop/display-shm.rst that QEMU can run:
 * the display protocol version 4 producer, read as an external viewer would,
 * on a machine with no guest. The picture comes from qemu.test stimuli, so
 * every frame can be checked against the function that defines it.
 */
#include "qemu/osdep.h"
#include <fcntl.h>
#include "qemu/memfd.h"
#include "libqtest.h"
#include "display-shm-viewer.h"

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

static char *sock_dir;
static char *sock_path;

static QTestState *start_with(const char *extra)
{
    return qtest_initf("-machine none -nodefaults "
                       "-device display-shm,path=%s,x-test=on%s",
                       sock_path, extra);
}

static QTestState *start(void)
{
    return start_with("");
}

static Viewer *open_v(uint32_t credit, uint32_t features)
{
    return v_open(sock_path, (VOpts) { .credit = credit, .test = true,
                                       .features = features });
}

/* A plain request after the last frame seen, and the frame that answers. */
static bool next_frame(Viewer *v, unsigned head, uint64_t seen, VFrame *f,
                       int ms)
{
    v_request(v, head, DSHM_REQ_FRAME, seen, 0, 0);
    return v_frame(v, f, ms);
}

/*
 * A frame that shows the latest stimulus whoever asked before: a sample of
 * its own, so past the quarter period in which one is shared.
 */
static bool fresh_frame(Viewer *v, unsigned head, uint64_t seen, VFrame *f)
{
    g_usleep(10000);
    v_request(v, head, DSHM_REQ_FRAME, seen, 0, DSHM_REQ_SAMPLE_NOW);
    return v_frame(v, f, V_WAIT_MS);
}

static void settle(Viewer *v)
{
    v_drain(v, 50);
}

/* ---- Session ---------------------------------------------------------- */

static void test_session(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(2, 0);
    VHead *h = &v->head[0];
    VRegion *r;

    g_assert_cmpuint(v->session, !=, 0);
    g_assert_cmpuint(v->granted_credit, ==, 2);
    g_assert_cmpuint(v->max_viewers, ==, 4);
    g_assert_cmpuint(le32(v->state), ==, DSHM_STATE_MAGIC);
    g_assert_cmpuint(le16(v->state + 4), ==, 4);
    g_assert_cmpuint(le64(v->state + 8), ==, v->session);
    g_assert_cmpuint(le32(v->state + 16), ==, 64);
    g_assert_cmpuint(le32(v->state + 20), ==, 64);
    g_assert_true(h->present);
    g_assert_cmpuint(h->width, ==, 128);
    g_assert_cmpuint(h->height, ==, 64);
    g_assert_cmpuint(h->fourcc, ==, DSHM_FOURCC_XR24);
    g_assert_cmpuint(h->stride % 256, ==, 0);
    g_assert_cmpuint(h->stride, ==, 512);
    g_assert_cmpuint(h->mode_serial, ==, 1);
    /* one slot per credit and one for the producer to write */
    g_assert_cmpuint(h->n_slots, ==, 3);
    g_assert_cmpuint(h->credit, ==, 2);
    g_assert_cmpuint(h->pointer_mode, ==, DSHM_POINTER_RELATIVE);
    for (unsigned i = 0; i < h->n_slots; i++) {
        r = v_region(v, h->slot[i]);
        g_assert_nonnull(r);
        g_assert_cmpuint(r->size % 65536, ==, 0);
        g_assert_cmpuint(r->size, >=, 512 * 64);
        g_assert_cmpuint(r->access, ==, DSHM_ACCESS_READ);
    }
    v_close(v);
    qtest_quit(qts);
}

#ifdef CONFIG_LINUX
static void test_regions_sealed(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VHead *h = &v->head[0];
    VRegion *regs[4];
    unsigned n = 0;

    regs[n++] = v_region(v, v->state_region);
    regs[n++] = v_region(v, v->feedback_region);
    regs[n++] = v_region(v, h->slot[0]);
    for (unsigned i = 0; i < n; i++) {
        int seals = fcntl(regs[i]->h, F_GET_SEALS);
        const unsigned want = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;

        g_assert_cmpint(seals, >=, 0);
        g_assert_cmpuint(seals & want, ==, want);
        g_assert_cmpint(ftruncate(regs[i]->h, regs[i]->size / 2), ==, -1);
        g_assert_cmpint(ftruncate(regs[i]->h, regs[i]->size * 2), ==, -1);
    }
    /* what the viewer may not write, it cannot map writable */
    for (unsigned i = 0; i < 3; i++) {
        int seals = fcntl(regs[i]->h, F_GET_SEALS);
        void *p = mmap(NULL, regs[i]->size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, regs[i]->h, 0);

        if (i == 1) {
            /* the feedback region is the viewer's */
            g_assert_true(p != MAP_FAILED);
            g_assert_cmpuint(seals & F_SEAL_FUTURE_WRITE, ==, 0);
            munmap(p, regs[i]->size);
        } else {
            g_assert_true(p == MAP_FAILED);
            g_assert_cmpuint(seals & F_SEAL_FUTURE_WRITE, ==,
                             F_SEAL_FUTURE_WRITE);
        }
    }
    v_close(v);
    qtest_quit(qts);
}

/* RW_PIXELS gets a writable set; its seal does not allow FUTURE_WRITE. */
static void test_rw_pixels_set(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, DSHM_FEAT_RW_PIXELS);
    VRegion *r = v_region(v, v->head[0].slot[0]);
    void *p;
    int seals;

    g_assert_cmpuint(r->access, ==, DSHM_ACCESS_READ | DSHM_ACCESS_WRITE);
    seals = fcntl(r->h, F_GET_SEALS);
    g_assert_cmpuint(seals & F_SEAL_FUTURE_WRITE, ==, 0);
    g_assert_cmpuint(seals & F_SEAL_SEAL, ==, F_SEAL_SEAL);
    p = mmap(NULL, r->size, PROT_READ | PROT_WRITE, MAP_SHARED, r->h, 0);
    g_assert_true(p != MAP_FAILED);
    munmap(p, r->size);
    v_close(v);
    qtest_quit(qts);
}
#endif

/* ---- Frames ----------------------------------------------------------- */

/* A head nobody has drawn on is served black, whole, after one request. */
static void test_still_head_on_connect(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f;
    VStateFrame sf;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    g_assert_cmpuint(f.serial, >=, 1);
    g_assert_cmpuint(f.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    g_assert_cmpuint(f.lease_seq, ==, 1);
    g_assert_cmpuint(f.mode_serial, ==, v->head[0].mode_serial);
    v_check_picture(v, &f, 1, 0, 0);
    sf = v_state_frame(v, 0);
    g_assert_cmpuint(sf.serial, >=, f.serial);
    g_assert_cmpuint(f.sample_ns, <=, f.publish_ns);
    g_assert_cmpuint(f.publish_ns, <=, sf.publish_ns + 1000000000ull);
    v_release(v, &f);
    v_close(v);
    qtest_quit(qts);
}

static void test_steps(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f;
    uint64_t seen = 0;

    for (uint32_t step = 0; step < 6; step++) {
        v_pattern(v, 0, 3, 77, step);
        g_assert_true(next_frame(v, 0, seen, &f, V_WAIT_MS));
        g_assert_cmpuint(f.serial, >, seen);
        g_assert_cmpuint(v_frame_step(v, &f), ==, step);
        v_check_picture(v, &f, 3, 77, step);
        seen = f.serial;
        v_release(v, &f);
    }
    v_close(v);
    qtest_quit(qts);
}

/* Pixels outside a frame's damage equal the viewer's previous frame. */
static void test_damage_coverage(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VHead *h = &v->head[0];
    VFrame f;
    uint32_t *prev = g_new0(uint32_t, 128 * 64);
    uint64_t seen = 0;
    bool partial = false;

    for (uint32_t step = 0; step < 16; step++) {
        v_pattern(v, 0, 3, 5, step);
        g_assert_true(next_frame(v, 0, seen, &f, V_WAIT_MS));
        for (unsigned y = 0; y < h->height; y++) {
            for (unsigned x = 0; x < h->width; x++) {
                uint32_t p = v_px(v, &f, x, y);

                if (!v_damaged(&f, x, y)) {
                    g_assert_cmphex(p, ==, prev[y * 128 + x]);
                    partial = true;
                }
                prev[y * 128 + x] = p;
            }
        }
        seen = f.serial;
        v_release(v, &f);
    }
    g_assert_true(partial);
    g_free(prev);
    v_close(v);
    qtest_quit(qts);
}

/* A viewer at its credit gets nothing until it releases and asks again. */
static void test_last_frame(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f, g;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    /* hold N; N+1 publishes; the request finds no credit */
    v_pattern(v, 0, 2, 1, 1);
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    /* release, ask again, change nothing: N+1 arrives */
    v_release(v, &f);
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(g.serial, ==, f.serial + 1);
    g_assert_cmpuint(g.lease_seq, ==, f.lease_seq + 1);
    v_check_picture(v, &g, 2, 1, 1);
    v_close(v);
    qtest_quit(qts);
}

/* A change with no request recorded still reaches one plain request. */
static void test_pending_changes(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f, g;
    VStats s0, s1;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_release(v, &f);
    s0 = v_stats(v);
    v_pattern(v, 0, 1, 9, 5);
    settle(v);
    s1 = v_stats(v);
    /* nobody was waiting, so nothing was sampled */
    g_assert_cmpuint(s1.samples, ==, s0.samples);
    g_assert_true(next_frame(v, 0, f.serial, &g, V_WAIT_MS));
    g_assert_cmpuint(v_frame_step(v, &g), ==, 5);
    v_check_picture(v, &g, 1, 9, 5);
    v_close(v);
    qtest_quit(qts);
}

static void test_no_unchanged_publish(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(2, 0);
    VFrame f, g;
    VStats s0, s1;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_release(v, &f);
    s0 = v_stats(v);
    /* past the quarter period in which a sample is shared */
    g_usleep(50000);
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, DSHM_REQ_SAMPLE_NOW);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    s1 = v_stats(v);
    g_assert_cmpuint(s1.samples, >, s0.samples);
    g_assert_cmpuint(s1.publishes, ==, s0.publishes);
    v_close(v);
    qtest_quit(qts);
}

static void test_credit_ceiling(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(2, 0);
    VFrame f[3];
    uint64_t seen = 0;
    VStats s;

    for (unsigned i = 0; i < 2; i++) {
        v_pattern(v, 0, 1, 3, i);
        g_assert_true(next_frame(v, 0, seen, &f[i], V_WAIT_MS));
        seen = f[i].serial;
    }
    g_assert_cmpuint(f[0].lease, !=, f[1].lease);
    g_assert_cmpuint(f[0].region, !=, f[1].region);
    /* credit is spent: more changes and requests deliver nothing */
    for (unsigned i = 2; i < 6; i++) {
        v_pattern(v, 0, 1, 3, i);
        v_request(v, 0, DSHM_REQ_FRAME, seen, 0, DSHM_REQ_SAMPLE_NOW);
        g_assert_false(v_frame(v, &f[2], 100));
    }
    s = v_stats(v);
    g_assert_cmpuint(s.max_queued, <=, 1);
    /* release out of order */
    v_release(v, &f[1]);
    v_request(v, 0, DSHM_REQ_FRAME, seen, 0, 0);
    g_assert_true(v_frame(v, &f[2], V_WAIT_MS));
    g_assert_cmpuint(f[2].lease, ==, f[1].lease);
    g_assert_cmpuint(f[2].lease_seq, >, f[1].lease_seq);
    v_close(v);
    qtest_quit(qts);
}

static uint32_t slot_hash(Viewer *v, const VFrame *f)
{
    const VHead *h = &v->head[f->head];
    uint32_t hash = 2166136261u;

    for (unsigned y = 0; y < h->height; y++) {
        for (unsigned x = 0; x < h->width; x++) {
            hash = (hash ^ v_px(v, f, x, y)) * 16777619u;
        }
    }
    return hash;
}

/* No slot changes between its FRAME and its release. */
static void test_no_write_while_lent(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(2, 0), *b = open_v(1, 0);
    VFrame fa[2], fb;
    uint32_t hash[2];
    uint64_t seen_a = 0, seen_b = 0;

    for (unsigned i = 0; i < 2; i++) {
        v_pattern(a, 0, 3, 11, i);
        g_assert_true(next_frame(a, 0, seen_a, &fa[i], V_WAIT_MS));
        hash[i] = slot_hash(a, &fa[i]);
        seen_a = fa[i].serial;
    }
    for (uint32_t step = 2; step < 102; step++) {
        v_pattern(b, 0, 3, 11, step);
        g_assert_true(next_frame(b, 0, seen_b, &fb, V_WAIT_MS));
        seen_b = fb.serial;
        v_release(b, &fb);
    }
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpuint(slot_hash(a, &fa[i]), ==, hash[i]);
        v_release(a, &fa[i]);
    }
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* A holds N and B causes N+1: A's SAMPLE_NOW finds nothing new, is served. */
static void test_unchanged_sample_newer_frame(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(2, 0), *b = open_v(1, 0);
    VFrame fa, fb, fa2;

    g_assert_true(next_frame(a, 0, 0, &fa, V_WAIT_MS));
    g_assert_true(next_frame(b, 0, 0, &fb, V_WAIT_MS));
    v_release(b, &fb);
    v_pattern(b, 0, 1, 4, 1);
    g_assert_true(next_frame(b, 0, fb.serial, &fb, V_WAIT_MS));
    g_assert_cmpuint(fb.serial, >, fa.serial);
    v_request(a, 0, DSHM_REQ_FRAME, fa.serial, 0, DSHM_REQ_SAMPLE_NOW);
    g_assert_true(v_frame(a, &fa2, V_WAIT_MS));
    g_assert_cmpuint(fa2.serial, ==, fb.serial);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* A SAMPLE_NOW waits for its own sample; a held one delivers nothing. */
static void test_sample_now_bound(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(2, 0), *b = open_v(1, 0);
    VFrame fa, fb, g;

    g_assert_true(next_frame(a, 0, 0, &fa, V_WAIT_MS));
    v_release(a, &fa);
    g_assert_true(next_frame(b, 0, 0, &fb, V_WAIT_MS));
    v_release(b, &fb);
    v_pattern(b, 0, 1, 4, 1);
    g_assert_true(next_frame(b, 0, fb.serial, &fb, V_WAIT_MS));
    /* more than a quarter period of refresh at 60 Hz */
    g_usleep(50000);
    g_assert_cmpuint(v_control(a, 1, 0, 0, 0), ==, 0);
    v_pattern(a, 0, 1, 4, 2);
    v_request(a, 0, DSHM_REQ_FRAME, fa.serial, 0, DSHM_REQ_SAMPLE_NOW);
    /* B's frame is newer than A's, but A asked for a sample of its own */
    g_assert_false(v_frame(a, &g, V_QUIET_MS));
    /* a release retry in the hold keeps the binding */
    v_request(a, 0, DSHM_REQ_FRAME, fa.serial, 0, 0);
    g_assert_false(v_frame(a, &g, V_QUIET_MS));
    g_assert_cmpuint(v_control(a, 2, 0, 0, 0), ==, 0);
    g_assert_true(v_frame(a, &g, V_WAIT_MS));
    g_assert_cmpuint(v_frame_step(a, &g), ==, 2);
    v_check_picture(a, &g, 1, 4, 2);
    /* releasing a sample nobody held has nothing to act on */
    g_assert_cmpuint(v_control(a, 2, 0, 0, 0), ==, 1);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* Retries crossing deliveries never repeat a serial or a lease_seq. */
static void test_retries_crossing(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(2, 0);
    VFrame f;
    uint64_t seen = 0, last_seq = 0, last_serial = 0;

    for (uint32_t step = 0; step < 12; step++) {
        v_pattern(v, 0, 1, 2, step);
        v_request(v, 0, DSHM_REQ_FRAME, seen, 0, 0);
        v_request(v, 0, DSHM_REQ_FRAME, seen, 0, 0);
        g_assert_true(v_frame(v, &f, V_WAIT_MS));
        g_assert_cmpuint(f.serial, >, last_serial);
        g_assert_cmpuint(f.lease_seq, >, last_seq);
        last_serial = f.serial;
        last_seq = f.lease_seq;
        seen = f.serial;
        v_release(v, &f);
        /* the release and its retry cross whatever is in flight */
        v_request(v, 0, DSHM_REQ_FRAME, seen, 0, 0);
        v_request(v, 0, DSHM_REQ_FRAME, seen, 0, DSHM_REQ_SAMPLE_NOW);
        v_discard(v, DSHM_MSG_FRAME);
    }
    v_close(v);
    qtest_quit(qts);
}

/* ---- Heads and modes -------------------------------------------------- */

static void test_mode_change(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f, g;
    VMsg m = { };
    uint32_t serial;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_release(v, &f);
    serial = v->head[0].mode_serial;
    /* the new mode's first sample is held: no frame, and never an old one */
    g_assert_cmpuint(v_control(v, 1, 0, 0, 0), ==, 0);
    v_stimulus(v, 0, 4, 96 | (48u << 16), 0, 0);
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    g_assert_cmpuint(v_control(v, 2, 0, 0, 0), ==, 0);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(v->head[0].width, ==, 96);
    g_assert_cmpuint(v->head[0].height, ==, 48);
    g_assert_cmpuint(v->head[0].mode_serial, ==, serial + 1);
    g_assert_cmpuint(g.mode_serial, ==, serial + 1);
    g_assert_cmpuint(g.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    g_assert_cmpuint(g.serial, >, f.serial);
    v_check_picture(v, &g, 1, 0, 0);
    v_close(v);
    qtest_quit(qts);
}

static void test_heads(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VMsg m = { };
    VFrame f;

    g_assert_false(v->head[1].seen);
    v_stimulus(v, 0, 5, 0, 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, 1);
    v_msg_free(&m);
    g_assert_true(v->head[1].present);
    g_assert_true(next_frame(v, 1, 0, &f, V_WAIT_MS));
    g_assert_cmpuint(f.head, ==, 1);
    v_release(v, &f);
    /* a removed head's numbers are not reused */
    v_stimulus(v, 1, 5, 0, 0, 0);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_false(v->head[1].present);
    v_request(v, 1, DSHM_REQ_FRAME, 0, 0, 0);
    g_assert_false(v_frame(v, &f, V_QUIET_MS));
    v_stimulus(v, 0, 5, 0, 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, 2);
    v_msg_free(&m);
    v_close(v);
    qtest_quit(qts);
}

/* ---- Slot sets -------------------------------------------------------- */

static void test_slot_list_changes(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(2, 0), *b;
    VFrame f0, f1, g;
    uint32_t hash0, hash1, before;
    VMsg m = { };

    v_pattern(a, 0, 3, 8, 0);
    g_assert_true(next_frame(a, 0, 0, &f0, V_WAIT_MS));
    v_pattern(a, 0, 3, 8, 1);
    g_assert_true(next_frame(a, 0, f0.serial, &f1, V_WAIT_MS));
    hash0 = slot_hash(a, &f0);
    hash1 = slot_hash(a, &f1);
    before = a->head[0].mode_serial;
    g_assert_cmpuint(a->head[0].n_slots, ==, 3);

    /* a second viewer joins: the first gets a longer slot list, nothing else */
    b = open_v(1, 0);
    g_assert_true(v_take(a, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(a->head[0].n_slots, ==, 4);
    g_assert_cmpuint(a->head[0].mode_serial, ==, before);
    g_assert_cmpuint(slot_hash(a, &f0), ==, hash0);
    g_assert_cmpuint(slot_hash(a, &f1), ==, hash1);
    /* releasing out of order works */
    v_release(a, &f1);
    v_release(a, &f0);
    /* and the HEAD did not hand A its current frame again */
    g_assert_false(v_frame(a, &g, V_QUIET_MS));
    v_request(a, 0, DSHM_REQ_FRAME, f1.serial, 0, 0);
    g_assert_false(v_frame(a, &g, V_QUIET_MS));

    v_close(b);
    g_assert_true(v_take(a, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(a->head[0].n_slots, ==, 3);
    g_assert_cmpuint(a->head[0].mode_serial, ==, before);
    v_close(a);
    qtest_quit(qts);
}

/* Two sets of one representation, alternating requests, a revert included. */
static void test_two_sets(void)
{
    QTestState *qts = start();
    Viewer *ro = open_v(1, 0), *rw = open_v(1, DSHM_FEAT_RW_PIXELS);
    Viewer *vs[2] = { ro, rw };
    uint32_t *prev[2] = { g_new0(uint32_t, 128 * 64),
                          g_new0(uint32_t, 128 * 64) };
    uint64_t seen[2] = { 0, 0 };
    VFrame f;

    g_assert_cmpuint(ro->head[0].slot[0], !=, rw->head[0].slot[0]);
    for (uint32_t step = 0; step < 30; step++) {
        unsigned w = (step % 3) ? step & 1 : 0;
        Viewer *v = vs[w];
        uint32_t st = step % 11 > 6 ? step - 7 : step;

        v_pattern(v, 0, 3, 21, st);
        g_assert_true(fresh_frame(v, 0, seen[w], &f));
        g_assert_cmpuint(v_frame_step(v, &f), ==, st);
        v_check_picture(v, &f, 3, 21, st);
        for (unsigned y = 0; y < 64; y++) {
            for (unsigned x = 0; x < 128; x++) {
                uint32_t p = v_px(v, &f, x, y);

                if (!v_damaged(&f, x, y)) {
                    g_assert_cmphex(p, ==, prev[w][y * 128 + x]);
                }
                prev[w][y * 128 + x] = p;
            }
        }
        seen[w] = f.serial;
        v_release(v, &f);
    }
    g_free(prev[0]);
    g_free(prev[1]);
    v_close(ro);
    v_close(rw);
    qtest_quit(qts);
}

static void test_rehome_keeps_lease(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f, g;
    VMsg m = { };
    uint8_t b[8] = { };
    uint32_t old_region;
    uint32_t hash;

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_release(v, &f);
    old_region = v->head[0].slot[0];
    stl_le_p(b, old_region);
    stl_le_p(b + 4, DSHM_IMPORT_DEVICE_LOST);
    v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(v->head[0].slot[0], !=, old_region);
    /* the screen is still: the same picture comes again, as a new lease */
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(g.serial, ==, f.serial);
    g_assert_cmpuint(g.lease_seq, >, f.lease_seq);
    g_assert_cmpuint(g.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    hash = slot_hash(v, &g);
    /* held: a change and a request deliver nothing, and the slot is steady */
    v_pattern(v, 0, 1, 6, 3);
    v_request(v, 0, DSHM_REQ_FRAME, g.serial, 0, 0);
    g_assert_false(v_frame(v, &f, V_QUIET_MS));
    g_assert_cmpuint(slot_hash(v, &g), ==, hash);
    v_close(v);
    qtest_quit(qts);
}

/* A request left recorded across a re-home is served without a new one. */
static void test_rehome_serves_recorded_request(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f, g;
    VMsg m = { };
    uint8_t b[8] = { };

    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_release(v, &f);
    v_request(v, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    stl_le_p(b, v->head[0].slot[0]);
    stl_le_p(b + 4, DSHM_IMPORT_DEVICE_LOST);
    v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(g.serial, ==, f.serial);
    g_assert_cmpuint(g.damage_kind, ==, DSHM_DAMAGE_WHOLE);
    v_close(v);
    qtest_quit(qts);
}

/* Too large marks the head the region belongs to, and no other. */
static void test_import_too_large(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VMsg m = { };
    uint8_t b[8] = { };

    v_stimulus(v, 0, 5, 0, 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_true(v->head[1].present);
    stl_le_p(b, v->head[1].slot[0]);
    stl_le_p(b + 4, DSHM_IMPORT_TOO_LARGE);
    v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(v->head[1].flags & DSHM_HEAD_TOO_LARGE, ==,
                     DSHM_HEAD_TOO_LARGE);
    g_assert_cmpuint(v->head[1].n_slots, ==, 0);
    g_assert_cmpuint(v->head[0].flags & DSHM_HEAD_TOO_LARGE, ==, 0);
    g_assert_cmpuint(v->head[0].n_slots, >, 0);
    v_close(v);
    qtest_quit(qts);
}

/* IMPORT_FAILED with a class the producer has no other of ends the session. */
static void test_import_failed_class(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    uint8_t b[8] = { };
    VMsg m = { };

    stl_le_p(b, v->head[0].slot[0]);
    stl_le_p(b + 4, DSHM_IMPORT_CLASS);
    v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    g_assert_true(v_take(v, DSHM_MSG_ERROR, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, DSHM_ERR_UNSUPPORTED);
    v_msg_free(&m);
    v_close(v);
    qtest_quit(qts);
}

static void test_disconnect_retires(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(1, 0), *b = open_v(1, 0);
    VFrame f, g;
    uint8_t *held;
    uint32_t hash;
    uint64_t seen = 0;
    VMsg m = { };
    VStats s;

    v_pattern(a, 0, 3, 31, 0);
    g_assert_true(next_frame(a, 0, 0, &f, V_WAIT_MS));
    held = v_map(a, f.region, false);
    hash = slot_hash(a, &f);
    /* the viewer goes without releasing; its mapping lives on */
    v_fd_close(a->fd);
    a->fd = -1;
    g_assert_true(v_take(b, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    for (uint32_t step = 1; step < 40; step++) {
        v_pattern(b, 0, 3, 31, step);
        g_assert_true(next_frame(b, 0, seen, &g, V_WAIT_MS));
        g_assert_cmpuint(g.region, !=, f.region);
        seen = g.serial;
        v_release(b, &g);
    }
    g_assert_cmpuint(slot_hash(a, &f), ==, hash);
    g_assert_nonnull(held);
    s = v_stats(b);
    /* the replacement fills from the reference: no full resolve each time */
    g_assert_cmpuint(s.lines_produced, <, 64 * 60);
    a->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

static void test_cancellation(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(1, 0), *c = open_v(1, 0);
    VFrame f, g;
    VMsg m = { };
    uint32_t region;

    v_pattern(a, 0, 3, 13, 0);
    g_assert_true(next_frame(a, 0, 0, &f, V_WAIT_MS));
    v_release(a, &f);
    settle(a);
    /* hold A's writes so its next FRAME stays wholly queued */
    g_assert_cmpuint(v_control(a, 5, 0, 0, 0), ==, 0);
    v_request(a, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    settle(c);
    /* the still screen: the retire needs a queued frame, so make one */
    v_pattern(c, 0, 3, 13, 1);
    v_request(a, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    usleep(100000);
    g_assert_cmpuint(v_control(c, 3, 0, a->id, 0), ==, 0);
    g_assert_cmpuint(v_control(a, 6, 0, 0, 0), ==, 0);
    /* a fresh frame of the same serial, damage covering the cancelled one */
    g_assert_true(v_take(a, DSHM_MSG_FRAME, &m, V_WAIT_MS));
    v_frame_parse(&m, &g);
    region = g.region;
    v_msg_free(&m);
    g_assert_cmpuint(region, !=, 0);
    g_assert_cmpuint(g.serial, >=, f.serial + 1);
    g_assert_cmpuint(g.lease_seq, >, f.lease_seq);
    v_check_picture(a, &g, 3, 13, 1);
    v_close(a);
    v_close(c);
    qtest_quit(qts);
}

/* ---- Cursor ------------------------------------------------------------ */

static void test_cursor(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VStateCursor c0, c1;
    VMsg m = { };

    c0 = v_state_cursor(v, 0);
    g_assert_cmpuint(c0.seq & 1, ==, 0);
    v_stimulus(v, 0, 3, 0x80ff4020, 0, 4);
    g_assert_true(v_take(v, DSHM_MSG_CURSOR_IMAGE, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 16), ==, 4);
    g_assert_cmpuint(le32(m.d + 20), ==, 4);
    g_assert_cmphex(le32(m.d + 32), ==, DSHM_FOURCC_AR24);
    g_assert_cmphex(le32(m.d + 40), ==, 0x80ff4020);
    g_assert_cmpuint(le32(m.d + 12), ==, v_state_cursor(v, 0).image_serial);
    v_msg_free(&m);
    v_stimulus(v, 0, 2, 20 | (30u << 16), 0, 1);
    settle(v);
    c1 = v_state_cursor(v, 0);
    g_assert_cmpint(c1.x, ==, 20);
    g_assert_cmpint(c1.y, ==, 30);
    g_assert_cmpuint(c1.flags & 1, ==, 1);
    g_assert_cmpuint(c1.seq & 1, ==, 0);
    g_assert_cmpuint(c1.seq, >, c0.seq);
    /* a request for the cursor: told at once when the seq differs, once */
    v_request(v, 0, DSHM_REQ_CURSOR, 0, c0.seq, 0);
    g_assert_true(v_take(v, DSHM_MSG_CURSOR_NOTIFY, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 12), ==, c1.seq);
    v_msg_free(&m);
    g_assert_false(v_take(v, DSHM_MSG_CURSOR_NOTIFY, &m, V_QUIET_MS));
    /* one-shot: the next change notifies once more */
    v_request(v, 0, DSHM_REQ_CURSOR, 0, c1.seq, 0);
    g_assert_false(v_take(v, DSHM_MSG_CURSOR_NOTIFY, &m, 100));
    v_stimulus(v, 0, 2, 22 | (31u << 16), 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_CURSOR_NOTIFY, &m, V_WAIT_MS));
    v_msg_free(&m);
    v_close(v);
    qtest_quit(qts);
}

/* ---- Framing ----------------------------------------------------------- */

static void expect_error(Viewer *v, uint32_t code)
{
    VMsg m = { };

    g_assert_true(v_take(v, DSHM_MSG_ERROR, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, code);
    v_msg_free(&m);
}

static Viewer *raw(void)
{
    Viewer *v = g_new0(Viewer, 1);

    v->in = g_malloc(DSHM_MSG_MAX);
    v->fd = v_connect_fd(sock_path);
    for (unsigned i = 0; i < V_REGIONS; i++) {
        v->reg[i].h = V_NO_HANDLE;
    }
    return v;
}

static void test_framing(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f;
    uint8_t req[32] = { };
    VMsg m = { };

    /* a request in pieces */
    stw_le_p(req, DSHM_MSG_REQUEST);
    stw_le_p(req + 2, DSHM_F_MUST);
    stl_le_p(req + 4, 32);
    stl_le_p(req + 8, 0);
    stl_le_p(req + 12, 1);
    stl_le_p(req + 36 - 8, 0);
    for (unsigned i = 0; i < 32; i += 5) {
        v_send(v, req + i, MIN(5u, 32 - i));
        usleep(2000);
    }
    g_assert_true(v_frame(v, &f, V_WAIT_MS));
    v_release(v, &f);
    /* unknown optional types and types of extensions not offered: skipped */
    v_send_msg(v, 0x0066, 0, "12345678", 8);
    v_send_msg(v, 0x7e01, 0, "12345678", 8);
    /* reserved fields are ignored; a longer fixed message is read to size */
    {
        uint8_t longer[40] = { };

        stl_le_p(longer, 0);
        stl_le_p(longer + 4, 1);
        stq_le_p(longer + 8, f.serial);
        stl_le_p(longer + 24, 0xdeadbeef);
        stl_le_p(longer + 28, 0xdeadbee0);
        v_send_msg(v, DSHM_MSG_REQUEST, DSHM_F_MUST, longer, sizeof(longer));
    }
    v_pattern(v, 0, 1, 1, 1);
    g_assert_true(v_frame(v, &f, V_WAIT_MS));
    g_assert_false(v->closed);
    /* an unknown type with MUST */
    v_send_msg(v, 0x0066, DSHM_F_MUST, "12345678", 8);
    expect_error(v, DSHM_ERR_UNSUPPORTED);
    g_assert_false(v_take(v, DSHM_MSG_FRAME, &m, 200));
    v_close(v);

    /* a message shorter than its fixed part */
    v = open_v(1, 0);
    v_send_msg(v, DSHM_MSG_REQUEST, DSHM_F_MUST, "1234", 4);
    expect_error(v, DSHM_ERR_PROTOCOL);
    v_close(v);

    /* a HANDLE message from a viewer */
    v = open_v(1, 0);
    v_send_msg(v, DSHM_MSG_REQUEST, DSHM_F_MUST | DSHM_F_HANDLE, req + 8, 24);
    expect_error(v, DSHM_ERR_PROTOCOL);
    v_close(v);

    /* HANDLES_SEEN backwards and past what was sent */
    v = open_v(1, 0);
    {
        uint8_t b[8];

        stq_le_p(b, 1000);
        v_send_msg(v, DSHM_MSG_HANDLES_SEEN, 0, b, 8);
    }
    expect_error(v, DSHM_ERR_PROTOCOL);
    v_close(v);

    /* major 5 gets the range */
    v = raw();
    {
        uint8_t hello[120] = { };

        stw_le_p(hello, DSHM_MSG_HELLO);
        stw_le_p(hello + 2, DSHM_F_MUST);
        stl_le_p(hello + 4, 120);
        stw_le_p(hello + 8, 5);
        stl_le_p(hello + 48, 1);
        stl_le_p(hello + 80, 1);
        v_send(v, hello, 120);
    }
    g_assert_true(v_take(v, DSHM_MSG_ERROR, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, DSHM_ERR_VERSION);
    g_assert_cmpuint(le16(m.d + 12), ==, 4);
    g_assert_cmpuint(le16(m.d + 14), ==, 4);
    v_msg_free(&m);
    v_close(v);

    /* a tail count that overruns the message */
    v = raw();
    {
        uint8_t hello[120] = { };

        stw_le_p(hello, DSHM_MSG_HELLO);
        stw_le_p(hello + 2, DSHM_F_MUST);
        stl_le_p(hello + 4, 120);
        stw_le_p(hello + 8, 4);
        stl_le_p(hello + 48, 1);
        stl_le_p(hello + 80, 1);
        stl_le_p(hello + 100, 3);
        v_send(v, hello, 120);
    }
    expect_error(v, DSHM_ERR_PROTOCOL);
    v_close(v);

    /* a version 3 HELLO (its length field is 0) is closed */
    v = raw();
    {
        uint8_t hello[48] = { };

        stw_le_p(hello, 1);
        stl_le_p(hello + 8, 3);
        v_send(v, hello, 48);
    }
    g_assert_false(v_take(v, DSHM_MSG_WELCOME, &m, V_WAIT_MS));
    g_assert_true(v->closed);
    v_close(v);
    qtest_quit(qts);
}

/* A connection that says nothing disturbs nobody; the viewer cap holds. */
static void test_connections(void)
{
    QTestState *qts = start_with(",max-viewers=2");
    Viewer *a = open_v(1, 0), *idle = raw(), *b = open_v(1, 0), *c;
    VFrame f;
    VMsg m = { };

    g_assert_true(next_frame(a, 0, 0, &f, V_WAIT_MS));
    g_assert_true(next_frame(b, 0, 0, &f, V_WAIT_MS));
    /* the third viewer is turned away */
    c = raw();
    {
        uint8_t hello[120] = { };

        stw_le_p(hello, DSHM_MSG_HELLO);
        stw_le_p(hello + 2, DSHM_F_MUST);
        stl_le_p(hello + 4, 120);
        stw_le_p(hello + 8, 4);
        stl_le_p(hello + 48, 1);
        stl_le_p(hello + 80, 1);
        v_send(c, hello, 120);
    }
    g_assert_true(v_take(c, DSHM_MSG_ERROR, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, DSHM_ERR_LIMIT);
    v_msg_free(&m);
    v_close(c);
    v_close(idle);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* ---- Input -------------------------------------------------------------- */

static Viewer *open_input(uint32_t credit)
{
    return v_open(sock_path, (VOpts) { .credit = credit, .test = true,
                                       .steer = true,
                                       .features = DSHM_FEAT_INPUT });
}

static void expect_echo(Viewer *v, uint32_t kind, uint32_t code, int32_t a,
                        int32_t b)
{
    VEcho e = { };

    g_assert_true(v_echo(v, &e, V_WAIT_MS));
    g_assert_cmpuint(e.kind, ==, kind);
    if (kind != 5) {
        g_assert_cmpuint(e.code, ==, code);
        g_assert_cmpint(e.a, ==, a);
        g_assert_cmpint(e.b, ==, b);
    }
}

/* A steering packet and the end of its report. */
static void expect_step(Viewer *v, int32_t dx, int32_t dy)
{
    expect_echo(v, 6, 0, dx, dy);
    expect_echo(v, 5, 0, 0, 0);
}

static void wait_ack(Viewer *v, uint32_t serial)
{
    VMsg m = { };

    v_sync(v, true, serial);
    g_assert_true(v_take(v, DSHM_MSG_INPUT_ACK, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8), ==, serial);
    v_msg_free(&m);
}

static void no_echo(Viewer *v)
{
    VEcho e = { };

    g_assert_false(v_echo(v, &e, V_QUIET_MS));
}

static void test_input_reports(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);
    VMsg m = { };

    /* the first viewer with INPUT owns it, and is told */
    g_assert_true(v_take(v, DSHM_MSG_INPUT_STATE, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8) & 1, ==, 1);
    v_msg_free(&m);
    /* a press and a release are two reports */
    v_key(v, 0, 30, 1);
    v_sync(v, false, 0);
    v_key(v, 0, 30, 0);
    wait_ack(v, 7);
    expect_echo(v, 1, 30, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 1, 30, 0, 0);
    expect_echo(v, 5, 0, 0, 0);
    no_echo(v);
    /* each button change is a report of its own, with no SYNC at all */
    v_key(v, 0, 31, 1);
    v_button(v, 0, 0, 1);
    v_button(v, 0, 0, 0);
    wait_ack(v, 8);
    expect_echo(v, 1, 31, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 2, 0, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 2, 0, 0, 0);
    expect_echo(v, 5, 0, 0, 0);
    no_echo(v);
    v_close(v);
    qtest_quit(qts);
}

static void test_input_rel_sums(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    /* sums in order around keys; a head change or any other event hands over */
    v_motion(v, DSHM_MSG_REL, 0, 5, 5);
    v_motion(v, DSHM_MSG_REL, 0, 7, -2);
    v_key(v, 0, 30, 1);
    v_motion(v, DSHM_MSG_REL, 0, 1, 1);
    wait_ack(v, 1);
    expect_echo(v, 3, 0, 12, 3);
    expect_echo(v, 1, 30, 1, 0);
    expect_echo(v, 3, 0, 1, 1);
    expect_echo(v, 5, 0, 0, 0);
    /* a sum past the clamp reaches the guest as 4096, never wrapped */
    qtest_clock_step(qts, 20 * 1000000);
    v_motion(v, DSHM_MSG_REL, 0, 2000000000, -2000000000);
    v_motion(v, DSHM_MSG_REL, 0, 2000000000, -2000000000);
    wait_ack(v, 2);
    expect_echo(v, 3, 0, 4096, -4096);
    expect_echo(v, 5, 0, 0, 0);
    /* the budget refills with guest time */
    qtest_clock_step(qts, 20 * 1000000);
    v_motion(v, DSHM_MSG_REL, 0, 100, 100);
    wait_ack(v, 3);
    expect_echo(v, 3, 0, 100, 100);
    expect_echo(v, 5, 0, 0, 0);
    v_close(v);
    qtest_quit(qts);
}

static void test_input_report_cap(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    for (unsigned i = 0; i < 300; i++) {
        v_key(v, 0, 30, !(i & 1));
    }
    wait_ack(v, 1);
    for (unsigned i = 0; i < 256; i++) {
        expect_echo(v, 1, 30, !(i & 1), 0);
    }
    expect_echo(v, 5, 0, 0, 0);
    for (unsigned i = 256; i < 300; i++) {
        expect_echo(v, 1, 30, !(i & 1), 0);
    }
    expect_echo(v, 5, 0, 0, 0);
    no_echo(v);
    v_close(v);
    qtest_quit(qts);
}

static void test_input_dropped(void)
{
    QTestState *qts = start();
    Viewer *v = v_open(sock_path, (VOpts) { .credit = 1, .test = true,
                                            .features = DSHM_FEAT_INPUT });

    v_key(v, 0, 0, 1);
    v_key(v, 0, 0x300, 1);
    v_key(v, 0, 30, 2);
    v_key(v, 9, 30, 1);
    v_button(v, 0, 9, 1);
    v_button(v, 0, 0, 7);
    v_motion(v, DSHM_MSG_REL, 9, 1, 1);
    v_motion(v, DSHM_MSG_ABS, 9, 1, 1);
    /* a relative pointer takes no absolute event */
    v_motion(v, DSHM_MSG_ABS, 0, 10, 10);
    wait_ack(v, 1);
    no_echo(v);
    g_assert_false(v->closed);
    v_close(v);
    qtest_quit(qts);
}

static void test_input_ownership(void)
{
    QTestState *qts = start();
    Viewer *a = open_input(1), *b = open_input(1);
    uint8_t claim[8] = { 1 };
    VMsg m = { };

    /* only the owner is heard */
    v_key(b, 0, 40, 1);
    wait_ack(b, 1);
    no_echo(a);
    v_key(a, 0, 41, 1);
    wait_ack(a, 1);
    expect_echo(a, 1, 41, 1, 0);
    expect_echo(a, 5, 0, 0, 0);
    expect_echo(b, 1, 41, 1, 0);
    expect_echo(b, 5, 0, 0, 0);
    /* a claim releases what the old owner held, and both are told */
    v_send_msg(b, DSHM_MSG_INPUT_CLAIM, 0, claim, 8);
    expect_echo(a, 1, 41, 0, 0);
    expect_echo(a, 5, 0, 0, 0);
    g_assert_true(v_take(a, DSHM_MSG_INPUT_STATE, &m, V_WAIT_MS));
    g_assert_true(v_take(a, DSHM_MSG_INPUT_STATE, &m, V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 8) & 1, ==, 0);
    v_msg_free(&m);
    expect_echo(b, 1, 41, 0, 0);
    expect_echo(b, 5, 0, 0, 0);
    v_key(a, 0, 42, 1);
    wait_ack(a, 2);
    v_key(b, 0, 43, 1);
    wait_ack(b, 2);
    expect_echo(b, 1, 43, 1, 0);
    expect_echo(b, 5, 0, 0, 0);
    expect_echo(a, 1, 43, 1, 0);
    expect_echo(a, 5, 0, 0, 0);
    /* INPUT_RESET and disconnect release too */
    {
        uint8_t reset[1] = { };

        v_send_msg(b, DSHM_MSG_INPUT_RESET, 0, reset, 0);
    }
    expect_echo(a, 1, 43, 0, 0);
    expect_echo(a, 5, 0, 0, 0);
    expect_echo(b, 1, 43, 0, 0);
    expect_echo(b, 5, 0, 0, 0);
    v_key(b, 0, 44, 1);
    wait_ack(b, 3);
    expect_echo(a, 1, 44, 1, 0);
    expect_echo(a, 5, 0, 0, 0);
    v_fd_close(b->fd);
    b->fd = -1;
    expect_echo(a, 1, 44, 0, 0);
    v_close(b);
    v_close(a);
    qtest_quit(qts);
}

/* ---- Steering ----------------------------------------------------------- */

static void guest_cursor(Viewer *v, int x, int y)
{
    v_stimulus(v, 0, 2, (uint64_t)x | ((uint64_t)y << 16), 0, 1);
    /* a round trip, so the move is applied before the clock steps */
    v_stats(v);
}

static void expect_done(Viewer *v, uint32_t reason, int32_t x, int32_t y)
{
    VMsg m = { };

    g_assert_true(v_take(v, v_steer_ext(v, DSHM_STEER_STEER_DONE), &m,
                         V_WAIT_MS));
    g_assert_cmpuint(le32(m.d + 12), ==, reason);
    if (x >= 0) {
        g_assert_cmpint((int32_t)le32(m.d + 16), ==, x);
        g_assert_cmpint((int32_t)le32(m.d + 20), ==, y);
    }
    v_msg_free(&m);
}

static void test_steer_basic(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);
    const uint8_t *blk;

    g_assert_cmpuint(v->head[0].pointer_mode, ==, DSHM_POINTER_STEERED);
    guest_cursor(v, 10, 10);
    v_motion(v, DSHM_MSG_ABS, 0, 110, 60);
    /* one packet, sized from a gain of one */
    expect_step(v, 100, 50);
    guest_cursor(v, 110, 60);
    qtest_clock_step(qts, 60 * 1000000);
    expect_done(v, 1, 110, 60);
    blk = v->state + v->steer_block_off;
    g_assert_cmpuint(le32(blk + 4), ==, 1);
    g_assert_cmpint((int32_t)le32(blk + 8), ==, 110);
    g_assert_cmpint((int32_t)le32(blk + 12), ==, 60);
    g_assert_cmpuint(le32(blk + 16), ==, 0);
    v_close(v);
    qtest_quit(qts);
}

/* A guest that never answers ends the move in 250 ms of guest time. */
static void test_steer_no_answer(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    guest_cursor(v, 0, 0);
    v_motion(v, DSHM_MSG_ABS, 0, 50, 50);
    expect_step(v, 50, 50);
    qtest_clock_step(qts, 300 * 1000000);
    expect_done(v, 3, -1, -1);
    v_close(v);
    qtest_quit(qts);
}

/* ABS retargets without a second packet in flight. */
static void test_steer_retarget(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    guest_cursor(v, 0, 0);
    v_motion(v, DSHM_MSG_ABS, 0, 40, 40);
    expect_step(v, 40, 40);
    v_motion(v, DSHM_MSG_ABS, 0, 60, 20);
    v_motion(v, DSHM_MSG_ABS, 0, 80, 30);
    no_echo(v);
    guest_cursor(v, 40, 40);
    qtest_clock_step(qts, 60 * 1000000);
    /* the next packet is sized toward the newest target */
    {
        VEcho e = { };

        g_assert_true(v_echo(v, &e, V_WAIT_MS));
        g_assert_cmpuint(e.kind, ==, 6);
        g_assert_cmpint(e.a, >, 0);
        g_assert_cmpint(e.b, <, 0);
    }
    v_close(v);
    qtest_quit(qts);
}

/* A click waits for the move; a key behind it waits for the click. */
static void test_steer_click_order(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    guest_cursor(v, 0, 0);
    v_motion(v, DSHM_MSG_ABS, 0, 90, 50);
    expect_step(v, 90, 50);
    v_button(v, 0, 0, 1);
    v_key(v, 0, 30, 1);
    v_button(v, 0, 0, 0);
    v_stats(v);
    no_echo(v);
    guest_cursor(v, 90, 50);
    qtest_clock_step(qts, 60 * 1000000);
    expect_done(v, 1, 90, 50);
    expect_echo(v, 2, 0, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 1, 30, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 2, 0, 0, 0);
    expect_echo(v, 5, 0, 0, 0);
    v_close(v);
    qtest_quit(qts);
}

/* A queue that has waited 150 ms flushes while the move is still going. */
static void test_steer_queue_deadline(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    guest_cursor(v, 0, 0);
    v_motion(v, DSHM_MSG_ABS, 0, 90, 50);
    expect_step(v, 90, 50);
    v_button(v, 0, 0, 1);
    v_stats(v);
    no_echo(v);
    qtest_clock_step(qts, 120 * 1000000);
    no_echo(v);
    qtest_clock_step(qts, 40 * 1000000);
    expect_echo(v, 2, 0, 1, 0);
    v_close(v);
    qtest_quit(qts);
}

/* REL ends a move with the queue flushed first; the REL follows. */
static void test_steer_cancel_rel(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);

    guest_cursor(v, 0, 0);
    v_motion(v, DSHM_MSG_ABS, 0, 90, 50);
    expect_step(v, 90, 50);
    v_button(v, 0, 0, 1);
    v_motion(v, DSHM_MSG_REL, 0, 3, 4);
    wait_ack(v, 1);
    expect_done(v, 4, -1, -1);
    expect_echo(v, 2, 0, 1, 0);
    expect_echo(v, 5, 0, 0, 0);
    expect_echo(v, 3, 0, 3, 4);
    expect_echo(v, 5, 0, 0, 0);
    v_close(v);
    qtest_quit(qts);
}

/* INPUT_RESET, owner change and a mode change end it too. */
static void test_steer_cancel_other(void)
{
    QTestState *qts = start();
    Viewer *a = open_input(1), *b = open_input(1);
    uint8_t claim[8] = { 1 };

    guest_cursor(a, 0, 0);
    v_motion(a, DSHM_MSG_ABS, 0, 90, 50);
    expect_step(a, 90, 50);
    expect_step(b, 90, 50);
    v_button(a, 0, 0, 1);
    /* the new owner: the old one's move ends and its queue is dropped */
    v_send_msg(b, DSHM_MSG_INPUT_CLAIM, 0, claim, 8);
    expect_done(a, 4, -1, -1);
    v_stats(a);
    qtest_clock_step(qts, 300 * 1000000);
    /* nothing from the old owner reaches the guest afterwards */
    no_echo(b);
    /* a mode change */
    guest_cursor(b, 5, 5);
    v_motion(b, DSHM_MSG_ABS, 0, 60, 60);
    expect_step(b, 55, 55);
    v_stimulus(b, 0, 4, 64 | (64u << 16), 0, 0);
    expect_done(b, 4, -1, -1);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* A SAMPLE_NOW with no credit survives the plain retry after the release. */
static void test_unbound_sample_now_kept(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(1, 0), *b = open_v(1, 0);
    VFrame f, fb, g;

    g_assert_true(next_frame(a, 0, 0, &f, V_WAIT_MS));
    g_assert_true(next_frame(b, 0, 0, &fb, V_WAIT_MS));
    v_release(b, &fb);
    v_pattern(b, 0, 1, 4, 1);
    g_assert_true(next_frame(b, 0, fb.serial, &fb, V_WAIT_MS));
    g_assert_cmpuint(fb.serial, >, f.serial);
    g_usleep(20000);
    g_assert_cmpuint(v_control(a, 1, 0, 0, 0), ==, 0);
    /* A is at its credit: the request is recorded unbound */
    v_request(a, 0, DSHM_REQ_FRAME, f.serial, 0, DSHM_REQ_SAMPLE_NOW);
    v_release(a, &f);
    v_request(a, 0, DSHM_REQ_FRAME, f.serial, 0, 0);
    /* it binds to the held sample, so B's newer frame is not handed over */
    g_assert_false(v_frame(a, &g, V_QUIET_MS));
    g_assert_cmpuint(v_control(a, 2, 0, 0, 0), ==, 0);
    g_assert_true(v_frame(a, &g, V_WAIT_MS));
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* A FRAME still wholly queued is replaced by the newer one, not sent too. */
static void test_replace_queued_frame(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VFrame f0, g;
    VStats s;

    v_pattern(v, 0, 3, 17, 0);
    g_assert_true(next_frame(v, 0, 0, &f0, V_WAIT_MS));
    v_release(v, &f0);
    g_assert_cmpuint(v_control(v, 5, 0, 0, 0), ==, 0);
    v_pattern(v, 0, 3, 17, 1);
    g_usleep(10000);
    v_request(v, 0, DSHM_REQ_FRAME, f0.serial, 0, DSHM_REQ_SAMPLE_NOW);
    /* an unknown op still answers, urgently: a round trip while held */
    v_control(v, 9, 0, 0, 0);
    v_pattern(v, 0, 3, 17, 2);
    g_usleep(10000);
    v_request(v, 0, DSHM_REQ_FRAME, f0.serial, 0, DSHM_REQ_SAMPLE_NOW);
    v_control(v, 9, 0, 0, 0);
    g_assert_cmpuint(v_control(v, 6, 0, 0, 0), ==, 0);
    g_assert_true(v_frame(v, &g, V_WAIT_MS));
    g_assert_cmpuint(v_frame_step(v, &g), ==, 2);
    v_check_picture(v, &g, 3, 17, 2);
    /* the replaced frame took the second lease_seq; this is the third */
    g_assert_cmpuint(g.lease_seq, ==, f0.lease_seq + 2);
    g_assert_cmpuint(g.lease, ==, f0.lease);
    g_assert_false(v_frame(v, &g, V_QUIET_MS));
    s = v_stats(v);
    g_assert_cmpuint(s.max_queued, ==, 1);
    v_close(v);
    qtest_quit(qts);
}

/* A removed head's lease frees its memory when the viewer lets go. */
static void test_head_removal_frees(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VMsg m = { };
    VFrame f;
    VStats before, after;

    v_stimulus(v, 0, 5, 0, 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_true(next_frame(v, 1, 0, &f, V_WAIT_MS));
    before = v_stats(v);
    v_stimulus(v, 1, 5, 0, 0, 0);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_false(v->head[1].present);
    /* the lease keeps the slots until it is released */
    after = v_stats(v);
    g_assert_cmpuint(after.retained, ==, before.retained);
    v_release(v, &f);
    /* noticed when the viewer next speaks, so the second answer shows it */
    v_stats(v);
    after = v_stats(v);
    g_assert_cmpuint(after.retained, <, before.retained);
    v_close(v);
    qtest_quit(qts);
}

/* IMPORT_FAILED needs-write and representation move the viewer; a second
 * device loss ends the session. */
static void test_import_failed_moves(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, DSHM_FEAT_RW_PIXELS), *w = open_v(1, 0);
    uint8_t b[8] = { };
    uint32_t old;
    VMsg m = { };

    for (unsigned i = 0; i < 3; i++) {
        old = v->head[0].slot[0];
        stl_le_p(b, old);
        stl_le_p(b + 4, i == 0 ? DSHM_IMPORT_NEEDS_WRITE :
                         DSHM_IMPORT_REPRESENTATION);
        v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
        g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
        v_msg_free(&m);
        g_assert_cmpuint(v->head[0].slot[0], !=, old);
        g_assert_cmpuint(v->head[0].n_slots, >, 0);
    }
    /* without RW_PIXELS there is nowhere to move a viewer that needs write */
    stl_le_p(b, w->head[0].slot[0]);
    stl_le_p(b + 4, DSHM_IMPORT_NEEDS_WRITE);
    v_send_msg(w, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    expect_error(w, DSHM_ERR_UNSUPPORTED);
    /* a device lost twice */
    for (unsigned i = 0; i < 2; i++) {
        stl_le_p(b, v->head[0].slot[0]);
        stl_le_p(b + 4, DSHM_IMPORT_DEVICE_LOST);
        v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
        if (!i) {
            g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
            v_msg_free(&m);
        }
    }
    expect_error(v, DSHM_ERR_UNSUPPORTED);
    v_close(v);
    v_close(w);
    qtest_quit(qts);
}

/* SET_STORAGE re-homes to a fresh set. */
static void test_set_storage(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    uint8_t b[40] = { };
    uint32_t old = v->head[0].slot[0];
    VMsg m = { };

    stl_le_p(b, 0xffffffffu);
    stl_le_p(b + 8, 1);
    v_send_msg(v, DSHM_MSG_SET_STORAGE, DSHM_F_MUST, b, sizeof(b));
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(v->head[0].slot[0], !=, old);
    /* no shm in the list is a protocol error */
    memset(b, 0, sizeof(b));
    stl_le_p(b, 0xffffffffu);
    stl_le_p(b + 8, 2);
    v_send_msg(v, DSHM_MSG_SET_STORAGE, DSHM_F_MUST, b, sizeof(b));
    expect_error(v, DSHM_ERR_PROTOCOL);
    v_close(v);
    qtest_quit(qts);
}

/* Held keys are released on the head they went to. */
static void test_held_key_head(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);
    uint8_t reset[1] = { };
    VMsg m = { };
    VEcho e = { };

    v_stimulus(v, 0, 5, 0, 0, 1);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    v_key(v, 1, 30, 1);
    wait_ack(v, 1);
    g_assert_true(v_echo(v, &e, V_WAIT_MS));
    g_assert_cmpuint(e.head, ==, 1);
    g_assert_cmpuint(e.kind, ==, 1);
    g_assert_true(v_echo(v, &e, V_WAIT_MS));
    v_send_msg(v, DSHM_MSG_INPUT_RESET, 0, reset, 0);
    g_assert_true(v_echo(v, &e, V_WAIT_MS));
    g_assert_cmpuint(e.kind, ==, 1);
    g_assert_cmpuint(e.code, ==, 30);
    g_assert_cmpint(e.a, ==, 0);
    g_assert_cmpuint(e.head, ==, 1);
    v_close(v);
    qtest_quit(qts);
}

/* More heads than the state region has room for are not published. */
static void test_head_capacity(void)
{
    QTestState *qts = start_with(",heads=1");
    Viewer *v = open_v(1, 0);
    VFrame f;

    v_stimulus(v, 0, 5, 0, 0, 1);
    v_stats(v);
    g_assert_false(v->head[1].present);
    g_assert_true(next_frame(v, 0, 0, &f, V_WAIT_MS));
    v_close(v);
    qtest_quit(qts);
}

/* Writes held while many messages queue: the queue is not walked each time. */
static void test_held_queue_linear(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    gint64 t0 = g_get_monotonic_time();
    VStats s;

    g_assert_cmpuint(v_control(v, 5, 0, 0, 0), ==, 0);
    for (unsigned i = 0; i < 30000; i++) {
        v_send_msg(v, v_ext(v, DSHM_TEST_STATS_REQUEST), 0, NULL, 0);
    }
    g_assert_cmpuint(v_control(v, 6, 0, 0, 0), ==, 0);
    for (unsigned i = 0; i < 30000; i++) {
        VMsg m = { };

        g_assert_true(v_take(v, v_ext(v, DSHM_TEST_STATS), &m, V_WAIT_MS));
        v_msg_free(&m);
    }
    s = v_stats(v);
    g_assert_cmpuint(s.max_queued, <=, 1);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, 20 * G_USEC_PER_SEC);
    v_close(v);
    qtest_quit(qts);
}

/* A press dropped for the wheel budget takes its release with it. */
static void test_wheel_pair_dropped(void)
{
    QTestState *qts = start();
    Viewer *v = open_input(1);
    unsigned downs = 0, ups = 0;
    VEcho e = { };

    for (unsigned i = 0; i < 70; i++) {
        v_button(v, 0, 3, 1);
        v_button(v, 0, 3, 0);
    }
    wait_ack(v, 1);
    while (v_echo(v, &e, V_QUIET_MS)) {
        if (e.kind == 2 && e.code == 3) {
            downs += e.a;
            ups += !e.a;
        }
    }
    g_assert_cmpuint(downs, ==, 64);
    g_assert_cmpuint(ups, ==, 64);
    v_close(v);
    qtest_quit(qts);
}

/* A mode past what a head can be is nothing, not a crash. */
static void test_mode_too_large(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, 0);
    VMsg m = { };
    VFrame f;

    v_stimulus(v, 0, 4, 20000 | (8ull << 16), 0, 0);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_cmpuint(v->head[0].width, ==, 0);
    v_stimulus(v, 0, 4, 8 | (20000ull << 16), 0, 0);
    v_stats(v);
    g_assert_cmpuint(v->head[0].height, ==, 0);
    v_request(v, 0, DSHM_REQ_FRAME, 0, 0, 0);
    g_assert_false(v_frame(v, &f, V_QUIET_MS));
    v_stimulus(v, 0, 4, 64 | (32ull << 16), 0, 0);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    g_assert_true(v_frame(v, &f, V_WAIT_MS));
    v_close(v);
    qtest_quit(qts);
}

/* vulkan and udmabuf are refused until they exist. */
static void test_unimplemented_refused(void)
{
    const char *bad[] = { ",vulkan=on", ",udmabuf=auto",
                          ",retire-on-disconnect=all" };

    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        g_autofree char *out = NULL;
        g_autofree char *err = NULL;
        g_autofree char *dev = g_strdup_printf("display-shm,path=%s%s",
                                               sock_path, bad[i]);
        char *argv[] = { getenv("QTEST_QEMU_BINARY"), (char *)"-machine",
                         (char *)"none", (char *)"-nodefaults",
                         (char *)"-display", (char *)"none",
                         (char *)"-device", dev, NULL };
        int status;

        g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL,
                                   NULL, &out, &err, &status, NULL));
#ifdef _WIN32
        g_assert_cmpint(status, !=, 0);
#else
        g_assert_true(!WIFEXITED(status) || WEXITSTATUS(status));
#endif
        g_assert_nonnull(strstr(err, "not implemented"));
    }
}

#ifdef _WIN32
/* A read-only section handle cannot be mapped for write. */
static void test_regions_rights(void)
{
    QTestState *qts = start();
    Viewer *v = open_v(1, DSHM_FEAT_RW_PIXELS);
    VRegion *regs[3] = { v_region(v, v->state_region),
                         v_region(v, v->feedback_region),
                         v_region(v, v->head[0].slot[0]) };

    g_assert_cmpuint(v->handles_taken, ==, v->handles_parsed);
    for (unsigned i = 0; i < 3; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        bool writable = i == 1 || regs[i]->access & DSHM_ACCESS_WRITE;
        void *p = MapViewOfFile(regs[i]->h, FILE_MAP_WRITE, 0, 0, 0);

        g_assert_cmpint(p != NULL, ==, writable);
        if (p) {
            UnmapViewOfFile(p);
        } else {
            g_assert_cmpuint(GetLastError(), ==, ERROR_ACCESS_DENIED);
        }
        p = MapViewOfFile(regs[i]->h, FILE_MAP_READ, 0, 0, 0);
        g_assert_nonnull(p);
        g_assert_cmpuint(VirtualQuery(p, &mbi, sizeof(mbi)), ==, sizeof(mbi));
        g_assert_cmpuint(mbi.RegionSize, ==, regs[i]->size);
        UnmapViewOfFile(p);
    }
    v_close(v);
    qtest_quit(qts);
}

/* Handles sent past the viewer's taken count come back in ORPHANS. */
static void test_orphans(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(1, 0), *b;
    uint64_t taken, session = a->session;
    uint32_t id = a->id, n;
    VMsg m = { };

    v_stimulus(a, 0, 4, 96 | (48u << 16), 0, 0);
    v_request(a, 0, DSHM_REQ_FRAME, 0, 0, 0);
    g_usleep(300000);
    taken = a->handles_taken;
    v_fd_close(a->fd);
    a->fd = -1;
    b = v_open(sock_path, (VOpts) { .credit = 1, .test = true, .prev_id = id,
                                    .prev_taken = taken,
                                    .prev_session = session });
    g_assert_true(v_take(b, DSHM_MSG_ORPHANS, &m, V_WAIT_MS));
    n = le32(m.d + 8);
    g_assert_cmpuint(n, >=, 1);
    g_assert_cmpuint(m.len, ==, 16 + 16 * n);
    for (uint32_t i = 0; i < n; i++) {
        HANDLE h = (HANDLE)(uintptr_t)le64(m.d + 24 + 16 * i);
        DWORD flags;

        g_assert_cmpuint(le64(m.d + 16 + 16 * i), ==, taken + 1 + i);
        g_assert_true(GetHandleInformation(h, &flags));
        CloseHandle(h);
    }
    v_msg_free(&m);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}

/* A taken count above what was sent is a protocol error, not an orphan. */
static void test_orphans_bad_count(void)
{
    QTestState *qts = start();
    Viewer *a = open_v(1, 0), *b = raw();
    uint8_t hello[120] = { };

    v_stimulus(a, 0, 4, 96 | (48u << 16), 0, 0);
    v_request(a, 0, DSHM_REQ_FRAME, 0, 0, 0);
    g_usleep(300000);
    stw_le_p(hello, DSHM_MSG_HELLO);
    stw_le_p(hello + 2, DSHM_F_MUST);
    stl_le_p(hello + 4, 120);
    stw_le_p(hello + 8, 4);
    stl_le_p(hello + 48, 1);
    stl_le_p(hello + 80, 1);
    stl_le_p(hello + 92, a->id);
    stq_le_p(hello + 104, a->handles_taken + 1000);
    stq_le_p(hello + 112, a->session);
    v_fd_close(a->fd);
    a->fd = -1;
    v_send(b, hello, 120);
    expect_error(b, DSHM_ERR_PROTOCOL);
    v_close(a);
    v_close(b);
    qtest_quit(qts);
}
#endif

/* ---- linux.dmabuf ------------------------------------------------------- */

#ifdef CONFIG_LINUX
/* A viewer that takes dma-buf slots; in these tests they are memfds. */
static Viewer *open_gpu(uint32_t credit, bool ext, uint32_t class)
{
    return v_open(sock_path, (VOpts) { .credit = credit, .test = true,
                                       .dmabuf = ext, .storage1 = class });
}

static void check_gpu_picture(Viewer *v, const VFrame *f, unsigned pattern,
                              uint64_t seed, uint32_t step)
{
    const VHead *h = &v->head[f->head];

    for (unsigned y = 0; y < h->height; y++) {
        for (unsigned x = 0; x < h->width; x++) {
            g_assert_cmphex(v_gpu_px(v, f, x, y), ==,
                            t_pixel(pattern, seed, step, x, y, h->width,
                                    h->height));
        }
    }
}

static void expect_slots(Viewer *v, uint32_t storage, unsigned n)
{
    const VHead *h = &v->head[0];

    g_assert_cmpuint(h->n_slots, ==, n);
    for (unsigned i = 0; i < h->n_slots; i++) {
        VRegion *r = v_region(v, h->slot[i]);

        g_assert_nonnull(r);
        g_assert_cmpuint(r->storage, ==, storage);
        g_assert_true(V_HANDLE_OK(r->h));
        if (storage == DSHM_STORAGE_DMABUF) {
            g_assert_cmpuint(r->gflags & DSHM_GPU_WRITTEN, ==,
                             DSHM_GPU_WRITTEN);
            g_assert_cmpuint(r->gstride, >=, h->width * 4);
        }
    }
}

/* A frame drawn by the GPU stand-in, none of it copied by the producer. */
static void test_dmabuf_frames(void)
{
    QTestState *qts = start();
    Viewer *v = open_gpu(2, true, DSHM_STORAGE_DMABUF);
    VStats before, after;
    uint64_t seen = 0;
    VFrame f;

    g_assert_cmpuint(v->gpu_alloc, ==, DSHM_ALLOC_GPU);
    /* the newest picture, the one being drawn and one per credit */
    expect_slots(v, DSHM_STORAGE_DMABUF, 4);
    before = v_stats(v);
    for (uint32_t step = 1; step <= 3; step++) {
        v_pattern(v, 0, 1, 7, step);
        g_assert_true(fresh_frame(v, 0, seen, &f));
        g_assert_cmpuint(f.serial, >, seen);
        check_gpu_picture(v, &f, 1, 7, step);
        v_release(v, &f);
        seen = f.serial;
    }
    after = v_stats(v);
    g_assert_cmpuint(after.frames_sent - before.frames_sent, ==, 3);
    g_assert_cmpuint(after.lines_produced, ==, before.lines_produced);
    g_assert_cmpuint(after.lines_copied, ==, before.lines_copied);
    v_close(v);
    qtest_quit(qts);
}

/* A viewer that comes while the GPU is still opening gets it once it is. */
static void test_dmabuf_gpu_opens_late(void)
{
    QTestState *qts = start();
    Viewer *c = open_v(1, 0);
    Viewer *v = NULL;
    VFrame f;

    v_stimulus(c, 0, 7, 0, 0, 1);
    v_stats(c);
    v = open_gpu(1, true, DSHM_STORAGE_DMABUF);
    expect_slots(v, DSHM_STORAGE_SHM, 3);   /* the set it shares with c */
    v_stimulus(c, 0, 7, 0, 0, 0);
    v_stats(c);
    for (unsigned i = 0; i < 20; i++) {
        const VHead *h = &v->head[0];
        VRegion *r = h->n_slots ? v_region(v, h->slot[0]) : NULL;

        if (r && r->storage == DSHM_STORAGE_DMABUF) {
            break;
        }
        v_pump(v, 500);
    }
    expect_slots(v, DSHM_STORAGE_DMABUF, 3);
    v_pattern(c, 0, 1, 5, 2);
    g_assert_true(fresh_frame(v, 0, 0, &f));
    check_gpu_picture(v, &f, 1, 5, 2);
    v_close(c);
    v_close(v);
    qtest_quit(qts);
}

/* Both halves of the negotiation, and an old viewer, keep shared memory. */
static void test_dmabuf_negotiation(void)
{
    QTestState *qts = start();
    Viewer *v;
    VFrame f;

    v = open_gpu(1, true, 0);           /* extension, but no class */
    g_assert_cmpuint(v->gpu_alloc, ==, 0);
    expect_slots(v, DSHM_STORAGE_SHM, 2);
    v_close(v);
    v = open_gpu(1, false, DSHM_STORAGE_DMABUF);    /* class, no extension */
    g_assert_cmpuint(v->gpu_alloc, ==, 0);
    expect_slots(v, DSHM_STORAGE_SHM, 2);
    v_pattern(v, 0, 1, 3, 1);
    g_assert_true(fresh_frame(v, 0, 0, &f));
    v_check_picture(v, &f, 1, 3, 1);
    v_close(v);
    qtest_quit(qts);
}

/* One viewer has the images; the next gets shared memory and a CPU frame. */
static void test_dmabuf_one_viewer(void)
{
    QTestState *qts = start();
    Viewer *v = open_gpu(1, true, DSHM_STORAGE_DMABUF);
    Viewer *w = open_gpu(1, true, DSHM_STORAGE_DMABUF);
    VStats before, after;
    VFrame f, g;

    expect_slots(v, DSHM_STORAGE_DMABUF, 3);
    expect_slots(w, DSHM_STORAGE_SHM, 2);
    before = v_stats(v);
    v_pattern(v, 0, 1, 9, 5);
    g_assert_true(fresh_frame(v, 0, 0, &f));
    g_assert_true(fresh_frame(w, 0, 0, &g));
    check_gpu_picture(v, &f, 1, 9, 5);
    v_check_picture(w, &g, 1, 9, 5);
    after = v_stats(v);
    g_assert_cmpuint(after.lines_produced, >, before.lines_produced);
    v_close(v);
    v_close(w);
    qtest_quit(qts);
}

/* A lent image is not drawn again, and comes back when released. */
static void test_dmabuf_leases(void)
{
    QTestState *qts = start();
    Viewer *v = open_gpu(2, true, DSHM_STORAGE_DMABUF);
    VFrame f1, f2, f3;

    v_pattern(v, 0, 1, 1, 1);
    g_assert_true(fresh_frame(v, 0, 0, &f1));
    v_pattern(v, 0, 1, 1, 2);
    g_assert_true(fresh_frame(v, 0, f1.serial, &f2));
    g_assert_cmpuint(f2.region, !=, f1.region);
    g_assert_cmpuint(f2.lease, !=, f1.lease);
    check_gpu_picture(v, &f1, 1, 1, 1);
    check_gpu_picture(v, &f2, 1, 1, 2);
    v_release(v, &f1);
    v_release(v, &f2);
    v_pattern(v, 0, 1, 1, 3);
    g_assert_true(fresh_frame(v, 0, f2.serial, &f3));
    check_gpu_picture(v, &f3, 1, 1, 3);
    v_close(v);
    qtest_quit(qts);
}

/* A viewer that cannot import the images is moved to shared memory. */
static void test_dmabuf_import_failed(void)
{
    QTestState *qts = start();
    Viewer *v = open_gpu(1, true, DSHM_STORAGE_DMABUF);
    uint8_t b[8] = { };
    VMsg m = { };
    VFrame f;

    stl_le_p(b, v->head[0].slot[0]);
    stl_le_p(b + 4, DSHM_IMPORT_ADAPTER);
    v_send_msg(v, DSHM_MSG_IMPORT_FAILED, DSHM_F_MUST, b, 8);
    g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
    v_msg_free(&m);
    expect_slots(v, DSHM_STORAGE_SHM, 2);
    v_pattern(v, 0, 1, 4, 6);
    g_assert_true(fresh_frame(v, 0, 0, &f));
    v_check_picture(v, &f, 1, 4, 6);
    v_close(v);
    qtest_quit(qts);
}

/* A new mode gets new images of its size. */
static void test_dmabuf_mode_change(void)
{
    QTestState *qts = start();
    Viewer *v = open_gpu(1, true, DSHM_STORAGE_DMABUF);
    VMsg m = { };
    VFrame f;

    v_stimulus(v, 0, 4, 64 | (32u << 16), 0, 0);
    do {
        g_assert_true(v_take(v, DSHM_MSG_HEAD, &m, V_WAIT_MS));
        v_msg_free(&m);
    } while (v->head[0].width != 64);
    g_assert_cmpuint(v->head[0].height, ==, 32);
    expect_slots(v, DSHM_STORAGE_DMABUF, 3);
    v_pattern(v, 0, 3, 5, 12);
    g_assert_true(fresh_frame(v, 0, 0, &f));
    check_gpu_picture(v, &f, 3, 5, 12);
    v_close(v);
    qtest_quit(qts);
}
#endif

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    sock_dir = g_dir_make_tmp("display-shm-XXXXXX", NULL);
    g_assert(sock_dir);
    sock_path = g_build_filename(sock_dir, "d.sock", NULL);

    qtest_add_func("display-shm/session", test_session);
#ifdef CONFIG_LINUX
    qtest_add_func("display-shm/regions-sealed", test_regions_sealed);
    qtest_add_func("display-shm/rw-pixels-set", test_rw_pixels_set);
#endif
#ifdef _WIN32
    qtest_add_func("display-shm/regions-rights", test_regions_rights);
    qtest_add_func("display-shm/orphans", test_orphans);
    qtest_add_func("display-shm/orphans-bad-count", test_orphans_bad_count);
#endif
    qtest_add_func("display-shm/still-head-on-connect",
                   test_still_head_on_connect);
    qtest_add_func("display-shm/steps", test_steps);
    qtest_add_func("display-shm/damage-coverage", test_damage_coverage);
    qtest_add_func("display-shm/last-frame", test_last_frame);
    qtest_add_func("display-shm/pending-changes", test_pending_changes);
    qtest_add_func("display-shm/no-unchanged-publish",
                   test_no_unchanged_publish);
    qtest_add_func("display-shm/credit-ceiling", test_credit_ceiling);
    qtest_add_func("display-shm/no-write-while-lent",
                   test_no_write_while_lent);
    qtest_add_func("display-shm/unchanged-sample-newer-frame",
                   test_unchanged_sample_newer_frame);
    qtest_add_func("display-shm/sample-now-bound", test_sample_now_bound);
    qtest_add_func("display-shm/retries-crossing", test_retries_crossing);
    qtest_add_func("display-shm/mode-change", test_mode_change);
    qtest_add_func("display-shm/heads", test_heads);
    qtest_add_func("display-shm/slot-list-changes", test_slot_list_changes);
    qtest_add_func("display-shm/two-sets", test_two_sets);
    qtest_add_func("display-shm/rehome-keeps-lease", test_rehome_keeps_lease);
    qtest_add_func("display-shm/rehome-serves-recorded-request",
                   test_rehome_serves_recorded_request);
    qtest_add_func("display-shm/import-too-large", test_import_too_large);
    qtest_add_func("display-shm/import-failed-class",
                   test_import_failed_class);
    qtest_add_func("display-shm/disconnect-retires", test_disconnect_retires);
    qtest_add_func("display-shm/cancellation", test_cancellation);
    qtest_add_func("display-shm/cursor", test_cursor);
    qtest_add_func("display-shm/unbound-sample-now-kept",
                   test_unbound_sample_now_kept);
    qtest_add_func("display-shm/replace-queued-frame",
                   test_replace_queued_frame);
    qtest_add_func("display-shm/head-removal-frees", test_head_removal_frees);
    qtest_add_func("display-shm/import-failed-moves",
                   test_import_failed_moves);
    qtest_add_func("display-shm/set-storage", test_set_storage);
    qtest_add_func("display-shm/held-key-head", test_held_key_head);
    qtest_add_func("display-shm/head-capacity", test_head_capacity);
    qtest_add_func("display-shm/wheel-pair-dropped", test_wheel_pair_dropped);
    qtest_add_func("display-shm/held-queue-linear", test_held_queue_linear);
    qtest_add_func("display-shm/mode-too-large", test_mode_too_large);
    qtest_add_func("display-shm/unimplemented-refused",
                   test_unimplemented_refused);
    qtest_add_func("display-shm/framing", test_framing);
    qtest_add_func("display-shm/input-reports", test_input_reports);
    qtest_add_func("display-shm/input-rel-sums", test_input_rel_sums);
    qtest_add_func("display-shm/input-report-cap", test_input_report_cap);
    qtest_add_func("display-shm/input-dropped", test_input_dropped);
    qtest_add_func("display-shm/input-ownership", test_input_ownership);
    qtest_add_func("display-shm/steer-basic", test_steer_basic);
    qtest_add_func("display-shm/steer-no-answer", test_steer_no_answer);
    qtest_add_func("display-shm/steer-retarget", test_steer_retarget);
    qtest_add_func("display-shm/steer-click-order", test_steer_click_order);
    qtest_add_func("display-shm/steer-queue-deadline",
                   test_steer_queue_deadline);
    qtest_add_func("display-shm/steer-cancel-rel", test_steer_cancel_rel);
    qtest_add_func("display-shm/steer-cancel-other", test_steer_cancel_other);
    qtest_add_func("display-shm/connections", test_connections);
#ifdef CONFIG_LINUX
    qtest_add_func("display-shm/dmabuf-frames", test_dmabuf_frames);
    qtest_add_func("display-shm/dmabuf-gpu-opens-late",
                   test_dmabuf_gpu_opens_late);
    qtest_add_func("display-shm/dmabuf-negotiation", test_dmabuf_negotiation);
    qtest_add_func("display-shm/dmabuf-one-viewer", test_dmabuf_one_viewer);
    qtest_add_func("display-shm/dmabuf-leases", test_dmabuf_leases);
    qtest_add_func("display-shm/dmabuf-import-failed",
                   test_dmabuf_import_failed);
    qtest_add_func("display-shm/dmabuf-mode-change", test_dmabuf_mode_change);
#endif
    ret = g_test_run();

    g_rmdir(sock_dir);
    return ret;
}
