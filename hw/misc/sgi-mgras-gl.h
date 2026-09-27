/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_GL_H
#define HW_MISC_SGI_MGRAS_GL_H

#include "hw/misc/sgi-mgras.h"

/* Whether a main-port header is an OpenGL packet the model executes. */
bool sgi_mgras_gl_header(uint32_t header);
/* Execute one such packet once its payload has arrived. */
void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words);
void sgi_mgras_gl_reset(SGIMgrasState *s);

#endif
