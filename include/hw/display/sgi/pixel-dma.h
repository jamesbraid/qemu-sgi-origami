/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics DMA: guest pages resolved to host rows under the big lock.
 *
 * A head's DMA engine reads or writes a guest buffer the driver described
 * as a list of pages. The family decodes the descriptor and hands this code
 * three things: the head's bus (address space and hooks), the page list as
 * device addresses, and the byte range of the buffer to move. The buffer is
 * resolved here, under the big lock, to host chunks with dma_memory_map(),
 * so the copy that follows touches only host memory and could run on a
 * render thread. A range that does not map directly (it is not RAM, or the
 * map is exhausted) is copied through a bounce buffer instead, so the
 * outcome never depends on host mapping resources.
 *
 * A chunk is at most SGI_DMA_CHUNK_MAX bytes and never crosses a guest
 * page. That is also the trace contract a family's replay tools read:
 * every run of guest bytes the head reads -- a page list fetched with
 * sgi_dma_read(), or a transfer's source -- reaches the family's
 * dma_chunk() hook as one call per chunk, at the device address before
 * dma_address() composed it, with the sequence number the submitter gave,
 * synchronously while the transfer is resolved. Bytes the head writes to
 * guest memory are not traced.
 *
 * SGIPixelDMA is one pixel transfer between a guest buffer and a plane: rows
 * of w pixels, pitch bytes apart in the guest buffer, to or from plane rows
 * y .. y + h - 1. One source line is always one destination row, of exactly
 * w * guest_bpp bytes: a descriptor whose source lines do not match the
 * destination's rows (two rows to a line, or a line that splits a row) is
 * one SGIPixelDMA, and one completion, per source line. Every size may come
 * from the guest: a transfer that does not fit its page list, or whose rows
 * do not fit its plane, fails with SGI_FAIL_BOUNDS before anything is mapped
 * or allocated for it. The plane's size is what bounds the chunks and bounce
 * buffers a transfer can take, so the map step checks it. A transfer runs
 * in three steps so a queue can split them across threads --
 * sgi_dma_pixels_map() under the lock, sgi_dma_pixels_run() on host memory
 * only, sgi_dma_pixels_finish() under the lock -- and sgi_dma_pixels() does
 * all three for a synchronous head.
 */
#ifndef HW_DISPLAY_SGI_PIXEL_DMA_H
#define HW_DISPLAY_SGI_PIXEL_DMA_H

#include "system/dma.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/head-ops.h"

#define SGI_DMA_CHUNK_MAX 4096
/* No SGI head has a plane taller than this; it bounds a transfer's chunks. */
#define SGI_DMA_MAX_ROWS 16384
/*
 * The smallest page a page list may name. The heads' drivers hand over
 * pages of 4 KiB (MGRAS) and 16 KiB (Odyssey). No smaller page than a
 * chunk means every chunk but a range's ends is a whole SGI_DMA_CHUNK_MAX,
 * where a 1-byte page would make one chunk per byte.
 */
#define SGI_DMA_MIN_PAGE_SIZE SGI_DMA_CHUNK_MAX

/* A head's view of guest memory. */
typedef struct SGIDMABus {
    AddressSpace *as;           /* the head's bus address space */
    const SGIHeadOps *ops;      /* dma_address, dma_chunk, publish */
    void *opaque;
} SGIDMABus;

/*
 * A guest buffer as the driver described it: page i at addr[i]. A page
 * that is not aligned to page_size, or a page_size that is not a power of
 * two or is below SGI_DMA_MIN_PAGE_SIZE, fails the transfer with
 * SGI_FAIL_BOUNDS.
 */
typedef struct SGIDMAPageList {
    const uint64_t *addr;       /* device addresses */
    uint32_t npages;
    uint32_t page_size;
} SGIDMAPageList;

typedef struct SGIDMAChunk {
    void *host;
    uint32_t len;
    uint64_t pos;               /* offset within the page-listed buffer */
    uint64_t addr;              /* device address, as traced */
    hwaddr bus_addr;            /* composed address the map used */
    bool bounce;                /* host is a bounce buffer, not a map */
    /*
     * The bytes the head wrote, as offsets into the chunk: one run from the
     * first to the last, empty while written_end is 0. A pixel transfer
     * writes each chunk as one run from its start, so the run is exactly
     * the bytes written; a caller that wrote one chunk in separate pieces
     * would also write back the zeroed bytes between them.
     */
    uint32_t written_start;
    uint32_t written_end;
} SGIDMAChunk;

typedef struct SGIDMAMap {
    SGIDMAChunk *chunks;
    unsigned nchunks;
    unsigned capacity;
    unsigned cursor;            /* the chunk the last copy ended in */
    AddressSpace *as;
    DMADirection dir;
} SGIDMAMap;

/*
 * The bytes a page list maps, or 0 (logged) when its page size is not a
 * power of two of at least SGI_DMA_MIN_PAGE_SIZE.
 */
uint64_t sgi_dma_page_list_bytes(const SGIDMAPageList *pl);

/*
 * Read len bytes at device address addr, composed through dma_address(),
 * tracing them in chunks when trace is set. For fetching a page list.
 * Returns false, having logged a guest error, when the read fails.
 */
bool sgi_dma_read(const SGIDMABus *bus, uint64_t addr, void *buf,
                  uint32_t len, bool trace, uint64_t trace_seq);

/*
 * Resolve [offset, offset + len) of a page-listed buffer and append its
 * chunks to map. dir is QEMU's DMA direction: TO_DEVICE when the head reads
 * the guest buffer. Source bytes are traced when trace is set. Returns 0,
 * or an SGIFailure after logging a guest error, SGI_FAIL_BOUNDS when the
 * host cannot allocate a chunk or bounce buffer; on failure the chunks
 * this call added stay in map for sgi_dma_unmap() to release.
 */
int sgi_dma_map(const SGIDMABus *bus, SGIDMAMap *map,
                const SGIDMAPageList *pl, uint64_t offset, uint64_t len,
                DMADirection dir, bool trace, uint64_t trace_seq);

/*
 * Copy len bytes at buffer offset pos between map and buf, host only.
 * Returns false when map does not cover the range.
 */
bool sgi_dma_map_copy(SGIDMAMap *map, uint64_t pos, void *buf, uint64_t len,
                      bool to_guest);

/*
 * Release map. The bytes the head wrote are marked dirty, or for a bounce
 * buffer written back to guest memory, whether or not the transfer
 * completed: the guest's memory then shows how far it got. A bounce buffer
 * for guest memory the head writes is never filled from that memory first,
 * so only the chunk's written run goes back and a target that is not RAM
 * sees no read. A direct map is released with the length from its start to
 * the end of that run, which is what dma_memory_unmap() takes.
 */
void sgi_dma_unmap(SGIDMAMap *map);

typedef enum SGIPixelDMADir {
    SGI_DMA_TO_PLANE,
    SGI_DMA_FROM_PLANE,
} SGIPixelDMADir;

typedef struct SGIPixelDMA {
    SGIPixelDMADir dir;
    uint32_t plane;
    int32_t x, y, w, h;         /* plane coordinates */
    uint32_t pitch;             /* guest bytes from one row to the next */
    uint8_t guest_bpp;          /* guest bytes per pixel */
    uint8_t conversion;         /* family constant for convert_row() */
    uint8_t logic_op;           /* SGI_LOGIC_*, SGI_LOGIC_COPY to store */
    uint32_t mask;              /* plane bits a store may change */
    uint32_t tag_plane;         /* SGI_PLANE_NONE: no tag written */
    uint8_t tag;                /* stored in tag_plane per pixel written */
    SGIClipList clip;
    bool trace_chunks;
    uint64_t trace_seq;
    uint64_t seq;               /* the completion's sequence number */
    uint64_t cookie;            /* the family's tag for the completion */
    uint64_t offset;            /* buffer offset of row 0 */
    SGIDMAMap map;              /* resolved by sgi_dma_pixels_map() */
} SGIPixelDMA;

/*
 * Under the lock. m is the board memory whose plane the transfer targets,
 * read for the plane's size only. Returns 0 or an SGIFailure.
 */
int sgi_dma_pixels_map(SGIPixelDMA *d, const SGIDMABus *bus,
                       const SGIDMAPageList *pl, const SGIBoardMemory *m);
/* Host memory only. Returns 0 or an SGIFailure. */
int sgi_dma_pixels_run(SGIPixelDMA *d, SGIBoardMemory *m,
                       const SGIHeadOps *ops, void *opaque);
/*
 * Under the lock: release the map and publish WRITTEN with the bytes
 * moved, or FAILED with status when status is non-zero.
 */
void sgi_dma_pixels_finish(SGIPixelDMA *d, int status, const SGIHeadOps *ops,
                           void *opaque);
/* All three steps, for a synchronous head. Returns 0 or an SGIFailure. */
int sgi_dma_pixels(SGIPixelDMA *d, const SGIDMABus *bus,
                   const SGIDMAPageList *pl, SGIBoardMemory *m);

#endif
