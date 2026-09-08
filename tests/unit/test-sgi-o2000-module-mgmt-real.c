/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Four real adapters, the real controller and the real management NVRAM on
 * one shared module wire, driven the way firmware drives them.
 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/misc/sgi-o2000-module-mgmt.h"
#include "qapi/error.h"
#include "qemu/aio.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "system/reset.h"

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

int64_t cpus_get_virtual_clock(void)
{
    return virtual_time;
}

void cpus_set_virtual_clock(int64_t value)
{
    virtual_time = value;
}

enum { LINE_RESET, LINE_NMI, LINE_POWER };

typedef struct Fixture {
    SGIO2000ModuleMgmtState *module;
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

static void setup(Fixture *f, gconstpointer unused)
{
    DeviceState *dev = qdev_new(TYPE_SGI_O2000_MODULE_MGMT);
    static const char *const names[] = { "reset", "nmi", "power" };
    unsigned i;

    memset(f, 0, sizeof(*f));
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        qdev_connect_gpio_out_named(dev, names[i], 0,
                                    qemu_allocate_irq(line_handler, f, i));
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

/*
 * One framed command and its response, ending the way firmware ends one:
 * every response byte acknowledged, then the closing repeated START to an
 * address nobody answers, then STOP.
 */
static unsigned msc_command(Fixture *f, unsigned a, const char *text,
                            uint8_t *response, unsigned cap)
{
    uint8_t request[16];
    unsigned i, len = strlen(text) + 2;
    uint8_t sum = 0;
    unsigned total;

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
     * Once the pulse ends the same command is accepted again. The controller
     * reinitialised its protocol queues on the way out of the pulse, and the
     * supervisor gate is part of those, so it has to be opened again first.
     */
    advance_to(f->module->pulse_deadline);
    total = msc_command(f, 0, "rst", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "err perm", 8), ==, 0);

    total = msc_command(f, 0, "pas none", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
    total = msc_command(f, 0, "rst", response, sizeof(response));
    g_assert_cmpint(memcmp(&response[1], "ok", 2), ==, 0);
    while (aio_poll(qemu_get_aio_context(), false)) {
        continue;
    }
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 2);
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    module_call_init(MODULE_INIT_QOM);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    qemu_register_resettable(OBJECT(sysbus_get_default()));

    g_test_add("/sgi-o2000-module-management/shared/population", Fixture,
               NULL, setup, test_population, teardown);
    g_test_add("/sgi-o2000-module-management/nvram/defaults", Fixture, NULL,
               setup, test_nvram_defaults, teardown);
    g_test_add("/sgi-o2000-module-management/nvram/shared-and-persistent",
               Fixture, NULL, setup, test_nvram_shared_and_persistent,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/busy-defers", Fixture,
               NULL, setup, test_busy_defers_without_advancing, teardown);
    g_test_add("/sgi-o2000-module-management/shared/grant-observed", Fixture,
               NULL, setup, test_grant_reaches_every_adapter, teardown);
    g_test_add("/sgi-o2000-module-management/shared/arbitration", Fixture,
               NULL, setup, test_arbitration_between_adapters, teardown);
    g_test_add("/sgi-o2000-module-management/shared/msc-version", Fixture,
               NULL, setup, test_msc_version_exchange, teardown);
    g_test_add("/sgi-o2000-module-management/shared/destructive-command",
               Fixture, NULL, setup, test_destructive_command, teardown);
    g_test_add("/sgi-o2000-module-management/shared/destructive-refused",
               Fixture, NULL, setup, test_destructive_command_refused,
               teardown);
    g_test_add("/sgi-o2000-module-management/shared/closing-resync",
               Fixture, NULL, setup, test_closing_resync_is_terminal,
               teardown);
    return g_test_run();
}
