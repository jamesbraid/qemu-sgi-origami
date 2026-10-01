/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The processor interface's graphics credit block, without a device: the
 * field widths, the page match, the count and return, the wrap, and the
 * two node-widget map layouts.
 */
#include "qemu/osdep.h"
#include "hw/mips/sgi/pi-gfx.h"

#define PAGE        0x20002120000ULL        /* bits 39:17 of an I/O store */
#define REG(slot, r) ((slot) * SGI_PI_GFX_SLOT_REGS + (r))

static void test_widths(void)
{
    SGIPIGfx g;

    sgi_pi_gfx_reset(&g);
    for (unsigned int n = 0; n < SGI_PI_GFX_REGS; n++) {
        g_assert_cmphex(sgi_pi_gfx_read(&g, n), ==, 0);
    }
    for (unsigned int slot = 0; slot < SGI_PI_GFX_SLOTS; slot++) {
        sgi_pi_gfx_write(&g, REG(slot, SGI_PI_GFX_PAGE), UINT64_MAX);
        sgi_pi_gfx_write(&g, REG(slot, SGI_PI_GFX_CREDIT), UINT64_MAX);
        sgi_pi_gfx_write(&g, REG(slot, SGI_PI_GFX_BIAS), UINT64_MAX);
        sgi_pi_gfx_write(&g, REG(slot, SGI_PI_GFX_INT_COUNT), UINT64_MAX);
        sgi_pi_gfx_write(&g, REG(slot, SGI_PI_GFX_INT_COMPARE), UINT64_MAX);
        g_assert_cmphex(sgi_pi_gfx_read(&g, REG(slot, SGI_PI_GFX_PAGE)), ==,
                        0x1fffffe0000ULL);
        g_assert_cmphex(sgi_pi_gfx_read(&g, REG(slot, SGI_PI_GFX_CREDIT)), ==,
                        0xfff);
        g_assert_cmphex(sgi_pi_gfx_read(&g, REG(slot, SGI_PI_GFX_BIAS)), ==,
                        0xfff);
        g_assert_cmphex(sgi_pi_gfx_read(&g, REG(slot, SGI_PI_GFX_INT_COUNT)),
                        ==, 0x3ffffff);
        g_assert_cmphex(sgi_pi_gfx_read(&g,
                                        REG(slot, SGI_PI_GFX_INT_COMPARE)),
                        ==, 0x3ffffff);
    }
    sgi_pi_gfx_reset(&g);
    for (unsigned int n = 0; n < SGI_PI_GFX_REGS; n++) {
        g_assert_cmphex(sgi_pi_gfx_read(&g, n), ==, 0);
    }
}

static void test_count_and_return(void)
{
    SGIPIGfx g;

    sgi_pi_gfx_reset(&g);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_PAGE),
                     SGI_PI_GFX_PAGE_ENABLE | PAGE);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_CREDIT), 100);
    sgi_pi_gfx_write(&g, REG(1, SGI_PI_GFX_CREDIT), 100);

    /* Anywhere in the 128 KB page counts, on the storing slot only. */
    g_assert_true(sgi_pi_gfx_count_store(&g, 0, PAGE, "test"));
    g_assert_true(sgi_pi_gfx_count_store(&g, 0, PAGE + 0x1fff8, "test"));
    g_assert_cmpuint(g.credit[0], ==, 98);
    g_assert_cmpuint(g.credit[1], ==, 100);

    /* The next page, the page below, another slot: nothing. */
    g_assert_false(sgi_pi_gfx_count_store(&g, 0, PAGE + 0x20000, "test"));
    g_assert_false(sgi_pi_gfx_count_store(&g, 0, PAGE - 8, "test"));
    g_assert_false(sgi_pi_gfx_count_store(&g, 1, PAGE, "test"));
    g_assert_cmpuint(g.credit[0], ==, 98);
    g_assert_cmpuint(g.credit[1], ==, 100);

    /* A disabled page counts nothing, even at its own address. */
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_PAGE), PAGE);
    g_assert_false(sgi_pi_gfx_count_store(&g, 0, PAGE, "test"));
    g_assert_cmpuint(g.credit[0], ==, 98);

    /* Returns add; a write replaces the count and returns add to that. */
    sgi_pi_gfx_add_credits(&g, 0, 2, "test");
    g_assert_cmpuint(g.credit[0], ==, 100);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_CREDIT), 50);
    sgi_pi_gfx_add_credits(&g, 0, 1, "test");
    g_assert_cmpuint(g.credit[0], ==, 51);
}

static void test_wrap(void)
{
    SGIPIGfx g;

    sgi_pi_gfx_reset(&g);
    sgi_pi_gfx_write(&g, REG(1, SGI_PI_GFX_PAGE),
                     SGI_PI_GFX_PAGE_ENABLE | PAGE);
    g_assert_true(sgi_pi_gfx_count_store(&g, 1, PAGE, "test"));
    g_assert_cmphex(g.credit[1], ==, 0xfff);
    sgi_pi_gfx_add_credits(&g, 1, 2, "test");
    g_assert_cmphex(g.credit[1], ==, 1);
    /* Never clamped at anything the driver chose. */
    sgi_pi_gfx_add_credits(&g, 1, 0x1000, "test");
    g_assert_cmphex(g.credit[1], ==, 1);
}

static void test_stall_logged_once(void)
{
    SGIPIGfx g;

    sgi_pi_gfx_reset(&g);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_PAGE),
                     SGI_PI_GFX_PAGE_ENABLE | PAGE);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_CREDIT), 3);
    sgi_pi_gfx_write(&g, REG(0, SGI_PI_GFX_BIAS), 2);
    g_assert_true(sgi_pi_gfx_count_store(&g, 0, PAGE, "test"));
    g_assert_false(g.stall_logged[0]);
    g_assert_true(sgi_pi_gfx_count_store(&g, 0, PAGE, "test"));
    g_assert_true(g.stall_logged[0]);
    g_assert_false(g.stall_logged[1]);
    sgi_pi_gfx_reset(&g);
    g_assert_false(g.stall_logged[0]);
}

/* A head sees the mark only while a counted store is being forwarded. */
static void test_store_mark(void)
{
    bool outer, inner;

    g_assert_false(sgi_pi_gfx_take_mark());
    outer = sgi_pi_gfx_mark_store(true);
    g_assert_false(outer);
    /* A store the forward itself makes nests, and restores the mark. */
    inner = sgi_pi_gfx_mark_store(false);
    g_assert_false(sgi_pi_gfx_take_mark());
    sgi_pi_gfx_unmark_store(inner);
    g_assert_true(sgi_pi_gfx_take_mark());
    sgi_pi_gfx_unmark_store(outer);
    g_assert_false(sgi_pi_gfx_take_mark());
}

/*
 * One counted store is seen once: a store split into two callbacks, or a
 * store the handler makes after reading the mark, sees false.
 */
static void test_store_mark_is_one_shot(void)
{
    bool previous = sgi_pi_gfx_mark_store(true);

    g_assert_true(sgi_pi_gfx_take_mark());
    g_assert_false(sgi_pi_gfx_take_mark());
    sgi_pi_gfx_unmark_store(previous);
    g_assert_false(sgi_pi_gfx_take_mark());
}

static void test_route_hub(void)
{
    /* valid, slot B, node 0x123, widget 0xc */
    uint64_t map[2] = { 0, (1 << 20) | (1 << 16) | (0x123 << 4) | 0xc };
    SGIPIGfxRoute route;

    g_assert_true(sgi_pi_gfx_route_hub(map, 0xc, &route));
    g_assert_cmpuint(route.node, ==, 0x123);
    g_assert_cmpuint(route.pi, ==, 0);
    g_assert_cmpuint(route.slot, ==, 1);
    g_assert_false(sgi_pi_gfx_route_hub(map, 0xb, &route));

    /* Without the valid bit an entry routes nothing. */
    map[1] &= ~(1ULL << 20);
    g_assert_false(sgi_pi_gfx_route_hub(map, 0xc, &route));

    /* Two entries for one widget: the first wins. */
    map[0] = (1 << 20) | (5 << 4) | 0xc;
    map[1] = (1 << 20) | (1 << 16) | (6 << 4) | 0xc;
    g_assert_true(sgi_pi_gfx_route_hub(map, 0xc, &route));
    g_assert_cmpuint(route.node, ==, 5);
    g_assert_cmpuint(route.slot, ==, 0);
}

static void test_route_bedrock(void)
{
    /* slot B, node 0x81, PI1, widget 0xb */
    uint64_t map[2] = { (1 << 16) | (0x81 << 5) | (1 << 4) | 0xb, 0 };
    SGIPIGfxRoute route;

    g_assert_true(sgi_pi_gfx_route_bedrock(map, 0xb, &route));
    g_assert_cmpuint(route.node, ==, 0x81);
    g_assert_cmpuint(route.pi, ==, 1);
    g_assert_cmpuint(route.slot, ==, 1);
    g_assert_false(sgi_pi_gfx_route_bedrock(map, 0xa, &route));

    /* No valid bit: a zeroed entry names widget 0, node 0, PI0, slot A. */
    g_assert_true(sgi_pi_gfx_route_bedrock(map, 0, &route));
    g_assert_cmpuint(route.node, ==, 0);
    g_assert_cmpuint(route.pi, ==, 0);
    g_assert_cmpuint(route.slot, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-pi-gfx/widths", test_widths);
    g_test_add_func("/sgi-pi-gfx/count-and-return", test_count_and_return);
    g_test_add_func("/sgi-pi-gfx/wrap", test_wrap);
    g_test_add_func("/sgi-pi-gfx/stall-logged-once", test_stall_logged_once);
    g_test_add_func("/sgi-pi-gfx/store-mark", test_store_mark);
    g_test_add_func("/sgi-pi-gfx/store-mark-is-one-shot",
                    test_store_mark_is_one_shot);
    g_test_add_func("/sgi-pi-gfx/route-hub", test_route_hub);
    g_test_add_func("/sgi-pi-gfx/route-bedrock", test_route_bedrock);
    return g_test_run();
}
