/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Exportable Vulkan memory for the SGI display regions. What the transport
 * (sgi-display-shm.c) asks of the Vulkan side: open the adapter whose
 * deviceUUID the user named, allocate host-visible memory a viewer can
 * import, and free it. Not a model interface.
 */
#ifndef UI_SGI_DISPLAY_VK_H
#define UI_SGI_DISPLAY_VK_H

typedef struct SGIVkDevice SGIVkDevice;

typedef struct SGIVkExport {
    void *ptr;                  /* the whole allocation, mapped */
    size_t size;                /* what was asked for, a multiple of 64 KiB */
    uint64_t alloc_size;        /* Vulkan allocationSize, for the importer */
    uint32_t memory_type;       /* Vulkan memory type index, for the importer */
    bool dedicated;             /* the importer must allocate it dedicated */
#ifdef _WIN32
    HANDLE handle;              /* an NT handle QEMU owns */
#else
    int fd;                     /* an opaque descriptor QEMU owns */
#endif
    /* Private to sgi-display-vk.c. */
    SGIVkDevice *dev;
    uint64_t buffer, memory;
} SGIVkExport;

/*
 * Load the Vulkan loader and open the physical device whose deviceUUID is
 * uuid. NULL with errp set when there is no loader, no such device, or the
 * device cannot export host-visible memory for a buffer. Main thread only,
 * as are all the calls below. Nothing is torn down at exit: the loader stays
 * mapped and the device open until the process ends.
 */
SGIVkDevice *sgi_display_vk_open(const uint8_t uuid[16], Error **errp);

const char *sgi_display_vk_device_name(const SGIVkDevice *dev);
const uint8_t *sgi_display_vk_device_uuid(const SGIVkDevice *dev);
const uint8_t *sgi_display_vk_driver_uuid(const SGIVkDevice *dev);

/*
 * Allocate size bytes (a multiple of 65,536), zeroed and mapped, exportable
 * as the host's opaque handle type. NULL with errp set on any failure; the
 * caller then falls back to plain shared memory for that region.
 */
SGIVkExport *sgi_display_vk_alloc(SGIVkDevice *dev, size_t size,
                                  Error **errp);

/* Unmap, close the handle and free. The viewer's own import stays valid. */
void sgi_display_vk_free(SGIVkExport *x);

#endif
