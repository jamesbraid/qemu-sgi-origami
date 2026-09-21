/*
 * SGI Origin 2000 BaseIO register-contract tests: the daughtercard IOC3s
 * reached through BaseIO's own Bridge window, apart from the origin2000
 * module's hub/router/topology contract in sn0-origin2000-test.c.
 *
 * Split out of sn0-origin2000-test.c so the shared 4-node module fabric
 * and routing tests are not crowded by IOC3/Timekeeper device detail, and
 * vice versa. start_module()/program_module() are duplicated here rather
 * than shared, matching this project's other qtest files that each carry
 * their own minimal machine-bring-up helpers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci/pci.h"
#include "libqtest.h"

#define NODE_STRIDE UINT64_C(0x100000000)
#define HUB_BASE UINT64_C(0x20001000000)
#define NI_STATUS 0x600000
#define NI_VECTOR_PARMS 0x600200
#define NI_VECTOR 0x600208
#define NI_VECTOR_DATA 0x600210
#define NI_VECTOR_STATUS 0x600300
#define NI_VECTOR_CLEAR 0x600380
#define NI_LOCAL_TABLE 0x638100
#define NI_META_TABLE 0x638000
#define NI_REGION_SIZE (UINT64_C(1) << 17)
#define VECTOR_VALID (UINT64_C(1) << 63)
#define VECTOR_SOURCE_MASK (UINT64_C(0x7ff) << 51)
#define ROUTER_PROTECTION 0x10
#define ROUTER_LOCAL_TABLE 0x70100

#define XTALK_BASE UINT64_C(0x20000000000)
#define XTALK_REG_DATA 4
#define XTALK_WIDGET_SIZE UINT64_C(0x1000000)
#define BASEIO_WIDGET 8

#define BRIDGE_DEVICE(n) (0x0200 + 8 * (n))
#define BRIDGE_CFG0(n) (0x020000 + (n) * 0x1000)
#define BRIDGE_DEV_MEMORY_SPACE (1u << 12)
#define PCI_COMMAND 0x04
#define PCI_BAR0 0x10
#define PCI_SUBSYSTEM 0x2c
#define SUBSYSTEM_SGI 0x10a9u
#define SUBSYSTEM(id) (((uint32_t)(id) << 16) | SUBSYSTEM_SGI)

#define IOC3_DEVICE 2
#define IOC3_MMIO 0x10000000u
#define IOC3_WINDOW 0x600000
#define IOC3_TIMEKEEPER (IOC3_WINDOW + 0x80000)

/*
 * The MediaIO daughtercard's own IOC3, io6g only; device 2's window
 * plus 4 x 1 MB (this file's own BRIDGE_DEVICE(6) window).
 */
#define MIO_IOC3_DEVICE 6
#define MIO_IOC3_MMIO 0x11000000u
#define MIO_IOC3_WINDOW 0xa00000

/* Chip-relative; combine with an IOC3's own window, e.g. IOC3_WINDOW. */
#define KBC_KBD_RECEIVE 0x0a0
#define KBC_AUX_RECEIVE 0x0a4
#define KBC_KBD_TRANSMIT 0x0a8
#define KBC_AUX_TRANSMIT 0x0ac

/*
 * Chip-relative; the combined interrupt source/enable registers
 * sn0-ioc3-test.c's own copy already names, restated here for this
 * file's BaseIO-window access style. INT_KEYBOARD is a latched bit: it
 * survives a drain of the receive register and only clears on a
 * write-one to INT_STATUS itself.
 */
#define INT_STATUS      0x01c
#define INT_ENABLE_SET  0x020
#define INT_ENABLE_CLR  0x024
#define INT_KEYBOARD    (1u << 22)

/*
 * Chip-relative; each IOC3's two 16550s (hw/char/serial.c), at the same
 * offsets sn0-ioc3-test.c's own SERIAL_A/SERIAL_B use. Only the registers
 * a local-loopback round trip needs are restated here: THR/RBR share
 * register 0, MCR is register 4 and LSR is register 5 in every 16550.
 */
#define SERIAL_A 0x020178
#define SERIAL_B 0x020170
#define REG_THR  0
#define REG_MCR  4
#define REG_LSR  5
#define UART_MCR_LOOP (1u << 4)   /* hw/char/serial.c's own bit, restated */
#define UART_LSR_DR   (1u << 0)

/*
 * The Bridge's own board-identity NIC (one-wire master register), the same
 * offset and bit-banging contract as sn1-xbridge-onewire-test.c's, reached
 * here through BaseIO's crosstalk window instead of that file's standalone
 * per-head sysbus mapping.
 */
#define BRIDGE_NIC 0x0b0

#define NIC_LEVEL (1u << 0)
#define NIC_DONE  (1u << 1)
#define NIC_PULSE(n)  ((uint32_t)(n) << 10)
#define NIC_SAMPLE(n) ((uint32_t)(n) << 2)

/*
 * The Bridge's own combined interrupt-status register and each IOC3's
 * SuperIO pin bit, restated from sn0-ioc3-test.c's own BRIDGE_INT_STATUS
 * and BRIDGE_PIN_SUPERIO. io6g wires device 2's board IOC3 to input 4 and
 * device 6's MediaIO IOC3 to input 6 (hw/mips/sgi/baseio.c's
 * BASEIO_IOC3_IRQ_PIN and BASEIO_G_MIO_IOC3_IRQ_PIN); IP29's single IOC3
 * rides input 3 instead, a different board's pin assignment.
 */
#define BRIDGE_INT_STATUS (0x100 + XTALK_REG_DATA)
#define BRIDGE_PIN_BOARD_IOC3 (1u << 4)
#define BRIDGE_PIN_MIO_IOC3   (1u << 6)

/* docs/datasheets/baseio.md's board-variant table. */
#define IO6G_BOARD_PART "030-0734-002"
#define IO6G_MIO_BOARD_PART "030-0880-003"

static const unsigned int module_nasids[] = { 0x11, 0x12, 0x23, 0x24 };

static uint64_t hub_read(QTestState *qts, unsigned int node, uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 2 * node, HUB_BASE + offset,
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void hub_write(QTestState *qts, unsigned int node, uint64_t offset,
                       uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 2 * node, HUB_BASE + offset,
                                     bytes, sizeof(bytes)));
}

static uint64_t vector_op(QTestState *qts, unsigned int source, uint64_t route,
                           uint64_t address, unsigned int op, uint64_t data)
{
    uint64_t identifiers = ((UINT64_C(0x30) + source + route) << 40) |
                           ((UINT64_C(0x80) + source + op) << 32);
    uint64_t status;

    hub_read(qts, source, NI_VECTOR_CLEAR);
    hub_write(qts, source, NI_VECTOR_DATA, data);
    hub_write(qts, source, NI_VECTOR, route);
    hub_write(qts, source, NI_VECTOR_PARMS,
                 identifiers | address | op);
    status = hub_read(qts, source, NI_VECTOR_STATUS);
    if (status & VECTOR_VALID) {
        g_assert_cmphex(status & (UINT64_C(0x3ffff) << 32), ==, identifiers);
        g_assert_cmphex(status & UINT64_C(0xfffffff8), ==, address & 0xffff8);
    }
    return status;
}

static void vector_write(QTestState *qts, uint64_t route, uint64_t address,
                          uint64_t data)
{
    uint64_t status = vector_op(qts, 0, route, address, 1, data);

    g_assert_cmphex(status & (VECTOR_VALID | 7), ==, VECTOR_VALID | 1);
    g_assert_cmphex(status & VECTOR_SOURCE_MASK, ==, 0);
}

/*
 * machine_options extends the "-M origin2000" argument itself (e.g.
 * ",baseio-preset=io6g"), while extra adds further, ordinary command-line
 * arguments after it -- the two cannot share one string, since the machine
 * type takes only one "-M" and a preset override has to ride inside it.
 */
static QTestState *start_module_with(const char *machine_options,
                                     const char *extra)
{
    QTestState *qts = qtest_initf("-M origin2000%s -smp 8 "
                                  "-m 256M -d unimp,guest_errors %s",
                                  machine_options, extra);
    const unsigned int numbers[] = { 0x11, 0x12, 0x21, 0x22 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(numbers); i++) {
        hub_write(qts, i, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)numbers[i] << 8));
        g_assert_cmphex((hub_read(qts, i, NI_STATUS) >> 8) & 0xff,
                        ==, numbers[i]);
    }
    return qts;
}

static QTestState *start_module(void)
{
    return start_module_with("", "");
}

/* The io6g preset: MediaIO's second IOC3, both chips' keyboard/mouse. */
static QTestState *start_module_io6g(void)
{
    return start_module_with(",baseio-preset=io6g", "");
}

static void program_module(QTestState *qts)
{
    /* N1/N3 enter port 5, N2/N4 port 4; port 6 joins the Routers. */
    static const uint8_t first_turns[][4] = {
        { 0, 5, 1, 1 },
        { 1, 0, 2, 2 },
        { 1, 1, 0, 5 },
        { 2, 2, 1, 0 },
    };
    static const uint32_t router_entries[][4] = {
        { 0xffffff, 0xffffff, 0x5fffff, 0x4fffff },
        { 0x5fffff, 0x4fffff, 0xffffff, 0xffffff },
    };
    unsigned int from, to;

    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        hub_write(qts, from, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)module_nasids[from] << 8));
    }
    for (from = 0; from < G_N_ELEMENTS(router_entries); from++) {
        uint64_t route = from ? 6 : 0;

        /* Distinct local IDs let each Router choose both far-side Hubs. */
        vector_write(qts, route, ROUTER_PROTECTION, UINT64_C(1) << 12);
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            vector_write(qts, route,
                         ROUTER_LOCAL_TABLE + (module_nasids[to] & 15) * 8,
                         router_entries[from][to]);
        }
    }
    for (from = 0; from < G_N_ELEMENTS(module_nasids); from++) {
        for (to = 0; to < G_N_ELEMENTS(module_nasids); to++) {
            bool local = (module_nasids[from] >> 4) ==
                         (module_nasids[to] >> 4);
            uint64_t entry = local ?
                NI_LOCAL_TABLE + (module_nasids[to] & 15) * 8 :
                NI_META_TABLE + (module_nasids[to] >> 4) * 8;

            hub_write(qts, from, entry, first_turns[from][to]);
        }
    }
}

static uint32_t baseio_window_read(QTestState *qts, uint64_t offset)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0,
                                    XTALK_BASE +
                                    module_nasids[0] * NODE_STRIDE +
                                    BASEIO_WIDGET * XTALK_WIDGET_SIZE +
                                    offset,
                                    bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

static void baseio_window_write(QTestState *qts, uint64_t offset,
                                uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0,
                                     XTALK_BASE +
                                     module_nasids[0] * NODE_STRIDE +
                                     BASEIO_WIDGET *
                                     XTALK_WIDGET_SIZE + offset,
                                     bytes, sizeof(bytes)));
}

static uint8_t baseio_window_readb(QTestState *qts, uint64_t offset)
{
    uint8_t value;

    g_assert_true(qtest_memread_cpu(qts, 0,
                                    XTALK_BASE +
                                    module_nasids[0] * NODE_STRIDE +
                                    BASEIO_WIDGET * XTALK_WIDGET_SIZE +
                                    offset,
                                    &value, sizeof(value)));
    return value;
}

static void baseio_window_writeb(QTestState *qts, uint64_t offset,
                                 uint8_t value)
{
    g_assert_true(qtest_memwrite_cpu(qts, 0,
                                     XTALK_BASE +
                                     module_nasids[0] * NODE_STRIDE +
                                     BASEIO_WIDGET * XTALK_WIDGET_SIZE +
                                     offset,
                                     &value, sizeof(value)));
}

static void program_baseio_ioc3(QTestState *qts)
{
    baseio_window_write(qts, BRIDGE_CFG0(IOC3_DEVICE) + PCI_BAR0,
                        IOC3_MMIO);
    baseio_window_write(qts, BRIDGE_CFG0(IOC3_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY);
    baseio_window_write(qts, BRIDGE_DEVICE(IOC3_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (IOC3_MMIO >> 20));
}

static void program_mio_ioc3(QTestState *qts)
{
    baseio_window_write(qts, BRIDGE_CFG0(MIO_IOC3_DEVICE) + PCI_BAR0,
                        MIO_IOC3_MMIO);
    baseio_window_write(qts, BRIDGE_CFG0(MIO_IOC3_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_IOC3_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_IOC3_MMIO >> 20));
}

static void test_baseio_timekeeper_storage_reset(void)
{
    QTestState *qts = start_module();

    program_module(qts);
    program_baseio_ioc3(qts);
    baseio_window_writeb(qts, IOC3_TIMEKEEPER + 0x0100, 0xc3);
    baseio_window_writeb(qts, IOC3_TIMEKEEPER + 0x7ff7, 0x3c);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x0100),
                    ==, 0xc3);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x7ff7),
                    ==, 0x3c);

    qtest_system_reset(qts);
    program_module(qts);
    program_baseio_ioc3(qts);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x0100),
                    ==, 0xc3);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x7ff7),
                    ==, 0x3c);
    qtest_quit(qts);
}

static void test_baseio_timekeeper_repair_stores(void)
{
    static const struct {
        uint16_t offset;
        uint8_t value;
    } clock[] = {
        { 0x7ff8, 0x80 }, { 0x7ff9, 0x00 }, { 0x7ffa, 0x00 },
        { 0x7ffb, 0x00 }, { 0x7ffd, 0x01 }, { 0x7ffe, 0x01 },
        { 0x7fff, 0x56 }, { 0x7ff8, 0x00 },
    };
    static const struct {
        uint8_t marker;
        uint8_t checksum;
    } repairs[] = {
        { 1, 0x6d },
        { 2, 0xad },
    };
    for (unsigned int cpu = 0;
         cpu < 2 * G_N_ELEMENTS(module_nasids); cpu++) {
        QTestState *qts = start_module();
        uint8_t marker = repairs[cpu & 1].marker;
        uint8_t checksum = repairs[cpu & 1].checksum;
        uint64_t window = XTALK_BASE + UINT64_C(0x20000000) +
                          IOC3_TIMEKEEPER;

        if (cpu >= 2) {
            /* The other Crossbow reaches BaseIO through node 0's window. */
            window += module_nasids[0] * NODE_STRIDE;
        }
        g_test_message("source CPU %u", cpu);
        program_module(qts);
        program_baseio_ioc3(qts);
        baseio_window_writeb(qts, IOC3_TIMEKEEPER + 0x3883, 0);
        hub_write(qts, 0, 0x400160, 0x1800);
        g_assert_true(qtest_memwrite_cpu(qts, cpu, window + 0x3883,
                                        &marker, 1));
        g_assert_cmphex(baseio_window_readb(qts,
                                            IOC3_TIMEKEEPER + 0x3883),
                        ==, marker);
        g_assert_true(qtest_memwrite_cpu(qts, cpu, window + 0x04bc,
                                        &checksum, 1));
        for (unsigned int i = 0; i < G_N_ELEMENTS(clock); i++) {
            g_assert_true(qtest_memwrite_cpu(qts, cpu,
                                            window + clock[i].offset,
                                            &clock[i].value, 1));
        }

        g_assert_cmphex(baseio_window_readb(qts,
                                            IOC3_TIMEKEEPER + 0x3883),
                        ==, marker);
        g_assert_cmphex(baseio_window_readb(qts,
                                            IOC3_TIMEKEEPER + 0x04bc),
                        ==, checksum);
        qtest_system_reset(qts);
        program_module(qts);
        program_baseio_ioc3(qts);
        g_assert_cmphex(baseio_window_readb(qts,
                                            IOC3_TIMEKEEPER + 0x3883),
                        ==, marker);
        g_assert_cmphex(baseio_window_readb(qts,
                                            IOC3_TIMEKEEPER + 0x04bc),
                        ==, checksum);
        qtest_quit(qts);
    }
}

static void test_baseio_timekeeper_backing_reopen(void)
{
    g_autofree char *path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-baseio-nvram-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 0x8000), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(path);
    extra = g_strdup_printf("-drive if=none,id=sgi-nvram0,file=%s,format=raw",
                            quoted);

    qts = start_module_with("", extra);
    program_module(qts);
    program_baseio_ioc3(qts);
    baseio_window_writeb(qts, IOC3_TIMEKEEPER + 0x3883, 2);
    baseio_window_writeb(qts, IOC3_TIMEKEEPER + 0x04bc, 0xad);
    qtest_quit(qts);

    qts = start_module_with("", extra);
    program_module(qts);
    program_baseio_ioc3(qts);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x3883),
                    ==, 2);
    g_assert_cmphex(baseio_window_readb(qts, IOC3_TIMEKEEPER + 0x04bc),
                    ==, 0xad);
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_mio_ioc3_subsystem_id(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(MIO_IOC3_DEVICE) +
                                       PCI_SUBSYSTEM),
                    ==, SUBSYSTEM(0xc301));
    qtest_quit(qts);
}

/*
 * io6g's four PS/2 ports come from two separate IOC3 keyboard
 * controllers: device 2's own board IOC3 (BASEIO_G_IOC3_POPULATION) and
 * device 6's MediaIO IOC3. Both carry the same generic
 * TYPE_SGI_IOC3_KBC child (hw/input/sgi-ioc3-kbc.c) wired up purely from
 * the SGI_IOC3_POPULATION_KEYBOARD_MOUSE bit, so a keyboard reset (0xff)
 * on either chip answers with the same ack/self-test-pass pattern --
 * checking both confirms each chip actually enumerates a keyboard/mouse
 * pair rather than only carrying the population bit.
 */
static void test_io6g_ps2(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    program_baseio_ioc3(qts);
    program_mio_ioc3(qts);

    baseio_window_write(qts, IOC3_WINDOW + KBC_KBD_TRANSMIT, 0xff);
    baseio_window_write(qts, IOC3_WINDOW + KBC_AUX_TRANSMIT, 0xff);
    g_assert_cmphex(baseio_window_read(qts, IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0xc0faaa00);
    g_assert_cmphex(baseio_window_read(qts, IOC3_WINDOW + KBC_AUX_RECEIVE),
                    ==, 0xe8faaa00);

    baseio_window_write(qts, MIO_IOC3_WINDOW + KBC_KBD_TRANSMIT, 0xff);
    baseio_window_write(qts, MIO_IOC3_WINDOW + KBC_AUX_TRANSMIT, 0xff);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0xc0faaa00);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + KBC_AUX_RECEIVE),
                    ==, 0xe8faaa00);
    qtest_quit(qts);
}

/*
 * The MediaIO IOC3's own combined interrupt reaches the Bridge on input 6
 * (BASEIO_G_MIO_IOC3_IRQ_PIN), separate from device 2's board IOC3 on
 * input 4 -- the same latched keyboard/mouse source
 * test_the_ps2_interrupt_latches_until_acknowledged exercises against
 * IP29's single chip in sn0-ioc3-test.c, applied here to confirm device 6
 * carries a working interrupt path of its own rather than only
 * enumerating a keyboard/mouse pair with nothing wired behind it.
 */
static void test_io6g_mio_keyboard_reaches_bridge_pin_6(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    program_mio_ioc3(qts);

    /* Nothing queued and nothing enabled: both chips' pins are clear. */
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    (BRIDGE_PIN_BOARD_IOC3 | BRIDGE_PIN_MIO_IOC3), ==, 0);

    /* The enable alone raises nothing while the source is still clear. */
    baseio_window_write(qts, MIO_IOC3_WINDOW + INT_ENABLE_SET, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, 0);

    /*
     * A queued keyboard reply drives the whole chain -- source, combined
     * status, SuperIO GPIO-out, board wire -- to Bridge input 6 alone;
     * device 2's own pin stays clear throughout, confirming the two
     * chips' interrupts are independent, not aliased.
     */
    baseio_window_write(qts, MIO_IOC3_WINDOW + KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, BRIDGE_PIN_MIO_IOC3);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_BOARD_IOC3, ==, 0);

    /* Latched: only an acknowledging write to the source clears the pin. */
    baseio_window_write(qts, MIO_IOC3_WINDOW + INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, 0);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, 0);

    qtest_quit(qts);
}

/*
 * The MediaIO IOC3's own two serial ports carry the same 16550 the board
 * IOC3's do (hw/char/serial.c), exercised here as actual data transport
 * rather than a register echo: the 16550's own local-loopback mode (MCR
 * bit 4) turns a transmitted byte straight back into the receive
 * register, the same synchronous path serial_xmit() takes for any port,
 * with no chardev backend and no guest console involved. These ports are
 * peripheral connections, not a console (baseio_build_io6g()'s own doc
 * comment), so what this confirms is that a byte put in over the MediaIO
 * chip's own window comes back out, not any text.
 */
static void test_io6g_mio_serial_loopback(void)
{
    QTestState *qts = start_module_io6g();
    static const struct {
        uint32_t port;
        uint8_t byte;
    } ports[] = {
        { SERIAL_A, 0x5a },
        { SERIAL_B, 0xa5 },
    };
    unsigned int i;

    program_module(qts);
    program_mio_ioc3(qts);

    for (i = 0; i < G_N_ELEMENTS(ports); i++) {
        uint64_t base = MIO_IOC3_WINDOW + ports[i].port;

        baseio_window_writeb(qts, base + REG_MCR, UART_MCR_LOOP);
        g_assert_cmphex(baseio_window_readb(qts, base + REG_LSR) &
                        UART_LSR_DR, ==, 0);

        baseio_window_writeb(qts, base + REG_THR, ports[i].byte);
        g_assert_cmphex(baseio_window_readb(qts, base + REG_LSR) &
                        UART_LSR_DR, ==, UART_LSR_DR);
        g_assert_cmphex(baseio_window_readb(qts, base + REG_THR), ==,
                        ports[i].byte);

        /* The read drained it: nothing left waiting. */
        g_assert_cmphex(baseio_window_readb(qts, base + REG_LSR) &
                        UART_LSR_DR, ==, 0);
    }

    qtest_quit(qts);
}

/*
 * One bit slot, mainline's widths: a long reset, a short write-one/read,
 * a middling write-zero.
 */
static bool nic_slot(QTestState *qts, unsigned int pulse, unsigned int sample)
{
    uint32_t nic;

    baseio_window_write(qts, BRIDGE_NIC + XTALK_REG_DATA,
                        NIC_PULSE(pulse) | NIC_SAMPLE(sample));
    nic = baseio_window_read(qts, BRIDGE_NIC + XTALK_REG_DATA);
    g_assert_cmphex(nic & NIC_DONE, ==, NIC_DONE);
    return nic & NIC_LEVEL;
}

#define NIC_RESET(q)      nic_slot((q), 520, 65)
#define NIC_READ(q)       nic_slot((q), 6, 13)
#define NIC_WRITE_ONE(q)  nic_slot((q), 6, 13)
#define NIC_WRITE_ZERO(q) nic_slot((q), 80, 30)

static void nic_write_byte(QTestState *qts, uint8_t byte)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if ((byte >> i) & 1) {
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
}

static uint8_t nic_read_byte(QTestState *qts)
{
    unsigned int i;
    uint8_t byte = 0;

    for (i = 0; i < 8; i++) {
        if (NIC_READ(qts)) {
            byte |= 1u << i;
        }
    }
    return byte;
}

static uint16_t nic_crc(uint16_t crc, const uint8_t *data, unsigned int len)
{
    unsigned int i, bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}
#define NIC_CRC_RESIDUE 0xb001

static uint8_t nic_crc8(uint8_t crc, uint64_t rom)
{
    unsigned int byte, bit;

    for (byte = 0; byte < 8; byte++) {
        crc ^= rom >> (byte * 8);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

/*
 * One pass of the standard 1-Wire Search ROM discrepancy walk (Maxim
 * AN187): drive command 0xf0, then for each of the 64 identity bits read
 * the bit and its complement, take the same branch `rom` took last pass
 * below `*last_discrepancy`, the not-yet-explored branch at
 * `*last_discrepancy` itself, and the zero branch first beyond it. Passed
 * `rom` and `*last_discrepancy` both 0 for the first pass, then chained
 * from each pass's return and updated `*last_discrepancy` -- a guest's
 * one-wire dispatch depends on exactly this walk to discover every part a
 * multi-drop bus carries without knowing any of their identities
 * beforehand, which is what lets it tell this board's own board-part
 * record from a fitted daughtercard's.
 */
static uint64_t nic_search_pass(QTestState *qts, uint64_t rom,
                                unsigned int *last_discrepancy)
{
    unsigned int bit_number, last_zero = 0;
    uint64_t found = 0;

    g_assert_false(NIC_RESET(qts));         /* pulled low: someone answers */
    nic_write_byte(qts, 0xf0);              /* search ROM */
    for (bit_number = 1; bit_number <= 64; bit_number++) {
        bool bit = NIC_READ(qts);
        bool complement = NIC_READ(qts);
        bool direction;

        g_assert_false(bit && complement); /* something must answer */
        if (bit != complement) {
            direction = bit;
        } else if (bit_number == *last_discrepancy) {
            direction = true;
        } else if (bit_number > *last_discrepancy) {
            direction = false;
            last_zero = bit_number;
        } else {
            direction = (rom >> (bit_number - 1)) & 1;
            if (!direction) {
                last_zero = bit_number;
            }
        }
        if (direction) {
            found |= UINT64_C(1) << (bit_number - 1);
            NIC_WRITE_ONE(qts);
        } else {
            NIC_WRITE_ZERO(qts);
        }
    }
    *last_discrepancy = last_zero;
    return found;
}

/*
 * The scanning reader: find "name" within the first `len` bytes and copy
 * everything up to the next ';' delimiter, exactly -- the same helper
 * sn1-xbridge-onewire-test.c's page_field() implements, duplicated rather
 * than shared per this project's other qtest files.
 */
static bool page_field(const uint8_t *page, unsigned int len,
                       const char *name, char *out)
{
    unsigned int pos = 0, n = strlen(name);

    while (pos < len) {
        unsigned int i;

        if (pos + n <= len && memcmp(&page[pos], name, n) == 0) {
            unsigned int j = 0;

            for (i = pos + n; i < len && page[i] != ';'; i++) {
                out[j++] = page[i];
            }
            out[j] = 0;
            return true;
        }
        for (i = pos; i < len && page[i] != ';'; i++) {
            /* skip to the next delimiter */
        }
        if (i >= len) {
            return false;
        }
        pos = i + 1;
    }
    return false;
}

/* Select `rom` by Match ROM, then read its part-number page back. */
static void nic_read_part(QTestState *qts, uint64_t rom, char *part)
{
    uint8_t exchange[6];
    uint8_t page[34];
    unsigned int i;

    g_assert_cmphex(nic_crc8(0, rom), ==, 0);

    g_assert_false(NIC_RESET(qts));
    nic_write_byte(qts, 0x55);              /* match ROM */
    for (i = 0; i < 8; i++) {
        nic_write_byte(qts, (uint8_t)(rom >> (i * 8)));
    }

    exchange[0] = 0xa5;                     /* read memory */
    exchange[1] = 0x00;
    exchange[2] = 0x00;
    for (i = 0; i < 3; i++) {
        nic_write_byte(qts, exchange[i]);
    }
    for (i = 3; i < 6; i++) {
        exchange[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, exchange, 6), ==, NIC_CRC_RESIDUE);

    for (i = 0; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, sizeof(page)), ==, NIC_CRC_RESIDUE);

    g_assert_cmpmem(&page[6], 5, "Part:", 5);
    g_assert_true(page_field(page, 30, "Part:", part));
}

/*
 * io6g's Bridge carries two board-identity records on the same
 * multi-drop one-wire bus: the io6g board's own, and the MediaIO
 * daughtercard's, added by sgi_bridge_add_onewire_board_part(). A search
 * from scratch, knowing neither identity beforehand, must discover
 * exactly the two and read each one's own part number back -- the same
 * mechanism and order-independence a guest's one-wire dispatch depends
 * on to tell the two apart.
 */
static void test_io6g_onewire_board_parts(void)
{
    QTestState *qts = start_module_io6g();
    uint64_t rom[4];
    unsigned int last_discrepancy = 0;
    unsigned int count = 0;
    char part[4][13];
    unsigned int i;

    program_module(qts);

    do {
        g_assert_cmpuint(count, <, G_N_ELEMENTS(rom));
        rom[count] = nic_search_pass(qts, count ? rom[count - 1] : 0,
                                     &last_discrepancy);
        count++;
    } while (last_discrepancy != 0);
    g_assert_cmpuint(count, ==, 2);

    for (i = 0; i < count; i++) {
        nic_read_part(qts, rom[i], part[i]);
    }
    g_assert_true((strcmp(part[0], IO6G_BOARD_PART) == 0 &&
                  strcmp(part[1], IO6G_MIO_BOARD_PART) == 0) ||
                 (strcmp(part[0], IO6G_MIO_BOARD_PART) == 0 &&
                  strcmp(part[1], IO6G_BOARD_PART) == 0));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/baseio/baseio-timekeeper-storage-reset",
                   test_baseio_timekeeper_storage_reset);
    qtest_add_func("sn0/baseio/baseio-timekeeper-repair-stores",
                   test_baseio_timekeeper_repair_stores);
    qtest_add_func("sn0/baseio/baseio-timekeeper-backing-reopen",
                   test_baseio_timekeeper_backing_reopen);
    qtest_add_func("sn0/baseio/mio-ioc3-subsystem-id",
                   test_mio_ioc3_subsystem_id);
    qtest_add_func("sn0/baseio/io6g-ps2", test_io6g_ps2);
    qtest_add_func("sn0/baseio/io6g-mio-keyboard-reaches-bridge-pin-6",
                   test_io6g_mio_keyboard_reaches_bridge_pin_6);
    qtest_add_func("sn0/baseio/io6g-mio-serial-loopback",
                   test_io6g_mio_serial_loopback);
    qtest_add_func("sn0/baseio/io6g-onewire-board-parts",
                   test_io6g_onewire_board_parts);
    return g_test_run();
}
