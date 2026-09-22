/*
 * The XBridge head's own error interrupt on the Origin 300, provoked the
 * way the Bridge datasheet records the Origin 200 PROM's bridge_sanity
 * provoking it: a 32-bit store to a register slot the map does not
 * define, with the error interrupt enabled and the destination registers
 * naming the Bedrock's interrupt-pending modify register. The IP35 PROM's
 * diagnostic expects the same status bit and pending bit; which slot it
 * stores to is not observed. What this file pins is the machine path from
 * the head's status bit to the processor interface's pending bit, for
 * both heads; the head's own error contract belongs to sn1-xbridge-test.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define XTALK_WIDGET(w) (SPACE_XTALK + (uint64_t)(w) * UINT64_C(0x1000000))
#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))

#define XBRIDGE_HOST_PORT 0xa
#define XBRIDGE_ERROR_ADDRESS_LOWER 0x18
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_RESET 0x110
#define XBRIDGE_HOST_ERROR 0x128
#define XBRIDGE_INVALID_ADDRESS (1U << 24)
#define XBRIDGE_RESET_INVALID_ADDRESS (1U << 3)
/* A slot the register map does not define; the Origin 200 PROM's choice. */
#define XBRIDGE_UNDEFINED_SLOT 0x78

/*
 * Where an interrupt packet lands: the node's hub-register window with the
 * register flag at crosstalk address bit 47, which the head takes from bit
 * 15 of the destination-upper register.
 */
#define PI_REMOTE_WINDOW 0x1800000
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_0 0x98
#define DESTINATION_UPPER ((XBRIDGE_HOST_PORT << 16) | (1U << 15))

static char *prom_path;

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-xbridge-host-error-prom-XXXXXX.bin", &path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static uint32_t xtalk_reg_read(QTestState *qts, unsigned widget,
                               uint64_t offset)
{
    return qtest_readl(qts, XTALK_WIDGET(widget) + offset + 4);
}

static void xtalk_reg_write(QTestState *qts, unsigned widget,
                            uint64_t offset, uint32_t value)
{
    qtest_writel(qts, XTALK_WIDGET(widget) + offset + 4, value);
}

static void test_error_reaches_pending(void)
{
    g_autofree char *prom = g_shell_quote(prom_path);
    QTestState *qts = qtest_initf(MACHINE_ARGS " -bios %s", prom);
    static const struct {
        unsigned widget;
        unsigned bit;
    } head[2] = { { 0xe, 5 }, { 0xf, 6 } };
    unsigned i;

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0), ==, 0);
    for (i = 0; i < 2; i++) {
        unsigned w = head[i].widget;

        xtalk_reg_write(qts, w, XBRIDGE_INT_UPPER, DESTINATION_UPPER);
        xtalk_reg_write(qts, w, XBRIDGE_INT_LOWER,
                        PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
        xtalk_reg_write(qts, w, XBRIDGE_HOST_ERROR, head[i].bit);
        xtalk_reg_write(qts, w, XBRIDGE_INT_ENABLE, XBRIDGE_INVALID_ADDRESS);
        g_assert_cmphex(xtalk_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, 0);
        /* The provoke: a store the head must post and report, not fault. */
        xtalk_reg_write(qts, w, XBRIDGE_UNDEFINED_SLOT, 0);
        g_assert_cmphex(xtalk_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, XBRIDGE_INVALID_ADDRESS);
        g_assert_cmphex(xtalk_reg_read(qts, w, XBRIDGE_ERROR_ADDRESS_LOWER),
                        ==, XBRIDGE_UNDEFINED_SLOT + 4);
        /* And the interrupt reaches the pending bit the host field names. */
        g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                        (1ULL << head[i].bit), ==, 1ULL << head[i].bit);
        xtalk_reg_write(qts, w, XBRIDGE_INT_RESET,
                        XBRIDGE_RESET_INVALID_ADDRESS);
        g_assert_cmphex(xtalk_reg_read(qts, w, XBRIDGE_INT_STATUS) &
                        XBRIDGE_INVALID_ADDRESS, ==, 0);
    }
    /* Each head named its own bit; neither named the other's. */
    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_PENDING_0) &
                    ((1ULL << 5) | (1ULL << 6)), ==, (1ULL << 5) | (1ULL << 6));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();
    qtest_add_func("origin300/xbridge/error-reaches-pending",
                   test_error_reaches_pending);
    ret = g_test_run();
    unlink(prom_path);
    g_free(prom_path);
    return ret;
}
