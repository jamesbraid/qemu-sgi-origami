/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_MGRAS_GL_H
#define HW_MISC_SGI_MGRAS_GL_H

#include "hw/misc/sgi-mgras.h"
#include "migration/vmstate.h"

/* Whether a main-port header is an OpenGL packet the model executes. */
bool sgi_mgras_gl_header(uint32_t header);
/* Execute one such packet once its payload has arrived. */
void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words);
void sgi_mgras_gl_reset(SGIMgrasState *s);
/* Select the GL state a completed context image names. */
void sgi_mgras_gl_context_loaded(SGIMgrasState *s);
/* The GE ports, through which the kernel copies GE external RAM. */
#define SGI_MGRAS_GE_PORTS      0x058000
#define SGI_MGRAS_GE_PORTS_END  0x058400
extern const VMStateDescription vmstate_sgi_mgras_gl_context;

/* Note GE-port traffic the per-context state cannot follow. */
void sgi_mgras_gl_ge_port_access(SGIMgrasState *s, hwaddr addr, bool write);
/* Whether migrated GL state is in range. */
bool sgi_mgras_gl_post_load(SGIMgrasState *s);

#endif
