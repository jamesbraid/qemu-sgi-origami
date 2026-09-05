/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the scsi-tape sequential-access device, driven through the
 * QLogic ISP1040 adapter it sits behind on the Origin 200. The adapter's
 * own register contract is qlogic-isp-test's; this file asserts only what
 * the tape answers, with the adapter as the transport, so it attaches to
 * the adapter's graph node and adds the tape on each test's command line.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "scsi/constants.h"

/* The subset of the adapter contract this transport needs. */
#define ISP_REG_SEMAPHORE       0x0c
#define ISP_REG_HOST_CMD        0xc0
#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)
#define ISP_REG_REQUEST_QUEUE   (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE  (ISP_REG_MAILBOX_BASE + 0x0a)

#define ISP_MBOX_CMD_INIT_REQ_QUEUE  0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE  0x0011
#define ISP_MBOX_COMPLETE            0x4000

#define ISP_HOST_CMD_SHIFT           12
#define ISP_HOST_CMD_SET_HOST_INT    0x5
#define ISP_HOST_CMD_CLEAR_RISC_INT  0x7

#define ISP_QUEUE_ENTRY_SIZE    64
#define ISP_ENTRY_TYPE_REQUEST  0x01
#define ISP_REQ_OFF_HANDLE      4
#define ISP_REQ_OFF_DEVICE      8
#define ISP_REQ_OFF_CDBLEN      10
#define ISP_REQ_OFF_SEGCOUNT    18
#define ISP_REQ_OFF_CDB         20
#define ISP_REQ_OFF_SEGS        32

#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_SENSE_LEN    18
#define ISP_STATUS_OFF_SENSE_DATA   32
#define ISP_SCSI_STATUS_SENSE_VALID 0x0200
#define ISP_IOCB_COMPLETE           0x0000
#define ISP_IOCB_DATA_UNDERRUN      0x0015

/* Fixed-format sense: byte 2 flags, information field, ASC and ASCQ. */
#define SENSE_KEY(s)        ((s)[2] & 0x0f)
#define SENSE_FILEMARK(s)   ((s)[2] & 0x80)
#define SENSE_EOM(s)        ((s)[2] & 0x40)
#define SENSE_ILI(s)        ((s)[2] & 0x20)
#define SENSE_INFO(s)       ldl_be_p((s) + 3)
#define SENSE_ASC(s)        ((s)[12])
#define SENSE_ASCQ(s)       ((s)[13])

/* Beside the adapter node's disk at target 0. */
#define TAPE_ID    5
#define RING       8

/* Same layout the adapter test's node object has; the graph hands it over. */
typedef struct QQlogicIsp {
    QOSGraphObject obj;
    QPCIDevice dev;
} QQlogicIsp;

typedef struct TapeSession {
    QPCIDevice *dev;
    QPCIBar io;
    QTestState *qts;
    QGuestAllocator *alloc;
    uint64_t rqst, resp;
    uint32_t slot;
    uint32_t handle;
} TapeSession;

typedef struct TapeStatus {
    uint8_t scsi_status;
    bool sense_valid;
    uint16_t completion;
    uint16_t sense_len;
    uint8_t sense[32];
} TapeStatus;

static void isp_put32(uint8_t *entry, int off, uint32_t val)
{
    uint32_t le = cpu_to_le32(val);

    memcpy(entry + off, &le, 4);
}

static void isp_put16(uint8_t *entry, int off, uint16_t val)
{
    uint16_t le = cpu_to_le16(val);

    memcpy(entry + off, &le, 2);
}

static uint32_t isp_get32(const uint8_t *entry, int off)
{
    uint32_t le;

    memcpy(&le, entry + off, 4);
    return le32_to_cpu(le);
}

static uint16_t isp_get16(const uint8_t *entry, int off)
{
    uint16_t le;

    memcpy(&le, entry + off, 2);
    return le16_to_cpu(le);
}

static void isp_ack(TapeSession *s)
{
    qpci_io_writew(s->dev, s->io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    qpci_io_writew(s->dev, s->io, ISP_REG_SEMAPHORE, 0);
}

static uint16_t isp_init_queue(TapeSession *s, uint16_t cmd, uint16_t count,
                               uint64_t addr)
{
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(1), count);
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(2), (addr >> 16) & 0xffff);
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(3), addr & 0xffff);
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(6), (addr >> 48) & 0xffff);
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(7), (addr >> 32) & 0xffff);
    qpci_io_writew(s->dev, s->io, ISP_MAILBOX(0), cmd);
    qpci_io_writew(s->dev, s->io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    return qpci_io_readw(s->dev, s->io, ISP_MAILBOX(0));
}

static void session_open(TapeSession *s, void *obj, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;

    s->dev = &isp->dev;
    s->alloc = alloc;
    qpci_device_enable(s->dev);
    s->io = qpci_iomap(s->dev, 0, NULL);
    s->qts = s->dev->bus->qts;
    s->rqst = guest_alloc(alloc, RING * ISP_QUEUE_ENTRY_SIZE);
    s->resp = guest_alloc(alloc, RING * ISP_QUEUE_ENTRY_SIZE);
    s->slot = 0;
    s->handle = 0x100;

    g_assert_cmphex(isp_init_queue(s, ISP_MBOX_CMD_INIT_REQ_QUEUE, RING,
                                   s->rqst), ==, ISP_MBOX_COMPLETE);
    isp_ack(s);
    g_assert_cmphex(isp_init_queue(s, ISP_MBOX_CMD_INIT_RSP_QUEUE, RING,
                                   s->resp), ==, ISP_MBOX_COMPLETE);
    isp_ack(s);
}

static void session_close(TapeSession *s)
{
    guest_free(s->alloc, s->rqst);
    guest_free(s->alloc, s->resp);
    qpci_iounmap(s->dev, s->io);
}

/*
 * One command through the ring: the entry goes into the next slot, the
 * producer index rings the doorbell, and the status the adapter posts in
 * the matching response slot comes back decoded. Data, in either
 * direction, is one segment at a guest buffer.
 */
static void tape_cmd(TapeSession *s, const uint8_t *cdb, int cdblen,
                     uint64_t data, uint32_t datalen, TapeStatus *st)
{
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint32_t slot = s->slot;
    uint32_t next = (slot + 1) % RING;
    uint32_t handle = s->handle++;

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, handle);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, TAPE_ID << 8);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, cdblen);
    memcpy(entry + ISP_REQ_OFF_CDB, cdb, cdblen);
    if (datalen) {
        isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 1);
        isp_put32(entry, ISP_REQ_OFF_SEGS + 0, (uint32_t)data);
        isp_put32(entry, ISP_REQ_OFF_SEGS + 4, datalen);
    }
    qtest_memwrite(s->qts, s->rqst + slot * ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(s->dev, s->io, ISP_REG_REQUEST_QUEUE, next);

    g_assert_cmphex(qpci_io_readw(s->dev, s->io, ISP_REG_RESPONSE_QUEUE),
                    ==, next);
    qtest_memread(s->qts, s->resp + slot * ISP_QUEUE_ENTRY_SIZE, status,
                  sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, handle);

    memset(st, 0, sizeof(*st));
    st->scsi_status = isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS) & 0xff;
    st->sense_valid = isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS) &
                      ISP_SCSI_STATUS_SENSE_VALID;
    st->completion = isp_get16(status, ISP_STATUS_OFF_COMPLETION);
    st->sense_len = isp_get16(status, ISP_STATUS_OFF_SENSE_LEN);
    memcpy(st->sense, status + ISP_STATUS_OFF_SENSE_DATA, sizeof(st->sense));

    isp_ack(s);
    qpci_io_writew(s->dev, s->io, ISP_REG_RESPONSE_QUEUE,
                   qpci_io_readw(s->dev, s->io, ISP_REG_RESPONSE_QUEUE));
    s->slot = next;
}

static void tape_good(TapeSession *s, const uint8_t *cdb, int cdblen,
                      uint64_t data, uint32_t datalen)
{
    TapeStatus st;

    tape_cmd(s, cdb, cdblen, data, datalen, &st);
    g_assert_cmphex(st.completion, ==, ISP_IOCB_COMPLETE);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
}

/*
 * A CHECK CONDITION whose sense the caller then inspects. A command that
 * fails before its data phase leaves the whole transfer unmoved, which the
 * adapter reports as a data underrun; that code is the adapter's contract,
 * asserted in its own test, so either completion is accepted here.
 */
static void tape_check(TapeSession *s, const uint8_t *cdb, int cdblen,
                       uint64_t data, uint32_t datalen, TapeStatus *st)
{
    tape_cmd(s, cdb, cdblen, data, datalen, st);
    g_assert_true(st->completion == ISP_IOCB_COMPLETE ||
                  st->completion == ISP_IOCB_DATA_UNDERRUN);
    g_assert_cmphex(st->scsi_status, ==, CHECK_CONDITION);
    g_assert_true(st->sense_valid);
    g_assert_cmpint(st->sense_len, >, 13);
}

/*
 * A freshly attached device answers its first non-exempt command with the
 * power-on unit attention. Tests that want a quiet device clear it first.
 */
static void tape_clear_ua(TapeSession *s)
{
    uint8_t cdb[6] = { TEST_UNIT_READY };
    TapeStatus st;

    tape_check(s, cdb, 6, 0, 0, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, UNIT_ATTENTION);
}

static void cdb_len24(uint8_t *cdb, uint32_t len)
{
    cdb[2] = len >> 16;
    cdb[3] = len >> 8;
    cdb[4] = len;
}

static void tape_write(TapeSession *s, uint64_t data, uint32_t len)
{
    uint8_t cdb[6] = { WRITE_6 };

    cdb_len24(cdb, len);
    tape_good(s, cdb, 6, data, len);
}

static void tape_write_fixed(TapeSession *s, uint64_t data, uint32_t blocks,
                             uint32_t bs)
{
    uint8_t cdb[6] = { WRITE_6, 0x01 };

    cdb_len24(cdb, blocks);
    tape_good(s, cdb, 6, data, blocks * bs);
}

static void tape_rewind(TapeSession *s)
{
    uint8_t cdb[6] = { REWIND };

    tape_good(s, cdb, 6, 0, 0);
}

static void tape_filemarks(TapeSession *s, uint32_t count)
{
    uint8_t cdb[6] = { WRITE_FILEMARKS };

    cdb_len24(cdb, count);
    tape_good(s, cdb, 6, 0, 0);
}

static void tape_space(TapeSession *s, uint8_t code, int32_t count,
                       TapeStatus *st)
{
    uint8_t cdb[6] = { SPACE, code };

    cdb_len24(cdb, (uint32_t)count & 0xffffff);
    tape_cmd(s, cdb, 6, 0, 0, st);
}

static uint32_t tape_read_position(TapeSession *s)
{
    uint8_t cdb[10] = { READ_POSITION };
    uint64_t data = guest_alloc(s->alloc, 20);
    uint8_t out[20];

    tape_good(s, cdb, 10, data, 20);
    qtest_memread(s->qts, data, out, sizeof(out));
    guest_free(s->alloc, data);
    return ldl_be_p(out + 4);
}

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        buf[i] = seed + i * 7;
    }
}

/* Each test boots its own machine; the tape gets a fresh, empty image. */
static void tape_image_destroy(void *p)
{
    char *path = p;

    unlink(path);
    g_free(path);
}

static void *tape_before_common(GString *cmd_line, const char *drive_extra)
{
    char *path;
    int fd = g_file_open_tmp("qtest-scsi-tape.XXXXXX", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_string_append_printf(cmd_line,
                           " -drive id=tape0,if=none,file=%s,format=raw%s"
                           " -device scsi-tape,bus=scsi0.0,scsi-id=%d,"
                           "drive=tape0",
                           path, drive_extra, TAPE_ID);
    g_test_queue_destroy(tape_image_destroy, path);
    return NULL;
}

static void *tape_before(GString *cmd_line, void *arg)
{
    return tape_before_common(cmd_line, "");
}

static void *tape_before_readonly(GString *cmd_line, void *arg)
{
    return tape_before_common(cmd_line, ",readonly=on");
}

/* The identity a real Origin 200 reports, set through the properties. */
static void *tape_before_identity(GString *cmd_line, void *arg)
{
    void *ret = tape_before_common(cmd_line, "");

    g_string_append(cmd_line, ",vendor=ARCHIVE,product=Python,ver=1.23");
    return ret;
}

/* Peripheral type 1, removable, with the identity strings as configured. */
static void test_inquiry(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[6] = { INQUIRY, 0, 0, 0, 36, 0 };
    uint64_t buf = guest_alloc(alloc, 36);
    uint8_t out[36];

    session_open(&s, obj, alloc);
    tape_good(&s, cdb, 6, buf, 36);
    qtest_memread(s.qts, buf, out, sizeof(out));

    g_assert_cmphex(out[0] & 0x1f, ==, TYPE_TAPE);
    g_assert_cmphex(out[1] & 0x80, ==, 0x80);
    g_assert_cmpmem(out + 8, 8, "QEMU    ", 8);
    g_assert_cmpmem(out + 16, 16, "QEMU TAPE       ", 16);

    guest_free(alloc, buf);
    session_close(&s);
}

/* Ready once the power-on attention is cleared; an unloaded drive is not. */
static void test_unit_ready_and_load(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t tur[6] = { TEST_UNIT_READY };
    uint8_t unload[6] = { LOAD_UNLOAD, 0, 0, 0, 0x00, 0 };
    uint8_t load[6] = { LOAD_UNLOAD, 0, 0, 0, 0x01, 0 };
    uint8_t prevent[6] = { ALLOW_MEDIUM_REMOVAL, 0, 0, 0, 0x01, 0 };
    uint8_t allow[6] = { ALLOW_MEDIUM_REMOVAL, 0, 0, 0, 0x00, 0 };
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    tape_good(&s, tur, 6, 0, 0);

    tape_good(&s, unload, 6, 0, 0);
    tape_check(&s, tur, 6, 0, 0, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, NOT_READY);
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x3a);

    tape_good(&s, load, 6, 0, 0);
    tape_good(&s, tur, 6, 0, 0);

    tape_good(&s, prevent, 6, 0, 0);
    tape_check(&s, unload, 6, 0, 0, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, NOT_READY);
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x53);
    tape_good(&s, allow, 6, 0, 0);
    tape_good(&s, unload, 6, 0, 0);

    session_close(&s);
}

static void test_read_block_limits(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[6] = { READ_BLOCK_LIMITS };
    uint64_t buf = guest_alloc(alloc, 6);
    uint8_t out[6];

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    tape_good(&s, cdb, 6, buf, 6);
    qtest_memread(s.qts, buf, out, sizeof(out));

    g_assert_cmphex((out[1] << 16) | (out[2] << 8) | out[3], ==, 0xffffff);
    g_assert_cmphex(lduw_be_p(out + 4), ==, 1);

    guest_free(alloc, buf);
    session_close(&s);
}

/* Header, block descriptor, variable-length records, writable medium. */
static void test_mode_sense(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[6] = { MODE_SENSE, 0, 0x00, 0, 12, 0 };
    uint64_t buf = guest_alloc(alloc, 12);
    uint8_t out[12];

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    tape_good(&s, cdb, 6, buf, 12);
    qtest_memread(s.qts, buf, out, sizeof(out));

    g_assert_cmphex(out[0], ==, 11);
    g_assert_cmphex(out[2] & 0x80, ==, 0);
    g_assert_cmphex(out[2] & 0x70, ==, 0x10);
    g_assert_cmphex(out[3], ==, 8);
    g_assert_cmphex((out[9] << 16) | (out[10] << 8) | out[11], ==, 0);

    guest_free(alloc, buf);
    session_close(&s);
}

static void test_mode_sense_write_protect(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[6] = { MODE_SENSE, 0, 0x00, 0, 12, 0 };
    uint8_t wcdb[6] = { WRITE_6 };
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t out[12];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    tape_good(&s, cdb, 6, buf, 12);
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmphex(out[2] & 0x80, ==, 0x80);

    cdb_len24(wcdb, 512);
    tape_check(&s, wcdb, 6, buf, 512, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, DATA_PROTECT);

    guest_free(alloc, buf);
    session_close(&s);
}

/* Variable-length records come back with their own lengths after a rewind. */
static void test_write_rewind_read(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t a[100], b[301], out[512];

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);

    fill_pattern(a, sizeof(a), 0x11);
    fill_pattern(b, sizeof(b), 0x22);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    qtest_memwrite(s.qts, buf, b, sizeof(b));
    tape_write(&s, buf, sizeof(b));
    g_assert_cmpuint(tape_read_position(&s), ==, 2);

    tape_rewind(&s);
    g_assert_cmpuint(tape_read_position(&s), ==, 0);

    memset(out, 0, sizeof(out));
    qtest_memwrite(s.qts, buf, out, sizeof(out));
    cdb_len24(rcdb, sizeof(a));
    tape_good(&s, rcdb, 6, buf, sizeof(a));
    qtest_memread(s.qts, buf, out, sizeof(a));
    g_assert_cmpmem(out, sizeof(a), a, sizeof(a));

    cdb_len24(rcdb, sizeof(b));
    tape_good(&s, rcdb, 6, buf, sizeof(b));
    qtest_memread(s.qts, buf, out, sizeof(b));
    g_assert_cmpmem(out, sizeof(b), b, sizeof(b));

    guest_free(alloc, buf);
    session_close(&s);
}

/*
 * A variable read longer than the record delivers the record and reports
 * the difference through the incorrect-length indicator.
 */
static void test_read_length_mismatch(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t a[100], out[100];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    fill_pattern(a, sizeof(a), 0x33);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    tape_rewind(&s);

    cdb_len24(rcdb, 512);
    tape_check(&s, rcdb, 6, buf, 512, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, NO_SENSE);
    g_assert_true(SENSE_ILI(st.sense));
    g_assert_cmpint((int32_t)SENSE_INFO(st.sense), ==, 512 - 100);
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), a, sizeof(a));

    guest_free(alloc, buf);
    session_close(&s);
}

/*
 * A filemark stops a read and a block space with the filemark bit, leaving
 * the position past it; spacing by filemarks steps over it outright, and
 * spacing backwards over a record moves the position back one block.
 */
static void test_filemark_space_sense(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t a[64], b[64], out[64];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    fill_pattern(a, sizeof(a), 0x44);
    fill_pattern(b, sizeof(b), 0x55);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    tape_filemarks(&s, 1);
    qtest_memwrite(s.qts, buf, b, sizeof(b));
    tape_write(&s, buf, sizeof(b));
    g_assert_cmpuint(tape_read_position(&s), ==, 3);

    /* Read through the mark: record a, then the mark itself. */
    tape_rewind(&s);
    cdb_len24(rcdb, sizeof(a));
    tape_good(&s, rcdb, 6, buf, sizeof(a));
    tape_check(&s, rcdb, 6, buf, sizeof(a), &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, NO_SENSE);
    g_assert_true(SENSE_FILEMARK(st.sense));
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x00);
    g_assert_cmphex(SENSE_ASCQ(st.sense), ==, 0x01);
    g_assert_cmpuint(tape_read_position(&s), ==, 2);
    tape_good(&s, rcdb, 6, buf, sizeof(b));
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), b, sizeof(b));

    /* Space by blocks: one record, then the mark stops the second. */
    tape_rewind(&s);
    tape_space(&s, 0x00, 2, &st);
    g_assert_cmphex(st.scsi_status, ==, CHECK_CONDITION);
    g_assert_true(SENSE_FILEMARK(st.sense));
    g_assert_cmpint((int32_t)SENSE_INFO(st.sense), ==, 1);
    g_assert_cmpuint(tape_read_position(&s), ==, 2);

    /* Space by filemarks steps over records to the far side of the mark. */
    tape_rewind(&s);
    tape_space(&s, 0x01, 1, &st);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
    g_assert_cmpuint(tape_read_position(&s), ==, 2);

    /* Back over one record. */
    tape_rewind(&s);
    tape_space(&s, 0x00, 1, &st);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
    tape_space(&s, 0x00, -1, &st);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
    g_assert_cmpuint(tape_read_position(&s), ==, 0);

    /* Back past beginning-of-tape is reported, not wrapped. */
    tape_space(&s, 0x00, -1, &st);
    g_assert_cmphex(st.scsi_status, ==, CHECK_CONDITION);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, BLANK_CHECK);
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x00);
    g_assert_cmphex(SENSE_ASCQ(st.sense), ==, 0x04);

    guest_free(alloc, buf);
    session_close(&s);
}

/* A blank tape is end-of-data at beginning-of-tape. */
static void test_end_of_data(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 64);
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    cdb_len24(rcdb, 64);
    tape_check(&s, rcdb, 6, buf, 64, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, BLANK_CHECK);
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x00);
    g_assert_cmphex(SENSE_ASCQ(st.sense), ==, 0x05);

    guest_free(alloc, buf);
    session_close(&s);
}

/* MODE SELECT sets a fixed block length that fixed-mode I/O then honours. */
static void test_mode_select_fixed(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t sel[6] = { MODE_SELECT, 0x10, 0, 0, 12, 0 };
    uint8_t sense[6] = { MODE_SENSE, 0, 0x00, 0, 12, 0 };
    uint8_t rcdb[6] = { READ_6, 0x01 };
    uint8_t params[12] = { 0, 0, 0, 8, 0, 0, 0, 0, 0, 0x00, 0x02, 0x00 };
    uint64_t buf = guest_alloc(alloc, 1024);
    uint8_t in[1024], out[1024];

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    qtest_memwrite(s.qts, buf, params, sizeof(params));
    tape_good(&s, sel, 6, buf, sizeof(params));
    tape_good(&s, sense, 6, buf, 12);
    qtest_memread(s.qts, buf, out, 12);
    g_assert_cmphex((out[9] << 16) | (out[10] << 8) | out[11], ==, 512);

    fill_pattern(in, sizeof(in), 0x66);
    qtest_memwrite(s.qts, buf, in, sizeof(in));
    tape_write_fixed(&s, buf, 2, 512);
    g_assert_cmpuint(tape_read_position(&s), ==, 2);

    tape_rewind(&s);
    memset(out, 0, sizeof(out));
    qtest_memwrite(s.qts, buf, out, sizeof(out));
    cdb_len24(rcdb, 2);
    tape_good(&s, rcdb, 6, buf, sizeof(out));
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), in, sizeof(in));

    guest_free(alloc, buf);
    session_close(&s);
}

/* Erase from the position drops everything after it. */
static void test_erase(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t erase[6] = { ERASE };
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 64);
    uint8_t a[64];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    fill_pattern(a, sizeof(a), 0x77);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    tape_rewind(&s);
    tape_space(&s, 0x00, 1, &st);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
    tape_good(&s, erase, 6, 0, 0);

    tape_rewind(&s);
    tape_space(&s, 0x00, 1, &st);
    g_assert_cmphex(st.scsi_status, ==, GOOD);
    cdb_len24(rcdb, sizeof(a));
    tape_check(&s, rcdb, 6, buf, sizeof(a), &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, BLANK_CHECK);

    guest_free(alloc, buf);
    session_close(&s);
}

/* LOCATE positions to a logical object counted from beginning-of-tape. */
static void test_locate(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t locate[10] = { LOCATE_10, 0, 0, 0, 0, 0, 2, 0, 0, 0 };
    uint8_t rcdb[6] = { READ_6 };
    uint64_t buf = guest_alloc(alloc, 64);
    uint8_t a[64], b[64], c[64], out[64];

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    fill_pattern(a, sizeof(a), 0x01);
    fill_pattern(b, sizeof(b), 0x02);
    fill_pattern(c, sizeof(c), 0x03);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    qtest_memwrite(s.qts, buf, b, sizeof(b));
    tape_write(&s, buf, sizeof(b));
    qtest_memwrite(s.qts, buf, c, sizeof(c));
    tape_write(&s, buf, sizeof(c));

    tape_good(&s, locate, 10, 0, 0);
    g_assert_cmpuint(tape_read_position(&s), ==, 2);
    cdb_len24(rcdb, sizeof(c));
    tape_good(&s, rcdb, 6, buf, sizeof(c));
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), c, sizeof(c));

    guest_free(alloc, buf);
    session_close(&s);
}

/* An opcode the tape does not implement is an illegal request, not GOOD. */
static void test_illegal_opcode(void *obj, void *data,
                                QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[10] = { READ_10 };
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    tape_check(&s, cdb, 10, 0, 0, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, ILLEGAL_REQUEST);
    g_assert_cmphex(SENSE_ASC(st.sense), ==, 0x20);

    session_close(&s);
}

/*
 * Configured identity strings land in their fields, space-padded, and the
 * bytes no handler writes are zero -- no WBus16, Sync or CmdQue claimed.
 */
static void test_inquiry_identity(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t cdb[6] = { INQUIRY, 0, 0, 0, 36, 0 };
    uint64_t buf = guest_alloc(alloc, 36);
    uint8_t out[36];

    session_open(&s, obj, alloc);
    tape_good(&s, cdb, 6, buf, 36);
    qtest_memread(s.qts, buf, out, sizeof(out));

    g_assert_cmpmem(out + 8, 8, "ARCHIVE ", 8);
    g_assert_cmpmem(out + 16, 16, "Python          ", 16);
    g_assert_cmpmem(out + 32, 4, "1.23", 4);
    g_assert_cmphex(out[5], ==, 0);
    g_assert_cmphex(out[6], ==, 0);
    g_assert_cmphex(out[7], ==, 0);

    guest_free(alloc, buf);
    session_close(&s);
}

/* A zero allocation length is a complete command, not a hung one. */
static void test_zero_allocation_length(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t inq[6] = { INQUIRY, 0, 0, 0, 0, 0 };
    uint8_t sense[6] = { MODE_SENSE, 0, 0x00, 0, 0, 0 };

    session_open(&s, obj, alloc);
    tape_good(&s, inq, 6, 0, 0);
    tape_clear_ua(&s);
    tape_good(&s, sense, 6, 0, 0);
    session_close(&s);
}

/*
 * The sense a check condition stored is what REQUEST SENSE returns; a
 * GOOD completion clears it, after which the tape answers no sense itself.
 */
static void test_request_sense(void *obj, void *data, QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t rcdb[6] = { READ_6 };
    uint8_t rs[6] = { REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t tur[6] = { TEST_UNIT_READY };
    uint64_t buf = guest_alloc(alloc, 64);
    uint8_t out[18];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    cdb_len24(rcdb, 64);
    tape_check(&s, rcdb, 6, buf, 64, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, BLANK_CHECK);

    tape_good(&s, rs, 6, buf, 18);
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmphex(out[0] & 0x7f, ==, 0x70);
    g_assert_cmphex(SENSE_KEY(out), ==, BLANK_CHECK);
    g_assert_cmphex(SENSE_ASCQ(out), ==, 0x05);

    tape_good(&s, tur, 6, 0, 0);
    tape_good(&s, rs, 6, buf, 18);
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmphex(out[0] & 0x7f, ==, 0x70);
    g_assert_cmphex(SENSE_KEY(out), ==, NO_SENSE);

    guest_free(alloc, buf);
    session_close(&s);
}

/*
 * A fixed-mode read of a record shorter than the block length delivers the
 * record and reports the wrong-length block as not transferred: the
 * information field holds the full block count, and the position is past
 * the record.
 */
static void test_read_fixed_length_mismatch(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    TapeSession s;
    uint8_t sel[6] = { MODE_SELECT, 0x10, 0, 0, 12, 0 };
    uint8_t params[12] = { 0, 0, 0, 8, 0, 0, 0, 0, 0, 0x00, 0x02, 0x00 };
    uint8_t rcdb[6] = { READ_6, 0x01 };
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t a[100], out[100];
    TapeStatus st;

    session_open(&s, obj, alloc);
    tape_clear_ua(&s);
    fill_pattern(a, sizeof(a), 0x88);
    qtest_memwrite(s.qts, buf, a, sizeof(a));
    tape_write(&s, buf, sizeof(a));
    tape_rewind(&s);

    qtest_memwrite(s.qts, buf, params, sizeof(params));
    tape_good(&s, sel, 6, buf, sizeof(params));

    cdb_len24(rcdb, 1);
    tape_check(&s, rcdb, 6, buf, 512, &st);
    g_assert_cmphex(SENSE_KEY(st.sense), ==, NO_SENSE);
    g_assert_true(SENSE_ILI(st.sense));
    g_assert_cmpint((int32_t)SENSE_INFO(st.sense), ==, 1);
    qtest_memread(s.qts, buf, out, sizeof(out));
    g_assert_cmpmem(out, sizeof(out), a, sizeof(a));
    g_assert_cmpuint(tape_read_position(&s), ==, 1);

    guest_free(alloc, buf);
    session_close(&s);
}

static void scsi_tape_register_tests(void)
{
    QOSGraphTestOptions opts = { .before = tape_before };
    QOSGraphTestOptions ro_opts = { .before = tape_before_readonly };
    QOSGraphTestOptions id_opts = { .before = tape_before_identity };

    qos_add_test("scsi-tape/inquiry", "qlogic-isp1040", test_inquiry, &opts);
    qos_add_test("scsi-tape/unit-ready-and-load", "qlogic-isp1040",
                 test_unit_ready_and_load, &opts);
    qos_add_test("scsi-tape/read-block-limits", "qlogic-isp1040",
                 test_read_block_limits, &opts);
    qos_add_test("scsi-tape/mode-sense", "qlogic-isp1040", test_mode_sense,
                 &opts);
    qos_add_test("scsi-tape/mode-sense-write-protect", "qlogic-isp1040",
                 test_mode_sense_write_protect, &ro_opts);
    qos_add_test("scsi-tape/write-rewind-read", "qlogic-isp1040",
                 test_write_rewind_read, &opts);
    qos_add_test("scsi-tape/read-length-mismatch", "qlogic-isp1040",
                 test_read_length_mismatch, &opts);
    qos_add_test("scsi-tape/filemark-space-sense", "qlogic-isp1040",
                 test_filemark_space_sense, &opts);
    qos_add_test("scsi-tape/end-of-data", "qlogic-isp1040", test_end_of_data,
                 &opts);
    qos_add_test("scsi-tape/mode-select-fixed", "qlogic-isp1040",
                 test_mode_select_fixed, &opts);
    qos_add_test("scsi-tape/erase", "qlogic-isp1040", test_erase, &opts);
    qos_add_test("scsi-tape/locate", "qlogic-isp1040", test_locate, &opts);
    qos_add_test("scsi-tape/illegal-opcode", "qlogic-isp1040",
                 test_illegal_opcode, &opts);
    qos_add_test("scsi-tape/inquiry-identity", "qlogic-isp1040",
                 test_inquiry_identity, &id_opts);
    qos_add_test("scsi-tape/zero-allocation-length", "qlogic-isp1040",
                 test_zero_allocation_length, &opts);
    qos_add_test("scsi-tape/request-sense", "qlogic-isp1040",
                 test_request_sense, &opts);
    qos_add_test("scsi-tape/read-fixed-length-mismatch", "qlogic-isp1040",
                 test_read_fixed_length_mismatch, &opts);
}

libqos_init(scsi_tape_register_tests);
