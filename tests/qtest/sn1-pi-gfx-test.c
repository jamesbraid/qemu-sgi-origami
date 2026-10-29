/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Bedrock's graphics credits. A credit is paid by a
 * processor's uncached I/O store into its slot's graphics page, on that
 * processor's own path through its Bedrock, which a qtest access never
 * takes: so every store here is made by a running processor. Each processor
 * serves a mailbox, a count of doubleword stores and an address to make them
 * at, and zeroes the count when done; a count with MAILBOX_LOAD set asks for
 * one doubleword load from the address instead. Credits come back through
 * a stand-in head, sgi-pi-gfx-test-head, told how many stores it took; the
 * node-widget map sends them to a node, an interface and a slot.
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
#define PI_PENDING_MODIFY UINT64_C(0x90)
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
#define MAILBOX_LOAD 0x8000
#define SLOT_A 0
#define SLOT_B 1
#define SLOT_C 2

#define ONE_NODE "-M origin300 -cpu R14000 -smp 2 "
#define TWO_NODES "-M origin300,topology=origin300-two-chassis " \
                  "-cpu R14000 -smp 4 -m 1024 "
#define IP53_NODE "-M origin350,ip53-experimental=on," \
                  "nvram0=sgi-ds1742-data-io9," \
                  "nvram0-clock=sgi-ds1742-clock-io9 " \
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
    unsigned int loop, store, at, to_load, done;

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
    /* A count with MAILBOX_LOAD set asks for one load instead. */
    emit(p, mips_andi(R_T2, R_T1, MAILBOX_LOAD));
    to_load = emit_branch(p, MIPS_OPC_BNE, R_T2, R_ZERO);
    store = p->pos;
    emit(p, mips_sd(R_ZERO, R_T3, 0));
    emit(p, mips_daddiu(R_T1, R_T1, -1));
    at = emit_branch(p, MIPS_OPC_BNE, R_T1, R_ZERO);
    patch_branch(p, at, store);
    done = p->pos;
    emit(p, mips_sd(R_ZERO, R_S1, 0));
    at = emit_branch(p, MIPS_OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, at, loop);
    patch_branch(p, to_load, p->pos);
    emit(p, mips_ld(R_T0, R_T3, 0));
    at = emit_branch(p, MIPS_OPC_BEQ, R_ZERO, R_ZERO);
    patch_branch(p, at, done);
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

static QTestState *start_ip53_vpro(void)
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

    fd = g_file_open_tmp("sn1-pi-gfx-vpro-data-XXXXXX", &data, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, 0x8000), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    fd = g_file_open_tmp("sn1-pi-gfx-vpro-clock-XXXXXX", &clock, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, 16), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted_data = g_shell_quote(data);
    quoted_clock = g_shell_quote(clock);
    qts = qtest_initf(
        "-M tezro,topology=tezro-tower-multi,nvram0=sgi-ds1742-data-io9,"
        "nvram0-clock=sgi-ds1742-clock-io9 "
        "-cpu R16000,r16000-experimental=on,"
        "r16000-reset-mode=0x009bad85,r16000-scache-bytes=0x400000 "
        "-smp 4 -m 4G " RUN "-bios %s "
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw "
        "-device sgi-odyssey,id=vpro,slot=widget-12,product=v12",
        quoted_prom, quoted_data, quoted_clock);
    unlink(prom);
    unlink(data);
    unlink(clock);
    return qts;
}

/*
 * A routed Origin 3400 whose node 0 carries the stand-in head's port over
 * its processor-facing I/O window, at widget 0xd. Node 0's bank 0 holds
 * the mailboxes.
 */
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define MD_BANK0_512MB UINT64_C(0x41)
#define ROUTED_PORT_WIDGET 0xd

static QTestState *start_routed(void)
{
    g_autofree char *prom = write_image();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf(
        "-M origin3000,topology=origin3400 -cpu R12000 -smp 16 -m 16384 "
        RUN "-bios %s "
        "-device sgi-pi-gfx-test-head,id=head,"
        "pio=/machine/node0/bedrock/io/sgi-bedrock-io-pio[0],"
        "pio-offset=0x%x000000",
        quoted, ROUTED_PORT_WIDGET);

    unlink(prom);
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, MD_BANK0_512MB);
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
    g_assert_cmpint(ftruncate(fd, 0x8000), ==, 0);
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

static uint64_t mailbox(unsigned int node, unsigned int slot)
{
    return SPACE_HUB + NODE(node) + MAILBOX + slot * MAILBOX_STRIDE;
}

/* The processor in physical 'slot' of 'node' starts 'count' stores. */
static void stores_start(QTestState *qts, unsigned int node, unsigned int slot,
                         uint64_t addr, uint64_t count)
{
    qtest_writeq(qts, mailbox(node, slot) + 8, addr);
    qtest_writeq(qts, mailbox(node, slot), count);
}

/* ... and has made them all. */
static void stores_wait(QTestState *qts, unsigned int node, unsigned int slot)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    while (qtest_readq(qts, mailbox(node, slot))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

/* The processor in physical 'slot' of 'node' makes 'count' stores. */
static void stores(QTestState *qts, unsigned int node, unsigned int slot,
                   uint64_t addr, uint64_t count)
{
    stores_start(qts, node, slot, addr, count);
    stores_wait(qts, node, slot);
}

static void head_returns(QTestState *qts, uint32_t credits)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set',"
        "  'arguments': { 'path': '/machine/peripheral/head',"
        "  'property': 'return', 'value': %u } }", credits);
}

static void head_set(QTestState *qts, const char *property, int64_t credits)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set',"
        "  'arguments': { 'path': '/machine/peripheral/head',"
        "  'property': %s, 'value': %" PRId64 " } }", property, credits);
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
 * The counter wraps both ways (the model's choice); the credit mode is
 * kept and changes no count; and a store that takes the counter below the
 * bias raises the processor's graphics interrupt, pending bit 1 + slot
 * (the SN1 header's flow-control comment; whether the edge is below the
 * bias or at it is unknown, and the model takes below).
 */
static void test_wrap_mode_and_interrupt(void)
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

    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 0);
    qtest_writeq(qts, reg(0, 0, PI_GFX_CREDIT(0)), 6);
    qtest_writeq(qts, reg(0, 0, PI_GFX_BIAS(0)), 5);
    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 0);
    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 4);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 2);
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

static void test_ip53_odyssey_credit_return(void)
{
    QTestState *qts = start_ip53_vpro();
    const uint64_t vpro_fifo = UINT64_C(0x920000000c110000);
    const uint64_t vpro_page = vpro_fifo & UINT64_C(0xfffffe0000);

    prime(qts, 0, 0, 0, PAGE_ENABLE | vpro_page, 64);
    prime(qts, 0, 1, 0, 0, 64);
    map(qts, 0, MAP(0, 1, 0, 12));
    run(qts);
    stores(qts, 0, SLOT_A, vpro_fifo, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 63);
    g_assert_cmpuint(credit(qts, 0, 1, 0), ==, 65);
    qtest_quit(qts);
}

/*
 * A counted store that leaves the counter below the bias raises the
 * processor's graphics interrupt, pending bit 1 + slot, and stops the
 * slot: its next counted store waits until the processor has taken the
 * interrupt or the counter has risen above the bias (the SN1 header's
 * credit-counter comment: write-ready withheld, interrupt sent, then
 * allowed again; the hold's end is the model's choice). The processor
 * here runs with its interrupts off, as a kernel command-issue site at
 * raised priority does, so only returned credits, or software clearing
 * the bit, let it go on. A store while the counter stays below the bias
 * raises the bit again once software has cleared it.
 */
static void test_stall_below_bias(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, PAGE_ENABLE | PAGE, 6);
    qtest_writeq(qts, reg(0, 0, PI_GFX_BIAS(0)), 5);
    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));
    run(qts);

    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 5);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 0);
    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 4);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 2);

    /* Held: the board has returned nothing and the bit is still set. */
    stores_start(qts, 0, SLOT_A, STORE_IO, 2);
    g_usleep(200 * 1000);
    g_assert_cmpuint(qtest_readq(qts, mailbox(0, SLOT_A)), ==, 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 4);

    /*
     * Two credits lift the counter to 6, above the bias: both stores go,
     * and the second leaves it below again.
     */
    head_returns(qts, 2);
    stores_wait(qts, 0, SLOT_A);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 4);

    /* Software clears the bit: taken, so the next store goes at once. */
    qtest_writeq(qts, reg(0, 0, PI_PENDING_MODIFY), 1);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 0);
    stores(qts, 0, SLOT_A, STORE_IO, 1);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 3);
    g_assert_cmphex(qtest_readq(qts, reg(0, 0, PI_PENDING_0)) & 6, ==, 2);

    /* A store held when the machine stops lets the stop finish. */
    stores_start(qts, 0, SLOT_A, STORE_IO, 1);
    g_usleep(50 * 1000);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 3);
    qtest_qmp_assert_success(qts, "{ 'execute': 'stop' }");
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 2);
    qtest_quit(qts);
}

/*
 * Credits a head owes for words it has not yet taken settle when software
 * rewrites the map entry naming the head: they are delivered under the
 * entry as it stood, before the write, and the head's later repayments of
 * them are dropped (the node's model choice; IRIX's detach zeroes the
 * entry and then saves the counter). A write of the value already there
 * settles nothing. A repayment of credits still owed is routed as a return.
 */
static void test_map_rewrite_settles_owed_credits(void)
{
    QTestState *qts = start(ONE_NODE);

    prime(qts, 0, 0, 0, 0, 10);
    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));

    head_set(qts, "owe", 5);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 10);
    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 10);
    map(qts, 0, 0);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 15);
    head_set(qts, "repay", 5);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 15);

    map(qts, 0, MAP(0, 0, 0, HEAD_WIDGET));
    head_set(qts, "owe", 3);
    head_set(qts, "repay", 2);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 17);
    /* A board reset cancels what it abandons. */
    head_set(qts, "owe", -1);
    map(qts, 0, 0);
    g_assert_cmpuint(credit(qts, 0, 0, 0), ==, 17);
    qtest_quit(qts);
}

/* The machine places the board under its own name; "vpro" is its id. */
static void vpro_set(QTestState *qts, const char *property, uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set',"
        "  'arguments': { 'path': 'vpro',"
        "  'property': %s, 'value': %" PRIu64 " } }", property, value);
}

/*
 * The VPro's command-FIFO page is its own (posted) region, and a processor
 * may sleep inside its handler: a load that must see queued command work
 * waits for the board's slice, which draws with the big lock let go.
 * Another processor's FIFO store must then still be taken. It dispatches
 * through the same entry points as the rest of the aperture, so it queues
 * behind the work and is repaid as it enters the FIFO, and the memory
 * core's per-device re-entrancy guard, which would refuse it while the
 * first processor is inside the device, is off for the page as for the
 * aperture.
 *
 * x-test-slice-sleep-ms makes the board's slice hold the board for a
 * while; x-test-slice-words queues every FIFO store. Processor A loads
 * from the FIFO page and waits. Processor B, whose graphics page covers
 * the FIFO, makes five counted stores, which must complete and be repaid
 * while A still waits.
 */
static void test_ip53_fifo_store_while_another_waits(void)
{
    QTestState *qts = start_ip53_vpro();
    const uint64_t vpro_fifo = UINT64_C(0x920000000c110000);
    const uint64_t vpro_page = vpro_fifo & UINT64_C(0xfffffe0000);
    int64_t deadline;

    prime(qts, 0, 0, 1, PAGE_ENABLE | vpro_page, 100);
    map(qts, 0, MAP(0, 0, 1, 12));
    run(qts);
    vpro_set(qts, "x-test-slice-words", 1);
    vpro_set(qts, "x-test-slice-sleep-ms", 3000);

    /* One queued word, from a third processor, starts a long slice. */
    stores(qts, 0, SLOT_C, vpro_fifo, 1);
    g_usleep(300 * 1000);
    stores_start(qts, 0, SLOT_A, vpro_fifo, MAILBOX_LOAD);
    g_usleep(300 * 1000);
    g_assert_cmpuint(qtest_readq(qts, mailbox(0, SLOT_A)), !=, 0);

    deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    stores_start(qts, 0, SLOT_B, vpro_fifo, 5);
    while (qtest_readq(qts, mailbox(0, SLOT_B))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmpuint(credit(qts, 0, 0, 1), ==, 100);
    g_assert_cmpuint(qtest_readq(qts, mailbox(0, SLOT_A)), !=, 0);

    vpro_set(qts, "x-test-slice-sleep-ms", 0);
    stores_wait(qts, 0, SLOT_A);
    qtest_quit(qts);
}

/*
 * On a routed machine every node-named access a processor makes goes
 * through that processor's own view of the fabric, and all of a node's
 * views are the node's. A processor waiting inside a board with the big
 * lock let go, as a load waits out a drawing slice, must not stop another
 * processor of the same node reaching its own Bedrock by number. Processor
 * A of node 0 loads from the head's port and sleeps there; processor B
 * loads its I/O interface's scratch 1 through the node-named form. Were the
 * views to share the memory core's per-device re-entrancy guard, B's load
 * would take a bus error and B would never answer.
 */
#define NODE0_SCRATCH (UINT64_C(0x9200000001800000) + UINT64_C(0x400158))
#define ROUTED_PORT (UINT64_C(0x9200000000000000) | \
                     ((uint64_t)ROUTED_PORT_WIDGET << 24))

static void test_routed_node_access_while_another_waits(void)
{
    QTestState *qts = start_routed();
    int64_t deadline;

    head_set(qts, "sleep-ms", 3000);
    run(qts);
    stores_start(qts, 0, SLOT_A, ROUTED_PORT, MAILBOX_LOAD);
    g_usleep(300 * 1000);
    g_assert_cmpuint(qtest_readq(qts, mailbox(0, SLOT_A)), !=, 0);

    deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    stores_start(qts, 0, SLOT_B, NODE0_SCRATCH, MAILBOX_LOAD);
    while (qtest_readq(qts, mailbox(0, SLOT_B))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readq(qts, mailbox(0, SLOT_A)), !=, 0);

    head_set(qts, "sleep-ms", 0);
    stores_wait(qts, 0, SLOT_A);
    qtest_quit(qts);
}

/*
 * A display update reads the planes that a slice's draw writes with the
 * big lock let go, so it waits the slice out: a screendump taken while
 * x-test-slice-sleep-ms holds the board returns only after the slice.
 */
static void test_ip53_scanout_waits_for_slice(void)
{
    QTestState *qts = start_ip53_vpro();
    const uint64_t vpro_fifo = UINT64_C(0x920000000c110000);
    g_autofree char *shot = NULL;
    int64_t start;
    int fd;

    fd = g_file_open_tmp("sn1-pi-gfx-shot-XXXXXX.ppm", &shot, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    run(qts);
    vpro_set(qts, "x-test-slice-words", 1);
    vpro_set(qts, "x-test-slice-sleep-ms", 1500);
    stores(qts, 0, SLOT_C, vpro_fifo, 1);
    g_usleep(200 * 1000);
    start = g_get_monotonic_time();
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'screendump',"
        "  'arguments': { 'filename': %s, 'device': 'vpro' } }", shot);
    g_assert_cmpint(g_get_monotonic_time() - start, >=,
                    1000 * G_TIME_SPAN_MILLISECOND);
    vpro_set(qts, "x-test-slice-sleep-ms", 0);
    unlink(shot);
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
    qtest_add_func("sn1/pi-gfx/wrap-mode-and-interrupt",
                   test_wrap_mode_and_interrupt);
    qtest_add_func("sn1/pi-gfx/stall-below-bias", test_stall_below_bias);
    qtest_add_func("sn1/pi-gfx/map-rewrite-settles-owed-credits",
                   test_map_rewrite_settles_owed_credits);
    qtest_add_func("sn1/pi-gfx/cross-node", test_cross_node);
    qtest_add_func("sn1/pi-gfx/head-returns-only-counted-stores",
                   test_head_returns_only_counted_stores);
    qtest_add_func("sn1/pi-gfx/ip53-host-credit-return",
                   test_ip53_host_credit_return);
    qtest_add_func("sn1/pi-gfx/ip53-odyssey-credit-return",
                   test_ip53_odyssey_credit_return);
    qtest_add_func("sn1/pi-gfx/ip53-fifo-store-while-another-waits",
                   test_ip53_fifo_store_while_another_waits);
    qtest_add_func("sn1/pi-gfx/ip53-scanout-waits-for-slice",
                   test_ip53_scanout_waits_for_slice);
    qtest_add_func("sn1/pi-gfx/routed-node-access-while-another-waits",
                   test_routed_node_access_while_another_waits);
    return g_test_run();
}
