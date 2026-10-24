/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Adaptec AIC-7880 PCI SCSI controller, isolated initiator experiment.
 *
 * The downloaded sequencer controls selection, command delivery, status,
 * and Command Complete. This subset accepts one asynchronous narrow command
 * at a time. Paused host PIO supports manual DATA and status exchanges.
 * Isolated qtest peer events exercise disconnect/reselection; targets stay
 * connected. Host command PIO and target mode are unsupported.
 * Reset values transferred from the AIC-7870 manual are provisional.
 * The O2 qtest policy selects its sibling's BAR1 size and SWINT gating.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/core/resettable.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/aic7880.h"
#include "scsi/utils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "system/qtest.h"
#include "trace.h"

#define TYPE_AIC7880 "aic7880"
OBJECT_DECLARE_SIMPLE_TYPE(AIC7880State, AIC7880)

#define AIC_REG_SPACE 0x100
#define AIC_SEQ_INSNS 512
#define AIC_SEQ_BYTES (AIC_SEQ_INSNS * 4)
#define AIC_SCB_PAGES 16
#define AIC_SCB_EXTERNAL_PAGES 256
#define AIC_SCB_PAGE_BYTES 32

#define REG_SCSISEQ  0x00
#define REG_SXFRCTL0 0x01
#define REG_SXFRCTL1 0x02
#define REG_SCSISIGI 0x03
#define REG_SCSIRATE 0x04
#define REG_SCSIBUSL 0x12
#define REG_SCSIID   0x05
#define REG_SCSIDATL 0x06
#define REG_STCNT    0x08
#define REG_SSTAT0   0x0b
#define REG_SSTAT1   0x0c
#define REG_SIMODE0  0x10
#define REG_SIMODE1  0x11
#define REG_SHADDR   0x14
#define REG_SBLKCTL  0x1f
#define REG_SEQCTL   0x60
#define REG_SEQRAM   0x61
#define REG_SEQADDR0 0x62
#define REG_SEQADDR1 0x63
#define REG_ACCUM    0x64
#define REG_SINDEX   0x65
#define REG_DINDEX   0x66
#define REG_BRKADDR0 0x67
#define REG_BRKADDR1 0x68
#define REG_SINDIR   0x6c
#define REG_DINDIR   0x6d
#define REG_ALLONES  0x69
#define REG_ALLZEROS 0x6a
#define REG_FLAGS    0x6b
#define REG_FUNCTION1 0x6e
#define REG_STACK    0x6f
#define REG_DSCOMMAND 0x84
#define REG_DSPCISTATUS 0x86
#define REG_HCNTRL   0x87
#define REG_HCNT_RSVD 0x8f
#define REG_SCBPTR   0x90
#define REG_SCBCNT   0x9a
#define REG_QINFIFO  0x9b
#define REG_QINCNT   0x9c
#define REG_SCB_BASE 0xa0
#define REG_INTSTAT  0x91
#define REG_CLRINT   0x92
#define REG_ERROR    0x92
#define REG_DFCNTRL  0x93
#define REG_DFSTATUS 0x94
#define REG_DFDAT    0x99
#define REG_QOUTFIFO 0x9d
#define REG_QOUTCNT  0x9e

#define SEQ_LOADRAM  0x01
#define SEQ_RESET    0x02
#define SEQ_STEP     0x04
#define SEQ_BRKINTEN 0x08
#define SEQ_FASTMODE 0x10
#define SEQ_FAILDIS  0x20
#define SEQ_PAUSEDIS 0x40
#define HOST_CHIPRST 0x01
#define HOST_INTEN   0x02
#define HOST_PAUSE   0x04
#define HOST_SWINT   0x10
#define HOST_POWRDN  0x40
#define FLAGS_CARRY  0x01
#define FLAGS_ZERO   0x02
#define SXFR_CLRSTCNT 0x10
#define SXFR_CLRCHN   0x02
#define DFC_FIFOFLUSH 0x02
#define DFC_FIFORESET 0x01
#define DFS_FIFOQWDEMP 0x20
#define DFS_HDONE 0x08
#define DFS_MREQPEND 0x10
#define DFS_DFTHRSH 0x04
#define DFS_FIFOFULL 0x02
#define DFS_FIFOEMP 0x01
#define SBLK_AUTOFLUSHDIS 0x20
#define SBLK_SELWIDE 0x02

#define SCSI_SIG_COMMAND 0x86
#define SCSI_SIG_DATA_IN 0x46
#define SCSI_SIG_DATA_OUT 0x06
#define SCSI_SIG_STATUS 0xc6
#define SCSI_SIG_MSG_IN 0xe6
#define SCSI_SIG_MSG_OUT 0xa6
#define SCSI_SIG_ATN 0x10

enum {
    AIC_PHASE_BUS_FREE,
    AIC_PHASE_COMMAND,
    AIC_PHASE_DATA,
    AIC_PHASE_STATUS,
    AIC_PHASE_MSG_IN,
    AIC_PHASE_MSG_OUT,
};

struct AIC7880State {
    PCIDevice parent_obj;
    MemoryRegion io;
    MemoryRegion mem;
    MemoryRegion dspc_io;
    MemoryRegion dspc_mem[16];
    SCSIBus bus;
    QEMUTimer *seq_timer;
    int64_t seq_deadline_ns;
    QEMUTimer *selection_timer;
    QEMUTimer *bus_free_timer;
    uint8_t regs[AIC_REG_SPACE];
    uint8_t seqram[AIC_SEQ_BYTES];
    uint8_t scb_ram[AIC_SCB_EXTERNAL_PAGES * AIC_SCB_PAGE_BYTES];
    uint8_t scb_parity[AIC_SCB_EXTERNAL_PAGES * AIC_SCB_PAGE_BYTES];
    bool parity_break_pending;
    bool external_scb_ram;
    bool external_scb_shared;
    uint32_t external_scb_size;
    uint8_t external_scb_width;
    uint16_t seqaddr;
    uint8_t seqbyte;
    uint8_t error;
    uint32_t error_serial;
    uint8_t cdb[16];
    uint8_t cdb_len;
    uint8_t cdb_need;
    bool command_dma_wait;
    bool command_fifo_stale;
    uint8_t target;
    uint8_t initiator;
    bool selection_wide;
    bool identity_valid;
    uint8_t lun;
    uint8_t phase;
    uint8_t expected_phase;
    uint8_t signal_outputs;
    uint8_t manual_data;
    bool manual_valid;
    uint8_t qout[256];
    uint8_t qin[256];
    uint8_t qin_head;
    uint8_t qin_tail;
    uint8_t qin_count;
    uint16_t return_pc[4];
    uint8_t return_ptr;
    uint8_t stack_byte;
    uint32_t scsi_data_len;
    uint32_t scsi_data_pos;
    uint8_t host_fifo[256];
    uint8_t host_fifo_read;
    uint8_t host_fifo_write;
    uint16_t host_fifo_len;
    bool host_dma_active;
    bool host_request_pending;
    uint8_t qout_head;
    uint8_t qout_tail;
    uint8_t qout_count;
    bool selected;
    bool selection_pending;
    bool initiator_atn;
    bool identify_seen;
    uint8_t message_out[5];
    uint8_t message_out_len;
    bool message_resume_command;
    bool disconnect_allowed;
    bool peer_reselection;
    bool peer_nexus_disconnected;
    bool peer_nexus_saved;
    uint8_t peer_nexus_message;
    uint8_t peer_nexus_signal;
    uint32_t peer_nexus_saved_pos;
    bool peer_scsi_irq_latched;
    bool peer_hold_data_in;
    bool peer_data_in_pending;
    bool peer_last_parity_bad;
    bool peer_faulted_command;
    uint32_t peer_data_len;
    uint32_t peer_data_pos;
    bool test_peer;
    bool maskable_break_pending;
    bool error_pending;
    bool running;
    bool o2_pci_diagnostic;
    bool o2_storage_diagnostic;
    AIC7880DMAReporter dma_reporter;
    void *dma_reporter_opaque;
    SCSIRequest *req;
};

void aic7880_set_storage_diagnostic_reporter(PCIDevice *pci,
                                             AIC7880DMAReporter reporter,
                                             void *opaque)
{
    AIC7880State *s = AIC7880(pci);

    g_assert(!DEVICE(pci)->realized);
    s->dma_reporter = reporter;
    s->dma_reporter_opaque = opaque;
}

static void aic7880_run(AIC7880State *s);
static void aic7880_host_dma(AIC7880State *s);
static void aic7880_process_scsi_data(AIC7880State *s);
static void aic7880_process_command(AIC7880State *s);

static int64_t aic7880_seq_period_ns(AIC7880State *s)
{
    /* Selected 40 MHz AIC-7870 sibling clock, not measured AIC-7880 timing. */
    return (s->regs[REG_SEQCTL] & SEQ_FASTMODE) ? 100 : 125;
}

static int64_t aic7880_event_time_ns(AIC7880State *s)
{
    return s->running ? s->seq_deadline_ns :
                       qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static bool aic7880_seq_runnable(AIC7880State *s)
{
    return !(s->regs[REG_HCNTRL] & HOST_PAUSE) &&
           !(s->regs[REG_SEQCTL] & SEQ_LOADRAM) &&
           !s->error_pending && !s->parity_break_pending;
}

static void aic7880_seq_stop(AIC7880State *s, const char *reason)
{
    if (timer_pending(s->seq_timer) || s->running) {
        trace_aic7880_seq_clock(PCI_DEVICE(s)->devfn, reason,
            s->seq_deadline_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            aic7880_seq_period_ns(s), s->regs[REG_HCNTRL],
            s->regs[REG_SEQCTL], s->regs[REG_INTSTAT]);
    }
    timer_del(s->seq_timer);
}

static bool aic7880_command_dma_owned(AIC7880State *s)
{
    return s->selected && s->phase == AIC_PHASE_COMMAND &&
           (s->regs[REG_DFCNTRL] & 0x30);
}

static void aic7880_dma_done(AIC7880State *s)
{
    s->regs[REG_SSTAT0] &= ~1;
    if ((s->regs[REG_SSTAT0] & 4) && (s->regs[REG_DFSTATUS] & DFS_HDONE)) {
        s->regs[REG_SSTAT0] |= 1;
    }
}

static bool aic7880_pio_pending(AIC7880State *s)
{
    if (!s->selected || (s->signal_outputs & 1)) {
        return false;
    }
    if (s->phase == AIC_PHASE_DATA) {
        return !(s->regs[REG_SXFRCTL0] & 8) &&
               !(s->regs[REG_DFCNTRL] & 0x38) &&
               !s->peer_nexus_disconnected && s->req &&
               s->scsi_data_pos < s->scsi_data_len;
    }
    if (s->phase == AIC_PHASE_COMMAND) {
        return !s->req && (!s->cdb_need || s->cdb_len < s->cdb_need);
    }
    return s->phase == AIC_PHASE_STATUS ||
           s->phase == AIC_PHASE_MSG_IN ||
           s->phase == AIC_PHASE_MSG_OUT;
}

static bool aic7880_manual_busy(AIC7880State *s)
{
    return s->manual_valid || (s->signal_outputs & 1);
}

static bool aic7880_host_fifo_accessible(AIC7880State *s)
{
    return !(s->regs[REG_DFCNTRL] & 0x30) &&
           !s->peer_nexus_disconnected && !aic7880_manual_busy(s);
}

static void aic7880_manual_clear(AIC7880State *s)
{
    s->manual_valid = false;
    s->manual_data = 0;
}

static void aic7880_pio_phase(AIC7880State *s)
{
    s->regs[REG_SSTAT1] &= ~0x10;
    if ((s->regs[REG_SSTAT1] & 1) &&
        (s->regs[REG_SCSISIGI] & 0xe0) != s->expected_phase) {
        s->regs[REG_SSTAT1] |= 0x10;
    }
}

static void aic7880_pio_present(AIC7880State *s)
{
    s->regs[REG_SSTAT1] &= ~0x11;
    s->regs[REG_SSTAT0] &= ~2;
    s->regs[REG_SCSISIGI] &= ~2;
    if (aic7880_pio_pending(s)) {
        s->regs[REG_SSTAT1] |= 1;
        s->regs[REG_SCSISIGI] |= 2;
        if ((s->regs[REG_SXFRCTL0] & 8) &&
            !aic7880_command_dma_owned(s)) {
            s->regs[REG_SSTAT0] |= 2;
        }
        aic7880_pio_phase(s);
    } else if (s->selected && s->phase == AIC_PHASE_DATA &&
               !(s->signal_outputs & 1) && !s->peer_nexus_disconnected &&
               s->req && s->scsi_data_pos < s->scsi_data_len) {
        /* DATA requests are observable even when DMA owns their handshake. */
        s->regs[REG_SCSISIGI] |= 2;
        if (s->command_dma_wait) {
            s->regs[REG_SSTAT1] |= 1;
            aic7880_pio_phase(s);
        }
    }
}

static bool aic7880_pio_enabled(AIC7880State *s)
{
    return (s->regs[REG_SXFRCTL0] & 8) &&
           !(s->regs[REG_SSTAT1] & 0x10);
}

static void aic7880_update_irq(AIC7880State *s)
{
    bool enabled = s->regs[REG_HCNTRL] & HOST_INTEN;
    bool decoded = PCI_DEVICE(s)->config[PCI_COMMAND] &
                   (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    bool swint = (s->regs[REG_HCNTRL] & HOST_SWINT) && decoded;
    bool exception = (s->regs[REG_INTSTAT] & 0x08) &&
                     (!s->maskable_break_pending || s->parity_break_pending ||
                      (s->error & 0x04) ||
                      (s->regs[REG_SEQCTL] & SEQ_BRKINTEN));
    bool scsiint = (s->regs[REG_INTSTAT] & 0x04) &&
                   (!s->peer_scsi_irq_latched || decoded);
    bool pending = (s->regs[REG_INTSTAT] & 0x03) || scsiint ||
                   exception || swint;

    bool level = enabled && pending &&
                 !(s->regs[REG_HCNTRL] & HOST_POWRDN) &&
                 (PCI_DEVICE(s)->config[PCI_COMMAND] & PCI_COMMAND_MASTER);

    trace_aic7880_irq(PCI_DEVICE(s)->devfn, level, s->regs[REG_INTSTAT],
                      s->regs[REG_HCNTRL],
                      pci_get_word(PCI_DEVICE(s)->config + PCI_COMMAND));
    pci_set_irq(PCI_DEVICE(s), level);
}

static void aic7880_update_scsi_irq(AIC7880State *s)
{
    if ((s->regs[REG_SSTAT0] & s->regs[REG_SIMODE0]) ||
        (s->regs[REG_SSTAT1] & s->regs[REG_SIMODE1])) {
        s->regs[REG_INTSTAT] |= 0x04;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_seq_stop(s, "scsi-irq");
    }
    aic7880_update_irq(s);
}

static void aic7880_error(AIC7880State *s, const char *reason);

static void aic7880_write_config(PCIDevice *pci, uint32_t addr,
                                  uint32_t val, int len)
{
    AIC7880State *s = AIC7880(pci);
    uint16_t before = pci_get_word(pci->config + PCI_COMMAND);

    if (s->external_scb_ram) {
        for (unsigned i = 0; i < len; i++) {
            uint8_t byte = val >> (i * 8);

            if ((addr + i == 0x40 && (byte & 0x10) &&
                 s->external_scb_width != 9) ||
                (addr + i == 0x42 && (byte & 1))) {
                aic7880_error(s,
                              "unsupported external SRAM configuration");
                return;
            }
        }
    }
    pci_default_write_config(pci, addr, val, len);
    if (pci_get_word(pci->config + PCI_COMMAND) != before) {
        trace_aic7880_pci_command(pci->devfn, before,
            pci_get_word(pci->config + PCI_COMMAND),
            ldl_le_p(&s->regs[0x88]),
            ldl_le_p(&s->regs[0x8c]) & 0xffffff,
            s->regs[REG_DFCNTRL]);
        aic7880_update_irq(s);
        aic7880_host_dma(s);
        if (!s->error_pending &&
            !(pci_get_word(pci->config + PCI_STATUS) &
              (PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT))) {
            aic7880_process_scsi_data(s);
        }
    }
}

static uint32_t aic7880_count(AIC7880State *s, unsigned offset)
{
    return s->regs[offset] | (s->regs[offset + 1] << 8) |
           (s->regs[offset + 2] << 16);
}

static void aic7880_set_count(AIC7880State *s, unsigned offset,
                              uint32_t count)
{
    for (unsigned i = 0; i < 3; i++) {
        s->regs[offset + i] = count >> (i * 8);
    }
}

static void aic7880_fifo_status(AIC7880State *s)
{
    /* The selected default follows the sibling's three-word example. */
    static const unsigned thresholds[] = { 3, 16, 24, 32 };
    unsigned fifo_len = s->host_fifo_len;
    unsigned read = s->host_fifo_read;
    unsigned head = (8 - (read & 7)) & 7;
    unsigned capacity_words = sizeof(s->host_fifo) / 8;
    unsigned complete_words = fifo_len == sizeof(s->host_fifo) ?
                              capacity_words :
                              fifo_len >= head ? (fifo_len - head) / 8 : 0;
    unsigned occupied_words = fifo_len ?
        MIN(capacity_words, DIV_ROUND_UP((read & 7) + fifo_len, 8)) : 0;
    unsigned threshold = thresholds[s->regs[REG_DSPCISTATUS] >> 6];
    uint8_t control = s->regs[REG_DFCNTRL];
    bool from_memory = control & 0x04;
    bool ready = from_memory ? capacity_words - occupied_words >= threshold :
                              complete_words >= threshold;
    uint8_t status = s->regs[REG_DFSTATUS] &
        ~(DFS_FIFOQWDEMP | DFS_FIFOFULL | DFS_FIFOEMP | DFS_DFTHRSH |
          DFS_MREQPEND);

    if (!complete_words) {
        status |= DFS_FIFOQWDEMP;
    }
    if (ready) {
        status |= DFS_DFTHRSH;
    }
    if (!(control & 0x08) || !aic7880_count(s, 0x8c) || s->error_pending ||
        (from_memory ? fifo_len == sizeof(s->host_fifo) : !fifo_len)) {
        s->host_request_pending = false;
    } else if (ready || (!from_memory && (control & DFC_FIFOFLUSH))) {
        s->host_request_pending = true;
    }
    /* A started request survives falling below its start threshold. */
    if (s->host_request_pending) {
        status |= DFS_MREQPEND;
    }
    if (fifo_len == sizeof(s->host_fifo)) {
        status |= DFS_FIFOFULL;
    }
    if (!fifo_len) {
        status |= DFS_FIFOEMP;
        s->command_fifo_stale = false;
    }
    s->regs[REG_DFSTATUS] = status;
    aic7880_dma_done(s);
}

static void aic7880_stop_diagnostic(AIC7880State *s)
{
    s->error_serial++;
    s->error_pending = true;
    s->regs[REG_HCNTRL] |= HOST_PAUSE;
    aic7880_seq_stop(s, "diagnostic");
    aic7880_update_irq(s);
}

static void aic7880_error(AIC7880State *s, const char *reason)
{
    if (s->running) {
        qemu_log_mask(LOG_GUEST_ERROR, "aic7880: %s at pc 0x%03x\n",
                      reason, s->seqaddr - 1);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "aic7880: %s\n", reason);
    }
    aic7880_stop_diagnostic(s);
}

static void aic7880_bad_opcode(AIC7880State *s, uint8_t opcode)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "aic7880: illegal sequencer opcode %u\n", opcode);
    s->error_serial++;
    s->error |= 0x04;
    s->error_pending = true;
    s->regs[REG_INTSTAT] |= 0x08;
    s->regs[REG_HCNTRL] |= HOST_PAUSE;
    aic7880_seq_stop(s, "illegal-opcode");
    aic7880_update_irq(s);
}

static bool aic7880_dma_attempt(AIC7880State *s, PCIDevice *pci,
                                uint32_t addr, bool to_memory,
                                uint8_t *buf, uint32_t len)
{
    MemTxResult result;
    MemReentrancyGuard *guard = &DEVICE(s)->mem_reentrancy_guard;
    bool was_engaged = guard->engaged_in_io;

    guard->engaged_in_io = true;
    if (to_memory) {
        result = pci_dma_write(pci, addr, buf, len);
    } else {
        result = pci_dma_read(pci, addr, buf, len);
    }
    guard->engaged_in_io = was_engaged;
    trace_aic7880_dma(pci->devfn, addr, len, to_memory, result);

    if (s->o2_storage_diagnostic &&
        !s->dma_reporter(s->dma_reporter_opaque, pci, addr, to_memory,
                         len, result)) {
        aic7880_error(s, "PCI DMA diagnostic record was not retained");
        return false;
    }
    if (result != MEMTX_OK) {
        aic7880_error(s, "PCI DMA refused by bus address space");
        return false;
    }
    return true;
}

static void aic7880_host_dma(AIC7880State *s)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t data[256];
    uint32_t addr = ldl_le_p(&s->regs[0x88]);
    uint32_t count = aic7880_count(s, 0x8c);
    uint8_t control = s->regs[REG_DFCNTRL];
    bool from_memory = control & 0x04;
    uint32_t len;
    bool success;

    aic7880_fifo_status(s);
    if (s->phase == AIC_PHASE_COMMAND && s->selected && !s->req &&
        !from_memory && count && (control & 0x30)) {
        aic7880_error(s, "COMMAND DMA direction is unsupported");
        return;
    }
    if ((s->phase == AIC_PHASE_DATA && s->command_dma_wait &&
         (control & 0x30)) ||
        (s->req && s->phase != AIC_PHASE_DATA &&
         !aic7880_host_fifo_accessible(s)) ||
        s->peer_nexus_disconnected || !s->host_request_pending ||
        (pci_get_word(pci->config + PCI_STATUS) &
         (PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT)) ||
        !(pci_get_word(pci->config + PCI_COMMAND) & PCI_COMMAND_MASTER) ||
        !(pci_get_word(pci->config + PCI_COMMAND) &
          (PCI_COMMAND_IO | PCI_COMMAND_MEMORY))) {
        return;
    }
    /* A short response can leave receive bytes for a STATUS flush. */
    if ((control & 0x30) && !s->req &&
        !(s->selected && s->phase == AIC_PHASE_COMMAND && from_memory) &&
        (from_memory || s->phase != AIC_PHASE_STATUS)) {
        return;
    }
    if ((s->selected && !s->req && !aic7880_host_fifo_accessible(s) &&
         s->phase != AIC_PHASE_COMMAND &&
         (from_memory || s->phase != AIC_PHASE_STATUS)) ||
        count > UINT32_MAX - addr) {
        aic7880_error(s, "unsupported host-only DMA transfer");
        return;
    }
    if (from_memory) {
        len = MIN(count, sizeof(s->host_fifo) - s->host_fifo_len);
        if (!len) {
            return;
        }
        s->host_dma_active = true;
        success = aic7880_dma_attempt(s, pci, addr, false, data, len);
        s->host_dma_active = false;
        if (!success) {
            return;
        }
        for (unsigned i = 0; i < len; i++) {
            s->host_fifo[s->host_fifo_write++] = data[i];
        }
        s->host_fifo_len += len;
    } else {
        len = MIN(count, s->host_fifo_len);
        if (!len) {
            return;
        }
        for (unsigned i = 0; i < len; i++) {
            data[i] = s->host_fifo[(uint8_t)(s->host_fifo_read + i)];
        }
        s->host_dma_active = true;
        success = aic7880_dma_attempt(s, pci, addr, true, data, len);
        s->host_dma_active = false;
        if (!success) {
            return;
        }
        s->host_fifo_read += len;
        s->host_fifo_len -= len;
        if (!s->host_fifo_len) {
            s->regs[REG_DFCNTRL] &= ~DFC_FIFOFLUSH;
        }
    }
    stl_le_p(&s->regs[0x88], addr + len);
    aic7880_set_count(s, 0x8c, count - len);
    if (count == len) {
        s->regs[REG_DFSTATUS] |= DFS_HDONE;
    }
    aic7880_fifo_status(s);
    if (!from_memory && !s->req && s->phase == AIC_PHASE_STATUS &&
        !s->host_fifo_len && !s->running) {
        aic7880_run(s);
    }
}

static void aic7880_advance_scsi(AIC7880State *s, uint32_t count)
{
    uint32_t shaddr = ldl_le_p(&s->regs[REG_SHADDR]);
    uint32_t stcnt = aic7880_count(s, REG_STCNT);

    if (!count) {
        return;
    }
    stl_le_p(&s->regs[REG_SHADDR], shaddr + count);
    aic7880_set_count(s, REG_STCNT, stcnt - count);
    if (stcnt == count) {
        s->regs[REG_SSTAT0] |= 0x04;
        if (s->phase == AIC_PHASE_DATA && s->host_fifo_len &&
            !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
            s->regs[REG_DFCNTRL] |= DFC_FIFOFLUSH;
        }
    }
    aic7880_update_scsi_irq(s);
}

static void aic7880_complete(SCSIRequest *req, size_t residual)
{
    AIC7880State *s = req->hba_private;
    bool from_data = s->phase == AIC_PHASE_DATA;

    s->regs[REG_SCSIDATL] = req->status;
    s->regs[REG_SCSISIGI] = SCSI_SIG_STATUS;
    s->phase = AIC_PHASE_STATUS;
    aic7880_pio_present(s);
    if (s->regs[REG_SSTAT1] & 0x10) {
        if (from_data && s->host_fifo_len &&
            !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
            s->regs[REG_DFCNTRL] |= DFC_FIFOFLUSH;
        }
    } else if (from_data && s->host_fifo_len &&
               aic7880_count(s, REG_STCNT) &&
               !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
        aic7880_error(s, "phase change without mismatch is unsupported");
    }
    s->command_dma_wait = false;
    s->phase = AIC_PHASE_STATUS;
    if (!s->host_fifo_len) {
        s->regs[REG_DFCNTRL] &= ~DFC_FIFOFLUSH;
    }
    aic7880_update_scsi_irq(s);
    s->req = NULL;
    s->scsi_data_len = s->scsi_data_pos = 0;
    scsi_req_unref(req);
    if (s->host_fifo_len && (s->regs[REG_DFCNTRL] & 0x0a) == 0x0a) {
        aic7880_host_dma(s);
    } else if (!s->running) {
        aic7880_run(s);
    }
}

static void aic7880_cancel(SCSIRequest *req)
{
    AIC7880State *s = req->hba_private;

    if (s->req == req) {
        s->peer_nexus_disconnected = false;
        s->peer_nexus_saved = false;
        s->peer_nexus_message = 0;
        s->scsi_data_len = s->scsi_data_pos = 0;
        s->regs[REG_DFSTATUS] |= 1;
        s->regs[REG_DFCNTRL] &= ~DFC_FIFOFLUSH;
        s->peer_data_in_pending = false;
        s->peer_data_len = s->peer_data_pos = 0;
        s->req = NULL;
        scsi_req_unref(req);
    }
}

static void aic7880_process_scsi_data(AIC7880State *s)
{
    SCSIRequest *req;
    bool from_target;

    aic7880_process_command(s);
    req = s->req;

    if (!req || !s->scsi_data_len || s->phase != AIC_PHASE_DATA ||
        s->peer_nexus_disconnected) {
        return;
    }
    if ((s->regs[REG_DFCNTRL] & 0x30) != 0x30) {
        return;
    }
    if (s->command_dma_wait) {
        if (s->expected_phase != (s->regs[REG_SCSISIGI] & 0xe0) ||
            !!(s->regs[REG_DFCNTRL] & 4) ==
            (req->cmd.mode == SCSI_XFER_FROM_DEV) || s->command_fifo_stale ||
            (req->cmd.mode == SCSI_XFER_FROM_DEV && s->host_fifo_len)) {
            return;
        }
        s->command_dma_wait = false;
    }
    if (s->regs[REG_SCSIRATE] & 0x8f) {
        aic7880_error(s, "synchronous or wide DATA is unsupported");
        scsi_req_cancel(req);
        return;
    }
    from_target = req->cmd.mode == SCSI_XFER_FROM_DEV;
    if (!!(s->regs[REG_DFCNTRL] & 0x04) == from_target) {
        aic7880_error(s, "SCSI data direction does not match target");
        scsi_req_cancel(req);
        return;
    }
    for (;;) {
        uint32_t stcnt = aic7880_count(s, REG_STCNT);
        uint32_t len = MIN(s->scsi_data_len - s->scsi_data_pos, stcnt);
        uint8_t *data = scsi_req_get_buf(req) + s->scsi_data_pos;

        aic7880_host_dma(s);
        if (s->error_pending) {
            scsi_req_cancel(req);
            return;
        }
        if ((s->regs[REG_DFCNTRL] & 0x30) != 0x30) {
            return;
        }
        if (from_target) {
            len = MIN(len, sizeof(s->host_fifo) - s->host_fifo_len);
            for (unsigned i = 0; i < len; i++) {
                s->host_fifo[s->host_fifo_write++] = data[i];
            }
            s->host_fifo_len += len;
        } else {
            len = MIN(len, s->host_fifo_len);
            for (unsigned i = 0; i < len; i++) {
                data[i] = s->host_fifo[s->host_fifo_read++];
            }
            s->host_fifo_len -= len;
        }
        s->scsi_data_pos += len;
        if (len) {
            aic7880_pio_present(s);
        }
        aic7880_advance_scsi(s, len);
        aic7880_fifo_status(s);
        aic7880_host_dma(s);
        if (s->error_pending) {
            scsi_req_cancel(req);
            return;
        }
        if (s->scsi_data_pos == s->scsi_data_len) {
            if (from_target && s->host_fifo_len && !req->residual &&
                !aic7880_count(s, REG_STCNT)) {
                return;
            }
            s->scsi_data_len = s->scsi_data_pos = 0;
            scsi_req_continue(req);
            return;
        }
        if (!len) {
            return;
        }
    }
}

static void aic7880_transfer_data(SCSIRequest *req, uint32_t len)
{
    AIC7880State *s = req->hba_private;
    bool from_target = req->cmd.mode == SCSI_XFER_FROM_DEV;

    s->phase = AIC_PHASE_DATA;
    s->regs[REG_SCSISIGI] = from_target ? SCSI_SIG_DATA_IN :
                             SCSI_SIG_DATA_OUT;
    if (s->test_peer && s->peer_hold_data_in && from_target && len) {
        s->peer_hold_data_in = false;
        s->peer_data_in_pending = true;
        s->peer_data_len = len;
        s->peer_data_pos = 0;
        aic7880_pio_present(s);
        return;
    }
    if (s->scsi_data_len) {
        aic7880_error(s, "overlapping SCSI data buffers");
        scsi_req_cancel(req);
        return;
    }
    if (!len) {
        scsi_req_continue(req);
        return;
    }
    s->peer_nexus_saved = false;
    s->scsi_data_len = len;
    s->scsi_data_pos = 0;
    aic7880_pio_present(s);
    aic7880_update_scsi_irq(s);
    aic7880_process_scsi_data(s);
}

static void aic7880_start_request(AIC7880State *s)
{
    SCSIDevice *target = scsi_device_find(&s->bus, 0, s->target, s->lun);
    int32_t length;

    if (!target) {
        s->regs[REG_SSTAT1] |= 0x80;
        aic7880_update_scsi_irq(s);
        return;
    }
    s->req = scsi_req_new(target, 0, s->lun, s->cdb, s->cdb_need, s);
    length = scsi_req_enqueue(s->req);
    if (length && s->req) {
        scsi_req_continue(s->req);
    }
}

static bool aic7880_collect_cdb(AIC7880State *s, uint8_t value)
{
    if (!s->selected || s->req || s->cdb_len >= sizeof(s->cdb) ||
        (s->cdb_need && s->cdb_len == s->cdb_need)) {
        aic7880_error(s, "CDB byte without an available selected command");
        return false;
    }
    if (!s->cdb_len) {
        int length = scsi_cdb_length(&value);

        if (length != 6 && length != 10 && length != 12 && length != 16) {
            aic7880_error(s, "unsupported CDB length");
            return false;
        }
        s->cdb_need = length;
    }
    s->cdb[s->cdb_len++] = value;
    return true;
}

static void aic7880_process_command(AIC7880State *s)
{
    uint32_t count;

    if (!s->selected || s->phase != AIC_PHASE_COMMAND || s->req ||
        s->error_pending || !(s->regs[REG_DFCNTRL] & 0x10)) {
        return;
    }
    count = aic7880_count(s, REG_STCNT);
    if (!count) {
        s->regs[REG_SSTAT0] |= 4;
        aic7880_dma_done(s);
        return;
    }
    if (!(s->regs[REG_DFCNTRL] & 0x20)) {
        return;
    }
    if (!(s->regs[REG_DFCNTRL] & 4)) {
        aic7880_error(s, "COMMAND DMA direction is unsupported");
        return;
    }
    if (s->expected_phase != 0x80) {
        aic7880_update_scsi_irq(s);
        return;
    }
    while (count && !s->error_pending) {
        aic7880_host_dma(s);
        if (s->error_pending || !s->host_fifo_len) {
            break;
        }
        if (!aic7880_collect_cdb(s, s->host_fifo[s->host_fifo_read])) {
            break;
        }
        s->host_fifo_read++;
        s->host_fifo_len--;
        s->command_dma_wait = true;
        aic7880_pio_present(s);
        aic7880_advance_scsi(s, 1);
        aic7880_fifo_status(s);
        count--;
        if (s->cdb_len == s->cdb_need) {
            /* Publish COMMAND progress before the target can call back. */
            s->command_fifo_stale = s->host_fifo_len != 0;
            aic7880_start_request(s);
            return;
        }
    }
    aic7880_update_scsi_irq(s);
}

static void aic7880_release_identity(AIC7880State *s)
{
    s->identity_valid = false;
    s->target = s->initiator = 0;
    s->selection_wide = false;
}

static void aic7880_bus_free_qualify(AIC7880State *s)
{
    int64_t origin = aic7880_event_time_ns(s);

    timer_mod(s->bus_free_timer, origin + 400);
    trace_aic7880_bus_free(PCI_DEVICE(s)->devfn, false, origin,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                          s->regs[REG_SSTAT1], s->regs[REG_SIMODE1]);
}

static void aic7880_bus_free_timer_cb(void *opaque)
{
    AIC7880State *s = opaque;

    if (!s->selected && !s->selection_pending && !s->peer_reselection &&
        (!s->req || s->peer_nexus_disconnected)) {
        if (!s->peer_nexus_disconnected) {
            aic7880_release_identity(s);
        }
        s->expected_phase = 0;
        s->signal_outputs = 0;
        s->initiator_atn = false;
        aic7880_manual_clear(s);
        s->regs[REG_SSTAT1] |= 0x08;
        trace_aic7880_bus_free(PCI_DEVICE(s)->devfn, true,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                              s->regs[REG_SSTAT1], s->regs[REG_SIMODE1]);
        aic7880_update_scsi_irq(s);
    }
}

static void aic7880_selection_timer_cb(void *opaque)
{
    AIC7880State *s = opaque;

    if (!s->selection_pending) {
        return;
    }
    s->selection_pending = false;
    s->regs[REG_SCSISIGI] &= ~0x08;
    s->regs[REG_SSTAT1] |= 0x80;
    aic7880_bus_free_qualify(s);
    aic7880_update_scsi_irq(s);
}

static void aic7880_selection_timer_update(AIC7880State *s, uint8_t old)
{
    uint8_t control = s->regs[REG_SXFRCTL1];
    int64_t interval;

    if (!s->selection_pending || !(control & 0x04)) {
        timer_del(s->selection_timer);
        return;
    }
    if (timer_pending(s->selection_timer) && !((old ^ control) & 0x1c)) {
        return;
    }
    /* Selected timing policy; the sibling's first table entry conflicts. */
    interval = 256000000LL >> ((control >> 3) & 3);
    timer_mod(s->selection_timer,
              aic7880_event_time_ns(s) + interval);
}

static void aic7880_cancel_selection(AIC7880State *s)
{
    timer_del(s->selection_timer);
    if (s->selection_pending) {
        s->selection_pending = false;
        s->regs[REG_SCSISIGI] &= ~0x08;
        aic7880_bus_free_qualify(s);
    }
}

static void aic7880_select(AIC7880State *s)
{
    uint8_t target = (s->regs[REG_SCSIID] >> 4) & 0x0f;
    uint8_t initiator = s->regs[REG_SCSIID] & 0x0f;
    bool wide = s->regs[REG_SBLKCTL] & SBLK_SELWIDE;

    if (target == initiator || (!wide && initiator > 7)) {
        aic7880_error(s, "unsupported initiator selection address");
        return;
    }
    if (s->host_fifo_len) {
        aic7880_error(s, "SCSI selection with host-only FIFO data");
        return;
    }
    if (s->selected || s->selection_pending || s->req || s->peer_reselection) {
        aic7880_error(s, "selection while SCSI bus is occupied");
        return;
    }
    timer_del(s->selection_timer);
    timer_del(s->bus_free_timer);
    s->target = target;
    s->initiator = initiator;
    s->selection_wide = wide;
    s->identity_valid = true;
    s->peer_nexus_disconnected = false;
    s->peer_nexus_saved = false;
    s->peer_nexus_message = 0;
    s->lun = 0;
    s->disconnect_allowed = false;
    s->cdb_len = s->cdb_need = 0;
    s->command_dma_wait = false;
    s->command_fifo_stale = false;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->message_out_len = 0;
    s->message_resume_command = false;
    s->selected = (wide || target < 8) &&
                  scsi_device_find(&s->bus, 0, target, 0) != NULL;
    if (s->selected) {
        s->initiator_atn = s->regs[REG_SCSISEQ] & 0x08;
        s->phase = s->initiator_atn ? AIC_PHASE_MSG_OUT : AIC_PHASE_COMMAND;
        s->regs[REG_SCSISIGI] = s->initiator_atn ? SCSI_SIG_MSG_OUT :
                                 SCSI_SIG_COMMAND;
        s->regs[REG_SSTAT0] = (s->regs[REG_SSTAT0] & ~0x10) | 0x42;
        s->regs[REG_SSTAT1] &= ~0x08;
        aic7880_pio_present(s);
        aic7880_update_scsi_irq(s);
    } else {
        s->selection_pending = true;
        s->regs[REG_SCSISIGI] = 0x08;
        s->regs[REG_SSTAT0] = (s->regs[REG_SSTAT0] & ~0x42) | 0x10;
        aic7880_selection_timer_update(s, 0);
        aic7880_update_scsi_irq(s);
    }
}

static bool aic7880_message_complete(AIC7880State *s)
{
    return s->message_out_len >= 2 &&
           s->message_out_len == s->message_out[1] + 2;
}

static void aic7880_finish_message_out(AIC7880State *s)
{
    if (s->phase != AIC_PHASE_MSG_OUT || !s->identify_seen ||
        s->initiator_atn ||
        (s->message_out_len && !aic7880_message_complete(s))) {
        return;
    }
    if (aic7880_message_complete(s)) {
        s->phase = AIC_PHASE_MSG_IN;
        s->message_resume_command = true;
        s->regs[REG_SCSIDATL] = 7; /* MESSAGE REJECT. */
        s->regs[REG_SCSISIGI] = SCSI_SIG_MSG_IN;
    } else {
        s->phase = AIC_PHASE_COMMAND;
        s->regs[REG_SCSISIGI] = SCSI_SIG_COMMAND;
    }
    aic7880_pio_present(s);
    aic7880_update_scsi_irq(s);
}

static void aic7880_message_out(AIC7880State *s, uint8_t val)
{
    unsigned n = s->message_out_len;

    if (!s->identify_seen) {
        if ((val & 0xb8) != 0x80) {
            aic7880_error(s, "unsupported Message Out byte");
            return;
        }
        s->lun = val & 7;
        s->disconnect_allowed = val & 0x40;
        s->identify_seen = true;
    } else {
        if (aic7880_message_complete(s) || n == sizeof(s->message_out) ||
            (!n && val != 1) || (n == 1 && val != 2 && val != 3) ||
            (n == 2 && val != (s->message_out[1] == 3 ? 1 : 3))) {
            aic7880_error(s, "unsupported extended Message Out");
            return;
        }
        s->message_out[s->message_out_len++] = val;
    }
    aic7880_pio_present(s);
    aic7880_finish_message_out(s);
}

static void aic7880_scsi_reset(AIC7880State *s)
{
    timer_del(s->selection_timer);
    timer_del(s->bus_free_timer);
    s->selection_pending = false;
    aic7880_release_identity(s);
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    bus_cold_reset(BUS(&s->bus));
    s->selected = false;
    s->lun = 0;
    s->disconnect_allowed = false;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->message_out_len = 0;
    s->message_resume_command = false;
    s->peer_reselection = false;
    s->peer_nexus_disconnected = false;
    s->peer_nexus_saved = false;
    s->peer_nexus_message = 0;
    s->peer_hold_data_in = false;
    s->peer_data_in_pending = false;
    s->peer_faulted_command = false;
    s->peer_data_len = s->peer_data_pos = 0;
    s->phase = AIC_PHASE_BUS_FREE;
    s->signal_outputs = 0;
    aic7880_manual_clear(s);
    s->expected_phase = 0;
    s->cdb_len = s->cdb_need = 0;
    s->command_dma_wait = false;
    s->command_fifo_stale = false;
    s->regs[REG_SCSISEQ] = 1;
    s->regs[REG_SCSISIGI] = 0;
    s->regs[REG_SSTAT0] = 0;
    s->regs[REG_SSTAT1] = 0x08 | (s->regs[REG_SSTAT1] & 0x04);
    aic7880_update_scsi_irq(s);
}

static void aic7880_chip_reset(AIC7880State *s)
{
    aic7880_seq_stop(s, "reset");
    s->seq_deadline_ns = 0;
    timer_del(s->selection_timer);
    timer_del(s->bus_free_timer);
    s->selection_pending = false;
    aic7880_release_identity(s);
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->cdb, 0, sizeof(s->cdb));
    s->error = 0;
    s->parity_break_pending = false;
    s->error_serial = 0;
    s->cdb_len = s->cdb_need = s->target = 0;
    s->command_dma_wait = false;
    s->command_fifo_stale = false;
    s->phase = AIC_PHASE_BUS_FREE;
    s->signal_outputs = 0;
    aic7880_manual_clear(s);
    s->expected_phase = 0;
    s->qout_head = s->qout_tail = s->qout_count = 0;
    s->qin_head = s->qin_tail = s->qin_count = 0;
    memset(s->return_pc, 0, sizeof(s->return_pc));
    s->return_ptr = 0;
    s->stack_byte = 0;
    s->scsi_data_len = s->scsi_data_pos = 0;
    s->host_fifo_read = 0;
    s->host_fifo_write = 0;
    s->host_fifo_len = 0;
    s->host_dma_active = false;
    s->host_request_pending = false;
    s->selected = false;
    s->lun = 0;
    s->disconnect_allowed = false;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->message_out_len = 0;
    s->message_resume_command = false;
    s->peer_reselection = false;
    s->peer_nexus_disconnected = false;
    s->peer_nexus_saved = false;
    s->peer_nexus_message = 0;
    s->peer_scsi_irq_latched = false;
    s->peer_hold_data_in = false;
    s->peer_data_in_pending = false;
    s->peer_last_parity_bad = false;
    s->peer_faulted_command = false;
    s->peer_data_len = s->peer_data_pos = 0;
    s->maskable_break_pending = false;
    s->error_pending = false;
    s->regs[REG_DFSTATUS] = DFS_FIFOQWDEMP | DFS_FIFOEMP;
    s->regs[REG_SBLKCTL] = 0xc2; /* Selected native wide connection. */
    s->regs[REG_SEQCTL] = 0x90;
    s->regs[REG_BRKADDR1] = 0x80;
    s->regs[REG_HCNTRL] = HOST_PAUSE | HOST_CHIPRST;
    s->seqaddr = 0;
    s->seqbyte = 0;
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void aic7880_reset_hold(Object *obj, ResetType type)
{
    AIC7880State *s = AIC7880(obj);

    memset(s->seqram, 0, sizeof(s->seqram));
    memset(s->scb_ram, 0, sizeof(s->scb_ram));
    if (s->external_scb_ram) {
        pci_set_long(PCI_DEVICE(s)->config + 0x40, 0x600);
    }
    aic7880_chip_reset(s);
}

static uint8_t aic7880_pci_status(AIC7880State *s)
{
    uint16_t status = pci_get_word(PCI_DEVICE(s)->config + PCI_STATUS);

    return ((status >> 11) & 0x1f) | ((status >> 3) & 0x20);
}

static uint8_t aic7880_error_status(AIC7880State *s)
{
    return s->error | (aic7880_pci_status(s) ? 0x40 : 0);
}

static void aic7880_nexus_message(AIC7880State *s, uint8_t message)
{
    s->peer_nexus_message = message;
    s->phase = AIC_PHASE_MSG_IN;
    s->regs[REG_SCSISIGI] = SCSI_SIG_MSG_IN;
    s->regs[REG_SCSIDATL] = message;
    aic7880_pio_present(s);
    aic7880_update_scsi_irq(s);
}

static void aic7880_nexus_message_done(AIC7880State *s)
{
    uint8_t message = s->peer_nexus_message;

    s->peer_nexus_message = 0;
    if (message == 4) { /* DISCONNECT. */
        s->peer_nexus_disconnected = true;
        s->selected = false;
        s->initiator_atn = false;
        s->phase = AIC_PHASE_BUS_FREE;
        s->signal_outputs = 0;
        aic7880_manual_clear(s);
        s->expected_phase = 0;
        s->regs[REG_SCSISIGI] = 0;
        s->regs[REG_SSTAT0] &= ~0x62;
        aic7880_bus_free_qualify(s);
    } else {
        if (message == 2) { /* SAVE DATA POINTER. */
            s->peer_nexus_saved = true;
            s->peer_nexus_saved_pos = s->scsi_data_pos;
        } else if (message == 3) { /* RESTORE POINTERS. */
            s->scsi_data_pos = s->peer_nexus_saved_pos;
        } else {
            s->peer_nexus_disconnected = false; /* IDENTIFY accepted. */
        }
        s->phase = AIC_PHASE_DATA;
        s->regs[REG_SCSISIGI] = s->peer_nexus_signal;
    }
    aic7880_pio_present(s);
    aic7880_update_scsi_irq(s);
    if (message != 4) {
        aic7880_process_scsi_data(s);
    }
}

static unsigned aic7880_queue_slots(AIC7880State *s)
{
    return s->external_scb_ram ? 256 : AIC_SCB_PAGES;
}

static unsigned aic7880_queue_limit(AIC7880State *s)
{
    return s->external_scb_ram ? 255 : AIC_SCB_PAGES;
}

static unsigned aic7880_scb_index(AIC7880State *s, unsigned offset)
{
    /* High internal page bits alias under the selected model approximation. */
    unsigned pages = s->external_scb_ram ?
                     s->external_scb_size / AIC_SCB_PAGE_BYTES : AIC_SCB_PAGES;

    return (s->regs[REG_SCBPTR] % pages) * AIC_SCB_PAGE_BYTES + offset;
}

static uint8_t aic7880_scb_parity(uint8_t value)
{
    return 1 ^ parity8(value);
}

static bool aic7880_scb_parity_bad(AIC7880State *s, unsigned index)
{
    return s->external_scb_width == 9 &&
           (PCI_DEVICE(s)->config[0x40] & 0x10) &&
           (s->regs[REG_DSCOMMAND] & 0x20) &&
           s->scb_parity[index] != aic7880_scb_parity(s->scb_ram[index]);
}

static void aic7880_check_scb_parity(AIC7880State *s, unsigned index)
{
    if (!aic7880_scb_parity_bad(s, index)) {
        return;
    }
    s->error |= 0x20;
    if (!(s->regs[REG_SEQCTL] & SEQ_FAILDIS)) {
        s->parity_break_pending = true;
        s->regs[REG_INTSTAT] |= 8;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_seq_stop(s, "pause");
    }
    aic7880_update_irq(s);
}

static void aic7880_pio_advance(AIC7880State *s)
{
    if (s->phase == AIC_PHASE_DATA) {
        if (s->req && s->scsi_data_pos == s->scsi_data_len) {
            SCSIRequest *req = s->req;

            s->scsi_data_len = s->scsi_data_pos = 0;
            aic7880_pio_present(s);
            aic7880_update_scsi_irq(s);
            scsi_req_continue(req);
        } else {
            aic7880_pio_present(s);
            aic7880_update_scsi_irq(s);
        }
        return;
    }
    if (s->phase == AIC_PHASE_MSG_IN && s->peer_nexus_message) {
        aic7880_nexus_message_done(s);
    } else if (s->phase == AIC_PHASE_STATUS) {
        s->phase = AIC_PHASE_MSG_IN;
        s->regs[REG_SCSISIGI] = SCSI_SIG_MSG_IN;
        s->regs[REG_SCSIDATL] = 0; /* Command Complete. */
        aic7880_pio_present(s);
        aic7880_update_scsi_irq(s);
    } else if (s->phase == AIC_PHASE_MSG_IN && s->message_resume_command) {
        s->message_resume_command = false;
        s->message_out_len = 0;
        s->phase = AIC_PHASE_COMMAND;
        s->regs[REG_SCSISIGI] = SCSI_SIG_COMMAND;
        aic7880_pio_present(s);
        aic7880_update_scsi_irq(s);
    } else if (s->phase == AIC_PHASE_MSG_IN) {
        s->phase = AIC_PHASE_BUS_FREE;
        s->signal_outputs = 0;
        aic7880_manual_clear(s);
        s->expected_phase = 0;
        s->initiator_atn = false;
        s->identify_seen = false;
        s->message_out_len = 0;
        s->message_resume_command = false;
        s->regs[REG_SCSISIGI] = 0;
        s->regs[REG_SSTAT0] &= ~0x62;
        s->selected = false;
        aic7880_release_identity(s);
        s->lun = 0;
        s->disconnect_allowed = false;
        aic7880_bus_free_qualify(s);
        aic7880_pio_present(s);
        aic7880_update_scsi_irq(s);
    }
}

static bool aic7880_manual_admitted(AIC7880State *s)
{
    if (!s->selected || (s->regs[REG_SXFRCTL0] & 8) ||
        (s->regs[REG_DFCNTRL] & 0x38) || s->peer_reselection ||
        s->peer_nexus_disconnected || s->peer_data_in_pending ||
        s->peer_faulted_command ||
        (s->phase != AIC_PHASE_DATA && s->phase != AIC_PHASE_STATUS &&
         s->phase != AIC_PHASE_MSG_IN)) {
        aic7880_error(s, "manual PIO without an admitted selected phase");
        return false;
    }
    if (s->phase == AIC_PHASE_DATA &&
        (s->host_fifo_len || s->host_dma_active || s->host_request_pending ||
         (s->regs[REG_SCSIRATE] & 0x8f))) {
        aic7880_error(s, "manual DATA conflicts with FIFO, DMA or mode");
        return false;
    }
    return true;
}

static bool aic7880_incoming(AIC7880State *s)
{
    return s->phase == AIC_PHASE_STATUS || s->phase == AIC_PHASE_MSG_IN ||
           (s->phase == AIC_PHASE_DATA && s->req &&
            s->req->cmd.mode == SCSI_XFER_FROM_DEV);
}

static uint8_t aic7880_bus_data(AIC7880State *s)
{
    if (s->signal_outputs & 1) {
        return s->manual_data;
    }
    if (s->phase == AIC_PHASE_DATA) {
        return scsi_req_get_buf(s->req)[s->scsi_data_pos];
    }
    return s->regs[REG_SCSIDATL];
}

static void aic7880_signal_write(AIC7880State *s, uint8_t val)
{
    bool rising = (val & 1) && !(s->signal_outputs & 1);
    bool falling = !(val & 1) && (s->signal_outputs & 1);
    bool new_atn = (val & SCSI_SIG_ATN) && !s->initiator_atn;

    if (new_atn && (!s->selected || s->peer_nexus_disconnected ||
                    s->req || s->cdb_len ||
                    (s->phase != AIC_PHASE_MSG_OUT &&
                     s->phase != AIC_PHASE_COMMAND))) {
        aic7880_error(s, "unsupported SCSISIGO attention request");
        return;
    }
    if (rising && (!aic7880_manual_admitted(s) ||
                   !(s->regs[REG_SCSISIGI] & 2) || !s->manual_valid)) {
        aic7880_error(s, "manual ACK without a sampled or staged request");
        return;
    }
    s->expected_phase = val & 0xe0;
    aic7880_pio_phase(s);
    if (new_atn) {
        s->initiator_atn = true;
        if (s->phase == AIC_PHASE_COMMAND && !s->req && !s->cdb_len) {
            s->phase = AIC_PHASE_MSG_OUT;
            s->identify_seen = true;
            s->message_out_len = 0;
            s->message_resume_command = false;
            s->regs[REG_SCSISIGI] = SCSI_SIG_MSG_OUT;
            aic7880_pio_present(s);
        }
    }
    /* A new expected phase qualifies this ACK before it can consume data. */
    if (rising && (s->regs[REG_SSTAT1] & 0x10)) {
        s->signal_outputs = val & 0x0c;
        aic7880_update_scsi_irq(s);
        return;
    }
    s->signal_outputs = val & 0x0d; /* Initiator REQO has no effect. */
    if (rising) {
        if (s->phase == AIC_PHASE_DATA) {
            if (!aic7880_incoming(s)) {
                scsi_req_get_buf(s->req)[s->scsi_data_pos] = s->manual_data;
            }
            s->scsi_data_pos++;
        }
        s->regs[REG_SCSISIGI] &= ~2;
        s->regs[REG_SSTAT1] &= ~0x11;
        s->regs[REG_SSTAT0] &= ~2;
    } else if (falling) {
        aic7880_manual_clear(s);
        aic7880_pio_advance(s);
    }
    aic7880_update_scsi_irq(s);
}

static uint64_t aic7880_read(void *opaque, hwaddr addr, unsigned size)
{
    AIC7880State *s = opaque;

    addr &= AIC_REG_SPACE - 1;
    if (addr == REG_HCNTRL || addr == REG_INTSTAT ||
        addr == REG_ERROR || addr == REG_QOUTCNT) {
        uint8_t value = addr == REG_QOUTCNT ? s->qout_count :
                        addr == REG_ERROR ? aic7880_error_status(s) :
                        s->regs[addr];

        trace_aic7880_host_reg(PCI_DEVICE(s)->devfn, false, addr, value,
                               ldl_le_p(&s->regs[0x88]),
                               aic7880_count(s, 0x8c),
                               s->regs[REG_DFCNTRL], s->regs[REG_INTSTAT]);
    }

    if (addr == REG_SCSIDATL) {
        uint8_t data;

        if (!s->running && !(s->regs[REG_HCNTRL] & HOST_PAUSE)) {
            aic7880_error(s, "unpaused host SCSIDATL read");
            return 0xff;
        }
        if (!aic7880_incoming(s) || s->peer_reselection ||
            s->peer_data_in_pending || s->peer_faulted_command) {
            aic7880_error(s, "SCSIDATL read outside incoming PIO phase");
            return 0xff;
        }
        if (!(s->regs[REG_SXFRCTL0] & 8)) {
            if (!aic7880_manual_admitted(s) ||
                (!(s->signal_outputs & 1) &&
                 !(s->regs[REG_SCSISIGI] & 2))) {
                aic7880_error(s, "manual input without target REQ");
                return 0xff;
            }
            data = aic7880_bus_data(s);
            if (!(s->signal_outputs & 1)) {
                s->manual_data = data;
                s->manual_valid = true;
            }
            return data;
        }
        data = s->regs[REG_SCSIDATL];
        if (s->phase == AIC_PHASE_DATA) {
            aic7880_error(s, "automatic DATA PIO is unsupported");
            return 0xff;
        }
        if (aic7880_pio_enabled(s)) {
            aic7880_pio_advance(s);
        }
        return data;
    }
    if (addr >= REG_SCB_BASE &&
        addr < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        unsigned offset = (s->regs[REG_SCBCNT] & 0x80) ?
                          s->regs[REG_SCBCNT] & 0x1f : addr - REG_SCB_BASE;
        unsigned index = aic7880_scb_index(s, offset);
        uint8_t data = s->scb_ram[index];

        if (s->external_scb_ram) {
            aic7880_check_scb_parity(s, index);
        }
        if (s->regs[REG_SCBCNT] & 0x80) {
            s->regs[REG_SCBCNT] = 0x80 | ((offset + 1) & 0x1f);
        }
        return data;
    }
    if ((addr >= REG_SHADDR && addr < REG_SHADDR + 4) ||
        (addr >= 0x88 && addr <= 0x8e) ||
        addr == REG_DFCNTRL ||
        (addr >= 0x20 && addr <= 0x5f)) {
        return s->regs[addr];
    }
    if (addr == REG_DFSTATUS) {
        aic7880_fifo_status(s);
        trace_aic7880_host_reg(PCI_DEVICE(s)->devfn, false, addr,
                               s->regs[addr],
                               ldl_le_p(&s->regs[0x88]),
                               aic7880_count(s, 0x8c),
                               s->regs[REG_DFCNTRL], s->regs[REG_INTSTAT]);
        return s->regs[addr];
    }
    if (addr == REG_DSCOMMAND && s->external_scb_ram) {
        return s->regs[addr];
    }
    switch (addr) {
    case REG_ALLONES:
        return 0xff;
    case REG_ALLZEROS:
        return 0;
    case REG_FUNCTION1:
        return 1u << ((s->regs[addr] >> 4) & 7);
    case REG_SCSISIGI:
        return s->regs[addr] | s->signal_outputs |
               (s->initiator_atn ? SCSI_SIG_ATN : 0);
    case REG_SCSISEQ:
    case REG_SXFRCTL0:
    case REG_SXFRCTL1:
    case REG_SCSIID:
    case REG_STCNT:
    case REG_STCNT + 1:
    case REG_STCNT + 2:
    case REG_SCSIRATE:
    case REG_SSTAT1:
    case REG_SIMODE0:
    case REG_SIMODE1:
    case REG_SBLKCTL:
    case REG_SEQCTL:
    case REG_ACCUM:
    case REG_SINDEX:
    case REG_DINDEX:
    case REG_FLAGS:
    case REG_BRKADDR0:
    case REG_BRKADDR1:
    case REG_HCNTRL:
    case REG_SCBPTR:
    case REG_SCBCNT:
    case REG_INTSTAT:
        return s->regs[addr];
    case REG_SSTAT0:
        aic7880_dma_done(s);
        return s->regs[addr];
    case REG_SCSIBUSL:
        if (!s->running && !(s->regs[REG_HCNTRL] & HOST_PAUSE)) {
            aic7880_error(s, "unpaused host SCSIBUSL read");
            return 0xff;
        }
        if (s->phase == AIC_PHASE_DATA && aic7880_incoming(s) &&
            aic7880_manual_admitted(s) &&
            ((s->signal_outputs & 1) || (s->regs[REG_SCSISIGI] & 2))) {
            return aic7880_bus_data(s);
        }
        if (s->phase == AIC_PHASE_MSG_IN || s->phase == AIC_PHASE_STATUS) {
            return s->regs[REG_SCSIDATL];
        }
        aic7880_error(s, "unqualified SCSIBUSL data");
        return 0xff;
    case REG_DSPCISTATUS:
        return (s->regs[addr] & 0xc0) | aic7880_pci_status(s);
    case REG_SEQRAM:
        if (!(s->regs[REG_SEQCTL] & SEQ_LOADRAM) ||
            s->seqaddr >= AIC_SEQ_INSNS) {
            return 0xff;
        }
        addr = s->seqaddr * 4 + s->seqbyte;
        s->seqbyte = (s->seqbyte + 1) & 3;
        if (s->seqbyte == 0) {
            s->seqaddr++;
        }
        return s->seqram[addr];
    case REG_SEQADDR0:
        return s->seqaddr & 0xff;
    case REG_SEQADDR1:
        return (s->seqaddr >> 8) & 1;
    case REG_STACK: {
        uint8_t data = s->return_pc[s->return_ptr] >> (8 * s->stack_byte);

        if (s->stack_byte) {
            s->return_ptr = (s->return_ptr + 1) % ARRAY_SIZE(s->return_pc);
        }
        s->stack_byte ^= 1;
        return data;
    }
    case REG_ERROR:
        return aic7880_error_status(s);
    case REG_HCNT_RSVD:
        return 0;
    case REG_DFDAT:
        if (!aic7880_host_fifo_accessible(s)) {
            aic7880_error(s, "DFDAT mixed with SCSI data path");
            return 0xff;
        }
        if (!s->host_fifo_len) {
            aic7880_error(s, "DFDAT read from empty FIFO");
            return 0xff;
        }
        uint8_t data = s->host_fifo[s->host_fifo_read++];

        s->host_fifo_len--;
        aic7880_fifo_status(s);
        aic7880_host_dma(s);
        return data;
    case REG_QOUTFIFO:
        if (s->qout_count) {
            uint8_t id = s->qout[s->qout_head];

            s->qout_head = (s->qout_head + 1) % aic7880_queue_slots(s);
            s->qout_count--;
            return id;
        }
        return 0xff;
    case REG_QOUTCNT:
        return s->qout_count;
    case REG_QINCNT:
        return s->qin_count;
    case REG_DINDIR:
        qemu_log_mask(LOG_UNIMP,
                      "aic7880: host read of DINDIR is unsupported\n");
        aic7880_stop_diagnostic(s);
        return 0xff;
    default:
        qemu_log_mask(LOG_UNIMP, "aic7880: read of unsupported register "
                      "0x%02" HWADDR_PRIx "\n", addr);
        aic7880_stop_diagnostic(s);
        return 0xff;
    }
}

static void aic7880_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    AIC7880State *s = opaque;
    uint8_t val = value;

    addr &= AIC_REG_SPACE - 1;
    if (s->running && (addr == REG_SINDEX || addr == REG_DINDEX ||
                       addr == REG_SCBPTR || addr == REG_SCBCNT)) {
        uint16_t at = s->seqaddr - 1;

        trace_aic7880_seq_index_write(PCI_DEVICE(s)->devfn, at,
            ldl_le_p(&s->seqram[at * 4]), addr, val,
            s->regs[REG_SCBPTR], s->regs[REG_SCBCNT]);
    }
    if ((addr >= 0x88 && addr <= 0x8e) ||
        addr == REG_DFCNTRL || addr == REG_HCNTRL) {
        trace_aic7880_host_reg(PCI_DEVICE(s)->devfn, true, addr, val,
                               ldl_le_p(&s->regs[0x88]),
                               aic7880_count(s, 0x8c),
                               s->regs[REG_DFCNTRL], s->regs[REG_INTSTAT]);
    }

    if (addr >= REG_SCB_BASE &&
        addr < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        unsigned offset = (s->regs[REG_SCBCNT] & 0x80) ?
                          s->regs[REG_SCBCNT] & 0x1f : addr - REG_SCB_BASE;

        unsigned index = aic7880_scb_index(s, offset);

        s->scb_ram[index] = val;
        if (s->external_scb_ram && s->external_scb_width == 9) {
            s->scb_parity[index] = aic7880_scb_parity(val);
        }
        if (s->regs[REG_SCBCNT] & 0x80) {
            s->regs[REG_SCBCNT] = 0x80 | ((offset + 1) & 0x1f);
        }
        return;
    }
    if ((addr >= 0x20 && addr <= 0x5f) ||
        (addr >= REG_SHADDR && addr < REG_SHADDR + 4) ||
        (addr >= 0x88 && addr <= 0x8e) ||
        (addr >= REG_STCNT && addr < REG_STCNT + 3)) {
        s->regs[addr] = val;
        if (addr >= 0x88 && addr <= 0x8b) {
            s->regs[REG_SHADDR + addr - 0x88] = val;
        }
        if (addr >= REG_STCNT && addr < REG_STCNT + 3) {
            if (aic7880_count(s, REG_STCNT)) {
                s->regs[REG_SSTAT0] &= ~0x04;
            } else if (aic7880_command_dma_owned(s) &&
                       (s->regs[REG_DFCNTRL] & 0x10)) {
                s->regs[REG_SSTAT0] |= 0x04;
            }
            aic7880_dma_done(s);
            aic7880_update_scsi_irq(s);
        }
        if (addr >= 0x8c && addr <= 0x8e && aic7880_count(s, 0x8c)) {
            s->regs[REG_DFSTATUS] &= ~DFS_HDONE;
            aic7880_dma_done(s);
        }
        return;
    }
    if (addr == REG_DSCOMMAND && s->external_scb_ram) {
        if (val & ~0x20) {
            aic7880_error(s, "unsupported DSCOMMAND control");
        } else {
            s->regs[addr] = val;
        }
        return;
    }
    switch (addr) {
    case REG_SCSIRATE:
        s->regs[addr] = val;
        break;
    case REG_SSTAT0: /* CLRSINT0 on writes. */
        s->regs[REG_SSTAT0] &= ~(val & 0x7a);
        aic7880_update_scsi_irq(s);
        break;
    case REG_SSTAT1: /* CLRSINT1 on writes. */
        s->regs[REG_SSTAT1] &= ~(val & 0xef);
        if (val & 1) {
            s->regs[REG_SSTAT1] &= ~0x10;
        }
        if ((val & 0x04) && s->peer_last_parity_bad) {
            s->regs[REG_SSTAT1] |= 0x04;
        }
        if (val & 0x40) {
            s->initiator_atn = false;
            aic7880_finish_message_out(s);
        }
        aic7880_update_scsi_irq(s);
        break;
    case REG_SIMODE0:
        if (val & ~0x66) {
            aic7880_error(s, "unsupported SIMODE0 event mask");
            break;
        }
        s->regs[addr] = val;
        aic7880_update_scsi_irq(s);
        break;
    case REG_SIMODE1:
        if (val & ~0xbd) {
            aic7880_error(s, "unsupported SIMODE1 event mask");
            break;
        }
        s->regs[addr] = val;
        aic7880_update_scsi_irq(s);
        break;
    case REG_SXFRCTL0: {
        bool pio_was_enabled = s->regs[addr] & 8;

        if (val & ~0x9a) {
            aic7880_error(s, "unsupported SXFRCTL0 control");
            break;
        }
        if (((val ^ s->regs[addr]) & 8) && aic7880_manual_busy(s)) {
            aic7880_error(s, "PIO mode change during a manual byte");
            break;
        }
        if ((val & SXFR_CLRCHN) &&
            ((s->signal_outputs & 1) || (s->regs[REG_DFCNTRL] & 0x30))) {
            aic7880_error(s, "channel clear during ACK or SCSI DMA");
            break;
        }
        if (val & SXFR_CLRSTCNT) {
            memset(&s->regs[REG_STCNT], 0, 3);
            memset(&s->regs[REG_SHADDR], 0, 4);
            s->regs[REG_SSTAT0] &= ~0x04;
        }
        if (val & SXFR_CLRCHN) {
            /* The target request and PCI FIFO have separate ownership. */
            aic7880_manual_clear(s);
        }
        /* DFON is a latch; REQ#/ACK# filter timing is not modeled. */
        s->regs[addr] = val & 0x88;
        if (!(val & 0x08)) {
            s->regs[REG_SSTAT0] &= ~0x02;
            if (pio_was_enabled && s->phase == AIC_PHASE_DATA) {
                aic7880_pio_present(s);
            }
        } else if (!pio_was_enabled) {
            aic7880_pio_present(s);
        }
        if (!s->selected) {
            aic7880_pio_present(s);
        }
        aic7880_update_scsi_irq(s);
        break;
    }
    case REG_SXFRCTL1: {
        uint8_t old = s->regs[addr];

        if (val & ~0x3f) {
            aic7880_error(s, "unsupported SXFRCTL1 control");
            break;
        }
        /*
         * Ordinary targets lack parity metadata. ACTNEGEN/STPWEN are synthetic
         * electrical latches.
         */
        s->regs[addr] = val;
        aic7880_selection_timer_update(s, old);
        break;
    }
    case REG_SCSISIGI: /* SCSISIGO on writes. */
        if (!s->running && !(s->regs[REG_HCNTRL] & HOST_PAUSE)) {
            aic7880_error(s, "unpaused host SCSISIGO write");
            break;
        }
        aic7880_signal_write(s, val);
        aic7880_process_scsi_data(s);
        break;
    case REG_SBLKCTL:
        if (val & ~0xea) {
            aic7880_error(s, "unsupported SBLKCTL control");
            break;
        }
        if (((val ^ s->regs[addr]) & SBLK_SELWIDE) &&
            (s->identity_valid || s->req || s->peer_reselection ||
             s->phase == AIC_PHASE_DATA)) {
            aic7880_error(s, "connection width change on an occupied bus");
            break;
        }
        s->regs[addr] = val & ~0x08; /* SELBUSB is inert on one channel. */
        break;
    case REG_DFCNTRL: {
        bool command_was_owned = aic7880_command_dma_owned(s);

        if ((val & 0x38) && aic7880_manual_busy(s)) {
            aic7880_error(s, "DMA enable during a manual byte");
            break;
        }
        if (val & ~0x7f) {
            aic7880_error(s, "unsupported DFCNTRL control");
            break;
        }
        /* AIC-7870 DIRECTIONACK changes only with all enables clear. */
        if (s->regs[addr] & 0x38) {
            val = (val & ~0x04) | (s->regs[addr] & 0x04);
        }
        if ((s->regs[addr] & DFC_FIFOFLUSH) &&
            s->host_fifo_len &&
            !(val & (DFC_FIFORESET | 0x04))) {
            val |= DFC_FIFOFLUSH;
        }
        if (!(val & 0x10)) {
            s->regs[REG_SSTAT0] &= ~4;
        }
        s->regs[addr] = val & ~DFC_FIFORESET;
        if (command_was_owned != aic7880_command_dma_owned(s)) {
            aic7880_pio_present(s);
        }
        if (val & DFC_FIFORESET) {
            if (s->host_fifo_len && s->req && !s->command_fifo_stale &&
                !s->command_dma_wait) {
                scsi_req_cancel(s->req);
            }
            s->host_fifo_read = s->regs[0x88] & 7;
            s->host_fifo_write = s->host_fifo_read;
            s->host_fifo_len = 0;
            s->command_fifo_stale = false;
            s->host_request_pending = false;
            s->regs[REG_DFSTATUS] =
                (s->regs[REG_DFSTATUS] & ~DFS_HDONE) |
                DFS_FIFOQWDEMP | DFS_FIFOEMP;
            aic7880_fifo_status(s);
        }
        if ((val & DFC_FIFOFLUSH) &&
            (!s->host_fifo_len || (s->regs[addr] & 0x04))) {
            s->regs[addr] &= ~DFC_FIFOFLUSH;
        }
        if (s->phase == AIC_PHASE_DATA && (val & 0x38)) {
            aic7880_pio_present(s);
        }
        aic7880_host_dma(s);
        aic7880_process_scsi_data(s);
        aic7880_update_scsi_irq(s);
        break;
    }
    case REG_DFDAT:
        if (!aic7880_host_fifo_accessible(s)) {
            aic7880_error(s, "DFDAT mixed with SCSI data path");
            break;
        }
        if (s->host_fifo_len == sizeof(s->host_fifo)) {
            aic7880_error(s, "DFDAT write to full FIFO");
            break;
        }
        s->host_fifo[s->host_fifo_write++] = val;
        s->host_fifo_len++;
        aic7880_fifo_status(s);
        aic7880_host_dma(s);
        break;
    case REG_SCSIID:
    case REG_SCBPTR:
    case REG_SINDEX:
    case REG_DINDEX:
    case REG_BRKADDR0:
    case REG_ACCUM:
    case REG_FUNCTION1:
        s->regs[addr] = val;
        break;
    case REG_ALLZEROS: /* NONE on writes. */
        break;
    case REG_DSPCISTATUS:
        if (s->running) {
            aic7880_error(s, "sequencer write to DSPCISTATUS is unsupported");
            break;
        }
        s->regs[addr] = val & 0xc0;
        aic7880_host_dma(s);
        aic7880_process_scsi_data(s);
        break;
    case REG_HCNT_RSVD:
        if (s->running) {
            aic7880_error(s, "sequencer write to reserved HCNT byte");
        }
        break;
    case REG_DINDIR:
        qemu_log_mask(LOG_UNIMP,
                      "aic7880: host write to DINDIR is unsupported\n");
        aic7880_stop_diagnostic(s);
        break;
    case REG_FLAGS:
        aic7880_error(s, "FLAGS is read-only");
        break;
    case REG_BRKADDR1:
        if (val & 0x7e) {
            aic7880_error(s, "unsupported breakpoint control");
            break;
        }
        s->regs[addr] = val;
        break;
    case REG_SCBCNT:
        s->regs[addr] = val & 0x9f;
        break;
    case REG_QINFIFO: {
        unsigned int count_before = s->qin_count;
        bool paused = s->regs[REG_HCNTRL] & HOST_PAUSE;

        if (s->external_scb_ram &&
            s->qin_count == aic7880_queue_limit(s)) {
            break;
        }
        if (s->running || (!s->external_scb_ram && val >= AIC_SCB_PAGES) ||
            s->qin_count == aic7880_queue_limit(s)) {
            trace_aic7880_qinfifo(PCI_DEVICE(s)->devfn, true, val,
                                  count_before, count_before, paused,
                                  s->running, false);
            aic7880_error(s, "unsupported queue-in entry");
            break;
        }
        s->qin[s->qin_tail] = val;
        s->qin_tail = (s->qin_tail + 1) % aic7880_queue_slots(s);
        s->qin_count++;
        trace_aic7880_qinfifo(PCI_DEVICE(s)->devfn, true, val,
                              count_before, s->qin_count, paused,
                              s->running, true);
        if (!(s->regs[REG_HCNTRL] & HOST_PAUSE) &&
            !(s->regs[REG_SEQCTL] & SEQ_LOADRAM)) {
            aic7880_run(s);
        }
        break;
    }
    case REG_SCSISEQ: {
        bool select = (val & 0x40) && !(s->regs[addr] & 0x40);

        if (val != 1 && (val & ~0x5a)) {
            aic7880_error(s, "unsupported SCSISEQ control");
            break;
        }
        if ((val & 0x40) && !s->running) {
            aic7880_error(s, "manual SCSI selection is unsupported");
            break;
        }
        if ((val & 1) && !(s->regs[addr] & 1)) {
            aic7880_scsi_reset(s);
        }
        s->regs[addr] = val;
        if (select) {
            aic7880_select(s);
        } else if (!(val & 0x40)) {
            aic7880_cancel_selection(s);
        }
        break;
    }
    case REG_SCSIDATL:
        if (aic7880_command_dma_owned(s)) {
            aic7880_error(s, "command PIO while DMA owns the byte path");
            break;
        }
        if (!s->running && !(s->regs[REG_HCNTRL] & HOST_PAUSE)) {
            aic7880_error(s, "unpaused host SCSIDATL write");
            break;
        }
        if (s->signal_outputs & 1) {
            aic7880_error(s, "SCSIDATL write while ACK is asserted");
            break;
        }
        if (s->phase == AIC_PHASE_DATA &&
            !(s->regs[REG_SXFRCTL0] & 8)) {
            if (!aic7880_manual_admitted(s) || aic7880_incoming(s) ||
                !(s->regs[REG_SCSISIGI] & 2)) {
                aic7880_error(s, "manual output without target DATA OUT REQ");
                break;
            }
            s->manual_data = val;
            s->manual_valid = true;
            break;
        }
        if (s->phase == AIC_PHASE_STATUS || s->phase == AIC_PHASE_MSG_IN) {
            aic7880_error(s, "SCSIDATL write in incoming PIO phase");
            break;
        }
        if (aic7880_pio_pending(s) && !aic7880_pio_enabled(s)) {
            break;
        }
        if (!s->running && s->phase != AIC_PHASE_MSG_OUT) {
            aic7880_error(s, "manual command PIO is unsupported");
            break;
        }
        if (s->phase == AIC_PHASE_MSG_OUT && s->selected) {
            aic7880_message_out(s, val);
            break;
        }
        if (s->phase != AIC_PHASE_COMMAND) {
            aic7880_error(s, "command PIO outside COMMAND phase");
            break;
        }
        s->regs[REG_SSTAT0] &= ~0x02;
        s->regs[REG_SSTAT1] &= ~0x11;
        s->regs[REG_SCSISIGI] &= ~0x02;
        if (!aic7880_collect_cdb(s, val)) {
            break;
        }
        if (s->cdb_len == s->cdb_need) {
            aic7880_start_request(s);
        } else {
            aic7880_pio_present(s);
        }
        aic7880_update_scsi_irq(s);
        break;
    case REG_SEQCTL: {
        uint8_t old = s->regs[REG_SEQCTL];

        if (!s->running && (val & SEQ_PAUSEDIS)) {
            qemu_log_mask(LOG_UNIMP,
                          "aic7880: unsupported host SEQCTL control 0x%02x\n",
                          val);
            aic7880_stop_diagnostic(s);
            break;
        }
        if (val & SEQ_LOADRAM) {
            aic7880_seq_stop(s, "load");
        }
        if (val & SEQ_RESET) {
            s->seqaddr = 0;
            s->seqbyte = 0;
        }
        if ((val ^ s->regs[REG_SEQCTL]) & SEQ_LOADRAM) {
            s->seqbyte = 0;
        }
        s->regs[REG_SEQCTL] = val & ~SEQ_RESET;
        if ((old ^ val) & SEQ_FASTMODE) {
            trace_aic7880_seq_clock(PCI_DEVICE(s)->devfn, "rate",
                s->seq_deadline_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                aic7880_seq_period_ns(s), s->regs[REG_HCNTRL],
                s->regs[REG_SEQCTL], s->regs[REG_INTSTAT]);
        }
        aic7880_update_irq(s);
        aic7880_run(s);
        break;
    }
    case REG_SEQRAM:
        if (!(s->regs[REG_SEQCTL] & SEQ_LOADRAM) ||
            s->seqaddr >= AIC_SEQ_INSNS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "aic7880: sequencer RAM write outside load mode\n");
            return;
        }
        s->seqram[s->seqaddr * 4 + s->seqbyte] = val;
        s->seqbyte = (s->seqbyte + 1) & 3;
        if (s->seqbyte == 0) {
            s->seqaddr++;
        }
        break;
    case REG_SEQADDR0:
        s->seqaddr = (s->seqaddr & 0x100) | val;
        s->seqbyte = 0;
        break;
    case REG_SEQADDR1:
        s->seqaddr = (s->seqaddr & 0xff) | ((val & 1) << 8);
        s->seqbyte = 0;
        break;
    case REG_HCNTRL:
        if (val & HOST_CHIPRST) {
            aic7880_chip_reset(s);
        } else {
            bool was_paused = s->regs[REG_HCNTRL] & HOST_PAUSE;

            s->regs[REG_HCNTRL] = val & (HOST_INTEN | HOST_PAUSE |
                                         HOST_SWINT | HOST_POWRDN);
            if (val & HOST_PAUSE) {
                aic7880_seq_stop(s, "pause");
            }
            aic7880_update_irq(s);
            if (was_paused && !(val & HOST_PAUSE)) {
                aic7880_run(s);
            }
        }
        break;
    case REG_CLRINT:
        if (s->external_scb_ram && (val & 0x10)) {
            s->error &= ~0x20;
            if (s->parity_break_pending && !(s->error & 4) &&
                !aic7880_pci_status(s) && !s->maskable_break_pending) {
                s->regs[REG_INTSTAT] &= ~8;
            }
            s->parity_break_pending = false;
        }
        s->regs[REG_INTSTAT] &= ~(val & 0x0f);
        if (s->parity_break_pending) {
            s->regs[REG_INTSTAT] |= 8;
        }
        if ((val & 0x04) &&
            !(s->regs[REG_SSTAT0] & s->regs[REG_SIMODE0] & 0x20) &&
            !(s->regs[REG_SSTAT1] & s->regs[REG_SIMODE1] & 0x04)) {
            s->peer_scsi_irq_latched = false;
        }
        if (val & 0x08) {
            s->maskable_break_pending = false;
        }
        aic7880_update_scsi_irq(s);
        break;
    default:
        if (s->running) {
            aic7880_error(s, "unsupported sequencer destination");
        } else {
            qemu_log_mask(LOG_UNIMP, "aic7880: write to unsupported register "
                          "0x%02" HWADDR_PRIx "\n", addr);
            aic7880_stop_diagnostic(s);
        }
        break;
    }
}

static uint8_t aic7880_seq_read(AIC7880State *s, uint8_t reg)
{
    uint16_t at = s->seqaddr - 1;
    uint32_t insn = ldl_le_p(&s->seqram[at * 4]);
    uint8_t value, scbptr, scbcnt, fifo_read;
    uint16_t fifo_len;

    if (reg == REG_DINDIR) {
        aic7880_error(s, "sequencer read of write-only DINDIR");
        return 0xff;
    }
    if (reg == REG_SINDIR) {
        uint8_t indexed = s->regs[REG_SINDEX]++;

        trace_aic7880_seq_indirect(PCI_DEVICE(s)->devfn, at, insn,
            indexed, s->regs[REG_DINDEX], s->regs[REG_SCBPTR],
            s->regs[REG_SCBCNT]);
        if (indexed == REG_SINDIR) {
            aic7880_error(s, "self-referential SINDIR source");
            return 0xff;
        }
        return aic7880_seq_read(s, indexed);
    }
    switch (reg) {
    case REG_DSPCISTATUS:
    case REG_HCNTRL:
    case REG_INTSTAT:
    case REG_ERROR:
    case REG_QOUTFIFO:
    case REG_QOUTCNT:
        trace_aic7880_seq_host_only(PCI_DEVICE(s)->devfn, at, insn,
            (insn >> 8) & 0xff, (insn >> 16) & 0xff, reg,
            s->regs[REG_SINDEX], s->regs[REG_DINDEX],
            s->regs[REG_SCBPTR], s->regs[REG_SCBCNT]);
        if (reg == REG_INTSTAT) {
            qemu_log_mask(LOG_UNIMP,
                          "aic7880: unsupported sequencer read at pc 0x%03x "
                          "of host-only register 0x%02x\n", at, reg);
            aic7880_stop_diagnostic(s);
        } else {
            aic7880_error(s, "sequencer read of host-only register");
        }
        return 0xff;
    default:
        break;
    }
    if (reg == REG_QINFIFO) {
        uint8_t id;
        unsigned int count_before = s->qin_count;

        if (!s->qin_count) {
            trace_aic7880_qinfifo(PCI_DEVICE(s)->devfn, false, 0xff,
                                  count_before, count_before,
                                  s->regs[REG_HCNTRL] & HOST_PAUSE,
                                  s->running, false);
            if (!s->external_scb_ram) {
                aic7880_error(s, "sequencer read of empty queue-in FIFO");
            }
            return 0xff;
        }
        id = s->qin[s->qin_head];
        s->qin_head = (s->qin_head + 1) % aic7880_queue_slots(s);
        s->qin_count--;
        trace_aic7880_qinfifo(PCI_DEVICE(s)->devfn, false, id,
                              count_before, s->qin_count,
                              s->regs[REG_HCNTRL] & HOST_PAUSE,
                              s->running, true);
        return id;
    }
    if (reg == REG_ALLZEROS) {
        return 0;
    }
    if (reg == REG_ALLONES) {
        return 0xff;
    }
    scbptr = s->regs[REG_SCBPTR];
    scbcnt = s->regs[REG_SCBCNT];
    fifo_read = s->host_fifo_read;
    fifo_len = s->host_fifo_len;
    value = aic7880_read(s, reg, 1);

    if ((reg >= 0x20 && reg <= 0x5f) || reg == REG_DFDAT ||
        (reg >= REG_SCB_BASE && reg < REG_SCB_BASE + AIC_SCB_PAGE_BYTES)) {
        trace_aic7880_seq_data(PCI_DEVICE(s)->devfn, at, insn, reg, value,
            scbptr, scbcnt, fifo_read, fifo_len);
    }
    return value;
}

typedef enum AICShiftKind {
    AIC_SHIFT_INVALID,
    AIC_SHIFT_SHL,
    AIC_SHIFT_SHR,
    AIC_SHIFT_ROL,
    AIC_SHIFT_ROR,
} AICShiftKind;

static AICShiftKind aic7880_decode_shift(uint8_t control, unsigned *count)
{
    unsigned high = control >> 4;
    unsigned low = control & 0x0f;

    switch (control) {
    case 0xf0:
        *count = 8;
        return AIC_SHIFT_SHL;
    case 0xf8:
        *count = 8;
        return AIC_SHIFT_SHR;
    case 0x00:
        *count = 8;
        return AIC_SHIFT_ROL;
    case 0x08:
        *count = 8;
        return AIC_SHIFT_ROR;
    default:
        break;
    }
    if (high == 0 && low >= 1 && low <= 7) {
        *count = low;
        return AIC_SHIFT_ROL;
    }
    if (high == 0 && low >= 9) {
        *count = 16 - low;
        return AIC_SHIFT_ROR;
    }
    if (high >= 1 && high <= 7 && low == high) {
        *count = high;
        return AIC_SHIFT_SHL;
    }
    if (high >= 1 && high <= 7 && low == 16 - high) {
        *count = high;
        return AIC_SHIFT_SHR;
    }
    return AIC_SHIFT_INVALID;
}

static uint8_t aic7880_shift_byte(AICShiftKind kind, unsigned count,
                                  uint8_t source)
{
    /* The selected old-format controls have no modeled flag/carry effects. */
    switch (kind) {
    case AIC_SHIFT_SHL:
        return source << count;
    case AIC_SHIFT_SHR:
        return source >> count;
    case AIC_SHIFT_ROL:
        return (source << count) | (source >> (8 - count));
    case AIC_SHIFT_ROR:
        return (source >> count) | (source << (8 - count));
    default:
        g_assert_not_reached();
    }
}

static bool aic7880_dindir_target_supported(uint8_t reg)
{
    if ((reg >= REG_STCNT && reg < REG_STCNT + 3) ||
        (reg >= REG_SHADDR && reg < REG_SHADDR + 4) ||
        (reg >= 0x20 && reg <= 0x5f) ||
        (reg >= 0x88 && reg <= 0x8e)) {
        return true;
    }
    if (reg >= REG_SCB_BASE && reg < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        return true;
    }
    /* Pointer aliases and unselected write paths remain unsupported. */
    switch (reg) {
    case REG_SCSISEQ:
    case REG_SXFRCTL0:
    case REG_SXFRCTL1:
    case REG_SCSISIGI:
    case REG_SCSIID:
    case REG_SCSIRATE:
    case REG_SCSIDATL:
    case REG_SSTAT0:
    case REG_SSTAT1:
    case REG_SIMODE0:
    case REG_SIMODE1:
    case REG_SBLKCTL:
    case REG_SEQCTL:
    case REG_SEQADDR0:
    case REG_SEQADDR1:
    case REG_ACCUM:
    case REG_BRKADDR0:
    case REG_BRKADDR1:
    case REG_ALLZEROS:
    case REG_FUNCTION1:
    case REG_SCBPTR:
    case REG_SCBCNT:
    case REG_DFCNTRL:
    case REG_INTSTAT:
    case REG_QOUTFIFO:
        return true;
    default:
        return false;
    }
}

static bool aic7880_dindir_equal_target_supported(uint8_t reg)
{
    /* Only scratch and the fixed low HADDR bytes have selected alias order. */
    return (reg >= 0x20 && reg <= 0x5f) ||
           (reg >= 0x88 && reg <= 0x8b);
}

static void aic7880_run(AIC7880State *s)
{
    if (s->parity_break_pending) {
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_seq_stop(s, "parity");
        return;
    }
    if (s->error_pending) {
        aic7880_stop_diagnostic(s);
        return;
    }
    if (!aic7880_seq_runnable(s) || s->running || timer_pending(s->seq_timer)) {
        return;
    }
    s->seq_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         aic7880_seq_period_ns(s);
    timer_mod(s->seq_timer, s->seq_deadline_ns);
    trace_aic7880_seq_clock(PCI_DEVICE(s)->devfn, "start",
        s->seq_deadline_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        aic7880_seq_period_ns(s), s->regs[REG_HCNTRL],
        s->regs[REG_SEQCTL], s->regs[REG_INTSTAT]);
}

static void aic7880_seq_timer_cb(void *opaque)
{
    AIC7880State *s = opaque;
    uint32_t insn;
    uint16_t at = s->seqaddr;
    uint8_t op, imm, result;
    uint8_t updated_flags = s->regs[REG_FLAGS];
    uint8_t src, dst;
    uint32_t error_before;
    AICShiftKind shift_kind = AIC_SHIFT_INVALID;
    unsigned shift_count = 0;
    bool indirect_dest = false;
    bool scb_rmw;
    uint8_t scb_counter;
    uint8_t source;

    if (!aic7880_seq_runnable(s)) {
        return;
    }
    s->running = true;

    if (at >= AIC_SEQ_INSNS) {
        aic7880_error(s, "sequencer ran past instruction RAM");
        goto done;
    }
    if (!(s->regs[REG_BRKADDR1] & 0x80) &&
        at == (s->regs[REG_BRKADDR0] |
               ((s->regs[REG_BRKADDR1] & 1) << 8))) {
        s->maskable_break_pending = true;
        s->regs[REG_INTSTAT] |= 0x08;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_update_irq(s);
        goto done;
    }
    insn = ldl_le_p(&s->seqram[at * 4]);
    s->seqaddr = at + 1;
    op = (insn >> 25) & 0x0f;
    imm = insn;
    src = (insn >> 8) & 0xff;
    dst = (insn >> 16) & 0xff;
    if (op >= 0x08) {
        uint16_t target = (insn >> 16) & 0x1ff;
        uint8_t mask = imm ? imm : s->regs[REG_ACCUM];
        uint8_t value = 0;
        bool taken;

        value = aic7880_seq_read(s, src);
        if (s->regs[REG_HCNTRL] & HOST_PAUSE) {
            goto done;
        }
        switch (op) {
        case 0x08: /* JMP */
        case 0x09: /* JC */
        case 0x0a: /* JNC */
        case 0x0b: /* CALL */
            value |= imm;
            aic7880_write(s, REG_SINDEX, value, 1);
            /* Selected transfer flag policy; fitted effects are unknown. */
            s->regs[REG_FLAGS] = (updated_flags & FLAGS_CARRY) |
                                 (value == 0 ? FLAGS_ZERO : 0);
            if (op == 0x09) {
                taken = updated_flags & FLAGS_CARRY;
            } else {
                taken = op != 0x0a || !(updated_flags & FLAGS_CARRY);
            }
            break;
        case 0x0c: /* JNE */
            taken = value != mask;
            break;
        case 0x0d: /* JNZ */
            taken = (value & mask) != 0;
            break;
        case 0x0e: /* JE */
            taken = value == mask;
            break;
        case 0x0f: /* JZ */
            taken = (value & mask) == 0;
            break;
        default:
            aic7880_bad_opcode(s, op);
            goto done;
        }
        if (taken) {
            if (op == 0x0b) {
                s->return_ptr = (s->return_ptr +
                                 ARRAY_SIZE(s->return_pc) - 1) %
                                ARRAY_SIZE(s->return_pc);
                s->return_pc[s->return_ptr] = s->seqaddr &
                                              (AIC_SEQ_INSNS - 1);
            }
            s->seqaddr = target;
        }
        if (s->regs[REG_SEQCTL] & SEQ_STEP) {
            s->regs[REG_HCNTRL] |= HOST_PAUSE;
            goto done;
        }
        goto done;
    }
    if (op > 5) {
        aic7880_bad_opcode(s, op);
        goto done;
    }
    if (dst == REG_HCNTRL) {
        aic7880_error(s, "sequencer write to host-only HCNTRL");
        goto done;
    }
    if (dst == REG_CLRINT) {
        aic7880_error(s, "sequencer write to host-only CLRINT");
        goto done;
    }
    if (dst == REG_FLAGS) {
        aic7880_error(s, "sequencer write to read-only FLAGS");
        goto done;
    }
    if (op == 5) {
        shift_kind = aic7880_decode_shift(imm, &shift_count);
        if (shift_kind == AIC_SHIFT_INVALID) {
            aic7880_error(s, "unsupported sequencer shift control");
            goto done;
        }
    }
    if (dst == REG_DINDIR) {
        uint8_t target = s->regs[REG_DINDEX];

        /* Other source and destination aliases have no reviewed order. */
        if (!aic7880_dindir_target_supported(target) ||
            target == src ||
            (src == REG_SINDIR &&
             target == s->regs[REG_SINDEX] &&
             !aic7880_dindir_equal_target_supported(target))) {
            aic7880_error(s, "unsupported DINDIR destination");
            goto done;
        }
        dst = target;
        indirect_dest = true;
    }
    source = src == REG_SINDIR ? s->regs[REG_SINDEX] : src;
    scb_counter = s->regs[REG_SCBCNT];
    scb_rmw = s->external_scb_ram && (scb_counter & 0x80) &&
              source >= REG_SCB_BASE &&
              source < REG_SCB_BASE + AIC_SCB_PAGE_BYTES &&
              dst >= REG_SCB_BASE &&
              dst < REG_SCB_BASE + AIC_SCB_PAGE_BYTES;
    if (scb_rmw && aic7880_scb_parity_bad(s,
                      aic7880_scb_index(s, scb_counter & 0x1f))) {
        /* Fault-time RMW ordering is unqualified; start neither half. */
        aic7880_error(s, "parity-faulting automatic SCB read-modify-write "
                        "is unsupported");
        goto done;
    }
    result = aic7880_seq_read(s, src);
    if (s->regs[REG_HCNTRL] & HOST_PAUSE) {
        goto done;
    }
    if (op == 0) {
        result |= imm ? imm : s->regs[REG_ACCUM];
    } else if (op == 1) {
        result &= imm ? imm : s->regs[REG_ACCUM];
    } else if (op >= 2 && op <= 4) {
        uint8_t operand = imm ? imm : s->regs[REG_ACCUM];

        /* Synthetic flag policy; fitted AIC-7880 effects are unknown. */
        if (op == 2) {
            result ^= operand;
            updated_flags = (updated_flags & FLAGS_CARRY) |
                            (result == 0 ? FLAGS_ZERO : 0);
        } else {
            unsigned sum = (unsigned)result + operand +
                           (op == 4 && (updated_flags & FLAGS_CARRY));

            result = sum;
            updated_flags = (sum > 0xff ? FLAGS_CARRY : 0) |
                            (result == 0 ? FLAGS_ZERO : 0);
        }
    } else {
        result = aic7880_shift_byte(shift_kind, shift_count, result);
    }
    error_before = s->error_serial;
    if (dst == REG_INTSTAT) {
        if (result & 1) {
            s->regs[REG_INTSTAT] = (s->regs[REG_INTSTAT] & 0x0f) |
                                   (result & 0xf0);
        }
        s->regs[REG_INTSTAT] |= result & 0x0f;
        if (result & 0x0d) {
            s->regs[REG_HCNTRL] |= HOST_PAUSE;
        }
        aic7880_update_irq(s);
    } else if (dst == REG_QOUTFIFO) {
        if (!s->external_scb_ram && result >= AIC_SCB_PAGES) {
            aic7880_error(s, "unsupported completion identifier");
            goto done;
        }
        if (s->qout_count == aic7880_queue_limit(s)) {
            if (!s->external_scb_ram) {
                aic7880_error(s, "completion queue full");
                goto done;
            }
        } else {
            s->qout[s->qout_tail] = result;
            s->qout_tail = (s->qout_tail + 1) % aic7880_queue_slots(s);
            s->qout_count++;
        }
    } else {
        if (scb_rmw) {
            /* One logical access reads and writes the selected byte. */
            s->regs[REG_SCBCNT] = scb_counter;
        }
        aic7880_write(s, dst, result, 1);
    }
    if (indirect_dest && s->error_serial == error_before) {
        s->regs[REG_DINDEX]++;
    }
    if (op >= 2 && op <= 4 && s->error_serial == error_before) {
        s->regs[REG_FLAGS] = updated_flags;
    }
    if ((insn & (1u << 24)) && s->error_serial == error_before) {
        s->seqaddr = s->return_pc[s->return_ptr];
        s->return_ptr = (s->return_ptr + 1) % ARRAY_SIZE(s->return_pc);
    }
    if (s->regs[REG_SEQCTL] & SEQ_STEP) {
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        goto done;
    }

done:
    if (aic7880_seq_runnable(s)) {
        /* Keep device time independent of timer dispatch latency. */
        s->seq_deadline_ns += aic7880_seq_period_ns(s);
        timer_mod(s->seq_timer, s->seq_deadline_ns);
    } else {
        aic7880_seq_stop(s, "instruction-pause");
    }
    s->running = false;
}

static MemTxResult aic7880_dspc_read(void *opaque, hwaddr addr,
                                    uint64_t *value, unsigned size,
                                    MemTxAttrs attrs)
{
    if (size != 1) {
        aic7880_error(opaque, "unsupported DSPCISTATUS width");
        *value = 0xffff;
        return MEMTX_ERROR;
    }
    *value = aic7880_read(opaque, REG_DSPCISTATUS + addr, size);
    return MEMTX_OK;
}

static MemTxResult aic7880_dspc_write(void *opaque, hwaddr addr, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    if (size != 1) {
        aic7880_error(opaque, "unsupported DSPCISTATUS width");
        return MEMTX_ERROR;
    }
    aic7880_write(opaque, REG_DSPCISTATUS + addr, value, size);
    return MEMTX_OK;
}

static const MemoryRegionOps aic7880_dspc_ops = {
    .read_with_attrs = aic7880_dspc_read,
    .write_with_attrs = aic7880_dspc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static MemTxResult aic7880_external_read(void *opaque, hwaddr addr,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    if (size != 1) {
        aic7880_error(opaque, "unsupported register read width");
        *value = UINT64_MAX;
        return MEMTX_ERROR;
    }
    *value = aic7880_read(opaque, addr, 1);
    return MEMTX_OK;
}

static MemTxResult aic7880_external_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    AIC7880State *s = opaque;
    unsigned reg = addr & (AIC_REG_SPACE - 1);

    if (size == 1) {
        aic7880_write(s, addr, value, 1);
        return MEMTX_OK;
    }
    if (size != 4 || (addr & 3) || reg < REG_SCB_BASE ||
        reg > REG_SCB_BASE + AIC_SCB_PAGE_BYTES - 4 ||
        !(s->regs[REG_SCBCNT] & 0x80)) {
        aic7880_error(s, "unsupported register write width");
        return MEMTX_ERROR;
    }
    for (unsigned lane = 0; lane < 4; lane++) {
        aic7880_write(s, addr, value >> (lane * 8), 1);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps aic7880_external_ops = {
    .read_with_attrs = aic7880_external_read,
    .write_with_attrs = aic7880_external_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static const MemoryRegionOps aic7880_regs_ops = {
    .read = aic7880_read,
    .write = aic7880_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static int aic7880_pre_save(void *opaque)
{
    AIC7880State *s = opaque;

    return ((s->signal_outputs & 1) || s->req || s->running ||
            s->scsi_data_len || s->command_fifo_stale ||
            s->phase == AIC_PHASE_DATA ||
            (s->selected && s->host_fifo_len) || s->host_dma_active ||
            ((s->regs[REG_DFCNTRL] & 8) && aic7880_count(s, 0x8c)) ||
            s->peer_reselection || s->peer_data_in_pending ||
            s->peer_faulted_command || s->peer_hold_data_in ||
            s->peer_scsi_irq_latched || s->initiator_atn ||
            (s->regs[REG_SSTAT1] & 0x04) ||
            s->phase == AIC_PHASE_MSG_OUT) ? -EBUSY : 0;
}

static void aic7880_peer_event_set(Object *obj, const char *value,
                                   Error **errp)
{
    AIC7880State *s = AIC7880(obj);
    g_auto(GStrv) parts = g_strsplit(value, ":", -1);
    unsigned long target_id, initiator_id, position, byte;

    if (g_strv_length(parts) == 3 &&
        (g_str_equal(parts[0], "save") ||
         g_str_equal(parts[0], "restore") ||
         g_str_equal(parts[0], "disconnect"))) {
        uint8_t message = g_str_equal(parts[0], "save") ? 2 :
                          g_str_equal(parts[0], "restore") ? 3 : 4;

        if (qemu_strtoul(parts[1], NULL, 10, &target_id) ||
            qemu_strtoul(parts[2], NULL, 10, &position) ||
            target_id != s->target || position != s->lun || !s->req ||
            !s->selected || s->phase != AIC_PHASE_DATA ||
            !s->scsi_data_len || s->peer_nexus_disconnected ||
            s->peer_nexus_message ||
            (message == 4 && !s->disconnect_allowed) ||
            (message == 3 && (!s->peer_nexus_saved ||
                              s->peer_nexus_saved_pos > s->scsi_data_len))) {
            error_setg(errp, "invalid synthetic nexus message");
            return;
        }
        s->peer_nexus_signal = s->regs[REG_SCSISIGI];
        aic7880_nexus_message(s, message);
        return;
    }
    if (g_strv_length(parts) == 4 && g_str_equal(parts[0], "reselect")) {
        if (qemu_strtoul(parts[1], NULL, 10, &target_id) ||
            qemu_strtoul(parts[2], NULL, 10, &initiator_id) ||
            qemu_strtoul(parts[3], NULL, 10, &position) ||
            target_id != s->target || position != s->lun ||
            target_id > 7 || initiator_id > 7 ||
            initiator_id == target_id || initiator_id != s->initiator ||
            !s->req || !s->peer_nexus_disconnected || s->selected ||
            s->phase != AIC_PHASE_BUS_FREE || s->peer_nexus_message ||
            !(s->regs[REG_SCSISEQ] & 0x10)) {
            error_setg(errp, "invalid synthetic nexus reselection");
            return;
        }
        timer_del(s->bus_free_timer);
        s->selected = true;
        s->regs[REG_SSTAT0] = (s->regs[REG_SSTAT0] & ~0xa0) | 0x20;
        aic7880_nexus_message(s, 0x80 | s->lun);
        return;
    }

    if (g_strv_length(parts) == 3 && g_str_equal(parts[0], "reselect")) {
        if (qemu_strtoul(parts[1], NULL, 10, &target_id) ||
            qemu_strtoul(parts[2], NULL, 10, &initiator_id) ||
            target_id > 7 || initiator_id > 7 ||
            target_id == initiator_id ||
            target_id != ((s->regs[REG_SCSIID] >> 4) & 15) ||
            initiator_id != (s->regs[REG_SCSIID] & 15) ||
            !scsi_device_find(&s->bus, 0, target_id, 0) ||
            !(s->regs[REG_SCSISEQ] & 0x10) ||
            s->phase != AIC_PHASE_BUS_FREE || s->selected || s->req ||
            s->peer_reselection) {
            error_setg(errp, "invalid synthetic SCSI reselection");
            return;
        }
        timer_del(s->bus_free_timer);
        s->peer_reselection = true;
        s->initiator_atn = false;
        s->regs[REG_SCSISIGI] = 0;
        s->regs[REG_SSTAT0] = (s->regs[REG_SSTAT0] & ~0xe0) | 0x20;
        s->regs[REG_SSTAT1] &= ~0x08;
        if (s->regs[REG_SIMODE0] & 0x20) {
            s->peer_scsi_irq_latched = true;
        }
        aic7880_update_scsi_irq(s);
        return;
    }
    if (g_str_equal(value, "release") && s->peer_reselection) {
        s->expected_phase = 0;
        s->peer_reselection = false;
        s->peer_nexus_disconnected = false;
        s->peer_nexus_saved = false;
        s->peer_nexus_message = 0;
        s->initiator_atn = false;
        s->regs[REG_SSTAT0] &= ~0x20;
        s->regs[REG_SCSISIGI] = 0;
        aic7880_bus_free_qualify(s);
        aic7880_update_scsi_irq(s);
        return;
    }
    if (g_str_equal(value, "hold-data-in") &&
        s->phase == AIC_PHASE_BUS_FREE && !s->selected && !s->req &&
        !s->peer_reselection && !s->peer_hold_data_in) {
        s->peer_hold_data_in = true;
        return;
    }
    if (g_strv_length(parts) == 4 &&
        (g_str_equal(parts[0], "bad") || g_str_equal(parts[0], "good")) &&
        !qemu_strtoul(parts[2], NULL, 10, &position) &&
        !qemu_strtoul(parts[3], NULL, 16, &byte) && byte <= UINT8_MAX &&
        s->selected && !s->peer_reselection) {
        uint8_t expected;
        bool data_phase = false;

        if (g_str_equal(parts[1], "data-in") &&
            s->phase == AIC_PHASE_DATA && s->peer_data_in_pending &&
            s->req && position == s->peer_data_pos &&
            position < s->peer_data_len) {
            expected = scsi_req_get_buf(s->req)[position];
            data_phase = true;
        } else if (position == 0 &&
                   ((g_str_equal(parts[1], "status") &&
                     s->phase == AIC_PHASE_STATUS) ||
                    (g_str_equal(parts[1], "msg-in") &&
                     s->phase == AIC_PHASE_MSG_IN))) {
            expected = s->regs[REG_SCSIDATL];
        } else {
            error_setg(errp, "synthetic SCSI sample has no pending byte");
            return;
        }
        if (byte != expected) {
            error_setg(errp, "synthetic SCSI sample byte differs from target");
            return;
        }
        if (data_phase) {
            s->peer_data_pos++;
        }
        s->peer_last_parity_bad = g_str_equal(parts[0], "bad") &&
                                  (s->regs[REG_SXFRCTL1] & 0x20);
        if (s->peer_last_parity_bad) {
            s->peer_faulted_command = true;
            s->regs[REG_SSTAT1] |= 0x04;
            if (s->regs[REG_SCSISEQ] & 0x02) {
                s->initiator_atn = true;
            }
            if (s->regs[REG_SIMODE1] & 0x04) {
                s->peer_scsi_irq_latched = true;
            }
        }
        aic7880_update_scsi_irq(s);
        return;
    }
    if (g_str_equal(value, "release") && s->peer_data_in_pending) {
        scsi_req_cancel(s->req);
        s->peer_data_in_pending = false;
        s->peer_data_len = s->peer_data_pos = 0;
        s->peer_faulted_command = false;
        s->selected = false;
        aic7880_release_identity(s);
        s->lun = 0;
        s->disconnect_allowed = false;
        s->initiator_atn = false;
        s->phase = AIC_PHASE_BUS_FREE;
        s->signal_outputs = 0;
        aic7880_manual_clear(s);
        s->expected_phase = 0;
        s->regs[REG_SCSISIGI] = 0;
        s->regs[REG_SSTAT0] &= ~0x62;
        aic7880_bus_free_qualify(s);
        aic7880_update_scsi_irq(s);
        return;
    }
    error_setg(errp, "unsupported synthetic SCSI peer event");
}

static int aic7880_post_load(void *opaque, int version_id)
{
    AIC7880State *s = opaque;

    if (s->seq_deadline_ns < 0 ||
        timer_pending(s->seq_timer) != aic7880_seq_runnable(s) ||
        (timer_pending(s->seq_timer) &&
         timer_expire_time_ns(s->seq_timer) != (uint64_t)s->seq_deadline_ns)) {
        return -EINVAL;
    }
    if (s->message_out_len > sizeof(s->message_out) ||
        (s->message_resume_command &&
         (s->phase != AIC_PHASE_MSG_IN || !aic7880_message_complete(s)))) {
        return -EINVAL;
    }
    if ((s->cdb_len && (!s->cdb_need || s->cdb_len > s->cdb_need ||
                        scsi_cdb_length(s->cdb) != s->cdb_need)) ||
        (!s->cdb_len && s->cdb_need) ||
        (s->command_dma_wait &&
         (!s->selected || s->phase != AIC_PHASE_COMMAND || !s->cdb_len ||
          s->cdb_len == s->cdb_need)) ||
        s->command_fifo_stale) {
        return -EINVAL;
    }
    if (s->seqaddr > AIC_SEQ_INSNS ||
        s->seqbyte >= 4 ||
        s->phase >= AIC_PHASE_MSG_OUT || s->lun > 7 || s->target > 15 ||
        s->initiator > 15 || (s->regs[REG_SBLKCTL] & ~0xe2) ||
        s->cdb_len > sizeof(s->cdb) ||
        s->cdb_need > sizeof(s->cdb) ||
        s->qout_count > aic7880_queue_limit(s) ||
        s->qout_head >= aic7880_queue_slots(s) ||
        s->qout_tail >= aic7880_queue_slots(s)) {
        return -EINVAL;
    }
    if ((!s->identity_valid &&
         (s->selected || s->selection_pending || s->target || s->initiator ||
          s->selection_wide)) ||
        (s->identity_valid &&
         (s->target == s->initiator ||
          (!s->selection_wide && (s->initiator > 7 ||
                                 (s->selected && s->target > 7))) ||
          s->selection_wide != !!(s->regs[REG_SBLKCTL] & SBLK_SELWIDE) ||
          (!s->selected && !s->selection_pending &&
           !timer_pending(s->bus_free_timer))))) {
        return -EINVAL;
    }
    if (s->selection_pending &&
        (s->selected || s->req || !(s->regs[REG_SCSISEQ] & 0x40))) {
        return -EINVAL;
    }
    if (s->qin_count > aic7880_queue_limit(s) ||
        s->qin_head >= aic7880_queue_slots(s) ||
        s->qin_tail >= aic7880_queue_slots(s)) {
        return -EINVAL;
    }
    if (s->return_ptr >= ARRAY_SIZE(s->return_pc) || s->stack_byte > 1) {
        return -EINVAL;
    }
    if (s->host_fifo_len > sizeof(s->host_fifo) ||
        (uint8_t)(s->host_fifo_read + s->host_fifo_len) !=
        s->host_fifo_write) {
        return -EINVAL;
    }
    if (s->host_request_pending &&
        (!(s->regs[REG_DFCNTRL] & 8) || !aic7880_count(s, 0x8c) ||
         s->error_pending ||
         ((s->regs[REG_DFCNTRL] & 4) ?
          s->host_fifo_len == sizeof(s->host_fifo) : !s->host_fifo_len))) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->return_pc); i++) {
        if (s->return_pc[i] >= AIC_SEQ_INSNS) {
            return -EINVAL;
        }
    }
    s->req = NULL;
    s->scsi_data_len = s->scsi_data_pos = 0;
    s->running = false;
    s->error_serial = 0;
    aic7880_fifo_status(s);
    aic7880_update_scsi_irq(s);
    return 0;
}

static const VMStateDescription vmstate_aic7880 = {
    .name = "aic7880",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = aic7880_pre_save,
    .post_load = aic7880_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AIC7880State),
        VMSTATE_UINT8_ARRAY(regs, AIC7880State, AIC_REG_SPACE),
        VMSTATE_UINT8_ARRAY(seqram, AIC7880State, AIC_SEQ_BYTES),
        VMSTATE_UINT32_EQUAL(external_scb_size, AIC7880State),
        VMSTATE_UINT8_EQUAL(external_scb_width, AIC7880State),
        VMSTATE_UINT8_ARRAY(scb_ram, AIC7880State,
                            AIC_SCB_EXTERNAL_PAGES * AIC_SCB_PAGE_BYTES),
        VMSTATE_UINT8_ARRAY(scb_parity, AIC7880State,
                            AIC_SCB_EXTERNAL_PAGES * AIC_SCB_PAGE_BYTES),
        VMSTATE_BOOL(parity_break_pending, AIC7880State),
        VMSTATE_UINT16(seqaddr, AIC7880State),
        VMSTATE_TIMER_PTR(seq_timer, AIC7880State),
        VMSTATE_INT64(seq_deadline_ns, AIC7880State),
        VMSTATE_TIMER_PTR(selection_timer, AIC7880State),
        VMSTATE_TIMER_PTR(bus_free_timer, AIC7880State),
        VMSTATE_BOOL(selection_pending, AIC7880State),
        VMSTATE_UINT8(seqbyte, AIC7880State),
        VMSTATE_UINT8(error, AIC7880State),
        VMSTATE_UINT8_ARRAY(cdb, AIC7880State, 16),
        VMSTATE_UINT8(cdb_len, AIC7880State),
        VMSTATE_UINT8(cdb_need, AIC7880State),
        VMSTATE_BOOL(command_dma_wait, AIC7880State),
        VMSTATE_BOOL(command_fifo_stale, AIC7880State),
        VMSTATE_UINT8(target, AIC7880State),
        VMSTATE_UINT8(initiator, AIC7880State),
        VMSTATE_BOOL(selection_wide, AIC7880State),
        VMSTATE_BOOL(identity_valid, AIC7880State),
        VMSTATE_UINT8(lun, AIC7880State),
        VMSTATE_BOOL(disconnect_allowed, AIC7880State),
        VMSTATE_BOOL(identify_seen, AIC7880State),
        VMSTATE_UINT8_ARRAY(message_out, AIC7880State, 5),
        VMSTATE_UINT8(message_out_len, AIC7880State),
        VMSTATE_BOOL(message_resume_command, AIC7880State),
        VMSTATE_UINT8(phase, AIC7880State),
        VMSTATE_UINT8(expected_phase, AIC7880State),
        VMSTATE_UINT8(signal_outputs, AIC7880State),
        VMSTATE_UINT8(manual_data, AIC7880State),
        VMSTATE_BOOL(manual_valid, AIC7880State),
        VMSTATE_UINT8_ARRAY(qout, AIC7880State, 256),
        VMSTATE_UINT8(qout_head, AIC7880State),
        VMSTATE_UINT8(qout_tail, AIC7880State),
        VMSTATE_UINT8(qout_count, AIC7880State),
        VMSTATE_UINT8_ARRAY(qin, AIC7880State, 256),
        VMSTATE_UINT8(qin_head, AIC7880State),
        VMSTATE_UINT8(qin_tail, AIC7880State),
        VMSTATE_UINT8(qin_count, AIC7880State),
        VMSTATE_UINT16_ARRAY(return_pc, AIC7880State, 4),
        VMSTATE_UINT8(return_ptr, AIC7880State),
        VMSTATE_UINT8(stack_byte, AIC7880State),
        VMSTATE_UINT8_ARRAY(host_fifo, AIC7880State, 256),
        VMSTATE_UINT8(host_fifo_read, AIC7880State),
        VMSTATE_UINT8(host_fifo_write, AIC7880State),
        VMSTATE_UINT16(host_fifo_len, AIC7880State),
        VMSTATE_BOOL(host_request_pending, AIC7880State),
        VMSTATE_BOOL(selected, AIC7880State),
        VMSTATE_BOOL(maskable_break_pending, AIC7880State),
        VMSTATE_BOOL(error_pending, AIC7880State),
        VMSTATE_END_OF_LIST()
    }
};

static const SCSIBusInfo aic7880_scsi_info = {
    .tcq = false,
    .max_target = 15,
    .max_lun = 7,
    .transfer_data = aic7880_transfer_data,
    .complete = aic7880_complete,
    .cancel = aic7880_cancel,
};

static void aic7880_test_pci_error_set(Object *obj, const char *value,
                                       Error **errp)
{
    PCIDevice *pci = PCI_DEVICE(obj);
    AIC7880State *s = AIC7880(obj);

    if (!g_str_equal(value, "received-master-abort")) {
        error_setg(errp, "unsupported synthetic PCI error");
        return;
    }
    pci_word_test_and_set_mask(pci->config + PCI_STATUS,
                              PCI_STATUS_REC_MASTER_ABORT);
    if (!(s->regs[REG_SEQCTL] & SEQ_FAILDIS)) {
        s->regs[REG_INTSTAT] |= 0x08;
        s->maskable_break_pending = true;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_seq_stop(s, "pause");
    }
    aic7880_update_irq(s);
}

static void aic7880_test_scb_parity_set(Object *obj, const char *value,
                                        Error **errp)
{
    AIC7880State *s = AIC7880(obj);
    unsigned page, offset;
    char extra;
    unsigned pages = s->external_scb_size / AIC_SCB_PAGE_BYTES;

    if (sscanf(value, "invert:%u:%u%c", &page, &offset, &extra) != 2 ||
        page > 255 || offset >= AIC_SCB_PAGE_BYTES) {
        error_setg(errp, "expected invert:page:offset within SCB space");
        return;
    }
    s->scb_parity[(page % pages) * AIC_SCB_PAGE_BYTES + offset] ^= 1;
}

static char *aic7880_test_selection_get(Object *obj, Error **errp)
{
    AIC7880State *s = AIC7880(obj);

    if (!s->identity_valid) {
        return g_strdup("none");
    }
    return g_strdup_printf("%u:%u:%u:%u", s->target, s->initiator, s->lun,
                            s->selection_wide);
}

static void aic7880_realize(PCIDevice *pci, Error **errp)
{
    AIC7880State *s = AIC7880(pci);
    unsigned dspc_windows;

    if (s->external_scb_ram) {
        if (s->external_scb_size != KiB && s->external_scb_size != 2 * KiB &&
            s->external_scb_size != 4 * KiB &&
            s->external_scb_size != 8 * KiB) {
            error_setg(errp, "external SCB SRAM requires 1, 2, 4 or 8 KiB");
            return;
        }
        if (s->external_scb_width != 8 && s->external_scb_width != 9) {
            error_setg(errp, "external SCB SRAM requires 8 or 9 data bits");
            return;
        }
        if (s->external_scb_shared) {
            error_setg(errp, "shared external SCB SRAM is unsupported");
            return;
        }
    } else if (s->external_scb_size || s->external_scb_width != 8 ||
               s->external_scb_shared) {
        error_setg(errp, "external SCB parameters require external-scb-ram");
        return;
    }

    if (s->o2_pci_diagnostic && s->o2_storage_diagnostic) {
        error_setg(errp, "AIC PCI and storage diagnostics are exclusive");
        return;
    }
    if (s->o2_pci_diagnostic && !qtest_enabled()) {
        error_setg(errp, "AIC PCI diagnostic requires QTest");
        return;
    }
    if (s->o2_storage_diagnostic && !s->dma_reporter) {
        error_setg(errp, "AIC storage diagnostic requires O2 machine reporter");
        return;
    }
    if (s->test_peer && (!qtest_enabled() || s->o2_pci_diagnostic ||
                         s->o2_storage_diagnostic)) {
        error_setg(errp, "AIC test peer requires isolated QTest");
        return;
    }
    if (s->test_peer) {
        object_property_add_str(OBJECT(s), "x-test-peer-event", NULL,
                                aic7880_peer_event_set);
    }
    if (qtest_enabled() && s->external_scb_ram && s->external_scb_width == 9) {
        object_property_add_str(OBJECT(s), "x-test-scb-parity", NULL,
                                aic7880_test_scb_parity_set);
    }
    if (qtest_enabled()) {
        object_property_add_str(OBJECT(s), "x-test-selection",
                                aic7880_test_selection_get, NULL);
        object_property_add_str(OBJECT(s), "x-test-pci-error", NULL,
                                aic7880_test_pci_error_set);
    }
    s->seq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                aic7880_seq_timer_cb, s);
    s->selection_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     aic7880_selection_timer_cb, s);
    s->bus_free_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    aic7880_bus_free_timer_cb, s);
    pci->config[PCI_INTERRUPT_PIN] = 1;
    if (s->external_scb_ram) {
        memset(pci->wmask + 0x40, 0, 4);
        pci->wmask[0x40] = 0x30;
    }
    pci->wmask[0x41] &= ~0x06;
    /* The selected AIC-7870 sibling policy covers readback only. */
    pci_word_test_and_set_mask(pci->wmask + PCI_COMMAND,
                               PCI_COMMAND_PARITY);
    memory_region_init_io(&s->io, OBJECT(s),
                          s->external_scb_ram ? &aic7880_external_ops :
                                                &aic7880_regs_ops, s,
                          "aic7880-io", AIC_REG_SPACE);
    /* The fitted AIC-7880 BAR1 mask is unknown; use the selected sibling. */
    memory_region_init_io(&s->mem, OBJECT(s),
                          s->external_scb_ram ? &aic7880_external_ops :
                                                &aic7880_regs_ops, s,
                          "aic7880-mmio", 4 * KiB);
    /* Keep a word at 0x86 intact for byte-only validation. */
    memory_region_init_io(&s->dspc_io, OBJECT(s), &aic7880_dspc_ops, s,
                          "aic7880-dspc-io", 2);
    memory_region_add_subregion(&s->io, REG_DSPCISTATUS, &s->dspc_io);
    dspc_windows = ARRAY_SIZE(s->dspc_mem);
    for (unsigned i = 0; i < dspc_windows; i++) {
        g_autofree char *name = g_strdup_printf("aic7880-dspc-mmio-%u", i);

        memory_region_init_io(&s->dspc_mem[i], OBJECT(s), &aic7880_dspc_ops,
                              s, name, 2);
        memory_region_add_subregion(&s->mem, i * AIC_REG_SPACE +
                                    REG_DSPCISTATUS, &s->dspc_mem[i]);
    }
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(pci, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mem);
    scsi_bus_init(&s->bus, sizeof(s->bus), DEVICE(s), &aic7880_scsi_info);
}

static void aic7880_exit(PCIDevice *pci)
{
    AIC7880State *s = AIC7880(pci);

    timer_free(s->seq_timer);
    timer_free(s->selection_timer);
    timer_free(s->bus_free_timer);
}

static const Property aic7880_properties[] = {
    DEFINE_PROP_BOOL("external-scb-ram", AIC7880State, external_scb_ram, false),
    DEFINE_PROP_UINT32("external-scb-size", AIC7880State, external_scb_size, 0),
    DEFINE_PROP_UINT8("external-scb-width", AIC7880State,
                      external_scb_width, 8),
    DEFINE_PROP_BOOL("external-scb-shared", AIC7880State,
                     external_scb_shared, false),
    DEFINE_PROP_BOOL("x-test-peer", AIC7880State, test_peer, false),
    DEFINE_PROP_BOOL("o2-pci-diagnostic", AIC7880State,
                     o2_pci_diagnostic, false),
    DEFINE_PROP_BOOL("o2-storage-diagnostic", AIC7880State,
                     o2_storage_diagnostic, false),
};

static void aic7880_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    pc->realize = aic7880_realize;
    pc->exit = aic7880_exit;
    pc->config_write = aic7880_write_config;
    pc->vendor_id = 0x9004;
    pc->device_id = 0x8078;
    pc->class_id = PCI_CLASS_STORAGE_SCSI;
    rc->phases.hold = aic7880_reset_hold;
    dc->desc = "Adaptec AIC-7880 SCSI host adapter";
    dc->vmsd = &vmstate_aic7880;
    device_class_set_props(dc, aic7880_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo aic7880_info = {
    .name = TYPE_AIC7880,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AIC7880State),
    .class_init = aic7880_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void aic7880_register_types(void)
{
    type_register_static(&aic7880_info);
}

type_init(aic7880_register_types)
