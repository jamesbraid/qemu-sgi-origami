#include "qemu/osdep.h"
#include "hw/1wire/crc.h"

/* The identity CRC-8 spans exactly seven bytes: family plus six serial. */
static void test_crc8_rom_identity(void)
{
    /* Family 0x0b, serial 01 02 03 04 05 06. */
    const uint8_t id[7] = { 0x0b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    uint8_t crc = onewire_crc8(0, id, sizeof(id));

    /* A full identity including its CRC checks to zero. */
    uint8_t full[8];
    memcpy(full, id, sizeof(id));
    full[7] = crc;
    g_assert_cmpuint(onewire_crc8(0, full, sizeof(full)), ==, 0);
}

static void test_crc8_is_not_inverted(void)
{
    const uint8_t zero[1] = { 0x00 };
    g_assert_cmpuint(onewire_crc8(0, zero, 1), ==, 0x00);
}

static void test_crc16_accumulates(void)
{
    const uint8_t a[2] = { 0xf0, 0x00 };
    const uint8_t b[1] = { 0x00 };
    uint16_t split = onewire_crc16(onewire_crc16(0, a, 2), b, 1);
    const uint8_t all[3] = { 0xf0, 0x00, 0x00 };
    g_assert_cmpuint(split, ==, onewire_crc16(0, all, 3));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/onewire/crc8/rom-identity", test_crc8_rom_identity);
    g_test_add_func("/onewire/crc8/not-inverted", test_crc8_is_not_inverted);
    g_test_add_func("/onewire/crc16/accumulates", test_crc16_accumulates);
    return g_test_run();
}
