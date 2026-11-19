/* SPDX-License-Identifier: BSD-3-Clause */
/* The L1 board settings image: sums, mirror, fields and refusals. */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "hw/nvram/sgi-l1-settings-image.h"

#define RECORD1     0x008
#define MIRROR      0x078
#define RECORD_LEN  0x70

static uint8_t sum(const uint8_t *p, size_t len)
{
    uint8_t s = 0;

    while (len--) {
        s += *p++;
    }
    return s;
}

typedef uint8_t img_t[SGI_L1_SETTINGS_WINDOW_BYTES];

static void check_image(const uint8_t *img)
{
    static const uint8_t header[8] = { 1, 1, 0, 0, 0, 0, 0, 0xfe };

    g_assert_cmpmem(img, 8, header, 8);
    g_assert_cmphex(sum(img, 8), ==, 0);
    g_assert_cmphex(sum(img + RECORD1, RECORD_LEN), ==, 0);
    g_assert_cmpmem(img + RECORD1, RECORD_LEN, img + MIRROR, RECORD_LEN);
    /* The version nibble the firmware takes as current. */
    g_assert_cmphex(img[RECORD1] & 0xf, ==, 0xa);
    /* Nothing past the first record and its mirror: the rest is rebuilt. */
    for (size_t i = MIRROR + RECORD_LEN; i < sizeof(img_t); i++) {
        g_assert_cmphex(img[i], ==, 0);
    }
}

static void test_defaults(void)
{
    uint8_t img[SGI_L1_SETTINGS_WINDOW_BYTES];
    SGIL1Settings s = { };

    memset(img, 0x55, sizeof(img));
    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    check_image(img);
    g_assert_cmphex(ldl_be_p(img + RECORD1), ==, 0x0a000000);
    /* The values a rebuilt record gets. */
    g_assert_cmphex(img[RECORD1 + 0x04], ==, 0x70);
    g_assert_cmphex(img[RECORD1 + 0x11], ==, 0x02);
    g_assert_cmphex(img[RECORD1 + 0x17], ==, 0xf4);
    g_assert_cmphex(img[RECORD1 + 0x1f], ==, 0x00);
    g_assert_cmphex(img[RECORD1 + 0x20], ==, 0x05);
    g_assert_cmphex(img[RECORD1 + 0x21], ==, 0x0a);
    g_assert_cmphex(img[RECORD1 + 0x22], ==, 0x00);
    g_assert_cmphex(img[RECORD1 + 0x2e], ==, 0x02);
}

static void test_fields(void)
{
    uint8_t img[SGI_L1_SETTINGS_WINDOW_BYTES];
    SGIL1Settings s = {
        .rack = 0x2a5, .bay = 0x15, .partition = 2, .brick_type = 'C',
        .auto_power_up = true, .skip_serial_check = true,
    };
    uint32_t id;

    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    check_image(img);
    id = ldl_be_p(img + RECORD1);
    g_assert_cmphex(extract32(id, 14, 10), ==, 0x2a5);
    g_assert_cmphex(extract32(id, 8, 6), ==, 0x15);
    g_assert_cmphex(img[RECORD1 + 0x04], ==, 0xf0);
    g_assert_cmphex(img[RECORD1 + 0x17], ==, 0xfc);
    g_assert_cmphex(img[RECORD1 + 0x1f], ==, 2);
    g_assert_cmphex(img[RECORD1 + 0x22], ==, 'C');
}

/* The serial lands at window 0x010; nothing else moves but the two sums. */
static void test_serial(void)
{
    uint8_t plain[SGI_L1_SETTINGS_WINDOW_BYTES];
    uint8_t img[SGI_L1_SETTINGS_WINDOW_BYTES];
    SGIL1Settings s = { };
    size_t i;

    g_assert_true(sgi_l1_settings_image(plain, &s, &error_abort));
    for (i = RECORD1 + 8; i < RECORD1 + 0x10; i++) {
        g_assert_cmphex(plain[i], ==, 0);
    }
    s.system_serial = "L1234567";
    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    check_image(img);
    g_assert_cmpmem(img + 0x010, 9, "L1234567", 9);
    s.system_serial = "M3000001";
    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    g_assert_cmpmem(img + 0x010, 9, "M3000001", 9);
    s.system_serial = "L1234567";
    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    for (i = 0; i < sizeof(img); i++) {
        bool field = (i >= 0x010 && i < 0x018) || (i >= MIRROR + 8 &&
                                                   i < MIRROR + 0x10);

        if (!field && i != RECORD1 + RECORD_LEN - 1 &&
            i != MIRROR + RECORD_LEN - 1) {
            g_assert_cmphex(img[i], ==, plain[i]);
        }
    }
}

static void test_serial_refused(void)
{
    static const char *const bad[] = {
        "", "L123456", "L12345678", "S1234567", "l1234567", "L4567890",
        "L12345x7", "L0000000", "M0000000", "L3999999", "99999999",
    };
    uint8_t img[SGI_L1_SETTINGS_WINDOW_BYTES];

    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        SGIL1Settings s = { .system_serial = bad[i] };
        Error *err = NULL;

        g_assert_false(sgi_l1_settings_image(img, &s, &err));
        error_free_or_abort(&err);
    }
}

static void test_limits(void)
{
    uint8_t img[SGI_L1_SETTINGS_WINDOW_BYTES];
    SGIL1Settings s = { .rack = 1023, .bay = 63, .partition = 254 };
    Error *err = NULL;

    g_assert_true(sgi_l1_settings_image(img, &s, &error_abort));
    check_image(img);
    s.rack = 1024;
    g_assert_false(sgi_l1_settings_image(img, &s, &err));
    error_free_or_abort(&err);
    s.rack = 0;
    s.bay = 64;
    g_assert_false(sgi_l1_settings_image(img, &s, &err));
    error_free_or_abort(&err);
    s.bay = 0;
    s.partition = 0xff;
    g_assert_false(sgi_l1_settings_image(img, &s, &err));
    error_free_or_abort(&err);
    s.partition = 0;
    s.brick_type = '4';
    g_assert_false(sgi_l1_settings_image(img, &s, &err));
    error_free_or_abort(&err);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-l1-settings/defaults", test_defaults);
    g_test_add_func("/sgi-l1-settings/fields", test_fields);
    g_test_add_func("/sgi-l1-settings/serial", test_serial);
    g_test_add_func("/sgi-l1-settings/serial-refused", test_serial_refused);
    g_test_add_func("/sgi-l1-settings/limits", test_limits);
    return g_test_run();
}
