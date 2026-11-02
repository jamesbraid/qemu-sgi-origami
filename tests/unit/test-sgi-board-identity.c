/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The default station address and text serial. The vector is a literal:
 * sha256("/x/disk.img") =
 * ee2063bd89eff3352f9179b8ae3fba6c422be9281cf7644585a05636ae7a0c4b.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/sgi-board-identity.h"

static void test_worked_vector(void)
{
    SGIBoardIdentity id;
    char text[SGI_BOARD_TEXT_SERIAL_SIZE];
    static const uint8_t expected[6] = { 0x08, 0x00, 0x69, 0xee, 0x20, 0x63 };

    sgi_board_identity_from_key("/x/disk.img", &id);
    g_assert_cmpmem(id.mac, 6, expected, 6);
    sgi_board_text_serial(&id, 0, text);
    g_assert_cmpstr(text, ==, "EMUNEF");
    /* Node n adds n to the byte, wrapping at 256. */
    sgi_board_text_serial(&id, 1, text);
    g_assert_cmpstr(text, ==, "EMUNF0");
    sgi_board_text_serial(&id, 0x11, text);
    g_assert_cmpstr(text, ==, "EMUN00");
}

/* The "E" nibble marker and "EMUN" survive whatever the key hashes to. */
static void test_markers_hold_for_any_key(void)
{
    const char *keys[] = { "", "octane", "octane2", "fuel", "/a", "/b/c.img",
                           "origin200", "x" };

    for (size_t i = 0; i < G_N_ELEMENTS(keys); i++) {
        SGIBoardIdentity id;
        char text[SGI_BOARD_TEXT_SERIAL_SIZE];

        sgi_board_identity_from_key(keys[i], &id);
        g_assert_cmphex(id.mac[0], ==, 0x08);
        g_assert_cmphex(id.mac[1], ==, 0x00);
        g_assert_cmphex(id.mac[2], ==, 0x69);
        g_assert_cmphex(id.mac[3] & 0xf0, ==, 0xe0);
        sgi_board_text_serial(&id, 0, text);
        g_assert_true(g_str_has_prefix(text, "EMUN"));
        g_assert_cmpuint(strlen(text), ==, 6);
        g_assert_true(strspn(text + 4, "0123456789ABCDEF") == 2);
    }
}

static void test_keys_differ(void)
{
    SGIBoardIdentity a, b;

    sgi_board_identity_from_key("/x/disk.img", &a);
    sgi_board_identity_from_key("/x/disk2.img", &b);
    g_assert_cmpint(memcmp(a.mac, b.mac, 6), !=, 0);
    g_assert_cmpint(a.text_byte, !=, b.text_byte);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-board-identity/worked-vector", test_worked_vector);
    g_test_add_func("/sgi-board-identity/markers",
                    test_markers_hold_for_any_key);
    g_test_add_func("/sgi-board-identity/keys-differ", test_keys_differ);
    return g_test_run();
}
