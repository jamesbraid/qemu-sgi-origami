/*
 * Two Bridge behaviors the PROM's sanity diagnostic depends on: the
 * external-SSRAM window completes reads (an unguarded probe loop sizes
 * it), and an undefined register-window access posts and latches the
 * invalid-address error instead of bus-erroring. "Posts, not faults" is
 * only visible to an executing processor, so those checks run a -kernel
 * fragment and watch for a RAM marker.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"

#include <elf.h>

#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/* Registers are 32 bits in the low half of an eight-byte slot; the data is at
 * the slot offset plus four. */
#define REG_DATA        4
#define REG(offset)     (BRIDGE_BASE + (offset) + REG_DATA)

#define BRIDGE_INT_STATUS   0x100
#define BRIDGE_INT_ENABLE   0x108
#define BRIDGE_INT_RESET    0x110
#define BRIDGE_HOST_ERR     0x128        /* host bit for the Bridge's own errors */
#define BRIDGE_INT_DEST_UP  0x030        /* interrupt destination, upper */
#define BRIDGE_INT_DEST_LO  0x038        /* interrupt destination, lower */
#define INT_INVALID_ADDR    (1u << 24)   /* "address invalid for this widget" */
#define INT_RESET_ALL       0x7fu        /* clears every error group */
#define INT_RESET_PCI_GRP   (1u << 0)    /* clears only the PCI group */
#define INT_RESET_REQ_DSP_GRP (1u << 3)  /* clears the invalid-address bit's group */

/* The local Hub's interrupt registers, where the error interrupt is delivered. */
#define HUB_LOCAL           (SPACE_XTALK + 0x01000000)
#define PI_INT_MODIFY       0x90
#define PI_INT_PEND0        0x98
#define ERR_HOST_BIT        5u

/*
 * The two-chassis machine. Real firmware is what hands out node numbers, so
 * the synthesized one is off here: both Hubs start on the reset number.
 */
#define MACHINE_TWO_NODE \
    "-M origin200,synthesized-firmware=off -m 128M " \
    "-object memory-backend-ram,id=ram0,size=64M " \
    "-object memory-backend-ram,id=ram1,size=64M " \
    "-numa node,nodeid=0,cpus=0-1,memdev=ram0 " \
    "-numa node,nodeid=1,cpus=2-3,memdev=ram1 " \
    "-smp 4,sockets=2,cores=2,threads=1"

/*
 * Two ways to name a Hub, and the pair is what makes the routing visible.
 * The first is the alias every processor decodes for the Hub beside it,
 * fixed to the board position; it always names the first chassis here,
 * because qtest drives the first processor's view. The second is a
 * node-qualified address, which the Hubs route by the numbers firmware
 * programmed. Give the chassis numbers that disagree with their positions
 * and the two handles name different boards.
 */
#define HUB_BY_POSITION     HUB_LOCAL
#define NODE_STRIDE         (1ULL << 32)
#define HUB_BY_NUMBER(n)    (SPACE_XTALK + 0x01800000 + (n) * NODE_STRIDE)
#define BRIDGE_BY_NUMBER(n) (BRIDGE_BASE + (n) * NODE_STRIDE)
#define REG_BY_NUMBER(n, offset) \
    (BRIDGE_BY_NUMBER(n) + (offset) + REG_DATA)

/* The network interface's status register carries the programmed number. */
#define HUB_NI_STATUS       0x600000
#define HUB_NI_REGION_SIZE  (1ULL << 17)
#define HUB_NI_NODE_MASK    (0x1ffULL << 8)
#define HUB_NI_NODE_SHIFT   8

/* An undefined slot inside the register window: the diagnostic's provoke. */
#define UNDEFINED_REG       0x7c
/* Somewhere inside the external-SSRAM window (0x08_0000, 512 KB). */
#define SSRAM_PROBE         0x0a3100

#define GUEST_MARKER_ADDR   0x30000
#define GUEST_MARKER        0x5a17c0deu

/*
 * The diagnostic's two accesses; the marker lands only if both returned,
 * so a bus-erroring model times this out.
 */
static char *write_invalid_access_guest(void)
{
    static const uint32_t code[] = {
        0x3c089200,             /* lui    t0, 0x9200                        */
        0x0008403c,             /* dsll32 t0, t0, 0                         */
        0x3c09080a,             /* lui    t1, 0x080a                        */
        0x35293100,             /* ori    t1, t1, 0x3100 -> widget 8 SSRAM  */
        0x01094025,             /* or     t0, t0, t1                        */
        0x8d0a0000,             /* lw     t2, 0(t0)   -- SSRAM read         */
        0x3c089200,             /* lui    t0, 0x9200                        */
        0x0008403c,             /* dsll32 t0, t0, 0                         */
        0x3c090800,             /* lui    t1, 0x0800                        */
        0x3529007c,             /* ori    t1, t1, 0x007c -> widget 8 + 0x7c */
        0x01094025,             /* or     t0, t0, t1                        */
        0x3c0adead,             /* lui    t2, 0xdead                        */
        0x354abeef,             /* ori    t2, t2, 0xbeef -> 0xdeadbeef      */
        0xad0a0000,             /* sw     t2, 0(t0)   -- undefined-slot store */
        0x3c089600,             /* lui    t0, 0x9600                        */
        0x0008403c,             /* dsll32 t0, t0, 0                         */
        0x3c090003,             /* lui    t1, 0x0003                        */
        0x01094025,             /* or     t0, t0, t1  -> phys 0x30000        */
        0x3c095a17,             /* lui    t1, 0x5a17                        */
        0x3529c0de,             /* ori    t1, t1, 0xc0de                    */
        0xad090000,             /* sw     t1, 0(t0)   -- both posted        */
        0x1000ffff,             /* b      .                                 */
        0x00000000,             /* nop                                      */
    };
    uint8_t image[0x1000 + sizeof(code)] = { 0 };
    const uint64_t entry = 0x8000000000020000ULL;
    GError *err = NULL;
    char *path = NULL;
    int fd;
    size_t i;

    fd = g_file_open_tmp("sn0-bridge-invalid-guest-XXXXXX", &path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    image[0] = ELFMAG0;
    image[1] = ELFMAG1;
    image[2] = ELFMAG2;
    image[3] = ELFMAG3;
    image[EI_CLASS] = ELFCLASS64;
    image[EI_DATA] = ELFDATA2MSB;
    image[EI_VERSION] = EV_CURRENT;
    stw_be_p(image + 16, ET_EXEC);
    stw_be_p(image + 18, EM_MIPS);
    stl_be_p(image + 20, EV_CURRENT);
    stq_be_p(image + 24, entry);
    stq_be_p(image + 32, 64);
    stw_be_p(image + 52, 64);
    stw_be_p(image + 54, 56);
    stw_be_p(image + 56, 1);

    stl_be_p(image + 64, PT_LOAD);
    stl_be_p(image + 68, PF_R | PF_X);
    stq_be_p(image + 72, 0x1000);
    stq_be_p(image + 80, entry);
    stq_be_p(image + 88, entry);
    stq_be_p(image + 96, sizeof(code));
    stq_be_p(image + 104, sizeof(code));
    stq_be_p(image + 112, 0x1000);

    for (i = 0; i < ARRAY_SIZE(code); i++) {
        stl_be_p(image + 0x1000 + i * sizeof(code[0]), code[i]);
    }

    g_assert_cmpint(write(fd, image, sizeof(image)), ==, (ssize_t)sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_ssram_and_undefined_slot_post(const void *data)
{
    g_autofree char *kernel = write_invalid_access_guest();
    QTestState *qts;
    uint32_t marker = 0;

    /*
     * A real instruction: a qtest read cannot tell refused from returned.
     * Reverting either fix makes this time out.
     */
    g_assert_true(g_setenv("QTEST_QEMU_ARGS", "-accel tcg", true));
    qts = qtest_initf("-M origin200 -smp 1 -kernel %s", kernel);
    g_unsetenv("QTEST_QEMU_ARGS");

    g_test_timer_start();
    while (marker != GUEST_MARKER && g_test_timer_elapsed() < 1) {
        marker = qtest_readl(qts, GUEST_MARKER_ADDR);
        g_usleep(1000);
    }

    g_assert_cmphex(marker, ==, GUEST_MARKER);
    qtest_quit(qts);
    g_unlink(kernel);
}

static void test_invalid_address_latches_status(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Plain register state: the store latches the bit, the group strobe
     * clears it -- the diagnostic's own sequence.
     */
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, 0);

    qtest_writel(qts, BRIDGE_BASE + UNDEFINED_REG, 0xdeadbeefu);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, INT_INVALID_ADDR);

    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_ALL);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, 0);

    qtest_quit(qts);
}

static void test_reset_strobe_clears_only_its_own_group(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Guests write all seven groups at once, so a strobe clearing more
     * than it names passes unnoticed until someone clears a single group.
     */
    qtest_writel(qts, BRIDGE_BASE + UNDEFINED_REG, 0xdeadbeefu);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, INT_INVALID_ADDR);

    /* A strobe for the PCI group says nothing about this error. */
    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_PCI_GRP);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, INT_INVALID_ADDR);

    /* Its own group's strobe clears it. */
    qtest_writel(qts, REG(BRIDGE_INT_RESET), INT_RESET_REQ_DSP_GRP);
    g_assert_cmphex(qtest_readl(qts, REG(BRIDGE_INT_STATUS)) & INT_INVALID_ADDR,
                    ==, 0);

    qtest_quit(qts);
}

static void test_invalid_address_raises_host_interrupt(const void *data)
{
    QTestState *qts = qtest_init("-M origin200");

    /*
     * Point the widget interrupt-destination pair at the local Hub's
     * interrupt-modify register, name the host bit in the host-error field, and
     * enable the invalid-address interrupt. A store to an undefined register
     * then drives the error interrupt across the fabric to the Hub, setting
     * that pending bit — the delivery the sanity diagnostic waits on. With the
     * interrupt disabled, or with no destination programmed, no packet goes.
     */
    qtest_writel(qts, REG(BRIDGE_INT_DEST_LO),
                 (uint32_t)(HUB_LOCAL - SPACE_XTALK) + PI_INT_MODIFY);
    qtest_writel(qts, REG(BRIDGE_INT_DEST_UP), 0);
    qtest_writel(qts, REG(BRIDGE_HOST_ERR), ERR_HOST_BIT);
    qtest_writel(qts, REG(BRIDGE_INT_ENABLE), INT_INVALID_ADDR);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==, 0);

    qtest_writel(qts, BRIDGE_BASE + UNDEFINED_REG, 0xdeadbeefu);

    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL + PI_INT_PEND0), ==,
                    1ULL << ERR_HOST_BIT);

    qtest_quit(qts);
}

static uint32_t number_at(QTestState *qts, uint64_t hub)
{
    uint64_t status = qtest_readq(qts, hub + HUB_NI_STATUS);

    return (status & HUB_NI_NODE_MASK) >> HUB_NI_NODE_SHIFT;
}

/*
 * Number the chassis against their positions: the first is given one, the
 * second keeps the reset number zero. This is the state the whole routing
 * question lives in -- while the numbers agree with the positions nothing
 * can tell a Hub picked by number from a Hub picked by position, and the
 * machine boots through a stretch where they disagree.
 *
 * Setting the first chassis's own number rather than the second's is what
 * makes the second addressable at all: qtest drives the first processor's
 * view, whose node-qualified accesses go through the claim layer, and the
 * unnumbered second chassis has no number to name it by.
 */
static void number_the_first_chassis_one(QTestState *qts)
{
    qtest_writeq(qts, HUB_BY_POSITION + HUB_NI_STATUS,
                 HUB_NI_REGION_SIZE | (1ULL << HUB_NI_NODE_SHIFT));

    /*
     * Now the handles part company, which is what the rest of the test
     * rests on: by position the first chassis, by number zero the second.
     */
    g_assert_cmpuint(number_at(qts, HUB_BY_POSITION), ==, 1);
    g_assert_cmpuint(number_at(qts, HUB_BY_NUMBER(0)), ==, 0);
    g_assert_cmpuint(number_at(qts, HUB_BY_NUMBER(1)), ==, 1);
}

/*
 * Aim the second chassis's Bridge host-error interrupt at the Hub of one
 * node number, and arm it. Nothing here says which board that is: the
 * Bridge has a number, and finding the Hub is the fabric's job.
 */
static void route_second_chassis_host_error(QTestState *qts,
                                            unsigned int number)
{
    qtest_writel(qts, REG_BY_NUMBER(0, BRIDGE_INT_DEST_LO),
                 (uint32_t)(HUB_LOCAL - SPACE_XTALK) + PI_INT_MODIFY);
    qtest_writel(qts, REG_BY_NUMBER(0, BRIDGE_INT_DEST_UP),
                 number << (38 - 32));
    qtest_writel(qts, REG_BY_NUMBER(0, BRIDGE_HOST_ERR), ERR_HOST_BIT);
    qtest_writel(qts, REG_BY_NUMBER(0, BRIDGE_INT_ENABLE), INT_INVALID_ADDR);
}

/*
 * A Bridge addressing its own chassis's number must reach the Hub beside
 * it, not the Hub that happens to sit where that number points. The
 * PROM's Bridge sanity check provokes this very error and then polls its
 * own pending register for the arrival, so a packet that lands on the
 * other chassis is a diagnostic that fails with nothing to show for it.
 */
static void test_host_error_reaches_its_own_number(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE);

    number_the_first_chassis_one(qts);

    route_second_chassis_host_error(qts, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NUMBER(0) + PI_INT_PEND0), ==, 0);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_POSITION + PI_INT_PEND0), ==, 0);

    qtest_writel(qts, BRIDGE_BY_NUMBER(0) + UNDEFINED_REG, 0xdeadbeefu);

    g_assert_cmphex(qtest_readq(qts, HUB_BY_NUMBER(0) + PI_INT_PEND0), ==,
                    1ULL << ERR_HOST_BIT);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_POSITION + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

/*
 * The other direction, so that arriving on the near Hub cannot be mistaken
 * for a packet that never crosses: the same Bridge addressing the other
 * chassis's number has to reach the far Hub.
 */
static void test_host_error_crosses_to_another_number(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_TWO_NODE);

    number_the_first_chassis_one(qts);

    route_second_chassis_host_error(qts, 1);
    qtest_writel(qts, BRIDGE_BY_NUMBER(0) + UNDEFINED_REG, 0xdeadbeefu);

    g_assert_cmphex(qtest_readq(qts, HUB_BY_POSITION + PI_INT_PEND0), ==,
                    1ULL << ERR_HOST_BIT);
    g_assert_cmphex(qtest_readq(qts, HUB_BY_NUMBER(0) + PI_INT_PEND0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sn0/bridge/ssram-and-undefined-slot-post",
                        NULL, test_ssram_and_undefined_slot_post);
    qtest_add_data_func("sn0/bridge/invalid-address-latches-status",
                        NULL, test_invalid_address_latches_status);
    qtest_add_data_func("sn0/bridge/reset-strobe-clears-only-its-group",
                        NULL, test_reset_strobe_clears_only_its_own_group);
    qtest_add_data_func("sn0/bridge/invalid-address-raises-host-interrupt",
                        NULL, test_invalid_address_raises_host_interrupt);
    qtest_add_data_func("sn0/bridge/host-error-reaches-hub-of-its-own-number",
                        NULL, test_host_error_reaches_its_own_number);
    qtest_add_data_func("sn0/bridge/host-error-crosses-to-another-number",
                        NULL, test_host_error_crosses_to_another_number);

    return g_test_run();
}
