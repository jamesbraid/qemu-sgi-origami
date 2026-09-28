/*
 * Origin 2000 BaseIO Bridge tests: daughtercard IOC3 and Timekeeper
 * registers.
 * Four-node Hub, router and topology tests are in sn0-origin2000-test.c.
 * Local start_module()/program_module() helpers follow other qtests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/pci/pci.h"
#include "libqtest.h"
#include "sgi-launch.h"
#include "sgi-migrate.h"
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
/*
 * The Bridge device register's byte-swap and 64-bit-device bits, restated
 * from hw/pci-host/sgi-bridge.c's DEV_SWAP_PROGRAMMED and DEV_64BIT_DEVICE.
 */
#define BRIDGE_DEV_SWAP (1u << 13)
#define BRIDGE_DEV_64BIT (1u << 22)
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
 * The MediaIO daughtercard's RAD1 audio ASIC, io6g only, at Bridge PCI
 * device 7, where a real BaseIO-G owner's hinv -v lists it.
 * Device 7's own 1 MB window (this file's own
 * BRIDGE_DEVICE(7) window) with BAR0, the 0x2000-byte register file, at the
 * window's base. IRIX's rad_attach maps exactly that much through BAR0 and
 * stores the result as the register base every later access uses; a
 * smaller BAR leaves it NULL. The same inventory shows no function at
 * device 5.
 */
#define MIO_RAD1_DEVICE 7
#define MIO_RAD1_MMIO 0x12000000u
#define MIO_RAD1_WINDOW 0xb00000
#define MIO_EMPTY_DEVICE 5
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
 * BAR0's documented register offsets, restated
 * independently from hw/audio/sgi-rad1-audio.h's own names, matching
 * this file's convention of not sharing offset macros with the model.
 */
#define RAD1_REG_PREDISPATCH_A    0x44
#define RAD1_REG_PREDISPATCH_B    0x48
#define RAD1_REG_SHIFT_LOAD_CLOCK 0x4c
#define RAD1_REG_PREDISPATCH_C    0x50
#define RAD1_REG_PREDISPATCH_D    0x54
#define RAD1_REG_ANALOG_OUT_EN    0x58
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
 * one-shot start, and the stop, from the RAD1 driver's
 * rad_start_status_dma, rad_one_shot_status and rad_stop_status_dma.
 *
 * What a completion leaves in its 0x80-byte slot, from the streaming-engine
 * contract and status-slot flag bits: registers 0x04..0x34
 * mirrored at register + 0x40, type 1's DMA pointer at +0x38, the flag word
 * at +0x78 and the completion word at +0x7c. With no ADAT, AES3 or video
 * reference present, +0x78 has ADAT-present (0x40) and the two video-field
 * bits (0x80, 0x100) clear, and +0x7c reads 0x80080000: done plus bit 19,
 * "no valid AES3 input".
 */
#define RAD1_STATUS_START         0x40003u
#define RAD1_STATUS_ONESHOT_START 0x40001u
#define RAD1_STATUS_STOP          0x1fffcu
#define RAD1_SLOT_STRIDE          0x80
#define RAD1_SLOT_MIRROR_OFFSET   0x40
#define RAD1_SLOT_OUT_PTR         0x38
#define RAD1_SLOT_DONE_OFFSET     0x7c
#define RAD1_SLOT_FLAGS_OFFSET    0x78
#define RAD1_SLOT_DONE_BIT        0x80000000u
#define RAD1_SLOT_DONE_WORD       0x80080000u
#define RAD1_SLOT_INPUT_BITS      0x1c0u      /* +0x78: 0x40, 0x80, 0x100 */
#define RAD1_LIMIT_BASE           0x78

/*
 * IRIX 6.5.30 writes continuous-start bits plus interval bits [16:2],
 * 250 * 33 + 1 = 8251. The model counts 33 MHz PCI clocks for roughly 250 us
 * between completions on an absolute schedule; this is an approximation,
 * not verified hardware behavior.
 */
#define RAD1_STATUS_INTERVAL      8251u
#define RAD1_STATUS_START_IRIX    (RAD1_STATUS_START | \
                                   (RAD1_STATUS_INTERVAL << 2))
#define RAD1_STATUS_CLOCK_HZ      33000000u
/* The model's host-protection floor: 330 clocks, 10 us. */
#define RAD1_STATUS_FLOOR_NS      10000
#define RAD1_UST_TICKS(ns)        ((ns) / RAD1_UST_NS_PER_TICK)

/*
 * Analog output, channel type 1:
 * set A address-low/high/control at 0x4f8/0x524/0x4fc, set B at
 * 0x40c/0x410/0x414, enable bit 0 of 0x58 (bring-up value 0x20000000),
 * position registers A = 0x2c and B = 0x18. A holds the frame count in
 * bits [31:18] and a 0x30 latch in [17:0]; type 1 reads B's bits [17:4],
 * type 0 its bits [31:18]. The control word is (tag << 3) | (-count << 7),
 * count in 128-byte units; sfplay's start writes 0xfffff008, tag
 * 1 and a 4 KiB ring. The analog input's (type 0) enable is 0x4c bit 0 and
 * its A register is 0x14. A frame is two 32-bit ring words, one channel
 * each. The rate (44.1 kHz) and the position registers counting frames are
 * flagged model approximations. The refill lead is 0xcc words:
 * the pointer must move well under that between completions.
 */
#define RAD1_OUT_A_ADDR_LO        0x4f8
#define RAD1_OUT_A_ADDR_HI        0x524
#define RAD1_OUT_A_CTRL           0x4fc
#define RAD1_OUT_B_ADDR_LO        0x40c
#define RAD1_OUT_B_ADDR_HI        0x410
#define RAD1_OUT_B_CTRL           0x414
#define RAD1_OUT_CTRL_4K          0xfffff008u
#define RAD1_OUT_RING_BYTES       4096
#define RAD1_OUT_IDLE             0x20000000u
#define RAD1_OUT_RUN              0x20000001u
#define RAD1_OUT_POS_A            0x2c
#define RAD1_OUT_POS_B            0x18
#define RAD1_IN_POS_A             0x14
#define RAD1_IN_ENABLE_IDLE       0x05000000u
#define RAD1_IN_ENABLE_RUN        0x05000001u
/* 0x4c bit 27, the clock the MIO mux bit-bang pulses. */
#define RAD1_IN_MUX_CLOCK         0x08000000u

/*
 * The analog input, channel type 0, from the same per-type table: set A
 * address-low/high/control at 0x4d0/0x510/0x4d4, set B at
 * 0x400/0x404/0x408, enable bit 0 of 0x4c, position registers A = 0x14
 * and B = 0x18 bits [31:18], status-slot pointer word +0x10. Its control
 * word for a 4 KiB ring carries tag 0. The model writes each 16-bit host
 * sample into bits [23:8] of a ring word, sign-extended, low byte zero:
 * the output's layout, specified for output only.
 */
#define RAD1_IN_A_ADDR_LO         0x4d0
#define RAD1_IN_A_ADDR_HI         0x510
#define RAD1_IN_A_CTRL            0x4d4
#define RAD1_IN_B_ADDR_LO         0x400
#define RAD1_IN_B_ADDR_HI         0x404
#define RAD1_IN_B_CTRL            0x408
#define RAD1_IN_CTRL_4K           0xfffff000u
#define RAD1_SLOT_IN_PTR          0x10
#define RAD1_IN_POISON            0xdeadbeefu
#define RAD1_REG_CENTRAL_CTRL     0x80
#define RAD1_CENTRAL_ADAT_OFF     0xffe20000u
#define RAD1_OUT_FREQ             44100
#define RAD1_OUT_FRAME_BYTES      8
#define RAD1_OUT_REFILL_LEAD_WORDS 0xcc
#define RAD1_OUT_TICK_FRAMES      8
#define RAD1_POS_A_FRAMES(v)      ((v) >> 18)
#define RAD1_POS_A_LATCH(v)       ((v) & 0x3ffffu)
#define RAD1_POS_B_TYPE1(v)       (((v) >> 4) & 0x3fffu)
#define RAD1_POS_B_TYPE0(v)       ((v) >> 18)

/*
 * To land the RAD1's status-DMA in node-0 RAM the ring's PCI address uses
 * the Bridge's 64-bit direct form: bits 63:60 name the local Hub widget
 * (9 on crossbar 0), bits 47:0 pass through as the crosstalk offset, which
 * carries node 0's programmed nasid in bits 39:32 and the RAM offset below.
 * The RAD1 is a little-endian PCI device that is not byte-lane self-ordered,
 * With its Device register in the reset (swap-enabled) state, the
 * Bridge byte-reverses each DMA word as IRIX runs it, turning the model's
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

/* The BaseIO board-variant part numbers. */
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
 * arguments after it. The two cannot share one string, since the machine
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

/*
 * A device the BaseIO builders create that fails to realize, here for want
 * of its option ROM, must stop the machine with that device's own error
 * and a failure exit, not a SIGABRT from the builder. One case per preset
 * builder that realizes the device: the board IOC3 and QLogic ISP1040s of
 * io6g and of IP29, and the io8 ISP12160, IOC3 and OHCI.
 */
static void test_baseio_rejects_bad_romfile(void)
{
    static const struct {
        const char *machine;
        const char *global;
    } cases[] = {
        { "origin2000,baseio-preset=io6g", "sgi-ioc3.romfile" },
        { "origin2000,baseio-preset=io6g", "qlogic-isp1040.romfile" },
        { "origin200", "sgi-ioc3.romfile" },
        { "origin200", "qlogic-isp1040.romfile" },
        { "origin300", "qlogic-isp12160.romfile" },
        { "origin300", "sgi-ioc3.romfile" },
        { "origin300", "lucent-ohci.romfile" },
    };

    for (unsigned int i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree char *global = g_strdup_printf("%s=/nonexistent.rom",
                                                  cases[i].global);
        const char *args[] = {
            qtest_qemu_binary(NULL), "-M", cases[i].machine,
            "-accel", "qtest", "-display", "none", "-S", "-nodefaults",
            "-qmp", "stdio", "-global", global, NULL,
        };

        assert_launch_rejected(args,
                               "failed to find romfile \"/nonexistent.rom\"");
    }
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

/*
 * The owner inventory's placement: RAD1's identity at device 7 and no
 * function at device 5. An empty slot reads all ones.
 */
static void test_io6g_rad1_slot(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(7) + PCI_VENDOR_ID),
                    ==, MIO_RAD1_ID_PAIR);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_CFG0(MIO_EMPTY_DEVICE) +
                                       PCI_VENDOR_ID),
                    ==, 0xffffffffu);
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
 * Every offset the RAD1 register map documents stores whatever the guest
 * last wrote and answers it back on
 * a later read (hw/audio/sgi-rad1-audio.c's own register-store
 * approximation, not an observed reset/read contract). An undocumented
 * BAR0 offset accepts no store at all, so a write there never surfaces on
 * a later read, confirming the model keeps unestablished offsets
 * observable rather than silently absorbing them.
 */
static void test_io6g_rad1_registers(void)
{
    static const uint32_t bar0_offsets[] = {
        RAD1_REG_PREDISPATCH_A, RAD1_REG_PREDISPATCH_B,
        RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_REG_PREDISPATCH_C,
        RAD1_REG_PREDISPATCH_D, RAD1_REG_ANALOG_OUT_EN,
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
 * offsets above; the read-only sample-position bank reads zero after reset
 * and ignores a write (only a running channel moves it); and the 0x5c
 * control register takes a non-start value (the stop word) as a plain
 * store, exercised here without triggering a DMA.
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

    /* Read-only bank: zero after reset, and a write leaves it there. */
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

/* The device reads descriptors big-endian; ring words move as raw bytes. */
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
 * transfers status into that single slot and marks it done. This places a
 * ring in node-0 RAM, seeds the slot with stale words, kicks the one-shot,
 * and confirms the completion rewrote the flag and completion words to the
 * no-input values, left the analog output's pointer word alone (the channel
 * has not run since reset), and left the adjacent slot alone.
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
     * big-endian words the guest reads, the configuration IRIX runs, and the
     * one the model's little-endian descriptor writes target.
     */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /*
     * Seed the base slot with stale words, including the ADAT and video-field
     * bits a completion must not report, and the next slot with a marker.
     */
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_DONE_OFFSET, 0x00000010u);
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_FLAGS_OFFSET,
                    0x00000020u | RAD1_SLOT_INPUT_BITS);
    rad1_ring_write(qts, ring_ram + RAD1_SLOT_OUT_PTR, 0xdeadbeefu);
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

    /* Done, with AES3 absent; no ADAT and no video-field events. */
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_FLAGS_OFFSET) &
                    RAD1_SLOT_INPUT_BITS, ==, 0);

    /* The analog output has not run since reset, so its word is untouched. */
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_OUT_PTR),
                    ==, 0xdeadbeefu);

    /* The one-shot touches only the base slot; the next one is untouched. */
    g_assert_cmphex(rad1_ring_read(qts, slot1 + RAD1_SLOT_DONE_OFFSET),
                    ==, 0x00000011u);
}

static uint32_t rad1_read(QTestState *qts, uint32_t reg)
{
    return baseio_window_read(qts, MIO_RAD1_WINDOW + reg);
}

static void rad1_write(QTestState *qts, uint32_t reg, uint32_t value)
{
    baseio_window_write(qts, MIO_RAD1_WINDOW + reg, value);
}

/*
 * The virtual time at which the previous register write landed. qtest
 * refuses a zero step, so step one nanosecond and subtract it.
 */
static int64_t rad1_clock_now(QTestState *qts)
{
    return qtest_clock_step(qts, 1) - 1;
}

/* Nanoseconds from a continuous start to completion k (1-based). */
static int64_t rad1_status_due(uint64_t k)
{
    return k * RAD1_STATUS_INTERVAL * 1000000000ull / RAD1_STATUS_CLOCK_HZ;
}

/* Step the clock to exactly t_start + offset, which must lie ahead. */
static void rad1_step_to(QTestState *qts, int64_t t_start, int64_t offset)
{
    int64_t now = qtest_clock_step(qts, 1);

    g_assert_cmpint(t_start + offset, >, now);
    qtest_clock_step(qts, t_start + offset - now);
}

/*
 * The continuous status-DMA engine. rad_start_status_dma writes the
 * ring-engine start word, with the ring bit and an interval field, to 0x5c;
 * the engine then keeps completing slots around the ring on its own, one
 * per interval. So, unlike the one-shot, no slot completes on the register
 * write itself. This checks, on a four-slot ring:
 *
 * - a start word with a zero interval runs nothing;
 * - with IRIX's word, completions land on successive slots, each reporting
 *   no ADAT, AES3 or video reference;
 * - they are one decoded interval apart, going by the 0x30 counter each
 *   slot mirrors;
 * - they stay on the absolute schedule start + k intervals. qtest fires
 *   timers exactly at their deadlines, so it cannot delay a callback; what
 *   it can show is that completion 100 lands at exactly start + 100
 *   intervals, not a nanosecond early, with no rounding carried from one
 *   re-arm to the next;
 * - a zero-interval start word stops a running engine;
 * - an interval under the model's 10 us floor runs at the floor;
 * - the stop word halts the engine.
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
    const uint64_t slot100 = ring_ram + (100 % nslots) * RAD1_SLOT_STRIDE;
    uint32_t ust1, ust2, spacing;
    int64_t t_start;
    unsigned int i;

    program_module(qts);
    program_rad1(qts);

    /* Bus mastering with Bridge swap on, as in the handshake test. */
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /*
     * Clear every slot's completion word and seed its flag word with the
     * input bits, so a completion that fails to clear them shows up.
     */
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_FLAGS_OFFSET,
                        RAD1_SLOT_INPUT_BITS);
    }

    rad1_write(qts, RAD1_REG_STATUS_BASE_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_REG_STATUS_BASE_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_REG_STATUS_LIMIT, limit);

    /* A zero interval names no period: nothing runs. */
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START);
    qtest_clock_step(qts, 4 * rad1_status_due(1));
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET) &
                        RAD1_SLOT_DONE_BIT, ==, 0);
    }

    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START_IRIX);
    t_start = rad1_clock_now(qts);
    g_assert_cmphex(rad1_read(qts, RAD1_REG_STATUS_DMA_CTRL),
                    ==, RAD1_STATUS_START_IRIX);

    /*
     * Three and a half intervals: slots 1, 2 and 3 are done, starting one
     * past the reset consumer index, and the base slot is not yet reached.
     */
    rad1_step_to(qts, t_start, (rad1_status_due(3) + rad1_status_due(4)) / 2);
    for (i = 1; i <= 3; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;
        uint32_t done = rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET);
        uint32_t flags = rad1_ring_read(qts, slot + RAD1_SLOT_FLAGS_OFFSET);

        /*
         * AES3 absent (bit 19 set), with no ADAT or video-field bits:
         * the model's "genlock absent" state. Setting 0x80/0x100 on every
         * completion would present a synthetic 60 Hz video reference.
         */
        g_assert_cmphex(done, ==, RAD1_SLOT_DONE_WORD);
        g_assert_cmphex(flags & RAD1_SLOT_INPUT_BITS, ==, 0);
    }
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);

    /* Slots 1 and 2 are one interval, about 250 us, apart. */
    ust1 = rad1_ring_read(qts, ring_ram + RAD1_SLOT_STRIDE +
                          RAD1_SLOT_MIRROR_OFFSET + RAD1_REG_UST_COUNTER);
    ust2 = rad1_ring_read(qts, ring_ram + 2 * RAD1_SLOT_STRIDE +
                          RAD1_SLOT_MIRROR_OFFSET + RAD1_REG_UST_COUNTER);
    spacing = RAD1_UST_TICKS(rad1_status_due(2) - rad1_status_due(1));
    g_assert_cmpuint(ust2 - ust1, >=, spacing - 1);
    g_assert_cmpuint(ust2 - ust1, <=, spacing + 1);

    /* Completion 100 is due at exactly start + 100 intervals. */
    rad1_step_to(qts, t_start, rad1_status_due(99) + 1);
    rad1_ring_write(qts, slot100 + RAD1_SLOT_DONE_OFFSET, 0);
    rad1_step_to(qts, t_start, rad1_status_due(100) - 1);
    g_assert_cmphex(rad1_ring_read(qts, slot100 + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rad1_ring_read(qts, slot100 + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);

    /* A zero-interval start stops the running schedule. */
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START);
    for (i = 0; i < nslots; i++) {
        rad1_ring_write(qts, ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE +
                        RAD1_SLOT_DONE_OFFSET, 0);
    }
    qtest_clock_step(qts, 4 * rad1_status_due(1));
    for (i = 0; i < nslots; i++) {
        g_assert_cmphex(rad1_ring_read(qts, ring_ram +
                                       (uint64_t)i * RAD1_SLOT_STRIDE +
                                       RAD1_SLOT_DONE_OFFSET) &
                        RAD1_SLOT_DONE_BIT, ==, 0);
    }

    /* An interval of one clock runs at the 10 us floor instead. */
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL,
               RAD1_STATUS_START | (1u << 2));
    t_start = rad1_clock_now(qts);
    rad1_step_to(qts, t_start, RAD1_STATUS_FLOOR_NS - 1);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_STRIDE +
                                   RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram + RAD1_SLOT_STRIDE +
                                   RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);

    /* Stop halts the engine: clear every slot, step, nothing completes. */
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
    }
    qtest_clock_step(qts, 3 * rad1_status_due(1));
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET) &
                        RAD1_SLOT_DONE_BIT, ==, 0);
    }
}

/* Frames the output engine should have moved t_ns after its enable edge. */
static uint64_t rad1_out_frames_due(int64_t t_ns)
{
    return (uint64_t)t_ns * RAD1_OUT_FREQ / 1000000000u;
}

/*
 * The addresses the output tests use in node-0 RAM, and the PCI forms the
 * device reaches them by through the Bridge's direct 64-bit window.
 */
#define RAD1_TEST_STATUS_RAM  (module_nasids[0] * NODE_STRIDE + 0x02000000)
#define RAD1_TEST_RING_RAM    (module_nasids[0] * NODE_STRIDE + 0x02100000)
#define RAD1_TEST_IN_RAM      (module_nasids[0] * NODE_STRIDE + 0x02200000)
#define RAD1_TEST_DIG_RAM     (module_nasids[0] * NODE_STRIDE + 0x02300000)
#define RAD1_TEST_DIG2_RAM    (module_nasids[0] * NODE_STRIDE + 0x02400000)
#define RAD1_TEST_PCI(ram)    (((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | (ram))

/*
 * Program an io6g machine the way IRIX leaves it before an sfplay start:
 * routing, RAD1's BAR and bus mastering, a ring whose every frame is the
 * word pair (@left, @right), and both register sets of the analog output.
 * The enable is left clear.
 */
static void rad1_prepare_output(QTestState *qts, uint32_t left,
                                uint32_t right)
{
    const uint64_t ring_pci = RAD1_TEST_PCI(RAD1_TEST_RING_RAM);
    unsigned int i;

    program_module(qts);
    program_rad1(qts);
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    for (i = 0; i < RAD1_OUT_RING_BYTES; i += RAD1_OUT_FRAME_BYTES) {
        rad1_ring_write(qts, RAD1_TEST_RING_RAM + i, left);
        rad1_ring_write(qts, RAD1_TEST_RING_RAM + i + 4, right);
    }

    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    rad1_write(qts, RAD1_OUT_A_ADDR_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_OUT_A_ADDR_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_OUT_A_CTRL, RAD1_OUT_CTRL_4K);
    rad1_write(qts, RAD1_OUT_B_ADDR_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_OUT_B_ADDR_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_OUT_B_CTRL, RAD1_OUT_CTRL_4K);
}

/*
 * Analog output streams continuously on type 1. IRIX sfplay programs A/B with
 * the same ring and control word 0xfffff008 (tag 1, 4 KiB), then sets
 * bit 0 of 0x58 on top of its bring-up value. From that edge, the
 * engine walks the ring at the sample rate with no re-arm. The status
 * slots report its
 * progress. The checks:
 *
 * - Neither a central-control 0x80 write nor the analog input's enable
 *   (0x4c bit 0) starts the output.
 * - 0x18 bits [17:4], type 1's B field, change inside the driver's 500 us
 *   poll window, while type 0's bits [31:18] stay put.
 * - 0x2c's frame field advances at the modeled 44.1 kHz and its low 18
 *   bits latch the 0x30 counter; a read catches the engine up first.
 * - Each status completion carries type 1's DMA pointer at +0x38 and the
 *   position registers mirrored at register + 0x40, consistent with each
 *   other: the pointer moves two ring words per frame counted, walks the
 *   ring and wraps inside it.
 * - Between consecutive completions the pointer moves 11 or 12 frames, 22
 *   or 24 words, far inside the kernel mixer's 0xcc-word refill lead.
 * - 0x14, type 0's A register, never moves.
 * - Clearing the enable stops the pointer and both registers.
 */
static void test_io6g_rad1_type1_output_stream(void)
{
    QTestState *qts = start_module_io6g();
    const uint64_t ring_ram = module_nasids[0] * NODE_STRIDE + 0x02000000;
    const uint64_t ring_pci =
        ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | ring_ram;
    const unsigned int nslots = 8;
    const uint32_t limit = RAD1_LIMIT_BASE - nslots * RAD1_SLOT_STRIDE;
    const uint64_t buf_ram = module_nasids[0] * NODE_STRIDE + 0x02100000;
    const uint64_t buf_pci = ((uint64_t)RAD1_LOCAL_HUB_WIDGET << 60) | buf_ram;
    const uint32_t buf_lo = (uint32_t)buf_pci;
    uint32_t a0, b0, a, b, ust, prev_ptr, ptr = 0, stop_ptr;
    uint64_t due, moved;
    int64_t t_status, t_enable, t;
    unsigned int i, wraps = 0;
    uint64_t k;

    program_module(qts);
    program_rad1(qts);
    baseio_window_write(qts, BRIDGE_CFG0(MIO_RAD1_DEVICE) + PCI_COMMAND,
                        PCI_COMMAND_MEMORY | RAD1_PCI_COMMAND_MASTER);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        BRIDGE_DEV_MEMORY_SPACE | (MIO_RAD1_MMIO >> 20));

    /* A known pattern in the output ring, and a cleared status ring. */
    for (i = 0; i < RAD1_OUT_RING_BYTES; i += 4) {
        rad1_ring_write(qts, buf_ram + i, 0x5a000000u | i);
    }
    for (i = 0; i < nslots; i++) {
        uint64_t slot = ring_ram + (uint64_t)i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_OUT_PTR, 0);
    }

    /* Bring-up values, then the sfplay start's two register sets. */
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);
    rad1_write(qts, RAD1_OUT_A_ADDR_LO, buf_lo);
    rad1_write(qts, RAD1_OUT_A_ADDR_HI, (uint32_t)(buf_pci >> 32));
    rad1_write(qts, RAD1_OUT_A_CTRL, RAD1_OUT_CTRL_4K);
    rad1_write(qts, RAD1_OUT_B_ADDR_LO, buf_lo);
    rad1_write(qts, RAD1_OUT_B_ADDR_HI, (uint32_t)(buf_pci >> 32));
    rad1_write(qts, RAD1_OUT_B_CTRL, RAD1_OUT_CTRL_4K);

    a0 = rad1_read(qts, RAD1_OUT_POS_A);
    b0 = rad1_read(qts, RAD1_OUT_POS_B);
    g_assert_cmphex(rad1_read(qts, RAD1_IN_POS_A), ==, 0);

    /* Negative controls: the old 0x80 kick and type 0's enable. */
    rad1_write(qts, RAD1_REG_CENTRAL_CTRL, RAD1_CENTRAL_ADAT_OFF);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_A), ==, a0);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_B), ==, b0);
    /*
     * With no input ring programmed this 0x4c enable starts nothing at
     * all, so it only shows the output ignores it. A running input's
     * effect on the output's registers is checked by
     * io6g-rad1-type0-input-stream.
     */
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_A), ==, a0);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_B), ==, b0);
    g_assert_cmphex(rad1_read(qts, RAD1_IN_POS_A), ==, 0);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);

    /* Start the status ring, then the output. */
    rad1_write(qts, RAD1_REG_STATUS_BASE_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_REG_STATUS_BASE_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_REG_STATUS_LIMIT, limit);
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START_IRIX);
    t_status = rad1_clock_now(qts);

    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_RUN);
    t_enable = rad1_clock_now(qts);
    g_assert_cmphex(rad1_read(qts, RAD1_REG_ANALOG_OUT_EN), ==, RAD1_OUT_RUN);

    /* B's type 1 field ticks inside rad_start_dma's 50 x 10 us poll. */
    qtest_clock_step(qts, 500000);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    g_assert_cmphex(RAD1_POS_B_TYPE1(b), !=, RAD1_POS_B_TYPE1(b0));
    g_assert_cmphex(RAD1_POS_B_TYPE0(b), ==, RAD1_POS_B_TYPE0(b0));

    /*
     * A read catches the engine up, so A's frame field matches the sample
     * clock exactly, and B's field counts the same frames.
     */
    t = qtest_clock_step(qts, 10000000);
    a = rad1_read(qts, RAD1_OUT_POS_A);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    ust = rad1_read(qts, RAD1_REG_UST_COUNTER);
    due = rad1_out_frames_due(t - t_enable);
    moved = (RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff;
    g_assert_cmpuint(moved, ==, due);
    g_assert_cmpuint((RAD1_POS_B_TYPE1(b) - RAD1_POS_B_TYPE1(b0)) & 0x3fff,
                     ==, moved);
    g_assert_cmphex(RAD1_POS_B_TYPE0(b), ==, RAD1_POS_B_TYPE0(b0));

    /* The latch is the 0x30 counter as of that catch-up, this instant. */
    g_assert_cmphex(RAD1_POS_A_LATCH(a), ==, RAD1_POS_A_LATCH(ust));

    /*
     * Walk the status slots one completion at a time, stepping to each
     * deadline. Completion k fills slot k mod nslots.
     */
    t = rad1_clock_now(qts);
    k = 1;
    while (t_status + rad1_status_due(k) <= t + 1) {
        k++;
    }
    prev_ptr = 0;
    for (i = 0; i < 120; i++, k++) {
        uint64_t slot = ring_ram + (k % nslots) * RAD1_SLOT_STRIDE;
        uint32_t m_a, m_b, frames;

        rad1_step_to(qts, t_status, rad1_status_due(k));

        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET),
                        ==, RAD1_SLOT_DONE_WORD);
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_FLAGS_OFFSET) &
                        RAD1_SLOT_INPUT_BITS, ==, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);

        ptr = rad1_ring_read(qts, slot + RAD1_SLOT_OUT_PTR);
        g_assert_cmphex(ptr, >=, buf_lo);
        g_assert_cmphex(ptr, <, buf_lo + RAD1_OUT_RING_BYTES);
        if (prev_ptr) {
            uint32_t words = ((ptr - prev_ptr) % RAD1_OUT_RING_BYTES) / 4;

            g_assert_cmpuint(words, >=, 22);
            g_assert_cmpuint(words, <=, 24);
            g_assert_cmpuint(words, <, RAD1_OUT_REFILL_LEAD_WORDS);
            if (ptr < prev_ptr) {
                wraps++;
            }
        }
        prev_ptr = ptr;

        /* The mirror agrees with the pointer and with itself. */
        m_a = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                             RAD1_OUT_POS_A);
        m_b = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                             RAD1_OUT_POS_B);
        frames = (RAD1_POS_A_FRAMES(m_a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff;
        g_assert_cmphex(ptr - buf_lo, ==,
                        frames * RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);
        g_assert_cmpuint((RAD1_POS_B_TYPE1(m_b) - RAD1_POS_B_TYPE1(b0)) &
                         0x3fff, ==, frames);
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                                       RAD1_IN_POS_A), ==, 0);
    }
    /* 30 ms against an 11.6 ms ring (512 frames): at least two laps. */
    g_assert_cmpuint(wraps, >=, 2);
    g_assert_cmphex(rad1_read(qts, RAD1_IN_POS_A), ==, 0);

    /*
     * Clearing the enable stops the registers and the pointer, midway
     * between two completions.
     */
    rad1_step_to(qts, t_status,
                 (rad1_status_due(k - 1) + rad1_status_due(k)) / 2);
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    a = rad1_read(qts, RAD1_OUT_POS_A);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    rad1_step_to(qts, t_status, rad1_status_due(k));
    stop_ptr = rad1_ring_read(qts, ring_ram + (k % nslots) * RAD1_SLOT_STRIDE +
                              RAD1_SLOT_OUT_PTR);
    g_assert_cmphex(stop_ptr - buf_lo, ==,
                    ((RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff) *
                    RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);
    k += 3;
    rad1_step_to(qts, t_status, rad1_status_due(k));
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_A), ==, a);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_B), ==, b);
    g_assert_cmphex(rad1_ring_read(qts, ring_ram +
                                   (k % nslots) * RAD1_SLOT_STRIDE +
                                   RAD1_SLOT_OUT_PTR), ==, stop_ptr);
    g_assert_cmphex(rad1_read(qts, RAD1_IN_POS_A), ==, 0);

    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
}

/*
 * One frame of the sample-readback tests. Bits [23:8] of each word, what
 * the host voice takes, differ from bits [15:0], so taking the wrong bits
 * shows; the two channels differ, so swapping them shows; the right word
 * is negative, so the sign extension through [31:24] is exercised.
 */
#define RAD1_SAMPLE_LEFT_WORD     0x00123456u
#define RAD1_SAMPLE_RIGHT_WORD    0xffabcd12u
#define RAD1_SAMPLE_LEFT          0x1234
#define RAD1_SAMPLE_RIGHT         ((int16_t)0xabcd)
/* The canonical 44-byte header of QEMU's wav backend. */
#define RAD1_WAV_HEADER_BYTES     44

/*
 * Bind the RAD1 voice to a wav audiodev at @path, at the voice's own
 * format so the mixer does not resample.
 */
static char *rad1_wav_args(const char *path)
{
    g_autofree char *quoted = g_shell_quote(path);

    return g_strdup_printf("-audiodev wav,id=rad1wav,path=%s,"
                           "out.frequency=%u,out.channels=2,out.format=s16 "
                           "-global sgi-rad1-audio.audiodev=rad1wav",
                           quoted, RAD1_OUT_FREQ);
}

/*
 * Count the frames of a wav capture that carry the seeded samples, and
 * assert every other frame is silence. The backend's header lengths are
 * only written when its voice closes, which a quit never does, so the
 * payload is read from the fixed header size to the end of the file.
 */
static unsigned int rad1_wav_seeded_frames(const char *path)
{
    g_autofree gchar *wav = NULL;
    gsize len;
    unsigned int seeded = 0;
    gsize off;

    g_assert_true(g_file_get_contents(path, &wav, &len, NULL));
    g_assert_cmpuint(len, >, RAD1_WAV_HEADER_BYTES);
    g_assert_cmpmem(wav, 4, "RIFF", 4);

    for (off = RAD1_WAV_HEADER_BYTES; off + 4 <= len; off += 4) {
        int16_t left = (int16_t)lduw_le_p(wav + off);
        int16_t right = (int16_t)lduw_le_p(wav + off + 2);

        if (left == RAD1_SAMPLE_LEFT && right == RAD1_SAMPLE_RIGHT) {
            seeded++;
        } else {
            g_assert_cmpint(left, ==, 0);
            g_assert_cmpint(right, ==, 0);
        }
    }
    return seeded;
}

/*
 * The samples themselves, read back from the host voice through a wav
 * audiodev. Every ring frame is the pair (0x00123456, 0xffabcd12), so a
 * correct engine plays (0x1234, -0x5433) in left, right order, the model's
 * flagged left-first default, taking bits [23:8] of each word. 50 ms at 44.1
 * kHz is 2205 frames; the wav backend drains the voice on its own
 * virtual-clock timer, so allow for frames still buffered when the
 * machine quits.
 *
 * This needs its own machine for the audiodev and quits it to flush
 * the file, unlike the shared io6g machine the other cases reset.
 */
static void test_io6g_rad1_output_samples(void)
{
    g_autofree char *path = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sn0-baseio-rad1-XXXXXX.wav", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = rad1_wav_args(path);

    qts = start_module_with(",baseio-preset=io6g", extra);
    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_RUN);
    qtest_clock_step(qts, 50000000);
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    qtest_quit(qts);

    g_assert_cmpuint(rad1_wav_seeded_frames(path), >=, 1500);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/*
 * Save and restore with both engines running. The source streams for a
 * while and migrates to a destination bound to a wav audiodev. qtest's
 * clock is not part of the migration stream, so the destination's is
 * first set to the source's. Then, on the destination:
 *
 * - the enable and both position registers read as they did on the
 *   source, and keep counting frames at 44.1 kHz from the enable edge;
 * - the next status completion lands at the source's schedule, not a
 *   nanosecond early, and carries the pointer that frame count implies;
 * - the voice is active again, so the capture holds the seeded samples.
 */
static void test_io6g_rad1_output_migration(void)
{
    const uint64_t status_pci = RAD1_TEST_PCI(RAD1_TEST_STATUS_RAM);
    const uint32_t ring_lo = (uint32_t)RAD1_TEST_PCI(RAD1_TEST_RING_RAM);
    const unsigned int nslots = 8;
    const uint32_t limit = RAD1_LIMIT_BASE - nslots * RAD1_SLOT_STRIDE;
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *wav = NULL;
    g_autofree char *extra = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t a, b, a0, m_a, ptr;
    int64_t t_status, t_enable, t_src;
    uint64_t k, frames, slot;

    dir = g_dir_make_tmp("sn0-baseio-rad1-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    wav = g_strdup_printf("%s/dest.wav", dir);
    extra = rad1_wav_args(wav);

    from = start_module_with(",baseio-preset=io6g", "");
    rad1_prepare_output(from, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    a0 = rad1_read(from, RAD1_OUT_POS_A);
    rad1_write(from, RAD1_REG_STATUS_BASE_LO, (uint32_t)status_pci);
    rad1_write(from, RAD1_REG_STATUS_BASE_HI, (uint32_t)(status_pci >> 32));
    rad1_write(from, RAD1_REG_STATUS_LIMIT, limit);
    rad1_write(from, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START_IRIX);
    t_status = rad1_clock_now(from);
    rad1_write(from, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_RUN);
    t_enable = rad1_clock_now(from);

    /* Stream past a ring lap and stop between two completions. */
    rad1_step_to(from, t_status,
                 (rad1_status_due(60) + rad1_status_due(61)) / 2);
    t_src = rad1_clock_now(from);
    a = rad1_read(from, RAD1_OUT_POS_A);
    b = rad1_read(from, RAD1_OUT_POS_B);
    g_assert_cmpuint((RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff,
                     ==, rad1_out_frames_due(t_src - t_enable));

    to = qtest_initf("-M origin2000,baseio-preset=io6g -smp 8 -m 256M "
                     "-d unimp,guest_errors %s -incoming %s", extra, uri);
    sgi_migrate_stopped(from, uri);
    qtest_clock_set(to, t_src);

    g_assert_cmphex(rad1_read(to, RAD1_REG_ANALOG_OUT_EN), ==, RAD1_OUT_RUN);
    g_assert_cmphex(rad1_read(to, RAD1_OUT_POS_A), ==, a);
    g_assert_cmphex(rad1_read(to, RAD1_OUT_POS_B), ==, b);

    /* Completion 61 lands on the source's schedule. */
    k = 61;
    slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
    rad1_ring_write(to, slot + RAD1_SLOT_DONE_OFFSET, 0);
    rad1_step_to(to, t_status, rad1_status_due(k) - 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);

    /* It carries the frame count and pointer of an uninterrupted stream. */
    frames = rad1_out_frames_due(t_status + rad1_status_due(k) - t_enable);
    m_a = rad1_ring_read(to, slot + RAD1_SLOT_MIRROR_OFFSET + RAD1_OUT_POS_A);
    ptr = rad1_ring_read(to, slot + RAD1_SLOT_OUT_PTR);
    g_assert_cmpuint((RAD1_POS_A_FRAMES(m_a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff,
                     ==, frames & 0x3fff);
    g_assert_cmphex(ptr - ring_lo, ==,
                    frames * RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);

    /* The destination's voice plays the ring. */
    qtest_clock_step(to, 50000000);
    rad1_write(to, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpuint(rad1_wav_seeded_frames(wav), >=, 1500);

    g_assert_cmpint(g_unlink(wav), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/*
 * Fill the input ring with a poison word no sample can equal, and program
 * both of the input's register sets for it, as rad_start_dma does. The
 * enable is left alone.
 */
static void rad1_prepare_input(QTestState *qts)
{
    const uint64_t ring_pci = RAD1_TEST_PCI(RAD1_TEST_IN_RAM);
    unsigned int i;

    for (i = 0; i < RAD1_OUT_RING_BYTES; i += 4) {
        rad1_ring_write(qts, RAD1_TEST_IN_RAM + i, RAD1_IN_POISON);
    }
    rad1_write(qts, RAD1_IN_A_ADDR_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_IN_A_ADDR_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_IN_A_CTRL, RAD1_IN_CTRL_4K);
    rad1_write(qts, RAD1_IN_B_ADDR_LO, (uint32_t)ring_pci);
    rad1_write(qts, RAD1_IN_B_ADDR_HI, (uint32_t)(ring_pci >> 32));
    rad1_write(qts, RAD1_IN_B_CTRL, RAD1_IN_CTRL_4K);
}

/*
 * Whether a ring word has the input's layout: a 16-bit sample in bits
 * [23:8], sign-extended through [31:24], bits [7:0] zero. The poison word
 * fails both tests.
 */
static bool rad1_is_input_word(uint32_t w)
{
    return (w & 0xff) == 0 && (uint32_t)((int32_t)(w << 8) >> 8) == w;
}

/* Start the status ring at RAD1_TEST_STATUS_RAM with IRIX's start word. */
static int64_t rad1_start_status(QTestState *qts, unsigned int nslots)
{
    const uint64_t status_pci = RAD1_TEST_PCI(RAD1_TEST_STATUS_RAM);
    unsigned int i;

    for (i = 0; i < nslots; i++) {
        rad1_ring_write(qts, RAD1_TEST_STATUS_RAM + i * RAD1_SLOT_STRIDE +
                        RAD1_SLOT_DONE_OFFSET, 0);
    }
    rad1_write(qts, RAD1_REG_STATUS_BASE_LO, (uint32_t)status_pci);
    rad1_write(qts, RAD1_REG_STATUS_BASE_HI, (uint32_t)(status_pci >> 32));
    rad1_write(qts, RAD1_REG_STATUS_LIMIT,
               RAD1_LIMIT_BASE - nslots * RAD1_SLOT_STRIDE);
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_START_IRIX);
    return rad1_clock_now(qts);
}

/*
 * The analog input's continuous streaming engine, channel type 0. It
 * shares the output's engine, with the direction reversed: from the 0x4c
 * bit 0 edge it writes frames into the ring at the sample rate and wraps.
 * The checks:
 *
 * - Neither the output's enable (0x58) nor 0x4c's other bits, the bring-up
 *   value and the mux clock at bit 27, start the input; pulsing bit 27
 *   while bit 0 stays set does not restart it.
 * - 0x14's frame field and 0x18's bits [31:18] advance at 44.1 kHz, and
 *   0x14's low bits latch the 0x30 counter. The output's registers, 0x2c
 *   and 0x18 bits [17:4], set to nonzero values by a short output run
 *   first, stay exactly as they were.
 * - The ring is written from its start, one frame per sample period, every
 *   word in the input layout, and nothing past the frames due is touched.
 *   Which samples arrive depends on the host backend, so this checks the
 *   layout, not the content; the migration case below pins silence to
 *   zero words.
 * - Each status completion carries the input's pointer at +0x10, agreeing
 *   with the 0x14 mirror; it walks the ring in 22 to 24 words per
 *   completion and wraps.
 * - Clearing bit 0 stops the pointer, both registers and the ring writes.
 */
static void test_io6g_rad1_type0_input_stream(void)
{
    QTestState *qts = start_module_io6g();
    const unsigned int nslots = 8;
    const uint32_t in_lo = (uint32_t)RAD1_TEST_PCI(RAD1_TEST_IN_RAM);
    uint32_t a0, b1, a_out, a, b, ust, prev_ptr, ptr, stop_ptr, stop_off;
    uint64_t due, moved, k;
    int64_t t_status, t_enable, t;
    unsigned int i, wraps = 0;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_input(qts);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);

    /*
     * Negative controls. The output's enable runs the output, and a 0x4c
     * write with the mux clock but not bit 0 runs nothing; neither touches
     * the input's registers or ring. The output run also leaves nonzero
     * values in 0x2c and 0x18 bits [17:4] for the input to leave alone.
     */
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_RUN);
    qtest_clock_step(qts, 1000000);
    rad1_write(qts, RAD1_REG_ANALOG_OUT_EN, RAD1_OUT_IDLE);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK,
               RAD1_IN_ENABLE_IDLE | RAD1_IN_MUX_CLOCK);
    qtest_clock_step(qts, 2000000);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);

    a0 = rad1_read(qts, RAD1_IN_POS_A);
    a_out = rad1_read(qts, RAD1_OUT_POS_A);
    b1 = rad1_read(qts, RAD1_OUT_POS_B);
    g_assert_cmphex(a0, ==, 0);
    g_assert_cmphex(RAD1_POS_B_TYPE0(b1), ==, 0);
    g_assert_cmphex(RAD1_POS_B_TYPE1(b1), !=, 0);
    g_assert_cmphex(RAD1_POS_A_FRAMES(a_out), !=, 0);
    for (i = 0; i < RAD1_OUT_RING_BYTES; i += 4) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_IN_RAM + i),
                        ==, RAD1_IN_POISON);
    }

    t_status = rad1_start_status(qts, nslots);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);
    t_enable = rad1_clock_now(qts);

    /* A mux clock pulse with bit 0 held does not restart the input. */
    qtest_clock_step(qts, 100000);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK,
               RAD1_IN_ENABLE_RUN | RAD1_IN_MUX_CLOCK);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);

    /* B's type 0 field ticks inside rad_start_dma's 50 x 10 us poll. */
    rad1_step_to(qts, t_enable, 500000);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    g_assert_cmphex(RAD1_POS_B_TYPE0(b), !=, 0);
    g_assert_cmphex(RAD1_POS_B_TYPE1(b), ==, RAD1_POS_B_TYPE1(b1));

    /* 10 ms after the edge: every register exactly where it should be. */
    rad1_step_to(qts, t_enable, 10000000);
    t = rad1_clock_now(qts);
    a = rad1_read(qts, RAD1_IN_POS_A);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    ust = rad1_read(qts, RAD1_REG_UST_COUNTER);
    due = rad1_out_frames_due(t - t_enable);
    moved = (RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff;
    g_assert_cmpuint(moved, ==, due);
    g_assert_cmpuint(RAD1_POS_B_TYPE0(b), ==, due & 0x3fff);
    g_assert_cmphex(RAD1_POS_B_TYPE1(b), ==, RAD1_POS_B_TYPE1(b1));
    g_assert_cmphex(RAD1_POS_A_LATCH(a), ==, RAD1_POS_A_LATCH(ust));
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_A), ==, a_out);

    /* 441 frames written from the ring's start, nothing beyond. */
    g_assert_cmpuint(due, <, RAD1_OUT_RING_BYTES / RAD1_OUT_FRAME_BYTES);
    for (i = 0; i < due * 2; i++) {
        uint32_t w = rad1_ring_read(qts, RAD1_TEST_IN_RAM + i * 4);

        g_assert_true(rad1_is_input_word(w));
    }
    for (; i < RAD1_OUT_RING_BYTES / 4; i++) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_IN_RAM + i * 4),
                        ==, RAD1_IN_POISON);
    }

    /* Walk the status slots one completion at a time. */
    k = 1;
    while (t_status + rad1_status_due(k) <= t + 1) {
        k++;
    }
    prev_ptr = 0;
    for (i = 0; i < 120; i++, k++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
        uint32_t m_a, m_b, frames;

        rad1_step_to(qts, t_status, rad1_status_due(k));
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET),
                        ==, RAD1_SLOT_DONE_WORD);
        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);

        ptr = rad1_ring_read(qts, slot + RAD1_SLOT_IN_PTR);
        g_assert_cmphex(ptr, >=, in_lo);
        g_assert_cmphex(ptr, <, in_lo + RAD1_OUT_RING_BYTES);
        if (prev_ptr) {
            uint32_t words = ((ptr - prev_ptr) % RAD1_OUT_RING_BYTES) / 4;

            g_assert_cmpuint(words, >=, 22);
            g_assert_cmpuint(words, <=, 24);
            if (ptr < prev_ptr) {
                wraps++;
            }
        }
        prev_ptr = ptr;

        m_a = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                             RAD1_IN_POS_A);
        m_b = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                             RAD1_OUT_POS_B);
        frames = (RAD1_POS_A_FRAMES(m_a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff;
        g_assert_cmphex(ptr - in_lo, ==,
                        frames * RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);
        g_assert_cmpuint(RAD1_POS_B_TYPE0(m_b), ==, frames);
        g_assert_cmphex(RAD1_POS_B_TYPE1(m_b), ==, RAD1_POS_B_TYPE1(b1));
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET +
                                       RAD1_OUT_POS_A), ==, a_out);
    }
    g_assert_cmpuint(wraps, >=, 2);

    /* Clearing bit 0 stops the registers, the pointer and the writes. */
    rad1_step_to(qts, t_status,
                 (rad1_status_due(k - 1) + rad1_status_due(k)) / 2);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);
    a = rad1_read(qts, RAD1_IN_POS_A);
    b = rad1_read(qts, RAD1_OUT_POS_B);
    rad1_step_to(qts, t_status, rad1_status_due(k));
    stop_ptr = rad1_ring_read(qts, RAD1_TEST_STATUS_RAM +
                              (k % nslots) * RAD1_SLOT_STRIDE +
                              RAD1_SLOT_IN_PTR);
    stop_off = stop_ptr - in_lo;
    g_assert_cmphex(stop_off, ==,
                    ((RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff) *
                    RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);
    for (i = 0; i < 16; i++) {
        rad1_ring_write(qts, RAD1_TEST_IN_RAM +
                        (stop_off + i * 4) % RAD1_OUT_RING_BYTES,
                        RAD1_IN_POISON);
    }
    k += 3;
    rad1_step_to(qts, t_status, rad1_status_due(k));
    g_assert_cmphex(rad1_read(qts, RAD1_IN_POS_A), ==, a);
    g_assert_cmphex(rad1_read(qts, RAD1_OUT_POS_B), ==, b);
    g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_STATUS_RAM +
                                   (k % nslots) * RAD1_SLOT_STRIDE +
                                   RAD1_SLOT_IN_PTR), ==, stop_ptr);
    for (i = 0; i < 16; i++) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_IN_RAM +
                                       (stop_off + i * 4) %
                                       RAD1_OUT_RING_BYTES),
                        ==, RAD1_IN_POISON);
    }

    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
}

/* Assert every word of the input ring is zero: silence, in the layout. */
static void rad1_assert_input_ring_silent(QTestState *qts)
{
    unsigned int i;

    for (i = 0; i < RAD1_OUT_RING_BYTES; i += 4) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_IN_RAM + i), ==, 0);
    }
}

/*
 * Save and restore with the input running, both machines bound to the
 * none audiodev, whose input voice delivers silence. That pins content
 * where the backend is known: once the ring has lapped, every word of it
 * is zero. On the destination, with its clock set to the source's:
 *
 * - 0x4c and both position registers read as they did on the source;
 * - the next status completion lands on the source's schedule and carries
 *   the +0x10 pointer and 0x14 mirror of an uninterrupted stream;
 * - the engine keeps writing: a re-poisoned ring is silent again a lap
 *   later.
 */
static void test_io6g_rad1_input_migration(void)
{
    const char *none = "-audiodev none,id=rad1none "
                       "-global sgi-rad1-audio.audiodev=rad1none";
    const uint32_t in_lo = (uint32_t)RAD1_TEST_PCI(RAD1_TEST_IN_RAM);
    const unsigned int nslots = 8;
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t a, b, a0, m_a, ptr;
    int64_t t_status, t_enable, t_src;
    uint64_t k, frames, slot;
    unsigned int i;

    dir = g_dir_make_tmp("sn0-baseio-rad1-input-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = start_module_with(",baseio-preset=io6g", none);
    rad1_prepare_output(from, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_input(from);
    a0 = rad1_read(from, RAD1_IN_POS_A);
    t_status = rad1_start_status(from, nslots);
    rad1_write(from, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);
    t_enable = rad1_clock_now(from);

    /* 15 ms is past a 11.6 ms lap: the whole ring now holds silence. */
    rad1_step_to(from, t_status,
                 (rad1_status_due(60) + rad1_status_due(61)) / 2);
    rad1_assert_input_ring_silent(from);
    t_src = rad1_clock_now(from);
    a = rad1_read(from, RAD1_IN_POS_A);
    b = rad1_read(from, RAD1_OUT_POS_B);
    g_assert_cmpuint((RAD1_POS_A_FRAMES(a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff,
                     ==, rad1_out_frames_due(t_src - t_enable));

    to = qtest_initf("-M origin2000,baseio-preset=io6g -smp 8 -m 256M "
                     "-d unimp,guest_errors %s -incoming %s", none, uri);
    sgi_migrate_stopped(from, uri);
    qtest_clock_set(to, t_src);

    g_assert_cmphex(rad1_read(to, RAD1_REG_SHIFT_LOAD_CLOCK),
                    ==, RAD1_IN_ENABLE_RUN);
    g_assert_cmphex(rad1_read(to, RAD1_IN_POS_A), ==, a);
    g_assert_cmphex(rad1_read(to, RAD1_OUT_POS_B), ==, b);

    for (i = 0; i < RAD1_OUT_RING_BYTES; i += 4) {
        rad1_ring_write(to, RAD1_TEST_IN_RAM + i, RAD1_IN_POISON);
    }

    /* Completion 61 lands on the source's schedule. */
    k = 61;
    slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
    rad1_ring_write(to, slot + RAD1_SLOT_DONE_OFFSET, 0);
    rad1_step_to(to, t_status, rad1_status_due(k) - 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);

    frames = rad1_out_frames_due(t_status + rad1_status_due(k) - t_enable);
    m_a = rad1_ring_read(to, slot + RAD1_SLOT_MIRROR_OFFSET + RAD1_IN_POS_A);
    ptr = rad1_ring_read(to, slot + RAD1_SLOT_IN_PTR);
    g_assert_cmpuint((RAD1_POS_A_FRAMES(m_a) - RAD1_POS_A_FRAMES(a0)) & 0x3fff,
                     ==, frames & 0x3fff);
    g_assert_cmphex(ptr - in_lo, ==,
                    frames * RAD1_OUT_FRAME_BYTES % RAD1_OUT_RING_BYTES);

    /* A lap later the destination has written silence over the poison. */
    qtest_clock_step(to, 15000000);
    rad1_assert_input_ring_silent(to);

    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/*
 * The input with no host input voice: a machine bound to the wav audiodev,
 * which has none. The engine still runs on its own clock and fills the
 * ring with silence, so the positions advance at 44.1 kHz and every word
 * of a lapped ring is zero.
 */
static void test_io6g_rad1_input_without_host_input(void)
{
    g_autofree char *path = NULL;
    g_autofree char *extra = NULL;
    QTestState *qts;
    uint32_t a;
    int64_t t_enable;
    int fd;

    fd = g_file_open_tmp("sn0-baseio-rad1-XXXXXX.wav", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = rad1_wav_args(path);

    qts = start_module_with(",baseio-preset=io6g", extra);
    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_input(qts);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);
    t_enable = rad1_clock_now(qts);

    rad1_step_to(qts, t_enable, 15000000);
    a = rad1_read(qts, RAD1_IN_POS_A);
    g_assert_cmpuint(RAD1_POS_A_FRAMES(a), ==,
                     rad1_out_frames_due(15000000));
    rad1_assert_input_ring_silent(qts);

    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/*
 * Digital channels, types 2 to 5, AES3 and ADAT. Each
 * enable is bit 0 of a register whose bring-up value has it clear; each reads
 * A and B at shift 18. The frame widths (two words for AES3, eight for ADAT)
 * and the rates are the model's flagged defaults: the outputs at the analog
 * output's 44.1 kHz, the ADAT input at its clock's fixed 48 kHz, the AES3
 * input at the connected source's rate or 48 kHz with none. The control word
 * carries the type as its tag and the ring as a negated count of 128-byte
 * units: 4 KiB for AES3, 16 KiB for ADAT.
 */
typedef struct Rad1DigitalChannel {
    const char *name;
    bool output;
    uint32_t enable_reg;
    uint32_t enable_idle;
    uint32_t a_lo, a_hi, a_ctrl;
    uint32_t b_lo, b_hi, b_ctrl;
    uint32_t ctrl;
    uint32_t ring_bytes;
    uint32_t pos_a, pos_b;
    uint32_t slot_ptr;
    unsigned int words;
    uint32_t freq;
} Rad1DigitalChannel;

static const Rad1DigitalChannel rad1_aes_in = {
    "AES3 input", false, 0x48, 0x10,
    0x4c8, 0x50c, 0x4cc, 0x418, 0x41c, 0x420,
    0xfffff010u, 4096, 0x0c, 0x10, 0x08, 2, 48000,
};
static const Rad1DigitalChannel rad1_aes_out = {
    "AES3 output", true, 0x54, 0x40000000u,
    0x4f0, 0x520, 0x4f4, 0x424, 0x428, 0x42c,
    0xfffff018u, 4096, 0x24, 0x28, 0x30, 2, 44100,
};
static const Rad1DigitalChannel rad1_adat_in = {
    "ADAT input", false, 0x44, 0xa0000008u,
    0x4c0, 0x508, 0x4c4, 0x430, 0x434, 0x438,
    0xffffc020u, 16384, 0x04, 0x08, 0x00, 8, 48000,
};
static const Rad1DigitalChannel rad1_adat_out = {
    "ADAT output", true, 0x50, 0x60000000u,
    0x4e8, 0x51c, 0x4ec, 0x43c, 0x440, 0x444,
    0xffffc028u, 16384, 0x1c, 0x20, 0x28, 8, 44100,
};

#define RAD1_POS_B_LOW(v)         ((v) & 0x3ffffu)
#define RAD1_AES_LOCK_48000       0x81000000u
#define RAD1_AES_LOCK_44100       0x82000000u
#define RAD1_AES_LOCK_32000       0x83000000u
#define RAD1_SLOT_ADAT_PRESENT    0x40u
#define RAD1_SUBCODE_AES          0x1100
#define RAD1_SUBCODE_AES_LAST     0x1218
#define RAD1_SUBCODE_ADAT         0x1000

static uint64_t rad1_frames_due(int64_t t_ns, uint32_t freq)
{
    return (uint64_t)t_ns * freq / 1000000000u;
}

/*
 * Program a digital channel's two register sets for a ring at @ram, and
 * fill the ring: an output's every frame is (left, right, then distinct
 * words for any further channels); an input's every word is poison.
 */
static void rad1_prepare_digital(QTestState *qts, const Rad1DigitalChannel *d,
                                 uint64_t ram)
{
    const uint64_t pci = RAD1_TEST_PCI(ram);
    unsigned int i;

    for (i = 0; i < d->ring_bytes / 4; i++) {
        unsigned int w = i % d->words;
        uint32_t v = !d->output ? RAD1_IN_POISON :
                     w == 0 ? RAD1_SAMPLE_LEFT_WORD :
                     w == 1 ? RAD1_SAMPLE_RIGHT_WORD : 0x00770000u + w;

        rad1_ring_write(qts, ram + i * 4, v);
    }
    rad1_write(qts, d->enable_reg, d->enable_idle);
    rad1_write(qts, d->a_lo, (uint32_t)pci);
    rad1_write(qts, d->a_hi, (uint32_t)(pci >> 32));
    rad1_write(qts, d->a_ctrl, d->ctrl);
    rad1_write(qts, d->b_lo, (uint32_t)pci);
    rad1_write(qts, d->b_hi, (uint32_t)(pci >> 32));
    rad1_write(qts, d->b_ctrl, d->ctrl);
}

/* A doubleword store and load through the BaseIO window. */
static void rad1_write64(QTestState *qts, uint32_t reg, uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, XTALK_BASE +
                                     module_nasids[0] * NODE_STRIDE +
                                     BASEIO_WIDGET * XTALK_WIDGET_SIZE +
                                     MIO_RAD1_WINDOW + reg,
                                     bytes, sizeof(bytes)));
}

static uint64_t rad1_read64(QTestState *qts, uint32_t reg)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 0, XTALK_BASE +
                                    module_nasids[0] * NODE_STRIDE +
                                    BASEIO_WIDGET * XTALK_WIDGET_SIZE +
                                    MIO_RAD1_WINDOW + reg,
                                    bytes, sizeof(bytes)));
    return ldq_be_p(bytes);
}

/* Assert the channel's A and B count @due frames and A latches 0x30. */
static void rad1_check_digital_pos(QTestState *qts,
                                   const Rad1DigitalChannel *d, uint64_t due)
{
    uint32_t a = rad1_read(qts, d->pos_a);
    uint32_t b = rad1_read(qts, d->pos_b);
    uint32_t ust = rad1_read(qts, RAD1_REG_UST_COUNTER);

    g_assert_cmpuint(RAD1_POS_A_FRAMES(a), ==, due & 0x3fff);
    g_assert_cmpuint(RAD1_POS_A_FRAMES(b), ==, due & 0x3fff);
    g_assert_cmphex(RAD1_POS_A_LATCH(a), ==, RAD1_POS_A_LATCH(ust));
    /* B's low bits belong to subcode or another channel: untouched. */
    g_assert_cmphex(RAD1_POS_B_LOW(b), ==, 0);
}

/*
 * Stream one digital channel on the shared machine and check what the
 * driver sees, as for the analog pair:
 *
 * - a write of the enable register's bits that start nothing, starts
 *   nothing: bit 3 on all four, and bit 2 on the outputs' 0x50 and 0x54,
 *   which have no subcode DMA channel (bit 2 of 0x44 and 0x48 is the
 *   input subcode channels', checked by rad1_check_subcode_stream);
 * - B ticks inside the 500 us poll;
 * - A and B count frames at the channel's rate, A latches 0x30, and B's
 *   low 18 bits, which hold subcode fields or another channel's count,
 *   stay zero;
 * - an input writes silence (no digital audiodev is bound, so no host
 *   voice) from the ring's start, and nothing past the frames due;
 * - each status completion carries the channel's pointer at its slot
 *   word, agreeing with A's mirror, and it wraps; the analog pair's
 *   pointer words stay untouched;
 * - @disturb, if given, runs while the channel streams and must not move
 *   it off the sample clock;
 * - clearing bit 0 stops the registers and the pointer.
 */
static void rad1_check_digital_stream(const Rad1DigitalChannel *d,
                                      void (*disturb)(QTestState *))
{
    QTestState *qts = start_module_io6g();
    const unsigned int nslots = 8;
    const uint32_t ring_lo = (uint32_t)RAD1_TEST_PCI(RAD1_TEST_DIG_RAM);
    const uint32_t frame_bytes = d->words * 4;
    const uint64_t per_lo = (uint64_t)rad1_status_due(1) * d->freq /
                            1000000000u;
    uint32_t a, b, ptr, prev_ptr = 0, stop_ptr;
    int64_t t_status, t_enable, t;
    uint64_t due, k;
    unsigned int i, wraps = 0;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_digital(qts, d, RAD1_TEST_DIG_RAM);
    for (i = 0; i < nslots; i++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + i * RAD1_SLOT_STRIDE;

        rad1_ring_write(qts, slot + RAD1_SLOT_IN_PTR, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_OUT_PTR, 0);
    }

    /* Bit 3 starts nothing, nor does bit 2 on an output. */
    rad1_write(qts, d->enable_reg,
               d->enable_idle | (d->output ? 0xcu : 0x8u));
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rad1_read(qts, d->pos_a), ==, 0);
    g_assert_cmphex(rad1_read(qts, d->pos_b), ==, 0);
    rad1_write(qts, d->enable_reg, d->enable_idle);

    t_status = rad1_start_status(qts, nslots);
    rad1_write(qts, d->enable_reg, d->enable_idle | 1);
    t_enable = rad1_clock_now(qts);

    rad1_step_to(qts, t_enable, 500000);
    b = rad1_read(qts, d->pos_b);
    g_assert_cmphex(RAD1_POS_A_FRAMES(b), !=, 0);

    rad1_step_to(qts, t_enable, 10000000);
    due = rad1_frames_due(10000000, d->freq);
    rad1_check_digital_pos(qts, d, due);

    if (!d->output) {
        g_assert_cmpuint(due * frame_bytes, <, d->ring_bytes);
        for (i = 0; i < due * d->words; i++) {
            g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG_RAM + i * 4),
                            ==, 0);
        }
        for (; i < d->ring_bytes / 4; i++) {
            g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG_RAM + i * 4),
                            ==, RAD1_IN_POISON);
        }
    }

    if (disturb) {
        disturb(qts);
        rad1_step_to(qts, t_enable, 12000000);
        rad1_check_digital_pos(qts, d, rad1_frames_due(12000000, d->freq));
    }

    t = rad1_clock_now(qts);
    k = 1;
    while (t_status + rad1_status_due(k) <= t + 1) {
        k++;
    }
    for (i = 0; i < 100; i++, k++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
        uint32_t m_a, frames;

        rad1_step_to(qts, t_status, rad1_status_due(k));
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET),
                        ==, RAD1_SLOT_DONE_WORD);
        ptr = rad1_ring_read(qts, slot + d->slot_ptr);
        m_a = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET + d->pos_a);
        frames = RAD1_POS_A_FRAMES(m_a);
        g_assert_cmphex(ptr, >=, ring_lo);
        g_assert_cmphex(ptr, <, ring_lo + d->ring_bytes);
        g_assert_cmphex(ptr - ring_lo, ==,
                        frames * frame_bytes % d->ring_bytes);
        if (prev_ptr) {
            uint32_t moved = ((ptr - prev_ptr) % d->ring_bytes) / frame_bytes;

            g_assert_cmpuint(moved, >=, per_lo);
            g_assert_cmpuint(moved, <=, per_lo + 1);
            if (ptr < prev_ptr) {
                wraps++;
            }
        }
        prev_ptr = ptr;
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_IN_PTR), ==, 0);
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_OUT_PTR), ==, 0);
    }
    g_assert_cmpuint(wraps, >=, 2);

    rad1_step_to(qts, t_status,
                 (rad1_status_due(k - 1) + rad1_status_due(k)) / 2);
    rad1_write(qts, d->enable_reg, d->enable_idle);
    a = rad1_read(qts, d->pos_a);
    b = rad1_read(qts, d->pos_b);
    rad1_step_to(qts, t_status, rad1_status_due(k));
    stop_ptr = rad1_ring_read(qts, RAD1_TEST_STATUS_RAM +
                              (k % nslots) * RAD1_SLOT_STRIDE + d->slot_ptr);
    g_assert_cmphex(stop_ptr - ring_lo, ==,
                    RAD1_POS_A_FRAMES(a) * frame_bytes % d->ring_bytes);
    k += 3;
    rad1_step_to(qts, t_status, rad1_status_due(k));
    g_assert_cmphex(rad1_read(qts, d->pos_a), ==, a);
    g_assert_cmphex(rad1_read(qts, d->pos_b), ==, b);
    g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_STATUS_RAM +
                                   (k % nslots) * RAD1_SLOT_STRIDE +
                                   d->slot_ptr), ==, stop_ptr);

    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
}

/*
 * AES3 output subcode traffic while type 3 streams.
 * 64-bit stores across 0x1100..0x121f and toggles of 0x54 bits 2 and 3
 * are accepted and shadowed, and the stream stays on its sample clock.
 */
static void rad1_disturb_aes_subcode(QTestState *qts)
{
    uint32_t reg;

    for (reg = RAD1_SUBCODE_AES; reg <= RAD1_SUBCODE_AES_LAST; reg += 8) {
        rad1_write64(qts, reg, 0xff00000000000000ull | reg);
    }
    for (reg = RAD1_SUBCODE_AES; reg <= RAD1_SUBCODE_AES_LAST; reg += 8) {
        g_assert_cmphex(rad1_read64(qts, reg), ==,
                        0xff00000000000000ull | reg);
    }
    rad1_write(qts, rad1_aes_out.enable_reg,
               rad1_aes_out.enable_idle | 1 | 0x4);
    rad1_write(qts, rad1_aes_out.enable_reg,
               rad1_aes_out.enable_idle | 1 | 0x8);
    rad1_write(qts, rad1_aes_out.enable_reg, rad1_aes_out.enable_idle | 1);
}

/*
 * ADAT receiver controls: the 0x80 sequence (bits
 * 0xc0 set, 0x40 left set, a 0x80 pulse) and ADAT subcode stores, made
 * while type 5 streams, move nothing. Type 5 itself was started with no
 * ADAT input and no 0x80 write.
 */
static void rad1_disturb_adat_control(QTestState *qts)
{
    rad1_write(qts, RAD1_REG_CENTRAL_CTRL, RAD1_CENTRAL_ADAT_OFF | 0xc0);
    rad1_write(qts, RAD1_REG_CENTRAL_CTRL, RAD1_CENTRAL_ADAT_OFF | 0x40);
    rad1_write(qts, RAD1_REG_CENTRAL_CTRL, RAD1_CENTRAL_ADAT_OFF | 0xc0);
    rad1_write(qts, RAD1_REG_CENTRAL_CTRL, RAD1_CENTRAL_ADAT_OFF | 0x40);
    g_assert_cmphex(rad1_read(qts, RAD1_REG_CENTRAL_CTRL),
                    ==, RAD1_CENTRAL_ADAT_OFF | 0x40);
    rad1_write64(qts, RAD1_SUBCODE_ADAT, 0x0123456789abcdefull);
    g_assert_cmphex(rad1_read64(qts, RAD1_SUBCODE_ADAT),
                    ==, 0x0123456789abcdefull);
}

static void test_io6g_rad1_aes_in_stream(void)
{
    rad1_check_digital_stream(&rad1_aes_in, NULL);
}

static void test_io6g_rad1_aes_out_stream(void)
{
    rad1_check_digital_stream(&rad1_aes_out, rad1_disturb_aes_subcode);
}

static void test_io6g_rad1_adat_in_stream(void)
{
    rad1_check_digital_stream(&rad1_adat_in, NULL);
}

static void test_io6g_rad1_adat_out_stream(void)
{
    rad1_check_digital_stream(&rad1_adat_out, rad1_disturb_adat_control);
}

/*
 * Host-configured receivers.
 * A machine told "aes-input-rate" and "adat-input" presents, on every
 * completion and without change, the configured AES3 lock word for that
 * rate and the ADAT-present bit; the AES3 input then runs at the source's
 * rate. The default machine's absent receivers are covered by
 * io6g-rad1-status-dma-continuous.
 */
static void rad1_check_receivers(uint32_t rate, bool adat, uint32_t done)
{
    g_autofree char *extra = g_strdup_printf(
        "-global sgi-rad1-audio.aes-input-rate=%u "
        "-global sgi-rad1-audio.adat-input=%s", rate, adat ? "on" : "off");
    QTestState *qts = start_module_with(",baseio-preset=io6g", extra);
    const unsigned int nslots = 8;
    Rad1DigitalChannel aes_in = rad1_aes_in;
    int64_t t_status, t_enable;
    unsigned int k;

    aes_in.freq = rate;
    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_digital(qts, &aes_in, RAD1_TEST_DIG_RAM);
    t_status = rad1_start_status(qts, nslots);
    rad1_write(qts, aes_in.enable_reg, aes_in.enable_idle | 1);
    t_enable = rad1_clock_now(qts);

    for (k = 1; k <= 36; k++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;

        rad1_step_to(qts, t_status, rad1_status_due(k));
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET),
                        ==, done);
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_FLAGS_OFFSET),
                        ==, adat ? RAD1_SLOT_ADAT_PRESENT : 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
    }

    rad1_step_to(qts, t_enable, 10000000);
    rad1_check_digital_pos(qts, &aes_in, rad1_frames_due(10000000, rate));
    qtest_quit(qts);
}

static void test_io6g_rad1_receivers_44100_adat(void)
{
    rad1_check_receivers(44100, true, RAD1_AES_LOCK_44100);
}

static void test_io6g_rad1_receivers_48000(void)
{
    rad1_check_receivers(48000, false, RAD1_AES_LOCK_48000);
}

static void test_io6g_rad1_receivers_32000(void)
{
    rad1_check_receivers(32000, false, RAD1_AES_LOCK_32000);
}

/*
 * The digital outputs' samples, read back through a wav audiodev bound to
 * each interface. Every AES3 frame is (0x00123456, 0xffabcd12); every ADAT
 * frame starts with the same pair and carries distinct nonzero words in
 * channels 3 to 8. Both captures must hold (0x1234, -0x5433) and silence
 * only: ADAT channels 1 and 2 reach the host voice, left first, and 3 to 8
 * are dropped. The analog output keeps its own binding and is not started.
 */
static void test_io6g_rad1_digital_output_samples(void)
{
    g_autofree char *dir = NULL;
    g_autofree char *aes_wav = NULL;
    g_autofree char *adat_wav = NULL;
    g_autofree char *extra = NULL;
    GError *err = NULL;
    QTestState *qts;

    dir = g_dir_make_tmp("sn0-baseio-rad1-digital-XXXXXX", &err);
    g_assert_no_error(err);
    aes_wav = g_strdup_printf("%s/aes.wav", dir);
    adat_wav = g_strdup_printf("%s/adat.wav", dir);
    extra = g_strdup_printf(
        "-audiodev wav,id=aeswav,path=%s,out.frequency=%u,out.channels=2,"
        "out.format=s16 "
        "-audiodev wav,id=adatwav,path=%s,out.frequency=%u,out.channels=2,"
        "out.format=s16 "
        "-global sgi-rad1-audio.aes-audiodev=aeswav "
        "-global sgi-rad1-audio.adat-audiodev=adatwav",
        aes_wav, rad1_aes_out.freq, adat_wav, rad1_adat_out.freq);

    qts = start_module_with(",baseio-preset=io6g", extra);
    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_digital(qts, &rad1_aes_out, RAD1_TEST_DIG_RAM);
    rad1_prepare_digital(qts, &rad1_adat_out, RAD1_TEST_DIG2_RAM);
    rad1_write(qts, rad1_aes_out.enable_reg, rad1_aes_out.enable_idle | 1);
    rad1_write(qts, rad1_adat_out.enable_reg, rad1_adat_out.enable_idle | 1);
    qtest_clock_step(qts, 50000000);
    rad1_write(qts, rad1_aes_out.enable_reg, rad1_aes_out.enable_idle);
    rad1_write(qts, rad1_adat_out.enable_reg, rad1_adat_out.enable_idle);
    qtest_quit(qts);

    g_assert_cmpuint(rad1_wav_seeded_frames(aes_wav), >=, 1500);
    g_assert_cmpuint(rad1_wav_seeded_frames(adat_wav), >=, 1500);
    g_assert_cmpint(g_unlink(aes_wav), ==, 0);
    g_assert_cmpint(g_unlink(adat_wav), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/*
 * The subcode windows' doubleword path. By default the Bridge splits a
 * doubleword store to this slot into two 4-byte writes, which the stream
 * tests exercise. With the slot's device register marking it 64-bit, the
 * Bridge instead makes one 8-byte access, byte-swapped when the swap bit
 * is set, which reaches the model's 4-byte window as two accesses in
 * address order. In both settings a doubleword stored in each window reads
 * back whole. A doubleword stored with swap set and read back without it
 * arrives byte-reversed: the device kept the eight bytes in order, as one
 * little-endian doubleword, not as two words swapped.
 */
static void test_io6g_rad1_subcode_64bit(void)
{
    static const uint32_t regs[] = {
        RAD1_SUBCODE_ADAT, RAD1_SUBCODE_ADAT + 0xf0,
        RAD1_SUBCODE_AES, RAD1_SUBCODE_AES_LAST,
    };
    const uint32_t dev = BRIDGE_DEV_MEMORY_SPACE | BRIDGE_DEV_64BIT |
                         (MIO_RAD1_MMIO >> 20);
    const uint64_t value = 0x0123456789abcdefull;
    QTestState *qts = start_module_io6g();
    unsigned int i, swap;

    program_module(qts);
    program_rad1(qts);

    for (swap = 0; swap < 2; swap++) {
        baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) +
                            XTALK_REG_DATA,
                            dev | (swap ? BRIDGE_DEV_SWAP : 0));
        for (i = 0; i < G_N_ELEMENTS(regs); i++) {
            rad1_write64(qts, regs[i], value + i + swap * 16);
            g_assert_cmphex(rad1_read64(qts, regs[i]),
                            ==, value + i + swap * 16);
        }
    }

    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        dev | BRIDGE_DEV_SWAP);
    rad1_write64(qts, RAD1_SUBCODE_AES, value);
    baseio_window_write(qts, BRIDGE_DEVICE(MIO_RAD1_DEVICE) + XTALK_REG_DATA,
                        dev);
    g_assert_cmphex(rad1_read64(qts, RAD1_SUBCODE_AES), ==, bswap64(value));
}

/*
 * "aes-input-rate" describes a real AES3 source, so realize refuses a rate
 * the driver's rate table cannot report, and QEMU exits with the error
 * rather than starting or aborting. The qtest accelerator stands in for a
 * PROM, which the machine otherwise requires before it gets that far.
 */
static void test_io6g_rad1_rejects_aes_input_rate(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000,baseio-preset=io6g",
        "-accel", "qtest", "-display", "none", "-S", "-nodefaults",
        "-qmp", "stdio",
        "-global", "sgi-rad1-audio.aes-input-rate=22050", NULL,
    };

    assert_launch_rejected(args, "aes-input-rate must be 0 (none), 32000, "
                                 "44100 or 48000, not 22050");
}

/*
 * Save and restore with the ADAT output and the AES3 input running and a
 * subcode doubleword stored. On the destination, with its clock set to the
 * source's, both channels' A registers read as they did, the next status
 * completion lands on the source's schedule carrying both pointers of an
 * uninterrupted stream, and the subcode shadow survived.
 */
static void test_io6g_rad1_digital_migration(void)
{
    const unsigned int nslots = 8;
    g_autofree char *dir = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint32_t a_out, a_in;
    int64_t t_status, t_enable, t_src;
    uint64_t k, slot, frames;

    dir = g_dir_make_tmp("sn0-baseio-rad1-digital-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);

    from = start_module_with(",baseio-preset=io6g", "");
    rad1_prepare_output(from, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_digital(from, &rad1_adat_out, RAD1_TEST_DIG_RAM);
    rad1_prepare_digital(from, &rad1_aes_in, RAD1_TEST_DIG2_RAM);
    rad1_write64(from, RAD1_SUBCODE_AES, 0xfedcba9876543210ull);
    t_status = rad1_start_status(from, nslots);
    rad1_write(from, rad1_adat_out.enable_reg,
               rad1_adat_out.enable_idle | 1);
    rad1_write(from, rad1_aes_in.enable_reg, rad1_aes_in.enable_idle | 1);
    t_enable = rad1_clock_now(from);

    rad1_step_to(from, t_status,
                 (rad1_status_due(60) + rad1_status_due(61)) / 2);
    t_src = rad1_clock_now(from);
    a_out = rad1_read(from, rad1_adat_out.pos_a);
    a_in = rad1_read(from, rad1_aes_in.pos_a);

    to = qtest_initf("-M origin2000,baseio-preset=io6g -smp 8 -m 256M "
                     "-d unimp,guest_errors -incoming %s", uri);
    sgi_migrate_stopped(from, uri);
    qtest_clock_set(to, t_src);

    g_assert_cmphex(rad1_read(to, rad1_adat_out.pos_a), ==, a_out);
    g_assert_cmphex(rad1_read(to, rad1_aes_in.pos_a), ==, a_in);
    g_assert_cmphex(rad1_read64(to, RAD1_SUBCODE_AES),
                    ==, 0xfedcba9876543210ull);

    k = 61;
    slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
    rad1_ring_write(to, slot + RAD1_SLOT_DONE_OFFSET, 0);
    rad1_step_to(to, t_status, rad1_status_due(k) - 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET) &
                    RAD1_SLOT_DONE_BIT, ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(rad1_ring_read(to, slot + RAD1_SLOT_DONE_OFFSET),
                    ==, RAD1_SLOT_DONE_WORD);

    frames = rad1_frames_due(t_status + rad1_status_due(k) - t_enable,
                             rad1_adat_out.freq);
    g_assert_cmphex(rad1_ring_read(to, slot + rad1_adat_out.slot_ptr) -
                    (uint32_t)RAD1_TEST_PCI(RAD1_TEST_DIG_RAM), ==,
                    frames * rad1_adat_out.words * 4 %
                    rad1_adat_out.ring_bytes);
    frames = rad1_frames_due(t_status + rad1_status_due(k) - t_enable,
                             rad1_aes_in.freq);
    g_assert_cmphex(rad1_ring_read(to, slot + rad1_aes_in.slot_ptr) -
                    (uint32_t)RAD1_TEST_PCI(RAD1_TEST_DIG2_RAM), ==,
                    frames * rad1_aes_in.words * 4 % rad1_aes_in.ring_bytes);

    qtest_quit(to);
    qtest_quit(from);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/*
 * The input subcode DMA channels: type 6 on bit 2 of 0x48 beside the AES3
 * input's bit 0, type 7 on bit 2 of 0x44 beside the ADAT input's. Each
 * reads one register as A at shift 4, bits [17:4], and B at shift 18, bits
 * [31:18], which is also its parent's B count. The model moves only the
 * A field and writes zero subcode. Frames are the driver's widths, 64
 * words for type 6 and 32 for type 7; the rate, one frame per parent
 * frame, is a flagged model policy. The control word's tag is the type.
 */
typedef struct Rad1SubcodeChannel {
    const char *name;
    uint32_t a_lo, a_hi, a_ctrl;
    uint32_t b_lo, b_hi, b_ctrl;
    uint32_t ctrl;
    uint32_t ring_bytes;
    uint32_t pos;
    uint32_t slot_ptr;
    unsigned int words;
    uint32_t freq;
    const Rad1DigitalChannel *parent;
} Rad1SubcodeChannel;

static const Rad1SubcodeChannel rad1_aes_sub = {
    "AES3-input subcode", 0x4e0, 0x518, 0x4e4, 0x448, 0x44c, 0x450,
    0xfffff030u, 4096, 0x10, 0x20, 64, 48000, &rad1_aes_in,
};
static const Rad1SubcodeChannel rad1_adat_sub = {
    "ADAT-input subcode", 0x4d8, 0x514, 0x4dc, 0x454, 0x458, 0x45c,
    0xfffff038u, 4096, 0x08, 0x18, 32, 48000, &rad1_adat_in,
};

#define RAD1_SUB_FIELD(v)         (((v) >> 4) & 0x3fffu)
#define RAD1_SUB_ENABLE           0x4u

/*
 * Run a subcode channel with and without its parent and check each moves
 * only its own fields:
 *
 * - bit 2 alone starts the subcode channel and not the parent: the
 *   register's [17:4] counts frames, [31:18] and [3:0] stay zero, the
 *   parent's A and ring stay untouched, and the subcode ring is written
 *   with zeros from its start;
 * - setting bit 0 as well starts the parent without restarting the subcode
 *   channel, and each field counts its own channel's frames;
 * - each status completion carries both pointers, each agreeing with its
 *   channel's count;
 * - clearing bit 0 stops only the parent, clearing bit 2 then stops the
 *   subcode channel.
 */
static void rad1_check_subcode_stream(const Rad1SubcodeChannel *d)
{
    const Rad1DigitalChannel *p = d->parent;
    QTestState *qts = start_module_io6g();
    const unsigned int nslots = 8;
    const uint64_t sub_pci = RAD1_TEST_PCI(RAD1_TEST_DIG2_RAM);
    const uint32_t sub_lo = (uint32_t)sub_pci;
    const uint32_t par_lo = (uint32_t)RAD1_TEST_PCI(RAD1_TEST_DIG_RAM);
    uint32_t reg, pa, frozen, par_frozen;
    int64_t t_status, t_sub, t_par, t;
    uint64_t k, due;
    unsigned int i;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_digital(qts, p, RAD1_TEST_DIG_RAM);
    for (i = 0; i < d->ring_bytes; i += 4) {
        rad1_ring_write(qts, RAD1_TEST_DIG2_RAM + i, RAD1_IN_POISON);
    }
    rad1_write(qts, d->a_lo, sub_lo);
    rad1_write(qts, d->a_hi, (uint32_t)(sub_pci >> 32));
    rad1_write(qts, d->a_ctrl, d->ctrl);
    rad1_write(qts, d->b_lo, sub_lo);
    rad1_write(qts, d->b_hi, (uint32_t)(sub_pci >> 32));
    rad1_write(qts, d->b_ctrl, d->ctrl);

    t_status = rad1_start_status(qts, nslots);
    rad1_write(qts, p->enable_reg, p->enable_idle | RAD1_SUB_ENABLE);
    t_sub = rad1_clock_now(qts);

    /* Short enough that the frames written fit the ring unwrapped. */
    rad1_step_to(qts, t_sub, 200000);
    due = rad1_frames_due(200000, d->freq);
    reg = rad1_read(qts, d->pos);
    g_assert_cmpuint(RAD1_SUB_FIELD(reg), ==, due & 0x3fff);
    g_assert_cmphex(reg & ~(0x3fffu << 4), ==, 0);
    g_assert_cmphex(rad1_read(qts, p->pos_a), ==, 0);
    g_assert_cmpuint(due * d->words * 4, <, d->ring_bytes);
    for (i = 0; i < d->ring_bytes / 4; i++) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG2_RAM + i * 4), ==,
                        i < due * d->words ? 0 : RAD1_IN_POISON);
    }
    for (i = 0; i < p->ring_bytes / 4; i++) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG_RAM + i * 4),
                        ==, RAD1_IN_POISON);
    }

    /* The parent joins; the subcode channel keeps its own clock. */
    rad1_write(qts, p->enable_reg,
               p->enable_idle | RAD1_SUB_ENABLE | 1);
    t_par = rad1_clock_now(qts);
    rad1_step_to(qts, t_sub, 8000000);
    t = rad1_clock_now(qts);
    reg = rad1_read(qts, d->pos);
    pa = rad1_read(qts, p->pos_a);
    g_assert_cmpuint(RAD1_SUB_FIELD(reg), ==,
                     rad1_frames_due(t - t_sub, d->freq) & 0x3fff);
    g_assert_cmpuint(RAD1_POS_A_FRAMES(reg), ==,
                     rad1_frames_due(t - t_par, p->freq) & 0x3fff);
    g_assert_cmpuint(RAD1_POS_A_FRAMES(pa), ==,
                     rad1_frames_due(t - t_par, p->freq) & 0x3fff);
    g_assert_cmphex(reg & 0xfu, ==, 0);

    k = 1;
    while (t_status + rad1_status_due(k) <= t + 1) {
        k++;
    }
    for (i = 0; i < 40; i++, k++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
        uint32_t m_a;

        rad1_step_to(qts, t_status, rad1_status_due(k));
        reg = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET + d->pos);
        m_a = rad1_ring_read(qts, slot + RAD1_SLOT_MIRROR_OFFSET + p->pos_a);
        g_assert_cmphex(rad1_ring_read(qts, slot + d->slot_ptr) - sub_lo, ==,
                        RAD1_SUB_FIELD(reg) * d->words * 4 % d->ring_bytes);
        g_assert_cmphex(rad1_ring_read(qts, slot + p->slot_ptr) - par_lo, ==,
                        RAD1_POS_A_FRAMES(m_a) * p->words * 4 %
                        p->ring_bytes);
    }

    /* Clearing bit 0 stops only the parent. */
    rad1_write(qts, p->enable_reg, p->enable_idle | RAD1_SUB_ENABLE);
    par_frozen = rad1_read(qts, p->pos_a);
    t = qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rad1_read(qts, p->pos_a), ==, par_frozen);
    g_assert_cmpuint(RAD1_SUB_FIELD(rad1_read(qts, d->pos)), ==,
                     rad1_frames_due(t - t_sub, d->freq) & 0x3fff);

    /* Clearing bit 2 stops the subcode channel. */
    rad1_write(qts, p->enable_reg, p->enable_idle);
    frozen = rad1_read(qts, d->pos);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rad1_read(qts, d->pos), ==, frozen);

    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
}

static void test_io6g_rad1_aes_subcode_stream(void)
{
    rad1_check_subcode_stream(&rad1_aes_sub);
}

static void test_io6g_rad1_adat_subcode_stream(void)
{
    rad1_check_subcode_stream(&rad1_adat_sub);
}

/*
 * A ring length comes in 128-byte units, and three of them are not a whole
 * number of 256-byte AES3-input subcode frames. The model starts nothing
 * for such a ring: the position holds and the ring is untouched. Four
 * units, programmed the same way, do start, so the refusal is the length's.
 * Positions persist on the shared machine, so each check is relative.
 */
static void rad1_program_subcode_ring(QTestState *qts,
                                      const Rad1SubcodeChannel *d,
                                      uint32_t ctrl)
{
    const uint64_t sub_pci = RAD1_TEST_PCI(RAD1_TEST_DIG2_RAM);

    rad1_write(qts, d->a_lo, (uint32_t)sub_pci);
    rad1_write(qts, d->a_hi, (uint32_t)(sub_pci >> 32));
    rad1_write(qts, d->a_ctrl, ctrl);
    rad1_write(qts, d->b_lo, (uint32_t)sub_pci);
    rad1_write(qts, d->b_hi, (uint32_t)(sub_pci >> 32));
    rad1_write(qts, d->b_ctrl, ctrl);
}

static void test_io6g_rad1_subcode_rejects_partial_frame(void)
{
    const Rad1SubcodeChannel *d = &rad1_aes_sub;
    const Rad1DigitalChannel *p = d->parent;
    QTestState *qts = start_module_io6g();
    uint32_t before;
    int64_t t_sub;
    unsigned int i;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    for (i = 0; i < d->ring_bytes; i += 4) {
        rad1_ring_write(qts, RAD1_TEST_DIG2_RAM + i, RAD1_IN_POISON);
    }

    /* Three units, tag 6: 384 bytes, a frame and a half. */
    rad1_program_subcode_ring(qts, d, 0xfffffeb0u);
    before = rad1_read(qts, d->pos);
    rad1_write(qts, p->enable_reg, p->enable_idle | RAD1_SUB_ENABLE);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(rad1_read(qts, d->pos), ==, before);
    for (i = 0; i < d->ring_bytes; i += 4) {
        g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG2_RAM + i), ==,
                        RAD1_IN_POISON);
    }
    rad1_write(qts, p->enable_reg, p->enable_idle);

    /* Four units, 512 bytes: two whole frames, so the channel runs. */
    rad1_program_subcode_ring(qts, d, 0xfffffe30u);
    rad1_write(qts, p->enable_reg, p->enable_idle | RAD1_SUB_ENABLE);
    t_sub = rad1_clock_now(qts);
    rad1_step_to(qts, t_sub, 1000000);
    g_assert_cmpuint(RAD1_SUB_FIELD(rad1_read(qts, d->pos)), ==,
                     (RAD1_SUB_FIELD(before) +
                      rad1_frames_due(1000000, d->freq)) & 0x3fff);
    g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG2_RAM), ==, 0);
    g_assert_cmphex(rad1_ring_read(qts, RAD1_TEST_DIG2_RAM + 512), ==,
                    RAD1_IN_POISON);
    rad1_write(qts, p->enable_reg, p->enable_idle);
}

/*
 * Genlock. The driver measures the period of status slots carrying +0x78
 * bit 0x80 or 0x100 and classifies it as a video field rate: 16 393 442 to
 * 16 949 152 ns for 59.94 Hz, 19 607 843 to 20 407 139 ns for 50 Hz. The
 * "genlock" property says what is cabled to the video sync inputs: nothing
 * by default, which must never produce an event, or an NTSC or PAL
 * reference, which must produce one event per field. The model alternates
 * the two bits by field, its stated choice. Register 0x34, mirrored at slot
 * +0x74, stays constant either way: its polarity is unknown.
 */
#define RAD1_SLOT_FIELD_BITS      0x180u
#define RAD1_SLOT_SYNC_STATUS     (RAD1_SLOT_MIRROR_OFFSET + 0x34)
#define RAD1_NTSC_BAND_LO         16393442
#define RAD1_NTSC_BAND_HI         16949152
#define RAD1_PAL_BAND_LO          19607843
#define RAD1_PAL_BAND_HI          20407139
#define RAD1_RAD1_QOM_PATH        "/machine/module0/slot8/baseio/rad1"

/*
 * Walk @n completions from a fresh continuous start and collect the ones
 * carrying a field event: their due times into @times and their bits into
 * @bits, returning how many. Every slot's 0x34 mirror must read 0.
 */
static unsigned int rad1_collect_field_events(QTestState *qts, unsigned int n,
                                              int64_t *times, uint32_t *bits,
                                              unsigned int max)
{
    const unsigned int nslots = 8;
    int64_t t_status = rad1_start_status(qts, nslots);
    unsigned int k, events = 0;

    for (k = 1; k <= n; k++) {
        uint64_t slot = RAD1_TEST_STATUS_RAM + (k % nslots) * RAD1_SLOT_STRIDE;
        uint32_t flags;

        rad1_step_to(qts, t_status, rad1_status_due(k));
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_DONE_OFFSET),
                        ==, RAD1_SLOT_DONE_WORD);
        g_assert_cmphex(rad1_ring_read(qts, slot + RAD1_SLOT_SYNC_STATUS),
                        ==, 0);
        rad1_ring_write(qts, slot + RAD1_SLOT_DONE_OFFSET, 0);
        flags = rad1_ring_read(qts, slot + RAD1_SLOT_FLAGS_OFFSET);
        if (flags & RAD1_SLOT_FIELD_BITS) {
            g_assert_cmpuint(events, <, max);
            times[events] = rad1_status_due(k);
            bits[events] = flags & RAD1_SLOT_FIELD_BITS;
            events++;
        }
    }
    rad1_write(qts, RAD1_REG_STATUS_DMA_CTRL, RAD1_STATUS_STOP);
    return events;
}

/* No reference connected: 800 completions, 200 ms, carry no field event. */
static void test_io6g_rad1_genlock_absent(void)
{
    QTestState *qts = start_module_io6g();
    int64_t times[1];
    uint32_t bits[1];

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    g_assert_cmpuint(rad1_collect_field_events(qts, 800, times, bits, 1),
                     ==, 0);
}

/*
 * A reference connected: over 200 ms of completions, the events alternate
 * between the two bits, one per slot, each a field period apart to within
 * one completion interval, and the driver's measurement, elapsed time over
 * the event count, falls inside the field rate's band.
 */
static void rad1_check_genlock(const char *standard, int64_t field_ns,
                               int64_t band_lo, int64_t band_hi)
{
    g_autofree char *extra =
        g_strdup_printf("-global sgi-rad1-audio.genlock=%s", standard);
    QTestState *qts = start_module_with(",baseio-preset=io6g", extra);
    int64_t times[16], interval = rad1_status_due(1), measured;
    uint32_t bits[16];
    unsigned int events, i;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    events = rad1_collect_field_events(qts, 800, times, bits,
                                       G_N_ELEMENTS(times));
    g_assert_cmpuint(events, >=, 200000000 / field_ns - 1);
    g_assert_cmpuint(events, <=, 200000000 / field_ns + 1);
    for (i = 0; i < events; i++) {
        g_assert_true(bits[i] == 0x80 || bits[i] == 0x100);
        if (i) {
            g_assert_cmphex(bits[i], !=, bits[i - 1]);
            g_assert_cmpint(times[i] - times[i - 1], >=, field_ns - interval);
            g_assert_cmpint(times[i] - times[i - 1], <=, field_ns + interval);
        }
    }
    measured = (times[events - 1] - times[0]) / (events - 1);
    g_assert_cmpint(measured, >=, band_lo);
    g_assert_cmpint(measured, <=, band_hi);
    qtest_quit(qts);
}

static void test_io6g_rad1_genlock_ntsc(void)
{
    rad1_check_genlock("ntsc", 16683333, RAD1_NTSC_BAND_LO, RAD1_NTSC_BAND_HI);
}

static void test_io6g_rad1_genlock_pal(void)
{
    rad1_check_genlock("pal", 20000000, RAD1_PAL_BAND_LO, RAD1_PAL_BAND_HI);
}

/*
 * A slot carries both field bits when two fields began since the previous
 * completion, which takes a gap in the status engine longer than a field.
 * Stop the engine for three NTSC fields and restart it: the first
 * completion carries both bits, and the next event, a field later, one.
 */
static void test_io6g_rad1_genlock_two_event_slot(void)
{
    const int64_t field_ns = 16683333;
    QTestState *qts = start_module_with(",baseio-preset=io6g",
                                        "-global sgi-rad1-audio.genlock=ntsc");
    int64_t times[16], interval = rad1_status_due(1);
    uint32_t bits[16];
    unsigned int events;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    events = rad1_collect_field_events(qts, 400, times, bits,
                                       G_N_ELEMENTS(times));
    g_assert_cmpuint(events, >=, 1);

    qtest_clock_step(qts, 3 * field_ns);
    events = rad1_collect_field_events(qts, 200, times, bits,
                                       G_N_ELEMENTS(times));
    g_assert_cmpuint(events, >=, 2);
    g_assert_cmpint(times[0], ==, rad1_status_due(1));
    g_assert_cmphex(bits[0], ==, 0x180);
    g_assert_true(bits[1] == 0x80 || bits[1] == 0x100);
    g_assert_cmpint(times[1] - times[0], <=, field_ns + interval);
    qtest_quit(qts);
}

/* "genlock" names a real video standard; anything else is refused. */
static void test_io6g_rad1_rejects_genlock(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000,baseio-preset=io6g",
        "-accel", "qtest", "-display", "none", "-S", "-nodefaults",
        "-qmp", "stdio", "-global", "sgi-rad1-audio.genlock=secam", NULL,
    };

    assert_launch_rejected(args, "genlock must be none, ntsc or pal, "
                                 "not secam");
}

static uint32_t rad1_mux_word(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{ 'execute': 'qom-get', 'arguments': "
                            "{ 'path': %s, 'property': 'mux-word' } }",
                            RAD1_RAD1_QOM_PATH);
    uint32_t word;

    g_assert_true(qdict_haskey(resp, "return"));
    word = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return word;
}

/*
 * The mux serial word, shifted out as rad_vl_write_MUX_SEL_MIO does: for
 * each bit, most significant first, 0x8c = bit | 2, then 0x4c from its
 * shadow with bit 27 clear, then set; then 0x88 = 2, 0x88 = 3 to latch,
 * and 0x8c restored. The model decodes the word into its read-only
 * "mux-word" property. The shadow here keeps bit 0 of 0x4c set, as the
 * driver's does with the analog input running, and the input keeps
 * running on its clock through it. A second word replaces the first only
 * at its own latch.
 */
static void rad1_send_mux_word(QTestState *qts, uint32_t shadow, uint16_t word)
{
    int i;

    for (i = 15; i >= 0; i--) {
        rad1_write(qts, RAD1_REG_SHIFT_LOAD_DATA, ((word >> i) & 1) | 2);
        rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, shadow & ~RAD1_IN_MUX_CLOCK);
        rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, shadow | RAD1_IN_MUX_CLOCK);
    }
}

static void test_io6g_rad1_mux_word(void)
{
    QTestState *qts = start_module_io6g();
    int64_t t_enable;
    uint32_t a;

    rad1_prepare_output(qts, RAD1_SAMPLE_LEFT_WORD, RAD1_SAMPLE_RIGHT_WORD);
    rad1_prepare_input(qts);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_RUN);
    t_enable = rad1_clock_now(qts);
    g_assert_cmphex(rad1_mux_word(qts), ==, 0);

    rad1_send_mux_word(qts, RAD1_IN_ENABLE_RUN, 0xa5c3);
    /* Not latched yet: the word only appears at 0x88 = 2 then 3. */
    g_assert_cmphex(rad1_mux_word(qts), ==, 0);
    rad1_write(qts, RAD1_REG_LATCH_PULSE, 2);
    rad1_write(qts, RAD1_REG_LATCH_PULSE, 3);
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_DATA, 2);
    g_assert_cmphex(rad1_mux_word(qts), ==, 0xa5c3);

    rad1_send_mux_word(qts, RAD1_IN_ENABLE_RUN, 0x1001);
    g_assert_cmphex(rad1_mux_word(qts), ==, 0xa5c3);
    rad1_write(qts, RAD1_REG_LATCH_PULSE, 2);
    rad1_write(qts, RAD1_REG_LATCH_PULSE, 3);
    g_assert_cmphex(rad1_mux_word(qts), ==, 0x1001);

    rad1_step_to(qts, t_enable, 5000000);
    a = rad1_read(qts, RAD1_IN_POS_A);
    g_assert_cmpuint(RAD1_POS_A_FRAMES(a), ==, rad1_out_frames_due(5000000));
    rad1_write(qts, RAD1_REG_SHIFT_LOAD_CLOCK, RAD1_IN_ENABLE_IDLE);
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

    /* Advances with the clock by about step_ns/tick (allow flooring slop). */
    g_assert_cmpuint(after, >, before);
    g_assert_cmpuint(after - before, >=, expect - 2);
    g_assert_cmpuint(after - before, <=, expect + 2);

    /* Read-only: a write neither sticks nor rewinds the clock count. */
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
 * on either chip answers with the same ack/self-test-pass pattern,
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
 * The board IOC3's own combined interrupt reaches the Bridge on input 4
 * (BASEIO_IOC3_IRQ_PIN). io6g is the only preset whose board chip carries
 * a keyboard, so it is the only board on which a keyboard reply can drive
 * that wire from the SuperIO side; the pin-6 tests below assert input 4
 * stays clear while the MediaIO chip fires, and this is the positive
 * counterpart: device 2's latched keyboard source raises input 4 alone and
 * leaves the MediaIO chip's input 6 clear.
 */
static void test_io6g_board_keyboard_reaches_bridge_pin_4(void)
{
    QTestState *qts = start_module_io6g();

    program_module(qts);
    program_baseio_ioc3(qts);

    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    (BRIDGE_PIN_BOARD_IOC3 | BRIDGE_PIN_MIO_IOC3), ==, 0);

    baseio_window_write(qts, IOC3_WINDOW + INT_ENABLE_SET, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_BOARD_IOC3, ==, 0);

    baseio_window_write(qts, IOC3_WINDOW + KBC_KBD_TRANSMIT, 0xff);
    g_assert_cmphex(baseio_window_read(qts, IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_BOARD_IOC3, ==, BRIDGE_PIN_BOARD_IOC3);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_MIO_IOC3, ==, 0);

    /* Latched: only an acknowledging write to the source clears the pin. */
    baseio_window_write(qts, IOC3_WINDOW + INT_STATUS, INT_KEYBOARD);
    g_assert_cmphex(baseio_window_read(qts, IOC3_WINDOW + INT_STATUS) &
                    INT_KEYBOARD, ==, 0);
    g_assert_cmphex(baseio_window_read(qts, BRIDGE_INT_STATUS) &
                    BRIDGE_PIN_BOARD_IOC3, ==, 0);
}

/*
 * The MediaIO IOC3's own combined interrupt reaches the Bridge on input 6
 * (BASEIO_G_MIO_IOC3_IRQ_PIN), separate from device 2's board IOC3 on
 * input 4. This is the same latched keyboard/mouse source
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
     * A queued keyboard reply drives the source, combined status, SuperIO
     * GPIO-out, and board wire to Bridge input 6 alone;
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
 * than overrunning it. Armed after one character (the tighter clamp
 * the control register offers), a two-character reply arrives in two
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
     * again through the source, combined status, SuperIO GPIO-out, and board
     * wire, with
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
 * from each pass's return and updated `*last_discrepancy`. A guest's
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
 * The part number as Linux's Bridge probe assembles it: bytes 11-29 and
 * 32-37 of the record, spaces dropped.
 */
static void page_part(const uint8_t *page, char *out)
{
    unsigned int i, j = 0;

    for (i = 11; i < 38; i++) {
        if (i != 30 && i != 31 && page[i] != ' ') {
            out[j++] = page[i];
        }
    }
    out[j] = 0;
}

/* Select `rom` by Match ROM, then read its record's two pages back. */
static void nic_read_part(QTestState *qts, uint64_t rom, char *part)
{
    uint8_t exchange[6];
    uint8_t redirect[3];
    uint8_t page[66];
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

    for (i = 0; i < 34; i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, page, 34), ==, NIC_CRC_RESIDUE);

    /*
     * Page 1 opens with its own redirection byte and check, then its data
     * lands over page 0's framing check so the record reads contiguously.
     */
    for (i = 0; i < sizeof(redirect); i++) {
        redirect[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, redirect, sizeof(redirect)), ==,
                    NIC_CRC_RESIDUE);
    for (i = 32; i < sizeof(page); i++) {
        page[i] = nic_read_byte(qts);
    }
    g_assert_cmphex(nic_crc(0, &page[32], 34), ==, NIC_CRC_RESIDUE);
    page_part(page, part);
}

/*
 * io6g's Bridge carries two board-identity records on the same
 * multi-drop one-wire bus: the io6g board's own, and the MediaIO
 * daughtercard's, added by sgi_bridge_add_onewire_board_part(). A search
 * from scratch, knowing neither identity beforehand, must discover
 * exactly the two and read each one's own part number back, the same
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
    qtest_add_func("sn0/baseio/baseio-rejects-bad-romfile",
                   test_baseio_rejects_bad_romfile);
    qtest_add_func("sn0/baseio/baseio-timekeeper-storage-reset",
                   test_baseio_timekeeper_storage_reset);
    qtest_add_func("sn0/baseio/baseio-timekeeper-repair-stores",
                   test_baseio_timekeeper_repair_stores);
    qtest_add_func("sn0/baseio/baseio-timekeeper-backing-reopen",
                   test_baseio_timekeeper_backing_reopen);
    qtest_add_func("sn0/baseio/mio-ioc3-subsystem-id",
                   test_mio_ioc3_subsystem_id);
    qtest_add_func("sn0/baseio/io6g-rad1-slot", test_io6g_rad1_slot);
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
    qtest_add_func("sn0/baseio/io6g-rad1-type1-output-stream",
                   test_io6g_rad1_type1_output_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-output-samples",
                   test_io6g_rad1_output_samples);
    qtest_add_func("sn0/baseio/io6g-rad1-output-migration",
                   test_io6g_rad1_output_migration);
    qtest_add_func("sn0/baseio/io6g-rad1-type0-input-stream",
                   test_io6g_rad1_type0_input_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-input-migration",
                   test_io6g_rad1_input_migration);
    qtest_add_func("sn0/baseio/io6g-rad1-input-without-host-input",
                   test_io6g_rad1_input_without_host_input);
    qtest_add_func("sn0/baseio/io6g-rad1-aes-in-stream",
                   test_io6g_rad1_aes_in_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-aes-out-stream",
                   test_io6g_rad1_aes_out_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-adat-in-stream",
                   test_io6g_rad1_adat_in_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-adat-out-stream",
                   test_io6g_rad1_adat_out_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-receivers-44100-adat",
                   test_io6g_rad1_receivers_44100_adat);
    qtest_add_func("sn0/baseio/io6g-rad1-receivers-48000",
                   test_io6g_rad1_receivers_48000);
    qtest_add_func("sn0/baseio/io6g-rad1-receivers-32000",
                   test_io6g_rad1_receivers_32000);
    qtest_add_func("sn0/baseio/io6g-rad1-digital-output-samples",
                   test_io6g_rad1_digital_output_samples);
    qtest_add_func("sn0/baseio/io6g-rad1-digital-migration",
                   test_io6g_rad1_digital_migration);
    qtest_add_func("sn0/baseio/io6g-rad1-subcode-64bit",
                   test_io6g_rad1_subcode_64bit);
    qtest_add_func("sn0/baseio/io6g-rad1-rejects-aes-input-rate",
                   test_io6g_rad1_rejects_aes_input_rate);
    qtest_add_func("sn0/baseio/io6g-rad1-aes-subcode-stream",
                   test_io6g_rad1_aes_subcode_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-adat-subcode-stream",
                   test_io6g_rad1_adat_subcode_stream);
    qtest_add_func("sn0/baseio/io6g-rad1-subcode-rejects-partial-frame",
                   test_io6g_rad1_subcode_rejects_partial_frame);
    qtest_add_func("sn0/baseio/io6g-rad1-genlock-absent",
                   test_io6g_rad1_genlock_absent);
    qtest_add_func("sn0/baseio/io6g-rad1-genlock-ntsc",
                   test_io6g_rad1_genlock_ntsc);
    qtest_add_func("sn0/baseio/io6g-rad1-genlock-pal",
                   test_io6g_rad1_genlock_pal);
    qtest_add_func("sn0/baseio/io6g-rad1-genlock-two-event-slot",
                   test_io6g_rad1_genlock_two_event_slot);
    qtest_add_func("sn0/baseio/io6g-rad1-mux-word",
                   test_io6g_rad1_mux_word);
    qtest_add_func("sn0/baseio/io6g-rad1-rejects-genlock",
                   test_io6g_rad1_rejects_genlock);
    qtest_add_func("sn0/baseio/io6g-rad1-ust-counter",
                   test_io6g_rad1_ust_counter);
    qtest_add_func("sn0/baseio/io6g-ps2", test_io6g_ps2);
    qtest_add_func("sn0/baseio/io6g-board-keyboard-reaches-bridge-pin-4",
                   test_io6g_board_keyboard_reaches_bridge_pin_4);
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
