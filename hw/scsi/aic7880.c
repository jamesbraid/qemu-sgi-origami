/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Adaptec AIC-7880 PCI SCSI controller, isolated initiator experiment.
 *
 * The downloaded sequencer controls selection, command delivery, status,
 * and Command Complete. This subset accepts one asynchronous narrow command
 * at a time. Manual host PIO, disconnect/reselection, and target mode
 * are unsupported.
 * Reset values transferred from the AIC-7870 manual are provisional.
 * The O2 qtest policy selects its sibling's BAR1 size and SWINT gating.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
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
/* Model scheduling delay; no AIC-7880 instruction timing is established. */
#define AIC_SEQ_QUANTUM_US 500
#define AIC_SCB_PAGES 16
#define AIC_SCB_PAGE_BYTES 32
#define AIC_PCI_ERROR_MASK (PCI_STATUS_PARITY | PCI_STATUS_SIG_TARGET_ABORT | \
                            PCI_STATUS_REC_TARGET_ABORT | \
                            PCI_STATUS_REC_MASTER_ABORT | \
                            PCI_STATUS_SIG_SYSTEM_ERROR | \
                            PCI_STATUS_DETECTED_PARITY)

#define REG_SCSISEQ  0x00
#define REG_SXFRCTL0 0x01
#define REG_SXFRCTL1 0x02
#define REG_SCSISIGI 0x03
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
#define DFS_FIFOFULL 0x02
#define DFS_FIFOEMP 0x01
#define SBLK_AUTOFLUSHDIS 0x20

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
    uint8_t regs[AIC_REG_SPACE];
    uint8_t seqram[AIC_SEQ_BYTES];
    uint8_t scb_ram[AIC_SCB_PAGES * AIC_SCB_PAGE_BYTES];
    uint16_t seqaddr;
    uint8_t seqbyte;
    uint8_t error;
    uint32_t error_serial;
    uint8_t cdb[16];
    uint8_t cdb_len;
    uint8_t cdb_need;
    uint8_t target;
    uint8_t phase;
    uint8_t expected_phase;
    uint8_t qout[16];
    uint8_t qin[16];
    uint8_t qin_head;
    uint8_t qin_tail;
    uint8_t qin_count;
    uint16_t return_pc[4];
    uint8_t return_depth;
    uint32_t scsi_data_len;
    uint32_t scsi_data_pos;
    uint8_t host_fifo[256];
    uint8_t host_fifo_read;
    uint8_t host_fifo_write;
    uint16_t host_fifo_len;
    bool host_dma_active;
    uint8_t qout_head;
    uint8_t qout_tail;
    uint8_t qout_count;
    bool selected;
    bool initiator_atn;
    bool identify_seen;
    bool peer_reselection;
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

static void aic7880_update_irq(AIC7880State *s)
{
    bool enabled = s->regs[REG_HCNTRL] & HOST_INTEN;
    bool decoded = PCI_DEVICE(s)->config[PCI_COMMAND] &
                   (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    bool swint = (s->regs[REG_HCNTRL] & HOST_SWINT) && decoded;
    bool exception = (s->regs[REG_INTSTAT] & 0x08) &&
                     (!s->maskable_break_pending || s->error_pending ||
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
        timer_del(s->seq_timer);
    }
    aic7880_update_irq(s);
}

static void aic7880_write_config(PCIDevice *pci, uint32_t addr,
                                  uint32_t val, int len)
{
    uint16_t before = pci_get_word(pci->config + PCI_COMMAND);

    pci_default_write_config(pci, addr, val, len);
    if (pci_get_word(pci->config + PCI_COMMAND) != before) {
        trace_aic7880_pci_command(pci->devfn, before,
            pci_get_word(pci->config + PCI_COMMAND),
            ldl_le_p(&AIC7880(pci)->regs[0x88]),
            ldl_le_p(&AIC7880(pci)->regs[0x8c]) & 0xffffff,
            AIC7880(pci)->regs[REG_DFCNTRL]);
        aic7880_update_irq(AIC7880(pci));
        aic7880_host_dma(AIC7880(pci));
        if (!AIC7880(pci)->error_pending &&
            !(pci_get_word(pci->config + PCI_STATUS) &
              (PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT))) {
            aic7880_process_scsi_data(AIC7880(pci));
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
    unsigned fifo_len = s->host_fifo_len;
    uint8_t status = s->regs[REG_DFSTATUS] &
                     ~(DFS_FIFOQWDEMP | DFS_FIFOFULL | DFS_FIFOEMP);

    unsigned read = s->host_fifo_read;
    unsigned head = (8 - (read & 7)) & 7;

    if (fifo_len != sizeof(s->host_fifo) && fifo_len < head + 8) {
        status |= DFS_FIFOQWDEMP;
    }
    bool from_memory = s->regs[REG_DFCNTRL] & 0x04;
    bool ready = from_memory ?
        s->host_fifo_len < sizeof(s->host_fifo) :
        s->host_fifo_len &&
        (s->host_fifo_len == sizeof(s->host_fifo) ||
         (s->regs[REG_DFCNTRL] & DFC_FIFOFLUSH));

    status &= ~DFS_MREQPEND;
    if ((s->regs[REG_DFCNTRL] & 0x08) &&
        aic7880_count(s, 0x8c) && !s->error_pending && ready) {
        status |= DFS_MREQPEND;
    }
    if (s->host_fifo_len == sizeof(s->host_fifo)) {
        status |= DFS_FIFOFULL;
    }
    if (!fifo_len) {
        status |= DFS_FIFOEMP;
    }
    s->regs[REG_DFSTATUS] = status;
}

static void aic7880_error(AIC7880State *s, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR, "aic7880: %s\n", reason);
    s->error_serial++;
    s->error |= 0x40;
    s->error_pending = true;
    s->regs[REG_INTSTAT] |= 0x08;
    s->regs[REG_HCNTRL] |= HOST_PAUSE;
    timer_del(s->seq_timer);
    aic7880_update_irq(s);
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
    timer_del(s->seq_timer);
    aic7880_update_irq(s);
}

static bool aic7880_dma_attempt(AIC7880State *s, PCIDevice *pci,
                                uint32_t addr, bool to_memory,
                                uint8_t *buf, uint32_t len)
{
    MemTxResult result;

    if (to_memory) {
        result = pci_dma_write(pci, addr, buf, len);
    } else {
        result = pci_dma_read(pci, addr, buf, len);
    }
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

    if (!(control & 0x08) || !count ||
        s->error_pending ||
        (pci_get_word(pci->config + PCI_STATUS) &
         (PCI_STATUS_REC_MASTER_ABORT | PCI_STATUS_REC_TARGET_ABORT)) ||
        !(pci_get_word(pci->config + PCI_COMMAND) & PCI_COMMAND_MASTER) ||
        !(pci_get_word(pci->config + PCI_COMMAND) &
          (PCI_COMMAND_IO | PCI_COMMAND_MEMORY))) {
        return;
    }
    /* A short response can leave receive bytes for a STATUS flush. */
    if ((control & 0x30) && !s->req &&
        (from_memory || s->phase != AIC_PHASE_STATUS)) {
        return;
    }
    if ((s->selected && !s->req &&
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
        if (s->host_fifo_len != sizeof(s->host_fifo) &&
            !(control & DFC_FIFOFLUSH)) {
            return;
        }
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
        if (s->host_fifo_len && !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
            s->regs[REG_DFCNTRL] |= DFC_FIFOFLUSH;
        }
    }
    aic7880_update_scsi_irq(s);
}

static void aic7880_complete(SCSIRequest *req, size_t residual)
{
    AIC7880State *s = req->hba_private;
    bool from_data = s->phase == AIC_PHASE_DATA;
    bool phase_mismatch;

    s->regs[REG_SCSIDATL] = req->status;
    s->regs[REG_SCSISIGI] = SCSI_SIG_STATUS;
    phase_mismatch = s->expected_phase != 0xff &&
                     (s->regs[REG_SCSISIGI] & 0x02) &&
                     (s->regs[REG_SCSISIGI] & 0xe0) != s->expected_phase;
    if (phase_mismatch) {
        s->regs[REG_SSTAT1] |= 0x10;
        if (s->host_fifo_len &&
            !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
            s->regs[REG_DFCNTRL] |= DFC_FIFOFLUSH;
        }
    } else if (from_data && s->host_fifo_len &&
               aic7880_count(s, REG_STCNT) &&
               !(s->regs[REG_SBLKCTL] & SBLK_AUTOFLUSHDIS)) {
        aic7880_error(s, "phase change without mismatch is unsupported");
    }
    if (s->regs[REG_SXFRCTL0] & 0x08) {
        s->regs[REG_SSTAT0] |= 0x02;
    } else {
        s->regs[REG_SSTAT0] &= ~0x02;
    }
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
    SCSIRequest *req = s->req;
    bool from_target;

    if (!req || !s->scsi_data_len) {
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
    s->scsi_data_len = len;
    s->scsi_data_pos = 0;
    aic7880_process_scsi_data(s);
}

static void aic7880_start_request(AIC7880State *s)
{
    SCSIDevice *target = scsi_device_find(&s->bus, 0, s->target, 0);
    int32_t length;

    if (!target) {
        s->regs[REG_SSTAT1] |= 0x80;
        aic7880_update_scsi_irq(s);
        return;
    }
    s->req = scsi_req_new(target, 0, 0, s->cdb, s->cdb_need, s);
    length = scsi_req_enqueue(s->req);
    if (length && s->req) {
        scsi_req_continue(s->req);
    }
}

static void aic7880_select(AIC7880State *s)
{
    if (s->host_fifo_len) {
        aic7880_error(s, "SCSI selection with host-only FIFO data");
        return;
    }
    if (s->selected || s->req || s->peer_reselection) {
        aic7880_error(s, "selection while SCSI bus is occupied");
        return;
    }
    s->target = (s->regs[REG_SCSIID] >> 4) & 0x0f;
    s->cdb_len = s->cdb_need = 0;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->selected = scsi_device_find(&s->bus, 0, s->target, 0) != NULL;
    if (s->selected) {
        s->initiator_atn = s->regs[REG_SCSISEQ] & 0x08;
        s->phase = s->initiator_atn ? AIC_PHASE_MSG_OUT : AIC_PHASE_COMMAND;
        s->regs[REG_SCSISIGI] = s->initiator_atn ? SCSI_SIG_MSG_OUT :
                                 SCSI_SIG_COMMAND;
        s->regs[REG_SSTAT0] |= 0x42; /* Selection done, PIO ready. */
        s->regs[REG_SSTAT1] &= ~0x08;
        aic7880_update_scsi_irq(s);
    } else if (s->regs[REG_SXFRCTL1] & 0x04) {
        s->regs[REG_SSTAT1] |= 0x80;
        aic7880_update_scsi_irq(s);
    } else {
        s->regs[REG_SSTAT0] |= 0x10;
        aic7880_update_scsi_irq(s);
    }
}

static void aic7880_finish_message_out(AIC7880State *s)
{
    if (s->phase == AIC_PHASE_MSG_OUT && s->identify_seen &&
        !s->initiator_atn) {
        s->phase = AIC_PHASE_COMMAND;
        s->identify_seen = false;
        s->regs[REG_SCSISIGI] = SCSI_SIG_COMMAND;
        s->regs[REG_SSTAT0] |= 0x02;
        aic7880_update_scsi_irq(s);
    }
}

static void aic7880_scsi_reset(AIC7880State *s)
{
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    bus_cold_reset(BUS(&s->bus));
    s->selected = false;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->peer_reselection = false;
    s->peer_hold_data_in = false;
    s->peer_data_in_pending = false;
    s->peer_faulted_command = false;
    s->peer_data_len = s->peer_data_pos = 0;
    s->phase = AIC_PHASE_BUS_FREE;
    s->expected_phase = 0xff;
    s->cdb_len = s->cdb_need = 0;
    s->regs[REG_SCSISEQ] = 1;
    s->regs[REG_SCSISIGI] = 0;
    s->regs[REG_SSTAT0] = 0;
    s->regs[REG_SSTAT1] = 0x08 | (s->regs[REG_SSTAT1] & 0x04);
    aic7880_update_scsi_irq(s);
}

static void aic7880_chip_reset(AIC7880State *s)
{
    timer_del(s->seq_timer);
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->cdb, 0, sizeof(s->cdb));
    s->error = 0;
    s->error_serial = 0;
    s->cdb_len = s->cdb_need = s->target = 0;
    s->phase = AIC_PHASE_BUS_FREE;
    s->expected_phase = 0xff;
    s->qout_head = s->qout_tail = s->qout_count = 0;
    s->qin_head = s->qin_tail = s->qin_count = 0;
    s->return_depth = 0;
    s->scsi_data_len = s->scsi_data_pos = 0;
    s->host_fifo_read = 0;
    s->host_fifo_write = 0;
    s->host_fifo_len = 0;
    s->host_dma_active = false;
    s->selected = false;
    s->initiator_atn = false;
    s->identify_seen = false;
    s->peer_reselection = false;
    s->peer_scsi_irq_latched = false;
    s->peer_hold_data_in = false;
    s->peer_data_in_pending = false;
    s->peer_last_parity_bad = false;
    s->peer_faulted_command = false;
    s->peer_data_len = s->peer_data_pos = 0;
    s->maskable_break_pending = false;
    s->error_pending = false;
    s->regs[REG_DFSTATUS] = DFS_FIFOQWDEMP | DFS_FIFOEMP;
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
    aic7880_chip_reset(s);
}

static uint64_t aic7880_read(void *opaque, hwaddr addr, unsigned size)
{
    AIC7880State *s = opaque;

    addr &= AIC_REG_SPACE - 1;
    if (addr == REG_HCNTRL || addr == REG_INTSTAT ||
        addr == REG_ERROR || addr == REG_QOUTCNT) {
        trace_aic7880_host_reg(PCI_DEVICE(s)->devfn, false, addr,
                               addr == REG_QOUTCNT ? s->qout_count :
                               s->regs[addr],
                               ldl_le_p(&s->regs[0x88]),
                               aic7880_count(s, 0x8c),
                               s->regs[REG_DFCNTRL], s->regs[REG_INTSTAT]);
    }

    if (addr == REG_SCSIDATL) {
        uint8_t data = s->regs[REG_SCSIDATL];

        if (s->peer_reselection || s->peer_data_in_pending ||
            s->peer_faulted_command) {
            aic7880_error(s, "SCSI transfer during synthetic peer event");
            return 0xff;
        }

        if (s->phase == AIC_PHASE_STATUS ||
            s->phase == AIC_PHASE_MSG_IN) {
            if (!s->running) {
                aic7880_error(s, "manual status PIO is unsupported");
                return 0xff;
            }
            if (!(s->regs[REG_SXFRCTL0] & 0x08)) {
                aic7880_error(s, "automatic SCSI PIO is disabled");
                return 0xff;
            }
        }
        if (s->phase == AIC_PHASE_STATUS) {
            s->phase = AIC_PHASE_MSG_IN;
            s->regs[REG_SCSISIGI] = SCSI_SIG_MSG_IN;
            s->regs[REG_SCSIDATL] = 0; /* Command Complete. */
            aic7880_update_scsi_irq(s);
        } else if (s->phase == AIC_PHASE_MSG_IN) {
            s->phase = AIC_PHASE_BUS_FREE;
            s->initiator_atn = false;
            s->identify_seen = false;
            s->regs[REG_SCSISIGI] = 0;
            s->regs[REG_SSTAT0] &= ~0x42;
            s->regs[REG_SSTAT1] |= 0x08;
            s->selected = false;
            aic7880_update_scsi_irq(s);
        }
        return data;
    }
    if (addr >= REG_SCB_BASE &&
        addr < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        if (s->regs[REG_SCBPTR] >= AIC_SCB_PAGES) {
            qemu_log_mask(LOG_UNIMP, "aic7880: external SCB RAM is absent\n");
            return 0xff;
        }
        unsigned offset = (s->regs[REG_SCBCNT] & 0x80) ?
                          s->regs[REG_SCBCNT] & 0x1f : addr - REG_SCB_BASE;
        uint8_t data = s->scb_ram[s->regs[REG_SCBPTR] *
                                   AIC_SCB_PAGE_BYTES + offset];

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
    switch (addr) {
    case REG_SCSISIGI:
        return s->regs[addr] | (s->initiator_atn ? SCSI_SIG_ATN : 0);
    case REG_SCSISEQ:
    case REG_SXFRCTL0:
    case REG_SXFRCTL1:
    case REG_SCSIID:
    case REG_STCNT:
    case REG_STCNT + 1:
    case REG_STCNT + 2:
    case REG_SSTAT0:
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
    case REG_DSPCISTATUS:
        return s->regs[addr] & 0xc0;
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
    case REG_ERROR:
        return s->error |
               ((pci_get_word(PCI_DEVICE(s)->config + PCI_STATUS) &
                 AIC_PCI_ERROR_MASK) ? 0x40 : 0);
    case REG_HCNT_RSVD:
        return 0;
    case REG_DFDAT:
        if ((s->regs[REG_DFCNTRL] & 0x30) || s->req ||
            s->selected) {
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

            s->qout_head = (s->qout_head + 1) % ARRAY_SIZE(s->qout);
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
        return 0xff;
    default:
        qemu_log_mask(LOG_UNIMP, "aic7880: read of unsupported register "
                      "0x%02" HWADDR_PRIx "\n", addr);
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
        if (s->regs[REG_SCBPTR] >= AIC_SCB_PAGES) {
            qemu_log_mask(LOG_UNIMP, "aic7880: external SCB RAM is absent\n");
            return;
        }
        unsigned offset = (s->regs[REG_SCBCNT] & 0x80) ?
                          s->regs[REG_SCBCNT] & 0x1f : addr - REG_SCB_BASE;

        s->scb_ram[s->regs[REG_SCBPTR] * AIC_SCB_PAGE_BYTES + offset] = val;
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
            s->regs[REG_SSTAT0] &= ~0x04;
            aic7880_update_scsi_irq(s);
        }
        if (addr >= 0x8c && addr <= 0x8e) {
            s->regs[REG_DFSTATUS] &= ~0x08;
        }
        return;
    }
    switch (addr) {
    case REG_SSTAT0: /* CLRSINT0 on writes. */
        s->regs[REG_SSTAT0] &= ~(val & 0x7a);
        aic7880_update_scsi_irq(s);
        break;
    case REG_SSTAT1: /* CLRSINT1 on writes. */
        s->regs[REG_SSTAT1] &= ~(val & 0xef);
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
        if (val & ~0xac) {
            aic7880_error(s, "unsupported SIMODE1 event mask");
            break;
        }
        s->regs[addr] = val;
        aic7880_update_scsi_irq(s);
        break;
    case REG_SXFRCTL0:
        if (val & ~0x9a) {
            aic7880_error(s, "unsupported SXFRCTL0 control");
            break;
        }
        if (val & SXFR_CLRSTCNT) {
            memset(&s->regs[REG_STCNT], 0, 3);
            memset(&s->regs[REG_SHADDR], 0, 4);
            s->regs[REG_SSTAT0] &= ~0x04;
        }
        if (val & SXFR_CLRCHN) {
            if (s->req) {
                scsi_req_cancel(s->req);
            }
            s->host_fifo_len = 0;
            s->host_fifo_write = s->host_fifo_read;
            s->regs[REG_DFSTATUS] |= 1;
            s->regs[REG_DFCNTRL] &= ~DFC_FIFOFLUSH;
            s->selected = false;
            s->initiator_atn = false;
            s->identify_seen = false;
            s->peer_hold_data_in = false;
            s->peer_faulted_command = false;
            s->phase = AIC_PHASE_BUS_FREE;
            s->cdb_len = s->cdb_need = 0;
            s->regs[REG_SCSISIGI] = 0;
            s->regs[REG_SSTAT0] &= ~0x42;
        }
        /* DFON is a latch; REQ#/ACK# filter timing is not modeled. */
        s->regs[addr] = val & 0x88;
        if (!(val & 0x08)) {
            s->regs[REG_SSTAT0] &= ~0x02;
        } else if (s->phase == AIC_PHASE_COMMAND ||
                   s->phase == AIC_PHASE_STATUS ||
                   s->phase == AIC_PHASE_MSG_IN) {
            s->regs[REG_SSTAT0] |= 0x02;
        }
        aic7880_update_scsi_irq(s);
        break;
    case REG_SXFRCTL1:
        if (val & ~0x27) {
            aic7880_error(s, "unsupported SXFRCTL1 control");
            break;
        }
        /*
         * Ordinary targets lack parity metadata. ACTNEGEN/STPWEN are synthetic
         * electrical latches.
         */
        s->regs[addr] = val;
        break;
    case REG_SCSISIGI: /* SCSISIGO on writes. */
        if (val & ~0xe0) {
            aic7880_error(s, "unsupported SCSISIGO control");
            break;
        }
        s->expected_phase = val;
        break;
    case REG_SBLKCTL:
        if (val & ~SBLK_AUTOFLUSHDIS) {
            aic7880_error(s, "unsupported SBLKCTL control");
            break;
        }
        s->regs[addr] = val;
        break;
    case REG_DFCNTRL:
        if ((val & ~0x3f) || (val & 3) == 3) {
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
        s->regs[addr] = val & ~DFC_FIFORESET;
        if (val & DFC_FIFORESET) {
            if (s->host_fifo_len && s->req) {
                scsi_req_cancel(s->req);
            }
            s->host_fifo_read = s->regs[0x88] & 7;
            s->host_fifo_write = s->host_fifo_read;
            s->host_fifo_len = 0;
            s->regs[REG_DFSTATUS] =
                (s->regs[REG_DFSTATUS] & ~DFS_HDONE) |
                DFS_FIFOQWDEMP | DFS_FIFOEMP;
            aic7880_fifo_status(s);
        }
        if ((val & DFC_FIFOFLUSH) &&
            (!s->host_fifo_len || (s->regs[addr] & 0x04))) {
            s->regs[addr] &= ~DFC_FIFOFLUSH;
        }
        aic7880_host_dma(s);
        aic7880_process_scsi_data(s);
        break;
    case REG_DFDAT:
        if ((s->regs[REG_DFCNTRL] & 0x30) || s->req ||
            s->selected) {
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
        break;
    case REG_HCNT_RSVD:
        if (s->running) {
            aic7880_error(s, "sequencer write to reserved HCNT byte");
        }
        break;
    case REG_DINDIR:
        qemu_log_mask(LOG_UNIMP,
                      "aic7880: host write to DINDIR is unsupported\n");
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

        if (s->running || val >= AIC_SCB_PAGES ||
            s->qin_count == ARRAY_SIZE(s->qin)) {
            trace_aic7880_qinfifo(PCI_DEVICE(s)->devfn, true, val,
                                  count_before, count_before, paused,
                                  s->running, false);
            aic7880_error(s, "unsupported queue-in entry");
            break;
        }
        s->qin[s->qin_tail] = val;
        s->qin_tail = (s->qin_tail + 1) % ARRAY_SIZE(s->qin);
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
        }
        break;
    }
    case REG_SCSIDATL:
        if (!s->running) {
            aic7880_error(s, "manual command PIO is unsupported");
            break;
        }
        if (s->phase == AIC_PHASE_MSG_OUT) {
            if (!s->selected || s->identify_seen || val != 0x80) {
                aic7880_error(s, "unsupported Message Out byte");
                break;
            }
            s->identify_seen = true;
            s->regs[REG_SSTAT0] &= ~0x02;
            aic7880_finish_message_out(s);
            break;
        }
        if (s->phase != AIC_PHASE_COMMAND) {
            aic7880_error(s, "command PIO outside COMMAND phase");
            break;
        }
        if (s->req ||
            (s->cdb_need && s->cdb_len == s->cdb_need)) {
            aic7880_error(s, "CDB byte while command is active");
            break;
        }
        if (!s->selected || s->cdb_len >= sizeof(s->cdb)) {
            aic7880_error(s, "CDB byte without selected target");
            break;
        }
        s->regs[REG_SSTAT0] &= ~0x02;
        s->regs[REG_SCSISIGI] &= ~0x02;
        s->cdb[s->cdb_len++] = val;
        if (s->cdb_len == 1) {
            s->cdb_need = scsi_cdb_length(s->cdb);
            if (!s->cdb_need || s->cdb_need > sizeof(s->cdb)) {
                aic7880_error(s, "unsupported CDB length");
                break;
            }
        }
        if (s->cdb_len == s->cdb_need) {
            aic7880_start_request(s);
        } else {
            s->regs[REG_SSTAT0] |= 0x02;
            s->regs[REG_SCSISIGI] |= 0x02;
        }
        aic7880_update_scsi_irq(s);
        break;
    case REG_SEQCTL:
        if (!s->running && (val & SEQ_PAUSEDIS)) {
            qemu_log_mask(LOG_UNIMP,
                          "aic7880: unsupported host SEQCTL control 0x%02x\n",
                          val);
            break;
        }
        if (val & SEQ_LOADRAM) {
            timer_del(s->seq_timer);
        }
        if (val & SEQ_RESET) {
            s->seqaddr = 0;
            s->seqbyte = 0;
        }
        if ((val ^ s->regs[REG_SEQCTL]) & SEQ_LOADRAM) {
            s->seqbyte = 0;
        }
        s->regs[REG_SEQCTL] = val & ~SEQ_RESET;
        aic7880_update_irq(s);
        break;
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
                timer_del(s->seq_timer);
            }
            aic7880_update_irq(s);
            if (was_paused && !(val & HOST_PAUSE)) {
                aic7880_run(s);
            }
        }
        break;
    case REG_CLRINT:
        s->regs[REG_INTSTAT] &= ~(val & 0x0f);
        if ((val & 0x04) &&
            !(s->regs[REG_SSTAT0] & s->regs[REG_SIMODE0] & 0x20) &&
            !(s->regs[REG_SSTAT1] & s->regs[REG_SIMODE1] & 0x04)) {
            s->peer_scsi_irq_latched = false;
        }
        if (val & 0x08) {
            s->maskable_break_pending = false;
            s->error_pending = false;
        }
        aic7880_update_scsi_irq(s);
        break;
    default:
        if (s->running) {
            aic7880_error(s, "unsupported sequencer destination");
        } else {
            qemu_log_mask(LOG_UNIMP, "aic7880: write to unsupported register "
                          "0x%02" HWADDR_PRIx "\n", addr);
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
            s->error_serial++;
            s->regs[REG_HCNTRL] |= HOST_PAUSE;
            timer_del(s->seq_timer);
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
            aic7880_error(s, "sequencer read of empty queue-in FIFO");
            return 0xff;
        }
        id = s->qin[s->qin_head];
        s->qin_head = (s->qin_head + 1) % ARRAY_SIZE(s->qin);
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

static bool aic7880_dindir_target_supported(AIC7880State *s, uint8_t reg)
{
    if ((reg >= REG_STCNT && reg < REG_STCNT + 3) ||
        (reg >= REG_SHADDR && reg < REG_SHADDR + 4) ||
        (reg >= 0x20 && reg <= 0x5f) ||
        (reg >= 0x88 && reg <= 0x8e)) {
        return true;
    }
    if (reg >= REG_SCB_BASE && reg < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        return s->regs[REG_SCBPTR] < AIC_SCB_PAGES;
    }
    /* Pointer aliases and unselected write paths remain unsupported. */
    switch (reg) {
    case REG_SCSISEQ:
    case REG_SXFRCTL0:
    case REG_SXFRCTL1:
    case REG_SCSISIGI:
    case REG_SCSIID:
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
    if (s->running) {
        return;
    }
    timer_del(s->seq_timer);
    s->running = true;
    for (unsigned i = 0; i < AIC_SEQ_INSNS; i++) {
        uint32_t insn;
        uint16_t at = s->seqaddr;
        uint8_t op, imm, result;
        uint8_t updated_flags = s->regs[REG_FLAGS];
        uint8_t return_depth_before;
        uint8_t src, dst;
        uint32_t error_before;
        AICShiftKind shift_kind = AIC_SHIFT_INVALID;
        unsigned shift_count = 0;
        bool indirect_dest = false;

        if ((s->regs[REG_HCNTRL] & HOST_PAUSE) ||
            (s->regs[REG_SEQCTL] & SEQ_LOADRAM)) {
            goto done;
        }
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
        if (op == 0x09 || op == 0x0a) {
            aic7880_bad_opcode(s, op);
            goto done;
        }
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
            case 0x0b: /* CALL */
                value |= imm;
                aic7880_write(s, REG_SINDEX, value, 1);
                /* Selected transfer flag policy; fitted effects are unknown. */
                s->regs[REG_FLAGS] = (updated_flags & FLAGS_CARRY) |
                                     (value == 0 ? FLAGS_ZERO : 0);
                taken = true;
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
                    if (s->return_depth == ARRAY_SIZE(s->return_pc)) {
                        aic7880_error(s, "sequencer return stack full");
                        goto done;
                    }
                    s->return_pc[s->return_depth++] = s->seqaddr;
                }
                s->seqaddr = target;
            }
            if (s->regs[REG_SEQCTL] & SEQ_STEP) {
                s->regs[REG_HCNTRL] |= HOST_PAUSE;
                goto done;
            }
            if (taken && target == at && !s->req) {
                timer_mod(s->seq_timer,
                          qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                          AIC_SEQ_QUANTUM_US);
                goto done;
            }
            if (taken && s->req) {
                goto done;
            }
            continue;
        }
        if (op > 5) {
            aic7880_bad_opcode(s, op);
            goto done;
        }
        if (op == 0 && (insn & (1u << 24))) {
            aic7880_error(s, "unsupported OR return instruction");
            goto done;
        }
        if (op && (insn & (1u << 24)) && !s->return_depth) {
            aic7880_error(s, "sequencer return without call");
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
            if (!aic7880_dindir_target_supported(s, target) ||
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
        return_depth_before = s->return_depth;
        if (dst == REG_INTSTAT) {
            s->regs[REG_INTSTAT] |= result & 0x0f;
            if (result & 0x0d) {
                s->regs[REG_HCNTRL] |= HOST_PAUSE;
            }
            aic7880_update_irq(s);
        } else if (dst == REG_QOUTFIFO) {
            if (result >= AIC_SCB_PAGES) {
                aic7880_error(s, "unsupported completion identifier");
                goto done;
            }
            if (s->qout_count == ARRAY_SIZE(s->qout)) {
                aic7880_error(s, "completion queue full");
                goto done;
            }
            s->qout[s->qout_tail] = result;
            s->qout_tail = (s->qout_tail + 1) % ARRAY_SIZE(s->qout);
            s->qout_count++;
        } else {
            aic7880_write(s, dst, result, 1);
        }
        if (indirect_dest && s->error_serial == error_before) {
            s->regs[REG_DINDEX]++;
        }
        if (op >= 2 && op <= 4 && s->error_serial == error_before) {
            s->regs[REG_FLAGS] = updated_flags;
        }
        if (op && (insn & (1u << 24)) &&
            s->error_serial == error_before &&
            return_depth_before &&
            s->return_depth == return_depth_before) {
            /* The destination write reported no controller error. */
            s->seqaddr = s->return_pc[--s->return_depth];
        }
        if (s->regs[REG_SEQCTL] & SEQ_STEP) {
            s->regs[REG_HCNTRL] |= HOST_PAUSE;
            goto done;
        }
    }
    if (!(s->regs[REG_HCNTRL] & HOST_PAUSE) &&
        !(s->regs[REG_SEQCTL] & SEQ_LOADRAM)) {
        timer_mod(s->seq_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                               AIC_SEQ_QUANTUM_US);
    }
done:
    s->running = false;
}

static void aic7880_seq_timer_cb(void *opaque)
{
    aic7880_run(opaque);
}

static uint64_t aic7880_dspc_read(void *opaque, hwaddr addr, unsigned size)
{
    return aic7880_read(opaque, REG_DSPCISTATUS + addr, size);
}

static void aic7880_dspc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    aic7880_write(opaque, REG_DSPCISTATUS + addr, value, size);
}

static bool aic7880_dspc_accepts(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    return size == 1;
}

static const MemoryRegionOps aic7880_dspc_ops = {
    .read = aic7880_dspc_read,
    .write = aic7880_dspc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .valid.accepts = aic7880_dspc_accepts,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
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

    return (s->req || s->running || s->scsi_data_len ||
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
        s->peer_reselection = false;
        s->initiator_atn = false;
        s->regs[REG_SSTAT0] &= ~0x20;
        s->regs[REG_SSTAT1] |= 0x08;
        s->regs[REG_SCSISIGI] = 0;
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
        s->initiator_atn = false;
        s->phase = AIC_PHASE_BUS_FREE;
        s->regs[REG_SCSISIGI] = 0;
        s->regs[REG_SSTAT0] &= ~0x62;
        s->regs[REG_SSTAT1] |= 0x08;
        aic7880_update_scsi_irq(s);
        return;
    }
    error_setg(errp, "unsupported synthetic SCSI peer event");
}

static int aic7880_post_load(void *opaque, int version_id)
{
    AIC7880State *s = opaque;

    if (s->seqaddr > AIC_SEQ_INSNS ||
        s->seqbyte >= 4 ||
        s->phase >= AIC_PHASE_MSG_OUT ||
        s->cdb_len > sizeof(s->cdb) ||
        s->cdb_need > sizeof(s->cdb) ||
        s->qout_count > ARRAY_SIZE(s->qout) ||
        s->qout_head >= ARRAY_SIZE(s->qout) ||
        s->qout_tail >= ARRAY_SIZE(s->qout)) {
        return -EINVAL;
    }
    if (s->qin_count > ARRAY_SIZE(s->qin) ||
        s->qin_head >= ARRAY_SIZE(s->qin) ||
        s->qin_tail >= ARRAY_SIZE(s->qin)) {
        return -EINVAL;
    }
    if (s->return_depth > ARRAY_SIZE(s->return_pc)) {
        return -EINVAL;
    }
    if (s->host_fifo_len > sizeof(s->host_fifo) ||
        (uint8_t)(s->host_fifo_read + s->host_fifo_len) !=
        s->host_fifo_write) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < s->return_depth; i++) {
        if (s->return_pc[i] > AIC_SEQ_INSNS) {
            return -EINVAL;
        }
    }
    s->req = NULL;
    s->scsi_data_len = s->scsi_data_pos = 0;
    s->running = false;
    s->error_serial = 0;
    if (version_id < 2) {
        s->expected_phase = 0xff;
    }
    aic7880_update_scsi_irq(s);
    if ((s->regs[REG_HCNTRL] & HOST_PAUSE) ||
        (s->regs[REG_SEQCTL] & SEQ_LOADRAM)) {
        timer_del(s->seq_timer);
    }
    return 0;
}

static const VMStateDescription vmstate_aic7880 = {
    .name = "aic7880",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = aic7880_pre_save,
    .post_load = aic7880_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AIC7880State),
        VMSTATE_UINT8_ARRAY(regs, AIC7880State, AIC_REG_SPACE),
        VMSTATE_UINT8_ARRAY(seqram, AIC7880State, AIC_SEQ_BYTES),
        VMSTATE_UINT8_ARRAY(scb_ram, AIC7880State,
                            AIC_SCB_PAGES * AIC_SCB_PAGE_BYTES),
        VMSTATE_UINT16(seqaddr, AIC7880State),
        VMSTATE_TIMER_PTR(seq_timer, AIC7880State),
        VMSTATE_UINT8(seqbyte, AIC7880State),
        VMSTATE_UINT8(error, AIC7880State),
        VMSTATE_UINT8_ARRAY(cdb, AIC7880State, 16),
        VMSTATE_UINT8(cdb_len, AIC7880State),
        VMSTATE_UINT8(cdb_need, AIC7880State),
        VMSTATE_UINT8(target, AIC7880State),
        VMSTATE_UINT8(phase, AIC7880State),
        VMSTATE_UINT8_V(expected_phase, AIC7880State, 2),
        VMSTATE_UINT8_ARRAY(qout, AIC7880State, 16),
        VMSTATE_UINT8(qout_head, AIC7880State),
        VMSTATE_UINT8(qout_tail, AIC7880State),
        VMSTATE_UINT8(qout_count, AIC7880State),
        VMSTATE_UINT8_ARRAY(qin, AIC7880State, 16),
        VMSTATE_UINT8(qin_head, AIC7880State),
        VMSTATE_UINT8(qin_tail, AIC7880State),
        VMSTATE_UINT8(qin_count, AIC7880State),
        VMSTATE_UINT16_ARRAY(return_pc, AIC7880State, 4),
        VMSTATE_UINT8(return_depth, AIC7880State),
        VMSTATE_UINT8_ARRAY(host_fifo, AIC7880State, 256),
        VMSTATE_UINT8(host_fifo_read, AIC7880State),
        VMSTATE_UINT8(host_fifo_write, AIC7880State),
        VMSTATE_UINT16(host_fifo_len, AIC7880State),
        VMSTATE_BOOL(selected, AIC7880State),
        VMSTATE_BOOL(maskable_break_pending, AIC7880State),
        VMSTATE_BOOL(error_pending, AIC7880State),
        VMSTATE_END_OF_LIST()
    }
};

static const SCSIBusInfo aic7880_scsi_info = {
    .tcq = false,
    .max_target = 16,
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
        timer_del(s->seq_timer);
    }
    aic7880_update_irq(s);
}

static void aic7880_realize(PCIDevice *pci, Error **errp)
{
    AIC7880State *s = AIC7880(pci);
    unsigned dspc_windows;

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
    if (qtest_enabled()) {
        object_property_add_str(OBJECT(s), "x-test-pci-error", NULL,
                                aic7880_test_pci_error_set);
    }
    s->seq_timer = timer_new_us(QEMU_CLOCK_VIRTUAL,
                                aic7880_seq_timer_cb, s);
    pci->config[PCI_INTERRUPT_PIN] = 1;
    /* The selected AIC-7870 sibling policy covers readback only. */
    pci_word_test_and_set_mask(pci->wmask + PCI_COMMAND,
                               PCI_COMMAND_PARITY);
    memory_region_init_io(&s->io, OBJECT(s), &aic7880_regs_ops, s,
                          "aic7880-io", AIC_REG_SPACE);
    /* The fitted AIC-7880 BAR1 mask is unknown; use the selected sibling. */
    memory_region_init_io(&s->mem, OBJECT(s), &aic7880_regs_ops, s,
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
}

static const Property aic7880_properties[] = {
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
