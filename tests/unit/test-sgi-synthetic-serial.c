/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Marked synthetic serials. The vectors are literals, the same ones
 * scripts/tests/test_sgi_synthetic_serial.py checks against the catalogue
 * generator's copy of the scheme, so the two cannot drift apart.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"

static void test_onewire(void)
{
    /* 'Q' 'E' 'M' 'U' 'N' 0 and 'Q' 'E' 'M' 'U' 'C' 3. */
    g_assert_cmphex(sgi_synthetic_onewire_serial('N', 0), ==,
                    UINT64_C(0x51454d554e00));
    g_assert_cmphex(sgi_synthetic_onewire_serial('C', 3), ==,
                    UINT64_C(0x51454d554303));
    g_assert_cmphex(sgi_synthetic_onewire_serial('R', 255), ==,
                    UINT64_C(0x51454d5552ff));
    g_assert_true(sgi_synthetic_onewire_serial_marked(
                      UINT64_C(0x51454d554e00)));
    g_assert_cmphex(SGI_SYNTHETIC_ONEWIRE_SERIAL('B', 1), ==,
                    UINT64_C(0x51454d554201));
}

static void test_uniqueware(void)
{
    g_assert_cmphex(sgi_synthetic_uniqueware_serial(0x5e7, 1), ==,
                    UINT64_C(0x5e7454d55001));
    g_assert_true(sgi_synthetic_onewire_serial_marked(
                      UINT64_C(0x5e7454d55001)));
    g_assert_cmphex(SGI_SYNTHETIC_UNIQUEWARE_SERIAL(0x5e7, 2), ==,
                    UINT64_C(0x5e7454d55002));
}

static void test_unmarked(void)
{
    /* The placeholder serials the scheme replaced carry no marker. */
    g_assert_false(sgi_synthetic_onewire_serial_marked(
                       UINT64_C(0x060504030201)));
    g_assert_false(sgi_synthetic_onewire_serial_marked(
                       UINT64_C(0x5e7000000001)));
    g_assert_false(sgi_synthetic_onewire_serial_marked(
                       UINT64_C(0x304e53494753)));
}

static void test_text(void)
{
    char text[16];

    sgi_synthetic_text_serial(text, 6, 'N', 0);
    g_assert_cmpstr(text, ==, "EMUN00");
    sgi_synthetic_text_serial(text, 6, 'I', 12);
    g_assert_cmpstr(text, ==, "EMUI12");
    sgi_synthetic_text_serial(text, 10, 'M', 1);
    g_assert_cmpstr(text, ==, "EMUM000001");
}

static void test_text_from_onewire(void)
{
    char text[16] = "untouched";

    g_assert_true(sgi_synthetic_text_from_onewire(
                      text, 10, UINT64_C(0x51454d554e02)));
    g_assert_cmpstr(text, ==, "EMUN000002");
    g_assert_true(sgi_synthetic_text_from_onewire(
                      text, 10, UINT64_C(0x51454d5552ff)));
    g_assert_cmpstr(text, ==, "EMUR000255");

    /* A raw override keeps its own text. */
    strcpy(text, "untouched");
    g_assert_false(sgi_synthetic_text_from_onewire(
                       text, 10, UINT64_C(0x0c0b0a090807)));
    g_assert_cmpstr(text, ==, "untouched");
}

static void test_barcode(void)
{
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

    sgi_synthetic_barcode(barcode, 'N', UINT64_C(0x51454d554203));
    g_assert_cmpstr(barcode, ==, "EMUN03");
    sgi_synthetic_barcode(barcode, 'O', UINT64_C(0x51454d554241));
    g_assert_cmpstr(barcode, ==, "EMUO65");
    sgi_synthetic_barcode(barcode, 'P', UINT64_C(0x5e7454d55064));
    g_assert_cmpstr(barcode, ==, "EMUP100");
    sgi_synthetic_barcode(barcode, 'I', UINT64_C(0x060504030201));
    g_assert_cmpstr(barcode, ==, "EMUI00");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-synthetic-serial/onewire", test_onewire);
    g_test_add_func("/sgi-synthetic-serial/uniqueware", test_uniqueware);
    g_test_add_func("/sgi-synthetic-serial/unmarked", test_unmarked);
    g_test_add_func("/sgi-synthetic-serial/text", test_text);
    g_test_add_func("/sgi-synthetic-serial/text-from-onewire",
                    test_text_from_onewire);
    g_test_add_func("/sgi-synthetic-serial/barcode", test_barcode);
    return g_test_run();
}
