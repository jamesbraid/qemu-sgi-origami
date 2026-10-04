/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-dma-buffer.h"
#include "host/cpuinfo.h"

#ifdef CPUINFO_SSE2
#include <immintrin.h>
#endif

typedef void (*BSwap32Buffer)(void *, const void *, size_t);

static void bswap32_words(void *dst, const void *src, size_t len)
{
    uint8_t *out = dst;
    const uint8_t *in = src;

    for (size_t i = 0; i < len; i += 4) {
        stl_be_p(out + i, ldl_le_p(in + i));
    }
}

#ifdef CPUINFO_SSE2
static void __attribute__((target("sse2")))
bswap32_sse2(void *dst, const void *src, size_t len)
{
    uint8_t *out = dst;
    const uint8_t *in = src;

    while (len >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)in);

        v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
        v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
        v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
        _mm_storeu_si128((__m128i *)out, v);
        out += 16;
        in += 16;
        len -= 16;
    }
    bswap32_words(out, in, len);
}

#ifdef CONFIG_AVX2_OPT
static void __attribute__((target("avx2")))
bswap32_avx2(void *dst, const void *src, size_t len)
{
    const __m256i order = _mm256_setr_epi8(
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
        3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    uint8_t *out = dst;
    const uint8_t *in = src;

    while (len >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)in);

        _mm256_storeu_si256((__m256i *)out, _mm256_shuffle_epi8(v, order));
        out += 32;
        in += 32;
        len -= 32;
    }
    bswap32_words(out, in, len);
}
#endif
#endif

static BSwap32Buffer bswap32_buffer = bswap32_words;

void sgi_bswap32_buf_ool(void *dst, const void *src, size_t len)
{
    assert(!(len & 3));
    bswap32_buffer(dst, src, len);
}

#ifdef CPUINFO_SSE2
static void __attribute__((constructor)) bswap32_init(void)
{
    unsigned info = cpuinfo_init();

    if (info & CPUINFO_SSE2) {
        bswap32_buffer = bswap32_sse2;
    }
#ifdef CONFIG_AVX2_OPT
    if (info & CPUINFO_AVX2) {
        bswap32_buffer = bswap32_avx2;
    }
#endif
}
#endif
