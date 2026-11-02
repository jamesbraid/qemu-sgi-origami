/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The C-brick, I-brick and V-brick profiles of the L1 controller. Each case
 * drives the controller through its UART window with the same protocol
 * frames the Origin 300 profile uses; no firmware runs.
 *
 * What these profiles answer is a declared model, not captured controller
 * output: the type letters, the attached-brick numbering and the command
 * refusals are choices that docs/system/mips/sgi/hardware/l1.rst marks as
 * approximations, and these cases pin the choices so a change to one is
 * deliberate.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"
#include "sgi-l1-test.h"
#include "sgi-l1-l2-stub.h"

#define BASE UINT64_C(0x11000000)
#define STRIDE UINT64_C(0x1000)
#define DEVICES "/machine/peripheral/"

/* Destinations: type 0 broadcast key, type 4 attached I/O brick, task 6. */
#define LOCAL_GENERAL  UINT32_C(0x0ffff006)
#define LOCAL_COMMAND  UINT32_C(0x0ffff003)
#define ATTACHED_IO    UINT32_C(0x4ffff006)
#define ATTACHED_C     UINT32_C(0x3ffff006)

#define MAC_PROP "02:ab:10:00:00:01"
#define MAC_TEXT "02AB10000001"
#define RESTART_STEP_NS (2 * NANOSECONDS_PER_SECOND)
/* The model restarts a brick for 1 s: probe just inside it, then past it. */
#define RESTART_INSIDE_NS (999 * NANOSECONDS_PER_SECOND / 1000)
#define RESTART_PAST_NS (2 * NANOSECONDS_PER_SECOND / 1000)

/* Packed rack and bay as the identity request returns them. */
static uint32_t packed(unsigned rack, unsigned bay)
{
    return (rack << 6) | bay;
}

/* Starts one machine with a controller and a UART window per property set. */
static QTestState *start_with(unsigned count, const char *const props[],
                              const char *extra)
{
    GString *line = g_string_new("-M none -cpu R10000 -S ");
    QTestState *s;

    for (unsigned i = 0; i < count; i++) {
        g_string_append_printf(line,
            "-device sgi-l1,id=l1c%u,%s "
            "-device sgi-l1-uart,id=l1u%u,controller=l1c%u ",
            i, props[i], i, i);
    }
    g_string_append(line, extra);
    s = qtest_init(line->str);
    g_string_free(line, TRUE);
    for (unsigned i = 0; i < count; i++) {
        g_autofree char *path = g_strdup_printf(DEVICES "l1u%u", i);

        qtest_sysbus_mmio_map(s, path, 0, BASE + i * STRIDE);
    }
    return s;
}

static QTestState *start_one(const char *props)
{
    const char *const all[] = { props };

    return start_with(1, all, "");
}

static void qom_link(QTestState *s, unsigned owner, const char *name,
                     const char *target)
{
    g_autofree char *path = g_strdup_printf(DEVICES "l1c%u", owner);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%s}}", path, name, target);
}

static bool qom_link_refused(QTestState *s, unsigned owner, const char *name,
                             const char *target)
{
    g_autofree char *path = g_strdup_printf(DEVICES "l1c%u", owner);
    QDict *response = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%s}}", path, name, target);
    bool refused = qdict_haskey(response, "error");

    qobject_unref(response);
    return refused;
}

static void link_io(QTestState *s, unsigned c, unsigned i)
{
    g_autofree char *target = g_strdup_printf(DEVICES "l1c%u", i);

    qom_link(s, c, "io-peer", target);
}

static void link_peer(QTestState *s, unsigned a, unsigned b)
{
    g_autofree char *to_b = g_strdup_printf(DEVICES "l1c%u", b);
    g_autofree char *to_a = g_strdup_printf(DEVICES "l1c%u", a);

    qom_link(s, a, "peer", to_b);
    qom_link(s, b, "peer", to_a);
}

static void send_rpc(QTestState *s, unsigned n, uint32_t destination,
                     uint16_t code, const uint8_t *args, size_t args_len,
                     unsigned count)
{
    uint8_t request[136] = { 0x10, destination >> 24, destination >> 16,
                             destination >> 8, destination, code >> 8, code,
                             count };

    g_assert_cmpuint(args_len, <=, sizeof(request) - 8);
    if (args_len) {
        memcpy(request + 8, args, args_len);
    }
    sgi_l1_test_send(s, BASE + n * STRIDE, request, 8 + args_len, false);
}

static size_t rpc(QTestState *s, unsigned n, uint32_t destination,
                  uint16_t code, const uint8_t *args, size_t args_len,
                  unsigned count, uint8_t *reply)
{
    send_rpc(s, n, destination, code, args, args_len, count);
    return sgi_l1_test_receive(s, BASE + n * STRIDE, reply, 131);
}

static void integer(uint8_t *out, uint32_t value)
{
    out[0] = 0;
    stl_be_p(out + 1, value);
}

static void expect_router_error(const uint8_t *reply, size_t length)
{
    static const uint8_t expected[] = { 0x30, 0xff, 0xff, 0xff, 0xff,
                                        0, 0, 0, 8 };

    if (length != sizeof(expected) || memcmp(reply, expected, length)) {
        g_autofree char *dump = g_strdup("");

        for (size_t i = 0; i < length; i++) {
            char *next = g_strdup_printf("%s %02x", dump, reply[i]);

            g_free(dump);
            dump = next;
        }
        fprintf(stderr, "not the router error:%s\n", dump);
    }
    g_assert_cmpuint(length, ==, sizeof(expected));
    g_assert_cmpmem(reply, length, expected, sizeof(expected));
}

static void expect_code(const uint8_t *reply, size_t length, int32_t code)
{
    g_assert_cmpuint(length, >=, 6);
    g_assert_cmpint((int32_t)ldl_be_p(reply + 1), ==, code);
}

static void expect_identity(QTestState *s, unsigned n, uint32_t destination,
                            uint32_t pack, char letter)
{
    uint8_t reply[131];
    size_t length = rpc(s, n, destination, 3, NULL, 0, 0, reply);

    g_assert_cmpuint(length, ==, 16);
    expect_code(reply, length, 0);
    g_assert_cmpuint(reply[5], ==, 2);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, pack);
    g_assert_cmphex(ldl_be_p(reply + 12), ==, (uint8_t)letter);
}

static void expect_unnamed_identity(QTestState *s, unsigned n)
{
    uint8_t reply[131];
    size_t length = rpc(s, n, LOCAL_GENERAL, 3, NULL, 0, 0, reply);

    expect_router_error(reply, length);
}

/* Runs one operator command line; returns its lines joined by newlines. */
static char *command(QTestState *s, unsigned n, const char *text,
                     int32_t *code)
{
    uint8_t args[130];
    uint8_t reply[131];
    GString *out = g_string_new(NULL);
    size_t length, offset;

    g_assert_cmpuint(strlen(text), <, 120);
    args[0] = 1;
    memcpy(args + 1, text, strlen(text) + 1);
    length = rpc(s, n, LOCAL_COMMAND, 0, args, 2 + strlen(text), 1, reply);
    g_assert_cmpuint(length, >=, 6);
    if (code) {
        *code = (int32_t)ldl_be_p(reply + 1);
    }
    offset = 6;
    for (unsigned i = 0; i < reply[5]; i++) {
        g_assert_cmpuint(reply[offset], ==, 1);
        offset++;
        g_string_append(out, (const char *)reply + offset);
        g_string_append_c(out, '\n');
        offset += strlen((const char *)reply + offset) + 1;
    }
    g_assert_cmpuint(offset, ==, length);
    return g_string_free(out, FALSE);
}

static void expect_command(QTestState *s, unsigned n, const char *text,
                           const char *expected)
{
    g_autofree char *output = command(s, n, text, NULL);

    g_assert_cmpstr(output, ==, expected);
}

#define UNSUPPORTED "INFO: command not supported on this brick type\n"

static void expect_refused(QTestState *s, unsigned n, const char *text)
{
    int32_t code = 0;
    g_autofree char *output = command(s, n, text, &code);

    g_assert_cmpstr(output, ==, UNSUPPORTED);
    g_assert_cmpint(code, ==, -1);
}

static size_t eeprom(QTestState *s, unsigned n, uint32_t destination,
                     unsigned component, unsigned area, unsigned offset,
                     unsigned length, uint8_t *reply)
{
    uint8_t args[20];

    integer(args, component);
    integer(args + 5, area);
    integer(args + 10, offset);
    integer(args + 15, length);
    return rpc(s, n, destination, 6, args, sizeof(args), 4, reply);
}

/* The data bytes of a successful record read: after the remaining count. */
static const uint8_t *eeprom_data(const uint8_t *reply, size_t reply_length,
                                  size_t *data_length)
{
    g_assert_cmpuint(reply_length, >=, 12);
    expect_code(reply, reply_length, 0);
    g_assert_cmpuint(reply[5], ==, 2);
    g_assert_cmphex(reply[11] & 0x80, ==, 0x80);
    *data_length = reply[11] & 0x7f;
    g_assert_cmpuint(reply_length, ==, 12 + *data_length);
    return reply + 12;
}

static bool contains_mac(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i + 13 <= length; i++) {
        if (data[i] == 0xcc && !memcmp(data + i + 1, MAC_TEXT, 12)) {
            return true;
        }
    }
    return false;
}

static bool board_has_address(QTestState *s, unsigned n, uint32_t destination,
                              unsigned component)
{
    uint8_t reply[131];
    size_t length, data_length;
    const uint8_t *data;

    length = eeprom(s, n, destination, component, 2, 0, 116, reply);
    data = eeprom_data(reply, length, &data_length);
    return contains_mac(data, data_length);
}

static void test_identity(void)
{
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=3,bay=1");

    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 1), 'C');
    qtest_quit(s);

    s = start_one("brick-type=ibrick,named=on,rack=4,bay=2,mac=" MAC_PROP);
    expect_identity(s, 0, LOCAL_GENERAL, packed(4, 2), 'I');
    qtest_quit(s);

    /*
     * A V-brick answers X: the L1 firmware takes the letter from the logic
     * board's name, and the V-brick is an X-brick chassis.
     */
    s = start_one("brick-type=vbrick,named=on,rack=1,bay=10");
    expect_identity(s, 0, LOCAL_GENERAL, packed(1, 10), 'X');
    expect_command(s, 0, "brick",
                   "rack: 001, slot: 10, partition: none, type: X [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    expect_command(s, 0, "config", "0: 001x10\n");
    qtest_quit(s);
}

static void test_unnamed(void)
{
    QTestState *s = start_one("brick-type=cbrick");

    /* No rack and slot are supplied, so the brick has none and no type. */
    expect_unnamed_identity(s, 0);
    expect_command(s, 0, "config", "0: ---c--\n");
    qtest_quit(s);

    s = start_one("brick-type=ibrick,mac=" MAC_PROP);
    expect_unnamed_identity(s, 0);
    expect_command(s, 0, "config", "0: ---i--\n");
    qtest_quit(s);

    /* A named brick refuses a destination that names another brick. */
    s = start_one("brick-type=cbrick,named=on,rack=3,bay=1");
    {
        uint8_t reply[131];
        size_t length = rpc(s, 0, UINT32_C(0x000c2006), 3, NULL, 0, 0,
                            reply);

        expect_router_error(reply, length);
    }
    qtest_quit(s);
}

static void test_naming_takes_effect_on_restart(void)
{
    QTestState *s = start_one("brick-type=cbrick,partition=2");
    int32_t code;
    g_autofree char *output = NULL;

    expect_command(s, 0, "brick",
                   "rack: ---, slot: --, partition: 2, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    output = command(s, 0, "brick rack 3", &code);
    g_assert_cmpint(code, ==, 0);
    g_free(output);
    output = command(s, 0, "brick slot 1", &code);
    g_assert_cmpint(code, ==, 0);
    g_free(output);
    output = command(s, 0, "brick partition 9", &code);
    g_assert_cmpint(code, ==, 0);
    /* Nothing changes until the controller restarts. */
    expect_unnamed_identity(s, 0);
    expect_command(s, 0, "config", "0: ---c--\n");
    expect_command(s, 0, "brick",
                   "rack: ---, slot: --, partition: 2, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    g_free(output);
    output = command(s, 0, "reboot_l1", &code);
    g_assert_cmpint(code, ==, 0);
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 1), 'C');
    expect_command(s, 0, "config", "0: 003c01\n");
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 01, partition: 9, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    {
        /* The partition request returns the stored value. */
        uint8_t reply[131];
        size_t length = rpc(s, 0, LOCAL_GENERAL, 9, NULL, 0, 0, reply);

        g_assert_cmpuint(length, ==, 11);
        expect_code(reply, length, 0);
        g_assert_cmpuint(reply[5], ==, 1);
        g_assert_cmphex(ldl_be_p(reply + 7), ==, 9);
    }
    qtest_quit(s);
}

static void test_half_named(void)
{
    QTestState *s = start_one("brick-type=ibrick,mac=" MAC_PROP);
    g_autofree char *output = command(s, 0, "brick slot 5", NULL);

    g_free(output);
    output = command(s, 0, "reboot_l1", NULL);
    /* A rack still unset leaves the brick without an identity to report. */
    expect_unnamed_identity(s, 0);
    expect_command(s, 0, "config", "0: ---i05\n");
    qtest_quit(s);
}

static void test_retention(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=3,bay=1,partition=5",
    };
    QTestState *s = start_with(1, props, "-no-shutdown");
    uint8_t before[131], after[131];
    size_t before_length, after_length;
    g_autofree char *output = NULL;

    before_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, before);
    output = command(s, 0, "brick slot 7", NULL);
    g_free(output);
    output = command(s, 0, "reboot_l1", NULL);
    g_free(output);
    /* Rack, partition, type and records survive; the slot took effect. */
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 7), 'C');
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 07, partition: 5, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    after_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, after);
    g_assert_cmpuint(after_length, ==, before_length);
    g_assert_cmpmem(after, after_length, before, before_length);
    /* So does a node reset. */
    qtest_system_reset(s);
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 7), 'C');
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 07, partition: 5, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    after_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, after);
    g_assert_cmpmem(after, after_length, before, before_length);
    /* So does a node power-off. */
    output = command(s, 0, "power down", NULL);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    expect_command(s, 0, "power check", "power appears off\n");
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 7), 'C');
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 07, partition: 5, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    after_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, after);
    g_assert_cmpmem(after, after_length, before, before_length);
    qtest_quit(s);
}

static void test_ibrick_records(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length;

    link_io(s, 0, 1);
    /* Component 1 is the logic board and carries the address. */
    g_assert_true(board_has_address(s, 1, LOCAL_GENERAL, 1));
    /* Component 0 is the power board and does not. */
    g_assert_false(board_has_address(s, 1, LOCAL_GENERAL, 0));
    /* The same record through the C-brick's attached I/O destination. */
    g_assert_true(board_has_address(s, 0, ATTACHED_IO, 1));
    /* Components past the table are absent. */
    length = eeprom(s, 1, LOCAL_GENERAL, 2, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);
}

/* The record's data holds @text as one of its fields. */
static bool contains_text(const uint8_t *data, size_t length,
                          const char *text)
{
    size_t n = strlen(text);

    for (size_t i = 0; i + n + 1 <= length; i++) {
        if (data[i] == (0xc0 | n) && !memcmp(data + i + 1, text, n)) {
            return true;
        }
    }
    return false;
}

/*
 * The V-brick holds the X-brick personality's two records, the power board
 * at component 0 and the logic board at 1, the component the stock PROM and
 * IRIX read for a brick that answers X. The logic board is named XBRICK,
 * the name that gives X; the rest is synthetic and carries no address. A
 * C-brick reaches them through the attached I/O destination as it reaches
 * an I-brick's, and nothing else is fitted.
 */
static void test_vbrick_records(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=vbrick,named=on,rack=1,bay=10",
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length, data_length;
    const uint8_t *data;

    link_io(s, 0, 1);
    for (unsigned component = 0; component <= 1; component++) {
        for (unsigned area = 1; area <= 2; area++) {
            length = eeprom(s, 1, LOCAL_GENERAL, component, area, 0, 64,
                            reply);
            eeprom_data(reply, length, &data_length);
            g_assert_cmpuint(data_length, >, 0);
        }
        g_assert_false(board_has_address(s, 1, LOCAL_GENERAL, component));
    }
    length = eeprom(s, 1, LOCAL_GENERAL, 0, 2, 0, 64, reply);
    data = eeprom_data(reply, length, &data_length);
    g_assert_true(contains_text(data, data_length, "POWER"));
    /* The same logic board through the C-brick's attached I/O address. */
    length = eeprom(s, 0, ATTACHED_IO, 1, 2, 0, 64, reply);
    data = eeprom_data(reply, length, &data_length);
    g_assert_true(contains_text(data, data_length, "XBRICK"));
    /* Nothing else is fitted, the earlier component 4 among it. */
    for (unsigned component = 2; component <= 4; component++) {
        length = eeprom(s, 1, LOCAL_GENERAL, component, 2, 0, 16, reply);
        expect_code(reply, length, -104);
    }
    expect_identity(s, 0, ATTACHED_IO, packed(1, 10), 'X');
    qtest_quit(s);
}

/*
 * Reads a record area the way the stock PROM does: 64-byte chunks until the
 * remaining count reaches 0. The area must be a whole number of eight-byte
 * units, byte 1 counts them, and every byte sums to 0 modulo 256.
 */
static GByteArray *read_area(QTestState *s, unsigned component, unsigned area)
{
    GByteArray *bytes = g_byte_array_new();
    uint8_t reply[131];
    size_t length, data_length;
    uint32_t remaining;

    do {
        const uint8_t *data;

        length = eeprom(s, 0, LOCAL_GENERAL, component, area, bytes->len, 64,
                        reply);
        data = eeprom_data(reply, length, &data_length);
        g_assert_cmpuint(data_length, >, 0);
        g_assert_cmpuint(data_length, <=, 64);
        g_byte_array_append(bytes, data, data_length);
        remaining = ldl_be_p(reply + 7);
        g_assert_cmpuint(bytes->len, <=, 512);
    } while (remaining);
    g_assert_cmpuint(bytes->len, >, 8);
    g_assert_cmpuint(bytes->data[1], >, 0);
    g_assert_cmpuint(bytes->len, ==, 8u * bytes->data[1]);
    return bytes;
}

static void expect_area_checksum(const GByteArray *bytes)
{
    uint8_t sum = 0;

    for (guint i = 0; i < bytes->len; i++) {
        sum += bytes->data[i];
    }
    g_assert_cmphex(sum, ==, 0);
}

/*
 * The board area's fields in the order the stock PROM reads them: six header
 * bytes, then manufacturer, product name, serial and part number, one FRU
 * file byte and the revision. Each must be typed 8-bit ASCII (type/length
 * bits 7:6 = 11), the type the PROM requires of name, serial, part and
 * revision, and carry exactly the expected text.
 */
static const uint8_t *expect_typed_field(const uint8_t *p, const uint8_t *end,
                                         const char *text)
{
    size_t n = strlen(text);

    g_assert_true(p < end);
    g_assert_cmphex(*p & 0xc0, ==, 0xc0);
    g_assert_cmpuint(*p & 0x3f, ==, n);
    g_assert_true(p + 1 + n <= end);
    g_assert_cmpmem(p + 1, n, text, n);
    return p + 1 + n;
}

static void expect_board_fields(const GByteArray *area, const char *part,
                                const char *revision)
{
    const uint8_t *p = area->data + 6, *end = area->data + area->len;

    p = expect_typed_field(p, end, "SYNTHETIC");
    p = expect_typed_field(p, end, "ROUTER");
    p = expect_typed_field(p, end, "EMUR00");
    p = expect_typed_field(p, end, part);
    p++;
    p = expect_typed_field(p, end, revision);
}

/*
 * A router L1 holds its board record at component 1, where the stock PROM
 * reads the chassis and board areas at router initialisation. The record is
 * named ROUTER and carries the part and revision the product's owner
 * inventories report: 030-1634-002 revision B for an Origin 3400 R-brick and
 * 030-1634-003 revision A for the Origin 300 and 350 NUMAlink module. The
 * Cx-brick router has no inventory: its record carries the model's
 * not-observed placeholders, which must not be either product's part. The
 * serial is synthetic. A router with no board named keeps no record.
 */
static void expect_router_board(const char *board, const char *part,
                                const char *revision)
{
    g_autofree char *props = g_strdup_printf("brick-type=router,"
                                             "router-board=%s", board);
    QTestState *s = start_one(props);
    uint8_t reply[131];
    size_t length, board_length = 0;

    for (unsigned area = 1; area <= 2; area++) {
        g_autoptr(GByteArray) bytes = read_area(s, 1, area);

        expect_area_checksum(bytes);
        if (area == 2) {
            board_length = bytes->len;
            expect_board_fields(bytes, part, revision);
        }
    }
    g_assert_false(board_has_address(s, 0, LOCAL_GENERAL, 1));
    /* An offset at the area's length is past its end. */
    length = eeprom(s, 0, LOCAL_GENERAL, 1, 2, board_length, 16, reply);
    expect_code(reply, length, -105);
    length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    length = eeprom(s, 0, LOCAL_GENERAL, 2, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);
}

static void test_router_records(void)
{
    uint8_t reply[131];
    QTestState *s;
    size_t length;

    expect_router_board("rbrick", "030-1634-002", "B");
    expect_router_board("numalink-module", "030-1634-003", "A");
    expect_router_board("cxbrick", "NOTOBSERVED0", "00");

    s = start_one("brick-type=router");
    length = eeprom(s, 0, LOCAL_GENERAL, 1, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);
}

static void test_cbrick_records(void)
{
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=1,bay=4");
    uint8_t reply[131];
    size_t length;

    /* The node board and the two PIMMs carry no address. */
    for (unsigned component = 0; component < 3; component++) {
        g_assert_false(board_has_address(s, 0, LOCAL_GENERAL, component));
    }
    /* A DIMM position is not a board record. */
    length = eeprom(s, 0, LOCAL_GENERAL, 3, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);
}

/* The synthetic DIMM record names its position at bytes 95 to 98. */
static int dimm_at(QTestState *s, unsigned component)
{
    uint8_t reply[131];
    size_t length = eeprom(s, 0, LOCAL_GENERAL, component, 4, 95, 4, reply);
    size_t data_length;
    const uint8_t *data;

    if ((int32_t)ldl_be_p(reply + 1) == -104) {
        return -1;
    }
    data = eeprom_data(reply, length, &data_length);
    g_assert_cmpuint(data_length, ==, 4);
    g_assert_cmpmem(data, 3, "EMU", 3);
    return data[3];
}

static void test_dimm_numbering(void)
{
    /* DIMMs 0, 2, 4, 6 at components 3 to 6; 1, 3, 5, 7 at 7 to 10. */
    static const int expected[] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=1,bay=4");
    uint8_t reply[131];
    size_t length;

    for (unsigned i = 0; i < G_N_ELEMENTS(expected); i++) {
        g_assert_cmpint(dimm_at(s, 3 + i), ==, expected[i]);
    }
    /* A linear numbering would put DIMM 1 at component 4. */
    g_assert_cmpint(dimm_at(s, 4), !=, 1);
    /* The board marker the Cx-brick's SPDs carry is 0 on a C-brick. */
    length = eeprom(s, 0, LOCAL_GENERAL, 3, 4, 99, 1, reply);
    g_assert_cmpuint(length, ==, 13);
    g_assert_cmpuint(reply[12], ==, 0);
    /* The rank density byte the Cx-brick fills in is 0 here too. */
    length = eeprom(s, 0, LOCAL_GENERAL, 3, 4, 31, 1, reply);
    g_assert_cmpuint(length, ==, 13);
    g_assert_cmpuint(reply[12], ==, 0);
    /* Past the table, and in the unfitted region, nothing answers. */
    length = eeprom(s, 0, LOCAL_GENERAL, 11, 4, 0, 16, reply);
    expect_code(reply, length, -104);
    length = eeprom(s, 0, LOCAL_GENERAL, 15, 4, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);

    /* An empty position answers the existing no-record code. */
    s = start_one("brick-type=cbrick,named=on,rack=1,bay=4,dimm-mask=0xfd");
    g_assert_cmpint(dimm_at(s, 7), ==, -1);
    g_assert_cmpint(dimm_at(s, 4), ==, 2);
    qtest_quit(s);

    s = start_one("brick-type=cbrick,named=on,rack=1,bay=4,pimm-mask=1");
    length = eeprom(s, 0, LOCAL_GENERAL, 1, 2, 0, 16, reply);
    expect_code(reply, length, 0);
    length = eeprom(s, 0, LOCAL_GENERAL, 2, 2, 0, 16, reply);
    expect_code(reply, length, -104);
    qtest_quit(s);
}

static void expect_pairs(const uint8_t *reply, size_t length, unsigned fields,
                         const uint32_t *expected)
{
    g_assert_cmpuint(length, ==, 6 + 5 * fields);
    expect_code(reply, length, 0);
    g_assert_cmpuint(reply[5], ==, fields);
    for (unsigned i = 0; i < fields; i++) {
        g_assert_cmpuint(reply[6 + 5 * i], ==, 0);
        g_assert_cmphex(ldl_be_p(reply + 7 + 5 * i), ==, expected[i]);
    }
}

static void test_attached_bricks(void)
{
    /* a: C-brick with an I-brick (b) and the other C-brick (c). */
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t none[8] = { 0 };
    uint32_t a_ports[8] = { packed(1, 31), 'I', 0, 0, 0, 0, 0, 0 };
    uint32_t a_ni[4] = { packed(1, 7), 'C', 0, 0 };

    /* Before any cable: nothing attached. */
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, none);
    length = rpc(s, 0, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_pairs(reply, length, 4, none);

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, a_ports);
    length = rpc(s, 0, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_pairs(reply, length, 4, a_ni);
    expect_command(s, 0, "config", "0: 001c04\n1: 001i31\n5: 001c07\n");

    /* The other C-brick sees only itself and the cable to a. */
    {
        uint32_t c_ni[4] = { packed(1, 4), 'C', 0, 0 };

        length = rpc(s, 2, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
        expect_pairs(reply, length, 8, none);
        length = rpc(s, 2, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
        expect_pairs(reply, length, 4, c_ni);
        expect_command(s, 2, "config", "0: 001c07\n5: 001c04\n");
    }

    /* The I-brick knows its C-brick and nothing else. */
    expect_command(s, 1, "config", "0: 001i31\n1: 001c04\n");
    {
        uint32_t b_ports[8] = { packed(1, 4), 'C', 0, 0, 0, 0, 0, 0 };

        length = rpc(s, 1, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
        expect_pairs(reply, length, 8, b_ports);
    }

    /* Removing the cable returns the entry to absent. */
    qom_link(s, 0, "io-peer", "");
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, none);
    expect_command(s, 0, "config", "0: 001c04\n5: 001c07\n");
    qtest_quit(s);
}

static void set_gfx_bay(QTestState *s, unsigned owner, unsigned bay)
{
    g_autofree char *path = g_strdup_printf(DEVICES "l1c%u", owner);

    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,'property':"
        "'io-gfx-bay','value':%u}}", path, bay);
}

/*
 * An I-brick lists the C-brick that asks and, when that C-brick's chip has a
 * graphics brick on its other port, the G-brick, as the C-brick's own reply
 * lists a brick. The same request through the C-brick's attached I/O address
 * reaches it. Without a graphics brick the C-brick alone is listed; 0x000d
 * keeps the unknown-request code. The stock PROM never sends 0x000c to an
 * I-brick, whose widget 0 is an XBridge.
 */
static void test_ibrick_attached_graphics(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=16,mac=" MAC_PROP,
        "brick-type=ibrick,named=on,rack=1,bay=17,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(4, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t listed[8] = { packed(1, 4), 'C', packed(1, 25), 'G', 0, 0, 0, 0 };
    uint32_t alone[8] = { packed(1, 4), 'C', 0, 0, 0, 0, 0, 0 };
    uint32_t other[8] = { packed(1, 7), 'C', 0, 0, 0, 0, 0, 0 };
    uint32_t high[4] = { 0 };

    link_io(s, 0, 1);
    link_io(s, 3, 2);
    /* No graphics brick yet: the I-brick lists its C-brick alone. */
    length = rpc(s, 1, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, alone);
    length = rpc(s, 0, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, alone);
    length = rpc(s, 1, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    set_gfx_bay(s, 0, 25);
    length = rpc(s, 1, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, listed);
    length = rpc(s, 1, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_pairs(reply, length, 4, high);
    length = rpc(s, 0, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, listed);
    /* Another I-brick whose C-brick has none lists that C-brick alone. */
    length = rpc(s, 2, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, other);
    qtest_quit(s);

    /* An I-brick with no named C-brick has nothing to list. */
    s = start_with(2, (const char *const[]) {
        "brick-type=ibrick,named=on,rack=1,bay=16,mac=" MAC_PROP,
        "brick-type=cbrick",
    }, "");
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    link_io(s, 1, 0);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    qtest_quit(s);
}

/*
 * A V-brick lists the C-brick that asks at port 1, as an I-brick does: the
 * L1 firmware answers 0x000c from its link table whatever the brick's
 * letter. The stock PROM never asks: an XBridge at the V-brick's widget 0
 * keeps it out of the crossbar arbitration that sends the request. The
 * port is the model's I-brick numbering; 0x000d keeps the unknown-request
 * code.
 */
static void test_vbrick_attached(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=vbrick,named=on,rack=1,bay=10",
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t alone[8] = { packed(1, 7), 'C', 0, 0, 0, 0, 0, 0 };

    link_io(s, 0, 1);
    length = rpc(s, 1, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, alone);
    length = rpc(s, 0, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, alone);
    length = rpc(s, 1, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    qtest_quit(s);
}

/*
 * The N-brick's one controller serves every C-brick cabled to it, reports
 * its own type letter, and answers each C-brick about that C-brick's own
 * chip.
 */
static void test_nbrick_attached_graphics(void)
{
    const char *const props[] = {
        "brick-type=nbrick,named=on,rack=1,bay=22",
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=cbrick,named=on,rack=1,bay=10",
        "brick-type=cbrick,named=on,rack=1,bay=13",
    };
    QTestState *s = start_with(4, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t first[8] = { packed(1, 7), 'C', packed(1, 25), 'G', 0, 0, 0, 0 };
    uint32_t second[8] = { packed(1, 10), 'C', packed(1, 28), 'G',
                           0, 0, 0, 0 };

    link_io(s, 1, 0);
    link_io(s, 2, 0);
    link_io(s, 3, 0);
    set_gfx_bay(s, 1, 25);
    set_gfx_bay(s, 2, 28);
    expect_identity(s, 1, ATTACHED_IO, packed(1, 22), 'N');
    expect_identity(s, 3, ATTACHED_IO, packed(1, 22), 'N');
    length = rpc(s, 1, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, first);
    length = rpc(s, 2, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, second);
    /* A C-brick whose chip has no pipe gets the unknown-request code. */
    length = rpc(s, 3, ATTACHED_IO, 0xc, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    expect_command(s, 0, "config", "0: 001n22\n");
    qtest_quit(s);

    /* An I-brick takes one C-brick; an N-brick takes no mac. */
    s = start_with(3, (const char *const[]) {
        "brick-type=ibrick,named=on,rack=1,bay=16,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=cbrick,named=on,rack=1,bay=10",
    }, "");
    link_io(s, 1, 0);
    g_assert_true(qom_link_refused(s, 2, "io-peer", DEVICES "l1c0"));
    qtest_quit(s);
}

static void test_attached_port_numbering(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4,io-port=3,c-port=10",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t ports[8] = { 0, 0, 0, 0, packed(1, 31), 'I', 0, 0 };
    uint32_t ni[4] = { 0, 0, packed(1, 7), 'C' };

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, ports);
    length = rpc(s, 0, LOCAL_GENERAL, 0xd, NULL, 0, 0, reply);
    expect_pairs(reply, length, 4, ni);
    qtest_quit(s);
}

static void test_unnamed_attachment(void)
{
    /* An attached brick without a name shows as absent and is logged. */
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t none[8] = { 0 };

    link_io(s, 0, 1);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, none);
    expect_command(s, 0, "config", "0: 001c04\n1: ---i--\n");
    qtest_quit(s);
}

static void test_type_four_requests(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length;

    /* No I-brick is cabled: the existing router error. */
    length = rpc(s, 0, ATTACHED_IO, 3, NULL, 0, 0, reply);
    expect_router_error(reply, length);
    link_io(s, 0, 1);
    expect_identity(s, 0, ATTACHED_IO, packed(1, 31), 'I');
    /* The local reply is the C-brick's own. */
    expect_identity(s, 0, LOCAL_GENERAL, packed(1, 4), 'C');
    /* No other C-brick is cabled. */
    length = rpc(s, 0, ATTACHED_C, 3, NULL, 0, 0, reply);
    expect_router_error(reply, length);
    qtest_quit(s);
}

static void test_refusals(void)
{
    static const char *const ibrick_refused[] = {
        "select", "nmi", "softreset", "cpu", "pimm", "autopower", "ctc brick",
        "cti brick", "debug", "leds", "junkbus", "* brick",
    };
    static const char *const both_refused[] = { "router" };
    QTestState *s = start_one("brick-type=ibrick,named=on,rack=1,bay=31,"
                              "mac=" MAC_PROP);
    g_autofree char *before = NULL;

    before = command(s, 0, "brick", NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(ibrick_refused); i++) {
        expect_refused(s, 0, ibrick_refused[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(both_refused); i++) {
        expect_refused(s, 0, both_refused[i]);
    }
    /* Nothing changed. */
    expect_command(s, 0, "brick", before);
    qtest_quit(s);

    s = start_one("brick-type=cbrick,named=on,rack=1,bay=4");
    expect_refused(s, 0, "pci");
    expect_refused(s, 0, "router");
    /* The C-brick keeps the commands the I-brick lacks. */
    {
        int32_t code = 0;
        g_autofree char *output = command(s, 0, "debug", &code);

        g_assert_cmpint(code, ==, 0);
        /* The command's own text ends in a bare newline. */
        g_assert_cmpstr(output, ==, "debug switches set to 0x0000\n\n");
    }
    qtest_quit(s);
}

static void expect_revision(QTestState *s, unsigned n, uint32_t destination,
                            uint32_t major, uint32_t minor, uint32_t patch)
{
    uint8_t reply[131];
    size_t length = rpc(s, n, destination, 5, NULL, 0, 0, reply);

    g_assert_cmpuint(length, ==, 21);
    expect_code(reply, length, 0);
    g_assert_cmpuint(reply[5], ==, 3);
    g_assert_cmpuint(ldl_be_p(reply + 7), ==, major);
    g_assert_cmpuint(ldl_be_p(reply + 12), ==, minor);
    g_assert_cmpuint(ldl_be_p(reply + 17), ==, patch);
}

/*
 * The C-brick, I-brick, N-brick and V-brick profiles answer request 0x0005
 * with the revision their machine configures, each its own; the attached
 * I/O address reaches the I-brick's and the V-brick's. Without one the
 * request is an error, as the Cx-brick's always is.
 */
static void test_firmware_revision(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4,revision-major=1,"
        "revision-minor=44,revision-patch=0",
        "brick-type=ibrick,named=on,rack=1,bay=16,mac=" MAC_PROP
        ",revision-major=1,revision-minor=7,revision-patch=2",
        "brick-type=nbrick,named=on,rack=1,bay=22,revision-major=1,"
        "revision-minor=9,revision-patch=0",
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=vbrick,named=on,rack=1,bay=13",
        "brick-type=cbrick,named=on,rack=1,bay=10",
        "brick-type=vbrick,named=on,rack=1,bay=14,revision-major=1,"
        "revision-minor=44,revision-patch=0",
    };
    QTestState *s = start_with(7, props, "");
    uint8_t reply[131];
    size_t length;

    link_io(s, 0, 1);
    link_io(s, 5, 6);
    expect_revision(s, 6, LOCAL_GENERAL, 1, 44, 0);
    expect_revision(s, 5, ATTACHED_IO, 1, 44, 0);
    expect_revision(s, 0, LOCAL_GENERAL, 1, 44, 0);
    expect_revision(s, 1, LOCAL_GENERAL, 1, 7, 2);
    expect_revision(s, 0, ATTACHED_IO, 1, 7, 2);
    expect_revision(s, 2, LOCAL_GENERAL, 1, 9, 0);
    length = rpc(s, 3, LOCAL_GENERAL, 5, NULL, 0, 0, reply);
    expect_code(reply, length, -104);
    length = rpc(s, 4, LOCAL_GENERAL, 5, NULL, 0, 0, reply);
    expect_code(reply, length, -104);
    /*
     * The revision is for 0x0005 only: version names no image bank and no
     * Origin 300 image label.
     */
    expect_command(s, 0, "version", "L1 1.44.0 (Image EMU-UNKNOWN)\n");
    expect_command(s, 1, "version", "L1 1.7.2 (Image EMU-UNKNOWN)\n");
    expect_command(s, 2, "version", "L1 1.9.0 (Image EMU-UNKNOWN)\n");
    qtest_quit(s);
}

/*
 * A C-brick answers the I/O link speed request 0x000a with a first integer
 * of 0, a 400 MHz link, the speed the stock PROM expects for an I-brick,
 * N-brick or X-brick, whichever I/O brick is cabled, and so for a V-brick,
 * which answers X. The V-brick answers its own ports A and B at 400 MHz,
 * an approximation the L1 firmware's default with no C-brick supports.
 */
static void test_port_speed(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=16,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=vbrick,named=on,rack=1,bay=13",
    };
    QTestState *s = start_with(4, props, "");
    uint8_t reply[131];
    size_t length;
    uint32_t speed[2] = { 0, 0 };

    link_io(s, 0, 1);
    link_io(s, 2, 3);
    for (unsigned n = 0; n < 4; n++) {
        if (n == 1) {
            continue;
        }
        length = rpc(s, n, LOCAL_GENERAL, 0xa, NULL, 0, 0, reply);
        expect_pairs(reply, length, 2, speed);
    }
    length = rpc(s, 2, ATTACHED_IO, 0xa, NULL, 0, 0, reply);
    expect_pairs(reply, length, 2, speed);
    qtest_quit(s);
}

static void test_ibrick_power_is_its_own(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    g_autofree char *output = NULL;

    link_io(s, 0, 1);
    /*
     * The system's start-up power-up reaches the I-brick, so it starts
     * powered and logs the power-up that reached it.
     */
    expect_command(s, 1, "power check", "power appears on\n");
    output = command(s, 1, "log", NULL);
    g_assert_nonnull(strstr(output, "power up (COMMAND)"));
    g_free(output);
    expect_command(s, 1, "power up", "Power is already on\n");
    expect_command(s, 0, "power check", "power appears on\n");
    output = command(s, 1, "power down", NULL);
    expect_command(s, 1, "power check", "power appears off\n");
    g_free(output);
    output = command(s, 1, "power up", NULL);
    expect_command(s, 1, "power check", "power appears on\n");
    g_free(output);
    output = command(s, 1, "power down", NULL);
    expect_command(s, 1, "power check", "power appears off\n");
    /* The C-brick is still up and still answers. */
    expect_command(s, 0, "power check", "power appears on\n");
    expect_identity(s, 0, LOCAL_GENERAL, packed(1, 4), 'C');
    expect_identity(s, 0, ATTACHED_IO, packed(1, 31), 'I');
    qtest_quit(s);
}

static void test_broadcast_and_relay(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    expect_command(s, 0, "cti power check",
                   "001i31:\npower appears on\n");
    expect_command(s, 0, "ctc power check",
                   "001c07:\npower appears on\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n"
                   "001i31:\npower appears on\n");
    /* A write reaches every controller. */
    {
        g_autofree char *output = command(s, 0, "* brick partition 4", NULL);

        g_free(output);
        output = command(s, 1, "reboot_l1", NULL);
        g_free(output);
        output = command(s, 2, "reboot_l1", NULL);
        g_free(output);
        output = command(s, 0, "reboot_l1", NULL);
    }
    expect_command(s, 1, "brick",
                   "rack: 001, slot: 31, partition: 4, type: I [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    expect_command(s, 2, "brick",
                   "rack: 001, slot: 07, partition: 4, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    qtest_quit(s);
}

static void test_restart_window(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    uint8_t reply[131];
    size_t length;
    g_autofree char *output = NULL;

    link_io(s, 0, 1);
    output = command(s, 1, "brick slot 30", NULL);
    g_free(output);
    expect_identity(s, 0, ATTACHED_IO, packed(1, 31), 'I');
    output = command(s, 1, "reboot_l1", NULL);
    /* The sender hears nothing from the restarting brick for a while. */
    length = rpc(s, 0, ATTACHED_IO, 3, NULL, 0, 0, reply);
    expect_router_error(reply, length);
    qtest_clock_step(s, RESTART_INSIDE_NS);
    length = rpc(s, 0, ATTACHED_IO, 3, NULL, 0, 0, reply);
    expect_router_error(reply, length);
    qtest_clock_step(s, RESTART_PAST_NS);
    expect_identity(s, 0, ATTACHED_IO, packed(1, 30), 'I');
    qtest_quit(s);
}

/*
 * A reply to a command interpreter request carries each line as its own
 * field, with no line ending: the model splits its CR LF-ended text there.
 */
static void test_task3_line_endings(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    static const char refusal[] = "INFO: no I/O brick is attached.";
    static const char name[] = "001c07:";
    static const char silent[] = "no response";
    QTestState *s = start_with(2, props, "");
    uint8_t args[130] = { 1 };
    uint8_t reply[131];
    size_t length, offset;
    g_autofree char *output = NULL;

    memcpy(args + 1, "cti power check", 16);
    length = rpc(s, 0, LOCAL_COMMAND, 0, args, 2 + 15, 1, reply);
    g_assert_cmpuint(reply[5], ==, 1);
    g_assert_cmpuint(length, ==, 6 + 1 + sizeof(refusal));
    g_assert_cmpuint(reply[6], ==, 1);
    g_assert_cmpmem(reply + 7, sizeof(refusal), refusal, sizeof(refusal));

    link_peer(s, 0, 1);
    output = command(s, 1, "reboot_l1", NULL);
    memcpy(args + 1, "ctc power check", 16);
    length = rpc(s, 0, LOCAL_COMMAND, 0, args, 2 + 15, 1, reply);
    g_assert_cmpuint(reply[5], ==, 2);
    g_assert_cmpuint(length, ==,
                     6 + 1 + sizeof(name) + 1 + sizeof(silent));
    offset = 6;
    g_assert_cmpuint(reply[offset++], ==, 1);
    g_assert_cmpmem(reply + offset, sizeof(name), name, sizeof(name));
    offset += sizeof(name);
    g_assert_cmpuint(reply[offset++], ==, 1);
    g_assert_cmpmem(reply + offset, sizeof(silent), silent, sizeof(silent));
    qtest_quit(s);
}

static void test_link_refusals(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=ip45",
    };
    QTestState *s = start_with(4, props, "");

    /* The I-brick takes one C-brick. */
    link_io(s, 0, 1);
    g_assert_true(qom_link_refused(s, 2, "io-peer", DEVICES "l1c1"));
    /* The same C-brick may name it again. */
    link_io(s, 0, 1);
    /* An I-brick link must name an I-brick. */
    g_assert_true(qom_link_refused(s, 0, "io-peer", DEVICES "l1c2"));
    /* An I-brick has no I/O brick to cable. */
    g_assert_true(qom_link_refused(s, 1, "io-peer", DEVICES "l1c1"));
    /* The C-brick pair link joins C-bricks only. */
    g_assert_true(qom_link_refused(s, 0, "peer", DEVICES "l1c3"));
    g_assert_true(qom_link_refused(s, 3, "peer", DEVICES "l1c0"));
    g_assert_true(qom_link_refused(s, 0, "peer", DEVICES "l1c1"));
    qtest_quit(s);
}

/* The same time budget for a line that was once exponential in its stars. */
#define BOUNDED_USEC (5 * G_USEC_PER_SEC)

#define NESTED_STAR "INFO: nested * is unsupported.\n"
#define NESTED_CTC "INFO: nested ctc is unsupported.\n"
#define NESTED_CTI "INFO: nested cti is unsupported.\n"
#define NO_IO_BRICK "INFO: no I/O brick is attached.\n"
#define NOT_FOUND "ERROR: command not found.\n"

static void expect_command_code(QTestState *s, unsigned n, const char *text,
                                const char *expected, int32_t want)
{
    int32_t code = 0;
    g_autofree char *output = command(s, n, text, &code);

    g_assert_cmpstr(output, ==, expected);
    g_assert_cmpint(code, ==, want);
}

static void expect_partition(QTestState *s, unsigned n, unsigned want)
{
    g_autofree char *output = command(s, n, "brick", NULL);
    /*
     * Zero prints none and 255 or more prints ?, as the L1 datasheet's brick
     * row has it (inferred for the Origin 3000 C-brick image).
     */
    g_autofree char *needle = want >= 255 ? g_strdup("partition: ?,") :
        want ? g_strdup_printf("partition: %u,", want) :
        g_strdup("partition: none,");

    g_assert_nonnull(strstr(output, needle));
}

/*
 * A "*" that names another "*", or that reaches a brick through a relay, is
 * refused: each extra star used to double the work, so a line of stars
 * could stall the controller. The refusal text is the model's.
 */
static void test_nested_broadcast(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");
    GString *deep = g_string_new(NULL);
    gint64 start;

    link_io(s, 0, 1);
    link_peer(s, 0, 2);

    expect_command_code(s, 0, "* * power check", NESTED_STAR, -1);

    /* Fifty separate stars, then a command that must not run. */
    for (unsigned i = 0; i < 50; i++) {
        g_string_append(deep, "* ");
    }
    g_string_append(deep, "brick partition 7");
    start = g_get_monotonic_time();
    expect_command_code(s, 0, deep->str, NESTED_STAR, -1);
    g_assert_cmpint(g_get_monotonic_time() - start, <, BOUNDED_USEC);
    g_string_free(deep, TRUE);
    expect_partition(s, 0, 0);
    expect_partition(s, 1, 0);
    expect_partition(s, 2, 0);

    /* Fifty stars with no spaces are one unknown word, not a broadcast. */
    deep = g_string_new(NULL);
    for (unsigned i = 0; i < 50; i++) {
        g_string_append_c(deep, '*');
    }
    start = g_get_monotonic_time();
    expect_command_code(s, 0, deep->str, NOT_FOUND, -1);
    g_assert_cmpint(g_get_monotonic_time() - start, <, BOUNDED_USEC);
    g_string_free(deep, TRUE);

    /* A "*" sent on to the other brick is refused there, and runs nothing. */
    expect_command_code(s, 0, "ctc * brick partition 7",
                        "001c07:\n" NESTED_STAR, -1);
    expect_command_code(s, 0, "cti * brick partition 7",
                        "001i31:\n" UNSUPPORTED, -1);
    expect_partition(s, 0, 0);
    expect_partition(s, 1, 0);
    expect_partition(s, 2, 0);
    qtest_quit(s);
}

/*
 * One level of relay only. The refusal for a nested cti is its own, whether
 * or not an I-brick is attached to the brick that refuses.
 */
static void test_relay_depth(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=ibrick,named=on,rack=1,bay=30,mac=" MAC_PROP,
    };
    QTestState *s = start_with(4, props, "");

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    link_io(s, 2, 3);
    expect_command_code(s, 0, "ctc ctc power check",
                        "001c07:\n" NESTED_CTC, -1);
    /* The peer has an I-brick of its own, and still refuses. */
    expect_command_code(s, 0, "ctc cti power check",
                        "001c07:\n" NESTED_CTI, -1);
    /* The peer's own I-brick answers when asked from that peer. */
    expect_command(s, 2, "cti power check", "001i30:\npower appears on\n");
    /* The I-brick has neither. */
    expect_command_code(s, 0, "cti ctc power check",
                        "001i31:\n" UNSUPPORTED, -1);
    expect_command_code(s, 0, "cti cti power check",
                        "001i31:\n" UNSUPPORTED, -1);
    qtest_quit(s);

    /*
     * With "*", the brick it is typed at runs its own part, the other C-brick
     * refuses as a relay, and the I-brick refuses by availability. The
     * other C-brick's I-brick is not reached.
     */
    {
        const char *const pair[] = {
            "brick-type=cbrick,named=on,rack=1,bay=4",
            "brick-type=cbrick,named=on,rack=1,bay=7",
        };

        s = start_with(2, pair, "");
        link_peer(s, 0, 1);
        expect_command_code(s, 0, "* cti power check",
                            "001c04:\n" NO_IO_BRICK
                            "001c07:\n" NESTED_CTI, -1);
        expect_command_code(s, 0, "* ctc power check",
                            "001c04:\n001c07:\npower appears on\n"
                            "001c07:\n" NESTED_CTC, -1);
        qtest_quit(s);
    }
    {
        const char *const cable[] = {
            "brick-type=cbrick,named=on,rack=1,bay=4",
            "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        };
        g_autofree char *output = NULL;

        s = start_with(2, cable, "");
        link_io(s, 0, 1);
        output = command(s, 0, "* cti power check", NULL);
        g_assert_true(g_str_has_prefix(output,
            "001c04:\n001i31:\npower appears on\n001i31:\n"));
        g_assert_true(g_str_has_suffix(output, UNSUPPORTED));
        qtest_quit(s);
    }
}

/*
 * A brick hears its own "*" part while it restarts; only another brick can
 * be silent.
 */
static void test_star_during_own_restart(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(2, props, "");
    g_autofree char *output = NULL;

    link_peer(s, 0, 1);
    output = command(s, 0, "reboot_l1", NULL);
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n");
    /* The other brick is the one that answers nothing. */
    qtest_clock_step(s, RESTART_STEP_NS);
    g_free(output);
    output = command(s, 1, "reboot_l1", NULL);
    expect_command_code(s, 0, "* power check",
                        "001c04:\npower appears on\n"
                        "001c07:\nno response\n", -1);
    qtest_clock_step(s, RESTART_INSIDE_NS);
    expect_command_code(s, 0, "* power check",
                        "001c04:\npower appears on\n"
                        "001c07:\nno response\n", -1);
    qtest_clock_step(s, RESTART_PAST_NS);
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n");
    qtest_quit(s);
}

/* "*" does not reach the other C-brick's I-brick; its own refuses ctc. */
static void test_star_reach(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
        "brick-type=ibrick,named=on,rack=1,bay=30,mac=" MAC_PROP,
    };
    QTestState *s = start_with(4, props, "");
    g_autofree char *output = NULL;

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    link_io(s, 2, 3);
    /* The other C-brick's I-brick is not reached. */
    output = command(s, 0, "* power check", NULL);
    g_assert_nonnull(strstr(output, "001i31:"));
    g_assert_null(strstr(output, "001i30:"));
    qtest_quit(s);

    /* The I-brick on the typing brick refuses ctc by availability. */
    s = start_with(2, props, "");
    link_io(s, 0, 1);
    g_free(output);
    output = command(s, 0, "* ctc power check", NULL);
    g_assert_true(g_str_has_suffix(output, "001i31:\n" UNSUPPORTED));
    qtest_quit(s);
}

/* A second C-brick is refused, and the first link keeps working. */
static void test_second_link_refused_keeps_first(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");

    link_io(s, 0, 1);
    g_assert_true(qom_link_refused(s, 2, "io-peer", DEVICES "l1c1"));
    expect_command(s, 0, "cti power check", "001i31:\npower appears on\n");
    expect_command(s, 0, "config", "0: 001c04\n1: 001i31\n");
    expect_command(s, 0, "cti config", "001i31:\n0: 001i31\n1: 001c04\n");
    expect_command(s, 2, "cti power check", NO_IO_BRICK);
    qtest_quit(s);
}

static void test_cti_without_ibrick(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");

    /* Nothing is attached: refused, runs nothing, changes nothing. */
    expect_command_code(s, 0, "cti brick partition 7", NO_IO_BRICK, -1);
    expect_partition(s, 1, 0);
    link_io(s, 0, 1);
    expect_command(s, 0, "cti power check", "001i31:\npower appears on\n");
    qtest_quit(s);
}

/*
 * A command for a restarting brick prints its name and "no response", then
 * answers again once the model's restart interval has passed.
 */
static void test_relay_during_restart(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");
    g_autofree char *output = NULL;

    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    output = command(s, 2, "reboot_l1", NULL);
    expect_command_code(s, 0, "ctc power check",
                        "001c07:\nno response\n", -1);
    qtest_clock_step(s, RESTART_INSIDE_NS);
    expect_command_code(s, 0, "ctc power check",
                        "001c07:\nno response\n", -1);
    qtest_clock_step(s, RESTART_PAST_NS);
    expect_command(s, 0, "ctc power check", "001c07:\npower appears on\n");
    g_free(output);
    output = command(s, 1, "reboot_l1", NULL);
    expect_command_code(s, 0, "cti power check",
                        "001i31:\nno response\n", -1);
    qtest_clock_step(s, RESTART_INSIDE_NS);
    expect_command_code(s, 0, "cti power check",
                        "001i31:\nno response\n", -1);
    qtest_clock_step(s, RESTART_PAST_NS);
    expect_command(s, 0, "cti power check", "001i31:\npower appears on\n");
    qtest_quit(s);
}

/*
 * config lists the local brick at 0 and each cabled brick at its port, in
 * ascending order of the port. A brick that is not cabled is not listed, and
 * "*" prints a block for each brick that is, and no placeholder for one that
 * is not.
 */
static void test_config_order_and_blocks(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4,io-port=10,c-port=5",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, "");

    expect_command(s, 0, "config", "0: 001c04\n");
    expect_command(s, 0, "* power check", "001c04:\npower appears on\n");
    link_io(s, 0, 1);
    expect_command(s, 0, "config", "0: 001c04\n10: 001i31\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001i31:\npower appears on\n");
    link_peer(s, 0, 2);
    /* The port numbers, not the order the cables were made, set the order. */
    expect_command(s, 0, "config", "0: 001c04\n5: 001c07\n10: 001i31\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n"
                   "001i31:\npower appears on\n");
    qom_link(s, 0, "peer", "");
    expect_command(s, 0, "config", "0: 001c04\n10: 001i31\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001i31:\npower appears on\n");
    qtest_quit(s);
}

/*
 * An attached brick with no name shows in config but is absent from
 * 0x000c; once it has a name, the same request returns it. The model
 * chooses to show the two views differently.
 */
static void test_unnamed_attachment_gets_named(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    g_autofree char *output = NULL;
    uint8_t reply[131];
    size_t length;
    uint32_t none[8] = { 0 };
    uint32_t named[8] = { packed(1, 31), 'I', 0, 0, 0, 0, 0, 0 };

    link_io(s, 0, 1);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, none);
    expect_command(s, 0, "config", "0: 001c04\n1: ---i--\n");
    output = command(s, 1, "brick rack 1", NULL);
    g_free(output);
    output = command(s, 1, "brick slot 31", NULL);
    g_free(output);
    output = command(s, 1, "reboot_l1", NULL);
    qtest_clock_step(s, RESTART_STEP_NS);
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, named);
    expect_command(s, 0, "config", "0: 001c04\n1: 001i31\n");
    qtest_quit(s);
}

/*
 * L1 datasheet, brick: "partition none" clears the stored partition and the
 * reply is "brick partition cleared."; like a number, it applies at the
 * next L1 boot.
 */
static void test_partition_clear(void)
{
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=1,bay=4,"
                              "partition=2");
    g_autofree char *output = NULL;
    uint8_t reply[131];
    size_t length;

    output = command(s, 0, "brick partition none", NULL);
    g_assert_nonnull(strstr(output, "brick partition cleared."));
    expect_partition(s, 0, 2);
    g_free(output);
    output = command(s, 0, "reboot_l1", NULL);
    /* Read back with the partition request: independent of the brick text. */
    length = rpc(s, 0, LOCAL_GENERAL, 0x9, NULL, 0, 0, reply);
    expect_code(reply, length, 0);
    g_assert_cmpuint(ldl_be_p(reply + 7), ==, 0);
    qtest_quit(s);
}

/*
 * L2 sheet, "A system with no L2": the model has no L2 or L3 controller, so a
 * request addressed to controller type 1 or 2 gets the router error reply
 * (-1, 8) and never a normal answer, from a named or unnamed C-brick and
 * I-brick alike.
 */
static void test_no_l2_route(void)
{
    static const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick",
        "brick-type=ibrick,mac=" MAC_PROP,
    };
    static const uint32_t destinations[] = {
        UINT32_C(0x1ffff006), UINT32_C(0x1ffff003), UINT32_C(0x2ffff006),
        UINT32_C(0x2ffff003),
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(props); i++) {
        QTestState *s = start_one(props[i]);

        for (unsigned j = 0; j < G_N_ELEMENTS(destinations); j++) {
            uint8_t reply[131];
            size_t length = rpc(s, 0, destinations[j], 3, NULL, 0, 0, reply);

            expect_router_error(reply, length);
        }
        qtest_quit(s);
    }
}

/*
 * The partition is limited to sixteen bits: the width of the stored value,
 * not a hardware figure.
 */
static void test_partition_limit(void)
{
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=1,bay=4,"
                              "partition=2");
    g_autofree char *output = NULL;
    uint8_t args[5] = { 0, 0, 1, 0, 0 };
    uint8_t reply[131];
    size_t length;

    expect_partition(s, 0, 2);
    output = command(s, 0, "brick partition 65536", NULL);
    g_assert_nonnull(strstr(output, "ERROR"));
    output = command(s, 0, "reboot_l1", NULL);
    expect_partition(s, 0, 2);
    g_free(output);
    output = command(s, 0, "brick partition 65535", NULL);
    g_assert_nonnull(strstr(output, "65535"));
    g_free(output);
    output = command(s, 0, "reboot_l1", NULL);
    expect_partition(s, 0, 65535);
    /* The request that sets it takes the same limit. */
    length = rpc(s, 0, LOCAL_GENERAL, 0x1006, args, sizeof(args), 1, reply);
    expect_code(reply, length, -105);
    expect_partition(s, 0, 65535);
    qtest_quit(s);
}

/*
 * Partition identity as the partitioning datasheet's L1 rows state it. A
 * C-brick stores its own ID, which 0x0009 returns and the brick line
 * prints. 0x000b answers 0 (an L2-less system:
 * the PROM never fetches a partition record). A router prints N/A and
 * refuses a set; an I/O brick has no ID of its own, reports its C-brick's
 * and refuses a set. The refusal texts are the datasheet's; their exit
 * code is the model's choice.
 */
static void expect_partition_request(QTestState *s, unsigned n,
                                     uint16_t request, uint32_t want)
{
    uint8_t reply[131];
    size_t length = rpc(s, n, LOCAL_GENERAL, request, NULL, 0, 0, reply);

    g_assert_cmpuint(length, ==, 11);
    expect_code(reply, length, 0);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, want);
}

/*
 * The brick line's partition field and tail follow the L1 datasheet's brick
 * row: zero prints none, the type is followed by a flash-size text and the
 * brick serial, with (null) for none. That the Origin 3000 C-brick image
 * does the same as the Chimera and Origin 300 images is inferred, and the
 * flash text is the unknown-size form.
 */
static void expect_brick_partition(QTestState *s, unsigned n,
                                   const char *want)
{
    g_autofree char *output = command(s, n, "brick", NULL);
    g_autofree char *needle = g_strdup_printf("partition: %s,", want);

    g_assert_nonnull(strstr(output, needle));
}

/*
 * PROVISIONAL for the I-brick: it follows its C-brick's partition and refuses
 * a set, one reading of two conflicting datasheets (the partitioning sheet
 * and the L1 sheet). Revisit when the conflict is settled.
 */
static void test_partition_identity(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4,partition=1",
        "brick-type=cbrick,named=on,rack=1,bay=7,partition=2",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=router,router-board=rbrick",
        "brick-type=cbrick,named=on,rack=1,bay=9",
    };
    QTestState *s = start_with(5, props, "");
    int32_t code = 0;
    g_autofree char *output = NULL;

    expect_brick_partition(s, 0, "1");
    expect_brick_partition(s, 1, "2");
    expect_partition_request(s, 0, 9, 1);
    expect_partition_request(s, 1, 9, 2);
    expect_partition_request(s, 0, 0xb, 0);
    expect_partition_request(s, 1, 0xb, 0);
    /* An unpartitioned C-brick answers 0 and prints none. */
    expect_brick_partition(s, 4, "none");
    expect_partition_request(s, 4, 9, 0);

    /* The I-brick reports its C-brick's ID, and cannot be given its own. */
    link_io(s, 1, 2);
    expect_brick_partition(s, 2, "2");
    expect_partition_request(s, 2, 9, 2);
    output = command(s, 2, "brick partition 5", &code);
    g_assert_cmpstr(output, ==, "INFO: can't set partition on I/O brick "
                    "(it is acquired from attached C-brick)\n");
    g_assert_cmpint(code, ==, -1);
    expect_brick_partition(s, 2, "2");
    g_free(output);

    /* The R-brick has none and refuses one. */
    expect_brick_partition(s, 3, "N/A");
    output = command(s, 3, "brick partition 5", &code);
    g_assert_cmpstr(output, ==, "INFO: can't set partition on routers.\n");
    g_assert_cmpint(code, ==, -1);
    expect_brick_partition(s, 3, "N/A");
    qtest_quit(s);
}

/*
 * L1 sheet, request 0x000b: the stored two-bit setting, 0 by default, which
 * the stock PROM reads to decide whether to fetch a partition record from an
 * L2. l2-partition-flag sets its initial value so a test can start that
 * fetch. The setting is not the partition ID that 0x0009 returns.
 */
static void test_l2_partition_flag(void)
{
    QTestState *s = start_one("brick-type=cbrick,named=on,rack=1,bay=4,"
                              "partition=2");

    expect_partition_request(s, 0, 0xb, 0);
    qtest_quit(s);
    for (unsigned flag = 1; flag <= 3; flag++) {
        g_autofree char *props = g_strdup_printf(
            "brick-type=cbrick,named=on,rack=1,bay=4,partition=2,"
            "l2-partition-flag=%u", flag);

        s = start_one(props);
        expect_partition_request(s, 0, 0xb, flag);
        expect_partition_request(s, 0, 9, 2);
        qtest_quit(s);
    }
}

static void test_l2_partition_flag_refusal(void)
{
    if (g_test_subprocess()) {
        QTestState *s = start_one("brick-type=cbrick,l2-partition-flag=4");

        qtest_quit(s);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*l2-partition-flag*");
}

/*
 * The link to an L2 (l1.rst, "L2 link"). The stub in sgi-l1-l2-stub.h
 * stands in for the peer; it is a fixture, not an L2 model. Router
 * segment offsets are those of the L1 sheet's management USB stream table.
 */
#define L2_FETCH_PROPS "brick-type=cbrick,named=on,rack=1,bay=4,partition=2"
/* Source word: key (rack 1, bay 4) in 31:12, tag in 11:5, return task 0x15. */
#define L2_SOURCE_CHANNEL_10 UINT32_C(0x00044215)

static QTestState *start_linked_traced(L2Stub *stub, const char *props,
                                       const char *trace)
{
    g_autofree char *extra = g_strdup_printf(
        "-chardev socket,id=l2c,path=%s %s", stub->path, trace);
    g_autofree char *all = g_strdup_printf("%s,l2=l2c", props);
    const char *const one[] = { all };
    QTestState *s = start_with(1, one, extra);

    l2_stub_accept(stub);
    /* The machine finishes connecting on its main loop, after accept. */
    for (unsigned i = 0; i < 10000 &&
         !qtest_qom_get_bool(s, DEVICES "l1c0", "l2-link-up"); i++) {
        g_usleep(1000);
    }
    g_assert_true(qtest_qom_get_bool(s, DEVICES "l1c0", "l2-link-up"));
    return s;
}

static QTestState *start_linked(L2Stub *stub, const char *props)
{
    return start_linked_traced(stub, props, "");
}

static bool output_ready(QTestState *s, unsigned n)
{
    return qtest_readq(s, BASE + n * STRIDE + 0x28) & 1;
}

static void wait_for_output(QTestState *s, unsigned n)
{
    for (unsigned i = 0; i < 10000; i++) {
        if (output_ready(s, n)) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static size_t receive_waiting(QTestState *s, unsigned n, uint8_t *reply)
{
    wait_for_output(s, n);
    return sgi_l1_test_receive(s, BASE + n * STRIDE, reply, 131);
}

/* With no peer connected the property changes nothing: no route. */
static void test_l2_link_down(void)
{
    L2Stub *stub = l2_stub_listen();
    g_autofree char *extra = g_strdup_printf(
        "-chardev socket,id=l2c,path=%s,server=on,wait=off ", stub->path);

    l2_stub_unlisten(stub);
    const char *const one[] = { L2_FETCH_PROPS ",l2=l2c,l2-partition-flag=2" };
    QTestState *s = start_with(1, one, extra);
    uint8_t reply[131];
    size_t length;

    g_assert_false(qtest_qom_get_bool(s, DEVICES "l1c0", "l2-link-up"));
    length = rpc(s, 0, 0x1ffff00a, 3, NULL, 0, 0, reply);

    expect_router_error(reply, length);
    length = rpc(s, 0, 0x2ffff003, 3, NULL, 0, 0, reply);
    expect_router_error(reply, length);
    expect_partition_request(s, 0, 0xb, 2);
    qtest_quit(s);
    l2_stub_free(stub);
}


/*
 * The daemon's recurring configuration request (L2 sheet, "The reply, and
 * the route it creates"). The stub plays the daemon: it sends the request
 * and reads what the controller puts on the link, which is two requests of
 * its own and then the reply. Integer descriptors are "00 00 0000 value".
 */
#define L2_CONFIG_SOURCE_WORD(rack, bay) \
    ((((rack) << 6 | (bay)) << 12) | SGI_L1_L2_CONFIG_TASK)

static uint32_t desc_integer(const uint8_t *segment, unsigned i)
{
    const uint8_t *desc = segment + SGI_L1_L2_SEG_DESC + 8 * i;

    g_assert_cmpuint(desc[0], ==, SGI_L1_L2_DESC_INTEGER);
    return ldl_be_p(desc + 4);
}

/* A blob descriptor's bytes: the offset counts 16 bytes off the wire. */
static const uint8_t *desc_blob(const uint8_t *segment, size_t length,
                                unsigned i, unsigned expect_length)
{
    const uint8_t *desc = segment + SGI_L1_L2_SEG_DESC + 8 * i;
    unsigned offset = ldl_be_p(desc + 4);

    g_assert_cmpuint(desc[0], ==, SGI_L1_L2_DESC_BLOB);
    g_assert_cmpuint(lduw_be_p(desc + 2), ==, expect_length);
    g_assert_cmpuint(offset, >=, SGI_L1_L2_DESC_PREFIX);
    g_assert_cmpuint(offset - SGI_L1_L2_DESC_PREFIX + expect_length, <=,
                     length);
    return segment + offset - SGI_L1_L2_DESC_PREFIX;
}

static void expect_config_request(L2Stub *stub, uint32_t source,
                                  uint32_t selector)
{
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    size_t length = l2_stub_read_segment(stub, segment, L2_STUB_TIMEOUT_MS);

    g_assert_cmpuint(length, ==, SGI_L1_L2_SEG_DESC + 8);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_COUNT], ==,
                     SGI_L1_L2_FIRST_SEGMENT | 1);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_CLASS], ==,
                     SGI_L1_L2_CLASS_REQUEST);
    g_assert_cmphex(ldl_be_p(segment + SGI_L1_L2_SEG_DEST), ==,
                    SGI_L1_L2_CONFIG_DEST);
    g_assert_cmphex(ldl_be_p(segment + SGI_L1_L2_SEG_SOURCE), ==, source);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_NDESC], ==, 1);
    g_assert_cmphex(lduw_be_p(segment + SGI_L1_L2_SEG_CODE), ==, 0);
    g_assert_cmpuint(desc_integer(segment, 0), ==, selector);
}

/*
 * Read the reply and check its header: class 1, back to the request's
 * source, the task 14 endpoint as source, status 0, the request's code and
 * sequence, one segment, six descriptors.
 */
static size_t read_config_reply(L2Stub *stub, uint8_t *segment, uint32_t source)
{
    size_t length = l2_stub_read_segment(stub, segment, L2_STUB_TIMEOUT_MS);

    g_assert_cmpuint(length, >=, SGI_L1_L2_SEG_DESC + 8);
    g_assert_cmpuint(lduw_be_p(segment), ==, length);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_COUNT], ==,
                     SGI_L1_L2_FIRST_SEGMENT | 1);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_CLASS], ==,
                     SGI_L1_L2_CLASS_RESPONSE);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_FLAGS], ==, 0);
    g_assert_cmphex((uint32_t)ldl_be_p(segment + SGI_L1_L2_SEG_DEST), ==,
                    L2_STUB_CONFIG_SOURCE);
    g_assert_cmphex(ldl_be_p(segment + SGI_L1_L2_SEG_SOURCE), ==, source);
    g_assert_cmphex(lduw_be_p(segment + SGI_L1_L2_SEG_CODE), ==, 0);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_SEQ_LOW], ==, 0x31);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_SEQ_HIGH], ==, 0x07);
    return length;
}

static void test_l2_config_reply(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, "brick-type=cbrick,named=on,rack=1,"
                                 "bay=4");
    uint32_t source = L2_CONFIG_SOURCE_WORD(1, 4);
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    const uint8_t *records;
    size_t length;

    length = l2_stub_build_config_request(segment, "SN12345", 0);
    l2_stub_write_segment(stub, segment, length);

    /* The controller asks for selectors 2 and 13 before it builds the reply. */
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_A);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_B);
    length = read_config_reply(stub, segment, source);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_NDESC], ==, 6);
    g_assert_cmpuint(desc_integer(segment, 0), ==, 7);
    g_assert_cmpuint(desc_integer(segment, 1), ==, 0);
    g_assert_cmpuint(desc_integer(segment, 2), ==, 1);
    g_assert_cmpuint(desc_integer(segment, 3), ==, 0x70);
    g_assert_cmpuint(*desc_blob(segment, length, 4, 1), ==, 0);
    records = desc_blob(segment, length, 5, 0x70);
    g_assert_cmpuint(ldl_be_p(records), !=, 0);
    g_assert_cmpuint(ldl_be_p(records + 0x08), ==, 1);
    g_assert_cmpuint(ldl_be_p(records + 0x0c), ==, 4);
    g_assert_cmpuint(ldl_be_p(records + 0x60), !=, 0);
    g_assert_cmpstr((const char *)records + 0x64, ==, "SN12345");

    /* A nonzero third descriptor ends the reply after the integer 7. */
    length = l2_stub_build_config_request(segment, NULL, 1);
    l2_stub_write_segment(stub, segment, length);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_A);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_B);
    length = read_config_reply(stub, segment, source);
    g_assert_cmpuint(segment[SGI_L1_L2_SEG_NDESC], ==, 1);
    g_assert_cmpuint(length, ==, SGI_L1_L2_SEG_DESC + 8);
    g_assert_cmpuint(desc_integer(segment, 0), ==, 7);
    qtest_quit(s);
    l2_stub_free(stub);
}

/* The records are the bricks the controller lists for "config". */
static void test_l2_config_reply_attached(void)
{
    L2Stub *stub = l2_stub_listen();
    g_autofree char *extra = g_strdup_printf(
        "-chardev socket,id=l2c,path=%s ", stub->path);
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4,l2=l2c",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(3, props, extra);
    uint32_t source = L2_CONFIG_SOURCE_WORD(1, 4);
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    const uint8_t *index, *records;
    size_t length;

    l2_stub_accept(stub);
    for (unsigned i = 0; i < 10000 &&
         !qtest_qom_get_bool(s, DEVICES "l1c0", "l2-link-up"); i++) {
        g_usleep(1000);
    }
    link_io(s, 0, 1);
    link_peer(s, 0, 2);
    length = l2_stub_build_config_request(segment, NULL, 0);
    l2_stub_write_segment(stub, segment, length);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_A);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_B);
    length = read_config_reply(stub, segment, source);
    g_assert_cmpuint(desc_integer(segment, 2), ==, 3);
    index = desc_blob(segment, length, 4, 3);
    records = desc_blob(segment, length, 5, 3 * 0x70);
    for (unsigned i = 0; i < 3; i++) {
        static const uint8_t bays[3] = { 4, 31, 7 };

        g_assert_cmpuint(index[i], ==, i);
        g_assert_cmpuint(ldl_be_p(records + 0x70 * i + 0x08), ==, 1);
        g_assert_cmpuint(ldl_be_p(records + 0x70 * i + 0x0c), ==, bays[i]);
        g_assert_cmpuint(ldl_be_p(records + 0x70 * i + 0x60), !=, 0);
    }
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * Nothing in the sheets says what an unnamed controller reports, so it
 * stays silent, and a request that is not selector 6 is not answered.
 */
static void test_l2_config_reply_refusals(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, "brick-type=cbrick");
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    size_t length = l2_stub_build_config_request(segment, NULL, 0);

    l2_stub_write_segment(stub, segment, length);
    g_assert_cmpuint(l2_stub_read_segment(stub, segment, 300), ==, 0);
    qtest_quit(s);
    l2_stub_free(stub);

    stub = l2_stub_listen();
    s = start_linked(stub, "brick-type=cbrick,named=on,rack=1,bay=4");
    length = l2_stub_build_config_request(segment, NULL, 0);
    stl_be_p(segment + SGI_L1_L2_SEG_DESC + 4, 9);
    l2_stub_write_segment(stub, segment, length);
    g_assert_cmpuint(l2_stub_read_segment(stub, segment, 300), ==, 0);
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * A configuration request that is cut short, or whose string descriptor
 * points outside the segment, does no harm: the first is not answered, and
 * the second is answered with an empty serial text (this model's choice; the
 * sheets do not say what the L1 does with a string it cannot read). The
 * controller stays up and answers the next good request.
 */
static void test_l2_config_request_malformed(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, "brick-type=cbrick,named=on,rack=1,"
                                 "bay=4");
    uint32_t source = L2_CONFIG_SOURCE_WORD(1, 4);
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    const uint8_t *records;
    size_t length, shorter;
    /* The string descriptor's text length and offset, each out of bounds. */
    static const struct {
        uint16_t text_length;
        uint32_t offset;
    } bad[] = {
        { 8, 0x1000 },
        { 200, SGI_L1_L2_SEG_DESC + 24 + 16 },
        { 8, 4 },
        { 0, SGI_L1_L2_SEG_DESC + 24 + 16 },
    };

    /* Three descriptors announced, two present. */
    length = l2_stub_build_config_request(segment, "SN12345", 0);
    shorter = SGI_L1_L2_SEG_DESC + 16;
    stw_be_p(segment, shorter);
    l2_stub_write_segment(stub, segment, shorter);
    g_assert_cmpuint(l2_stub_read_segment(stub, segment, 300), ==, 0);

    for (unsigned i = 0; i < G_N_ELEMENTS(bad); i++) {
        uint8_t *desc;

        length = l2_stub_build_config_request(segment, "SN12345", 0);
        desc = segment + SGI_L1_L2_SEG_DESC + 8;
        stw_be_p(desc + 2, bad[i].text_length);
        stl_be_p(desc + 4, bad[i].offset);
        l2_stub_write_segment(stub, segment, length);
        expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_A);
        expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_B);
        length = read_config_reply(stub, segment, source);
        records = desc_blob(segment, length, 5, 0x70);
        g_assert_cmpstr((const char *)records + 0x64, ==, "");
    }

    length = l2_stub_build_config_request(segment, "SN12345", 0);
    l2_stub_write_segment(stub, segment, length);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_A);
    expect_config_request(stub, source, SGI_L1_L2_CONFIG_SELECTOR_B);
    length = read_config_reply(stub, segment, source);
    records = desc_blob(segment, length, 5, 0x70);
    g_assert_cmpstr((const char *)records + 0x64, ==, "SN12345");
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * Trace output: run with the named events logged to a file, then read the
 * file once the machine has quit.
 */
static char *trace_start(const char *events, char **args)
{
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sgi-l1-trace-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);
    *args = g_strdup_printf("-trace enable=%s -D %s ", events, path);
    return path;
}

static char *trace_finish(QTestState *s, char *path)
{
    g_autoptr(GError) error = NULL;
    char *text = NULL;

    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_no_error(error);
    g_unlink(path);
    g_free(path);
    return text;
}

/*
 * L2 sheet, "Transport over the bulk endpoints", and the interface sheet's
 * bulk transfer layout: the forwarded request is one segment of 20 bytes
 * and eight per argument, first-segment bit and count 1 in byte 5, class 0,
 * destination, source, then code. The source packing is the chunk-transport
 * rows' (transport and subchannel in 11:5, return task 0x15).
 */
static void test_l2_forward_plain(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, L2_FETCH_PROPS);
    uint8_t args[10], segment[SGI_L1_L2_SEGMENT_MAX], reply[131];
    size_t length;

    integer(args, 0x11223344);
    integer(args + 5, 7);
    send_rpc(s, 0, 0x1ffff006, 0x1234, args, sizeof(args), 2);
    length = l2_stub_read_segment(stub, segment, L2_STUB_TIMEOUT_MS);
    g_assert_cmpuint(length, ==, 36);
    g_assert_cmpuint(lduw_be_p(segment), ==, 36);
    g_assert_cmpuint(segment[2], ==, 2);
    g_assert_cmpuint(segment[5], ==, 0x81);
    g_assert_cmpuint(segment[6], ==, 0);
    g_assert_cmphex(ldl_be_p(segment + 8), ==, 0x1ffff006);
    g_assert_cmphex(ldl_be_p(segment + 12), ==, L2_SOURCE_CHANNEL_10);
    g_assert_cmpuint(segment[16], ==, 2);
    g_assert_cmphex(lduw_be_p(segment + 18), ==, 0x1234);
    g_assert_cmphex(ldl_be_p(segment + 20), ==, 0);
    g_assert_cmphex(ldl_be_p(segment + 24), ==, 0x11223344);
    g_assert_cmphex(ldl_be_p(segment + 28), ==, 0);
    g_assert_cmphex(ldl_be_p(segment + 32), ==, 7);

    /* The response comes back as the L1's ordinary reply: result, then ints. */
    l2_stub_build_fetch_reply(reply, segment, 0x55);
    memcpy(segment, reply, 36);
    stl_be_p(segment + SGI_L1_L2_SEG_DESC + 4, -105);
    l2_stub_write_segment(stub, segment, 36);
    length = receive_waiting(s, 0, reply);
    g_assert_cmpuint(length, ==, 11);
    expect_code(reply, length, -105);
    g_assert_cmpuint(reply[5], ==, 1);
    g_assert_cmpuint(reply[6], ==, 0);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, 0x55);

    /* A non-integer argument has no descriptor form: no route. */
    args[0] = 1;
    args[1] = 'a';
    args[2] = 0;
    send_rpc(s, 0, 0x1ffff006, 3, args, 3, 1);
    length = receive_waiting(s, 0, reply);
    expect_router_error(reply, length);
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * Local requests are served as before with the link up, and a request the
 * stub does not answer gets no reply at all.
 */
static void test_l2_unanswered_and_local(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, L2_FETCH_PROPS ",l2-partition-flag=1");
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];

    expect_partition_request(s, 0, 0xb, 1);
    expect_partition_request(s, 0, 9, 2);
    send_rpc(s, 0, 0x1ffff006, 3, NULL, 0, 0);
    g_assert_cmpuint(l2_stub_serve(stub, segment, 2, L2_STUB_TIMEOUT_MS), ==,
                     SGI_L1_L2_SEG_DESC);
    send_rpc(s, 0, 0x2ffff00a, 3, NULL, 0, 0);
    g_assert_cmpuint(l2_stub_serve(stub, segment, 2, L2_STUB_TIMEOUT_MS), ==,
                     SGI_L1_L2_SEG_DESC);
    g_usleep(200000);
    g_assert_false(output_ready(s, 0));
    /* Local requests still answer after the traffic. */
    expect_partition_request(s, 0, 0xb, 1);
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * The PROM's partition-record fetch as the sheets give it: a 0x3001 chunk
 * (channel byte, four zero bytes, code, fragment count, 16-bit length)
 * carrying a router segment for controller type 1, task 0xa, code 1 and one
 * integer argument, as a boot run of the stock PROM sent it. The L1 rewrites
 * the source word, forwards the segment, and wraps the stub's one-segment
 * reply in a single 0x3001 frame whose count and length are the reply's.
 */
static size_t send_fetch(QTestState *s, uint8_t *inner)
{
    uint8_t frame[10 + 28] = { 0x10, 0, 0, 0, 0, 0x30, 0x01, 1, 0, 28 };
    const uint8_t request[28] = {
        0, 28, 2, 0, 0x55, 0x81, 0, 0,
        0x1f, 0xff, 0xf0, 0x0a, 0xde, 0xad, 0xbe, 0xef,
        1, 0, 0, 1,
        /* One integer argument: the requesting brick's bay, 4 in a boot run. */
        0, 0, 0, 0, 0, 0, 0, 4,
    };

    memcpy(inner, request, sizeof(request));
    memcpy(frame + 10, request, sizeof(request));
    sgi_l1_test_send(s, BASE, frame, sizeof(frame), false);
    return sizeof(request);
}

/*
 * A chunk request that is not for the L2 gets the same reply with a link as
 * without one, on a profile that has no IRouter of its own: the link only adds
 * the L2's destinations to what the chunk transport carries.
 */
static size_t send_chunk_other(QTestState *s, uint8_t *reply)
{
    uint8_t frame[10 + 28] = { 0x10, 0, 0, 0, 0, 0x30, 0x01, 1, 0, 28 };
    const uint8_t request[28] = {
        0, 28, 2, 0, 0x55, 0x81, 0, 0,
        0x3f, 0xff, 0xf0, 0x0a, 0xde, 0xad, 0xbe, 0xef,
        1, 0, 0, 1,
        0, 0, 0, 0, 0, 0, 0, 4,
    };

    memcpy(frame + 10, request, sizeof(request));
    sgi_l1_test_send(s, BASE, frame, sizeof(frame), false);
    g_usleep(200000);
    return output_ready(s, 0) ? sgi_l1_test_receive(s, BASE, reply, 131) : 0;
}

static void test_l2_chunk_other_destination(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_one(L2_FETCH_PROPS);
    uint8_t unlinked[131], linked[131];
    size_t unlinked_length = send_chunk_other(s, unlinked);
    size_t linked_length;

    qtest_quit(s);
    s = start_linked(stub, L2_FETCH_PROPS);
    linked_length = send_chunk_other(s, linked);
    g_assert_cmpuint(linked_length, ==, unlinked_length);
    g_assert_cmpmem(linked, linked_length, unlinked, unlinked_length);
    qtest_quit(s);
    l2_stub_free(stub);
}

/* A segment event names the controller whose link it crossed. */
static void test_l2_segment_trace(void)
{
    g_autofree char *args = NULL;
    char *path = trace_start("sgi_l1_l2_segment", &args);
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked_traced(stub, L2_FETCH_PROPS, args);
    uint8_t request[28], segment[SGI_L1_L2_SEGMENT_MAX], reply[131];
    g_autofree char *text = NULL;

    send_fetch(s, request);
    g_assert_cmpuint(l2_stub_serve(stub, segment, 2, L2_STUB_TIMEOUT_MS), ==,
                     28);
    receive_waiting(s, 0, reply);
    text = trace_finish(s, path);
    g_assert_nonnull(strstr(text,
                            "profile=cbrick rack=1 bay=4 to_l2=1 len=28"));
    g_assert_nonnull(strstr(text,
                            "profile=cbrick rack=1 bay=4 to_l2=0 len=36"));
    l2_stub_free(stub);
}

/* A request event shows what the node sent, with its integer arguments. */
static void test_request_trace(void)
{
    g_autofree char *args = NULL;
    char *path = trace_start("sgi_l1_request", &args);
    const char *const one[] = { L2_FETCH_PROPS ",l2-partition-flag=1" };
    QTestState *s = start_with(1, one, args);
    uint8_t arg[5];
    g_autofree char *text = NULL;

    expect_partition_request(s, 0, 0xb, 1);
    integer(arg, 0x1234);
    send_rpc(s, 0, LOCAL_GENERAL, 0x1006, arg, sizeof(arg), 1);
    text = trace_finish(s, path);
    g_assert_nonnull(strstr(text, "profile=cbrick rack=1 bay=4 transport=0 "
                            "address=0x"));
    g_assert_nonnull(strstr(text, "command=0x000b count=0 args="));
    g_assert_nonnull(strstr(text, "command=0x1006 count=1 args=0000001234"));
}

/*
 * A chunk event gives each frame's code, count, length field and payload
 * length in both directions. A 300-byte reply is three frames, and the
 * first carries its own payload length, not the segment's.
 */
static void test_chunk_trace(void)
{
    g_autofree char *args = NULL;
    char *path = trace_start("sgi_l1_chunk", &args);
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked_traced(stub, L2_FETCH_PROPS, args);
    uint8_t chain[1][SGI_L1_L2_SEGMENT_MAX], segment[1024], sent[28];
    unsigned sizes[] = { 300 };
    uint8_t frame[131];
    g_autofree char *text = NULL;

    send_fetch(s, sent);
    g_assert_cmpuint(l2_stub_read_segment(stub, segment, L2_STUB_TIMEOUT_MS),
                     ==, 28);
    l2_stub_build_chain(chain, sizes, 1, segment);
    l2_stub_write_segment(stub, chain[0], 300);
    for (unsigned i = 0; i < 3; i++) {
        receive_waiting(s, 0, frame);
    }
    text = trace_finish(s, path);
    g_assert_nonnull(strstr(text, "to_node=0 transport=0 channel=16 "
                            "code=0x3001 count=1 length_field=28 payload=28"));
    g_assert_nonnull(strstr(text, "to_node=1 transport=0 channel=16 "
                            "code=0x3001 count=1 length_field=113 "
                            "payload=113"));
    g_assert_nonnull(strstr(text, "code=0x3003 count=0 length_field=0 "
                            "payload=113"));
    g_assert_nonnull(strstr(text, "code=0x3002 count=0 length_field=0 "
                            "payload=74"));
    l2_stub_free(stub);
}

static void test_l2_fetch_single_buffer(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, L2_FETCH_PROPS ",l2-partition-flag=1");
    uint8_t sent[28], segment[SGI_L1_L2_SEGMENT_MAX], reply[131];
    size_t length = send_fetch(s, sent);

    g_assert_cmpuint(l2_stub_serve(stub, segment, 2, L2_STUB_TIMEOUT_MS), ==,
                     length);
    /* Same bytes, but the source word is the L1's return address. */
    g_assert_cmpmem(segment, 12, sent, 12);
    g_assert_cmphex(ldl_be_p(segment + 12), ==, L2_SOURCE_CHANNEL_10);
    g_assert_cmpmem(segment + 16, 12, sent + 16, 12);

    length = receive_waiting(s, 0, reply);
    g_assert_cmpuint(length, ==, 10 + 36);
    g_assert_cmphex(reply[0], ==, 0x30);
    g_assert_cmphex(ldl_be_p(reply + 1), ==, 0);
    g_assert_cmphex(lduw_be_p(reply + 5), ==, 0x3001);
    g_assert_cmpuint(reply[7], ==, 1);
    g_assert_cmpuint(lduw_be_p(reply + 8), ==, 36);
    /* The PROM reads byte 3 and the word at 32 of this payload. */
    g_assert_cmpuint(reply[10 + 3], ==, 0);
    g_assert_cmphex(ldl_be_p(reply + 10 + 32), ==, 2);
    /* The response is addressed to the requester the source word named. */
    g_assert_cmphex(ldl_be_p(reply + 10 + 8), ==, L2_SOURCE_CHANNEL_10);
    g_assert_false(output_ready(s, 0));
    qtest_quit(s);
    l2_stub_free(stub);
}

/*
 * Reply frames carry at most 113 bytes. The first frame of each segment is
 * 0x3001 with the segment's remaining-count byte and the frame's own payload
 * length; the others are 0x3003 while the segment has more and 0x3002 on the
 * frame that exhausts it, with a zero length field. The stock reader took the
 * single-frame and three-frame forms of this in a boot run and refused a
 * 0x3001 frame that carried the whole segment's length; the sizes here are
 * the edges of the rule and chains of two and three segments.
 */
static void expect_frames(QTestState *s, const unsigned *sizes, unsigned n,
                          uint8_t chain[][SGI_L1_L2_SEGMENT_MAX])
{
    for (unsigned i = 0; i < n; i++) {
        for (unsigned offset = 0; offset < sizes[i]; ) {
            uint8_t frame[131];
            unsigned bytes = MIN(113, sizes[i] - offset);
            bool last = offset + bytes == sizes[i];
            uint16_t code = last ? 0x3002 : 0x3003;
            size_t length = receive_waiting(s, 0, frame);

            if (!offset) {
                code = 0x3001;
            }
            g_assert_cmpuint(length, ==, 10 + bytes);
            g_assert_cmphex(frame[0], ==, 0x30);
            g_assert_cmphex(ldl_be_p(frame + 1), ==, 0);
            g_assert_cmphex(lduw_be_p(frame + 5), ==, code);
            if (code == 0x3001) {
                g_assert_cmpuint(frame[7], ==, n - i);
                g_assert_cmpuint(lduw_be_p(frame + 8), ==, bytes);
            } else {
                g_assert_cmpuint(frame[7], ==, 0);
                g_assert_cmpuint(lduw_be_p(frame + 8), ==, 0);
            }
            g_assert_cmpmem(frame + 10, bytes, chain[i] + offset, bytes);
            offset += bytes;
        }
    }
    g_assert_false(output_ready(s, 0));
}

static void test_l2_fetch_multi_frame(void)
{
    static const struct {
        unsigned sizes[3];
        unsigned count;
    } cases[] = {
        { { 113 }, 1 }, { { 114 }, 1 }, { { 226 }, 1 }, { { 227 }, 1 },
        { { 1024 }, 1 }, { { 300, 200 }, 2 }, { { 113, 28, 1024 }, 3 },
    };

    for (unsigned c = 0; c < G_N_ELEMENTS(cases); c++) {
        L2Stub *stub = l2_stub_listen();
        QTestState *s = start_linked(stub, L2_FETCH_PROPS);
        uint8_t chain[3][SGI_L1_L2_SEGMENT_MAX], segment[1024], sent[28];

        send_fetch(s, sent);
        g_assert_cmpuint(l2_stub_read_segment(stub, segment,
                                              L2_STUB_TIMEOUT_MS), ==, 28);
        l2_stub_build_chain(chain, cases[c].sizes, cases[c].count, segment);
        for (unsigned i = 0; i < cases[c].count; i++) {
            l2_stub_write_segment(stub, chain[i], cases[c].sizes[i]);
        }
        expect_frames(s, cases[c].sizes, cases[c].count, chain);
        qtest_quit(s);
        l2_stub_free(stub);
    }
}

/*
 * Anything on the link that is not a well-formed segment is skipped, and a
 * response that nobody asked for is dropped, so a later real reply still
 * reaches the requester.
 */
static void test_l2_link_noise(void)
{
    L2Stub *stub = l2_stub_listen();
    QTestState *s = start_linked(stub, L2_FETCH_PROPS);
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX], reply[131], noise[] = {
        9,                          /* a kind nobody defines */
        SGI_L1_L2_KIND_SEGMENT, 0, 3, /* a segment too short to hold a count */
    };
    size_t length;

    l2_stub_write_raw(stub, noise, sizeof(noise));
    /* A response for a requester that has nothing pending: channel 0x11. */
    memset(segment, 0, 36);
    l2_stub_build_fetch_reply(segment, segment, 1);
    stl_be_p(segment + SGI_L1_L2_SEG_DEST, L2_SOURCE_CHANNEL_10 + 0x20);
    l2_stub_write_segment(stub, segment, 36);
    /* And one that names no requester at all. */
    stl_be_p(segment + SGI_L1_L2_SEG_DEST, 0);
    l2_stub_write_segment(stub, segment, 36);
    send_rpc(s, 0, 0x1ffff00a, 3, NULL, 0, 0);
    g_assert_cmpuint(l2_stub_serve(stub, segment, 2, L2_STUB_TIMEOUT_MS), ==,
                     SGI_L1_L2_SEG_DESC);
    length = receive_waiting(s, 0, reply);
    g_assert_cmpuint(length, ==, 11);
    expect_code(reply, length, 0);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, 2);
    qtest_quit(s);
    l2_stub_free(stub);
}


/*
 * A link carries state outside the machine, so migration is refused while
 * the property is set, connected or not. Without it migration is not
 * blocked by the link.
 */
static void test_l2_migration_blocker(void)
{
    L2Stub *stub = l2_stub_listen();
    g_autofree char *extra = g_strdup_printf(
        "-chardev socket,id=l2c,path=%s,server=on,wait=off ", stub->path);
    const char *const linked[] = { L2_FETCH_PROPS ",l2=l2c" };
    QTestState *s;
    g_autoptr(QDict) reply = NULL;

    l2_stub_unlisten(stub);
    s = start_with(1, linked, extra);
    reply = qtest_qmp(s,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    g_assert_true(qdict_haskey(reply, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                          "desc"), "L2 link"));
    qtest_quit(s);
    l2_stub_free(stub);

    s = start_one(L2_FETCH_PROPS);
    qobject_unref(reply);
    reply = qtest_qmp(s,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    /* An unblocked migration starts and fails later, on the socket. */
    if (qdict_haskey(reply, "error")) {
        g_assert_null(strstr(qdict_get_str(qdict_get_qdict(reply, "error"),
                                           "desc"), "L2 link"));
    }
    qtest_quit(s);
}

/*
 * ctci is not modelled: it is reported as an unknown command, and nothing
 * runs on the other C-brick or its I-brick.
 */
static void test_ctci_unimplemented(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=cbrick,named=on,rack=1,bay=7",
    };
    QTestState *s = start_with(2, props, "");

    link_peer(s, 0, 1);
    expect_command_code(s, 0, "ctci brick partition 7", NOT_FOUND, -1);
    expect_partition(s, 1, 0);
    qtest_quit(s);
}

/*
 * The profile check holds whichever side of realize the link is set. A link
 * given with the device is set before the device has a profile, and is
 * checked when it realizes; a link set afterwards is checked when it is set.
 * These are the controllers that exist before the one under test.
 */
static const char *const link_base[] = {
    "brick-type=cbrick,named=on,rack=1,bay=4",
    "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    "brick-type=ip45",
    "brick-type=ip45",
};

#define NOT_C_BRICK "*peer of a C-brick must be another C-brick*"
#define NOT_AN_IBRICK "*io-peer must name an I-brick*"
#define C_BRICK_ONLY "*io-peer is for the C-brick and Cx-brick profiles only*"

#define LATE(props) "-device sgi-l1,id=x," props
#define HOST(id) "-device sgi-l1,id=" id ",brick-type=cbrick," \
                 "io-peer=" DEVICES "l1c1 "

static const struct {
    const char *extra;
    const char *message;
} link_cases[] = {
    { LATE("brick-type=cbrick,peer=" DEVICES "l1c2"), NOT_C_BRICK },
    { LATE("brick-type=cbrick,peer=" DEVICES "l1c1"), NOT_C_BRICK },
    { LATE("brick-type=ip45,peer=" DEVICES "l1c0"), NOT_C_BRICK },
    { LATE("brick-type=ip45,peer=" DEVICES "l1c1"), NOT_C_BRICK },
    /* The property may also come before the type that decides it. */
    { LATE("peer=" DEVICES "l1c2,brick-type=cbrick"), NOT_C_BRICK },
    { LATE("brick-type=cbrick,io-peer=" DEVICES "l1c0"), NOT_AN_IBRICK },
    { LATE("brick-type=cbrick,io-peer=" DEVICES "l1c2"), NOT_AN_IBRICK },
    { LATE("brick-type=ip45,io-peer=" DEVICES "l1c1"), C_BRICK_ONLY },
    { LATE("brick-type=ibrick,mac=" MAC_PROP ",io-peer=" DEVICES "l1c1"),
      C_BRICK_ONLY },
    /* A second C-brick for an I-brick that already has one. */
    { HOST("h") LATE("brick-type=cbrick,io-peer=" DEVICES "l1c1"),
      "*I-brick takes one C-brick*" },
};

static void test_link_before_realize(gconstpointer opaque)
{
    unsigned index = GPOINTER_TO_UINT(opaque);

    if (g_test_subprocess()) {
        QTestState *s = start_with(4, link_base, link_cases[index].extra);

        qtest_quit(s);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(link_cases[index].message);
}

static char *link_target(QTestState *s, const char *id, const char *property)
{
    g_autofree char *path = g_strdup_printf(DEVICES "%s", id);
    QDict *result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    char *target = g_strdup(qdict_get_str(result, "return"));

    qobject_unref(result);
    return target;
}

static void test_link_before_realize_accepted(void)
{
    /* Pairs the profiles allow, given with the device. */
    QTestState *s = start_with(4, link_base,
        "-device sgi-l1,id=x,brick-type=cbrick,peer=" DEVICES "l1c0 "
        "-device sgi-l1,id=y,brick-type=ip45,peer=" DEVICES "l1c2 "
        "-device sgi-l1,id=z,io-peer=" DEVICES "l1c1,brick-type=cbrick");
    g_autofree char *peer = link_target(s, "x", "peer");
    g_autofree char *other = link_target(s, "y", "peer");
    g_autofree char *cable = link_target(s, "z", "io-peer");

    g_assert_cmpstr(peer, ==, DEVICES "l1c0");
    g_assert_cmpstr(other, ==, DEVICES "l1c2");
    g_assert_cmpstr(cable, ==, DEVICES "l1c1");
    /* The cable made first takes the I-brick; a second is refused now. */
    g_assert_true(qom_link_refused(s, 0, "io-peer", DEVICES "l1c1"));
    qtest_quit(s);
}

static void test_link_after_realize(void)
{
    QTestState *s = start_with(4, link_base, "");

    /* The same pairs as before realize, refused as the link is set. */
    g_assert_true(qom_link_refused(s, 0, "peer", DEVICES "l1c2"));
    g_assert_true(qom_link_refused(s, 2, "peer", DEVICES "l1c0"));
    g_assert_true(qom_link_refused(s, 0, "peer", DEVICES "l1c1"));
    g_assert_true(qom_link_refused(s, 1, "peer", DEVICES "l1c0"));
    g_assert_true(qom_link_refused(s, 0, "io-peer", DEVICES "l1c2"));
    g_assert_true(qom_link_refused(s, 2, "io-peer", DEVICES "l1c1"));
    g_assert_true(qom_link_refused(s, 1, "io-peer", DEVICES "l1c1"));
    /* The Origin 300 pair is as free as before. */
    qom_link(s, 2, "peer", DEVICES "l1c3");
    qom_link(s, 3, "peer", DEVICES "l1c2");
    qtest_quit(s);
}

static void test_origin300_unchanged(void)
{
    /* The new properties change no Origin 300 reply. */
    QTestState *s = start_one("brick-type=ip45,rack=1,bay=1");
    uint8_t reply[131];
    size_t length = rpc(s, 0, LOCAL_GENERAL, 3, NULL, 0, 0, reply);

    g_assert_cmpuint(length, ==, 16);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, packed(1, 1));
    g_assert_cmphex(ldl_be_p(reply + 12), ==, '4');
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 46);
    qtest_quit(s);
}

static void test_realize_refusal(gconstpointer opaque)
{
    static const struct {
        const char *props;
        const char *message;
    } cases[] = {
        { "brick-type=ibrick", "*I-brick profile needs a mac*" },
        { "brick-type=cbrick,brick-type-code=0x43",
          "*brick-type-code is not accepted*" },
        { "brick-type=cbrick,irouter-compat=on", "*Origin 300 compatibility*" },
        { "brick-type=cbrick,o300-l1cmd-compat=on",
          "*Origin 300 compatibility*" },
        { "brick-type=cbrick,dimm-mask=0x1ff", "*dimm-mask*" },
        { "brick-type=cbrick,pimm-mask=4", "*pimm-mask*" },
        { "brick-type=router,router-board=nosuch", "*router-board*" },
        { "brick-type=cbrick,router-board=rbrick", "*router-board*" },
        { "brick-type=router,router-board=rbrick,node-board-name=X",
          "*node board identity needs a name, part and revision*" },
        { "brick-type=cbrick,io-port=2,c-port=2", "*different ports*" },
        { "brick-type=cbrick,io-port=6", "*port must be*" },
        { "brick-type=cbrick,identity-index=50", "*identity-index*" },
        { "brick-type=cbrick,mac=" MAC_PROP, "*mac is for the I-brick*" },
        { "brick-type=ibrick,mac=" MAC_PROP ",dimm-mask=3",
          "*apply to the C-brick*" },
        { "brick-type=vbrick,mac=" MAC_PROP, "*do not apply to the vbrick*" },
        { "brick-type=vbrick,dimm-mask=3", "*do not apply to the vbrick*" },
        { "brick-type=ip45,named=on", "*named applies to*" },
        { "brick-type=cbrick,rack=1023", "*rack or bay must be*" },
        { "brick-type=cbrick,partition=65536", "*partition must fit*" },
    };
    unsigned index = GPOINTER_TO_UINT(opaque);

    if (g_test_subprocess()) {
        QTestState *s = start_one(cases[index].props);

        qtest_quit(s);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(cases[index].message);
}

static void migrate_pair(QTestState *from, QTestState *to, const char *uri)
{
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(from, "completed");
    sgi_wait_migration(to, "completed");
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-l1-o3k-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL, *output = NULL;
    const char *const props[] = { "brick-type=cbrick,partition=2" };
    QTestState *from, *to;
    int32_t code;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_with(1, props, "");
    to = start_with(1, props, "-incoming defer");
    output = command(from, 0, "brick rack 3", &code);
    g_free(output);
    output = command(from, 0, "brick slot 1", &code);
    g_free(output);
    output = command(from, 0, "brick partition 9", &code);
    migrate_pair(from, to, uri);
    /* The pending name and partition crossed; neither has taken effect. */
    expect_unnamed_identity(to, 0);
    expect_command(to, 0, "brick",
                   "rack: ---, slot: --, partition: 2, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    g_free(output);
    output = command(to, 0, "reboot_l1", &code);
    expect_identity(to, 0, LOCAL_GENERAL, packed(3, 1), 'C');
    expect_command(to, 0, "brick",
                   "rack: 003, slot: 01, partition: 9, type: C [?MB flash], "
                   "serial:(null), source: EEPROM\n");
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_migration_profile_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-l1-o3k-mismatch-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL;
    const char *const source[] = { "brick-type=cbrick" };
    const char *const destination[] = { "brick-type=ibrick,mac=" MAC_PROP };
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_with(1, source, "");
    to = start_with(1, destination, "-incoming defer");
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_autofree char *name = NULL;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-l1-o3k/identity", test_identity);
    qtest_add_func("sgi-l1-o3k/unnamed", test_unnamed);
    qtest_add_func("sgi-l1-o3k/naming-on-restart",
                   test_naming_takes_effect_on_restart);
    qtest_add_func("sgi-l1-o3k/half-named", test_half_named);
    qtest_add_func("sgi-l1-o3k/retention", test_retention);
    qtest_add_func("sgi-l1-o3k/records/ibrick", test_ibrick_records);
    qtest_add_func("sgi-l1-o3k/records/vbrick", test_vbrick_records);
    qtest_add_func("sgi-l1-o3k/records/router", test_router_records);
    qtest_add_func("sgi-l1-o3k/records/cbrick", test_cbrick_records);
    qtest_add_func("sgi-l1-o3k/records/dimm-numbering", test_dimm_numbering);
    qtest_add_func("sgi-l1-o3k/attached/bricks", test_attached_bricks);
    qtest_add_func("sgi-l1-o3k/attached/ibrick-graphics",
                   test_ibrick_attached_graphics);
    qtest_add_func("sgi-l1-o3k/attached/vbrick", test_vbrick_attached);
    qtest_add_func("sgi-l1-o3k/attached/nbrick-graphics",
                   test_nbrick_attached_graphics);
    qtest_add_func("sgi-l1-o3k/attached/ports", test_attached_port_numbering);
    qtest_add_func("sgi-l1-o3k/attached/unnamed", test_unnamed_attachment);
    qtest_add_func("sgi-l1-o3k/attached/type-four", test_type_four_requests);
    qtest_add_func("sgi-l1-o3k/refusals", test_refusals);
    qtest_add_func("sgi-l1-o3k/firmware-revision", test_firmware_revision);
    qtest_add_func("sgi-l1-o3k/port-speed", test_port_speed);
    qtest_add_func("sgi-l1-o3k/ibrick-power", test_ibrick_power_is_its_own);
    qtest_add_func("sgi-l1-o3k/relay", test_broadcast_and_relay);
    qtest_add_func("sgi-l1-o3k/task3-line-endings",
                   test_task3_line_endings);
    qtest_add_func("sgi-l1-o3k/restart-window", test_restart_window);
    qtest_add_func("sgi-l1-o3k/link-refusals", test_link_refusals);
    qtest_add_func("sgi-l1-o3k/origin300-unchanged", test_origin300_unchanged);
    qtest_add_func("sgi-l1-o3k/nested-broadcast", test_nested_broadcast);
    qtest_add_func("sgi-l1-o3k/relay-depth", test_relay_depth);
    qtest_add_func("sgi-l1-o3k/star-own-restart",
                   test_star_during_own_restart);
    qtest_add_func("sgi-l1-o3k/star-reach", test_star_reach);
    qtest_add_func("sgi-l1-o3k/second-link-refused",
                   test_second_link_refused_keeps_first);
    qtest_add_func("sgi-l1-o3k/cti-without-ibrick", test_cti_without_ibrick);
    qtest_add_func("sgi-l1-o3k/relay-during-restart",
                   test_relay_during_restart);
    qtest_add_func("sgi-l1-o3k/config-order", test_config_order_and_blocks);
    qtest_add_func("sgi-l1-o3k/attached/unnamed-then-named",
                   test_unnamed_attachment_gets_named);
    qtest_add_func("sgi-l1-o3k/partition-limit", test_partition_limit);
    qtest_add_func("sgi-l1-o3k/partition-identity", test_partition_identity);
    qtest_add_func("sgi-l1-o3k/no-l2-route", test_no_l2_route);
    qtest_add_func("sgi-l1-o3k/l2-partition-flag", test_l2_partition_flag);
    qtest_add_func("sgi-l1-o3k/l2-partition-flag/refusal",
                   test_l2_partition_flag_refusal);
    qtest_add_func("sgi-l1-o3k/l2-link/down", test_l2_link_down);
    qtest_add_func("sgi-l1-o3k/l2-link/chunk-other-destination",
                   test_l2_chunk_other_destination);
    qtest_add_func("sgi-l1-o3k/l2-link/segment-trace", test_l2_segment_trace);
    qtest_add_func("sgi-l1-o3k/request-trace", test_request_trace);
    qtest_add_func("sgi-l1-o3k/l2-link/config-request-malformed",
                   test_l2_config_request_malformed);
    qtest_add_func("sgi-l1-o3k/l2-link/chunk-trace", test_chunk_trace);
    qtest_add_func("sgi-l1-o3k/l2-link/forward-plain", test_l2_forward_plain);
    qtest_add_func("sgi-l1-o3k/l2-link/unanswered-and-local",
                   test_l2_unanswered_and_local);
    qtest_add_func("sgi-l1-o3k/l2-link/fetch-single-buffer",
                   test_l2_fetch_single_buffer);
    qtest_add_func("sgi-l1-o3k/l2-link/fetch-multi-frame",
                   test_l2_fetch_multi_frame);
    qtest_add_func("sgi-l1-o3k/l2-link/noise", test_l2_link_noise);
    qtest_add_func("sgi-l1-o3k/l2-link/config-reply", test_l2_config_reply);
    qtest_add_func("sgi-l1-o3k/l2-link/config-reply/attached",
                   test_l2_config_reply_attached);
    qtest_add_func("sgi-l1-o3k/l2-link/config-reply/refusals",
                   test_l2_config_reply_refusals);
    qtest_add_func("sgi-l1-o3k/l2-link/migration-blocker",
                   test_l2_migration_blocker);
    qtest_add_func("sgi-l1-o3k/partition-clear", test_partition_clear);
    qtest_add_func("sgi-l1-o3k/ctci", test_ctci_unimplemented);
    for (unsigned i = 0; i < G_N_ELEMENTS(link_cases); i++) {
        name = g_strdup_printf("sgi-l1-o3k/link-before-realize/%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_link_before_realize);
        g_free(name);
        name = NULL;
    }
    qtest_add_func("sgi-l1-o3k/link-before-realize/accepted",
                   test_link_before_realize_accepted);
    qtest_add_func("sgi-l1-o3k/link-after-realize", test_link_after_realize);
    for (unsigned i = 0; i < 15; i++) {
        name = g_strdup_printf("sgi-l1-o3k/realize-refusal/%u", i);
        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_realize_refusal);
        g_free(name);
        name = NULL;
    }
    qtest_add_func("sgi-l1-o3k/migration", test_migration);
    qtest_add_func("sgi-l1-o3k/migration-mismatch",
                   test_migration_profile_mismatch);
    return g_test_run();
}
