/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Hub's graphics credits. A credit is paid by a processor's
 * uncached I/O store into its slot's graphics page, on the processor's own
 * path through its Hub, which a qtest access never takes: so every store
 * here is made by a running processor. Each processor serves a mailbox in
 * its own node's memory, a count of doubleword stores and an address to
 * make them at, and zeroes the count when done. Credits come back through
 * a stand-in head, sgi-pi-gfx-test-head, told by the test how many stores
 * it took; the node-widget map sends them to a slot.
 *
 * The page is the one holding the local Hub's I/O-interface scratch
 * registers, so a store there is harmless: scratch 0 takes a zero.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-test.h"
#include "sn0-user-image.h"

#define HUB_BY_NODE         (SPACE_IO + 0x01800000)
#define PI_GFX_PAGE(s)      (0x300 + 0x28 * (s))
#define PI_GFX_CREDIT(s)    (0x308 + 0x28 * (s))
#define PI_GFX_BIAS(s)      (0x310 + 0x28 * (s))
#define IO_GFX_INIT(n)      (0x400140 + 8 * (n))
#define PI_GFX_PAGE_ENABLE  (1ULL << 40)

/* The local Hub's I/O-interface scratch 0, through the uncached I/O space. */
#define STORE_IO            UINT64_C(0x9200000001400150)
/* The same low address through the uncached memory space: not I/O. */
#define STORE_HSPEC         UINT64_C(0x9000000001400150)
#define PAGE                UINT64_C(0x01400000)
#define OTHER_PAGE          UINT64_C(0x01420000)

#define HEAD_WIDGET         0xc
/* Map entry: valid, slot, node, widget (Linux sn0/hubio.h IIO_IGFX_INIT). */
#define MAP(node, slot, widget) \
    ((1ULL << 20) | ((uint64_t)(slot) << 16) | ((uint64_t)(node) << 4) | \
     (widget))

/* Node-relative mailboxes, one per local slice: count, then address. */
#define MAILBOX             0x50000u
#define MAILBOX_STRIDE      0x10u

#define ENTRY               0x800

#define ONE_NODE \
    "-M origin200 -m 64M -smp 2 -cpu R10000 -accel tcg,thread=multi " \
    "-no-reboot -no-shutdown -S "
#define TWO_NODES \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -no-reboot -no-shutdown -S "
/*
 * The head's own port, node 0's widget 0xe small window, placed over the
 * global map: a store the path counted there returns its credit at once.
 */
#define PORT_IO             UINT64_C(0x920000000e000000)
#define PORT_PAGE           UINT64_C(0x0e000000)
#define PI_GFX_CREDIT_MODE  0x020068
#define HEAD \
    "-device sgi-pi-gfx-test-head,id=head,pi=/machine/node0/hub,widget=12," \
    "port=0x2000e000000,auto-return=on "

static void build_store_server(SN0Image *p)
{
    unsigned int at, loop, store;

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

    /* s0 = this slice's mailbox in its own node's memory. */
    sn0_emit_cpu_number(p, SN0_R_T2);
    sn0_emit_sll(p, SN0_R_T2, SN0_R_T2, 4);
    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(MAILBOX));
    sn0_emit_addu(p, SN0_R_S0, SN0_R_S0, SN0_R_T2);

    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T1, 0);        /* count */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T2, 8);        /* address */
    store = p->pos;
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_T2, SN0_R_ZERO, 0);
    sn0_emit_i(p, 25, SN0_R_T1, SN0_R_T1, 0xffff);           /* daddiu -1 */
    at = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, store);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, 0);      /* done */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
}

static QTestState *start_with(const char *machine, const char *head)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;

    build_store_server(image);
    path = sn0_image_write(image, "sn0-pi-gfx");
    qts = qtest_initf("%s%s-bios %s", machine, head, path);
    unlink(path);
    return qts;
}

static QTestState *start(const char *machine)
{
    return start_with(machine, HEAD);
}

static uint64_t node_base(unsigned int node)
{
    return node * NODE_STRIDE;
}

/* Processor 'slice' of 'node' makes 'count' doubleword stores at 'addr'. */
static void stores(QTestState *qts, unsigned int node, unsigned int slice,
                   uint64_t addr, uint64_t count)
{
    uint64_t box = node_base(node) + MAILBOX + slice * MAILBOX_STRIDE;
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    qtest_writeq(qts, box + 8, addr);
    qtest_writeq(qts, box, count);
    while (qtest_readq(qts, box)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void head_returns(QTestState *qts, uint32_t credits)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set',"
        "  'arguments': { 'path': '/machine/peripheral/head',"
        "  'property': 'return', 'value': %u } }", credits);
}

static uint64_t hub(unsigned int node, uint64_t reg)
{
    return HUB_BY_NODE + node * NODE_STRIDE + reg;
}

static uint64_t credit(QTestState *qts, unsigned int node, unsigned int slot)
{
    return qtest_readq(qts, hub(node, PI_GFX_CREDIT(slot)));
}

static void prime(QTestState *qts, unsigned int node, unsigned int slot,
                  uint64_t page, uint64_t count)
{
    qtest_writeq(qts, hub(node, PI_GFX_PAGE(slot)), page);
    qtest_writeq(qts, hub(node, PI_GFX_CREDIT(slot)), count);
}

static void run(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
}

/*
 * The decrement and the return, told apart: the map sends the head's
 * credits to slot B, so slot A's store shows as A's loss and B's gain.
 * A synchronous head would hide both behind one unchanged counter.
 */
static void test_store_charges_its_slot_and_the_map_routes(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PAGE, 100);
    prime(qts, 0, 1, 0, 100);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 1, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, 0, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 100);

    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 101);
    qtest_quit(qts);
}

/* Slot B's own store charges slot B. */
static void test_slot_b_store_charges_slot_b(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PAGE, 100);
    prime(qts, 0, 1, PI_GFX_PAGE_ENABLE | PAGE, 100);
    run(qts);

    stores(qts, 0, 1, STORE_IO, 3);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 97);
    qtest_quit(qts);
}

/* Every counted store's credit comes back through the map to its slot. */
static void test_burst_round_trips(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PAGE, 101);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, 0, STORE_IO, 40);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 61);
    head_returns(qts, 40);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 101);
    qtest_quit(qts);
}

/*
 * Not counted: a disabled page, another page, and the same low address
 * through the uncached memory space instead of I/O.
 */
static void test_uncovered_stores_are_not_counted(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, PAGE, 100);
    run(qts);
    stores(qts, 0, 0, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);

    qtest_writeq(qts, hub(0, PI_GFX_PAGE(0)), PI_GFX_PAGE_ENABLE | OTHER_PAGE);
    stores(qts, 0, 0, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);

    qtest_writeq(qts, hub(0, PI_GFX_PAGE(0)), PI_GFX_PAGE_ENABLE | PAGE);
    stores(qts, 0, 0, STORE_HSPEC, 2);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);

    stores(qts, 0, 0, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 98);
    qtest_quit(qts);
}

/*
 * The map routes nothing without its valid bit or for another widget, and
 * with both entries naming the head the first wins (the model's choice).
 */
static void test_map_matching(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, 10);
    prime(qts, 0, 1, 0, 20);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)),
                 MAP(0, 0, HEAD_WIDGET) & ~(1ULL << 20));
    qtest_writeq(qts, hub(0, IO_GFX_INIT(1)), MAP(0, 1, HEAD_WIDGET + 1));
    head_returns(qts, 5);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 20);

    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 1, HEAD_WIDGET));
    qtest_writeq(qts, hub(0, IO_GFX_INIT(1)), MAP(0, 0, HEAD_WIDGET));
    head_returns(qts, 5);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 25);

    /* A write replaces the count; a return adds to it. */
    qtest_writeq(qts, hub(0, PI_GFX_CREDIT(1)), 50);
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 1), ==, 51);
    qtest_quit(qts);
}

/*
 * The counter wraps both ways (the model's choice), and falling below the
 * bias raises no graphics interrupt: the trigger is unknown and unmodelled.
 */
static void test_wrap_and_no_interrupt(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PAGE, 0);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, 0, STORE_IO, 1);
    g_assert_cmphex(credit(qts, 0, 0), ==, 0xfff);
    head_returns(qts, 2);
    g_assert_cmphex(credit(qts, 0, 0), ==, 1);

    qtest_writeq(qts, hub(0, PI_GFX_CREDIT(0)), 3);
    qtest_writeq(qts, hub(0, PI_GFX_BIAS(0)), 5);
    stores(qts, 0, 0, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 1);
    g_assert_cmphex(qtest_readq(qts, hub(0, PI_INT_PEND0)) & 6, ==, 0);
    qtest_quit(qts);
}

/*
 * Two nodes: a node 1 processor is charged on node 1's Hub, and the head's
 * master node (node 0) sends the credit back to node 1 by its number. Then
 * a node 0 processor pays and node 1 gains.
 */
static void test_cross_node(const void *d)
{
    QTestState *qts = start(TWO_NODES);

    sn0_program_nasid(qts, 1);
    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PAGE, 100);
    prime(qts, 1, 0, PI_GFX_PAGE_ENABLE | PAGE, 100);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(1, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 1, 0, STORE_IO, 4);
    g_assert_cmpuint(credit(qts, 1, 0), ==, 96);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);
    head_returns(qts, 4);
    g_assert_cmpuint(credit(qts, 1, 0), ==, 100);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);

    stores(qts, 0, 0, STORE_IO, 1);
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 1, 0), ==, 101);
    qtest_quit(qts);
}

static uint64_t head_taken(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{ 'execute': 'qom-get',"
        "  'arguments': { 'path': '/machine/peripheral/head',"
        "  'property': 'taken' } }");

    g_assert(qdict_haskey(response, "return"));
    return qdict_get_int(response, "return");
}

/*
 * A head returns credit only for a store the path counted: a synchronous
 * head keeps the count where it was, and a store the page did not cover
 * earns nothing, so the count can never rise above what the driver wrote.
 * IRIX's zero store to 0x2_0068 is taken on the way.
 */
static void test_head_returns_only_counted_stores(const void *d)
{
    QTestState *qts = start(ONE_NODE);

    qtest_writeq(qts, hub(0, PI_GFX_CREDIT_MODE), 0);
    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PORT_PAGE, 100);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, 0, PORT_IO, 10);
    g_assert_cmpuint(head_taken(qts), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);

    qtest_writeq(qts, hub(0, PI_GFX_PAGE(0)), 0);
    stores(qts, 0, 0, PORT_IO, 5);
    g_assert_cmpuint(head_taken(qts), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);
    qtest_quit(qts);
}

/*
 * A port left at a four-byte access width sees a doubleword store as two
 * callbacks. Only one of them is told the store was counted, so the head
 * returns one credit for the one it cost.
 */
static void test_split_store_returns_one_credit(const void *d)
{
    QTestState *qts = start_with(ONE_NODE,
        "-device sgi-pi-gfx-test-head,id=head,pi=/machine/node0/hub,"
        "widget=12,port=0x2000e000000,auto-return=on,port-width=4 ");

    prime(qts, 0, 0, PI_GFX_PAGE_ENABLE | PORT_PAGE, 100);
    qtest_writeq(qts, hub(0, IO_GFX_INIT(0)), MAP(0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, 0, PORT_IO, 1);
    g_assert_cmpuint(head_taken(qts), ==, 1);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);
    stores(qts, 0, 0, PORT_IO, 6);
    g_assert_cmpuint(head_taken(qts), ==, 7);
    g_assert_cmpuint(credit(qts, 0, 0), ==, 100);
    qtest_quit(qts);
}

/* The block and the map survive migration. */
static void test_migration(const void *d)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpdir = g_dir_make_tmp("sn0-pi-gfx-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);

    from = qtest_init("-M origin200 -m 64M -S");
    qtest_writeq(from, hub(0, PI_GFX_PAGE(1)), PI_GFX_PAGE_ENABLE | PAGE);
    qtest_writeq(from, hub(0, PI_GFX_CREDIT(1)), 0x65);
    qtest_writeq(from, hub(0, PI_GFX_BIAS(1)), 0x44);
    qtest_writeq(from, hub(0, IO_GFX_INIT(1)), MAP(0, 1, HEAD_WIDGET));
    to = qtest_initf("-M origin200 -m 64M -S -incoming %s", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(qtest_readq(to, hub(0, PI_GFX_PAGE(1))), ==,
                    PI_GFX_PAGE_ENABLE | PAGE);
    g_assert_cmphex(qtest_readq(to, hub(0, PI_GFX_CREDIT(1))), ==, 0x65);
    g_assert_cmphex(qtest_readq(to, hub(0, PI_GFX_BIAS(1))), ==, 0x44);
    g_assert_cmphex(qtest_readq(to, hub(0, IO_GFX_INIT(1))), ==,
                    MAP(0, 1, HEAD_WIDGET));

    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/pi-gfx/migration", NULL, test_migration);
    /* Every other case runs processors. */
    if (!qtest_has_accel("tcg")) {
        return g_test_run();
    }
    qtest_add_data_func("sn0/pi-gfx/store-charges-its-slot-map-routes", NULL,
                        test_store_charges_its_slot_and_the_map_routes);
    qtest_add_data_func("sn0/pi-gfx/slot-b-store-charges-slot-b", NULL,
                        test_slot_b_store_charges_slot_b);
    qtest_add_data_func("sn0/pi-gfx/burst-round-trips", NULL,
                        test_burst_round_trips);
    qtest_add_data_func("sn0/pi-gfx/uncovered-stores-not-counted", NULL,
                        test_uncovered_stores_are_not_counted);
    qtest_add_data_func("sn0/pi-gfx/map-matching", NULL, test_map_matching);
    qtest_add_data_func("sn0/pi-gfx/wrap-and-no-interrupt", NULL,
                        test_wrap_and_no_interrupt);
    qtest_add_data_func("sn0/pi-gfx/cross-node", NULL, test_cross_node);
    qtest_add_data_func("sn0/pi-gfx/head-returns-only-counted-stores", NULL,
                        test_head_returns_only_counted_stores);
    qtest_add_data_func("sn0/pi-gfx/split-store-returns-one-credit", NULL,
                        test_split_store_returns_one_credit);
    return g_test_run();
}
