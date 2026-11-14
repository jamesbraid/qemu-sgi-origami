/*
 * Register-contract tests for the sgi-l1-board machine.
 *
 * Each test builds a 1 MiB boot flash image that holds test-owned ColdFire
 * code, starts the machine with it as -bios, lets the code run and reads
 * the results it left in the on-chip SRAM. No vendor image is used.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define FLASH_SIZE      (1024 * 1024)
#define FLASH_BASE      0xffe00000u
#define MAIN_OFFSET     0x400
#define HANDLER_OFFSET  0x800
#define HANDLER2_OFFSET 0x900
#define FLASH_PATTERN   0x40

#define SRAM            0x30100000u
#define VBR_ADDR        SRAM
#define RESULT(n)       (SRAM + 0x800 + 4 * (n))
#define DONE            (SRAM + 0xff0)
#define MBAR_BASE       0x10000000u
#define STACK           0x30101ff0u

#define CR_VBR          0x801
#define CR_RAMBAR       0xc04
#define CR_MBAR         0xc0f

/*
 * Counters and the done flag carry this tag. Until the program maps its SRAM
 * the global chip select answers the same addresses with flash contents, so
 * an untagged value is not a result.
 */
#define TAG             0x5a5a0000u

#define RUN_TIMEOUT_US  (20 * G_USEC_PER_SEC)

/* MBAR-relative registers. */
#define SIMR            0x03
#define ICR(n)          (0x13 + (n))
#define IMR             0x36
#define IPR             0x3a
#define RSR             0x40
#define SYPCR           0x41
#define SWIVR           0x42
#define SWSR            0x43
#define CSAR(n)         (0x64 + 12 * (n))
#define CSMR(n)         (0x68 + 12 * (n))
#define CSCR(n)         (0x6e + 12 * (n))
#define DMCR            0xc6
#define PAR             0xca
#define TMR(n)          (0x100 + 0x20 * (n))
#define TRR(n)          (0x104 + 0x20 * (n))
#define TCN(n)          (0x10c + 0x20 * (n))
#define TER(n)          (0x111 + 0x20 * (n))

typedef struct Asm {
    uint8_t buf[1024];
    size_t len;
} Asm;

static void emit16(Asm *a, uint16_t v)
{
    a->buf[a->len++] = v >> 8;
    a->buf[a->len++] = v;
}

static void emit32(Asm *a, uint32_t v)
{
    emit16(a, v >> 16);
    emit16(a, v);
}

/* move.l #imm,Dn */
static void movei_d(Asm *a, int n, uint32_t imm)
{
    emit16(a, 0x203c | (n << 9));
    emit32(a, imm);
}

/* move.l Dn,(addr).l */
static void st_l(Asm *a, int n, uint32_t addr)
{
    emit16(a, 0x23c0 | n);
    emit32(a, addr);
}

/* move.w Dn,(addr).l */
static void st_w(Asm *a, int n, uint32_t addr)
{
    emit16(a, 0x33c0 | n);
    emit32(a, addr);
}

/* move.b Dn,(addr).l */
static void st_b(Asm *a, int n, uint32_t addr)
{
    emit16(a, 0x13c0 | n);
    emit32(a, addr);
}

/* move.l (addr).l,Dn */
static void ld_l(Asm *a, uint32_t addr, int n)
{
    emit16(a, 0x2039 | (n << 9));
    emit32(a, addr);
}

/* move.w (addr).l,Dn, after clearing Dn */
static void ld_w(Asm *a, uint32_t addr, int n)
{
    movei_d(a, n, 0);
    emit16(a, 0x3039 | (n << 9));
    emit32(a, addr);
}

/* move.b (addr).l,Dn, after clearing Dn */
static void ld_b(Asm *a, uint32_t addr, int n)
{
    movei_d(a, n, 0);
    emit16(a, 0x1039 | (n << 9));
    emit32(a, addr);
}

static void movec_d(Asm *a, int n, int cr)
{
    emit16(a, 0x4e7b);
    emit16(a, (n << 12) | cr);
}

static void move_sr_imm(Asm *a, uint16_t sr)
{
    emit16(a, 0x46fc);
    emit16(a, sr);
}

static void move_sr_d(Asm *a, int n)
{
    emit16(a, 0x40c0 | n);
}

static void tst_l(Asm *a, int n)
{
    emit16(a, 0x4a80 | n);
}

static void addq1_l(Asm *a, int n)
{
    emit16(a, 0x5280 | n);
}

static void rte(Asm *a)
{
    emit16(a, 0x4e73);
}

static void bra_self(Asm *a)
{
    emit16(a, 0x60fe);
}

static void bra_to(Asm *a, size_t target)
{
    emit16(a, 0x6000);
    emit16(a, target - a->len);
}

/* beq.w to be patched; returns the extension word's offset. */
static size_t beq_w(Asm *a)
{
    emit16(a, 0x6700);
    emit16(a, 0);
    return a->len - 2;
}

static void patch_here(Asm *a, size_t at)
{
    uint16_t d = a->len - at;

    a->buf[at] = d >> 8;
    a->buf[at + 1] = d;
}

/*
 * Spin until some of the mask bits read back from a word or byte register
 * are set, leaving them in d1. The chip's timers run on the host clock
 * and are serviced by a different thread from the one running the guest,
 * so a loop of a fixed length proves nothing about whether an event has
 * happened yet; waiting for the bit itself does not depend on host load.
 */
static void poll_bits(Asm *a, uint32_t addr, uint32_t mask, bool byte)
{
    size_t loop = a->len;

    if (byte) {
        ld_b(a, addr, 1);
    } else {
        ld_w(a, addr, 1);
    }
    emit16(a, 0x0281);         /* andi.l #mask,d1 */
    emit32(a, mask);
    emit16(a, 0x6700);         /* beq.w back to the read */
    emit16(a, loop - a->len);
}

/* Store an immediate through d0. */
static void put_l(Asm *a, uint32_t addr, uint32_t val)
{
    movei_d(a, 0, val);
    st_l(a, 0, addr);
}

static void put_w(Asm *a, uint32_t addr, uint32_t val)
{
    movei_d(a, 0, val);
    st_w(a, 0, addr);
}

static void put_b(Asm *a, uint32_t addr, uint32_t val)
{
    movei_d(a, 0, val);
    st_b(a, 0, addr);
}

static void prologue(Asm *a)
{
    movei_d(a, 0, SRAM | 1);
    movec_d(a, 0, CR_RAMBAR);
}

static void mbar_on(Asm *a)
{
    movei_d(a, 0, MBAR_BASE | 1);
    movec_d(a, 0, CR_MBAR);
}

static void finish(Asm *a)
{
    put_l(a, DONE, TAG | 1);
    bra_self(a);
}

/* Program a chip select the way the board's firmware does. */
static void cs_setup(Asm *a, int n, uint16_t csar, uint32_t csmr,
                     uint16_t cscr)
{
    put_w(a, MBAR_BASE + CSAR(n), csar);
    put_w(a, MBAR_BASE + CSCR(n), cscr);
    put_l(a, MBAR_BASE + CSMR(n), csmr);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

typedef struct Flash {
    char *dir;
    char *path;
} Flash;

/* Write the flash image: vectors, a pattern the tests read, the code. */
static Flash flash_build(const Asm *main_code, const Asm *handler,
                         const Asm *handler2)
{
    Flash f;
    uint8_t *img = g_malloc(FLASH_SIZE);
    GError *err = NULL;

    memset(img, 0xff, FLASH_SIZE);
    put_be32(img, STACK);
    put_be32(img + 4, FLASH_BASE + MAIN_OFFSET);
    img[FLASH_PATTERN] = 0xa5;
    img[FLASH_PATTERN + 1] = 0x5a;
    memcpy(img + MAIN_OFFSET, main_code->buf, main_code->len);
    if (handler) {
        memcpy(img + HANDLER_OFFSET, handler->buf, handler->len);
    }
    if (handler2) {
        memcpy(img + HANDLER2_OFFSET, handler2->buf, handler2->len);
    }
    f.dir = g_dir_make_tmp("sgi-l1-board-XXXXXX", &err);
    g_assert_no_error(err);
    f.path = g_build_filename(f.dir, "flash.bin", NULL);
    g_assert_true(g_file_set_contents(f.path, (char *)img, FLASH_SIZE, &err));
    g_assert_no_error(err);
    g_free(img);
    return f;
}

static void flash_free(Flash *f)
{
    g_unlink(f->path);
    g_rmdir(f->dir);
    g_free(f->path);
    g_free(f->dir);
}

static QTestState *start_machine(const Flash *f, const char *machine,
                                 const char *extra)
{
    return qtest_initf("-machine %s -bios %s -accel tcg -S %s",
                       machine, f->path, extra);
}

static QTestState *start(const Flash *f, const char *extra)
{
    return start_machine(f, "sgi-l1-board", extra);
}

static void go(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
}

static void wait_for(QTestState *qts, uint32_t addr, uint32_t at_least)
{
    gint64 end = g_get_monotonic_time() + RUN_TIMEOUT_US;
    uint32_t v;

    while (((v = qtest_readl(qts, addr)) & 0xffff0000) != TAG ||
           (v & 0xffff) < at_least) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
}

/* Run a main program alone and wait for it to finish. */
static QTestState *run_main(Flash *f, const Asm *code, const char *extra)
{
    QTestState *qts;

    *f = flash_build(code, NULL, NULL);
    qts = start(f, extra);
    go(qts);
    wait_for(qts, DONE, 1);
    return qts;
}

static void quit_all(QTestState *qts, Flash *f)
{
    qtest_quit(qts);
    flash_free(f);
}

/*
 * Until CSMR0 is written chip select 0 answers every address, so the reset
 * vectors and a read of the module block's address come from the flash. Once
 * MBAR is valid the same address reads the register.
 */
static void test_global_cs0(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    ld_b(&a, MBAR_BASE + RSR, 1);
    st_l(&a, 1, RESULT(0));
    mbar_on(&a);
    ld_b(&a, MBAR_BASE + RSR, 1);
    st_l(&a, 1, RESULT(1));
    finish(&a);
    qts = run_main(&f, &a, "");

    /* The core took its stack and entry from flash through the global CS0. */
    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0xa5);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0x80);
    quit_all(qts, &f);
}

/*
 * The first CSMR0 write ends the global select; the programmed window
 * takes over. A 1 MiB flash repeats through a 2 MiB window, and writes to
 * the flash are ignored.
 */
static void test_cs0_window(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    ld_w(&a, MBAR_BASE + CSCR(0), 1);
    st_l(&a, 1, RESULT(0));
    ld_l(&a, MBAR_BASE + CSMR(0), 1);
    st_l(&a, 1, RESULT(1));
    cs_setup(&a, 0, 0xffe0, 0x001f0000, 0x1183);
    ld_l(&a, FLASH_BASE, 1);
    st_l(&a, 1, RESULT(2));
    ld_l(&a, FLASH_BASE + 0x100000, 1);
    st_l(&a, 1, RESULT(3));
    put_l(&a, FLASH_BASE + 0x800, 0xdeadbeef);
    ld_l(&a, FLASH_BASE + 0x800, 1);
    st_l(&a, 1, RESULT(4));
    /* No window covers this address: default memory reads as zero. */
    ld_l(&a, 0x70000000, 1);
    st_l(&a, 1, RESULT(5));
    ld_w(&a, MBAR_BASE + CSAR(0), 1);
    st_l(&a, 1, RESULT(6));
    finish(&a);
    qts = run_main(&f, &a, "");

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x3d9f);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RESULT(2)), ==, STACK);
    g_assert_cmphex(qtest_readl(qts, RESULT(3)), ==, STACK);
    g_assert_cmphex(qtest_readl(qts, RESULT(4)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, RESULT(5)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RESULT(6)), ==, 0xffe0);
    quit_all(qts, &f);
}

/*
 * The external RAM size probe, on the 512 KiB part and the aliased 128 KiB.
 * A marker word stored in the first 128 KiB is read back at the same offset
 * in each following 128 KiB: on the aliased part every copy of the window
 * shows it, so each alias copy has to be present, and on the 512 KiB part
 * the same offsets are separate, untouched RAM.
 */
static void ram_probe(const char *extra, uint32_t expect_alias,
                      uint32_t expect_copies)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    int i;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    cs_setup(&a, 2, 0x3000, 0x00070000, 0x0983);
    put_l(&a, 0x30000000, 0x12345678);
    ld_l(&a, 0x30000000, 1);
    st_l(&a, 1, RESULT(0));
    put_l(&a, 0x3001fffc, 0);
    put_l(&a, 0x3007fffc, 0xffffffff);
    ld_l(&a, 0x3001fffc, 1);
    st_l(&a, 1, RESULT(1));
    put_l(&a, 0x30000010, 0xa5a50001);
    for (i = 1; i < 4; i++) {
        ld_l(&a, 0x30000010 + i * 0x20000, 1);
        st_l(&a, 1, RESULT(1 + i));
    }
    finish(&a);
    qts = run_main(&f, &a, extra);

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, expect_alias);
    for (i = 1; i < 4; i++) {
        g_assert_cmphex(qtest_readl(qts, RESULT(1 + i)), ==, expect_copies);
    }
    quit_all(qts, &f);
}

static void test_ram_512k(void)
{
    ram_probe("", 0, 0);
}

static void test_ram_128k_aliased(void)
{
    ram_probe("-m 128K", 0xffffffff, 0xa5a50001);
}

/* The clock RAM at CS1 holds bytes. */
static void test_nvram(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    cs_setup(&a, 1, 0x2000, 0, 0x1143);
    put_b(&a, 0x20000010, 0x5a);
    put_b(&a, 0x20000011, 0xa5);
    ld_b(&a, 0x20000010, 1);
    st_l(&a, 1, RESULT(0));
    ld_b(&a, 0x20000011, 1);
    st_l(&a, 1, RESULT(1));
    finish(&a);
    qts = run_main(&f, &a, "");

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x5a);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0xa5);
    quit_all(qts, &f);
}

/* Reset values and write rules of the system integration module. */
static void test_sim_registers(void)
{
    static const struct {
        uint32_t off;
        int width;
        uint32_t expect;
    } reset[] = {
        { SIMR, 1, 0xc0 }, { ICR(1), 1, 0x04 }, { ICR(2), 1, 0x08 },
        { ICR(3), 1, 0x0c }, { ICR(4), 1, 0x10 }, { ICR(5), 1, 0x14 },
        { ICR(6), 1, 0x18 }, { ICR(7), 1, 0x1c }, { ICR(8), 1, 0x1c },
        { ICR(9), 1, 0x80 }, { ICR(10), 1, 0x80 }, { ICR(11), 1, 0x80 },
        { ICR(12), 1, 0x00 }, { ICR(13), 1, 0x00 }, { IMR, 2, 0x3ffe },
        { IPR, 2, 0x0000 }, { RSR, 1, 0x80 }, { SYPCR, 1, 0x00 },
        { SWIVR, 1, 0x0f }, { CSAR(0), 2, 0x0000 }, { CSMR(0), 4, 0 },
        { DMCR, 2, 0x0000 }, { PAR, 2, 0x0000 }, { TMR(0), 2, 0x0000 },
        { TRR(0), 2, 0xffff }, { TCN(0), 2, 0x0000 }, { TER(0), 1, 0x00 },
        { TMR(1), 2, 0x0000 }, { TRR(1), 2, 0xffff },
    };
    Asm a = { };
    Flash f;
    QTestState *qts;
    int n = ARRAY_SIZE(reset);
    int i;

    prologue(&a);
    mbar_on(&a);
    for (i = 0; i < n; i++) {
        if (reset[i].width == 1) {
            ld_b(&a, MBAR_BASE + reset[i].off, 1);
        } else if (reset[i].width == 2) {
            ld_w(&a, MBAR_BASE + reset[i].off, 1);
        } else {
            ld_l(&a, MBAR_BASE + reset[i].off, 1);
        }
        st_l(&a, 1, RESULT(i));
    }
    finish(&a);
    qts = run_main(&f, &a, "");
    for (i = 0; i < n; i++) {
        g_assert_cmphex(qtest_readl(qts, RESULT(i)), ==, reset[i].expect);
    }
    quit_all(qts, &f);
}

/* Write rules: fixed levels, write-once SYPCR, write-one-to-clear RSR. */
static void test_sim_write_rules(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    /* The level of an external source cannot be changed. */
    put_b(&a, MBAR_BASE + ICR(3), 0x80);
    ld_b(&a, MBAR_BASE + ICR(3), 1);
    st_l(&a, 1, RESULT(0));
    put_b(&a, MBAR_BASE + ICR(9), 0x94);
    ld_b(&a, MBAR_BASE + ICR(9), 1);
    st_l(&a, 1, RESULT(1));
    /* SYPCR keeps the first value written. */
    put_b(&a, MBAR_BASE + SYPCR, 0x06);
    put_b(&a, MBAR_BASE + SYPCR, 0x07);
    ld_b(&a, MBAR_BASE + SYPCR, 1);
    st_l(&a, 1, RESULT(2));
    put_b(&a, MBAR_BASE + RSR, 0x80);
    ld_b(&a, MBAR_BASE + RSR, 1);
    st_l(&a, 1, RESULT(3));
    /*
     * A byte write to one half of the 16-bit mask keeps the other half, and
     * bit 0 is reserved so it stays clear.
     */
    put_w(&a, MBAR_BASE + IMR, 0x1234);
    put_b(&a, MBAR_BASE + IMR + 1, 0xff);
    ld_w(&a, MBAR_BASE + IMR, 1);
    st_l(&a, 1, RESULT(4));
    finish(&a);
    qts = run_main(&f, &a, "");

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x8c);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0x94);
    g_assert_cmphex(qtest_readl(qts, RESULT(2)), ==, 0x06);
    g_assert_cmphex(qtest_readl(qts, RESULT(3)), ==, 0x00);
    g_assert_cmphex(qtest_readl(qts, RESULT(4)), ==, 0x12fe);
    quit_all(qts, &f);
}

/* Set VBR to the SRAM and point a vector at a flash handler. */
static void vector_to(Asm *a, int vec, uint32_t handler_offset)
{
    movei_d(a, 0, VBR_ADDR);
    movec_d(a, 0, CR_VBR);
    put_l(a, VBR_ADDR + 4 * vec, FLASH_BASE + handler_offset);
}

/* Timer 1 interrupts at level 5 through the interrupt controller. */
static void test_timer_interrupt(void)
{
    Asm a = { }, h = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    vector_to(&a, 29, HANDLER_OFFSET);
    put_l(&a, RESULT(2), TAG);
    put_b(&a, MBAR_BASE + ICR(9), 0x94);
    put_w(&a, MBAR_BASE + IMR, 0x3dfe);
    put_w(&a, MBAR_BASE + TMR(0), 0);
    put_w(&a, MBAR_BASE + TRR(0), 999);
    put_w(&a, MBAR_BASE + TMR(0), 0x1d);
    move_sr_imm(&a, 0x2000);
    bra_self(&a);

    /* Record the SR on entry and IPR, count, acknowledge, return. */
    move_sr_d(&h, 1);
    st_l(&h, 1, RESULT(0));
    ld_w(&h, MBAR_BASE + IPR, 1);
    st_l(&h, 1, RESULT(1));
    put_b(&h, MBAR_BASE + TER(0), 0x02);
    ld_l(&h, RESULT(2), 1);
    addq1_l(&h, 1);
    st_l(&h, 1, RESULT(2));
    rte(&h);

    f = flash_build(&a, &h, NULL);
    qts = start(&f, "");
    go(qts);
    wait_for(qts, RESULT(2), 3);

    /* The interrupt mask rose to the interrupt's level, the source set. */
    g_assert_cmphex(qtest_readl(qts, RESULT(0)) & 0x0700, ==, 0x0500);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)) & 0x0200, ==, 0x0200);
    quit_all(qts, &f);
}

/* A masked source stays pending but does not interrupt. */
static void test_masked_interrupt(void)
{
    Asm a = { }, h = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    vector_to(&a, 29, HANDLER_OFFSET);
    put_b(&a, MBAR_BASE + ICR(9), 0x94);
    /* IMR keeps the timer 1 bit set. */
    put_w(&a, MBAR_BASE + TMR(0), 0);
    put_w(&a, MBAR_BASE + TRR(0), 99);
    put_w(&a, MBAR_BASE + TMR(0), 0x1d);
    move_sr_imm(&a, 0x2000);
    /*
     * Wait until the source is pending, then keep running for a while so
     * that an interrupt wrongly delivered would have been taken.
     */
    poll_bits(&a, MBAR_BASE + IPR, 0x0200, false);
    st_l(&a, 1, RESULT(0));
    movei_d(&a, 2, 200000);
    emit16(&a, 0x5382 | 0);    /* subq.l #1,d2 */
    emit16(&a, 0x66fc);        /* bne.s back to the subq */
    ld_b(&a, MBAR_BASE + TER(0), 1);
    st_l(&a, 1, RESULT(1));
    finish(&a);

    put_l(&h, RESULT(2), 1);
    rte(&h);

    f = flash_build(&a, &h, NULL);
    qts = start(&f, "");
    go(qts);
    wait_for(qts, DONE, 1);
    g_assert_cmphex(qtest_readl(qts, RESULT(0)) & 0x0200, ==, 0x0200);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0x02);
    g_assert_cmphex(qtest_readl(qts, RESULT(2)), ==, 0);
    quit_all(qts, &f);
}

/*
 * A timer that does not interrupt still reports its reference event, and
 * a mode write with the reset bit cleared restores the reset values.
 */
static void test_timer_event_and_reset(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    put_w(&a, MBAR_BASE + TMR(1), 0);
    put_w(&a, MBAR_BASE + TRR(1), 1);
    put_w(&a, MBAR_BASE + TMR(1), 0x2b);
    poll_bits(&a, MBAR_BASE + TER(1), 0x02, true);
    st_l(&a, 1, RESULT(0));
    put_b(&a, MBAR_BASE + TER(1), 0x02);
    ld_w(&a, MBAR_BASE + TRR(1), 1);
    st_l(&a, 1, RESULT(1));
    put_w(&a, MBAR_BASE + TMR(1), 0);
    ld_w(&a, MBAR_BASE + TRR(1), 1);
    st_l(&a, 1, RESULT(2));
    ld_w(&a, MBAR_BASE + TMR(1), 1);
    st_l(&a, 1, RESULT(3));
    finish(&a);
    qts = run_main(&f, &a, "");

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x02);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 1);
    g_assert_cmphex(qtest_readl(qts, RESULT(2)), ==, 0xffff);
    g_assert_cmphex(qtest_readl(qts, RESULT(3)), ==, 0);
    quit_all(qts, &f);
}

/* The watchdog resets the chip, RSR then names it, and SRAM survives. */
static void test_watchdog_reset(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    size_t first;

    prologue(&a);
    mbar_on(&a);
    ld_l(&a, RESULT(7), 1);
    tst_l(&a, 1);
    first = beq_w(&a);
    ld_b(&a, MBAR_BASE + RSR, 1);
    st_l(&a, 1, RESULT(0));
    finish(&a);
    patch_here(&a, first);
    put_l(&a, RESULT(7), 1);
    put_b(&a, MBAR_BASE + SYPCR, 0xfe);
    bra_self(&a);
    f = flash_build(&a, NULL, NULL);
    qts = start(&f, "");
    go(qts);
    wait_for(qts, DONE, 1);

    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0x20);
    quit_all(qts, &f);
}

/*
 * A serviced watchdog does not reset the chip. The machine runs on the
 * instruction-count clock, so model time moves only as the program runs
 * and a host that stalls the vCPU thread cannot starve the watchdog.
 * Timer 1 is the witness that time has passed: it counts 4096 steps in
 * one 0.35 s watchdog period, and the program copies it to the SRAM while
 * it services the watchdog. The program has survived three periods when
 * the copy reaches three times that.
 */
static void test_watchdog_service(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    size_t loop, first;
    gint64 end;
    uint32_t steps;

    prologue(&a);
    mbar_on(&a);
    ld_l(&a, RESULT(7), 1);
    tst_l(&a, 1);
    first = beq_w(&a);
    /* A second start: the chip was reset. */
    put_l(&a, RESULT(6), 1);
    bra_self(&a);
    patch_here(&a, first);
    put_l(&a, RESULT(7), 1);
    put_l(&a, RESULT(5), TAG);
    put_w(&a, MBAR_BASE + TMR(1), 0);
    put_w(&a, MBAR_BASE + TRR(1), 0xffff);
    put_w(&a, MBAR_BASE + TMR(1), 0xff05);
    put_b(&a, MBAR_BASE + SYPCR, 0xfe);
    loop = a.len;
    put_b(&a, MBAR_BASE + SWSR, 0x55);
    put_b(&a, MBAR_BASE + SWSR, 0xaa);
    ld_w(&a, MBAR_BASE + TCN(1), 1);
    st_l(&a, 1, RESULT(4));
    bra_to(&a, loop);
    f = flash_build(&a, NULL, NULL);
    qts = start(&f, "-icount shift=10,align=off");
    go(qts);
    /* The SRAM reads as unmapped until the program has set it up. */
    wait_for(qts, RESULT(5), 0);
    end = g_get_monotonic_time() + RUN_TIMEOUT_US;
    while ((steps = qtest_readl(qts, RESULT(4))) < 3 * 4096 &&
           !qtest_readl(qts, RESULT(6))) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    /* No restart on the way: the program never ran its second start. */
    g_assert_cmpuint(qtest_readl(qts, RESULT(6)), ==, 0);
    g_assert_cmpuint(steps, >=, 3 * 4096);
    quit_all(qts, &f);
}

/*
 * The longest watchdog period, SWP = 1 and SWT = 3, is 2^24 system clocks:
 * 0.35 s at the model clock. Timer 1 counts in 4096-clock steps (the
 * system clock divided by 16 and by a prescaler of 256) from before the
 * watchdog is armed, and the program keeps copying the count to the SRAM
 * until the watchdog resets the chip. The SRAM survives the reset, so the
 * last copy is the elapsed time at the reset: 4096 steps for 2^24 clocks,
 * where a period of 2^20 clocks would leave 256. The instruction-count
 * clock keeps the program's last copy from lagging behind the reset by
 * however long the host stalled it.
 */
static void test_watchdog_period(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    size_t first, loop;
    uint32_t steps;

    prologue(&a);
    mbar_on(&a);
    ld_l(&a, RESULT(7), 1);
    tst_l(&a, 1);
    first = beq_w(&a);
    finish(&a);
    patch_here(&a, first);
    put_l(&a, RESULT(7), 1);
    put_w(&a, MBAR_BASE + TMR(1), 0);
    put_w(&a, MBAR_BASE + TRR(1), 0xffff);
    put_w(&a, MBAR_BASE + TMR(1), 0xff05);
    put_b(&a, MBAR_BASE + SYPCR, 0xfe);
    loop = a.len;
    ld_w(&a, MBAR_BASE + TCN(1), 1);
    st_l(&a, 1, RESULT(0));
    bra_to(&a, loop);
    f = flash_build(&a, NULL, NULL);
    qts = start(&f, "-icount shift=10,align=off");
    go(qts);
    wait_for(qts, DONE, 1);

    steps = qtest_readl(qts, RESULT(0));
    g_assert_cmpuint(steps, >=, 3500);
    g_assert_cmpuint(steps, <=, 4400);
    quit_all(qts, &f);
}

/*
 * A module clock of zero has no period to count in and is refused. The shell
 * feeds "quit" to the monitor, so a machine that does start exits at once
 * instead of waiting, and the test fails on the exit status.
 */
static void test_clock_hz_zero(void)
{
    const char *argv[] = {
        "/bin/sh", "-c", "echo quit | exec \"$0\" \"$@\"",
        qtest_qemu_binary(NULL), "-machine", "sgi-l1-board",
        "-global", "mcf5206e-sim.clock-hz=0", "-display", "none",
        "-nodefaults", "-monitor", "stdio", NULL
    };
    g_autofree char *err = NULL;
    int status = 0;

    g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, 0, NULL, NULL,
                               NULL, &err, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(err, "clock-hz must be non-zero"));
}

/*
 * A write to the read-only IPR and a read of the write-only SWSR are
 * logged as guest errors and change nothing.
 */
static void test_guest_errors_logged(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;

    prologue(&a);
    mbar_on(&a);
    put_w(&a, MBAR_BASE + IPR, 0xffff);
    ld_w(&a, MBAR_BASE + IPR, 1);
    st_l(&a, 1, RESULT(0));
    ld_b(&a, MBAR_BASE + SWSR, 1);
    st_l(&a, 1, RESULT(1));
    finish(&a);

    f = flash_build(&a, NULL, NULL);
    log_path = g_build_filename(f.dir, "log.txt", NULL);
    extra = g_strdup_printf("-d guest_errors -D %s", log_path);
    qts = start(&f, extra);
    go(qts);
    wait_for(qts, DONE, 1);
    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "write of 0xffff to read-only IPR"));
    g_assert_nonnull(strstr(log, "read of write-only SWSR"));
    g_unlink(log_path);
    flash_free(&f);
}

/*
 * The four-channel UART: the scratch probe, transmit to the console
 * backend, and the shared interrupt on external level 4.
 */
static void test_quad_uart(void)
{
    Asm a = { }, h = { };
    Flash f;
    QTestState *qts;
    g_autofree char *out = NULL;
    g_autofree char *extra = NULL;
    char *dir_out;
    gint64 end;
    int ch;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    cs_setup(&a, 4, 0x5000, 0, 0x095f);
    for (ch = 0; ch < 4; ch++) {
        put_b(&a, 0x50000003 + 8 * ch, 0);
        put_b(&a, 0x50000007 + 8 * ch, 0xa0 + ch);
    }
    for (ch = 0; ch < 4; ch++) {
        ld_b(&a, 0x50000007 + 8 * ch, 1);
        st_l(&a, 1, RESULT(ch));
    }
    /* Channel 2 is the console. */
    put_b(&a, 0x50000010, 'L');
    put_b(&a, 0x50000010, '1');
    /* Interrupt on level 4 from channel 0: transmit holding empty. */
    put_l(&a, RESULT(4), TAG);
    vector_to(&a, 28, HANDLER_OFFSET);
    put_w(&a, MBAR_BASE + PAR, 0x0040);
    put_b(&a, MBAR_BASE + ICR(4), 0x90);
    put_w(&a, MBAR_BASE + IMR, 0x3fee);
    put_b(&a, 0x50000001, 0x02);
    move_sr_imm(&a, 0x2000);
    bra_self(&a);

    /* Count, then stop the channel asking. */
    put_b(&h, 0x50000001, 0x00);
    ld_l(&h, RESULT(4), 1);
    addq1_l(&h, 1);
    st_l(&h, 1, RESULT(4));
    rte(&h);

    f = flash_build(&a, &h, NULL);
    dir_out = g_build_filename(f.dir, "console.txt", NULL);
    extra = g_strdup_printf("-serial file:%s", dir_out);
    qts = start(&f, extra);
    go(qts);
    wait_for(qts, RESULT(4), 1);
    for (ch = 0; ch < 4; ch++) {
        g_assert_cmphex(qtest_readl(qts, RESULT(ch)), ==, 0xa0 + ch);
    }
    g_assert_cmpuint(qtest_readl(qts, RESULT(4)) & 0xffff, ==, 1);
    /* Give the transmit timer time to run, then read what came out. */
    end = g_get_monotonic_time() + RUN_TIMEOUT_US;
    do {
        g_usleep(10000);
        g_clear_pointer(&out, g_free);
        g_file_get_contents(dir_out, &out, NULL, NULL);
    } while ((!out || !strstr(out, "L1")) && g_get_monotonic_time() < end);
    g_assert_nonnull(out);
    g_assert_nonnull(strstr(out, "L1"));
    qtest_quit(qts);
    g_unlink(dir_out);
    g_free(dir_out);
    flash_free(&f);
}

/* Unmodelled registers and windows are logged and read as zero. */
static void test_unimplemented_logged(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;

    prologue(&a);
    mbar_on(&a);
    cs_setup(&a, 0, 0xffe0, 0x000f0000, 0x1183);
    put_w(&a, MBAR_BASE + 0x46, 0x1234);
    ld_b(&a, MBAR_BASE + 0x1c5, 1);
    st_l(&a, 1, RESULT(0));
    cs_setup(&a, 3, 0x4000, 0, 0x0953);
    ld_b(&a, 0x40000018, 1);
    st_l(&a, 1, RESULT(1));
    ld_l(&a, 0x70000000, 1);
    put_l(&a, FLASH_BASE + 0x800, 0x11223344);
    finish(&a);

    f = flash_build(&a, NULL, NULL);
    log_path = g_build_filename(f.dir, "log.txt", NULL);
    extra = g_strdup_printf("-d unimp -D %s", log_path);
    qts = start(&f, extra);
    go(qts);
    wait_for(qts, DONE, 1);
    g_assert_cmphex(qtest_readl(qts, RESULT(0)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)), ==, 0);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "write of DCRR (MBAR+0x46) = 0x1234"));
    g_assert_nonnull(strstr(log, "read of PPDDR (MBAR+0x1c5)"));
    g_assert_nonnull(strstr(log, "usb-controller-slot0"));
    g_assert_nonnull(strstr(log, "default memory"));
    g_assert_nonnull(strstr(log, "flash command set"));
    g_unlink(log_path);
    flash_free(&f);
}

/*
 * The two-wire (M-Bus) controller. The flash code only enables the module
 * block; the test then drives the five registers from the host, with a
 * stock AT24C EEPROM as the peer, so no expected value comes from the
 * model. The bus runs in virtual time, which follows the host clock here, so
 * the test polls with a time limit and asserts no duration.
 */
#define MADR            0x1e0
#define MFDR            0x1e4
#define MBCR            0x1e8
#define MBSR            0x1ec
#define MBDR            0x1f0

#define MBCR_MEN        0x80
#define MBCR_MIEN       0x40
#define MBCR_MSTA       0x20
#define MBCR_MTX        0x10
#define MBCR_TXAK       0x08
#define MBCR_RSTA       0x04

#define MBSR_MCF        0x80
#define MBSR_MBB        0x20
#define MBSR_MAL        0x10
#define MBSR_MIF        0x02
#define MBSR_RXAK       0x01

#define PEER_ADDR7      0x2c
#define PEER_CALL_W     (PEER_ADDR7 << 1)
#define PEER_CALL_R     ((PEER_ADDR7 << 1) | 1)
#define PEER_ARGS       "-device at24c-eeprom,bus=mbus,address=0x2c," \
                        "rom-size=256"

static uint8_t mb_rd(QTestState *qts, unsigned reg)
{
    return qtest_readb(qts, MBAR_BASE + reg);
}

static void mb_wr(QTestState *qts, unsigned reg, uint8_t val)
{
    qtest_writeb(qts, MBAR_BASE + reg, val);
}

static bool mbus_irq_pending(QTestState *qts)
{
    return qtest_readw(qts, MBAR_BASE + IPR) & (1 << 11);
}

static QTestState *mbus_start(Flash *f, const char *extra)
{
    Asm a = { };
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    finish(&a);
    *f = flash_build(&a, NULL, NULL);
    qts = start(f, extra);
    go(qts);
    wait_for(qts, DONE, 1);
    return qts;
}

/* Wait until (MBSR & mask) == value. */
static void mbus_wait(QTestState *qts, uint8_t mask, uint8_t value)
{
    gint64 end = g_get_monotonic_time() + RUN_TIMEOUT_US;

    while ((mb_rd(qts, MBSR) & mask) != value) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(200);
    }
}

static void mbus_enable(QTestState *qts)
{
    mb_wr(qts, MFDR, 0x13);
    mb_wr(qts, MBCR, MBCR_MEN);
}

/* Send one byte as master; returns MBSR once MIF sets, MIF then cleared. */
static uint8_t mbus_send(QTestState *qts, uint8_t byte)
{
    uint8_t sr;

    mb_wr(qts, MBDR, byte);
    mbus_wait(qts, MBSR_MIF, MBSR_MIF);
    sr = mb_rd(qts, MBSR);
    mb_wr(qts, MBSR, 0);
    return sr;
}

static void mbus_start_write(QTestState *qts)
{
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_MTX);
}

static void mbus_stop(QTestState *qts)
{
    mb_wr(qts, MBCR, MBCR_MEN);
    mbus_wait(qts, MBSR_MBB, 0);
}

/* Reset bytes, field masks and the write rules of the five registers. */
static void test_mbus_registers(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, "");

    g_assert_cmphex(mb_rd(qts, MADR), ==, 0x00);
    g_assert_cmphex(mb_rd(qts, MFDR), ==, 0x00);
    g_assert_cmphex(mb_rd(qts, MBCR), ==, 0x00);
    g_assert_cmphex(mb_rd(qts, MBSR), ==, 0x81);
    g_assert_cmphex(mb_rd(qts, MBDR), ==, 0x00);
    g_assert_false(mbus_irq_pending(qts));

    mb_wr(qts, MFDR, 0x13);
    g_assert_cmphex(mb_rd(qts, MFDR) & 0x3f, ==, 0x13);
    mb_wr(qts, MFDR, 0x3f);
    g_assert_cmphex(mb_rd(qts, MFDR) & 0x3f, ==, 0x3f);
    mb_wr(qts, MFDR, 0x13);

    /* MIEN and TXAK are plain storage; RSTA never reads back. */
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_TXAK);
    g_assert_cmphex(mb_rd(qts, MBCR) & 0xfc, ==,
                    MBCR_MEN | MBCR_MIEN | MBCR_TXAK);
    mb_wr(qts, MBCR, MBCR_MEN);

    /* Writing the other status bits does nothing. */
    mb_wr(qts, MBSR, 0x00);
    g_assert_cmphex(mb_rd(qts, MBSR) & ~MBSR_MIF & ~MBSR_MAL, ==, 0x81);
    quit_all(qts, &f);
}

/*
 * The calling byte is acknowledged by a present peer and not by an absent
 * address, and the two give different RXAK. The flag and the interrupt come
 * together, after the byte.
 */
static void test_mbus_address_ack(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);
    uint8_t sr;

    mbus_enable(qts);
    mbus_start_write(qts);
    mb_wr(qts, MBDR, PEER_CALL_W);
    mbus_wait(qts, MBSR_MIF, MBSR_MIF);
    sr = mb_rd(qts, MBSR);
    g_assert_cmphex(sr & (MBSR_MCF | MBSR_MBB | MBSR_MAL | MBSR_MIF |
                          MBSR_RXAK), ==,
                    MBSR_MCF | MBSR_MBB | MBSR_MIF);
    g_assert_true(mbus_irq_pending(qts));
    mb_wr(qts, MBSR, 0);
    g_assert_false(mbus_irq_pending(qts));
    mbus_stop(qts);

    /* An address no part answers. */
    mbus_start_write(qts);
    sr = mbus_send(qts, 0xa4);
    g_assert_cmphex(sr & (MBSR_MCF | MBSR_MAL | MBSR_RXAK), ==,
                    MBSR_MCF | MBSR_RXAK);
    mbus_stop(qts);
    quit_all(qts, &f);
}

/* MIEN hides the interrupt request without clearing the flag. */
static void test_mbus_interrupt_enable(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);

    mbus_enable(qts);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    mb_wr(qts, MBDR, PEER_CALL_W);
    mbus_wait(qts, MBSR_MIF, MBSR_MIF);
    g_assert_false(mbus_irq_pending(qts));
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_MTX);
    g_assert_true(mbus_irq_pending(qts));
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    g_assert_false(mbus_irq_pending(qts));
    g_assert_true(mb_rd(qts, MBSR) & MBSR_MIF);
    mb_wr(qts, MBSR, 0);
    mbus_stop(qts);
    quit_all(qts, &f);
}

/*
 * A multi-byte master write reaches the peer in order, and a repeated START
 * followed by a two-byte read returns what the peer holds: every data byte
 * is acknowledged and the calling byte of the second transfer is not
 * mistaken for data.
 */
static void test_mbus_write_and_read(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);
    uint8_t sr, first, second;

    mbus_enable(qts);
    mbus_start_write(qts);
    sr = mbus_send(qts, PEER_CALL_W);
    g_assert_cmphex(sr & MBSR_RXAK, ==, 0);
    sr = mbus_send(qts, 0x10);
    g_assert_cmphex(sr & MBSR_RXAK, ==, 0);
    sr = mbus_send(qts, 0xa5);
    g_assert_cmphex(sr & MBSR_RXAK, ==, 0);
    sr = mbus_send(qts, 0x5a);
    g_assert_cmphex(sr & (MBSR_RXAK | MBSR_MCF), ==, MBSR_MCF);
    mbus_stop(qts);

    /* Set the peer's pointer, then repeat START to read two bytes. */
    mbus_start_write(qts);
    mbus_send(qts, PEER_CALL_W);
    mbus_send(qts, 0x10);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_MTX |
                     MBCR_RSTA);
    g_assert_cmphex(mb_rd(qts, MBCR) & MBCR_RSTA, ==, 0);
    sr = mbus_send(qts, PEER_CALL_R);
    g_assert_cmphex(sr & MBSR_RXAK, ==, 0);

    /* Receive: clear MTX, a read of MBDR starts the first byte. */
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA);
    mb_rd(qts, MBDR);
    mbus_wait(qts, MBSR_MIF, MBSR_MIF);
    mb_wr(qts, MBSR, 0);
    /* The next is the last: NACK it, and the read returns the first. */
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_TXAK);
    first = mb_rd(qts, MBDR);
    mbus_wait(qts, MBSR_MIF, MBSR_MIF);
    mb_wr(qts, MBSR, 0);
    /* STOP before reading the last byte. */
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN);
    second = mb_rd(qts, MBDR);
    g_assert_cmphex(first, ==, 0xa5);
    g_assert_cmphex(second, ==, 0x5a);
    mbus_wait(qts, MBSR_MBB, 0);
    quit_all(qts, &f);
}

/* A repeated START asked for by a module that is not the master. */
static void test_mbus_arbitration_loss(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);

    mbus_enable(qts);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_RSTA);
    g_assert_cmphex(mb_rd(qts, MBSR) & (MBSR_MAL | MBSR_MIF), ==,
                    MBSR_MAL | MBSR_MIF);
    g_assert_cmphex(mb_rd(qts, MBCR) & MBCR_MSTA, ==, 0);
    g_assert_true(mbus_irq_pending(qts));
    /* MAL and MIF clear independently, by a written zero each. */
    mb_wr(qts, MBSR, MBSR_MAL);
    g_assert_cmphex(mb_rd(qts, MBSR) & (MBSR_MAL | MBSR_MIF), ==, MBSR_MAL);
    mb_wr(qts, MBSR, 0);
    g_assert_cmphex(mb_rd(qts, MBSR) & (MBSR_MAL | MBSR_MIF), ==, 0);
    g_assert_false(mbus_irq_pending(qts));
    /* The other way round: a zero in MAL only. */
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_RSTA);
    mb_wr(qts, MBSR, MBSR_MIF);
    g_assert_cmphex(mb_rd(qts, MBSR) & (MBSR_MAL | MBSR_MIF), ==, MBSR_MIF);
    mb_wr(qts, MBSR, 0);
    quit_all(qts, &f);
}

/*
 * A module clock of 100 kHz makes the slowest divider a clock cycle of 38 ms,
 * so two stores in a row land within one cycle however the host schedules
 * the guest.
 */
#define SLOW_CLOCK      " -global mcf5206e-sim.clock-hz=100000"

/*
 * A STOP asked for while a byte is on the bus waits for the end of that
 * byte. The guest writes the byte and clears MSTA with two stores in a row,
 * and records MBSR after them; the host then expects the bus to go idle
 * without another write. At the slowest divider a byte lasts a third of a
 * second, and MCF clear in the recorded MBSR shows the byte was still
 * on the bus when the STOP was asked for.
 */
static void test_mbus_stop_mid_byte(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    put_b(&a, MBAR_BASE + MFDR, 0x1f);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    put_b(&a, MBAR_BASE + MBDR, PEER_CALL_W);
    poll_bits(&a, MBAR_BASE + MBSR, MBSR_MIF, true);
    put_b(&a, MBAR_BASE + MBSR, 0);
    put_b(&a, MBAR_BASE + MBDR, 0x10);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN | MBCR_MTX);
    ld_b(&a, MBAR_BASE + MBSR, 2);
    st_l(&a, 2, RESULT(0));
    poll_bits(&a, MBAR_BASE + MBSR, MBSR_MIF, true);
    finish(&a);
    f = flash_build(&a, NULL, NULL);
    qts = start(&f, PEER_ARGS SLOW_CLOCK);
    go(qts);
    wait_for(qts, DONE, 1);

    g_assert_cmphex(qtest_readl(qts, RESULT(0)) & (MBSR_MCF | MBSR_MBB), ==,
                    MBSR_MBB);
    mbus_wait(qts, MBSR_MBB, 0);
    quit_all(qts, &f);
}

/*
 * A START asked for while the bus is still busy loses arbitration and clears
 * MSTA, which the write had just set. The guest starts a transfer, asks for
 * the STOP and, before the STOP has finished, sets MSTA again.
 */
static void test_mbus_busy_start_loss(void)
{
    Asm a = { };
    Flash f;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    put_b(&a, MBAR_BASE + MFDR, 0x1f);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    poll_bits(&a, MBAR_BASE + MBSR, MBSR_MBB, true);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN | MBCR_MTX);
    put_b(&a, MBAR_BASE + MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    ld_b(&a, MBAR_BASE + MBCR, 2);
    st_l(&a, 2, RESULT(0));
    ld_b(&a, MBAR_BASE + MBSR, 2);
    st_l(&a, 2, RESULT(1));
    finish(&a);
    f = flash_build(&a, NULL, NULL);
    qts = start(&f, PEER_ARGS SLOW_CLOCK);
    go(qts);
    wait_for(qts, DONE, 1);

    g_assert_cmphex(qtest_readl(qts, RESULT(0)) & MBCR_MSTA, ==, 0);
    g_assert_cmphex(qtest_readl(qts, RESULT(1)) & (MBSR_MAL | MBSR_MIF), ==,
                    MBSR_MAL | MBSR_MIF);
    quit_all(qts, &f);
}

/*
 * No MBCR bit but MEN has an effect while MEN is clear, though the bits read
 * back. Setting MEN afterwards is the 0 to 1 edge of MSTA that starts the
 * transfer.
 */
static void test_mbus_bits_before_enable(void)
{
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);

    mb_wr(qts, MFDR, 0x13);
    mb_wr(qts, MBCR, MBCR_MSTA | MBCR_MTX);
    g_assert_cmphex(mb_rd(qts, MBCR), ==, MBCR_MSTA | MBCR_MTX);
    g_assert_cmphex(mb_rd(qts, MBSR) & (MBSR_MBB | MBSR_MAL), ==, 0);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
    mbus_wait(qts, MBSR_MBB, MBSR_MBB);
    quit_all(qts, &f);
}

/* The path of the M-Bus controller, from the QOM tree's indentation. */
static char *mbus_qom_path(QTestState *qts)
{
    g_autofree char *tree = qtest_hmp(qts, "info qom-tree");
    g_auto(GStrv) lines = g_strsplit(tree, "\n", -1);
    char *parts[32] = { };
    char *found = NULL;

    for (char **l = lines; *l && !found; l++) {
        const char *name = *l + strspn(*l, " ");
        unsigned depth = (name - *l) / 2;

        if (name[0] != '/') {
            continue;
        }
        g_assert_cmpuint(depth, <, G_N_ELEMENTS(parts));
        g_free(parts[depth]);
        parts[depth] = g_strndup(name, strcspn(name, " "));
        if (strstr(name, "(mcf5206e-mbus)")) {
            GString *path = g_string_new(NULL);

            for (unsigned d = 0; d <= depth; d++) {
                g_string_append(path, parts[d]);
            }
            found = g_string_free(path, false);
        }
    }
    for (unsigned d = 0; d < G_N_ELEMENTS(parts); d++) {
        g_free(parts[d]);
    }
    g_assert_nonnull(found);
    return found;
}

static int64_t mbus_phase_ns(QTestState *qts, const char *path,
                             const char *name)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': %s, 'property': %s } }", path, name);
    int64_t ns;

    g_assert(qdict_haskey(rsp, "return"));
    ns = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return ns;
}

/*
 * The divider selector indexes a table, and each clock phase is half the
 * period rounded up. The controller reports the phases it computed, so no
 * host time is involved. The divisors are the manual's (0x00 is 28, 0x07 68,
 * 0x13 480, 0x20 20, 0x3f 2048) and the module clock is the board's 48 MHz.
 */
static void test_mbus_divider(void)
{
    static const struct { uint8_t mfdr; uint32_t divisor; } cases[] = {
        { 0x00, 28 }, { 0x07, 68 }, { 0x13, 480 }, { 0x20, 20 },
        { 0x3f, 2048 },
    };
    Flash f;
    QTestState *qts = mbus_start(&f, PEER_ARGS);
    g_autofree char *path = mbus_qom_path(qts);

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        int64_t half = DIV_ROUND_UP((uint64_t)cases[i].divisor * 1000000000ull,
                                    2 * 48000000ull);

        mb_wr(qts, MBCR, 0);
        mb_wr(qts, MFDR, cases[i].mfdr);
        mb_wr(qts, MBCR, MBCR_MEN);
        mb_wr(qts, MBCR, MBCR_MEN | MBCR_MSTA | MBCR_MTX);
        g_assert_cmpint(mbus_phase_ns(qts, path, "scl-low-ns"), ==, half);
        g_assert_cmpint(mbus_phase_ns(qts, path, "scl-high-ns"), ==, half);
    }
    quit_all(qts, &f);
}

/* Wider and misaligned accesses over the registers are refused and logged. */
static void test_mbus_wide_access(void)
{
    Asm a = { };
    Flash f;
    g_autofree char *log_path = NULL;
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    finish(&a);
    f = flash_build(&a, NULL, NULL);
    log_path = g_build_filename(f.dir, "log.txt", NULL);
    extra = g_strdup_printf("-d guest_errors -D %s", log_path);
    qts = start(&f, extra);
    go(qts);
    wait_for(qts, DONE, 1);

    mbus_enable(qts);
    g_assert_cmphex(qtest_readw(qts, MBAR_BASE + MBCR), ==, 0);
    qtest_writew(qts, MBAR_BASE + MBCR, 0xe0e0);
    qtest_writel(qts, MBAR_BASE + MFDR, 0x3f3f3f3f);
    g_assert_cmphex(mb_rd(qts, MBCR), ==, MBCR_MEN);
    g_assert_cmphex(mb_rd(qts, MFDR) & 0x3f, ==, 0x13);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "over the M-Bus MBCR"));
    g_assert_nonnull(strstr(log, "over the M-Bus MFDR"));
    g_unlink(log_path);
    flash_free(&f);
}

/*
 * The identity EEPROMs. Each test reads a part at the address the firmware
 * calls, through the controller, the way the firmware does: calling byte,
 * word address, repeated START, calling byte with the read bit, then bytes.
 * The expectations come from the record layout (an eight-byte common header
 * whose bytes 2 and 3 give the chassis and board areas' offsets in eight-byte
 * units, areas that sum to zero and whose second byte counts eight-byte units)
 * and from the names the test gives, not from the model's own bytes.
 */
#define EEPROM_READ_MAX 256

static bool eeprom_read(QTestState *qts, uint8_t call, uint8_t *buf, size_t n)
{
    uint8_t sr;
    size_t i;

    g_assert_cmpuint(n, >=, 2);
    mbus_enable(qts);
    mbus_start_write(qts);
    sr = mbus_send(qts, call);
    if (sr & MBSR_RXAK) {
        mbus_stop(qts);
        return false;
    }
    g_assert_cmphex(mbus_send(qts, 0x00) & MBSR_RXAK, ==, 0);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_MTX |
                     MBCR_RSTA);
    g_assert_cmphex(mbus_send(qts, call | 1) & MBSR_RXAK, ==, 0);
    mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA);
    mb_rd(qts, MBDR);
    for (i = 0; i < n; i++) {
        mbus_wait(qts, MBSR_MIF, MBSR_MIF);
        mb_wr(qts, MBSR, 0);
        if (i == n - 1) {
            mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN);
        } else if (i == n - 2) {
            mb_wr(qts, MBCR, MBCR_MEN | MBCR_MIEN | MBCR_MSTA | MBCR_TXAK);
        }
        buf[i] = mb_rd(qts, MBDR);
    }
    mbus_wait(qts, MBSR_MBB, 0);
    return true;
}

static uint8_t sum_bytes(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;

    while (n--) {
        sum += *p++;
    }
    return sum;
}

/* Find an ASCII field (type 3, length in the low six bits) holding @text. */
static bool has_text_field(const uint8_t *area, size_t len, const char *text)
{
    size_t n = strlen(text);
    size_t i;

    for (i = 0; i + 1 + n <= len; i++) {
        if (area[i] == (0xc0 | n) && !memcmp(area + i + 1, text, n)) {
            return true;
        }
    }
    return false;
}

/* Check one board image and return its board area for name checks. */
static const uint8_t *check_image(const uint8_t *img, size_t *board_len)
{
    size_t chassis = img[2] * 8, board = img[3] * 8;

    g_assert_cmphex(sum_bytes(img, 8), ==, 0);
    g_assert_cmpuint(img[0], !=, 0);
    g_assert_cmpuint(chassis, ==, 8);
    g_assert_cmpuint(board, >, chassis);
    g_assert_cmphex(sum_bytes(img + chassis, img[chassis + 1] * 8), ==, 0);
    *board_len = img[board + 1] * 8;
    g_assert_cmpuint(*board_len, >, 0);
    g_assert_cmphex(sum_bytes(img + board, *board_len), ==, 0);
    g_assert_cmpuint(board + *board_len, <=, EEPROM_READ_MAX);
    return img + board;
}

static QTestState *board_start(Flash *f, const char *machine,
                               const char *extra)
{
    Asm a = { };
    QTestState *qts;

    prologue(&a);
    mbar_on(&a);
    finish(&a);
    *f = flash_build(&a, NULL, NULL);
    qts = start_machine(f, machine, extra);
    go(qts);
    wait_for(qts, DONE, 1);
    return qts;
}

/* A C-brick: the node board's record at 0xa4 and nothing else on segment 0. */
static void test_identity_cbrick(void)
{
    uint8_t img[EEPROM_READ_MAX];
    const uint8_t *board;
    size_t board_len;
    Flash f;
    QTestState *qts = board_start(&f, "sgi-l1-board,brick-type=cbrick,"
                                  "node-board-name=TESTNODE,"
                                  "node-board-part=TESTPART0,"
                                  "node-board-revision=7", "");

    g_assert_true(eeprom_read(qts, 0xa4, img, sizeof(img)));
    board = check_image(img, &board_len);
    g_assert_true(has_text_field(board, board_len, "TESTNODE"));
    g_assert_true(has_text_field(board, board_len, "TESTPART0"));
    g_assert_true(has_text_field(board, board_len, "7"));
    g_assert_true(has_text_field(board, board_len, "SYNTHETIC"));
    /* The processor modules' records sit behind a bus segment: not fitted. */
    g_assert_false(eeprom_read(qts, 0xa8, img, 8));
    g_assert_false(eeprom_read(qts, 0xa0, img, 8));
    quit_all(qts, &f);
}

/* An I-brick: the power board at 0xa4 and the logic board at 0xa8. */
static void test_identity_ibrick(void)
{
    uint8_t img[EEPROM_READ_MAX];
    const uint8_t *board;
    size_t board_len;
    Flash f;
    QTestState *qts = board_start(&f, "sgi-l1-board,brick-type=ibrick,"
                                  "mac=00:00:5e:00:53:01", "");

    g_assert_true(eeprom_read(qts, 0xa4, img, sizeof(img)));
    board = check_image(img, &board_len);
    g_assert_true(has_text_field(board, board_len, "POWER"));
    g_assert_true(eeprom_read(qts, 0xa8, img, sizeof(img)));
    board = check_image(img, &board_len);
    g_assert_true(has_text_field(board, board_len, "LOGIC"));
    g_assert_true(has_text_field(board, board_len, "00005E005301"));
    quit_all(qts, &f);
}

/* With no brick type, no identity part is fitted. */
static void test_identity_none(void)
{
    uint8_t img[8];
    Flash f;
    QTestState *qts = board_start(&f, "sgi-l1-board", "");

    g_assert_false(eeprom_read(qts, 0xa4, img, sizeof(img)));
    g_assert_false(eeprom_read(qts, 0xa8, img, sizeof(img)));
    quit_all(qts, &f);
}

/* The identity parts keep what the host writes, as the part's pins allow. */
static void test_identity_write(void)
{
    uint8_t img[EEPROM_READ_MAX], again[EEPROM_READ_MAX];
    Flash f;
    QTestState *qts = board_start(&f, "sgi-l1-board,brick-type=cbrick", "");

    g_assert_true(eeprom_read(qts, 0xa4, img, sizeof(img)));
    mbus_enable(qts);
    mbus_start_write(qts);
    g_assert_cmphex(mbus_send(qts, 0xa4) & MBSR_RXAK, ==, 0);
    mbus_send(qts, 0xf0);
    mbus_send(qts, 0x5a);
    mbus_stop(qts);
    g_assert_true(eeprom_read(qts, 0xa4, again, sizeof(again)));
    g_assert_cmphex(again[0xf0], ==, 0x5a);
    g_assert_cmpuint(img[0xf0], !=, 0x5a);
    quit_all(qts, &f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("sgi-l1-board/global-cs0", test_global_cs0);
    qtest_add_func("sgi-l1-board/cs0-window", test_cs0_window);
    qtest_add_func("sgi-l1-board/ram-512k", test_ram_512k);
    qtest_add_func("sgi-l1-board/ram-128k-aliased", test_ram_128k_aliased);
    qtest_add_func("sgi-l1-board/nvram", test_nvram);
    qtest_add_func("sgi-l1-board/sim-registers", test_sim_registers);
    qtest_add_func("sgi-l1-board/sim-write-rules", test_sim_write_rules);
    qtest_add_func("sgi-l1-board/timer-interrupt", test_timer_interrupt);
    qtest_add_func("sgi-l1-board/masked-interrupt", test_masked_interrupt);
    qtest_add_func("sgi-l1-board/timer-event-and-reset",
                   test_timer_event_and_reset);
    qtest_add_func("sgi-l1-board/watchdog-reset", test_watchdog_reset);
    qtest_add_func("sgi-l1-board/watchdog-service", test_watchdog_service);
    qtest_add_func("sgi-l1-board/watchdog-period", test_watchdog_period);
    qtest_add_func("sgi-l1-board/clock-hz-zero", test_clock_hz_zero);
    qtest_add_func("sgi-l1-board/guest-errors-logged",
                   test_guest_errors_logged);
    qtest_add_func("sgi-l1-board/quad-uart", test_quad_uart);
    qtest_add_func("sgi-l1-board/unimplemented-logged",
                   test_unimplemented_logged);
    qtest_add_func("sgi-l1-board/mbus-registers", test_mbus_registers);
    qtest_add_func("sgi-l1-board/mbus-address-ack", test_mbus_address_ack);
    qtest_add_func("sgi-l1-board/mbus-interrupt-enable",
                   test_mbus_interrupt_enable);
    qtest_add_func("sgi-l1-board/mbus-write-and-read",
                   test_mbus_write_and_read);
    qtest_add_func("sgi-l1-board/mbus-arbitration-loss",
                   test_mbus_arbitration_loss);
    qtest_add_func("sgi-l1-board/mbus-stop-mid-byte",
                   test_mbus_stop_mid_byte);
    qtest_add_func("sgi-l1-board/mbus-busy-start-loss",
                   test_mbus_busy_start_loss);
    qtest_add_func("sgi-l1-board/mbus-bits-before-enable",
                   test_mbus_bits_before_enable);
    qtest_add_func("sgi-l1-board/mbus-divider", test_mbus_divider);
    qtest_add_func("sgi-l1-board/mbus-wide-access", test_mbus_wide_access);
    qtest_add_func("sgi-l1-board/identity-cbrick", test_identity_cbrick);
    qtest_add_func("sgi-l1-board/identity-ibrick", test_identity_ibrick);
    qtest_add_func("sgi-l1-board/identity-none", test_identity_none);
    qtest_add_func("sgi-l1-board/identity-write", test_identity_write);

    return g_test_run();
}
