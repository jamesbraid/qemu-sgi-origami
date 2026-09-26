/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_DISPLAY_H
#define HW_MISC_SGI_MGRAS_DISPLAY_H

#include "hw/misc/sgi-mgras.h"
#include "ui/console.h"

extern const GraphicHwOps sgi_mgras_display_ops;
void sgi_mgras_display_ensure_surface(SGIMgrasState *s);
/* The window ID scanout uses at (x, y), from the main or overlay table. */
uint8_t sgi_mgras_display_did(SGIMgrasState *s, bool overlay, unsigned int x,
                              unsigned int y);

#endif
