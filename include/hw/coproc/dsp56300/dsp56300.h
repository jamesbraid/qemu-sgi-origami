/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Motorola/NXP DSP56300-family (DSP56301) instruction-set interpreter.
 *
 * A device-internal interpreter, not a TCG target: a board model embeds one
 * DSP56300State per DSP, hands it memory through DSP56300Ops and calls
 * dsp56300_run() with a cycle budget from a timer. On-chip peripherals (host
 * interface, ESSI, DMA, timers, PLL) are not part of the core. They live in
 * the X and Y I/O windows (X:$FFFF80-$FFFFFF and Y:$FFFF80-$FFFFFF) that the
 * core reaches through the same callbacks as memory, and they signal the core
 * through dsp56300_irq_raise() and friends.
 *
 * Written from the NXP DSP56300 Family Manual (rev 5). Known limits:
 *  - Sixteen-bit Arithmetic mode (SR.SA), Sixteen-bit Compatibility mode
 *    (SR.SC), the stack extension (OMR.SEN) and the double-precision
 *    multiply mode (SR.DM) are stored but not modelled.
 *  - Cycle counts are an approximation of the instruction timing table: the
 *    best-case count with the pipeline full and no wait states, plus the
 *    pre-update, long-absolute and long-immediate extras.
 *  - The instruction cache and its PLOCK/PUNLOCK/PFLUSH instructions are
 *    treated as an always-absent cache: they raise an illegal instruction
 *    exception unless SR.CE is set, and do nothing when it is.
 *  - On-chip debug (OnCE, DEBUG, DEBUGcc) is not modelled; DEBUG is a NOP.
 *  Setting SR.SA, SR.DM or OMR.SEN logs LOG_UNIMP.
 *
 * Reserved encodings: an undefined opcode, a reserved effective address
 * mode, a reserved register field in a parallel move, and an instruction
 * the manual forbids in a REP (REP, DO, WAIT, STOP, jumps and calls) raise
 * the illegal-instruction exception, and log LOG_GUEST_ERROR. A reserved
 * register code in MOVEC, MOVEM, MOVEP, LUA or the displacement MOVEs reads
 * as zero and ignores writes, and logs LOG_GUEST_ERROR.
 *
 * Where the manual's text and its encoding tables disagree or are silent,
 * the core follows these readings:
 *  - The bit number of the bit-manipulation and bit-test-and-branch
 *    instructions is the five-bit field bbbbb at bits 4-0 (the BCLR/BSET/
 *    BCHG/BTST encodings print four bits, which cannot reach bit 23); bit 5
 *    selects clear/change against set/test.
 *  - MOVEP with W set writes the peripheral.
 *  - MPYsu/MACsu/DMACsu: the first operand of the S1,S2 table entry is
 *    signed, the second unsigned; the "uu" forms are both unsigned. DMAC
 *    shifts the accumulator right 24 bits arithmetically in all variants.
 *  - The scaling-mode field is S1:S0 = 01 scale down, 10 scale up, 00 none;
 *    11 is treated as no scaling.
 *  - A long interrupt sets the interrupt mask to the level of the source
 *    plus one (capped at 3), so the same level cannot interrupt itself.
 *  - STOP is left only by reset or by IRQA being pending, even if masked;
 *    the core then services the highest-priority unmasked interrupt, or
 *    resumes after the STOP when there is none. WAIT leaves on any unmasked
 *    interrupt.
 *  - An address register at a modulo boundary offset outside the buffer, an
 *    offset larger than the modulus that is not a multiple of the block
 *    size, and reserved modifier values have no defined result; the core
 *    falls back to linear arithmetic or plain modulo.
 */
#ifndef HW_COPROC_DSP56300_H
#define HW_COPROC_DSP56300_H

typedef enum DSP56300Space {
    DSP56300_SPACE_X = 0,
    DSP56300_SPACE_Y = 1,
    DSP56300_SPACE_P = 2,
} DSP56300Space;

/*
 * Register numbers: the six-bit register encoding the instruction set uses,
 * so a field of an opcode is also a valid argument. Accumulator parts are
 * 24 bits wide (A2/B2 hold 8 bits, sign-extended on read); A and B read
 * through the shifter/limiter like a move does.
 */
enum {
    DSP56300_REG_X0 = 0x04, DSP56300_REG_X1, DSP56300_REG_Y0, DSP56300_REG_Y1,
    DSP56300_REG_A0 = 0x08, DSP56300_REG_B0, DSP56300_REG_A2, DSP56300_REG_B2,
    DSP56300_REG_A1, DSP56300_REG_B1, DSP56300_REG_A, DSP56300_REG_B,
    DSP56300_REG_R0 = 0x10,     /* R0..R7 */
    DSP56300_REG_N0 = 0x18,     /* N0..N7 */
    DSP56300_REG_M0 = 0x20,     /* M0..M7 */
    DSP56300_REG_EP = 0x2a,
    DSP56300_REG_VBA = 0x30,
    DSP56300_REG_SC = 0x31,
    DSP56300_REG_SZ = 0x38,
    DSP56300_REG_SR = 0x39,
    DSP56300_REG_OMR = 0x3a,
    DSP56300_REG_SP = 0x3b,
    DSP56300_REG_SSH = 0x3c,
    DSP56300_REG_SSL = 0x3d,
    DSP56300_REG_LA = 0x3e,
    DSP56300_REG_LC = 0x3f,
};

/* Status register bits. */
#define DSP56300_SR_C   (1u << 0)
#define DSP56300_SR_V   (1u << 1)
#define DSP56300_SR_Z   (1u << 2)
#define DSP56300_SR_N   (1u << 3)
#define DSP56300_SR_U   (1u << 4)
#define DSP56300_SR_E   (1u << 5)
#define DSP56300_SR_L   (1u << 6)
#define DSP56300_SR_S   (1u << 7)
#define DSP56300_SR_I0  (1u << 8)
#define DSP56300_SR_I1  (1u << 9)
#define DSP56300_SR_S0  (1u << 10)
#define DSP56300_SR_S1  (1u << 11)
#define DSP56300_SR_SC  (1u << 13)
#define DSP56300_SR_DM  (1u << 14)
#define DSP56300_SR_LF  (1u << 15)
#define DSP56300_SR_FV  (1u << 16)
#define DSP56300_SR_SA  (1u << 17)
#define DSP56300_SR_CE  (1u << 19)
#define DSP56300_SR_SM  (1u << 20)
#define DSP56300_SR_RM  (1u << 21)
#define DSP56300_SR_RESET 0xc00300u

/* Interrupt vector addresses of the core's own sources (offsets from VBA). */
#define DSP56300_VEC_STACK_ERROR 0x02
#define DSP56300_VEC_ILLEGAL     0x04
#define DSP56300_VEC_DEBUG       0x06
#define DSP56300_VEC_TRAP        0x08
#define DSP56300_VEC_NMI         0x0a
#define DSP56300_VEC_IRQA        0x10
#define DSP56300_VEC_IRQB        0x12
#define DSP56300_VEC_IRQC        0x14
#define DSP56300_VEC_IRQD        0x16

#define DSP56300_STACK_DEPTH 16

typedef struct DSP56300State DSP56300State;

typedef struct DSP56300Ops {
    /*
     * Memory and peripheral access beyond the fast-path arrays below. The
     * address is 24 bits, the value 24 bits. Both are required.
     */
    uint32_t (*read)(void *opaque, DSP56300Space space, uint32_t addr);
    void (*write)(void *opaque, DSP56300Space space, uint32_t addr,
                  uint32_t val);
    /*
     * Optional: the RESET instruction was executed. It resets the on-chip
     * peripherals and the interrupt priority registers, which the device
     * owns.
     */
    void (*peripheral_reset)(void *opaque);
} DSP56300Ops;

typedef enum DSP56300RunState {
    DSP56300_RUNNING,
    DSP56300_WAIT,      /* WAIT executed: until an unmasked interrupt */
    DSP56300_STOP,      /* STOP executed: until an interrupt or reset */
} DSP56300RunState;

struct DSP56300State {
    /* Programming model. Accumulators hold 56 bits in the low bits. */
    uint64_t acc[2];
    uint32_t x0, x1, y0, y1;
    uint32_t r[8], n[8], m[8];
    uint32_t pc, sr, omr, la, lc, vba, sz, sc, ep, sp;
    uint32_t ss[DSP56300_STACK_DEPTH][2];   /* [0] SSH, [1] SSL */

    uint32_t run_state;                 /* a DSP56300RunState */

    /*
     * Memory. An address below mem_words[space] is served from mem[space]
     * (when non-NULL); everything else goes through ops. The fast path is
     * optional, the callbacks alone are enough.
     */
    uint32_t *mem[3];
    uint32_t mem_words[3];
    DSP56300Ops ops;
    void *opaque;

    /* Cycles consumed since reset, and the last instruction's share. */
    uint64_t cycles;
    unsigned int icycles;

    /* Set by dsp56300_run() when the program is in a tight wait loop. */
    bool idle;

    /* Interrupts: pending flag, level, and level-triggered flag per vector. */
    uint8_t irq_pending[128];
    uint8_t irq_ipl[128];
    uint8_t irq_level[128];
    unsigned int irq_count;
    int irq_best;               /* cached arbitration result */
    unsigned int irq_best_mask; /* SR.I it was computed for */
    bool irq_best_valid;

    /* Execution scratch; not part of the architectural state. */
    uint32_t ipc;               /* address of the instruction being run */
    uint32_t npc;               /* where flow continues after it */
    unsigned int ext;           /* extension words consumed */
    unsigned int flow;          /* FLOW_* flags from the handler */
    bool in_int;                /* executing a vector's instruction */
    bool rep_active;
    bool in_rep;                /* running the repeated instruction */
    bool rep_illegal;           /* it was not allowed in a REP */
    uint32_t rep_count, rep_save, rep_ipc;
    uint64_t wcount;            /* stores since reset, for idle detection */
    uint32_t idle_pc;
    uint64_t idle_hash, idle_wcount;
};

/* ops->read and ops->write are required. */
void dsp56300_init(DSP56300State *s, const DSP56300Ops *ops, void *opaque);

/* Attach a RAM array for the fast path; words must stay valid. */
void dsp56300_set_memory(DSP56300State *s, DSP56300Space space,
                         uint32_t *words, uint32_t nwords);

/*
 * Callbacks may read s->cycles, but it only advances at instruction
 * boundaries: it holds the count as of the end of the previous instruction.
 */

/* Hardware reset: registers to their reset values, execution at boot_pc. */
void dsp56300_reset(DSP56300State *s, uint32_t boot_pc);

/*
 * Run for at least max_cycles, or until the program sits in a tight wait
 * loop or in the WAIT/STOP state (s->idle is then set), or until
 * max_cycles is spent. May overshoot by one instruction. Returns the cycles
 * consumed. When s->idle is set the caller may skip simulated time until
 * something changes the world (an interrupt, or a peripheral value the
 * program is polling) and call again; the core never relies on the hint.
 */
uint64_t dsp56300_run(DSP56300State *s, uint64_t max_cycles);

/*
 * Execute one instruction (or take one interrupt). Returns the cycles it
 * took.
 */
unsigned int dsp56300_step(DSP56300State *s);

/*
 * Interrupts. vector is the byte offset in the table (0x10, 0x12, ... as in
 * the manual); ipl is 0-2 for maskable sources and 3 for non-maskable. An
 * edge-triggered request clears when taken. A level-triggered request stays
 * pending until the device calls dsp56300_irq_lower().
 */
void dsp56300_irq_raise(DSP56300State *s, uint32_t vector, unsigned int ipl);
void dsp56300_irq_raise_level(DSP56300State *s, uint32_t vector,
                              unsigned int ipl);
void dsp56300_irq_lower(DSP56300State *s, uint32_t vector);

/* Register access for devices, tests and the monitor; no side effects. */
uint32_t dsp56300_get_reg(const DSP56300State *s, unsigned int reg);
void dsp56300_set_reg(DSP56300State *s, unsigned int reg, uint32_t val);

/* The 56-bit accumulators as the sign-extended value. */
int64_t dsp56300_get_acc(const DSP56300State *s, unsigned int which);
void dsp56300_set_acc(DSP56300State *s, unsigned int which, int64_t val);

/*
 * Migration. vmstate_dsp56300 carries the architectural state: the
 * accumulators and registers, the system stack, the run state, the cycle
 * count, a REP in progress, and the pending interrupts. It leaves out what
 * the device owns or what can be rebuilt: ops, opaque and the memory
 * pointers (the device wires them and migrates its memories), the idle
 * hint and its detector, the cached interrupt arbitration, and the
 * per-instruction scratch (ipc, npc, ext, flow, icycles, in_int, in_rep,
 * rep_illegal). A migration is taken between instructions, so none of the
 * scratch is live.
 */
extern const struct VMStateDescription vmstate_dsp56300;
#define VMSTATE_DSP56300(_f, _s) \
    VMSTATE_STRUCT(_f, _s, 1, vmstate_dsp56300, DSP56300State)

#endif /* HW_COPROC_DSP56300_H */
