/*
 * Placement of the SGI PCI Digital Audio Board (RAD board 13) on the
 * machines that carry it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The card is an optional PCI board, not a fixed part of any machine, so a
 * machine only has to accept it on the bus its option slots sit on. Two
 * machines do, each placement taken from a datasheet's slot record:
 *
 * - Fuel: XBridge head E (widget 14, the 66 MHz option bus), configuration
 *   device 2, where a real option card was observed.
 * - Tezro: head 1 of the IO9's PIC (widget 15), the option bus of the
 *   tower; device 1 is a free configuration device there.
 *
 * The O2 is not covered: its model refuses every attached PCI device, and
 * which cards it takes is a machine decision this test does not make.
 *
 * The register contract is tested on a Bridge head in sgi-rad1-pci-test.c.
 * Here the check is that each bus takes the card and reports itself as its
 * parent.
 *
 * Fuel and the Tezro need a boot PROM image to start; these tests run no
 * guest code, so a zero-filled image of the catalogue size serves.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

static char *zero_file(const char *tmpl, size_t size)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(size);
    char *path = NULL;
    int fd = g_file_open_tmp(tmpl, &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, size), ==, (ssize_t)size);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QDict *qom_get(QTestState *qts, const char *property)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/peripheral/rad','property':%s}}", property);

    g_assert_nonnull(rsp);
    g_assert_false(qdict_haskey(rsp, "error"));
    return rsp;
}

/*
 * The card is realized on @bus at slot @slot function 0 and reports that bus
 * as its parent. The device number is the PCI devfn, slot << 3.
 */
static void check_placed(QTestState *qts, const char *bus, unsigned slot)
{
    g_autoptr(QDict) parent = qom_get(qts, "parent_bus");
    g_autoptr(QDict) where = qom_get(qts, "addr");
    g_autoptr(QDict) type = qom_get(qts, "type");

    g_assert_cmpstr(qdict_get_str(type, "return"), ==, "sgi-rad1-pci");
    g_assert_cmpint(qdict_get_int(where, "return"), ==, slot << 3);
    g_assert_nonnull(strstr(qdict_get_str(parent, "return"), bus));
}

static void test_fuel_head_e(void)
{
    g_autofree char *prom = zero_file("sn1-rad1-fuel-XXXXXX.bin", PROM_SIZE);
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel -cpu R14000 -smp 1 -m 512M "
                                  "-bios %s -S -device sgi-rad1-pci,id=rad,"
                                  "bus=head-e.0,addr=2", quoted);

    check_placed(qts, "head-e.0", 2);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

/* The head F bus is the on-board one; the card still takes a free device. */
static void test_fuel_head_f(void)
{
    g_autofree char *prom = zero_file("sn1-rad1-fuel-XXXXXX.bin", PROM_SIZE);
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *qts = qtest_initf("-M fuel -cpu R14000 -smp 1 -m 512M "
                                  "-bios %s -S -device sgi-rad1-pci,id=rad,"
                                  "bus=head-f.0,addr=2", quoted);

    check_placed(qts, "head-f.0", 2);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
}

static void test_tezro_option_bus(void)
{
    g_autofree char *prom = zero_file("sn1-rad1-tezro-XXXXXX.bin", PROM_SIZE);
    g_autofree char *data = zero_file("sn1-rad1-tezro-data-XXXXXX.bin", 0x8000);
    g_autofree char *clock = zero_file("sn1-rad1-tezro-clock-XXXXXX.bin", 16);
    g_autofree char *qprom = g_shell_quote(prom);
    g_autofree char *qdata = g_shell_quote(data);
    g_autofree char *qclock = g_shell_quote(clock);
    QTestState *qts = qtest_initf(
        "-M tezro,topology=tezro-ip53,nvram0=sgi-ds1742-data-io9,"
        "nvram0-clock=sgi-ds1742-clock-io9 "
        "-drive if=none,id=sgi-ds1742-data-io9,file=%s,format=raw "
        "-drive if=none,id=sgi-ds1742-clock-io9,file=%s,format=raw "
        "-cpu R16000,reset-mode=0x009bad85 -smp 4 -m 4G -bios %s -S "
        "-device sgi-rad1-pci,id=rad,bus=pic-pic-xio-f-head-1.0,addr=1",
        qdata, qclock, qprom);

    check_placed(qts, "pic-pic-xio-f-head-1.0", 1);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom), ==, 0);
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sn1-rad1-pci/fuel-head-e", test_fuel_head_e);
    qtest_add_func("/sn1-rad1-pci/fuel-head-f", test_fuel_head_f);
    qtest_add_func("/sn1-rad1-pci/tezro-option-bus", test_tezro_option_bus);

    return g_test_run();
}
