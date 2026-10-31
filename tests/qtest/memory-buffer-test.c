/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Contract of the optional MemoryRegionOps buffer callbacks, driven
 * through memory-buffer-testdev: qtest memread/memwrite reach the device
 * as address_space_read/write of the whole length, so the core decides
 * per step whether to offer a buffer.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "exec/memattrs.h"

#define STORE_ADDR      0x10000
#define STATUS_ADDR     0x20000
#define DEVICE_PATH     "/machine/peripheral/buf"

#define ST_SCALAR       (STATUS_ADDR + 0x00)
#define ST_BUFFER       (STATUS_ADDR + 0x08)
#define ST_RESULT       (STATUS_ADDR + 0x10)
#define ST_OFFERED      (STATUS_ADDR + 0x18)
#define ST_MAP_READ     (STATUS_ADDR + 0x20)
#define ST_MAP_WRITE    (STATUS_ADDR + 0x28)
#define ST_MAPPED_SUM   (STATUS_ADDR + 0x30)
#define ST_WRITE        (STATUS_ADDR + 0x38)
#define ST_TRANSACTION  (STATUS_ADDR + 0x40)
#define ST_READ         (STATUS_ADDR + 0x48)
#define ST_TARGET       (STATUS_ADDR + 0x50)
#define ST_ONLY_WRITE   (STATUS_ADDR + 0x58)
#define ST_MOVED        (STATUS_ADDR + 0x60)
#define ST_ONLY_READ    (STATUS_ADDR + 0x68)
#define ST_MOVED_SUM    (STATUS_ADDR + 0x70)
#define ST_RAM_DIRTY    (STATUS_ADDR + 0x78)

/* The device's RAM sits right below its store, so one range spans both. */
#define RAM_ADDR        (STORE_ADDR - 256)

/* qtest memory commands go through the first CPU's address space. */
static const struct {
    const char *arch;
    const char *cpu;
} cpu_for_arch[] = {
    { "aarch64", "cortex-a57" },
    { "arm", "cortex-a15" },
    { "i386", "qemu32" },
    { "x86_64", "qemu64" },
    { "mips", "4Kc" },
    { "mipsel", "I7200" },
    { "mips64", "20Kc" },
    { "mips64el", "I6500" },
    { "ppc64", "power11_v2.0" },
    { "riscv64", "rv64" },
    { "s390x", "qemu" },
    { "loongarch64", "la464" },
};

static const char *test_cpu;

static QTestState *start(const char *props, const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu %s"
                                  " -device memory-buffer-testdev,id=buf%s %s",
                                  test_cpu, props, extra);

    qtest_sysbus_mmio_map(qts, DEVICE_PATH, 0, STORE_ADDR);
    qtest_sysbus_mmio_map(qts, DEVICE_PATH, 1, STATUS_ADDR);
    return qts;
}

static void clear_counts(QTestState *qts)
{
    qtest_writeq(qts, ST_SCALAR, 0);
}

static void fill_pattern(uint8_t *p, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++) {
        p[i] = i * 37 + seed;
    }
}

/* A long access is one buffer call each way, bytes in address order. */
static void test_raw_bytes(void)
{
    QTestState *qts = start("", "");
    uint8_t in[64], out[64];

    fill_pattern(in, sizeof(in), 19);
    qtest_memwrite(qts, STORE_ADDR + 3, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR + 3, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, sizeof(out));
    qtest_quit(qts);
}

/* Eight bytes or fewer, and CPU-sized accesses, never see a buffer. */
static void test_short_steps_are_narrow(void)
{
    QTestState *qts = start("", "");
    uint8_t in[8], out[8];

    fill_pattern(in, sizeof(in), 5);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    qtest_writeq(qts, STORE_ADDR + 8, 0x1928374655aabbccULL);
    g_assert_cmphex(qtest_readq(qts, STORE_ADDR + 8), ==,
                    0x1928374655aabbccULL);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 4);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 0);
    qtest_quit(qts);
}

/*
 * Without the callback, with a declining one, or on a region with
 * valid.accepts or an ioeventfd, the access is the narrow sequence: eight
 * 8-byte steps each way for 64 aligned bytes.
 */
static void check_narrow(const char *props, uint64_t buffer_calls)
{
    QTestState *qts = start(props, "");
    uint8_t in[64], out[64];

    fill_pattern(in, sizeof(in), 0x5c);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 16);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, buffer_calls);
    qtest_quit(qts);
}

static void test_no_callback(void)
{
    check_narrow(",mode=1", 0);
}

/*
 * Each step longer than eight bytes is offered again after a decline:
 * seven of the eight steps each way.
 */
static void test_declined(void)
{
    check_narrow(",mode=2", 14);
}

static void test_accepts_is_never_offered(void)
{
    check_narrow(",accepts=on", 0);
}

static void test_ioeventfd_is_never_offered(void)
{
    check_narrow(",eventfd=on", 0);
}

/*
 * A partial prefix is honoured and the rest continues: 16 bytes by buffer,
 * then the remaining 49 bytes are offered again, consumed 16 at a time,
 * leaving a single byte that is too short to offer.
 */
static void test_partial_prefix(void)
{
    QTestState *qts = start(",mode=3", "");
    uint8_t in[65], out[65];

    fill_pattern(in, sizeof(in), 23);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 8);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 2);
    qtest_quit(qts);
}

/*
 * The device's reentrancy guard covers the buffer callback: re-entering
 * the same device from inside it is refused, and the guard is released
 * afterwards.
 */
static void test_reentrancy_guard(void)
{
    QTestState *qts = start(",mode=4", "");
    uint8_t out[64];

    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, 0);
    }
    g_assert_cmpuint(qtest_readq(qts, ST_RESULT), ==, MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    qtest_writeq(qts, STORE_ADDR, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readq(qts, STORE_ADDR), ==, 0x1122334455667788ULL);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 2);
    qtest_quit(qts);
}

/* An error result on a consumed prefix stops nothing else. */
static void test_error_result(void)
{
    QTestState *qts = start(",mode=5", "");
    uint8_t out[32];

    memset(out, 0xff, sizeof(out));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, 0);
    }
    g_assert_cmpuint(qtest_readq(qts, ST_RESULT), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    qtest_quit(qts);
}

/*
 * The result a buffer callback reports reaches the caller of the whole
 * transaction, each way: the device's internal reads and writes record
 * what address_space_read() and address_space_write() returned.
 */
static void test_transaction_result(void)
{
    QTestState *qts = start(",mode=5", "");

    qtest_writeq(qts, ST_WRITE, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_ERROR);
    qtest_writeq(qts, ST_READ, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_ERROR);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 4);
    qtest_quit(qts);

    qts = start("", "");
    qtest_writeq(qts, ST_WRITE, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_OK);
    qtest_writeq(qts, ST_READ, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    qtest_quit(qts);
}

/*
 * The offer ends at the region's end even though translation does not
 * clamp an MMIO length: a read running 16 bytes past a 256-byte region is
 * offered 16 bytes, and the unassigned rest reads as zero.
 */
static void test_offer_ends_at_region_end(void)
{
    QTestState *qts = start("", "");
    uint8_t in[16], out[32];

    memset(in, 0x7b, sizeof(in));
    qtest_memwrite(qts, STORE_ADDR + 240, in, sizeof(in));
    clear_counts(qts);
    qtest_memread(qts, STORE_ADDR + 240, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(in));
    for (size_t i = sizeof(in); i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, 0);
    }
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, sizeof(in));
    qtest_quit(qts);
}

/*
 * A region mapped later over the middle takes precedence and ends the
 * section: the region below is offered the 32 bytes before it and the 64
 * after it, and the overlay (no callback) takes four 8-byte steps.
 */
static void test_offer_ends_at_section_end(void)
{
    QTestState *qts = start("",
                            "-device memory-buffer-testdev,id=over,"
                            "size=32,mode=1");
    uint8_t in[128], out[128];

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/over", 0,
                          STORE_ADDR + 32);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/over", 1,
                          STATUS_ADDR + 0x1000);
    fill_pattern(in, sizeof(in), 7);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 4);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, 64);
    g_assert_cmpuint(qtest_readq(qts, STATUS_ADDR + 0x1000), ==, 8);
    qtest_quit(qts);
}

/*
 * The offer is bounded by the section, but narrow dispatch keeps the
 * widths it has without the callback: after the 13-byte offer is declined,
 * a 16-byte access to a 13-byte region is two 8-byte steps, the second
 * running past the region's end as it always has.
 */
static void test_declined_keeps_narrow_widths(void)
{
    QTestState *qts = start(",size=13,mode=2", "");
    uint8_t in[16], out[16];

    memset(in, 0x39, sizeof(in));
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 4);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, 13);
    qtest_quit(qts);
}

/* address_space_map's bounce buffer reaches the callback both ways. */
static void test_bounce_buffer(void)
{
    QTestState *qts = start("", "");
    uint8_t in[48], out[48];
    uint64_t sum = 0;

    fill_pattern(in, sizeof(in), 3);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    for (size_t i = 0; i < sizeof(in); i++) {
        sum += in[i];
    }
    clear_counts(qts);
    qtest_writeq(qts, ST_MAP_READ, sizeof(in));
    g_assert_cmpuint(qtest_readq(qts, ST_RESULT), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq(qts, ST_MAPPED_SUM), ==, sum);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, sizeof(in));

    qtest_writeq(qts, ST_MAP_WRITE, sizeof(out));
    g_assert_cmpuint(qtest_readq(qts, ST_RESULT), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    clear_counts(qts);
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); i++) {
        g_assert_cmphex(out[i], ==, (uint8_t)(0xc0 + i));
    }
    qtest_quit(qts);
}

static QTestState *start_with_ram(const char *props, const char *extra)
{
    QTestState *qts = start(props, extra);

    qtest_sysbus_mmio_map(qts, DEVICE_PATH, 2, RAM_ADDR);
    return qts;
}

static uint64_t pattern_sum(size_t len)
{
    uint64_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += (uint8_t)(0xa0 + i);
    }
    return sum;
}

static void check_pattern(QTestState *qts, uint64_t addr, size_t len,
                          size_t first)
{
    uint8_t out[256];

    g_assert_cmpuint(len, <=, sizeof(out));
    qtest_memread(qts, addr, out, len);
    for (size_t i = 0; i < len; i++) {
        g_assert_cmphex(out[i], ==, (uint8_t)(0xa0 + first + i));
    }
}

/*
 * A buffer-only transfer into RAM is a copy that keeps dirty tracking: the
 * write marks the RAM dirty and the read does not.
 */
static void test_buffer_only_ram(void)
{
    QTestState *qts = start_with_ram("", "");

    qtest_readq(qts, ST_RAM_DIRTY);
    g_assert_cmpuint(qtest_readq(qts, ST_RAM_DIRTY), ==, 0);
    qtest_writeq(qts, ST_TARGET, RAM_ADDR);
    qtest_writeq(qts, ST_ONLY_WRITE, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq(qts, ST_RAM_DIRTY), ==, 1);
    qtest_writeq(qts, ST_ONLY_READ, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED_SUM), ==, pattern_sum(64));
    g_assert_cmpuint(qtest_readq(qts, ST_RAM_DIRTY), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 0);
    check_pattern(qts, RAM_ADDR, 64, 0);
    qtest_quit(qts);
}

/*
 * It stops where an ordinary access would be needed and makes none there:
 * the RAM half of a range moves, the store without callbacks sees no access
 * and keeps its bytes.
 */
static void test_buffer_only_stops_before_ordinary_region(void)
{
    QTestState *qts = start_with_ram(",mode=1", "");
    uint8_t in[64], out[64];

    fill_pattern(in, sizeof(in), 11);
    qtest_memwrite(qts, STORE_ADDR, in, sizeof(in));
    clear_counts(qts);
    qtest_writeq(qts, ST_TARGET, STORE_ADDR - 64);
    qtest_writeq(qts, ST_ONLY_WRITE, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 64);
    qtest_writeq(qts, ST_ONLY_READ, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED_SUM), ==, pattern_sum(64));
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    qtest_memread(qts, STORE_ADDR, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    check_pattern(qts, STORE_ADDR - 64, 64, 0);
    qtest_quit(qts);
}

/* From RAM it carries on into a region whose buffer callback accepts. */
static void test_buffer_only_continues_into_buffer_region(void)
{
    QTestState *qts = start_with_ram("", "");

    qtest_writeq(qts, ST_TARGET, STORE_ADDR - 64);
    qtest_writeq(qts, ST_ONLY_WRITE, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, ST_OFFERED), ==, 64);
    qtest_writeq(qts, ST_ONLY_READ, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 128);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED_SUM), ==, pattern_sum(128));
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    check_pattern(qts, STORE_ADDR - 64, 128, 0);
    qtest_quit(qts);
}

/*
 * A partial accept is kept and the rest offered again until what is left
 * is too short to offer: 40 bytes taken 16 at a time move 32.
 */
static void test_buffer_only_partial(void)
{
    QTestState *qts = start_with_ram(",mode=3", "");

    qtest_writeq(qts, ST_TARGET, STORE_ADDR);
    qtest_writeq(qts, ST_ONLY_WRITE, 40);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_OK);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    check_pattern(qts, STORE_ADDR, 32, 0);
    qtest_quit(qts);
}

/*
 * A decline, a range of 8 bytes or less and an unassigned address all
 * move nothing and make no ordinary access.
 */
static void test_buffer_only_moves_nothing(void)
{
    QTestState *qts = start_with_ram(",mode=2", "");

    qtest_writeq(qts, ST_TARGET, STORE_ADDR);
    qtest_writeq(qts, ST_ONLY_WRITE, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 0);
    qtest_writeq(qts, ST_ONLY_READ, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 2);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    qtest_quit(qts);

    qts = start_with_ram("", "");
    qtest_writeq(qts, ST_TARGET, STORE_ADDR);
    qtest_writeq(qts, ST_ONLY_WRITE, 8);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 0);
    qtest_writeq(qts, ST_TARGET, STATUS_ADDR + 0x10000);
    qtest_writeq(qts, ST_ONLY_WRITE, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    qtest_quit(qts);
}

/*
 * The device's reentrancy guard covers an offer made by a buffer-only
 * transfer: the callback's re-entry into the same device is refused.
 */
static void test_buffer_only_reentrancy_guard(void)
{
    QTestState *qts = start_with_ram(",mode=4", "");

    qtest_writeq(qts, ST_TARGET, STORE_ADDR);
    qtest_writeq(qts, ST_ONLY_READ, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 64);
    g_assert_cmpuint(qtest_readq(qts, ST_RESULT), ==, MEMTX_ACCESS_ERROR);
    g_assert_cmpuint(qtest_readq(qts, ST_BUFFER), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, ST_SCALAR), ==, 0);
    qtest_quit(qts);
}

/* A callback's error result on what it consumed reaches the caller. */
static void test_buffer_only_error_result(void)
{
    QTestState *qts = start_with_ram(",mode=5", "");

    qtest_writeq(qts, ST_TARGET, STORE_ADDR);
    qtest_writeq(qts, ST_ONLY_WRITE, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_MOVED), ==, 32);
    g_assert_cmpuint(qtest_readq(qts, ST_TRANSACTION), ==, MEMTX_ERROR);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);
    for (size_t i = 0; i < ARRAY_SIZE(cpu_for_arch); i++) {
        if (g_str_equal(arch, cpu_for_arch[i].arch)) {
            test_cpu = cpu_for_arch[i].cpu;
        }
    }
    if (!test_cpu) {
        g_test_skip("no CPU model listed for this target");
        return g_test_run();
    }
    if (!qtest_has_device("memory-buffer-testdev")) {
        g_test_skip("memory-buffer-testdev is not built for this target");
        return g_test_run();
    }
    qtest_add_func("/memory-buffer/raw-bytes", test_raw_bytes);
    qtest_add_func("/memory-buffer/short-steps-are-narrow",
                   test_short_steps_are_narrow);
    qtest_add_func("/memory-buffer/no-callback", test_no_callback);
    qtest_add_func("/memory-buffer/declined", test_declined);
    qtest_add_func("/memory-buffer/accepts-is-never-offered",
                   test_accepts_is_never_offered);
    qtest_add_func("/memory-buffer/ioeventfd-is-never-offered",
                   test_ioeventfd_is_never_offered);
    qtest_add_func("/memory-buffer/partial-prefix", test_partial_prefix);
    qtest_add_func("/memory-buffer/reentrancy-guard", test_reentrancy_guard);
    qtest_add_func("/memory-buffer/error-result", test_error_result);
    qtest_add_func("/memory-buffer/transaction-result",
                   test_transaction_result);
    qtest_add_func("/memory-buffer/offer-ends-at-region-end",
                   test_offer_ends_at_region_end);
    qtest_add_func("/memory-buffer/offer-ends-at-section-end",
                   test_offer_ends_at_section_end);
    qtest_add_func("/memory-buffer/declined-keeps-narrow-widths",
                   test_declined_keeps_narrow_widths);
    qtest_add_func("/memory-buffer/bounce-buffer", test_bounce_buffer);
    qtest_add_func("/memory-buffer/buffer-only-ram", test_buffer_only_ram);
    qtest_add_func("/memory-buffer/buffer-only-stops-before-ordinary-region",
                   test_buffer_only_stops_before_ordinary_region);
    qtest_add_func("/memory-buffer/buffer-only-continues-into-buffer-region",
                   test_buffer_only_continues_into_buffer_region);
    qtest_add_func("/memory-buffer/buffer-only-partial",
                   test_buffer_only_partial);
    qtest_add_func("/memory-buffer/buffer-only-moves-nothing",
                   test_buffer_only_moves_nothing);
    qtest_add_func("/memory-buffer/buffer-only-reentrancy-guard",
                   test_buffer_only_reentrancy_guard);
    qtest_add_func("/memory-buffer/buffer-only-error-result",
                   test_buffer_only_error_result);
    return g_test_run();
}
