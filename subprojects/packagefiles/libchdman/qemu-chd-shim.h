/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A C interface to MAME's chd_file, for QEMU's block/chd.c.
 *
 * MAME's CHD core is C++ and reads and writes its file through an object it is
 * given. This header is all of it that QEMU sees: opaque handles, plain
 * integers, and a table of I/O callbacks that MAME calls for every byte it
 * moves, so the caller decides where the bytes live.
 *
 * Every function returns 0, or MAME's chd_file::error value on failure (see
 * chd_shim_strerror). None of them lets a C++ exception out.
 */
#ifndef QEMU_CHD_SHIM_H
#define QEMU_CHD_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned when one of the I/O callbacks failed. */
#define CHD_SHIM_ERR_IO 1000

typedef struct ChdShimFile ChdShimFile;

/*
 * How MAME reaches the bytes of a CHD. Each callback returns 0 or nonzero for
 * an I/O error. read_at may return fewer bytes than asked only at end of
 * file. release runs once, when the ChdShimFile is closed.
 */
typedef struct ChdShimIO {
    int (*read_at)(void *opaque, uint64_t offset, void *buf, size_t len,
                   size_t *actual);
    int (*write_at)(void *opaque, uint64_t offset, const void *buf, size_t len);
    int (*length)(void *opaque, uint64_t *len);
    void (*release)(void *opaque);
} ChdShimIO;

typedef struct ChdShimInfo {
    uint32_t version;
    uint32_t hunk_bytes;
    uint32_t hunk_count;
    uint32_t unit_bytes;
    uint64_t logical_bytes;
    uint32_t codecs[4];     /* FourCC, 0 for none */
    bool compressed;
    bool has_parent;        /* the file asks for a parent (parent_sha1 set) */
    bool is_hd;             /* carries hard-disk geometry metadata */
    uint8_t sha1[20];
    uint8_t raw_sha1[20];
    uint8_t parent_sha1[20];
} ChdShimInfo;

typedef void (*ChdShimProgress)(void *opaque, uint64_t done, uint64_t total);

const char *chd_shim_strerror(int err);

/*
 * Open a CHD. parent is required for a diff and must outlive the result; it
 * is not closed with it. writable needs a version 5 file.
 */
int chd_shim_open(ChdShimFile **out, const ChdShimIO *io, void *opaque,
                  bool writable, ChdShimFile *parent);

/*
 * Create a CHD that stores only what differs from parent: uncompressed, with
 * the parent's size and hunk size, and with the parent's metadata copied in
 * (MAME's create does not). The new file is open for writing.
 */
int chd_shim_create_diff(ChdShimFile **out, const ChdShimIO *io, void *opaque,
                         ChdShimFile *parent);

void chd_shim_close(ChdShimFile *f);

void chd_shim_get_info(ChdShimFile *f, ChdShimInfo *info);
int chd_shim_hd_geometry(ChdShimFile *f, uint32_t *cylinders, uint32_t *heads,
                         uint32_t *sectors, uint32_t *bytes_per_sector);

int chd_shim_read_hunk(ChdShimFile *f, uint32_t hunk, void *buf);
int chd_shim_write_hunk(ChdShimFile *f, uint32_t hunk, const void *buf);
/* Byte-granular access; a write that covers part of a hunk reads the rest. */
int chd_shim_read_bytes(ChdShimFile *f, uint64_t offset, void *buf,
                        uint32_t len);
int chd_shim_write_bytes(ChdShimFile *f, uint64_t offset, const void *buf,
                         uint32_t len);
/*
 * compressor is the hunk's codec FourCC, or MAME's pseudo codec for a
 * stored/self/parent hunk; compbytes is 0 for a hunk the file does not hold.
 */
int chd_shim_hunk_info(ChdShimFile *f, uint32_t hunk, uint32_t *compressor,
                       uint32_t *compbytes);

/*
 * Compress the raw hard disk image raw into a new CHD at out (UTF-8 paths),
 * with the given codecs (FourCC, 0 for none) and hunk size, and record the
 * 16-head, 63-sector geometry that chdman createhd uses. Only the test
 * fixtures need this.
 */
int chd_shim_create_from_raw(const char *out, const char *raw,
                             const uint32_t codecs[4], uint32_t hunk_bytes);

#ifdef __cplusplus
}
#endif

#endif
