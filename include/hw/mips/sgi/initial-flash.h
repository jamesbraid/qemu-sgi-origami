/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The contents a new machine's flash parts start with, built from plain
 * values and a decoded PROM payload. Nothing here touches QOM, devices or
 * machine state, so a host tool can build the same bytes the machine does.
 *
 * Copyright (c) 2026 James Braid
 */

#ifndef HW_MIPS_SGI_INITIAL_FLASH_H
#define HW_MIPS_SGI_INITIAL_FLASH_H

/* The Hub's two fixed clock rates: hardwired, not configuration. */
#define SN0_HUB_CLOCK_HZ     0x55d4a80ULL
#define SN0_HUB_RTC_CLOCK_HZ 0x1312d0ULL

/* The IP27 boot flash's erase sector. */
#define SGI_IP27_FLASH_SECTOR_SIZE 0x10000

/* The IP27 configuration block, at flash offset 0x60. */
#define SGI_IP27_CONFIG_OFFSET              0x60
#define SGI_IP27_CONFIG_SIZE                0x50
/* "ip27conf" */
#define SGI_IP27_CONFIG_MAGIC               0x69703237636f6e66ULL
#define SGI_IP27_CONFIG_MAGIC_OFFSET        0x08
#define SGI_IP27_CONFIG_FPROM_CYC_OFFSET    0x2c
#define SGI_IP27_CONFIG_MACHINE_TYPE_OFFSET 0x30
#define SGI_IP27_CONFIG_CHECKSUM_OFFSET     0x34
#define SGI_IP27_CONFIG_FPROM_WR_OFFSET     0x3c

/* The factory-fresh IP27 environment log: its sector and label. */
#define SGI_IP27_PROMLOG_SECTOR     14
#define SGI_IP27_PROMLOG_MAGIC      0x504c4f47      /* "PLOG" */
#define SGI_IP27_PROMLOG_VERSION    1

/*
 * The IP35 boot flash's two 8 MiB PROM positions back the lower and upper
 * halves of its 16 MiB storage. The PROM's blank-media formatter writes a
 * 28-byte outer label into position one, at this position-relative offset.
 */
#define SGI_IP35_FLASH_POSITIONS        2
#define SGI_IP35_LABEL_OFFSET           0x1e0000
#define SGI_IP35_LABEL_SIZE             28
#define SGI_IP35_LABEL_MAGIC            0x504c4f49  /* "PLOI" */
#define SGI_IP35_LABEL_FORMAT           1
#define SGI_IP35_LABEL_GENERATION       1

/*
 * What a product and its processor put into the IP27 configuration block.
 * The processor values only reach the default template; a supplied PROM
 * image keeps its own.
 */
typedef struct SGIIP27FlashValues {
    uint32_t machine_type;
    uint32_t read_cycles;
    uint32_t write_cycles;
    uint32_t cpu_mode_bits;
    uint64_t cpu_clock_hz;
} SGIIP27FlashValues;

/*
 * Fill @flash_size bytes at @flash: the default template, then the first
 * @payload_size bytes of @payload over it (none when @payload is NULL),
 * then the product's three configuration-block values patched into
 * whichever configuration block is present.
 */
void sgi_ip27_initial_flash(uint8_t *flash, size_t flash_size,
                            const SGIIP27FlashValues *values,
                            const uint8_t *payload, size_t payload_size);

/*
 * What a product and its processor module put into the IP35 configuration
 * block, which sits where the IP27 one does and has its layout.
 */
typedef struct SGIIP35FlashValues {
    uint32_t machine_type;
    uint32_t cpu_mode_word;
    uint32_t cpu_clock_hz;
} SGIIP35FlashValues;

/*
 * Write @values into the IP35 configuration block of the PROM image at
 * @flash, with a flash program count of one, and set the block's
 * check_sum_adj so its byte sum is what it was: the PROM checks that sum,
 * as IP27 firmware does. The image need not carry the block's magic.
 */
void sgi_ip35_flash_configure(uint8_t *flash,
                              const SGIIP35FlashValues *values);

/*
 * Fill @flash_size bytes at @flash as a blank IP35 boot flash holding
 * @payload in position zero and the formatter's outer label in position
 * one, everything else erased.
 */
void sgi_ip35_initial_flash(uint8_t *flash, size_t flash_size,
                            const uint8_t *payload, size_t payload_size);

/*
 * Fill @flash_size bytes at @flash as a new IP30 boot flash: @payload at
 * offset zero and every other byte erased. IP30 flash carries no
 * configuration block or label the machine formats for the PROM.
 */
void sgi_ip30_initial_flash(uint8_t *flash, size_t flash_size,
                            const uint8_t *payload, size_t payload_size);

/* The BASEIO and GIGAchannel I/O PROM flash part. */
#define SGI_IO6_FLASH_SIZE  0x100000

/*
 * Fill @flash_size bytes at @flash with @file, the I/O PROM exactly as
 * supplied -- container header and compressed payload included, the form
 * the IP27 PROM's loader reads from the part -- and erased bytes after it.
 */
void sgi_io6_initial_flash(uint8_t *flash, size_t flash_size,
                           const uint8_t *file, size_t file_size);

#endif /* HW_MIPS_SGI_INITIAL_FLASH_H */
