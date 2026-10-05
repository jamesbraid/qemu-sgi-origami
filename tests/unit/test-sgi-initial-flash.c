/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Initial IP27, IP30, IP35 and I/O PROM flash contents, over synthetic
 * payloads: no SGI firmware bytes appear here. Offsets and values are
 * repeated as literals rather than taken from the header under test.
 *
 * Copyright (c) 2026 James Braid
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "hw/mips/sgi/initial-flash.h"

#define IP27_FLASH_SIZE     (1 * MiB)
#define IP30_FLASH_SIZE     (1 * MiB)
#define IP35_FLASH_SIZE     (16 * MiB)
#define IO6_FLASH_SIZE      (1 * MiB)

#define CONFIG              0x60
#define CONFIG_SIZE         0x50
#define CONFIG_MAGIC        0x69703237636f6e66ULL
#define CONFIG_FPROM_CYC    0x2c
#define CONFIG_MACHINE_TYPE 0x30
#define CONFIG_CHECKSUM     0x34
#define CONFIG_FPROM_WR     0x3c

/* The IP27 factory-fresh environment log, in sector 14 of 64 KiB. */
#define IP27_LOG            0xe0000

/* The IP35 outer label: position one (8 MiB up), then 0x1e0000. */
#define IP35_LABEL          0x9e0000
#define IP35_LABEL_BACKUP   0x9f0000

/* Origin 200's product values. */
static const SGIIP27FlashValues origin200 = {
    .machine_type = 1,
    .read_cycles = 15,
    .write_cycles = 4,
    .cpu_mode_bits = 0x12345678,
    .cpu_clock_hz = 180000000,
};

static uint8_t byte_sum(const uint8_t *data, size_t size)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

/*
 * A payload whose configuration block carries a stock image's machine
 * type 0, 8 read cycles and 1 write cycle, plus bytes the patch must keep.
 */
static uint8_t *make_payload(size_t size)
{
    uint8_t *payload = g_malloc(size);
    size_t i;

    for (i = 0; i < size; i++) {
        payload[i] = i * 37 + 11;
    }
    stq_be_p(payload + CONFIG + 0x08, CONFIG_MAGIC);
    stl_be_p(payload + CONFIG + CONFIG_FPROM_CYC, 8);
    stl_be_p(payload + CONFIG + CONFIG_MACHINE_TYPE, 0);
    stl_be_p(payload + CONFIG + CONFIG_FPROM_WR, 1);
    return payload;
}

static void assert_ip27_log(const uint8_t *flash)
{
    size_t i;

    for (i = 0; i < 0x10; i++) {
        g_assert_cmphex(flash[IP27_LOG + i], ==, 0xff);
    }
    g_assert_cmphex(ldl_be_p(flash + IP27_LOG + 0x10), ==, 0x504c4f47);
    g_assert_cmphex(ldl_be_p(flash + IP27_LOG + 0x14), ==, 1);
    g_assert_cmphex(ldl_be_p(flash + IP27_LOG + 0x18), ==, 1);
    /* An erased first entry ends the log. */
    g_assert_cmphex((uint32_t)ldl_be_p(flash + IP27_LOG + 0x1c), ==,
                    0xffffffff);
}

/*
 * Origin 200's values replace the payload's own, the block's byte sum
 * stays where the payload left it, and nothing else in the payload moves.
 * The template shows through past the payload's end.
 */
static void test_ip27_payload(void)
{
    const size_t size = 0x400;
    g_autofree uint8_t *payload = make_payload(size);
    g_autofree uint8_t *flash = g_malloc(IP27_FLASH_SIZE);
    size_t i;

    sgi_ip27_initial_flash(flash, IP27_FLASH_SIZE, &origin200, payload, size);

    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_MACHINE_TYPE), ==, 1);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_FPROM_CYC), ==, 15);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_FPROM_WR), ==, 4);
    g_assert_cmphex(byte_sum(flash + CONFIG, CONFIG_SIZE), ==,
                    byte_sum(payload + CONFIG, CONFIG_SIZE));

    for (i = 0; i < size; i++) {
        size_t field = i - CONFIG;

        if (i >= CONFIG && field < CONFIG_SIZE &&
            ((field >= CONFIG_FPROM_CYC && field < CONFIG_CHECKSUM + 4) ||
             (field >= CONFIG_FPROM_WR && field < CONFIG_FPROM_WR + 4))) {
            continue;
        }
        g_assert_cmphex(flash[i], ==, payload[i]);
    }
    for (i = size; i < IP27_LOG; i++) {
        g_assert_cmphex(flash[i], ==, 0xff);
    }
    assert_ip27_log(flash);
}

/* With no payload the template's own block carries the product values. */
static void test_ip27_template(void)
{
    g_autofree uint8_t *flash = g_malloc(IP27_FLASH_SIZE);

    sgi_ip27_initial_flash(flash, IP27_FLASH_SIZE, &origin200, NULL, 0);

    g_assert_cmphex(ldl_be_p(flash), ==, 0x1000ffff);
    g_assert_cmphex(ldl_be_p(flash + 8), ==, 0x03e00008);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + 0x04), ==, 0x12345678);
    g_assert_cmphex(ldq_be_p(flash + CONFIG + 0x08), ==, CONFIG_MAGIC);
    g_assert_cmpuint(ldq_be_p(flash + CONFIG + 0x10), ==, 180000000);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_MACHINE_TYPE), ==, 1);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_FPROM_CYC), ==, 15);
    g_assert_cmphex(ldl_be_p(flash + CONFIG + CONFIG_FPROM_WR), ==, 4);
    assert_ip27_log(flash);
}

/* A payload without the configuration-block magic is left untouched. */
static void test_ip27_no_magic(void)
{
    const size_t size = 0x400;
    g_autofree uint8_t *payload = make_payload(size);
    g_autofree uint8_t *flash = g_malloc(IP27_FLASH_SIZE);

    stq_be_p(payload + CONFIG + 0x08, 0);
    sgi_ip27_initial_flash(flash, IP27_FLASH_SIZE, &origin200, payload, size);
    g_assert_cmpmem(flash, size, payload, size);
}

/*
 * A blank IP35 flash: the payload in position zero, the formatter's outer
 * label in position one, and every other byte erased.
 */
static void test_ip35(void)
{
    static const uint8_t label[28] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x50, 0x4c, 0x4f, 0x49, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
    };
    const size_t size = 0x1000;
    g_autofree uint8_t *payload = make_payload(size);
    g_autofree uint8_t *flash = g_malloc0(IP35_FLASH_SIZE);
    size_t i;

    sgi_ip35_initial_flash(flash, IP35_FLASH_SIZE, payload, size);

    g_assert_cmpmem(flash, size, payload, size);
    g_assert_cmpmem(flash + IP35_LABEL, sizeof(label), label, sizeof(label));
    for (i = size; i < IP35_FLASH_SIZE; i++) {
        if (i >= IP35_LABEL + 0x10 && i < IP35_LABEL + sizeof(label)) {
            continue;
        }
        if (flash[i] != 0xff) {
            g_test_message("byte 0x%zx is 0x%02x", i, flash[i]);
            g_assert_not_reached();
        }
    }
    g_assert_cmphex(flash[IP35_LABEL_BACKUP + 0x10], ==, 0xff);
}

/* An IP30 flash holds the payload, then erased bytes, and nothing else. */
static void test_ip30(void)
{
    const size_t size = 0x2345;
    g_autofree uint8_t *payload = make_payload(size);
    g_autofree uint8_t *flash = g_malloc0(IP30_FLASH_SIZE);
    size_t i;

    sgi_ip30_initial_flash(flash, IP30_FLASH_SIZE, payload, size);
    g_assert_cmpmem(flash, size, payload, size);
    for (i = size; i < IP30_FLASH_SIZE; i++) {
        g_assert_cmphex(flash[i], ==, 0xff);
    }
    /* No payload: an erased part. */
    sgi_ip30_initial_flash(flash, IP30_FLASH_SIZE, NULL, 0);
    for (i = 0; i < IP30_FLASH_SIZE; i++) {
        g_assert_cmphex(flash[i], ==, 0xff);
    }
}

/* The I/O PROM part holds the file as given, then erased bytes. */
static void test_io6(void)
{
    const size_t size = 0x1234;
    g_autofree uint8_t *file = make_payload(size);
    g_autofree uint8_t *flash = g_malloc0(IO6_FLASH_SIZE);
    size_t i;

    sgi_io6_initial_flash(flash, IO6_FLASH_SIZE, file, size);
    g_assert_cmpmem(flash, size, file, size);
    for (i = size; i < IO6_FLASH_SIZE; i++) {
        g_assert_cmphex(flash[i], ==, 0xff);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-initial-flash/ip27-payload", test_ip27_payload);
    g_test_add_func("/sgi-initial-flash/ip27-template", test_ip27_template);
    g_test_add_func("/sgi-initial-flash/ip27-no-magic", test_ip27_no_magic);
    g_test_add_func("/sgi-initial-flash/ip30", test_ip30);
    g_test_add_func("/sgi-initial-flash/ip35", test_ip35);
    g_test_add_func("/sgi-initial-flash/io6", test_io6);
    return g_test_run();
}
