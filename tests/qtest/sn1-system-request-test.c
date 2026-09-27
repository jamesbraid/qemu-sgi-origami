/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 processor system requests racing other accesses to the node.
 *
 * A secondary-cache instruction or coherent prefetch reaches the node's
 * memory unit as a system request that has no memory access behind it. The
 * processor issues it from its own thread, outside the lock that serializes
 * device access, so it can run at the same moment as another processor's
 * ordinary register read on the same node, or another processor's system
 * request. Neither may cost the other anything: the register read must not
 * fault and no request may be lost.
 *
 * Both checks need guest code on two processors at once, so each case
 * boots a generated PROM on two processors under multi-threaded TCG.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define SN1_MD_MEMORY_CONFIG (SPACE_IO + UINT64_C(0x01800000) + 0x780008)
/* Software bank zero on the installed memory, as the stock PROM leaves it. */
#define SN1_MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

/* Lines the processors flush, one per slice, and their shared results. */
#define LINE_PHYS 0x580000
#define LINE_STRIDE 0x80
#define RESULT_PHYS 0x600000
#define RESULT_DONE(_slice) (0x00 + (_slice) * 8)
#define RESULT_SUM(_slice) (0x20 + (_slice) * 8)
#define RESULT_ERRORS 0x40
#define RESULT_CAUSE 0x48
#define RESULT_GO 0x50

/* The board identification register reads this on an Origin 300. */
#define BOARD_ID_VALUE UINT64_C(0x8001)

/* Iterations per processor; enough for the loops to overlap many times. */
#define REQUEST_LOOPS (1u << 20)
#define FOLLOWED_LOOPS (1u << 16)

#define POLL_TRIES 12000
#define POLL_INTERVAL_US 10000

/* Registers. */
#define R_ZERO 0
#define R_T0 8  /* this slice's line, cached */
#define R_T1 9  /* an uncached pointer */
#define R_T2 10 /* the result area, cached */
#define R_T3 11
#define R_T4 12
#define R_T5 13 /* loop count */
#define R_T6 14 /* sum */
#define R_K0 26
#define R_K1 27

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
#define XORI(_rt, _rs, _imm)    itype(0x0e, _rs, _rt, _imm)
#define ANDI(_rt, _rs, _imm)    itype(0x0c, _rs, _rt, _imm)
#define LW(_rt, _off, _base)    itype(0x23, _base, _rt, _off)
#define LD(_rt, _off, _base)    itype(0x37, _base, _rt, _off)
#define SD(_rt, _off, _base)    itype(0x3f, _base, _rt, _off)
#define CACHE(_op, _off, _base) itype(0x2f, _base, _op, _off)
#define DADDIU(_rt, _rs, _imm)  itype(0x19, _rs, _rt, _imm)
#define DSLL32(_rd, _rt)        rtype(0, _rt, _rd, 0, 0x3c)
#define DSRL32(_rd, _rt)        rtype(0, _rt, _rd, 0, 0x3e)
#define SRL(_rd, _rt, _sa)      rtype(0, _rt, _rd, _sa, 0x02)
#define DADDU(_rd, _rs, _rt)    rtype(_rs, _rt, _rd, 0, 0x2d)
#define MFC0(_rt, _rd)          (0x40000000 | ((_rt) << 16) | ((_rd) << 11))
#define DMFC0(_rt, _rd)         (0x40200000 | ((_rt) << 16) | ((_rd) << 11))
#define MTC0(_rt, _rd)          (0x40800000 | ((_rt) << 16) | ((_rd) << 11))
#define DMTC0(_rt, _rd)         (0x40a00000 | ((_rt) << 16) | ((_rd) << 11))
#define CP0_STATUS              12
#define CP0_CAUSE               13
#define CP0_EPC                 14
#define STATUS_CH_SHIFT         18
#define HIT_WRITEBACK_INVALIDATE_S 0x17
#define ERET                    0x42000018
#define SYNC                    0x0000000f
#define NOP                     0x00000000

typedef void (*EmitBody)(Program *p);

/* A branch to @target from the instruction at the current position. */
static void emit_branch(Program *p, unsigned op, int rs, int rt,
                        unsigned int target)
{
    int32_t offset = ((int32_t)target - (int32_t)(p->pc + 4)) / 4;

    emit(p, itype(op, rs, rt, (uint16_t)offset));
    emit(p, NOP);
}

/* Upper 32 bits @hi:@hi_low, then add the zero-extended 32-bit @lo. */
static void emit_address(Program *p, int reg, uint16_t hi, uint16_t hi_low,
                         uint32_t lo)
{
    emit(p, LUI(reg, hi));
    emit(p, ORI(reg, reg, hi_low));
    emit(p, DSLL32(reg, reg));
    emit(p, LUI(R_T3, lo >> 16));
    emit(p, ORI(R_T3, R_T3, lo & 0xffff));
    emit(p, DSLL32(R_T3, R_T3));
    emit(p, DSRL32(R_T3, R_T3));
    emit(p, DADDU(reg, reg, R_T3));
}

/* Load @count into the loop counter. */
static void emit_count(Program *p, unsigned int count)
{
    emit(p, LUI(R_T5, count >> 16));
    emit(p, ORI(R_T5, R_T5, count & 0xffff));
}

/* Count down and branch back to @top while the counter is nonzero. */
static void emit_loop_end(Program *p, unsigned int top)
{
    emit(p, DADDIU(R_T5, R_T5, -1));
    emit_branch(p, 0x05, R_T5, R_ZERO, top); /* bne t5, zero, top */
}

/* Publish the sum for @slice, then that it is done, and stop. */
static void emit_finish(Program *p, unsigned int slice)
{
    emit(p, SD(R_T6, RESULT_SUM(slice), R_T2));
    emit(p, ORI(R_T3, R_ZERO, 1));
    emit(p, SYNC);
    emit(p, SD(R_T3, RESULT_DONE(slice), R_T2));
    emit(p, 0x1000ffff); /* beq zero, zero, self */
    emit(p, NOP);
}

/*
 * Every processor runs the same image and finds its slice (A to D, 0 to 3)
 * in its CPU number, then runs @body[slice]. A general exception counts
 * itself, records its cause and resumes after the faulting instruction, so
 * a bus error on a load shows as a count and a short sum, not a hang.
 */
static char *create_prom(const EmitBody body[4])
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    Program p = { .image = image };
    char *path = NULL;
    int fd;

    emit(&p, itype(0x04, 0, 0, (CODE_START - 4) / 4)); /* b CODE_START */
    emit(&p, NOP);
    for (unsigned int offset = 0x200; offset < 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p.pc = 0x380;
    emit(&p, LD(R_K1, RESULT_ERRORS, R_T2));
    emit(&p, DADDIU(R_K1, R_K1, 1));
    emit(&p, SD(R_K1, RESULT_ERRORS, R_T2));
    emit(&p, MFC0(R_K1, CP0_CAUSE));
    emit(&p, SD(R_K1, RESULT_CAUSE, R_T2));
    emit(&p, DMFC0(R_K0, CP0_EPC));
    emit(&p, DADDIU(R_K0, R_K0, 4));
    emit(&p, DMTC0(R_K0, CP0_EPC));
    emit(&p, ERET);
    g_assert_cmpuint(p.pc, <=, CODE_START);

    p.pc = CODE_START;
    /*
     * 64-bit kernel addressing for the xkphys pointers below, and error
     * level off, so that ERET returns to EPC rather than ErrorEPC.
     */
    emit(&p, MFC0(R_T4, CP0_STATUS));
    emit(&p, ORI(R_T4, R_T4, 0x84));
    emit(&p, XORI(R_T4, R_T4, 0x04));
    emit(&p, MTC0(R_T4, CP0_STATUS));
    emit(&p, NOP);
    emit_address(&p, R_T2, 0xa800, 0, RESULT_PHYS);
    /* Which slice this processor is: bits 1:0 of its CPU number. */
    emit(&p, LUI(R_T4, 0x9200));
    emit(&p, DSLL32(R_T4, R_T4));
    emit(&p, LUI(R_T3, 0x0100));
    emit(&p, DADDU(R_T4, R_T4, R_T3));
    emit(&p, LD(R_T4, 0x20, R_T4));
    emit(&p, ANDI(R_T4, R_T4, 3));
    for (unsigned int slice = 0; slice < 4; slice++) {
        emit(&p, ORI(R_T3, R_ZERO, slice));
        emit_branch(&p, 0x04, R_T4, R_T3, 0x1000 + slice * 0x1000);
    }
    emit(&p, 0x1000ffff);
    emit(&p, NOP);

    for (unsigned int slice = 0; slice < 4; slice++) {
        g_assert_cmpuint(p.pc, <=, 0x1000 + slice * 0x1000);
        p.pc = 0x1000 + slice * 0x1000;
        emit_address(&p, R_T0, 0xa800, 0,
                     LINE_PHYS + slice * LINE_STRIDE);
        emit(&p, DADDU(R_T6, R_ZERO, R_ZERO));
        if (body[slice]) {
            body[slice](&p);
            emit_finish(&p, slice);
        } else {
            emit(&p, 0x1000ffff);
            emit(&p, NOP);
        }
    }

    fd = g_file_open_tmp("sn1-system-request-prom-XXXXXX.bin", &path,
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

/*
 * Start both processors, with further arguments @extra, and wait for
 * slices A and C to finish.
 */
static QTestState *run(const EmitBody body[4], const char *extra, char **prom)
{
    g_autofree char *quoted = NULL;
    QTestState *s;
    unsigned int tries;

    *prom = create_prom(body);
    quoted = g_shell_quote(*prom);
    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S %s", quoted,
                    extra ? extra : "");
    qtest_writeq(s, SN1_MD_MEMORY_CONFIG, SN1_MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (result(s, RESULT_DONE(0)) && result(s, RESULT_DONE(2))) {
            break;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmpuint(result(s, RESULT_DONE(0)), ==, 1);
    g_assert_cmpuint(result(s, RESULT_DONE(2)), ==, 1);
    return s;
}

static void finish(QTestState *s, char *prom)
{
    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
    g_free(prom);
}

/* Hit WriteBack Invalidate (S) this slice's line, over and over. */
static void flush_repeatedly(Program *p)
{
    unsigned int top;

    emit_count(p, REQUEST_LOOPS);
    top = p->pc;
    emit(p, CACHE(HIT_WRITEBACK_INVALIDATE_S, 0, R_T0));
    emit_loop_end(p, top);
}

/* Point t1 at the local board identification register, uncached. */
static void emit_board_id_address(Program *p)
{
    emit(p, LUI(R_T1, 0x9000));
    emit(p, DSLL32(R_T1, R_T1));
    emit(p, LUI(R_T3, 0x1400));
    emit(p, DADDU(R_T1, R_T1, R_T3));
}

/*
 * Read the local board identification register over and over, summing
 * what each read returns; a read that faults adds nothing.
 */
static void read_board_id_repeatedly(Program *p)
{
    unsigned int top;

    emit_board_id_address(p);
    emit_count(p, REQUEST_LOOPS);
    top = p->pc;
    emit(p, DADDU(R_T3, R_ZERO, R_ZERO));
    emit(p, LD(R_T3, 0, R_T1));
    emit(p, DADDU(R_T6, R_T6, R_T3));
    emit_loop_end(p, top);
}

/*
 * One processor's cache instructions run while the other reads a register
 * of the same node. The register read happens under the device lock; the
 * cache instruction does not, and must not make the node refuse the read.
 */
static void test_register_read_beside_requests(void)
{
    const EmitBody body[4] = {
        [0] = flush_repeatedly,
        [2] = read_board_id_repeatedly,
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run(body, NULL, &prom);
    g_assert_cmphex(result(s, RESULT_CAUSE), ==, 0);
    g_assert_cmpuint(result(s, RESULT_ERRORS), ==, 0);
    g_assert_cmphex(result(s, RESULT_SUM(2)), ==,
                    (uint64_t)REQUEST_LOOPS * BOARD_ID_VALUE);
    finish(s, prom);
}

/*
 * Dirty this slice's line, flush it with Hit WriteBack Invalidate (S) and
 * add Status.CH to the sum, over and over.
 */
static void emit_dirty_and_flush_loop(Program *p)
{
    unsigned int top;

    emit_count(p, FOLLOWED_LOOPS);
    top = p->pc;
    emit(p, SD(R_ZERO, 0, R_T0));
    emit(p, CACHE(HIT_WRITEBACK_INVALIDATE_S, 0, R_T0));
    emit(p, MFC0(R_T3, CP0_STATUS));
    emit(p, SRL(R_T3, R_T3, STATUS_CH_SHIFT));
    emit(p, ANDI(R_T3, R_T3, 1));
    emit(p, DADDU(R_T6, R_T6, R_T3));
    emit_loop_end(p, top);
}

/*
 * Name slice A's line and read its directory word, which has the memory
 * unit follow the line's page; then let slice C go, and loop.
 */
static void follow_page_then_dirty_and_flush(Program *p)
{
    /* Node zero's directory word for the line, through the uncached form. */
    emit_address(p, R_T1, 0x9000, 0x0001,
                 0xc0000000 | ((LINE_PHYS >> 3) & ~0xfu));
    emit(p, CACHE(HIT_WRITEBACK_INVALIDATE_S, 0, R_T0));
    emit(p, SYNC);
    emit(p, LD(R_T3, 0, R_T1));
    emit(p, ORI(R_T3, R_ZERO, 1));
    emit(p, SYNC);
    emit(p, SD(R_T3, RESULT_GO, R_T2));
    emit_dirty_and_flush_loop(p);
}

static void wait_then_dirty_and_flush(Program *p)
{
    unsigned int wait = p->pc;

    emit(p, LD(R_T3, RESULT_GO, R_T2));
    emit_branch(p, 0x04, R_T3, R_ZERO, wait); /* beq t3, zero, wait */
    emit_dirty_and_flush_loop(p);
}

/*
 * Both processors of a node dirty their own line on a followed page and
 * flush it with Hit WriteBack Invalidate (S), at the same time. Every flush
 * finds the dirty copy the store before it made, and says so in Status.CH,
 * only if the node served every request; a refused one reads as a miss.
 */
static void test_concurrent_requests_all_served(void)
{
    const EmitBody body[4] = {
        [0] = follow_page_then_dirty_and_flush,
        [2] = wait_then_dirty_and_flush,
    };
    char *prom;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    s = run(body, NULL, &prom);
    g_assert_cmphex(result(s, RESULT_CAUSE), ==, 0);
    g_assert_cmpuint(result(s, RESULT_ERRORS), ==, 0);
    g_assert_cmpuint(result(s, RESULT_SUM(0)), ==, FOLLOWED_LOOPS);
    g_assert_cmpuint(result(s, RESULT_SUM(2)), ==, FOLLOWED_LOOPS);
    finish(s, prom);
}

/* A four-byte read of the board ID, which answers only eight bytes. */
static void read_board_id_narrow(Program *p)
{
    emit_board_id_address(p);
    emit(p, LW(R_T3, 0, R_T1));
}

/*
 * An eight-byte read 0x0500_0000 above the board ID register, which the
 * IP35 address map leaves unmapped.
 */
static void read_past_board_id(Program *p)
{
    emit_board_id_address(p);
    emit(p, LUI(R_T3, 0x0500));
    emit(p, DADDU(R_T1, R_T1, R_T3));
    emit(p, LD(R_T3, 0, R_T1));
}

static void do_nothing(Program *p)
{
}

/*
 * Run @body0 on slice A with the unimplemented-access log on, check it took
 * one data bus error, and return the log's text.
 */
static char *run_logged_bus_error(EmitBody body0)
{
    const EmitBody body[4] = {
        [0] = body0,
        [2] = do_nothing,
    };
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;
    char *text = NULL;
    char *prom;
    QTestState *s;
    int fd;

    fd = g_file_open_tmp("sn1-system-request-log-XXXXXX", &log, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    extra = g_strdup_printf("-d unimp -D %s", log);
    s = run(body, extra, &prom);
    g_assert_cmphex(result(s, RESULT_CAUSE) & 0x7c, ==, 7 << 2);
    g_assert_cmpuint(result(s, RESULT_ERRORS), ==, 1);
    finish(s, prom);
    g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_cmpint(unlink(log), ==, 0);
    return text;
}

/*
 * The bus error a device's refusal raises names the result, a device
 * error. The log said the access "reached no device" for every failure,
 * which sent the investigation of the refused board ID reads above looking
 * for a hole in the address map.
 */
static void test_refused_read_is_named(void)
{
    g_autofree char *text = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    text = run_logged_bus_error(read_board_id_narrow);
    g_assert_nonnull(strstr(text, "mips: load of 4 bytes at "
                                  "0x9000000014000000 (physical "
                                  "0x10014000000) failed with a device "
                                  "error; raising a bus error"));
    g_assert_null(strstr(text, "decode error"));
}

/* A read of an unmapped address names its result, a decode error. */
static void test_unmapped_read_is_named(void)
{
    g_autofree char *text = NULL;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    text = run_logged_bus_error(read_past_board_id);
    g_assert_nonnull(strstr(text, "mips: load of 8 bytes at "
                                  "0x9000000019000000 (physical "
                                  "0x10019000000) failed with a decode "
                                  "error; raising a bus error"));
    g_assert_null(strstr(text, "device error"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/system-request/register-read-beside-requests",
                   test_register_read_beside_requests);
    qtest_add_func("sn1/system-request/concurrent-requests-all-served",
                   test_concurrent_requests_all_served);
    qtest_add_func("sn1/system-request/refused-read-is-named",
                   test_refused_read_is_named);
    qtest_add_func("sn1/system-request/unmapped-read-is-named",
                   test_unmapped_read_is_named);
    return g_test_run();
}
