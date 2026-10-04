/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA qtests for the graphics credits the board returns to the Hub.
 *
 * A credit is charged by a processor's uncached store into its slot's
 * graphics page, on the processor's own path through its Hub, which a
 * qtest access never takes: so every store the credits are about is made
 * by a running processor. It serves a mailbox in node 0's memory, a count
 * of stores, an address, a value and a width, and zeroes the count when
 * done. The board is KONA as a -device at the empty slot IO4, crossbow0's
 * widget 11, which the machine links to node 0's Hub; the driver's
 * node-widget map sends widget 11's credits to slot A.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"
#include "sgi-migrate.h"
#include "sn0-user-image.h"

#define KONA_WIDGET         11
#define SMALL_WINDOW_SIZE   UINT64_C(0x1000000)
#define KONA_BASE   (SPACE_IO + (uint64_t)KONA_WIDGET * SMALL_WINDOW_SIZE)
#define KONA_REG(offset)    (KONA_BASE + (offset))
#define REG_INT_CAUSE       KONA_REG(0x800238)
#define REG_MAILBOX         KONA_REG(0x800240)
#define REG_COMMAND         KONA_REG(0xc10000)
#define INT_CAUSE_MAILBOX   0x10

/* The command page through node 0's uncached I/O space, as IRIX maps it. */
#define COMMAND_IO          UINT64_C(0x920000000bc10000)
/* The 128 KiB graphics page holding it, bits 39:17 of its address. */
#define COMMAND_PAGE        UINT64_C(0x0bc00000)

#define HUB                 (SPACE_IO + 0x01800000)
#define PI_GFX_PAGE(s)      (0x300 + 0x28 * (s))
#define PI_GFX_CREDIT(s)    (0x308 + 0x28 * (s))
#define IO_GFX_INIT(n)      (0x400140 + 8 * (n))
#define PI_GFX_PAGE_ENABLE  (1ULL << 40)
/* Map entry: valid, slot, node, widget (Linux sn0/hubio.h IIO_IGFX_INIT). */
#define MAP(node, slot, widget) \
    ((1ULL << 20) | ((uint64_t)(slot) << 16) | ((uint64_t)(node) << 4) | \
     (widget))

/* The driver's total for one pipe, as it writes the counter at attach. */
#define TOTAL               101

/* The mailbox: count, address, value, and 8 for a doubleword store. */
#define MAILBOX             0x50000u
#define ENTRY               0x800

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M -cpu R10000 " \
    "-accel tcg,thread=multi -no-reboot -no-shutdown -S " \
    "-device sgi-kona,id=kona0,slot=io4,fabric-hub-widget=9 "

/* Pool and list for the display-list case, in node 0's memory. */
#define POOL_BASE           UINT64_C(0x40000)
#define LIST_CALL_POOL      0x16eu
#define DMA_POOL            0x1edu

static void build_store_server(SN0Image *p)
{
    unsigned int at, loop, store, word, next;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }
    p->pos = ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(MAILBOX));

    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T1, 0);        /* count */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T2, 8);        /* address */
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_A0, 16);       /* value */
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_A1, 24);       /* width */
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_ZERO, SN0_R_A2, 8);
    store = p->pos;
    word = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_A1, SN0_R_A2);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_T2, SN0_R_A0, 0);
    next = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, word, p->pos);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_T2, SN0_R_A0, 0);
    sn0_patch_branch(p, next, p->pos);
    sn0_emit_i(p, 25, SN0_R_T1, SN0_R_T1, 0xffff);           /* daddiu -1 */
    at = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, store);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, 0);      /* done */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
}

/* Source and destination load the same synthetic store-server firmware. */
static QTestState *store_server(const char *uri)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    build_store_server(image);
    path = sn0_image_write(image, "sn0-kona-pi-credit");
    qts = qtest_initf("%s-bios %s %s %s", MACHINE_ARGS, path,
                       uri ? "-incoming" : "", uri ? uri : "");
    unlink(path);
    return qts;
}

/*
 * A machine whose processor serves the mailbox, with slot A's page over
 * the command page, its counter at the driver's total and the map
 * sending widget 11's credits to slot A, as the driver's attach leaves
 * them.
 */
static QTestState *start(void)
{
    QTestState *qts = store_server(NULL);

    qtest_writeq(qts, HUB + PI_GFX_PAGE(0), PI_GFX_PAGE_ENABLE | COMMAND_PAGE);
    qtest_writeq(qts, HUB + PI_GFX_CREDIT(0), TOTAL);
    qtest_writeq(qts, HUB + IO_GFX_INIT(0), MAP(0, 0, KONA_WIDGET));
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    return qts;
}

/* The processor makes one store of 'width' bytes of 'value' at 'addr'. */
static void store(QTestState *qts, uint64_t addr, uint64_t value,
                  unsigned width)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    qtest_writeq(qts, MAILBOX + 8, addr);
    qtest_writeq(qts, MAILBOX + 16, value);
    qtest_writeq(qts, MAILBOX + 24, width);
    qtest_writeq(qts, MAILBOX, 1);
    while (qtest_readq(qts, MAILBOX)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void store_word(QTestState *qts, uint32_t value)
{
    store(qts, COMMAND_IO, value, 4);
}

static uint64_t credit(QTestState *qts)
{
    return qtest_readq(qts, HUB + PI_GFX_CREDIT(0));
}

/* The PROM's sync probe 1 9 0 was answered on the mailbox. */
static void assert_probe_answered(QTestState *qts)
{
    g_assert_cmphex(qtest_readl(qts, REG_INT_CAUSE) & INT_CAUSE_MAILBOX, ==,
                    INT_CAUSE_MAILBOX);
    g_assert_cmphex(qtest_readl(qts, REG_MAILBOX), ==, 0x1234abcd);
}

/*
 * Each store on the command page costs a credit and earns it back: the
 * sync word at once, since no packet holds it, and the probe's 9 with its
 * 0, the packet's last word. Midway the counter is one short, the 9 the
 * framer still holds. A board that returned nothing leaves the counter
 * three below the total the driver waits for.
 */
static void test_command_page_stores_return_credits(void)
{
    QTestState *qts = start();

    store_word(qts, 1);
    g_assert_cmpuint(credit(qts), ==, TOTAL);
    store_word(qts, 9);
    g_assert_cmpuint(credit(qts), ==, TOTAL - 1);
    store_word(qts, 0);
    g_assert_cmpuint(credit(qts), ==, TOTAL);
    assert_probe_answered(qts);
    qtest_quit(qts);
}

/*
 * A doubleword store carries two words and costs one credit, so it
 * earns one: 1 and 9 in one store, then 0, bring the counter back to
 * the total and no further.
 */
static void test_doubleword_store_returns_one_credit(void)
{
    QTestState *qts = start();

    store(qts, COMMAND_IO, (UINT64_C(1) << 32) | 9, 8);
    g_assert_cmpuint(credit(qts), ==, TOTAL - 1);
    store_word(qts, 0);
    g_assert_cmpuint(credit(qts), ==, TOTAL);
    assert_probe_answered(qts);
    qtest_quit(qts);
}

/*
 * A display list's words come from memory, not from stores, and earn
 * nothing: the three stores of a call on a 12-word list, which ends in
 * the sync probe, bring the counter back to the total, not 12 above it.
 * The pool and the list are set up by qtest accesses, which no processor
 * charges.
 */
static void test_list_words_return_no_credit(void)
{
    static const uint32_t list[] = {
        1, 0x3, 5, 0x11 << 2, 0x22 << 2, 0x33 << 2,
        1, 0x2, 5,
        1, 9, 0,
    };
    QTestState *qts = start();

    for (unsigned i = 0; i < ARRAY_SIZE(list); i++) {
        uint8_t bytes[4];

        stl_be_p(bytes, list[i]);
        qtest_memwrite(qts, POOL_BASE + 4 * i, bytes, sizeof(bytes));
    }
    qtest_writel(qts, REG_COMMAND, DMA_POOL);
    qtest_writel(qts, REG_COMMAND, (uint32_t)(POOL_BASE >> 12));
    qtest_writel(qts, REG_COMMAND, 0x9000 | (uint32_t)(POOL_BASE & 0xfff));
    qtest_writel(qts, REG_COMMAND, 0x4000);
    g_assert_cmpuint(credit(qts), ==, TOTAL);

    store_word(qts, LIST_CALL_POOL);
    store_word(qts, 0);
    g_assert_cmpuint(credit(qts), ==, TOTAL - 2);
    store_word(qts, ARRAY_SIZE(list));
    g_assert_cmpuint(credit(qts), ==, TOTAL);
    assert_probe_answered(qts);
    qtest_quit(qts);
}

/* Buffer configuration keeps its counted stores until the tenth argument. */
static void test_gl_buffer_holds_credits(void)
{
    static const uint32_t config[] = { 0, 12, 0, 1, 0, 0, 24, 0, 0, 0 };
    QTestState *qts = start();

    qtest_writel(qts, REG_COMMAND, 0x1fd);
    qtest_writel(qts, REG_COMMAND, 0x10000340);
    qtest_writel(qts, REG_COMMAND, 0x9000);
    qtest_writel(qts, REG_COMMAND, 0x10000);
    store_word(qts, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
        store_word(qts, config[i]);
        g_assert_cmpuint(credit(qts), ==,
                         i + 1 == ARRAY_SIZE(config) ? TOTAL : TOTAL - i - 1);
    }
    qtest_quit(qts);
}

/* The DMA push page costs the same credits as the command page. */
static void test_dma_push_holds_credits(void)
{
    static const uint32_t packet[] = { 0x1eb, 0x40, 0x9000, 4, 4, 0, 0 };
    QTestState *qts = start();

    qtest_writeq(qts, HUB + PI_GFX_PAGE(0),
                  PI_GFX_PAGE_ENABLE | UINT64_C(0x0b800000));
    for (unsigned i = 0; i < ARRAY_SIZE(packet); i++) {
        store(qts, UINT64_C(0x920000000b80007c), packet[i], 4);
        g_assert_cmpuint(credit(qts), ==,
                         i + 1 == ARRAY_SIZE(packet) ? TOTAL : TOTAL - i - 1);
    }
    qtest_quit(qts);
}

/* Finishing a migrated packet returns the credits charged on its source. */
static void test_owed_credits_migrate(void)
{
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from = start(), *to;

    dir = g_dir_make_tmp("sn0-kona-credit-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    to = store_server(uri);
    store_word(from, 1);
    store_word(from, 9);
    g_assert_cmpuint(credit(from), ==, TOTAL - 1);
    qtest_qmp_assert_success(from, "{ 'execute': 'stop' }");
    qtest_qmp_eventwait(from, "STOP");
    sgi_migrate_stopped(from, uri);
    g_assert_cmpuint(credit(to), ==, TOTAL - 1);
    qtest_qmp_assert_success(to, "{ 'execute': 'cont' }");
    store_word(to, 0);
    g_assert_cmpuint(credit(to), ==, TOTAL);
    assert_probe_answered(to);
    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Every case runs a processor. */
    if (!qtest_has_accel("tcg")) {
        return g_test_run();
    }
    qtest_add_func("sn0/origin2000/kona-pi-credit/command-page-stores",
                   test_command_page_stores_return_credits);
    qtest_add_func("sn0/origin2000/kona-pi-credit/doubleword-store",
                   test_doubleword_store_returns_one_credit);
    qtest_add_func("sn0/origin2000/kona-pi-credit/list-words",
                   test_list_words_return_no_credit);
    qtest_add_func("sn0/origin2000/kona-pi-credit/gl-buffer-holds",
                   test_gl_buffer_holds_credits);
    qtest_add_func("sn0/origin2000/kona-pi-credit/dma-push-holds",
                   test_dma_push_holds_credits);
    qtest_add_func("sn0/origin2000/kona-pi-credit/owed-credits-migrate",
                   test_owed_credits_migrate);
    return g_test_run();
}
