/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The few Vulkan declarations the SGI display export needs, written out so
 * the build needs no Vulkan header and QEMU needs no Vulkan library at link
 * time: the loader is opened at run time and every entry point comes from
 * vkGetInstanceProcAddr(). Names, structure layouts and values follow the
 * published Vulkan 1.1 specification and its VK_KHR_external_memory_fd and
 * VK_KHR_external_memory_win32 extensions. Only what ui/sgi-display-vk.c and
 * tests/qtest/sgi-display-shm-test.c use is here.
 *
 * Handles are 64-bit integers for non-dispatchable objects, which is what the
 * specification makes them on every host QEMU builds for, and pointers for
 * the instance, physical device and device.
 */
#ifndef UI_SGI_DISPLAY_VK_ABI_H
#define UI_SGI_DISPLAY_VK_ABI_H

#ifdef _WIN32
#define SGI_VKAPI __stdcall
#else
#define SGI_VKAPI
#endif

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef uint64_t VkBuffer;
typedef uint64_t VkDeviceMemory;
typedef void (SGI_VKAPI *PFN_vkVoidFunction)(void);

#define VK_SUCCESS                          0
#define VK_WHOLE_SIZE                       (~0ULL)
#define VK_MAKE_API_VERSION_1_1             ((1u << 22) | (1u << 12))
#define VK_API_VERSION_MINOR(v)             (((v) >> 12) & 0x3ff)
#define VK_API_VERSION_MAJOR(v)             (((v) >> 22) & 0x7f)

enum {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
    VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 = 1000059001,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO = 1000071002,
    VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES = 1000071003,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES = 1000071004,
    VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO = 1000072000,
    VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO = 1000072002,
    VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR = 1000073000,
    VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR = 1000073003,
    VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR = 1000074000,
    VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR = 1000074002,
    VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO = 1000127001,
};

#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT                    0x00000001
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT                  0x00000020
#define VK_SHARING_MODE_EXCLUSIVE                           0
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT        0x00000001
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT     0x00000002
#define VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT       0x00000001
#define VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT           0x00000002
#define VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT           0x00000004
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT                 0x00000001
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT                 0x00000002
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT                0x00000004
#define VK_MEMORY_PROPERTY_HOST_CACHED_BIT                  0x00000008
#define VK_MAX_MEMORY_TYPES                                 32
#define VK_MAX_MEMORY_HEAPS                                 16
#define VK_MAX_EXTENSION_NAME_SIZE                          256
#define VK_UUID_SIZE                                        16

#ifdef _WIN32
#define SGI_VK_HANDLE_TYPE  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define SGI_VK_EXTENSION    "VK_KHR_external_memory_win32"
#define SGI_VK_LOADER       "vulkan-1.dll"
#else
#define SGI_VK_HANDLE_TYPE  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define SGI_VK_EXTENSION    "VK_KHR_external_memory_fd"
#define SGI_VK_LOADER       "libvulkan.so.1"
#endif

typedef struct VkApplicationInfo {
    uint32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkDeviceQueueCreateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkExtensionProperties {
    char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct VkPhysicalDeviceIDProperties {
    uint32_t sType;
    void *pNext;
    uint8_t deviceUUID[VK_UUID_SIZE];
    uint8_t driverUUID[VK_UUID_SIZE];
    uint8_t deviceLUID[8];
    uint32_t deviceNodeMask;
    VkBool32 deviceLUIDValid;
} VkPhysicalDeviceIDProperties;

/*
 * VkPhysicalDeviceProperties2 embeds VkPhysicalDeviceProperties, whose limits
 * run to hundreds of bytes this code never reads. The blob is larger than the
 * structure (824 bytes on a 64-bit host) so a driver writing the whole thing
 * stays inside it; the fields used sit at the offsets the specification's
 * layout gives: apiVersion at 0, deviceName at 20.
 */
typedef struct VkPhysicalDeviceProperties2 {
    uint32_t sType;
    void *pNext;
    uint64_t properties[128];
} VkPhysicalDeviceProperties2;
#define SGI_VK_PROPS_API_VERSION_OFFSET     0
#define SGI_VK_PROPS_DEVICE_NAME_OFFSET     20

typedef struct VkMemoryType {
    VkFlags propertyFlags;
    uint32_t heapIndex;
} VkMemoryType;

typedef struct VkMemoryHeap {
    VkDeviceSize size;
    VkFlags flags;
} VkMemoryHeap;

typedef struct VkPhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkPhysicalDeviceExternalBufferInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags flags;
    VkFlags usage;
    VkFlags handleType;
} VkPhysicalDeviceExternalBufferInfo;

typedef struct VkExternalBufferProperties {
    uint32_t sType;
    void *pNext;
    VkFlags externalMemoryFeatures;
    VkFlags exportFromImportedHandleTypes;
    VkFlags compatibleHandleTypes;
} VkExternalBufferProperties;

typedef struct VkExternalMemoryBufferCreateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags handleTypes;
} VkExternalMemoryBufferCreateInfo;

typedef struct VkBufferCreateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags flags;
    VkDeviceSize size;
    VkFlags usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkMemoryRequirements {
    VkDeviceSize size;
    VkDeviceSize alignment;
    uint32_t memoryTypeBits;
} VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
    uint32_t sType;
    const void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkExportMemoryAllocateInfo {
    uint32_t sType;
    const void *pNext;
    VkFlags handleTypes;
} VkExportMemoryAllocateInfo;

typedef struct VkMemoryDedicatedAllocateInfo {
    uint32_t sType;
    const void *pNext;
    uint64_t image;
    VkBuffer buffer;
} VkMemoryDedicatedAllocateInfo;

#ifdef _WIN32
typedef struct VkMemoryGetWin32HandleInfoKHR {
    uint32_t sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkFlags handleType;
} VkMemoryGetWin32HandleInfoKHR;

typedef struct VkImportMemoryWin32HandleInfoKHR {
    uint32_t sType;
    const void *pNext;
    VkFlags handleType;
    HANDLE handle;
    const wchar_t *name;
} VkImportMemoryWin32HandleInfoKHR;
#else
typedef struct VkMemoryGetFdInfoKHR {
    uint32_t sType;
    const void *pNext;
    VkDeviceMemory memory;
    VkFlags handleType;
} VkMemoryGetFdInfoKHR;

typedef struct VkImportMemoryFdInfoKHR {
    uint32_t sType;
    const void *pNext;
    VkFlags handleType;
    int fd;
} VkImportMemoryFdInfoKHR;
#endif

typedef PFN_vkVoidFunction (SGI_VKAPI *PFN_vkGetInstanceProcAddr)(
    VkInstance, const char *);
typedef PFN_vkVoidFunction (SGI_VKAPI *PFN_vkGetDeviceProcAddr)(
    VkDevice, const char *);
typedef VkResult (SGI_VKAPI *PFN_vkCreateInstance)(
    const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef void (SGI_VKAPI *PFN_vkDestroyInstance)(VkInstance, const void *);
typedef VkResult (SGI_VKAPI *PFN_vkEnumeratePhysicalDevices)(
    VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (SGI_VKAPI *PFN_vkGetPhysicalDeviceProperties2)(
    VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
typedef void (SGI_VKAPI *PFN_vkGetPhysicalDeviceMemoryProperties)(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
typedef void (SGI_VKAPI *PFN_vkGetPhysicalDeviceExternalBufferProperties)(
    VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo *,
    VkExternalBufferProperties *);
typedef VkResult (SGI_VKAPI *PFN_vkEnumerateDeviceExtensionProperties)(
    VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
typedef VkResult (SGI_VKAPI *PFN_vkCreateDevice)(
    VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
typedef void (SGI_VKAPI *PFN_vkDestroyDevice)(VkDevice, const void *);
typedef VkResult (SGI_VKAPI *PFN_vkCreateBuffer)(
    VkDevice, const VkBufferCreateInfo *, const void *, VkBuffer *);
typedef void (SGI_VKAPI *PFN_vkDestroyBuffer)(VkDevice, VkBuffer,
                                              const void *);
typedef void (SGI_VKAPI *PFN_vkGetBufferMemoryRequirements)(
    VkDevice, VkBuffer, VkMemoryRequirements *);
typedef VkResult (SGI_VKAPI *PFN_vkAllocateMemory)(
    VkDevice, const VkMemoryAllocateInfo *, const void *, VkDeviceMemory *);
typedef void (SGI_VKAPI *PFN_vkFreeMemory)(VkDevice, VkDeviceMemory,
                                           const void *);
typedef VkResult (SGI_VKAPI *PFN_vkBindBufferMemory)(VkDevice, VkBuffer,
                                                     VkDeviceMemory,
                                                     VkDeviceSize);
typedef VkResult (SGI_VKAPI *PFN_vkMapMemory)(VkDevice, VkDeviceMemory,
                                              VkDeviceSize, VkDeviceSize,
                                              VkFlags, void **);
typedef void (SGI_VKAPI *PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef void (SGI_VKAPI *PFN_vkGetPhysicalDeviceQueueFamilyProperties)(
    VkPhysicalDevice, uint32_t *, void *);
#ifdef _WIN32
typedef VkResult (SGI_VKAPI *PFN_vkGetMemoryWin32HandleKHR)(
    VkDevice, const VkMemoryGetWin32HandleInfoKHR *, HANDLE *);
#else
typedef VkResult (SGI_VKAPI *PFN_vkGetMemoryFdKHR)(
    VkDevice, const VkMemoryGetFdInfoKHR *, int *);
#endif

QEMU_BUILD_BUG_ON(sizeof(VkMemoryRequirements) != 24);
QEMU_BUILD_BUG_ON(sizeof(VkPhysicalDeviceMemoryProperties) != 520);
QEMU_BUILD_BUG_ON(sizeof(VkExtensionProperties) != 260);

#endif
