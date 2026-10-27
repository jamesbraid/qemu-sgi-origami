/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Bedrock processor interface's pending banks, polled by guest code.
 *
 * While the stock IP35 PROM works on one processor, the others wait in a loop
 * that reads both pending banks of their own processor interface. With four
 * processors that is three threads reading device registers as fast as TCG
 * runs them. Each read used to take the BQL, so the working processor waited
 * behind them for every one of its own device accesses: its graphics console
 * ran at about a fifth of its one-processor speed.
 *
 * A qtest access holds the BQL and carries no processor, so only guest code
 * can show which reads take the lock. The processors here run a PROM image
 * written below and report through their real-time compare registers and
 * node memory.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"
#include "sgi-wait.h"

#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define BEDROCK_PI1 UINT64_C(0x200000)
#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_PENDING_MODIFY UINT64_C(0x90)
#define PI_PENDING_0 UINT64_C(0x98)
#define PI_PENDING_1 UINT64_C(0xa0)
#define PI_RT_COMPARE_A UINT64_C(0x108)

/* Install node zero's memory as the stock PROM leaves it. */
#define SN1_MD_MEMORY_CONFIG (SPACE_IO + UINT64_C(0x01800000) + 0x780008)
#define SN1_MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

#define POLL_INTERVAL_US 10000

#define R_ZERO 0
#define R_T0 8  /* this processor's own interface, uncached */
#define R_T1 9  /* the other interface, uncached */
#define R_T2 10 /* the mailbox, cached node memory */
#define R_T3 11
#define R_T4 12
#define R_T5 13
#define R_T6 14
#define R_T7 15
#define R_S0 16
#define R_S2 18

#define OPC_BEQ 0x10000000u
#define OPC_BNE 0x14000000u

typedef struct Program {
    uint8_t *image;
    unsigned int pc;
} Program;

static void emit(Program *p, uint32_t insn)
{
    stl_be_p(p->image + p->pc, insn);
    p->pc += 4;
}

/* A branch to @target from the instruction at the current position. */
static void emit_branch(Program *p, uint32_t op, int rs, int rt,
                        unsigned int target)
{
    int32_t offset = ((int32_t)target - (int32_t)(p->pc + 4)) / 4;

    emit(p, mips_branch(op, rs, rt, offset));
    emit(p, mips_nop());
}

/* A forward branch whose target patch_branch() fills in. */
static unsigned int emit_branch_fwd(Program *p, uint32_t op, int rs, int rt)
{
    unsigned int at = p->pc;

    emit(p, mips_branch(op, rs, rt, 0));
    emit(p, mips_nop());
    return at;
}

static void patch_branch(Program *p, unsigned int at)
{
    uint32_t insn = ldl_be_p(p->image + at);
    int32_t offset = ((int32_t)p->pc - (int32_t)(at + 4)) / 4;

    stl_be_p(p->image + at, (insn & 0xffff0000u) | (uint16_t)offset);
}

/* Load a 32-bit unsigned value. */
static void emit_li(Program *p, int reg, uint32_t value)
{
    emit(p, mips_lui(reg, value >> 16));
    emit(p, mips_ori(reg, reg, value & 0xffff));
}

/*
 * The reset vector jumps over the exception vectors, which spin, to the
 * code at 0x400. That starts with 64-bit kernel addressing, without which
 * the uncached pointers fault, and points t0 at this processor's own
 * interface through the local form, which the requester's interface answers.
 */
static void emit_prologue(Program *p)
{
    emit(p, mips_branch(OPC_BEQ, R_ZERO, R_ZERO, 0xff));
    emit(p, mips_nop());
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(p->image + offset, mips_branch(OPC_BEQ, R_ZERO, R_ZERO, -1));
    }
    p->pc = 0x400;

    emit(p, mips_mfc0(R_T4, 12));
    emit(p, mips_ori(R_T4, R_T4, 0x80));
    emit(p, mips_mtc0(R_T4, 12));
    emit(p, mips_nop());

    emit(p, mips_lui(R_T0, 0x9200));
    emit(p, mips_dsll32(R_T0, R_T0));
    emit(p, mips_lui(R_T3, 0x0100));
    emit(p, mips_daddu(R_T0, R_T0, R_T3));
}

/* Publish @mark in this processor's own real-time compare, then stop. */
static void emit_finish(Program *p, uint16_t mark)
{
    unsigned int self;

    emit(p, mips_ori(R_T3, R_ZERO, mark));
    emit(p, mips_sd(R_T3, R_T0, PI_RT_COMPARE_A));
    self = p->pc;
    emit_branch(p, OPC_BEQ, R_ZERO, R_ZERO, self);
}

static char *write_prom(const uint8_t *image)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-pi-polled-read-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *start(const char *prom, unsigned int cpus)
{
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;

    /* The accelerator named here wins over the -accel qtest one. */
    s = qtest_initf("-M origin300 -cpu R14000 -smp %u -m 512M -bios %s "
                    "-accel tcg,thread=multi -no-shutdown -S", cpus, quoted);
    qtest_writeq(s, SN1_MD_MEMORY_CONFIG, SN1_MD_INSTALLED_SELECT2);
    return s;
}

/* Wait for @mark in the compare register at @address. */
static void wait_mark(QTestState *s, uint64_t address, uint64_t mark)
{
    SgiWait w;

    sgi_wait_start(&w, "mark 0x%" PRIx64 " at 0x%" PRIx64, mark, address);
    while (qtest_readq(s, address) != mark) {
        sgi_wait_pause(&w, POLL_INTERVAL_US);
    }
}

/*
 * Big-lock acquisitions since the last "sync-profile reset", except the main
 * loop's own: those grow with how long the guest takes and say nothing about
 * what its accesses do.
 */
static uint64_t bql_acquisitions(QTestState *s)
{
    g_autofree char *report = qtest_hmp(s, "info sync-profile 200");
    g_auto(GStrv) lines = g_strsplit(report, "\n", -1);
    uint64_t total = 0;

    for (unsigned int i = 0; lines[i]; i++) {
        g_auto(GStrv) split = g_strsplit_set(lines[i], " \t", -1);
        const char *field[8];
        unsigned int n = 0;

        for (unsigned int j = 0; split[j] && n < G_N_ELEMENTS(field); j++) {
            if (split[j][0]) {
                field[n++] = split[j];
            }
        }
        /* "BQL mutex  <object>  <call site>  <wait>  <count>  <mean>" */
        if (n == 7 && g_str_equal(field[0], "BQL") &&
            g_str_equal(field[1], "mutex") &&
            !g_str_has_prefix(field[3], "util/main-loop.c:")) {
            total += g_ascii_strtoull(field[5], NULL, 10);
        }
    }
    return total;
}

/*
 * One processor makes POLLED_READS doubleword reads of one register of its
 * own interface. A pending bank is read without the lock; the CPU number
 * register, the control, keeps it and must count at least one acquisition a
 * read, so a profile that misses device locking cannot pass the other two.
 */
#define POLLED_READS 100000u
#define POLLED_MARK 0x5a

typedef struct PolledRead {
    uint64_t offset;
    bool lockless;
} PolledRead;

static const PolledRead pending_0 = { PI_PENDING_0, true };
static const PolledRead pending_1 = { PI_PENDING_1, true };
static const PolledRead cpu_number = { PI_CPU_NUMBER, false };

static void test_polled_read_lock(const void *data)
{
    const PolledRead *r = data;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *prom = NULL;
    Program p = { .image = image };
    unsigned int idle, loop;
    uint64_t locks;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    emit_prologue(&p);
    /* The machine has two processors; the one on interface 1 just waits. */
    emit(&p, mips_ld(R_T3, R_T0, PI_CPU_NUMBER));
    emit(&p, mips_andi(R_T3, R_T3, 2));
    idle = p.pc;
    emit_branch(&p, OPC_BNE, R_T3, R_ZERO, idle);
    emit_li(&p, R_T7, POLLED_READS);
    loop = p.pc;
    emit(&p, mips_ld(R_T6, R_T0, r->offset));
    emit(&p, mips_daddiu(R_T7, R_T7, -1));
    emit_branch(&p, OPC_BNE, R_T7, R_ZERO, loop);
    emit_finish(&p, POLLED_MARK);
    prom = write_prom(image);

    s = start(prom, 2);
    g_free(qtest_hmp(s, "sync-profile on"));
    g_free(qtest_hmp(s, "sync-profile reset"));
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    wait_mark(s, BEDROCK_LOCAL + PI_RT_COMPARE_A, POLLED_MARK);
    locks = bql_acquisitions(s);
    g_test_message("big-lock acquisitions over %u reads of 0x%" PRIx64
                   ": %" PRIu64, POLLED_READS, r->offset, locks);

    if (r->lockless) {
        g_assert_cmpuint(locks, <, POLLED_READS / 10);
    } else {
        g_assert_cmpuint(locks, >=, POLLED_READS);
    }

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The PROM passes messages through pending bits: one processor writes a
 * word to memory, then sets or clears a bit through the pending-modify
 * register, and the other, polling its bank, reads the word once it sees the
 * bit move. The poll no longer takes the lock that used to order it after
 * the store, so the bank's own publication has to. Each round, the
 * processor in slot A of interface 0 writes the round number, flips a bit of
 * interface 1's bank 0 and waits for an acknowledgement; the processor of
 * interface 1 counts every round in which the word it reads after seeing the
 * flip is not that round's. A sync separates the sender's word and its
 * modify store, and TCG turns that into a host barrier, so this exercises
 * the reader's acquire; on a host with total store order it passes without
 * one. It still runs the lock-free reads against locked writes on two
 * threads for a thread sanitizer to watch.
 */
#define MAILBOX_PHYS 0x600000
#define MAILBOX_WORD 0x00
#define MAILBOX_ACK 0x08
#define MAILBOX_STALE 0x10
#define MAILBOX_ROUNDS 20000u
#define MAILBOX_BIT 40u     /* bank 0, so not a reflected bank-1 bit */
#define MAILBOX_SENDER_MARK 0xa0
#define MAILBOX_READER_MARK 0xa1

static void test_pending_mailbox_orders_the_word(void)
{
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    g_autofree char *prom = NULL;
    Program p = { .image = image };
    unsigned int reader, sender_loop, ack, reader_loop, poll, fresh;
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    emit_prologue(&p);
    /* t1: interface 1, the reader's; t2: the mailbox, cached. */
    emit(&p, mips_lui(R_T3, BEDROCK_PI1 >> 16));
    emit(&p, mips_daddu(R_T1, R_T0, R_T3));
    emit(&p, mips_lui(R_T2, 0xa800));
    emit(&p, mips_dsll32(R_T2, R_T2));
    emit(&p, mips_lui(R_T3, MAILBOX_PHYS >> 16));
    emit(&p, mips_daddu(R_T2, R_T2, R_T3));
    emit(&p, mips_ori(R_S0, R_ZERO, 1));
    emit_li(&p, R_S2, MAILBOX_ROUNDS + 1);
    /* The CPU number's bit 1 is the interface: 1 reads, 0 sends. */
    emit(&p, mips_ld(R_T3, R_T0, PI_CPU_NUMBER));
    emit(&p, mips_andi(R_T3, R_T3, 2));
    reader = emit_branch_fwd(&p, OPC_BNE, R_T3, R_ZERO);

    /* Sender: the word, a sync, then the bit; odd rounds set it. */
    sender_loop = p.pc;
    emit(&p, mips_sd(R_S0, R_T2, MAILBOX_WORD));
    emit(&p, mips_sync());
    emit(&p, mips_andi(R_T3, R_S0, 1));
    emit(&p, mips_dsll(R_T3, R_T3, 8));
    emit(&p, mips_ori(R_T3, R_T3, MAILBOX_BIT));
    emit(&p, mips_sd(R_T3, R_T1, PI_PENDING_MODIFY));
    ack = p.pc;
    emit(&p, mips_ld(R_T4, R_T2, MAILBOX_ACK));
    emit_branch(&p, OPC_BNE, R_T4, R_S0, ack);
    emit(&p, mips_daddiu(R_S0, R_S0, 1));
    emit_branch(&p, OPC_BNE, R_S0, R_S2, sender_loop);
    emit_finish(&p, MAILBOX_SENDER_MARK);

    /* Reader: poll its own bank 0 for the flip, then read the word. */
    patch_branch(&p, reader);
    reader_loop = p.pc;
    emit(&p, mips_andi(R_T5, R_S0, 1));
    emit(&p, mips_dsll(R_T5, R_T5, MAILBOX_BIT - 32));
    poll = p.pc;
    emit(&p, mips_ld(R_T6, R_T0, PI_PENDING_0));
    emit(&p, mips_dsrl32(R_T6, R_T6));
    emit(&p, mips_andi(R_T6, R_T6, 1u << (MAILBOX_BIT - 32)));
    emit_branch(&p, OPC_BNE, R_T6, R_T5, poll);
    emit(&p, mips_ld(R_T7, R_T2, MAILBOX_WORD));
    fresh = emit_branch_fwd(&p, OPC_BEQ, R_T7, R_S0);
    emit(&p, mips_ld(R_T4, R_T2, MAILBOX_STALE));
    emit(&p, mips_daddiu(R_T4, R_T4, 1));
    emit(&p, mips_sd(R_T4, R_T2, MAILBOX_STALE));
    patch_branch(&p, fresh);
    emit(&p, mips_sd(R_S0, R_T2, MAILBOX_ACK));
    emit(&p, mips_sync());
    emit(&p, mips_daddiu(R_S0, R_S0, 1));
    emit_branch(&p, OPC_BNE, R_S0, R_S2, reader_loop);
    emit_finish(&p, MAILBOX_READER_MARK);
    prom = write_prom(image);

    s = start(prom, 2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    wait_mark(s, BEDROCK_LOCAL + PI_RT_COMPARE_A, MAILBOX_SENDER_MARK);
    wait_mark(s, BEDROCK_LOCAL + BEDROCK_PI1 + PI_RT_COMPARE_A,
              MAILBOX_READER_MARK);

    g_assert_cmpuint(qtest_readq(s, MAILBOX_PHYS + MAILBOX_ACK), ==,
                     MAILBOX_ROUNDS);
    g_assert_cmpuint(qtest_readq(s, MAILBOX_PHYS + MAILBOX_STALE), ==, 0);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn1/pi-polled-read/pending-0-stays-off-the-lock",
                        &pending_0, test_polled_read_lock);
    qtest_add_data_func("sn1/pi-polled-read/pending-1-stays-off-the-lock",
                        &pending_1, test_polled_read_lock);
    qtest_add_data_func("sn1/pi-polled-read/cpu-number-keeps-the-lock",
                        &cpu_number, test_polled_read_lock);
    qtest_add_func("sn1/pi-polled-read/pending-mailbox-orders-the-word",
                   test_pending_mailbox_orders_the_word);
    return g_test_run();
}
