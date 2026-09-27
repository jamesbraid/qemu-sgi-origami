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
#include "sgi-test.h"

#define SN1_MD_MEMORY_CONFIG (SPACE_XTALK + UINT64_C(0x01800000) + 0x780008)
/* Software bank zero on the installed memory, as the stock PROM leaves it. */
#define SN1_MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

/* The line under test and where the processors publish what they read. */
#define LINE_PHYS 0x580000
#define RESULT_PHYS 0x600000
#define RESULT_A_DONE 0x100
#define RESULT_C_DONE 0x108
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

/*
 * Both processors run the same image. Slot A runs @slot_a and raises its
 * done word; slot C waits for that word, runs @slot_c and raises its own.
 */
static char *create_prom(EmitBody slot_a, EmitBody slot_c)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    Program p = { .image = image };
    unsigned int wait, slice_c = 0x800;
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
    /* Which slot this processor is in: the PI bit of its CPU number. */
    emit(&p, LUI(R_T4, 0x9200));
    emit(&p, DSLL32(R_T4, R_T4));
    emit(&p, LUI(R_T3, 0x0100));
    emit(&p, DADDU(R_T4, R_T4, R_T3));
    emit(&p, LD(R_T4, 0x20, R_T4));
    emit(&p, ANDI(R_T4, R_T4, 2));
    emit_branch(&p, 0x05, R_T4, R_ZERO, slice_c); /* bne t4, zero */

    emit_publish(&p, 0x00); /* before any request */
    slot_a(&p);
    emit(&p, ORI(R_T3, R_ZERO, 1));
    emit(&p, SYNC);
    emit(&p, SD(R_T3, RESULT_A_DONE, R_T2));
    emit(&p, 0x1000ffff);
    emit(&p, NOP);

    g_assert_cmpuint(p.pc, <=, slice_c);
    p.pc = slice_c;
    wait = p.pc;
    emit(&p, LD(R_T3, RESULT_A_DONE, R_T2));
    emit_branch(&p, 0x04, R_T3, R_ZERO, wait); /* beq t3, zero, wait */
    slot_c(&p);
    emit(&p, ORI(R_T3, R_ZERO, 1));
    emit(&p, SYNC);
    emit(&p, SD(R_T3, RESULT_C_DONE, R_T2));
    emit(&p, 0x1000ffff);
    emit(&p, NOP);

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

/* Run both processors through @slot_a then @slot_c; the caller checks. */
static QTestState *run_prom(EmitBody slot_a, EmitBody slot_c, char **prom)
{
    g_autofree char *quoted = NULL;
    QTestState *s;
    unsigned int tries;

    *prom = create_prom(slot_a, slot_c);
    quoted = g_shell_quote(*prom);
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", quoted);
    qtest_writeq(s, SN1_MD_MEMORY_CONFIG, SN1_MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (result(s, RESULT_C_DONE) || result(s, RESULT_EPC)) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(result(s, RESULT_EPC), ==, 0);
    g_assert_cmphex(result(s, RESULT_A_DONE), ==, 1);
    g_assert_cmphex(result(s, RESULT_C_DONE), ==, 1);
    /* Reading the word through the backdoor is what starts following it. */
    g_assert_cmphex(result(s, 0x00) & DIRECTORY_FIELDS, ==, UNOWNED);
    return s;
}

static void finish(QTestState *s, char *prom)
{
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_free(prom);
}

static void emit_nothing(Program *p)
{
}

static void emit_loads_and_stores_a(Program *p)
{
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x08);
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_publish(p, 0x10);
}

static void emit_loads_and_stores_c(Program *p)
{
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x18);
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_publish(p, 0x20);
}

static void test_loads_and_stores(void)
{
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_prom(emit_loads_and_stores_a, emit_loads_and_stores_c, &prom);
    /* A read miss on an unowned line grants it exclusive to the reader. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* A store to a line the processor holds exclusive sends nothing. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* Slot C's read of A's line shares it between their interfaces. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, SHARED(0x3));
    /* Its store upgrades its shared copy, which takes the line exclusive. */
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, POINTER(4));
    finish(s, prom);
}

static void emit_flushes_a(Program *p)
{
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_flush(p);
    emit_publish(p, 0x08);
    emit(p, LD(R_T3, 0, R_T0));
    emit_flush(p);
    emit_publish(p, 0x10);
    emit(p, SD(R_ZERO, 0, R_T0));
    emit(p, SYNC);
    emit(p, CACHE(0x17, 0, R_T0)); /* Hit WriteBack Invalidate (S) */
    emit_publish(p, 0x18);
}

/*
 * Read the directory words of sixteen other lines, as a walk over the
 * directory does, which fills the unit's slots; then store to, flush and
 * check a line 128 KiB above the one under test, which the walk left out.
 */
static void emit_busy_a(Program *p)
{
    emit(p, DADDU(R_T4, R_T1, R_ZERO));
    for (unsigned int i = 0; i < 16; i++) {
        emit(p, DADDIU(R_T4, R_T4, 0x200));
        emit(p, LD(R_T3, 0, R_T4));
    }
    emit(p, LUI(R_T3, 0x0002));
    emit(p, DADDU(R_T0, R_T0, R_T3));
    emit(p, DADDIU(R_T1, R_T1, 0x4000));
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_flush(p);
    emit_publish(p, 0x08);
    emit(p, LD(R_T3, 0, R_T0));
    emit_publish(p, 0x10);
}

static void emit_flushes_c(Program *p)
{
    emit(p, LD(R_T3, 0, R_T0));
    emit_flush(p);
    emit_publish(p, 0x20);
}

static void test_flushes(void)
{
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_prom(emit_flushes_a, emit_flushes_c, &prom);
    /* The index flush writes the dirty line back, which unowns it. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* A clean exclusive line is dropped without telling the directory. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* The hit form writes back as the index form does. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, UNOWNED);
    /* Slot C's clean copy, flushed, leaves C the owner. */
    g_assert_cmphex(result(s, 0x20) & DIRECTORY_FIELDS, ==, POINTER(4));
    finish(s, prom);
}

/*
 * The unit follows only a few lines, and reuses a slot only once it has
 * been idle for a while; a line a processor has just flushed is followed
 * even when every slot is busy.
 */
static void test_flushed_line_when_busy(void)
{
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_prom(emit_busy_a, emit_nothing, &prom);
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, UNOWNED);
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

static void emit_prefetch_a(Program *p)
{
    emit(p, PREF(6, 0, R_T0));
    emit_publish(p, 0x08);
    emit(p, SD(R_ZERO, 0, R_T0));
    emit_publish(p, 0x10);
    emit_flush(p);
    emit(p, PREF(1, 0, R_T0));
    emit_publish(p, 0x18);
}

static void test_prefetch(void)
{
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run_prom(emit_prefetch_a, emit_nothing, &prom);
    /* A prefetch with a load hint reads the line shared. */
    g_assert_cmphex(result(s, 0x08) & DIRECTORY_FIELDS, ==, SHARED(0x1));
    /* A store to the shared copy upgrades it. */
    g_assert_cmphex(result(s, 0x10) & DIRECTORY_FIELDS, ==, POINTER(0));
    /* A prefetch with a store hint reads the line exclusive. */
    g_assert_cmphex(result(s, 0x18) & DIRECTORY_FIELDS, ==, POINTER(0));
    finish(s, prom);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/directory-state/loads-and-stores",
                   test_loads_and_stores);
    qtest_add_func("sn1/directory-state/flushes", test_flushes);
    qtest_add_func("sn1/directory-state/flushed-line-when-busy",
                   test_flushed_line_when_busy);
    qtest_add_func("sn1/directory-state/prefetch", test_prefetch);
    return g_test_run();
}
