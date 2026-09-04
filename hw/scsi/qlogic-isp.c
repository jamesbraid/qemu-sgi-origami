/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QLogic ISP1020 family Fast/Wide SCSI host adapter.
 *
 * The host hands whole SCSI commands to a request ring and collects
 * completions from a response ring; the guest's microcode download is
 * accepted and answered, never executed, so no vendor firmware is present
 * or required. See docs/specs/sgi-qlogic-isp.rst for the register and
 * ring contracts and their driver-sourced provenance.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/queue.h"
#include "hw/pci/pci_device.h"
#include "hw/scsi/scsi.h"
#include "hw/scsi/qlogic-isp.h"
#include "migration/qemu-file-types.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_QLOGIC_ISP1040 "qlogic-isp1040"
typedef struct QLogicISPState QLogicISPState;
typedef struct QLogicISPRequest QLogicISPRequest;
DECLARE_INSTANCE_CHECKER(QLogicISPState, QLOGIC_ISP1040, TYPE_QLOGIC_ISP1040)

#define QLOGIC_ISP_VENDOR_ID    0x1077
#define QLOGIC_ISP_DEVICE_ID    0x1020

#define QLOGIC_ISP_REVISION     0x05

#define QLOGIC_ISP_REG_WINDOW_SIZE  0x100

#define ISP_REG_ID_LOW      0x00    /* RO: chip identity, low half. */
#define ISP_REG_ID_HIGH     0x02    /* RO: chip identity, high half. */
#define ISP_REG_BUS_CFG0    0x04    /* RO: low four bits are a revision. */
#define ISP_REG_BUS_CFG1    0x06    /* RW: FIFO threshold and burst enable. */
#define ISP_REG_BUS_CTRL    0x08    /* RW: soft reset and interrupt enables. */
#define ISP_REG_BUS_STATUS  0x0a    /* RO: which interrupt is pending. */
#define ISP_REG_SEMAPHORE   0x0c    /* RW: the mailbox-result lock. */
#define ISP_REG_NVRAM       0x0e    /* RW: bit-banged serial EEPROM. */
#define ISP_REG_DMA_CMD_CFG   0x20  /* RW: command DMA block, config. */
#define ISP_REG_DMA_CMD_CTRL  0x22  /* WO: command DMA block, control. */
#define ISP_REG_DMA_DATA_CFG  0x40  /* RW: data DMA block, config. */
#define ISP_REG_DMA_DATA_CTRL 0x42  /* WO: data DMA block, control. */
#define ISP_REG_HOST_CMD    0xc0    /* RW: command in, RISC state flags out. */

#define ISP_BUS_CFG0_HWREV      (QLOGIC_ISP_REVISION + 1)

#define ISP_BUS_CFG1_FIFO_MASK      0x0070
#define ISP_BUS_CFG1_BANK_SELECT    0x0008
#define ISP_BUS_CFG1_BURST_ENABLE   0x0004
#define ISP_BUS_CFG1_WRITE_MASK     (ISP_BUS_CFG1_FIFO_MASK | \
                                     ISP_BUS_CFG1_BANK_SELECT | \
                                     ISP_BUS_CFG1_BURST_ENABLE)

/* Stored: the burst enable here is read-modify-written by drivers. */
#define ISP_DMA_CFG_BURST_ENABLE    0x0002
#define ISP_DMA_CFG_WRITE_MASK      ISP_DMA_CFG_BURST_ENABLE

#define ISP_BUS_CTRL_SOFT_RESET     0x0001
#define ISP_BUS_CTRL_INT_ENABLE     0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004
#define ISP_BUS_CTRL_WRITE_MASK     (ISP_BUS_CTRL_INT_ENABLE | \
                                     ISP_BUS_CTRL_RISC_INT_ENABLE)

#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004

#define ISP_SEMAPHORE_LOCK      0x0001
#define ISP_SEMAPHORE_STATUS    0x0002
#define ISP_SEMAPHORE_MASK      (ISP_SEMAPHORE_LOCK | ISP_SEMAPHORE_STATUS)

/*
 * Data-in is never asserted: every EEPROM read fails validation and the
 * guest falls back to built-in defaults, as with an unprogrammed part.
 */
#define ISP_NVRAM_CLOCK      0x0001
#define ISP_NVRAM_CHIP_SEL   0x0002
#define ISP_NVRAM_DATA_OUT   0x0004
#define ISP_NVRAM_DATA_IN    0x0008
#define ISP_NVRAM_WRITE_MASK (ISP_NVRAM_CLOCK | ISP_NVRAM_CHIP_SEL | \
                              ISP_NVRAM_DATA_OUT)

/*
 * Mailboxes 4 and 5 double as the queue pointers once a queue exists: the
 * request side is plain storage, but a response-side read must answer the
 * adapter's own producer count, never the guest's last write. Until a
 * queue is initialised both stay ordinary mailboxes, which the firmware's
 * register test requires.
 */
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX_COUNT       8
#define ISP_REG_MAILBOX_END     (ISP_REG_MAILBOX_BASE + ISP_MAILBOX_COUNT * 2)

/* The queue pointers, at these offsets from the mailbox base. */
#define ISP_MAILBOX_REQUEST_QUEUE   4
#define ISP_MAILBOX_RESPONSE_QUEUE  5
#define ISP_REG_REQUEST_QUEUE   (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE  (ISP_REG_MAILBOX_BASE + 0x0a)

/*
 * The post-reset product-ID signature in mailboxes 1-4; Linux aborts the
 * attach on any mismatch.
 */
#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020
#define ISP_MAILBOX4_SIGNATURE  0x0001

#define ISP_MBOX_CMD_NOP                0x0000
#define ISP_MBOX_CMD_LOAD_RAM           0x0001
#define ISP_MBOX_CMD_EXEC_FIRMWARE      0x0002
#define ISP_MBOX_CMD_DUMP_RAM           0x0003
#define ISP_MBOX_CMD_WRITE_RAM_WORD     0x0004
#define ISP_MBOX_CMD_READ_RAM_WORD      0x0005
#define ISP_MBOX_CMD_MAILBOX_REG_TEST   0x0006
#define ISP_MBOX_CMD_VERIFY_CHECKSUM    0x0007
#define ISP_MBOX_CMD_ABOUT_FIRMWARE     0x0008
#define ISP_MBOX_CMD_INIT_REQ_QUEUE     0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE     0x0011
#define ISP_MBOX_CMD_BUS_RESET          0x0018
#define ISP_MBOX_CMD_GET_FIRMWARE_STATUS 0x001f
#define ISP_MBOX_CMD_INIT_REQ_QUEUE_A64  0x0052
#define ISP_MBOX_CMD_INIT_RSP_QUEUE_A64  0x0053
#define ISP_MBOX_CMD_SET_INITIATOR_ID      0x0030
#define ISP_MBOX_CMD_SET_SELECTION_TIMEOUT 0x0031
#define ISP_MBOX_CMD_SET_RETRY_COUNT       0x0032
#define ISP_MBOX_CMD_SET_TAG_AGE_LIMIT      0x0033
#define ISP_MBOX_CMD_SET_CLOCK_RATE         0x0034
#define ISP_MBOX_CMD_SET_ACTIVE_NEGATION    0x0035
#define ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP   0x0036
#define ISP_MBOX_CMD_SET_PCI_CONTROL        0x0037
#define ISP_MBOX_CMD_GET_TARGET_PARAMETERS  0x0028
#define ISP_MBOX_CMD_SET_TARGET_PARAMETERS  0x0038
#define ISP_MBOX_CMD_SET_DEVICE_QUEUE       0x0039
#define ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS 0x003a
#define ISP_MBOX_CMD_SET_SYSTEM_PARAMETER   0x0045
#define ISP_MBOX_CMD_SET_FIRMWARE_FEATURES  0x004a
#define ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY 0x005a

#define ISP_MBOX_COMPLETE           0x4000
#define ISP_MBOX_INVALID            0x4001
#define ISP_MBOX_TEST_FAILED        0x4003
#define ISP_MBOX_CMD_PARAM_ERROR    0x4006

/*
 * The downloaded microcode, retained but never executed, so that
 * verify-checksum can answer honestly about a corrupt image.
 */
#define ISP_RISC_RAM_WORDS      0x10000

#define ISP_FIRMWARE_STATUS_MAX_REQUESTS 512

#define ISP_HOST_CMD_SHIFT          12
#define ISP_HOST_CMD_NOP            0x0
#define ISP_HOST_CMD_RESET_RISC     0x1
#define ISP_HOST_CMD_PAUSE_RISC     0x2
#define ISP_HOST_CMD_RELEASE_RISC   0x3
#define ISP_HOST_CMD_MASK_PARITY    0x4
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_HOST_INT 0x6
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7
#define ISP_HOST_CMD_BIOS_ENABLE    0x9
#define ISP_HOST_CMD_ENABLE_PARITY  0xa
#define ISP_HOST_CMD_PARITY_ERROR   0xe

#define ISP_RISC_FLAG_HOST_INT      0x0080
#define ISP_RISC_FLAG_RESET         0x0040
#define ISP_RISC_FLAG_PAUSED        0x0020
#define ISP_RISC_FLAG_EXTERNAL      0x0010

/*
 * Bit 3 of bus configuration 1 -- not bit 0, a decoy constant in driver
 * headers -- selects which bank the window at 0x80 presents.
 */
#define ISP_REG_SXP_RISC_BASE   0x80
#define ISP_REG_SXP_RISC_END    0xc0
#define ISP_SXP_RISC_COUNT      \
    ((ISP_REG_SXP_RISC_END - ISP_REG_SXP_RISC_BASE) / 2)

/* Queue base split across mailboxes 2, 3, 6, 7; mailbox 1 is the count. */
#define ISP_MBOX_ADDR_LOW(mb)   ((uint64_t)(mb)[3])
#define ISP_MBOX_ADDR_MID(mb)   ((uint64_t)(mb)[2] << 16)
#define ISP_MBOX_ADDR_HIGH(mb)  ((uint64_t)(mb)[7] << 32)
#define ISP_MBOX_ADDR_TOP(mb)   ((uint64_t)(mb)[6] << 48)

#define ISP_QUEUE_ENTRY_SIZE    64

#define ISP_ENTRY_OFF_TYPE      0
#define ISP_ENTRY_OFF_COUNT     1

#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_CONTINUATION 0x02
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_ENTRY_TYPE_MARKER       0x04

#define ISP_REQ_OFF_HANDLE      4
#define ISP_REQ_OFF_DEVICE      8
#define ISP_REQ_OFF_CDBLEN      10
#define ISP_REQ_OFF_TIMEOUT     16
#define ISP_REQ_OFF_SEGCOUNT    18
#define ISP_REQ_OFF_CDB         20
#define ISP_REQ_CDB_LEN         12
#define ISP_REQ_OFF_SEGS        32
#define ISP_REQ_SEGS_INLINE     4

#define ISP_DEVICE_LUN(dev)     ((dev) & 0xff)
#define ISP_DEVICE_TARGET(dev)  (((dev) >> 8) & 0x7f)
#define ISP_DEVICE_BUS(dev)     (((dev) >> 15) & 0x1)

/* Largest usable addresses, not counts: the core's check is inclusive. */
#define ISP_MAX_TARGET_ID   15
#define ISP_MAX_LUN         7

/* A continuation entry: four bytes reserved, then seven more segments. */
#define ISP_CONT_OFF_SEGS       8
#define ISP_CONT_SEGS           7

/* One inline or continuation segment: a 32-bit address and a 32-bit length. */
#define ISP_SEG_SIZE            8

/* Byte-identical to the type-0 header through offset 31. */
#define ISP_A64_ENTRY_TYPE_COMMAND    0x09
#define ISP_A64_ENTRY_TYPE_CONTINUE   0x0a

#define ISP_A64_REQ_OFF_SEGS        40
#define ISP_A64_REQ_SEGS_INLINE     2

#define ISP_A64_SEG_SIZE            12

/* No handle: offset 4 is the low half of a DMA address. */
#define ISP_A64_CONT_OFF_SEGS        4
#define ISP_A64_CONT_SEGS            5

/* Sense validity rides in the SCSI status word's bit 9. */
#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_STATE_FLAGS  12
#define ISP_STATUS_OFF_STATUS_FLAGS 14
#define ISP_STATUS_OFF_RSP_LEN      16
#define ISP_STATUS_OFF_SENSE_LEN    18
#define ISP_STATUS_OFF_RESID        20
#define ISP_STATUS_OFF_SENSE_DATA   32
#define ISP_STATUS_SENSE_MAX        32

#define ISP_SCSI_STATUS_SENSE_VALID 0x0200

#define ISP_IOCB_COMPLETE            0x0000
#define ISP_IOCB_INCOMPLETE          0x0001
#define ISP_IOCB_DATA_UNDERRUN       0x0015

#define ISP_STATE_FLAG_GOT_BUS            0x0100
#define ISP_STATE_FLAG_GOT_TARGET         0x0200
#define ISP_STATE_FLAG_SENT_CDB           0x0400
#define ISP_STATE_FLAG_TRANSFERRED_DATA   0x0800
#define ISP_STATE_FLAG_GOT_STATUS         0x1000
#define ISP_STATE_FLAG_GOT_SENSE          0x2000
#define ISP_STATE_FLAG_TRANSFER_COMPLETE  0x4000
#define ISP_STATE_FLAGS_ALL_PHASES  (ISP_STATE_FLAG_GOT_BUS | \
                                     ISP_STATE_FLAG_GOT_TARGET | \
                                     ISP_STATE_FLAG_SENT_CDB | \
                                     ISP_STATE_FLAG_TRANSFERRED_DATA | \
                                     ISP_STATE_FLAG_GOT_STATUS | \
                                     ISP_STATE_FLAG_GOT_SENSE | \
                                     ISP_STATE_FLAG_TRANSFER_COMPLETE)

#define ISP_STATUS_FLAG_TIMEOUT           0x0040

/*
 * A completion held for response-ring room, a case only a model with no
 * bus latency reaches; the SCSIRequest is already finished.
 */
typedef struct QLogicISPPendingStatus {
    QTAILQ_ENTRY(QLogicISPPendingStatus) next;
    uint32_t handle;
    uint16_t scsi_status;
    uint16_t completion;
    uint16_t state_flags;
    uint16_t status_flags;
    uint32_t resid;
    uint8_t sense[ISP_STATUS_SENSE_MAX];
    int sense_len;
} QLogicISPPendingStatus;

struct QLogicISPState {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1: an alias of io. */

    uint16_t bus_cfg1;
    uint16_t bus_ctrl;
    uint16_t bus_status;
    uint16_t semaphore;
    uint16_t nvram;    /* Echoes the clock/select/data-out bits it was sent. */
    uint16_t risc_flags;    /* What a read of the host command register says. */
    uint16_t dma_cmd_cfg;    /* Command DMA block's config register. */
    uint16_t dma_data_cfg;   /* Data DMA block's config register. */
    uint16_t mailbox[ISP_MAILBOX_COUNT];
    uint16_t risc_bank[ISP_SXP_RISC_COUNT];
    uint16_t sxp_bank[ISP_SXP_RISC_COUNT];
    uint16_t risc_ram[ISP_RISC_RAM_WORDS];    /* Downloaded firmware. */

    SCSIBus bus;

    hwaddr rqst_base;
    hwaddr resp_base;
    uint16_t rqst_len;
    uint16_t resp_len;
    uint16_t rqst_cons;    /* Next request entry this device will read. */
    uint16_t resp_prod;    /* Next response slot this device will write. */
    uint16_t resp_cons;    /* Next response slot the guest has acknowledged. */

    /* Not migrated: real hardware never reaches the condition. */
    QTAILQ_HEAD(, QLogicISPPendingStatus) pending_status;

    /* In-flight requests, for the guest-reset and migration paths. */
    QTAILQ_HEAD(, QLogicISPRequest) requests;
};

/* One in-flight command, handed back through sreq->hba_private. */
struct QLogicISPRequest {
    QLogicISPState *dev;
    SCSIRequest *sreq;
    QEMUSGList qsg;
    uint32_t handle;
    QTAILQ_ENTRY(QLogicISPRequest) next;
};

/*
 * Only the RISC request bit and its enable drive the line; the single AND
 * works because they share a bit position.
 */
QEMU_BUILD_BUG_ON(ISP_BUS_CTRL_RISC_INT_ENABLE != ISP_BUS_STATUS_RISC_INT_REQ);

static void qlogic_isp_update_irq(QLogicISPState *s)
{
    bool level = (s->bus_status & s->bus_ctrl &
                  ISP_BUS_STATUS_RISC_INT_REQ) != 0;

    trace_qlogic_isp_irq(level);
    pci_set_irq(PCI_DEVICE(s), level);
}

/* Frees every held completion without posting it. */
static void qlogic_isp_drop_pending_status(QLogicISPState *s)
{
    QLogicISPPendingStatus *pending;

    while ((pending = QTAILQ_FIRST(&s->pending_status)) != NULL) {
        QTAILQ_REMOVE(&s->pending_status, pending, next);
        g_free(pending);
    }
}

/*
 * Power-on and guest soft reset land on one state. Mailbox 4's signature
 * lives in the array so the register test's echo works before any queue
 * exists.
 */
static void qlogic_isp_reset_state(QLogicISPState *s)
{
    s->bus_cfg1 = 0;
    s->bus_ctrl = 0;
    s->bus_status = 0;
    s->semaphore = 0;
    s->nvram = 0;
    s->risc_flags = 0;
    s->dma_cmd_cfg = 0;
    s->dma_data_cfg = 0;

    memset(s->mailbox, 0, sizeof(s->mailbox));
    s->mailbox[1] = ISP_MAILBOX1_SIGNATURE;
    s->mailbox[2] = ISP_MAILBOX2_SIGNATURE;
    s->mailbox[3] = ISP_MAILBOX3_SIGNATURE;
    s->mailbox[4] = ISP_MAILBOX4_SIGNATURE;

    memset(s->risc_bank, 0, sizeof(s->risc_bank));
    memset(s->sxp_bank, 0, sizeof(s->sxp_bank));

    /* The guest re-downloads firmware after every reset. */
    memset(s->risc_ram, 0, sizeof(s->risc_ram));

    /* Zeroed so a stale ring cannot be acted on. */
    s->rqst_base = 0;
    s->resp_base = 0;
    s->rqst_len = 0;
    s->resp_len = 0;
    s->rqst_cons = 0;
    s->resp_prod = 0;
    s->resp_cons = 0;

    qlogic_isp_drop_pending_status(s);

    qlogic_isp_update_irq(s);
}

static void qlogic_isp_ram_block_dma(QLogicISPState *s, bool to_adapter)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t base = s->mailbox[1];
    uint16_t count = s->mailbox[4];
    hwaddr host = ISP_MBOX_ADDR_TOP(s->mailbox)
                | ISP_MBOX_ADDR_HIGH(s->mailbox)
                | ISP_MBOX_ADDR_MID(s->mailbox)
                | ISP_MBOX_ADDR_LOW(s->mailbox);
    uint16_t i;

    if (to_adapter) {
        trace_qlogic_isp_firmware_load(base, count, host);
    } else {
        trace_qlogic_isp_firmware_dump(base, count, host);
    }

    for (i = 0; i < count; i++) {
        uint16_t *slot = &s->risc_ram[(uint16_t)(base + i)];

        if (to_adapter) {
            lduw_le_pci_dma(pci, host + i * 2, slot, MEMTXATTRS_UNSPECIFIED);
        } else {
            stw_le_pci_dma(pci, host + i * 2, *slot, MEMTXATTRS_UNSPECIFIED);
        }
    }
}

/* Synchronous: the protocol has no way to leave a command pending. */
static void qlogic_isp_mailbox_command(QLogicISPState *s)
{
    uint16_t cmd = s->mailbox[0];

    trace_qlogic_isp_mbox_command(cmd);

    switch (s->mailbox[0]) {
    case ISP_MBOX_CMD_NOP:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_WRITE_RAM_WORD:
        s->risc_ram[s->mailbox[1]] = s->mailbox[2];
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* Mailbox 3 is an output too; left alone it hands back stale data. */
    case ISP_MBOX_CMD_READ_RAM_WORD:
        s->mailbox[2] = s->risc_ram[s->mailbox[1]];
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_LOAD_RAM:
        qlogic_isp_ram_block_dma(s, true);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_DUMP_RAM:
        qlogic_isp_ram_block_dma(s, false);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* Mailboxes 1-7 are expected back unchanged; they already are. */
    case ISP_MBOX_CMD_MAILBOX_REG_TEST:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* The word at base+3 is the image's length; a real image sums to 0. */
    case ISP_MBOX_CMD_VERIFY_CHECKSUM: {
        uint16_t start = s->mailbox[1];
        uint16_t len = s->risc_ram[(uint16_t)(start + 3)];
        uint16_t sum = 0;
        uint16_t i;

        for (i = 0; i < len; i++) {
            sum += s->risc_ram[(uint16_t)(start + i)];
        }

        /* The SGI firmware reads the mailbox 2 copy. */
        s->mailbox[1] = sum;
        s->mailbox[2] = sum;
        s->mailbox[0] = (sum == 0) ? ISP_MBOX_COMPLETE : ISP_MBOX_TEST_FAILED;
        break;
    }

    /* Accepted, never executed; the mailbox set was live from reset. */
    case ISP_MBOX_CMD_EXEC_FIRMWARE:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* Printed by guests, never branched on. */
    case ISP_MBOX_CMD_ABOUT_FIRMWARE:
        s->mailbox[1] = 1;
        s->mailbox[2] = 0;
        s->mailbox[3] = 0;
        s->mailbox[6] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* Sizes the guest's queues and command budget. */
    case ISP_MBOX_CMD_GET_FIRMWARE_STATUS:
        s->mailbox[1] = 0;
        s->mailbox[2] = ISP_FIRMWARE_STATUS_MAX_REQUESTS;
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* Asynchronous and narrow: the true state of an unnegotiated bus. */
    case ISP_MBOX_CMD_GET_TARGET_PARAMETERS:
        s->mailbox[2] = 0;
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Setters for state nothing reads back; answering invalid is a fatal
     * attach error, so they complete.
     */
    case ISP_MBOX_CMD_SET_FIRMWARE_FEATURES:
    case ISP_MBOX_CMD_SET_CLOCK_RATE:
    case ISP_MBOX_CMD_SET_RETRY_COUNT:
    case ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP:
    case ISP_MBOX_CMD_SET_ACTIVE_NEGATION:
    case ISP_MBOX_CMD_SET_TAG_AGE_LIMIT:
    case ISP_MBOX_CMD_SET_SELECTION_TIMEOUT:
    case ISP_MBOX_CMD_SET_INITIATOR_ID:
    case ISP_MBOX_CMD_SET_TARGET_PARAMETERS:
    case ISP_MBOX_CMD_SET_DEVICE_QUEUE:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* IRIX's bus-reset delay setter; same contract as the group above. */
    case ISP_MBOX_CMD_SET_RESET_DELAY_PARAMS:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_SET_PCI_CONTROL:
    case ISP_MBOX_CMD_SET_SYSTEM_PARAMETER:
    case ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /* The plain and A64 opcodes are one operation. */
    case ISP_MBOX_CMD_INIT_REQ_QUEUE:
    case ISP_MBOX_CMD_INIT_REQ_QUEUE_A64: {
        uint64_t addr = ISP_MBOX_ADDR_TOP(s->mailbox) |
                        ISP_MBOX_ADDR_HIGH(s->mailbox) |
                        ISP_MBOX_ADDR_MID(s->mailbox) |
                        ISP_MBOX_ADDR_LOW(s->mailbox);

        if (addr == 0 || s->mailbox[1] == 0) {
            s->mailbox[0] = ISP_MBOX_CMD_PARAM_ERROR;
            break;
        }

        s->rqst_base = addr;
        s->rqst_len = s->mailbox[1];
        s->rqst_cons = 0;
        s->mailbox[ISP_MAILBOX_REQUEST_QUEUE] = 0;
        trace_qlogic_isp_queue_init("request", addr, s->rqst_len);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;
    }

    case ISP_MBOX_CMD_INIT_RSP_QUEUE:
    case ISP_MBOX_CMD_INIT_RSP_QUEUE_A64: {
        uint64_t addr = ISP_MBOX_ADDR_TOP(s->mailbox) |
                        ISP_MBOX_ADDR_HIGH(s->mailbox) |
                        ISP_MBOX_ADDR_MID(s->mailbox) |
                        ISP_MBOX_ADDR_LOW(s->mailbox);

        if (addr == 0 || s->mailbox[1] == 0) {
            s->mailbox[0] = ISP_MBOX_CMD_PARAM_ERROR;
            break;
        }

        s->resp_base = addr;
        s->resp_len = s->mailbox[1];
        s->resp_prod = 0;
        s->resp_cons = 0;
        /* From here reads of the pointer address answer resp_prod. */
        qlogic_isp_drop_pending_status(s);
        trace_qlogic_isp_queue_init("response", addr, s->resp_len);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;
    }

    /* Resets the bus, not the adapter: chip state survives. */
    case ISP_MBOX_CMD_BUS_RESET:
        bus_cold_reset(BUS(&s->bus));
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: mailbox command 0x%04x not modelled\n",
                      s->mailbox[0]);
        s->mailbox[0] = ISP_MBOX_INVALID;
        break;
    }

    trace_qlogic_isp_mbox_complete(cmd, s->mailbox[0]);

    s->semaphore = ISP_SEMAPHORE_LOCK;
    s->bus_status |= ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ;
    qlogic_isp_update_irq(s);
}

static void qlogic_isp_host_command(QLogicISPState *s, uint16_t val)
{
    unsigned cmd = val >> ISP_HOST_CMD_SHIFT;

    trace_qlogic_isp_host_command(cmd);

    switch (cmd) {
    case ISP_HOST_CMD_NOP:
        break;

    /* A RISC reset replaces the whole flag set (86Box's state machine). */
    case ISP_HOST_CMD_RESET_RISC:
        s->risc_flags = ISP_RISC_FLAG_RESET;
        break;

    case ISP_HOST_CMD_PAUSE_RISC:
        s->risc_flags |= ISP_RISC_FLAG_PAUSED;
        break;

    /* Release clears reset too, or the flag would stand forever. */
    case ISP_HOST_CMD_RELEASE_RISC:
        s->risc_flags &= ~(ISP_RISC_FLAG_PAUSED | ISP_RISC_FLAG_RESET);
        break;

    /* "A mailbox command is ready" -- the trigger to process one. */
    case ISP_HOST_CMD_SET_HOST_INT:
        s->risc_flags |= ISP_RISC_FLAG_HOST_INT;
        qlogic_isp_mailbox_command(s);
        break;

    /* Does nothing on real hardware (86Box); neither driver issues it. */
    case ISP_HOST_CMD_CLEAR_HOST_INT:
        break;

    /* The semaphore is released by the driver's separate write, not here. */
    case ISP_HOST_CMD_CLEAR_RISC_INT:
        s->risc_flags &= ~ISP_RISC_FLAG_HOST_INT;
        s->bus_status = 0;
        qlogic_isp_update_irq(s);
        break;

    /* Accepted with nothing behind them, so the attach proceeds. */
    case ISP_HOST_CMD_MASK_PARITY:
    case ISP_HOST_CMD_BIOS_ENABLE:
    case ISP_HOST_CMD_ENABLE_PARITY:
    case ISP_HOST_CMD_PARITY_ERROR:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: host command 0x%x accepted, not modelled\n",
                      cmd);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: unknown host command 0x%x\n", cmd);
        break;
    }
}

/* Read fresh so a bank-select write takes effect on the next access. */
static uint16_t *qlogic_isp_active_bank(QLogicISPState *s)
{
    return (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ?
           s->sxp_bank : s->risc_bank;
}

/*
 * A descriptor read the fabric refuses answers zero, deliberately: a
 * zeroed entry type takes the fail-visibly default and a zeroed segment
 * transfers nothing. The CDB read and status write check their results.
 */
static uint32_t qlogic_isp_desc_ld32(PCIDevice *pci, hwaddr addr)
{
    uint32_t v = 0;

    ldl_le_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static uint16_t qlogic_isp_desc_ld16(PCIDevice *pci, hwaddr addr)
{
    uint16_t v = 0;

    lduw_le_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static uint8_t qlogic_isp_desc_ld8(PCIDevice *pci, hwaddr addr)
{
    uint8_t v = 0;

    ldub_pci_dma(pci, addr, &v, MEMTXATTRS_UNSPECIFIED);
    return v;
}

static hwaddr qlogic_isp_rqst_entry_addr(QLogicISPState *s, uint16_t index)
{
    return s->rqst_base + (hwaddr)index * ISP_QUEUE_ENTRY_SIZE;
}

static hwaddr qlogic_isp_resp_entry_addr(QLogicISPState *s, uint16_t index)
{
    return s->resp_base + (hwaddr)index * ISP_QUEUE_ENTRY_SIZE;
}

/*
 * One slot reserved so producer equal to consumer always means empty; an
 * uninitialised queue has no room rather than a division by zero.
 */
static bool qlogic_isp_resp_has_room(QLogicISPState *s)
{
    if (s->resp_len == 0) {
        return false;
    }
    return (s->resp_prod + 1) % s->resp_len != s->resp_cons;
}

/*
 * An I/O completion never takes the semaphore lock: the driver reads the
 * semaphore first and falls through to bus status only when it is clear.
 */
static void qlogic_isp_post_one_status(QLogicISPState *s, uint32_t handle,
                                       uint16_t scsi_status,
                                       uint16_t completion,
                                       uint16_t state_flags,
                                       uint16_t status_flags, uint32_t resid,
                                       const uint8_t *sense, int sense_len)
{
    PCIDevice *pci = PCI_DEVICE(s);
    hwaddr addr = qlogic_isp_resp_entry_addr(s, s->resp_prod);
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t clamped_sense_len = sense_len > 0
                                  ? MIN(sense_len, ISP_STATUS_SENSE_MAX) : 0;

    /* Built whole and written once, so the slot is never half-visible. */
    trace_qlogic_isp_status_post(handle, scsi_status, completion,
                                 state_flags, status_flags, resid);

    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_ENTRY_TYPE_STATUS;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    stl_le_p(entry + ISP_STATUS_OFF_HANDLE, handle);
    stw_le_p(entry + ISP_STATUS_OFF_SCSI_STATUS, scsi_status);
    stw_le_p(entry + ISP_STATUS_OFF_COMPLETION, completion);
    stw_le_p(entry + ISP_STATUS_OFF_STATE_FLAGS, state_flags);
    stw_le_p(entry + ISP_STATUS_OFF_STATUS_FLAGS, status_flags);
    stw_le_p(entry + ISP_STATUS_OFF_SENSE_LEN, clamped_sense_len);
    stl_le_p(entry + ISP_STATUS_OFF_RESID, resid);
    if (clamped_sense_len > 0) {
        memcpy(entry + ISP_STATUS_OFF_SENSE_DATA, sense, clamped_sense_len);
    }

    /*
     * A completion the fabric refuses to store is dropped: advancing the
     * produce index or raising the interrupt would point the guest at a
     * slot still holding a previous entry's bytes. The refusal itself is
     * the Bridge's own error path's to report.
     */
    if (pci_dma_write(pci, addr, entry, sizeof(entry)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: status entry write to 0x%" HWADDR_PRIx
                      " refused by the fabric; completion dropped\n", addr);
        return;
    }

    s->resp_prod = (s->resp_prod + 1) % s->resp_len;
    trace_qlogic_isp_response_ring(s->resp_prod, s->resp_cons);

    s->bus_status |= ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ;
    qlogic_isp_update_irq(s);
}

/* Only the guest's acknowledgement can free room, so only it calls this. */
static void qlogic_isp_drain_pending_status(QLogicISPState *s)
{
    QLogicISPPendingStatus *pending;

    while (qlogic_isp_resp_has_room(s) &&
           (pending = QTAILQ_FIRST(&s->pending_status)) != NULL) {
        QTAILQ_REMOVE(&s->pending_status, pending, next);
        qlogic_isp_post_one_status(s, pending->handle, pending->scsi_status,
                                   pending->completion, pending->state_flags,
                                   pending->status_flags, pending->resid,
                                   pending->sense_len > 0 ? pending->sense
                                                           : NULL,
                                   pending->sense_len);
        g_free(pending);
    }
}

/* A model with no bus latency can outrun the guest; the surplus is held. */
static void qlogic_isp_post_status(QLogicISPState *s, uint32_t handle,
                                   uint16_t scsi_status, uint16_t completion,
                                   uint16_t state_flags, uint16_t status_flags,
                                   uint32_t resid,
                                   const uint8_t *sense, int sense_len)
{
    if (s->resp_len == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: status posted before the response "
                      "queue was initialised\n");
        return;
    }

    if (!qlogic_isp_resp_has_room(s)) {
        QLogicISPPendingStatus *pending = g_new0(QLogicISPPendingStatus, 1);

        /* Loud on purpose: holding must not make the condition invisible. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: response queue full, holding handle "
                      "0x%08" PRIx32 " until the guest acknowledges\n",
                      handle);
        trace_qlogic_isp_status_held(handle);

        pending->handle = handle;
        pending->scsi_status = scsi_status;
        pending->completion = completion;
        pending->state_flags = state_flags;
        pending->status_flags = status_flags;
        pending->resid = resid;
        pending->sense_len = sense_len > 0
                              ? MIN(sense_len, ISP_STATUS_SENSE_MAX) : 0;
        if (pending->sense_len > 0) {
            memcpy(pending->sense, sense, pending->sense_len);
        }
        QTAILQ_INSERT_TAIL(&s->pending_status, pending, next);
        return;
    }

    qlogic_isp_post_one_status(s, handle, scsi_status, completion,
                               state_flags, status_flags, resid,
                               sense, sense_len);
}

/* Detached from hba_private before the unref: the request can outlive us. */
static void qlogic_isp_free_request(QLogicISPRequest *req)
{
    QTAILQ_REMOVE(&req->dev->requests, req, next);
    if (req->sreq != NULL) {
        req->sreq->hba_private = NULL;
        scsi_req_unref(req->sreq);
        req->sreq = NULL;
    }
    qemu_sglist_destroy(&req->qsg);
    g_free(req);
}

static void qlogic_isp_scsi_complete(SCSIRequest *sreq, size_t resid)
{
    QLogicISPRequest *req = sreq->hba_private;
    QLogicISPState *s = req->dev;
    uint8_t sense[SCSI_SENSE_BUF_SIZE];
    int sense_len;
    uint16_t scsi_status = sreq->status;
    uint16_t completion;
    uint16_t state_flags;

    sense_len = scsi_req_get_sense(sreq, sense, sizeof(sense));
    if (sense_len > 0) {
        scsi_status |= ISP_SCSI_STATUS_SENSE_VALID;
    }

    completion = resid ? ISP_IOCB_DATA_UNDERRUN : ISP_IOCB_COMPLETE;

    /*
     * Auto-sense runs only after a CHECK CONDITION, so GOT_SENSE reports the
     * sense phase alone; setting it on a clean command makes a driver that
     * checks the phase mask retry without bound.
     */
    state_flags = ISP_STATE_FLAGS_ALL_PHASES & ~ISP_STATE_FLAG_GOT_SENSE;
    if (sense_len > 0) {
        state_flags |= ISP_STATE_FLAG_GOT_SENSE;
    }

    /* The timeout bit belongs to the missing-target answer alone. */
    qlogic_isp_post_status(s, req->handle, scsi_status, completion,
                           state_flags, 0, resid, sense,
                           sense_len);

    qlogic_isp_free_request(req);
}

static void qlogic_isp_scsi_cancel(SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;

    trace_qlogic_isp_request_cancelled(req->handle);
    qlogic_isp_free_request(req);
}

/*
 * Without this, a data-carrying command reaches scsi_req_data() with no
 * sg list and the core falls back to transfer_data(), undefined here.
 */
static QEMUSGList *qlogic_isp_get_sg_list(SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;

    return &req->qsg;
}

/* Restores the HBA half behind hba_private; the shape is mptsas's. */
static void qlogic_isp_save_request(QEMUFile *f, SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;
    int i;

    qemu_put_be32(f, req->handle);
    qemu_put_be32(f, req->qsg.nsg);
    for (i = 0; i < req->qsg.nsg; i++) {
        qemu_put_be64(f, req->qsg.sg[i].base);
        qemu_put_be64(f, req->qsg.sg[i].len);
    }
}

/*
 * The most segments any saved request can carry: the 32-bit format's four
 * inline segments plus seven per continuation entry, with the eight-bit
 * entry count counting the command entry itself. The A64 format stays
 * below this (two inline, five per continuation).
 */
#define ISP_MIGRATE_SEGS_MAX \
    (ISP_REQ_SEGS_INLINE + ISP_CONT_SEGS * (UINT8_MAX - 1))

static void *qlogic_isp_load_request(QEMUFile *f, SCSIRequest *sreq)
{
    SCSIBus *bus = sreq->bus;
    QLogicISPState *s = container_of(bus, QLogicISPState, bus);
    PCIDevice *pci = PCI_DEVICE(s);
    QLogicISPRequest *req;
    int i, n;

    req = g_new0(QLogicISPRequest, 1);
    req->dev = s;
    req->handle = qemu_get_be32(f);
    pci_dma_sglist_init(&req->qsg, pci, 1);

    scsi_req_ref(sreq);
    req->sreq = sreq;
    QTAILQ_INSERT_TAIL(&s->requests, req, next);

    /*
     * No saved request can exceed the entry format's ceiling, so allocate
     * nothing on a claim above it. The callback has no failure return
     * (mptsas asserts here instead), so the stream is marked failed
     * directly; the wrapper is already consistent for teardown.
     */
    n = qemu_get_be32(f);
    if (n < 0 || n > ISP_MIGRATE_SEGS_MAX || qemu_file_get_error(f)) {
        qemu_file_set_error(f, -EINVAL);
        return req;
    }
    for (i = 0; i < n; i++) {
        uint64_t base = qemu_get_be64(f);
        uint64_t len = qemu_get_be64(f);

        qemu_sglist_add(&req->qsg, base, len);
    }

    return req;
}

static const struct SCSIBusInfo qlogic_isp_scsi_info = {
    .tcq = false,
    .max_target = ISP_MAX_TARGET_ID,
    .max_lun = ISP_MAX_LUN,
    .get_sg_list = qlogic_isp_get_sg_list,
    .complete = qlogic_isp_scsi_complete,
    .cancel = qlogic_isp_scsi_cancel,
    .save_request = qlogic_isp_save_request,
    .load_request = qlogic_isp_load_request,
};

static void qlogic_isp_read_segments(QLogicISPState *s, QEMUSGList *qsg,
                                     hwaddr rqst_addr, uint8_t entry_count,
                                     uint16_t seg_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t collected = 0;
    uint16_t cont_index = s->rqst_cons;
    int conts_left = entry_count - 1;
    int i;

    for (i = 0; i < ISP_REQ_SEGS_INLINE && collected < seg_count; i++) {
        hwaddr seg_addr = rqst_addr + ISP_REQ_OFF_SEGS + i * ISP_SEG_SIZE;
        uint32_t addr, len;

        addr = qlogic_isp_desc_ld32(pci, seg_addr);
        len = qlogic_isp_desc_ld32(pci, seg_addr + 4);
        qemu_sglist_add(qsg, addr, len);
        collected++;
    }

    while (collected < seg_count && conts_left > 0) {
        hwaddr cont_addr;

        cont_index = (cont_index + 1) % s->rqst_len;
        cont_addr = qlogic_isp_rqst_entry_addr(s, cont_index);

        for (i = 0; i < ISP_CONT_SEGS && collected < seg_count; i++) {
            hwaddr seg_addr = cont_addr + ISP_CONT_OFF_SEGS + i * ISP_SEG_SIZE;
            uint32_t addr, len;

            addr = qlogic_isp_desc_ld32(pci, seg_addr);
            len = qlogic_isp_desc_ld32(pci, seg_addr + 4);
            qemu_sglist_add(qsg, addr, len);
            collected++;
        }
        conts_left--;
    }
}

/*
 * Returns false when the fabric refused the CDB read: the walk stops on
 * the entry and nothing is posted -- the part documents no status for a
 * refused transfer, and the Bridge reports it on its own error path.
 */
static bool qlogic_isp_handle_request(QLogicISPState *s, hwaddr addr,
                                      uint8_t entry_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t handle;
    uint16_t device;
    uint8_t target, lun;
    uint16_t cdb_len;
    uint16_t seg_count;
    uint8_t cdb[ISP_REQ_CDB_LEN];
    SCSIDevice *sdev;
    QLogicISPRequest *req;


    handle = qlogic_isp_desc_ld32(pci, addr + ISP_REQ_OFF_HANDLE);
    device = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_DEVICE);
    cdb_len = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_CDBLEN);
    seg_count = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_SEGCOUNT);
    if (pci_dma_read(pci, addr + ISP_REQ_OFF_CDB, cdb, sizeof(cdb))
        != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: CDB read for the request entry at 0x%"
                      HWADDR_PRIx " refused by the fabric; request dropped\n",
                      addr);
        return false;
    }

    target = ISP_DEVICE_TARGET(device);
    lun = ISP_DEVICE_LUN(device);

    trace_qlogic_isp_request(handle, target, lun, cdb[0], seg_count);

    /* One bus on this part: naming the second answers incomplete. */
    if (ISP_DEVICE_BUS(device) != 0) {
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               0, 0, 0, NULL, 0);
        return true;
    }

    sdev = scsi_device_find(&s->bus, 0, target, lun);
    if (!sdev) {
        /*
         * GOT_BUS alone is Linux's DID_BAD_TARGET; the timeout bit beside
         * it is what lets IRIX treat the empty slot as benign.
         */
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               ISP_STATE_FLAG_GOT_BUS, ISP_STATUS_FLAG_TIMEOUT,
                               0, NULL, 0);
        return true;
    }

    req = g_new0(QLogicISPRequest, 1);
    req->dev = s;
    req->handle = handle;
    QTAILQ_INSERT_TAIL(&s->requests, req, next);
    /* A hint, never the raw seg_count: 65535 is a megabyte per request. */
    pci_dma_sglist_init(&req->qsg, pci, ISP_REQ_SEGS_INLINE);
    qlogic_isp_read_segments(s, &req->qsg, addr, entry_count, seg_count);

    req->sreq = scsi_req_new(sdev, handle, lun, cdb,
                             MIN(cdb_len, sizeof(cdb)), req);
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
    return true;
}

static void qlogic_isp_read_segments_a64(QLogicISPState *s, QEMUSGList *qsg,
                                         hwaddr rqst_addr, uint8_t entry_count,
                                         uint16_t seg_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint16_t collected = 0;
    uint16_t cont_index = s->rqst_cons;
    int conts_left = entry_count - 1;
    int i;

    for (i = 0; i < ISP_A64_REQ_SEGS_INLINE && collected < seg_count; i++) {
        hwaddr seg_addr = rqst_addr + ISP_A64_REQ_OFF_SEGS
                           + i * ISP_A64_SEG_SIZE;
        uint32_t lo, hi, len;

        lo = qlogic_isp_desc_ld32(pci, seg_addr);
        hi = qlogic_isp_desc_ld32(pci, seg_addr + 4);
        len = qlogic_isp_desc_ld32(pci, seg_addr + 8);
        qemu_sglist_add(qsg, lo | ((uint64_t)hi << 32), len);
        collected++;
    }

    while (collected < seg_count && conts_left > 0) {
        hwaddr cont_addr;

        cont_index = (cont_index + 1) % s->rqst_len;
        cont_addr = qlogic_isp_rqst_entry_addr(s, cont_index);

        for (i = 0; i < ISP_A64_CONT_SEGS && collected < seg_count; i++) {
            hwaddr seg_addr = cont_addr + ISP_A64_CONT_OFF_SEGS
                               + i * ISP_A64_SEG_SIZE;
            uint32_t lo, hi, len;

            lo = qlogic_isp_desc_ld32(pci, seg_addr);
            hi = qlogic_isp_desc_ld32(pci, seg_addr + 4);
            len = qlogic_isp_desc_ld32(pci, seg_addr + 8);
            qemu_sglist_add(qsg, lo | ((uint64_t)hi << 32), len);
            collected++;
        }
        conts_left--;
    }
}

static bool qlogic_isp_handle_request_a64(QLogicISPState *s, hwaddr addr,
                                          uint8_t entry_count)
{
    PCIDevice *pci = PCI_DEVICE(s);
    uint32_t handle;
    uint16_t device;
    uint8_t target, lun;
    uint16_t cdb_len;
    uint16_t seg_count;
    uint8_t cdb[ISP_REQ_CDB_LEN];
    SCSIDevice *sdev;
    QLogicISPRequest *req;


    handle = qlogic_isp_desc_ld32(pci, addr + ISP_REQ_OFF_HANDLE);
    device = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_DEVICE);
    cdb_len = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_CDBLEN);
    seg_count = qlogic_isp_desc_ld16(pci, addr + ISP_REQ_OFF_SEGCOUNT);
    /* The same refusal contract as the 32-bit path's CDB read. */
    if (pci_dma_read(pci, addr + ISP_REQ_OFF_CDB, cdb, sizeof(cdb))
        != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: CDB read for the request entry at 0x%"
                      HWADDR_PRIx " refused by the fabric; request dropped\n",
                      addr);
        return false;
    }

    target = ISP_DEVICE_TARGET(device);
    lun = ISP_DEVICE_LUN(device);

    trace_qlogic_isp_request(handle, target, lun, cdb[0], seg_count);

    /* The same single-bus check as the 32-bit path. */
    if (ISP_DEVICE_BUS(device) != 0) {
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               0, 0, 0, NULL, 0);
        return true;
    }

    sdev = scsi_device_find(&s->bus, 0, target, lun);
    if (!sdev) {
        /* The same missing-target answer as the 32-bit path. */
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               ISP_STATE_FLAG_GOT_BUS, ISP_STATUS_FLAG_TIMEOUT,
                               0, NULL, 0);
        return true;
    }

    req = g_new0(QLogicISPRequest, 1);
    req->dev = s;
    req->handle = handle;
    QTAILQ_INSERT_TAIL(&s->requests, req, next);
    /* A size hint only, for the reason the 32-bit path gives. */
    pci_dma_sglist_init(&req->qsg, pci, ISP_A64_REQ_SEGS_INLINE);
    qlogic_isp_read_segments_a64(s, &req->qsg, addr, entry_count, seg_count);

    req->sreq = scsi_req_new(sdev, handle, lun, cdb,
                             MIN(cdb_len, sizeof(cdb)), req);
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
    return true;
}

/*
 * No exact-equality exit terminates for every guest-supplied index and
 * entry_count -- a stride sharing a factor with the ring length loops
 * forever -- so the walk is budgeted by the count of published entries.
 */
static void qlogic_isp_process_requests(QLogicISPState *s)
{
    uint16_t prod;
    uint16_t budget;

    if (s->rqst_len == 0) {
        return;
    }

    prod = s->mailbox[ISP_MAILBOX_REQUEST_QUEUE] % s->rqst_len;
    budget = (prod + s->rqst_len - s->rqst_cons) % s->rqst_len;
    trace_qlogic_isp_request_ring(prod, s->rqst_cons);

    while (budget > 0) {
        PCIDevice *pci = PCI_DEVICE(s);
        hwaddr addr = qlogic_isp_rqst_entry_addr(s, s->rqst_cons);
        uint8_t entry_type;
        uint8_t entry_count;


        entry_type = qlogic_isp_desc_ld8(pci, addr + ISP_ENTRY_OFF_TYPE);
        entry_count = qlogic_isp_desc_ld8(pci, addr + ISP_ENTRY_OFF_COUNT);
        if (entry_count == 0) {
            entry_count = 1;
        }
        entry_count = MIN(entry_count, budget);

        switch (entry_type) {
        case ISP_ENTRY_TYPE_REQUEST:
            if (!qlogic_isp_handle_request(s, addr, entry_count)) {
                return;
            }
            break;

        /* Continuations are stepped past here; reading segments visits them. */
        case ISP_A64_ENTRY_TYPE_COMMAND:
            if (!qlogic_isp_handle_request_a64(s, addr, entry_count)) {
                return;
            }
            break;

        case ISP_ENTRY_TYPE_MARKER:
            break;

        /* Answered incomplete rather than consumed silently. */
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp: unexpected request entry type "
                          "0x%02x at index %u\n", entry_type,
                          s->rqst_cons);
            {
                uint32_t handle;

                handle = qlogic_isp_desc_ld32(
                    pci,
                    addr + ISP_REQ_OFF_HANDLE);
                qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                                       0, 0, 0, NULL, 0);
            }
            break;
        }

        s->rqst_cons = (s->rqst_cons + entry_count) % s->rqst_len;
        budget -= entry_count;
    }
}

static uint64_t qlogic_isp_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    QLogicISPState *s = opaque;

    /*
     * Must answer this adapter's own producer count, never the guest's
     * last write to the shared address.
     */
    if (addr == ISP_REG_RESPONSE_QUEUE) {
        if (s->resp_len == 0) {
            return s->mailbox[ISP_MAILBOX_RESPONSE_QUEUE];
        }
        return s->resp_prod;
    }

    /*
     * A read answers this adapter's consumer position; Linux computes its
     * free ring space from it, so echoing the guest's write breaks that.
     */
    if (addr == ISP_REG_REQUEST_QUEUE) {
        if (s->rqst_len == 0) {
            return s->mailbox[ISP_MAILBOX_REQUEST_QUEUE];
        }
        return s->rqst_cons;
    }

    if (addr >= ISP_REG_MAILBOX_BASE && addr < ISP_REG_MAILBOX_END) {
        return s->mailbox[(addr - ISP_REG_MAILBOX_BASE) / 2];
    }

    /* Traced, not LOG_UNIMP: the select works, the registers are undecoded. */
    if (addr >= ISP_REG_SXP_RISC_BASE && addr < ISP_REG_SXP_RISC_END) {
        uint16_t val =
            qlogic_isp_active_bank(s)[(addr - ISP_REG_SXP_RISC_BASE) / 2];

        trace_qlogic_isp_bank_read(
            (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ? "SXP" : "RISC",
            addr, val);
        return val;
    }

    switch (addr) {
    case ISP_REG_ID_LOW:
        return QLOGIC_ISP_VENDOR_ID;
    case ISP_REG_ID_HIGH:
        return QLOGIC_ISP_DEVICE_ID;
    case ISP_REG_BUS_CFG0:
        return ISP_BUS_CFG0_HWREV;
    case ISP_REG_BUS_CFG1:
        return s->bus_cfg1;
    case ISP_REG_BUS_CTRL:
        return s->bus_ctrl;
    case ISP_REG_BUS_STATUS:
        return s->bus_status;
    case ISP_REG_SEMAPHORE:
        return s->semaphore;
    case ISP_REG_NVRAM:
        return s->nvram;
    case ISP_REG_DMA_CMD_CFG:
        return s->dma_cmd_cfg;
    case ISP_REG_DMA_DATA_CFG:
        return s->dma_data_cfg;
    case ISP_REG_HOST_CMD:
        return s->risc_flags;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: read of unimplemented register "
                      "0x%02" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void qlogic_isp_reg_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    QLogicISPState *s = opaque;

    /*
     * The consumer acknowledgement: recorded into resp_cons, never over
     * the adapter's own producer count.
     */
    if (addr == ISP_REG_RESPONSE_QUEUE) {
        if (s->resp_len == 0) {
            s->mailbox[ISP_MAILBOX_RESPONSE_QUEUE] = val;
            return;
        }
        s->resp_cons = val % s->resp_len;
        trace_qlogic_isp_response_ring(s->resp_prod, s->resp_cons);
        qlogic_isp_drain_pending_status(s);
        return;
    }

    if (addr >= ISP_REG_MAILBOX_BASE && addr < ISP_REG_MAILBOX_END) {
        s->mailbox[(addr - ISP_REG_MAILBOX_BASE) / 2] = val;
        /* Writing the request-in pointer is itself the trigger. */
        if (addr == ISP_REG_REQUEST_QUEUE) {
            qlogic_isp_process_requests(s);
        }
        return;
    }

    /* Traced, not LOG_UNIMP, for the reason the read side gives. */
    if (addr >= ISP_REG_SXP_RISC_BASE && addr < ISP_REG_SXP_RISC_END) {
        trace_qlogic_isp_bank_write(
            (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ? "SXP" : "RISC",
            addr, val);
        qlogic_isp_active_bank(s)[(addr - ISP_REG_SXP_RISC_BASE) / 2] = val;
        return;
    }

    switch (addr) {
    case ISP_REG_BUS_CFG1:
        s->bus_cfg1 = val & ISP_BUS_CFG1_WRITE_MASK;
        break;

    /* Applied after the enables are stored, so the reset wins. */
    case ISP_REG_BUS_CTRL:
        s->bus_ctrl = val & ISP_BUS_CTRL_WRITE_MASK;
        if (val & ISP_BUS_CTRL_SOFT_RESET) {
            qlogic_isp_reset_state(s);
            /*
             * Only here, not in reset_state(): the device's own reset
             * phase reaches the child bus through the qdev tree.
             */
            bus_cold_reset(BUS(&s->bus));
        }
        qlogic_isp_update_irq(s);
        break;

    case ISP_REG_SEMAPHORE:
        s->semaphore = val & ISP_SEMAPHORE_MASK;
        break;

    case ISP_REG_NVRAM:
        s->nvram = val & ISP_NVRAM_WRITE_MASK;
        break;

    case ISP_REG_DMA_CMD_CFG:
        s->dma_cmd_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    case ISP_REG_DMA_DATA_CFG:
        s->dma_data_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    /* Nothing to quiesce: every transfer completes inside its write. */
    case ISP_REG_DMA_CMD_CTRL:
    case ISP_REG_DMA_DATA_CTRL:
        break;

    case ISP_REG_HOST_CMD:
        qlogic_isp_host_command(s, val);
        break;

    case ISP_REG_ID_LOW:
    case ISP_REG_ID_HIGH:
    case ISP_REG_BUS_CFG0:
    case ISP_REG_BUS_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp: write 0x%04x to read-only register "
                      "0x%02" HWADDR_PRIx "\n", (unsigned)val, addr);
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp: write 0x%04x to unimplemented register "
                      "0x%02" HWADDR_PRIx "\n", (unsigned)val, addr);
        break;
    }
}

/*
 * impl is pinned alongside valid: the defaults would chop or widen
 * accesses into repeated handler calls, and several writes here act.
 */
static const MemoryRegionOps qlogic_isp_reg_ops = {
    .read = qlogic_isp_reg_read,
    .write = qlogic_isp_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 2,
    },
};

static void qlogic_isp_reset_hold(Object *obj, ResetType type)
{
    QLogicISPState *s = QLOGIC_ISP1040(obj);

    qlogic_isp_reset_state(s);
}

static void qlogic_isp_realize(PCIDevice *dev, Error **errp)
{
    QLogicISPState *s = QLOGIC_ISP1040(dev);

    /* The adapter presents one interrupt line, and it is a level. */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /* IRIX fails the slot unless Status reads medium DEVSEL at reset. */
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);

    /* The memory BAR is an alias of the I/O BAR: one register file. */
    memory_region_init_io(&s->io, OBJECT(s), &qlogic_isp_reg_ops, s,
                          "qlogic-isp-io", QLOGIC_ISP_REG_WINDOW_SIZE);
    memory_region_init_alias(&s->mmio, OBJECT(s), "qlogic-isp-mmio",
                             &s->io, 0, QLOGIC_ISP_REG_WINDOW_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    QTAILQ_INIT(&s->pending_status);
    QTAILQ_INIT(&s->requests);

    scsi_bus_init(&s->bus, sizeof(s->bus), DEVICE(dev),
                 &qlogic_isp_scsi_info);
}

void qlogic_isp_handle_legacy_cmdline(DeviceState *dev)
{
    QLogicISPState *s = QLOGIC_ISP1040(dev);

    scsi_bus_legacy_handle_cmdline(&s->bus);
}

/*
 * In-flight requests need no walk: unrealize purges each child device's
 * queue first and each cancellation frees our wrapper. Held completions
 * are the one allocation not tied to a SCSIRequest.
 */
static void qlogic_isp_exit(PCIDevice *dev)
{
    QLogicISPState *s = QLOGIC_ISP1040(dev);

    qlogic_isp_drop_pending_status(s);
}

/*
 * Version 7 added resp_cons; an older stream loads with it at 0, which
 * is exactly what an unacknowledged guest means.
 */
static const VMStateDescription vmstate_qlogic_isp = {
    .name = "qlogic-isp",
    .version_id = 8,
    .minimum_version_id = 6,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QLogicISPState),
        VMSTATE_UINT16(bus_cfg1, QLogicISPState),
        VMSTATE_UINT16(bus_ctrl, QLogicISPState),
        VMSTATE_UINT16(bus_status, QLogicISPState),
        VMSTATE_UINT16(semaphore, QLogicISPState),
        VMSTATE_UINT16(nvram, QLogicISPState),
        VMSTATE_UINT16(risc_flags, QLogicISPState),
        VMSTATE_UINT16_V(dma_cmd_cfg, QLogicISPState, 8),
        VMSTATE_UINT16_V(dma_data_cfg, QLogicISPState, 8),
        VMSTATE_UINT16_ARRAY(mailbox, QLogicISPState, ISP_MAILBOX_COUNT),
        VMSTATE_UINT16_ARRAY(risc_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_UINT16_ARRAY(sxp_bank, QLogicISPState, ISP_SXP_RISC_COUNT),
        VMSTATE_UINT16_ARRAY(risc_ram, QLogicISPState, ISP_RISC_RAM_WORDS),
        VMSTATE_UINT64(rqst_base, QLogicISPState),
        VMSTATE_UINT64(resp_base, QLogicISPState),
        VMSTATE_UINT16(rqst_len, QLogicISPState),
        VMSTATE_UINT16(resp_len, QLogicISPState),
        VMSTATE_UINT16(rqst_cons, QLogicISPState),
        VMSTATE_UINT16(resp_prod, QLogicISPState),
        VMSTATE_UINT16_V(resp_cons, QLogicISPState, 7),
        VMSTATE_END_OF_LIST()
    }
};

static void qlogic_isp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = qlogic_isp_realize;
    pc->exit = qlogic_isp_exit;
    pc->vendor_id = QLOGIC_ISP_VENDOR_ID;
    pc->device_id = QLOGIC_ISP_DEVICE_ID;
    pc->revision = QLOGIC_ISP_REVISION;
    pc->class_id = PCI_CLASS_STORAGE_SCSI;
    rc->phases.hold = qlogic_isp_reset_hold;
    dc->vmsd = &vmstate_qlogic_isp;
    dc->desc = "QLogic ISP1040 SCSI host adapter";
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo qlogic_isp_types[] = {
    {
        .name = TYPE_QLOGIC_ISP1040,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(QLogicISPState),
        .class_init = qlogic_isp_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(qlogic_isp_types)
