/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QLogic ISP2200 Fibre Channel host adapter: the host interface.
 *
 * The host talks to the adapter's firmware through 24 mailboxes and two
 * rings in host memory. As with the SCSI parts in qlogic-isp.c, the guest's
 * firmware download is accepted, retained and checksummed, never executed;
 * the model implements the mailbox commands and rings that firmware
 * provides. The Fibre Channel side -- loop state, logins and whatever sits
 * behind a loop handle -- is a separate service behind four hooks. None is
 * attached yet, so the adapter reports a disconnected loop. The contract
 * and its sources are in docs/system/mips/sgi/hardware/qlogic-isp2200.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
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
#define ISP2200_MBOX_FIRMWARE_STATUS    0x001f
#define ISP2200_MBOX_GET_FW_OPTIONS     0x0028
#define ISP2200_MBOX_SET_FW_OPTIONS     0x0038
#define ISP2200_MBOX_INIT_FIRMWARE      0x0060
#define ISP2200_MBOX_GET_FW_STATE       0x0069

#define ISP2200_MBOX_COMPLETE           0x4000
#define ISP2200_MBOX_INVALID            0x4001
#define ISP2200_MBOX_HOST_INTERFACE_ERROR 0x4002
#define ISP2200_MBOX_TEST_FAILED        0x4003
#define ISP2200_MBOX_PARAM_ERROR        0x4006

#define ISP2200_ASYNC_REQUEST_DMA_ERROR  0x8003
#define ISP2200_ASYNC_RESPONSE_DMA_ERROR 0x8004

/* A model constant, deliberately not any vendor firmware release. */
#define ISP2200_FW_VERSION_MAJOR    1
#define ISP2200_FW_VERSION_MINOR    0
#define ISP2200_FW_VERSION_MICRO    0
/* Expanded LUN only: no fabric, no 2K logins. */
#define ISP2200_FW_ATTRIBUTES       0x0002

#define ISP2200_MAX_OUTSTANDING     512

/* Get Firmware State's "configuration wait", the disconnected answer. */
#define ISP2200_FW_STATE_CONFIG_WAIT 0

/* 64K 16-bit words of controller RAM; code starts at word 0x1000. */
#define ISP2200_RAM_WORDS           0x10000

#define ISP2200_ICB_SIZE            96
#define ISP2200_ICB_VERSION         1
#define ISP2200_ICB_OFF_VERSION     0
#define ISP2200_ICB_OFF_OPTIONS     2
#define ISP2200_ICB_OFF_RQST_LEN    36
#define ISP2200_ICB_OFF_RESP_LEN    38
#define ISP2200_ICB_OFF_RQST_BASE   40
#define ISP2200_ICB_OFF_RESP_BASE   48
#define ISP2200_ICB_OPT_FAST_POST   0x0008

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

#define ISP2200_COMPLETION_PORT_UNAVAILABLE 0x0028

/*
 * Events waiting for the host to release the result before them. No
 * result is ever dropped, so the queue is sized for every event that can
 * wait: the host interface raises two kinds itself, and a refused read
 * that finds an identical event still queued adds nothing (see
 * isp2200_raise_event()).
 */
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

struct QLogicISP2200State {
    PCIDevice parent_obj;

    MemoryRegion io;    /* Base address register 0, the I/O window. */
    MemoryRegion mmio;  /* Base address register 1: an alias of io. */

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
     * What the disconnected FC service saw, read by qtests through QOM
     * properties. Not guest-visible and not migrated.
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

/*
 * HCCR reset and the CSR soft reset hold the RISC the same way. The
 * firmware goes with it: every supported driver downloads again after a
 * reset, and a guest that expects firmware to survive one is unsupported.
 */
static void isp2200_hold_risc(QLogicISP2200State *s)
{
    s->risc_reset = true;
    s->risc_paused = false;
    s->cmd_window = false;
    s->cmd_pending = false;

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

    /* Room in the held queue lets a walk stopped on the budget resume. */
    if (s->rqst_budget_stall && s->held_count < ISP2200_HELD_MAX) {
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

    /* Two kinds, each queued at most once: the queue cannot fill. */
    assert(s->event_count < ISP2200_EVENTS_MAX);
    trace_qlogic_isp2200_event(code);
    memcpy(s->events[s->event_count++], mb, sizeof(mb));
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
    entry[ISP2200_ENTRY_OFF_TYPE] = ISP2200_ENTRY_STATUS;
    entry[ISP2200_ENTRY_OFF_COUNT] = 1;
    stl_le_p(entry + ISP2200_STATUS_OFF_HANDLE, handle);
    stw_le_p(entry + ISP2200_STATUS_OFF_SCSI_STATUS, scsi_status);
    stw_le_p(entry + ISP2200_STATUS_OFF_COMPLETION, completion);
    stw_le_p(entry + ISP2200_STATUS_OFF_SENSE_LEN, sense_len);
    stl_le_p(entry + ISP2200_STATUS_OFF_RESIDUAL, residual);
    if (sense_len) {
        memcpy(entry + ISP2200_STATUS_OFF_SENSE, sense, sense_len);
    }

    isp2200_hold_entry(s, entry);
    isp2200_service_posts(s);
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
 * The Fibre Channel service. With none attached the adapter is
 * disconnected: firmware state configuration wait, every command
 * completes "port unavailable" and every loop command is refused.
 */
static uint16_t isp2200_fc_fw_state(QLogicISP2200State *s)
{
    return ISP2200_FW_STATE_CONFIG_WAIT;
}

static void isp2200_fc_submit(QLogicISP2200State *s, ISP2200Command *cmd)
{
    uint32_t i;

    trace_qlogic_isp2200_submit(cmd->handle, cmd->loop_id, cmd->lun,
                                cmd->cdb[0], cmd->nsegs);

    s->fc_submissions++;
    g_free(s->fc_last.segs);
    s->fc_last = *cmd;
    s->fc_last_bytes = 0;
    for (i = 0; i < cmd->nsegs; i++) {
        s->fc_last_bytes += cmd->segs[i].len;
    }
    cmd->segs = NULL;

    isp2200_complete(s, s->fc_last.handle, 0,
                     ISP2200_COMPLETION_PORT_UNAVAILABLE, 0, NULL, 0);
}

static uint16_t isp2200_fc_mailbox(QLogicISP2200State *s, uint16_t cmd,
                                   const uint16_t *in, uint16_t *out)
{
    qemu_log_mask(LOG_UNIMP,
                  "qlogic-isp2200: loop command 0x%04x refused: no Fibre "
                  "Channel service is attached\n", cmd);
    return ISP2200_MBOX_INVALID;
}

static bool isp2200_fc_owned(uint16_t cmd)
{
    switch (cmd) {
    case 0x0020: case 0x0040: case 0x0041: case 0x0047: case 0x0062:
    case 0x0063: case 0x0064: case 0x006a: case 0x006b: case 0x006c:
    case 0x006e: case 0x006f: case 0x0071: case 0x0072: case 0x0074:
    case 0x0075: case 0x007c:
        return true;
    default:
        return false;
    }
}

/* Error recovery and SCSI-part queue setup, not yet in the contract. */
static bool isp2200_deferred(uint16_t cmd)
{
    switch (cmd) {
    case 0x0010: case 0x0011: case 0x0014: case 0x0015: case 0x0016:
    case 0x0017: case 0x0018: case 0x0042:
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
     * old ones would land in the new ones under stale handles.
     */
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
 * continuations are not all published yet, when a read is refused, or
 * when the announced command budget is held waiting for the host.
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

        if (s->held_count >= ISP2200_HELD_MAX) {
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
        if (val & ISP2200_CSR_SOFT_RESET) {
            isp2200_soft_reset(s);
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
}

static void isp2200_exit(PCIDevice *dev)
{
    QLogicISP2200State *s = QLOGIC_ISP2200(dev);

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

    object_property_add_uint32_ptr(obj, "x-fc-submissions",
                                   &s->fc_submissions, OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "x-fc-last-submission",
                            isp2200_get_fc_last, NULL);
}

static bool isp2200_counts_valid(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    return s->event_count <= ISP2200_EVENTS_MAX &&
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

static int isp2200_post_load(void *opaque, int version_id)
{
    QLogicISP2200State *s = opaque;

    s->rqst_budget_stall = s->held_count >= ISP2200_HELD_MAX;
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
        VMSTATE_END_OF_LIST()
    },
};

static void isp2200_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    pc->realize = isp2200_realize;
    pc->exit = isp2200_exit;
    pc->vendor_id = ISP2200_VENDOR_ID;
    pc->device_id = ISP2200_DEVICE_ID;
    pc->subsystem_vendor_id = ISP2200_VENDOR_ID;
    pc->subsystem_id = ISP2200_SUBSYSTEM_ID;
    pc->class_id = ISP2200_CLASS_FIBRE_CHANNEL;
    rc->phases.hold = isp2200_reset_hold;
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
        { },
    },
};

static void isp2200_register_types(void)
{
    type_register_static(&isp2200_info);
}

type_init(isp2200_register_types)
