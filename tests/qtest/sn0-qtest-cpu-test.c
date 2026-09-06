/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise CPU-local physical access using the two independent Hub adapters. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define DATA_BASE 0x20001220000ULL
#define MACHINE_DUAL \
    "-M origin200 -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

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

    /* Disable serial operation before selecting each adapter's own address. */
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

static void user_mtc0(UserCode *p, unsigned int rt, unsigned int rd)
{
    user_emit(p, 0x40800000 | (rt << 16) | (rd << 11));
    /* Keep the synthetic sequence valid without a later ISA's EHB. */
    for (unsigned int i = 0; i < 4; i++) {
        user_emit(p, 0);
    }
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
    user_i(p, 9, 8, 18, 0);       /* s2 survives the user/kernel transitions */
    if (remote) {
        user_i(p, 12, 8, 8, 1);   /* keep CPU0 and CPU2, park CPU1 and CPU3 */
    }
    parked = user_branch(p, 5, 8, 0);

    if (remote) {
        primary = user_branch(p, 4, 18, 0);
        /* CPU2 assigns node1 and removes its low local alias before mapping. */
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
        user_i(p, 63, 8, 0, 0);   /* PI_CALIAS_SIZE: use physical node claims */
        user_li(p, 19, 0xa0000000 | USER_MAILBOX);
        user_wait(p, 19, 60, 1);  /* CPU0 has copied the shared user program */
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/qtest/cpu-local-registers", test_cpu_local_registers);
    qtest_add_func("/sn0/qtest/user-code-first-call",
                   test_user_code_first_call);
    qtest_add_func("/sn0/qtest/remote-user-code-first-call",
                   test_remote_user_code_first_call);
    return g_test_run();
}
