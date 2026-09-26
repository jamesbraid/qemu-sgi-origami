/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Exercise CPU-local physical access using the two independent Hub adapters.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define R12000_CPU_1 "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567," \
    "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576," \
    "r12000-scache-block-words=16"
#define R12000_CPU_2 "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"
#define R12000_CPU_1_32 "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567," \
    "r12000-reset-mode=0x19a683,r12000-scache-bytes=1048576," \
    "r12000-scache-block-words=32"
#define R12000_MAILBOX 0x00200000

#define DATA_BASE 0x20001220000ULL
#define MACHINE_SINGLE \
    "-M origin200 -m 128M -cpu R10000 -accel tcg,thread=multi -S"
#define MACHINE_DUAL \
    "-M origin200,topology=origin200-dual -m 128M " \
    "-smp 4"

static void write_register(QTestState *qts, unsigned int cpu, uint64_t addr,
                           uint8_t value)
{
    uint8_t bytes[8] = { 0 };

    bytes[7] = value;
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint8_t read_register(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    for (unsigned int i = 0; i < 7; i++) {
        g_assert_cmphex(bytes[i], ==, 0);
    }
    return bytes[7];
}

static void test_cpu_local_registers(void)
{
    QTestState *qts = qtest_init(MACHINE_DUAL);
    const uint8_t sentinel[8] = { 0xaa, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t invalid[8];
    uint8_t expected[8] = { 0 };
    uint8_t actual[8];

    /*
     * Disable serial operation before selecting each adapter's own address.
     */
    for (unsigned int node = 0; node < 2; node++) {
        write_register(qts, node * 2, DATA_BASE + 8, 0x80);
        write_register(qts, node * 2, DATA_BASE, 0x10 + node);
    }
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE), ==, 0x10 + cpu / 2);
    }
    expected[7] = 0x10;
    qtest_memread(qts, DATA_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    expected[7] = 0x20;
    qtest_memwrite(qts, DATA_BASE, expected, sizeof(expected));
    g_assert_cmphex(read_register(qts, 0, DATA_BASE), ==, 0x20);
    g_assert_cmphex(read_register(qts, 1, DATA_BASE), ==, 0x20);
    g_assert_cmphex(read_register(qts, 2, DATA_BASE), ==, 0x11);

    memcpy(invalid, sentinel, sizeof(invalid));
    g_assert_false(qtest_memwrite_cpu(qts, 4, DATA_BASE,
                                     invalid, sizeof(invalid)));
    g_assert_false(qtest_memread_cpu(qts, UINT_MAX, DATA_BASE,
                                    invalid, sizeof(invalid)));
    g_assert_cmpmem(invalid, sizeof(invalid), sentinel, sizeof(sentinel));
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE),
                        ==, cpu < 2 ? 0x20 : 0x11);
    }
    g_assert_true(qtest_memread_cpu(qts, UINT_MAX, DATA_BASE, invalid, 0));
    g_assert_true(qtest_memwrite_cpu(qts, UINT_MAX, DATA_BASE, invalid, 0));

    qtest_system_reset(qts);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE), ==, 0);
        g_assert_cmphex(read_register(qts, cpu, DATA_BASE + 8), ==, 0x80);
    }
    qtest_quit(qts);
}

/* Independent instructions, not code copied from a guest image. */
#define USER_IMAGE_SIZE 0x2000
#define USER_TEXT       0x00400000
#define USER_PHYS       0x00200000
#define USER_MAILBOX    (USER_PHYS + 0xc00)
#define NODE_ID_PHYS    0x02000000

typedef struct UserCode {
    uint8_t bytes[USER_IMAGE_SIZE];
    unsigned int pos;
} UserCode;

static void user_emit(UserCode *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, sizeof(p->bytes));
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static void user_i(UserCode *p, unsigned int op, unsigned int rs,
                   unsigned int rt, uint16_t imm)
{
    user_emit(p, (op << 26) | (rs << 21) | (rt << 16) | imm);
}

static void user_li(UserCode *p, unsigned int rt, uint32_t value)
{
    user_i(p, 15, 0, rt, value >> 16);
    user_i(p, 13, rt, rt, value);
}

static void user_li64(UserCode *p, unsigned int rt, uint64_t value)
{
    user_i(p, 13, 0, rt, value >> 48);
    for (unsigned int shift = 32; ; shift -= 16) {
        user_emit(p, (rt << 16) | (rt << 11) | (16 << 6) | 0x38);
        user_i(p, 13, rt, rt, value >> shift);
        if (shift == 0) {
            break;
        }
    }
}

static void user_mtc0(UserCode *p, unsigned int rt, unsigned int rd)
{
    user_emit(p, 0x40800000 | (rt << 16) | (rd << 11));
    /* Keep the synthetic sequence valid without a later ISA's EHB. */
    for (unsigned int i = 0; i < 4; i++) {
        user_emit(p, 0);
    }
}

static void user_mfc0(UserCode *p, unsigned int rt, unsigned int rd)
{
    user_emit(p, 0x40000000 | (rt << 16) | (rd << 11));
}

static unsigned int user_branch(UserCode *p, unsigned int op,
                                 unsigned int rs, unsigned int rt)
{
    unsigned int at = p->pos;

    user_i(p, op, rs, rt, 0);
    user_emit(p, 0);
    return at;
}

static void user_patch(UserCode *p, unsigned int at, unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    g_assert_cmpint(delta, >=, INT16_MIN);
    g_assert_cmpint(delta, <=, INT16_MAX);
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

static void user_park(UserCode *p)
{
    user_emit(p, 0x1000ffff); /* beq zero, zero, self */
    user_emit(p, 0);
}

static void user_wait(UserCode *p, unsigned int base, unsigned int offset,
                      unsigned int value)
{
    unsigned int loop, back;

    user_i(p, 9, 0, 10, value);
    loop = p->pos;
    user_i(p, 35, base, 8, offset);
    back = user_branch(p, 5, 8, 10);
    user_patch(p, back, loop);
    user_emit(p, 0x0000000f);
}

static void user_sync_call(UserCode *p)
{
    user_i(p, 9, 16, 4, 0);
    user_i(p, 9, 0, 2, 1);
    user_emit(p, 0x0000000c);
}

static void build_user_visibility(UserCode *p, bool remote)
{
    unsigned int wrong_cause, completed, wrong_call, parked, loop, back;
    unsigned int user_size, primary = 0, installed = 0;

    /* Reset and every boot exception vector have explicit destinations. */
    user_emit(p, 0x100001ff); /* reset -> offset 0x800 */
    user_emit(p, 0);
    for (unsigned int off = 0x200; off <= 0x380; off += 0x80) {
        p->pos = off;
        back = user_branch(p, 4, 0, 0);
        user_patch(p, back, 0x400);
    }

    p->pos = 0x1000;
    user_li(p, 16, USER_TEXT + 0x800); /* s0: generated function */
    user_li(p, 17, USER_TEXT + 0xc00); /* s1: mapped result mailbox */
    user_li(p, 8, 0x24021357);        /* addiu v0, zero, A */
    user_i(p, 43, 16, 8, 0);
    user_li(p, 8, 0x03e00008);        /* jr ra */
    user_i(p, 43, 16, 8, 4);
    user_i(p, 43, 16, 0, 8);         /* delay-slot nop */
    for (unsigned int phase = 0; phase < 2; phase++) {
        if (phase) {
            user_li(p, 8, 0x24022468); /* addiu v0, zero, B */
            user_i(p, 43, 16, 8, 0);
        }
        user_sync_call(p);
        if (remote) {
            /* Publish after synchronization, then wait for CPU2's ack. */
            user_emit(p, 0x0000000f);
            user_i(p, 9, 0, 8, phase + 1);
            user_i(p, 43, 17, 8, 40);
            user_wait(p, 17, 44, phase + 1);
        } else {
            user_emit(p, (16u << 21) | (31u << 11) | 9); /* jalr ra,s0 */
            user_emit(p, 0);
            user_i(p, 43, 17, 2, 4 + phase * 4);
        }
    }
    user_i(p, 9, 0, 2, 2);           /* syscall 2: finish */
    user_emit(p, 0x0000000c);
    user_park(p);
    if (remote) {
        g_assert_cmpuint(p->pos, <=, 0x1200);
        p->pos = 0x1200;
        user_li(p, 16, USER_TEXT + 0x800);
        user_li(p, 17, USER_TEXT + 0xc40); /* CPU2's distinct mailbox */
        user_li(p, 19, USER_TEXT + 0xc00); /* shared publication/ack */
        for (unsigned int phase = 0; phase < 2; phase++) {
            user_wait(p, 19, 40, phase + 1);
            user_sync_call(p);
            user_emit(p, (16u << 21) | (31u << 11) | 9);
            user_emit(p, 0);
            user_i(p, 43, 17, 2, 4 + phase * 4);
            user_emit(p, 0x0000000f);
            user_i(p, 9, 0, 8, phase + 1);
            user_i(p, 43, 19, 8, 44);
        }
        user_i(p, 9, 0, 2, 2);
        user_emit(p, 0x0000000c);
        user_park(p);
    }
    user_size = p->pos - 0x1000;

    /* A small privileged syscall handler; unexpected exceptions stop here. */
    p->pos = 0x400;
    user_li(p, 26, 0xa0000000 | USER_MAILBOX);
    user_emit(p, (18u << 16) | (8u << 11) | (5u << 6)); /* sll t0,s2,5 */
    user_emit(p, (26u << 21) | (8u << 16) | (26u << 11) | 0x21);
    user_i(p, 43, 26, 18, 0);       /* executing CPU's fixture identity */
    user_emit(p, 0x40000000 | (27u << 16) | (13u << 11)); /* Cause */
    user_i(p, 12, 27, 8, 0x7c);
    user_i(p, 9, 0, 9, 8 << 2);     /* architectural syscall */
    wrong_cause = user_branch(p, 5, 8, 9);
    user_i(p, 9, 0, 9, 2);
    completed = user_branch(p, 4, 2, 9);
    user_i(p, 9, 0, 9, 1);
    wrong_call = user_branch(p, 5, 2, 9);
    /* Linux r4kcache.h uses writeback-invalidate, not Hit_Writeback_D. */
    user_emit(p, 0x0000000f);        /* sync */
    user_i(p, 47, 4, 0x15, 0);     /* Hit_Writeback_Inv_D */
    user_i(p, 47, 4, 0x10, 0);     /* Hit_Invalidate_I */
    user_i(p, 47, 4, 0x14, 0);     /* R10000 Cache Barrier */
    user_emit(p, 0x0000000f);
    user_i(p, 35, 26, 8, 12);
    user_i(p, 9, 8, 8, 1);
    user_i(p, 43, 26, 8, 12);      /* synchronization count */
    user_emit(p, 0x40000000 | (8u << 16) | (14u << 11));
    user_i(p, 9, 8, 8, 4);
    user_mtc0(p, 8, 14);
    user_emit(p, 0x42000018);       /* eret */
    user_emit(p, 0);

    user_patch(p, completed, p->pos);
    user_emit(p, 0x40000000 | (8u << 16) | (12u << 11));
    user_i(p, 43, 26, 8, 32);      /* saved user KSU at syscall */
    user_i(p, 9, 0, 8, 0x55);
    user_i(p, 43, 26, 8, 28);
    user_park(p);
    user_patch(p, wrong_cause, p->pos);
    user_patch(p, wrong_call, p->pos);
    user_i(p, 43, 26, 27, 16);
    user_emit(p, 0x40000000 | (8u << 16) | (14u << 11));
    user_i(p, 43, 26, 8, 20);      /* unexpected EPC */
    user_emit(p, 0x40000000 | (8u << 16) | (8u << 11));
    user_i(p, 43, 26, 8, 24);      /* unexpected BadVAddr */
    user_i(p, 9, 0, 8, 0xff);
    user_i(p, 43, 26, 8, 28);
    user_park(p);
    g_assert_cmpuint(p->pos, <=, 0x800);

    p->pos = 0x800;
    user_li(p, 8, 0x00400080);     /* BEV, KX; no ERL */
    user_mtc0(p, 8, 12);
    user_li(p, 8, 0x92000000);
    user_emit(p, (8u << 16) | (8u << 11) | 0x3c); /* dsll32 t0,t0,0 */
    user_li(p, 9, 0x01000020);
    user_emit(p, (8u << 21) | (9u << 16) | (8u << 11) | 0x25);
    user_i(p, 55, 8, 9, 0);       /* local processor number */
    user_li(p, 8, 0xa0000000 | NODE_ID_PHYS);
    user_i(p, 35, 8, 8, 0);       /* host-seeded node identity */
    user_emit(p, (8u << 21) | (9u << 16) | (8u << 11) | 0x25);
    /* s2 survives the user/kernel transitions */
    user_i(p, 9, 8, 18, 0);
    if (remote) {
        user_i(p, 12, 8, 8, 1);   /* keep CPU0 and CPU2, park CPU1 and CPU3 */
    }
    parked = user_branch(p, 5, 8, 0);

    if (remote) {
        primary = user_branch(p, 4, 18, 0);
        /*
         * CPU2 assigns node1 and removes its low local alias before mapping.
         */
        user_li(p, 8, 0x92000000);
        user_emit(p, (8u << 16) | (8u << 11) | 0x3c);
        user_li(p, 9, 0x01600000);
        user_emit(p, (8u << 21) | (9u << 16) | (8u << 11) | 0x25);
        user_li(p, 9, (1u << 17) | (1u << 8));
        user_i(p, 63, 8, 9, 0);   /* NI_STATUS: programmed NASID1 */
        user_li(p, 8, 0x92000000);
        user_emit(p, (8u << 16) | (8u << 11) | 0x3c);
        user_li(p, 9, 0x01000028);
        user_emit(p, (8u << 21) | (9u << 16) | (8u << 11) | 0x25);
        /* PI_CALIAS_SIZE: use physical node claims */
        user_i(p, 63, 8, 0, 0);
        user_li(p, 19, 0xa0000000 | USER_MAILBOX);
        /* CPU0 has copied the shared user program */
        user_wait(p, 19, 60, 1);
        installed = user_branch(p, 4, 0, 0);
        user_patch(p, primary, p->pos);
    }

    user_li(p, 8, 0xbfc01000);
    user_li(p, 9, 0xa0000000 | USER_PHYS);
    user_li(p, 10, 0xbfc01000 + user_size);
    loop = p->pos;
    user_i(p, 35, 8, 11, 0);
    user_i(p, 43, 9, 11, 0);
    user_i(p, 9, 8, 8, 4);
    user_i(p, 9, 9, 9, 4);
    back = user_branch(p, 5, 8, 10);
    user_patch(p, back, loop);
    if (remote) {
        user_li(p, 19, 0xa0000000 | USER_MAILBOX);
        user_emit(p, 0x0000000f);
        user_i(p, 9, 0, 8, 1);
        user_i(p, 43, 19, 8, 60);
        user_patch(p, installed, p->pos);
    }
    user_mtc0(p, 0, 0);           /* Index */
    user_mtc0(p, 0, 5);           /* PageMask: 4 KiB */
    user_li(p, 8, USER_TEXT);
    user_mtc0(p, 8, 10);          /* EntryHi */
    user_li(p, 8, (USER_PHYS >> 6) | 0x1f); /* C=3, D,V,G */
    user_mtc0(p, 8, 2);
    user_i(p, 9, 0, 8, 1);       /* unused odd half: G only */
    user_mtc0(p, 8, 3);
    user_emit(p, 0x42000002);     /* tlbwi */
    for (unsigned int i = 0; i < 4; i++) {
        user_emit(p, 0);
    }
    user_li(p, 8, USER_TEXT);
    if (remote) {
        user_emit(p, (18u << 16) | (9u << 11) | (8u << 6)); /* CPU2: +0x200 */
        user_emit(p, (8u << 21) | (9u << 16) | (8u << 11) | 0x21);
    }
    user_mtc0(p, 8, 14);
    user_li(p, 8, 0x00400012);    /* BEV, user KSU, EXL */
    user_mtc0(p, 8, 12);
    user_emit(p, 0x42000018);
    user_emit(p, 0);
    user_patch(p, parked, p->pos);
    user_park(p);
    g_assert_cmpuint(p->pos, <=, 0x1000);
}

static void run_user_code_first_call(bool remote)
{
    g_autofree UserCode *image = g_new0(UserCode, 1);
    g_autofree char *path = NULL;
    uint8_t mailbox[100] = { 0 }, identity[4], node1_code[4];
    unsigned int result = remote ? 64 : 0;
    QTestState *qts;
    int fd;
    int64_t deadline;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_user_visibility(image, remote);
    fd = g_file_open_tmp("sn0-user-code-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image->bytes, sizeof(image->bytes)), ==,
                    sizeof(image->bytes));
    close(fd);
    qts = qtest_initf(MACHINE_DUAL " -cpu R10000 -bios %s "
                      "-accel tcg,thread=multi -S", path);
    unlink(path);
    stl_be_p(identity, 0);
    g_assert_true(qtest_memwrite_cpu(qts, 0, NODE_ID_PHYS, identity, 4));
    stl_be_p(identity, 2);
    g_assert_true(qtest_memwrite_cpu(qts, 2, NODE_ID_PHYS, identity, 4));
    if (remote) {
        stl_be_p(node1_code, 0xdeadbeef);
        g_assert_true(qtest_memwrite_cpu(qts, 2, USER_PHYS + 0x800,
                                         node1_code, sizeof(node1_code)));
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    do {
        g_assert_true(qtest_memread_cpu(qts, 0, USER_MAILBOX,
                                        mailbox, sizeof(mailbox)));
        if (ldl_be_p(mailbox + 28) && ldl_be_p(mailbox + result + 28)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    for (unsigned int i = 0; i < sizeof(mailbox); i += 4) {
        g_test_message("mailbox[%u]=0x%08x", i, ldl_be_p(mailbox + i));
    }
    if (remote) {
        g_assert_true(qtest_memread_cpu(qts, 0,
                         0x100000000ULL + USER_PHYS + 0x800,
                         node1_code, sizeof(node1_code)));
        g_test_message("node1 code sentinel=0x%08x", ldl_be_p(node1_code));
    }
    qtest_quit(qts);
    g_assert_cmphex(ldl_be_p(mailbox + 28), ==, 0x55);
    g_assert_cmphex(ldl_be_p(mailbox + 16), ==, 0);
    g_assert_cmphex(ldl_be_p(mailbox + result + 28), ==, 0x55);
    g_assert_cmphex(ldl_be_p(mailbox + result + 16), ==, 0);
    g_assert_cmphex(ldl_be_p(mailbox + result), ==, remote ? 2 : 0);
    g_assert_cmphex(ldl_be_p(mailbox + result + 4), ==, 0x1357);
    g_assert_cmphex(ldl_be_p(mailbox + result + 8), ==, 0x2468);
    g_assert_cmpuint(ldl_be_p(mailbox + 12), ==, 2);
    g_assert_cmpuint(ldl_be_p(mailbox + result + 12), ==, 2);
    g_assert_cmphex(ldl_be_p(mailbox + result + 32) & 0x18, ==, 0x10);
    if (remote) {
        g_assert_cmphex((uint32_t)ldl_be_p(node1_code), ==,
                        UINT32_C(0xdeadbeef));
        g_assert_cmpuint(ldl_be_p(mailbox + 40), ==, 2);
        g_assert_cmpuint(ldl_be_p(mailbox + 44), ==, 2);
    }
}

static void test_user_code_first_call(void)
{
    run_user_code_first_call(false);
}

static void test_remote_user_code_first_call(void)
{
    run_user_code_first_call(true);
}

#define WORD_LOAD_DATA     (USER_PHYS + 0x100)
#define WORD_LOAD_MAILBOX  (USER_PHYS + 0xc00)
#define WORD_LOAD_DONE     0x1f0
#define WORD_LOAD_RESULTS  22

static const uint64_t word_load_expected[WORD_LOAD_RESULTS] = {
    UINT64_C(0x0000000012345678), /* LWL clear, offset 0 */
    UINT64_C(0x00000000345678d4), /* LWL clear, offset 1 */
    UINT64_C(0x000000005678c3d4), /* LWL clear, offset 2 */
    UINT64_C(0x0000000078b2c3d4), /* LWL clear, offset 3 */
    UINT64_C(0xffffffff89abcdef), /* LWL set, offset 0 */
    UINT64_C(0xffffffffabcdefd4), /* LWL set, offset 1 */
    UINT64_C(0xffffffffcdefc3d4), /* LWL set, offset 2 */
    UINT64_C(0xffffffffefb2c3d4), /* LWL set, offset 3 */
    UINT64_C(0x0000000012345678), /* full LWR, bit 31 clear */
    UINT64_C(0xffffffff89abcdef), /* full LWR, bit 31 set */
    UINT64_C(0x1122334471b2c312), /* partial LWR 0, old bit 31 clear */
    UINT64_C(0x1122334471b21234), /* partial LWR 1, old bit 31 clear */
    UINT64_C(0x1122334471123456), /* partial LWR 2, old bit 31 clear */
    UINT64_C(0x55667788a1b2c312), /* partial LWR 0, old bit 31 set */
    UINT64_C(0x55667788a1b21234), /* partial LWR 1, old bit 31 set */
    UINT64_C(0x55667788a1123456), /* partial LWR 2, old bit 31 set */
    UINT64_C(0x0000000012345678), /* clear pair, LWL then LWR */
    UINT64_C(0x0000000012345678), /* clear pair, LWR then LWL */
    UINT64_C(0xffffffff89abcdef), /* set pair, LWL then LWR */
    UINT64_C(0xffffffff89abcdef), /* set pair, LWR then LWL */
    UINT64_C(0x0000000071b2c312), /* 32-bit mode, old bit 31 clear */
    UINT64_C(0xffffffffa1b2c312), /* 32-bit mode, old bit 31 set */
};

static const uint64_t word_load_default_partial[6] = {
    UINT64_C(0x0000000071b2c312),
    UINT64_C(0x0000000071b21234),
    UINT64_C(0x0000000071123456),
    UINT64_C(0xffffffffa1b2c312),
    UINT64_C(0xffffffffa1b21234),
    UINT64_C(0xffffffffa1123456),
};

static const char *const word_load_names[WORD_LOAD_RESULTS] = {
    "lwl-clear-0", "lwl-clear-1", "lwl-clear-2", "lwl-clear-3",
    "lwl-set-0", "lwl-set-1", "lwl-set-2", "lwl-set-3",
    "lwr-full-clear", "lwr-full-set",
    "lwr-partial-0-old-clear", "lwr-partial-1-old-clear",
    "lwr-partial-2-old-clear", "lwr-partial-0-old-set",
    "lwr-partial-1-old-set", "lwr-partial-2-old-set",
    "pair-clear-lwl-lwr", "pair-clear-lwr-lwl",
    "pair-set-lwl-lwr", "pair-set-lwr-lwl",
    "lwr-32bit-old-clear", "lwr-32bit-old-set",
};

static void word_load_result(UserCode *p, unsigned int *result,
                             uint64_t initial, unsigned int op,
                             unsigned int offset)
{
    user_li64(p, 8, initial);
    user_i(p, op, 16, 8, offset);
    user_i(p, 63, 17, 8, (*result)++ * 8);
}

static void build_word_load_test(UserCode *p)
{
    const uint64_t initial = UINT64_C(0x11223344a1b2c3d4);
    unsigned int second, result = 0;

    user_emit(p, 0x100001ff); /* reset -> offset 0x800 */
    user_emit(p, 0);
    p->pos = 0x380;
    user_emit(p, 0x1000001f); /* general exception -> offset 0x400 */
    user_emit(p, 0);

    /* Save both 32-bit-register-mode results from the syscall handler. */
    p->pos = 0x400;
    second = user_branch(p, 5, 27, 0);
    user_i(p, 63, 26, 8, 20 * 8);
    user_i(p, 9, 0, 27, 1);
    user_li64(p, 8, UINT64_C(0x55667788a1b2c3d4));
    user_emit(p, 0x40000000 | (9u << 16) | (14u << 11)); /* EPC */
    user_i(p, 9, 9, 9, 4);
    user_mtc0(p, 9, 14);
    user_emit(p, 0x42000018); /* eret */
    user_emit(p, 0);
    user_patch(p, second, p->pos);
    user_i(p, 63, 26, 8, 21 * 8);
    user_i(p, 9, 0, 9, 1);
    user_i(p, 43, 26, 9, WORD_LOAD_DONE);
    user_park(p);

    p->pos = 0x800;
    user_li(p, 8, 0x00400080); /* BEV, KX; no ERL */
    user_mtc0(p, 8, 12);
    user_li(p, 16, 0xa0000000 | WORD_LOAD_DATA);
    user_li(p, 17, 0xa0000000 | WORD_LOAD_MAILBOX);

    for (unsigned int offset = 0; offset < 4; offset++) {
        word_load_result(p, &result, initial, 34, offset);
    }
    for (unsigned int offset = 0; offset < 4; offset++) {
        word_load_result(p, &result, initial, 34, 8 + offset);
    }
    word_load_result(p, &result, initial, 38, 3);
    word_load_result(p, &result, initial, 38, 11);
    for (unsigned int offset = 0; offset < 3; offset++) {
        word_load_result(p, &result, UINT64_C(0x1122334471b2c3d4),
                         38, offset);
    }
    for (unsigned int offset = 0; offset < 3; offset++) {
        word_load_result(p, &result, UINT64_C(0x55667788a1b2c3d4),
                         38, offset);
    }

    user_li64(p, 8, initial);
    user_i(p, 34, 16, 8, 17);
    user_i(p, 38, 16, 8, 20);
    user_i(p, 63, 17, 8, result++ * 8);
    user_li64(p, 8, initial);
    user_i(p, 38, 16, 8, 20);
    user_i(p, 34, 16, 8, 17);
    user_i(p, 63, 17, 8, result++ * 8);
    user_li64(p, 8, initial);
    user_i(p, 34, 16, 8, 33);
    user_i(p, 38, 16, 8, 36);
    user_i(p, 63, 17, 8, result++ * 8);
    user_li64(p, 8, initial);
    user_i(p, 38, 16, 8, 36);
    user_i(p, 34, 16, 8, 33);
    user_i(p, 63, 17, 8, result++ * 8);
    g_assert_cmpuint(result, ==, 20);

    /* Map one page, then repeat a partial LWR with 64-bit GPRs disabled. */
    user_mtc0(p, 0, 0);           /* Index */
    user_mtc0(p, 0, 5);           /* PageMask: 4 KiB */
    user_li(p, 8, USER_TEXT);
    user_mtc0(p, 8, 10);          /* EntryHi */
    user_li(p, 8, (USER_PHYS >> 6) | 0x1f); /* C=3, D,V,G */
    user_mtc0(p, 8, 2);
    user_i(p, 9, 0, 8, 1);       /* unused odd half: G only */
    user_mtc0(p, 8, 3);
    user_emit(p, 0x42000002);     /* tlbwi */
    for (unsigned int i = 0; i < 4; i++) {
        user_emit(p, 0);
    }
    user_li(p, 16, USER_TEXT + 0x100);
    user_li(p, 26, 0xa0000000 | WORD_LOAD_MAILBOX);
    user_i(p, 9, 0, 27, 0);
    user_li64(p, 8, UINT64_C(0x1122334471b2c3d4));
    user_li(p, 9, USER_TEXT + 0x800);
    user_mtc0(p, 9, 14);
    user_li(p, 9, 0x00400092);    /* BEV, KX, user KSU, EXL; no UX */
    user_mtc0(p, 9, 12);
    user_emit(p, 0x42000018);     /* eret */
    user_emit(p, 0);
    g_assert_cmpuint(p->pos, <=, 0x1000);
}

static void run_word_load_test(const char *machine, const char *cpu,
                               bool preserves_upper)
{
    g_autofree UserCode *image = g_new0(UserCode, 1);
    g_autofree char *path = NULL;
    uint8_t data[64] = {
        0x12, 0x34, 0x56, 0x78,
        [8] = 0x89, 0xab, 0xcd, 0xef,
        [16] = 0xee, 0x12, 0x34, 0x56, 0x78,
        [32] = 0xee, 0x89, 0xab, 0xcd, 0xef,
    };
    uint8_t mailbox[0x200] = { 0 };
    uint8_t user_code[16] = { 0 };
    QTestState *qts;
    int64_t deadline;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_word_load_test(image);
    stl_be_p(user_code, (38u << 26) | (16u << 21) | (8u << 16));
    stl_be_p(user_code + 4, 0x0000000c); /* syscall */
    stl_be_p(user_code + 8, (38u << 26) | (16u << 21) | (8u << 16));
    stl_be_p(user_code + 12, 0x0000000c); /* syscall */
    fd = g_file_open_tmp("sn0-word-loads-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image->bytes, sizeof(image->bytes)), ==,
                    sizeof(image->bytes));
    close(fd);
    qts = qtest_initf("%s -cpu %s -bios %s -accel tcg,thread=multi -S",
                      machine, cpu, path);
    unlink(path);
    qtest_memwrite(qts, WORD_LOAD_DATA, data, sizeof(data));
    qtest_memwrite(qts, USER_PHYS + 0x800, user_code, sizeof(user_code));
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    do {
        qtest_memread(qts, WORD_LOAD_MAILBOX, mailbox, sizeof(mailbox));
        if (ldl_be_p(mailbox + WORD_LOAD_DONE)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    qtest_quit(qts);
    g_assert_cmphex(ldl_be_p(mailbox + WORD_LOAD_DONE), ==, 1);
    for (unsigned int i = 0; i < WORD_LOAD_RESULTS; i++) {
        uint64_t expected = word_load_expected[i];
        uint64_t actual = ldq_be_p(mailbox + i * 8);

        if (!preserves_upper && i >= 10 && i < 16) {
            expected = word_load_default_partial[i - 10];
        }
        if (actual != expected) {
            g_test_message("%s: expected 0x%016" PRIx64
                           ", got 0x%016" PRIx64,
                           word_load_names[i], expected, actual);
            g_test_fail();
        }
    }
}

static void test_r10000_word_loads(void)
{
    run_word_load_test(MACHINE_DUAL, "R10000", true);
}

static void test_r14000_word_loads(void)
{
    run_word_load_test("-M malta -m 64M -nic none -vga none", "R14000",
                       true);
}

/* CP0 register numbers used by the timer image below. */
#define CP0_COUNT      9
#define CP0_COMPARE    11
#define CP0_CAUSE      13
#define CAUSE_IP7      (1u << 15)
#define TIMER_MAILBOX  (USER_PHYS + 0x1000)
#define TIMER_TARGET   5000000

/*
 * Compare = Count - 1 arms a value that is behind the live counter by
 * construction: Count only advances between the read and the write, so the
 * value written is never ahead of it. The manual's continuous comparator
 * (14.8) fires on equality, not on "at or past", so this write must leave IP7
 * clear rather than raise it - the disarming pattern OpenBSD's mips64 clock
 * code depends on (sys/arch/mips64/mips64/clock.c, cp0_startclock).
 */
static void build_timer_register_test(UserCode *p)
{
    unsigned int loop, back;

    user_li(p, 26, 0xa0000000 | TIMER_MAILBOX);
    user_mtc0(p, 0, CP0_COMPARE);
    user_mtc0(p, 0, CP0_COUNT);
    user_mfc0(p, 9, CP0_CAUSE);
    user_i(p, 43, 26, 9, 0);          /* pending equality match */
    user_mfc0(p, 8, CP0_COUNT);
    user_i(p, 9, 8, 8, (uint16_t)-1); /* t0 = Count - 1 */
    user_mtc0(p, 8, CP0_COMPARE);
    user_mfc0(p, 9, CP0_CAUSE);
    user_i(p, 43, 26, 9, 4);          /* cleared by past Compare write */

    user_mtc0(p, 0, CP0_COUNT);
    user_li(p, 10, TIMER_TARGET);
    user_mtc0(p, 10, CP0_COMPARE);
    user_mfc0(p, 9, CP0_CAUSE);
    user_i(p, 43, 26, 9, 8);          /* future Compare is still clear */
    user_mfc0(p, 8, CP0_COUNT);
    user_i(p, 43, 26, 8, 12);         /* prove the sample preceded Compare */
    loop = p->pos;

    user_mfc0(p, 8, CP0_COUNT);
    user_emit(p, (8u << 21) | (10u << 16) | (11u << 11) | 0x2b);
    back = user_branch(p, 5, 11, 0);
    user_patch(p, back, loop);
    user_i(p, 43, 26, 8, 16);
    user_mfc0(p, 9, CP0_CAUSE);
    user_i(p, 43, 26, 9, 20);         /* equality match after deadline */
    user_mfc0(p, 8, CP0_COUNT);
    user_i(p, 9, 8, 8, (uint16_t)-1);
    user_mtc0(p, 8, CP0_COMPARE);
    user_mfc0(p, 9, CP0_CAUSE);
    user_i(p, 43, 26, 9, 24);         /* clearing a delivered match */
    user_i(p, 9, 0, 9, 1);
    user_i(p, 43, 26, 9, 28);
    user_park(p);
}

static void run_cpu_timer_test(const char *machine, const char *cpu)
{
    g_autofree UserCode *image = g_new0(UserCode, 1);
    g_autofree char *path = NULL;
    uint8_t mailbox[32] = { 0 };
    QTestState *qts;
    int64_t deadline;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    build_timer_register_test(image);
    fd = g_file_open_tmp("sn0-cpu-timer-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image->bytes, sizeof(image->bytes)), ==,
                    sizeof(image->bytes));
    close(fd);
    qts = qtest_initf("%s -cpu %s -bios %s -accel tcg,thread=single "
                      "-icount shift=10,sleep=off -S",
                      machine, cpu, path);
    unlink(path);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    do {
        qtest_memread(qts, TIMER_MAILBOX, mailbox, sizeof(mailbox));
        if (ldl_be_p(mailbox + 28)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    qtest_quit(qts);
    g_assert_cmpuint(ldl_be_p(mailbox + 28), ==, 1);
    g_assert_cmphex(ldl_be_p(mailbox) & CAUSE_IP7, ==, CAUSE_IP7);
    g_assert_cmphex(ldl_be_p(mailbox + 4) & CAUSE_IP7, ==, 0);
    g_assert_cmphex(ldl_be_p(mailbox + 8) & CAUSE_IP7, ==, 0);
    g_assert_cmpuint(ldl_be_p(mailbox + 12), <, TIMER_TARGET);
    g_assert_cmpuint(ldl_be_p(mailbox + 16), >=, TIMER_TARGET);
    g_assert_cmphex(ldl_be_p(mailbox + 20) & CAUSE_IP7, ==, CAUSE_IP7);
    g_assert_cmphex(ldl_be_p(mailbox + 24) & CAUSE_IP7, ==, 0);
}

static void test_r10000_timer(void)
{
    run_cpu_timer_test("-M origin200 -m 128M", "R10000");
}

static void test_r14000_timer(void)
{
    run_cpu_timer_test("-M malta -m 64M -nic none -vga none", "R14000");
}

static void r12000_emit_reg(UserCode *p, unsigned int rt, unsigned int rd,
                           unsigned int offset)
{
    user_mfc0(p, rt, rd);
    for (unsigned int i = 0; i < 4; i++) {
        user_emit(p, 0);
    }
    user_i(p, 43, 26, rt, offset);
}

static void r12000_build_probe(UserCode *p, bool wide_block)
{
    uint32_t upper = wide_block ? 0xa00f : 0xa007;
    uint16_t lower = 0xff81;
    unsigned int loop;

    user_li(p, 26, 0xa0000000 | R12000_MAILBOX);
    r12000_emit_reg(p, 8, 15, 0);
    r12000_emit_reg(p, 8, 16, 4);
    user_mfc0(p, 9, 12);
    user_li(p, 10, 0x20000000);
    user_emit(p, (9 << 21) | (10 << 16) | (9 << 11) | 0x25);
    user_mtc0(p, 9, 12);
    user_emit(p, 0);
    user_emit(p, 0x44480000); /* cfc1 t0, fcr0 */
    user_i(p, 43, 26, 8, 8);

    /* Use the upper half of the diagnostic table, beyond R10000's 512. */
    user_li(p, 8, (1536 << 3) | (2 << 14) | 1);
    user_mtc0(p, 8, 22);
    user_li(p, 8, 1536 << 3);
    user_mtc0(p, 8, 22);
    r12000_emit_reg(p, 8, 22, 12);

    /* Two addresses occupy one 128-byte line but two 64-byte lines. */
    user_i(p, 15, 0, 9, upper);
    user_i(p, 13, 9, 9, lower);
    user_li(p, 8, 0x11111111);
    user_mtc0(p, 8, 28);
    user_li(p, 8, 0x80000002);
    user_mtc0(p, 8, 29);
    user_i(p, 47, 9, 0x0b, 0);
    user_i(p, 9, 9, 9, 64);
    user_li(p, 8, 0x22222222);
    user_mtc0(p, 8, 28);
    user_i(p, 47, 9, 0x0b, 0);
    user_i(p, 9, 9, 9, (uint16_t)-64);
    user_i(p, 47, 9, 0x07, 0);
    r12000_emit_reg(p, 8, 28, 16);
    user_i(p, 9, 9, 9, 64);
    user_i(p, 47, 9, 0x07, 0);
    r12000_emit_reg(p, 8, 28, 20);
    user_i(p, 9, 0, 8, 1);
    user_i(p, 43, 26, 8, 24);
    loop = p->pos;
    user_i(p, 35, 26, 8, 28);
    user_i(p, 4, 8, 0, (uint16_t)-2);
    user_emit(p, 0);
    user_li(p, 8, 1536 << 3);
    user_mtc0(p, 8, 22);
    r12000_emit_reg(p, 8, 22, 32);
    user_i(p, 15, 0, 9, upper);
    user_i(p, 13, 9, 9, lower);
    user_i(p, 47, 9, 0x07, 0);
    r12000_emit_reg(p, 8, 28, 36);
    user_i(p, 9, 0, 8, 1);
    user_i(p, 43, 26, 8, 40);
    user_park(p);
    g_assert_cmpuint(loop, <, p->pos);
}

static void r12000_run_profile(const char *profile, bool wide_block,
                              uint32_t expected_prid, uint32_t expected_mode,
                              uint32_t expected_fpu)
{
    g_autofree UserCode *image = g_new0(UserCode, 1);
    g_autofree char *path = NULL;
    uint8_t mailbox[28] = { 0 };
    QTestState *qts;
    int64_t deadline;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    r12000_build_probe(image, wide_block);
    fd = g_file_open_tmp("r12000-probe-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image->bytes, sizeof(image->bytes)), ==,
                    sizeof(image->bytes));
    close(fd);
    qts = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                      "-bios %s -accel tcg -S", profile, path);
    unlink(path);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    do {
        qtest_memread(qts, R12000_MAILBOX, mailbox, sizeof(mailbox));
        if (ldl_be_p(mailbox + 24)) {
            break;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    qtest_quit(qts);
    g_assert_cmphex(ldl_be_p(mailbox + 24), ==, 1);
    g_assert_cmphex(ldl_be_p(mailbox), ==, expected_prid);
    g_assert_cmphex(ldl_be_p(mailbox + 4) & 0x1ffffff, ==, expected_mode);
    g_assert_cmphex(ldl_be_p(mailbox + 4) >> 26, ==, 0x1b);
    g_assert_cmphex(ldl_be_p(mailbox + 8), ==, expected_fpu);
    g_assert_cmphex((ldl_be_p(mailbox + 12) >> 14) & 3, ==, 2);
    g_assert_cmphex(ldl_be_p(mailbox + 16), ==, wide_block ? 0x22220022 :
                                                      0x11110111);
    g_assert_cmphex(ldl_be_p(mailbox + 20), ==, 0x22220022);
}

static void test_r12000_profile_16(void)
{
    r12000_run_profile(R12000_CPU_1, false, 0xe12, 0x198683, 0x4567);
}

static void test_r12000_profile_32(void)
{
    r12000_run_profile(R12000_CPU_2, true, 0xe24, 0x1aa683, 0x7654);
}

static void test_r12000_requires_profile(void)
{
    const char *const args[] = { qtest_qemu_binary(NULL), "-M", "malta",
        "-cpu", "R12000", "-display", "none", "-S", NULL };
    g_autofree char *stderr_text = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    g_assert_nonnull(strstr(stderr_text, "r12000-prid"));
}

static void test_r12000_rejects_inconsistent_profile(void)
{
    static const struct {
        const char *profile;
        const char *error;
    } cases[] = {
        { "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,"
          "r12000-reset-mode=0x198683,r12000-scache-bytes=2097152,"
          "r12000-scache-block-words=16", "Config SS" },
        { "R12000,r12000-prid=0xe12,r12000-fpu-id=0x4567,"
          "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576,"
          "r12000-scache-block-words=32", "Config SB" },
        { "R12000,r12000-prid=0xd12,r12000-fpu-id=0x4567,"
          "r12000-reset-mode=0x198683,r12000-scache-bytes=1048576,"
          "r12000-scache-block-words=16", "implementation 0x0e" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        const char *const args[] = { qtest_qemu_binary(NULL), "-M", "malta",
            "-cpu", cases[i].profile, "-display", "none", "-S", NULL };
        g_autofree char *stderr_text = NULL;
        int status;

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                                   NULL, &stderr_text, &status, NULL));
        g_assert_false(g_spawn_check_exit_status(status, NULL));
        g_assert_nonnull(strstr(stderr_text, cases[i].error));
    }
}

static char *r12000_probe_image(bool wide_block)
{
    g_autofree UserCode *image = g_new0(UserCode, 1);
    char *path = NULL;
    int fd;

    r12000_build_probe(image, wide_block);
    fd = g_file_open_tmp("r12000-migrate-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image->bytes, sizeof(image->bytes)), ==,
                    sizeof(image->bytes));
    close(fd);
    return path;
}

static void r12000_wait_mailbox(QTestState *qts, unsigned int offset)
{
    uint8_t value[4];
    int64_t deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    do {
        qtest_memread(qts, R12000_MAILBOX + offset, value, sizeof(value));
        if (ldl_be_p(value) == 1) {
            return;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_not_reached();
}

static void r12000_migrate_profile(const char *profile, bool wide_block)
{
    g_autofree char *image = r12000_probe_image(wide_block);
    g_autofree char *dir = g_dir_make_tmp("r12000-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t value[4];
    QTestState *from, *to;

    from = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                       "-bios %s -accel tcg -S", profile, image);
    to = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                     "-bios %s -accel tcg -incoming %s -S",
                     profile, image, uri);
    qtest_qmp_assert_success(from, "{ 'execute': 'cont' }");
    r12000_wait_mailbox(from, 24);
    qtest_qmp_assert_success(from, "{ 'execute': 'stop' }");
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    stl_be_p(value, 1);
    qtest_memwrite(to, R12000_MAILBOX + 28, value, sizeof(value));
    qtest_qmp_assert_success(to, "{ 'execute': 'cont' }");
    r12000_wait_mailbox(to, 40);
    qtest_memread(to, R12000_MAILBOX + 32, value, sizeof(value));
    g_assert_cmphex((ldl_be_p(value) >> 14) & 3, ==, 2);
    qtest_memread(to, R12000_MAILBOX + 36, value, sizeof(value));
    g_assert_cmphex(ldl_be_p(value), ==, wide_block ? 0x22220022 :
                                                0x11110111);
    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_r12000_migrate_16(void)
{
    r12000_migrate_profile(R12000_CPU_1, false);
}

static void test_r12000_migrate_32(void)
{
    r12000_migrate_profile(R12000_CPU_2, true);
}

static void test_r12000_migrate_rejects_different_block_size(void)
{
    g_autofree char *image = r12000_probe_image(false);
    g_autofree char *dir = g_dir_make_tmp("r12000-mismatch-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "state.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from, *to;
    int64_t deadline;

    from = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                       "-bios %s -accel tcg -S", R12000_CPU_1, image);
    to = qtest_initf("-M malta -m 64M -nic none -vga none -cpu %s "
                     "-bios %s -accel tcg -incoming %s -S",
                     R12000_CPU_1_32, image, uri);
    qtest_qmp_assert_success(from, "{ 'execute': 'cont' }");
    r12000_wait_mailbox(from, 24);
    qtest_qmp_assert_success(from, "{ 'execute': 'stop' }");
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/qtest/cpu-local-registers", test_cpu_local_registers);
    qtest_add_func("/sn0/qtest/user-code-first-call",
                   test_user_code_first_call);
    qtest_add_func("/sn0/qtest/remote-user-code-first-call",
                   test_remote_user_code_first_call);
    qtest_add_func("/sn0/qtest/r10000-word-loads", test_r10000_word_loads);
    if (qtest_has_machine("malta")) {
        qtest_add_func("/sn0/qtest/r14000-word-loads", test_r14000_word_loads);
    }
    qtest_add_func("/sn0/qtest/r10000-timer", test_r10000_timer);
    if (qtest_has_machine("malta")) {
        qtest_add_func("/sn0/qtest/r14000-timer", test_r14000_timer);
        qtest_add_func("/r12000/profile-required",
                       test_r12000_requires_profile);
        qtest_add_func("/r12000/profile-inconsistent",
                       test_r12000_rejects_inconsistent_profile);
        qtest_add_func("/r12000/profile-16-words", test_r12000_profile_16);
        qtest_add_func("/r12000/profile-32-words", test_r12000_profile_32);
        qtest_add_func("/r12000/migrate-16-words", test_r12000_migrate_16);
        qtest_add_func("/r12000/migrate-32-words", test_r12000_migrate_32);
        qtest_add_func("/r12000/migrate-incompatible-block",
                       test_r12000_migrate_rejects_different_block_size);
    }
    return g_test_run();
}
