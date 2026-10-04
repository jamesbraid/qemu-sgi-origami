/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_DMA_BUFFER_H
#define HW_MISC_SGI_DMA_BUFFER_H

#include "qemu/bswap.h"
#include "qom/object.h"
#include "system/dma.h"

#define TYPE_SGI_DMA_BUFFER "sgi-dma-buffer"
typedef struct SGIDMABufferClass SGIDMABufferClass;
DECLARE_CLASS_CHECKERS(SGIDMABufferClass, SGI_DMA_BUFFER, TYPE_SGI_DMA_BUFFER)

/* Conversion composes by XOR; zero leaves native access widths unchanged. */
typedef struct SGIDMABufferRequest {
    bool swap32;
    unsigned max_access_size;
} SGIDMABufferRequest;

/* Immutable controller metadata, borrowed for the native region's lifetime. */
typedef struct SGIDMABufferProvider {
    /* Decline before effects; accepted prefixes consume 1..*len bytes. */
    bool (*transfer)(void *opaque, dma_addr_t addr, void *buf,
                     dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                     const SGIDMABufferRequest *request,
                          MemTxResult *result);
    void *opaque;
} SGIDMABufferProvider;

struct SGIDMABufferClass {
    InterfaceClass parent_class;
    const SGIDMABufferProvider *(*get_provider)(Object *owner,
                                               MemoryRegion *region);
};

/* The caller owns the native address space and its device throughout DMA. */
typedef struct SGIDMABufferContext {
    AddressSpace *as;
    PCIDevice *pci_dev;
    bool (*enabled)(PCIDevice *dev);
    const SGIDMABufferProvider *provider;
    SGIDMABufferRequest request;
} SGIDMABufferContext;

const SGIDMABufferProvider *sgi_dma_buffer_provider(MemoryRegion *region);
void sgi_dma_buffer_init_as(SGIDMABufferContext *ctx, AddressSpace *as);
void sgi_dma_buffer_init_pci(SGIDMABufferContext *ctx, PCIDevice *dev);
MemTxResult sgi_dma_buffer_rw(SGIDMABufferContext *ctx, dma_addr_t addr,
                              void *buf, dma_addr_t len, DMADirection dir,
                              MemTxAttrs attrs);
bool sgi_dma_buffer_rw_ram(AddressSpace *as, dma_addr_t addr, void *buf,
                           dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                           const SGIDMABufferRequest *request,
                           MemTxResult *result);
bool sgi_dma_buffer_try_rw(AddressSpace *as, dma_addr_t addr, void *buf,
                           dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                           const SGIDMABufferRequest *request,
                           MemTxResult *result);

MemTxResult sgi_dma_buffer_native_rw(AddressSpace *as, dma_addr_t addr,
                                     void *buf, dma_addr_t len,
                                     DMADirection dir, MemTxAttrs attrs,
                                     const SGIDMABufferRequest *request);

static inline MemTxResult sgi_dma_buffer_read(SGIDMABufferContext *ctx,
                                              dma_addr_t addr, void *buf,
                                              dma_addr_t len)
{
    return sgi_dma_buffer_rw(ctx, addr, buf, len, DMA_DIRECTION_TO_DEVICE,
                             MEMTXATTRS_UNSPECIFIED);
}

static inline MemTxResult sgi_dma_buffer_write(SGIDMABufferContext *ctx,
                                               dma_addr_t addr,
                                               const void *buf, dma_addr_t len)
{
    return sgi_dma_buffer_rw(ctx, addr, (void *)buf, len,
                             DMA_DIRECTION_FROM_DEVICE,
                             MEMTXATTRS_UNSPECIFIED);
}

/* Requires the BQL, as for the controller callbacks; adds no DMA barrier. */
static inline MemTxResult sgi_dma_buffer_rw_relaxed(SGIDMABufferContext *ctx,
                                                   dma_addr_t addr, void *buf,
                                                   dma_addr_t len,
                                                   DMADirection dir,
                                                   MemTxAttrs attrs)
{
    const SGIDMABufferProvider *provider = ctx->provider;
    MemTxResult result = MEMTX_OK;
    uint8_t *bytes = buf;

    if (!provider || !len || (ctx->enabled && !ctx->enabled(ctx->pci_dev))) {
        return address_space_rw(ctx->as, addr, attrs, buf, len,
                                dir == DMA_DIRECTION_FROM_DEVICE);
    }
    while (len) {
        dma_addr_t consumed = len;
        MemTxResult part;

        if (!provider->transfer(provider->opaque, addr, bytes, &consumed,
                                dir, attrs, &ctx->request, &part)) {
            break;
        }
        assert(consumed && consumed <= len);
        result |= part;
        addr += consumed;
        bytes += consumed;
        len -= consumed;
    }
    if (len) {
        result |= address_space_rw(ctx->as, addr, attrs, bytes, len,
                                   dir == DMA_DIRECTION_FROM_DEVICE);
    }
    return result;
}

static inline MemTxResult sgi_dma_buffer_read_relaxed(SGIDMABufferContext *ctx,
                                                      dma_addr_t addr,
                                                      void *buf,
                                                      dma_addr_t len)
{
    return sgi_dma_buffer_rw_relaxed(ctx, addr, buf, len,
                                     DMA_DIRECTION_TO_DEVICE,
                                     MEMTXATTRS_UNSPECIFIED);
}

static inline MemTxResult sgi_dma_buffer_write_relaxed(SGIDMABufferContext *ctx,
                                                       dma_addr_t addr,
                                                       const void *buf,
                                                       dma_addr_t len)
{
    return sgi_dma_buffer_rw_relaxed(ctx, addr, (void *)buf, len,
                                     DMA_DIRECTION_FROM_DEVICE,
                                     MEMTXATTRS_UNSPECIFIED);
}

/* In-place or disjoint buffers, with no host alignment requirement. */
void sgi_bswap32_buf_ool(void *dst, const void *src, size_t len);
static inline void sgi_bswap32_buf(void *dst, const void *src, size_t len)
{
    uint8_t *out = dst;
    const uint8_t *in = src;

    assert(!(len & 3));
    if (len >= 64) {
        sgi_bswap32_buf_ool(dst, src, len);
        return;
    }
    for (size_t i = 0; i < len; i += 4) {
        stl_be_p(out + i, ldl_le_p(in + i));
    }
}

#endif
