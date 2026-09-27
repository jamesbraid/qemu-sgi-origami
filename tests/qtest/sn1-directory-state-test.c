/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 directory state on processor requests, driven by the
 * processors themselves.
 *
 * The Bedrock's memory unit keeps one directory word per 128-byte memory
 * line and rewrites it as it serves each processor's cache misses, upgrades
 * and write-backs. A qtest access carries no requesting processor and makes
 * no cached reference, so the transitions can only be driven by guest code:
 * the processors load, store, prefetch and flush one line and publish the
 * directory word they read back through the directory backdoor after each
 * step.
 *
 * Expected words follow the Origin directory protocol (the SGI Origin
 * ccNUMA paper, ISCA 1997, section 3.2), the R10000's requests (R10000
 * User's Manual version 2.0, Table 4-2 and the processor block read, write
 * and eliminate protocols) and the public SN1 standard directory layout
 * (hubmd_next.h at Linux v2.5.40): format in bits 1:0, a pointer entry's
 * state in 4:2 and pointer in 15:8, a shared-fine entry's first tag in 4:2
 * and first vector in 15:8. The pointer value of the processor in slot C
 * of node zero is 4 and its shared-vector bit is 1, which the memory
 * unit's datasheet records as inferred.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define SN1_MD_MEMORY_CONFIG (SPACE_IO + UINT64_C(0x01800000) + 0x780008)
/* Software bank zero on the installed memory, as the stock PROM leaves it. */
#define SN1_MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

/* The memory unit's directory configuration: bit 0 selects premium. */
#define SN1_MD_DIRECTORY_CONFIG (SPACE_IO + UINT64_C(0x01800000) + 0x7c0000)

/* The line under test and where the processors publish what they read. */
#define LINE_PHYS 0x580000
/* Its directory word as qtest reaches it, node zero's backdoor. */
#define LINE_DIRECTORY (UINT64_C(0x101c0000000) + (LINE_PHYS >> 3))
#define RESULT_PHYS 0x600000
/* The step the processors have reached, and the host's go-ahead. */
#define RESULT_STEP 0x100
#define RESULT_GO 0x108
#define RESULT_CAUSE 0x1f0
#define RESULT_EPC 0x1f8

/* The fields every expectation compares: format, state or tag, pointer. */
#define DIRECTORY_FIELDS 0xff1f
#define UNOWNED 0x0000
#define POINTER(_p) (((_p) << 8) | 0x1)
#define SHARED(_vector) (((_vector) << 8) | 0x2)

#define POLL_TRIES 500
#define POLL_INTERVAL_US 10000

/* Registers. */
#define R_ZERO 0
#define R_T0 8  /* the line, cached */
#define R_T1 9  /* its directory word, uncached */
#define R_T2 10 /* the result area, cached */
#define R_T3 11
#define R_T4 12
#define R_T5 13 /* another line, cached */

typedef struct Program {
    uint8_t *image;
    unsigned int pc;
} Program;

static void emit(Program *p, uint32_t insn)
{
    stl_be_p(p->image + p->pc, insn);
    p->pc += 4;
}

static uint32_t itype(unsigned op, int rs, int rt, uint16_t imm)
{
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}

static uint32_t rtype(int rs, int rt, int rd, int sa, unsigned funct)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}

#define LUI(_rt, _imm)          itype(0x0f, 0, _rt, _imm)
#define ORI(_rt, _rs, _imm)     itype(0x0d, _rs, _rt, _imm)
#define ANDI(_rt, _rs, _imm)    itype(0x0c, _rs, _rt, _imm)
#define LD(_rt, _off, _base)    itype(0x37, _base, _rt, _off)
#define SD(_rt, _off, _base)    itype(0x3f, _base, _rt, _off)
#define CACHE(_op, _off, _base) itype(0x2f, _base, _op, _off)
#define PREF(_hint, _off, _base) itype(0x33, _base, _hint, _off)
#define DSLL32(_rd, _rt)        rtype(0, _rt, _rd, 0, 0x3c)
#define DSRL32(_rd, _rt)        rtype(0, _rt, _rd, 0, 0x3e)
#define DADDU(_rd, _rs, _rt)    rtype(_rs, _rt, _rd, 0, 0x2d)
#define DADDIU(_rt, _rs, _imm)  itype(0x19, _rs, _rt, _imm)
#define MFC0_STATUS(_rt)        (0x40000000 | ((_rt) << 16) | (12 << 11))
#define STATUS_CH               (1u << 18)
#define SYNC                    0x0000000f
#define NOP                     0x00000000

/* A branch to @target from the instruction at the current position. */
static void emit_branch(Program *p, unsigned op, int rs, int rt,
                        unsigned int target)
{
    int32_t offset = ((int32_t)target - (int32_t)(p->pc + 4)) / 4;

    emit(p, itype(op, rs, rt, (uint16_t)offset));
    emit(p, NOP);
}

/* Upper 32 bits @hi, then add the zero-extended 32-bit @lo. */
static void emit_address(Program *p, int reg, uint16_t hi, uint16_t hi_low,
                         uint32_t lo)
{
    emit(p, LUI(reg, hi));
    emit(p, ORI(reg, reg, hi_low));
    emit(p, DSLL32(reg, reg));
    emit(p, LUI(R_T3, lo >> 16));
    emit(p, DSLL32(R_T3, R_T3));
    emit(p, DSRL32(R_T3, R_T3));
    emit(p, DADDU(reg, reg, R_T3));
}

/* Read the line's directory word and publish it at @slot. */
static void emit_publish(Program *p, unsigned int slot)
{
    emit(p, SYNC);
    emit(p, LD(R_T3, 0, R_T1));
    emit(p, SD(R_T3, slot, R_T2));
}

/* Index WriteBack Invalidate of the line's primary and secondary sets. */
static void emit_flush(Program *p)
{
    emit(p, SYNC);
    emit(p, CACHE(0x1, 0, R_T0));
    emit(p, CACHE(0x1, 1, R_T0));
    emit(p, CACHE(0x3, 0, R_T0));
    emit(p, CACHE(0x3, 1, R_T0));
}

typedef void (*EmitBody)(Program *p);

/* One step of a scenario: the processor in @slice runs @body. */
typedef struct Step {
    unsigned int slice;
    EmitBody body;
} Step;

typedef struct Scenario {
    const char *cpus;       /* the -smp argument */
    bool premium;           /* select the premium directory format */
    bool ignore_ecc;        /* set the directory's ignore-ECC bit */
    const char *snapshot;   /* if set, save this snapshot before running */
    const char *extra;      /* further command-line arguments */
    uint64_t word;          /* if nonzero, write the line's word first */
    const Step *steps;
    unsigned int nr_steps;
} Scenario;

/*
 * Every processor runs the same image and finds its slice (A to D, 0 to 3)
 * in its CPU number. The steps run one at a time in order: each waits for
 * the step word to reach its own index, runs, and advances the word.
 */
static char *create_prom(const Scenario *sc)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    Program p = { .image = image };
    unsigned int block[4];
    char *path = NULL;
    int fd;

    emit(&p, itype(0x04, 0, 0, (CODE_START - 4) / 4)); /* b CODE_START */
    emit(&p, NOP);
    for (unsigned int offset = 0x200; offset < 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    /* General exceptions publish Cause and EPC, so a fault reads as one. */
    p.pc = 0x380;
    emit(&p, 0x40000000 | (R_T3 << 16) | (13 << 11)); /* mfc0 t3, Cause */
    emit(&p, SD(R_T3, RESULT_CAUSE, R_T2));
    emit(&p, 0x40200000 | (R_T3 << 16) | (14 << 11)); /* dmfc0 t3, EPC */
    emit(&p, SD(R_T3, RESULT_EPC, R_T2));
    emit(&p, 0x1000ffff);
    emit(&p, NOP);

    p.pc = CODE_START;
    /* 64-bit kernel addressing for the xkphys pointers below. */
    emit(&p, 0x40000000 | (R_T4 << 16) | (12 << 11)); /* mfc0 t4, Status */
    emit(&p, ORI(R_T4, R_T4, 0x80));
    emit(&p, 0x40800000 | (R_T4 << 16) | (12 << 11)); /* mtc0 t4, Status */
    emit(&p, NOP);
    emit_address(&p, R_T2, 0xa800, 0, RESULT_PHYS);
    emit_address(&p, R_T0, 0xa800, 0, LINE_PHYS);
    /* Node zero's directory word for the line, through the uncached form. */
    emit_address(&p, R_T1, 0x9000, 0x0001,
                 0xc0000000 | ((LINE_PHYS >> 3) & ~0xfu));
    /* Which slice this processor is: bits 1:0 of its CPU number. */
    emit(&p, LUI(R_T4, 0x9200));
    emit(&p, DSLL32(R_T4, R_T4));
    emit(&p, LUI(R_T3, 0x0100));
    emit(&p, DADDU(R_T4, R_T4, R_T3));
    emit(&p, LD(R_T4, 0x20, R_T4));
    emit(&p, ANDI(R_T4, R_T4, 3));
    for (unsigned int slice = 0; slice < 4; slice++) {
        block[slice] = 0x1000 + slice * 0x1000;
        emit(&p, ORI(R_T3, R_ZERO, slice));
        emit_branch(&p, 0x04, R_T4, R_T3, block[slice]); /* beq t4, t3 */
    }
    emit(&p, 0x1000ffff);
    emit(&p, NOP);

    for (unsigned int slice = 0; slice < 4; slice++) {
        g_assert_cmpuint(p.pc, <=, block[slice]);
        p.pc = block[slice];
        for (unsigned int i = 0; i < sc->nr_steps; i++) {
            unsigned int wait;

            if (sc->steps[i].slice != slice) {
                continue;
            }
            wait = p.pc;
            emit(&p, LD(R_T3, RESULT_STEP, R_T2));
            emit(&p, ORI(R_T4, R_ZERO, i));
            emit_branch(&p, 0x05, R_T3, R_T4, wait); /* bne t3, t4, wait */
            sc->steps[i].body(&p);
            emit(&p, ORI(R_T3, R_ZERO, i + 1));
            emit(&p, SYNC);
            emit(&p, SD(R_T3, RESULT_STEP, R_T2));
        }
        emit(&p, 0x1000ffff);
        emit(&p, NOP);
    }

    fd = g_file_open_tmp("sn1-directory-state-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint64_t result(QTestState *s, unsigned int slot)
{
    return qtest_readq(s, RESULT_PHYS + slot);
}

/* Wait until the processors have finished step @count or faulted. */
static void wait_steps(QTestState *s, unsigned int count)
{
    for (unsigned int tries = 0; tries < POLL_TRIES; tries++) {
        if (result(s, RESULT_STEP) >= count || result(s, RESULT_EPC)) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(result(s, RESULT_EPC), ==, 0);
    g_assert_cmpuint(result(s, RESULT_STEP), ==, count);
}

/* Start the processors on @sc; the caller waits for the steps it needs. */
static QTestState *start_scenario(const Scenario *sc, char **prom)
{
    g_autofree char *quoted = NULL;
    QTestState *s;

    *prom = create_prom(sc);
    quoted = g_shell_quote(*prom);
    s = qtest_initf("-M origin300 -cpu R14000 -smp %s -m 512M -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S %s", sc->cpus,
                    quoted, sc->extra ? sc->extra : "");
    qtest_writeq(s, SN1_MD_MEMORY_CONFIG, SN1_MD_INSTALLED_SELECT2);
    if (sc->premium || sc->ignore_ecc) {
        qtest_writeq(s, SN1_MD_DIRECTORY_CONFIG,
                     (sc->premium ? 1 : 0) | (sc->ignore_ecc ? 2 : 0));
    }
    if (sc->word) {
        qtest_writeq(s, LINE_DIRECTORY, sc->word);
    }
    if (sc->snapshot) {
        g_free(qtest_hmp(s, "savevm %s", sc->snapshot));
    }
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    return s;
}

/* Run every step of @sc and check the word each started from. */
static QTestState *run_scenario(const Scenario *sc, char **prom)
{
    QTestState *s = start_scenario(sc, prom);

    wait_steps(s, sc->nr_steps);
    /* Reading the word through the backdoor is what starts following it. */
    g_assert_cmphex(result(s, 0x00) & DIRECTORY_FIELDS, ==,
                    sc->word & DIRECTORY_FIELDS);
    return s;
}

static void finish(QTestState *s, char *prom)
{
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_free(prom);
}

#define STEPS(...) \
    .steps = (const Step[]) { __VA_ARGS__ }, \
    .nr_steps = sizeof((const Step[]) { __VA_ARGS__ }) / sizeof(Step)

/*
 * Name the line in a Hit WriteBack Invalidate (S), which the unit needs
 * before a read of its word will watch its page, then publish the word.
 */
static void emit_first(Program *p)
{
    emit(p, CACHE(0x17, 0, R_T0));
    emit_publish(p, 0x00);
}

/*
 * Name, then read the word of, the first line of each of the @count pages
 * after the one under test, as a check of those lines would; each page
 * takes a slot.
 */
static void emit_name_pages(Program *p, unsigned int count, bool rewarm)
{
    emit(p, DADDU(R_T4, R_T1, R_ZERO));
    emit(p, DADDU(R_T5, R_T0, R_ZERO));
    for (unsigned int i = 0; i < count; i++) {
        emit(p, DADDIU(R_T4, R_T4, 0x200));
        emit(p, DADDIU(R_T5, R_T5, 0x1000));
        emit(p, CACHE(0x17, 0, R_T5));
        emit(p, LD(R_T3, 0, R_T4));
        if (rewarm) {
            emit(p, LD(R_T3, 0, R_T1));  /* the line under test's word */
        }
    }
}

/* Named after what they do and where they publish the word afterwards. */
#define LOAD_STEP(_name, _slot) \
    static void _name(Program *p) \
    { \
        emit(p, LD(R_T3, 0, R_T0)); \
        emit_publish(p, _slot); \
    }
#define STORE_STEP(_name, _slot) \
    static void _name(Program *p) \
    { \
        emit(p, SD(R_ZERO, 0, R_T0)); \
        emit_publish(p, _slot); \
    }
#define FLUSH_STEP(_name, _slot) \
    static void _name(Program *p) \
    { \
        emit_flush(p); \
        emit_publish(p, _slot); \
    }

LOAD_STEP(load_08, 0x08)
LOAD_STEP(load_10, 0x10)
LOAD_STEP(load_18, 0x18)
LOAD_STEP(load_28, 0x28)
STORE_STEP(store_08, 0x08)
STORE_STEP(store_10, 0x10)
STORE_STEP(store_20, 0x20)
FLUSH_STEP(flush_10, 0x10)
FLUSH_STEP(flush_48, 0x48)

/*
 * A read miss on an unowned line grants it exclusive; another interface's
 * read shares it; that processor's store takes it exclusive and invalidates
 * the first copy, so the first processor's next read misses again and
 * shares the line once more.
 */
static void test_loads_and_stores(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, load_08 }, { 0, store_10 },
              { 2, load_18 }, { 2, store_20 }, { 0, load_28 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, POINTER(4));
    g_assert_cmphex(result(s, 0x28) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    finish(s, prom);
}

/*
 * Four processors, two to an interface. A vector bit stands for an
 * interface, so a second reader on the owner's interface shares the line
 * with one bit, and a reader on the other interface adds the second. A
 * store from the fourth invalidates the three copies, and the second
 * processor's next read shares it with the writer.
 */
static void test_three_readers(void)
{
    const Scenario sc = {
        .cpus = "4",
        STEPS({ 0, emit_first }, { 0, load_08 }, { 1, load_10 },
              { 2, load_18 }, { 3, store_20 }, { 1, load_28 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, SHARED(0x1));
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, POINTER(5));
    g_assert_cmphex(result(s, 0x28) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    finish(s, prom);
}

/* Hit WriteBack Invalidate (S), then publish Status.CH at @slot. */
static void emit_hit_flush(Program *p, unsigned int slot)
{
    emit(p, SYNC);
    emit(p, CACHE(0x17, 0, R_T0));
    emit(p, MFC0_STATUS(R_T3));
    emit(p, SD(R_T3, slot, R_T2));
}

/* A clean copy flushed by index is gone: the hit form then misses. */
static void clean_flush_then_hit(Program *p)
{
    emit(p, LD(R_T3, 0, R_T0));
    emit_flush(p);
    emit_publish(p, 0x28);
    emit_hit_flush(p, 0x30);
}

/* The hit form finds a dirty copy, writes it back and says so. */
static void dirty_hit_flush(Program *p)
{
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_hit_flush(p, 0x38);
    emit_publish(p, 0x40);
}

static void test_flushes(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, flush_10 },
              { 0, clean_flush_then_hit }, { 0, dirty_hit_flush },
              { 2, load_18 }, { 2, flush_48 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* The index flush writes the dirty line back, which unowns it. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* A clean exclusive copy is dropped without telling the directory... */
    g_assert_cmphex(result(s, 0x28) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* ...but it is dropped: a hit flush of it misses. */
    g_assert_cmphex(result(s, 0x30) & STATUS_CH, ==, 0);
    /* A hit flush of a dirty copy hits and writes it back. */
    g_assert_cmphex(result(s, 0x38) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(s, 0x40) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* Slot C reads the unowned line and keeps it through its flush. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, POINTER(4));
    g_assert_cmphex(result(s, 0x48) & DIRECTORY_FIELDS, ==, POINTER(4));
    finish(s, prom);
}

/*
 * Read the directory words of sixteen other lines, as a walk over the
 * directory does, which fills the unit's slots; then store to, flush and
 * check a line 128 KiB above the one under test, which the walk left out.
 */
static void emit_busy(Program *p)
{
    emit_name_pages(p, 16, false);
    emit(p, LUI(R_T3, 0x0002));
    emit(p, DADDU(R_T0, R_T0, R_T3));
    emit(p, DADDIU(R_T1, R_T1, 0x4000));
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_publish(p, 0x08);
    emit_flush(p);
    emit_publish(p, 0x10);
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x18);
}

/*
 * With every slot taken by a directory walk, reading another line's word
 * still watches its page, in the least recently used slot.
 */
static void test_full_pool_takes_the_oldest_slot(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, emit_busy }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    /* Not followed yet: the store before the flush is not seen. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* Followed once flushed and read: the next load is seen. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

static void prefetches(Program *p)
{
    emit(p, PREF(6, 0, R_T0));
    emit_publish(p, 0x08);
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_publish(p, 0x10);
    emit_flush(p);
    emit(p, PREF(1, 0, R_T0));
    emit_publish(p, 0x18);
    emit_flush(p);
    emit_publish(p, 0x20);
    emit(p, PREF(2, 0, R_T0)); /* an undefined hint */
    emit_publish(p, 0x28);
}

static void test_prefetch(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, prefetches }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    /* A prefetch with a load hint reads the line shared. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, SHARED(0x1));
    /* A store to the shared copy upgrades it. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* A prefetch with a store hint reads the line exclusive, clean... */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* ...so the flush drops it silently. */
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* An undefined hint does nothing; the dropped copy asks nothing. */
    g_assert_cmphex(result(s, 0x28) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * The premium format: the same fields, with the check field at 37:32 and
 * 48 rather than 21:16. The words are whole, check field included; the
 * check columns are the Origin 300 directory diagnostic's own table.
 */
static void test_premium_format(void)
{
    const Scenario sc = {
        .cpus = "2", .premium = true,
        STEPS({ 0, emit_first }, { 0, load_08 }, { 2, load_18 },
              { 2, store_20 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08), ==, UINT64_C(0x1f00000001));
    g_assert_cmphex(result(s, 0x18), ==, UINT64_C(0x2c00000302));
    g_assert_cmphex(result(s, 0x20), ==, UINT64_C(0xc00000401));
    finish(s, prom);
}

/*
 * Uncached loads and stores bypass the caches and make no coherent request
 * (R10000 User's Manual version 2.0, Table 4-2); the memory form of the
 * uncached window is attribute 3.
 */
static void uncached_accesses(Program *p)
{
    emit_address(p, R_T4, 0x9600, 0, LINE_PHYS);
    emit(p, LD(R_T3, 0, R_T4));
    emit_publish(p, 0x08);
    emit(p, SD(R_ZERO, 0, R_T4));
    emit_publish(p, 0x10);
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x18);
}

static void test_uncached_accesses(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, uncached_accesses }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* The cached load that follows is seen. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/* Wait for the host's go-ahead, then load the line. */
static void wait_then_load(Program *p)
{
    unsigned int wait;

    emit(p, CACHE(0x17, 0, R_T0)); /* named, but its word not yet read */
    wait = p->pc;
    emit(p, LD(R_T3, RESULT_GO, R_T2));
    emit_branch(p, 0x04, R_T3, R_ZERO, wait); /* beq t3, zero, wait */
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x08);
}

/*
 * A debugger's read of the directory word is not software's: it must not
 * start following the line, so the load after it is not seen.
 */
static void test_debugger_read(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, wait_then_load }),
    };
    char *prom;
    QTestState *s;
    char *out;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = start_scenario(&sc, &prom);
    /* The processors are running, so 64-bit addressing is on. */
    g_usleep(100 * 1000);
    out = qtest_hmp(s, "x /1gx 0x90000001c00b0000");
    g_assert_nonnull(strstr(out, "0000000000000000"));
    g_free(out);
    qtest_writeq(s, RESULT_PHYS + RESULT_GO, 1);
    wait_steps(s, sc.nr_steps);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * Whether the system view maps @address to a region of @type ("ram" or
 * "i/o"): an overlay the memory unit places over a followed line shows as
 * I/O over the RAM.
 */
static bool flat_view_type_is(QTestState *s, uint64_t address,
                              const char *type)
{
    g_autofree char *mtree = qtest_hmp(s, "info mtree -f");
    const char *view = strstr(mtree, "Root memory region: system");
    const char *end;
    bool found = false;

    g_assert_nonnull(view);
    end = strstr(view + 1, "\nFlatView #");
    if (!end) {
        end = view + strlen(view);
    }
    for (const char *line = strchr(view, '\n'); line && line < end;
         line = strchr(line + 1, '\n')) {
        uint64_t start, last;
        char region[32];

        if (sscanf(line, " %" SCNx64 "-%" SCNx64 " (prio %*d, %31[^)])",
                   &start, &last, region) == 3 &&
            address >= start && address <= last) {
            found = !strcmp(region, type);
            break;
        }
    }
    return found;
}

/*
 * Read the directory words of sixteen lines on other pages, enough to take
 * every one of the unit's slots, so the line under test loses its overlay.
 */
static void evict(Program *p)
{
    emit_name_pages(p, 16, false);
}

/*
 * After the host's go-ahead, read the word again, which follows the line
 * once more, then write the line back with a hit flush and publish the
 * word and Status.CH.
 */
static void resume_after_release(Program *p)
{
    unsigned int wait = p->pc;

    emit(p, LD(R_T3, RESULT_GO, R_T2));
    emit_branch(p, 0x04, R_T3, R_ZERO, wait); /* beq t3, zero, wait */
    emit_publish(p, 0x10);
    emit_hit_flush(p, 0x18);
    emit_publish(p, 0x20);
}

/*
 * The memory unit overlays only a few pages, and reusing a slot drops the
 * overlay of the page it held. That is the model's bookkeeping, not the
 * hardware's: the directory word and the processor's dirty copy stay as
 * they were, and when the line is followed again the dirty copy is still
 * there to write back.
 */
static void test_evicted_line_keeps_its_state(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, evict },
              { 0, resume_after_release }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = start_scenario(&sc, &prom);
    wait_steps(s, 3);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    qtest_writeq(s, RESULT_PHYS + RESULT_GO, 1);
    wait_steps(s, sc.nr_steps);
    /* Released and followed again, the word is as the store left it... */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* ...and the processor still holds the dirty copy, which it writes back. */
    g_assert_cmphex(result(s, 0x18) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * A word naming a processor the machine does not have -- node 1's first
 * slot, on one node -- did not come from the protocol, and a load leaves
 * it as it is.
 */
static void test_absent_owner(void)
{
    const Scenario sc = {
        .cpus = "2", .word = POINTER(8),
        STEPS({ 0, emit_first }, { 0, load_08 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(8));
    finish(s, prom);
}

/* A hit flush by slot C, publishing Status.CH at 0x08 and the word at 0x10. */
static void hit_flush_c(Program *p)
{
    emit_hit_flush(p, 0x08);
    emit_publish(p, 0x10);
}

/*
 * A line followed for the first time takes its copies from the directory
 * word: the owner a pointer names holds the line, clean, so a hit flush
 * finds it and drops it without writing back.
 */
static void test_owner_from_the_word(void)
{
    const Scenario sc = {
        .cpus = "2", .word = POINTER(4),
        STEPS({ 0, emit_first }, { 2, hit_flush_c }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(4));
    finish(s, prom);
}

/* A sharer bit gives the processors on that interface shared copies. */
static void test_sharers_from_the_word(void)
{
    const Scenario sc = {
        .cpus = "2", .word = SHARED(0x2),
        STEPS({ 0, emit_first }, { 2, hit_flush_c }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, SHARED(0x2));
    finish(s, prom);
}

/* Load the line through xkphys with cache algorithm @cca, then publish. */
static void emit_load_cca(Program *p, unsigned int cca, unsigned int slot)
{
    emit_address(p, R_T4, 0x8000 | cca << 11, 0, LINE_PHYS);
    emit(p, LD(R_T3, 0, R_T4));
    emit_publish(p, slot);
}

static void load_cca4_10(Program *p)
{
    emit_load_cca(p, 4, 0x10);
}

static void load_cca3_08(Program *p)
{
    emit_load_cca(p, 3, 0x08);
}

/*
 * Under cache algorithm 4, coherent exclusive, a load miss reads the line
 * exclusive (R10000 User's Manual version 2.0, Table 4-2), so it takes the
 * line from the processor that owns it rather than sharing it.
 */
static void test_exclusive_algorithm_load(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 2, load_08 }, { 0, load_cca4_10 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(4));
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * Under cache algorithm 3, cacheable noncoherent, a miss is a noncoherent
 * block read that no agent checks for coherency (the same manual, section
 * 4.6), so the directory is not consulted.
 */
static void test_noncoherent_algorithm_load(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, load_cca3_08 }, { 0, load_10 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* The coherent load after it is seen. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * The block transfer engine copies a line into the line under test. It is
 * an I/O requester, not the processor whose store started it (the public
 * SN1 header's MD_PTR_OWNER_IO), and makes no processor's copy.
 */
#define BTE_SOURCE 0x640000
#define BTE_NOTIFY (RESULT_PHYS + 0x1e0)
#define BTE_MARKER 0x5a5a1234

static void emit_store_imm(Program *p, int base, uint16_t off, uint32_t imm)
{
    emit(p, LUI(R_T3, imm >> 16));
    emit(p, ORI(R_T3, R_T3, imm & 0xffff));
    emit(p, SD(R_T3, off, base));
}

static void bte_copy(Program *p)
{
    /* A marker in the source line, through the uncached memory form. */
    emit_address(p, R_T4, 0x9600, 0, BTE_SOURCE);
    emit_store_imm(p, R_T4, 0, BTE_MARKER);
    /* Node zero's engine 0, through the local register window. */
    emit_address(p, R_T4, 0x9200, 0, 0x01410000);
    emit_store_imm(p, R_T4, 0x00, 1 | 1 << 20);  /* one line, busy */
    emit_store_imm(p, R_T4, 0x08, BTE_SOURCE);
    emit_store_imm(p, R_T4, 0x10, LINE_PHYS);
    emit_store_imm(p, R_T4, 0x20, BTE_NOTIFY);
    emit_store_imm(p, R_T4, 0x18, 1 << 4);       /* start, notify */
    emit_publish(p, 0x08);
}

static void test_block_transfer_is_not_a_processor(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, bte_copy }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    /* The copy happened... */
    g_assert_cmphex(qtest_readq(s, LINE_PHYS), ==, BTE_MARKER);
    g_assert_cmphex(qtest_readq(s, BTE_NOTIFY), ==, 0);
    /* ...and no processor holds the line for it. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * With the directory's ignore-ECC bit set the unit keeps a word as written,
 * check field included (the directory configuration register's bit 1), and
 * so it does with the words it rewrites: the check field the line's word
 * had goes unchanged. Without the bit it is regenerated.
 */
static void test_ignore_ecc_keeps_the_check_field(void)
{
    const Scenario sc = {
        .cpus = "2", .ignore_ecc = true,
        STEPS({ 0, emit_first }, { 0, load_08 }),
    };
    const Scenario checked = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, load_08 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08), ==, POINTER(0));
    finish(s, prom);
    s = run_scenario(&checked, &prom);
    g_assert_cmphex(result(s, 0x08) & ~(uint64_t)DIRECTORY_FIELDS, !=, 0);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * The unit watches the whole 4 KiB page around a line whose word software
 * read: a load of another line on that page is followed too, and its word
 * shows the transition when it is read afterwards.
 */
static void neighbour_line(Program *p)
{
    emit(p, LD(R_T3, 0x80, R_T0));
    emit(p, SYNC);
    emit(p, LD(R_T3, 0x10, R_T1));  /* the next line's word */
    emit(p, SD(R_T3, 0x08, R_T2));
}

static void test_page_is_watched(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, neighbour_line }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * Reading a followed line's word again makes it the most recently used, so
 * fifteen other pages can come and go without taking its slot.
 */
static void keep_warm(Program *p)
{
    emit_name_pages(p, 20, true);
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x08);
}

static void test_recently_read_page_stays_watched(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, keep_warm }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

/*
 * A directory walk names no line, so reading a word watches nothing: a
 * load of a line on another page, whose word was read first, is not seen.
 */
static void walk_then_load(Program *p)
{
    emit(p, DADDIU(R_T4, R_T1, 0x400));   /* the word two pages on */
    emit(p, DADDIU(R_T5, R_T0, 0x2000));  /* that line */
    emit(p, LD(R_T3, 0, R_T4));
    emit(p, LD(R_T3, 0, R_T5));
    emit(p, SYNC);
    emit(p, LD(R_T3, 0, R_T4));
    emit(p, SD(R_T3, 0x08, R_T2));
}

static void test_unnamed_line_is_not_watched(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, walk_then_load }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * Writing a named line's word through the backdoor sets it but does not
 * watch the page: software checks a word by reading it.
 */
static void name_write_then_load(Program *p)
{
    emit(p, CACHE(0x17, 0, R_T0));
    emit(p, SD(R_ZERO, 0, R_T1));
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x08);
}

static void test_backdoor_write_does_not_watch(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, name_write_then_load }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = start_scenario(&sc, &prom);
    wait_steps(s, sc.nr_steps);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * A reset forgets the pages being watched and the lines processors named:
 * once the memory is configured again, neither the old overlay nor a read
 * of the named line's word watches the page.
 */
static void test_reset_forgets_watching(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, load_08 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "i/o"));
    /* The processors restart and wait on a step that never comes. */
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_writeq(s, SN1_MD_MEMORY_CONFIG, SN1_MD_INSTALLED_SELECT2);
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    qtest_readq(s, LINE_DIRECTORY);
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    finish(s, prom);
}

/*
 * A snapshot saved while nothing was named or watched carries none of the
 * unit's watch state. Loading it after the processors have named, watched
 * and dirtied the line must leave none behind: run again from the
 * snapshot, the processor's store is a miss that takes the line
 * exclusive, where a stale dirty copy would make it a silent hit.
 */
static void test_loadvm_forgets_watching(void)
{
    g_autofree char *dir = NULL;
    g_autofree char *disk = NULL;
    g_autofree char *extra = NULL;
    GError *error = NULL;
    Scenario sc = {
        .cpus = "2", .snapshot = "clean",
        STEPS({ 0, emit_first }, { 0, store_08 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }
    dir = g_dir_make_tmp("sn1-directory-state-snapshot-XXXXXX", &error);
    g_assert_no_error(error);
    disk = g_build_filename(dir, "state.qcow2", NULL);
    g_assert_true(mkimg(disk, "qcow2", 64));
    extra = g_strdup_printf("-drive file=%s,format=qcow2,if=none", disk);
    sc.extra = extra;
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "i/o"));
    g_free(qtest_hmp(s, "stop"));
    g_free(qtest_hmp(s, "loadvm clean"));
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    g_assert_cmphex(result(s, RESULT_STEP), ==, 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    wait_steps(s, sc.nr_steps);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
    g_assert_cmpint(unlink(disk), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

/* A hit flush of the line, then its word, on a page no longer watched. */
static void hit_flush_released(Program *p)
{
    emit_hit_flush(p, 0x10);
    emit_publish(p, 0x18);
}

static void index_flush_released(Program *p)
{
    emit_flush(p);
    emit_publish(p, 0x18);
}

/*
 * A cache instruction acts on the processor's copy whether or not the
 * unit is watching the line's page: the dirty copy of a line whose page
 * lost its overlay is still found by a hit flush and written back, and so
 * is it by an index flush, and the word shows it at once.
 */
static void test_flush_of_a_released_line(void)
{
    const Scenario hit = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, evict },
              { 0, hit_flush_released }),
    };
    const Scenario index = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, evict },
              { 0, index_flush_released }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&hit, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x10) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
    s = run_scenario(&index, &prom);
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * Follow every line of @count pages after the one under test: name each
 * page's first line, read its word, which watches the page, and load each
 * of its 32 lines, which gives every line a copy the unit keeps.
 */
static void emit_fill_pages(Program *p, unsigned int count)
{
    const int pages = 14, lines = 15, line = 24;
    unsigned int page_loop, line_loop;

    emit(p, DADDU(R_T4, R_T1, R_ZERO));
    emit(p, DADDU(R_T5, R_T0, R_ZERO));
    emit(p, ORI(pages, R_ZERO, count));
    page_loop = p->pc;
    emit(p, DADDIU(R_T4, R_T4, 0x200));
    emit(p, DADDIU(R_T5, R_T5, 0x1000));
    emit(p, CACHE(0x17, 0, R_T5));
    emit(p, LD(R_T3, 0, R_T4));
    emit(p, ORI(lines, R_ZERO, 32));
    emit(p, DADDU(line, R_T5, R_ZERO));
    line_loop = p->pc;
    emit(p, LD(R_T3, 0, line));
    emit(p, DADDIU(line, line, 128));
    emit(p, DADDIU(lines, lines, 0xffff));
    emit_branch(p, 0x05, lines, R_ZERO, line_loop); /* bne */
    emit(p, DADDIU(pages, pages, 0xffff));
    emit_branch(p, 0x05, pages, R_ZERO, page_loop); /* bne */
}

/*
 * More lines followed than an index instruction's set has lines in all of
 * memory: 24 pages of 32 lines against 512 MiB over the R14000's 1 MiB set
 * span. The unit then finds the set's lines by address rather than by
 * walking every followed line, and must still find the dirty copy.
 */
static void fill_pages(Program *p)
{
    emit_fill_pages(p, 24);
}

static void test_index_flush_among_many_followed_lines(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, fill_pages },
              { 0, index_flush_released }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

/*
 * With directory ECC ignored a rewritten word keeps its old check field,
 * which may leave it not a codeword; the unit is not checking, so that
 * does not stop the next transition.
 */
static void test_ignore_ecc_keeps_transitions_going(void)
{
    const Scenario sc = {
        .cpus = "2", .ignore_ecc = true,
        STEPS({ 0, emit_first }, { 0, load_08 }, { 2, load_18 }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    finish(s, prom);
}

/* Read the word again without naming the line, load it, read it again. */
static void reread_then_load(Program *p)
{
    emit_publish(p, 0x10);
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x18);
}

/*
 * A naming is spent by the page it lets the unit watch: once that page
 * has been given up, reading the line's word again, as a later walk
 * would, does not watch it again until a processor names the line anew.
 */
static void test_naming_is_spent_on_watching(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, evict }, { 0, reread_then_load }),
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_scenario(&sc, &prom);
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, UNOWNED);
    finish(s, prom);
}

static void migrate_wait(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        QDict *response = qtest_qmp(qts, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response,
                                                           "return"),
                                           "status");
        bool done = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(response);
        if (done) {
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

/*
 * The copies of a line the unit no longer overlays are part of the
 * machine's state: a migration taken while the line is released carries
 * the dirty copy, and the destination writes it back when asked.
 */
static void test_released_line_state_migrates(void)
{
    const Scenario sc = {
        .cpus = "2",
        STEPS({ 0, emit_first }, { 0, store_08 }, { 0, evict },
              { 0, resume_after_release }),
    };
    GError *error = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *sock = NULL;
    g_autofree char *quoted = NULL;
    char *prom;
    QTestState *s, *to;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    directory = g_dir_make_tmp("sn1-directory-state-migration-XXXXXX",
                               &error);
    g_assert_no_error(error);
    sock = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", sock);
    s = start_scenario(&sc, &prom);
    quoted = g_shell_quote(prom);
    to = qtest_initf("-M origin300 -cpu R14000 -smp %s -m 512M -bios %s "
                     "-accel tcg,thread=multi -no-shutdown -S "
                     "-incoming defer", sc.cpus, quoted);
    wait_steps(s, 3);
    g_assert_true(flat_view_type_is(s, LINE_PHYS, "ram"));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(s,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(s);
    migrate_wait(to);
    qtest_quit(s);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_writeq(to, RESULT_PHYS + RESULT_GO, 1);
    wait_steps(to, sc.nr_steps);
    g_assert_cmphex(result(to, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    g_assert_cmphex(result(to, 0x18) & STATUS_CH, ==, STATUS_CH);
    g_assert_cmphex(result(to, 0x20) & DIRECTORY_FIELDS, ==, UNOWNED);
    qtest_quit(to);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_free(prom);
    unlink(sock);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/directory-state/loads-and-stores",
                   test_loads_and_stores);
    qtest_add_func("sn1/directory-state/three-readers", test_three_readers);
    qtest_add_func("sn1/directory-state/flushes", test_flushes);
    qtest_add_func("sn1/directory-state/full-pool-takes-the-oldest-slot",
                   test_full_pool_takes_the_oldest_slot);
    qtest_add_func("sn1/directory-state/prefetch", test_prefetch);
    qtest_add_func("sn1/directory-state/premium-format",
                   test_premium_format);
    qtest_add_func("sn1/directory-state/uncached-accesses",
                   test_uncached_accesses);
    qtest_add_func("sn1/directory-state/debugger-read", test_debugger_read);
    qtest_add_func("sn1/directory-state/absent-owner", test_absent_owner);
    qtest_add_func("sn1/directory-state/exclusive-algorithm-load",
                   test_exclusive_algorithm_load);
    qtest_add_func("sn1/directory-state/noncoherent-algorithm-load",
                   test_noncoherent_algorithm_load);
    qtest_add_func("sn1/directory-state/block-transfer-is-not-a-processor",
                   test_block_transfer_is_not_a_processor);
    qtest_add_func("sn1/directory-state/ignore-ecc-keeps-the-check-field",
                   test_ignore_ecc_keeps_the_check_field);
    qtest_add_func("sn1/directory-state/recently-read-page-stays-watched",
                   test_recently_read_page_stays_watched);
    qtest_add_func("sn1/directory-state/unnamed-line-is-not-watched",
                   test_unnamed_line_is_not_watched);
    qtest_add_func("sn1/directory-state/backdoor-write-does-not-watch",
                   test_backdoor_write_does_not_watch);
    qtest_add_func("sn1/directory-state/reset-forgets-watching",
                   test_reset_forgets_watching);
    qtest_add_func("sn1/directory-state/loadvm-forgets-watching",
                   test_loadvm_forgets_watching);
    qtest_add_func("sn1/directory-state/flush-of-a-released-line",
                   test_flush_of_a_released_line);
    qtest_add_func("sn1/directory-state/ignore-ecc-keeps-transitions-going",
                   test_ignore_ecc_keeps_transitions_going);
    qtest_add_func("sn1/directory-state/naming-is-spent-on-watching",
                   test_naming_is_spent_on_watching);
    qtest_add_func("sn1/directory-state/page-is-watched",
                   test_page_is_watched);
    qtest_add_func("sn1/directory-state/owner-from-the-word",
                   test_owner_from_the_word);
    qtest_add_func("sn1/directory-state/sharers-from-the-word",
                   test_sharers_from_the_word);
    qtest_add_func("sn1/directory-state/evicted-line-keeps-its-state",
                   test_evicted_line_keeps_its_state);
    qtest_add_func("sn1/directory-state/released-line-state-migrates",
                   test_released_line_state_migrates);
    qtest_add_func("sn1/directory-state/index-flush-among-many-followed-lines",
                   test_index_flush_among_many_followed_lines);
    return g_test_run();
}
