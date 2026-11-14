/*
 * Motorola MCF5206e system integration module and on-chip peripherals.
 *
 * Models the parts of the chip's register block that a board needs to run
 * code from an external boot flash: the module and SRAM base registers,
 * the global and general chip selects, the interrupt controller, the two
 * timers, the software watchdog and the two on-chip UARTs. Every other
 * register is accepted and logged as unimplemented; none of them returns
 * invented state.
 *
 * Register meanings follow the MCF5206e User's Manual, revision 1, chapters
 * 8, 9 and 14 and appendix A. The page of the manual that describes the
 * board's use of the model lists what each choice rests on.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/m68k/mcf.h"
#include "hw/m68k/mcf5206e.h"
#include "system/address-spaces.h"
#include "system/runstate.h"

#define MBAR_V              0x1
#define RAMBAR_V            0x1

#define SYPCR_SWE           0x80
#define SYPCR_SWRI          0x40
#define SYPCR_SWP           0x20
#define SYPCR_SWT_SHIFT     3
#define SYPCR_SWT_MASK      0x18

#define PAR_ENCODED_IPL     0x0040

#define TMR_RST             0x0001
#define TMR_CLK_SHIFT       1
#define TMR_FRR             0x0008
#define TMR_ORI             0x0010
#define TMR_CE_MASK         0x00c0
#define TER_CAP             0x01
#define TER_REF             0x02

#define CSCR_RD             0x0001
#define CSCR_WR             0x0002
/* CSCR0 after reset with a 16-bit boot port and automatic acknowledge. */
#define CSCR0_RESET         0x3d9f

#define RSR_HRST            0x80
#define RSR_SWTR            0x20

static const uint8_t icr_reset[16] = {
    0, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x1c, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00,
};

/* A window that answers every access by logging it. */
static uint64_t unimp_window_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: read%u at 0x%" HWADDR_PRIx
                  ": not modelled, reads as 0\n",
                  (const char *)opaque, size * 8, addr);
    return 0;
}

static void unimp_window_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: write%u at 0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 ": not modelled, ignored\n",
                  (const char *)opaque, size * 8, addr, val);
}

static const MemoryRegionOps unimp_window_ops = {
    .read = unimp_window_read,
    .write = unimp_window_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* Global chip select: the boot flash answers every address. */
static uint64_t global_cs0_read(void *opaque, hwaddr addr, unsigned size)
{
    MCF5206ESIMState *s = opaque;
    uint64_t len = memory_region_size(s->boot_rom);
    uint8_t *p = memory_region_get_ram_ptr(s->boot_rom);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | p[(addr + i) & (len - 1)];
    }
    return val;
}

static void global_cs0_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "mcf5206e global CS0: write%u at 0x%"
                  HWADDR_PRIx " = 0x%" PRIx64 ": flash commands are not "
                  "modelled, ignored\n", size * 8, addr, val);
}

static const MemoryRegionOps global_cs0_ops = {
    .read = global_cs0_read,
    .write = global_cs0_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* Interrupt controller. */

static uint16_t sim_ipr(MCF5206ESIMState *s)
{
    uint16_t ipr = s->src_state & 0xff00;
    int n;

    if (s->par & PAR_ENCODED_IPL) {
        /* The board's encoder presents the highest asserted level. */
        for (n = 7; n >= 1; n--) {
            if (s->ext_in & (1 << n)) {
                ipr |= 1 << n;
                break;
            }
        }
    } else {
        ipr |= s->ext_in & ((1 << 1) | (1 << 4) | (1 << 7));
    }
    return ipr;
}

static int sim_src_level(MCF5206ESIMState *s, int src)
{
    if (src <= 7) {
        return src;
    }
    if (src == MCF5206E_SRC_SWT) {
        return 7;
    }
    return (s->icr[src] >> 2) & 7;
}

static uint8_t sim_src_vector(MCF5206ESIMState *s, int src, int level)
{
    bool avec = s->icr[src] & 0x80;

    if (src == MCF5206E_SRC_SWT) {
        return s->swivr;
    }
    if (src >= MCF5206E_SRC_TIMER1 && src <= MCF5206E_SRC_MBUS) {
        /* The manual fixes AVEC to 1 for the timers and the M-Bus. */
        avec = true;
    }
    if (src <= 7 && !avec) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e: external interrupt %d wants "
                      "an acknowledge cycle (AVEC clear): not modelled, "
                      "autovector used\n", src);
        avec = true;
    }
    if (avec) {
        return 24 + level;
    }
    if (src == MCF5206E_SRC_UART1 || src == MCF5206E_SRC_UART2) {
        return s->uivr[src - MCF5206E_SRC_UART1];
    }
    qemu_log_mask(LOG_UNIMP, "mcf5206e: no vector source for interrupt %d\n",
                  src);
    return 0x0f;
}

static bool sim_pending(MCF5206ESIMState *s, int *level, int *vector)
{
    uint16_t active = sim_ipr(s) & ~s->imr;
    int best_level = 0, best_prio = -1, best_src = 0;
    int src;

    for (src = 1; src < 16; src++) {
        int lvl, prio;

        if (!(active & (1 << src))) {
            continue;
        }
        lvl = sim_src_level(s, src);
        prio = s->icr[src] & 3;
        if (lvl > best_level || (lvl == best_level && prio > best_prio)) {
            best_level = lvl;
            best_prio = prio;
            best_src = src;
        }
    }
    if (!best_level) {
        return false;
    }
    *level = best_level;
    *vector = sim_src_vector(s, best_src, best_level);
    return true;
}

static void sim_update_irq(MCF5206ESIMState *s)
{
    int level, vector;

    if (sim_pending(s, &level, &vector)) {
        m68k_set_irq_level(s->cpu, level, vector);
    } else {
        m68k_set_irq_level(s->cpu, 0, 0);
    }
}

static void sim_set_src(MCF5206ESIMState *s, int src, bool level)
{
    if (level) {
        s->src_state |= 1 << src;
    } else {
        s->src_state &= ~(1 << src);
    }
    sim_update_irq(s);
}

static void sim_internal_irq(void *opaque, int n, int level)
{
    sim_set_src(opaque, n, level);
}

static void sim_ext_irq(void *opaque, int n, int level)
{
    MCF5206ESIMState *s = opaque;

    if (level) {
        s->ext_in |= 1 << n;
    } else {
        s->ext_in &= ~(1 << n);
    }
    sim_update_irq(s);
}

static bool sim_board_irq_select(void *opaque, int *level, int *vector)
{
    return sim_pending(opaque, level, vector);
}

static void sim_board_irq_accept(void *opaque)
{
    /* The sources are level sensitive and their devices clear them. */
    sim_update_irq(opaque);
}

/* Timers. */

static unsigned timer_div(MCF5206ETimer *t)
{
    switch ((t->tmr >> TMR_CLK_SHIFT) & 3) {
    case 1:
        return 1;
    case 2:
        return 16;
    default:
        return 0;
    }
}

static uint64_t timer_ticks_to_ns(MCF5206ETimer *t, uint64_t ticks)
{
    uint64_t cycles = ticks * timer_div(t) * (((t->tmr >> 8) & 0xff) + 1);

    return muldiv64(cycles, NANOSECONDS_PER_SECOND, t->sim->clock_hz);
}

static uint64_t timer_ns_to_ticks(MCF5206ETimer *t, int64_t ns)
{
    uint64_t cycles = muldiv64(ns, t->sim->clock_hz, NANOSECONDS_PER_SECOND);

    return cycles / (timer_div(t) * (((t->tmr >> 8) & 0xff) + 1));
}

static uint16_t timer_count(MCF5206ETimer *t)
{
    uint64_t ticks, span;

    if (!t->running) {
        return t->tcn_base;
    }
    ticks = t->tcn_base + timer_ns_to_ticks(t,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns);
    span = (t->tmr & TMR_FRR) ? (uint64_t)t->trr + 1 : 0x10000;
    return ticks % span;
}

static void timer_update_irq(MCF5206ETimer *t)
{
    bool on = ((t->tmr & TMR_ORI) && (t->ter & TER_REF)) ||
              ((t->tmr & TMR_CE_MASK) && (t->ter & TER_CAP));

    sim_set_src(t->sim, t->src, on);
}

/*
 * Reference events happen every (TRR + 1) ticks in restart mode and every
 * 65536 ticks when free running. Only a timer that interrupts on the
 * reference is scheduled; for the others the event flag is brought up to
 * date when a register is read, so a timer that runs one event per few
 * system clocks costs nothing.
 */
static uint64_t timer_period_ticks(MCF5206ETimer *t)
{
    return (t->tmr & TMR_FRR) ? (uint64_t)t->trr + 1 : 0x10000;
}

static void timer_catch_up(MCF5206ETimer *t)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t done;

    /* A timer that interrupts is scheduled; its event is raised there. */
    if (!t->running || (t->tmr & TMR_ORI)) {
        return;
    }
    done = t->tcn_base + timer_ns_to_ticks(t, now - t->start_ns);
    if (done >= timer_period_ticks(t) ||
        (t->tcn_base <= t->trr && done > t->trr)) {
        t->ter |= TER_REF;
        /* Count on from the present value so the next event is a new one. */
        t->tcn_base = timer_count(t);
        t->start_ns = now;
    }
}

static void timer_rearm(MCF5206ETimer *t)
{
    uint64_t first;

    timer_del(t->timer);
    if (!t->running || !(t->tmr & TMR_ORI)) {
        return;
    }
    if (t->tcn_base <= t->trr) {
        first = (uint64_t)t->trr + 1 - t->tcn_base;
    } else {
        first = 0x10000 - t->tcn_base + (uint64_t)t->trr + 1;
    }
    t->next_ns = t->start_ns + timer_ticks_to_ns(t, first);
    timer_mod_ns(t->timer, t->next_ns);
}

/* Restart the count from the given value under the current mode. */
static void timer_restart(MCF5206ETimer *t, uint16_t count)
{
    t->tcn_base = count;
    t->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    t->running = (t->tmr & TMR_RST) && timer_div(t);
    if ((t->tmr & TMR_RST) && ((t->tmr >> TMR_CLK_SHIFT) & 3) == 3) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e timer: TIN pin clock source "
                      "not modelled, timer stopped\n");
    }
    timer_rearm(t);
}

static void timer_fire(void *opaque)
{
    MCF5206ETimer *t = opaque;

    t->ter |= TER_REF;
    /* Count on from the event, at its nominal time, not the late wakeup. */
    t->start_ns = t->next_ns;
    t->tcn_base = (t->tmr & TMR_FRR) ? 0 : t->trr;
    t->next_ns = t->start_ns +
                 timer_ticks_to_ns(t, timer_period_ticks(t));
    timer_mod_ns(t->timer, t->next_ns);
    timer_update_irq(t);
}

static void timer_reset(MCF5206ETimer *t)
{
    timer_del(t->timer);
    t->tmr = 0;
    t->trr = 0xffff;
    t->tcr = 0;
    t->ter = 0;
    t->tcn_base = 0;
    t->running = false;
    timer_update_irq(t);
}

static uint32_t timer_read(MCF5206ETimer *t, unsigned off)
{
    timer_catch_up(t);
    switch (off) {
    case 0x00:
        return t->tmr;
    case 0x04:
        return t->trr;
    case 0x08:
        return t->tcr;
    case 0x0c:
        return timer_count(t);
    case 0x11:
        return t->ter;
    }
    return 0;
}

static void timer_write(MCF5206ETimer *t, unsigned off, uint32_t val)
{
    uint16_t count;

    timer_catch_up(t);
    count = timer_count(t);
    switch (off) {
    case 0x00:
        if ((t->tmr & TMR_RST) && !(val & TMR_RST)) {
            timer_reset(t);
            count = 0;
        }
        t->tmr = val;
        timer_restart(t, count);
        break;
    case 0x04:
        t->trr = val;
        timer_restart(t, count);
        break;
    case 0x0c:
        timer_restart(t, 0);
        break;
    case 0x11:
        t->ter &= ~val;
        break;
    default:
        /* TCR is read-only */
        break;
    }
    timer_update_irq(t);
}

/* Software watchdog. */

static uint64_t wdt_timeout_ns(MCF5206ESIMState *s)
{
    static const int shifts[2][4] = { { 9, 11, 13, 15 }, { 18, 20, 22, 24 } };
    int swp = !!(s->sypcr & SYPCR_SWP);
    int swt = (s->sypcr & SYPCR_SWT_MASK) >> SYPCR_SWT_SHIFT;

    return muldiv64(1ULL << shifts[swp][swt], NANOSECONDS_PER_SECOND,
                    s->clock_hz);
}

static void wdt_restart(MCF5206ESIMState *s)
{
    if (s->sypcr & SYPCR_SWE) {
        timer_mod_ns(s->wdt, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     wdt_timeout_ns(s));
    } else {
        timer_del(s->wdt);
    }
}

static void wdt_expired(void *opaque)
{
    MCF5206ESIMState *s = opaque;

    if (s->sypcr & SYPCR_SWRI) {
        s->wdt_reset_cause = true;
        qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: software watchdog "
                      "timeout, resetting the chip\n");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        sim_set_src(s, MCF5206E_SRC_SWT, true);
    }
}

static void wdt_service(MCF5206ESIMState *s, uint8_t val)
{
    if (val == 0x55) {
        s->swsr_seq = 1;
    } else if (val == 0xaa && s->swsr_seq == 1) {
        s->swsr_seq = 0;
        sim_set_src(s, MCF5206E_SRC_SWT, false);
        wdt_restart(s);
    } else {
        s->swsr_seq = 0;
    }
}

/* Chip selects and base registers. */

static void sim_cs_update(MCF5206ESIMState *s, int n)
{
    MemoryRegion *mr = &s->cs_window[n];
    uint32_t bam = s->csmr[n] >> 16;
    bool enabled = s->csmr_written[n] && (s->cscr[n] & (CSCR_RD | CSCR_WR));
    uint64_t size = ((uint64_t)bam + 1) << 16;
    hwaddr base = (hwaddr)(s->csar[n] & ~bam & 0xffff) << 16;
    int i;

    if (enabled && (bam & (bam + 1))) {
        qemu_log_mask(LOG_UNIMP, "mcf5206e: CS%d mask 0x%04x is not a "
                      "contiguous block mask: not modelled, select off\n",
                      n, bam);
        enabled = false;
    }

    memory_region_transaction_begin();
    memory_region_set_enabled(mr, enabled);
    if (enabled) {
        memory_region_set_size(mr, size);
        memory_region_set_address(mr, base);
    }
    if (n == 0) {
        /* The boot flash repeats through its window a megabyte at a time. */
        uint64_t len = memory_region_size(s->boot_rom);

        for (i = 0; i < 16; i++) {
            memory_region_set_enabled(s->rom_alias[i],
                                      (uint64_t)(i + 1) * len <= size);
        }
    }
    memory_region_transaction_commit();
}

static void sim_global_cs0_off(MCF5206ESIMState *s)
{
    int i;

    memory_region_transaction_begin();
    memory_region_set_enabled(&s->global_io, false);
    for (i = 0; i < 3; i++) {
        memory_region_set_enabled(&s->global_alias[i], false);
    }
    memory_region_transaction_commit();
    s->global_cs0 = false;
}

static void sim_map(MCF5206ESIMState *s, MemoryRegion *mr, bool valid,
                    hwaddr base)
{
    memory_region_transaction_begin();
    memory_region_set_enabled(mr, valid);
    if (valid) {
        memory_region_set_address(mr, base);
    }
    memory_region_transaction_commit();
}

static void sim_control_write(void *opaque, uint32_t reg, uint32_t val)
{
    MCF5206ESIMState *s = opaque;

    /* The core takes the BQL around control_write. */
    switch (reg) {
    case M68K_CR_RAMBAR0:
        s->rambar_valid = val & RAMBAR_V;
        sim_map(s, &s->sram, s->rambar_valid, val & 0xffffe000);
        break;
    case M68K_CR_MBAR:
        s->mbar_valid = val & MBAR_V;
        sim_map(s, &s->mbar_io, s->mbar_valid, val & 0xfffffc00);
        break;
    default:
        break;
    }
}

/* Register file. */

enum {
    R_NONE, R_SIMR, R_MARB, R_ICR, R_IMR, R_IPR, R_RSR, R_SYPCR, R_SWIVR,
    R_SWSR, R_CSAR, R_CSMR, R_CSCR, R_DMCR, R_PAR, R_TIMER, R_UART,
    R_UIVR, R_UIP, R_UOP1, R_UOP0, R_MBUS, R_UNIMP,
};

typedef struct RegSpan {
    int kind;
    unsigned start;
    unsigned width;
    unsigned index;
    const char *name;
} RegSpan;

/* Find the register that holds @off, or a span of kind R_NONE. */
static RegSpan sim_reg_at(unsigned off)
{
    RegSpan r = { R_NONE, off, 1, 0, "reserved" };
    unsigned n;

    switch (off) {
    case 0x03:
        return (RegSpan){ R_SIMR, 0x03, 1, 0, "SIMR" };
    case 0x07:
        return (RegSpan){ R_MARB, 0x07, 1, 0, "MARB" };
    case 0x14 ... 0x22:
        return (RegSpan){ R_ICR, off, 1, off - 0x13, "ICR" };
    case 0x36 ... 0x37:
        return (RegSpan){ R_IMR, 0x36, 2, 0, "IMR" };
    case 0x3a ... 0x3b:
        return (RegSpan){ R_IPR, 0x3a, 2, 0, "IPR" };
    case 0x40:
        return (RegSpan){ R_RSR, 0x40, 1, 0, "RSR" };
    case 0x41:
        return (RegSpan){ R_SYPCR, 0x41, 1, 0, "SYPCR" };
    case 0x42:
        return (RegSpan){ R_SWIVR, 0x42, 1, 0, "SWIVR" };
    case 0x43:
        return (RegSpan){ R_SWSR, 0x43, 1, 0, "SWSR" };
    case 0x46 ... 0x47:
        return (RegSpan){ R_UNIMP, 0x46, 2, 0, "DCRR" };
    case 0x4a ... 0x4b:
        return (RegSpan){ R_UNIMP, 0x4a, 2, 0, "DCTR" };
    case 0x4c ... 0x4d:
        return (RegSpan){ R_UNIMP, 0x4c, 2, 0, "DCAR0" };
    case 0x50 ... 0x53:
        return (RegSpan){ R_UNIMP, 0x50, 4, 0, "DCMR0" };
    case 0x57:
        return (RegSpan){ R_UNIMP, 0x57, 1, 0, "DCCR0" };
    case 0x58 ... 0x59:
        return (RegSpan){ R_UNIMP, 0x58, 2, 0, "DCAR1" };
    case 0x5c ... 0x5f:
        return (RegSpan){ R_UNIMP, 0x5c, 4, 0, "DCMR1" };
    case 0x63:
        return (RegSpan){ R_UNIMP, 0x63, 1, 0, "DCCR1" };
    case 0xc6 ... 0xc7:
        return (RegSpan){ R_DMCR, 0xc6, 2, 0, "DMCR" };
    case 0xca ... 0xcb:
        return (RegSpan){ R_PAR, 0xca, 2, 0, "PAR" };
    case 0x1c5:
        return (RegSpan){ R_UNIMP, 0x1c5, 1, 0, "PPDDR" };
    case 0x1c9:
        return (RegSpan){ R_UNIMP, 0x1c9, 1, 0, "PPDAT" };
    case 0x170:
        return (RegSpan){ R_UIVR, 0x170, 1, 0, "UIVR1" };
    case 0x174:
        return (RegSpan){ R_UIP, 0x174, 1, 0, "UIP1" };
    case 0x178:
        return (RegSpan){ R_UOP1, 0x178, 1, 0, "UOP1(1)" };
    case 0x17c:
        return (RegSpan){ R_UOP0, 0x17c, 1, 0, "UOP0(1)" };
    case 0x1b0:
        return (RegSpan){ R_UIVR, 0x1b0, 1, 1, "UIVR2" };
    case 0x1b4:
        return (RegSpan){ R_UIP, 0x1b4, 1, 1, "UIP2" };
    case 0x1b8:
        return (RegSpan){ R_UOP1, 0x1b8, 1, 1, "UOP1(2)" };
    case 0x1bc:
        return (RegSpan){ R_UOP0, 0x1bc, 1, 1, "UOP0(2)" };
    case 0x1e0:
        return (RegSpan){ R_MBUS, 0x1e0, 1, 0, "MADR" };
    case 0x1e4:
        return (RegSpan){ R_MBUS, 0x1e4, 1, 0, "MFDR" };
    case 0x1e8:
        return (RegSpan){ R_MBUS, 0x1e8, 1, 0, "MBCR" };
    case 0x1ec:
        return (RegSpan){ R_MBUS, 0x1ec, 1, 0, "MBSR" };
    case 0x1f0:
        return (RegSpan){ R_MBUS, 0x1f0, 1, 0, "MBDR" };
    case 0x200 ... 0x25f:
        return (RegSpan){ R_UNIMP, off & ~3u, 4, 0, "DMA" };
    }

    if (off >= 0x64 && off < 0x64 + 12 * MCF5206E_NUM_CS) {
        n = (off - 0x64) / 12;
        switch ((off - 0x64) % 12) {
        case 0 ... 1:
            return (RegSpan){ R_CSAR, 0x64 + 12 * n, 2, n, "CSAR" };
        case 4 ... 7:
            return (RegSpan){ R_CSMR, 0x68 + 12 * n, 4, n, "CSMR" };
        case 10 ... 11:
            return (RegSpan){ R_CSCR, 0x6e + 12 * n, 2, n, "CSCR" };
        }
        return r;
    }
    if (off >= 0x100 && off < 0x140) {
        n = (off - 0x100) / 0x20;
        switch ((off - 0x100) % 0x20) {
        case 0 ... 1:
            return (RegSpan){ R_TIMER, 0x100 + 0x20 * n, 2, n, "TMR" };
        case 4 ... 5:
            return (RegSpan){ R_TIMER, 0x104 + 0x20 * n, 2, n, "TRR" };
        case 8 ... 9:
            return (RegSpan){ R_TIMER, 0x108 + 0x20 * n, 2, n, "TCR" };
        case 0xc ... 0xd:
            return (RegSpan){ R_TIMER, 0x10c + 0x20 * n, 2, n, "TCN" };
        case 0x11:
            return (RegSpan){ R_TIMER, 0x111 + 0x20 * n, 1, n, "TER" };
        }
        return r;
    }
    if ((off >= 0x140 && off < 0x160) || (off >= 0x180 && off < 0x1a0)) {
        if (!(off & 3)) {
            return (RegSpan){ R_UART, off, 1, off >= 0x180, "UART" };
        }
    }
    return r;
}

static uint32_t sim_reg_get(MCF5206ESIMState *s, RegSpan r, bool side_effects)
{
    switch (r.kind) {
    case R_SIMR:
        return s->simr;
    case R_MARB:
        return s->marb;
    case R_ICR:
        return s->icr[r.index];
    case R_IMR:
        return s->imr;
    case R_IPR:
        return sim_ipr(s);
    case R_RSR:
        return s->rsr;
    case R_SYPCR:
        return s->sypcr;
    case R_SWIVR:
        return s->swivr;
    case R_CSAR:
        return s->csar[r.index];
    case R_CSMR:
        return s->csmr[r.index];
    case R_CSCR:
        return s->cscr[r.index];
    case R_DMCR:
        return s->dmcr;
    case R_PAR:
        return s->par;
    case R_TIMER:
        return timer_read(&s->timer[r.index], (r.start - 0x100) & 0x1f);
    case R_UIVR:
        return s->uivr[r.index];
    case R_UART:
        return mcf_uart_read(s->uart[r.index],
                             r.start - (r.index ? 0x180 : 0x140), 1);
    case R_UIP:
        /* The input port has no pins connected, so it reads its pull-ups. */
        return 0xff;
    case R_SWSR:
        if (side_effects) {
            qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: read of write-only "
                          "SWSR\n");
        }
        return 0;
    case R_UOP1:
    case R_UOP0:
        /* write-only */
        return 0;
    case R_MBUS:
        return mcf5206e_mbus_read(s->mbus, r.start, side_effects);
    case R_UNIMP:
        if (side_effects) {
            qemu_log_mask(LOG_UNIMP, "mcf5206e: read of %s (MBAR+0x%x): "
                          "not modelled, reads as 0\n", r.name, r.start);
        }
        return 0;
    }
    return 0;
}

static void sim_uop(MCF5206ESIMState *s, RegSpan r, uint8_t val)
{
    unsigned u = r.index;

    if (r.kind == R_UOP1) {
        s->uop[u] |= val;
    } else {
        s->uop[u] &= ~val;
    }
    qemu_log_mask(LOG_UNIMP, "mcf5206e: UART %u output port now 0x%02x: "
                  "no pin is connected to it\n", u + 1, s->uop[u]);
}

static void sim_reg_set(MCF5206ESIMState *s, RegSpan r, uint32_t val)
{
    int n;

    switch (r.kind) {
    case R_SIMR:
        s->simr = val;
        break;
    case R_MARB:
        s->marb = val;
        break;
    case R_ICR:
        n = r.index;
        if (n <= 8) {
            /* The levels of the external and watchdog sources are fixed. */
            val = (val & ~0x1c) | (icr_reset[n] & 0x1c);
        }
        if (n == MCF5206E_SRC_SWT) {
            val &= ~0x80;
        }
        s->icr[n] = val;
        sim_update_irq(s);
        break;
    case R_IMR:
        /* Bit 0 is reserved. */
        s->imr = val & 0xfffe;
        sim_update_irq(s);
        break;
    case R_IPR:
        qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: write of 0x%x to "
                      "read-only IPR ignored\n", val);
        break;
    case R_RSR:
        s->rsr &= ~val;
        break;
    case R_SYPCR:
        if (!s->sypcr_written) {
            s->sypcr_written = true;
            s->sypcr = val;
            wdt_restart(s);
        }
        break;
    case R_SWIVR:
        s->swivr = val;
        break;
    case R_SWSR:
        wdt_service(s, val);
        break;
    case R_CSAR:
        s->csar[r.index] = val;
        sim_cs_update(s, r.index);
        break;
    case R_CSMR:
        memory_region_transaction_begin();
        s->csmr[r.index] = val;
        s->csmr_written[r.index] = true;
        if (r.index == 0 && s->global_cs0) {
            sim_global_cs0_off(s);
        }
        sim_cs_update(s, r.index);
        memory_region_transaction_commit();
        break;
    case R_CSCR:
        s->cscr[r.index] = val;
        sim_cs_update(s, r.index);
        break;
    case R_DMCR:
        s->dmcr = val;
        if (val) {
            qemu_log_mask(LOG_UNIMP, "mcf5206e: DMCR = 0x%04x: default "
                          "memory responses are not modelled\n", val);
        }
        break;
    case R_PAR:
        s->par = val;
        sim_update_irq(s);
        break;
    case R_TIMER:
        timer_write(&s->timer[r.index], (r.start - 0x100) & 0x1f, val);
        break;
    case R_UIVR:
        s->uivr[r.index] = val;
        break;
    case R_UART:
        mcf_uart_write(s->uart[r.index],
                       r.start - (r.index ? 0x180 : 0x140), val, 1);
        break;
    case R_UOP1:
    case R_UOP0:
        sim_uop(s, r, val);
        break;
    case R_MBUS:
        mcf5206e_mbus_write(s->mbus, r.start, val);
        break;
    case R_UNIMP:
        qemu_log_mask(LOG_UNIMP, "mcf5206e: write of %s (MBAR+0x%x) = 0x%x:"
                      " not modelled, ignored\n", r.name, r.start, val);
        break;
    }
}

static uint64_t sim_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MCF5206ESIMState *s = opaque;
    uint64_t val = 0;
    unsigned i = 0;

    while (i < size) {
        RegSpan r = sim_reg_at(addr + i);
        unsigned from = (addr + i) - r.start;
        unsigned take = MIN(r.width - from, size - i);
        uint32_t v;

        if (r.kind == R_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: read of MBAR+0x%"
                          HWADDR_PRIx " is not a register\n", addr + i);
            val <<= 8;
            i++;
            continue;
        }
        if (r.kind == R_MBUS && size != 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: %u-byte read over the "
                          "M-Bus %s (MBAR+0x%x): only byte accesses are "
                          "defined, reads as 0\n", size, r.name, r.start);
            val <<= 8;
            i++;
            continue;
        }
        v = sim_reg_get(s, r, true);
        v >>= 8 * (r.width - from - take);
        v &= (take == 4) ? 0xffffffff : (1u << (8 * take)) - 1;
        val = (val << (8 * take)) | v;
        i += take;
    }
    return val;
}

static void sim_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    MCF5206ESIMState *s = opaque;
    unsigned i = 0;

    while (i < size) {
        RegSpan r = sim_reg_at(addr + i);
        unsigned from = (addr + i) - r.start;
        unsigned take = MIN(r.width - from, size - i);
        unsigned shift = 8 * (r.width - from - take);
        uint64_t mask = (take == 4) ? 0xffffffffull : (1ull << (8 * take)) - 1;
        uint32_t piece = (val >> (8 * (size - i - take))) & mask;
        uint32_t cur;

        if (r.kind == R_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: write of MBAR+0x%"
                          HWADDR_PRIx " is not a register\n", addr + i);
            i++;
            continue;
        }
        if (r.kind == R_MBUS && size != 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "mcf5206e: %u-byte write over the "
                          "M-Bus %s (MBAR+0x%x): only byte accesses are "
                          "defined, ignored\n", size, r.name, r.start);
            i++;
            continue;
        }
        if (take == r.width) {
            cur = piece;
        } else {
            /* A narrow write into a wider register keeps the other lanes. */
            cur = sim_reg_get(s, r, false);
            cur = (cur & ~(mask << shift)) | (piece << shift);
        }
        sim_reg_set(s, r, cur);
        i += take;
    }
}

static const MemoryRegionOps sim_mmio_ops = {
    .read = sim_mmio_read,
    .write = sim_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const M68kBoardOps sim_board_ops = {
    .control_write = sim_control_write,
    .irq_select = sim_board_irq_select,
    .irq_accept = sim_board_irq_accept,
};

/* Reset and realization. */

static void mcf5206e_sim_reset(DeviceState *dev)
{
    MCF5206ESIMState *s = MCF5206E_SIM(dev);
    int i;

    s->mbar_valid = false;
    s->rambar_valid = false;
    sim_map(s, &s->sram, false, 0);
    sim_map(s, &s->mbar_io, false, 0);

    s->simr = 0xc0;
    s->marb = 0;
    memcpy(s->icr, icr_reset, sizeof(s->icr));
    s->imr = 0x3ffe;
    s->par = 0;
    s->dmcr = 0;
    s->sypcr = 0;
    s->sypcr_written = false;
    s->swivr = 0x0f;
    s->swsr_seq = 0;
    s->rsr = s->wdt_reset_cause ? RSR_SWTR : RSR_HRST;
    s->wdt_reset_cause = false;
    timer_del(s->wdt);
    s->src_state = 0;
    s->uivr[0] = s->uivr[1] = 0x0f;
    s->uop[0] = s->uop[1] = 0;

    for (i = 0; i < MCF5206E_NUM_CS; i++) {
        s->csar[i] = 0;
        s->csmr[i] = 0;
        s->cscr[i] = i ? 0 : CSCR0_RESET;
        s->csmr_written[i] = false;
        sim_cs_update(s, i);
    }

    timer_reset(&s->timer[0]);
    timer_reset(&s->timer[1]);

    /* Chip select 0 is the global select until CSMR0 is written. */
    s->global_cs0 = true;
    memory_region_set_enabled(&s->global_io, true);
    for (i = 0; i < 3; i++) {
        memory_region_set_enabled(&s->global_alias[i], true);
    }
}

static void mcf5206e_sim_realize(DeviceState *dev, Error **errp)
{
    MCF5206ESIMState *s = MCF5206E_SIM(dev);
    MemoryRegion *sysmem = get_system_memory();
    static const hwaddr global_alias_base[3] = { 0, 0xffe00000, 0xfff00000 };
    uint64_t rom_len;
    int i;

    if (!s->cpu || !s->boot_rom) {
        error_setg(errp, "mcf5206e-sim needs its m68k-cpu and boot-rom links");
        return;
    }
    if (!s->clock_hz) {
        error_setg(errp, "mcf5206e-sim clock-hz must be non-zero");
        return;
    }
    rom_len = memory_region_size(s->boot_rom);
    if (!is_power_of_2(rom_len) || rom_len > 16 * MiB) {
        error_setg(errp, "the boot ROM size must be a power of two up to "
                   "16 MiB");
        return;
    }

    memory_region_init_io(&s->mbar_io, OBJECT(dev), &sim_mmio_ops, s,
                          "mcf5206e.mbar", MCF5206E_MBAR_SIZE);
    memory_region_add_subregion_overlap(sysmem, 0, &s->mbar_io, 10);
    memory_region_set_enabled(&s->mbar_io, false);
    memory_region_init_ram(&s->sram, OBJECT(dev), "mcf5206e.sram",
                           MCF5206E_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, 0, &s->sram, 10);
    memory_region_set_enabled(&s->sram, false);

    for (i = 0; i < MCF5206E_NUM_CS; i++) {
        g_autofree char *name = g_strdup_printf("mcf5206e.cs%d", i);

        memory_region_init(&s->cs_window[i], OBJECT(dev), name, 64 * KiB);
        memory_region_add_subregion_overlap(sysmem, 0, &s->cs_window[i], 0);
        memory_region_set_enabled(&s->cs_window[i], false);
    }
    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("mcf5206e.cs0.flash%d", i);

        s->rom_alias[i] = g_new0(MemoryRegion, 1);
        memory_region_init_alias(s->rom_alias[i], OBJECT(dev), name,
                                 s->boot_rom, 0, rom_len);
        memory_region_add_subregion(&s->cs_window[0], i * rom_len,
                                    s->rom_alias[i]);
        memory_region_set_enabled(s->rom_alias[i], false);
    }

    memory_region_init_io(&s->global_io, OBJECT(dev), &global_cs0_ops, s,
                          "mcf5206e.global-cs0", 4 * GiB);
    memory_region_add_subregion_overlap(sysmem, 0, &s->global_io, -1000);
    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("mcf5206e.global-cs0.%d", i);

        memory_region_init_alias(&s->global_alias[i], OBJECT(dev), name,
                                 s->boot_rom, 0, rom_len);
        memory_region_add_subregion_overlap(sysmem, global_alias_base[i],
                                            &s->global_alias[i], -500);
    }
    memory_region_init_io(&s->default_mem, OBJECT(dev), &unimp_window_ops,
                          (void *)"mcf5206e default memory", "default-mem",
                          4 * GiB);
    memory_region_add_subregion_overlap(sysmem, 0, &s->default_mem, -2000);

    for (i = 0; i < 2; i++) {
        MCF5206ETimer *t = &s->timer[i];

        t->sim = s;
        t->src = MCF5206E_SRC_TIMER1 + i;
        t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_fire, t);
    }
    s->wdt = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_expired, s);

    qdev_init_gpio_in_named(dev, sim_internal_irq, "internal-irq", 16);
    qdev_init_gpio_in_named(dev, sim_ext_irq, "ext-irq", 8);
    for (i = 0; i < 2; i++) {
        qemu_irq irq = qdev_get_gpio_in_named(dev, "internal-irq",
                                              MCF5206E_SRC_UART1 + i);

        s->uart[i] = mcf_uart_create(irq,
                                     qemu_chr_fe_get_driver(&s->uart_chr[i]));
    }

    s->mbus = MCF5206E_MBUS(qdev_new(TYPE_MCF5206E_MBUS));
    object_property_add_child(OBJECT(dev), "mbus", OBJECT(s->mbus));
    qdev_prop_set_uint32(DEVICE(s->mbus), "clock-hz", s->clock_hz);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->mbus), errp)) {
        return;
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(s->mbus), 0,
                       qdev_get_gpio_in_named(dev, "internal-irq",
                                              MCF5206E_SRC_MBUS));

    m68k_cpu_set_board_ops(s->cpu, &sim_board_ops, s);
}

static const Property mcf5206e_sim_properties[] = {
    DEFINE_PROP_LINK("m68k-cpu", MCF5206ESIMState, cpu, TYPE_M68K_CPU,
                     M68kCPU *),
    DEFINE_PROP_LINK("boot-rom", MCF5206ESIMState, boot_rom, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("clock-hz", MCF5206ESIMState, clock_hz, 48000000),
    DEFINE_PROP_CHR("uart1-chardev", MCF5206ESIMState, uart_chr[0]),
    DEFINE_PROP_CHR("uart2-chardev", MCF5206ESIMState, uart_chr[1]),
};

static void mcf5206e_sim_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, mcf5206e_sim_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "MCF5206e system integration module";
    dc->realize = mcf5206e_sim_realize;
    device_class_set_legacy_reset(dc, mcf5206e_sim_reset);
    /* Registers, timers and the watchdog are not migrated. */
    dc->user_creatable = false;
}

static const TypeInfo mcf5206e_sim_info = {
    .name          = TYPE_MCF5206E_SIM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MCF5206ESIMState),
    .class_init    = mcf5206e_sim_class_init,
};

I2CBus *mcf5206e_sim_mbus(MCF5206ESIMState *s)
{
    return mcf5206e_mbus_get_bus(s->mbus);
}

MemoryRegion *mcf5206e_sim_cs_window(MCF5206ESIMState *s, int n)
{
    return &s->cs_window[n];
}

static void mcf5206e_sim_register_types(void)
{
    type_register_static(&mcf5206e_sim_info);
}

type_init(mcf5206e_sim_register_types)
