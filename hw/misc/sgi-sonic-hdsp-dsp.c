/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic Solutions HDSP PCI audio card: the four DSP56301 processors.
 *
 * Each DSP is a PCI function on the i960's private secondary bus (its HI32
 * host port) wrapped around the DSP56300 interpreter in hw/coproc/dsp56300.
 * This file adds what the core leaves out: the bootstrap, the HI32 DSP side,
 * the ESSI serial ports, the DMA controller, the triple timer and the
 * interrupt priority registers, plus the slice timer that runs the four
 * cores and the sample clock that frames the serial ports.
 *
 * Hardware reference: docs/system/mips/sgi/hardware/sonic-hdsp.rst
 * ("The four DSP56301 processors"). Register layouts are the DSP56301 User's
 * Manual (Rev. 4) and the DSP56300 Family Manual (Rev. 5).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bridge.h"
#include "hw/misc/sgi-sonic-hdsp.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define DSP_VENDOR_ID       0x1057      /* hardwired, DSP56301 CDID/CVID */
#define DSP_DEVICE_ID       0x1801
#define DSP_BAR_SIZE        (64 * KiB)

#define DSP_BOOT_ROM        0xff0000u   /* reset vector, mode C bootstrap */
#define DSP_PERIPH_BASE     0xffff80u

/* Host side. */
#define HSTR_TRDY           (1u << 0)
#define HSTR_HTRQ           (1u << 1)
#define HSTR_HRRQ           (1u << 2)
#define HCTR_HF0            (1u << 3)
#define HCVR_HC             (1u << 0)
#define HCVR_HNMI           (1u << 15)

/* DSP side: DCTR and DSR bits. */
#define DCTR_HCIE           (1u << 0)
#define DCTR_STIE           (1u << 1)
#define DCTR_SRIE           (1u << 2)
#define DCTR_HM_SHIFT       20
#define DCTR_HM_PCI         1u

/* ESSI. */
#define CRA_WL_SHIFT        19
#define CRA_DC_SHIFT        12
#define CRB_REIE            (1u << 23)
#define CRB_TEIE            (1u << 22)
#define CRB_RIE             (1u << 19)
#define CRB_TIE             (1u << 18)
#define CRB_RE              (1u << 17)
#define CRB_TE0             (1u << 16)
#define CRB_TE1             (1u << 15)
#define CRB_TE2             (1u << 14)
#define CRB_MOD             (1u << 13)
#define SSISR_RDF           (1u << 7)
#define SSISR_TDE           (1u << 6)
#define SSISR_ROE           (1u << 5)
#define SSISR_TUE           (1u << 4)
#define SSISR_RFS           (1u << 3)
#define SSISR_TFS           (1u << 2)

/* DMA control register. */
#define DCR_DE              (1u << 23)
#define DCR_DIE             (1u << 22)

/* Timer control/status register. */
#define TCSR_TCF            (1u << 21)
#define TCSR_TOF            (1u << 20)
#define TCSR_PCE            (1u << 15)
#define TCSR_DO             (1u << 13)
#define TCSR_DIR            (1u << 11)
#define TCSR_TRM            (1u << 9)
#define TCSR_INV            (1u << 8)
#define TCSR_TCIE           (1u << 2)
#define TCSR_TOIE           (1u << 1)
#define TCSR_TE             (1u << 0)

/*
 * The slice the cores run in. Host accesses that could change what a core
 * sees are handled when they happen, so the slice only paces execution.
 */
#define DSP_SLICE_NS        (100 * 1000)
#define DSP_MAX_CATCHUP_NS  (2 * 1000 * 1000)
#define DSP_MAX_FRAMES_PER_SLICE 64

/* A per-slice, per-slot or per-word condition would flood the log. */
#define LOG_UNIMP_ONCE(...) \
    do { \
        static bool once_; \
        if (!once_) { \
            once_ = true; \
            qemu_log_mask(LOG_UNIMP, __VA_ARGS__); \
        } \
    } while (0)

static inline int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                         */
/* ------------------------------------------------------------------ */

/* The two IPL bits of a priority field: 0 disables, 1 to 3 give IPL 0 to 2. */
static void dsp_irq_level(SonicHDSPDSPState *s, uint32_t vector,
                          unsigned field, bool active)
{
    if (active && field) {
        dsp56300_irq_raise_level(&s->core, vector, field - 1);
    } else {
        dsp56300_irq_lower(&s->core, vector);
    }
}

static bool dsp_srrq(SonicHDSPDSPState *s)
{
    return s->tx_count > 0;
}

static bool dsp_strq(SonicHDSPDSPState *s)
{
    return s->rx_count < SONIC_DSP_FIFO_DEPTH;
}

/*
 * A timer's compare and overflow flags clear when the core services their
 * interrupt (DSP56301 manual, pp. 9-4 and 9-27): the request is raised once,
 * and the flag goes when the core has taken it.
 */
static void timer_irq(SonicHDSPDSPState *s, unsigned n, unsigned field)
{
    static const struct { uint32_t flag, enable; unsigned vec; } src[2] = {
        { TCSR_TCF, TCSR_TCIE, 0x24 }, { TCSR_TOF, TCSR_TOIE, 0x26 },
    };
    SonicDspTimer *t = &s->timer[n];

    for (unsigned k = 0; k < 2; k++) {
        uint32_t vec = src[k].vec + 4 * n;
        bool want = (t->tcsr & src[k].enable) && (t->tcsr & src[k].flag) &&
                    field;

        if (t->irq_out & (1u << k)) {
            if (!want) {
                dsp56300_irq_lower(&s->core, vec);
                t->irq_out &= ~(1u << k);
            } else if (!s->core.irq_pending[vec >> 1]) {
                t->tcsr &= ~src[k].flag;        /* taken: the flag clears */
                t->irq_out &= ~(1u << k);
            }
        } else if (want) {
            dsp56300_irq_raise(&s->core, vec, field - 1);
            t->irq_out |= 1u << k;
        }
    }
}

/* Recompute every level-triggered request from the peripheral state. */
static void dsp_update_irqs(SonicHDSPDSPState *s)
{
    unsigned hpl = s->ipr_p & 3, tpl = (s->ipr_p >> 8) & 3;

    for (unsigned n = 0; n < SONIC_DSP_NESSI; n++) {
        SonicDspEssi *e = &s->essi[n];
        unsigned pl = (s->ipr_p >> (2 + 2 * n)) & 3;
        uint32_t base = 0x30 + 0x10 * n;

        /* With an exception pending only the exception vector is taken. */
        bool rx_exc = (e->crb & CRB_REIE) && (e->ssisr & SSISR_ROE);
        bool tx_exc = (e->crb & CRB_TEIE) && (e->ssisr & SSISR_TUE);

        dsp_irq_level(s, base + 0x00, pl, (e->crb & CRB_RIE) &&
                      (e->ssisr & SSISR_RDF) && !rx_exc);
        dsp_irq_level(s, base + 0x02, pl, rx_exc);
        dsp_irq_level(s, base + 0x06, pl, (e->crb & CRB_TIE) &&
                      (e->ssisr & SSISR_TDE) && !tx_exc);
        dsp_irq_level(s, base + 0x08, pl, tx_exc);
    }
    dsp_irq_level(s, 0x6a, hpl, (s->dctr & DCTR_SRIE) && dsp_srrq(s));
    dsp_irq_level(s, 0x6e, hpl, (s->dctr & DCTR_STIE) && dsp_strq(s));
    for (unsigned n = 0; n < SONIC_DSP_NTIMER; n++) {
        timer_irq(s, n, tpl);
    }
}

/*
 * A host command: HV gives the vector (twice its value), HNMI makes it
 * non-maskable and ignores HCIE. The request is taken once; the pending
 * flag clears when the core takes it, which dsp_post_run notices.
 */
static void dsp_hc_raise(SonicHDSPDSPState *s)
{
    uint32_t vec = s->hcvr & 0xfe;
    unsigned hpl = s->ipr_p & 3;

    if (!(s->hcvr & HCVR_HC) || s->hc_raised) {
        return;
    }
    if (s->hcvr & HCVR_HNMI) {
        dsp56300_irq_raise(&s->core, vec, 3);
        s->hc_raised = 1;
    } else if ((s->dctr & DCTR_HCIE) && hpl) {
        dsp56300_irq_raise(&s->core, vec, hpl - 1);
        s->hc_raised = 1;
    }
}

static void dsp_hc_check(SonicHDSPDSPState *s)
{
    if (s->hc_raised && !s->core.irq_pending[(s->hcvr & 0xfe) >> 1]) {
        s->hc_raised = 0;
        s->hcvr &= ~HCVR_HC;        /* cleared by the interrupt logic */
    }
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

static uint32_t dsp_ext_read(void *opaque, DSP56300Space space, uint32_t addr);
static void dsp_ext_write(void *opaque, DSP56300Space space, uint32_t addr,
                          uint32_t val);

/* A DMA access: on-chip arrays first, then the same path the core uses. */
static uint32_t dsp_mem_read(SonicHDSPDSPState *s, unsigned space,
                             uint32_t addr)
{
    const uint32_t *mem[3] = { s->xmem, s->ymem, s->pmem };
    static const uint32_t words[3] = {
        SONIC_DSP_XMEM_WORDS, SONIC_DSP_YMEM_WORDS, SONIC_DSP_PMEM_WORDS,
    };

    if (addr < words[space]) {
        return mem[space][addr];
    }
    return dsp_ext_read(s, space, addr);
}

static void dsp_mem_write(SonicHDSPDSPState *s, unsigned space, uint32_t addr,
                          uint32_t val)
{
    uint32_t *mem[3] = { s->xmem, s->ymem, s->pmem };
    static const uint32_t words[3] = {
        SONIC_DSP_XMEM_WORDS, SONIC_DSP_YMEM_WORDS, SONIC_DSP_PMEM_WORDS,
    };

    if (addr < words[space]) {
        mem[space][addr] = val & 0xffffff;
        return;
    }
    dsp_ext_write(s, space, addr, val);
}

/* ------------------------------------------------------------------ */
/* ESSI                                                               */
/* ------------------------------------------------------------------ */

static unsigned essi_wl(const SonicDspEssi *e)
{
    switch ((e->cra >> CRA_WL_SHIFT) & 7) {
    case 0:
        return 8;
    case 1:
        return 12;
    case 2:
        return 16;
    case 4:
        LOG_UNIMP_ONCE("sgi-sonic-hdsp: ESSI 32-bit words\n");
        /* fall through */
    default:
        return 24;
    }
}

/* Bit n of the result: transmitter n is enabled. */
static uint32_t essi_te(const SonicDspEssi *e)
{
    return ((e->crb & CRB_TE0) ? 1 : 0) | ((e->crb & CRB_TE1) ? 2 : 0) |
           ((e->crb & CRB_TE2) ? 4 : 0);
}

/* Where a word an ESSI shifts out goes, and where its input comes from. */
static void essi_link_tx(SonicHDSPDSPState *s, unsigned essi, uint32_t word,
                         unsigned wl)
{
    SonicHDSPState *h = s->hub;

    if (s->index == 2 && essi == 0 && h->box) {
        sonic_ada8824_tx(h->box, word, wl);
    } else if (essi == 1 && h->essi1_chain &&
               s->index + 1 < SONIC_HDSP_NUM_DSP) {
        SonicDspEssi *peer = &h->dsp[s->index + 1]->essi[1];

        peer->in_word = word << (24 - wl);
        peer->in_valid = 1;
    }
}

static bool essi_link_rx(SonicHDSPDSPState *s, unsigned essi, uint32_t *word,
                         unsigned wl)
{
    SonicHDSPState *h = s->hub;
    SonicDspEssi *e = &s->essi[essi];

    if (s->index == 2 && essi == 0 && h->box) {
        return sonic_ada8824_rx(h->box, word, wl);
    }
    if (essi == 1 && h->essi1_chain && e->in_valid) {
        e->in_valid = 0;
        *word = e->in_word >> (24 - wl);
        return true;
    }
    return false;
}

static void dma_service(SonicHDSPDSPState *s);

static void essi_slot(SonicHDSPDSPState *s, unsigned n, unsigned slot)
{
    SonicDspEssi *e = &s->essi[n];
    unsigned wl = essi_wl(e);
    bool network = e->crb & CRB_MOD;
    uint32_t mask = (e->tsma & 0xffff) | ((e->tsmb & 0xffff) << 16);
    uint32_t rmask = (e->rsma & 0xffff) | ((e->rsmb & 0xffff) << 16);
    uint32_t te = essi_te(e);

    if (te) {
        e->ssisr &= ~SSISR_TFS;
        if (slot == 0) {
            e->ssisr |= SSISR_TFS;
        }
        if (network && !((mask >> slot) & 1)) {
            e->ssisr |= SSISR_TDE;          /* an inactive slot */
        } else if (e->tsr_skip) {
            e->tsr_skip = 0;
            e->ssisr |= SSISR_TDE;
        } else {
            if ((e->tx_written & te) != te) {
                e->ssisr |= SSISR_TUE;      /* the old word goes out again */
                e->sr_read = 0;
            }
            if (te & ~1u) {
                LOG_UNIMP_ONCE("sgi-sonic-hdsp: ESSI transmitters 1 and 2 "
                               "are not modelled\n");
            }
            essi_link_tx(s, n, (e->tx[0] >> (24 - wl)) & ((1u << wl) - 1), wl);
            e->tx_written = 0;
            e->ssisr |= SSISR_TDE;
        }
    }
    if ((e->crb & CRB_RE) && (!network || ((rmask >> slot) & 1))) {
        uint32_t w;

        if (essi_link_rx(s, n, &w, wl)) {
            if (e->ssisr & SSISR_RDF) {
                e->ssisr |= SSISR_ROE;
                e->sr_read = 0;
            } else {
                e->rx = (w << (24 - wl)) & 0xffffff;
                e->ssisr |= SSISR_RDF;
            }
            e->ssisr = (e->ssisr & ~SSISR_RFS) | (slot ? 0 : SSISR_RFS);
        }
    }
    dsp_update_irqs(s);
    dma_service(s);
}

/* One frame sync: every active slot of both ports. */
static void essi_frame(SonicHDSPDSPState *s)
{
    for (unsigned n = 0; n < SONIC_DSP_NESSI; n++) {
        SonicDspEssi *e = &s->essi[n];
        unsigned slots = 1;

        if (!essi_te(e) && !(e->crb & CRB_RE)) {
            continue;
        }
        if (e->crb & CRB_MOD) {
            slots = ((e->cra >> CRA_DC_SHIFT) & 0x1f) + 1;
        }
        for (unsigned slot = 0; slot < slots; slot++) {
            essi_slot(s, n, slot);
        }
    }
}

static uint32_t essi_read(SonicHDSPDSPState *s, unsigned n, unsigned off)
{
    SonicDspEssi *e = &s->essi[n];
    uint32_t v;

    switch (off) {
    case 0x8:
        v = e->rx;
        e->ssisr &= ~SSISR_RDF;
        if (e->sr_read) {
            e->ssisr &= ~SSISR_ROE;
        }
        dsp_update_irqs(s);
        return v;
    case 0x7:
        e->sr_read = 1;
        return e->ssisr;
    case 0xc:
    case 0xb:
    case 0xa:
        return e->tx[0xc - off];
    case 0x9:
        return 0;                       /* TSR is write-only */
    case 0x6:
        return e->crb;
    case 0x5:
        return e->cra;
    case 0x4:
        return e->tsma;
    case 0x3:
        return e->tsmb;
    case 0x2:
        return e->rsma;
    case 0x1:
        return e->rsmb;
    case 0xf:
        return e->pcr;
    case 0xe:
        return e->prr;
    case 0xd:
        return e->pdr;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u ESSI%u read of "
                      "offset 0x%x\n", s->index, n, off);
        return 0;
    }
}

static void essi_write(SonicHDSPDSPState *s, unsigned n, unsigned off,
                       uint32_t val)
{
    SonicDspEssi *e = &s->essi[n];
    uint32_t old_te = essi_te(e);

    val &= 0xffffff;
    switch (off) {
    case 0xc:
    case 0xb:
    case 0xa:
        e->tx[0xc - off] = val;
        e->tx_written |= 1u << (0xc - off);
        if ((e->tx_written & essi_te(e)) == essi_te(e)) {
            e->ssisr &= ~SSISR_TDE;
            if (e->sr_read) {
                e->ssisr &= ~SSISR_TUE;
            }
        }
        break;
    case 0x9:
        e->tsr_skip = 1;
        e->ssisr &= ~SSISR_TDE;
        if (e->sr_read) {
            e->ssisr &= ~SSISR_TUE;
        }
        break;
    case 0x6:
        e->crb = val;
        if (essi_te(e) && !old_te) {
            e->ssisr |= SSISR_TDE;          /* an empty register to fill */
        } else if (!essi_te(e)) {
            e->ssisr &= ~(SSISR_TDE | SSISR_TUE | SSISR_TFS);
            e->tx_written = 0;
        }
        if (!(e->crb & CRB_RE)) {
            e->ssisr &= ~(SSISR_RDF | SSISR_ROE | SSISR_RFS);
        }
        break;
    case 0x5:
        e->cra = val;
        break;
    case 0x4:
        e->tsma = val;
        break;
    case 0x3:
        e->tsmb = val;
        break;
    case 0x2:
        e->rsma = val;
        break;
    case 0x1:
        e->rsmb = val;
        break;
    case 0xf:
        e->pcr = val;
        break;
    case 0xe:
        e->prr = val;
        break;
    case 0xd:
        e->pdr = val;
        break;
    case 0x8:
    case 0x7:
        break;                              /* read-only */
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u ESSI%u write of "
                      "offset 0x%x\n", s->index, n, off);
        break;
    }
    dsp_update_irqs(s);
    dma_service(s);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                */
/* ------------------------------------------------------------------ */

/* Whether the request source in a DRS field is asserting. */
static bool dma_request(SonicHDSPDSPState *s, unsigned drs)
{
    switch (drs) {
    case 0x0a:
        return s->essi[0].ssisr & SSISR_RDF;
    case 0x0b:
        return s->essi[0].ssisr & SSISR_TDE;
    case 0x0c:
        return s->essi[1].ssisr & SSISR_RDF;
    case 0x0d:
        return s->essi[1].ssisr & SSISR_TDE;
    case 0x10:
    case 0x11:
    case 0x12:
        return s->timer[drs - 0x10].tcsr & TCSR_TCF;
    case 0x1c:
        return dsp_srrq(s);
    case 0x1e:
        return dsp_strq(s);
    default:
        LOG_UNIMP_ONCE("sgi-sonic-hdsp: DMA request source 0x%x\n", drs);
        return false;
    }
}

static void dma_done(SonicHDSPDSPState *s, unsigned ch)
{
    SonicDspDma *d = &s->dma[ch];
    unsigned dtm = (d->dcr >> 19) & 7;
    unsigned field = (s->ipr_c >> (12 + 2 * ch)) & 3;

    d->blk_active = 0;
    s->dtd |= 1u << ch;
    if (dtm < 4) {
        d->dcr &= ~DCR_DE;
    }
    if ((d->dcr & DCR_DIE) && field) {
        dsp56300_irq_raise(&s->core, 0x18 + 2 * ch, field - 1);
    }
}

static void dma_word(SonicHDSPDSPState *s, unsigned ch)
{
    SonicDspDma *d = &s->dma[ch];
    unsigned sspace = d->dcr & 3, dspace = (d->dcr >> 2) & 3;
    unsigned dam = (d->dcr >> 4) & 0x3f;
    unsigned smode = dam & 7, dmode = (dam >> 3) & 7;
    uint32_t val;

    if (sspace == 3 || dspace == 3) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: DSP %u DMA channel "
                      "%u uses a reserved memory space\n", s->index, ch);
        d->dcr &= ~DCR_DE;
        return;
    }
    if ((d->dcr & (1u << 10)) || smode < 4 || dmode < 4) {
        LOG_UNIMP_ONCE("sgi-sonic-hdsp: DMA multi-dimensional addressing\n");
    }
    if (smode > 5 || dmode > 5) {
        LOG_UNIMP_ONCE("sgi-sonic-hdsp: reserved DMA address mode\n");
    }
    val = dsp_mem_read(s, sspace, d->dsr);
    dsp_mem_write(s, dspace, d->ddr, val);
    /* Modes 0 to 3 (2D) are treated as postincrement; 4 holds, 6 and 7 hold. */
    if (smode < 4 || smode == 5) {
        d->dsr = (d->dsr + 1) & 0xffffff;
    }
    if (dmode < 4 || dmode == 5) {
        d->ddr = (d->ddr + 1) & 0xffffff;
    }
    if (d->dco) {
        d->dco--;
    } else {
        d->dco = d->dco_init;
        dma_done(s, ch);
    }
}

/* Move words for every channel whose trigger is asserting. */
static void dma_service(SonicHDSPDSPState *s)
{
    if (s->dma_busy) {
        return;             /* a write made by DMA: the loop sees it */
    }
    s->dma_busy = 1;
    for (unsigned guard = 0; guard < 1u << 16; guard++) {
        bool progress = false;

        for (unsigned ch = 0; ch < SONIC_DSP_NDMA; ch++) {
            SonicDspDma *d = &s->dma[ch];
            unsigned dtm = (d->dcr >> 19) & 7;
            bool go;

            if (!(d->dcr & DCR_DE)) {
                continue;
            }
            if (dtm == 3 || d->blk_active) {
                go = true;
            } else {
                go = dma_request(s, (d->dcr >> 11) & 0x1f);
            }
            if (!go) {
                continue;
            }
            if (dtm == 0 || dtm == 4) {
                d->blk_active = 1;
            }
            if (!d->blk_active && ((d->dcr >> 11) & 0x1f) >= 0x10 &&
                ((d->dcr >> 11) & 0x1f) <= 0x12) {
                /* A timer's compare flag is the request; serving clears it. */
                s->timer[((d->dcr >> 11) & 0x1f) - 0x10].tcsr &= ~TCSR_TCF;
            }
            if (dtm == 6 || dtm == 7) {
                d->dcr &= ~DCR_DE;      /* reserved transfer mode */
                continue;
            }
            dma_word(s, ch);
            progress = true;
        }
        if (!progress) {
            break;
        }
    }
    s->dma_busy = 0;
    dsp_update_irqs(s);
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */
/* ------------------------------------------------------------------ */

static void timer_line(SonicDspTimer *t, SonicHDSPDSPState *s, unsigned n)
{
    SonicHDSPState *h = s->hub;
    bool gpio_out = !((t->tcsr >> 4) & 0xf) && (t->tcsr & TCSR_DIR);
    bool level;

    /* Only timer 0's output reaches the board: the interrupt-out line. */
    if (n != 0 || !gpio_out) {
        return;
    }
    level = !!(t->tcsr & TCSR_DO) != !!(t->tcsr & TCSR_INV);
    if (level) {
        h->glue_intreq |= 1u << s->index;
    } else {
        h->glue_intreq &= ~(1u << s->index);
    }
    sonic_hdsp_update_irq(h);
}

/* Count events: one per two core cycles, as TCSR[PCE] = 0 selects CLK/2. */
static void timer_advance(SonicHDSPDSPState *s, unsigned n, uint64_t events)
{
    SonicDspTimer *t = &s->timer[n];
    unsigned mode = (t->tcsr >> 4) & 0xf;

    if (!(t->tcsr & TCSR_TE)) {
        return;
    }
    if (mode > 2 || (t->tcsr & TCSR_PCE)) {
        LOG_UNIMP_ONCE("sgi-sonic-hdsp: timer mode %u or the prescaler clock\n",
                       mode);
        return;
    }
    for (unsigned guard = 0; events && guard < 1u << 20; guard++) {
        uint64_t to_cmp = ((t->tcpr - t->tcr) & 0xffffff) + 1;
        uint64_t to_ovf = (0xffffff - t->tcr) + 1;
        uint64_t e = MIN(events, MIN(to_cmp, to_ovf));

        events -= e;
        if (e < to_cmp && e < to_ovf) {
            t->tcr = (t->tcr + e) & 0xffffff;
            break;
        }
        if (e == to_cmp) {
            t->tcsr |= TCSR_TCF;
            t->tcr = (t->tcsr & TCSR_TRM) ? t->tlr : (t->tcr + e) & 0xffffff;
            dsp_update_irqs(s);
            dma_service(s);     /* a request lasts until it is served */
        }
        if (e == to_ovf) {
            t->tcsr |= TCSR_TOF;
            t->tcr = 0;
        }
    }
}

static uint32_t timer_read(SonicHDSPDSPState *s, unsigned n, unsigned reg)
{
    SonicDspTimer *t = &s->timer[n];

    switch (reg) {
    case 0:
        return t->tcsr;
    case 1:
        return t->tlr;
    case 2:
        return t->tcpr;
    default:
        return t->tcr;
    }
}

static void timer_write(SonicHDSPDSPState *s, unsigned n, unsigned reg,
                        uint32_t val)
{
    SonicDspTimer *t = &s->timer[n];

    val &= 0xffffff;
    switch (reg) {
    case 0: {
        uint32_t old = t->tcsr;
        uint32_t flags = old & (TCSR_TCF | TCSR_TOF);

        flags &= ~(val & (TCSR_TCF | TCSR_TOF));    /* write one to clear */
        t->tcsr = (val & ~(TCSR_TCF | TCSR_TOF)) | flags;
        if ((t->tcsr & TCSR_TE) && !(old & TCSR_TE)) {
            t->tcr = t->tlr;                        /* enabling clears */
        } else if (!(t->tcsr & TCSR_TE)) {
            t->tcsr &= ~(TCSR_TCF | TCSR_TOF);
        }
        timer_line(t, s, n);
        break;
    }
    case 1:
        t->tlr = val;
        break;
    case 2:
        t->tcpr = val;
        break;
    default:
        t->tcr = val;
        break;
    }
    dsp_update_irqs(s);
    dma_service(s);
}

/* ------------------------------------------------------------------ */
/* HI32, DSP side                                                     */
/* ------------------------------------------------------------------ */

static uint32_t dsp_hstr(SonicHDSPDSPState *s)
{
    uint32_t v = 0;

    if (!s->tx_count) {
        v |= HSTR_TRDY;
    }
    if (s->tx_count < SONIC_DSP_FIFO_DEPTH) {
        v |= HSTR_HTRQ;
    }
    if (s->rx_count) {
        v |= HSTR_HRRQ;
    }
    v |= s->dctr & 0x78;        /* HINT, then HF5 to HF3 */
    return v;
}

static uint32_t dsp_dsr(SonicHDSPDSPState *s)
{
    uint32_t v = 0;

    if ((s->dctr >> DCTR_HM_SHIFT) & 7) {
        v |= 1u << 23;                      /* HACT */
    }
    v |= s->hctr & 0x38;                    /* HF2 to HF0 */
    if (dsp_srrq(s)) {
        v |= 1u << 2;
    }
    if (dsp_strq(s)) {
        v |= 1u << 1;
    }
    if (s->hcvr & HCVR_HC) {
        v |= 1u << 0;                       /* HCP */
    }
    return v;
}

static uint32_t dsp_host_to_dsp_pop(SonicHDSPDSPState *s)
{
    uint32_t v = 0;

    if (s->tx_count) {
        v = s->tx_fifo[0];
        memmove(s->tx_fifo, s->tx_fifo + 1,
                (--s->tx_count) * sizeof(uint32_t));
    } else if (s->boot == SONIC_DSP_BOOT_RUN) {
        /* The manual also sets SRRQ here; the model does not (the page). */
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: DSP %u read DRXR "
                      "with the receive FIFO empty\n", s->index);
    }
    return v;
}

static void dsp_dsp_to_host_push(SonicHDSPDSPState *s, uint32_t val)
{
    if (s->rx_count < SONIC_DSP_FIFO_DEPTH) {
        s->rx_fifo[s->rx_count++] = val & 0xffffff;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: DSP %u wrote DTXS "
                      "with the transmit FIFO full\n", s->index);
    }
}

/* ------------------------------------------------------------------ */
/* Bootstrap                                                          */
/* ------------------------------------------------------------------ */

static void dsp_boot_finish(SonicHDSPDSPState *s)
{
    uint32_t dctr = s->dctr;

    s->boot = SONIC_DSP_BOOT_RUN;
    dsp56300_reset(&s->core, s->boot_start);
    s->dctr = dctr;
}

/*
 * Mode C, as the bootstrap listing in the DSP56301 manual (appendix A) does
 * it: a word count, a start address, then one program word per host word
 * into program memory from that address; HF0 set with the FIFO empty ends
 * the load early. It runs as the host's words arrive, which is faster than
 * the ROM loop could take them.
 */
static void dsp_boot_step(SonicHDSPDSPState *s)
{
    while (s->boot != SONIC_DSP_BOOT_RUN) {
        uint32_t w;

        if (!s->tx_count) {
            if (s->boot == SONIC_DSP_BOOT_DATA && (s->hctr & HCTR_HF0)) {
                dsp_boot_finish(s);
            }
            return;
        }
        w = dsp_host_to_dsp_pop(s) & 0xffffff;
        switch (s->boot) {
        case SONIC_DSP_BOOT_COUNT:
            s->boot_count = w;
            s->boot = SONIC_DSP_BOOT_ADDR;
            break;
        case SONIC_DSP_BOOT_ADDR:
            s->boot_next = s->boot_start = w;
            s->boot = SONIC_DSP_BOOT_DATA;
            if (!s->boot_count) {
                dsp_boot_finish(s);
            }
            break;
        default:
            dsp_mem_write(s, DSP56300_SPACE_P, s->boot_next, w);
            s->boot_next = (s->boot_next + 1) & 0xffffff;
            if (!--s->boot_count) {
                dsp_boot_finish(s);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Peripheral registers (X:$FFFF80 and up)                            */
/* ------------------------------------------------------------------ */

/* Registers that only store, because nothing on the card depends on them. */
static bool periph_stored(uint32_t a)
{
    return (a >= 0xffff82 && a <= 0xffff83) ||      /* timer prescaler */
           (a >= 0xffff93 && a <= 0xffff9f) ||      /* SCI and port E */
           (a >= 0xffffc6 && a <= 0xffffc8) ||      /* DPCR, DPMC, DPAR */
           a == 0xffffca ||                         /* DPSR */
           a == 0xffffce || a == 0xffffcf ||        /* host port GPIO */
           (a >= 0xfffff5 && a <= 0xfffffd);        /* IDR, BIU, PLL */
}

static uint32_t periph_read(SonicHDSPDSPState *s, uint32_t a)
{
    if (a >= 0xffffa1 && a <= 0xffffaf) {
        return essi_read(s, 1, a - 0xffffa0);
    }
    if (a >= 0xffffb1 && a <= 0xffffbf) {
        return essi_read(s, 0, a - 0xffffb0);
    }
    if (a >= 0xffff84 && a <= 0xffff8f) {
        return timer_read(s, (0xffff8f - a) / 4, (0xffff8f - a) % 4);
    }
    if (a >= 0xffffd8 && a <= 0xffffef) {
        SonicDspDma *d = &s->dma[(0xffffef - a) / 4];

        switch ((a - 0xffffd8) % 4) {
        case 0:
            return d->dcr;
        case 1:
            return d->dco;
        case 2:
            return d->ddr;
        default:
            return d->dsr;
        }
    }
    switch (a) {
    case 0xfffff4:
        return s->dtd;                  /* DSTR: no transfer in progress */
    case 0xfffff3:
    case 0xfffff2:
    case 0xfffff1:
    case 0xfffff0:
        return s->dor[0xfffff3 - a];
    case 0xffffc5:
        return s->dctr;
    case 0xffffc9:
        return dsp_dsr(s);
    case 0xffffcb: {
        uint32_t v = dsp_host_to_dsp_pop(s);

        dsp_update_irqs(s);
        dma_service(s);
        return v;
    }
    case 0xffffff:
        return s->ipr_c;
    case 0xfffffe:
        return s->ipr_p;
    default:
        break;
    }
    if (periph_stored(a)) {
        return s->periph[a - DSP_PERIPH_BASE];
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u read of X:0x%06x\n",
                  s->index, a);
    return 0;
}

static void periph_write(SonicHDSPDSPState *s, uint32_t a, uint32_t val)
{
    val &= 0xffffff;
    if (a >= 0xffffa1 && a <= 0xffffaf) {
        essi_write(s, 1, a - 0xffffa0, val);
        return;
    }
    if (a >= 0xffffb1 && a <= 0xffffbf) {
        essi_write(s, 0, a - 0xffffb0, val);
        return;
    }
    if (a >= 0xffff84 && a <= 0xffff8f) {
        timer_write(s, (0xffff8f - a) / 4, (0xffff8f - a) % 4, val);
        return;
    }
    if (a >= 0xffffd8 && a <= 0xffffef) {
        unsigned ch = (0xffffef - a) / 4;
        SonicDspDma *d = &s->dma[ch];

        switch ((a - 0xffffd8) % 4) {
        case 0: {
            uint32_t old = d->dcr;

            d->dcr = val;
            if ((val & DCR_DE) && !(old & DCR_DE)) {
                s->dtd &= ~(1u << ch);
                d->blk_active = 0;
            } else if ((old & DCR_DE) && !(val & DCR_DE) && (val & DCR_DIE)) {
                /* Software stopping a channel interrupts too (DIE). */
                unsigned field = (s->ipr_c >> (12 + 2 * ch)) & 3;

                d->blk_active = 0;
                if (field) {
                    dsp56300_irq_raise(&s->core, 0x18 + 2 * ch, field - 1);
                }
            }
            break;
        }
        case 1:
            d->dco = d->dco_init = val;
            break;
        case 2:
            d->ddr = val;
            break;
        default:
            d->dsr = val;
            break;
        }
        dma_service(s);
        return;
    }
    switch (a) {
    case 0xfffff3:
    case 0xfffff2:
    case 0xfffff1:
    case 0xfffff0:
        s->dor[0xfffff3 - a] = val;
        return;
    case 0xffffc5:
        s->dctr = val & 0x7ff0ff;
        s->pci_mode = ((s->dctr >> DCTR_HM_SHIFT) & 7) == DCTR_HM_PCI;
        dsp_hc_raise(s);
        dsp_update_irqs(s);
        dma_service(s);
        return;
    case 0xffffcd:
        dsp_dsp_to_host_push(s, val);
        dsp_update_irqs(s);
        dma_service(s);
        return;
    case 0xffffcc:
        qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u HI32 master "
                      "transmit not modelled\n", s->index);
        return;
    case 0xffffff:
        s->ipr_c = val;
        dsp_update_irqs(s);
        return;
    case 0xfffffe:
        s->ipr_p = val;
        dsp_hc_raise(s);
        dsp_update_irqs(s);
        return;
    default:
        break;
    }
    if (periph_stored(a)) {
        if (a >= 0xffff93 && a <= 0xffff9c) {
            LOG_UNIMP_ONCE("sgi-sonic-hdsp: SCI write of X:0x%06x not "
                           "modelled\n", a);
        }
        s->periph[a - DSP_PERIPH_BASE] = val;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u write of X:0x%06x = "
                  "0x%06x\n", s->index, a, val);
}

static void dsp_periph_reset(void *opaque)
{
    SonicHDSPDSPState *s = opaque;

    s->ipr_c = s->ipr_p = 0;
    memset(s->dma, 0, sizeof(s->dma));
    memset(s->timer, 0, sizeof(s->timer));
    s->dtd = 0x3f;
    for (unsigned n = 0; n < SONIC_DSP_NESSI; n++) {
        SonicDspEssi *e = &s->essi[n];

        memset(e, 0, sizeof(*e));
        e->tsma = e->tsmb = e->rsma = e->rsmb = 0xffff;
    }
    dsp_update_irqs(s);
}

/* ------------------------------------------------------------------ */
/* The core's view of memory                                          */
/* ------------------------------------------------------------------ */

static uint32_t dsp_ext_read(void *opaque, DSP56300Space space, uint32_t addr)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t off = addr - SONIC_DSP_EXT_BASE;

    if (space == DSP56300_SPACE_X && addr >= DSP_PERIPH_BASE) {
        return periph_read(s, addr);
    }
    if (off < SONIC_DSP_EXT_WORDS) {
        if (space == DSP56300_SPACE_P) {
            return s->ext_p[off];
        }
        if (space == DSP56300_SPACE_Y) {
            return s->ext_y[off];
        }
    }
    if (space == DSP56300_SPACE_Y && addr >= 0xfffff7) {
        return s->hub->sync[addr - 0xfffff7];
    }
    if (space == DSP56300_SPACE_P && addr >= DSP_BOOT_ROM &&
        addr < DSP_BOOT_ROM + 0xc00) {
        return 0;               /* the ROM's code is the model's own */
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u read of %c:0x%06x\n",
                  s->index, "XYP"[space], addr);
    return 0;
}

static void dsp_ext_write(void *opaque, DSP56300Space space, uint32_t addr,
                          uint32_t val)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t off = addr - SONIC_DSP_EXT_BASE;

    if (space == DSP56300_SPACE_X && addr >= DSP_PERIPH_BASE) {
        periph_write(s, addr, val);
        return;
    }
    if (off < SONIC_DSP_EXT_WORDS) {
        if (space == DSP56300_SPACE_P) {
            s->ext_p[off] = val & 0xffffff;
            return;
        }
        if (space == DSP56300_SPACE_Y) {
            s->ext_y[off] = val & 0xffffff;
            return;
        }
    }
    if (space == DSP56300_SPACE_Y && addr >= 0xfffff7) {
        s->hub->sync[addr - 0xfffff7] = val & 0xffffff;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-sonic-hdsp: DSP %u write of %c:0x%06x = "
                  "0x%06x\n", s->index, "XYP"[space], addr, val);
}

static const DSP56300Ops dsp_ops = {
    .read = dsp_ext_read,
    .write = dsp_ext_write,
    .peripheral_reset = dsp_periph_reset,
};

/* ------------------------------------------------------------------ */
/* The host port on the secondary bus                                 */
/* ------------------------------------------------------------------ */

static uint64_t dsp_bar_read(void *opaque, hwaddr off, unsigned size)
{
    SonicHDSPDSPState *s = opaque;
    uint32_t v = 0;

    if (s->in_reset) {
        return UINT32_MAX >> (32 - 8 * size);
    }
    switch (off & ~3) {
    case 0x10:
        v = s->hctr;
        break;
    case 0x14:
        v = dsp_hstr(s);
        break;
    case 0x18:
        v = s->hcvr;
        break;
    default:
        if (off >= 0x1c) {
            /* Receive data port: the oldest word the DSP wrote. */
            if (s->rx_count) {
                v = s->rx_fifo[0];
                memmove(s->rx_fifo, s->rx_fifo + 1,
                        (--s->rx_count) * sizeof(uint32_t));
                dsp_update_irqs(s);
                dma_service(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: host read of "
                              "DSP %u's data port with nothing queued\n",
                              s->index);
            }
        }
        break;
    }
    return v >> ((off & 3) * 8);
}

static void dsp_bar_write(void *opaque, hwaddr off, uint64_t val,
                          unsigned size)
{
    SonicHDSPDSPState *s = opaque;

    if (s->in_reset) {
        return;
    }
    switch (off & ~3) {
    case 0x10:
        s->hctr = val;
        dsp_boot_step(s);
        break;
    case 0x14:
        break;                  /* HSTR is read-only from the host */
    case 0x18:
        s->hcvr = (val & 0x80ff);   /* HC, HV and HNMI */
        s->hc_raised = 0;
        dsp_hc_raise(s);
        break;
    default:
        if (off >= 0x1c && s->tx_count < SONIC_DSP_FIFO_DEPTH) {
            s->tx_fifo[s->tx_count++] = val & 0xffffff;
            dsp_boot_step(s);
        } else if (off >= 0x1c) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-sonic-hdsp: host write to "
                          "DSP %u's data port dropped, %u words queued\n",
                          s->index, SONIC_DSP_FIFO_DEPTH);
        }
        break;
    }
    dsp_update_irqs(s);
    dma_service(s);
}

static const MemoryRegionOps dsp_bar_ops = {
    .read = dsp_bar_read,
    .write = dsp_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* A held-in-reset or hidden chip does not answer a configuration cycle. */
static bool dsp_answers(SonicHDSPDSPState *s)
{
    if (s->in_reset || !s->pci_mode) {
        return false;       /* in reset, or the HI32 is not yet in PCI mode */
    }
    return s->hub->iop_access ||
           sonic_hdsp_dsp_visible(s->hub, PCI_SLOT(PCI_DEVICE(s)->devfn));
}

static uint32_t dsp_config_read(PCIDevice *d, uint32_t addr, int len)
{
    if (!dsp_answers(SGI_SONIC_HDSP_DSP(d))) {
        return UINT32_MAX >> (32 - 8 * len);
    }
    return pci_default_read_config(d, addr, len);
}

static void dsp_config_write(PCIDevice *d, uint32_t addr, uint32_t val,
                             int len)
{
    if (!dsp_answers(SGI_SONIC_HDSP_DSP(d))) {
        return;
    }
    pci_default_write_config(d, addr, val, len);
}

static void dsp_clear(SonicHDSPDSPState *s)
{
    memset(s->pmem, 0, sizeof(s->pmem));
    memset(s->xmem, 0, sizeof(s->xmem));
    memset(s->ymem, 0, sizeof(s->ymem));
    memset(s->ext_p, 0, sizeof(s->ext_p));
    memset(s->ext_y, 0, sizeof(s->ext_y));
    memset(s->periph, 0, sizeof(s->periph));
    memset(s->dor, 0, sizeof(s->dor));
    s->hctr = 0;
    s->hcvr = 0;
    s->hc_raised = 0;
    s->dctr = 0;
    s->tx_count = 0;
    s->rx_count = 0;
    s->boot = SONIC_DSP_BOOT_RUN;
    s->boot_count = s->boot_next = s->boot_start = 0;
    s->cyc_rem = 0;
    s->dma_busy = 0;
    dsp_periph_reset(s);
}

/*
 * The glue's reset byte: entering reset clears the core, its memories and its
 * peripherals; leaving it starts the bootstrap, which puts the HI32 in PCI
 * target mode and waits for the host's program.
 */
void sonic_hdsp_dsp_reset_line(SonicHDSPState *h, unsigned n, bool held)
{
    SonicHDSPDSPState *s = h->dsp[n];

    if (!s) {
        return;
    }
    if (held) {
        dsp_clear(s);
        dsp56300_reset(&s->core, DSP_BOOT_ROM);
        s->pci_mode = false;
        h->glue_intreq &= ~(1u << n);       /* its pin is released */
        sonic_hdsp_update_irq(h);
    } else if (s->in_reset) {
        s->dctr = DCTR_HM_PCI << DCTR_HM_SHIFT;
        s->pci_mode = true;
        s->boot = SONIC_DSP_BOOT_COUNT;
    }
    s->in_reset = held;
    if (!held && h->dsp_timer && !timer_pending(h->dsp_timer)) {
        /* The slice timer sleeps while every DSP is in reset: restart it. */
        int64_t now = now_ns();

        h->dsp_last_ns = now;
        h->frame_epoch_ns = now;
        h->frame_count = 0;
        h->dsp_ns_rem = 0;
        timer_mod(h->dsp_timer, now + DSP_SLICE_NS);
    }
}

static void dsp_realize(PCIDevice *d, Error **errp)
{
    SonicHDSPDSPState *s = SGI_SONIC_HDSP_DSP(d);

    if (!s->hub) {
        error_setg(errp, "%s is created by %s", TYPE_SGI_SONIC_HDSP_DSP,
                   TYPE_SGI_SONIC_HDSP);
        return;
    }
    memory_region_init_io(&s->bar0, OBJECT(d), &dsp_bar_ops, s,
                          "sgi-sonic-hdsp.dsp-host-port", DSP_BAR_SIZE);
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
    pci_config_set_interrupt_pin(d->config, 1);
    /* CSID reads 0 until the DSP's own boot program writes it. */
    pci_set_word(d->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(d->config + PCI_SUBSYSTEM_ID, 0);
    dsp56300_init(&s->core, &dsp_ops, s);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_P, s->pmem,
                        SONIC_DSP_PMEM_WORDS);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_X, s->xmem,
                        SONIC_DSP_XMEM_WORDS);
    dsp56300_set_memory(&s->core, DSP56300_SPACE_Y, s->ymem,
                        SONIC_DSP_YMEM_WORDS);
    s->in_reset = true;
}

static void dsp_reset(DeviceState *dev)
{
    SonicHDSPDSPState *s = SGI_SONIC_HDSP_DSP(dev);

    dsp_clear(s);
    dsp56300_reset(&s->core, DSP_BOOT_ROM);
    s->pci_mode = false;
    s->in_reset = true;     /* held until the glue's reset byte releases it */
}

static int dsp_post_load(void *opaque, int version_id)
{
    SonicHDSPDSPState *s = opaque;

    if (s->tx_count > SONIC_DSP_FIFO_DEPTH ||
        s->rx_count > SONIC_DSP_FIFO_DEPTH ||
        s->boot > SONIC_DSP_BOOT_DATA || s->hc_raised > 1 ||
        s->dma_busy > 1) {
        return -EINVAL;
    }
    for (unsigned n = 0; n < SONIC_DSP_NDMA; n++) {
        if (s->dma[n].blk_active > 1) {
            return -EINVAL;
        }
    }
    s->dma_busy = 0;
    /* The board's own load ran first and saw this DSP still in reset. */
    if (!s->in_reset && s->hub && s->hub->dsp_timer &&
        !timer_pending(s->hub->dsp_timer)) {
        timer_mod(s->hub->dsp_timer, now_ns() + DSP_SLICE_NS);
    }
    return 0;
}

static const VMStateDescription vmstate_sonic_dsp_essi = {
    .name = "sgi-sonic-hdsp-dsp/essi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cra, SonicDspEssi),
        VMSTATE_UINT32(crb, SonicDspEssi),
        VMSTATE_UINT32(tsma, SonicDspEssi),
        VMSTATE_UINT32(tsmb, SonicDspEssi),
        VMSTATE_UINT32(rsma, SonicDspEssi),
        VMSTATE_UINT32(rsmb, SonicDspEssi),
        VMSTATE_UINT32(ssisr, SonicDspEssi),
        VMSTATE_UINT32_ARRAY(tx, SonicDspEssi, 3),
        VMSTATE_UINT32(rx, SonicDspEssi),
        VMSTATE_UINT32(tx_written, SonicDspEssi),
        VMSTATE_UINT32(tsr_skip, SonicDspEssi),
        VMSTATE_UINT32(sr_read, SonicDspEssi),
        VMSTATE_UINT32(pcr, SonicDspEssi),
        VMSTATE_UINT32(prr, SonicDspEssi),
        VMSTATE_UINT32(pdr, SonicDspEssi),
        VMSTATE_UINT32(in_word, SonicDspEssi),
        VMSTATE_UINT32(in_valid, SonicDspEssi),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sonic_dsp_dma = {
    .name = "sgi-sonic-hdsp-dsp/dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dsr, SonicDspDma),
        VMSTATE_UINT32(ddr, SonicDspDma),
        VMSTATE_UINT32(dco, SonicDspDma),
        VMSTATE_UINT32(dco_init, SonicDspDma),
        VMSTATE_UINT32(dcr, SonicDspDma),
        VMSTATE_UINT32(blk_active, SonicDspDma),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sonic_dsp_timer = {
    .name = "sgi-sonic-hdsp-dsp/timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(tcsr, SonicDspTimer),
        VMSTATE_UINT32(tlr, SonicDspTimer),
        VMSTATE_UINT32(tcpr, SonicDspTimer),
        VMSTATE_UINT32(tcr, SonicDspTimer),
        VMSTATE_UINT32(irq_out, SonicDspTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sonic_hdsp_dsp = {
    .name = "sgi-sonic-hdsp-dsp",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dsp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SonicHDSPDSPState),
        VMSTATE_DSP56300(core, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(pmem, SonicHDSPDSPState, SONIC_DSP_PMEM_WORDS),
        VMSTATE_UINT32_ARRAY(xmem, SonicHDSPDSPState, SONIC_DSP_XMEM_WORDS),
        VMSTATE_UINT32_ARRAY(ymem, SonicHDSPDSPState, SONIC_DSP_YMEM_WORDS),
        VMSTATE_UINT32_ARRAY(ext_p, SonicHDSPDSPState, SONIC_DSP_EXT_WORDS),
        VMSTATE_UINT32_ARRAY(ext_y, SonicHDSPDSPState, SONIC_DSP_EXT_WORDS),
        VMSTATE_UINT32(hctr, SonicHDSPDSPState),
        VMSTATE_UINT32(hcvr, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(tx_fifo, SonicHDSPDSPState,
                             SONIC_DSP_FIFO_DEPTH),
        VMSTATE_UINT32(tx_count, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(rx_fifo, SonicHDSPDSPState,
                             SONIC_DSP_FIFO_DEPTH),
        VMSTATE_UINT32(rx_count, SonicHDSPDSPState),
        VMSTATE_BOOL(in_reset, SonicHDSPDSPState),
        VMSTATE_BOOL(pci_mode, SonicHDSPDSPState),
        VMSTATE_UINT32(ipr_c, SonicHDSPDSPState),
        VMSTATE_UINT32(ipr_p, SonicHDSPDSPState),
        VMSTATE_UINT32(dctr, SonicHDSPDSPState),
        VMSTATE_UINT32_ARRAY(dor, SonicHDSPDSPState, 4),
        VMSTATE_UINT32(dtd, SonicHDSPDSPState),
        VMSTATE_UINT32(boot, SonicHDSPDSPState),
        VMSTATE_UINT32(boot_count, SonicHDSPDSPState),
        VMSTATE_UINT32(boot_next, SonicHDSPDSPState),
        VMSTATE_UINT32(boot_start, SonicHDSPDSPState),
        VMSTATE_UINT32(hc_raised, SonicHDSPDSPState),
        VMSTATE_UINT32(cyc_rem, SonicHDSPDSPState),
        VMSTATE_STRUCT_ARRAY(dma, SonicHDSPDSPState, SONIC_DSP_NDMA, 1,
                             vmstate_sonic_dsp_dma, SonicDspDma),
        VMSTATE_STRUCT_ARRAY(essi, SonicHDSPDSPState, SONIC_DSP_NESSI, 1,
                             vmstate_sonic_dsp_essi, SonicDspEssi),
        VMSTATE_STRUCT_ARRAY(timer, SonicHDSPDSPState, SONIC_DSP_NTIMER, 1,
                             vmstate_sonic_dsp_timer, SonicDspTimer),
        VMSTATE_UINT32_ARRAY(periph, SonicHDSPDSPState,
                             SONIC_DSP_PERIPH_WORDS),
        VMSTATE_END_OF_LIST()
    }
};

static void dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = dsp_realize;
    k->config_read = dsp_config_read;
    k->config_write = dsp_config_write;
    k->vendor_id = DSP_VENDOR_ID;
    k->device_id = DSP_DEVICE_ID;
    dc->desc = "Sonic HDSP DSP56301 (secondary bus)";
    dc->vmsd = &vmstate_sonic_hdsp_dsp;
    device_class_set_legacy_reset(dc, dsp_reset);
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo dsp_info = {
    .name = TYPE_SGI_SONIC_HDSP_DSP,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SonicHDSPDSPState),
    .class_init = dsp_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void sonic_hdsp_dsp_register_types(void)
{
    type_register_static(&dsp_info);
}

type_init(sonic_hdsp_dsp_register_types)

/* ------------------------------------------------------------------ */
/* Scheduling: the cores and the sample clock                         */
/* ------------------------------------------------------------------ */

/* After a stretch of core time: the peripherals that count it. */
static void dsp_post_run(SonicHDSPDSPState *s, uint64_t cycles)
{
    uint64_t events;

    cycles += s->cyc_rem;
    events = cycles / 2;
    s->cyc_rem = cycles & 1;
    for (unsigned n = 0; n < SONIC_DSP_NTIMER; n++) {
        timer_advance(s, n, events);
    }
    dsp_hc_check(s);
    dsp_update_irqs(s);
    dma_service(s);
}

static void dsp_run_cores(SonicHDSPState *h, int64_t ns)
{
    uint64_t total = (uint64_t)ns * h->dsp_clock + h->dsp_ns_rem;
    uint64_t cycles = total / NANOSECONDS_PER_SECOND;

    h->dsp_ns_rem = total % NANOSECONDS_PER_SECOND;
    if (!cycles) {
        return;
    }
    for (unsigned n = 0; n < SONIC_HDSP_NUM_DSP; n++) {
        SonicHDSPDSPState *s = h->dsp[n];

        if (!s || s->in_reset || s->boot != SONIC_DSP_BOOT_RUN) {
            continue;
        }
        /* An idle core is waiting on the world: its time still passes. */
        dsp56300_run(&s->core, cycles);
        dsp_post_run(s, cycles);
    }
}

static int64_t frame_time(SonicHDSPState *h, uint64_t frame)
{
    return h->frame_epoch_ns +
           muldiv64(frame, NANOSECONDS_PER_SECOND, h->sample_rate);
}

static void dsp_frame_tick(SonicHDSPState *h)
{
    for (unsigned n = 0; n < SONIC_HDSP_NUM_DSP; n++) {
        SonicHDSPDSPState *s = h->dsp[n];

        if (s && !s->in_reset && s->boot == SONIC_DSP_BOOT_RUN) {
            essi_frame(s);
        }
    }
}

static bool dsp_any_out_of_reset(SonicHDSPState *h)
{
    for (unsigned n = 0; n < SONIC_HDSP_NUM_DSP; n++) {
        if (h->dsp[n] && !h->dsp[n]->in_reset) {
            return true;
        }
    }
    return false;
}

static void dsp_slice(void *opaque)
{
    SonicHDSPState *h = opaque;
    int64_t now = now_ns();
    int64_t t = MAX(h->dsp_last_ns, now - DSP_MAX_CATCHUP_NS);
    int64_t next_frame;
    unsigned ticks = 0;

    while (t < now) {
        int64_t t1;

        next_frame = frame_time(h, h->frame_count + 1);
        t1 = MIN(now, MAX(next_frame, t));
        if (t1 > t) {
            dsp_run_cores(h, t1 - t);
        }
        t = t1;
        if (t >= next_frame) {
            h->frame_count++;
            dsp_frame_tick(h);
            if (++ticks >= DSP_MAX_FRAMES_PER_SLICE) {
                /* A stall: drop the frames missed rather than replay them. */
                h->frame_epoch_ns = now - muldiv64(h->frame_count,
                                                   NANOSECONDS_PER_SECOND,
                                                   h->sample_rate);
                t = now;
            }
        }
    }
    h->dsp_last_ns = now;
    if (dsp_any_out_of_reset(h)) {
        timer_mod(h->dsp_timer, now + DSP_SLICE_NS);
    }
}

bool sonic_hdsp_dsp_realize(SonicHDSPState *h, Error **errp)
{
    static const uint32_t rates[] = {
        44100, 48000, 88200, 96000, 176400, 192000,
    };
    bool ok = false;

    for (unsigned i = 0; i < ARRAY_SIZE(rates); i++) {
        ok |= h->sample_rate == rates[i];
    }
    if (!ok) {
        error_setg(errp, "sgi-sonic-hdsp: sample-rate must be 44100, 48000, "
                   "88200, 96000, 176400 or 192000");
        return false;
    }
    if (h->box) {
        sonic_ada8824_set_rate(h->box, h->sample_rate);
    }
    h->dsp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dsp_slice, h);
    return true;
}

void sonic_hdsp_dsp_unrealize(SonicHDSPState *h)
{
    if (h->dsp_timer) {
        timer_free(h->dsp_timer);
        h->dsp_timer = NULL;
    }
}

void sonic_hdsp_dsp_board_reset(SonicHDSPState *h)
{
    int64_t now = now_ns();

    h->dsp_last_ns = now;
    h->frame_epoch_ns = now;
    h->frame_count = 0;
    h->dsp_ns_rem = 0;
    memset(h->sync, 0, sizeof(h->sync));
    timer_del(h->dsp_timer);        /* every DSP is in reset */
}

/* Virtual-time stamps travel as ages: the destination's clock may differ. */
void sonic_hdsp_dsp_pre_save(SonicHDSPState *h)
{
    int64_t now = now_ns();

    h->mig_dsp_age = now - h->dsp_last_ns;
    h->mig_frame_epoch_age = now - h->frame_epoch_ns;
}

void sonic_hdsp_dsp_post_load(SonicHDSPState *h)
{
    int64_t now = now_ns();

    h->dsp_last_ns = now - h->mig_dsp_age;
    h->frame_epoch_ns = now - h->mig_frame_epoch_age;
    h->dsp_ns_rem %= NANOSECONDS_PER_SECOND;
    if (dsp_any_out_of_reset(h)) {
        timer_mod(h->dsp_timer, now + DSP_SLICE_NS);
    }
}
