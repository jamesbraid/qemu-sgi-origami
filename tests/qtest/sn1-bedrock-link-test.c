/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Two Bedrocks joined by one NUMAlink 3 cable, as a two-chassis Origin 300
 * joins its nodes (the network-interface datasheet, "Link bring-up"; the
 * local-block datasheet, "Vector engine on the boot path"). The empty
 * machine's R10000 supplies qtest's address space only: no firmware runs,
 * and this bench is not an Origin 300. It covers what the stock PROM needs
 * of the pair at register level: link-up visible from reset with no software
 * step, and route 0 reaching the peer's registers for reads and writes.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define A_PATH      "/machine/peripheral/a"
#define B_PATH      "/machine/peripheral/b"
#define CABLE_PATH  "/machine/peripheral/cable"
#define A_BASE      UINT64_C(0x10000000)
#define B_BASE      UINT64_C(0x20000000)

#define MACHINE_BASE \
    "-M none -cpu R10000 -m 256M " \
    "-object memory-backend-ram,id=dma,size=2M " \
    "-device sgi-bedrock,id=a,revision=2,dma-memory=/objects/dma/dma[0] " \
    "-device sgi-bedrock,id=b,revision=2,dma-memory=/objects/dma/dma[0] "
#define MACHINE MACHINE_BASE \
    "-device sgi-net-cable,id=cable,end0=" A_PATH ",end1=" B_PATH " -S"

#define LB(base, reg)       ((base) + UINT64_C(0x600000) + (reg))
#define NI(base, reg)       ((base) + UINT64_C(0x680000) + (reg))
#define REVISION            0x0000
#define SCRATCH(n)          (0x8000 + (n) * 8)
#define VECTOR_PARAMS       0xc000
#define VECTOR_ROUTE        0xc008
#define VECTOR_DATA         0xc010
#define VECTOR_STATUS       0xc020
#define VECTOR_RETURN       0xc028
#define VECTOR_READ         0xc030
#define VECTOR_STATUS_CLEAR 0xc038
#define PORT_STATUS         0x0000
#define LINK_UP             (UINT64_C(1) << 1)

/* Hub-relative offsets, as the parameters register's address field holds. */
#define HUB_REVISION        UINT64_C(0x600000)
#define HUB_SCRATCH(n)      (UINT64_C(0x608000) + (n) * 8)

#define TYPE_WRITE          UINT64_C(1)
#define WRITE_ID(n)         ((uint64_t)(n) << 32)
#define REQUEST_ID(n)       ((uint64_t)(n) << 40)
#define STATUS_ECHO         ((UINT64_C(0x1fffff) << 3) | \
                             (UINT64_C(0xff) << 32) | \
                             (UINT64_C(0x7ff) << 40))
#define STATUS_ERROR        UINT64_C(7)
#define STATUS_ADDR_ERROR   UINT64_C(4)
#define STATUS_SOURCE       (UINT64_C(0x7ff) << 51)
#define STATUS_OVERRUN      (UINT64_C(1) << 62)
#define STATUS_VALID        (UINT64_C(1) << 63)
#define SENTINEL            UINT64_C(0xdeadbeefdeadbeef)

#define BEDROCK_IDENTITY    ((UINT64_C(0x36) << 1) | \
                             (UINT64_C(0x3012) << 12) | (UINT64_C(2) << 28))

static void wire(QTestState *s, const char *bedrock)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'net-cable','value':%s}}",
        bedrock, CABLE_PATH);
}

static void set_down(QTestState *s, bool down)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'down','value':%i}}", CABLE_PATH, down);
}

static QTestState *pair_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    wire(s, A_PATH);
    wire(s, B_PATH);
    qtest_sysbus_mmio_map(s, A_PATH, 0, A_BASE);
    qtest_sysbus_mmio_map(s, B_PATH, 0, B_BASE);
    return s;
}

/*
 * The firmware's own launch: read status clear, seed read data (and, as the
 * PROM's POD primitive does, the return route), write the route and, for a
 * write, the data, then parameters last. Returns status.
 */
static uint64_t vector_launch(QTestState *s, uint64_t base, uint64_t route,
                              uint64_t params, uint64_t data)
{
    (void)qtest_readq(s, LB(base, VECTOR_STATUS_CLEAR));
    qtest_writeq(s, LB(base, VECTOR_READ), SENTINEL);
    qtest_writeq(s, LB(base, VECTOR_RETURN), SENTINEL);
    qtest_writeq(s, LB(base, VECTOR_ROUTE), route);
    qtest_writeq(s, LB(base, VECTOR_DATA), data);
    qtest_writeq(s, LB(base, VECTOR_PARAMS), params);
    return qtest_readq(s, LB(base, VECTOR_STATUS));
}

/* Return route and source are not established; every reply leaves 0. */
static void assert_unobserved_fields_clear(QTestState *s, uint64_t base,
                                           uint64_t status)
{
    g_assert_cmphex(status & STATUS_SOURCE, ==, 0);
    g_assert_cmphex(qtest_readq(s, LB(base, VECTOR_RETURN)), ==, 0);
}

static void assert_good_reply(uint64_t status, uint64_t params)
{
    g_assert_cmphex(status & STATUS_VALID, ==, STATUS_VALID);
    g_assert_cmphex(status & STATUS_OVERRUN, ==, 0);
    /* Bits 2:0 echo the request type: 0 for a read, 1 for a write. */
    g_assert_cmphex(status & STATUS_ERROR, ==, params & TYPE_WRITE);
    g_assert_cmphex(status & STATUS_ECHO, ==, params & STATUS_ECHO);
}

/*
 * Link-up comes from the cable, from reset, with nothing written to either
 * network interface; the stock PROM only polls bit 1. Marking the cable down
 * takes it away on both sides at once, and back up restores it. Bits 0 and 2
 * and the reserved M-mode bit 18 stay clear.
 */
static void test_link_follows_the_cable(void)
{
    QTestState *s = pair_start("");

    g_assert_cmphex(qtest_readq(s, NI(A_BASE, PORT_STATUS)), ==, LINK_UP);
    g_assert_cmphex(qtest_readq(s, NI(B_BASE, PORT_STATUS)), ==, LINK_UP);
    set_down(s, true);
    g_assert_cmphex(qtest_readq(s, NI(A_BASE, PORT_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(s, NI(B_BASE, PORT_STATUS)), ==, 0);
    set_down(s, false);
    g_assert_cmphex(qtest_readq(s, NI(A_BASE, PORT_STATUS)), ==, LINK_UP);
    g_assert_cmphex(qtest_readq(s, NI(B_BASE, PORT_STATUS)), ==, LINK_UP);
    qtest_quit(s);
}

/* A Bedrock whose cable reference is never wired has no link. */
static void test_unwired_port_has_no_link(void)
{
    QTestState *s = qtest_initf(MACHINE);

    qtest_sysbus_mmio_map(s, A_PATH, 0, A_BASE);
    g_assert_cmphex(qtest_readq(s, NI(A_BASE, PORT_STATUS)), ==, 0);
    qtest_quit(s);
}

/*
 * Route 0 reads the peer's register, not the launcher's: each side's scratch
 * 0 holds a different value, and each side reads the other's. The reply
 * echoes the launch and read data replaces the sentinel. The status-clear
 * read is what lets the next launch succeed; without it the second launch
 * finds status still valid and only marks overrun.
 */
static void test_route_zero_reads_the_peer(void)
{
    QTestState *s = pair_start("");
    const uint64_t params = HUB_SCRATCH(0) | REQUEST_ID(0x1c8);
    uint64_t status;

    qtest_writeq(s, LB(A_BASE, SCRATCH(0)), 0xaaaa0000aaaa0000ULL);
    qtest_writeq(s, LB(B_BASE, SCRATCH(0)), 0xbbbb0000bbbb0000ULL);

    status = vector_launch(s, A_BASE, 0, params, 0);
    assert_good_reply(status, params);
    assert_unobserved_fields_clear(s, A_BASE, status);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_READ)), ==,
                    0xbbbb0000bbbb0000ULL);

    status = vector_launch(s, B_BASE, 0, params | WRITE_ID(0x5a), 0);
    assert_good_reply(status, params | WRITE_ID(0x5a));
    g_assert_cmphex(qtest_readq(s, LB(B_BASE, VECTOR_READ)), ==,
                    0xaaaa0000aaaa0000ULL);

    /* Reading status clear returns the reply and clears it. */
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS_CLEAR)) &
                    STATUS_VALID, ==, STATUS_VALID);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS)), ==, 0);

    /* A launch over an unread reply is an overrun and nothing else. */
    qtest_writeq(s, LB(B_BASE, SCRATCH(0)), 0x1234);
    qtest_writeq(s, LB(A_BASE, VECTOR_PARAMS), params);
    qtest_writeq(s, LB(A_BASE, VECTOR_PARAMS), params);
    status = qtest_readq(s, LB(A_BASE, VECTOR_STATUS));
    g_assert_cmphex(status & (STATUS_VALID | STATUS_OVERRUN), ==,
                    STATUS_VALID | STATUS_OVERRUN);

    /* A write to status clear clears a valid reply too, the model's choice. */
    qtest_writeq(s, LB(A_BASE, VECTOR_STATUS_CLEAR), 0);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS)), ==, 0);
    /* So does a write of zero to status itself; a nonzero write does not. */
    qtest_writeq(s, LB(A_BASE, VECTOR_PARAMS), params);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS)) & STATUS_VALID,
                    ==, STATUS_VALID);
    qtest_writeq(s, LB(A_BASE, VECTOR_STATUS), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS)) & STATUS_VALID,
                    ==, STATUS_VALID);
    qtest_writeq(s, LB(A_BASE, VECTOR_STATUS), 0);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_STATUS)), ==, 0);
    qtest_quit(s);
}

/*
 * The node-number paths the master uses on a peer: a vector write of the
 * peer's scratch 1, where a peer with processors waits for its number, and a
 * read-modify-write of a headless peer's revision word. Each changes the peer
 * and not the launcher.
 */
static void test_route_zero_writes_the_peer(void)
{
    QTestState *s = pair_start("");
    uint64_t params, status, revision;

    qtest_writeq(s, LB(A_BASE, SCRATCH(1)), 0x4000000fffULL);
    qtest_writeq(s, LB(B_BASE, SCRATCH(1)), 0x4000000fffULL);
    params = HUB_SCRATCH(1) | REQUEST_ID(0x7b) | WRITE_ID(0x11) | TYPE_WRITE;
    status = vector_launch(s, A_BASE, 0, params, 0x4000000001ULL);
    assert_good_reply(status, params);
    g_assert_cmphex(qtest_readq(s, LB(B_BASE, SCRATCH(1))), ==,
                    0x4000000001ULL);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, SCRATCH(1))), ==,
                    0x4000000fffULL);
    /* A write leaves read data as the launcher seeded it. */
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_READ)), ==, SENTINEL);

    params = HUB_REVISION | REQUEST_ID(0x1c8);
    status = vector_launch(s, A_BASE, 0, params, 0);
    assert_good_reply(status, params);
    revision = qtest_readq(s, LB(A_BASE, VECTOR_READ));
    g_assert_cmphex(revision, ==, BEDROCK_IDENTITY);
    revision = (revision & ~(UINT64_C(0xff) << 32)) | (UINT64_C(1) << 32);
    params = HUB_REVISION | REQUEST_ID(0x7b) | TYPE_WRITE;
    status = vector_launch(s, A_BASE, 0, params, revision);
    assert_good_reply(status, params);
    g_assert_cmphex(qtest_readq(s, LB(B_BASE, REVISION)), ==,
                    BEDROCK_IDENTITY | (UINT64_C(1) << 32));
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, REVISION)), ==,
                    BEDROCK_IDENTITY);
    qtest_quit(s);
}

/*
 * What latches no reply: a nonzero route, which a Bedrock at the other end
 * of a direct cable cannot forward, and any route while the cable is down.
 * Hardware's answer to either is unknown; the model's is silence, so the
 * firmware's own poll times out. The peer is untouched either way.
 */
static void test_unroutable_launch_latches_nothing(void)
{
    QTestState *s = pair_start("");
    uint64_t params = HUB_SCRATCH(1) | REQUEST_ID(0x7b) | TYPE_WRITE;

    qtest_writeq(s, LB(B_BASE, SCRATCH(1)), 0x0fff);
    g_assert_cmphex(vector_launch(s, A_BASE, 1, params, 0x1), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB(B_BASE, SCRATCH(1))), ==, 0x0fff);

    set_down(s, true);
    g_assert_cmphex(vector_launch(s, A_BASE, 0, params, 0x1), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB(B_BASE, SCRATCH(1))), ==, 0x0fff);
    params = HUB_SCRATCH(1) | REQUEST_ID(0x1c8);
    g_assert_cmphex(vector_launch(s, A_BASE, 0, params, 0), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_READ)), ==, SENTINEL);
    qtest_quit(s);
}

/*
 * A peer that has nothing at the addressed offset refuses it. The error code
 * for that is not observed; the address error is the model's choice, and the
 * reply still echoes the launch so the firmware can match it.
 */
static void test_refused_offset_is_an_address_error(void)
{
    QTestState *s = pair_start("");
    const uint64_t params = UINT64_C(0xfffff8) | REQUEST_ID(0x1c8);
    uint64_t status = vector_launch(s, A_BASE, 0, params, 0);

    g_assert_cmphex(status & STATUS_VALID, ==, STATUS_VALID);
    g_assert_cmphex(status & STATUS_ERROR, ==, STATUS_ADDR_ERROR);
    g_assert_cmphex(status & STATUS_ECHO, ==, params & STATUS_ECHO);
    assert_unobserved_fields_clear(s, A_BASE, status);
    g_assert_cmphex(qtest_readq(s, LB(A_BASE, VECTOR_READ)), ==, SENTINEL);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-link/link-follows-the-cable",
                   test_link_follows_the_cable);
    qtest_add_func("sn1/bedrock-link/unwired-port-has-no-link",
                   test_unwired_port_has_no_link);
    qtest_add_func("sn1/bedrock-link/route-zero-reads-the-peer",
                   test_route_zero_reads_the_peer);
    qtest_add_func("sn1/bedrock-link/route-zero-writes-the-peer",
                   test_route_zero_writes_the_peer);
    qtest_add_func("sn1/bedrock-link/unroutable-launch-latches-nothing",
                   test_unroutable_launch_latches_nothing);
    qtest_add_func("sn1/bedrock-link/refused-offset-is-an-address-error",
                   test_refused_offset_is_an_address_error);
    return g_test_run();
}
