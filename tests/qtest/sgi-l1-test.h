/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared qtest framing for the existing L1 UART protocol. */
#ifndef SGI_L1_TEST_H
#define SGI_L1_TEST_H

#include "libqtest.h"

static inline uint16_t sgi_l1_test_fcs(uint16_t v, uint8_t b)
{
    v ^= b;
    for (unsigned i = 0; i < 8; i++) {
        v = (v >> 1) ^ ((v & 1) ? 0x8408 : 0);
    }
    return v;
}

static inline void sgi_l1_test_put(QTestState *s, uint64_t base, uint8_t b)
{
    if (b == 0x7e || b == 0x7d) {
        qtest_writeq(s, base, 0x7d);
        b ^= 0x20;
    }
    qtest_writeq(s, base, b);
}

static inline void sgi_l1_test_send(QTestState *s, uint64_t base,
                                    const uint8_t *p, size_t n, bool corrupt)
{
    uint16_t crc = 0xffff;
    qtest_writeq(s, base, 0x7e);
    for (size_t i = 0; i < n; i++) {
        crc = sgi_l1_test_fcs(crc, p[i]);
        sgi_l1_test_put(s, base, p[i]);
    }
    crc = ~crc;
    sgi_l1_test_put(s, base, crc ^ corrupt);
    sgi_l1_test_put(s, base, crc >> 8);
    qtest_writeq(s, base, 0x7e);
}

static inline size_t sgi_l1_test_receive(QTestState *s, uint64_t base,
                                        uint8_t *p, size_t capacity)
{
    size_t n = 0;
    bool escape = false;
    uint16_t crc = 0xffff;
    g_assert_cmphex(qtest_readq(s, base + 0x28), ==, 0x61);
    g_assert_cmphex(qtest_readq(s, base), ==, 0x7e);
    for (;;) {
        uint8_t b;
        g_assert_cmphex(qtest_readq(s, base + 0x28), ==, 0x61);
        b = qtest_readq(s, base);
        if (b == 0x7e) {
            break;
        }
        if (b == 0x7d && !escape) {
            escape = true;
            continue;
        }
        if (escape) {
            b ^= 0x20;
            escape = false;
        }
        g_assert_cmpuint(n, <, capacity);
        p[n++] = b;
        crc = sgi_l1_test_fcs(crc, b);
    }
    g_assert_false(escape);
    g_assert_cmpuint(n, >=, 3);
    g_assert_cmphex(crc, ==, 0xf0b8);
    return n - 2;
}

#endif /* SGI_L1_TEST_H */
