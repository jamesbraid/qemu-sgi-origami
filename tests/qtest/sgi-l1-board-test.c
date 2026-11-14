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

static QTestState *start(const Flash *f, const char *extra)
{
    return qtest_initf("-machine sgi-l1-board -bios %s -accel tcg -S %s",
                       f->path, extra);
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
    ld_b(&a, MBAR_BASE + 0x1ec, 1);
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
    g_assert_nonnull(strstr(log, "read of MBSR (MBAR+0x1ec)"));
    g_assert_nonnull(strstr(log, "usb-controller-slot0"));
    g_assert_nonnull(strstr(log, "default memory"));
    g_assert_nonnull(strstr(log, "flash command set"));
    g_unlink(log_path);
    flash_free(&f);
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

    return g_test_run();
}
