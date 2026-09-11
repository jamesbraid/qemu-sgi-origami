/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Four real adapters, the real controller and the real management NVRAM on
 * one shared module wire, driven the way firmware drives them.
 */
#include "qemu/osdep.h"
#include "chardev/char.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-o2000-module-mgmt.h"
#include "qapi/error.h"
#include "qemu/aio.h"
#include "qemu/config-file.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "system/reset.h"
#include "system/system.h"

#define DATA_REG        0
#define CTRL_REG        PCF8584_A0_STRIDE

#define CTL_START       (PCF8584_CTL_PIN | PCF8584_CTL_ESO | \
                         PCF8584_CTL_STA | PCF8584_CTL_ACK)
#define CTL_RESTART     (PCF8584_CTL_ESO | PCF8584_CTL_STA | PCF8584_CTL_ACK)
#define CTL_RUN         (PCF8584_CTL_PIN | PCF8584_CTL_ESO)
#define CTL_NAK         PCF8584_CTL_ESO
#define CTL_STOP        (PCF8584_CTL_PIN | PCF8584_CTL_ESO | PCF8584_CTL_STO)

#define SERVICE_PERIOD_NS   1000000
#define GRANT_SETTLE_NS     500000
#define GRANT_BASE          0xe0

#define POLL_STEP_NS        2000
#define POLL_STEPS          400

/* The address every transaction resyncs to on the way out; nobody answers. */
#define INVALID_ADDRESS     0x7f

/* The address firmware derives from a node's own slot. */
#define ADAPTER_OWN_ADDRESS(slot)   (0x09 + (slot))
#define OWN_ADDRESS_PLACEHOLDER     0x7f
#define CLOCK_12MHZ_90KHZ           0x1c

static int64_t virtual_time;
static int64_t virtual_time_step;

int64_t cpus_get_virtual_clock(void)
{
    int64_t now = virtual_time;

    virtual_time += virtual_time_step;
    return now;
}

void cpus_set_virtual_clock(int64_t value)
{
    virtual_time = value;
}

enum { LINE_RESET, LINE_NMI, LINE_POWER };
enum { TEST_PARTIAL = 1, TEST_CONSOLE };

typedef struct Fixture {
    SGIO2000ModuleMgmtState *module;
    Chardev *chr;
    PCF8584State *adapter[SGI_O2000_ADAPTER_COUNT];
    int level[3];
    unsigned raises[3];
} Fixture;

static void line_handler(void *opaque, int line, int level)
{
    Fixture *f = opaque;

    if (level && !f->level[line]) {
        f->raises[line]++;
    }
    f->level[line] = level;
}

static void advance_to(int64_t time)
{
    g_assert_cmpint(qemu_clock_advance_virtual_time(time), ==, time);
}

static void init_adapter(Fixture *f, unsigned slot);

static void setup(Fixture *f, gconstpointer options)
{
    DeviceState *dev = qdev_new(TYPE_SGI_O2000_MODULE_MGMT);
    static const char *const names[] = { "reset", "nmi", "power" };
    unsigned i;

    /*
     * Deliberately no cold reset here. A machine resets after it builds, so
     * driving a freshly realized complex is the case a machine never reaches
     * and therefore the one nothing else covers; test_machine_reset in the
     * sibling file drives the reset path. Adding a reset here would look
     * tidier and would delete that cover.
     */
    virtual_time_step = 0;
    memset(f, 0, sizeof(*f));
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        g_autofree char *capability = g_strdup_printf("can-%s", names[i]);

        qdev_prop_set_bit(dev, capability, true);
        qdev_connect_gpio_out_named(dev, names[i], 0,
                                    qemu_allocate_irq(line_handler, f, i));
    }
    if (options == GINT_TO_POINTER(TEST_CONSOLE)) {
        f->chr = qemu_chr_new("reinitialize-console", "null", NULL);
        g_assert_nonnull(f->chr);
        qdev_prop_set_chr(dev, "chardev", f->chr);
    }
    qdev_realize(dev, NULL, &error_abort);
    f->module = SGI_O2000_MODULE_MGMT(dev);
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        f->adapter[i] =
            PCF8584(sgi_o2000_module_mgmt_get_adapter(f->module, i));
        init_adapter(f, i);
    }
}

static void teardown(Fixture *f, gconstpointer unused)
{
    object_unparent(OBJECT(f->module));
    object_unref(OBJECT(f->module));
    /*
     * Child-bus finalization is deferred to RCU. Complete it before the next
     * fixture advances the global virtual clock and walks its timer list.
     */
    drain_call_rcu();
    if (f->chr) {
        object_unparent(OBJECT(f->chr));
    }
}

static void run_tick(Fixture *f)
{
    advance_to(f->module->service_deadline + GRANT_SETTLE_NS);
}

static uint8_t read_reg(Fixture *f, unsigned a, hwaddr off)
{
    return pcf8584_read_register(f->adapter[a], off, 1);
}

static void write_reg(Fixture *f, unsigned a, hwaddr off, uint8_t value)
{
    pcf8584_write_register(f->adapter[a], off, value, 1);
}

/*
 * The initialisation firmware runs on each adapter, including the redundant
 * second control write and the placeholder read-back that decides whether
 * anything else is attempted on this bus.
 */
static void init_adapter(Fixture *f, unsigned slot)
{
    write_reg(f, slot, CTRL_REG, PCF8584_CTL_PIN);
    write_reg(f, slot, DATA_REG, OWN_ADDRESS_PLACEHOLDER);
    write_reg(f, slot, CTRL_REG, PCF8584_CTL_PIN | PCF8584_CTL_ES1);
    write_reg(f, slot, DATA_REG, CLOCK_12MHZ_90KHZ);
    write_reg(f, slot, CTRL_REG, PCF8584_CTL_PIN);
    g_assert_cmphex(read_reg(f, slot, DATA_REG), ==, OWN_ADDRESS_PLACEHOLDER);
    write_reg(f, slot, CTRL_REG, PCF8584_CTL_PIN);
    write_reg(f, slot, DATA_REG, ADAPTER_OWN_ADDRESS(slot));
    write_reg(f, slot, CTRL_REG, PCF8584_CTL_PIN | PCF8584_CTL_ESO);
    g_assert_cmphex(f->adapter[slot]->own_address, ==,
                    ADAPTER_OWN_ADDRESS(slot));
}

/* The firmware's own wait: poll the status register until the byte is out. */
static bool wait_done(Fixture *f, unsigned a)
{
    unsigned i;

    for (i = 0; i < POLL_STEPS; i++) {
        uint8_t status = read_reg(f, a, CTRL_REG);

        if (!(status & PCF8584_STS_PIN)) {
            return true;
        }
        if (status & (PCF8584_STS_BER | PCF8584_STS_LAB)) {
            return false;
        }
        advance_to(virtual_time + POLL_STEP_NS);
    }
    return false;
}

static bool start_transfer(Fixture *f, unsigned a, uint8_t address, bool recv)
{
    write_reg(f, a, CTRL_REG, CTL_RUN);
    write_reg(f, a, DATA_REG, (address << 1) | recv);
    write_reg(f, a, CTRL_REG, CTL_START);
    if (!wait_done(f, a)) {
        return false;
    }
    return !(read_reg(f, a, CTRL_REG) & PCF8584_STS_LRB);
}

static bool restart_transfer(Fixture *f, unsigned a, uint8_t address,
                             bool recv)
{
    write_reg(f, a, CTRL_REG, CTL_RESTART);
    write_reg(f, a, DATA_REG, (address << 1) | recv);
    if (!wait_done(f, a)) {
        return false;
    }
    return !(read_reg(f, a, CTRL_REG) & PCF8584_STS_LRB);
}

static bool send_byte(Fixture *f, unsigned a, uint8_t value)
{
    write_reg(f, a, DATA_REG, value);
    if (!wait_done(f, a)) {
        return false;
    }
    return !(read_reg(f, a, CTRL_REG) & PCF8584_STS_LRB);
}

/* The first read after a read-direction address returns the stale byte. */
static bool discard_byte(Fixture *f, unsigned a)
{
    (void)read_reg(f, a, DATA_REG);
    return wait_done(f, a);
}

static uint8_t take_byte(Fixture *f, unsigned a, bool last)
{
    uint8_t value;

    if (last) {
        write_reg(f, a, CTRL_REG, CTL_NAK);
    }
    value = read_reg(f, a, DATA_REG);
    g_assert_true(wait_done(f, a));
    return value;
}

static void stop_transfer(Fixture *f, unsigned a)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    unsigned i;

    write_reg(f, a, CTRL_REG, CTL_STOP);
    for (i = 0; i < POLL_STEPS; i++) {
        if (!i2c_bus_busy(bus)) {
            return;
        }
        advance_to(virtual_time + POLL_STEP_NS);
    }
    g_assert_not_reached();
}

/* Clock progress during validation must not abandon a fresh START. */
static void test_start_clock_progress(Fixture *f, gconstpointer unused)
{
    write_reg(f, 0, CTRL_REG, CTL_RUN);
    write_reg(f, 0, DATA_REG, SGI_O2000_NVRAM_FIRST_ADDR << 1);
    virtual_time_step = 1;
    write_reg(f, 0, CTRL_REG, CTL_START);
    virtual_time_step = 0;

    g_assert_true(f->adapter[0]->wire_active);
    g_assert_true(wait_done(f, 0));
    g_assert_cmphex(read_reg(f, 0, CTRL_REG) & PCF8584_STS_LRB, ==, 0);
    stop_transfer(f, 0);
}

/* A progressing virtual clock must not invalidate a slave clock release. */
static void test_addressed_release_clock_progress(Fixture *f,
                                                  gconstpointer unused)
{
    I2CSlave *rx = f->adapter[1]->rx;

    g_assert_true(start_transfer(f, 0, ADAPTER_OWN_ADDRESS(1), false));
    g_assert_true(rx->wire_hold);
    g_assert_cmpuint(f->adapter[1]->wire_hold_release, ==,
                     PCF8584_HOLD_READ);

    virtual_time_step = 2;
    (void)read_reg(f, 1, DATA_REG);
    virtual_time_step = 0;
    g_assert_true(rx->wire_hold_pending);
    advance_to(virtual_time + 1);
    g_assert_false(rx->wire_hold);

    stop_transfer(f, 0);
}

/* Send a framed request and prepare its response for register reads. */
static void msc_begin_response(Fixture *f, unsigned a, const char *text)
{
    uint8_t request[16];
    unsigned i, len = strlen(text) + 2;
    uint8_t sum = 0;

    g_assert_cmpuint(len, <=, sizeof(request));
    request[0] = strlen(text);
    memcpy(&request[1], text, strlen(text));
    for (i = 0; i < strlen(text) + 1; i++) {
        sum += request[i];
    }
    request[strlen(text) + 1] = (uint8_t)-(int)sum;

    g_assert_true(start_transfer(f, a, SGI_O2000_MSC_ADDRESS, false));
    for (i = 0; i < len; i++) {
        g_assert_true(send_byte(f, a, request[i]));
    }
    g_assert_true(restart_transfer(f, a, SGI_O2000_MSC_ADDRESS, true));
    g_assert_true(discard_byte(f, a));
}

/*
 * One framed command and its response, ending the way firmware ends one:
 * every response byte acknowledged, then the closing repeated START to an
 * address nobody answers, then STOP.
 */
static unsigned msc_command(Fixture *f, unsigned a, const char *text,
                            uint8_t *response, unsigned cap)
{
    unsigned i, total;
    uint8_t sum = 0;

    msc_begin_response(f, a, text);
    response[0] = take_byte(f, a, false);
    total = (unsigned)response[0] + 2;
    g_assert_cmpuint(total, <=, cap);
    for (i = 1; i < total; i++) {
        response[i] = take_byte(f, a, false);
    }
    g_assert_false(restart_transfer(f, a, INVALID_ADDRESS, false));
    stop_transfer(f, a);

    sum = 0;
    for (i = 0; i < total; i++) {
        sum += response[i];
    }
    g_assert_cmphex(sum, ==, 0);
    return total;
}

/* One logical NVRAM byte, addressed the way the eleven-bit space is split. */
static uint8_t nvram_read(Fixture *f, unsigned a, uint16_t offset)
{
    uint8_t address = SGI_O2000_NVRAM_FIRST_ADDR + (offset >> 8);
    uint8_t value;

    g_assert_cmpuint(offset, <, SGI_O2000_NVRAM_BYTES);
    g_assert_true(start_transfer(f, a, address, false));
    g_assert_true(send_byte(f, a, offset & 0xff));
    g_assert_true(restart_transfer(f, a, address, true));
    g_assert_true(discard_byte(f, a));
    value = take_byte(f, a, true);
    stop_transfer(f, a);
    return value;
}

static void nvram_write(Fixture *f, unsigned a, uint16_t offset, uint8_t value)
{
    uint8_t address = SGI_O2000_NVRAM_FIRST_ADDR + (offset >> 8);

    g_assert_cmpuint(offset, <, SGI_O2000_NVRAM_BYTES);
    g_assert_true(start_transfer(f, a, address, false));
    g_assert_true(send_byte(f, a, offset & 0xff));
    g_assert_true(send_byte(f, a, value));
    stop_transfer(f, a);
}

/*
 * One shared complex: four adapters, one controller endpoint and one logical
 * NVRAM space, all on the same wire. Package count is not asserted; the
 * guest-visible logical space is what the contract establishes.
 */
static void test_population(Fixture *f, gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    unsigned i;

    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_nonnull(f->adapter[i]);
        g_assert_true(f->adapter[i]->bus == bus);
    }
    g_assert_null(sgi_o2000_module_mgmt_get_adapter(f->module,
                                                    SGI_O2000_ADAPTER_COUNT));
    g_assert_nonnull(f->module->msc);
    g_assert_cmphex(I2C_SLAVE(f->module->msc)->address, ==,
                    SGI_O2000_MSC_ADDRESS);

    /* Every logical address in the space answers from every adapter. */
    for (i = 0; i < SGI_O2000_NVRAM_ADDRESSES; i++) {
        g_assert_true(start_transfer(f, i % SGI_O2000_ADAPTER_COUNT,
                                     SGI_O2000_NVRAM_FIRST_ADDR + i, false));
        stop_transfer(f, i % SGI_O2000_ADAPTER_COUNT);
    }
    /* And an address outside it does not. */
    g_assert_false(start_transfer(f, 0,
                                  SGI_O2000_NVRAM_FIRST_ADDR +
                                  SGI_O2000_NVRAM_ADDRESSES, false));
    stop_transfer(f, 0);
}

/* The reviewed validity marker and the model-derived module number. */
static void test_nvram_defaults(Fixture *f, gconstpointer unused)
{
    uint16_t reserved = SGI_O2000_NVRAM_RESERVED_PAGE *
                        SGI_O2000_NVRAM_PAGE_BYTES;

    g_assert_cmphex(nvram_read(f, 0, reserved +
                               SGI_O2000_NVRAM_VALIDITY_OFFSET), ==,
                    SGI_O2000_NVRAM_VALIDITY_VALUE);
    g_assert_cmphex(nvram_read(f, 1, reserved +
                               SGI_O2000_NVRAM_MODULE_OFFSET), ==,
                    f->module->module_number);
    /* Nothing else in the region is invented. */
    g_assert_cmphex(nvram_read(f, 2, reserved + 1), ==, 0);
    g_assert_cmphex(nvram_read(f, 3, reserved + 15), ==, 0);
    g_assert_cmphex(nvram_read(f, 0, 0), ==, 0);
}

/*
 * The storage is one space shared by every node, not a private copy behind
 * each adapter, and a guest's writes survive a whole-machine reset.
 */
static void test_nvram_shared_and_persistent(Fixture *f, gconstpointer unused)
{
    unsigned page;

    for (page = 0; page < SGI_O2000_NVRAM_ADDRESSES; page++) {
        uint16_t offset = page * SGI_O2000_NVRAM_PAGE_BYTES + 0x23;

        nvram_write(f, page % SGI_O2000_ADAPTER_COUNT, offset, 0x40 + page);
    }
    for (page = 0; page < SGI_O2000_NVRAM_ADDRESSES; page++) {
        uint16_t offset = page * SGI_O2000_NVRAM_PAGE_BYTES + 0x23;
        unsigned other = (page + 2) % SGI_O2000_ADAPTER_COUNT;

        g_assert_cmphex(nvram_read(f, other, offset), ==, 0x40 + page);
    }

    /* The last logical byte is reachable, and is not page zero. */
    nvram_write(f, 0, SGI_O2000_NVRAM_BYTES - 1, 0xa5);
    g_assert_cmphex(nvram_read(f, 3, SGI_O2000_NVRAM_BYTES - 1), ==, 0xa5);
    g_assert_cmphex(nvram_read(f, 3, SGI_O2000_NVRAM_PAGE_BYTES - 1), ==, 0);

    /*
     * Reset returns the adapters to their power-on state, so a node has to
     * initialise its own again before it can read anything back.
     */
    qemu_devices_reset(RESET_TYPE_COLD);
    init_adapter(f, 1);
    g_assert_cmphex(nvram_read(f, 1, SGI_O2000_NVRAM_BYTES - 1), ==, 0xa5);
    g_assert_cmphex(nvram_read(f, 1, 0x23), ==, 0x40);
    /* The reviewed defaults are still there too. */
    g_assert_cmphex(nvram_read(f, 1, SGI_O2000_NVRAM_RESERVED_PAGE *
                               SGI_O2000_NVRAM_PAGE_BYTES), ==,
                    SGI_O2000_NVRAM_VALIDITY_VALUE);
}

/*
 * A guest may hold the bus across a transaction for as long as it likes: the
 * ticks it spans defer without advancing the processor code, and the first
 * eligible tick after STOP publishes the next one.
 */
static void test_busy_defers_without_advancing(Fixture *f,
                                               gconstpointer unused)
{
    run_tick(f);
    g_assert_cmphex(f->adapter[0]->data, ==, GRANT_BASE);

    g_assert_true(start_transfer(f, 1, SGI_O2000_NVRAM_FIRST_ADDR, false));
    g_assert_true(send_byte(f, 1, 0x00));
    /* Sit on the bus across several service ticks. */
    advance_to(virtual_time + 4 * SERVICE_PERIOD_NS);
    /*
     * No grant was published, so the processor code has not moved. The
     * peer's own address has replaced the grant in the observing adapter's
     * buffer, which is what a later completed address phase is meant to do.
     */
    g_assert_cmpuint(f->module->processor_code, ==, 0);
    g_assert_cmphex(f->adapter[0]->data, ==,
                    SGI_O2000_NVRAM_FIRST_ADDR << 1);

    stop_transfer(f, 1);
    run_tick(f);
    g_assert_cmpuint(f->module->processor_code, ==, 1);
    g_assert_cmphex(f->adapter[0]->data, ==, GRANT_BASE + 1);
}

static int64_t request_start_during_grant(Fixture *f)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    int64_t next_grant;

    run_tick(f);
    g_assert_cmphex(read_reg(f, 0, CTRL_REG) & PCF8584_STS_BB,
                    ==, PCF8584_STS_BB);

    next_grant = f->module->service_deadline;
    advance_to(next_grant + 6 * POLL_STEP_NS);
    g_assert_true(f->module->grant_active);
    g_assert_true(i2c_bus_busy(bus));
    g_assert_cmphex(f->adapter[0]->status & PCF8584_STS_BB, ==, 0);

    write_reg(f, 0, CTRL_REG, CTL_RUN);
    write_reg(f, 0, DATA_REG, SGI_O2000_NVRAM_FIRST_ADDR << 1);
    write_reg(f, 0, CTRL_REG, CTL_START);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_true(f->adapter[0]->wire_start_waiting);
    g_assert_cmphex(i2c_wire_contenders(bus), ==,
                    1U << SGI_O2000_ADAPTER_COUNT);
    return next_grant;
}

/* A START issued under a newer grant loses instead of running out of slot. */
static void test_waiting_start_loses_to_grant(Fixture *f,
                                              gconstpointer unused)
{
    int64_t next_grant = request_start_during_grant(f);
    uint8_t status;

    advance_to(next_grant + GRANT_SETTLE_NS);
    status = read_reg(f, 0, CTRL_REG);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_false(f->adapter[0]->wire_start_waiting);
    g_assert_false(f->adapter[0]->start_address_staged);
    g_assert_cmphex(status & PCF8584_STS_LAB, ==, PCF8584_STS_LAB);
    g_assert_cmphex(status & PCF8584_STS_PIN, ==, 0);
}

/* A START overtaken during tBUF loses at the completing grant's STOP. */
static void test_start_overtaken_during_bus_free_time(Fixture *f,
                                                      gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    int64_t next_grant;
    uint8_t status;

    run_tick(f);
    g_assert_true(start_transfer(f, 1, SGI_O2000_NVRAM_FIRST_ADDR, false));
    while (f->module->service_deadline <= virtual_time + 5 * POLL_STEP_NS) {
        advance_to(virtual_time + POLL_STEP_NS);
    }
    next_grant = f->module->service_deadline;
    advance_to(next_grant - 5 * POLL_STEP_NS);
    stop_transfer(f, 1);
    g_assert_false(i2c_bus_busy(bus));
    g_assert_cmpint(virtual_time, <, next_grant);

    write_reg(f, 0, DATA_REG, SGI_O2000_NVRAM_FIRST_ADDR << 1);
    write_reg(f, 0, CTRL_REG, CTL_START);
    g_assert_true(f->adapter[0]->wire_active);

    advance_to(next_grant + POLL_STEP_NS);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_true(f->adapter[0]->wire_start_waiting);

    advance_to(next_grant + GRANT_SETTLE_NS);
    status = read_reg(f, 0, CTRL_REG);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_false(f->adapter[0]->wire_start_waiting);
    g_assert_false(f->adapter[0]->start_address_staged);
    g_assert_cmphex(status & PCF8584_STS_LAB, ==, PCF8584_STS_LAB);
    g_assert_cmphex(status & PCF8584_STS_PIN, ==, 0);
}

/* Two STARTs delayed by one grant must take the wire one at a time. */
static void test_waiting_starts_serialize(Fixture *f, gconstpointer unused)
{
    int64_t start_at;

    g_assert_true(start_transfer(f, 2, SGI_O2000_NVRAM_FIRST_ADDR, false));

    for (unsigned a = 0; a < 2; a++) {
        write_reg(f, a, CTRL_REG, CTL_RUN);
        write_reg(f, a, DATA_REG, SGI_O2000_MSC_ADDRESS << 1);
        write_reg(f, a, CTRL_REG, CTL_START);
        g_assert_true(f->adapter[a]->wire_start_waiting);
    }

    stop_transfer(f, 2);
    start_at = f->adapter[0]->wire_free_at;
    advance_to(start_at + POLL_STEP_NS);
    g_assert_true(f->adapter[0]->wire_active);
    g_assert_false(f->adapter[1]->wire_active);
    g_assert_true(f->adapter[1]->wire_start_waiting);
    g_assert_true(wait_done(f, 0));
    stop_transfer(f, 0);

    g_assert_true(wait_done(f, 1));
    g_assert_cmphex(read_reg(f, 1, CTRL_REG) & PCF8584_STS_LAB, ==, 0);
    stop_transfer(f, 1);
}

static void test_waiting_start_cancellation(Fixture *f,
                                            gconstpointer disable)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    int64_t next_grant = request_start_during_grant(f);

    write_reg(f, 0, CTRL_REG, disable ? PCF8584_CTL_PIN : CTL_STOP);
    g_assert_false(f->adapter[0]->wire_start_waiting);
    advance_to(next_grant + GRANT_SETTLE_NS);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_false(i2c_bus_busy(bus));
}

static void test_waiting_start_reset(Fixture *f, gconstpointer unused)
{
    request_start_during_grant(f);
    qemu_devices_reset(RESET_TYPE_COLD);
    g_assert_false(f->adapter[0]->wire_start_waiting);
    g_assert_false(f->adapter[0]->wire_active);
    g_assert_cmphex(f->adapter[0]->status, ==,
                    PCF8584_STS_PIN | PCF8584_STS_BB);
}

static void test_waiting_start_sets_pin(Fixture *f, gconstpointer unused)
{
    request_start_during_grant(f);
    write_reg(f, 0, CTRL_REG, CTL_STOP);

    f->adapter[0]->status &= ~PCF8584_STS_PIN;
    write_reg(f, 0, DATA_REG, SGI_O2000_NVRAM_FIRST_ADDR << 1);
    write_reg(f, 0, CTRL_REG,
              PCF8584_CTL_ESO | PCF8584_CTL_STA | PCF8584_CTL_ACK);
    g_assert_true(f->adapter[0]->wire_start_waiting);
    g_assert_cmphex(f->adapter[0]->status & PCF8584_STS_PIN, ==,
                    PCF8584_STS_PIN);
}

/*
 * Every adapter sees the completed grant address in its own data register
 * without being selected by it. That retention is a software-required
 * approximation, not measured PCF8584 behaviour.
 */
static void test_grant_reaches_every_adapter(Fixture *f, gconstpointer unused)
{
    unsigned i, code;

    for (code = 0; code < 3; code++) {
        run_tick(f);
        for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
            g_assert_cmphex(f->adapter[i]->data, ==, GRANT_BASE + code);
            g_assert_cmphex(f->adapter[i]->status & PCF8584_STS_AAS, ==, 0);
            g_assert_cmphex(f->adapter[i]->status & PCF8584_STS_BB, ==,
                            PCF8584_STS_BB);
        }
    }
}

/* A grant becomes visible only once its terminal STOP releases the bus. */
static void test_grant_published_after_stop(Fixture *f,
                                            gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    uint8_t old_data[SGI_O2000_ADAPTER_COUNT];
    unsigned i, slot;

    for (slot = 0; slot < SGI_O2000_ADAPTER_COUNT; slot++) {
        old_data[slot] = f->adapter[slot]->data;
    }

    advance_to(f->module->service_deadline + 1);
    g_assert_true(i2c_bus_busy(bus));
    for (i = 0; i < POLL_STEPS && i2c_bus_busy(bus); i++) {
        for (slot = 0; slot < SGI_O2000_ADAPTER_COUNT; slot++) {
            g_assert_cmphex(f->adapter[slot]->data, ==, old_data[slot]);
        }
        advance_to(virtual_time + POLL_STEP_NS);
    }
    g_assert_false(i2c_bus_busy(bus));
    for (slot = 0; slot < SGI_O2000_ADAPTER_COUNT; slot++) {
        g_assert_cmphex(f->adapter[slot]->data, ==, GRANT_BASE);
    }
}

/* Two adapters starting together: one wins, the other reports the loss. */
static void test_arbitration_between_adapters(Fixture *f, gconstpointer unused)
{
    write_reg(f, 0, CTRL_REG, CTL_RUN);
    write_reg(f, 1, CTRL_REG, CTL_RUN);
    write_reg(f, 0, DATA_REG, SGI_O2000_MSC_ADDRESS << 1);
    write_reg(f, 1, DATA_REG, SGI_O2000_NVRAM_FIRST_ADDR << 1);
    write_reg(f, 0, CTRL_REG, CTL_START);
    write_reg(f, 1, CTRL_REG, CTL_START);

    g_assert_true(wait_done(f, 0));
    g_assert_cmphex(read_reg(f, 0, CTRL_REG) & PCF8584_STS_LAB, ==, 0);
    g_assert_cmphex(read_reg(f, 0, CTRL_REG) & PCF8584_STS_LRB, ==, 0);
    g_assert_cmphex(read_reg(f, 1, CTRL_REG) & PCF8584_STS_LAB, ==,
                    PCF8584_STS_LAB);
    stop_transfer(f, 0);
}

/* A framed controller command answers over the shared wire. */
static void test_msc_version_exchange(Fixture *f, gconstpointer unused)
{
    static const char text[] = "ver";
    uint8_t request[8], response[96];
    unsigned i, len, total;
    uint8_t sum = 0;

    request[0] = strlen(text);
    memcpy(&request[1], text, strlen(text));
    for (i = 0; i < strlen(text) + 1; i++) {
        sum += request[i];
    }
    request[strlen(text) + 1] = (uint8_t)-(int)sum;
    len = strlen(text) + 2;

    g_assert_true(start_transfer(f, 2, SGI_O2000_MSC_ADDRESS, false));
    for (i = 0; i < len; i++) {
        g_assert_true(send_byte(f, 2, request[i]));
    }
    g_assert_true(restart_transfer(f, 2, SGI_O2000_MSC_ADDRESS, true));
    g_assert_true(discard_byte(f, 2));
    response[0] = take_byte(f, 2, false);
    total = (unsigned)response[0] + 2;
    g_assert_cmpuint(total, <=, sizeof(response));
    for (i = 1; i < total; i++) {
        response[i] = take_byte(f, 2, i == total - 1);
    }
    stop_transfer(f, 2);

    sum = 0;
    for (i = 0; i < total; i++) {
        sum += response[i];
    }
    g_assert_cmphex(sum, ==, 0);
    g_assert_cmpuint(response[0], >=, 2);
    g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
}

/*
 * For a one-byte read, firmware clears ACK before the discarded read starts
 * the receive pipeline.  The following read consumes that NACKed byte; it
 * must leave the adapter paused so the closing repeated START can follow.
 */
static void test_nacked_receive_allows_repeated_start(
    Fixture *f, gconstpointer unused)
{
    uint16_t offset = SGI_O2000_NVRAM_RESERVED_PAGE *
                      SGI_O2000_NVRAM_PAGE_BYTES +
                      SGI_O2000_NVRAM_VALIDITY_OFFSET;
    uint8_t address = SGI_O2000_NVRAM_FIRST_ADDR + (offset >> 8);
    uint8_t value;

    g_assert_true(start_transfer(f, 0, address, false));
    g_assert_true(send_byte(f, 0, offset & 0xff));
    g_assert_true(restart_transfer(f, 0, address, true));

    write_reg(f, 0, CTRL_REG, CTL_NAK);
    (void)read_reg(f, 0, DATA_REG);
    g_assert_true(wait_done(f, 0));
    value = read_reg(f, 0, DATA_REG);
    g_assert_cmphex(value, ==, SGI_O2000_NVRAM_VALIDITY_VALUE);
    g_assert_true(f->adapter[0]->wire_active);
    g_assert_cmpuint(f->adapter[0]->wire_phase, ==, PCF8584_WIRE_PAUSED);

    write_reg(f, 0, CTRL_REG, CTL_RESTART);
    write_reg(f, 0, DATA_REG, INVALID_ADDRESS << 1);
    g_assert_true(wait_done(f, 0));
    g_assert_cmphex(read_reg(f, 0, CTRL_REG) & PCF8584_STS_LRB, !=, 0);
    g_assert_cmpuint(f->adapter[0]->wire_phase, ==, PCF8584_WIRE_PAUSED);
    stop_transfer(f, 0);
}

/*
 * A destructive command carries out its action, and is refused rather than
 * acknowledged when the controller cannot carry it out.
 */
static void test_destructive_command(Fixture *f, gconstpointer unused)
{
    static const char *const commands[] = { "pas none", "rst" };
    uint8_t request[16], response[96];
    unsigned c, i, len, total;

    for (c = 0; c < ARRAY_SIZE(commands); c++) {
        const char *text = commands[c];
        uint8_t sum = 0;

        request[0] = strlen(text);
        memcpy(&request[1], text, strlen(text));
        for (i = 0; i < strlen(text) + 1; i++) {
            sum += request[i];
        }
        request[strlen(text) + 1] = (uint8_t)-(int)sum;
        len = strlen(text) + 2;

        g_assert_true(start_transfer(f, 0, SGI_O2000_MSC_ADDRESS, false));
        for (i = 0; i < len; i++) {
            g_assert_true(send_byte(f, 0, request[i]));
        }
        g_assert_true(restart_transfer(f, 0, SGI_O2000_MSC_ADDRESS, true));
        g_assert_true(discard_byte(f, 0));
        response[0] = take_byte(f, 0, false);
        total = (unsigned)response[0] + 2;
        for (i = 1; i < total; i++) {
            response[i] = take_byte(f, 0, i == total - 1);
        }
        stop_transfer(f, 0);
        g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
    }

    /*
     * The action is deferred until the guest has read the whole "ok", so it
     * runs from a bottom half rather than inside the transaction.
     */
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);
    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);
    g_assert_cmpint(f->level[LINE_RESET], ==, 1);
    advance_to(f->module->pulse_deadline);
    g_assert_cmpint(f->level[LINE_RESET], ==, 0);
}

/*
 * Firmware ends a transaction with a repeated START to an address nobody
 * answers, and only then a STOP. That drops the controller from the
 * selection, so it is the last event it sees: an action committed before it
 * has to fire there, and the controller has to report itself idle there, or
 * the scheduler waits on a controller that is never coming back.
 */
static void test_closing_resync_is_terminal(Fixture *f, gconstpointer unused)
{
    static const char *const commands[] = { "pas none", "rst" };
    uint8_t request[16], response[96];
    unsigned c, i, len, total;

    run_tick(f);
    g_assert_cmphex(f->adapter[0]->data, ==, GRANT_BASE);

    for (c = 0; c < ARRAY_SIZE(commands); c++) {
        const char *text = commands[c];
        uint8_t sum = 0;

        request[0] = strlen(text);
        memcpy(&request[1], text, strlen(text));
        for (i = 0; i < strlen(text) + 1; i++) {
            sum += request[i];
        }
        request[strlen(text) + 1] = (uint8_t)-(int)sum;
        len = strlen(text) + 2;

        g_assert_true(start_transfer(f, 0, SGI_O2000_MSC_ADDRESS, false));
        for (i = 0; i < len; i++) {
            g_assert_true(send_byte(f, 0, request[i]));
        }
        g_assert_true(restart_transfer(f, 0, SGI_O2000_MSC_ADDRESS, true));
        g_assert_true(discard_byte(f, 0));
        response[0] = take_byte(f, 0, false);
        total = (unsigned)response[0] + 2;
        /* Every byte acknowledged: no closing NAK to fall back on. */
        for (i = 1; i < total; i++) {
            response[i] = take_byte(f, 0, false);
        }
        g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
        /* The closing resync, which no endpoint answers. */
        g_assert_false(restart_transfer(f, 0, INVALID_ADDRESS, false));
        stop_transfer(f, 0);
    }

    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);

    /* And the scheduler is not left waiting on a controller that went away. */
    g_assert_false(f->module->slave_active);
    advance_to(f->module->pulse_deadline);
    /*
     * The controller reinitialises itself and nothing else. A node's adapter
     * is reached by the reset line the machine wires, which no unit test
     * has, so here it keeps the address its node gave it.
     */
    g_assert_cmphex(f->adapter[0]->own_address, ==, ADAPTER_OWN_ADDRESS(0));
    run_tick(f);
    g_assert_cmphex(f->adapter[0]->data, ==, GRANT_BASE);
}

/*
 * The whole point of the refusal: a guest that asks for a reset the
 * controller cannot carry out is told so, rather than handed an "ok" it
 * would take as proof and stop trying.
 */
static void test_destructive_command_refused(Fixture *f, gconstpointer unused)
{
    uint8_t response[96];
    unsigned total;

    total = msc_command(f, 0, "pas none", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);

    /* A pulse is running, so the controller cannot start another action. */
    g_assert_true(sgi_o2000_module_mgmt_pulse_reset(f->module));
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);

    total = msc_command(f, 0, "rst", response, sizeof(response));
    g_assert_cmpuint(total, >=, 5);
    g_assert_cmpint(memcmp(&response[1], "err", 3), ==, 0);
    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
    /* Refused, so no second pulse and no fabricated action. */
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);

    /* "pwr d" gets the same answer, not a different one. */
    total = msc_command(f, 0, "pwr d 0", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "err", 3), ==, 0);
    g_assert_cmpint(f->level[LINE_POWER], ==, 1);

    /*
     * Once the pulse ends the same command is accepted again, without the
     * guest re-opening the supervisor gate: reinitialisation takes the
     * protocol queues and leaves what is not a queue, and the gate is not
     * one.
     */
    advance_to(f->module->pulse_deadline);
    total = msc_command(f, 0, "rst", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 2);
}

/* Drain only when the test explicitly crosses the deferred-action boundary. */
static void drain_actions(void)
{
    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
}

static void expect_response(Fixture *f, const char *command, const char *text)
{
    uint8_t response[96];

    msc_command(f, 0, command, response, sizeof(response));
    g_assert_cmpuint(response[0], ==, strlen(text));
    g_assert_cmpmem(&response[1], response[0], text, strlen(text));
}

/* Reinitialization must not cancel an acknowledged action waiting for AIO. */
static void test_reinitialize_committed(Fixture *f, gconstpointer unused)
{
    SGIElscState *s = f->module->msc;

    expect_response(f, "pas none", "ok");
    expect_response(f, "rst", "ok");
    g_assert_true(s->deferred_committed);
    g_assert_true(s->deferred_scheduled);
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);

    sgi_elsc_reinitialize(s);
    /* More terminal events before polling must not duplicate the action. */
    expect_response(f, "ver", "ok origami");
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);
    g_assert_true(f->module->pulse_active);
    advance_to(f->module->pulse_deadline);
    expect_response(f, "ver", "ok origami");
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);
    g_assert_false(f->module->pulse_active);
}

static void test_reinitialize_uncommitted(Fixture *f, gconstpointer partial)
{
    SGIElscState *s = f->module->msc;

    expect_response(f, "pas none", "ok");
    msc_begin_response(f, 0, "rst");
    if (partial) {
        /* Stop before the wire prefetch reaches the acknowledgement end. */
        g_assert_cmpuint(take_byte(f, 0, false), ==, 2);
    }
    g_assert_cmpuint(s->deferred_action, ==, SGI_ELSC_ACTION_RESET);
    g_assert_false(s->deferred_committed);
    g_assert_cmpuint(s->txpos, <, s->txlen);

    sgi_elsc_reinitialize(s);
    /* Clock the discarded response and its terminal event before a request. */
    take_byte(f, 0, false);
    take_byte(f, 0, true);
    stop_transfer(f, 0);
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);
    expect_response(f, "ver", "ok origami");
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);
    g_assert_cmpuint(s->deferred_action, ==, SGI_ELSC_ACTION_NONE);
    /* The retained supervisor gate still permits a subsequent real action. */
    expect_response(f, "rst", "ok");
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);
    advance_to(f->module->pulse_deadline);
}

static void test_reinitialize_queues(Fixture *f, gconstpointer unused)
{
    SGIElscState *s = f->module->msc;
    static const uint8_t input[] = { 's', 't', 'a', 'l', 'e', 0x14, 'x' };
    static const uint8_t fresh[] = { 'N' };

    expect_response(f, "pas none", "ok");
    expect_response(f, "dsp RETAINED", "ok");
    expect_response(f, "tas A", "ok 0");
    qemu_chr_be_write(f->chr, input, sizeof(input));
    g_assert_cmpuint(s->rx_queue_len, ==, 5);
    g_assert_true(s->in_escape);
    g_assert_true(f->module->console_input_pending);

    msc_begin_response(f, 0, "ver");
    g_assert_cmpuint(s->txlen, >, s->txpos);
    g_assert_cmpint(s->resp_owner, >=, 0);
    take_byte(f, 0, true);
    stop_transfer(f, 0);
    g_assert_true(start_transfer(f, 0, SGI_O2000_MSC_ADDRESS, false));
    g_assert_true(send_byte(f, 0, 3));
    g_assert_true(send_byte(f, 0, 'v'));
    g_assert_cmpuint(s->rxlen, ==, 2);
    g_assert_true(f->module->slave_active);

    sgi_elsc_reinitialize(s);
    g_assert_cmpuint(s->rxlen, ==, 0);
    g_assert_cmpuint(s->txlen, ==, 0);
    g_assert_cmpuint(s->txpos, ==, 0);
    g_assert_cmpint(s->resp_owner, ==, -1);
    g_assert_false(f->module->slave_active);
    g_assert_false(f->module->console_input_pending);
    stop_transfer(f, 0);

    /* Input after recovery escapes neither into the old line nor old queue. */
    qemu_chr_be_write(f->chr, fresh, sizeof(fresh));
    expect_response(f, "get", "acp 0 N");
    expect_response(f, "tas B", "ok A");
    g_assert_cmpmem(s->display, sizeof(s->display), "RETAINED", 8);
    expect_response(f, "dsc 0 X", "ok");
    g_assert_cmpmem(s->display, sizeof(s->display), "XETAINED", 8);
    expect_response(f, "rst", "ok");
    drain_actions();
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);
    advance_to(f->module->pulse_deadline);
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    module_call_init(MODULE_INIT_QOM);
    qemu_add_opts(&qemu_chardev_opts);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    qemu_register_resettable(OBJECT(sysbus_get_default()));

    g_test_add("/sgi-o2000-module-management/shared/start-clock-progress",
               Fixture, NULL, setup, test_start_clock_progress, teardown);
    g_test_add("/sgi-o2000-module-management/shared/addressed-release-progress",
               Fixture, NULL, setup, test_addressed_release_clock_progress,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/population", Fixture,
               NULL, setup, test_population, teardown);
    g_test_add("/sgi-o2000-module-management/nvram/defaults", Fixture, NULL,
               setup, test_nvram_defaults, teardown);
    g_test_add("/sgi-o2000-module-management/nvram/shared-and-persistent",
               Fixture, NULL, setup, test_nvram_shared_and_persistent,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/busy-defers", Fixture,
               NULL, setup, test_busy_defers_without_advancing, teardown);
    g_test_add("/sgi-o2000-module-management/shared/stale-start-loses-to-grant",
               Fixture, NULL, setup, test_waiting_start_loses_to_grant,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/start-overtaken-during-tbuf",
               Fixture, NULL, setup,
               test_start_overtaken_during_bus_free_time, teardown);
    g_test_add("/sgi-o2000-module-management/shared/waiting-starts-serialize",
               Fixture, NULL, setup, test_waiting_starts_serialize, teardown);
    g_test_add("/sgi-o2000-module-management/shared/waiting-start-stop",
               Fixture, NULL, setup, test_waiting_start_cancellation,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/waiting-start-disable",
               Fixture, GINT_TO_POINTER(1), setup,
               test_waiting_start_cancellation, teardown);
    g_test_add("/sgi-o2000-module-management/shared/waiting-start-reset",
               Fixture, NULL, setup, test_waiting_start_reset, teardown);
    g_test_add("/sgi-o2000-module-management/shared/waiting-start-pin",
               Fixture, NULL, setup, test_waiting_start_sets_pin, teardown);
    g_test_add("/sgi-o2000-module-management/shared/grant-observed", Fixture,
               NULL, setup, test_grant_reaches_every_adapter, teardown);
    g_test_add("/sgi-o2000-module-management/shared/grant-after-stop", Fixture,
               NULL, setup, test_grant_published_after_stop, teardown);
    g_test_add("/sgi-o2000-module-management/shared/arbitration", Fixture,
               NULL, setup, test_arbitration_between_adapters, teardown);
    g_test_add("/sgi-o2000-module-management/shared/msc-version", Fixture,
               NULL, setup, test_msc_version_exchange, teardown);
    g_test_add("/sgi-o2000-module-management/shared/nacked-receive-restart",
               Fixture, NULL, setup,
               test_nacked_receive_allows_repeated_start, teardown);
    g_test_add("/sgi-o2000-module-management/shared/destructive-command",
               Fixture, NULL, setup, test_destructive_command, teardown);
    g_test_add("/sgi-o2000-module-management/shared/destructive-refused",
               Fixture, NULL, setup, test_destructive_command_refused,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/closing-resync",
               Fixture, NULL, setup, test_closing_resync_is_terminal,
               teardown);
    g_test_add("/sgi-o2000-module-management/reinitialize/committed", Fixture,
               NULL, setup, test_reinitialize_committed, teardown);
    g_test_add("/sgi-o2000-module-management/reinitialize/unread", Fixture,
               NULL, setup, test_reinitialize_uncommitted, teardown);
    g_test_add("/sgi-o2000-module-management/reinitialize/partial", Fixture,
               GINT_TO_POINTER(TEST_PARTIAL), setup,
               test_reinitialize_uncommitted, teardown);
    g_test_add("/sgi-o2000-module-management/reinitialize/queues", Fixture,
               GINT_TO_POINTER(TEST_CONSOLE), setup, test_reinitialize_queues,
               teardown);
    return g_test_run();
}
