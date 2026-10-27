/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QLogic ISP2200 Fibre Channel host adapter.
 *
 * The host talks to the adapter's firmware through 24 mailboxes and two
 * rings in host memory. As with the SCSI parts in qlogic-isp.c, the guest's
 * firmware download is accepted, retained and checksummed, never executed;
 * the model implements the mailbox commands and rings that firmware
 * provides.
 *
 * Behind the host interface sits a private arbitrated loop kept inside the
 * device rather than a Fibre Channel subsystem: firmware hides frames from
 * the host, which sees only loop IDs, names and SCSI commands. Each SCSI
 * device on the adapter's bus is one LUN of the loop port whose loop ID is
 * its target number, named by its port_wwn and wwn properties. The contract
 * and its sources are in docs/system/mips/sgi/hardware/qlogic-isp2200.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/hotplug.h"
#include "hw/pci/pci_device.h"
#include "hw/scsi/scsi.h"
#include "migration/qemu-file-types.h"
#include "migration/qemu-file.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "scsi/constants.h"
#include "system/dma.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_QLOGIC_ISP2200 "qlogic-isp2200"
OBJECT_DECLARE_SIMPLE_TYPE(QLogicISP2200State, QLOGIC_ISP2200)

#define ISP2200_VENDOR_ID           0x1077
#define ISP2200_DEVICE_ID           0x2200
/* The retail QLA2200 board's subsystem entry in the PCI ID list. */
#define ISP2200_SUBSYSTEM_ID        0x0002
/* Fibre Channel serial bus controller; no driver matches on it. */
#define ISP2200_CLASS_FIBRE_CHANNEL 0x0c04

#define ISP2200_REG_WINDOW_SIZE     0x100

#define ISP2200_REG_FLASH_ADDR  0x00
#define ISP2200_REG_FLASH_DATA  0x02
#define ISP2200_REG_CSR         0x06
#define ISP2200_REG_ICR         0x08
#define ISP2200_REG_ISR         0x0a
#define ISP2200_REG_SEMAPHORE   0x0c
#define ISP2200_REG_NVRAM       0x0e
#define ISP2200_REG_HCCR        0xc0
#define ISP2200_REG_GPIO_DATA   0xcc
#define ISP2200_REG_GPIO_ENABLE 0xce

/* Mailboxes 0-7 sit after the bus interface; 8-23 at the window's end. */
#define ISP2200_MAILBOX_COUNT   24
#define ISP2200_REG_MBOX_LO     0x10
#define ISP2200_MBOX_LO_COUNT   8
#define ISP2200_REG_MBOX_HI     0xe0

/* The request and response queue pointers share these two mailboxes. */
#define ISP2200_MBOX_REQUEST    4
#define ISP2200_MBOX_RESPONSE   5

#define ISP2200_CSR_SOFT_RESET  0x0001
#define ISP2200_CSR_BANK_MASK   0x0030

#define ISP2200_ICR_GLOBAL      0x8000
#define ISP2200_ICR_RISC        0x0008
#define ISP2200_ICR_MASK        (ISP2200_ICR_GLOBAL | ISP2200_ICR_RISC)

#define ISP2200_ISR_PENDING     0x8000
#define ISP2200_ISR_RISC        0x0008

#define ISP2200_SEMAPHORE_LOCK  0x0001

/*
 * Data-in is never asserted: every EEPROM read fails validation and the
 * guest falls back to built-in defaults, as with an unprogrammed part.
 */
#define ISP2200_NVRAM_WRITE_MASK 0x0007

#define ISP2200_HCCR_SHIFT          12
#define ISP2200_HCCR_NOP            0x0
#define ISP2200_HCCR_RESET_RISC     0x1
#define ISP2200_HCCR_PAUSE_RISC     0x2
#define ISP2200_HCCR_RELEASE_RISC   0x3
#define ISP2200_HCCR_PARITY_4       0x4
#define ISP2200_HCCR_SET_HOST_INT   0x5
#define ISP2200_HCCR_CLEAR_HOST_INT 0x6
#define ISP2200_HCCR_CLEAR_RISC_INT 0x7
#define ISP2200_HCCR_BREAKPOINT     0x8
#define ISP2200_HCCR_BIOS           0x9
#define ISP2200_HCCR_PARITY         0xa
#define ISP2200_HCCR_PARITY_ERROR   0xe
#define ISP2200_HCCR_TEST_MODE      0xf

#define ISP2200_HCCR_HOST_INT       0x0080
#define ISP2200_HCCR_RISC_RESET     0x0040
#define ISP2200_HCCR_RISC_PAUSED    0x0020

/* What the mailboxes hold once the ROM code has booted. */
#define ISP2200_MBOX_BUSY           0x0004
#define ISP2200_PRODUCT_ID_1        0x4953
#define ISP2200_PRODUCT_ID_2        0x5020
#define ISP2200_PRODUCT_ID_3        0x2020
/* The ROM revision the 2200A reports, and the only 2200/2200A tell. */
#define ISP2200_ROM_REVISION_2200A  0x0004

#define ISP2200_MBOX_NOP                0x0000
#define ISP2200_MBOX_EXEC_FIRMWARE      0x0002
#define ISP2200_MBOX_WRITE_RAM_WORD     0x0004
#define ISP2200_MBOX_READ_RAM_WORD      0x0005
#define ISP2200_MBOX_REGISTER_TEST      0x0006
#define ISP2200_MBOX_VERIFY_CHECKSUM    0x0007
#define ISP2200_MBOX_ABOUT_FIRMWARE     0x0008
#define ISP2200_MBOX_LOAD_RAM           0x0009
#define ISP2200_MBOX_LOAD_RAM_EXTENDED  0x000b
#define ISP2200_MBOX_ABORT_COMMAND      0x0015
#define ISP2200_MBOX_ABORT_TARGET       0x0017
#define ISP2200_MBOX_FIRMWARE_STATUS    0x001f
#define ISP2200_MBOX_GET_ADAPTER_ID     0x0020
#define ISP2200_MBOX_ENHANCED_PORT_DB   0x0047
#define ISP2200_MBOX_INITIATE_LIP       0x0062
#define ISP2200_MBOX_POSITION_MAP       0x0063
#define ISP2200_MBOX_GET_PORT_DB        0x0064
#define ISP2200_MBOX_LIP_RESET          0x006c
#define ISP2200_MBOX_FULL_LOGIN_LIP     0x0072
#define ISP2200_MBOX_PORT_NAME_LIST     0x0075
#define ISP2200_MBOX_GET_ID_LIST        0x007c
#define ISP2200_MBOX_LUN_RESET          0x007e

/* LIP reset's m1 high byte for every loop ID. */
#define ISP2200_LIP_RESET_ALL           0xff
#define ISP2200_MBOX_GET_FW_OPTIONS     0x0028
#define ISP2200_MBOX_SET_FW_OPTIONS     0x0038
#define ISP2200_MBOX_INIT_FIRMWARE      0x0060
#define ISP2200_MBOX_GET_FW_STATE       0x0069

#define ISP2200_MBOX_COMPLETE           0x4000
#define ISP2200_MBOX_INVALID            0x4001
#define ISP2200_MBOX_HOST_INTERFACE_ERROR 0x4002
#define ISP2200_MBOX_TEST_FAILED        0x4003
#define ISP2200_MBOX_COMMAND_ERROR      0x4005
#define ISP2200_MBOX_PARAM_ERROR        0x4006

#define ISP2200_ASYNC_REQUEST_DMA_ERROR  0x8003
#define ISP2200_ASYNC_RESPONSE_DMA_ERROR 0x8004
#define ISP2200_ASYNC_LIP_OCCURRED       0x8010
#define ISP2200_ASYNC_LOOP_UP            0x8011
#define ISP2200_ASYNC_LIP_RESET          0x8013
#define ISP2200_ASYNC_PORT_DB_CHANGED    0x8014

/* Port-database changed m1: every port, not one that changed. */
#define ISP2200_PORT_DB_GLOBAL           0xffff

/* A model constant, deliberately not any vendor firmware release. */
#define ISP2200_FW_VERSION_MAJOR    1
#define ISP2200_FW_VERSION_MINOR    0
#define ISP2200_FW_VERSION_MICRO    0
/* Expanded LUN only: no fabric, no 2K logins. */
#define ISP2200_FW_ATTRIBUTES       0x0002

#define ISP2200_MAX_OUTSTANDING     512

/* Get Firmware State: the only three the loop ever reports. */
#define ISP2200_FW_STATE_CONFIG_WAIT 0
#define ISP2200_FW_STATE_WAIT_AL_PA  1
#define ISP2200_FW_STATE_READY       3

/*
 * How long a loop takes to come up once something starts it. A model
 * constant, long enough for a test to see the initialising state; how
 * long a real loop initialisation takes is unknown.
 */
#define ISP2200_BRINGUP_NS          (100 * SCALE_MS)

/* Loop IDs 0..0x7d are loop ports; 0x7e and up are fabric and special. */
#define ISP2200_MAX_LOOP_ID         0x7d
#define ISP2200_MAX_LUN             255

/* Get Adapter ID m1 while the loop is down: Linux's "loop in transition". */
#define ISP2200_ADAPTER_ID_NOT_READY 7

/* Bit 1 of the stored Set Firmware Options m1 enables LIP-reset events. */
#define ISP2200_FW_OPT_LIP_RESET_EVENTS 0x0002

/* 64K 16-bit words of controller RAM; code starts at word 0x1000. */
#define ISP2200_RAM_WORDS           0x10000

#define ISP2200_ICB_SIZE            96
#define ISP2200_ICB_VERSION         1
#define ISP2200_ICB_OFF_VERSION     0
#define ISP2200_ICB_OFF_OPTIONS     2
#define ISP2200_ICB_OFF_HARD_ADDR   20
#define ISP2200_ICB_OFF_XOPTIONS    64
#define ISP2200_ICB_OFF_RQST_LEN    36
#define ISP2200_ICB_OFF_RESP_LEN    38
#define ISP2200_ICB_OFF_RQST_BASE   40
#define ISP2200_ICB_OFF_RESP_BASE   48
#define ISP2200_ICB_OPT_HARD_ADDR   0x0001
#define ISP2200_ICB_OPT_FAST_POST   0x0008
#define ISP2200_ICB_OPT_PORT_DB_EVENTS 0x0100
/* Extended options: bits 3:0 the reduced-completion mode, 6:4 topology. */
#define ISP2200_ICB_XOPT_TIMER_MASK 0x000f
#define ISP2200_ICB_XOPT_CONN(x)    (((x) >> 4) & 7)
#define ISP2200_CONN_LOOP           0
#define ISP2200_CONN_LOOP_PREFERRED 2
#define ISP2200_CONN_P2P_PREFERRED  3

/* The port database the Get Port Database command returns. */
#define ISP2200_PDB_SIZE            128
#define ISP2200_PDB_OFF_MASTER      2
#define ISP2200_PDB_OFF_SLAVE       3
#define ISP2200_PDB_OFF_HARD_ADDR   6
#define ISP2200_PDB_OFF_PORT_ID     8
#define ISP2200_PDB_OFF_NODE_NAME   12
#define ISP2200_PDB_OFF_PORT_NAME   20
#define ISP2200_PDB_OFF_PRLI_SVC3   78
#define ISP2200_PDB_OFF_LOOP_ID     80
#define ISP2200_PDB_LOGGED_IN       6
#define ISP2200_PDB_TARGET_ROLE     0x10

#define ISP2200_POSITION_MAP_SIZE   128
#define ISP2200_ID_LIST_ENTRY       4
#define ISP2200_NAME_LIST_ENTRY     10
/* Port Name List options OpenBSD sets: include initiators, all loop IDs. */
#define ISP2200_NAME_LIST_OPTIONS   0x000a

#define ISP2200_ENTRY_SIZE          64

#define ISP2200_ENTRY_OFF_TYPE      0
#define ISP2200_ENTRY_OFF_COUNT     1
#define ISP2200_ENTRY_OFF_STATUS    3

#define ISP2200_ENTRY_CONTINUATION      0x02
#define ISP2200_ENTRY_STATUS            0x03
#define ISP2200_ENTRY_MARKER            0x04
#define ISP2200_ENTRY_CONTINUATION_A64  0x0a
#define ISP2200_ENTRY_COMMAND           0x11
#define ISP2200_ENTRY_COMMAND_LIST      0x15
#define ISP2200_ENTRY_COMMAND_A64       0x19

/* Entry status flags a response entry can carry. */
#define ISP2200_ENTRY_FLAG_BAD_TYPE     0x04
#define ISP2200_ENTRY_FLAG_BAD_PARAM    0x08
#define ISP2200_ENTRY_FLAG_BAD_COUNT    0x10
#define ISP2200_ENTRY_FLAG_BAD_ORDER    0x20
#define ISP2200_ENTRY_FLAG_DMA_ERROR    0x40

#define ISP2200_CMD_OFF_HANDLE      4
#define ISP2200_CMD_OFF_TARGET      9   /* High byte of the field at 8. */
#define ISP2200_CMD_OFF_LUN         10
#define ISP2200_CMD_OFF_CONTROL     12
#define ISP2200_CMD_OFF_TIMEOUT     16
#define ISP2200_CMD_OFF_SEGCOUNT    18
#define ISP2200_CMD_OFF_CDB         20
#define ISP2200_CMD_CDB_LEN         16
#define ISP2200_CMD_OFF_BYTECOUNT   36
#define ISP2200_CMD_OFF_SEGS        40
#define ISP2200_CMD_OFF_LIST_TYPE   40
#define ISP2200_CMD_OFF_LIST_ADDR   46

#define ISP2200_CONTROL_DATA_IN     0x0020
#define ISP2200_CONTROL_DATA_OUT    0x0040

#define ISP2200_SEG32_SIZE          8
#define ISP2200_SEG64_SIZE          12
#define ISP2200_CMD_SEGS32          3
#define ISP2200_CMD_SEGS64          2
#define ISP2200_CONT_OFF_SEGS32     8
#define ISP2200_CONT_SEGS32         7
#define ISP2200_CONT_OFF_SEGS64     4
#define ISP2200_CONT_SEGS64         5

#define ISP2200_STATUS_OFF_HANDLE       4
#define ISP2200_STATUS_OFF_SCSI_STATUS  8
#define ISP2200_STATUS_OFF_COMPLETION   10
#define ISP2200_STATUS_OFF_SENSE_LEN    18
#define ISP2200_STATUS_OFF_RESIDUAL     20
#define ISP2200_STATUS_OFF_SENSE        32
#define ISP2200_STATUS_SENSE_MAX        32
#define ISP2200_STATUS_OFF_SIGNATURE    60
/* What Linux writes over the last word of each response it has read. */
#define ISP2200_RESPONSE_PROCESSED      0xdeaddead

#define ISP2200_SCSI_STATUS_SENSE_VALID 0x0200
#define ISP2200_SCSI_STATUS_RESID_UNDER 0x0800

#define ISP2200_COMPLETION_COMPLETE         0x0000
#define ISP2200_COMPLETION_DMA_ERROR        0x0002
#define ISP2200_COMPLETION_RESET            0x0004
#define ISP2200_COMPLETION_ABORTED          0x0005
#define ISP2200_COMPLETION_UNDERRUN         0x0015
#define ISP2200_COMPLETION_PORT_UNAVAILABLE 0x0028
#define ISP2200_COMPLETION_LOGGED_OUT       0x0029

/*
 * Events waiting for the host to release the result before them. No
 * result is ever dropped, so the queue is sized for every event that can
 * wait: the host interface raises two kinds itself, each queued at most
 * once (see isp2200_raise_event()), and the loop queues at most one
 * bring-up's three, because a new bring-up first takes back the events
 * of any earlier one still queued (see isp2200_bringup()). That is five;
 * the queue keeps a power of two.
 */
#define ISP2200_EVENTS_REACHABLE    5
#define ISP2200_EVENTS_MAX          8

/*
 * Response entries waiting for the host to clear the latch, release the
 * semaphore or make ring room. Every request entry produces at most one,
 * and consumption stops once the announced command budget is held, so the
 * queue never needs more.
 */
#define ISP2200_HELD_MAX            ISP2200_MAX_OUTSTANDING

/* One SCSI command as the host interface hands it to the FC service. */
typedef struct ISP2200Segment {
    uint64_t addr;
    uint32_t len;
} ISP2200Segment;

typedef struct ISP2200Command {
    uint32_t handle;
    uint8_t loop_id;
    uint16_t lun;
    uint16_t control;
    uint8_t cdb[ISP2200_CMD_CDB_LEN];
    uint32_t byte_count;
    uint32_t nsegs;
    ISP2200Segment *segs;
} ISP2200Command;

/* One command in flight on a loop port, behind sreq->hba_private. */
typedef struct ISP2200Request {
    QLogicISP2200State *dev;
    SCSIRequest *sreq;
    QEMUSGList qsg;
    uint64_t described;         /* Buffer bytes the segments named. */
    uint64_t transferred;
    uint64_t sg_offset;
    uint32_t sg_index;
    uint32_t handle;
    /*
     * A recovery retires this request: it owes the host retire_completion
     * whether the backend finishes it or the cancellation does. Transient,
     * set and cleared inside one recovery.
     */
    bool retiring;
    bool cancel_issued;
    uint16_t retire_completion;
    /*
     * A reset or re-initialisation is cancelling every request: this one
     * moves no more data and posts nothing. Transient, set and cleared
     * inside one cancellation pass.
     */
    bool dropping;
    QTAILQ_ENTRY(ISP2200Request) next;
} ISP2200Request;

struct QLogicISP2200State {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1: an alias of io. */

    /* The loop: every device on it is a LUN of the port at its target. */
    SCSIBus bus;

    uint16_t csr;
    bool csr_reset_visible;     /* Soft reset bit reads set until a read. */
    uint16_t icr;
    uint16_t semaphore;
    uint16_t nvram;
    uint16_t gpio_data;
    uint16_t gpio_enable;

    bool risc_int;      /* The RISC interrupt latch, ISR bit 3. */
    bool host_int;      /* HCCR bit 7: the doorbell, until cleared. */
    bool risc_reset;    /* HCCR bit 6. */
    bool risc_paused;   /* HCCR bit 5. */

    /*
     * Two banks: commands read what the host last wrote, the host reads
     * what the firmware last wrote. A host write lands in both, so a
     * mailbox reads back until a result replaces it, and an input the
     * host did not rewrite keeps its last value.
     */
    uint16_t mbox_in[ISP2200_MAILBOX_COUNT];
    uint16_t mbox_out[ISP2200_MAILBOX_COUNT];

    /*
     * Open from a mailbox 0 write until the doorbell or a reset. Inside
     * it, a mailbox 4 or 5 write is a command input, never a ring pointer.
     */
    bool cmd_window;
    /*
     * Doorbell rung but the command not yet run: the RISC is paused, or a
     * completion already waits in the slot. One latch, as HCCR bit 7 is,
     * so a second doorbell before the command runs adds nothing.
     */
    bool cmd_pending;

    bool fw_running;
    uint16_t fw_options[3];
    uint16_t risc_ram[ISP2200_RAM_WORDS];

    /* The accepted initialisation control block, for the FC service. */
    uint8_t icb[ISP2200_ICB_SIZE];
    bool queues_init;
    uint64_t rqst_base;
    uint64_t resp_base;
    uint16_t rqst_len;
    uint16_t resp_len;
    uint16_t rqst_in;   /* Host producer. */
    uint16_t rqst_out;  /* Adapter consumer. */
    uint16_t resp_in;   /* Adapter producer. */
    uint16_t resp_out;  /* Host consumer. */

    /*
     * A mailbox completion waiting for the host to release the result
     * before it. It goes ahead of every queued event.
     */
    bool completion_held;
    uint16_t completion[ISP2200_MAILBOX_COUNT];
    uint32_t event_count;
    uint16_t events[ISP2200_EVENTS_MAX][ISP2200_MAILBOX_COUNT];

    uint32_t held_head;
    uint32_t held_count;
    uint8_t held[ISP2200_HELD_MAX][ISP2200_ENTRY_SIZE];

    /*
     * The ring walk steps its cursor only after it has dispatched an
     * entry; a nested walk would issue the same entry twice, so the walk
     * refuses re-entry and repeats on the way out. Transient.
     */
    bool processing;
    bool reprocess;

    /*
     * The walk stopped because the command budget is held. Only that stop
     * resumes on its own; a refused read or a partly published command
     * waits for the host's next producer write. Derived from the held
     * count on load.
     */
    bool rqst_budget_stall;

    /*
     * Initialise-firmware accepted a loop connection mode; until then, or
     * after a reset, nothing brings the loop up.
     */
    bool loop_active;
    uint16_t loop_state;    /* What Get Firmware State reports. */
    bool has_loop_id;
    uint8_t loop_id;        /* The adapter's own, chosen at initialisation. */
    bool bringup_lip_reset; /* The running bring-up came from a LIP reset. */
    QEMUTimer *bringup_timer;

    /* Commands in flight on the loop's devices. */
    QTAILQ_HEAD(, ISP2200Request) requests;
    uint32_t outstanding;

    /*
     * A recovery, reset or unplug is cancelling requests. Cancelling waits
     * for the backend, and a command finishing elsewhere in that wait must
     * not start the request ring again part-way through; the walk stops,
     * notes it, and resumes once the recovery is over. Transient.
     */
    bool recovering;
    bool rqst_recovery_stall;

    /*
     * What reached the loop service from the host interface, read by
     * qtests through QOM properties. Not guest-visible and not migrated.
     */
    uint32_t fc_submissions;
    ISP2200Command fc_last;
    uint64_t fc_last_bytes;
};

static void isp2200_update_irq(QLogicISP2200State *s)
{
    bool level = s->risc_int && (s->icr & ISP2200_ICR_MASK) ==
                                ISP2200_ICR_MASK;

    trace_qlogic_isp2200_irq(level);
    pci_set_irq(PCI_DEVICE(s), level);
}

static bool isp2200_bus_master(QLogicISP2200State *s)
{
    PCIDevice *pci = PCI_DEVICE(s);

    return pci_get_word(pci->config + PCI_COMMAND) & PCI_COMMAND_MASTER;
}

/* Every adapter DMA depends on the host having enabled bus mastering. */
static MemTxResult isp2200_dma_read(QLogicISP2200State *s, uint64_t addr,
                                    void *buf, dma_addr_t len)
{
    if (!isp2200_bus_master(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: DMA read of 0x%" PRIx64 " refused: "
                      "bus mastering is disabled\n", addr);
        return MEMTX_ERROR;
    }
    return pci_dma_read(PCI_DEVICE(s), addr, buf, len);
}

static MemTxResult isp2200_dma_write(QLogicISP2200State *s, uint64_t addr,
                                     const void *buf, dma_addr_t len)
{
    if (!isp2200_bus_master(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: DMA write of 0x%" PRIx64 " refused: "
                      "bus mastering is disabled\n", addr);
        return MEMTX_ERROR;
    }
    return pci_dma_write(PCI_DEVICE(s), addr, buf, len);
}

/* Mapping D: m2 bits 31:16, m3 bits 15:0, m6 bits 63:48, m7 bits 47:32. */
static uint64_t isp2200_mbox_addr(const uint16_t *mb)
{
    return ((uint64_t)mb[6] << 48) | ((uint64_t)mb[7] << 32) |
           ((uint64_t)mb[2] << 16) | mb[3];
}

static void isp2200_drop_queued(QLogicISP2200State *s)
{
    s->completion_held = false;
    s->event_count = 0;
    s->held_head = 0;
    s->held_count = 0;
}

static void isp2200_clear_queues(QLogicISP2200State *s)
{
    s->queues_init = false;
    s->rqst_base = 0;
    s->resp_base = 0;
    s->rqst_len = 0;
    s->resp_len = 0;
    s->rqst_in = 0;
    s->rqst_out = 0;
    s->resp_in = 0;
    s->resp_out = 0;
    memset(s->icb, 0, sizeof(s->icb));
}

static void isp2200_cancel_all(QLogicISP2200State *s);
static void isp2200_loop_reset(QLogicISP2200State *s);

/*
 * HCCR reset and the CSR soft reset hold the RISC the same way. The
 * firmware goes with it: every supported driver downloads again after a
 * reset, and a guest that expects firmware to survive one is unsupported.
 * So does the loop, and every command in flight is cancelled without a
 * status: the rings it would post to are gone.
 */
static void isp2200_hold_risc(QLogicISP2200State *s)
{
    s->risc_reset = true;
    s->risc_paused = false;
    s->cmd_window = false;
    s->cmd_pending = false;

    isp2200_cancel_all(s);
    isp2200_loop_reset(s);

    memset(s->mbox_in, 0, sizeof(s->mbox_in));
    memset(s->mbox_out, 0, sizeof(s->mbox_out));
    s->mbox_out[0] = ISP2200_MBOX_BUSY;

    s->fw_running = false;
    memset(s->fw_options, 0, sizeof(s->fw_options));
    memset(s->risc_ram, 0, sizeof(s->risc_ram));

    isp2200_clear_queues(s);
    /* In-flight work posts nothing: the rings it would post to are gone. */
    isp2200_drop_queued(s);
    s->rqst_budget_stall = false;
}

/* The mailbox image the ROM code leaves once it has booted. */
static void isp2200_boot_rom(QLogicISP2200State *s)
{
    memset(s->mbox_out, 0, sizeof(s->mbox_out));
    s->mbox_out[1] = ISP2200_PRODUCT_ID_1;
    s->mbox_out[2] = ISP2200_PRODUCT_ID_2;
    s->mbox_out[3] = ISP2200_PRODUCT_ID_3;
    s->mbox_out[7] = ISP2200_ROM_REVISION_2200A;
}

static void isp2200_soft_reset(QLogicISP2200State *s)
{
    trace_qlogic_isp2200_soft_reset();

    isp2200_hold_risc(s);
    s->csr = 0;
    s->csr_reset_visible = true;
    s->icr = 0;
    s->semaphore = 0;
    s->nvram = 0;
    s->risc_int = false;
    s->host_int = false;
    isp2200_update_irq(s);
}

/*
 * Power-on and PCI reset leave the RISC running its ROM code, already
 * showing the product-ID image: OpenBSD reads it at attach before it
 * resets anything.
 */
static void isp2200_reset_hold(Object *obj, ResetType type)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(obj);

    isp2200_soft_reset(s);
    s->csr_reset_visible = false;
    s->gpio_data = 0;
    s->gpio_enable = 0;
    s->risc_reset = false;
    memset(s->mbox_out, 0, sizeof(s->mbox_out));
    isp2200_boot_rom(s);

    /* What the stand-in service saw belongs to this power cycle only. */
    s->fc_submissions = 0;
    s->fc_last_bytes = 0;
    g_free(s->fc_last.segs);
    memset(&s->fc_last, 0, sizeof(s->fc_last));
}

static void isp2200_process_requests(QLogicISP2200State *s);

static uint16_t isp2200_resp_space(QLogicISP2200State *s)
{
    uint16_t used = (s->resp_in + s->resp_len - s->resp_out) % s->resp_len;

    /* One slot stays empty so that in == out always means empty. */
    return s->resp_len - 1 - used;
}

static void isp2200_raise_event(QLogicISP2200State *s, uint16_t code);

/*
 * Writes held response entries into the ring, as many as fit, then
 * advances response-in once and sets the latch once. A refused write
 * drops its entry and queues an event in its place.
 */
static void isp2200_post_entries(QLogicISP2200State *s)
{
    uint16_t space = isp2200_resp_space(s);
    uint16_t written = 0;

    while (s->held_count > 0 && written < space) {
        uint16_t slot = (s->resp_in + written) % s->resp_len;
        uint64_t addr = s->resp_base + (uint64_t)slot * ISP2200_ENTRY_SIZE;

        /* Whole 64 bytes: Linux finds new entries by their last word. */
        if (isp2200_dma_write(s, addr, s->held[s->held_head],
                              ISP2200_ENTRY_SIZE) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: response entry write to 0x%"
                          PRIx64 " refused; entry dropped\n", addr);
            s->held_head = (s->held_head + 1) % ISP2200_HELD_MAX;
            s->held_count--;
            isp2200_raise_event(s, ISP2200_ASYNC_RESPONSE_DMA_ERROR);
            break;
        }
        s->held_head = (s->held_head + 1) % ISP2200_HELD_MAX;
        s->held_count--;
        written++;
    }

    if (written > 0) {
        s->resp_in = (s->resp_in + written) % s->resp_len;
        trace_qlogic_isp2200_response_ring(s->resp_in, s->resp_out, written);
        s->risc_int = true;
        isp2200_update_irq(s);
    }
}

static void isp2200_post_mailbox(QLogicISP2200State *s, const uint16_t *mb)
{
    memcpy(s->mbox_out, mb, sizeof(s->mbox_out));
    trace_qlogic_isp2200_post_mailbox(s->mbox_out[0]);
    s->semaphore |= ISP2200_SEMAPHORE_LOCK;
    s->risc_int = true;
    isp2200_update_irq(s);
}

/*
 * Nothing is written while the latch is set or the semaphore is held:
 * Linux clears the latch after walking the ring, so an entry landing in
 * between would lose its interrupt. A mailbox result goes first and
 * alone; response entries wait for the next pass after its release.
 */
static void isp2200_service_posts(QLogicISP2200State *s)
{
    for (;;) {
        if (s->risc_reset || s->risc_paused || s->risc_int ||
            (s->semaphore & ISP2200_SEMAPHORE_LOCK)) {
            return;
        }
        if (s->completion_held) {
            s->completion_held = false;
            isp2200_post_mailbox(s, s->completion);
            return;
        }
        if (s->event_count > 0) {
            isp2200_post_mailbox(s, s->events[0]);
            s->event_count--;
            memmove(s->events[0], s->events[1],
                    s->event_count * sizeof(s->events[0]));
            return;
        }
        if (!s->queues_init || s->held_count == 0) {
            break;
        }
        isp2200_post_entries(s);
        if (s->risc_int || s->event_count == 0) {
            break;
        }
        /* A refused write left an event to post in its place. */
    }

    /* Room in the budget lets a walk stopped on it resume. */
    if (s->rqst_budget_stall && !s->recovering &&
        s->outstanding + s->held_count < ISP2200_MAX_OUTSTANDING) {
        s->rqst_budget_stall = false;
        isp2200_process_requests(s);
    }
}

/*
 * The payload of the events the host interface raises itself is zero.
 * Such an event that finds an identical one still queued adds nothing:
 * the queued one reports the same failure, and the host reads it once it
 * has released the result before. A guest that keeps retrying a refused
 * ring therefore cannot grow the queue.
 */
static void isp2200_push_event(QLogicISP2200State *s, uint16_t code,
                               uint16_t m1)
{
    uint16_t mb[ISP2200_MAILBOX_COUNT] = { code, m1 };

    /* Bounded by its sources; see ISP2200_EVENTS_MAX. */
    assert(s->event_count < ISP2200_EVENTS_MAX);
    trace_qlogic_isp2200_event(code);
    memcpy(s->events[s->event_count++], mb, sizeof(mb));
}

static void isp2200_raise_event(QLogicISP2200State *s, uint16_t code)
{
    uint16_t mb[ISP2200_MAILBOX_COUNT] = { code };
    uint32_t i;

    for (i = 0; i < s->event_count; i++) {
        if (!memcmp(s->events[i], mb, sizeof(mb))) {
            trace_qlogic_isp2200_event_waiting(code);
            return;
        }
    }
    isp2200_push_event(s, code, 0);
}

static void isp2200_hold_entry(QLogicISP2200State *s, const uint8_t *entry)
{
    uint32_t tail = (s->held_head + s->held_count) % ISP2200_HELD_MAX;

    /* Consumption stops at the bound, so there is always a slot here. */
    assert(s->held_count < ISP2200_HELD_MAX);
    memcpy(s->held[tail], entry, ISP2200_ENTRY_SIZE);
    s->held_count++;
}

static void isp2200_complete(QLogicISP2200State *s, uint32_t handle,
                             uint16_t scsi_status, uint16_t completion,
                             uint32_t residual, const uint8_t *sense,
                             unsigned sense_len)
{
    uint8_t entry[ISP2200_ENTRY_SIZE] = { 0 };

    trace_qlogic_isp2200_complete(handle, scsi_status, completion);

    sense_len = MIN(sense_len, ISP2200_STATUS_SENSE_MAX);
    if (sense_len) {
        memcpy(entry + ISP2200_STATUS_OFF_SENSE, sense, sense_len);
    }
    /*
     * Linux would take an entry ending in its own processed mark for one
     * it had already read, and never see it. Sense that ends that way
     * loses its last word instead.
     */
    if (ldl_le_p(entry + ISP2200_STATUS_OFF_SIGNATURE) ==
        ISP2200_RESPONSE_PROCESSED) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: sense for handle 0x%08x cut to 28 "
                      "bytes: its last word is Linux's processed mark\n",
                      handle);
        sense_len = ISP2200_STATUS_OFF_SIGNATURE - ISP2200_STATUS_OFF_SENSE;
        stl_le_p(entry + ISP2200_STATUS_OFF_SIGNATURE, 0);
    }
    entry[ISP2200_ENTRY_OFF_TYPE] = ISP2200_ENTRY_STATUS;
    entry[ISP2200_ENTRY_OFF_COUNT] = 1;
    stl_le_p(entry + ISP2200_STATUS_OFF_HANDLE, handle);
    stw_le_p(entry + ISP2200_STATUS_OFF_SCSI_STATUS, scsi_status);
    stw_le_p(entry + ISP2200_STATUS_OFF_COMPLETION, completion);
    stw_le_p(entry + ISP2200_STATUS_OFF_SENSE_LEN, sense_len);
    stl_le_p(entry + ISP2200_STATUS_OFF_RESIDUAL, residual);

    isp2200_hold_entry(s, entry);
    /* A recovery's retirements post once it is over, after its result. */
    if (!s->recovering) {
        isp2200_service_posts(s);
    }
}

/* A malformed request goes back as itself, its entry status flagged. */
static void isp2200_reject_entry(QLogicISP2200State *s, const uint8_t *req,
                                 uint8_t flag)
{
    uint8_t entry[ISP2200_ENTRY_SIZE];

    qemu_log_mask(LOG_GUEST_ERROR,
                  "qlogic-isp2200: request entry type 0x%02x at index %u "
                  "returned with entry status 0x%02x\n",
                  req[ISP2200_ENTRY_OFF_TYPE], s->rqst_out, flag);

    memcpy(entry, req, sizeof(entry));
    entry[ISP2200_ENTRY_OFF_STATUS] = flag;
    isp2200_hold_entry(s, entry);
    isp2200_service_posts(s);
}

/*
 * The private loop.
 *
 * Loop ID n is the standard table's AL_PA n: 0xef at 0 down to 0x01 at
 * 0x7d, then 0x00 for the fabric port. A private loop has domain and area
 * 0, so a port ID is its AL_PA alone.
 */
static const uint8_t isp2200_alpa[ISP2200_MAX_LOOP_ID + 2] = {
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
    0x10, 0x0f, 0x08, 0x04, 0x02, 0x01, 0x00,
};

/*
 * A realized device at @loop_id, which is then a port; NULL when none is.
 * A device being plugged is not realized yet, and one being unplugged has
 * already left the bus.
 */
static SCSIDevice *isp2200_port(QLogicISP2200State *s, unsigned loop_id)
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

/* A port other than the adapter, at a loop ID the loop has. */
static SCSIDevice *isp2200_target(QLogicISP2200State *s, unsigned loop_id)
{
    if (loop_id > ISP2200_MAX_LOOP_ID ||
        (s->has_loop_id && loop_id == s->loop_id)) {
        return NULL;
    }
    return isp2200_port(s, loop_id);
}

static bool isp2200_loop_ready(QLogicISP2200State *s)
{
    return s->loop_state == ISP2200_FW_STATE_READY;
}

static bool isp2200_bringup_event(uint16_t code)
{
    switch (code) {
    case ISP2200_ASYNC_LIP_OCCURRED:
    case ISP2200_ASYNC_LOOP_UP:
    case ISP2200_ASYNC_LIP_RESET:
    case ISP2200_ASYNC_PORT_DB_CHANGED:
        return true;
    default:
        return false;
    }
}

/* Takes back the queued, not yet posted, events of earlier bring-ups. */
static void isp2200_drop_bringup_events(QLogicISP2200State *s)
{
    uint32_t i, kept = 0;

    for (i = 0; i < s->event_count; i++) {
        if (isp2200_bringup_event(s->events[i][0])) {
            continue;
        }
        if (kept != i) {
            memcpy(s->events[kept], s->events[i], sizeof(s->events[0]));
        }
        kept++;
    }
    s->event_count = kept;
}

/*
 * Starts the loop coming up: state 1 until the timer fires. A new trigger
 * restarts the timer and takes back whatever an earlier bring-up queued
 * and has not posted, so the queue holds at most one bring-up's events
 * and each bring-up posts a whole set. An event already posted stays
 * until the host releases it.
 */
static void isp2200_bringup(QLogicISP2200State *s, bool lip_reset)
{
    if (!s->loop_active) {
        return;
    }
    trace_qlogic_isp2200_bringup(lip_reset);
    s->loop_state = ISP2200_FW_STATE_WAIT_AL_PA;
    s->bringup_lip_reset = lip_reset;
    isp2200_drop_bringup_events(s);
    timer_mod(s->bringup_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ISP2200_BRINGUP_NS);
}

/*
 * The loop is up. The events are a model choice conditioned on the two
 * option bits the drivers set: LIP (or LIP reset), loop up, and port
 * database changed for every port. Their payloads are zero but for that
 * last m1, which Linux reads as a global update.
 */
static void isp2200_bringup_done(void *opaque)
{
    QLogicISP2200State *s = opaque;
    uint16_t options = lduw_le_p(s->icb + ISP2200_ICB_OFF_OPTIONS);

    /* With every loop ID taken the adapter never gets one, nor a loop. */
    if (!s->has_loop_id) {
        trace_qlogic_isp2200_loop_full();
        return;
    }

    s->loop_state = ISP2200_FW_STATE_READY;
    trace_qlogic_isp2200_loop_ready(s->loop_id);
    if (!s->bringup_lip_reset) {
        isp2200_push_event(s, ISP2200_ASYNC_LIP_OCCURRED, 0);
    } else if (s->fw_options[0] & ISP2200_FW_OPT_LIP_RESET_EVENTS) {
        isp2200_push_event(s, ISP2200_ASYNC_LIP_RESET, 0);
    }
    isp2200_push_event(s, ISP2200_ASYNC_LOOP_UP, 0);
    if (options & ISP2200_ICB_OPT_PORT_DB_EVENTS) {
        isp2200_push_event(s, ISP2200_ASYNC_PORT_DB_CHANGED,
                           ISP2200_PORT_DB_GLOBAL);
    }
    isp2200_service_posts(s);
}

static void isp2200_loop_reset(QLogicISP2200State *s)
{
    timer_del(s->bringup_timer);
    s->loop_active = false;
    s->loop_state = ISP2200_FW_STATE_CONFIG_WAIT;
    s->has_loop_id = false;
    s->loop_id = 0;
    s->bringup_lip_reset = false;
}

/*
 * Initialise-firmware starts the loop from its control block. The adapter
 * takes the requested hard address when it asks for one, the address is
 * a loop ID and no port holds it, and otherwise the lowest loop ID no
 * port holds; real loops settle this in loop initialisation, and which
 * soft address a port then wins is unknown. The choice holds until the
 * next initialisation or reset.
 */
static void isp2200_loop_init(QLogicISP2200State *s)
{
    uint16_t options = lduw_le_p(s->icb + ISP2200_ICB_OFF_OPTIONS);
    uint16_t hard = lduw_le_p(s->icb + ISP2200_ICB_OFF_HARD_ADDR);
    uint16_t xopts = lduw_le_p(s->icb + ISP2200_ICB_OFF_XOPTIONS);
    unsigned id;

    isp2200_loop_reset(s);
    isp2200_drop_bringup_events(s);

    if (xopts & ISP2200_ICB_XOPT_TIMER_MASK) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: reduced-completion mode %u is not "
                      "modelled; every command posts a full status entry\n",
                      xopts & ISP2200_ICB_XOPT_TIMER_MASK);
    }

    /* Point-to-point preferred finds no partner and falls back to loop. */
    switch (ISP2200_ICB_XOPT_CONN(xopts)) {
    case ISP2200_CONN_LOOP:
    case ISP2200_CONN_LOOP_PREFERRED:
    case ISP2200_CONN_P2P_PREFERRED:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: connection mode %u leaves the loop "
                      "down: only a loop is modelled\n",
                      ISP2200_ICB_XOPT_CONN(xopts));
        return;
    }

    if ((options & ISP2200_ICB_OPT_HARD_ADDR) &&
        hard <= ISP2200_MAX_LOOP_ID && !isp2200_port(s, hard)) {
        s->has_loop_id = true;
        s->loop_id = hard;
    } else {
        for (id = 0; id <= ISP2200_MAX_LOOP_ID; id++) {
            if (!isp2200_port(s, id)) {
                s->has_loop_id = true;
                s->loop_id = id;
                break;
            }
        }
    }
    if (s->has_loop_id) {
        trace_qlogic_isp2200_loop_id(s->loop_id);
    }

    s->loop_active = true;
    isp2200_bringup(s, false);
}

static uint16_t isp2200_fc_fw_state(QLogicISP2200State *s)
{
    return s->loop_state;
}

/* Detached from hba_private before the unref: the request can outlive us. */
static void isp2200_free_request(ISP2200Request *req)
{
    QLogicISP2200State *s = req->dev;

    QTAILQ_REMOVE(&s->requests, req, next);
    s->outstanding--;
    if (req->sreq != NULL) {
        req->sreq->hba_private = NULL;
        scsi_req_unref(req->sreq);
        req->sreq = NULL;
    }
    qemu_sglist_destroy(&req->qsg);
    g_free(req);
}

static ISP2200Request *isp2200_new_request(QLogicISP2200State *s,
                                           uint32_t handle)
{
    ISP2200Request *req = g_new0(ISP2200Request, 1);

    req->dev = s;
    req->handle = handle;
    /* A hint only: a segment list can name 65535. */
    pci_dma_sglist_init(&req->qsg, PCI_DEVICE(s), 2);
    QTAILQ_INSERT_TAIL(&s->requests, req, next);
    s->outstanding++;
    return req;
}

/*
 * Every request in flight, cancelled with no status. A cancellation waits
 * for its backend, which may finish others on the way, so all are marked
 * first: one that finishes in that wait moves no data and posts nothing.
 * The head is taken afresh each time. A request whose callback never
 * comes -- it is no longer queued in the SCSI layer -- is let go here.
 */
static void isp2200_cancel_all(QLogicISP2200State *s)
{
    bool was_recovering = s->recovering;
    ISP2200Request *req;

    s->recovering = true;
    QTAILQ_FOREACH(req, &s->requests, next) {
        req->dropping = true;
    }
    while ((req = QTAILQ_FIRST(&s->requests)) != NULL) {
        trace_qlogic_isp2200_request_cancelled(req->handle);
        if (!req->sreq->enqueued) {
            isp2200_free_request(req);
            continue;
        }
        scsi_req_cancel(req->sreq);
    }
    s->recovering = was_recovering;
}

/* A walk the recovery held off runs once the recovery is over. */
static void isp2200_end_recovery(QLogicISP2200State *s, bool was_recovering)
{
    s->recovering = was_recovering;
    if (!s->recovering) {
        isp2200_service_posts(s);
        if (s->rqst_recovery_stall) {
            s->rqst_recovery_stall = false;
            isp2200_process_requests(s);
        }
    }
}

/*
 * One segment into the transfer. The segments name the host's buffer and
 * the command names how much of it the target fills, so only that much
 * is offered to the SCSI layer; the whole named length still counts
 * toward the residual.
 */
static void isp2200_add_segment(ISP2200Request *req, uint64_t max,
                                uint64_t base, uint32_t len)
{
    uint64_t room = req->described < max ? max - req->described : 0;
    uint64_t placed = MIN(len, room);

    req->described += len;
    if (placed > 0) {
        qemu_sglist_add(&req->qsg, base, placed);
    }
}

/*
 * A command from the host interface onto the loop. With no loop -- the
 * connection mode left it down, or the adapter never took a loop ID --
 * every command completes "port unavailable" with no SCSI status, as a
 * disconnected adapter's do. So does one to a loop ID with no port, the
 * adapter's own, or one past 0x7d; which code real firmware returns for
 * those is unknown. Commands to present ports run while the loop comes
 * up. A
 * port without the LUN answers through the SCSI layer's target-level
 * handling, as an FCP target with a missing LUN does, and a LUN past 255
 * gets the same CHECK CONDITION without reaching it.
 */
static void isp2200_fc_submit(QLogicISP2200State *s, ISP2200Command *cmd)
{
    SCSIDevice *sdev = NULL;
    ISP2200Request *req;
    uint64_t described = 0;
    uint32_t i;

    trace_qlogic_isp2200_submit(cmd->handle, cmd->loop_id, cmd->lun,
                                cmd->cdb[0], cmd->nsegs);

    s->fc_submissions++;
    g_free(s->fc_last.segs);
    s->fc_last = *cmd;
    s->fc_last.segs = NULL;
    for (i = 0; i < cmd->nsegs; i++) {
        described += cmd->segs[i].len;
    }
    s->fc_last_bytes = described;

    /* The LUN's device, or another of the port's when it has no such LUN. */
    if (s->loop_active && s->has_loop_id &&
        isp2200_target(s, cmd->loop_id)) {
        sdev = scsi_device_find(&s->bus, 0, cmd->loop_id, cmd->lun);
    }
    if (!sdev) {
        isp2200_complete(s, cmd->handle, 0,
                         ISP2200_COMPLETION_PORT_UNAVAILABLE, 0, NULL, 0);
        goto out;
    }

    if (cmd->lun > ISP2200_MAX_LUN) {
        uint8_t sense[SCSI_SENSE_LEN];
        int sense_len = scsi_build_sense(sense,
                                         SENSE_CODE(LUN_NOT_SUPPORTED));

        isp2200_complete(s, cmd->handle,
                         CHECK_CONDITION | ISP2200_SCSI_STATUS_SENSE_VALID |
                         (described ? ISP2200_SCSI_STATUS_RESID_UNDER : 0),
                         ISP2200_COMPLETION_COMPLETE,
                         MIN(described, UINT32_MAX), sense, sense_len);
        goto out;
    }

    req = isp2200_new_request(s, cmd->handle);
    /* The command first: its length is what caps the segments. */
    req->sreq = scsi_req_new(sdev, cmd->handle, cmd->lun, cmd->cdb,
                             sizeof(cmd->cdb), req);
    for (i = 0; i < cmd->nsegs; i++) {
        isp2200_add_segment(req, req->sreq->cmd.xfer, cmd->segs[i].addr,
                            cmd->segs[i].len);
    }
    /* Enqueueing may complete the request and free the wrapper. */
    if (scsi_req_enqueue(req->sreq)) {
        scsi_req_continue(req->sreq);
    }

out:
    g_free(cmd->segs);
    cmd->segs = NULL;
}

/*
 * Completion precedence, following what the two drivers decode rather
 * than any firmware evidence: a SCSI status other than GOOD completes
 * 0x0000 carrying the status and any sense, with residual-under when the
 * transfer was short; GOOD and short is underrun 0x0015; GOOD and whole
 * is 0x0000 with no flags.
 */
static void isp2200_scsi_complete(SCSIRequest *sreq, size_t resid)
{
    ISP2200Request *req = sreq->hba_private;
    QLogicISP2200State *s = req->dev;
    uint8_t sense[SCSI_SENSE_BUF_SIZE];
    uint32_t handle = req->handle;
    uint16_t scsi_status = 0, completion = ISP2200_COMPLETION_COMPLETE;
    uint64_t unused;
    int sense_len = 0;

    /* Finished while a reset was cancelling it: it owes nothing. */
    if (req->dropping) {
        isp2200_free_request(req);
        return;
    }
    /* Finished while a recovery was retiring it: it still owes that. */
    if (req->retiring) {
        completion = req->retire_completion;
        isp2200_free_request(req);
        isp2200_complete(s, handle, 0, completion, 0, NULL, 0);
        return;
    }

    /*
     * A buffered transfer counts successful DMA; a direct one, offered
     * only for a write shorter than its command, leaves the count to the
     * SCSI layer. Segments beyond the command's length stay unfilled.
     */
    if (sreq->sg) {
        unused = resid;
        if (req->described > sreq->cmd.xfer) {
            unused += req->described - sreq->cmd.xfer;
        }
    } else {
        unused = MAX(req->described, sreq->cmd.xfer) - req->transferred;
    }

    if (sreq->status != GOOD) {
        scsi_status = sreq->status & 0xff;
        sense_len = scsi_req_get_sense(sreq, sense, sizeof(sense));
        if (sense_len > 0) {
            scsi_status |= ISP2200_SCSI_STATUS_SENSE_VALID;
        } else {
            sense_len = 0;
        }
    } else if (unused) {
        completion = ISP2200_COMPLETION_UNDERRUN;
    }
    if (unused) {
        scsi_status |= ISP2200_SCSI_STATUS_RESID_UNDER;
    }

    /* Out of the budget before the entry joins it. */
    isp2200_free_request(req);
    isp2200_complete(s, handle, scsi_status, completion,
                     MIN(unused, UINT32_MAX), sense, sense_len);
}

/*
 * A retired request posts the completion its recovery owes, with its own
 * handle. Any other cancellation -- a reset, a purge by the SCSI layer --
 * is not one the host asked for and posts nothing.
 */
static void isp2200_scsi_cancel(SCSIRequest *sreq)
{
    ISP2200Request *req = sreq->hba_private;
    QLogicISP2200State *s;
    uint32_t handle;
    uint16_t completion;

    if (req == NULL) {
        return;
    }
    s = req->dev;
    handle = req->handle;
    completion = req->retiring ? req->retire_completion : 0;
    isp2200_free_request(req);
    if (completion) {
        isp2200_complete(s, handle, 0, completion, 0, NULL, 0);
    }
}

/*
 * Retires every request the selector names with @completion: -1 matches
 * any loop ID, LUN or handle, and a non-NULL @sdev only that device's.
 * All are marked first, so one finishing in another's cancellation wait
 * still posts what it owes; then each is cancelled, the head taken afresh
 * because the wait may free others. Entries post once the caller's
 * recovery ends. Mirrors qlogic-isp's retirement, with the marking first.
 */
static unsigned isp2200_retire(QLogicISP2200State *s, int loop_id, int lun,
                               int64_t handle, SCSIDevice *sdev,
                               uint16_t completion)
{
    ISP2200Request *req;
    unsigned retired = 0;
    bool again = true;

    assert(s->recovering);
    QTAILQ_FOREACH(req, &s->requests, next) {
        if (req->retiring ||
            (loop_id >= 0 && req->sreq->dev->id != loop_id) ||
            (lun >= 0 && req->sreq->lun != lun) ||
            (handle >= 0 && req->handle != (uint32_t)handle) ||
            (sdev && req->sreq->dev != sdev)) {
            continue;
        }
        req->retiring = true;
        req->retire_completion = completion;
        retired++;
    }

    while (again) {
        again = false;
        QTAILQ_FOREACH(req, &s->requests, next) {
            if (!req->retiring || req->cancel_issued) {
                continue;
            }
            req->cancel_issued = true;
            trace_qlogic_isp2200_request_retired(req->handle, completion);
            if (req->sreq->enqueued) {
                scsi_req_cancel(req->sreq);
            } else {
                uint32_t h = req->handle;

                isp2200_free_request(req);
                isp2200_complete(s, h, 0, completion, 0, NULL, 0);
            }
            again = true;
            break;
        }
    }
    return retired;
}

/* Resets the devices at @loop_id: every LUN, or only @lun. */
static void isp2200_reset_devices(QLogicISP2200State *s, int loop_id, int lun)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &s->bus.qbus.children, sibling) {
        SCSIDevice *sdev = SCSI_DEVICE(kid->child);

        if ((loop_id < 0 || sdev->id == loop_id) &&
            (lun < 0 || sdev->lun == lun)) {
            device_cold_reset(DEVICE(sdev));
        }
    }
}

/*
 * The recovery commands: abort one command, abort a target, reset one
 * LUN, LIP reset. The completions they post are the public definitions
 * and the reset semantics follow qlogic-isp's; what a real ISP2200 does,
 * including any frames it sends the target, is unknown.
 */
static uint16_t isp2200_recovery(QLogicISP2200State *s, uint16_t cmd,
                                 const uint16_t *in)
{
    bool was_recovering = s->recovering;
    int loop_id = in[1] >> 8;
    uint16_t status = ISP2200_MBOX_COMPLETE;
    unsigned retired;

    if (cmd == ISP2200_MBOX_LIP_RESET && !s->loop_active) {
        return ISP2200_MBOX_COMMAND_ERROR;
    }

    s->recovering = true;
    switch (cmd) {
    /*
     * The entry's handle bytes are read as a little-endian word, like the
     * entry's other fields, and compared with m3:m2, the halves Linux
     * passes. How real firmware compares them is unknown.
     */
    case ISP2200_MBOX_ABORT_COMMAND:
        retired = isp2200_retire(s, loop_id, in[6],
                                 ((uint32_t)in[3] << 16) | in[2], NULL,
                                 ISP2200_COMPLETION_ABORTED);
        if (!retired) {
            status = ISP2200_MBOX_COMMAND_ERROR;
        }
        break;

    case ISP2200_MBOX_ABORT_TARGET:
        retired = isp2200_retire(s, loop_id, -1, -1, NULL,
                                 ISP2200_COMPLETION_RESET);
        isp2200_reset_devices(s, loop_id, -1);
        break;

    case ISP2200_MBOX_LUN_RESET:
        retired = isp2200_retire(s, loop_id, in[2], -1, NULL,
                                 ISP2200_COMPLETION_RESET);
        isp2200_reset_devices(s, loop_id, in[2]);
        break;

    case ISP2200_MBOX_LIP_RESET:
        if (loop_id == ISP2200_LIP_RESET_ALL) {
            loop_id = -1;
        }
        retired = isp2200_retire(s, loop_id, -1, -1, NULL,
                                 ISP2200_COMPLETION_RESET);
        isp2200_reset_devices(s, loop_id, -1);
        isp2200_bringup(s, true);
        break;

    default:
        g_assert_not_reached();
    }
    trace_qlogic_isp2200_recovery(cmd, in[1] >> 8, retired);

    /* The ring resumes, and the retirements post, after the result. */
    s->recovering = was_recovering;
    return status;
}

/*
 * A data DMA the host bridge refused completes the command "DMA error",
 * through the same path as a recovery's retirement so that it posts
 * once, however the cancellation ends. A recovery that already owns the
 * command keeps its own code.
 */
static void isp2200_data_dma_error(ISP2200Request *req, uint64_t addr)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "qlogic-isp2200: data transfer at 0x%" PRIx64 " for handle "
                  "0x%08x refused; the command completes with a DMA error\n",
                  addr, req->handle);
    if (!req->retiring) {
        req->retiring = true;
        req->retire_completion = ISP2200_COMPLETION_DMA_ERROR;
    }
    if (!req->cancel_issued) {
        req->cancel_issued = true;
        scsi_req_cancel(req->sreq);
    }
}

static void isp2200_scsi_transfer_data(SCSIRequest *sreq, uint32_t len)
{
    ISP2200Request *req = sreq->hba_private;
    QLogicISP2200State *s = req->dev;
    uint8_t *buf = scsi_req_get_buf(sreq);
    uint32_t offset = 0;

    /* Left for the reset's own cancellation, which reaches it next. */
    if (req->dropping) {
        return;
    }
    while (offset < len && req->sg_index < req->qsg.nsg) {
        ScatterGatherEntry *sg = &req->qsg.sg[req->sg_index];
        uint32_t count = MIN(len - offset, sg->len - req->sg_offset);
        dma_addr_t addr = sg->base + req->sg_offset;
        MemTxResult result;

        if (sreq->cmd.mode == SCSI_XFER_FROM_DEV) {
            result = isp2200_dma_write(s, addr, buf + offset, count);
        } else {
            result = isp2200_dma_read(s, addr, buf + offset, count);
        }
        if (result != MEMTX_OK) {
            isp2200_data_dma_error(req, addr);
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

    /* A read may drain target bytes beyond the host's buffer. */
    if (offset < len && sreq->cmd.mode == SCSI_XFER_TO_DEV) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: write buffer for handle 0x%08x "
                      "exhausted\n", req->handle);
        return;
    }
    /* Continuing may complete and free the wrapper. */
    scsi_req_continue(sreq);
}

/*
 * A write shorter than its command goes direct, so the target's own
 * buffer is not padded; everything else is buffered through
 * isp2200_scsi_transfer_data(). The direct path's DMA helpers cannot
 * report a refusal: a segment that does not map directly goes through a
 * bounce buffer whose failed read is ignored, and the write would reach
 * the disk with whatever that read left. So a write whose segments the
 * bridge would refuse is buffered too, and its first refused read
 * completes it as any other refused data DMA does. The check is made
 * once, here; a refusal that begins after it is not seen.
 */
static QEMUSGList *isp2200_scsi_get_sg_list(SCSIRequest *sreq)
{
    ISP2200Request *req = sreq->hba_private;
    QLogicISP2200State *s = req->dev;
    int i;

    if (sreq->cmd.mode != SCSI_XFER_TO_DEV ||
        req->qsg.size >= sreq->cmd.xfer || !isp2200_bus_master(s)) {
        return NULL;
    }
    for (i = 0; i < req->qsg.nsg; i++) {
        if (!dma_memory_valid(req->qsg.as, req->qsg.sg[i].base,
                              req->qsg.sg[i].len, DMA_DIRECTION_TO_DEVICE,
                              MEMTXATTRS_UNSPECIFIED)) {
            return NULL;
        }
    }
    return &req->qsg;
}

/* The ID list: four bytes per port, AL_PA, area 0, domain 0, loop ID. */
static uint16_t isp2200_get_id_list(QLogicISP2200State *s, const uint16_t *in,
                                    uint16_t *out)
{
    /* Its own split: m1 bits 31:16, m2 15:0, m3 63:48, m6 47:32. */
    uint64_t addr = ((uint64_t)in[3] << 48) | ((uint64_t)in[6] << 32) |
                    ((uint64_t)in[1] << 16) | in[2];
    uint8_t list[(ISP2200_MAX_LOOP_ID + 1) * ISP2200_ID_LIST_ENTRY];
    unsigned id, n = 0;

    if (!isp2200_loop_ready(s)) {
        return ISP2200_MBOX_COMMAND_ERROR;
    }
    for (id = 0; id <= ISP2200_MAX_LOOP_ID; id++) {
        uint8_t *e = list + n * ISP2200_ID_LIST_ENTRY;

        if (!isp2200_target(s, id)) {
            continue;
        }
        e[0] = isp2200_alpa[id];
        e[1] = 0;
        e[2] = 0;
        e[3] = id;
        n++;
    }
    if (n && isp2200_dma_write(s, addr, list, n * ISP2200_ID_LIST_ENTRY) !=
        MEMTX_OK) {
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }
    out[1] = n;
    return ISP2200_MBOX_COMPLETE;
}

/*
 * OpenBSD's port name list: ten bytes per port, the port name then the
 * loop ID as a little-endian word. OpenBSD decodes this name as a
 * little-endian integer but the port database's as big-endian, and
 * compares the two; the name goes here with its wire-order bytes
 * reversed so both decodes agree. The real firmware's order is unknown.
 */
static uint16_t isp2200_port_name_list(QLogicISP2200State *s,
                                       const uint16_t *in, uint16_t *out)
{
    uint8_t list[(ISP2200_MAX_LOOP_ID + 1) * ISP2200_NAME_LIST_ENTRY];
    unsigned id, n = 0;

    if (!isp2200_loop_ready(s)) {
        return ISP2200_MBOX_COMMAND_ERROR;
    }
    if (in[1] & ~ISP2200_NAME_LIST_OPTIONS) {
        return ISP2200_MBOX_PARAM_ERROR;
    }
    for (id = 0; id <= ISP2200_MAX_LOOP_ID; id++) {
        SCSIDevice *sdev = isp2200_target(s, id);
        uint8_t *e = list + n * ISP2200_NAME_LIST_ENTRY;

        if (!sdev) {
            continue;
        }
        stq_le_p(e, sdev->port_wwn);
        stw_le_p(e + 8, id);
        n++;
    }
    if (n && isp2200_dma_write(s, isp2200_mbox_addr(in), list,
                               n * ISP2200_NAME_LIST_ENTRY) != MEMTX_OK) {
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }
    out[1] = n * ISP2200_NAME_LIST_ENTRY;
    return ISP2200_MBOX_COMPLETE;
}

/*
 * One port's database entry. Every present port is logged in with the
 * FCP target role and class 3 only; the bytes not set here are zero.
 */
static uint16_t isp2200_get_port_db(QLogicISP2200State *s, const uint16_t *in)
{
    unsigned id = in[1] >> 8;
    SCSIDevice *sdev = isp2200_target(s, id);
    uint8_t pd[ISP2200_PDB_SIZE] = { 0 };

    if (!isp2200_loop_ready(s) || !sdev) {
        return ISP2200_MBOX_COMMAND_ERROR;
    }
    pd[ISP2200_PDB_OFF_MASTER] = ISP2200_PDB_LOGGED_IN;
    pd[ISP2200_PDB_OFF_SLAVE] = ISP2200_PDB_LOGGED_IN;
    pd[ISP2200_PDB_OFF_HARD_ADDR] = isp2200_alpa[id];
    /* Domain, a reserved byte, AL_PA, area. */
    pd[ISP2200_PDB_OFF_PORT_ID + 2] = isp2200_alpa[id];
    stq_be_p(pd + ISP2200_PDB_OFF_NODE_NAME, sdev->wwn);
    stq_be_p(pd + ISP2200_PDB_OFF_PORT_NAME, sdev->port_wwn);
    /* Both drivers find the target role in the low byte. */
    pd[ISP2200_PDB_OFF_PRLI_SVC3] = ISP2200_PDB_TARGET_ROLE;
    stw_le_p(pd + ISP2200_PDB_OFF_LOOP_ID, id);

    if (isp2200_dma_write(s, isp2200_mbox_addr(in), pd, sizeof(pd)) !=
        MEMTX_OK) {
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }
    return ISP2200_MBOX_COMPLETE;
}

/*
 * The position map: the count with the adapter, the adapter's AL_PA, then
 * the ports' in loop ID order. A real map follows loop position; the
 * order here is a model choice, and Linux reads only the count.
 */
static uint16_t isp2200_position_map(QLogicISP2200State *s,
                                     const uint16_t *in)
{
    uint8_t map[ISP2200_POSITION_MAP_SIZE] = { 0 };
    unsigned id, n = 1;

    if (!isp2200_loop_ready(s)) {
        return ISP2200_MBOX_COMMAND_ERROR;
    }
    map[1] = isp2200_alpa[s->loop_id];
    for (id = 0; id <= ISP2200_MAX_LOOP_ID; id++) {
        if (isp2200_target(s, id)) {
            map[1 + n++] = isp2200_alpa[id];
        }
    }
    map[0] = n;
    if (isp2200_dma_write(s, isp2200_mbox_addr(in), map, sizeof(map)) !=
        MEMTX_OK) {
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }
    return ISP2200_MBOX_COMPLETE;
}

/*
 * The loop commands. Each refusal's code is a model choice: the public
 * definitions name the codes but not which refusal returns which.
 */
static uint16_t isp2200_fc_mailbox(QLogicISP2200State *s, uint16_t cmd,
                                   const uint16_t *in, uint16_t *out)
{
    switch (cmd) {
    case ISP2200_MBOX_GET_ADAPTER_ID:
        if (!isp2200_loop_ready(s)) {
            out[1] = ISP2200_ADAPTER_ID_NOT_READY;
            return ISP2200_MBOX_COMMAND_ERROR;
        }
        /* m2 AL_PA and area 0, m3 domain 0, m6 topology 0: private loop. */
        out[1] = s->loop_id;
        out[2] = isp2200_alpa[s->loop_id];
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_GET_ID_LIST:
        return isp2200_get_id_list(s, in, out);

    case ISP2200_MBOX_PORT_NAME_LIST:
        return isp2200_port_name_list(s, in, out);

    case ISP2200_MBOX_GET_PORT_DB:
        return isp2200_get_port_db(s, in);

    case ISP2200_MBOX_POSITION_MAP:
        return isp2200_position_map(s, in);

    case ISP2200_MBOX_ABORT_COMMAND:
    case ISP2200_MBOX_ABORT_TARGET:
    case ISP2200_MBOX_LUN_RESET:
    case ISP2200_MBOX_LIP_RESET:
        return isp2200_recovery(s, cmd, in);

    case ISP2200_MBOX_INITIATE_LIP:
    case ISP2200_MBOX_FULL_LOGIN_LIP:
        if (!s->loop_active) {
            return ISP2200_MBOX_COMMAND_ERROR;
        }
        isp2200_bringup(s, false);
        return ISP2200_MBOX_COMPLETE;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: loop command 0x%04x not modelled\n",
                      cmd);
        return ISP2200_MBOX_INVALID;
    }
}

static bool isp2200_fc_owned(uint16_t cmd)
{
    switch (cmd) {
    case 0x0015: case 0x0017: case 0x0020: case 0x0040: case 0x0041:
    case 0x0047: case 0x0062: case 0x0063: case 0x0064: case 0x006a:
    case 0x006b: case 0x006c: case 0x006e: case 0x006f: case 0x0071:
    case 0x0072: case 0x0074: case 0x0075: case 0x007c: case 0x007e:
        return true;
    default:
        return false;
    }
}

/*
 * Abort device, bus reset and target reset, which no supported driver
 * issues; queue setup the SCSI parts use; retry and resource counts.
 */
static bool isp2200_deferred(uint16_t cmd)
{
    switch (cmd) {
    case 0x0010: case 0x0011: case 0x0014: case 0x0016: case 0x0018:
    case 0x0022: case 0x0042: case 0x0066:
        return true;
    default:
        return false;
    }
}

/* The ROM answers these before any firmware is executed. */
static bool isp2200_rom_command(uint16_t cmd)
{
    switch (cmd) {
    case ISP2200_MBOX_NOP:
    case ISP2200_MBOX_EXEC_FIRMWARE:
    case ISP2200_MBOX_WRITE_RAM_WORD:
    case ISP2200_MBOX_READ_RAM_WORD:
    case ISP2200_MBOX_REGISTER_TEST:
    case ISP2200_MBOX_VERIFY_CHECKSUM:
    case ISP2200_MBOX_ABOUT_FIRMWARE:
    case ISP2200_MBOX_LOAD_RAM:
    case ISP2200_MBOX_LOAD_RAM_EXTENDED:
        return true;
    default:
        return false;
    }
}

/*
 * The whole transfer is read before a word of it is kept, so a refused
 * DMA leaves RAM as it was and a later checksum cannot blame the image
 * for bytes the adapter never received.
 */
static uint16_t isp2200_load_ram(QLogicISP2200State *s, const uint16_t *in)
{
    uint16_t base = in[1];
    uint16_t count = in[4];
    uint64_t host = isp2200_mbox_addr(in);
    g_autofree uint8_t *bytes = NULL;
    uint32_t i;

    trace_qlogic_isp2200_load_ram(base, count, host);

    if (in[0] == ISP2200_MBOX_LOAD_RAM_EXTENDED && in[8] != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: extended RAM load above word 0xffff "
                      "(m8 0x%04x) refused\n", in[8]);
        return ISP2200_MBOX_PARAM_ERROR;
    }
    if ((uint32_t)base + count > ISP2200_RAM_WORDS) {
        return ISP2200_MBOX_PARAM_ERROR;
    }
    if (count == 0) {
        return ISP2200_MBOX_COMPLETE;
    }

    bytes = g_malloc(count * 2u);
    if (isp2200_dma_read(s, host, bytes, count * 2u) != MEMTX_OK) {
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }
    for (i = 0; i < count; i++) {
        s->risc_ram[base + i] = lduw_le_p(bytes + i * 2);
    }
    return ISP2200_MBOX_COMPLETE;
}

/* Word 3 of an image is its length; a good image sums to zero. */
static uint16_t isp2200_verify_checksum(QLogicISP2200State *s,
                                        const uint16_t *in, uint16_t *out)
{
    uint32_t start = in[1];
    uint32_t len, i;
    uint16_t sum = 0;

    out[1] = in[1];
    if (start + 3 >= ISP2200_RAM_WORDS) {
        return ISP2200_MBOX_PARAM_ERROR;
    }
    len = s->risc_ram[start + 3];
    if (start + len > ISP2200_RAM_WORDS) {
        return ISP2200_MBOX_PARAM_ERROR;
    }
    for (i = 0; i < len; i++) {
        sum += s->risc_ram[start + i];
    }
    out[1] = sum;
    out[2] = sum;
    return sum == 0 ? ISP2200_MBOX_COMPLETE : ISP2200_MBOX_TEST_FAILED;
}

static uint16_t isp2200_init_firmware(QLogicISP2200State *s,
                                      const uint16_t *in)
{
    uint64_t addr = isp2200_mbox_addr(in);
    uint8_t icb[ISP2200_ICB_SIZE];
    uint16_t options, rqst_len, resp_len;
    uint64_t rqst_base, resp_base;

    if (isp2200_dma_read(s, addr, icb, sizeof(icb)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: initialisation control block at 0x%"
                      PRIx64 " unreadable\n", addr);
        return ISP2200_MBOX_HOST_INTERFACE_ERROR;
    }

    options = lduw_le_p(icb + ISP2200_ICB_OFF_OPTIONS);
    rqst_len = lduw_le_p(icb + ISP2200_ICB_OFF_RQST_LEN);
    resp_len = lduw_le_p(icb + ISP2200_ICB_OFF_RESP_LEN);
    rqst_base = ldq_le_p(icb + ISP2200_ICB_OFF_RQST_BASE);
    resp_base = ldq_le_p(icb + ISP2200_ICB_OFF_RESP_BASE);

    trace_qlogic_isp2200_init_firmware(addr, rqst_base, rqst_len,
                                       resp_base, resp_len, options);

    if (icb[ISP2200_ICB_OFF_VERSION] != ISP2200_ICB_VERSION ||
        rqst_len == 0 || resp_len == 0 || rqst_base == 0 || resp_base == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: initialisation control block "
                      "refused: version %u, request ring 0x%" PRIx64
                      "/%u, response ring 0x%" PRIx64 "/%u\n",
                      icb[ISP2200_ICB_OFF_VERSION], rqst_base, rqst_len,
                      resp_base, resp_len);
        return ISP2200_MBOX_PARAM_ERROR;
    }
    /* Fast posting completes through events this model does not raise. */
    if (options & ISP2200_ICB_OPT_FAST_POST) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: fast posting is not modelled\n");
        return ISP2200_MBOX_PARAM_ERROR;
    }

    /*
     * A re-initialisation replaces the rings; completions held for the
     * old ones, or still to come from commands in flight, would land in
     * the new ones under stale handles.
     */
    isp2200_cancel_all(s);
    s->held_head = 0;
    s->held_count = 0;

    memcpy(s->icb, icb, sizeof(icb));
    s->queues_init = true;
    s->rqst_base = rqst_base;
    s->resp_base = resp_base;
    s->rqst_len = rqst_len;
    s->resp_len = resp_len;
    s->rqst_in = 0;
    s->rqst_out = 0;
    s->resp_in = 0;
    s->resp_out = 0;

    isp2200_loop_init(s);
    return ISP2200_MBOX_COMPLETE;
}

/*
 * One mailbox command, from the input bank into a fresh output bank.
 * Output mailboxes a command does not name read zero.
 */
static uint16_t isp2200_dispatch(QLogicISP2200State *s, const uint16_t *in,
                                 uint16_t *out)
{
    uint16_t cmd = in[0];

    if (!s->fw_running && !isp2200_rom_command(cmd)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: mailbox command 0x%04x before "
                      "firmware is executed\n", cmd);
        return ISP2200_MBOX_INVALID;
    }

    switch (cmd) {
    case ISP2200_MBOX_NOP:
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_REGISTER_TEST:
        memcpy(out + 1, in + 1, 7 * sizeof(uint16_t));
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_WRITE_RAM_WORD:
        s->risc_ram[in[1]] = in[2];
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_READ_RAM_WORD:
        out[2] = s->risc_ram[in[1]];
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_LOAD_RAM:
    case ISP2200_MBOX_LOAD_RAM_EXTENDED:
        out[1] = in[1];
        return isp2200_load_ram(s, in);

    case ISP2200_MBOX_VERIFY_CHECKSUM:
        return isp2200_verify_checksum(s, in, out);

    /* Nothing runs: the model is the firmware. */
    case ISP2200_MBOX_EXEC_FIRMWARE:
        s->fw_running = true;
        trace_qlogic_isp2200_exec_firmware(in[1]);
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_ABOUT_FIRMWARE:
        out[1] = ISP2200_FW_VERSION_MAJOR;
        out[2] = ISP2200_FW_VERSION_MINOR;
        out[3] = ISP2200_FW_VERSION_MICRO;
        out[6] = ISP2200_FW_ATTRIBUTES;
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_FIRMWARE_STATUS:
        out[2] = ISP2200_MAX_OUTSTANDING;
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_INIT_FIRMWARE:
        return isp2200_init_firmware(s, in);

    case ISP2200_MBOX_GET_FW_STATE:
        out[1] = isp2200_fc_fw_state(s);
        return ISP2200_MBOX_COMPLETE;

    /* Stored and returned; what the options would change is not modelled. */
    case ISP2200_MBOX_GET_FW_OPTIONS:
        memcpy(out + 1, s->fw_options, sizeof(s->fw_options));
        return ISP2200_MBOX_COMPLETE;

    case ISP2200_MBOX_SET_FW_OPTIONS:
        memcpy(s->fw_options, in + 1, sizeof(s->fw_options));
        return ISP2200_MBOX_COMPLETE;

    default:
        break;
    }

    if (isp2200_fc_owned(cmd)) {
        return isp2200_fc_mailbox(s, cmd, in, out);
    }
    if (isp2200_deferred(cmd)) {
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: mailbox command 0x%04x not "
                      "modelled\n", cmd);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: unknown mailbox command 0x%04x\n",
                      cmd);
    }
    return ISP2200_MBOX_INVALID;
}

/* Runs only with the slot free, so its result always has a place. */
static void isp2200_mailbox_command(QLogicISP2200State *s)
{
    assert(!s->completion_held);

    trace_qlogic_isp2200_mbox_command(s->mbox_in[0]);
    memset(s->completion, 0, sizeof(s->completion));
    s->completion[0] = isp2200_dispatch(s, s->mbox_in, s->completion);
    trace_qlogic_isp2200_mbox_complete(s->mbox_in[0], s->completion[0]);

    s->completion_held = true;
    isp2200_service_posts(s);
    isp2200_end_recovery(s, s->recovering);
}

/*
 * A latched doorbell runs once the RISC runs and the slot is free. The
 * slot frees only when a result posts, which the host must then release,
 * so the register write that follows a post always gets here.
 */
static void isp2200_run_pending(QLogicISP2200State *s)
{
    if (s->cmd_pending && !s->risc_reset && !s->risc_paused &&
        !s->completion_held) {
        s->cmd_pending = false;
        isp2200_mailbox_command(s);
    }
}

/*
 * Reads a run of segment descriptors, 32-bit (address, length) pairs or
 * packed 64-bit address and 32-bit length triples.
 */
static void isp2200_parse_segments(ISP2200Command *cmd, const uint8_t *p,
                                   unsigned n, bool a64)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        ISP2200Segment *seg = &cmd->segs[cmd->nsegs++];

        if (a64) {
            seg->addr = ldq_le_p(p + i * ISP2200_SEG64_SIZE);
            seg->len = ldl_le_p(p + i * ISP2200_SEG64_SIZE + 8);
        } else {
            seg->addr = ldl_le_p(p + i * ISP2200_SEG32_SIZE);
            seg->len = ldl_le_p(p + i * ISP2200_SEG32_SIZE + 4);
        }
    }
}

/*
 * A command entry and the continuations its entry count claims. The
 * count, not each entry's type, says how many slots the command takes:
 * a 64-bit continuation has no handle, only segment data from byte 4.
 */
static void isp2200_command_entry(QLogicISP2200State *s, const uint8_t *req,
                                  unsigned count)
{
    uint8_t type = req[ISP2200_ENTRY_OFF_TYPE];
    bool a64 = type != ISP2200_ENTRY_COMMAND;
    uint16_t segcount = lduw_le_p(req + ISP2200_CMD_OFF_SEGCOUNT);
    ISP2200Command cmd = { 0 };
    unsigned inline_segs, cont_segs, capacity, i;
    uint8_t cont_type;

    cmd.handle = ldl_le_p(req + ISP2200_CMD_OFF_HANDLE);
    cmd.loop_id = req[ISP2200_CMD_OFF_TARGET];
    cmd.lun = lduw_le_p(req + ISP2200_CMD_OFF_LUN);
    cmd.control = lduw_le_p(req + ISP2200_CMD_OFF_CONTROL);
    memcpy(cmd.cdb, req + ISP2200_CMD_OFF_CDB, sizeof(cmd.cdb));
    cmd.byte_count = ldl_le_p(req + ISP2200_CMD_OFF_BYTECOUNT);

    if (type == ISP2200_ENTRY_COMMAND_LIST) {
        uint64_t list = ldq_le_p(req + ISP2200_CMD_OFF_LIST_ADDR);
        g_autofree uint8_t *buf = NULL;
        uint16_t list_type = lduw_le_p(req + ISP2200_CMD_OFF_LIST_TYPE);

        if (list_type != 1) {
            qemu_log_mask(LOG_UNIMP,
                          "qlogic-isp2200: segment list type %u, read as "
                          "type 1\n", list_type);
        }
        cmd.segs = g_new0(ISP2200Segment, MAX(segcount, 1));
        if (segcount) {
            buf = g_malloc((size_t)segcount * ISP2200_SEG64_SIZE);
            if (isp2200_dma_read(s, list, buf, (size_t)segcount *
                                 ISP2200_SEG64_SIZE) != MEMTX_OK) {
                g_free(cmd.segs);
                isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_DMA_ERROR);
                return;
            }
            isp2200_parse_segments(&cmd, buf, segcount, true);
        }
        isp2200_fc_submit(s, &cmd);
        return;
    }

    inline_segs = a64 ? ISP2200_CMD_SEGS64 : ISP2200_CMD_SEGS32;
    cont_segs = a64 ? ISP2200_CONT_SEGS64 : ISP2200_CONT_SEGS32;
    cont_type = a64 ? ISP2200_ENTRY_CONTINUATION_A64
                    : ISP2200_ENTRY_CONTINUATION;
    capacity = inline_segs + (count - 1) * cont_segs;
    if (segcount > capacity) {
        isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_BAD_PARAM);
        return;
    }

    cmd.segs = g_new0(ISP2200Segment, MAX(segcount, 1));
    isp2200_parse_segments(&cmd, req + ISP2200_CMD_OFF_SEGS,
                           MIN(segcount, inline_segs), a64);

    for (i = 1; i < count && cmd.nsegs < segcount; i++) {
        uint16_t index = (s->rqst_out + i) % s->rqst_len;
        uint64_t addr = s->rqst_base + (uint64_t)index * ISP2200_ENTRY_SIZE;
        uint8_t cont[ISP2200_ENTRY_SIZE];

        if (isp2200_dma_read(s, addr, cont, sizeof(cont)) != MEMTX_OK) {
            g_free(cmd.segs);
            isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_DMA_ERROR);
            return;
        }
        if (cont[ISP2200_ENTRY_OFF_TYPE] != cont_type) {
            g_free(cmd.segs);
            isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_BAD_ORDER);
            return;
        }
        isp2200_parse_segments(&cmd, cont + (a64 ? ISP2200_CONT_OFF_SEGS64
                                                 : ISP2200_CONT_OFF_SEGS32),
                               MIN(segcount - cmd.nsegs, cont_segs), a64);
    }

    isp2200_fc_submit(s, &cmd);
}

/*
 * The adapter consumes the request ring as fast as the host fills it, so
 * the ring is never seen full. It stops short only when the entry's
 * continuations are not all published yet, when a read is refused, when
 * a recovery is cancelling requests, or when the announced command budget
 * -- commands in flight plus completions waiting to post -- is used up.
 */
static void isp2200_consume_requests(QLogicISP2200State *s)
{
    while (s->queues_init && !s->risc_reset && !s->risc_paused &&
           s->rqst_out != s->rqst_in) {
        uint16_t avail = (s->rqst_in + s->rqst_len - s->rqst_out) %
                         s->rqst_len;
        uint64_t addr = s->rqst_base +
                        (uint64_t)s->rqst_out * ISP2200_ENTRY_SIZE;
        uint8_t req[ISP2200_ENTRY_SIZE];
        unsigned consumed = 1;
        uint8_t count;

        if (s->recovering) {
            s->rqst_recovery_stall = true;
            return;
        }
        if (s->outstanding + s->held_count >= ISP2200_MAX_OUTSTANDING) {
            s->rqst_budget_stall = true;
            return;
        }

        /* Not consumed: the next producer write tries the entry again. */
        if (isp2200_dma_read(s, addr, req, sizeof(req)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: request entry read at 0x%" PRIx64
                          " refused\n", addr);
            isp2200_raise_event(s, ISP2200_ASYNC_REQUEST_DMA_ERROR);
            isp2200_service_posts(s);
            return;
        }

        count = req[ISP2200_ENTRY_OFF_COUNT];
        trace_qlogic_isp2200_request(s->rqst_out, req[ISP2200_ENTRY_OFF_TYPE],
                                     count);

        switch (req[ISP2200_ENTRY_OFF_TYPE]) {
        case ISP2200_ENTRY_COMMAND:
        case ISP2200_ENTRY_COMMAND_A64:
        case ISP2200_ENTRY_COMMAND_LIST:
            if (count == 0 || count >= s->rqst_len) {
                isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_BAD_COUNT);
                break;
            }
            if (count > avail) {
                /* The host has not published the whole command yet. */
                return;
            }
            consumed = count;
            isp2200_command_entry(s, req, count);
            break;

        /* A marker resynchronises firmware state; nothing is posted. */
        case ISP2200_ENTRY_MARKER:
            break;

        case ISP2200_ENTRY_CONTINUATION:
        case ISP2200_ENTRY_CONTINUATION_A64:
            isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_BAD_ORDER);
            break;

        default:
            isp2200_reject_entry(s, req, ISP2200_ENTRY_FLAG_BAD_TYPE);
            break;
        }

        /* A reset inside the dispatch has taken the ring with it. */
        if (!s->queues_init) {
            return;
        }
        s->rqst_out = (s->rqst_out + consumed) % s->rqst_len;
    }
}

static void isp2200_process_requests(QLogicISP2200State *s)
{
    if (s->processing) {
        s->reprocess = true;
        return;
    }
    s->processing = true;
    do {
        s->reprocess = false;
        isp2200_consume_requests(s);
    } while (s->reprocess);
    s->processing = false;
}

static void isp2200_release_risc(QLogicISP2200State *s)
{
    if (s->risc_reset) {
        s->risc_reset = false;
        s->risc_paused = false;
        isp2200_boot_rom(s);
        return;
    }
    if (!s->risc_paused) {
        return;
    }

    s->risc_paused = false;
    s->mbox_out[0] = 0;
    isp2200_run_pending(s);
    isp2200_service_posts(s);
    isp2200_process_requests(s);
}

/* Bits 15:12 are the command; the rest of a command not modelled is ignored. */
static void isp2200_host_command(QLogicISP2200State *s, uint16_t val)
{
    unsigned cmd = val >> ISP2200_HCCR_SHIFT;

    trace_qlogic_isp2200_host_command(val);

    switch (cmd) {
    case ISP2200_HCCR_NOP:
        break;

    case ISP2200_HCCR_RESET_RISC:
        isp2200_hold_risc(s);
        break;

    case ISP2200_HCCR_PAUSE_RISC:
        s->risc_paused = true;
        s->mbox_out[0] = ISP2200_MBOX_BUSY;
        break;

    case ISP2200_HCCR_RELEASE_RISC:
        isp2200_release_risc(s);
        break;

    /* The doorbell: the command is in the mailboxes. */
    case ISP2200_HCCR_SET_HOST_INT:
        s->host_int = true;
        s->cmd_window = false;
        if (s->risc_reset) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: mailbox command 0x%04x dropped: "
                          "the RISC is held in reset\n", s->mbox_in[0]);
            break;
        }
        if (s->cmd_pending) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: doorbell rung again before the "
                          "last command ran; one command runs\n");
        }
        s->cmd_pending = true;
        isp2200_run_pending(s);
        break;

    case ISP2200_HCCR_CLEAR_HOST_INT:
        s->host_int = false;
        break;

    case ISP2200_HCCR_CLEAR_RISC_INT:
        s->risc_int = false;
        isp2200_update_irq(s);
        isp2200_service_posts(s);
        break;

    /*
     * Parity masking or single step at 0x4000, parity pause at 0x4001,
     * and the diagnostic commands: accepted, and none changes the RISC.
     */
    case ISP2200_HCCR_PARITY_4:
    case ISP2200_HCCR_BREAKPOINT:
    case ISP2200_HCCR_BIOS:
    case ISP2200_HCCR_PARITY:
    case ISP2200_HCCR_PARITY_ERROR:
    case ISP2200_HCCR_TEST_MODE:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: host command 0x%04x accepted, not "
                      "modelled\n", val);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: unknown host command 0x%04x\n", val);
        break;
    }
}

static int isp2200_mbox_index(hwaddr addr)
{
    if (addr >= ISP2200_REG_MBOX_LO &&
        addr < ISP2200_REG_MBOX_LO + ISP2200_MBOX_LO_COUNT * 2) {
        return (addr - ISP2200_REG_MBOX_LO) / 2;
    }
    if (addr >= ISP2200_REG_MBOX_HI) {
        return ISP2200_MBOX_LO_COUNT + (addr - ISP2200_REG_MBOX_HI) / 2;
    }
    return -1;
}

static bool isp2200_queue_pointers_live(QLogicISP2200State *s)
{
    return s->queues_init && !s->cmd_window;
}

static void isp2200_mailbox_write(QLogicISP2200State *s, int idx,
                                  uint16_t val)
{
    if (idx == 0) {
        s->cmd_window = true;
    }

    if (idx == ISP2200_MBOX_REQUEST && isp2200_queue_pointers_live(s)) {
        if (val >= s->rqst_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: request-in %u past a ring of "
                          "%u ignored\n", val, s->rqst_len);
            return;
        }
        s->rqst_in = val;
        trace_qlogic_isp2200_request_ring(s->rqst_in, s->rqst_out);
        isp2200_process_requests(s);
        return;
    }
    if (idx == ISP2200_MBOX_RESPONSE && isp2200_queue_pointers_live(s)) {
        if (val >= s->resp_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "qlogic-isp2200: response-out %u past a ring of "
                          "%u ignored\n", val, s->resp_len);
            return;
        }
        s->resp_out = val;
        trace_qlogic_isp2200_response_ring(s->resp_in, s->resp_out, 0);
        isp2200_service_posts(s);
        return;
    }

    s->mbox_in[idx] = val;
    s->mbox_out[idx] = val;
}

static uint16_t isp2200_mailbox_read(QLogicISP2200State *s, int idx)
{
    /* While a result holds the semaphore, mailboxes 4 and 5 are its data. */
    if (s->queues_init && !(s->semaphore & ISP2200_SEMAPHORE_LOCK)) {
        if (idx == ISP2200_MBOX_REQUEST) {
            return s->rqst_out;
        }
        if (idx == ISP2200_MBOX_RESPONSE) {
            return s->resp_in;
        }
    }
    return s->mbox_out[idx];
}

static void isp2200_semaphore_write(QLogicISP2200State *s, uint16_t val)
{
    if (val != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: semaphore write 0x%04x ignored; only "
                      "a write of 0 releases it\n", val);
        return;
    }
    s->semaphore = 0;
    isp2200_service_posts(s);
}

static uint64_t isp2200_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    QLogicISP2200State *s = opaque;
    bool reset_visible = s->csr_reset_visible;
    int idx;

    s->csr_reset_visible = false;

    if (size != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: %u-byte read at 0x%02" HWADDR_PRIx
                      " answered as 16 bits\n", size, addr);
    }

    idx = isp2200_mbox_index(addr);
    if (idx >= 0) {
        return isp2200_mailbox_read(s, idx);
    }

    switch (addr) {
    case ISP2200_REG_CSR:
        return s->csr | (reset_visible ? ISP2200_CSR_SOFT_RESET : 0);
    case ISP2200_REG_ICR:
        return s->icr;
    case ISP2200_REG_ISR: {
        uint16_t isr = s->risc_int ? ISP2200_ISR_RISC : 0;

        if (s->risc_int && (s->icr & ISP2200_ICR_MASK) == ISP2200_ICR_MASK) {
            isr |= ISP2200_ISR_PENDING;
        }
        return isr;
    }
    case ISP2200_REG_SEMAPHORE:
        return s->semaphore;
    case ISP2200_REG_NVRAM:
        return s->nvram;
    case ISP2200_REG_HCCR:
        return (s->host_int ? ISP2200_HCCR_HOST_INT : 0) |
               (s->risc_reset ? ISP2200_HCCR_RISC_RESET : 0) |
               (s->risc_paused ? ISP2200_HCCR_RISC_PAUSED : 0);
    case ISP2200_REG_GPIO_DATA:
        return s->gpio_data;
    case ISP2200_REG_GPIO_ENABLE:
        return s->gpio_enable;
    default:
        /* Flash, DMA engine, banked and diagnostic registers. */
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: read of unimplemented register "
                      "0x%02" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void isp2200_reg_write(void *opaque, hwaddr addr, uint64_t val64,
                              unsigned size)
{
    QLogicISP2200State *s = opaque;
    uint16_t val = val64;
    int idx;

    if (size != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: %u-byte write at 0x%02" HWADDR_PRIx
                      " taken as 16 bits\n", size, addr);
    }

    idx = isp2200_mbox_index(addr);
    if (idx >= 0) {
        isp2200_mailbox_write(s, idx, val);
        isp2200_run_pending(s);
        return;
    }

    switch (addr) {
    case ISP2200_REG_CSR:
        if (val & ~(ISP2200_CSR_SOFT_RESET | ISP2200_CSR_BANK_MASK)) {
            qemu_log_mask(LOG_UNIMP,
                          "qlogic-isp2200: CSR flash bits 0x%04x not "
                          "modelled\n",
                          val & ~(ISP2200_CSR_SOFT_RESET |
                                  ISP2200_CSR_BANK_MASK));
        }
        /* The loop's devices see the reset too. */
        if (val & ISP2200_CSR_SOFT_RESET) {
            isp2200_soft_reset(s);
            bus_cold_reset(BUS(&s->bus));
        }
        s->csr = val & ISP2200_CSR_BANK_MASK;
        break;

    case ISP2200_REG_ICR:
        if (val & ~ISP2200_ICR_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "qlogic-isp2200: interrupt control bits 0x%04x "
                          "not modelled\n", val & ~ISP2200_ICR_MASK);
        }
        s->icr = val & ISP2200_ICR_MASK;
        isp2200_update_irq(s);
        break;

    case ISP2200_REG_ISR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qlogic-isp2200: write 0x%04x to the read-only "
                      "interrupt status register\n", val);
        break;

    case ISP2200_REG_SEMAPHORE:
        isp2200_semaphore_write(s, val);
        break;

    case ISP2200_REG_NVRAM:
        s->nvram = val & ISP2200_NVRAM_WRITE_MASK;
        break;

    case ISP2200_REG_HCCR:
        isp2200_host_command(s, val);
        break;

    case ISP2200_REG_GPIO_DATA:
        s->gpio_data = val;
        break;

    case ISP2200_REG_GPIO_ENABLE:
        s->gpio_enable = val;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "qlogic-isp2200: write 0x%04x to unimplemented "
                      "register 0x%02" HWADDR_PRIx "\n", val, addr);
        break;
    }

    /* A write that let a result post may have freed the slot. */
    isp2200_run_pending(s);
}

/*
 * Drivers use 16-bit accesses only. A 32-bit access reaches the handler
 * once and is answered as a 16-bit one at its address; byte accesses are
 * refused by the memory core.
 */
static const MemoryRegionOps isp2200_reg_ops = {
    .read = isp2200_reg_read,
    .write = isp2200_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
};

/* The HBA half of a request in flight; the shape is qlogic-isp's. */
static void isp2200_save_request(QEMUFile *f, SCSIRequest *sreq)
{
    ISP2200Request *req = sreq->hba_private;
    int i;

    qemu_put_be32(f, req->handle);
    qemu_put_be32(f, sreq->dev->id);
    qemu_put_be32(f, sreq->lun);
    qemu_put_be32(f, req->qsg.nsg);
    for (i = 0; i < req->qsg.nsg; i++) {
        qemu_put_be64(f, req->qsg.sg[i].base);
        qemu_put_be64(f, req->qsg.sg[i].len);
    }
    qemu_put_be64(f, req->described);
    qemu_put_be64(f, req->transferred);
    qemu_put_be32(f, req->sg_index);
    qemu_put_be64(f, req->sg_offset);
}

/*
 * The checks keep a stream that does not describe a request this build
 * could have made from reaching the transfer path. The callback has no
 * failure return, so a bad stream is marked failed and the wrapper left
 * consistent for teardown.
 */
static void *isp2200_load_request(QEMUFile *f, SCSIRequest *sreq)
{
    QLogicISP2200State *s = container_of(sreq->bus, QLogicISP2200State, bus);
    ISP2200Request *req;
    uint32_t loop_id, lun, n, i;
    uint64_t transferred;

    req = isp2200_new_request(s, qemu_get_be32(f));
    scsi_req_ref(sreq);
    req->sreq = sreq;

    /* The budget the walk keeps, whichever of the two loads first. */
    if (s->outstanding + s->held_count > ISP2200_MAX_OUTSTANDING) {
        qemu_file_set_error(f, -EINVAL);
        return req;
    }

    loop_id = qemu_get_be32(f);
    lun = qemu_get_be32(f);
    n = qemu_get_be32(f);
    if (loop_id != sreq->dev->id || lun != sreq->lun ||
        n > UINT16_MAX || qemu_file_get_error(f)) {
        qemu_file_set_error(f, -EINVAL);
        return req;
    }
    for (i = 0; i < n; i++) {
        uint64_t base = qemu_get_be64(f);
        uint64_t len = qemu_get_be64(f);

        if (!len || req->qsg.size > sreq->cmd.xfer ||
            len > sreq->cmd.xfer - req->qsg.size ||
            base > UINT64_MAX - len || qemu_file_get_error(f)) {
            qemu_file_set_error(f, -EINVAL);
            return req;
        }
        qemu_sglist_add(&req->qsg, base, len);
    }
    req->described = qemu_get_be64(f);
    req->transferred = qemu_get_be64(f);
    req->sg_index = qemu_get_be32(f);
    req->sg_offset = qemu_get_be64(f);
    if (req->described < req->qsg.size || req->sg_index > req->qsg.nsg ||
        (req->sg_index == req->qsg.nsg ? req->sg_offset != 0 :
         req->sg_offset >= req->qsg.sg[req->sg_index].len)) {
        qemu_file_set_error(f, -EINVAL);
        return req;
    }
    transferred = req->sg_offset;
    for (i = 0; i < req->sg_index; i++) {
        transferred += req->qsg.sg[i].len;
    }
    if (transferred != req->transferred || qemu_file_get_error(f)) {
        qemu_file_set_error(f, -EINVAL);
    }
    return req;
}

static const struct SCSIBusInfo isp2200_scsi_info = {
    .tcq = false,
    .max_channel = 0,
    .max_target = ISP2200_MAX_LOOP_ID,
    .max_lun = ISP2200_MAX_LUN,
    .get_sg_list = isp2200_scsi_get_sg_list,
    .transfer_data = isp2200_scsi_transfer_data,
    .complete = isp2200_scsi_complete,
    .cancel = isp2200_scsi_cancel,
    .save_request = isp2200_save_request,
    .load_request = isp2200_load_request,
};

/*
 * A device joins the loop as a LUN of the port at its target number. A
 * port needs both names -- the drivers match ports by them, and inventing
 * names would be worse than refusing -- and every LUN of one port carries
 * the same pair, no other port has its port name, and the adapter's own
 * loop ID is not a port's. The other devices are walked, not looked up:
 * the one being plugged is on the bus already, not yet realized.
 */
static void isp2200_pre_plug(const HotplugHandler *hotplug_dev,
                             DeviceState *dev, Error **errp)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(hotplug_dev);
    SCSIDevice *sdev = SCSI_DEVICE(dev);
    BusChild *kid;

    if (sdev->id == -1) {
        error_setg(errp, "a loop port needs an explicit scsi-id: it is the "
                   "port's loop ID");
        return;
    }
    if (!sdev->port_wwn) {
        error_setg(errp, "the port at loop ID %u needs a nonzero port_wwn",
                   sdev->id);
        return;
    }
    if (!sdev->wwn) {
        error_setg(errp, "the port at loop ID %u needs a nonzero node name, "
                   "wwn", sdev->id);
        return;
    }
    if (s->has_loop_id && sdev->id == s->loop_id) {
        error_setg(errp, "loop ID %u is the adapter's own", sdev->id);
        return;
    }

    QTAILQ_FOREACH(kid, &s->bus.qbus.children, sibling) {
        SCSIDevice *other = SCSI_DEVICE(kid->child);

        if (other == sdev) {
            continue;
        }
        if (other->id == sdev->id &&
            (other->port_wwn != sdev->port_wwn || other->wwn != sdev->wwn)) {
            error_setg(errp, "the port at loop ID %u is named port 0x%016"
                       PRIx64 " node 0x%016" PRIx64 ", and every LUN of a "
                       "port carries its names", other->id, other->port_wwn,
                       other->wwn);
            return;
        }
        if (other->id != sdev->id && other->port_wwn == sdev->port_wwn) {
            error_setg(errp, "port_wwn 0x%016" PRIx64 " already names the "
                       "port at loop ID %u", sdev->port_wwn, other->id);
            return;
        }
    }
}

/* A device that makes a new port changes the loop; another LUN does not. */
static void isp2200_plug(const HotplugHandler *hotplug_dev, DeviceState *dev,
                         Error **errp)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(hotplug_dev);

    if (!isp2200_port(s, SCSI_DEVICE(dev)->id)) {
        isp2200_bringup(s, false);
    }
}

/*
 * A port leaving the loop is not a device leaving a SCSI bus: both drivers
 * expect firmware to complete its commands "port logged out" and then
 * announce the change. Its requests are retired so before it goes. The
 * purge and drain that follow must not let a command finishing on another
 * port start the request ring part-way. Removing a port's last LUN takes
 * the port off the loop, which comes up again without it.
 */
static void isp2200_unplug(const HotplugHandler *hotplug_dev,
                           DeviceState *dev, Error **errp)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(hotplug_dev);
    bool was_recovering = s->recovering;
    int id = SCSI_DEVICE(dev)->id;

    s->recovering = true;
    isp2200_retire(s, -1, -1, -1, SCSI_DEVICE(dev),
                   ISP2200_COMPLETION_LOGGED_OUT);
    qdev_simple_device_unplug_cb(hotplug_dev, dev, errp);
    if (!isp2200_port(s, id)) {
        isp2200_bringup(s, false);
    }
    isp2200_end_recovery(s, was_recovering);
}

static void isp2200_realize(PCIDevice *dev, Error **errp)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(dev);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /* One register file behind both BARs. */
    memory_region_init_io(&s->io, OBJECT(s), &isp2200_reg_ops, s,
                          "qlogic-isp2200-io", ISP2200_REG_WINDOW_SIZE);
    memory_region_init_alias(&s->mmio, OBJECT(s), "qlogic-isp2200-mmio",
                             &s->io, 0, ISP2200_REG_WINDOW_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    s->bringup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isp2200_bringup_done,
                                    s);
    scsi_bus_init(&s->bus, sizeof(s->bus), DEVICE(dev), &isp2200_scsi_info);
    qbus_set_hotplug_handler(BUS(&s->bus), OBJECT(dev));
}

/* Unrealizing the loop's devices has already cancelled their requests. */
static void isp2200_exit(PCIDevice *dev)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(dev);

    timer_free(s->bringup_timer);
    g_free(s->fc_last.segs);
    s->fc_last.segs = NULL;
}

static char *isp2200_get_fc_last(Object *obj, Error **errp)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(obj);
    GString *str = g_string_new(NULL);
    unsigned i;

    g_string_append_printf(str, "handle=%08x loop=%u lun=%u control=%04x "
                           "cdb=", s->fc_last.handle, s->fc_last.loop_id,
                           s->fc_last.lun, s->fc_last.control);
    for (i = 0; i < ISP2200_CMD_CDB_LEN; i++) {
        g_string_append_printf(str, "%02x", s->fc_last.cdb[i]);
    }
    g_string_append_printf(str, " bytes=%u segs=%u seglen=%" PRIu64,
                           s->fc_last.byte_count, s->fc_last.nsegs,
                           s->fc_last_bytes);
    return g_string_free(str, false);
}

static void isp2200_instance_init(Object *obj)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(obj);

    QTAILQ_INIT(&s->requests);

    object_property_add_uint32_ptr(obj, "x-fc-submissions",
                                   &s->fc_submissions, OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "x-fc-last-submission",
                            isp2200_get_fc_last, NULL);
}

/*
 * The queue as this build can fill it: at most one each of the host
 * interface's two events and one bring-up's set, so that the next push
 * cannot pass the bound isp2200_push_event() asserts.
 */
static bool isp2200_events_valid(QLogicISP2200State *s)
{
    unsigned lip = 0, i, j;

    if (s->event_count > ISP2200_EVENTS_REACHABLE) {
        return false;
    }
    for (i = 0; i < s->event_count; i++) {
        uint16_t code = s->events[i][0];

        switch (code) {
        case ISP2200_ASYNC_LIP_OCCURRED:
        case ISP2200_ASYNC_LIP_RESET:
            lip++;
            break;
        case ISP2200_ASYNC_LOOP_UP:
        case ISP2200_ASYNC_PORT_DB_CHANGED:
        case ISP2200_ASYNC_REQUEST_DMA_ERROR:
        case ISP2200_ASYNC_RESPONSE_DMA_ERROR:
            break;
        default:
            return false;
        }
        for (j = 0; j < i; j++) {
            if (s->events[j][0] == code) {
                return false;
            }
        }
    }
    return lip <= 1;
}

static bool isp2200_counts_valid(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    return isp2200_events_valid(s) &&
           s->held_head < ISP2200_HELD_MAX &&
           s->held_count <= ISP2200_HELD_MAX;
}

static bool isp2200_rings_valid(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    if (!s->queues_init) {
        return true;
    }
    return s->rqst_len && s->resp_len &&
           s->rqst_in < s->rqst_len && s->rqst_out < s->rqst_len &&
           s->resp_in < s->resp_len && s->resp_out < s->resp_len;
}

static bool isp2200_loop_valid(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    switch (s->loop_state) {
    case ISP2200_FW_STATE_CONFIG_WAIT:
    case ISP2200_FW_STATE_WAIT_AL_PA:
    case ISP2200_FW_STATE_READY:
        break;
    default:
        return false;
    }
    return s->loop_id <= ISP2200_MAX_LOOP_ID &&
           (s->has_loop_id || s->loop_state != ISP2200_FW_STATE_READY);
}

/*
 * The loop itself is not in the stream: it is the bus's devices, which a
 * matching configuration makes the same. An adapter loop ID one of them
 * holds is a mismatched configuration.
 */
static int isp2200_post_load(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    if (s->has_loop_id && isp2200_port(s, s->loop_id)) {
        return -EINVAL;
    }
    if (s->outstanding + s->held_count > ISP2200_MAX_OUTSTANDING) {
        return -EINVAL;
    }
    isp2200_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_isp2200 = {
    .name = "qlogic-isp2200",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = isp2200_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QLogicISP2200State),
        VMSTATE_UINT16(csr, QLogicISP2200State),
        VMSTATE_BOOL(csr_reset_visible, QLogicISP2200State),
        VMSTATE_UINT16(icr, QLogicISP2200State),
        VMSTATE_UINT16(semaphore, QLogicISP2200State),
        VMSTATE_UINT16(nvram, QLogicISP2200State),
        VMSTATE_UINT16(gpio_data, QLogicISP2200State),
        VMSTATE_UINT16(gpio_enable, QLogicISP2200State),
        VMSTATE_BOOL(risc_int, QLogicISP2200State),
        VMSTATE_BOOL(host_int, QLogicISP2200State),
        VMSTATE_BOOL(risc_reset, QLogicISP2200State),
        VMSTATE_BOOL(risc_paused, QLogicISP2200State),
        VMSTATE_UINT16_ARRAY(mbox_in, QLogicISP2200State,
                             ISP2200_MAILBOX_COUNT),
        VMSTATE_UINT16_ARRAY(mbox_out, QLogicISP2200State,
                             ISP2200_MAILBOX_COUNT),
        VMSTATE_BOOL(cmd_window, QLogicISP2200State),
        VMSTATE_BOOL(cmd_pending, QLogicISP2200State),
        VMSTATE_BOOL(fw_running, QLogicISP2200State),
        VMSTATE_UINT16_ARRAY(fw_options, QLogicISP2200State, 3),
        VMSTATE_UINT16_ARRAY(risc_ram, QLogicISP2200State,
                             ISP2200_RAM_WORDS),
        VMSTATE_UINT8_ARRAY(icb, QLogicISP2200State, ISP2200_ICB_SIZE),
        VMSTATE_BOOL(queues_init, QLogicISP2200State),
        VMSTATE_UINT64(rqst_base, QLogicISP2200State),
        VMSTATE_UINT64(resp_base, QLogicISP2200State),
        VMSTATE_UINT16(rqst_len, QLogicISP2200State),
        VMSTATE_UINT16(resp_len, QLogicISP2200State),
        VMSTATE_UINT16(rqst_in, QLogicISP2200State),
        VMSTATE_UINT16(rqst_out, QLogicISP2200State),
        VMSTATE_UINT16(resp_in, QLogicISP2200State),
        VMSTATE_UINT16(resp_out, QLogicISP2200State),
        VMSTATE_VALIDATE("ring pointers within their rings",
                         isp2200_rings_valid),
        VMSTATE_BOOL(completion_held, QLogicISP2200State),
        VMSTATE_UINT16_ARRAY(completion, QLogicISP2200State,
                             ISP2200_MAILBOX_COUNT),
        VMSTATE_UINT32(event_count, QLogicISP2200State),
        VMSTATE_UINT16_2DARRAY(events, QLogicISP2200State,
                               ISP2200_EVENTS_MAX, ISP2200_MAILBOX_COUNT),
        VMSTATE_UINT32(held_head, QLogicISP2200State),
        VMSTATE_UINT32(held_count, QLogicISP2200State),
        VMSTATE_VALIDATE("queued results within their bounds",
                         isp2200_counts_valid),
        VMSTATE_UINT8_2DARRAY(held, QLogicISP2200State, ISP2200_HELD_MAX,
                              ISP2200_ENTRY_SIZE),
        VMSTATE_BOOL(rqst_budget_stall, QLogicISP2200State),
        VMSTATE_BOOL(loop_active, QLogicISP2200State),
        VMSTATE_UINT16(loop_state, QLogicISP2200State),
        VMSTATE_BOOL(has_loop_id, QLogicISP2200State),
        VMSTATE_UINT8(loop_id, QLogicISP2200State),
        VMSTATE_VALIDATE("loop state and loop ID", isp2200_loop_valid),
        VMSTATE_BOOL(bringup_lip_reset, QLogicISP2200State),
        VMSTATE_TIMER_PTR(bringup_timer, QLogicISP2200State),
        VMSTATE_END_OF_LIST()
    },
};

static void isp2200_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);

    pc->realize = isp2200_realize;
    pc->exit = isp2200_exit;
    pc->vendor_id = ISP2200_VENDOR_ID;
    pc->device_id = ISP2200_DEVICE_ID;
    pc->subsystem_vendor_id = ISP2200_VENDOR_ID;
    pc->subsystem_id = ISP2200_SUBSYSTEM_ID;
    pc->class_id = ISP2200_CLASS_FIBRE_CHANNEL;
    rc->phases.hold = isp2200_reset_hold;
    hc->pre_plug = isp2200_pre_plug;
    hc->plug = isp2200_plug;
    hc->unplug = isp2200_unplug;
    dc->desc = "QLogic ISP2200 Fibre Channel host adapter";
    dc->vmsd = &vmstate_isp2200;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo isp2200_info = {
    .name = TYPE_QLOGIC_ISP2200,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QLogicISP2200State),
    .instance_init = isp2200_instance_init,
    .class_init = isp2200_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { TYPE_HOTPLUG_HANDLER },
        { },
    },
};

static void isp2200_register_types(void)
{
    type_register_static(&isp2200_info);
}

type_init(isp2200_register_types)
