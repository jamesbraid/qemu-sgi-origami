/*
 * Synthetic L1 identity areas.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_MISC_SGI_L1_IDENTITY_H
#define HW_MISC_SGI_L1_IDENTITY_H

#define SGI_L1_IA_EOF 0xc1

typedef struct SGIL1CPUContext {
    uint32_t mode_word;
    uint32_t cpu_mhz;
    uint32_t hub_mhz;
} SGIL1CPUContext;

/* The chassis area: both identity strings the placeholder "NA". */
GByteArray *sgi_l1_identity_chassis_area(void);

/*
 * A board area with the given name, part and revision and a marked
 * synthetic serial (component @letter, instance @index) unless
 * @serial_text gives one. @mac and @cpu_context are optional. The result
 * sums to zero.
 */
GByteArray *sgi_l1_identity_board_area(const char *name, const char *part,
                                       const char *revision,
                                       const uint8_t *mac, char letter,
                                       unsigned int index,
                                       const char *serial_text,
                                       const SGIL1CPUContext *cpu_context);

#endif
