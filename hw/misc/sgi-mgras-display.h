/* SPDX-License-Identifier: BSD-3-Clause */
/* MGRAS host scanout and display surface hooks. */
#ifndef HW_MISC_SGI_MGRAS_DISPLAY_H
#define HW_MISC_SGI_MGRAS_DISPLAY_H

#include "hw/misc/sgi-mgras.h"

/* The scanout's resolve_row hook: display line y as 0x00RRGGBB pixels. */
void sgi_mgras_display_resolve_row(void *opaque, unsigned y, uint32_t *dst);
/* Hand a pending whole-frame change to the scanout as dirty lines. */
void sgi_mgras_display_flush(SGIMgrasState *s);
/*
 * The display mode: the board's screen, and the retrace period the VC3
 * timing tables and the DAC's pixel clock give. Tables that give no frame,
 * or one shorter than the scanout's shortest retrace period, give the
 * default timing instead, and the return value says why; NULL otherwise.
 */
const char *sgi_mgras_display_timing(SGIMgrasState *s, SGIScanoutMode *mode);
/* The stock mode the model runs before the guest loads a timing table. */
void sgi_mgras_display_default_timing(SGIMgrasState *s, SGIScanoutMode *mode);
/* The window ID scanout uses at (x, y), from the main or overlay table. */
uint8_t sgi_mgras_display_did(SGIMgrasState *s, bool overlay, unsigned int x,
                              unsigned int y);

#endif
