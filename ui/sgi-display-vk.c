/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Exportable Vulkan memory for the SGI display regions.
 *
 * A viewer on the same GPU imports the allocation into its own Vulkan device
 * and reads what the models write, with no copy: the standard
 * cross-process path, VK_KHR_external_memory_fd (an OPAQUE_FD) on Linux and
 * VK_KHR_external_memory_win32 (an OPAQUE_WIN32 NT handle) on Windows. No
 * queue is ever submitted to; the device exists only to own memory.
 *
 * The Vulkan loader is opened at run time (dlopen, LoadLibrary on Windows)
 * and QEMU links nothing: a host without a loader, or without the adapter,
 * simply gets plain shared memory. See docs/system/mips/sgi/display-shm.rst.
 * macOS has no opaque handle to export; the viewer uses Metal there.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "ui/sgi-display-vk.h"

#ifdef CONFIG_DARWIN

SGIVkDevice *sgi_display_vk_open(const uint8_t uuid[16], Error **errp)
{
    error_setg(errp, "macOS exports no Vulkan memory");
    return NULL;
}

const char *sgi_display_vk_device_name(const SGIVkDevice *dev)
{
    return "";
}

const uint8_t *sgi_display_vk_device_uuid(const SGIVkDevice *dev)
{
    return NULL;
}

const uint8_t *sgi_display_vk_driver_uuid(const SGIVkDevice *dev)
{
    return NULL;
}

SGIVkExport *sgi_display_vk_alloc(SGIVkDevice *dev, size_t size, Error **errp)
{
    error_setg(errp, "macOS exports no Vulkan memory");
    return NULL;
}

void sgi_display_vk_free(SGIVkExport *x)
{
}

#else

#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "ui/sgi-display-vk-abi.h"

/*
 * The loader's file name is the platform's default; this names another one.
 * It is how a test, or a host that keeps the loader somewhere unusual,
 * points QEMU at it.
 */
#define SGI_VK_LOADER_ENV "QEMU_SGI_VULKAN_LOADER"

#ifdef _WIN32
#define LIB_OPEN(name)      ((void *)LoadLibraryA(name))
#define LIB_SYM(lib, name)  ((void *)GetProcAddress((HMODULE)(lib), name))
#else
#define LIB_OPEN(name)      dlopen(name, RTLD_NOW | RTLD_LOCAL)
#define LIB_SYM(lib, name)  dlsym(lib, name)
#endif

struct SGIVkDevice {
    void *lib;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    char name[256];
    uint8_t device_uuid[VK_UUID_SIZE];
    uint8_t driver_uuid[VK_UUID_SIZE];
    VkPhysicalDeviceMemoryProperties mem;
    bool dedicated_only;        /* the driver wants these buffers dedicated */

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
#ifdef _WIN32
    PFN_vkGetMemoryWin32HandleKHR GetMemoryHandle;
#else
    PFN_vkGetMemoryFdKHR GetMemoryHandle;
#endif
};

static bool has_extension(PFN_vkEnumerateDeviceExtensionProperties enumerate,
                          VkPhysicalDevice pd, const char *name)
{
    g_autofree VkExtensionProperties *ext = NULL;
    uint32_t n = 0;

    if (enumerate(pd, NULL, &n, NULL) != VK_SUCCESS || !n) {
        return false;
    }
    ext = g_new0(VkExtensionProperties, n);
    if (enumerate(pd, NULL, &n, ext) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!strncmp(ext[i].extensionName, name,
                     VK_MAX_EXTENSION_NAME_SIZE)) {
            return true;
        }
    }
    return false;
}

#define NEED(var, getter, handle, sym)                                  \
    do {                                                                \
        var = (typeof(var))getter(handle, sym);                         \
        if (!var) {                                                     \
            error_setg(errp, "the Vulkan loader has no %s", sym);       \
            goto fail;                                                  \
        }                                                               \
    } while (0)

SGIVkDevice *sgi_display_vk_open(const uint8_t uuid[16], Error **errp)
{
    SGIVkDevice *d = g_new0(SGIVkDevice, 1);
    const char *loader = g_getenv(SGI_VK_LOADER_ENV);
    g_autofree VkPhysicalDevice *pds = NULL;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkCreateInstance create_instance;
    PFN_vkEnumeratePhysicalDevices enumerate_devices;
    PFN_vkGetPhysicalDeviceProperties2 get_props2;
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props;
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_ext;
    PFN_vkCreateDevice create_device;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_families;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "qemu-sgi-display",
        .apiVersion = VK_MAKE_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    const char *dev_exts[] = { SGI_VK_EXTENSION };
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts,
    };
    VkPhysicalDeviceExternalBufferInfo ebi = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .handleType = SGI_VK_HANDLE_TYPE,
    };
    VkExternalBufferProperties ebp = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
    };
    uint32_t npd = 0, nfamilies = 0;
    bool found = false;

    d->lib = LIB_OPEN(loader && *loader ? loader : SGI_VK_LOADER);
    if (!d->lib) {
        error_setg(errp, "cannot load the Vulkan loader %s",
                   loader && *loader ? loader : SGI_VK_LOADER);
        goto fail;
    }
    get_instance_proc = (PFN_vkGetInstanceProcAddr)
        LIB_SYM(d->lib, "vkGetInstanceProcAddr");
    if (!get_instance_proc) {
        error_setg(errp, "the Vulkan loader has no vkGetInstanceProcAddr");
        goto fail;
    }
    NEED(create_instance, get_instance_proc, NULL, "vkCreateInstance");
    if (create_instance(&ici, NULL, &d->instance) != VK_SUCCESS) {
        error_setg(errp, "cannot create a Vulkan 1.1 instance");
        d->instance = NULL;
        goto fail;
    }
    NEED(enumerate_devices, get_instance_proc, d->instance,
         "vkEnumeratePhysicalDevices");
    NEED(get_props2, get_instance_proc, d->instance,
         "vkGetPhysicalDeviceProperties2");
    NEED(get_mem_props, get_instance_proc, d->instance,
         "vkGetPhysicalDeviceMemoryProperties");
    NEED(get_external, get_instance_proc, d->instance,
         "vkGetPhysicalDeviceExternalBufferProperties");
    NEED(enumerate_ext, get_instance_proc, d->instance,
         "vkEnumerateDeviceExtensionProperties");
    NEED(create_device, get_instance_proc, d->instance, "vkCreateDevice");
    NEED(get_queue_families, get_instance_proc, d->instance,
         "vkGetPhysicalDeviceQueueFamilyProperties");
    NEED(d->GetDeviceProcAddr, get_instance_proc, d->instance,
         "vkGetDeviceProcAddr");

    if (enumerate_devices(d->instance, &npd, NULL) != VK_SUCCESS || !npd) {
        error_setg(errp, "Vulkan reports no physical device");
        goto fail;
    }
    pds = g_new0(VkPhysicalDevice, npd);
    if (enumerate_devices(d->instance, &npd, pds) < 0) {
        error_setg(errp, "cannot list the Vulkan physical devices");
        goto fail;
    }
    for (uint32_t i = 0; i < npd && !found; i++) {
        VkPhysicalDeviceIDProperties id = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &id,
        };

        get_props2(pds[i], &p2);
        if (memcmp(id.deviceUUID, uuid, VK_UUID_SIZE)) {
            continue;
        }
        found = true;
        d->phys = pds[i];
        memcpy(d->device_uuid, id.deviceUUID, VK_UUID_SIZE);
        memcpy(d->driver_uuid, id.driverUUID, VK_UUID_SIZE);
        pstrcpy(d->name, sizeof(d->name),
                (const char *)p2.properties + SGI_VK_PROPS_DEVICE_NAME_OFFSET);
        {
            uint32_t api;

            memcpy(&api, (const char *)p2.properties +
                   SGI_VK_PROPS_API_VERSION_OFFSET, sizeof(api));
            if (VK_API_VERSION_MAJOR(api) < 1 ||
                (VK_API_VERSION_MAJOR(api) == 1 &&
                 VK_API_VERSION_MINOR(api) < 1)) {
                error_setg(errp, "the Vulkan device %s speaks Vulkan %u.%u, "
                           "1.1 is needed", d->name, VK_API_VERSION_MAJOR(api),
                           VK_API_VERSION_MINOR(api));
                goto fail;
            }
        }
    }
    if (!found) {
        error_setg(errp, "no Vulkan device has that deviceUUID");
        goto fail;
    }
    if (!has_extension(enumerate_ext, d->phys, SGI_VK_EXTENSION)) {
        error_setg(errp, "the Vulkan device %s lacks %s", d->name,
                   SGI_VK_EXTENSION);
        goto fail;
    }

    /* Can this device export a storage buffer's memory at all? */
    get_external(d->phys, &ebi, &ebp);
    if (!(ebp.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        error_setg(errp, "the Vulkan device %s cannot export buffer memory "
                   "as an opaque handle", d->name);
        goto fail;
    }
    d->dedicated_only = ebp.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    get_mem_props(d->phys, &d->mem);

    /*
     * A device needs one queue to exist. Family 0 is it, if there is a
     * family; nothing is ever submitted to it.
     */
    get_queue_families(d->phys, &nfamilies, NULL);
    if (!nfamilies) {
        error_setg(errp, "the Vulkan device %s has no queue family", d->name);
        goto fail;
    }
    if (create_device(d->phys, &dci, NULL, &d->dev) != VK_SUCCESS) {
        error_setg(errp, "cannot create a device on %s", d->name);
        d->dev = NULL;
        goto fail;
    }
    NEED(d->CreateBuffer, d->GetDeviceProcAddr, d->dev, "vkCreateBuffer");
    NEED(d->DestroyBuffer, d->GetDeviceProcAddr, d->dev, "vkDestroyBuffer");
    NEED(d->GetBufferMemoryRequirements, d->GetDeviceProcAddr, d->dev,
         "vkGetBufferMemoryRequirements");
    NEED(d->AllocateMemory, d->GetDeviceProcAddr, d->dev, "vkAllocateMemory");
    NEED(d->FreeMemory, d->GetDeviceProcAddr, d->dev, "vkFreeMemory");
    NEED(d->BindBufferMemory, d->GetDeviceProcAddr, d->dev,
         "vkBindBufferMemory");
    NEED(d->MapMemory, d->GetDeviceProcAddr, d->dev, "vkMapMemory");
    NEED(d->UnmapMemory, d->GetDeviceProcAddr, d->dev, "vkUnmapMemory");
#ifdef _WIN32
    NEED(d->GetMemoryHandle, d->GetDeviceProcAddr, d->dev,
         "vkGetMemoryWin32HandleKHR");
#else
    NEED(d->GetMemoryHandle, d->GetDeviceProcAddr, d->dev,
         "vkGetMemoryFdKHR");
#endif
    return d;

fail:
    /*
     * The device and instance are left to the process exit: tearing down a
     * driver half way is no safer than leaving it, and the loader stays
     * mapped for the same reason.
     */
    g_free(d);
    return NULL;
}

const char *sgi_display_vk_device_name(const SGIVkDevice *dev)
{
    return dev->name;
}

const uint8_t *sgi_display_vk_device_uuid(const SGIVkDevice *dev)
{
    return dev->device_uuid;
}

const uint8_t *sgi_display_vk_driver_uuid(const SGIVkDevice *dev)
{
    return dev->driver_uuid;
}

/*
 * The memory type to export from, among those the buffer allows: host
 * visible and coherent, because nothing here flushes; then cached ones
 * first, so QEMU's frequent CPU reads of video memory do not crawl through a
 * write-combined window; then not device local, which on a discrete GPU is
 * that window (the PCIe BAR). -1 for none.
 */
static int pick_memory_type(const SGIVkDevice *d, uint32_t allowed)
{
    const VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int best = -1, best_rank = -1;

    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++) {
        VkFlags f = d->mem.memoryTypes[i].propertyFlags;
        int rank;

        if (!(allowed & (1u << i)) || (f & want) != want) {
            continue;
        }
        rank = 2 * !!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) +
               !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (rank > best_rank) {
            best = i;
            best_rank = rank;
        }
    }
    return best;
}

SGIVkExport *sgi_display_vk_alloc(SGIVkDevice *d, size_t size, Error **errp)
{
    VkExternalMemoryBufferCreateInfo ebci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = SGI_VK_HANDLE_TYPE,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ebci, .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkExportMemoryAllocateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = SGI_VK_HANDLE_TYPE,
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
    };
    VkMemoryRequirements req;
    SGIVkExport *x = g_new0(SGIVkExport, 1);
    int type;
    void *ptr = NULL;
    VkResult r;

    x->dev = d;
    x->size = size;
#ifdef _WIN32
    x->handle = NULL;
#else
    x->fd = -1;
#endif
    r = d->CreateBuffer(d->dev, &bci, NULL, &x->buffer);
    if (r != VK_SUCCESS) {
        error_setg(errp, "vkCreateBuffer failed: %d", r);
        x->buffer = 0;
        goto fail;
    }
    d->GetBufferMemoryRequirements(d->dev, x->buffer, &req);
    type = pick_memory_type(d, req.memoryTypeBits);
    if (type < 0) {
        error_setg(errp, "no host-visible coherent memory type can be "
                   "exported");
        goto fail;
    }
    if (d->dedicated_only) {
        ded.buffer = x->buffer;
        exp.pNext = &ded;
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = d->AllocateMemory(d->dev, &mai, NULL, &x->memory);
    if (r != VK_SUCCESS) {
        error_setg(errp, "vkAllocateMemory of %zu bytes failed: %d", size, r);
        x->memory = 0;
        goto fail;
    }
    r = d->BindBufferMemory(d->dev, x->buffer, x->memory, 0);
    if (r != VK_SUCCESS) {
        error_setg(errp, "vkBindBufferMemory failed: %d", r);
        goto fail;
    }
    r = d->MapMemory(d->dev, x->memory, 0, VK_WHOLE_SIZE, 0, &ptr);
    if (r != VK_SUCCESS || !ptr) {
        error_setg(errp, "vkMapMemory failed: %d", r);
        ptr = NULL;
        goto fail;
    }
    x->ptr = ptr;
    /* Fresh driver memory is not promised to be zero; the contract is. */
    memset(ptr, 0, req.size);
    {
#ifdef _WIN32
        VkMemoryGetWin32HandleInfoKHR gi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
            .memory = x->memory, .handleType = SGI_VK_HANDLE_TYPE,
        };

        r = d->GetMemoryHandle(d->dev, &gi, &x->handle);
#else
        VkMemoryGetFdInfoKHR gi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = x->memory, .handleType = SGI_VK_HANDLE_TYPE,
        };

        r = d->GetMemoryHandle(d->dev, &gi, &x->fd);
#endif
        if (r != VK_SUCCESS) {
            error_setg(errp, "cannot export the memory as a handle: %d", r);
            goto fail;
        }
#ifndef _WIN32
        qemu_set_cloexec(x->fd);
#endif
    }
    x->alloc_size = req.size;
    x->memory_type = type;
    x->dedicated = d->dedicated_only;
    return x;

fail:
    sgi_display_vk_free(x);
    return NULL;
}

void sgi_display_vk_free(SGIVkExport *x)
{
    SGIVkDevice *d;

    if (!x) {
        return;
    }
    d = x->dev;
#ifdef _WIN32
    if (x->handle) {
        CloseHandle(x->handle);
    }
#else
    if (x->fd >= 0) {
        close(x->fd);
    }
#endif
    if (x->ptr) {
        d->UnmapMemory(d->dev, x->memory);
    }
    if (x->memory) {
        d->FreeMemory(d->dev, x->memory, NULL);
    }
    if (x->buffer) {
        d->DestroyBuffer(d->dev, x->buffer, NULL);
    }
    g_free(x);
}

#endif /* !CONFIG_DARWIN */
