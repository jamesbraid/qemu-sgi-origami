/*
 * SGI Crossbow registers and routing on an isolated switch, not Origin 2000.
 * Three RAM widget faces and per-port views distinguish packet entry ports
 * from destination widgets; machine wiring is tested with the machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "exec/hwaddr.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define XBOW "/machine/peripheral/xbow"

/*
 * The base I/O board sits at widget 8 and the two attached Hubs at 9 and 10,
 * which is the population the module's descriptor gives the first switch.
 * Widget 11 is attached to nothing.
 */
#define WIDGET_IO       8
#define WIDGET_HUB_A    9
#define WIDGET_HUB_B    10
#define WIDGET_EMPTY    11
#define WIDGET_LAST     15

/* Widget 0 and 8 through 15 are destinations. Nothing between them is. */
#define WIDGET_ILLEGAL_FIRST    1
#define WIDGET_ILLEGAL_LAST     7
#define WIDGET_ILLEGAL          3

#define PRESENT         0x0700
#define STARTUP         0x0700
#define SECOND_PRESENT  0x0600
#define SECOND_STARTUP  0x0600
/* Attached but not brought up at launch, so the two masks cannot be one. */
#define UNHEALTHY_START 0x0300
#define WIDGET_BIT(w)   (1u << (w))

/*
 * One widget's crosstalk space. The machine gives this the size its address
 * map defines; a smaller one here keeps the fixture's addresses readable and
 * exercises the property at the same time.
 */
#define SPAN            UINT64_C(0x1000000)
#define PORT_VIEW(w)    (UINT64_C(0x100000000) * ((w) - 7))
#define WIDGET_SLOT(view, w)  ((view) + SPAN * (w))

/* Where each widget's face is mapped for the test to read it directly. */
#define FACE_IO         UINT64_C(0x20000000)
#define FACE_HUB_A      UINT64_C(0x21000000)
#define FACE_HUB_B      UINT64_C(0x22000000)
#define FACE_SIZE       UINT64_C(0x10000)
#define SCRATCH         0x40

/* Registers sit in the low half of an eight-byte slot: offset plus four. */
#define REG_DATA        4
#define ID              0x0000
#define STATUS          0x0008
#define ERR_UPPER       0x0010
#define ERR_LOWER       0x0018
#define CONTROL         0x0020
#define PACKET_TIMEOUT  0x0028
#define INT_UPPER       0x0030
#define INT_LOWER       0x0038
#define ERR_CMD         0x0040
#define LLP_CONTROL     0x0048
#define STATUS_CLEAR    0x0050
#define ARB_RELOAD      0x0058
#define PERF_A          0x0060
#define PERF_B          0x0068
#define NIC             0x0070
#define RESERVED_FIRST  0x0078
#define RESERVED_LAST   0x00f8

#define LINK(w)         (0x0100 + ((w) - 8) * 0x40)
#define LINK_FLUSH      0x00
#define LINK_CONTROL    0x08
#define LINK_STATUS     0x10
#define LINK_ARB_UPPER  0x18
#define LINK_ARB_LOWER  0x20
#define LINK_STATUS_CLR 0x28
#define LINK_RESET      0x30
#define LINK_AUX        0x38

#define ID_REVISION(v)      (((v) >> 28) & 0xf)
#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE       (1u << 0)

#define STATUS_LINK_REQUEST(w)  (1u << (16 + (w)))
#define STATUS_REQUEST          (1u << 23)
#define STATUS_ACCESS_ERROR     (1u << 5)
#define STATUS_MULTIPLE_ERROR   (1u << 0)

/* Error command word: destination widget, then the port that sent it. */
#define ERR_CMD_DIDN(w)         ((uint32_t)(w) << 28)
#define ERR_CMD_SIDN(w)         ((uint32_t)(w) << 24)
#define CONTROL_WRITABLE        0x00000026
#define CONTROL_ACCESS_ENABLE   (1u << 5)

#define INT_UPPER_WRITABLE      0xff07ffff
#define INT_UPPER_TARGET_HIGH   (1u << 19)
#define INT_UPPER_RESET         0x00080000
#define INT_UPPER_TARGET(w)     ((uint32_t)(w) << 16)
#define INT_UPPER_VECTOR(v)     ((uint32_t)(v) << 24)
#define INT_ASSERT              (1u << 8)

#define LINK_ALIVE              (1u << 31)
#define LINK_MULTIPLE_ERROR     (1u << 18)
#define LINK_ILLEGAL_DEST       (1u << 17)
#define LINK_ILLEGAL_ENABLE     (1u << 17)
#define LINK_MAX_RETRY          (1u << 5)
#define LINK_FORCE_BAD          (1u << 23)
#define LINK_EIGHT_BIT_FORCE    (1u << 24)
#define LINK_ALIVE_ENABLE       (1u << 31)
#define LINK_MAX_RETRY_ENABLE   (1u << 5)
#define LINK_CONTROL_WRITABLE   ((1u << 31) | (3u << 28) | (7u << 25) | \
                                 (1u << 24) | (0x1fu << 18) | (1u << 17) | \
                                 (1u << 16) | 0x1ffu)
#define LINK_CREDIT_RESET       (2u << 18)
#define AUX_FAILURE_MODE        (1u << 6)
#define AUX_PRESENT             (1u << 5)
#define AUX_EIGHT_BIT           (1u << 4)

#define LINK_RESET_NS           20

static uint32_t reg_read_from(QTestState *qts, unsigned int port,
                              unsigned int offset)
{
    return qtest_readl(qts, PORT_VIEW(port) + offset + REG_DATA);
}

static void reg_write_from(QTestState *qts, unsigned int port,
                           unsigned int offset, uint32_t value)
{
    qtest_writel(qts, PORT_VIEW(port) + offset + REG_DATA, value);
}

static uint32_t reg_read(QTestState *qts, unsigned int offset)
{
    return reg_read_from(qts, WIDGET_HUB_A, offset);
}

static void reg_write(QTestState *qts, unsigned int offset, uint32_t value)
{
    reg_write_from(qts, WIDGET_HUB_A, offset, value);
}

static void set_links_alive(QTestState *qts, unsigned int widgets)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': {"
        "  'path': '" XBOW "', 'property': 'links-alive', 'value': %u } }",
        widgets);
}

static QTestState *xbow_start_with(unsigned int present, unsigned int startup,
                                   const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xtalk-test-target,id=io "
        "-device sgi-xtalk-test-target,id=huba "
        "-device sgi-xtalk-test-target,id=hubb "
        "-device sgi-xbow,id=xbow,span=%" PRIu64 ","
        "links-present=%u,startup-links-alive=%u,"
        "target-8=/machine/peripheral/io/io.ram[0],"
        "target-9=/machine/peripheral/huba/huba.ram[0],"
        "target-a=/machine/peripheral/hubb/hubb.ram[0] %s",
        SPAN, present, startup, extra);

    qtest_sysbus_mmio_map(qts, XBOW, WIDGET_IO - 8, PORT_VIEW(WIDGET_IO));
    qtest_sysbus_mmio_map(qts, XBOW, WIDGET_HUB_A - 8, PORT_VIEW(WIDGET_HUB_A));
    qtest_sysbus_mmio_map(qts, XBOW, WIDGET_HUB_B - 8, PORT_VIEW(WIDGET_HUB_B));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/io", 0, FACE_IO);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/huba", 0, FACE_HUB_A);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/hubb", 0, FACE_HUB_B);
    return qts;
}

static QTestState *xbow_start(const char *extra)
{
    return xbow_start_with(PRESENT, STARTUP, extra);
}

static void face_clear(QTestState *qts, uint64_t face)
{
    qtest_writeq(qts, face + SCRATCH, 0);
}

static uint64_t face_read(QTestState *qts, uint64_t face)
{
    return qtest_readq(qts, face + SCRATCH);
}

/* A real Crossbow reports part zero and manufacturer zero. */
static void test_identity(void)
{
    QTestState *qts = xbow_start("");
    uint32_t id = reg_read(qts, ID);

    g_assert_cmpuint(ID_PART(id), ==, 0);
    g_assert_cmpuint(ID_MANUFACTURER(id), ==, 0);
    g_assert_cmpuint(id & ID_ALWAYS_ONE, ==, ID_ALWAYS_ONE);
    g_assert_cmpuint(ID_REVISION(id), ==, 1);

    /* Every operational port reaches the one register block. */
    g_assert_cmphex(reg_read_from(qts, WIDGET_IO, ID), ==, id);
    g_assert_cmphex(reg_read_from(qts, WIDGET_HUB_B, ID), ==, id);
    reg_write_from(qts, WIDGET_HUB_B, CONTROL, CONTROL_ACCESS_ENABLE);
    g_assert_cmphex(reg_read_from(qts, WIDGET_IO, CONTROL), ==,
                    CONTROL_ACCESS_ENABLE);

    qtest_quit(qts);
}

static void test_configured_identity_survives_reset(void)
{
    QTestState *qts = xbow_start(
        "-global sgi-xbow.revision=5 "
        "-global sgi-xbow.part=0x1234 "
        "-global sgi-xbow.manufacturer=0x321");

    g_assert_cmphex(reg_read(qts, ID), ==, 0x51234643);
    qtest_system_reset(qts);
    g_assert_cmphex(reg_read(qts, ID), ==, 0x51234643);
    qtest_quit(qts);
}

static void test_reset_values(void)
{
    QTestState *qts = xbow_start("");
    unsigned int widget;

    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, 0);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, PACKET_TIMEOUT), ==, 0x000fffff);
    g_assert_cmphex(reg_read(qts, INT_UPPER), ==, INT_UPPER_RESET);
    g_assert_cmphex(reg_read(qts, INT_LOWER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==, 0);
    g_assert_cmphex(reg_read(qts, LLP_CONTROL), ==, 0x03ff1810);
    g_assert_cmphex(reg_read(qts, ARB_RELOAD), ==, 0x00000002);
    g_assert_cmphex(reg_read(qts, PERF_A), ==, 0x00800000);
    g_assert_cmphex(reg_read(qts, PERF_B), ==, 0x00800000);
    g_assert_cmphex(reg_read(qts, NIC), ==, 0x00000002);

    for (widget = WIDGET_IO; widget <= WIDGET_LAST; widget++) {
        g_assert_cmphex(reg_read(qts, LINK(widget) + LINK_CONTROL), ==,
                        LINK_CREDIT_RESET);
        g_assert_cmphex(reg_read(qts, LINK(widget) + LINK_ARB_UPPER), ==, 0);
        g_assert_cmphex(reg_read(qts, LINK(widget) + LINK_ARB_LOWER), ==, 0);
        /* Nothing is queued, so a flush has finished before it is read. */
        g_assert_cmphex(reg_read(qts, LINK(widget) + LINK_FLUSH), ==, 0);
    }
    qtest_quit(qts);
}

/* The per-port register sets decode where the specification puts them. */
static void test_link_register_decode(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex(LINK(8) + LINK_CONTROL, ==, 0x0108);
    g_assert_cmphex(LINK(9) + LINK_CONTROL, ==, 0x0148);
    g_assert_cmphex(LINK(15) + LINK_CONTROL, ==, 0x02c8);

    reg_write(qts, LINK(9) + LINK_ARB_UPPER, 0x12345678);
    g_assert_cmphex(reg_read(qts, LINK(9) + LINK_ARB_UPPER), ==, 0x12345678);
    g_assert_cmphex(reg_read(qts, LINK(8) + LINK_ARB_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, LINK(10) + LINK_ARB_UPPER), ==, 0);
    qtest_quit(qts);
}

/*
 * Attachment is reported whether or not a link trained. A port that never
 * came out of reset stays in initialization failure, whether it has a widget
 * on it or not.
 */
static void test_link_population(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS) & LINK_ALIVE,
                    ==, LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_AUX), ==,
                    AUX_PRESENT);

    g_assert_cmphex(reg_read(qts, LINK(WIDGET_EMPTY) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_EMPTY) + LINK_AUX), ==,
                    AUX_FAILURE_MODE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_LAST) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);

    /* A link taken down after coming up is a failure of a different kind. */
    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT);
    qtest_quit(qts);
}

/*
 * The force control says how a port should come up, not how it came up. Width
 * is what was negotiated, so it moves when the port next does, and the force
 * survives a link reset so software can reset a port into it.
 */
static void test_link_width_and_control(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, LINK(WIDGET_HUB_B) + LINK_CONTROL, 0xffffffff);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_CONTROL), ==,
                    LINK_CONTROL_WRITABLE & ~LINK_FORCE_BAD);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX) &
                    AUX_EIGHT_BIT, ==, 0);

    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    set_links_alive(qts, PRESENT);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX) &
                    AUX_EIGHT_BIT, ==, AUX_EIGHT_BIT);

    /*
     * The port asks for an interrupt, having come up with its arrival
     * notification on. A link reset must not answer that: the clearing
     * aliases are the only acknowledgment this chip has.
     */
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_LINK_REQUEST(WIDGET_HUB_B),
                    ==, STATUS_LINK_REQUEST(WIDGET_HUB_B));
    reg_write(qts, LINK(WIDGET_HUB_B) + LINK_RESET, 0);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_LINK_REQUEST(WIDGET_HUB_B),
                    ==, STATUS_LINK_REQUEST(WIDGET_HUB_B));
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_CONTROL) &
                    LINK_EIGHT_BIT_FORCE, ==, LINK_EIGHT_BIT_FORCE);
    qtest_quit(qts);
}

/*
 * A port with nothing on it goes to initialization failure and stays there.
 * Resetting its link needs no completion predicate, because the specification
 * states where an unattached port lands.
 */
static void test_link_reset_on_an_absent_port(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, LINK(WIDGET_EMPTY) + LINK_RESET, 0);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_EMPTY) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_EMPTY) + LINK_AUX), ==,
                    AUX_FAILURE_MODE);
    qtest_quit(qts);
}

/*
 * A reserved address, a write to a read-only register and a write to a
 * clearing alias are all register-access errors, and a second one while the
 * first stands reports the repeat.
 */
static void test_access_errors(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_ACCESS_ERROR, ==, 0);
    (void)reg_read(qts, RESERVED_FIRST);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);

    (void)reg_read(qts, RESERVED_LAST);
    g_assert_cmphex(reg_read(qts, STATUS), ==,
                    STATUS_ACCESS_ERROR | STATUS_MULTIPLE_ERROR);

    /* The clearing alias returns the value and negates the error bits. */
    g_assert_cmphex(reg_read(qts, STATUS_CLEAR), ==,
                    STATUS_ACCESS_ERROR | STATUS_MULTIPLE_ERROR);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);

    reg_write(qts, ID, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    g_assert_cmpuint(ID_PART(reg_read(qts, ID)), ==, 0);
    (void)reg_read(qts, STATUS_CLEAR);

    reg_write(qts, STATUS_CLEAR, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    (void)reg_read(qts, STATUS_CLEAR);

    reg_write(qts, LINK(WIDGET_IO) + LINK_STATUS, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    qtest_quit(qts);
}

/*
 * The established host form of a register access is the 32-bit access to the
 * data half. An eight-byte access has no established packet form, so the
 * model refuses it instead of answering, and refusing is not a report that
 * the chip saw an error.
 */
static void test_wide_access_is_refused(void)
{
    QTestState *qts = xbow_start("");

    /* The refusal answers nothing, where the 32-bit form answers here. */
    g_assert_cmphex(qtest_readq(qts, PORT_VIEW(WIDGET_HUB_A) + ID), ==, 0);
    g_assert_cmpuint(reg_read(qts, ID) & ID_ALWAYS_ONE, ==, ID_ALWAYS_ONE);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);

    qtest_writeq(qts, PORT_VIEW(WIDGET_HUB_A) + CONTROL, CONTROL_WRITABLE);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);

    /*
     * The same refusal at the data half, where a 32-bit access is served.
     * This is the form that could be mistaken for a wrong packet type and
     * reported as one; the width is unestablished, not wrong.
     */
    g_assert_cmphex(qtest_readq(qts, PORT_VIEW(WIDGET_HUB_A) + CONTROL +
                                REG_DATA), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    qtest_quit(qts);
}

static void test_writable_storage(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, CONTROL, 0xffffffff);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, CONTROL_WRITABLE);
    reg_write(qts, PACKET_TIMEOUT, 0xffffffff);
    g_assert_cmphex(reg_read(qts, PACKET_TIMEOUT), ==, 0x000fffff);
    reg_write(qts, LLP_CONTROL, 0xffffffff);
    g_assert_cmphex(reg_read(qts, LLP_CONTROL), ==, 0x03ffffff);
    reg_write(qts, ARB_RELOAD, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ARB_RELOAD), ==, 0x3f);
    reg_write(qts, PERF_A, 0xffffffff);
    g_assert_cmphex(reg_read(qts, PERF_A), ==, 0x00f00000);
    reg_write(qts, NIC, 0xffffffff);
    g_assert_cmphex(reg_read(qts, NIC), ==, 0x000fffff);

    /* Bit 19 of the destination restricts the target to widgets 8 to 15. */
    reg_write(qts, INT_UPPER, 0);
    g_assert_cmphex(reg_read(qts, INT_UPPER), ==, INT_UPPER_TARGET_HIGH);
    reg_write(qts, INT_UPPER, 0xffffffff);
    g_assert_cmphex(reg_read(qts, INT_UPPER), ==,
                    INT_UPPER_WRITABLE | INT_UPPER_TARGET_HIGH);
    reg_write(qts, INT_LOWER, 0xdeadbeef);
    g_assert_cmphex(reg_read(qts, INT_LOWER), ==, 0xdeadbeef);

    qtest_quit(qts);
}

/*
 * Writing the error command word clears it and both error addresses, and the
 * data written is discarded. Only the discard is pinned here: this device is
 * pristine, so all three registers were already zero and clearing them proves
 * nothing on its own. The illegal-destination tests cover the latch actually
 * holding and rearming.
 */
static void test_error_command_word(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, ERR_CMD, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, 0);
    qtest_quit(qts);
}

/*
 * One assertion is outstanding at a time: two enabled conditions produce one
 * packet, acknowledging one leaves it outstanding, and acknowledging the last
 * produces the deassertion.
 */
static void test_interrupt_coalescing(void)
{
    QTestState *qts = xbow_start("");
    const uint32_t vector = 0x5a;

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_HUB_B));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_B);

    /* An enabled error raises the widget's own request and one packet. */
    reg_write(qts, CONTROL, CONTROL_ACCESS_ENABLE);
    (void)reg_read(qts, RESERVED_FIRST);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_REQUEST, ==,
                    STATUS_REQUEST);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, INT_ASSERT | vector);

    /* A link condition accumulates behind the same assertion. */
    face_clear(qts, FACE_HUB_B);
    reg_write(qts, LINK(WIDGET_IO) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_MAX_RETRY_ENABLE);
    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_IO));
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_LINK_REQUEST(WIDGET_IO),
                    ==, STATUS_LINK_REQUEST(WIDGET_IO));
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);

    /* Acknowledging widget zero alone leaves the link request standing. */
    (void)reg_read(qts, STATUS_CLEAR);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_REQUEST, ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_LINK_REQUEST(WIDGET_IO),
                    ==, STATUS_LINK_REQUEST(WIDGET_IO));
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);

    /* Acknowledging the last one deasserts. */
    (void)reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS_CLR);
    g_assert_cmphex(reg_read(qts, STATUS) & 0xff800000, ==, 0);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, vector);
    qtest_quit(qts);
}

/*
 * Disabling a link's notification suppresses its request without
 * acknowledging the condition, and deasserts if it was the last one.
 */
static void test_interrupt_masking(void)
{
    QTestState *qts = xbow_start("");
    const uint32_t vector = 0x11;

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_HUB_A));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_A);

    reg_write(qts, LINK(WIDGET_IO) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_MAX_RETRY_ENABLE);
    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_IO));
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, INT_ASSERT | vector);

    reg_write(qts, LINK(WIDGET_IO) + LINK_CONTROL, LINK_CREDIT_RESET);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_LINK_REQUEST(WIDGET_IO),
                    ==, 0);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, vector);

    /* The condition was suppressed, not acknowledged: it is still there. */
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    qtest_quit(qts);
}

/* A link that comes up asks only when its arrival notification is on. */
static void test_interrupt_on_link_arrival(void)
{
    QTestState *qts = xbow_start("");
    const uint32_t vector = 0x27;

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_HUB_A));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_A);

    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    reg_write(qts, LINK(WIDGET_HUB_B) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_ALIVE_ENABLE);
    set_links_alive(qts, PRESENT);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, INT_ASSERT | vector);

    /*
     * The arrival is an event, not the level of the alive bit: once it is
     * acknowledged the request goes even though the link is still up.
     */
    (void)reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS_CLR);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS) &
                    LINK_ALIVE, ==, LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, STATUS) & 0xff800000, ==, 0);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, vector);
    qtest_quit(qts);
}

/* A packet's destination widget selects the port; the offset is unchanged. */
static void test_routes_by_destination_widget(void)
{
    QTestState *qts = xbow_start("");
    uint64_t from_a = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_IO);
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_HUB_B);

    face_clear(qts, FACE_IO);
    face_clear(qts, FACE_HUB_B);

    qtest_writeq(qts, from_a + SCRATCH, 0x1122334455667788);
    g_assert_cmphex(face_read(qts, FACE_IO), ==, 0x1122334455667788);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);

    qtest_writeq(qts, from_io + SCRATCH, 0x99aabbccddeeff00);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0x99aabbccddeeff00);
    qtest_quit(qts);
}

/* A link does not send to itself, so that slot decodes like an empty one. */
static void test_no_route_to_self(void)
{
    QTestState *qts = xbow_start("");
    uint64_t self = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_HUB_A);
    uint64_t empty = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_EMPTY);

    face_clear(qts, FACE_HUB_A);
    qtest_writeq(qts, self + SCRATCH, 0xdeadbeefcafef00d);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, 0);
    g_assert_cmphex(qtest_readq(qts, self + SCRATCH), ==,
                    qtest_readq(qts, empty + SCRATCH));
    qtest_quit(qts);
}

/*
 * A destination number that names no port is an error on the port the packet
 * arrived at. Reading the illegal slot cannot tell a correct switch from one
 * that never decoded it. Both answer nothing. Check which port's status
 * changed and which did not.
 */
static void test_illegal_destination(void)
{
    QTestState *qts = xbow_start("");
    uint64_t from_a = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_ILLEGAL);
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL);
    unsigned int widget;

    qtest_writeq(qts, from_a + SCRATCH, 0xcccccccccccccccc);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==,
                    LINK_ALIVE);

    /* A second one while the first stands reports the repeat. */
    (void)qtest_readq(qts, from_a + SCRATCH);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST | LINK_MULTIPLE_ERROR);

    /* The clearing alias returns both and leaves the port clean. */
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS_CLR), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST | LINK_MULTIPLE_ERROR);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS), ==,
                    LINK_ALIVE);

    /* Another port's bad packet lands on that port, not on the last one. */
    qtest_writeq(qts, from_io + SCRATCH, 0xdddddddddddddddd);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS), ==,
                    LINK_ALIVE);
    (void)reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS_CLR);

    /* Every number between the register block and the first link. */
    for (widget = WIDGET_ILLEGAL_FIRST; widget <= WIDGET_ILLEGAL_LAST;
         widget++) {
        qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), widget) + SCRATCH,
                     widget);
        g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS_CLR), ==,
                        LINK_ALIVE | LINK_ILLEGAL_DEST);
    }

    /* Widget zero is a destination, and the register block answers there. */
    g_assert_cmphex(reg_read_from(qts, WIDGET_IO, ID) & ID_ALWAYS_ONE, ==,
                    ID_ALWAYS_ONE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    qtest_quit(qts);
}

/*
 * The first illegal-destination error also fills the global capture
 * registers: the command word's DIDN is the widget the packet named and its
 * SIDN is the widget attached to the port that sent it, and the address
 * registers hold the offset the packet carried within that (illegal) widget's
 * own space. A repeat while the first still stands leaves the capture alone,
 * and writing the command word clears it and rearms capture for the next
 * error.
 */
static void test_illegal_destination_captures_command_and_address(void)
{
    QTestState *qts = xbow_start("");
    uint64_t from_a = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_ILLEGAL);
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL);
    uint64_t from_io_last = WIDGET_SLOT(PORT_VIEW(WIDGET_IO),
                                        WIDGET_ILLEGAL_LAST);

    qtest_writeq(qts, from_a + SCRATCH, 0);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==,
                    ERR_CMD_DIDN(WIDGET_ILLEGAL) | ERR_CMD_SIDN(WIDGET_HUB_A));
    g_assert_cmphex(reg_read(qts, ERR_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, SCRATCH);

    /* A second error from a different port while the first stands. */
    qtest_writeq(qts, from_io + SCRATCH, 0);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==,
                    ERR_CMD_DIDN(WIDGET_ILLEGAL) | ERR_CMD_SIDN(WIDGET_HUB_A));
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, SCRATCH);

    /* Writing it clears all three and rearms capture for the next error. */
    reg_write(qts, ERR_CMD, 0xffffffff);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, 0);

    qtest_writeq(qts, from_io_last + SPAN - 8, 0);
    g_assert_cmphex(reg_read(qts, ERR_CMD), ==,
                    ERR_CMD_DIDN(WIDGET_ILLEGAL_LAST) |
                    ERR_CMD_SIDN(WIDGET_IO));
    g_assert_cmphex(reg_read(qts, ERR_UPPER), ==, 0);
    g_assert_cmphex(reg_read(qts, ERR_LOWER), ==, SPAN - 8);
    qtest_quit(qts);
}

/*
 * A legal destination does not become illegal because nothing answers there.
 * A link with no widget on it, one the population leaves out, and one taken
 * down after coming up are all silent, which is what separates a number that
 * decodes to no port from a port that is not working.
 */
static void test_unavailable_destination_is_not_illegal(void)
{
    QTestState *qts = xbow_start("");

    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_EMPTY) +
                 SCRATCH, 0x1111111111111111);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_LAST) +
                 SCRATCH, 0x2222222222222222);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);

    face_clear(qts, FACE_HUB_B);
    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_HUB_B) +
                 SCRATCH, 0x3333333333333333);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    qtest_quit(qts);
}

/*
 * The request bit and the packet follow the port's own enable. With the
 * notification off the error still latches, so a test that only read the link
 * status would pass against a switch that interrupted regardless.
 */
static void test_illegal_destination_interrupt(void)
{
    QTestState *qts = xbow_start("");
    const uint32_t vector = 0x6d;
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL);

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_HUB_B));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_B);

    qtest_writeq(qts, from_io + SCRATCH, 0x1);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);
    (void)reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS_CLR);

    /* Enabled, the receiving port's request latches and one packet goes. */
    reg_write(qts, LINK(WIDGET_IO) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_ILLEGAL_ENABLE);
    qtest_writeq(qts, from_io + SCRATCH, 0x2);
    g_assert_cmphex(reg_read(qts, STATUS), ==,
                    STATUS_LINK_REQUEST(WIDGET_IO));
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, INT_ASSERT | vector);

    /* That link's alias acknowledges the error and its request together. */
    (void)reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS_CLR);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, vector);
    qtest_quit(qts);
}

/*
 * One error on this port, no repeat behind it, and clear it again. A macro
 * rather than a function so a failure names the access that caused it instead
 * of naming this line.
 */
#define assert_one_illegal_report(qts, widget) \
    g_assert_cmphex(reg_read(qts, LINK(widget) + LINK_STATUS_CLR), ==, \
                    LINK_ALIVE | LINK_ILLEGAL_DEST)

/*
 * A packet is wrong at its destination before its width matters, so every
 * access the host can issue is one report and never two: multiple error stays
 * clear each time. The last case starts inside the range and runs off the top
 * of it, which is still one packet naming where it began.
 */
static void test_illegal_destination_one_report_per_access(void)
{
    QTestState *qts = xbow_start("");
    uint64_t at = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL) + 0x33;

    qtest_writeb(qts, at, 0x11);
    assert_one_illegal_report(qts, WIDGET_IO);
    g_assert_cmphex(qtest_readb(qts, at), ==, 0);
    assert_one_illegal_report(qts, WIDGET_IO);

    qtest_writew(qts, at, 0x2233);
    assert_one_illegal_report(qts, WIDGET_IO);
    g_assert_cmphex(qtest_readw(qts, at), ==, 0);
    assert_one_illegal_report(qts, WIDGET_IO);

    qtest_writel(qts, at, 0x44556677);
    assert_one_illegal_report(qts, WIDGET_IO);
    g_assert_cmphex(qtest_readl(qts, at), ==, 0);
    assert_one_illegal_report(qts, WIDGET_IO);

    qtest_writeq(qts, at, 0x8899aabbccddeeff);
    assert_one_illegal_report(qts, WIDGET_IO);
    g_assert_cmphex(qtest_readq(qts, at), ==, 0);
    assert_one_illegal_report(qts, WIDGET_IO);

    /* Off the top of the range, from a port that has a route to widget 8. */
    qtest_writeq(qts, FACE_IO, 0);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A),
                                  WIDGET_ILLEGAL_LAST) + SPAN - 4,
                 0x1122334455667788);
    assert_one_illegal_report(qts, WIDGET_HUB_A);
    g_assert_cmphex(qtest_readq(qts, FACE_IO), ==, 0);
    qtest_quit(qts);
}

/*
 * A machine reset takes a standing illegal destination with it, request and
 * all. Nothing here is acknowledged first, so a reset that left the sticky
 * bits or the request behind would show.
 */
static void test_reset_clears_a_latched_illegal_destination(void)
{
    QTestState *qts = xbow_start("");
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL);

    reg_write(qts, LINK(WIDGET_IO) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_ILLEGAL_ENABLE);
    qtest_writeq(qts, from_io + SCRATCH, 0x1);
    qtest_writeq(qts, from_io + SCRATCH, 0x2);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST | LINK_MULTIPLE_ERROR);
    g_assert_cmphex(reg_read(qts, STATUS), ==,
                    STATUS_LINK_REQUEST(WIDGET_IO));

    qtest_system_reset(qts);

    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_CONTROL), ==,
                    LINK_CREDIT_RESET);
    qtest_quit(qts);
}

/*
 * A down destination link blocks what is addressed to it while the rest of
 * the switch keeps working, and a down source link blocks everything that
 * port sends, the register block included.
 */
static void test_link_failure_blocks_both_directions(void)
{
    QTestState *qts = xbow_start("");
    uint64_t a_to_io = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_IO);
    uint64_t a_to_b = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_HUB_B);
    uint64_t io_to_a = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_HUB_A);

    face_clear(qts, FACE_IO);
    face_clear(qts, FACE_HUB_A);
    face_clear(qts, FACE_HUB_B);

    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_IO));
    qtest_writeq(qts, a_to_io + SCRATCH, 0x1111111111111111);
    g_assert_cmphex(face_read(qts, FACE_IO), ==, 0);
    qtest_writeq(qts, a_to_b + SCRATCH, 0x2222222222222222);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0x2222222222222222);

    /* The same link down at the source blocks what that port sends. */
    qtest_writeq(qts, io_to_a + SCRATCH, 0x3333333333333333);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, 0);
    reg_write_from(qts, WIDGET_IO, CONTROL, CONTROL_WRITABLE);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);

    /* Bringing it back restores both directions. */
    set_links_alive(qts, PRESENT);
    qtest_writeq(qts, a_to_io + SCRATCH, 0x4444444444444444);
    g_assert_cmphex(face_read(qts, FACE_IO), ==, 0x4444444444444444);
    qtest_writeq(qts, io_to_a + SCRATCH, 0x5555555555555555);
    g_assert_cmphex(face_read(qts, FACE_HUB_A), ==, 0x5555555555555555);
    qtest_quit(qts);
}

/* An interrupt cannot leave by a link that is not operational. */
static void test_interrupt_needs_its_link(void)
{
    QTestState *qts = xbow_start("");

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(3) |
                              INT_UPPER_TARGET(WIDGET_HUB_B));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_B);

    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    reg_write(qts, CONTROL, CONTROL_ACCESS_ENABLE);
    (void)reg_read(qts, RESERVED_FIRST);
    g_assert_cmphex(reg_read(qts, STATUS) & STATUS_REQUEST, ==,
                    STATUS_REQUEST);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);
    qtest_quit(qts);
}

/*
 * Link reset drives the remote signal for two core cycles and leaves the port
 * in initialization: no reviewed source supplies a predicate for a link
 * having trained, so nothing here decides that one has.
 */
static void test_link_reset_pulse(void)
{
    QTestState *qts = xbow_start("");
    uint64_t a_to_b = WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_HUB_B);

    qtest_irq_intercept_out_named(qts, XBOW, "link-reset");
    face_clear(qts, FACE_HUB_B);

    /* Virtual time stands still while the machine is stopped. */
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");

    reg_write(qts, LINK(WIDGET_HUB_B) + LINK_RESET, 0);
    g_assert_true(qtest_get_irq(qts, WIDGET_HUB_B - 8));
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==, 0);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT);

    qtest_writeq(qts, a_to_b + SCRATCH, 0x6666666666666666);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);

    qtest_clock_step(qts, LINK_RESET_NS);
    g_assert_false(qtest_get_irq(qts, WIDGET_HUB_B - 8));
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==, 0);
    qtest_quit(qts);
}

/*
 * Check startup health and the absence of a reset-off write to this RAM
 * endpoint. Combined reset of actual Hub pending banks is a separate machine
 * acceptance test; this fixture cannot establish that receiving behavior.
 */
static void test_reset_restores_startup_health(void)
{
    QTestState *qts = xbow_start("");
    const uint32_t vector = 0x3c;

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_HUB_B));
    reg_write(qts, INT_LOWER, SCRATCH);
    face_clear(qts, FACE_HUB_B);
    reg_write(qts, CONTROL, CONTROL_ACCESS_ENABLE);
    (void)reg_read(qts, RESERVED_FIRST);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, INT_ASSERT | vector);

    /* Take a link the interrupt does not leave by, and change a register. */
    set_links_alive(qts, PRESENT & ~WIDGET_BIT(WIDGET_IO));
    reg_write(qts, PACKET_TIMEOUT, 0);

    qtest_system_reset(qts);

    /* No parting packet: what the assertion wrote is still what is there. */
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, INT_ASSERT | vector);

    g_assert_cmphex(reg_read(qts, PACKET_TIMEOUT), ==, 0x000fffff);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, 0);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS) & LINK_ALIVE,
                    ==, LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS) &
                    LINK_ALIVE, ==, LINK_ALIVE);

    face_clear(qts, FACE_IO);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_B), WIDGET_IO) +
                 SCRATCH, 0x7777777777777777);
    g_assert_cmphex(face_read(qts, FACE_IO), ==, 0x7777777777777777);
    qtest_quit(qts);
}

/*
 * Migration carries the mask the machine is running with, not the startup
 * one, along with the registers and the routing derived from both.
 */
static void test_migration_keeps_runtime_state(void)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *arrival = NULL;
    g_autofree char *socket = NULL;
    GError *err = NULL;
    QTestState *from = xbow_start("");
    QTestState *to;

    tmpdir = g_dir_make_tmp("sn0-xbow-test-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);
    socket = g_strdup_printf("%s/migrate.sock", tmpdir);

    g_assert_cmphex(reg_read(from, ID), ==, 0x10000001);
    reg_write(from, PACKET_TIMEOUT, 0x1234);
    reg_write(from, LINK(WIDGET_HUB_B) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_EIGHT_BIT_FORCE);
    set_links_alive(from, PRESENT & ~WIDGET_BIT(WIDGET_HUB_B));
    set_links_alive(from, PRESENT & ~WIDGET_BIT(WIDGET_IO));

    arrival = g_strdup_printf("-incoming %s", uri);
    to = xbow_start(arrival);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(reg_read(to, ID), ==, 0x10000001);
    g_assert_cmphex(reg_read(to, PACKET_TIMEOUT), ==, 0x1234);
    g_assert_cmphex(reg_read(to, LINK(WIDGET_HUB_B) + LINK_CONTROL), ==,
                    LINK_CREDIT_RESET | LINK_EIGHT_BIT_FORCE);
    g_assert_cmphex(reg_read(to, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(to, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT | AUX_EIGHT_BIT);

    /* The route the runtime mask took away is still gone on the far side. */
    face_clear(to, FACE_IO);
    qtest_writeq(to, WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_IO) +
                 SCRATCH, 0x8888888888888888);
    g_assert_cmphex(face_read(to, FACE_IO), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(tmpdir);
}

/*
 * A latched illegal destination is device state, so it crosses a migration
 * with its repeat and its request, and the far side can acknowledge it. Both
 * status arrays were already in the vmstate; this is what would fail if a
 * later change kept the condition somewhere else.
 */
static void test_illegal_destination_crosses_migration(void)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *arrival = NULL;
    g_autofree char *socket = NULL;
    GError *err = NULL;
    QTestState *from = xbow_start("");
    QTestState *to;
    uint64_t from_io = WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_ILLEGAL);

    tmpdir = g_dir_make_tmp("sn0-xbow-test-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpdir);
    socket = g_strdup_printf("%s/migrate.sock", tmpdir);

    reg_write(from, LINK(WIDGET_IO) + LINK_CONTROL,
              LINK_CREDIT_RESET | LINK_ILLEGAL_ENABLE);
    qtest_writeq(from, from_io + SCRATCH, 0x1);
    qtest_writeq(from, from_io + SCRATCH, 0x2);
    g_assert_cmphex(reg_read(from, STATUS), ==,
                    STATUS_LINK_REQUEST(WIDGET_IO));

    arrival = g_strdup_printf("-incoming %s", uri);
    to = xbow_start(arrival);
    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(reg_read(to, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE | LINK_ILLEGAL_DEST | LINK_MULTIPLE_ERROR);
    g_assert_cmphex(reg_read(to, STATUS), ==, STATUS_LINK_REQUEST(WIDGET_IO));

    /* The request the far side inherited is one it can acknowledge. */
    (void)reg_read(to, LINK(WIDGET_IO) + LINK_STATUS_CLR);
    g_assert_cmphex(reg_read(to, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_ALIVE);
    g_assert_cmphex(reg_read(to, STATUS), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(tmpdir);
}

/*
 * The module's second switch carries the two Hubs and no I/O board, so a
 * populated link and an empty one swap places against the first switch.
 */
static void test_second_population(void)
{
    QTestState *qts = xbow_start_with(SECOND_PRESENT, SECOND_STARTUP, "");

    /*
     * Presence comes from the population, not from whether a face happens to
     * be attached: this fixture attaches widget 8's, and the switch that does
     * not carry it must still report it absent.
     */
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_A) + LINK_STATUS) &
                    LINK_ALIVE, ==, LINK_ALIVE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_AUX), ==,
                    AUX_FAILURE_MODE);

    /* An absent widget's routes are gone with it, from every port. */
    face_clear(qts, FACE_IO);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_HUB_A), WIDGET_IO) +
                 SCRATCH, 0x9999999999999999);
    g_assert_cmphex(face_read(qts, FACE_IO), ==, 0);
    qtest_quit(qts);
}

/*
 * A register lives in the low half of its eight-byte slot. The upper half
 * holds none, so an access there is the wrong packet and an error.
 */
static void test_packet_boundary(void)
{
    QTestState *qts = xbow_start("");

    g_assert_cmphex(qtest_readl(qts, PORT_VIEW(WIDGET_HUB_A) + ID), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    (void)reg_read(qts, STATUS_CLEAR);

    qtest_writel(qts, PORT_VIEW(WIDGET_HUB_A) + CONTROL, CONTROL_WRITABLE);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    (void)reg_read(qts, STATUS_CLEAR);

    /* A one-byte access is not a doubleword packet either. */
    qtest_writeb(qts, PORT_VIEW(WIDGET_HUB_A) + CONTROL + REG_DATA, 0xff);
    g_assert_cmphex(reg_read(qts, CONTROL), ==, 0);
    g_assert_cmphex(reg_read(qts, STATUS), ==, STATUS_ACCESS_ERROR);
    qtest_quit(qts);
}

/*
 * The destination is 48 bits: the upper register carries address bits 47:32
 * beside the vector and the target, and the lower register the rest. Widget
 * 11 answers into the machine's own space here, because no widget face this
 * fixture can build reaches four gigabytes to prove the upper half moves.
 */
static void test_interrupt_destination_address(void)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors "
        "-device sgi-xtalk-test-target,id=huba "
        "-device sgi-xtalk-test-target,id=high "
        "-device sgi-xbow,id=xbow,span=%" PRIu64 ","
        "links-present=%u,startup-links-alive=%u,"
        "target-9=/machine/peripheral/huba/huba.ram[0],"
        "target-b=/machine/unattached/system[0]",
        SPAN, WIDGET_BIT(WIDGET_HUB_A) | WIDGET_BIT(WIDGET_EMPTY),
        WIDGET_BIT(WIDGET_HUB_A) | WIDGET_BIT(WIDGET_EMPTY));
    const uint32_t vector = 0x7e;
    const uint32_t upper_address = 0x0002;
    const uint64_t lower_address = 0x01000000;
    const uint64_t landing = ((uint64_t)upper_address << 32) | lower_address;

    qtest_sysbus_mmio_map(qts, XBOW, WIDGET_HUB_A - 8, PORT_VIEW(WIDGET_HUB_A));
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/high", 0, landing);

    reg_write(qts, INT_UPPER, INT_UPPER_VECTOR(vector) |
                              INT_UPPER_TARGET(WIDGET_EMPTY) | upper_address);
    reg_write(qts, INT_LOWER, lower_address + SCRATCH);
    qtest_writeq(qts, landing + SCRATCH, 0);

    reg_write(qts, CONTROL, CONTROL_ACCESS_ENABLE);
    (void)reg_read(qts, RESERVED_FIRST);
    g_assert_cmphex(qtest_readq(qts, landing + SCRATCH), ==,
                    INT_ASSERT | vector);

    /* Acknowledging sends the deassertion to the same 48-bit address. */
    (void)reg_read(qts, STATUS_CLEAR);
    g_assert_cmphex(qtest_readq(qts, landing + SCRATCH), ==, vector);
    qtest_quit(qts);
}

/*
 * Startup health and attachment are different things. A widget bolted to the
 * switch that the configuration does not bring up reports itself present and
 * failed, and a machine reset puts it back there however the running machine
 * had moved it.
 */
static void test_startup_health_is_not_attachment(void)
{
    QTestState *qts = xbow_start_with(PRESENT, UNHEALTHY_START, "");

    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT | AUX_FAILURE_MODE);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_IO) + LINK_STATUS) & LINK_ALIVE,
                    ==, LINK_ALIVE);

    /* A route to it does not exist until the machine brings the link up. */
    face_clear(qts, FACE_HUB_B);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_HUB_B) +
                 SCRATCH, 0xaaaaaaaaaaaaaaaa);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0);

    set_links_alive(qts, PRESENT);
    qtest_writeq(qts, WIDGET_SLOT(PORT_VIEW(WIDGET_IO), WIDGET_HUB_B) +
                 SCRATCH, 0xbbbbbbbbbbbbbbbb);
    g_assert_cmphex(face_read(qts, FACE_HUB_B), ==, 0xbbbbbbbbbbbbbbbb);

    /* Reset restores the configured health, not everything attached. */
    qtest_system_reset(qts);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_STATUS), ==,
                    LINK_MAX_RETRY);
    g_assert_cmphex(reg_read(qts, LINK(WIDGET_HUB_B) + LINK_AUX), ==,
                    AUX_PRESENT | AUX_FAILURE_MODE);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sn0/xbow/identity", test_identity);
    qtest_add_func("/sn0/xbow/configured-identity-survives-reset",
                   test_configured_identity_survives_reset);
    qtest_add_func("/sn0/xbow/reset-values", test_reset_values);
    qtest_add_func("/sn0/xbow/link-register-decode",
                   test_link_register_decode);
    qtest_add_func("/sn0/xbow/link-population", test_link_population);
    qtest_add_func("/sn0/xbow/second-population", test_second_population);
    qtest_add_func("/sn0/xbow/startup-health-is-not-attachment",
                   test_startup_health_is_not_attachment);
    qtest_add_func("/sn0/xbow/packet-boundary", test_packet_boundary);
    qtest_add_func("/sn0/xbow/interrupt-destination-address",
                   test_interrupt_destination_address);
    qtest_add_func("/sn0/xbow/link-width-and-control",
                   test_link_width_and_control);
    qtest_add_func("/sn0/xbow/link-reset-on-an-absent-port",
                   test_link_reset_on_an_absent_port);
    qtest_add_func("/sn0/xbow/access-errors", test_access_errors);
    qtest_add_func("/sn0/xbow/wide-access-is-refused",
                   test_wide_access_is_refused);
    qtest_add_func("/sn0/xbow/writable-storage", test_writable_storage);
    qtest_add_func("/sn0/xbow/error-command-word", test_error_command_word);
    qtest_add_func("/sn0/xbow/interrupt-coalescing",
                   test_interrupt_coalescing);
    qtest_add_func("/sn0/xbow/interrupt-masking", test_interrupt_masking);
    qtest_add_func("/sn0/xbow/interrupt-on-link-arrival",
                   test_interrupt_on_link_arrival);
    qtest_add_func("/sn0/xbow/routes-by-destination-widget",
                   test_routes_by_destination_widget);
    qtest_add_func("/sn0/xbow/no-route-to-self", test_no_route_to_self);
    qtest_add_func("/sn0/xbow/illegal-destination", test_illegal_destination);
    qtest_add_func("/sn0/xbow/illegal-destination-captures-command-and-address",
                   test_illegal_destination_captures_command_and_address);
    qtest_add_func("/sn0/xbow/unavailable-destination-is-not-illegal",
                   test_unavailable_destination_is_not_illegal);
    qtest_add_func("/sn0/xbow/illegal-destination-interrupt",
                   test_illegal_destination_interrupt);
    qtest_add_func("/sn0/xbow/illegal-destination-one-report-per-access",
                   test_illegal_destination_one_report_per_access);
    qtest_add_func("/sn0/xbow/reset-clears-a-latched-illegal-destination",
                   test_reset_clears_a_latched_illegal_destination);
    qtest_add_func("/sn0/xbow/illegal-destination-crosses-migration",
                   test_illegal_destination_crosses_migration);
    qtest_add_func("/sn0/xbow/link-failure-blocks-both-directions",
                   test_link_failure_blocks_both_directions);
    qtest_add_func("/sn0/xbow/interrupt-needs-its-link",
                   test_interrupt_needs_its_link);
    qtest_add_func("/sn0/xbow/link-reset-pulse", test_link_reset_pulse);
    qtest_add_func("/sn0/xbow/reset-restores-startup-health",
                   test_reset_restores_startup_health);
    qtest_add_func("/sn0/xbow/migration-keeps-runtime-state",
                   test_migration_keeps_runtime_state);

    return g_test_run();
}
