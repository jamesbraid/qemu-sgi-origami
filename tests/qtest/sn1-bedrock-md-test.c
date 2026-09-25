/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-test.h"

#define BASE UINT64_C(0x78000000)
#define MD(_x) (BASE + (_x))
#define MD_PATH "/machine/peripheral/md"
#define DIRECTORY_BASE UINT64_C(0x100000000)
#define DIRECTORY(_x) (DIRECTORY_BASE + (_x))
#define MEMORY_BASE UINT64_C(0x400000000)
#define MEMORY(_x) (MEMORY_BASE + (_x))
#define FETCHOP_BASE UINT64_C(0x800000000)
#define FETCHOP(_x) (FETCHOP_BASE + (_x))
/* The Origin 300 machine carrier of the memory-special space, node zero. */
#define FETCHOP_MACHINE_BASE UINT64_C(0x30000000000)
#define FETCHOP_DIAGNOSTIC "sgi-bedrock-md: fetch-and-op "
#define MEMORY_ERROR 0x60010
#define MEMORY_ERROR_CLEAR 0x60018
#define ECC_CONFIG 0x60000
#define MEMORY_ERROR_READ_CE_VALID (UINT64_C(1) << 52)
#define MEMORY_ERROR_READ_CE_OVERRUN (UINT64_C(1) << 53)
#define MEMORY_ERROR_READ_UCE_VALID (UINT64_C(1) << 60)
#define MEMORY_ERROR_READ_UCE_OVERRUN (UINT64_C(1) << 61)
#define MEMORY_ERROR_SYNDROME(_s) ((uint64_t)(_s) << 40)
#define PI_PENDING_MODIFY 0x90
#define PI_PENDING_1 0xa0
#define PI_ERROR_SET 0x3f8
#define PI_ERROR_PENDING 0x400
#define PI_ERROR_MASK_A 0x408
#define PI_ERROR_MASK_B 0x410
#define PI1_OFFSET 0x200000
#define MEMORY_CORRECTABLE (UINT64_C(1) << 61)
#define MEMORY_UNCORRECTABLE (UINT64_C(1) << 24)
/* The parent's hardware-irq lines: interface, processor, then the line. */
#define ERROR_LINE(_pi, _cpu) ((_pi) * 10 + (_cpu) * 5 + 4)
/* The Origin 300's node-named Bedrock register window, crosstalk space. */
#define BEDROCK_NODE_REGISTERS UINT64_C(0x20001800000)
#define DIRECTORY_CONFIG 0x40000
#define DIRECTORY_ERROR 0x40010
#define DIRECTORY_ERROR_CLEAR 0x40018
#define DIRECTORY_ERROR_UCE_VALID (UINT64_C(1) << 63)
#define DIRECTORY_ERROR_CE_VALID (UINT64_C(1) << 61)
#define DIRECTORY_ERROR_UCE_OVERRUN (UINT64_C(1) << 59)
#define DIRECTORY_ERROR_CE_OVERRUN (UINT64_C(1) << 57)
#define DIRECTORY_ERROR_SYNDROME(_s) ((uint64_t)(_s) << 40)
#define DIRECTORY_HSPEC UINT64_C(0x1c0000000)
#define DIRECTORY_ERROR_ADDRESS(_offset) \
    ((DIRECTORY_HSPEC + (_offset)) & (((UINT64_C(1) << 30) - 1) << 3))
#define MEMORY_BANK_SIZE UINT64_C(0x40000000)
#define DIRECTORY_BANK_SIZE UINT64_C(0x08000000)
#define BEDROCK_BASE UINT64_C(0x70000000)
#define BEDROCK_MD(_x) (BEDROCK_BASE + 0x780000 + (_x))
#define DIRECTORY_LINE_01A00000 0x0340000
#define DIRECTORY_OTHER_LINE 0x0123450
#define DIRECTORY_LAST_512M 0x3fffff0
#define DIRECTORY_FIRST_ABSENT_512M 0x4000000
#define DIRECTORY_LAST_256M 0x1fffff0
#define DIRECTORY_FIRST_ABSENT_256M 0x2000000
#define DIRECTORY_COMPAT_ENTRIES 74
#define PROTECTION_FIRST 0x408
#define PROTECTION_INTERLEAVE_3 0x438
#define PROTECTION_NEXT_PAGE 0xc08
#define PROTECTION_SAMPLE_STRIDE 0x800000
#define PROTECTION_FITTED_192M 0x01800408
#define PROTECTION_FITTED_448M 0x03800408
#define PROTECTION_BANK1_HIGH 0xf800408
#define PROTECTION_LAST 0x3ffffc38
#define PROTECTION_FIRST_ABSENT 0x40000408
#define PROTECTION_MACHINE_BASE UINT64_C(0x101c0000000)
#define HUB_LOCAL_REGISTERS UINT64_C(0x10010000000)
#define SELFTEST_MACHINE(_x) (HUB_LOCAL_REGISTERS + 0x780000 + (_x))
#define PROTECTION_MACHINE_BANK2 \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x10000408))
#define PROTECTION_MACHINE_BANK2_ALIAS \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x12000408))
#define PROTECTION_MACHINE_BANK3 \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x18000408))
#define PROTECTION_MACHINE_BANK3_ALIAS \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x1a000408))
#define PROTECTION_MACHINE_BANK0 \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x00000408))
#define DIRECTORY_MACHINE_BANK2_FIRST_ABSENT \
    (PROTECTION_MACHINE_BASE + UINT64_C(0x12000000))
#define NODE_MEMORY_512M \
    "-object memory-backend-ram,id=node-memory,size=512M "

/*
 * The Origin 300 machine address of the five-eighths ECC-byte carrier for
 * node zero. The machine does not map it yet, so these tests place the
 * device's own region there and use the datasheet's addresses verbatim.
 */
#define ECC_BASE UINT64_C(0x10140000000)
#define ECC(_x) (ECC_BASE + (_x))
#define ECC_SCRUB_BASE UINT64_C(0x10100000000)
#define ECC_SCRUB(_x) (ECC_SCRUB_BASE + (_x))
#define ECC_FIRST_ABSENT_BYTE_512M 0x4000007
#define ECC_ADMITTED_HALFWORD_LOW 0x4000006
#define ECC_ADMITTED_HALFWORD_HIGH 0x8000006

/*
 * The three diagnostics the reviewed contract spells out character for
 * character. Matching them is what separates this model from an empty
 * mapping, a full-aperture sink and an over-wide handler.
 */
#define ECC_WRITE_UNALIGNED \
    "sgi-bedrock-md: ECC backdoor write offset 0x00000007 size 2 " \
    "rejected: unaligned halfword"
#define ECC_WRITE_WIDTH \
    "sgi-bedrock-md: ECC backdoor write offset 0x00000004 size 4 " \
    "rejected: unsupported width"
#define ECC_WRITE_ABSENT \
    "sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 1 " \
    "rejected: outside fitted memory"

/* Every diagnostic carries this, so counting it counts all of them. */
#define ECC_DIAGNOSTIC "ECC backdoor "

#define ECC_LOG_OPTIONS "-d guest_errors -D "
#define MEMORY_LOG_OPTIONS "-d guest_errors -D "
#define MEMORY_DIAGNOSTIC "sgi-bedrock-md: memory "
#define SELFTEST_DIAGNOSTIC "sgi-bedrock-md: unsupported self-test command"

#define SELFTEST_CONTROL 0x20030
#define SELFTEST_DATA 0x20038
#define SELFTEST_ERROR_ADDRESS 0x20040
#define SELFTEST_STATUS 0x20048
#define SELFTEST_DIRECTORY_ERROR 0x40058
#define SELFTEST_MEMORY_ERROR0 0x60020
#define SELFTEST_INTERVAL_NS 1000
#define SELFTEST_DONE 2
#define SELFTEST_PASSED 1
#define SELFTEST_MEMORY_ERROR (UINT64_C(1) << 48)

/*
 * Bits 58:56 are write-only in the public definition. Reading them back,
 * like the zero cold-reset value of the whole register, is model policy.
 */
#define MEMORY_CONFIG_MASK UINT64_C(0x373ff003fbfbfbfb)
#define REPLY_QUEUE UINT64_C(0x001818180c0c0c00)
#define REQUEST_QUEUE UINT64_C(0x0000000004040400)

/* Every register with retained storage and its writable mask. */
static const struct {
    uint64_t offset;
    uint64_t mask;
} storage_registers[] = {
    { 0x10, 0xff },
    { 0x18, UINT64_C(0x100ff03ff) },
    { 0x10000, UINT64_C(0x8fffffffffff7ff3) },
    { 0x10008, UINT64_C(0x8fffffffffff7ff3) },
    { 0x10010, UINT64_C(0x1ffffffffff) },
    { 0x10018, UINT64_C(0x1ffffffffff) },
    { 0x20008, UINT64_C(0xffffffffffff) },
    { 0x20038, 0xff }, { 0x20060, 3 },
    { 0x40040, UINT64_C(0x8000003f00007fff) },
    { 0x40048, UINT64_C(0xc000003f00007fff) },
    { 0x40060, 3 }, { 0x60000, 1 }, { 0x60040, 1 },
};

static const uint64_t protection_bank_bounds[8][2] = {
    { 0x00000408, 0x07800408 },
    { 0x08000408, 0x0f800408 },
    { 0x10000408, 0x17800408 },
    { 0x18000408, 0x1f800408 },
    { 0x20000408, 0x27800408 },
    { 0x28000408, 0x2f800408 },
    { 0x30000408, 0x37800408 },
    { 0x38000408, 0x3f800408 },
};

/*
 * Captured error results, migration candidates and the LED banks read zero
 * while the model generates no event. A guest write is dropped, so no valid
 * or error state can come from guest input.
 */
static const uint64_t policy_zero_registers[] = {
    0x20, 0x28, 0x20010, 0x20018, 0x20020, 0x20028, 0x20040,
    0x40030, 0x40038, 0x40058, 0x60020, 0x60028, 0x60030, 0x60038,
};

static QTestState *start_with_memory(const char *size, const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-object memory-backend-ram,"
                                "id=node-memory,size=%s "
                                "-device sgi-bedrock-md,id=md,"
                                "node-memory=/objects/node-memory/"
                                "node-memory[0] -S %s", size, extra);

    qtest_sysbus_mmio_map(s, MD_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 1, DIRECTORY_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 2, ECC_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 3, MEMORY_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 4, FETCHOP_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 5, ECC_SCRUB_BASE);
    return s;
}

static QTestState *start(const char *extra)
{
    return start_with_memory("512M", extra);
}

static QTestState *start_with_installed_memory(const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                NODE_MEMORY_512M
                                "-device sgi-bedrock-md,id=md,"
                                "node-memory=/objects/node-memory/"
                                "node-memory[0],installed-bank-mask=12,"
                                "installed-bank2-mib=256,"
                                "installed-bank3-mib=256 -S %s", extra);

    qtest_sysbus_mmio_map(s, MD_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 1, DIRECTORY_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 2, ECC_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 3, MEMORY_BASE);
    return s;
}

static void ensure_clock_running(QTestState *s)
{
    g_autoptr(QDict) response =
        qtest_qmp(s, "{'execute':'query-status'}");
    QDict *status = qdict_get_qdict(response, "return");

    if (!qdict_get_bool(status, "running")) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
}

static char *flat_view_region_type_at(QTestState *s, uint64_t address)
{
    g_autofree char *mtree = qtest_hmp(s, "info mtree -f");
    const char *system_view = strstr(mtree, "Root memory region: system");
    const char *system_view_end;
    const char *line;

    g_assert_nonnull(system_view);
    system_view = strchr(system_view, '\n');
    g_assert_nonnull(system_view);
    system_view++;
    system_view_end = strstr(system_view, "\nFlatView #");
    if (!system_view_end) {
        system_view_end = system_view + strlen(system_view);
    }

    for (line = system_view; line < system_view_end; ) {
        const char *line_end = strchr(line, '\n');
        uint64_t start_address;
        uint64_t end_address;
        char type[32];

        if (!line_end || line_end > system_view_end) {
            line_end = system_view_end;
        }
        if (sscanf(line, " %" SCNx64 "-%" SCNx64 " (prio %*d, %31[^)])",
                   &start_address, &end_address, type) == 3 &&
            address >= start_address && address <= end_address) {
            return g_strdup(type);
        }
        line = line_end + (line_end < system_view_end);
    }
    return NULL;
}

static void assert_flat_view_type(QTestState *s, uint64_t address,
                                  const char *expected)
{
    g_autofree char *type = flat_view_region_type_at(s, address);

    g_assert_cmpstr(type, ==, expected);
}

static QTestState *start_clock_with_memory(const char *size, const char *extra)
{
    QTestState *s = start_with_memory(size, extra);

    ensure_clock_running(s);
    return s;
}

static QTestState *start_parent(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 " NODE_MEMORY_512M
                               "-object memory-backend-ram,id=dma,size=2M "
                               "-device sgi-bedrock,id=bedrock,"
                               "dma-memory=/objects/dma/dma[0],"
                               "node-memory=/objects/node-memory/"
                               "node-memory[0] -S");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 0, BEDROCK_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 5,
                         DIRECTORY_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 7, MEMORY_BASE);
    return s;
}

static char *log_file_new(void)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-md-log-XXXXXX", &path, &error);

    g_assert_no_error(error);
    close(fd);
    return path;
}

static char *empty_prom_new(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-md-prom-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *log_file_take(const char *path)
{
    GError *error = NULL;
    char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    unlink(path);
    return contents;
}

static unsigned log_count(const char *contents, const char *needle)
{
    unsigned found = 0;

    for (const char *p = strstr(contents, needle); p;
         p = strstr(p + strlen(needle), needle)) {
        found++;
    }
    return found;
}

static uint64_t memory_bank_config(unsigned locational_bank,
                                   unsigned size_code)
{
    unsigned shift = (locational_bank / 2) * 8;
    uint64_t byte = (size_code << 4) | (1 << (locational_bank & 1));

    return byte << shift;
}

static uint64_t memory_bank_size_config(unsigned locational_bank,
                                        unsigned size_code)
{
    return (uint64_t)size_code << ((locational_bank / 2) * 8 + 4);
}

static void selftest_start(QTestState *s, unsigned selector,
                           uint64_t data)
{
    qtest_writeq(s, MD(SELFTEST_DATA), data);
    qtest_writeq(s, MD(SELFTEST_CONTROL), (selector << 4) | 1);
}

static void assert_selftest_result(const char *memory_size,
                                   uint64_t memory_config,
                                   unsigned selector,
                                   uint64_t expected_status)
{
    QTestState *s = start_clock_with_memory(memory_size, "");

    qtest_writeq(s, MD(0x8), memory_config);
    selftest_start(s, selector, 0x5a);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    expected_status);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_quit(s);
}

static void test_selftest_phases(void)
{
    uint64_t config = (UINT64_C(3) << 32) | memory_bank_config(0, 1);
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 3, 0xda);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_CONTROL)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_writeq(s, MD(0x8), 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);

    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 3, 0x5a);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_quit(s);
}

/*
 * A read at or after the completion time sees the result whether or not
 * the host has run the completion timer yet. The machine here stays
 * stopped, so stepping the clock runs no timer: that stands in for a host
 * too loaded to run the main loop while a processor polls. The stock PROM
 * samples its clock before the start, reads status, then checks the clock,
 * and reports a hang if that read showed done clear and the clock has
 * passed its deadline. The zero-pattern phase is the one whose fill made
 * that window long. The fill takes no virtual time under qtest, so this
 * covers publication on read, not counting the interval from the start.
 */
static void test_selftest_completion_without_timer(void)
{
    uint64_t config = (UINT64_C(3) << 32) | memory_bank_config(0, 1);
    QTestState *s = start_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 3, 0x80);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);

    /* A failed selector's error address is published by its own read. */
    selftest_start(s, 1, 0x5a);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_quit(s);
}

static void test_selftest_bank_remap(void)
{
    uint64_t config = (UINT64_C(3) << 32) | memory_bank_config(2, 1);
    QTestState *s = start_clock_with_memory("64M", "");

    /* Selector one maps to locational bank two; bank one is absent. */
    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 1, 0x5a);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_quit(s);
}

static void test_selftest_negative_carriers(void)
{
    assert_selftest_result("64M", 0, 1, SELFTEST_DONE);
    assert_selftest_result("64M", memory_bank_size_config(1, 1), 1,
                           SELFTEST_DONE);
    assert_selftest_result("64M", memory_bank_config(1, 6), 1,
                           SELFTEST_DONE);
    assert_selftest_result("96M", memory_bank_config(0, 1) |
                                  memory_bank_config(1, 1), 1,
                           SELFTEST_DONE);
    assert_selftest_result("128M", memory_bank_config(0, 6) |
                                   memory_bank_config(1, 1), 1,
                           SELFTEST_DONE);
}

/*
 * Origin 300 installs locational banks two and three. Temporary descriptors
 * for the other selectors must not consume their dense RAM; checking every
 * selector in both phases distinguishes installed population from guest
 * configuration alone.
 */
static void test_selftest_origin300_installed_population(void)
{
    static const uint64_t detail[] = {
        SELFTEST_ERROR_ADDRESS, SELFTEST_DIRECTORY_ERROR,
        SELFTEST_MEMORY_ERROR0, SELFTEST_MEMORY_ERROR0 + 8,
        SELFTEST_MEMORY_ERROR0 + 0x10, SELFTEST_MEMORY_ERROR0 + 0x18,
    };
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);

    ensure_clock_running(s);
    for (unsigned selector = 0; selector < 8; selector++) {
        unsigned bank_select = selector & 3;
        unsigned configured_bank = selector ^ bank_select;

        qtest_writeq(s, SELFTEST_MACHINE(0x8),
                     ((uint64_t)bank_select << 32) |
                     memory_bank_config(configured_bank, 3));
        for (unsigned phase = 0; phase < 2; phase++) {
            bool installed = selector == 2 || selector == 3;
            uint64_t expected_status = installed ?
                SELFTEST_DONE | SELFTEST_PASSED : SELFTEST_DONE;
            uint64_t expected_error = installed ?
                0 : SELFTEST_MEMORY_ERROR;

            qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_DATA),
                         phase == 0 ? 0xda : 0x5a);
            qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_CONTROL),
                         (selector << 4) | 1);
            g_assert_cmphex(qtest_readq(s,
                                       SELFTEST_MACHINE(SELFTEST_STATUS)),
                            ==, 0);
            g_assert_cmphex(qtest_readq(s,
                                       SELFTEST_MACHINE(
                                           SELFTEST_ERROR_ADDRESS)),
                            ==, 0);
            qtest_clock_step(s, SELFTEST_INTERVAL_NS);
            g_assert_cmphex(qtest_readq(s,
                                       SELFTEST_MACHINE(SELFTEST_STATUS)),
                            ==, expected_status);
            for (unsigned i = 0; i < ARRAY_SIZE(detail); i++) {
                g_assert_cmphex(qtest_readq(s,
                                           SELFTEST_MACHINE(detail[i])),
                                ==, i == 0 ? expected_error : 0);
            }
        }
    }
    qtest_quit(s);
    unlink(prom);
}

/*
 * Enabling absent descriptors must not move the two installed banks within
 * dense backing. A nonzero selector also pins XOR-before-presence ordering.
 */
static void test_memory_installed_population(void)
{
    uint64_t config = UINT64_C(1) << 32;
    QTestState *s = start_with_installed_memory("");

    for (unsigned bank = 0; bank < 8; bank++) {
        config |= memory_bank_config(bank, 3);
    }
    qtest_writeq(s, MD(0x8), config);
    qtest_writeq(s, MEMORY(3 * MEMORY_BANK_SIZE), UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, MEMORY(2 * MEMORY_BANK_SIZE), UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, MEMORY(3 * MEMORY_BANK_SIZE)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, MEMORY(2 * MEMORY_BANK_SIZE)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, MEMORY(MEMORY_BANK_SIZE), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MEMORY(MEMORY_BANK_SIZE)), !=, UINT64_MAX);
    qtest_writeq(s, MEMORY(3 * MEMORY_BANK_SIZE + 0x10000000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s,
                    MEMORY(3 * MEMORY_BANK_SIZE + 0x10000000)), !=,
                    UINT64_MAX);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, config);
    assert_flat_view_type(s, MEMORY(3 * MEMORY_BANK_SIZE), "ram");
    g_assert_cmphex(qtest_readq(s, MEMORY(3 * MEMORY_BANK_SIZE)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
}

/*
 * Origin 300 exposes installed banks in fixed 1 GiB windows. This rejects a
 * raw-RAM mapping at zero and a mapping truncated to the 512 MiB backing.
 */
static void test_origin300_memory_aperture(void)
{
    static const uint8_t prom_marker[] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    int fd = open(prom, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, prom_marker, sizeof(prom_marker)), ==,
                    sizeof(prom_marker));
    g_assert_cmpint(close(fd), ==, 0);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);

    g_assert_cmphex(qtest_readq(s, UINT64_C(0x1fc00000)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(0x8)), ==, 0);
    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 memory_bank_config(2, 3) | memory_bank_config(3, 3));
    qtest_writeq(s, 0, UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, 0), !=, UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, UINT64_C(0x80000000), UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, UINT64_C(0xc0000000), UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, UINT64_C(0x80000000)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, UINT64_C(0xc0000000)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_writeb(s, UINT64_C(0x8fffffff), 0xa5);
    g_assert_cmphex(qtest_readb(s, UINT64_C(0x8fffffff)), ==, 0xa5);
    qtest_writeb(s, UINT64_C(0x90000000), 0x5a);
    g_assert_cmphex(qtest_readb(s, UINT64_C(0x90000000)), !=, 0x5a);
    qtest_quit(s);
    unlink(prom);
}

/*
 * Temporary descriptors must not relocate directory or ECC storage. Swapping
 * software banks with selector three distinguishes dense installed backing
 * from storage keyed only by carrier address.
 */
static void test_backdoors_installed_population(void)
{
    uint64_t config = UINT64_C(2) << 32;
    QTestState *s = start_with_installed_memory("");

    for (unsigned bank = 0; bank < 8; bank++) {
        config |= memory_bank_config(bank, 3);
    }
    qtest_writeq(s, MD(0x8), config);
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(0), 0x13579bdf);
    qtest_writeq(s, DIRECTORY(DIRECTORY_BANK_SIZE), 0x2468ace0);
    qtest_writeb(s, ECC(7), 0x35);
    qtest_writeb(s, ECC(DIRECTORY_BANK_SIZE + 7), 0x46);

    qtest_writeq(s, MD(0x8), (config & ~(UINT64_C(3) << 32)) |
                              (UINT64_C(3) << 32));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0)), ==, 0x2468ace0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_BANK_SIZE)), ==,
                    0x13579bdf);
    g_assert_cmphex(qtest_readb(s, ECC(7)), ==, 0x46);
    g_assert_cmphex(qtest_readb(s, ECC(DIRECTORY_BANK_SIZE + 7)), ==, 0x35);

    qtest_writeq(s, MD(0x8), config);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_256M), 0x10203040);
    qtest_writeq(s, DIRECTORY(DIRECTORY_BANK_SIZE + DIRECTORY_LAST_256M),
                 0x50607080);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)), ==,
                    0x10203040);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE + DIRECTORY_LAST_256M)),
                    ==, 0x50607080);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), UINT64_MAX);
    qtest_writeq(s,
                 DIRECTORY(DIRECTORY_BANK_SIZE +
                           DIRECTORY_FIRST_ABSENT_256M), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)), ==, 0);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE +
                              DIRECTORY_FIRST_ABSENT_256M)), ==, 0);
    /*
     * Software bank 2 selects absent locational bank 0 here, within the
     * declared bank size: the compatibility store admits it as an ordinary
     * standard word rather than rejecting it outside configured memory.
     */
    qtest_writeq(s, DIRECTORY(2 * DIRECTORY_BANK_SIZE), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(2 * DIRECTORY_BANK_SIZE)), ==,
                    0xffffffff);

    qtest_writeb(s, ECC(0x01fffff8), 0xa5);
    qtest_writeb(s, ECC(DIRECTORY_BANK_SIZE + 0x01fffff8), 0x5a);
    g_assert_cmphex(qtest_readb(s, ECC(0x01fffff8)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(s,
                    ECC(DIRECTORY_BANK_SIZE + 0x01fffff8)), ==, 0x5a);
    qtest_writeb(s, ECC(0x02000007), 0xff);
    qtest_writeb(s, ECC(DIRECTORY_BANK_SIZE + 0x02000007), 0xff);
    qtest_writeb(s, ECC(2 * DIRECTORY_BANK_SIZE + 7), 0xff);
    g_assert_cmphex(qtest_readb(s, ECC(0x02000007)), ==, 0);
    g_assert_cmphex(qtest_readb(s,
                    ECC(DIRECTORY_BANK_SIZE + 0x02000007)), ==, 0);
    g_assert_cmphex(qtest_readb(s, ECC(2 * DIRECTORY_BANK_SIZE + 7)), ==, 0);
    qtest_quit(s);
}

static void test_selftest_replacement(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0x5a);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2 - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_quit(s);
}

static void assert_selftest_cancelled(QTestState *s)
{
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
}

static void test_selftest_cancellation(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 4);
    assert_selftest_cancelled(s);

    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    selftest_start(s, 1, 0xda);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 4);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 2);
    assert_selftest_cancelled(s);

    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 2);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 0);
    assert_selftest_cancelled(s);

    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_system_reset(s);
    assert_selftest_cancelled(s);

    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_quit(s);
}

static void test_selftest_unsupported_writes(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_clock_with_memory("64M", extra);

    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, 400);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 3);
    qtest_clock_step(s, 599);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 0x100);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 0x14);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_cmpuint(log_count(contents, SELFTEST_DIAGNOSTIC), ==, 3);
}

static void test_selftest_read_only_results(void)
{
    static const struct {
        uint64_t offset;
        uint64_t expected;
    } detail[] = {
        { SELFTEST_ERROR_ADDRESS, SELFTEST_MEMORY_ERROR },
        { SELFTEST_DIRECTORY_ERROR, 0 },
        { SELFTEST_MEMORY_ERROR0, 0 },
        { SELFTEST_MEMORY_ERROR0 + 8, 0 },
        { SELFTEST_MEMORY_ERROR0 + 0x10, 0 },
        { SELFTEST_MEMORY_ERROR0 + 0x18, 0 },
    };
    QTestState *s = start_clock_with_memory("64M", "");

    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    selftest_start(s, 1, 0x1da);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_DATA)), ==, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_writeq(s, MD(SELFTEST_STATUS), UINT64_MAX);
    for (unsigned i = 0; i < ARRAY_SIZE(detail); i++) {
        qtest_writeq(s, MD(detail[i].offset), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, MD(detail[i].offset)), ==,
                        detail[i].expected);
    }
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_quit(s);
}

static void test_selftest_access_width_policy(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_writeb(s, MD(SELFTEST_CONTROL), 4);
    qtest_writew(s, MD(SELFTEST_CONTROL), 4);
    qtest_writel(s, MD(SELFTEST_CONTROL), 4);
    qtest_writeq(s, MD(SELFTEST_CONTROL) + 1, 4);
    qtest_writel(s, MD(SELFTEST_STATUS), UINT32_MAX);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(s);
}

static unsigned directory_walk(uint64_t base, uint64_t offsets[49])
{
    unsigned count = 0;

    offsets[count++] = base;
    for (unsigned bit = 3; bit <= 26; bit++) {
        offsets[count++] = base | (UINT64_C(1) << bit);
    }
    offsets[count++] = base | UINT64_C(0x07fffff8);
    for (unsigned bit = 3; bit <= 25; bit++) {
        offsets[count++] = (base | UINT64_C(0x07fffff8)) ^
                           (UINT64_C(1) << bit);
    }
    g_assert_cmpuint(count, ==, 49);
    return count;
}

static unsigned directory_untranslated_walk(
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES])
{
    uint64_t bank0[49], bank1[49];
    unsigned count = 0;

    directory_walk(0, bank0);
    directory_walk(DIRECTORY_BANK_SIZE, bank1);
    for (unsigned i = 0; i < ARRAY_SIZE(bank0); i++) {
        if (bank0[i] >= DIRECTORY_FIRST_ABSENT_512M) {
            offsets[count++] = bank0[i];
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(bank1); i++) {
        offsets[count++] = bank1[i];
    }
    g_assert_cmphex(bank1[7], ==, DIRECTORY_BANK_SIZE + 0x200);
    g_assert_cmpuint(count, ==, DIRECTORY_COMPAT_ENTRIES);
    return count;
}

static void test_memory_banks(void)
{
    QTestState *s = start_with_memory("192M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeq(s, MEMORY(0x0123450), UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x0765430),
                 UINT64_C(0xfedcba9876543210));

    g_assert_cmphex(qtest_readq(s, MEMORY(0x0123450)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s,
                    MEMORY(2 * MEMORY_BANK_SIZE + 0x0765430)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(s);
}

static void test_directory_bank_translation(void)
{
    const uint64_t config = memory_bank_config(0, 1);
    const uint64_t value = UINT64_C(0x89abcdef);
    QTestState *s = start_with_memory("64M", "");

    /* Selector one maps software bank one to locational bank zero. */
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) | config);
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(DIRECTORY_BANK_SIZE + 0x20), value);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE + 0x20)), ==, value);

    qtest_writeq(s, MD(0x8), config);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x20)), ==, value);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE + 0x20)), ==, 0);
    qtest_quit(s);
}

static void test_memory_size_boundaries(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("192M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeb(s, MEMORY(0x03ffffff), 0xa5);
    g_assert_cmphex(qtest_readb(s, MEMORY(0x03ffffff)), ==, 0xa5);
    qtest_writeb(s, MEMORY(0x04000000), 0x5a);
    qtest_writeb(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x07ffffff), 0x3c);
    g_assert_cmphex(qtest_readb(s,
                    MEMORY(2 * MEMORY_BANK_SIZE + 0x07ffffff)), ==, 0x3c);
    qtest_writeb(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x08000000), 0xc3);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x04000000 is beyond "
                           "locational bank 0 size 0x04000000"));
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x88000000 is beyond "
                           "locational bank 2 size 0x08000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 2);
}

static void test_memory_absent_banks(void)
{
    g_autofree char *log_disabled = log_file_new();
    g_autofree char *log_empty = log_file_new();
    g_autofree char *log_unsupported = log_file_new();
    g_autofree char *extra_disabled =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_disabled);
    g_autofree char *extra_empty =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_empty);
    g_autofree char *extra_unsupported =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_unsupported);
    g_autofree char *contents_disabled = NULL;
    g_autofree char *contents_empty = NULL;
    g_autofree char *contents_unsupported = NULL;
    QTestState *s;

    s = start_with_memory("64M", extra_disabled);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeb(s, MEMORY(MEMORY_BANK_SIZE), 0x5a);
    qtest_quit(s);

    s = start_with_memory("64M", extra_empty);
    qtest_writeq(s, MD(0x8), 0x01);
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    s = start_with_memory("64M", extra_unsupported);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 6));
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    contents_disabled = log_file_take(log_disabled);
    contents_empty = log_file_take(log_empty);
    contents_unsupported = log_file_take(log_unsupported);
    g_assert_nonnull(strstr(contents_disabled,
                           "memory write at offset 0x40000000 rejected: "
                           "locational bank 1 is disabled"));
    g_assert_nonnull(strstr(contents_empty,
                           "memory read at offset 0x00000000 rejected: "
                           "locational bank 0 has size code 0"));
    g_assert_nonnull(strstr(contents_unsupported,
                           "memory read at offset 0x00000000 rejected: "
                           "locational bank 0 has unsupported size code 6"));
    g_assert_cmpuint(log_count(contents_disabled, MEMORY_DIAGNOSTIC), ==, 1);
    g_assert_cmpuint(log_count(contents_empty, MEMORY_DIAGNOSTIC), ==, 1);
    g_assert_cmpuint(log_count(contents_unsupported, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_crossing_does_not_corrupt(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("128M", extra);

    qtest_writeq(s, MD(0x8), 0x13);
    qtest_writel(s, MEMORY(0x03fffffc), 0x11223344);
    qtest_writel(s, MEMORY(MEMORY_BANK_SIZE), 0x55667788);
    qtest_writeq(s, MEMORY(0x03fffffc), UINT64_C(0xa5a5a5a5c3c3c3c3));
    g_assert_cmphex(qtest_readl(s, MEMORY(0x03fffffc)), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(s, MEMORY(MEMORY_BANK_SIZE)), ==, 0x55667788);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x03fffffc size 8 "
                           "crosses locational bank 0 size 0x04000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_selector_and_backing_bound(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("64M", extra);

    /* Selector one maps software bank one to locational bank zero. */
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) | 0x13);
    qtest_writeq(s, MEMORY(MEMORY_BANK_SIZE + 0x18),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, MEMORY(MEMORY_BANK_SIZE + 0x18)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writeb(s, MEMORY(0), 0xa5);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x00000000 rejected: "
                           "locational bank 1 exceeds raw backing size "
                           "0x04000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_selector_high_bank(void)
{
    QTestState *s = start_with_memory("64M", "");

    /* Selector three maps software bank five to locational bank six. */
    qtest_writeq(s, MD(0x8), (UINT64_C(3) << 32) |
                              memory_bank_config(6, 1));
    qtest_writeq(s, MEMORY(5 * MEMORY_BANK_SIZE + 0x28),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, MEMORY(5 * MEMORY_BANK_SIZE + 0x28)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(s);
}

static void test_memory_access_widths_and_byte_order(void)
{
    QTestState *s = start_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x20), UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readb(s, MEMORY(0x20)), ==, 0x01);
    g_assert_cmphex(qtest_readw(s, MEMORY(0x20)), ==, 0x0123);
    g_assert_cmphex(qtest_readl(s, MEMORY(0x20)), ==, 0x01234567);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x20)), ==,
                    UINT64_C(0x0123456789abcdef));

    qtest_writeb(s, MEMORY(0x28), 0xa5);
    qtest_writew(s, MEMORY(0x2a), 0xbcde);
    qtest_writel(s, MEMORY(0x2c), 0xf0123456);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x28)), ==,
                    UINT64_C(0xa500bcdef0123456));
    qtest_quit(s);
}

static void test_memory_reset_decode(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("64M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeb(s, MEMORY(0x30), 0xa5);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    g_assert_cmphex(qtest_readb(s, MEMORY(0x30)), ==, 0xa5);
    qtest_system_reset(s);
    qtest_readb(s, MEMORY(0x30));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory read at offset 0x00000030 rejected: "
                           "locational bank 0 is disabled"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_without_node_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-bedrock-md,id=md -S "
                                MEMORY_LOG_OPTIONS "%s", log);

    qtest_sysbus_mmio_map(s, MD_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 3, MEMORY_BASE);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory read at offset 0x00000000 rejected: "
                           "node memory is not linked"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_parent_exports_memory(void)
{
    QTestState *s = start_parent();

    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x40), UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x40)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
}

static void program_all(QTestState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(s, MD(storage_registers[i].offset), UINT64_MAX);
    }
    qtest_writeq(s, MD(0x8), UINT64_MAX);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000555));
    qtest_writeq(s, MD(0x40000), 3);
    qtest_writeq(s, MD(0), 0);
}

static void assert_storage_zero(QTestState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)),
                        ==, 0);
    }
}

static void test_reset(void)
{
    uint64_t compat_offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned compat_count = directory_untranslated_walk(compat_offsets);
    QTestState *s = start("");

    program_all(s);
    qtest_set_irq_in(s, "/machine/peripheral/md", "soft-reset", 0, 1);
    assert_storage_zero(s);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, MEMORY_CONFIG_MASK);
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000555));
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);

    program_all(s);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, DIRECTORY(PROTECTION_FIRST),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        qtest_writeq(s, DIRECTORY(compat_offsets[i]),
                     UINT64_C(0x13579bdf00000000) | i);
    }
    qtest_system_reset(s);
    assert_storage_zero(s);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FIRST)), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)), ==, 0);
    for (unsigned i = 0; i < compat_count; i++) {
        g_assert_cmphex(qtest_readq(s, DIRECTORY(compat_offsets[i])), ==, 0);
    }
    qtest_quit(s);
}

static void test_protection_storage(void)
{
    static const uint64_t interleave_offsets[] = {
        PROTECTION_FIRST, PROTECTION_FIRST + 0x10,
        PROTECTION_FIRST + 0x20, PROTECTION_INTERLEAVE_3,
        PROTECTION_NEXT_PAGE, PROTECTION_BANK1_HIGH, PROTECTION_LAST,
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(interleave_offsets); i++) {
        uint64_t value = UINT64_C(0x1020304050607080) + i;

        g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                        ==, 0);
        qtest_writeq(s, DIRECTORY(interleave_offsets[i]), value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(interleave_offsets); i++) {
        uint64_t value = UINT64_C(0x1020304050607080) + i;

        if (i < 5) {
            g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                            ==, value);
        } else {
            g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                            !=, value);
        }
    }

    for (unsigned bank = 0; bank < ARRAY_SIZE(protection_bank_bounds); bank++) {
        g_assert_cmphex(protection_bank_bounds[bank][1] -
                        15 * PROTECTION_SAMPLE_STRIDE,
                        ==, protection_bank_bounds[bank][0]);
        for (unsigned sample = 0; sample < 16; sample++) {
            uint64_t offset = protection_bank_bounds[bank][1] -
                              sample * PROTECTION_SAMPLE_STRIDE;
            uint64_t value = UINT64_C(0xfedcba9876543200) +
                             bank * 16 + sample;

            qtest_writeq(s, DIRECTORY(offset), value);
        }
    }
    for (unsigned bank = 0; bank < ARRAY_SIZE(protection_bank_bounds); bank++) {
        for (unsigned sample = 0; sample < 16; sample++) {
            uint64_t offset = protection_bank_bounds[bank][1] -
                              sample * PROTECTION_SAMPLE_STRIDE;
            uint64_t value = UINT64_C(0xfedcba9876543200) +
                             bank * 16 + sample;

            if (bank == 0 && sample >= 8) {
                g_assert_cmphex(qtest_readq(s, DIRECTORY(offset)), ==, value);
            } else {
                g_assert_cmphex(qtest_readq(s, DIRECTORY(offset)), !=, value);
            }
        }
    }
    qtest_quit(s);
}

static void test_protection_capacity_probe(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    const uint64_t bank2 = UINT64_C(0xa55aa55aa55aa55a);
    const uint64_t bank3 = UINT64_C(0x5aa55aa55aa55aa5);
    const uint64_t absent = UINT64_C(0x599a599a599a599a);
    const uint64_t selected_lanes = UINT64_C(0x0000ffff0000ffff);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S -d guest_errors -D %s",
                                quoted_prom, log);

    qtest_writeq(s, PROTECTION_MACHINE_BANK2, bank2);
    qtest_writeq(s, PROTECTION_MACHINE_BANK3, bank3);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK2), ==, bank2);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK2_ALIAS), ==, bank2);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK3), ==, bank3);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK3_ALIAS), ==, bank3);
    qtest_writeq(s, SELFTEST_MACHINE(0x8), UINT64_C(1) << 32);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK2), ==, bank3);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK3), ==, bank2);

    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 memory_bank_config(2, 3) | memory_bank_config(3, 3));
    qtest_writeq(s, PROTECTION_MACHINE_BANK0, absent);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK0) &
                    selected_lanes, !=, absent & selected_lanes);
    qtest_writeq(s, DIRECTORY_MACHINE_BANK2_FIRST_ABSENT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY_MACHINE_BANK2_FIRST_ABSENT),
                    ==, 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_null(strstr(contents, "protection"));
    g_assert_nonnull(strstr(contents,
                           "directory write at offset 0x12000000"));
    unlink(prom);
}

static void test_directory_storage(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)), ==, 0);

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)), ==, 0);

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x01234567));

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x10),
                 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x76543210));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x10)),
                    ==, 0x76543210);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, UINT64_C(0x0123456789abcdef));

    qtest_writeq(s, MD(0x40000), 3);
    qtest_writeq(s, DIRECTORY(DIRECTORY_OTHER_LINE),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)),
                    ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x76543210));
    qtest_quit(s);
}

static void test_directory_compatibility_storage(void)
{
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned count = directory_untranslated_walk(offsets);
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(offsets[0]),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==, 0x01234567);
    qtest_writeq(s, DIRECTORY(offsets[1]),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[1])), ==,
                    UINT64_C(0xfedcba9876543210));

    qtest_writeq(s, MD(0x40000), 3);
    for (unsigned i = 0; i < count; i++) {
        uint64_t value = UINT64_C(0xa500000000000000) | i;

        qtest_writeq(s, DIRECTORY(offsets[i]), value);
    }
    for (unsigned i = 0; i < count; i++) {
        g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[i])), ==,
                        UINT64_C(0xa500000000000000) | i);
    }
    qtest_writeq(s, DIRECTORY(offsets[0]),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(s, MD(0x40000), 2);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==, 0x01234567);
    qtest_writeq(s, MD(0x40000), 3);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==,
                    UINT64_C(0x89abcdef01234567));
    qtest_quit(s);
}

static void test_directory_compatibility_capacity(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned count = directory_untranslated_walk(offsets);
    unsigned admitted = 1;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    qtest_writeq(s, MD(0x40000), 3);
    qtest_writeq(s, DIRECTORY(0x04001230), UINT64_C(0xfeedfacecafebeef));
    for (unsigned i = 0; i < count - 1; i++) {
        qtest_writeq(s, DIRECTORY(offsets[i]), admitted++);
    }
    g_assert_cmpuint(admitted, ==, DIRECTORY_COMPAT_ENTRIES);
    qtest_writeq(s, DIRECTORY(offsets[count - 1]), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(offsets[count - 1])), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x04001230)), ==,
                    UINT64_C(0xfeedfacecafebeef));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "compatibility store is full"));
}

/*
 * Origin 300's installed-bank-mask=12 population leaves locational banks
 * zero and one absent. Stock IP35 PROM provisionally enables every bank
 * descriptor and walks each enabled bank's directory backdoor as a
 * retention probe, including the absent ones. An aligned carrier within
 * the declared bank size must reach the compatibility store rather than a
 * bus error; only a disabled bank or an offset beyond the declared bank
 * size stays outside configured memory.
 */
static void test_directory_installed_absent_bank_compat(void)
{
    g_autofree char *admitted_log = log_file_new();
    g_autofree char *disabled_log = log_file_new();
    g_autofree char *beyond_log = log_file_new();
    g_autofree char *admitted_extra =
        g_strdup_printf("-d guest_errors -D %s", admitted_log);
    g_autofree char *disabled_extra =
        g_strdup_printf("-d guest_errors -D %s", disabled_log);
    g_autofree char *beyond_extra =
        g_strdup_printf("-d guest_errors -D %s", beyond_log);
    g_autofree char *admitted_contents = NULL;
    g_autofree char *disabled_contents = NULL;
    g_autofree char *beyond_contents = NULL;
    QTestState *s;

    /* Locational bank 0 is absent; within stays inside the declared size. */
    s = start_with_installed_memory(admitted_extra);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 3));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(0x20), UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x20)), ==, 0x01234567);
    qtest_quit(s);
    admitted_contents = log_file_take(admitted_log);
    g_assert_null(strstr(admitted_contents, "outside configured memory"));

    /* Locational bank 0 left disabled still rejects. */
    s = start_with_installed_memory(disabled_extra);
    qtest_writeq(s, DIRECTORY(0x20), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x20)), ==, 0);
    qtest_quit(s);
    disabled_contents = log_file_take(disabled_log);
    g_assert_nonnull(strstr(disabled_contents, "outside configured memory"));

    /* An offset beyond the declared 256 MiB bank size still rejects. */
    s = start_with_installed_memory(beyond_extra);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 3));
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)),
                    ==, 0);
    qtest_quit(s);
    beyond_contents = log_file_take(beyond_log);
    g_assert_nonnull(strstr(beyond_contents, "outside configured memory"));
}

static void test_directory_access_policy(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);
    const uint64_t addr[] = {
        DIRECTORY(DIRECTORY_LINE_01A00000),
        DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
    };
    const uint64_t value[] = {
        UINT64_C(0x1122334455667788),
        UINT64_C(0x8877665544332211),
    };

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, MD(0x40000), 3);
    for (unsigned i = 0; i < ARRAY_SIZE(addr); i++) {
        qtest_writeq(s, addr[i], value[i]);
        qtest_writeb(s, addr[i], 0);
        qtest_writew(s, addr[i], 0);
        qtest_writel(s, addr[i], 0);
        qtest_writeq(s, addr[i] + 1, 0);
        g_assert_cmphex(qtest_readq(s, addr[i]), ==, value[i]);
        g_assert_cmphex(qtest_readb(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readw(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readl(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readq(s, addr[i] + 1), ==, 0);
    }
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M), 0xc001d00d);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M + 8),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M)),
                    ==, 0xc001d00d);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M + 8)),
                    ==, UINT64_C(0x0123456789abcdef));

    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeb(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writew(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writel(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M + 1), 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_FIRST_ABSENT), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FIRST_ABSENT)),
                    ==, 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "not an aligned 64-bit carrier word"));
}

static void test_directory_fitted_memory(void)
{
    QTestState *s = start_with_memory("256M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 3));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_256M), 0x2468ace0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)),
                    ==, 0x2468ace0);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), 0x13579bdf);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)), ==,
                    0x2468ace0);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_192M),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_192M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_parent_exports_directory(void)
{
    QTestState *s = start_parent();

    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 4));
    /* Raw patterns need directory ECC ignored, as the PROM's walk has. */
    qtest_writeq(s, BEDROCK_MD(0x40000), 2);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x13579bdf);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0x13579bdf);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_without_node_memory(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 "
                               "-device sgi-bedrock-md,id=md -S");

    qtest_sysbus_mmio_map(s, MD_PATH, 1, DIRECTORY_BASE);
    qtest_writeq(s, DIRECTORY(0), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0)), ==, 0);
    qtest_writeq(s, DIRECTORY(8), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(8)), ==, 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_LAST),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_LAST)),
                    !=, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_register_contract(void)
{
    QTestState *s = start("");

    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    qtest_writeq(s, MD(0), 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, 0);
    qtest_writeq(s, MD(0x8), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, MEMORY_CONFIG_MASK);
    qtest_writeq(s, MD(0x10), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10)), ==, 0xff);
    qtest_writeq(s, MD(0x10000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10000)), ==,
                    UINT64_C(0x8fffffffffff7ff3));
    qtest_writeq(s, MD(0x10010), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    qtest_writeq(s, MD(0x20008), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0xffffffffffff));
    qtest_writeq(s, MD(0x40000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x20030), 1);
    g_assert_cmphex(qtest_readq(s, MD(0x20030)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20048)), ==, 0);
    qtest_writeq(s, MD(0x60040), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x60040)), ==, 1);
    qtest_writeq(s, MD(0x10100), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);
    qtest_quit(s);
}

/*
 * Every register is a big-endian 64-bit word. Byte order is the hardware
 * contract. Refusing a narrower access is model policy: the datasheet
 * states the width and nothing about a narrower access.
 */
static void test_access_width_policy(void)
{
    QTestState *s = start("");
    const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t expected[8] = { 0, 0, 3, 4, 5, 6, 7, 8 };
    uint8_t actual[8];

    qtest_memwrite(s, MD(0x20008), bytes, sizeof(bytes));
    qtest_memread(s, MD(0x20008), actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_writeb(s, MD(0x20008), 0);
    qtest_writew(s, MD(0x20008), 0);
    qtest_writel(s, MD(0x20008), 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_quit(s);
}

/* The registers outside the storage table keep state of their own. */
static void test_explicit_registers_independent(void)
{
    static const struct {
        uint64_t offset;
        uint64_t value;
    } registers[] = {
        { 0x0, UINT64_C(0x0123456789abcdef) },
        { 0x8, UINT64_C(0x3000000000000101) },
        { 0x10, 0x5a },
        { 0x20000, UINT64_C(0x8000000000000c35) },
        { 0x20008, UINT64_C(0x112233445566) },
        { 0x40000, 2 },
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, MD(registers[i].offset), registers[i].value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(registers[i].offset)), ==,
                        registers[i].value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, MD(registers[i].offset), 0);
        for (unsigned j = 0; j < ARRAY_SIZE(registers); j++) {
            if (j != i) {
                g_assert_cmphex(qtest_readq(s, MD(registers[j].offset)), ==,
                                registers[j].value);
            }
        }
        qtest_writeq(s, MD(registers[i].offset), registers[i].value);
    }
    qtest_quit(s);
}

/*
 * A write to the reply queue register resets hardware credit counters and
 * the request queue fields are programmable up to four. The model has
 * neither credits nor queues, so both keep their fixed reads.
 */
static void test_queue_sizes_fixed(void)
{
    QTestState *s = start("");

    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    qtest_writeq(s, MD(0x60), 0);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    qtest_writeq(s, MD(0x60), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x40050), UINT64_C(0x0000000001010100));
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x40050), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_quit(s);
}

/*
 * The Itanium probe reads the four clear registers and expects the base
 * error registers clean. Idle presence only: the model raises no error,
 * so nothing here shows read-to-clear.
 */
static void test_error_cluster_idle(void)
{
    static const uint64_t clears[] = { 0x58, 0x40018, 0x40028, 0x60018 };
    static const uint64_t bases[] = { 0x40, 0x48, 0x40010, 0x40020, 0x60010 };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(clears); i++) {
        g_assert_cmphex(qtest_readq(s, MD(clears[i])), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(clears[i])), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readq(s, MD(bases[i])), ==, 0);
    }
    qtest_quit(s);
}

static void test_policy_zero_ignores_writes(void)
{
    static const uint64_t unsupported_commands[] = {
        0, 3, 5, 6, 7, 0x12, 0x14, 0x70, UINT64_MAX,
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(s, MD(storage_registers[i].offset), UINT64_MAX);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(policy_zero_registers); i++) {
        qtest_writeq(s, MD(policy_zero_registers[i]), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, MD(policy_zero_registers[i])), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported_commands); i++) {
        qtest_writeq(s, MD(0x20030), unsupported_commands[i]);
        g_assert_cmphex(qtest_readq(s, MD(0x20030)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x20040)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x20048)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x40058)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x60020)), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)), ==,
                        storage_registers[i].mask);
    }
    qtest_quit(s);
}

/*
 * Bits 62:24 are reserved. Bits 23:12 are the live counter, whose write
 * rule is open, so they are never set here.
 */
static void test_refresh_mask(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x20000), UINT64_C(0xffffffffff000c35));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    qtest_writeq(s, MD(0x20000), UINT64_C(0x7fffffffff000000));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==, 0);
    qtest_quit(s);
}

/* The Itanium L1 code sets and clears single bits of a 20-bit lock word. */
static void test_perf_counter_lock_word(void)
{
    QTestState *s = start("");
    uint64_t pattern = 0;

    for (unsigned bit = 0; bit < 20; bit++) {
        pattern |= UINT64_C(1) << bit;
        qtest_writeq(s, MD(0x10010),
                     qtest_readq(s, MD(0x10010)) | (UINT64_C(1) << bit));
        g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==, pattern);
    }
    for (unsigned bit = 0; bit < 20; bit++) {
        pattern &= ~(UINT64_C(1) << bit);
        qtest_writeq(s, MD(0x10010),
                     qtest_readq(s, MD(0x10010)) & ~(UINT64_C(1) << bit));
        g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==, pattern);
    }
    qtest_quit(s);
}

static void test_writable_masks(void)
{
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        uint64_t value = UINT64_MAX ^ (UINT64_C(1) << (i + 2));

        for (unsigned j = 0; j < ARRAY_SIZE(storage_registers); j++) {
            qtest_writeq(s, MD(storage_registers[j].offset), 0);
        }
        qtest_writeq(s, MD(storage_registers[i].offset), value);
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)), ==,
                        value & storage_registers[i].mask);
        for (unsigned j = 0; j < ARRAY_SIZE(storage_registers); j++) {
            if (j != i) {
                g_assert_cmphex(qtest_readq(s,
                                            MD(storage_registers[j].offset)),
                                ==, 0);
            }
        }
    }
    qtest_quit(s);
}

static void migrate_wait(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response =
            qtest_qmp(from, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                           "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void migrate_pair(QTestState *from, QTestState *to, const char *uri)
{
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
}

/*
 * Post-load must rebuild RAM aliases from the migrated selector and leave
 * capacity holes on the decoder. Data alone would also pass through MMIO.
 */
static void test_memory_installed_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-md-memory-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    uint64_t config = (UINT64_C(2) << 32) |
                      memory_bank_config(2, 3) |
                      memory_bank_config(3, 3);
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M "
                       "-bios %s -S", quoted_prom);
    to = qtest_initf("-M origin300 -cpu R14000 -smp 2 -m 512M "
                     "-bios %s -S -incoming defer", quoted_prom);
    qtest_writeq(from, SELFTEST_MACHINE(0x8), config);
    qtest_writeq(from, 0, UINT64_C(0x0123456789abcdef));
    qtest_writeq(from, MEMORY_BANK_SIZE,
                 UINT64_C(0xfedcba9876543210));
    assert_flat_view_type(from, 0, "ram");
    assert_flat_view_type(from, 0x10000000, "i/o");
    ensure_clock_running(from);
    migrate_pair(from, to, uri);
    g_assert_cmphex(qtest_readq(to, SELFTEST_MACHINE(0x8)), ==, config);
    assert_flat_view_type(to, 0, "ram");
    assert_flat_view_type(to, 0x10000000, "i/o");
    assert_flat_view_type(to, 2 * MEMORY_BANK_SIZE, "i/o");
    g_assert_cmphex(qtest_readq(to, 0), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(to, MEMORY_BANK_SIZE), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
    unlink(prom);
}

static void test_selftest_migration_running(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-md-selftest-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    int64_t source_time;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_clock_with_memory("64M", "");
    to = start_with_memory("64M", "-incoming defer");
    qtest_clock_set(from, 0);
    qtest_writeq(from, MD(0x8), (UINT64_C(3) << 32) |
                                  memory_bank_config(0, 1));
    selftest_start(from, 3, 0xda);
    source_time = qtest_clock_step(from, 400);
    migrate_pair(from, to, uri);
    ensure_clock_running(to);
    qtest_clock_set(to, source_time);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_clock_step(to, 599);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==, 0);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_ERROR_ADDRESS)), ==, 0);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    uint64_t compat_offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned compat_count = directory_untranslated_walk(compat_offsets);
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_clock_with_memory("512M", "");
    to = start("-incoming defer");
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(from, MD(storage_registers[i].offset),
                     UINT64_MAX ^ (UINT64_C(1) << (i + 2)));
    }
    qtest_writeq(from, MD(0), 0x55aa);
    qtest_writeq(from, MD(0x8), memory_bank_config(0, 5) |
                                 memory_bank_config(1, 5));
    qtest_writeq(from, MD(0x20000), UINT64_C(0x8000000000000c35));
    qtest_writeq(from, MD(0x40000), 3);
    selftest_start(from, 1, UINT64_MAX);
    qtest_clock_step(from, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(from, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(from, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(from, DIRECTORY(PROTECTION_FIRST),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(from, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        qtest_writeq(from, DIRECTORY(compat_offsets[i]),
                     UINT64_C(0x13579bdf00000000) | i);
    }
    migrate_pair(from, to, uri);
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(to, MD(storage_registers[i].offset)), ==,
                        (UINT64_MAX ^ (UINT64_C(1) << (i + 2))) &
                        storage_registers[i].mask);
    }
    g_assert_cmphex(qtest_readq(to, MD(0)), ==, 0x55aa);
    g_assert_cmphex(qtest_readq(to, MD(0x8)), ==,
                    memory_bank_config(0, 5) | memory_bank_config(1, 5));
    g_assert_cmphex(qtest_readq(to, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(to, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_ERROR_ADDRESS)), ==,
                    SELFTEST_MEMORY_ERROR);
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(to,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_FIRST)),
                    ==, UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        g_assert_cmphex(qtest_readq(to, DIRECTORY(compat_offsets[i])), ==,
                        UINT64_C(0x13579bdf00000000) | i);
    }
    qtest_writeq(to, MEMORY(0x23450), UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, MEMORY(0x23450)), ==,
                    UINT64_C(0x13579bdf2468ace0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/* Fitted ECC bytes retain state in physical doubleword order. */
static void test_ecc_carrier(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0);
    qtest_writew(s, ECC(6), 0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0xa55a);
    /* Byte carrier 7 is doubleword 0's, the halfword's low half. */
    g_assert_cmphex(qtest_readb(s, ECC(7)), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, ECC(6)), ==, 0xa5);

    qtest_writeb(s, ECC(7), 0x12);
    qtest_writeb(s, ECC(6), 0x34);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x3412);

    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0x5aa5);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x9999);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x3412);
    qtest_quit(s);
}

static void test_ecc_bank_translation(void)
{
    QTestState *s = start_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) |
                              memory_bank_config(0, 1));
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x1357);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x1357);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==, 0);

    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x2468);
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) |
                              memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x1357);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x2468);
    qtest_quit(s);
}

/* Width first, alignment second, admission last. */
static void test_ecc_validation_order(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writel(s, ECC(4), 0xffffffff);
    qtest_writew(s, ECC(7), 0xffff);
    qtest_writeb(s, ECC(ECC_FIRST_ABSENT_BYTE_512M), 0xff);
    qtest_writeb(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0xff);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW + 0x10), 0xffff);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH + 0x10), 0xffff);
    qtest_writew(s, ECC(2 * DIRECTORY_BANK_SIZE + 6), 0xffff);
    qtest_writel(s, ECC(0x4000005), 0xffffffff);
    qtest_writew(s, ECC(0x4000007), 0xffff);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, ECC_WRITE_WIDTH));
    g_assert_nonnull(strstr(contents, ECC_WRITE_UNALIGNED));
    g_assert_nonnull(strstr(contents, ECC_WRITE_ABSENT));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000006 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000016 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000016 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x10000006 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000005 size 4 "
        "rejected: unsupported width"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 2 "
        "rejected: unaligned halfword"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 9);
}

static void test_ecc_fitted_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("192M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeb(s, ECC(0x007fffff), 0xa5);
    qtest_writeb(s, ECC(2 * DIRECTORY_BANK_SIZE + 0x00ffffff), 0x5a);
    g_assert_cmphex(qtest_readb(s, ECC(0x007fffff)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(s,
                    ECC(2 * DIRECTORY_BANK_SIZE + 0x00ffffff)), ==, 0x5a);
    qtest_writeb(s, ECC(DIRECTORY_BANK_SIZE + 7), 0xff);
    qtest_writeb(s, ECC(2 * DIRECTORY_BANK_SIZE + 0x01000007), 0xff);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000007 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x11000007 size 1 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

static void test_ecc_without_node_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-bedrock-md,id=md -S "
                                ECC_LOG_OPTIONS "%s", log);

    qtest_sysbus_mmio_map(s, MD_PATH, 2, ECC_BASE);
    qtest_writeb(s, ECC(0), 0xff);
    g_assert_cmphex(qtest_readb(s, ECC(0)), ==, 0);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0xa55a);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x5aa5);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x5aa5);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x00000000 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor read offset 0x00000000 size 1 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

static void test_ecc_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writew(s, ECC(6), 0xa55a);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    qtest_system_reset(s);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==, 0);
    qtest_quit(s);
}

static void test_ecc_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-ecc-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start("");
    to = start("-incoming defer");
    qtest_writeq(from, MD(0x8), memory_bank_config(0, 4));
    qtest_writew(from, ECC(6), 0xa55a);
    qtest_writew(from, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(from, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
    g_assert_cmphex(qtest_readw(to, ECC(6)), ==, 0xa55a);
    g_assert_cmphex(qtest_readw(to, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0x5aa5);
    g_assert_cmphex(qtest_readw(to, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x9999);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_parent_exports_ecc(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 " NODE_MEMORY_512M
                               "-object memory-backend-ram,id=dma,size=2M "
                               "-device sgi-bedrock,id=bedrock,"
                               "dma-memory=/objects/dma/dma[0],"
                               "node-memory=/objects/node-memory/"
                               "node-memory[0] -S");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 0, BEDROCK_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 6, ECC_BASE);
    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 4));
    qtest_writew(s, ECC(6), 0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0xa55a);
    qtest_quit(s);
}

/*
 * The memory-special space (docs/datasheets/bedrock-memory-directory.md,
 * "Memory special space"): the doubleword at the base of each 64-byte block
 * of node memory is a fetch-and-op variable and the offset inside the block
 * names the operation. What separates this from a plain alias of memory is
 * the value returned before the variable changes.
 */
static void test_fetchop_operations(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x1000), 5);
    qtest_writeq(s, MEMORY(0x1040), UINT64_C(0x1122334455667788));

    /* Offset 0 is a plain load or store of the variable. */
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1000)), ==, 5);
    qtest_writeq(s, FETCHOP(0x1000), 7);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 7);

    /* Fetch-and-increment, -decrement and -clear return the old value. */
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1008)), ==, 7);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 8);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1010)), ==, 8);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 7);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1018)), ==, 7);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1010)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, UINT64_MAX);

    /* A store at the increment and decrement offsets ignores its data. */
    qtest_writeq(s, FETCHOP(0x1008), UINT64_C(0x1234));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 0);
    qtest_writeq(s, FETCHOP(0x1010), UINT64_C(0x1234));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, UINT64_MAX);

    /* Store-and and store-or. */
    qtest_writeq(s, FETCHOP(0x1018), UINT64_C(0x0f0f0f0f0f0f0f0f));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x0f0f0f0f0f0f0f0f));
    qtest_writeq(s, FETCHOP(0x1020), UINT64_C(0xf000000000000000));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0xff0f0f0f0f0f0f0f));

    /*
     * 32-bit forms: bit 2 names the high (0) or low (4) word, and every
     * operation acts on that word alone as a 32-bit variable and returns
     * that word of the old value. The Origin 300 memory diagnostic's word
     * tests (its 0x1000c100 and 0x1000caa0) require the two words to be
     * independent; that an increment or decrement wraps inside its word
     * rather than carrying into the other is inferred from that.
     */
    qtest_writel(s, FETCHOP(0x1004), 0xfffffffe);
    qtest_writel(s, FETCHOP(0x1000), 0x11223344);
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x1004)), ==, 0xfffffffe);
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x1000)), ==, 0x11223344);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223344fffffffe));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x100c)), ==, 0xfffffffe);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223344ffffffff));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x1008)), ==, 0x11223344);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223345ffffffff));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x100c)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x1122334500000000));
    qtest_writel(s, FETCHOP(0x1014), 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223345ffffffff));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x1010)), ==, 0x11223345);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223344ffffffff));
    qtest_writel(s, FETCHOP(0x1014), 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x11223344fffffffe));
    qtest_writel(s, FETCHOP(0x101c), 0x0000ffff);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x112233440000fffe));
    qtest_writel(s, FETCHOP(0x1020), 0xf0000000);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0xf12233440000fffe));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x101c)), ==, 0x0000fffe);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0xf122334400000000));
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x1018)), ==, 0xf1223344);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 0);

    /*
     * The diagnostic's own form: 32-bit stores at the increment and
     * decrement offsets, whose data is ignored, each reach one word. The
     * high word wraps without touching the low one.
     */
    qtest_writel(s, FETCHOP(0x1000), 0xffffffff);
    qtest_writel(s, FETCHOP(0x1004), 0x80000000);
    qtest_writel(s, FETCHOP(0x1008), 0x12345678);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x0000000080000000));
    qtest_writel(s, FETCHOP(0x100c), 0x12345678);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0x0000000080000001));
    qtest_writel(s, FETCHOP(0x1010), 0x12345678);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0xffffffff80000001));
    qtest_writel(s, FETCHOP(0x1014), 0x12345678);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==,
                    UINT64_C(0xffffffff80000000));
    qtest_writeq(s, MEMORY(0x1000), 0);

    /* The next block is its own variable; the rest of a block is unreached. */
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x1048)), ==,
                    UINT64_C(0x1122334455667788));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1040)), ==,
                    UINT64_C(0x1122334455667789));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1008)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1018)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1020)), ==, 0);
    qtest_quit(s);
}

static void test_fetchop_rejections(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x2000), 0x55);
    /* Offsets that name no operation, in each direction and width. */
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x2020)), ==, 0);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x2038)), ==, 0);
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x2024)), ==, 0);
    qtest_writeq(s, FETCHOP(0x2028), 1);
    qtest_writeq(s, FETCHOP(0x2038), 1);
    /* Narrow and unaligned accesses. */
    g_assert_cmphex(qtest_readw(s, FETCHOP(0x2000)), ==, 0);
    qtest_writew(s, FETCHOP(0x2000), 0x99);
    qtest_writeb(s, FETCHOP(0x2008), 1);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x2004)), ==, 0);
    g_assert_cmphex(qtest_readl(s, FETCHOP(0x2002)), ==, 0);
    /* A block in a bank the memory decoder does not answer. */
    g_assert_cmphex(qtest_readq(s, FETCHOP(MEMORY_BANK_SIZE + 8)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x2000)), ==, 0x55);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002020 "
        "rejected: no such operation"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002038 "
        "rejected: no such operation"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002024 "
        "rejected: no such operation"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op write at offset 0x000002028 "
        "rejected: no such operation"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op write at offset 0x000002038 "
        "rejected: no such operation"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002000 size 2 "
        "rejected: not a 64-bit or 32-bit access"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op write at offset 0x000002000 size 2 "
        "rejected: not a 64-bit or 32-bit access"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op write at offset 0x000002008 size 1 "
        "rejected: not a 64-bit or 32-bit access"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002004 size 8 "
        "rejected: unaligned"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: fetch-and-op read at offset 0x000002002 size 4 "
        "rejected: unaligned"));
    g_assert_cmpuint(log_count(contents, FETCHOP_DIAGNOSTIC), ==, 10);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: memory read at offset 0x40000000 rejected: "
        "locational bank 1 is disabled"));
}

static void test_parent_exports_fetchop(void)
{
    QTestState *s = start_parent();

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 9, FETCHOP_BASE);
    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x40), 41);
    g_assert_cmphex(qtest_readq(s, FETCHOP(0x48)), ==, 41);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x40)), ==, 42);
    qtest_quit(s);
}

/*
 * The two addresses the Origin 300's own diagnostics reach through the
 * space: nmem's section 3 pattern word and scct's processor barrier, both
 * in software bank 0, which the PROM's bank-select 2 places on installed
 * locational bank 2.
 */
static void test_origin300_fetchop(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);

    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 (UINT64_C(2) << 32) | memory_bank_config(2, 3) |
                 memory_bank_config(3, 3));
    qtest_writeq(s, UINT64_C(0x506000), 3);
    g_assert_cmphex(qtest_readq(s, FETCHOP_MACHINE_BASE + 0x506000), ==, 3);
    qtest_writeq(s, FETCHOP_MACHINE_BASE + 0x506000, 9);
    g_assert_cmphex(qtest_readq(s, UINT64_C(0x506000)), ==, 9);
    g_assert_cmphex(qtest_readq(s, FETCHOP_MACHINE_BASE + 0x38c008), ==, 0);
    g_assert_cmphex(qtest_readq(s, FETCHOP_MACHINE_BASE + 0x38c008), ==, 1);
    g_assert_cmphex(qtest_readq(s, UINT64_C(0x38c000)), ==, 2);
    qtest_quit(s);
    unlink(prom);
}

/*
 * The memory ECC code's column table as the Origin 300's memory diagnostic
 * carries it (docs/datasheets/bedrock-memory-directory.md, "Memory ECC").
 * Kept here in full so the check is judged against the code, not against
 * whatever the model happens to compute.
 */
static const uint8_t ecc_column[72] = {
    0x13, 0x23, 0x43, 0x83, 0x2f, 0xf1, 0x0d, 0x07,
    0xd0, 0x70, 0x4f, 0xf8, 0x61, 0x62, 0x64, 0x68,
    0x1c, 0x2c, 0x4c, 0x8c, 0x15, 0x25, 0x45, 0x85,
    0x19, 0x29, 0x49, 0x89, 0x1a, 0x2a, 0x4a, 0x8a,
    0x51, 0x52, 0x54, 0x58, 0x91, 0x92, 0x94, 0x98,
    0xa1, 0xa2, 0xa4, 0xa8, 0x31, 0x32, 0x34, 0x38,
    0x16, 0x26, 0x46, 0x86, 0x1f, 0xf2, 0x0b, 0x0e,
    0xb0, 0xe0, 0x8f, 0xf4, 0xc1, 0xc2, 0xc4, 0xc8,
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

static uint8_t ecc_of(uint64_t data)
{
    uint8_t ecc = 0;

    for (unsigned bit = 0; bit < 64; bit++) {
        if (data & (UINT64_C(1) << bit)) {
            ecc ^= ecc_column[bit];
        }
    }
    return ecc;
}

/* The byte carrier of doubleword @index's ECC byte in software bank 0. */
#define ECC_BYTE(_index) ECC((_index) ^ 7)

/*
 * A doubleword whose backdoor-written ECC byte disagrees with its data is
 * checked on every read: a single-bit error is corrected and latched with
 * its syndrome and address, a check-bit error latched without changing the
 * data, and a double-bit error latched as uncorrectable. A read with no
 * cache attribute, this test's, completes with the word as stored, as a
 * processor's cached read does; the uncached read that fails is the
 * processor's (sn1-bedrock-ecc-cpu-test). A data write regenerates the ECC
 * and ends the checking; the ignore bit suspends it.
 */
static void test_ecc_check_on_read(void)
{
    const uint64_t data = UINT64_C(0x0123456789abcdef);
    const uint64_t address = 0x100;
    const unsigned index = address / 8;
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(address), data);
    /* The backdoor reads the code's own byte for data it never overrode. */
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==, ecc_of(data));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);

    /*
     * Data bit 5 flipped: the read returns the corrected word, latches the
     * syndrome and address, and scrubs the word and its check byte, so the
     * next read is clean and the backdoor shows the code's byte again.
     */
    qtest_writeb(s, ECC_BYTE(index), ecc_of(data) ^ ecc_column[5]);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==,
                    ecc_of(data) ^ ecc_column[5]);
    g_assert_cmphex(qtest_readl(s, MEMORY(address + 4)), ==,
                    (uint32_t)(data ^ (UINT64_C(1) << 5)));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[5]) | address);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==,
                    data ^ (UINT64_C(1) << 5));
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==,
                    ecc_of(data ^ (UINT64_C(1) << 5)));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[5]) | address);
    /* A second correctable error while the first is recorded: overrun. */
    qtest_writeb(s, ECC_BYTE(index + 1), ecc_column[0]);
    g_assert_cmphex(qtest_readb(s, MEMORY(address + 15)), ==, 1);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID | MEMORY_ERROR_READ_CE_OVERRUN |
                    MEMORY_ERROR_SYNDROME(ecc_column[5]) | address);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==, 0);
    qtest_writeq(s, MEMORY(address), data);

    /* The ignore bit returns the raw word, latches and scrubs nothing. */
    qtest_writeb(s, ECC_BYTE(index), ecc_of(data) ^ ecc_column[5]);
    qtest_writeq(s, MD(ECC_CONFIG), 1);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==, data);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==,
                    ecc_of(data) ^ ecc_column[5]);
    qtest_writeq(s, MD(ECC_CONFIG), 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==,
                    data ^ (UINT64_C(1) << 5));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[5]) | address);
    qtest_writeq(s, MEMORY(address), data);

    /* Check bit 4 flipped: the data is right, the syndrome names the bit. */
    qtest_writeb(s, ECC_BYTE(index), ecc_of(data) ^ ecc_column[64 + 4]);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==, data);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[64 + 4]) | address);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==, ecc_of(data));

    /* A write regenerates the ECC: no error, and the backdoor agrees. */
    qtest_writeb(s, ECC_BYTE(index), ecc_of(data) ^ ecc_column[9]);
    qtest_writeq(s, MEMORY(address), ~data);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==, ~data);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==, ecc_of(~data));

    /* A narrow write merges into the doubleword and regenerates as well. */
    qtest_writeb(s, ECC_BYTE(index), ecc_of(~data) ^ ecc_column[63]);
    qtest_writeb(s, MEMORY(address + 7), 0xa5);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==,
                    (~data & ~UINT64_C(0xff)) | 0xa5);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);

    /*
     * Two bits flipped: uncorrectable. The read completes with the word as
     * stored and latches the record; neither the word nor its check byte is
     * scrubbed, so the next read latches an overrun.
     */
    qtest_writeb(s, ECC_BYTE(index),
                 ecc_of((~data & ~UINT64_C(0xff)) | 0xa5) ^
                 ecc_column[1] ^ ecc_column[2]);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==,
                    (~data & ~UINT64_C(0xff)) | 0xa5);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_UCE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[1] ^ ecc_column[2]) |
                    address);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==,
                    ecc_of((~data & ~UINT64_C(0xff)) | 0xa5) ^
                    ecc_column[1] ^ ecc_column[2]);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==,
                    (~data & ~UINT64_C(0xff)) | 0xa5);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_UCE_VALID |
                    MEMORY_ERROR_READ_UCE_OVERRUN |
                    MEMORY_ERROR_SYNDROME(ecc_column[1] ^ ecc_column[2]) |
                    address);
    /* A correctable error after it keeps the uncorrectable record. */
    qtest_writeq(s, MEMORY(address + 8), 1);
    qtest_writeb(s, ECC_BYTE(index + 1), ecc_of(1) ^ ecc_column[0]);
    g_assert_cmphex(qtest_readq(s, MEMORY(address + 8)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_UCE_VALID |
                    MEMORY_ERROR_READ_UCE_OVERRUN |
                    MEMORY_ERROR_READ_CE_VALID | MEMORY_ERROR_READ_CE_OVERRUN |
                    MEMORY_ERROR_SYNDROME(ecc_column[1] ^ ecc_column[2]) |
                    address);
    /* The uncorrectable word is written over: clean again. */
    qtest_writeq(s, MEMORY(address), 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(address)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    /* Writes to the error registers change nothing. */
    qtest_writeq(s, MD(MEMORY_ERROR), UINT64_MAX);
    qtest_writeq(s, MD(MEMORY_ERROR_CLEAR), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    qtest_quit(s);
}

/*
 * The scrub carrier reads the same bytes from four eighths of the node and
 * an aligned doubleword store there scrubs the 64-byte block: the
 * corrections a read would make, latched the same way.
 */
static void test_ecc_scrub_carrier(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x1000), 0);
    qtest_writeq(s, MEMORY(0x1038), 0);
    qtest_writeb(s, ECC_BYTE(0x200), ecc_column[0]);
    qtest_writeb(s, ECC(0x200), ecc_column[3]);
    g_assert_cmphex(qtest_readb(s, ECC_SCRUB(0x200 ^ 7)), ==, ecc_column[0]);
    g_assert_cmphex(qtest_readw(s, ECC_SCRUB(0x200 ^ 6)), ==, ecc_column[0]);
    qtest_writeq(s, ECC_SCRUB(0x200), 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1000)), ==, 1);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x1038)), ==, 8);
    g_assert_cmphex(qtest_readb(s, ECC_SCRUB(0x200 ^ 7)), ==, ecc_of(1));
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(0x207)), ==, ecc_of(8));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID | MEMORY_ERROR_READ_CE_OVERRUN |
                    MEMORY_ERROR_SYNDROME(ecc_column[0]) | 0x1000);
    /* Rejected: a narrow or unaligned store, one outside fitted memory. */
    qtest_writel(s, ECC_SCRUB(0x200), 0);
    qtest_writeq(s, ECC_SCRUB(0x204), 0);
    qtest_writeq(s, ECC_SCRUB(ECC_FIRST_ABSENT_BYTE_512M & ~7), 0);
    qtest_writel(s, ECC_SCRUB(0x204), 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC scrub write offset 0x00000200 size 4 "
        "rejected: unsupported width"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC scrub write offset 0x00000204 size 8 "
        "rejected: unaligned doubleword"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC scrub write offset 0x04000000 size 8 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, "ECC scrub write"), ==, 4);
}

/* A halfword carrier write overrides two doublewords' bytes at once. */
static void test_ecc_halfword_injection(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    /* The low half is doubleword 0's byte, the high half doubleword 1's. */
    qtest_writew(s, ECC(6), (ecc_column[63] << 8) | ecc_column[0]);
    g_assert_cmphex(qtest_readq(s, MEMORY(0)), ==, 1);
    g_assert_cmphex(qtest_readq(s, MEMORY(8)), ==, UINT64_C(1) << 63);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID | MEMORY_ERROR_READ_CE_OVERRUN |
                    MEMORY_ERROR_SYNDROME(ecc_column[0]));
    /* Both words were scrubbed to the codewords the corrections made. */
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==,
                    (ecc_column[63] << 8) | ecc_column[0]);
    g_assert_cmphex(qtest_readq(s, MEMORY(0)), ==, 1);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    qtest_writeq(s, MEMORY(0), 0);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, ecc_column[63] << 8);
    qtest_quit(s);
}

/* The checked doubleword follows a bank-select relocation of its bank. */
static void test_ecc_check_follows_bank_select(void)
{
    QTestState *s = start_with_installed_memory("");

    qtest_writeq(s, MD(0x8), (UINT64_C(2) << 32) | memory_bank_config(2, 3) |
                              memory_bank_config(3, 3));
    qtest_writeq(s, MEMORY(0x1000), 0x30);
    qtest_writeb(s, ECC_BYTE(0x1000 / 8), ecc_of(0x30) ^ ecc_column[0]);
    qtest_writeq(s, MD(0x8), memory_bank_config(2, 3) |
                              memory_bank_config(3, 3));
    g_assert_cmphex(qtest_readq(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x1000)),
                    ==, 0x31);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[0]) |
                    (2 * MEMORY_BANK_SIZE + 0x1000));
    qtest_quit(s);
}

/* Cold reset ends every check and clears the error record. */
static void test_ecc_check_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x40), 2);
    qtest_writeb(s, ECC_BYTE(8), ecc_of(2) ^ ecc_column[0]);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x40)), ==, 2);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(8)), ==, ecc_of(2));
    qtest_quit(s);
}

/* A correctable error raises bank-1 bit 61 on both processor interfaces. */
static void test_parent_ecc_interrupt(void)
{
    QTestState *s = start_parent();

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 6, ECC_BASE);
    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, 0);
    qtest_writeq(s, MEMORY(0x80), 0);
    qtest_writeb(s, ECC_BYTE(0x10), ecc_column[7]);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x80)), ==, 0x80);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    /* Software clears it through the pending-modify register. */
    qtest_writeq(s, BEDROCK_BASE + PI_PENDING_MODIFY, (1 << 6) | 61);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, 0);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    qtest_quit(s);
}

/*
 * An uncorrectable error raises error-interrupt pending bit 24, memory
 * uncorrectable, on both processor interfaces, and each processor's error
 * line follows it through that processor's error mask. The bit is not the
 * writable view's to set or clear; a write of it to the pending register
 * clears it, on that interface alone.
 */
static void test_parent_ecc_uncorrectable_interrupt(void)
{
    QTestState *s = start_parent();

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 6, ECC_BASE);
    qtest_irq_intercept_out_named(s, "/machine/peripheral/bedrock",
                                  "hardware-irq");
    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, BEDROCK_BASE + PI_ERROR_MASK_A, MEMORY_UNCORRECTABLE);
    qtest_writeq(s, BEDROCK_BASE + PI1_OFFSET + PI_ERROR_MASK_B,
                 MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==, 0);
    g_assert_false(qtest_get_irq(s, ERROR_LINE(0, 0)));

    qtest_writeq(s, MEMORY(0x80), 0);
    qtest_writeb(s, ECC_BYTE(0x10), ecc_column[64] ^ ecc_column[65]);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x80)), ==, 0);
    g_assert_cmphex(qtest_readq(s, BEDROCK_MD(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_UCE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[64] ^ ecc_column[65]) |
                    0x80);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==,
                    MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET +
                                PI_ERROR_PENDING), ==, MEMORY_UNCORRECTABLE);
    /* The sMDK monitor polls the bit through the writable view. */
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_SET), ==,
                    MEMORY_UNCORRECTABLE);
    /* Interface 0's processor A and interface 1's processor B are masked in. */
    g_assert_true(qtest_get_irq(s, ERROR_LINE(0, 0)));
    g_assert_false(qtest_get_irq(s, ERROR_LINE(0, 1)));
    g_assert_false(qtest_get_irq(s, ERROR_LINE(1, 0)));
    g_assert_true(qtest_get_irq(s, ERROR_LINE(1, 1)));

    /* The writable view leaves the bit alone. */
    qtest_writeq(s, BEDROCK_BASE + PI_ERROR_SET, 0);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==,
                    MEMORY_UNCORRECTABLE);
    g_assert_true(qtest_get_irq(s, ERROR_LINE(0, 0)));
    /* Writing it back to the pending register clears that interface's. */
    qtest_writeq(s, BEDROCK_BASE + PI_ERROR_PENDING, MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==, 0);
    g_assert_false(qtest_get_irq(s, ERROR_LINE(0, 0)));
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET +
                                PI_ERROR_PENDING), ==, MEMORY_UNCORRECTABLE);
    g_assert_true(qtest_get_irq(s, ERROR_LINE(1, 1)));
    /* The writable view cannot raise it either. */
    qtest_writeq(s, BEDROCK_BASE + PI_ERROR_SET, MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==, 0);
    g_assert_false(qtest_get_irq(s, ERROR_LINE(0, 0)));
    qtest_quit(s);
}

/*
 * A directory error raises the memory errors' interrupts: a correctable one
 * pending bank 1 bit 61, an uncorrectable one error-interrupt bit 24, on
 * both processor interfaces. The Origin 300's directory diagnostic waits
 * for each and then reads the directory error register.
 */
static void test_parent_directory_ecc_interrupt(void)
{
    const uint64_t line = 0x2000;
    const uint64_t word = (line >> 3) & UINT64_C(0x3ffffff0);
    QTestState *s = start_parent();

    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, BEDROCK_MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), UINT64_C(0x0000001f00000000));
    qtest_writeq(s, BEDROCK_MD(DIRECTORY_CONFIG), 1);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, 0);
    qtest_readq(s, MEMORY(line));
    g_assert_cmphex(qtest_readq(s, BEDROCK_MD(DIRECTORY_ERROR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS(word));
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==, 0);

    /* Two data bits: uncorrectable. */
    qtest_writeq(s, BEDROCK_MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word + 0x10), 3);
    qtest_writeq(s, BEDROCK_MD(DIRECTORY_CONFIG), 1);
    qtest_readq(s, MEMORY(line + 0x80));
    g_assert_cmphex(qtest_readq(s, BEDROCK_MD(DIRECTORY_ERROR)) &
                    DIRECTORY_ERROR_UCE_VALID, ==, DIRECTORY_ERROR_UCE_VALID);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI_ERROR_PENDING), ==,
                    MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_BASE + PI1_OFFSET +
                                PI_ERROR_PENDING), ==, MEMORY_UNCORRECTABLE);
    qtest_quit(s);
}

/*
 * The Origin 300's memory diagnostic injects at physical 0x506000 through
 * the machine's ECC carrier and expects the corrected word, the error
 * record through the local register window and the interrupt. Its SECDED
 * verification then sweeps the uncorrectable syndromes the same way,
 * beginning with 0x03, reads the word uncached expecting a bus error, and
 * waits for the error interrupt whose handler reads the memory error
 * register; that interrupt is error pending bit 24, which its error mask
 * admits.
 */
static void test_origin300_ecc_check(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);
    const uint64_t address = 0x506000;
    const uint64_t carrier = UINT64_C(0x10140000000) + ((address / 8) ^ 7);

    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 (UINT64_C(2) << 32) | memory_bank_config(2, 3) |
                 memory_bank_config(3, 3));
    qtest_writeq(s, address, 0);
    g_assert_cmphex(qtest_readb(s, carrier), ==, 0);
    qtest_writeb(s, carrier, ecc_column[0]);
    g_assert_cmphex(qtest_readq(s, address), ==, 1);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[0]) | address);
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS + PI_PENDING_1) &
                    MEMORY_CORRECTABLE, ==, MEMORY_CORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS + PI1_OFFSET +
                                PI_PENDING_1) & MEMORY_CORRECTABLE, ==,
                    MEMORY_CORRECTABLE);
    /* The diagnostic's scrub store and check-byte read at four eighths. */
    qtest_writeq(s, UINT64_C(0x10100000000) + (address / 8), 0);
    g_assert_cmphex(qtest_readb(s, UINT64_C(0x10100000000) +
                                ((address / 8) ^ 7)), ==, ecc_of(1));
    qtest_writeq(s, address, 0);
    g_assert_cmphex(qtest_readq(s, address), ==, 0);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[0]) | address);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(MEMORY_ERROR)), ==, 0);

    /*
     * Syndrome 0x03: two check bits, uncorrectable, latched, interrupting.
     * A read with no cache attribute completes with the word as stored; the
     * uncached read the diagnostic makes, and its bus error, are the
     * processor's (sn1-bedrock-ecc-cpu-test).
     */
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS +
                                PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, 0);
    qtest_writeq(s, address, UINT64_C(0x0123456789abcdef));
    qtest_writeb(s, carrier, qtest_readb(s, carrier) ^ 0x03);
    g_assert_cmphex(qtest_readq(s, address), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(MEMORY_ERROR)), ==,
                    MEMORY_ERROR_READ_UCE_VALID | MEMORY_ERROR_SYNDROME(0x03) |
                    address);
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS +
                                PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, MEMORY_UNCORRECTABLE);
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS + PI1_OFFSET +
                                PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, MEMORY_UNCORRECTABLE);
    qtest_quit(s);
    unlink(prom);
}

/*
 * A self-test write phase with pattern zero fills its bank with zero, check
 * bytes included. The stock PROM runs one over each bank after its memory
 * test, which leaves backdoor check bytes that disagree with their data;
 * the Origin 300's cache-coherency and directory diagnostics later read
 * those doublewords without writing them first. The doubleword here is the
 * one the coherency diagnostic read, left the way the memory test leaves
 * it: data 0x0f0f..., check byte 0x81, with a directory non-codeword on its
 * line. A phase with another pattern leaves memory alone, the fill rewrites
 * the line's directory entry as any data write does, and it stays inside
 * the selected bank.
 */
static void test_origin300_selftest_fill(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);
    /*
     * Bank select 0, as the PROM has it for the test: selector 3 is
     * locational and software bank 3. The PROM later selects 2, which puts
     * this doubleword at 0x40800008, where the coherency diagnostic read it.
     */
    const uint64_t address = 3 * MEMORY_BANK_SIZE + 0x800008;
    const uint64_t carrier = ECC_BASE + ((address / 8) ^ 7);
    const uint64_t other_bank = 2 * MEMORY_BANK_SIZE + 0x506000;
    const uint64_t directory = PROTECTION_MACHINE_BASE +
                               3 * DIRECTORY_BANK_SIZE +
                               ((address & (MEMORY_BANK_SIZE - 1)) >> 3 &
                                ~UINT64_C(0xf));
    const uint64_t data = UINT64_C(0x0f0f0f0f0f0f0f0f);

    ensure_clock_running(s);
    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 memory_bank_config(2, 3) | memory_bank_config(3, 3));
    qtest_writeq(s, address, data);
    qtest_writeq(s, other_bank, UINT64_C(0x0123456789abcdef));
    qtest_writeb(s, carrier, 0x81);
    g_assert_cmphex(qtest_readb(s, carrier), ==, 0x81);
    /* A directory non-codeword on the line, stored with ECC ignored. */
    qtest_writeq(s, SELFTEST_MACHINE(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, directory, UINT64_C(0x0000001f00000000));
    qtest_writeq(s, SELFTEST_MACHINE(DIRECTORY_CONFIG), 1);

    /* A nonzero pattern's write phase leaves the mark and the data. */
    qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_DATA), 0xc5);
    qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_CONTROL), (3 << 4) | 1);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readb(s, carrier), ==, 0x81);

    /* The zero-pattern write phase clears the bank, mark and data. */
    qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_DATA), 0x80);
    qtest_writeq(s, SELFTEST_MACHINE(SELFTEST_CONTROL), (3 << 4) | 1);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    g_assert_cmphex(qtest_readb(s, carrier), ==, 0);
    g_assert_cmphex(qtest_readq(s, address), ==, 0);
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(MEMORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, BEDROCK_NODE_REGISTERS +
                                PI_ERROR_PENDING) & MEMORY_UNCORRECTABLE,
                    ==, 0);
    /* The line's directory entry was rewritten, so the read checks none. */
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(DIRECTORY_ERROR)), ==, 0);
    /* The fill stays in selector 3's bank; bank 2 keeps its data. */
    g_assert_cmphex(qtest_readq(s, other_bank), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
    unlink(prom);
}

/*
 * The directory word carries its own check field (premium: bits 37:32 and
 * 48; standard: 21:16). A backdoor write regenerates it unless directory
 * ECC is ignored, in which case a non-codeword is stored as written and the
 * next read of its memory line checks it: a single-bit error is corrected
 * in the stored word and latched with the syndrome and the word's Hub
 * special address, a double-bit error latched as uncorrectable and refused.
 * The syndromes are the Origin 300 directory diagnostic's own columns.
 */
static void test_directory_ecc_check(void)
{
    const uint64_t line = 0x2000;
    const uint64_t word = (line >> 3) & UINT64_C(0x3ffffff0);
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_writeq(s, DIRECTORY(word), 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==, 0);
    qtest_writeq(s, DIRECTORY(word), 1);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000001f00000001));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);

    /* Data bit 0 flipped: the read corrects the stored word and latches. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), UINT64_C(0x0000001f00000000));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000001f00000000));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_readq(s, MEMORY(line));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000001f00000001));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS(word));
    qtest_readq(s, MEMORY(line + 0x78));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS(word));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);

    /* Check bit 0 flipped: the word becomes the zero codeword. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), UINT64_C(1) << 32);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_readb(s, MEMORY(line + 1));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x01) |
                    DIRECTORY_ERROR_ADDRESS(word));

    /* With directory ECC ignored a read checks nothing. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), UINT64_C(0x0000001f00000000));
    qtest_readq(s, MEMORY(line));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000001f00000000));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_readq(s, MEMORY(line));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000001f00000001));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS(word));

    /* Two data bits: uncorrectable, refused, latched, left as stored. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), 3);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==,
                    DIRECTORY_ERROR_UCE_VALID | DIRECTORY_ERROR_SYNDROME(0x30) |
                    DIRECTORY_ERROR_ADDRESS(word));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==,
                    DIRECTORY_ERROR_UCE_VALID | DIRECTORY_ERROR_UCE_OVERRUN |
                    DIRECTORY_ERROR_SYNDROME(0x30) |
                    DIRECTORY_ERROR_ADDRESS(word));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==, 3);
    /* A correctable error elsewhere keeps the uncorrectable record. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word + 0x10), UINT64_C(0x0000001f00000000));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_readq(s, MEMORY(line + 0x80));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_UCE_VALID | DIRECTORY_ERROR_UCE_OVERRUN |
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_CE_OVERRUN |
                    DIRECTORY_ERROR_SYNDROME(0x30) |
                    DIRECTORY_ERROR_ADDRESS(word));
    /*
     * A data write inside the line rewrites its entry: the word keeps its
     * bits under a fresh check field, the data lands, and the next read is
     * clean. The stock PROM relies on this after its own directory walk.
     */
    qtest_writeq(s, MEMORY(line + 0x40), UINT64_C(0x1234));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000003000000003));
    g_assert_cmphex(qtest_readq(s, MEMORY(line + 0x40)), ==, UINT64_C(0x1234));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    /* Rewriting the word with ECC in force ends the checking as well. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), 3);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_writeq(s, DIRECTORY(word), 3);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000003000000003));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    qtest_writeq(s, MD(DIRECTORY_ERROR), UINT64_MAX);
    qtest_writeq(s, MD(DIRECTORY_ERROR_CLEAR), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);

    /* Standard flavor: six check bits at 21:16 over the low word. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 0);
    qtest_writeq(s, DIRECTORY(word + 0x20), 1);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word + 0x20)), ==, 0x001f0001);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 2);
    qtest_writeq(s, DIRECTORY(word + 0x20), 0x00010000);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 0);
    qtest_readq(s, MEMORY(line + 0x100));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word + 0x20)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x01) |
                    DIRECTORY_ERROR_ADDRESS(word + 0x20));
    qtest_quit(s);
}

/* Cold reset ends every directory check and clears the record. */
static void test_directory_ecc_check_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(0x400), UINT64_C(0x0000001f00000000));
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_readq(s, MEMORY(0x2000));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x400)), ==, 0);
    qtest_quit(s);
}

/*
 * A data write rewrites the entry of every marked line it touches whichever
 * path carries it: the doubleword overlay that shadows the line's own, the
 * decoder, and the unit's fetch-and-op stores, which regenerate the
 * doubleword's ECC the way any write does.
 */
static void test_directory_rewritten_by_data_write(void)
{
    const uint64_t line = 0x4000;
    const uint64_t word = (line >> 3) & UINT64_C(0x3ffffff0);
    const unsigned index = line / 8;
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));

    /* Through an ECC overlay on a doubleword of the line. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), 3);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_writeb(s, ECC_BYTE(index + 1), ecc_column[2]);
    qtest_writeq(s, MEMORY(line + 8), 0x55);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000003000000003));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MEMORY(line + 8)), ==, 0x55);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index + 1)), ==, ecc_of(0x55));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);

    /* Through a fetch-and-op store, which regenerates the ECC as well. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), 3);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    qtest_writeb(s, ECC_BYTE(index), ecc_column[4]);
    qtest_writeq(s, FETCHOP(line), 0x77);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000003000000003));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0x77);
    g_assert_cmphex(qtest_readb(s, ECC_BYTE(index)), ==, ecc_of(0x77));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR)), ==, 0);

    /* A fetch-and-increment writes too. */
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, DIRECTORY(word), 3);
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    g_assert_cmphex(qtest_readq(s, FETCHOP(line + 8)), ==, 0x77);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(word)), ==,
                    UINT64_C(0x0000003000000003));
    g_assert_cmphex(qtest_readq(s, MEMORY(line)), ==, 0x78);
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR)), ==, 0);
    qtest_quit(s);
}

/*
 * The stock PROM's memory test holds 259 checked doublewords and 45 checked
 * lines at once on a two-bank node. A marked set bounded at 128 or 32 drops
 * the writes past its bound, and the PROM then reads the code's bytes where
 * its own patterns should be and fails the bank.
 */
static void test_ecc_check_many(void)
{
    const unsigned doublewords = 1024;
    const unsigned lines = 256;
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    for (unsigned i = 0; i < doublewords; i++) {
        qtest_writeb(s, ECC_BYTE(i), ecc_column[i % 64]);
    }
    for (unsigned i = 0; i < doublewords; i++) {
        g_assert_cmphex(qtest_readb(s, ECC_BYTE(i)), ==, ecc_column[i % 64]);
    }
    g_assert_cmphex(qtest_readq(s, MEMORY((doublewords - 1) * 8)), ==,
                    UINT64_C(1) << ((doublewords - 1) % 64));
    g_assert_cmphex(qtest_readq(s, MD(MEMORY_ERROR_CLEAR)), ==,
                    MEMORY_ERROR_READ_CE_VALID |
                    MEMORY_ERROR_SYNDROME(ecc_column[(doublewords - 1) % 64]) |
                    (doublewords - 1) * 8);

    qtest_writeq(s, MD(DIRECTORY_CONFIG), 3);
    for (unsigned i = 0; i < lines; i++) {
        qtest_writeq(s, DIRECTORY(i * 0x10), UINT64_C(0x0000001f00000000));
    }
    qtest_writeq(s, MD(DIRECTORY_CONFIG), 1);
    for (unsigned i = 0; i < lines; i++) {
        g_assert_cmphex(qtest_readq(s, DIRECTORY(i * 0x10)), ==,
                        UINT64_C(0x0000001f00000000));
    }
    qtest_readq(s, MEMORY((lines - 1) * 128));
    g_assert_cmphex(qtest_readq(s, DIRECTORY((lines - 1) * 0x10)), ==,
                    UINT64_C(0x0000001f00000001));
    g_assert_cmphex(qtest_readq(s, MD(DIRECTORY_ERROR_CLEAR)), ==,
                    DIRECTORY_ERROR_CE_VALID | DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS((lines - 1) * 0x10));
    qtest_quit(s);
}

/*
 * The Origin 300's directory diagnostic injects at the word for physical
 * 0x506000 through the machine's directory carrier and expects the
 * corrected word and the error record through the local window.
 */
static void test_origin300_directory_ecc_check(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S", quoted_prom);
    const uint64_t line = 0x506000;
    const uint64_t word = (line >> 3) & UINT64_C(0x3ffffff0);
    const uint64_t carrier = PROTECTION_MACHINE_BASE + word;

    qtest_writeq(s, SELFTEST_MACHINE(0x8),
                 (UINT64_C(2) << 32) | memory_bank_config(2, 3) |
                 memory_bank_config(3, 3));
    qtest_writeq(s, line, UINT64_C(0xabcd));
    qtest_writeq(s, SELFTEST_MACHINE(DIRECTORY_CONFIG), 3);
    qtest_writeq(s, carrier, UINT64_C(0x0000001f00000000));
    qtest_writeq(s, SELFTEST_MACHINE(DIRECTORY_CONFIG), 1);
    g_assert_cmphex(qtest_readq(s, line), ==, UINT64_C(0xabcd));
    g_assert_cmphex(qtest_readq(s, carrier), ==,
                    UINT64_C(0x0000001f00000001));
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(DIRECTORY_ERROR_CLEAR)),
                    ==, DIRECTORY_ERROR_CE_VALID |
                    DIRECTORY_ERROR_SYNDROME(0x1f) |
                    DIRECTORY_ERROR_ADDRESS(word));
    g_assert_cmphex(qtest_readq(s, SELFTEST_MACHINE(DIRECTORY_ERROR)), ==,
                    0);
    qtest_quit(s);
    unlink(prom);
}

/* Bedrock ECC stores as issued by the Origin 300 processor. */
#define HUB_ECC (SPACE_HUB + UINT64_C(0x140000000))
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define MD_MEMORY_CONFIG UINT64_C(0x780008)
/*
 * Origin 300's installed memory occupies locational banks 2 and 3, not
 * bank 0: map software bank 0 onto them so the guest's low-memory
 * coordination state (its completion flag included) lands in backed RAM
 * instead of an I/O hole.
 */
#define MD_INSTALLED_SELECT2 ((UINT64_C(2) << 32) | UINT64_C(0x3300))

static void emit(uint8_t *image, unsigned *offset, uint32_t instruction)
{
    stl_be_p(image + *offset, instruction);
    *offset += 4;
}

static void emit_load64(uint8_t *image, unsigned *offset, unsigned reg,
                        uint64_t value)
{
    emit(image, offset, 0x34000000 | reg << 16 | (value >> 48));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         ((value >> 32) & 0xffff));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         ((value >> 16) & 0xffff));
    emit(image, offset, reg << 16 | reg << 11 | 16 << 6 | 0x38);
    emit(image, offset, 0x34000000 | reg << 21 | reg << 16 |
         (value & 0xffff));
}

static char *create_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned offset = 0x380;
    int fd;

    stl_be_p(image, 0x100000ff); /* beq zero, zero, 0x400 */

    emit_load64(image, &offset, 27, UINT64_C(0x9000000000000110));
    emit(image, &offset, 0x401a6800); /* mfc0 k0, Cause */
    emit(image, &offset, 0xaf7a0000); /* sw k0, 0(k1) */
    emit(image, &offset, 0x401a7000); /* mfc0 k0, EPC */
    emit(image, &offset, 0x275a0004); /* addiu k0, k0, 4 */
    emit(image, &offset, 0x409a7000); /* mtc0 k0, EPC */
    emit(image, &offset, 0x42000018); /* eret */
    emit(image, &offset, 0);          /* nop */

    offset = 0x400;
    emit(image, &offset, 0x400c6000); /* mfc0 t4, Status */
    emit(image, &offset, 0x358c0080); /* ori t4, t4, Status.KX */
    emit(image, &offset, 0x240dfffb); /* addiu t5, zero, ~Status.ERL */
    emit(image, &offset, 0x018d6024); /* and t4, t4, t5 */
    emit(image, &offset, 0x408c6000); /* mtc0 t4, Status */
    emit(image, &offset, 0x000000c0); /* ehb */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000140000000));
    emit_load64(image, &offset, 1, UINT64_C(0x1111222233334444));
    emit(image, &offset, 0xa5010006); /* sh at, 6(t0) */
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677778888));
    emit(image, &offset, 0xa5010004); /* sh at, 4(t0) */
    emit_load64(image, &offset, 1, UINT64_C(0x9999aaaabbbbcccc));
    emit(image, &offset, 0xa5010002); /* sh at, 2(t0) */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677770000));
    emit(image, &offset, 0xa5010000); /* sh at, 0(t0) */
    emit_load64(image, &offset, 8, UINT64_C(1));
    emit(image, &offset, 0xa5010000); /* unaligned sh at, 0(t0) */
    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit(image, &offset, 0x340b0001); /* ori t3, zero, 1 */
    emit(image, &offset, 0xfd0b0008); /* sd t3, 8(t0) */
    emit(image, &offset, 0x1000ffff); /* beq zero, zero, self */
    emit(image, &offset, 0);          /* nop */

    fd = g_file_open_tmp("sn1-bedrock-ecc-cpu-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_cpu_halfword_stores(void)
{
    g_autofree char *prom = create_prom();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=single -no-shutdown -S", quoted);
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned tries = 0; tries < 200 && !qtest_readq(s, 0x108); tries++) {
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(s, 0x108), ==, 1);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0), ==, 0);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 2), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 4), ==, 0x5555);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 6), ==, 0x4444);
    g_assert_cmphex(qtest_readw(s, 0x100), ==, 0);
    g_assert_cmphex(qtest_readl(s, 0x110) & 0x7c, ==, 5 << 2);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

/*
 * The stock PROM's walking-address memory test reads a positioned check word
 * back from the very top of the fitted bank. Repeat all four residues at the
 * last fitted 64-byte line of installed memory. Its residue 0x30 (ECC
 * carrier offset 0) selects source bits 31:16, the slice the first-line
 * cases above never exercise.
 *
 * Origin 300's installed memory occupies locational banks 2 and 3, 256 MiB
 * each (see MD_INSTALLED_SELECT2 above), not one 512 MiB bank at slot 0.
 * The last fitted line sits in the second (higher) of those two banks:
 * software bank 1, which the 2-selector routes to locational bank 3, at
 * device offset 0x9fffff8 -- that bank's own last 64-byte line, 0x1fffff8,
 * past its dense base of 0x2000000 (bank 2's own 0x2000000 ECC bytes).
 */
static char *create_prom_top_line(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    unsigned offset = 0x380;
    int fd;

    stl_be_p(image, 0x100000ff); /* beq zero, zero, 0x400 */

    emit_load64(image, &offset, 27, UINT64_C(0x9000000000000110));
    emit(image, &offset, 0x401a6800); /* mfc0 k0, Cause */
    emit(image, &offset, 0xaf7a0000); /* sw k0, 0(k1) */
    emit(image, &offset, 0x401a7000); /* mfc0 k0, EPC */
    emit(image, &offset, 0x275a0004); /* addiu k0, k0, 4 */
    emit(image, &offset, 0x409a7000); /* mtc0 k0, EPC */
    emit(image, &offset, 0x42000018); /* eret */
    emit(image, &offset, 0);          /* nop */

    offset = 0x400;
    emit(image, &offset, 0x400c6000); /* mfc0 t4, Status */
    emit(image, &offset, 0x358c0080); /* ori t4, t4, Status.KX */
    emit(image, &offset, 0x240dfffb); /* addiu t5, zero, ~Status.ERL */
    emit(image, &offset, 0x018d6024); /* and t4, t4, t5 */
    emit(image, &offset, 0x408c6000); /* mtc0 t4, Status */
    emit(image, &offset, 0x000000c0); /* ehb */

    /*
     * t0 = xkphys view of the last fitted ECC carrier word (device
     * 0x9fffff8).
     */
    emit_load64(image, &offset, 8, UINT64_C(0x9000000149fffff8));
    emit_load64(image, &offset, 1, UINT64_C(0x1111222233334444));
    emit(image, &offset, 0xa5010006); /* sh at, 6(t0)  residue 0x00 -> 0x4444 */
    emit_load64(image, &offset, 1, UINT64_C(0x5555666677778888));
    emit(image, &offset, 0xa5010004); /* sh at, 4(t0)  residue 0x10 -> 0x5555 */
    emit_load64(image, &offset, 1, UINT64_C(0x9999aaaabbbbcccc));
    emit(image, &offset, 0xa5010002); /* sh at, 2(t0)  residue 0x20 -> 0xaaaa */
    emit_load64(image, &offset, 1, UINT64_C(0xddddeeee13572468));
    emit(image, &offset, 0xa5010000); /* sh at, 0(t0)  residue 0x30 -> 0x1357 */

    emit_load64(image, &offset, 8, UINT64_C(0x9000000000000100));
    emit(image, &offset, 0x340b0001); /* ori t3, zero, 1 */
    emit(image, &offset, 0xfd0b0008); /* sd t3, 8(t0) */
    emit(image, &offset, 0x1000ffff); /* beq zero, zero, self */
    emit(image, &offset, 0);          /* nop */

    fd = g_file_open_tmp("sn1-bedrock-ecc-cpu-top-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_cpu_halfword_stores_top_line(void)
{
    g_autofree char *prom = create_prom_top_line();
    g_autofree char *quoted = g_shell_quote(prom);
    QTestState *s;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    s = qtest_initf("-M origin300 -cpu R14000 -smp 2 -bios %s "
                    "-accel tcg,thread=single -no-shutdown -S", quoted);
    qtest_writeq(s, BEDROCK_NODE0 + MD_MEMORY_CONFIG, MD_INSTALLED_SELECT2);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");

    for (unsigned tries = 0; tries < 200 && !qtest_readq(s, 0x108); tries++) {
        g_usleep(10000);
    }
    g_assert_cmphex(qtest_readq(s, 0x108), ==, 1);

    /*
     * Each residue keeps its own positioned check word at the top
     * fitted line.
     */
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffe), ==, 0x4444);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffc), ==, 0x5555);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffffa), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffff8), ==, 0x1357);
    /*
     * Big-endian byte view of the residue 0x30 word: the halfword carrier
     * keeps the even doubleword's byte (0x9fffffe, at byte carrier
     * 0x9fffffe ^ 7) in its low half.
     */
    g_assert_cmphex(qtest_readb(s, HUB_ECC + 0x9fffff8), ==, 0x13);
    g_assert_cmphex(qtest_readb(s, HUB_ECC + 0x9fffff9), ==, 0x57);
    /* The adjacent lower carrier word was never written. */
    g_assert_cmphex(qtest_readw(s, HUB_ECC + 0x9fffff0), ==, 0);

    qtest_quit(s);
    g_assert_cmpint(unlink(prom), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-md/directory-ecc-check",
                   test_directory_ecc_check);
    qtest_add_func("sn1/bedrock-md/directory-ecc-check-reset",
                   test_directory_ecc_check_reset);
    qtest_add_func("sn1/bedrock-md/directory-rewritten-by-data-write",
                   test_directory_rewritten_by_data_write);
    qtest_add_func("sn1/bedrock-md/ecc-check-many", test_ecc_check_many);
    qtest_add_func("sn1/bedrock-md/origin300-directory-ecc-check",
                   test_origin300_directory_ecc_check);
    qtest_add_func("sn1/bedrock-md/ecc-check-on-read",
                   test_ecc_check_on_read);
    qtest_add_func("sn1/bedrock-md/ecc-halfword-injection",
                   test_ecc_halfword_injection);
    qtest_add_func("sn1/bedrock-md/ecc-scrub-carrier", test_ecc_scrub_carrier);
    qtest_add_func("sn1/bedrock-md/ecc-check-follows-bank-select",
                   test_ecc_check_follows_bank_select);
    qtest_add_func("sn1/bedrock-md/ecc-check-reset", test_ecc_check_reset);
    qtest_add_func("sn1/bedrock-md/parent-ecc-interrupt",
                   test_parent_ecc_interrupt);
    qtest_add_func("sn1/bedrock-md/parent-ecc-uncorrectable-interrupt",
                   test_parent_ecc_uncorrectable_interrupt);
    qtest_add_func("sn1/bedrock-md/parent-directory-ecc-interrupt",
                   test_parent_directory_ecc_interrupt);
    qtest_add_func("sn1/bedrock-md/origin300-ecc-check",
                   test_origin300_ecc_check);
    qtest_add_func("sn1/bedrock-md/origin300-selftest-fill",
                   test_origin300_selftest_fill);
    qtest_add_func("sn1/bedrock-md/fetchop-operations",
                   test_fetchop_operations);
    qtest_add_func("sn1/bedrock-md/fetchop-rejections",
                   test_fetchop_rejections);
    qtest_add_func("sn1/bedrock-md/parent-fetchop-export",
                   test_parent_exports_fetchop);
    qtest_add_func("sn1/bedrock-md/origin300-fetchop",
                   test_origin300_fetchop);
    qtest_add_func("sn1/bedrock-md/register-contract", test_register_contract);
    qtest_add_func("sn1/bedrock-md/selftest-phases", test_selftest_phases);
    qtest_add_func("sn1/bedrock-md/selftest-completion-without-timer",
                   test_selftest_completion_without_timer);
    qtest_add_func("sn1/bedrock-md/selftest-bank-remap",
                   test_selftest_bank_remap);
    qtest_add_func("sn1/bedrock-md/selftest-negative-carriers",
                   test_selftest_negative_carriers);
    qtest_add_func("sn1/bedrock-md/selftest-origin300-installed-population",
                   test_selftest_origin300_installed_population);
    qtest_add_func("sn1/bedrock-md/selftest-replacement",
                   test_selftest_replacement);
    qtest_add_func("sn1/bedrock-md/selftest-cancellation",
                   test_selftest_cancellation);
    qtest_add_func("sn1/bedrock-md/selftest-unsupported-writes",
                   test_selftest_unsupported_writes);
    qtest_add_func("sn1/bedrock-md/selftest-read-only-results",
                   test_selftest_read_only_results);
    qtest_add_func("sn1/bedrock-md/selftest-access-width-policy",
                   test_selftest_access_width_policy);
    qtest_add_func("sn1/bedrock-md/selftest-migration-running",
                   test_selftest_migration_running);
    qtest_add_func("sn1/bedrock-md/memory-banks", test_memory_banks);
    qtest_add_func("sn1/bedrock-md/memory-installed-population",
                   test_memory_installed_population);
    qtest_add_func("sn1/bedrock-md/origin300-memory-aperture",
                   test_origin300_memory_aperture);
    qtest_add_func("sn1/bedrock-md/backdoors-installed-population",
                   test_backdoors_installed_population);
    qtest_add_func("sn1/bedrock-md/directory-bank-translation",
                   test_directory_bank_translation);
    qtest_add_func("sn1/bedrock-md/memory-size-boundaries",
                   test_memory_size_boundaries);
    qtest_add_func("sn1/bedrock-md/memory-absent-banks",
                   test_memory_absent_banks);
    qtest_add_func("sn1/bedrock-md/memory-crossing-does-not-corrupt",
                   test_memory_crossing_does_not_corrupt);
    qtest_add_func("sn1/bedrock-md/memory-selector-and-backing-bound",
                   test_memory_selector_and_backing_bound);
    qtest_add_func("sn1/bedrock-md/memory-selector-high-bank",
                   test_memory_selector_high_bank);
    qtest_add_func("sn1/bedrock-md/memory-access-widths-and-byte-order",
                   test_memory_access_widths_and_byte_order);
    qtest_add_func("sn1/bedrock-md/memory-reset-decode",
                   test_memory_reset_decode);
    qtest_add_func("sn1/bedrock-md/memory-without-node-memory",
                   test_memory_without_node_memory);
    qtest_add_func("sn1/bedrock-md/parent-memory-export",
                   test_parent_exports_memory);
    qtest_add_func("sn1/bedrock-md/access-width-policy",
                   test_access_width_policy);
    qtest_add_func("sn1/bedrock-md/directory-storage",
                   test_directory_storage);
    qtest_add_func("sn1/bedrock-md/directory-compatibility-storage",
                   test_directory_compatibility_storage);
    qtest_add_func("sn1/bedrock-md/directory-compatibility-capacity",
                   test_directory_compatibility_capacity);
    qtest_add_func("sn1/bedrock-md/directory-installed-absent-bank-compat",
                   test_directory_installed_absent_bank_compat);
    qtest_add_func("sn1/bedrock-md/protection-storage",
                   test_protection_storage);
    qtest_add_func("sn1/bedrock-md/protection-capacity-probe",
                   test_protection_capacity_probe);
    qtest_add_func("sn1/bedrock-md/directory-access-policy",
                   test_directory_access_policy);
    qtest_add_func("sn1/bedrock-md/directory-fitted-memory",
                   test_directory_fitted_memory);
    qtest_add_func("sn1/bedrock-md/parent-directory-export",
                   test_parent_exports_directory);
    qtest_add_func("sn1/bedrock-md/without-node-memory",
                   test_without_node_memory);
    qtest_add_func("sn1/bedrock-md/explicit-registers",
                   test_explicit_registers_independent);
    qtest_add_func("sn1/bedrock-md/queue-sizes", test_queue_sizes_fixed);
    qtest_add_func("sn1/bedrock-md/error-cluster-idle",
                   test_error_cluster_idle);
    qtest_add_func("sn1/bedrock-md/policy-zero-writes",
                   test_policy_zero_ignores_writes);
    qtest_add_func("sn1/bedrock-md/refresh-mask", test_refresh_mask);
    qtest_add_func("sn1/bedrock-md/perf-counter-lock-word",
                   test_perf_counter_lock_word);
    qtest_add_func("sn1/bedrock-md/reset", test_reset);
    qtest_add_func("sn1/bedrock-md/migration", test_migration);
    qtest_add_func("sn1/bedrock-md/memory-installed-migration",
                   test_memory_installed_migration);
    qtest_add_func("sn1/bedrock-md/writable-masks", test_writable_masks);
    qtest_add_func("sn1/bedrock-md/ecc-carrier", test_ecc_carrier);
    qtest_add_func("sn1/bedrock-md/ecc-bank-translation",
                   test_ecc_bank_translation);
    qtest_add_func("sn1/bedrock-md/ecc-validation-order",
                   test_ecc_validation_order);
    qtest_add_func("sn1/bedrock-md/ecc-fitted-memory", test_ecc_fitted_memory);
    qtest_add_func("sn1/bedrock-md/ecc-without-node-memory",
                   test_ecc_without_node_memory);
    qtest_add_func("sn1/bedrock-md/ecc-reset", test_ecc_reset);
    qtest_add_func("sn1/bedrock-md/ecc-migration", test_ecc_migration);
    qtest_add_func("sn1/bedrock-md/parent-ecc-export",
                   test_parent_exports_ecc);
    qtest_add_func("sn1/bedrock-ecc/cpu-halfword-stores",
                   test_cpu_halfword_stores);
    qtest_add_func("sn1/bedrock-ecc/cpu-halfword-stores-top-line",
                   test_cpu_halfword_stores_top_line);
    return g_test_run();
}
