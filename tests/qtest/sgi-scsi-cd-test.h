/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The CD-ROM block length contract the SGI machines give scsi-cd, driven
 * through one host adapter.
 *
 * The drive powers on at 2048-byte logical blocks and reports them in an
 * eight-byte block descriptor; MODE SELECT switches it to 512, after which
 * MODE SENSE, READ CAPACITY and READ(10) all count 512-byte blocks.
 * Each adapter's test supplies the transport; the expectations here are
 * shared.
 *
 * Every 512-byte block of the medium is filled with its own number, so a
 * READ(10) counted in the wrong unit returns another block's bytes.
 */

#ifndef SGI_SCSI_CD_TEST_H
#define SGI_SCSI_CD_TEST_H

#include "qemu/osdep.h"
#include "qemu/bswap.h"

/* 64 blocks of 512, 16 of 2048: a quarter of the medium is 16 blocks. */
#define SGI_CD_BLOCKS_512       64
#define SGI_CD_BLOCKS_2048      (SGI_CD_BLOCKS_512 / 4)
#define SGI_CD_MEDIUM_BYTES     (SGI_CD_BLOCKS_512 * 512)
#define SGI_CD_DESCRIPTOR_BYTES 8
#define SGI_CD_FILL(block512)   ((uint8_t)(0x40 + (block512)))

#define SGI_CD_GOOD             0x00
#define SGI_CD_CHECK_CONDITION  0x02

typedef struct SgiCdTransport {
    /*
     * Run one command to the CD-ROM and return its SCSI status. @data holds
     * @len bytes going out when @out, or receives @len bytes coming in.
     * On CHECK CONDITION, fill @sense (18 bytes, fixed format), by
     * autosense or by a REQUEST SENSE of the transport's own.
     */
    uint8_t (*command)(void *opaque, const uint8_t *cdb, size_t cdb_len,
                       uint8_t *data, uint32_t len, bool out,
                       uint8_t *sense);
    void *opaque;
} SgiCdTransport;

/* A medium file whose 512-byte blocks hold their own numbers. */
static inline char *sgi_cd_medium_create(void)
{
    g_autofree uint8_t *medium = g_malloc(SGI_CD_MEDIUM_BYTES);
    char *path = NULL;
    int fd;

    for (unsigned i = 0; i < SGI_CD_BLOCKS_512; i++) {
        memset(medium + i * 512, SGI_CD_FILL(i), 512);
    }
    fd = g_file_open_tmp("sgi-cd-XXXXXX.iso", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, medium, SGI_CD_MEDIUM_BYTES), ==,
                    SGI_CD_MEDIUM_BYTES);
    close(fd);
    return path;
}

static inline uint8_t sgi_cd_run(const SgiCdTransport *t, const uint8_t *cdb,
                                 size_t cdb_len, uint8_t *data, uint32_t len,
                                 bool out, uint8_t *sense)
{
    uint8_t scratch[18];

    return t->command(t->opaque, cdb, cdb_len, data, len, out,
                      sense ? sense : scratch);
}

/* The block descriptor's length and block count, from MODE SENSE(6). */
static inline void sgi_cd_expect_descriptor(const SgiCdTransport *t,
                                            uint32_t length, uint32_t blocks)
{
    static const uint8_t sense6[6] = { 0x1a, 0, 0x3f, 0, 0xfe, 0 };
    static const uint8_t sense10[10] = { 0x5a, 0, 0x3f, 0, 0, 0, 0, 0, 16,
                                         0 };
    uint8_t mode[0xfe];

    memset(mode, 0xcc, sizeof(mode));
    g_assert_cmphex(sgi_cd_run(t, sense6, sizeof(sense6), mode, sizeof(mode),
                               false, NULL), ==, SGI_CD_GOOD);
    g_assert_cmpuint(mode[3], ==, SGI_CD_DESCRIPTOR_BYTES);
    g_assert_cmpuint(ldl_be_p(mode + 4) & 0xffffff, ==, blocks);
    g_assert_cmpuint(ldl_be_p(mode + 8) & 0xffffff, ==, length);

    /* MODE SENSE(10) carries the same descriptor after a longer header. */
    memset(mode, 0xcc, 16);
    g_assert_cmphex(sgi_cd_run(t, sense10, sizeof(sense10), mode, 16,
                               false, NULL), ==, SGI_CD_GOOD);
    g_assert_cmpuint(lduw_be_p(mode + 6), ==, SGI_CD_DESCRIPTOR_BYTES);
    g_assert_cmpuint(ldl_be_p(mode + 8) & 0xffffff, ==, blocks);
    g_assert_cmpuint(ldl_be_p(mode + 12) & 0xffffff, ==, length);
}

static inline void sgi_cd_expect_capacity(const SgiCdTransport *t,
                                          uint32_t length, uint32_t blocks)
{
    static const uint8_t read_capacity[10] = { 0x25 };
    uint8_t capacity[8];

    memset(capacity, 0xcc, sizeof(capacity));
    g_assert_cmphex(sgi_cd_run(t, read_capacity, sizeof(read_capacity),
                               capacity, sizeof(capacity), false, NULL),
                    ==, SGI_CD_GOOD);
    g_assert_cmpuint(ldl_be_p(capacity), ==, blocks - 1);
    g_assert_cmpuint(ldl_be_p(capacity + 4), ==, length);
}

/* READ(10) of one @length block at @lba, compared with the medium. */
static inline void sgi_cd_expect_read(const SgiCdTransport *t,
                                      uint32_t length, uint32_t lba)
{
    uint8_t cdb[10] = { 0x28 };
    g_autofree uint8_t *got = g_malloc(length);
    g_autofree uint8_t *want = g_malloc(length);
    uint32_t first = lba * (length / 512);

    stl_be_p(cdb + 2, lba);
    stw_be_p(cdb + 7, 1);
    for (uint32_t i = 0; i < length / 512; i++) {
        memset(want + i * 512, SGI_CD_FILL(first + i), 512);
    }
    memset(got, 0xcc, length);
    g_assert_cmphex(sgi_cd_run(t, cdb, sizeof(cdb), got, length, false, NULL),
                    ==, SGI_CD_GOOD);
    g_assert_cmpmem(got, length, want, length);
}

/*
 * MODE SELECT(6) with a header and one descriptor asking for @length, the
 * list the Octane PROM sends. @pf is the page format bit.
 */
static inline uint8_t sgi_cd_select(const SgiCdTransport *t, uint32_t length,
                                    bool pf)
{
    const uint8_t cdb[6] = { 0x15, pf ? 0x10 : 0, 0, 0, 12, 0 };
    uint8_t list[12] = { 0, 0, 0, SGI_CD_DESCRIPTOR_BYTES };

    list[9] = length >> 16;
    list[10] = length >> 8;
    list[11] = length;
    return sgi_cd_run(t, cdb, sizeof(cdb), list, sizeof(list), true, NULL);
}

/*
 * Power on: type 5, 2048-byte blocks in MODE SENSE and READ CAPACITY, and
 * no descriptor when DBD asks for none.
 */
static inline void sgi_cd_check_power_on(const SgiCdTransport *t)
{
    static const uint8_t tur[6] = { 0x00 };
    static const uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    static const uint8_t sense_dbd[6] = { 0x1a, 0x08, 0x3f, 0, 0xfe, 0 };
    uint8_t buf[0xfe];

    /* The power-on unit attention; which one it is does not matter here. */
    sgi_cd_run(t, tur, sizeof(tur), NULL, 0, false, NULL);

    memset(buf, 0xcc, 36);
    g_assert_cmphex(sgi_cd_run(t, inquiry, sizeof(inquiry), buf, 36, false,
                               NULL), ==, SGI_CD_GOOD);
    g_assert_cmphex(buf[0] & 0x1f, ==, 0x05);
    /* The product text names a CD-ROM, as QEMU's default identity does. */
    g_assert_nonnull(g_strstr_len((char *)buf + 16, 16, "CD-ROM"));

    sgi_cd_expect_descriptor(t, 2048, SGI_CD_BLOCKS_2048);
    memset(buf, 0xcc, sizeof(buf));
    g_assert_cmphex(sgi_cd_run(t, sense_dbd, sizeof(sense_dbd), buf,
                               sizeof(buf), false, NULL), ==, SGI_CD_GOOD);
    g_assert_cmpuint(buf[3], ==, 0);
    sgi_cd_expect_capacity(t, 2048, SGI_CD_BLOCKS_2048);
    sgi_cd_expect_read(t, 2048, SGI_CD_BLOCKS_2048 - 1);
}

/*
 * After MODE SELECT to 512, a READ(10) of a block beyond the first quarter
 * of the medium, before anything else: no READ CAPACITY has recounted the
 * last LBA for the new unit. Then MODE SENSE and READ CAPACITY in 512s.
 */
static inline void sgi_cd_check_select_512(const SgiCdTransport *t, bool pf)
{
    g_assert_cmphex(sgi_cd_select(t, 512, pf), ==, SGI_CD_GOOD);
    sgi_cd_expect_read(t, 512, SGI_CD_BLOCKS_512 - 1);
    sgi_cd_expect_read(t, 512, SGI_CD_BLOCKS_2048 + 1);
    sgi_cd_expect_descriptor(t, 512, SGI_CD_BLOCKS_512);
    sgi_cd_expect_capacity(t, 512, SGI_CD_BLOCKS_512);
}

#endif /* SGI_SCSI_CD_TEST_H */
