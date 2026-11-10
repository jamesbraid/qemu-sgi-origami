/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The standard INQUIRY data of scsi-hd-inquiry, read through one host
 * adapter (hw/mips/sgi/scsi-hd-inquiry.c).
 *
 * Each case names the device properties and the bytes 0 and 8 to 55 the
 * device must then return; the checks read the data with allocation
 * lengths of 36, 56 and 96 and compare the unit serial number VPD page
 * with the serial property. Each adapter's test supplies the transport,
 * the same SgiCdTransport the CD-ROM checks use (sgi-scsi-cd-test.h):
 * only its command callback is called here.
 */

#ifndef SGI_SCSI_INQUIRY_TEST_H
#define SGI_SCSI_INQUIRY_TEST_H

#include "sgi-scsi-cd-test.h"

#define SGI_INQ_VENDOR_OFF      8
#define SGI_INQ_REVISION_OFF    32
#define SGI_INQ_VS_OFF          36
#define SGI_INQ_VS_LEN          20
#define SGI_INQ_LEN             (SGI_INQ_VS_OFF + SGI_INQ_VS_LEN)

typedef struct SgiInquiryCase {
    const char *name;
    /* Device properties after the drive, joined by commas. */
    const char *props;
    const char *serial;
    uint8_t byte0;
    /* Bytes 8 to 55, exactly as the device must return them. */
    char fields[SGI_INQ_LEN - SGI_INQ_VENDOR_OFF + 1];
} SgiInquiryCase;

/*
 * The expected fields are the 48 bytes from 8 to 55 as concatenated
 * literals: vendor (8), product (16), revision (4), vendor specific (20).
 * The first case's values are short and space padded; the second's are as
 * wide as their fields, with an explicit byte string that includes 0x00
 * and 0xff (in octal, so no escape swallows the next character) and a
 * peripheral qualifier and type in byte 0; the third sets no inquiry-
 * property, so it shows scsi-hd's identity and zeros in bytes 36 to 55.
 */
static const SgiInquiryCase sgi_inquiry_cases[] = {
    {
        .name = "ascii",
        .props = "vendor=ACME,product=GENERIC-DISK,ver=1.2,"
                 "serial=SN-0123456789,"
                 "inquiry-vendor-specific=SN-0123456789",
        .serial = "SN-0123456789",
        .byte0 = 0x00,
        .fields = "ACME    " "GENERIC-DISK    " "1.2 "
                  "SN-0123456789       ",
    },
    {
        .name = "hex",
        .props = "vendor=VENDOR01,product=0123456789ABCDEF,ver=ABCD,"
                 "serial=Z9,inquiry-vendor-specific=hex:00ff0180,"
                 "inquiry-peripheral-qualifier=1,"
                 "inquiry-peripheral-type=0x0e",
        .serial = "Z9",
        .byte0 = 0x2e,
        .fields = "VENDOR01" "0123456789ABCDEF" "ABCD"
                  "\000\377\001\200"
                  "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
    },
    {
        .name = "default",
        .props = "ver=9,serial=PLAIN",
        .serial = "PLAIN",
        .byte0 = 0x00,
        .fields = "QEMU    " "QEMU HARDDISK   " "9   "
                  "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
    },
};

static inline uint8_t sgi_inquiry_run(const SgiCdTransport *t,
                                      const uint8_t *cdb, uint8_t *data,
                                      uint32_t len)
{
    memset(data, 0xcc, len);
    return sgi_cd_run(t, cdb, 6, data, len, false, NULL);
}

/* Bytes 0 to 3 and the additional length, then the identity fields. */
static inline void sgi_inquiry_expect(const SgiInquiryCase *c,
                                      const uint8_t *data, uint32_t len)
{
    uint32_t n = MIN(len, SGI_INQ_LEN);

    g_assert_cmphex(data[0], ==, c->byte0);
    g_assert_cmphex(data[1], ==, 0x00);
    /* From scsi-hd: SPC-3 as scsi_version 5, response format 2. */
    g_assert_cmphex(data[2], ==, 0x05);
    g_assert_cmphex(data[3], ==, 0x12);
    g_assert_cmpuint(data[4], ==, len > 36 ? len - 5 : 31);
    g_assert_cmpmem(data + SGI_INQ_VENDOR_OFF, n - SGI_INQ_VENDOR_OFF,
                    c->fields, n - SGI_INQ_VENDOR_OFF);
}

static inline void sgi_inquiry_check(const SgiCdTransport *t,
                                     const SgiInquiryCase *c)
{
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 0, 0 };
    uint8_t data[96];
    uint8_t vpd[64];
    size_t serial_len = strlen(c->serial);

    /* Shorter than the standard data, and partway into the vendor bytes. */
    cdb[4] = 12;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, 12), ==, SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, 12);
    cdb[4] = 40;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, 40), ==, SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, 40);

    /* CMDDT is not looked at: the standard data comes back, changed. */
    cdb[1] = 0x02;
    cdb[4] = SGI_INQ_LEN;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, SGI_INQ_LEN), ==,
                    SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, SGI_INQ_LEN);
    cdb[1] = 0x00;

    /* A page code without EVPD is refused, not answered with the data. */
    cdb[2] = 0x01;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, SGI_INQ_LEN), ==,
                    SGI_CD_CHECK_CONDITION);
    cdb[2] = 0x00;

    /* Exactly the standard data: no vendor-specific bytes asked for. */
    cdb[4] = 36;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, 36), ==, SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, 36);

    /* Through byte 55, the last vendor-specific byte. */
    cdb[4] = SGI_INQ_LEN;
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, SGI_INQ_LEN), ==,
                    SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, SGI_INQ_LEN);

    /* A longer transfer: the bytes past 55 stay zero. */
    cdb[4] = sizeof(data);
    g_assert_cmphex(sgi_inquiry_run(t, cdb, data, sizeof(data)), ==,
                    SGI_CD_GOOD);
    sgi_inquiry_expect(c, data, sizeof(data));
    for (size_t i = SGI_INQ_LEN; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0x00);
    }

    /* The serial number stays in the unit serial number VPD page. */
    cdb[1] = 0x01;
    cdb[2] = 0x80;
    cdb[4] = sizeof(vpd);
    g_assert_cmphex(sgi_inquiry_run(t, cdb, vpd, sizeof(vpd)), ==,
                    SGI_CD_GOOD);
    g_assert_cmphex(vpd[1], ==, 0x80);
    g_assert_cmpuint(vpd[3], ==, serial_len);
    g_assert_cmpmem(vpd + 4, serial_len, c->serial, serial_len);
}

#endif
