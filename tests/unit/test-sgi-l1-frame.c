/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1-frame.h"

/* A request on the wire: flags, no PPP prefix, FCS complemented, low first. */
static const uint8_t identity[] = {
    0x7e, 0x02, 0x0f, 0xff, 0xf0, 0x06, 0x00, 0x03, 0x00,
    0x96, 0x92, 0x7e,
};

/* Payload bytes that need escaping: both flags and a carriage return. */
static const uint8_t awkward[] = { 0x7e, 0x7d, 0x0d, 0x00, 0x41, 0x7e };

static unsigned encode(uint8_t *out, const uint8_t *payload, unsigned len,
                       bool escape_cr)
{
    return sgi_l1_frame_encode(out, payload, len, escape_cr);
}

/* Feed bytes[1..n) and report whether the last one completed a frame. */
static bool feed(SGIL1FrameState *f, unsigned capacity, const uint8_t *bytes,
                 unsigned n)
{
    bool done = false;

    for (unsigned i = 0; i < n; i++) {
        g_assert_false(done);
        done = sgi_l1_frame_parse(f, capacity, bytes[i]);
    }
    return done;
}

static void test_encode_vector(void)
{
    static const uint8_t payload[] = { 2, 15, 255, 240, 6, 0, 3, 0 };
    uint8_t out[2 * SGI_L1_FRAME_SIZE + 2];
    unsigned n = encode(out, payload, sizeof(payload), true);

    g_assert_cmpmem(out, n, identity, sizeof(identity));
}

static void test_round_trip(void)
{
    for (unsigned cr = 0; cr < 2; cr++) {
        uint8_t out[2 * SGI_L1_FRAME_SIZE + 2];
        unsigned n = encode(out, awkward, sizeof(awkward), cr);
        unsigned raw_cr = 0;
        SGIL1FrameState f = { 0 };

        for (unsigned i = 1; i < n - 1; i++) {
            g_assert_cmpuint(out[i], !=, 0x7e);
            raw_cr += out[i] == 0x0d;
        }
        /* Only the multiplexed link escapes CR. */
        g_assert_cmpuint(raw_cr, ==, cr ? 0 : 1);

        g_assert_true(feed(&f, SGI_L1_FRAME_SIZE, out, n));
        g_assert_cmpuint(f.length, ==, sizeof(awkward) + 2);
        g_assert_cmpmem(f.data, sizeof(awkward), awkward, sizeof(awkward));
    }
}

/* The closing flag also opens the next frame; a bad check loses only one. */
static void test_recovery(void)
{
    uint8_t bad[sizeof(identity)];
    SGIL1FrameState f = { 0 };

    memcpy(bad, identity, sizeof(bad));
    bad[9] ^= 1;
    g_assert_false(feed(&f, SGI_L1_FRAME_SIZE, bad, sizeof(bad)));
    g_assert_true(feed(&f, SGI_L1_FRAME_SIZE, identity + 1,
                       sizeof(identity) - 1));
    g_assert_cmpuint(f.length, ==, 8 + 2);
    sgi_l1_frame_restart(&f);
    g_assert_true(feed(&f, SGI_L1_FRAME_SIZE, identity + 1,
                       sizeof(identity) - 1));
}

static void test_capacity(void)
{
    uint8_t out[2 * SGI_L1_FRAME_SIZE + 2];
    SGIL1FrameState f = { 0 };
    unsigned n = encode(out, awkward, 6, true);

    /* Six payload bytes and two check bytes fit eight; seven do not. */
    g_assert_true(feed(&f, 8, out, n));
    sgi_l1_frame_restart(&f);
    n = encode(out, awkward, 6, true);
    g_assert_false(feed(&f, 7, out, n));
}

static void test_arguments(void)
{
    static const uint8_t two[] = { 0, 0, 0, 0, 1, 1, 'a', 0 };
    static const uint8_t indirect[] = { 0x82, 9, 9 };

    g_assert_true(sgi_l1_arguments_valid(two, sizeof(two), 2));
    g_assert_false(sgi_l1_arguments_valid(two, sizeof(two) - 1, 2));
    g_assert_false(sgi_l1_arguments_valid(two, sizeof(two), 3));
    g_assert_true(sgi_l1_arguments_valid(indirect, sizeof(indirect), 1));
    g_assert_false(sgi_l1_arguments_valid(indirect, sizeof(indirect) - 1, 1));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-l1-frame/encode-vector", test_encode_vector);
    g_test_add_func("/sgi-l1-frame/round-trip", test_round_trip);
    g_test_add_func("/sgi-l1-frame/recovery", test_recovery);
    g_test_add_func("/sgi-l1-frame/capacity", test_capacity);
    g_test_add_func("/sgi-l1-frame/arguments", test_arguments);
    return g_test_run();
}
