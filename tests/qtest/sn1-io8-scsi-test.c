/*
 * The Origin 300 IO-8 board's QLogic ISP12160, reached the way a guest
 * reaches it. Register cycles cross crosstalk into XBridge head F and out
 * of head F's programmed I/O window for slot 1; the adapter's own
 * transfers cross back through head F into Bedrock's memory.
 *
 * The part's own register contract belongs to sn0-isp12160-test.c and is
 * not repeated here. What this file pins is that the board carries the
 * adapter at the position the board puts it, and that both directions of
 * the fabric path between it and the processor's memory work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-test.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define XTALK_WIDGET(w) (SPACE_XTALK + (uint64_t)(w) * UINT64_C(0x1000000))
#define HUB_LOCAL_IO (SPACE_HUB + UINT64_C(0x10000000) + 0x400000)
#define IO_INBOUND_ACCESS 0x118

#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
/*
 * Origin 300's installed memory occupies locational banks 2 and 3, not
 * bank 0: map software bank 0 onto them so the request and response
 * rings, both under 4 MiB, land in backed RAM instead of an I/O hole.
 */
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

/* Head F carries the IO-8 board, and the adapter is device 1 on it. */
#define IO8_WIDGET 0xf
#define IO8_SCSI_SLOT 1
#define IO8_SCSI_PIN 1
#define IO8_SCSI_CFG (XTALK_WIDGET(IO8_WIDGET) + 0x20000 + \
                      IO8_SCSI_SLOT * 0x1000)

/*
 * A head's programmed I/O region runs from 0x200000 to 0xc00000 and is
 * not uniform: the XBridge datasheet's device-window table gives slots 0
 * and 1 two megabytes each from 0x200000, slots 2 to 7 one megabyte each
 * from 0x600000, and a four-megabyte section at 0xc00000. Slot 1 is
 * therefore the second two-megabyte block. The same table is what puts
 * the IOC3 at slot 4 on 0x800000 and the OHCI at slot 5 on 0x900000,
 * where sn1-address-map-test reaches them.
 */
#define IO8_SCSI_WINDOW (XTALK_WIDGET(IO8_WIDGET) + 0x400000)

/*
 * The window's target comes from the head's device register in units of a
 * megabyte, and a two-megabyte window ignores the low bit of that field,
 * so a slot 0 or 1 aperture has to be aligned to two megabytes. Nothing
 * else on this bus is enabled while the test runs, so the address is free.
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
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_INTERRUPT_LINE 0x3c

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
 * A constant fill, so a buffer left unwritten can never match either
 * disk's first sector: those walk their byte values and are constant
 * nowhere.
 */
#define BUFFER_SENTINEL 0xcc

/*
 * A 64-bit transfer address names its target widget in the top nibble and
 * passes the low 48 bits through to it. Bit 55 asks the head to leave the
 * byte lanes alone, so guest memory holds the stream the adapter produced
 * rather than its word-reversed image, and the fields below are read
 * straight back as the little-endian words the adapter wrote.
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
 * Each disk's first sector walks the byte values with its own stride, so
 * no two neighbours repeat and the two disks differ at every offset. A
 * payload that came back byte-reversed within a word, one sector late, or
 * off the other channel's medium is then a different sequence rather than
 * a plausible one. A constant fill would survive all three.
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

static uint32_t xtalk_reg_read(QTestState *qts, unsigned int widget,
                               uint64_t offset)
{
    return qtest_readl(qts, XTALK_WIDGET(widget) + offset + 4);
}

static void xtalk_reg_write(QTestState *qts, unsigned int widget,
                            uint64_t offset, uint32_t value)
{
    qtest_writel(qts, XTALK_WIDGET(widget) + offset + 4, value);
}

/*
 * Every 16-bit access to the adapter goes through the window's byte-lane
 * swizzle: the device register's swap bit is left at its reset-clear
 * value, so a host offset lands at the PCI offset with its low two bits
 * flipped, and the value itself is not touched. A 32-bit access is not
 * lane-swapped and needs no correction.
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
 * A whole submission: inputs into mailboxes 1 upwards, the opcode last,
 * then the host interrupt the protocol triggers on. The answer is read
 * without acknowledging, which is the caller's decision to make.
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
 * The aperture the adapter decodes, and the head window aimed at it. The
 * memory bar is used rather than the I/O one because the head's memory
 * space is where a slot's window can reach a 32-bit PCI address directly.
 * The device register's swap bit is left at its reset-clear value, which
 * is the word-invariant placement every driver on this bus assumes.
 */
static void io8_scsi_attach(QTestState *qts)
{
    qtest_writel(qts, IO8_SCSI_CFG + PCI_BAR1, ADAPTER_MMIO);
    qtest_writel(qts, IO8_SCSI_CFG + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_DEVICE(IO8_SCSI_SLOT),
                    XBRIDGE_DEVICE_MEMORY | (ADAPTER_MMIO >> 20));
}

static void io8_route_interrupt(QTestState *qts, unsigned int pin,
                                unsigned int pending_bit)
{
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_UPPER,
                    XBRIDGE_HOST_PORT << 16);
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_LOWER,
                    PI_REMOTE_WINDOW + PI_PENDING_MODIFY);
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_HOST(pin),
                    (1U << 17) | pending_bit);
    xtalk_reg_write(qts, IO8_WIDGET, XBRIDGE_INT_ENABLE, 1U << pin);
}

/* The head's interrupt status carries one bit per physical pin. */
static unsigned head_f_pin(QTestState *qts, unsigned int pin)
{
    return (xtalk_reg_read(qts, IO8_WIDGET, XBRIDGE_INT_STATUS) >> pin) & 1;
}

static QTestState *machine_start(void)
{
    g_autofree char *prom = g_shell_quote(prom_path);

    return qtest_initf(MACHINE_ARGS " -bios %s", prom);
}

/*
 * Channel 0 is bus scsi.0 and channel 1 is scsi.1: the adapter is the
 * board's only SCSI device, so its two channels take the first two names.
 * A wrong bus name is refused at start-up rather than quietly putting both
 * disks on one channel.
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
 * One queue initialisation. Mailbox 6 carries address bits 63:48 and
 * mailbox 7 bits 47:32, so a model that drops either upper word aims the
 * ring somewhere else. Mailboxes 4 and 5 carry the initial indices.
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
 * One command written into the next request slot and published. Every
 * field the adapter reads comes out of guest memory, so a broken outbound
 * path leaves the command unseen. At most one data descriptor is placed,
 * which is as much as a single-sector transfer describes; nothing here
 * needs the continuation entries a longer transfer would spill into.
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
 * Reading mailbox 5 answers the adapter's response producer index, so a
 * completion the test has not read yet is ready once that index moves.
 * The wait is bounded in wall time because a request held by the block
 * layer is waiting on its timer rather than on the guest clock.
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
 * The completion read out of guest memory and then released. Writing
 * mailbox 5 back frees the slot, and clearing the interrupt is what a
 * driver does once it has taken the entry.
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
 * READ(10) of one sector into one descriptor. The buffer is filled with a
 * sentinel first, so a transfer that never reaches memory leaves the
 * caller comparing the sentinel against the medium rather than against
 * whatever happened to be in the page.
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
 * WRITE(10) of one sector composed in guest memory out to the medium. The
 * adapter pulls the payload back across the head as a 64-bit direct
 * transfer, which is the DMA-read direction the READ path never drives:
 * READ has the head place the medium's bytes into memory, WRITE has it
 * fetch the caller's bytes out of memory.
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
 * The board's own record of what is in slot 1. A machine that creates the
 * adapter somewhere else, or not at all, answers all ones here, and a
 * machine that creates a different part of the family answers a different
 * device number and revision.
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
 * The register file through the head's slot-1 window. The identity pair
 * and the reset signature say the window reaches the adapter at all; the
 * mailbox register test writes a different pattern into each of seven
 * registers and reads them back, so a window landing a megabyte away, an
 * offset correction the head should not be applying, or aliased mailbox
 * storage all show up as a wrong word rather than as silence.
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
 * and the adapter reaches them over crosstalk, so an inbound path that
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
 * A data phase across the head. TEST UNIT READY moves nothing, so the
 * case above shows only that ring entries cross; this one puts a sector
 * of medium into a page of guest memory through the same descriptor a
 * driver would write, and compares every byte against what the fixture
 * put in the image. The residual has to be zero and the state flags have
 * to claim both the data phase and its completion: a status entry
 * reporting a transfer the adapter did not make is worse than one
 * reporting a failure.
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
    qtest_writeq(qts, HUB_LOCAL_IO + IO_INBOUND_ACCESS,
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
 * The other direction of the same 64-bit path: a sector composed in the
 * processor's memory and sent out to the medium, then read back. The head
 * fetches the payload out of memory (a DMA read) where the case above had
 * it deposit the payload into memory (a DMA write), so this is the outbound
 * lane the READ path leaves untried. The bytes walk with a stride that
 * shares nothing with either disk fixture and is not its own reverse within
 * a word: because the inbound path is already shown to land the medium's
 * bytes unreversed, a round trip that returns byte-for-byte can only mean
 * the head pulled the stream out of memory in the same orientation. A head
 * that word-reversed the outbound lanes, or aimed the fetch at the wrong
 * address, would put a different sequence on the medium and read it back
 * different. The transfer runs with bit 55 set, the same request IRIX
 * composes, so the head leaves the byte lanes alone in both directions.
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
 * Both channels, each carrying its own disk at the same target number.
 * The shared target number is the point: what picks the medium is the
 * channel bit of the request entry and nothing else. A machine that built
 * one bus and published two names for it, or an adapter whose second
 * channel reaches the first channel's disk, answers one disk's sector to
 * both reads, and the two sectors differ at every offset.
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
 * The interrupt the completion raises, on the pin the board wires it to.
 * The adapter's interrupt enables are left clear while the rings are set
 * up so the mailbox commands do not raise the line first, and the pin is
 * read before the command as well as after: a head that reports every pin
 * high would otherwise pass the raised half.
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
