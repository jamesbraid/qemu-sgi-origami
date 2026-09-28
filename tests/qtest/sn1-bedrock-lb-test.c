/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Isolated Bedrock local-block MMIO; the empty machine supplies only an
 * address space and does not represent an Origin 300.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define LB_PATH "/machine/peripheral/lb"
#define LB_BASE 0x11000000ULL
#define MACHINE_BASE "-M none -cpu R10000 -device sgi-bedrock-lb,id=lb," \
                     "manufacturer=0x36,part-number=0x3012,revision=2," \
                     "node-number=0x5a,region-size=0,request-region=7"
#define MACHINE MACHINE_BASE " -S"
/*
 * The block's own register window stands in for the node's hub window, the
 * one a peer's vector request would reach. A standalone block has no cable,
 * so its own launches must still reach nothing.
 */
#define MACHINE_HUB MACHINE_BASE \
                    ",hub-registers=" LB_PATH "/sgi-bedrock-lb[0] -S"

#define REVISION        0x0000
#define CPU_PERMISSION  0x4000
#define CPU_OVERRIDE    0x4008
#define IO_PERMISSION   0x4010
#define SOFT_RESET      0x4018
#define REGION_PRESENT  0x4020
#define NODES_ABSENT    0x4028
#define MICROLAN        0x4030
#define ERROR_BITS      0x4040
#define ERROR_CLEAR     0x4048
#define ERROR_HEADER_1  0x4050
#define ERROR_HEADER_2  0x4058
#define ERROR_DATA      0x4060
#define DEBUG_SELECT    0x4100
#define RTC_LOCAL       0x4200
#define RTC_FILTER      0x4208
#define SCRATCH(n)      (0x8000 + (n) * 8)
#define SCRATCH_WZ(n)   (0x8040 + (n) * 8)
#define SCRATCH_RS(n)   (0x8058 + ((n) - 3) * 8)
#define VECTOR_PARAMS   0xc000
#define VECTOR_ROUTE    0xc008
#define VECTOR_DATA     0xc010
#define VECTOR_STATUS   0xc020
#define VECTOR_RETURN   0xc028
#define VECTOR_READ     0xc030
#define VECTOR_STATUS_CLEAR 0xc038

/* The one-bit type field is the model's reading of an open width. */
#define VECTOR_PARAMS_MASK (1ULL | (0x1fffffULL << 3) | (0xffULL << 32) | \
                            (0x7ffULL << 40))
#define RTC_LOCAL_MASK (1ULL | (0x3ffULL << 4) | (0x3ffULL << 16) | \
                        (1ULL << 28) | (1ULL << 32))
#define RTC_FILTER_MASK (0x1fULL | (0xfffULL << 8) | (1ULL << 20) | \
                         (0x3ffULL << 24) | (0x3ffULL << 36) | \
                         (0x3ffULL << 48))
/* Command, supplemental, source, error type, overrun, valid. */
#define HEADER_1_MASK (0x7fULL | (0x7ffULL << 8) | (0x7ffULL << 20) | \
                       (0xfULL << 32) | (1ULL << 40) | (1ULL << 44))

/*
 * The MicroLAN controller. Bit 0 is the sampled level and bit 1 completion,
 * both the device's to publish; the sample offset, the low-pulse duration and
 * the two phase dividers are the guest's, and the upper divider reaches bit
 * 33.
 */
#define MICROLAN_RXDATA (1ULL << 0)
#define MICROLAN_DONE   (1ULL << 1)
#define MICROLAN_TIMING ((0xffULL << 2) | (0x3ffULL << 10) | \
                         (0x7fULL << 20) | (0x7fULL << 27))

/*
 * Pulse lengths as the shared SGI slot decode reads them, and the model's
 * slot delay. Every number here is QEMU's, not Bedrock's: no public source
 * turns this register's encodings into time.
 */
#define MICROLAN_PULSE_RESET 0x321  /* long low: 1-Wire reset/presence */
#define MICROLAN_PULSE_SHORT 8      /* releases the wire: a read, or a one */
#define MICROLAN_PULSE_ZERO  60     /* held low: a zero */
#define MICROLAN_SLOT_NS     1000

/*
 * One write programming all six fields. The phase-1 divider is 0x66, which
 * sets register bits 32 and 33: a controller keeping this register in a
 * 32-bit word loses them while every other assertion here still passes.
 */
static uint64_t microlan_word(unsigned int pulse)
{
    return MICROLAN_RXDATA | MICROLAN_DONE | (0x41ULL << 2) |
           ((uint64_t)pulse << 10) | (0x2cULL << 20) | (0x66ULL << 27);
}

static QTestState *lb_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, LB_PATH, 0, LB_BASE);
    return s;
}

/*
 * The block stands in for its own hub, mapped at zero so a register's window
 * offset is the address a vector request names. The 21-bit address field
 * cannot carry a high base, and a composed node reaches the window through a
 * zero-based alias for the same reason.
 */
#define LB_HUB_BASE 0x0ULL
static QTestState *lb_start_hub(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE_HUB " %s", extra);

    qtest_sysbus_mmio_map(s, LB_PATH, 0, LB_HUB_BASE);
    return s;
}

/*
 * QEMU starts the virtual clock disabled and vm_start() enables it, so a
 * device timer on a machine launched with -S never fires however far the
 * clock is stepped. Every test that waits for a slot, or asserts that no
 * slot is waiting, has to run the machine, or it asserts nothing.
 */
static QTestState *lb_start_clock(const char *extra)
{
    QTestState *s = lb_start(extra);

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    return s;
}

/*
 * A migration destination can still be loading when the source calls the
 * migration complete, and cont() while it is still coming in only sets
 * autostart and returns success (monitor/qmp-cmds.c). The machine
 * starts a moment later. Sampling query-status once would therefore step the
 * clock
 * while it is still disabled, so ask once and then wait for it to be running.
 */
static void lb_wait_for_clock_running(QTestState *s)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    bool asked = false;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(s, "{'execute':'query-status'}");
        QDict *status = qdict_get_qdict(response, "return");

        if (qdict_get_bool(status, "running")) {
            return;
        }
        if (!asked) {
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            asked = true;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_identity_and_permissions(void)
{
    QTestState *s = lb_start("");
    const uint64_t identity = (0x36ULL << 1) | (0x3012ULL << 12) |
                              (2ULL << 28) | (0x5aULL << 32);

    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==, identity);

    qtest_writeq(s, LB_BASE + CPU_PERMISSION, 0);
    qtest_writeq(s, LB_BASE + CPU_OVERRIDE, 0x1234);
    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, 0);
    qtest_writeq(s, LB_BASE + CPU_OVERRIDE, 0x53474972756c6573ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, 1ULL << 7);
    qtest_writeq(s, LB_BASE + IO_PERMISSION, 0xa55aa55aa55aa55aULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + IO_PERMISSION), ==,
                    0xa55aa55aa55aa55aULL);
    qtest_quit(s);
}

/*
 * The parent composes the block with only its revision set, so a one-chassis
 * guest reads the property defaults: manufacturer 0x036 (the model's value
 * from two public widget tables; the hardware word is unobserved), part
 * 0x3012, node 0 and region size 0 (the model's chosen value, and what the
 * Itanium port consults for the machine's size).
 */
static void test_default_identity(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 "
                               "-device sgi-bedrock-lb,id=lb -S");

    qtest_sysbus_mmio_map(s, LB_PATH, 0, LB_BASE);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==,
                    (0x36ULL << 1) | (0x3012ULL << 12) | (2ULL << 28));
    qtest_quit(s);
}

/*
 * The stock PROM assigns the node number by storing the whole revision word:
 * zero at hub initialisation, then the word with bits 39:32 and 47:46
 * replaced, and it uses the value it reads back. The two fields latch; the
 * identity fields do not, so the early zero store must leave them intact.
 * Region size 3 is invalid and refused with a log, and a reset restores the
 * realized number, the model's stand-in for an unknown reset value.
 */
static void test_revision_write_latches_node(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn1-lb-identity-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    const uint64_t identity = (0x36ULL << 1) | (0x3012ULL << 12) |
                              (2ULL << 28);
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d guest_errors -D %s", log);
    s = lb_start(extra);

    qtest_writeq(s, LB_BASE + REVISION, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==, identity);
    qtest_writeq(s, LB_BASE + REVISION, identity | (0x21ULL << 32) |
                 (1ULL << 46));
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==,
                    identity | (0x21ULL << 32) | (1ULL << 46));
    /* Identity bits in the write do not take: only the two fields latch. */
    qtest_writeq(s, LB_BASE + REVISION, (0x7ffULL << 1) | (0x44ULL << 32));
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==,
                    identity | (0x44ULL << 32));
    qtest_writeq(s, LB_BASE + REVISION, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==,
                    identity | (0x44ULL << 32));

    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REVISION), ==,
                    identity | (0x5aULL << 32));
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "names the invalid region size 3"));
    g_assert_null(strstr(found, "write to read-only register 0x0\n"));
    g_assert_cmpint(unlink(log), ==, 0);
}

/*
 * Permission-register reset values are unknown; consumers initially assume
 * access is allowed. All-ones values at power-on and soft reset are model
 * policy.
 */
static void test_permissive_reset(void)
{
    QTestState *s = lb_start("");

    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + IO_PERMISSION), ==, UINT64_MAX);
    qtest_writeq(s, LB_BASE + CPU_PERMISSION, 0);
    qtest_writeq(s, LB_BASE + IO_PERMISSION, 0);
    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + CPU_PERMISSION), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + IO_PERMISSION), ==, UINT64_MAX);
    qtest_quit(s);
}

/*
 * No public source states either register's power-on contents. Reading zero
 * at power-on and after the block's own soft reset is the model's choice,
 * asserted as a model choice, not a hardware fact. The maximum-count field
 * this pins
 * at zero is the one the stock PROM's own real-time conversion divides by, so
 * this reset value is known to make that conversion come out about eighty
 * times short (jamesb/qemu#581); a source that settles the real reset value
 * replaces this test's expectation.
 */
static void test_rtc_reset(void)
{
    QTestState *s = lb_start("");

    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_LOCAL), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_FILTER), ==, 0);
    qtest_writeq(s, LB_BASE + RTC_LOCAL, RTC_LOCAL_MASK);
    qtest_writeq(s, LB_BASE + RTC_FILTER, RTC_FILTER_MASK);
    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_LOCAL), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_FILTER), ==, 0);
    qtest_quit(s);
}

static void test_storage_masks_and_error(void)
{
    QTestState *s = lb_start("");

    qtest_irq_intercept_out_named(s, LB_PATH, "error");
    qtest_writeq(s, LB_BASE + REGION_PRESENT, 0x8000000000000001ULL);
    qtest_writeq(s, LB_BASE + NODES_ABSENT, 0xffff01238000ff7fULL);
    qtest_writeq(s, LB_BASE + RTC_LOCAL, UINT64_MAX);
    qtest_writeq(s, LB_BASE + RTC_FILTER, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + REGION_PRESENT), ==,
                    0x8000000000000001ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + NODES_ABSENT), ==,
                    0x80ff00238000807fULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_LOCAL), ==, RTC_LOCAL_MASK);
    g_assert_cmphex(qtest_readq(s, LB_BASE + RTC_FILTER), ==, RTC_FILTER_MASK);

    qtest_writeq(s, LB_BASE + ERROR_HEADER_1, 1ULL << 44);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_1), ==, 1ULL << 44);
    qtest_writeq(s, LB_BASE + ERROR_HEADER_1, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_1), ==,
                    HEADER_1_MASK);
    qtest_writeq(s, LB_BASE + ERROR_HEADER_1, ~HEADER_1_MASK);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_1), ==, 0);
    qtest_writeq(s, LB_BASE + ERROR_HEADER_2, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_HEADER_2), ==,
                    (1ULL << 38) - 1);
    qtest_writeq(s, LB_BASE + ERROR_DATA, 0x0123456789abcdefULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_DATA), ==,
                    0x0123456789abcdefULL);
    qtest_writeq(s, LB_BASE + DEBUG_SELECT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + DEBUG_SELECT), ==, 0x3f);
    qtest_writeq(s, LB_BASE + VECTOR_ROUTE, 0xfedcba9876543210ULL);
    qtest_writeq(s, LB_BASE + VECTOR_DATA, 0x1020304050607080ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_ROUTE), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_DATA), ==,
                    0x1020304050607080ULL);
    qtest_writeq(s, LB_BASE + ERROR_BITS, (1ULL << 10) | 3);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==,
                    (1ULL << 10) | 3);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, LB_BASE + ERROR_CLEAR, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==,
                    (1ULL << 10) | 2);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, LB_BASE + ERROR_CLEAR, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_byte_order_and_width(void)
{
    QTestState *s = lb_start("");
    const uint8_t bytes[8] = { 1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    uint8_t actual[8];
    uint64_t addr = LB_BASE + CPU_PERMISSION;

    qtest_memwrite(s, addr, bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x0123456789abcdefULL);
    qtest_memread(s, addr, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    qtest_writeb(s, addr + 7, 0);
    qtest_writew(s, addr + 6, 0);
    qtest_writel(s, addr + 4, 0);
    qtest_writeq(s, addr + 1, 0);
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x0123456789abcdefULL);
    qtest_quit(s);
}

static void test_scratch_aliases_and_reset(void)
{
    QTestState *s = lb_start("");

    qtest_irq_intercept_out_named(s, LB_PATH, "soft-reset");
    for (unsigned i = 0; i < 5; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0x100 + i);
    }
    for (unsigned i = 0; i < 3; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        qtest_writeq(s, LB_BASE + SCRATCH_WZ(i), 0xdeadbeef + i);
        qtest_writeq(s, LB_BASE + SCRATCH_WZ(i), 0xcafebabe + i);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(i)), ==,
                        0xdeadbeef + i);
    }
    for (unsigned i = 3; i < 5; i++) {
        qtest_writeq(s, LB_BASE + SCRATCH(i), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(i)), ==, 1);
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 1);
        qtest_writeq(s, LB_BASE + SCRATCH(i), 0);
        g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH_RS(i)), ==, 0);
    }

    qtest_writeq(s, LB_BASE + ERROR_BITS, 4);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0xdeadbef0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(2)), ==, 0xdeadbef1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 4);

    qtest_writeq(s, LB_BASE + SCRATCH(0), 0x99);
    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(0)), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0xdeadbef0);
    /*
     * The reboot mailbox is not reset, the block's own soft reset included.
     */
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(2)), ==, 0xdeadbef1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + ERROR_BITS), ==, 4);
    qtest_quit(s);
}

/*
 * hw/misc/sgi-bedrock.c's reset-barrier park at LB scratch 1
 * (sgi_bedrock_hold_scratch1_write) only arbitrates an access this node's own
 * processor issues. A standalone sgi-bedrock-lb, as every test in this file
 * runs, has no owning node (SGIBedrockLBState.bedrock is only set by
 * sgi-bedrock.c's bedrock_realize), and a qtest backdoor write carries no
 * issuing vCPU either way. A second write to scratch 1 must therefore still
 * round-trip at once, never withheld, exactly as the first assertion in
 * test_scratch_aliases_and_reset already exercises in passing; this test
 * names the property directly.
 */
static void test_scratch1_second_write_no_hold(void)
{
    QTestState *s = lb_start("");

    qtest_writeq(s, LB_BASE + SCRATCH(1), 0x7f80000);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0x7f80000);
    qtest_writeq(s, LB_BASE + SCRATCH(1), 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0);
    qtest_writeq(s, LB_BASE + SCRATCH(1), 0x7f80000);
    g_assert_cmphex(qtest_readq(s, LB_BASE + SCRATCH(1)), ==, 0x7f80000);
    qtest_quit(s);
}

/*
 * Vector registers: parameters
 * keep their fields with busy reading clear, route and data are plain
 * storage, the PROM's power-on zero writes to status, return route and read
 * data are accepted, and a read of status clear is not an unimplemented
 * access. A nonzero status write is ignored and logged: nothing but a reply
 * may set status. A standalone block has no cable, so a launch reaches no
 * device and is logged; sn1-bedrock-link-test covers launches that do.
 */
static void test_vector_registers(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn1-lb-vector-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp,guest_errors -D %s", log);
    s = lb_start(extra);

    qtest_writeq(s, LB_BASE + VECTOR_STATUS, 0);
    qtest_writeq(s, LB_BASE + VECTOR_RETURN, 0);
    qtest_writeq(s, LB_BASE + VECTOR_READ, 0);
    qtest_writeq(s, LB_BASE + VECTOR_ROUTE, 0xfedcba9876543210ULL);
    qtest_writeq(s, LB_BASE + VECTOR_DATA, 0x1020304050607080ULL);
    qtest_writeq(s, LB_BASE + VECTOR_PARAMS, UINT64_MAX);
    /* Bit 1 reading 0 is the model's one-bit type, not a datasheet fact. */
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_PARAMS), ==,
                    VECTOR_PARAMS_MASK);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_ROUTE), ==,
                    0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_DATA), ==,
                    0x1020304050607080ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_RETURN), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_READ), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS_CLEAR), ==, 0);
    qtest_writeq(s, LB_BASE + VECTOR_STATUS_CLEAR, UINT64_MAX);
    qtest_writeq(s, LB_BASE + VECTOR_STATUS, UINT64_MAX);
    qtest_writeq(s, LB_BASE + VECTOR_RETURN, 0x1111222233334444ULL);
    qtest_writeq(s, LB_BASE + VECTOR_READ, 0xdeadbeefdeadbeefULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_RETURN), ==,
                    0x1111222233334444ULL);
    g_assert_cmphex(qtest_readq(s, LB_BASE + VECTOR_READ), ==,
                    0xdeadbeefdeadbeefULL);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "vector write of 0x"));
    g_assert_nonnull(strstr(found, "reached no device"));
    g_assert_nonnull(strstr(found, "nonzero write 0xffffffffffffffff to "
                                   "the vector status is ignored"));
    g_assert_null(strstr(found, "unsupported write at 0xc000"));
    g_assert_null(strstr(found, "unsupported read at 0xc020"));
    g_assert_null(strstr(found, "unsupported read at 0xc028"));
    g_assert_null(strstr(found, "unsupported read at 0xc030"));
    g_assert_null(strstr(found, "unsupported write at 0xc038"));
    g_assert_null(strstr(found, "unsupported read at 0xc038"));
    g_assert_null(strstr(found, "status-clear alias"));
    g_assert_null(strstr(found, "read from write-only register 0xc038"));
    g_assert_null(strstr(found, "write to read-only register 0xc02"));
    g_assert_null(strstr(found, "write to read-only register 0xc03"));
    g_assert_cmpint(unlink(log), ==, 0);
}

/*
 * The stock PROM launches nothing without a link, and route 0 names the
 * device across the cable, never this node. So a launch from a block with a
 * hub window but no cable must not answer from that window: status stays not
 * valid and read data keeps the sentinel the firmware seeds. The self-route
 * reply this replaces made a node its own neighbour.
 */
static void test_vector_no_link(void)
{
    QTestState *s = lb_start_hub("");

    qtest_writeq(s, LB_HUB_BASE + SCRATCH(0), 0x0123456789abcdefULL);
    (void)qtest_readq(s, LB_HUB_BASE + VECTOR_STATUS_CLEAR);
    qtest_writeq(s, LB_HUB_BASE + VECTOR_READ, 0xdeadbeefdeadbeefULL);
    qtest_writeq(s, LB_HUB_BASE + VECTOR_ROUTE, 0);
    qtest_writeq(s, LB_HUB_BASE + VECTOR_PARAMS,
                 SCRATCH(0) | (0x1c8ULL << 40));
    g_assert_cmphex(qtest_readq(s, LB_HUB_BASE + VECTOR_STATUS), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_HUB_BASE + VECTOR_READ), ==,
                    0xdeadbeefdeadbeefULL);
    qtest_quit(s);
}

/*
 * The controller's model choices in one 64-bit access: a write programs all
 * six fields, clears completion, and publishes completion and the sampled
 * level only once the slot's virtual delay has passed. Start, completion and
 * that delay are the model's; none is a claim about Bedrock timing.
 * No endpoint is fitted because the part IP45 carries is not
 * established. The bus is empty and every slot reports the bare wire. A
 * released wire
 * samples one, a wire held low samples zero, and a reset finds no presence.
 * The zero-sampling slot is what separates this from a controller whose
 * sampled bit is fixed.
 */
static void test_microlan_slot(void)
{
    static const struct {
        unsigned int pulse;
        uint64_t data;
    } slots[] = {
        { MICROLAN_PULSE_SHORT, MICROLAN_RXDATA },
        { MICROLAN_PULSE_ZERO, 0 },
        { MICROLAN_PULSE_RESET, MICROLAN_RXDATA },
    };
    QTestState *s = lb_start_clock("");

    for (size_t i = 0; i < ARRAY_SIZE(slots); i++) {
        uint64_t word = microlan_word(slots[i].pulse);
        uint64_t timing = word & MICROLAN_TIMING;

        g_assert_cmphex(word & (3ULL << 32), ==, 3ULL << 32);
        qtest_writeq(s, LB_BASE + MICROLAN, word);

        /* Bits 1:0 belong to the device: the guest's copies do not stick. */
        g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, timing);
        /* Polling is not a slot, and the deadline has not arrived. */
        for (unsigned int poll = 0; poll < 4; poll++) {
            g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, timing);
        }

        qtest_clock_step(s, 2 * MICROLAN_SLOT_NS);
        g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==,
                        timing | MICROLAN_DONE | slots[i].data);
    }

    /* Bits 63:34 are reserved and store nothing. */
    qtest_writeq(s, LB_BASE + MICROLAN, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, MICROLAN_TIMING);
    qtest_quit(s);
}

/*
 * Half a register cannot start a wire slot. The block takes one aligned
 * eight-byte access. This model choice prevents QEMU from splitting a
 * guest's write and running two slots. The whole access carries
 * the dividers above bit 31.
 */
static void test_microlan_rejects_a_narrow_access(void)
{
    QTestState *s = lb_start_clock("");

    qtest_writel(s, LB_BASE + MICROLAN, 0xffffffff);
    qtest_writel(s, LB_BASE + MICROLAN + 4, 0xffffffff);
    qtest_clock_step(s, 2 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, 0);

    qtest_writeq(s, LB_BASE + MICROLAN,
                 microlan_word(MICROLAN_PULSE_ZERO));
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN) >> 32, ==, 3);
    qtest_quit(s);
}

/*
 * A machine reset and the block's own soft reset both cancel a slot in
 * flight, so completion cannot appear afterwards. Zero is the model's chosen
 * value for the register: no public source gives a power-on state.
 */
static void test_microlan_reset_cancels_a_pending_slot(void)
{
    QTestState *s = lb_start_clock("");

    qtest_writeq(s, LB_BASE + MICROLAN,
                 microlan_word(MICROLAN_PULSE_SHORT));
    qtest_system_reset(s);
    qtest_clock_step(s, 20 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, 0);

    qtest_writeq(s, LB_BASE + MICROLAN,
                 microlan_word(MICROLAN_PULSE_SHORT));
    qtest_writeq(s, LB_BASE + SOFT_RESET, 1);
    qtest_clock_step(s, 20 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==, 0);

    /* The controller still works: cancelling is not wedging. */
    qtest_writeq(s, LB_BASE + MICROLAN,
                 microlan_word(MICROLAN_PULSE_SHORT));
    qtest_clock_step(s, 2 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN) & MICROLAN_DONE, ==,
                    MICROLAN_DONE);
    qtest_quit(s);
}

/*
 * What a second write during a slot does is not published, so the model
 * refuses it rather than guessing a transaction semantics. The first slot
 * runs to its own completion with its own timing, and the discarded write is
 * logged. Its pulse would have sampled the other level, so a model that
 * quietly took it would be visible here.
 */
static void test_microlan_write_during_a_slot_is_refused(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn1-lb-microlan-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    const uint64_t first = microlan_word(MICROLAN_PULSE_SHORT);
    const uint64_t second = microlan_word(MICROLAN_PULSE_ZERO);
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp -D %s", log);
    s = lb_start_clock(extra);

    qtest_writeq(s, LB_BASE + MICROLAN, first);
    qtest_writeq(s, LB_BASE + MICROLAN, second);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==,
                    first & MICROLAN_TIMING);
    qtest_clock_step(s, 2 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(s, LB_BASE + MICROLAN), ==,
                    (first & MICROLAN_TIMING) | MICROLAN_DONE |
                    MICROLAN_RXDATA);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "while a slot is pending"));
    g_assert_cmpint(unlink(log), ==, 0);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-lb-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    /*
     * One entry per migrated field that a write can set directly, each
     * differing from its reset value. The MicroLAN register, its pending
     * sample and its slot deadline migrate too but cannot go in this table:
     * writing that register starts a wire slot, so its value depends on when
     * the clock is stepped. test_microlan_migration covers those three.
     */
    static const struct {
        uint64_t offset;
        uint64_t written;
        uint64_t expected;
    } fields[] = {
        { CPU_PERMISSION, 0x123456789abcdef0ULL, 0x123456789abcdef0ULL },
        { IO_PERMISSION, 0x0f1e2d3c4b5a6978ULL, 0x0f1e2d3c4b5a6978ULL },
        { REGION_PRESENT, 0x55aa55aa55aa55aaULL, 0x55aa55aa55aa55aaULL },
        { NODES_ABSENT, 0xffff01238000ff7fULL, 0x80ff00238000807fULL },
        { ERROR_BITS, 1ULL << 9, 1ULL << 9 },
        { ERROR_HEADER_1, UINT64_MAX, HEADER_1_MASK },
        { ERROR_HEADER_2, 0x2bcdef0123ULL, 0x2bcdef0123ULL },
        { ERROR_DATA, 0xfeedfacecafef00dULL, 0xfeedfacecafef00dULL },
        { DEBUG_SELECT, 0x2a, 0x2a },
        { RTC_LOCAL, UINT64_MAX, RTC_LOCAL_MASK },
        { RTC_FILTER, UINT64_MAX, RTC_FILTER_MASK },
        { SCRATCH(0), 0x52c5878c, 0x52c5878c },
        { SCRATCH(1), 0x0000080690349000ULL, 0x0000080690349000ULL },
        { SCRATCH(2), 0x0ead0040, 0x0ead0040 },
        { SCRATCH(3), 1, 1 },
        { SCRATCH(4), 1, 1 },
        /* Bit 1 masked off is the model's one-bit type; the width is open. */
        { VECTOR_PARAMS, 0x5a5a5a5a5a5a5a5aULL,
          0x5a5a5a5a5a5a5a5aULL & VECTOR_PARAMS_MASK },
        { VECTOR_ROUTE, 0x123456789abcdef0ULL, 0x123456789abcdef0ULL },
        { VECTOR_DATA, 0xfedcba9876543210ULL, 0xfedcba9876543210ULL },
        { VECTOR_RETURN, 0x0123012301230123ULL, 0x0123012301230123ULL },
        { VECTOR_READ, 0xdeadbeefdeadbeefULL, 0xdeadbeefdeadbeefULL },
        /* The latched node number and region size, over the identity. */
        { REVISION, (0x33ULL << 32) | (1ULL << 46),
          (0x36ULL << 1) | (0x3012ULL << 12) | (2ULL << 28) |
          (0x33ULL << 32) | (1ULL << 46) },
    };

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = lb_start("");
    to = lb_start("-incoming defer");
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        qtest_writeq(from, LB_BASE + fields[i].offset, fields[i].written);
    }
    qtest_irq_intercept_out_named(to, LB_PATH, "error");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);
    for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
        g_assert_cmphex(qtest_readq(to, LB_BASE + fields[i].offset), ==,
                        fields[i].expected);
    }
    g_assert_true(qtest_get_irq(to, 0));
    qtest_writeq(to, LB_BASE + ERROR_CLEAR, 1ULL << 9);
    g_assert_false(qtest_get_irq(to, 0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * The stream is written with a slot in flight. The destination has to finish
 * it once, with the level the source's wire returned rather than a level
 * recomputed after load: the slot held the wire low and sampled zero, while a
 * controller that dropped the pending level would fall back to the reset
 * level and report one.
 */
static void test_microlan_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-lb-microlan-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    const uint64_t timing = microlan_word(MICROLAN_PULSE_ZERO) &
                            MICROLAN_TIMING;
    QTestState *from, *to;
    uint64_t after;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = lb_start_clock("");
    to = lb_start("-incoming defer");

    qtest_writeq(from, LB_BASE + MICROLAN,
                 microlan_word(MICROLAN_PULSE_ZERO));
    g_assert_cmphex(qtest_readq(from, LB_BASE + MICROLAN), ==, timing);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    lb_wait_for_clock_running(to);

    /* Nothing completed in transit: the deadline is still ahead. */
    g_assert_cmphex(qtest_readq(to, LB_BASE + MICROLAN), ==, timing);
    qtest_clock_step(to, 2 * MICROLAN_SLOT_NS);
    after = qtest_readq(to, LB_BASE + MICROLAN);
    g_assert_cmphex(after, ==, timing | MICROLAN_DONE);

    /* And it stays finished: no re-armed timer, no second slot. */
    qtest_clock_step(to, 100 * MICROLAN_SLOT_NS);
    g_assert_cmphex(qtest_readq(to, LB_BASE + MICROLAN), ==, after);

    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-lb/identity-permissions",
                   test_identity_and_permissions);
    qtest_add_func("sn1/bedrock-lb/default-identity", test_default_identity);
    qtest_add_func("sn1/bedrock-lb/revision-write-latches-node",
                   test_revision_write_latches_node);
    qtest_add_func("sn1/bedrock-lb/permissive-reset", test_permissive_reset);
    qtest_add_func("sn1/bedrock-lb/rtc-reset", test_rtc_reset);
    qtest_add_func("sn1/bedrock-lb/storage-masks-error",
                   test_storage_masks_and_error);
    qtest_add_func("sn1/bedrock-lb/byte-order-width",
                   test_byte_order_and_width);
    qtest_add_func("sn1/bedrock-lb/scratch-reset",
                   test_scratch_aliases_and_reset);
    qtest_add_func("sn1/bedrock-lb/scratch1-second-write-no-hold",
                   test_scratch1_second_write_no_hold);
    qtest_add_func("sn1/bedrock-lb/vector-registers", test_vector_registers);
    qtest_add_func("sn1/bedrock-lb/vector-no-link", test_vector_no_link);
    qtest_add_func("sn1/bedrock-lb/microlan-slot", test_microlan_slot);
    qtest_add_func("sn1/bedrock-lb/microlan-narrow-access",
                   test_microlan_rejects_a_narrow_access);
    qtest_add_func("sn1/bedrock-lb/microlan-reset",
                   test_microlan_reset_cancels_a_pending_slot);
    qtest_add_func("sn1/bedrock-lb/microlan-write-during-slot",
                   test_microlan_write_during_a_slot_is_refused);
    qtest_add_func("sn1/bedrock-lb/migration", test_migration);
    qtest_add_func("sn1/bedrock-lb/microlan-migration",
                   test_microlan_migration);
    return g_test_run();
}
