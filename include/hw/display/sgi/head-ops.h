/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The hooks an SGI graphics head gives the shared graphics code.
 *
 * The code under hw/display/sgi/ executes what a family device has already
 * decoded -- pixel transfers, display-list fetches, scanout -- and knows no
 * family register. Everything that belongs to the family comes back to it
 * through these hooks: how a completion becomes a status flag, a credit or
 * an interrupt; how a native pixel resolves to RGB; how guest pixel bytes
 * become native words; how a device address reaches the bus; and what a
 * trace of guest bytes looks like. A hook the family leaves NULL takes the
 * default its comment names. There are no family-named switches anywhere in
 * the shared code: a difference between families is a hook or a parameter.
 */
#ifndef HW_DISPLAY_SGI_HEAD_OPS_H
#define HW_DISPLAY_SGI_HEAD_OPS_H

#include "exec/hwaddr.h"

/*
 * What a completion record says happened. One kind per record; a family
 * that needs two facts about one item gets two records, in order.
 */
typedef enum SGICompletionKind {
    SGI_DONE_CONSUMED = 1,      /* the item's words were consumed */
    SGI_DONE_WRITTEN = 2,       /* its pixels are in board or guest memory */
    SGI_DONE_QUERY_READY = 4,   /* a query's answer is ready */
    SGI_DONE_SWAPPED = 8,       /* a swap took effect at a retrace */
    SGI_DONE_FAILED = 16,       /* the item stopped; payload is SGIFailure */
} SGICompletionKind;

/* The payload of an SGI_DONE_FAILED record. */
typedef enum SGIFailure {
    SGI_FAIL_BUS = 1,           /* a guest-memory transaction failed */
    SGI_FAIL_BOUNDS = 2,        /* outside the page list, plane or pool */
    SGI_FAIL_FORMAT = 3,        /* no conversion between the two formats */
    SGI_FAIL_FULL = 4,          /* a fixed-size record queue was full */
} SGIFailure;

/*
 * One completion, published to the family in item order.
 *
 * seq is the item's position in the head's order, assigned by whoever
 * submitted it. cookie is the family's own tag for the item: which fence
 * flag, swap token, mailbox slot or bitmask it stands for, handed back
 * untouched. What each kind carries, and who publishes it:
 *
 *   CONSUMED     the number of 32-bit words the shared code read from
 *                memory and fed to the family's grammar, as a fetched
 *                display list is. These words never passed through the
 *                host command port, so they are not port or FIFO words:
 *                a family whose driver waits on credits for port words
 *                counts those itself, as its own framer takes them, and
 *                treats this record as the end of the fetched words.
 *   WRITTEN      the number of bytes a pixel transfer moved, published by
 *                SGIPixelDMA, which publishes no CONSUMED record: the
 *                words of the packet that started it are the family's.
 *   QUERY_READY  the number of answer bytes.
 *   SWAPPED      zero, published by SGIScanout at the retrace where the
 *                swap took effect.
 *   FAILED       an SGIFailure, in place of the record the item would
 *                otherwise have published.
 */
typedef struct SGICompletion {
    uint64_t seq;
    SGICompletionKind kind;
    uint64_t cookie;
    uint64_t payload;
} SGICompletion;

typedef struct SGIHeadOps {
    /*
     * A completion, under the big lock. Required by every helper that
     * completes items.
     */
    void (*publish)(void *opaque, const SGICompletion *c);

    /*
     * Scanout: display line y (0 is the top line on the monitor) as
     * width pixels of 0x00RRGGBB in dst. The family applies its own
     * resolve rules: overlay, bank select, tags, colour maps, gamma.
     * Required by SGIScanout.
     */
    void (*resolve_row)(void *opaque, unsigned y, uint32_t *dst);

    /*
     * A row of pixels between guest bytes and native plane words.
     * to_plane: src is guest bytes, dst is 'pixels' uint32_t native words.
     * Otherwise src is 'pixels' uint32_t native words and dst guest bytes.
     * conversion is the family's own constant for the transfer. NULL means
     * a guest pixel is the plane's stored bytes, in the plane's packing.
     */
    void (*convert_row)(void *opaque, uint8_t conversion, bool to_plane,
                        const uint8_t *src, uint8_t *dst, unsigned pixels);

    /*
     * A query about family state, answered at submit under the big lock.
     * Nothing in hw/display/sgi/ calls it yet: the render queue will, for
     * a query item, when it lands.
     */
    void (*answer)(void *opaque, const uint32_t *request, unsigned n,
                   uint8_t *dst, uint32_t *len);

    /*
     * The bus address at which the head's address space reaches a device
     * address, for example by composing a widget number into it. Returns
     * false, having logged why, when the address cannot be reached. NULL
     * means the device address is the bus address.
     */
    bool (*dma_address)(void *opaque, uint64_t addr, hwaddr *bus);

    /*
     * A run of guest bytes the head read, for a trace: at most
     * SGI_DMA_CHUNK_MAX bytes, at the device address before dma_address()
     * composed it (the address a replay's own guest RAM is preloaded at),
     * tagged with the sequence number the submitter gave the transfer.
     * Called synchronously while the transfer is resolved, so a family
     * that passes the sequence number of the MMIO access that started
     * the transfer gets every chunk before its next MMIO record. The
     * family owns the trace event. NULL means no trace.
     */
    void (*dma_chunk)(void *opaque, uint64_t seq, uint64_t addr,
                      const uint8_t *bytes, uint32_t len);

    /*
     * The scanout reached a vertical retrace, after publishing any swap
     * that took effect there. The family raises its retrace interrupt
     * here. NULL means the family has none.
     */
    void (*retrace)(void *opaque);
} SGIHeadOps;

static inline void sgi_head_publish(const SGIHeadOps *ops, void *opaque,
                                    uint64_t seq, SGICompletionKind kind,
                                    uint64_t cookie, uint64_t payload)
{
    SGICompletion c = {
        .seq = seq,
        .kind = kind,
        .cookie = cookie,
        .payload = payload,
    };

    ops->publish(opaque, &c);
}

#endif
