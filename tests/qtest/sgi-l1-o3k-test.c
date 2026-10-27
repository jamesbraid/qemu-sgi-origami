/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The C-brick and I-brick profiles of the L1 controller. Each case drives
 * the controller through its UART window with the same protocol frames the
 * Origin 300 profile uses; no firmware runs.
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

static size_t rpc(QTestState *s, unsigned n, uint32_t destination,
                  uint16_t code, const uint8_t *args, size_t args_len,
                  unsigned count, uint8_t *reply)
{
    uint8_t request[136] = { 0x10, destination >> 24, destination >> 16,
                             destination >> 8, destination, code >> 8, code,
                             count };

    g_assert_cmpuint(args_len, <=, sizeof(request) - 8);
    if (args_len) {
        memcpy(request + 8, args, args_len);
    }
    sgi_l1_test_send(s, BASE + n * STRIDE, request, 8 + args_len, false);
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
                   "rack: ---, slot: --, partition: 2, type: C, "
                   "source: EEPROM\n");
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
                   "rack: ---, slot: --, partition: 2, type: C, "
                   "source: EEPROM\n");
    g_free(output);
    output = command(s, 0, "reboot_l1", &code);
    g_assert_cmpint(code, ==, 0);
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 1), 'C');
    expect_command(s, 0, "config", "0: 003c01\n");
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 01, partition: 9, type: C, "
                   "source: EEPROM\n");
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
                   "rack: 003, slot: 07, partition: 5, type: C, "
                   "source: EEPROM\n");
    after_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, after);
    g_assert_cmpuint(after_length, ==, before_length);
    g_assert_cmpmem(after, after_length, before, before_length);
    /* So does a node reset. */
    qtest_system_reset(s);
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 7), 'C');
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 07, partition: 5, type: C, "
                   "source: EEPROM\n");
    after_length = eeprom(s, 0, LOCAL_GENERAL, 0, 2, 0, 64, after);
    g_assert_cmpmem(after, after_length, before, before_length);
    /* So does a node power-off. */
    output = command(s, 0, "power down", NULL);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    expect_command(s, 0, "power check", "power appears off\n");
    expect_identity(s, 0, LOCAL_GENERAL, packed(3, 7), 'C');
    expect_command(s, 0, "brick",
                   "rack: 003, slot: 07, partition: 5, type: C, "
                   "source: EEPROM\n");
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
    length = rpc(s, 1, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_code(reply, length, -101);

    /* Removing the cable returns the entry to absent. */
    qom_link(s, 0, "io-peer", "");
    length = rpc(s, 0, LOCAL_GENERAL, 0xc, NULL, 0, 0, reply);
    expect_pairs(reply, length, 8, none);
    expect_command(s, 0, "config", "0: 001c04\n5: 001c07\n");
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

static void test_ibrick_power_is_its_own(void)
{
    const char *const props[] = {
        "brick-type=cbrick,named=on,rack=1,bay=4",
        "brick-type=ibrick,named=on,rack=1,bay=31,mac=" MAC_PROP,
    };
    QTestState *s = start_with(2, props, "");
    g_autofree char *output = NULL;

    link_io(s, 0, 1);
    expect_command(s, 1, "power check", "power appears off\n");
    expect_command(s, 0, "power check", "power appears on\n");
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
                   "001i31:\npower appears off\n");
    expect_command(s, 0, "ctc power check",
                   "001c07:\npower appears on\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n"
                   "001i31:\npower appears off\n");
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
                   "rack: 001, slot: 31, partition: 4, type: I, "
                   "source: EEPROM\n");
    expect_command(s, 2, "brick",
                   "rack: 001, slot: 07, partition: 4, type: C, "
                   "source: EEPROM\n");
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
    g_autofree char *needle = g_strdup_printf("partition: %u,", want);

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
    expect_command(s, 2, "cti power check", "001i30:\npower appears off\n");
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
            "001c04:\n001i31:\npower appears off\n001i31:\n"));
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
    expect_command(s, 0, "cti power check", "001i31:\npower appears off\n");
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
    expect_command(s, 0, "cti power check", "001i31:\npower appears off\n");
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
    expect_command(s, 0, "cti power check", "001i31:\npower appears off\n");
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
                   "001i31:\npower appears off\n");
    link_peer(s, 0, 2);
    /* The port numbers, not the order the cables were made, set the order. */
    expect_command(s, 0, "config", "0: 001c04\n5: 001c07\n10: 001i31\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001c07:\npower appears on\n"
                   "001i31:\npower appears off\n");
    qom_link(s, 0, "peer", "");
    expect_command(s, 0, "config", "0: 001c04\n10: 001i31\n");
    expect_command(s, 0, "* power check",
                   "001c04:\npower appears on\n"
                   "001i31:\npower appears off\n");
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
#define C_BRICK_ONLY "*io-peer is for the C-brick profile only*"

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
        { "brick-type=cbrick,revision-major=1,revision-minor=2,"
          "revision-patch=3", "*revision is unknown*" },
        { "brick-type=cbrick,irouter-compat=on", "*Origin 300 compatibility*" },
        { "brick-type=cbrick,o300-l1cmd-compat=on",
          "*Origin 300 compatibility*" },
        { "brick-type=cbrick,dimm-mask=0x1ff", "*dimm-mask*" },
        { "brick-type=cbrick,pimm-mask=4", "*pimm-mask*" },
        { "brick-type=cbrick,io-port=2,c-port=2", "*different ports*" },
        { "brick-type=cbrick,io-port=6", "*port must be*" },
        { "brick-type=cbrick,identity-index=50", "*identity-index*" },
        { "brick-type=cbrick,mac=" MAC_PROP, "*mac is for the I-brick*" },
        { "brick-type=ibrick,mac=" MAC_PROP ",dimm-mask=3",
          "*apply to the C-brick*" },
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
                   "rack: ---, slot: --, partition: 2, type: C, "
                   "source: EEPROM\n");
    g_free(output);
    output = command(to, 0, "reboot_l1", &code);
    expect_identity(to, 0, LOCAL_GENERAL, packed(3, 1), 'C');
    expect_command(to, 0, "brick",
                   "rack: 003, slot: 01, partition: 9, type: C, "
                   "source: EEPROM\n");
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
    qtest_add_func("sgi-l1-o3k/records/cbrick", test_cbrick_records);
    qtest_add_func("sgi-l1-o3k/records/dimm-numbering", test_dimm_numbering);
    qtest_add_func("sgi-l1-o3k/attached/bricks", test_attached_bricks);
    qtest_add_func("sgi-l1-o3k/attached/ports", test_attached_port_numbering);
    qtest_add_func("sgi-l1-o3k/attached/unnamed", test_unnamed_attachment);
    qtest_add_func("sgi-l1-o3k/attached/type-four", test_type_four_requests);
    qtest_add_func("sgi-l1-o3k/refusals", test_refusals);
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
