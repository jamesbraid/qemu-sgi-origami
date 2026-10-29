/*
 * Network CRC tests.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "net/net.h"

static uint32_t reference_crc(const uint8_t *data, int length)
{
    uint32_t state = UINT32_MAX;

    for (int i = 0; i < length; i++) {
        state ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            state = (state >> 1) ^ ((state & 1) ? 0xedb88320U : 0);
        }
    }
    return state;
}

static void test_empty(void)
{
    g_assert_cmphex(net_crc32_le(NULL, 0), ==, UINT32_MAX);
    g_assert_cmphex(net_crc32_le(NULL, -1), ==, UINT32_MAX);
    g_assert_cmphex(net_crc32_le(NULL, INT_MIN), ==, UINT32_MAX);
}

static void test_known(void)
{
    const uint8_t text[] = "123456789";

    /* The Ethernet helper returns the state before the final complement. */
    g_assert_cmphex(net_crc32_le(text, 9), ==, 0x340bc6d9);
}

static void test_bytes(void)
{
    for (unsigned int value = 0; value < 256; value++) {
        uint8_t byte = value;

        g_assert_cmphex(net_crc32_le(&byte, 1), ==, reference_crc(&byte, 1));
    }
}

static void test_packets(void)
{
    static const int lengths[] = {
        0, 1, 2, 3, 7, 8, 9, 14, 42, 59, 60, 63, 64, 65,
        127, 128, 255, 256, 511, 512, 1024, 1500, 1514, 1518,
        1522, 2048, 4096, 9000, 9018, 65535,
    };
    uint8_t *storage = g_malloc(65535 + 8);
    uint32_t random = 0x12345678;

    for (unsigned int pattern = 0; pattern < 5; pattern++) {
        for (int i = 0; i < 65535 + 8; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            storage[i] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                         pattern == 2 ? i : pattern == 3 ? (i & 1) * 255 :
                         random;
        }
        for (unsigned int offset = 0; offset < 8; offset++) {
            for (unsigned int i = 0; i < G_N_ELEMENTS(lengths); i++) {
                g_assert_cmphex(net_crc32_le(storage + offset, lengths[i]),
                                ==, reference_crc(storage + offset,
                                                  lengths[i]));
            }
        }
    }
    /* Every short packet length, including unaligned starts. */
    for (int length = 0; length <= 2048; length++) {
        const uint8_t *packet = storage + (length & 7);

        g_assert_cmphex(net_crc32_le(packet, length), ==,
                        reference_crc(packet, length));
    }
    g_free(storage);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/net/crc/empty", test_empty);
    g_test_add_func("/net/crc/known", test_known);
    g_test_add_func("/net/crc/bytes", test_bytes);
    g_test_add_func("/net/crc/packets", test_packets);
    return g_test_run();
}
