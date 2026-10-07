/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Bedrock's graphics credits. A credit is paid by a
 * processor's uncached I/O store into its slot's graphics page, on that
 * processor's own path through its Bedrock, which a qtest access never
 * takes: so every store here is made by a running processor. Each processor
 * serves a mailbox, a count of doubleword stores and an address to make them
 * at, and zeroes the count when done. Credits come back through a stand-in
 * head, sgi-pi-gfx-test-head, told how many stores it took; the node-widget
 * map sends them to a node, an interface and a slot.
 *
 * The stores go to the local I/O interface's scratch 1, which nothing
 * watches. The mailboxes sit in low node memory, reached through Hub
 * special space's uncached alias as sn1-hub-lock-test.c's state does. With
 * two processors, the machine fits slots A (interface 0) and C (interface
 * 1, its slot A).
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"
#include "sgi-mips-emit.h"

#define NODE_SHIFT 33
#define NODE(n) ((uint64_t)(n) << NODE_SHIFT)
#define BEDROCK_NODE(n) (SPACE_IO + NODE(n) + UINT64_C(0x01800000))
#define BEDROCK_LOCAL (SPACE_IO + UINT64_C(0x01000000))
#define PI1 UINT64_C(0x200000)
#define PI_PENDING_0 UINT64_C(0x98)
#define PI_GFX_PAGE(s) (UINT64_C(0x300) + 0x28 * (s))
#define PI_GFX_CREDIT(s) (UINT64_C(0x308) + 0x28 * (s))
#define PI_GFX_BIAS(s) (UINT64_C(0x310) + 0x28 * (s))
#define PI_GFX_CREDIT_MODE UINT64_C(0x20068)
#define II_GFX_MAP(n) (UINT64_C(0x400140) + 8 * (n))
#define PAGE_ENABLE (UINT64_C(1) << 40)

/* I/O-interface scratch 1 through the uncached I/O space, and its page. */
#define STORE_IO UINT64_C(0x9200000001400158)
/* The same low address through Hub special space: node memory, not I/O. */
#define STORE_HSPEC UINT64_C(0x9000000001400158)
#define PAGE UINT64_C(0x01400000)
#define OTHER_PAGE UINT64_C(0x01420000)

#define HEAD_WIDGET 0xb
/* Map entry: slot, node, PI, widget (Linux sn1/hubio_next.h IIO_IGFX_INIT). */
#define MAP(node, pi, slot, widget) \
    (((uint64_t)(slot) << 16) | ((uint64_t)(node) << 5) | \
     ((uint64_t)(pi) << 4) | (widget))

/* One mailbox per physical slot: count, then address. */
#define MAILBOX UINT64_C(0x180000)
#define MAILBOX_STRIDE 0x10
#define SLOT_A 0
#define SLOT_C 2

#define ONE_NODE "-M origin300 -cpu R14000 -smp 2 "
#define TWO_NODES "-M origin300,topology=origin300-two-chassis " \
                  "-cpu R14000 -smp 4 -m 1024 "
#define IP53_NODE "-M origin350,ip53-experimental=on " \
                  "-cpu R16000,r16000-experimental=on," \
                  "r16000-reset-mode=0x009cad85," \
                  "r16000-scache-bytes=0x800000 -smp 4 -m 2G "
#define RUN "-accel tcg,thread=multi -no-shutdown -S "
/*
 * The head's own port, over the global map in node 0's I/O space: a store
 * the path counted there returns its credit at once.
 */
#define PORT_IO UINT64_C(0x920000000e000000)
#define PORT_PAGE UINT64_C(0x0e000000)
#define HEAD \
    "-device sgi-pi-gfx-test-head,id=head,pi=/machine/node0/bedrock," \
    "widget=11,port=0x2000e000000,auto-return=%s "

#define R_ZERO 0
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_S0 16
#define R_S1 17
#define CP0_STATUS 12
#define STATUS_KX 0x80

typedef struct Image {
    uint8_t *bytes;
    unsigned int pos;
} Image;

static void emit(Image *p, uint32_t word)
{
    g_assert_cmpuint(p->pos + 4, <=, PROM_SIZE);
    stl_be_p(p->bytes + p->pos, word);
    p->pos += 4;
}

static void emit_li64(Image *p, unsigned int rt, uint64_t value)
{
    emit(p, mips_ori(rt, R_ZERO, value >> 48));
    for (unsigned int shift = 32; ; shift -= 16) {
        emit(p, mips_dsll(rt, rt, 16));
        emit(p, mips_ori(rt, rt, value >> shift));
        if (shift == 0) {
            break;
        }
    }
}

static unsigned int emit_branch(Image *p, uint32_t op, unsigned int rs,
                                unsigned int rt)
{
    unsigned int at = p->pos;

    emit(p, mips_branch(op, rs, rt, 0));
    emit(p, 0);
    return at;
}

static void patch_branch(Image *p, unsigned int at, unsigned int dest)
{
    int delta = ((int)dest - (int)at - 4) / 4;

    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) | (uint16_t)delta);
}

static void build_store_server(Image *p)
{
    unsigned int loop, store, at;

    stl_be_p(p->bytes + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(p->bytes + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p->pos = 0x400;
    emit(p, mips_mfc0(R_T0, CP0_STATUS));
    emit(p, mips_ori(R_T0, R_T0, STATUS_KX));
    emit(p, mips_mtc0(R_T0, CP0_STATUS));
    for (unsigned int i = 0; i < 4; i++) {
        emit(p, 0);
    }

    /* s1 = this physical slot's mailbox: the CPU number's two bits. */
    emit_li64(p, R_S0, UINT64_C(0x9200000001000000));
    emit(p, mips_ld(R_T2, R_S0, 0x20));
    emit(p, mips_andi(R_T2, R_T2, 3));
    emit(p, mips_dsll(R_T2, R_T2, 4));
    emit_li64(p, R_S1, UINT64_C(0x9000000000000000) | MAILBOX);
    emit(p, mips_daddu(R_S1, R_S1, R_T2));

    loop = p->pos;
    emit(p, mips_ld(R_T1, R_S1, 0));
    at = emit_branch(p, MIPS_OPC_BEQ, R_T1, R_ZERO);
    patch_branch(p, at, loop);
    emit(p, mips_ld(R_T3, R_S1, 8));
    store = p->pos;
    emit(p, mips_sd(R_ZERO, R_T3, 0));
    emit(p, mips_daddiu(R_T1, R_T1, -1));
    at = emit_branch(p, MIPS_OPC_BNE, R_T1, R_ZERO);
    patch_branch(p, at, store);
    emit(p, mips_sd(R_ZERO, R_S1, 0));
    at = emit_branch(p, MIPS_OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, at, loop);
}

static char *write_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    Image image = { .bytes = bytes };
    char *path = NULL;
    int fd;

    build_store_server(&image);
    fd = g_file_open_tmp("sn1-pi-gfx-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, bytes, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *start(const char *machine)
{
    g_autofree char *prom = write_image();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("%s" RUN HEAD "-bios %s",
                                 machine, "on", quoted);

    unlink(prom);
    return qts;
}

static QTestState *start_ip53(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *prom = write_image();
    g_autofree char *data = NULL;
    g_autofree char *clock = NULL;
    g_autofree char *quoted_prom = g_shell_quote(prom);
    g_autofree char *quoted_data = NULL;
    g_autofree char *quoted_clock = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn1-pi-gfx-data-XXXXXX", &data, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, 0x800), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-pi-gfx-clock-XXXXXX", &clock, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, 16), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted_data = g_shell_quote(data);
    quoted_clock = g_shell_quote(clock);
    qts = qtest_initf(
        IP53_NODE RUN HEAD "-bios %s "
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw",
        "off", quoted_prom, quoted_data, quoted_clock);
    unlink(prom);
    unlink(data);
    unlink(clock);
    return qts;
}

static void run(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
}

/* The processor in physical 'slot' of 'node' makes 'count' stores. */
static void stores(QTestState *qts, unsigned int node, unsigned int slot,
                   uint64_t addr, uint64_t count)
{
    uint64_t box = SPACE_HUB + NODE(node) + MAILBOX + slot * MAILBOX_STRIDE;
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

static uint64_t reg(unsigned int node, unsigned int pi, uint64_t offset)
{
    return BEDROCK_NODE(node) + pi * PI1 + offset;
}

static uint64_t credit(QTestState *qts, unsigned int node, unsigned int pi,
                       unsigned int slot)
{
    return qtest_readq(qts, reg(node, pi, PI_GFX_CREDIT(slot)));
}

static void prime(QTestState *qts, unsigned int node, unsigned int pi,
                  unsigned int slot, uint64_t page, uint64_t count)
{
    qtest_writeq(qts, reg(node, pi, PI_GFX_PAGE(slot)), page);
    qtest_writeq(qts, reg(node, pi, PI_GFX_CREDIT(slot)), count);
}

static void map(QTestState *qts, unsigned int entry, uint64_t value)
{
    qtest_writeq(qts, reg(0, 0, II_GFX_MAP(entry)), value);
}

/*
 * Slot A's store charges PI0 slot A, and the map's PI bit alone moves the
 * return to PI1: the decrement and the return, told apart.
 */
static void test_pi_bit_routes_between_interfaces(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, PAGE_ENABLE | PAGE, 100);
    prime(qts, 0, 1, 0, 0, 100);
    map(qts, 0, MAP(0, 1, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 100);
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 101);

    /* The slot bit moves it to PI0's slot B. */
    qtest_writeq(qts, reg(0, 0, PI_GFX_CREDIT(1)), 7);
    map(qts, 0, MAP(0, 0, 1, HEAD_WIDGET));
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 1), ==, 8);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 99);
    qtest_quit(qts);
}

/* Slot C, on interface 1, is charged on interface 1 and repaid there. */
static void test_interface_1_round_trip(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 1, 0, PAGE_ENABLE | PAGE, 256);
    map(qts, 0, MAP(0, 1, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, SLOT_C, STORE_IO, 30);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 226);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 0);
    head_returns(qts, 30);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 256);
    qtest_quit(qts);
}

static void test_uncovered_stores_are_not_counted(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, PAGE, 100);
    run(qts);
    stores(qts, 0, SLOT_A, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 100);

    qtest_writeq(qts, reg(0, 0, PI_GFX_PAGE(0)), PAGE_ENABLE | OTHER_PAGE);
    stores(qts, 0, SLOT_A, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 100);

    qtest_writeq(qts, reg(0, 0, PI_GFX_PAGE(0)), PAGE_ENABLE | PAGE);
    stores(qts, 0, SLOT_A, STORE_HSPEC, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 100);

    stores(qts, 0, SLOT_A, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 98);
    qtest_quit(qts);
}

/*
 * No valid bit: a zeroed entry names widget 0. Another widget routes
 * nothing, and with both entries naming the head the first wins (the
 * model's choice). A write replaces the count and a return adds to it.
 */
static void test_map_matching(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, 0, 10);
    prime(qts, 0, 0, 1, 0, 20);
    map(qts, 0, 0);
    map(qts, 1, MAP(0, 0, 1, HEAD_WIDGET + 1));
    head_returns(qts, 5);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 0, 1), ==, 20);

    map(qts, 0, MAP(0, 0, 1, HEAD_WIDGET));
    map(qts, 1, MAP(0, 0, 0, HEAD_WIDGET));
    head_returns(qts, 5);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 0, 1), ==, 25);

    qtest_writeq(qts, reg(0, 0, PI_GFX_CREDIT(1)), 50);
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 1), ==, 51);
    qtest_quit(qts);
}

/*
 * The counter wraps both ways (the model's choice); falling below the bias
 * raises no graphics interrupt; the credit mode is kept and changes no
 * count.
 */
static void test_wrap_mode_and_no_interrupt(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, PAGE_ENABLE | PAGE, 0);
    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));
    qtest_writeq(qts, reg(0, 0, PI_GFX_CREDIT_MODE), 1);
    qtest_writeq(qts, reg(0, 1, PI_GFX_CREDIT_MODE), ~UINT64_C(0));
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_GFX_CREDIT_MODE)), ==, 1);
    g_assert_cmphex(qtest_readq(qts, reg(0, 1, PI_GFX_CREDIT_MODE)), ==, 1);
    run(qts);

    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmphex(credit(qts, 0, 0, 0), ==, 0xfff);
    head_returns(qts, 2);
    g_assert_cmphex(credit(qts, 0, 0, 0), ==, 1);

    qtest_writeq(qts, reg(0, 0, PI_GFX_CREDIT(0)), 3);
    qtest_writeq(qts, reg(0, 0, PI_GFX_BIAS(0)), 5);
    stores(qts, 0, SLOT_A, STORE_IO, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 1);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 0);
    qtest_quit(qts);
}

/*
 * Node 1 is reached, before it has a number, the way the PROM reaches a
 * peer: node 0's vector engine on route 0 (sn1-cross-node-test.c). Its
 * revision word carries the node number in bits 39:32.
 */
#define LB_REVISION UINT64_C(0x600000)
#define LB_VECTOR_PARAMS UINT64_C(0x60c000)
#define LB_VECTOR_ROUTE UINT64_C(0x60c008)
#define LB_VECTOR_DATA UINT64_C(0x60c010)
#define LB_VECTOR_STATUS UINT64_C(0x60c020)
#define LB_VECTOR_READ UINT64_C(0x60c030)
#define LB_VECTOR_STATUS_CLEAR UINT64_C(0x60c038)
#define VECTOR_WRITE UINT64_C(1)
#define VECTOR_VALID (UINT64_C(1) << 63)
#define VECTOR_REQUEST(n) ((uint64_t)(n) << 40)
#define REVISION_NODE_MASK (UINT64_C(0xff) << 32)

static uint64_t peer_access(QTestState *qts, uint64_t hub_offset,
                            bool write, uint64_t data)
{
    uint64_t status;

    (void)qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS_CLEAR);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_ROUTE, 0);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_DATA, data);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_VECTOR_PARAMS,
                 (hub_offset & UINT64_C(0xfffff8)) | VECTOR_REQUEST(0x1c8) |
                 (write ? VECTOR_WRITE : 0));
    status = qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_STATUS);
    g_assert_cmphex(status & VECTOR_VALID, ==, VECTOR_VALID);
    return qtest_readq(qts, BEDROCK_LOCAL + LB_VECTOR_READ);
}

static void set_peer_node_number(QTestState *qts, unsigned int number)
{
    uint64_t revision = peer_access(qts, LB_REVISION, false, 0);

    revision = (revision & ~REVISION_NODE_MASK) | ((uint64_t)number << 32);
    peer_access(qts, LB_REVISION, true, revision);
}

/*
 * Two chassis: a node 1 processor is charged on node 1's Bedrock, and the
 * head's master node (node 0) sends the credit to node 1 by its number, at
 * the far end of its cable. Then a node 0 processor pays and node 1 gains.
 */
static void test_cross_node(void)
{
    QTestState *qts = start(TWO_NODES);

    set_peer_node_number(qts, 1);

    prime(qts, 0, 0, 0, PAGE_ENABLE | PAGE, 100);
    prime(qts, 1, 0, 0, PAGE_ENABLE | PAGE, 100);
    map(qts, 0, MAP(1, 0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 1, SLOT_A, STORE_IO, 4);
    g_assert_cmpuint(credit(qts, 1, 0, 0), ==, 96);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 100);
    head_returns(qts, 4);
    g_assert_cmpuint(credit(qts, 1, 0, 0), ==, 100);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 100);

    stores(qts, 0, SLOT_A, STORE_IO, 1);
    head_returns(qts, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 99);
    g_assert_cmpuint(credit(qts, 1, 0, 0), ==, 101);
    qtest_quit(qts);
}

static uint64_t head_taken(QTestState *qts);

static void test_ip53_host_credit_return(void)
{
    QTestState *qts = start_ip53();

    prime(qts, 0, 0, 0, PAGE_ENABLE | PORT_PAGE, 64);
    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));
    run(qts);
    stores(qts, 0, SLOT_A, PORT_IO, 7);
    g_assert_cmpuint(head_taken(qts), ==, 7);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 57);
    head_returns(qts, 7);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 64);
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
 * A head returns credit only for a store the path counted, so a store the
 * page did not cover can never lift the count above what the driver wrote.
 */
static void test_head_returns_only_counted_stores(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 1, 0, PAGE_ENABLE | PORT_PAGE, 100);
    map(qts, 0, MAP(0, 1, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, SLOT_C, PORT_IO, 10);
    g_assert_cmpuint(head_taken(qts), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 100);

    qtest_writeq(qts, reg(0, 1, PI_GFX_PAGE(0)), 0);
    stores(qts, 0, SLOT_C, PORT_IO, 5);
    g_assert_cmpuint(head_taken(qts), ==, 10);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 100);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Every other case runs processors. */
    if (!qtest_has_accel("tcg")) {
        return g_test_run();
    }
    qtest_add_func("sn1/pi-gfx/pi-bit-routes-between-interfaces",
                   test_pi_bit_routes_between_interfaces);
    qtest_add_func("sn1/pi-gfx/interface-1-round-trip",
                   test_interface_1_round_trip);
    qtest_add_func("sn1/pi-gfx/uncovered-stores-not-counted",
                   test_uncovered_stores_are_not_counted);
    qtest_add_func("sn1/pi-gfx/map-matching", test_map_matching);
    qtest_add_func("sn1/pi-gfx/wrap-mode-and-no-interrupt",
                   test_wrap_mode_and_no_interrupt);
    qtest_add_func("sn1/pi-gfx/cross-node", test_cross_node);
    qtest_add_func("sn1/pi-gfx/head-returns-only-counted-stores",
                   test_head_returns_only_counted_stores);
    qtest_add_func("sn1/pi-gfx/ip53-host-credit-return",
                   test_ip53_host_credit_return);
    return g_test_run();
}
