/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SGI MediaIO RAD1 model's conversions between ring frames and 16-bit
 * stereo host frames (include/hw/audio/sgi-rad1-audio-pcm.h), pinned here
 * because no QEMU audio backend can feed a qtest known capture samples:
 * the only input a qtest can bind is silence, which every placement of a
 * sample turns into the same zero word.
 *
 * The cases put a positive sample in one channel and a negative one in the
 * other, and use two frames, so a wrong bit position, a lost sign
 * extension, swapped channels and a wrong frame stride each change the
 * result. The eight-word cases are ADAT's frame; the two-word ones the
 * analog and AES3 frame; the 64- and 32-word ones the AES3-input and
 * ADAT-input subcode records, which the model fills from silence.
 */

#include "qemu/osdep.h"
#include "hw/audio/sgi-rad1-audio-pcm.h"

#define ADAT_WORDS 8
#define AES_SUB_WORDS 64
#define ADAT_SUB_WORDS 32

static void test_word_to_sample(void)
{
    g_assert_cmpint(sgi_rad1_audio_word_to_sample(0x00123456u), ==, 0x1234);
    g_assert_cmpint(sgi_rad1_audio_word_to_sample(0xffabcd12u),
                    ==, (int16_t)0xabcd);
    g_assert_cmpint(sgi_rad1_audio_word_to_sample(0xff800000u),
                    ==, (int16_t)0x8000);
    g_assert_cmpint(sgi_rad1_audio_word_to_sample(0x007fffffu), ==, 0x7fff);
}

static void test_sample_to_word(void)
{
    g_assert_cmphex(sgi_rad1_audio_sample_to_word(0x1234), ==, 0x00123400u);
    g_assert_cmphex(sgi_rad1_audio_sample_to_word((int16_t)0xabcd),
                    ==, 0xffabcd00u);
    g_assert_cmphex(sgi_rad1_audio_sample_to_word((int16_t)0x8000),
                    ==, 0xff800000u);
    g_assert_cmphex(sgi_rad1_audio_sample_to_word(0x7fff), ==, 0x007fff00u);
    g_assert_cmphex(sgi_rad1_audio_sample_to_word(-1), ==, 0xffffff00u);
    g_assert_cmphex(sgi_rad1_audio_sample_to_word(0), ==, 0);
}

/* Two ring frames of @words words: frame f, word w holds a distinct value. */
static void fill_ring(uint8_t *ring, unsigned int words)
{
    static const uint32_t left[2] = { 0x00123456u, 0xff876543u };
    static const uint32_t right[2] = { 0xffabcd12u, 0x00345678u };
    unsigned int f, w;

    for (f = 0; f < 2; f++) {
        for (w = 0; w < words; w++) {
            uint32_t v = w == 0 ? left[f] :
                         w == 1 ? right[f] : 0x00111100u * (w + 1);

            stl_le_p(ring + (f * words + w) * 4, v);
        }
    }
}

static void check_pcm(const uint8_t *pcm)
{
    g_assert_cmpint((int16_t)lduw_le_p(pcm), ==, 0x1234);
    g_assert_cmpint((int16_t)lduw_le_p(pcm + 2), ==, (int16_t)0xabcd);
    g_assert_cmpint((int16_t)lduw_le_p(pcm + 4), ==, (int16_t)0x8765);
    g_assert_cmpint((int16_t)lduw_le_p(pcm + 6), ==, 0x3456);
}

/* Output: left then right, from each frame's first two words. */
static void test_frames_to_pcm(void)
{
    uint8_t ring[2 * ADAT_WORDS * 4], pcm[8];

    fill_ring(ring, 2);
    sgi_rad1_audio_frames_to_pcm(ring, pcm, 2, 2);
    check_pcm(pcm);

    /* ADAT: channels 1 and 2 reach the host, 3 to 8 are dropped. */
    fill_ring(ring, ADAT_WORDS);
    sgi_rad1_audio_frames_to_pcm(ring, pcm, 2, ADAT_WORDS);
    check_pcm(pcm);
}

/*
 * Input: two host frames, (0x1234, -0x5433) and (-0x8000, 0x7fff), into
 * frames of @nwords words. The host pair fills words 0 and 1, the rest are
 * silence, and nothing past the two frames is written.
 */
static void check_pcm_to_frames(unsigned int nwords)
{
    static const uint32_t words[2][2] = {
        { 0x00123400u, 0xffabcd00u },
        { 0xff800000u, 0x007fff00u },
    };
    uint8_t pcm[8], ring[3 * AES_SUB_WORDS * 4];
    unsigned int f, w;

    stw_le_p(pcm, 0x1234);
    stw_le_p(pcm + 2, 0xabcd);
    stw_le_p(pcm + 4, 0x8000);
    stw_le_p(pcm + 6, 0x7fff);

    memset(ring, 0xa5, sizeof(ring));
    sgi_rad1_audio_pcm_to_frames(pcm, ring, 2, nwords);
    for (f = 0; f < 2; f++) {
        for (w = 0; w < nwords; w++) {
            uint32_t want = w < 2 ? words[f][w] : 0;

            g_assert_cmphex((uint32_t)ldl_le_p(ring + (f * nwords + w) * 4),
                            ==, want);
        }
    }
    g_assert_cmphex((uint32_t)ldl_le_p(ring + 2 * nwords * 4),
                    ==, 0xa5a5a5a5u);
}

static void test_pcm_to_frames(void)
{
    check_pcm_to_frames(2);
    /* ADAT: channels 1 and 2 from the host, 3 to 8 silence. */
    check_pcm_to_frames(ADAT_WORDS);
    check_pcm_to_frames(ADAT_SUB_WORDS);
    check_pcm_to_frames(AES_SUB_WORDS);
}

/* A recorded frame plays back unchanged. */
static void test_round_trip(void)
{
    uint8_t pcm[8], ring[2 * ADAT_WORDS * 4], back[8];

    stw_le_p(pcm, 0x1234);
    stw_le_p(pcm + 2, 0xabcd);
    stw_le_p(pcm + 4, 0x8000);
    stw_le_p(pcm + 6, 0x7fff);
    sgi_rad1_audio_pcm_to_frames(pcm, ring, 2, ADAT_WORDS);
    sgi_rad1_audio_frames_to_pcm(ring, back, 2, ADAT_WORDS);

    g_assert_cmpmem(back, sizeof(back), pcm, sizeof(pcm));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-rad1-audio/pcm/word-to-sample", test_word_to_sample);
    g_test_add_func("/sgi-rad1-audio/pcm/sample-to-word", test_sample_to_word);
    g_test_add_func("/sgi-rad1-audio/pcm/frames-to-pcm", test_frames_to_pcm);
    g_test_add_func("/sgi-rad1-audio/pcm/pcm-to-frames", test_pcm_to_frames);
    g_test_add_func("/sgi-rad1-audio/pcm/round-trip", test_round_trip);
    return g_test_run();
}
