/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The contents a new machine's flash parts start with. Only plain values
 * go in, so the machine and a host tool build the same bytes.
 *
 * Copyright (c) 2026 James Braid
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/mips/sgi/initial-flash.h"

/*
 * The IP27 PROM jump table at flash offset zero: ten entries, real
 * hardware's own layout.
 */
#define SGI_IP27_FLASH_ENTRIES          10
#define SGI_IP27_FLASH_ENTRY_STRIDE     8

/*
 * The default IP27 flash template: entry zero spins (an unprogrammed reset
 * vector), the rest return, plus a configuration block and factory-fresh
 * environment log in the real IP27 firmware's own layout. A payload
 * shorter than the flash overlays only its own bytes, so a short image
 * still finds these pre-formatted underneath.
 */
static void sgi_ip27_flash_template(uint8_t *flash, size_t flash_size,
                                    const SGIIP27FlashValues *values)
{
    static const uint32_t jr_ra = 0x03e00008;          /* jr $31 */
    static const uint32_t branch_to_self = 0x1000ffff; /* b . */
    uint8_t *config = flash + SGI_IP27_CONFIG_OFFSET;
    uint8_t *log;
    int i;

    g_assert(flash_size >=
             (SGI_IP27_PROMLOG_SECTOR + 1) * SGI_IP27_FLASH_SECTOR_SIZE);
    memset(flash, 0xff, flash_size);

    for (i = 0; i < SGI_IP27_FLASH_ENTRIES; i++) {
        uint8_t *entry = flash + i * SGI_IP27_FLASH_ENTRY_STRIDE;

        stl_be_p(entry, i == 0 ? branch_to_self : jr_ra);
        stl_be_p(entry + 4, 0);                    /* the delay slot */
    }

    /* The field order and widths are the IP27 firmware's. */
    stl_be_p(config + 0x00, 0xb);                  /* timing constant */
    stl_be_p(config + 0x04, values->cpu_mode_bits);
    stq_be_p(config + SGI_IP27_CONFIG_MAGIC_OFFSET, SGI_IP27_CONFIG_MAGIC);
    stq_be_p(config + 0x10, values->cpu_clock_hz);
    stq_be_p(config + 0x18, SN0_HUB_CLOCK_HZ);
    stq_be_p(config + 0x20, SN0_HUB_RTC_CLOCK_HZ);
    stl_be_p(config + 0x28, 1);                    /* error correction on */
    stl_be_p(config + SGI_IP27_CONFIG_FPROM_CYC_OFFSET, values->read_cycles);
    stl_be_p(config + SGI_IP27_CONFIG_MACHINE_TYPE_OFFSET,
             values->machine_type);
    /* checksum adjustment */
    stl_be_p(config + SGI_IP27_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + 0x38, 0);                    /* flash program count */
    stl_be_p(config + SGI_IP27_CONFIG_FPROM_WR_OFFSET, values->write_cycles);
    stl_be_p(config + 0x40, 6);                    /* firmware version */
    stl_be_p(config + 0x44, 11);                   /* firmware revision */
    stl_be_p(config + 0x48, 0);                    /* configuration flags */

    /* The factory-fresh environment log; an erased first entry ends it. */
    log = flash + SGI_IP27_PROMLOG_SECTOR * SGI_IP27_FLASH_SECTOR_SIZE;
    stl_be_p(log + 0x10, SGI_IP27_PROMLOG_MAGIC);
    stl_be_p(log + 0x14, SGI_IP27_PROMLOG_VERSION);
    stl_be_p(log + 0x18, 1);                       /* initial sequence */
}

static uint8_t sgi_ip27_config_byte_sum(const uint8_t *config)
{
    uint8_t sum = 0;
    unsigned int i;

    for (i = 0; i < SGI_IP27_CONFIG_SIZE; i++) {
        sum += config[i];
    }
    return sum;
}

/*
 * The flasher's contract: the three product values and the fitted
 * processor's mode word and clock in, every other field and the old block
 * sum preserved; no IP27 signature, no touch. Runs whether the
 * configuration block present is the default template's or a real PROM
 * image's -- every stock PROM image carries one at this same offset with
 * this same magic. A stock image is built for 180 MHz with 1 MiB of cache,
 * so the processor fields are those the PROM's own reconfiguration writes
 * for any other module.
 */
static void sgi_ip27_flash_configure(uint8_t *flash,
                                     const SGIIP27FlashValues *values)
{
    uint8_t *config = flash + SGI_IP27_CONFIG_OFFSET;
    uint8_t old_sum;
    uint8_t new_sum;

    if (ldq_be_p(config + SGI_IP27_CONFIG_MAGIC_OFFSET) !=
        SGI_IP27_CONFIG_MAGIC) {
        return;
    }

    old_sum = sgi_ip27_config_byte_sum(config);
    stl_be_p(config + SGI_IP27_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + 0x04, values->cpu_mode_bits);
    stq_be_p(config + 0x10, values->cpu_clock_hz);
    stl_be_p(config + SGI_IP27_CONFIG_FPROM_CYC_OFFSET, values->read_cycles);
    stl_be_p(config + SGI_IP27_CONFIG_MACHINE_TYPE_OFFSET,
             values->machine_type);
    stl_be_p(config + SGI_IP27_CONFIG_FPROM_WR_OFFSET, values->write_cycles);
    new_sum = sgi_ip27_config_byte_sum(config);
    stl_be_p(config + SGI_IP27_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

/*
 * Origin 300 runs have always flashed check_sum_adj 0x9c over the stock
 * image's 0, which is exactly the byte-sum-preserving value for the fields
 * below; a Fuel run with machine type 2 and that same 0x9c failed the
 * PROM's checksum, and one with 0x9b passed.
 */
void sgi_ip35_flash_configure(uint8_t *flash,
                              const SGIIP35FlashValues *values)
{
    uint8_t *config = flash + SGI_IP27_CONFIG_OFFSET;
    uint8_t old_sum = sgi_ip27_config_byte_sum(config);
    uint8_t new_sum;

    stl_be_p(config + SGI_IP27_CONFIG_CHECKSUM_OFFSET, 0);
    stl_be_p(config + 0x04, values->cpu_mode_word);
    /* The low word of the 64-bit processor frequency. */
    stl_be_p(config + 0x14, values->cpu_clock_hz);
    stl_be_p(config + SGI_IP27_CONFIG_MACHINE_TYPE_OFFSET,
             values->machine_type);
    stl_be_p(config + 0x38, 1);                    /* flash program count */
    new_sum = sgi_ip27_config_byte_sum(config);
    stl_be_p(config + SGI_IP27_CONFIG_CHECKSUM_OFFSET,
             (uint8_t)(old_sum - new_sum));
}

void sgi_ip27_initial_flash(uint8_t *flash, size_t flash_size,
                            const SGIIP27FlashValues *values,
                            const uint8_t *payload, size_t payload_size)
{
    g_assert(payload_size <= flash_size);
    sgi_ip27_flash_template(flash, flash_size, values);
    if (payload) {
        memcpy(flash, payload, payload_size);
    }
    sgi_ip27_flash_configure(flash, values);
}

void sgi_io6_initial_flash(uint8_t *flash, size_t flash_size,
                           const uint8_t *file, size_t file_size)
{
    g_assert(file_size <= flash_size);
    memset(flash, 0xff, flash_size);
    if (file) {
        memcpy(flash, file, file_size);
    }
}

void sgi_ip30_initial_flash(uint8_t *flash, size_t flash_size,
                            const uint8_t *payload, size_t payload_size)
{
    g_assert(payload_size <= flash_size);
    memset(flash, 0xff, flash_size);
    if (payload) {
        memcpy(flash, payload, payload_size);
    }
}

void sgi_ip35_initial_flash(uint8_t *flash, size_t flash_size,
                            const uint8_t *payload, size_t payload_size)
{
    size_t position = flash_size / SGI_IP35_FLASH_POSITIONS;
    uint8_t *label = flash + position + SGI_IP35_LABEL_OFFSET;

    g_assert(payload_size <= position);
    g_assert(SGI_IP35_LABEL_OFFSET + SGI_IP35_LABEL_SIZE <= position);
    memset(flash, 0xff, flash_size);
    if (payload) {
        memcpy(flash, payload, payload_size);
    }

    /* Bytes 0x00-0x0f of the record stay erased. */
    stl_be_p(label + 0x10, SGI_IP35_LABEL_MAGIC);
    stl_be_p(label + 0x14, SGI_IP35_LABEL_FORMAT);
    stl_be_p(label + 0x18, SGI_IP35_LABEL_GENERATION);
}
