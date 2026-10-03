/* SPDX-License-Identifier: BSD-3-Clause */
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

typedef struct SGIL1TestIO {
    void *opaque;
    uint64_t (*read)(void *opaque, uint64_t offset);
    void (*write)(void *opaque, uint64_t offset, uint64_t value);
} SGIL1TestIO;

static inline void sgi_l1_test_put_io(const SGIL1TestIO *io, uint8_t b)
{
    if (b == 0x7e || b == 0x7d) {
        io->write(io->opaque, 0, 0x7d);
        b ^= 0x20;
    }
    io->write(io->opaque, 0, b);
}

static inline void sgi_l1_test_send_io(const SGIL1TestIO *io,
                                      const uint8_t *p, size_t n, bool corrupt)
{
    uint16_t crc = 0xffff;
    io->write(io->opaque, 0, 0x7e);
    for (size_t i = 0; i < n; i++) {
        crc = sgi_l1_test_fcs(crc, p[i]);
        sgi_l1_test_put_io(io, p[i]);
    }
    crc = ~crc;
    sgi_l1_test_put_io(io, crc ^ corrupt);
    sgi_l1_test_put_io(io, crc >> 8);
    io->write(io->opaque, 0, 0x7e);
}

static inline size_t sgi_l1_test_receive_io(const SGIL1TestIO *io,
                                          uint8_t *p, size_t capacity)
{
    size_t n = 0;
    bool escape = false;
    uint16_t crc = 0xffff;
    g_assert_cmphex(io->read(io->opaque, 0x28), ==, 0x61);
    g_assert_cmphex(io->read(io->opaque, 0), ==, 0x7e);
    for (;;) {
        uint8_t b;
        g_assert_cmphex(io->read(io->opaque, 0x28), ==, 0x61);
        b = io->read(io->opaque, 0);
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

typedef struct SGIL1TestMMIO {
    QTestState *qts;
    uint64_t base;
} SGIL1TestMMIO;

static inline uint64_t sgi_l1_test_read(void *opaque, uint64_t offset)
{
    SGIL1TestMMIO *mmio = opaque;
    return qtest_readq(mmio->qts, mmio->base + offset);
}

static inline void sgi_l1_test_write(void *opaque, uint64_t offset,
                                    uint64_t value)
{
    SGIL1TestMMIO *mmio = opaque;
    qtest_writeq(mmio->qts, mmio->base + offset, value);
}

static inline void sgi_l1_test_put(QTestState *s, uint64_t base, uint8_t b)
{
    SGIL1TestMMIO mmio = { s, base };
    SGIL1TestIO io = { &mmio, sgi_l1_test_read, sgi_l1_test_write };
    sgi_l1_test_put_io(&io, b);
}

static inline void sgi_l1_test_send(QTestState *s, uint64_t base,
                                   const uint8_t *p, size_t n, bool corrupt)
{
    SGIL1TestMMIO mmio = { s, base };
    SGIL1TestIO io = { &mmio, sgi_l1_test_read, sgi_l1_test_write };
    sgi_l1_test_send_io(&io, p, n, corrupt);
}

static inline size_t sgi_l1_test_receive(QTestState *s, uint64_t base,
                                       uint8_t *p, size_t capacity)
{
    SGIL1TestMMIO mmio = { s, base };
    SGIL1TestIO io = { &mmio, sgi_l1_test_read, sgi_l1_test_write };
    return sgi_l1_test_receive_io(&io, p, capacity);
}

#endif /* SGI_L1_TEST_H */
