/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * InfiniteReality/KONA's DMA engine, on the shared pixel DMA.
 *
 * The packet, established by a full reading of kona_idma and
 * kona_dmawrite: the selector 0x1eb, then six words --
 *
 *   1  io >> 12
 *   2  (io & 0xfff) | 0x9000
 *   3  the descriptor's total byte count
 *   4  its per-line byte count
 *   5  its gap, the bytes between one line's end and the next line
 *   6  0x100 when the sender wants a completion, else 0
 *
 * -- where io is the device address of the pinned buffer. The kick
 * register takes the literals 0x30000, 0x10000 and 0 around the packet as
 * a bracket; it is not a word count, so this engine frames on the
 * selector and ignores the kick. Before the packet, kona_dmawrite writes
 * its command buffer's records to the same port: 0x1c8 with a pipe-memory
 * address and a byte count, when a display list is copied into pipe
 * memory. A record says
 * where the next packet's bytes go, so the engine holds it and places
 * that packet's transfer in the list plane. With no record, a packet
 * that arrives while an X pixel write opened under its DMA mode bit waits
 * for its rows is the X server's image: its bytes, read through the XG
 * map RAM, feed that write (sgi_kona_x_image_dma()). With neither, the
 * packet is logged without transferring bytes or posting completion.
 *
 * Every transfer is an SGIPixelDMA (hw/display/sgi/pixel-dma.c) against
 * the board memory, so the shared layer resolves the guest pages, bounds
 * the transfer by its plane and page list, and publishes its completion.
 * The completion's cookie comes from the packet's word 6: kona_dmawrite
 * sleeps after each packet until the mailbox delivers 0x100 under cause
 * bit 0x10. That the pipe posts the packet's own last word is inferred.
 *
 * Approximated, and kept from the engine this replaces:
 *
 *   - The push register at +0x80007c. The kernel FIFO port's placement
 *     is unresolved: the driver's +0x7c could name three places.
 *   - The synthetic framebuffer transfer requires the host-only
 *     x-test-dma-framebuffer opt-in, disabled by default. Its destination
 *     x-test-dma-fb-offset counts from the top display line's left end,
 *     one source line per display line going down. No guest command
 *     enables this test interface.
 *   - The reverse direction, framebuffer to guest, which no guest sends:
 *     x-test-dma-reverse, qtest only, reading pixels resolved by their
 *     tags as the scanout shows them.
 *   - A pipe-memory address is a byte offset into the list plane. How
 *     the pipe places the display-list region the kernel reserves is
 *     unknown, so the model's list plane is that region, from offset 0.
 *   - The destination widget: the packet names none, so SGIKonaState's
 *     fabric_hub_widget names one fixed widget (see that struct's
 *     comment).
 *
 * The packet and the record reach the engine two ways: on the push
 * register, and on the +0xc10000 command page, where the traced guest
 * run shows the kernel's packets arrive (sgi_kona_command_tokens[] frames
 * them there and hw/misc/sgi-kona.c hands them to sgi_kona_dma_command()).
 */
#ifndef HW_MISC_SGI_KONA_DMA_H
#define HW_MISC_SGI_KONA_DMA_H

#include "qemu/units.h"
#include "migration/vmstate.h"
#include "hw/display/sgi/command-fetch.h"
#include "hw/misc/sgi-command-port.h"
#include "hw/misc/sgi-kona-ports.h"

typedef struct SGIKonaState SGIKonaState;

/* The packet's selector and its six words, kona_idma. */
#define SGI_KONA_DMA_PACKET        SGI_KONA_CMD_DMA_PACKET
#define SGI_KONA_DMA_PACKET_WORDS  6

/* A command-buffer record: pipe-memory address and byte count. */
#define SGI_KONA_DMA_LIST_WRITE        SGI_KONA_CMD_DMA_LIST_WRITE
#define SGI_KONA_DMA_LIST_WRITE_WORDS  2

/* Word 2's fixed tag, OR'd onto the transfer address's low 12 bits. */
#define SGI_KONA_DMA_ADDR_TAG 0x9000

/*
 * The DMA pool 0x1ed registers: kona_dmapool requires its base and length
 * to be multiples of SGI_KONA_DMA_POOL_PAGE, which the model takes as the
 * pool's page size. SGI_KONA_DMA_POOL_MAX bounds the length, the model's
 * choice: the library's pool starts at 8 MiB and grows 4 MiB at a time,
 * and no limit is known.
 */
#define SGI_KONA_DMA_POOL_PAGE  0x4000
#define SGI_KONA_DMA_POOL_MAX   ((uint32_t)(256 * MiB))

/*
 * The most words one list invocation may fetch: the display-list class's
 * capacity, 0x3c00 granules of 0x400 bytes, 15 MiB (the configuration
 * block's class 1 descriptor), so a resident list can take the whole
 * class. The most ranges is the most one packet names, 0x170's eight.
 */
#define SGI_KONA_DMA_LIST_MAX_WORDS    ((uint32_t)(15 * MiB / 4))
#define SGI_KONA_DMA_LIST_MAX_RANGES   SGI_KONA_LIST_CALLS_MAX

/* Word 6 when the sender waits for the pipe's completion. */
#define SGI_KONA_DMA_DONE_WORD 0x100

/*
 * The bytes the engine this replaces migrated for its word accumulator:
 * six words and two counts. The main migration section keeps them as an
 * unused hole so version 6 streams still load.
 */
#define SGI_KONA_DMA_RETIRED_BYTES ((6 + 2) * 4)

/*
 * The most selectors the engine's port frames: its own two and the
 * kernel FIFO table's (include/hw/misc/sgi-kona-ports.h).
 */
#define SGI_KONA_DMA_MAX_SELECTORS 16

/*
 * What a completion's cookie stands for: the mailbox word to post, in
 * the low half, and the cause bit it posts under, in the high half.
 * SGI_KONA_DMA_COOKIE_NONE posts nothing.
 */
#define SGI_KONA_DMA_COOKIE(cause, word) \
    (((uint64_t)(cause) << 32) | (uint32_t)(word))
#define SGI_KONA_DMA_COOKIE_NONE 0
/*
 * The cookie of a packet a command port framed: its CONSUMED completion
 * returns the packet's words to the credit register.
 */
#define SGI_KONA_DMA_COOKIE_PORT (1ULL << 63)
#define SGI_KONA_DMA_COOKIE_CAUSE(cookie) ((uint32_t)((cookie) >> 32))
#define SGI_KONA_DMA_COOKIE_WORD(cookie)  ((uint32_t)(cookie))

typedef struct SGIKonaDma {
    /*
     * The kernel FIFO command port. opaque is the owning SGIKonaState, the
     * same convention hw/misc/sgi-kona.c's own textport field uses, so
     * this port's ops callbacks recover it with SGI_KONA(opaque).
     */
    SGICommandPort fifo_port;

    /* The port's selector table, built at init. Not migrated. */
    SGICommandPortSelector selectors[SGI_KONA_DMA_MAX_SELECTORS];
    unsigned num_selectors;

    /*
     * A 0x1c8 record waiting for the packet whose bytes it places:
     * record_dest is the pipe-memory byte address and record_bytes the
     * count.
     */
    bool record_pending;
    uint32_t record_dest;
    uint32_t record_bytes;

    /* The sequence number the next transfer item takes. */
    uint64_t seq;

    /*
     * The DMA pool 0x1ed registered: pool_base is its device address and
     * pool_len its bytes, valid while pool_valid is set.
     */
    bool pool_valid;
    uint64_t pool_base;
    uint32_t pool_len;

    /*
     * A 0x170 or 0x171 still taking its pairs: calls_token, and
     * calls_have of the calls_need words in calls_words. calls_need is 0
     * when none is open.
     */
    uint32_t calls_token;
    uint32_t calls_need;
    uint32_t calls_have;
    uint32_t calls_words[2 * SGI_KONA_DMA_LIST_MAX_RANGES];

    /*
     * The display-list fetch and the ranges its next() hook still has to
     * give it. A fetch runs within the command that invokes it, so none
     * of this outlives an access and none of it is migrated.
     */
    SGICommandFetch fetch;
    const uint32_t *ranges;
    unsigned nranges;

    /*
     * The pixel the from-plane conversion resolves next: the reverse
     * direction moves one display line per transfer, and
     * sgi_kona_dma_convert_row() is handed only the words, so the engine
     * names their plane row and first column here. Host only.
     */
    int32_t convert_x;
    int32_t convert_y;

    /*
     * Host-only qtest introspection, matching hw/misc/sgi-kona.c's own
     * test_pixel_x/test_pixel_y precedent: not guest state, not migrated.
     * test_reverse flips the next packet's direction (see the header
     * comment above); test_fb_offset places the framebuffer end of a
     * transfer, since no packet word names one. test_last_* are the kind,
     * cookie and payload of the last completion the engine published and
     * test_completions how many it has published, read through the
     * x-test-dma-last-* and x-test-dma-completions properties.
     */
    bool test_framebuffer;
    bool test_reverse;
    uint32_t test_fb_offset;
    uint32_t test_last_kind;
    uint64_t test_last_cookie;
    uint64_t test_last_payload;
    uint32_t test_completions;
} SGIKonaDma;

/*
 * Initializes the FIFO command port with the engine's selectors and the
 * kernel FIFO table (include/hw/misc/sgi-kona-ports.h). Call from
 * finish_realize(), the same point hw/misc/sgi-kona.c already
 * initializes its textport field.
 */
void sgi_kona_dma_init(SGIKonaDma *d, SGIKonaState *kona);

/* Resets the FIFO command port and drops a pending record. */
void sgi_kona_dma_reset(SGIKonaDma *d);

/*
 * A command framed on the command page and handed to the engine: the DMA
 * packet, the list-write record, the pool registration, or a display-list
 * packet, with its words.
 */
void sgi_kona_dma_command(SGIKonaState *s, uint32_t selector,
                          const uint32_t *args);

/*
 * True while a 0x170 or 0x171 takes its pairs: the command page then hands
 * each word to sgi_kona_dma_calls_word() instead of framing it.
 */
bool sgi_kona_dma_calls_open(SGIKonaState *s);
void sgi_kona_dma_calls_word(SGIKonaState *s, uint32_t word);

/* Drops a 0x170 or 0x171 still taking its pairs, logged by the caller. */
void sgi_kona_dma_calls_drop(SGIKonaState *s);

/* The engine's state beyond the port, a subsection of the device's. */
extern const VMStateDescription vmstate_sgi_kona_dma;

/* Registers the x-test-dma-* qtest-only properties on kona's own object. */
void sgi_kona_dma_test_props_init(Object *obj, SGIKonaDma *d);

#endif /* HW_MISC_SGI_KONA_DMA_H */
