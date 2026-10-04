/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-dma-buffer.h"
#include "hw/pci/pci_device.h"

/* Exercise the production fallbacks without adding a public test hook. */
#include "hw/misc/sgi-dma-bswap.c"
#ifndef _WIN32
#include "qemu/mmap-alloc.h"
#endif

static unsigned native_calls;
static dma_addr_t native_addr;
static dma_addr_t native_len;
static MemTxAttrs native_attrs;

MemTxResult address_space_rw(const AddressSpace *as, hwaddr addr,
                            MemTxAttrs attrs, void *buf, hwaddr len,
                            bool is_write)
{
    native_calls++;
    native_addr = addr;
    native_len = len;
    native_attrs = attrs;
    if (!is_write) {
        memset(buf, 0xcc, len);
    }
    return MEMTX_DECODE_ERROR;
}

typedef struct TestProvider {
    unsigned calls;
    unsigned handled;
    bool fail;
    bool swap32;
} TestProvider;

static bool transfer(void *opaque, dma_addr_t addr, void *buf,
                     dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                     const SGIDMABufferRequest *request, MemTxResult *result)
{
    TestProvider *s = opaque;

    s->calls++;
    g_assert_cmpint(request->swap32, ==, s->swap32);
    if (!s->handled) {
        return false;
    }
    *len = MIN(*len, s->handled);
    s->handled = 0;
    if (dir == DMA_DIRECTION_TO_DEVICE) {
        memset(buf, 0x5a, *len);
    }
    *result = s->fail ? MEMTX_ERROR : MEMTX_OK;
    return true;
}

static void test_root(void)
{
    MemoryRegion root = { 0 };
    AddressSpace parent = { .root = &root };
    AddressSpace child = { .root = &root };
    TestProvider state = { .handled = 8 };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    SGIDMABufferContext parent_ctx = { .as = &parent };
    SGIDMABufferContext child_ctx = { .as = &child };
    uint8_t bytes[8];

    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&child_ctx,
                                               0x123, bytes, 8), ==,
                    MEMTX_DECODE_ERROR);
    g_assert_cmpuint(native_calls, ==, 1);
    parent_ctx.provider = child_ctx.provider = &provider;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&parent_ctx,
                                               0x123, bytes, 8), ==,
                    MEMTX_OK);
    g_assert_cmpuint(bytes[0], ==, 0x5a);
    state.handled = 8;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&child_ctx,
                                               0x123, bytes, 8), ==,
                    MEMTX_OK);
    g_assert_cmpuint(state.calls, ==, 2);
    g_assert_cmpuint(native_calls, ==, 1);
}

static void test_conversion_request(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    TestProvider state = { .handled = 8, .swap32 = true };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    SGIDMABufferContext ctx = {
        .as = &as, .provider = &provider,
        .request = { .swap32 = true, .max_access_size = 4 },
    };
    uint8_t bytes[8];

    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x100, bytes, 8), ==,
                    MEMTX_OK);
}

static void test_decline(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    SGIDMABufferContext ctx = { .as = &as };
    TestProvider state = { 0 };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    uint8_t bytes[8];
    MemTxAttrs attrs = { .memory = true, .requester_id = 0x321 };

    ctx.provider = &provider;
    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_rw_relaxed(&ctx, 0x123, bytes, 8,
                    DMA_DIRECTION_TO_DEVICE, attrs), ==, MEMTX_DECODE_ERROR);
    g_assert_cmpuint(state.calls, ==, 1);
    g_assert_cmpuint(native_calls, ==, 1);
    g_assert_cmphex(native_addr, ==, 0x123);
    g_assert_cmpuint(native_len, ==, 8);
    g_assert_true(native_attrs.memory);
    g_assert_cmphex(native_attrs.requester_id, ==, 0x321);
    g_assert_cmpuint(bytes[0], ==, 0xcc);
}

static void test_error(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    SGIDMABufferContext ctx = { .as = &as };
    TestProvider state = { .handled = 8, .fail = true };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    uint8_t bytes[8];

    ctx.provider = &provider;
    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, bytes, 8), ==,
                    MEMTX_ERROR);
    g_assert_cmpuint(state.calls, ==, 1);
    g_assert_cmpuint(native_calls, ==, 0);
    g_assert_cmpuint(bytes[7], ==, 0x5a);
}

static void test_partial(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    SGIDMABufferContext ctx = { .as = &as };
    TestProvider state = { .handled = 4 };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    uint8_t bytes[8];
    uint8_t source[8], before[8];

    ctx.provider = &provider;
    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, bytes, 8), ==,
                    MEMTX_DECODE_ERROR);
    g_assert_cmpuint(state.calls, ==, 2);
    g_assert_cmpuint(native_calls, ==, 1);
    g_assert_cmphex(native_addr, ==, 0x127);
    g_assert_cmpuint(native_len, ==, 4);
    g_assert_cmpuint(bytes[3], ==, 0x5a);
    g_assert_cmpuint(bytes[4], ==, 0xcc);
    memset(source, 0xa5, sizeof(source));
    memcpy(before, source, sizeof(before));
    state.handled = 4;
    sgi_dma_buffer_write_relaxed(&ctx, 0x123, source, sizeof(source));
    g_assert_cmpmem(source, sizeof(source), before, sizeof(before));
}

static void test_zero(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    SGIDMABufferContext ctx = { .as = &as };
    TestProvider state = { .handled = 4 };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    uint8_t byte = 0xa5;

    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, &byte, 0), ==,
                    MEMTX_DECODE_ERROR);
    g_assert_cmpuint(native_calls, ==, 1);
    ctx.provider = &provider;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, &byte, 0), ==,
                    MEMTX_DECODE_ERROR);
    g_assert_cmpuint(native_calls, ==, 2);
    g_assert_cmpuint(native_len, ==, 0);
    g_assert_cmpuint(state.calls, ==, 0);
    g_assert_cmpuint(byte, ==, 0xa5);
}

static void test_partial_error(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    SGIDMABufferContext ctx = { .as = &as };
    TestProvider state = { .handled = 4, .fail = true };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    uint8_t bytes[8];

    ctx.provider = &provider;
    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, bytes, 8), ==,
                    MEMTX_ERROR | MEMTX_DECODE_ERROR);
    g_assert_cmpuint(state.calls, ==, 2);
    g_assert_cmpuint(native_calls, ==, 1);
    g_assert_cmphex(native_addr, ==, 0x127);
    g_assert_cmpuint(native_len, ==, 4);
    g_assert_cmpuint(bytes[3], ==, 0x5a);
    g_assert_cmpuint(bytes[4], ==, 0xcc);
}

static bool master_enabled(PCIDevice *dev)
{
    return dev->is_master;
}

static void test_master(void)
{
    MemoryRegion root = { 0 };
    AddressSpace as = { .root = &root };
    PCIDevice dev = { 0 };
    TestProvider state = { .handled = 8 };
    SGIDMABufferProvider provider = { .transfer = transfer, .opaque = &state };
    SGIDMABufferContext ctx = {
        .as = &as, .pci_dev = &dev, .enabled = master_enabled,
        .provider = &provider,
    };
    uint8_t bytes[8];

    native_calls = 0;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, bytes, 8), ==,
                    MEMTX_DECODE_ERROR);
    g_assert_cmpuint(state.calls, ==, 0);
    g_assert_cmpuint(native_calls, ==, 1);
    dev.is_master = true;
    g_assert_cmpint(sgi_dma_buffer_read_relaxed(&ctx, 0x123, bytes, 8), ==,
                    MEMTX_OK);
    g_assert_cmpuint(state.calls, ==, 1);
    g_assert_cmpuint(native_calls, ==, 1);
}

static void exercise_swap(BSwap32Buffer convert)
{
    const size_t lengths[] = {
        0, 4, 8, 12, 16, 20, 28, 32, 36, 60, 64, 68, 124, 128, 132,
        1500, 4092, 4096,
    };
    uint8_t input[4160], before[4160], output[4160], expected[4160];

    for (unsigned src = 0; src < 32; src++) {
        for (unsigned dst = 0; dst < 32; dst++) {
            for (unsigned size = 0; size < G_N_ELEMENTS(lengths); size++) {
                size_t length = lengths[size];

                for (unsigned i = 0; i < sizeof(input); i++) {
                    input[i] = i;
                }
                memcpy(before, input, sizeof(input));
                memset(output, 0xab, sizeof(output));
                memcpy(expected, output, sizeof(output));
                for (unsigned i = 0; i < length; i++) {
                    expected[dst + i] = input[src + (i ^ 3)];
                }
                convert(output + dst, input + src, length);
                g_assert_cmpmem(output, sizeof(output),
                                expected, sizeof(expected));
                g_assert_cmpmem(input, sizeof(input), before, sizeof(before));
                memcpy(expected, before, sizeof(expected));
                for (unsigned i = 0; i < length; i++) {
                    expected[src + i] = before[src + (i ^ 3)];
                }
                convert(input + src, input + src, length);
                g_assert_cmpmem(input, sizeof(input),
                                expected, sizeof(expected));
            }
        }
    }
#ifndef _WIN32
    /* Native guard pages discriminate a load or store beyond the span. */
    size_t page = qemu_fd_getpagesize(-1);
    uint8_t *in = qemu_ram_mmap(-1, page, page, 0, 0);
    uint8_t *out = qemu_ram_mmap(-1, page, page, 0, 0);

    g_assert_true(in != MAP_FAILED && out != MAP_FAILED);
    for (unsigned size = 0; size < G_N_ELEMENTS(lengths); size++) {
        size_t length = MIN(lengths[size], page);
        uint8_t *source = in + page - length;
        uint8_t *target = out + page - length;

        for (unsigned i = 0; i < length; i++) {
            source[i] = i;
        }
        convert(target, source, length);
        for (unsigned i = 0; i < length; i++) {
            g_assert_cmpuint(target[i], ==, (uint8_t)(i ^ 3));
        }
        convert(source, source, length);
        for (unsigned i = 0; i < length; i++) {
            g_assert_cmpuint(source[i], ==, (uint8_t)(i ^ 3));
        }
    }
    qemu_ram_munmap(-1, in, page);
    qemu_ram_munmap(-1, out, page);
#endif
}

static void test_swap(void)
{
    exercise_swap(sgi_bswap32_buf);
}

static void test_swap_words(void)
{
    exercise_swap(bswap32_words);
}

#ifdef CPUINFO_SSE2
static void test_swap_sse2(void)
{
    exercise_swap(bswap32_sse2);
}
#ifdef CONFIG_AVX2_OPT
static void test_swap_avx2(void)
{
    exercise_swap(bswap32_avx2);
}
#endif
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dma/buffer/conversion-request", test_conversion_request);
    g_test_add_func("/dma-buffer/shared-root", test_root);
    g_test_add_func("/dma-buffer/decline", test_decline);
    g_test_add_func("/dma-buffer/handled-error", test_error);
    g_test_add_func("/dma-buffer/partial", test_partial);
    g_test_add_func("/dma-buffer/zero", test_zero);
    g_test_add_func("/dma-buffer/partial-error", test_partial_error);
    g_test_add_func("/dma-buffer/master", test_master);
    g_test_add_func("/dma-buffer/swap32", test_swap);
    g_test_add_func("/dma-buffer/swap32-words", test_swap_words);
#ifdef CPUINFO_SSE2
    if (cpuinfo_init() & CPUINFO_SSE2) {
        g_test_add_func("/dma-buffer/swap32-sse2", test_swap_sse2);
    }
#ifdef CONFIG_AVX2_OPT
    if (cpuinfo_init() & CPUINFO_AVX2) {
        g_test_add_func("/dma-buffer/swap32-avx2", test_swap_avx2);
    }
#endif
#endif
    return g_test_run();
}
