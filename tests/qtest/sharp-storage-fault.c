/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Fault injection for the marker directory, confined to the QEMU child. */
#include "qemu/osdep.h"
#include <sys/syscall.h>

int fsync(int fd)
{
    static unsigned directory_syncs;
    const char *directory = getenv("SHARP_SYNC_DIRECTORY");
    const char *fail_at = getenv("SHARP_FAIL_DIR_SYNC");
    struct stat actual, target;
    unsigned count;
    int ret;

    if (directory && fail_at && fstat(fd, &actual) == 0 &&
        S_ISDIR(actual.st_mode) && stat(directory, &target) == 0 &&
        actual.st_dev == target.st_dev && actual.st_ino == target.st_ino) {
        count = __atomic_add_fetch(&directory_syncs, 1, __ATOMIC_RELAXED);
        if ((count == 1 && !strcmp(fail_at, "1")) ||
            (count == 2 && !strcmp(fail_at, "2"))) {
            fprintf(stderr, "sharp-storage-fault: directory sync %u failed\n",
                    count);
            errno = EIO;
            return -1;
        }
        ret = syscall(SYS_fsync, fd);
        fprintf(stderr, "sharp-storage-fault: directory sync %u returned %d\n",
                count, ret);
        return ret;
    }
    return syscall(SYS_fsync, fd);
}

int unlink(const char *path)
{
    const char *marker = getenv("SHARP_FAILURE_MARKER");
    const char *fail = getenv("SHARP_FAIL_UNLINK");

    if (marker && fail && !strcmp(fail, "1") && !strcmp(path, marker)) {
        fprintf(stderr, "sharp-storage-fault: marker unlink failed\n");
        errno = EIO;
        return -1;
    }
    return syscall(SYS_unlinkat, AT_FDCWD, path, 0);
}

int open(const char *path, int flags, ...)
{
    const char *marker = getenv("SHARP_FAILURE_MARKER");
    const char *fail = getenv("SHARP_FAIL_MARKER_RESTORE");
    mode_t mode = 0;

    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap;

        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (marker && fail && !strcmp(fail, "1") && !strcmp(path, marker) &&
        (flags & O_CREAT) && !(flags & O_EXCL)) {
        fprintf(stderr, "sharp-storage-fault: "
                "marker restoration open failed\n");
        errno = EIO;
        return -1;
    }
    return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}
