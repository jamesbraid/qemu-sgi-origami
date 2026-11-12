/*
 * Intel i960 Rx/Jx core instruction-set interpreter.
 *
 * A device-internal interpreter for the I/O processor that sits on a board
 * (an i960 Rx or Jx class part), not a TCG target.  The core owns the
 * architectural state: global and local registers, the local register
 * cache, AC/PC/TC, the interrupt controller registers (IPND, IMSK, ICON,
 * IMAP0-2), 1 KB of on-chip data RAM and the initialisation boot record
 * (IBR) / process control block (PRCB) boot sequence.  Everything outside
 * the core (external bus, flash, on-chip timers, ATU, DMA, messaging unit)
 * is reached through the callbacks in I960Ops.
 *
 * Ported from MAME src/devices/cpu/i960/ (i960.cpp, i960.h).
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
 * Reworked as a plain C core following the Intel i960 Rx and Jx
 * developer's manuals.  The Rx/Jx behaviour in those manuals replaces MAME's
 * i960 KB behaviour wherever the two differ; the differences are listed in
 * hw/coproc/i960/i960.c.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_COPROC_I960_I960_H
#define HW_COPROC_I960_I960_H

#include <setjmp.h>

/* Registers r0-r15 are the local set, r16-r31 are g0-g15. */
#define I960_PFP        0
#define I960_SP         1
#define I960_RIP        2
#define I960_G0         16
#define I960_G14        30
#define I960_FP         31

/* Local register sets the on-chip cache can hold besides the current one. */
#define I960_RCACHE_FRAMES 7

/* Fixed addresses from the manuals. */
#define I960_IBR_ADDR       0xfeffff30u
#define I960_FAIL_BUS_TEST  0xfeffff64u
#define I960_FAIL_ALIGN     0xfeffff70u
#define I960_FAIL_SYSERR    0xfeffff68u
#define I960_DATA_RAM_SIZE  0x400u
#define I960_RAM_PROT_SIZE  0x40u      /* always user-write protected */
/* Fault record below the new frame pointer; inferred, the prose says 80. */
#define I960_FAULT_RECORD_SIZE 64u
#define I960_MMR_BASE       0xff000000u
#define I960_MMR_SUPERVISOR 0xff008000u

/* Memory-mapped registers the core implements itself. */
#define I960_MMR_IPND       0xff008500u
#define I960_MMR_IMSK       0xff008504u
#define I960_MMR_ICON       0xff008510u
#define I960_MMR_IMAP0      0xff008520u
#define I960_MMR_IMAP1      0xff008524u
#define I960_MMR_IMAP2      0xff008528u
#define I960_MMR_PRCB       0xff008700u
#define I960_MMR_ISP        0xff008704u
#define I960_MMR_SSP        0xff008708u
#define I960_MMR_DEVICEID   0xff008710u
#define I960_MMR_BCON       0xff0086fcu

/*
 * BCON bits.  CTV is bit 0, IRP bit 1, SIRP bit 2 on every part.  The Rx
 * manual's table 12-3 (and the RM/RN table 12-3) print CTV at bit 2 and
 * SIRP at bit 0; that table is wrong.  The Jx (figure 13-3), VH (table
 * 13-3, which corrects the table but keeps the stale "bit 2 of BCON"
 * prose) and Hx (figure 14-3) manuals all give CTV 0 / IRP 1 / SIRP 2, and
 * an RD boot ROM that sets BCON = 1 and then writes data RAM at 0x10 as
 * supervisor only works with that layout.  The core uses IRP and SIRP; the
 * rest of BCON belongs to the device.
 */
#define I960_BCON_CTV       (1u << 0)
#define I960_BCON_IRP       (1u << 1)   /* user writes to data RAM fault */
#define I960_BCON_SIRP      (1u << 2)   /* first 64 bytes read-only */

/*
 * Which part the core models; chosen at i960_init() and migrated.  No
 * behaviour depends on it today (BCON is the same on both); it is kept for
 * differences between the parts that turn up later.
 */
typedef enum I960Variant {
    I960_VARIANT_JX,
    I960_VARIANT_RX,        /* the Rx-class I/O processor, e.g. i960 RD */
} I960Variant;

/* AC register */
#define I960_AC_CC_MASK     0x7u
#define I960_AC_OF          (1u << 8)
#define I960_AC_OM          (1u << 12)
#define I960_AC_NIF         (1u << 15)

/* PC register */
#define I960_PC_TE          (1u << 0)
#define I960_PC_EM          (1u << 1)   /* 1 = supervisor */
#define I960_PC_TFP         (1u << 10)
#define I960_PC_S           (1u << 13)  /* 1 = interrupted state */
#define I960_PC_PRIO_SHIFT  16
#define I960_PC_PRIO_MASK   (0x1fu << I960_PC_PRIO_SHIFT)

/* TC register mode bits (trace modes); bit 24-27 are breakpoint flags. */
#define I960_TC_INSN        (1u << 1)
#define I960_TC_BRANCH      (1u << 2)
#define I960_TC_CALL        (1u << 3)
#define I960_TC_RET         (1u << 4)
#define I960_TC_PRERET      (1u << 5)
#define I960_TC_SUPER       (1u << 6)
#define I960_TC_MARK        (1u << 7)

/* ICON register */
#define I960_ICON_IM_MASK   0x3u        /* 0 dedicated, 1 expanded, 2 mixed */
#define I960_ICON_GIE       (1u << 10)  /* 1 = interrupts disabled */
#define I960_ICON_MO_SHIFT  11
#define I960_ICON_MO_MASK   (0x3u << I960_ICON_MO_SHIFT)

/* IPND/IMSK bit positions: XINT0-7 are bits 0-7, TINT0/1 are 12/13. */
#define I960_IPND_TINT0     (1u << 12)
#define I960_IPND_TINT1     (1u << 13)

/* Fault types and subtypes (Jx manual, table 8-1). */
#define I960_FT_TRACE       0x1
#define I960_FT_OPERATION   0x2
#define I960_FT_ARITHMETIC  0x3
#define I960_FT_CONSTRAINT  0x5
#define I960_FT_PROTECTION  0x7
#define I960_FT_TYPE        0xa

#define I960_FS_OP_INVALID_OPCODE   0x1
#define I960_FS_OP_UNIMPLEMENTED    0x2
#define I960_FS_OP_UNALIGNED        0x3
#define I960_FS_OP_INVALID_OPERAND  0x4
#define I960_FS_ARITH_OVERFLOW      0x1
#define I960_FS_ARITH_ZERO_DIVIDE   0x2
#define I960_FS_CONSTRAINT_RANGE    0x1
#define I960_FS_PROTECTION_LENGTH   0x2
#define I960_FS_TYPE_MISMATCH       0x1
#define I960_FS_TRACE_INSN          0x02
#define I960_FS_TRACE_BRANCH        0x04
#define I960_FS_TRACE_CALL          0x08
#define I960_FS_TRACE_RET           0x10
#define I960_FS_TRACE_PRERET        0x20
#define I960_FS_TRACE_SUPER         0x40
#define I960_FS_TRACE_MARK          0x80

typedef enum I960CtlKind {
    I960_CTL_SYSCTL,
    I960_CTL_ICCTL,
    I960_CTL_DCCTL,
} I960CtlKind;

typedef struct I960State I960State;

typedef struct I960RegFrame {
    uint32_t r[16];
    uint32_t fp;
} I960RegFrame;

/*
 * Callbacks into the device.  A callback must not call i960_run().  It may
 * call i960_set_xint(), i960_set_tint() and i960_nmi(); i960_post_interrupt()
 * reads and writes the interrupt table through ops->read and ops->write, so
 * it re-enters those callbacks and must not be called from inside them.
 *
 * Migration: the architectural state is described by vmstate_i960 (below);
 * a device embeds it with VMSTATE_I960.  It covers variant, r, rcache, ip,
 * ac, pc, tc, bcon, the interrupt registers, the cached table pointers,
 * the data RAM, halted/failed, the cache enables, cycles and the boot
 * record fields.  Not migrated: ops, opaque and device_id (the device
 * supplies them at init), idle_hint and the spin detector (rebuilt as the
 * core runs), and all per-instruction scratch (cur_ip, next_ip, budget,
 * insn_cycles, trace_events, trace_force, trace_en, unaligned, flt,
 * in_fault, jb, stored, skip_writeback).  A migrated core must be
 * between instructions, which i960_run() always leaves it.  The device
 * must migrate its own state.  Loading rejects out-of-range indices.
 *
 * All addresses are 32-bit physical addresses
 * and all data is little-endian.  Only `read` and `write` are required.
 * The core splits unaligned accesses into aligned ones before calling.
 */
typedef struct I960Ops {
    /* size is 1, 2 or 4; addr is aligned to size */
    uint32_t (*read)(void *opaque, uint32_t addr, unsigned size);
    void (*write)(void *opaque, uint32_t addr, uint32_t val, unsigned size);

    /*
     * Optional.  ldl/ldt/ldq and stl/stt/stq are word bursts of 2-4 words
     * from a word-aligned address.  Without these the core issues word
     * reads or writes in ascending order, which is the same sequence a
     * burst-aware device sees one beat at a time.
     */
    void (*read_burst)(void *opaque, uint32_t addr, uint32_t *words,
                       unsigned nwords);
    void (*write_burst)(void *opaque, uint32_t addr, const uint32_t *words,
                        unsigned nwords);

    /* Optional.  Instruction fetch; defaults to read(addr, 4). */
    uint32_t (*fetch)(void *opaque, uint32_t addr);

    /*
     * Optional.  sysctl/icctl/dcctl.  src1 and src2 are the instruction's
     * operands and *srcdst is the third operand's value on entry and is
     * written back to the register on return.  Return true when the
     * device handled the operation; false lets the core apply its own
     * behaviour (cache operations are accepted and reported disabled,
     * unknown message types fault with INVALID_OPERAND).
     */
    bool (*ctl)(void *opaque, I960CtlKind kind, uint32_t src1, uint32_t src2,
                uint32_t *srcdst);

    /* Optional.  The core entered halt mode via the halt instruction. */
    void (*halted)(void *opaque);

    /*
     * Optional.  The core finished a software re-initialisation (sysctl
     * message 3), after the control table was reloaded.  The tail of the
     * manual's Process_PRCB flow is the device's job: clear TMR0/1 bits
     * 1-5 (keep TRR and TCR), and clear DLMCON.dcen and LMMR0/1.lmte.
     */
    void (*reinit)(void *opaque);

    /*
     * Optional.  Initialisation failed (fail code 0xfeffffnn, as the
     * FAIL pin message would carry) or a system error occurred.  The
     * core stays stopped until the next i960_reset().
     */
    void (*system_error)(void *opaque, uint32_t fail_code);
} I960Ops;

struct I960State {
    /* Configuration, set before i960_reset(). */
    const I960Ops *ops;
    void *opaque;
    uint32_t variant;           /* an I960Variant */
    uint32_t device_id;         /* value left in g0 after initialisation */

    /* Architectural state. */
    uint32_t r[32];
    uint32_t ip;                /* address of the next instruction */
    uint32_t ac;
    uint32_t pc;
    uint32_t tc;

    /* Local register cache: saved sets, oldest first. */
    I960RegFrame rcache[I960_RCACHE_FRAMES];
    unsigned rcache_n;
    unsigned rcache_reserved;   /* frames kept free for priority >= 28 */

    /* Pointers cached from the PRCB and system procedure table. */
    uint32_t prcb;
    uint32_t fault_table;
    uint32_t ctrl_table;
    uint32_t int_table;
    uint32_t sys_proc_table;
    uint32_t isp;
    uint32_t ssp;
    bool ssp_te;
    uint32_t fault_config;
    uint32_t bcon;              /* writes are also forwarded to the device */

    /* Interrupt controller. */
    uint32_t ipnd;              /* latched dedicated-mode requests */
    uint32_t imsk;
    uint32_t icon;
    uint32_t imap[3];
    uint8_t xint_pins;          /* asserted external interrupt pins */
    bool nmi_pending;
    bool block_nmi;
    unsigned sw_prio;           /* highest priority posted in int table */

    uint8_t data_ram[I960_DATA_RAM_SIZE];

    /* Boot record fields the device may want to look at. */
    uint32_t boot_pmcon14_15;   /* the four IBR PMCON bytes, byte 0 lowest */
    bool boot_big_endian;       /* IBR asked for big-endian; unsupported */

    /* Run state. */
    bool halted;                /* executing halt or stopped by an error */
    bool failed;                /* initialisation or system error */
    bool gie_disabled_by_halt;
    bool idle_hint;             /* polling loop detected, see i960_run() */
    uint64_t cycles;            /* approximate, see i960_run() */

    /* Per-instruction scratch; not part of the device interface. */
    uint32_t cur_ip;
    uint32_t next_ip;
    int64_t budget;
    unsigned insn_cycles;
    uint32_t trace_events;
    uint32_t trace_force;
    bool icache_on;
    bool dcache_on;
    bool unaligned;
    uint32_t unaligned_ea;
    struct {
        uint32_t type, subtype, data;
        bool rip_next, has_data;
    } flt;
    bool in_fault;
    jmp_buf jb;
    uint32_t spin_ip;
    uint32_t spin_hash;
    unsigned spin_count;
    bool stored;
    bool trace_en;
    bool skip_writeback;
};

extern const struct VMStateDescription vmstate_i960;
#define VMSTATE_I960(_f, _s) \
    VMSTATE_STRUCT(_f, _s, 1, vmstate_i960, I960State)

void i960_init(I960State *s, I960Variant variant, const I960Ops *ops,
               void *opaque);

/*
 * Cold reset: run the IBR / PRCB boot sequence through the read callback
 * and leave the core ready to execute the first instruction.  On a failed
 * bus confidence check or a misaligned data structure the core stops
 * (s->failed) and reports the fail code through ops->system_error.
 */
void i960_reset(I960State *s);

/*
 * Run for about `cycles` cycles.  The count is an approximation taken from
 * the Jx manual's execution-time column with no pipeline, cache or bus
 * wait model, so a device should treat it as a rate, not as timing.
 * Returns the cycles actually consumed, which can exceed the request by
 * one instruction.  Returns early with s->idle_hint set when the core
 * halted, or when it has spent a stretch of iterations in a polling loop
 * that left every register, AC and the branch address unchanged; the
 * caller can sleep until the next event.  Any interrupt input clears the hint.
 */
int64_t i960_run(I960State *s, int64_t cycles);

/* Dedicated/expanded/mixed-mode external pins; bit n set = XINTn asserted. */
void i960_set_xint(I960State *s, uint8_t asserted);

/* Latch a timer interrupt request (timer 0 or 1) in IPND. */
void i960_set_tint(I960State *s, unsigned timer);

/* Non-maskable interrupt (edge). */
void i960_nmi(I960State *s);

/*
 * Post interrupt `vector` (8-255) into the interrupt table, as sysctl
 * message 0 and an external agent would.  Serviced when its priority is
 * above the current process priority.
 */
void i960_post_interrupt(I960State *s, uint8_t vector);

/* Current process priority (0-31). */
static inline unsigned i960_priority(const I960State *s)
{
    return (s->pc & I960_PC_PRIO_MASK) >> I960_PC_PRIO_SHIFT;
}

/* Write all cached local register sets to their save areas. */
void i960_flush_regs(I960State *s);

#endif /* HW_COPROC_I960_I960_H */
