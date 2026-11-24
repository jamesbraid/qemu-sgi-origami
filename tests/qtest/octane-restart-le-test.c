/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator-only little-endian restart of an Octane's CPU, into RAM the
 * HEART bank wrap (x-heart-bank-wrap) places at 0. A stub in each byte order
 * stores the word 0x11223344, so raw RAM shows the order.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define IMAGE_SIZE      0x10000
#define HEART           0x0ff00000ULL
#define MAILBOX_PHYS    0xc00u
#define MAILBOX_KSEG_HI 0xa000
#define LE_RAM_ENTRY    0x600000u
#define BE_ENTRY        0x400
#define LE_FLASH_ENTRY  0x800
#define LE_DISPATCH     0x8444
#define BANK_128M_WRAP  (0x80030000u | 0x1f0)
#define HEART_IP2       (1ULL << 3)
#define CAUSE_IP2       0x400u

#include "sgi-restart-le.h"

/*
 * The reset vector must run under either byte order. Word 0 reads as
 * "beq $1,$1,+0x10" big-endian and "beq $0,$0,+0x2110" little-endian, so
 * each order branches to its own stub and no wrong-order instruction runs.
 */
static void build_image(uint8_t *img)
{
    Code c = { .b = img, .pos = BE_ENTRY, .be = true };

    memset(img, 0, IMAGE_SIZE);
    stl_be_p(img, 0x10210010);
    stl_be_p(img + 0x44, 0x10000000 | (uint16_t)((BE_ENTRY - 0x44 - 4) / 4));
    stl_le_p(img + LE_DISPATCH, 0x10000000 |
             (uint16_t)((LE_FLASH_ENTRY - LE_DISPATCH - 4) / 4));
    emit_stub(&c, BE_BASE, BE_DONE, STUB_CAUSE);
    c.pos = LE_FLASH_ENTRY;
    c.be = false;
    emit_stub(&c, LE_BASE, LE_DONE, STUB_CAUSE);
}

static QTestState *start(const char *machine_args, const char *extra)
{
    g_autofree uint8_t *img = g_malloc(IMAGE_SIZE);
    g_autofree char *bios = NULL;
    QTestState *qts;
    int fd = g_file_open_tmp("octane-restart-le-XXXXXX", &bios, NULL);

    g_assert_cmpint(fd, >=, 0);
    build_image(img);
    g_assert_cmpint(write(fd, img, IMAGE_SIZE), ==, IMAGE_SIZE);
    close(fd);
    qts = qtest_initf("-M octane%s -m 128M -cpu R10000 -bios %s -accel tcg %s",
                      machine_args, bios, extra);
    unlink(bios);
    return qts;
}

static void put_le_stub(QTestState *qts)
{
    uint8_t stub[0x100] = { };
    Code c = { .b = stub };

    emit_stub(&c, LE_BASE, LE_DONE, STUB_CAUSE);
    qtest_memwrite(qts, LE_RAM_ENTRY, stub, c.pos);
}

/* A machine reset brings the CPU back big-endian and runs the flash again. */
static void reset_to_big(QTestState *qts)
{
    qtest_memset(qts, MAILBOX_PHYS, 0, 0x40);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    check_order(qts, true, 0);
    g_assert_true(order_is(qts, "big"));
}

/*
 * The guest runs big-endian until the harness restarts it at the entry, in
 * RAM the bank wrap put at 0. HEART's held interrupt reaches the restarted
 * CPU, as the reset had dropped it.
 */
static void test_entry(void)
{
    QTestState *qts = start(",x-heart-bank-wrap=on", "");

    check_order(qts, true, 0);
    g_assert_true(order_is(qts, "big"));
    qtest_writel(qts, HEART + 0x20, BANK_128M_WRAP);
    put_le_stub(qts);
    qtest_writeq(qts, HEART + 0x10000, HEART_IP2);
    qtest_writeq(qts, HEART + 0x10020, HEART_IP2);

    restart(qts, LE_RAM_ENTRY);
    check_order(qts, false, 0);
    g_assert_true(order_is(qts, "little"));
    g_assert_cmphex(mailbox(qts, LE_BASE + W_CAUSE, false) & CAUSE_IP2, ==,
                    CAUSE_IP2);
    reset_to_big(qts);
    qtest_quit(qts);
}

/* With no entry the CPU takes the reset vector, in either order. */
static void test_reset_vector(void)
{
    QTestState *qts = start("", "");

    check_order(qts, true, 0);
    restart(qts, 0);
    check_order(qts, false, 0);
    reset_to_big(qts);
    restart(qts, 0);
    check_order(qts, false, 0);
    qtest_quit(qts);
}

/*
 * A snapshot saved little-endian at the reset vector loads into a process
 * that has since run big-endian there, and a restarted machine saves.
 */
static void test_snapshot_load(void)
{
    g_autofree char *dir = g_dir_make_tmp("octane-restart-le-XXXXXX", NULL);
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
    qts = start("", drive);
    set_order(qts, "little");
    sgi_wait_start(&w, "restart while stopped");
    while (!order_is(qts, "little") && sgi_wait_continue(&w, 5000)) {
        /* poll */
    }
    out = qtest_hmp(qts, "savevm le");
    g_assert_cmpstr(out, ==, "");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    check_order(qts, false, 0);
    reset_to_big(qts);

    g_free(qtest_hmp(qts, "loadvm le"));
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    check_order(qts, false, 0);
    qtest_quit(qts);
    unlink(qcow2);
    rmdir(dir);
}

static void test_refusals(void)
{
    QTestState *qts = start("", "");
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
    qtest_add_func("/mips64/octane/restart-le/entry", test_entry);
    qtest_add_func("/mips64/octane/restart-le/reset-vector",
                   test_reset_vector);
    qtest_add_func("/mips64/octane/restart-le/snapshot-load",
                   test_snapshot_load);
    qtest_add_func("/mips64/octane/restart-le/refusals", test_refusals);
    return g_test_run();
}
