/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Adaptec AIC-1160 Fibre Channel controller.
 *
 * The part is a microsequencer controller. The host downloads microcode
 * into an on-chip sequencer RAM through a data port, starts the sequencer,
 * tells it where its rings are by writing the sequencer's scratch RAM, and
 * hands it command blocks (TCBs) with a producer index. As with the QLogic
 * parts in qlogic-isp2200.c, the downloaded code is accepted, retained and
 * read back but never executed; the model does what the sequencer would do
 * to a SCSI command and its completion.
 *
 * Behind the host interface sits a private arbitrated loop kept inside the
 * device: every SCSI device on the controller's bus is one LUN of the loop
 * port whose loop ID is its target number. The contract and its sources
 * are in docs/system/mips/sgi/hardware/adaptec-aic1160.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/sgi-scsi-cd.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "scsi/constants.h"
#include "system/dma.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_AIC1160 "aic1160"
OBJECT_DECLARE_SIMPLE_TYPE(AIC1160State, AIC1160)

#define AIC1160_VENDOR_ID           0x9004
#define AIC1160_DEVICE_ID           0x1160
#define AIC1160_WINDOW_SIZE         0x100

/* The chip registers are I/O BAR 0 and the sequencer memory I/O BAR 3. */
#define AIC1160_BAR_REGS            0
#define AIC1160_BAR_SEQ_MEM         3

/* A device-config register of the chip; bit 2 enables 64-bit addressing. */
#define AIC1160_CFG_DEVICE          0x40
/* The driver requests the LIP primitive by writing this config byte. */
#define AIC1160_CFG_LIP_REQUEST     0x4b

/* Fibre Channel serial bus controller; attach does not check it. */
#define AIC1160_CLASS_FIBRE_CHANNEL 0x0c04

#define AIC1160_REG_SEQ_CTL         0x00
#define AIC1160_REG_SEQ_ADDR        0x02
#define AIC1160_REG_SEQ_DATA        0x14
#define AIC1160_REG_HOST_CTL        0x22
#define AIC1160_REG_ERROR_STATUS    0x23
#define AIC1160_REG_INT_STATUS      0x24
#define AIC1160_REG_COMPLETE_CTL    0x29
#define AIC1160_REG_DOORBELL        0x2a
#define AIC1160_REG_BANK            0x2c
#define AIC1160_REG_SCRATCH_PAGE    0x34
#define AIC1160_REG_SEEPROM_DATA    0x68
#define AIC1160_REG_SEEPROM_ADDR    0x6a
#define AIC1160_REG_SEEPROM_CMD     0x6b
#define AIC1160_REG_TCB_CONSUMER    0x6a
#define AIC1160_REG_GLOBAL_CELLS    0x38
#define AIC1160_GLOBAL_CELLS        8
#define AIC1160_REG_CMD_DATA        0x20
#define AIC1160_REG_CMD_ADDR        0x9e
#define AIC1160_CMD_BUF_SIZE        128
#define AIC1160_REG_SG_DATA         0x60
#define AIC1160_REG_SG_REWIND       0x8b
#define AIC1160_REG_TCB_INDEX       0x64
#define AIC1160_SG_AREA_SIZE        128
#define AIC1160_REG_LINK_SPEED      0x63
#define AIC1160_REG_STATUS_BLOCK    0x70
#define AIC1160_REG_LOOP_STATE      0xa1

#define AIC1160_BANKS               8
#define AIC1160_BANK_SEEPROM        0
#define AIC1160_BANK_LOOP           1
#define AIC1160_BANK_SG             2
#define AIC1160_BANK_DMA            4

#define AIC1160_SEQ_CTL_LOAD        0x01
#define AIC1160_SEQ_CTL_RUN         0x40
/* Restart the sequencer's program counter at the entry point. */
#define AIC1160_SEQ_CTL_RESTART     0x02

#define AIC1160_HOST_CTL_RESET      0x01
#define AIC1160_HOST_CTL_FC_RESET   0x02
#define AIC1160_HOST_CTL_IRQ_EN     0x04
#define AIC1160_HOST_CTL_PAUSE      0x40
#define AIC1160_HOST_CTL_PAUSE_TOP  0x80

/* Bank 4 register 0x63 reads the clock divisor 1 value, full link speed. */
#define AIC1160_LINK_SPEED_FULL     0xbe

/* Bank 4 register 0x29: arms completion and status-block posting. */
#define AIC1160_COMPLETE_ARM        0x20

/* Interrupt status: bit 3 of byte 0 and all of byte 1 are sources. */
#define AIC1160_INT_PENDING_MASK    0xff08
#define AIC1160_INT_LOOP            0x0008
#define AIC1160_INT_COMPLETE        0x1000
#define AIC1160_INT_LIP_READY       0x0100

/* Sequencer RAM: 4096 bytes. */
#define AIC1160_SEQ_RAM_WORDS       2048

/* The sequencer memory window: 128-byte scratch pages, then more RAM. */
#define AIC1160_SCRATCH_PAGE_SIZE   0x80
#define AIC1160_SCRATCH_PAGES       4
#define AIC1160_SCRATCH_DONE_BASE   0x08
#define AIC1160_SCRATCH_TCB_BASE    0x1c
#define AIC1160_SCRATCH_DONE_NEG    0x48
#define AIC1160_SCRATCH_PASS        0x45

#define AIC1160_SEEPROM_WORDS       256
#define AIC1160_SEEPROM_WWN_WORD    3
#define AIC1160_SEEPROM_CONTROL     0x000f
/* Word 2, the media sense: voltage sensing, as a copper port is. */
#define AIC1160_SEEPROM_MEDIA_COPPER 0x0100
/* A plausible name for a port given none: NAA 1, no vendor. */
#define AIC1160_DEFAULT_WWN         UINT64_C(0x2100000000000001)
#define AIC1160_SEEPROM_OP_READ     0x61
#define AIC1160_SEEPROM_BUSY        0x01

#define AIC1160_TCB_SIZE            128
#define AIC1160_MAX_TCBS            512
#define AIC1160_DONE_ENTRY_SIZE     4
#define AIC1160_RSP_SIZE            256
#define AIC1160_SG_STRIDE           16
#define AIC1160_SG_MAX              4096
#define AIC1160_SG_LAST             1
#define AIC1160_SG_LEN_MASK         0x00ffffff

#define AIC1160_TCB_MODE_FLAT       0x08
#define AIC1160_TCB_MODE_SG         0x0b

/* FCP_RSP status flags, byte 0x0a. */
#define AIC1160_RSP_SENSE_VALID     0x02
#define AIC1160_RSP_RESID_OVER      0x04
#define AIC1160_RSP_RESID_UNDER     0x08

/* The adapter residual-info block in the response buffer. */
#define AIC1160_RSP_RESID_INFO      0x80

/* Done-queue completion status codes. */
#define AIC1160_DONE_OK             0x00
#define AIC1160_DONE_OVERRUN        0x01
#define AIC1160_DONE_UNDERRUN       0x02
#define AIC1160_DONE_ERROR_RSP      0x04
#define AIC1160_DONE_FORMAT         0x06
#define AIC1160_DONE_NO_DEVICE      0x08

/* TCB fields that tell a link service from a SCSI command. */
#define AIC1160_TCB_R_CTL           0x07
#define AIC1160_TCB_TYPE            0x0f
#define AIC1160_TCB_CONTROL         0x21
#define AIC1160_TCB_MODE_NONE       0x0c
#define AIC1160_R_CTL_ELS           0x22
#define AIC1160_TYPE_ELS            0x01
#define AIC1160_CONTROL_ELS         0x0a
#define AIC1160_CONTROL_RECEIVE     0x01
#define AIC1160_TCB_HANDLE          0x68

#define AIC1160_TCB_F_CTL2          0x0e
/* The loop image is 822 words and four of padding; the mainline 1956. */
#define AIC1160_LOOP_IMAGE_MIN      800
#define AIC1160_LOOP_IMAGE_MAX      900
/* The LIP TCB: the map buffer, and where the ring continues afterwards. */
#define AIC1160_LIP_MAP_ADDR        0x38
#define AIC1160_TCB_RAM_OFFSET      AIC1160_SCRATCH_PAGE_SIZE

#define AIC1160_ELS_PLOGI           0x03
#define AIC1160_ELS_PRLI            0x20
#define AIC1160_ELS_ADISC           0x52

/* The host's loop ID when it asks for none, a driver default. */
#define AIC1160_HOST_LOOP_ID        125
#define AIC1160_LIP_MAP_SIZE        132

#define AIC1160_MAX_LOOP_ID         125
#define AIC1160_MAX_LUN             255

/* Loop ID n is the standard table's AL_PA n; see qlogic-isp2200.c. */
static const uint8_t aic1160_alpa[AIC1160_MAX_LOOP_ID + 1] = {
    0xef, 0xe8, 0xe4, 0xe2, 0xe1, 0xe0, 0xdc, 0xda,
    0xd9, 0xd6, 0xd5, 0xd4, 0xd3, 0xd2, 0xd1, 0xce,
    0xcd, 0xcc, 0xcb, 0xca, 0xc9, 0xc7, 0xc6, 0xc5,
    0xc3, 0xbc, 0xba, 0xb9, 0xb6, 0xb5, 0xb4, 0xb3,
    0xb2, 0xb1, 0xae, 0xad, 0xac, 0xab, 0xaa, 0xa9,
    0xa7, 0xa6, 0xa5, 0xa3, 0x9f, 0x9e, 0x9d, 0x9b,
    0x98, 0x97, 0x90, 0x8f, 0x88, 0x84, 0x82, 0x81,
    0x80, 0x7c, 0x7a, 0x79, 0x76, 0x75, 0x74, 0x73,
    0x72, 0x71, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69,
    0x67, 0x66, 0x65, 0x63, 0x5c, 0x5a, 0x59, 0x56,
    0x55, 0x54, 0x53, 0x52, 0x51, 0x4e, 0x4d, 0x4c,
    0x4b, 0x4a, 0x49, 0x47, 0x46, 0x45, 0x43, 0x3c,
    0x3a, 0x39, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
    0x2e, 0x2d, 0x2c, 0x2b, 0x2a, 0x29, 0x27, 0x26,
    0x25, 0x23, 0x1f, 0x1e, 0x1d, 0x1b, 0x18, 0x17,
    0x10, 0x0f, 0x08, 0x04, 0x02, 0x01,
};

typedef struct AIC1160Request {
    AIC1160State *dev;
    SCSIRequest *sreq;
    QEMUSGList qsg;
    uint64_t described;     /* Buffer bytes the descriptors named. */
    uint64_t transferred;
    uint64_t sg_offset;
    uint32_t sg_index;
    uint32_t handle;
    uint32_t fcp_dl;
    uint64_t rsp_addr;
    uint64_t desc_addr;     /* The buffer, or the descriptor list. */
    bool desc_list;
    uint32_t *desc_len;     /* Each descriptor's length. */
    uint32_t ndesc;
    /* A reset is cancelling this request: it posts nothing. */
    bool dropping;
    QTAILQ_ENTRY(AIC1160Request) next;
} AIC1160Request;

struct AIC1160State {
    PCIDevice parent_obj;

    MemoryRegion regs;
    MemoryRegion seq_mem;
    SCSIBus bus;

    uint8_t seq_ctl;
    uint16_t seq_addr;
    uint16_t seq_ram[AIC1160_SEQ_RAM_WORDS];
    uint8_t host_ctl;
    uint16_t int_status;
    uint16_t doorbell;
    uint8_t bank;
    uint8_t scratch_page;
    /* The register cells that only store, by bank. */
    uint8_t bank_reg[AIC1160_BANKS][AIC1160_WINDOW_SIZE];
    uint8_t scratch[AIC1160_SCRATCH_PAGES][AIC1160_SCRATCH_PAGE_SIZE];
    /* The on-chip TCB RAM: the BAR 3 window's upper half, by TCB index. */
    uint8_t tcb_ram[AIC1160_MAX_TCBS][AIC1160_SCRATCH_PAGE_SIZE];
    uint16_t tcb_index;
    /* Words of the image the sequencer RAM holds. */
    uint16_t fw_words;
    /* The TCB ring: its base, and the pointer last seen in scratch. */
    uint64_t ring_base;
    uint64_t ring_seen;
    /* Where the chip fetches the next TCB. */
    uint64_t ring_ptr;
    uint16_t seeprom[AIC1160_SEEPROM_WORDS];
    /* RAM regions the chip's self test exercises. */
    uint8_t sg_ram[AIC1160_MAX_TCBS][AIC1160_SG_AREA_SIZE];
    uint8_t sg_ptr;
    uint8_t global_cells[AIC1160_GLOBAL_CELLS];
    uint8_t cmd_buf[AIC1160_CMD_BUF_SIZE];
    uint8_t cmd_ptr;
    uint16_t tcb_consumer;
    uint16_t done_index;
    uint8_t done_lap;
    /* Words the guest has written to the sequencer RAM since load mode. */
    uint32_t words_loaded;

    QTAILQ_HEAD(, AIC1160Request) requests;

    /* Properties. */
    uint8_t revision;
    uint64_t wwn;
    char *bus_name;
};

static bool aic1160_running(AIC1160State *s)
{
    return (s->seq_ctl & AIC1160_SEQ_CTL_RUN) != 0;
}

static bool aic1160_bus_master(AIC1160State *s)
{
    return pci_get_word(PCI_DEVICE(s)->config + PCI_COMMAND) &
           PCI_COMMAND_MASTER;
}

static MemTxResult aic1160_dma_read(AIC1160State *s, uint64_t addr,
                                    void *buf, dma_addr_t len)
{
    if (!aic1160_bus_master(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: DMA read of 0x%" PRIx64 " refused: bus "
                      "mastering is disabled\n", addr);
        return MEMTX_ERROR;
    }
    return pci_dma_read(PCI_DEVICE(s), addr, buf, len);
}

static MemTxResult aic1160_dma_write(AIC1160State *s, uint64_t addr,
                                     const void *buf, dma_addr_t len)
{
    if (!aic1160_bus_master(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: DMA write of 0x%" PRIx64 " refused: bus "
                      "mastering is disabled\n", addr);
        return MEMTX_ERROR;
    }
    return pci_dma_write(PCI_DEVICE(s), addr, buf, len);
}

static void aic1160_update_irq(AIC1160State *s)
{
    bool level = (s->int_status & AIC1160_INT_PENDING_MASK) &&
                 (s->host_ctl & AIC1160_HOST_CTL_IRQ_EN);

    trace_aic1160_irq(level);
    pci_set_irq(PCI_DEVICE(s), level);
}

/*
 * Status bits arise: the chip copies status bytes 0x24 and 0x25 to the
 * host's status block when completion posting is armed, then raises the
 * line if the master enable lets it.
 */
static void aic1160_raise(AIC1160State *s, uint16_t bits)
{
    uint8_t *bank4 = s->bank_reg[AIC1160_BANK_DMA];

    s->int_status |= bits;
    if ((bank4[AIC1160_REG_COMPLETE_CTL] & AIC1160_COMPLETE_ARM) &&
        aic1160_bus_master(s)) {
        uint64_t addr = ldq_le_p(bank4 + AIC1160_REG_STATUS_BLOCK);
        uint8_t mirror[2] = { s->int_status & 0xff, s->int_status >> 8 };

        if (addr) {
            aic1160_dma_write(s, addr, mirror, sizeof(mirror));
        }
    }
    aic1160_update_irq(s);
}

/* The queue size the driver wrote into scratch page 0, negated. */
static unsigned aic1160_ring_count(AIC1160State *s)
{
    uint16_t neg = lduw_le_p(&s->scratch[0][AIC1160_SCRATCH_DONE_NEG]);
    unsigned count = (uint16_t)-neg;

    return count > AIC1160_MAX_TCBS ? 0 : count;
}

static void aic1160_cancel_all(AIC1160State *s);

/* The chip reset: banks, sequencer and internal state to power-on. */
static void aic1160_chip_reset(AIC1160State *s)
{
    trace_aic1160_chip_reset();
    aic1160_cancel_all(s);
    s->seq_ctl = 0;
    s->seq_addr = 0;
    s->host_ctl = 0;
    s->int_status = 0;
    s->doorbell = 0;
    s->bank = 0;
    s->scratch_page = 0;
    s->tcb_consumer = 0;
    s->done_index = 0;
    s->done_lap = 0;
    s->words_loaded = 0;
    memset(s->bank_reg, 0, sizeof(s->bank_reg));
    s->bank_reg[AIC1160_BANK_DMA][AIC1160_REG_LINK_SPEED] =
        AIC1160_LINK_SPEED_FULL;
    memset(s->seq_ram, 0, sizeof(s->seq_ram));
    memset(s->scratch, 0, sizeof(s->scratch));
    memset(s->tcb_ram, 0, sizeof(s->tcb_ram));
    s->tcb_index = 0;
    s->fw_words = 0;
    s->ring_base = 0;
    s->ring_seen = 0;
    s->ring_ptr = 0;
    s->sg_ptr = 0;
    s->cmd_ptr = 0;
    aic1160_update_irq(s);
}

static void aic1160_seeprom_build(AIC1160State *s)
{
    memset(s->seeprom, 0xff, sizeof(s->seeprom));
    s->seeprom[0] = AIC1160_SEEPROM_CONTROL;
    s->seeprom[2] = AIC1160_SEEPROM_MEDIA_COPPER;
    /* The name, most significant word first. */
    for (int i = 0; i < 4; i++) {
        s->seeprom[AIC1160_SEEPROM_WWN_WORD + i] = s->wwn >> (48 - 16 * i);
    }
}

static void aic1160_seeprom_command(AIC1160State *s, uint8_t cmd)
{
    uint8_t *bank0 = s->bank_reg[AIC1160_BANK_SEEPROM];
    uint8_t addr = bank0[AIC1160_REG_SEEPROM_ADDR];

    if (cmd != AIC1160_SEEPROM_OP_READ) {
        qemu_log_mask(LOG_UNIMP,
                      "aic1160: SEEPROM opcode 0x%02x is not modelled; "
                      "only the read opcode 0x61 is\n", cmd);
    } else {
        stw_le_p(&bank0[AIC1160_REG_SEEPROM_DATA], s->seeprom[addr]);
    }
    /* The access is done by the time the driver looks at the busy bit. */
    bank0[AIC1160_REG_SEEPROM_CMD] = cmd & ~AIC1160_SEEPROM_BUSY;
}

/* Which register cells in the visible bank only store what is written. */
static bool aic1160_stores(AIC1160State *s, hwaddr addr)
{
    switch (addr) {
    case 0x28 ... 0x29:
    case 0x40 ... 0x5f:
    case 0x62 ... 0x65:
    case 0x70 ... 0x77:
    case 0x7c:
    case 0x98 ... 0x99:
    case 0xa1 ... 0xa3:
    case 0xd0:
    case 0xe1:
    case 0xeb:
        return true;
    case 0x68 ... 0x6b:
        /* Bank 0 is the SEEPROM controller, bank 4 the consumer index. */
        return s->bank != AIC1160_BANK_SEEPROM &&
               !(s->bank == AIC1160_BANK_DMA && addr >= 0x6a);
    default:
        return false;
    }
}

static uint16_t aic1160_seq_data_read(AIC1160State *s)
{
    uint16_t val = 0;

    if (s->seq_addr < AIC1160_SEQ_RAM_WORDS) {
        val = s->seq_ram[s->seq_addr];
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: sequencer RAM read at word 0x%04x is past "
                      "the RAM\n", s->seq_addr);
    }
    s->seq_addr++;
    return val;
}

static void aic1160_seq_data_write(AIC1160State *s, uint16_t val)
{
    if (s->seq_addr < AIC1160_SEQ_RAM_WORDS) {
        s->seq_ram[s->seq_addr] = val;
        s->words_loaded++;
        /* An image starts at word 0; its length is how far it got. */
        if (s->seq_addr == 0) {
            s->fw_words = 0;
        }
        s->fw_words = MAX(s->fw_words, s->seq_addr + 1);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: sequencer RAM write at word 0x%04x is past "
                      "the RAM\n", s->seq_addr);
    }
    s->seq_addr++;
}

static void aic1160_seq_ctl_write(AIC1160State *s, uint8_t val)
{
    uint8_t known = AIC1160_SEQ_CTL_LOAD | AIC1160_SEQ_CTL_RUN |
                    AIC1160_SEQ_CTL_RESTART;

    if (val & ~known) {
        qemu_log_mask(LOG_UNIMP,
                      "aic1160: sequencer control bits 0x%02x have no "
                      "described meaning\n", val & ~known);
    }
    if ((val & AIC1160_SEQ_CTL_RUN) && !aic1160_running(s)) {
        trace_aic1160_sequencer_run(s->words_loaded);
    }
    s->seq_ctl = val;
    if (val & AIC1160_SEQ_CTL_LOAD) {
        s->words_loaded = 0;
    }
}

static void aic1160_bank_write(AIC1160State *s, uint8_t val)
{
    uint8_t bank = val >> 4;

    if ((val & 0xf) != bank || bank >= AIC1160_BANKS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: bank select 0x%02x is not 0x00 to 0x77 in "
                      "steps of 0x11\n", val);
        return;
    }
    s->bank = bank;
}

static void aic1160_loop_init(AIC1160State *s);

static void aic1160_host_ctl_write(AIC1160State *s, uint8_t val)
{
    uint8_t paused = s->host_ctl & (AIC1160_HOST_CTL_PAUSE |
                                    AIC1160_HOST_CTL_PAUSE_TOP);

    if (val & AIC1160_HOST_CTL_RESET) {
        aic1160_chip_reset(s);
        return;
    }
    /* Pause and pause-at-top acknowledge at once, so they read back. */
    s->host_ctl = val & ~AIC1160_HOST_CTL_FC_RESET;
    aic1160_update_irq(s);
    /*
     * Unpausing the sequencer with the loop image running starts loop
     * initialisation.
     */
    if (paused && !(val & (AIC1160_HOST_CTL_PAUSE |
                           AIC1160_HOST_CTL_PAUSE_TOP)) &&
        aic1160_running(s) && s->fw_words >= AIC1160_LOOP_IMAGE_MIN &&
        s->fw_words <= AIC1160_LOOP_IMAGE_MAX) {
        aic1160_loop_init(s);
    }
}

static void aic1160_process_ring(AIC1160State *s);

static uint8_t aic1160_read8(AIC1160State *s, hwaddr addr)
{
    uint8_t *bank = s->bank_reg[s->bank];

    switch (addr) {
    case AIC1160_REG_SEQ_CTL:
        return s->seq_ctl;
    case AIC1160_REG_SEQ_ADDR:
    case AIC1160_REG_SEQ_ADDR + 1:
        return extract16(s->seq_addr, (addr & 1) * 8, 8);
    case AIC1160_REG_HOST_CTL:
        return s->host_ctl;
    case AIC1160_REG_ERROR_STATUS:
        /* Nothing raises a hardware error; reading would clear it. */
        return 0;
    case AIC1160_REG_INT_STATUS:
    case AIC1160_REG_INT_STATUS + 1:
        return extract16(s->int_status, (addr & 1) * 8, 8);
    case AIC1160_REG_DOORBELL:
    case AIC1160_REG_DOORBELL + 1:
        return extract16(s->doorbell, (addr & 1) * 8, 8);
    case AIC1160_REG_BANK:
        return s->bank * 0x11;
    case AIC1160_REG_SCRATCH_PAGE:
        return s->scratch_page;
    case AIC1160_REG_SEQ_DATA:
    case AIC1160_REG_SEQ_DATA + 1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: byte read of the sequencer data port; the "
                      "driver uses 16-bit accesses\n");
        return 0;
    default:
        break;
    }
    if (s->bank == AIC1160_BANK_DMA && (addr == AIC1160_REG_TCB_CONSUMER ||
                                        addr == AIC1160_REG_TCB_CONSUMER + 1)) {
        return extract16(s->tcb_consumer, (addr & 1) * 8, 8);
    }
    if (addr >= AIC1160_REG_GLOBAL_CELLS &&
        addr < AIC1160_REG_GLOBAL_CELLS + AIC1160_GLOBAL_CELLS) {
        return s->global_cells[addr - AIC1160_REG_GLOBAL_CELLS];
    }
    if (s->bank == AIC1160_BANK_SG &&
        (addr == AIC1160_REG_SG_DATA || addr == AIC1160_REG_SG_DATA + 1)) {
        uint8_t *area = s->sg_ram[lduw_le_p(&bank[AIC1160_REG_TCB_INDEX]) %
                                  AIC1160_MAX_TCBS];

        return area[s->sg_ptr++ % AIC1160_SG_AREA_SIZE];
    }
    if (s->bank == AIC1160_BANK_DMA &&
        (addr == AIC1160_REG_CMD_DATA || addr == AIC1160_REG_CMD_DATA + 1)) {
        return s->cmd_buf[s->cmd_ptr++ % AIC1160_CMD_BUF_SIZE];
    }
    if ((s->bank == AIC1160_BANK_SEEPROM &&
         addr >= AIC1160_REG_SEEPROM_DATA &&
         addr <= AIC1160_REG_SEEPROM_CMD) || aic1160_stores(s, addr)) {
        return bank[addr];
    }
    qemu_log_mask(LOG_UNIMP,
                  "aic1160: read of undescribed register 0x%02" HWADDR_PRIx
                  " in bank %u\n", addr, s->bank);
    return 0;
}

static void aic1160_write8(AIC1160State *s, hwaddr addr, uint8_t val)
{
    uint8_t *bank = s->bank_reg[s->bank];

    switch (addr) {
    case AIC1160_REG_SEQ_CTL:
        aic1160_seq_ctl_write(s, val);
        return;
    case AIC1160_REG_SEQ_ADDR:
    case AIC1160_REG_SEQ_ADDR + 1:
        s->seq_addr = deposit32(s->seq_addr, (addr & 1) * 8, 8, val);
        return;
    case AIC1160_REG_HOST_CTL:
        aic1160_host_ctl_write(s, val);
        return;
    case AIC1160_REG_INT_STATUS:
    case AIC1160_REG_INT_STATUS + 1:
        /* The driver clears what it handled by writing it back. */
        s->int_status &= ~((uint16_t)val << ((addr & 1) * 8));
        aic1160_update_irq(s);
        return;
    case AIC1160_REG_DOORBELL:
    case AIC1160_REG_DOORBELL + 1:
        s->doorbell = deposit32(s->doorbell, (addr & 1) * 8, 8, val);
        return;
    case AIC1160_REG_BANK:
        aic1160_bank_write(s, val);
        return;
    case AIC1160_REG_SCRATCH_PAGE:
        s->scratch_page = val;
        return;
    case AIC1160_REG_SEQ_DATA:
    case AIC1160_REG_SEQ_DATA + 1:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: byte write to the sequencer data port; the "
                      "driver uses 16-bit accesses\n");
        return;
    default:
        break;
    }
    if (s->bank == AIC1160_BANK_DMA && (addr == AIC1160_REG_TCB_CONSUMER ||
                                        addr == AIC1160_REG_TCB_CONSUMER + 1)) {
        s->tcb_consumer = deposit32(s->tcb_consumer, (addr & 1) * 8, 8, val);
        return;
    }
    if (addr >= AIC1160_REG_GLOBAL_CELLS &&
        addr < AIC1160_REG_GLOBAL_CELLS + AIC1160_GLOBAL_CELLS) {
        s->global_cells[addr - AIC1160_REG_GLOBAL_CELLS] = val;
        return;
    }
    if (s->bank == AIC1160_BANK_SG &&
        (addr == AIC1160_REG_SG_DATA || addr == AIC1160_REG_SG_DATA + 1)) {
        uint8_t *area = s->sg_ram[lduw_le_p(&bank[AIC1160_REG_TCB_INDEX]) %
                                  AIC1160_MAX_TCBS];

        area[s->sg_ptr++ % AIC1160_SG_AREA_SIZE] = val;
        return;
    }
    if (s->bank == AIC1160_BANK_SG && addr == AIC1160_REG_SG_REWIND) {
        s->sg_ptr = val;
        return;
    }
    if (s->bank == AIC1160_BANK_DMA &&
        (addr == AIC1160_REG_CMD_DATA || addr == AIC1160_REG_CMD_DATA + 1)) {
        s->cmd_buf[s->cmd_ptr++ % AIC1160_CMD_BUF_SIZE] = val;
        return;
    }
    if (s->bank == AIC1160_BANK_DMA && addr == AIC1160_REG_CMD_ADDR) {
        s->cmd_ptr = val;
        return;
    }
    if (s->bank == AIC1160_BANK_SEEPROM &&
        addr >= AIC1160_REG_SEEPROM_DATA && addr <= AIC1160_REG_SEEPROM_CMD) {
        /* The read-data cells are the controller's to fill. */
        if (addr == AIC1160_REG_SEEPROM_ADDR) {
            bank[addr] = val;
        } else if (addr == AIC1160_REG_SEEPROM_CMD) {
            bank[addr] = val;
            aic1160_seeprom_command(s, val);
        }
        return;
    }
    if (aic1160_stores(s, addr)) {
        bank[addr] = val;
        if (addr == AIC1160_REG_TCB_INDEX ||
            addr == AIC1160_REG_TCB_INDEX + 1) {
            /* Every block's pointer aims at the same TCB. */
            s->tcb_index = lduw_le_p(&bank[AIC1160_REG_TCB_INDEX]);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "aic1160: write of 0x%02x to undescribed register 0x%02"
                  HWADDR_PRIx " in bank %u\n", val, addr, s->bank);
}

static uint64_t aic1160_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AIC1160State *s = opaque;
    uint64_t val;

    if (size == 2 && addr == AIC1160_REG_SEQ_DATA) {
        val = aic1160_seq_data_read(s);
    } else if (size == 2) {
        val = aic1160_read8(s, addr) | (aic1160_read8(s, addr + 1) << 8);
    } else {
        val = aic1160_read8(s, addr);
    }
    trace_aic1160_reg_read(addr, size, val);
    return val;
}

static void aic1160_reg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    AIC1160State *s = opaque;

    trace_aic1160_reg_write(addr, size, val);
    if (size == 2 && addr == AIC1160_REG_SEQ_DATA) {
        aic1160_seq_data_write(s, val);
    } else if (size == 2 && addr == AIC1160_REG_DOORBELL) {
        s->doorbell = val;
        aic1160_process_ring(s);
    } else if (size == 2) {
        aic1160_write8(s, addr, val & 0xff);
        aic1160_write8(s, addr + 1, val >> 8);
    } else {
        aic1160_write8(s, addr, val);
    }
}

/*
 * The driver uses byte and halfword accesses only; whether the chip takes
 * a word access is unknown, so the memory core refuses one.
 */
static const MemoryRegionOps aic1160_reg_ops = {
    .read = aic1160_reg_read,
    .write = aic1160_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* The sequencer memory window: the paged scratch RAM and more RAM. */
static uint8_t *aic1160_seq_mem_byte(AIC1160State *s, hwaddr addr)
{
    if (addr < AIC1160_SCRATCH_PAGE_SIZE) {
        return &s->scratch[s->scratch_page % AIC1160_SCRATCH_PAGES][addr];
    }
    return &s->tcb_ram[s->tcb_index % AIC1160_MAX_TCBS]
                      [addr - AIC1160_TCB_RAM_OFFSET];
}

static uint64_t aic1160_seq_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    AIC1160State *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        val |= (uint64_t)*aic1160_seq_mem_byte(s, addr + i) << (8 * i);
    }
    return val;
}

static void aic1160_seq_mem_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    AIC1160State *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        *aic1160_seq_mem_byte(s, addr + i) = val >> (8 * i);
    }
    /* The done queue restarts when its pass byte is set up afresh. */
    for (unsigned i = 0; i < size; i++) {
        if (s->scratch_page % AIC1160_SCRATCH_PAGES == 0 &&
            addr + i == AIC1160_SCRATCH_PASS &&
            ((val >> (8 * i)) & 0xff) == 0xff) {
            s->done_index = 0;
            s->done_lap = 0;
        }
    }
    /* ... and when its size is written. */
    if (s->scratch_page % AIC1160_SCRATCH_PAGES == 0 &&
        addr <= AIC1160_SCRATCH_DONE_NEG + 1 &&
        addr + size > AIC1160_SCRATCH_DONE_NEG) {
        s->done_index = 0;
        s->done_lap = 0;
    }
}

static const MemoryRegionOps aic1160_seq_mem_ops = {
    .read = aic1160_seq_mem_read,
    .write = aic1160_seq_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/* ---- Command processing ---- */

static void aic1160_free_request(AIC1160Request *req)
{
    QTAILQ_REMOVE(&req->dev->requests, req, next);
    if (req->sreq) {
        req->sreq->hba_private = NULL;
        scsi_req_unref(req->sreq);
        req->sreq = NULL;
    }
    qemu_sglist_destroy(&req->qsg);
    g_free(req->desc_len);
    g_free(req);
}

/* One completion into the done queue, then the interrupt. */
static void aic1160_post_done(AIC1160State *s, uint16_t handle,
                              uint8_t status)
{
    uint8_t *scratch = s->scratch[0];
    unsigned count = aic1160_ring_count(s);
    uint64_t base = ldq_le_p(scratch + AIC1160_SCRATCH_DONE_BASE);
    uint8_t entry[AIC1160_DONE_ENTRY_SIZE];

    if (!count || !base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: completion of handle 0x%04x with no done "
                      "queue set up\n", handle);
        return;
    }
    stw_le_p(entry, handle);
    entry[2] = status;
    entry[3] = s->done_lap;
    if (aic1160_dma_write(s, base + s->done_index * AIC1160_DONE_ENTRY_SIZE,
                          entry, sizeof(entry)) != MEMTX_OK) {
        return;
    }
    trace_aic1160_done(handle, status, s->done_index, s->done_lap);
    if (++s->done_index >= count) {
        s->done_index = 0;
        s->done_lap++;
    }
    aic1160_raise(s, AIC1160_INT_COMPLETE);
}

/*
 * The FCP response of a command that ended with a SCSI status other than
 * GOOD: the status flags, the SCSI status, the residual and the sense data.
 */
static void aic1160_write_response(AIC1160State *s, uint64_t rsp_addr,
                                   uint8_t scsi_status, uint32_t resid,
                                   bool under, const uint8_t *sense,
                                   int sense_len)
{
    uint8_t rsp[AIC1160_RSP_RESID_INFO] = { 0 };

    if (resid) {
        rsp[0x0a] |= under ? AIC1160_RSP_RESID_UNDER : AIC1160_RSP_RESID_OVER;
    }
    rsp[0x0b] = scsi_status;
    stl_be_p(rsp + 0x0c, resid);
    if (sense_len > 0) {
        sense_len = MIN(sense_len, AIC1160_RSP_RESID_INFO - 0x18);
        rsp[0x0a] |= AIC1160_RSP_SENSE_VALID;
        stl_be_p(rsp + 0x10, sense_len);
        memcpy(rsp + 0x18, sense, sense_len);
    }
    aic1160_dma_write(s, rsp_addr, rsp, sizeof(rsp));
}

/*
 * The adapter residual-info block: the bytes left in the descriptor the
 * transfer stopped in, and where the driver is to count on from. For one
 * buffer there is nothing to walk and the pointer is zero. For a list the
 * driver takes the descriptor before element k, k = (P - 0x80 - L) / 16 + i,
 * so with the stopped descriptor d the pointer is the list address plus 0x80
 * for each page of eight descriptors passed, plus 0x80, and the index
 * nibble is the next descriptor within its page.
 */
static void aic1160_write_resid_info(AIC1160State *s, AIC1160Request *req)
{
    uint8_t info[16] = { 0 };
    uint32_t left = 0, index = 0;
    uint64_t done = 0;

    for (; index < req->ndesc; index++) {
        if (done + req->desc_len[index] > req->transferred) {
            left = done + req->desc_len[index] - req->transferred;
            break;
        }
        done += req->desc_len[index];
    }
    if (req->desc_list) {
        uint32_t next = index + 1;

        stq_le_p(info, req->desc_addr + 0x80 * (next / 8) + 0x80);
        info[12] = (next % 8) << 4;
    }
    info[8] = left;
    info[9] = left >> 8;
    info[10] = left >> 16;
    aic1160_dma_write(s, req->rsp_addr + AIC1160_RSP_RESID_INFO, info,
                      sizeof(info));
}

static void aic1160_scsi_complete(SCSIRequest *sreq, size_t resid)
{
    AIC1160Request *req = sreq->hba_private;
    AIC1160State *s = req->dev;
    uint8_t sense[SCSI_SENSE_BUF_SIZE];
    int sense_len = 0;
    uint32_t unused = 0;
    uint16_t handle = req->handle;
    uint8_t status = AIC1160_DONE_OK;
    bool under;

    sgi_scsi_cd_command_done(sreq);
    if (req->dropping) {
        aic1160_free_request(req);
        return;
    }
    under = req->fcp_dl >= req->transferred;
    unused = under ? req->fcp_dl - req->transferred :
                     req->transferred - req->fcp_dl;
    /*
     * A target that moves a different number of bytes than the FCP data
     * length answers with an FCP_RSP carrying the residual. The driver logs
     * the bare underrun and overrun codes as errors, and a short INQUIRY is
     * routine, so both go the error response way, with GOOD status.
     */
    if (sreq->status != GOOD || unused) {
        if (sreq->status != GOOD) {
            sense_len = scsi_req_get_sense(sreq, sense, sizeof(sense));
            if (sense_len < 0) {
                sense_len = 0;
            }
        }
        aic1160_write_response(s, req->rsp_addr, sreq->status & 0xff, unused,
                               under, sense, sense_len);
        status = AIC1160_DONE_ERROR_RSP;
    }
    trace_aic1160_residual(handle, req->fcp_dl, req->transferred,
                           req->described, req->ndesc);
    if (unused) {
        aic1160_write_resid_info(s, req);
    }
    aic1160_free_request(req);
    aic1160_post_done(s, handle, status);
}

static void aic1160_scsi_cancel(SCSIRequest *sreq)
{
    AIC1160Request *req = sreq->hba_private;

    if (req) {
        aic1160_free_request(req);
    }
}

static void aic1160_scsi_transfer_data(SCSIRequest *sreq, uint32_t len)
{
    AIC1160Request *req = sreq->hba_private;
    AIC1160State *s = req->dev;
    uint8_t *buf = scsi_req_get_buf(sreq);
    uint32_t offset = 0;

    if (req->dropping) {
        return;
    }
    while (offset < len && req->sg_index < req->qsg.nsg) {
        ScatterGatherEntry *sg = &req->qsg.sg[req->sg_index];
        uint32_t count = MIN(len - offset, sg->len - req->sg_offset);
        dma_addr_t addr = sg->base + req->sg_offset;
        MemTxResult result;

        if (sreq->cmd.mode == SCSI_XFER_FROM_DEV) {
            result = aic1160_dma_write(s, addr, buf + offset, count);
        } else {
            result = aic1160_dma_read(s, addr, buf + offset, count);
        }
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "aic1160: data transfer at 0x%" PRIx64 " for "
                          "handle 0x%04x refused\n", (uint64_t)addr,
                          req->handle);
            scsi_req_cancel(sreq);
            return;
        }
        req->transferred += count;
        req->sg_offset += count;
        offset += count;
        if (req->sg_offset == sg->len) {
            req->sg_index++;
            req->sg_offset = 0;
        }
    }
    scsi_req_continue(sreq);
}

static const struct SCSIBusInfo aic1160_scsi_info = {
    .tcq = true,
    .max_channel = 0,
    .max_target = AIC1160_MAX_LOOP_ID,
    .max_lun = AIC1160_MAX_LUN,
    .transfer_data = aic1160_scsi_transfer_data,
    .complete = aic1160_scsi_complete,
    .cancel = aic1160_scsi_cancel,
};

/* Every request in flight cancelled with nothing posted. */
static void aic1160_cancel_all(AIC1160State *s)
{
    AIC1160Request *req;

    QTAILQ_FOREACH(req, &s->requests, next) {
        req->dropping = true;
    }
    while ((req = QTAILQ_FIRST(&s->requests)) != NULL) {
        if (!req->sreq->enqueued) {
            aic1160_free_request(req);
            continue;
        }
        scsi_req_cancel(req->sreq);
    }
    sgi_scsi_cd_adapter_reset(DEVICE(s));
}

static int aic1160_loop_id(uint8_t alpa)
{
    for (int i = 0; i < ARRAY_SIZE(aic1160_alpa); i++) {
        if (aic1160_alpa[i] == alpa) {
            return i;
        }
    }
    return -1;
}

/*
 * The data descriptors: one buffer in the TCB, or a list of 16-byte
 * entries, each an address, a length and four reserved bytes. The driver
 * leaves the TCB's count at 0 for a list: the last entry is the one whose
 * length has a top byte of 1.
 */
static bool aic1160_map_data(AIC1160State *s, AIC1160Request *req,
                             const uint8_t *tcb, uint64_t max)
{
    uint64_t ptr = ldq_le_p(tcb + 0x4c);
    uint32_t len = ldl_le_p(tcb + 0x54);
    bool last = false;

    if (tcb[0x23] == AIC1160_TCB_MODE_NONE) {
        return true;
    }
    req->desc_addr = ptr;
    req->desc_list = tcb[0x23] == AIC1160_TCB_MODE_SG;
    for (uint32_t i = 0; !last && i < AIC1160_SG_MAX; i++) {
        uint64_t base = ptr, room, placed;
        uint32_t seg = len;

        if (req->desc_list) {
            uint8_t entry[AIC1160_SG_STRIDE];

            if (aic1160_dma_read(s, ptr + (uint64_t)i * AIC1160_SG_STRIDE,
                                 entry, sizeof(entry)) != MEMTX_OK) {
                return false;
            }
            base = ldq_le_p(entry);
            seg = ldl_le_p(entry + 8) & AIC1160_SG_LEN_MASK;
            last = entry[11] == AIC1160_SG_LAST;
        } else {
            last = true;
        }
        room = req->described < max ? max - req->described : 0;
        placed = MIN(seg, room);
        req->described += seg;
        req->desc_len = g_renew(uint32_t, req->desc_len, req->ndesc + 1);
        req->desc_len[req->ndesc++] = seg;
        if (placed) {
            qemu_sglist_add(&req->qsg, base, placed);
        }
    }
    return last;
}

static void aic1160_submit_scsi(AIC1160State *s, const uint8_t *tcb)
{
    uint16_t handle = lduw_le_p(tcb + AIC1160_TCB_HANDLE);
    int loop_id = aic1160_loop_id(tcb[0x04]);
    unsigned lun = ((tcb[0x24] & 0x3f) << 8) | tcb[0x25];
    uint64_t rsp_addr = ldq_le_p(tcb + 0x58);
    uint8_t cdb[16];
    SCSIDevice *sdev = NULL;
    AIC1160Request *req;

    if (loop_id >= 0 && lun <= AIC1160_MAX_LUN) {
        sdev = scsi_device_find(&s->bus, 0, loop_id, lun);
    }
    if (!sdev) {
        aic1160_post_done(s, handle, AIC1160_DONE_NO_DEVICE);
        return;
    }

    memcpy(cdb, tcb + 0x30, sizeof(cdb));
    req = g_new0(AIC1160Request, 1);
    req->dev = s;
    req->handle = handle;
    req->fcp_dl = ldl_be_p(tcb + 0x40);
    req->rsp_addr = rsp_addr;
    pci_dma_sglist_init(&req->qsg, PCI_DEVICE(s), 2);
    QTAILQ_INSERT_TAIL(&s->requests, req, next);
    req->sreq = scsi_req_new(sdev, handle, lun, cdb, sizeof(cdb), req);
    if (!aic1160_map_data(s, req, tcb, req->sreq->cmd.xfer)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: descriptor list for handle 0x%04x could not "
                      "be read\n", handle);
        aic1160_free_request(req);
        aic1160_post_done(s, handle, AIC1160_DONE_FORMAT);
        return;
    }
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
}

/* The device at a loop ID, any LUN of it: the port. */
static SCSIDevice *aic1160_port(AIC1160State *s, unsigned loop_id)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &s->bus.qbus.children, sibling) {
        SCSIDevice *sdev = SCSI_DEVICE(kid->child);

        if (sdev->id == loop_id && qdev_is_realized(DEVICE(sdev))) {
            return sdev;
        }
    }
    return NULL;
}

/*
 * The accept a port gives to a login: LS_ACC, the service parameters and
 * the port's names. Only the names and the AL_PA come from the device; the
 * rest of the parameters are plausible values, not the target's.
 */
static unsigned aic1160_els_accept(SCSIDevice *sdev, uint8_t code,
                                   uint8_t *b)
{
    uint8_t alpa = aic1160_alpa[sdev->id];

    memset(b, 0, 128);
    b[0] = 0x02;
    switch (code) {
    case AIC1160_ELS_PLOGI:
        b[4] = 0x20;                /* FC-PH version, high and low */
        b[5] = 0x20;
        stw_be_p(b + 10, 2048);     /* BB receive data field size */
        stw_be_p(b + 12, 0xff);     /* Concurrent sequences */
        stl_be_p(b + 16, 2000);     /* E_D_TOV, ms */
        stq_be_p(b + 20, sdev->port_wwn);
        stq_be_p(b + 28, sdev->wwn);
        b[68] = 0x80;               /* Class 3 valid */
        stw_be_p(b + 76, 2048);
        stw_be_p(b + 80, 0xff);
        return 116;
    case AIC1160_ELS_PRLI:
        b[1] = 0x10;                /* Page length */
        stw_be_p(b + 2, 20);
        b[4] = 0x08;                /* FCP */
        stw_be_p(b + 6, 0x2100);    /* Image pair established, accepted */
        stl_be_p(b + 16, 0x12);     /* Target function */
        return 20;
    case AIC1160_ELS_ADISC:
        b[7] = alpa;                /* Hard address */
        stq_be_p(b + 8, sdev->port_wwn);
        stq_be_p(b + 16, sdev->wwn);
        b[27] = alpa;               /* N_Port ID */
        return 28;
    default:
        /* LS_RJT, invalid command code. */
        b[0] = 0x01;
        b[5] = 0x01;
        return 8;
    }
}

/* PLOGI, PRLI and ADISC: the accept is DMA'd into the response buffer. */
static void aic1160_submit_els(AIC1160State *s, const uint8_t *tcb)
{
    uint16_t handle = lduw_le_p(tcb + AIC1160_TCB_HANDLE);
    int loop_id = aic1160_loop_id(tcb[0x04]);
    uint64_t payload = ldq_le_p(tcb + 0x4c);
    uint32_t len = ldl_le_p(tcb + 0x54);
    uint64_t rsp_addr = ldq_le_p(tcb + 0x58);
    SCSIDevice *sdev = loop_id >= 0 ? aic1160_port(s, loop_id) : NULL;
    uint8_t code, accept[128];
    unsigned size;

    if (!sdev) {
        aic1160_post_done(s, handle, AIC1160_DONE_NO_DEVICE);
        return;
    }
    if (len < 4 || aic1160_dma_read(s, payload, &code, 1) != MEMTX_OK) {
        aic1160_post_done(s, handle, AIC1160_DONE_FORMAT);
        return;
    }
    size = aic1160_els_accept(sdev, code, accept);
    if (aic1160_dma_write(s, rsp_addr, accept, size) != MEMTX_OK) {
        aic1160_post_done(s, handle, AIC1160_DONE_FORMAT);
        return;
    }
    aic1160_post_done(s, handle, AIC1160_DONE_OK);
}

/*
 * Loop initialisation. The driver loads the LIP TCB straight into TCB RAM,
 * the loop image into the sequencer and runs it; there is no wire for the
 * model to walk, so it reports the loop as it is. It DMAs the position map
 * in the count-list form to the buffer the TCB names, then posts one done
 * entry for the TCB with the host's negotiated AL_PA as its status.
 */
static void aic1160_loop_init(AIC1160State *s)
{
    unsigned n = s->tcb_index % AIC1160_MAX_TCBS;
    uint64_t map_addr = ldq_le_p(&s->tcb_ram[n][AIC1160_LIP_MAP_ADDR]);
    uint8_t map[AIC1160_LIP_MAP_SIZE] = { 0 };
    unsigned count = 0;

    trace_aic1160_loop_init(n, map_addr);
    /* In loop order: the lowest loop ID, the highest priority AL_PA, first. */
    for (unsigned id = 0; id <= AIC1160_MAX_LOOP_ID; id++) {
        if (id != AIC1160_HOST_LOOP_ID && aic1160_port(s, id)) {
            map[1 + count++] = aic1160_alpa[id];
        }
    }
    map[0] = count;
    if (map_addr) {
        aic1160_dma_write(s, map_addr, map, sizeof(map));
    }
    aic1160_post_done(s, n, aic1160_alpa[AIC1160_HOST_LOOP_ID]);
}

static void aic1160_process_ring(AIC1160State *s)
{
    unsigned count = aic1160_ring_count(s);
    uint64_t ptr = ldq_le_p(s->scratch[0] + AIC1160_SCRATCH_TCB_BASE);
    uint64_t base;
    unsigned prod, budget;

    if (!aic1160_running(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: doorbell 0x%04x with the sequencer not "
                      "running\n", s->doorbell);
        return;
    }
    /*
     * Scratch holds the pointer to the next TCB. The driver sets it to the
     * ring's start, and puts it back to the next free entry after a loop
     * initialisation, restarting its indices at 0 from there: a new value
     * inside the ring is a position in it, any other a new ring. The chip
     * advances its own pointer, round the ring's end.
     */
    if (count && ptr != s->ring_seen) {
        uint64_t end = s->ring_base + (uint64_t)count * AIC1160_TCB_SIZE;

        s->ring_seen = ptr;
        if (!(s->ring_base && ptr >= s->ring_base && ptr < end &&
              !((ptr - s->ring_base) % AIC1160_TCB_SIZE))) {
            s->ring_base = ptr;
        }
        s->ring_ptr = ptr;
        s->tcb_consumer = 0;
    }
    base = s->ring_base;
    if (!count || !base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "aic1160: doorbell 0x%04x with no TCB ring set up\n",
                      s->doorbell);
        return;
    }
    prod = s->doorbell % count;
    for (budget = count; s->tcb_consumer % count != prod && budget; budget--) {
        uint8_t tcb[AIC1160_TCB_SIZE];
        unsigned slot = (s->ring_ptr - base) / AIC1160_TCB_SIZE;

        if (aic1160_dma_read(s, s->ring_ptr, tcb, sizeof(tcb)) != MEMTX_OK) {
            return;
        }
        s->tcb_consumer = (s->tcb_consumer + 1) % count;
        s->ring_ptr += AIC1160_TCB_SIZE;
        if (s->ring_ptr >= base + (uint64_t)count * AIC1160_TCB_SIZE) {
            s->ring_ptr = base;
        }
        if (trace_event_get_state_backends(TRACE_AIC1160_TCB_DUMP)) {
            g_autofree char *hex = g_malloc(sizeof(tcb) * 2 + 1);

            for (unsigned b = 0; b < sizeof(tcb); b++) {
                sprintf(hex + 2 * b, "%02x", tcb[b]);
            }
            trace_aic1160_tcb_dump(slot, hex);
        }
        trace_aic1160_tcb(slot, lduw_le_p(tcb), tcb[0x04],
                          tcb[AIC1160_TCB_R_CTL], tcb[AIC1160_TCB_F_CTL2],
                          tcb[AIC1160_TCB_TYPE], tcb[AIC1160_TCB_CONTROL],
                          tcb[0x23]);
        if (tcb[AIC1160_TCB_CONTROL] == AIC1160_CONTROL_RECEIVE) {
            /*
             * An unsolicited-receive buffer: the chip keeps it until a
             * frame arrives, and the model has no frame to give it.
             */
            trace_aic1160_receive_buffer(slot,
                                         lduw_le_p(tcb + AIC1160_TCB_HANDLE));
        } else if (tcb[AIC1160_TCB_CONTROL] == AIC1160_CONTROL_ELS &&
            tcb[AIC1160_TCB_TYPE] == AIC1160_TYPE_ELS &&
            tcb[AIC1160_TCB_R_CTL] == AIC1160_R_CTL_ELS) {
            aic1160_submit_els(s, tcb);
        } else if (tcb[0x23] == AIC1160_TCB_MODE_FLAT ||
                   tcb[0x23] == AIC1160_TCB_MODE_SG ||
                   tcb[0x23] == AIC1160_TCB_MODE_NONE) {
            aic1160_submit_scsi(s, tcb);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "aic1160: TCB 0x%04x is not a SCSI command, a login "
                          "or a receive buffer\n",
                          lduw_le_p(tcb + AIC1160_TCB_HANDLE));
            aic1160_post_done(s, lduw_le_p(tcb + AIC1160_TCB_HANDLE),
                              AIC1160_DONE_FORMAT);
        }
    }
}

/* ---- Device ---- */

static void aic1160_reset_hold(Object *obj, ResetType type)
{
    AIC1160State *s = AIC1160(obj);

    aic1160_chip_reset(s);
    aic1160_seeprom_build(s);
}

/*
 * The LIP primitive is requested by a write to a byte of config space,
 * alternately a request and a close; the chip answers with the LIP-ready
 * interrupt each time.
 */
static void aic1160_config_write(PCIDevice *dev, uint32_t addr, uint32_t val,
                                 int len)
{
    AIC1160State *s = AIC1160(dev);

    pci_default_write_config(dev, addr, val, len);
    if (addr <= AIC1160_CFG_LIP_REQUEST &&
        addr + len > AIC1160_CFG_LIP_REQUEST) {
        trace_aic1160_lip_request(dev->config[AIC1160_CFG_LIP_REQUEST]);
        if (dev->config[AIC1160_CFG_LIP_REQUEST]) {
            aic1160_raise(s, AIC1160_INT_LIP_READY);
        }
    }
}

static void aic1160_realize(PCIDevice *dev, Error **errp)
{
    AIC1160State *s = AIC1160(dev);

    dev->config[PCI_REVISION_ID] = s->revision;
    dev->config[PCI_INTERRUPT_PIN] = 0x01;
    /* The driver sets the dual-address-cycle enable here at attach. */
    dev->wmask[AIC1160_CFG_DEVICE] = 0xff;
    dev->wmask[PCI_LATENCY_TIMER] = 0xff;
    dev->wmask[AIC1160_CFG_LIP_REQUEST] = 0xff;

    memory_region_init_io(&s->regs, OBJECT(s), &aic1160_reg_ops, s,
                          "aic1160-regs", AIC1160_WINDOW_SIZE);
    memory_region_init_io(&s->seq_mem, OBJECT(s), &aic1160_seq_mem_ops, s,
                          "aic1160-seq-mem", AIC1160_WINDOW_SIZE);
    pci_register_bar(dev, AIC1160_BAR_REGS, PCI_BASE_ADDRESS_SPACE_IO,
                     &s->regs);
    pci_register_bar(dev, AIC1160_BAR_SEQ_MEM, PCI_BASE_ADDRESS_SPACE_IO,
                     &s->seq_mem);

    QTAILQ_INIT(&s->requests);
    scsi_bus_init_named(&s->bus, sizeof(s->bus), DEVICE(dev),
                        &aic1160_scsi_info, s->bus_name);
    aic1160_seeprom_build(s);
}

static const VMStateDescription vmstate_aic1160 = {
    .name = TYPE_AIC1160,
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, AIC1160State),
        VMSTATE_UINT8(seq_ctl, AIC1160State),
        VMSTATE_UINT16(seq_addr, AIC1160State),
        VMSTATE_UINT16_ARRAY(seq_ram, AIC1160State, AIC1160_SEQ_RAM_WORDS),
        VMSTATE_UINT8(host_ctl, AIC1160State),
        VMSTATE_UINT16(int_status, AIC1160State),
        VMSTATE_UINT16(doorbell, AIC1160State),
        VMSTATE_UINT8(bank, AIC1160State),
        VMSTATE_UINT8(scratch_page, AIC1160State),
        VMSTATE_UINT8_2DARRAY(bank_reg, AIC1160State, AIC1160_BANKS,
                              AIC1160_WINDOW_SIZE),
        VMSTATE_UINT8_2DARRAY(scratch, AIC1160State, AIC1160_SCRATCH_PAGES,
                              AIC1160_SCRATCH_PAGE_SIZE),
        VMSTATE_UINT8_2DARRAY(tcb_ram, AIC1160State, AIC1160_MAX_TCBS,
                              AIC1160_SCRATCH_PAGE_SIZE),
        VMSTATE_UINT16(tcb_index, AIC1160State),
        VMSTATE_UINT16(fw_words, AIC1160State),
        VMSTATE_UINT64(ring_base, AIC1160State),
        VMSTATE_UINT64(ring_seen, AIC1160State),
        VMSTATE_UINT64(ring_ptr, AIC1160State),
        VMSTATE_UINT8_2DARRAY(sg_ram, AIC1160State, AIC1160_MAX_TCBS,
                              AIC1160_SG_AREA_SIZE),
        VMSTATE_UINT8(sg_ptr, AIC1160State),
        VMSTATE_UINT8_ARRAY(global_cells, AIC1160State,
                            AIC1160_GLOBAL_CELLS),
        VMSTATE_UINT8_ARRAY(cmd_buf, AIC1160State, AIC1160_CMD_BUF_SIZE),
        VMSTATE_UINT8(cmd_ptr, AIC1160State),
        VMSTATE_UINT16(tcb_consumer, AIC1160State),
        VMSTATE_UINT16(done_index, AIC1160State),
        VMSTATE_UINT8(done_lap, AIC1160State),
        VMSTATE_UINT32(words_loaded, AIC1160State),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Test hooks, not part of the guest interface. The model has no source of
 * loop events or hardware errors, so a test raises status bits through
 * these; the register reads and write-back clears are the guest's.
 */
static void aic1160_set_raise_status(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    AIC1160State *s = AIC1160(obj);
    uint16_t bits;

    if (visit_type_uint16(v, name, &bits, errp)) {
        aic1160_raise(s, bits);
    }
}

/* A loop-state register bit and the attention interrupt it comes with. */
static void aic1160_set_loop_event(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    AIC1160State *s = AIC1160(obj);
    uint8_t bits;

    if (visit_type_uint8(v, name, &bits, errp)) {
        s->bank_reg[AIC1160_BANK_LOOP][AIC1160_REG_LOOP_STATE] |= bits;
        aic1160_raise(s, AIC1160_INT_LOOP);
    }
}

static void aic1160_get_words_loaded(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    AIC1160State *s = AIC1160(obj);

    visit_type_uint32(v, name, &s->words_loaded, errp);
}

static void aic1160_get_running(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    AIC1160State *s = AIC1160(obj);
    bool running = aic1160_running(s);

    visit_type_bool(v, name, &running, errp);
}

static void aic1160_instance_init(Object *obj)
{
    object_property_add(obj, "x-raise-status", "uint16", NULL,
                        aic1160_set_raise_status, NULL, NULL);
    object_property_add(obj, "x-loop-event", "uint8", NULL,
                        aic1160_set_loop_event, NULL, NULL);
    object_property_add(obj, "x-words-loaded", "uint32",
                        aic1160_get_words_loaded, NULL, NULL, NULL);
    object_property_add(obj, "x-sequencer-running", "bool",
                        aic1160_get_running, NULL, NULL, NULL);
}

static const Property aic1160_properties[] = {
    DEFINE_PROP_UINT8("revision", AIC1160State, revision, 2),
    DEFINE_PROP_UINT64("wwn", AIC1160State, wwn,
                       AIC1160_DEFAULT_WWN),
    DEFINE_PROP_STRING("bus-name", AIC1160State, bus_name),
};

static void aic1160_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = aic1160_realize;
    pc->config_write = aic1160_config_write;
    pc->vendor_id = AIC1160_VENDOR_ID;
    pc->device_id = AIC1160_DEVICE_ID;
    pc->class_id = AIC1160_CLASS_FIBRE_CHANNEL;
    rc->phases.hold = aic1160_reset_hold;
    dc->desc = "Adaptec AIC-1160 Fibre Channel controller";
    dc->vmsd = &vmstate_aic1160;
    device_class_set_props(dc, aic1160_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo aic1160_info = {
    .name = TYPE_AIC1160,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AIC1160State),
    .instance_init = aic1160_instance_init,
    .class_init = aic1160_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void aic1160_register_types(void)
{
    type_register_static(&aic1160_info);
}

type_init(aic1160_register_types)
