/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-ip29-io and sgi-baseio resolve their "chardev-a" and "nvram-drive"
 * properties against the command line's own chardev and drive backends a
 * second time, inside build_widget(): the machine builder hands each
 * board an id, not a claimed backend (hw/mips/sgi/board-ip29-io.c's
 * header comment explains why). An id that names no real backend must
 * be refused at realize, naming the id, rather than leaving the IOC3
 * silently built without it. sgi-board-test-fixture stands in for the
 * machine builder here, the same way tests/qtest/sgi-board-test.c uses
 * it for the widget-identity and PCI-population contracts.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

static void assert_backend_id_rejected(const char *board_type,
                                       const char *property,
                                       const char *unresolved_id)
{
    g_autofree char *device = g_strdup_printf(
        "sgi-board-test-fixture,board-type=%s,module=0,slot=8,%s=%s",
        board_type, property, unresolved_id);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-display", "none",
        "-device", device, NULL
    };

    assert_qemu_rejects(args, unresolved_id);
}

static void test_ip29_io_unresolved_chardev(const void *data)
{
    assert_backend_id_rejected("sgi-ip29-io", "chardev-a", "no-such-chardev");
}

static void test_ip29_io_unresolved_drive(const void *data)
{
    assert_backend_id_rejected("sgi-ip29-io", "nvram-drive", "no-such-drive");
}

static void test_baseio_unresolved_chardev(const void *data)
{
    assert_backend_id_rejected("sgi-baseio", "chardev-a", "no-such-chardev");
}

static void test_baseio_unresolved_drive(const void *data)
{
    assert_backend_id_rejected("sgi-baseio", "nvram-drive", "no-such-drive");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sgi-board/ip29-io/unresolved-chardev-refused", NULL,
                        test_ip29_io_unresolved_chardev);
    qtest_add_data_func("/sgi-board/ip29-io/unresolved-drive-refused", NULL,
                        test_ip29_io_unresolved_drive);
    qtest_add_data_func("/sgi-board/baseio/unresolved-chardev-refused", NULL,
                        test_baseio_unresolved_chardev);
    qtest_add_data_func("/sgi-board/baseio/unresolved-drive-refused", NULL,
                        test_baseio_unresolved_drive);

    return g_test_run();
}
