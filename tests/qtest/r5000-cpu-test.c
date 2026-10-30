/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Functional CPU policy, exercised by original synthetic instructions. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-wait.h"
#include "sgi-migrate.h"

#define MAILBOX 0x1000
#define FIXTURE "R5000,r5000-ec=3,r5000-ep=5"
#define CONFIG_RO 0x3551e6f0u

static char *image_path;
static uint32_t program[256];
static unsigned int length;
static unsigned int next_code;

static void emit(uint32_t instruction)
{
    g_assert_cmpuint(length, <, G_N_ELEMENTS(program));
    program[length++] = instruction;
}

static void constant(unsigned int reg, uint32_t value)
{
    emit(0x3c000000 | reg << 16 | value >> 16);
    emit(0x34000000 | reg << 21 | reg << 16 | (value & 0xffff));
}

static void constant64(unsigned int reg, uint64_t value)
{
    emit(0x34000000 | reg << 16 | (value >> 48));
    for (int shift = 32; shift >= 0; shift -= 16) {
        emit(reg << 16 | reg << 11 | 16 << 6 | 0x38); /* dsll */
        emit(0x34000000 | reg << 21 | reg << 16 |
             ((value >> shift) & 0xffff));
    }
}

static void cp0_read(unsigned int reg, unsigned int offset)
{
    emit(0x40080000 | reg << 11); /* mfc0 t0, reg */
    emit(0xaf480000 | offset);   /* sw t0, offset(k0) */
}

static void cp0_write(unsigned int reg, uint32_t value)
{
    constant(8, value);
    emit(0x40880000 | reg << 11); /* mtc0 t0, reg */
    for (unsigned int i = 0; i < 4; i++) {
        emit(0);
    }
}

static void submit(QTestState *qts)
{
    uint8_t bytes[sizeof(program)];

    for (unsigned int i = 0; i < length; i++) {
        stl_be_p(bytes + i * 4, program[i]);
    }
    qtest_memwrite(qts, next_code, bytes, length * 4);
    qtest_writel(qts, MAILBOX + 4, 0);
    qtest_writel(qts, MAILBOX, 0xa0000000 | next_code);
    next_code += 0x1000;
    length = 0;
}

static void execute(QTestState *qts)
{
    SgiWait wait;

    emit(0x03e00008); /* jr ra */
    emit(0);
    submit(qts);
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "R5000 synthetic instruction completion");
    while (!qtest_readl(qts, MAILBOX + 4)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(qts, "STOP");
}

static void wait_entry(QTestState *qts, uint32_t old)
{
    SgiWait wait;

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    sgi_wait_start(&wait, "R5000 reset-vector entry");
    while (qtest_readl(qts, MAILBOX + 8) == old) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(qts, "STOP");
}

static QTestState *start_stopped(const char *cpu, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(image_path);
    QTestState *qts = qtest_initf("-M malta -m 64M -cpu %s -bios %s "
        "-nic none -accel tcg -S "
        "-device mips-cpu-exception-fixture,id=reset-fixture,"
        "attribute-probe-base=0x1e000000,upper-probe-base=0xffffff000 %s",
        cpu, quoted, extra);

    next_code = 0x4000;
    length = 0;
    return qts;
}

static QTestState *start(const char *cpu)
{
    QTestState *qts = start_stopped(cpu, "");

    wait_entry(qts, 0);
    return qts;
}

static void test_reset(void)
{
    QTestState *qts = start(FIXTURE);

    for (unsigned int i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x20), ==, 0x00400004);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x24), ==, CONFIG_RO | 2);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x28), ==, 0x2321);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x2c), ==, 0);
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x30), <=, 47);
        cp0_write(12, 0x24400004); /* CU1 and FR, preserving kernel/ERL. */
        emit(0x44480000); /* cfc1 t0, fcr0 */
        emit(0xaf480034);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x34), ==, 0x2310);
        cp0_write(16, 0xffffffff);
        execute(qts);
        qtest_system_reset(qts);
        wait_entry(qts, i + 1);
    }
    qtest_quit(qts);
}

static void test_config(void)
{
    static const uint32_t writes[] = {
        0, 0xffffffff, 1, 2, 3, 4, 5, 6, 7, 0x1000, 0x80000008,
        0x7fffefe0,
    };
    QTestState *qts = start(FIXTURE);

    for (unsigned int i = 0; i < G_N_ELEMENTS(writes); i++) {
        cp0_write(16, writes[i]);
        cp0_read(16, 0x40);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==,
                        CONFIG_RO | (writes[i] & 0x1007));
    }
    qtest_quit(qts);
}

static void test_immutable_modes(void)
{
    static const char *names[] = { "r5000-ec", "r5000-ep" };
    QTestState *qts = start(FIXTURE);
    g_autoptr(QDict) cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    QDict *cpu = qobject_to(QDict,
        qlist_peek(qdict_get_qlist(cpus, "return")));

    for (unsigned int i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(QDict) reply = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':%s,"
            "'property':%s,'value':0}}", qdict_get_str(cpu, "qom-path"),
            names[i]);

        g_assert_nonnull(qdict_get_qdict(reply, "error"));
    }
    qtest_system_reset(qts);
    wait_entry(qts, 1);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x24), ==, CONFIG_RO | 2);
    qtest_quit(qts);
}

static void test_kseg0_attributes(void)
{
    static const uint32_t modes[] = { 2, 3, 2, 3 };
    QTestState *qts = start(FIXTURE);

    for (unsigned int i = 0; i < G_N_ELEMENTS(modes); i++) {
        cp0_write(16, modes[i]);
        constant(9, 0x9e000000);
        emit(0x8d280000);
        emit(0xaf480040);
        execute(qts);
        g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x40), ==, modes[i]);
    }
    qtest_quit(qts);
}

static uint64_t property(QTestState *qts, const char *path, const char *name)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, name);

    g_assert_true(qdict_haskey(reply, "return"));
    return qdict_get_int(reply, "return");
}

static void test_timer(void)
{
    QTestState *qts = start(FIXTURE);
    unsigned int loop;

    cp0_write(12, 0); /* Pending IP7 must remain observable while masked. */
    cp0_write(9, 0xfffffff0);
    cp0_write(11, 0x00400000);
    loop = length;
    emit(0x40086800); /* mfc0 t0, Cause */
    emit(0x31098000); /* andi t1, t0, IP7 */
    emit(0x11200000 | ((loop - length - 1) & 0xffff));
    emit(0);
    cp0_read(13, 0x40);
    cp0_read(9, 0x44);
    cp0_write(11, 0x80000000);
    cp0_read(13, 0x48);
    cp0_read(12, 0x4c);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40) & 0x8000, ==, 0x8000);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x44), >=, 0x00400000);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x44), <, 0x80000000);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x48) & 0x8000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x4c), ==, 0);
    qtest_quit(qts);
}

static void test_status(void)
{
    QTestState *qts = start(FIXTURE);

    /* Keep kernel access while testing reserved diagnostic positions. */
    cp0_write(12, 0xffffffff & ~0x18u);
    cp0_read(12, 0x40);
    cp0_write(12, 0x00400004);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0xf653ffe7);
    cp0_write(13, 0x300);
    cp0_read(13, 0x40);
    cp0_write(12, 0x0040ff04);
    cp0_read(13, 0x44);
    cp0_read(12, 0x48);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40) & 0x300, ==, 0x300);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44) & 0x300, ==, 0x300);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x48), ==, 0x0040ff04);
    qtest_quit(qts);
}

static void test_external_pending(void)
{
    QTestState *qts = start(FIXTURE);

    for (unsigned int line = 0; line < 6; line++) {
        uint32_t pending = 1 << (line + 10);

        qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture",
                         "external-irq", line, 1);
        cp0_write(12, 0x00400004); /* ERL, with every IM bit clear. */
        cp0_read(13, 0x40);
        cp0_write(12, 0x00400004 | pending);
        cp0_read(13, 0x44);
        cp0_read(12, 0x48);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40) & pending, ==,
                        pending);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44) & pending, ==,
                        pending);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x48), ==,
                        0x00400004 | pending);
        qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture",
                         "external-irq", line, 0);
        cp0_read(13, 0x40);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40) & pending, ==, 0);
    }
    qtest_quit(qts);
}

static void test_count_divider(void)
{
    QTestState *qts = qtest_init("-M malta -m 64M -cpu " FIXTURE " -S");
    g_autoptr(QDict) cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    QDict *cpu = qobject_to(QDict,
        qlist_peek(qdict_get_qlist(cpus, "return")));
    const char *path = qdict_get_str(cpu, "qom-path");
    g_autofree char *input = g_strdup_printf("%s/clk-in", path);
    g_autofree char *count = g_strdup_printf("%s/clk-count", path);
    uint64_t input_period = property(qts, input, "qtest-clock-period");
    uint64_t count_period = property(qts, count, "qtest-clock-period");

    /* Malta supplies 320 MHz; the CPU policy divides that input by two. */
    g_assert_cmpuint(input_period, >, 0);
    g_assert_cmpuint(count_period, ==, input_period * 2);
    qtest_quit(qts);
}

static void test_migration(gconstpointer opaque)
{
    unsigned int variant = GPOINTER_TO_INT(opaque);
    bool mismatch = variant != 0;
    g_autofree char *dir = g_dir_make_tmp("r5000-migration-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    QTestState *from = variant == 2 ? start_stopped("R4000", "") :
                                     start(FIXTURE);
    const char *destination = variant == 1 ? "R5000,r5000-ec=0,r5000-ep=5" :
        variant == 3 ? "R5000,r5000-ec=3,r5000-ep=0" : FIXTURE;
    QTestState *to = start_stopped(destination, incoming);

    if (variant != 2) {
        cp0_write(16, 0x1005);
        cp0_read(16, 0x40);
        execute(from);
    }

    if (mismatch) {
        SgiWait wait;

        qtest_set_expected_status(to, 1);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        sgi_wait_start(&wait, "refused R5000 mode mismatch migration");
        while (qtest_probe_child(to)) {
            sgi_wait_pause(&wait, 1000);
        }
    } else {
        sgi_migrate_stopped(from, uri);
        sgi_wait_migration(to, "completed");
        cp0_read(16, 0x40);
        execute(to);
        g_assert_cmphex(qtest_readl(to, MAILBOX + 0x40), ==,
                        CONFIG_RO | 0x1005);
        qtest_system_reset(to);
        wait_entry(to, 1);
        g_assert_cmphex(qtest_readl(to, MAILBOX + 0x24), ==, CONFIG_RO | 2);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_invalid_modes(void)
{
    static const struct {
        const char *cpu;
        const char *error;
    } cases[] = {
        { "R5000", "r5000-ec must be explicitly supplied" },
        { "R5000,r5000-ep=0", "r5000-ec must be explicitly supplied" },
        { "R5000,r5000-ec=0", "r5000-ep must be explicitly supplied" },
        { "R5000,r5000-ec=7,r5000-ep=0", "r5000-ec must be in 0..6" },
        { "R5000,r5000-ec=0,r5000-ep=9", "r5000-ep must be in 0..8" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        const char *args[] = { qtest_qemu_binary(NULL), "-M", "malta",
            "-cpu", cases[i].cpu, "-display", "none", "-S", NULL };
        g_autofree char *error = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &error, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_test_message("%s: %s", cases[i].cpu, error);
        g_assert_nonnull(strstr(error, cases[i].error));
    }
}

static void test_legal_modes(void)
{
    static const struct { unsigned int ec, ep; } modes[] = {
        { 0, 0 }, { 6, 8 }, { 1, 7 },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(modes); i++) {
        g_autofree char *cpu = g_strdup_printf(
            "R5000,r5000-ec=%u,r5000-ep=%u", modes[i].ec, modes[i].ep);
        QTestState *qts = start(cpu);

        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x24), ==,
                        0x0051e6f2 | modes[i].ec << 28 | modes[i].ep << 24);
        qtest_quit(qts);
    }
}

static void test_warm_reset(gconstpointer opaque)
{
    bool nmi = GPOINTER_TO_INT(opaque);
    QTestState *qts = start(FIXTURE);
    g_autoptr(QDict) cpus = qtest_qmp(qts, "{'execute':'query-cpus-fast'}");
    QDict *cpu = qobject_to(QDict,
        qlist_peek(qdict_get_qlist(cpus, "return")));
    uint8_t loop[16];
    uint32_t resume;
    const uint64_t fpr = UINT64_C(0x0123456789abcdef);
    const uint64_t hi = UINT64_C(0x23456789abcdef01);
    const uint64_t lo = UINT64_C(0x3456789abcdef012);
    const uint64_t s0 = UINT64_C(0x456789abcdef0123);
    const uint64_t s1 = UINT64_C(0x56789abcdef01234);

    /* Seed a real delay-slot Break exception, including the read-only BD. */
    cp0_write(12, 0x24400000);
    cp0_write(11, 0x80000000);
    cp0_write(13, 0x300);
    resume = 0xa0000000 | (next_code + (length + 2) * 4);
    qtest_writel(qts, MAILBOX + 0xa0, resume);
    emit(0x10000001);
    emit(0x0000000d); /* break in the delay slot */
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x90), ==, 0x80000324);

    /* The user loop and another live mapping must survive the warm entry. */
    for (unsigned int i = 0; i < 2; i++) {
        uint32_t phys = 0x200000 + i * 0x100000;

        cp0_write(0, i);
        cp0_write(10, 0x400000 + i * 0x200000);
        cp0_write(2, (phys >> 6) | 0x1f);
        cp0_write(3, ((phys + 0x1000) >> 6) | 0x1f);
        emit(0x42000002);
    }
    cp0_write(16, 0x1003);
    cp0_write(6, 47);
    cp0_write(14, 0x12345678);
    cp0_write(17, 0x89abcdef);
    constant64(8, hi);
    emit(0x01000011); /* mthi t0 */
    constant64(8, lo);
    emit(0x01000013); /* mtlo t0 */
    constant64(8, fpr);
    emit(0x44a85000); /* dmtc1 t0,f10 */
    constant(8, 0x02800003);
    emit(0x44c8f800); /* ctc1 t0,FCR31: conditions and rounding only */
    constant64(16, s0);
    constant64(17, s1);
    cp0_write(30, 0x400000);
    cp0_write(12, 0x24430014); /* CU1/FR, CE/DE, user KSU, ERL, XX=0. */
    emit(0x42000018); /* eret through ErrorEPC, preserving ordinary EPC */
    emit(0);
    submit(qts);
    /* Seed the diagnostic after ERET, which changes common QEMU LL state. */
    stl_be_p(loop, 0x3c080060);     /* t0 = mapped 0x00600000 */
    stl_be_p(loop + 4, 0xc1090000); /* ll t1,0(t0): physical 0x00300000 */
    stl_be_p(loop + 8, 0x1000ffff); /* branch to itself */
    stl_be_p(loop + 12, 0);
    qtest_memwrite(qts, 0x200000, loop, sizeof(loop));
    qtest_writel(qts, 0x300000, 0xfeed1234);
    qtest_qmp_assert_success(qts,
        "{'execute':'x-mips-debug-breakpoint','arguments':"
        "{'cpu-index':0,'pc':4194316}}");
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    qtest_qmp_eventwait(qts, "STOP");
    qtest_qmp_assert_success(qts,
        "{'execute':'x-mips-debug-breakpoint','arguments':"
        "{'cpu-index':0,'pc':4194316,'remove':true}}");
    qtest_writel(qts, MAILBOX + 0xb4, 0);
    if (nmi) {
        qtest_set_irq_in(qts, qdict_get_str(cpu, "qom-path"), "nmi", 0, 1);
        qtest_set_irq_in(qts, qdict_get_str(cpu, "qom-path"), "nmi", 0, 0);
    } else {
        qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture",
                         "soft-reset", 0, 1);
        qtest_set_irq_in(qts, "/machine/peripheral/reset-fixture",
                         "soft-reset", 0, 0);
    }
    wait_entry(qts, 1);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x20), ==, 0x24530014);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x24), ==, CONFIG_RO | 0x1003);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x2c), ==, 47);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0xb4), ==, 1);
    cp0_read(13, 0x44);
    cp0_read(17, 0x48);
    emit(0x4448f800); emit(0xaf48004c); /* FCR31 */
    emit(0x40287000); emit(0xff4800c8); /* ordinary EPC */
    emit(0x44285000); emit(0xff4800d0); /* f10 */
    emit(0x4028f000); emit(0xff4800d8); /* ErrorEPC */
    emit(0x00004010); emit(0xff4800e0); /* HI */
    emit(0x00004012); emit(0xff4800e8); /* LO */
    emit(0xff5000f0); emit(0xff5100f8); /* s0, s1 */
    /* Select mapped kuseg after checking retention. */
    cp0_write(12, 0x24430000);
    constant(9, 0x600000);
    emit(0x8d280000); emit(0xaf480040);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0xfeed1234);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44), ==, 0x80000324);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x48), ==, 0x30000);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x4c), ==, 0x02800003);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xc8), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xd0), ==, fpr);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xd8), ==, 0x400008);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xe0), ==, hi);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xe8), ==, lo);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xf0), ==, s0);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xf8), ==, s1);
    qtest_quit(qts);
}

static void test_tlb(void)
{
    QTestState *qts = start(FIXTURE);

    for (unsigned int i = 0; i < 48; i++) {
        uint32_t phys = 0x100000 + i * 0x2000;

        qtest_writel(qts, phys, 0x12340000 | i);
        qtest_writel(qts, phys + 0x1000, 0x56780000 | i);
        cp0_write(0, i);
        cp0_write(10, 0x400000 + i * 0x2000);
        cp0_write(2, (phys >> 6) | 0x1f);
        cp0_write(3, ((phys + 0x1000) >> 6) | 0x1f);
        emit(0x42000002); /* tlbwi */
        execute(qts);
    }
    cp0_write(16, 3); /* K0 changes must retain architectural TLB entries. */
    cp0_write(12, 0); /* kuseg must now use the architectural TLB. */
    execute(qts);
    for (unsigned int i = 0; i < 48; i++) {
        constant(9, 0x400000 + i * 0x2000);
        emit(0x8d280000); /* lw t0, 0(t1) */
        emit(0xaf480040);
        emit(0x8d281000); /* lw t0, 4096(t1) */
        emit(0xaf480044);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0x12340000 | i);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44), ==, 0x56780000 | i);
    }
    cp0_write(10, 0x45e000);
    emit(0x42000008); /* tlbp */
    cp0_read(0, 0x40);
    cp0_write(0, 48); /* An out-of-range index must not select slot 48. */
    cp0_read(0, 0x44);
    cp0_write(6, 47);
    cp0_read(1, 0x48);
    execute(qts);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x40), ==, 47);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x44), ==, 47);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x48), ==, 47);
    for (unsigned int bits = 0; bits <= 12; bits += 2) {
        uint32_t mask = ((1u << bits) - 1) << 13;

        cp0_write(5, mask);
        cp0_read(5, 0x40);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, mask);
    }
    cp0_write(5, 0x80006000);
    cp0_read(5, 0x40);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0x6000);
    cp0_write(5, 0x2000); /* Unlisted field pattern: selected 4 KiB policy. */
    cp0_read(5, 0x40);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0);
    cp0_write(5, 0xa000); /* A non-contiguous field pattern is also unlisted. */
    cp0_read(5, 0x40);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0);
    cp0_write(2, 0xffffffff);
    cp0_read(2, 0x44);
    cp0_write(5, 0xffffffff);
    cp0_read(5, 0x40);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0x01ffe000);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44), ==, 0x3fffffff);
    qtest_quit(qts);
}

static void test_mips4(void)
{
    QTestState *qts = start(FIXTURE);
    static const uint32_t functions[] = { 0x20, 0x28, 0x30, 0x38 };
    static const uint32_t expected[] = {
        0x40e00000, 0x40a00000, 0xc0e00000, 0xc0a00000,
    };

    cp0_write(12, 0x24400004);
    constant(8, 0x3f800000); /* 1.0 */
    emit(0x44880000);       /* mtc1 t0, f0 */
    constant(8, 0x40000000); /* 2.0 */
    emit(0x44881000);       /* mtc1 t0, f2 */
    constant(8, 0x40400000); /* 3.0 */
    emit(0x44882000);       /* mtc1 t0, f4 */
    for (unsigned int i = 0; i < G_N_ELEMENTS(functions); i++) {
        /* COP1X fd=f6, fr=f0, fs=f2, ft=f4. */
        emit(0x4c000000 | 4 << 16 | 2 << 11 | 6 << 6 | functions[i]);
        emit(0x44083000); /* mfc1 t0, f6 */
        emit(0xaf480040 | i * 4);
    }
    constant(8, 0x12345678);
    constant(9, 1);
    constant(10, 0xabcdef01);
    emit(8 << 21 | 9 << 16 | 10 << 11 | 0x0b); /* movn t2,t0,t1 */
    emit(0xaf4a0050);
    constant(9, 0);
    emit(8 << 21 | 9 << 16 | 11 << 11 | 0x0a); /* movz t3,t0,t1 */
    emit(0xaf4b0054);
    constant(8, 1);
    emit(0x0008403c); /* dsll32 t0,t0,0 */
    emit(0xff480060); /* sd t0,96(k0) */
    constant(9, 0xa0001080);
    emit(0x4c000000 | 9 << 21 | 8 << 6); /* lwxc1 f8,t1(zero) */
    emit(0x44084000); /* mfc1 t0,f8 */
    emit(0xaf480058);
    constant(9, 0xa0001084);
    emit(0x4c000008 | 9 << 21 | 8 << 11); /* swxc1 f8,t1(zero) */
    qtest_writel(qts, MAILBOX + 0x80, 0x43210000);
    execute(qts);
    for (unsigned int i = 0; i < G_N_ELEMENTS(expected); i++) {
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40 + i * 4), ==,
                        expected[i]);
    }
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x50), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x54), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x58), ==, 0x43210000);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x84), ==, 0x43210000);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0x60), ==, 1ULL << 32);
    qtest_quit(qts);
}

static void test_user_mips4(gconstpointer opaque)
{
    unsigned int mode = GPOINTER_TO_INT(opaque);
    bool enabled = mode & 1;
    bool supervisor = mode & 2;
    uint8_t code[8];
    QTestState *qts = start(FIXTURE);
    uint32_t resume;

    stl_be_p(code, supervisor ? 0x640a1234 : 0x0109500b);
    /* daddiu t2,zero,0x1234 in supervisor mode; movn t2,t0,t1 in user. */
    stl_be_p(code + 4, 0x0000000c); /* syscall returns to the kernel fixture */
    qtest_memwrite(qts, 0x200000, code, sizeof(code));
    cp0_write(0, 0);
    cp0_write(10, 0x400000);
    cp0_write(2, (0x200000 >> 6) | 0x1f);
    cp0_write(3, (0x201000 >> 6) | 0x1f);
    emit(0x42000002); /* tlbwi */
    cp0_write(14, 0x400000);
    cp0_write(12, (supervisor ? 0x0040000a : 0x00400012) |
               (enabled ? (supervisor ? 0x40 : 0x80000000u) : 0));
    constant(8, 0x12345678);
    constant(9, 1);
    constant(10, 0x11111111);
    resume = 0xa0000000 | (next_code + (length + 5) * 4);
    constant(11, resume);
    emit(0xaf4b00a0);
    emit(0x42000018); /* eret to the user mapping */
    emit(0);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x90) & 0x7c, ==,
                    enabled ? 0x20 : 0x28);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0xa4), ==,
                    enabled ? (supervisor ? 0x1234 : 0x12345678) :
                              0x11111111);
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0xb0), ==, 1);
    qtest_quit(qts);
}

static void test_address_exception(void)
{
    QTestState *qts = start(FIXTURE);
    uint32_t resume, fault_pc;

    cp0_write(12, 0x00400000);
    constant(9, 0xa0002001);
    resume = 0xa0000000 | (next_code + (length + 4) * 4);
    fault_pc = resume - 4;
    constant(11, resume);
    emit(0xaf4b00a0);
    emit(0x8d280000); /* misaligned lw t0,0(t1) */
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x90) & 0x7c, ==, 0x10);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x94), ==, 0xa0002001);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x98), ==, fault_pc);
    qtest_quit(qts);
}

static void test_register_masks(void)
{
    static const uint32_t lladdr[] = {
        0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
    };
    QTestState *qts = start(FIXTURE);

    for (unsigned int bit = 0; bit < 32; bit++) {
        uint32_t value = 1u << bit;

        cp0_write(12, value | 4); /* ERL keeps the mask test in kernel. */
        cp0_read(12, 0x40);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==,
                        (value & 0xf653ffffu) | 4);
    }
    for (unsigned int i = 0; i < 34; i++) {
        uint32_t value = i < 32 ? 1u << i : i == 32 ? 0 : 0xffffffff;
        uint32_t resume;

        cp0_write(12, 0x24400000); /* CU1/FR, BEV, kernel. */
        constant(8, value);
        emit(0x44c8f800); /* ctc1 t0,FCR31 */
        resume = 0xa0000000 | (next_code + length * 4);
        qtest_writel(qts, MAILBOX + 0xa0, resume);
        emit(0x4448f800); /* cfc1 t0,FCR31 */
        emit(0xaf480040);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==,
                        value & 0xff83ffffu);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(lladdr); i++) {
        cp0_write(17, lladdr[i]);
        cp0_read(17, 0x40);
        execute(qts);
        g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, lladdr[i]);
    }
    constant64(8, UINT64_MAX);
    emit(0x40a88800); /* dmtc0 t0,LLAddr: high input bits are reserved. */
    cp0_read(17, 0x40);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0xffffffff);
    cp0_write(16, 3);
    constant(9, 0x80012340);
    emit(0xc1280000); /* ll t0,0(t1), used only for its diagnostic address. */
    cp0_read(17, 0x40);
    qtest_writel(qts, 0x12340, 0x12345678);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==, 0x1234);
    qtest_quit(qts);
}

static void test_physical_bound(void)
{
    QTestState *qts = start(FIXTURE);
    uint64_t beyond = UINT64_C(0x9000001000000000);
    uint32_t resume;

    cp0_write(12, 0x00400080); /* Kernel extended addressing. */
    constant64(9, UINT64_C(0x9000000ffffffffc));
    resume = 0xa0000000 | (next_code + (length + 1) * 4);
    qtest_writel(qts, MAILBOX + 0xa0, resume);
    emit(0x8d280000);
    emit(0xaf480040);
    execute(qts);
    /* The synthetic MMIO observer covers the last 4 KiB below 2^36. */
    g_assert_cmpuint(qtest_readl(qts, MAILBOX + 0x40), ==, 2);
    constant64(9, beyond);
    resume = 0xa0000000 | (next_code + (length + 1) * 4);
    qtest_writel(qts, MAILBOX + 0xa0, resume);
    emit(0x8d280000);
    execute(qts);
    g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x90) & 0x7c, ==, 0x10);
    g_assert_cmphex(qtest_readq(qts, MAILBOX + 0xc0), ==, beyond);
    qtest_quit(qts);
}

static void test_fp_admission(gconstpointer opaque)
{
    const char *cpu = opaque;
    bool r10000 = !strcmp(cpu, "R10000");
    static const struct {
        const char *name;
        uint32_t opcode;
        uint32_t condition;
        uint32_t result;
        bool r10k_unimplemented;
        bool compare;
        bool load;
        bool store;
        bool control;
    } cases[] = {
        { "movt",  0x46010091, 0, 0x3f800000, true },
        { "movz",  0x46090092, 0, 0x3f800000, true },
        { "movn",  0x46090093, 1, 0x3f800000, true },
        { "recip", 0x46000095, 0, 0x3f800000, true },
        { "rsqrt", 0x46000096, 0, 0x3f800000, true },
        { "compare-cc1", 0x46000132, 0, 0x40000000, true, true },
        { "madd",   0x4c0620a0, 0, 0x40e00000 },
        { "msub",   0x4c0620a8, 0, 0x40a00000 },
        { "nmadd",  0x4c0620b0, 0, 0xc0e00000 },
        { "nmsub",  0x4c0620b8, 0, 0xc0a00000 },
        { "lwxc1",  0x4d000080, 0, 0x40800000, false, false, true },
        { "swxc1",  0x4d000008, 0, 0x40000000, false, false, false, true },
        { "prefx",  0x4d00000f, 0, 0x40000000 },
        { "pref",   0xcd000000, 0, 0x40000000 },
        { "bc1-cc1", 0x45050001, 0, 0x40000000 },
        { "add-control", 0x46040080, 0, 0x40800000,
          false, false, false, false, true },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        for (unsigned int mode = 0; mode < 5; mode++) {
            bool usable = mode != 3;
            bool invalid = mode == 4;
            bool kernel = mode == 2;
            bool xx = mode == 1;
            bool admitted = kernel || xx || cases[i].control;
            uint32_t fcr31 = 0x00800000;
            uint32_t expected_cause;
            uint32_t expected_result = 0x40000000;
            uint32_t expected_memory = 0x40800000;
            uint32_t resume;
            uint8_t user[8];
            QTestState *qts;

            if ((mode == 3 && i != 1) ||
                (invalid && !cases[i].load && !cases[i].store)) {
                continue;
            }
            g_test_message("%s %s: mode %u", cpu, cases[i].name, mode);
            qts = start(cpu);
            stl_be_p(user, cases[i].opcode);
            stl_be_p(user + 4, 0x0000000c); /* syscall back to the fixture */
            qtest_memwrite(qts, 0x200000, user, sizeof(user));
            qtest_writel(qts, 0x300000, 0x40800000);
            cp0_write(12, 0x24400004);
            for (unsigned int n = 0; n < 2; n++) {
                uint32_t phys = 0x200000 + n * 0x100000;

                cp0_write(0, n);
                cp0_write(10, 0x400000 + n * 0x200000);
                cp0_write(2, (phys >> 6) | 0x1f);
                cp0_write(3, ((phys + 0x1000) >> 6) | 0x1f);
                emit(0x42000002);
            }
            constant(8, 0x3f800000); emit(0x44880000); /* f0=1 */
            constant(8, 0x40000000); emit(0x44881000); /* f2=2 */
            constant(8, 0x40400000); emit(0x44882000); /* f4=3 */
            constant(8, 0x40000000); emit(0x44883000); /* f6=2 */
            constant(8, fcr31); emit(0x44c8f800);
            cp0_read(8, 0xd0); /* Preserve the initial BadVAddr observation. */
            cp0_write(14, 0x400000);
            cp0_write(12, 0x04400002 | (usable ? 0x20000000 : 0) |
                       (kernel ? 0 : 0x10) | (xx ? 0x80000000u : 0));
            constant(8, invalid ? 0x80000001 : 0x600000);
            constant(9, cases[i].condition);
            constant(10, 0x11111111);
            resume = 0xa0000000 | (next_code + (length + 5) * 4);
            constant(11, resume);
            emit(0xaf4b00a0);
            emit(0x42000018);
            emit(0);
            /* Re-enable CU1 only after capturing any original refusal. */
            cp0_write(12, 0x24400000);
            emit(0x44081000); emit(0xaf480040); /* f2 */
            emit(0x4448f800); emit(0xaf480044); /* FCR31 */
            execute(qts);
            if (!usable) {
                expected_cause = 0x2c;
            } else if (admitted) {
                expected_cause = 0x20;
                if (cases[i].compare) {
                    fcr31 |= 0x02000000;
                }
            } else if (r10000 && cases[i].r10k_unimplemented) {
                expected_cause = 0x3c;
                fcr31 |= 0x00020000;
            } else {
                expected_cause = 0x28; /* Accepted functional R5000 refusal. */
            }
            if (usable && admitted) {
                expected_result = cases[i].result;
                if (cases[i].store) {
                    expected_memory = 0x3f800000;
                }
            }
            g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x90) & 0x7c, ==,
                            expected_cause);
            g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x40), ==,
                            expected_result);
            g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x44), ==, fcr31);
            g_assert_cmphex(qtest_readl(qts, 0x300000), ==, expected_memory);
            if (!admitted || !usable) {
                g_assert_cmphex(qtest_readl(qts, MAILBOX + 0xa4), ==,
                                0x11111111);
            }
            if (invalid) {
                g_assert_cmphex(qtest_readl(qts, MAILBOX + 0x94), ==,
                                qtest_readl(qts, MAILBOX + 0xd0));
            }
            qtest_quit(qts);
        }
    }
}

int main(int argc, char **argv)
{
    uint8_t image[0x2000] = { 0 };
    int fd, result;
    static const uint32_t code[] = {
        0x3c1aa000, 0x375a1000, /* k0 = uncached mailbox */
        0x24180000, 0x24190001, /* t8=0, t9=1 */
        0x0339c00b, 0xaf5800b4, /* kernel MIPS IV before any Status write */
        0x40086000, 0xaf480020, /* entry Status */
        0x40088000, 0xaf480024, /* entry Config */
        0x40087800, 0xaf480028, /* PRId */
        0x40083000, 0xaf48002c, /* Wired */
        0x40080800, 0xaf480030, /* Random */
        0x8f480008, 0x25080001, 0xaf480008, /* publish entry count */
        0x8f5b0000,            /* request: lw k1, 0(k0) */
        0x1360fffe, 0,        /* beq k1, zero, request */
        0xaf400000,           /* consume request */
        0x0360f809, 0,        /* jalr k1 */
        0x24080001, 0xaf480004, /* completion */
        0x1000fff7, 0,        /* request loop */
    };

    static const uint32_t handler[] = {
        0x24180000, 0x24190001, /* t8=0, t9=1 */
        0x0339c00b, 0xaf5800b0, /* movn t8,t9,t9; kernel admission marker */
        0x40186800, 0xaf580090, /* Cause */
        0x40184000, 0xaf580094, /* BadVAddr */
        0x40384000, 0xff5800c0, /* full-width BadVAddr */
        0x40187000, 0xaf580098, /* EPC */
        0xaf4a00a4,             /* user t2 result */
        0x8f5900a0, 0x40997000, /* return to test-selected kernel address */
        0x40196000,             /* Status, including CU1/FR */
        0x3c18ffff, 0x3718ffe7, /* clear only KSU */
        0x0338c824, 0x40996000,
        0, 0, 0, 0, 0x42000018, 0,
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned int i = 0; i < G_N_ELEMENTS(code); i++) {
        stl_be_p(image + 0x40 + i * 4, code[i]);
    }
    for (unsigned int i = 0; i < G_N_ELEMENTS(handler); i++) {
        stl_be_p(image + 0x380 + i * 4, handler[i]);
    }
    /* Malta owns the board-ID word at ROM offset 0x10. */
    stl_be_p(image, 0x0bf00010); /* j 0xbfc00040 */
    fd = g_file_open_tmp("r5000-cpu-code-XXXXXX", &image_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    qtest_add_func("/r5000/missing-invalid-modes", test_invalid_modes);
    if (qtest_has_accel("tcg") &&
        qtest_has_device("mips-cpu-exception-fixture")) {
        qtest_add_func("/r5000/reset", test_reset);
        qtest_add_func("/r5000/legal-modes", test_legal_modes);
        qtest_add_func("/r5000/immutable-modes", test_immutable_modes);
        qtest_add_data_func("/r5000/soft-reset", GINT_TO_POINTER(0),
                            test_warm_reset);
        qtest_add_data_func("/r5000/nmi", GINT_TO_POINTER(1), test_warm_reset);
        qtest_add_func("/r5000/tlb", test_tlb);
        qtest_add_func("/r5000/mips4", test_mips4);
        qtest_add_data_func("/r5000/fp-admission", FIXTURE, test_fp_admission);
        qtest_add_data_func("/r5000/r10000-fp-admission", "R10000",
                            test_fp_admission);
        qtest_add_data_func("/r5000/user-mips4-disabled", GINT_TO_POINTER(0),
                            test_user_mips4);
        qtest_add_data_func("/r5000/user-mips4-enabled", GINT_TO_POINTER(1),
                            test_user_mips4);
        qtest_add_func("/r5000/address-exception", test_address_exception);
        qtest_add_func("/r5000/register-masks", test_register_masks);
        qtest_add_func("/r5000/physical-bound", test_physical_bound);
        qtest_add_data_func("/r5000/supervisor64-disabled", GINT_TO_POINTER(2),
                            test_user_mips4);
        qtest_add_data_func("/r5000/supervisor64-enabled", GINT_TO_POINTER(3),
                            test_user_mips4);
        qtest_add_func("/r5000/config", test_config);
        qtest_add_func("/r5000/kseg0-attributes", test_kseg0_attributes);
        qtest_add_func("/r5000/count-divider", test_count_divider);
        qtest_add_func("/r5000/timer-wrap-compare", test_timer);
        qtest_add_func("/r5000/status", test_status);
        qtest_add_func("/r5000/external-pending", test_external_pending);
        qtest_add_data_func("/r5000/migration", GINT_TO_POINTER(0),
                            test_migration);
        qtest_add_data_func("/r5000/migration-mode-mismatch",
                            GINT_TO_POINTER(1), test_migration);
        qtest_add_data_func("/r5000/migration-missing-profile",
                            GINT_TO_POINTER(2), test_migration);
        qtest_add_data_func("/r5000/migration-ep-mismatch",
                            GINT_TO_POINTER(3), test_migration);
    }
    result = g_test_run();
    unlink(image_path);
    g_free(image_path);
    return result;
}
