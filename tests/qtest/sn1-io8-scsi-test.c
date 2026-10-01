/*
 * The Origin 300 IO-8 board's QLogic ISP12160, reached the way a guest
 * reaches it. Register cycles cross XIO into XBridge head F and out
 * of head F's programmed I/O window for slot 1; the adapter's own
 * transfers cross back through head F into Bedrock's memory.
 *
 * The part's own register contract belongs to sn0-isp12160-test.c and is
 * not repeated here. What this file pins is that the board carries the
 * adapter at the position the board puts it, and that both directions of
 * the fabric path between it and the processor's memory work.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Guest register cycles cross crosstalk and head F's slot-1 I/O window;
 * adapter transfers return through head F. sn0-isp12160-test.c covers the
 * adapter register contract. This test checks board placement and both
 * directions of the path between the adapter and processor memory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define IO_WIDGET(w) (SPACE_IO + (uint64_t)(w) * UINT64_C(0x1000000))
#define IO_INBOUND_ACCESS 0x118

#define BEDROCK_NODE0 (SPACE_IO + UINT64_C(0x01800000))
#define BEDROCK_IO (BEDROCK_NODE0 + 0x400000)
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
/*
 * Origin 300's installed memory occupies locational banks 2 and 3, not bank
 * 0: map software bank 0 onto them so the request and response rings, both
 * under 4 MiB, land in backed RAM instead of an I/O hole.
 */
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

/* Head F carries the IO-8 board, and the adapter is device 1 on it. */
#define IO8_WIDGET 0xf
#define IO8_SCSI_SLOT 1
#define IO8_SCSI_PIN 1
#define IO8_SCSI_CFG (IO_WIDGET(IO8_WIDGET) + 0x20000 + \
                      IO8_SCSI_SLOT * 0x1000)

/*
 * The head's I/O region spans 0x200000..0xc00000: slots 0/1 get 2 MiB each
 * from 0x200000, slots 2..7 get 1 MiB each from 0x600000, and 0xc00000 holds
 * a 4 MiB section. Slot 1 is the second 2 MiB block. The same map puts IOC3
 * slot 4 at 0x800000 and OHCI slot 5 at 0x900000; sn1-address-map-test checks
 * those devices.
 */
#define IO8_SCSI_WINDOW (IO_WIDGET(IO8_WIDGET) + 0x400000)

/*
 * The device register gives the window target in MiB. A 2 MiB slot-0/1 window
 * ignores the low target bit and needs 2 MiB alignment. No other bus device
 * is enabled here, so this address is free.
 */
#define ADAPTER_MMIO 0x10000000

#define XBRIDGE_DEVICE(n) (0x200 + (n) * 8)
#define XBRIDGE_DEVICE_MEMORY (1U << 12)
#define XBRIDGE_INT_STATUS 0x100
#define XBRIDGE_INT_ENABLE 0x108
#define XBRIDGE_INT_UPPER 0x30
#define XBRIDGE_INT_LOWER 0x38
#define XBRIDGE_INT_HOST(n) (0x130 + (n) * 8)
#define XBRIDGE_HOST_PORT 0xa

/* Where an interrupt packet lands, and which pending bit it names. */
#define PI_REMOTE_WINDOW 0x1800000
#define PI_PENDING_MODIFY 0x90
#define PI_TEST_PENDING_BIT 17

#define PCI_VENDOR_ID 0x00
#define PCI_CLASS_REVISION 0x08
#define PCI_CACHE_LINE_SIZE 0x0c
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_INTERRUPT_LINE 0x3c
/* The power-management control word, inside the capability at 0x44. */
#define PCI_PM_CTRL 0x48

/* Class 0x01 mass storage, subclass 0x00 SCSI, programming interface 0. */
#define PCI_CLASS_SCSI 0x010000

#define ISP_VENDOR_ID 0x1077
#define ISP12160_DEVICE_ID 0x1216
#define ISP12160_REVISION 0x06

/* The two identity registers as one doubleword. */
#define ISP_ID_PAIR (((uint32_t)ISP12160_DEVICE_ID << 16) | ISP_VENDOR_ID)

/* Offsets in the adapter's own window: PCI space, so no plus-four here. */
#define ISP_REG_ID_LOW 0x00
#define ISP_REG_ID_HIGH 0x02
#define ISP_REG_BUS_CTRL 0x08
#define ISP_REG_SEMAPHORE 0x0c
#define ISP_REG_MAILBOX_BASE 0x70
#define ISP_MAILBOX(n) (ISP_REG_MAILBOX_BASE + (n) * 2)

/* The guest's halfword address: offset ^ (4 - width), low two bits only. */
#define GUEST_HALFWORD(natural) (((natural) & ~3u) | (((natural) & 3u) ^ 2u))

#define ISP_REG_HOST_CMD 0xc0
#define ISP_HOST_CMD_SHIFT 12
#define ISP_HOST_CMD_SET_HOST_INT 0x5
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7

#define ISP_BUS_CTRL_INT_ENABLE 0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004

/* The signature mailboxes 1-4 carry out of reset, and the not-ready code. */
#define ISP_MAILBOX1_SIGNATURE 0x4953
#define ISP_MAILBOX2_SIGNATURE 0x5020
#define ISP_MAILBOX3_SIGNATURE 0x2020
#define ISP_MAILBOX4_SIGNATURE 0x0001
#define ISP_MBOX_NOT_READY 0x0004

#define ISP_MBOX_CMD_MAILBOX_REG_TEST 0x0006
#define ISP_MBOX_CMD_INIT_REQ_QUEUE_A64 0x0052
#define ISP_MBOX_CMD_INIT_RSP_QUEUE_A64 0x0053
#define ISP_MBOX_COMPLETE 0x4000

/* Mailboxes 4 and 5 are the two ring cursors, and each has two roles. */
#define ISP_REG_REQUEST_QUEUE ISP_MAILBOX(4)
#define ISP_REG_RESPONSE_QUEUE ISP_MAILBOX(5)

#define ISP_QUEUE_ENTRY_SIZE 64
#define ISP_ENTRY_OFF_TYPE 0
#define ISP_ENTRY_OFF_COUNT 1
#define ISP_ENTRY_TYPE_STATUS 0x03
#define ISP_A64_ENTRY_TYPE_COMMAND 0x09

#define ISP_REQ_OFF_HANDLE 4
#define ISP_REQ_OFF_DEVICE 8
#define ISP_REQ_OFF_CDBLEN 10
#define ISP_REQ_OFF_CONTROL 12
#define ISP_REQ_OFF_SEGCOUNT 18
#define ISP_REQ_OFF_CDB 20
#define ISP_A64_REQ_OFF_SEGS 40

#define ISP_STATUS_OFF_HANDLE 4
#define ISP_STATUS_OFF_SCSI_STATUS 8
#define ISP_STATUS_OFF_COMPLETION 10
#define ISP_STATUS_OFF_STATE_FLAGS 12
#define ISP_STATUS_OFF_STATUS_FLAGS 14
#define ISP_STATUS_OFF_RESID 20

#define ISP_IOCB_COMPLETE 0x0000

/* One bit per SCSI phase the command reached. */
#define ISP_STATE_FLAG_GOT_BUS 0x0100
#define ISP_STATE_FLAG_GOT_TARGET 0x0200
#define ISP_STATE_FLAG_SENT_CDB 0x0400
#define ISP_STATE_FLAG_TRANSFERRED_DATA 0x0800
#define ISP_STATE_FLAG_GOT_STATUS 0x1000
#define ISP_STATE_FLAG_TRANSFER_COMPLETE 0x4000

/* Data from the target into host memory. */
#define ISP_REQ_CONTROL_DATA_IN 0x0020

/* Data from host memory out to the target. */
#define ISP_REQ_CONTROL_DATA_OUT 0x0040

/* Bit 15 is the channel; the target and LUN below it are local to it. */
#define ISP_DEVICE_WORD(channel, target, lun) \
    ((uint16_t)(((channel) << 15) | ((target) << 8) | (lun)))

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a
#define SCSI_CDB6_LEN 6
#define SCSI_CDB10_LEN 10

/* A sector past the fixture's first, so the outbound test owns its medium. */
#define SCRATCH_LBA 5

/* Both disks sit at the same target number, one on each channel. */
#define DISK_TARGET 1
#define DISK_BYTES (1024 * 1024)
#define SECTOR_BYTES 512

/* Ring and buffer homes, in memory nothing else on this machine uses. */
#define RQST_PHYS 0x00200000u
#define RESP_PHYS 0x00300040u
#define BUF_PHYS 0x00400000u
#define RQST_SLOTS 4
#define RESP_SLOTS 8

/*
 * A constant fill, so a buffer left unwritten can never match either disk's
 * first sector: those walk their byte values and are constant nowhere.
 */
#define BUFFER_SENTINEL 0xcc

/*
 * A 64-bit transfer uses its top nibble for target widget and passes the low
 * 48 bits through. Bit 55 disables head byte-lane reversal, preserving the
 * adapter's stream; the fields read back as its little-endian words.
 */
#define DIRECT64(phys) (((uint64_t)XBRIDGE_HOST_PORT << 60) | \
                        (UINT64_C(1) << 55) | (uint64_t)(phys))

/* A read held by the block layer waits on wall time, not the guest clock. */
#define STATUS_STEP_NS (1000 * 1000)
#define STATUS_WAIT_US (10 * 1000 * 1000)

static char *prom_path;

/* The ring geometry the helpers work against; one test runs at a time. */
static struct {
    uint64_t rqst_iova;
    uint64_t resp_iova;
    uint16_t rqst_len;
    uint16_t resp_len;
    uint16_t rqst_next;    /* The slot the test will write next. */
    uint16_t resp_next;    /* The slot the test will read next. */
} ring;

typedef struct DiskPair {
    char *dir;
    char *path[2];
} DiskPair;

/* One inline data descriptor, which is all a single-sector command needs. */
typedef struct RingSeg {
    uint64_t iova;
    uint32_t len;
} RingSeg;

static void put16le(uint8_t *b, uint16_t v)
{
    b[0] = v;
    b[1] = v >> 8;
}

static void put32le(uint8_t *b, uint32_t v)
{
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
}

static uint16_t get16le(const uint8_t *b)
{
    return b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t get32le(const uint8_t *b)
{
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-io8-scsi-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/*
 * Each disk's first sector uses a distinct byte stride: adjacent bytes never
 * repeat and the disks differ at every offset. Word reversal, a one-sector
 * offset, or the wrong channel cannot match; constant fill could.
 */
static uint8_t disk_byte(unsigned disk, unsigned offset)
{
    return disk == 0 ? (uint8_t)(offset * 7 + 1) : (uint8_t)(offset * 11 + 3);
}

static void disks_create(DiskPair *disks)
{
    GError *error = NULL;
    unsigned disk, i;

    disks->dir = g_dir_make_tmp("sn1-io8-scsi-XXXXXX", &error);
    g_assert_no_error(error);

    for (disk = 0; disk < 2; disk++) {
        g_autofree uint8_t *image = g_malloc0(DISK_BYTES);
        g_autofree char *name = g_strdup_printf("disk%u.raw", disk);

        for (i = 0; i < SECTOR_BYTES; i++) {
            image[i] = disk_byte(disk, i);
        }
        disks->path[disk] = g_build_filename(disks->dir, name, NULL);
        g_assert_true(g_file_set_contents(disks->path[disk],
                                          (const char *)image, DISK_BYTES,
                                          &error));
        g_assert_no_error(error);
    }
}

static void disks_remove(DiskPair *disks)
{
    unsigned disk;

    for (disk = 0; disk < 2; disk++) {
        g_assert_cmpint(unlink(disks->path[disk]), ==, 0);
        g_free(disks->path[disk]);
    }
    g_assert_cmpint(rmdir(disks->dir), ==, 0);
    g_free(disks->dir);
}

static uint32_t widget_reg_read(QTestState *qts, unsigned int widget,
                                uint64_t offset)
{
    return qtest_readl(qts, IO_WIDGET(widget) + offset + 4);
}

static void widget_reg_write(QTestState *qts, unsigned int widget,
                             uint64_t offset, uint32_t value)
{
    qtest_writel(qts, IO_WIDGET(widget) + offset + 4, value);
}

/*
 * With the device swap bit reset clear, 16-bit window accesses flip PCI
 * offset bits 1:0 but preserve the value. Full 32-bit accesses need no
 * byte-lane correction.
 */
static void isp_writew(QTestState *qts, unsigned offset, uint16_t value)
{
    qtest_writew(qts, IO8_SCSI_WINDOW + GUEST_HALFWORD(offset), value);
}

static uint16_t isp_readw(QTestState *qts, unsigned offset)
{
    return qtest_readw(qts, IO8_SCSI_WINDOW + GUEST_HALFWORD(offset));
}

/* The order Linux uses to release a completed mailbox command. */
static void isp_mbox_ack(QTestState *qts)
{
    isp_writew(qts, ISP_REG_SEMAPHORE, 0);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
}

/*
 * A whole submission: inputs into mailboxes 1 upwards, the opcode last, then
 * the host interrupt the protocol triggers on. The answer is read without
 * acknowledging, which is the caller's decision to make.
 */
static uint16_t isp_mbox(QTestState *qts, uint16_t cmd, const uint16_t *in,
                         int nin)
{
    int i;

    for (i = 0; i < nin; i++) {
        isp_writew(qts, ISP_MAILBOX(1 + i), in[i]);
    }
    isp_writew(qts, ISP_MAILBOX(0), cmd);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    return isp_readw(qts, ISP_MAILBOX(0));
}

/*
 * Use the adapter memory BAR: the head's memory window directly reaches a
 * 32-bit PCI address. The device swap bit stays reset clear, preserving word
 * placement as bus drivers expect.
 */
static void io8_scsi_attach(QTestState *qts)
{
    qtest_writel(qts, IO8_SCSI_CFG + PCI_BAR1, ADAPTER_MMIO);
    qtest_writel(qts, IO8_SCSI_CFG + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    widget_reg_write(qts, IO8_WIDGET, XBRIDGE_DEVICE(IO8_SCSI_SLOT),
                     XBRIDGE_DEVICE_MEMORY | (ADAPTER_MMIO >> 20));
}

static void io8_route_interrupt(QTestState *qts, unsigned int pin,
                                unsigned int pending_bit)
{
    widget_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_UPPER,
                     XBRIDGE_HOST_PORT << 16);
    widget_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_LOWER,
                     PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
    widget_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_HOST(pin),
                     (1U << 17) | pending_bit);
    widget_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_ENABLE, 1U << pin);
}

/* The head's interrupt status carries one bit per physical pin. */
static unsigned head_f_pin(QTestState *qts, unsigned int pin)
{
    return (widget_reg_read(qts, IO8_WIDGET, XBRIDGE_INT_STATUS) >> pin) & 1;
}

static QTestState *machine_start(void)
{
    g_autofree char *prom = g_shell_quote(prom_path);

    return qtest_initf(MACHINE_ARGS " -bios %s", prom);
}

/*
 * The only SCSI adapter names channel 0 scsi.0 and channel 1 scsi.1. A wrong
 * bus name fails startup instead of placing both disks on one bus.
 */
static QTestState *machine_start_with_disks(DiskPair *disks)
{
    g_autofree char *prom = NULL;
    g_autofree char *file0 = NULL;
    g_autofree char *file1 = NULL;
    QTestState *qts;

    disks_create(disks);
    prom = g_shell_quote(prom_path);
    file0 = g_shell_quote(disks->path[0]);
    file1 = g_shell_quote(disks->path[1]);
    qts = qtest_initf(MACHINE_ARGS " -bios %s"
                      " -drive if=none,id=d0,format=raw,file=%s"
                      " -device scsi-hd,bus=scsi.0,scsi-id=%u,drive=d0"
                      " -drive if=none,id=d1,format=raw,file=%s"
                      " -device scsi-hd,bus=scsi.1,scsi-id=%u,drive=d1",
                      prom, file0, DISK_TARGET, file1, DISK_TARGET);
    qtest_writeq(qts, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    io8_scsi_attach(qts);
    return qts;
}

/* A direct 64-bit transfer passes the low bits through unchanged. */
static uint64_t ring_phys(uint64_t iova)
{
    return iova & 0xffffffffu;
}

/*
 * One queue initialisation. Mailbox 6 carries address bits 63:48 and mailbox
 * 7 bits 47:32, so a model that drops either upper word aims the ring
 * somewhere else. Mailboxes 4 and 5 carry the initial indices.
 */
static void init_one_queue(QTestState *qts, uint16_t cmd, uint16_t count,
                           uint64_t iova)
{
    const uint16_t in[] = {
        count,
        (uint16_t)(iova >> 16),
        (uint16_t)iova,
        0,
        0,
        (uint16_t)(iova >> 48),
        (uint16_t)(iova >> 32),
    };

    g_assert_cmphex(isp_mbox(qts, cmd, in, ARRAY_SIZE(in)), ==,
                    ISP_MBOX_COMPLETE);
    isp_mbox_ack(qts);
}

static void init_queues(QTestState *qts)
{
    qtest_memset(qts, RQST_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RQST_SLOTS);
    qtest_memset(qts, RESP_PHYS, 0, ISP_QUEUE_ENTRY_SIZE * RESP_SLOTS);
    init_one_queue(qts, ISP_MBOX_CMD_INIT_REQ_QUEUE_A64, RQST_SLOTS,
                   DIRECT64(RQST_PHYS));
    init_one_queue(qts, ISP_MBOX_CMD_INIT_RSP_QUEUE_A64, RESP_SLOTS,
                   DIRECT64(RESP_PHYS));

    ring.rqst_iova = DIRECT64(RQST_PHYS);
    ring.resp_iova = DIRECT64(RESP_PHYS);
    ring.rqst_len = RQST_SLOTS;
    ring.resp_len = RESP_SLOTS;
    ring.rqst_next = 0;
    ring.resp_next = 0;
}

/*
 * Publish one command in the next guest-memory request slot. Broken outbound
 * DMA leaves it unseen. One descriptor covers a sector; longer transfers
 * would need continuation entries.
 */
static void submit_command(QTestState *qts, uint32_t handle,
                           unsigned channel, unsigned target, unsigned lun,
                           const uint8_t *cdb, unsigned cdb_len,
                           uint16_t control, const RingSeg *seg)
{
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t slot = ring.rqst_next;

    memset(entry, 0, sizeof(entry));
    entry[ISP_ENTRY_OFF_TYPE] = ISP_A64_ENTRY_TYPE_COMMAND;
    entry[ISP_ENTRY_OFF_COUNT] = 1;
    put32le(entry + ISP_REQ_OFF_HANDLE, handle);
    put16le(entry + ISP_REQ_OFF_DEVICE,
            ISP_DEVICE_WORD(channel, target, lun));
    put16le(entry + ISP_REQ_OFF_CDBLEN, cdb_len);
    put16le(entry + ISP_REQ_OFF_CONTROL, control);
    memcpy(entry + ISP_REQ_OFF_CDB, cdb, cdb_len);
    if (seg) {
        put16le(entry + ISP_REQ_OFF_SEGCOUNT, 1);
        put32le(entry + ISP_A64_REQ_OFF_SEGS, (uint32_t)seg->iova);
        put32le(entry + ISP_A64_REQ_OFF_SEGS + 4,
                (uint32_t)(seg->iova >> 32));
        put32le(entry + ISP_A64_REQ_OFF_SEGS + 8, seg->len);
    }
    qtest_memwrite(qts, ring_phys(ring.rqst_iova) +
                   slot * ISP_QUEUE_ENTRY_SIZE, entry, sizeof(entry));

    ring.rqst_next = (slot + 1) % ring.rqst_len;
    isp_writew(qts, ISP_REG_REQUEST_QUEUE, ring.rqst_next);
}

/*
 * Mailbox 5 reports the response producer index; a new completion moves it.
 * Use a wall-time bound because a block-layer request waits on its timer, not
 * the guest clock.
 */
static void wait_for_completion(QTestState *qts)
{
    gint64 deadline = g_get_monotonic_time() + STATUS_WAIT_US;

    while (isp_readw(qts, ISP_REG_RESPONSE_QUEUE) == ring.resp_next) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        qtest_clock_step(qts, STATUS_STEP_NS);
    }
}

/*
 * The completion read out of guest memory and then released. Writing mailbox
 * 5 back frees the slot, and clearing the interrupt is what a driver does
 * once it has taken the entry.
 */
static void take_status(QTestState *qts, uint32_t handle, uint8_t *status)
{
    qtest_memread(qts, ring_phys(ring.resp_iova) +
                  ring.resp_next * ISP_QUEUE_ENTRY_SIZE, status,
                  ISP_QUEUE_ENTRY_SIZE);
    g_assert_cmphex(status[ISP_ENTRY_OFF_TYPE], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(get32le(status + ISP_STATUS_OFF_HANDLE), ==, handle);

    ring.resp_next = (ring.resp_next + 1) % ring.resp_len;
    isp_writew(qts, ISP_REG_RESPONSE_QUEUE, ring.resp_next);
    isp_writew(qts, ISP_REG_HOST_CMD,
               ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
}

/* A six-byte command that moves no data, run to its status entry. */
static void run_simple(QTestState *qts, uint32_t handle, unsigned channel,
                       unsigned target, unsigned lun, uint8_t op,
                       uint8_t *status)
{
    uint8_t cdb[SCSI_CDB6_LEN] = { op };

    submit_command(qts, handle, channel, target, lun, cdb, sizeof(cdb), 0,
                   NULL);
    wait_for_completion(qts);
    take_status(qts, handle, status);
}

/*
 * A power-on leaves each disk reporting a unit attention, which the first
 * command to it collects. Drivers absorb it the same way.
 */
static void absorb_unit_attention(QTestState *qts, unsigned channel,
                                  uint32_t handle)
{
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    run_simple(qts, handle, channel, DISK_TARGET, 0, SCSI_TEST_UNIT_READY,
               status);
}

static void put_lba10(uint8_t *cdb, uint32_t lba, uint16_t blocks)
{
    cdb[2] = lba >> 24;
    cdb[3] = lba >> 16;
    cdb[4] = lba >> 8;
    cdb[5] = lba;
    cdb[7] = blocks >> 8;
    cdb[8] = blocks;
}

/*
 * READ(10) uses one sector and descriptor. Prefill its buffer with a sentinel
 * so absent DMA cannot resemble medium data.
 */
static void read_sector(QTestState *qts, uint32_t handle, unsigned channel,
                        uint32_t lba, uint8_t *out, uint8_t *status)
{
    uint8_t cdb[SCSI_CDB10_LEN] = { SCSI_READ_10 };
    const RingSeg seg = { DIRECT64(BUF_PHYS), SECTOR_BYTES };

    put_lba10(cdb, lba, 1);
    qtest_memset(qts, BUF_PHYS, BUFFER_SENTINEL, SECTOR_BYTES);
    submit_command(qts, handle, channel, DISK_TARGET, 0, cdb, sizeof(cdb),
                   ISP_REQ_CONTROL_DATA_IN, &seg);
    wait_for_completion(qts);
    take_status(qts, handle, status);
    qtest_memread(qts, BUF_PHYS, out, SECTOR_BYTES);
}

/*
 * WRITE(10) fetches a guest-memory sector through head DMA read, opposite to
 * READ(10)'s DMA write of medium bytes into memory. It uses one 64-bit direct
 * transfer.
 */
static void write_sector(QTestState *qts, uint32_t handle, unsigned channel,
                         uint32_t lba, const uint8_t *in, uint8_t *status)
{
    uint8_t cdb[SCSI_CDB10_LEN] = { SCSI_WRITE_10 };
    const RingSeg seg = { DIRECT64(BUF_PHYS), SECTOR_BYTES };

    put_lba10(cdb, lba, 1);
    qtest_memwrite(qts, BUF_PHYS, in, SECTOR_BYTES);
    submit_command(qts, handle, channel, DISK_TARGET, 0, cdb, sizeof(cdb),
                   ISP_REQ_CONTROL_DATA_OUT, &seg);
    wait_for_completion(qts);
    take_status(qts, handle, status);
}

/* The whole first sector of one disk, as the fixture wrote it. */
static void expect_first_sector(const uint8_t *got, unsigned disk)
{
    uint8_t want[SECTOR_BYTES];
    unsigned i;

    for (i = 0; i < SECTOR_BYTES; i++) {
        want[i] = disk_byte(disk, i);
    }
    g_assert_cmpmem(got, SECTOR_BYTES, want, sizeof(want));
}

/*
 * The board records the adapter in slot 1. Missing or misplaced hardware
 * reads as all ones; another family member has a different device ID and
 * revision.
 */
static void test_identity(const void *data)
{
    QTestState *qts = machine_start();
    uint32_t class_revision;

    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_VENDOR_ID), ==,
                    ISP_ID_PAIR);
    class_revision = qtest_readl(qts, IO8_SCSI_CFG + PCI_CLASS_REVISION);
    g_assert_cmphex(class_revision >> 8, ==, PCI_CLASS_SCSI);
    g_assert_cmphex(class_revision & 0xff, ==, ISP12160_REVISION);
    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_SUBSYSTEM_VENDOR_ID),
                    ==, 0xffffffff);
    /* The adapter presents one interrupt line, on INTA. */
    g_assert_cmphex((qtest_readl(qts, IO8_SCSI_CFG + PCI_INTERRUPT_LINE) >> 8)
                    & 0xff, ==, 1);

    qtest_quit(qts);
}

/*
 * io8bt sets Command SERR, parity response, memory-write-invalidate, bus
 * master, and memory-space bits; all five must read back. The PCI core
 * normally leaves two of them read-only.
 */
static void test_io8bt_command_register(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writel(qts, IO8_SCSI_CFG + PCI_COMMAND, 0x156);
    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_COMMAND) & 0xffff,
                    ==, 0x156);

    qtest_quit(qts);
}

/*
 * At config 0x0c, io8bt writes 0xf8ff across cache-line size, Latency Timer,
 * header type, and self-test. The Latency Timer must accept a write despite
 * the PCI core default. Its low three writable bits are unknown; the exact
 * diagnostic value avoids claiming them.
 */
static void test_io8bt_latency_timer(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writel(qts, IO8_SCSI_CFG + PCI_CACHE_LINE_SIZE, 0xf8ff);
    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_CACHE_LINE_SIZE),
                    ==, 0xf8ff);

    qtest_quit(qts);
}

/*
 * Power-management control is at 0x48 in the 0x44 capability. After zeroing
 * it, io8bt enables events and requires 0x100 readback. The PCI core makes
 * the capability read-only; power-state and data-select writability remain
 * unknown and are not tested.
 */
static void test_io8bt_power_management(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writel(qts, IO8_SCSI_CFG + PCI_PM_CTRL, 0);
    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_PM_CTRL), ==, 0);
    qtest_writel(qts, IO8_SCSI_CFG + PCI_PM_CTRL, 0x100);
    g_assert_cmphex(qtest_readl(qts, IO8_SCSI_CFG + PCI_PM_CTRL), ==, 0x100);

    qtest_quit(qts);
}

/*
 * The slot-1 window must expose identity and reset signature. Seven distinct
 * mailbox patterns then detect a 1 MiB misroute, unwanted offset correction,
 * or aliased registers as wrong words.
 */
static void test_registers_through_the_fabric(const void *data)
{
    static const uint16_t pattern[] = {
        0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777
    };
    QTestState *qts = machine_start();
    unsigned i;

    io8_scsi_attach(qts);

    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_LOW), ==, ISP_VENDOR_ID);
    g_assert_cmphex(isp_readw(qts, ISP_REG_ID_HIGH), ==, ISP12160_DEVICE_ID);

    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1)), ==,
                    ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(2)), ==,
                    ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(3)), ==,
                    ISP_MAILBOX3_SIGNATURE);
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(4)), ==,
                    ISP_MAILBOX4_SIGNATURE);
    /* The initialisation both drivers run waits for this to go away. */
    g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(0)), !=, ISP_MBOX_NOT_READY);

    g_assert_cmphex(isp_mbox(qts, ISP_MBOX_CMD_MAILBOX_REG_TEST, pattern,
                             ARRAY_SIZE(pattern)), ==, ISP_MBOX_COMPLETE);
    for (i = 0; i < ARRAY_SIZE(pattern); i++) {
        g_assert_cmphex(isp_readw(qts, ISP_MAILBOX(1 + i)), ==, pattern[i]);
    }
    isp_mbox_ack(qts);

    qtest_quit(qts);
}

/*
 * One command from end to end. Both rings live in the processor's memory
 * and the adapter reaches them over XIO, so an inbound path that
 * does not work leaves the response producer where it was and the wait
 * runs out. The handle and the completion code are asserted rather than
 * the entry merely having changed: a status entry written into the wrong
 * slot, or carrying another command's handle, is not this command
 * completing.
 */
static void test_command_reaches_memory(const void *data)
{
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0x81);

    run_simple(qts, 0x82, 0, DISK_TARGET, 0, SCSI_TEST_UNIT_READY, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAG_GOT_BUS | ISP_STATE_FLAG_GOT_TARGET |
                    ISP_STATE_FLAG_SENT_CDB | ISP_STATE_FLAG_GOT_STATUS);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==, 0);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * TEST UNIT READY only checks rings. Here READ moves a fixture sector into
 * guest memory through a driver-format descriptor. Compare every byte and
 * require zero residual plus data-phase and completion flags; status alone
 * cannot prove transfer.
 */
static void test_data_transfer_reaches_memory(const void *data)
{
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t sector[SECTOR_BYTES];

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0xa1);

    read_sector(qts, 0xa2, 0, 0, sector, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAG_GOT_BUS | ISP_STATE_FLAG_GOT_TARGET |
                    ISP_STATE_FLAG_SENT_CDB |
                    ISP_STATE_FLAG_TRANSFERRED_DATA |
                    ISP_STATE_FLAG_GOT_STATUS |
                    ISP_STATE_FLAG_TRANSFER_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==, 0);
    g_assert_cmpuint(get32le(status + ISP_STATUS_OFF_RESID), ==, 0);
    expect_first_sector(sector, 0);

    qtest_quit(qts);
    disks_remove(&disks);
}

typedef struct InboundMaskCase {
    uint64_t denied_widget;
    bool permitted;
} InboundMaskCase;

/* The shared inbound window lacks the DMA requester's widget number. */
static void test_scsi_dma_with_inbound_mask(const void *data)
{
    const InboundMaskCase *mask_case = data;
    const uint8_t cdb[SCSI_CDB10_LEN] = { SCSI_READ_10, 0, 0, 0, 0, 0,
                                         0, 0, 1, 0 };
    const RingSeg seg = { DIRECT64(BUF_PHYS), SECTOR_BYTES };
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t sector[SECTOR_BYTES];
    uint8_t sentinel[SECTOR_BYTES];

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0xd1);
    qtest_memset(qts, BUF_PHYS, BUFFER_SENTINEL, SECTOR_BYTES);
    memset(sentinel, BUFFER_SENTINEL, sizeof(sentinel));
    qtest_writeq(qts, BEDROCK_IO + IO_INBOUND_ACCESS,
                 UINT64_MAX & ~mask_case->denied_widget);

    submit_command(qts, 0xd2, 0, DISK_TARGET, 0, cdb, sizeof(cdb),
                   ISP_REQ_CONTROL_DATA_IN, &seg);
    if (mask_case->permitted) {
        wait_for_completion(qts);
        take_status(qts, 0xd2, status);
        g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                        ISP_IOCB_COMPLETE);
        qtest_memread(qts, BUF_PHYS, sector, sizeof(sector));
        expect_first_sector(sector, 0);
    } else {
        qtest_clock_step(qts, 100 * STATUS_STEP_NS);
        g_assert_cmpuint(isp_readw(qts, ISP_REG_RESPONSE_QUEUE), ==,
                         ring.resp_next);
        qtest_memread(qts, BUF_PHYS, sector, sizeof(sector));
        g_assert_cmpmem(sector, sizeof(sector), sentinel, sizeof(sentinel));
    }

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * WRITE fetches a guest-memory sector by head DMA read, then READ returns it
 * by DMA write. The source stride differs from both fixture disks and from
 * its own word reverse. Since inbound DMA is known unreversed, byte-exact
 * round-trip detects outbound reversal or wrong fetch address. Bit 55 matches
 * IRIX's request and disables byte-lane reversal in both directions.
 */
static void test_write_transfer_from_memory(const void *data)
{
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t sent[SECTOR_BYTES];
    uint8_t got[SECTOR_BYTES];
    unsigned i;

    for (i = 0; i < SECTOR_BYTES; i++) {
        sent[i] = (uint8_t)(i * 13 + 5);
    }

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0xc1);

    write_sector(qts, 0xc2, 0, SCRATCH_LBA, sent, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_SCSI_STATUS), ==, 0);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAG_GOT_BUS | ISP_STATE_FLAG_GOT_TARGET |
                    ISP_STATE_FLAG_SENT_CDB |
                    ISP_STATE_FLAG_TRANSFERRED_DATA |
                    ISP_STATE_FLAG_GOT_STATUS |
                    ISP_STATE_FLAG_TRANSFER_COMPLETE);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_STATUS_FLAGS), ==, 0);
    g_assert_cmpuint(get32le(status + ISP_STATUS_OFF_RESID), ==, 0);

    read_sector(qts, 0xc3, 0, SCRATCH_LBA, got, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmpmem(got, sizeof(got), sent, sizeof(sent));

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Each channel has a disk at the same target number; only the request's
 * channel bit selects it. Aliased buses or channel routing return one disk's
 * sector twice, while the sectors differ at every offset.
 */
static void test_channels_are_independent(const void *data)
{
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t sector[SECTOR_BYTES];

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0xb1);
    absorb_unit_attention(qts, 1, 0xb2);

    read_sector(qts, 0xb3, 0, 0, sector, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    expect_first_sector(sector, 0);

    read_sector(qts, 0xb4, 1, 0, sector, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    expect_first_sector(sector, 1);

    qtest_quit(qts);
    disks_remove(&disks);
}

/*
 * Check the board-wired completion pin before and after the command. Adapter
 * interrupts stay disabled during ring setup to avoid earlier mailbox IRQs;
 * the pre-check catches a pin stuck high.
 */
static void test_completion_raises_head_f_pin(const void *data)
{
    uint8_t cdb[SCSI_CDB6_LEN] = { SCSI_TEST_UNIT_READY };
    DiskPair disks;
    QTestState *qts = machine_start_with_disks(&disks);
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    init_queues(qts);
    absorb_unit_attention(qts, 0, 0x91);

    isp_writew(qts, ISP_REG_BUS_CTRL,
               ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);
    io8_route_interrupt(qts, IO8_SCSI_PIN, PI_TEST_PENDING_BIT);
    g_assert_cmpuint(head_f_pin(qts, IO8_SCSI_PIN), ==, 0);

    submit_command(qts, 0x92, 0, DISK_TARGET, 0, cdb, sizeof(cdb), 0, NULL);
    wait_for_completion(qts);
    g_assert_cmpuint(head_f_pin(qts, IO8_SCSI_PIN), ==, 1);

    /* Taking the entry clears the adapter's interrupt, and the pin falls. */
    take_status(qts, 0x92, status);
    g_assert_cmphex(get16le(status + ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmpuint(head_f_pin(qts, IO8_SCSI_PIN), ==, 0);

    qtest_quit(qts);
    disks_remove(&disks);
}

int main(int argc, char **argv)
{
    static const InboundMaskCase inbound_masks[] = {
        { UINT64_C(1) << 15, false },
        { UINT64_C(1) << 14, false },
        { 0, true },
    };
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom();

    qtest_add_data_func("origin300/io8-scsi/identity", NULL, test_identity);
    qtest_add_data_func("origin300/io8-scsi/io8bt-command-register", NULL,
                        test_io8bt_command_register);
    qtest_add_data_func("origin300/io8-scsi/io8bt-latency-timer", NULL,
                        test_io8bt_latency_timer);
    qtest_add_data_func("origin300/io8-scsi/io8bt-power-management", NULL,
                        test_io8bt_power_management);
    qtest_add_data_func("origin300/io8-scsi/registers-through-the-fabric",
                        NULL, test_registers_through_the_fabric);
    qtest_add_data_func("origin300/io8-scsi/command-reaches-memory", NULL,
                        test_command_reaches_memory);
    qtest_add_data_func("origin300/io8-scsi/data-transfer-reaches-memory",
                        NULL, test_data_transfer_reaches_memory);
    qtest_add_data_func("origin300/io8-scsi/inbound-deny-15-allow-14",
                        &inbound_masks[0], test_scsi_dma_with_inbound_mask);
    qtest_add_data_func("origin300/io8-scsi/inbound-allow-15-deny-14",
                        &inbound_masks[1], test_scsi_dma_with_inbound_mask);
    qtest_add_data_func("origin300/io8-scsi/inbound-allow-both",
                        &inbound_masks[2], test_scsi_dma_with_inbound_mask);
    qtest_add_data_func("origin300/io8-scsi/write-transfer-from-memory",
                        NULL, test_write_transfer_from_memory);
    qtest_add_data_func("origin300/io8-scsi/channels-are-independent", NULL,
                        test_channels_are_independent);
    qtest_add_data_func("origin300/io8-scsi/completion-raises-pin", NULL,
                        test_completion_raises_head_f_pin);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_free(prom_path);
    return result;
}
