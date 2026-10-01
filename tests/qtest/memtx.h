/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef QTEST_MEMTX_H
#define QTEST_MEMTX_H

#include "exec/memattrs.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"

#define MEMTX_TEST_DEVICE " -device memory-transaction-test,id=memtx "
#define MEMTX_TEST_PATH "/machine/peripheral/memtx"

/* Raw bytes, including failed-read data; no translated CPU-load semantics. */
static inline MemTxResult memtx_transaction(QTestState *qts, int cpu,
                                            uint64_t addr, uint8_t *bytes,
                                            unsigned size, bool write)
{
    g_autofree char *request = NULL;
    g_autoptr(QDict) reply = NULL;
    g_auto(GStrv) fields = NULL;
    uint64_t result;

    g_assert(size == 1 || size == 2 || size == 4 || size == 8);
    if (write) {
        char hex[17];

        for (unsigned i = 0; i < size; i++) {
            snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
        }
        request = g_strdup_printf("write %d 0x%" PRIx64 " %s", cpu, addr, hex);
    } else {
        request = g_strdup_printf("read %d 0x%" PRIx64 " %u", cpu, addr, size);
    }
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':'request',"
        "'value':%s}}", MEMTX_TEST_PATH, request);
    reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'result'}}",
        MEMTX_TEST_PATH);
    fields = g_strsplit(qdict_get_str(reply, "return"), " ", -1);
    g_assert_cmpuint(g_strv_length(fields), ==, 2);
    g_assert_cmpint(qemu_strtou64(fields[0], NULL, 0, &result), ==, 0);
    g_assert_cmpuint(result, <=, UINT32_MAX);
    g_assert_cmpuint(strlen(fields[1]), ==, 2 * size);
    for (unsigned i = 0; i < size; i++) {
        int high = g_ascii_xdigit_value(fields[1][2 * i]);
        int low = g_ascii_xdigit_value(fields[1][2 * i + 1]);

        g_assert(high >= 0 && low >= 0);
        bytes[i] = high << 4 | low;
    }
    return result;
}

static inline MemTxResult memtx_read(QTestState *qts, uint64_t addr,
                                     uint64_t *value, unsigned size)
{
    uint8_t bytes[8];
    MemTxResult result = memtx_transaction(qts, -1, addr, bytes, size, false);
    bool big = qtest_big_endian(qts);

    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        *value |= (uint64_t)bytes[i] << (8 * (big ? size - 1 - i : i));
    }
    return result;
}

static inline MemTxResult memtx_write(QTestState *qts, uint64_t addr,
                                      uint64_t value, unsigned size)
{
    uint8_t bytes[8];
    bool big = qtest_big_endian(qts);

    for (unsigned i = 0; i < size; i++) {
        bytes[i] = value >> (8 * (big ? size - 1 - i : i));
    }
    return memtx_transaction(qts, -1, addr, bytes, size, true);
}

#define MEMTX_ADAPTER(suffix, type, size) \
static inline MemTxResult memtx_read##suffix(QTestState *qts, uint64_t addr, \
                                             type *value) \
{ \
    uint64_t wide; \
    MemTxResult result = memtx_read(qts, addr, &wide, size); \
    *value = wide; \
    return result; \
} \
static inline MemTxResult memtx_write##suffix(QTestState *qts, uint64_t addr, \
                                              type value) \
{ \
    return memtx_write(qts, addr, value, size); \
}

MEMTX_ADAPTER(b, uint8_t, 1)
MEMTX_ADAPTER(w, uint16_t, 2)
MEMTX_ADAPTER(l, uint32_t, 4)
MEMTX_ADAPTER(q, uint64_t, 8)
#undef MEMTX_ADAPTER

#endif
