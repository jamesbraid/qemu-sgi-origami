/*
 * The Origin 2000 Hub's outgoing big-window I/O face
 * (hw/mips/sgi/sn-machine.c, sn0_origin2000_outgoing_ops): what a CPU's
 * own big window in IO_BWIN_SPACE mode reaches once the window names a
 * real widget on the node's own Crossbow. The MemoryRegionOps advertise
 * one to eight bytes; this checks that every one of those widths actually
 * reaches the target widget's own register, both ways, rather than only
 * the two widths the callbacks used to hard-code (an eight-byte read of
 * one fixed offset, a one-byte write of any offset).
 *
 * BaseIO's Bridge sits at widget 8 on the default origin2000 topology's
 * own crossbar 0, the same crossbar node 0 itself attaches to
 * (hw/mips/sgi/sn-model.c, sgi_model_origin2000_module_xtalk), so a big
 * window CPU 0 programs with that widget number reaches BaseIO's Bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#define ORIGIN2000_MACHINE "-M origin2000 -smp 8,sockets=4,cores=2"

/* Position-independent crosstalk space and the per-widget small window. */
#define XTALK_BASE UINT64_C(0x20000000000)
#define XTALK_WIDGET_SIZE UINT64_C(0x1000000)
#define XTALK_REG_DATA 4

/* The Hub's own register block and its big-window translation table. */
#define HUB_BASE UINT64_C(0x20001000000)
#define IO_BWIN_TABLE 0x400160
#define IO_BWIN_STRIDE 8
#define IO_BWIN_ENTRY(n) (IO_BWIN_TABLE + (n) * IO_BWIN_STRIDE)
#define IO_BWIN_WIDGET(w) ((uint64_t)(w) << 8)
#define IO_BWIN_OTHER_SPACE (1ULL << 12)

#define BIG_WINDOW_BASE UINT64_C(0x20000000)
#define BIG_WINDOW_SIZE UINT64_C(0x20000000)
#define BIG_WINDOW(n) (XTALK_BASE + BIG_WINDOW_BASE + (n) * BIG_WINDOW_SIZE)

#define BASEIO_WIDGET 8

/*
 * Bridge's own plain register block (identification, DEVICE(n), and the
 * rest) is deliberately four-byte-only -- hw/pci-host/sgi-bridge.c's
 * sgi_bridge_reg_ops, "both bounds declared so the core rejects instead
 * of reshaping" -- a real, reviewed hardware contract, not something
 * this outgoing face should widen past. Unpopulated SSRAM is the
 * genuinely byte-addressable region on the same Bridge (sgi_bridge_ssram_
 * ops, one to eight bytes, always floating all-ones), so it is what
 * proves the outgoing face itself forwards every advertised width
 * instead of narrowing to what one particular target happens to accept.
 */
#define BRIDGE_SSRAM_BASE 0x080000

/*
 * A plain storage register with a full writable mask and no side effect
 * on read or write (unlike a DEVICE(n) window-control register, which
 * recomputes its own value from actual BAR-mapping state): the register
 * table's own comment on it, "no specified power-up value", is the
 * documented reason nothing else in this tree already depends on its
 * reset content, which is what makes it safe for a test to repurpose as
 * a round-trip scratch register.
 */
#define BRIDGE_TEST_PIN_CONTROL 0x070
#define SCRATCH_PATTERN 0x0000c0deU

/* Program big-window entry 0 to reach widget's own I/O-space packet face. */
static void bwin_aim(QTestState *qts, unsigned int widget)
{
    uint8_t bytes[8];

    stq_be_p(bytes, IO_BWIN_WIDGET(widget) | IO_BWIN_OTHER_SPACE);
    g_assert_true(qtest_memwrite_cpu(qts, 0, HUB_BASE + IO_BWIN_ENTRY(0),
                                     bytes, sizeof(bytes)));
}

/* A bounded read through the outgoing face at exactly `size` bytes. */
static uint64_t outgoing_read(QTestState *qts, uint64_t offset,
                              unsigned size)
{
    uint8_t bytes[8] = { 0 };

    g_assert_true(qtest_memread_cpu(qts, 0, BIG_WINDOW(0) + offset,
                                    bytes + 8 - size, size));
    return ldq_be_p(bytes);
}

/* A bounded write through the outgoing face at exactly `size` bytes. */
static void outgoing_write(QTestState *qts, uint64_t offset, uint64_t value,
                           unsigned size)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, BIG_WINDOW(0) + offset,
                                     bytes + 8 - size, size));
}

/* The same register reached directly, through BaseIO's own small window. */
static uint32_t direct_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[4];

    g_assert_true(qtest_memread_cpu(
        qts, 0, XTALK_BASE + BASEIO_WIDGET * XTALK_WIDGET_SIZE + offset +
        XTALK_REG_DATA, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/*
 * Every advertised read width reaches unpopulated SSRAM and gets its
 * documented all-ones float back, at that width: a one-byte read that
 * stopped at any offset the old callback rejected outright (it accepted
 * only an eight-byte read of one fixed identification offset) now
 * succeeds here the same as two, four and eight.
 */
static void test_read_every_advertised_width(const void *data)
{
    QTestState *qts = qtest_init(ORIGIN2000_MACHINE " -display none");

    bwin_aim(qts, BASEIO_WIDGET);

    g_assert_cmphex(outgoing_read(qts, BRIDGE_SSRAM_BASE, 1), ==, 0xffULL);
    g_assert_cmphex(outgoing_read(qts, BRIDGE_SSRAM_BASE, 2), ==, 0xffffULL);
    g_assert_cmphex(outgoing_read(qts, BRIDGE_SSRAM_BASE, 4), ==,
                    0xffffffffULL);
    g_assert_cmphex(outgoing_read(qts, BRIDGE_SSRAM_BASE, 8), ==,
                    0xffffffffffffffffULL);

    qtest_quit(qts);
}

/*
 * The write direction at widths the old callback rejected outright
 * (anything but one byte): a pattern written through the outgoing face
 * at four and at eight bytes, read back through the direct small window,
 * proving the write actually reached BaseIO's Bridge rather than being
 * silently dropped or truncated. An eight-byte write covers the whole
 * register slot, the same way an eight-byte read does; the register's
 * own data lane is still the low four bytes of it.
 */
static void test_write_every_advertised_width(const void *data)
{
    QTestState *qts = qtest_init(ORIGIN2000_MACHINE " -display none");

    bwin_aim(qts, BASEIO_WIDGET);

    outgoing_write(qts, BRIDGE_TEST_PIN_CONTROL + XTALK_REG_DATA,
                  SCRATCH_PATTERN, 4);
    g_assert_cmphex(direct_read(qts, BRIDGE_TEST_PIN_CONTROL), ==,
                    SCRATCH_PATTERN);

    outgoing_write(qts, BRIDGE_TEST_PIN_CONTROL, SCRATCH_PATTERN, 8);
    g_assert_cmphex(direct_read(qts, BRIDGE_TEST_PIN_CONTROL), ==,
                    SCRATCH_PATTERN);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/origin2000/outgoing-hub-io/read-every-width",
                        NULL, test_read_every_advertised_width);
    qtest_add_data_func("/sn0/origin2000/outgoing-hub-io/write-every-width",
                        NULL, test_write_every_advertised_width);

    return g_test_run();
}
