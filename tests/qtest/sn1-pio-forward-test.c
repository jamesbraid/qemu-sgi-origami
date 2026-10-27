/*
 * The IP35 counterpart of sn0-pio-forward-test. A Bedrock forwards each
 * processor I/O access into the processor's view, and its I/O interface
 * forwards each PIO into the widget's view. Both remember the region a page
 * of their target space resolved to (hw/misc/sgi-pio-forward.c). These
 * tests make processor accesses on a Fuel, change what an address reaches
 * between two of them, and expect the second to follow:
 *
 * - the node is renumbered, so the processor-interface view no longer
 *   places this node's I/O space at node 0;
 * - the I/O interface's big-window entry is re-aimed at another widget;
 * - a store and a load reach the VPro through the widget view a Fuel's
 *   XBridge port presents, whose root is an alias;
 * - a Bridge board at the same widget switches its window 0 between PCI
 *   memory and I/O space, which changes the I/O interface's widget view.
 *
 * The target is a VPro register that keeps what is written to it: the
 * interrupt destination's upper half.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"
#include "sgi-mips-emit.h"
#include "sgi-wait.h"

#define NODE(n)             ((uint64_t)(n) << 33)
#define BEDROCK_LOCAL       (SPACE_IO + UINT64_C(0x01000000))
#define LB_REVISION         UINT64_C(0x600000)
#define LB_NODE_SHIFT       32
#define BEDROCK_NODE(n)     (SPACE_IO + NODE(n) + UINT64_C(0x01800000))
#define II_ITTE0            UINT64_C(0x400160)
#define ITTE_WIDGET(w)      ((uint64_t)(w) << 8)

#define VPRO_WIDGET         13
#define EMPTY_WIDGET        8
#define ODSY_INT_DEST_UPPER 0x34
/* The VPro register in its small window, as an I/O-space offset. */
#define SMALL(n)            (NODE(n) + ((uint64_t)VPRO_WIDGET << 24) + \
                             ODSY_INT_DEST_UPPER)
/* The same register through big window 0. */
#define BIG                 (UINT64_C(0x20000000) + ODSY_INT_DEST_UPPER)
/* A processor's uncached reference to an I/O-space offset. */
#define UNCACHED            UINT64_C(0x9200000000000000)

#define MAILBOX             UINT64_C(0x180000)
#define OP_STORE            1
#define OP_LOAD             2
#define OP_STORE_BYTE       3
#define FAULT_MARK          0xfa17

#define R_ZERO 0
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_T3 11
#define R_S0 16
#define R_K0 26
#define R_K1 27
#define CP0_STATUS 12
#define CP0_EPC 14
#define STATUS_KX 0x80
#define ENTRY 0x400

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

/* Point the branch at @at to the current position. */
static void patch(Image *p, unsigned int at)
{
    stl_be_p(p->bytes + at, ldl_be_p(p->bytes + at) |
             (uint16_t)(((int)p->pos - (int)at - 4) / 4));
}

static void emit_branch_to(Image *p, uint32_t op, unsigned int rs,
                           unsigned int rt, unsigned int dest)
{
    int delta = ((int)dest - (int)p->pos - 4) / 4;

    emit(p, mips_branch(op, rs, rt, delta));
    emit(p, 0);
}

/*
 * A server for one request at a time in the mailbox: the operation at +0,
 * the processor address at +8, the word at +16. A store writes the word,
 * a byte store its low byte, and a load leaves the word it read at +16.
 * Clearing +0 says it is done. A bus error on the access marks +24 and
 * resumes after the access, so a request that reaches nothing still
 * completes.
 */
static char *write_image(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *bytes = g_malloc0(PROM_SIZE);
    Image p = { .bytes = bytes };
    unsigned int loop, store, store_byte, done, done_word;
    char *path = NULL;
    int fd;

    stl_be_p(bytes, 0x100000ff); /* beq zero, zero, ENTRY */
    for (unsigned int offset = 0x200; offset < 0x380; offset += 0x80) {
        stl_be_p(bytes + offset, 0x1000ffff); /* beq zero, zero, self */
    }
    p.pos = 0x380;
    emit(&p, 0x40200000 | (R_K0 << 16) | (CP0_EPC << 11)); /* dmfc0 k0 */
    emit(&p, mips_daddiu(R_K0, R_K0, 4));
    emit(&p, mips_dmtc0(R_K0, CP0_EPC));
    emit(&p, mips_ori(R_K1, R_ZERO, FAULT_MARK));
    emit(&p, mips_sd(R_K1, R_S0, 24));
    emit(&p, 0x42000018); /* eret */

    p.pos = ENTRY;
    emit(&p, mips_mfc0(R_T0, CP0_STATUS));
    emit(&p, mips_ori(R_T0, R_T0, STATUS_KX));
    emit(&p, mips_mtc0(R_T0, CP0_STATUS));
    for (unsigned int i = 0; i < 4; i++) {
        emit(&p, 0);
    }
    emit_li64(&p, R_S0, UINT64_C(0x9000000000000000) | MAILBOX);
    loop = p.pos;
    emit(&p, mips_ld(R_T1, R_S0, 0));
    emit_branch_to(&p, MIPS_OPC_BEQ, R_T1, R_ZERO, loop);
    emit(&p, mips_ld(R_T2, R_S0, 8));
    emit(&p, mips_ori(R_T3, R_ZERO, OP_STORE));
    store = p.pos;
    emit(&p, mips_branch(MIPS_OPC_BEQ, R_T1, R_T3, 0));
    emit(&p, 0);
    emit(&p, mips_ori(R_T3, R_ZERO, OP_STORE_BYTE));
    store_byte = p.pos;
    emit(&p, mips_branch(MIPS_OPC_BEQ, R_T1, R_T3, 0));
    emit(&p, 0);
    emit(&p, mips_lw(R_T0, R_T2, 0));
    emit(&p, mips_sd(R_T0, R_S0, 16));
    done = p.pos;
    emit(&p, mips_branch(MIPS_OPC_BEQ, R_ZERO, R_ZERO, 0));
    emit(&p, 0);
    patch(&p, store);
    emit(&p, mips_ld(R_T0, R_S0, 16));
    emit(&p, mips_sw(R_T0, R_T2, 0));
    done_word = p.pos;
    emit(&p, mips_branch(MIPS_OPC_BEQ, R_ZERO, R_ZERO, 0));
    emit(&p, 0);
    patch(&p, store_byte);
    emit(&p, mips_ld(R_T0, R_S0, 16));
    emit(&p, 0xa0000000 | (R_T2 << 21) | (R_T0 << 16)); /* sb t0, 0(t2) */
    patch(&p, done);
    patch(&p, done_word);
    emit(&p, mips_sd(R_ZERO, R_S0, 0));
    emit_branch_to(&p, MIPS_OPC_BEQ, R_ZERO, R_ZERO, loop);

    fd = g_file_open_tmp("sn1-pio-forward-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(write(fd, bytes, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *start_with(const char *extra)
{
    g_autofree char *prom = write_image();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel -cpu R14000 -smp 1 -m 512M "
                                  "-accel tcg -bios %s -S %s", quoted, extra);

    unlink(prom);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    return qts;
}

static QTestState *start(void)
{
    return start_with("");
}

/* Run one request; return whether the access raised a bus error. */
static bool request(QTestState *qts, uint64_t op, uint64_t io, uint64_t *word)
{
    SgiWait w;

    qtest_writeq(qts, SPACE_HUB + MAILBOX + 24, 0);
    qtest_writeq(qts, SPACE_HUB + MAILBOX + 16, *word);
    qtest_writeq(qts, SPACE_HUB + MAILBOX + 8, UNCACHED | io);
    qtest_writeq(qts, SPACE_HUB + MAILBOX, op);
    sgi_wait_start(&w, "processor access");
    while (qtest_readq(qts, SPACE_HUB + MAILBOX)) {
        sgi_wait_pause(&w, 1000);
    }
    *word = qtest_readq(qts, SPACE_HUB + MAILBOX + 16);
    return qtest_readq(qts, SPACE_HUB + MAILBOX + 24) == FAULT_MARK;
}

static bool guest_store(QTestState *qts, uint64_t io, uint32_t value)
{
    uint64_t word = value;

    return request(qts, OP_STORE, io, &word);
}

static bool guest_store_byte(QTestState *qts, uint64_t io, uint8_t value)
{
    uint64_t word = value;

    return request(qts, OP_STORE_BYTE, io, &word);
}

static uint32_t guest_load(QTestState *qts, uint64_t io)
{
    uint64_t word = 0;

    g_assert_false(request(qts, OP_LOAD, io, &word));
    return word;
}

/* The register, read by qtest at node @n's address. */
static uint32_t vpro_reg(QTestState *qts, unsigned int n)
{
    return qtest_readl(qts, SPACE_IO + SMALL(n));
}

/*
 * Store at node 0's address, renumber the node to 1, and store at node 0's
 * address again. That second store must not reach the board: its page
 * resolved to this node's I/O interface only while the node was number 0.
 * A store at node 1's address then reaches it.
 */
static void test_pi_follows_a_renumbered_node(void)
{
    QTestState *qts = start();
    uint64_t revision;

    g_assert_false(guest_store(qts, SMALL(0), 0x00112233u));
    g_assert_cmphex(vpro_reg(qts, 0), ==, 0x00112233u);

    revision = qtest_readq(qts, BEDROCK_LOCAL + LB_REVISION);
    revision = (revision & ~(UINT64_C(0xff) << LB_NODE_SHIFT)) |
               (UINT64_C(1) << LB_NODE_SHIFT);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_REVISION, revision);

    guest_store(qts, SMALL(0), 0x00445566u);
    g_assert_cmphex(vpro_reg(qts, 1), ==, 0x00112233u);

    g_assert_false(guest_store(qts, SMALL(1), 0x00778899u));
    g_assert_cmphex(vpro_reg(qts, 1), ==, 0x00778899u);
    qtest_quit(qts);
}

/*
 * The same processor address through big window 0, aimed at the VPro, then
 * at a widget with nothing attached, then back.
 */
static void test_ii_follows_a_reaimed_itte(void)
{
    QTestState *qts = start();

    qtest_writeq(qts, BEDROCK_NODE(0) + II_ITTE0, ITTE_WIDGET(VPRO_WIDGET));
    g_assert_false(guest_store(qts, BIG, 0x00112233u));
    g_assert_cmphex(vpro_reg(qts, 0), ==, 0x00112233u);

    qtest_writeq(qts, BEDROCK_NODE(0) + II_ITTE0, ITTE_WIDGET(EMPTY_WIDGET));
    guest_store(qts, BIG, 0x00445566u);
    g_assert_cmphex(vpro_reg(qts, 0), ==, 0x00112233u);

    qtest_writeq(qts, BEDROCK_NODE(0) + II_ITTE0, ITTE_WIDGET(VPRO_WIDGET));
    g_assert_false(guest_store(qts, BIG, 0x00778899u));
    g_assert_cmphex(vpro_reg(qts, 0), ==, 0x00778899u);
    qtest_quit(qts);
}

/*
 * A Fuel's VPro sits behind its XBridge, so the I/O interface's view of the
 * widget is rooted at the XBridge port's alias. A processor store and load
 * reach the register through it, and agree with qtest's view.
 */
static void test_alias_rooted_widget_view(void)
{
    QTestState *qts = start();

    g_assert_false(guest_store(qts, SMALL(0), 0x0badcafeu));
    g_assert_cmphex(vpro_reg(qts, 0), ==, 0x0badcafeu);
    qtest_writel(qts, SPACE_IO + SMALL(0), 0x00c0ffeeu);
    g_assert_cmphex(guest_load(qts, SMALL(0)), ==, 0x00c0ffeeu);
    qtest_quit(qts);
}

/*
 * A Bridge-based XIO board in the Fuel's widget 13 slot, with a PCI test
 * device whose memory BAR and I/O BAR sit at the same PCI address. The
 * Bridge maps its device windows straight into the widget view, and a
 * Device(n) write switches window n between its PCI memory and I/O
 * regions. Both of the device's BARs are register regions, so the I/O
 * interface's page cache can hold either.
 *
 * The device's registers take single bytes only; a wider access would be
 * split, and the cache serves only accesses its region takes whole, so the
 * processor stores bytes here. Writing a test number to register 0 selects
 * that test,
 * counted from 0 in memory space and from 3 in I/O space, and every read
 * then returns the selected test's header. Its name, from byte 16, starts
 * "mmio" or "portio".
 */
#define SHOEHORN \
    "-vga none -device sgi-pci-shoehorn,slot=widget-13,pci-bus-name=shoe " \
    "-device pci-testdev,bus=shoe,addr=0 "
#define BRIDGE_REG(off)     (((uint64_t)VPRO_WIDGET << 24) + (off) + 4)
#define BRIDGE_DEVICE0      BRIDGE_REG(0x200)
#define BRIDGE_CFG0(off)    (((uint64_t)VPRO_WIDGET << 24) + 0x20000 + (off))
#define WINDOW0(off)        (((uint64_t)VPRO_WIDGET << 24) + 0x200000 + (off))
#define TESTDEV_PCI         0x20000000u
#define TESTDEV_NAME        16
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_COMMAND_IO      0x0001

static void aim_window0(QTestState *qts, bool memory)
{
    qtest_writel(qts, SPACE_IO + BRIDGE_DEVICE0,
                 (memory ? DEV_MEMORY_SPACE : 0) | (TESTDEV_PCI >> 20));
}

static uint32_t selected_name(QTestState *qts)
{
    return qtest_readl(qts, SPACE_IO + WINDOW0(TESTDEV_NAME));
}

/*
 * Zero the window's first four bytes, so register 0 is written whichever
 * byte lane the Bridge puts it on.
 */
static void select_test_zero(QTestState *qts)
{
    for (unsigned int i = 0; i < 4; i++) {
        g_assert_false(guest_store_byte(qts, WINDOW0(i), 0));
    }
}

/*
 * Select test 0 by processor stores through window 0 in memory space,
 * switch the window to PCI I/O space and make the same stores at the same
 * processor addresses. It must reach the I/O BAR and select the I/O test, so
 * the name read back afterwards changes.
 */
static void test_ii_follows_a_bridge_window_switch(void)
{
    QTestState *qts = start_with(SHOEHORN);
    uint32_t memory_name;

    qtest_writel(qts, SPACE_IO + BRIDGE_CFG0(PCI_BAR0), TESTDEV_PCI);
    qtest_writel(qts, SPACE_IO + BRIDGE_CFG0(PCI_BAR1), TESTDEV_PCI);
    qtest_writel(qts, SPACE_IO + BRIDGE_CFG0(PCI_COMMAND),
                 PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
    aim_window0(qts, true);

    select_test_zero(qts);
    memory_name = selected_name(qts);

    aim_window0(qts, false);
    select_test_zero(qts);

    aim_window0(qts, true);
    g_test_message("selected name 0x%08x after the memory store, 0x%08x "
                   "after the I/O store", memory_name, selected_name(qts));
    g_assert_cmphex(selected_name(qts), !=, memory_name);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!qtest_has_accel("tcg")) {
        return g_test_run();
    }
    qtest_add_func("/sn1-pio-forward/pi-follows-a-renumbered-node",
                   test_pi_follows_a_renumbered_node);
    qtest_add_func("/sn1-pio-forward/ii-follows-a-reaimed-itte",
                   test_ii_follows_a_reaimed_itte);
    qtest_add_func("/sn1-pio-forward/alias-rooted-widget-view",
                   test_alias_rooted_widget_view);
    qtest_add_func("/sn1-pio-forward/ii-follows-a-bridge-window-switch",
                   test_ii_follows_a_bridge_window_switch);
    return g_test_run();
}
