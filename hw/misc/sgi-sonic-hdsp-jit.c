/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The DSP56301 cores on the dsp56300 crate's JIT. Only this file sees the
 * crate's header, which clashes with the interpreter's DSP56300_REG_*.
 */

#include "qemu/osdep.h"
#include "sgi-sonic-hdsp-jit.h"
/* The archive's names for what the interpreter also calls these. */
#define dsp56300_reset dsp56300_jit_reset
#define dsp56300_run dsp56300_jit_run
#define dsp56300_step dsp56300_jit_step
#include <dsp56300.h>

#define JIT_END     0x1000000u
/* The sizes the device declares in include/hw/misc/sgi-sonic-hdsp.h. */
#define PMEM_WORDS  0x1000
#define XMEM_WORDS  0x800
#define YMEM_WORDS  0x800
#define EXT_BASE    0x400000
#define EXT_END     (EXT_BASE + 0x40000)

#define JIT_CALLBACKS(name, space) \
    static uint32_t name##_read(void *s, uint32_t addr) \
    { \
        return sonic_dsp_ext_read(s, space, addr); \
    } \
    static void name##_write(void *s, uint32_t addr, uint32_t val) \
    { \
        sonic_dsp_ext_write(s, space, addr, val); \
    }

JIT_CALLBACKS(jit_x, DSP56300_MEM_SPACE_X)
JIT_CALLBACKS(jit_y, DSP56300_MEM_SPACE_Y)
JIT_CALLBACKS(jit_p, DSP56300_MEM_SPACE_P)

static Dsp56300MemoryRegion buf(uint32_t lo, uint32_t hi, uint32_t *mem)
{
    return (Dsp56300MemoryRegion) {
        .start = lo, .end = hi, .kind = DSP56300_REGION_BUFFER,
        .data.buffer = { mem, 0 },
    };
}

static Dsp56300MemoryRegion cb(uint32_t lo, uint32_t hi, void *s,
                               uint32_t (*rd)(void *, uint32_t),
                               void (*wr)(void *, uint32_t, uint32_t))
{
    return (Dsp56300MemoryRegion) {
        .start = lo, .end = hi, .kind = DSP56300_REGION_CALLBACK,
        .data.callback = { s, rd, wr },
    };
}

SonicDspJit *sonic_dsp_jit_create(void *s, uint32_t *xmem, uint32_t *ymem,
                                  uint32_t *pmem, uint32_t *ext)
{
    /* Mirrors dsp_ext_read: X, Y and P share the one external array. */
    const Dsp56300MemoryRegion x[] = {
        buf(0, XMEM_WORDS, xmem),
        cb(XMEM_WORDS, EXT_BASE, s, jit_x_read, jit_x_write),
        buf(EXT_BASE, EXT_END, ext),
        cb(EXT_END, JIT_END, s, jit_x_read, jit_x_write),
    };
    const Dsp56300MemoryRegion y[] = {
        buf(0, YMEM_WORDS, ymem),
        cb(YMEM_WORDS, EXT_BASE, s, jit_y_read, jit_y_write),
        buf(EXT_BASE, EXT_END, ext),
        cb(EXT_END, JIT_END, s, jit_y_read, jit_y_write),
    };
    const Dsp56300MemoryRegion p[] = {
        buf(0, PMEM_WORDS, pmem),
        cb(PMEM_WORDS, EXT_BASE, s, jit_p_read, jit_p_write),
        buf(EXT_BASE, EXT_END, ext),
        cb(EXT_END, JIT_END, s, jit_p_read, jit_p_write),
    };
    const Dsp56300CreateInfo info = {
        .memory_map = {
            .x_regions = x, .x_count = ARRAY_SIZE(x),
            .y_regions = y, .y_count = ARRAY_SIZE(y),
            .p_regions = p, .p_count = ARRAY_SIZE(p),
        },
    };

    return (SonicDspJit *)dsp56300_create(&info);
}

void sonic_dsp_jit_destroy(SonicDspJit *j)
{
    if (j) {
        dsp56300_destroy((Dsp56300Jit *)j);
    }
}

void sonic_dsp_jit_reset(SonicDspJit *j, uint32_t boot_pc)
{
    Dsp56300State st;

    dsp56300_reset((Dsp56300Jit *)j);
    dsp56300_get_state((Dsp56300Jit *)j, &st);
    st.pc = boot_pc;
    dsp56300_set_state((Dsp56300Jit *)j, &st);
}

void sonic_dsp_jit_run(SonicDspJit *j, uint64_t cycles)
{
    dsp56300_run((Dsp56300Jit *)j, MIN(cycles, INT32_MAX));
}

void sonic_dsp_jit_irq_raise(SonicDspJit *j, uint32_t vector, unsigned ipl,
                             bool level)
{
    dsp56300_raise_interrupt((Dsp56300Jit *)j, vector, ipl, level);
}

void sonic_dsp_jit_irq_lower(SonicDspJit *j, uint32_t vector)
{
    dsp56300_lower_interrupt((Dsp56300Jit *)j, vector);
}

bool sonic_dsp_jit_irq_pending(SonicDspJit *j, uint32_t vector)
{
    return dsp56300_interrupt_pending((Dsp56300Jit *)j, vector);
}

void sonic_dsp_jit_invalidate(SonicDspJit *j)
{
    dsp56300_invalidate_cache((Dsp56300Jit *)j);
}

void sonic_dsp_jit_p_write(SonicDspJit *j, uint32_t addr, uint32_t val)
{
    dsp56300_write_memory((Dsp56300Jit *)j, DSP56300_MEM_SPACE_P, addr, val);
}
