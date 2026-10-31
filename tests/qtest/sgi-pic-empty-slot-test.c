/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * PIC configuration cycles that no device claims, as the processor sees
 * them (docs/datasheets/pic.md, "Empty-slot configuration reads"). The rule
 * is inferred there from the PROM's probe and the public register header,
 * not established on silicon, so these cases pin the selected model and do
 * not claim the hardware does it.
 *
 * Window w of a head is configuration address 0x20000 + w * 0x1000, device 0
 * being the reserved window. Function f adds f * 0x100. Registers are the
 * per-head status 0x100, enable 0x108, reset 0x110 and error view 0x170;
 * the PROM's 32-bit accesses use the low word, at +4.
 *
 * Not asserted because they are unknown: the PCI-X master-timeout bit (34),
 * whether bit 0 of a 0x41 reset write is a PCI-group clear, what the PIC
 * returns at device 0, and the processor-interface record in Bedrock.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"

#define PIC_CHILD "/machine/peripheral/fabric/pic"
#define BASE 0x100000000000ULL
#define CPU_BASE (BASE + 0x100000000ULL)
#define HEAD_OFFSET 0x800000ULL
#define CPU_HEAD(n) (CPU_BASE + (n) * HEAD_OFFSET)
#define CFG(window, function) (0x20000 + (window) * 0x1000 + (function) * 0x100)
#define TYPE1_WINDOW 0x28000
#define STATUS 0x100
#define ENABLE 0x108
#define RESET 0x110
#define ERROR_VIEW 0x170
#define TIMEOUT_BIT (1ULL << 11)
#define INVALID_ADDRESS_BIT (1ULL << 24)

/*
 * Head 0: a single-function card at window 1 (ID dword 0x50011234) and a
 * multifunction card at window 3, functions 0 and 1. Head 1: a single card
 * at window 3. Every other window of both heads is empty.
 */
static QTestState *pic_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
        "-d unimp,guest_errors" MEMTX_TEST_DEVICE
        "-device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,"
        "pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=1,"
        "vendor-id=0x1234,device-id=0x5001 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=3.0,"
        "multifunction=on,vendor-id=0xfeed,device-id=0x3510 "
        "-device sgi-bridge-dma-test,bus=pic-head-0.0,addr=3.1,"
        "vendor-id=0xfeed,device-id=0x3511 "
        "-device sgi-bridge-dma-test,bus=pic-head-1.0,addr=3,"
        "vendor-id=0xfeed,device-id=0x3503 %s", extra);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 0, 0x20000000ULL);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/fabric", 1, BASE);
    qtest_sysbus_mmio_map(qts, PIC_CHILD, 0, CPU_BASE);
    return qts;
}

static MemTxResult load(QTestState *qts, unsigned head, uint64_t offset,
                        unsigned size, uint64_t *value)
{
    return memtx_load(qts, CPU_HEAD(head) + offset, value, size);
}

static uint64_t load_ok(QTestState *qts, unsigned head, uint64_t offset,
                        unsigned size)
{
    uint64_t value = 0;

    g_assert_cmpint(load(qts, head, offset, size, &value), ==, MEMTX_OK);
    return value;
}

static void store_ok(QTestState *qts, unsigned head, uint64_t offset,
                     unsigned size, uint64_t value)
{
    g_assert_cmpint(memtx_store(qts, CPU_HEAD(head) + offset, value, size),
                    ==, MEMTX_OK);
}

static uint64_t view(QTestState *qts, unsigned head)
{
    return load_ok(qts, head, ERROR_VIEW, 8);
}

static uint64_t status(QTestState *qts, unsigned head)
{
    return load_ok(qts, head, STATUS, 8);
}

/* The PROM's write before every probe, the low word of the reset register. */
static void prom_clear(QTestState *qts, unsigned head)
{
    store_ok(qts, head, RESET + 4, 4, 0x41);
}

static uint64_t width_mask(unsigned size)
{
    return size == 8 ? UINT64_MAX : (1ULL << (size * 8)) - 1;
}

/* One read at the given width fails and the value delivered is all ones. */
static void assert_read_fails(QTestState *qts, unsigned head,
                              unsigned window, unsigned function)
{
    static const struct {
        unsigned size;
        unsigned lane;      /* big-endian lane of the low-order bytes */
    } widths[] = { { 4, 0 }, { 2, 2 }, { 1, 3 } };

    for (unsigned i = 0; i < G_N_ELEMENTS(widths); i++) {
        uint64_t value = 0;

        g_assert_cmpint(load(qts, head,
                             CFG(window, function) + widths[i].lane,
                             widths[i].size, &value), !=, MEMTX_OK);
        g_assert_cmphex(value, ==, width_mask(widths[i].size));
    }
}

/*
 * What every failed read leaves: error view bit 11 set, seen at 64 bits
 * and through the low word, and status bit 11 clear because no enable is
 * set (the rule that a status bit sets only when enabled).
 */
static void assert_timeout_recorded_view_only(QTestState *qts, unsigned head)
{
    g_assert_cmphex(view(qts, head) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    g_assert_cmphex(load_ok(qts, head, ERROR_VIEW + 4, 4) & TIMEOUT_BIT, ==,
                    TIMEOUT_BIT);
    g_assert_cmphex(status(qts, head) & TIMEOUT_BIT, ==, 0);
    g_assert_cmphex(load_ok(qts, head, STATUS + 4, 4) & TIMEOUT_BIT, ==, 0);
}

/*
 * An unclaimed read fails at 8, 16 and 32 bits and delivers all ones.
 * Device 0 is one of the unclaimed windows, an approximation until a PIC
 * identity is sourced.
 */
static void test_unclaimed_read_fails_at_every_width(void)
{
    static const struct {
        unsigned head;
        unsigned window;
        unsigned function;
    } probes[] = {
        { 0, 0, 0 }, { 0, 2, 0 }, { 0, 2, 1 }, { 0, 5, 1 },
        { 0, 4, 0 }, { 0, 5, 0 }, { 0, 7, 0 }, { 0, 1, 1 },
        { 1, 0, 0 }, { 1, 2, 0 }, { 1, 2, 1 }, { 1, 5, 1 },
        { 1, 4, 0 }, { 1, 5, 0 }, { 1, 7, 0 },
    };
    QTestState *qts = pic_start("");

    for (unsigned i = 0; i < G_N_ELEMENTS(probes); i++) {
        unsigned head = probes[i].head;

        prom_clear(qts, head);
        g_assert_cmphex(view(qts, head), ==, 0);
        assert_read_fails(qts, head, probes[i].window, probes[i].function);
        assert_timeout_recorded_view_only(qts, head);
    }
    qtest_quit(qts);
}

/* The PROM's low-word 0x41 and an aligned 0x41 both clear the view. */
static void test_reset_write_clears_the_view(void)
{
    QTestState *qts = pic_start("");
    uint64_t value;

    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 0), !=, 0);
    prom_clear(qts, 0);
    g_assert_cmphex(view(qts, 0), ==, 0);

    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 0), !=, 0);
    store_ok(qts, 0, RESET, 8, 0x41);
    g_assert_cmphex(view(qts, 0), ==, 0);

    /* Any write to the register clears the view, not only 0x41. */
    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    store_ok(qts, 0, RESET, 8, 0);
    g_assert_cmphex(view(qts, 0), ==, 0);
    qtest_quit(qts);
}

/* A failed read sets the view of its own head only. */
static void test_views_are_per_head(void)
{
    QTestState *qts = pic_start("");
    uint64_t value;

    for (unsigned head = 0; head < 2; head++) {
        g_assert_cmpint(load(qts, head, CFG(2, 0), 4, &value), !=, MEMTX_OK);
        g_assert_cmphex(view(qts, head) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
        g_assert_cmphex(view(qts, !head), ==, 0);
        prom_clear(qts, head);
    }
    qtest_quit(qts);
}

/* The view is read-only, and the refused write is logged. */
static void test_error_view_is_read_only(void)
{
    g_autofree char *dir = g_dir_make_tmp("sgi-pic-empty-slot-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *extra = g_strdup_printf("-D %s", path);
    g_autofree char *log = NULL;
    QTestState *qts = pic_start(extra);
    uint64_t value;

    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    store_ok(qts, 0, ERROR_VIEW, 8, TIMEOUT_BIT << 1);
    g_assert_cmphex(view(qts, 0), ==, TIMEOUT_BIT);
    prom_clear(qts, 0);
    store_ok(qts, 0, ERROR_VIEW, 8, TIMEOUT_BIT);
    g_assert_cmphex(view(qts, 0), ==, 0);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log,
        "origin=widget-mmio write offset 0x170 size 8 value 0x1000 "
        "unsupported"));
    unlink(path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

/*
 * A configuration write to an empty window goes nowhere. It does not fail,
 * records nothing and sends no packet. The sheet states no write rule, so
 * this is the model's choice, labelled inferred.
 */
static void test_unclaimed_write_goes_nowhere(void)
{
    QTestState *qts = pic_start("");
    uint64_t packets = 0;
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'x-test-linked-packet-count'}}", PIC_CHILD);

    packets = qdict_get_int(reply, "return");
    for (unsigned head = 0; head < 2; head++) {
        store_ok(qts, head, CFG(2, 0) + 4, 4, PCI_COMMAND_MEMORY);
        store_ok(qts, head, CFG(2, 1) + 7, 1, 0x5a);
        g_assert_cmphex(view(qts, head), ==, 0);
        g_assert_cmphex(status(qts, head), ==, 0);
    }
    qobject_unref(reply);
    reply = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,"
        "'property':'x-test-linked-packet-count'}}", PIC_CHILD);
    g_assert_cmpuint(qdict_get_int(reply, "return"), ==, packets);
    qtest_quit(qts);
}

/* A reset clears the view. */
static void test_reset_clears_the_view(void)
{
    QTestState *qts = pic_start("");
    uint64_t value;

    g_assert_cmpint(load(qts, 1, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 1), !=, 0);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(view(qts, 1), ==, 0);
    qtest_quit(qts);
}

/*
 * With enable bit 11 set a failed read sets status bit 11 as well. A 0x41
 * write leaves it set, a one at bit 11 clears just that bit, and another
 * set status bit survives. The second bit is the invalid-address error,
 * which a 32-bit write to head offset 0x7c sets.
 *
 * Not modelled: pic.md does not say the enabled bit raises an interrupt
 * packet, and which destination it would use is unknown.
 */
static void test_enabled_timeout_sets_status(void)
{
    QTestState *qts = pic_start("");
    uint64_t value;

    store_ok(qts, 0, ENABLE, 8, TIMEOUT_BIT | INVALID_ADDRESS_BIT);
    store_ok(qts, 0, 0x7c, 4, 0);
    g_assert_cmphex(status(qts, 0) & INVALID_ADDRESS_BIT, ==,
                    INVALID_ADDRESS_BIT);

    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(status(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    g_assert_cmphex(view(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    g_assert_cmphex(load_ok(qts, 0, STATUS + 4, 4) & TIMEOUT_BIT, ==,
                    TIMEOUT_BIT);

    /* The PROM's 0x41 clears the view and leaves the status bits. */
    prom_clear(qts, 0);
    g_assert_cmphex(view(qts, 0), ==, 0);
    g_assert_cmphex(status(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    g_assert_cmphex(status(qts, 0) & INVALID_ADDRESS_BIT, ==,
                    INVALID_ADDRESS_BIT);

    /* One bit at a time: bit 11 goes, bit 24 stays. */
    store_ok(qts, 0, RESET, 8, TIMEOUT_BIT);
    g_assert_cmphex(status(qts, 0) & TIMEOUT_BIT, ==, 0);
    g_assert_cmphex(status(qts, 0) & INVALID_ADDRESS_BIT, ==,
                    INVALID_ADDRESS_BIT);
    g_assert_cmphex(status(qts, 1), ==, 0);
    qtest_quit(qts);
}

/*
 * Negative requirement: a model that latched status bit 11 whatever the
 * enable fails here, and would make the PROM skip every device after an
 * empty one.
 */
static void test_status_needs_the_enable(void)
{
    QTestState *qts = pic_start("");
    uint64_t value;

    store_ok(qts, 0, ENABLE, 8, 1ULL << 10);
    g_assert_cmpint(load(qts, 0, CFG(2, 0), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(status(qts, 0) & TIMEOUT_BIT, ==, 0);
    g_assert_cmphex(view(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    qtest_quit(qts);
}

/*
 * One PROM probe: the 0x41 write, a configuration read, and "absent" if the
 * read fails or status bit 11 is set.
 */
static bool probe_present(QTestState *qts, unsigned head, unsigned window,
                          uint32_t *id)
{
    uint64_t value = 0;
    MemTxResult result;

    prom_clear(qts, head);
    result = load(qts, head, CFG(window, 0), 4, &value);
    if (result != MEMTX_OK) {
        return false;
    }
    *id = value;
    return !(load_ok(qts, head, STATUS + 4, 4) & TIMEOUT_BIT);
}

/* A device after an empty one is found, as on the real head. */
static void test_device_after_an_empty_one_is_found(void)
{
    QTestState *qts = pic_start("");
    uint32_t id = 0;

    g_assert_false(probe_present(qts, 1, 1, &id));
    g_assert_false(probe_present(qts, 1, 2, &id));
    g_assert_true(probe_present(qts, 1, 3, &id));
    g_assert_cmphex(id, ==, 0x3503feed);
    g_assert_false(probe_present(qts, 1, 4, &id));
    qtest_quit(qts);
}

/* Function 1 follows the card's header type. */
static void test_functions_follow_the_card(void)
{
    QTestState *qts = pic_start("");
    uint64_t value = 0;

    /* Single function: bit 7 of the header type clear, function 1 faults. */
    g_assert_cmphex((load_ok(qts, 1, CFG(3, 0) + 0xc, 4) >> 16) & 0x80, ==, 0);
    g_assert_cmphex((load_ok(qts, 0, CFG(1, 0) + 0xc, 4) >> 16) & 0x80, ==, 0);
    g_assert_cmpint(load(qts, 1, CFG(3, 1), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 1) & TIMEOUT_BIT, ==, TIMEOUT_BIT);
    prom_clear(qts, 0);
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0), 4), ==, 0x50011234);
    g_assert_cmphex(view(qts, 0), ==, 0);
    g_assert_cmpint(load(qts, 0, CFG(1, 1), 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);

    /* Multifunction: bit 7 set, function 1 reads normally. */
    prom_clear(qts, 0);
    g_assert_cmphex((load_ok(qts, 0, CFG(3, 0) + 0xc, 4) >> 16) & 0x80, ==,
                    0x80);
    g_assert_cmphex(load_ok(qts, 0, CFG(3, 0), 4), ==, 0x3510feed);
    g_assert_cmphex(load_ok(qts, 0, CFG(3, 1), 4), ==, 0x3511feed);
    g_assert_cmphex(view(qts, 0), ==, 0);
    /* Function 2 is not there. */
    g_assert_cmpint(load(qts, 0, CFG(3, 2), 4, &value), !=, MEMTX_OK);
    qtest_quit(qts);
}

/* A claimed read, at every width, and a write that sticks. */
static void test_claimed_devices_are_unaffected(void)
{
    QTestState *qts = pic_start("");

    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0), 4), ==, 0x50011234);
    /* Big-endian lanes: the high-order bytes sit at the low offsets. */
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0), 2), ==, 0x5001);
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0) + 2, 2), ==, 0x1234);
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0), 1), ==, 0x50);
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0) + 3, 1), ==, 0x34);
    store_ok(qts, 0, CFG(1, 0) + 4, 4, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(load_ok(qts, 0, CFG(1, 0) + 4, 4) & 7, ==,
                    PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    g_assert_cmphex(view(qts, 0), ==, 0);
    g_assert_cmphex(status(qts, 0), ==, 0);
    qtest_quit(qts);
}

/*
 * A type-1 cycle to a missing bus or an empty device fails; one that
 * selects the populated device reads it. Taking bus 0 device n as window n
 * is the model's own choice: pic.md maps the bus-0 windows by device but
 * does not say how a type-1 cycle's device selects among them.
 */
static void test_type1_cycles_to_missing_targets_fail(void)
{
    QTestState *qts = pic_start("");
    uint64_t value = 0;

    store_ok(qts, 0, 0xc8, 8, (1ULL << 16) | (2 << 11));
    g_assert_cmpint(load(qts, 0, TYPE1_WINDOW, 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xffffffff);
    g_assert_cmphex(view(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);

    prom_clear(qts, 0);
    store_ok(qts, 0, 0xc8, 8, 4 << 11);
    g_assert_cmpint(load(qts, 0, TYPE1_WINDOW, 4, &value), !=, MEMTX_OK);
    g_assert_cmphex(view(qts, 0) & TIMEOUT_BIT, ==, TIMEOUT_BIT);

    prom_clear(qts, 0);
    store_ok(qts, 0, 0xc8, 8, 1 << 11);
    g_assert_cmphex(load_ok(qts, 0, TYPE1_WINDOW, 4), ==, 0x50011234);
    g_assert_cmphex(view(qts, 0), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-pic/empty-slot/unclaimed-read-fails-at-every-width",
                   test_unclaimed_read_fails_at_every_width);
    qtest_add_func("/sgi-pic/empty-slot/reset-write-clears-the-view",
                   test_reset_write_clears_the_view);
    qtest_add_func("/sgi-pic/empty-slot/views-are-per-head",
                   test_views_are_per_head);
    qtest_add_func("/sgi-pic/empty-slot/error-view-is-read-only",
                   test_error_view_is_read_only);
    qtest_add_func("/sgi-pic/empty-slot/unclaimed-write-goes-nowhere",
                   test_unclaimed_write_goes_nowhere);
    qtest_add_func("/sgi-pic/empty-slot/reset-clears-the-view",
                   test_reset_clears_the_view);
    qtest_add_func("/sgi-pic/empty-slot/enabled-timeout-sets-status",
                   test_enabled_timeout_sets_status);
    qtest_add_func("/sgi-pic/empty-slot/status-needs-the-enable",
                   test_status_needs_the_enable);
    qtest_add_func("/sgi-pic/empty-slot/device-after-an-empty-one-is-found",
                   test_device_after_an_empty_one_is_found);
    qtest_add_func("/sgi-pic/empty-slot/functions-follow-the-card",
                   test_functions_follow_the_card);
    qtest_add_func("/sgi-pic/empty-slot/claimed-devices-are-unaffected",
                   test_claimed_devices_are_unaffected);
    qtest_add_func("/sgi-pic/empty-slot/type1-cycles-to-missing-targets-fail",
                   test_type1_cycles_to_missing_targets_fail);
    return g_test_run();
}
