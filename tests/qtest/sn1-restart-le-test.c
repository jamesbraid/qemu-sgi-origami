/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator-only little-endian restart of an Origin 300's CPUs. A stub in
 * each byte order stores the word 0x11223344, so raw RAM shows the order.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"
#include "sgi-wait.h"

#define IP53_PROM_SIZE  (16 * 1024 * 1024)
#define MAILBOX_PHYS    0x200c00u
#define LE_RAM_ENTRY    0x300000u
#define BE_ENTRY        0x4000
#define LE_FLASH_ENTRY  0x400
#define LE_DISPATCH     0x8444

#define BEDROCK_LOCAL   (SPACE_IO + UINT64_C(0x01000000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define NI_PORT_RESET   UINT64_C(0x680008)
#define PI_PENDING_MODIFY UINT64_C(0x90)
#define PI_MASK_0_A     UINT64_C(0xa8)
#define PI_MASK_0_B     UINT64_C(0xb8)
#define PI1             UINT64_C(0x200000)
#define CAUSE_IP_MASK   0xff00u
#define PORT_RESET_LOCAL 4
/* The installed banks: 0 and 1, 256 MiB each, and the IP53 population. */
#define MD_ORIGIN300    UINT64_C(0x33)
#define MD_IP53         UINT64_C(0x53)

#define R_ZERO          0
#define R_T0            8
#define R_T1            9
#define R_T2            10
#define R_T3            11
#define R_T4            12
#define R_S6            22
#define OP_LL           0x30
#define OP_SC           0x38

/* Each stub owns a block of mailbox words at MAILBOX_PHYS. */
#define BE_BASE         0x00
#define LE_BASE         0x20
#define W_CONFIG0       0x00
#define W_STORED        0x04
#define W_LOADED        0x08
#define W_MARKER        0x0c
#define W_COUNT         0x10
#define W_CAUSE         0x14

#define BE_DONE         0xbe5b1001u
#define LE_DONE         0x1e5b2002u
#define CONFIG0_BE      (1u << 15)
#define WORD            0x11223344u

typedef struct Code {
    uint8_t *b;
    unsigned int pos;
    bool be;
    uint64_t md_config;
} Code;

static void emit(Code *c, uint32_t w)
{
    if (c->be) {
        stl_be_p(c->b + c->pos, w);
    } else {
        stl_le_p(c->b + c->pos, w);
    }
    c->pos += 4;
}

static void li32(Code *c, int rt, uint32_t v)
{
    emit(c, mips_lui(rt, v >> 16));
    emit(c, mips_ori(rt, rt, v & 0xffff));
}

static void emit_configure_memory(Code *c)
{
    emit(c, mips_mfc0(R_T4, 12));
    emit(c, mips_ori(R_T4, R_T4, 0x80));            /* Status.KX */
    emit(c, mips_mtc0(R_T4, 12));
    for (int i = 0; i < 4; i++) {
        emit(c, mips_nop());
    }
    emit(c, mips_lui(R_T0, 0x9200));
    emit(c, mips_dsll32(R_T0, R_T0));
    emit(c, mips_lui(R_T1, 0x0178));
    emit(c, mips_rtype(R_T0, R_T1, R_T0, 0, 0x25)); /* or */
    emit(c, mips_ori(R_T3, R_ZERO, c->md_config));
    emit(c, mips_sd(R_T3, R_T0, 8));
    emit(c, mips_sync());
}

typedef struct Machine {
    const char *args;
    size_t prom_size;
    unsigned int cpus;
    uint64_t md_config;
    bool ip53;          /* the PROM is a pflash and the IO9 needs stores */
} Machine;

static const Machine origin300 = {
    "origin300 -cpu R14000 -smp 2", PROM_SIZE, 2, MD_ORIGIN300, false
};
static const Machine origin300_two_nodes = {
    "origin300,topology=origin300-two-chassis -cpu R14000 -smp 4", PROM_SIZE,
    4, MD_ORIGIN300, false
};
static const Machine origin350 = {
    "origin350,topology=origin350-ip53,nvram0=sgi-ds1742-data-io9,"
    "nvram0-clock=sgi-ds1742-clock-io9 -smp 4", IP53_PROM_SIZE, 4, MD_IP53,
    true
};

/*
 * Record Config0, Cause and a stored word, count with ll/sc, set a marker. A
 * big-endian stub enables the banks first; a reversed Bedrock write kills them.
 */
static void emit_stub(Code *c, unsigned int base, uint32_t done, bool count)
{
    unsigned int retry;

    if (c->be) {
        emit_configure_memory(c);
    }

    emit(c, mips_lui(R_S6, 0x8020));
    emit(c, mips_mfc0(R_T0, 16));
    emit(c, mips_sw(R_T0, R_S6, 0xc00 + base + W_CONFIG0));
    emit(c, mips_mfc0(R_T0, 13));
    emit(c, mips_sw(R_T0, R_S6, 0xc00 + base + W_CAUSE));
    li32(c, R_T1, WORD);
    emit(c, mips_sw(R_T1, R_S6, 0xc00 + base + W_STORED));
    emit(c, mips_lw(R_T2, R_S6, 0xc00 + base + W_STORED));
    emit(c, mips_sw(R_T2, R_S6, 0xc00 + base + W_LOADED));
    if (count) {
        retry = c->pos;
        emit(c, mips_itype(OP_LL, R_S6, R_T0, 0xc00 + base + W_COUNT));
        emit(c, mips_addiu(R_T0, R_T0, 1));
        emit(c, mips_itype(OP_SC, R_S6, R_T0, 0xc00 + base + W_COUNT));
        emit(c, mips_branch(MIPS_OPC_BEQ, R_T0, R_ZERO,
                            ((int)retry - (int)(c->pos + 4)) / 4));
        emit(c, mips_nop());
    }
    li32(c, R_T0, done);
    emit(c, mips_sw(R_T0, R_S6, 0xc00 + base + W_MARKER));
    emit(c, mips_branch(MIPS_OPC_BEQ, R_ZERO, R_ZERO, -1));
    emit(c, mips_nop());
}

/* Word 0 branches to a different stub in each byte order. */
static void build_image(uint8_t *img, const Machine *m, bool le_in_flash)
{
    Code c = { .b = img, .pos = BE_ENTRY, .be = true,
               .md_config = m->md_config };

    memset(img, 0, m->prom_size);
    stl_be_p(img, 0x10210010);
    stl_be_p(img + 0x44, 0x10000000 | (uint16_t)((BE_ENTRY - 0x44 - 4) / 4));
    stl_le_p(img + LE_DISPATCH, 0x10000000 |
             (uint16_t)((LE_FLASH_ENTRY - LE_DISPATCH - 4) / 4));
    emit_stub(&c, BE_BASE, BE_DONE, false);
    if (le_in_flash) {
        c.pos = LE_FLASH_ENTRY;
        c.be = false;
        emit_stub(&c, LE_BASE, LE_DONE, true);
    }
}

/* A file of @size bytes holding @data, or zeros, in the test's temp area. */
static char *temp_file(const uint8_t *data, size_t size)
{
    g_autofree uint8_t *zero = g_malloc0(size);
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-restart-le-XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, data ? data : zero, size), ==, size);
    close(fd);
    return path;
}

/* Stopped, with the installed banks enabled, so a stub can be placed. */
static QTestState *start(const Machine *m, bool le_in_flash)
{
    g_autofree uint8_t *img = g_malloc(m->prom_size);
    g_autofree char *bios = NULL;
    g_autofree char *stores = NULL;
    g_autofree char *clock = NULL;
    g_autofree char *drives = NULL;
    QTestState *qts;

    build_image(img, m, le_in_flash);
    bios = temp_file(img, m->prom_size);
    if (m->ip53) {
        stores = temp_file(NULL, 0x8000);
        clock = temp_file(NULL, 16);
        drives = g_strdup_printf(
            "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
            "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw "
            "-drive if=pflash,index=0,file=%s,format=raw",
            stores, clock, bios);
    } else {
        drives = g_strdup_printf("-bios %s", bios);
    }
    qts = qtest_initf("-M %s %s -accel tcg -S", m->args, drives);
    unlink(bios);
    if (m->ip53) {
        unlink(stores);
        unlink(clock);
    }
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, m->md_config);
    return qts;
}

static void cont(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
}

static uint32_t mailbox(QTestState *qts, unsigned int off, bool be)
{
    uint8_t raw[4];

    qtest_memread(qts, MAILBOX_PHYS + off, raw, 4);
    return be ? ldl_be_p(raw) : ldl_le_p(raw);
}

static void wait_mailbox(QTestState *qts, unsigned int off, uint32_t want,
                         bool be)
{
    SgiWait w;

    sgi_wait_start(&w, "mailbox 0x%x", off);
    while (mailbox(qts, off, be) != want && sgi_wait_continue(&w, 5000)) {
        /* poll */
    }
    g_assert_cmphex(mailbox(qts, off, be), ==, want);
}

static void set_order(QTestState *qts, const char *order)
{
    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine', 'property': "
                             "'x-cpu-byte-order', 'value': %s}}", order);
}

/* Stop, change the byte order and resume, as the harness does. */
static void restart(QTestState *qts, bool with_entry)
{
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    if (with_entry) {
        qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                                 "{'path': '/machine', 'property': "
                                 "'x-restart-entry', 'value': %u}}",
                                 LE_RAM_ENTRY);
    }
    set_order(qts, "little");
    cont(qts);
}

static bool order_is(QTestState *qts, const char *want)
{
    QDict *rsp = qtest_qmp(
        qts, "{'execute': 'qom-get', 'arguments': {'path': '/machine', "
        "'property': 'x-cpu-byte-order'}}");
    bool same = !strcmp(qdict_get_str(rsp, "return"), want);

    qobject_unref(rsp);
    return same;
}

/* A stub ran in this order: Config0.BE, and the word's bytes in RAM. */
static void check_order(QTestState *qts, bool be, unsigned int cpus)
{
    unsigned int base = be ? BE_BASE : LE_BASE;
    uint8_t raw[4], want[4];

    if (cpus) {
        wait_mailbox(qts, base + W_COUNT, cpus, false);
    }
    wait_mailbox(qts, base + W_MARKER, be ? BE_DONE : LE_DONE, be);
    g_assert_cmphex(mailbox(qts, base + W_CONFIG0, be) & CONFIG0_BE, ==,
                    be ? CONFIG0_BE : 0);
    if (be) {
        stl_be_p(want, WORD);
    } else {
        stl_le_p(want, WORD);
    }
    qtest_memread(qts, MAILBOX_PHYS + base + W_STORED, raw, 4);
    g_assert_cmpmem(raw, 4, want, 4);
    g_assert_cmphex(mailbox(qts, base + W_LOADED, be), ==, WORD);
}

/* Config0.BE of CPU @cpu, from the monitor's register dump. */
static bool cpu_is_big(QTestState *qts, unsigned int cpu)
{
    g_autofree char *dump = qtest_hmp(qts, "info registers -a");
    g_autofree char *tag = g_strdup_printf("CPU#%u", cpu);
    const char *at = strstr(dump, tag);

    g_assert_nonnull(at);
    at = strstr(at, "Config0 0x");
    g_assert_nonnull(at);
    return g_ascii_strtoull(at + 10, NULL, 16) & CONFIG0_BE;
}

#define check_big_endian(qts)       check_order(qts, true, 0)
#define check_little_endian(qts, n) check_order(qts, false, n)

static void put_le_stub(QTestState *qts)
{
    uint8_t stub[0x100] = { };
    Code c = { .b = stub, .md_config = 0 };

    emit_stub(&c, LE_BASE, LE_DONE, true);
    qtest_memwrite(qts, LE_RAM_ENTRY, stub, c.pos);
}

/*
 * Two CPUs run big-endian until the harness restarts both at the entry. Each
 * must come out of its start-powered-off halt to add to the count.
 */
static void test_entry(const void *data)
{
    const Machine *m = data;
    QTestState *qts = start(m, false);
    uint32_t cause;

    /* Level 7 pending and unmasked for the CPUs: a line the reset drops. */
    for (uint64_t pi = 0; pi <= PI1; pi += PI1) {
        qtest_writeq(qts, BEDROCK_LOCAL + pi + PI_PENDING_MODIFY, 0x107);
        qtest_writeq(qts, BEDROCK_LOCAL + pi + PI_MASK_0_A, 1 << 7);
        qtest_writeq(qts, BEDROCK_LOCAL + pi + PI_MASK_0_B, 1 << 7);
    }
    put_le_stub(qts);
    cont(qts);
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
    cause = mailbox(qts, BE_BASE + W_CAUSE, true) & CAUSE_IP_MASK;
    g_assert_cmphex(cause, !=, 0);

    restart(qts, true);
    check_little_endian(qts, m->cpus);
    g_assert_true(order_is(qts, "little"));
    g_assert_cmphex(mailbox(qts, LE_BASE + W_CAUSE, false) & CAUSE_IP_MASK,
                    ==, cause);
    qtest_quit(qts);
}

/* A reset, from the monitor or the Bedrock, undoes it. */
static void reset_to_big(QTestState *qts, bool bedrock)
{
    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    if (bedrock) {
        qtest_writeq(qts, BEDROCK_LOCAL + NI_PORT_RESET, PORT_RESET_LOCAL);
    } else {
        qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    }
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
}

/* With no entry the CPUs take the reset vector, in either order. */
static void test_reset_vector(void)
{
    QTestState *qts = start(&origin300, true);

    cont(qts);
    check_big_endian(qts);
    restart(qts, false);
    check_little_endian(qts, 2);
    reset_to_big(qts, true);
    restart(qts, false);
    check_little_endian(qts, 2);
    reset_to_big(qts, false);
    qtest_quit(qts);
}

/* One node's port reset restarts that node alone; the other stays little. */
static void test_node_restart(void)
{
    QTestState *qts = start(&origin300_two_nodes, true);

    cont(qts);
    check_big_endian(qts);
    restart(qts, false);
    check_little_endian(qts, 2);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_false(cpu_is_big(qts, cpu));
    }
    reset_to_big(qts, true);
    for (unsigned int cpu = 0; cpu < 4; cpu++) {
        g_assert_cmpint(cpu_is_big(qts, cpu), ==, cpu < 2);
    }
    qtest_quit(qts);
}

static void test_refusals(void)
{
    QTestState *qts = start(&origin300, false);
    const uint64_t bad[] = { 2, 512 * 1024 * 1024 };

    for (int i = 0; i < ARRAY_SIZE(bad); i++) {
        qobject_unref(qtest_qmp_assert_failure_ref(
            qts, "{'execute': 'qom-set', 'arguments': {'path': '/machine', "
            "'property': 'x-restart-entry', 'value': %" PRIu64 "}}", bad[i]));
    }
    qobject_unref(qtest_qmp_assert_failure_ref(
        qts, "{'execute': 'qom-set', 'arguments': {'path': '/machine', "
        "'property': 'x-cpu-byte-order', 'value': 'middle'}}"));
    /* The current order is a no-op. */
    set_order(qts, "big");
    g_assert_true(order_is(qts, "big"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/mips64/sn1/restart-le/entry", &origin300,
                        test_entry);
    qtest_add_data_func("/mips64/sn1/restart-le/entry-origin350", &origin350,
                        test_entry);
    qtest_add_func("/mips64/sn1/restart-le/reset-vector", test_reset_vector);
    qtest_add_func("/mips64/sn1/restart-le/node-restart", test_node_restart);
    qtest_add_func("/mips64/sn1/restart-le/refusals", test_refusals);
    return g_test_run();
}
