/*
 * SGI Router register contract.
 *
 * The Router is reached through vector operations, not a processor MMIO
 * window.  Exercise the device's vector target directly here; the routed
 * fabric qtest will cover the Hub-to-Router transport once that adapter exists.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/misc/sgi-router.h"
#include "io/channel-file.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "system/reset.h"

#define RR_STATUS_REV_ID       0x00000
#define RR_PORT_RESET          0x00008
#define RR_PROTECTION_CONFIG   0x00010
#define RR_GLOBAL_PARMS        0x00018
#define RR_SCRATCH_REG0        0x00020
#define RR_SCRATCH_REG1        0x00028
#define RR_DIAG_PARMS          0x00030
#define RR_NIC_ULAN            0x00038
#define RR_BIST_DATA           0x00050
#define RR_BIST_READY          0x00058
#define RR_BIST_COUNT_TARGET   0x00060
#define RR_BIST_SHIFT_LOAD     0x00068
#define RR_BIST_SHIFT_UNLOAD   0x00070
#define RR_BIST_ENTER_RUN      0x00078

#define RR_PORT_BASE(port)     ((uint64_t)(port) << 16)
#define RR_PORT_PARMS(port)    (RR_PORT_BASE(port) + 0x0000)
#define RR_STATUS_ERROR(port)  (RR_PORT_BASE(port) + 0x0008)
#define RR_HISTOGRAM(port)     (RR_PORT_BASE(port) + 0x0010)
#define RR_RESET_MASK(port)    (RR_PORT_BASE(port) + 0x0018)
#define RR_ERROR_CLEAR(port)   (RR_PORT_BASE(port) + 0x0088)

#define RR_META_TABLE(index)   (0x70000 + UINT64_C(8) * (index))
#define RR_LOCAL_TABLE(index)  (0x70100 + UINT64_C(8) * (index))

#define STATUS_CHIP_ID         UINT64_C(1)
#define STATUS_REVISION        (UINT64_C(1) << 4)
#define STATUS_INPUTS(value)   ((uint64_t)(value) << 8)
#define STATUS_OUTPUTS(value)  ((uint64_t)(value) << 12)
#define STATUS_LEDS(value)     ((uint64_t)(value) << 16)
#define STATUS_LOCAL_ERRORS    (UINT64_C(0xf) << 24)
#define STATUS_PORT_RESET_FAIL(port) (UINT64_C(1) << (25 + 3 * (port)))
#define STATUS_PORT_WORKING(port)    (UINT64_C(1) << (26 + 3 * (port)))
#define STATUS_PORT_EIGHT_BIT(port)  (UINT64_C(1) << (27 + 3 * (port)))
#define STATUS_INPUT_PORT(port)      ((uint64_t)(port) << 46)

#define PROTECTION_RESET       UINT64_C(0x3f)
#define PROTECTION_MASK        UINT64_C(0x1fffff)
#define GLOBAL_READ_ONLY       (UINT64_C(3) << 10)
#define GLOBAL_AGE_WRAP        (UINT64_C(0xff) << 28)
#define GLOBAL_RESET           UINT64_C(0x3fffffe0a01ffff1)
#define PORT_PARMS_RESET       UINT64_C(0x1bf0)
#define PORT_HISTOGRAM_MODE(mode) ((uint64_t)(mode) << 18)
#define PORT_HISTOGRAM_VC(vc)     ((uint64_t)(vc) << 16)
#define PORT_HISTOGRAM_FIELDS     UINT64_C(0xf0000)
#define PORT_ERROR_MASK        UINT64_C(0x3ffffffff)
#define DIAG_ABSOLUTE_HISTOGRAM (UINT64_C(1) << 19)
#define HISTOGRAM_TICK_NS      20
#define NIC_DIVIDERS_RESET     ((UINT64_C(0x31) << 27) | \
                                (UINT64_C(0x31) << 20))
#define NIC_DONE               (UINT64_C(1) << 1)

static int64_t virtual_time;

int64_t cpus_get_virtual_clock(void)
{
    return virtual_time;
}

void cpus_set_virtual_clock(int64_t value)
{
    virtual_time = value;
}

typedef struct Fixture {
    DeviceState *dev;
    SGIRouterState *router;
} Fixture;

static DeviceState *new_router(uint8_t chip_inputs)
{
    DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);

    qdev_prop_set_uint8(dev, "chip-inputs", chip_inputs);
    qdev_realize(dev, NULL, &error_abort);
    device_cold_reset(dev);
    return dev;
}

static void fixture_setup(Fixture *f, gconstpointer data)
{
    f->dev = new_router(GPOINTER_TO_UINT(data));
    f->router = SGI_ROUTER(f->dev);
}

static void fixture_teardown(Fixture *f, gconstpointer data)
{
    object_unparent(OBJECT(f->dev));
    object_unref(OBJECT(f->dev));
}

static bool access_reg(SGIRouterState *router, unsigned int port,
                       SN0FabricVectorOp op, uint64_t addr, uint64_t data,
                       uint64_t *value)
{
    SN0FabricVectorAccess access = {
        .op = op,
        .addr = addr,
        .data = data,
    };
    bool accepted = sgi_router_access(router, port, &access);

    if (value) {
        *value = access.value;
    }
    return accepted;
}

static uint64_t read_reg(Fixture *f, unsigned int port, uint64_t addr)
{
    uint64_t value = UINT64_MAX;

    g_assert_true(access_reg(f->router, port, SN0_FABRIC_VECTOR_READ,
                             addr, 0, &value));
    return value;
}

static void write_reg(Fixture *f, unsigned int port, uint64_t addr,
                      uint64_t value)
{
    g_assert_true(access_reg(f->router, port, SN0_FABRIC_VECTOR_WRITE,
                             addr, value, NULL));
}

static void select_histogram(Fixture *f, unsigned int port,
                             unsigned int mode, unsigned int vc)
{
    uint64_t parms = read_reg(f, 1, RR_PORT_PARMS(port));

    write_reg(f, 1, RR_PORT_PARMS(port),
              (parms & ~PORT_HISTOGRAM_FIELDS) |
              PORT_HISTOGRAM_MODE(mode) | PORT_HISTOGRAM_VC(vc));
}

static uint16_t histogram_bucket(uint64_t histogram, unsigned int bucket)
{
    return extract64(histogram, bucket * 16, 16);
}

typedef struct Observer {
    Fixture *fixture;
    unsigned int calls;
    uint64_t address;
    uint64_t expected;
} Observer;

static void observe_change(void *opaque)
{
    Observer *observer = opaque;

    observer->calls++;
    g_assert_cmphex(read_reg(observer->fixture, 1, observer->address), ==,
                    observer->expected);
    /* Publishing physical link state must not re-enter the observer. */
    sgi_router_set_port_link(observer->fixture->router, 3, true, true);
}

static void test_nic_slot_deadline(Fixture *f, gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t request = NIC_DIVIDERS_RESET | (UINT64_C(520) << 10) |
                       (UINT64_C(65) << 2);

    write_reg(f, 1, RR_NIC_ULAN, request);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN), ==, NIC_DIVIDERS_RESET);
    qemu_clock_advance_virtual_time(start + 520000);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==, 0);
    qemu_clock_advance_virtual_time(start + 584999);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==, 0);
    qemu_clock_advance_virtual_time(start + 585000);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==, NIC_DONE);
}

static void test_change_notifications(Fixture *f, gconstpointer unused)
{
    Observer observer = { .fixture = f };
    const uint64_t addresses[] = {
        RR_LOCAL_TABLE(15), RR_META_TABLE(31), RR_PROTECTION_CONFIG,
    };
    unsigned int i;

    sgi_router_set_changed(f->router, observe_change, &observer);
    g_assert_cmpuint(observer.calls, ==, 0);
    for (i = 0; i < G_N_ELEMENTS(addresses); i++) {
        observer.address = addresses[i];
        observer.expected = i == 2 ? PROTECTION_MASK : 0xffffff;
        write_reg(f, 1, addresses[i], UINT64_MAX);
        g_assert_cmpuint(observer.calls, ==, i + 1);
    }
    write_reg(f, 1, RR_SCRATCH_REG0, 1);
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                              RR_LOCAL_TABLE(16), 1, NULL));
    g_assert_cmpuint(observer.calls, ==, 3);

    observer.expected = PROTECTION_RESET;
    resettable_assert_reset(OBJECT(f->dev), RESET_TYPE_COLD);
    g_assert_cmpuint(observer.calls, ==, 3);
    resettable_release_reset(OBJECT(f->dev), RESET_TYPE_COLD);
    g_assert_cmpuint(observer.calls, ==, 4);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_REV_ID) & STATUS_PORT_WORKING(3),
                    ==, STATUS_PORT_WORKING(3));

    sgi_router_set_changed(f->router, NULL, NULL);
    write_reg(f, 1, RR_PROTECTION_CONFIG, 0);
    device_cold_reset(f->dev);
    g_assert_cmpuint(observer.calls, ==, 4);
}

static void test_identity_and_link_state(Fixture *f, gconstpointer unused)
{
    DeviceState *r2 = new_router(0xe);
    SGIRouterState *r2_router = SGI_ROUTER(r2);
    uint64_t fixed = STATUS_CHIP_ID | STATUS_REVISION | STATUS_INPUTS(0xf) |
                     STATUS_OUTPUTS(1);
    uint64_t value;

    g_assert_cmphex(read_reg(f, 4, RR_STATUS_REV_ID), ==,
                    fixed | STATUS_INPUT_PORT(4));
    sgi_router_set_port_link(f->router, 2, true, true);
    g_assert_cmphex(read_reg(f, 6, RR_STATUS_REV_ID), ==,
                    fixed | STATUS_INPUT_PORT(6) | STATUS_PORT_WORKING(2) |
                    STATUS_PORT_EIGHT_BIT(2));
    sgi_router_set_port_link(f->router, 2, false, false);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_REV_ID), ==,
                    fixed | STATUS_INPUT_PORT(1));

    g_assert_true(access_reg(r2_router, 5, SN0_FABRIC_VECTOR_READ,
                             RR_STATUS_REV_ID, 0, &value));
    g_assert_cmphex(value, ==,
                    STATUS_CHIP_ID | STATUS_REVISION | STATUS_INPUTS(0xe) |
                    STATUS_OUTPUTS(1) | STATUS_INPUT_PORT(5));
    object_unparent(OBJECT(r2));
    object_unref(OBJECT(r2));
}

static void test_fabric_access_boundary(Fixture *f, gconstpointer unused)
{
    SN0FabricVectorAccess access = {
        .op = SN0_FABRIC_VECTOR_READ,
        .addr = RR_STATUS_REV_ID,
        .source = 0x123,
    };

    g_assert_true(sgi_router_access(f->router, 3, &access));
    g_assert_cmphex(access.value & STATUS_INPUT_PORT(7), ==,
                    STATUS_INPUT_PORT(3));
    g_assert_cmphex(access.source, ==, 0x123);

    access.op = SN0_FABRIC_VECTOR_INVALID;
    g_assert_false(sgi_router_access(f->router, 3, &access));
    access.op = SN0_FABRIC_VECTOR_READ;
    access.addr = RR_STATUS_REV_ID;
    g_assert_false(sgi_router_access(f->router, 0, &access));
    g_assert_false(sgi_router_access(f->router, 7, &access));
    access.addr = UINT64_C(1) << 20;
    g_assert_false(sgi_router_access(f->router, 1, &access));
}

static void test_reset_values(Fixture *f, gconstpointer unused)
{
    unsigned int port;

    g_assert_cmphex(read_reg(f, 1, RR_PROTECTION_CONFIG), ==,
                    PROTECTION_RESET);
    g_assert_cmphex(read_reg(f, 1, RR_GLOBAL_PARMS), ==, GLOBAL_RESET);
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG0), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG1), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_DIAG_PARMS), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN), ==,
                    NIC_DIVIDERS_RESET | NIC_DONE);
    g_assert_cmphex(read_reg(f, 1, RR_BIST_DATA), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_BIST_READY), ==, 1);
    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        g_assert_cmphex(read_reg(f, 1, RR_PORT_PARMS(port)), ==,
                        PORT_PARMS_RESET);
        g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(port)), ==, 0);
        g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(port)), ==, 0);
        g_assert_cmphex(read_reg(f, 1, RR_RESET_MASK(port)), ==, 0x3f);
    }
}

static void test_register_storage_masks(Fixture *f, gconstpointer unused)
{
    uint64_t global;
    unsigned int port;

    write_reg(f, 1, RR_PROTECTION_CONFIG, UINT64_MAX);
    g_assert_cmphex(read_reg(f, 1, RR_PROTECTION_CONFIG), ==,
                    PROTECTION_MASK);
    write_reg(f, 1, RR_GLOBAL_PARMS, 0);
    g_assert_cmphex(read_reg(f, 1, RR_GLOBAL_PARMS), ==, GLOBAL_READ_ONLY);
    global = read_reg(f, 1, RR_GLOBAL_PARMS);
    write_reg(f, 1, RR_GLOBAL_PARMS,
              (global & ~GLOBAL_AGE_WRAP) | (UINT64_C(0x20) << 28));
    g_assert_cmphex(read_reg(f, 1, RR_GLOBAL_PARMS) & GLOBAL_AGE_WRAP, ==,
                    UINT64_C(0x20) << 28);

    write_reg(f, 1, RR_SCRATCH_REG0, UINT64_C(0x0123456789abcdef));
    write_reg(f, 1, RR_SCRATCH_REG1, UINT64_C(0x123456789abcdef0));
    write_reg(f, 1, RR_DIAG_PARMS, UINT64_MAX);
    write_reg(f, 1, RR_BIST_DATA, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG0), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG1), ==, UINT64_C(0xdef0));
    g_assert_cmphex(read_reg(f, 1, RR_DIAG_PARMS), ==, UINT64_C(0xfffff));
    g_assert_cmphex(read_reg(f, 1, RR_BIST_DATA), ==,
                    UINT64_C(0xfedcba9876543210));
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                              RR_BIST_READY, UINT64_MAX, NULL));
    g_assert_cmphex(read_reg(f, 1, RR_BIST_READY), ==, 1);

    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        write_reg(f, 1, RR_PORT_PARMS(port), UINT64_MAX);
        write_reg(f, 1, RR_RESET_MASK(port), UINT64_MAX);
        g_assert_cmphex(read_reg(f, 1, RR_PORT_PARMS(port)), ==,
                        UINT64_C(0xfffff));
        g_assert_cmphex(read_reg(f, 1, RR_RESET_MASK(port)), ==,
                        UINT64_C(0x3f));
    }

    write_reg(f, 1, RR_STATUS_REV_ID,
              STATUS_OUTPUTS(0xa) | STATUS_LEDS(0x25) | (UINT64_C(3) << 22));
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_REV_ID) &
                    (STATUS_OUTPUTS(0xf) | STATUS_LEDS(0x3f) |
                     (UINT64_C(3) << 22)), ==,
                    STATUS_OUTPUTS(0xa) | STATUS_LEDS(0x25) |
                    (UINT64_C(3) << 22));
}

static void test_conditional_exchange(Fixture *f, gconstpointer unused)
{
    uint64_t old;

    g_assert_true(access_reg(f->router, 1, SN0_FABRIC_VECTOR_EXCHANGE,
                             RR_SCRATCH_REG1, 0xffff, &old));
    g_assert_cmphex(old, ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG1), ==, 0xffff);
    g_assert_true(access_reg(f->router, 1, SN0_FABRIC_VECTOR_EXCHANGE,
                             RR_SCRATCH_REG1, 0x1234, &old));
    g_assert_cmphex(old, ==, 0xffff);
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG1), ==, 0xffff);
    write_reg(f, 1, RR_SCRATCH_REG1, 0);
    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG1), ==, 0);
}

static void test_error_clear_addresses(Fixture *f, gconstpointer unused)
{
    uint64_t latched = UINT64_C(0x312345678);

    sgi_router_latch_port_error(f->router, 5, latched);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(5)), ==,
                    latched & PORT_ERROR_MASK);
    write_reg(f, 1, RR_STATUS_ERROR(5), UINT64_C(0x010000078));
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(5)), ==,
                    (latched & PORT_ERROR_MASK) & ~UINT64_C(0x010000078));

    sgi_router_latch_port_error(f->router, 5, UINT64_C(0x200000005));
    g_assert_cmphex(read_reg(f, 1, RR_ERROR_CLEAR(5)), ==,
                    ((latched & PORT_ERROR_MASK) & ~UINT64_C(0x010000078)) |
                    UINT64_C(0x200000005));
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(5)), ==, 0);

    sgi_router_latch_port_error(f->router, 5, UINT64_C(0x123));
    write_reg(f, 1, RR_ERROR_CLEAR(5), 0);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(5)), ==, 0);

    sgi_router_latch_local_error(f->router, STATUS_LOCAL_ERRORS);
    write_reg(f, 1, RR_STATUS_REV_ID, UINT64_C(0x5) << 24);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_REV_ID) & STATUS_LOCAL_ERRORS, ==,
                    UINT64_C(0xa) << 24);
}

static void test_error_counters_saturate(Fixture *f, gconstpointer unused)
{
    for (unsigned int i = 0; i < 300; i++) {
        sgi_router_record_port_error(f->router, 2,
                                     SGI_ROUTER_PORT_SEQUENCE_ERROR);
        sgi_router_record_port_error(f->router, 2,
                                     SGI_ROUTER_PORT_CHECK_ERROR);
        sgi_router_record_port_error(f->router, 2,
                                     SGI_ROUTER_PORT_RETRY);
    }
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(2)), ==, 0xffffff);
}

static void test_histogram_event_boundaries(Fixture *f,
                                            gconstpointer unused)
{
    select_histogram(f, 1, 1, 0);

    sgi_router_histogram_receive(f->router, 1, false, true, true);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    sgi_router_histogram_receive(f->router, 1, true, true, false);
    sgi_router_histogram_receive(f->router, 1, true, true, true);
    sgi_router_histogram_send(f->router, 1, false);
    sgi_router_histogram_send(f->router, 1, true);

    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0000000100030001));
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, 0);
}

static void test_histogram_clear(Fixture *f, gconstpointer unused)
{
    uint64_t before;
    uint64_t clear = UINT64_C(0x0000000100010001);

    select_histogram(f, 1, 1, 0);
    select_histogram(f, 6, 1, 0);
    for (unsigned int i = 0; i < 3; i++) {
        sgi_router_histogram_receive(f->router, 1, true, i < 1, true);
        sgi_router_histogram_send(f->router, 1, true);
    }
    sgi_router_histogram_receive(f->router, 6, true, true, true);
    sgi_router_histogram_receive(f->router, 6, true, true, true);

    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0000000300030001));
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(6)), ==,
                    UINT64_C(0x0000000000020002));
    write_reg(f, 1, RR_HISTOGRAM(1), 0);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(6)), ==,
                    UINT64_C(0x0000000000020002));

    for (unsigned int i = 0; i < 5; i++) {
        sgi_router_histogram_receive(f->router, 1, true, i < 3, true);
    }
    for (unsigned int i = 0; i < 7; i++) {
        sgi_router_histogram_send(f->router, 1, true);
    }
    before = read_reg(f, 1, RR_HISTOGRAM(1));
    g_assert_cmphex(before, ==, UINT64_C(0x0000000700050003));
    write_reg(f, 1, RR_HISTOGRAM(1), clear);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==, before & ~clear);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(6)), ==,
                    UINT64_C(0x0000000000020002));
}

static void test_histogram_timebase_and_phase(Fixture *f,
                                              gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    select_histogram(f, 1, 1, 0);
    select_histogram(f, 6, 1, 0);
    qemu_clock_advance_virtual_time(start + 19);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==, 0);
    qemu_clock_advance_virtual_time(start + 60);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 3);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(6)), 3),
                     ==, 3);
    g_usleep(1000);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 3);

    select_histogram(f, 1, 0, 0);
    qemu_clock_advance_virtual_time(start + 85);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 3);
    select_histogram(f, 1, 1, 0);
    sgi_router_set_port_link(f->router, 1, false, false);
    write_reg(f, 1, RR_DIAG_PARMS, UINT64_C(1) << 6);
    qemu_clock_advance_virtual_time(start + 99);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 3);
    qemu_clock_advance_virtual_time(start + 100);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 4);
    write_reg(f, 1, RR_DIAG_PARMS,
              (UINT64_C(1) << 6) | DIAG_ABSOLUTE_HISTOGRAM);
    qemu_clock_advance_virtual_time(start + 125);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 4);
    write_reg(f, 1, RR_DIAG_PARMS, UINT64_C(1) << 6);
    qemu_clock_advance_virtual_time(start + 139);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 4);
    qemu_clock_advance_virtual_time(start + 163);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 6);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(6)), 3),
                     ==, 7);
}

static void prepare_histogram_callback_order(Fixture *f)
{
    select_histogram(f, 1, 1, 0);
    sgi_router_histogram_send(f->router, 1, true);
    sgi_router_histogram_send(f->router, 1, true);
    for (unsigned int i = 0; i < 0xffff; i++) {
        sgi_router_histogram_receive(f->router, 1, true, false, true);
    }
}

static void test_histogram_equal_time_order(Fixture *f,
                                            gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    select_histogram(f, 1, 1, 0);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    qemu_clock_advance_virtual_time(
        start + UINT64_C(0xffff) * HISTOGRAM_TICK_NS);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 0xffff);
    qemu_clock_advance_virtual_time(
        start + UINT64_C(0x10000) * HISTOGRAM_TICK_NS);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 1),
                     ==, 2);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 0x8000);

    device_cold_reset(f->dev);
    start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    select_histogram(f, 1, 1, 0);
    qemu_clock_advance_virtual_time(start + HISTOGRAM_TICK_NS);
    select_histogram(f, 1, 0, 0);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 1);

    device_cold_reset(f->dev);
    start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    select_histogram(f, 1, 1, 0);
    qemu_clock_advance_virtual_time(start + HISTOGRAM_TICK_NS);
    write_reg(f, 1, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 3),
                     ==, 1);

    device_cold_reset(f->dev);
    prepare_histogram_callback_order(f);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    sgi_router_histogram_send(f->router, 1, true);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 2),
                     ==, 2);

    device_cold_reset(f->dev);
    prepare_histogram_callback_order(f);
    sgi_router_histogram_send(f->router, 1, true);
    sgi_router_histogram_receive(f->router, 1, true, false, true);
    g_assert_cmpuint(histogram_bucket(read_reg(f, 1, RR_HISTOGRAM(1)), 2),
                     ==, 1);
}

static void test_histogram_unsupported_modes(Fixture *f,
                                             gconstpointer unused)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t before;

    select_histogram(f, 2, 1, 0);
    sgi_router_histogram_receive(f->router, 2, true, true, true);
    sgi_router_histogram_send(f->router, 2, true);
    qemu_clock_advance_virtual_time(now + 40);
    before = read_reg(f, 1, RR_HISTOGRAM(2));

    select_histogram(f, 2, 0, 2);
    sgi_router_histogram_receive(f->router, 2, true, true, true);
    sgi_router_histogram_send(f->router, 2, true);
    qemu_clock_advance_virtual_time(now + 80);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, before);
    g_assert_cmphex(read_reg(f, 1, RR_PORT_PARMS(2)) &
                    PORT_HISTOGRAM_FIELDS, ==, PORT_HISTOGRAM_VC(2));

    select_histogram(f, 2, 2, 3);
    sgi_router_histogram_receive(f->router, 2, true, true, true);
    sgi_router_histogram_send(f->router, 2, true);
    qemu_clock_advance_virtual_time(now + 120);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, before);
    g_assert_cmphex(read_reg(f, 1, RR_PORT_PARMS(2)) &
                    PORT_HISTOGRAM_FIELDS, ==,
                    PORT_HISTOGRAM_MODE(2) | PORT_HISTOGRAM_VC(3));

    select_histogram(f, 2, 3, 1);
    sgi_router_histogram_receive(f->router, 2, true, true, true);
    sgi_router_histogram_send(f->router, 2, true);
    qemu_clock_advance_virtual_time(now + 160);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, before);

    select_histogram(f, 2, 1, 1);
    write_reg(f, 1, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    sgi_router_histogram_receive(f->router, 2, true, true, true);
    sgi_router_histogram_send(f->router, 2, true);
    qemu_clock_advance_virtual_time(now + 200);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, before);
    g_assert_cmphex(read_reg(f, 1, RR_DIAG_PARMS) &
                    DIAG_ABSOLUTE_HISTOGRAM, ==,
                    DIAG_ABSOLUTE_HISTOGRAM);
    write_reg(f, 1, RR_HISTOGRAM(2), 0);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(2)), ==, 0);
}

static void test_histogram_common_rescale(Fixture *f,
                                          gconstpointer unused)
{
    const uint64_t expected[] = {
        UINT64_C(0x0004000300038000),
        UINT64_C(0x0004000380000001),
        UINT64_C(0x0004800000020001),
        UINT64_C(0x8000000300020001),
    };

    for (unsigned int overflowing = 0; overflowing < 4; overflowing++) {
        int64_t start;
        uint64_t before;

        device_cold_reset(f->dev);
        start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        select_histogram(f, 1, 1, 0);
        if (overflowing != 3) {
            qemu_clock_advance_virtual_time(start + 9 * HISTOGRAM_TICK_NS);
        }

        if (overflowing == 0) {
            for (unsigned int i = 0; i < 0xffff; i++) {
                sgi_router_histogram_receive(f->router, 1, true, true, true);
            }
            write_reg(f, 1, RR_HISTOGRAM(1),
                      (UINT64_C(0xffff) ^ 5) << 16);
        } else {
            for (unsigned int i = 0; i < 3; i++) {
                sgi_router_histogram_receive(f->router, 1, true, true, true);
            }
            for (unsigned int i = 3;
                 i < (overflowing == 1 ? 0xffff : 5); i++) {
                sgi_router_histogram_receive(f->router, 1, true, false, true);
            }
        }
        for (unsigned int i = 0;
             i < (overflowing == 2 ? 0xffff : 7); i++) {
            sgi_router_histogram_send(f->router, 1, true);
        }
        if (overflowing == 3) {
            qemu_clock_advance_virtual_time(
                start + UINT64_C(0xffff) * HISTOGRAM_TICK_NS);
        }

        before = read_reg(f, 1, RR_HISTOGRAM(1));
        g_assert_cmpuint(histogram_bucket(before, overflowing), ==, 0xffff);
        switch (overflowing) {
        case 0:
            sgi_router_histogram_receive(f->router, 1, true, true, true);
            break;
        case 1:
            sgi_router_histogram_receive(f->router, 1, true, false, true);
            break;
        case 2:
            sgi_router_histogram_send(f->router, 1, true);
            break;
        case 3:
            qemu_clock_advance_virtual_time(
                start + UINT64_C(0x10000) * HISTOGRAM_TICK_NS);
            break;
        default:
            g_assert_not_reached();
        }
        g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                        expected[overflowing]);
    }
}

static void test_histogram_batched_rescale(Fixture *f,
                                           gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks;

    select_histogram(f, 1, 1, 0);
    for (unsigned int i = 0; i < 0x1235; i++) {
        sgi_router_histogram_receive(f->router, 1, true, true, true);
    }
    for (unsigned int i = 0x1235; i < 0x4567; i++) {
        sgi_router_histogram_receive(f->router, 1, true, false, true);
    }
    for (unsigned int i = 0; i < 0x89ab; i++) {
        sgi_router_histogram_send(f->router, 1, true);
    }
    ticks = UINT64_C(0x10000) + 2 * UINT64_C(0x8000) + 13;
    qemu_clock_advance_virtual_time(start + ticks * HISTOGRAM_TICK_NS);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x800d113508ac0246));

    device_cold_reset(f->dev);
    start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    select_histogram(f, 1, 1, 0);
    ticks = UINT64_C(1) << 48;
    qemu_clock_advance_virtual_time(start + ticks * HISTOGRAM_TICK_NS);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x8000) << 48);
}

static void test_routing_table_decode(Fixture *f, gconstpointer unused)
{
    uint64_t value;
    unsigned int i;

    for (i = 0; i < 32; i++) {
        write_reg(f, 1, RR_META_TABLE(i), UINT64_C(0xabc00000) | i);
        g_assert_cmphex(read_reg(f, 1, RR_META_TABLE(i)), ==,
                        UINT64_C(0xc00000) | i);
    }
    for (i = 0; i < 16; i++) {
        write_reg(f, 1, RR_LOCAL_TABLE(i), UINT64_C(0xdef00000) | i);
        g_assert_cmphex(read_reg(f, 1, RR_LOCAL_TABLE(i)), ==,
                        UINT64_C(0xf00000) | i);
    }
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_READ,
                              RR_LOCAL_TABLE(16), 0, &value));
}

static void test_valid_route_lookup(Fixture *f, gconstpointer unused)
{
    unsigned int exit_port;
    uint8_t next_turn;

    /* Own meta ID 3, valid. Destination 0x35 therefore uses local entry 5. */
    write_reg(f, 1, RR_PROTECTION_CONFIG, (UINT64_C(1) << 11) | (3 << 6));
    write_reg(f, 1, RR_LOCAL_TABLE(5), UINT64_C(4) << (4 * (6 - 1)));
    g_assert_true(sgi_router_route(f->router, 2, 4, 0x35,
                                   &exit_port, &next_turn));
    g_assert_cmpuint(exit_port, ==, 6);
    g_assert_cmpuint(next_turn, ==, 4);

    /* A destination in meta 7 uses meta entry 7 and the same exit field. */
    write_reg(f, 1, RR_META_TABLE(7), UINT64_C(2) << (4 * (6 - 1)));
    g_assert_true(sgi_router_route(f->router, 2, 4, 0x74,
                                   &exit_port, &next_turn));
    g_assert_cmpuint(exit_port, ==, 6);
    g_assert_cmpuint(next_turn, ==, 2);

    /* Force-local selects local entry 4 despite the mismatched meta ID. */
    write_reg(f, 1, RR_LOCAL_TABLE(4), UINT64_C(3) << (4 * (6 - 1)));
    write_reg(f, 1, RR_PROTECTION_CONFIG,
              (UINT64_C(1) << 12) | (UINT64_C(1) << 11) | (3 << 6));
    g_assert_true(sgi_router_route(f->router, 2, 4, 0x74,
                                   &exit_port, &next_turn));
    g_assert_cmpuint(next_turn, ==, 3);

    g_assert_true(sgi_router_resolve_turn(6, 1, &exit_port));
    g_assert_cmpuint(exit_port, ==, 1);
    g_assert_true(sgi_router_resolve_turn(1, 5, &exit_port));
    g_assert_cmpuint(exit_port, ==, 6);
    g_assert_false(sgi_router_resolve_turn(0, 1, &exit_port));
    g_assert_true(sgi_router_resolve_turn(1, 0, &exit_port));
    g_assert_cmpuint(exit_port, ==, 1);
    g_assert_false(sgi_router_resolve_turn(1, 6, &exit_port));

    write_reg(f, 1, RR_LOCAL_TABLE(4), 0);
    g_assert_true(sgi_router_route(f->router, 2, 4, 0x74,
                                   &exit_port, &next_turn));
    g_assert_cmpuint(next_turn, ==, 0);
}

static void test_next_turn_consumption(Fixture *f, gconstpointer unused)
{
    unsigned int exit_port;
    unsigned int next_exit;
    uint8_t next_turn;
    unsigned int turn;

    write_reg(f, 1, RR_PROTECTION_CONFIG, UINT64_C(1) << 12);
    /* Reset contents do not become a programmed route, even with turn zero. */
    g_assert_false(sgi_router_route(f->router, 5, 0, 4,
                                    &exit_port, &next_turn));
    for (turn = 0; turn < 16; turn++) {
        write_reg(f, 1, RR_LOCAL_TABLE(4), (uint64_t)turn << 16);
        g_assert_true(sgi_router_route(f->router, 5, 0, 4,
                                       &exit_port, &next_turn));
        g_assert_cmpuint(exit_port, ==, 5);
        g_assert_cmpuint(next_turn, ==, turn);
        /* A terminal Hub ignores this field. Another Router consumes it. */
        g_assert_cmpint(sgi_router_resolve_turn(6, next_turn, &next_exit),
                        ==, turn <= 5);
        if (turn <= 5) {
            g_assert_cmpuint(next_exit, ==, ((5 + turn) % 6) + 1);
        }
    }
    g_assert_false(sgi_router_route(f->router, 5, 6, 4,
                                    &exit_port, &next_turn));
    g_assert_false(sgi_router_route(f->router, 5, 15, 4,
                                    &exit_port, &next_turn));
}

static void test_port_disable(Fixture *f, gconstpointer unused)
{
    unsigned int disabled;
    unsigned int port;
    Observer observer = { .fixture = f, .address = RR_DIAG_PARMS };

    sgi_router_set_changed(f->router, observe_change, &observer);
    for (disabled = 1; disabled <= SGI_ROUTER_PORTS; disabled++) {
        observer.expected = UINT64_C(1) << (5 + disabled);
        write_reg(f, 1, RR_DIAG_PARMS, observer.expected);
        g_assert_cmpuint(observer.calls, ==, disabled);
        for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
            g_assert_cmpint(sgi_router_port_enabled(f->router, port),
                            ==, port != disabled);
        }
    }
    observer.expected = 0;
    device_cold_reset(f->dev);
    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        g_assert_true(sgi_router_port_enabled(f->router, port));
    }
    sgi_router_set_changed(f->router, NULL, NULL);
}

static void test_forced_width(Fixture *f, gconstpointer unused)
{
    unsigned int forced;
    unsigned int port;

    for (forced = 1; forced <= SGI_ROUTER_PORTS; forced++) {
        write_reg(f, 1, RR_DIAG_PARMS, UINT64_C(1) << (11 + forced));
        for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
            g_assert_cmpint(sgi_router_port_eight_bit_forced(f->router, port),
                            ==, port == forced);
            g_assert_true(sgi_router_port_enabled(f->router, port));
        }
    }
    device_cold_reset(f->dev);
    for (port = 1; port <= SGI_ROUTER_PORTS; port++) {
        g_assert_false(sgi_router_port_eight_bit_forced(f->router, port));
    }
}

static void test_unsupported_operations_fail(Fixture *f, gconstpointer unused)
{
    uint64_t nic = read_reg(f, 1, RR_NIC_ULAN);
    uint64_t value;

    g_assert_true(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                             RR_PORT_RESET, 0, NULL));
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                              RR_NIC_ULAN, UINT64_C(1) << 10, NULL));
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN), ==, nic);
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                              RR_BIST_COUNT_TARGET, 1, NULL));
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_WRITE,
                              RR_BIST_SHIFT_LOAD, 0, NULL));
    g_assert_false(access_reg(f->router, 1, SN0_FABRIC_VECTOR_READ,
                              RR_LOCAL_TABLE(3) + 1, 0, &value));
}

static QEMUFile *state_file(int fd, bool output)
{
    int copy = dup(fd);
    QIOChannel *ioc;
    QEMUFile *file;

    g_assert_cmpint(copy, >=, 0);
    g_assert_cmpint(lseek(copy, 0, SEEK_SET), ==, 0);
    ioc = QIO_CHANNEL(qio_channel_file_new_fd(copy));
    file = output ? qemu_file_new_output(ioc) : qemu_file_new_input(ioc);
    object_unref(OBJECT(ioc));
    return file;
}

static void test_migration(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    DeviceState *target = new_router(0xf);
    SGIRouterState *target_router = SGI_ROUTER(target);
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(f->dev)->vmsd;
    int fd = g_file_open_tmp("test-sgi-router-XXXXXX", &path, NULL);
    QEMUFile *file;
    Error *err = NULL;
    uint8_t invalid_status = 0x80;
    uint64_t value;
    Fixture target_fixture = { .dev = target, .router = target_router };
    Observer source_observer = {
        .fixture = f, .address = RR_LOCAL_TABLE(9), .expected = 0x123456,
    };
    Observer target_observer = {
        .fixture = &target_fixture,
        .address = RR_LOCAL_TABLE(9), .expected = 0x123456,
    };

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);
    write_reg(f, 1, RR_SCRATCH_REG0, UINT64_C(0xfeedface01234567));
    write_reg(f, 1, RR_LOCAL_TABLE(9), UINT64_C(0x123456));
    sgi_router_set_port_link(f->router, 4, true, true);
    sgi_router_latch_port_error(f->router, 4, UINT64_C(0x200000001));

    sgi_router_set_changed(f->router, observe_change, &source_observer);
    sgi_router_set_changed(target_router, observe_change, &target_observer);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, vmsd, f->router,
                                      NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, vmsd, target_router,
                                      vmsd->version_id, &error_abort), ==, 0);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    g_assert_true(access_reg(target_router, 1, SN0_FABRIC_VECTOR_READ,
                             RR_SCRATCH_REG0, 0, &value));
    g_assert_cmphex(value, ==, UINT64_C(0xfeedface01234567));
    g_assert_true(access_reg(target_router, 1, SN0_FABRIC_VECTOR_READ,
                             RR_LOCAL_TABLE(9), 0, &value));
    g_assert_cmphex(value, ==, UINT64_C(0x123456));
    g_assert_true(access_reg(target_router, 1, SN0_FABRIC_VECTOR_READ,
                             RR_STATUS_ERROR(4), 0, &value));
    g_assert_cmphex(value, ==, UINT64_C(0x200000001));
    g_assert_true(access_reg(target_router, 2, SN0_FABRIC_VECTOR_READ,
                             RR_STATUS_REV_ID, 0, &value));
    g_assert_cmphex(value & (STATUS_PORT_WORKING(4) |
                            STATUS_PORT_EIGHT_BIT(4)), ==,
                    STATUS_PORT_WORKING(4) | STATUS_PORT_EIGHT_BIT(4));

    g_assert_cmpuint(source_observer.calls, ==, 0);
    g_assert_cmpuint(target_observer.calls, ==, 1);
    /* The first migrated field is status_control, a big-endian uint64. */
    g_assert_cmpint(pwrite(fd, &invalid_status, 1, 0), ==, 1);
    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, vmsd, target_router,
                                      vmsd->version_id, &err),
                    ==, -EINVAL);
    error_free(err);
    qemu_fclose(file);
    g_assert_cmpuint(target_observer.calls, ==, 1);
    sgi_router_set_changed(f->router, NULL, NULL);
    sgi_router_set_changed(target_router, NULL, NULL);
    close(fd);
    object_unparent(OBJECT(target));
    object_unref(OBJECT(target));
}

static DeviceState *new_nic_router(uint64_t clock, uint64_t serial,
                                    bool present)
{
    DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);

    qdev_prop_set_uint64(dev, "nic-clock-hz", clock);
    qdev_prop_set_uint64(dev, "nic-serial", serial);
    qdev_prop_set_bit(dev, "nic-present", present);
    qdev_realize(dev, NULL, &error_abort);
    device_cold_reset(dev);
    return dev;
}

static void test_nic_calibration(void)
{
    const uint64_t clocks[] = { 97500000, 100000000, 99000000 };
    const unsigned int phases[] = { 47, 49, 48 };

    for (unsigned int i = 0; i < G_N_ELEMENTS(clocks); i++) {
        DeviceState *dev = new_nic_router(clocks[i], 0, true);
        Fixture f = { dev, SGI_ROUTER(dev) };
        uint64_t before = read_reg(&f, 1, RR_NIC_ULAN);

        for (unsigned int j = 0; j < G_N_ELEMENTS(phases); j++) {
            uint64_t request = ((uint64_t)phases[j] << 27) |
                               ((uint64_t)phases[j] << 20) |
                               (UINT64_C(520) << 10) | (65 << 2);
            bool supported = i == j && i < 2;

            g_assert_cmpint(access_reg(f.router, 1, SN0_FABRIC_VECTOR_WRITE,
                                       RR_NIC_ULAN, request, NULL), ==,
                             supported);
            if (supported) {
                qemu_clock_advance_virtual_time(
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 585000);
                g_assert_cmphex(read_reg(&f, 1, RR_NIC_ULAN) & 3, ==, NIC_DONE);
                before = read_reg(&f, 1, RR_NIC_ULAN);
            } else {
                g_assert_cmphex(read_reg(&f, 1, RR_NIC_ULAN), ==, before);
            }
        }
        object_unparent(OBJECT(dev));
        object_unref(OBJECT(dev));
    }
}

static void test_nic_serial_limit(void)
{
    DeviceState *dev = qdev_new(TYPE_SGI_ROUTER);
    Error *err = NULL;

    qdev_prop_set_uint64(dev, "nic-serial", UINT64_C(1) << 48);
    g_assert_false(qdev_realize(dev, NULL, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "48 bits"));
    error_free(err);
    object_unparent(OBJECT(dev));
    object_unref(OBJECT(dev));
}

static void test_nic_delayed_callback(Fixture *f, gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    write_reg(f, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
              (UINT64_C(520) << 10) | (65 << 2));
    /* Advance without dispatching the intermediate release event. */
    virtual_time = start + 1000000;
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==, NIC_DONE);
    g_assert_false(qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL));
    for (unsigned int bit = 0; bit < 8; bit++) {
        bool one = (0x33 >> bit) & 1; /* Read ROM. */
        unsigned int pulse = one ? 6 : 80;
        unsigned int sample = one ? 110 : 30;

        write_reg(f, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
                  ((uint64_t)pulse << 10) | ((uint64_t)sample << 2));
        qemu_clock_advance_virtual_time(virtual_time + (pulse + sample) * 1000);
    }
    write_reg(f, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
              (UINT64_C(6) << 10) | (13 << 2));
    virtual_time += 1000000;
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
    g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==, 3);
    g_assert_false(qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL));
    /* Family 0x0b: the next two bits are one then zero, not skipped bits. */
    for (unsigned int bit = 1; bit <= 2; bit++) {
        write_reg(f, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
                  (UINT64_C(6) << 10) | (13 << 2));
        qemu_clock_advance_virtual_time(virtual_time + 19000);
        g_assert_cmphex(read_reg(f, 1, RR_NIC_ULAN) & 3, ==,
                        bit == 1 ? 3 : 2);
    }
}

static int load_nic_state(Fixture *source, Fixture *target)
{
    g_autofree char *path = NULL;
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(source->dev)->vmsd;
    int fd = g_file_open_tmp("router-nic-state-XXXXXX", &path, NULL);
    QEMUFile *file;
    Error *err = NULL;
    int result;

    g_assert_cmpint(fd, >=, 0);
    unlink(path);
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, vmsd, source->router,
                                      NULL, &error_abort), ==, 0);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    file = state_file(fd, false);
    result = vmstate_load_state(file, vmsd, target->router,
                                vmsd->version_id, &err);
    error_free(err);
    qemu_fclose(file);
    close(fd);
    return result;
}

static void test_histogram_migration(Fixture *f, gconstpointer unused)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    DeviceState *dev;
    Fixture target;
    uint64_t port1;
    uint64_t port6;

    select_histogram(f, 1, 1, 0);
    select_histogram(f, 6, 1, 0);
    for (unsigned int i = 0; i < 5; i++) {
        sgi_router_histogram_receive(f->router, 1, true, i < 3, true);
    }
    for (unsigned int i = 0; i < 7; i++) {
        sgi_router_histogram_send(f->router, 1, true);
    }
    sgi_router_histogram_receive(f->router, 6, true, true, true);
    sgi_router_histogram_send(f->router, 6, true);
    qemu_clock_advance_virtual_time(start + 50);
    port1 = read_reg(f, 1, RR_HISTOGRAM(1));
    port6 = read_reg(f, 1, RR_HISTOGRAM(6));
    g_assert_cmphex(port1, ==, UINT64_C(0x0002000700050003));
    g_assert_cmphex(port6, ==, UINT64_C(0x0002000100010001));
    select_histogram(f, 6, 2, 3);
    write_reg(f, 1, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);

    dev = new_router(0xf);
    target = (Fixture) { dev, SGI_ROUTER(dev) };
    g_usleep(1000);
    g_assert_cmpint(load_nic_state(f, &target), ==, 0);
    g_assert_cmphex(read_reg(&target, 1, RR_HISTOGRAM(1)), ==, port1);
    g_assert_cmphex(read_reg(&target, 1, RR_HISTOGRAM(6)), ==, port6);
    g_assert_cmphex(read_reg(&target, 1, RR_PORT_PARMS(1)) &
                    PORT_HISTOGRAM_FIELDS, ==, PORT_HISTOGRAM_MODE(1));
    g_assert_cmphex(read_reg(&target, 1, RR_PORT_PARMS(6)) &
                    PORT_HISTOGRAM_FIELDS, ==,
                    PORT_HISTOGRAM_MODE(2) | PORT_HISTOGRAM_VC(3));
    g_assert_cmphex(read_reg(&target, 1, RR_DIAG_PARMS) &
                    DIAG_ABSOLUTE_HISTOGRAM, ==,
                    DIAG_ABSOLUTE_HISTOGRAM);

    write_reg(&target, 1, RR_DIAG_PARMS, 0);
    qemu_clock_advance_virtual_time(start + 59);
    g_assert_cmphex(read_reg(&target, 1, RR_HISTOGRAM(1)), ==, port1);
    qemu_clock_advance_virtual_time(start + 60);
    g_assert_cmpuint(
        histogram_bucket(read_reg(&target, 1, RR_HISTOGRAM(1)), 3), ==, 3);

    object_unparent(OBJECT(dev));
    object_unref(OBJECT(dev));
}

static void test_nic_migration_config(Fixture *f, gconstpointer unused)
{
    const uint64_t clocks[] = { 97500000, 100000000, 100000000 };
    const uint64_t serials[] = { 0, 1, 0 };
    const bool present[] = { true, true, false };

    for (unsigned int i = 0; i < G_N_ELEMENTS(clocks); i++) {
        DeviceState *dev = new_nic_router(clocks[i], serials[i], present[i]);
        Fixture target = { dev, SGI_ROUTER(dev) };

        g_assert_cmpint(load_nic_state(f, &target), ==, -EINVAL);
        object_unparent(OBJECT(dev));
        object_unref(OBJECT(dev));
    }
}

static void corrupt_nic_field(Fixture *f, const char *name, uint64_t value)
{
    const VMStateDescription *vmsd = DEVICE_GET_CLASS(f->dev)->vmsd;

    /* Serialize malicious values through the actual migration field layout. */
    for (const VMStateField *field = vmsd->fields; field->name; field++) {
        void *address = (char *)f->router + field->offset;

        if (strcmp(field->name, name)) {
            continue;
        }
        switch (field->size) {
        case 1:
            *(uint8_t *)address = value;
            return;
        case 2:
            *(uint16_t *)address = value;
            return;
        case 8:
            *(uint64_t *)address = value;
            return;
        default:
            g_assert_not_reached();
        }
    }
    g_assert_not_reached();
}

static void test_nic_migration_invalid(Fixture *f, gconstpointer unused)
{
    const struct {
        const char *field;
        uint64_t value;
        bool sampled;
    } cases[] = {
        { "nic_phase", 3 },
        { "nic_candidate", 2 },
        { "nic_pulse", 7 },
        { "nic_sample", 14 },
        { "nic_release_ns", UINT64_MAX },
        { "nic_sample_ns", 0 },
        { "nic_ulan", NIC_DIVIDERS_RESET | NIC_DONE },
        { "nic_candidate", 1, true },
    };
    DeviceState *dev = new_nic_router(100000000, 0, true);
    Fixture target = { dev, SGI_ROUTER(dev) };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        device_cold_reset(f->dev);
        device_cold_reset(dev);
        write_reg(f, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
                  (UINT64_C(520) << 10) | (65 << 2));
        if (cases[i].sampled) {
            qemu_clock_advance_virtual_time(virtual_time + 520000);
        }
        write_reg(&target, 1, RR_NIC_ULAN, NIC_DIVIDERS_RESET |
                  (UINT64_C(6) << 10) | (13 << 2));
        corrupt_nic_field(f, cases[i].field, cases[i].value);
        g_assert_cmpint(load_nic_state(f, &target), ==, -EINVAL);
        device_cold_reset(f->dev);
        virtual_time += 1000000;
        /* Failed load cancels the destination's previously pending timer. */
        g_assert_false(qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL));
    }
    object_unparent(OBJECT(dev));
    object_unref(OBJECT(dev));
}

static void test_cold_reset(Fixture *f, gconstpointer unused)
{
    int64_t reset_time;
    unsigned int exit_port;
    uint8_t next_turn;

    write_reg(f, 1, RR_SCRATCH_REG0, UINT64_MAX);
    write_reg(f, 1, RR_PROTECTION_CONFIG, UINT64_MAX);
    write_reg(f, 1, RR_LOCAL_TABLE(4), 0x123456);
    write_reg(f, 1, RR_META_TABLE(7), 0x654321);
    sgi_router_set_port_link(f->router, 3, true, true);
    sgi_router_latch_port_error(f->router, 3, 0x1234);
    select_histogram(f, 1, 1, 3);
    sgi_router_histogram_receive(f->router, 1, true, true, true);
    write_reg(f, 1, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    qemu_clock_advance_virtual_time(
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HISTOGRAM_TICK_NS - 1);
    qemu_devices_reset(RESET_TYPE_COLD);
    reset_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g_assert_cmphex(read_reg(f, 1, RR_SCRATCH_REG0), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_PROTECTION_CONFIG), ==,
                    PROTECTION_RESET);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_ERROR(3)), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_PORT_PARMS(1)), ==,
                    PORT_PARMS_RESET);
    g_assert_cmphex(read_reg(f, 1, RR_DIAG_PARMS) &
                    DIAG_ABSOLUTE_HISTOGRAM, ==, 0);
    g_assert_cmphex(read_reg(f, 1, RR_STATUS_REV_ID) &
                    (STATUS_PORT_WORKING(3) | STATUS_PORT_EIGHT_BIT(3)), ==, 0);
    /* Undefined table contents must not retain programmed-route validity. */
    write_reg(f, 1, RR_PROTECTION_CONFIG, UINT64_C(1) << 12);
    g_assert_false(sgi_router_route(f->router, 1, 2, 4,
                                    &exit_port, &next_turn));
    write_reg(f, 1, RR_PROTECTION_CONFIG, UINT64_C(1) << 11);
    g_assert_false(sgi_router_route(f->router, 1, 2, 0x70,
                                    &exit_port, &next_turn));

    select_histogram(f, 1, 1, 0);
    qemu_clock_advance_virtual_time(reset_time + HISTOGRAM_TICK_NS - 1);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==, 0);
    qemu_clock_advance_virtual_time(reset_time + HISTOGRAM_TICK_NS);
    g_assert_cmphex(read_reg(f, 1, RR_HISTOGRAM(1)), ==,
                    UINT64_C(1) << 48);
}

static void test_init_machine(void)
{
    Object *machine = object_property_add_new_container(object_get_root(),
                                                         "machine");

    object_property_add_new_container(machine, "unattached");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    module_call_init(MODULE_INIT_QOM);
    test_init_machine();

#define ADD_TEST(name, fn) \
    g_test_add("/sgi-router/" name, Fixture, GUINT_TO_POINTER(0xf), \
               fixture_setup, fn, fixture_teardown)
    g_test_add_func("/sgi-router/nic-calibration", test_nic_calibration);
    g_test_add_func("/sgi-router/nic-serial-limit", test_nic_serial_limit);
    ADD_TEST("nic-delayed-callback", test_nic_delayed_callback);
    ADD_TEST("nic-migration-config", test_nic_migration_config);
    ADD_TEST("nic-migration-invalid", test_nic_migration_invalid);
    ADD_TEST("histogram-migration", test_histogram_migration);
    ADD_TEST("nic-slot-deadline", test_nic_slot_deadline);
    ADD_TEST("identity-and-link-state", test_identity_and_link_state);
    ADD_TEST("fabric-access-boundary", test_fabric_access_boundary);
    ADD_TEST("reset-values", test_reset_values);
    ADD_TEST("register-storage-masks", test_register_storage_masks);
    ADD_TEST("conditional-exchange", test_conditional_exchange);
    ADD_TEST("error-clear-addresses", test_error_clear_addresses);
    ADD_TEST("error-counters-saturate", test_error_counters_saturate);
    ADD_TEST("histogram-event-boundaries", test_histogram_event_boundaries);
    ADD_TEST("histogram-clear", test_histogram_clear);
    ADD_TEST("histogram-timebase-and-phase",
             test_histogram_timebase_and_phase);
    ADD_TEST("histogram-equal-time-order", test_histogram_equal_time_order);
    ADD_TEST("histogram-unsupported-modes",
             test_histogram_unsupported_modes);
    ADD_TEST("histogram-common-rescale", test_histogram_common_rescale);
    ADD_TEST("histogram-batched-rescale", test_histogram_batched_rescale);
    ADD_TEST("routing-table-decode", test_routing_table_decode);
    ADD_TEST("valid-route-lookup", test_valid_route_lookup);
    ADD_TEST("next-turn-consumption", test_next_turn_consumption);
    ADD_TEST("port-disable", test_port_disable);
    ADD_TEST("forced-width", test_forced_width);
    ADD_TEST("unsupported-operations-fail", test_unsupported_operations_fail);
    ADD_TEST("migration", test_migration);
    ADD_TEST("change-notifications", test_change_notifications);
    ADD_TEST("cold-reset", test_cold_reset);
#undef ADD_TEST

    return g_test_run();
}
