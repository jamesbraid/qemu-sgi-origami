/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The configuration NVRAM of an SGI Origin 200 or Origin 2000 module system
 * controller: one 2048-byte store on the module's management I2C bus.
 *
 * NVRAM address A answers at seven-bit I2C address 0x50 + (A >> 8) with the
 * one-byte word address A & 0xff, as a 24C16-class EEPROM does. Addresses
 * 0x700-0x7ff are reserved for the controller; 0x700-0x70d hold its validity
 * marker, password, debug-switch values, configuration byte, module number,
 * self-test flags and partition, domain, cluster and cell numbers.
 */

#ifndef HW_NVRAM_SGI_ELSC_NVRAM_H
#define HW_NVRAM_SGI_ELSC_NVRAM_H

#define TYPE_SGI_ELSC_NVRAM "sgi-elsc-nvram"

#define SGI_ELSC_NVRAM_FIRST_ADDR   0x50
#define SGI_ELSC_NVRAM_BLOCKS       8
#define SGI_ELSC_NVRAM_BLOCK_BYTES  256
#define SGI_ELSC_NVRAM_BYTES \
    (SGI_ELSC_NVRAM_BLOCKS * SGI_ELSC_NVRAM_BLOCK_BYTES)

/* The controller's block, and the fields it initializes at power-up. */
#define SGI_ELSC_NVRAM_CONTROLLER_BLOCK 7
#define SGI_ELSC_NVRAM_MARKER           0x700
#define SGI_ELSC_NVRAM_PASSWORD         0x701
#define SGI_ELSC_NVRAM_CONFIG           0x707
#define SGI_ELSC_NVRAM_MODULE           0x708
#define SGI_ELSC_NVRAM_CLOCK            0x70b
#define SGI_ELSC_NVRAM_MARKER_VALUE     0x37

/* The configuration byte's automatic power-up bit. */
#define SGI_ELSC_NVRAM_CONFIG_AUTO_POWER 0x02

/*
 * The clock-select default a controller writes at 0x70b on first power-up
 * for the full-clock midplane type, which the Origin 200 build always
 * assumes. The same byte is the domain on Origin 2000.
 */
#define SGI_ELSC_NVRAM_CLOCK_FULL       0x78

/*
 * Fill the SGI_ELSC_NVRAM_BYTES at @image with the state a master controller
 * leaves after its first power-up: zeroes, then at 0x700 the validity marker,
 * the password "none", zero debug-switch values, configuration byte @config,
 * module number @module, zero self-test flags and partition, and @clock at
 * 0x70b.
 * Plain bytes, so a host tool creating a machine's store writes what an
 * unbacked store starts with.
 */
void sgi_elsc_nvram_fresh_image(uint8_t *image, uint8_t config,
                                uint8_t module, uint8_t clock);

#endif /* HW_NVRAM_SGI_ELSC_NVRAM_H */
