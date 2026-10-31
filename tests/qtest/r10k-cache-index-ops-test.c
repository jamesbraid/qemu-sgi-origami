/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * R10000-family secondary cache index operations: lines that software
 * stages with Index Store Tag and Index Store Data must satisfy later cached
 * loads, stores and instruction fetches.
 *
 * Each case runs a small guest image on a Malta board with an R10000-family
 * CPU. Memory behind the staged addresses is filled with 0xee, so a load that
 * returns anything else was served by a staged line, and one that returns
 * 0xee read memory. The host cannot count system-bus reads, so "no memory
 * read" below means the staged value came back instead of the memory bytes.
 *
 * Behaviour follows docs/datasheets/r10000-cache-index-ops.md in the Origami
 * repository. Cases that depend on an inferred choice or on a model choice
 * say so. The model choices are: the word order (TagLo is the word at the
 * lower address; the datasheet lists refill delivery order as not
 * established), a store making a staged line DirtyExclusive (inferred),
 * undefined cache field 10 doing nothing, and staged lines surviving
 * ordinary misses.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "libqtest.h"
#include "sn0-user-image.h"
#include "sgi-migrate.h"

#define ENTRY        0x800
#define FAULT_ENTRY  0x400
#define RESULT       0x00201000u
#define RESULT_GUEST SN0_KSEG1(RESULT)
#define PHASE        0x00
#define FAULT        0x08
#define COMMAND      0x20
#define RUNS         0x28
#define EXC_COUNT    0x30
#define EXC_CAUSE    0x34
#define EXC_VECTOR   0x38
#define SLOTS        0x100
#define SLOT(n)      (SLOTS + 8 * (n))
#define FAULT_MAGIC  0xdead

#define BASE         0x02000000u
#define FILL         0xee
#define FILL_WORD    0xeeeeeeeeu
#define PAGE         0x1000u

typedef struct Geo {
    const char *name;
    const char *cpu;
    uint32_t size;
    uint32_t line;
} Geo;

#define R12000_1M_64B \
    "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567," \
    "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576," \
    "r12000-scache-block-words=16"
#define R16000_4M_128B \
    "R16000,big-endian=on,r16000-experimental=on," \
    "r16000-reset-mode=0x3a005,r16000-scache-bytes=4194304"

static const Geo geos[] = {
    { "1m-128", "R10000", 1 << 20, 128 },
    { "1m-64", R12000_1M_64B, 1 << 20, 64 },
    { "2m-128", "R10000,reset-mode=0x12a785", 2 << 20, 128 },
    { "4m-128", R16000_4M_128B, 4 << 20, 128 },
};

static uint32_t way_size(const Geo *g)
{
    return g->size / 2;
}

static uint64_t xk(uint64_t pa)
{
    return UINT64_C(0x9800000000000000) | pa;
}

static uint32_t page_pa(unsigned int n)
{
    return BASE + n * PAGE;
}

/* TagLo for a secondary tag: PA[35:18], state, virtual index VA[13:12]. */
static uint32_t tag_lo(uint64_t pa, unsigned int state, unsigned int vidx)
{
    return (uint32_t)(extract64(pa, 18, 18) << 14) | (state << 10) |
           (vidx << 7);
}

static uint32_t tag_hi(uint64_t pa)
{
    return extract64(pa, 36, 4);
}

/* ---- guest code emitters ---- */

#define R_S0 SN0_R_S0
#define R_S1 SN0_R_S1
#define R_S2 SN0_R_S2
#define R_T0 SN0_R_T0
#define R_T1 SN0_R_T1
#define R_V0 SN0_R_V0
#define R_RA 31

static void begin(SN0Image *p)
{
    unsigned int jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, jump, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL;
         vec <= SN0_VEC_GENERAL; vec += 0x80) {
        p->pos = vec;
        jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
        sn0_patch_branch(p, jump, FAULT_ENTRY);
    }
    p->pos = FAULT_ENTRY;
    sn0_emit_li(p, SN0_R_K0, RESULT_GUEST);
    sn0_emit_li(p, SN0_R_K1, FAULT_MAGIC);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, FAULT);
    sn0_emit_park(p);
    p->pos = ENTRY;
    sn0_emit_li(p, R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, R_S1, RESULT_GUEST);
}

/*
 * Like begin(), but each exception is recorded and skipped: the count, the
 * Cause register and which vector took it (1 TLB refill, 2 general), then an
 * ERET past the faulting instruction.
 */
static void emit_recorder(SN0Image *p, unsigned int entry, unsigned int marker)
{
    p->pos = entry;
    sn0_emit_li(p, SN0_R_K0, RESULT_GUEST);
    sn0_emit_mfc0(p, SN0_R_K1, SN0_CP0_CAUSE);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, EXC_CAUSE);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_K0, SN0_R_K1, EXC_COUNT);
    sn0_emit(p, mips_addiu(SN0_R_K1, SN0_R_K1, 1));
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, EXC_COUNT);
    sn0_emit_li(p, SN0_R_K1, marker);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_K0, SN0_R_K1, EXC_VECTOR);
    sn0_emit(p, (0x10u << 26) | (1u << 21) | (SN0_R_K1 << 16) |
                (SN0_CP0_EPC << 11));                      /* dmfc0 EPC */
    sn0_emit(p, mips_daddiu(SN0_R_K1, SN0_R_K1, 4));
    sn0_emit_dmtc0(p, SN0_R_K1, SN0_CP0_EPC);
    sn0_emit_eret(p);
}

static void begin_recording(SN0Image *p)
{
    begin(p);
    emit_recorder(p, 0x500, 1);
    emit_recorder(p, 0x600, 2);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        unsigned int jump;

        p->pos = vec;
        jump = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
        sn0_patch_branch(p, jump, vec == SN0_VEC_GENERAL ? 0x600 : 0x500);
    }
    p->pos = ENTRY;
    /* begin() left the pointer set up; code resumes after its prologue. */
    sn0_emit_li(p, R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, R_S1, RESULT_GUEST);
}

/* Map the even page at @va to @frame, cached, global; the odd page is off. */
static void tlb_map(SN0Image *p, unsigned int index, uint64_t va,
                    uint64_t frame, bool dirty, bool valid)
{
    uint64_t lo0 = ((frame >> 12) << 6) | (3 << 3) | (dirty << 2) |
                   (valid << 1) | 1;

    sn0_emit_li64(p, R_T0, va);
    sn0_emit_dmtc0(p, R_T0, SN0_CP0_ENTRYHI);
    sn0_emit_li64(p, R_T0, lo0);
    sn0_emit_dmtc0(p, R_T0, SN0_CP0_ENTRYLO0);
    sn0_emit_li64(p, R_T0, 1);
    sn0_emit_dmtc0(p, R_T0, SN0_CP0_ENTRYLO1);
    sn0_emit_mtc0(p, SN0_R_ZERO, SN0_CP0_PAGEMASK);
    sn0_emit_li(p, R_T0, index);
    sn0_emit_mtc0(p, R_T0, SN0_CP0_INDEX);
    sn0_emit(p, 0x42000002);                                /* tlbwi */
    for (unsigned int i = 0; i < 4; i++) {
        sn0_emit_nop(p);
    }
}

static void finish(SN0Image *p)
{
    sn0_emit_li(p, R_T1, 1);
    sn0_emit_i(p, SN0_OPC_SW, R_S1, R_T1, PHASE);
    sn0_emit_park(p);
}

static void set_regs(SN0Image *p, uint32_t lo, uint32_t hi)
{
    sn0_emit_li(p, R_T0, lo);
    sn0_emit_mtc0(p, R_T0, 28);
    sn0_emit_li(p, R_T0, hi);
    sn0_emit_mtc0(p, R_T0, 29);
}

static void cache_at(SN0Image *p, unsigned int op, uint64_t va)
{
    sn0_emit_li64(p, R_S0, va);
    sn0_emit(p, mips_cache(op, R_S0, 0));
}

/* Index Store Tag (S) of @tagged_pa's tag into the slot @pa indexes. */
static void stage_tag_for(SN0Image *p, uint64_t pa, unsigned int way,
                          uint64_t tagged_pa, unsigned int state,
                          unsigned int vidx)
{
    set_regs(p, tag_lo(tagged_pa, state, vidx), tag_hi(tagged_pa));
    cache_at(p, 0x0b, xk(pa) | way);
}

static void stage_tag(SN0Image *p, uint64_t pa, unsigned int way,
                      unsigned int state)
{
    stage_tag_for(p, pa, way, pa, state, extract64(pa, 12, 2));
}

/* Index Store Data (S): doubleword at @pa, TagLo is the word at @pa. */
static void stage_data(SN0Image *p, uint64_t pa, unsigned int way,
                       uint32_t w0, uint32_t w1)
{
    set_regs(p, w0, w1);
    cache_at(p, 0x1f, xk(pa) | way);
}

static void end_line(SN0Image *p, unsigned int op, uint64_t va)
{
    cache_at(p, op, va);
}

static void put_slot(SN0Image *p, unsigned int reg, unsigned int slot)
{
    sn0_emit_i(p, SN0_OPC_SD, R_S1, reg, SLOT(slot));
}

/* Cached load of size 1, 2, 4 or 8 bytes, zero extended. */
static void load_to(SN0Image *p, unsigned int size, uint64_t pa,
                    unsigned int slot)
{
    static const unsigned int opcode[] = {
        [1] = 0x24, [2] = 0x25, [4] = 0x27, [8] = 0x37,
    };

    sn0_emit_li64(p, R_S0, xk(pa));
    sn0_emit(p, mips_itype(opcode[size], R_S0, R_T0, 0));
    put_slot(p, R_T0, slot);
}

static void store_from(SN0Image *p, unsigned int size, uint64_t pa,
                       uint32_t value)
{
    static const unsigned int opcode[] = {
        [1] = 0x28, [2] = 0x29, [4] = 0x2b,
    };

    sn0_emit_li64(p, R_S0, xk(pa));
    sn0_emit_li(p, R_T1, value);
    sn0_emit(p, mips_itype(opcode[size], R_S0, R_T1, 0));
}

static void tag_to(SN0Image *p, uint64_t va, unsigned int slot)
{
    cache_at(p, 0x07, va);
    sn0_emit_mfc0(p, R_T0, 28);
    put_slot(p, R_T0, slot);
    sn0_emit_mfc0(p, R_T0, 29);
    put_slot(p, R_T0, slot + 1);
}

static void data_to(SN0Image *p, uint64_t va, unsigned int slot)
{
    cache_at(p, 0x1b, va);
    sn0_emit_mfc0(p, R_T0, 28);
    put_slot(p, R_T0, slot);
    sn0_emit_mfc0(p, R_T0, 29);
    put_slot(p, R_T0, slot + 1);
}

/* Call the code at @pa through the cached segment, save $v0. */
static void call_to(SN0Image *p, uint64_t pa, unsigned int slot)
{
    sn0_emit_li64(p, R_S2, xk(pa));
    sn0_emit(p, (R_S2 << 21) | (R_RA << 11) | 0x09); /* jalr $ra, $s2 */
    sn0_emit_nop(p);
    put_slot(p, R_V0, slot);
}

/* ---- host helpers ---- */

static void fill_memory(QTestState *qts, const Geo *g)
{
    qtest_memset(qts, BASE, FILL, 0x40000);
    qtest_memset(qts, BASE + way_size(g), FILL, 0x40000);
    qtest_memset(qts, BASE + 2 * way_size(g), FILL, 0x40000);
}

static QTestState *start_machine(const char *cpu, SN0Image *p,
                                 const char *extra)
{
    g_autofree char *path = sn0_image_write(p, "r10k-cache-index-ops");
    QTestState *qts;

    qts = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                      "-bios %s -accel tcg -S %s", cpu, path, extra);
    unlink(path);
    return qts;
}

static void wait_phase(QTestState *qts, unsigned int phase)
{
    SgiWait wait;

    sgi_wait_start(&wait, "R10000 index operations phase %u", phase);
    while (qtest_readl(qts, RESULT + PHASE) != phase) {
        g_assert_cmphex(qtest_readl(qts, RESULT + FAULT), ==, 0);
        sgi_wait_pause(&wait, 1000);
    }
    g_assert_cmphex(qtest_readl(qts, RESULT + FAULT), ==, 0);
}

typedef void (*SeedFn)(QTestState *qts, const Geo *g);

static QTestState *run_image_with(const Geo *g, SN0Image *p, SeedFn seed,
                                  const char *extra)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return NULL;
    }
    qts = start_machine(g->cpu, p, extra);
    fill_memory(qts, g);
    if (seed) {
        seed(qts, g);
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    wait_phase(qts, 1);
    return qts;
}

static QTestState *run_image(const Geo *g, SN0Image *p, SeedFn seed)
{
    return run_image_with(g, p, seed, "-no-reboot -no-shutdown");
}

static uint64_t slot(QTestState *qts, unsigned int n)
{
    return qtest_readq(qts, RESULT + SLOT(n));
}

static uint32_t slot32(QTestState *qts, unsigned int n)
{
    return slot(qts, n);
}

static uint32_t mem32(QTestState *qts, uint64_t pa)
{
    uint8_t bytes[4];

    qtest_memread(qts, pa, bytes, sizeof(bytes));
    return ldl_be_p(bytes);
}

static uint64_t mem64(QTestState *qts, uint64_t pa)
{
    uint8_t bytes[8];

    qtest_memread(qts, pa, bytes, sizeof(bytes));
    return ldq_be_p(bytes);
}

static void put_mem32(QTestState *qts, uint64_t pa, uint32_t value)
{
    uint8_t bytes[4];

    stl_be_p(bytes, value);
    qtest_memwrite(qts, pa, bytes, sizeof(bytes));
}

/* ---- case 1: encodings and the undefined cache field ---- */

static void test_encodings(gconstpointer opaque)
{
    const Geo *g = opaque;
    static const unsigned int inert[] = {
        0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a, 0x1e, /* cache field 10 */
        0x09, 0x1d, 0x08, 0x1c,         /* primary D and I tag and data */
    };
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t pa = page_pa(0);
    QTestState *qts;

    begin(p);
    stage_tag(p, pa, 0, 2);
    stage_data(p, pa, 0, 0x55667788, 0x11223344);
    for (unsigned int i = 0; i < ARRAY_SIZE(inert); i++) {
        cache_at(p, inert[i], xk(pa));
    }
    load_to(p, 4, pa, 0);
    load_to(p, 4, pa + 4, 1);
    tag_to(p, xk(pa), 2);
    data_to(p, xk(pa), 4);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0x55667788);
    g_assert_cmphex(slot32(qts, 1), ==, 0x11223344);
    g_assert_cmphex(slot32(qts, 2), ==, tag_lo(pa, 2, extract64(pa, 12, 2)));
    g_assert_cmphex(slot32(qts, 3), ==, tag_hi(pa));
    g_assert_cmphex(slot32(qts, 4), ==, 0x55667788);
    g_assert_cmphex(slot32(qts, 5), ==, 0x11223344);
    qtest_quit(qts);
}

/* ---- case 2: the way comes from address bit 0 ---- */

static void test_ways(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    uint64_t b = a + way_size(g);
    QTestState *qts;

    begin(p);
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0xa0a1a2a3, 0xa4a5a6a7);
    stage_tag_for(p, a, 1, b, 2, extract64(b, 12, 2));
    stage_data(p, a, 1, 0xb0b1b2b3, 0xb4b5b6b7);
    load_to(p, 4, a, 0);
    load_to(p, 4, b, 1);
    tag_to(p, xk(a), 2);            /* way 0 */
    tag_to(p, xk(a) | 1, 4);        /* way 1 */
    end_line(p, 0x03, xk(a) | 1);
    load_to(p, 4, a, 6);
    load_to(p, 4, b, 7);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0xa0a1a2a3);
    g_assert_cmphex(slot32(qts, 1), ==, 0xb0b1b2b3);
    g_assert_cmphex(slot32(qts, 2), ==, tag_lo(a, 2, extract64(a, 12, 2)));
    g_assert_cmphex(slot32(qts, 4), ==, tag_lo(b, 2, extract64(b, 12, 2)));
    g_assert_cmphex(slot32(qts, 3), ==, tag_hi(a));
    g_assert_cmphex(slot32(qts, 5), ==, tag_hi(b));
    g_assert_cmphex(slot32(qts, 6), ==, 0xa0a1a2a3);
    g_assert_cmphex(slot32(qts, 7), ==, FILL_WORD);
    qtest_quit(qts);
}

/* ---- case 3: which states hit ---- */

static void test_states(gconstpointer opaque)
{
    const Geo *g = opaque;
    static const unsigned int states[] = { 1, 2, 3, 0 };
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;

    begin(p);
    for (unsigned int i = 0; i < ARRAY_SIZE(states); i++) {
        stage_tag(p, page_pa(i), 0, states[i]);
        stage_data(p, page_pa(i), 0, 0x10000000 + i, 0x20000000 + i);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(states); i++) {
        load_to(p, 4, page_pa(i), i);
    }
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    for (unsigned int i = 0; i < 3; i++) {
        g_assert_cmphex(slot32(qts, i), ==, 0x10000000 + i);
    }
    g_assert_cmphex(slot32(qts, 3), ==, FILL_WORD);
    qtest_quit(qts);
}

/* ---- cases 4, 6 and 12: hit data, widths and word order ---- */

/*
 * The word order checks here (TagLo at the lower address) assert a MODEL
 * CHOICE, not an established fact.
 */
static void test_hit_data(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    uint64_t b = page_pa(1);
    QTestState *qts;

    begin(p);
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0x89abcdef, 0x01234567);
    load_to(p, 8, a, 0);
    load_to(p, 4, a, 1);
    load_to(p, 4, a + 4, 2);
    load_to(p, 2, a, 3);
    load_to(p, 2, a + 2, 4);
    for (unsigned int i = 0; i < 4; i++) {
        load_to(p, 1, a + i, 5 + i);
    }
    load_to(p, 1, a + 7, 9);
    load_to(p, 4, a + 8, 10);       /* the other doubleword reads zero */
    load_to(p, 8, a + 8, 11);
    /* Two stores into one quadword keep only the second. */
    stage_tag(p, b, 0, 2);
    stage_data(p, b, 0, 0x0c0c0c0c, 0x0d0d0d0d);
    stage_data(p, b + 8, 0, 0x0e0e0e0e, 0x0f0f0f0f);
    load_to(p, 8, b, 12);
    load_to(p, 8, b + 8, 13);
    load_to(p, 8, b + 16, 14);      /* a never written quadword */
    data_to(p, xk(a), 16);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot(qts, 0), ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(slot32(qts, 1), ==, 0x89abcdef);
    g_assert_cmphex(slot32(qts, 2), ==, 0x01234567);
    g_assert_cmphex(slot32(qts, 3), ==, 0x89ab);
    g_assert_cmphex(slot32(qts, 4), ==, 0xcdef);
    g_assert_cmphex(slot32(qts, 5), ==, 0x89);
    g_assert_cmphex(slot32(qts, 6), ==, 0xab);
    g_assert_cmphex(slot32(qts, 7), ==, 0xcd);
    g_assert_cmphex(slot32(qts, 8), ==, 0xef);
    g_assert_cmphex(slot32(qts, 9), ==, 0x67);
    g_assert_cmphex(slot32(qts, 10), ==, 0);
    g_assert_cmphex(slot(qts, 11), ==, 0);
    g_assert_cmphex(slot(qts, 12), ==, 0);
    g_assert_cmphex(slot(qts, 13), ==, UINT64_C(0x0e0e0e0e0f0f0f0f));
    g_assert_cmphex(slot(qts, 14), ==, 0);
    g_assert_cmphex(slot32(qts, 16), ==, 0x89abcdef);
    g_assert_cmphex(slot32(qts, 17), ==, 0x01234567);
    qtest_quit(qts);
}

/* ---- case 5: misses ---- */

static void test_misses(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    /*
     * The lowest tag bit the compare can test: the way's worth of address
     * is the first bit above the index.
     */
    unsigned int low = ctz32(g->size) - 1;
    unsigned int variants = 40 - low;
    uint64_t good = page_pa(variants);
    QTestState *qts;

    begin(p);
    for (unsigned int bit = low; bit < 40; bit++) {
        unsigned int n = bit - low;
        uint32_t lo = tag_lo(page_pa(n), 2, extract64(page_pa(n), 12, 2));
        uint32_t hi = tag_hi(page_pa(n));

        if (bit < 36) {
            lo ^= 1u << (14 + bit - 18);
        } else {
            hi ^= 1u << (bit - 36);
        }
        set_regs(p, lo, hi);
        cache_at(p, 0x0b, xk(page_pa(n)));
        stage_data(p, page_pa(n), 0, 0x30000000 + n, 0x40000000 + n);
        load_to(p, 4, page_pa(n), n);
    }
    stage_tag(p, good, 0, 2);
    stage_data(p, good, 0, 0x50505050, 0x60606060);
    load_to(p, 4, good + g->line, variants);    /* another set, same tag */
    load_to(p, 4, good, variants + 1);          /* the line survives */
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    for (unsigned int n = 0; n < variants; n++) {
        g_assert_cmphex(slot32(qts, n), ==, FILL_WORD);
    }
    g_assert_cmphex(slot32(qts, variants), ==, FILL_WORD);
    g_assert_cmphex(slot32(qts, variants + 1), ==, 0x50505050);
    qtest_quit(qts);
}

/* ---- cases 6 and 7: instruction fetch, memory versus staged code ---- */

#define INSN_ORI_V0(imm) mips_ori(SN0_R_V0, 0, imm)
#define INSN_JR_RA       0x03e00008u

static void seed_code(QTestState *qts, const Geo *g)
{
    uint64_t pa = page_pa(0);

    put_mem32(qts, pa, INSN_ORI_V0(0x2222));
    put_mem32(qts, pa + 4, INSN_JR_RA);
    put_mem32(qts, pa + 8, 0);
}

/* Word order within a staged doubleword is a MODEL CHOICE (see above). */
static void test_fetch(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    uint64_t b = page_pa(1);
    QTestState *qts;

    begin(p);
    call_to(p, a, 0);                   /* compiled from memory first */
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, INSN_ORI_V0(0x1111), INSN_JR_RA);
    call_to(p, a, 1);                   /* now the staged words run */
    /*
     * Words never stored fetch as zero (no-ops). The second line has no
     * code in memory, so a fetch from memory would fault.
     */
    stage_tag(p, b, 0, 2);
    stage_data(p, b, 0, INSN_ORI_V0(0x3333), 0);
    stage_data(p, b + 16, 0, INSN_JR_RA, 0);
    call_to(p, b, 2);
    end_line(p, 0x03, xk(a));
    call_to(p, a, 3);                   /* memory again */
    finish(p);

    qts = run_image(g, p, seed_code);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0x2222);
    g_assert_cmphex(slot32(qts, 1), ==, 0x1111);
    g_assert_cmphex(slot32(qts, 2), ==, 0x3333);
    g_assert_cmphex(slot32(qts, 3), ==, 0x2222);
    qtest_quit(qts);
}

/*
 * Each quadword of a block is its own array entry: rewriting quadword 0
 * must not disturb the others, and a fetch from quadword 1 still finds its
 * staged words.
 */
static void test_quadwords(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    unsigned int quads = g->line / 16;
    uint64_t a = page_pa(0);
    QTestState *qts;

    begin(p);
    stage_tag(p, a, 0, 2);
    stage_data(p, a + 16, 0, INSN_ORI_V0(0x4444), INSN_JR_RA);
    for (unsigned int q = 2; q < quads; q++) {
        stage_data(p, a + q * 16, 0, 0xc0000000 + q, 0xc1000000 + q);
    }
    for (unsigned int i = 0; i < 20; i++) {
        stage_data(p, a + (i & 1) * 8, 0, 0xdd000000 + i, 0xde000000 + i);
    }
    call_to(p, a + 16, 0);
    load_to(p, 4, a + 16, 1);
    for (unsigned int q = 2; q < quads; q++) {
        load_to(p, 4, a + q * 16, q);
    }
    load_to(p, 4, a, 10);
    load_to(p, 4, a + 8, 11);
    /* Index Load Data of quadword 1, and of each other doubleword. */
    data_to(p, xk(a + 16), 12);
    for (unsigned int q = 1; q < quads; q++) {
        data_to(p, xk(a + q * 16 + 8), 16 + 2 * q);
    }
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 12), ==, INSN_ORI_V0(0x4444));
    g_assert_cmphex(slot32(qts, 13), ==, INSN_JR_RA);
    for (unsigned int q = 1; q < quads; q++) {
        g_assert_cmphex(slot32(qts, 16 + 2 * q), ==, 0);
        g_assert_cmphex(slot32(qts, 17 + 2 * q), ==, 0);
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0x4444);
    g_assert_cmphex(slot32(qts, 1), ==, INSN_ORI_V0(0x4444));
    for (unsigned int q = 2; q < quads; q++) {
        g_assert_cmphex(slot32(qts, q), ==, 0xc0000000 + q);
    }
    g_assert_cmphex(slot32(qts, 10), ==, 0);        /* last write was +8 */
    g_assert_cmphex(slot32(qts, 11), ==, 0xdd000013);
    qtest_quit(qts);
}

/* ---- case 8: what ends a line ---- */

static void test_ending(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;

    begin(p);
    for (unsigned int n = 0; n < 6; n++) {
        stage_tag(p, page_pa(n), 0, 2);
        stage_data(p, page_pa(n), 0, 0x70000000 + n, 0x80000000 + n);
    }
    end_line(p, 0x03, xk(page_pa(0)));                  /* index */
    end_line(p, 0x13, xk(page_pa(1)));                  /* hit invalidate */
    end_line(p, 0x17, xk(page_pa(2)));                  /* hit writeback */
    stage_tag(p, page_pa(3), 0, 0);                     /* state 0 */
    /* A different line stored into the same slot. */
    stage_tag_for(p, page_pa(4), 0, page_pa(4) + 2 * way_size(g), 2, 0);
    /* Operations that must leave page 5's line alone. */
    end_line(p, 0x03, xk(page_pa(5)) + g->line);        /* neighbour */
    end_line(p, 0x13, xk(page_pa(5)) + 2 * way_size(g)); /* hit that misses */
    end_line(p, 0x03, xk(page_pa(5)) | 1);              /* other way */
    for (unsigned int n = 0; n < 6; n++) {
        load_to(p, 4, page_pa(n), n);
    }
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    for (unsigned int n = 0; n < 5; n++) {
        g_assert_cmphex(slot32(qts, n), ==, FILL_WORD);
    }
    g_assert_cmphex(slot32(qts, 5), ==, 0x70000005);
    qtest_quit(qts);
}

/* ---- case 9: write back ---- */

static void stage_pattern(SN0Image *p, uint64_t pa, const Geo *g,
                          unsigned int state)
{
    stage_tag(p, pa, 0, state);
    for (unsigned int q = 0; q < g->line / 16; q++) {
        stage_data(p, pa + q * 16 + (q & 1) * 8, 0, 0x10000000 + q,
                   0x20000000 + q);
    }
}

static void check_pattern(QTestState *qts, uint64_t pa, const Geo *g,
                          bool written)
{
    for (unsigned int q = 0; q < g->line / 16; q++) {
        for (unsigned int d = 0; d < 2; d++) {
            uint64_t want = FILL_WORD | (uint64_t)FILL_WORD << 32;

            if (written) {
                want = d == (q & 1) ?
                    ((uint64_t)(0x10000000 + q) << 32) | (0x20000000 + q) : 0;
            }
            g_assert_cmphex(mem64(qts, pa + q * 16 + d * 8), ==, want);
        }
    }
    /* The block after it is not touched. */
    g_assert_cmphex(mem32(qts, pa + g->line), ==, FILL_WORD);
}

/* The word order of the written-back line is a MODEL CHOICE. */
static void test_writeback(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;

    begin(p);
    stage_pattern(p, page_pa(0), g, 3);
    stage_pattern(p, page_pa(1), g, 2);
    stage_pattern(p, page_pa(2), g, 1);
    stage_pattern(p, page_pa(3), g, 3);
    stage_pattern(p, page_pa(4), g, 3);
    end_line(p, 0x03, xk(page_pa(0)));
    end_line(p, 0x03, xk(page_pa(1)));
    end_line(p, 0x03, xk(page_pa(2)));
    end_line(p, 0x13, xk(page_pa(3)));          /* never writes */
    end_line(p, 0x17, xk(page_pa(4)));
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    check_pattern(qts, page_pa(0), g, true);
    check_pattern(qts, page_pa(1), g, false);
    check_pattern(qts, page_pa(2), g, false);
    check_pattern(qts, page_pa(3), g, false);
    check_pattern(qts, page_pa(4), g, true);
    qtest_quit(qts);
}

/* ---- cases 10 and 11: stores, and a store makes the line dirty ---- */

/* DirtyExclusive after a store is INFERRED; word order is a MODEL CHOICE. */
static void test_stores(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    QTestState *qts;

    begin(p);
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0x01020304, 0x05060708);
    store_from(p, 4, a, 0xcafebabe);
    store_from(p, 1, a + 6, 0x5a);
    store_from(p, 4, a + 256, 0x0badf00d);      /* an unstaged line */
    load_to(p, 4, a, 0);
    load_to(p, 4, a + 4, 1);
    load_to(p, 4, a + 256, 2);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0xcafebabe);
    g_assert_cmphex(slot32(qts, 1), ==, 0x05065a08);
    g_assert_cmphex(slot32(qts, 2), ==, 0x0badf00d);
    g_assert_cmphex(mem32(qts, a), ==, FILL_WORD);
    g_assert_cmphex(mem32(qts, a + 4), ==, FILL_WORD);
    g_assert_cmphex(mem32(qts, a + 256), ==, 0x0badf00d);
    qtest_quit(qts);
}

/* The DirtyExclusive state after a store is INFERRED in the datasheet. */
static void test_store_dirties(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;

    begin(p);
    for (unsigned int n = 0; n < 4; n++) {
        stage_tag(p, page_pa(n), 0, n < 3 ? n + 1 : 2);
        stage_data(p, page_pa(n), 0, 0x0a0b0c0d, 0x01020304);
    }
    for (unsigned int n = 0; n < 3; n++) {
        store_from(p, 4, page_pa(n) + 4, 0x99999999);
    }
    load_to(p, 4, page_pa(3), 8);               /* a load alone */
    for (unsigned int n = 0; n < 4; n++) {
        tag_to(p, xk(page_pa(n)), 2 * n);
    }
    end_line(p, 0x03, xk(page_pa(0)));          /* state 1, now dirty */
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    for (unsigned int n = 0; n < 3; n++) {
        g_assert_cmpuint(extract32(slot32(qts, 2 * n), 10, 2), ==, 3);
    }
    g_assert_cmpuint(extract32(slot32(qts, 6), 10, 2), ==, 2);
    g_assert_cmphex(mem32(qts, page_pa(0)), ==, 0x0a0b0c0d);
    g_assert_cmphex(mem32(qts, page_pa(0) + 4), ==, 0x99999999);
    qtest_quit(qts);
}

/* ---- cases 13 and 14: translations made earlier, page bookkeeping ---- */

static void test_pages(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    uint64_t b = page_pa(1);
    QTestState *qts;

    begin(p);
    /* Translations made before the first line is staged. */
    load_to(p, 4, a, 0);
    store_from(p, 4, a + 256, 0x77777777);
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0x90909090, 0x91919191);
    load_to(p, 4, a, 1);                /* sees the line */
    load_to(p, 4, a + 256, 2);          /* the page's other lines pass */
    end_line(p, 0x03, xk(a));
    load_to(p, 4, a, 3);                /* back to memory */
    load_to(p, 4, a + 256, 4);
    /* Two lines in one page. */
    stage_tag(p, b, 0, 2);
    stage_data(p, b, 0, 0xb1b1b1b1, 0);
    stage_tag(p, b + 256, 0, 2);
    stage_data(p, b + 256, 0, 0xb2b2b2b2, 0);
    load_to(p, 4, b, 5);
    load_to(p, 4, b + 256, 6);
    end_line(p, 0x03, xk(b));
    load_to(p, 4, b, 7);
    load_to(p, 4, b + 256, 8);
    end_line(p, 0x03, xk(b + 256));
    load_to(p, 4, b + 256, 9);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, FILL_WORD);
    g_assert_cmphex(slot32(qts, 1), ==, 0x90909090);
    g_assert_cmphex(slot32(qts, 2), ==, 0x77777777);
    g_assert_cmphex(slot32(qts, 3), ==, FILL_WORD);
    g_assert_cmphex(slot32(qts, 4), ==, 0x77777777);
    g_assert_cmphex(slot32(qts, 5), ==, 0xb1b1b1b1);
    g_assert_cmphex(slot32(qts, 6), ==, 0xb2b2b2b2);
    g_assert_cmphex(slot32(qts, 7), ==, FILL_WORD);
    g_assert_cmphex(slot32(qts, 8), ==, 0xb2b2b2b2);
    g_assert_cmphex(slot32(qts, 9), ==, FILL_WORD);
    qtest_quit(qts);
}

/* ---- case 16: virtual index bits ---- */

static void test_virtual_index(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    QTestState *qts;

    begin(p);
    stage_tag_for(p, a, 0, a, 2, (extract64(a, 12, 2) + 1) & 3);
    stage_data(p, a, 0, 0xd0d0d0d0, 0xd1d1d1d1);
    load_to(p, 4, a, 0);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0xd0d0d0d0);
    qtest_quit(qts);
}

/*
 * Hit operations ignore the low bits of their operand: bit 0 set, as a
 * kernel's secondary flush leaves it, still hits and takes no Address
 * Error. The model keeps this tolerance although the manual's wording
 * conflicts (datasheet, "Unresolved wording"); this pins the choice.
 */
static void test_hit_low_bit(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;

    begin(p);
    for (unsigned int n = 0; n < 2; n++) {
        stage_tag(p, page_pa(n), 0, 2);
        stage_data(p, page_pa(n), 0, 0x61000000 + n, 0x62000000 + n);
        load_to(p, 4, page_pa(n), n);
    }
    end_line(p, 0x13, xk(page_pa(0)) | 1);
    end_line(p, 0x17, xk(page_pa(1)) | 1);
    for (unsigned int n = 0; n < 2; n++) {
        load_to(p, 4, page_pa(n), 2 + n);
    }
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0x61000000);
    g_assert_cmphex(slot32(qts, 1), ==, 0x61000001);
    g_assert_cmphex(slot32(qts, 2), ==, FILL_WORD);
    g_assert_cmphex(slot32(qts, 3), ==, FILL_WORD);
    qtest_quit(qts);
}

/* Operands through kseg0 (cached by Config.K0 at reset) stage and hit. */
static void test_kseg0(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t pa = page_pa(0);
    uint64_t k0 = UINT64_C(0xffffffff80000000) | pa;
    QTestState *qts;

    begin(p);
    set_regs(p, tag_lo(pa, 2, extract64(pa, 12, 2)), tag_hi(pa));
    cache_at(p, 0x0b, k0);
    set_regs(p, 0x71717171, 0x72727272);
    cache_at(p, 0x1f, k0);
    sn0_emit_li64(p, R_S0, k0);
    sn0_emit(p, mips_itype(0x27, R_S0, R_T0, 0));
    put_slot(p, R_T0, 0);
    sn0_emit(p, mips_itype(0x27, R_S0, R_T0, 4));
    put_slot(p, R_T0, 1);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0x71717171);
    g_assert_cmphex(slot32(qts, 1), ==, 0x72727272);
    qtest_quit(qts);
}

/*
 * A dirty line whose tag names an address nothing answers cannot be
 * written back. The failure is logged and the line is invalidated anyway.
 */
static void test_failed_writeback(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t pa = page_pa(0);
    QTestState *qts;

    begin(p);
    stage_tag_for(p, pa, 0, pa | (UINT64_C(1) << 38), 3, 0);
    end_line(p, 0x03, xk(pa));
    tag_to(p, xk(pa), 0);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmpuint(extract32(slot32(qts, 0), 10, 2), ==, 0);
    qtest_quit(qts);
}

/* ---- case 8 (power-on): a CPU reset empties the cache ---- */

static void test_reset(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(0);
    unsigned int skip;
    QTestState *qts;

    begin(p);
    /* A second pass, flagged by the host, skips the staging. */
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, RUNS);
    skip = sn0_emit_branch(p, SN0_OPC_BNE, R_T0, 0);
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0xe0e0e0e0, 0xe1e1e1e1);
    sn0_patch_branch(p, skip, p->pos);
    load_to(p, 4, a, 0);
    finish(p);

    /* A reset has to restart the machine, so no -no-reboot here. */
    qts = run_image_with(g, p, NULL, "");
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, 0xe0e0e0e0);
    qtest_qmp_assert_success(qts, "{ 'execute': 'stop' }");
    qtest_system_reset_nowait(qts);
    qtest_qmp_eventwait(qts, "RESET");
    qtest_writel(qts, RESULT + RUNS, 1);
    qtest_writel(qts, RESULT + PHASE, 0);
    qtest_writeq(qts, RESULT + SLOT(0), 0);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    wait_phase(qts, 1);
    g_assert_cmphex(slot32(qts, 0), ==, FILL_WORD);
    qtest_quit(qts);
}

/* ---- mapped operands: translated as data addresses (checks 1 to 3) ---- */

#define V1  UINT64_C(0x10000000)
#define V2  UINT64_C(0x50000000)
#define V3  (V1 + 0x2000)
#define F1  UINT64_C(0x02001000)
#define F2  UINT64_C(0x02002000)
#define OFF 0x80

static void test_mapped_index(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint32_t t1 = tag_lo(0x03000000, 2, 1);
    uint32_t t2 = tag_lo(0x03400000, 2, 2);
    QTestState *qts;

    begin(p);
    /* V1 and V2 differ only above the index; F1 and F2 differ in it. */
    tlb_map(p, 0, V1, F1, true, true);
    tlb_map(p, 1, V2, F2, true, true);
    tlb_map(p, 2, V3, F1, true, true);
    set_regs(p, t1, 0);
    cache_at(p, 0x0b, V1 + OFF);
    set_regs(p, t2, 0);
    cache_at(p, 0x0b, V2 + OFF);
    tag_to(p, xk(F1 + OFF), 0);         /* check 1 */
    tag_to(p, xk(F2 + OFF), 2);
    tag_to(p, V3 + OFF, 4);             /* check 2: same frame, other VA */
    /* Check 3: the primary index is virtual, whatever the frame. */
    set_regs(p, 0xabcdef40, 0x5);
    cache_at(p, 0x08, V1 + OFF);
    cache_at(p, 0x04, xk(BASE + 0x4000 + OFF));
    sn0_emit_mfc0(p, R_T0, 28);
    put_slot(p, R_T0, 6);
    sn0_emit_mfc0(p, R_T0, 29);
    put_slot(p, R_T0, 7);
    set_regs(p, 0xabcdef40, 0xe0000005);
    cache_at(p, 0x09, V1 + OFF);
    cache_at(p, 0x05, xk(BASE + 0x4000 + OFF));
    sn0_emit_mfc0(p, R_T0, 28);
    put_slot(p, R_T0, 8);
    sn0_emit_mfc0(p, R_T0, 29);
    put_slot(p, R_T0, 9);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmphex(slot32(qts, 0), ==, t1);
    g_assert_cmphex(slot32(qts, 2), ==, t2);
    g_assert_cmphex(slot32(qts, 4), ==, t1);
    g_assert_cmphex(slot32(qts, 6), ==, 0xabcdef40);
    g_assert_cmphex(slot32(qts, 7), ==, 0x5);
    g_assert_cmphex(slot32(qts, 8), ==, 0xabcdef40);
    g_assert_cmphex(slot32(qts, 9), ==, 0xe0000005);
    qtest_quit(qts);
}

/* ---- check 6: exceptions on a mapped operand ---- */

static void test_mapped_exceptions(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint32_t sentinel = tag_lo(0x03000000, 2, 0);
    uint32_t other = tag_lo(0x03800000, 3, 0);
    uint64_t fs = BASE + 0x4000;
    QTestState *qts;

    begin_recording(p);
    /* A sentinel line the faulting operations must not disturb. */
    set_regs(p, sentinel, 0);
    cache_at(p, 0x0b, xk(fs + OFF));
    set_regs(p, other, 0);
    cache_at(p, 0x0b, 0x30000000 + OFF);        /* no TLB entry */
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_COUNT);
    put_slot(p, R_T0, 0);
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_VECTOR);
    put_slot(p, R_T0, 1);
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_CAUSE);
    put_slot(p, R_T0, 2);
    tlb_map(p, 3, 0x40000000, fs, true, false);
    set_regs(p, other, 0);
    cache_at(p, 0x0b, 0x40000000 + OFF);        /* invalid entry */
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_COUNT);
    put_slot(p, R_T0, 3);
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_VECTOR);
    put_slot(p, R_T0, 4);
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_CAUSE);
    put_slot(p, R_T0, 5);
    tag_to(p, xk(fs + OFF), 6);                 /* untouched */
    /* A valid entry with the dirty bit clear takes the operation. */
    tlb_map(p, 4, 0x60000000, fs + 0x1000, false, true);
    set_regs(p, other, 0);
    cache_at(p, 0x0b, 0x60000000 + OFF);
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, EXC_COUNT);
    put_slot(p, R_T0, 8);
    tag_to(p, xk(fs + 0x1000 + OFF), 9);
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    g_assert_cmpuint(slot32(qts, 0), ==, 1);
    g_assert_cmpuint(slot32(qts, 1), ==, 1);
    g_assert_cmpuint(extract32(slot32(qts, 2), 2, 5), ==, 2);   /* TLBL */
    g_assert_cmpuint(slot32(qts, 3), ==, 2);
    g_assert_cmpuint(slot32(qts, 4), ==, 2);
    g_assert_cmpuint(extract32(slot32(qts, 5), 2, 5), ==, 2);
    g_assert_cmphex(slot32(qts, 6), ==, sentinel);
    g_assert_cmpuint(slot32(qts, 8), ==, 2);
    g_assert_cmphex(slot32(qts, 9), ==, other);
    qtest_quit(qts);
}

/* ---- check 4: many staged lines, and a primary block (check 5) ---- */

static void test_many(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    uint64_t a = page_pa(5);
    uint64_t b = a + way_size(g);
    QTestState *qts;

    begin(p);
    for (unsigned int n = 0; n < 4; n++) {
        stage_tag(p, page_pa(n), 0, 2);
        stage_data(p, page_pa(n), 0, 0x11000000 + n, 0x12000000 + n);
    }
    stage_tag(p, a, 0, 2);
    stage_data(p, a, 0, 0x21000000, 0x22000000);
    stage_tag_for(p, a, 1, b, 2, extract64(b, 12, 2));
    stage_data(p, a, 1, 0x31000000, 0x32000000);
    for (unsigned int n = 0; n < 4; n++) {
        tag_to(p, xk(page_pa(n)), 4 * n);
        data_to(p, xk(page_pa(n)), 4 * n + 2);
    }
    tag_to(p, xk(a), 16);
    data_to(p, xk(a), 18);
    tag_to(p, xk(a) | 1, 20);
    data_to(p, xk(a) | 1, 22);
    /* One primary instruction block: 16 words, each its own operation. */
    for (unsigned int i = 0; i < 16; i++) {
        set_regs(p, 0x40000000 + i, i & 0xf);
        cache_at(p, 0x1c, xk(page_pa(8)) + 4 * i);
    }
    for (unsigned int i = 0; i < 16; i++) {
        cache_at(p, 0x18, xk(page_pa(8)) + 4 * i);
        sn0_emit_mfc0(p, R_T0, 28);
        put_slot(p, R_T0, 32 + 2 * i);
        sn0_emit_mfc0(p, R_T0, 29);
        put_slot(p, R_T0, 33 + 2 * i);
    }
    finish(p);

    qts = run_image(g, p, NULL);
    if (!qts) {
        return;
    }
    for (unsigned int n = 0; n < 4; n++) {
        g_assert_cmphex(slot32(qts, 4 * n), ==,
                        tag_lo(page_pa(n), 2, extract64(page_pa(n), 12, 2)));
        g_assert_cmphex(slot32(qts, 4 * n + 2), ==, 0x11000000 + n);
        g_assert_cmphex(slot32(qts, 4 * n + 3), ==, 0x12000000 + n);
    }
    g_assert_cmphex(slot32(qts, 16), ==, tag_lo(a, 2, extract64(a, 12, 2)));
    g_assert_cmphex(slot32(qts, 18), ==, 0x21000000);
    g_assert_cmphex(slot32(qts, 19), ==, 0x22000000);
    g_assert_cmphex(slot32(qts, 20), ==, tag_lo(b, 2, extract64(b, 12, 2)));
    g_assert_cmphex(slot32(qts, 22), ==, 0x31000000);
    g_assert_cmphex(slot32(qts, 23), ==, 0x32000000);
    for (unsigned int i = 0; i < 16; i++) {
        g_assert_cmphex(slot32(qts, 32 + 2 * i), ==, 0x40000000 + i);
        g_assert_cmphex(slot32(qts, 33 + 2 * i), ==, i & 0xf);
    }
    qtest_quit(qts);
}

/* ---- case 15: other processors do not stage ---- */

static void test_r4400(void)
{
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    QTestState *qts;
    uint64_t pa = page_pa(0);

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    begin(p);
    /* The R4000's own status has no KX; its 64-bit segments are off. */
    sn0_emit_li64(p, R_S0, UINT64_C(0xffffffff80000000) | pa);
    set_regs(p, tag_lo(pa, 2, 0), tag_hi(pa));
    sn0_emit(p, mips_cache(0x0b, R_S0, 0));
    sn0_emit(p, mips_itype(0x27, R_S0, R_T0, 0));
    put_slot(p, R_T0, 0);
    finish(p);
    qts = start_machine("R4000", p, "-no-reboot -no-shutdown");
    qtest_memset(qts, pa, FILL, PAGE);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    wait_phase(qts, 1);
    g_assert_cmphex(slot32(qts, 0), ==, FILL_WORD);
    qtest_quit(qts);
}

/* ---- case 18: snapshot and migration ---- */

static void test_migration(gconstpointer opaque)
{
    const Geo *g = opaque;
    g_autofree SN0Image *p = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *src_args = NULL;
    g_autofree char *dst_args = NULL;
    QTestState *from, *to;
    uint64_t a = page_pa(0);
    uint64_t b = page_pa(1);
    unsigned int wait_loop;
    unsigned int again;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    begin(p);
    stage_tag(p, a, 0, 3);
    stage_data(p, a, 0, 0xf0f0f0f0, 0xf1f1f1f1);
    sn0_emit_li(p, R_T1, 1);
    sn0_emit_i(p, SN0_OPC_SW, R_S1, R_T1, PHASE);
    wait_loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LW, R_S1, R_T0, COMMAND);
    again = sn0_emit_branch(p, SN0_OPC_BEQ, R_T0, 0);
    sn0_patch_branch(p, again, wait_loop);
    load_to(p, 4, a, 0);
    load_to(p, 4, b, 1);
    tag_to(p, xk(a), 2);
    sn0_emit_li(p, R_T1, 2);
    sn0_emit_i(p, SN0_OPC_SW, R_S1, R_T1, PHASE);
    sn0_emit_park(p);

    path = sn0_image_write(p, "r10k-cache-index-ops-mig");
    uri = g_strdup_printf("unix:%s/r10k-cache-mig-%d.sock",
                          g_get_tmp_dir(), getpid());
    src_args = g_strdup_printf("-M malta -m 64M -nic none -vga none "
                               "-cpu %s -bios %s -accel tcg "
                               "-no-reboot -no-shutdown", g->cpu, path);
    dst_args = g_strdup_printf("%s -incoming defer", src_args);
    from = qtest_init(src_args);
    to = qtest_init(dst_args);
    unlink(path);
    qtest_memset(from, a, FILL, 2 * PAGE);
    qtest_memset(to, a, FILL, 2 * PAGE);
    wait_phase(from, 1);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration(from, "completed");
    sgi_migrate_resume(to);
    qtest_writel(to, RESULT + COMMAND, 1);
    wait_phase(to, 2);
    g_assert_cmphex(slot32(to, 0), ==, 0xf0f0f0f0);
    g_assert_cmphex(slot32(to, 1), ==, FILL_WORD);
    g_assert_cmphex(slot32(to, 2), ==, tag_lo(a, 3, extract64(a, 12, 2)));
    qtest_quit(from);
    qtest_quit(to);
}

/* ---- case 15 elsewhere, registration ---- */

static void add_geo_case(const char *name, void (*fn)(gconstpointer),
                         bool all_geometries)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(geos); i++) {
        g_autofree char *path = g_strdup_printf("r10k/cache-index/%s/%s",
                                                name, geos[i].name);

        qtest_add_data_func(path, &geos[i], fn);
        if (!all_geometries) {
            break;
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    add_geo_case("encodings", test_encodings, false);
    add_geo_case("ways", test_ways, true);
    add_geo_case("states", test_states, false);
    add_geo_case("hit-data", test_hit_data, true);
    add_geo_case("misses", test_misses, true);
    add_geo_case("fetch", test_fetch, false);
    add_geo_case("quadwords", test_quadwords, true);
    add_geo_case("ending", test_ending, true);
    add_geo_case("writeback", test_writeback, true);
    add_geo_case("stores", test_stores, false);
    add_geo_case("store-dirties", test_store_dirties, false);
    add_geo_case("pages", test_pages, false);
    add_geo_case("virtual-index", test_virtual_index, false);
    add_geo_case("hit-low-bit", test_hit_low_bit, false);
    add_geo_case("kseg0", test_kseg0, false);
    add_geo_case("failed-writeback", test_failed_writeback, false);
    add_geo_case("mapped-index", test_mapped_index, false);
    add_geo_case("mapped-exceptions", test_mapped_exceptions, false);
    add_geo_case("many-lines", test_many, true);
    add_geo_case("reset", test_reset, false);
    add_geo_case("migration", test_migration, true);
    qtest_add_func("r10k/cache-index/non-r10000", test_r4400);
    return g_test_run();
}
