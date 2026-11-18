/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * IOSurfaces for qemu.test's GPU images: the stand-in the test head draws
 * with the CPU where a board would draw with the GPU.
 */
#include "qemu/osdep.h"
#include "ui/display-shm-priv.h"
#include <IOSurface/IOSurface.h>

static CFNumberRef num(int64_t v)
{
    return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

bool dshm_mach_test_surface_new(uint32_t w, uint32_t h, DisplayShmGpuSlot *out,
                                void **surface)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const struct {
        CFStringRef key;
        int64_t value;
    } props[] = {
        { kIOSurfaceWidth, w }, { kIOSurfaceHeight, h },
        { kIOSurfaceBytesPerElement, 4 },
        { kIOSurfacePixelFormat, DSHM_FOURCC_BGRA },
    };
    IOSurfaceRef s;

    for (unsigned i = 0; i < ARRAY_SIZE(props); i++) {
        CFNumberRef n = num(props[i].value);

        CFDictionarySetValue(d, props[i].key, n);
        CFRelease(n);
    }
    s = IOSurfaceCreate(d);
    CFRelease(d);
    if (!s) {
        return false;
    }
    *out = (DisplayShmGpuSlot) {
        .kind = DSHM_GPU_IOSURFACE, .width = w, .height = h,
        .stride = IOSurfaceGetBytesPerRow(s), .fourcc = DSHM_FOURCC_BGRA,
        .handle = IOSurfaceCreateMachPort(s), .fence = -1,
    };
    if (!out->handle) {
        CFRelease(s);
        return false;
    }
    *surface = (void *)s;
    return true;
}

uint8_t *dshm_mach_test_surface_lock(void *surface)
{
    IOSurfaceLock(surface, 0, NULL);
    return IOSurfaceGetBaseAddress(surface);
}

void dshm_mach_test_surface_unlock(void *surface)
{
    IOSurfaceUnlock(surface, 0, NULL);
}

void dshm_mach_test_surface_free(void *surface)
{
    CFRelease(surface);
}
