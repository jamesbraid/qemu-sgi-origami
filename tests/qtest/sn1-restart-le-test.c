/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator-only little-endian restart of an Origin 300's CPUs. A stub in
 * each byte order stores the word 0x11223344, so raw RAM shows the order.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-test.h"

#define IP53_PROM_SIZE  (16 * 1024 * 1024)
#define MAILBOX_PHYS    0x200c00u
#define MAILBOX_KSEG_HI 0x8020
#define LE_RAM_ENTRY    0x300000u
#define BE_ENTRY        0x4000
#define LE_FLASH_ENTRY  0x400
#define LE_DISPATCH     0x8444
/* The configuration block's signature and mode word, in flash. */
#define CONFIG_MODE     0x64
#define CONFIG_MAGIC    0x68
#define MODE_WORD_LE    0x549a2985u

#define BEDROCK_LOCAL   (SPACE_IO + UINT64_C(0x01000000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
#define NI_PORT_RESET   UINT64_C(0x680008)
#define PI_PENDING_MODIFY UINT64_C(0x90)
#define PI_MASK_0_A     UINT64_C(0xa8)
#define PI_MASK_0_B     UINT64_C(0xb8)
#define PI_NMI_A        UINT64_C(0x70)
#define PI_NMI_B        UINT64_C(0x78)
#define LB_SOFT_RESET   UINT64_C(0x604018)
#define PI1             UINT64_C(0x200000)
#define CAUSE_IP_MASK   0xff00u
#define PORT_RESET_LOCAL 4
/* The installed banks: 0 and 1, 256 MiB each, and the IP53 population. */
#define MD_ORIGIN300    UINT64_C(0x33)
#define MD_IP53         UINT64_C(0x53)

#include "sgi-restart-le.h"

#define R_T3            11
#define R_T4            12

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

/* Word 0 branches to a different stub in each byte order. */
static void build_image(uint8_t *img, const Machine *m, bool le_in_flash,
                        uint32_t mode_word)
{
    Code c = { .b = img, .pos = BE_ENTRY, .be = true,
               .prologue = emit_configure_memory, .md_config = m->md_config };

    memset(img, 0, m->prom_size);
    if (mode_word) {
        stl_be_p(img + CONFIG_MODE, mode_word);
        stq_be_p(img + CONFIG_MAGIC, 0x69703237636f6e66ULL);
    }
    stl_be_p(img, 0x10210010);
    stl_be_p(img + 0x44, 0x10000000 | (uint16_t)((BE_ENTRY - 0x44 - 4) / 4));
    stl_le_p(img + LE_DISPATCH, 0x10000000 |
             (uint16_t)((LE_FLASH_ENTRY - LE_DISPATCH - 4) / 4));
    emit_stub(&c, BE_BASE, BE_DONE, STUB_CAUSE);
    if (le_in_flash) {
        c.pos = LE_FLASH_ENTRY;
        c.be = false;
        emit_stub(&c, LE_BASE, LE_DONE, STUB_CAUSE | STUB_COUNT);
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
static QTestState *start_word(const Machine *m, bool le_in_flash,
                              uint32_t mode_word)
{
    g_autofree uint8_t *img = g_malloc(m->prom_size);
    g_autofree char *bios = NULL;
    g_autofree char *stores = NULL;
    g_autofree char *clock = NULL;
    g_autofree char *drives = NULL;
    QTestState *qts;

    build_image(img, m, le_in_flash, mode_word);
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

static QTestState *start(const Machine *m, bool le_in_flash)
{
    return start_word(m, le_in_flash, 0);
}

static void cont(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
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
    Code c = { .b = stub };

    emit_stub(&c, LE_BASE, LE_DONE, STUB_CAUSE | STUB_COUNT);
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

    restart(qts, LE_RAM_ENTRY);
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
    restart(qts, 0);
    check_little_endian(qts, 2);
    reset_to_big(qts, true);
    restart(qts, 0);
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
    restart(qts, 0);
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

/*
 * An NMI keeps the order. The local block's soft reset restarts the node as a
 * local reset does, so it loads the word again: an approximation.
 */
static void test_nmi_and_soft_reset(void)
{
    QTestState *qts = start(&origin300, true);

    cont(qts);
    check_big_endian(qts);
    restart(qts, 0);
    check_little_endian(qts, 2);

    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    for (uint64_t pi = 0; pi <= PI1; pi += PI1) {
        qtest_writeq(qts, BEDROCK_LOCAL + pi + PI_NMI_A, 1);
        qtest_writeq(qts, BEDROCK_LOCAL + pi + PI_NMI_B, 1);
    }
    check_little_endian(qts, 2);
    g_assert_true(order_is(qts, "little"));

    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    qtest_writeq(qts, BEDROCK_LOCAL + LB_SOFT_RESET, 1);
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
    qtest_quit(qts);
}

/* An IP53 flash keeps its block: bit 15 clear runs every CPU little-endian. */
static void test_flash_word_ip53(void)
{
    QTestState *qts = start_word(&origin350, true, MODE_WORD_LE);

    cont(qts);
    check_order(qts, false, 0);
    g_assert_true(order_is(qts, "little"));
    for (unsigned int cpu = 0; cpu < origin350.cpus; cpu++) {
        g_assert_false(cpu_is_big(qts, cpu));
    }

    /* A little-endian CPU cannot set the banks, and a reset clears them. */
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_writeq(qts, BEDROCK_LOCAL + MD_MEMORY_CONFIG, origin350.md_config);
    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    cont(qts);
    check_order(qts, false, 0);
    for (unsigned int cpu = 0; cpu < origin350.cpus; cpu++) {
        g_assert_false(cpu_is_big(qts, cpu));
    }
    qtest_quit(qts);
}

/* Other IP35 boards get the machine's mode word over the image's at start. */
static void test_flash_word_overwritten(void)
{
    QTestState *qts = start_word(&origin300, true, MODE_WORD_LE);

    cont(qts);
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
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
    qtest_add_func("/mips64/sn1/restart-le/nmi-and-soft-reset",
                   test_nmi_and_soft_reset);
    qtest_add_func("/mips64/sn1/restart-le/flash-word-ip53",
                   test_flash_word_ip53);
    qtest_add_func("/mips64/sn1/restart-le/flash-word-overwritten",
                   test_flash_word_overwritten);
    qtest_add_func("/mips64/sn1/restart-le/refusals", test_refusals);
    return g_test_run();
}
