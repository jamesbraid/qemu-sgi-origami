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
#include "sgi-test.h"

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

/*
 * The MediaIO daughtercard's RAD1 audio ASIC, io6g only. Device 5's own
 * 1 MB window (this file's own BRIDGE_DEVICE(5) window) with BAR0, the
 * 0x2000-byte register file, at the window's base. IRIX's rad_attach maps
 * exactly that much through BAR0 and stores the result as the register
 * base every later access uses; a smaller BAR leaves it NULL.
 */
#define MIO_RAD1_DEVICE 5
#define MIO_RAD1_MMIO 0x12000000u
#define MIO_RAD1_WINDOW 0x900000
#define MIO_RAD1_BAR0_SIZE 0x2000u

#define MIO_RAD1_VENDOR_ID 0x10a9
#define MIO_RAD1_DEVICE_ID 0x0005
#define MIO_RAD1_ID_PAIR \
    (((uint32_t)MIO_RAD1_DEVICE_ID << 16) | MIO_RAD1_VENDOR_ID)

/*
 * The revision byte's high nibble is the board mode rad_create_subsys
 * dispatches on (7: MediaIO), its low nibble a sub-revision; hinv prints
 * them as "RAD revision 7.0".
 */
#define MIO_RAD1_REVISION 0x70

/*
 * BAR0's documented register offsets, from
 * docs/datasheets/mediaio-rad1-audio.md's register map and restated
 * independently from hw/audio/sgi-rad1-audio.h's own names, matching
 * this file's convention of not sharing offset macros with the model.
 */
#define RAD1_REG_PREDISPATCH_A    0x44
#define RAD1_REG_PREDISPATCH_B    0x48
#define RAD1_REG_SHIFT_LOAD_CLOCK 0x4c
#define RAD1_REG_PREDISPATCH_C    0x50
#define RAD1_REG_PREDISPATCH_D    0x54
#define RAD1_REG_PREDISPATCH_E    0x58
#define RAD1_REG_STATUS_DMA_CTRL  0x5c
#define RAD1_REG_HALT_TEARDOWN    0x70
#define RAD1_REG_BRINGUP_FIXED_A  0x74
#define RAD1_REG_BRINGUP_FIXED_B  0x78
#define RAD1_REG_MUX_CLOCK_SELECT 0x7c
#define RAD1_REG_SHADOW_MASKED    0x80
#define RAD1_REG_MODE_GATED_A     0x84
#define RAD1_REG_LATCH_PULSE      0x88
#define RAD1_REG_SHIFT_LOAD_DATA  0x8c
#define RAD1_REG_MODE_GATED_B     0x90
#define RAD1_REG_BRINGUP_FINAL    0xa0
#define RAD1_REG_CONVERGENCE_A    0xb8
#define RAD1_REG_CONVERGENCE_B    0xbc

/* A gap the register map documents nothing for, between 0x5c and 0x70. */
#define RAD1_REG_UNDOCUMENTED_GAP 0x60

/*
 * The status-DMA descriptor-ring base/limit registers rad_start_status_dma
 * and rad_one_shot_status program, plus the parallel second set, in the
 * 0x400..0x528 DMA bank. Restated independently from the model's own names.
 */
#define RAD1_REG_STATUS_BASE2_LO  0x4b4
#define RAD1_REG_STATUS_BASE2_HI  0x4b8
#define RAD1_REG_STATUS_LIMIT2    0x4bc
#define RAD1_REG_STATUS_BASE_LO   0x500
#define RAD1_REG_STATUS_LIMIT     0x504
#define RAD1_REG_STATUS_BASE_HI   0x528

/* The read-only per-channel sample-position bank, 0x04..0x2c step 4. */
#define RAD1_REG_SAMPLE_POS_FIRST 0x04
#define RAD1_REG_SAMPLE_POS_LAST  0x2c

/*
 * The free-running UST counter rad_set_ust_offset polls for advancement,
 * read-only, one tick per RAD1_UST_NS_PER_TICK ns of virtual time.
 */
#define RAD1_REG_UST_COUNTER      0x30
#define RAD1_UST_NS_PER_TICK      800

/*
 * Status-DMA control values written to 0x5c: the ring-engine start, the
 * one-shot start, and the stop, and the descriptor-slot layout the engine
 * marks done -- bit 31 of each processed slot's +0x7c word and the frame
 * bits in its +0x78 word. From rad_start_status_dma/rad_one_shot_status/
 * rad_update_subsys and docs/datasheets/mediaio-rad1-audio.md.
 */
#define RAD1_STATUS_START         0x40003u
#define RAD1_STATUS_ONESHOT_START 0x40001u
#define RAD1_STATUS_STOP          0x1fffcu
#define RAD1_SLOT_STRIDE          0x80
#define RAD1_SLOT_DONE_OFFSET     0x7c
#define RAD1_SLOT_FLAGS_OFFSET    0x78
#define RAD1_SLOT_DONE_BIT        0x80000000u
#define RAD1_SLOT_FRAME_BITS      0x180u
#define RAD1_LIMIT_BASE           0x78
/* The continuous engine's cadence, matching the model. */
#define RAD1_SLOT_ADAT_BIT        0x40u       /* +0x78: ADAT-present */
#define RAD1_STATUS_PERIOD_NS     33333334

/*
 * The main audio-data DMA, output-only on the master channel. rad_start_dma
 * programs the master channel's set-A and set-B address-low/high and control
 * registers in the 0x400..0x528 bank, sets the channel's enable bit in the
 * enable register at 0x50, then writes central control 0x80 with the ~0xc0
 * DMA-enable pattern -- the kick. The offsets are the master channel's own
 * (device type 4): rad_create_device loads them from a per-type table, read
 * out of rad_dd.o's .data and its store-back sequence; restated here from the
 * disasm independent of the model's names. Set-A address-low/high are 0x4e8/
 * 0x51c and control 0x4ec; set-B are 0x43c/0x440 and control 0x444. The
 * channel's sample-position registers (index A 0x04, index B 0x08) advance by
 * (frames << 18) as samples transfer. The control word carries the transfer
 * length as a negated count of 128-byte units in bits [31:7]. Sample
 * format/rate are undecoded in the driver; the model streams a fixed
 * big-endian 16-bit stereo voice (4 bytes/frame), a stated approximation, so
 * the frame count a length implies is length / 4.
 */
#define RAD1_MAIN_A_ADDR_LO       0x4e8
#define RAD1_MAIN_A_ADDR_HI       0x51c
#define RAD1_MAIN_A_CTRL          0x4ec
#define RAD1_MAIN_B_ADDR_LO       0x43c
#define RAD1_MAIN_B_ADDR_HI       0x440
#define RAD1_MAIN_B_CTRL          0x444
#define RAD1_MAIN_ENABLE          0x50
#define RAD1_MAIN_ENABLE_BIT      0x1u
#define RAD1_REG_CENTRAL_CTRL     0x80
#define RAD1_CENTRAL_KICK         0xffe20000u /* shadow & ~0xc0 */
#define RAD1_MAIN_POS_A           0x04
#define RAD1_MAIN_POS_B           0x08
#define RAD1_MAIN_UNIT_BYTES      128
#define RAD1_MAIN_FRAME_BYTES     4           /* 16-bit stereo, approximated */
#define RAD1_MAIN_POS_SHIFT       18

/*
 * To land the RAD1's status-DMA in node-0 RAM the ring's PCI address uses
 * the Bridge's 64-bit direct form: bits 63:60 name the local Hub widget
 * (9 on crossbar 0), bits 47:0 pass through as the crosstalk offset, which
 * carries node 0's programmed nasid in bits 39:32 and the RAM offset below.
 * The RAD1 is a little-endian PCI device that is not byte-lane self-ordered,
 * so with its Device register left in the reset (swap-enabled) state the Bridge
 * byte-reverses each DMA word -- exactly as IRIX runs it -- turning the model's
 * little-endian descriptor writes into the big-endian words the driver reads.
 * These tests therefore leave the swap bit clear (the guest reads each ring
 * word big-endian) and only enable bus mastering. Node-0 RAM offset P is
 * CPU-physical module_nasids[0] * NODE_STRIDE + P.
 */
#define RAD1_LOCAL_HUB_WIDGET     9
#define RAD1_PCI_COMMAND_MASTER   0x0004u

/* Chip-relative; combine with an IOC3's own window, e.g. IOC3_WINDOW. */
#define KBC_CONTROL 0x09c
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
 * Chip-relative; the keyboard port's clamp-after-one-character bit, at the
 * position mainline Linux's own header assigns it
 * (arch/mips/include/asm/sn/ioc3.h).
 */
#define KBC_K_CLAMP_1   0x00100000

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
/*
 * Give each node the number the module's routing is written in terms of.
 * Nothing addresses anything before this runs, at boot or after a reset.
 */
static void number_the_nodes(QTestState *qts)
{
    const unsigned int numbers[] = { 0x11, 0x12, 0x21, 0x22 };
    unsigned int i;

    for (i = 0; i < G_N_ELEMENTS(numbers); i++) {
        hub_write(qts, i, NI_STATUS,
                     NI_REGION_SIZE | ((uint64_t)numbers[i] << 8));
        g_assert_cmphex((hub_read(qts, i, NI_STATUS) >> 8) & 0xff,
                        ==, numbers[i]);
    }
}

static QTestState *start_module_with(const char *machine_options,
                                     const char *extra)
{
    QTestState *qts = qtest_initf("-M origin2000%s -smp 8 "
                                  "-m 256M -d unimp,guest_errors %s",
                                  machine_options, extra);

    number_the_nodes(qts);
    return qts;
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

/*
 * One machine per preset serves this file, started on first use and reset
 * for each case that asks for it. Building a four-node module with eight
 * processors and 256 MB is what this file spent its time on: it did it
 * seventeen times, eight of them inside one loop over the processors that
 * may store a repair.
 */
static QTestState *module;
static QTestState *module_io6g;

/*
 * Hand one of them back, reset and renumbered. Every case reprograms the
 * routing and the controller's window itself, exactly as it did after a
 * boot, and checking the controller's memory decode is off here is what
 * proves the reset landed before that reprogramming starts.
 */
static QTestState *reset_module_machine(QTestState **slot,
                                        const char *machine_options)
{
    if (!*slot) {
        *slot = start_module_with(machine_options, "");
        return *slot;
    }

    qtest_system_reset(*slot);
    number_the_nodes(*slot);
    g_assert_cmphex(baseio_window_read(*slot, BRIDGE_CFG0(IOC3_DEVICE) +
                                       PCI_COMMAND) & PCI_COMMAND_MEMORY,
                    ==, 0);
    return *slot;
}

static QTestState *start_module(void)
{
    return reset_module_machine(&module, "");
}

/* The io6g preset: MediaIO's second IOC3, both chips' keyboard/mouse. */
static QTestState *start_module_io6g(void)
{
    return reset_module_machine(&module_io6g, ",baseio-preset=io6g");
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

static void program_rad1(QTestState *qts)
{
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_BAR0,
                        MIO_RAD1_MMIO);
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));
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
}

static void test_io6g_rad1_identity(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) +
                                       PCI_VENDOR_ID),
                    ==, MIO_RAD1_ID_PAIR);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) +
                                       PCI_CLASS_REVISION) & 0xffu,
                    ==, MIO_RAD1_REVISION);

    /* Size BAR0 the way a PCI enumerator does: all ones, read the mask. */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_BAR0,
                        0xffffffffu);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) +
                                       PCI_BAR0) & ~0xfu,
                    ==, (uint32_t)-MIO_RAD1_BAR0_SIZE);
}

/*
 * Every offset docs/datasheets/mediaio-rad1-audio.md's register map
 * documents stores whatever the guest last wrote and answers it back on
 * a later read (hw/audio/sgi-rad1-audio.c's own register-store
 * approximation, not a captured reset/read contract). An undocumented
 * BAR0 offset accepts no store at all, so a write there never surfaces on
 * a later read -- confirming the model keeps unestablished offsets
 * observable rather than silently absorbing them.
 */
static void test_io6g_rad1_registers(void)
{
    static const uint32_t bar0_offsets[] = {
        RAD1_REG_PREDISPATCH_A, RAD1_REG_PREDISPATCH_B,
        RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_REG_PREDISPATCH_C,
        RAD1_REG_PREDISPATCH_D, RAD1_REG_PREDISPATCH_E,
        RAD1_REG_STATUS_DMA_CTRL, RAD1_REG_HALT_TEARDOWN,
        RAD1_REG_BRINGUP_FIXED_A, RAD1_REG_BRINGUP_FIXED_B,
        RAD1_REG_MUX_CLOCK_SELECT, RAD1_REG_SHADOW_MASKED,
        RAD1_REG_MODE_GATED_A, RAD1_REG_LATCH_PULSE,
        RAD1_REG_SHIFT_LOAD_DATA, RAD1_REG_MODE_GATED_B,
        RAD1_REG_BRINGUP_FINAL, RAD1_REG_CONVERGENCE_A,
        RAD1_REG_CONVERGENCE_B,
    };
    QTestState *qts = start_module_io6g();

    program_module(qts);
    program_rad1(qts);

    for (unsigned int i = 0; i < G_N_ELEMENTS(bar0_offsets); i++) {
        uint32_t value = 0xa5000000u | (i << 4);

        baseio_window_write(qts, MIO_RAD1_WINDOW + bar0_offsets[i], value);
        g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                           bar0_offsets[i]),
                        ==, value);
    }

    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_UNDOCUMENTED_GAP,
                        0xdeadbeef);
    g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                       RAD1_REG_UNDOCUMENTED_GAP),
                    ==, 0);
}

/*
 * The status-DMA register bank the completion handshake reads. The ring
 * base/limit registers (both sets) store and answer back like the bring-up
 * offsets above; the read-only sample-position bank answers its fixed
 * modeled value whatever is written (the model has no audio data path to
 * advance it yet); and the 0x5c control register takes a non-start value
 * (the stop word) as a plain store, exercised here without triggering a DMA.
 */
static void test_io6g_rad1_status_dma_registers(void)
{
    static const uint32_t bank_offsets[] = {
        RAD1_REG_STATUS_BASE2_LO, RAD1_REG_STATUS_BASE2_HI,
        RAD1_REG_STATUS_LIMIT2, RAD1_REG_STATUS_BASE_LO,
        RAD1_REG_STATUS_LIMIT, RAD1_REG_STATUS_BASE_HI,
    };
    QTestState *qts = start_module_io6g();
    uint32_t pos;

    program_module(qts);
    program_rad1(qts);

    for (unsigned int i = 0; i < G_N_ELEMENTS(bank_offsets); i++) {
        uint32_t value = 0x5c000000u | (i << 8) | 0x11u;

        baseio_window_write(qts, MIO_RAD1_WINDOW + bank_offsets[i], value);
        g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                           bank_offsets[i]),
                        ==, value);
    }

    /* Read-only bank: a fixed modeled value, and a write leaves it fixed. */
    for (pos = RAD1_REG_SAMPLE_POS_FIRST; pos <= RAD1_REG_SAMPLE_POS_LAST;
         pos += 4) {
        g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW + pos),
                        ==, 0);
    }
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_SAMPLE_POS_FIRST,
                        0x12345678);
    g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                       RAD1_REG_SAMPLE_POS_FIRST),
                    ==, 0);

    /* The stop word is a plain store; only a start word kicks a DMA. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_DMA_CTRL,
                        RAD1_STATUS_STOP);
    g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                       RAD1_REG_STATUS_DMA_CTRL),
                    ==, RAD1_STATUS_STOP);
}

/* The device reads its descriptors big-endian; ring words move as raw bytes. */
static void rad1_ring_write(QTestState *qts, uint64_t ram, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    stl_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, ram, bytes, sizeof(bytes)));
}

static uint32_t rad1_ring_read(QTestState *qts, uint64_t ram)
{
    uint8_t bytes[sizeof(uint32_t)];

    g_assert_true(qtest_memread_cpu(qts, 0, ram, bytes, sizeof(bytes)));
    return ldl_be_p(bytes);
}

/*
 * The one-shot status-DMA handshake. rad_one_shot_status programs the ring
 * base and a -4 limit, writes the one-shot start word (0x40001, ring bit
 * clear) to 0x5c, and busy-polls the base slot's +0x7c bit 31; the engine
 * transfers status into that single slot and marks it done -- bit 31 of its
 * +0x7c word and the frame bits in its +0x78 word. This places a ring in
 * node-0 RAM, kicks the one-shot, and confirms the model set those bits
 * without disturbing the slot's other bits, and left the adjacent slot alone.
 * This is the boot-liveness handshake; the continuous engine that advances the
 * streaming rate is exercised separately below.
 */
static void test_io6g_rad1_status_dma_handshake(void)
{
    QTestState *qts = start_module_io6g();
    const uint64_t ring_off = 0x02000000;
    const uint64_t ring_ram = module_nasids[0] * NODE_STRIDE + ring_off;
    const uint64_t ring_pci =
        ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | ring_ram;
    const uint64_t slot1 = ring_ram + RAD1_SLOT_STRIDE;

    program_module(qts);
    program_rad1(qts);

    /*
     * Enable bus mastering, and leave the Device register's swap bit clear so
     * the Bridge byte-reverses the device's little-endian DMA into the
     * big-endian words the guest reads -- the configuration IRIX runs, and the
     * one the model's little-endian descriptor writes target.
     */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /*
     * Seed the base slot and the next one with a distinct not-done pattern, so
     * the handshake's read-modify-write (bits OR'd in, the rest preserved) is
     * visible rather than a blind overwrite.
     */
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_DONE_OFFSET, 0x00000010u);
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_FLAGS_OFFSET, 0x00000020u);
    rad1_ring_write(qts, slot1 + RAD1_SLOT_DONE_OFFSET, 0x00000011u);

    /* Program the ring base and a -4 limit, then the one-shot start. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_LO,
                        (uint32_t)ring_pci);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_HI,
                        (uint32_t)(ring_pci >> 32));
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_LIMIT,
                        0xfffffffcu);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_DMA_CTRL,
                        RAD1_STATUS_ONESHOT_START);

    /* The control register took the start word. */
    g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                       RAD1_REG_STATUS_DMA_CTRL),
                    ==, RAD1_STATUS_ONESHOT_START);

    /* The base slot is marked done, with its seeded low bits preserved. */
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_BIT | 0x00000010u);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_FLAGS_OFFSET),
                    ==, RAD1_SLOT_FRAME_BITS | 0x00000020u);

    /* The one-shot touches only the base slot; the next one is untouched. */
    g_assert_cmphex(rad1_ring_read(qts, slot1 + RAD1_SLOT_DONE_OFFSET),
                    ==, 0x00000011u);
}

/*
 * The continuous status-DMA engine. rad_start_status_dma writes the ring-engine
 * start word (0x40003, ring bit set) to 0x5c; the engine then keeps producing
 * completions around the ring on its own, which rad_update_subsys counts into a
 * nonzero streaming rate that locks the audio clock. The model runs this from a
 * periodic timer that completes the next ring slot each period -- so, unlike
 * the one-shot, no slot completes on the register write itself; the completions
 * arrive as the virtual clock advances. This seeds a four-slot ring, starts the
 * engine, and confirms that stepping the clock completes successive distinct
 * slots (each with the done bit and the frame bits, and the ADAT-present bit
 * left clear so the analog output can start), and that the stop word halts the
 * engine so no further slot completes.
 */
static void test_io6g_rad1_status_dma_continuous(void)
{
    QTestState *qts = start_module_io6g();
    const uint64_t ring_off = 0x02000000;
    const uint64_t ring_ram = module_nasids[0] * NODE_STRIDE + ring_off;
    const uint64_t ring_pci =
        ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | ring_ram;
    const unsigned int nslots = 4;
    const uint32_t limit = RAD1_LIMIT_BASE - nslots * RAD1_SLOT_STRIDE;
    unsigned int i;

    program_module(qts);
    program_rad1(qts);

    /* Bus mastering with the Bridge swap left on, as in the handshake test. */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /* Clear every slot's completion words. */
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_FLAGS_OFFSET, 0);
    }

    /* Program the ring base and the count-encoding limit, then start. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_LO,
                        (uint32_t)ring_pci);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_HI,
                        (uint32_t)(ring_pci >> 32));
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_LIMIT, limit);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_DMA_CTRL,
                        RAD1_STATUS_START);

    /* The control register took the start word. */
    g_assert_cmphex(baseio_window_read(qts, MIO_RAD1_WINDOW +
                                       RAD1_REG_STATUS_DMA_CTRL),
                    ==, RAD1_STATUS_START);

    /* The engine runs on the timer: nothing completes before a clock step. */
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET) &
                        RAD1_SLOT_DONE_BIT, ==, 0);
    }

    /*
     * Step across three periods. The engine completes the next slot each
     * period, starting at slot 1 (one past the reset consumer index), so slots
     * 1, 2 and 3 are now done with the done bit and the frame flags (and the
     * ADAT-present bit clear) -- the multiple, distinct completions that prove
     * the continuous engine, not a single burst.
     */
    qtest_clock_step(qts, 7 * RAD1_STATUS_PERIOD_NS / 2); /* 3.5 periods */
    for (i = 1; i <= 3; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;
        uint32_t done = rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET);
        uint32_t flags = rad1_ring_read(qts, slot + RAD1_SLOT_FLAGS_OFFSET);

        g_assert_cmphex(done & RAD1_SLOT_DONE_BIT, ==, RAD1_SLOT_DONE_BIT);
        g_assert_cmphex(flags & RAD1_SLOT_FRAME_BITS, ==, RAD1_SLOT_FRAME_BITS);
        /*
         * The ADAT-present bit stays clear: rad_start_dma starts the master
         * channel's analog output only when the last completed slot reports
         * ADAT absent, so a set bit here would lock the clock but silence
         * playback.
         */
        g_assert_cmphex(flags & RAD1_SLOT_ADAT_BIT, ==, 0);
    }
    /* Only three ticks: the base slot has not been reached yet. */
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);

    /* Stop halts the engine: clear every slot, step, nothing completes. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_DMA_CTRL,
                        RAD1_STATUS_STOP);
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
    }
    qtest_clock_step(qts, 3 * RAD1_STATUS_PERIOD_NS);
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET) &
                        RAD1_SLOT_DONE_BIT, ==, 0);
    }
}

/*
 * The main audio-data DMA: output-only streaming on the master channel.
 * This mirrors what rad_start_dma does at a stream start -- it starts the
 * status ring (whose completion handshake this suite already covers), then
 * programs the master channel's main-DMA address/control registers, sets the
 * channel's enable bit, and kicks central control 0x80. On the kick the model
 * DMA-reads the programmed buffer out to its audio backend and advances the
 * channel's sample-position registers by (frames << 18), so the driver's MSC
 * tracks. rad_stop_dma clearing the enable bit halts it: a later kick with no
 * enable moves nothing. The audio bytes themselves reach a host sink only
 * through -audiodev (the wav backend), checked by the end-to-end boot test,
 * not here; this asserts the register-visible contract -- position advance,
 * ring completion, and the stop -- with no audio device.
 */
static void test_io6g_rad1_main_dma_stream(void)
{
    QTestState *qts = start_module_io6g();
    const uint64_t ring_off = 0x02000000;
    const uint64_t ring_ram = module_nasids[0] * NODE_STRIDE + ring_off;
    const uint64_t ring_pci =
        ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | ring_ram;
    const uint64_t buf_off = 0x02100000;
    const uint64_t buf_ram = module_nasids[0] * NODE_STRIDE + buf_off;
    const uint64_t buf_pci = ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | buf_ram;
    const uint32_t nframes = 128;
    const uint32_t length = nframes * RAD1_MAIN_FRAME_BYTES;   /* 512 bytes */
    const uint32_t units = length / RAD1_MAIN_UNIT_BYTES;      /* 4 units */
    /* Control word: negated 128-byte-unit count in bits [31:7], tag 0. */
    const uint32_t ctrl = ((0u - units) & 0x1ffffffu) << 7;
    const uint32_t expect_adv = nframes << RAD1_MAIN_POS_SHIFT;
    const uint32_t stop_limit = 0xfffffffcu;
    uint32_t pos_a_before, pos_a_after, pos_b_after, pos_a_stopped;
    unsigned int i;

    program_module(qts);
    program_rad1(qts);

    /*
     * Bus mastering with the Bridge swap left on (as in the status-DMA
     * handshake test): the device's little-endian DMA is byte-reversed into
     * the big-endian words the guest reads.
     */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /* A single-slot status ring, its completion word cleared. */
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_DONE_OFFSET, 0);

    /* A known PCM buffer in guest RAM for the main DMA to move. */
    for (i = 0; i < length; i += 4) {
        rad1_ring_write(qts, buf_ram + i, 0xa5a50000u | i);
    }

    /* The sample-position bank reads its base value before any DMA runs. */
    pos_a_before = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_MAIN_POS_A);
    g_assert_cmphex(pos_a_before, ==, 0);

    /* Start the status ring; its slot is marked done (the completion). */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_LO,
                        (uint32_t)ring_pci);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_BASE_HI,
                        (uint32_t)(ring_pci >> 32));
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_LIMIT,
                        stop_limit);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_STATUS_DMA_CTRL,
                        RAD1_STATUS_ONESHOT_START);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, RAD1_SLOT_DONE_BIT);

    /* Program the master channel's set-A and set-B triplets. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_A_ADDR_LO,
                        (uint32_t)buf_pci);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_A_ADDR_HI,
                        (uint32_t)(buf_pci >> 32));
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_A_CTRL, ctrl);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_B_ADDR_LO,
                        (uint32_t)buf_pci);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_B_ADDR_HI,
                        (uint32_t)(buf_pci >> 32));
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_B_CTRL, ctrl);

    /* Set the channel's enable bit, then kick central control 0x80. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_ENABLE,
                        RAD1_MAIN_ENABLE_BIT);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_CENTRAL_CTRL,
                        RAD1_CENTRAL_KICK);

    /* Both of the channel's position registers advanced by (frames << 18). */
    pos_a_after = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_MAIN_POS_A);
    pos_b_after = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_MAIN_POS_B);
    g_assert_cmphex(pos_a_after - pos_a_before, ==, expect_adv);
    g_assert_cmphex(pos_b_after, ==, expect_adv);

    /*
     * rad_stop_dma clears the channel's enable bit (a write to 0x50 with the
     * bit clear). A later kick with the address still programmed then moves
     * nothing -- the position holds.
     */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_MAIN_ENABLE, 0);
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_CENTRAL_CTRL,
                        RAD1_CENTRAL_KICK);
    pos_a_stopped = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_MAIN_POS_A);
    g_assert_cmphex(pos_a_stopped, ==, pos_a_after);
}

/*
 * The free-running UST counter at 0x30. rad_set_ust_offset reads it twice a
 * fixed delay apart and warns "RAD UST not incrementing" if it moved by fewer
 * than three ticks; the model derives it from the virtual clock, so stepping
 * that clock advances it in proportion. Confirm it advances by about the
 * expected tick count across a clock step (not a constant, not a fixed
 * just-past-threshold value), and that a write cannot change it.
 */
static void test_io6g_rad1_ust_counter(void)
{
    QTestState *qts = start_module_io6g();
    const int64_t step_ns = 80000;                       /* 80 us */
    const uint32_t expect = step_ns / RAD1_UST_NS_PER_TICK; /* 100 ticks */
    uint32_t before, after, again;

    program_module(qts);
    program_rad1(qts);

    before = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_REG_UST_COUNTER);
    qtest_clock_step(qts, step_ns);
    after = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_REG_UST_COUNTER);

    /* Advanced with the clock, by about step_ns/tick (allow flooring slop). */
    g_assert_cmpuint(after, >, before);
    g_assert_cmpuint(after - before, >=, expect - 2);
    g_assert_cmpuint(after - before, <=, expect + 2);

    /* Read-only: a write neither sticks nor rewinds the clock-derived count. */
    baseio_window_write(qts, MIO_RAD1_WINDOW + RAD1_REG_UST_COUNTER,
                        0xdeadbeef);
    again = baseio_window_read(qts, MIO_RAD1_WINDOW + RAD1_REG_UST_COUNTER);
    g_assert_cmphex(again, !=, 0xdeadbeef);
    g_assert_cmpuint(again, >=, after);
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
}

/*
 * The same route, carrying a receive backlog. The controller holds three
 * characters at most, and the clamp bits hold a port's clock line low once
 * it has taken its quota, so a device with more to say is stopped rather
 * than overrunning it. Armed after one character -- the tighter of the two
 * clamps the control register offers -- a two-character reply arrives in two
 * instalments, and the second instalment has to reach Bridge input 6 on its
 * own, with nothing but the guest's own read of the first to prompt it.
 * sn0-ioc3-test.c's ps2-clamp-after-one-splits-a-reply covers the same
 * mechanism on the chip alone; what this adds is the board's wire.
 */
static void test_io6g_mio_keyboard_backlog_reaches_bridge_pin_6(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    program_mio_ioc3(qts);

    baseio_window_write(qts, MIO_IOC3_WINDOW + INT_ENABLE_SET, INT_KEYBOARD);
    baseio_window_write(qts, MIO_IOC3_WINDOW + KBC_CONTROL, KBC_K_CLAMP_1);

    /* The reset reply is two characters; the clamp lets one through. */
    baseio_window_write(qts, MIO_IOC3_WINDOW + KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, BRIDGE_PIN_MIO_IOC3);

    /*
     * Acknowledge before reading, the order mainline Linux's edge handler
     * uses. The pin falls: a character is still held, but an already
     * asserted line does not latch the source a second time.
     */
    baseio_window_write(qts, MIO_IOC3_WINDOW + INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, 0);

    /* The ACK alone, in the oldest slot. */
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0x80fa0000);

    /*
     * That read emptied the holding and released the clamp, so the stopped
     * keyboard sent its second character. Its arrival drives the whole chain
     * again -- source, combined status, SuperIO GPIO-out, board wire -- with
     * no other event to help it, and device 2's pin stays clear.
     */
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, BRIDGE_PIN_MIO_IOC3);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_BOARD_IOC3, ==, 0);

    /* The second character, and then a drained port and a quiet pin. */
    baseio_window_write(qts, MIO_IOC3_WINDOW + INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + KBC_KBD_RECEIVE),
                    ==, 0x80aa0000);
    g_assert_cmphex(baseio_window_read(qts, MIO_IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, 0);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, 0);
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
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/baseio/baseio-timekeeper-storage-reset",
                   test_baseio_timekeeper_storage_reset);
    qtest_add_func("sn0/baseio/baseio-timekeeper-repair-stores",
                   test_baseio_timekeeper_repair_stores);
    qtest_add_func("sn0/baseio/baseio-timekeeper-backing-reopen",
                   test_baseio_timekeeper_backing_reopen);
    qtest_add_func("sn0/baseio/mio-ioc3-subsystem-id",
                   test_mio_ioc3_subsystem_id);
    qtest_add_func("sn0/baseio/io6g-rad1-identity",
                   test_io6g_rad1_identity);
    qtest_add_func("sn0/baseio/io6g-rad1-registers",
                   test_io6g_rad1_registers);
    qtest_add_func("sn0/baseio/io6g-rad1-status-dma-registers",
                   test_io6g_rad1_status_dma_registers);
    qtest_add_func("sn0/baseio/io6g-rad1-status-dma-handshake",
                   test_io6g_rad1_status_dma_handshake);
    qtest_add_func("sn0/baseio/io6g-rad1-status-dma-continuous",
                   test_io6g_rad1_status_dma_continuous);
    qtest_add_func("sn0/baseio/io6g-rad1-main-dma-stream",
                   test_io6g_rad1_main_dma_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-ust-counter",
                   test_io6g_rad1_ust_counter);
    qtest_add_func("sn0/baseio/io6g-ps2", test_io6g_ps2);
    qtest_add_func("sn0/baseio/io6g-mio-keyboard-reaches-bridge-pin-6",
                   test_io6g_mio_keyboard_reaches_bridge_pin_6);
    qtest_add_func("sn0/baseio/io6g-mio-keyboard-backlog-reaches-bridge-pin-6",
                   test_io6g_mio_keyboard_backlog_reaches_bridge_pin_6);
    qtest_add_func("sn0/baseio/io6g-mio-serial-loopback",
                   test_io6g_mio_serial_loopback);
    qtest_add_func("sn0/baseio/io6g-onewire-board-parts",
                   test_io6g_onewire_board_parts);

    ret = g_test_run();
    if (module) {
        qtest_quit(module);
    }
    if (module_io6g) {
        qtest_quit(module_io6g);
    }

    return ret;
}
