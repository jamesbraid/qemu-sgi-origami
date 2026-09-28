/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics command fetch: a display list the head reads for itself.
 *
 * Some heads execute a display list by fetching its words from memory
 * rather than receiving them on the command port: from a pool of guest
 * pages the driver installed as a page list, or from a plane of words in
 * the pipe's own memory. The family decodes the invocation packet and
 * describes where the words are; this code resolves them under the big
 * lock into host-order word segments, feeds them to the family's grammar
 * exactly as if they had arrived on the port, and publishes one CONSUMED
 * completion carrying the family's cookie and the number of words
 * consumed.
 *
 * The first user is MGRAS, whose list invocation names a pool offset and a
 * size after installing the pool's page list. In the one pool measured, a
 * 256-triangle list, each 4 KiB page begins with three header words and
 * the size equals the words of page 0 from offset 0 through its last list
 * word, headers included; that the size counts 32-bit words is inferred
 * from that dump alone. How the board reaches the list's later pages is
 * not known, so it is an open hook here, next(), rather than a rule: the
 * family decides after each range whether the fetch continues and where.
 * A family with no rule leaves it NULL and the fetch ends at the words it
 * was asked for. The words handed to the grammar are the pool's words as
 * they are, headers included; what a header means is the family's.
 *
 * Every size a fetch resolves is the guest's, and every word it resolves
 * is host memory held until the finish, so each invocation has two caps:
 * the most words over all its ranges and the most ranges. A range that
 * would pass either fails the invocation with SGI_FAIL_BOUNDS, logged as
 * a guest error, before anything is mapped or allocated for it; so does a
 * range the host cannot allocate, rather than aborting QEMU. The ranges
 * already resolved stay for the finish to release.
 */
#ifndef HW_DISPLAY_SGI_COMMAND_FETCH_H
#define HW_DISPLAY_SGI_COMMAND_FETCH_H

#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/pixel-dma.h"

/*
 * The caps a fetch with max_words or max_segments zero uses. They leave
 * wide headroom over the longest list measured on MGRAS, 13,355 words in
 * 14 ranges, while holding one invocation to 4 MiB of words. A family
 * whose lists can be longer raises them.
 */
#define SGI_FETCH_MAX_WORDS     (1u << 20)
#define SGI_FETCH_MAX_SEGMENTS  2048

typedef struct SGICommandFetch SGICommandFetch;

typedef struct SGIFetchSegment {
    uint32_t *words;            /* host order, owned by the fetch */
    uint32_t nwords;
    uint64_t addr;              /* device address of the first word */
} SGIFetchSegment;

struct SGICommandFetch {
    uint32_t context;           /* the hardware context the words are for */
    /* The family's grammar: n words, in order, on the port's terms. */
    void (*decode)(void *opaque, uint32_t context, const uint32_t *words,
                   unsigned n);
    /*
     * Open: where the fetch goes after the range just resolved. Return
     * true with the next range's pool byte offset and word count to
     * continue, false to stop. NULL stops after the first range.
     */
    bool (*next)(void *opaque, const SGICommandFetch *f, uint64_t *offset,
                 uint64_t *nwords);
    bool little_endian;         /* the words' byte order in memory */
    bool trace_chunks;
    uint64_t trace_seq;
    uint64_t seq;               /* the completion's sequence number */
    uint64_t cookie;            /* the family's fence, reply or interrupt */
    /*
     * One invocation's caps: its words over all ranges, and its ranges.
     * Zero means SGI_FETCH_MAX_WORDS or SGI_FETCH_MAX_SEGMENTS, so a
     * zeroed fetch is bounded too. A family sets them once, before its
     * first map, from what its hardware can hold.
     */
    uint64_t max_words;
    unsigned max_segments;

    /*
     * Resolved at submit, released by sgi_fetch_finish(). A fetch holds
     * one invocation at a time, from its map to its finish, so a list
     * invoked from inside decode() needs a fetch of its own. A map of a
     * fetch not yet finished fails with SGI_FAIL_BUSY and changes nothing
     * in it, seq and cookie included, which stay the running invocation's.
     * The finish that goes with that map publishes nothing and leaves the
     * running invocation alone: the refused invocation's seq and cookie
     * never reached the fetch, so its caller publishes its FAILED record.
     * A fetch starts zeroed.
     */
    SGIFetchSegment *segments;
    unsigned nsegments;
    unsigned capacity;          /* segments allocated */
    uint64_t nwords;            /* total over the segments */
    unsigned open;              /* maps not yet finished */
};

/*
 * Under the lock: resolve nwords words at byte offset in a page-listed
 * pool, then whatever next() asks for, into host-order segments. A range
 * of no words adds nothing. A fetch
 * never resolves more words in total than the pool holds or its caps
 * allow, whatever next() says. Returns 0 or an SGIFailure. next() gets
 * the bus's opaque.
 */
int sgi_fetch_map(SGICommandFetch *f, const SGIDMABus *bus,
                  const SGIDMAPageList *pl, uint64_t offset, uint64_t nwords);
/*
 * Under the lock: nwords words at byte offset in a plane of words in
 * board memory, in the plane's packing, then whatever next() asks for,
 * with offsets within the same plane. A list invoked as several ranges is
 * one fetch and one completion. next() gets opaque.
 */
int sgi_fetch_map_plane(SGICommandFetch *f, const SGIBoardMemory *m,
                        uint32_t plane, uint64_t offset, uint64_t nwords,
                        void *opaque);
/* Host memory only: feed every segment to decode(). */
void sgi_fetch_run(SGICommandFetch *f, void *opaque);
/*
 * Under the lock, once after every map whatever it returned: release the
 * segments and publish CONSUMED with the words fed to the grammar, or
 * FAILED with status when status is non-zero. After a map refused with
 * SGI_FAIL_BUSY it only closes that map and publishes nothing.
 */
void sgi_fetch_finish(SGICommandFetch *f, int status, const SGIHeadOps *ops,
                      void *opaque);
/*
 * All three from a page-listed pool, for a synchronous head. On
 * SGI_FAIL_BUSY nothing is published: the caller publishes FAILED for
 * the refused invocation under that invocation's own seq and cookie.
 */
int sgi_fetch(SGICommandFetch *f, const SGIDMABus *bus,
              const SGIDMAPageList *pl, uint64_t offset, uint64_t nwords);

#endif
