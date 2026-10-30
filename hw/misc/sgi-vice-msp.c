/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Original MSP scalar executor. Contract: o2-vice-msp-scalar.md, SHA-256
 * 3be44dcc92d7a12c9d3116f4837ac406b3c434a32cf0b94abd10ebcc1a37a814.
 * Vector computation and BSP execution have no implementation here.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-vice-msp.h"
#include "qemu/timer.h"
#include "qemu/bitops.h"

#define PC_MASK 0xfffc

enum {
    MSP_ALU,
    MSP_LOAD,
    MSP_STORE,
    MSP_BRANCH,
    MSP_COMMON_READ,
    MSP_COMMON_WRITE,
    MSP_BREAK,
    MSP_RI,
};

typedef struct MspEvent {
    uint8_t bits;
    uint16_t pc;
    uint32_t address;
    bool delay;
} MspEvent;

static bool held(SGIViceMsp *m, const char *reason, uint16_t pc)
{
    m->model_blocked = true;
    m->ops->held(m->opaque, reason, pc);
    return false;
}

static void next_tick(SGIViceMsp *m, bool first)
{
    uint64_t period = clock_get(m->clock);
    uint64_t fraction;

    if (!m->running || m->model_blocked) {
        m->pending = false;
        return;
    }
    if (first) {
        m->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        m->next_fraction = 0;
    }
    fraction = (uint64_t)m->next_fraction + (uint32_t)period;
    m->next_ns += (period >> 32) + (fraction >> 32);
    m->next_fraction = fraction;
    m->pending = true;
}

void sgi_vice_msp_init(SGIViceMsp *m, const SGIViceMspOps *ops,
                       void *opaque, Clock *clock)
{
    m->ops = ops;
    m->opaque = opaque;
    m->clock = clock;
    sgi_vice_msp_reset(m);
}

void sgi_vice_msp_reset(SGIViceMsp *m)
{
    const SGIViceMspOps *ops = m->ops;
    void *opaque = m->opaque;
    Clock *clock = m->clock;

    memset(m, 0, sizeof(*m));
    m->ops = ops;
    m->opaque = opaque;
    m->clock = clock;
    m->reset_held = true;
    m->reset_start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    m->reserved_bank = -1;
}

static bool empty(SGIViceMsp *m)
{
    return !m->ex.valid && !m->ls.valid && !m->wb.valid;
}

static void stop(SGIViceMsp *m)
{
    bool exception = m->fault_drain;

    m->running = false;
    m->draining = false;
    m->fault_drain = false;
    m->pending = false;
    m->fresh_pc = false;
    if (exception) {
        m->ops->exception(m->opaque);
    }
}

bool sgi_vice_msp_host_read(SGIViceMsp *m, unsigned slot, uint32_t *value)
{
    switch (slot) {
    case 0x40:
        *value = m->reset_held ? 0 : 2 | m->running;
        break;
    case 0x48:
        *value = m->flags;
        break;
    case 0x50:
        *value = m->decode_pc;
        break;
    case 0x58:
        *value = m->bad_address;
        break;
    case 0x60:
        *value = m->watch;
        break;
    case 0x68:
        *value = m->epc;
        break;
    case 0x70:
        *value = m->cause;
        break;
    default:
        return false;
    }
    return true;
}

bool sgi_vice_msp_host_write(SGIViceMsp *m, unsigned slot, uint32_t value,
                             uint32_t mask)
{
    int64_t elapsed;

    switch (slot) {
    case 0x40:
        if (value & mask & ~3U) {
            return held(m, "MSP reserved control", m->decode_pc);
        }
        if (!(mask & 3)) {
            return true;
        }
        /* Read-running is state, not an unwritten GO command. */
        value = ((m->reset_held ? 0 : 2 | m->running) & ~mask) |
                (value & mask);
        if (!(value & 2)) {
            if (!m->reset_held) {
                sgi_vice_msp_reset(m);
            }
            return true;
        }
        if (m->reset_held) {
            elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - m->reset_start;
            if (!m->ops->admitted(m->opaque) || elapsed < 0 ||
                ((unsigned __int128)elapsed << 32) <
                (unsigned __int128)16 * clock_get(m->clock)) {
                return held(m, "MSP short or unstable reset release", 0);
            }
            if ((mask & value & 1) && !m->fresh_pc) {
                return held(m, "MSP start requires fresh PC", 0);
            }
            m->reset_held = false;
            m->model_blocked = false;
        }
        if (mask & value & 1) {
            if (!m->ops->admitted(m->opaque) || m->running ||
                m->draining || !m->fresh_pc) {
                return held(m, "MSP start admission", m->decode_pc);
            }
            m->running = true;
            m->fresh_pc = false;
            m->draining = false;
            m->fault_drain = false;
            m->model_blocked = false;
            m->count = 0;
            m->branch_valid = false;
            m->delay_pending = false;
            m->waiting_target = false;
            m->interlock = 0;
            m->last_dest = 0;
            m->previous_dest = 0;
            m->penalty = 0;
            next_tick(m, true);
        } else if ((mask & 1) && m->running) {
            m->draining = true;
            m->fresh_pc = false;
            if (empty(m)) {
                stop(m);
            }
        }
        return true;
    case 0x48:
        mask &= 0xff;
        m->flags = (m->flags & ~mask) | (value & mask);
        return true;
    case 0x50:
        mask &= PC_MASK;
        if (!mask) {
            return true;
        }
        if (m->running || m->draining) {
            return held(m, "MSP active fetch-PC write", m->decode_pc);
        }
        /* Fetch is writable; decode remains the independent read view. */
        m->fetch_pc = (m->fetch_pc & ~mask) | (value & mask);
        m->fresh_pc = true;
        return true;
    case 0x60:
        mask &= ~7U;
        m->watch = (m->watch & ~mask) | (value & mask);
        return true;
    default:
        return held(m, "MSP read-only register write", m->decode_pc);
    }
}

static bool fetchable(uint16_t pc)
{
    return pc >= 0x2000 && pc <= 0x2ffc;
}

static bool fetch(SGIViceMsp *m)
{
    uint16_t pc;
    uint32_t words[2];

    if (m->count == 2 || (m->count && m->fetch_fault[0]) ||
        (m->count && m->delay_pending)) {
        return true;
    }
    pc = m->count ? m->pcs[0] : m->fetch_pc;
    if (!fetchable(pc)) {
        m->pcs[0] = pc;
        m->words[0] = 0;
        m->fetch_fault[0] = true;
        m->count = 1;
        return true;
    }
    if (pc == 0x2ffc) {
        return held(m, "MSP last-word fetch pair", pc);
    }
    if (!m->ops->fetch(m->opaque, pc, words)) {
        return held(m, "MSP instruction store unavailable", pc);
    }
    if (!m->count) {
        m->words[0] = words[0];
        m->pcs[0] = pc;
        m->fetch_fault[0] = false;
    }
    m->words[1] = words[1];
    m->pcs[1] = (pc + 4) & PC_MASK;
    m->fetch_fault[1] = false;
    m->count = 2;
    m->fetch_pc = (pc + 8) & PC_MASK;
    return true;
}

static SGIViceMspIssue classify(uint32_t w)
{
    unsigned op = w >> 26, rs = (w >> 21) & 31, fn = w & 63;

    if (op == 0x12) {
        if (rs >= 16) {
            return VICE_MSP_VECTOR_COMPUTE;
        }
        if (rs == 0 || rs == 2 || rs == 4 || rs == 6) {
            return VICE_MSP_VECTOR_SCALAR;
        }
        return VICE_MSP_UNCLASSIFIED;
    }
    if (op == 0x32 || op == 0x3a) {
        return VICE_MSP_VECTOR_SCALAR;
    }
    if ((op >= 1 && op <= 7) || (!op && (fn == 8 || fn == 9))) {
        return VICE_MSP_BRANCH;
    }
    return VICE_MSP_SCALAR;
}

static bool decode(SGIViceMsp *m, SGIViceMspStage *s, uint32_t w)
{
    unsigned op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
    unsigned rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;

    s->word = w;
    s->kind = MSP_ALU;
    s->dest = rt;
    s->writes = true;
    if (!op) {
        s->dest = rd;
        switch (fn) {
        case 0:
        case 2:
        case 3:
            if (rs) {
                goto noncanonical;
            }
            s->sources = BIT(rt);
            return true;
        case 4:
        case 6:
        case 7:
            if (sa) {
                goto noncanonical;
            }
            s->sources = BIT(rs) | BIT(rt);
            return true;
        case 8:
        case 9:
            if (rt || sa || (fn == 8 && rd)) {
                goto noncanonical;
            }
            if (fn == 9 && rs == rd) {
                return held(m, "MSP JALR source/link alias", s->pc);
            }
            s->kind = MSP_BRANCH;
            s->writes = fn == 9;
            s->sources = BIT(rs);
            return true;
        case 13:
            s->kind = MSP_BREAK;
            s->writes = false;
            return true;
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        case 38:
        case 39:
        case 42:
        case 43:
            if (sa) {
                goto noncanonical;
            }
            s->sources = BIT(rs) | BIT(rt);
            return true;
        default:
            s->kind = MSP_RI;
            s->writes = false;
            return true;
        }
    }
    switch (op) {
    case 1:
        if (rt != 0 && rt != 1 && rt != 16 && rt != 17) {
            goto noncanonical;
        }
        if (rt >= 16 && rs == 31) {
            return held(m, "MSP branch source/link alias", s->pc);
        }
        s->writes = rt >= 16;
        s->dest = 31;
        s->sources = BIT(rs);
        s->kind = MSP_BRANCH;
        return true;
    case 2:
    case 3:
        s->writes = op == 3;
        s->dest = 31;
        s->kind = MSP_BRANCH;
        return true;
    case 4:
    case 5:
    case 6:
    case 7:
        if (op >= 6 && rt) {
            goto noncanonical;
        }
        s->writes = false;
        s->sources = BIT(rs) | (op <= 5 ? BIT(rt) : 0);
        s->kind = MSP_BRANCH;
        return true;
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
        s->sources = BIT(rs);
        return true;
    case 15:
        if (rs) {
            goto noncanonical;
        }
        return true;
    case 0x20:
    case 0x21:
    case 0x23:
    case 0x24:
    case 0x25:
        s->kind = MSP_LOAD;
        s->sources = BIT(rs);
        s->loadlike = true;
        s->size = op == 0x23 ? 4 : (op & 1) ? 2 : 1;
        return true;
    case 0x28:
    case 0x29:
    case 0x2b:
        s->kind = MSP_STORE;
        s->sources = BIT(rs) | BIT(rt);
        s->writes = false;
        s->size = op == 0x2b ? 4 : (op & 1) ? 2 : 1;
        return true;
    case 0x11:
    case 0x13:
        if ((w & 0x7ff) || (rs != 0 && rs != 2 && rs != 4 && rs != 6)) {
            goto noncanonical;
        }
        if (op == 0x11 && rs != 2 && rs != 6) {
            return held(m, "MSP COP1 alias", s->pc);
        }
        s->writes = rs == 0 || rs == 2;
        s->loadlike = s->writes;
        s->sources = s->writes ? 0 : BIT(rt);
        s->kind = s->writes ? MSP_COMMON_READ : MSP_COMMON_WRITE;
        s->common_slot = op == 0x11 ? 0x100 + rd * 8 :
                         ((rs == 2 || rs == 6) ? 0x1100 : 0x1000) + rd * 8;
        return true;
    default:
        s->kind = MSP_RI;
        s->writes = false;
        return true;
    }
noncanonical:
    return held(m, "MSP noncanonical encoding", s->pc);
}

static bool interlocked(SGIViceMsp *m, uint32_t w)
{
    unsigned rs = (w >> 21) & 31, op = w >> 26;
    SGIViceMspStage *producer[] = { &m->ex, &m->ls };

    m->interlock = 0;
    for (unsigned i = 0; i < 2; i++) {
        SGIViceMspStage *s = producer[i];

        if (s->valid && s->loadlike &&
            sgi_vice_msp_interlock_match(w, s->dest)) {
            m->interlock = 2 - i;
            if (!op && ((w & 63) == 8 || (w & 63) == 9)) {
                m->waiting_target = true;
                m->target_reg = rs;
                m->waiting_pc = m->decode_pc;
            }
            return true;
        }
    }
    return false;
}

static bool operand(SGIViceMsp *m, unsigned reg, uint32_t *value)
{
    SGIViceMspStage *writers[] = { &m->ex, &m->ls, &m->wb };

    if (!reg) {
        *value = 0;
        return true;
    }
    for (unsigned i = 0; i < 3; i++) {
        SGIViceMspStage *s = writers[i];

        if (s->valid && s->writes && s->dest == reg) {
            if (!s->result_ready) {
                return false;
            }
            *value = s->result;
            return true;
        }
    }
    *value = m->gpr[reg];
    return true;
}

static bool execute(SGIViceMsp *m, SGIViceMspStage *s)
{
    uint32_t w = s->word, a = s->a, b = s->b, result = 0;
    unsigned op = w >> 26, fn = w & 63, sa = (w >> 6) & 31;
    int32_t imm = (int16_t)w;
    int64_t signed_result = 0;
    bool checked = false, taken = true;

    if (!s->valid || s->ex_done) {
        return true;
    }
    if (s->kind == MSP_ALU) {
        if (!op) {
            switch (fn) {
            case 0:
                result = b << sa;
                break;
            case 2:
                result = b >> sa;
                break;
            case 3:
                result = (int32_t)b >> sa;
                break;
            case 4:
                result = b << (a & 31);
                break;
            case 6:
                result = b >> (a & 31);
                break;
            case 7:
                result = (int32_t)b >> (a & 31);
                break;
            case 32:
                signed_result = (int64_t)(int32_t)a + (int32_t)b;
                checked = true;
                break;
            case 33:
                result = a + b;
                break;
            case 34:
                signed_result = (int64_t)(int32_t)a - (int32_t)b;
                checked = true;
                break;
            case 35:
                result = a - b;
                break;
            case 36:
                result = a & b;
                break;
            case 37:
                result = a | b;
                break;
            case 38:
                result = a ^ b;
                break;
            case 39:
                result = ~(a | b);
                break;
            case 42:
                result = (int32_t)a < (int32_t)b;
                break;
            case 43:
                result = a < b;
                break;
            }
        } else {
            switch (op) {
            case 8:
                signed_result = (int64_t)(int32_t)a + imm;
                checked = true;
                break;
            case 9:
                result = a + imm;
                break;
            case 10:
                result = (int32_t)a < imm;
                break;
            case 11:
                result = a < (uint32_t)imm;
                break;
            case 12:
                result = a & (uint16_t)w;
                break;
            case 13:
                result = a | (uint16_t)w;
                break;
            case 14:
                result = a ^ (uint16_t)w;
                break;
            case 15:
                result = (uint32_t)(uint16_t)w << 16;
                break;
            }
        }
        if (checked) {
            if (signed_result < INT32_MIN || signed_result > INT32_MAX) {
                return held(m, "MSP signed overflow", s->pc);
            }
            result = signed_result;
        }
        s->result = result;
        s->result_ready = true;
    } else if (s->kind == MSP_LOAD || s->kind == MSP_STORE) {
        s->address = a + imm;
    } else if (s->kind == MSP_BRANCH) {
        if (op == 1) {
            taken = ((w >> 16) & 1) ? (int32_t)a >= 0 : (int32_t)a < 0;
        } else if (op == 4) {
            taken = a == b;
        } else if (op == 5) {
            taken = a != b;
        } else if (op == 6) {
            taken = (int32_t)a <= 0;
        } else if (op == 7) {
            taken = (int32_t)a > 0;
        }
        m->branch_resolved = true;
        m->branch_taken = taken;
        s->result = s->pc + 8;
        s->result_ready = s->writes;
    }
    s->ex_done = true;
    return true;
}

static void reserve(SGIViceMsp *m, unsigned bank)
{
    uint64_t period = clock_get(m->clock);
    uint64_t fraction = (uint64_t)m->next_fraction + (uint32_t)period;

    m->reserved_bank = bank;
    m->reservation_start_ns = m->next_ns;
    m->reservation_start_fraction = m->next_fraction;
    m->reservation_end_ns = m->next_ns + (period >> 32) + (fraction >> 32);
    m->reservation_end_fraction = fraction;
}

static bool memory_stage(SGIViceMsp *m, SGIViceMspStage *s)
{
    uint32_t value = 0, address = s->address;
    bool write = s->kind == MSP_STORE;
    unsigned op = s->word >> 26;
    bool valid;

    if (!s->valid || s->ls_done) {
        return true;
    }
    if (s->kind == MSP_COMMON_READ || s->kind == MSP_COMMON_WRITE) {
        value = s->b;
        if (!m->ops->common(m->opaque, s->common_slot, &value,
                             s->kind == MSP_COMMON_WRITE)) {
            return held(m, "MSP common-bus operation", s->pc);
        }
        s->result = value;
        s->result_ready = s->loadlike;
    } else if (s->kind == MSP_LOAD || s->kind == MSP_STORE) {
        if (s->size == 2 && (address & 3) == 2) {
            return held(m, "MSP disputed halfword alignment", s->pc);
        }
        valid = address >= 0x8000 && (uint64_t)address + s->size <= 0x9800 &&
                !(address & (s->size - 1));
        if (m->watch && (address & ~7U) == m->watch) {
            s->faults |= BIT(3);
        }
        if (!valid) {
            s->faults |= BIT(write ? 1 : 0);
        } else if (!(m->ops->grants(m->opaque) &
                     BIT((address - 0x8000) / 0x800))) {
            s->faults |= BIT(6);
        } else {
            reserve(m, (address - 0x8000) / 0x800);
            value = s->b;
            if (!m->ops->local(m->opaque, address, &value, s->size, write)) {
                return held(m, "MSP local memory transport", s->pc);
            }
        }
        if (!write) {
            if (s->faults & (BIT(0) | BIT(6))) {
                value = 0;
            } else if (op == 0x20) {
                value = (int8_t)value;
            } else if (op == 0x21) {
                value = (int16_t)value;
            }
            s->result = value;
            s->result_ready = true;
        }
    }
    s->ls_done = true;
    return true;
}

static void events(SGIViceMsp *m, MspEvent wb, MspEvent rd)
{
    static const uint8_t priority[] = { 7, 2, 4, 5, 6, 0, 1, 3 };
    uint8_t bits = wb.bits | rd.bits;

    if (!bits) {
        return;
    }
    if (!m->flags) {
        for (unsigned i = 0; i < G_N_ELEMENTS(priority); i++) {
            unsigned code = priority[i];
            MspEvent *chosen = rd.bits & BIT(code) ? &rd : &wb;

            if (bits & BIT(code)) {
                m->epc = chosen->pc;
                m->cause = code << 2 | (chosen->delay ? BIT(31) : 0);
                if (code <= 1) {
                    m->bad_address = chosen->address;
                }
                break;
            }
        }
    }
    m->flags |= bits;
    m->draining = true;
    m->fault_drain = true;
    m->fresh_pc = false;
}

void sgi_vice_msp_tick(SGIViceMsp *m)
{
    SGIViceMspStage next = { 0 };
    MspEvent wb_event = { 0 }, rd_event = { 0 };
    bool admitted = false, blocked = false;

    m->pending = false;
    if (!m->running || m->model_blocked) {
        return;
    }
    m->cycles++;
    if (m->wb.valid && !m->wb.wb_done) {
        if (m->wb.writes && m->wb.dest) {
            m->gpr[m->wb.dest] = m->wb.result;
        }
        wb_event = (MspEvent) { m->wb.faults, m->wb.pc,
                               m->wb.address, m->wb.delay };
        m->wb.wb_done = true;
    }
    if (!memory_stage(m, &m->ls) || !execute(m, &m->ex)) {
        events(m, wb_event, rd_event);
        return;
    }
    if (!m->draining && m->count) {
        uint16_t pc = m->pcs[0];
        bool delay = m->delay_pending && pc == m->delay_pc;
        SGIViceMspIssue first = classify(m->words[0]);
        SGIViceMspIssue second = m->count > 1 ? classify(m->words[1]) :
                                VICE_MSP_SCALAR;
        unsigned group = m->count > 1 ?
                         sgi_vice_msp_group(first, second, delay) : 1;

        m->decode_pc = pc;
        next.pc = pc;
        next.delay = delay;
        next.valid = true;
        if (m->fetch_fault[0]) {
            rd_event = (MspEvent) { BIT(7), pc, 0, delay };
        } else if (first == VICE_MSP_VECTOR_COMPUTE ||
                   first == VICE_MSP_VECTOR_SCALAR ||
                   first == VICE_MSP_UNCLASSIFIED ||
                   (group == 2 && second == VICE_MSP_VECTOR_COMPUTE)) {
            held(m, "MSP disconnected vector group", pc);
            blocked = true;
        } else if (!decode(m, &next, m->words[0])) {
            blocked = true;
        } else if (delay && (next.kind == MSP_BRANCH ||
                            next.kind == MSP_BREAK ||
                    (next.sources & (BIT(m->last_dest) |
                                     BIT(m->previous_dest)) & ~1U))) {
            held(m, "MSP prohibited branch-delay operation or dependency", pc);
            blocked = true;
        } else if (next.kind == MSP_RI) {
            rd_event = (MspEvent) { BIT(4), pc, 0, delay };
        } else if (!interlocked(m, next.word)) {
            unsigned rs = (next.word >> 21) & 31;
            unsigned rt = (next.word >> 16) & 31;
            unsigned op = next.word >> 26;
            bool ready = (!(next.sources & BIT(rs)) ||
                          operand(m, rs, &next.a)) &&
                         (!(next.sources & BIT(rt)) ||
                          operand(m, rt, &next.b));

            m->waiting_target = false;
            if (!ready) {
                m->waiting_target = next.kind == MSP_BRANCH && !op;
                m->target_reg = rs;
                m->waiting_pc = pc;
            } else if (next.kind == MSP_BREAK) {
                rd_event = (MspEvent) { BIT(2), pc, 0, delay };
            } else {
                if (next.kind == MSP_BRANCH) {
                    if (!op) {
                        next.target = next.a & PC_MASK;
                    } else if (op == 2 || op == 3) {
                        next.target = (((pc + 4) & 0xf0000000) |
                                      ((next.word & 0x3ffffff) << 2)) & PC_MASK;
                    } else {
                        next.target = (pc + 4 + (int16_t)next.word * 4) &
                                      PC_MASK;
                    }
                    if (op != 0 && op != 2 && op != 3 &&
                        (!fetchable(next.target) || next.target == 0x2ffc)) {
                        held(m, "MSP speculative conditional fetch", pc);
                        blocked = true;
                    }
                }
                if (!blocked) {
                    admitted = true;
                }
            }
        }
    }
    events(m, wb_event, rd_event);
    m->wb = m->ls;
    m->ls = m->ex;
    memset(&m->ex, 0, sizeof(m->ex));
    if (admitted && !m->draining && !blocked) {
        m->ex = next;
        m->previous_dest = m->last_dest;
        m->last_dest = next.writes ? next.dest : 0;
        if (next.kind == MSP_BRANCH) {
            m->branch_valid = true;
            m->branch_resolved = false;
            m->branch_pc = next.pc;
            m->branch_target = next.target;
            m->branch_sequential = (next.pc + 8) & PC_MASK;
            m->delay_pending = true;
            m->delay_pc = (next.pc + 4) & PC_MASK;
        }
        m->count--;
        if (m->count) {
            m->words[0] = m->words[1];
            m->pcs[0] = m->pcs[1];
            m->fetch_fault[0] = m->fetch_fault[1];
        }
        if (next.delay) {
            m->count = 0;
            m->fetch_pc = m->branch_taken ? m->branch_target :
                          m->branch_sequential;
            m->penalty = m->branch_taken ? 0 : 1;
            m->delay_pending = false;
            m->branch_valid = false;
        }
    }
    m->gpr[0] = 0;
    if (m->model_blocked) {
        return;
    }
    if (m->draining) {
        if (empty(m)) {
            stop(m);
        }
    } else if (m->penalty) {
        m->penalty--;
    } else if (!fetch(m)) {
        return;
    }
    next_tick(m, false);
}

bool sgi_vice_msp_reserved(SGIViceMsp *m, unsigned bank,
                           int64_t ns, uint32_t fraction)
{
    return m->reserved_bank == bank &&
        (ns > m->reservation_start_ns ||
         (ns == m->reservation_start_ns &&
          fraction >= m->reservation_start_fraction)) &&
        (ns < m->reservation_end_ns ||
         (ns == m->reservation_end_ns &&
          fraction < m->reservation_end_fraction));
}

bool sgi_vice_msp_rebase(SGIViceMsp *m, int64_t delta)
{
    SGIViceMspStage *stages[] = { &m->ex, &m->ls, &m->wb };

    if (m->count > 2 || m->last_dest >= 32 || m->previous_dest >= 32 ||
        m->target_reg >= 32 || m->reserved_bank < -1 || m->reserved_bank > 2 ||
        m->interlock > 2 || m->gpr[0]) {
        return false;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(stages); i++) {
        SGIViceMspStage *s = stages[i];

        if (s->valid && (s->dest >= 32 || s->kind > MSP_RI ||
            ((s->kind == MSP_LOAD || s->kind == MSP_STORE) &&
             s->size != 1 && s->size != 2 && s->size != 4))) {
            return false;
        }
    }
    m->reset_start += delta;
    m->next_ns += delta;
    m->reservation_start_ns += delta;
    m->reservation_end_ns += delta;
    return true;
}

static const VMStateDescription vmstate_msp_stage = {
    .name = "sgi-vice-msp/stage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(word, SGIViceMspStage),
        VMSTATE_UINT32(a, SGIViceMspStage),
        VMSTATE_UINT32(b, SGIViceMspStage),
        VMSTATE_UINT32(result, SGIViceMspStage),
        VMSTATE_UINT32(address, SGIViceMspStage),
        VMSTATE_UINT32(sources, SGIViceMspStage),
        VMSTATE_UINT16(pc, SGIViceMspStage),
        VMSTATE_UINT16(target, SGIViceMspStage),
        VMSTATE_UINT16(common_slot, SGIViceMspStage),
        VMSTATE_UINT8(kind, SGIViceMspStage),
        VMSTATE_UINT8(dest, SGIViceMspStage),
        VMSTATE_UINT8(size, SGIViceMspStage),
        VMSTATE_UINT8(faults, SGIViceMspStage),
        VMSTATE_BOOL(valid, SGIViceMspStage),
        VMSTATE_BOOL(delay, SGIViceMspStage),
        VMSTATE_BOOL(writes, SGIViceMspStage),
        VMSTATE_BOOL(loadlike, SGIViceMspStage),
        VMSTATE_BOOL(result_ready, SGIViceMspStage),
        VMSTATE_BOOL(ex_done, SGIViceMspStage),
        VMSTATE_BOOL(ls_done, SGIViceMspStage),
        VMSTATE_BOOL(wb_done, SGIViceMspStage),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_sgi_vice_msp = {
    .name = "sgi-vice-msp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gpr, SGIViceMsp, 32),
        VMSTATE_UINT32_ARRAY(words, SGIViceMsp, 2),
        VMSTATE_UINT16_ARRAY(pcs, SGIViceMsp, 2),
        VMSTATE_BOOL_ARRAY(fetch_fault, SGIViceMsp, 2),
        VMSTATE_UINT8(count, SGIViceMsp),
        VMSTATE_UINT16(fetch_pc, SGIViceMsp),
        VMSTATE_UINT16(decode_pc, SGIViceMsp),
        VMSTATE_UINT32(bad_address, SGIViceMsp),
        VMSTATE_UINT32(watch, SGIViceMsp),
        VMSTATE_UINT16(epc, SGIViceMsp),
        VMSTATE_UINT32(cause, SGIViceMsp),
        VMSTATE_UINT8(flags, SGIViceMsp),
        VMSTATE_BOOL(reset_held, SGIViceMsp),
        VMSTATE_INT64(reset_start, SGIViceMsp),
        VMSTATE_BOOL(fresh_pc, SGIViceMsp),
        VMSTATE_BOOL(running, SGIViceMsp),
        VMSTATE_BOOL(draining, SGIViceMsp),
        VMSTATE_BOOL(fault_drain, SGIViceMsp),
        VMSTATE_BOOL(model_blocked, SGIViceMsp),
        VMSTATE_BOOL(pending, SGIViceMsp),
        VMSTATE_INT64(next_ns, SGIViceMsp),
        VMSTATE_UINT32(next_fraction, SGIViceMsp),
        VMSTATE_UINT64(cycles, SGIViceMsp),
        VMSTATE_STRUCT(ex, SGIViceMsp, 0, vmstate_msp_stage, SGIViceMspStage),
        VMSTATE_STRUCT(ls, SGIViceMsp, 0, vmstate_msp_stage, SGIViceMspStage),
        VMSTATE_STRUCT(wb, SGIViceMsp, 0, vmstate_msp_stage, SGIViceMspStage),
        VMSTATE_UINT8(last_dest, SGIViceMsp),
        VMSTATE_UINT8(previous_dest, SGIViceMsp),
        VMSTATE_UINT8(interlock, SGIViceMsp),
        VMSTATE_BOOL(waiting_target, SGIViceMsp),
        VMSTATE_UINT8(target_reg, SGIViceMsp),
        VMSTATE_UINT16(waiting_pc, SGIViceMsp),
        VMSTATE_BOOL(branch_valid, SGIViceMsp),
        VMSTATE_BOOL(branch_resolved, SGIViceMsp),
        VMSTATE_BOOL(branch_taken, SGIViceMsp),
        VMSTATE_UINT16(branch_pc, SGIViceMsp),
        VMSTATE_UINT16(branch_target, SGIViceMsp),
        VMSTATE_UINT16(branch_sequential, SGIViceMsp),
        VMSTATE_BOOL(delay_pending, SGIViceMsp),
        VMSTATE_UINT16(delay_pc, SGIViceMsp),
        VMSTATE_UINT8(penalty, SGIViceMsp),
        VMSTATE_INT8(reserved_bank, SGIViceMsp),
        VMSTATE_INT64(reservation_start_ns, SGIViceMsp),
        VMSTATE_UINT32(reservation_start_fraction, SGIViceMsp),
        VMSTATE_INT64(reservation_end_ns, SGIViceMsp),
        VMSTATE_UINT32(reservation_end_fraction, SGIViceMsp),
        VMSTATE_END_OF_LIST()
    },
};
