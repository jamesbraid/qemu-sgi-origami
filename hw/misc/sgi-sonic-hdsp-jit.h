/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Sonic HDSP's JIT engine, sgi-sonic-hdsp-jit.c. Opaque pointers only:
 * the crate's header and the interpreter's cannot share a translation unit.
 */
#ifndef HW_MISC_SGI_SONIC_HDSP_JIT_H
#define HW_MISC_SGI_SONIC_HDSP_JIT_H

typedef struct SonicDspJit SonicDspJit;

/* The RAM arrays the JIT runs in place; owner is passed to the callbacks. */
SonicDspJit *sonic_dsp_jit_create(void *owner, uint32_t *xmem, uint32_t *ymem,
                                  uint32_t *pmem, uint32_t *ext);
void sonic_dsp_jit_destroy(SonicDspJit *j);
void sonic_dsp_jit_reset(SonicDspJit *j, uint32_t boot_pc);
void sonic_dsp_jit_run(SonicDspJit *j, uint64_t cycles);
void sonic_dsp_jit_irq_raise(SonicDspJit *j, uint32_t vector, unsigned ipl,
                             bool level);
void sonic_dsp_jit_irq_lower(SonicDspJit *j, uint32_t vector);
bool sonic_dsp_jit_irq_pending(SonicDspJit *j, uint32_t vector);
void sonic_dsp_jit_invalidate(SonicDspJit *j);
void sonic_dsp_jit_p_write(SonicDspJit *j, uint32_t addr, uint32_t val);

/* Implemented by the device: words past the arrays (space: X 0, Y 1, P 2). */
uint32_t sonic_dsp_ext_read(void *owner, unsigned space, uint32_t addr);
void sonic_dsp_ext_write(void *owner, unsigned space, uint32_t addr,
                         uint32_t val);

#endif
