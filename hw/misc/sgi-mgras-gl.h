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
/* The answer to the 0xa104 current-attribute query for index. */
uint32_t sgi_mgras_gl_attribute_answer(SGIMgrasState *s, uint32_t index);
/* Select the GL state a completed context image names. */
void sgi_mgras_gl_context_loaded(SGIMgrasState *s);
/* The GE ports, through which the kernel copies GE external RAM. */
#define SGI_MGRAS_GE_PORTS      0x058000
#define SGI_MGRAS_GE_PORTS_END  0x058400
/* GL floats travel as the IEEE single words the client sent. */
#define VMSTATE_SGI_GL_FLOATS(_f, _s, _n, _v) \
    VMSTATE_ARRAY(_f, _s, _n, _v, vmstate_info_uint32, float)
/* The open primitive, in the layout of the stream since version 15. */
extern const VMStateDescription vmstate_sgi_mgras_gl_primitive;
extern const VMStateDescription vmstate_sgi_mgras_gl_context;
extern const VMStateDescription vmstate_sgi_mgras_gl_context_v20;
extern const VMStateDescription vmstate_sgi_mgras_gl_context_v21;
extern const VMStateDescription vmstate_sgi_mgras_gl_context_v22;

/* Note GE-port traffic the per-context state cannot follow. */
void sgi_mgras_gl_ge_port_access(SGIMgrasState *s, hwaddr addr, bool write);
/* The command parser's position, to set aside and take up again. */
void sgi_mgras_cmd_parser_save(SGIMgrasState *s, SGIMgrasCmdParser *p);
void sgi_mgras_cmd_parser_load(SGIMgrasState *s, const SGIMgrasCmdParser *p);
/* Whether a migrated parser position is in range. */
bool sgi_mgras_cmd_parser_valid(uint32_t cls, uint32_t reg,
                                uint32_t words_remaining,
                                uint32_t payload_count);
/* Whether migrated GL state is in range. */
bool sgi_mgras_gl_post_load(SGIMgrasState *s);

#endif
