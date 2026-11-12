/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic Solutions HDSP card: the i960 Rx I/O processor's side.
 *
 * The i960 core is a library (hw/coproc/i960); this file is everything the
 * core does not own: the local bus decode, the on-chip peripheral units
 * (memory-mapped at 0x1000), the timers, the flash and DRAM, the byte-wide
 * glue and the debug UART, and the slice timer that runs the core. The host
 * faces (the two PCI functions and the secondary bus) are in
 * sgi-sonic-hdsp.c. Hardware reference:
 * docs/system/mips/sgi/hardware/sonic-hdsp.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qemu/host-utils.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "system/dma.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/misc/sgi-sonic-hdsp.h"

#define LOCAL_DRAM_BASE     0xa0000000u
#define LOCAL_BANK0_BASE    0xfe000000u
#define LOCAL_BANK0_SIZE    0x01000000u
#define LOCAL_PMMR_BASE     0x1000u
#define LOCAL_PMMR_END      0x1800u
#define LOCAL_BANK1_SIZE    (16 * MiB)
#define LOCAL_UART_BASE     0xd0000000u
#define LOCAL_BOARD_REG     0xe0040000u
#define LOCAL_TABLES_BASE   0xf0000000u
#define LOCAL_TABLES_SIZE   0x0e000000u
#define LOCAL_POUT_BASE     0x80000000u     /* primary outbound memory */
#define LOCAL_SOUT_BASE     0x88000000u     /* secondary outbound memory */
#define LOCAL_OUT_SIZE      0x04000000u
/*
 * The bus configuration and breakpoint registers the core forwards at reset
 * and for the control table. Bus timing and breakpoints are not modelled, so
 * they only hold what is written.
 */
#define LOCAL_CORE_MMR      0xff008600u
/* Logical memory configuration (DLMCON, LMADRn, LMMRn): stored only. */
#define LOCAL_LMCON         0xff008100u

/* Memory controller: bank 1 base register is word 4 of 0x1500. */
#define MC_BANK1_BASE       (0x10 / 4)

/* The glue logic's register offsets from the bank 1 base. */
#define GLUE_CMD            0x030000u
#define GLUE_RESET          0x0a0000u
#define GLUE_INTREQ         0x028000u
#define GLUE_ENABLE0        0x048000u
#define GLUE_ENABLE1        0x050000u
#define GLUE_BOOT_MODE      0x018000u
#define GLUE_UNK_080000     0x080000u
#define GLUE_UNK_038000     0x038000u

/* Messaging unit */
#define MU_IFH  0
#define MU_IFT  1
#define MU_IPH  2
#define MU_IPT  3
#define MU_OFH  4
#define MU_OFT  5
#define MU_OPH  6
#define MU_OPT  7
#define MUCR_QENABLE        0x1
#define IISR_IMR0           (1u << 0)
#define IISR_IMR1           (1u << 1)
#define IISR_IDB            (1u << 2)
#define IISR_NMI_DB         (1u << 3)
#define IISR_IPQ            (1u << 4)
#define IISR_OFQ_OVERFLOW   (1u << 5)
#define IISR_INDEX          (1u << 6)
#define IISR_APIC_SEL       (1u << 7)
#define IISR_APIC_WIN       (1u << 8)
#define IISR_XINT7_MASK     (IISR_IMR0 | IISR_IMR1 | IISR_IDB | IISR_IPQ | \
                             IISR_INDEX | IISR_APIC_SEL | IISR_APIC_WIN)
#define IISR_NMI_MASK       (IISR_NMI_DB | IISR_OFQ_OVERFLOW)
#define IISR_W1C_MASK       (IISR_IMR0 | IISR_IMR1 | IISR_IPQ | \
                             IISR_OFQ_OVERFLOW | IISR_INDEX | \
                             IISR_APIC_SEL | IISR_APIC_WIN)
#define OISR_OMR0           (1u << 0)
#define OISR_OMR1           (1u << 1)
#define OISR_ODB            (1u << 2)
#define OISR_OPQ            (1u << 3)
#define OISR_PIN_MASK       0xfu
#define ODR_SOFT_MASK       0x0fffffffu
#define ODR_PIN_MASK        0xf0000000u

/*
 * The slice the core runs in, and how long an idle core sleeps. The page
 * wants slices of at most 100 us while the host waits; an idle core sleeps
 * up to 1 ms, but every host access that could wake it (message unit,
 * window write) kicks the slice timer to run at once, so the host never
 * waits on the long sleep. A kicked slice may catch up to 2 ms of guest time.
 */
#define SLICE_NS            (100 * 1000)
#define IDLE_SLICE_NS       (1000 * 1000)
#define MAX_CATCHUP_NS      (2 * 1000 * 1000)

/* AT29C040A: load period and write cycle times (data sheet). */
#define FLASH_LOAD_NS       (150 * 1000)
#define FLASH_WRITE_NS      (10 * 1000 * 1000)

#define PDIDR_VALUE         0x00000013u     /* Intel manufacturer id only */

static inline int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint8_t *bridge_cfg(SonicHDSPState *h)
{
    return PCI_DEVICE(h)->config;
}

static uint8_t *atu_cfg(SonicHDSPState *h)
{
    return PCI_DEVICE(h->atu)->config;
}

static void local_unclaimed(SonicHDSPState *h, const char *what,
                            uint32_t addr, unsigned size, uint32_t val)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-sonic-hdsp: unclaimed local %s of %u at 0x%08x "
                  "(value 0x%x, pc 0x%08x)\n", what, size, addr, val,
                  h->cpu.cur_ip);
}

/* ------------------------------------------------------------------ */
/* Flash: the AT29C040A programming model                             */
/* ------------------------------------------------------------------ */

static void flash_commit_finish(SonicHDSPState *h)
{
    uint32_t base = h->fl_sector * 256;

    if (h->fl_count) {
        for (unsigned i = 0; i < 256; i++) {
            bool loaded = h->fl_loaded[i / 8] & (1u << (i % 8));

            h->flash[base + i] = loaded ? h->fl_data[i] : 0xff;
        }
        if (h->flash_fd >= 0 &&
            (lseek(h->flash_fd, base, SEEK_SET) != base ||
             qemu_write_full(h->flash_fd, h->flash + base, 256) != 256)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-sonic-hdsp: flash write-back failed\n");
        }
        if (h->fl_arm_protect) {
            h->fl_protect = true;
        }
    }
    h->fl_arm_protect = false;
    h->fl_armed = false;
    h->fl_committing = false;
}

static void flash_commit_start(SonicHDSPState *h, int64_t at)
{
    h->fl_loading = false;
    h->fl_committing = true;
    h->fl_busy_until_ns = at + FLASH_WRITE_NS;
}

/* Lazily end the load period and the write cycle against virtual time. */
static void flash_sync(SonicHDSPState *h, int64_t now)
{
    if (h->fl_loading && now >= h->fl_deadline_ns) {
        flash_commit_start(h, h->fl_deadline_ns - FLASH_LOAD_NS);
    }
    if (h->fl_committing && now >= h->fl_busy_until_ns) {
        flash_commit_finish(h);
    }
}

static uint8_t flash_read8(SonicHDSPState *h, uint32_t off)
{
    uint8_t v;

    if (!h->fl_loading && !h->fl_committing) {
        return h->flash[off];
    }
    flash_sync(h, now_ns());
    if (!h->fl_committing) {
        return h->flash[off];
    }
    /* DATA polling on the last byte loaded, toggle bit on any read. */
    h->fl_toggle = !h->fl_toggle;
    if (off == h->fl_last) {
        v = ~h->fl_data[off & 0xff] & 0x80;
        v |= h->fl_data[off & 0xff] & 0x3f;
    } else {
        v = h->flash[off] & ~0x40;
    }
    return v | (h->fl_toggle ? 0x40 : 0);
}

static const struct { uint16_t addr; uint8_t data; } fl_enable_seq[3] = {
    { 0x5555, 0xaa }, { 0x2aaa, 0x55 }, { 0x5555, 0xa0 },
};
static const struct { uint16_t addr; uint8_t data; } fl_disable_seq[6] = {
    { 0x5555, 0xaa }, { 0x2aaa, 0x55 }, { 0x5555, 0x80 },
    { 0x5555, 0xaa }, { 0x2aaa, 0x55 }, { 0x5555, 0x20 },
};

static void flash_write8(SonicHDSPState *h, uint32_t off, uint8_t val)
{
    int64_t now = now_ns();

    if (!h->flash_writable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-sonic-hdsp: flash write 0x%02x at 0x%05x dropped "
                      "(flash-writable is off)\n", val, off);
        return;
    }
    flash_sync(h, now);
    if (h->fl_committing) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: flash write during the "
                      "write cycle refused\n");
        return;
    }

    /* Command prefix bytes, accepted between loads. */
    if (!h->fl_loading) {
        unsigned en = h->fl_seq & 7, dis = h->fl_seq >> 3;

        if (en < 3 && off == fl_enable_seq[en].addr &&
            val == fl_enable_seq[en].data) {
            h->fl_seq = (dis << 3) | (en + 1);
            if (en + 1 == 3) {
                h->fl_armed = true;
                h->fl_arm_protect = true;
                h->fl_seq = 0;
            }
            return;
        }
        if (dis < 6 && off == fl_disable_seq[dis].addr &&
            val == fl_disable_seq[dis].data) {
            h->fl_seq = ((dis + 1) << 3) | en;
            if (dis + 1 == 6) {
                h->fl_protect = false;
                h->fl_seq = 0;
            }
            return;
        }
        if (h->fl_seq) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: flash command "
                          "sequence broken at 0x%05x\n", off);
            h->fl_seq = 0;
            return;
        }
        if (h->fl_protect && !h->fl_armed) {
            /* A write with protection on and no prefix stores nothing. */
            h->fl_count = 0;
            h->fl_last = off;
            memset(h->fl_data, 0, sizeof(h->fl_data));
            h->fl_committing = true;
            h->fl_busy_until_ns = now + FLASH_WRITE_NS;
            return;
        }
        h->fl_loading = true;
        h->fl_sector = off >> 8;
        h->fl_count = 0;
        memset(h->fl_loaded, 0, sizeof(h->fl_loaded));
        memset(h->fl_data, 0xff, sizeof(h->fl_data));
    } else if ((off >> 8) != h->fl_sector) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: flash byte outside the "
                      "sector being loaded: load abandoned\n");
        h->fl_loading = false;
        h->fl_count = 0;
        return;
    }

    if (!(h->fl_loaded[(off & 0xff) / 8] & (1u << (off % 8)))) {
        h->fl_loaded[(off & 0xff) / 8] |= 1u << (off % 8);
        h->fl_count++;
    }
    h->fl_data[off & 0xff] = val;
    h->fl_last = off;
    h->fl_deadline_ns = now + FLASH_LOAD_NS;
    if (h->fl_count == 256) {
        flash_commit_start(h, now);
    }
}

static uint32_t flash_read(SonicHDSPState *h, uint32_t off, unsigned size)
{
    uint32_t v = 0;

    if (!h->fl_loading && !h->fl_committing) {
        return ldn_le_p(h->flash + off, size);
    }
    for (unsigned i = 0; i < size; i++) {
        v |= (uint32_t)flash_read8(h, off + i) << (8 * i);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */
/* ------------------------------------------------------------------ */

static unsigned timer_prescale(uint32_t tmr)
{
    return 1u << ((tmr >> 4) & 3);
}

static uint64_t timer_ticks(SonicHDSPState *h, SonicHDSPTimer *t, int64_t now)
{
    uint64_t hz = h->iop_clock / timer_prescale(t->tmr);

    return muldiv64(now - t->start_ns, hz, NANOSECONDS_PER_SECOND);
}

static uint32_t timer_count(SonicHDSPState *h, unsigned n, int64_t now)
{
    SonicHDSPTimer *t = &h->timer[n];
    uint64_t ticks;

    if (!(t->tmr & 2)) {
        return t->tcr;
    }
    ticks = timer_ticks(h, t, now);
    return ticks >= t->tcr ? 0 : t->tcr - ticks;
}

/* Fold elapsed time into the timers; an expiry latches a timer interrupt. */
static void timers_sync(SonicHDSPState *h, int64_t now)
{
    for (unsigned n = 0; n < 2; n++) {
        SonicHDSPTimer *t = &h->timer[n];
        uint64_t ticks;

        if (!(t->tmr & 2)) {
            continue;
        }
        ticks = timer_ticks(h, t, now);
        if (ticks < t->tcr) {
            continue;
        }
        if (t->tmr & 4) {
            uint64_t period = t->trr ? t->trr : 1;
            uint64_t rest = (ticks - t->tcr) % period;

            t->tcr = period - rest;
            t->start_ns = now;
        } else {
            t->tmr &= ~2u;
            t->tmr |= 1;
            t->tcr = 0;
        }
        i960_set_tint(&h->cpu, n);
    }
}

/* Nanoseconds to the next timer expiry, or -1. */
static int64_t timers_next(SonicHDSPState *h, int64_t now)
{
    int64_t best = -1;

    for (unsigned n = 0; n < 2; n++) {
        SonicHDSPTimer *t = &h->timer[n];
        uint64_t hz = h->iop_clock / timer_prescale(t->tmr);
        uint64_t left, ns;

        if (!(t->tmr & 2)) {
            continue;
        }
        left = timer_count(h, n, now);
        ns = muldiv64(left, NANOSECONDS_PER_SECOND, hz) + 1;
        if (best < 0 || (int64_t)ns < best) {
            best = ns;
        }
    }
    return best;
}

static uint32_t timer_reg_read(SonicHDSPState *h, uint32_t addr)
{
    unsigned n = (addr >> 4) & 1;
    SonicHDSPTimer *t = &h->timer[n];
    int64_t now = now_ns();
    uint32_t v;

    timers_sync(h, now);
    switch (addr & 0xc) {
    case 0x0:
        return t->trr;
    case 0x4:
        return timer_count(h, n, now);
    case 0x8:
        v = t->tmr;
        t->tmr &= ~1u;      /* terminal count clears on access */
        return v;
    default:
        return 0;
    }
}

static void timer_reg_write(SonicHDSPState *h, uint32_t addr, uint32_t val)
{
    unsigned n = (addr >> 4) & 1;
    SonicHDSPTimer *t = &h->timer[n];
    int64_t now = now_ns();

    timers_sync(h, now);
    switch (addr & 0xc) {
    case 0x0:
        t->trr = val;
        break;
    case 0x4:
        t->tcr = val;
        t->start_ns = now;
        break;
    case 0x8: {
        bool was = t->tmr & 2;

        t->tcr = timer_count(h, n, now);
        t->start_ns = now;
        t->tmr = (val & ~1u);
        if (!was && (val & 2) && (val & 4) && !t->tcr) {
            t->tcr = t->trr;
        }
        break;
    }
    default:
        break;
    }
    sonic_hdsp_iop_kick(h);
}

/* ------------------------------------------------------------------ */
/* Interrupt wiring                                                   */
/* ------------------------------------------------------------------ */

static uint32_t iisr_eff(SonicHDSPState *h)
{
    uint32_t v = h->iisr & ~(IISR_IDB | IISR_NMI_DB);

    if (h->idr & 0x7fffffffu) {
        v |= IISR_IDB;
    }
    if (h->idr & 0x80000000u) {
        v |= IISR_NMI_DB;
    }
    return v;
}

static uint32_t oisr_eff(SonicHDSPState *h)
{
    uint32_t v = h->oisr & (OISR_OMR0 | OISR_OMR1);

    if (h->odr & ODR_SOFT_MASK) {
        v |= OISR_ODB;
    }
    if (h->qptr[MU_OPH] != h->qptr[MU_OPT]) {
        v |= OISR_OPQ;
    }
    return v | ((h->odr & ODR_PIN_MASK) >> 24);
}

void sonic_hdsp_update_irq(SonicHDSPState *h)
{
    uint32_t ii = iisr_eff(h) & ~h->iimr;
    bool pin, nmi, x6 = false;
    uint8_t xint = 0;

    pin = (oisr_eff(h) & ~h->oimr & OISR_PIN_MASK) ||
          ((h->glue_intreq & 0x1f) && h->glue_enable[0] && h->glue_enable[1]);
    /* Pin 0 means the function uses no interrupt pin: nothing to drive. */
    pin = pin && atu_cfg(h)[PCI_INTERRUPT_PIN] >= 1 &&
          atu_cfg(h)[PCI_INTERRUPT_PIN] <= PCI_NUM_PINS;
    if (pin != h->irq_level) {
        h->irq_level = pin;
        pci_set_irq(PCI_DEVICE(h->atu), pin);
    }

    for (unsigned c = 0; c < 3; c++) {
        if ((h->dma[c].csr & 0x300) && (h->dma[c].dcr & 0x10)) {
            x6 = true;
        }
    }
    if (ii & IISR_XINT7_MASK) {
        xint |= 1u << 7;
    }
    if (x6) {
        xint |= 1u << 6;
    }
    i960_set_xint(&h->cpu, xint);

    nmi = ii & IISR_NMI_MASK;
    if (nmi && !h->nmi_latched) {
        i960_nmi(&h->cpu);
    }
    h->nmi_latched = nmi;
    if (xint || nmi) {
        sonic_hdsp_iop_kick(h);
    }
}

/* ------------------------------------------------------------------ */
/* Local bus access helpers (the DMA engine's and the ATU's view)      */
/* ------------------------------------------------------------------ */

static void local_read_buf(SonicHDSPState *h, uint32_t addr, uint8_t *buf,
                           size_t len)
{
    uint32_t off = addr - LOCAL_DRAM_BASE;

    if (addr >= LOCAL_DRAM_BASE && off < h->dram_size &&
        len <= h->dram_size - off) {
        memcpy(buf, h->dram_ptr + off, len);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = sonic_hdsp_local_read(h, addr + i, 1);
    }
}

static void local_write_buf(SonicHDSPState *h, uint32_t addr,
                            const uint8_t *buf, size_t len)
{
    uint32_t off = addr - LOCAL_DRAM_BASE;

    if (addr >= LOCAL_DRAM_BASE && off < h->dram_size &&
        len <= h->dram_size - off) {
        memcpy(h->dram_ptr + off, buf, len);
        memory_region_set_dirty(&h->dram, off, len);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        sonic_hdsp_local_write(h, addr + i, buf[i], 1);
    }
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

#define DMA_CSR_ACTIVE      (1u << 10)
#define DMA_CSR_EOT         (1u << 9)
#define DMA_CSR_EOC         (1u << 8)
#define DMA_CSR_LOCAL_FAULT (1u << 5)
#define DMA_CSR_MASTER_ABORT (1u << 3)
#define DMA_CSR_W1C         0x36fu
/* A chain longer than this in one run is a loop or a wild pointer. */
#define DMA_MAX_DESCRIPTORS 4096
/* Bytes one run moves before it yields to the next slice. */
#define DMA_MAX_BYTES       (2 * 1024 * 1024)

/*
 * Run a channel for at most DMA_MAX_BYTES of data. A transfer that is not
 * finished stays active and the slice timer calls this again with @resume,
 * so a huge or looping descriptor never holds the main loop for long.
 */
static void dma_run(SonicHDSPState *h, unsigned ch, bool resume)
{
    SonicHDSPDMA *d = &h->dma[ch];
    PCIDevice *atu = PCI_DEVICE(h->atu);
    uint32_t budget = DMA_MAX_BYTES;
    unsigned count = 0;
    bool mid = false;

    if (!(d->ccr & 1)) {
        return;
    }
    if (d->csr & DMA_CSR_ACTIVE) {
        if (!resume) {
            return;
        }
        mid = true;
    } else {
        if (!d->ndar) {
            return;
        }
        d->csr |= DMA_CSR_ACTIVE;
    }
    for (;;) {
        unsigned cmd;
        bool to_pci;

        if (!mid) {
            uint32_t w[6];

            if (++count > DMA_MAX_DESCRIPTORS) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: DMA channel "
                              "%u chain longer than %u descriptors "
                              "stopped\n", ch, DMA_MAX_DESCRIPTORS);
                d->csr |= DMA_CSR_LOCAL_FAULT;
                break;
            }
            local_read_buf(h, d->ndar, (uint8_t *)w, sizeof(w));
            d->dar = d->ndar;
            d->ndar = le32_to_cpu(w[0]) & ~0x1fu;
            d->padr = le32_to_cpu(w[1]);
            d->puadr = le32_to_cpu(w[2]);
            d->ladr = le32_to_cpu(w[3]);
            d->bcr = le32_to_cpu(w[4]) & 0xffffff;
            d->dcr = le32_to_cpu(w[5]);
        }
        mid = false;
        cmd = d->dcr & 0xf;
        to_pci = cmd == 0x7 || cmd == 0xf;
        if (!to_pci && cmd != 0x6 && cmd != 0xc && cmd != 0xe) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DMA channel %u PCI "
                          "command 0x%x not supported\n", ch, cmd);
            d->csr |= DMA_CSR_MASTER_ABORT;
            break;
        }
        if (ch == 2 || d->puadr) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DMA channel %u to the "
                          "secondary bus or a 64-bit address is not "
                          "modelled\n", ch);
            d->csr |= DMA_CSR_MASTER_ABORT;
            break;
        }
        while (d->bcr) {
            uint8_t buf[4096];
            uint32_t n = MIN(d->bcr, sizeof(buf));
            MemTxResult r;

            if (!budget) {
                return;     /* still active: the next slice continues */
            }
            n = MIN(n, budget);
            if (to_pci) {
                local_read_buf(h, d->ladr, buf, n);
                r = pci_dma_write(atu, d->padr, buf, n);
            } else {
                r = pci_dma_read(atu, d->padr, buf, n);
                if (r == MEMTX_OK) {
                    local_write_buf(h, d->ladr, buf, n);
                }
            }
            if (r != MEMTX_OK) {
                d->csr |= DMA_CSR_MASTER_ABORT;
                goto out;
            }
            d->padr += n;
            d->ladr += n;
            d->bcr -= n;
            budget -= n;
        }
        d->csr |= d->ndar ? DMA_CSR_EOT : DMA_CSR_EOC;
        if (!d->ndar) {
            break;
        }
    }
out:
    d->csr &= ~DMA_CSR_ACTIVE;
    d->ccr &= ~2u;
    sonic_hdsp_update_irq(h);
}

/* ------------------------------------------------------------------ */
/* Messaging unit                                                     */
/* ------------------------------------------------------------------ */

static uint32_t mu_qbytes(SonicHDSPState *h)
{
    return ((h->mucr >> 1) & 0x1f) * 16 * KiB;
}

static uint32_t mu_advance(SonicHDSPState *h, unsigned reg)
{
    uint32_t qsz = mu_qbytes(h), p = h->qptr[reg] + 4;

    if (qsz && !(p & (qsz - 1))) {
        p -= qsz;
    }
    return p & 0xfffff;
}

static uint32_t mu_queue_read(SonicHDSPState *h, unsigned tail, unsigned head)
{
    uint32_t v;

    if (!(h->mucr & MUCR_QENABLE) || h->qptr[tail] == h->qptr[head]) {
        return 0xffffffffu;
    }
    v = sonic_hdsp_local_read(h, h->qbar + h->qptr[tail], 4);
    h->qptr[tail] = mu_advance(h, tail);
    return v;
}

static void mu_queue_write(SonicHDSPState *h, unsigned head, uint32_t val)
{
    if (!(h->mucr & MUCR_QENABLE)) {
        return;
    }
    sonic_hdsp_local_write(h, h->qbar + h->qptr[head], val, 4);
    h->qptr[head] = mu_advance(h, head);
}

/* The host's view of the messaging unit: BAR0 offsets 0 to 0xfff. */
uint32_t sonic_hdsp_mu_host_read(SonicHDSPState *h, hwaddr off, unsigned size)
{
    uint32_t v = 0;

    /* Queue ports take one word per access (Rx manual 17.4). */
    if (size != 4 && ((off & ~3) == 0x40 || (off & ~3) == 0x44)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: %u-byte read of a "
                      "queue port ignored\n", size);
        return UINT32_MAX >> (32 - 8 * size);
    }

    switch (off & ~3) {
    case 0x10:
        v = h->imr[0];
        break;
    case 0x14:
        v = h->imr[1];
        break;
    case 0x18:
        v = h->omr[0];
        break;
    case 0x1c:
        v = h->omr[1];
        break;
    case 0x20:
        v = h->idr;
        break;
    case 0x24:
        v = iisr_eff(h);
        break;
    case 0x28:
        v = h->iimr;
        break;
    case 0x2c:
        v = h->odr;
        break;
    case 0x30:
        v = oisr_eff(h);
        break;
    case 0x34:
        v = h->oimr;
        break;
    case 0x40:
        v = mu_queue_read(h, MU_IFT, MU_IFH);
        sonic_hdsp_update_irq(h);
        break;
    case 0x44:
        v = mu_queue_read(h, MU_OPT, MU_OPH);
        sonic_hdsp_update_irq(h);
        break;
    default:
        if (off < 0x50) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: host read of "
                          "messaging unit offset 0x%" HWADDR_PRIx "\n", off);
        }
        break;
    }
    return v >> ((off & 3) * 8);
}

void sonic_hdsp_mu_host_write(SonicHDSPState *h, hwaddr off, uint32_t val,
                              unsigned size)
{
    if (size != 4 && off < 0x50) {
        /* The registers are words: a partial write has no defined result. */
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: %u-byte write of "
                      "messaging unit offset 0x%" HWADDR_PRIx " ignored\n",
                      size, off);
        return;
    }
    switch (off & ~3) {
    case 0x00:
        h->arsr = val;
        h->iisr |= IISR_APIC_SEL;
        break;
    case 0x08:
        h->awr = val;
        h->iisr |= IISR_APIC_WIN;
        break;
    case 0x10:
        h->imr[0] = val;
        h->iisr |= IISR_IMR0;
        break;
    case 0x14:
        h->imr[1] = val;
        h->iisr |= IISR_IMR1;
        break;
    case 0x20:
        h->idr |= val;          /* the host can only set bits */
        break;
    case 0x28:
        h->iimr = val;
        break;
    case 0x2c:
        h->odr &= ~val;         /* the host can only clear bits */
        break;
    case 0x30:
        h->oisr &= ~(val & (OISR_OMR0 | OISR_OMR1));
        break;
    case 0x34:
        h->oimr = val;
        break;
    case 0x40:
        if (!(h->mucr & MUCR_QENABLE)) {
            break;
        }
        mu_queue_write(h, MU_IPH, val);
        h->iisr |= IISR_IPQ;
        break;
    case 0x44:
        if (!(h->mucr & MUCR_QENABLE)) {
            break;
        }
        mu_queue_write(h, MU_OFH, val);
        if (h->qptr[MU_OFH] == h->qptr[MU_OFT]) {
            h->iisr |= IISR_OFQ_OVERFLOW;
        }
        break;
    default:
        if (off >= 0x50) {
            /* Index registers: a write interrupts the i960. */
            h->iisr |= IISR_INDEX;
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: host write of "
                          "messaging unit offset 0x%" HWADDR_PRIx "\n", off);
        }
        break;
    }
    sonic_hdsp_update_irq(h);
}

/* ------------------------------------------------------------------ */
/* On-chip peripheral registers (local 0x1000 to 0x17ff)              */
/* ------------------------------------------------------------------ */

static void atu_apply_limit(SonicHDSPState *h)
{
    PCIDevice *f1 = PCI_DEVICE(h->atu);
    uint32_t limit = h->atu_x[0];

    if ((limit & 0xfffffff0u) != 0xf8000000u) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: inbound limit 0x%08x "
                      "narrows the BAR, but the region stays 128 MiB\n",
                      limit);
    }
    /* The BAR sizes itself from the limit: bits above the window are base. */
    pci_set_long(f1->wmask + PCI_BASE_ADDRESS_0, limit & 0xfffffff0u);
}

static uint32_t soccdp_access(SonicHDSPState *h, bool write, uint32_t val)
{
    uint32_t addr = h->atu_x[(0x12a8 - 0x1240) / 4];

    /*
     * A type 0 cycle: one IDSEL line, S_AD16 and up, selects the device, as
     * the host's type 1 conversion does; lines 11 to 15 are private to the
     * chip and have no device.
     */
    h->iop_access = true;
    if (write) {
        sonic_hdsp_secondary_config_write(h, addr, val);
    } else {
        val = sonic_hdsp_secondary_config_read(h, addr);
    }
    h->iop_access = false;
    return val;
}

static uint32_t pmmr_read32(SonicHDSPState *h, uint32_t addr)
{
    unsigned c;

    switch (addr) {
    case 0x1240 ... 0x12b4:
        if (addr == 0x12b0) {
            return soccdp_access(h, false, 0);
        }
        if (addr == 0x12ac) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: primary outbound "
                          "configuration cycle not modelled\n");
            return 0xffffffffu;
        }
        return h->atu_x[(addr - 0x1240) / 4];
    case 0x1300:
        return h->arsr;
    case 0x1308:
        return h->awr;
    case 0x1310:
        return h->imr[0];
    case 0x1314:
        return h->imr[1];
    case 0x1318:
        return h->omr[0];
    case 0x131c:
        return h->omr[1];
    case 0x1320:
        return h->idr;
    case 0x1324:
        return iisr_eff(h);
    case 0x1328:
        return h->iimr;
    case 0x132c:
        return h->odr;
    case 0x1330:
        return oisr_eff(h);
    case 0x1334:
        return h->oimr;
    case 0x1350:
        return h->mucr;
    case 0x1354:
        return h->qbar;
    case 0x1360 ... 0x137c:
        /* A read returns the local address: QBAR's upper bits, the offset. */
        return (h->qbar & 0xfff00000u) | h->qptr[(addr - 0x1360) / 4];
    case 0x1380:
        return h->iar;
    case 0x1400 ... 0x14a4:
        c = (addr - 0x1400) >> 6;
        {
            SonicHDSPDMA *d = &h->dma[c];

            switch (addr & 0x3c) {
            case 0x00:
                return d->ccr;
            case 0x04:
                return d->csr;
            case 0x0c:
                return d->dar;
            case 0x10:
                return d->ndar;
            case 0x14:
                return d->padr;
            case 0x18:
                return d->puadr;
            case 0x1c:
                return d->ladr;
            case 0x20:
                return d->bcr;
            case 0x24:
                return d->dcr;
            default:
                return 0;
            }
        }
    case 0x1500 ... 0x153c:
        return h->mc[(addr - 0x1500) / 4];
    case 0x1600:
        return h->lba[0];
    case 0x1604:
        return h->lba[1];
    case 0x1610:
        return h->reg_1610;
    case 0x1680 ... 0x1690:
        return h->i2c[(addr - 0x1680) / 4];
    case 0x1700: {
        uint32_t v = 0;

        if (iisr_eff(h) & ~h->iimr & IISR_NMI_MASK) {
            v |= 0x100;
        }
        return v;
    }
    case 0x1704:
        return (iisr_eff(h) & ~h->iimr & IISR_XINT7_MASK) ? 4 : 0;
    case 0x1708: {
        uint32_t v = 0;

        for (c = 0; c < 3; c++) {
            if ((h->dma[c].csr & 0x300) && (h->dma[c].dcr & 0x10)) {
                v |= 1u << c;
            }
        }
        return v;
    }
    case 0x1710:
        return PDIDR_VALUE;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: read of on-chip register "
                      "0x%04x\n", addr);
        return 0;
    }
}

/*
 * The I2C unit as master with one slave: a 24C08-class EEPROM at 7-bit
 * address 0x50 (four 256-byte pages on address bits 1 and 2). A transfer
 * request completes at once. Any other address is not acknowledged.
 */
#define I2C_ICR_START   (1u << 0)
#define I2C_ICR_STOP    (1u << 1)
#define I2C_ICR_TB      (1u << 3)
#define I2C_ISR_NACK    (1u << 1)
#define I2C_ISR_BUSY    (1u << 3)
#define I2C_ISR_ITE     (1u << 6)
#define I2C_ISR_IRF     (1u << 7)

static void i2c_icr_write(SonicHDSPState *h, uint32_t val)
{
    uint32_t *icr = &h->i2c[0], *isr = &h->i2c[1], *idbr = &h->i2c[3];

    if (val & (1u << 14)) {         /* unit reset */
        h->i2c_state = 0;
        *isr = 0;
        *icr = 0;
        return;
    }
    *icr = val & ~(I2C_ICR_TB | I2C_ICR_START | I2C_ICR_STOP);
    if (!(val & I2C_ICR_TB)) {
        return;
    }
    *isr &= ~(I2C_ISR_NACK | I2C_ISR_ITE | I2C_ISR_IRF);
    if (val & I2C_ICR_START) {
        uint8_t a = *idbr;

        h->i2c_state = 0;
        if ((a >> 3) == 0x50 >> 2) {
            h->i2c_ptr = (h->i2c_ptr & 0xff) | (((a >> 1) & 3) << 8);
            h->i2c_state = (a & 1) ? 3 : 1;
            *isr |= I2C_ISR_BUSY;
        } else {
            *isr |= I2C_ISR_NACK;
        }
        *isr |= I2C_ISR_ITE;
    } else if (h->i2c_state == 1) {     /* word address */
        h->i2c_ptr = (h->i2c_ptr & 0x300) | (*idbr & 0xff);
        h->i2c_state = 2;
        *isr |= I2C_ISR_ITE;
    } else if (h->i2c_state == 2) {     /* data written to the part */
        h->eeprom[h->i2c_ptr & 0x3ff] = *idbr;
        h->i2c_ptr = (h->i2c_ptr & 0x300) | ((h->i2c_ptr + 1) & 0xff);
        *isr |= I2C_ISR_ITE;
    } else if (h->i2c_state == 3) {     /* data read from the part */
        *idbr = h->eeprom[h->i2c_ptr & 0x3ff];
        h->i2c_ptr = (h->i2c_ptr & 0x300) | ((h->i2c_ptr + 1) & 0xff);
        *isr |= I2C_ISR_IRF;
    } else {
        *isr |= I2C_ISR_NACK | I2C_ISR_ITE;
    }
    if (val & I2C_ICR_STOP) {
        h->i2c_state = 0;
        *isr &= ~I2C_ISR_BUSY;
    }
}

static void pmmr_write32(SonicHDSPState *h, uint32_t addr, uint32_t val)
{
    unsigned c;

    switch (addr) {
    case 0x1240 ... 0x12b4: {
        unsigned i = (addr - 0x1240) / 4;

        if (addr == 0x12b0) {
            soccdp_access(h, true, val);
            break;
        }
        if (addr == 0x12ac) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: primary outbound "
                          "configuration cycle not modelled\n");
            break;
        }
        if (addr == 0x1290 || addr == 0x1294) {
            h->atu_x[i] &= ~val;        /* status: write one to clear */
            break;
        }
        h->atu_x[i] = val;
        if (addr == 0x1240) {
            atu_apply_limit(h);
        }
        break;
    }
    case 0x1300:
        h->arsr = val;
        break;
    case 0x1308:
        h->awr = val;
        break;
    case 0x1310:
        h->imr[0] = val;
        break;
    case 0x1314:
        h->imr[1] = val;
        break;
    case 0x1318:
        h->omr[0] = val;
        h->oisr |= OISR_OMR0;
        break;
    case 0x131c:
        h->omr[1] = val;
        h->oisr |= OISR_OMR1;
        break;
    case 0x1320:
        h->idr &= ~val;
        break;
    case 0x1324:
        h->iisr &= ~(val & IISR_W1C_MASK);
        break;
    case 0x1328:
        h->iimr = val;
        break;
    case 0x132c:
        h->odr |= val;
        if (val & ODR_PIN_MASK) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: outbound doorbell "
                          "dedicated PCI interrupt pins not modelled\n");
        }
        break;
    case 0x1330:
        h->oisr &= ~(val & (OISR_OMR0 | OISR_OMR1));
        break;
    case 0x1334:
        h->oimr = val;
        break;
    case 0x1350:
        h->mucr = val & 0x3f;
        break;
    case 0x1354:
        h->qbar = val & 0xfff00000u;
        break;
    case 0x1360 ... 0x137c:
        /* A write keeps bits 19:2 only. */
        h->qptr[(addr - 0x1360) / 4] = val & 0xffffc;
        break;
    case 0x1380:
        h->iar = val;
        break;
    case 0x1400 ... 0x14a4: {
        SonicHDSPDMA *d;

        c = (addr - 0x1400) >> 6;
        d = &h->dma[c];
        switch (addr & 0x3c) {
        case 0x00:
            d->ccr = val & 3;
            dma_run(h, c, false);
            break;
        case 0x04:
            d->csr &= ~(val & DMA_CSR_W1C);
            break;
        case 0x10:
            d->ndar = val & ~0x1fu;
            dma_run(h, c, false);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: write of "
                          "read-only DMA register 0x%04x\n", addr);
            break;
        }
        break;
    }
    case 0x1500 ... 0x153c:
        h->mc[(addr - 0x1500) / 4] = val;
        break;
    case 0x1600:
        h->lba[0] = val;
        break;
    case 0x1604:
        h->lba[1] = val;
        break;
    case 0x1610:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: write of reserved "
                      "register 0x1610 = 0x%x\n", val);
        h->reg_1610 = val;
        break;
    case 0x1680:
        i2c_icr_write(h, val);
        break;
    case 0x1684:
        h->i2c[1] &= ~(val & 0x7f0);
        break;
    case 0x1688 ... 0x1690:
        h->i2c[(addr - 0x1680) / 4] = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: write of on-chip register "
                      "0x%04x = 0x%x\n", addr, val);
        break;
    }
    sonic_hdsp_update_irq(h);
}

/* Bridge unit and ATU configuration registers are the functions' own. */
static uint8_t *pmmr_cfg_byte(SonicHDSPState *h, uint32_t addr)
{
    if (addr >= 0x1000 && addr < 0x1060) {
        return bridge_cfg(h) + (addr - 0x1000);
    }
    if (addr >= 0x1200 && addr < 0x1240) {
        return atu_cfg(h) + (addr - 0x1200);
    }
    return NULL;
}

static bool pmmr_data_port(uint32_t a);

static uint32_t pmmr_read(SonicHDSPState *h, uint32_t addr, unsigned size)
{
    uint8_t *p = pmmr_cfg_byte(h, addr);

    if (p) {
        uint32_t v = 0;

        for (unsigned i = 0; i < size; i++) {
            uint8_t *q = pmmr_cfg_byte(h, addr + i);

            v |= (uint32_t)(q ? *q : 0) << (8 * i);
        }
        return v;
    }
    if (size != 4 && pmmr_data_port(addr & ~3u)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: %u-byte read of "
                      "configuration data port 0x%04x ignored\n", size, addr);
        return UINT32_MAX >> (32 - 8 * size);
    }
    return pmmr_read32(h, addr & ~3u) >> ((addr & 3) * 8);
}

/* Registers whose bits clear when written with one. */
static bool pmmr_w1c(uint32_t a)
{
    return a == 0x1290 || a == 0x1294 || a == 0x1324 || a == 0x1330 ||
           a == 0x1404 || a == 0x1444 || a == 0x1484 || a == 0x1684;
}

/* Registers whose read starts a bus cycle: no read-modify-write on them. */
static bool pmmr_data_port(uint32_t a)
{
    return a == 0x12ac || a == 0x12b0;
}

static void pmmr_write(SonicHDSPState *h, uint32_t addr, uint32_t val,
                       unsigned size)
{
    uint8_t *p = pmmr_cfg_byte(h, addr);

    if (p) {
        for (unsigned i = 0; i < size; i++) {
            uint8_t *q = pmmr_cfg_byte(h, addr + i);

            if (q && !(addr + i == 0x100e)) {   /* header type is fixed */
                *q = val >> (8 * i);
            }
        }
        if (addr >= 0x1000 && addr < 0x1060) {
            pci_bridge_update_mappings(PCI_BRIDGE(h));
        }
        if (addr <= 0x123d && addr + size > 0x123d) {
            /* A new pin: let go of the old one before the line follows it. */
            pci_device_deassert_intx(PCI_DEVICE(h->atu));
            h->irq_level = false;
            sonic_hdsp_update_irq(h);
        }
        return;
    }
    if (size != 4) {
        uint32_t shift = (addr & 3) * 8;
        uint32_t mask = (size == 4 ? ~0u : (1u << (size * 8)) - 1) << shift;

        if (pmmr_data_port(addr & ~3u)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: %u-byte access "
                          "to configuration data port 0x%04x ignored\n",
                          size, addr);
            return;
        }
        if (pmmr_w1c(addr & ~3u)) {
            /* Write one to clear: only the bytes written can clear bits. */
            val = (val << shift) & mask;
        } else {
            val = (pmmr_read32(h, addr & ~3u) & ~mask) |
                  ((val << shift) & mask);
        }
    }
    pmmr_write32(h, addr & ~3u, val);
}

/* ------------------------------------------------------------------ */
/* Board glue (memory controller bank 1) and byte registers           */
/* ------------------------------------------------------------------ */

/* Registers sit at byte 3 of their word. */
static uint8_t *glue_reg(SonicHDSPState *h, uint32_t off)
{
    if ((off & 3) != 3) {
        return NULL;
    }
    switch (off & ~3u) {
    case GLUE_RESET:
        return &h->glue_reset;
    case GLUE_CMD:
        return &h->glue_cmd;
    case GLUE_INTREQ:
        return &h->glue_intreq;
    case GLUE_ENABLE0:
        return &h->glue_enable[0];
    case GLUE_ENABLE1:
        return &h->glue_enable[1];
    case GLUE_UNK_080000:
        return &h->glue_080000;
    case GLUE_UNK_038000:
        return &h->glue_038000;
    default:
        return NULL;
    }
}

static uint8_t glue_read8(SonicHDSPState *h, uint32_t off)
{
    if (off == GLUE_BOOT_MODE + 3) {
        return 0x80;        /* boot-mode switch: start the system */
    }
    uint8_t *r = glue_reg(h, off);

    if (!r) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: glue logic read at "
                      "0x%06x\n", off);
        return 0;
    }
    return *r;
}

static void glue_write8(SonicHDSPState *h, uint32_t off, uint8_t val)
{
    if (off == 3) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: boot progress code %u\n",
                      val);
        return;
    }
    if (off == GLUE_BOOT_MODE + 3) {
        return;
    }
    uint8_t *r = glue_reg(h, off);

    if (!r) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: glue logic write 0x%02x at "
                      "0x%06x\n", val, off);
        return;
    }
    if (r == &h->glue_intreq) {
        return;     /* driven by the DSPs' interrupt lines */
    }
    if (r == &h->glue_reset) {
        uint8_t old = h->glue_reset;

        h->glue_reset = val & 0xf;
        for (unsigned n = 0; n < SONIC_HDSP_NUM_DSP; n++) {
            bool was = old & (1u << n), now = h->glue_reset & (1u << n);

            if (was != now) {
                sonic_hdsp_dsp_reset_line(h, n, now);
            }
        }
        return;
    }
    *r = val;
    sonic_hdsp_update_irq(h);
}

static uint32_t glue_read(SonicHDSPState *h, uint32_t off, unsigned size)
{
    uint32_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        v |= (uint32_t)glue_read8(h, off + i) << (8 * i);
    }
    return v;
}

static void glue_write(SonicHDSPState *h, uint32_t off, uint32_t val,
                       unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        glue_write8(h, off + i, val >> (8 * i));
    }
}

static uint32_t bank1_base(SonicHDSPState *h)
{
    return h->mc[MC_BANK1_BASE] & ~0xfffu;
}

/* ------------------------------------------------------------------ */
/* Local bus                                                          */
/* ------------------------------------------------------------------ */

uint32_t sonic_hdsp_local_read(SonicHDSPState *h, uint32_t addr, unsigned size)
{
    uint32_t off = addr - LOCAL_DRAM_BASE;
    uint32_t b1;

    if (addr >= LOCAL_DRAM_BASE && off < h->dram_size) {
        return ldn_le_p(h->dram_ptr + off, size);
    }
    if (addr - LOCAL_BANK0_BASE < LOCAL_BANK0_SIZE) {
        /* Bank 0: the 512 KiB flash repeats inside the 16 MiB bank. */
        return flash_read(h, addr & (SONIC_HDSP_FLASH_SIZE - 1), size);
    }
    if (addr >= LOCAL_PMMR_BASE && addr < LOCAL_PMMR_END) {
        return pmmr_read(h, addr, size);
    }
    if (addr >= 0xff000300u && addr <= 0xff000318u) {
        return timer_reg_read(h, addr);
    }
    if (addr - LOCAL_CORE_MMR < sizeof(h->core_mmr)) {
        return h->core_mmr[(addr - LOCAL_CORE_MMR) / 4];
    }
    if (addr - LOCAL_LMCON < sizeof(h->lmcon)) {
        return h->lmcon[(addr - LOCAL_LMCON) / 4];
    }
    b1 = bank1_base(h);
    if (b1 >= LOCAL_DRAM_BASE && addr - b1 < LOCAL_BANK1_SIZE) {
        return glue_read(h, addr - b1, size);
    }
    if (addr - LOCAL_UART_BASE < 0x100000u) {
        unsigned n = (addr >> 16) & 0xf;

        if ((addr & 0xffff) != 3 || size != 1) {
            local_unclaimed(h, "read", addr, size, 0);
            return 0xffffffffu >> (32 - 8 * size);
        }
        if (n == 9) {
            /* Transmitter ready and empty always; receive only with input. */
            return 0x0c | (h->uart_rx_n ? 0x01 : 0);
        }
        if (n == 0xb) {
            uint8_t c = h->uart_rx_n ? h->uart_rx[0] : 0;

            if (h->uart_rx_n) {
                memmove(h->uart_rx, h->uart_rx + 1, --h->uart_rx_n);
            }
            return c;
        }
        if (n != 0xb) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: byte register %u read\n",
                          n);
        }
        return h->uart_regs[n];
    }
    if (addr == LOCAL_BOARD_REG) {
        return 0x10;
    }
    if (addr - LOCAL_TABLES_BASE < LOCAL_TABLES_SIZE) {
        /* Outside every bank: undriven data, no fault, LPISR bit 5 latches. */
        h->mc[0x3c / 4] |= 0x20;
        local_unclaimed(h, "read", addr, size, 0);
        return 0xffffffffu >> (32 - 8 * size);
    }
    if (addr - LOCAL_POUT_BASE < LOCAL_OUT_SIZE) {
        uint8_t buf[8] = { 0 };
        uint32_t pci = (h->atu_x[(0x1254 - 0x1240) / 4] & 0xfc000000u) |
                       (addr & (LOCAL_OUT_SIZE - 1));

        if (pci_dma_read(PCI_DEVICE(h->atu), pci, buf, size) != MEMTX_OK) {
            return 0xffffffffu >> (32 - 8 * size);
        }
        return ldn_le_p(buf, size);
    }
    if (addr - LOCAL_SOUT_BASE < LOCAL_OUT_SIZE) {
        uint8_t buf[8] = { 0 };
        uint32_t pci = (h->atu_x[(0x1268 - 0x1240) / 4] & 0xfc000000u) |
                       (addr & (LOCAL_OUT_SIZE - 1));

        if (address_space_read(&PCI_BRIDGE(h)->as_mem, pci,
                               MEMTXATTRS_UNSPECIFIED, buf, size) != MEMTX_OK) {
            return 0xffffffffu >> (32 - 8 * size);
        }
        return ldn_le_p(buf, size);
    }
    local_unclaimed(h, "read", addr, size, 0);
    return 0xffffffffu >> (32 - 8 * size);
}

void sonic_hdsp_local_write(SonicHDSPState *h, uint32_t addr, uint32_t val,
                            unsigned size)
{
    uint32_t off = addr - LOCAL_DRAM_BASE;
    uint32_t b1;

    if (addr >= LOCAL_DRAM_BASE && off < h->dram_size) {
        stn_le_p(h->dram_ptr + off, size, val);
        memory_region_set_dirty(&h->dram, off, size);
        return;
    }
    if (addr - LOCAL_BANK0_BASE < LOCAL_BANK0_SIZE) {
        if (size != 1) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: %u-byte flash write "
                          "refused\n", size);
            return;
        }
        flash_write8(h, addr & (SONIC_HDSP_FLASH_SIZE - 1), val);
        return;
    }
    if (addr >= LOCAL_PMMR_BASE && addr < LOCAL_PMMR_END) {
        pmmr_write(h, addr, val, size);
        return;
    }
    if (addr >= 0xff000300u && addr <= 0xff000318u) {
        timer_reg_write(h, addr, val);
        return;
    }
    if (addr - LOCAL_CORE_MMR < sizeof(h->core_mmr)) {
        h->core_mmr[(addr - LOCAL_CORE_MMR) / 4] = val;
        return;
    }
    if (addr - LOCAL_LMCON < sizeof(h->lmcon)) {
        h->lmcon[(addr - LOCAL_LMCON) / 4] = val;
        return;
    }
    b1 = bank1_base(h);
    if (b1 >= LOCAL_DRAM_BASE && addr - b1 < LOCAL_BANK1_SIZE) {
        glue_write(h, addr - b1, val, size);
        return;
    }
    if (addr - LOCAL_UART_BASE < 0x100000u) {
        unsigned n = (addr >> 16) & 0xf;

        if ((addr & 0xffff) != 3 || size != 1) {
            local_unclaimed(h, "write", addr, size, val);
            return;
        }
        if (n == 0xb) {
            uint8_t c = val;

            qemu_chr_fe_write_all(&h->iop_console, &c, 1);
        } else if (n != 9) {
            qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: byte register %u "
                          "write 0x%02x\n", n, (uint8_t)val);
        }
        h->uart_regs[n] = val;
        return;
    }
    if (addr == LOCAL_BOARD_REG) {
        return;     /* the board-identification byte ignores writes */
    }
    if (addr - LOCAL_TABLES_BASE < LOCAL_TABLES_SIZE) {
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: write to 0x%08x dropped "
                      "(pc 0x%08x)\n", addr, h->cpu.cur_ip);
        return;
    }
    if (addr - LOCAL_POUT_BASE < LOCAL_OUT_SIZE) {
        uint8_t buf[8] = { 0 };
        uint32_t pci = (h->atu_x[(0x1254 - 0x1240) / 4] & 0xfc000000u) |
                       (addr & (LOCAL_OUT_SIZE - 1));

        stn_le_p(buf, size, val);
        pci_dma_write(PCI_DEVICE(h->atu), pci, buf, size);
        return;
    }
    if (addr - LOCAL_SOUT_BASE < LOCAL_OUT_SIZE) {
        uint8_t buf[8] = { 0 };
        uint32_t pci = (h->atu_x[(0x1268 - 0x1240) / 4] & 0xfc000000u) |
                       (addr & (LOCAL_OUT_SIZE - 1));

        stn_le_p(buf, size, val);
        address_space_write(&PCI_BRIDGE(h)->as_mem, pci,
                            MEMTXATTRS_UNSPECIFIED, buf, size);
        return;
    }
    local_unclaimed(h, "write", addr, size, val);
}

static uint32_t iop_read(void *opaque, uint32_t addr, unsigned size)
{
    return sonic_hdsp_local_read(opaque, addr, size);
}

static void iop_write(void *opaque, uint32_t addr, uint32_t val,
                      unsigned size)
{
    sonic_hdsp_local_write(opaque, addr, val, size);
}

static uint32_t iop_fetch(void *opaque, uint32_t addr)
{
    SonicHDSPState *h = opaque;
    uint32_t off = addr - LOCAL_DRAM_BASE;

    if (addr >= LOCAL_DRAM_BASE && off < h->dram_size) {
        return ldl_le_p(h->dram_ptr + off);
    }
    if (addr - LOCAL_BANK0_BASE < LOCAL_BANK0_SIZE &&
        !h->fl_loading && !h->fl_committing) {
        return ldl_le_p(h->flash + (addr & (SONIC_HDSP_FLASH_SIZE - 1)));
    }
    return sonic_hdsp_local_read(h, addr, 4);
}

static int uart_can_read(void *opaque)
{
    SonicHDSPState *h = opaque;

    return sizeof(h->uart_rx) - h->uart_rx_n;
}

static void uart_read(void *opaque, const uint8_t *buf, int size)
{
    SonicHDSPState *h = opaque;

    for (int i = 0; i < size && h->uart_rx_n < sizeof(h->uart_rx); i++) {
        h->uart_rx[h->uart_rx_n++] = buf[i];
    }
}

static void iop_system_error(void *opaque, uint32_t code)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: i960 stopped, fail code "
                  "0x%08x\n", code);
}

static const I960Ops iop_ops = {
    .read = iop_read,
    .write = iop_write,
    .fetch = iop_fetch,
    .system_error = iop_system_error,
};

/* ------------------------------------------------------------------ */
/* Slice timer                                                        */
/* ------------------------------------------------------------------ */

static void iop_slice(void *opaque)
{
    SonicHDSPState *h = opaque;
    int64_t now = now_ns();
    int64_t elapsed = MIN(now - h->iop_last_ns, MAX_CATCHUP_NS);
    int64_t next, t;

    h->iop_last_ns = now;
    flash_sync(h, now);
    timers_sync(h, now);
    for (unsigned c = 0; c < 3; c++) {
        if (h->dma[c].csr & DMA_CSR_ACTIVE) {
            dma_run(h, c, true);
        }
    }
    if (!h->cpu.failed) {
        int64_t cycles = muldiv64(MAX(elapsed, SLICE_NS), h->iop_clock,
                                  NANOSECONDS_PER_SECOND);

        i960_run(&h->cpu, cycles);
    }
    next = (h->cpu.idle_hint || h->cpu.halted || h->cpu.failed) ?
           IDLE_SLICE_NS : SLICE_NS;
    t = timers_next(h, now);
    if (t >= 0 && t < next) {
        next = MAX(t, 1000);
    }
    timer_mod(h->iop_timer, now + next);
}

void sonic_hdsp_iop_kick(SonicHDSPState *h)
{
    if (h->iop_timer) {
        timer_mod(h->iop_timer, now_ns());
    }
}

/* ------------------------------------------------------------------ */
/* Realize, reset, migration                                          */
/* ------------------------------------------------------------------ */

void sonic_hdsp_iop_realize(SonicHDSPState *h, Error **errp)
{
    ERRP_GUARD();
    g_autofree char *contents = NULL;
    gsize length;
    GError *err = NULL;

    if (!h->flash_image) {
        error_setg(errp, "sgi-sonic-hdsp: flash-image is required; the "
                   "device carries no firmware");
        return;
    }
    if (!g_file_get_contents(h->flash_image, &contents, &length, &err)) {
        error_setg(errp, "sgi-sonic-hdsp: cannot read flash-image '%s': %s",
                   h->flash_image, err->message);
        g_error_free(err);
        return;
    }
    if (length != SONIC_HDSP_FLASH_SIZE) {
        error_setg(errp, "sgi-sonic-hdsp: flash-image '%s' is %zu bytes, "
                   "not %d", h->flash_image, (size_t)length,
                   SONIC_HDSP_FLASH_SIZE);
        return;
    }
    if (h->dram_size != 16 * MiB && h->dram_size != 32 * MiB &&
        h->dram_size != 64 * MiB) {
        error_setg(errp, "sgi-sonic-hdsp: dram-size must be 16, 32 or 64 MiB");
        return;
    }
    if (h->iop_clock < 8 || !h->dsp_clock) {
        error_setg(errp, "sgi-sonic-hdsp: iop-clock must be at least 8 Hz "
                   "(the timers divide by up to 8) and dsp-clock nonzero");
        return;
    }
    memcpy(h->flash, contents, length);
    h->flash_fd = -1;
    if (h->flash_persist) {
        h->flash_fd = qemu_open_old(h->flash_image, O_RDWR | O_BINARY);
        if (h->flash_fd < 0) {
            error_setg_errno(errp, errno, "sgi-sonic-hdsp: cannot open "
                             "flash-image '%s' for write-back",
                             h->flash_image);
            return;
        }
    }

    memory_region_init_ram(&h->dram, OBJECT(h), "sgi-sonic-hdsp.dram",
                           h->dram_size, errp);
    if (*errp) {
        return;
    }
    h->dram_ptr = memory_region_get_ram_ptr(&h->dram);

    memset(h->eeprom, 0xff, sizeof(h->eeprom));
    stl_le_p(h->eeprom, h->nv_word);
    qemu_chr_fe_set_handlers(&h->iop_console, uart_can_read, uart_read, NULL,
                             NULL, h, NULL, true);
    i960_init(&h->cpu, I960_VARIANT_RX, &iop_ops, h);
    h->cpu.device_id = PDIDR_VALUE;
    h->iop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, iop_slice, h);
}

void sonic_hdsp_iop_unrealize(SonicHDSPState *h)
{
    if (h->iop_timer) {
        timer_free(h->iop_timer);
        h->iop_timer = NULL;
    }
    if (h->flash_fd >= 0) {
        close(h->flash_fd);
        h->flash_fd = -1;
    }
}

/* Chip reset values, applied to the functions' configuration bytes. */
static void iop_reset_config(SonicHDSPState *h)
{
    uint8_t *b = bridge_cfg(h), *a = atu_cfg(h);

    pci_set_word(b + PCI_VENDOR_ID, 0x8086);
    pci_set_word(b + PCI_DEVICE_ID, 0x0960);
    b[PCI_CLASS_PROG] = 0;
    pci_set_word(b + PCI_CLASS_DEVICE, 0x0604);
    b[PCI_HEADER_TYPE] = 0x81;
    pci_set_word(b + 0x34, 0);
    pci_set_word(b + 0x36, 0);
    /* The configuration-cycle retry strap: set from reset. */
    pci_set_word(b + 0x40, 0x0004);
    pci_set_word(b + 0x42, 0);

    pci_set_word(a + PCI_VENDOR_ID, 0x8086);
    pci_set_word(a + PCI_DEVICE_ID, 0x1960);
    a[PCI_CLASS_PROG] = 0;
    pci_set_word(a + PCI_CLASS_DEVICE, 0x0580);
    a[PCI_HEADER_TYPE] = 0x00;
    pci_set_word(a + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(a + PCI_SUBSYSTEM_ID, 0);
    a[PCI_INTERRUPT_PIN] = 1;
    a[PCI_MIN_GNT] = 0;
    a[PCI_MAX_LAT] = 0;
}

void sonic_hdsp_iop_reset(SonicHDSPState *h)
{
    int64_t now = now_ns();

    timer_del(h->iop_timer);
    memset(h->dram_ptr, 0, h->dram_size);
    memory_region_set_dirty(&h->dram, 0, h->dram_size);

    h->fl_loading = h->fl_committing = h->fl_armed = false;
    h->fl_arm_protect = false;
    h->fl_seq = 0;
    h->fl_count = 0;

    memset(h->atu_x, 0, sizeof(h->atu_x));
    h->atu_x[0] = 0xff000000u;              /* inbound limit */
    atu_apply_limit(h);
    h->imr[0] = h->imr[1] = h->omr[0] = h->omr[1] = 0;
    h->idr = h->iisr = h->odr = h->oisr = 0;
    h->iimr = 0x1ff;
    h->oimr = 0xff;
    h->mucr = 0x2;
    h->qbar = 0;
    memset(h->qptr, 0, sizeof(h->qptr));
    h->iar = h->arsr = h->awr = 0;
    h->nmi_latched = false;
    memset(h->dma, 0, sizeof(h->dma));
    memset(h->mc, 0, sizeof(h->mc));
    h->lba[0] = h->lba[1] = h->reg_1610 = 0;
    memset(h->timer, 0, sizeof(h->timer));
    for (unsigned n = 0; n < 2; n++) {
        h->timer[n].start_ns = now;
    }
    memset(h->i2c, 0, sizeof(h->i2c));
    memset(h->core_mmr, 0, sizeof(h->core_mmr));
    memset(h->lmcon, 0, sizeof(h->lmcon));
    h->glue_reset = 0xf;
    h->glue_cmd = h->glue_080000 = h->glue_038000 = 0;
    h->glue_intreq = 0;
    h->glue_enable[0] = h->glue_enable[1] = 0;
    memset(h->uart_regs, 0, sizeof(h->uart_regs));
    h->uart_rx_n = 0;
    h->i2c_state = 0;
    h->iop_access = false;
    h->irq_level = false;

    iop_reset_config(h);

    h->cpu.cycles = 0;
    h->iop_last_ns = now;
    i960_reset(&h->cpu);
    timer_mod(h->iop_timer, now + SLICE_NS);
}

/* Virtual-time stamps travel as ages: the destination's clock may differ. */
void sonic_hdsp_iop_pre_save(SonicHDSPState *h)
{
    int64_t now = now_ns();

    for (unsigned n = 0; n < 2; n++) {
        h->mig_timer_age[n] = now - h->timer[n].start_ns;
    }
    h->mig_deadline_left = h->fl_deadline_ns - now;
    h->mig_busy_left = h->fl_busy_until_ns - now;
}

/* Reject stream values the model uses as indices or lengths. */
bool sonic_hdsp_iop_state_valid(SonicHDSPState *h)
{
    return h->fl_sector < SONIC_HDSP_FLASH_SIZE / 256 &&
           h->fl_last < SONIC_HDSP_FLASH_SIZE && h->fl_count <= 256 &&
           h->uart_rx_n <= sizeof(h->uart_rx) && h->i2c_state <= 3 &&
           h->i2c_ptr < sizeof(h->eeprom);
}

void sonic_hdsp_iop_post_load(SonicHDSPState *h)
{
    int64_t now = now_ns();

    for (unsigned n = 0; n < 2; n++) {
        h->timer[n].start_ns = now - h->mig_timer_age[n];
    }
    h->fl_deadline_ns = now + h->mig_deadline_left;
    h->fl_busy_until_ns = now + h->mig_busy_left;
    atu_apply_limit(h);
    h->iop_last_ns = now_ns();
    timer_mod(h->iop_timer, h->iop_last_ns + SLICE_NS);
    sonic_hdsp_update_irq(h);
}

/* Whether a host type 1 cycle to secondary device `slot` reaches it. */
bool sonic_hdsp_dsp_visible(SonicHDSPState *h, unsigned slot)
{
    uint16_t sisr = lduw_le_p(bridge_cfg(h) + 0x42) & 0x3ff;

    return !(sisr & (1u << slot));
}
