/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-dma-buffer.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/memory_cached.h"

const SGIDMABufferProvider *sgi_dma_buffer_provider(MemoryRegion *region)
{
    Object *owner = memory_region_owner(region);
    SGIDMABufferClass *bc;

    if (!owner || !object_dynamic_cast(owner, TYPE_SGI_DMA_BUFFER)) {
        return NULL;
    }
    bc = SGI_DMA_BUFFER_GET_CLASS(owner);
    return bc->get_provider(owner, region);
}

void sgi_dma_buffer_init_as(SGIDMABufferContext *ctx, AddressSpace *as)
{
    *ctx = (SGIDMABufferContext) {
        .as = as,
        .provider = sgi_dma_buffer_provider(as->root),
    };
}

/* Controllers use this after translation, without translating a fault twice. */
MemTxResult sgi_dma_buffer_native_rw(AddressSpace *as, dma_addr_t addr,
                                     void *buf, dma_addr_t len,
                                     DMADirection dir, MemTxAttrs attrs,
                                     const SGIDMABufferRequest *request)
{
    uint8_t bytes[4] = { 0 };
    bool write = dir == DMA_DIRECTION_FROM_DEVICE;
    MemTxResult result;

    assert(!request->max_access_size || len <= request->max_access_size);
    if (!request->swap32) {
        return address_space_rw(as, addr, attrs, buf, len, write);
    }
    /* Converted providers accept whole aligned words only. */
    assert(!(addr & 3) && len == 4);
    if (write) {
        sgi_bswap32_buf(bytes, buf, len);
    }
    result = address_space_rw(as, addr, attrs, bytes, len, write);
    if (!write) {
        sgi_bswap32_buf(buf, bytes, len);
    }
    return result;
}

MemTxResult sgi_dma_buffer_rw(SGIDMABufferContext *ctx, dma_addr_t addr,
                              void *buf, dma_addr_t len, DMADirection dir,
                              MemTxAttrs attrs)
{
    BQL_LOCK_GUARD();

    dma_barrier(ctx->as, dir);
    return sgi_dma_buffer_rw_relaxed(ctx, addr, buf, len, dir, attrs);
}

static bool sgi_dma_buffer_cached_ram(MemoryRegionCache *cache, void *buf,
                                     dma_addr_t *len, DMADirection dir,
                                     const SGIDMABufferRequest *request,
                                     MemTxResult *result)
{
    bool swap32 = request->swap32;
    uint8_t scratch[4096];
    hwaddr length = MIN(*len, cache->len);

    if (!cache->ptr || cache->mrs.mr->ram_device) {
        return false;
    }
    if (swap32 || request->max_access_size) {
        length &= ~7ULL;
    }
    if (swap32) {
        length = MIN(length, sizeof(scratch));
    }
    if (!length) {
        return false;
    }
    if (dir == DMA_DIRECTION_FROM_DEVICE) {
        const void *source = buf;

        if (swap32) {
            sgi_bswap32_buf(scratch, buf, length);
            source = scratch;
        }
        qemu_ram_move(cache->ptr, source, length);
        address_space_cache_invalidate(cache, 0, length);
    } else {
        fuzz_dma_read_cb(cache->xlat, length, cache->mrs.mr);
        qemu_ram_move(buf, cache->ptr, length);
        if (swap32) {
            sgi_bswap32_buf(buf, buf, length);
        }
    }
    *len = length;
    *result = MEMTX_OK;
    return true;
}

static bool sgi_dma_buffer_try_buffer(AddressSpace *as, dma_addr_t addr,
                                     void *buf, dma_addr_t *len,
                                     DMADirection dir, MemTxAttrs attrs,
                                     const SGIDMABufferRequest *request,
                                     MemTxResult *result, bool controllers)
{
    MemoryRegionCache cache = { 0 };
    bool handled;
    RCU_READ_LOCK_GUARD();

    if (!*len || attrs.memory || (request->swap32 && (addr & 7))) {
        return false;
    }
    address_space_cache_init(&cache, as, addr, *len,
                              dir == DMA_DIRECTION_FROM_DEVICE);
    handled = sgi_dma_buffer_cached_ram(&cache, buf, len, dir, request,
                                         result);
    if (!handled && controllers && !cache.mrs.mr->is_iommu) {
        const SGIDMABufferProvider *provider =
            sgi_dma_buffer_provider(cache.mrs.mr);
        hwaddr length = MIN(*len, cache.len);

        handled = provider && provider->transfer(provider->opaque, cache.xlat,
                                                  buf, &length, dir, attrs,
                                                  request, result);
        if (handled) {
            assert(length && length <= *len && length <= cache.len);
            *len = length;
        }
    }
    if (!handled && !cache.ptr && !cache.mrs.mr->is_iommu) {
        *len = memory_access_size(cache.mrs.mr, *len,
                                   cache.xlat);
    }
    address_space_cache_destroy(&cache);
    return handled;
}

bool sgi_dma_buffer_rw_ram(AddressSpace *as, dma_addr_t addr, void *buf,
                           dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                           const SGIDMABufferRequest *request,
                           MemTxResult *result)
{
    return sgi_dma_buffer_try_buffer(as, addr, buf, len, dir, attrs, request,
                                    result, false);
}

bool sgi_dma_buffer_try_rw(AddressSpace *as, dma_addr_t addr, void *buf,
                           dma_addr_t *len, DMADirection dir, MemTxAttrs attrs,
                           const SGIDMABufferRequest *request,
                           MemTxResult *result)
{
    return sgi_dma_buffer_try_buffer(as, addr, buf, len, dir, attrs, request,
                                    result, true);
}

static const TypeInfo sgi_dma_buffer_type = {
    .name = TYPE_SGI_DMA_BUFFER,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(SGIDMABufferClass),
};

static void sgi_dma_buffer_register_types(void)
{
    type_register_static(&sgi_dma_buffer_type);
}

type_init(sgi_dma_buffer_register_types)
