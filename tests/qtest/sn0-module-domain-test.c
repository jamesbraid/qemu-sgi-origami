/*
 * Power domains of a two-module Origin 200: a module's reset or power cycle
 * restarts that module's processors and parts and nothing else, the
 * controllers run through every module event, and the machine ends only when
 * every module is off with no power-up to come.
 *
 * Each processor counts how many times it has started, and then counts for
 * as long as it runs, in its own node's memory. A module event restarts the
 * count only on the processors it reached, and the controllers' own state
 * (here the console selection) shows what they kept.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sn0-user-image.h"
#include "sgi-test.h"

#define MACHINE_ARGS \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -cpu R10000 -accel tcg,thread=multi -S -no-shutdown"
/* The same machine with no processor running, for the controllers alone. */
#define IDLE_MACHINE_ARGS \
    "-M origin200,topology=origin200-dual " SN0_TWO_NODE_TOPOLOGY \
    " -no-shutdown"

#define HUB_BY_NODE         (SPACE_IO + 0x01800000)
#define I2C_DATA            0x220000
#define I2C_CTRL            0x220008
#define CTL_PIN             0x80
#define CTL_ESO             0x40
#define CTL_STA             0x04
#define CTL_STO             0x02
#define CTL_ACK             0x01
#define ELSC_ADDR           0x08
#define RESP_CAP            96

#define HUB_NI_SCRATCH_0    0x600100
#define SCRATCH_MARK        0x1234

/* Per-processor words in the processor's own node: starts and progress. */
#define START_COUNT         0x3000u
#define PROGRESS_COUNT      0x3100u
/* The Status register as the processor's latest start found it. */
#define ENTRY_STATUS        0x3200u

/* "pwr c 5": five seconds off, then the four-second power-up hold. */
#define ELSC_CYCLE_NS       (9 * NANOSECONDS_PER_SECOND)

static void stub_exception_vectors(SN0Image *p, unsigned int entry)
{
    unsigned int at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);

    sn0_patch_branch(p, at, entry);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }
    p->pos = entry;
}

/* Count a start, then count for as long as the processor runs. */
static void build_counting_image(SN0Image *p)
{
    unsigned int loop;

    stub_exception_vectors(p, 0x800);
    sn0_emit_mfc0(p, SN0_R_S2, SN0_CP0_STATUS);
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    sn0_emit_cpu_number(p, SN0_R_T2);
    sn0_emit_sll(p, SN0_R_T1, SN0_R_T2, 3);

    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(ENTRY_STATUS));
    sn0_emit_addu(p, SN0_R_S0, SN0_R_S0, SN0_R_T1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_S2, 0);

    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(START_COUNT));
    sn0_emit_addu(p, SN0_R_S0, SN0_R_S0, SN0_R_T1);
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S0, SN0_R_S1, 0);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S1, SN0_R_S1, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_S1, 0);

    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(PROGRESS_COUNT));
    sn0_emit_addu(p, SN0_R_S0, SN0_R_S0, SN0_R_T1);
    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LW, SN0_R_S0, SN0_R_S1, 0);
    sn0_emit_i(p, SN0_OPC_ADDIU, SN0_R_S1, SN0_R_S1, 1);
    sn0_emit_i(p, SN0_OPC_SW, SN0_R_S0, SN0_R_S1, 0);
    sn0_patch_branch(p, sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO,
                                        SN0_R_ZERO), loop);
}

/* A word of the memory a processor sees as its own node's, by CPU index. */
static uint32_t cpu_word(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[4];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static uint64_t cpu_dword(QTestState *qts, unsigned int cpu, uint64_t addr)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void cpu_write_dword(QTestState *qts, unsigned int cpu, uint64_t addr,
                            uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, cpu, addr, bytes, sizeof(bytes)));
}

static uint32_t starts(QTestState *qts, unsigned int cpu)
{
    return cpu_word(qts, cpu, START_COUNT + 8 * (cpu & 1));
}

static uint32_t entry_status(QTestState *qts, unsigned int cpu)
{
    return cpu_word(qts, cpu, ENTRY_STATUS + 8 * (cpu & 1));
}

static uint32_t progress(QTestState *qts, unsigned int cpu)
{
    return cpu_word(qts, cpu, PROGRESS_COUNT + 8 * (cpu & 1));
}

/* Wait for a processor's start count to reach a value. */
static void wait_starts(QTestState *qts, unsigned int cpu, uint32_t count)
{
    SgiWait w;

    sgi_wait_start(&w, "CPU %u to have started %u times", cpu, count);
    while (starts(qts, cpu) < count) {
        sgi_wait_pause(&w, 1000);
    }
}

/* A processor that is running moves its progress count along. */
static void assert_running(QTestState *qts, unsigned int cpu)
{
    uint32_t before = progress(qts, cpu);
    SgiWait w;

    sgi_wait_start(&w, "CPU %u to make progress", cpu);
    while (progress(qts, cpu) == before) {
        sgi_wait_pause(&w, 1000);
    }
}

/*
 * A processor that is stopped leaves its progress count where it was. The
 * stop reaches it a moment after the controller's answer, so first wait for
 * the count to settle.
 */
static void assert_stopped(QTestState *qts, unsigned int cpu)
{
    uint32_t before = progress(qts, cpu), after;
    SgiWait w;

    sgi_wait_start(&w, "CPU %u to stop", cpu);
    for (;;) {
        g_usleep(50 * 1000);
        after = progress(qts, cpu);
        if (after == before) {
            break;
        }
        before = after;
        sgi_wait_pause(&w, 1000);
    }
    g_usleep(300 * 1000);
    g_assert_cmpuint(progress(qts, cpu), ==, before);
}

static unsigned int elsc_transact_at(QTestState *qts, uint64_t hub,
                                     const char *text, uint8_t *resp)
{
    uint8_t req[16];
    unsigned int len = strlen(text), sum = 0, i, total;

    req[0] = len;
    memcpy(&req[1], text, len);
    for (i = 0; i < len + 1; i++) {
        sum += req[i];
    }
    req[len + 1] = (uint8_t)-(int)sum;

    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO);
    qtest_writeq(qts, hub + I2C_DATA, (ELSC_ADDR << 1) | 0);
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STA | CTL_ACK);
    for (i = 0; i < len + 2; i++) {
        qtest_writeq(qts, hub + I2C_DATA, req[i]);
    }
    qtest_writeq(qts, hub + I2C_CTRL, CTL_ESO | CTL_STA | CTL_ACK);
    qtest_writeq(qts, hub + I2C_DATA, (ELSC_ADDR << 1) | 1);
    (void)qtest_readq(qts, hub + I2C_DATA);
    resp[0] = qtest_readq(qts, hub + I2C_DATA);
    total = resp[0] + 2;
    g_assert_cmpuint(total, <=, RESP_CAP);
    for (i = 1; i < total; i++) {
        if (i == total - 1) {
            qtest_writeq(qts, hub + I2C_CTRL, CTL_ESO);
        }
        resp[i] = qtest_readq(qts, hub + I2C_DATA);
    }
    qtest_writeq(qts, hub + I2C_CTRL, CTL_PIN | CTL_ESO | CTL_STO);
    return total;
}

#define MASTER_HUB          HUB_LOCAL
#define SLAVE_HUB           (HUB_BY_NODE + NODE_STRIDE)

static void assert_reply(QTestState *qts, uint64_t hub, const char *text,
                         const char *expect)
{
    uint8_t resp[RESP_CAP];
    unsigned int total = elsc_transact_at(qts, hub, text, resp);

    g_assert_cmpuint(total, >=, 2);
    g_assert_cmpuint(resp[0], >=, strlen(expect));
    g_assert_cmpmem(&resp[1], strlen(expect), expect, strlen(expect));
}

/* The firmware's job: the slave's Hub takes node number 1. */
static void number_slave(QTestState *qts)
{
    sn0_program_nasid(qts, 1);
}

static QTestState *start_counting(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *qts;
    uint8_t zero[4], one[4];

    build_counting_image(image);
    path = sn0_image_write(image, "sn0-module-domain");
    qts = qtest_initf(MACHINE_ARGS " -bios %s", path);
    unlink(path);

    stl_be_p(zero, 0);
    stl_be_p(one, 2);
    g_assert_true(qtest_memwrite_cpu(qts, 0, SN0_NODE_ID_PHYS,
                                     zero, sizeof(zero)));
    g_assert_true(qtest_memwrite_cpu(qts, 2, SN0_NODE_ID_PHYS,
                                     one, sizeof(one)));
    number_slave(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        wait_starts(qts, cpu, 1);
    }
    return qts;
}

/*
 * Leave a mark in each module's Hub and in each controller: a Hub register
 * only a reset of that Hub clears, and the console selection only the
 * controller's own start clears.
 */
static void plant_state(QTestState *qts)
{
    cpu_write_dword(qts, 0, SN0_HUB_LOCAL + HUB_NI_SCRATCH_0, SCRATCH_MARK);
    cpu_write_dword(qts, 2, SN0_HUB_LOCAL + HUB_NI_SCRATCH_0, SCRATCH_MARK);
    assert_reply(qts, MASTER_HUB, "sel none", "ok");
    assert_reply(qts, SLAVE_HUB, "sel none", "ok");
}

static void assert_controllers_kept_state(QTestState *qts)
{
    assert_reply(qts, MASTER_HUB, "sel", "ok none");
    assert_reply(qts, SLAVE_HUB, "sel", "ok none");
}

/* The module whose Hub is @cpu's got its Hub reset; the other's did not. */
static void assert_hub_reset(QTestState *qts, unsigned int cpu, bool reset)
{
    g_assert_cmphex(cpu_dword(qts, cpu, SN0_HUB_LOCAL + HUB_NI_SCRATCH_0), ==,
                    reset ? 0 : SCRATCH_MARK);
}

static void assert_no_machine_reset(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-status' }");

    qobject_unref(rsp);
    g_assert_null(qtest_qmp_event_ref(qts, "RESET"));
}

static void test_slave_reset_restarts_only_the_slave(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_state(qts);

    assert_reply(qts, SLAVE_HUB, "rst", "ok");
    wait_starts(qts, 2, 2);
    wait_starts(qts, 3, 2);
    number_slave(qts);

    /* The slave's processors and Hub started over; the master's did not. */
    g_assert_cmpuint(starts(qts, 0), ==, 1);
    g_assert_cmpuint(starts(qts, 1), ==, 1);
    g_assert_cmpuint(starts(qts, 2), ==, 2);
    g_assert_cmpuint(starts(qts, 3), ==, 2);
    assert_hub_reset(qts, 0, false);
    assert_hub_reset(qts, 2, true);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        assert_running(qts, cpu);
    }
    assert_controllers_kept_state(qts);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

static void test_master_reset_restarts_only_the_master(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_state(qts);

    assert_reply(qts, MASTER_HUB, "rst", "ok");
    wait_starts(qts, 0, 2);
    wait_starts(qts, 1, 2);

    g_assert_cmpuint(starts(qts, 2), ==, 1);
    g_assert_cmpuint(starts(qts, 3), ==, 1);
    assert_hub_reset(qts, 0, true);
    assert_hub_reset(qts, 2, false);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        assert_running(qts, cpu);
    }
    assert_controllers_kept_state(qts);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

/*
 * A slave power cycle stops its processors for the whole off interval while
 * the master's run on, and cold-starts them when power returns. With
 * processors running on the host's TCG threads the virtual clock is the
 * host's, so these cases wait out the controller's seconds.
 */
static void test_slave_power_cycle_stops_only_the_slave(void)
{
    QTestState *qts;
    uint8_t resp[RESP_CAP];

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_state(qts);

    assert_reply(qts, SLAVE_HUB, "pwr c 5", "ok");
    assert_reply(qts, SLAVE_HUB, "pwr", "ok d");
    assert_stopped(qts, 2);
    assert_stopped(qts, 3);
    assert_running(qts, 0);
    assert_running(qts, 1);
    assert_reply(qts, MASTER_HUB, "pwr", "ok u");

    wait_starts(qts, 2, 2);
    wait_starts(qts, 3, 2);
    number_slave(qts);
    g_assert_cmpuint(starts(qts, 0), ==, 1);
    g_assert_cmpuint(starts(qts, 1), ==, 1);
    assert_hub_reset(qts, 0, false);
    assert_hub_reset(qts, 2, true);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        assert_running(qts, cpu);
    }
    assert_reply(qts, SLAVE_HUB, "pwr", "ok u");

    /* The power-up set the slave's cold-power latch, once. */
    elsc_transact_at(qts, SLAVE_HUB, "pwr q", resp);
    g_assert_cmpmem(&resp[1], 4, "ok 1", 4);
    elsc_transact_at(qts, SLAVE_HUB, "pwr q", resp);
    g_assert_cmpmem(&resp[1], 4, "ok 0", 4);
    assert_controllers_kept_state(qts);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

#define HUB_PI_NMI_A        0x000070
#define HUB_PI_NMI_B        0x000078
#define HUB_PI_SOFT_RESET   0x000080
#define HUB_PI_ERR_SPOOL_A  0x000418
#define HUB_PI_ERR_SPOOL_B  0x000420
#define HUB_NI_PORT_RESET   0x600008
#define HUB_NI_PROTECTION   0x600010
#define PORT_RESET_LINK     (1u << 1)
#define PORT_RESET_WARM     (1u << 7)
#define PORT_RESET_LOCAL    (1u << 0)
#define SPOOL_MARKER        0x52737430u
#define SPOOL_PROM_OP       0x0ead0030u
#define STATUS_SR           (1u << 20)
#define STATUS_NMI          (1u << 19)

static void hub_write(QTestState *qts, unsigned int cpu, uint64_t off,
                      uint64_t value)
{
    cpu_write_dword(qts, cpu, SN0_HUB_LOCAL + off, value);
}

static uint64_t hub_read(QTestState *qts, unsigned int cpu, uint64_t off)
{
    return cpu_dword(qts, cpu, SN0_HUB_LOCAL + off);
}

/*
 * Plant what a node reset must leave alone (the two error-spool addresses
 * the PROM keeps across its own reset) and what it must clear (a scratch
 * register), in the node whose first processor is @cpu.
 */
static void plant_node(QTestState *qts, unsigned int cpu)
{
    hub_write(qts, cpu, HUB_PI_ERR_SPOOL_A, SPOOL_PROM_OP);
    hub_write(qts, cpu, HUB_PI_ERR_SPOOL_B, SPOOL_MARKER);
    hub_write(qts, cpu, HUB_NI_SCRATCH_0, SCRATCH_MARK);
}

static void assert_spool_kept(QTestState *qts, unsigned int cpu)
{
    g_assert_cmphex(hub_read(qts, cpu, HUB_PI_ERR_SPOOL_A), ==,
                    SPOOL_PROM_OP);
    g_assert_cmphex(hub_read(qts, cpu, HUB_PI_ERR_SPOOL_B), ==, SPOOL_MARKER);
}

/* Time for a wrongly scheduled restart to have happened. */
static void settle(void)
{
    g_usleep(400 * 1000);
}

/*
 * Port reset bit 0 resets this node and no other: its processors start
 * again and its Hub returns to reset, while the error-spool addresses the
 * firmware keeps across a reset survive; the other node runs on untouched,
 * and neither controller notices.
 */
static void test_port_reset_bit0_resets_one_node(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_node(qts, 0);
    plant_node(qts, 2);
    assert_reply(qts, MASTER_HUB, "sel none", "ok");
    assert_reply(qts, SLAVE_HUB, "sel none", "ok");

    hub_write(qts, 2, HUB_NI_PORT_RESET, PORT_RESET_LOCAL);
    wait_starts(qts, 2, 2);
    wait_starts(qts, 3, 2);
    settle();
    number_slave(qts);

    g_assert_cmpuint(starts(qts, 0), ==, 1);
    g_assert_cmpuint(starts(qts, 1), ==, 1);
    assert_hub_reset(qts, 0, false);
    assert_hub_reset(qts, 2, true);
    assert_spool_kept(qts, 0);
    assert_spool_kept(qts, 2);
    g_assert_cmphex(entry_status(qts, 2) & (STATUS_SR | STATUS_NMI), ==, 0);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        assert_running(qts, cpu);
    }
    assert_controllers_kept_state(qts);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

/*
 * Bits 7 and 0 send the warm reset over the link to the node across it,
 * which resets unless its protection register's reset bit is clear.
 */
static void test_port_reset_bit7_reaches_unfenced_node(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_node(qts, 0);
    plant_node(qts, 2);

    hub_write(qts, 0, HUB_NI_PORT_RESET, PORT_RESET_WARM | PORT_RESET_LOCAL);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        wait_starts(qts, cpu, 2);
    }
    settle();
    number_slave(qts);
    assert_hub_reset(qts, 0, true);
    assert_hub_reset(qts, 2, true);
    assert_spool_kept(qts, 0);
    assert_spool_kept(qts, 2);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

/* A node that opted out of resets from the link keeps running. */
static void test_port_reset_bit7_stops_at_the_fence(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_node(qts, 0);
    plant_node(qts, 2);
    hub_write(qts, 2, HUB_NI_PROTECTION, 0);

    hub_write(qts, 0, HUB_NI_PORT_RESET, PORT_RESET_WARM | PORT_RESET_LOCAL);
    wait_starts(qts, 0, 2);
    wait_starts(qts, 1, 2);
    settle();
    g_assert_cmpuint(starts(qts, 2), ==, 1);
    g_assert_cmpuint(starts(qts, 3), ==, 1);
    assert_hub_reset(qts, 0, true);
    assert_hub_reset(qts, 2, false);
    assert_running(qts, 2);
    assert_running(qts, 3);

    /*
     * The warm reset alone, with the fence open again, resets the far node
     * and leaves the sender running: the model's choice for a write no
     * software makes.
     */
    hub_write(qts, 2, HUB_NI_PROTECTION, 1);
    hub_write(qts, 0, HUB_NI_PORT_RESET, PORT_RESET_WARM);
    wait_starts(qts, 2, 2);
    wait_starts(qts, 3, 2);
    settle();
    g_assert_cmpuint(starts(qts, 0), ==, 2);
    g_assert_cmpuint(starts(qts, 1), ==, 2);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

/* The Hub's NMI register for a processor raises that processor's NMI. */
static void test_hub_nmi_reaches_one_processor(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();

    hub_write(qts, 0, HUB_PI_NMI_A, 1);
    wait_starts(qts, 0, 2);
    settle();
    g_assert_cmphex(entry_status(qts, 0) & (STATUS_SR | STATUS_NMI), ==,
                    STATUS_SR | STATUS_NMI);
    g_assert_cmpuint(starts(qts, 1), ==, 1);
    g_assert_cmpuint(starts(qts, 2), ==, 1);
    g_assert_cmpuint(starts(qts, 3), ==, 1);

    hub_write(qts, 2, HUB_PI_NMI_B, 1);
    wait_starts(qts, 3, 2);
    settle();
    g_assert_cmphex(entry_status(qts, 3) & (STATUS_SR | STATUS_NMI), ==,
                    STATUS_SR | STATUS_NMI);
    g_assert_cmpuint(starts(qts, 0), ==, 2);
    g_assert_cmpuint(starts(qts, 1), ==, 1);
    g_assert_cmpuint(starts(qts, 2), ==, 1);
    qtest_quit(qts);
}

/*
 * The soft reset register gives both of its node's processors the soft
 * reset entry, Status.SR set and NMI clear, and leaves the other node and
 * the Hub's state alone.
 */
static void test_hub_soft_reset_takes_the_node_s_processors(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }
    qts = start_counting();
    plant_node(qts, 0);

    hub_write(qts, 0, HUB_PI_SOFT_RESET, 1);
    wait_starts(qts, 0, 2);
    wait_starts(qts, 1, 2);
    settle();
    for (unsigned int cpu = 0; cpu < 2; cpu++) {
        g_assert_cmphex(entry_status(qts, cpu) & (STATUS_SR | STATUS_NMI), ==,
                        STATUS_SR);
    }
    g_assert_cmpuint(starts(qts, 2), ==, 1);
    g_assert_cmpuint(starts(qts, 3), ==, 1);
    assert_hub_reset(qts, 0, false);
    assert_no_machine_reset(qts);
    qtest_quit(qts);
}

static bool powered_down(QTestState *qts)
{
    QDict *rsp = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    bool down = g_str_equal(qdict_get_str(qdict_get_qdict(rsp, "return"),
                                          "status"), "shutdown");

    qobject_unref(rsp);
    return down;
}

/*
 * The machine ends only when every module is off and nothing will power one
 * up. A power cycle in progress is a power-up to come, on either module.
 */
static void test_shutdown_waits_for_a_power_up(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);

    sn0_program_nasid(qts, 1);
    assert_reply(qts, SLAVE_HUB, "pwr c 5", "ok");
    assert_reply(qts, MASTER_HUB, "pwr d 0", "ok");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    assert_reply(qts, MASTER_HUB, "pwr", "ok d");
    assert_reply(qts, SLAVE_HUB, "pwr", "ok d");
    g_assert_false(powered_down(qts));

    /*
     * The interval and the power-up hold pass: the slave is back, with a
     * cold Hub that the firmware would number again.
     */
    qtest_clock_step(qts, ELSC_CYCLE_NS);
    number_slave(qts);
    assert_reply(qts, SLAVE_HUB, "pwr", "ok u");
    g_assert_false(powered_down(qts));

    /* Now nothing is pending: the slave's own power-off ends the machine. */
    assert_reply(qts, SLAVE_HUB, "pwr d 0", "ok");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

/* The same, with the master cycling and the slave powering off. */
static void test_shutdown_waits_for_a_master_power_up(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);

    sn0_program_nasid(qts, 1);
    assert_reply(qts, MASTER_HUB, "pwr c 5", "ok");
    assert_reply(qts, SLAVE_HUB, "pwr d 0", "ok");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));
    qtest_clock_step(qts, ELSC_CYCLE_NS);
    assert_reply(qts, MASTER_HUB, "pwr", "ok u");
    g_assert_false(powered_down(qts));
    qtest_quit(qts);
}

/*
 * The heartbeat watchdog's power cycle is a power-up to come too: its stage
 * leads to a cycle, so the machine stays while it is armed.
 */
static void test_shutdown_waits_for_a_watchdog_cycle(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);

    sn0_program_nasid(qts, 1);
    assert_reply(qts, SLAVE_HUB, "hbt 10 5", "ok");
    assert_reply(qts, MASTER_HUB, "pwr d 0", "ok");
    assert_reply(qts, SLAVE_HUB, "pwr d 0", "ok");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));

    /* Cancelling it leaves nothing to power a module up: the machine ends. */
    assert_reply(qts, SLAVE_HUB, "hbt 0", "ok");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

/*
 * A power-down countdown that runs out switches off its own module and not
 * the machine, and the other module's controller is untouched.
 */
static void test_countdown_expiry_powers_off_one_module(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);

    sn0_program_nasid(qts, 1);
    assert_reply(qts, MASTER_HUB, "sel none", "ok");
    assert_reply(qts, SLAVE_HUB, "pwr d 3", "ok");
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, SLAVE_HUB, "pwr", "ok u");
    qtest_clock_step(qts, 2 * NANOSECONDS_PER_SECOND);
    assert_reply(qts, SLAVE_HUB, "pwr", "ok d");
    assert_reply(qts, MASTER_HUB, "pwr", "ok u");
    assert_reply(qts, MASTER_HUB, "sel", "ok none");
    g_assert_false(powered_down(qts));
    qtest_quit(qts);
}

/*
 * Every power-up of a module sets its cold-power latch once, for its
 * controller alone: the first query after each reads 1, the next 0, and the
 * other controller's latch is neither set nor cleared by it.
 */
static void test_each_power_up_sets_the_latch(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);
    uint8_t resp[RESP_CAP];

    sn0_program_nasid(qts, 1);
    for (unsigned int cycle = 0; cycle < 2; cycle++) {
        elsc_transact_at(qts, SLAVE_HUB, "pwr q", resp);
        assert_reply(qts, SLAVE_HUB, "pwr c 5", "ok");
        qtest_clock_step(qts, ELSC_CYCLE_NS);
        number_slave(qts);
        elsc_transact_at(qts, SLAVE_HUB, "pwr q", resp);
        g_assert_cmpmem(&resp[1], 4, "ok 1", 4);
        elsc_transact_at(qts, SLAVE_HUB, "pwr q", resp);
        g_assert_cmpmem(&resp[1], 4, "ok 0", 4);
    }
    /* The master was never powered up again, and its latch is untouched. */
    elsc_transact_at(qts, MASTER_HUB, "pwr q", resp);
    g_assert_cmpmem(&resp[1], 4, "ok 1", 4);
    qtest_quit(qts);
}

/* Nothing pending and every module off: the machine ends. */
static void test_shutdown_ends_when_all_off(void)
{
    QTestState *qts = qtest_init(IDLE_MACHINE_ARGS);

    sn0_program_nasid(qts, 1);
    assert_reply(qts, MASTER_HUB, "pwr d 0", "ok");
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND);
    g_assert_false(powered_down(qts));
    assert_reply(qts, SLAVE_HUB, "pwr d 0", "ok");
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/module-domain/slave-reset",
                   test_slave_reset_restarts_only_the_slave);
    qtest_add_func("/sn0/module-domain/master-reset",
                   test_master_reset_restarts_only_the_master);
    qtest_add_func("/sn0/module-domain/port-reset-bit0",
                   test_port_reset_bit0_resets_one_node);
    qtest_add_func("/sn0/module-domain/port-reset-bit7",
                   test_port_reset_bit7_reaches_unfenced_node);
    qtest_add_func("/sn0/module-domain/port-reset-fence",
                   test_port_reset_bit7_stops_at_the_fence);
    qtest_add_func("/sn0/module-domain/hub-nmi",
                   test_hub_nmi_reaches_one_processor);
    qtest_add_func("/sn0/module-domain/hub-soft-reset",
                   test_hub_soft_reset_takes_the_node_s_processors);
    qtest_add_func("/sn0/module-domain/slave-power-cycle",
                   test_slave_power_cycle_stops_only_the_slave);
    qtest_add_func("/sn0/module-domain/shutdown-waits-for-power-up",
                   test_shutdown_waits_for_a_power_up);
    qtest_add_func("/sn0/module-domain/shutdown-waits-for-master-power-up",
                   test_shutdown_waits_for_a_master_power_up);
    qtest_add_func("/sn0/module-domain/shutdown-waits-for-watchdog-cycle",
                   test_shutdown_waits_for_a_watchdog_cycle);
    qtest_add_func("/sn0/module-domain/countdown-expiry",
                   test_countdown_expiry_powers_off_one_module);
    qtest_add_func("/sn0/module-domain/each-power-up-sets-latch",
                   test_each_power_up_sets_the_latch);
    qtest_add_func("/sn0/module-domain/shutdown-when-all-off",
                   test_shutdown_ends_when_all_off);
    return g_test_run();
}
