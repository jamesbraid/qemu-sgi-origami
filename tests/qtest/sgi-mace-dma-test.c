/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic MACE PCI bus-master tests. No guest device uses this path. */

#include "qemu/osdep.h"
#include <gio/gio.h>
#include "libqtest.h"
#include "memtx.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"

#define NATIVE_BASE 0x40000000ULL
#define FIT_64M (64ULL * 1024 * 1024)
#define FIT_1G (1ULL * 1024 * 1024 * 1024)
#define CARD_PATH "/machine/peripheral/card"
#define MACE_PATH "/machine/peripheral/mace"
#define MACE_BASE 0x1f000000ULL
#define MACE_PCI_CFGADDR (MACE_BASE + 0x080cf8)
#define MACE_PCI_CFGDATA (MACE_BASE + 0x080cfc)

typedef struct MACEDMATest {
    QTestState *qts;
    char *dir;
    char *path;
    int fd;
    uint64_t size;
} MACEDMATest;

static void card_set(QTestState *qts, const char *property, uint64_t value)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{'path':'" CARD_PATH "',"
        "'property':%s,'value':%" PRIu64 "}}", property, value);

    if (qdict_haskey(reply, "error")) {
        g_error("qom-set %s: %s", property,
                qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
    g_assert_nonnull(qdict_get(reply, "return"));
}

static uint64_t card_get(QTestState *qts, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" CARD_PATH "',"
        "'property':%s}}", property);

    g_assert_nonnull(qdict_get(reply, "return"));
    return qdict_get_uint(reply, "return");
}

static void card_master_set(QTestState *qts, bool enabled)
{
    g_autoptr(QDict) reply;

    if (enabled) {
        reply = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'" CARD_PATH "',"
            "'property':'test-master-enable','value':true}}");
    } else {
        reply = qtest_qmp(qts,
            "{'execute':'qom-set','arguments':{'path':'" CARD_PATH "',"
            "'property':'test-master-enable','value':false}}");
    }
    if (qdict_haskey(reply, "error")) {
        g_error("qom-set test-master-enable: %s",
                qdict_get_str(qdict_get_qdict(reply, "error"), "desc"));
    }
    g_assert_nonnull(qdict_get(reply, "return"));
}

static bool card_master_get(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'" CARD_PATH "',"
        "'property':'test-master-enable'}}");

    g_assert_nonnull(qdict_get(reply, "return"));
    return qdict_get_bool(reply, "return");
}

static uint32_t card_fill(QTestState *qts, uint64_t address,
                          unsigned length, uint32_t seed)
{
    card_set(qts, "test-address", address);
    card_set(qts, "test-length", length);
    card_set(qts, "test-seed", seed);
    card_set(qts, "test-command", 1);
    return card_get(qts, "test-result");
}

static uint32_t card_copy(QTestState *qts, uint64_t source,
                          uint64_t destination, unsigned length)
{
    card_set(qts, "test-source", source);
    card_set(qts, "test-address", destination);
    card_set(qts, "test-length", length);
    card_set(qts, "test-command", 2);
    return card_get(qts, "test-result");
}

static void file_read_exact(int fd, void *buf, size_t length, off_t offset)
{
    g_assert_cmpint(pread(fd, buf, length, offset), ==, length);
}

static void file_write_exact(int fd, const void *buf, size_t length,
                             off_t offset)
{
    g_assert_cmpint(pwrite(fd, buf, length, offset), ==, length);
}

static MACEDMATest mace_dma_start_mode(uint64_t size, bool link_ram,
                                       bool aic_diagnostic, bool word_swap)
{
    MACEDMATest t = { .fd = -1, .size = size };
    g_autofree char *cwd = g_get_current_dir();
    g_autoptr(QDict) reply = NULL;

    /* The qtest build directory is on the configured project scratch volume. */
    t.dir = g_build_filename(cwd, ".mace-dma-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(t.dir));
    t.path = g_build_filename(t.dir, "ram", NULL);
    t.fd = g_open(t.path, O_RDWR | O_CREAT | O_EXCL, 0600);
    g_assert_cmpint(t.fd, >=, 0);
    g_assert_cmpint(ftruncate(t.fd, size), ==, 0);

    t.qts = qtest_initf(MEMTX_TEST_DEVICE
        "-M none -m 1G -cpu R10000 -accel qtest -S "
        "-object memory-backend-file,id=dma,size=%" PRIu64 ","
        "mem-path=%s,share=on -device sgi-mace,id=mace%s%s%s "
        "-device sgi-bridge-dma-test,id=card,bus=mace.0,addr=3",
        size, t.path, link_ram ? ",ram=/objects/dma/dma[0]" : "",
        aic_diagnostic ? ",aic-pci-diagnostic=on" : "",
        word_swap ? ",pci-native-word-swap=on" : "");
    qtest_sysbus_mmio_map(t.qts, MACE_PATH, 0, MACE_BASE);

    reply = qtest_qmp(t.qts,
        "{'execute':'qom-list','arguments':{'path':'/objects/dma/dma[0]'}}");
    g_assert_nonnull(qdict_get_qlist(reply, "return"));
    if (link_ram) {
        g_autoptr(QDict) linked = qtest_qmp(t.qts,
            "{'execute':'qom-get','arguments':{'path':'" MACE_PATH "',"
            "'property':'ram'}}");

        g_assert_cmpstr(qdict_get_str(linked, "return"), ==,
                        "/objects/dma/dma[0]");
    }
    return t;
}

static MACEDMATest mace_dma_start(uint64_t size, bool link_ram)
{
    return mace_dma_start_mode(size, link_ram, false, false);
}

static void mace_dma_stop(MACEDMATest *t)
{
    qtest_quit(t->qts);
    g_assert_cmpint(close(t->fd), ==, 0);
    g_assert_cmpint(g_unlink(t->path), ==, 0);
    g_assert_cmpint(g_rmdir(t->dir), ==, 0);
    g_free(t->path);
    g_free(t->dir);
}

static void test_aic_diagnostic_refuses_both_ram_windows(void)
{
    MACEDMATest t = mace_dma_start_mode(FIT_64M, true, true, false);
    uint8_t guard[16], actual[16];

    memset(guard, 0x69, sizeof(guard));
    file_write_exact(t.fd, guard, sizeof(guard), 0x100);
    card_master_set(t.qts, true);
    g_assert_true(card_master_get(t.qts));
    for (uint64_t base = 0; base <= NATIVE_BASE; base += NATIVE_BASE) {
        g_assert_cmpuint(card_fill(t.qts, base + 0x104, 4, 0x20), ==,
                         MEMTX_ERROR);
        g_assert_cmpuint(card_copy(t.qts, base + 0x100,
                                   base + 0x108, 4), ==, MEMTX_ERROR);
    }
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));
    mace_dma_stop(&t);
}

static void check_no_ram_link(bool word_swap)
{
    MACEDMATest t = mace_dma_start_mode(FIT_64M, false, false, word_swap);
    uint8_t guard[16], actual[16];

    memset(guard, 0x69, sizeof(guard));
    file_write_exact(t.fd, guard, sizeof(guard), 0x100);
    qtest_memset(t.qts, 0x100, 0x3b, sizeof(guard));
    card_master_set(t.qts, true);
    g_assert_true(card_master_get(t.qts));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x104, 4, 0x20), !=, 0);
    g_assert_cmpuint(card_copy(t.qts, NATIVE_BASE + 0x100,
                               NATIVE_BASE + 0x108, 4), !=, 0);
    g_assert_cmpuint(card_fill(t.qts, 0x104, 4, 0x20), !=, 0);
    g_assert_cmpuint(card_copy(t.qts, 0x100,
                               NATIVE_BASE + 0x108, 4), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));
    for (unsigned i = 0; i < sizeof(guard); i++) {
        g_assert_cmphex(qtest_readb(t.qts, 0x100 + i), ==, 0x3b);
    }
    mace_dma_stop(&t);
}

static void test_no_ram_link(void)
{
    check_no_ram_link(false);
}

static void test_o2_no_ram_link(void)
{
    check_no_ram_link(true);
}

static void test_master_non_qtest_refused(void)
{
    const char *argv[] = {
        qtest_qemu_binary(NULL), "-M", "malta", "-bios", NULL,
        "-accel", "tcg", "-S", "-nodefaults", "-display", "none",
        "-qmp", "stdio", "-device", "sgi-bridge-dma-test,id=card",
        NULL,
    };
    const char *commands =
        "{\"execute\":\"qmp_capabilities\",\"id\":\"cap\"}\n"
        "{\"execute\":\"qom-set\",\"arguments\":{\"path\":\"" CARD_PATH
        "\",\"property\":\"test-master-enable\",\"value\":true},"
        "\"id\":\"set\"}\n"
        "{\"execute\":\"qom-get\",\"arguments\":{\"path\":\"" CARD_PATH
        "\",\"property\":\"test-master-enable\"},\"id\":\"state\"}\n"
        "{\"execute\":\"quit\",\"id\":\"quit\"}\n";
    g_autoptr(GError) error = NULL;
    g_autoptr(GSubprocess) proc = NULL;
    g_autofree char *cwd = g_get_current_dir();
    g_autofree char *rom_path = g_build_filename(cwd,
                                                 ".mace-blank-rom-XXXXXX",
                                                 NULL);
    g_autofree char *output = NULL;
    g_autofree char *stderr_output = NULL;
    g_auto(GStrv) lines = NULL;
    bool refused = false, unchanged = false;
    const uint8_t blank = 0;
    int rom_fd = g_mkstemp(rom_path);

    g_assert_cmpint(rom_fd, >=, 0);
    g_assert_cmpint(write(rom_fd, &blank, sizeof(blank)), ==, sizeof(blank));
    g_assert_cmpint(close(rom_fd), ==, 0);
    argv[4] = rom_path;
    proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error);
    g_assert_true(g_subprocess_communicate_utf8(proc, commands, NULL,
                                                &output, &stderr_output,
                                                &error));
    g_assert_no_error(error);
    if (!g_subprocess_get_successful(proc)) {
        g_error("non-qtest QEMU exited %d: %s",
                g_subprocess_get_exit_status(proc), stderr_output);
    }
    lines = g_strsplit(output, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        QObject *obj;
        QDict *reply;
        const char *id;

        if (!*lines[i]) {
            continue;
        }
        obj = qobject_from_json(lines[i], &error_abort);
        reply = qobject_to(QDict, obj);
        g_assert_nonnull(reply);
        id = qdict_get_try_str(reply, "id");
        if (g_strcmp0(id, "set") == 0) {
            QDict *failure = qdict_get_qdict(reply, "error");

            g_assert_nonnull(failure);
            g_assert_nonnull(strstr(qdict_get_str(failure, "desc"),
                                    "requires qtest"));
            refused = true;
        } else if (g_strcmp0(id, "state") == 0) {
            g_assert_false(qdict_get_bool(reply, "return"));
            unchanged = true;
        }
        qobject_unref(obj);
    }
    g_assert_true(refused);
    g_assert_true(unchanged);
    g_assert_cmpint(g_unlink(rom_path), ==, 0);
}

static void test_master_gate(void)
{
    MACEDMATest t = mace_dma_start(FIT_64M, true);
    uint8_t guard[8], actual[8];

    memset(guard, 0x76, sizeof(guard));
    file_write_exact(t.fd, guard, sizeof(guard), 0x100);
    g_assert_false(card_master_get(t.qts));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x102, 2, 0x30), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmpmem(actual, sizeof(actual), guard, sizeof(guard));

    qtest_writel(t.qts, MACE_PCI_CFGADDR, (3u << 11) | 0x04);
    g_assert_cmpuint(memtx_writeb(t.qts, MACE_PCI_CFGDATA + 3, 0x04), ==,
                     MEMTX_ERROR);
    g_assert_false(card_master_get(t.qts));

    card_master_set(t.qts, true);
    g_assert_true(card_master_get(t.qts));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x102, 2, 0x30), ==, 0);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmphex(actual[2], ==, 0x30);
    g_assert_cmphex(actual[3], ==, 0x31);

    g_assert_cmpuint(memtx_writeb(t.qts, MACE_PCI_CFGDATA + 3, 0x04), ==,
                     MEMTX_ERROR);
    g_assert_true(card_master_get(t.qts));
    card_master_set(t.qts, false);
    g_assert_false(card_master_get(t.qts));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x102, 2, 0x40), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmphex(actual[2], ==, 0x30);
    g_assert_cmphex(actual[3], ==, 0x31);

    card_master_set(t.qts, true);
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x102, 2, 0x50), ==, 0);
    qtest_system_reset(t.qts);
    g_assert_false(card_master_get(t.qts));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x102, 2, 0x60), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmphex(actual[2], ==, 0x50);
    g_assert_cmphex(actual[3], ==, 0x51);
    mace_dma_stop(&t);
}

static void check_native_bytes(MACEDMATest *t, uint64_t offset,
                               unsigned length)
{
    uint8_t source[16], before[16], actual[16];
    uint64_t read_data = 0;

    g_assert_cmpuint(length, <=, 8);
    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x10 + i * 17;
    }
    memset(before, 0xd3, sizeof(before));
    file_write_exact(t->fd, source, sizeof(source), offset - 4);
    file_write_exact(t->fd, before, sizeof(before), offset + 32);
    qtest_memset(t->qts, offset - 4, 0x5e, sizeof(source));
    qtest_memset(t->qts, offset + 32, 0x5e, sizeof(before));

    g_assert_cmpuint(card_copy(t->qts, NATIVE_BASE + offset,
                               NATIVE_BASE + offset + 36, length), ==, 0);
    for (unsigned i = 0; i < length; i++) {
        read_data |= (uint64_t)source[4 + i] << (8 * i);
    }
    for (unsigned i = 0; i < length; i++) {
        g_assert_cmphex((card_get(t->qts, "test-read-data") >> (8 * i)) &
                        0xff, ==, (read_data >> (8 * i)) & 0xff);
    }
    file_read_exact(t->fd, actual, sizeof(actual), offset + 32);
    memcpy(before + 4, source + 4, length);
    g_assert_cmpmem(actual, sizeof(actual), before, sizeof(before));

    g_assert_cmpuint(card_fill(t->qts, NATIVE_BASE + offset,
                               length, 0x83), ==, 0);
    file_read_exact(t->fd, actual, sizeof(actual), offset - 4);
    for (unsigned i = 0; i < length; i++) {
        source[4 + i] = 0x83 + i;
    }
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    for (unsigned i = 0; i < sizeof(source); i++) {
        g_assert_cmphex(qtest_readb(t->qts, offset - 4 + i), ==, 0x5e);
    }
    for (unsigned i = 0; i < sizeof(before); i++) {
        g_assert_cmphex(qtest_readb(t->qts, offset + 32 + i), ==, 0x5e);
    }
}

static void test_native_fit(uint64_t size)
{
    MACEDMATest t = mace_dma_start(size, true);
    static const uint64_t offsets[] = { 0x1e, 0x3d, 0x7d, 0xfe };
    uint8_t guard[16], low_guard[16], actual[16];
    struct stat st;

    card_master_set(t.qts, true);
    g_assert_false(qtest_qom_get_bool(t.qts, MACE_PATH,
                                       "pci-native-word-swap"));

    for (unsigned i = 0; i < G_N_ELEMENTS(offsets); i++) {
        for (unsigned length = 1; length <= 8; length *= 2) {
            check_native_bytes(&t, offsets[i], length);
        }
    }
    if (size == FIT_1G) {
        check_native_bytes(&t, 0x0ffffffe, 8);
        check_native_bytes(&t, 0x10000000, 8);
        check_native_bytes(&t, 0x1000007e, 8);
    }

    memset(guard, 0xb6, sizeof(guard));
    memset(low_guard, 0x94, sizeof(low_guard));
    file_write_exact(t.fd, guard, sizeof(guard), size - sizeof(guard));
    file_write_exact(t.fd, low_guard, sizeof(low_guard), 0x100);
    qtest_memset(t.qts, 0x100, 0x59, sizeof(guard));
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + size - 1, 1, 0x42), ==, 0);
    file_read_exact(t.fd, actual, sizeof(actual), size - sizeof(actual));
    g_assert_cmphex(actual[sizeof(actual) - 1], ==, 0x42);
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + size, 1, 0x42), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), size - sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual) - 1, guard,
                    sizeof(guard) - 1);
    g_assert_cmphex(actual[sizeof(actual) - 1], ==, 0x42);
    g_assert_cmpuint(card_copy(t.qts, NATIVE_BASE + size,
                               NATIVE_BASE + 0x100, 1), !=, 0);
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + size - 2, 8, 0x61), !=, 0);
    g_assert_cmpuint(card_fill(t.qts, 0x100, 4, 0x41), !=, 0);
    g_assert_cmpuint(card_copy(t.qts, 0x100,
                               NATIVE_BASE + 0x100, 4), !=, 0);
    g_assert_cmpuint(card_fill(t.qts, 0x80000100, 4, 0x41), !=, 0);
    g_assert_cmpuint(card_copy(t.qts, 0x80000100,
                               NATIVE_BASE + 0x100, 4), !=, 0);
    file_read_exact(t.fd, actual, sizeof(actual), size - sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual) - 2, guard,
                    sizeof(guard) - 2);
    file_read_exact(t.fd, actual, sizeof(actual), 0x100);
    g_assert_cmpmem(actual, sizeof(actual), low_guard, sizeof(low_guard));
    for (unsigned i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(qtest_readb(t.qts, 0x100 + i), ==, 0x59);
    }
    g_assert_cmpint(fstat(t.fd, &st), ==, 0);
    g_assert_cmpuint(st.st_size, ==, size);
    mace_dma_stop(&t);
}

static void test_o2_flat_views(uint64_t size)
{
    MACEDMATest t = mace_dma_start_mode(size, true, false, true);
    static const unsigned lengths[] = { 1, 2, 3, 4, 5, 6, 7, 8, 28 };
    uint8_t source[64], expected[64], actual[64], low_guard[16];

    card_master_set(t.qts, true);
    for (unsigned n = 0; n < G_N_ELEMENTS(lengths); n++) {
        unsigned length = lengths[n];

        for (unsigned i = 0; i < sizeof(source); i++) {
            source[i] = 0x40 + i;
        }
        memset(expected, 0xb6, sizeof(expected));
        file_write_exact(t.fd, source, sizeof(source), 0x100);
        file_write_exact(t.fd, expected, sizeof(expected), 0x2000);
        qtest_memset(t.qts, 0x100, 0x5e, sizeof(source));
        qtest_memset(t.qts, 0x2000, 0x5e, sizeof(expected));
        g_assert_cmpuint(card_copy(t.qts, NATIVE_BASE + 0x104, 0x2008,
                                   length), ==, MEMTX_OK);
        for (unsigned i = 0; i < length; i++) {
            expected[8 + i] = source[(4 + i) ^ 3];
        }
        file_read_exact(t.fd, actual, sizeof(actual), 0x2000);
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

        memset(expected, 0xd7, sizeof(expected));
        file_write_exact(t.fd, expected, sizeof(expected), 0x100);
        g_assert_cmpuint(card_fill(t.qts, 0x2008, length, 0x80), ==, MEMTX_OK);
        g_assert_cmpuint(card_copy(t.qts, 0x2008, NATIVE_BASE + 0x104,
                                   length), ==, MEMTX_OK);
        for (unsigned i = 0; i < length; i++) {
            expected[(4 + i) ^ 3] = 0x80 + i;
        }
        file_read_exact(t.fd, actual, sizeof(actual), 0x100);
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        qtest_memread(t.qts, 0x100, actual, sizeof(actual));
        for (unsigned i = 0; i < sizeof(actual); i++) {
            g_assert_cmphex(actual[i], ==, 0x5e);
        }
        qtest_memread(t.qts, 0x2000, actual, sizeof(actual));
        for (unsigned i = 0; i < sizeof(actual); i++) {
            g_assert_cmphex(actual[i], ==, 0x5e);
        }
    }

    memset(low_guard, 0x94, sizeof(low_guard));
    file_write_exact(t.fd, low_guard, sizeof(low_guard), 0);
    for (unsigned upper = 0; upper < 2; upper++) {
        uint64_t window = upper ? NATIVE_BASE : 0;

        memset(expected, 0xb6, sizeof(expected));
        file_write_exact(t.fd, expected, sizeof(expected), size - 64);
        g_assert_cmpuint(card_fill(t.qts, window + size - 2, 8, 0x60), !=,
                         MEMTX_OK);
        expected[upper ? 61 : 62] = 0x60;
        expected[upper ? 60 : 63] = 0x61;
        file_read_exact(t.fd, actual, sizeof(actual), size - 64);
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
        g_assert_cmpuint(card_copy(t.qts, window + size, 0x100, 1), !=,
                         MEMTX_OK);
        g_assert_cmpuint(card_fill(t.qts, window + size, 1, 0x80), !=,
                         MEMTX_OK);
    }
    file_read_exact(t.fd, actual, sizeof(low_guard), 0);
    g_assert_cmpmem(actual, sizeof(low_guard), low_guard, sizeof(low_guard));
    card_master_set(t.qts, false);
    g_assert_cmpuint(card_fill(t.qts, NATIVE_BASE + 0x104, 4, 0x80), !=,
                     MEMTX_OK);
    g_assert_cmpuint(card_fill(t.qts, 0x104, 4, 0x80), !=, MEMTX_OK);
    mace_dma_stop(&t);
}

static void test_o2_flat_64m(void)
{
    test_o2_flat_views(FIT_64M);
}

static void test_o2_flat_128m(void)
{
    test_o2_flat_views(2 * FIT_64M);
}

static void test_native_64m(void)
{
    test_native_fit(FIT_64M);
}

static void test_native_1g(void)
{
    test_native_fit(FIT_1G);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace-dma/no-ram-link", test_no_ram_link);
    qtest_add_func("/sgi/mace-dma/o2-no-ram-link", test_o2_no_ram_link);
    qtest_add_func("/sgi/mace-dma/master-non-qtest-refused",
                   test_master_non_qtest_refused);
    qtest_add_func("/sgi/mace-dma/master-gate", test_master_gate);
    qtest_add_func("/sgi/mace-dma/o2-flat-64m", test_o2_flat_64m);
    qtest_add_func("/sgi/mace-dma/o2-flat-128m", test_o2_flat_128m);
    qtest_add_func("/sgi/mace-dma/native-64m", test_native_64m);
    qtest_add_func("/sgi/mace-dma/native-1g", test_native_1g);
    qtest_add_func("/sgi/mace-dma/aic-diagnostic-refuses-ram",
                   test_aic_diagnostic_refuses_both_ram_windows);
    return g_test_run();
}
