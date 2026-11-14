/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Make and inspect CHD files for QEMU's qemu-iotests/tests/chd, so that the
 * test generates its images and the tree carries no binary blobs.
 *
 *   chd-fixture create OUT.chd RAW CODEC[,CODEC...] [HUNK_BYTES]
 *   chd-fixture info FILE.chd
 *
 * CODEC is lzma, zlib, huff, flac, zstd or none. info prints the version,
 * the hunk size, the codecs, and whether the file records a SHA-1.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qemu-chd-shim.h"

#define FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

static int parse_codecs(char *list, uint32_t codecs[4])
{
    int n = 0;

    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        uint32_t c;

        if (n == 4) {
            return -1;
        } else if (!strcmp(tok, "lzma")) {
            c = FOURCC('l', 'z', 'm', 'a');
        } else if (!strcmp(tok, "zlib")) {
            c = FOURCC('z', 'l', 'i', 'b');
        } else if (!strcmp(tok, "huff")) {
            c = FOURCC('h', 'u', 'f', 'f');
        } else if (!strcmp(tok, "flac")) {
            c = FOURCC('f', 'l', 'a', 'c');
        } else if (!strcmp(tok, "zstd")) {
            c = FOURCC('z', 's', 't', 'd');
        } else if (!strcmp(tok, "none")) {
            c = 0;
        } else {
            return -1;
        }
        codecs[n++] = c;
    }
    return n ? 0 : -1;
}

static int cmd_create(char **argv, int argc)
{
    uint32_t codecs[4] = { 0 };
    uint32_t hunk = 4096;
    int err;

    if (argc > 3 && sscanf(argv[3], "%" SCNu32, &hunk) != 1) {
        hunk = 0;
    }
    if (parse_codecs(argv[2], codecs) || !hunk) {
        fprintf(stderr, "chd-fixture: bad codec list or hunk size\n");
        return 2;
    }
    err = chd_shim_create_from_raw(argv[0], argv[1], codecs, hunk);
    if (err) {
        fprintf(stderr, "chd-fixture: %s: %s\n", argv[0],
                chd_shim_strerror(err));
        return 1;
    }
    return 0;
}

/* Plain stdio underneath the shim, for a tool that never writes. */
static int info_read(void *opaque, uint64_t off, void *buf, size_t len,
                     size_t *actual)
{
    FILE *f = opaque;
    size_t n;

    if (fseek(f, (long)off, SEEK_SET)) {
        return 1;
    }
    n = fread(buf, 1, len, f);
    *actual = n;
    return n < len && ferror(f);
}

static int info_length(void *opaque, uint64_t *len)
{
    FILE *f = opaque;

    if (fseek(f, 0, SEEK_END)) {
        return 1;
    }
    *len = (uint64_t)ftell(f);
    return 0;
}

static int cmd_info(char **argv)
{
    static const uint8_t zero[20];
    ChdShimIO io = { .read_at = info_read, .length = info_length };
    ChdShimFile *chd;
    ChdShimInfo info;
    FILE *f = fopen(argv[0], "rb");
    int err;

    if (!f) {
        perror(argv[0]);
        return 1;
    }
    err = chd_shim_open(&chd, &io, f, false, NULL);
    if (err) {
        fprintf(stderr, "chd-fixture: %s: %s\n", argv[0],
                chd_shim_strerror(err));
        fclose(f);
        return 1;
    }
    chd_shim_get_info(chd, &info);
    printf("version %" PRIu32 " hunk %" PRIu32 " codecs", info.version,
           info.hunk_bytes);
    for (int i = 0; i < 4; i++) {
        uint32_t c = info.codecs[i];

        if (c) {
            printf(" %c%c%c%c", c >> 24, (c >> 16) & 0xff, (c >> 8) & 0xff,
                   c & 0xff);
        }
    }
    printf(" sha1 %s\n", memcmp(info.sha1, zero, 20) ? "recorded" : "none");
    chd_shim_close(chd);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "create")) {
        return cmd_create(argv + 2, argc - 2);
    }
    if (argc == 3 && !strcmp(argv[1], "info")) {
        return cmd_info(argv + 2);
    }
    fprintf(stderr, "usage: chd-fixture create OUT.chd RAW CODECS [HUNK]\n"
                    "       chd-fixture info FILE.chd\n");
    return 2;
}
