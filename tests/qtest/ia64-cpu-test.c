/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Firmware-free native execution fixture. Encodings: Intel 245319-002,
 * formats A1/A4, B1, I19, M4, M24, M33 and M44. Exception state:
 * Intel 245318-002 tables 3-3, 5-6 and the BREAK vector, p. 8-19.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "ia64-imgcheck-vector.h"

#define NOP (UINT64_C(1) << 27)

static void bundle(QTestState *s, uint64_t addr, unsigned template,
                   uint64_t a, uint64_t b, uint64_t c)
{
    uint8_t bytes[16];

    stq_le_p(bytes, template | (a << 5) | (b << 46));
    stq_le_p(bytes + 8, (b >> 18) | (c << 23));
    qtest_memwrite(s, addr, bytes, sizeof(bytes));
}

static uint64_t add(unsigned dest, unsigned a, unsigned b)
{
    return (UINT64_C(8) << 37) | ((uint64_t)b << 20) | (a << 13) |
           (dest << 6);
}

static uint64_t adds(unsigned dest, int imm, unsigned source)
{
    unsigned v = (unsigned)imm & 0x3fff;

    return (UINT64_C(8) << 37) | (UINT64_C(2) << 34) |
           ((uint64_t)(v >> 13) << 36) |
           ((uint64_t)((v >> 7) & 0x3f) << 27) |
           ((uint64_t)source << 20) | ((uint64_t)(v & 0x7f) << 13) |
           (dest << 6);
}

static uint64_t st8(unsigned base, unsigned source)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x33) << 30) |
           ((uint64_t)base << 20) | (source << 13);
}

static uint64_t from_cr(unsigned dest, unsigned cr)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x24) << 27) |
           ((uint64_t)cr << 20) | (dest << 6);
}

static uint64_t branch(int displacement)
{
    unsigned field = (displacement / 16) & 0x1fffff;

    return (UINT64_C(4) << 37) | ((uint64_t)(field >> 20) << 36) |
           ((uint64_t)(field & 0xfffff) << 13);
}

/* Imported decoder's MSR forms; these test policy, not MSR hardware. */
static uint64_t to_msr(unsigned index, unsigned source)
{
    return (UINT64_C(1) << 37) | (UINT64_C(6) << 27) |
           ((uint64_t)index << 20) | (source << 13);
}

static uint64_t from_msr(unsigned dest, unsigned index)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x16) << 27) |
           ((uint64_t)index << 20) | (dest << 6);
}

static void test_inferred_reset(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S");
    g_autofree char *registers = qtest_hmp(s, "info registers");

    g_assert_nonnull(strstr(registers, "IP: 0x00000000ffffffb0"));
    g_assert_nonnull(strstr(registers, "PSR: 0x0000000000000000"));
    /* Itanium 2 hardware developer's manual 251109-001 Table 5-6. */
    g_assert_nonnull(strstr(registers, "CFM: sof=96 sol=0 sor=0 rrb.gr=0 "
                                       "rrb.fr=0 rrb.pr=0"));
    g_assert_nonnull(strstr(registers, "RSC=0x0000000000000000"));
    qtest_quit(s);
}

static void test_unknown_msr_reads_return_zero(void)
{
    g_autofree char *trace_file = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *trace = NULL;
    QTestState *s;
    int64_t deadline;
    uint64_t marker = 0;
    int fd = g_file_open_tmp("ia64-msr-XXXXXX", &trace_file, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(trace_file);
    s = qtest_initf("-machine none -cpu madison-1600-3m -m 16M -accel tcg -S "
                    "-device loader,addr=0x2000,cpu-num=0 "
                    "-d trace:ia64_unimplemented_msr -D %s", quoted);
    /* MSR 1537 and 1715 are unknown to the model, so they read zero. */
    bundle(s, 0x2000, 1, adds(8, 1537, 0), adds(9, 1715, 0),
           adds(10, 4096, 0));
    bundle(s, 0x2010, 1, adds(12, -1, 0), adds(14, -1, 0), adds(13, 0x42, 0));
    bundle(s, 0x2020, 1, from_msr(12, 8), NOP, NOP);
    bundle(s, 0x2030, 1, st8(10, 12), adds(10, 8, 10), NOP);
    bundle(s, 0x2040, 1, from_msr(14, 9), NOP, NOP);
    bundle(s, 0x2050, 1, st8(10, 14), adds(10, 8, 10), NOP);
    bundle(s, 0x2060, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x2070, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1010);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(marker, ==, 0x42);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(trace_file, &trace, NULL, NULL));
    g_assert_nonnull(strstr(trace, "index=1537 value=0x0000000000000000 "
                            "write=0 ip=0x0000000000002020 slot=0"));
    g_assert_nonnull(strstr(trace, "index=1715 value=0x0000000000000000 "
                            "write=0 ip=0x0000000000002040 slot=0"));
    unlink(trace_file);
}

/* Run until the guest stores 0x42 at marker, then stop the machine. */
static void run_to_marker_at(QTestState *s, uint64_t marker)
{
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, marker) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(qtest_readq(s, marker), ==, 0x42);
}

static QTestState *run_to_marker(QTestState *s)
{
    run_to_marker_at(s, 0x1018);
    return s;
}

static void test_unimplemented_msrs_store_the_last_write(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");

    /* MSR 300 reads back its write; MSR 301, never written, reads zero. */
    bundle(s, 0x2000, 1, adds(8, 300, 0), adds(9, 5, 0), adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_msr(8, 9), adds(11, 301, 0), adds(12, -1, 0));
    bundle(s, 0x2020, 1, from_msr(12, 8), adds(14, -1, 0), NOP);
    bundle(s, 0x2030, 1, st8(10, 12), adds(10, 8, 10), NOP);
    bundle(s, 0x2040, 1, from_msr(14, 11), adds(13, 0x42, 0), NOP);
    bundle(s, 0x2050, 1, st8(10, 14), adds(10, 8, 10), NOP);
    bundle(s, 0x2060, 1, adds(10, 4120, 0), NOP, NOP);
    bundle(s, 0x2070, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x2080, 0x11, NOP, NOP, branch(0));
    run_to_marker(s);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 5);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0);
    qtest_quit(s);
}

/* Write MSR 1546 with a value whose top 16 bits are top, then read MSR 1212. */
static void run_completion_probe(uint64_t top_hi, uint64_t top_lo,
                                 uint64_t *first, uint64_t *after_write,
                                 uint64_t *msr1226)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2030;

    /* r9 = ((top_hi * 8 + top_lo) << 48) by doubling; r8 = 1546, r11 = 1212. */
    bundle(s, 0x2000, 1, adds(8, 1546, 0), adds(9, top_hi, 0),
           adds(10, 4096, 0));
    bundle(s, 0x2010, 1, adds(11, 1212, 0), adds(12, -1, 0), adds(14, -1, 0));
    bundle(s, 0x2020, 1, adds(16, 1226, 0), adds(17, -1, 0), NOP);
    for (unsigned i = 0; i < 3; i++, at += 16) {
        bundle(s, at, 1, add(9, 9, 9), NOP, NOP);
    }
    bundle(s, at, 1, adds(9, top_lo, 9), NOP, NOP);
    at += 16;
    for (unsigned i = 0; i < 48; i++, at += 16) {
        bundle(s, at, 1, add(9, 9, 9), NOP, NOP);
    }
    bundle(s, at, 1, to_msr(8, 9), NOP, NOP);
    bundle(s, at + 16, 1, from_msr(12, 11), adds(13, 0x42, 0), NOP);
    bundle(s, at + 32, 1, from_msr(17, 16), NOP, NOP);
    bundle(s, at + 48, 1, st8(10, 12), adds(10, 8, 10), NOP);
    bundle(s, at + 64, 1, st8(10, 17), adds(10, 8, 10), NOP);
    bundle(s, at + 80, 1, to_msr(11, 0), NOP, NOP);
    bundle(s, at + 96, 1, from_msr(14, 11), NOP, NOP);
    bundle(s, at + 112, 1, st8(10, 14), adds(10, 4120, 0), NOP);
    bundle(s, at + 128, 1, st8(10, 13), NOP, NOP);
    bundle(s, at + 144, 0x11, NOP, NOP, branch(0));
    run_to_marker(s);
    *first = qtest_readq(s, 0x1000);
    *msr1226 = qtest_readq(s, 0x1008);
    *after_write = qtest_readq(s, 0x1010);
    qtest_quit(s);
}

static void test_msr1212_completion_after_the_class4_command(void)
{
    uint64_t first;
    uint64_t after;
    uint64_t known;

    /* 0x9021 << 48: bit 14 and the passing 0x317c, and MSR 1226's constant. */
    run_completion_probe(0x1204, 1, &first, &after, &known);
    g_assert_cmphex(first, ==, 0x717c);
    g_assert_cmphex(after, ==, 0);
    g_assert_cmphex(known, ==, 0x82518167826f836cULL);
    /* Another command value does not complete it: top 0x9080 << 48. */
    run_completion_probe(0x1210, 0, &first, &after, &known);
    g_assert_cmphex(first, ==, 0);
    g_assert_cmphex(known, ==, 0);
}

/* Emit one single-instruction bundle at *at and advance. */
static void step(QTestState *s, uint64_t *at, uint64_t insn)
{
    bundle(s, *at, 1, insn, NOP, NOP);
    *at += 16;
}

/* Read MSR r9 and store it at the next result slot (r10 points at it). */
static void read_and_store(QTestState *s, uint64_t *at)
{
    step(s, at, from_msr(12, 9));
    bundle(s, *at, 1, st8(10, 12), adds(10, 8, 10), NOP);
    *at += 16;
}

static void test_msr1730_training_latch(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    step(s, &at, adds(8, 1728, 0));
    step(s, &at, adds(9, 1730, 0));
    step(s, &at, adds(10, 4096, 0));
    step(s, &at, adds(11, 300, 0));
    step(s, &at, adds(20, 1, 0));
    for (unsigned i = 0; i < 48; i++) {
        step(s, &at, add(20, 20, 20));      /* r20 = bit 48 */
    }
    step(s, &at, add(21, 20, 20));          /* r21 = bit 49 */
    step(s, &at, add(22, 20, 21));
    read_and_store(s, &at);                 /* 0x1000: before any write */
    step(s, &at, to_msr(8, 20));            /* MSR 1728 selects nibble A */
    step(s, &at, to_msr(9, 20));            /* strobe, direction clear */
    read_and_store(s, &at);                 /* 0x1008: A = 0 */
    step(s, &at, to_msr(9, 22));            /* strobe, direction set */
    read_and_store(s, &at);                 /* 0x1010: A = 0xf */
    step(s, &at, to_msr(9, 21));            /* no strobe: nothing latches */
    read_and_store(s, &at);                 /* 0x1018: as written */
    step(s, &at, to_msr(8, 21));            /* MSR 1728 selects nibble B */
    step(s, &at, to_msr(9, 22));
    read_and_store(s, &at);                 /* 0x1020: B = 0xf, A as written */
    step(s, &at, to_msr(8, 22));            /* both nibbles */
    step(s, &at, to_msr(9, 22));
    read_and_store(s, &at);                 /* 0x1028: both 0xf */
    step(s, &at, to_msr(9, 20));
    read_and_store(s, &at);                 /* 0x1030: both 0, and it stays */
    step(s, &at, to_msr(11, 20));
    step(s, &at, from_msr(12, 11));         /* other MSRs stay plain storage */
    bundle(s, at, 1, st8(10, 12), adds(13, 0x42, 0), adds(10, 4352, 0));
    at += 16;
    bundle(s, at, 1, st8(10, 13), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1100);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, UINT64_C(1) << 48);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, (UINT64_C(3) << 48) | 0xf);
    g_assert_cmphex(qtest_readq(s, 0x1018), ==, UINT64_C(1) << 49);
    g_assert_cmphex(qtest_readq(s, 0x1020), ==, (UINT64_C(3) << 48) | 0xf00);
    g_assert_cmphex(qtest_readq(s, 0x1028), ==, (UINT64_C(3) << 48) | 0xf0f);
    g_assert_cmphex(qtest_readq(s, 0x1030), ==, UINT64_C(1) << 48);
    g_assert_cmphex(qtest_readq(s, 0x1038), ==, UINT64_C(1) << 48);
    qtest_quit(s);
}

/* MSR 1559 bits 21:19 are the front-side-bus agent: the slice, halved. */
static void test_msr1559_slice_follows_the_sapic_id(void)
{
    static const struct { const char *id; uint64_t bits; } cases[] = {
        { "0x0000", 0 }, { "0x0100", 1 }, { "0x0200", 2 }, { "0x0380", 3 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        g_autofree char *args = g_strdup_printf(
            "-machine none -cpu madison-1600-3m,sapic-id=%s -m 16M "
            "-accel tcg -S -device loader,addr=0x2000,cpu-num=0",
            cases[i].id);
        QTestState *s = qtest_init(args);
        uint64_t at = 0x2000;

        step(s, &at, adds(9, 1559, 0));
        step(s, &at, adds(10, 4096, 0));
        read_and_store(s, &at);
        bundle(s, at, 1, adds(13, 0x42, 0), adds(10, 4352, 0), NOP);
        at += 16;
        bundle(s, at, 1, st8(10, 13), NOP, NOP);
        bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
        run_to_marker_at(s, 0x1100);
        g_assert_cmphex((qtest_readq(s, 0x1000) >> 19) & 7, ==,
                        cases[i].bits);
        g_assert_cmphex(qtest_readq(s, 0x1000) >> 22 & 0x1f, ==, 16);
        qtest_quit(s);
    }
}

/* The PAL's CPU test needs MSR 1559 bits 26:22 >= 8; the model says 16. */
static void test_msr1559_clock_ratio(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    step(s, &at, adds(9, 1559, 0));
    step(s, &at, adds(10, 4096, 0));
    read_and_store(s, &at);
    step(s, &at, adds(11, 1560, 0));
    step(s, &at, adds(9, 1560, 0));
    read_and_store(s, &at);                 /* a neighbour stays plain */
    bundle(s, at, 1, adds(13, 0x42, 0), adds(10, 4352, 0), NOP);
    at += 16;
    bundle(s, at, 1, st8(10, 13), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1100);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, UINT64_C(16) << 22);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0);
    qtest_quit(s);
}

/* MSR 1552 reads bit 2 set once it has been written, and not before. */
static void test_msr1552_command_completes(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    step(s, &at, adds(9, 1552, 0));
    step(s, &at, adds(10, 4096, 0));
    step(s, &at, adds(11, 0x40, 0));
    read_and_store(s, &at);                 /* 0x1000: never written */
    step(s, &at, to_msr(9, 11));
    read_and_store(s, &at);                 /* 0x1008: written 0x40 */
    step(s, &at, adds(9, 1553, 0));
    read_and_store(s, &at);                 /* 0x1010: a neighbour */
    bundle(s, at, 1, adds(13, 0x42, 0), adds(10, 4352, 0), NOP);
    at += 16;
    bundle(s, at, 1, st8(10, 13), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1100);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0x44);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0);
    qtest_quit(s);
}

static void test_write_only_msrs_are_accepted(void)
{
    g_autofree char *trace_file = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *trace = NULL;
    QTestState *s;
    int64_t deadline;
    uint64_t marker = 0;
    int fd = g_file_open_tmp("ia64-msr-XXXXXX", &trace_file, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(trace_file);
    s = qtest_initf("-machine none -cpu madison-1600-3m -m 16M -accel tcg -S "
                    "-device loader,addr=0x2000,cpu-num=0 "
                    "-d trace:ia64_msr_write_accepted,trace:"
                    "ia64_unimplemented_msr -D %s", quoted);
    bundle(s, 0x2000, 1, adds(8, 1632, 0), adds(9, 0x660, 0),
           adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_msr(8, 9), adds(8, 1635, 0), adds(13, 0x42, 0));
    bundle(s, 0x2020, 1, to_msr(8, 0), NOP, NOP);
    bundle(s, 0x2030, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x2040, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1000);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(marker, ==, 0x42);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(trace_file, &trace, NULL, NULL));
    g_assert_nonnull(strstr(trace, "ia64_msr_write_accepted cpu=0 "
                            "index=1632 value=0x0000000000000660"));
    g_assert_nonnull(strstr(trace, "ia64_msr_write_accepted cpu=0 "
                            "index=1635 value=0x0000000000000000"));
    g_assert_null(strstr(trace, "ia64_unimplemented_msr"));
    unlink(trace_file);
}

static void test_diagnostic_msr_policy(void)
{
    g_autofree char *trace_file = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *trace = NULL;
    g_auto(GStrv) lines = NULL;
    QTestState *s;
    int64_t deadline;
    uint64_t marker = 0;
    unsigned events = 0;
    int fd = g_file_open_tmp("ia64-msr-XXXXXX", &trace_file, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(trace_file);
    s = qtest_initf("-machine none -cpu madison-1600-3m -m 16M -accel tcg -S "
                    "-device loader,addr=0x2000,cpu-num=0 "
                    "-d trace:ia64_unimplemented_msr,trace:ia64_msr_gate_access "
                    "-D %s", quoted);
    qtest_writeq(s, 0x1000, UINT64_MAX);
    qtest_writeq(s, 0x1008, UINT64_MAX);
    qtest_writeq(s, 0x1010, UINT64_MAX);
    bundle(s, 0x2000, 1, adds(8, 212, 0), adds(9, 7, 0), adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_msr(8, 9), adds(12, -1, 0), NOP);
    bundle(s, 0x2020, 1, from_msr(12, 8), NOP, NOP);
    bundle(s, 0x2030, 1, st8(10, 12), adds(10, 8, 10), adds(11, 1225, 0));
    bundle(s, 0x2040, 1, to_msr(11, 9), adds(12, -1, 0), NOP);
    bundle(s, 0x2050, 1, from_msr(12, 11), NOP, NOP);
    bundle(s, 0x2060, 1, st8(10, 12), adds(10, 8, 10), NOP);
    bundle(s, 0x2070, 1, to_msr(11, 9), NOP, NOP);
    bundle(s, 0x2080, 1, adds(9, 9, 0), adds(12, -1, 0), NOP);
    bundle(s, 0x2090, 1, to_msr(8, 9), NOP, NOP);
    bundle(s, 0x20a0, 1, from_msr(12, 8), NOP, NOP);
    bundle(s, 0x20b0, 1, st8(10, 12), adds(10, 8, 10), NOP);
    /* An unused read result must not eliminate the diagnostic access. */
    bundle(s, 0x20c0, 1, from_msr(14, 11), adds(13, 0x42, 0), NOP);
    bundle(s, 0x20d0, 1, st8(10, 13), adds(14, 1, 0), NOP);
    bundle(s, 0x20e0, 0x11, NOP, NOP, branch(0));

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1018);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(marker, ==, 0x42);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 7);
    /* MSR 1225 is modelled as storage, so it reads back its last write. */
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 7);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 9);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(trace_file, &trace, NULL, NULL));
    lines = g_strsplit(trace, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        events += strstr(lines[i], "ia64_unimplemented_msr") != NULL ||
                  strstr(lines[i], "ia64_msr_gate_access") != NULL;
    }
    g_assert_cmpuint(events, ==, 8);
    /* The trace retains repeats and changed values even when warnings stop. */
    g_assert_nonnull(strstr(trace, "index=1225 value=0x0000000000000007 "
                           "write=1 ip=0x0000000000002070 slot=0"));
    g_assert_nonnull(strstr(trace, "index=212 value=0x0000000000000009 "
                           "write=1 ip=0x0000000000002090 slot=0"));
    g_assert_nonnull(strstr(trace, "index=212 value=0x0000000000000009 "
                           "write=0 ip=0x00000000000020a0 slot=0"));
    g_assert_nonnull(strstr(trace, "index=1225 value=0x0000000000000007 "
                           "write=0 ip=0x00000000000020c0 slot=0"));
    unlink(trace_file);
}

/* M1, ld8: opcode 4, x6 3. */
static uint64_t ld8(unsigned dest, unsigned base)
{
    return (UINT64_C(4) << 37) | (UINT64_C(3) << 30) |
           ((uint64_t)base << 20) | (dest << 6);
}

/* M1, ld8.fill r1 = [r3]: opcode 4, x6 0x1b. */
static uint64_t ld8_fill(unsigned dest, unsigned base)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x1b) << 30) |
           ((uint64_t)base << 20) | (dest << 6);
}

static void test_bad_access_stops_at_the_load(void)
{
    g_autofree char *log_file = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *log = NULL;
    g_autofree char *registers = NULL;
    QDict *reply;
    QTestState *s;
    uint64_t load = 0x2010 + 16 * 20;
    int fd = g_file_open_tmp("ia64-bus-XXXXXX", &log_file, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(log_file);
    s = qtest_initf("-machine none -cpu madison-1600-3m -m 16M -accel tcg -S "
                    "-action panic=pause "
                    "-device loader,addr=0x2000,cpu-num=0 "
                    "-d unimp -D %s", quoted);

    /* r12 doubles from 0x400 to 1 GiB, above the 16 MiB of RAM. */
    bundle(s, 0x2000, 1, adds(12, 0x400, 0), adds(10, 4096, 0),
           adds(13, 0x42, 0));
    for (unsigned i = 0; i < 20; i++) {
        bundle(s, 0x2010 + 16 * i, 1, add(12, 12, 12), NOP, NOP);
    }
    /* The store shares the load's translation block and must not run. */
    bundle(s, load, 8, ld8(14, 12), st8(10, 13), NOP);
    bundle(s, load + 16, 0x11, NOP, NOP, branch(0));

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(s, "GUEST_PANICKED");
    reply = qtest_qmp(s, "{ 'execute': 'query-status' }");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(reply, "return"), "status"),
                    ==, "guest-panicked");
    qobject_unref(reply);
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "IP: 0x0000000000002150"));
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log_file, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "bus error on load of 8 bytes at "
                            "0x0000000040000000 (physical 0x40000000) "
                            "ip=0x0000000000002150"));
    unlink(log_file);
}

/* With -action panic=none a checkstop halts the CPU; the VM keeps running. */
static void test_bus_error_is_a_guest_panic(void)
{
    QDict *reply;
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m -m 16M "
                              "-accel tcg -S -action panic=none "
                              "-device loader,addr=0x2000,cpu-num=0");

    bundle(s, 0x2000, 1, adds(12, 0x400, 0), adds(10, 4096, 0),
           adds(13, 0x42, 0));
    for (unsigned i = 0; i < 20; i++) {
        bundle(s, 0x2010 + 16 * i, 1, add(12, 12, 12), NOP, NOP);
    }
    bundle(s, 0x2150, 8, ld8(14, 12), st8(10, 13), NOP);
    bundle(s, 0x2160, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(s, "GUEST_PANICKED");
    reply = qtest_qmp(s, "{ 'execute': 'query-status' }");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(reply, "return"), "status"),
                    ==, "running");
    qobject_unref(reply);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    qtest_quit(s);
}

static uint64_t ld8_acq(unsigned dest, unsigned base);

static uint64_t to_cr(unsigned cr, unsigned source)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x2c) << 27) |
           ((uint64_t)cr << 20) | (source << 13);
}

/* B8, rfi: opcode 0, x6 0x08. */
#define RFI (UINT64_C(0x08) << 27)

static void test_rfi_enters_ipsr_at_iip(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    g_autofree char *registers = NULL;
    int64_t deadline;
    uint64_t marker = 0;

    /* PSR.ic is clear after reset, so cr.ipsr and cr.iip are writable. */
    bundle(s, 0x2000, 1, adds(8, 0x18, 0), adds(9, 0x1800, 0),
           adds(10, 0x1000, 0));
    bundle(s, 0x2010, 1, to_cr(16, 8), adds(13, 0x42, 0), NOP);
    bundle(s, 0x2020, 1, to_cr(19, 9), NOP, NOP);
    bundle(s, 0x2030, 1, (UINT64_C(3) << 31) | (1 << 27), NOP, NOP);
    bundle(s, 0x2040, 0x11, NOP, NOP, RFI);
    bundle(s, 0x1800, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x1810, 0x11, NOP, NOP, branch(0));

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1000);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(marker, ==, 0x42);
    /* rfi loads PSR from cr.ipsr. */
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "PSR: 0x0000000000000018"));
    qtest_quit(s);
}

static uint64_t br_ia(unsigned b)
{
    return (UINT64_C(0x20) << 27) | ((uint64_t)b << 13) | (1 << 6);
}

/* Gate MSR 1225 to 4, set r2 and run br.ia at 0x2020; r10 is 0x1000. */
static QTestState *bria_program(unsigned r2, unsigned r5)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S -action panic=pause "
                              "-device loader,addr=0x2000,cpu-num=0");

    bundle(s, 0x2000, 1, adds(11, 1225, 0), adds(9, 4, 0), adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_msr(11, 9), adds(2, r2, 0), adds(13, 0x42, 0));
    bundle(s, 0x2020, 0x11, adds(5, r5, 0), NOP, br_ia(0));
    bundle(s, 0x2030, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x2040, 0x11, NOP, NOP, branch(0));
    return s;
}

static void test_bria_service_continues(void)
{
    QTestState *s = bria_program(0, 0);
    int64_t deadline;
    uint64_t marker = 0;

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1000);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    /* Execution resumed at the bundle after br.ia, not in IA-32 mode. */
    g_assert_cmphex(marker, ==, 0x42);
    qtest_quit(s);
}

static void test_bria_without_gate_stops(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S -action panic=pause "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_autofree char *status = NULL;
    g_autofree char *registers = NULL;

    /* MSR 1225 is zero: the IA-32 transition is not modelled, so stop. */
    bundle(s, 0x2000, 0x11, NOP, NOP, br_ia(0));
    bundle(s, 0x2010, 1, st8(10, 13), NOP, NOP);
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");

        g_free(status);
        status = g_strdup(qdict_get_str(qdict_get_qdict(r, "return"),
                                        "status"));
        qobject_unref(r);
    } while (strcmp(status, "guest-panicked") &&
             g_get_monotonic_time() < deadline);
    g_assert_cmpstr(status, ==, "guest-panicked");
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "IP: 0x0000000000002000"));
    qtest_quit(s);
}

static void test_rfi_to_ia32_stops(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S -action panic=pause "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    uint64_t rfi_at = 0x2020 + 16 * 24 + 32;
    g_autofree char *status = NULL;
    g_autofree char *registers = NULL;
    g_autofree char *want = NULL;

    /* cr.ipsr = 1 << 34 (psr.is) by doubling 0x400 24 times. */
    bundle(s, 0x2000, 1, adds(8, 0x400, 0), adds(9, 0x1800, 0),
           adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_cr(19, 9), adds(13, 0x42, 0), NOP);
    for (unsigned i = 0; i < 24; i++) {
        bundle(s, 0x2020 + 16 * i, 1, add(8, 8, 8), NOP, NOP);
    }
    bundle(s, 0x2020 + 16 * 24, 1, to_cr(16, 8), NOP, NOP);
    bundle(s, 0x2020 + 16 * 24 + 16, 1, (UINT64_C(3) << 31) | (1 << 27),
           NOP, NOP);
    bundle(s, rfi_at, 0x11, NOP, NOP, RFI);
    bundle(s, 0x1800, 1, st8(10, 13), NOP, NOP);
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");

        g_free(status);
        status = g_strdup(qdict_get_str(qdict_get_qdict(r, "return"),
                                        "status"));
        qobject_unref(r);
    } while (strcmp(status, "guest-panicked") &&
             g_get_monotonic_time() < deadline);
    g_assert_cmpstr(status, ==, "guest-panicked");
    registers = qtest_hmp(s, "info registers");
    want = g_strdup_printf("IP: 0x%016" PRIx64, rfi_at);
    g_assert_nonnull(strstr(registers, want));
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    qtest_quit(s);
}

/* The CPU declares itself unmigratable. */
static void test_migration_is_refused(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m -m 16M "
                              "-accel tcg -S");
    QDict *r = qtest_qmp(s, "{ 'execute': 'migrate', 'arguments': "
                         "{ 'uri': 'exec:cat > /dev/null' } }");
    QDict *err = qdict_get_qdict(r, "error");

    g_assert_nonnull(err);
    g_assert_nonnull(strstr(qdict_get_str(err, "desc"), "non-migratable"));
    qobject_unref(r);
    qtest_quit(s);
}

/* M34, alloc r32 = ar.pfs, 0, 4, 0, 0: opcode 1, x3 6; sof 4, sol 4. */
#define ALLOC_R32_L4 ((UINT64_C(1) << 37) | (UINT64_C(6) << 33) | \
                      (32 << 6) | (4 << 13) | (4 << 20))
/* B3, br.call b0 = next bundle: opcode 5, imm20b 1. */
#define CALL_NEXT ((UINT64_C(5) << 37) | (UINT64_C(1) << 13))
/* M35, mov psr.l = r9. */
#define MOV_PSRL_R9 ((UINT64_C(1) << 37) | (UINT64_C(0x2d) << 27) | (9 << 13))
/* M35, mov psr.l = r8: opcode 1, x6 0x2d. */
#define MOV_PSRL_R8 ((UINT64_C(1) << 37) | (UINT64_C(0x2d) << 27) | (8 << 13))

/*
 * Run a program that ends in a gated br.ia and wait for an exception trace
 * line containing want. The text of the trace is returned.
 */
static char *run_bria_order(bool dirty_rse, const char *want)
{
    g_autofree char *trace_file = NULL;
    g_autofree char *quoted = NULL;
    char *trace = NULL;
    QTestState *s;
    int64_t deadline;
    uint64_t at = 0x2020;
    int fd = g_file_open_tmp("ia64-order-XXXXXX", &trace_file, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    quoted = g_shell_quote(trace_file);
    s = qtest_initf("-machine none -cpu madison-1600-3m -m 16M -accel tcg -S "
                    "-device loader,addr=0x2000,cpu-num=0 "
                    "-d trace:ia64_exception_raise,trace:ia64_br_ia_service "
                    "-D %s", quoted);
    bundle(s, 0x2000, 1, adds(11, 1225, 0), adds(9, 4, 0), adds(8, 0x1000, 0));
    bundle(s, 0x2010, 1, to_msr(11, 9), NOP, NOP);
    if (dirty_rse) {
        /* A call pushes the caller's four locals onto the dirty partition. */
        bundle(s, at, 1, ALLOC_R32_L4, NOP, NOP);
        bundle(s, at + 16, 0x11, NOP, NOP, CALL_NEXT);
        at += 32;
    } else {
        /* psr.di is bit 22: double r8 from 0x1000 ten times. */
        for (unsigned i = 0; i < 10; i++, at += 16) {
            bundle(s, at, 1, add(8, 8, 8), NOP, NOP);
        }
        bundle(s, at, 1, MOV_PSRL_R8, NOP, NOP);
        at += 16;
    }
    bundle(s, at, 0x11, NOP, NOP, br_ia(0));

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        g_free(trace);
        trace = NULL;
        g_file_get_contents(trace_file, &trace, NULL, NULL);
        g_usleep(10000);
    } while ((!trace || !strstr(trace, want)) &&
             g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    qtest_quit(s);
    g_file_get_contents(trace_file, &trace, NULL, NULL);
    unlink(trace_file);
    return trace;
}

static void test_bria_dirty_rse_faults_before_service(void)
{
    g_autofree char *trace = run_bria_order(true, "exception=2 ip=0x");

    /* Illegal Operation at the br.ia bundle, and no service call. */
    g_assert_nonnull(strstr(trace, "ia64_exception_raise cpu=0 exception=2 "
                            "ip=0x0000000000002040"));
    g_assert_null(strstr(trace, "ia64_br_ia_service"));
}

static void test_bria_psr_di_faults_before_service(void)
{
    g_autofree char *trace = run_bria_order(false, "exception=30 ip=0x");

    /* Disabled Instruction Set Transition, and no service call. */
    g_assert_nonnull(strstr(trace, "ia64_exception_raise cpu=0 exception=30 "
                            "ip=0x00000000000020d0"));
    g_assert_null(strstr(trace, "ia64_br_ia_service"));
}

/*
 * Run one bundle with an instruction word in the given slot and unit; the
 * Illegal Operation handler at IVA 0 + 0x5400 stores 0x77 at 0x1008.
 */
static char *run_reserved_word(const char *cpu, unsigned template, uint64_t a,
                               uint64_t b, uint64_t c, bool *faulted)
{
    g_autofree char *args = g_strdup_printf(
        "-machine none -cpu %s -m 16M -accel tcg -S -action panic=pause "
        "-device loader,addr=0x2000,cpu-num=0", cpu);
    QTestState *s = qtest_init(args);
    char *registers;
    int64_t deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;

    bundle(s, 0x5400, 1, adds(11, 0x77, 0), adds(12, 4104, 0), NOP);
    bundle(s, 0x5410, 1, st8(12, 11), NOP, NOP);
    bundle(s, 0x5420, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x2000, template, a, b, c);
    bundle(s, 0x2010, 1, st8(10, 13), NOP, NOP);
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1008) != 0x77 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    *faulted = qtest_readq(s, 0x1008) == 0x77;
    registers = qtest_hmp(s, "info registers");
    qtest_quit(s);
    return registers;
}

/* Another family has no such words: Illegal Operation at the word's bundle. */
static void test_reserved_pal_words_fault_on_another_family(void)
{
    bool faulted;

    /* Table 4-43 cell x4 1, x2 2 in an M slot; Table 4-48 x6 9 in a B slot. */
    g_free(run_reserved_word("merced-800", 1, NOP, 0x108000000ULL, NOP,
                             &faulted));
    g_assert_true(faulted);
    g_free(run_reserved_word("merced-800", 0x11, NOP, NOP, 0x48000000ULL,
                             &faulted));
    g_assert_true(faulted);
}

/*
 * Run a program with a reserved word in the bundle at word_at (M in slot 1 or B
 * in slot 2); true if it reached its marker store, false if the VM stopped.
 */
static bool run_noop_word(const char *cpu, uint64_t word_at, bool b_word,
                          uint64_t m_word)
{
    g_autofree char *args = g_strdup_printf(
        "-machine none -cpu %s -m 4G -accel tcg -S -action panic=pause "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0", cpu, word_at - 0x10);
    QTestState *s = qtest_init(args);
    int64_t deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    bool ran = false;

    /* IVA is 0: an Illegal Operation fault stores 0x77 at 0x1008. */
    bundle(s, 0x5400, 1, adds(11, 0x77, 0), adds(12, 4104, 0), NOP);
    bundle(s, 0x5410, 1, st8(12, 11), NOP, NOP);
    bundle(s, 0x5420, 0x11, NOP, NOP, branch(0));
    bundle(s, word_at - 0x10, 1, adds(10, 4096, 0), adds(13, 0x42, 0), NOP);
    if (b_word) {
        bundle(s, word_at, 0x11, NOP, NOP, 0x48000000ULL);
    } else {
        /* M;MI; with the reserved word in slot 1, as in PAL_A 5.37. */
        bundle(s, word_at, 0x0b, NOP, m_word, NOP);
    }
    bundle(s, word_at + 16, 1, st8(10, 13), NOP, NOP);
    bundle(s, word_at + 32, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");
        g_autofree char *st = g_strdup(qdict_get_str(
            qdict_get_qdict(r, "return"), "status"));

        qobject_unref(r);
        if (!strcmp(st, "guest-panicked") || qtest_readq(s, 0x1008) == 0x77) {
            break;
        }
        if (qtest_readq(s, 0x1000) == 0x42) {
            ran = true;
            break;
        }
    } while (g_get_monotonic_time() < deadline);
    if (ran) {
        g_autofree char *registers = qtest_hmp(s, "info registers");

        /* The word changed no register, and the program kept going. */
        g_assert_nonnull(strstr(registers, "r8   0x0000000000000000"));
        g_assert_nonnull(strstr(registers, "r10  0x0000000000001000"));
        g_assert_nonnull(strstr(registers, "r13  0x0000000000000042"));
    }
    qtest_quit(s);
    return ran;
}

static bool run_noop_site(const char *cpu, uint64_t word_at, bool b_word)
{
    return run_noop_word(cpu, word_at, b_word, 0x108000000ULL);
}

static void test_reserved_words_run_at_pl0(void)
{
    /* The verified PAL_A 5.37 site, neighbours, generic, 1.27 and PAL_B. */
    g_assert_true(run_noop_site("madison-1600-3m", 0xfffca400,
                                false));
    g_assert_true(run_noop_site("madison-1600-3m", 0xfffca410,
                                false));
    g_assert_true(run_noop_site("madison-1600-3m", 0xffff9000,
                                false));
    g_assert_true(run_noop_site("madison-1600-3m", 0xfffd0470,
                                false));
    g_assert_true(run_noop_site("madison-1600-3m", 0xffed51c0,
                                false));
    g_assert_true(run_noop_site("mckinley-1000", 0xfffca400,
                                false));
}

/* The words run wherever they are fetched: the PROM's RAM copy, or RAM. */
static void test_reserved_words_run_in_ram(void)
{
    g_assert_true(run_noop_site("madison-1600-3m", 0x1812b80, false));
    g_assert_true(run_noop_site("madison-1600-3m", 0x2000000, false));
    g_assert_true(run_noop_site("madison-1600-3m", 0x10000, false));
}

/*
 * The reserved B word returns from interruption: IP from cr.iip, PSR from
 * cr.ipsr, and cr.ifs is ignored.
 */
static bool run_reserved_b(uint64_t at, bool *frame_kept)
{
    g_autofree char *args = g_strdup_printf(
        "-machine none -cpu madison-1600-3m -m 4G "
        "-accel tcg -S -action panic=pause "
        "-device loader,addr=0x%" PRIx64 ",cpu-num=0", at - 0x60);
    QTestState *s = qtest_init(args);
    int64_t deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    bool ran = false;

    /* cr.ifs with v set and a frame of 5, which a plain rfi would install. */
    qtest_writeq(s, 0x1100, (UINT64_C(1) << 63) | 5);
    bundle(s, at - 0x60, 1, adds(8, 0x18, 0), adds(9, 0x1800, 0),
           adds(10, 0x1000, 0));
    bundle(s, at - 0x50, 1, to_cr(16, 8), adds(13, 0x42, 0),
           adds(12, 0x1100, 0));
    bundle(s, at - 0x40, 1, to_cr(19, 9), NOP, NOP);
    bundle(s, at - 0x30, 1, ld8_acq(11, 12), NOP, NOP);
    bundle(s, at - 0x20, 1, to_cr(23, 11), NOP, NOP);
    bundle(s, at - 0x10, 1, (UINT64_C(3) << 31) | (1 << 27), NOP, NOP);
    bundle(s, at, 0x11, NOP, NOP, 0x48000000ULL);
    bundle(s, 0x1800, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x1810, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");
        g_autofree char *st = g_strdup(qdict_get_str(
            qdict_get_qdict(r, "return"), "status"));

        qobject_unref(r);
        if (!strcmp(st, "guest-panicked")) {
            break;
        }
        if (qtest_readq(s, 0x1000) == 0x42) {
            ran = true;
            break;
        }
    } while (g_get_monotonic_time() < deadline);
    if (ran) {
        g_autofree char *registers = qtest_hmp(s, "info registers");

        *frame_kept = strstr(registers, "CFM: sof=5 ") == NULL;
        g_assert_nonnull(strstr(registers, "PSR: 0x0000000000000018"));
    }
    qtest_quit(s);
    return ran;
}

static void test_reserved_b_word_returns_from_interruption(void)
{
    bool frame_kept = false;

    g_assert_true(run_reserved_b(0xffed51c0, &frame_kept));
    g_assert_true(frame_kept);
}

/* The cache-line word of the PAL flush loops (opcode 1, x6 0x36, r3 = r28). */
static void test_reserved_cache_word_runs_at_pl0(void)
{
    const uint64_t word = 0x21b1c00000ULL;

    g_assert_true(run_noop_word("madison-1600-3m", 0xffe92ba0,
                                false, word));
    /* Another r3 runs too; another family or an undefined word faults. */
    g_assert_true(run_noop_word("madison-1600-3m", 0xffe92ba0,
                                false, 0x21b1a00000ULL));
    g_assert_false(run_noop_word("merced-800", 0xffe92ba0, false,
                                 word));
    /* r3 = 0 never occurs in the firmware and is not covered. */
    g_assert_false(run_noop_word("madison-1600-3m", 0xffe92ba0,
                                 false, 0x21b0000000ULL));
    /* A set r2 makes it a different, undefined word. */
    g_assert_false(run_noop_word("madison-1600-3m", 0xffe92ba0,
                                 false, word | (5ULL << 13)));
}

/* Above privilege level 0 a reserved word takes Illegal Operation at IVA. */
static void test_reserved_words_fault_above_pl0(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m -m 16M "
                              "-accel tcg -S -action panic=pause "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    qtest_writeq(s, 0x1100, 0x300000018ULL);        /* psr.cpl 3 */
    qtest_writeq(s, 0x1108, 0x3000);
    /* IVA is 0: the Illegal Operation handler stores 0x77 at 0x1010. */
    bundle(s, 0x5400, 1, adds(11, 0x77, 0), adds(12, 0x1010, 0), NOP);
    bundle(s, 0x5410, 1, st8(12, 11), NOP, NOP);
    bundle(s, 0x5420, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x2000, 1, adds(5, 0x1100, 0), adds(6, 0x1108, 0), NOP);
    bundle(s, 0x2010, 1, ld8(8, 5), ld8(9, 6), NOP);
    bundle(s, 0x2020, 1, to_cr(16, 8), NOP, NOP);
    bundle(s, 0x2030, 1, to_cr(19, 9), NOP, NOP);
    bundle(s, 0x2040, 1, to_cr(23, 0), NOP, NOP);
    bundle(s, 0x2050, 0x11, NOP, NOP, RFI);
    /* The word at CPL 3, with a store after it that must not run. */
    bundle(s, 0x3000, 0x0b, NOP, 0x108000000ULL, NOP);
    bundle(s, 0x3010, 1, adds(10, 0x1000, 0), adds(13, 0x42, 0), NOP);
    bundle(s, 0x3020, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1010) != 0x77 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x77);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    qtest_quit(s);
}

static void test_reserved_word_stops_on_another_family(void)
{
    /* Merced reports family 7, not 0x1f. */
    g_assert_false(run_noop_site("merced-800", 0xfffca400,
                                 false));
    g_assert_false(run_noop_site("merced-800", 0xfffca400, true));
}

static void test_other_reserved_words_still_fault(void)
{
    bool faulted;

    /* Same cell with one more bit set is not one of the PROM's words. */
    g_free(run_reserved_word("madison-1600-3m", 1, NOP, 0x108000040ULL, NOP,
                             &faulted));
    g_assert_true(faulted);
}

#define IC_WRAP_UNITS   1  /* r3 = 2^60 + units */
#define IC_HIGH_IMAGE   2  /* r2 = 4 GiB, past the end of RAM */
#define IC_NO_GATE      4  /* leave MSR 1225 clear */
#define IC_NO_KEY       8  /* 1 GiB of RAM: the key addresses are unmapped */
#define IC_NO_KEY_ADDR  16 /* leave the pal-auth-key-addr property at 0 */

/* Append "reg = 0x1000 << doublings" to the program at *at. */
static void load_shifted(QTestState *s, uint64_t *at, unsigned reg,
                         unsigned doublings)
{
    bundle(s, *at, 1, adds(reg, 0x1000, 0), NOP, NOP);
    *at += 16;
    for (unsigned i = 0; i < doublings; i++, *at += 16) {
        bundle(s, *at, 1, add(reg, reg, reg), NOP, NOP);
    }
}

/*
 * Run the br.ia image check on a component at 0x1800, keyed at the generic
 * PAL_A's addresses. Execution continues at b0 (marker 0x1010); *r1 gets r1.
 */
static QTestState *start_image_check(const uint8_t *component, uint64_t units,
                                     unsigned flags)
{
    g_autofree char *args = g_strdup_printf(
        "-machine none -cpu madison-1600-3m%s -m %s -accel tcg -S "
        "-action panic=pause -device loader,addr=0x2000,cpu-num=0",
        (flags & IC_NO_KEY_ADDR) ? "" : ",pal-auth-key-addr=0xffffa1d0",
        (flags & IC_NO_KEY) ? "1G" : "4G");
    QTestState *s = qtest_init(args);
    uint64_t at = 0x2020;

    qtest_memwrite(s, 0xffffa1d0, vec_p, sizeof(vec_p));
    qtest_memwrite(s, 0xffffa250, vec_g, sizeof(vec_g));
    qtest_memwrite(s, 0xffffa2d0, vec_y, sizeof(vec_y));
    qtest_memwrite(s, 0xffffa350, vec_q, sizeof(vec_q));
    qtest_memwrite(s, 0x1800, component, sizeof(vec_component));
    qtest_writeq(s, 0x1000, UINT64_MAX);
    /* b0 is zero after reset, so the service must resume at address 0. */
    bundle(s, 0x0, 1, st8(10, 1), adds(10, 16, 10), NOP);
    bundle(s, 0x10, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x20, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x2000, 1, adds(11, 1225, 0), adds(9, 4, 0), adds(10, 4096, 0));
    bundle(s, 0x2010, 1, (flags & IC_NO_GATE) ? NOP : to_msr(11, 9),
           adds(13, 0x42, 0), NOP);
    if (flags & IC_HIGH_IMAGE) {
        load_shifted(s, &at, 2, 20);
    } else {
        bundle(s, at, 1, adds(2, 0x1800, 0), NOP, NOP);
        at += 16;
    }
    if (flags & IC_WRAP_UNITS) {
        load_shifted(s, &at, 3, 48);
        bundle(s, at, 1, adds(14, units, 0), NOP, NOP);
        bundle(s, at + 16, 1, add(3, 3, 14), NOP, NOP);
        at += 32;
    } else {
        bundle(s, at, 1, adds(3, units, 0), NOP, NOP);
        at += 16;
    }
    bundle(s, at, 1, adds(5, 1, 0), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, br_ia(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    return s;
}

static uint64_t run_image_check(const uint8_t *component, uint64_t units,
                                unsigned flags)
{
    QTestState *s = start_image_check(component, units, flags);
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint64_t marker = 0;
    uint64_t r1;

    do {
        marker = qtest_readq(s, 0x1010);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(marker, ==, 0x42);
    r1 = qtest_readq(s, 0x1000);
    qtest_quit(s);
    return r1;
}

static void test_bria_image_check_accepts_signed_image(void)
{
    g_assert_cmphex(run_image_check(vec_component, 63, 0), ==, 0);
}

static void test_bria_image_check_rejects_tampered_image(void)
{
    uint8_t tampered[sizeof(vec_component)];

    memcpy(tampered, vec_component, sizeof(tampered));
    tampered[100] ^= 1;
    g_assert_cmphex(run_image_check(tampered, 63, 0), ==, 1);
}

static void test_bria_image_check_rejects_wrong_size(void)
{
    /* A shorter component hashes different bytes and has no signature. */
    g_assert_cmphex(run_image_check(vec_component, 62, 0), ==, 1);
}

static void test_bria_image_check_rejects_tiny_sizes(void)
{
    /* No message bytes are left at or below 80 bytes. */
    g_assert_cmphex(run_image_check(vec_component, 5, 0), ==, 1);
    g_assert_cmphex(run_image_check(vec_component, 0, 0), ==, 1);
}

static void test_bria_image_check_rejects_wrapped_size(void)
{
    /* 2^60 + 63 times 16 wraps to 1008, the size of the signed component. */
    g_assert_cmphex(run_image_check(vec_component, 63, IC_WRAP_UNITS), ==, 1);
}

static void test_bria_image_check_rejects_unreadable_image(void)
{
    g_assert_cmphex(run_image_check(vec_component, 63, IC_HIGH_IMAGE), ==, 1);
}

static void test_bria_image_check_rejects_unreadable_key(void)
{
    g_assert_cmphex(run_image_check(vec_component, 63, IC_NO_KEY), ==, 1);
}

static void test_bria_image_check_rejects_without_a_key_address(void)
{
    g_assert_cmphex(run_image_check(vec_component, 63, IC_NO_KEY_ADDR), ==, 1);
}

static void test_bria_image_check_needs_the_gate(void)
{
    QTestState *s = start_image_check(vec_component, 63, IC_NO_GATE);
    QDict *reply;

    /* With MSR 1225 clear the call is the IA-32 transition and stops. */
    qtest_qmp_eventwait(s, "GUEST_PANICKED");
    reply = qtest_qmp(s, "{ 'execute': 'query-status' }");
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(reply, "return"), "status"),
                    ==, "guest-panicked");
    qobject_unref(reply);
    g_assert_cmphex(qtest_readq(s, 0x1010), !=, 0x42);
    qtest_quit(s);
}

static void test_bria_service_image_stops(void)
{
    QTestState *s = bria_program(1, 2);
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_autofree char *status = NULL;
    g_autofree char *registers = NULL;

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");

        g_free(status);
        status = g_strdup(qdict_get_str(qdict_get_qdict(r, "return"),
                                        "status"));
        qobject_unref(r);
    } while (strcmp(status, "guest-panicked") &&
             g_get_monotonic_time() < deadline);
    g_assert_cmpstr(status, ==, "guest-panicked");
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "IP: 0x0000000000002020"));
    /* The store after br.ia never ran. */
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0);
    qtest_quit(s);
}

/* Run PAL_B's br.ia shape (r2 set, r5 given, r1 = 0x99); true if it resumed. */
static bool run_table_load(unsigned r5, uint64_t *r1_after)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S -action panic=pause "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_autofree char *status = NULL;
    bool resumed = false;

    bundle(s, 0x2000, 1, adds(11, 1225, 0), adds(9, 4, 0), adds(10, 4096, 0));
    bundle(s, 0x2010, 1, to_msr(11, 9), adds(2, 1, 0), adds(5, r5, 0));
    bundle(s, 0x2020, 1, adds(1, 0x99, 0), adds(15, 4104, 0), NOP);
    bundle(s, 0x2030, 0x11, NOP, NOP, CALL_NEXT);
    /* b0 = 0x2040: count passes and store r1, then br.ia. */
    bundle(s, 0x2040, 1, st8(10, 14), adds(14, 1, 14), NOP);
    bundle(s, 0x2050, 1, st8(15, 1), NOP, NOP);
    bundle(s, 0x2060, 0x11, NOP, NOP, br_ia(0));
    bundle(s, 0x2070, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    do {
        QDict *r = qtest_qmp(s, "{ 'execute': 'query-status' }");

        g_free(status);
        status = g_strdup(qdict_get_str(qdict_get_qdict(r, "return"),
                                        "status"));
        qobject_unref(r);
        /* Pass count 2 stored: the second pass ran, so br.ia resumed at b0. */
        if (qtest_readq(s, 0x1000) >= 2) {
            resumed = true;
            break;
        }
    } while (strcmp(status, "guest-panicked") &&
             g_get_monotonic_time() < deadline);
    *r1_after = qtest_readq(s, 0x1008);
    qtest_quit(s);
    return resumed;
}

static void test_bria_table_load_reports_done(void)
{
    uint64_t r1;

    /* Resume at b0 with r1 = 0 (it was 0x99). */
    g_assert_true(run_table_load(0, &r1));
    g_assert_cmphex(r1, ==, 0);
    /* With another r5 the VM stops. */
    g_assert_false(run_table_load(2, &r1));
}


/* M29, mov ar3 = r2 and M31, mov r1 = ar3: opcode 1, x6 0x2a and 0x22. */
static uint64_t m_to_ar(unsigned ar, unsigned src)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x2a) << 27) | ((uint64_t)ar << 20) |
           ((uint64_t)src << 13);
}

static uint64_t m_from_ar(unsigned dest, unsigned ar)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x22) << 27) | ((uint64_t)ar << 20) |
           ((uint64_t)dest << 6);
}

/*
 * AR.RNAT bits at or above BSPSTORE{8:3} are undefined (SDM Vol. 2 6.1.3);
 * Madison reads back what was written, as the PAL's self-test needs.
 */
static void test_rnat_reads_back_what_was_written(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    const uint64_t pat = 0xdeadbeefcafef00dULL;
    uint64_t at = 0x2000;
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    qtest_writeq(s, 0x1100, pat);
    bundle(s, at, 1, adds(10, 4352, 0), adds(11, 0x400, 0), adds(13, 0x42, 0));
    bundle(s, at + 0x10, 1, ld8(9, 10), adds(14, 4096, 0), NOP);
    /* BSPSTORE{8:3} = 0: only the lowest RNAT bit is defined. */
    bundle(s, at + 0x20, 1, m_to_ar(18, 11), NOP, NOP);
    bundle(s, at + 0x30, 1, m_to_ar(19, 9), NOP, NOP);
    bundle(s, at + 0x40, 1, m_from_ar(12, 19), NOP, NOP);
    bundle(s, at + 0x50, 1, st8(14, 12), adds(14, 8, 14), NOP);
    /* Rewriting BSPSTORE leaves RNAT as it was. */
    bundle(s, at + 0x60, 1, m_to_ar(18, 11), NOP, NOP);
    bundle(s, at + 0x70, 1, m_from_ar(12, 19), NOP, NOP);
    bundle(s, at + 0x80, 1, st8(14, 12), adds(14, 4096 + 0x300, 0), NOP);
    bundle(s, at + 0x90, 1, st8(14, 13), NOP, NOP);
    bundle(s, at + 0xa0, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1300) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(qtest_readq(s, 0x1300), ==, 0x42);
    /* All 63 bits come back, and bit 63 reads 0. */
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, pat & INT64_MAX);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, pat & INT64_MAX);
    qtest_quit(s);
}

/* I20, chk.s.i r2, imm21: opcode 0, x3 1, imm7a in bits 12:6. */
static uint64_t chk_s_i(unsigned r2, unsigned imm21)
{
    return (UINT64_C(1) << 33) | ((uint64_t)r2 << 13) |
           ((uint64_t)(imm21 & 0x7f) << 6);
}

/*
 * A failing chk takes the Speculative Operation fault (0x5700) with PSR.it
 * clear (251110-003 2.4.1, SDM Vol. 2 8.1); ISR.code is 1, IIM the immediate.
 */
static void test_chk_s_i_takes_the_speculation_vector(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    /* IVA = 0x8000, handler at IVA + 0x5700. */
    bundle(s, 0xd700, 1, from_cr(20, 17), NOP, NOP);
    bundle(s, 0xd710, 1, from_cr(21, 24), NOP, NOP);
    bundle(s, 0xd720, 1, st8(10, 20), NOP, NOP);
    bundle(s, 0xd730, 1, st8(11, 21), NOP, NOP);
    bundle(s, 0xd740, 1, st8(17, 22), NOP, NOP);
    bundle(s, 0xd750, 1, st8(18, 23), NOP, NOP);
    bundle(s, 0xd760, 0x11, NOP, NOP, branch(0));
    bundle(s, at, 1, adds(8, 0x1000, 0), adds(9, 0x1000, 0), adds(10, 4096, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), add(9, 9, 9), adds(11, 4104, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(17, 4112, 0), adds(18, 4120, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(22, 0x33, 0), adds(23, 0x42, 0));
    at += 16;
    bundle(s, at, 1, to_cr(2, 8), adds(12, -1, 0), adds(15, 4352, 0));
    at += 16;
    bundle(s, at, 1, MOV_PSRL_R9, NOP, NOP);
    at += 16;
    bundle(s, at, 1, m_to_ar(36, 12), NOP, NOP);
    at += 16;
    bundle(s, at, 1, ld8_fill(16, 15), NOP, NOP);
    at += 16;
    /* Not taken on Madison; the model used to branch to at + 32. */
    bundle(s, at, 1, NOP, chk_s_i(16, 2), NOP);
    at += 16;
    bundle(s, at, 1, st8(17, 23), NOP, NOP);
    at += 16;
    bundle(s, at, 1, st8(18, 23), NOP, NOP);
    at += 16;
    bundle(s, at, 0x11, NOP, NOP, branch(0));
    run_to_marker(s);
    g_assert_cmphex(qtest_readq(s, 0x1000) & 0xffff, ==, 1);
    g_assert_cmphex((qtest_readq(s, 0x1000) >> 41) & 3, ==, 1);
    /* The interruption came with psr.ic clear, so ISR.ni is set. */
    g_assert_cmphex((qtest_readq(s, 0x1000) >> 39) & 1, ==, 1);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 2);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x33);
    qtest_quit(s);
}

/* M1 ld8 forms: x6 0x0b ld8.a, 0x27 ld8.c.nc, 0x23 ld8.c.clr. */
static uint64_t ld8_x(unsigned x6, unsigned dest, unsigned base)
{
    return (UINT64_C(4) << 37) | ((uint64_t)x6 << 30) |
           ((uint64_t)base << 20) | (dest << 6);
}

/* M24 invala (opcode 0, x4 0, x2 1) and M26 invala.e r1 (x4 2, x2 1). */
#define INVALA (UINT64_C(1) << 31)
static uint64_t invala_e(unsigned r1)
{
    return (UINT64_C(1) << 31) | (UINT64_C(2) << 27) | ((uint64_t)r1 << 6);
}

/* M6 ldfd.a f1 = [r3] and M27 invala.e f1. */
static uint64_t ldfd_a(unsigned f1, unsigned r3)
{
    return UINT64_C(0xc2c0000000) | ((uint64_t)r3 << 20) | ((uint64_t)f1 << 6);
}

static uint64_t invala_e_f(unsigned f1)
{
    return (UINT64_C(1) << 31) | (UINT64_C(3) << 27) | ((uint64_t)f1 << 6);
}

/* M22 chk.a.nc (x3 4) and chk.a.clr (x3 5) r1, with a target 4 bundles on. */
static uint64_t chk_a(unsigned x3, unsigned r1)
{
    return ((uint64_t)x3 << 33) | (UINT64_C(4) << 13) | ((uint64_t)r1 << 6);
}

/*
 * Run insns, then a final check: the handler marker (0x1000) means the entry
 * was missing, the fall-through one (0x1008) found (SDM Vol. 1 4.4.5.3-5).
 */
static bool alat_check_hits_x(const uint64_t *insns, unsigned n,
                              uint64_t last, bool fuse_pairs)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    bool hit;

    bundle(s, 0xd700, 1, st8(11, 19), NOP, NOP);
    bundle(s, 0xd710, 0x11, NOP, NOP, branch(0));
    qtest_writeq(s, 0x1100, 0x11);
    bundle(s, at, 1, adds(8, 0x1000, 0), adds(5, 0x1100, 0), adds(11, 4096, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(12, 4104, 0), adds(9, 0x22, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(19, 0x42, 0), adds(6, 0x1108, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), NOP, NOP);
    at += 16;
    step(s, &at, to_cr(2, 8));
    for (unsigned i = 0; i < n; i++) {
        if (fuse_pairs && i + 1 < n && i % 2 == 0) {
            bundle(s, at, 0x09, insns[i], insns[i + 1], NOP);
            at += 16;
            i++;
        } else {
            step(s, &at, insns[i]);
        }
    }
    step(s, &at, last);
    bundle(s, at, 1, st8(12, 19), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    for (int i = 0; i < 10000 && !qtest_readq(s, 0x1000) &&
         !qtest_readq(s, 0x1008); i++) {
        g_usleep(1000);
    }
    hit = qtest_readq(s, 0x1008) == 0x42;
    g_assert_true(hit || qtest_readq(s, 0x1000) == 0x42);
    qtest_quit(s);
    return hit;
}

/*
 * A store taking a Data Nested TLB fault must close the ALAT write it opened,
 * or the handler's own faulting store trips "write already active".
 */
static void test_alat_store_fault_does_not_wedge_the_next_store(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    g_autofree char *registers = NULL;
    uint64_t at = 0x2000;

    /* IVA = 0x8000; the vector at +0x1400 stores through the same VA. */
    bundle(s, 0x9400, 1, st8(11, 12), NOP, NOP);
    bundle(s, 0x9410, 0x11, NOP, NOP, branch(0));
    bundle(s, at, 1, adds(8, 0x1000, 0), adds(9, 0x1000, 0), adds(11, 4096, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(12, 0x42, 0), adds(5, 0x2000, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), add(5, 5, 5), NOP);
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), add(5, 5, 5), NOP);
    at += 16;
    step(s, &at, to_cr(2, 8));
    /* psr.l = 1 << 17 (dt), then the faulting store through an unmapped VA. */
    step(s, &at, adds(9, 1, 0));
    for (unsigned i = 0; i < 17; i++) {
        step(s, &at, add(9, 9, 9));
    }
    step(s, &at, MOV_PSRL_R9);
    step(s, &at, st8(5, 12));
    bundle(s, at, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    for (int64_t end = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;;) {
        g_free(registers);
        registers = qtest_hmp(s, "info registers");
        if (strstr(registers, "IP: 0x0000000000009400") ||
            g_get_monotonic_time() >= end) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_nonnull(strstr(registers, "IP: 0x0000000000009400"));
    qtest_quit(s);
}

/*
 * Under -icount an I/O store restarts in a new block; with the ALAT on, the
 * restart must not begin a second ALAT write.
 */
static void test_alat_io_store_under_icount(void)
{
    g_autofree char *flash = NULL;
    g_autofree char *args = NULL;
    g_autoptr(GError) err = NULL;
    uint64_t at = 0x2000;
    QTestState *s;
    int fd = g_file_open_tmp("ia64-io-XXXXXX.bin", &flash, &err);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 6 * 1024 * 1024), ==, 0);
    close(fd);
    args = g_strdup_printf("-machine altix350 -cpu madison-1600-3m -bios %s "
                           "-accel tcg -S -icount shift=0,align=off,sleep=off "
                           "-device loader,addr=0x2000,cpu-num=0", flash);
    s = qtest_init(args);
    /* SHub RTC control and counter, the PROM's clock set-up stores. */
    qtest_writeq(s, 0x1000, 0x8000008000000000ULL + 0x110060280ULL);
    qtest_writeq(s, 0x1008, 0x100000000ULL);
    qtest_writeq(s, 0x1010, 0x8000008000000000ULL + 0x1101c0000ULL);
    qtest_writeq(s, 0x1018, 0);
    step(s, &at, adds(5, 0x1000, 0));
    step(s, &at, adds(6, 0x1008, 0));
    step(s, &at, adds(7, 0x1010, 0));
    step(s, &at, adds(8, 0x1018, 0));
    step(s, &at, ld8(10, 5));
    step(s, &at, ld8(11, 6));
    step(s, &at, ld8(12, 7));
    step(s, &at, ld8(13, 8));
    step(s, &at, st8(10, 11));
    step(s, &at, st8(12, 13));
    step(s, &at, st8(10, 11));
    step(s, &at, adds(14, 0x42, 0));
    step(s, &at, adds(15, 0x1100, 0));
    step(s, &at, st8(15, 14));
    bundle(s, at, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1100);
    qtest_quit(s);
    unlink(flash);
}

static bool alat_check_hits(const uint64_t *insns, unsigned n, uint64_t last)
{
    return alat_check_hits_x(insns, n, last, false);
}

/*
 * A store matches an ALAT entry on bits 19:0 of its physical address (Itanium 2
 * manual 5.1): 1 MiB away still removes it, 512 KiB away does not.
 */
static void test_alat_store_compares_20_address_bits(void)
{
    uint64_t far_store[2 + 20 + 1 + 1];
    uint64_t near_store[2 + 19 + 1 + 1];
    unsigned n = 0;

    far_store[n++] = ld8_x(0x0b, 10, 5);
    far_store[n++] = adds(7, 1, 0);
    for (unsigned i = 0; i < 20; i++) {
        far_store[n++] = add(7, 7, 7);
    }
    far_store[n++] = add(7, 7, 5);
    far_store[n++] = st8(7, 9);
    g_assert_cmpuint(n, ==, ARRAY_SIZE(far_store));
    g_assert_false(alat_check_hits(far_store, n, chk_a(4, 10)));

    n = 0;
    near_store[n++] = ld8_x(0x0b, 10, 5);
    near_store[n++] = adds(7, 1, 0);
    for (unsigned i = 0; i < 19; i++) {
        near_store[n++] = add(7, 7, 7);
    }
    near_store[n++] = add(7, 7, 5);
    near_store[n++] = st8(7, 9);
    g_assert_cmpuint(n, ==, ARRAY_SIZE(near_store));
    g_assert_true(alat_check_hits(near_store, n, chk_a(4, 10)));
}

static void test_alat_checks(void)
{
    const uint64_t ld_a[] = { ld8_x(0x0b, 10, 5) };
    const uint64_t ld_st[] = { ld8_x(0x0b, 10, 5), st8(5, 9) };
    const uint64_t ld_inv[] = { ld8_x(0x0b, 10, 5), INVALA };
    const uint64_t ld_inve[] = { ld8_x(0x0b, 10, 5), invala_e(10) };
    const uint64_t ld_clr[] = { ld8_x(0x0b, 10, 5), chk_a(5, 10) };
    const uint64_t ld_two[] = { ld8_x(0x0b, 10, 5), ld8_x(0x0b, 11, 5),
                                st8(5, 9), ld8_x(0x0b, 10, 5) };

    /* A found entry falls through; so does a store elsewhere. */
    g_assert_true(alat_check_hits(ld_a, 1, chk_a(4, 10)));
    /* An overlapping store, invala, invala.e and no load at all: missing. */
    g_assert_false(alat_check_hits(ld_st, 2, chk_a(4, 10)));
    g_assert_false(alat_check_hits(ld_inv, 2, chk_a(4, 10)));
    g_assert_false(alat_check_hits(ld_inve, 2, chk_a(4, 10)));
    g_assert_false(alat_check_hits(NULL, 0, chk_a(4, 14)));
    /* Integer and floating-point entries are separate (PAL_B 5.73 0xffef9710). */
    {
        const uint64_t both_inv_i[] = { ld8_x(0x0b, 10, 5), ldfd_a(10, 5),
                                        invala_e(10) };
        const uint64_t both_inv_f[] = { ld8_x(0x0b, 10, 5), ldfd_a(10, 5),
                                        invala_e_f(10) };

        g_assert_true(alat_check_hits(both_inv_i, 3, chk_a(6, 10)));
        g_assert_true(alat_check_hits(both_inv_f, 3, chk_a(4, 10)));
        g_assert_false(alat_check_hits(both_inv_f, 3, chk_a(6, 10)));
        g_assert_false(alat_check_hits(both_inv_i, 3, chk_a(4, 10)));
    }
    /* A full table replaces an entry instead of dropping the new one. */
    {
        uint64_t full[33];

        for (unsigned f = 0; f < 32; f++) {
            full[f] = ldfd_a(f + 2, 5);
        }
        full[32] = ld8_x(0x0b, 10, 5);
        g_assert_true(alat_check_hits(full, 33, chk_a(4, 10)));
        /* The oldest was replaced; a later one is still there. */
        g_assert_false(alat_check_hits(full, 33, chk_a(6, 2)));
        g_assert_true(alat_check_hits(full, 33, chk_a(6, 3)));
    }
    /* The PAL's two loads share one bundle, the fp one at the next address. */
    {
        const uint64_t pair[] = { ld8_x(0x0b, 10, 5), ldfd_a(32, 6),
                                  invala_e(10) };

        g_assert_true(alat_check_hits_x(pair, 3, chk_a(6, 32), true));
    }
    /* An empty ALAT misses on every register, r0 and r1 included. */
    for (unsigned r = 0; r < 32; r += 15) {
        g_assert_false(alat_check_hits(NULL, 0, chk_a(4, r)));
    }
    /* Twenty-four entries at one address all hit (the rest of r2-r31 hold
     * the test's own pointers). */
    {
        uint64_t many[24];
        unsigned n = 0;

        for (unsigned r = 2; r < 32; r++) {
            if (r != 5 && r != 8 && r != 9 && r != 11 && r != 12 && r != 19) {
                many[n++] = ld8_x(0x0b, r, 5);
            }
        }
        g_assert_cmpuint(n, ==, 24);
        g_assert_true(alat_check_hits(many, n, chk_a(4, 31)));
        g_assert_true(alat_check_hits(many, n, chk_a(4, 2)));
    }
    /* chk.a.clr hit removes the entry; a reload after the store allocates. */
    g_assert_false(alat_check_hits(ld_clr, 2, chk_a(4, 10)));
    g_assert_true(alat_check_hits(ld_two, 4, chk_a(4, 10)));
}

/* M42/M43 indirect forms: mov pmc/pmd[r3] = r2 (x6 4, 5), mov r1 = pmc/pmd[r3]. */
static uint64_t mov_to_pm(bool pmd, unsigned index_reg, unsigned src)
{
    return (UINT64_C(1) << 37) | ((uint64_t)(pmd ? 5 : 4) << 27) |
           ((uint64_t)index_reg << 20) | ((uint64_t)src << 13);
}

static uint64_t mov_from_pm(bool pmd, unsigned dest, unsigned index_reg)
{
    return (UINT64_C(1) << 37) | ((uint64_t)(pmd ? 0x15 : 0x14) << 27) |
           ((uint64_t)index_reg << 20) | ((uint64_t)dest << 6);
}

/* What the Madison PMU registers read back after w is written. */
static uint64_t madison_pm_readback(bool pmd, unsigned i, uint64_t w)
{
    static const uint64_t pmc_mask[16] = {
        0xf1, 0, 0, 0, 0x0fffff7f, 0x0f7fff7f, 0x0f7fff7f, 0x0f7fff7f,
        0xffffffff3fffffffULL, 0xffffffff3ffffffcULL, UINT64_MAX, 0x3ffffff,
        0xffcf, UINT64_MAX, UINT64_MAX, UINT64_MAX
    };

    if (!pmd) {
        return w & pmc_mask[i];
    }
    switch (i) {
    case 1:
        return w & 0x1fff;
    case 3:
        return w & 0xffff;
    case 4 ... 7:
        w &= (UINT64_C(1) << 47) - 1;
        return (w ^ (UINT64_C(1) << 46)) - (UINT64_C(1) << 46);
    case 16:
        return w & 0xfffffffffULL;
    default:
        return w;
    }
}

/*
 * The PAL's register test (PAL_B 5.73, 0xffefdc80) writes one pattern to the
 * PMCs and PMDs and reads it back; masks from 251110-003 ch. 10 and the test.
 */
static void test_madison_pmu_readback(void)
{
    static const uint64_t patterns[] = {
        0, UINT64_MAX, 0x5555555555555555ULL, 0x2aaa2aaa2aaa2aaaULL
    };

    for (unsigned k = 0; k < G_N_ELEMENTS(patterns); k++) {
        QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                                  "-m 16M -accel tcg -S "
                                  "-device loader,addr=0x8000,cpu-num=0");
        uint64_t at = 0x8000;
        unsigned n = 0;

        step(s, &at, adds(5, 0x1800, 0));
        step(s, &at, adds(10, 4096, 0));
        for (unsigned pmd = 0; pmd < 2; pmd++) {
            for (unsigned i = 0; i < (pmd ? 18u : 16u); i++, n++) {
                uint64_t w = patterns[k];

                if (!pmd) {
                    w |= i == 0 ? 1 : i == 4 ? UINT64_C(1) << 23 : 0;
                }
                qtest_writeq(s, 0x1800 + 8 * n, w);
                step(s, &at, adds(7, i, 0));
                bundle(s, at, 1, ld8(6, 5), adds(5, 8, 5), NOP);
                at += 16;
                step(s, &at, mov_to_pm(pmd, 7, 6));
                step(s, &at, mov_from_pm(pmd, 8, 7));
                bundle(s, at, 1, st8(10, 8), adds(10, 8, 10), NOP);
                at += 16;
            }
        }
        bundle(s, at, 1, adds(13, 0x42, 0), adds(6, 0x1400, 0), NOP);
        at += 16;
        bundle(s, at, 1, st8(6, 13), NOP, NOP);
        bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
        run_to_marker_at(s, 0x1400);
        n = 0;
        for (unsigned pmd = 0; pmd < 2; pmd++) {
            for (unsigned i = 0; i < (pmd ? 18u : 16u); i++, n++) {
                g_assert_cmphex(qtest_readq(s, 0x1000 + 8 * n), ==,
                                madison_pm_readback(pmd, i,
                                    qtest_readq(s, 0x1800 + 8 * n)));
            }
        }
        qtest_quit(s);
    }
}

/*
 * Cache-array port ("Cache-array port" in the machine page): the reserved word
 * 0x108000000 runs the MSR 1546 command; steps follow PAL_B 5.73 at 0xffe8f300.
 */
#define CA_WRITE_ARRAY9 0x8d20001c00000000ULL
#define CA_READ_ARRAY8  0x8900001c60000000ULL
#define CA_WRITE_ARRAY8 0x8d00001c60000000ULL
#define CA_READ_ARRAY9  0x8920001c60000000ULL
#define CA_LINE 0x1c000ULL     /* MSR 1544 index 0x380 */
#define CA_INDEX 0x380ULL

typedef struct CaProgram {
    QTestState *s;
    uint64_t at;
    uint64_t slot;      /* next value-table entry at 0x1800 */
    uint64_t out;       /* next result at 0x1000 */
} CaProgram;

static void ca_msr_set(CaProgram *p, unsigned msr, uint64_t value)
{
    qtest_writeq(p->s, 0x1800 + 8 * p->slot, value);
    step(p->s, &p->at, adds(5, 0x1800 + 8 * p->slot, 0));
    step(p->s, &p->at, ld8(6, 5));
    step(p->s, &p->at, adds(7, msr, 0));
    step(p->s, &p->at, to_msr(7, 6));
    p->slot++;
}

static void ca_stage_clear(CaProgram *p)
{
    for (unsigned j = 0; j < 16; j++) {
        step(p->s, &p->at, adds(7, 1600 + 8 * (j >> 2) + (j & 3), 0));
        step(p->s, &p->at, to_msr(7, 0));
    }
}

static void ca_mword(CaProgram *p)
{
    bundle(p->s, p->at, 0x0b, NOP, 0x108000000ULL, NOP);
    p->at += 16;
}

static void ca_read_msr(CaProgram *p, unsigned msr)
{
    step(p->s, &p->at, adds(7, msr, 0));
    step(p->s, &p->at, from_msr(12, 7));
    bundle(p->s, p->at, 1, st8(10, 12), adds(10, 8, 10), NOP);
    p->at += 16;
}

/* One iteration of the PAL's loop for dword j with data and check-bit mask. */
static void ca_inject_and_store(CaProgram *p, unsigned j, uint64_t data,
                                uint64_t mask)
{
    uint64_t payload = ((mask & 0x7f) << 13) | ((mask >> 7) << 45);

    ca_stage_clear(p);
    ca_msr_set(p, 1546, CA_WRITE_ARRAY9);
    if (mask) {
        ca_msr_set(p, 1600 + (j & 3), payload);
    }
    ca_mword(p);
    ca_msr_set(p, 1544, CA_INDEX);
    ca_msr_set(p, 1546, CA_READ_ARRAY8);
    ca_mword(p);
    ca_msr_set(p, 1600 + 8 * (j >> 2) + (j & 3), data);
    ca_msr_set(p, 1546, CA_WRITE_ARRAY8);
    ca_mword(p);
    ca_stage_clear(p);
    ca_msr_set(p, 1546, CA_WRITE_ARRAY9);
    ca_mword(p);
}

static void ca_begin(CaProgram *p, const char *extra)
{
    g_autofree char *args = g_strdup_printf(
        "-machine none -cpu madison-1600-3m%s -m 16M -accel tcg -S "
        "-device loader,addr=0x2000,cpu-num=0", extra);

    p->s = qtest_init(args);
    p->at = 0x2000;
    p->slot = 0;
    step(p->s, &p->at, adds(10, 4096, 0));
}

static uint64_t ca_finish(CaProgram *p, unsigned results, uint64_t *out)
{
    bundle(p->s, p->at, 1, adds(13, 0x42, 0), adds(6, 0x1400, 0), NOP);
    p->at += 16;
    bundle(p->s, p->at, 1, st8(6, 13), NOP, NOP);
    bundle(p->s, p->at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(p->s, 0x1400);
    for (unsigned i = 0; i < results; i++) {
        out[i] = qtest_readq(p->s, 0x1000 + 8 * i);
    }
    qtest_quit(p->s);
    return 0;
}

/* After the PAL's sequence a load reports MSR 1505 from the check bits. */
static uint64_t ca_run_log(unsigned j, uint64_t data, uint64_t mask, bool load)
{
    CaProgram p;
    uint64_t out[1];

    ca_begin(&p, "");
    ca_inject_and_store(&p, j, data, mask);
    if (load) {
        qtest_writeq(p.s, 0x1c00, CA_LINE + 8 * j);
        step(p.s, &p.at, adds(5, 0x1c00, 0));
        step(p.s, &p.at, ld8(5, 5));
        step(p.s, &p.at, ld8(11, 5));
    }
    ca_read_msr(&p, 1505);
    ca_finish(&p, 1, out);
    return out[0];
}

static void test_madison_cache_array_log(void)
{
    /* No injection: nothing is logged. */
    g_assert_cmphex(ca_run_log(3, 0x1234, 0, true), ==, 0);
    /* One flipped check bit, in either half, and for another dword. */
    g_assert_cmphex(ca_run_log(3, 0x1234, 1, true), ==, 0x100001);
    g_assert_cmphex(ca_run_log(3, 0x1234, 0x40, true), ==, 0x100001);
    g_assert_cmphex(ca_run_log(3, 0x1234, 0x80, true), ==, 0x100001);
    g_assert_cmphex(ca_run_log(2, 0x1234, 0x2000, true), ==, 0x100001);
    /* Two adjacent bits, also across the halves (bits 6 and 7). */
    g_assert_cmphex(ca_run_log(3, 0x1234, 3, true), ==, 0x80001);
    g_assert_cmphex(ca_run_log(3, 0x1234, 0xc0, true), ==, 0x80001);
    /* The error shows only on the load. */
    g_assert_cmphex(ca_run_log(3, 0x1234, 1, false), ==, 0);
}

/* Reading array 9 back returns the Hsiao check bits the data write computed. */
static void test_madison_cache_array_check_bits(void)
{
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    ca_inject_and_store(&p, 5, 1, 0);          /* data32 bit 0: check 0b0000111 */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1546, CA_READ_ARRAY9);
    ca_mword(&p);
    ca_read_msr(&p, 1600 + 8 * 1 + 1);         /* dword 5 */
    ca_inject_and_store(&p, 5, 1ULL << 32, 0); /* the same bit in the high half */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1546, CA_READ_ARRAY9);
    ca_mword(&p);
    ca_read_msr(&p, 1600 + 8 * 1 + 1);
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 7ULL << 13);
    g_assert_cmphex(out[1], ==, 7ULL << 45);
}

/*
 * A line the port does not hold reads as the RAM contents of the latest line
 * stored to its set (the PAL's tests store, then read the arrays back).
 */
static void test_madison_cache_array_reads_stored_lines(void)
{
    CaProgram p;
    uint64_t out[5];

    ca_begin(&p, "");
    /* One array command first, so that the port starts watching stores. */
    ca_msr_set(&p, 1546, CA_READ_ARRAY9);
    ca_mword(&p);
    /* Store 0x4444... to the line 0x1c000 (set 0x380) and 0x2222... to 0x20000. */
    qtest_writeq(p.s, 0x1c10, 0x1c000);
    qtest_writeq(p.s, 0x1c18, 0x4444444444444444ULL);
    qtest_writeq(p.s, 0x1c20, 0x20000);
    qtest_writeq(p.s, 0x1c28, 0x2222222222222222ULL);
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, adds(5, 0x1c18, 0));
    step(p.s, &p.at, ld8(9, 5));
    step(p.s, &p.at, st8(8, 9));
    step(p.s, &p.at, adds(5, 0x1c20, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, adds(5, 0x1c28, 0));
    step(p.s, &p.at, ld8(9, 5));
    step(p.s, &p.at, st8(8, 9));
    /* Read data of set 0x380 with way 1 (bit 13): the stored line. */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, CA_INDEX | (1 << 13));
    ca_msr_set(&p, 1546, CA_READ_ARRAY8);
    ca_mword(&p);
    ca_read_msr(&p, 1600);
    /* Its check bits are non-zero: the Hsiao code of the data. */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1546, CA_READ_ARRAY9);
    ca_mword(&p);
    ca_read_msr(&p, 1600);
    /* A set nothing was stored to reads zeros. */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, 0x77);
    ca_msr_set(&p, 1546, CA_READ_ARRAY8);
    ca_mword(&p);
    ca_read_msr(&p, 1600);
    /* A tag write at the stored line's index does not hide its data. */
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, 0x400);
    ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
    ca_mword(&p);
    ca_msr_set(&p, 1546, CA_READ_ARRAY8);
    ca_mword(&p);
    ca_read_msr(&p, 1600);
    ca_finish(&p, 4, out);
    g_assert_cmphex(out[0], ==, 0x4444444444444444ULL);
    g_assert_cmpuint(out[1] & ((0x7fULL << 13) | (0x7fULL << 45)), !=, 0);
    g_assert_cmphex(out[2], ==, 0);
    g_assert_cmphex(out[3], ==, 0x2222222222222222ULL);
}

/*
 * Tag array: the write word holds tag bit-reversed 34:0, valid 35, parity 39:36
 * and state 43:40; MSR 1545 reads tag 53:19, parity 8:5, state 3:0.
 */
static void test_madison_cache_array_tag(void)
{
    const uint64_t tag = 0x123456789ULL;
    uint64_t word = 1ULL << 35 | 0x5ULL << 36 | 1ULL << 40;
    CaProgram p;
    uint64_t out[1];

    for (unsigned i = 0; i < 35; i++) {
        word |= (tag >> i & 1) << (34 - i);
    }
    ca_begin(&p, "");
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, 0x2105);
    ca_msr_set(&p, 1600, word);
    ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
    ca_mword(&p);
    ca_msr_set(&p, 1546, 0x8940000000000000ULL);
    ca_mword(&p);
    ca_read_msr(&p, 1545);
    ca_finish(&p, 1, out);
    g_assert_cmphex(out[0], ==, tag << 19 | 0x5ULL << 5 | 1);
}

/*
 * A load hitting a valid tagged line returns its data, not RAM's; a store sets
 * state 7 and marks its column in the replacement mask (sub-tests 5 and 8).
 */
static void test_madison_cache_array_tag_hit(void)
{
    const uint64_t tag = 9;     /* address 0x48280 sits in set 0x105 */
    uint64_t word = 1ULL << 35 | 1ULL << 36 | 0xaULL << 40;
    CaProgram p;
    uint64_t out[4];

    for (unsigned i = 0; i < 35; i++) {
        word |= (tag >> i & 1) << (34 - i);
    }
    ca_begin(&p, "");
    qtest_writeq(p.s, 0x48288, 0x1111);
    qtest_writeq(p.s, 0x1c10, 0x48288);
    qtest_writeq(p.s, 0x1c18, 0x2222);
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, 0x2105);
    ca_msr_set(&p, 1601, 0xdeadbeefULL);
    ca_msr_set(&p, 1546, CA_WRITE_ARRAY8);
    ca_mword(&p);
    ca_msr_set(&p, 1600, word);
    ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
    ca_mword(&p);
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    step(p.s, &p.at, adds(5, 0x1c18, 0));
    step(p.s, &p.at, ld8(11, 5));
    step(p.s, &p.at, st8(8, 11));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    /* The tag read shows the state and column; the data read the new dword. */
    ca_msr_set(&p, 1546, 0x8940000000000000ULL);
    ca_mword(&p);
    ca_read_msr(&p, 1545);
    ca_msr_set(&p, 1546, CA_READ_ARRAY8);
    ca_mword(&p);
    ca_read_msr(&p, 1601);
    ca_finish(&p, 4, out);
    g_assert_cmphex(out[0], ==, 0xdeadbeefULL);
    g_assert_cmphex(out[1], ==, 0x2222);
    g_assert_cmphex(out[2], ==, tag << 19 | 2 << 9 | 1 << 5 | 7);
    g_assert_cmphex(out[3], ==, 0x2222);
}

/*
 * fc drops the line's data, so a load after it reads the RAM that firmware
 * or DMA rewrote; fc.i may but need not (SDM), and the model leaves the line.
 */
static void ca_fc_case(bool fc_i, uint64_t expect)
{
    const uint64_t tag = 9;
    uint64_t word = 1ULL << 35 | 1ULL << 36 | 0xaULL << 40;
    CaProgram p;
    uint64_t out[2];

    for (unsigned i = 0; i < 35; i++) {
        word |= (tag >> i & 1) << (34 - i);
    }
    ca_begin(&p, "");
    qtest_writeq(p.s, 0x48288, 0x1111);
    qtest_writeq(p.s, 0x1c10, 0x48288);
    ca_stage_clear(&p);
    ca_msr_set(&p, 1544, 0x2105);
    ca_msr_set(&p, 1601, 0xdeadbeefULL);
    ca_msr_set(&p, 1546, CA_WRITE_ARRAY8);
    ca_mword(&p);
    ca_msr_set(&p, 1600, word);
    ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
    ca_mword(&p);
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    step(p.s, &p.at, (UINT64_C(1) << 37) | (fc_i ? UINT64_C(1) << 36 : 0) |
                     (UINT64_C(0x30) << 27) | (8 << 20));       /* fc[.i] r8 */
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 0xdeadbeefULL);
    g_assert_cmphex(out[1], ==, expect);
}

static void test_madison_cache_array_fc_drops_line_data(void)
{
    ca_fc_case(false, 0x1111);
}

static void test_madison_cache_array_fc_i_keeps_line_data(void)
{
    ca_fc_case(true, 0xdeadbeefULL);
}

/* fc drops every column holding the address's tag, not only the first. */
static void test_madison_cache_array_fc_drops_every_column(void)
{
    const uint64_t tag = 9;
    const uint64_t index[2] = { 0x005, 0x105 };
    const uint64_t data[2] = { 0xdeadbeef, 0xcafe };
    uint64_t word = 1ULL << 35 | 1ULL << 36 | 0xaULL << 40;
    CaProgram p;
    uint64_t out[2];

    for (unsigned i = 0; i < 35; i++) {
        word |= (tag >> i & 1) << (34 - i);
    }
    ca_begin(&p, "");
    qtest_writeq(p.s, 0x48288, 0x1111);
    qtest_writeq(p.s, 0x1c10, 0x48288);
    for (unsigned col = 0; col < 2; col++) {
        ca_stage_clear(&p);
        ca_msr_set(&p, 1544, index[col]);
        ca_msr_set(&p, 1601, data[col]);
        ca_msr_set(&p, 1546, CA_WRITE_ARRAY8);
        ca_mword(&p);
        ca_msr_set(&p, 1600, word);
        ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
        ca_mword(&p);
    }
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    step(p.s, &p.at, (UINT64_C(1) << 37) | (UINT64_C(0x30) << 27) |
                     (8 << 20));                                    /* fc r8 */
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 0xdeadbeefULL);
    g_assert_cmphex(out[1], ==, 0x1111);
}

/* fc drops a second-level line's data too; @ways ways hold the same tag. */
static void l3_fc_case(unsigned ways)
{
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    qtest_writeq(p.s, 0x3000, 0x1111);
    qtest_writeq(p.s, 0x1c10, 0x3000);
    for (unsigned way = 0; way < ways; way++) {
        ca_msr_set(&p, 1544, 0x60 | way << 11);
        ca_msr_set(&p, 1600, way ? 0xcafe : 0xdeadbeefULL);
        ca_msr_set(&p, 1546, 0x8c980052c0000000ULL); /* array 4, code made */
        ca_mword(&p);
        ca_msr_set(&p, 1545, 3);                     /* tag 0, state 3 */
        ca_msr_set(&p, 1546, 0x8cc0001800000000ULL); /* array 6 */
        ca_mword(&p);
    }
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    step(p.s, &p.at, (UINT64_C(1) << 37) | (UINT64_C(0x30) << 27) |
                     (8 << 20));                                    /* fc r8 */
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 0xdeadbeefULL);
    g_assert_cmphex(out[1], ==, 0x1111);
}

static void test_madison_l3_array_fc_drops_line_data(void)
{
    l3_fc_case(1);
}

static void test_madison_l3_array_fc_drops_every_way(void)
{
    l3_fc_case(2);
}

/*
 * A load from a set holding a valid tag with bad parity logs 0x4005 in MSR 1509
 * (PAL sub-test 6, 0xfff03e80).
 */
static void test_madison_cache_array_tag_parity(void)
{
    const uint64_t word = 1ULL << 34 | 1ULL << 35;      /* tag 1, valid */
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    ca_msr_set(&p, 1544, 8);        /* the set of address 0x400 */
    ca_msr_set(&p, 1600, word);
    ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
    ca_mword(&p);
    step(p.s, &p.at, adds(5, 0x400, 0));
    step(p.s, &p.at, ld8(8, 5));
    ca_read_msr(&p, 1509);
    ca_msr_set(&p, 1600, word | 1ULL << 36);
    ca_mword(&p);
    step(p.s, &p.at, adds(5, 0x400, 0));
    step(p.s, &p.at, ld8(8, 5));
    ca_read_msr(&p, 1509);
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmphex(out[1], ==, 0x4005);
}

/*
 * Stores to the eight columns of a set mark them in the replacement mask (MSR
 * 1545 bits 16:9), which restarts after all eight (sub-test 8).
 */
static void test_madison_cache_array_replacement_mask(void)
{
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    for (unsigned c = 0; c < 8; c++) {
        uint64_t addr = 0x48280 + (c << 15);
        uint64_t tag = addr >> 15;
        uint64_t word = 1ULL << 35;

        for (unsigned i = 0; i < 35; i++) {
            word |= (tag >> i & 1) << (34 - i);
        }
        qtest_writeq(p.s, 0x1c10 + 16 * c, addr);
        qtest_writeq(p.s, 0x1c18 + 16 * c, c);
        ca_msr_set(&p, 1544, 5 | c << 8);
        ca_msr_set(&p, 1600, word);
        ca_msr_set(&p, 1546, 0x8d40000000000000ULL);
        ca_mword(&p);
        ca_msr_set(&p, 1546, CA_WRITE_ARRAY8);
        ca_mword(&p);
        step(p.s, &p.at, adds(5, 0x1c10 + 16 * c, 0));
        step(p.s, &p.at, ld8(8, 5));
        step(p.s, &p.at, adds(5, 0x1c18 + 16 * c, 0));
        step(p.s, &p.at, ld8(9, 5));
        step(p.s, &p.at, st8(8, 9));
        if (c >= 6) {
            ca_msr_set(&p, 1546, 0x8940000000000000ULL);
            ca_mword(&p);
            ca_read_msr(&p, 1545);
        }
    }
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0] >> 9 & 0xff, ==, 0x7f);
    g_assert_cmphex(out[1] >> 9 & 0xff, ==, 0);
}

/*
 * Second array family (PAL group 11, 0xfff04ff0): data array 4, tag array 6. A
 * code off by one bit logs 0x080a2000 in MSR 1558, by two bits 0x080b2000.
 */
static void test_madison_cache_array_second_level(void)
{
    const uint64_t v = 1;       /* its code is 0xd */
    CaProgram p;
    uint64_t out[6];

    ca_begin(&p, "");
    qtest_writeq(p.s, 0x1c10, 0x3000);
    ca_msr_set(&p, 1544, 0x60);
    ca_msr_set(&p, 1600, v);
    ca_msr_set(&p, 1546, 0x8c980052c0000000ULL);   /* write, code generated */
    ca_mword(&p);
    ca_msr_set(&p, 1545, 3);                       /* tag 0, state 3 */
    ca_msr_set(&p, 1546, 0x8cc0001800000000ULL);
    ca_mword(&p);
    for (unsigned i = 0; i < 3; i++) {
        if (i) {
            /* Flip one code bit, then two. */
            ca_msr_set(&p, 1604, 0xd ^ (i == 1 ? 1 : 3));
            ca_msr_set(&p, 1546, 0x8c880052c0000000ULL);    /* as given */
            ca_mword(&p);
        }
        step(p.s, &p.at, adds(5, 0x1c10, 0));
        step(p.s, &p.at, ld8(8, 5));
        step(p.s, &p.at, ld8(9, 8));
        bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
        p.at += 16;
        ca_read_msr(&p, 1558);
    }
    ca_finish(&p, 6, out);
    g_assert_cmphex(out[0], ==, v);
    g_assert_cmphex(out[1], ==, 0);
    g_assert_cmphex(out[2], ==, v);
    g_assert_cmphex(out[3], ==, 0x080a2000);
    g_assert_cmphex(out[4], ==, v);
    g_assert_cmphex(out[5], ==, 0x080b2000);
}

/*
 * A modelled cache-array line answers physical-mode loads from the first array
 * command until reset, wherever the load is fetched from.
 */
static void test_cache_array_line_is_seen_while_the_port_is_active(void)
{
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    qtest_writeq(p.s, 0x1c10, 0x3000);
    ca_msr_set(&p, 1544, 0x60);
    ca_msr_set(&p, 1600, 1);
    ca_msr_set(&p, 1546, 0x8c980052c0000000ULL);
    ca_mword(&p);
    ca_msr_set(&p, 1545, 3);
    ca_msr_set(&p, 1546, 0x8cc0001800000000ULL);
    ca_mword(&p);
    step(p.s, &p.at, adds(5, 0x1c10, 0));
    step(p.s, &p.at, ld8(8, 5));
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    bundle(p.s, p.at, 0x11, NOP, NOP, branch(0x20100 - p.at));
    p.at = 0x20100;                                 /* another address */
    step(p.s, &p.at, ld8(9, 8));
    bundle(p.s, p.at, 1, st8(10, 9), adds(10, 8, 10), NOP);
    p.at += 16;
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 1);
    g_assert_cmphex(out[1], ==, 1);
}

/*
 * MSR 1536 bits 59:48 give the L3 ways: a 3 MiB part (12 ways) sets six bits
 * (0xfc0); bits the PAL writes below stay as written.
 */
static void test_msr1536_l3_ways(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    qtest_writeq(s, 0x1800, 0x8080);
    step(s, &at, adds(10, 4096, 0));
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(6, 5));
    step(s, &at, adds(9, 1536, 0));
    step(s, &at, to_msr(9, 6));
    step(s, &at, adds(9, 1536, 0));
    read_and_store(s, &at);
    bundle(s, at, 1, adds(13, 0x42, 0), adds(10, 4352, 0), NOP);
    at += 16;
    bundle(s, at, 1, st8(10, 13), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1100);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x0fc0000000008080ULL);
    qtest_quit(s);
}

/* The ITC is the virtual clock: leave 1 ms of it before a match. */
#define ITM_MARGIN 1600000ULL

static void load_const(QTestState *s, uint64_t *at, unsigned reg,
                       uint64_t slot, uint64_t value);

/*
 * ar.itc passing cr.itm raises cr.itv, taken at IVA + 0x3000; after cr.eoi,
 * cr.ivr reads 15 (spurious) once nothing is pending (SDM vol. 2, 5.8).
 */
static void test_interval_timer_interrupt_is_delivered(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    qtest_writeq(s, 0x1800, 0x6018);    /* psr.i, psr.ic and the usual bits */
    qtest_writeq(s, 0x1808, 0x2100);
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv */
    step(s, &at, m_from_ar(10, 44));            /* r10 = ar.itc */
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
    step(s, &at, add(10, 10, 12));
    step(s, &at, to_cr(1, 10));                 /* cr.itm = ar.itc + margin */
    step(s, &at, to_cr(2, 0));                  /* cr.iva = 0 */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));                /* cr.ipsr */
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(12, 5));
    step(s, &at, to_cr(19, 12));                /* cr.iip */
    bundle(s, at, 0x11, NOP, NOP, RFI);
    bundle(s, 0x2100, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x3000, 1, from_cr(20, 65), adds(14, 0x1000, 0), NOP);
    bundle(s, 0x3010, 1, st8(14, 20), adds(15, 8, 14), NOP);
    bundle(s, 0x3020, 1, to_cr(67, 0), NOP, NOP);
    bundle(s, 0x3030, 1, from_cr(21, 65), NOP, NOP);
    bundle(s, 0x3040, 1, st8(15, 21), adds(16, 0x42, 0), adds(17, 16, 14));
    bundle(s, 0x3050, 1, st8(17, 16), NOP, NOP);
    bundle(s, 0x3060, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x40);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0xf);
    qtest_quit(s);
}

static void test_interval_timer_is_taken_at_its_tick(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t itm, taken;

    qtest_writeq(s, 0x1800, 0x6018);
    qtest_writeq(s, 0x1808, 0x2200);
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv */
    step(s, &at, adds(14, 0x1000, 0));
    step(s, &at, m_from_ar(10, 44));
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
    step(s, &at, add(10, 10, 12));
    step(s, &at, to_cr(1, 10));                 /* cr.itm = ar.itc + margin */
    bundle(s, at, 1, st8(14, 10), NOP, NOP);    /* the match, at 0x1000 */
    at += 16;
    step(s, &at, to_cr(2, 0));                  /* cr.iva = 0 */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));                /* cr.ipsr */
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(12, 5));
    step(s, &at, to_cr(19, 12));                /* cr.iip */
    bundle(s, at, 0x11, NOP, NOP, RFI);
    bundle(s, 0x2200, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x3000, 1, m_from_ar(20, 44), adds(15, 8, 14), NOP);
    bundle(s, 0x3010, 1, st8(15, 20), adds(16, 0x42, 0), adds(17, 16, 14));
    bundle(s, 0x3020, 1, st8(17, 16), NOP, NOP);
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    itm = qtest_readq(s, 0x1000);
    taken = qtest_readq(s, 0x1008);
    g_assert_cmpuint(taken, >=, itm);
    g_assert_cmpuint(taken - itm, <, 80000000);   /* within 50 ms */
    qtest_quit(s);
}

/*
 * The reserved M word 0x108000000 with no array command armed and MSR 1553
 * bits 43:42 set halts until an interrupt is pending, whatever psr.i says.
 */
static QTestState *halt_program(bool arm_timer, bool earlier_command)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    if (earlier_command) {
        /* An array command, as the PAL's self-test ran, leaves MSR 1546 set. */
        qtest_writeq(s, 0x1c00, 0x8900001c60000000ULL);
        step(s, &at, adds(5, 0x1c00, 0));
        step(s, &at, ld8(8, 5));
        step(s, &at, adds(7, 1546, 0));
        step(s, &at, to_msr(7, 8));
        bundle(s, at, 0x0b, NOP, 0x108000000ULL, NOP);
        at += 16;
    }
    step(s, &at, adds(14, 0x1000, 0));
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv = 0x40 */
    step(s, &at, adds(9, 3, 0));
    for (unsigned i = 0; i < 42; i++) {
        step(s, &at, add(9, 9, 9));
    }
    step(s, &at, adds(7, 1553, 0));
    step(s, &at, to_msr(7, 9));                 /* msr[1553] bits 43:42 */
    step(s, &at, m_from_ar(10, 44));
    bundle(s, at, 1, st8(14, 10), adds(15, 8, 14), NOP);
    at += 16;
    if (arm_timer) {
        load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
        step(s, &at, add(10, 10, 12));
        step(s, &at, to_cr(1, 10));             /* cr.itm = ar.itc + margin */
    }
    bundle(s, at, 0x0b, NOP, 0x108000000ULL, NOP);
    at += 16;
    step(s, &at, m_from_ar(11, 44));
    bundle(s, at, 1, st8(15, 11), adds(16, 0x42, 0), adds(17, 16, 14));
    bundle(s, at + 16, 1, st8(17, 16), NOP, NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(0));
    return s;
}

static void test_halt_word_waits_for_the_interval_timer(void)
{
    QTestState *s = halt_program(true, false);
    uint64_t before, after;

    run_to_marker_at(s, 0x1010);
    before = qtest_readq(s, 0x1000);
    after = qtest_readq(s, 0x1008);
    g_assert_cmpuint(after - before, >=, ITM_MARGIN);
    qtest_quit(s);
}

static void halt_stays_halted(bool earlier_command)
{
    QTestState *s = halt_program(false, earlier_command);
    g_autofree char *registers = NULL;

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    /* Negative wait: a halted CPU runs nothing that could show time passed. */
    g_usleep(500000);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0);
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "HALTED: 1"));
    qtest_quit(s);
}

static void test_halt_word_stays_halted_without_an_interrupt(void)
{
    halt_stays_halted(false);
}

/* PAL_HALT_LIGHT arms no command, so a command left in MSR 1546 is not one. */
static void test_halt_word_halts_after_an_earlier_array_command(void)
{
    halt_stays_halted(true);
}

/*
 * The platform assigns cr.lid bits 31:16; they read back as set, survive a
 * guest write and hold no other bits.
 */
static void test_sapic_id_is_the_platform_assignment(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m,"
                              "sapic-id=0x0100 -m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    step(s, &at, adds(14, 0x1000, 0));
    step(s, &at, from_cr(10, 64));              /* cr.lid at reset */
    step(s, &at, adds(9, 0x180, 0));
    for (unsigned i = 0; i < 16; i++) {
        step(s, &at, add(9, 9, 9));
    }
    step(s, &at, to_cr(64, 9));                 /* cr.lid = 0x0180_0000 */
    step(s, &at, from_cr(11, 64));
    bundle(s, at, 1, st8(14, 10), adds(15, 8, 14), NOP);
    bundle(s, at + 16, 1, st8(15, 11), adds(16, 0x42, 0), adds(17, 16, 14));
    bundle(s, at + 32, 1, st8(17, 16), NOP, NOP);
    bundle(s, at + 48, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x01000000);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0x01800000);
    qtest_quit(s);
}

/*
 * With cr.tpr.mic 4, cr.ivr reads vector 0x40 as 15 (spurious) and as 0x40 once
 * cr.tpr is cleared (SDM vol. 2, 5.8.3); psr.i does not matter.
 */
static void test_tpr_masks_a_pending_vector(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t code = 0x2100;

    qtest_writeq(s, 0x1800, 0x2018);    /* psr.ic set, psr.i clear */
    qtest_writeq(s, 0x1808, 0x2100);
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv = 0x40 */
    step(s, &at, to_cr(66, 9));                 /* cr.tpr: mic 4 */
    step(s, &at, m_from_ar(10, 44));
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
    step(s, &at, add(10, 10, 12));
    step(s, &at, to_cr(1, 10));                 /* cr.itm = ar.itc + margin */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(12, 5));
    step(s, &at, to_cr(19, 12));
    bundle(s, at, 0x11, NOP, NOP, RFI);
    /* Spin past the match (8000 ticks is 5000 bundles), then read ivr masked,
     * clear tpr, read it again. */
    for (unsigned i = 0; i < 5500; i++) {
        bundle(s, code, 1, NOP, NOP, NOP);
        code += 16;
    }
    bundle(s, code, 1, from_cr(20, 65), adds(14, 0x1000, 0), NOP);
    bundle(s, code + 16, 1, st8(14, 20), adds(15, 8, 14), NOP);
    bundle(s, code + 32, 1, to_cr(66, 0), NOP, NOP);
    bundle(s, code + 48, 1, UINT64_C(0x30) << 27, NOP, NOP);    /* srlz.d */
    bundle(s, code + 64, 1, from_cr(21, 65), NOP, NOP);
    bundle(s, code + 80, 1, st8(15, 21), adds(16, 0x42, 0), adds(17, 16, 14));
    bundle(s, code + 96, 1, st8(17, 16), NOP, NOP);
    bundle(s, code + 112, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0xf);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0x40);
    qtest_quit(s);
}

/* M42: mov rr[r3] = r2 (x6 0), itr.d dtr[r3] = r2 (0x0e), itr.i (0x0f). */
static uint64_t m42(unsigned x6, unsigned r3, unsigned r2)
{
    return (UINT64_C(1) << 37) | ((uint64_t)x6 << 27) | ((uint64_t)r3 << 20) |
           ((uint64_t)r2 << 13);
}

#define SRLZ_I (UINT64_C(0x31) << 27)
#define SRLZ_D (UINT64_C(0x30) << 27)

/* Load the 64-bit value at a table slot into a register through r5. */
static void load_const(QTestState *s, uint64_t *at, unsigned reg,
                       uint64_t slot, uint64_t value)
{
    qtest_writeq(s, slot, value);
    step(s, at, adds(5, slot, 0));
    step(s, at, ld8(reg, 5));
}

/*
 * Map virtual 0 to physical 0 (64 KiB) and 0x10000 to 0x1000 (4 KiB) with
 * translation registers and rfi to virtual mode; a store must reach 0x1000.
 */
static void ia64_translation_setup(QTestState *s, uint64_t *at,
                                   uint64_t iva, uint64_t rr0)
{
    /* rr0: region id 1, 4 KiB pages. */
    load_const(s, at, 9, 0x1800, rr0);
    step(s, at, m42(0, 0, 9));
    step(s, at, SRLZ_D);
    /* itr.i[0] and itr.d[0]: va 0..0xffff -> pa 0, rwx, 64 KiB. */
    load_const(s, at, 10, 0x1808, 16 << 2);
    step(s, at, to_cr(21, 10));                 /* cr.itir */
    step(s, at, to_cr(20, 0));                  /* cr.ifa = 0 */
    step(s, at, SRLZ_D);
    load_const(s, at, 11, 0x1810, (3 << 9) | 0x61);
    step(s, at, m42(0x0f, 0, 11));              /* itr.i itr[r0] */
    step(s, at, SRLZ_I);
    step(s, at, m42(0x0e, 0, 11));              /* itr.d dtr[r0] */
    step(s, at, SRLZ_D);
    /* dtr[1]: va 0x10000 -> pa 0x1000, rw, 4 KiB. */
    load_const(s, at, 10, 0x1818, 12 << 2);
    step(s, at, to_cr(21, 10));
    load_const(s, at, 12, 0x1820, 0x10000);
    step(s, at, to_cr(20, 12));
    step(s, at, SRLZ_D);
    load_const(s, at, 11, 0x1828, 0x1000 | (2 << 9) | 0x61);
    step(s, at, adds(13, 1, 0));
    step(s, at, m42(0x0e, 13, 11));             /* itr.d dtr[1] */
    step(s, at, SRLZ_D);
    load_const(s, at, 14, 0x1830, iva);
    step(s, at, to_cr(2, 14));                  /* cr.iva */
}

#define PSR_VIRTUAL ((1ULL << 13) | (1ULL << 17) | (1ULL << 27) | \
                     (1ULL << 36) | (1ULL << 44))
#define PSR_PHYSICAL ((1ULL << 13) | (1ULL << 44))

static void ia64_rfi_psr(QTestState *s, uint64_t *at, uint64_t entry,
                         uint64_t psr)
{
    /* rfi into psr: ic and bn, and dt, rt and it for virtual mode. */
    load_const(s, at, 15, 0x1838, psr);
    step(s, at, to_cr(16, 15));
    load_const(s, at, 16, 0x1840, entry);
    step(s, at, to_cr(19, 16));
    step(s, at, SRLZ_I);
    bundle(s, *at, 0x11, NOP, NOP, RFI);
}

static void ia64_rfi_virtual(QTestState *s, uint64_t *at, uint64_t entry)
{
    ia64_rfi_psr(s, at, entry, PSR_VIRTUAL);
}

/*
 * A guest kernel calls the SAL in virtual mode, through region 7 mapped to
 * the same physical address. The reserved M word must still be a no-op there.
 */
static void test_reserved_word_in_virtual_mode(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    /* rr7: region id 2, 4 KiB pages. */
    load_const(s, &at, 9, 0x1800, 0xe000000000000000ULL);
    load_const(s, &at, 10, 0x1808, (2 << 8) | (12 << 2));
    step(s, &at, m42(0, 9, 10));
    step(s, &at, SRLZ_D);
    /* itr.i[0]: va 0xe000000000000000 -> pa 0, 16 MiB, rwx. */
    load_const(s, &at, 12, 0x1810, 0xe000000000000000ULL);
    step(s, &at, to_cr(20, 12));
    load_const(s, &at, 10, 0x1818, 24 << 2);
    step(s, &at, to_cr(21, 10));
    step(s, &at, SRLZ_D);
    load_const(s, &at, 11, 0x1820, (3 << 9) | 0x61);
    step(s, &at, m42(0x0f, 0, 11));
    step(s, &at, SRLZ_I);
    /* rfi with psr.ic, psr.it and psr.bn: data stays physical. */
    load_const(s, &at, 15, 0x1828, (1ULL << 13) | (1ULL << 36) |
               (1ULL << 44));
    step(s, &at, to_cr(16, 15));
    load_const(s, &at, 16, 0x1830, 0xe000000000003000ULL);
    step(s, &at, to_cr(19, 16));
    step(s, &at, SRLZ_I);
    bundle(s, at, 0x11, NOP, NOP, RFI);
    bundle(s, 0x3000, 0x0b, NOP, 0x108000000ULL, NOP);
    bundle(s, 0x3010, 1, adds(10, 4096, 0), adds(13, 0x42, 0), NOP);
    bundle(s, 0x3020, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1000) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x42);
    qtest_quit(s);
}

static void ia64_enter_virtual_mode(QTestState *s, uint64_t *at,
                                    uint64_t entry, uint64_t iva)
{
    ia64_translation_setup(s, at, iva, (1ULL << 8) | (12 << 2));
    ia64_rfi_virtual(s, at, entry);
}

static void test_translation_registers_map_a_virtual_page(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_enter_virtual_mode(s, &at, 0x3000, 0x8000);
    /* In virtual mode: r10 = va 0x10000, store 0x42 at +0 and 7 at +8. */
    qtest_writeq(s, 0x1848, 0x10000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), adds(13, 0x42, 0), adds(14, 7, 0));
    bundle(s, 0x3020, 1, st8(10, 14), adds(15, 8, 10), NOP);
    bundle(s, 0x3030, 1, st8(15, 13), NOP, NOP);
    bundle(s, 0x3040, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 7);
    g_assert_cmphex(qtest_readq(s, 0x10000), ==, 0);
    qtest_quit(s);
}

/*
 * With rr.ve clear, a data access with no translation takes the Alternate Data
 * TLB vector at IVA + 0x1000, cr.ifa the address (SDM vol. 2, 5.5).
 */
static void test_unmapped_access_takes_the_alternate_data_tlb_vector(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_enter_virtual_mode(s, &at, 0x3000, 0x8000);
    qtest_writeq(s, 0x1848, 0x20000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), NOP, NOP);
    bundle(s, 0x3020, 1, ld8(11, 10), NOP, NOP);        /* va 0x20000 */
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    /* Alternate Data TLB vector: record cr.ifa and cr.iip, then the marker. */
    bundle(s, 0x9000, 1, from_cr(21, 20), adds(14, 0x1000, 0), NOP);
    bundle(s, 0x9010, 1, st8(14, 21), adds(15, 8, 14), NOP);
    bundle(s, 0x9020, 1, from_cr(22, 19), adds(16, 0x42, 0), NOP);
    bundle(s, 0x9030, 1, st8(15, 22), adds(17, 8, 15), NOP);
    bundle(s, 0x9040, 1, st8(17, 16), NOP, NOP);
    bundle(s, 0x9050, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x20000);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0x3020);
    qtest_quit(s);
}

/* A data fault from slot 1 reports ISR.ei = 1 (SDM Vol. 2 Table 3-7). */
static void test_data_fault_in_slot_1_sets_isr_ei(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_enter_virtual_mode(s, &at, 0x3000, 0x8000);
    qtest_writeq(s, 0x1848, 0x20000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), NOP, NOP);
    bundle(s, 0x3020, 8, NOP, ld8(11, 10), NOP);        /* slot 1, va 0x20000 */
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    /* Alternate Data TLB vector: record cr.isr, then the marker. */
    bundle(s, 0x9000, 1, from_cr(21, 17), adds(14, 0x1000, 0),
           adds(16, 0x42, 0));
    bundle(s, 0x9010, 1, st8(14, 21), adds(15, 8, 14), NOP);
    bundle(s, 0x9020, 1, st8(15, 16), NOP, NOP);
    bundle(s, 0x9030, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex((qtest_readq(s, 0x1000) >> 41) & 3, ==, 1);
    g_assert_cmphex((qtest_readq(s, 0x1000) >> 39) & 1, ==, 0);
    qtest_quit(s);
}

/*
 * With rr.ve and pta.ve set, a data TLB miss makes the walker insert the VHPT
 * entry at thash(va) if its tag matches ttag(va) (SDM vol. 2, 4.1.5).
 */
static void test_vhpt_walker_inserts_a_long_format_entry(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    qtest_writeq(s, 0x4000, 0x55);
    ia64_translation_setup(s, &at, 0, (1ULL << 8) | (12 << 2) | 1);
    /* cr.pta: base 0x8000, 2^15 bytes, long format, enabled. */
    load_const(s, &at, 9, 0x1850, 0x8000 | (15 << 2) | (1 << 8) | 1);
    step(s, &at, to_cr(8, 9));
    step(s, &at, SRLZ_D);
    load_const(s, &at, 10, 0x1858, 0x20000);
    step(s, &at, (UINT64_C(1) << 37) | (UINT64_C(0x1a) << 27) |
         (10 << 20) | (11 << 6));               /* thash r11 = r10 */
    step(s, &at, (UINT64_C(1) << 37) | (UINT64_C(0x1b) << 27) |
         (10 << 20) | (12 << 6));               /* ttag r12 = r10 */
    load_const(s, &at, 13, 0x1860, 0x4000 | (2 << 9) | 0x61);
    step(s, &at, st8(11, 13));                  /* word 0: the pte */
    step(s, &at, adds(14, 8, 11));
    step(s, &at, adds(15, 12 << 2, 0));
    step(s, &at, st8(14, 15));                  /* word 1: itir */
    step(s, &at, adds(14, 16, 11));
    step(s, &at, st8(14, 12));                  /* word 2: the tag */
    ia64_rfi_virtual(s, &at, 0x3000);
    qtest_writeq(s, 0x1868, 0x20000);
    bundle(s, 0x3000, 1, adds(5, 0x1868, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), adds(14, 0x1000, 0), adds(16, 0x42, 0));
    bundle(s, 0x3020, 1, ld8(11, 10), NOP, NOP);        /* va 0x20000 */
    bundle(s, 0x3030, 1, st8(14, 11), adds(15, 8, 14), NOP);
    bundle(s, 0x3040, 1, st8(15, 16), NOP, NOP);
    bundle(s, 0x3050, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x55);
    qtest_quit(s);
}

/*
 * ptr.d purges the overlapping data translation registers: the page then takes
 * the Alternate Data TLB vector (SDM vol. 2, 4.4.1).
 */
static void test_ptr_d_purges_a_data_translation_register(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_translation_setup(s, &at, 0x8000, (1ULL << 8) | (12 << 2));
    load_const(s, &at, 9, 0x1850, 0x10000);
    step(s, &at, adds(10, 12 << 2, 0));
    step(s, &at, m42(0x0c, 9, 10));             /* ptr.d r9, r10 */
    step(s, &at, SRLZ_D);
    ia64_rfi_virtual(s, &at, 0x3000);
    qtest_writeq(s, 0x1848, 0x10000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), NOP, NOP);
    bundle(s, 0x3020, 1, ld8(11, 10), NOP, NOP);        /* va 0x10000 */
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x9000, 1, from_cr(21, 20), adds(14, 0x1000, 0), NOP);
    bundle(s, 0x9010, 1, st8(14, 21), adds(15, 8, 14), NOP);
    bundle(s, 0x9020, 1, adds(16, 0x42, 0), NOP, NOP);
    bundle(s, 0x9030, 1, st8(15, 16), NOP, NOP);
    bundle(s, 0x9040, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x10000);
    qtest_quit(s);
}

/*
 * The Alternate Data TLB handler inserts the page with itc.d and rfi retries
 * the load, which succeeds (SDM vol. 2, 4.1.1).
 */
static void test_itc_d_in_a_handler_lets_the_load_retry(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    qtest_writeq(s, 0x4000, 0x55);
    qtest_writeq(s, 0x1870, 0x4000 | (2 << 9) | 0x61);
    ia64_enter_virtual_mode(s, &at, 0x3000, 0x8000);
    qtest_writeq(s, 0x1848, 0x20000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), adds(14, 0x1000, 0), adds(16, 0x42, 0));
    bundle(s, 0x3020, 1, ld8(11, 10), NOP, NOP);        /* faults, retries */
    bundle(s, 0x3030, 1, st8(14, 11), adds(15, 8, 14), NOP);
    bundle(s, 0x3040, 1, st8(15, 16), NOP, NOP);
    bundle(s, 0x3050, 0x11, NOP, NOP, branch(0));
    /* Alternate Data TLB handler: itir for 4 KiB, itc.d, serialize, rfi. */
    bundle(s, 0x9000, 1, adds(5, 0x1870, 0), adds(22, 12 << 2, 0), NOP);
    bundle(s, 0x9010, 1, to_cr(21, 22), NOP, NOP);
    bundle(s, 0x9020, 1, ld8(23, 5), NOP, NOP);
    /* itc.d ends its instruction group: template M;MI has a stop after it. */
    bundle(s, 0x9030, 0x0a, m42(0x2e, 0, 23), NOP, NOP);    /* itc.d r23 */
    bundle(s, 0x9040, 1, SRLZ_D, NOP, NOP);
    bundle(s, 0x9050, 0x11, NOP, NOP, RFI);
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x55);
    qtest_quit(s);
}

/*
 * An itc.d entry is used by a store unless ptc.e or a covering ptc.l ran first;
 * a ptc.l of another page leaves it (SDM vol. 2, 4.4.1).
 */
enum { PURGE_NONE, PURGE_PTC_E, PURGE_PTC_L_PAGE, PURGE_PTC_L_OTHER };

static void run_tc_purge(unsigned how, uint64_t *mapped, uint64_t *missed)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_translation_setup(s, &at, 0x8000, (1ULL << 8) | (12 << 2));
    load_const(s, &at, 12, 0x1850, 0x30000);
    step(s, &at, to_cr(20, 12));                /* cr.ifa */
    step(s, &at, adds(10, 12 << 2, 0));
    step(s, &at, to_cr(21, 10));                /* cr.itir */
    step(s, &at, SRLZ_D);
    load_const(s, &at, 11, 0x1858, 0x4000 | (2 << 9) | 0x61);
    bundle(s, at, 0x0a, m42(0x2e, 0, 11), NOP, NOP);    /* itc.d */
    at += 16;
    step(s, &at, SRLZ_D);
    if (how == PURGE_PTC_E) {
        step(s, &at, adds(9, 0, 0));
        step(s, &at, m42(0x34, 9, 0));          /* ptc.e r9 */
    } else if (how != PURGE_NONE) {
        load_const(s, &at, 9, 0x1860,
                   how == PURGE_PTC_L_PAGE ? 0x30000 : 0x20000);
        step(s, &at, m42(0x09, 9, 10));         /* ptc.l r9, r10 */
    }
    step(s, &at, SRLZ_D);
    ia64_rfi_virtual(s, &at, 0x3000);
    qtest_writeq(s, 0x1848, 0x30000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), adds(13, 0x42, 0), adds(14, 0x1008, 0));
    bundle(s, 0x3020, 1, st8(10, 13), NOP, NOP);        /* va 0x30000 */
    bundle(s, 0x3030, 1, st8(14, 13), NOP, NOP);
    bundle(s, 0x3040, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x9000, 1, adds(14, 0x1000, 0), adds(16, 1, 0),
           adds(17, 0x42, 0));
    bundle(s, 0x9010, 1, st8(14, 16), adds(15, 8, 14), NOP);
    bundle(s, 0x9020, 1, st8(15, 17), NOP, NOP);
    bundle(s, 0x9030, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    *mapped = qtest_readq(s, 0x4000);
    *missed = qtest_readq(s, 0x1000);
    qtest_quit(s);
}

static void test_ptc_purges_the_translation_cache(void)
{
    uint64_t mapped;
    uint64_t missed;

    run_tc_purge(PURGE_NONE, &mapped, &missed);
    g_assert_cmphex(mapped, ==, 0x42);
    g_assert_cmphex(missed, ==, 0);
    run_tc_purge(PURGE_PTC_L_OTHER, &mapped, &missed);
    g_assert_cmphex(mapped, ==, 0x42);
    g_assert_cmphex(missed, ==, 0);
    run_tc_purge(PURGE_PTC_E, &mapped, &missed);
    g_assert_cmphex(mapped, ==, 0);
    g_assert_cmphex(missed, ==, 1);
    run_tc_purge(PURGE_PTC_L_PAGE, &mapped, &missed);
    g_assert_cmphex(mapped, ==, 0);
    g_assert_cmphex(missed, ==, 1);
}

/*
 * With psr.i set and cr.tpr.mic 4, vector 0x40 stays pending after the timer
 * fires until cr.tpr is cleared and data serialized (SDM vol. 2, 5.8.3).
 */
static void test_tpr_defers_an_enabled_interrupt_until_it_is_lowered(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t code = 0x10000;

    qtest_writeq(s, 0x1800, 0x6018);    /* psr.ic and psr.i */
    qtest_writeq(s, 0x1808, code);
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv = 0x40 */
    step(s, &at, to_cr(66, 9));                 /* cr.tpr: mic 4 */
    step(s, &at, to_cr(2, 0));                  /* cr.iva = 0 */
    step(s, &at, m_from_ar(10, 44));
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
    step(s, &at, add(10, 10, 12));
    step(s, &at, to_cr(1, 10));                 /* cr.itm */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(12, 5));
    step(s, &at, to_cr(19, 12));
    bundle(s, at, 0x11, NOP, NOP, RFI);
    for (unsigned i = 0; i < 2000; i++) {
        bundle(s, code, 1, NOP, NOP, NOP);
        code += 16;
    }
    /* Set the flag, lower tpr, serialize, and wait for the interrupt. */
    bundle(s, code, 1, adds(14, 0x1018, 0), adds(13, 1, 0), NOP);
    bundle(s, code + 16, 1, st8(14, 13), NOP, NOP);
    bundle(s, code + 32, 1, to_cr(66, 0), NOP, NOP);
    bundle(s, code + 48, 1, SRLZ_D, NOP, NOP);
    bundle(s, code + 64, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x3000, 1, from_cr(20, 65), adds(14, 0x1000, 0),
           adds(15, 0x1018, 0));
    bundle(s, 0x3010, 1, st8(14, 20), adds(16, 0x42, 0), adds(17, 0x1008, 0));
    bundle(s, 0x3020, 1, ld8(21, 15), adds(18, 0x1010, 0), NOP);
    bundle(s, 0x3030, 1, st8(17, 21), NOP, NOP);
    bundle(s, 0x3040, 1, st8(18, 16), NOP, NOP);
    bundle(s, 0x3050, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x40);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 1);
    qtest_quit(s);
}

/*
 * An unmapped fetch with rr.ve clear takes the Alternate Instruction TLB vector
 * at IVA + 0xc00 (SDM vol. 2, 5.5); rfi to physical mode ignores the TR map.
 */
static void test_alternate_itlb_vector_and_rfi_to_physical_mode(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_enter_virtual_mode(s, &at, 0x3000, 0x8000);
    qtest_writeq(s, 0x1848, 0x10000);
    qtest_writeq(s, 0x1870, 0x2018);            /* psr.ic only: physical */
    qtest_writeq(s, 0x1878, 0x4000);
    bundle(s, 0x3000, 0x11, NOP, NOP, branch(0x20000 - 0x3000));
    /* Alternate ITLB handler at 0x8c00: record cr.ifa, rfi to 0x4000. */
    bundle(s, 0x8c00, 1, from_cr(21, 20), adds(14, 0x1000, 0),
           adds(5, 0x1870, 0));
    bundle(s, 0x8c10, 1, st8(14, 21), NOP, NOP);
    bundle(s, 0x8c20, 1, ld8(22, 5), adds(5, 0x1878, 0), NOP);
    bundle(s, 0x8c30, 1, to_cr(16, 22), NOP, NOP);      /* cr.ipsr */
    bundle(s, 0x8c40, 1, ld8(23, 5), NOP, NOP);
    bundle(s, 0x8c50, 1, to_cr(19, 23), NOP, NOP);      /* cr.iip */
    bundle(s, 0x8c60, 1, SRLZ_D, NOP, NOP);
    bundle(s, 0x8c70, 0x11, NOP, NOP, RFI);
    bundle(s, 0x4000, 1, adds(5, 0x1848, 0), adds(13, 0x42, 0), NOP);
    bundle(s, 0x4010, 1, ld8(10, 5), adds(14, 0x1008, 0), NOP);
    bundle(s, 0x4020, 1, st8(10, 13), NOP, NOP);        /* pa 0x10000 */
    bundle(s, 0x4030, 1, st8(14, 13), NOP, NOP);
    bundle(s, 0x4040, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 0x20000);
    g_assert_cmphex(qtest_readq(s, 0x10000), ==, 0x42);
    qtest_quit(s);
}

/* M18, setf.sig f1 = r2: opcode 6, x 0, x6 0x1c. */
static uint64_t setf_sig(unsigned f1, unsigned r2)
{
    return (UINT64_C(6) << 37) | (UINT64_C(0x1c) << 30) | (UINT64_C(1) << 27) |
           ((uint64_t)r2 << 13) | ((uint64_t)f1 << 6);
}

/* F4, fcmp.eq.s0 p6, p7 = f6, f7: opcode 4, sf 0, ra 0, ta 0. */
#define FCMP_EQ_F6_F7 ((UINT64_C(4) << 37) | (UINT64_C(7) << 27) | \
                       (UINT64_C(7) << 20) | (UINT64_C(6) << 13) | (6 << 6))

/*
 * Floating-point Fault vector 0x5c00: returns ISR if op faulted, else 0. f6 is
 * setf.sig(sig), unnormal if the integer bit is clear; FPSR is reset or 0.
 */
static uint64_t run_fp_op(uint64_t sig, bool traps_enabled, uint64_t op)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t isr;

    qtest_writeq(s, 0x1100, sig);
    bundle(s, 0xdc00, 1, from_cr(20, 17), NOP, NOP);
    bundle(s, 0xdc10, 1, st8(10, 20), NOP, NOP);
    bundle(s, 0xdc20, 1, st8(18, 23), NOP, NOP);
    bundle(s, 0xdc30, 0x11, NOP, NOP, branch(0));
    bundle(s, at, 1, adds(8, 0x1000, 0), adds(10, 4096, 0), adds(18, 4120, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), adds(23, 0x42, 0), adds(15, 4352, 0));
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), NOP, NOP);
    at += 16;
    bundle(s, at, 1, add(8, 8, 8), NOP, NOP);
    at += 16;
    bundle(s, at, 1, to_cr(2, 8), NOP, NOP);
    at += 16;
    bundle(s, at, 1, ld8(9, 15), NOP, NOP);
    at += 16;
    if (traps_enabled) {
        /* The reset FPSR with every IEEE trap enabled and sf1.td clear. */
        qtest_writeq(s, 0x1108, UINT64_C(0x9804c00000300));
        bundle(s, at, 1, adds(16, 4360, 0), NOP, NOP);
        at += 16;
        bundle(s, at, 1, ld8(11, 16), NOP, NOP);
        at += 16;
        bundle(s, at, 1, m_to_ar(40, 11), NOP, NOP);
        at += 16;
    }
    bundle(s, at, 1, setf_sig(6, 9), NOP, NOP);
    at += 16;
    bundle(s, at, 1, setf_sig(7, 9), NOP, NOP);
    at += 16;
    bundle(s, at, 0x0c, NOP, op, NOP);
    at += 16;
    bundle(s, at, 1, st8(18, 23), NOP, NOP);
    at += 16;
    bundle(s, at, 0x11, NOP, NOP, branch(0));
    run_to_marker(s);
    isr = qtest_readq(s, 0x1000);
    qtest_quit(s);
    return isr;
}

static uint64_t run_fcmp(uint64_t sig, bool traps_enabled)
{
    return run_fp_op(sig, traps_enabled, FCMP_EQ_F6_F7);
}

/* F10 fnorm.s1 f8 = f6, the form SAL_B uses on setf.sig integers. */
#define FNORM_F8_F6 UINT64_C(0x10408600200)

/*
 * Estimated: an unnormal operand takes the Software Assist fault (ISR.code
 * 0x08) whatever the D trap mask; normals never fault (SDM Vol. 1 5.4.1.2).
 */
static void test_unnormal_operand_takes_software_assist(void)
{
    /* Integer bit clear: unnormal. Integer bit set: a normal 1.0-like value. */
    g_assert_cmphex(run_fcmp(1, false) & 0xff, ==, 0x08);
    g_assert_cmphex(run_fcmp(1, true) & 0xff, ==, 0x08);
    g_assert_cmphex(run_fcmp(UINT64_C(1) << 63, false), ==, 0);
    g_assert_cmphex(run_fcmp(UINT64_C(1) << 63, true), ==, 0);
}

/*
 * fnorm on an unnormal significand keeps the SDM response (SAL_B converts with
 * setf.sig and fnorm): no fault with D masked, ISR.code 0x02 with it enabled.
 */
static void test_fnorm_of_an_integer_pattern_takes_no_assist(void)
{
    g_assert_cmphex(run_fp_op(1, false, FNORM_F8_F6), ==, 0);
    g_assert_cmphex(run_fp_op(1, true, FNORM_F8_F6) & 0xff, ==, 0x02);
}

/*
 * Write 0x2000 to MSR index and read it back. Returns the value read; *faulted
 * says whether either access took Reserved Register/Field.
 */
static uint64_t msr_readback(unsigned index, bool *faulted)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint64_t value;

    /* IVA = 0x8000: the handler at +0x5400 stores 0x42 at 0x1000. */
    bundle(s, 0xd400, 1, st8(10, 13), NOP, NOP);
    bundle(s, 0xd410, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x2000, 1, adds(8, 0x1000, 0), adds(9, 0x1000, 0),
           adds(10, 4096, 0));
    bundle(s, 0x2010, 1, add(8, 8, 8), add(9, 9, 9), adds(13, 0x42, 0));
    bundle(s, 0x2020, 1, add(8, 8, 8), NOP, NOP);
    bundle(s, 0x2030, 1, add(8, 8, 8), NOP, NOP);
    bundle(s, 0x2040, 1, to_cr(2, 8), NOP, NOP);
    bundle(s, 0x2050, 1, MOV_PSRL_R9, NOP, NOP);
    bundle(s, 0x2060, 1, adds(11, index, 0), adds(14, 4104, 0),
           adds(15, 4112, 0));
    bundle(s, 0x2070, 1, to_msr(11, 9), NOP, NOP);
    bundle(s, 0x2080, 1, from_msr(12, 11), NOP, NOP);
    bundle(s, 0x2090, 1, st8(15, 12), NOP, NOP);
    bundle(s, 0x20a0, 1, st8(14, 13), NOP, NOP);
    bundle(s, 0x20b0, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1000) != 0x42 && qtest_readq(s, 0x1008) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    *faulted = qtest_readq(s, 0x1000) == 0x42;
    g_assert_true(*faulted || qtest_readq(s, 0x1008) == 0x42);
    value = qtest_readq(s, 0x1010);
    qtest_quit(s);
    return value;
}

/* The PAL reads indices past the 2048 stored ones and goes on: reads give 0. */
static void test_msr_index_above_range_is_ignored(void)
{
    bool faulted;

    g_assert_cmphex(msr_readback(2047, &faulted), ==, 0x2000);
    g_assert_false(faulted);
    g_assert_cmphex(msr_readback(2048, &faulted), ==, 0);
    g_assert_false(faulted);
}

static void test_native_break(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    int64_t deadline;
    uint64_t marker = 0;

    /* ssm psr.ic followed by instruction serialization before the fault. */
    bundle(s, 0x2000, 1, (UINT64_C(6) << 27) | (1 << (13 + 6)), NOP, NOP);
    bundle(s, 0x2010, 1, (UINT64_C(3) << 31) | (1 << 27), NOP, NOP);
    bundle(s, 0x2020, 1, adds(8, 5, 0), adds(9, 7, 0), adds(11, 4096, 0));
    bundle(s, 0x2030, 1,
           (UINT64_C(8) << 37) | (9 << 20) | (8 << 13) | (10 << 6),
           NOP, NOP);
    bundle(s, 0x2040, 0x11, NOP, NOP, branch(32));
    bundle(s, 0x2050, 1, adds(10, -1, 0), NOP, NOP);
    bundle(s, 0x2060, 1, st8(11, 10), NOP, NOP);
    /* Former FPSWA portal immediate must take an ordinary BREAK fault. */
    bundle(s, 0x2070, 1, NOP, (UINT64_C(1) << 36) | (1 << 6), NOP);

    /* IVA is zero in the fixture; BREAK's architectural offset is 0x2c00. */
    bundle(s, 0x2c00, 1, from_cr(10, 19), adds(8, 0x1008, 0), NOP);
    bundle(s, 0x2c10, 1, st8(8, 10), NOP, NOP);
    bundle(s, 0x2c20, 1, from_cr(11, 24), adds(8, 8, 8), NOP);
    bundle(s, 0x2c30, 1, st8(8, 11), NOP, NOP);
    bundle(s, 0x2c40, 1, from_cr(12, 16), adds(8, 8, 8), NOP);
    bundle(s, 0x2c50, 1, st8(8, 12), NOP, NOP);
    bundle(s, 0x2c60, 1, adds(13, 0x42, 0), adds(8, 8, 8), NOP);
    bundle(s, 0x2c70, 1, st8(8, 13), NOP, NOP);
    bundle(s, 0x2c80, 0x11, NOP, NOP, branch(0));

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do {
        marker = qtest_readq(s, 0x1020);
    } while (marker != 0x42 && g_get_monotonic_time() < deadline);
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    if (marker != 0x42) {
        g_autofree char *registers = qtest_hmp(s, "info registers");
        g_test_message("%s", registers);
    }
    g_assert_cmphex(marker, ==, 0x42);
    g_assert_cmpuint(qtest_readq(s, 0x1000), ==, 12);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0x2070);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x100001);
    g_assert_cmpuint((qtest_readq(s, 0x1018) >> 41) & 3, ==, 1);
    qtest_quit(s);
}

/*
 * Expected values come from the Itanium SDM rev 2.3 instruction pages, not
 * from a run of the model. One instruction per bundle; results at RES_BASE.
 */
#define CODE_IVA  0x110000
#define TAB_BASE  0x120000      /* word 0 is the NaT source for ld8.fill */
#define RES_BASE  0x180000
#define FLOG_BASE 0x1f0000
#define NAT_MARK  UINT64_C(0x4e61542121212121)

typedef struct Prog {
    QTestState *s;
    uint64_t at;
    uint64_t tab;
    GArray *exp;        /* expected result words */
    GPtrArray *tag;
    GArray *fexp;       /* expected ISR words of the faults, in order */
} Prog;

static uint64_t addl(unsigned dest, int imm, unsigned r3)
{
    unsigned v = (unsigned)imm & 0x3fffff;

    return (UINT64_C(9) << 37) | ((uint64_t)(v >> 21) << 36) |
           ((uint64_t)((v >> 7) & 0x1ff) << 27) |
           ((uint64_t)((v >> 16) & 0x1f) << 22) | ((uint64_t)r3 << 20) |
           ((uint64_t)(v & 0x7f) << 13) | (dest << 6);
}

/* A1 logical ops: x4 3, x2b 0 and, 1 andcm, 2 or, 3 xor. */
static uint64_t alu_and(unsigned dest, unsigned a, unsigned b)
{
    return (UINT64_C(8) << 37) | (UINT64_C(3) << 29) |
           ((uint64_t)b << 20) | (a << 13) | (dest << 6);
}

/* I23 mov pr = r2, mask17 (imm16 = mask17 >> 1) and I25 mov r1 = pr. */
static uint64_t mov_to_pr(unsigned r2, unsigned mask17)
{
    unsigned imm = (mask17 >> 1) & 0xffff;

    return ((uint64_t)(imm >> 15) << 36) | (UINT64_C(3) << 33) |
           ((uint64_t)((imm >> 7) & 0xff) << 24) | ((uint64_t)r2 << 13) |
           ((uint64_t)(imm & 0x7f) << 6);
}

static uint64_t mov_from_pr(unsigned r1)
{
    return (UINT64_C(0x33) << 27) | ((uint64_t)r1 << 6);
}

/* I17 tnat.z p1, p2 = r3. */
static uint64_t tnat_z(unsigned p1, unsigned p2, unsigned r3)
{
    return (UINT64_C(5) << 37) | ((uint64_t)p2 << 27) | ((uint64_t)r3 << 20) |
           (UINT64_C(1) << 13) | ((uint64_t)p1 << 6);
}

static void emit(Prog *p, char unit, uint64_t insn)
{
    switch (unit) {
    case 'I':
        bundle(p->s, p->at, 1, NOP, insn, NOP);
        break;
    case 'F':
        bundle(p->s, p->at, 0x0d, NOP, insn, NOP);
        break;
    default:
        bundle(p->s, p->at, 1, insn, NOP, NOP);
    }
    p->at += 16;
}

/* Load a 64-bit constant through the table. */
static void movi(Prog *p, unsigned reg, uint64_t v)
{
    if ((int64_t)v >= -8192 && (int64_t)v < 8192) {
        emit(p, 'M', adds(reg, (int)(int64_t)v, 0));
        return;
    }
    qtest_writeq(p->s, p->tab, v);
    emit(p, 'M', addl(5, p->tab, 0));
    emit(p, 'M', ld8(reg, 5));
    p->tab += 8;
}

static void G_GNUC_PRINTF(3, 4)
expect(Prog *p, uint64_t v, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_ptr_array_add(p->tag, g_strdup_vprintf(fmt, ap));
    va_end(ap);
    g_array_append_val(p->exp, v);
}

/* st8 [r10] = reg, r10 += 8; a NaT source stores NAT_MARK instead. */
static void G_GNUC_PRINTF(4, 5)
store_gr(Prog *p, unsigned reg, uint64_t want, const char *fmt, ...)
{
    g_autofree char *t = NULL;
    va_list ap;

    va_start(ap, fmt);
    t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    emit(p, 'I', tnat_z(8, 9, reg));
    bundle(p->s, p->at, 9, st8(10, reg) | 8, st8(10, 31) | 9,
           adds(10, 8, 10));
    p->at += 16;
    emit(p, 'I', mov_to_pr(0, 0x300));      /* leave p8 and p9 clear */
    expect(p, want, "%s", t);
}

static void G_GNUC_PRINTF(3, 4)
store_pr(Prog *p, uint64_t want, const char *fmt, ...)
{
    g_autofree char *t = NULL;
    va_list ap;

    va_start(ap, fmt);
    t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    emit(p, 'I', mov_from_pr(20));
    store_gr(p, 20, want, "%s", t);
}

/* Log cr.isr at r27 (and cr.iim for 0x5700), then resume at the next bundle. */
static void install_handler(Prog *p, unsigned vector)
{
    uint64_t at = CODE_IVA + vector;

    bundle(p->s, at, 1, from_cr(28, 17), NOP, NOP);
    bundle(p->s, at + 16, 1, st8(27, 28), adds(27, 8, 27), NOP);
    /* The Speculation vector also logs cr.iim, the other vectors do not. */
    bundle(p->s, at + 32, 1, vector == 0x5700 ? from_cr(28, 24) : NOP, NOP,
           NOP);
    bundle(p->s, at + 48, 1, vector == 0x5700 ? st8(27, 28) : NOP,
           vector == 0x5700 ? adds(27, 8, 27) : NOP, NOP);
    bundle(p->s, at + 64, 1, from_cr(28, 19), NOP, NOP);
    bundle(p->s, at + 80, 1, adds(28, 16, 28), NOP, NOP);
    bundle(p->s, at + 96, 1, to_cr(19, 28), NOP, NOP);
    bundle(p->s, at + 112, 1, from_cr(28, 16), NOP, NOP);
    bundle(p->s, at + 128, 1, alu_and(28, 28, 26), NOP, NOP);
    bundle(p->s, at + 144, 1, to_cr(16, 28), NOP, NOP);
    bundle(p->s, at + 160, 0x11, NOP, NOP, RFI);
}

/* r10 results, r27 fault log, r26 clears ipsr.ri, r30 a NaT, r31 NAT_MARK. */
static Prog *prog_new(void)
{
    Prog *p = g_new0(Prog, 1);

    p->s = qtest_init("-machine none -cpu madison-1600-3m -m 16M -accel tcg "
                      "-S -device loader,addr=0x2000,cpu-num=0");
    p->at = 0x2000;
    p->tab = TAB_BASE + 8;
    p->exp = g_array_new(false, false, sizeof(uint64_t));
    p->fexp = g_array_new(false, false, sizeof(uint64_t));
    p->tag = g_ptr_array_new_with_free_func(g_free);
    install_handler(p, 0x5400);
    install_handler(p, 0x5600);
    install_handler(p, 0x5c00);
    install_handler(p, 0x5700);
    emit(p, 'M', addl(10, RES_BASE, 0));
    emit(p, 'M', addl(27, FLOG_BASE, 0));
    emit(p, 'M', addl(5, CODE_IVA, 0));
    emit(p, 'M', to_cr(2, 5));
    movi(p, 26, ~(UINT64_C(3) << 41));
    movi(p, 31, NAT_MARK);
    emit(p, 'M', adds(5, 1, 0));
    emit(p, 'M', m_to_ar(36, 5));
    emit(p, 'M', addl(5, TAB_BASE, 0));
    emit(p, 'M', ld8_fill(30, 5));
    emit(p, 'M', (UINT64_C(6) << 27) | (1 << (13 + 6)));    /* ssm psr.ic */
    emit(p, 'M', (UINT64_C(3) << 31) | (1 << 27));          /* srlz.i */
    emit(p, 'I', mov_to_pr(0, 0x1fffe));                     /* all clear */
    return p;
}

#define ANY UINT64_C(0xa5a5a5a5a5a5a5a5)    /* result checked by the caller */

typedef unsigned (*PostCheck)(const uint64_t *got, void *ctx);

/* Run the program; returns the number of rows that differ from the SDM. */
static unsigned prog_check_post(Prog *p, PostCheck post, void *ctx)
{
    uint64_t *got = g_new0(uint64_t, p->exp->len);
    uint64_t *flog = g_new0(uint64_t, p->fexp->len + 1);
    unsigned bad = 0;

    movi(p, 13, 0x42);
    emit(p, 'M', st8(27, 13));
    emit(p, 'M', adds(14, 4096, 0));
    emit(p, 'M', st8(14, 13));
    bundle(p->s, p->at, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(p->s, "{ 'execute': 'cont' }");
    for (int64_t end = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
         qtest_readq(p->s, 0x1000) != 0x42 && g_get_monotonic_time() < end;) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(p->s, "{ 'execute': 'stop' }");
    if (qtest_readq(p->s, 0x1000) != 0x42) {
        g_autofree char *regs = qtest_hmp(p->s, "info registers");

        g_test_message("program did not finish: %s", regs);
        bad++;
    }
    qtest_memread(p->s, RES_BASE, got, p->exp->len * 8);
    qtest_memread(p->s, FLOG_BASE, flog, (p->fexp->len + 1) * 8);
    for (unsigned i = 0; i < p->exp->len; i++) {
        uint64_t want = g_array_index(p->exp, uint64_t, i);

        got[i] = le64_to_cpu(got[i]);
        if (want != ANY && got[i] != want) {
            g_test_message("%s: got 0x%" PRIx64 " want 0x%" PRIx64,
                           (char *)g_ptr_array_index(p->tag, i),
                           got[i], want);
            bad++;
        }
    }
    if (post) {
        bad += post(got, ctx);
    }
    for (unsigned i = 0; i <= p->fexp->len; i++) {
        uint64_t want = i < p->fexp->len ? g_array_index(p->fexp, uint64_t, i)
                                         : 0x42;
        uint64_t have = le64_to_cpu(flog[i]);

        if (i < p->fexp->len) {
            have &= (UINT64_C(3) << 41) | 0xffff;     /* ISR.ei, ISR.code */
        }
        if (have != want) {
            g_test_message("fault %u: got 0x%" PRIx64 " want 0x%" PRIx64,
                           i, have, want);
            bad++;
        }
    }
    qtest_quit(p->s);
    g_free(got);
    g_free(flog);
    g_array_free(p->exp, true);
    g_array_free(p->fexp, true);
    g_ptr_array_free(p->tag, true);
    g_free(p);
    return bad;
}

#define prog_check(p) prog_check_post(p, NULL, NULL)

/* The next instruction faults: ISR.code 0 and ISR.ei the slot (SDM 2:8-?). */
static void expect_fault_code(Prog *p, unsigned slot, unsigned code)
{
    uint64_t isr = ((uint64_t)slot << 41) | code;

    g_array_append_val(p->fexp, isr);
}

#define expect_fault(p, slot) expect_fault_code(p, slot, 0)

/* A Speculation fault logs ISR then cr.iim. */
static void expect_spec_fault(Prog *p, unsigned slot, unsigned code,
                              uint64_t iim)
{
    expect_fault_code(p, slot, code);
    g_array_append_val(p->fexp, iim);
}

enum { CT_NONE, CT_UNC, CT_AND, CT_OR, CT_ORANDCM };
enum { R_EQ, R_NE, R_LT, R_LE, R_GT, R_GE, R_LTU, R_LEU, R_GTU, R_GEU };

static const char *const ct_name[] = { "", ".unc", ".and", ".or", ".or.andcm" };
static const char *const rel_name[] = {
    "eq", "ne", "lt", "le", "gt", "ge", "ltu", "leu", "gtu", "geu"
};

/* SDM Vol. 3 p. 3:39 Table 2-15: how a compare type updates p1 and p2. */
static void apply_ctype(int ct, bool qp, bool nat, bool rel, bool *p1, bool *p2)
{
    if (!qp) {
        if (ct == CT_UNC) {
            *p1 = *p2 = false;
        }
        return;
    }
    switch (ct) {
    case CT_AND:
        if (nat || !rel) {
            *p1 = *p2 = false;
        }
        break;
    case CT_OR:
        if (!nat && rel) {
            *p1 = *p2 = true;
        }
        break;
    case CT_ORANDCM:
        if (!nat && rel) {
            *p1 = true;
            *p2 = false;
        }
        break;
    default:
        *p1 = !nat && rel;
        *p2 = !nat && !rel;
    }
}

/* Truth of lt, ltu and eq for (a, b) gives every other relation. */
static bool rel_true(int rel, const bool t[3])
{
    switch (rel) {
    case R_EQ:
        return t[2];
    case R_NE:
        return !t[2];
    case R_LT:
        return t[0];
    case R_LE:
        return t[0] || t[2];
    case R_GT:
        return !t[0] && !t[2];
    case R_GE:
        return !t[0];
    case R_LTU:
        return t[1];
    case R_LEU:
        return t[1] || t[2];
    case R_GTU:
        return !t[1] && !t[2];
    default:
        return !t[1];
    }
}

/* A6, A7, A8 (Vol. 3 Tables 4-10, 4-11); pseudo-ops: Tables 2-16, 2-17. */
static uint64_t cmp_insn(char form, bool w32, int rel, int ct, unsigned p1,
                         unsigned p2, int a, unsigned r3, unsigned qp)
{
    static const unsigned opc_of[] = { 0xe, 0xe, 0xc, 0xc, 0xc, 0xc,
                                       0xd, 0xd, 0xd, 0xd };
    unsigned opc = opc_of[rel], ta = 0, tb = 0, c = ct == CT_UNC;
    bool par = ct >= CT_AND, swp = false;
    uint64_t insn;
    unsigned tmp;

    if (par) {
        opc = ct == CT_AND ? 0xc : ct == CT_OR ? 0xd : 0xe;
    }
    if (form == 'z') {
        /* A7 reads (0 rel r3): gt, le, ge and lt are the hardware ones. */
        /* (ta, c): gt 0 0, le 0 1, ge 1 0, lt 1 1. */
        unsigned i = rel == R_GT ? 0 : rel == R_LE ? 1 : rel == R_GE ? 2 : 3;

        g_assert_true(par);
        return ((uint64_t)opc << 37) | (UINT64_C(1) << 36) |
               ((uint64_t)w32 << 34) | ((uint64_t)(i >> 1) << 33) |
               ((uint64_t)p2 << 27) | ((uint64_t)r3 << 20) |
               ((uint64_t)(i & 1) << 12) | ((uint64_t)p1 << 6) | qp;
    }
    if (par) {
        g_assert_true(rel == R_EQ || rel == R_NE);
        ta = 1;
        c = rel == R_NE;
    } else {
        switch (rel) {
        case R_NE:
            swp = true;
            break;
        case R_LE:
            if (form == 'r') {
                tmp = a;
                a = r3;
                r3 = tmp;
                swp = true;
            } else {
                a--;
            }
            break;
        case R_GT:
            if (form == 'r') {
                tmp = a;
                a = r3;
                r3 = tmp;
            } else {
                a--;
                swp = true;
            }
            break;
        case R_GE:
            swp = true;
            break;
        case R_LEU:
            if (form == 'r') {
                tmp = a;
                a = r3;
                r3 = tmp;
                swp = true;
            } else {
                a--;
            }
            break;
        case R_GTU:
            if (form == 'r') {
                tmp = a;
                a = r3;
                r3 = tmp;
            } else {
                a--;
                swp = true;
            }
            break;
        case R_GEU:
            swp = true;
            break;
        }
    }
    if (swp) {
        tmp = p1;
        p1 = p2;
        p2 = tmp;
    }
    insn = ((uint64_t)opc << 37) | ((uint64_t)tb << 36) | ((uint64_t)ta << 33) |
           ((uint64_t)p2 << 27) | ((uint64_t)r3 << 20) | ((uint64_t)c << 12) |
           ((uint64_t)p1 << 6) | qp;
    if (form == 'r') {
        return insn | ((uint64_t)w32 << 34) | ((uint64_t)a << 13);
    }
    return insn | ((uint64_t)(w32 ? 3 : 2) << 34) |
           ((uint64_t)(((unsigned)a >> 7) & 1) << 36) |
           ((uint64_t)((unsigned)a & 0x7f) << 13);
}

typedef struct CmpRow {
    int64_t a, b;
    bool t64[3], t32[3];        /* lt, ltu, eq of the 64-bit and low-32 views */
} CmpRow;

/* Truth values worked out by hand from the signed and unsigned orderings. */
static const CmpRow cmp_rows[] = {
    { 5, 5, { 0, 0, 1 }, { 0, 0, 1 } },
    { 3, 5, { 1, 1, 0 }, { 1, 1, 0 } },
    { 7, 5, { 0, 0, 0 }, { 0, 0, 0 } },
    { -1, 1, { 1, 0, 0 }, { 1, 0, 0 } },
    { 1, -1, { 0, 1, 0 }, { 0, 1, 0 } },
    /* Only the low 32 bits count for cmp4: these two are equal there. */
    { 0x100000001, 1, { 0, 0, 0 }, { 0, 0, 1 } },
    { 0x100000000, 0, { 0, 0, 0 }, { 0, 0, 1 } },
    /* 0x80000000 is negative as 32 bits, positive as 64. */
    { 0x80000000, 0, { 0, 0, 0 }, { 1, 0, 0 } },
    { -1, INT64_MAX, { 1, 0, 0 }, { 0, 0, 1 } },
};

/* imm8 as the first operand against a register. */
static const struct { int imm; int64_t b; bool t64[3], t32[3]; } imm_rows[] = {
    { 5, 5, { 0, 0, 1 }, { 0, 0, 1 } },
    { -1, 1, { 1, 0, 0 }, { 1, 0, 0 } },
    { 127, 128, { 1, 1, 0 }, { 1, 1, 0 } },
    { 5, 0x100000005, { 1, 1, 0 }, { 0, 0, 1 } },
    { -1, 0xffffffff, { 1, 0, 0 }, { 0, 0, 1 } },
};

/* x against zero: sign of x as 64 and as the low 32 bits. */
static const struct { int64_t x; int s64, s32; } zero_rows[] = {
    { 5, 1, 1 }, { -5, -1, -1 }, { 0, 0, 0 }, { 0x100000000, 1, 0 },
    { 0x80000000, 1, -1 }, { 0xffffffff, 1, -1 },
};

static void set_init(Prog *p, unsigned k)
{
    emit(p, 'I', mov_to_pr(12 + k, 0xc0));
}

static void cmp_check(Prog *p, bool w32, char form, int rel, int ct, int init,
                      bool qp, bool nat, bool truth, int64_t a, int64_t b)
{
    bool p6 = init & 1, p7 = init & 2;

    apply_ctype(ct, qp, nat, truth, &p6, &p7);
    store_pr(p, 1 | ((uint64_t)p6 << 6) | ((uint64_t)p7 << 7),
             "cmp%s.%s%s form %c a=%" PRIx64 " b=%" PRIx64 " init=%d qp=%d "
             "nat=%d", w32 ? "4" : "", rel_name[rel], ct_name[ct], form,
             (uint64_t)a, (uint64_t)b, init, qp, nat);
}

/* SDM Vol. 3 pp. 3:39-3:44 (cmp, cmp4) and Tables 2-15 to 2-17. */
static void run_cmp(bool w32)
{
    Prog *p = prog_new();

    for (unsigned k = 0; k < 4; k++) {
        movi(p, 12 + k, k * 0x40);
    }
    for (unsigned r = 0; r < G_N_ELEMENTS(cmp_rows) + 2; r++) {
        const CmpRow *row = &cmp_rows[r < G_N_ELEMENTS(cmp_rows) ? r : 0];
        bool nat_a = r == G_N_ELEMENTS(cmp_rows);
        bool nat_b = r == G_N_ELEMENTS(cmp_rows) + 1;
        const bool *t = w32 ? row->t32 : row->t64;

        movi(p, 8, row->a);
        movi(p, 9, row->b);
        if (nat_a) {
            emit(p, 'M', adds(8, 0, 30));
        }
        if (nat_b) {
            emit(p, 'M', adds(9, 0, 30));
        }
        for (int rel = R_EQ; rel <= R_GEU; rel++) {
            for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
                if (ct >= CT_AND && rel > R_NE) {
                    continue;
                }
                for (int init = 0; init < 4; init++) {
                    set_init(p, init);
                    emit(p, 'M', cmp_insn('r', w32, rel, ct, 6, 7, 8, 9, 0));
                    cmp_check(p, w32, 'r', rel, ct, init, true,
                              nat_a || nat_b, rel_true(rel, t), row->a, row->b);
                }
            }
        }
    }
    /* A false qualifying predicate leaves p6 and p7 alone, but .unc clears. */
    movi(p, 8, 5);
    movi(p, 9, 5);
    for (int rel = R_EQ; rel <= R_NE; rel++) {
        for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
            for (int init = 0; init < 4; init++) {
                set_init(p, init);
                emit(p, 'M', cmp_insn('r', w32, rel, ct, 6, 7, 8, 9, 5));
                cmp_check(p, w32, 'r', rel, ct, init, false, false, true, 5, 5);
            }
        }
    }
    /* imm8 form: le/gt subtract one from the immediate, the others swap. */
    for (unsigned r = 0; r < G_N_ELEMENTS(imm_rows); r++) {
        const bool *t = w32 ? imm_rows[r].t32 : imm_rows[r].t64;

        movi(p, 9, imm_rows[r].b);
        for (int rel = R_EQ; rel <= R_GEU; rel++) {
            for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
                if (ct >= CT_AND && rel > R_NE) {
                    continue;
                }
                for (int init = 0; init < 4; init++) {
                    set_init(p, init);
                    emit(p, 'M', cmp_insn('i', w32, rel, ct, 6, 7,
                                          imm_rows[r].imm, 9, 0));
                    cmp_check(p, w32, 'i', rel, ct, init, true, false,
                              rel_true(rel, t), imm_rows[r].imm,
                              imm_rows[r].b);
                }
            }
        }
    }
    /* Compare with zero: 0 rel x in hardware, x rel 0 by the pseudo-op. */
    for (unsigned r = 0; r < G_N_ELEMENTS(zero_rows) + 1; r++) {
        bool nat = r == G_N_ELEMENTS(zero_rows);
        int s = w32 ? zero_rows[nat ? 0 : r].s32 : zero_rows[nat ? 0 : r].s64;

        movi(p, 9, zero_rows[nat ? 0 : r].x);
        if (nat) {
            emit(p, 'M', adds(9, 0, 30));
        }
        for (int rel = R_LT; rel <= R_GE; rel++) {
            for (int ct = CT_AND; ct <= CT_ORANDCM; ct++) {
                for (int pseudo = 0; pseudo < 2; pseudo++) {
                    /* (0 rel x) has sign -s; (x rel 0) has sign s. */
                    int sg = pseudo ? s : -s;
                    bool truth = rel == R_LT ? sg < 0 : rel == R_LE ? sg <= 0 :
                                 rel == R_GT ? sg > 0 : sg >= 0;
                    int hw = !pseudo ? rel : rel == R_LT ? R_GT :
                             rel == R_LE ? R_GE : rel == R_GT ? R_LT : R_LE;

                    for (int init = 0; init < 4; init++) {
                        set_init(p, init);
                        emit(p, 'M', cmp_insn('z', w32, hw, ct, 6, 7, 0, 9, 0));
                        cmp_check(p, w32, 'z', rel, ct, init, true, nat, truth,
                                  pseudo, zero_rows[nat ? 0 : r].x);
                    }
                }
            }
        }
    }
    if (!w32) {
        /* p1 == p2 takes an Illegal Operation fault if qp is set or for .unc */
        emit(p, 'M', cmp_insn('r', w32, R_EQ, CT_NONE, 6, 6, 8, 9, 0));
        expect_fault(p, 0);
        emit(p, 'M', cmp_insn('r', w32, R_EQ, CT_UNC, 6, 6, 8, 9, 5));
        expect_fault(p, 0);
        emit(p, 'M', cmp_insn('r', w32, R_EQ, CT_NONE, 6, 6, 8, 9, 5));
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

static void test_cmp_relations_and_types(void)
{
    run_cmp(false);
}

static void test_cmp4_uses_the_low_32_bits(void)
{
    run_cmp(true);
}

/* tbit, tnat, tf: Vol. 3 Table 4-23. Only .z is hardware for normal/unc. */
static uint64_t tbit_insn(char kind, bool nz, int ct, unsigned p1, unsigned p2,
                          unsigned r3, unsigned field, unsigned qp)
{
    unsigned ta = ct == CT_OR || ct == CT_ORANDCM;
    unsigned tb = ct == CT_AND || ct == CT_ORANDCM;
    unsigned c = ct >= CT_AND ? nz : ct == CT_UNC;

    if (ct < CT_AND && nz) {
        unsigned t = p1;

        p1 = p2;
        p2 = t;
    }
    return (UINT64_C(5) << 37) | ((uint64_t)tb << 36) | ((uint64_t)ta << 33) |
           ((uint64_t)p2 << 27) | ((uint64_t)r3 << 20) |
           ((uint64_t)field << 14) | ((uint64_t)c << 12) |
           ((uint64_t)p1 << 6) | qp |
           (kind == 'b' ? 0 : UINT64_C(1) << 13) |
           (kind == 'f' ? UINT64_C(1) << 19 : 0);
}

static void tbit_check(Prog *p, char kind, bool nz, int ct, int init, bool qp,
                       bool nat, bool rel, uint64_t v, unsigned f)
{
    bool p6 = init & 1, p7 = init & 2;

    apply_ctype(ct, qp, nat, rel, &p6, &p7);
    store_pr(p, 1 | ((uint64_t)p6 << 6) | ((uint64_t)p7 << 7),
             "t%c.%s%s v=%" PRIx64 " f=%u init=%d qp=%d nat=%d", kind,
             nz ? "nz" : "z", ct_name[ct], v, f, init, qp, nat);
}

/* SDM Vol. 3 pp. 3:261-3:268 (tbit, tf, tnat); CPUID[4] bits per 1:34. */
static void test_tbit_tnat_tf(void)
{
    static const struct { uint64_t v; unsigned pos; } bits[] = {
        { 5, 0 }, { 5, 1 }, { 5, 2 }, { UINT64_C(1) << 63, 63 },
        { UINT64_C(1) << 63, 0 },
    };
    Prog *p = prog_new();

    for (unsigned k = 0; k < 4; k++) {
        movi(p, 12 + k, k * 0x40);
    }
    /* Itanium 2 CPUID[4] has only lb, sd and ao set (Itanium 2 manual 12.4) */
    for (unsigned r = 0; r < G_N_ELEMENTS(bits) + 2; r++) {
        bool nat = r == G_N_ELEMENTS(bits);
        bool natreg = nat || r == G_N_ELEMENTS(bits) + 1;
        unsigned i = r < G_N_ELEMENTS(bits) ? r : 0;
        bool bit = (bits[i].v >> bits[i].pos) & 1;

        movi(p, 9, bits[i].v);
        if (natreg) {
            emit(p, 'M', adds(9, 0, 30));
        }
        for (int nz = 0; nz < 2; nz++) {
            for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
                for (int init = 0; init < 4; init++) {
                    /* tbit, except the same row is not repeated for tnat */
                    if (r < G_N_ELEMENTS(bits) || nat) {
                        set_init(p, init);
                        emit(p, 'I', tbit_insn('b', nz, ct, 6, 7, 9,
                                               bits[i].pos, 0));
                        tbit_check(p, 'b', nz, ct, init, true, nat,
                                   nz ? bit : !bit, bits[i].v, bits[i].pos);
                    }
                    if (r == 0 || natreg) {
                        set_init(p, init);
                        emit(p, 'I', tbit_insn('n', nz, ct, 6, 7, 9, 0, 0));
                        tbit_check(p, 'n', nz, ct, init, true, false,
                                   nz ? natreg : !natreg, bits[i].v, natreg);
                    }
                }
            }
        }
    }
    /* tf selects CPUID[4] bit 32 + imm5b: clz, mpy4, then undefined. */
    for (unsigned f = 0; f < 3; f++) {
        for (int nz = 0; nz < 2; nz++) {
            for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
                for (int init = 0; init < 4; init++) {
                    set_init(p, init);
                    emit(p, 'I', tbit_insn('f', nz, ct, 6, 7, 0, f, 0));
                    tbit_check(p, 'f', nz, ct, init, true, false, !nz, 0, f);
                }
            }
        }
    }
    /* A false qp skips every type but .unc, which clears both. */
    movi(p, 9, 5);
    for (int ct = CT_NONE; ct <= CT_ORANDCM; ct++) {
        for (int init = 0; init < 4; init++) {
            set_init(p, init);
            emit(p, 'I', tbit_insn('b', 0, ct, 6, 7, 9, 0, 5));
            tbit_check(p, 'b', 0, ct, init, false, false, true, 5, 0);
            set_init(p, init);
            emit(p, 'I', tbit_insn('n', 0, ct, 6, 7, 9, 0, 5));
            tbit_check(p, 'n', 0, ct, init, false, false, true, 5, 0);
            set_init(p, init);
            emit(p, 'I', tbit_insn('f', 0, ct, 6, 7, 0, 0, 5));
            tbit_check(p, 'f', 0, ct, init, false, false, true, 0, 0);
        }
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* M34 alloc, M25 flushrs, B8 cover, B4 br.ret, I21 mov b = r, I26 mov.i ar. */
static uint64_t alloc_frame(unsigned r1, unsigned sof)
{
    return (UINT64_C(1) << 37) | (UINT64_C(6) << 33) | ((uint64_t)sof << 20) |
           ((uint64_t)sof << 13) | (r1 << 6);
}

#define FLUSHRS (UINT64_C(0xc) << 27)
#define COVER   (UINT64_C(0x02) << 27)
#define BR_RET0 ((UINT64_C(0x21) << 27) | (UINT64_C(4) << 6))

static uint64_t mov_to_br(unsigned b1, unsigned r2)
{
    return (UINT64_C(7) << 33) | ((uint64_t)r2 << 13) | (b1 << 6);
}

static uint64_t i_to_ar(unsigned ar, unsigned r2)
{
    return (UINT64_C(0x2a) << 27) | ((uint64_t)ar << 20) | ((uint64_t)r2 << 13);
}

/*
 * Linux's kernel-mode break entry writes RNAT, then BSPSTORE; later fills
 * must take NaTs from that RNAT, not stale memory at the collection slot.
 */
static void test_rnat_survives_a_bspstore_write(void)
{
    const uint64_t rbs = 0x140000, bspstore = rbs + 20 * 8;
    Prog *p = prog_new();
    uint64_t ret;

    /* Registers at RNAT bits 16..19; bit 16 is a NaT, the rest are not. */
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeq(p->s, rbs + (16 + i) * 8, 0x1000 + i);
    }
    /* Never stored by the RSE: bits 17..19 set, bit 16 clear. */
    qtest_writeq(p->s, rbs + 0x1f8, INT64_MAX & ~(UINT64_C(1) << 16));
    emit(p, 'M', alloc_frame(2, 0));
    movi(p, 20, bspstore);
    movi(p, 21, UINT64_C(1) << 16);
    movi(p, 22, rbs + 8);
    emit(p, 'M', m_to_ar(18, 20));
    emit(p, 'M', m_to_ar(19, 21));
    emit(p, 'M', m_to_ar(18, 22));
    emit(p, 'M', m_to_ar(19, 21));
    emit(p, 'M', m_to_ar(18, 20));
    /* Spill 50 registers across the collection slot, then return to 54. */
    emit(p, 'M', alloc_frame(2, 50));
    bundle(p->s, p->at, 0x11, NOP, NOP, COVER);
    p->at += 16;
    emit(p, 'M', FLUSHRS);
    movi(p, 24, 54 | (54 << 7));
    emit(p, 'I', i_to_ar(64, 24));
    ret = p->at + 3 * 16;
    emit(p, 'M', addl(25, ret, 0));
    emit(p, 'I', mov_to_br(0, 25));
    bundle(p->s, p->at, 0x11, NOP, NOP, BR_RET0);
    p->at += 16;
    g_assert_cmphex(p->at, ==, ret);
    store_gr(p, 32, NAT_MARK, "r32 NaT from RNAT bit 16");
    for (unsigned i = 1; i < 4; i++) {
        store_gr(p, 32 + i, 0x1000 + i, "r%u", 32 + i);
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* I-unit and multimedia ALU encodings, Vol. 3 chapter 4. */
static uint64_t extr_i(bool sgn, unsigned r1, unsigned r3, unsigned pos,
                       unsigned len)
{
    return (UINT64_C(5) << 37) | (UINT64_C(1) << 34) |
           ((uint64_t)(len - 1) << 27) | ((uint64_t)r3 << 20) |
           ((uint64_t)pos << 14) | ((uint64_t)sgn << 13) | ((uint64_t)r1 << 6);
}

static uint64_t dep_z(unsigned r1, unsigned r2, unsigned pos, unsigned len)
{
    return (UINT64_C(5) << 37) | (UINT64_C(1) << 34) | (UINT64_C(1) << 33) |
           ((uint64_t)(len - 1) << 27) | ((uint64_t)(63 - pos) << 20) |
           ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

/* I5 shr, shr.u (value r3, count r2) and I7 shl (value r2, count r3). */
static uint64_t shift_var(int kind, unsigned r1, unsigned val, unsigned cnt)
{
    uint64_t insn = (UINT64_C(7) << 37) | (UINT64_C(1) << 36) |
                    (UINT64_C(1) << 33) | ((uint64_t)r1 << 6);

    if (kind == 0) {
        return insn | (UINT64_C(1) << 30) | ((uint64_t)cnt << 20) |
               ((uint64_t)val << 13);
    }
    return insn | ((uint64_t)(kind == 2 ? 2 : 0) << 28) |
           ((uint64_t)val << 20) | ((uint64_t)cnt << 13);
}

/* A9 pcmp: size 1, 2, 4 and relation eq (x2b 0) or gt (x2b 1). */
static uint64_t pcmp(unsigned size, bool gt, unsigned r1, unsigned r2,
                     unsigned r3)
{
    return (UINT64_C(8) << 37) | ((uint64_t)(size == 4) << 36) |
           (UINT64_C(1) << 34) | ((uint64_t)(size == 2) << 33) |
           (UINT64_C(9) << 29) | ((uint64_t)gt << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

static uint64_t mux_i(bool two, unsigned r1, unsigned r2, unsigned field)
{
    return (UINT64_C(7) << 37) | (UINT64_C(3) << 34) |
           ((uint64_t)two << 33) | (UINT64_C(2) << 30) | (UINT64_C(2) << 28) |
           ((uint64_t)field << 20) | ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

static uint64_t addp4_rr(unsigned r1, unsigned r2, unsigned r3)
{
    return (UINT64_C(8) << 37) | (UINT64_C(2) << 29) | ((uint64_t)r3 << 20) |
           ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

/* I9 clz (x2c 3) and I2 mpy4 (x2b 1) / mpyshl4 (x2b 3). */
static uint64_t i9_clz(unsigned r1, unsigned r3)
{
    return (UINT64_C(7) << 37) | (UINT64_C(1) << 36) | (UINT64_C(1) << 34) |
           (UINT64_C(1) << 33) | (UINT64_C(3) << 28) | (UINT64_C(3) << 30) |
           ((uint64_t)r3 << 20) | ((uint64_t)r1 << 6);
}

static uint64_t i2_mpy(bool shl, unsigned r1, unsigned r2, unsigned r3)
{
    return (UINT64_C(7) << 37) | (UINT64_C(1) << 36) | (UINT64_C(3) << 30) |
           ((uint64_t)(shl ? 3 : 1) << 28) | ((uint64_t)r3 << 20) |
           ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

typedef uint64_t (*Enc)(unsigned r1, unsigned a, unsigned b, unsigned arg);

static uint64_t e_shl_imm(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return dep_z(d, a, n, 64 - n);
}

static uint64_t e_shru_imm(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return extr_i(false, d, a, n, 64 - n);
}

static uint64_t e_shr_imm(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return extr_i(true, d, a, n, 64 - n);
}

static uint64_t e_shl(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return shift_var(0, d, a, b);
}

static uint64_t e_shru(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return shift_var(1, d, a, b);
}

static uint64_t e_shr(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return shift_var(2, d, a, b);
}

static uint64_t e_mux1(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return mux_i(false, d, a, n);
}

static uint64_t e_mux2(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return mux_i(true, d, a, n);
}

#define E_PCMP(name, size, gt) \
    static uint64_t name(unsigned d, unsigned a, unsigned b, unsigned n) \
    { return pcmp(size, gt, d, a, b); }
E_PCMP(e_pcmp1_eq, 1, false)
E_PCMP(e_pcmp1_gt, 1, true)
E_PCMP(e_pcmp2_eq, 2, false)
E_PCMP(e_pcmp2_gt, 2, true)
E_PCMP(e_pcmp4_eq, 4, false)
E_PCMP(e_pcmp4_gt, 4, true)

static uint64_t e_addp4(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return addp4_rr(d, a, b);
}

static uint64_t e_addp4_imm(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return adds(d, (int)n, b) | (UINT64_C(1) << 34);
}

static uint64_t e_clz(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return i9_clz(d, a);
}

static uint64_t e_mpy4(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return i2_mpy(false, d, a, b);
}

static uint64_t e_mpyshl4(unsigned d, unsigned a, unsigned b, unsigned n)
{
    return i2_mpy(true, d, a, b);
}

enum { NAT_NONE, NAT_A, NAT_B, FAULT };

typedef struct AluRow {
    const char *name;
    Enc enc;
    char unit;
    unsigned arg;
    uint64_t a, b;
    int special;        /* NAT_A, NAT_B: that source is a NaT; FAULT */
    uint64_t want;      /* NAT_MARK when the result must be a NaT */
} AluRow;

/* Run each row as r11 = op(r8, r9); a faulting row leaves r11 as 0x5a5a. */
static void run_alu_rows(const AluRow *rows, unsigned n)
{
    Prog *p = prog_new();

    for (unsigned i = 0; i < n; i++) {
        const AluRow *r = &rows[i];

        movi(p, 8, r->a);
        movi(p, 9, r->b);
        movi(p, 11, 0x5a5a);
        if (r->special == NAT_A) {
            emit(p, 'M', adds(8, 0, 30));
        } else if (r->special == NAT_B) {
            emit(p, 'M', adds(9, 0, 30));
        }
        emit(p, r->unit, r->enc(11, 8, 9, r->arg));
        if (r->special == FAULT) {
            expect_fault(p, r->unit == 'I');
        }
        store_gr(p, 11, r->want, "%s a=%" PRIx64 " b=%" PRIx64 " nat/fault=%d",
                 r->name, r->a, r->b, r->special);
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* shl/shr count6 are dep.z and extr (Vol. 3 pp. 3:54, 3:244, 3:247). */
static void test_shift_immediate_and_variable(void)
{
    static const AluRow rows[] = {
        { "shl imm 0", e_shl_imm, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shl imm 4", e_shl_imm, 'I', 4, 0x8123456789abcdefULL, 0, 0,
          0x123456789abcdef0ULL },
        { "shl imm 63", e_shl_imm, 'I', 63, 0x8123456789abcdefULL, 0, 0,
          0x8000000000000000ULL },
        { "shl imm nat", e_shl_imm, 'I', 4, 1, 0, NAT_A, NAT_MARK },
        { "shr.u imm 0", e_shru_imm, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shr.u imm 4", e_shru_imm, 'I', 4, 0x8123456789abcdefULL, 0, 0,
          0x08123456789abcdeULL },
        { "shr.u imm 63", e_shru_imm, 'I', 63, 0x8123456789abcdefULL, 0, 0, 1 },
        { "shr.u imm nat", e_shru_imm, 'I', 4, 1, 0, NAT_A, NAT_MARK },
        { "shr imm 0", e_shr_imm, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shr imm 4", e_shr_imm, 'I', 4, 0x8123456789abcdefULL, 0, 0,
          0xf8123456789abcdeULL },
        { "shr imm 63", e_shr_imm, 'I', 63, 0x8123456789abcdefULL, 0, 0,
          UINT64_MAX },
        { "shr imm 63 pos", e_shr_imm, 'I', 63, 0x7123456789abcdefULL, 0, 0,
          0 },
        { "shr imm nat", e_shr_imm, 'I', 4, 1, 0, NAT_A, NAT_MARK },
        { "shl 0", e_shl, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shl 63", e_shl, 'I', 63, 0x8123456789abcdefULL, 63, 0,
          0x8000000000000000ULL },
        { "shl 64", e_shl, 'I', 0, 0x8123456789abcdefULL, 64, 0, 0 },
        { "shl 200", e_shl, 'I', 0, 0x8123456789abcdefULL, 200, 0, 0 },
        { "shl nat count", e_shl, 'I', 0, 1, 1, NAT_B, NAT_MARK },
        { "shl nat value", e_shl, 'I', 0, 1, 1, NAT_A, NAT_MARK },
        { "shr.u 0", e_shru, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shr.u 63", e_shru, 'I', 0, 0x8123456789abcdefULL, 63, 0, 1 },
        { "shr.u 64", e_shru, 'I', 0, 0x8123456789abcdefULL, 64, 0, 0 },
        { "shr.u 200", e_shru, 'I', 0, 0x8123456789abcdefULL, 200, 0, 0 },
        { "shr.u nat", e_shru, 'I', 0, 1, 1, NAT_B, NAT_MARK },
        { "shr 0", e_shr, 'I', 0, 0x8123456789abcdefULL, 0, 0,
          0x8123456789abcdefULL },
        { "shr 63", e_shr, 'I', 0, 0x8123456789abcdefULL, 63, 0, UINT64_MAX },
        { "shr 64 neg", e_shr, 'I', 0, 0x8123456789abcdefULL, 64, 0,
          UINT64_MAX },
        { "shr 200 neg", e_shr, 'I', 0, 0x8123456789abcdefULL, 200, 0,
          UINT64_MAX },
        { "shr 64 pos", e_shr, 'I', 0, 0x7123456789abcdefULL, 64, 0, 0 },
        { "shr nat", e_shr, 'I', 0, 1, 1, NAT_A, NAT_MARK },
    };

    run_alu_rows(rows, G_N_ELEMENTS(rows));
}

/* mux1/mux2 (Vol. 3 p. 3:191), pcmp (3:206), addp4 (3:15); bytes 01..08. */
static void test_mux_pcmp_addp4(void)
{
    static const AluRow rows[] = {
        { "mux1 @rev", e_mux1, 'I', 0xb, 0x0807060504030201ULL, 0, 0,
          0x0102030405060708ULL },
        { "mux1 @mix", e_mux1, 'I', 8, 0x0807060504030201ULL, 0, 0,
          0x0804060207030501ULL },
        { "mux1 @shuf", e_mux1, 'I', 9, 0x0807060504030201ULL, 0, 0,
          0x0804070306020501ULL },
        { "mux1 @alt", e_mux1, 'I', 0xa, 0x0807060504030201ULL, 0, 0,
          0x0806040207050301ULL },
        { "mux1 @brcst", e_mux1, 'I', 0, 0x0807060504030201ULL, 0, 0,
          0x0101010101010101ULL },
        { "mux1 nat", e_mux1, 'I', 0xb, 1, 0, NAT_A, NAT_MARK },
        { "mux2 identity", e_mux2, 'I', 0xe4, 0x4444333322221111ULL, 0, 0,
          0x4444333322221111ULL },
        { "mux2 reverse", e_mux2, 'I', 0x1b, 0x4444333322221111ULL, 0, 0,
          0x1111222233334444ULL },
        { "mux2 shuffle", e_mux2, 'I', 0x8d, 0x4444333322221111ULL, 0, 0,
          0x3333111144442222ULL },
        { "mux2 broadcast", e_mux2, 'I', 0xaa, 0x4444333322221111ULL, 0, 0,
          0x3333333333333333ULL },
        { "mux2 nat", e_mux2, 'I', 0x1b, 1, 0, NAT_A, NAT_MARK },
        /* Bytes: 01 02 03 04 80 ff 00 7f against 01 01 04 03 7f ff 80 7f. */
        { "pcmp1.eq", e_pcmp1_eq, 'M', 0, 0x0102030480ff007fULL,
          0x010104037fff807fULL, 0, 0xff00000000ff00ffULL },
        { "pcmp1.gt", e_pcmp1_gt, 'M', 0, 0x0102030480ff007fULL,
          0x010104037fff807fULL, 0, 0x00ff00ff0000ff00ULL },
        /* Halves: 8000 0001 ffff 0000 against 7fff 0001 0000 8000. */
        { "pcmp2.eq", e_pcmp2_eq, 'M', 0, 0x80000001ffff0000ULL,
          0x7fff000100008000ULL, 0, 0x0000ffff00000000ULL },
        { "pcmp2.gt", e_pcmp2_gt, 'M', 0, 0x80000001ffff0000ULL,
          0x7fff000100008000ULL, 0, 0x000000000000ffffULL },
        { "pcmp4.eq", e_pcmp4_eq, 'M', 0, 0x8000000000000005ULL,
          0x7fffffff00000005ULL, 0, 0x00000000ffffffffULL },
        { "pcmp4.gt", e_pcmp4_gt, 'M', 0, 0x00000006ffffffffULL,
          0x0000000500000000ULL, 0, 0xffffffff00000000ULL },
        { "pcmp4.gt signed", e_pcmp4_gt, 'M', 0, 0x8000000000000000ULL,
          0x7fffffff00000000ULL, 0, 0 },
        { "pcmp2.eq nat", e_pcmp2_eq, 'M', 0, 1, 1, NAT_B, NAT_MARK },
        /* addp4: low 32 bits of the sum, bits 31:30 of r3 land in 62:61. */
        { "addp4 carry", e_addp4, 'M', 0, 1, 0x7fffffff, 0,
          0x2000000080000000ULL },
        { "addp4 both bits", e_addp4, 'M', 0, 0x10, 0xc0000000, 0,
          0x60000000c0000010ULL },
        { "addp4 upper ignored", e_addp4, 'M', 0, 0xffffffff00000001ULL, 1, 0,
          2 },
        { "addp4 nat", e_addp4, 'M', 0, 1, 1, NAT_A, NAT_MARK },
        { "addp4 imm", e_addp4_imm, 'M', (unsigned)-1, 0, 0x40000001, 0,
          0x2000000040000000ULL },
        { "addp4 imm nat", e_addp4_imm, 'M', 5, 0, 1, NAT_B, NAT_MARK },
    };

    run_alu_rows(rows, G_N_ELEMENTS(rows));
}

/* clz, mpy4, mpyshl4 need CPUID[4] bits 32/33 or fault (Vol. 3 p. 3:38). */
static void test_clz_and_mpy4_are_not_implemented(void)
{
    static const AluRow rows[] = {
        { "clz", e_clz, 'I', 0, 1, 0, FAULT, 0x5a5a },
        { "mpy4", e_mpy4, 'I', 0, 3, 5, FAULT, 0x5a5a },
        { "mpyshl4", e_mpyshl4, 'I', 0, 3, 5, FAULT, 0x5a5a },
    };

    run_alu_rows(rows, G_N_ELEMENTS(rows));
}

/* F-unit and FR/GR transfer encodings: Vol. 3 4.6 and 4.4.4. */
#define PAIR(hi, lo) (((uint64_t)(hi) << 32) | (lo))
#define FPSR_MASKED 0x60033f    /* all traps masked, sf0/sf1 pc 3, rc nearest */

static uint64_t f1_op(unsigned opc, unsigned f1, unsigned f3, unsigned f4,
                      unsigned f2, unsigned sf)
{
    return ((uint64_t)opc << 37) | (UINT64_C(1) << 36) | ((uint64_t)sf << 34) |
           ((uint64_t)f4 << 27) | ((uint64_t)f3 << 20) | ((uint64_t)f2 << 13) |
           ((uint64_t)f1 << 6);
}

/* F8, F9 and F10 share a layout: opcode 0/1, sf, x6, f3, f2, f1. */
static uint64_t f_misc(unsigned opc, unsigned sf, unsigned x6, unsigned f1,
                       unsigned f2, unsigned f3)
{
    return ((uint64_t)opc << 37) | ((uint64_t)sf << 34) |
           ((uint64_t)x6 << 27) | ((uint64_t)f3 << 20) |
           ((uint64_t)f2 << 13) | ((uint64_t)f1 << 6);
}

static uint64_t getf(unsigned x6, unsigned r1, unsigned f2)
{
    return (UINT64_C(4) << 37) | (UINT64_C(1) << 27) | ((uint64_t)x6 << 30) |
           ((uint64_t)f2 << 13) | ((uint64_t)r1 << 6);
}

static uint64_t setf(unsigned x6, unsigned f1, unsigned r2)
{
    return (UINT64_C(6) << 37) | (UINT64_C(1) << 27) | ((uint64_t)x6 << 30) |
           ((uint64_t)r2 << 13) | ((uint64_t)f1 << 6);
}

enum { X_SIG = 0x1c, X_EXP, X_S, X_D };     /* M18 and M19 x6 */

typedef uint64_t (*FEnc)(unsigned f1, unsigned a, unsigned b, unsigned c);

static uint64_t e_fpma(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f1_op(9, d, a, b, c, 0);
}

static uint64_t e_fpms(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f1_op(0xb, d, a, b, c, 0);
}

static uint64_t e_fpnma(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f1_op(0xd, d, a, b, c, 0);
}

#define E_F8(name, opc, x6) \
    static uint64_t name(unsigned d, unsigned a, unsigned b, unsigned c) \
    { return f_misc(opc, 0, x6, d, a, b); }
E_F8(e_fpmin, 1, 0x14)
E_F8(e_fpmax, 1, 0x15)
E_F8(e_fpamin, 1, 0x16)
E_F8(e_fpamax, 1, 0x17)
E_F8(e_fpmerge_s, 1, 0x10)
E_F8(e_fpmerge_ns, 1, 0x11)
E_F8(e_fpmerge_se, 1, 0x12)
E_F8(e_fswap, 0, 0x34)
E_F8(e_fswap_nl, 0, 0x35)
E_F8(e_fswap_nr, 0, 0x36)

/* fpabs, fpneg, fpnegabs and fmov are pseudo-ops of the merge forms. */
static uint64_t e_fpabs(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f_misc(1, 0, 0x10, d, 0, a);
}

static uint64_t e_fpneg(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f_misc(1, 0, 0x11, d, a, a);
}

static uint64_t e_fpnegabs(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f_misc(1, 0, 0x11, d, 0, a);
}

static uint64_t e_fmov(unsigned d, unsigned a, unsigned b, unsigned c)
{
    return f_misc(0, 0, 0x10, d, a, a);
}

#define E_F10(name, x6) \
    static uint64_t name(unsigned d, unsigned a, unsigned b, unsigned c) \
    { return f_misc(1, 0, x6, d, a, 0); }
E_F10(e_fpcvt_fx, 0x18)
E_F10(e_fpcvt_fxu, 0x19)
E_F10(e_fpcvt_fx_trunc, 0x1a)
E_F10(e_fpcvt_fxu_trunc, 0x1b)

typedef struct FpRow {
    const char *name;
    FEnc enc;
    uint64_t a, b, c;   /* f6, f7, f8 as significands set by setf.sig */
    int nat;            /* bit 0: f6 is a NaTVal, bit 1: f7 */
    uint64_t want;      /* significand of f9, or NAT_MARK for a NaTVal */
} FpRow;

static void fp_init(Prog *p)
{
    movi(p, 12, FPSR_MASKED);
    emit(p, 'M', m_to_ar(40, 12));
}

/* f9 = op(f6, f7, f8); getf.sig returns the result's significand. */
static void run_fp_rows(const FpRow *rows, unsigned n)
{
    Prog *p = prog_new();

    fp_init(p);
    for (unsigned i = 0; i < n; i++) {
        const FpRow *r = &rows[i];

        movi(p, 8, r->a);
        movi(p, 9, r->b);
        movi(p, 12, r->c);
        if (r->nat & 1) {
            emit(p, 'M', adds(8, 0, 30));
        }
        if (r->nat & 2) {
            emit(p, 'M', adds(9, 0, 30));
        }
        emit(p, 'M', setf(X_SIG, 6, 8));
        emit(p, 'M', setf(X_SIG, 7, 9));
        emit(p, 'M', setf(X_SIG, 8, 12));
        emit(p, 'F', r->enc(9, 6, 7, 8));
        emit(p, 'M', getf(X_SIG, 11, 9));
        store_gr(p, 11, r->want, "%s a=%" PRIx64 " b=%" PRIx64 " c=%" PRIx64,
                 r->name, r->a, r->b, r->c);
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* fpma, fpms, fpnma (Vol. 3 p. 3:107) and fpmin/max/amin/amax (3:98-3:110). */
static void test_fp_parallel_arithmetic(void)
{
    static const FpRow rows[] = {
        { "fpma", e_fpma, PAIR(0x3f800000, 0x40000000),
          PAIR(0x40400000, 0x40800000), PAIR(0x3f000000, 0x3f800000), 0,
          PAIR(0x40600000, 0x41100000) },
        { "fpms", e_fpms, PAIR(0x3f800000, 0x40000000),
          PAIR(0x40400000, 0x40800000), PAIR(0x3f000000, 0x3f800000), 0,
          PAIR(0x40200000, 0x40e00000) },
        { "fpnma", e_fpnma, PAIR(0x3f800000, 0x40000000),
          PAIR(0x40400000, 0x40800000), PAIR(0x3f000000, 0x3f800000), 0,
          PAIR(0xc0200000, 0xc0e00000) },
        { "fpma natval", e_fpma, 1, 1, 1, 1, NAT_MARK },
        { "fpms natval addend", e_fpms, 1, 1, 1, 2, NAT_MARK },
        /* {1, -3} against {2, -2}: high 1 < 2, low -3 < -2. */
        { "fpmax", e_fpmax, PAIR(0x3f800000, 0xc0400000),
          PAIR(0x40000000, 0xc0000000), 0, 0, PAIR(0x40000000, 0xc0000000) },
        { "fpmin", e_fpmin, PAIR(0x3f800000, 0xc0400000),
          PAIR(0x40000000, 0xc0000000), 0, 0, PAIR(0x3f800000, 0xc0400000) },
        /* |-3| > |-2| on the low half, so fpamax keeps f2 there. */
        { "fpamax", e_fpamax, PAIR(0x3f800000, 0xc0400000),
          PAIR(0x40000000, 0xc0000000), 0, 0, PAIR(0x40000000, 0xc0400000) },
        { "fpamin", e_fpamin, PAIR(0x3f800000, 0xc0400000),
          PAIR(0x40000000, 0xc0000000), 0, 0, PAIR(0x3f800000, 0xc0000000) },
        /* A NaN in f2 makes the result take f3 for that half. */
        { "fpmax nan", e_fpmax, PAIR(0x7fc00000, 0x3f800000),
          PAIR(0x40a00000, 0x40000000), 0, 0, PAIR(0x40a00000, 0x40000000) },
        { "fpmin nan", e_fpmin, PAIR(0x7fc00000, 0x3f800000),
          PAIR(0x40a00000, 0x40000000), 0, 0, PAIR(0x40a00000, 0x3f800000) },
        { "fpmax natval", e_fpmax, 1, 1, 0, 2, NAT_MARK },
        { "fpamin natval", e_fpamin, 1, 1, 0, 1, NAT_MARK },
    };

    run_fp_rows(rows, G_N_ELEMENTS(rows));
}

/* fpmerge and pseudo-ops (Vol. 3 p. 3:111), fswap (3:137), fmov (fmerge.s). */
static void test_fp_merge_and_swap(void)
{
    static const FpRow rows[] = {
        { "fpmerge.s", e_fpmerge_s, PAIR(0xc0000000, 0x40000000),
          PAIR(0x3f800000, 0xc0400000), 0, 0, PAIR(0xbf800000, 0x40400000) },
        { "fpmerge.ns", e_fpmerge_ns, PAIR(0xc0000000, 0x40000000),
          PAIR(0x3f800000, 0xc0400000), 0, 0, PAIR(0x3f800000, 0xc0400000) },
        { "fpmerge.se", e_fpmerge_se, PAIR(0xc0000000, 0x40000000),
          PAIR(0x3f800000, 0xc0400000), 0, 0, PAIR(0xc0000000, 0x40400000) },
        { "fpmerge natval", e_fpmerge_s, 1, 1, 0, 1, NAT_MARK },
        { "fpabs", e_fpabs, PAIR(0xbf800000, 0xc0400000), 0, 0, 0,
          PAIR(0x3f800000, 0x40400000) },
        { "fpneg", e_fpneg, PAIR(0xbf800000, 0x40400000), 0, 0, 0,
          PAIR(0x3f800000, 0xc0400000) },
        { "fpnegabs", e_fpnegabs, PAIR(0x3f800000, 0xc0400000), 0, 0, 0,
          PAIR(0xbf800000, 0xc0400000) },
        { "fpabs natval", e_fpabs, 1, 0, 0, 1, NAT_MARK },
        /* The swapped pair is {f3.low, f2.high}; nl and nr flip one sign. */
        { "fswap", e_fswap, PAIR(0x11111111, 0x22222222),
          PAIR(0x33333333, 0x44444444), 0, 0, PAIR(0x44444444, 0x11111111) },
        { "fswap.nl", e_fswap_nl, PAIR(0x11111111, 0x22222222),
          PAIR(0x33333333, 0x44444444), 0, 0, PAIR(0xc4444444, 0x11111111) },
        { "fswap.nr", e_fswap_nr, PAIR(0x11111111, 0x22222222),
          PAIR(0x33333333, 0x44444444), 0, 0, PAIR(0x44444444, 0x91111111) },
        { "fswap natval", e_fswap, 1, 1, 0, 2, NAT_MARK },
        /* fmov keeps the whole register. */
        { "fmov", e_fmov, 0x8123456789abcdefULL, 0, 0, 0,
          0x8123456789abcdefULL },
        { "fmov natval", e_fmov, 1, 0, 0, 1, NAT_MARK },
    };

    run_fp_rows(rows, G_N_ELEMENTS(rows));
}

/* fpcvt (Vol. 3 p. 3:104): nearest even or truncate; 2^31 is invalid. */
static void test_fp_convert_parallel(void)
{
    static const FpRow rows[] = {
        /* {2.5, -1.5} */
        { "fpcvt.fx", e_fpcvt_fx, PAIR(0x40200000, 0xbfc00000), 0, 0, 0,
          PAIR(2, 0xfffffffe) },
        { "fpcvt.fx.trunc", e_fpcvt_fx_trunc, PAIR(0x40200000, 0xbfc00000),
          0, 0, 0, PAIR(2, 0xffffffff) },
        /* {3.5, 1.5} */
        { "fpcvt.fxu", e_fpcvt_fxu, PAIR(0x40600000, 0x3fc00000), 0, 0, 0,
          PAIR(4, 2) },
        { "fpcvt.fxu.trunc", e_fpcvt_fxu_trunc, PAIR(0x40600000, 0x3fc00000),
          0, 0, 0, PAIR(3, 1) },
        /* {2^31, 1.0} */
        { "fpcvt.fx range", e_fpcvt_fx, PAIR(0x4f000000, 0x3f800000), 0, 0, 0,
          PAIR(0x80000000, 1) },
        { "fpcvt.fxu natval", e_fpcvt_fxu, 1, 0, 0, 1, NAT_MARK },
    };

    run_fp_rows(rows, G_N_ELEMENTS(rows));
}

/* setf/getf (Vol. 3 pp. 3:143, 3:242); FR exponent bias is 65535. */
static void test_getf_setf_formats(void)
{
    static const struct { const char *name; uint64_t v; unsigned set, get; }
    xfers[] = {
        { "2.5f", 0x40200000, X_S, X_S }, { "-2.5f", 0xc0200000, X_S, X_S },
        { "0.0f", 0, X_S, X_S }, { "-0.0f", 0x80000000, X_S, X_S },
        { "inf", 0x7f800000, X_S, X_S },
        { "2.5d", 0x4004000000000000ULL, X_D, X_D },
        { "-2.5d", 0xc004000000000000ULL, X_D, X_D },
    };
    static const struct { uint64_t sig, exp; } fr[] = {
        { 0xa000000000000000ULL, 0x10000 }, { 0xa000000000000000ULL, 0x30000 },
        { 0, 0 }, { 0, 0x20000 }, { 0x8000000000000000ULL, 0x1ffff },
        { 0xa000000000000000ULL, 0x10000 }, { 0xa000000000000000ULL, 0x30000 },
    };
    Prog *p = prog_new();

    for (unsigned i = 0; i < G_N_ELEMENTS(xfers); i++) {
        movi(p, 8, xfers[i].v);
        emit(p, 'M', setf(xfers[i].set, 6, 8));
        emit(p, 'M', getf(X_SIG, 11, 6));
        store_gr(p, 11, fr[i].sig, "setf %s sig", xfers[i].name);
        emit(p, 'M', getf(X_EXP, 11, 6));
        store_gr(p, 11, fr[i].exp, "setf %s exp", xfers[i].name);
        emit(p, 'M', getf(xfers[i].get, 11, 6));
        store_gr(p, 11, xfers[i].v, "getf %s round trip", xfers[i].name);
    }
    /* getf.d of a single-built 2.5, and getf.s of a double-built 2.5. */
    movi(p, 8, 0x40200000);
    emit(p, 'M', setf(X_S, 6, 8));
    emit(p, 'M', getf(X_D, 11, 6));
    store_gr(p, 11, 0x4004000000000000ULL, "getf.d of 2.5f");
    movi(p, 8, 0x4004000000000000ULL);
    emit(p, 'M', setf(X_D, 6, 8));
    emit(p, 'M', getf(X_S, 11, 6));
    store_gr(p, 11, 0x40200000, "getf.s of 2.5d");
    /* setf.exp builds 1.0 * 2^(exp - 65535): significand is 1 << 63. */
    movi(p, 8, 0x20000 | 0xffff);
    emit(p, 'M', setf(X_EXP, 6, 8));
    emit(p, 'M', getf(X_SIG, 11, 6));
    store_gr(p, 11, 0x8000000000000000ULL, "setf.exp sig");
    emit(p, 'M', getf(X_EXP, 11, 6));
    store_gr(p, 11, 0x2ffff, "setf.exp exp");
    /* A NaT source makes a NaTVal, and getf of a NaTVal sets the NaT bit. */
    for (unsigned x = X_SIG; x <= X_D; x++) {
        emit(p, 'M', adds(8, 0, 30));
        emit(p, 'M', setf(x, 6, 8));
        emit(p, 'M', getf(X_SIG + (x - X_SIG), 11, 6));
        store_gr(p, 11, NAT_MARK, "setf/getf natval x6=0x%x", x);
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

static float half_f(uint64_t sig, bool hi)
{
    uint32_t bits = hi ? sig >> 32 : (uint32_t)sig;
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Relative error of an approximation against the exact answer, per half. */
static unsigned approx_bad(const char *what, uint64_t got, const double want[2],
                           double bound)
{
    unsigned bad = 0;

    for (unsigned h = 0; h < 2; h++) {
        double r = half_f(got, h == 0);

        if (!(r / want[h] - 1 < bound && 1 - r / want[h] < bound)) {
            g_test_message("%s half %u: %g is not within %g of %g", what, h, r,
                           bound, want[h]);
            bad++;
        }
    }
    return bad;
}

static unsigned recip_post(const uint64_t *got, void *ctx)
{
    /* 1 / {3.0, 4.0} and 1 / sqrt({4.0, 16.0}). */
    static const double rcp[2] = { 1.0 / 3.0, 0.25 }, rsq[2] = { 0.5, 0.25 };

    return approx_bad("fprcpa", got[0], rcp, 0.002114) +
           approx_bad("fprsqrta", got[2], rsq, 0.002196);
}

static uint64_t fprcpa_insn(unsigned f1, unsigned p2, unsigned f2, unsigned f3)
{
    return (UINT64_C(1) << 37) | (UINT64_C(1) << 33) | ((uint64_t)p2 << 27) |
           ((uint64_t)f3 << 20) | ((uint64_t)f2 << 13) | ((uint64_t)f1 << 6);
}

static uint64_t fprsqrta_insn(unsigned f1, unsigned p2, unsigned f3)
{
    return (UINT64_C(1) << 37) | (UINT64_C(1) << 36) | (UINT64_C(1) << 33) |
           ((uint64_t)p2 << 27) | ((uint64_t)f3 << 20) | ((uint64_t)f1 << 6);
}

/* fprcpa, fprsqrta (Vol. 3 pp. 3:122, 3:128): error under 2^-8.886/-8.831 */
static void test_fp_reciprocal_approximations(void)
{
    Prog *p = prog_new();

    fp_init(p);
    movi(p, 8, PAIR(0x3f800000, 0x3f800000));       /* {1.0, 1.0} */
    movi(p, 9, PAIR(0x40400000, 0x40800000));       /* {3.0, 4.0} */
    movi(p, 13, 0x40);                              /* p6 set */
    emit(p, 'M', setf(X_SIG, 6, 8));
    emit(p, 'M', setf(X_SIG, 7, 9));
    emit(p, 'F', fprcpa_insn(9, 6, 6, 7));
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, ANY, "fprcpa approximation");
    store_pr(p, 1 | (1 << 6), "fprcpa p2");
    movi(p, 9, PAIR(0x40800000, 0x41800000));       /* {4.0, 16.0} */
    emit(p, 'M', setf(X_SIG, 7, 9));
    emit(p, 'F', fprsqrta_insn(9, 6, 7));
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, ANY, "fprsqrta approximation");
    store_pr(p, 1 | (1 << 6), "fprsqrta p2");
    /* 1.0 / {0.0, 0.0} is {+inf, +inf} and p2 is written as 0. */
    emit(p, 'M', setf(X_SIG, 7, 0));
    emit(p, 'F', fprcpa_insn(9, 6, 6, 7));
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, PAIR(0x7f800000, 0x7f800000), "fprcpa x/0");
    store_pr(p, 1, "fprcpa x/0 p2");
    /* A NaTVal source gives NaTVal and clears p2, even when it was set. */
    emit(p, 'M', adds(8, 0, 30));
    emit(p, 'M', setf(X_SIG, 6, 8));
    emit(p, 'I', mov_to_pr(13, 0x40));
    emit(p, 'F', fprcpa_insn(9, 6, 6, 7));
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, NAT_MARK, "fprcpa natval");
    store_pr(p, 1, "fprcpa natval p2");
    emit(p, 'I', mov_to_pr(13, 0x40));
    emit(p, 'F', fprsqrta_insn(9, 6, 6));
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, NAT_MARK, "fprsqrta natval");
    store_pr(p, 1, "fprsqrta natval p2");
    g_assert_cmpuint(prog_check_post(p, recip_post, NULL), ==, 0);
}

/* fclrf, fchkf (Vol. 3 pp. 3:63, 3:66); Itanium 2 takes the fchkf fault. */
static void test_fclrf_and_fchkf(void)
{
    const uint64_t s0_inexact = UINT64_C(1) << 18;
    const uint64_t s1_inexact = UINT64_C(1) << 31;
    Prog *p = prog_new();

    fp_init(p);
    movi(p, 8, PAIR(0x40200000, 0xbfc00000));
    emit(p, 'M', setf(X_SIG, 6, 8));
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED, "fpsr after init");
    emit(p, 'F', f_misc(1, 1, 0x18, 9, 6, 0));          /* fpcvt.fx.s1 */
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED | s1_inexact, "s1 inexact flag");
    emit(p, 'F', (UINT64_C(1) << 34) | (UINT64_C(5) << 27));    /* fclrf.s1 */
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED, "fclrf.s1");
    /* A false qp leaves the flags alone. */
    emit(p, 'F', f_misc(1, 1, 0x18, 9, 6, 0));
    emit(p, 'F', ((UINT64_C(1) << 34) | (UINT64_C(5) << 27)) | 5);
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED | s1_inexact, "(p5) fclrf.s1");
    /* s1 flag with s0 clear: fchkf.s1 takes the Speculation fault. */
    emit(p, 'F', (UINT64_C(1) << 34) | (UINT64_C(8) << 27) | (2 << 6));
    expect_spec_fault(p, 1, 4, 2);
    /* The same flag also in s0 is not a failure, nor is a clean s1. */
    emit(p, 'F', f_misc(1, 0, 0x18, 9, 6, 0));          /* fpcvt.fx.s0 */
    emit(p, 'F', (UINT64_C(1) << 34) | (UINT64_C(8) << 27) | (2 << 6));
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED | s1_inexact | s0_inexact, "s0 and s1 flags");
    emit(p, 'F', (UINT64_C(5) << 27));                  /* fclrf.s0 */
    emit(p, 'F', (UINT64_C(1) << 34) | (UINT64_C(5) << 27));
    emit(p, 'F', (UINT64_C(1) << 34) | (UINT64_C(8) << 27) | (2 << 6));
    emit(p, 'M', m_from_ar(11, 40));
    store_gr(p, 11, FPSR_MASKED, "both cleared");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* M2/M3 loads, M6 stores, M11/M12 pair loads, M16 semaphores (Vol. 3 4.4). */
#define DATA_A  0x140000
#define DATA_B  0x140020

static uint64_t m_x1(unsigned x6, unsigned r3, unsigned r2, unsigned r1)
{
    return (UINT64_C(4) << 37) | ((uint64_t)x6 << 30) | (UINT64_C(1) << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

static uint64_t ldfp(unsigned x6, bool post, unsigned f1, unsigned f2,
                     unsigned r3)
{
    return (UINT64_C(6) << 37) | ((uint64_t)post << 36) |
           ((uint64_t)x6 << 30) | (UINT64_C(1) << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)f2 << 13) | ((uint64_t)f1 << 6);
}

/* lfetch (x6 0x2c + lftype bits) with no update, or +r2, or +imm9. */
static uint64_t lfetch(unsigned x6, char form, unsigned r3, int inc)
{
    switch (form) {
    case 'r':
        return (UINT64_C(6) << 37) | (UINT64_C(1) << 36) |
               ((uint64_t)x6 << 30) | ((uint64_t)r3 << 20) |
               ((uint64_t)inc << 13);
    case 'i':
        return (UINT64_C(7) << 37) | ((uint64_t)((inc >> 8) & 1) << 36) |
               ((uint64_t)x6 << 30) | ((uint64_t)((inc >> 7) & 1) << 27) |
               ((uint64_t)r3 << 20) | ((uint64_t)(inc & 0x7f) << 13);
    default:
        return (UINT64_C(6) << 37) | ((uint64_t)x6 << 30) |
               ((uint64_t)r3 << 20);
    }
}

#define LFETCH 0x2c
#define LFETCH_FAULT 0x2e

#define AR_CSD 25
#define AR_CCV 32

/*
 * ld.a/.s/.sa/.c (Vol. 3 p. 3:151, Vol. 1 4.4.5.3). Physical mode is WBL,
 * where ld.s and ld.sa always fail (Vol. 2 Table 4-13); chk.a shows the ALAT.
 */
static void alat_probe(Prog *p, bool want_entry, const char *what)
{
    emit(p, 'M', chk_a(4, 11));
    if (!want_entry) {
        expect_spec_fault(p, 0, 0, 4);
    }
    store_gr(p, 14, DATA_A, "%s", what);
}

static void test_advanced_and_check_loads(void)
{
    static const uint64_t mask[3] = { 0xff, 0xffff, 0xffffffff };
    Prog *p = prog_new();

    qtest_writeq(p->s, DATA_A, UINT64_C(0x8182838485868788));
    movi(p, 14, DATA_A);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t v = UINT64_C(0x8182838485868788) & mask[i];
        unsigned sz = 1u << i;

        movi(p, 11, 0x5a5a);
        emit(p, 'M', ld8_x(0x00 + i, 11, 14));                      /* ld */
        store_gr(p, 11, v, "ld%u zero-extends", sz);
        emit(p, 'M', ld8_x(0x04 + i, 11, 14));                      /* ld.s */
        store_gr(p, 11, NAT_MARK, "ld%u.s in physical mode", sz);
        emit(p, 'M', ld8_x(0x08 + i, 11, 14));                      /* ld.a */
        store_gr(p, 11, v, "ld%u.a", sz);
        alat_probe(p, true, "ld.a allocates");
        emit(p, 'M', ld8_x(0x20 + i, 11, 14));                      /* c.clr */
        store_gr(p, 11, v, "ld%u.c.clr", sz);
        alat_probe(p, false, "ld.c.clr leaves no entry");
        emit(p, 'M', ld8_x(0x24 + i, 11, 14));                      /* c.nc */
        store_gr(p, 11, v, "ld%u.c.nc miss reloads", sz);
        alat_probe(p, true, "ld.c.nc miss allocates");
        emit(p, 'M', ld8_x(0x24 + i, 11, 14));
        store_gr(p, 11, v, "ld%u.c.nc hit", sz);
        alat_probe(p, true, "ld.c.nc hit keeps the entry");
        emit(p, 'M', ld8_x(0x0c + i, 11, 14));                      /* ld.sa */
        store_gr(p, 11, NAT_MARK, "ld%u.sa in physical mode", sz);
        alat_probe(p, false, "failed ld.sa removes the entry");
    }
    /* Deferral: a NaT base gives a NaT result and removes the entry. */
    emit(p, 'M', adds(16, 0, 30));
    emit(p, 'M', ld8_x(0x06, 11, 16));
    store_gr(p, 11, NAT_MARK, "ld4.s nat base");
    emit(p, 'M', ld8_x(0x0e, 11, 14));
    emit(p, 'M', ld8_x(0x0e, 11, 16));
    store_gr(p, 11, NAT_MARK, "ld4.sa nat base");
    alat_probe(p, false, "deferred ld4.sa leaves no entry");
    movi(p, 11, 0x5a5a);
    emit(p, 'M', ld8_x(0x02, 11, 16));                              /* ld4 */
    expect_fault_code(p, 0, 0x10);
    store_gr(p, 11, 0x5a5a, "ld4 nat base faults");
    /* A base update onto the destination register is an Illegal Operation. */
    emit(p, 'M', (UINT64_C(5) << 37) | (UINT64_C(3) << 30) | (14 << 20) |
                 (8 << 13) | (14 << 6));
    expect_fault(p, 0);
    store_gr(p, 14, DATA_A, "ld8 r14 = [r14], 8 leaves r14");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* M43 mov r1 = cpuid[r3]. */
static uint64_t mov_from_cpuid(unsigned r1, unsigned r3)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x17) << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)r1 << 6);
}

static unsigned cpuid_post(const uint64_t *got, void *ctx)
{
    *(uint64_t *)ctx = got[0];
    return 0;
}

static uint64_t read_cpuid(unsigned idx)
{
    Prog *p = prog_new();
    uint64_t v = 0;

    movi(p, 3, idx);
    emit(p, 'M', mov_from_cpuid(11, 3));
    store_gr(p, 11, ANY, "cpuid[%u]", idx);
    g_assert_cmpuint(prog_check_post(p, cpuid_post, &v), ==, 0);
    return v;
}

/* ld16, st16, cmp8xchg16 (Vol. 3 p. 3:45, 3:151): Illegal Operation if !ao. */
static void test_sixteen_byte_accesses(void)
{
    bool ao = read_cpuid(4) & 4;
    Prog *p = prog_new();

    qtest_writeq(p->s, DATA_A, 0x1111);
    qtest_writeq(p->s, DATA_A + 8, 0x2222);
    movi(p, 14, DATA_A);
    movi(p, 15, DATA_B);
    movi(p, 2, 0x3333);
    movi(p, 3, 0x4444);
    movi(p, 4, 0x1111);
    emit(p, 'M', m_to_ar(AR_CSD, 3));
    emit(p, 'M', m_to_ar(AR_CCV, 4));
    if (!ao) {
        movi(p, 11, 0x5a5a);
        emit(p, 'M', m_x1(0x28, 14, 0, 11));                /* ld16 */
        expect_fault(p, 0);
        emit(p, 'M', m_x1(0x2c, 14, 0, 11));                /* ld16.acq */
        expect_fault(p, 0);
        emit(p, 'M', m_x1(0x30, 15, 2, 0));                 /* st16 */
        expect_fault(p, 0);
        emit(p, 'M', m_x1(0x20, 14, 2, 11));                /* cmp8xchg16 */
        expect_fault(p, 0);
        store_gr(p, 11, 0x5a5a, "ao clear: no instruction wrote r11");
        emit(p, 'M', ld8(11, 14));
        store_gr(p, 11, 0x1111, "ao clear: memory untouched");
        g_assert_cmpuint(prog_check(p), ==, 0);
        return;
    }
    emit(p, 'M', m_x1(0x28, 14, 0, 11));                    /* ld16 */
    store_gr(p, 11, 0x1111, "ld16 low");
    emit(p, 'M', m_from_ar(12, AR_CSD));
    store_gr(p, 12, 0x2222, "ld16 csd");
    emit(p, 'M', m_x1(0x2c, 14, 0, 11));                    /* ld16.acq */
    store_gr(p, 11, 0x1111, "ld16.acq low");
    emit(p, 'M', m_x1(0x30, 15, 2, 0));                     /* st16 */
    emit(p, 'M', ld8(11, 15));
    store_gr(p, 11, 0x3333, "st16 low");
    emit(p, 'M', adds(16, 8, 15));
    emit(p, 'M', ld8(11, 16));
    store_gr(p, 11, 0x2222, "st16 high");
    emit(p, 'M', m_x1(0x20, 14, 2, 11));                    /* cmp8xchg16 */
    store_gr(p, 11, 0x1111, "cmp8xchg16 hit returns old low");
    emit(p, 'M', ld8(11, 14));
    store_gr(p, 11, 0x3333, "cmp8xchg16 hit stores r2");
    emit(p, 'M', adds(16, 8, 14));
    emit(p, 'M', ld8(11, 16));
    store_gr(p, 11, 0x2222, "cmp8xchg16 hit stores csd");
    movi(p, 2, 0x5555);
    emit(p, 'M', m_x1(0x24, 14, 2, 11));                    /* .rel, ccv miss */
    store_gr(p, 11, 0x3333, "cmp8xchg16 miss returns old low");
    emit(p, 'M', ld8(11, 14));
    store_gr(p, 11, 0x3333, "cmp8xchg16 miss writes nothing");
    movi(p, 4, 0x2222);
    emit(p, 'M', m_to_ar(AR_CCV, 4));
    emit(p, 'M', m_x1(0x20, 16, 2, 11));                    /* r3 bit 3 set */
    store_gr(p, 11, 0x2222, "cmp8xchg16 bit 3 returns the word at r3");
    emit(p, 'M', ld8(11, 14));
    store_gr(p, 11, 0x5555, "cmp8xchg16 bit 3 stores low at r3 & ~8");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* ldfp8/ldfpd/ldfps (Vol. 3 p. 3:161): f1 low, f2 high, f1/f2 odd and even. */
static void test_floating_point_pair_loads(void)
{
    Prog *p = prog_new();

    qtest_writeq(p->s, DATA_A, UINT64_C(0x8182838485868788));
    qtest_writeq(p->s, DATA_A + 8, UINT64_C(0x9192939495969798));
    qtest_writeq(p->s, DATA_B, UINT64_C(0x4004000000000000));       /* 2.5 */
    qtest_writeq(p->s, DATA_B + 8, UINT64_C(0xc004000000000000));   /* -2.5 */
    movi(p, 14, DATA_A);
    movi(p, 15, DATA_B);
    emit(p, 'M', ldfp(0x01, false, 6, 7, 14));              /* ldfp8 */
    emit(p, 'M', getf(X_SIG, 11, 6));
    store_gr(p, 11, UINT64_C(0x8182838485868788), "ldfp8 f1");
    emit(p, 'M', getf(X_SIG, 11, 7));
    store_gr(p, 11, UINT64_C(0x9192939495969798), "ldfp8 f2");
    emit(p, 'M', getf(X_EXP, 11, 7));
    store_gr(p, 11, 0x1003e, "ldfp8 integer exponent");
    emit(p, 'M', ldfp(0x03, false, 8, 9, 15));              /* ldfpd */
    emit(p, 'M', getf(X_D, 11, 8));
    store_gr(p, 11, UINT64_C(0x4004000000000000), "ldfpd f1 as double");
    emit(p, 'M', getf(X_EXP, 11, 9));
    store_gr(p, 11, 0x30000, "ldfpd f2 exponent and sign");
    emit(p, 'M', getf(X_SIG, 11, 9));
    store_gr(p, 11, UINT64_C(0xa000000000000000), "ldfpd f2 significand");
    /* The post-increment forms add 16 for ldfp8/ldfpd and 8 for ldfps. */
    emit(p, 'M', ldfp(0x01, true, 6, 7, 14));
    store_gr(p, 14, DATA_A + 16, "ldfp8 [r3], 16");
    movi(p, 14, DATA_A);
    emit(p, 'M', ldfp(0x02, true, 6, 7, 14));
    store_gr(p, 14, DATA_A + 8, "ldfps [r3], 8");
    emit(p, 'M', getf(X_S, 11, 6));
    store_gr(p, 11, 0x85868788, "ldfps f1 as single");
    emit(p, 'M', getf(X_S, 11, 7));
    store_gr(p, 11, 0x81828384, "ldfps f2 as single");
    emit(p, 'M', ldfp(0x01, false, 6, 8, 15));
    expect_fault(p, 0);
    store_gr(p, 14, DATA_A + 8, "ldfp8 f6, f8 leaves r14");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* lfetch (Vol. 3 p. 3:164): only the post-increment shows; .fault checks NaT */
static void test_lfetch_updates_and_faults(void)
{
    Prog *p = prog_new();

    movi(p, 14, DATA_A);
    movi(p, 4, 0x40);
    emit(p, 'M', lfetch(LFETCH, 'n', 14, 0));
    store_gr(p, 14, DATA_A, "lfetch [r3]");
    emit(p, 'M', lfetch(LFETCH, 'r', 14, 4));
    store_gr(p, 14, DATA_A + 0x40, "lfetch [r3], r2");
    emit(p, 'M', lfetch(LFETCH_FAULT, 'r', 14, 4));
    store_gr(p, 14, DATA_A + 0x80, "lfetch.fault [r3], r2");
    emit(p, 'M', lfetch(LFETCH, 'i', 14, -8 & 0x1ff));
    store_gr(p, 14, DATA_A + 0x78, "lfetch [r3], -8");
    emit(p, 'M', lfetch(LFETCH_FAULT, 'i', 14, 0x70));
    store_gr(p, 14, DATA_A + 0xe8, "lfetch.fault [r3], 0x70");
    /* A NaT increment makes the base a NaT. */
    emit(p, 'M', adds(5, 0, 30));
    emit(p, 'M', lfetch(LFETCH, 'r', 14, 5));
    store_gr(p, 14, NAT_MARK, "lfetch [r3], nat");
    /* A NaT base: no fault without .fault, a fault with it. */
    emit(p, 'M', lfetch(LFETCH, 'n', 14, 0));
    emit(p, 'M', lfetch(LFETCH_FAULT, 'n', 14, 0));
    expect_fault_code(p, 0, 0x14);
    movi(p, 11, 0x77);
    store_gr(p, 11, 0x77, "after lfetch.fault nat");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* System and user-level register moves, Vol. 3 4.3.6 and 4.4.7 to 4.4.11. */
static uint64_t m43(unsigned x6, unsigned r1, unsigned r3)
{
    return (UINT64_C(1) << 37) | ((uint64_t)x6 << 27) | ((uint64_t)r3 << 20) |
           ((uint64_t)r1 << 6);
}

static uint64_t mov_to_um(unsigned r2)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x29) << 27) | ((uint64_t)r2 << 13);
}

static uint64_t mov_from_um(unsigned r1)
{
    return (UINT64_C(1) << 37) | (UINT64_C(0x21) << 27) | ((uint64_t)r1 << 6);
}

/* M44 sum (x4 4) and rum (x4 5). */
static uint64_t um_mask_op(unsigned x4, unsigned imm)
{
    return ((uint64_t)x4 << 27) | ((uint64_t)imm << 6);
}

static unsigned bits_post(const uint64_t *got, void *ctx)
{
    unsigned bad = 0;

    /* got: vendor 0, vendor 1, cpuid2, cpuid3, cpuid4 (Vol. 1 3.1.11). */
    bad += (got[3] & 0xff) < 4;
    bad += (got[3] >> 40) != 0;
    bad += ((got[3] >> 24) & 0xff) != 0x1f;     /* Itanium 2 family */
    bad += ((got[3] >> 32) & 0xff) != 0;        /* archrev */
    bad += (got[4] & ~(UINT64_C(0xf) | (UINT64_C(3) << 32))) != 0;
    if (bad) {
        g_test_message("cpuid[3]=0x%" PRIx64 " cpuid[4]=0x%" PRIx64, got[3],
                       got[4]);
    }
    return bad;
}

/* CPUID (Vol. 1 3.1.11): vendor string, [2] zero, field layout, [5] reserved */
static void test_cpuid_registers(void)
{
    Prog *p = prog_new();

    for (unsigned i = 0; i < 5; i++) {
        movi(p, 3, i);
        emit(p, 'M', mov_from_cpuid(11, 3));
        store_gr(p, 11, i == 0 ? UINT64_C(0x49656e69756e6547) :
                 i == 1 ? UINT64_C(0x6c65746e) : i == 2 ? 0 : ANY,
                 "cpuid[%u]", i);
    }
    movi(p, 3, 5);
    movi(p, 11, 0x5a5a);
    emit(p, 'M', mov_from_cpuid(11, 3));
    expect_fault_code(p, 0, 0x30);                      /* reserved register */
    store_gr(p, 11, 0x5a5a, "cpuid[5] faults and writes nothing");
    g_assert_cmpuint(prog_check_post(p, bits_post, NULL), ==, 0);
}

/* mov psr.um, sum, rum (Vol. 3 pp. 3:186, 3:257): bits 5:1 only. */
static void test_user_mask_moves(void)
{
    Prog *p = prog_new();

    movi(p, 2, 0x3c);
    emit(p, 'M', mov_to_um(0));
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0, "mov psr.um = r0");
    emit(p, 'M', mov_to_um(2));
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x3c, "mov psr.um = 0x3c");
    emit(p, 'M', um_mask_op(5, 0x08));                  /* rum 0x08 */
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x34, "rum clears ac");
    emit(p, 'M', um_mask_op(4, 0x08));                  /* sum 0x08 */
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x3c, "sum sets ac");
    emit(p, 'M', um_mask_op(5, 0x30));
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x0c, "rum clears mfl and mfh");
    movi(p, 2, 0x40);
    emit(p, 'M', mov_to_um(2));                         /* bit 6 reserved */
    expect_fault_code(p, 0, 0x30);
    emit(p, 'M', um_mask_op(4, 0x40));                  /* sum 0x40 */
    expect_fault_code(p, 0, 0x30);
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x0c, "reserved writes leave the mask alone");
    emit(p, 'M', mov_to_um(30));                        /* NaT source */
    expect_fault_code(p, 0, 0x10);
    emit(p, 'M', mov_from_um(11));
    store_gr(p, 11, 0x0c, "nat source leaves the mask alone");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* M42/M43 indirect registers: dbr 1, ibr 2, pkr 3, pmc 4, pmd 5 (read +0x10) */
static void test_indirect_register_moves(void)
{
    static const struct { const char *name; unsigned x6, idx; uint64_t v; }
    regs[] = {
        /* Odd registers: dbr has r and w at 63:62, ibr only x at 63. */
        { "dbr[0]", 1, 0, UINT64_C(0x123456789abcdef0) },
        { "dbr[1]", 1, 1, UINT64_C(0xc0f00123456789ab) },
        { "ibr[0]", 2, 0, UINT64_C(0x123456789abcdef0) },
        { "ibr[1]", 2, 1, UINT64_C(0x80f00123456789ab) },
        /* v, wd, rd, xd and a key of 0x2345; reserved bits stay clear. */
        { "pkr[3]", 3, 3, (UINT64_C(0x2345) << 8) | 0xb },
        { "pmc[5]", 4, 5, 0x2f },
        { "pmd[5]", 5, 5, 0x1234 },
    };
    Prog *p = prog_new();

    for (unsigned i = 0; i < G_N_ELEMENTS(regs); i++) {
        movi(p, 3, regs[i].idx);
        movi(p, 2, regs[i].v);
        emit(p, 'M', m42(regs[i].x6, 3, 2));
        emit(p, 'M', m43(regs[i].x6 + 0x10, 11, 3));
        store_gr(p, 11, regs[i].v, "mov %s round trip", regs[i].name);
    }
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* mov ip (3:183), hints, fwb, fc.i (3:63), vmsw (3:273: no PSR.vm: illegal). */
static void test_ip_hints_and_cache_flush(void)
{
    Prog *p = prog_new();
    uint64_t at;

    at = p->at;
    emit(p, 'I', (UINT64_C(0x30) << 27) | (11 << 6));       /* mov r11 = ip */
    store_gr(p, 11, at, "mov r1 = ip");
    movi(p, 11, 0x77);
    emit(p, 'M', (UINT64_C(1) << 27) | (UINT64_C(1) << 26));   /* hint.m */
    emit(p, 'I', (UINT64_C(1) << 27) | (UINT64_C(1) << 26));   /* hint.i */
    emit(p, 'F', (UINT64_C(1) << 27) | (UINT64_C(1) << 26));   /* hint.f */
    bundle(p->s, p->at, 0x11, NOP, NOP, (UINT64_C(2) << 37) | (1 << 27));
    p->at += 16;                                                /* hint.b */
    /* hint.x: an MLX bundle whose X slot is opcode 0, x6 1, y 1. */
    bundle(p->s, p->at, 5, NOP, 0, (UINT64_C(1) << 27) | (UINT64_C(1) << 26));
    p->at += 16;
    emit(p, 'M', UINT64_C(2) << 31);                            /* fwb */
    movi(p, 3, p->at);
    emit(p, 'M', (UINT64_C(1) << 37) | (UINT64_C(1) << 36) |
                 (UINT64_C(0x30) << 27) | (3 << 20));           /* fc.i r3 */
    store_gr(p, 11, 0x77, "hints, fwb and fc.i change nothing");
    emit(p, 'M', (UINT64_C(1) << 37) | (UINT64_C(1) << 36) |
                 (UINT64_C(0x30) << 27) | (30 << 20));          /* fc.i nat */
    expect_fault_code(p, 0, 0x11);
    bundle(p->s, p->at, 0x11, NOP, NOP, UINT64_C(0x18) << 27);  /* vmsw.0 */
    p->at += 16;
    expect_fault(p, 2);
    bundle(p->s, p->at, 0x11, NOP, NOP, UINT64_C(0x19) << 27);  /* vmsw.1 */
    p->at += 16;
    expect_fault(p, 2);
    store_gr(p, 11, 0x77, "vmsw leaves r11");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* Data translation fixtures for probe and ptc.g/ptc.ga, Vol. 2 chapter 4. */
#define PTE_BASE(pa, ar, pl) ((pa) | ((uint64_t)(ar) << 9) | \
                              ((uint64_t)(pl) << 7) | 0x61)
#define RSM_IC um_mask_op(7, 1 << 13)
#define SSM_IC um_mask_op(6, 1 << 13)
#define SSM_DT um_mask_op(6, 1 << 17)

/* Insert a data translation (a TR when idx >= 0, else the TC) at ps. */
static void insert_data(Prog *p, uint64_t va, uint64_t pte, unsigned ps,
                        int idx)
{
    movi(p, 12, va);
    emit(p, 'M', to_cr(20, 12));
    movi(p, 12, ps << 2);
    emit(p, 'M', to_cr(21, 12));
    emit(p, 'M', SRLZ_D);
    movi(p, 13, pte);
    if (idx >= 0) {
        movi(p, 14, idx);
        emit(p, 'M', m42(0x0e, 14, 13));                /* itr.d dtr[r14] */
    } else {
        /* itc.d must end its instruction group: M;MI has a stop after it. */
        bundle(p->s, p->at, 0x0a, m42(0x2e, 0, 13), NOP, NOP);
        p->at += 16;
    }
    emit(p, 'M', SRLZ_D);
}

/* Identity-map RAM with one 16 MiB TR, run the caller's inserts, turn dt on. */
static void data_translation_on(Prog *p, void (*inserts)(Prog *))
{
    emit(p, 'M', RSM_IC);
    emit(p, 'M', SRLZ_D);
    movi(p, 9, (1 << 8) | (12 << 2));                   /* rr0: rid 1, 4 KiB */
    emit(p, 'M', m42(0, 0, 9));
    emit(p, 'M', SRLZ_D);
    insert_data(p, 0, PTE_BASE(0, 3, 0), 24, 0);
    inserts(p);
    emit(p, 'M', SSM_IC);
    emit(p, 'M', (UINT64_C(3) << 31) | (1 << 27));      /* srlz.i */
    emit(p, 'M', SSM_DT);
    emit(p, 'M', SRLZ_D);
}

/* Pages: 0 read-only to level 3, 1 read-write to level 3, 2 level 0 only. */
static void probe_pages(Prog *p)
{
    insert_data(p, 0x20000000, PTE_BASE(0x500000, 0, 3), 12, -1);
    insert_data(p, 0x20001000, PTE_BASE(0x501000, 2, 3), 12, 2);
    insert_data(p, 0x20002000, PTE_BASE(0x502000, 2, 0), 12, 3);
}

static uint64_t probe_i(bool write, unsigned r1, unsigned r3, unsigned pl)
{
    return (UINT64_C(1) << 37) | ((uint64_t)(write ? 0x19 : 0x18) << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)pl << 13) | ((uint64_t)r1 << 6);
}

static uint64_t probe_r(bool write, unsigned r1, unsigned r3, unsigned r2)
{
    return (UINT64_C(1) << 37) | ((uint64_t)(write ? 0x39 : 0x38) << 27) |
           ((uint64_t)r3 << 20) | ((uint64_t)r2 << 13) | ((uint64_t)r1 << 6);
}

/* M40 probe.rw.fault 0x31, probe.r.fault 0x32, probe.w.fault 0x33. */
static uint64_t probe_fault(unsigned x6, unsigned r3, unsigned pl)
{
    return (UINT64_C(1) << 37) | ((uint64_t)x6 << 27) | ((uint64_t)r3 << 20) |
           ((uint64_t)pl << 13);
}

/* probe (Vol. 3 p. 3:217); ISR.code 2, or 5 for probe.fault (Vol. 2 5-1). */
static void test_probe_access_rights(void)
{
    static const struct { uint64_t va; bool r[2], w[2]; } pages[] = {
        { 0x20000000, { 1, 1 }, { 0, 0 } },
        { 0x20001000, { 1, 1 }, { 1, 1 } },
        { 0x20002000, { 1, 0 }, { 1, 0 } },
    };
    Prog *p = prog_new();

    install_handler(p, 0x5300);
    data_translation_on(p, probe_pages);
    for (unsigned i = 0; i < G_N_ELEMENTS(pages); i++) {
        movi(p, 12, pages[i].va);
        for (unsigned k = 0; k < 2; k++) {
            unsigned pl = k ? 3 : 0;

            emit(p, 'M', probe_i(false, 11, 12, pl));
            store_gr(p, 11, pages[i].r[k], "probe.r page %u pl %u", i, pl);
            emit(p, 'M', probe_i(true, 11, 12, pl));
            store_gr(p, 11, pages[i].w[k], "probe.w page %u pl %u", i, pl);
            movi(p, 2, pl);
            emit(p, 'M', probe_r(false, 11, 12, 2));
            store_gr(p, 11, pages[i].r[k], "probe.r reg page %u pl %u", i, pl);
            emit(p, 'M', probe_r(true, 11, 12, 2));
            store_gr(p, 11, pages[i].w[k], "probe.w reg page %u pl %u", i, pl);
        }
    }
    /* Fault forms: allowed accesses do nothing, others fault. */
    movi(p, 12, 0x20000000);                                /* read-only */
    emit(p, 'M', probe_fault(0x32, 12, 3));
    emit(p, 'M', probe_fault(0x31, 12, 0));
    expect_fault_code(p, 0, 5);
    emit(p, 'M', probe_fault(0x33, 12, 0));
    expect_fault_code(p, 0, 5);
    movi(p, 12, 0x20002000);                                /* level 0 only */
    emit(p, 'M', probe_fault(0x33, 12, 0));
    emit(p, 'M', probe_fault(0x32, 12, 3));
    expect_fault_code(p, 0, 5);
    movi(p, 11, 0x77);
    store_gr(p, 11, 0x77, "execution continues after the faults");
    /* NaT addresses. */
    emit(p, 'M', probe_i(false, 11, 30, 0));
    expect_fault_code(p, 0, 0x12);
    emit(p, 'M', probe_fault(0x32, 30, 0));
    expect_fault_code(p, 0, 0x15);
    store_gr(p, 11, 0x77, "nat probes leave r11");
    g_assert_cmpuint(prog_check(p), ==, 0);
}

/* ptc.g, ptc.ga (Vol. 3 p. 3:226): as ptc.l here; reuses its sequence. */
static void run_ptc_global(unsigned x6, uint64_t purge_va, uint64_t *mapped,
                           uint64_t *missed)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;

    ia64_translation_setup(s, &at, 0x8000, (1ULL << 8) | (12 << 2));
    load_const(s, &at, 12, 0x1850, 0x30000);
    step(s, &at, to_cr(20, 12));
    step(s, &at, adds(10, 12 << 2, 0));
    step(s, &at, to_cr(21, 10));
    step(s, &at, SRLZ_D);
    load_const(s, &at, 11, 0x1858, 0x4000 | (2 << 9) | 0x61);
    bundle(s, at, 0x0a, m42(0x2e, 0, 11), NOP, NOP);        /* itc.d */
    at += 16;
    step(s, &at, SRLZ_D);
    load_const(s, &at, 9, 0x1860, purge_va);
    step(s, &at, m42(x6, 9, 10));                           /* ptc.x r9, r10 */
    step(s, &at, SRLZ_D);
    ia64_rfi_virtual(s, &at, 0x3000);
    qtest_writeq(s, 0x1848, 0x30000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), NOP, NOP);
    bundle(s, 0x3010, 1, ld8(10, 5), adds(13, 0x42, 0), adds(14, 0x1008, 0));
    bundle(s, 0x3020, 1, st8(10, 13), NOP, NOP);            /* va 0x30000 */
    bundle(s, 0x3030, 1, st8(14, 13), NOP, NOP);
    bundle(s, 0x3040, 0x11, NOP, NOP, branch(0));
    bundle(s, 0x9000, 1, adds(14, 0x1000, 0), adds(16, 1, 0),
           adds(17, 0x42, 0));
    bundle(s, 0x9010, 1, st8(14, 16), adds(15, 8, 14), NOP);
    bundle(s, 0x9020, 1, st8(15, 17), NOP, NOP);
    bundle(s, 0x9030, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1008);
    *mapped = qtest_readq(s, 0x4000);
    *missed = qtest_readq(s, 0x1000);
    qtest_quit(s);
}

static void test_ptc_g_and_ptc_ga_purge(void)
{
    for (unsigned x6 = 0x0a; x6 <= 0x0b; x6++) {
        uint64_t mapped, missed;

        run_ptc_global(x6, 0x20000, &mapped, &missed);
        g_assert_cmphex(mapped, ==, 0x42);
        g_assert_cmphex(missed, ==, 0);
        run_ptc_global(x6, 0x30000, &mapped, &missed);
        g_assert_cmphex(mapped, ==, 0);
        g_assert_cmphex(missed, ==, 1);
    }
}

/*
 * A single-step trap reports the slot that just completed in ISR.ei, even when
 * IIP has moved on to the next bundle (SDM Vol. 2 Table 3-7): three traps with
 * ei 0, 1, 2, the last one at the next bundle with IPSR.ri 0.
 */
static void single_step_trap_ei(bool physical)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t h = 0xe000;                /* IVA 0x8000 + single-step 0x6000 */

    qtest_writeq(s, 0x1210, -(1LL << 40));
    ia64_translation_setup(s, &at, 0x8000, (1ULL << 8) | (12 << 2));
    ia64_rfi_psr(s, &at, 0x3000,
                 (physical ? PSR_PHYSICAL : PSR_VIRTUAL) | 1ULL << 40);
    bundle(s, 0x3000, 0x01, adds(8, 1, 0), adds(9, 2, 0), adds(10, 3, 0));
    bundle(s, 0x3010, 0x01, adds(18, 0x1008, 0), adds(19, 0x42, 0), NOP);
    bundle(s, 0x3020, 0x01, st8(18, 19), NOP, NOP);
    bundle(s, 0x3030, 0x11, NOP, NOP, branch(0));
    /* Handler: log ISR, IIP, IPSR per trap; clear psr.ss after the third. */
    step(s, &h, adds(14, 0x1200, 0));
    step(s, &h, ld8(16, 14));                       /* n */
    step(s, &h, add(19, 16, 16));
    step(s, &h, add(19, 19, 19));
    step(s, &h, add(19, 19, 19));                   /* 8 * n */
    step(s, &h, adds(15, 0x1100, 0));
    step(s, &h, add(20, 15, 19));
    step(s, &h, from_cr(21, 17));                   /* cr.isr */
    step(s, &h, st8(20, 21));
    step(s, &h, adds(20, 0x20, 20));
    step(s, &h, from_cr(21, 19));                   /* cr.iip */
    step(s, &h, st8(20, 21));
    step(s, &h, adds(20, 0x20, 20));
    step(s, &h, from_cr(21, 16));                   /* cr.ipsr */
    step(s, &h, st8(20, 21));
    step(s, &h, adds(16, 1, 16));
    step(s, &h, st8(14, 16));
    step(s, &h, adds(17, 3, 0));
    step(s, &h, cmp_insn('r', false, R_EQ, CT_NONE, 6, 7, 16, 17, 0));
    step(s, &h, adds(23, 0x1210, 0));
    step(s, &h, ld8(24, 23));
    step(s, &h, add(22, 21, 24));                   /* ipsr without ss */
    step(s, &h, to_cr(16, 22) | 6);                 /* (p6) */
    bundle(s, h, 0x11, NOP, NOP, RFI);
    run_to_marker_at(s, 0x1008);
    g_assert_cmpuint(qtest_readq(s, 0x1200), ==, 3);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t isr = qtest_readq(s, 0x1100 + 8 * i);
        uint64_t iip = qtest_readq(s, 0x1120 + 8 * i);
        uint64_t ipsr = qtest_readq(s, 0x1140 + 8 * i);

        g_assert_cmpuint((isr >> 41) & 3, ==, i);           /* the slot done */
        g_assert_cmphex(iip, ==, i == 2 ? 0x3010 : 0x3000);
        g_assert_cmpuint((ipsr >> 41) & 3, ==, i == 2 ? 0 : i + 1);
    }
    qtest_quit(s);
}

static void test_single_step_trap_reports_the_completed_slot(void)
{
    single_step_trap_ei(false);
    single_step_trap_ei(true);
}

/* Two vCPUs under MTTCG, each running a RAM program from its loader cpu-num. */
static QTestState *smp_start(void)
{
    g_autofree char *flash = NULL;
    g_autofree char *args = NULL;
    int fd = g_file_open_tmp("ia64-smp-XXXXXX.bin", &flash, NULL);
    QTestState *s;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 6 * 1024 * 1024), ==, 0);
    close(fd);
    args = g_strdup_printf("-machine altix350 -smp 2 -m 2G -bios %s "
                           "-accel tcg,thread=multi -S "
                           "-device loader,addr=0x2000,cpu-num=0 "
                           "-device loader,addr=0x4000,cpu-num=1", flash);
    s = qtest_init(args);
    unlink(flash);
    return s;
}

static uint64_t ld8_acq(unsigned dest, unsigned base)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x23) << 30) |
           ((uint64_t)base << 20) | (dest << 6);
}

static uint64_t st8_rel(unsigned base, unsigned source)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x37) << 30) |
           ((uint64_t)base << 20) | (source << 13);
}

/* fetchadd8.acq r8 = [r14], 1 (M17, x6 0x13, inc 1) */
static uint64_t fetchadd8_acq(void)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x13) << 30) | (UINT64_C(1) << 27) |
           (UINT64_C(14) << 20) | (UINT64_C(3) << 13) | (8 << 6);
}

static void smp_wait_markers(QTestState *s, uint64_t a, uint64_t b)
{
    int64_t deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while ((qtest_readq(s, a) != 0x42 || qtest_readq(s, b) != 0x42) &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(qtest_readq(s, a), ==, 0x42);
    g_assert_cmphex(qtest_readq(s, b), ==, 0x42);
}

/* ar.lc counted loop of fetchadd8.acq on the dword at 0x1000, then a marker. */
static void smp_fetchadd_program(QTestState *s, uint64_t at, uint64_t marker)
{
    step(s, &at, adds(14, 0x1000, 0));
    step(s, &at, adds(9, 3999, 0));
    bundle(s, at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    at += 16;
    bundle(s, at, 0x11, fetchadd8_acq(), NOP, branch(0) | (5 << 6));
    at += 16;
    step(s, &at, adds(15, marker, 0));
    step(s, &at, adds(16, 0x42, 0));
    bundle(s, at, 1, st8(15, 16), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
}

static void test_smp_fetchadd_loses_no_increment(void)
{
    QTestState *s = smp_start();

    smp_fetchadd_program(s, 0x2000, 0x1008);
    smp_fetchadd_program(s, 0x4000, 0x1010);
    smp_wait_markers(s, 0x1008, 0x1010);
    g_assert_cmpuint(qtest_readq(s, 0x1000), ==, 8000);
    qtest_quit(s);
}

/* cpu 0 writes data and releases a flag; cpu 1 acquires it, sees the data. */
static void test_smp_release_acquire_message_passing(void)
{
    QTestState *s = smp_start();
    uint64_t at = 0x2000;
    uint64_t bt = 0x4000;
    const unsigned rounds = 200;

    /* cpu 0: per round, [0x1800+8i] = 7 then st8.rel [0x1100+8i] = 1 */
    step(s, &at, adds(14, 0x1800, 0));
    step(s, &at, adds(15, 0x1100, 0));
    step(s, &at, adds(16, 7, 0));
    step(s, &at, adds(17, 1, 0));
    step(s, &at, adds(9, rounds - 1, 0));
    bundle(s, at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    at += 16;
    bundle(s, at, 1, st8(14, 16), adds(14, 8, 14), NOP);
    at += 16;
    bundle(s, at, 1, st8_rel(15, 17), adds(15, 8, 15), NOP);
    at += 16;
    bundle(s, at, 0x11, NOP, NOP, branch(-32) | (5 << 6));
    at += 16;
    step(s, &at, adds(18, 0x1008, 0));
    step(s, &at, adds(19, 0x42, 0));
    bundle(s, at, 1, st8(18, 19), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));

    /* cpu 1: wait for each flag, sum the data, store the sum at 0x1020 */
    step(s, &bt, adds(14, 0x1800, 0));
    step(s, &bt, adds(15, 0x1100, 0));
    step(s, &bt, adds(20, 0, 0));
    step(s, &bt, adds(9, rounds - 1, 0));
    bundle(s, bt, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    bt += 16;
    /* wait: r10 = ld8.acq [r15]; p6 = r10 bit 0 clear; (p6) br wait */
    bundle(s, bt, 0x01, ld8_acq(10, 15), NOP, NOP);
    bundle(s, bt + 16, 0x01, NOP,
           tbit_insn('b', false, CT_NONE, 6, 7, 10, 0, 0), NOP);
    bundle(s, bt + 32, 0x11, NOP, NOP, branch(-32) | 6);
    bt += 48;
    bundle(s, bt, 1, ld8(11, 14), adds(14, 8, 14), adds(15, 8, 15));
    bt += 16;
    bundle(s, bt, 0x01, NOP, add(20, 20, 11), NOP);
    bt += 16;
    bundle(s, bt, 0x11, NOP, NOP, branch(-80) | (5 << 6));
    bt += 16;
    step(s, &bt, adds(18, 0x1020, 0));
    bundle(s, bt, 1, st8(18, 20), adds(19, 0x42, 0), adds(21, 0x1010, 0));
    bt += 16;
    bundle(s, bt, 1, st8(21, 19), NOP, NOP);
    bundle(s, bt + 16, 0x11, NOP, NOP, branch(0));
    smp_wait_markers(s, 0x1008, 0x1010);
    g_assert_cmpuint(qtest_readq(s, 0x1020), ==, 7 * rounds);
    qtest_quit(s);
}

/* cmpxchg8.acq r8 = [r14], r16, ar.ccv (M16, x6 0x03). */
static uint64_t cmpxchg8_acq(void)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x03) << 30) | (UINT64_C(1) << 27) |
           (UINT64_C(14) << 20) | (UINT64_C(16) << 13) | (8 << 6);
}

/* A cmpxchg8.acq/st8.rel lock guards a plain load-add-store counter. */
static void smp_lock_program(QTestState *s, uint64_t at, uint64_t marker,
                             unsigned rounds, uint64_t acquire)
{
    uint64_t loop;

    step(s, &at, adds(14, 0x1200, 0));
    step(s, &at, adds(15, 0x1208, 0));
    step(s, &at, adds(16, 1, 0));
    step(s, &at, m_to_ar(32, 0));                   /* ar.ccv = 0 */
    step(s, &at, adds(9, rounds - 1, 0));
    bundle(s, at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    at += 16;
    loop = at;
    bundle(s, at, 0x01, acquire, NOP, NOP);
    bundle(s, at + 16, 0x01, NOP,
           tbit_insn('b', false, CT_NONE, 6, 7, 8, 0, 0), NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(-32) | 7);   /* (p7) retry */
    bundle(s, at + 48, 0x01, ld8(11, 15), NOP, NOP);
    bundle(s, at + 64, 0x01, NOP, adds(11, 1, 11), NOP);
    bundle(s, at + 80, 0x01, st8(15, 11), NOP, NOP);
    bundle(s, at + 96, 0x11, st8_rel(14, 0), NOP,
           branch(-96) | (5 << 6));                           /* release */
    at = loop + 112;
    step(s, &at, adds(18, marker, 0));
    step(s, &at, adds(19, 0x42, 0));
    bundle(s, at, 1, st8(18, 19), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
}

static void test_smp_lock_gives_mutual_exclusion(void)
{
    QTestState *s = smp_start();

    smp_lock_program(s, 0x2000, 0x1008, 3000, cmpxchg8_acq());
    smp_lock_program(s, 0x4000, 0x1010, 3000, cmpxchg8_acq());
    smp_wait_markers(s, 0x1008, 0x1010);
    g_assert_cmpuint(qtest_readq(s, 0x1208), ==, 6000);
    qtest_quit(s);
}

/* xchg8.acq r8 = [r14], r16 (M16, x6 0x0b). */
static uint64_t xchg8_acq(void)
{
    return (UINT64_C(4) << 37) | (UINT64_C(0x0b) << 30) | (UINT64_C(1) << 27) |
           (UINT64_C(14) << 20) | (UINT64_C(16) << 13) | (8 << 6);
}

/* The same lock taken with a test-and-set xchg8.acq. */
static void test_smp_xchg_lock_gives_mutual_exclusion(void)
{
    QTestState *s = smp_start();

    smp_lock_program(s, 0x2000, 0x1008, 3000, xchg8_acq());
    smp_lock_program(s, 0x4000, 0x1010, 3000, xchg8_acq());
    smp_wait_markers(s, 0x1008, 0x1010);
    g_assert_cmpuint(qtest_readq(s, 0x1208), ==, 6000);
    qtest_quit(s);
}

/* Read ar.itc 2000 times: count readings below the last, report first/last. */
static void smp_itc_program(QTestState *s, uint64_t at, uint64_t out)
{
    uint64_t loop;

    step(s, &at, adds(22, 0, 0));
    step(s, &at, adds(9, 1999, 0));
    bundle(s, at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    at += 16;
    step(s, &at, m_from_ar(19, 44));
    step(s, &at, adds(20, 0, 19));
    loop = at;
    bundle(s, at, 0x01, m_from_ar(21, 44), NOP, NOP);
    bundle(s, at + 16, 0x01, NOP,
           cmp_insn('r', false, R_LTU, CT_NONE, 6, 7, 21, 20, 0), NOP);
    bundle(s, at + 32, 0x01, NOP, adds(22, 1, 22) | 6, NOP);
    bundle(s, at + 48, 0x01, NOP, adds(20, 0, 21), NOP);
    bundle(s, at + 64, 0x11, NOP, NOP, branch(-64) | (5 << 6));
    at = loop + 80;
    step(s, &at, adds(18, out, 0));
    step(s, &at, adds(23, out + 8, 0));
    step(s, &at, adds(24, out + 16, 0));
    step(s, &at, adds(25, out + 24, 0));
    step(s, &at, adds(26, 0x42, 0));
    step(s, &at, st8(18, 22));
    step(s, &at, st8(23, 19));
    step(s, &at, st8(24, 20));
    step(s, &at, st8(25, 26));
    bundle(s, at, 0x11, NOP, NOP, branch(0));
}

/* Each CPU's ITC never reads below an earlier reading and keeps advancing. */
static void test_smp_itc_is_monotonic_on_each_cpu(void)
{
    QTestState *s = smp_start();
    const uint64_t outs[2] = { 0x1000, 0x1100 };

    smp_itc_program(s, 0x2000, outs[0]);
    smp_itc_program(s, 0x4000, outs[1]);
    smp_wait_markers(s, outs[0] + 24, outs[1] + 24);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t first = qtest_readq(s, outs[i] + 8);
        uint64_t last = qtest_readq(s, outs[i] + 16);

        g_assert_cmpuint(qtest_readq(s, outs[i]), ==, 0);
        g_assert_cmpuint(last - first, >=, 2000 * 5);
    }
    qtest_quit(s);
}

/*
 * cpu 0 maps 0x30000 to 0x6000 with itc.d, stores through it and signals; cpu 1
 * purges the page with ptc.g and signals back; cpu 0's next store must miss.
 */
static void smp_ptc_cpu0_program(QTestState *s, uint64_t at)
{
    ia64_translation_setup(s, &at, 0x8000, (1ULL << 8) | (12 << 2));
    load_const(s, &at, 12, 0x1850, 0x30000);
    step(s, &at, to_cr(20, 12));
    step(s, &at, adds(10, 12 << 2, 0));
    step(s, &at, to_cr(21, 10));
    step(s, &at, SRLZ_D);
    load_const(s, &at, 11, 0x1858, 0x6000 | (2 << 9) | 0x61);
    bundle(s, at, 0x0a, m42(0x2e, 0, 11), NOP, NOP);        /* itc.d */
    at += 16;
    step(s, &at, SRLZ_D);
    ia64_rfi_virtual(s, &at, 0x3000);
    qtest_writeq(s, 0x1848, 0x30000);
    bundle(s, 0x3000, 1, adds(5, 0x1848, 0), adds(14, 0x1100, 0),
           adds(13, 0x42, 0));
    bundle(s, 0x3010, 1, ld8(10, 5), adds(16, 0x1108, 0), adds(17, 0x77, 0));
    bundle(s, 0x3020, 1, st8(10, 13), adds(15, 1, 0), NOP);     /* mapped */
    bundle(s, 0x3030, 1, st8_rel(14, 15), NOP, NOP);            /* ready */
    bundle(s, 0x3040, 0x01, ld8_acq(11, 16), NOP, NOP);
    bundle(s, 0x3050, 0x01, NOP,
           tbit_insn('b', false, CT_NONE, 6, 7, 11, 0, 0), NOP);
    bundle(s, 0x3060, 0x11, NOP, NOP, branch(-32) | 6);
    bundle(s, 0x3070, 1, st8(10, 17), adds(18, 0x1010, 0),
           adds(19, 0x42, 0));                                  /* purged? */
    bundle(s, 0x3080, 1, st8(18, 19), NOP, NOP);
    bundle(s, 0x3090, 0x11, NOP, NOP, branch(0));
    /* Alternate Data TLB vector: record the miss, then the marker. */
    bundle(s, 0x9000, 1, adds(14, 0x1000, 0), adds(16, 1, 0),
           adds(17, 0x42, 0));
    bundle(s, 0x9010, 1, st8(14, 16), adds(15, 0x1010, 0), NOP);
    bundle(s, 0x9020, 1, st8(15, 17), NOP, NOP);
    bundle(s, 0x9030, 0x11, NOP, NOP, branch(0));
}

static void smp_ptc_cpu1_program(QTestState *s, uint64_t at)
{
    load_const(s, &at, 9, 0x1900, (1ULL << 8) | (12 << 2));
    step(s, &at, m42(0, 0, 9));                             /* rr0 as cpu 0 */
    step(s, &at, SRLZ_D);
    step(s, &at, adds(15, 0x1100, 0));
    bundle(s, at, 0x01, ld8_acq(11, 15), NOP, NOP);
    bundle(s, at + 16, 0x01, NOP,
           tbit_insn('b', false, CT_NONE, 6, 7, 11, 0, 0), NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(-32) | 6);
    at += 48;
    load_const(s, &at, 9, 0x1908, 0x30000);
    step(s, &at, adds(10, 12 << 2, 0));
    step(s, &at, m42(0x0a, 9, 10));                         /* ptc.g */
    step(s, &at, SRLZ_D);
    step(s, &at, adds(16, 0x1108, 0));
    step(s, &at, adds(17, 1, 0));
    step(s, &at, adds(18, 0x1018, 0));
    step(s, &at, adds(19, 0x42, 0));
    bundle(s, at, 0x01, st8_rel(16, 17), NOP, NOP);
    at += 16;
    bundle(s, at, 0x01, st8(18, 19), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
}

static void test_smp_ptc_g_purges_the_other_cpus_translation(void)
{
    QTestState *s = smp_start();

    smp_ptc_cpu0_program(s, 0x2000);
    smp_ptc_cpu1_program(s, 0x4000);
    smp_wait_markers(s, 0x1010, 0x1018);
    g_assert_cmphex(qtest_readq(s, 0x6000), ==, 0x42);  /* store before ptc.g */
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 1);     /* store after: miss */
    qtest_quit(s);
}

/*
 * Another processor's store invalidates an ALAT entry only if it overlaps the
 * entry (SDM Vol. 1 4.4.5.3.2). cpu 1 holds an ld8.a entry for 0x1000, the host
 * changes that dword behind its back, and cpu 0 stores to 0x1040 or
 * to 0x1000. The ld8.c.nc that follows reloads only if the entry is gone.
 */
static QTestState *smp_alat_program(uint64_t cpu0_store_address,
                                     unsigned stores)
{
    QTestState *s = smp_start();
    uint64_t at = 0x2000;
    uint64_t bt = 0x4000;
    int64_t deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    qtest_writeq(s, 0x1000, 0x11);
    /* cpu 0: wait for the host's go, store `stores` times, say done. */
    step(s, &at, adds(14, 0x1200, 0));
    bundle(s, at, 0x01, ld8_acq(11, 14), NOP, NOP);
    bundle(s, at + 16, 0x01, NOP,
           cmp_insn('r', false, R_EQ, CT_NONE, 6, 7, 11, 0, 0), NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(-32) | 6);
    at += 48;
    step(s, &at, adds(15, cpu0_store_address, 0));
    step(s, &at, adds(16, 0x88, 0));
    step(s, &at, adds(9, stores - 1, 0));
    bundle(s, at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    at += 16;
    bundle(s, at, 0x11, st8(15, 16), NOP, branch(0) | (5 << 6));
    at += 16;
    step(s, &at, adds(14, 0x1208, 0));
    step(s, &at, adds(17, 1, 0));
    bundle(s, at, 1, st8_rel(14, 17), NOP, NOP);
    bundle(s, at + 16, 0x11, NOP, NOP, branch(0));
    /* cpu 1: ld8.a, announce, wait for done, ld8.c.nc, report. */
    step(s, &bt, adds(14, 0x1000, 0));
    step(s, &bt, ld8_x(0x0b, 11, 14));                  /* ld8.a r11 = [r14] */
    step(s, &bt, adds(15, 0x1100, 0));
    step(s, &bt, adds(17, 1, 0));
    bundle(s, bt, 1, st8_rel(15, 17), NOP, NOP);
    bt += 16;
    /* Wait on the ITC, which loads nothing: cpu 0's stores pile up unseen. */
    qtest_writeq(s, 0x1900, 3200000);
    step(s, &bt, adds(15, 0x1900, 0));
    step(s, &bt, ld8(13, 15));
    step(s, &bt, m_from_ar(12, 44));
    step(s, &bt, add(13, 13, 12));
    bundle(s, bt, 0x01, m_from_ar(12, 44), NOP, NOP);
    bundle(s, bt + 16, 0x01, NOP,
           cmp_insn('r', false, R_LTU, CT_NONE, 6, 7, 12, 13, 0), NOP);
    bundle(s, bt + 32, 0x11, NOP, NOP, branch(-32) | 6);
    bt += 48;
    step(s, &bt, adds(15, 0x1208, 0));
    bundle(s, bt, 0x01, ld8_acq(12, 15), NOP, NOP);
    bundle(s, bt + 16, 0x01, NOP,
           cmp_insn('r', false, R_EQ, CT_NONE, 6, 7, 12, 0, 0), NOP);
    bundle(s, bt + 32, 0x11, NOP, NOP, branch(-32) | 6);
    bt += 48;
    step(s, &bt, ld8_x(0x27, 11, 14));              /* ld8.c.nc r11 = [r14] */
    step(s, &bt, adds(15, 0x1010, 0));
    step(s, &bt, st8(15, 11));
    step(s, &bt, adds(15, 0x1018, 0));
    step(s, &bt, adds(17, 0x42, 0));
    bundle(s, bt, 1, st8(15, 17), NOP, NOP);
    bundle(s, bt + 16, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1100) != 1 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_writeq(s, 0x1000, 0x77);              /* not a store by a CPU */
    qtest_writeq(s, 0x1200, 1);
    while (qtest_readq(s, 0x1018) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(qtest_readq(s, 0x1018), ==, 0x42);
    return s;
}

static void test_smp_store_elsewhere_keeps_an_alat_entry(void)
{
    QTestState *s = smp_alat_program(0x1040, 400);

    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x11);   /* hit: no reload */
    qtest_quit(s);
}

static void test_smp_store_to_the_same_dword_invalidates_an_alat_entry(void)
{
    QTestState *s = smp_alat_program(0x1000, 400);

    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x88);   /* miss: reload */
    qtest_quit(s);
}

/*
 * cpu 1 replays up to 1023 unseen stores of cpu 0 (the loop and its final flag
 * store). With 1024 the writer may already have refilled the first slot, so it
 * drops the table instead, as the SDM allows.
 */
static void test_smp_alat_log_holds_1023_stores_and_then_laps(void)
{
    QTestState *s = smp_alat_program(0x1040, 1022);

    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x11);   /* replayed: kept */
    qtest_quit(s);
    s = smp_alat_program(0x1040, 1023);
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x77);   /* lapped: dropped */
    qtest_quit(s);
}

/*
 * PAL 520 installs an L3 way with one range tag write (array 6, type 5, MSR
 * 1545 state 3). The state 0 invalidate sweep leaves it alone and clearing the
 * MSR 1504 enable bits drops it. The way is private, zero-filled and never
 * reaches RAM.
 */
#define L3_WAY_BASE 0x800000ULL
#define L3_TAG_STATE3 ((L3_WAY_BASE >> 18 << 22) | 3)
#define L3_RANGE_WAY0 0x94c00000000007ffULL

static void l3_range_write(CaProgram *p, uint64_t tag_word)
{
    ca_msr_set(p, 1545, tag_word);
    ca_msr_set(p, 1544, 0);
    ca_msr_set(p, 1546, L3_RANGE_WAY0);
    ca_mword(p);
}

/* The PAL's enable bits in MSR 1504 (ways 24, 32 and 33) and bit 48's use. */
#define L3_MSR1504_OFF 0x26fd0008000800f3ULL
#define L3_MSR1504_ON  (L3_MSR1504_OFF | 0x0000000301000000ULL)

/* Load the dword at r8 and store it in the result list. */
static void l3_report(CaProgram *p, unsigned reg)
{
    step(p->s, &p->at, ld8(reg, 8));
    bundle(p->s, p->at, 1, st8(10, reg), adds(10, 8, 10), NOP);
    p->at += 16;
}

/* Replays the PAL's order: enable, install, state 0 sweep, bit 48, disable. */
static void test_l3_way_is_private_and_dropped_without_write_back(void)
{
    CaProgram p;
    uint64_t out[5];

    ca_begin(&p, "");
    qtest_writeq(p.s, L3_WAY_BASE, 0x77);
    load_const(p.s, &p.at, 8, 0x1c10, L3_WAY_BASE);
    ca_msr_set(&p, 1504, L3_MSR1504_ON);
    l3_range_write(&p, L3_TAG_STATE3);
    l3_report(&p, 11);                  /* zero-filled over RAM's 0x77 */
    step(p.s, &p.at, adds(9, 0x55, 0));
    step(p.s, &p.at, st8(8, 9));
    l3_report(&p, 12);
    l3_range_write(&p, 0);              /* the invalidate sweep */
    l3_report(&p, 13);
    ca_msr_set(&p, 1504, L3_MSR1504_ON | (1ULL << 48));
    l3_report(&p, 14);
    ca_msr_set(&p, 1504, L3_MSR1504_OFF);
    l3_report(&p, 15);
    ca_finish(&p, 5, out);
    g_assert_cmphex(out[0], ==, 0);
    g_assert_cmphex(out[1], ==, 0x55);
    g_assert_cmphex(out[2], ==, 0x55);  /* the sweep left the way alone */
    g_assert_cmphex(out[3], ==, 0x55);  /* so did bit 48 */
    g_assert_cmphex(out[4], ==, 0x77);  /* dropped: RAM, never written back */
}

/* Rewriting a way's tag moves it: the old address shows RAM again. */
static void test_l3_way_moves_when_its_tag_is_rewritten(void)
{
    const uint64_t other = 0x900000;
    CaProgram p;
    uint64_t out[2];

    ca_begin(&p, "");
    qtest_writeq(p.s, L3_WAY_BASE, 0x77);
    qtest_writeq(p.s, other, 0x66);
    load_const(p.s, &p.at, 8, 0x1c10, L3_WAY_BASE);
    ca_msr_set(&p, 1504, L3_MSR1504_ON);
    l3_range_write(&p, L3_TAG_STATE3);
    step(p.s, &p.at, adds(9, 0x55, 0));
    step(p.s, &p.at, st8(8, 9));
    l3_range_write(&p, (other >> 18 << 22) | 3);
    l3_report(&p, 11);                  /* old address: RAM, not the way */
    load_const(p.s, &p.at, 8, 0x1c20, other);
    l3_report(&p, 12);                  /* new address: the zero-filled way */
    ca_finish(&p, 2, out);
    g_assert_cmphex(out[0], ==, 0x77);
    g_assert_cmphex(out[1], ==, 0);
}

/* A system reset removes a way: the host sees RAM again. */
static void test_l3_way_does_not_survive_a_reset(void)
{
    CaProgram p;

    ca_begin(&p, "");
    qtest_writeq(p.s, L3_WAY_BASE, 0x77);
    load_const(p.s, &p.at, 8, 0x1c10, L3_WAY_BASE);
    ca_msr_set(&p, 1504, L3_MSR1504_ON);
    l3_range_write(&p, L3_TAG_STATE3);
    step(p.s, &p.at, adds(9, 0x55, 0));
    step(p.s, &p.at, st8(8, 9));
    bundle(p.s, p.at, 1, adds(13, 0x42, 0), adds(6, 0x1400, 0), NOP);
    p.at += 16;
    bundle(p.s, p.at, 1, st8(6, 13), NOP, NOP);
    bundle(p.s, p.at + 16, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(p.s, 0x1400);
    g_assert_cmphex(qtest_readq(p.s, L3_WAY_BASE), ==, 0x55);
    qtest_qmp_assert_success(p.s, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readq(p.s, L3_WAY_BASE), ==, 0x77);
    qtest_quit(p.s);
}

/* The machine needs a PROM image to start: one with a FIT pointer and entry. */
static QTestState *l3_start(void)
{
    g_autofree char *flash = NULL;
    g_autofree char *args = NULL;
    const uint32_t flash_size = 6 * 1024 * 1024;
    uint8_t fit[32] = "_FIT_   ";
    const uint8_t ptr[4] = { 0x00, 0x01, 0xa0, 0xff };  /* table at +0x100 */
    int fd = g_file_open_tmp("ia64-l3-XXXXXX.bin", &flash, NULL);
    QTestState *s;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, flash_size), ==, 0);
    fit[8] = 2;                                 /* the header and one entry */
    fit[18] = 0xa0;                             /* entry: 0xffa00000, */
    fit[19] = 0xff;
    fit[24] = 1;                                /* 16 bytes, */
    fit[30] = 1;                                /* type 1 */
    g_assert_cmpint(pwrite(fd, fit, sizeof(fit), 0x100), ==, sizeof(fit));
    g_assert_cmpint(pwrite(fd, ptr, 4, flash_size - 0x20), ==, 4);
    close(fd);
    args = g_strdup_printf("-machine altix350 -smp 2 -m 2G -bios %s "
                           "-accel tcg,thread=multi -S "
                           "-device loader,addr=0x1100000,cpu-num=0 "
                           "-device loader,addr=0x1104000,cpu-num=1", flash);
    s = qtest_init(args);
    unlink(flash);
    return s;
}

/* Store an own value 2000 times, sum the reads, drop the way, report. */
static void l3_private_program(QTestState *s, uint64_t at, uint64_t own,
                               uint64_t result)
{
    CaProgram p = { .s = s, .at = at, .slot = 0 };

    step(s, &p.at, adds(10, 4096, 0));
    load_const(s, &p.at, 8, 0x1c10, L3_WAY_BASE);
    ca_msr_set(&p, 1504, L3_MSR1504_ON);
    l3_range_write(&p, L3_TAG_STATE3);
    step(s, &p.at, adds(9, 1999, 0));
    step(s, &p.at, adds(14, own, 0));
    step(s, &p.at, adds(20, 0, 0));
    bundle(s, p.at, 1, NOP, (UINT64_C(0x2a) << 27) | (UINT64_C(65) << 20) |
           (UINT64_C(9) << 13), NOP);
    p.at += 16;
    bundle(s, p.at, 1, st8(8, 14), NOP, NOP);
    p.at += 16;
    bundle(s, p.at, 1, ld8(11, 8), NOP, NOP);
    p.at += 16;
    bundle(s, p.at, 0x11, NOP, add(20, 20, 11), branch(-32) | (5 << 6));
    p.at += 16;
    ca_msr_set(&p, 1504, L3_MSR1504_OFF);
    step(s, &p.at, adds(18, result, 0));
    bundle(s, p.at, 1, st8(18, 20), adds(19, 0x42, 0), adds(21, result + 8, 0));
    p.at += 16;
    bundle(s, p.at, 1, st8(21, 19), NOP, NOP);
    bundle(s, p.at + 16, 0x11, NOP, NOP, branch(0));
}

static void test_l3_way_is_private_to_each_cpu(void)
{
    QTestState *s = l3_start();

    l3_private_program(s, 0x1100000, 0x11, 0x1000);
    l3_private_program(s, 0x1104000, 0x22, 0x1100);
    smp_wait_markers(s, 0x1008, 0x1108);
    g_assert_cmphex(qtest_readq(s, 0x1000), ==, 2000 * 0x11);
    g_assert_cmphex(qtest_readq(s, 0x1100), ==, 2000 * 0x22);
    /* The host reads through cpu 0, whose way is gone: RAM is untouched. */
    g_assert_cmphex(qtest_readq(s, L3_WAY_BASE), ==, 0);
    qtest_quit(s);
}

/*
 * The PAL's timer round: read the ITC, set it and cr.itm 100 ticks ahead, then
 * rewind the ITC and spin on it. The vector must arrive exactly once, before
 * the spin's exit is seen, and not again when the ITC has passed cr.itm.
 */
static void test_interval_timer_round_delivers_exactly_once(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m "
                              "-m 16M -accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");
    uint64_t at = 0x2000;
    uint64_t loop;

    qtest_writeq(s, 0x1800, 0x6018);    /* psr.i, psr.ic and the usual bits */
    qtest_writeq(s, 0x1808, 0x2100);
    step(s, &at, adds(9, 0xff, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv = 0xff */
    step(s, &at, to_cr(2, 0));                  /* cr.iva = 0 */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));                /* cr.ipsr */
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(12, 5));
    step(s, &at, to_cr(19, 12));                /* cr.iip */
    bundle(s, at, 0x11, NOP, NOP, RFI);

    /* The handler (IVA + 0x3000): count, then end the interrupt. */
    bundle(s, 0x3000, 1, from_cr(20, 65), adds(16, 1, 16), NOP);
    bundle(s, 0x3010, 1, to_cr(67, 0), NOP, NOP);
    bundle(s, 0x3020, 1, from_cr(21, 65), NOP, NOP);
    bundle(s, 0x3030, 0x11, NOP, NOP, RFI);

    at = 0x2100;
    step(s, &at, adds(16, 0, 0));
    step(s, &at, m_from_ar(38, 44));            /* r38 = ar.itc */
    step(s, &at, adds(39, 100, 38));
    step(s, &at, m_to_ar(44, 39));              /* ar.itc = r38 + 100 */
    step(s, &at, to_cr(1, 39));                 /* cr.itm = the same */
    step(s, &at, m_to_ar(44, 38));              /* rewind */
    loop = at;
    bundle(s, at, 0x01, m_from_ar(21, 44), NOP, NOP);
    bundle(s, at + 16, 0x01, from_cr(20, 1),
           cmp_insn('r', false, R_LTU, CT_NONE, 6, 7, 21, 20, 0), NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(loop - (at + 32)) | 6);
    at += 48;
    step(s, &at, adds(14, 0x1000, 0));
    step(s, &at, st8(14, 16));                  /* the count at loop exit */
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN);
    step(s, &at, m_from_ar(10, 44));
    step(s, &at, add(10, 10, 12));
    loop = at;
    bundle(s, at, 0x01, m_from_ar(21, 44), NOP, NOP);
    bundle(s, at + 16, 0x01, NOP,
           cmp_insn('r', false, R_LTU, CT_NONE, 6, 7, 21, 10, 0), NOP);
    bundle(s, at + 32, 0x11, NOP, NOP, branch(loop - (at + 32)) | 6);
    at += 48;
    step(s, &at, adds(14, 0x1008, 0));
    step(s, &at, st8(14, 16));                  /* the count 1 ms later */
    step(s, &at, adds(14, 0x1010, 0));
    step(s, &at, adds(15, 0x42, 0));
    step(s, &at, st8(14, 15));
    bundle(s, at, 0x11, NOP, NOP, branch(0));
    run_to_marker_at(s, 0x1010);
    g_assert_cmpuint(qtest_readq(s, 0x1000), ==, 1);
    g_assert_cmpuint(qtest_readq(s, 0x1008), ==, 1);
    qtest_quit(s);
}

/* A checkstopped CPU stays halted when its interval timer fires. */
static void test_checkstopped_cpu_ignores_the_interval_timer(void)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m -m 16M "
                              "-accel tcg -S -action panic=none "
                              "-device loader,addr=0x2000,cpu-num=0");
    g_autofree char *registers = NULL;
    uint64_t at = 0x2000;

    qtest_writeq(s, 0x1800, 0x6018);    /* psr.i, psr.ic and the usual bits */
    qtest_writeq(s, 0x1808, 0x2400);
    step(s, &at, adds(9, 0x40, 0));
    step(s, &at, to_cr(72, 9));                 /* cr.itv = 0x40 */
    step(s, &at, to_cr(2, 0));                  /* cr.iva = 0 */
    step(s, &at, m_from_ar(10, 44));
    load_const(s, &at, 12, 0x1c40, ITM_MARGIN * 100);
    step(s, &at, add(10, 10, 12));
    step(s, &at, to_cr(1, 10));                 /* cr.itm: 100 ms ahead */
    step(s, &at, adds(5, 0x1800, 0));
    step(s, &at, ld8(11, 5));
    step(s, &at, to_cr(16, 11));
    step(s, &at, adds(5, 0x1808, 0));
    step(s, &at, ld8(13, 5));
    step(s, &at, to_cr(19, 13));
    bundle(s, at, 0x11, NOP, NOP, RFI);
    /* The timer handler would store 0x77 at 0x1008. */
    bundle(s, 0x3000, 1, adds(14, 0x1008, 0), adds(15, 0x77, 0), NOP);
    bundle(s, 0x3010, 1, st8(14, 15), NOP, NOP);
    bundle(s, 0x3020, 0x11, NOP, NOP, branch(0));
    /* Interrupts on, then a load from 1 GiB, past the 16 MiB of RAM. */
    bundle(s, 0x2400, 1, adds(12, 0x400, 0), NOP, NOP);
    for (unsigned i = 0; i < 20; i++) {
        bundle(s, 0x2410 + 16 * i, 1, add(12, 12, 12), NOP, NOP);
    }
    bundle(s, 0x2550, 1, ld8(14, 12), NOP, NOP);
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    qtest_qmp_eventwait(s, "GUEST_PANICKED");
    /* Wait out the timer (100 ms of ITC) and then some. */
    g_usleep(400000);
    g_assert_cmphex(qtest_readq(s, 0x1008), ==, 0);
    registers = qtest_hmp(s, "info registers");
    g_assert_nonnull(strstr(registers, "HALTED: 1"));
    qtest_quit(s);
}

/* The ITC counts the virtual clock in physical and in virtual mode alike. */
static QTestState *itc_host_program(bool virtual_mode, uint64_t *at)
{
    QTestState *s = qtest_init("-machine none -cpu madison-1600-3m -m 16M "
                              "-accel tcg -S "
                              "-device loader,addr=0x2000,cpu-num=0");

    *at = 0x2000;
    step(s, at, adds(14, 0x1000, 0));
    step(s, at, m_from_ar(10, 44));
    step(s, at, st8(14, 10));                           /* first reading */
    if (virtual_mode) {
        ia64_translation_setup(s, at, 0x8000, (1ULL << 8) | (12 << 2));
        ia64_rfi_virtual(s, at, 0x3000);
    } else {
        bundle(s, *at, 0x11, NOP, NOP, branch(0x3000 - *at));
    }
    return s;
}

static uint64_t itc_across_a_host_wait(bool virtual_mode)
{
    uint64_t at;
    QTestState *s = itc_host_program(virtual_mode, &at);
    int64_t deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    uint64_t t0, t1;

    /* Spin on the ITC until 0.3 s of it (480M ticks at 1.6 GHz) have passed. */
    qtest_writeq(s, 0x1c40, 480000000);
    bundle(s, 0x3000, 1, adds(14, 0x1000, 0), adds(5, 0x1c40, 0), NOP);
    bundle(s, 0x3010, 1, ld8(9, 14), NOP, NOP);
    bundle(s, 0x3020, 1, ld8(12, 5), NOP, NOP);
    bundle(s, 0x3030, 1, add(11, 9, 12), NOP, NOP);
    bundle(s, 0x3040, 0x01, m_from_ar(10, 44), NOP, NOP);
    bundle(s, 0x3050, 0x01, NOP,
           cmp_insn('r', false, R_LTU, CT_NONE, 6, 7, 10, 11, 0), NOP);
    bundle(s, 0x3060, 0x11, NOP, NOP, branch(-32) | 6);
    bundle(s, 0x3070, 1, m_from_ar(10, 44), adds(14, 0x1008, 0),
           adds(15, 0x42, 0));
    bundle(s, 0x3080, 1, st8(14, 10), adds(14, 0x1010, 0), NOP);
    bundle(s, 0x3090, 1, st8(14, 15), NOP, NOP);
    bundle(s, 0x30a0, 0x11, NOP, NOP, branch(0));
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
    while (qtest_readq(s, 0x1010) != 0x42 &&
           g_get_monotonic_time() < deadline) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{ 'execute': 'stop' }");
    g_assert_cmphex(qtest_readq(s, 0x1010), ==, 0x42);
    t0 = qtest_readq(s, 0x1000);
    t1 = qtest_readq(s, 0x1008);
    qtest_quit(s);
    g_assert_cmpuint(t1, >=, t0);       /* monotonic across the mode switch */
    return t1 - t0;
}

static void test_itc_counts_virtual_time_in_either_mode(void)
{
    /* 0.3 s is 480M ticks at 1.6 GHz. */
    g_assert_cmpuint(itc_across_a_host_wait(true), >=, 450000000);
    g_assert_cmpuint(itc_across_a_host_wait(false), >=, 450000000);
}

/*
 * A running translated CPU takes the interval timer at the virtual time of its
 * match: 0.5 s of ITC.
 */
static void test_interval_timer_fires_on_virtual_time_while_running(void)
{
    uint64_t at;
    QTestState *s = itc_host_program(true, &at);
    int64_t start, took;

    qtest_writeq(s, 0x1900, 800000000);                 /* 0.5 s of ITC */
    bundle(s, 0x3000, 1, adds(9, 0x40, 0), NOP, NOP);
    bundle(s, 0x3010, 1, to_cr(72, 9), NOP, NOP);       /* cr.itv = 0x40 */
    bundle(s, 0x3020, 1, m_from_ar(10, 44), adds(5, 0x1900, 0), NOP);
    bundle(s, 0x3030, 1, ld8(12, 5), NOP, NOP);
    bundle(s, 0x3040, 1, add(10, 10, 12), NOP, NOP);
    bundle(s, 0x3050, 1, to_cr(1, 10), NOP, NOP);       /* cr.itm */
    bundle(s, 0x3060, 0x01, from_cr(11, 69), NOP, NOP); /* cr.irr1 */
    bundle(s, 0x3070, 0x01, NOP,
           cmp_insn('r', false, R_EQ, CT_NONE, 6, 7, 11, 0, 0), NOP);
    bundle(s, 0x3080, 0x11, NOP, NOP, branch(-32) | 6);
    bundle(s, 0x3090, 1, adds(15, 0x42, 0), adds(14, 0x1010, 0), NOP);
    bundle(s, 0x30a0, 1, st8(14, 15), NOP, NOP);
    bundle(s, 0x30b0, 0x11, NOP, NOP, branch(0));
    start = g_get_monotonic_time();
    run_to_marker_at(s, 0x1010);
    took = g_get_monotonic_time() - start;
    g_assert_cmpint(took, >=, 400 * 1000);
    g_assert_cmpint(took, <, 1500 * 1000);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("ia64/inferred-reset", test_inferred_reset);
    qtest_add_func("ia64/diagnostic-msr-policy", test_diagnostic_msr_policy);
    qtest_add_func("ia64/unknown-msr-reads-return-zero",
                   test_unknown_msr_reads_return_zero);
    qtest_add_func("ia64/unimplemented-msrs-store-last-write",
                   test_unimplemented_msrs_store_the_last_write);
    qtest_add_func("ia64/msr1212-completes-after-class4-command",
                   test_msr1212_completion_after_the_class4_command);
    qtest_add_func("ia64/interval-timer-interrupt-is-delivered",
                   test_interval_timer_interrupt_is_delivered);
    qtest_add_func("ia64/tpr-masks-a-pending-vector",
                   test_tpr_masks_a_pending_vector);
    qtest_add_func("ia64/translation-registers-map-a-virtual-page",
                   test_translation_registers_map_a_virtual_page);
    qtest_add_func("ia64/unmapped-access-takes-the-alternate-data-tlb-vector",
                   test_unmapped_access_takes_the_alternate_data_tlb_vector);
    qtest_add_func("ia64/data-fault-in-slot-1-sets-isr-ei",
                   test_data_fault_in_slot_1_sets_isr_ei);
    qtest_add_func("ia64/vhpt-walker-inserts-a-long-format-entry",
                   test_vhpt_walker_inserts_a_long_format_entry);
    qtest_add_func("ia64/ptr-d-purges-a-data-translation-register",
                   test_ptr_d_purges_a_data_translation_register);
    qtest_add_func("ia64/itc-d-in-a-handler-lets-the-load-retry",
                   test_itc_d_in_a_handler_lets_the_load_retry);
    qtest_add_func("ia64/ptc-purges-the-translation-cache",
                   test_ptc_purges_the_translation_cache);
    qtest_add_func("ia64/tpr-defers-an-enabled-interrupt-until-lowered",
                   test_tpr_defers_an_enabled_interrupt_until_it_is_lowered);
    qtest_add_func("ia64/alternate-itlb-vector-and-rfi-to-physical-mode",
                   test_alternate_itlb_vector_and_rfi_to_physical_mode);
    qtest_add_func("ia64/cache-array-line-is-seen-while-the-port-is-active",
                   test_cache_array_line_is_seen_while_the_port_is_active);
    qtest_add_func("ia64/msr1536-l3-ways", test_msr1536_l3_ways);
    qtest_add_func("ia64/reserved-word-in-virtual-mode",
                   test_reserved_word_in_virtual_mode);
    qtest_add_func("ia64/msr1559-slice-follows-the-sapic-id",
                   test_msr1559_slice_follows_the_sapic_id);
    qtest_add_func("ia64/msr1559-clock-ratio", test_msr1559_clock_ratio);
    qtest_add_func("ia64/madison-cache-array-log",
                   test_madison_cache_array_log);
    qtest_add_func("ia64/madison-cache-array-stored-lines",
                   test_madison_cache_array_reads_stored_lines);
    qtest_add_func("ia64/madison-cache-array-fc-drops-line-data",
                   test_madison_cache_array_fc_drops_line_data);
    qtest_add_func("ia64/madison-cache-array-fc-i-keeps-line-data",
                   test_madison_cache_array_fc_i_keeps_line_data);
    qtest_add_func("ia64/madison-cache-array-fc-drops-every-column",
                   test_madison_cache_array_fc_drops_every_column);
    qtest_add_func("ia64/madison-l3-array-fc-drops-line-data",
                   test_madison_l3_array_fc_drops_line_data);
    qtest_add_func("ia64/madison-l3-array-fc-drops-every-way",
                   test_madison_l3_array_fc_drops_every_way);
    qtest_add_func("ia64/madison-cache-array-tag",
                   test_madison_cache_array_tag);
    qtest_add_func("ia64/madison-cache-array-tag-hit",
                   test_madison_cache_array_tag_hit);
    qtest_add_func("ia64/madison-cache-array-tag-parity",
                   test_madison_cache_array_tag_parity);
    qtest_add_func("ia64/madison-cache-array-replacement-mask",
                   test_madison_cache_array_replacement_mask);
    qtest_add_func("ia64/madison-cache-array-second-level",
                   test_madison_cache_array_second_level);
    qtest_add_func("ia64/madison-cache-array-check-bits",
                   test_madison_cache_array_check_bits);
    qtest_add_func("ia64/alat-checks", test_alat_checks);
    qtest_add_func("ia64/interval-timer-is-taken-at-its-tick",
                   test_interval_timer_is_taken_at_its_tick);
    qtest_add_func("ia64/alat-store-compares-20-address-bits",
                   test_alat_store_compares_20_address_bits);
    qtest_add_func("ia64/halt-word-waits-for-the-interval-timer",
                   test_halt_word_waits_for_the_interval_timer);
    qtest_add_func("ia64/halt-word-stays-halted-without-an-interrupt",
                   test_halt_word_stays_halted_without_an_interrupt);
    qtest_add_func("ia64/halt-word-halts-after-an-earlier-array-command",
                   test_halt_word_halts_after_an_earlier_array_command);
    qtest_add_func("ia64/sapic-id-is-the-platform-assignment",
                   test_sapic_id_is_the_platform_assignment);
    qtest_add_func("ia64/smp-fetchadd-loses-no-increment",
                   test_smp_fetchadd_loses_no_increment);
    qtest_add_func("ia64/smp-lock-gives-mutual-exclusion",
                   test_smp_lock_gives_mutual_exclusion);
    qtest_add_func("ia64/smp-xchg-lock-gives-mutual-exclusion",
                   test_smp_xchg_lock_gives_mutual_exclusion);
    qtest_add_func("ia64/smp-store-elsewhere-keeps-an-alat-entry",
                   test_smp_store_elsewhere_keeps_an_alat_entry);
    qtest_add_func("ia64/smp-store-to-the-same-dword-invalidates-an-alat-entry",
                   test_smp_store_to_the_same_dword_invalidates_an_alat_entry);
    qtest_add_func("ia64/smp-alat-log-holds-1023-stores-and-then-laps",
                   test_smp_alat_log_holds_1023_stores_and_then_laps);
    qtest_add_func("ia64/smp-itc-is-monotonic-on-each-cpu",
                   test_smp_itc_is_monotonic_on_each_cpu);
    qtest_add_func("ia64/smp-ptc-g-purges-the-other-cpus-translation",
                   test_smp_ptc_g_purges_the_other_cpus_translation);
    qtest_add_func("ia64/smp-release-acquire-message-passing",
                   test_smp_release_acquire_message_passing);
    qtest_add_func("ia64/l3-way-is-private-and-dropped-without-write-back",
                   test_l3_way_is_private_and_dropped_without_write_back);
    qtest_add_func("ia64/l3-way-moves-when-its-tag-is-rewritten",
                   test_l3_way_moves_when_its_tag_is_rewritten);
    qtest_add_func("ia64/l3-way-does-not-survive-a-reset",
                   test_l3_way_does_not_survive_a_reset);
    qtest_add_func("ia64/l3-way-is-private-to-each-cpu",
                   test_l3_way_is_private_to_each_cpu);
    qtest_add_func("ia64/interval-timer-round-delivers-exactly-once",
                   test_interval_timer_round_delivers_exactly_once);
    qtest_add_func("ia64/itc-counts-virtual-time-in-either-mode",
                   test_itc_counts_virtual_time_in_either_mode);
    qtest_add_func("ia64/interval-timer-fires-on-virtual-time-while-running",
                   test_interval_timer_fires_on_virtual_time_while_running);
    qtest_add_func("ia64/alat-io-store-under-icount",
                   test_alat_io_store_under_icount);
    qtest_add_func("ia64/alat-store-fault-does-not-wedge",
                   test_alat_store_fault_does_not_wedge_the_next_store);
    qtest_add_func("ia64/madison-pmu-readback", test_madison_pmu_readback);
    qtest_add_func("ia64/msr1552-command-completes",
                   test_msr1552_command_completes);
    qtest_add_func("ia64/msr1730-training-latch",
                   test_msr1730_training_latch);
    qtest_add_func("ia64/write-only-msrs-accepted",
                   test_write_only_msrs_are_accepted);
    qtest_add_func("ia64/bria-table-load-reports-done",
                   test_bria_table_load_reports_done);
    qtest_add_func("ia64/rnat-reads-back-what-was-written",
                   test_rnat_reads_back_what_was_written);
    qtest_add_func("ia64/chk-s-i-speculation-vector",
                   test_chk_s_i_takes_the_speculation_vector);
    qtest_add_func("ia64/unnormal-operand-software-assist",
                   test_unnormal_operand_takes_software_assist);
    qtest_add_func("ia64/fnorm-integer-pattern-no-assist",
                   test_fnorm_of_an_integer_pattern_takes_no_assist);
    qtest_add_func("ia64/native-arithmetic-branch-break", test_native_break);
    qtest_add_func("ia64/rfi-enters-ipsr-at-iip", test_rfi_enters_ipsr_at_iip);
    qtest_add_func("ia64/reserved-b-word-returns-from-interruption",
                   test_reserved_b_word_returns_from_interruption);
    qtest_add_func("ia64/bad-access-stops-at-the-load",
                   test_bad_access_stops_at_the_load);
    qtest_add_func("ia64/bria-service-continues", test_bria_service_continues);
    qtest_add_func("ia64/msr-index-above-range-is-ignored",
                   test_msr_index_above_range_is_ignored);
    qtest_add_func("ia64/checkstopped-cpu-ignores-the-interval-timer",
                   test_checkstopped_cpu_ignores_the_interval_timer);
    qtest_add_func("ia64/bus-error-is-a-guest-panic",
                   test_bus_error_is_a_guest_panic);
    qtest_add_func("ia64/bria-without-gate-stops", test_bria_without_gate_stops);
    qtest_add_func("ia64/rfi-to-ia32-stops", test_rfi_to_ia32_stops);
    qtest_add_func("ia64/migration-is-refused", test_migration_is_refused);
    qtest_add_func("ia64/bria-dirty-rse-faults-first",
                   test_bria_dirty_rse_faults_before_service);
    qtest_add_func("ia64/bria-psr-di-faults-first",
                   test_bria_psr_di_faults_before_service);
    qtest_add_func("ia64/reserved-pal-words-fault-on-another-family",
                   test_reserved_pal_words_fault_on_another_family);
    qtest_add_func("ia64/reserved-words-run-at-pl0",
                   test_reserved_words_run_at_pl0);
    qtest_add_func("ia64/reserved-words-run-in-ram",
                   test_reserved_words_run_in_ram);
    qtest_add_func("ia64/reserved-cache-word-runs-at-pl0",
                   test_reserved_cache_word_runs_at_pl0);
    qtest_add_func("ia64/reserved-words-fault-above-pl0",
                   test_reserved_words_fault_above_pl0);
    qtest_add_func("ia64/reserved-word-stops-other-family",
                   test_reserved_word_stops_on_another_family);
    qtest_add_func("ia64/other-reserved-words-fault",
                   test_other_reserved_words_still_fault);
    qtest_add_func("ia64/bria-image-check-accepts",
                   test_bria_image_check_accepts_signed_image);
    qtest_add_func("ia64/bria-image-check-rejects-tampered",
                   test_bria_image_check_rejects_tampered_image);
    qtest_add_func("ia64/bria-image-check-rejects-size",
                   test_bria_image_check_rejects_wrong_size);
    qtest_add_func("ia64/bria-image-check-rejects-tiny",
                   test_bria_image_check_rejects_tiny_sizes);
    qtest_add_func("ia64/bria-image-check-rejects-wrapped-size",
                   test_bria_image_check_rejects_wrapped_size);
    qtest_add_func("ia64/bria-image-check-rejects-unreadable-image",
                   test_bria_image_check_rejects_unreadable_image);
    qtest_add_func("ia64/bria-image-check-rejects-without-a-key-address",
                   test_bria_image_check_rejects_without_a_key_address);
    qtest_add_func("ia64/bria-image-check-rejects-unreadable-key",
                   test_bria_image_check_rejects_unreadable_key);
    qtest_add_func("ia64/bria-image-check-needs-the-gate",
                   test_bria_image_check_needs_the_gate);
    qtest_add_func("ia64/bria-service-image-stops",
                   test_bria_service_image_stops);
    qtest_add_func("ia64/cmp-relations-and-types",
                   test_cmp_relations_and_types);
    qtest_add_func("ia64/cmp4-uses-the-low-32-bits",
                   test_cmp4_uses_the_low_32_bits);
    qtest_add_func("ia64/tbit-tnat-tf", test_tbit_tnat_tf);
    qtest_add_func("ia64/rnat-survives-a-bspstore-write",
                   test_rnat_survives_a_bspstore_write);
    qtest_add_func("ia64/shift-immediate-and-variable",
                   test_shift_immediate_and_variable);
    qtest_add_func("ia64/mux-pcmp-addp4", test_mux_pcmp_addp4);
    qtest_add_func("ia64/clz-and-mpy4-are-not-implemented",
                   test_clz_and_mpy4_are_not_implemented);
    qtest_add_func("ia64/fp-parallel-arithmetic",
                   test_fp_parallel_arithmetic);
    qtest_add_func("ia64/fp-merge-and-swap", test_fp_merge_and_swap);
    qtest_add_func("ia64/fp-convert-parallel", test_fp_convert_parallel);
    qtest_add_func("ia64/getf-setf-formats", test_getf_setf_formats);
    qtest_add_func("ia64/fp-reciprocal-approximations",
                   test_fp_reciprocal_approximations);
    qtest_add_func("ia64/fclrf-and-fchkf", test_fclrf_and_fchkf);
    qtest_add_func("ia64/advanced-and-check-loads",
                   test_advanced_and_check_loads);
    qtest_add_func("ia64/sixteen-byte-accesses", test_sixteen_byte_accesses);
    qtest_add_func("ia64/floating-point-pair-loads",
                   test_floating_point_pair_loads);
    qtest_add_func("ia64/lfetch-updates-and-faults",
                   test_lfetch_updates_and_faults);
    qtest_add_func("ia64/cpuid-registers", test_cpuid_registers);
    qtest_add_func("ia64/user-mask-moves", test_user_mask_moves);
    qtest_add_func("ia64/indirect-register-moves",
                   test_indirect_register_moves);
    qtest_add_func("ia64/ip-hints-and-cache-flush",
                   test_ip_hints_and_cache_flush);
    qtest_add_func("ia64/probe-access-rights", test_probe_access_rights);
    qtest_add_func("ia64/single-step-trap-reports-the-completed-slot",
                   test_single_step_trap_reports_the_completed_slot);
    qtest_add_func("ia64/ptc-g-and-ptc-ga-purge",
                   test_ptc_g_and_ptc_ga_purge);
    return g_test_run();
}
