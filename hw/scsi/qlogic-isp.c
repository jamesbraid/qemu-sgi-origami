/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QLogic ISP1020 family Fast/Wide SCSI host adapter.
 *
 * An intelligent controller running vendor microcode on an internal RISC
 * processor: the host hands it whole SCSI commands through a request ring
 * and it posts completions into a response ring. This model implements the
 * mailbox command set and the rings; the microcode download is accepted and
 * answered, never executed, so no vendor firmware is present or required.
 *
 * One PCI identification answers for the whole family; the revision byte
 * selects the part, hence the family file name and the per-part QOM type.
 *
 * Written from docs/datasheets/isp1040.md in the origami project tree,
 * itself driver-sourced (Linux qla1280 cross-checked against NetBSD isp,
 * OpenBSD 7.9 qlw for behaviour); no vendor or driver code is reproduced.
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

/*
 * Revision 5 is the ISP1040C: OpenBSD 7.9 prints "rev 0x05" attaching this
 * adapter on a captured Origin 200 boot.
 */
#define QLOGIC_ISP_REVISION     0x05

/*
 * A 0x100-byte window of 16-bit registers, presented through both an I/O BAR
 * and a memory BAR; OpenBSD tries the memory window first, so both decode.
 */
#define QLOGIC_ISP_REG_WINDOW_SIZE  0x100

/* Register offsets within that window. */
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

/*
 * Bus configuration 0's hardware-revision field runs one ahead of the PCI
 * revision byte across the family, so it is derived rather than stated twice.
 * Linux reads it only to detect the ISP1040A's broken FIFO.
 */
#define ISP_BUS_CFG0_HWREV      (QLOGIC_ISP_REVISION + 1)

/*
 * Bus configuration 1 on the PCI variant: FIFO threshold, burst enable, and
 * the select for which bank the window at 0x80 presents. Writes drop the rest.
 */
#define ISP_BUS_CFG1_FIFO_MASK      0x0070
#define ISP_BUS_CFG1_BANK_SELECT    0x0008
#define ISP_BUS_CFG1_BURST_ENABLE   0x0004
#define ISP_BUS_CFG1_WRITE_MASK     (ISP_BUS_CFG1_FIFO_MASK | \
                                     ISP_BUS_CFG1_BANK_SELECT | \
                                     ISP_BUS_CFG1_BURST_ENABLE)

/*
 * Each DMA block's config register, stored because OpenBSD read-modify-writes
 * the burst enable there and a readback must reflect what was written. The
 * blocks' other registers fall through to the unimplemented logger.
 */
#define ISP_DMA_CFG_BURST_ENABLE    0x0002
#define ISP_DMA_CFG_WRITE_MASK      ISP_DMA_CFG_BURST_ENABLE

/*
 * Bus interface control: a soft-reset trigger that must read back clear
 * (the guest polls for it and fails the attach otherwise), and two
 * interrupt enables.
 */
#define ISP_BUS_CTRL_SOFT_RESET     0x0001
#define ISP_BUS_CTRL_INT_ENABLE     0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004
#define ISP_BUS_CTRL_WRITE_MASK     (ISP_BUS_CTRL_INT_ENABLE | \
                                     ISP_BUS_CTRL_RISC_INT_ENABLE)

/* Bus interface status: which kind of interrupt is outstanding. */
#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004

/*
 * The adapter takes the semaphore lock when it posts a mailbox result; the
 * guest releases it by writing the register back to zero.
 */
#define ISP_SEMAPHORE_LOCK      0x0001
#define ISP_SEMAPHORE_STATUS    0x0002
#define ISP_SEMAPHORE_MASK      (ISP_SEMAPHORE_LOCK | ISP_SEMAPHORE_STATUS)

/*
 * Bit-banged serial EEPROM interface. Data-in is never asserted, so every
 * read fails contents validation and the guest falls back to built-in
 * defaults (OpenBSD prints the NVRAM corrupt and proceeds). Deliberate: no
 * source exists for a real Origin 200's EEPROM contents.
 */
#define ISP_NVRAM_CLOCK      0x0001
#define ISP_NVRAM_CHIP_SEL   0x0002
#define ISP_NVRAM_DATA_OUT   0x0004
#define ISP_NVRAM_DATA_IN    0x0008
#define ISP_NVRAM_WRITE_MASK (ISP_NVRAM_CLOCK | ISP_NVRAM_CHIP_SEL | \
                              ISP_NVRAM_DATA_OUT)

/*
 * Eight 16-bit mailboxes: mailbox 0 carries a command in and its result out,
 * the rest are per-command arguments and returns.
 *
 * Mailboxes 4 and 5 double as the queue pointers, each address meaning the
 * host's pointer when written and the adapter's when read. The request side
 * is plain storage (the guest is the only writer). The response side is not:
 * a read answers the adapter's own producer count and a write records the
 * guest's consumer position, never overwriting that count. Both addresses
 * stay ordinary mailboxes until their queue is initialised -- the firmware's
 * register test writes a pattern through them before any queue exists -- so
 * every queue-pointer path below is gated on the queue's length.
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
 * The product-ID signature reset leaves in mailboxes 1 through 4. Linux's
 * qla1280_chip_diag() compares them against PROD_ID_1..4 (qla1280.h) before
 * any mailbox command and aborts the attach on a mismatch; 86Box presents
 * the same four values.
 */
#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020
#define ISP_MAILBOX4_SIGNATURE  0x0001

/*
 * The mailbox command protocol: the host writes mailbox 0 and whatever
 * arguments the command needs, then sets the host interrupt, and the adapter
 * answers by writing a result into mailbox 0 and posting an interrupt. Complete
 * and invalid are the two outcomes every command shares; a command-specific
 * error is any other value.
 */
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
 * Adapter memory for the microcode download the guest performs on every
 * attach. Sized to the full 16-bit address space a mailbox argument can
 * name, so every address is valid by construction. The contents are
 * retained, never executed: keeping them is what lets verify-checksum
 * answer honestly about a corrupt image.
 */
#define ISP_RISC_RAM_WORDS      0x10000

/*
 * The request count a guest sizes its queues from. No source states a real
 * adapter's number; 512 is OpenBSD's own clamp on whatever is reported.
 */
#define ISP_FIRMWARE_STATUS_MAX_REQUESTS 512

/*
 * Host command register. A write carries a command in the top four bits; a
 * read returns flags describing what the RISC processor is doing.
 */
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
 * The SXP and RISC register blocks share the window from 0x80 to 0xc0; bus
 * configuration 1 bit 3 -- not bit 0, a decoy constant in driver headers --
 * selects which answers. Two real storage banks, so the select is
 * observable; no individual register is decoded.
 */
#define ISP_REG_SXP_RISC_BASE   0x80
#define ISP_REG_SXP_RISC_END    0xc0
#define ISP_SXP_RISC_COUNT      \
    ((ISP_REG_SXP_RISC_END - ISP_REG_SXP_RISC_BASE) / 2)

/*
 * A queue-init command's base address is a 64-bit host address split across
 * mailboxes 2, 3, 6 and 7 (qlw_mbox_putaddr()'s order); mailbox 1 is the
 * entry count. Zero for either is rejected, as 86Box also does.
 */
#define ISP_MBOX_ADDR_LOW(mb)   ((uint64_t)(mb)[3])
#define ISP_MBOX_ADDR_MID(mb)   ((uint64_t)(mb)[2] << 16)
#define ISP_MBOX_ADDR_HIGH(mb)  ((uint64_t)(mb)[7] << 32)
#define ISP_MBOX_ADDR_TOP(mb)   ((uint64_t)(mb)[6] << 48)

/*
 * Queue entries are 64 bytes; the offsets below are confirmed against
 * OpenBSD's packed struct declarations for each entry kind.
 */
#define ISP_QUEUE_ENTRY_SIZE    64

/* The entry header every kind of entry starts with. */
#define ISP_ENTRY_OFF_TYPE      0
#define ISP_ENTRY_OFF_COUNT     1

#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_CONTINUATION 0x02
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_ENTRY_TYPE_MARKER       0x04

/*
 * A request entry. The device word packs bus, target and lun -- the bit
 * layout is how OpenBSD's driver builds it: (((bus << 7) | target) << 8) | lun.
 */
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

/*
 * Wide SCSI. The SCSI core's realize check is inclusive, so these are the
 * largest usable addresses, not counts.
 */
#define ISP_MAX_TARGET_ID   15
#define ISP_MAX_LUN         7

/* A continuation entry: four bytes reserved, then seven more segments. */
#define ISP_CONT_OFF_SEGS       8
#define ISP_CONT_SEGS           7

/* One inline or continuation segment: a 32-bit address and a 32-bit length. */
#define ISP_SEG_SIZE            8

/*
 * The 64-bit addressing pair, Linux's only request format. Offsets 0-31 are
 * byte-identical to the type-0 format (qla1280.h's cmd_a64_entry_t against
 * cmd_entry), so the header constants above are shared; only the segment
 * area from offset 32 differs.
 */
#define ISP_A64_ENTRY_TYPE_COMMAND    0x09
#define ISP_A64_ENTRY_TYPE_CONTINUE   0x0a

#define ISP_A64_REQ_OFF_SEGS        40
#define ISP_A64_REQ_SEGS_INLINE     2

/*
 * A 64-bit segment: the address as two 32-bit words, low half first
 * (qla1280_64bit_start_scsi()'s write order), then a 32-bit length.
 */
#define ISP_A64_SEG_SIZE            12

/*
 * A64 continuation entry: no handle -- offset 4 holds the low half of a DMA
 * address, so a continuation must never be dispatched on its own. The
 * entry_count advance steps the ring walker past it.
 */
#define ISP_A64_CONT_OFF_SEGS        4
#define ISP_A64_CONT_SEGS            5

/*
 * A status entry, written by the adapter. Sense validity rides in the SCSI
 * status word's own bit 9, not a separate flag.
 */
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

/*
 * State flags: which phases the command reached, read by the drivers only
 * on an incomplete completion to classify the failure -- Linux's
 * qla1280_return_status() ladder turns them into DID_NO_CONNECT,
 * DID_BAD_TARGET or DID_ERROR. Layout confirmed against both qlwreg.h and
 * qla1280.h.
 */
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

/*
 * status_flags reports conditions about the completion, not phases. IRIX
 * reads the timeout bit to accept an incomplete completion as a benign
 * selection timeout; the value is NetBSD ispmbox.h's RQSTF_TIMEOUT.
 */
#define ISP_STATUS_FLAG_TIMEOUT           0x0040

/*
 * A status entry the response ring had no room for. Real hardware never
 * reaches the full case, but a model with no bus latency can; the entry is
 * held until the guest's next acknowledgement frees a slot. The SCSIRequest
 * is already finished by the time this is built.
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

    /*
     * Held completions, not migrated: a migration landing while this is
     * non-empty loses them, and real hardware never reaches the condition.
     */
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
 * One level interrupt line, recomputed from scratch on every change. Only
 * the RISC request bit and its enable drive it: per 86Box, the global
 * enable does nothing observable on real hardware. The single AND works
 * only because enable and request share a bit position.
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
 * The state a power-on and a guest soft reset both land on. Mailboxes 1-4
 * get the product-ID signature Linux's post-reset check requires; mailbox
 * 4's copy lives in the array so the register test's echo still works
 * before a request queue exists. Everything else zeroes as an
 * uncontradicted default -- no vendor register table exists for this part.
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

    /*
     * Zeroed so this device cannot act on a stale ring, whatever the
     * driver's own re-initialisation ordering.
     */
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

/*
 * The load-RAM and dump-RAM block transfers: mailbox 1 is the word offset
 * within the adapter, mailbox 4 the word count, and the host address is the
 * usual four-mailbox split. The offset advances as a uint16_t so it wraps
 * within storage sized to that range; little-endian PCI DMA, like the rings.
 */
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

/*
 * Runs a mailbox command: result into mailbox 0, semaphore lock taken,
 * interrupt posted. Synchronous, inside the register write that triggers
 * it -- the protocol has no way to leave a command pending.
 */
static void qlogic_isp_mailbox_command(QLogicISPState *s)
{
    uint16_t cmd = s->mailbox[0];

    trace_qlogic_isp_mbox_command(cmd);

    switch (s->mailbox[0]) {
    case ISP_MBOX_CMD_NOP:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * One per 16-bit word of the firmware image: mailbox 1 the address,
     * mailbox 2 the data. Every address indexes storage sized to the range.
     */
    case ISP_MBOX_CMD_WRITE_RAM_WORD:
        s->risc_ram[s->mailbox[1]] = s->mailbox[2];
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Issued by the SGI firmware's power-on diagnostic (write a word, read
     * it back), not by any OS driver. Mailbox 3 is answered zero as well:
     * 86Box declares both mailboxes outputs, and leaving it alone would
     * hand back a previous command's value.
     */
    case ISP_MBOX_CMD_READ_RAM_WORD:
        s->mailbox[2] = s->risc_ram[s->mailbox[1]];
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Block forms of the two directions; the firmware's diagnostic pushes a
     * pattern through the adapter's memory and reads it back over DMA.
     */
    case ISP_MBOX_CMD_LOAD_RAM:
        qlogic_isp_ram_block_dma(s, true);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    case ISP_MBOX_CMD_DUMP_RAM:
        qlogic_isp_ram_block_dma(s, false);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Echo test: mailboxes 1-7 are expected back unchanged. The values are
     * already in s->mailbox[], so completing untouched is the implementation.
     */
    case ISP_MBOX_CMD_MAILBOX_REG_TEST:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The word at base+3 is the image's own length in words -- the same
     * value the guest's download loop uses. A real image sums to zero;
     * answering honestly lets a corrupt download fail visibly.
     */
    case ISP_MBOX_CMD_VERIFY_CHECKSUM: {
        uint16_t start = s->mailbox[1];
        uint16_t len = s->risc_ram[(uint16_t)(start + 3)];
        uint16_t sum = 0;
        uint16_t i;

        for (i = 0; i < len; i++) {
            sum += s->risc_ram[(uint16_t)(start + i)];
        }

        /*
         * Answered in mailbox 2 as well (NetBSD's output mask); the SGI
         * firmware reads that one.
         */
        s->mailbox[1] = sum;
        s->mailbox[2] = sum;
        s->mailbox[0] = (sum == 0) ? ISP_MBOX_COMPLETE : ISP_MBOX_TEST_FAILED;
        break;
    }

    /* Accepted, never executed; the mailbox set was live from reset. */
    case ISP_MBOX_CMD_EXEC_FIRMWARE:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * A synthetic version triple: OpenBSD prints it and never branches on
     * it, so the command only has to succeed.
     */
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

    /*
     * Reports every target asynchronous and narrow -- the true state of a
     * bus this model does not negotiate.
     */
    case ISP_MBOX_CMD_GET_TARGET_PARAMETERS:
        s->mailbox[2] = 0;
        s->mailbox[3] = 0;
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * Config setters for negotiation and timing this model does not track,
     * and nothing reads back. OpenBSD's qlw_attach() treats any of them
     * answering invalid as fatal, so the contract is that they complete.
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

    /*
     * Linux-only setters from qla1280_nvram_config(), whose caller discards
     * the result; accepted like the group above.
     */
    case ISP_MBOX_CMD_SET_PCI_CONTROL:
    case ISP_MBOX_CMD_SET_SYSTEM_PARAMETER:
    case ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY:
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;

    /*
     * The plain and A64 opcodes are one operation: OpenBSD sends 0x10 and
     * Linux 0x52, both composing the same 64-bit address across the same
     * four mailboxes.
     */
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
        /*
         * From here the queue is live: reads of the pointer address answer
         * resp_prod, not the mailbox array.
         */
        qlogic_isp_drop_pending_status(s);
        trace_qlogic_isp_queue_init("response", addr, s->resp_len);
        s->mailbox[0] = ISP_MBOX_COMPLETE;
        break;
    }

    /*
     * Cold-resetting the SCSI bus cancels in-flight requests and arms each
     * device's unit attention. The chip's own state survives: this resets
     * the bus, not the adapter.
     */
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

    /*
     * Release clears reset as well as paused (86Box agrees): the guest
     * resets then releases, and a standing reset flag would report a
     * processor held in reset forever.
     */
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

    /*
     * First half of every interrupt acknowledgement: clears the host-int
     * flag and bus status (86Box). The semaphore is released by the
     * driver's separate zero write, not here.
     */
    case ISP_HOST_CMD_CLEAR_RISC_INT:
        s->risc_flags &= ~ISP_RISC_FLAG_HOST_INT;
        s->bus_status = 0;
        qlogic_isp_update_irq(s);
        break;

    /*
     * Accepted so the guest's attach proceeds, but with nothing behind them:
     * the parity and BIOS commands control paths this model does not have.
     */
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

/*
 * Read fresh from bus_cfg1 so a bank-select write takes effect on the very
 * next window access -- the order NetBSD's access pattern relies on.
 */
static uint16_t *qlogic_isp_active_bank(QLogicISPState *s)
{
    return (s->bus_cfg1 & ISP_BUS_CFG1_BANK_SELECT) ?
           s->sxp_bank : s->risc_bank;
}

/*
 * Descriptor reads: plain little-endian, any byte-lane permutation is the
 * Bridge's. A read the fabric refuses answers zero, deliberately -- a
 * zeroed entry type takes the fail-visibly default and a zeroed segment
 * transfers nothing. The CDB read and the status write, where a defined
 * zero would lie, check their results at their own call sites.
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
 * One slot is reserved so producer equal to consumer always means empty,
 * never full. An uninitialised queue (resp_len == 0) has no room, checked
 * here rather than left to divide by zero.
 */
static bool qlogic_isp_resp_has_room(QLogicISPState *s)
{
    if (s->resp_len == 0) {
        return false;
    }
    return (s->resp_prod + 1) % s->resp_len != s->resp_cons;
}

/*
 * Writes one status entry and raises the interrupt. An I/O completion never
 * takes the semaphore lock -- the driver reads the semaphore first and only
 * falls through to bus status when it finds the lock clear. The caller has
 * already confirmed a slot is free. The whole slot is zeroed first: the
 * ring is reused, and fields with no confirmed reader stay defined rather
 * than inheriting a previous occupant's bytes.
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

/*
 * Posts held completions, oldest first, while room lasts. Only the guest's
 * acknowledgement can free room, so only that write handler calls this.
 */
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

/*
 * Posts a status entry, holding it if the ring is full. Real hardware never
 * reaches the full case, but a model with no bus latency can outrun the
 * guest's acknowledgements, and the surplus must be held, not dropped.
 */
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

        /*
         * Loud on purpose: holding the completion must not also make the
         * condition invisible.
         */
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

/*
 * The SCSIRequest can outlive this bookkeeping, so it is detached from
 * hba_private before the unref -- mptsas_free_request()'s shape.
 */
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

    sense_len = scsi_req_get_sense(sreq, sense, sizeof(sense));
    if (sense_len > 0) {
        scsi_status |= ISP_SCSI_STATUS_SENSE_VALID;
    }

    completion = resid ? ISP_IOCB_DATA_UNDERRUN : ISP_IOCB_COMPLETE;

    /*
     * The full phase set is the true record for a command that completed;
     * the timeout bit belongs to the missing-target answer alone.
     */
    qlogic_isp_post_status(s, req->handle, scsi_status, completion,
                           ISP_STATE_FLAGS_ALL_PHASES, 0, resid, sense,
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
 * Without this, a data-carrying command reaches scsi_req_data() with no sg
 * list and the core's fallback calls bus->info->transfer_data(), which
 * this model does not define.
 */
static QEMUSGList *qlogic_isp_get_sg_list(SCSIRequest *sreq)
{
    QLogicISPRequest *req = sreq->hba_private;

    return &req->qsg;
}

/*
 * The SCSI core recreates every in-flight SCSIRequest on the destination;
 * these callbacks restore the HBA half behind hba_private -- the handle
 * and the scatter-gather layout. The shape is mptsas's.
 */
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

/*
 * Reads a command entry's segments: four inline, then seven per
 * continuation, until seg_count or the header's entry_count runs out.
 */
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
 * Dispatches one request entry to the SCSI layer; an empty target answers
 * with an incomplete status entry, as on real hardware. Returns false when
 * the fabric refused the CDB read: the walk stops with the consume index
 * on the entry, and nothing is posted -- the part documents no status for
 * a refused transfer, and the Bridge reports it on its own error path.
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

    /*
     * One SCSI bus on this part: a request naming the second one is
     * answered incomplete with no phase reached, never executed on bus 0.
     */
    if (ISP_DEVICE_BUS(device) != 0) {
        qlogic_isp_post_status(s, handle, 0, ISP_IOCB_INCOMPLETE,
                               0, 0, 0, NULL, 0);
        return true;
    }

    sdev = scsi_device_find(&s->bus, 0, target, lun);
    if (!sdev) {
        /*
         * Reached the bus, selected nothing, timed out. GOT_BUS alone is
         * Linux's DID_BAD_TARGET; the timeout bit alongside it is what
         * lets IRIX, which also checks status_flags, treat the empty slot
         * as benign rather than fault the probe.
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
    /*
     * A size hint only, never the guest's raw seg_count: a claimed 65535
     * would reserve a megabyte per request.
     */
    pci_dma_sglist_init(&req->qsg, pci, ISP_REQ_SEGS_INLINE);
    qlogic_isp_read_segments(s, &req->qsg, addr, entry_count, seg_count);

    req->sreq = scsi_req_new(sdev, handle, lun, cdb,
                             MIN(cdb_len, sizeof(cdb)), req);
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }
    return true;
}

/*
 * The A64 counterpart of qlogic_isp_read_segments(): two inline, five per
 * continuation, 12 bytes each; address words low half first, matching
 * qla1280_64bit_start_scsi()'s write order.
 */
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

/*
 * The A64 counterpart of qlogic_isp_handle_request(): the header is
 * byte-identical through offset 31, so only the segment area differs.
 */
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
 * Walks every entry between the last stop and the request-in index the
 * host just wrote. Both the index and each entry's entry_count are
 * guest-supplied, and no exact-equality exit terminates for every
 * combination of them -- an unwrapped index, or a stride sharing a factor
 * with the ring length, loops forever. So the walk is budgeted by the
 * count of published entries, and each entry's claimed slot count is
 * clamped to what remains.
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

        /*
         * A64 continuations are never dispatched on their own: the
         * entry_count advance below steps the walker past them, and
         * reading segments is what visits them.
         */
        case ISP_A64_ENTRY_TYPE_COMMAND:
            if (!qlogic_isp_handle_request_a64(s, addr, entry_count)) {
                return;
            }
            break;

        case ISP_ENTRY_TYPE_MARKER:
            break;

        /*
         * An unimplemented entry type is answered incomplete with the
         * handle at the offset every handle-carrying type shares, rather
         * than consumed silently.
         */
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
     * The response-in read must answer this adapter's own producer count,
     * never the guest's last write to the shared address. Until a queue
     * exists the address is a plain mailbox -- the register test's pattern
     * must echo.
     */
    if (addr == ISP_REG_RESPONSE_QUEUE) {
        if (s->resp_len == 0) {
            return s->mailbox[ISP_MAILBOX_RESPONSE_QUEUE];
        }
        return s->resp_prod;
    }

    /*
     * The same shape with the directions swapped: a read answers this
     * adapter's consumer position. Linux computes its free request-ring
     * space from exactly this read (qla1280_start_scsi()), so answering
     * the guest's own write back would break that arithmetic.
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

    /*
     * Traced, not LOG_UNIMP: the bank select is implemented, the individual
     * registers are just not decoded.
     */
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
     * The guest's consumer acknowledgement, recorded into resp_cons and
     * never over the adapter's own producer count; wrapped defensively
     * since a guest write need not be a valid ring index. Until a queue
     * exists it is the mailbox store it looks like.
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

    /*
     * The soft reset takes effect immediately and reads back clear; it is
     * applied after the enables are stored so the reset wins. The line is
     * recomputed either way, since the enables are half of what decides it.
     */
    case ISP_REG_BUS_CTRL:
        s->bus_ctrl = val & ISP_BUS_CTRL_WRITE_MASK;
        if (val & ISP_BUS_CTRL_SOFT_RESET) {
            qlogic_isp_reset_state(s);
            /*
             * In-flight requests must not complete into rings the reset
             * dissolved. Only here, not in reset_state(): the device's own
             * reset phase reaches the child bus through the qdev tree.
             */
            bus_cold_reset(BUS(&s->bus));
        }
        qlogic_isp_update_irq(s);
        break;

    case ISP_REG_SEMAPHORE:
        s->semaphore = val & ISP_SEMAPHORE_MASK;
        break;

    /*
     * Data-in is outside the write mask, so a readback always shows it
     * clear; nothing shifts anything back.
     */
    case ISP_REG_NVRAM:
        s->nvram = val & ISP_NVRAM_WRITE_MASK;
        break;

    case ISP_REG_DMA_CMD_CFG:
        s->dma_cmd_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    case ISP_REG_DMA_DATA_CFG:
        s->dma_data_cfg = val & ISP_DMA_CFG_WRITE_MASK;
        break;

    /*
     * Quiesce-engine writes from the firmware's diagnostic. Every transfer
     * here completes inside the write that started it, so there is nothing
     * to stop; recognised to keep the unimplemented log honest.
     */
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
 * Little-endian 16-bit registers. impl is pinned alongside valid because
 * the defaults would chop or widen accesses into repeated handler calls,
 * and several writes here act rather than store.
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

    /*
     * IRIX's io_config_space diagnostic fails the slot unless the Status
     * half of the command/status dword reads medium DEVSEL. The bit is
     * read-only, so it survives reset.
     */
    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);

    /*
     * One register file behind two windows: the memory BAR is an alias of
     * the I/O BAR, so the two cannot drift apart.
     */
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
 * In-flight requests need no walk here: unrealize purges each child SCSI
 * device's queue before this hook runs, and each cancellation frees our
 * wrapper. Completions held for response-ring room are the one allocation
 * whose life is not tied to a SCSIRequest, so unrealizing the device with
 * the list non-empty leaked them.
 */
static void qlogic_isp_exit(PCIDevice *dev)
{
    QLogicISPState *s = QLOGIC_ISP1040(dev);

    qlogic_isp_drop_pending_status(s);
}

/*
 * Version 7 added resp_cons; an older stream is accepted with it at 0,
 * which is exactly what an unacknowledged guest means. Held completions
 * are not migrated (real hardware never reaches the condition); in-flight
 * requests migrate through the save/load callbacks.
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
