/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Graphics flow control in the Hub and Bedrock processor interfaces.
 *
 * Each processor interface carries ten graphics registers, five for each of
 * its two processor slots: a page, a credit counter, a bias, an interrupt
 * counter and an interrupt compare. A processor's uncached I/O store into
 * its slot's enabled page costs one credit; a graphics widget returns
 * credits, which the node's I/O interface routes to a slot through its
 * graphics node-widget map (Linux sn0/hubpi.h and sn1/hubpi.h,
 * PI_GFX_*; sn0/hubio.h and sn1/hubio_next.h, IIO_IGFX_INIT).
 *
 * SGIPIGfx is the register block one interface embeds. The functions on it
 * are pure state: the device decides which slot a store came from and which
 * slot a return reaches, and hands the block the result.
 *
 * TYPE_SGI_PI_GFX is the interface a node device (the Hub, or the Bedrock
 * composite) implements for the graphics heads behind it. A head holds a
 * "pi" link to its master node and returns credits through
 * sgi_pi_gfx_return_credits(); it takes no part in the count.
 */

#ifndef HW_MIPS_SGI_PI_GFX_H
#define HW_MIPS_SGI_PI_GFX_H

#include "qom/object.h"
#include "exec/hwaddr.h"

#define SGI_PI_GFX_SLOTS        2
/* The block's first register and its extent in the interface's window. */
#define SGI_PI_GFX_BASE         0x300
#define SGI_PI_GFX_SLOT_STRIDE  0x28
#define SGI_PI_GFX_REGS         10

/* The five registers of one slot, in address order. */
enum {
    SGI_PI_GFX_PAGE,
    SGI_PI_GFX_CREDIT,
    SGI_PI_GFX_BIAS,
    SGI_PI_GFX_INT_COUNT,
    SGI_PI_GFX_INT_COMPARE,
    SGI_PI_GFX_SLOT_REGS,
};

/* Page: bit 40 enables it, bits 39:17 name it; the rest is reserved. */
#define SGI_PI_GFX_PAGE_ENABLE  (1ULL << 40)
#define SGI_PI_GFX_PAGE_ADDRESS 0xfffffe0000ULL
#define SGI_PI_GFX_PAGE_MASK    (SGI_PI_GFX_PAGE_ENABLE | \
                                 SGI_PI_GFX_PAGE_ADDRESS)
#define SGI_PI_GFX_CREDIT_MASK  0xfffU
#define SGI_PI_GFX_BIAS_MASK    0xfffU
#define SGI_PI_GFX_INT_MASK     0x3ffffffU

typedef struct SGIPIGfx {
    uint64_t page[SGI_PI_GFX_SLOTS];
    uint32_t credit[SGI_PI_GFX_SLOTS];
    uint32_t bias[SGI_PI_GFX_SLOTS];
    uint32_t int_count[SGI_PI_GFX_SLOTS];
    uint32_t int_compare[SGI_PI_GFX_SLOTS];
    /* Host-only: the unmodelled stall has been logged for this slot. */
    bool stall_logged[SGI_PI_GFX_SLOTS];
} SGIPIGfx;

/* Every register zero, every page disabled (the model's choice). */
void sgi_pi_gfx_reset(SGIPIGfx *g);

/*
 * Register n of the block, 0 to SGI_PI_GFX_REGS - 1, counted in doublewords
 * from SGI_PI_GFX_BASE. A write keeps the field's bits; a write to the
 * credit counter replaces the count.
 */
uint64_t sgi_pi_gfx_read(const SGIPIGfx *g, unsigned int n);
void sgi_pi_gfx_write(SGIPIGfx *g, unsigned int n, uint64_t value);

/*
 * An uncached I/O store by the processor in 'slot' reached physical
 * address bits 39:0 'addr'. Charges one credit when the slot's page is
 * enabled and covers it, and returns whether it did. 'who' names the
 * device in log messages.
 */
bool sgi_pi_gfx_count_store(SGIPIGfx *g, unsigned int slot, hwaddr addr,
                            const char *who);

/*
 * Whether the processor store now being forwarded was counted. A processor
 * interface marks each store it forwards down its I/O path, and restores
 * the previous mark when the store returns.
 *
 * A graphics head asks from its port's write handler, on the issuing
 * vCPU's thread, and returns credits only for stores that were counted.
 * The answer is one-shot: the call clears the mark, so exactly one handler
 * call per counted store sees true, however the memory core splits the
 * store into narrower callbacks and whatever the handler stores in turn.
 * A head that queues the word samples the mark in the handler and carries
 * it with the item. Called with no current vCPU (a qtest access, a device's
 * DMA, a bottom half or another thread) it answers false and logs a guest
 * error once.
 *
 * sgi_pi_gfx_take_mark() is the one-shot read without that check.
 */
bool sgi_pi_gfx_mark_store(bool counted);
void sgi_pi_gfx_unmark_store(bool previous);
bool sgi_pi_gfx_take_mark(void);
bool sgi_pi_gfx_store_counted(void);

/* 'credits' credits returned to 'slot'. */
void sgi_pi_gfx_add_credits(SGIPIGfx *g, unsigned int slot, uint32_t credits,
                            const char *who);

/*
 * Where a graphics node-widget map sends a widget's credits. The two chips
 * lay the map out differently: the Hub has a valid bit and a nine-bit node,
 * the Bedrock a PI bit, an eight-bit node and no valid bit.
 */
typedef struct SGIPIGfxRoute {
    unsigned int node;
    unsigned int pi;
    unsigned int slot;
} SGIPIGfxRoute;

bool sgi_pi_gfx_route_hub(const uint64_t map[2], unsigned int widget,
                          SGIPIGfxRoute *route);
bool sgi_pi_gfx_route_bedrock(const uint64_t map[2], unsigned int widget,
                              SGIPIGfxRoute *route);

extern const VMStateDescription vmstate_sgi_pi_gfx;

#define TYPE_SGI_PI_GFX "sgi-pi-gfx"
typedef struct SGIPIGfxClass SGIPIGfxClass;
DECLARE_CLASS_CHECKERS(SGIPIGfxClass, SGI_PI_GFX, TYPE_SGI_PI_GFX)

struct SGIPIGfxClass {
    InterfaceClass parent_class;
    void (*return_credits)(Object *node, unsigned int widget,
                           uint32_t credits);
};

/*
 * A graphics head took 'credits' counted stores off its port (those for
 * which sgi_pi_gfx_store_counted() was true). 'node' is the head's
 * "pi" link, its master node. 'widget' is the head's crosstalk widget
 * number, the value the driver writes into the node-widget map.
 */
void sgi_pi_gfx_return_credits(Object *node, unsigned int widget,
                               uint32_t credits);

#endif
