/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-dma-buffer.h"

/* Keep the byte loop as the independently compiled comparison. */
static void __attribute__((noinline)) scalar(uint8_t *dst,
                                            const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        uint8_t a = src[i], b = src[i + 1];
        uint8_t c = src[i + 2], d = src[i + 3];
        dst[i] = d;
        dst[i + 1] = c;
        dst[i + 2] = b;
        dst[i + 3] = a;
    }
}

static void __attribute__((noinline)) words(uint8_t *dst,
                                           const uint8_t *src, size_t len)
{
    for (size_t i = 0; i < len; i += 4) {
        stl_be_p(dst + i, ldl_le_p(src + i));
    }
}

static void __attribute__((noinline)) optimized(uint8_t *dst,
                                               const uint8_t *src, size_t len)
{
    sgi_bswap32_buf(dst, src, len);
}

static void speed(void)
{
    const size_t lengths[] = { 8, 16, 32, 64, 128, 1024, 1500, 4096, 65536 };
    uint8_t *src = g_malloc(65540), *dst = g_malloc(65540);

    struct {
        const char *name;
        void (*convert)(uint8_t *, const uint8_t *, size_t);
    } methods[] = {
        { "scalar", scalar }, { "words", words },
        { "optimized", optimized },
    };

    for (unsigned i = 0; i < 65540; i++) {
        src[i] = i;
    }
    for (unsigned size = 0; size < G_N_ELEMENTS(lengths); size++) {
        for (unsigned align = 0; align < 4; align++) {
            for (unsigned inplace = 0; inplace < 2; inplace++) {
                for (unsigned order = 0; order < G_N_ELEMENTS(methods);
                     order++) {
                    unsigned mode = (order + size + align + inplace) %
                                    G_N_ELEMENTS(methods);
                    void (*convert)(uint8_t *, const uint8_t *, size_t) =
                        methods[mode].convert;
                    uint64_t count = 0;
                    uint8_t *out = inplace ? src : dst;
                    double elapsed;

                    convert(out + align, src + align, lengths[size]);
                    for (unsigned j = 0; j < lengths[size]; j++) {
                        g_assert_cmpuint(out[align + j], ==,
                            (uint8_t)(align + (j ^ 3)));
                    }
                    if (inplace) {
                        convert(out + align, out + align, lengths[size]);
                    }
                    g_test_timer_start();
                    do {
                        for (unsigned repeat = 0; repeat < 1000; repeat++) {
                            convert(out + align, src + align, lengths[size]);
                        }
                        count += 1000;
                        elapsed = g_test_timer_elapsed();
                    } while (elapsed < 0.03);
                    g_test_message("%s order=%u length=%zu alignment=%u "
                                   "inplace=%u ns=%.2f MiB/s=%.2f",
                                   methods[mode].name, order, lengths[size],
                                   align, inplace, elapsed * 1e9 / count,
                                   lengths[size] * count / elapsed / (1 << 20));
                }
            }
        }
    }
    g_free(src);
    g_free(dst);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dma/bswap32/speed", speed);
    return g_test_run();
}
