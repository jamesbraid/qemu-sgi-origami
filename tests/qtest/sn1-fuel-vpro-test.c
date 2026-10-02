/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Fuel's VPro board in its graphics slot. A Fuel IRIX inventory lists
 * the board under xtalk/13 and OpenBSD finds Odyssey at XXBow widget 13,
 * so the processor must reach the board's widget registers there, and the
 * board's interrupt packets must reach the Bedrock through the XBridge.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define FUEL_VPRO_WIDGET 13
#define XTALK_WIDGET(w) (SPACE_IO + (uint64_t)(w) * UINT64_C(0x1000000))
#define VPRO (XTALK_WIDGET(FUEL_VPRO_WIDGET))
#define VPRO_PATH "/machine/module0/graphics-slot"

/* The widget identification register, in its slot's data half. */
#define ODSY_IDENTITY 0x0004
#define ODSY_STATUS 0x000c
#define ODSY_CONTROL 0x0024
#define ODSY_INT_DEST_UPPER 0x0034
#define ODSY_INT_DEST_LOWER 0x003c
#define ODSY_INTR_CLEAR 0x101c
#define ODSY_INTR_ENABLE_SET 0x1034
#define ODSY_INTR_FIFO_LOW_WATER 0x00000200

/* Revision 0xa, part 0xc013, manufacturer 0x023. */
#define ODSY_IDENTITY_DEFAULT 0xac013047
/* The widget number a board learns from its link, in control bits 3:0. */
#define ODSY_CONTROL_WIDGET_MASK 0xf

/*
 * The XBridge crossbar's own registers at widget 0: each link's status
 * register, in the data half of its eight-byte slot.
 */
#define XBRIDGE_CROSSBAR XTALK_WIDGET(0)
#define XBOW_LINK_STATUS(w) (0x100 + ((w) - 8) * 0x40 + 0x10 + 4)
#define XBOW_LINK_ALIVE (1u << 31)

/* The node's Processor Interface pending pair, reached from the processor. */
#define BEDROCK_NODE0 (SPACE_IO + UINT64_C(0x01800000))
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98

/*
 * The same PENDING_MODIFY register as a crosstalk packet addresses it: bit
 * 47 selects register space, the node's hub window 0x1800000 and the
 * offset follow. Widget 0xa is the Bedrock's port on the XBridge.
 */
#define BEDROCK_PACKET_PI_PENDING_MODIFY \
    ((UINT64_C(1) << 47) | UINT64_C(0x1800000) | PI_PENDING_MODIFY)
#define DEST_WIDGET 0xa
#define TEST_VECTOR 5
#define INT_DEST_UPPER \
    (((uint32_t)TEST_VECTOR << 24) | (DEST_WIDGET << 16) | \
     (uint32_t)(BEDROCK_PACKET_PI_PENDING_MODIFY >> 32))
#define INT_DEST_LOWER ((uint32_t)BEDROCK_PACKET_PI_PENDING_MODIFY)

static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-fuel-vpro-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *fuel_start(const char *topology)
{
    g_autofree char *quoted = g_shell_quote(prom_path);

    return qtest_initf("-M fuel,topology=%s,fuel-board-id-word=0x4abc,"
                       "fuel-ioc3-subsystem-id=0xc30a,"
                       "fuel-l1-type-code=0x61,fuel-bedrock-revision=0x2 "
                       "-cpu R14000 -smp 1 -m 512M -bios %s -S",
                       topology, quoted);
}

static bool qom_exists(QTestState *qts, const char *path)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'type'}}",
        path);

    return qdict_haskey(response, "return");
}

/* The board answers as Odyssey at widget 13, linked to that widget. */
static void test_widget_identity(void)
{
    QTestState *qts = fuel_start("fuel-vpro");
    g_autoptr(QDict) response = NULL;

    g_assert_true(qom_exists(qts, VPRO_PATH));
    response = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'type'}}", VPRO_PATH);
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, "sgi-odyssey");

    g_assert_cmphex(qtest_readl(qts, VPRO + ODSY_IDENTITY), ==,
                    ODSY_IDENTITY_DEFAULT);
    g_assert_cmphex(qtest_readl(qts, VPRO + ODSY_CONTROL) &
                    ODSY_CONTROL_WIDGET_MASK, ==, FUEL_VPRO_WIDGET);
    g_assert_cmphex(qtest_readl(qts, XBRIDGE_CROSSBAR +
                                XBOW_LINK_STATUS(FUEL_VPRO_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, XBOW_LINK_ALIVE);
    qtest_quit(qts);
}

/* The base Fuel fits no graphics board, and nothing answers at 13. */
static void test_base_fuel_has_no_board(void)
{
    QTestState *qts = fuel_start("fuel");

    g_assert_false(qom_exists(qts, VPRO_PATH));
    g_assert_cmphex(qtest_readl(qts, XBRIDGE_CROSSBAR +
                                XBOW_LINK_STATUS(FUEL_VPRO_WIDGET)) &
                    XBOW_LINK_ALIVE, ==, 0);
    qtest_quit(qts);
}

/*
 * A raised and a lowered board interrupt each travel as a crosstalk packet
 * through the XBridge to the Bedrock, setting and clearing the vector's
 * own pending bit there.
 */
static void test_interrupt_reaches_bedrock(void)
{
    QTestState *qts = fuel_start("fuel-vpro");
    uint64_t bit = UINT64_C(1) << TEST_VECTOR;

    qtest_writel(qts, VPRO + ODSY_INT_DEST_UPPER, INT_DEST_UPPER);
    qtest_writel(qts, VPRO + ODSY_INT_DEST_LOWER, INT_DEST_LOWER);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) & bit,
                    ==, 0);

    /* The FIFO is empty, so its low-water flag is already pending. */
    qtest_writel(qts, VPRO + ODSY_INTR_ENABLE_SET, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) & bit,
                    ==, bit);

    qtest_writel(qts, VPRO + ODSY_INTR_CLEAR, ODSY_INTR_FIFO_LOW_WATER);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + PI_PENDING_0) & bit,
                    ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("sn1/fuel-vpro/widget-identity", test_widget_identity);
    qtest_add_func("sn1/fuel-vpro/base-fuel-has-no-board",
                   test_base_fuel_has_no_board);
    qtest_add_func("sn1/fuel-vpro/interrupt-reaches-bedrock",
                   test_interrupt_reaches_bedrock);
    ret = g_test_run();
    unlink(prom_path);
    g_free(prom_path);
    return ret;
}
