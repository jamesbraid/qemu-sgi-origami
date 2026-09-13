/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reviewed Router compatibility slots, through the real vector target. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define ROUTER_NIC 0x38
#define NIC_PHASES ((UINT64_C(47) << 27) | (UINT64_C(47) << 20))
#define NIC_RESET ((UINT64_C(49) << 27) | (UINT64_C(49) << 20) | 2)
#define DATA_SIZE 2048

typedef struct NIC {
    QTestState *qts;
    unsigned int node;
} NIC;

static const char *fixture_path(NIC *nic)
{
    return nic->node < 2 ? "/machine/peripheral/router0" :
                           "/machine/peripheral/router1";
}

static void select_port(NIC *nic)
{
    qtest_qmp_assert_success(nic->qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':'port',"
        "'value':%u}}", fixture_path(nic), nic->node == 1 ? 4 : 5);
}

static uint64_t nic_read(NIC *nic)
{
    QDict *response;
    uint64_t value;

    response = qtest_qmp(nic->qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'register'}}",
        fixture_path(nic));
    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static bool nic_request(NIC *nic, uint64_t value)
{
    QDict *response;
    bool accepted;

    select_port(nic);
    response = qtest_qmp(nic->qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':'register',"
        "'value':%llu}}", fixture_path(nic), (unsigned long long)value);
    accepted = qdict_haskey(response, "return");
    if (!accepted) {
        /* A test-fixture refusal, not an asserted hardware error reply. */
        g_assert_cmpstr(qdict_get_str(qdict_get_qdict(response, "error"),
                                      "desc"), ==,
                        "Router target refused the test request");
    }
    qobject_unref(response);
    return accepted;
}

static uint64_t request(unsigned int pulse, unsigned int sample)
{
    return NIC_PHASES | ((uint64_t)pulse << 10) | ((uint64_t)sample << 2);
}

static bool slot(NIC *nic, unsigned int pulse, unsigned int sample)
{
    g_assert_true(nic_request(nic, request(pulse, sample)));
    g_assert_cmphex(nic_read(nic) & 3, ==, 0);
    qtest_clock_step(nic->qts, (pulse + sample) * 1000);
    g_assert_cmphex(nic_read(nic) & 2, ==, 2);
    return nic_read(nic) & 1;
}

static void reset_bus(NIC *nic)
{
    g_assert_false(slot(nic, 520, 65));
}

static void write_bit(NIC *nic, bool bit)
{
    g_assert_true(slot(nic, bit ? 6 : 80, bit ? 110 : 30));
}

static bool read_bit(NIC *nic)
{
    return slot(nic, 6, 13);
}

static void write_byte(NIC *nic, uint8_t value)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        write_bit(nic, (value >> bit) & 1);
    }
}

static uint8_t read_byte(NIC *nic)
{
    uint8_t value = 0;

    for (unsigned int bit = 0; bit < 8; bit++) {
        value |= read_bit(nic) << bit;
    }
    return value;
}

/* Separate bitwise oracles from the DS2505 production CRC helpers. */
static uint16_t crc16(uint16_t crc, uint8_t byte)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        bool carry = (crc ^ (byte >> bit)) & 1;

        crc >>= 1;
        if (carry) {
            crc ^= 0xa001;
        }
    }
    return crc;
}

static uint8_t crc8(const uint8_t *bytes, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        for (unsigned int bit = 0; bit < 8; bit++) {
            bool carry = (crc ^ (bytes[i] >> bit)) & 1;

            crc >>= 1;
            if (carry) {
                crc ^= 0x8c;
            }
        }
    }
    return crc;
}

static void search_rom(NIC *nic, uint8_t rom[8])
{
    memset(rom, 0, 8);
    reset_bus(nic);
    write_byte(nic, 0xf0);
    for (unsigned int bit = 0; bit < 64; bit++) {
        bool value = read_bit(nic);

        g_assert_cmpint(read_bit(nic), ==, !value);
        rom[bit / 8] |= value << (bit % 8);
        write_bit(nic, value);
    }
    g_assert_cmphex(rom[0], ==, 0x0b);
    g_assert_cmphex(crc8(rom, 8), ==, 0);
}

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -d unimp,guest_errors "
                       "-global sgi-router.nic-clock-hz=97500000 "
                       "-device sgi-router-test,id=router0,nic-serial=1 "
                       "-device sgi-router-test,id=router1,nic-serial=2 %s",
                       extra);

    /* This test only addresses the NIC; polls need not reselect its address. */
    for (unsigned int node = 0; node <= 2; node += 2) {
        NIC nic = { qts, node };

        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':%s,'property':'address',"
            "'value':%u}}", fixture_path(&nic), ROUTER_NIC);
    }
    return qts;
}

static void test_search(void)
{
    QTestState *qts = start("");
    NIC first = { qts, 0 }, other_port = { qts, 1 }, second = { qts, 2 };
    uint8_t a[8], b[8], c[8];

    search_rom(&first, a);
    search_rom(&other_port, b);
    search_rom(&second, c);
    g_assert_cmpmem(a, 8, b, 8);
    g_assert_cmpint(memcmp(a, c, 8), !=, 0);
    reset_bus(&first);
    write_byte(&first, 0xf0);
    g_assert_true(read_bit(&first)); /* Family 0x0b starts with one. */
    g_assert_false(read_bit(&first));
    write_bit(&first, false);
    for (unsigned int i = 0; i < 12; i++) {
        g_assert_true(read_bit(&first));
    }
    search_rom(&first, b);
    g_assert_cmpmem(a, 8, b, 8);
    qtest_quit(qts);
}

static void test_timing(void)
{
    QTestState *qts = start("");
    NIC nic = { qts, 0 };
    uint64_t value;

    g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
    g_assert_true(nic_request(&nic, request(520, 65) | 3));
    for (unsigned int i = 0; i < 10; i++) {
        g_assert_cmphex(nic_read(&nic), ==, NIC_PHASES);
    }
    g_assert_false(nic_request(&nic, request(6, 13)));
    qtest_clock_step(qts, 519999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 64999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 2);
    value = nic_read(&nic);
    g_assert_false(nic_request(&nic, request(6, 14)));
    g_assert_false(nic_request(&nic, request(6, 13) ^ (UINT64_C(1) << 27)));
    g_assert_false(nic_request(&nic, (NIC_RESET & ~UINT64_C(3)) |
                                  (UINT64_C(6) << 10) | (13 << 2)));
    g_assert_cmphex(nic_read(&nic), ==, value);
    write_byte(&nic, 0x33);
    g_assert_true(read_bit(&nic));
    g_assert_true(read_bit(&nic));
    /* Retain bit two (zero) beyond the DS2505's 15us guaranteed window. */
    g_assert_true(nic_request(&nic, request(6, 13)));
    qtest_clock_step(qts, 15000);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 3999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 2);
    g_assert_true(read_bit(&nic)); /* One consumed bit: bit three is one. */
    for (unsigned int pulse = 6; pulse <= 80; pulse += 74) {
        unsigned int sample = pulse == 6 ? 110 : 30;

        g_assert_true(nic_request(&nic, request(pulse, sample)));
        qtest_clock_step(qts, (pulse + sample) * 1000 - 1);
        g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(nic_read(&nic) & 3, ==, 3);
    }
    qtest_quit(qts);
}

static void test_absent(void)
{
    QTestState *qts = start("-global sgi-router.nic-present=off");
    NIC nic = { qts, 0 };

    g_assert_true(slot(&nic, 520, 65));
    g_assert_true(read_bit(&nic));
    g_assert_true(slot(&nic, 80, 30));
    qtest_quit(qts);
}

static uint8_t pattern(unsigned int address)
{
    return (address * 29 + (address >> 3) + 7) & 0xff;
}

static void check_crc(NIC *nic, uint16_t crc)
{
    uint16_t actual = read_byte(nic);

    actual |= (uint16_t)read_byte(nic) << 8;
    g_assert_cmphex(actual, ==, (uint16_t)~crc);
}

static uint16_t memory_header(NIC *nic, uint8_t command, uint16_t address)
{
    uint16_t crc = crc16(0, command);

    reset_bus(nic);
    write_byte(nic, 0xcc);
    write_byte(nic, command);
    write_byte(nic, address);
    write_byte(nic, address >> 8);
    crc = crc16(crc, address);
    return crc16(crc, address >> 8);
}

static void test_memory(void)
{
    g_autofree char *path = NULL;
    uint8_t image[DATA_SIZE];
    int fd = g_file_open_tmp("router-nic-data-XXXXXX", &path, NULL);
    QTestState *qts;
    NIC nic;
    uint16_t crc;

    g_assert_cmpint(fd, >=, 0);
    for (unsigned int i = 0; i < DATA_SIZE; i++) {
        image[i] = pattern(i);
    }
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    {
        g_autofree char *extra = g_strdup_printf(
            "-global sgi-router.nic-image=%s", path);

        qts = start(extra);
    }
    nic = (NIC) { qts, 0 };
    crc = memory_header(&nic, 0xf0, 29);
    for (unsigned int i = 29; i < DATA_SIZE; i++) {
        uint8_t byte = read_byte(&nic);

        g_assert_cmphex(byte, ==, image[i]);
        crc = crc16(crc, byte);
    }
    check_crc(&nic, crc);
    g_assert_cmphex(read_byte(&nic), ==, 0xff);
    crc = memory_header(&nic, 0xa5, 29);
    g_assert_cmphex(read_byte(&nic), ==, 0xff);
    check_crc(&nic, crc16(crc, 0xff));
    for (unsigned int page = 0; page < 2; page++) {
        unsigned int begin = page ? 32 : 29;
        unsigned int end = page ? 64 : 32;

        crc = 0;
        for (unsigned int i = begin; i < end; i++) {
            uint8_t byte = read_byte(&nic);

            g_assert_cmphex(byte, ==, image[i]);
            crc = crc16(crc, byte);
        }
        check_crc(&nic, crc);
        if (!page) {
            g_assert_cmphex(read_byte(&nic), ==, 0xff);
            check_crc(&nic, crc16(0, 0xff));
        }
    }
    crc = memory_header(&nic, 0xaa, 3);
    for (unsigned int i = 3; i < 8; i++) {
        g_assert_cmphex(read_byte(&nic), ==, 0xff);
        crc = crc16(crc, 0xff);
    }
    check_crc(&nic, crc);
    qtest_quit(qts);
    unlink(path);
}

static void test_reset(void)
{
    QTestState *qts = start("");
    NIC nic = { qts, 0 };
    uint8_t original[8], after[8];
    const int64_t elapsed[] = { 5000, 10000 };

    search_rom(&nic, original);
    for (unsigned int i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        g_assert_true(nic_request(&nic, request(6, 13)));
        qtest_clock_step(qts, elapsed[i]);
        qtest_system_reset(qts);
        g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
        search_rom(&nic, after);
        g_assert_cmpmem(original, 8, after, 8);
    }
    qtest_quit(qts);
}

static bool is_running(QTestState *qts)
{
    g_autoptr(QDict) response = qtest_qmp(qts, "{'execute':'query-status'}");

    return qdict_get_bool(qdict_get_qdict(response, "return"), "running");
}

static QTestState *migrate(NIC *nic, int64_t now)
{
    g_autofree char *directory = g_dir_make_tmp("router-nic-migrate-XXXXXX",
                                                NULL);
    g_autofree char *socket = g_build_filename(directory, "socket", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *to = start("-incoming defer");
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    /* Qtest's explicit clock is a harness input, not guest migration state. */
    qtest_clock_set(to, now);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(nic->qts,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(nic->qts,
                                             "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                           "status");

        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    /*
     * The source completes once its state is sent, which can be before the
     * destination has loaded it; a read in between returns reset values.
     */
    while (!is_running(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
    return to;
}

static void test_migration(void)
{
    const int64_t elapsed[] = { 5000, 10000 };

    for (unsigned int i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        QTestState *from = start("");
        NIC source = { from, 0 }, target;
        uint8_t rom[8];
        int64_t now;

        search_rom(&source, rom);
        reset_bus(&source);
        write_byte(&source, 0xf0);
        g_assert_true(read_bit(&source));
        /* Migrate the complement slot before/after its bus advancement. */
        g_assert_true(nic_request(&source, request(6, 13)));
        now = qtest_clock_step(from, elapsed[i]);

        target = (NIC) { migrate(&source, now), 0 };
        g_assert_cmphex(nic_read(&target), ==, nic_read(&source));
        qtest_clock_step(target.qts, 19000 - elapsed[i] - 1);
        g_assert_cmphex(nic_read(&target) & 3, ==, 0);
        qtest_clock_step(target.qts, 1);
        g_assert_cmphex(nic_read(&target) & 3, ==, 2);
        write_bit(&target, true);
        for (unsigned int bit = 1; bit < 64; bit++) {
            bool expected = (rom[bit / 8] >> (bit % 8)) & 1;

            g_assert_cmpint(read_bit(&target), ==, expected);
            g_assert_cmpint(read_bit(&target), ==, !expected);
            write_bit(&target, expected);
        }
        qtest_quit(from);
        qtest_quit(target.qts);
    }
}

static void test_migration_crc(void)
{
    QTestState *from = start("");
    NIC source = { from, 0 }, target;
    uint16_t crc = memory_header(&source, 0xf0, DATA_SIZE - 3);
    uint16_t expected;
    uint8_t actual = 0;
    int64_t now;

    for (unsigned int i = 0; i < 3; i++) {
        g_assert_cmphex(read_byte(&source), ==, 0xff);
        crc = crc16(crc, 0xff);
    }
    expected = ~crc;
    g_assert_cmphex(read_byte(&source), ==, (uint8_t)expected);
    /* Preserve a partial CRC byte, including a latched awaiting-sample bit. */
    for (unsigned int bit = 0; bit < 3; bit++) {
        actual |= read_bit(&source) << bit;
    }
    g_assert_true(nic_request(&source, request(6, 13)));
    now = qtest_clock_step(from, 10000);

    target = (NIC) { migrate(&source, now), 0 };
    qtest_clock_step(target.qts, 9000);
    actual |= (nic_read(&target) & 1) << 3;
    for (unsigned int bit = 4; bit < 8; bit++) {
        actual |= read_bit(&target) << bit;
    }
    g_assert_cmphex(actual, ==, expected >> 8);
    g_assert_cmphex(read_byte(&target), ==, 0xff);
    qtest_quit(from);
    qtest_quit(target.qts);
}

static void test_oversize_image(void)
{
    g_autofree char *path = NULL;
    g_autofree char *output = NULL;
    g_autofree char *property = NULL;
    int fd = g_file_open_tmp("router-nic-oversize-XXXXXX", &path, NULL);
    int status;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-display", "none",
        "-S", "-global", NULL, "-device", "sgi-router-test", NULL,
    };

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, DATA_SIZE + 1), ==, 0);
    close(fd);
    property = g_strdup_printf("sgi-router.nic-image=%s", path);
    args[7] = property;
    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &output, &status, NULL));
    unlink(path);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(output, "exceeds 2048 bytes"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/router-nic/search", test_search);
    qtest_add_func("sn0/router-nic/timing", test_timing);
    qtest_add_func("sn0/router-nic/absent", test_absent);
    qtest_add_func("sn0/router-nic/memory", test_memory);
    qtest_add_func("sn0/router-nic/reset", test_reset);
    qtest_add_func("sn0/router-nic/migration", test_migration);
    qtest_add_func("sn0/router-nic/migration-crc", test_migration_crc);
    qtest_add_func("sn0/router-nic/oversize-image", test_oversize_image);
    return g_test_run();
}
