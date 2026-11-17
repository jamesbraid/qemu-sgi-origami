/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The shm storage class of the display-shm producer: a sealed memfd on
 * Linux, an unlinked POSIX shared-memory object elsewhere, a pagefile-backed
 * section on Windows. See docs/interop/display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/memfd.h"
#include "qemu/bswap.h"
#include "ui/display-shm-priv.h"

#ifndef _WIN32
#include <sys/mman.h>
#endif

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

#ifndef _WIN32
#ifdef CONFIG_LINUX
static bool seal(int fd, unsigned seals, Error **errp)
{
    int got;

    if (fcntl(fd, F_ADD_SEALS, seals) < 0) {
        error_setg_errno(errp, errno, "cannot seal shared memory");
        return false;
    }
    got = fcntl(fd, F_GET_SEALS);
    if (got < 0 || (got & seals) != seals) {
        error_setg(errp, "shared memory did not take its seals");
        return false;
    }
    return true;
}
#endif

static int object_create(DisplayShm *d, size_t size, Error **errp)
{
    int fd;

#ifdef CONFIG_LINUX
    fd = qemu_memfd_create("display-shm", size, false, 0,
                           F_SEAL_SHRINK | F_SEAL_GROW, errp);
    return fd;
#else
    static unsigned serial;
    g_autofree char *name = g_strdup_printf("%s%u-%u",
        d->shm_prefix && *d->shm_prefix ? d->shm_prefix : "/dshm-",
        (unsigned)getpid(), serial++);

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        error_setg_errno(errp, errno, "cannot create shared memory");
        return -1;
    }
    /* The name goes at once; the viewer gets the descriptor. */
    shm_unlink(name);
    if (ftruncate(fd, size) < 0) {
        error_setg_errno(errp, errno, "cannot size shared memory");
        close(fd);
        return -1;
    }
    qemu_set_cloexec(fd);
    return fd;
#endif
}

#endif /* !_WIN32 */

#ifdef _WIN32
static DShmRegion *region_map(size_t size, Error **errp)
{
    DShmRegion *r = g_new0(DShmRegion, 1);

    r->h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                              (DWORD)((uint64_t)size >> 32),
                              (DWORD)(size & 0xffffffffu), NULL);
    if (!r->h) {
        error_setg(errp, "cannot create shared memory: error %lu",
                   GetLastError());
        g_free(r);
        return NULL;
    }
    r->ptr = MapViewOfFile(r->h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!r->ptr) {
        error_setg(errp, "cannot map shared memory: error %lu",
                   GetLastError());
        CloseHandle(r->h);
        g_free(r);
        return NULL;
    }
    return r;
}
#else
static DShmRegion *region_map(DisplayShm *d, size_t size, bool writable,
                              Error **errp)
{
    DShmRegion *r;
    void *p;
    int fd = object_create(d, size, errp);

    if (fd < 0) {
        return NULL;
    }
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        error_setg_errno(errp, errno, "cannot map shared memory");
        close(fd);
        return NULL;
    }
#ifdef CONFIG_LINUX
    /* FUTURE_WRITE only after our mapping; SEAL stops viewer-added seals. */
    if (!seal(fd, F_SEAL_SHRINK | F_SEAL_GROW |
              (writable ? 0 : F_SEAL_FUTURE_WRITE) | F_SEAL_SEAL, errp)) {
        munmap(p, size);
        close(fd);
        return NULL;
    }
#endif
    r = g_new0(DShmRegion, 1);
    r->ptr = p;
    r->h = fd;
    return r;
}
#endif

/* A zeroed region of immutable size, read-only to the viewer unless told. */
DShmRegion *dshm_region_new(DisplayShm *d, uint32_t kind, size_t size,
                            bool writable_by_viewer, Error **errp)
{
    DShmRegion *r;

    size = ROUND_UP(MAX(size, 1), DSHM_REGION_ALIGN);
    if (d->retained + size > d->max_retained) {
        error_setg(errp, "display-shm: max-retained-bytes reached");
        return NULL;
    }
#ifdef _WIN32
    r = region_map(size, errp);
#else
    r = region_map(d, size, writable_by_viewer, errp);
#endif
    if (!r) {
        return NULL;
    }
    r->id = d->next_region_id++;
    r->kind = kind;
    r->access = writable_by_viewer ? DSHM_ACCESS_READ | DSHM_ACCESS_WRITE
                                   : DSHM_ACCESS_READ;
    r->size = size;
    r->refs = 1;
    d->retained += size;
    return r;
}

/* A GPU slot: only the dma-buf's descriptor or NT handles, ours to close. */
DShmRegion *dshm_region_new_gpu(DisplayShm *d, const DisplayShmGpuSlot *g,
                                uint32_t id)
{
#if defined(CONFIG_LINUX) || defined(_WIN32)
    DShmRegion *r;
#ifdef _WIN32
    bool kind_ok = g->kind == DSHM_GPU_WIN32 && g->handle > 0;
#else
    bool kind_ok = g->kind == DSHM_GPU_DMABUF && g->handle >= 0;
#endif

    if (!kind_ok || !g->width || !g->height || g->stride < g->width * 4) {
#ifdef _WIN32
        if (g->handle > 0) {
            CloseHandle((HANDLE)(intptr_t)g->handle);
        }
        if (g->fence > 0) {
            CloseHandle((HANDLE)(intptr_t)g->fence);
        }
#else
        if (g->handle >= 0) {
            close(g->handle);
        }
#endif
        return NULL;
    }
    r = g_new0(DShmRegion, 1);
#ifdef _WIN32
    if (g->fence > 0) {
        r->fence_id = d->next_region_id++;
        r->fence_h = (HANDLE)(intptr_t)g->fence;
    }
    r->h = (HANDLE)(intptr_t)g->handle;
#else
    r->h = g->handle;
#endif
    r->id = d->next_region_id++;
    r->kind = DSHM_KIND_SLOT;
    r->access = DSHM_ACCESS_READ;
    r->size = (size_t)g->stride * g->height;
    r->refs = 1;
    r->gpu = true;
    r->gpu_id = id;
    r->gpu_slot = *g;
    return r;
#else
    return NULL;
#endif
}

void dshm_region_ref(DShmRegion *r)
{
    r->refs++;
}

void dshm_region_unref(DisplayShm *d, DShmRegion *r)
{
    assert(r->refs);
    if (--r->refs) {
        return;
    }
    if (r->gpu) {
        /* GPU memory is not host memory: not counted in d->retained */
#ifdef _WIN32
        CloseHandle(r->h);
        if (r->fence_h) {
            CloseHandle(r->fence_h);
        }
#else
        close(r->h);
#endif
        g_free(r);
        return;
    }
#ifdef _WIN32
    UnmapViewOfFile(r->ptr);
    CloseHandle(r->h);
#else
    munmap(r->ptr, r->size);
    close(r->h);
#endif
    d->retained -= r->size;
    g_free(r);
}

/* A handle the queue owns, with no more rights than the viewer is given. */
DShmHandle dshm_region_dup(const DShmRegion *r, uint32_t access)
{
#ifdef _WIN32
    HANDLE self = GetCurrentProcess(), dup;
    DWORD rights = SECTION_MAP_READ | SECTION_QUERY;

    if (r->gpu) {
        /* a shared D3D12 object, which has no section rights to narrow */
        return DuplicateHandle(self, r->h, self, &dup, 0, FALSE,
                               DUPLICATE_SAME_ACCESS) ? dup : NULL;
    }
    if (access & DSHM_ACCESS_WRITE) {
        /* D3D12 heap import needs EXECUTE and READ_CONTROL too. */
        rights |= SECTION_MAP_WRITE | SECTION_MAP_EXECUTE | READ_CONTROL;
    }
    if (!DuplicateHandle(self, r->h, self, &dup, rights, FALSE, 0)) {
        return NULL;
    }
    return dup;
#else
    return qemu_dup(r->h);
#endif
}

#ifdef _WIN32
/* The slot's shared fence, for its own REGION; NULL if it has none. */
HANDLE dshm_region_dup_fence(const DShmRegion *r)
{
    HANDLE self = GetCurrentProcess(), dup;

    if (!r->fence_h ||
        !DuplicateHandle(self, r->fence_h, self, &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return NULL;
    }
    return dup;
}
#endif
