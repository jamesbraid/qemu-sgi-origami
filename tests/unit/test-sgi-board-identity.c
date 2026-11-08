/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The emulated serial scheme: system serial, module serial, default station
 * address, board text serial, clock serial and the host id each one gives
 * IRIX. The vectors are literals worked out by hand from the layouts in
 * sgi-board-identity.h, so a change to the layout shows up here.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/sgi-board-identity.h"

static void test_kind_constant(void)
{
    /* The QEMU machines are kind 1; iris mirrors the scheme with kind 2. */
    g_assert_cmpuint(SGI_BOARD_KIND, ==, 1);
}

static void test_system_serial(void)
{
    char text[SGI_BOARD_SYSTEM_SERIAL_SIZE];

    sgi_board_system_serial(1, 42, text);
    g_assert_cmpstr(text, ==, "LEMU100420");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5100420);

    sgi_board_system_serial(1, 0, text);
    g_assert_cmpstr(text, ==, "LEMU100000");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5100000);

    sgi_board_system_serial(2, 42, text);
    g_assert_cmpstr(text, ==, "LEMU200420");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5200420);

    sgi_board_system_serial(1, 4095, text);
    g_assert_cmpstr(text, ==, "LEMU140950");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5140950);
}

static void test_module_serial(void)
{
    char text[SGI_BOARD_MODULE_SERIAL_SIZE];

    g_assert_true(sgi_board_module_serial(1, 42, 0, text));
    g_assert_cmpstr(text, ==, "KEMU100420");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5100420);
    g_assert_true(sgi_board_module_serial(1, 42, 1, text));
    g_assert_cmpstr(text, ==, "KEMU100421");
    g_assert_cmpuint(sgi_board_hostid_from_text(text), ==, 0xd5100421);
    g_assert_true(sgi_board_module_serial(1, 42, 9, text));
    g_assert_cmpstr(text, ==, "KEMU100429");
    /* The module number is one digit, so module 10 is refused, not 0. */
    g_assert_false(sgi_board_module_serial(1, 42, 10, text));
}

/* IRIX folds only the first ten characters; the first two never show. */
static void test_hostid_fold(void)
{
    g_assert_cmpuint(sgi_board_hostid_from_text("LEMU1042"), ==, 0xc5d51042);
    g_assert_cmpuint(sgi_board_hostid_from_text("EMU1-0042"), ==, 0xd51d0042);
    g_assert_cmpuint(sgi_board_hostid_from_text("EMU1L0000042"), ==,
                     0x51c00000);
    g_assert_cmpuint(sgi_board_hostid_from_text("M2001347"), ==, 0xd2001347);
    g_assert_cmpuint(sgi_board_hostid_from_text("L3962017"), ==, 0xc3962017);
    g_assert_cmpuint(sgi_board_hostid_from_text(""), ==, 0);
}

static void test_default_mac(void)
{
    uint8_t mac[6];
    static const uint8_t first[6] = { 0x08, 0x00, 0x69, 0xe1, 0x02, 0xa0 };

    g_assert_true(sgi_board_default_mac(1, 42, 0, mac));
    g_assert_cmpmem(mac, 6, first, 6);
    g_assert_cmpuint(sgi_board_hostid_from_mac(mac), ==, 0x69e102a0);

    /* Kind 2 changes only the nibble after E. */
    g_assert_true(sgi_board_default_mac(2, 42, 0, mac));
    g_assert_cmpuint(sgi_board_hostid_from_mac(mac), ==, 0x69e202a0);

    /* Serial 0 is the same layout with zeros. */
    g_assert_true(sgi_board_default_mac(1, 0, 0, mac));
    g_assert_cmpuint(sgi_board_hostid_from_mac(mac), ==, 0x69e10000);
}

/*
 * The sub-index fills the low nibble of the last byte and never reaches the
 * kind or the serial, at both ends of the serial range.
 */
static void test_sub_index(void)
{
    static const unsigned serials[] = { 0, 1, 42, 255, 4094, 4095 };

    for (size_t i = 0; i < G_N_ELEMENTS(serials); i++) {
        uint8_t base[6];

        g_assert_true(sgi_board_default_mac(1, serials[i], 0, base));
        g_assert_cmpuint(base[5] & 0xf, ==, 0);
        for (unsigned sub = 0; sub < SGI_BOARD_SUB_INDEX_COUNT; sub++) {
            uint8_t mac[6];

            g_assert_true(sgi_board_default_mac(1, serials[i], sub, mac));
            g_assert_cmpmem(mac, 5, base, 5);
            g_assert_cmpuint(mac[5] >> 4, ==, base[5] >> 4);
            g_assert_cmpuint(mac[5] & 0xf, ==, sub);
            g_assert_cmpuint(mac[3], ==, 0xe1);
            g_assert_cmpuint((mac[4] << 4) | (mac[5] >> 4), ==, serials[i]);
        }
        g_assert_false(sgi_board_default_mac(1, serials[i], 16, base));
    }
}

/*
 * The board serial follows the node and nothing else: the PROM ties NVRAM
 * ownership to it, so it must not change with the machine's serial.
 */
static void test_text_serial(void)
{
    char text[SGI_BOARD_TEXT_SERIAL_SIZE];

    sgi_board_text_serial(0, text);
    g_assert_cmpstr(text, ==, "EMUN00");
    sgi_board_text_serial(3, text);
    g_assert_cmpstr(text, ==, "EMUN03");
    sgi_board_text_serial(0x1b, text);
    g_assert_cmpstr(text, ==, "EMUN1B");
    sgi_board_text_serial(256, text);
    g_assert_cmpstr(text, ==, "EMUN00");
}

static void test_clock_serial(void)
{
    g_assert_cmphex(sgi_board_clock_serial(1, 42), ==, 0x454d5501002aULL);
    g_assert_cmphex(sgi_board_clock_serial(2, 4095), ==, 0x454d55020fffULL);
    g_assert_cmphex(sgi_board_clock_serial(1, 0), ==, 0x454d55010000ULL);
}

static void test_fuel_text(void)
{
    uint32_t id = 0;

    /* Hex digits 4 to 11 of the 12-digit station address text. */
    g_assert_true(sgi_board_hostid_from_hex_text("080069E102A0", &id));
    g_assert_cmphex(id, ==, 0x69e102a0);
    g_assert_false(sgi_board_hostid_from_hex_text("080069E102A", &id));
    g_assert_false(sgi_board_hostid_from_hex_text("080069E102AG", &id));
}

static void test_emu_id_valid(void)
{
    g_assert_true(sgi_board_emu_id_valid(0));
    g_assert_true(sgi_board_emu_id_valid(1));
    g_assert_true(sgi_board_emu_id_valid(4095));
    g_assert_false(sgi_board_emu_id_valid(4096));
}

/* The spelling of an id as a serial text, and the fold back. */
static void test_spelling(void)
{
    static const struct {
        uint32_t id;
        const char *text;
    } cases[] = {
        { 0xd2100887, "M2100887" },
        { 0xd21a0f87, "M21J0O87" },
        { 0x00000001, "P0000001" },
        { 0xffffffff, "OOOOOOOO" },
        { 0xc0000000, "L0000000" },
        { 0x69e102a0, "F9N102J0" },
        { 0xd5100420, "M5100420" },
        { 0, "P0000000" },
    };
    char text[9];

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        sgi_board_text_from_hostid(cases[i].id, text);
        g_assert_cmpstr(text, ==, cases[i].text);
        g_assert_cmphex(sgi_board_hostid_from_text(text), ==, cases[i].id);
    }
    /* The id round trip holds for every high nibble and a spread of ids. */
    for (uint64_t id = 0; id < 0x100000000ULL; id += 0x01234567) {
        sgi_board_text_from_hostid(id, text);
        g_assert_cmphex(sgi_board_hostid_from_text(text), ==, id);
        g_assert_true(g_ascii_isalpha(text[0]));
    }
}

static void test_hostid_parse(void)
{
    uint32_t id = 0;

    g_assert_true(sgi_board_hostid_parse("d2100887", &id));
    g_assert_cmphex(id, ==, 0xd2100887);
    g_assert_true(sgi_board_hostid_parse("00000001", &id));
    g_assert_cmphex(id, ==, 1);
    g_assert_false(sgi_board_hostid_parse("d210088", &id));
    g_assert_false(sgi_board_hostid_parse("d21008877", &id));
    g_assert_false(sgi_board_hostid_parse("d210088g", &id));
    g_assert_false(sgi_board_hostid_parse("", &id));
}

static bool resolve(SGIBoardFamily family, unsigned int emu_id,
                    const char *serial, const char *hostid,
                    const uint8_t *mac, SGIBoardId *out, char *error)
{
    return sgi_board_id_resolve(family, SGI_BOARD_KIND, emu_id, serial,
                                hostid, mac, out, error);
}

static void test_resolve_text(void)
{
    SGIBoardId id;
    char error[SGI_BOARD_ERROR_SIZE];

    /* Nothing given: emu-id 0, the generic layout. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, NULL, NULL, &id,
                          error));
    g_assert_cmpstr(id.text, ==, "LEMU100000");
    g_assert_cmphex(id.id, ==, 0xd5100000);
    g_assert_false(id.user);
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 42, "", "", NULL, &id,
                          error));
    g_assert_cmpstr(id.text, ==, "LEMU100420");
    g_assert_cmphex(id.id, ==, 0xd5100420);

    /* serial, any text. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 42, "M2100887", NULL, NULL,
                          &id, error));
    g_assert_cmpstr(id.text, ==, "M2100887");
    g_assert_cmphex(id.id, ==, 0xd2100887);
    g_assert_true(id.user);
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, "LEMU100420", NULL, NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0xd5100420);

    /* hostid, spelled. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "d2100887", NULL,
                          &id, error));
    g_assert_cmpstr(id.text, ==, "M2100887");
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "00000001", NULL,
                          &id, error));
    g_assert_cmpstr(id.text, ==, "P0000001");
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "ffffffff", NULL,
                          &id, error));
    g_assert_cmpstr(id.text, ==, "OOOOOOOO");
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "c0000000", NULL,
                          &id, error));
    g_assert_cmpstr(id.text, ==, "L0000000");
    /* Any id, including one that starts 69, on a text family. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "69e102a0", NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x69e102a0);
    g_assert_cmphex(sgi_board_hostid_from_text(id.text), ==, 0x69e102a0);

    /* The two spellings agree or the machine refuses. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, "M2100887", "d2100887",
                          NULL, &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, "M2100887", "d2100888",
                           NULL, &id, error));
    g_assert_nonnull(strstr(error, "M2100887"));
    g_assert_nonnull(strstr(error, "d2100888"));
    g_assert_nonnull(strstr(error, "serial text"));

    /* Bounds. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 4096, NULL, NULL, NULL, &id,
                           error));
    g_assert_cmpstr(error, ==, "emu-id must be 0 to 4095, not 4096");
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, "M210088700", NULL, NULL,
                          &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, "M2100887000", NULL, NULL,
                           &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, "xyz", NULL, &id,
                           error));

    /* A serial is 1 to 10 printable characters, none of ':' or ';'. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, "AB:CD", NULL, NULL, &id,
                           error));
    g_assert_cmpstr(error, ==, "serial 'AB:CD' must be 1 to 10 printable "
                    "characters, none of ':' or ';'");
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, "AB;CD", NULL, NULL, &id,
                           error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_TEXT, 0, "AB\tCD", NULL, NULL,
                           &id, error));
    g_assert_true(sgi_board_serial_valid("A"));
    g_assert_true(sgi_board_serial_valid("AB CD"));
    g_assert_false(sgi_board_serial_valid(""));
    g_assert_false(sgi_board_serial_valid("M2100887000"));
}

/*
 * The module families: the guest reads a module serial only when it starts
 * with K, the text is ten characters, and a short serial is padded on the left
 * with K.
 */
static void test_resolve_module(void)
{
    SGIBoardId id;
    char error[SGI_BOARD_ERROR_SIZE], text[SGI_BOARD_MODULE_SERIAL_SIZE];

    /* The emulator layout counts the module digit. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 42, NULL, NULL, NULL, &id,
                          error));
    g_assert_cmphex(id.id, ==, 0xd5100420);
    g_assert_true(sgi_board_id_module_text(&id, 1, text));
    g_assert_cmpstr(text, ==, "KEMU100421");
    g_assert_false(sgi_board_id_module_text(&id, 10, text));

    /* A serial must start with K. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_MODULE, 0, "M2100887", NULL, NULL,
                           &id, error));
    g_assert_cmpstr(error, ==, "serial 'M2100887' must start with K on this "
                    "machine: the guest ignores a module serial that does "
                    "not");
    g_assert_false(resolve(SGI_BOARD_FAMILY_MODULE, 0, "LEMU100420", NULL,
                           NULL, &id, error));

    /* A short serial is padded on the left with K and folds as padded. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, "K0017299", NULL, NULL,
                          &id, error));
    g_assert_true(sgi_board_id_module_text(&id, 0, text));
    g_assert_cmpstr(text, ==, "KKK0017299");
    g_assert_cmphex(sgi_board_hostid_from_text(text), ==, 0xb0017299);
    g_assert_cmphex(id.id, ==, 0xb0017299);
    /* A ten-character one is used as is. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, "K210088799", NULL, NULL,
                          &id, error));
    g_assert_true(sgi_board_id_module_text(&id, 0, text));
    g_assert_cmpstr(text, ==, "K210088799");
    g_assert_cmphex(id.id, ==, 0x10088799);
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, "KEMU100887", NULL, NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0xd5100887);
    /* The id is the fold of the padded text even for a very short one. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, "K12", NULL, NULL, &id,
                          error));
    g_assert_true(sgi_board_id_module_text(&id, 0, text));
    g_assert_cmpstr(text, ==, "KKKKKKKK12");
    g_assert_cmphex(id.id, ==, sgi_board_hostid_from_text(text));

    /* hostid: "KK" and the spelling, so any id is reachable. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, NULL, "d2100887", NULL,
                          &id, error));
    g_assert_true(sgi_board_id_module_text(&id, 0, text));
    g_assert_cmpstr(text, ==, "KKM2100887");
    g_assert_cmphex(sgi_board_hostid_from_text(text), ==, 0xd2100887);
    g_assert_true(sgi_board_id_module_text(&id, 1, text));
    g_assert_cmpstr(text, ==, "KKM2100888");
    g_assert_cmphex(sgi_board_hostid_from_text(text), ==, 0xd2100888);
    g_assert_true(sgi_board_id_module_text(&id, 9, text));
    g_assert_cmphex(sgi_board_hostid_from_text(text), ==, 0xd2100890);
    g_assert_false(sgi_board_id_module_text(&id, 10, text));
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, NULL, "ffffffff", NULL,
                          &id, error));
    g_assert_true(sgi_board_id_module_text(&id, 0, text));
    g_assert_cmpstr(text, ==, "KKOOOOOOOO");
    /* The wrap past ffffffff stays a valid spelling. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, NULL, "fffffffe", NULL,
                          &id, error));
    g_assert_true(sgi_board_id_module_text(&id, 3, text));
    g_assert_cmphex(sgi_board_hostid_from_text(text), ==, 0x00000001);

    /* serial and hostid must agree, as the padded text folds. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, "K0017299", "b0017299",
                          NULL, &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_MODULE, 0, "K0017299", "b0017298",
                           NULL, &id, error));
    g_assert_nonnull(strstr(error, "serial K0017299 folds to b0017299"));
    /* The id round trip: hostid -> text -> id. */
    for (uint64_t value = 0; value < 0x100000000ULL; value += 0x01234567) {
        char hex[9];

        snprintf(hex, sizeof(hex), "%08x", (unsigned)value);
        g_assert_true(resolve(SGI_BOARD_FAMILY_MODULE, 0, NULL, hex, NULL, &id,
                              error));
        g_assert_true(sgi_board_id_module_text(&id, 0, text));
        g_assert_cmpuint(text[0], ==, 'K');
        g_assert_cmphex(sgi_board_hostid_from_text(text), ==, value);
    }
}

static void test_resolve_address(void)
{
    static const uint8_t mac[6] = { 0x08, 0x00, 0x69, 0xe1, 0x02, 0xa0 };
    static const uint8_t other[6] = { 0x08, 0x00, 0x69, 0xaa, 0xbb, 0xcc };
    static const uint8_t foreign[6] = { 0x52, 0x54, 0x00, 0x11, 0x22, 0x33 };
    static const uint8_t sugg[6] = { 0x08, 0x00, 0x69, 0x10, 0x08, 0x87 };
    SGIBoardId id;
    char error[SGI_BOARD_ERROR_SIZE];

    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, NULL, NULL, &id,
                          error));
    g_assert_cmphex(id.id, ==, 0x69e10000);
    g_assert_false(id.user);
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 42, NULL, NULL, NULL, &id,
                          error));
    g_assert_cmphex(id.id, ==, 0x69e102a0);

    /* hostid starts 69 and sets the address. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 42, NULL, "69aabbcc",
                          NULL, &id, error));
    g_assert_cmphex(id.id, ==, 0x69aabbcc);
    g_assert_true(id.user);
    g_assert_true(id.address_from_id);
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "d2100887",
                           NULL, &id, error));
    g_assert_cmpstr(error, ==, "hostid=d2100887: this machine's host id is "
                    "69 plus 6 hex digits; use hostid=100887 or "
                    "mac=08:00:69:10:08:87");
    /* The suggestions are accepted, and name the same id. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "100887", NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x69100887);
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, NULL, sugg, &id,
                          error));
    g_assert_cmphex(id.id, ==, 0x69100887);
    /* The six-digit shorthand equals the full form. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "0bea40", NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x690bea40);
    g_assert_true(id.address_from_id);
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "690bea40",
                          NULL, &id, error));
    g_assert_cmphex(id.id, ==, 0x690bea40);
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "0BEA40", NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x690bea40);
    /* Any other length, or a non-hex digit, is refused. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "0bea4", NULL,
                           &id, error));
    g_assert_cmpstr(error, ==, "hostid must be 6 hex digits (the 69 is "
                    "implied) or 8 starting 69, not '0bea4'");
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "690bea400",
                           NULL, &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "0bea4g", NULL,
                           &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "690bea4g",
                           NULL, &id, error));

    /* serial is refused unless it folds to 69xxxxxx, with this message. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, "M2100887", NULL,
                           NULL, &id, error));
    g_assert_cmpstr(error, ==, "serial folds to d2100887; this machine's "
                    "host id comes from its station address, which always "
                    "starts 69; use hostid=100887 or mac=08:00:69:10:08:87");
    /* Feeding the suggestion back works, as hostid and as mac. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "100887", NULL,
                          &id, error));
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, NULL, sugg, &id,
                          error));
    /* "69" is characters whose low nibbles are 6 and 9: 'F' and 'I'. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, "F9N102J0", NULL, NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x69e102a0);

    /* mac sets the id; the three spellings must agree. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, NULL, other, &id,
                          error));
    g_assert_cmphex(id.id, ==, 0x69aabbcc);
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "69e102a0", mac,
                          &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "69aabbcc", mac,
                           &id, error));
    g_assert_cmpstr(error, ==, "mac=08:00:69:e1:02:a0 gives host id "
                    "69e102a0 but hostid=69aabbcc gives 69aabbcc; this "
                    "machine's host id comes from its station address, so it "
                    "would take 69e102a0 from mac; make them agree");
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "aabbcc", mac,
                           &id, error));
    g_assert_nonnull(strstr(error, "hostid=aabbcc"));
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "e102a0", mac,
                          &id, error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, "F9N102J0", NULL,
                           other, &id, error));
    g_assert_nonnull(strstr(error, "mac"));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, "F9N102J0", "69aabbcc",
                           NULL, &id, error));
    g_assert_nonnull(strstr(error, "F9N102J0"));
    g_assert_nonnull(strstr(error, "69aabbcc"));
    /* A mac outside 08:00:69 is refused, and the suggestion is accepted. */
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, NULL, foreign,
                           &id, error));
    g_assert_cmpstr(error, ==, "mac=52:54:00:11:22:33: this machine's "
                    "station address must start 08:00:69; use "
                    "mac=08:00:69:11:22:33 or hostid=112233");
    g_assert_true(resolve(SGI_BOARD_FAMILY_ADDRESS, 0, NULL, "112233", NULL,
                          &id, error));
    g_assert_cmphex(id.id, ==, 0x69112233);
    /* The text families keep any port address. */
    g_assert_true(resolve(SGI_BOARD_FAMILY_TEXT, 0, NULL, NULL, foreign, &id,
                          error));
    g_assert_false(resolve(SGI_BOARD_FAMILY_ADDRESS, 4096, NULL, NULL, NULL,
                           &id, error));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-board-identity/kind", test_kind_constant);
    g_test_add_func("/sgi-board-identity/system-serial", test_system_serial);
    g_test_add_func("/sgi-board-identity/module-serial", test_module_serial);
    g_test_add_func("/sgi-board-identity/hostid-fold", test_hostid_fold);
    g_test_add_func("/sgi-board-identity/default-mac", test_default_mac);
    g_test_add_func("/sgi-board-identity/sub-index", test_sub_index);
    g_test_add_func("/sgi-board-identity/text-serial", test_text_serial);
    g_test_add_func("/sgi-board-identity/clock-serial", test_clock_serial);
    g_test_add_func("/sgi-board-identity/fuel-text", test_fuel_text);
    g_test_add_func("/sgi-board-identity/emu-id-valid", test_emu_id_valid);
    g_test_add_func("/sgi-board-identity/spelling", test_spelling);
    g_test_add_func("/sgi-board-identity/hostid-parse", test_hostid_parse);
    g_test_add_func("/sgi-board-identity/resolve-text", test_resolve_text);
    g_test_add_func("/sgi-board-identity/resolve-module", test_resolve_module);
    g_test_add_func("/sgi-board-identity/resolve-address",
                    test_resolve_address);
    return g_test_run();
}
