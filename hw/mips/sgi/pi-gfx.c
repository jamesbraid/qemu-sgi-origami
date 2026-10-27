/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Graphics flow control in the Hub and Bedrock processor interfaces: the
 * register block's rules. See the header for the contract.
 *
 * This file holds no QOM or migration code so a host unit test can link
 * it alone; pi-gfx-state.c carries the interface type and the state
 * description.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/mips/sgi/pi-gfx.h"

/* The counter above the bias re-arms the slot's graphics interrupt. */
static void sgi_pi_gfx_rearm(SGIPIGfx *g, unsigned int slot)
{
    if (g->credit[slot] > g->bias[slot]) {
        g->starved[slot] = false;
    }
}

void sgi_pi_gfx_reset(SGIPIGfx *g)
{
    memset(g, 0, sizeof(*g));
}

uint64_t sgi_pi_gfx_read(const SGIPIGfx *g, unsigned int n)
{
    unsigned int slot = n / SGI_PI_GFX_SLOT_REGS;

    g_assert(n < SGI_PI_GFX_REGS);
    switch (n % SGI_PI_GFX_SLOT_REGS) {
    case SGI_PI_GFX_PAGE:
        return g->page[slot];
    case SGI_PI_GFX_CREDIT:
        return g->credit[slot];
    case SGI_PI_GFX_BIAS:
        return g->bias[slot];
    case SGI_PI_GFX_INT_COUNT:
        return g->int_count[slot];
    default:
        return g->int_compare[slot];
    }
}

void sgi_pi_gfx_write(SGIPIGfx *g, unsigned int n, uint64_t value)
{
    unsigned int slot = n / SGI_PI_GFX_SLOT_REGS;

    g_assert(n < SGI_PI_GFX_REGS);
    switch (n % SGI_PI_GFX_SLOT_REGS) {
    case SGI_PI_GFX_PAGE:
        g->page[slot] = value & SGI_PI_GFX_PAGE_MASK;
        break;
    case SGI_PI_GFX_CREDIT:
        g->credit[slot] = value & SGI_PI_GFX_CREDIT_MASK;
        sgi_pi_gfx_rearm(g, slot);
        break;
    case SGI_PI_GFX_BIAS:
        g->bias[slot] = value & SGI_PI_GFX_BIAS_MASK;
        break;
    case SGI_PI_GFX_INT_COUNT:
        g->int_count[slot] = value & SGI_PI_GFX_INT_MASK;
        break;
    default:
        g->int_compare[slot] = value & SGI_PI_GFX_INT_MASK;
        break;
    }
}

bool sgi_pi_gfx_covers(const SGIPIGfx *g, unsigned int slot, hwaddr addr)
{
    uint64_t page;

    g_assert(slot < SGI_PI_GFX_SLOTS);
    page = g->page[slot];
    return (page & SGI_PI_GFX_PAGE_ENABLE) &&
           (addr & SGI_PI_GFX_PAGE_ADDRESS) == (page & SGI_PI_GFX_PAGE_ADDRESS);
}

bool sgi_pi_gfx_count_store(SGIPIGfx *g, unsigned int slot, hwaddr addr,
                            const char *who)
{
    if (!sgi_pi_gfx_covers(g, slot, addr)) {
        return false;
    }

    /*
     * Whether the twelve-bit counter wraps, saturates or goes negative is
     * unknown. It wraps, and says so: a driver that over-issues sees a
     * count above its total and tears the pipe down, as it would on any
     * of those.
     */
    if (!g->credit[slot]) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: graphics credit counter %c "
                      "wrapped below zero\n", who, 'A' + slot);
    }
    g->credit[slot] = (g->credit[slot] - 1) & SGI_PI_GFX_CREDIT_MASK;

    return true;
}

/*
 * Below the bias the processor interface holds the processor's writes,
 * sends the graphics interrupt, then lets the writes go on: the SN1
 * header's credit-counter comment, inferred for SN0. Whether the edge is
 * below the bias or at it is unknown; the model takes below, the stall
 * comment's reading. The stall is zero length here: a vCPU held inside a
 * store takes no timer interrupt, and IRIX's handler puts the storing
 * thread to sleep instead. The interrupt counter and compare, which on
 * the chip delay the interrupt by an unknown clock, are not modelled.
 * The interrupt is raised as the store is counted, before the head the
 * store reaches can return its credit within the same store; that only
 * matters when the counter sits exactly at the bias.
 */
bool sgi_pi_gfx_take_starved(SGIPIGfx *g, unsigned int slot,
                             const char *who)
{
    g_assert(slot < SGI_PI_GFX_SLOTS);
    if (g->starved[slot] || g->credit[slot] >= g->bias[slot]) {
        return false;
    }
    g->starved[slot] = true;
    if (!g->stall_logged[slot]) {
        g->stall_logged[slot] = true;
        qemu_log_mask(LOG_UNIMP, "%s: graphics credit counter %c fell below "
                      "its bias; interrupt raised at once, the write stall "
                      "and the interrupt counter are not modelled\n", who,
                      'A' + slot);
    }
    return true;
}

/* Ticks from the count to the compare: a whole turn when they are equal. */
static uint64_t sgi_pi_gfx_int_left(const SGIPIGfx *g, unsigned int slot)
{
    uint32_t left = (g->int_compare[slot] - g->int_count[slot]) &
                    SGI_PI_GFX_INT_MASK;

    return left ? left : (uint64_t)SGI_PI_GFX_INT_MASK + 1;
}

bool sgi_pi_gfx_int_advance(SGIPIGfx *g, unsigned int slot, uint64_t now)
{
    bool fired = false;

    g_assert(slot < SGI_PI_GFX_SLOTS);
    if (g->int_running[slot] && now > g->int_epoch[slot]) {
        uint64_t elapsed = now - g->int_epoch[slot];

        if (elapsed >= sgi_pi_gfx_int_left(g, slot)) {
            g->int_count[slot] = g->int_compare[slot];
            fired = true;
        } else {
            g->int_count[slot] = (g->int_count[slot] + elapsed) &
                                 SGI_PI_GFX_INT_MASK;
        }
    }
    g->int_epoch[slot] = now;
    /*
     * At or below zero, as the header's counter comment has it; the
     * twelve-bit counter is never below. Whether the chip also asks for
     * the counter to be below the bias is unknown. The model asks for
     * both, its choice, so a slot whose bias is zero, as every slot is out
     * of reset and after IRIX detaches, sends nothing.
     */
    g->int_running[slot] = g->credit[slot] == 0 && g->bias[slot] > 0;
    return fired;
}

uint64_t sgi_pi_gfx_int_due(const SGIPIGfx *g, unsigned int slot)
{
    g_assert(slot < SGI_PI_GFX_SLOTS);
    if (!g->int_running[slot]) {
        return UINT64_MAX;
    }
    return g->int_epoch[slot] + sgi_pi_gfx_int_left(g, slot);
}

/*
 * The mark on the store a processor path is forwarding, per host thread:
 * the forward and the head's handler run on the issuing vCPU's thread.
 */
static __thread bool sgi_pi_gfx_forwarding_counted;

bool sgi_pi_gfx_mark_store(bool counted)
{
    bool previous = sgi_pi_gfx_forwarding_counted;

    sgi_pi_gfx_forwarding_counted = counted;
    return previous;
}

void sgi_pi_gfx_unmark_store(bool previous)
{
    sgi_pi_gfx_forwarding_counted = previous;
}

bool sgi_pi_gfx_take_mark(void)
{
    bool counted = sgi_pi_gfx_forwarding_counted;

    sgi_pi_gfx_forwarding_counted = false;
    return counted;
}

/* The node sending the store being forwarded, per host thread as above. */
static __thread Object *sgi_pi_gfx_forwarding_source;

Object *sgi_pi_gfx_mark_source(Object *node)
{
    Object *previous = sgi_pi_gfx_forwarding_source;

    sgi_pi_gfx_forwarding_source = node;
    return previous;
}

Object *sgi_pi_gfx_store_source(void)
{
    return sgi_pi_gfx_forwarding_source;
}

void sgi_pi_gfx_add_credits(SGIPIGfx *g, unsigned int slot, uint32_t credits,
                            const char *who)
{
    uint64_t sum;

    g_assert(slot < SGI_PI_GFX_SLOTS);
    sum = (uint64_t)g->credit[slot] + credits;
    if (sum > SGI_PI_GFX_CREDIT_MASK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: graphics credit counter %c "
                      "wrapped above 0x%x\n", who, 'A' + slot,
                      SGI_PI_GFX_CREDIT_MASK);
    }
    g->credit[slot] = sum & SGI_PI_GFX_CREDIT_MASK;
    sgi_pi_gfx_rearm(g, slot);
}

/*
 * The Hub's map: widget 3:0, node 12:4, processor 16, valid 20 (Linux
 * sn0/hubio.h, IIO_IGFX_INIT). The first matching entry wins; IRIX never
 * programs both alike.
 */
bool sgi_pi_gfx_route_hub(const uint64_t map[2], unsigned int widget,
                          SGIPIGfxRoute *route)
{
    for (unsigned int i = 0; i < 2; i++) {
        uint64_t entry = map[i];

        if (!(entry & (1ULL << 20)) || (entry & 0xf) != widget) {
            continue;
        }
        route->node = (entry >> 4) & 0x1ff;
        route->pi = 0;
        route->slot = (entry >> 16) & 1;
        return true;
    }
    return false;
}

/*
 * The Bedrock's map: widget 3:0, PI 4, node 12:5, processor 16 (Linux
 * sn1/hubio_next.h, IIO_IGFX_INIT). There is no valid bit, so an entry
 * of zero names widget 0 on node 0.
 */
bool sgi_pi_gfx_route_bedrock(const uint64_t map[2], unsigned int widget,
                              SGIPIGfxRoute *route)
{
    for (unsigned int i = 0; i < 2; i++) {
        uint64_t entry = map[i];

        if ((entry & 0xf) != widget) {
            continue;
        }
        route->node = (entry >> 5) & 0xff;
        route->pi = (entry >> 4) & 1;
        route->slot = (entry >> 16) & 1;
        return true;
    }
    return false;
}
