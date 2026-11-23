/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator-only little-endian restart of an Origin 200's CPUs. A stub
 * in each byte order stores the word 0x11223344, so raw RAM shows the order.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-mips-emit.h"
#include "sgi-test.h"
#include "sgi-wait.h"

#define IMAGE_SIZE      0x10000
#define MAILBOX_PHYS    0x200c00u
#define LE_RAM_ENTRY    0x300000u
#define PI_CPU_ENABLE_B 0x58
#define NI_PORT_RESET   0x600008
#define BE_ENTRY        0x4000
#define LE_FLASH_ENTRY  0x400
#define LE_DISPATCH     0x8444

#define R_ZERO          0
#define R_T0            8
#define R_T1            9
#define R_T2            10
#define R_S6            22
#define OP_LL           0x30
#define OP_SC           0x38

/* Each stub owns a block of mailbox words in RAM at MAILBOX_PHYS. */
#define BE_BASE         0x00
#define LE_BASE         0x20
#define W_CONFIG0       0x00
#define W_STORED        0x04
#define W_LOADED        0x08
#define W_MARKER        0x0c
#define W_COUNT         0x10

#define BE_DONE         0xbe5b1001u
#define LE_DONE         0x1e5b2002u
#define CONFIG0_BE      (1u << 15)
#define WORD            0x11223344u

typedef struct Code {
    uint8_t *b;
    unsigned int pos;
    bool be;
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

/*
 * Record Config0 and the word stored then loaded back, optionally add one
 * to a shared count with ll/sc, then set a marker and spin.
 */
static void emit_stub(Code *c, unsigned int base, uint32_t done, bool count)
{
    unsigned int retry;

    emit(c, mips_lui(R_S6, 0x8020));
    emit(c, mips_mfc0(R_T0, 16));
    emit(c, mips_sw(R_T0, R_S6, 0xc00 + base + W_CONFIG0));
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

/*
 * The reset vector must run under either byte order. Word 0 reads as
 * "beq $1,$1,+0x10" big-endian and "beq $0,$0,+0x2110" little-endian, so
 * each order branches to its own stub and no wrong-order instruction runs.
 */
static void build_image(uint8_t *img, bool le_in_flash)
{
    Code c = { .b = img, .pos = BE_ENTRY, .be = true };

    memset(img, 0, IMAGE_SIZE);
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

static QTestState *start(unsigned int smp, bool le_in_flash, const char *extra)
{
    g_autofree uint8_t *img = g_malloc(IMAGE_SIZE);
    g_autofree char *bios = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("sn0-restart-le-XXXXXX", &bios, NULL);

    g_assert_cmpint(fd, >=, 0);
    build_image(img, le_in_flash);
    g_assert_cmpint(write(fd, img, IMAGE_SIZE), ==, IMAGE_SIZE);
    close(fd);
    qts = qtest_initf("-M origin200 -m 128M -smp %u -bios %s -accel tcg %s",
                      smp, bios, extra);
    unlink(bios);
    return qts;
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
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
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

#define check_big_endian(qts)       check_order(qts, true, 0)
#define check_little_endian(qts, n) check_order(qts, false, n)

static void put_le_stub(QTestState *qts)
{
    uint8_t stub[0x100] = { };
    Code c = { .b = stub };

    emit_stub(&c, LE_BASE, LE_DONE, true);
    qtest_memwrite(qts, LE_RAM_ENTRY, stub, c.pos);
}

/*
 * The guest runs big-endian until the harness restarts it at the entry.
 * A second CPU the Hub has disabled stays halted until it is enabled.
 */
static void test_entry(const void *data)
{
    unsigned int smp = GPOINTER_TO_UINT(data);
    QTestState *qts = start(smp, false, "");

    put_le_stub(qts);
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
    if (smp == 2) {
        qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 0);
    }

    restart(qts, true);
    check_little_endian(qts, smp == 1);
    g_assert_true(order_is(qts, "little"));
    if (smp == 2) {
        /* A running CPU 1 would have counted by now; a halted one cannot. */
        g_usleep(G_USEC_PER_SEC / 5);
        g_assert_cmpuint(mailbox(qts, LE_BASE + W_COUNT, false), ==, 1);
        qtest_writeq(qts, HUB_LOCAL + PI_CPU_ENABLE_B, 1);
        wait_mailbox(qts, LE_BASE + W_COUNT, 2, false);
    }
    qtest_quit(qts);
}

/* Whole-machine resets, by the Hub's port reset or the monitor, undo it. */
static void reset_to_big(QTestState *qts, bool hub)
{
    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    if (hub) {
        qtest_writeq(qts, HUB_LOCAL + NI_PORT_RESET, 0x81);
    } else {
        qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    }
    check_big_endian(qts);
    g_assert_true(order_is(qts, "big"));
}

/* With no entry the CPU takes the reset vector, in either order. */
static void test_reset_vector(void)
{
    QTestState *qts = start(1, true, "");

    check_big_endian(qts);
    restart(qts, false);
    check_little_endian(qts, 1);
    reset_to_big(qts, true);
    restart(qts, false);
    check_little_endian(qts, 1);
    reset_to_big(qts, false);
    qtest_quit(qts);
}

/*
 * A snapshot saved little-endian at the reset vector loads into a process
 * that has since run big-endian there, and a restarted machine saves.
 */
static void test_snapshot_load(void)
{
    g_autofree char *dir = g_dir_make_tmp("sn0-restart-le-XXXXXX", NULL);
    g_autofree char *qcow2 = g_strdup_printf("%s/state.qcow2", dir);
    g_autofree char *drive = g_strdup_printf(
        "-S -drive file=%s,format=qcow2,if=none", qcow2);
    g_autofree char *out = NULL;
    SgiWait w;
    QTestState *qts;

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }
    g_assert(mkimg(qcow2, "qcow2", 128));
    qts = start(1, true, drive);
    set_order(qts, "little");
    sgi_wait_start(&w, "restart while stopped");
    while (!order_is(qts, "little") && sgi_wait_continue(&w, 5000)) {
        /* poll */
    }
    out = qtest_hmp(qts, "savevm le");
    g_assert_cmpstr(out, ==, "");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    check_little_endian(qts, 1);
    reset_to_big(qts, false);

    g_free(qtest_hmp(qts, "loadvm le"));
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    check_little_endian(qts, 1);
    qtest_quit(qts);
}

static void test_refusals(void)
{
    QTestState *qts = start(1, false, "");
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

/* Only the IP27 machines have the option; an Origin 300 refuses it. */
static void test_not_ip27(void)
{
    g_autofree char *image = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("sn0-restart-le-XXXXXX", &image, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, PROM_SIZE), ==, 0);
    close(fd);
    qts = qtest_initf("-M origin300 -cpu R14000 -bios %s -accel tcg", image);
    unlink(image);
    qobject_unref(qtest_qmp_assert_failure_ref(
        qts, "{'execute': 'qom-set', 'arguments': {'path': '/machine', "
        "'property': 'x-cpu-byte-order', 'value': 'little'}}"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("/mips64/sn0/restart-le/entry", GUINT_TO_POINTER(1),
                        test_entry);
    qtest_add_data_func("/mips64/sn0/restart-le/entry-two-cpus",
                        GUINT_TO_POINTER(2), test_entry);
    qtest_add_func("/mips64/sn0/restart-le/reset-vector", test_reset_vector);
    qtest_add_func("/mips64/sn0/restart-le/snapshot-load", test_snapshot_load);
    qtest_add_func("/mips64/sn0/restart-le/refusals", test_refusals);
    qtest_add_func("/mips64/sn0/restart-le/not-ip27", test_not_ip27);
    return g_test_run();
}
