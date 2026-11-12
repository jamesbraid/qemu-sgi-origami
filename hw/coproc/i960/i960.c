/*
 * Intel i960 Rx/Jx core instruction-set interpreter.
 *
 * Ported from MAME src/devices/cpu/i960/i960.cpp
 * license:BSD-3-Clause
 * copyright-holders:Farfetch'd, R. Belmont
 *
 * Copyright (c) Farfetch'd, R. Belmont (MAME i960 core)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Derived from MAME's i960 KB core: the structure of the register set and
 * local register cache, the interrupt-table walk, and the REG/COBR/CTRL/MEM
 * opcode decode are carried over and rewritten as C with a callback bus.
 * Most of the instruction set, the fault, system-call and boot machinery
 * are new, written from the Intel 80960Jx and 80960Rx developer's manuals
 * (order numbers 272483 and 272736).  Where MAME's KB core and those
 * manuals disagree the manuals win.  Differences found:
 *
 *  - Stack frames are 16-byte aligned (SALIGN=1), not 64 (MAME's KB).
 *  - The PFP address field is bits 31:4; MAME masks 0x3f.
 *  - The interrupt record stores the full vector number at NFP-8; MAME
 *    stores vector-8.
 *  - Interrupt entry sets the process priority, PC.s and PC.em and clears
 *    PC.te; saves IMSK in r3 and clears it per ICON.mo.  MAME has no IMSK.
 *  - IBR boot reads PRCB/IBR as the Jx/Rx manuals describe (IBR at
 *    0xfeffff30, eight-word checksum, control table load).  MAME reads
 *    words 0/4/12 of a KB-style boot structure.
 *  - synmov/synmovl/synmovq and the 0xff000010 IAC mailbox are KB/KA
 *    only; they are invalid opcodes here.  sysctl, icctl, dcctl, intctl,
 *    inten, intdis, halt, eshro, atmod, atadd, bswap, cmpob/cmpib/cmpos/
 *    cmpis and the conditional add/subtract/select group are Rx/Jx and
 *    are implemented.  The floating-point set is absent from Rx/Jx.
 *  - movl/movt/movq from a literal zero-extend the literal; MAME writes
 *    it to every register of the group.
 *  - faultno does not branch (MAME copies bno's displacement handling).
 *    Fault<cc> raises CONSTRAINT.RANGE.
 *  - addi/subi/muli/divi/shli/stib/stis raise ARITHMETIC.INTEGER_OVERFLOW
 *    unless AC.om is set, in which case AC.of is set; MAME ignores
 *    overflow.
 *  - shrdi on a negative operand rounds toward zero for shifts below 32;
 *    results of shifts by 32 or more follow the manual's table.
 *  - subc: the manual's action text sets the overflow bit from the operand
 *    sign bits, src2[31] == src1[31], which is the add rule; subtraction
 *    overflows when the signs differ.  The text looks like an erratum, and
 *    the code uses the subtraction rule (MAME does the same).
 *  - Mid-instruction unaligned accesses are split into byte accesses, and
 *    OPERATION.UNALIGNED follows the access unless masked by the PRCB
 *    fault configuration word.
 *
 * Approximations and omissions, all deliberate:
 *  - Cycle counts are the manual's per-instruction execution times with no
 *    pipeline, cache, parallel-issue or bus wait model.
 *  - The fault record is I960_FAULT_RECORD_SIZE (64) bytes below the new
 *    frame pointer.  The figures say 64 (SP = NFP + 64 for the frame); the
 *    prose in section 8.5.2 says 80.  The 64 is inferred.
 *  - NMI service blocks every other interrupt until a supervisor return
 *    from interrupt (section 11.6.2).  Example 11-3 lets a priority-31
 *    interrupt through; the prose is followed.
 *  - Faults are always precise.  PARALLEL and OVERRIDE fault tables and
 *    system-error escalation are not modelled; a fault while delivering a
 *    fault stops the core (system error).  AC.nif has no effect.
 *  - Trace faults are raised right after the traced instruction.  The
 *    four-instruction delay on PC.te/TC updates, trace-fault-pending
 *    resumption and hardware breakpoints are not modelled.
 *  - The interrupt vector cache (ICON.vce) is not modelled; vectors are
 *    always read from the interrupt table, except the NMI vector which is
 *    cached in on-chip RAM word 0 as the manual describes.
 *  - Instruction and data caches are not modelled.
 *  - Timers, bus control (PMCON/BCON), breakpoints and every other MMR are
 *    the device's: the core forwards them as word accesses.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/coproc/i960/i960.h"

/* Instruction cycle approximations, Jx manual table B-2..B-4 */
#define CYC_BRANCH_TAKEN 1
#define CYC_CALL         9
#define CYC_RET          7
#define CYC_CALLS        30
#define CYC_DIV          40
#define CYC_MUL          4
#define CYC_CTL          10
#define CYC_FLUSHREG     15
#define CYC_SPIN_LIMIT   64

enum {
    RT_LOCAL = 0,
    RT_FAULT = 1,
    RT_SUPER = 2,
    RT_INTERRUPT = 7,
};

static G_NORETURN void raise_fault(I960State *s, uint32_t type,
                                   uint32_t subtype, bool rip_next)
{
    s->flt.type = type;
    s->flt.subtype = subtype;
    s->flt.rip_next = rip_next;
    s->flt.has_data = false;
    longjmp(s->jb, 1);
}

static G_NORETURN void raise_fault_data(I960State *s, uint32_t type,
                                        uint32_t subtype, bool rip_next,
                                        uint32_t data)
{
    s->flt.type = type;
    s->flt.subtype = subtype;
    s->flt.rip_next = rip_next;
    s->flt.has_data = true;
    s->flt.data = data;
    longjmp(s->jb, 1);
}

static G_NORETURN void op_fault(I960State *s, uint32_t subtype)
{
    if (subtype == I960_FS_OP_INVALID_OPCODE) {
        qemu_log_mask(LOG_GUEST_ERROR, "i960: invalid opcode at 0x%08x\n",
                      s->cur_ip);
    }
    raise_fault(s, I960_FT_OPERATION, subtype, false);
}

static inline unsigned msb32(uint32_t x)
{
    return 31 - clz32(x);
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

static uint32_t level_pins(I960State *s);

/* Dedicated-mode pins that post into IPND, by ICON.im. */
static uint32_t dedicated_pins(I960State *s)
{
    switch (s->icon & I960_ICON_IM_MASK) {
    case 0:
        return 0xff;
    case 2:
        return 0xe0;
    default:
        return 0;
    }
}

static uint32_t mmr_read(I960State *s, uint32_t addr)
{
    switch (addr) {
    case I960_MMR_IPND:
        /* a level-detected pin reads as pending while it is asserted */
        return s->ipnd | (level_pins(s) & dedicated_pins(s));
    case I960_MMR_BCON:
        return s->bcon;
    case I960_MMR_IMSK:
        return s->imsk;
    case I960_MMR_ICON:
        return s->icon;
    case I960_MMR_IMAP0:
    case I960_MMR_IMAP1:
    case I960_MMR_IMAP2:
        return s->imap[(addr - I960_MMR_IMAP0) / 4];
    case I960_MMR_PRCB:
        return s->prcb;
    case I960_MMR_ISP:
        return s->isp;
    case I960_MMR_SSP:
        return s->ssp;
    case I960_MMR_DEVICEID:
        return s->device_id;
    default:
        return s->ops->read(s->opaque, addr, 4);
    }
}

static void mmr_write(I960State *s, uint32_t addr, uint32_t val)
{
    if (addr >= I960_MMR_SUPERVISOR && !(s->pc & I960_PC_EM)) {
        raise_fault(s, I960_FT_TYPE, I960_FS_TYPE_MISMATCH, false);
    }
    switch (addr) {
    case I960_MMR_BCON:
        s->bcon = val;
        s->ops->write(s->opaque, addr, val, 4);
        break;
    case I960_MMR_IPND:
        s->ipnd = val & (0xffu | I960_IPND_TINT0 | I960_IPND_TINT1);
        break;
    case I960_MMR_IMSK:
        s->imsk = val & (0xffu | I960_IPND_TINT0 | I960_IPND_TINT1);
        break;
    case I960_MMR_ICON:
        s->icon = val;
        break;
    case I960_MMR_IMAP0:
    case I960_MMR_IMAP1:
    case I960_MMR_IMAP2:
        s->imap[(addr - I960_MMR_IMAP0) / 4] = val;
        break;
    case I960_MMR_PRCB:
    case I960_MMR_DEVICEID:
        break;
    case I960_MMR_ISP:
        s->isp = val;
        break;
    case I960_MMR_SSP:
        s->ssp = val;
        break;
    default:
        s->ops->write(s->opaque, addr, val, 4);
        break;
    }
}

static uint32_t mem_read(I960State *s, uint32_t addr, unsigned size)
{
    uint32_t v;

    if (addr >= I960_MMR_BASE) {
        if (size != 4 || (addr & 3)) {
            op_fault(s, I960_FS_OP_UNIMPLEMENTED);
        }
        v = mmr_read(s, addr);
    } else if (size > 1 && (addr & (size - 1))) {
        /* unaligned: perform as bytes, report after the instruction */
        s->unaligned = true;
        s->unaligned_ea = addr;
        v = 0;
        for (unsigned i = 0; i < size; i++) {
            v |= mem_read(s, addr + i, 1) << (8 * i);
        }
        return v;
    } else if (addr < I960_DATA_RAM_SIZE) {
        v = ldn_le_p(&s->data_ram[addr], size);
    } else {
        v = s->ops->read(s->opaque, addr, size);
    }
    return v;
}

/* On-chip data RAM write protection, Jx manual sections 4.1 and 8.10.8. */
static void data_ram_check(I960State *s, uint32_t addr, unsigned size)
{
    bool super = s->pc & I960_PC_EM;
    bool low = addr < I960_RAM_PROT_SIZE;
    bool high = addr + size > I960_RAM_PROT_SIZE;

    if ((low && (!super || (s->bcon & I960_BCON_SIRP))) ||
        (high && !super && (s->bcon & I960_BCON_IRP))) {
        raise_fault(s, I960_FT_TYPE, I960_FS_TYPE_MISMATCH, false);
    }
}

static void mem_write(I960State *s, uint32_t addr, uint32_t val, unsigned size)
{
    s->stored = true;
    if (addr < I960_DATA_RAM_SIZE) {
        data_ram_check(s, addr, size);
    }
    if (addr >= I960_MMR_BASE) {
        if (size != 4 || (addr & 3)) {
            op_fault(s, I960_FS_OP_UNIMPLEMENTED);
        }
        mmr_write(s, addr, val);
    } else if (size > 1 && (addr & (size - 1))) {
        s->unaligned = true;
        s->unaligned_ea = addr;
        for (unsigned i = 0; i < size; i++) {
            mem_write(s, addr + i, val >> (8 * i), 1);
        }
    } else if (addr < I960_DATA_RAM_SIZE) {
        stn_le_p(&s->data_ram[addr], size, val);
    } else {
        s->ops->write(s->opaque, addr, val, size);
    }
}

/* Multiword (ldl/ldt/ldq, stl/stt/stq): natural boundary is 8 or 16 bytes. */
static void mem_read_group(I960State *s, uint32_t addr, uint32_t *w,
                           unsigned nwords)
{
    unsigned natural = nwords == 2 ? 8 : 16;

    if (addr + nwords * 4 - 1 >= I960_MMR_BASE && addr < I960_MMR_BASE) {
        op_fault(s, I960_FS_OP_UNIMPLEMENTED);
    }
    if (addr >= I960_MMR_BASE) {
        op_fault(s, I960_FS_OP_UNIMPLEMENTED);
    }
    if (addr & (natural - 1)) {
        s->unaligned = true;
        s->unaligned_ea = addr;
    }
    if ((addr & 3) == 0 && addr >= I960_DATA_RAM_SIZE &&
        addr + nwords * 4 > addr && s->ops->read_burst) {
        s->ops->read_burst(s->opaque, addr, w, nwords);
        return;
    }
    for (unsigned i = 0; i < nwords; i++) {
        w[i] = mem_read(s, addr + 4 * i, 4);
    }
}

static void mem_write_group(I960State *s, uint32_t addr, const uint32_t *w,
                            unsigned nwords)
{
    unsigned natural = nwords == 2 ? 8 : 16;

    if (addr >= I960_MMR_BASE ||
        (addr + nwords * 4 - 1 >= I960_MMR_BASE && addr < I960_MMR_BASE)) {
        op_fault(s, I960_FS_OP_UNIMPLEMENTED);
    }
    if (addr & (natural - 1)) {
        s->unaligned = true;
        s->unaligned_ea = addr;
    }
    if ((addr & 3) == 0 && addr >= I960_DATA_RAM_SIZE && s->ops->write_burst) {
        s->stored = true;
        s->ops->write_burst(s->opaque, addr, w, nwords);
        return;
    }
    for (unsigned i = 0; i < nwords; i++) {
        mem_write(s, addr + 4 * i, w[i], 4);
    }
}

static uint32_t fetch32(I960State *s, uint32_t addr)
{
    if (addr < I960_DATA_RAM_SIZE || addr >= I960_MMR_BASE) {
        op_fault(s, I960_FS_OP_UNIMPLEMENTED);
    }
    if (s->ops->fetch) {
        return s->ops->fetch(s->opaque, addr);
    }
    return s->ops->read(s->opaque, addr, 4);
}

/*
 * Reads and writes done by the core's own machinery (frames, tables).
 * These never fault: the architecture keeps them out of the on-chip data
 * RAM tail and the MMR space, and a misplaced table reads as whatever the
 * bus returns.
 */
static uint32_t raw_read(I960State *s, uint32_t addr, unsigned size)
{
    if (size > 1 && (addr & (size - 1))) {
        uint32_t v = 0;

        for (unsigned i = 0; i < size; i++) {
            v |= raw_read(s, addr + i, 1) << (8 * i);
        }
        return v;
    }
    if (addr < I960_DATA_RAM_SIZE) {
        return ldn_le_p(&s->data_ram[addr], size);
    }
    return s->ops->read(s->opaque, addr, size);
}

static void raw_write(I960State *s, uint32_t addr, uint32_t val, unsigned size)
{
    if (size > 1 && (addr & (size - 1))) {
        for (unsigned i = 0; i < size; i++) {
            raw_write(s, addr + i, val >> (8 * i), 1);
        }
        return;
    }
    if (addr < I960_DATA_RAM_SIZE) {
        stn_le_p(&s->data_ram[addr], size, val);
    } else {
        s->ops->write(s->opaque, addr, val, size);
    }
}

static uint32_t sys_read32(I960State *s, uint32_t addr)
{
    return raw_read(s, addr, 4);
}

static void sys_write32(I960State *s, uint32_t addr, uint32_t val)
{
    raw_write(s, addr, val, 4);
}

/* ------------------------------------------------------------------ */
/* Local register cache and call machinery                            */
/* ------------------------------------------------------------------ */

static unsigned rcache_limit(I960State *s)
{
    bool critical = (s->pc & I960_PC_S) && i960_priority(s) >= 28;

    return critical ? I960_RCACHE_FRAMES :
                      I960_RCACHE_FRAMES - s->rcache_reserved;
}

static void spill_oldest(I960State *s)
{
    for (unsigned i = 0; i < 16; i++) {
        sys_write32(s, s->rcache[0].fp + 4 * i, s->rcache[0].r[i]);
    }
    memmove(&s->rcache[0], &s->rcache[1],
            (s->rcache_n - 1) * sizeof(s->rcache[0]));
    s->rcache_n--;
}

/* Save the current local set, spilling to memory when the cache is full. */
static void push_frame(I960State *s)
{
    unsigned limit = rcache_limit(s);
    uint32_t fp = s->r[I960_FP] & ~0xfu;

    while (s->rcache_n >= limit && s->rcache_n > 0) {
        spill_oldest(s);
    }
    if (limit == 0) {
        for (unsigned i = 0; i < 16; i++) {
            sys_write32(s, fp + 4 * i, s->r[i]);
        }
        return;
    }
    memcpy(s->rcache[s->rcache_n].r, s->r, 16 * sizeof(uint32_t));
    s->rcache[s->rcache_n].fp = fp;
    s->rcache_n++;
}

static void pop_frame(I960State *s, uint32_t fp)
{
    fp &= ~0xfu;
    if (s->rcache_n > 0 && s->rcache[s->rcache_n - 1].fp == fp) {
        s->rcache_n--;
        memcpy(s->r, s->rcache[s->rcache_n].r, 16 * sizeof(uint32_t));
    } else {
        for (unsigned i = 0; i < 16; i++) {
            s->r[i] = sys_read32(s, fp + 4 * i);
        }
    }
}

void i960_flush_regs(I960State *s)
{
    for (unsigned n = 0; n < s->rcache_n; n++) {
        for (unsigned i = 0; i < 16; i++) {
            sys_write32(s, s->rcache[n].fp + 4 * i, s->rcache[n].r[i]);
        }
    }
    s->rcache_n = 0;
}

/*
 * Common call mechanics.  `base_sp` is the stack pointer the new frame is
 * allocated from (the current SP, the interrupt stack or the supervisor
 * stack), `record` the number of bytes of interrupt or fault record below
 * the new frame pointer.
 */
static void enter_frame(I960State *s, uint32_t target, uint32_t rip,
                        uint32_t base_sp, uint32_t rt, uint32_t record)
{
    uint32_t old_fp = s->r[I960_FP];
    uint32_t nfp = ((base_sp + 15) & ~15u) + record;

    s->r[I960_RIP] = rip;
    push_frame(s);
    s->r[I960_PFP] = (old_fp & ~0xfu) | rt;
    s->r[I960_FP] = nfp;
    s->r[I960_SP] = nfp + 64;
    s->ip = target & ~3u;
    s->insn_cycles = CYC_CALL;
}

static void do_local_call(I960State *s, uint32_t target)
{
    enter_frame(s, target, s->next_ip, s->r[I960_SP], RT_LOCAL, 0);
    if ((s->pc & I960_PC_TE) && (s->tc & I960_TC_CALL)) {
        s->r[I960_PFP] |= 8;
        s->trace_events |= I960_TC_CALL;
    }
}

static G_NORETURN void bad_sysproc(I960State *s)
{
    raise_fault(s, I960_FT_PROTECTION, I960_FS_PROTECTION_LENGTH, false);
}

static uint32_t sysproc_entry(I960State *s, uint32_t n)
{
    if (n > 259) {
        bad_sysproc(s);
    }
    return sys_read32(s, s->sys_proc_table + 48 + 4 * n);
}

/*
 * calls, and the system-call half of a fault call.  `fault` selects the
 * fault return type; `trace_fault` clears PC.te in the handler instead of
 * taking the system procedure table's trace control bit.
 */
static void do_system_call(I960State *s, uint32_t entry, uint32_t rip,
                           bool fault, uint32_t record, bool trace_fault)
{
    uint32_t target = entry & ~3u;
    uint32_t rt_local = fault ? RT_FAULT : RT_LOCAL;

    if (entry & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "i960: reserved system procedure entry type, 0x%08x\n",
                      entry);
    }

    if ((entry & 3) == 0) {
        enter_frame(s, target, rip, s->r[I960_SP], rt_local, record);
    } else if (s->pc & I960_PC_EM) {
        enter_frame(s, target, rip, s->r[I960_SP], rt_local, record);
        if (fault) {
            s->pc = (s->pc & ~I960_PC_TE) |
                    (!trace_fault && s->ssp_te ? I960_PC_TE : 0);
        }
    } else {
        uint32_t rt = fault ? RT_FAULT : (RT_SUPER | (s->pc & I960_PC_TE));

        enter_frame(s, target, rip, s->ssp, rt, record);
        s->pc |= I960_PC_EM;
        s->pc = (s->pc & ~I960_PC_TE) |
                (!trace_fault && s->ssp_te ? I960_PC_TE : 0);
        if (!fault) {
            s->trace_events |= I960_TC_SUPER;
        }
    }
}

static void do_calls(I960State *s, uint32_t n)
{
    uint32_t entry = sysproc_entry(s, n);

    do_system_call(s, entry, s->next_ip, false, 0, false);
    s->insn_cycles = CYC_CALLS;
    if ((s->pc & I960_PC_TE) && (s->tc & I960_TC_CALL)) {
        s->r[I960_PFP] |= 8;
        s->trace_events |= I960_TC_CALL;
    }
}

static unsigned refresh_sw_prio(I960State *s)
{
    uint32_t x = sys_read32(s, s->int_table);

    s->sw_prio = x ? msb32(x) : 0;
    return s->sw_prio;
}

static void get_fp_and_ip(I960State *s)
{
    uint32_t pfp = s->r[I960_PFP];

    pop_frame(s, pfp & ~0xfu);
    s->ip = s->r[I960_RIP];
    s->r[I960_FP] = pfp & ~0xfu;
}

static void do_ret(I960State *s)
{
    uint32_t pfp = s->r[I960_PFP];
    uint32_t tempa, tempb;

    s->insn_cycles = CYC_RET;
    if ((pfp & 8) && (s->pc & I960_PC_TE) && (s->tc & I960_TC_PRERET)) {
        s->r[I960_PFP] &= ~8u;
        raise_fault(s, I960_FT_TRACE, I960_FS_TRACE_PRERET, false);
    }
    switch (pfp & 7) {
    case 1:
        tempa = sys_read32(s, s->r[I960_FP] - 16);
        tempb = sys_read32(s, s->r[I960_FP] - 12);
        get_fp_and_ip(s);
        s->ac = tempb;
        if (s->pc & I960_PC_EM) {
            s->pc = tempa;
        }
        s->trace_events |= I960_TC_RET;
        break;
    case 2:
    case 3:
        if (s->pc & I960_PC_EM) {
            s->pc = (s->pc & ~(I960_PC_TE | I960_PC_EM)) | (pfp & 1);
            s->trace_events |= I960_TC_SUPER;
        }
        get_fp_and_ip(s);
        s->trace_events |= I960_TC_RET;
        break;
    case 7:
        tempa = sys_read32(s, s->r[I960_FP] - 16);
        tempb = sys_read32(s, s->r[I960_FP] - 12);
        get_fp_and_ip(s);
        s->ac = tempb;
        if (s->pc & I960_PC_EM) {
            s->pc = tempa;
            s->block_nmi = false;
        }
        refresh_sw_prio(s);
        break;
    default:
        /* reserved return types: unpredictable; treat as local */
        get_fp_and_ip(s);
        s->trace_events |= I960_TC_RET;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Faults and interrupts                                              */
/* ------------------------------------------------------------------ */

static void deliver_fault(I960State *s)
{
    uint32_t type = s->flt.type;
    uint32_t entry = sys_read32(s, s->fault_table + type * 8);
    uint32_t rip = s->flt.rip_next ? s->next_ip : s->cur_ip;
    uint32_t old_pc = s->pc;
    uint32_t old_ac = s->ac;
    bool trace_like = type == I960_FT_TRACE;
    uint32_t nfp;

    if (entry & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "i960: reserved fault table entry type, 0x%08x\n",
                      entry);
    }
    if ((entry & 3) == 0) {
        enter_frame(s, entry, rip, s->r[I960_SP], RT_FAULT,
                    I960_FAULT_RECORD_SIZE);
    } else {
        uint32_t proc = sysproc_entry(s, entry >> 2);

        do_system_call(s, proc, rip, true, I960_FAULT_RECORD_SIZE,
                       trace_like);
    }
    nfp = s->r[I960_FP];
    if (s->flt.has_data) {
        sys_write32(s, nfp - 24, s->flt.data);
    }
    sys_write32(s, nfp - 16, old_pc);
    sys_write32(s, nfp - 12, old_ac);
    sys_write32(s, nfp - 8, (type << 16) | s->flt.subtype);
    sys_write32(s, nfp - 4, s->cur_ip);
    s->insn_cycles = 40;
}

static unsigned xint_vector(I960State *s, unsigned n)
{
    uint32_t field = n < 4 ? s->imap[0] >> (4 * n)
                           : s->imap[1] >> (4 * (n - 4));

    return ((field & 0xf) << 4) | 0x2;
}

static unsigned tint_vector(I960State *s, unsigned t)
{
    return (((s->imap[2] >> (16 + 4 * t)) & 0xf) << 4) | 0x2;
}

static uint32_t level_pins(I960State *s)
{
    /* level-low pins (ICON.sdm bit clear) request while asserted */
    uint32_t edge_mode = (s->icon >> 2) & 0xff;

    return s->xint_pins & ~edge_mode;
}

typedef struct HwIrq {
    unsigned vector;
    unsigned prio;
    bool expanded;
    int ipnd_bit;
} HwIrq;

static bool hw_irq_pending(I960State *s, HwIrq *h)
{
    unsigned mode = s->icon & I960_ICON_IM_MASK;
    uint32_t eff = (s->ipnd | level_pins(s)) & s->imsk;
    bool found = false;

    h->prio = 0;
    if (s->icon & I960_ICON_GIE) {
        return false;
    }
    if (mode == 1 || mode == 2) {
        unsigned v = mode == 1 ? s->xint_pins :
                                 ((s->xint_pins & 0x1f) << 3) | 2;

        if ((s->imsk & 1) && v >= 8) {
            h->vector = v;
            h->prio = v >> 3;
            h->expanded = true;
            h->ipnd_bit = -1;
            found = true;
        }
    }
    for (int bit = 0; bit < 10; bit++) {
        unsigned v, p, b = bit < 8 ? bit : bit + 4;

        if (!(eff & (1u << b))) {
            continue;
        }
        if (bit < 8 && (mode == 1 || (mode == 2 && bit < 5))) {
            continue;
        }
        v = bit < 8 ? xint_vector(s, bit) : tint_vector(s, bit - 8);
        p = v >> 3;
        if (p == 0) {
            continue;
        }
        if (!found || p > h->prio || (p == h->prio && v > h->vector)) {
            h->vector = v;
            h->prio = p;
            h->expanded = false;
            h->ipnd_bit = b;
            found = true;
        }
    }
    return found;
}

static void take_interrupt(I960State *s, unsigned vector, unsigned prio,
                           bool nmi)
{
    uint32_t old_pc = s->pc;
    uint32_t old_ac = s->ac;
    uint32_t handler;
    uint32_t base;

    if (nmi && vector == 248) {
        handler = (uint32_t)ldl_le_p(&s->data_ram[0]);
    } else {
        handler = sys_read32(s, s->int_table + 4 * vector + 4);
    }
    base = (old_pc & I960_PC_S) ? s->r[I960_SP] : s->isp;
    enter_frame(s, handler, s->ip, base, RT_INTERRUPT, 16);
    sys_write32(s, s->r[I960_FP] - 16, old_pc);
    sys_write32(s, s->r[I960_FP] - 12, old_ac);
    sys_write32(s, s->r[I960_FP] - 8, vector);
    s->pc = (old_pc & ~(I960_PC_TE | I960_PC_TFP | I960_PC_PRIO_MASK)) |
            I960_PC_S | I960_PC_EM | (prio << I960_PC_PRIO_SHIFT);
    s->halted = false;
    s->idle_hint = false;
    s->spin_count = 0;
    s->insn_cycles = 16;
}

static bool take_sw_from_table(I960State *s)
{
    uint32_t x = sys_read32(s, s->int_table);
    unsigned y, w;
    uint8_t z;

    if (x == 0) {
        s->sw_prio = 0;
        return false;
    }
    y = msb32(x);
    if (y != 31 && y <= i960_priority(s)) {
        s->sw_prio = y;
        return false;
    }
    z = raw_read(s, s->int_table + 4 + y, 1);
    if (z == 0) {
        x &= ~(1u << y);
        sys_write32(s, s->int_table, x);
        s->sw_prio = x ? msb32(x) : 0;
        return false;
    }
    w = msb32(z);
    z &= ~(1u << w);
    raw_write(s, s->int_table + 4 + y, z, 1);
    if (z == 0) {
        x &= ~(1u << y);
    }
    sys_write32(s, s->int_table, x);
    s->sw_prio = x ? msb32(x) : 0;
    take_interrupt(s, y * 8 + w, y, false);
    return true;
}

/* Instruction-boundary interrupt resolution, Jx manual example 11-3. */
static bool service_interrupts(I960State *s)
{
    HwIrq h = { 0 };
    bool hw;
    unsigned prio = i960_priority(s);
    unsigned best;

    if (s->block_nmi) {
        return false;       /* section 11.6.2: nothing interrupts an NMI */
    }
    if (!s->nmi_pending && !s->sw_prio && !s->xint_pins && !s->ipnd) {
        return false;
    }
    if (s->nmi_pending && !s->block_nmi) {
        s->nmi_pending = false;
        s->block_nmi = true;
        take_interrupt(s, 248, 31, true);
        return true;
    }
    hw = hw_irq_pending(s, &h);
    best = hw ? h.prio : 0;
    if (s->sw_prio > best) {
        best = s->sw_prio;
    }
    if (best == 0 || !(best == 31 || best > prio)) {
        return false;
    }
    if (hw && h.prio >= s->sw_prio) {
        uint32_t old_imsk = s->imsk;
        unsigned mo = (s->icon & I960_ICON_MO_MASK) >> I960_ICON_MO_SHIFT;

        if (h.ipnd_bit >= 0) {
            s->ipnd &= ~(1u << h.ipnd_bit);
        }
        take_interrupt(s, h.vector, h.prio, false);
        s->r[3] = old_imsk;
        if ((h.expanded && (mo & 2)) || (!h.expanded && (mo & 1))) {
            s->imsk = 0;
        }
        return true;
    }
    return take_sw_from_table(s);
}

static void post_sw(I960State *s, unsigned vector)
{
    unsigned prio = vector >> 3;
    uint32_t pend = sys_read32(s, s->int_table);

    if (prio != 0) {
        uint8_t z = raw_read(s, s->int_table + 4 + prio, 1);

        z |= 1u << (vector & 7);
        raw_write(s, s->int_table + 4 + prio, z, 1);
        pend |= 1u << prio;
        sys_write32(s, s->int_table, pend);
    }
    s->sw_prio = pend ? msb32(pend) : 0;
}

static void wake(I960State *s)
{
    s->idle_hint = false;
    s->spin_count = 0;
}

void i960_set_xint(I960State *s, uint8_t asserted)
{
    uint8_t rising = asserted & ~s->xint_pins;
    uint8_t edge_mode = (s->icon >> 2) & 0xff;

    s->xint_pins = asserted;
    s->ipnd |= rising & edge_mode;
    if (asserted) {
        wake(s);
    }
}

void i960_set_tint(I960State *s, unsigned timer)
{
    s->ipnd |= timer ? I960_IPND_TINT1 : I960_IPND_TINT0;
    wake(s);
}

void i960_nmi(I960State *s)
{
    s->nmi_pending = true;
    wake(s);
}

void i960_post_interrupt(I960State *s, uint8_t vector)
{
    post_sw(s, vector);
    wake(s);
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                     */
/* ------------------------------------------------------------------ */

static void stop_with_error(I960State *s, uint32_t code)
{
    qemu_log_mask(LOG_GUEST_ERROR, "i960: system error, fail code 0x%08x\n",
                  code);
    s->failed = true;
    s->halted = true;
    if (s->ops->system_error) {
        s->ops->system_error(s->opaque, code);
    }
}

/* Control table offset -> control register, Jx manual figure 12-7. */
static bool ctrl_off_to_mmr(uint32_t off, uint32_t *mmr)
{
    switch (off) {
    case 0x10:
        *mmr = I960_MMR_IMAP0;
        return true;
    case 0x14:
        *mmr = I960_MMR_IMAP1;
        return true;
    case 0x18:
        *mmr = I960_MMR_IMAP2;
        return true;
    case 0x1c:
        *mmr = I960_MMR_ICON;
        return true;
    case 0x6c:
        *mmr = 0xff0086fc;                          /* BCON */
        return true;
    default:
        if (off >= 0x20 && off <= 0x58 && !(off & 4)) {
            *mmr = 0xff008600 + (off - 0x20);       /* PMCONn_m */
            return true;
        }
        return false;
    }
}

static void load_ctrl_word(I960State *s, uint32_t off)
{
    uint32_t mmr, val = sys_read32(s, s->ctrl_table + off);

    if (off == 0x68) {
        s->tc = val;
    } else if (ctrl_off_to_mmr(off, &mmr)) {
        uint32_t saved_pc = s->pc;

        s->pc |= I960_PC_EM;
        mmr_write(s, mmr, val);
        s->pc = saved_pc;
    }
}

/* Process_PRCB, Jx manual example 12-2.  Returns false on a bad alignment. */
static bool process_prcb(I960State *s, uint32_t prcb)
{
    uint32_t temp;

    s->prcb = prcb;
    /*
     * On-chip data RAM is deliberately not cleared here.  Example 12-2
     * (Jx; example 11-2, p. 11-18, in the Rx manual) says it is
     * unpredictable whether the RAM keeps its contents.  Observed on a
     * shipped i960 RD boot ROM: state left at data RAM 0x40 is read after
     * its sysctl reinitialise, so real silicon keeps it.  Only i960_reset()
     * clears it.  Word 0 (the cached NMI vector) is rewritten below.
     *
     * Table 12-2 also says IPND and IMSK keep their values over a
     * re-init, the timers and the LMAR registers keep theirs (the device's
     * state, untouched here), and AC, TC, ICON, IMAP, BCON and the PMCON
     * registers come from the PRCB and control table.  The table gives
     * re-init values only for FP, PFP and SP (RIP undefined).
     *
     * Only FP, SP and PFP are set here (from the interrupt stack base);
     * g0 = DEVICE_ID is in the cold-reset flow only (example 12-1), so
     * i960_reset() sets it and zeroes the registers first.  A sysctl
     * re-init keeps g0-g14: observed on a shipped i960 RD boot ROM, which
     * passes a value to the next image in g0 across its re-init.
     *
     * Model choices, the manual being silent: r0 (PFP) and r1 (SP) are the
     * new frame's, r2 (RIP, undefined) and r3-r15 are left as they were,
     * and the cached local register sets are dropped, since FP now names a
     * frame on the new interrupt stack.
     */
    s->fault_table = sys_read32(s, prcb);
    s->ctrl_table = sys_read32(s, prcb + 0x4);
    s->ac = sys_read32(s, prcb + 0x8);
    s->fault_config = sys_read32(s, prcb + 0xc);
    s->int_table = sys_read32(s, prcb + 0x10);
    s->sys_proc_table = sys_read32(s, prcb + 0x14);
    s->isp = sys_read32(s, prcb + 0x1c);

    if ((prcb | s->fault_table | s->ctrl_table | s->int_table |
         s->sys_proc_table) & 3) {
        return false;
    }
    s->block_nmi = false;
    stl_le_p(&s->data_ram[0], sys_read32(s, s->int_table + 248 * 4 + 4));
    temp = sys_read32(s, s->sys_proc_table + 0xc);
    s->ssp = temp & ~3u;
    s->ssp_te = temp & 1;
    s->r[I960_FP] = s->isp & ~0xfu;
    s->r[I960_SP] = s->r[I960_FP] + 64;
    s->r[I960_PFP] = s->r[I960_FP];
    s->rcache_n = 0;
    s->rcache_reserved = (sys_read32(s, prcb + 0x24) >> 8) & 7;
    s->icache_on = !((sys_read32(s, prcb + 0x20) >> 16) & 1);
    s->dcache_on = false;
    s->tc = 0;
    s->sw_prio = 0;
    s->nmi_pending = false;
    for (uint32_t off = 0x10; off <= 0x58; off += 4) {
        load_ctrl_word(s, off);
    }
    load_ctrl_word(s, 0x68);
    load_ctrl_word(s, 0x6c);
    return true;
}

void i960_init(I960State *s, I960Variant variant, const I960Ops *ops,
               void *opaque)
{
    memset(s, 0, sizeof(*s));
    s->variant = variant;
    s->ops = ops;
    s->opaque = opaque;
}

void i960_reset(I960State *s)
{
    uint32_t acc = 0xffffffff;
    uint32_t carry = 0;
    uint32_t pmcon = 0;

    s->failed = false;
    s->halted = false;
    s->idle_hint = false;
    s->spin_count = 0;
    s->unaligned = false;
    s->in_fault = false;
    s->xint_pins = 0;
    s->imsk = 0;
    s->icon = 0;
    memset(s->imap, 0, sizeof(s->imap));
    s->pc = 0x001f2002;
    memset(s->r, 0, sizeof(s->r));
    s->ipnd = 0;                /* undefined after a cold reset */
    memset(s->data_ram, 0, sizeof(s->data_ram));   /* likewise */

    for (int i = 0; i < 4; i++) {
        pmcon |= (s->ops->read(s->opaque, I960_IBR_ADDR + 4 * i, 4) & 0xff)
                 << (8 * i);
    }
    s->boot_pmcon14_15 = pmcon;
    s->boot_big_endian = (pmcon >> 31) & 1;
    if (s->boot_big_endian) {
        qemu_log_mask(LOG_UNIMP, "i960: big-endian boot record is not "
                      "supported\n");
    }
    s->bcon = 0;

    /* eight words from the first-instruction pointer on, with carry */
    for (int i = 0; i < 8; i++) {
        uint64_t t = (uint64_t)s->ops->read(s->opaque,
                                            I960_IBR_ADDR + 0x10 + 4 * i, 4) +
                     acc + carry;

        acc = (uint32_t)t;
        carry = t >> 32;
    }
    if (acc != 0) {
        (void)s->ops->read(s->opaque, I960_FAIL_BUS_TEST, 4);
        stop_with_error(s, I960_FAIL_BUS_TEST);
        return;
    }
    if (!process_prcb(s, s->ops->read(s->opaque, I960_IBR_ADDR + 0x14, 4))) {
        stop_with_error(s, I960_FAIL_ALIGN);
        return;
    }
    s->pc = 0x001f2002;
    s->ip = s->ops->read(s->opaque, I960_IBR_ADDR + 0x10, 4) & ~3u;
    s->r[I960_G0] = s->device_id;
    s->cycles = 0;
}

/* ------------------------------------------------------------------ */
/* Operand decode                                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t reg_src1(I960State *s, uint32_t insn)
{
    if (insn & (1u << 5)) {
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    return (insn & (1u << 11)) ? (insn & 0x1f) : s->r[insn & 0x1f];
}

static inline uint32_t reg_src2(I960State *s, uint32_t insn)
{
    if (insn & (1u << 6)) {
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    return (insn & (1u << 12)) ? ((insn >> 14) & 0x1f)
                               : s->r[(insn >> 14) & 0x1f];
}

static inline unsigned reg_dst(I960State *s, uint32_t insn)
{
    if (insn & (1u << 13)) {
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    return (insn >> 19) & 0x1f;
}

static inline void set_reg(I960State *s, unsigned idx, uint32_t v)
{
    if (idx == I960_RIP) {
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    }
    s->r[idx] = v;
}

static inline void chk_group(I960State *s, unsigned idx, unsigned align)
{
    if (idx & (align - 1)) {
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    }
}

/* A register pair operand in the src2 slot: literal zero-extends. */
static uint64_t reg_src2_pair(I960State *s, uint32_t insn)
{
    unsigned idx = (insn >> 14) & 0x1f;

    if (insn & (1u << 6)) {
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    if (insn & (1u << 12)) {
        return idx;
    }
    chk_group(s, idx, 2);
    return s->r[idx] | ((uint64_t)s->r[idx + 1] << 32);
}

static inline void set_cc(I960State *s, unsigned cc)
{
    s->ac = (s->ac & ~I960_AC_CC_MASK) | cc;
}

static void cmp_cc(I960State *s, bool lt, bool eq)
{
    set_cc(s, lt ? 4 : eq ? 2 : 1);
}

static inline bool cond_ok(I960State *s, unsigned mask)
{
    unsigned cc = s->ac & I960_AC_CC_MASK;

    return (mask & cc) || mask == cc;
}

static void int_overflow(I960State *s)
{
    if (s->ac & I960_AC_OM) {
        s->ac |= I960_AC_OF;
    } else {
        raise_fault(s, I960_FT_ARITHMETIC, I960_FS_ARITH_OVERFLOW, true);
    }
}

static uint32_t arch_hash(I960State *s)
{
    uint32_t h = s->ac * 2654435761u + s->cur_ip;

    for (unsigned i = 0; i < 32; i++) {
        h = (h ^ s->r[i]) * 16777619u;
    }
    return h;
}

static void branch_to(I960State *s, uint32_t target)
{
    s->ip = target & ~3u;
    s->trace_events |= I960_TC_BRANCH;
    s->insn_cycles += CYC_BRANCH_TAKEN;
    if (target <= s->cur_ip && s->cur_ip - target <= 32) {
        /*
         * A short backward branch is a spin when a trip through it stored
         * nothing and left every register, AC and the branch address as
         * the previous trip did.  A counting loop changes a register each
         * trip, so it never qualifies; `b .` and a poll that rereads the
         * same status do.
         */
        uint32_t h = arch_hash(s);

        if (!s->stored && s->spin_ip == s->cur_ip && s->spin_hash == h) {
            s->spin_count++;
        } else {
            s->spin_count = 0;
        }
        s->spin_ip = s->cur_ip;
        s->spin_hash = h;
        s->stored = false;
    }
}

/* Effective address of a MEM-format instruction. */
static uint32_t mem_ea(I960State *s, uint32_t insn)
{
    unsigned abase = (insn >> 14) & 0x1f;

    if (!(insn & 0x1000)) {
        uint32_t off = insn & 0xfff;

        return (insn & 0x2000) ? s->r[abase] + off : off;
    } else {
        unsigned mode = (insn >> 10) & 0xf;
        unsigned scale = (insn >> 7) & 7;
        unsigned index = insn & 0x1f;
        uint32_t disp;

        if (insn & 0x60) {
            op_fault(s, I960_FS_OP_INVALID_OPCODE);
        }
        if (scale > 4 && (mode == 7 || mode == 14 || mode == 15)) {
            op_fault(s, I960_FS_OP_INVALID_OPCODE);
        }
        switch (mode) {
        case 0x4:
            return s->r[abase];
        case 0x5:
            disp = fetch32(s, s->cur_ip + 4);
            s->next_ip = s->ip = s->cur_ip + 8;
            return s->cur_ip + 8 + disp;
        case 0x7:
            return s->r[abase] + (s->r[index] << scale);
        case 0xc:
            disp = fetch32(s, s->cur_ip + 4);
            s->next_ip = s->ip = s->cur_ip + 8;
            return disp;
        case 0xd:
            disp = fetch32(s, s->cur_ip + 4);
            s->next_ip = s->ip = s->cur_ip + 8;
            return disp + s->r[abase];
        case 0xe:
            disp = fetch32(s, s->cur_ip + 4);
            s->next_ip = s->ip = s->cur_ip + 8;
            return disp + (s->r[index] << scale);
        case 0xf:
            disp = fetch32(s, s->cur_ip + 4);
            s->next_ip = s->ip = s->cur_ip + 8;
            return disp + s->r[abase] + (s->r[index] << scale);
        default:
            op_fault(s, I960_FS_OP_INVALID_OPCODE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* sysctl, icctl, dcctl                                               */
/* ------------------------------------------------------------------ */

static void require_supervisor(I960State *s)
{
    if (!(s->pc & I960_PC_EM)) {
        raise_fault(s, I960_FT_TYPE, I960_FS_TYPE_MISMATCH, false);
    }
}

static void do_reinit(I960State *s, uint32_t prcb, uint32_t new_ip)
{
    s->icache_on = false;
    s->dcache_on = false;
    if (!process_prcb(s, prcb)) {
        stop_with_error(s, I960_FAIL_ALIGN);
        return;
    }
    s->pc = 0x001f2002;         /* table 12-2: PC after a re-init */
    s->ip = new_ip & ~3u;
    if (s->ops->reinit) {
        s->ops->reinit(s->opaque);
    }
}

static void do_sysctl(I960State *s, uint32_t src1, uint32_t src2,
                      uint32_t *srcdst)
{
    uint32_t type = (src1 >> 8) & 0xff;

    require_supervisor(s);
    s->insn_cycles = CYC_CTL;
    if (s->ops->ctl && s->ops->ctl(s->opaque, I960_CTL_SYSCTL, src1, src2,
                                   srcdst)) {
        return;
    }
    switch (type) {
    case 0:
        post_sw(s, src1 & 0xff);
        break;
    case 1:
        break;
    case 2: {
        uint32_t mode = src1 & 0xff;

        qemu_log_mask(LOG_UNIMP, "i960: sysctl cache configuration mode %u "
                      "accepted without a cache model\n", mode);

        if (mode & 1) {
            s->icache_on = false;
        } else if (mode == 0) {
            s->icache_on = true;
        } else if (mode != 4 && mode != 6) {
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        break;
    }
    case 3:
        do_reinit(s, *srcdst, src2);
        s->skip_writeback = true;
        break;
    case 4: {
        uint32_t grp = (src1 & 0xff) * 16;

        for (uint32_t i = 0; i < 16; i += 4) {
            load_ctrl_word(s, grp + i);
        }
        break;
    }
    case 5: {
        uint32_t addr = I960_MMR_BASE | (src1 >> 16);
        uint32_t old = mmr_read(s, addr);

        mmr_write(s, addr, (src2 & *srcdst) | (old & ~*srcdst));
        *srcdst = old;
        break;
    }
    case 6:
        qemu_log_mask(LOG_UNIMP, "i960: sysctl breakpoint request: "
                      "hardware breakpoints are not modelled\n");
        *srcdst = 0;
        break;
    default:
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    }
}

static void do_cachectl(I960State *s, I960CtlKind kind, uint32_t src1,
                        uint32_t src2, uint32_t *srcdst)
{
    bool *on = kind == I960_CTL_ICCTL ? &s->icache_on : &s->dcache_on;

    require_supervisor(s);
    s->insn_cycles = CYC_CTL;
    if (s->ops->ctl && s->ops->ctl(s->opaque, kind, src1, src2, srcdst)) {
        return;
    }
    qemu_log_mask(LOG_UNIMP, "i960: %s operation %u accepted without a "
                  "cache model\n", kind == I960_CTL_ICCTL ? "icctl" : "dcctl",
                  src1);
    switch (src1) {
    case 0:
        *on = false;
        break;
    case 1:
        *on = true;
        break;
    case 2:
        break;
    case 3:
        if (kind == I960_CTL_ICCTL) {
            break;      /* load and lock: no cache to lock */
        }
        break;          /* dcctl 3: ensure coherency */
    case 4:
        *srcdst = *on ? 1 : 0;      /* geometry bits are not modelled */
        break;
    case 5:
        if (kind == I960_CTL_ICCTL) {
            *srcdst = 0;
            break;
        }
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    case 6:
        break;          /* store cache sets: nothing cached to store */
    case 8:
        if (kind == I960_CTL_DCCTL) {
            break;
        }
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    default:
        op_fault(s, I960_FS_OP_INVALID_OPERAND);
    }
}

/* ------------------------------------------------------------------ */
/* Instruction execution                                              */
/* ------------------------------------------------------------------ */

static void exec_reg(I960State *s, uint32_t insn)
{
    uint32_t opc = ((insn >> 20) & 0xff0) | ((insn >> 7) & 0xf);
    uint32_t t1, t2;
    uint64_t r64;
    unsigned d = (insn >> 19) & 0x1f;

    switch (opc) {
    case 0x580: /* notbit */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 ^ (1u << (t1 & 31)));
        break;
    case 0x581: /* and */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 & t1);
        break;
    case 0x582: /* andnot */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 & ~t1);
        break;
    case 0x583: /* setbit */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 | (1u << (t1 & 31)));
        break;
    case 0x584: /* notand */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), ~t2 & t1);
        break;
    case 0x586: /* xor */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 ^ t1);
        break;
    case 0x587: /* or */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 | t1);
        break;
    case 0x588: /* nor */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), ~t2 & ~t1);
        break;
    case 0x589: /* xnor */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), ~(t2 ^ t1));
        break;
    case 0x58a: /* not */
        t1 = reg_src1(s, insn);
        set_reg(s, reg_dst(s, insn), ~t1);
        break;
    case 0x58b: /* ornot */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 | ~t1);
        break;
    case 0x58c: /* clrbit */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 & ~(1u << (t1 & 31)));
        break;
    case 0x58d: /* notor */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), ~t2 | t1);
        break;
    case 0x58e: /* nand */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), ~t2 | ~t1);
        break;
    case 0x58f: /* alterbit */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn),
                (s->ac & 2) ? t2 | (1u << (t1 & 31))
                            : t2 & ~(1u << (t1 & 31)));
        break;

    case 0x590: /* addo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 + t1);
        break;
    case 0x591: /* addi */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 + t1);
        if (((t2 ^ (t2 + t1)) & (t1 ^ (t2 + t1))) >> 31) {
            int_overflow(s);
        }
        break;
    case 0x592: /* subo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 - t1);
        break;
    case 0x593: /* subi */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 - t1);
        if (((t2 ^ t1) & (t2 ^ (t2 - t1))) >> 31) {
            int_overflow(s);
        }
        break;
    case 0x594: /* cmpob */
        t1 = reg_src1(s, insn) & 0xff; t2 = reg_src2(s, insn) & 0xff;
        cmp_cc(s, t1 < t2, t1 == t2);
        break;
    case 0x595: /* cmpib */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, (int8_t)t1 < (int8_t)t2, (int8_t)t1 == (int8_t)t2);
        break;
    case 0x596: /* cmpos */
        t1 = reg_src1(s, insn) & 0xffff; t2 = reg_src2(s, insn) & 0xffff;
        cmp_cc(s, t1 < t2, t1 == t2);
        break;
    case 0x597: /* cmpis */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, (int16_t)t1 < (int16_t)t2, (int16_t)t1 == (int16_t)t2);
        break;
    case 0x598: /* shro */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t1 < 32 ? t2 >> t1 : 0);
        break;
    case 0x59a: /* shrdi */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        s->insn_cycles = 6;
        if (t1 >= 32) {
            set_reg(s, reg_dst(s, insn), 0);
        } else if ((int32_t)t2 < 0 && (t2 & ((1u << t1) - 1))) {
            set_reg(s, reg_dst(s, insn), ((int32_t)t2 >> t1) + 1);
        } else {
            set_reg(s, reg_dst(s, insn), (uint32_t)((int32_t)t2 >> t1));
        }
        break;
    case 0x59b: /* shri */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn),
                t1 < 32 ? (uint32_t)((int32_t)t2 >> t1)
                        : ((int32_t)t2 < 0 ? ~0u : 0));
        break;
    case 0x59c: /* shlo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t1 < 32 ? t2 << t1 : 0);
        break;
    case 0x59d: /* rotate */
        t1 = reg_src1(s, insn) & 31; t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), rol32(t2, t1));
        break;
    case 0x59e: { /* shli */
        unsigned count;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        count = t1 > 32 ? 32 : t1;
        while (((t2 >> 31) & 1) == ((t2 >> 30) & 1) && count > 0) {
            t2 <<= 1;
            count--;
        }
        set_reg(s, reg_dst(s, insn), t2);
        if (count > 0) {
            int_overflow(s);
        }
        break;
    }

    case 0x5a0: /* cmpo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, t1 < t2, t1 == t2);
        break;
    case 0x5a1: /* cmpi */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, (int32_t)t1 < (int32_t)t2, t1 == t2);
        break;
    case 0x5a2: /* concmpo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        if (!(s->ac & 4)) {
            set_cc(s, t1 <= t2 ? 2 : 1);
        }
        break;
    case 0x5a3: /* concmpi */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        if (!(s->ac & 4)) {
            set_cc(s, (int32_t)t1 <= (int32_t)t2 ? 2 : 1);
        }
        break;
    case 0x5a4: /* cmpinco */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, t1 < t2, t1 == t2);
        set_reg(s, reg_dst(s, insn), t2 + 1);
        break;
    case 0x5a5: /* cmpinci */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, (int32_t)t1 < (int32_t)t2, t1 == t2);
        set_reg(s, reg_dst(s, insn), t2 + 1);
        break;
    case 0x5a6: /* cmpdeco */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, t1 < t2, t1 == t2);
        set_reg(s, reg_dst(s, insn), t2 - 1);
        break;
    case 0x5a7: /* cmpdeci */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        cmp_cc(s, (int32_t)t1 < (int32_t)t2, t1 == t2);
        set_reg(s, reg_dst(s, insn), t2 - 1);
        break;
    case 0x5ac: /* scanbyte */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        t1 ^= t2;
        set_cc(s, (t1 & 0xff) == 0 || (t1 & 0xff00) == 0 ||
                  (t1 & 0xff0000) == 0 || (t1 >> 24) == 0 ? 2 : 0);
        break;
    case 0x5ad: /* bswap */
        t1 = reg_src1(s, insn);
        s->insn_cycles = 7;
        set_reg(s, reg_dst(s, insn), bswap32(t1));
        break;
    case 0x5ae: /* chkbit */
        t1 = reg_src1(s, insn) & 31; t2 = reg_src2(s, insn);
        set_cc(s, (t2 >> t1) & 1 ? 2 : 0);
        break;

    case 0x5b0: /* addc */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        r64 = (uint64_t)t2 + t1 + ((s->ac >> 1) & 1);
        set_reg(s, reg_dst(s, insn), (uint32_t)r64);
        set_cc(s, (((uint32_t)r64 ^ t1) & ((uint32_t)r64 ^ t2)) >> 31 |
                  ((r64 >> 32) & 1) << 1);
        break;
    case 0x5b2: /* subc */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        r64 = (uint64_t)t2 + (uint32_t)~t1 + ((s->ac >> 1) & 1);
        set_reg(s, reg_dst(s, insn), (uint32_t)r64);
        set_cc(s, ((t2 ^ t1) & (t2 ^ (uint32_t)r64)) >> 31 |
                  ((r64 >> 32) & 1) << 1);
        break;
    case 0x5b4: /* intdis */
        require_supervisor(s);
        s->icon |= I960_ICON_GIE;
        break;
    case 0x5b5: /* inten */
        require_supervisor(s);
        s->icon &= ~I960_ICON_GIE;
        break;

    case 0x5cc: /* mov */
        t1 = reg_src1(s, insn);
        set_reg(s, reg_dst(s, insn), t1);
        s->insn_cycles = 2;
        break;
    case 0x5d8: { /* eshro */
        uint64_t v;

        t1 = reg_src1(s, insn);
        v = reg_src2_pair(s, insn);
        set_reg(s, reg_dst(s, insn), (uint32_t)(v >> (t1 & 31)));
        s->insn_cycles = 11;
        break;
    }
    case 0x5dc: /* movl */
    case 0x5ec: /* movt */
    case 0x5fc: { /* movq */
        unsigned kind = (opc >> 4) & 0xf;
        unsigned n = kind == 0xd ? 2 : kind == 0xe ? 3 : 4;
        unsigned sidx = insn & 0x1f;
        uint32_t tmp[4];

        d = reg_dst(s, insn);
        chk_group(s, d, n == 2 ? 2 : 4);
        if (insn & (1u << 11)) {
            tmp[0] = sidx;
            tmp[1] = tmp[2] = tmp[3] = 0;
        } else {
            chk_group(s, sidx, n == 2 ? 2 : 4);
            memcpy(tmp, &s->r[sidx], n * sizeof(uint32_t));
        }
        if ((d + n) > 32) {
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        if (d <= I960_RIP && d + n > I960_RIP) {
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        memcpy(&s->r[d], tmp, n * sizeof(uint32_t));
        s->insn_cycles = n + 2;
        break;
    }

    case 0x610: /* atmod */
        t1 = reg_src1(s, insn) & ~3u; t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        r64 = mem_read(s, t1, 4);
        mem_write(s, t1, ((uint32_t)r64 & ~t2) | (s->r[d] & t2), 4);
        set_reg(s, d, (uint32_t)r64);
        s->insn_cycles = 24;
        break;
    case 0x612: /* atadd */
        t1 = reg_src1(s, insn) & ~3u; t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        r64 = mem_read(s, t1, 4);
        mem_write(s, t1, (uint32_t)r64 + t2, 4);
        set_reg(s, d, (uint32_t)r64);
        s->insn_cycles = 24;
        break;

    case 0x640: /* spanbit */
        t1 = reg_src1(s, insn);
        if (t1 == 0xffffffff) {
            set_reg(s, reg_dst(s, insn), 0xffffffff);
            set_cc(s, 0);
        } else {
            set_reg(s, reg_dst(s, insn), msb32(~t1));
            set_cc(s, 2);
        }
        s->insn_cycles = 6;
        break;
    case 0x641: /* scanbit */
        t1 = reg_src1(s, insn);
        if (t1 == 0) {
            set_reg(s, reg_dst(s, insn), 0xffffffff);
            set_cc(s, 0);
        } else {
            set_reg(s, reg_dst(s, insn), msb32(t1));
            set_cc(s, 2);
        }
        s->insn_cycles = 5;
        break;
    case 0x645: { /* modac */
        uint32_t old = s->ac;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        s->ac = (t2 & t1) | (old & ~t1);
        set_reg(s, reg_dst(s, insn), old);
        s->insn_cycles = 10;
        break;
    }
    case 0x650: /* modify */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        set_reg(s, d, (t2 & t1) | (s->r[d] & ~t1));
        s->insn_cycles = 6;
        break;
    case 0x651: /* extract */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        {
            uint32_t v = t1 >= 32 ? 0 : s->r[d] >> t1;
            uint32_t m = t2 >= 32 ? 0 : (0xffffffffu << t2);

            set_reg(s, d, v & ~m);
        }
        s->insn_cycles = 7;
        break;
    case 0x654: { /* modtc */
        uint32_t old = s->tc;
        uint32_t tempa;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        tempa = (0x0f000000u & old & t1) | (0x000000feu & t1);
        s->tc = (tempa & t2) | (old & ~tempa);
        set_reg(s, reg_dst(s, insn), old);
        s->insn_cycles = 10;
        break;
    }
    case 0x655: { /* modpc */
        uint32_t old = s->pc;
        unsigned oldprio = i960_priority(s);

        /* modpc src, mask, src/dst: src (src1) is a dummy */
        t1 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        if (t1 != 0) {
            require_supervisor(s);
            s->pc = (t1 & s->r[d]) | (old & ~t1);
            set_reg(s, d, old);
            if (oldprio > i960_priority(s)) {
                refresh_sw_prio(s);
            }
        } else {
            set_reg(s, d, old);
        }
        s->insn_cycles = 17;
        break;
    }
    case 0x658: { /* intctl */
        bool enabled = !(s->icon & I960_ICON_GIE);

        t1 = reg_src1(s, insn);
        require_supervisor(s);
        switch (t1) {
        case 0:
            s->icon |= I960_ICON_GIE;
            break;
        case 1:
            s->icon &= ~I960_ICON_GIE;
            break;
        case 2:
            break;
        default:
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        set_reg(s, reg_dst(s, insn), enabled ? 1 : 0);
        s->insn_cycles = 12;
        break;
    }
    case 0x659: /* sysctl */
    case 0x65b: /* icctl */
    case 0x65c: { /* dcctl */
        uint32_t v;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        v = s->r[d];
        if (opc == 0x659) {
            do_sysctl(s, t1, t2, &v);
        } else {
            do_cachectl(s, opc == 0x65b ? I960_CTL_ICCTL : I960_CTL_DCCTL,
                        t1, t2, &v);
        }
        if (s->skip_writeback) {
            s->skip_writeback = false;      /* reinit zeroed the registers */
        } else {
            set_reg(s, d, v);
        }
        break;
    }
    case 0x65d: /* halt */
        t1 = reg_src1(s, insn);
        require_supervisor(s);
        switch (t1) {
        case 0:
            s->icon |= I960_ICON_GIE;
            break;
        case 1:
            s->icon &= ~I960_ICON_GIE;
            break;
        case 2:
            break;
        default:
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        s->halted = true;
        s->idle_hint = true;
        if (s->ops->halted) {
            s->ops->halted(s->opaque);
        }
        break;

    case 0x660: /* calls */
        t1 = reg_src1(s, insn);
        do_calls(s, t1);
        break;
    case 0x66b: /* mark */
        s->insn_cycles = 8;
        if (s->pc & I960_PC_TE) {
            s->trace_events |= I960_TC_MARK;
        }
        break;
    case 0x66c: /* fmark */
        s->insn_cycles = 8;
        if (s->pc & I960_PC_TE) {
            s->trace_force |= I960_TC_MARK;
        }
        break;
    case 0x66d: /* flushreg */
        i960_flush_regs(s);
        s->insn_cycles = CYC_FLUSHREG;
        break;
    case 0x66f: /* syncf */
        s->insn_cycles = 4;
        break;

    case 0x670: /* emul */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        d = reg_dst(s, insn);
        chk_group(s, d, 2);
        r64 = (uint64_t)t1 * t2;
        set_reg(s, d, (uint32_t)r64);
        set_reg(s, d + 1, r64 >> 32);
        s->insn_cycles = 7;
        break;
    case 0x671: { /* ediv */
        uint64_t n;

        t1 = reg_src1(s, insn);
        n = reg_src2_pair(s, insn);
        d = reg_dst(s, insn);
        chk_group(s, d, 2);
        s->insn_cycles = CYC_DIV;
        if (t1 == 0) {
            raise_fault(s, I960_FT_ARITHMETIC, I960_FS_ARITH_ZERO_DIVIDE, true);
        }
        set_reg(s, d + 1, (uint32_t)(n / t1));
        set_reg(s, d, (uint32_t)(n % t1));
        break;
    }

    case 0x701: /* mulo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        set_reg(s, reg_dst(s, insn), t2 * t1);
        s->insn_cycles = CYC_MUL;
        break;
    case 0x708: /* remo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        s->insn_cycles = CYC_DIV;
        if (t1 == 0) {
            raise_fault(s, I960_FT_ARITHMETIC, I960_FS_ARITH_ZERO_DIVIDE, true);
        }
        set_reg(s, reg_dst(s, insn), t2 - (t2 / t1) * t1);
        break;
    case 0x70b: /* divo */
        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        s->insn_cycles = CYC_DIV;
        if (t1 == 0) {
            raise_fault(s, I960_FT_ARITHMETIC, I960_FS_ARITH_ZERO_DIVIDE, true);
        }
        set_reg(s, reg_dst(s, insn), t2 / t1);
        break;
    case 0x741: { /* muli */
        int64_t p;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        p = (int64_t)(int32_t)t2 * (int32_t)t1;
        set_reg(s, reg_dst(s, insn), (uint32_t)p);
        s->insn_cycles = CYC_MUL;
        if (p > INT32_MAX || p < INT32_MIN) {
            int_overflow(s);
        }
        break;
    }
    case 0x748: /* remi */
    case 0x749: /* modi */
    case 0x74b: { /* divi */
        int32_t a, b, q, rem;

        t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
        a = t2; b = t1;
        s->insn_cycles = CYC_DIV;
        if (b == 0) {
            raise_fault(s, I960_FT_ARITHMETIC, I960_FS_ARITH_ZERO_DIVIDE, true);
        }
        if (a == INT32_MIN && b == -1) {
            q = INT32_MIN;
            rem = 0;
            if (opc == 0x74b) {
                set_reg(s, reg_dst(s, insn), (uint32_t)q);
                int_overflow(s);
                break;
            }
        } else {
            q = a / b;
            rem = a - q * b;
        }
        if (opc == 0x749 && rem != 0 && ((a < 0) != (b < 0))) {
            rem += b;
        }
        set_reg(s, reg_dst(s, insn),
                opc == 0x74b ? (uint32_t)q : (uint32_t)rem);
        break;
    }

    default:
        if (opc >= 0x780 && opc <= 0x7f4 && (opc & 0xf) <= 4) {
            unsigned mask = (opc >> 4) & 7;
            bool cond;

            t1 = reg_src1(s, insn); t2 = reg_src2(s, insn);
            cond = cond_ok(s, mask);
            d = reg_dst(s, insn);
            switch (opc & 0xf) {
            case 0:
                if (cond) {
                    set_reg(s, d, t2 + t1);
                }
                break;
            case 1:
                if (cond) {
                    uint32_t r = t2 + t1;

                    set_reg(s, d, r);
                    if (((t2 ^ r) & (t1 ^ r)) >> 31) {
                        int_overflow(s);
                    }
                }
                break;
            case 2:
                if (cond) {
                    set_reg(s, d, t2 - t1);
                }
                break;
            case 3:
                if (cond) {
                    uint32_t r = t2 - t1;

                    set_reg(s, d, r);
                    if (((t2 ^ t1) & (t2 ^ r)) >> 31) {
                        int_overflow(s);
                    }
                }
                break;
            case 4:
                set_reg(s, d, cond ? t2 : t1);
                break;
            }
            break;
        }
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
}

static void exec_cobr(I960State *s, uint32_t insn)
{
    uint32_t op = insn >> 24;
    uint32_t t1, t2;
    uint32_t disp = (uint32_t)sextract32(insn, 2, 11) * 4;
    uint32_t target = s->cur_ip + disp;
    unsigned dst = (insn >> 19) & 0x1f;
    bool taken;

    s->insn_cycles = 2;
    if (op < 0x30) {
        unsigned mask = op & 7;

        if (insn & 0x2000) {
            op_fault(s, I960_FS_OP_INVALID_OPCODE);
        }
        set_reg(s, dst, cond_ok(s, mask) ? 1 : 0);
        s->insn_cycles = 4;
        return;
    }
    t1 = (insn & 0x2000) ? dst : s->r[dst];
    t2 = s->r[(insn >> 14) & 0x1f];
    switch (op) {
    case 0x30: /* bbc */
    case 0x37: /* bbs */
        /* bbc: taken (bit clear) cc=000, else 010; bbs the opposite */
        taken = ((t2 >> (t1 & 31)) & 1) == (op == 0x37);
        set_cc(s, (taken == (op == 0x37)) ? 2 : 0);
        break;
    case 0x31 ... 0x36: /* cmpobg .. cmpoble */
        cmp_cc(s, t1 < t2, t1 == t2);
        taken = cond_ok(s, op & 7);
        break;
    case 0x38 ... 0x3f: /* cmpibno .. cmpibo */
        cmp_cc(s, (int32_t)t1 < (int32_t)t2, t1 == t2);
        taken = cond_ok(s, op & 7);
        break;
    default:
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    if (taken) {
        branch_to(s, target);
    }
}

static void exec_ctrl(I960State *s, uint32_t insn)
{
    uint32_t op = insn >> 24;
    uint32_t target = s->cur_ip + (uint32_t)sextract32(insn, 2, 22) * 4;

    s->insn_cycles = 1;
    switch (op) {
    case 0x08: /* b */
        branch_to(s, target);
        break;
    case 0x09: /* call */
        do_local_call(s, target);
        break;
    case 0x0a: /* ret */
        do_ret(s);
        break;
    case 0x0b: /* bal */
        s->r[I960_G14] = s->next_ip;
        s->ip = target & ~3u;
        s->insn_cycles = 5;
        if ((s->pc & I960_PC_TE) && (s->tc & I960_TC_CALL)) {
            s->r[I960_PFP] |= 8;
            s->trace_events |= I960_TC_CALL;
        }
        break;
    case 0x10 ... 0x17: /* bno .. bo */
        if (cond_ok(s, op & 7)) {
            branch_to(s, target);
        }
        break;
    case 0x18 ... 0x1f: /* faultno .. faulto */
        if (cond_ok(s, op & 7)) {
            raise_fault(s, I960_FT_CONSTRAINT, I960_FS_CONSTRAINT_RANGE, true);
        }
        s->insn_cycles = 1;
        break;
    default:
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
}

static void exec_mem(I960State *s, uint32_t insn)
{
    uint32_t op = insn >> 24;
    unsigned r = (insn >> 19) & 0x1f;
    uint32_t ea, v;
    uint32_t w[4];

    ea = mem_ea(s, insn);
    s->insn_cycles = 2;
    switch (op) {
    case 0x80: /* ldob */
        set_reg(s, r, mem_read(s, ea, 1));
        break;
    case 0x82: /* stob */
        mem_write(s, ea, s->r[r], 1);
        break;
    case 0x84: /* bx */
        branch_to(s, ea);
        break;
    case 0x85: /* balx */
        set_reg(s, r, s->next_ip);
        s->ip = ea & ~3u;
        s->insn_cycles = 5;
        if ((s->pc & I960_PC_TE) && (s->tc & I960_TC_CALL)) {
            s->r[I960_PFP] |= 8;
            s->trace_events |= I960_TC_CALL;
        }
        break;
    case 0x86: /* callx */
        do_local_call(s, ea);
        break;
    case 0x88: /* ldos */
        set_reg(s, r, mem_read(s, ea, 2));
        break;
    case 0x8a: /* stos */
        mem_write(s, ea, s->r[r], 2);
        break;
    case 0x8c: /* lda */
        set_reg(s, r, ea);
        s->insn_cycles = 1;
        break;
    case 0x90: /* ld */
        set_reg(s, r, mem_read(s, ea, 4));
        break;
    case 0x92: /* st */
        mem_write(s, ea, s->r[r], 4);
        break;
    case 0x98: /* ldl */
    case 0xa0: /* ldt */
    case 0xb0: { /* ldq */
        unsigned n = op == 0x98 ? 2 : op == 0xa0 ? 3 : 4;

        chk_group(s, r, n == 2 ? 2 : 4);
        if (n == 3) {
            /* triple-word transfers are a quad burst on the bus */
            mem_read_group(s, ea, w, 3);
        } else {
            mem_read_group(s, ea, w, n);
        }
        if (r <= I960_RIP && r + n > I960_RIP) {
            op_fault(s, I960_FS_OP_INVALID_OPERAND);
        }
        memcpy(&s->r[r], w, n * sizeof(uint32_t));
        s->insn_cycles = 2 + n;
        break;
    }
    case 0x9a: /* stl */
    case 0xa2: /* stt */
    case 0xb2: { /* stq */
        unsigned n = op == 0x9a ? 2 : op == 0xa2 ? 3 : 4;

        chk_group(s, r, n == 2 ? 2 : 4);
        memcpy(w, &s->r[r], n * sizeof(uint32_t));
        mem_write_group(s, ea, w, n);
        s->insn_cycles = 2 + n;
        break;
    }
    case 0xc0: /* ldib */
        set_reg(s, r, (uint32_t)(int8_t)mem_read(s, ea, 1));
        break;
    case 0xc2: /* stib */
        v = s->r[r];
        mem_write(s, ea, v, 1);
        if ((int32_t)v != (int8_t)v) {
            int_overflow(s);
        }
        break;
    case 0xc8: /* ldis */
        set_reg(s, r, (uint32_t)(int16_t)mem_read(s, ea, 2));
        break;
    case 0xca: /* stis */
        v = s->r[r];
        mem_write(s, ea, v, 2);
        if ((int32_t)v != (int16_t)v) {
            int_overflow(s);
        }
        break;
    default:
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
}

static void step(I960State *s)
{
    uint32_t insn, op;

    s->cur_ip = s->ip & ~3u;
    s->next_ip = s->cur_ip + 4;
    s->insn_cycles = 1;
    s->trace_events = 0;
    s->trace_force = 0;
    s->trace_en = s->pc & I960_PC_TE;
    s->unaligned = false;
    insn = fetch32(s, s->cur_ip);
    s->ip = s->next_ip;
    op = insn >> 24;
    if (op < 0x20) {
        exec_ctrl(s, insn);
    } else if (op < 0x28 || (op >= 0x30 && op < 0x40)) {
        exec_cobr(s, insn);
    } else if (op >= 0x58 && op < 0x80) {
        exec_reg(s, insn);
    } else if (op >= 0x80) {
        exec_mem(s, insn);
    } else {
        op_fault(s, I960_FS_OP_INVALID_OPCODE);
    }
    if (s->unaligned && !((s->fault_config >> 30) & 1)) {
        raise_fault_data(s, I960_FT_OPERATION, I960_FS_OP_UNALIGNED, true,
                         s->unaligned_ea);
    }
}

static void trace_check(I960State *s)
{
    uint32_t ev = (s->trace_events & s->tc) | s->trace_force;

    if (s->tc & I960_TC_INSN) {
        ev |= I960_TC_INSN;
    }
    ev &= 0xfe;
    if (s->trace_en && ev) {
        s->flt.type = I960_FT_TRACE;
        s->flt.subtype = ev;
        s->flt.rip_next = true;
        s->flt.has_data = false;
        s->next_ip = s->ip;
        longjmp(s->jb, 1);
    }
}

int64_t i960_run(I960State *s, int64_t cycles)
{
    int64_t start = s->cycles;

    if (s->failed) {
        return 0;
    }
    if (setjmp(s->jb)) {
        if (s->in_fault) {
            /* a fault while building a fault frame: system error */
            s->in_fault = false;
            stop_with_error(s, I960_FAIL_SYSERR);
            return s->cycles - start;
        }
        s->in_fault = true;
        deliver_fault(s);
        s->in_fault = false;
        s->cycles += s->insn_cycles;
        s->spin_count = 0;
    }
    s->idle_hint = false;
    while (s->cycles - start < cycles && !s->failed) {
        s->insn_cycles = 1;
        if (service_interrupts(s)) {
            s->cycles += s->insn_cycles;
            continue;
        }
        if (s->halted) {
            s->idle_hint = true;
            break;
        }
        step(s);
        trace_check(s);
        s->cycles += s->insn_cycles;
        if (s->spin_count >= CYC_SPIN_LIMIT) {
            s->idle_hint = true;
            s->spin_count = 0;
            break;
        }
    }
    return s->cycles - start;
}

static const VMStateDescription vmstate_i960_frame = {
    .name = "i960/frame",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(r, I960RegFrame, 16),
        VMSTATE_UINT32(fp, I960RegFrame),
        VMSTATE_END_OF_LIST()
    }
};

static int i960_post_load(void *opaque, int version_id)
{
    I960State *s = opaque;

    /* indices that later select array elements */
    if (s->rcache_n > I960_RCACHE_FRAMES ||
        s->rcache_reserved > I960_RCACHE_FRAMES ||
        s->sw_prio > 31 || s->variant > I960_VARIANT_RX) {
        return -EINVAL;
    }
    s->idle_hint = false;
    s->spin_count = 0;
    return 0;
}

/* Architectural state only; jmp_buf and per-instruction scratch are not. */
const VMStateDescription vmstate_i960 = {
    .name = "i960",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i960_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(variant, I960State),
        VMSTATE_UINT32(boot_pmcon14_15, I960State),
        VMSTATE_BOOL(boot_big_endian, I960State),
        VMSTATE_UINT32_ARRAY(r, I960State, 32),
        VMSTATE_STRUCT_ARRAY(rcache, I960State, I960_RCACHE_FRAMES, 0,
                             vmstate_i960_frame, I960RegFrame),
        VMSTATE_UINT32(rcache_n, I960State),
        VMSTATE_UINT32(rcache_reserved, I960State),
        VMSTATE_UINT32(ip, I960State),
        VMSTATE_UINT32(ac, I960State),
        VMSTATE_UINT32(pc, I960State),
        VMSTATE_UINT32(tc, I960State),
        VMSTATE_UINT32(prcb, I960State),
        VMSTATE_UINT32(fault_table, I960State),
        VMSTATE_UINT32(ctrl_table, I960State),
        VMSTATE_UINT32(int_table, I960State),
        VMSTATE_UINT32(sys_proc_table, I960State),
        VMSTATE_UINT32(isp, I960State),
        VMSTATE_UINT32(ssp, I960State),
        VMSTATE_BOOL(ssp_te, I960State),
        VMSTATE_UINT32(fault_config, I960State),
        VMSTATE_UINT32(bcon, I960State),
        VMSTATE_UINT32(ipnd, I960State),
        VMSTATE_UINT32(imsk, I960State),
        VMSTATE_UINT32(icon, I960State),
        VMSTATE_UINT32_ARRAY(imap, I960State, 3),
        VMSTATE_UINT8(xint_pins, I960State),
        VMSTATE_BOOL(nmi_pending, I960State),
        VMSTATE_BOOL(block_nmi, I960State),
        VMSTATE_UINT32(sw_prio, I960State),
        VMSTATE_UINT8_ARRAY(data_ram, I960State, I960_DATA_RAM_SIZE),
        VMSTATE_BOOL(halted, I960State),
        VMSTATE_BOOL(failed, I960State),
        VMSTATE_BOOL(icache_on, I960State),
        VMSTATE_BOOL(dcache_on, I960State),
        VMSTATE_UINT64(cycles, I960State),
        VMSTATE_END_OF_LIST()
    }
};
