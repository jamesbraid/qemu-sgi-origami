/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Adaptec AIC-7880 PCI SCSI controller, isolated initiator experiment.
 *
 * The downloaded sequencer controls selection and command delivery. This
 * subset accepts one asynchronous narrow target command at a time; other
 * sequencer operations and SCSI phases remain unsupported and observable.
 * Reset values transferred from the AIC-7870 manual are provisional.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/scsi/scsi.h"
#include "scsi/utils.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_AIC7880 "aic7880"
OBJECT_DECLARE_SIMPLE_TYPE(AIC7880State, AIC7880)

#define AIC_REG_SPACE 0x100
#define AIC_SEQ_INSNS 512
#define AIC_SEQ_BYTES (AIC_SEQ_INSNS * 4)
#define AIC_SCB_PAGES 16
#define AIC_SCB_PAGE_BYTES 32

#define REG_SCSISEQ  0x00
#define REG_SXFRCTL0 0x01
#define REG_SCSIID   0x05
#define REG_SCSIDATL 0x06
#define REG_STCNT    0x08
#define REG_SSTAT0   0x0b
#define REG_SSTAT1   0x0c
#define REG_SHADDR   0x14
#define REG_SEQCTL   0x60
#define REG_SEQRAM   0x61
#define REG_SEQADDR0 0x62
#define REG_SEQADDR1 0x63
#define REG_ACCUM    0x64
#define REG_ALLONES  0x69
#define REG_ALLZEROS 0x6a
#define REG_HCNTRL   0x87
#define REG_SCBPTR   0x90
#define REG_SCB_BASE 0xa0
#define REG_INTSTAT  0x91
#define REG_CLRINT   0x92
#define REG_ERROR    0x92
#define REG_DFCNTRL  0x93
#define REG_DFSTATUS 0x94
#define REG_QOUTFIFO 0x9d
#define REG_QOUTCNT  0x9e

#define SEQ_LOADRAM  0x01
#define SEQ_RESET    0x02
#define HOST_CHIPRST 0x01
#define HOST_INTEN   0x02
#define HOST_PAUSE   0x04
#define HOST_SWINT   0x10
#define HOST_POWRDN  0x40

struct AIC7880State {
    PCIDevice parent_obj;
    MemoryRegion io;
    MemoryRegion mem;
    SCSIBus bus;
    uint8_t regs[AIC_REG_SPACE];
    uint8_t seqram[AIC_SEQ_BYTES];
    uint8_t scb_ram[AIC_SCB_PAGES * AIC_SCB_PAGE_BYTES];
    uint16_t seqaddr;
    uint8_t seqbyte;
    uint8_t error;
    uint8_t cdb[16];
    uint8_t cdb_len;
    uint8_t cdb_need;
    uint8_t target;
    uint8_t qout[16];
    uint8_t qout_head;
    uint8_t qout_tail;
    uint8_t qout_count;
    bool selected;
    bool running;
    SCSIRequest *req;
};

static void aic7880_run(AIC7880State *s);

static void aic7880_update_irq(AIC7880State *s)
{
    bool enabled = s->regs[REG_HCNTRL] & HOST_INTEN;
    bool pending = (s->regs[REG_INTSTAT] & 0x0f) ||
                   (s->regs[REG_HCNTRL] & HOST_SWINT);

    pci_set_irq(PCI_DEVICE(s), enabled && pending &&
                !(s->regs[REG_HCNTRL] & HOST_POWRDN) &&
                (PCI_DEVICE(s)->config[PCI_COMMAND] & PCI_COMMAND_MASTER));
}

static void aic7880_write_config(PCIDevice *pci, uint32_t addr,
                                  uint32_t val, int len)
{
    uint16_t before = pci_get_word(pci->config + PCI_COMMAND);

    pci_default_write_config(pci, addr, val, len);
    if (pci_get_word(pci->config + PCI_COMMAND) != before) {
        aic7880_update_irq(AIC7880(pci));
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

static void aic7880_error(AIC7880State *s, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR, "aic7880: %s\n", reason);
    s->error |= 0x40;
    s->regs[REG_INTSTAT] |= 0x08;
    s->regs[REG_HCNTRL] |= HOST_PAUSE;
    aic7880_update_irq(s);
}

static void aic7880_bad_opcode(AIC7880State *s, uint8_t opcode)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "aic7880: illegal sequencer opcode %u\n", opcode);
    s->error |= 0x04;
    s->regs[REG_INTSTAT] |= 0x08;
    s->regs[REG_HCNTRL] |= HOST_PAUSE;
    aic7880_update_irq(s);
}

static void aic7880_complete(SCSIRequest *req, size_t residual)
{
    AIC7880State *s = req->hba_private;

    s->regs[REG_SCSIDATL] = req->status;
    s->regs[REG_SSTAT1] |= 0x08; /* Bus free after status and message. */
    s->selected = false;
    s->req = NULL;
    scsi_req_unref(req);
    if (!s->running) {
        aic7880_run(s);
    }
}

static void aic7880_cancel(SCSIRequest *req)
{
    AIC7880State *s = req->hba_private;

    if (s->req == req) {
        s->req = NULL;
        scsi_req_unref(req);
    }
}

static void aic7880_transfer_data(SCSIRequest *req, uint32_t len)
{
    AIC7880State *s = req->hba_private;
    PCIDevice *pci = PCI_DEVICE(s);
    uint8_t *data = scsi_req_get_buf(req);
    uint32_t haddr = ldl_le_p(&s->regs[0x88]);
    uint32_t shaddr = ldl_le_p(&s->regs[REG_SHADDR]);
    uint32_t hcnt = aic7880_count(s, 0x8c);
    uint32_t stcnt = aic7880_count(s, REG_STCNT);
    bool from_target = req->cmd.mode == SCSI_XFER_FROM_DEV;
    uint8_t fifo[64];
    uint32_t done = 0;

    if ((s->regs[REG_DFCNTRL] & 0x38) != 0x38 ||
        !!(s->regs[REG_DFCNTRL] & 0x04) == from_target ||
        len > hcnt || len > stcnt) {
        aic7880_error(s, "invalid SCSI data path or transfer count");
        scsi_req_cancel(req);
        return;
    }
    while (done < len) {
        uint32_t chunk = MIN(len - done, sizeof(fifo));
        MemTxResult result;

        if (from_target) {
            memcpy(fifo, data + done, chunk);
            result = pci_dma_write(pci, haddr + done, fifo, chunk);
        } else {
            result = pci_dma_read(pci, haddr + done, fifo, chunk);
            if (result == MEMTX_OK) {
                memcpy(data + done, fifo, chunk);
            }
        }
        if (result != MEMTX_OK) {
            aic7880_error(s, "PCI DMA refused by bus address space");
            scsi_req_cancel(req);
            return;
        }
        done += chunk;
    }
    haddr += len;
    shaddr += len;
    stl_le_p(&s->regs[0x88], haddr);
    stl_le_p(&s->regs[REG_SHADDR], shaddr);
    aic7880_set_count(s, 0x8c, hcnt - len);
    aic7880_set_count(s, REG_STCNT, stcnt - len);
    if (hcnt == len) {
        s->regs[REG_DFSTATUS] |= 0x09; /* Host done, FIFO empty. */
    }
    if (stcnt == len) {
        s->regs[REG_SSTAT0] |= 0x04;
    }
    scsi_req_continue(req);
}

static void aic7880_start_request(AIC7880State *s)
{
    SCSIDevice *target = scsi_device_find(&s->bus, 0, s->target, 0);
    int32_t length;

    if (!target) {
        s->regs[REG_SSTAT1] |= 0x80;
        s->regs[REG_INTSTAT] |= 0x04;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_update_irq(s);
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
    if (s->selected || s->req) {
        aic7880_error(s, "selection while command is active");
        return;
    }
    s->target = (s->regs[REG_SCSIID] >> 4) & 0x0f;
    s->cdb_len = s->cdb_need = 0;
    s->selected = scsi_device_find(&s->bus, 0, s->target, 0) != NULL;
    if (s->selected) {
        s->regs[REG_SSTAT0] |= 0x42; /* Selection done, PIO ready. */
        s->regs[REG_SSTAT1] &= ~0x08;
    } else {
        s->regs[REG_SSTAT1] |= 0x80;
        s->regs[REG_INTSTAT] |= 0x04;
        s->regs[REG_HCNTRL] |= HOST_PAUSE;
        aic7880_update_irq(s);
    }
}

static void aic7880_chip_reset(AIC7880State *s)
{
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->cdb, 0, sizeof(s->cdb));
    s->error = 0;
    s->cdb_len = s->cdb_need = s->target = 0;
    s->qout_head = s->qout_tail = s->qout_count = 0;
    s->selected = false;
    s->regs[REG_DFSTATUS] = 0x01;
    s->regs[REG_SEQCTL] = 0x90;
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

    if (addr >= AIC_REG_SPACE) {
        return 0xff;
    }
    if (addr >= REG_SCB_BASE &&
        addr < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        if (s->regs[REG_SCBPTR] >= AIC_SCB_PAGES) {
            qemu_log_mask(LOG_UNIMP, "aic7880: external SCB RAM is absent\n");
            return 0xff;
        }
        return s->scb_ram[s->regs[REG_SCBPTR] * AIC_SCB_PAGE_BYTES +
                          addr - REG_SCB_BASE];
    }
    if ((addr <= 0x11) ||
        (addr >= REG_SHADDR && addr < REG_SHADDR + 4) ||
        (addr >= 0x88 && addr <= 0x8e) ||
        addr == REG_DFCNTRL || addr == REG_DFSTATUS ||
        (addr >= 0x20 && addr <= 0x5f)) {
        return s->regs[addr];
    }
    switch (addr) {
    case REG_SEQCTL:
    case REG_HCNTRL:
    case REG_SCBPTR:
    case REG_INTSTAT:
        return s->regs[addr];
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
        return s->error;
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

    if (addr >= AIC_REG_SPACE) {
        return;
    }
    if (addr >= REG_SCB_BASE &&
        addr < REG_SCB_BASE + AIC_SCB_PAGE_BYTES) {
        if (s->regs[REG_SCBPTR] >= AIC_SCB_PAGES) {
            qemu_log_mask(LOG_UNIMP, "aic7880: external SCB RAM is absent\n");
            return;
        }
        s->scb_ram[s->regs[REG_SCBPTR] * AIC_SCB_PAGE_BYTES +
                   addr - REG_SCB_BASE] = val;
        return;
    }
    if ((addr >= 0x20 && addr <= 0x5f) ||
        (addr >= REG_SHADDR && addr < REG_SHADDR + 4) ||
        (addr >= 0x88 && addr <= 0x8e) ||
        (addr >= REG_STCNT && addr < REG_STCNT + 3)) {
        s->regs[addr] = val;
        if (addr >= REG_STCNT && addr < REG_STCNT + 3) {
            s->regs[REG_SSTAT0] &= ~0x04;
        }
        if (addr >= 0x8c && addr <= 0x8e) {
            s->regs[REG_DFSTATUS] &= ~0x08;
        }
        return;
    }
    switch (addr) {
    case REG_SSTAT0: /* CLRSINT0 on writes. */
        s->regs[REG_SSTAT0] &= ~(val & 0x7a);
        break;
    case REG_SSTAT1: /* CLRSINT1 on writes. */
        s->regs[REG_SSTAT1] &= ~(val & 0xef);
        break;
    case REG_SXFRCTL0:
    case REG_SCSIID:
    case REG_SCBPTR:
    case REG_DFCNTRL:
        s->regs[addr] = val;
        break;
    case REG_SCSISEQ:
        s->regs[addr] = val;
        if (val & 0x40) {
            aic7880_select(s);
        }
        break;
    case REG_SCSIDATL:
        if (s->req ||
            (s->cdb_need && s->cdb_len == s->cdb_need)) {
            aic7880_error(s, "CDB byte while command is active");
            break;
        }
        if (!s->selected || s->cdb_len >= sizeof(s->cdb)) {
            aic7880_error(s, "CDB byte without selected target");
            break;
        }
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
        }
        break;
    case REG_SEQCTL:
        if (val & SEQ_RESET) {
            s->seqaddr = 0;
            s->seqbyte = 0;
        }
        if ((val ^ s->regs[REG_SEQCTL]) & SEQ_LOADRAM) {
            s->seqbyte = 0;
        }
        s->regs[REG_SEQCTL] = val & ~SEQ_RESET;
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
            aic7880_update_irq(s);
            if (was_paused && !(val & HOST_PAUSE)) {
                aic7880_run(s);
            }
        }
        break;
    case REG_CLRINT:
        s->regs[REG_INTSTAT] &= ~(val & 0x0f);
        aic7880_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "aic7880: write to unsupported register "
                      "0x%02" HWADDR_PRIx "\n", addr);
        break;
    }
}

static uint8_t aic7880_seq_read(AIC7880State *s, uint8_t reg)
{
    if (reg == REG_ALLZEROS) {
        return 0;
    }
    if (reg == REG_ALLONES) {
        return 0xff;
    }
    return aic7880_read(s, reg, 1);
}

static void aic7880_run(AIC7880State *s)
{
    if (s->running) {
        return;
    }
    s->running = true;
    for (unsigned i = 0; i < AIC_SEQ_INSNS; i++) {
        uint32_t insn;
        uint16_t at = s->seqaddr;
        uint8_t op, imm, src, dst, result;

        if ((s->regs[REG_HCNTRL] & HOST_PAUSE) ||
            (s->regs[REG_SEQCTL] & SEQ_LOADRAM)) {
            goto done;
        }
        if (at >= AIC_SEQ_INSNS) {
            aic7880_error(s, "sequencer ran past instruction RAM");
            goto done;
        }
        insn = ldl_le_p(&s->seqram[at * 4]);
        s->seqaddr = at + 1;
        op = (insn >> 25) & 0x0f;
        imm = insn;
        src = (insn >> 8) & 0xff;
        dst = (insn >> 16) & 0xff;
        if (op == 0x0f || op == 0x08) {
            uint16_t target = (insn >> 16) & 0x1ff;
            uint8_t mask = imm ? imm : s->regs[REG_ACCUM];
            bool taken = op == 0x08 ||
                         !(aic7880_seq_read(s, src) & mask);

            if (taken) {
                s->seqaddr = target;
                if (target == at || s->req) {
                    goto done;
                }
            }
            continue;
        }
        if (op != 0) {
            aic7880_bad_opcode(s, op);
            goto done;
        }
        result = aic7880_seq_read(s, src) |
                 (imm ? imm : s->regs[REG_ACCUM]);
        if (dst == REG_INTSTAT) {
            s->regs[REG_INTSTAT] |= result & 0x0f;
            if (result & 0x0d) {
                s->regs[REG_HCNTRL] |= HOST_PAUSE;
            }
            aic7880_update_irq(s);
        } else if (dst == REG_QOUTFIFO) {
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
    }
    aic7880_error(s, "sequencer run limit");
done:
    s->running = false;
}

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

    return (s->req || s->running) ? -EBUSY : 0;
}

static int aic7880_post_load(void *opaque, int version_id)
{
    AIC7880State *s = opaque;

    if (s->seqaddr > AIC_SEQ_INSNS ||
        s->seqbyte >= 4 ||
        s->cdb_len > sizeof(s->cdb) ||
        s->cdb_need > sizeof(s->cdb) ||
        s->qout_count > ARRAY_SIZE(s->qout) ||
        s->qout_head >= ARRAY_SIZE(s->qout) ||
        s->qout_tail >= ARRAY_SIZE(s->qout)) {
        return -EINVAL;
    }
    s->req = NULL;
    s->running = false;
    aic7880_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_aic7880 = {
    .name = "aic7880",
    .version_id = 1,
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
        VMSTATE_UINT8(seqbyte, AIC7880State),
        VMSTATE_UINT8(error, AIC7880State),
        VMSTATE_UINT8_ARRAY(cdb, AIC7880State, 16),
        VMSTATE_UINT8(cdb_len, AIC7880State),
        VMSTATE_UINT8(cdb_need, AIC7880State),
        VMSTATE_UINT8(target, AIC7880State),
        VMSTATE_UINT8_ARRAY(qout, AIC7880State, 16),
        VMSTATE_UINT8(qout_head, AIC7880State),
        VMSTATE_UINT8(qout_tail, AIC7880State),
        VMSTATE_UINT8(qout_count, AIC7880State),
        VMSTATE_BOOL(selected, AIC7880State),
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

static void aic7880_realize(PCIDevice *pci, Error **errp)
{
    AIC7880State *s = AIC7880(pci);

    pci->config[PCI_INTERRUPT_PIN] = 1;
    memory_region_init_io(&s->io, OBJECT(s), &aic7880_regs_ops, s,
                          "aic7880-io", AIC_REG_SPACE);
    memory_region_init_io(&s->mem, OBJECT(s), &aic7880_regs_ops, s,
                          "aic7880-mmio", AIC_REG_SPACE);
    pci_register_bar(pci, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(pci, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mem);
    scsi_bus_init(&s->bus, sizeof(s->bus), DEVICE(s), &aic7880_scsi_info);
}

static void aic7880_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    pc->realize = aic7880_realize;
    pc->config_write = aic7880_write_config;
    pc->vendor_id = 0x9004;
    pc->device_id = 0x8078;
    pc->class_id = PCI_CLASS_STORAGE_SCSI;
    rc->phases.hold = aic7880_reset_hold;
    dc->desc = "Adaptec AIC-7880 SCSI host adapter";
    dc->vmsd = &vmstate_aic7880;
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
