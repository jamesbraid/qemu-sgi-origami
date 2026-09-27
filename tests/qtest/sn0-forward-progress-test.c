/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Multi-threaded TCG forward-progress tests. Origin 200 and eight-CPU Origin
 * 2000 IRIX boots lost scheduler interrupts for 10-47 seconds and never
 * reached login despite passing register tests. The line was pending and
 * unmasked while vCPUs queued on the big lock; cpu_handle_interrupt() took
 * that lock every execution-loop pass while the interrupt flag stayed set.
 * Guest-counted loop passes and profiler lock acquisitions are compared per
 * guest work unit; host load changes wall time, so it supplies no bound.
 * A masked pending line costs no lock per pass (mips_cpu_exec_interrupt()
 * drops the flag); the defect costs one acquisition per pass. An unmasked
 * pending line must arrive within bounded passes of its CPU after a guest
 * unmask, Count/Compare match or own Hub raise. Unmask reasserts the local
 * line; the comparator matches immediately.
 * Timer and peer raises have arrival checks only: the guest cannot timestamp
 * their start without taking them. count-wrap tests every query-machines
 * R10000-default machine, failing by name if uncovered: 32-bit Count wraps
 * against unwritten Compare immediately instead of 47.7 s after reset,
 * then tests masked, unmasked, open and matching Count states. steady-state
 * tests an Origin 200 pair: one mtc0 loop with a
 * translation-block end each pass; masked/unmasked Hub line; Hub or memory
 * polling beside peer compute; and self-raised open Hub line. The plain loop
 * verifies reentry; pending-line phases wait for Cause before counting.
 * Atomic claims checked against query-cpus-fast prove both CPUs ran, and
 * atomic completion counts prove both finished each phase.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-test.h"

/*
 * On SN machines, state words sit in plain node memory at physical
 * 0x300000, reached uncached: above the PROM-data window at 0x100000,
 * which the machine
 * filters through an I/O region per processor, and above the image copy
 * other CPU-side tests keep at 0x200000.  A machine whose nodes each see
 * their own low memory has one view per node: the host seeds each view
 * with a token through one of its processors and groups processors by the
 * token they read back.
 */
#define VIEW_PHYS           0x00300000u

#define CLAIMS_OFF          0x000   /* processors that took a slot here */
#define TOKEN_OFF           0x008   /* host-seeded, one value per view */
#define GO_OFF              0x010   /* the phase the host has released */
#define FINISHED_OFF        0x018   /* phases completed, summed over slots */
/* Per-slot words, eight bytes each, up to MAX_SLOTS per view. */
#define ALIVE_OFF           0x040
#define EXC_MARK_OFF        0x080
#define EXC_CAUSE_OFF       0x0c0
#define EXC_EPC_OFF         0x100
#define EXC_VADDR_OFF       0x140
#define EXC_PASSES_OFF      0x180   /* passes from the arm to the handler */
#define EXC_PHASE_OFF       0x1c0
#define CAUSE_SEEN_OFF      0x200

#define MAX_SLOTS           8
#define MAX_CPUS            16

#define ALIVE_MAGIC         0x00a11e00ULL
#define EXC_MAGIC           0x00ecc000ULL
#define FAULT_MAGIC         0x00fa0170ULL

#define HANDLER_BODY        0x1000

/* Passes per counted phase, per processor: the guest's unit of work. */
#define PASSES              100000u

/*
 * Passes a deliverable line may wait.  Delivery is due at the next pass;
 * the bound leaves room for a translation block or two either side.
 */
#define DELIVERY_PASSES     100u

/* Count parks 4096 ticks short of the wrap: tens of microseconds here. */
#define COUNT_PARK          0xfffff000u

#define STATUS_IE           0x0001u
#define STATUS_IM2          0x0400u
#define STATUS_IM7          0x8000u
#define CAUSE_IP2           0x0400u
#define CAUSE_IP7           0x8000u
#define CAUSE_EXCCODE       0x007cu

#define CP0_COUNT           9
#define CP0_COMPARE         11

/* Hub processor-interface registers, through the uncached XKPHYS window. */
#define HUB_LOCAL_BASE      0x9200000001000000ULL
#define PI_INT_MODIFY       0x090
#define PI_INT_MASK0_A      0x0a8
#define PI_INT_MODIFY_SET   0x100u
/* Bank 0 lines 5 and 6: one per slice, so an acknowledge lowers one line. */
#define HUB_LINE_BASE       5

#define OPC_DADDIU          25
#define OPC_LLD             52
#define OPC_SCD             60
#define FN_SYNC             0x0f
#define FN_DSLLV            0x14
#define FN_SLTU             0x2b
#define FN_DADDU            0x2d

#define R_T3                11
#define R_T4                12
#define R_T5                13
#define R_T6                14
#define R_T7                15
#define R_S3                19
#define R_S4                20
#define R_S5                21
#define R_S6                22
#define R_S7                23

#define POLL_INTERVAL_US    5000
#define POLL_TRIES          6000

typedef enum {
    PROGRAM_COUNT_WRAP,
    PROGRAM_STEADY_STATE,
} Program;

typedef struct {
    const char *machine;
    const char *args;       /* what it needs besides -bios and -accel */
    size_t image_size;      /* the only size the machine accepts, or 0 */
    void (*prepare)(QTestState *qts);
    uint32_t view_phys;     /* 0 uses VIEW_PHYS */
} Recipe;

#define ORIGIN300_BEDROCK_MD_MEMORY_CONFIG UINT64_C(0x20001f80008)
#define ORIGIN300_MD_INSTALLED_SELECT2 \
    ((UINT64_C(2) << 32) | UINT64_C(0x3300))

static void prepare_origin300(QTestState *qts)
{
    /* Map the installed banks over the synthetic guest's low-memory state. */
    qtest_writeq(qts, ORIGIN300_BEDROCK_MD_MEMORY_CONFIG,
                 ORIGIN300_MD_INSTALLED_SELECT2);
}

/* Test-owned R12000 configuration, as in octane-test.c. */
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

static void prepare_fuel(QTestState *qts)
{
    qtest_writeq(qts, ORIGIN300_BEDROCK_MD_MEMORY_CONFIG, 0x33);
}

/*
 * How each machine takes a raw image at the reset vector.  A machine
 * query-machines reports with a family processor and this table lacks is
 * registered to fail by name.  The Origin 200 gets the two processors the
 * project boots it with; the others fix their own processor count.
 */
static const Recipe recipes[] = {
    { "origin200", "-M origin200 -m 128M -smp 2", 0 },
    { "origin2000", "-M origin2000", 0 },
    { "onyx2", "-M onyx2", 0 },
    { "origin300", "-M origin300", 0x1686a8, prepare_origin300 },
    /* IP30 has only 16 KiB of low RAM; the SN state address is unmapped. */
    { "octane", "-M octane -m 128M -cpu R10000", 0, NULL, 0x3000 },
    { "octane2", "-M octane2 -m 256M -cpu " OCTANE2_CPU,
      0, NULL, 0x3000 },
    { "fuel", "-M fuel,fuel-board-id-word=0x4abc,"
              "fuel-ioc3-subsystem-id=0xc30a", 0x1686a8, prepare_fuel },
};

static const char *const r10000_family[] = {
    "R10000-mips64-cpu", "R12000-mips64-cpu", "R14000-mips64-cpu",
    "R16000-mips64-cpu",
};

/* ---------------------------------------------------------------- */
/* Guest program                                                     */
/* ---------------------------------------------------------------- */

/*
 * Registers held across every program: t0 = this view's base, s5 = the
 * slot this processor claimed, t2 = t0 + 8 * s5, s0 = Status with IE set
 * and every line masked, s1 = Hub slice and s2 = Hub base where a program
 * uses the Hub.  s3 counts passes from an arm to its delivery, s4 is set
 * by the handler on delivery, s6 holds the Status a phase runs under and
 * s7 the phase number the handler records.  The handler owns k0 and k1.
 */

static void emit_r(SN0Image *p, unsigned int fn, unsigned int rd,
                   unsigned int rs, unsigned int rt)
{
    sn0_emit(p, (rs << 21) | (rt << 16) | (rd << 11) | fn);
}

static void emit_sync(SN0Image *p)
{
    sn0_emit(p, FN_SYNC);
}

static void emit_ld(SN0Image *p, unsigned int rt, unsigned int base,
                    uint16_t off)
{
    sn0_emit_i(p, SN0_OPC_LD, base, rt, off);
}

static void emit_sd(SN0Image *p, unsigned int rt, unsigned int base,
                    uint16_t off)
{
    sn0_emit_i(p, SN0_OPC_SD, base, rt, off);
}

static void emit_branch_back(SN0Image *p, unsigned int op, unsigned int rs,
                             unsigned int rt, unsigned int target)
{
    int delta = ((int)target - (int)(p->pos + 4)) / 4;

    g_assert_cmpint(delta, <, 0);
    g_assert_cmpint(delta, >=, INT16_MIN);
    sn0_emit_i(p, op, rs, rt, (uint16_t)delta);
    sn0_emit_nop(p);
}

static void emit_jump_to(SN0Image *p, unsigned int target)
{
    unsigned int at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);

    sn0_patch_branch(p, at, target);
}

/* Publish a per-slot word: t2 already carries this slot's offset. */
static void emit_publish(SN0Image *p, unsigned int rt, uint16_t off)
{
    emit_sd(p, rt, SN0_R_T2, off);
    emit_sync(p);
}

/* Atomically add one to a view-wide word; t3 receives the old value. */
static void emit_fetch_add(SN0Image *p, uint16_t off)
{
    unsigned int retry = p->pos;

    sn0_emit_i(p, OPC_LLD, SN0_R_T0, R_T3, off);
    sn0_emit_i(p, OPC_DADDIU, R_T3, R_T4, 1);
    sn0_emit_i(p, OPC_SCD, SN0_R_T0, R_T4, off);
    emit_branch_back(p, SN0_OPC_BEQ, R_T4, SN0_R_ZERO, retry);
    emit_sync(p);
}

/* Spin until the host has released phase @n, and record the phase. */
static void emit_wait_go(SN0Image *p, unsigned int n)
{
    unsigned int spin;

    sn0_emit_li(p, R_T3, n);
    spin = p->pos;
    emit_ld(p, SN0_R_T8, SN0_R_T0, GO_OFF);
    emit_r(p, FN_SLTU, SN0_R_T9, SN0_R_T8, R_T3);
    emit_branch_back(p, SN0_OPC_BNE, SN0_R_T9, SN0_R_ZERO, spin);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, R_S7, n);
}

static void emit_finished(SN0Image *p)
{
    emit_fetch_add(p, FINISHED_OFF);
}

/*
 * The unit of work: PASSES writes of @status to Status.  Each ends its
 * translation block, so each is one pass through the execution loop.
 */
static void emit_passes(SN0Image *p, unsigned int status)
{
    unsigned int loop;

    sn0_emit_li(p, R_T7, PASSES);
    loop = p->pos;
    sn0_emit_mtc0(p, status, SN0_CP0_STATUS);
    sn0_emit_i(p, SN0_OPC_ADDIU, R_T7, R_T7, 0xffff);
    emit_branch_back(p, SN0_OPC_BNE, R_T7, SN0_R_ZERO, loop);
}

/* PASSES loads from @off(base): the poller's unit of work. */
static void emit_poll_loads(SN0Image *p, unsigned int base, uint16_t off)
{
    unsigned int loop;

    sn0_emit_li(p, R_T7, PASSES);
    loop = p->pos;
    emit_ld(p, SN0_R_T8, base, off);
    sn0_emit_i(p, SN0_OPC_ADDIU, R_T7, R_T7, 0xffff);
    emit_branch_back(p, SN0_OPC_BNE, R_T7, SN0_R_ZERO, loop);
}

/* Spin until Cause shows @ip, then record the Cause that showed it. */
static void emit_wait_line(SN0Image *p, unsigned int ip)
{
    unsigned int spin = p->pos;

    sn0_emit_mfc0(p, R_T6, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_ANDI, R_T6, R_T7, ip);
    emit_branch_back(p, SN0_OPC_BEQ, R_T7, SN0_R_ZERO, spin);
    emit_publish(p, R_T6, CAUSE_SEEN_OFF);
}

/*
 * Arm a line with @arm, then pass through the execution loop under
 * @status until the handler reports delivery.  The handler reads s3 as
 * the passes the line waited, so the arm is inside the count.
 */
static void emit_deliver_wait(SN0Image *p, unsigned int status,
                              void (*arm)(SN0Image *p))
{
    unsigned int loop;

    sn0_emit_or(p, R_S3, SN0_R_ZERO, SN0_R_ZERO);
    sn0_emit_or(p, R_S4, SN0_R_ZERO, SN0_R_ZERO);
    arm(p);
    loop = p->pos;
    sn0_emit_mtc0(p, status, SN0_CP0_STATUS);
    sn0_emit_i(p, OPC_DADDIU, R_S3, R_S3, 1);
    emit_branch_back(p, SN0_OPC_BEQ, R_S4, SN0_R_ZERO, loop);
}

static void arm_unmask(SN0Image *p)
{
    sn0_emit_mtc0(p, R_S6, SN0_CP0_STATUS);
}

static void arm_count_park(SN0Image *p)
{
    sn0_emit_li(p, R_T5, COUNT_PARK);
    sn0_emit_mtc0(p, R_T5, CP0_COUNT);
}

/*
 * The wrap again after an acknowledge has moved Compare: put Compare back
 * at its reset value first, then park Count. Compare is already settled
 * by the time the Count write runs. That write reschedules using the store's
 * literal Count argument against the Compare field, with no fresh clock
 * sample. It needs no later instruction to land on time.
 *
 * The other order raced the wrap. Parking Count first leaves only
 * COUNT_PARK's fixed 4096-tick gap for the Compare write that follows to
 * land before Count crosses zero; that write is a second instruction under
 * the same big lock seven other vCPUs are contending for, easily delayed
 * past the gap under load. Missing it used to cost nothing: the
 * half-counter window still called a Compare landing just past the wrap a
 * match. Equality-only matching has no such margin, so a late write there
 * now waits out a full wrap instead of the few ticks this phase intends.
 */
static void arm_count_park_compare_reset(SN0Image *p)
{
    sn0_emit_mtc0(p, SN0_R_ZERO, CP0_COMPARE);
    arm_count_park(p);
}

/*
 * Count written equal to the standing Compare matches at once. Reading
 * Count and writing it into Compare would race the counter instead,
 * Count keeps advancing between the read and the write, so by the time
 * the write lands it is normally already behind, and the comparator is
 * equality only. Arming from the other side is deterministic: Compare
 * only changes when this code changes it, so the value read back is
 * exactly the value the Count write compares against.
 */
static void arm_count_from_compare(SN0Image *p)
{
    sn0_emit_mfc0(p, R_T5, CP0_COMPARE);
    sn0_emit_mtc0(p, R_T5, CP0_COUNT);
}

/* Raise this slice's own bank 0 line. */
static void arm_hub_line(SN0Image *p)
{
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S1, R_T5,
               PI_INT_MODIFY_SET | HUB_LINE_BASE);
    emit_sd(p, R_T5, SN0_R_S2, PI_INT_MODIFY);
}

/*
 * The handler on every boot-exception vector.  An interrupt is recorded
 * for this slot with the passes it waited, acknowledged at its source, and
 * returned from with s4 set; k0 and k1 are the only registers it touches
 * besides s4.  Anything else is recorded and parked on, so a fault stays
 * visible instead of looking like a hang.
 */
static void emit_exception_handler(SN0Image *p)
{
    unsigned int fault, no_timer, done;

    sn0_emit_mfc0(p, SN0_R_K0, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_K0, SN0_R_K1, CAUSE_EXCCODE);
    fault = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_K1, SN0_R_ZERO);

    emit_sd(p, SN0_R_K0, SN0_R_T2, EXC_CAUSE_OFF);
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_EPC);
    emit_sd(p, SN0_R_K1, SN0_R_T2, EXC_EPC_OFF);
    emit_sd(p, R_S3, SN0_R_T2, EXC_PASSES_OFF);
    emit_sd(p, R_S7, SN0_R_T2, EXC_PHASE_OFF);
    sn0_emit_li(p, SN0_R_K1, EXC_MAGIC);
    emit_sd(p, SN0_R_K1, SN0_R_T2, EXC_MARK_OFF);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, R_S4, 1);

    /* The timer: a Compare a quarter-wrap ahead lowers IP7. */
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_K0, SN0_R_K1, CAUSE_IP7);
    no_timer = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_K1, SN0_R_ZERO);
    sn0_emit_mfc0(p, SN0_R_K1, CP0_COUNT);
    sn0_emit_i(p, SN0_OPC_LUI, SN0_R_ZERO, SN0_R_K0, 0x4000);
    sn0_emit_addu(p, SN0_R_K1, SN0_R_K1, SN0_R_K0);
    sn0_emit_mtc0(p, SN0_R_K1, CP0_COMPARE);
    sn0_emit_mfc0(p, SN0_R_K0, SN0_CP0_CAUSE);
    sn0_patch_branch(p, no_timer, p->pos);

    /* The Hub: clearing this slice's line lowers IP2. */
    sn0_emit_i(p, SN0_OPC_ANDI, SN0_R_K0, SN0_R_K1, CAUSE_IP2);
    done = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_K1, SN0_R_ZERO);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S1, SN0_R_K1, HUB_LINE_BASE);
    emit_sd(p, SN0_R_K1, SN0_R_S2, PI_INT_MODIFY);
    sn0_patch_branch(p, done, p->pos);
    emit_sync(p);
    sn0_emit_eret(p);

    sn0_patch_branch(p, fault, p->pos);
    emit_sd(p, SN0_R_K0, SN0_R_T2, EXC_CAUSE_OFF);
    sn0_emit_mfc0(p, SN0_R_K0, SN0_CP0_BADVADDR);
    emit_sd(p, SN0_R_K0, SN0_R_T2, EXC_VADDR_OFF);
    sn0_emit_mfc0(p, SN0_R_K0, SN0_CP0_EPC);
    emit_sd(p, SN0_R_K0, SN0_R_T2, EXC_EPC_OFF);
    emit_sd(p, R_S7, SN0_R_T2, EXC_PHASE_OFF);
    sn0_emit_li(p, SN0_R_K0, FAULT_MAGIC);
    emit_sd(p, SN0_R_K0, SN0_R_T2, EXC_MARK_OFF);
    emit_sync(p);
    sn0_emit_park(p);
}

static void emit_preamble(SN0Image *p, uint32_t view_phys)
{
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_T0, SN0_KSEG1(view_phys));

    emit_fetch_add(p, CLAIMS_OFF);
    sn0_emit_or(p, R_S5, R_T3, SN0_R_ZERO);
    sn0_emit_sll(p, SN0_R_T1, R_S5, 3);
    sn0_emit_addu(p, SN0_R_T2, SN0_R_T0, SN0_R_T1);

    sn0_emit_li(p, SN0_R_S0, SN0_STATUS_KERNEL | STATUS_IE);
    sn0_emit_mtc0(p, SN0_R_S0, SN0_CP0_STATUS);
    sn0_emit_or(p, SN0_R_S1, SN0_R_ZERO, SN0_R_ZERO);
    sn0_emit_or(p, SN0_R_S2, SN0_R_ZERO, SN0_R_ZERO);

    sn0_emit_li(p, R_T3, ALIVE_MAGIC);
    emit_publish(p, R_T3, ALIVE_OFF);
}

static void build_count_wrap(SN0Image *p)
{
    /* 1: the loop with nothing pending. */
    emit_wait_go(p, 1);
    emit_passes(p, SN0_R_S0);
    emit_finished(p);

    /* 2: Count wraps past the reset Compare while IM7 is masked. */
    emit_wait_go(p, 2);
    arm_count_park(p);
    emit_wait_line(p, CAUSE_IP7);
    emit_passes(p, SN0_R_S0);
    emit_finished(p);

    /* 3: unmasking must deliver the line the loop carried. */
    emit_wait_go(p, 3);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_S0, R_S6, STATUS_IM7);
    emit_deliver_wait(p, R_S6, arm_unmask);
    emit_finished(p);

    /* 4: the wrap again with IM7 open; arrival only. */
    emit_wait_go(p, 4);
    emit_deliver_wait(p, R_S6, arm_count_park_compare_reset);
    emit_finished(p);

    /* 5: Count written to match a standing Compare matches at once. */
    emit_wait_go(p, 5);
    emit_deliver_wait(p, R_S6, arm_count_from_compare);
    emit_finished(p);

    sn0_emit_park(p);
}

static void build_steady_state(SN0Image *p)
{
    unsigned int worker, joined;

    /* s1 = this processor's Hub slice, s2 = its Hub's local registers. */
    sn0_emit_li64(p, SN0_R_T8, SN0_PI_CPU_NUM);
    emit_ld(p, SN0_R_S1, SN0_R_T8, 0);
    sn0_emit_li64(p, SN0_R_S2, HUB_LOCAL_BASE);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_S0, R_S6, STATUS_IM2);

    /* 1: compute. */
    emit_wait_go(p, 1);
    emit_passes(p, SN0_R_S0);
    emit_finished(p);

    /* 2: this slice's line, masked into it and raised, stands masked. */
    emit_wait_go(p, 2);
    sn0_emit_sll(p, R_T4, SN0_R_S1, 4);
    emit_r(p, FN_DADDU, R_T3, SN0_R_S2, R_T4);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S1, R_T5, HUB_LINE_BASE);
    sn0_emit_i(p, SN0_OPC_ORI, SN0_R_ZERO, R_T6, 1);
    emit_r(p, FN_DSLLV, R_T6, R_T5, R_T6);
    emit_sd(p, R_T6, R_T3, PI_INT_MASK0_A);
    arm_hub_line(p);
    emit_wait_line(p, CAUSE_IP2);
    emit_passes(p, SN0_R_S0);
    emit_finished(p);

    /* 3: unmasking must deliver it. */
    emit_wait_go(p, 3);
    emit_deliver_wait(p, R_S6, arm_unmask);
    emit_finished(p);

    /* 4: slot 0 polls the pending bank, slot 1 computes. */
    emit_wait_go(p, 4);
    worker = sn0_emit_branch(p, SN0_OPC_BNE, R_S5, SN0_R_ZERO);
    emit_poll_loads(p, SN0_R_S2, PI_INT_PEND0);
    joined = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, worker, p->pos);
    emit_passes(p, SN0_R_S0);
    sn0_patch_branch(p, joined, p->pos);
    emit_finished(p);

    /* 5: slot 0 polls a word of memory, slot 1 computes. */
    emit_wait_go(p, 5);
    worker = sn0_emit_branch(p, SN0_OPC_BNE, R_S5, SN0_R_ZERO);
    emit_poll_loads(p, SN0_R_T0, TOKEN_OFF);
    joined = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, worker, p->pos);
    emit_passes(p, SN0_R_S0);
    sn0_patch_branch(p, joined, p->pos);
    emit_finished(p);

    /* 6: raising this slice's line while open delivers it at once. */
    emit_wait_go(p, 6);
    emit_deliver_wait(p, R_S6, arm_hub_line);
    emit_finished(p);

    sn0_emit_park(p);
}

static void build_image(SN0Image *p, Program program, uint32_t view_phys)
{
    unsigned int vector;

    emit_jump_to(p, CODE_START);
    for (vector = SN0_VEC_TLB_REFILL; vector <= SN0_VEC_GENERAL;
         vector += 0x80) {
        p->pos = vector;
        emit_jump_to(p, HANDLER_BODY);
    }

    p->pos = CODE_START;
    emit_preamble(p, view_phys);
    if (program == PROGRAM_COUNT_WRAP) {
        build_count_wrap(p);
    } else {
        build_steady_state(p);
    }
    g_assert_cmpuint(p->pos, <=, HANDLER_BODY);

    p->pos = HANDLER_BODY;
    emit_exception_handler(p);
}

/* The image as built, or padded to the one size a machine accepts. */
static char *write_image(const SN0Image *p, size_t size)
{
    g_autofree uint8_t *padded = NULL;
    char *path = NULL;
    int fd;

    if (!size) {
        return sn0_image_write(p, "sn0-forward-progress");
    }
    g_assert_cmpuint(size, >=, sizeof(p->bytes));
    padded = g_malloc0(size);
    memcpy(padded, p->bytes, sizeof(p->bytes));
    fd = g_file_open_tmp("sn0-forward-progress-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, padded, size), ==, size);
    close(fd);
    return path;
}

/* ---------------------------------------------------------------- */
/* Host side                                                         */
/* ---------------------------------------------------------------- */

typedef struct {
    QTestState *qts;
    unsigned int cpus;
    uint32_t view_phys;
    unsigned int views;
    unsigned int view_cpu[MAX_CPUS];    /* one CPU sees each view */
    unsigned int view_slots[MAX_CPUS];  /* processors that claimed a slot */
} Run;

static uint64_t cpu_read_word(Run *r, unsigned int cpu, unsigned int off)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(r->qts, cpu, r->view_phys + off, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void cpu_write_word(Run *r, unsigned int cpu, unsigned int off,
                           uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(r->qts, cpu, r->view_phys + off, bytes,
                                     sizeof(bytes)));
}

static uint64_t view_word(Run *r, unsigned int view, unsigned int off)
{
    return cpu_read_word(r, r->view_cpu[view], off);
}

static uint64_t slot_word(Run *r, unsigned int view, unsigned int slot,
                          unsigned int off)
{
    return view_word(r, view, off + 8 * slot);
}

static unsigned int count_cpus(QTestState *qts)
{
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    unsigned int n = qlist_size(qdict_get_qlist(response, "return"));

    qobject_unref(response);
    return n;
}

/*
 * Big-lock acquisitions the profiler has counted since its last reset,
 * with the call sites behind them when running verbosely.
 */
static uint64_t bql_acquisitions(QTestState *qts)
{
    g_autofree char *report = qtest_hmp(qts, "info sync-profile 200");
    g_auto(GStrv) lines = g_strsplit(report, "\n", -1);
    g_autoptr(GString) sites = g_string_new(NULL);
    uint64_t total = 0;

    for (unsigned int i = 0; lines[i]; i++) {
        g_auto(GStrv) split = g_strsplit_set(lines[i], " \t", -1);
        const char *field[8];
        unsigned int n = 0;

        for (unsigned int j = 0; split[j] && n < G_N_ELEMENTS(field); j++) {
            if (split[j][0]) {
                field[n++] = split[j];
            }
        }
        /* "BQL mutex  <object>  <call site>  <wait>  <count>  <mean>" */
        if (n == 7 && g_str_equal(field[0], "BQL") &&
            g_str_equal(field[1], "mutex")) {
            total += g_ascii_strtoull(field[5], NULL, 10);
            g_string_append_printf(sites, " %s=%s", field[3], field[5]);
        }
    }
    if (g_test_verbose()) {
        g_test_message("big-lock acquisitions by call site:%s", sites->str);
    }
    return total;
}

/*
 * Seed every processor's view with a token and group the processors by the
 * token they read back.  Processors sharing memory share a view; the last
 * writer's token names it.
 */
static void discover_views(Run *r)
{
    unsigned int cpu, view;

    for (cpu = 0; cpu < r->cpus; cpu++) {
        cpu_write_word(r, cpu, TOKEN_OFF, cpu + 1);
    }
    r->views = 0;
    for (cpu = 0; cpu < r->cpus; cpu++) {
        uint64_t token = cpu_read_word(r, cpu, TOKEN_OFF);

        for (view = 0; view < r->views; view++) {
            if (view_word(r, view, TOKEN_OFF) == token) {
                break;
            }
        }
        if (view == r->views) {
            r->view_cpu[r->views++] = cpu;
        }
    }
}

static uint64_t sum_over_views(Run *r, unsigned int off)
{
    uint64_t total = 0;
    unsigned int view;

    for (view = 0; view < r->views; view++) {
        total += view_word(r, view, off);
    }
    return total;
}

static bool wait_for_sum(Run *r, unsigned int off, uint64_t want)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (sum_over_views(r, off) >= want) {
            return true;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    return false;
}

static bool wait_for_slot_word(Run *r, unsigned int view, unsigned int slot,
                               unsigned int off, uint64_t want)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (slot_word(r, view, slot, off) == want) {
            return true;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    return false;
}

static void report(Run *r)
{
    unsigned int view, slot;

    g_test_message("%u processors, %u memory views", r->cpus, r->views);
    for (view = 0; view < r->views; view++) {
        g_test_message("view %u through cpu %u: claims %" PRIu64
                       " finished %" PRIu64, view, r->view_cpu[view],
                       view_word(r, view, CLAIMS_OFF),
                       view_word(r, view, FINISHED_OFF));
        for (slot = 0; slot < r->view_slots[view]; slot++) {
            g_test_message("  slot %u: alive 0x%" PRIx64 " cause-seen 0x%"
                           PRIx64 " exc 0x%" PRIx64 " phase %" PRIu64
                           " cause 0x%" PRIx64 " badvaddr 0x%" PRIx64
                           " epc 0x%" PRIx64 " passes %" PRIu64, slot,
                           slot_word(r, view, slot, ALIVE_OFF),
                           slot_word(r, view, slot, CAUSE_SEEN_OFF),
                           slot_word(r, view, slot, EXC_MARK_OFF),
                           slot_word(r, view, slot, EXC_PHASE_OFF),
                           slot_word(r, view, slot, EXC_CAUSE_OFF),
                           slot_word(r, view, slot, EXC_VADDR_OFF),
                           slot_word(r, view, slot, EXC_EPC_OFF),
                           slot_word(r, view, slot, EXC_PASSES_OFF));
        }
    }
}

/* Every processor has claimed a slot and reported alive. */
static void wait_for_alive(Run *r)
{
    unsigned int view, slot;
    bool ok;

    ok = wait_for_sum(r, CLAIMS_OFF, r->cpus);
    for (view = 0; view < r->views; view++) {
        r->view_slots[view] = MIN(view_word(r, view, CLAIMS_OFF), MAX_SLOTS);
    }
    for (view = 0; ok && view < r->views; view++) {
        for (slot = 0; slot < r->view_slots[view]; slot++) {
            ok &= wait_for_slot_word(r, view, slot, ALIVE_OFF, ALIVE_MAGIC);
        }
    }
    if (!ok) {
        report(r);
    }
    g_assert_true(ok);
    g_assert_cmpuint(sum_over_views(r, CLAIMS_OFF), ==, r->cpus);
}

/*
 * Release phase @go and count big-lock acquisitions until every processor
 * has finished it.  @go is also the number of phases each processor has
 * then completed.
 */
static uint64_t run_phase(Run *r, unsigned int go)
{
    unsigned int view;
    uint64_t locks;
    bool done;

    g_free(qtest_hmp(r->qts, "sync-profile reset"));
    for (view = 0; view < r->views; view++) {
        cpu_write_word(r, r->view_cpu[view], GO_OFF, go);
    }
    done = wait_for_sum(r, FINISHED_OFF, (uint64_t)r->cpus * go);
    locks = bql_acquisitions(r->qts);
    if (!done) {
        g_test_message("phase %u did not finish", go);
        report(r);
    }
    g_assert_true(done);
    return locks;
}

/* Every slot's recorded Cause shows @ip: the line pended before counting. */
static void assert_line_seen(Run *r, unsigned int ip)
{
    unsigned int view, slot;

    for (view = 0; view < r->views; view++) {
        for (slot = 0; slot < r->view_slots[view]; slot++) {
            g_assert_cmphex(slot_word(r, view, slot, CAUSE_SEEN_OFF) & ip,
                            ==, ip);
        }
    }
}

/*
 * Every slot took the interrupt of phase @go on line @ip.  With a bound,
 * within that many of its own passes; without one, at all.  Clears *ok on
 * any miss and returns the most passes any slot waited, so a case can
 * print every figure before it fails.
 */
static uint64_t check_delivery(Run *r, unsigned int go, unsigned int ip,
                               uint64_t bound, bool *ok)
{
    unsigned int view, slot;
    uint64_t worst = 0;

    for (view = 0; view < r->views; view++) {
        for (slot = 0; slot < r->view_slots[view]; slot++) {
            uint64_t cause = slot_word(r, view, slot, EXC_CAUSE_OFF);
            uint64_t passes = slot_word(r, view, slot, EXC_PASSES_OFF);
            bool taken;

            taken = slot_word(r, view, slot, EXC_MARK_OFF) == EXC_MAGIC &&
                    slot_word(r, view, slot, EXC_PHASE_OFF) == go &&
                    (cause & ip) == ip && (cause & CAUSE_EXCCODE) == 0 &&
                    (!bound || passes <= bound);
            if (!taken) {
                g_test_message("phase %u: view %u slot %u did not take its "
                               "interrupt as required", go, view, slot);
                report(r);
                *ok = false;
            }
            worst = MAX(worst, passes);
        }
    }
    return worst;
}

static void boot(Run *r, const Recipe *recipe, Program program)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;

    r->view_phys = recipe->view_phys ? recipe->view_phys : VIEW_PHYS;
    build_image(image, program, r->view_phys);
    path = write_image(image, recipe->image_size);
    r->qts = qtest_initf("%s -bios %s -accel tcg,thread=multi -S",
                         recipe->args, path);
    unlink(path);

    r->cpus = count_cpus(r->qts);
    g_assert_cmpuint(r->cpus, >=, 1);
    g_assert_cmpuint(r->cpus, <=, MAX_CPUS);
    if (recipe->prepare) {
        recipe->prepare(r->qts);
    }
    discover_views(r);
    g_free(qtest_hmp(r->qts, "sync-profile on"));
    qtest_qmp_assert_success(r->qts, "{ 'execute': 'cont' }");
    wait_for_alive(r);
}

/*
 * The budget for a phase whose work should never reach the lock: a tenth
 * of the passes.  The harness's own polling and the timers land inside it;
 * a model paying one acquisition per pass lands ten times outside it.
 */
static uint64_t off_the_lock_budget(Run *r)
{
    return (uint64_t)r->cpus * PASSES / 10;
}

static void test_count_wrap(const void *data)
{
    const Recipe *recipe = data;
    uint64_t clear, masked, unmask, open, behind;
    uint64_t waited_unmask, waited_open, waited_behind;
    bool ok = true;
    Run r = { };

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    if (!recipe->args) {
        g_test_message("%s offers an R10000-family processor and has no "
                       "launch recipe here; add one so this check runs "
                       "there", recipe->machine);
        g_test_fail();
        return;
    }

    boot(&r, recipe, PROGRAM_COUNT_WRAP);
    clear = run_phase(&r, 1);
    masked = run_phase(&r, 2);
    unmask = run_phase(&r, 3);
    waited_unmask = check_delivery(&r, 3, CAUSE_IP7, DELIVERY_PASSES, &ok);
    open = run_phase(&r, 4);
    waited_open = check_delivery(&r, 4, CAUSE_IP7, 0, &ok);
    behind = run_phase(&r, 5);
    waited_behind = check_delivery(&r, 5, CAUSE_IP7, DELIVERY_PASSES, &ok);
    g_test_message("%s: %u processors in %u views, %u passes each: "
                   "big-lock acquisitions %" PRIu64 " with no line, %"
                   PRIu64 " with the wrapped line standing masked, %"
                   PRIu64 " unmasking it, %" PRIu64 " wrapping open, %"
                   PRIu64 " writing Count to match Compare; passes waited "
                   "%" PRIu64 ", %" PRIu64 " and %" PRIu64,
                   recipe->machine, r.cpus, r.views, PASSES, clear, masked,
                   unmask, open, behind, waited_unmask, waited_open,
                   waited_behind);

    g_assert_true(ok);
    g_assert_cmpuint(clear, <, off_the_lock_budget(&r));
    assert_line_seen(&r, CAUSE_IP7);
    g_assert_cmpuint(masked, <, off_the_lock_budget(&r));

    qtest_quit(r.qts);
}

static void test_steady_state(const void *data)
{
    uint64_t compute, device_line, unmask, device_poll, memory_poll, raise;
    uint64_t waited_unmask, waited_raise;
    bool ok = true;
    Run r = { };

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    boot(&r, &recipes[0], PROGRAM_STEADY_STATE);
    g_assert_cmpuint(r.cpus, ==, 2);
    compute = run_phase(&r, 1);
    device_line = run_phase(&r, 2);
    unmask = run_phase(&r, 3);
    waited_unmask = check_delivery(&r, 3, CAUSE_IP2, DELIVERY_PASSES, &ok);
    device_poll = run_phase(&r, 4);
    memory_poll = run_phase(&r, 5);
    raise = run_phase(&r, 6);
    waited_raise = check_delivery(&r, 6, CAUSE_IP2, DELIVERY_PASSES, &ok);
    g_test_message("%u passes per processor: big-lock acquisitions %"
                   PRIu64 " computing, %" PRIu64 " with a Hub line standing "
                   "masked, %" PRIu64 " unmasking it, %" PRIu64
                   " polling a Hub register beside a computing peer, %"
                   PRIu64 " polling memory beside a computing peer, %"
                   PRIu64 " raising an open line; passes waited %" PRIu64
                   " and %" PRIu64,
                   PASSES, compute, device_line, unmask, device_poll,
                   memory_poll, raise, waited_unmask, waited_raise);

    g_assert_true(ok);
    g_assert_cmpuint(compute, <, off_the_lock_budget(&r));
    assert_line_seen(&r, CAUSE_IP2);
    g_assert_cmpuint(device_line, <, off_the_lock_budget(&r));
    /*
     * Every register read takes the lock once on the poller's own thread.
     * A peer kicked out of its run loop on each read would add as many.
     */
    g_assert_cmpuint(device_poll, <, PASSES + off_the_lock_budget(&r));
    g_assert_cmpuint(memory_poll, <, off_the_lock_budget(&r));

    qtest_quit(r.qts);
}

static bool in_r10000_family(const char *cpu_type)
{
    unsigned int i;

    for (i = 0; cpu_type && i < G_N_ELEMENTS(r10000_family); i++) {
        if (g_str_equal(cpu_type, r10000_family[i])) {
            return true;
        }
    }
    return false;
}

static const Recipe *find_recipe(const char *machine)
{
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(recipes); i++) {
        if (g_str_equal(recipes[i].machine, machine)) {
            return &recipes[i];
        }
    }
    return NULL;
}

/*
 * One count-wrap case per machine with an R10000-family default processor.
 * Machines named "-test" are fixtures other qtests construct and are left
 * out; anything else without a recipe is registered to fail by name.
 */
static void add_count_wrap_cases(void)
{
    QTestState *qts = qtest_init("-machine none");
    QDict *response = qtest_qmp(qts, "{ 'execute': 'query-machines' }");
    QList *machines = qdict_get_qlist(response, "return");
    const QListEntry *entry;

    for (entry = qlist_first(machines); entry; entry = qlist_next(entry)) {
        QDict *info = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(info, "name");
        const char *cpu = qdict_get_try_str(info, "default-cpu-type");
        const Recipe *recipe;
        g_autofree char *path = NULL;

        if (!in_r10000_family(cpu) || g_str_has_suffix(name, "-test")) {
            continue;
        }
        recipe = find_recipe(name);
        if (!recipe) {
            Recipe *missing = g_new0(Recipe, 1);

            missing->machine = g_strdup(name);
            recipe = missing;
        }
        path = g_strdup_printf("forward-progress/count-wrap/%s", name);
        qtest_add_data_func(path, recipe, test_count_wrap);
    }
    qobject_unref(response);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    add_count_wrap_cases();
    qtest_add_data_func("forward-progress/steady-state", NULL,
                        test_steady_state);
    return g_test_run();
}
