/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DSP56300 core: hand-assembled programs. Opcodes are written as the bit
 * strings of the instruction formats in the DSP56300 Family Manual, chapter
 * 13, and the expected results are worked out from the instruction
 * descriptions, not from the implementation.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "../migration/qemu-file.h"
#include "io/channel-file.h"
#include "hw/coproc/dsp56300/dsp56300.h"

#define MEM_WORDS 0x1000
#define IO_BASE   0xffff80u

typedef struct Fixture {
    DSP56300State d;
    uint32_t mem[3][MEM_WORDS];
    uint32_t io[2][128];                /* X and Y I/O windows */
    unsigned int io_writes;
    uint32_t last_io_write;
    unsigned int periph_resets;
} Fixture;

static uint32_t fx_read(void *opaque, DSP56300Space sp, uint32_t addr)
{
    Fixture *f = opaque;

    if (addr >= IO_BASE && sp != DSP56300_SPACE_P) {
        return f->io[sp][addr - IO_BASE];
    }
    return 0xdead00;
}

static void fx_write(void *opaque, DSP56300Space sp, uint32_t addr,
                     uint32_t val)
{
    Fixture *f = opaque;

    if (addr >= IO_BASE && sp != DSP56300_SPACE_P) {
        f->io[sp][addr - IO_BASE] = val;
        f->io_writes++;
        f->last_io_write = addr;
    }
}

static void fx_reset(void *opaque)
{
    Fixture *f = opaque;

    f->periph_resets++;
}

static Fixture *fx_new(void)
{
    static const DSP56300Ops ops = {
        .read = fx_read, .write = fx_write, .peripheral_reset = fx_reset,
    };
    Fixture *f = g_new0(Fixture, 1);

    dsp56300_init(&f->d, &ops, f);
    for (int sp = 0; sp < 3; sp++) {
        dsp56300_set_memory(&f->d, sp, f->mem[sp], MEM_WORDS);
    }
    return f;
}

/* "0000 1010 ..." -> 24-bit word; spaces are ignored. */
static uint32_t bits(const char *s)
{
    uint32_t v = 0;

    for (; *s; s++) {
        if (*s == '0' || *s == '1') {
            v = (v << 1) | (*s - '0');
        }
    }
    return v;
}

#define NOP     0x000000u
#define WAIT    0x000086u
#define RTS     0x00000cu
#define RTI     0x000004u
#define INC_A   0x000008u
#define INC_B   0x000009u
#define NOMOVE  0x200000u               /* parallel move field: none */

static void load(Fixture *f, uint32_t addr, const uint32_t *w, size_t n)
{
    memcpy(&f->mem[DSP56300_SPACE_P][addr], w, n * sizeof(*w));
}

#define LOAD(f, addr, ...) do { \
        const uint32_t w_[] = { __VA_ARGS__ }; \
        load(f, addr, w_, ARRAY_SIZE(w_)); \
    } while (0)

/* Run until the program waits or spins; fails if it runs away. */
static void run(Fixture *f)
{
    uint64_t used = dsp56300_run(&f->d, 100000);

    g_assert_true(f->d.idle);
    g_assert_cmpuint(used, <, 100000);
}

/* Run from PC 0 to a WAIT at the end of the program. */
static void go(Fixture *f)
{
    f->d.pc = 0;
    f->d.run_state = DSP56300_RUNNING;
    run(f);
}

#define ACC(a2, a1, a0) \
    (((uint64_t)(a2) << 48) | ((uint64_t)(a1) << 24) | (uint64_t)(a0))

static uint64_t acc(Fixture *f, int w)
{
    return (uint64_t)dsp56300_get_acc(&f->d, w) & 0xffffffffffffffULL;
}

static void set_acc(Fixture *f, int w, uint64_t v)
{
    dsp56300_set_acc(&f->d, w, (int64_t)(v << 8) >> 8);
}

static unsigned int ccr(Fixture *f)
{
    return f->d.sr & 0xff;
}

#define C_  DSP56300_SR_C
#define V_  DSP56300_SR_V
#define Z_  DSP56300_SR_Z
#define N_  DSP56300_SR_N
#define U_  DSP56300_SR_U
#define E_  DSP56300_SR_E
#define L_  DSP56300_SR_L

#define ASSERT_ACC(f, w, v) g_assert_cmphex(acc(f, w), ==, (v))
#define ASSERT_CCR(f, mask, v) \
    g_assert_cmphex(ccr(f) & (mask), ==, (v))


#define SR_I_MASK (DSP56300_SR_I0 | DSP56300_SR_I1)

static uint32_t reg(Fixture *f, unsigned int r)
{
    return dsp56300_get_reg(&f->d, r);
}

/* ---- reset and register access ----------------------------------------- */

static void test_reset(void)
{
    Fixture *f = fx_new();

    f->d.sr = 0;
    f->d.m[3] = 5;
    dsp56300_reset(&f->d, 0x1234);
    g_assert_cmphex(f->d.sr, ==, 0xc00300);
    g_assert_cmphex(f->d.pc, ==, 0x1234);
    g_assert_cmphex(f->d.m[3], ==, 0xffffff);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_RUNNING);

    /* the register numbers are the instruction set's own */
    dsp56300_set_reg(&f->d, DSP56300_REG_B0, 0x123456);
    dsp56300_set_reg(&f->d, DSP56300_REG_B2, 0xfe);
    g_assert_cmphex(reg(f, DSP56300_REG_B0), ==, 0x123456);
    /* A2 and B2 read back sign-extended */
    g_assert_cmphex(reg(f, DSP56300_REG_B2), ==, 0xfffffe);
    ASSERT_ACC(f, 1, ACC(0xfe, 0, 0x123456));
    g_free(f);
}

/* ---- arithmetic -------------------------------------------------------- */

static void test_add_sub(void)
{
    Fixture *f = fx_new();

    /* ADD X0,A ; ADD X0,A ; SUB X0,A ; WAIT */
    LOAD(f, 0, NOMOVE | 0x40, NOMOVE | 0x40, NOMOVE | 0x44, WAIT);
    set_acc(f, 0, ACC(0, 0x100000, 0));
    f->d.x0 = 0x200000;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x300000, 0));
    /* no carry, no overflow; bits 47 and 46 equal: unnormalised */
    ASSERT_CCR(f, 0x3f, U_);
    g_free(f);
}

static void test_add_into_extension(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x40, WAIT);
    set_acc(f, 0, ACC(0, 0x7fffff, 0));
    f->d.x0 = 0x7fffff;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0xfffffe, 0));
    /* A1 bit 47 set but A2 clear: the extension is in use */
    ASSERT_CCR(f, 0x3f, E_ | U_);
    g_free(f);
}

static void test_add_carry_and_overflow(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x40, WAIT);           /* ADD X0,A */
    set_acc(f, 0, ACC(0xff, 0xffffff, 0xffffff));       /* -1 */
    f->d.x0 = 1;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0, 0xffffff));
    ASSERT_CCR(f, C_ | V_, C_);                /* carry out, no overflow */

    set_acc(f, 0, ACC(0x7f, 0xffffff, 0));
    f->d.x0 = 0x7fffff;
    go(f);
    ASSERT_CCR(f, C_ | V_ | N_ | L_, V_ | N_ | L_);   /* 56-bit overflow */
    g_free(f);
}

static void test_sub_borrow(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x44, WAIT);           /* SUB X0,A */
    f->d.x0 = 1;
    go(f);
    ASSERT_ACC(f, 0, ACC(0xff, 0xffffff, 0));
    ASSERT_CCR(f, 0x3f, C_ | N_ | U_);
    g_free(f);
}

static void test_inc_dec_overflow(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, INC_A, WAIT);
    set_acc(f, 0, ACC(0x7f, 0xffffff, 0xffffff));
    go(f);
    ASSERT_ACC(f, 0, ACC(0x80, 0, 0));
    ASSERT_CCR(f, V_ | N_ | L_ | C_, V_ | N_ | L_);

    LOAD(f, 0, 0x00000au, WAIT);               /* DEC A */
    set_acc(f, 0, 0);
    go(f);
    ASSERT_ACC(f, 0, ACC(0xff, 0xffffff, 0xffffff));
    ASSERT_CCR(f, C_ | N_, C_ | N_);
    g_free(f);
}

static void test_cmp_does_not_store(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x45, WAIT);           /* CMP X0,A */
    set_acc(f, 0, ACC(0, 0x123456, 0));
    f->d.x0 = 0x123456;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x123456, 0));
    ASSERT_CCR(f, Z_ | N_ | C_, Z_);

    set_acc(f, 0, ACC(0, 0x100000, 0));
    f->d.x0 = 0x200000;
    go(f);
    ASSERT_CCR(f, Z_ | N_ | C_, N_ | C_);
    g_free(f);
}

static void test_clr_abs_neg_tst(void)
{
    Fixture *f = fx_new();

    /* ABS A ; NEG A ; TST A ; CLR A */
    LOAD(f, 0, NOMOVE | 0x26, NOMOVE | 0x36, NOMOVE | 0x03, NOMOVE | 0x13,
         WAIT);
    set_acc(f, 0, ACC(0xff, 0xffffff, 0));      /* -2^24 */
    f->d.sr |= C_;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 1, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0xff, 0xffffff, 0));
    ASSERT_CCR(f, N_, N_);
    dsp56300_step(&f->d);                      /* TST clears V, keeps C */
    ASSERT_CCR(f, N_ | V_ | C_, N_ | C_);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 0);
    ASSERT_CCR(f, E_ | U_ | N_ | Z_ | V_ | C_, U_ | Z_ | C_);
    g_free(f);
}

static void test_addl_addr_subl(void)
{
    Fixture *f = fx_new();

    /* ADDL B,A ; ADDR B,A ; SUBL B,A */
    LOAD(f, 0, NOMOVE | 0x12, NOMOVE | 0x02, NOMOVE | 0x16, WAIT);
    set_acc(f, 0, ACC(0, 0x100000, 0));
    set_acc(f, 1, ACC(0, 0x010000, 0));
    dsp56300_step(&f->d);                      /* 2*A + B */
    ASSERT_ACC(f, 0, ACC(0, 0x210000, 0));
    dsp56300_step(&f->d);                      /* A/2 + B */
    ASSERT_ACC(f, 0, ACC(0, 0x118000, 0));
    dsp56300_step(&f->d);                      /* 2*A - B */
    ASSERT_ACC(f, 0, ACC(0, 0x220000, 0));
    g_free(f);
}

static void test_adc_sbc(void)
{
    Fixture *f = fx_new();

    /* ADC X,A ; SBC Y,B */
    LOAD(f, 0, NOMOVE | 0x21, NOMOVE | 0x35 | 0x08, WAIT);
    f->d.sr |= C_;
    f->d.x1 = 0;
    f->d.x0 = 1;
    f->d.y1 = 0;
    f->d.y0 = 4;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 2);                       /* 0 + X + C */
    f->d.sr |= C_;
    dsp56300_step(&f->d);                      /* 0 - Y - C */
    ASSERT_ACC(f, 1, ACC(0xff, 0xffffff, 0xfffffb));
    g_free(f);
}

static void test_max(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x1d, WAIT);           /* MAX A,B */
    set_acc(f, 0, ACC(0, 0x300000, 0));
    set_acc(f, 1, ACC(0, 0x100000, 0));
    go(f);
    ASSERT_ACC(f, 1, ACC(0, 0x300000, 0));
    ASSERT_CCR(f, C_, 0);                      /* transfer made */

    set_acc(f, 1, ACC(0, 0x500000, 0));
    go(f);
    ASSERT_ACC(f, 1, ACC(0, 0x500000, 0));
    ASSERT_CCR(f, C_, C_);
    g_free(f);
}

static void test_immediate_alu(void)
{
    Fixture *f = fx_new();

    /* ADD #5,A ; ADD #$123456,A (long) ; CMP #3,B ; SUB #1,A */
    LOAD(f, 0,
         bits("0000 0001 0100 0101 1000 0000"),
         bits("0000 0001 0100 0000 1100 0000"), 0x123456,
         bits("0000 0001 0100 0011 1000 1101"),
         bits("0000 0001 0100 0001 1000 0100"),
         WAIT);
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 5 + 0x123456 - 1, 0));
    ASSERT_ACC(f, 1, 0);                       /* CMP does not store */
    ASSERT_CCR(f, Z_ | N_, 0);
    g_free(f);
}

static void test_saturation_mode(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOMOVE | 0x40, WAIT);
    f->d.sr |= DSP56300_SR_SM;
    set_acc(f, 0, ACC(0, 0x7fffff, 0));
    f->d.x0 = 0x7fffff;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x7fffff, 0xffffff));
    ASSERT_CCR(f, V_ | L_, V_ | L_);
    g_free(f);
}

static void test_round(void)
{
    Fixture *f = fx_new();

    /* RND A: convergent rounding sends ties to even */
    LOAD(f, 0, NOMOVE | 0x11, WAIT);
    set_acc(f, 0, ACC(0, 1, 0x800000));
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 2, 0));
    set_acc(f, 0, ACC(0, 2, 0x800000));
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 2, 0));
    set_acc(f, 0, ACC(0, 2, 0x7fffff));
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 2, 0));

    /* two's-complement rounding rounds a tie up */
    f->d.sr |= DSP56300_SR_RM;
    set_acc(f, 0, ACC(0, 2, 0x800000));
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 3, 0));
    g_free(f);
}

/* ---- logic and shifts -------------------------------------------------- */

static void test_logic(void)
{
    Fixture *f = fx_new();

    /* AND X0,A ; OR X0,B ; EOR X0,A ; NOT B */
    LOAD(f, 0, NOMOVE | 0x46, NOMOVE | 0x4a, NOMOVE | 0x43, NOMOVE | 0x1f,
         WAIT);
    set_acc(f, 0, ACC(0xab, 0xf0f0f0, 0x123456));
    f->d.x0 = 0x00ffff;
    dsp56300_step(&f->d);
    /* only A1 changes; A2 and A0 stay */
    ASSERT_ACC(f, 0, ACC(0xab, 0x00f0f0, 0x123456));
    ASSERT_CCR(f, N_ | Z_ | V_, 0);
    dsp56300_step(&f->d);
    g_assert_cmphex(reg(f, DSP56300_REG_B1), ==, 0x00ffff);
    dsp56300_step(&f->d);
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0x000f0f);
    dsp56300_step(&f->d);
    g_assert_cmphex(reg(f, DSP56300_REG_B1), ==, 0xff0000);
    ASSERT_CCR(f, N_ | Z_ | V_, N_);
    g_free(f);
}

static void test_logic_immediate(void)
{
    Fixture *f = fx_new();

    /* AND #$3f,A ; OR #$800000,B (long) ; EOR #$ff00ff,A (long) */
    LOAD(f, 0,
         bits("0000 0001 0111 1111 1000 0110"),
         bits("0000 0001 0100 0000 1100 1010"), 0x800000,
         bits("0000 0001 0100 0000 1100 0011"), 0xff00ff,
         WAIT);
    set_acc(f, 0, ACC(0, 0xff00ff, 0));
    go(f);
    g_assert_cmphex(reg(f, DSP56300_REG_B1), ==, 0x800000);
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0x3f ^ 0xff00ff);
    g_free(f);
}

static void test_shifts(void)
{
    Fixture *f = fx_new();

    /* ASL A ; ASL A ; ASR A */
    LOAD(f, 0, NOMOVE | 0x32, NOMOVE | 0x32, NOMOVE | 0x22, WAIT);
    set_acc(f, 0, 1ULL << 54);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 1ULL << 55);
    ASSERT_CCR(f, V_ | N_ | C_, V_ | N_);      /* bit 55 changed */
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 0);
    ASSERT_CCR(f, V_ | Z_ | C_, V_ | Z_ | C_);
    set_acc(f, 0, 1);
    dsp56300_step(&f->d);                      /* ASR: bit 0 goes to C */
    ASSERT_ACC(f, 0, 0);
    ASSERT_CCR(f, C_ | V_, C_);
    set_acc(f, 0, ACC(0xff, 0x800000, 0));
    f->d.pc = 2;
    dsp56300_step(&f->d);                      /* the sign is held */
    ASSERT_ACC(f, 0, ACC(0xff, 0xc00000, 0));
    g_free(f);
}

static void test_logical_shifts_rotates(void)
{
    Fixture *f = fx_new();

    /* LSL A ; LSR A ; ROL A ; ROL A ; ROR A ; ROR A */
    LOAD(f, 0, NOMOVE | 0x33, NOMOVE | 0x23, NOMOVE | 0x37, NOMOVE | 0x37,
         NOMOVE | 0x27, NOMOVE | 0x27, WAIT);
    set_acc(f, 0, ACC(0, 0x800001, 5));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 2, 5));            /* A0 is not shifted */
    ASSERT_CCR(f, C_, C_);
    dsp56300_step(&f->d);
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 1);
    ASSERT_CCR(f, C_, 0);
    dsp56300_step(&f->d);                      /* ROL, carry in is 0 */
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 2);
    set_acc(f, 0, ACC(0, 0x800000, 0));
    f->d.sr &= ~C_;
    dsp56300_step(&f->d);                      /* bit 23 rotates into C */
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0);
    ASSERT_CCR(f, C_ | Z_, C_ | Z_);
    set_acc(f, 0, ACC(0, 1, 0));
    f->d.sr &= ~C_;
    dsp56300_step(&f->d);                      /* ROR: bit 0 into C */
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0);
    ASSERT_CCR(f, C_, C_);
    dsp56300_step(&f->d);                      /* C rotates into bit 23 */
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0x800000);
    ASSERT_CCR(f, C_ | N_, N_);
    g_free(f);
}

static void test_multibit_shifts(void)
{
    Fixture *f = fx_new();

    /* ASL #4,A,B ; ASR #4,B,A ; LSL #4,A ; LSR #4,A */
    LOAD(f, 0,
         bits("0000 1100 0001 1101 0 000100 1"),
         bits("0000 1100 0001 1100 1 000100 0"),
         bits("0000 1100 0001 1110 10 00100 0"),
         bits("0000 1100 0001 1110 11 00100 0"),
         WAIT);
    set_acc(f, 0, 1);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, 0x10);
    ASSERT_ACC(f, 0, 1);                       /* the source is untouched */
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 1);                       /* 0x10 >> 4 */
    set_acc(f, 0, ACC(0, 0x123456, 0xabcdef));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x234560, 0xabcdef));
    ASSERT_CCR(f, C_, C_);                     /* last bit out is bit 3 of $1 */
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x023456, 0xabcdef));
    ASSERT_CCR(f, C_, 0);
    g_free(f);
}

/* ---- multiplier -------------------------------------------------------- */

static void test_mpy_mac(void)
{
    Fixture *f = fx_new();

    /* MPY X0,Y0,A ; MAC X0,Y0,A ; MAC -X0,Y0,A ; MPY -X0,Y0,B */
    LOAD(f, 0, NOMOVE | 0xd0, NOMOVE | 0xd2, NOMOVE | 0xd6,
         NOMOVE | 0xdc, WAIT);
    f->d.x0 = 0x400000;                        /* 0.5 */
    f->d.y0 = 0x400000;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x200000, 0));     /* 0.25 */
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x400000, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x200000, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, ACC(0xff, 0xe00000, 0));  /* -0.25 */
    ASSERT_CCR(f, N_, N_);
    g_free(f);
}

static void test_mpyr_rounds(void)
{
    Fixture *f = fx_new();

    /* MPYR X0,X0,A: product 2*3*3 = 18, rounded away */
    LOAD(f, 0, NOMOVE | 0x81, WAIT);
    f->d.x0 = 3;
    go(f);
    ASSERT_ACC(f, 0, 0);
    ASSERT_CCR(f, Z_, Z_);

    /* (2^22+1)^2 * 2 = 2^45 + 2^24 + 2: low half is below one half */
    f->d.x0 = 0x400001;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x200001, 0));
    g_free(f);
}

static void test_mac_rounds_up(void)
{
    Fixture *f = fx_new();

    /* MACR X0,X0,A on A = 0.xxx + a product whose low half exceeds 1/2 */
    LOAD(f, 0, NOMOVE | 0x83, WAIT);
    f->d.x0 = 0;
    set_acc(f, 0, ACC(0, 5, 0xc00000));
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 6, 0));
    g_free(f);
}

static void test_mpyi_and_pow2(void)
{
    Fixture *f = fx_new();

    /* MPYI #$400000,X0,A ; MPY X0,#1,B */
    LOAD(f, 0, bits("0000 0001 0100 0001 1100 0000"), 0x400000,
         bits("0000 0001 0000 0001 1101 1000"), WAIT);
    f->d.x0 = 0x400000;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x200000, 0));
    ASSERT_ACC(f, 1, ACC(0, 0x200000, 0));     /* 0.5 * 2^-1 */
    g_free(f);
}

static void test_mpy_unsigned_and_dmac(void)
{
    Fixture *f = fx_new();

    /* MPYuu X0,X0,A ; DMACss X0,X0,B */
    LOAD(f, 0, bits("0000 0001 0010 0111 1100 0000"),
         bits("0000 0001 0010 0100 1010 0000"), WAIT);
    f->d.x0 = 0xffffff;
    set_acc(f, 1, ACC(0, 0x123456, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, (uint64_t)0xffffff * 0xffffff * 2);
    f->d.x0 = 0x100000;
    dsp56300_step(&f->d);                      /* (B >> 24) + 2*x0^2 */
    ASSERT_ACC(f, 1, 0x123456ULL + (2ULL << 40));
    g_free(f);
}

/* ---- bit fields, normalisation, divide --------------------------------- */

static void test_clb_normf(void)
{
    Fixture *f = fx_new();

    /* CLB A,B ; NORMF X0,A */
    LOAD(f, 0, bits("0000 1100 0001 1110 0000 0001"),
         bits("0000 1100 0001 1110 0010 1000"), WAIT);
    set_acc(f, 0, ACC(0, 0x200000, 0));        /* ten leading zeros */
    f->d.x0 = 0xffffff;                        /* shift left by one */
    go(f);
    ASSERT_ACC(f, 1, ACC(0xff, 0xffffff, 0));  /* 9 - 10 = -1 */
    ASSERT_ACC(f, 0, ACC(0, 0x400000, 0));
    g_free(f);
}

static void test_bitfield(void)
{
    Fixture *f = fx_new();

    /* EXTRACT #CO,A,B ; EXTRACTU #CO,A,B ; INSERT #CO,X0,A ; MERGE X0,A */
    LOAD(f, 0,
         bits("0000 1100 0001 1000 0000 0001"), 0x008004,
         bits("0000 1100 0001 1000 1000 0001"), 0x008004,
         bits("0000 1100 0001 1001 0100 0000"), 0x008008,
         bits("0000 1100 0001 1011 1000 1000"),
         WAIT);
    set_acc(f, 0, 0xabc);                      /* bits 11..4 are $ab */
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, ACC(0xff, 0xffffff, 0xffffab));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, 0xab);
    f->d.x0 = 0xcd;
    set_acc(f, 0, 0);
    dsp56300_step(&f->d);                      /* field $cd at offset 8 */
    ASSERT_ACC(f, 0, 0xcd00);
    set_acc(f, 0, ACC(0, 0x000abc, 0));
    f->d.x0 = 0x123;
    dsp56300_step(&f->d);
    g_assert_cmphex(reg(f, DSP56300_REG_A1), ==, 0x123abc);
    g_free(f);
}

static void test_div_steps(void)
{
    Fixture *f = fx_new();

    /* DIV X0,A twice: 0.25 / 0.5 */
    LOAD(f, 0, bits("0000 0001 1000 0000 0100 0000"),
         bits("0000 0001 1000 0000 0100 0000"), WAIT);
    set_acc(f, 0, ACC(0, 0x200000, 0));
    f->d.x0 = 0x400000;
    f->d.sr &= ~C_;
    dsp56300_step(&f->d);                      /* 2*D - S = 0, C = 1 */
    ASSERT_ACC(f, 0, 0);
    ASSERT_CCR(f, C_ | V_, C_);
    dsp56300_step(&f->d);                      /* 2*D + C - S < 0, C = 0 */
    ASSERT_ACC(f, 0, ACC(0xff, 0xc00000, 1));
    ASSERT_CCR(f, C_ | V_, 0);
    g_free(f);
}

static void test_norm(void)
{
    Fixture *f = fx_new();

    /* NORM R0,A: E=0 U=1 Z=0 shifts left and decrements R0 */
    LOAD(f, 0, bits("0000 0001 1101 1000 0001 0101"), WAIT);
    set_acc(f, 0, 1ULL << 30);
    f->d.r[0] = 10;
    f->d.sr = (f->d.sr & ~0xff) | U_;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 1ULL << 31);
    g_assert_cmpuint(f->d.r[0], ==, 9);
    g_free(f);
}

/* ---- parallel moves ---------------------------------------------------- */

static void test_move_x_postinc(void)
{
    Fixture *f = fx_new();

    /* MOVE X:(R0)+,X0 ; MOVE X0,Y:(R1)- */
    LOAD(f, 0, 0x44d800, 0x4c5100, WAIT);
    f->mem[DSP56300_SPACE_X][0x100] = 0x112233;
    f->d.r[0] = 0x100;
    f->d.r[1] = 0x200;
    go(f);
    g_assert_cmphex(f->d.x0, ==, 0x112233);
    g_assert_cmphex(f->d.r[0], ==, 0x101);
    g_assert_cmphex(f->mem[DSP56300_SPACE_Y][0x200], ==, 0x112233);
    g_assert_cmphex(f->d.r[1], ==, 0x1ff);
    g_free(f);
}

static void test_move_immediates(void)
{
    Fixture *f = fx_new();

    /*
     * MOVE #$12,X0 (fraction) ; MOVE #$12,R0 (integer) ;
     * MOVE #$123456,Y1 (long) ; MOVE #$80,A
     */
    LOAD(f, 0, 0x241200, 0x301200, 0x4ff400, 0x123456, 0x2e8000, WAIT);
    go(f);
    g_assert_cmphex(f->d.x0, ==, 0x120000);
    g_assert_cmphex(f->d.r[0], ==, 0x12);
    g_assert_cmphex(f->d.y1, ==, 0x123456);
    ASSERT_ACC(f, 0, ACC(0xff, 0x800000, 0));  /* signed fraction */
    g_free(f);
}

static void test_move_long_and_regs(void)
{
    Fixture *f = fx_new();

    /* MOVE L:(R0)+,A10 ; MOVE A,B ; MOVE X0,A */
    LOAD(f, 0, 0x40d800, 0x21cf00, 0x208e00, WAIT);
    f->mem[DSP56300_SPACE_X][0x20] = 0x111111;
    f->mem[DSP56300_SPACE_Y][0x20] = 0x222222;
    f->d.r[0] = 0x20;
    f->d.x0 = 0x800000;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x21);
    ASSERT_ACC(f, 1, ACC(0, 0x111111, 0));     /* A1 only, B0 cleared */
    ASSERT_ACC(f, 0, ACC(0xff, 0x800000, 0));  /* A10 loaded, then X0 */
    g_free(f);
}

static void test_long_move_store_limits(void)
{
    Fixture *f = fx_new();

    /* MOVE A,L:$20: both words saturate with the extension in use */
    LOAD(f, 0, 0x482000, WAIT);                /* L:aa form, W=0, LLL=A */
    set_acc(f, 0, ACC(1, 0, 0));
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x20], ==, 0x7fffff);
    g_assert_cmphex(f->mem[DSP56300_SPACE_Y][0x20], ==, 0xffffff);
    g_free(f);
}

static void test_xy_dual_mac(void)
{
    Fixture *f = fx_new();

    /*
     * MAC X0,Y0,A  X:(R0)+,X0  Y:(R4)+,Y0: the multiply sees the old X0
     * and Y0, the loads land afterwards
     */
    LOAD(f, 0, 0xf098d2, WAIT);
    f->d.x0 = 0x400000;
    f->d.y0 = 0x400000;
    f->d.r[0] = 0x10;
    f->d.r[4] = 0x40;
    f->mem[DSP56300_SPACE_X][0x10] = 0x111111;
    f->mem[DSP56300_SPACE_Y][0x40] = 0x222222;
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 0x200000, 0));
    g_assert_cmphex(f->d.x0, ==, 0x111111);
    g_assert_cmphex(f->d.y0, ==, 0x222222);
    g_assert_cmphex(f->d.r[0], ==, 0x11);
    g_assert_cmphex(f->d.r[4], ==, 0x41);
    g_free(f);
}

static void test_class1_xr_ry(void)
{
    Fixture *f = fx_new();

    /* X:R class I: X:(R0)+,X0  A,Y0 ; R:Y class I: A,X1  Y:(R4)-,Y1 */
    LOAD(f, 0, bits("0001 00 0 0 1 0 011 000 0000 0000"),
         bits("0001 0 1 01 1 1 010 100 0000 0000"), WAIT);
    f->mem[DSP56300_SPACE_X][0x10] = 0x111111;
    f->mem[DSP56300_SPACE_Y][0x40] = 0x222222;
    f->d.r[0] = 0x10;
    f->d.r[4] = 0x40;
    set_acc(f, 0, ACC(0, 0x333333, 0));
    go(f);
    g_assert_cmphex(f->d.x0, ==, 0x111111);
    g_assert_cmphex(f->d.y0, ==, 0x333333);
    g_assert_cmphex(f->d.x1, ==, 0x333333);
    g_assert_cmphex(f->d.y1, ==, 0x222222);
    g_assert_cmphex(f->d.r[4], ==, 0x3f);
    g_free(f);
}

static void test_class2_moves(void)
{
    Fixture *f = fx_new();

    /* MOVE A,X:(R0)+ X0,A (A is stored before X0 replaces it) */
    LOAD(f, 0, 0x081800, WAIT);
    set_acc(f, 0, ACC(0, 0x123456, 0));
    f->d.x0 = 0x654321;
    f->d.r[0] = 0x30;
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x30], ==, 0x123456);
    ASSERT_ACC(f, 0, ACC(0, 0x654321, 0));
    g_free(f);
}

static void test_acc_move_limiting_and_scaling(void)
{
    Fixture *f = fx_new();

    /* MOVE A,X:$30 */
    LOAD(f, 0, 0x563000, WAIT);
    set_acc(f, 0, ACC(1, 0, 0));               /* A2 = 1: out of range */
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x30], ==, 0x7fffff);
    g_assert_cmphex(ccr(f) & L_, ==, L_);
    ASSERT_ACC(f, 0, ACC(1, 0, 0));            /* the register is not limited */

    /* scale down: the word is bits 48-25 */
    f->d.sr = (f->d.sr & ~(L_ | DSP56300_SR_S1)) | DSP56300_SR_S0;
    set_acc(f, 0, ACC(0, 0x400000, 0));
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x30], ==, 0x200000);
    g_assert_cmphex(ccr(f) & L_, ==, 0);

    /* scale up: $400000 << 1 overflows the word */
    f->d.sr = (f->d.sr & ~DSP56300_SR_S0) | DSP56300_SR_S1;
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x30], ==, 0x7fffff);
    g_assert_cmphex(ccr(f) & L_, ==, L_);
    g_free(f);
}

static void test_ifcc(void)
{
    Fixture *f = fx_new();

    /* ADD X0,A IFeq ; ADD X0,A IFne.U */
    LOAD(f, 0, 0x202a40, 0x203240, WAIT);
    f->d.x0 = 0x100000;
    f->d.sr |= Z_;
    dsp56300_step(&f->d);                      /* eq: executes */
    ASSERT_ACC(f, 0, ACC(0, 0x100000, 0));
    g_assert_cmphex(ccr(f) & Z_, ==, Z_);      /* IFcc leaves CCR alone */
    dsp56300_step(&f->d);                      /* ne is false: nothing */
    ASSERT_ACC(f, 0, ACC(0, 0x100000, 0));
    f->d.sr &= ~Z_;
    f->d.pc = 1;
    dsp56300_step(&f->d);                      /* .U updates the CCR */
    ASSERT_ACC(f, 0, ACC(0, 0x200000, 0));
    g_assert_cmphex(ccr(f) & U_, ==, U_);
    g_free(f);
}

static void test_tcc(void)
{
    Fixture *f = fx_new();

    /* TCC X0,A (carry clear) ; TCS X0,B (carry set) */
    LOAD(f, 0, 0x020040, bits("0000 0010 1000 0000 0100 1000"), WAIT);
    f->d.x0 = 0x123456;
    f->d.sr &= ~C_;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x123456, 0));
    dsp56300_step(&f->d);                      /* C clear: TCS does nothing */
    ASSERT_ACC(f, 1, 0);
    f->d.sr |= C_;
    f->d.pc = 1;
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, ACC(0, 0x123456, 0));
    g_free(f);
}

/* ---- program control --------------------------------------------------- */

static void test_jumps_and_calls(void)
{
    Fixture *f = fx_new();

    /* 0: JSR $20 ; 1: JEQ $30 (not taken) ; 2: JMP $10 */
    LOAD(f, 0, 0x0d0020, 0x0ea030, 0x0c0010);
    LOAD(f, 0x10, WAIT);
    LOAD(f, 0x20, INC_A, RTS);
    f->d.sr &= ~Z_;
    go(f);
    ASSERT_ACC(f, 0, 1);
    g_assert_cmphex(f->d.pc, ==, 0x11);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_jump_ea(void)
{
    Fixture *f = fx_new();

    /* JMP (R0) ; JSR $1234 via the absolute-address ea */
    LOAD(f, 0, 0x0ae080);
    LOAD(f, 0x40, WAIT);
    f->d.r[0] = 0x40;
    go(f);
    g_assert_cmphex(f->d.pc, ==, 0x41);

    LOAD(f, 0, 0x0bf080, 0x50);
    LOAD(f, 0x50, WAIT);
    go(f);
    g_assert_cmphex(f->d.pc, ==, 0x51);
    g_assert_cmphex(f->d.sp, ==, 1);
    g_assert_cmphex(f->d.ss[1][0], ==, 2);     /* return after the ext word */
    g_free(f);
}

static void test_branches(void)
{
    Fixture *f = fx_new();

    /* BRA +5 ; BNE +5 ; BSR (long) +6 ; WAIT */
    LOAD(f, 0, 0x050c05);
    LOAD(f, 5, 0x052405);
    LOAD(f, 10, bits("0000 1101 0001 0000 1000 0000"), 0x000006);
    LOAD(f, 12, WAIT);
    LOAD(f, 16, INC_A, RTS);
    f->d.sr &= ~Z_;
    go(f);
    ASSERT_ACC(f, 0, 1);
    g_assert_cmphex(f->d.pc, ==, 13);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_do_loop(void)
{
    Fixture *f = fx_new();

    /* DO #3,LA=3 ; INC A ; NOP ; WAIT */
    LOAD(f, 0, 0x060380, 3, INC_A, NOP, WAIT);
    go(f);
    ASSERT_ACC(f, 0, 3);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmpuint(f->d.sr & DSP56300_SR_LF, ==, 0);
    g_free(f);
}

static void test_do_register_and_dor(void)
{
    Fixture *f = fx_new();

    /* DO X0,LA=3 ; DOR #3 (relative) */
    LOAD(f, 0, 0x06c400, 3, INC_A, NOP, WAIT);
    f->d.x0 = 4;
    go(f);
    ASSERT_ACC(f, 0, 4);

    LOAD(f, 0, 0x060390, 3, INC_A, NOP, WAIT);
    set_acc(f, 0, 0);
    go(f);
    ASSERT_ACC(f, 0, 3);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_do_zero_skips(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, 0x060080, 3, INC_A, NOP, WAIT);   /* DO #0,3 */
    f->d.la = 0x77;
    f->d.lc = 0x88;
    go(f);
    ASSERT_ACC(f, 0, 0);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmphex(f->d.la, ==, 0x77);          /* untouched */
    g_assert_cmphex(f->d.lc, ==, 0x88);
    g_free(f);
}

static void test_nested_do(void)
{
    Fixture *f = fx_new();

    /* DO #2,7 { DO #3,5 { INC A ; NOP } NOP ; NOP } WAIT: six passes */
    LOAD(f, 0, 0x060280, 7, 0x060380, 5, INC_A, NOP, NOP, NOP, WAIT);
    go(f);
    ASSERT_ACC(f, 0, 6);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_do_forever_brk(void)
{
    Fixture *f = fx_new();

    /* DO FOREVER,LA=5 ; ADD #1,A ; CMP #3,A ; BRKeq ; NOP ; WAIT */
    LOAD(f, 0, 0x000203, 5, 0x014180, 0x014385, 0x00021a, NOP, WAIT);
    go(f);
    ASSERT_ACC(f, 0, ACC(0, 3, 0));
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmphex(f->d.pc, ==, 7);
    g_assert_cmpuint(f->d.sr & (DSP56300_SR_LF | DSP56300_SR_FV), ==, 0);
    g_free(f);
}

static void test_enddo(void)
{
    Fixture *f = fx_new();

    /* DO #10,3 { ENDDO ; NOP } ; WAIT */
    LOAD(f, 0, 0x060a80, 3, 0x00008c, NOP, WAIT);
    go(f);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmphex(f->d.pc, ==, 5);
    g_free(f);
}

static void test_rep(void)
{
    Fixture *f = fx_new();

    /* REP #5 ; INC A ; WAIT */
    LOAD(f, 0, 0x0605a0, INC_A, WAIT);
    f->d.lc = 77;
    go(f);
    ASSERT_ACC(f, 0, 5);
    g_assert_cmpuint(f->d.lc, ==, 77);         /* LC is restored */
    g_free(f);
}

static void test_bit_ops_memory(void)
{
    Fixture *f = fx_new();

    /* BSET #3,X:$10 ; BTST #3,X:$10 ; BCHG #0,X:$10 ; BCLR #3,X:$10 */
    LOAD(f, 0, 0x0a1023, 0x0b1023, 0x0b1000, 0x0a1003, WAIT);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x10], ==, 0x8);
    ASSERT_CCR(f, C_, 0);
    dsp56300_step(&f->d);
    ASSERT_CCR(f, C_, C_);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x10], ==, 0x9);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x10], ==, 0x1);
    ASSERT_CCR(f, C_, C_);
    g_free(f);
}

static void test_bit_ops_reg_and_io(void)
{
    Fixture *f = fx_new();

    /* BSET #0,R0 ; BSET #1,X:$FFFFC1 ; BCLR #1,X:$FFFFC1 */
    LOAD(f, 0, 0x0ad060, 0x0a8121, 0x0a8101, WAIT);
    f->d.r[0] = 0x100;
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.r[0], ==, 0x101);
    f->io[0][0x41] = 0x5;
    dsp56300_step(&f->d);
    g_assert_cmphex(f->io[0][0x41], ==, 0x7);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->io[0][0x41], ==, 0x5);
    g_assert_cmpuint(f->io_writes, ==, 2);
    g_free(f);
}

static void test_bit_jumps(void)
{
    Fixture *f = fx_new();

    /*
     * JSET #0,X:$10,$20 (taken) ; $20: JCLR #0,X:$10,$30 (not taken) ;
     * BRSET #0,X:(R0),+3 (taken)
     */
    f->mem[DSP56300_SPACE_X][0x10] = 1;
    LOAD(f, 0, 0x0a10a0, 0x20);
    LOAD(f, 0x20, 0x0a1080, 0x30, 0x0ca020, 3, NOP, WAIT);
    f->d.r[0] = 0x10;
    go(f);
    g_assert_cmphex(f->d.pc, ==, 0x26);
    g_assert_cmphex(f->d.r[0], ==, 0x10);
    g_free(f);
}

static void test_jsclr(void)
{
    Fixture *f = fx_new();

    /* JSCLR #0,X:$10,$20 ; WAIT ; $20: INC A ; RTS */
    LOAD(f, 0, 0x0b1080, 0x20, WAIT);
    LOAD(f, 0x20, INC_A, RTS);
    go(f);
    ASSERT_ACC(f, 0, 1);
    g_assert_cmphex(f->d.pc, ==, 3);
    g_free(f);
}

static void test_movec_movep_movem(void)
{
    Fixture *f = fx_new();

    /*
     * MOVEC #5,M0 ; MOVEC X0,SR ; MOVEP X0,X:$FFFFC5 ;
     * MOVEP X:$FFFFC5,Y1 ; MOVEM P:(R0),X1 ; MOVEP X0,X:$FFFF82
     */
    LOAD(f, 0, 0x0505a0, 0x04c4b9, 0x08c405, 0x084705, 0x07e085, 0x04c482,
         WAIT);
    f->d.x0 = 0x000300;
    f->mem[DSP56300_SPACE_P][0x80] = 0xabcdef;
    f->d.r[0] = 0x80;
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.m[0], ==, 5);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.sr, ==, 0x000300);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->io[0][0x45], ==, 0x000300);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.y1, ==, 0x000300);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.x1, ==, 0xabcdef);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->io[0][0x02], ==, 0x000300);
    g_free(f);
}

static void test_stack_registers(void)
{
    Fixture *f = fx_new();

    /* MOVEC X0,SSH pushes ; MOVEC SSH,X1 pulls */
    LOAD(f, 0, 0x04c4bc, 0x0445bc, WAIT);
    f->d.x0 = 0x424242;
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.sp, ==, 1);
    g_assert_cmphex(f->d.ss[1][0], ==, 0x424242);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.x1, ==, 0x424242);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_lua_lra_disp(void)
{
    Fixture *f = fx_new();

    /*
     * LUA (R0)+N0,R1 ; LRA R2,R3 ; MOVE X:(R0+3),X0 ;
     * MOVE X:(R0+$800),X1 ; MOVE X0,X:(R0+$800)
     */
    LOAD(f, 0, 0x044811, 0x04c213, 0x0208d4, 0x0a70c5, 0x800, WAIT);
    f->d.r[0] = 0x100;
    f->d.n[0] = 4;
    f->d.r[2] = 0x40;
    f->mem[DSP56300_SPACE_X][0x103] = 0x555555;
    f->mem[DSP56300_SPACE_X][0x900] = 0x777777;
    go(f);
    g_assert_cmphex(f->d.r[1], ==, 0x104);     /* updated, R0 itself is not */
    g_assert_cmphex(f->d.r[0], ==, 0x100);
    g_assert_cmphex(f->d.r[3], ==, 0x41);      /* PC of the LRA plus R2 */
    g_assert_cmphex(f->d.x0, ==, 0x555555);
    g_assert_cmphex(f->d.x1, ==, 0x777777);
    g_free(f);
}

static void test_andi_ori(void)
{
    Fixture *f = fx_new();

    /* ORI #5,CCR ; ANDI #$fe,CCR ; ORI #3,MR */
    LOAD(f, 0, 0x0005f9, 0x00feb9, 0x0003f8, WAIT);
    f->d.sr &= ~(0xff | SR_I_MASK);
    dsp56300_step(&f->d);
    g_assert_cmphex(ccr(f), ==, 0x05);
    dsp56300_step(&f->d);
    g_assert_cmphex(ccr(f), ==, 0x04);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.sr & SR_I_MASK, ==, SR_I_MASK);
    g_free(f);
}

/* ---- addressing modifiers ---------------------------------------------- */

#define MOVE_X_R0_POSTINC  0x44d800     /* MOVE X:(R0)+,X0 */
#define MOVE_X_R0_POSTN    0x44c800     /* MOVE X:(R0)+N0,X0 */
#define MOVE_X_R0_POSTDEC  0x44d000     /* MOVE X:(R0)-,X0 */

static void test_modulo(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTINC, MOVE_X_R0_POSTINC, MOVE_X_R0_POSTINC, WAIT);
    f->d.m[0] = 3;                             /* modulo 4 */
    f->d.r[0] = 0x102;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x101);     /* 103, 100, 101 */
    g_free(f);
}

static void test_modulo_with_offset(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTN, WAIT);
    f->d.m[0] = 7;                             /* modulo 8 */
    f->d.n[0] = 3;
    f->d.r[0] = 0x105;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x100);     /* 108 wraps to 100 */
    g_free(f);
}

static void test_modulo_decrement(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTDEC, WAIT);
    f->d.m[0] = 3;
    f->d.r[0] = 0x100;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x103);
    g_free(f);
}

static void test_bit_reverse(void)
{
    Fixture *f = fx_new();
    static const uint32_t seq[] = { 4, 2, 6, 1, 5, 3, 7, 0 };

    for (int i = 0; i < 8; i++) {
        LOAD(f, i, MOVE_X_R0_POSTN);
    }
    f->d.m[0] = 0;                             /* reverse carry */
    f->d.n[0] = 4;                             /* 8-point FFT */
    f->d.r[0] = 0;
    f->d.pc = 0;
    for (int i = 0; i < 8; i++) {
        dsp56300_step(&f->d);
        g_assert_cmphex(f->d.r[0], ==, seq[i]);
    }
    g_free(f);
}

static void test_multiple_wrap(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTN, WAIT);
    f->d.m[0] = 0x8003;                        /* wrap-around modulo 4 */
    f->d.n[0] = 6;
    f->d.r[0] = 0x101;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x103);     /* (1 + 6) mod 4 = 3 */
    g_free(f);
}

static void test_modulo_block_jump(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTN, WAIT);
    f->d.m[0] = 7;                             /* modulo 8 */
    f->d.n[0] = 16;                            /* two blocks on, same offset */
    f->d.r[0] = 0x103;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0x113);
    g_free(f);
}

static void test_linear(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, MOVE_X_R0_POSTINC, WAIT);
    f->d.r[0] = 0xffffff;
    go(f);
    g_assert_cmphex(f->d.r[0], ==, 0);
    g_free(f);
}

/* ---- interrupts -------------------------------------------------------- */

static void test_fast_interrupt(void)
{
    Fixture *f = fx_new();

    /* main: INC A ... JMP $100 ; vector $10 (IRQA): INC B ; NOP */
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x100, INC_A, INC_A, INC_A, INC_A, 0x0c0100);
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    dsp56300_step(&f->d);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    dsp56300_step(&f->d);                      /* takes the interrupt */
    ASSERT_ACC(f, 1, 1);
    g_assert_cmphex(f->d.pc, ==, 0x101);       /* flow resumes, no stack */
    g_assert_cmphex(f->d.sp, ==, 0);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, 2);
    ASSERT_ACC(f, 1, 1);                       /* an edge is taken once */
    g_free(f);
}

static void test_long_interrupt_and_rti(void)
{
    Fixture *f = fx_new();

    /* vector $12 (IRQB): JSR $200. handler: MOVEC SR,X:$10 ; INC B ; RTI */
    LOAD(f, 0x12, 0x0d0200);
    LOAD(f, 0x200, 0x051039, INC_B, RTI);
    LOAD(f, 0x100, INC_A, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr = (f->d.sr & ~SR_I_MASK) | DSP56300_SR_LF | DSP56300_SR_S0;
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQB, 1);
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 2);
    /* in the handler: mask raised to IPL 2, LF and scaling cleared */
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x10] & 0xf00, ==, 0x200);
    g_assert_cmpuint(f->mem[DSP56300_SPACE_X][0x10] &
                     (DSP56300_SR_LF | DSP56300_SR_S0), ==, 0);
    /* RTI brought the old status back */
    g_assert_cmphex(f->d.sr & (DSP56300_SR_LF | DSP56300_SR_S0 | SR_I_MASK),
                    ==, DSP56300_SR_LF | DSP56300_SR_S0);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_interrupt_priority(void)
{
    Fixture *f = fx_new();

    /*
     * IRQA (IPL 0): JSR $300 / INC A / RTI ; IRQB (IPL 2): JSR $310 /
     * INC B / RTI. Both pending: IPL 2 first, and it masks the other.
     */
    LOAD(f, 0x10, 0x0d0300, NOP);
    LOAD(f, 0x12, 0x0d0310, NOP);
    LOAD(f, 0x300, INC_A, RTI);
    LOAD(f, 0x310, INC_B, RTI);
    LOAD(f, 0x100, NOP, NOP, NOP, WAIT);
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQB, 2);
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.pc, ==, 0x310);
    g_assert_cmpuint(f->d.irq_pending[DSP56300_VEC_IRQA >> 1], ==, 1);
    dsp56300_step(&f->d);                      /* INC B: A stays masked */
    ASSERT_ACC(f, 0, 0);
    ASSERT_ACC(f, 1, 1);
    run(f);
    ASSERT_ACC(f, 0, 1);
    ASSERT_ACC(f, 1, 1);
    g_free(f);
}

static void test_interrupt_mask(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x100, NOP, NOP, WAIT, WAIT);
    f->d.pc = 0x100;
    f->d.sr = (f->d.sr & ~SR_I_MASK) | DSP56300_SR_I1;   /* IPL 2 and up */
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 1);
    run(f);
    ASSERT_ACC(f, 1, 0);
    f->d.sr &= ~SR_I_MASK;
    run(f);                                    /* now it gets through */
    ASSERT_ACC(f, 1, 1);
    g_free(f);
}

static void test_level_interrupt(void)
{
    Fixture *f = fx_new();

    /* a level-sensitive request stays pending until the device drops it */
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x100, NOP, NOP, NOP, WAIT);
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    dsp56300_irq_raise_level(&f->d, DSP56300_VEC_IRQA, 0);
    dsp56300_step(&f->d);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, 2);
    dsp56300_irq_lower(&f->d, DSP56300_VEC_IRQA);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, 2);
    g_free(f);
}

static void test_trap_is_not_maskable(void)
{
    Fixture *f = fx_new();

    /* TRAP at $100 with the mask at 3; vector $08: INC B ; NOP */
    LOAD(f, 0x08, INC_B, NOP);
    LOAD(f, 0x100, 0x000006, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr |= SR_I_MASK;
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 1);                       /* resumed after the TRAP */
    g_free(f);
}

static void test_illegal_instruction(void)
{
    Fixture *f = fx_new();

    /* $000007 is not an instruction: vector $04: INC B ; NOP */
    LOAD(f, 0x04, INC_B, NOP);
    LOAD(f, 0x100, 0x000007, INC_A, WAIT);
    f->d.pc = 0x100;
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 1);
    g_free(f);
}

static void test_vector_base(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0x400 + 0x10, INC_B, NOP);
    LOAD(f, 0x100, NOP, WAIT);
    f->d.vba = 0x400;
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    ASSERT_ACC(f, 1, 1);
    g_free(f);
}

static void test_wait_wakes(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x100, WAIT, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_WAIT);
    ASSERT_ACC(f, 1, 0);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 1);                       /* resumed after the WAIT */
    g_free(f);
}

static void test_rep_is_uninterruptible(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x100, 0x0664a0, INC_A, WAIT);     /* REP #100 ; INC A */
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    dsp56300_step(&f->d);                      /* REP */
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    for (int i = 0; i < 100; i++) {
        dsp56300_step(&f->d);
        ASSERT_ACC(f, 1, 0);                   /* not taken inside the REP */
    }
    ASSERT_ACC(f, 0, 100);
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, 1);
    g_free(f);
}

static void test_stack_error(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, 0x0d0020);                      /* JSR $20 */
    f->d.sp = 0x0f;                            /* stack full */
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.sp & 0x30, ==, 0x10); /* overflow flagged */
    g_assert_cmpuint(f->d.irq_pending[DSP56300_VEC_STACK_ERROR >> 1], ==, 1);
    g_free(f);
}

/* ---- idle detection and cycles ----------------------------------------- */

static void test_idle_poll_loop(void)
{
    Fixture *f = fx_new();

    /* JCLR #0,X:$FFFFC0,0 spins until the peripheral bit is set */
    LOAD(f, 0, 0x0a8080, 0, WAIT);
    f->io[0][0x40] = 0;
    f->d.pc = 0;
    run(f);
    g_assert_cmphex(f->d.pc, ==, 0);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_RUNNING);
    f->io[0][0x40] = 1;
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_WAIT);
    g_free(f);
}

static void test_idle_two_instruction_loop(void)
{
    Fixture *f = fx_new();

    /* MOVEP X:$FFFFC1,X0 ; JCLR #0,X0,0: polls through a register */
    LOAD(f, 0, 0x084401, 0x0ac400, 0, WAIT);
    f->io[0][0x41] = 0;
    f->d.pc = 0;
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_RUNNING);
    f->io[0][0x41] = 1;
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_WAIT);
    g_free(f);
}

static void test_idle_not_when_state_changes(void)
{
    Fixture *f = fx_new();

    /* INC A ; JMP 0: A changes on every pass, so this is not a wait loop */
    LOAD(f, 0, INC_A, 0x0c0000);
    f->d.pc = 0;
    g_assert_cmpuint(dsp56300_run(&f->d, 1000), >=, 1000);
    g_assert_false(f->d.idle);
    g_assert_cmpuint(acc(f, 0), >, 100);
    g_free(f);
}

static void test_cycle_counts(void)
{
    Fixture *f = fx_new();

    LOAD(f, 0, NOP, 0x0c0003, NOP, NOMOVE | 0x40, 0x0605a0, NOP, WAIT);
    g_assert_cmpuint(dsp56300_step(&f->d), ==, 1);     /* NOP */
    g_assert_cmpuint(dsp56300_step(&f->d), ==, 3);     /* JMP */
    g_assert_cmpuint(dsp56300_step(&f->d), ==, 1);     /* ADD X0,A */
    g_assert_cmpuint(dsp56300_step(&f->d), ==, 5);     /* REP #5 */
    g_assert_cmpuint(dsp56300_run(&f->d, 5), ==, 5);   /* five repeats */
    g_assert_cmpuint(f->d.cycles, ==, 5);              /* only run() adds */
    g_free(f);
}


static void test_call_on_bit(void)
{
    Fixture *f = fx_new();

    /* BSCLR #0,X:$10,+4: calls ipc+4 when the bit is clear */
    LOAD(f, 0, 0x0d9080, 4, WAIT);
    LOAD(f, 4, INC_A, RTS);
    go(f);
    ASSERT_ACC(f, 0, 1);
    g_assert_cmphex(f->d.pc, ==, 3);
    g_assert_cmphex(f->d.sp, ==, 0);

    /* with the bit set it falls through, past the displacement word */
    f->mem[DSP56300_SPACE_X][0x10] = 1;
    set_acc(f, 0, 0);
    go(f);
    ASSERT_ACC(f, 0, 0);
    g_assert_cmphex(f->d.pc, ==, 3);
    g_free(f);
}

static void test_jcc_ea(void)
{
    Fixture *f = fx_new();

    /* JEQ (R0) */
    LOAD(f, 0, 0x0ae0aa);
    LOAD(f, 0x30, WAIT);
    f->d.r[0] = 0x30;
    f->d.sr |= Z_;
    go(f);
    g_assert_cmphex(f->d.pc, ==, 0x31);
    f->d.sr &= ~Z_;
    f->d.pc = 0;
    f->d.run_state = DSP56300_RUNNING;
    dsp56300_step(&f->d);
    g_assert_cmphex(f->d.pc, ==, 1);           /* not taken */
    g_free(f);
}

static void test_trapcc(void)
{
    Fixture *f = fx_new();

    /* TRAPeq ; INC A ; WAIT: only taken when Z is set */
    LOAD(f, 0x08, INC_B, NOP);
    LOAD(f, 0x100, 0x00001a, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr &= ~Z_;
    run(f);
    ASSERT_ACC(f, 1, 0);
    f->d.sr |= Z_;
    f->d.pc = 0x100;
    f->d.run_state = DSP56300_RUNNING;
    run(f);
    ASSERT_ACC(f, 1, 1);
    g_free(f);
}

static void test_cache_instructions(void)
{
    Fixture *f = fx_new();

    /* PFLUSH is an illegal instruction while the cache is off */
    LOAD(f, 0x04, INC_B, NOP);
    LOAD(f, 0x100, 0x000003, INC_A, WAIT);
    f->d.pc = 0x100;
    run(f);
    ASSERT_ACC(f, 1, 1);
    f->d.sr |= DSP56300_SR_CE;
    f->d.pc = 0x100;
    f->d.run_state = DSP56300_RUNNING;
    run(f);
    ASSERT_ACC(f, 1, 1);                       /* a NOP now */
    g_free(f);
}

static void test_stop_and_reset_instruction(void)
{
    Fixture *f = fx_new();

    /* RESET ; STOP: an interrupt gets the core out */
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0, 0x000084, 0x000087, INC_A, WAIT);
    f->d.sr &= ~SR_I_MASK;
    run(f);
    g_assert_cmpuint(f->periph_resets, ==, 1);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_STOP);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 1);
    g_free(f);
}

static void test_shift_by_register_and_cmpu(void)
{
    Fixture *f = fx_new();

    /* ASL X0,A,B ; NORMF X0,A (positive: right) ; CMPU X0,A */
    LOAD(f, 0, 0x0c1e49, 0x0c1e28, 0x0c1ff8, WAIT);
    f->d.x0 = 3;
    set_acc(f, 0, ACC(0, 0x000100, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 1, ACC(0, 0x000800, 0));
    dsp56300_step(&f->d);
    ASSERT_ACC(f, 0, ACC(0, 0x000020, 0));
    /* unsigned: $000020 against X0 = $800000 aligned left borrows */
    f->d.x0 = 0x800000;
    dsp56300_step(&f->d);
    ASSERT_CCR(f, C_ | Z_ | V_, C_);
    set_acc(f, 0, ACC(0, 0x900000, 0));
    f->d.pc = 2;
    dsp56300_step(&f->d);
    ASSERT_CCR(f, C_ | Z_ | V_, 0);
    g_free(f);
}

static void test_vsl(void)
{
    Fixture *f = fx_new();

    /* VSL A,1,L:(R0)+ */
    LOAD(f, 0, 0x0ad8d0, WAIT);
    set_acc(f, 0, ACC(0, 0x123456, 0x400001));
    f->d.r[0] = 0x20;
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x20], ==, 0x123456);
    g_assert_cmphex(f->mem[DSP56300_SPACE_Y][0x20], ==, 0x800003);
    g_assert_cmphex(f->d.r[0], ==, 0x21);
    g_free(f);
}

static void test_displacement_store(void)
{
    Fixture *f = fx_new();

    /* MOVE X0,X:(R0+$800) */
    LOAD(f, 0, 0x0a7084, 0x800, WAIT);
    f->d.x0 = 0x314159;
    f->d.r[0] = 0x100;
    go(f);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x900], ==, 0x314159);
    g_free(f);
}

static void test_loop_survives_interrupts(void)
{
    Fixture *f = fx_new();

    /* DO #100,LA=3 { INC A ; NOP } ; the interrupt lands mid-loop */
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0, 0x066480, 3, INC_A, NOP, WAIT);
    f->d.sr &= ~SR_I_MASK;
    for (int i = 0; i < 20; i++) {
        dsp56300_step(&f->d);
    }
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    ASSERT_ACC(f, 0, 100);
    ASSERT_ACC(f, 1, 1);
    g_assert_cmphex(f->d.sp, ==, 0);

    /* the same with a long interrupt: LF is cleared inside and restored */
    f = fx_new();
    LOAD(f, 0x12, 0x0d0200);
    LOAD(f, 0x200, 0x0a1023, INC_B, RTI);      /* BSET #3,X:$10 ; INC B */
    LOAD(f, 0, 0x066480, 3, INC_A, NOP, WAIT);
    f->d.sr &= ~SR_I_MASK;
    for (int i = 0; i < 20; i++) {
        dsp56300_step(&f->d);
    }
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQB, 0);
    run(f);
    ASSERT_ACC(f, 0, 100);
    ASSERT_ACC(f, 1, 1);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_assert_cmphex(f->mem[DSP56300_SPACE_X][0x10], ==, 8);
    g_free(f);
}



/* ---- stack flags, STOP, REP restrictions, migration -------------------- */

static void test_stack_counter_flags(void)
{
    Fixture *f = fx_new();

    /* REP #32 { MOVEC X0,SSH }: the pointer wraps, SE sticks, UF stays 0 */
    LOAD(f, 0x02, INC_B, NOP);
    LOAD(f, 0x100, 0x0620a0, 0x04c4bc, WAIT);
    f->d.sr &= ~SR_I_MASK;
    f->d.pc = 0x100;
    run(f);
    g_assert_cmphex(f->d.sp & 0x3f, ==, 0x10);
    ASSERT_ACC(f, 1, 1);

    /* 40 pushes then 40 pulls: one exception in all; the underflow adds UF */
    f = fx_new();
    LOAD(f, 0x02, INC_B, NOP);
    LOAD(f, 0x100, 0x0628a0, 0x04c4bc, 0x0628a0, 0x0445bc, WAIT);
    f->d.sr &= ~SR_I_MASK;
    f->d.pc = 0x100;
    run(f);
    g_assert_cmphex(f->d.sp & 0x30, ==, 0x30);
    ASSERT_ACC(f, 1, 1);

    /* a write to SP clears the flags */
    dsp56300_set_reg(&f->d, DSP56300_REG_SP, 0);
    g_assert_cmphex(f->d.sp, ==, 0);
    g_free(f);
}

static void test_stop_wake(void)
{
    Fixture *f = fx_new();

    /* STOP ; INC A ; WAIT. IRQB vector: INC A ; NOP. IRQA vector: INC B */
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x12, INC_A, NOP);
    LOAD(f, 0x100, 0x000087, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr = f->d.sr | SR_I_MASK;             /* IPL 0-2 masked */
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_STOP);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQB, 1);
    run(f);
    g_assert_cmpint(f->d.run_state, ==, DSP56300_STOP);   /* not IRQA */
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    /*
     * IRQA wakes the core even though it is masked; nothing is unmasked,
     * so execution resumes after the STOP.
     */
    g_assert_cmpint(f->d.run_state, ==, DSP56300_WAIT);
    ASSERT_ACC(f, 0, 1);
    ASSERT_ACC(f, 1, 0);
    g_free(f);

    /* unmasked: the highest-priority pending interrupt is serviced */
    f = fx_new();
    LOAD(f, 0x10, INC_B, NOP);
    LOAD(f, 0x12, INC_A, NOP);
    LOAD(f, 0x100, 0x000087, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.sr &= ~SR_I_MASK;
    run(f);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQB, 2);
    dsp56300_irq_raise(&f->d, DSP56300_VEC_IRQA, 0);
    run(f);
    ASSERT_ACC(f, 0, 2);                       /* IRQB, then the INC A */
    ASSERT_ACC(f, 1, 1);                       /* IRQA behind it */
    g_free(f);
}

static void test_rep_forbidden(void)
{
    Fixture *f = fx_new();

    /* REP #3 ; WAIT must not hang: illegal instruction, REP abandoned */
    LOAD(f, 0x04, INC_B, NOP);
    LOAD(f, 0x100, 0x0603a0, WAIT, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.lc = 9;
    run(f);
    ASSERT_ACC(f, 1, 1);
    ASSERT_ACC(f, 0, 1);
    g_assert_cmpuint(f->d.lc, ==, 9);
    g_assert_false(f->d.rep_active);

    /* a jump in a REP is refused too, and does not jump */
    LOAD(f, 0x100, 0x0603a0, 0x0c0200, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.run_state = DSP56300_RUNNING;
    run(f);
    ASSERT_ACC(f, 1, 2);
    ASSERT_ACC(f, 0, 2);

    /* and a REP of a REP or a DO */
    LOAD(f, 0x100, 0x0603a0, 0x0603a0, INC_A, WAIT);
    f->d.pc = 0x100;
    f->d.run_state = DSP56300_RUNNING;
    run(f);
    ASSERT_ACC(f, 1, 3);

    /* ENDDO, BRKcc, TRAP and TRAPcc are refused as well */
    {
        static const uint32_t forbidden[] = {
            0x00008c, 0x00021a, 0x000006, 0x00001a,
        };

        for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++) {
            f->d.sr |= Z_;
            LOAD(f, 0x100, 0x0603a0, forbidden[i], INC_A, WAIT);
            f->d.pc = 0x100;
            f->d.run_state = DSP56300_RUNNING;
            run(f);
            ASSERT_ACC(f, 1, 4 + i);
            g_assert_false(f->d.rep_active);
        }
    }
    g_free(f);
}

static void test_reserved_bits_masked(void)
{
    Fixture *f = fx_new();

    /* ORI #$10,MR (bit 12 is reserved) ; ORI #$20,COM (OMR bit 5) */
    LOAD(f, 0, 0x0010f8, 0x0020fa, WAIT);
    go(f);
    g_assert_cmphex(f->d.sr & (1u << 12), ==, 0);
    g_assert_cmphex(f->d.omr & 0x20, ==, 0);
    g_free(f);
}

static int vmstate_roundtrip(DSP56300State *from, DSP56300State *to)
{
    char *path = NULL;
    int fd = g_file_open_tmp("test-dsp56300-XXXXXX", &path, NULL);
    QIOChannel *ioc;
    QEMUFile *f;
    Error *err = NULL;
    int ret;

    g_assert_cmpint(fd, >=, 0);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(dup(fd)));
    f = qemu_file_new_output(ioc);
    object_unref(OBJECT(ioc));
    g_assert_cmpint(vmstate_save_state(f, &vmstate_dsp56300, from, NULL,
                                       &err), ==, 0);
    qemu_fclose(f);
    lseek(fd, 0, SEEK_SET);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(dup(fd)));
    f = qemu_file_new_input(ioc);
    object_unref(OBJECT(ioc));
    ret = vmstate_load_state(f, &vmstate_dsp56300, to, 1, &err);
    if (ret) {
        error_free(err);
    }
    qemu_fclose(f);
    close(fd);
    unlink(path);
    g_free(path);
    return ret;
}

/* Move the state to a second fixture with the same memory and finish there. */
static Fixture *migrate(Fixture *a)
{
    Fixture *b = fx_new();

    memcpy(b->mem, a->mem, sizeof(a->mem));
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), ==, 0);
    g_assert_true(b->d.ops.read == fx_read);     /* wiring is untouched */
    g_assert_true(b->d.mem[0] == b->mem[0]);
    return b;
}

static void test_migration_mid_rep_and_do(void)
{
    Fixture *a = fx_new(), *b;

    /* REP #1000 { INC A } stopped part-way */
    LOAD(a, 0, 0x06e8a3, INC_A, WAIT);            /* REP #$3e8 */
    a->d.lc = 5;
    dsp56300_step(&a->d);
    for (int i = 0; i < 100; i++) {
        dsp56300_step(&a->d);
    }
    g_assert_true(a->d.rep_active);
    b = migrate(a);
    g_assert_true(b->d.rep_active);
    g_assert_cmpuint(b->d.rep_count, ==, a->d.rep_count);
    run(a);
    run(b);
    ASSERT_ACC(b, 0, 1000);
    g_assert_cmpuint(b->d.lc, ==, 5);
    g_assert_cmphex(b->d.pc, ==, a->d.pc);
    g_assert_cmpuint(b->d.cycles, ==, a->d.cycles);
    g_free(a);
    g_free(b);

    /* DO #10 stopped in the loop, with an interrupt pending behind a mask */
    a = fx_new();
    LOAD(a, 0x10, INC_B, NOP);
    LOAD(a, 0, 0x060a80, 3, INC_A, NOP, WAIT);
    a->d.sr |= SR_I_MASK;
    dsp56300_irq_raise(&a->d, DSP56300_VEC_IRQA, 1);
    for (int i = 0; i < 7; i++) {
        dsp56300_step(&a->d);
    }
    g_assert_cmpuint(a->d.sp, ==, 2);
    b = migrate(a);
    g_assert_cmpuint(b->d.irq_count, ==, 1);
    g_assert_cmphex(b->d.sp, ==, a->d.sp);
    g_assert_cmpuint(b->d.lc, ==, a->d.lc);
    g_assert_cmphex(b->d.la, ==, a->d.la);
    b->d.sr &= ~SR_I_MASK;
    a->d.sr &= ~SR_I_MASK;
    run(a);
    run(b);
    ASSERT_ACC(b, 0, 10);
    ASSERT_ACC(b, 1, 1);
    ASSERT_ACC(b, 0, acc(a, 0));
    g_assert_cmphex(b->d.sp, ==, 0);
    g_assert_cmpuint(b->d.cycles, ==, a->d.cycles);
    g_free(a);
    g_free(b);
}

static void test_migration_rejects(void)
{
    Fixture *a = fx_new(), *b = fx_new();

    a->d.rep_active = true;
    a->d.rep_count = 0;
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), <, 0);
    a->d.rep_active = false;
    a->d.sp = 0x40;
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), <, 0);
    a->d.sp = 0;
    a->d.run_state = 9;
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), <, 0);
    a->d.run_state = DSP56300_RUNNING;
    a->d.irq_ipl[5] = 4;
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), <, 0);
    a->d.irq_ipl[5] = 0;
    a->d.r[3] = 0x1000000;
    g_assert_cmpint(vmstate_roundtrip(&a->d, &b->d), <, 0);
    g_free(a);
    g_free(b);
}

/* Throughput of a dual-move MAC loop; only with -m perf. */
static void test_throughput(void)
{
    static const uint64_t budget = 50000000;

    if (!g_test_perf()) {
        g_test_skip("run with -m perf");
        return;
    }
    for (int masked = 0; masked < 2; masked++) {
        Fixture *f = fx_new();
        int64_t t0, t1;

        /* DO FOREVER { MAC X0,Y0,A X:(R0)+,X0 Y:(R4)+,Y0 ; same } */
        LOAD(f, 0, 0x000203, 3, 0xf098d2, 0xf098d2, WAIT);
        f->d.m[0] = f->d.m[4] = 0xff;
        f->d.sr |= SR_I_MASK;
        if (masked) {
            dsp56300_irq_raise_level(&f->d, DSP56300_VEC_IRQA, 0);
        }
        t0 = g_get_monotonic_time();
        g_assert_cmpuint(dsp56300_run(&f->d, budget), >=, budget);
        t1 = g_get_monotonic_time();
        g_test_message("%s: %.1f M DSP cycles/s", masked ?
                       "masked interrupt pending" : "no interrupt pending",
                       (double)budget / (double)(t1 - t0));
        g_free(f);
    }
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/dsp56300/reset", test_reset);
    g_test_add_func("/dsp56300/alu/add-sub", test_add_sub);
    g_test_add_func("/dsp56300/alu/add-extension", test_add_into_extension);
    g_test_add_func("/dsp56300/alu/add-carry-overflow",
                    test_add_carry_and_overflow);
    g_test_add_func("/dsp56300/alu/sub-borrow", test_sub_borrow);
    g_test_add_func("/dsp56300/alu/inc-dec-overflow", test_inc_dec_overflow);
    g_test_add_func("/dsp56300/alu/cmp", test_cmp_does_not_store);
    g_test_add_func("/dsp56300/alu/clr-abs-neg-tst", test_clr_abs_neg_tst);
    g_test_add_func("/dsp56300/alu/addl-addr-subl", test_addl_addr_subl);
    g_test_add_func("/dsp56300/alu/adc-sbc", test_adc_sbc);
    g_test_add_func("/dsp56300/alu/max", test_max);
    g_test_add_func("/dsp56300/alu/immediate", test_immediate_alu);
    g_test_add_func("/dsp56300/alu/saturation", test_saturation_mode);
    g_test_add_func("/dsp56300/alu/round", test_round);
    g_test_add_func("/dsp56300/logic/ops", test_logic);
    g_test_add_func("/dsp56300/logic/immediate", test_logic_immediate);
    g_test_add_func("/dsp56300/shift/asl-asr", test_shifts);
    g_test_add_func("/dsp56300/shift/logical-rotate",
                    test_logical_shifts_rotates);
    g_test_add_func("/dsp56300/shift/multibit", test_multibit_shifts);
    g_test_add_func("/dsp56300/mul/mpy-mac", test_mpy_mac);
    g_test_add_func("/dsp56300/mul/mpyr", test_mpyr_rounds);
    g_test_add_func("/dsp56300/mul/macr", test_mac_rounds_up);
    g_test_add_func("/dsp56300/mul/immediate", test_mpyi_and_pow2);
    g_test_add_func("/dsp56300/mul/unsigned-dmac", test_mpy_unsigned_and_dmac);
    g_test_add_func("/dsp56300/bitfield/clb-normf", test_clb_normf);
    g_test_add_func("/dsp56300/bitfield/extract-insert", test_bitfield);
    g_test_add_func("/dsp56300/div", test_div_steps);
    g_test_add_func("/dsp56300/norm", test_norm);
    g_test_add_func("/dsp56300/move/x-postinc", test_move_x_postinc);
    g_test_add_func("/dsp56300/move/immediate", test_move_immediates);
    g_test_add_func("/dsp56300/move/long-regs", test_move_long_and_regs);
    g_test_add_func("/dsp56300/move/long-limits", test_long_move_store_limits);
    g_test_add_func("/dsp56300/move/xy-dual", test_xy_dual_mac);
    g_test_add_func("/dsp56300/move/class1", test_class1_xr_ry);
    g_test_add_func("/dsp56300/move/class2", test_class2_moves);
    g_test_add_func("/dsp56300/move/limit-scale",
                    test_acc_move_limiting_and_scaling);
    g_test_add_func("/dsp56300/ifcc", test_ifcc);
    g_test_add_func("/dsp56300/tcc", test_tcc);
    g_test_add_func("/dsp56300/flow/jumps", test_jumps_and_calls);
    g_test_add_func("/dsp56300/flow/jump-ea", test_jump_ea);
    g_test_add_func("/dsp56300/flow/branches", test_branches);
    g_test_add_func("/dsp56300/loop/do", test_do_loop);
    g_test_add_func("/dsp56300/loop/do-reg-dor", test_do_register_and_dor);
    g_test_add_func("/dsp56300/loop/do-zero", test_do_zero_skips);
    g_test_add_func("/dsp56300/loop/nested", test_nested_do);
    g_test_add_func("/dsp56300/loop/forever-brk", test_do_forever_brk);
    g_test_add_func("/dsp56300/loop/enddo", test_enddo);
    g_test_add_func("/dsp56300/loop/rep", test_rep);
    g_test_add_func("/dsp56300/bit/memory", test_bit_ops_memory);
    g_test_add_func("/dsp56300/bit/reg-io", test_bit_ops_reg_and_io);
    g_test_add_func("/dsp56300/bit/jumps", test_bit_jumps);
    g_test_add_func("/dsp56300/bit/jsclr", test_jsclr);
    g_test_add_func("/dsp56300/ctl/movec-movep-movem", test_movec_movep_movem);
    g_test_add_func("/dsp56300/ctl/stack-registers", test_stack_registers);
    g_test_add_func("/dsp56300/ctl/lua-lra-disp", test_lua_lra_disp);
    g_test_add_func("/dsp56300/ctl/andi-ori", test_andi_ori);
    g_test_add_func("/dsp56300/agu/modulo", test_modulo);
    g_test_add_func("/dsp56300/agu/modulo-offset", test_modulo_with_offset);
    g_test_add_func("/dsp56300/agu/modulo-decrement", test_modulo_decrement);
    g_test_add_func("/dsp56300/agu/modulo-block-jump", test_modulo_block_jump);
    g_test_add_func("/dsp56300/agu/bit-reverse", test_bit_reverse);
    g_test_add_func("/dsp56300/agu/multiple-wrap", test_multiple_wrap);
    g_test_add_func("/dsp56300/agu/linear", test_linear);
    g_test_add_func("/dsp56300/irq/fast", test_fast_interrupt);
    g_test_add_func("/dsp56300/irq/long-rti", test_long_interrupt_and_rti);
    g_test_add_func("/dsp56300/irq/priority", test_interrupt_priority);
    g_test_add_func("/dsp56300/irq/mask", test_interrupt_mask);
    g_test_add_func("/dsp56300/irq/level", test_level_interrupt);
    g_test_add_func("/dsp56300/irq/trap", test_trap_is_not_maskable);
    g_test_add_func("/dsp56300/irq/illegal", test_illegal_instruction);
    g_test_add_func("/dsp56300/irq/vector-base", test_vector_base);
    g_test_add_func("/dsp56300/irq/wait", test_wait_wakes);
    g_test_add_func("/dsp56300/irq/rep", test_rep_is_uninterruptible);
    g_test_add_func("/dsp56300/stack-error", test_stack_error);
    g_test_add_func("/dsp56300/idle/poll", test_idle_poll_loop);
    g_test_add_func("/dsp56300/idle/two-instruction",
                    test_idle_two_instruction_loop);
    g_test_add_func("/dsp56300/idle/busy", test_idle_not_when_state_changes);
    g_test_add_func("/dsp56300/cycles", test_cycle_counts);
    g_test_add_func("/dsp56300/bit/call", test_call_on_bit);
    g_test_add_func("/dsp56300/flow/jcc-ea", test_jcc_ea);
    g_test_add_func("/dsp56300/irq/trapcc", test_trapcc);
    g_test_add_func("/dsp56300/cache", test_cache_instructions);
    g_test_add_func("/dsp56300/stop-reset", test_stop_and_reset_instruction);
    g_test_add_func("/dsp56300/shift/register-cmpu",
                    test_shift_by_register_and_cmpu);
    g_test_add_func("/dsp56300/move/vsl", test_vsl);
    g_test_add_func("/dsp56300/move/displacement-store",
                    test_displacement_store);
    g_test_add_func("/dsp56300/loop/interrupted",
                    test_loop_survives_interrupts);
    g_test_add_func("/dsp56300/stack/flags", test_stack_counter_flags);
    g_test_add_func("/dsp56300/stop-wake", test_stop_wake);
    g_test_add_func("/dsp56300/rep-forbidden", test_rep_forbidden);
    g_test_add_func("/dsp56300/reserved-bits", test_reserved_bits_masked);
    g_test_add_func("/dsp56300/migration/roundtrip",
                    test_migration_mid_rep_and_do);
    g_test_add_func("/dsp56300/migration/reject", test_migration_rejects);
    g_test_add_func("/dsp56300/perf/mac-loop", test_throughput);
    return g_test_run();
}
