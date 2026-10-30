/* SPDX-License-Identifier: BSD-3-Clause */
/* Original scalar words and RAM products, independent of vendor programs. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/misc/sgi-vice-msp.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include <glib/gstdio.h>

#define BASE UINT64_C(0x17000000)
#define VICE_PATH "/machine/peripheral/vice"
#define I(op, rs, rt, imm) \
    ((uint32_t)(op) << 26 | (rs) << 21 | (rt) << 16 | (uint16_t)(imm))
#define R(rs, rt, rd, sa, fn) \
    ((rs) << 21 | (rt) << 16 | (rd) << 11 | (sa) << 6 | (fn))
#define BREAK R(0, 0, 0, 0, 13)
#define C(op, form, rt, rd) \
    ((uint32_t)(op) << 26 | (form) << 21 | (rt) << 16 | (rd) << 11)

static QTestState *start_clocks(unsigned execution, unsigned counter,
                                const char *extra)
{
    QTestState *s = qtest_initf("-M none -m 32M -cpu R10000 "
        "-device sgi-vice,id=vice,part-id=225,device-clock-hz=%u,"
        "msp-clock-hz=%u,stable-reference=on " MEMTX_TEST_DEVICE " %s",
        execution, counter, extra);

    qtest_sysbus_mmio_map(s, VICE_PATH, 0, BASE);
    return s;
}

static QTestState *start(const char *extra)
{
    return start_clocks(60000000, 66000000, extra);
}

static void reg(QTestState *s, unsigned offset, uint64_t value)
{
    g_assert_cmpint(memtx_store(s, BASE + offset, value, 8), ==, MEMTX_OK);
}

static uint64_t rd(QTestState *s, unsigned offset)
{
    return qtest_readq(s, BASE + offset);
}

static void program(QTestState *s, unsigned pc, const uint32_t *words,
                    unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        g_assert_cmpint(memtx_store(s, BASE + pc + i * 4, words[i], 4),
                       ==, MEMTX_OK);
    }
}

static int64_t launch(QTestState *s, unsigned pc)
{
    reg(s, 0x100, 7);
    int64_t now = qtest_clock_step(s, 267);

    reg(s, 0x50, pc);
    reg(s, 0x40, 3);
    return now;
}

static void test_arithmetic(void)
{
    const uint32_t words[] = {
        I(15, 0, 1, 0), I(13, 1, 1, 0x8000), I(9, 0, 2, 0x1234),
        I(9, 0, 3, -7), R(2, 3, 4, 0, 33), I(14, 4, 5, 0x55aa),
        I(43, 1, 5, 0), BREAK, 0, 0
    };
    QTestState *s = start("");

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    launch(s, 0x2000);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(rd(s, 0x8000), ==, 0);
    }
    qtest_clock_step(s, 150);
    g_assert_cmphex(rd(s, 0x48), ==, 4);
    g_assert_cmphex(rd(s, 0x40), ==, 3);
    g_assert_cmphex(rd(s, 0x8000), ==, 0);
    qtest_clock_step(s, 17);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x4787);
    g_assert_cmphex(rd(s, 0x40), ==, 3);
    qtest_clock_step(s, 17);
    g_assert_cmphex(rd(s, 0x40), ==, 2);
    g_assert_cmphex(rd(s, 0x68), ==, 0x201c);
    g_assert_cmphex(rd(s, 0x70), ==, 8);
    g_assert_cmphex(rd(s, 0x160), ==, 8);
    qtest_quit(s);
}


static void finish(QTestState *s)
{
    qtest_clock_step(s, 20000);
    g_assert_cmphex(rd(s, 0x40), ==, 2);
}

static void tick(QTestState *s, int64_t epoch, unsigned cycle)
{
    qtest_clock_set(s, epoch + (cycle * 50 + 2) / 3);
}

static void test_groups(void)
{
    static const struct {
        SGIViceMspIssue a, b;
        bool delay;
        unsigned count;
    } cases[] = {
        { VICE_MSP_SCALAR, VICE_MSP_SCALAR, false, 1 },
        { VICE_MSP_SCALAR, VICE_MSP_VECTOR_COMPUTE, false, 2 },
        { VICE_MSP_VECTOR_COMPUTE, VICE_MSP_SCALAR, false, 2 },
        { VICE_MSP_VECTOR_COMPUTE, VICE_MSP_BRANCH, false, 2 },
        { VICE_MSP_BRANCH, VICE_MSP_VECTOR_COMPUTE, false, 1 },
        { VICE_MSP_VECTOR_COMPUTE, VICE_MSP_VECTOR_COMPUTE, false, 1 },
        { VICE_MSP_VECTOR_SCALAR, VICE_MSP_SCALAR, false, 1 },
        { VICE_MSP_SCALAR, VICE_MSP_VECTOR_COMPUTE, true, 1 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmpuint(sgi_vice_msp_group(cases[i].a, cases[i].b,
                                          cases[i].delay), ==, cases[i].count);
    }
}

static void test_alu_forms(void)
{
    static const struct {
        uint32_t instruction, a, b, want;
    } cases[] = {
        { R(0, 3, 4, 31, 0), 0, 1, 0x80000000 },
        { R(0, 3, 4, 0, 0), 0, 0x89abcdef, 0x89abcdef },
        { R(0, 3, 4, 31, 2), 0, 0x80000000, 1 },
        { R(0, 3, 4, 31, 3), 0, 0x80000000, 0xffffffff },
        { R(2, 3, 4, 0, 4), 63, 3, 0x80000000 },
        { R(2, 3, 4, 0, 6), 33, 0x80000004, 0x40000002 },
        { R(2, 3, 4, 0, 7), 33, 0x80000004, 0xc0000002 },
        { R(2, 3, 4, 0, 32), 0x7ffffff0, 15, 0x7fffffff },
        { R(2, 3, 4, 0, 33), 0xffffffff, 2, 1 },
        { R(2, 3, 4, 0, 34), 0x80000001, 1, 0x80000000 },
        { R(2, 3, 4, 0, 35), 0, 1, 0xffffffff },
        { R(2, 3, 4, 0, 36), 0x963c5aa5, 0xf0f00f0f, 0x90300a05 },
        { R(2, 3, 4, 0, 37), 0x963c5aa5, 0xf0f00f0f, 0xf6fc5faf },
        { R(2, 3, 4, 0, 38), 0x963c5aa5, 0xf0f00f0f, 0x66cc55aa },
        { R(2, 3, 4, 0, 39), 0x963c5aa5, 0xf0f00f0f, 0x0903a050 },
        { R(2, 3, 4, 0, 42), 0xffffffff, 1, 1 },
        { R(2, 3, 4, 0, 43), 0xffffffff, 1, 0 },
        { I(8, 2, 4, -1), 0x80000001, 0, 0x80000000 },
        { I(9, 2, 4, 1), 0xffffffff, 0, 0 },
        { I(10, 2, 4, -1), 0x80000000, 0, 1 },
        { I(11, 2, 4, -1), 0x7fffffff, 0, 1 },
        { I(12, 2, 4, 0x8042), 0x963c5aa5, 0, 0x00000000 },
        { I(13, 2, 4, 0x8042), 0x963c5aa5, 0, 0x963cdae7 },
        { I(14, 2, 4, 0x8042), 0x963c5aa5, 0, 0x963cdae7 },
        { I(15, 0, 4, 0xa17c), 0, 0, 0xa17c0000 },
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        uint32_t words[] = {
            I(13, 0, 1, 0x8000), I(15, 0, 2, cases[i].a >> 16),
            I(13, 2, 2, cases[i].a), I(15, 0, 3, cases[i].b >> 16),
            I(13, 3, 3, cases[i].b), cases[i].instruction,
            I(43, 1, 4, 0), BREAK, 0, 0
        };

        reg(s, 0x40, 0);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        reg(s, 0x8000, UINT64_MAX);
        launch(s, 0x2000);
        finish(s);
        g_test_message("ALU case %u", i);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, cases[i].want);
    }
    qtest_quit(s);
}

static void test_gprs(void)
{
    QTestState *s = start("");

    for (unsigned r = 0; r < 32; r++) {
        unsigned base = r == 31 ? 30 : 31;
        uint32_t words[] = {
            I(13, 0, base, 0x8000), I(13, 0, r, 0x130 + r),
            R(r, r, r, 0, 33), I(43, base, r, 0), BREAK, 0, 0
        };

        reg(s, 0x40, 0);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        finish(s);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==,
                        r ? (0x130 + r) * 2 : 0);
    }
    qtest_quit(s);
}

static void test_branches(void)
{
    static const struct {
        unsigned op, rt;
        int a, b;
        bool taken, link;
    } predicates[] = {
        {4, 3, 7, 7, true, false}, {4, 3, 7, 8, false, false},
        {5, 3, 7, 8, true, false}, {5, 3, 7, 7, false, false},
        {6, 0, -1, 0, true, false}, {6, 0, 1, 0, false, false},
        {7, 0, 1, 0, true, false}, {7, 0, 0, 0, false, false},
        {1, 0, -1, 0, true, false}, {1, 0, 0, 0, false, false},
        {1, 1, 0, 0, true, false}, {1, 1, -1, 0, false, false},
        {1, 16, -1, 0, true, true}, {1, 16, 0, 0, false, true},
        {1, 17, 0, 0, true, true}, {1, 17, -1, 0, false, true},
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < G_N_ELEMENTS(predicates); i++) {
        uint32_t words[] = {
            I(13, 0, 1, 0x8000), I(9, 0, 2, predicates[i].a),
            I(9, 0, 3, predicates[i].b), I(13, 0, 4, 0x5a3c), 0,
            I(predicates[i].op, 2, predicates[i].rt, 5), I(43, 1, 4, 0),
            I(13, 0, 5, 0x1122), I(43, 1, 5, 16),
            I(2, 0, 0, 0x2038 >> 2), 0,
            I(13, 0, 5, 0x3344), 0, 0,
            I(43, 1, 5, 4), I(43, 1, 31, 8), BREAK, 0, 0
        };
        int64_t epoch;

        reg(s, 0x40, 0);
        reg(s, 0x8010, 0xccccccccccccccccULL);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        epoch = launch(s, 0x2000);
        tick(s, epoch, 10);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x5a3c);
        qtest_writel(s, BASE + 0x8000, 0xdeadbeef);
        finish(s);
        g_test_message("branch case %u", i);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0xdeadbeef);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==,
                        predicates[i].taken ? 0x3344 : 0x1122);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==,
                        predicates[i].link ? 0x201c : 0);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8010), ==,
                        predicates[i].taken ? 0xcccccccc : 0x1122);
    }
    qtest_quit(s);
}

static void test_register_jumps(void)
{
    QTestState *s = start("");
    const uint32_t target[] = { I(43, 1, 4, 4), I(43, 1, 7, 8), BREAK, 0, 0 };
    const uint32_t wrong[] = { I(43, 1, 4, 16), BREAK, 0, 0 };

    for (unsigned link = 0; link < 2; link++) {
        for (unsigned gap = 0; gap < 3; gap++) {
            uint32_t words[16] = {
                I(13, 0, 1, 0x8000), I(13, 0, 4, 0x39a7),
                I(13, 0, 2, 0x20c0), I(15, 0, 2, 0x9abc),
                I(13, 2, 2, 0x2083)
            };
            unsigned at = 5 + gap;

            words[at] = R(2, 0, link ? 7 : 0, 0, link ? 9 : 8);
            words[at + 1] = I(13, 0, 2, 0x20c0);
            words[at + 2] = I(43, 1, 4, 16);
            words[at + 3] = BREAK;
            reg(s, 0x40, 0);
            reg(s, 0x8010, 0);
            program(s, 0x2000, words, G_N_ELEMENTS(words));
            program(s, 0x2080, target, G_N_ELEMENTS(target));
            program(s, 0x20c0, wrong, G_N_ELEMENTS(wrong));
            launch(s, 0x2000);
            finish(s);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==, 0x39a7);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==,
                            link ? 0x201c + gap * 4 : 0);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8010), ==, 0);
        }
    }
    qtest_quit(s);
}

static void test_load_interlocks(void)
{
    QTestState *s = start("");

    for (unsigned gap = 0; gap < 3; gap++) {
        uint32_t words[14] = {
            I(13, 0, 1, 0x8000), I(35, 1, 2, 32)
        };
        unsigned at = 2 + gap;
        int64_t epoch;

        words[at] = R(2, 2, 3, 0, 33);
        words[at + 1] = I(43, 1, 3, 0);
        words[at + 2] = BREAK;
        reg(s, 0x40, 0);
        qtest_writel(s, BASE + 0x8000, 0);
        qtest_writel(s, BASE + 0x8020, 0x12345678);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        epoch = launch(s, 0x2000);
        tick(s, epoch, 8);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0);
        tick(s, epoch, 9);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x2468acf0);
        finish(s);
    }
    qtest_quit(s);
}

static void test_immediate_false_dependency(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(35, 1, 2, 32), I(9, 0, 2, 0x4321),
        I(43, 1, 2, 0), BREAK, 0, 0
    };
    int64_t epoch;

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    qtest_writel(s, BASE + 0x8020, 0xaabbccdd);
    epoch = launch(s, 0x2000);
    tick(s, epoch, 5);
    g_assert_cmphex(rd(s, 0x50), ==, 0x200c);
    tick(s, epoch, 8);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x4321);
    finish(s);
    qtest_quit(s);
}

static void test_simultaneous_faults(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x1000), I(35, 2, 3, 0),
        0, 0, BREAK, I(43, 1, 3, 0), 0, 0
    };

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    launch(s, 0x2000);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 5);
    g_assert_cmphex(rd(s, 0x68), ==, 0x2014);
    g_assert_cmphex(rd(s, 0x70), ==, 8);
    g_assert_cmphex(rd(s, 0x58), ==, 0);
    qtest_quit(s);
}

static void test_imprecise_drain(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 4, 0x3a7c), I(13, 0, 2, 0x1000),
        I(35, 2, 3, 0), I(43, 1, 4, 0),
        I(3, 0, 0, 0x2080 >> 2), 0, 0
    };
    const uint32_t target[] = { I(43, 1, 4, 16), BREAK, 0, 0 };
    const uint32_t restart[] = { I(43, 1, 31, 4), BREAK, 0, 0 };

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    program(s, 0x2080, target, G_N_ELEMENTS(target));
    launch(s, 0x2000);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 1);
    g_assert_cmphex(rd(s, 0x68), ==, 0x200c);
    g_assert_cmphex(rd(s, 0x58), ==, 0x1000);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x3a7c);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8010), ==, 0);
    program(s, 0x2100, restart, G_N_ELEMENTS(restart));
    reg(s, 0x50, 0x2100);
    reg(s, 0x40, 3);
    finish(s);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==, 0x201c);
    g_assert_cmphex(rd(s, 0x48), ==, 5);
    g_assert_cmphex(rd(s, 0x68), ==, 0x200c);
    qtest_quit(s);
}


static void test_local_memory(void)
{
    QTestState *s = start("");

    for (unsigned bank = 0; bank < 3; bank++) {
        unsigned address = 0x8000 + bank * 0x800;
        uint32_t words[] = {
            I(13, 0, 1, address), I(13, 0, 31, 0x8400),
            I(32, 1, 2, 0), I(43, 31, 2, 0),
            I(36, 1, 3, 1), I(43, 31, 3, 4),
            I(33, 1, 4, 0), I(43, 31, 4, 8),
            I(37, 1, 5, 0), I(43, 31, 5, 12),
            I(35, 1, 6, 0), I(43, 31, 6, 16),
            I(13, 0, 7, 0xa7), I(40, 1, 7, 0x101),
            I(13, 0, 8, 0xb6c8), I(41, 1, 8, 0x104),
            I(15, 0, 9, 0x7f01), I(13, 9, 9, 0x62d3),
            I(43, 1, 9, 0x108), BREAK, 0, 0
        };

        reg(s, 0x40, 0);
        qtest_writel(s, BASE + address, 0x80e1c27f);
        reg(s, address + 0x100, 0x11112222deadbeefULL);
        reg(s, address + 0x108, 0x3333444455556666ULL);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        finish(s);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8400), ==, 0xffffff80);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8404), ==, 0xe1);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8408), ==, 0xffff80e1);
        g_assert_cmphex(qtest_readl(s, BASE + 0x840c), ==, 0x80e1);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8410), ==, 0x80e1c27f);
        g_assert_cmphex(rd(s, address + 0x100), ==, 0x11a72222b6c8beefULL);
        g_assert_cmphex(rd(s, address + 0x108), ==, 0x7f0162d355556666ULL);
    }
    qtest_quit(s);
}

static void test_address_faults(void)
{
    static const uint32_t addresses[] = {
        0, 0x1000, 0x2000, 0x4000, 0x5000, 0x7000, 0x9800,
        0xf000, 0x10008000, 0x8001, 0xffffffff
    };
    QTestState *s = start("");

    for (unsigned write = 0; write < 2; write++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
            uint32_t words[] = {
                I(15, 0, 1, addresses[i] >> 16),
                I(13, 1, 1, addresses[i]), I(13, 0, 2, 0x3678),
                I(write ? 43 : 35, 1, 2, 0), 0, 0, 0, 0, BREAK, 0, 0
            };
            const uint32_t probe[] = {
                I(13, 0, 1, 0x8400), I(43, 1, 2, 0), BREAK, 0, 0
            };

            reg(s, 0x40, 0);
            reg(s, 0x8000, 0x123456789abcdef0ULL);
            program(s, 0x2000, words, G_N_ELEMENTS(words));
            launch(s, 0x2000);
            finish(s);
            g_assert_cmphex(rd(s, 0x48), ==, write ? 2 : 1);
            g_assert_cmphex(rd(s, 0x58), ==, addresses[i]);
            g_assert_cmphex(rd(s, 0x68), ==, 0x200c);
            g_assert_cmphex(rd(s, 0x70), ==, write ? 4 : 0);
            g_assert_cmphex(rd(s, 0x8000), ==, 0x123456789abcdef0ULL);
            if (!write) {
                program(s, 0x2100, probe, G_N_ELEMENTS(probe));
                reg(s, 0x50, 0x2100);
                reg(s, 0x40, 3);
                finish(s);
                g_assert_cmphex(qtest_readl(s, BASE + 0x8400), ==, 0);
            }
        }
    }
    qtest_quit(s);
}

static void test_contention_watch(void)
{
    for (unsigned denied = 0; denied < 2; denied++) {
        for (unsigned write = 0; write < 2; write++) {
            QTestState *s = start("");
            uint32_t words[] = {
                I(13, 0, 1, 0x8000), I(13, 0, 2, 0x3a17),
                I(write ? 43 : 35, 1, 2, 0), 0, 0, 0, 0, BREAK, 0, 0
            };
            const uint32_t probe[] = {
                I(13, 0, 1, 0x8400), I(43, 1, 2, 0), BREAK, 0, 0
            };
            program(s, 0x2000, words, G_N_ELEMENTS(words));
            qtest_writel(s, BASE + 0x8000, 0x8abc5127);
            reg(s, 0x60, 0x8007);
            g_assert_cmphex(rd(s, 0x60), ==, 0x8000);
            launch(s, 0x2000);
            if (denied) {
                reg(s, 0x100, 6);
            }
            finish(s);
            g_assert_cmphex(rd(s, 0x48), ==, denied ? 0x48 : 8);
            g_assert_cmphex(rd(s, 0x70), ==, denied ? 24 : 12);
            g_assert_cmphex(rd(s, 0x58), ==, 0);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==,
                            write && !denied ? 0x3a17 : 0x8abc5127);
            if (!write) {
                program(s, 0x2100, probe, G_N_ELEMENTS(probe));
                reg(s, 0x100, 7);
                reg(s, 0x50, 0x2100);
                reg(s, 0x40, 3);
                finish(s);
                g_assert_cmphex(qtest_readl(s, BASE + 0x8400), ==,
                                denied ? 0 : 0x8abc5127);
            }
            qtest_quit(s);
        }
    }
}

static void test_fetch_pc(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 2, 0x1122), I(9, 2, 3, 7), I(13, 0, 1, 0x8000),
        I(43, 1, 3, 0), BREAK, 0, 0
    };

    for (unsigned odd = 0; odd < 2; odd++) {
        reg(s, 0x40, 0);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        qtest_clock_step(s, 267);
        reg(s, 0x100, 7);
        reg(s, 0x50, 0xabcd2003 + odd * 4);
        g_assert_cmphex(rd(s, 0x50), ==, 0);
        reg(s, 0x40, 3);
        qtest_clock_step(s, 17);
        g_assert_cmphex(rd(s, 0x50), ==, 0);
        qtest_clock_step(s, 17);
        g_assert_cmphex(rd(s, 0x50), ==, 0x2000 + odd * 4);
        finish(s);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, odd ? 7 : 0x1129);
    }
    for (unsigned bad = 0; bad < 2; bad++) {
        reg(s, 0x40, 0);
        launch(s, bad ? 0x3000 : 0);
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, 0x80);
        g_assert_cmphex(rd(s, 0x68), ==, bad ? 0x3000 : 0);
        g_assert_cmphex(rd(s, 0x70), ==, 28);
        g_assert_cmphex(rd(s, 0x58), ==, 0);
    }
    qtest_quit(s);
}

static void test_jump_fetch_fault(void)
{
    const uint32_t jumps[] = {
        I(2, 0, 0, 0), I(3, 0, 0, 0), R(2, 0, 0, 0, 8),
        R(2, 0, 7, 0, 9)
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(jumps); i++) {
        QTestState *s = start("");
        uint32_t words[] = {
            I(13, 0, 1, 0x8000), I(13, 0, 4, 0x7a39),
            I(13, 0, 2, 0), 0, jumps[i], I(43, 1, 4, 0), 0, 0
        };
        uint32_t probe[] = {
            I(43, 1, i == 3 ? 7 : 31, 8), BREAK, 0, 0
        };

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, 0x80);
        g_assert_cmphex(rd(s, 0x68), ==, 0);
        g_assert_cmphex(rd(s, 0x70), ==, 28);
        g_assert_cmphex(rd(s, 0x58), ==, 0);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x7a39);
        program(s, 0x2100, probe, G_N_ELEMENTS(probe));
        reg(s, 0x50, 0x2100);
        reg(s, 0x40, 3);
        finish(s);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==,
                        i == 1 || i == 3 ? 0x2018 : 0);
        qtest_quit(s);
    }
}

static void test_delay_fault(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x1000), 0,
        I(2, 0, 0, 0x2080 >> 2), I(35, 2, 3, 0), 0, 0
    };
    const uint32_t target[] = { 0, 0, 0, 0, 0, 0, BREAK, 0, 0 };

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    program(s, 0x2080, target, G_N_ELEMENTS(target));
    launch(s, 0x2000);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 1);
    g_assert_cmphex(rd(s, 0x68), ==, 0x2010);
    g_assert_cmphex(rd(s, 0x70), ==, 0x80000000);
    g_assert_cmphex(rd(s, 0x58), ==, 0x1000);
    qtest_quit(s);
}

static void assert_held(QTestState *s)
{
    g_autoptr(QDict) response = qtest_qmp_assert_success_ref(s,
        "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(response, "status"), ==, "internal-error");
}

static void test_held_words(void)
{
    static const struct { uint32_t a, b; } cases[] = {
        { R(1, 2, 3, 1, 0), 0 },
        { I(1, 1, 2, 0), 0 },
        { C(0x11, 0, 2, 0), 0 },
        { C(0x13, 0, 2, 0) | 1, 0 },
        { I(0x32, 1, 2, 0), 0 },
        { I(0x3a, 1, 2, 0), 0 },
        { I(43, 0, 0, 0), 0x4a000000 },
        { 0x4a000000, I(43, 0, 0, 0) },
        { I(5, 0, 0, -0x801), 0 },
        { I(2, 0, 0, 0x2080 >> 2), BREAK },
        { R(2, 0, 2, 0, 9), 0 },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *s = start("");
        uint32_t words[16] = { cases[i].a, cases[i].b };

        words[12] = BREAK;
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        qtest_clock_step(s, 20000);
        assert_held(s);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        g_assert_cmphex(rd(s, 0x8000), ==, 0);
        qtest_quit(s);
    }
}

static void test_held_data_cases(void)
{
    static const uint32_t cases[][5] = {
        { I(15, 0, 1, 0x7fff), I(13, 1, 1, 0xffff), I(9, 0, 2, 1),
          R(1, 2, 3, 0, 32), 0 },
        { I(13, 0, 1, 0x8000), I(33, 1, 2, 2), 0, 0, 0 },
        { I(13, 0, 1, 0x8000), I(13, 0, 2, 0x1234),
          I(4, 0, 0, 4), I(43, 1, 2, 0), 0 },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        QTestState *s = start("");
        uint32_t words[16] = { 0 };

        memcpy(words, cases[i], sizeof(cases[i]));
        words[12] = BREAK;
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        qtest_clock_step(s, 20000);
        assert_held(s);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        g_assert_cmphex(rd(s, 0x8000), ==, 0);
        qtest_quit(s);
    }
    QTestState *s = start("");
    g_assert_cmpint(memtx_store(s, BASE + 0x2ffc, BREAK, 4), ==, MEMTX_OK);
    launch(s, 0x2ffc);
    qtest_clock_step(s, 1000);
    assert_held(s);
    g_assert_cmphex(rd(s, 0x48), ==, 0);
    qtest_quit(s);
}

static void test_common_dma(void)
{
    QTestState *s = start("");
    uint32_t words[256];
    unsigned n = 0;
    uint8_t bytes[2][16], actual[16];

    words[n++] = I(13, 0, 1, 0x8400);
    words[n++] = I(13, 0, 2, 7);
    words[n++] = C(0x11, 6, 2, 0);
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned d = 0; d < 4; d++) {
            uint16_t fields[] = {
                d == 3 ? 0xa000 : 0x4000, 0x80,
                d == 1 ? 0x3333 + c : 0x2000 + c * 0x100,
                16, 0, 1, 0x8000 + c * 0x800, 0x1234
            };
            for (unsigned f = 0; f < 8; f++) {
                words[n++] = I(13, 0, 2, fields[f]);
                words[n++] = C(0x13, c ? 6 : 4, 2, d * 8 + f);
            }
        }
        words[n++] = C(0x13, c ? 2 : 0, 3, 10);
        words[n++] = I(43, 1, 3, c * 4);
    }
    for (unsigned c = 0; c < 2; c++) {
        words[n++] = I(13, 0, 2, 0x83);
        words[n++] = C(0x11, 6, 2, c ? 24 : 16);
        for (unsigned i = 0; i < 16; i++) {
            bytes[c][i] = 0x13 + c * 71 + i * 19;
        }
        qtest_memwrite(s, 0x12000 + c * 0x100, bytes[c], 16);
    }
    words[n++] = C(0x11, 6, 0, 23);
    words[n++] = C(0x11, 2, 5, 1);
    words[n++] = I(43, 1, 5, 8);
    words[n++] = BREAK;
    words[n++] = 0;
    words[n++] = 0;
    reg(s, 0xf000, 0x10003);
    program(s, 0x2000, words, n);
    launch(s, 0x2000);
    finish(s);
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned i = 0; i < 16; i++) {
            actual[i] = qtest_readb(s, BASE + 0x8000 + c * 0x800 + i);
        }
        g_assert_cmpmem(actual, 16, bytes[c], 16);
        g_assert_cmphex(rd(s, 0x188 + c * 0x40), ==, 0x289);
        g_assert_cmphex(rd(s, 0x1a8 + c * 0x40), ==, 0);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8400 + c * 4), ==, 0x3333 + c);
        g_assert_cmphex(rd(s, 0x1050 + c * 0x100), ==, 0x3333 + c);
        g_assert_cmphex(rd(s, 0x1058 + c * 0x100), ==, 16);
    }
    g_assert_cmpuint(qtest_readl(s, BASE + 0x8408), >, 17);
    g_assert_cmphex(rd(s, 0x160), ==, 0x8d);
    qtest_quit(s);
}


static void test_load_jump_targets(void)
{
    QTestState *s = start("");
    const uint32_t target[] = {
        I(43, 1, 4, 4), I(43, 1, 7, 8), BREAK, 0, 0
    };
    const uint32_t wrong[] = { I(43, 1, 4, 16), BREAK, 0, 0 };

    for (unsigned common = 0; common < 2; common++) {
        for (unsigned gap = 0; gap < 3; gap++) {
            uint32_t words[16] = {
                I(13, 0, 1, 0x8000), I(13, 0, 4, 0x7b39),
                I(13, 0, 2, 0x20c0),
                common ? C(0x11, 2, 2, 0) : I(35, 1, 2, 32)
            };
            unsigned at = 4 + gap;
            int64_t epoch;

            words[at] = R(2, 0, 7, 0, 9);
            words[at + 1] = I(43, 1, 4, 0);
            words[at + 2] = I(43, 1, 4, 16);
            words[at + 3] = BREAK;
            reg(s, 0x40, 0);
            reg(s, 0x8000, 0);
            reg(s, 0x8008, 0);
            reg(s, 0x8010, 0);
            qtest_writel(s, BASE + 0x8020, 0x9abc2087);
            program(s, 0x2000, words, G_N_ELEMENTS(words));
            program(s, 0x2084, target, G_N_ELEMENTS(target));
            program(s, 0x20c0, wrong, G_N_ELEMENTS(wrong));
            epoch = launch(s, 0x2000);
            if (common) {
                reg(s, 0x100, 0x9abc2087);
            }
            tick(s, epoch, 8);
            g_assert_cmphex(rd(s, 0x50), ==, 0x2010 + gap * 4);
            g_assert_cmphex(rd(s, 0x8000), ==, 0);
            tick(s, epoch, 11);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x7b39);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==, 0);
            tick(s, epoch, 12);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==, 0x7b39);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==, 0);
            tick(s, epoch, 13);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==,
                            0x2018 + gap * 4);
            finish(s);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8010), ==, 0);
        }
    }
    qtest_quit(s);
}

static void test_raw_interlocks(void)
{
    for (unsigned common = 0; common < 2; common++) {
        QTestState *s = start("");
        uint32_t words[] = {
            I(13, 0, 1, 0x8000), I(35, 1, common ? 6 : 0, 32),
            common ? C(0x11, 6, 0, 23) : I(13, 0, 4, 0x7319),
            common ? 0 : I(43, 1, 4, 0), BREAK, 0, 0
        };
        int64_t epoch;

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        qtest_writel(s, BASE + 0x8020, 0xabcdef12);
        epoch = launch(s, 0x2000);
        if (common) {
            tick(s, epoch, 7);
            g_assert_cmphex(rd(s, 0x160), ==, 0);
            tick(s, epoch, 8);
            g_assert_cmphex(rd(s, 0x160), ==, 4);
            reg(s, 0xe008, 4);
            finish(s);
            g_assert_cmphex(rd(s, 0x160), ==, 8);
        } else {
            tick(s, epoch, 8);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0);
            tick(s, epoch, 9);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x7319);
            finish(s);
        }
        qtest_quit(s);
    }
}

static void test_host_reset_halt(void)
{
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x1111), I(43, 1, 2, 0),
        I(13, 0, 3, 0x2222), I(43, 1, 3, 4), 0,
        I(43, 1, 3, 8), 0, BREAK, 0, 0
    };

    for (unsigned reset = 0; reset < 2; reset++) {
        QTestState *s = start("");
        int64_t epoch;

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        reg(s, 0x8000, 0xccccccccccccccccULL);
        reg(s, 0x8008, 0xccccccccccccccccULL);
        epoch = launch(s, 0x2000);
        tick(s, epoch, 6);
        qtest_clock_step(s, 5);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x1111);
        reg(s, 0x40, reset ? 0 : 2);
        qtest_clock_step(s, 1000);
        g_assert_cmphex(rd(s, 0x40), ==, reset ? 0 : 2);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8004), ==,
                        reset ? 0xcccccccc : 0x2222);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8008), ==, 0xcccccccc);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        g_assert_cmphex(rd(s, 0x160), ==, 0);
        g_assert_cmpint(memtx_store(s, BASE + 0x40, 3, 8), !=, MEMTX_OK);
        assert_held(s);
        qtest_quit(s);
    }
}

static void test_msp_reset_duration(void)
{
    const unsigned hz[] = { 60000000, 50000000, 125000000 };
    const unsigned first[] = { 250, 300, 120 };
    const unsigned minimum[] = { 267, 320, 128 };
    const uint32_t words[] = { BREAK, 0, 0 };

    for (unsigned i = 0; i < G_N_ELEMENTS(hz); i++) {
        QTestState *s = start_clocks(hz[i], 66000000, "");

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        reg(s, 0x50, 0x2000);
        qtest_clock_step(s, first[i]);
        g_assert_cmpint(memtx_store(s, BASE + 0x40, 3, 8), !=, MEMTX_OK);
        g_assert_cmphex(rd(s, 0x40), ==, 0);
        reg(s, 0x40, 0);
        qtest_clock_step(s, minimum[i] - first[i] - 1);
        g_assert_cmpint(memtx_store(s, BASE + 0x40, 3, 8), !=, MEMTX_OK);
        qtest_clock_step(s, 1);
        reg(s, 0x40, 3);
        qtest_clock_timers(s, true);
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, 4);
        g_assert_cmphex(rd(s, 0x160), ==, 8);
        reg(s, 0x48, 0);
        g_assert_cmphex(rd(s, 0x160), ==, 8);
        reg(s, 0xe008, 8);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        g_assert_cmphex(rd(s, 0x160), ==, 0);
        qtest_quit(s);
    }
}

static void test_ri_and_break_payload(void)
{
    const uint32_t words[] = { 0xfc000000, 0x0000003f, 0x0000000c,
                               (0xabcde << 6) | 13 };
    for (unsigned i = 0; i < G_N_ELEMENTS(words); i++) {
        QTestState *s = start("");
        uint32_t body[] = { words[i], 0, 0 };

        program(s, 0x2000, body, G_N_ELEMENTS(body));
        launch(s, 0x2000);
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, i == 3 ? 4 : 16);
        g_assert_cmphex(rd(s, 0x70), ==, i == 3 ? 8 : 16);
        g_assert_cmphex(rd(s, 0x68), ==, 0x2000);
        qtest_quit(s);
    }
}

static void test_branch_wrap(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 4, 0x3297), 0, 0,
        I(4, 0, 0, 0x401b), I(43, 1, 4, 0), 0, 0
    };
    const uint32_t target[] = { I(43, 1, 4, 4), BREAK, 0, 0 };

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    program(s, 0x2080, target, G_N_ELEMENTS(target));
    launch(s, 0x2000);
    finish(s);
    g_assert_cmphex(rd(s, 0x8000), ==, 0x0000329700003297ULL);
    qtest_quit(s);
}

static void test_active_ports(void)
{
    for (unsigned operation = 0; operation < 4; operation++) {
        QTestState *s = start("");
        const uint32_t words[] = {
            I(13, 0, 1, 0x8000), 0, 0, 0, 0, BREAK, 0, 0
        };
        uint64_t value;

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        launch(s, 0x2000);
        if (operation == 0) {
            g_assert_cmpint(memtx_load(s, BASE + 0x2000, &value, 4),
                           !=, MEMTX_OK);
        } else if (operation == 1) {
            g_assert_cmpint(memtx_store(s, BASE + 0x2000, 0, 4), !=, MEMTX_OK);
        } else if (operation == 2) {
            g_assert_cmpint(memtx_store(s, BASE + 0x50, 0x2080, 8),
                           !=, MEMTX_OK);
        } else {
            g_assert_cmpint(memtx_store(s, BASE + 0x40, 3, 8), !=, MEMTX_OK);
        }
        assert_held(s);
        reg(s, 0x40, 0);
        g_assert_cmphex(qtest_readl(s, BASE + 0x2000), ==, words[0]);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        qtest_quit(s);
    }
}


static void dma_descriptor(QTestState *s, unsigned channel, uint16_t flags,
                            uint32_t system, unsigned length, unsigned local)
{
    uint16_t fields[] = { flags, system >> 16, system, length, 0, 1, local, 0 };

    for (unsigned i = 0; i < 8; i++) {
        reg(s, 0x1000 + channel * 0x100 + i * 8, fields[i]);
    }
}

static void dma_go(QTestState *s, unsigned channel)
{
    reg(s, 0x180 + channel * 0x40, 0x113);
}

static void check_local(QTestState *s, unsigned address,
                         const uint8_t *expected, unsigned length)
{
    for (unsigned i = 0; i < length; i++) {
        g_assert_cmphex(qtest_readb(s, BASE + address + i), ==, expected[i]);
    }
}

static void test_branch_penalty(void)
{
    for (unsigned taken = 0; taken < 2; taken++) {
        QTestState *s = start("");
        uint32_t words[] = {
            I(13, 0, 1, 0x8000), I(13, 0, 2, 0x5913), 0,
            I(taken ? 4 : 5, 0, 0, 1), 0, I(43, 1, 2, 0), BREAK, 0, 0
        };
        int64_t epoch;

        program(s, 0x2000, words, G_N_ELEMENTS(words));
        epoch = launch(s, 0x2000);
        tick(s, epoch, 9);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, taken ? 0x5913 : 0);
        tick(s, epoch, 10);
        g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x5913);
        finish(s);
        qtest_quit(s);
    }
}

static void test_initial_dma_deferral(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x5aa5), I(43, 1, 2, 0),
        0, 0, 0, 0, 0, 0, BREAK, 0, 0
    };
    int64_t epoch, dma_epoch;

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    reg(s, 0xe000, 2);
    dma_descriptor(s, 0, 0x8000, 0x10000, 8, 0x8000);
    epoch = launch(s, 0x2000);
    tick(s, epoch, 5);
    dma_epoch = qtest_clock_step(s, 5);
    dma_go(s, 0);
    g_assert_cmphex(rd(s, 0x1a8), ==, 0);
    tick(s, epoch, 6);
    reg(s, 0x100, 0); /* An accepted interval survives grant revocation. */
    tick(s, dma_epoch, 1);
    g_assert_cmphex(rd(s, 0x1a8), ==, 0);
    g_assert_cmphex(rd(s, 0x1a0), ==, 0);
    reg(s, 0x8000, 0x91ab37cd2876f042ULL);
    tick(s, dma_epoch, 2);
    g_assert_cmphex(rd(s, 0x1a8), ==, 8);
    g_assert_cmphex(rd(s, 0x1a0), ==, 0x8000);
    tick(s, dma_epoch, 3);
    g_assert_cmphex(qtest_readq(s, 0x10000), ==, 0x91ab37cd2876f042ULL);
    g_assert_cmphex(rd(s, 0x188), ==, 0x211);
    finish(s);
    qtest_quit(s);
}

static void test_stop_without_local_phase(void)
{
    QTestState *s = start("");
    const uint32_t setup[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 4, 0x5aa5), BREAK, 0, 0
    };
    const uint32_t wave[] = {
        I(43, 1, 4, 0), I(43, 1, 4, 4), I(43, 1, 4, 8),
        0, 0, 0, 0, BREAK, 0, 0
    };
    int64_t epoch, dma_epoch;

    program(s, 0x2000, setup, G_N_ELEMENTS(setup));
    launch(s, 0x2000);
    finish(s);
    reg(s, 0x48, 0);
    reg(s, 0xe008, 8);
    program(s, 0x2100, wave, G_N_ELEMENTS(wave));
    reg(s, 0xe000, 2);
    reg(s, 0x8000, 0x91ab37cd2876f042ULL);
    dma_descriptor(s, 0, 0x8000, 0x10000, 16, 0x8000);
    epoch = qtest_clock_step(s, 1);
    reg(s, 0x50, 0x2100);
    reg(s, 0x40, 3);
    tick(s, epoch, 2);
    dma_epoch = qtest_clock_step(s, 5);
    dma_go(s, 0);
    tick(s, dma_epoch, 2);
    g_assert_cmphex(rd(s, 0x1a8), ==, 8);
    reg(s, 0x180, 0x117);
    tick(s, dma_epoch, 3);
    g_assert_cmphex(rd(s, 0x188), ==, 0x110);
    g_assert_cmphex(rd(s, 0x1a8), ==, 8);
    g_assert_cmphex(qtest_readq(s, 0x10000), ==, 0x91ab37cd2876f042ULL);
    g_assert_cmphex(qtest_readq(s, 0x10008), ==, 0);
    finish(s);
    g_assert_cmphex(rd(s, 0x160), ==, 8);
    qtest_quit(s);
}

static void test_msp_first_contention(void)
{
    for (unsigned protected = 0; protected < 2; protected++) {
        QTestState *s = start("");
        unsigned bank = 0x8000 + protected * 0x800;
        unsigned other = 0x8000 + (protected ^ 1) * 0x800;
        uint32_t words[22] = {
            I(13, 0, 1, bank), I(13, 0, 2, other),
            I(13, 0, 3, 0x5aa5), 0,
            I(43, 1, 3, 0), I(43, 2, 3, 0), I(43, 1, 3, 4),
            I(43, 2, 3, 4), I(43, 1, 3, 8)
        };
        uint8_t a[16], b[16], got[16];
        int64_t epoch, dma_epoch;

        words[18] = BREAK;
        for (unsigned i = 0; i < 16; i++) {
            a[i] = 7 + i * 23;
            b[i] = 0x91 + i * 13;
        }
        qtest_memwrite(s, 0x10000, a, sizeof(a));
        qtest_memwrite(s, 0x11000, b, sizeof(b));
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        reg(s, 0xe000, 2);
        dma_descriptor(s, 0, 0xa000, 0x10000, 16, bank);
        dma_descriptor(s, 1, 0xa000, 0x11000, 16, 0x9000);
        epoch = launch(s, 0x2000);
        tick(s, epoch, 7);
        dma_epoch = qtest_clock_step(s, 5);
        dma_go(s, 0);
        dma_go(s, 1);
        tick(s, dma_epoch, 2);
        qtest_memset(s, 0x10000, 0xff, 8);
        tick(s, dma_epoch, 3);
        g_assert_cmphex(rd(s, 0x1a8), ==, 16);
        g_assert_cmphex(rd(s, 0x1a0), ==, 0);
        g_assert_cmphex(rd(s, 0x1e8), ==, 16);
        tick(s, dma_epoch, 4);
        g_assert_cmphex(rd(s, 0x1e8), ==, 8);
        check_local(s, 0x9000, b, 8);
        tick(s, dma_epoch, 5);
        g_assert_cmphex(rd(s, 0x1a8), ==, 16);
        g_assert_cmphex(rd(s, 0x1a0), ==, 0);
        tick(s, dma_epoch, 7);
        g_assert_cmphex(rd(s, 0x1a8), ==, 8);
        check_local(s, bank, a, 8);
        finish(s);
        check_local(s, bank, a, 16);
        check_local(s, 0x9000, b, 16);
        qtest_memread(s, 0x10000, got, 8);
        for (unsigned i = 0; i < 8; i++) {
            g_assert_cmphex(got[i], ==, 0xff);
        }
        qtest_quit(s);
    }
}

static void test_dma_first_contention(void)
{
    for (unsigned protected = 0; protected < 2; protected++) {
        QTestState *s = start("");
        unsigned bank = 0x8000 + protected * 0x800;
        unsigned other = 0x8000 + (protected ^ 1) * 0x800;
        uint32_t words[20] = {
            I(13, 0, 1, bank), I(13, 0, 2, 0x5aa5),
            I(43, 1, 2, 0), I(43, 1, 2, 4), I(43, 1, 2, 8)
        };
        uint8_t a[16], b[32];
        int64_t dma_epoch;

        words[15] = BREAK;
        for (unsigned i = 0; i < 16; i++) {
            a[i] = 0x29 + i * 17;
        }
        for (unsigned i = 0; i < 32; i++) {
            b[i] = 0x73 + i * 21;
        }
        qtest_memwrite(s, 0x10000, a, 16);
        qtest_memwrite(s, 0x11000, b, 32);
        program(s, 0x2000, words, G_N_ELEMENTS(words));
        reg(s, 0xe000, 2);
        reg(s, 0x100, 7);
        dma_descriptor(s, 0, 0xa000, 0x10000, 16, bank);
        dma_descriptor(s, 1, 0xa000, 0x11000, 32, other);
        dma_epoch = qtest_clock_step(s, 267);
        dma_go(s, 0);
        dma_go(s, 1);
        qtest_clock_step(s, 5);
        reg(s, 0x50, 0x2000);
        reg(s, 0x40, 3);
        tick(s, dma_epoch, 6);
        g_assert_cmphex(rd(s, 0x1a8), ==, 8);
        qtest_memset(s, 0x10008, 0xee, 8);
        tick(s, dma_epoch, 7);
        g_assert_cmphex(rd(s, 0x1a8), ==, 8);
        g_assert_cmphex(rd(s, 0x1e8), ==, 24);
        tick(s, dma_epoch, 8);
        g_assert_cmphex(rd(s, 0x1e8), ==, 16);
        tick(s, dma_epoch, 9);
        g_assert_cmphex(rd(s, 0x1a8), ==, 8);
        tick(s, dma_epoch, 11);
        g_assert_cmphex(rd(s, 0x1a8), ==, 0);
        check_local(s, bank + 8, a + 8, 8);
        g_assert_cmphex(rd(s, bank), ==, 0x00005aa500005aa5ULL);
        finish(s);
        check_local(s, other, b, 32);
        qtest_quit(s);
    }
}

static void test_reservation_boundary(void)
{
    for (unsigned grant_write = 0; grant_write < 2; grant_write++) {
        QTestState *s = start("");
        const uint32_t setup[] = {
            I(13, 0, 1, 0x8000), I(13, 0, 4, 0x5aa5), BREAK, 0, 0
        };
        uint32_t words[16] = {
            I(13, 0, 2, 0x13), C(0x11, 6, 2, 16),
            grant_write ? C(0x11, 6, 0, 0) : I(43, 1, 4, 0),
            grant_write ? I(43, 1, 4, 0) : 0
        };
        const uint8_t bytes[] = { 0x19, 0x27, 0x35, 0x43,
                                  0x51, 0x6f, 0x7d, 0x8b };
        int64_t epoch;

        words[11] = BREAK;
        program(s, 0x2000, setup, G_N_ELEMENTS(setup));
        launch(s, 0x2000);
        finish(s);
        reg(s, 0x48, 0);
        reg(s, 0xe008, 8);
        reg(s, 0x8000, 0);
        reg(s, 0xf000, 0x10003);
        qtest_memwrite(s, 0x12000, bytes, sizeof(bytes));
        dma_descriptor(s, 0, 0xa000, 0x00802000, 8, 0x8000);
        program(s, 0x2100, words, G_N_ELEMENTS(words));
        epoch = qtest_clock_step(s, 1);
        reg(s, 0x50, 0x2100);
        reg(s, 0x40, 3);
        tick(s, epoch, 6);
        g_assert_cmphex(rd(s, 0x1a8), ==, 8);
        if (!grant_write) {
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x5aa5);
        } else {
            g_assert_cmphex(rd(s, 0x100), ==, 0);
        }
        tick(s, epoch, 7);
        g_assert_cmphex(rd(s, 0x188), ==, 0x219);
        check_local(s, 0x8000, bytes, sizeof(bytes));
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, grant_write ? 64 : 4);
        qtest_quit(s);
    }
}


static void transfer_state(QTestState *from, QTestState *to,
                            const char *uri)
{
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
}

static void test_pipeline_migration(void)
{
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0xa7), C(0x11, 6, 0, 23),
        I(43, 1, 2, 0), I(9, 2, 2, 1), I(43, 1, 2, 4), BREAK, 0, 0
    };

    for (unsigned phase = 0; phase <= 10; phase++) {
        g_autofree char *dir = g_strdup("/tmp/vice-msp-migrate-XXXXXX");
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        QTestState *from = start("");
        QTestState *to = start("-incoming defer");
        int64_t epoch;

        g_assert_nonnull(g_mkdtemp(dir));
        socket = g_build_filename(dir, "socket", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        program(from, 0x2000, words, G_N_ELEMENTS(words));
        reg(from, 0xe010, 12);
        epoch = launch(from, 0x2000);
        if (phase) {
            tick(from, epoch, phase);
        }
        qtest_irq_intercept_out_named(to, VICE_PATH, "sysbus-irq");
        transfer_state(from, to, uri);
        g_assert_cmphex(rd(to, 0x40), ==, rd(from, 0x40));
        g_assert_cmphex(rd(to, 0x50), ==, rd(from, 0x50));
        g_assert_cmphex(rd(to, 0x48), ==, rd(from, 0x48));
        g_assert_cmphex(rd(to, 0x108), ==, rd(from, 0x108));
        if (phase >= 6) {
            reg(to, 0xe008, 4);
        }
        if (phase >= 7) {
            qtest_writel(to, BASE + 0x8000, 0xdeadbeef);
        }
        qtest_clock_timers(to, true);
        finish(to);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8000), ==,
                        phase >= 7 ? 0xdeadbeef : 0xa7);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8004), ==, 0xa8);
        g_assert_cmphex(rd(to, 0x160), ==, phase >= 6 ? 8 : 12);
        g_assert_cmphex(rd(to, 0x48), ==, 4);
        g_assert_cmphex(rd(to, 0x68), ==, 0x2018);
        reg(to, 0xe008, 12);
        qtest_clock_step(to, 20000);
        g_assert_false(qtest_get_irq(to, 0));
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_target_wait_migration(void)
{
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 4, 0x7b39), I(13, 0, 2, 0x20c0),
        I(35, 1, 2, 32), R(2, 0, 7, 0, 9), I(43, 1, 4, 0),
        I(43, 1, 4, 16), BREAK, 0, 0
    };
    const uint32_t target[] = {
        I(43, 1, 4, 4), I(43, 1, 7, 8), BREAK, 0, 0
    };
    for (unsigned phase = 6; phase <= 9; phase++) {
        g_autofree char *dir = g_strdup("/tmp/vice-msp-target-XXXXXX");
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        QTestState *from = start("");
        QTestState *to = start("-incoming defer");
        int64_t epoch;

        g_assert_nonnull(g_mkdtemp(dir));
        socket = g_build_filename(dir, "socket", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        qtest_writel(from, BASE + 0x8020, 0x9abc2087);
        program(from, 0x2000, words, G_N_ELEMENTS(words));
        program(from, 0x2084, target, G_N_ELEMENTS(target));
        epoch = launch(from, 0x2000);
        tick(from, epoch, phase);
        transfer_state(from, to, uri);
        if (phase >= 7) {
            /* The LS result or latched target must retain the old value. */
            qtest_writel(to, BASE + 0x8020, 0x20c0);
        }
        qtest_clock_timers(to, true);
        finish(to);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8000), ==, 0x7b39);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8004), ==, 0x7b39);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8008), ==, 0x2018);
        g_assert_cmphex(qtest_readl(to, BASE + 0x8010), ==, 0);
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_yielded_migration(void)
{
    const uint32_t words[22] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x8800),
        I(13, 0, 3, 0x5aa5), 0,
        I(43, 1, 3, 0), I(43, 2, 3, 0), I(43, 1, 3, 4),
        I(43, 2, 3, 4), I(43, 1, 3, 8),
        0, 0, 0, 0, 0, 0, 0, 0, 0, BREAK, 0, 0, 0
    };
    uint8_t a[16], b[16];

    for (unsigned i = 0; i < 16; i++) {
        a[i] = 7 + i * 23;
        b[i] = 0x91 + i * 13;
    }
    for (unsigned action = 0; action < 5; action++) {
        g_autofree char *dir = g_strdup("/tmp/vice-msp-yield-XXXXXX");
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        QTestState *from = start("");
        QTestState *to = start("-incoming defer");
        int64_t epoch, dma_epoch;

        g_assert_nonnull(g_mkdtemp(dir));
        socket = g_build_filename(dir, "socket", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        qtest_memwrite(from, 0x10000, a, 16);
        qtest_memwrite(from, 0x11000, b, 16);
        program(from, 0x2000, words, G_N_ELEMENTS(words));
        reg(from, 0xe000, 2);
        dma_descriptor(from, 0, 0xa000, 0x10000, 16, 0x8000);
        dma_descriptor(from, 1, 0xa000, 0x11000, 16, 0x9000);
        epoch = launch(from, 0x2000);
        tick(from, epoch, 7);
        dma_epoch = qtest_clock_step(from, 5);
        dma_go(from, 0);
        dma_go(from, 1);
        tick(from, dma_epoch, 3);
        qtest_clock_step(from, 3);
        g_assert_cmphex(rd(from, 0x1a8), ==, 16);
        transfer_state(from, to, uri);
        g_assert_cmphex(rd(to, 0x1a8), ==, 16);
        if (action == 1) {
            reg(to, 0x40, 2);
        } else if (action == 2) {
            reg(to, 0x40, 0);
        } else if (action == 4) {
            reg(to, 0x180, 0x117);
        } else if (action == 3) {
            qtest_set_irq_in(to, VICE_PATH, "external-reset", 0, 1);
            qtest_clock_step(to, 267);
            qtest_set_irq_in(to, VICE_PATH, "external-reset", 0, 0);
        }
        qtest_memset(to, 0x10000, 0xff, 8);
        qtest_clock_timers(to, true);
        if (action != 3) {
            qtest_clock_step(to, 14);
            g_assert_cmphex(rd(to, 0x1e8), ==, 8);
            g_assert_cmphex(rd(to, 0x1a8), ==, 16);
            qtest_clock_step(to, 17);
            g_assert_cmphex(rd(to, 0x1a8), ==, action == 2 ? 8 : 16);
        }
        qtest_clock_step(to, 20000);
        if (action == 3) {
            g_assert_cmphex(rd(to, 0x40), ==, 0);
            g_assert_cmphex(rd(to, 0x188), ==, 0x10);
            g_assert_cmphex(rd(to, 0x1c8), ==, 0x10);
            g_assert_cmphex(rd(to, 0x8000), ==, 0);
            g_assert_cmphex(rd(to, 0x9000), ==, 0);
            g_assert_cmphex(rd(to, 0x160), ==, 0);
        } else {
            check_local(to, 0x8000, a, action == 4 ? 8 : 16);
            if (action == 4) {
                g_assert_cmphex(rd(to, 0x8008), ==, 0x00005aa500000000ULL);
                g_assert_cmphex(rd(to, 0x1a8), ==, 8);
            }
            check_local(to, 0x9000, b, 16);
            g_assert_cmphex(rd(to, 0x188), ==, action == 4 ? 0x118 : 0x219);
            g_assert_cmphex(rd(to, 0x1c8), ==, 0x219);
            g_assert_cmphex(rd(to, 0x160), ==,
                            action == 4 ? 0x88 : action ? 0x81 : 0x89);
            g_assert_cmphex(rd(to, 0x40), ==, action == 2 ? 0 : 2);
        }
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(socket);
        g_rmdir(dir);
    }
}

static void test_pure_dma_trace(void)
{
    const unsigned offsets[] = { 0x180, 0x188, 0x198, 0x1a0, 0x1a8,
                                 0x1c0, 0x1c8, 0x1d8, 0x1e0, 0x1e8 };
    const uint16_t remaining[8][2] = {
        {16, 0}, {16, 16}, {8, 16}, {8, 8}, {8, 8}, {8, 8}, {0, 8}, {0, 0}
    };
    uint32_t nop[24] = { 0 };
    QTestState *idle = start("");
    QTestState *core = start("");
    QTestState *machines[] = { idle, core };
    uint8_t bytes[16];
    int64_t epoch = 0;

    nop[20] = BREAK;
    for (unsigned i = 0; i < 16; i++) {
        bytes[i] = 0x17 + i * 13;
    }
    for (unsigned j = 0; j < 2; j++) {
        QTestState *s = machines[j];

        program(s, 0x2000, nop, G_N_ELEMENTS(nop));
        qtest_memwrite(s, 0x10000, bytes, 16);
        reg(s, 0xe000, 2);
        dma_descriptor(s, 0, 0xa000, 0x10000, 16, 0x8000);
        dma_descriptor(s, 1, 0xa000, 0x10000, 16, 0x8800);
        epoch = qtest_clock_step(s, 267);
        dma_go(s, 0);
        dma_go(s, 1);
    }
    qtest_clock_step(core, 5);
    reg(core, 0x50, 0x2000);
    reg(core, 0x40, 3);
    for (unsigned turn = 1; turn <= 8; turn++) {
        tick(idle, epoch, turn);
        tick(core, epoch, turn);
        for (unsigned k = 0; k < G_N_ELEMENTS(offsets); k++) {
            g_assert_cmphex(rd(core, offsets[k]), ==, rd(idle, offsets[k]));
        }
        g_assert_cmphex(rd(core, 0x1a8), ==, remaining[turn - 1][0]);
        g_assert_cmphex(rd(core, 0x1e8), ==, remaining[turn - 1][1]);
        g_assert_cmphex(rd(core, 0x108), ==,
                        (epoch + (turn * 50 + 2) / 3) * 66 / 1000);
    }
    for (unsigned j = 0; j < 2; j++) {
        check_local(machines[j], 0x8000, bytes, 16);
        check_local(machines[j], 0x8800, bytes, 16);
        qtest_quit(machines[j]);
    }
}

static void test_distinct_clocks(void)
{
    QTestState *s = start_clocks(50000000, 23000000, "");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x3279),
        I(43, 1, 2, 0), BREAK, 0, 0
    };

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    reg(s, 0x100, 7);
    qtest_clock_step(s, 400);
    reg(s, 0x50, 0x2000);
    reg(s, 0x40, 3);
    qtest_clock_step(s, 100);
    g_assert_cmphex(rd(s, 0x48), ==, 4);
    g_assert_cmphex(rd(s, 0x8000), ==, 0);
    g_assert_cmphex(rd(s, 0x108), ==, 11);
    qtest_clock_step(s, 20);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x3279);
    g_assert_cmphex(rd(s, 0x40), ==, 3);
    qtest_clock_step(s, 20);
    g_assert_cmphex(rd(s, 0x40), ==, 2);
    g_assert_cmphex(rd(s, 0x108), ==, 12);
    qtest_quit(s);
}

static void test_reset_migration(void)
{
    g_autofree char *dir = g_strdup("/tmp/vice-msp-reset-XXXXXX");
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    g_assert_nonnull(g_mkdtemp(dir));
    socket = g_build_filename(dir, "socket", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_clock_step(from, 125);
    transfer_state(from, to, uri);
    qtest_clock_step(to, 141);
    g_assert_cmpint(memtx_store(to, BASE + 0x40, 2, 8), !=, MEMTX_OK);
    qtest_clock_step(to, 1);
    reg(to, 0x40, 2);
    g_assert_cmphex(rd(to, 0x40), ==, 2);
    g_assert_cmphex(rd(to, 0x48), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_rmdir(dir);
}


static void test_common_directions(void)
{
    static const uint32_t words[] = {
        C(0x11, 2, 2, 6), C(0x11, 2, 2, 7), C(0x11, 2, 2, 22),
        C(0x11, 2, 2, 2), C(0x11, 2, 2, 13), C(0x11, 2, 2, 23),
        C(0x11, 6, 2, 1)
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(words); i++) {
        QTestState *s = start("");
        uint32_t body[12] = { words[i] };

        body[9] = BREAK;
        program(s, 0x2000, body, G_N_ELEMENTS(body));
        launch(s, 0x2000);
        qtest_clock_step(s, 20000);
        assert_held(s);
        g_assert_cmphex(rd(s, 0x160), ==, 0);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
        qtest_quit(s);
    }
}

static void test_capture_rearming(void)
{
    QTestState *s = start("");
    const uint32_t bad[] = {
        I(13, 0, 1, 0x1000), I(35, 1, 0, 0), 0, 0, 0, 0, BREAK, 0, 0
    };
    const uint32_t ri[] = { 0xfc000000, 0, 0 };
    const uint32_t bp[] = { BREAK, 0, 0 };

    program(s, 0x2000, bad, G_N_ELEMENTS(bad));
    reg(s, 0x60, 0x1007);
    launch(s, 0x2000);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 9);
    g_assert_cmphex(rd(s, 0x68), ==, 0x2004);
    g_assert_cmphex(rd(s, 0x70), ==, 0);
    g_assert_cmphex(rd(s, 0x58), ==, 0x1000);
    reg(s, 0x48, 0x80);
    g_assert_cmphex(rd(s, 0x48), ==, 0x80);
    reg(s, 0x60, 0);
    program(s, 0x2100, ri, G_N_ELEMENTS(ri));
    reg(s, 0x50, 0x2100);
    reg(s, 0x40, 3);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 0x90);
    g_assert_cmphex(rd(s, 0x68), ==, 0x2004);
    reg(s, 0xe008, 8);
    g_assert_cmphex(rd(s, 0x48), ==, 0x90);
    reg(s, 0x48, 0);
    program(s, 0x2200, bp, G_N_ELEMENTS(bp));
    reg(s, 0x50, 0x2200);
    reg(s, 0x40, 3);
    finish(s);
    g_assert_cmphex(rd(s, 0x48), ==, 4);
    g_assert_cmphex(rd(s, 0x68), ==, 0x2200);
    g_assert_cmphex(rd(s, 0x70), ==, 8);
    g_assert_cmphex(rd(s, 0x58), ==, 0x1000);
    g_assert_cmphex(rd(s, 0x160), ==, 8);
    qtest_quit(s);
}

static void test_common_read_sampling(void)
{
    QTestState *s = start("");
    const uint32_t words[] = {
        I(13, 0, 1, 0x8000), C(0x11, 2, 2, 1),
        I(43, 1, 2, 0), BREAK, 0, 0
    };
    int64_t epoch;

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    epoch = launch(s, 0x2000);
    tick(s, epoch, 8);
    g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 23);
    g_assert_cmphex(rd(s, 0x108), ==, 26);
    finish(s);
    qtest_quit(s);
}

static void test_loop_polling(void)
{
    QTestState *s = start("");
    const uint32_t words[] = { I(4, 0, 0, -1), 0, 0 };
    uint64_t pc, counter;

    program(s, 0x2000, words, G_N_ELEMENTS(words));
    launch(s, 0x2000);
    qtest_clock_step(s, 10000);
    pc = rd(s, 0x50);
    counter = rd(s, 0x108);
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(rd(s, 0x40), ==, 3);
        g_assert_cmphex(rd(s, 0x50), ==, pc);
        g_assert_cmphex(rd(s, 0x108), ==, counter);
        g_assert_cmphex(rd(s, 0x48), ==, 0);
    }
    reg(s, 0x40, 2);
    finish(s);
    g_assert_cmphex(rd(s, 0x160), ==, 0);
    qtest_quit(s);
}

static void test_interlock_classes(void)
{
    g_assert_true(sgi_vice_msp_interlock_match(I(9, 4, 7, 1), 4));
    g_assert_false(sgi_vice_msp_interlock_match(I(9, 4, 7, 1), 7));
    g_assert_true(sgi_vice_msp_interlock_match(I(35, 4, 7, 1), 7));
    g_assert_false(sgi_vice_msp_interlock_match(I(0x32, 4, 7, 1), 7));
    g_assert_false(sgi_vice_msp_interlock_match(I(0x3a, 4, 7, 1), 7));
    g_assert_true(sgi_vice_msp_interlock_match(I(0x32, 4, 7, 1), 4));
    g_assert_true(sgi_vice_msp_interlock_match(R(3, 0, 0, 0, 8), 0));
}


static void test_same_time_priority(void)
{
    const uint32_t setup[] = {
        I(13, 0, 1, 0x8000), I(13, 0, 2, 0x13),
        I(13, 0, 4, 0x5aa5), BREAK, 0, 0
    };
    const uint32_t words[16] = {
        C(0x11, 6, 2, 16), 0, I(43, 1, 4, 0),
        0, 0, 0, 0, 0, 0, 0, BREAK, 0, 0, 0, 0, 0
    };
    const uint8_t data[] = { 0x31, 0x42, 0x53, 0x64,
                             0x75, 0x86, 0x97, 0xa8 };

    for (unsigned host_first = 0; host_first < 2; host_first++) {
        QTestState *s = start("");
        int64_t epoch;

        program(s, 0x2000, setup, G_N_ELEMENTS(setup));
        launch(s, 0x2000);
        finish(s);
        reg(s, 0x48, 0);
        reg(s, 0xe008, 8);
        reg(s, 0x8000, 0);
        program(s, 0x2100, words, G_N_ELEMENTS(words));
        reg(s, 0xf000, 0x10003);
        qtest_memwrite(s, 0x12000, data, sizeof(data));
        dma_descriptor(s, 0, 0xa000, 0x00802000, 8, 0x8000);
        epoch = qtest_clock_step(s, 1);
        reg(s, 0x50, 0x2100);
        reg(s, 0x40, 3);
        if (host_first) {
            tick(s, epoch, 5);
            qtest_clock_timers(s, false);
            tick(s, epoch, 6);
            reg(s, 0x100, 0);
            g_assert_cmphex(rd(s, 0x1a8), ==, 8);
            qtest_clock_timers(s, true);
            qtest_clock_step(s, 1);
            g_assert_cmphex(rd(s, 0x1a8), ==, 0);
            check_local(s, 0x8000, data, sizeof(data));
        } else {
            tick(s, epoch, 6);
            g_assert_cmphex(rd(s, 0x1a8), ==, 8);
            g_assert_cmphex(qtest_readl(s, BASE + 0x8000), ==, 0x5aa5);
            tick(s, epoch, 7);
            g_assert_cmphex(rd(s, 0x1a8), ==, 0);
            check_local(s, 0x8000, data, sizeof(data));
        }
        finish(s);
        g_assert_cmphex(rd(s, 0x48), ==, host_first ? 64 : 4);
        qtest_quit(s);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-vice-msp/arithmetic", test_arithmetic);
    qtest_add_func("/sgi-vice-msp/groups", test_groups);
    qtest_add_func("/sgi-vice-msp/alu-forms", test_alu_forms);
    qtest_add_func("/sgi-vice-msp/gprs", test_gprs);
    qtest_add_func("/sgi-vice-msp/branches", test_branches);
    qtest_add_func("/sgi-vice-msp/register-jumps", test_register_jumps);
    qtest_add_func("/sgi-vice-msp/load-interlocks", test_load_interlocks);
    qtest_add_func("/sgi-vice-msp/immediate-false-dependency",
                   test_immediate_false_dependency);
    qtest_add_func("/sgi-vice-msp/simultaneous-faults",
                   test_simultaneous_faults);
    qtest_add_func("/sgi-vice-msp/imprecise-drain", test_imprecise_drain);
    qtest_add_func("/sgi-vice-msp/local-memory", test_local_memory);
    qtest_add_func("/sgi-vice-msp/address-faults", test_address_faults);
    qtest_add_func("/sgi-vice-msp/contention-watch", test_contention_watch);
    qtest_add_func("/sgi-vice-msp/fetch-pc", test_fetch_pc);
    qtest_add_func("/sgi-vice-msp/jump-fetch-fault", test_jump_fetch_fault);
    qtest_add_func("/sgi-vice-msp/delay-fault", test_delay_fault);
    qtest_add_func("/sgi-vice-msp/held-words", test_held_words);
    qtest_add_func("/sgi-vice-msp/held-data-cases", test_held_data_cases);
    qtest_add_func("/sgi-vice-msp/common-dma", test_common_dma);
    qtest_add_func("/sgi-vice-msp/load-jump-targets", test_load_jump_targets);
    qtest_add_func("/sgi-vice-msp/raw-interlocks", test_raw_interlocks);
    qtest_add_func("/sgi-vice-msp/host-reset-halt", test_host_reset_halt);
    qtest_add_func("/sgi-vice-msp/msp-reset-duration", test_msp_reset_duration);
    qtest_add_func("/sgi-vice-msp/ri-break-payload", test_ri_and_break_payload);
    qtest_add_func("/sgi-vice-msp/branch-wrap", test_branch_wrap);
    qtest_add_func("/sgi-vice-msp/active-ports", test_active_ports);
    qtest_add_func("/sgi-vice-msp/branch-penalty", test_branch_penalty);
    qtest_add_func("/sgi-vice-msp/initial-dma-deferral",
                   test_initial_dma_deferral);
    qtest_add_func("/sgi-vice-msp/stop-without-local-phase",
                   test_stop_without_local_phase);
    qtest_add_func("/sgi-vice-msp/msp-first-contention",
                   test_msp_first_contention);
    qtest_add_func("/sgi-vice-msp/dma-first-contention",
                   test_dma_first_contention);
    qtest_add_func("/sgi-vice-msp/reservation-boundary",
                   test_reservation_boundary);
    qtest_add_func("/sgi-vice-msp/pipeline-migration", test_pipeline_migration);
    qtest_add_func("/sgi-vice-msp/target-wait-migration",
                   test_target_wait_migration);
    qtest_add_func("/sgi-vice-msp/yielded-migration", test_yielded_migration);
    qtest_add_func("/sgi-vice-msp/pure-dma-trace", test_pure_dma_trace);
    qtest_add_func("/sgi-vice-msp/distinct-clocks", test_distinct_clocks);
    qtest_add_func("/sgi-vice-msp/reset-migration", test_reset_migration);
    qtest_add_func("/sgi-vice-msp/common-directions", test_common_directions);
    qtest_add_func("/sgi-vice-msp/capture-rearming", test_capture_rearming);
    qtest_add_func("/sgi-vice-msp/common-read-sampling",
                   test_common_read_sampling);
    qtest_add_func("/sgi-vice-msp/loop-polling", test_loop_polling);
    qtest_add_func("/sgi-vice-msp/interlock-classes", test_interlock_classes);
    qtest_add_func("/sgi-vice-msp/same-time-priority", test_same_time_priority);
    return g_test_run();
}
