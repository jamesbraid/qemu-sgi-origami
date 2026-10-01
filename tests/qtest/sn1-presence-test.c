/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The Bedrock presence-publication join under multi-threaded TCG, taken
 * by the processors themselves.
 *
 * Right after the reset-edge rendezvous the stock IP35 PROM's lowest
 * processor on each processor interface publishes the interface's fitted
 * processors as bits 4 to 7 of II scratch 0, under the hub lock, and every
 * processor's first barrier then reads those bits to decide whom to wait
 * for: a processor whose bit is still clear is treated as absent. On
 * hardware the publisher's two stores land long before any peer's first
 * such read. Under multi-threaded TCG the publisher's host thread can be
 * left unscheduled across that window, so its peers read the bits clear,
 * pass their first barrier without it, and stay a barrier ahead of it for
 * the rest of the boot -- until the copy barrier, where they switch to the
 * PROM's RAM copy while it is still being made. A successful first barrier
 * therefore does not prove the processors stayed synchronized.
 *
 * Four processors run the same program, with the roles the PROM gives them:
 * every one makes its first LB scratch 1 write; the publishers (slots A and
 * C) make their second, take the hub lock, publish their interface's two
 * bits and release, C only after half a second of the real-time counter
 * has passed; the others (B and D) make their second write at once. Every
 * processor then records its first read of II scratch 0. The model must
 * show each publisher seeing all four bits on that read, since a
 * publisher's first presence read is of the other interface, in the
 * handshake between the two publishers: A must see C's and D's bits, which
 * only its park at the release gives it. B's and D's reads are recorded,
 * not judged: this program has no liveness handshake, so the partner park
 * the second test covers does not apply.
 * Without the join, A reads before C's publication and records 0x30.
 *
 * The second test covers B and D. Between the join and the publication the
 * PROM runs a pair liveness handshake: each processor writes a status value
 * into its own real-time compare register and polls its partner's for it.
 * The publisher then publishes; the partner waits out about 3 ms of the
 * real-time counter and then makes its first presence read. Here both
 * publishers are late after the handshake, by half a second, so the
 * partners' 3 ms run out first unless the model parks them until their own
 * publisher has published. Without that park B reads 0 and D reads 0.
 *
 * The reset barrier comes before all of this: every processor's first LB
 * scratch 1 write clears it, and the master's second posts the release
 * the others poll for. A late processor's clear would erase that release,
 * so the model withholds the release and parks the master until every
 * processor has made its first write. The late-join test has B half a
 * second late to its first write and checks that A's release store took at
 * least that long, under each scheduler.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define II_SCRATCH_0 UINT64_C(0x400150)
#define LB_SCRATCH_1 UINT64_C(0x608008)
#define LB_SCRATCH_3 UINT64_C(0x608018)
#define LB_SCRATCH_3_TEST_AND_SET UINT64_C(0x608058)
#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_RT_COMPARE_A UINT64_C(0x108)
#define PI_RT_COUNTER UINT64_C(0x30100)

/*
 * State words in low RAM, clear of the flash and of the 32 KiB
 * per-processor window the firmware filter overlays at 1 MiB, reached by
 * the guest through Hub special space's uncached alias of its own node
 * memory and by the host through the same alias: plain node memory sits
 * behind the memory unit's bank translation, which answers nothing until
 * firmware configures it. One block per processor, by the PI's own
 * processor number.
 */
#define STATE_PHYS UINT64_C(0x180000)
#define STATE_HOST (SPACE_HUB + STATE_PHYS)
#define STATE_SLOT_STRIDE 0x100
#define STATE_SEEN 0x00
#define STATE_DONE 0x08
#define STATE_LIVE 0x10
#define STATE_WAITED 0x18

#define DONE_MAGIC 0xd04e

/* Every fitted slot's bit, once both interfaces have published. */
#define PRESENCE_ALL 0xf0

/*
 * How long C holds back its publication: half a second of the 1.25 MHz
 * real-time counter, which follows the host clock under multi-threaded
 * TCG, so it is the same under any host load. Far longer than the others
 * need to reach their first read, and a quarter of the model's hold
 * bound, so a parked processor is woken by the publication rather than
 * timing out.
 */
#define PUBLISH_DELAY_TICKS 625000

/*
 * The handshake test's values: the status each processor posts, and the
 * partner's timed wait, 3 ms of the real-time counter, the PROM's own.
 */
#define POST_VALUE 0x24

/*
 * The reset barrier: the master's second LB scratch 1 write, the value
 * the stock PROM releases the other processors with, and a progress code
 * the master then writes to its own real-time compare register before any
 * liveness post (0x3 at 0x1fc0094c in the stock PROM).
 */
#define SCRATCH1_RELEASE 0x7f80000
#define PROGRESS_CODE 0x3
#define PARTNER_WAIT_TICKS 3750

/*
 * The liveness test's values: how late B posts (half a second of the
 * real-time counter, as a host thread left waiting for a host processor
 * through the PROM's fixed 3 s real-time counter delay would make it), and
 * how many times each processor reads
 * its partner's register before giving up. The PROM reads 200,000 times;
 * this is shorter so the test is quick, and still far longer than a partner
 * posting on time needs.
 */
#define LATE_POST_TICKS 625000
#define LIVENESS_TRIES 20000

/*
 * The model's park bound, 2 s, in real-time counter ticks, and the
 * longest a poll woken by its partner's post may take: under the bound, so
 * a park that only its bound released cannot pass for one the post
 * released, with room for round robin's slower turns.
 */
#define PARK_BOUND_TICKS 2500000
#define WOKEN_BY_POST_TICKS (PARK_BOUND_TICKS * 9 / 10)

/* Each interface's own two presence bits, by interface. */
#define PRESENCE_PI0 0x30
#define PRESENCE_PI1 0xc0

#define POLL_TRIES 3000
#define POLL_INTERVAL_US 10000

#define R_ZERO 0
#define R_A0 4
#define R_A1 5
#define R_A2 6
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_T4 12
#define R_T5 13
#define R_S0 16
#define R_S1 17
#define R_S2 18
#define R_S3 19
#define R_S4 20
#define R_S5 21
#define R_S6 22
#define R_S7 23
#define R_FP 30

#define OPC_BEQ 4
#define OPC_BNE 5
#define OPC_ANDI 12
#define OPC_DADDIU 25
#define OPC_ORI 13
#define OPC_XORI 14
#define OPC_LUI 15
#define OPC_LD 55
#define OPC_SD 63

#define CP0_STATUS 12
#define STATUS_KX 0x80

typedef struct Image {
    uint8_t *bytes;
    unsigned int pos;
} Image;

static void emit(Image *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, PROM_SIZE);
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static void emit_i(Image *p, unsigned int op, unsigned int rs,
                   unsigned int rt, uint16_t imm)
{
    emit(p, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

static void emit_nop(Image *p)
{
    emit(p, 0);
}

/* Load a 32-bit constant into @rt. */
static void emit_li32(Image *p, unsigned int rt, uint32_t value)
{
    emit_i(p, OPC_LUI, R_ZERO, rt, value >> 16);
    emit_i(p, OPC_ORI, rt, rt, value);
}

/* Load a 64-bit constant: high halfword, then three shift-and-or steps. */
static void emit_li64(Image *p, unsigned int rt, uint64_t value)
{
    emit_i(p, OPC_ORI, R_ZERO, rt, value >> 48);
    for (unsigned int shift = 32; ; shift -= 16) {
        emit(p, (rt << 16) | (rt << 11) | (16 << 6) | 0x38); /* dsll 16 */
        emit_i(p, OPC_ORI, rt, rt, value >> shift);
        if (shift == 0) {
            break;
        }
    }
}

static void emit_daddu(Image *p, unsigned int rd, unsigned int rs,
                       unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x2d);
}

static void emit_or(Image *p, unsigned int rd, unsigned int rs,
                    unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x25);
}

/* rd = rt << rs. */
static void emit_dsllv(Image *p, unsigned int rd, unsigned int rt,
                       unsigned int rs)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x14);
}

/* rd = rt << sa, for a shift below 32. */
static void emit_dsll(Image *p, unsigned int rd, unsigned int rt,
                      unsigned int sa)
{
    emit(p, (rt << 16) | (rd << 11) | (sa << 6) | 0x38);
}

static void emit_dsubu(Image *p, unsigned int rd, unsigned int rs,
                       unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x2f);
}

static void emit_sltu(Image *p, unsigned int rd, unsigned int rs,
                      unsigned int rt)
{
    emit(p, (rs << 21) | (rt << 16) | (rd << 11) | 0x2b);
}

/* Branch with a delay-slot nop; the target is patched in later. */
static unsigned int emit_branch(Image *p, unsigned int op, unsigned int rs,
                                unsigned int rt)
{
    unsigned int at = p->pos;

    emit_i(p, op, rs, rt, 0);
    emit_nop(p);
    return at;
}

static void patch_branch(Image *p, unsigned int at, unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

/*
 * Guest code. s0 is the local hub window, s1 this processor's state
 * block, s2 LB scratch 1, s3 II scratch 0, s4 the hub lock's test-and-set
 * twin, s5 plain LB scratch 3, s6 the real-time counter, and t2 this
 * processor's number from the PI: bit 0 its slot within the interface, bit
 * 1 the interface.
 */
static void emit_rt_wait(Image *p, uint32_t ticks);

static void emit_prologue_late(Image *p, bool late_b)
{
    unsigned int to_master, spin, to_released, to_first, to_on;

    stl_be_p(p->bytes + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(p->bytes + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p->pos = 0x400;

    /* 64-bit kernel addressing, without which the pointers below fault. */
    emit(p, 0x40000000 | (R_T0 << 16) | (CP0_STATUS << 11)); /* mfc0 */
    emit_i(p, OPC_ORI, R_T0, R_T0, STATUS_KX);
    emit(p, 0x40800000 | (R_T0 << 16) | (CP0_STATUS << 11)); /* mtc0 */
    for (unsigned int i = 0; i < 4; i++) {
        emit_nop(p);
    }

    /* Uncached, I/O space: the local hub register window. */
    emit_li64(p, R_S0, UINT64_C(0x9200000001000000));
    emit_li64(p, R_S2, UINT64_C(0x9200000001000000) | LB_SCRATCH_1);
    emit_li64(p, R_S3, UINT64_C(0x9200000001000000) | II_SCRATCH_0);
    emit_li64(p, R_S4,
              UINT64_C(0x9200000001000000) | LB_SCRATCH_3_TEST_AND_SET);
    emit_li64(p, R_S5, UINT64_C(0x9200000001000000) | LB_SCRATCH_3);
    emit_li64(p, R_S6, UINT64_C(0x9200000001000000) | PI_RT_COUNTER);

    /* This processor's state block, by its processor number. */
    emit_li64(p, R_S1, UINT64_C(0x9000000000000000) | STATE_PHYS);
    emit_i(p, OPC_LD, R_S0, R_T2, PI_CPU_NUMBER);
    emit_i(p, OPC_ANDI, R_T2, R_T2, 3);
    emit(p, (R_T2 << 16) | (R_T0 << 11) | (8 << 6) | 0x38); /* dsll t0,t2,8 */
    emit_daddu(p, R_S1, R_S1, R_T0);

    /*
     * Every processor's first LB scratch 1 write: the reset-edge join. With
     * @late_b, B makes it half a second late, as a host thread left
     * unscheduled would.
     */
    if (late_b) {
        emit_i(p, OPC_XORI, R_T2, R_T4, 1);         /* zero only for B */
        to_first = emit_branch(p, OPC_BNE, R_T4, R_ZERO);
        emit_rt_wait(p, LATE_POST_TICKS);
        patch_branch(p, to_first, p->pos);
    }
    emit_i(p, OPC_SD, R_S2, R_ZERO, 0);

    /*
     * Then the barrier's release, as the stock PROM does it: the master (A)
     * writes the release value, and the others spin until they read it.
     * Every later scratch 1 write stores the same value, so a late reader
     * still sees it.
     */
    emit_li32(p, R_A2, SCRATCH1_RELEASE);
    to_master = emit_branch(p, OPC_BEQ, R_T2, R_ZERO);
    spin = p->pos;
    emit_i(p, OPC_LD, R_S2, R_T0, 0);
    emit_i(p, OPC_BNE, R_T0, R_A2, 0);
    patch_branch(p, p->pos - 4, spin);
    emit_nop(p);
    to_released = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, to_master, p->pos);
    /*
     * The master records how long its release store took, in ticks. A
     * branch follows the store, as the PROM's return does, so the store
     * ends a translation block before the second counter read.
     */
    emit_i(p, OPC_LD, R_S6, R_A0, 0);
    emit_i(p, OPC_SD, R_S2, R_A2, 0);
    to_on = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, to_on, p->pos);
    emit_i(p, OPC_LD, R_S6, R_A1, 0);
    emit_dsubu(p, R_A1, R_A1, R_A0);
    emit_i(p, OPC_SD, R_S1, R_A1, STATE_WAITED);
    patch_branch(p, to_released, p->pos);
}

static void emit_prologue(Image *p)
{
    emit_prologue_late(p, false);
}

/* Wait out @ticks of the real-time counter. */
static void emit_rt_wait(Image *p, uint32_t ticks)
{
    unsigned int wait;

    emit_i(p, OPC_LD, R_S6, R_T0, 0);
    emit_li32(p, R_T1, ticks);
    emit_daddu(p, R_T5, R_T0, R_T1);                 /* deadline */
    wait = p->pos;
    emit_i(p, OPC_LD, R_S6, R_T0, 0);
    emit_sltu(p, R_T1, R_T0, R_T5);                  /* now < deadline */
    emit_i(p, OPC_BNE, R_T1, R_ZERO, 0);
    patch_branch(p, p->pos - 4, wait);
    emit_nop(p);
}

/* Take the hub lock, publish this interface's two bits, release. */
static void emit_publish(Image *p)
{
    unsigned int lock = p->pos;

    emit_i(p, OPC_LD, R_S4, R_T0, 0);                /* test-and-set */
    emit_i(p, OPC_BNE, R_T0, R_ZERO, 0);
    patch_branch(p, p->pos - 4, lock);
    emit_nop(p);
    emit_i(p, OPC_LD, R_S3, R_T0, 0);
    emit_i(p, OPC_ORI, R_ZERO, R_T1, 0x30);          /* both slots' bits */
    emit_dsllv(p, R_T1, R_T1, R_T2);                 /* ... of my interface */
    emit_or(p, R_T0, R_T0, R_T1);
    emit_i(p, OPC_SD, R_S3, R_T0, 0);
    emit_i(p, OPC_SD, R_S5, R_ZERO, 0);              /* release */
}

/* The first presence read, recorded as is, and then a halt in place. */
static void emit_record(Image *p)
{
    emit_i(p, OPC_LD, R_S3, R_T0, 0);
    emit_i(p, OPC_SD, R_S1, R_T0, STATE_SEEN);
    emit_i(p, OPC_ORI, R_ZERO, R_T0, DONE_MAGIC);
    emit_i(p, OPC_SD, R_S1, R_T0, STATE_DONE);
    emit(p, 0x1000ffff); /* beq zero, zero, self */
    emit_nop(p);
}

static void emit_program_after_join(Image *p)
{
    unsigned int branch, to_peer, to_read;

    /* Its slot within the interface: the second one only reads. */
    emit_i(p, OPC_ANDI, R_T2, R_T3, 1);             /* t3 = t2 & 1 */
    to_peer = emit_branch(p, OPC_BNE, R_T3, R_ZERO);

    /*
     * A publisher. C alone (interface bit set) first waits out the delay
     * on the real-time counter, the way a late host thread would.
     */
    emit_i(p, OPC_ANDI, R_T2, R_T4, 2);             /* t4 = t2 & 2 */
    branch = emit_branch(p, OPC_BEQ, R_T4, R_ZERO);
    emit_rt_wait(p, PUBLISH_DELAY_TICKS);
    patch_branch(p, branch, p->pos);

    /* The second scratch 1 write, then publish under the hub lock. */
    emit_i(p, OPC_SD, R_S2, R_A2, 0);
    emit_publish(p);
    to_read = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);

    /* The interface's other processor: its second write, then the read. */
    patch_branch(p, to_peer, p->pos);
    emit_i(p, OPC_SD, R_S2, R_A2, 0);

    patch_branch(p, to_read, p->pos);
    emit_record(p);
}

static void build_program(Image *p)
{
    emit_prologue(p);
    emit_program_after_join(p);
}

/*
 * The same with B half a second late to its first scratch 1 write, so
 * the reset-barrier park withholds A's release until B has written.
 */
static void build_late_join_program(Image *p)
{
    emit_prologue_late(p, true);
    emit_program_after_join(p);
}

/*
 * The handshake program. After both scratch 1 writes, s7 is this
 * processor's own real-time compare register and fp its partner's: the
 * register of the other slot of the same interface.
 */
static void build_handshake_program(Image *p)
{
    unsigned int poll, to_partner, to_read;

    emit_prologue(p);
    emit_i(p, OPC_SD, R_S2, R_A2, 0);               /* past the join */

    emit_i(p, OPC_ANDI, R_T2, R_T3, 1);             /* t3 = slot */
    emit_dsll(p, R_T4, R_T3, 3);
    emit_daddu(p, R_S7, R_S0, R_T4);
    emit_i(p, OPC_XORI, R_T3, R_T4, 1);             /* the partner's slot */
    emit_dsll(p, R_T4, R_T4, 3);
    emit_daddu(p, R_FP, R_S0, R_T4);

    /* Post, then poll the partner's register for its post. */
    emit_i(p, OPC_ORI, R_ZERO, R_T0, POST_VALUE);
    emit_i(p, OPC_SD, R_S7, R_T0, PI_RT_COMPARE_A);
    poll = p->pos;
    emit_i(p, OPC_LD, R_FP, R_T1, PI_RT_COMPARE_A);
    emit_i(p, OPC_BNE, R_T1, R_T0, 0);
    patch_branch(p, p->pos - 4, poll);
    emit_nop(p);

    to_partner = emit_branch(p, OPC_BNE, R_T3, R_ZERO);

    /* A publisher, late: half a second, then publish. */
    emit_rt_wait(p, PUBLISH_DELAY_TICKS);
    emit_publish(p);
    to_read = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);

    /* A partner: its timed wait, then its read. */
    patch_branch(p, to_partner, p->pos);
    emit_rt_wait(p, PARTNER_WAIT_TICKS);

    patch_branch(p, to_read, p->pos);
    emit_record(p);
}

/*
 * The liveness program: the handshake program with the PROM's bound on the
 * pair liveness poll. Each processor reads its partner's register a fixed
 * number of times rather than forever, records in STATE_LIVE whether the
 * partner's post arrived, and in STATE_WAITED how many real-time counter
 * ticks the poll took from its own post. B posts late, or with
 * @b_posts false never posts at all. On the PROM a poller that gives up
 * clears its partner's CPU enable, and the late processor is lost ("CPU
 * failed early init"). The poll counts reads rather than timing on the
 * real-time counter, so the partner park never sees it.
 */
static void emit_liveness_program(Image *p, bool b_posts)
{
    unsigned int to_post, to_poll, poll, to_seen, to_after, to_partner = 0;
    unsigned int to_read, to_publish = 0;
    unsigned int to_not_master;

    emit_prologue(p);

    /*
     * The master's progress code: a write to its own compare register
     * after the barrier's release and before its join, which is not a post.
     */
    to_not_master = emit_branch(p, OPC_BNE, R_T2, R_ZERO);
    emit_i(p, OPC_ORI, R_ZERO, R_T0, PROGRESS_CODE);
    emit_i(p, OPC_SD, R_S0, R_T0, PI_RT_COMPARE_A);
    patch_branch(p, to_not_master, p->pos);
    emit_i(p, OPC_SD, R_S2, R_A2, 0);               /* past the join */

    emit_i(p, OPC_ANDI, R_T2, R_T3, 1);             /* t3 = slot */
    emit_dsll(p, R_T4, R_T3, 3);
    emit_daddu(p, R_S7, R_S0, R_T4);
    emit_i(p, OPC_XORI, R_T3, R_T4, 1);             /* the partner's slot */
    emit_dsll(p, R_T4, R_T4, 3);
    emit_daddu(p, R_FP, R_S0, R_T4);

    /* B alone (processor number 1) posts late, or not at all. */
    emit_i(p, OPC_XORI, R_T2, R_T4, 1);             /* zero only for B */
    to_post = emit_branch(p, OPC_BNE, R_T4, R_ZERO);
    if (b_posts) {
        unsigned int wait_for_partner_post;

        /*
         * Start B's delay only after A sampled the counter and posted. This
         * keeps B's host scheduling before A's post out of A's measurement.
         */
        emit_li32(p, R_T0, POST_VALUE);
        wait_for_partner_post = p->pos;
        emit_i(p, OPC_LD, R_FP, R_T1, PI_RT_COMPARE_A);
        emit_i(p, OPC_BNE, R_T1, R_T0, 0);
        patch_branch(p, p->pos - 4, wait_for_partner_post);
        emit_nop(p);
        emit_rt_wait(p, LATE_POST_TICKS);
    } else {
        to_partner = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);
    }
    patch_branch(p, to_post, p->pos);

    /* Post, then read the partner's register at most LIVENESS_TRIES times. */
    emit_i(p, OPC_LD, R_S6, R_A0, 0);               /* when it posted */
    emit_i(p, OPC_ORI, R_ZERO, R_T0, POST_VALUE);
    emit_i(p, OPC_SD, R_S7, R_T0, PI_RT_COMPARE_A);
    emit_li32(p, R_T5, LIVENESS_TRIES);
    poll = p->pos;
    emit_i(p, OPC_LD, R_FP, R_T1, PI_RT_COMPARE_A);
    to_seen = emit_branch(p, OPC_BEQ, R_T1, R_T0);
    emit_i(p, OPC_DADDIU, R_T5, R_T5, 0xffff);      /* one try fewer */
    emit_i(p, OPC_BNE, R_T5, R_ZERO, 0);
    patch_branch(p, p->pos - 4, poll);
    emit_nop(p);
    to_after = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO); /* gave up */
    patch_branch(p, to_seen, p->pos);
    emit_i(p, OPC_ORI, R_ZERO, R_T1, 1);
    emit_i(p, OPC_SD, R_S1, R_T1, STATE_LIVE);
    patch_branch(p, to_after, p->pos);
    emit_i(p, OPC_LD, R_S6, R_A1, 0);
    emit_dsubu(p, R_A1, R_A1, R_A0);
    emit_i(p, OPC_SD, R_S1, R_A1, STATE_WAITED);

    /*
     * Then as the handshake program: late publishers, timed partners. With
     * a silent partner, A has already been held for the park's bound, so it
     * publishes at once; C's publisher park, which waits for A's
     * publication, then ends satisfied rather than at its own bound.
     */
    to_poll = emit_branch(p, OPC_BNE, R_T3, R_ZERO);
    if (!b_posts) {
        to_publish = emit_branch(p, OPC_BEQ, R_T2, R_ZERO);
    }
    emit_rt_wait(p, PUBLISH_DELAY_TICKS);
    if (!b_posts) {
        patch_branch(p, to_publish, p->pos);
    }
    emit_publish(p);
    to_read = emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, to_poll, p->pos);
    if (!b_posts) {
        patch_branch(p, to_partner, p->pos);
    }
    emit_rt_wait(p, PARTNER_WAIT_TICKS);
    patch_branch(p, to_read, p->pos);
    emit_record(p);
}

static void build_liveness_program(Image *p)
{
    emit_liveness_program(p, true);
}

static void build_silent_partner_program(Image *p)
{
    emit_liveness_program(p, false);
}

static void write_exact(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        g_assert_cmpint(n, >, 0);
        sent += n;
    }
}

static char *write_image(void (*build)(Image *p))
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    Image image = { .bytes = bytes };
    char *path = NULL;
    int fd;

    build(&image);
    fd = g_file_open_tmp("sn1-presence-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_exact(fd, bytes, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint64_t state_word(QTestState *s, unsigned int number,
                           unsigned int offset)
{
    return qtest_readq(s, STATE_HOST + number * STATE_SLOT_STRIDE + offset);
}

/*
 * The ways TCG can schedule the four processors. The join has to hold in
 * each: multi-threaded, where a processor's host thread can be left
 * unscheduled; round robin, where one host thread takes the processors in
 * turn; and instruction counting, where round robin also drives the
 * virtual clock the real-time counter follows, so the counter only
 * advances while some processor executes.
 */
static const char accel_mttcg[] = "-accel tcg,thread=multi";
static const char accel_rr[] = "-accel tcg,thread=single";
static const char accel_icount[] =
    "-accel tcg,thread=single -icount shift=5,align=off,sleep=off";

/*
 * Boot the four processors on @build's program under @accel, wait for
 * every one to record its first presence read, and hand back what each
 * read, by processor number. Both interfaces must have published and the
 * hub lock must have come back free, whatever the reads were.
 */
static void run_program(void (*build)(Image *p), const char *accel,
                        uint64_t seen[4], uint64_t *live,
                        uint64_t *waited)
{
    static const char names[4] = { 'A', 'B', 'C', 'D' };
    g_autofree char *prom = NULL;
    g_autofree char *quoted = NULL;
    QTestState *s;
    uint64_t done[4] = { 0, 0, 0, 0 };

    prom = write_image(build);
    quoted = g_shell_quote(prom);
    /*
     * The accelerator named here wins over the -accel qtest libqtest
     * appends: guest code has to run.
     */
    s = qtest_initf("-M origin300 -cpu R14000 -smp 4 -bios %s %s "
                    "-no-shutdown -S", quoted, accel);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        bool all = true;

        for (unsigned int n = 0; n < 4; n++) {
            done[n] = state_word(s, n, STATE_DONE);
            all = all && done[n] == DONE_MAGIC;
        }
        if (all) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    for (unsigned int n = 0; n < 4; n++) {
        seen[n] = state_word(s, n, STATE_SEEN);
        g_test_message("processor %c: first presence read 0x%" PRIx64,
                       names[n], seen[n]);
        if (live) {
            live[n] = state_word(s, n, STATE_LIVE);
            g_test_message("processor %c: saw its partner's post: %" PRIu64,
                           names[n], live[n]);
        }
        if (waited) {
            waited[n] = state_word(s, n, STATE_WAITED);
            g_test_message("processor %c: polled for %" PRIu64 " ticks",
                           names[n], waited[n]);
        }
    }
    for (unsigned int n = 0; n < 4; n++) {
        g_assert_cmphex(done[n], ==, DONE_MAGIC);
    }
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + II_SCRATCH_0), ==,
                    PRESENCE_ALL);
    g_assert_cmphex(qtest_readq(s, BEDROCK_LOCAL + LB_SCRATCH_3), ==, 0);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

static void check_first_presence_read_sees_every_interface(const char *accel)
{
    uint64_t seen[4];

    /* Guest code has to run, so the accelerator has to be a real one. */
    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    run_program(build_program, accel, seen, NULL, NULL);
    /* Neither publisher read the other interface's bits early. */
    g_assert_cmphex(seen[0], ==, PRESENCE_ALL);
    g_assert_cmphex(seen[2], ==, PRESENCE_ALL);
}

static void check_partner_read_sees_its_interface(const char *accel)
{
    uint64_t seen[4];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    run_program(build_handshake_program, accel, seen, NULL, NULL);
    /* Each partner read its own interface's bits, published late. */
    g_assert_cmphex(seen[1] & PRESENCE_PI0, ==, PRESENCE_PI0);
    g_assert_cmphex(seen[3] & PRESENCE_PI1, ==, PRESENCE_PI1);
    /* And the publishers still saw both interfaces. */
    g_assert_cmphex(seen[0], ==, PRESENCE_ALL);
    g_assert_cmphex(seen[2], ==, PRESENCE_ALL);
}

static void check_late_partner_post_survives_liveness_poll(const char *accel)
{
    uint64_t seen[4], live[4], waited[4];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    run_program(build_liveness_program, accel, seen, live, waited);
    /* Every processor saw its partner's post, B's late one included. */
    for (unsigned int n = 0; n < 4; n++) {
        g_assert_cmpuint(live[n], ==, 1);
    }
    /*
     * A posted on time and waited for B: its progress code after the
     * barrier's release did not count as its post and park it until B's.
     */
    g_assert_cmpuint(waited[0], >=, LATE_POST_TICKS / 2);
    /* And A was woken by that post, not by the park's bound. */
    g_assert_cmpuint(waited[0], <, WOKEN_BY_POST_TICKS);
}

static void check_silent_partner_releases_at_bound(const char *accel)
{
    uint64_t seen[4], live[4], waited[4];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    run_program(build_silent_partner_program, accel, seen, live, waited);
    /* A's partner never posted: its park ran to the bound, then it gave up. */
    g_assert_cmpuint(live[0], ==, 0);
    g_assert_cmpuint(waited[0], >=, WOKEN_BY_POST_TICKS);
    /* The other pair's handshake went on as usual. */
    g_assert_cmpuint(live[2], ==, 1);
    g_assert_cmpuint(live[3], ==, 1);
    /*
     * And the join was not abandoned: C's publisher park still waited for
     * A's publication, and D's partner park for C's.
     */
    g_assert_cmphex(seen[2], ==, PRESENCE_ALL);
    g_assert_cmphex(seen[3] & PRESENCE_PI1, ==, PRESENCE_PI1);
}

static void check_release_waits_for_late_first_write(const char *accel)
{
    uint64_t seen[4], waited[4];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    run_program(build_late_join_program, accel, seen, NULL, waited);
    /* A's release did not land until B's late first write had. */
    g_assert_cmpuint(waited[0], >=, LATE_POST_TICKS / 2);
    g_assert_cmpuint(waited[0], <, WOKEN_BY_POST_TICKS);
    /* And the publication join after it still held. */
    g_assert_cmphex(seen[0], ==, PRESENCE_ALL);
    g_assert_cmphex(seen[2], ==, PRESENCE_ALL);
}

static void test_release_waits(void)
{
    check_release_waits_for_late_first_write(accel_mttcg);
}

static void test_release_waits_round_robin(void)
{
    check_release_waits_for_late_first_write(accel_rr);
}

static void test_release_waits_icount(void)
{
    check_release_waits_for_late_first_write(accel_icount);
}

static void test_first_presence_read_sees_every_interface(void)
{
    check_first_presence_read_sees_every_interface(accel_mttcg);
}

static void test_partner_read_sees_its_interface(void)
{
    check_partner_read_sees_its_interface(accel_mttcg);
}

static void test_first_presence_read_round_robin(void)
{
    check_first_presence_read_sees_every_interface(accel_rr);
}

static void test_partner_read_round_robin(void)
{
    check_partner_read_sees_its_interface(accel_rr);
}

static void test_first_presence_read_icount(void)
{
    check_first_presence_read_sees_every_interface(accel_icount);
}

static void test_partner_read_icount(void)
{
    check_partner_read_sees_its_interface(accel_icount);
}

static void test_late_partner_post(void)
{
    check_late_partner_post_survives_liveness_poll(accel_mttcg);
}

static void test_late_partner_post_round_robin(void)
{
    check_late_partner_post_survives_liveness_poll(accel_rr);
}

static void test_late_partner_post_icount(void)
{
    check_late_partner_post_survives_liveness_poll(accel_icount);
}

static void test_silent_partner(void)
{
    check_silent_partner_releases_at_bound(accel_mttcg);
}

static void test_silent_partner_round_robin(void)
{
    check_silent_partner_releases_at_bound(accel_rr);
}

static void test_silent_partner_icount(void)
{
    check_silent_partner_releases_at_bound(accel_icount);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/presence/first-read-sees-every-interface",
                   test_first_presence_read_sees_every_interface);
    qtest_add_func("sn1/presence/partner-read-sees-its-interface",
                   test_partner_read_sees_its_interface);
    qtest_add_func("sn1/presence/round-robin/first-read-sees-every-interface",
                   test_first_presence_read_round_robin);
    qtest_add_func("sn1/presence/round-robin/partner-read-sees-its-interface",
                   test_partner_read_round_robin);
    qtest_add_func("sn1/presence/icount/first-read-sees-every-interface",
                   test_first_presence_read_icount);
    qtest_add_func("sn1/presence/icount/partner-read-sees-its-interface",
                   test_partner_read_icount);
    qtest_add_func("sn1/presence/late-partner-post-survives-liveness-poll",
                   test_late_partner_post);
    qtest_add_func("sn1/presence/round-robin/"
                   "late-partner-post-survives-liveness-poll",
                   test_late_partner_post_round_robin);
    qtest_add_func("sn1/presence/icount/"
                   "late-partner-post-survives-liveness-poll",
                   test_late_partner_post_icount);
    qtest_add_func("sn1/presence/silent-partner-releases-at-bound",
                   test_silent_partner);
    qtest_add_func("sn1/presence/round-robin/"
                   "silent-partner-releases-at-bound",
                   test_silent_partner_round_robin);
    qtest_add_func("sn1/presence/icount/silent-partner-releases-at-bound",
                   test_silent_partner_icount);
    qtest_add_func("sn1/presence/release-waits-for-late-first-write",
                   test_release_waits);
    qtest_add_func("sn1/presence/round-robin/"
                   "release-waits-for-late-first-write",
                   test_release_waits_round_robin);
    qtest_add_func("sn1/presence/icount/release-waits-for-late-first-write",
                   test_release_waits_icount);
    return g_test_run();
}
