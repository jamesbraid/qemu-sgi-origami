/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The shm storage class of the display-shm producer: a sealed memfd on
 * Linux, an unlinked POSIX shared-memory object elsewhere. See
 * docs/interop/display-shm.rst.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/memfd.h"
#include "qemu/bswap.h"
#include "ui/display-shm-priv.h"

#include <sys/mman.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

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

/* A zeroed region of immutable size, read-only to the viewer unless told. */
DShmRegion *dshm_region_new(DisplayShm *d, uint32_t kind, size_t size,
                            bool writable_by_viewer, Error **errp)
{
    DShmRegion *r;
    void *p;
    int fd;

    size = ROUND_UP(MAX(size, 1), DSHM_REGION_ALIGN);
    if (d->retained + size > d->max_retained) {
        error_setg(errp, "display-shm: max-retained-bytes reached");
        return NULL;
    }
    fd = object_create(d, size, errp);
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
              (writable_by_viewer ? 0 : F_SEAL_FUTURE_WRITE) | F_SEAL_SEAL,
              errp)) {
        munmap(p, size);
        close(fd);
        return NULL;
    }
#endif
    r = g_new0(DShmRegion, 1);
    r->id = d->next_region_id++;
    r->kind = kind;
    r->access = writable_by_viewer ? DSHM_ACCESS_READ | DSHM_ACCESS_WRITE
                                   : DSHM_ACCESS_READ;
    r->size = size;
    r->ptr = p;
    r->fd = fd;
    r->refs = 1;
    d->retained += size;
    return r;
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
    munmap(r->ptr, r->size);
    close(r->fd);
    d->retained -= r->size;
    g_free(r);
}

int dshm_region_dup(const DShmRegion *r)
{
    return qemu_dup(r->fd);
}
