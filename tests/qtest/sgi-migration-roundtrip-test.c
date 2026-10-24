/*
 * Save and restore every SGI machine configuration and compare what the
 * guest can see on both sides.
 *
 * Each configuration starts a source, drives a fixed workload of register
 * and RAM writes that moves its devices out of reset, migrates to a fresh
 * destination started from the same build, and compares every register in
 * its read-safe list plus the RAM the workload wrote. A field missing from
 * a device's VMStateDescription leaves the destination at its reset value
 * and fails the comparison. Then it resets the destination and requires
 * every probe to read its pre-load reset value again, so state a load
 * restores that a reset does not clear fails too.
 *
 * The comparison only proves anything for state the workload actually
 * moved. So before loading, the test reads each probe marked "moved" on the
 * destination, which is still at reset, and requires the source to differ.
 * A table entry whose write did not take effect, or whose reset value
 * equals the value written, fails here instead of passing vacuously.
 *
 * Hand-written migration cases remain for mechanisms that are mid-flight
 * when the machine stops: partly delivered commands, transfers in
 * progress, armed timers and pending interrupts. This test covers state at
 * rest. Adding a device means adding a table, not code. State the guest
 * can only observe through a command and its reply, or by drawing (GL
 * context state, framebuffer planes), is outside what plain register reads
 * can compare and keeps its hand-written cases.
 *
 * Probes are read twice on the source before migration and must agree, so
 * a register with a read side effect that slips into a table shows up as
 * an unstable source rather than a confusing mismatch.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

typedef enum RtKind {
    RT_END,
    RT_WRITE,   /* workload: write @value */
    RT_PROBE,   /* compare on both sides */
    RT_MOVED,   /* compare, and the workload must have moved it from reset */
    RT_KEPT,    /* as RT_MOVED, but the model keeps it across system reset */
    RT_SELECT,  /* write @value before the probes that follow, every pass */
} RtKind;

typedef struct RtStep {
    RtKind kind;
    uint8_t width;      /* access size in bytes: 1, 2, 4 or 8 */
    uint8_t via;        /* 0: the system address space; n: CPU n - 1's */
    uint64_t addr;
    uint64_t value;
} RtStep;

#define W8(a, v)        { RT_WRITE, 1, 0, (a), (v) }
#define W16(a, v)       { RT_WRITE, 2, 0, (a), (v) }
#define W32(a, v)       { RT_WRITE, 4, 0, (a), (v) }
#define W64(a, v)       { RT_WRITE, 8, 0, (a), (v) }
#define P8(a)           { RT_PROBE, 1, 0, (a), 0 }
#define P16(a)          { RT_PROBE, 2, 0, (a), 0 }
#define P32(a)          { RT_PROBE, 4, 0, (a), 0 }
#define P64(a)          { RT_PROBE, 8, 0, (a), 0 }
#define M8(a)           { RT_MOVED, 1, 0, (a), 0 }
#define M16(a)          { RT_MOVED, 2, 0, (a), 0 }
#define M32(a)          { RT_MOVED, 4, 0, (a), 0 }
#define M64(a)          { RT_MOVED, 8, 0, (a), 0 }
#define K64(a)          { RT_KEPT, 8, 0, (a), 0 }
/*
 * An index or select register that picks what the next probes read. Each
 * probe pass writes it in table order on whichever side it reads, so both
 * sides read the same registers; it must leave the state it selects alone.
 */
#define S8(a, v)        { RT_SELECT, 1, 0, (a), (v) }
/* The same through one processor's view, for state off node 0. */
#define CW32(c, a, v)   { RT_WRITE, 4, (c) + 1, (a), (v) }
#define CW64(c, a, v)   { RT_WRITE, 8, (c) + 1, (a), (v) }
#define CM32(c, a)      { RT_MOVED, 4, (c) + 1, (a), 0 }
#define CM64(c, a)      { RT_MOVED, 8, (c) + 1, (a), 0 }
#define RT_DONE         { RT_END, 0, 0, 0, 0 }

/*
 * One device instance: its table, and where this machine puts it. Every
 * step address is relative to @base, so one table serves a device that
 * different machines place at different addresses.
 */
typedef struct RtDevice {
    const char *name;
    const RtStep *steps;
    uint64_t base;
} RtDevice;

/* A run of guest RAM the workload fills with a pattern and compares. */
typedef struct RtRam {
    uint64_t addr;
    uint32_t size;
} RtRam;

typedef struct RtMachine {
    const char *name;
    const char *machine;        /* -M value, for qtest_has_machine() */
    const char *device;         /* a -device the args need, or NULL */
    const char *args;
    size_t prom_size;           /* erased -bios image size, or 0 for none */
    const RtDevice *const *devices;
    const RtRam *ram;
} RtMachine;

/* ---- Device tables ---- */

/*
 * Every probe below is a register whose read in the model returns stored
 * state and changes nothing: no clear-on-read status, no FIFO or data
 * port that advances, no strobe. That is a fact about the model, checked
 * against its code; for some registers, such as the IMPACT colour-map
 * data port, the hardware's read behaviour is not documented. The register
 * maps are in each device's datasheet, and the values written follow the
 * device's own register qtest, which already reads each one back.
 *
 * Some tables write interrupt enables and masks (the Bridge interrupt
 * enable, the Hub's PI mask 0A, the Heart masks, the Bedrock CC pending
 * mask). Nothing is pending on a machine that runs no guest code, so no
 * interrupt is delivered.
 */

/* ---- SN0 node: Hub, Bridge, IOC3, Timekeeper (Origin 200, node 0) ---- */

#define HUB(o)                  (HUB_LOCAL + (o))
#define XBRIDGE                 (SPACE_IO + (UINT64_C(8) << 24))
/* Bridge offsets are from the widget base, which the instance gives. */
#define BRIDGE_REG(o)           ((o) + 4)
#define BRIDGE_CFG0(dev)        (0x020000 + (dev) * 0x1000)
#define BRIDGE_ATE(n)           (0x010000 + 8 * (n))
#define BRIDGE_ATE_LOW(n)       (0x011000 + 4 * (n))
#define IOC3_SLOT               2
#define IOC3_MMIO               0x10000000u
/* IOC3 and Timekeeper offsets are from the IOC3's PCI window. */
#define IOC3(o)                 (o)
#define IOC3_UART_A             0x020178
#define TIMEKEEPER(o)           (0x80000 + (o))
#define SN0_IOC3_WINDOW         (XBRIDGE + 0x600000)

static const RtStep sn0_hub[] = {
    W64(HUB(0x4003c0), 0x123456),               /* IO crosstalk prescale */
    W64(HUB(0x4003b8), 0x5a),                   /* IO crosstalk timeout */
    W64(HUB(0x400150), 0x0000000500f01abcULL),  /* IO scratch 0 */
    W64(HUB(0x400158), 0x0000000200f01111ULL),  /* IO scratch 1 */
    W64(HUB(0x400120), 0x00ff00ff00ff00ffULL),  /* IO device error mask */
    W64(HUB(0x0000a8), 0xfedcba9876543210ULL),  /* PI interrupt mask 0A */
    W64(HUB(0x600100), 0x0123456789abcdefULL),  /* NI scratch 0 */
    W64(HUB(0x600108), 0xfedcba9876543210ULL),  /* NI scratch 1 */
    W64(HUB(0x600110), 0x7ffff),                /* NI diagnostic parms */
    W64(HUB(0x608000), 0x2a5a),                 /* NI port parms */
    W64(HUB(0x638100), 0x1b),                   /* NI local route 0 */
    W64(HUB(0x6380f8), 0x16),                   /* NI meta route 31 */
    M64(HUB(0x4003c0)),
    M64(HUB(0x4003b8)),
    M64(HUB(0x400150)),
    M64(HUB(0x400158)),
    M64(HUB(0x400120)),
    M64(HUB(0x0000a8)),
    M64(HUB(0x600100)),
    M64(HUB(0x600108)),
    M64(HUB(0x600110)),
    M64(HUB(0x608000)),
    M64(HUB(0x638100)),
    M64(HUB(0x6380f8)),
    RT_DONE
};

static const RtStep sn0_bridge[] = {
    /* Aim slot 2's window at the IOC3, as the PROM does. */
    W32(BRIDGE_CFG0(IOC3_SLOT) + PCI_BAR0, IOC3_MMIO),
    W32(BRIDGE_CFG0(IOC3_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY),
    W32(BRIDGE_REG(0x200 + 8 * IOC3_SLOT),
        DEV_MEMORY_SPACE | (IOC3_MMIO >> 20)),
    W32(BRIDGE_REG(0x070), 0xa5c3e1f7),         /* test pin control */
    W32(BRIDGE_REG(0x030), 0x0a3f1234),         /* interrupt dest upper */
    W32(BRIDGE_REG(0x038), 0x5a5a5a5a),         /* interrupt dest lower */
    W32(BRIDGE_REG(0x028), 0x00012345),         /* request timeout */
    W32(BRIDGE_REG(0x048), 0x01230a58),         /* link config */
    W32(BRIDGE_REG(0x0a0), 0x0003a547),         /* arbitration */
    W32(BRIDGE_REG(0x0c0), 0x000a02bc),         /* bus timeout */
    W32(BRIDGE_REG(0x108), 0x0a5a0000),         /* interrupt enable */
    W32(BRIDGE_REG(0x118), 0xa5),               /* interrupt mode */
    W32(BRIDGE_REG(0x120), 0x00ab4c91),         /* interrupt device */
    W32(BRIDGE_REG(0x128), 0x5a),               /* host error field */
    W32(BRIDGE_REG(0x148), 0x00012345),         /* interrupt host 3 */
    W32(BRIDGE_REG(0x230), 0x0a5a5a5a),         /* device 6 */
    W64(BRIDGE_ATE(0), 0x123456789abcdef0ULL),
    W64(BRIDGE_ATE(77), 0x0fedcba987654321ULL),
    M32(BRIDGE_CFG0(IOC3_SLOT) + PCI_BAR0),
    M32(BRIDGE_CFG0(IOC3_SLOT) + PCI_COMMAND),
    M32(BRIDGE_REG(0x200 + 8 * IOC3_SLOT)),
    M32(BRIDGE_REG(0x070)),
    M32(BRIDGE_REG(0x030)),
    M32(BRIDGE_REG(0x038)),
    M32(BRIDGE_REG(0x028)),
    M32(BRIDGE_REG(0x048)),
    M32(BRIDGE_REG(0x0a0)),
    M32(BRIDGE_REG(0x0c0)),
    M32(BRIDGE_REG(0x108)),
    M32(BRIDGE_REG(0x118)),
    M32(BRIDGE_REG(0x120)),
    M32(BRIDGE_REG(0x128)),
    M32(BRIDGE_REG(0x148)),
    M32(BRIDGE_REG(0x230)),
    M32(BRIDGE_ATE(0) + 4),
    M32(BRIDGE_ATE_LOW(0)),
    M32(BRIDGE_ATE(77) + 4),
    M32(BRIDGE_ATE_LOW(77)),
    RT_DONE
};

static const RtStep sn0_ioc3[] = {
    W32(IOC3(0x28), 0x0007ae5e),                /* chip control */
    W32(IOC3(0x34), 1u << 5),                   /* pin control set */
    W32(IOC3(0x40000), 0x000055aa),             /* Ethernet buffer SSRAM */
    W32(IOC3(0x7fffc), 0x00001234),
    W32(IOC3(0xb0), 0),                         /* serial DMA ring */
    W32(IOC3(0xb4), 0x80100001),
    W8(IOC3(IOC3_UART_A + 7), 0x5a),            /* UART A scratch */
    M32(IOC3(0x28)),
    M32(IOC3(0x34)),
    M32(IOC3(0x40000)),
    M32(IOC3(0x7fffc)),
    M32(IOC3(0xb4)),
    M8(IOC3(IOC3_UART_A + 7)),
    RT_DONE
};

static const RtStep sn0_ioc3_eth[] = {
    W32(IOC3(0x138), 0x00005544),               /* station address */
    W32(IOC3(0x13c), 0x33221100),
    W32(IOC3(0x140), 0x01020304),               /* multicast hash */
    W32(IOC3(0x144), 0x05060708),
    W32(IOC3(0x100), 0),                        /* receive ring */
    W32(IOC3(0x104), 0x80200000),
    /* Buffer RAM parity on, then a word that stores its parity bit. */
    W32(IOC3(0xf0), 0x200),
    W32(IOC3(0x60000), 0x00000001),
    M32(IOC3(0xf0)),
    M32(IOC3(0x60000)),
    M32(IOC3(0x138)),
    M32(IOC3(0x13c)),
    M32(IOC3(0x140)),
    M32(IOC3(0x144)),
    M32(IOC3(0x104)),
    RT_DONE
};

/* Battery-backed storage below the clock registers; plain bytes. */
static const RtStep sn0_timekeeper[] = {
    W8(TIMEKEEPER(0x0000), 0xa5),
    W8(TIMEKEEPER(0x0100), 0xc3),
    W8(TIMEKEEPER(0x1234), 0x3c),
    W8(TIMEKEEPER(0x7fef), 0x96),
    M8(TIMEKEEPER(0x0000)),
    M8(TIMEKEEPER(0x0100)),
    M8(TIMEKEEPER(0x1234)),
    M8(TIMEKEEPER(0x7fef)),
    RT_DONE
};

/*
 * Node 0's ECC and directory back-door windows in Hub-special space. The
 * storage is a ROM device's backing, so it crosses in the RAM stream.
 */
#define BACKDOOR_ECC            (SPACE_HUB + 0x80000000)
#define BACKDOOR_DIR            (SPACE_HUB + 0xc0000000)

static const RtStep sn0_backdoor[] = {
    W64(BACKDOOR_ECC, 0xcafe),
    W64(BACKDOOR_DIR, 0xbabe),
    W64(BACKDOOR_ECC + 0x1238, 0x1234),
    K64(BACKDOOR_ECC),
    K64(BACKDOOR_DIR),
    K64(BACKDOOR_ECC + 0x1238),
    RT_DONE
};

/*
 * The graphics flow-control registers, reached through the Hub's
 * by-node window (HUB_BY_NODE) as the PROM does.
 */
#define HUB_BY_NODE(o)          (SPACE_IO + 0x01800000 + (o))

static const RtStep sn0_hub_gfx[] = {
    W64(HUB_BY_NODE(0x328), 0x10001400000ULL),  /* PI graphics page 1 */
    W64(HUB_BY_NODE(0x330), 0x65),              /* credit */
    W64(HUB_BY_NODE(0x338), 0x44),              /* bias */
    W64(HUB_BY_NODE(0x400148), 0x11000c),       /* IO graphics init 1 */
    M64(HUB_BY_NODE(0x328)),
    M64(HUB_BY_NODE(0x330)),
    M64(HUB_BY_NODE(0x338)),
    M64(HUB_BY_NODE(0x400148)),
    RT_DONE
};

static const RtDevice dev_sn0_hub = { "sgi-hub", sn0_hub };
static const RtDevice dev_sn0_hub_gfx = { "sgi-hub graphics", sn0_hub_gfx };
static const RtDevice dev_sn0_backdoor = { "sgi-hub back door", sn0_backdoor };
static const RtDevice dev_sn0_bridge = { "sgi-bridge", sn0_bridge, XBRIDGE };
static const RtDevice dev_sn0_ioc3 = { "sgi-ioc3", sn0_ioc3,
                                       SN0_IOC3_WINDOW };
static const RtDevice dev_sn0_ioc3_eth = { "sgi-ioc3-eth", sn0_ioc3_eth,
                                           SN0_IOC3_WINDOW };
static const RtDevice dev_sn0_timekeeper = { "m48t35", sn0_timekeeper,
                                             SN0_IOC3_WINDOW };

static const RtDevice *const origin200_devices[] = {
    &dev_sn0_hub, &dev_sn0_hub_gfx, &dev_sn0_backdoor, &dev_sn0_bridge,
    &dev_sn0_ioc3, &dev_sn0_ioc3_eth, &dev_sn0_timekeeper, NULL
};

/* ---- Origin 200 dual chassis: the second node's Hub, Bridge, IOC3 ---- */

/*
 * Node 1 answers at its NASID once node 0 has told it its number over the
 * link, the vector write the PROM sends. Its IOC3 has the same devfn as
 * node 0's on a different root bus, so the two must migrate apart.
 */
#define N1(a)                   ((a) + NODE_STRIDE)

static const RtStep sn0_node1[] = {
    W64(HUB(0x600208), 0),                      /* vector route */
    W64(HUB(0x600210), (UINT64_C(1) << 17) | (1 << 8)), /* NASID 1 */
    W64(HUB(0x600200), 0x600000 | 1),           /* write NI status */
    W64(N1(HUB(0x600100)), 0x0badc0ffee0ddf00ULL),
    W64(N1(HUB(0x4003c0)), 0x654321),
    W32(N1(XBRIDGE + BRIDGE_CFG0(IOC3_SLOT) + PCI_BAR0), IOC3_MMIO),
    W32(N1(XBRIDGE + BRIDGE_CFG0(IOC3_SLOT) + PCI_COMMAND),
        PCI_COMMAND_MEMORY),
    W32(N1(XBRIDGE + BRIDGE_REG(0x200 + 8 * IOC3_SLOT)),
        DEV_MEMORY_SPACE | (IOC3_MMIO >> 20)),
    W32(N1(XBRIDGE + BRIDGE_REG(0x070)), 0x1e2d3c4b),
    W8(N1(SN0_IOC3_WINDOW + IOC3_UART_A + 7), 0xa5),
    W32(N1(SN0_IOC3_WINDOW + 0x40000), 0x0000aa55),
    M64(N1(HUB(0x600100))),
    M64(N1(HUB(0x4003c0))),
    M32(N1(XBRIDGE + BRIDGE_CFG0(IOC3_SLOT) + PCI_BAR0)),
    M32(N1(XBRIDGE + BRIDGE_REG(0x200 + 8 * IOC3_SLOT))),
    M32(N1(XBRIDGE + BRIDGE_REG(0x070))),
    M8(N1(SN0_IOC3_WINDOW + IOC3_UART_A + 7)),
    M32(N1(SN0_IOC3_WINDOW + 0x40000)),
    RT_DONE
};

static const RtDevice dev_sn0_node1 = { "node 1 Hub, Bridge, IOC3", sn0_node1 };

static const RtDevice *const origin200_dual_devices[] = {
    &dev_sn0_hub, &dev_sn0_bridge, &dev_sn0_ioc3, &dev_sn0_node1, NULL
};

/* ---- Four-node SN0: the other Hubs and the Crossbow ---- */

/*
 * Nodes 1 to 3 are reached through one of their own processors, two per
 * node, so no NASID programming is needed. The Crossbow is widget 0
 * behind node 0.
 */
#define NODE_CPU(n)             (2 * (n))
/* Crossbow offsets are from widget 0, which the instance places. */
#define XBOW(o)                 ((o) + 4)
#define XBOW_LINK(w, o)         XBOW(0x100 + ((w) - 8) * 0x40 + (o))

static const RtStep sn0_far_hubs[] = {
    CW64(NODE_CPU(1), HUB(0x600100), 0x1111111111111111ULL),
    CW64(NODE_CPU(2), HUB(0x600100), 0x2222222222222222ULL),
    CW64(NODE_CPU(3), HUB(0x600100), 0x3333333333333333ULL),
    CW64(NODE_CPU(1), HUB(0x4003c0), 0x111111),
    CW64(NODE_CPU(3), HUB(0x400158), 0x0000000300f03333ULL),
    CM64(NODE_CPU(1), HUB(0x600100)),
    CM64(NODE_CPU(2), HUB(0x600100)),
    CM64(NODE_CPU(3), HUB(0x600100)),
    CM64(NODE_CPU(1), HUB(0x4003c0)),
    CM64(NODE_CPU(3), HUB(0x400158)),
    RT_DONE
};

static const RtStep sn0_xbow[] = {
    CW32(0, XBOW(0x28), 0x00012345),            /* packet timeout */
    CW32(0, XBOW(0x30), 0x0a050001),            /* interrupt dest upper */
    CW32(0, XBOW(0x38), 0x1234),                /* interrupt dest lower */
    CW32(0, XBOW(0x48), 0x01230a58),            /* LLP control */
    CW32(0, XBOW(0x58), 0x15),                  /* arbitration reload */
    CW32(0, XBOW(0x70), 0x000abcde),            /* NIC */
    CW32(0, XBOW_LINK(11, 0x18), 0xdeadbeef),   /* link B arbitration */
    CW32(0, XBOW_LINK(11, 0x20), 0x5eedf00d),
    CM32(0, XBOW(0x28)),
    CM32(0, XBOW(0x30)),
    CM32(0, XBOW(0x38)),
    CM32(0, XBOW(0x48)),
    CM32(0, XBOW(0x58)),
    CM32(0, XBOW(0x70)),
    CM32(0, XBOW_LINK(11, 0x18)),
    CM32(0, XBOW_LINK(11, 0x20)),
    RT_DONE
};

static const RtDevice dev_sn0_far_hubs = { "sgi-hub nodes 1-3", sn0_far_hubs };
static const RtDevice dev_sn0_xbow = { "sgi-xbow", sn0_xbow, SPACE_IO };

/* ---- InfiniteReality (KONA), widget 15 of an Onyx2 rack ---- */

/* Offsets from the widget base; the instance gives the base. */
#define KONA(o)                 (o)
#define XIO_WIDGET(n)           (SPACE_IO + ((uint64_t)(n) << 24))

static const RtStep kona[] = {
    W32(KONA(0x24), 0xdeadbeef),                /* widget control */
    W32(KONA(0x34), 0x98000),                   /* interrupt dest upper */
    W32(KONA(0x3c), 0x1800090),                 /* interrupt dest lower */
    W32(KONA(0x5c), 0x3e2210),                  /* interrupt vectors */
    W32(KONA(0x7c), 0x23),                      /* low window */
    W32(KONA(0x800210), 0x700),                 /* credit configuration */
    W32(KONA(0x800220), 0x8000),                /* reset argument */
    W32(KONA(0x8003b0), 5),                     /* loader words */
    W32(KONA(0x800328), 1),
    W32(KONA(0x100000), 0x12345678),            /* XG map RAM */
    W32(KONA(0x100000 + 4 * 0x1234), 0x1234),
    W32(KONA(0x1ffffc), 0x10000000),
    M32(KONA(0x24)),
    M32(KONA(0x34)),
    M32(KONA(0x3c)),
    M32(KONA(0x5c)),
    M32(KONA(0x7c)),
    M32(KONA(0x800210)),
    M32(KONA(0x800220)),
    M32(KONA(0x8003b0)),
    M32(KONA(0x800328)),
    M32(KONA(0x100000)),
    P32(KONA(0x100004)),                        /* left at zero */
    M32(KONA(0x100000 + 4 * 0x1234)),
    M32(KONA(0x1ffffc)),
    RT_DONE
};

static const RtDevice dev_kona = { "sgi-kona", kona, XIO_WIDGET(15) };

static const RtDevice *const onyx2_devices[] = {
    &dev_sn0_far_hubs, &dev_sn0_xbow, &dev_kona, NULL
};

/* ---- IMPACT (MGRAS), XIO slot io3 = widget 15 of an Origin 2000 ---- */

#define MGRAS(o)                (o)

static const RtStep mgras[] = {
    P32(MGRAS(0x04)),                           /* widget identity */
    P64(MGRAS(0x00)),                           /* HQ revision */
    W32(MGRAS(0x24), 0x0000300f),               /* widget control */
    W32(MGRAS(0x34), 0x12340000),               /* interrupt dest upper */
    W32(MGRAS(0x3c), 0x5678),                   /* interrupt dest lower */
    W32(MGRAS(0x011008), 0x765b900f),           /* reset argument */
    W8(MGRAS(0x073c08), 0xa7),                  /* I2C scratch */
    W32(MGRAS(0x011030), 0x11000f),             /* HQ configuration */
    W32(MGRAS(0x011038), 0xffffffff),
    W32(MGRAS(0x011060), 0x2000f000),           /* HQ vector destination */
    W32(MGRAS(0x011068), 0x9000),
    W32(MGRAS(0x011070), 0x01020304),
    W32(MGRAS(0x040000), 0x1a2b3c4d),           /* FIFO threshold 0 */
    W32(MGRAS(0x040068), 0x11111111),           /* scheduler arguments */
    W32(MGRAS(0x040078), 0x22222222),
    W32(MGRAS(0x040088), 0x33333333),
    W32(MGRAS(0x040070), 0x44444444),
    W8(MGRAS(0x071988), 0x1c),                  /* DAC configuration */
    W16(MGRAS(0x071810), 0x0600),               /* DAC register 6 */
    W8(MGRAS(0x071910), 0xe4),
    W16(MGRAS(0x070c30), (0x42 << 8) | 5),      /* colour map 5, entry */
    W32(MGRAS(0x070d18), 0x11223300),           /* 0x42; reselect so the */
    W16(MGRAS(0x070c30), (0x42 << 8) | 5),      /* model's data port reads it */
    M32(MGRAS(0x24)),
    M32(MGRAS(0x34)),
    M32(MGRAS(0x3c)),
    M32(MGRAS(0x011008)),
    M8(MGRAS(0x073c08)),
    M32(MGRAS(0x011030)),
    M32(MGRAS(0x011038)),
    M32(MGRAS(0x011060)),
    M32(MGRAS(0x011068)),
    M32(MGRAS(0x011070)),
    M32(MGRAS(0x040000)),
    M32(MGRAS(0x040068)),
    M32(MGRAS(0x040078)),
    M32(MGRAS(0x040088)),
    M32(MGRAS(0x040070)),
    M8(MGRAS(0x071988)),
    M16(MGRAS(0x071810)),
    M8(MGRAS(0x071910)),
    M32(MGRAS(0x070d18)),
    RT_DONE
};

/* A one-RE board's single PP1 XMAP, mode entry 3. */
static const RtStep mgras_xmap[] = {
    W8(MGRAS(0x071c88), 3 << 2),
    W32(MGRAS(0x071e00), 15 << 5),
    M8(MGRAS(0x071c88)),
    M32(MGRAS(0x071e00)),
    RT_DONE
};

static const RtDevice dev_mgras = { "sgi-mgras", mgras, XIO_WIDGET(15) };
static const RtDevice dev_mgras_xmap = { "sgi-mgras XMAP", mgras_xmap,
                                       XIO_WIDGET(15) };

/*
 * A two-RE board has four PP1s, each with its own XMAP register file
 * behind PP1SELECT. Give each one different contents and read each back
 * through the selector, leaving PP1 1 selected.
 */
#define MGRAS_PP1_SELECT        MGRAS(0x071c08)
#define MGRAS_XMAP_INDEX        MGRAS(0x071c88)
#define PP1(n)                  ((n) << 1)

static const RtStep mgras_pp1s[] = {
    W8(MGRAS_PP1_SELECT, PP1(0)),
    W8(MGRAS_XMAP_INDEX, 0),
    W32(MGRAS(0x071e00), 0x100),                 /* main mode */
    W32(MGRAS(0x071d80), 0x10),                  /* buffer select */
    W32(MGRAS(0x071e80), 0x20),                  /* overlay mode */
    W32(MGRAS(0x071d00), 0x1000),                /* configuration */
    W32(MGRAS(0x071d08), 0x2000),
    W8(MGRAS_XMAP_INDEX, 4),
    W8(MGRAS_PP1_SELECT, PP1(1)),
    W8(MGRAS_XMAP_INDEX, 0),
    W32(MGRAS(0x071e00), 0x101),
    W32(MGRAS(0x071d80), 0x11),
    W32(MGRAS(0x071e80), 0x21),
    W32(MGRAS(0x071d00), 0x1001),
    W32(MGRAS(0x071d08), 0x2001),
    W8(MGRAS_XMAP_INDEX, 8),
    W8(MGRAS_PP1_SELECT, PP1(2)),
    W8(MGRAS_XMAP_INDEX, 0),
    W32(MGRAS(0x071e00), 0x102),
    W32(MGRAS(0x071d80), 0x12),
    W32(MGRAS(0x071e80), 0x22),
    W32(MGRAS(0x071d00), 0x1002),
    W32(MGRAS(0x071d08), 0x2002),
    W8(MGRAS_XMAP_INDEX, 12),
    W8(MGRAS_PP1_SELECT, PP1(3)),
    W8(MGRAS_XMAP_INDEX, 0),
    W32(MGRAS(0x071e00), 0x103),
    W32(MGRAS(0x071d80), 0x13),
    W32(MGRAS(0x071e80), 0x23),
    W32(MGRAS(0x071d00), 0x1003),
    W32(MGRAS(0x071d08), 0x2003),
    W8(MGRAS_XMAP_INDEX, 16),
    W8(MGRAS_PP1_SELECT, PP1(1)),
    M32(MGRAS_PP1_SELECT),
    S8(MGRAS_PP1_SELECT, PP1(0)),
    M8(MGRAS_XMAP_INDEX),
    S8(MGRAS_XMAP_INDEX, 0),
    M32(MGRAS(0x071e00)),
    M32(MGRAS(0x071d80)),
    M32(MGRAS(0x071e80)),
    M32(MGRAS(0x071d00)),
    M32(MGRAS(0x071d08)),
    S8(MGRAS_XMAP_INDEX, 4),
    S8(MGRAS_PP1_SELECT, PP1(1)),
    M8(MGRAS_XMAP_INDEX),
    S8(MGRAS_XMAP_INDEX, 0),
    M32(MGRAS(0x071e00)),
    M32(MGRAS(0x071d80)),
    M32(MGRAS(0x071e80)),
    M32(MGRAS(0x071d00)),
    M32(MGRAS(0x071d08)),
    S8(MGRAS_XMAP_INDEX, 8),
    S8(MGRAS_PP1_SELECT, PP1(2)),
    M8(MGRAS_XMAP_INDEX),
    S8(MGRAS_XMAP_INDEX, 0),
    M32(MGRAS(0x071e00)),
    M32(MGRAS(0x071d80)),
    M32(MGRAS(0x071e80)),
    M32(MGRAS(0x071d00)),
    M32(MGRAS(0x071d08)),
    S8(MGRAS_XMAP_INDEX, 12),
    S8(MGRAS_PP1_SELECT, PP1(3)),
    M8(MGRAS_XMAP_INDEX),
    S8(MGRAS_XMAP_INDEX, 0),
    M32(MGRAS(0x071e00)),
    M32(MGRAS(0x071d80)),
    M32(MGRAS(0x071e80)),
    M32(MGRAS(0x071d00)),
    M32(MGRAS(0x071d08)),
    S8(MGRAS_XMAP_INDEX, 16),
    S8(MGRAS_PP1_SELECT, PP1(1)),
    RT_DONE
};

static const RtDevice dev_mgras_pp1s = { "sgi-mgras PP1 XMAPs", mgras_pp1s,
                                       XIO_WIDGET(15) };

static const RtDevice *const origin2000_mgras_devices[] = {
    &dev_sn0_far_hubs, &dev_sn0_xbow, &dev_mgras, &dev_mgras_xmap, NULL
};

static const RtDevice *const origin2000_mgras_two_re_devices[] = {
    &dev_sn0_far_hubs, &dev_sn0_xbow, &dev_mgras, &dev_mgras_pp1s, NULL
};

/* ---- Octane (IP30): IMPACT SI at crosstalk widget 0xb, main memory ---- */

/* The SI's long window, and its short alias in the low 512 MB. */
#define OCTANE_SI_LONG          UINT64_C(0xb000000000)
#define OCTANE_SI_SHORT         UINT64_C(0x1b000000)
#define OCTANE_HEART(o)         (UINT64_C(0x0ff00000) + (o))
#define OCTANE_RAM              UINT64_C(0x20000000)

static const RtStep octane_si_alias[] = {
    M32(OCTANE_SI_SHORT + 0x24),
    M32(OCTANE_SI_SHORT + 0x040000),
    RT_DONE
};

/* The Heart's memory-bank configuration, set by the launch. */
static const RtStep octane_heart_banks[] = {
    P32(OCTANE_HEART(0x20)),
    P32(OCTANE_HEART(0x24)),
    RT_DONE
};

/*
 * The Heart's memory and flow-control registers, its timer compare and
 * interrupt masks, and its own widget registers. The bank descriptors and
 * mode stay at reset: they remap RAM.
 */
#define OCTANE_HEART_WIDGET(o)  (UINT64_C(0x18000000) + (o))

static const RtStep octane_heart[] = {
    W64(OCTANE_HEART(0x08), 0x21),              /* SDRAM mode */
    W64(OCTANE_HEART(0x10), 0x84000),           /* memory refresh */
    W64(OCTANE_HEART(0x18), 0x13579bdf),        /* memory arbitration */
    W64(OCTANE_HEART(0x50), 0x0123456789abcdefULL), /* flow cells */
    W64(OCTANE_HEART(0x58), 0xfedcba9876543210ULL),
    W64(OCTANE_HEART(0xb0), 0x123456789abca55aULL), /* clock divider */
    W64(OCTANE_HEART(0x30000), 0x01573eca),     /* timer compare */
    W64(OCTANE_HEART(0x10000), 0x5a5a),         /* interrupt masks */
    W64(OCTANE_HEART(0x10018), 0xa5a5),
    W32(OCTANE_HEART_WIDGET(0x24), 0x5a420ff0), /* widget control */
    W64(OCTANE_HEART_WIDGET(0x68), 0x3ff62c07), /* widget error mask */
    M64(OCTANE_HEART(0x08)),
    M64(OCTANE_HEART(0x10)),
    M64(OCTANE_HEART(0x18)),
    M64(OCTANE_HEART(0x50)),
    M64(OCTANE_HEART(0x58)),
    M64(OCTANE_HEART(0xb0)),
    M64(OCTANE_HEART(0x30000)),
    M64(OCTANE_HEART(0x10000)),
    M64(OCTANE_HEART(0x10018)),
    M32(OCTANE_HEART_WIDGET(0x24)),
    M64(OCTANE_HEART_WIDGET(0x68)),
    RT_DONE
};

/* The Octane Crossbow, widget 0. */
static const RtStep octane_xbow[] = {
    W32(XBOW(0x28), 0x00012345),                /* packet timeout */
    W32(XBOW(0x30), 0x0a050001),                /* interrupt dest upper */
    W32(XBOW(0x38), 0x1234),                    /* interrupt dest lower */
    W32(XBOW(0x48), 0x01230a58),                /* LLP control */
    W32(XBOW(0x58), 0x2a),                      /* arbitration reload */
    W32(XBOW(0x70), 0x1c),                      /* NIC */
    W32(XBOW_LINK(8, 0x18), 0x11223344),        /* link 8 arbitration */
    W32(XBOW_LINK(8, 0x20), 0x55667788),
    M32(XBOW(0x28)),
    M32(XBOW(0x30)),
    M32(XBOW(0x38)),
    M32(XBOW(0x48)),
    M32(XBOW(0x58)),
    M32(XBOW(0x70)),
    M32(XBOW_LINK(8, 0x18)),
    M32(XBOW_LINK(8, 0x20)),
    RT_DONE
};

#define OCTANE_XBOW             UINT64_C(0x10000000)
#define OCTANE_BRIDGE           UINT64_C(0x1f000000)

static const RtDevice dev_octane_si = { "sgi-mgras", mgras, OCTANE_SI_LONG };
static const RtDevice dev_octane_heart = { "sgi-heart", octane_heart };
static const RtDevice dev_octane_xbow = { "sgi-xbow", octane_xbow,
                                          OCTANE_XBOW };
static const RtDevice dev_octane_bridge = { "sgi-bridge", sn0_bridge,
                                            OCTANE_BRIDGE };
static const RtDevice dev_octane_ioc3 = { "sgi-ioc3", sn0_ioc3,
                                          OCTANE_BRIDGE + 0x600000 };
static const RtDevice dev_octane_ioc3_eth = { "sgi-ioc3-eth", sn0_ioc3_eth,
                                              OCTANE_BRIDGE + 0x600000 };
static const RtDevice dev_octane_si_xmap = { "sgi-mgras XMAP", mgras_xmap,
                                           OCTANE_SI_LONG };
static const RtDevice dev_octane_si_alias = { "sgi-mgras short window",
                                            octane_si_alias };
static const RtDevice dev_octane_heart_banks = { "sgi-heart",
                                               octane_heart_banks };

static const RtDevice *const octane_si_devices[] = {
    &dev_octane_heart, &dev_octane_xbow, &dev_octane_bridge,
    &dev_octane_ioc3, &dev_octane_ioc3_eth, &dev_octane_si,
    &dev_octane_si_xmap, &dev_octane_si_alias, NULL
};

static const RtDevice *const octane_banks_devices[] = {
    &dev_octane_heart_banks, NULL
};

/* Test-owned R12000 configuration, as octane-test.c gives it. */
#define OCTANE2_CPU "R12000,r12000-prid=0xe24,r12000-fpu-id=0x7654," \
    "r12000-reset-mode=0x1aa683,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=32"

/* The low 16 KB alias of main memory, and a window above it. */
static const RtRam octane_ram[] = {
    { 0, 0x4000 },
    { OCTANE_RAM + 0x100000, 0x1000 },
    { 0, 0 }
};

/* Both sides of the boundary between two 64 MB banks, and the top. */
static const RtRam octane_banks_ram[] = {
    { OCTANE_RAM + 0x4000, 8 },
    { OCTANE_RAM + 0x4000000 - 8, 16 },
    { OCTANE_RAM + 0x8000000 - 8, 8 },
    { 0, 0 }
};

/* ---- Origin 300 (IP45): Bedrock and the IO8's IOC3 behind an XBridge ---- */

#define BEDROCK_REGS(o)         (SPACE_HUB + 0x10000000 + (o))
#define BEDROCK_PI1(o)          (SPACE_IO + 0x01800000 + 0x200000 + (o))
#define BEDROCK_II(o)           (SPACE_IO + 0x01800000 + (o))
#define IO8_WIDGET              XIO_WIDGET(0xf)
#define IO8_IOC3_SLOT           4
#define IO8_CFG(o)              (IO8_WIDGET + 0x20000 + \
                                 IO8_IOC3_SLOT * 0x1000 + (o))
#define IO8_IOC3_WINDOW         (IO8_WIDGET + 0x800000)

/*
 * Banks 2 and 3 populated, which puts RAM at physical 0 and at the second
 * 1 GB bank through aliases the destination rebuilds after loading.
 */
#define BEDROCK_MD(o)           (SPACE_IO + 0x01000000 + 0x780000 + (o))
#define BEDROCK_BANK_SIZE       UINT64_C(0x40000000)

static const RtStep bedrock[] = {
    W64(BEDROCK_MD(0x8), (UINT64_C(2) << 32) | 0x3300),
    W64(BEDROCK_REGS(0xd0), 0x5d),              /* LED, slice 2 */
    W64(BEDROCK_PI1(0x328), 0x10001400000ULL),  /* PI graphics page 1 */
    W64(BEDROCK_PI1(0x330), 0x64),              /* credit */
    W64(BEDROCK_PI1(0x338), 0x44),              /* bias */
    W64(BEDROCK_PI1(0x20068), 1),               /* credit mode */
    W64(BEDROCK_II(0x400148), 0x1001b),         /* II graphics map 1 */
    P64(BEDROCK_REGS(0x4000000)),               /* board identity */
    M64(BEDROCK_MD(0x8)),
    M64(BEDROCK_REGS(0xd0)),
    M64(BEDROCK_PI1(0x328)),
    M64(BEDROCK_PI1(0x330)),
    M64(BEDROCK_PI1(0x338)),
    M64(BEDROCK_PI1(0x20068)),
    M64(BEDROCK_II(0x400148)),
    RT_DONE
};

/*
 * The rest of the Bedrock: processor interface 0, the local block's
 * scratch and configuration, the IO interface, the memory/directory
 * unit's plain storage, the crossbar and the network interface with its
 * route tables. Registers that gate access paths, arm timers, launch
 * transfers or raise the error line stay at reset.
 */
#define BEDROCK(o)              (SPACE_IO + 0x01000000 + (o))
#define BEDROCK_LB(o)           BEDROCK(0x600000 + (o))
#define BEDROCK_IO(o)           BEDROCK(0x400000 + (o))
#define BEDROCK_XB(o)           BEDROCK(0x700000 + (o))
#define BEDROCK_NI(o)           BEDROCK(0x680000 + (o))

static const RtStep bedrock_blocks[] = {
    W64(BEDROCK(0x030), 0x5a),                  /* PI request timeout */
    W64(BEDROCK(0x038), 0x123456),
    W64(BEDROCK(0x0e8), 0x8),                   /* CC pending mask */
    W64(BEDROCK(0x490), 0x2a),                  /* error check enable */
    W64(BEDROCK(0x4b8), 0x5a5a5),               /* NACK compare */
    W64(BEDROCK(0x4c0), 0x15),                  /* spool mask */
    W64(BEDROCK_LB(0x4020), 0x8000000000000001ULL), /* region present */
    W64(BEDROCK_LB(0x4100), 0x2a),              /* debug select */
    W64(BEDROCK_LB(0x4200), 0x113ff3ff1ULL),    /* RTC local */
    W64(BEDROCK_LB(0x8000), 0x1111222233334444ULL), /* scratch 0 */
    W64(BEDROCK_LB(0x8010), 0x8badf00d12345678ULL), /* scratch 2 */
    W64(BEDROCK_LB(0x8020), 1),                 /* scratch 4 */
    W64(BEDROCK_IO(0x158), 0x0000000200f01111ULL), /* IO scratch 1 */
    W64(BEDROCK_IO(0x190), 0x1822),             /* ITTE entry 6 */
    W64(BEDROCK_IO(0x1e0), 0x2a5a5a5),          /* credit timeout */
    W64(BEDROCK_IO(0x1f0), 0x0000012301005a5aULL), /* tail timeout */
    W64(BEDROCK_IO(0x3b8), 0x5a),               /* request timeout */
    W64(BEDROCK_IO(0x3c0), 0x123456),           /* request prescaler */
    W64(BEDROCK_MD(0x10), 0xa5),                /* MD arbitration */
    W64(BEDROCK_MD(0x10000), 0x0000123456787ff3ULL), /* perf select */
    W64(BEDROCK_XB(0x008), 0x76543210),         /* slow grant */
    W64(BEDROCK_XB(0x098), 0x1234),             /* debug data */
    W64(BEDROCK_NI(0x10), 1),                   /* reset enable */
    W64(BEDROCK_NI(0x18), 5),                   /* diagnostic parms */
    W64(BEDROCK_NI(0x30), 0x00a5a5a5),          /* port parms */
    W64(BEDROCK_NI(0x38), 0x5aa5),              /* channel age */
    W64(BEDROCK_NI(0x1000), 0x15),              /* local route 0 */
    W64(BEDROCK_NI(0x13f8), 0x19),              /* local route 127 */
    W64(BEDROCK_NI(0x2000), 0x17),              /* global route */
    M64(BEDROCK(0x030)),
    M64(BEDROCK(0x038)),
    M64(BEDROCK(0x0e8)),
    M64(BEDROCK(0x490)),
    M64(BEDROCK(0x4b8)),
    M64(BEDROCK(0x4c0)),
    M64(BEDROCK_LB(0x4020)),
    M64(BEDROCK_LB(0x4100)),
    M64(BEDROCK_LB(0x4200)),
    M64(BEDROCK_LB(0x8000)),
    K64(BEDROCK_LB(0x8010)),
    M64(BEDROCK_LB(0x8020)),
    M64(BEDROCK_IO(0x158)),
    M64(BEDROCK_IO(0x190)),
    M64(BEDROCK_IO(0x1e0)),
    M64(BEDROCK_IO(0x1f0)),
    M64(BEDROCK_IO(0x3b8)),
    M64(BEDROCK_IO(0x3c0)),
    M64(BEDROCK_MD(0x10)),
    M64(BEDROCK_MD(0x10000)),
    M64(BEDROCK_XB(0x008)),
    M64(BEDROCK_XB(0x098)),
    M64(BEDROCK_NI(0x10)),
    M64(BEDROCK_NI(0x18)),
    M64(BEDROCK_NI(0x30)),
    M64(BEDROCK_NI(0x38)),
    M64(BEDROCK_NI(0x1000)),
    M64(BEDROCK_NI(0x13f8)),
    M64(BEDROCK_NI(0x2000)),
    RT_DONE
};

/* The L1 controller's UART, through the Hub-special window. */
static const RtStep l1_uart[] = {
    W64(SPACE_HUB + 0x30000000 + 0xb8, 0x5a),   /* scratch */
    M64(SPACE_HUB + 0x30000000 + 0xb8),
    RT_DONE
};

/* XBridge head F's widget registers, beside the IOC3 slot's device. */
static const RtStep xbridge_head[] = {
    W32(BRIDGE_REG(0x028), 0x00012345),         /* request timeout */
    W32(BRIDGE_REG(0x030), 0x000a1234),         /* interrupt dest upper */
    W32(BRIDGE_REG(0x038), 0x5a5a5a5a),         /* interrupt dest lower */
    W32(BRIDGE_REG(0x048), 0x01230a58),         /* link config */
    W32(BRIDGE_REG(0x070), 0x00a5a5a5),
    W32(BRIDGE_REG(0x0a0), 0x0003a547),         /* arbitration */
    W32(BRIDGE_REG(0x0c0), 0x000a02bc),         /* bus timeout */
    W32(BRIDGE_REG(0x118), 0xa5),               /* interrupt mode */
    W32(BRIDGE_REG(0x120), 0x00ab4c91),         /* interrupt device */
    W32(BRIDGE_REG(0x128), 0x5a),               /* host error field */
    W32(BRIDGE_REG(0x230), 0x0a4a5a5a),         /* device 6 */
    M32(BRIDGE_REG(0x028)),
    M32(BRIDGE_REG(0x030)),
    M32(BRIDGE_REG(0x038)),
    M32(BRIDGE_REG(0x048)),
    M32(BRIDGE_REG(0x070)),
    M32(BRIDGE_REG(0x0a0)),
    M32(BRIDGE_REG(0x0c0)),
    M32(BRIDGE_REG(0x118)),
    M32(BRIDGE_REG(0x120)),
    M32(BRIDGE_REG(0x128)),
    M32(BRIDGE_REG(0x230)),
    RT_DONE
};

static const RtStep io8_xbridge[] = {
    W32(IO8_CFG(PCI_BAR0), 0x10000000),
    W32(IO8_CFG(PCI_COMMAND), 0x0006),
    W32(IO8_WIDGET + 0x200 + 8 * IO8_IOC3_SLOT + 4, DEV_MEMORY_SPACE | 0x100),
    M32(IO8_CFG(PCI_BAR0)),
    M32(IO8_CFG(PCI_COMMAND)),
    M32(IO8_WIDGET + 0x200 + 8 * IO8_IOC3_SLOT + 4),
    RT_DONE
};

static const RtDevice dev_bedrock = { "sgi-bedrock", bedrock };
static const RtDevice dev_bedrock_blocks = { "sgi-bedrock blocks",
                                             bedrock_blocks };
static const RtDevice dev_l1_uart = { "sgi-l1-uart", l1_uart };
static const RtDevice dev_xbridge_head = { "sgi-xbridge head F", xbridge_head,
                                           IO8_WIDGET };
static const RtDevice dev_io8_xbridge = { "sgi-xbridge", io8_xbridge };
static const RtDevice dev_io8_ioc3 = { "sgi-ioc3", sn0_ioc3, IO8_IOC3_WINDOW };
static const RtDevice dev_io8_timekeeper = { "m48t35", sn0_timekeeper,
                                             IO8_IOC3_WINDOW };

static const RtRam bedrock_ram[] = {
    { 0x00100000, 0x10000 },
    { BEDROCK_BANK_SIZE + 0x1000, 0x1000 },
    { 0, 0 }
};

static const RtDevice *const origin300_devices[] = {
    &dev_bedrock, &dev_bedrock_blocks, &dev_l1_uart, &dev_xbridge_head,
    &dev_io8_xbridge, &dev_io8_ioc3, &dev_io8_timekeeper, NULL
};

/* ---- VPro (Odyssey) on the Origin 300's XIO connector, widget 0xb ---- */

static const RtStep odyssey[] = {
    W32(0x24, 0x0f00320b),                      /* widget control */
    W32(0x2c, 0xaaaaaaaa),                      /* request timeout */
    W32(0x34, 0x00000001),                      /* interrupt dest upper */
    W32(0x3c, 0x23450000),                      /* interrupt dest lower */
    W32(0x1070, 0x55555555),                    /* flow-control config */
    W32(0x107c, 0x5a5a5a5a),                    /* software sync */
    W32(0x1078, 0xaaaaaaaa),                    /* buzz 1 to 6 */
    W32(0x1088, 0x55555555),
    W32(0x1098, 0xaaaaaaaa),
    W32(0x10a8, 0x55555555),
    W32(0x10b8, 0xa5a5a5a5),
    W32(0x10c0, 0x5a5a5a5a),
    M32(0x24),
    M32(0x2c),
    M32(0x34),
    M32(0x3c),
    M32(0x1070),
    M32(0x107c),
    M32(0x1078),
    M32(0x1088),
    M32(0x1098),
    M32(0x10a8),
    M32(0x10b8),
    M32(0x10c0),
    RT_DONE
};

static const RtDevice dev_odyssey = { "sgi-odyssey", odyssey,
                                      XIO_WIDGET(0xb) };

static const RtDevice *const origin300_vpro_devices[] = {
    &dev_bedrock, &dev_odyssey, NULL
};


static const RtRam node0_ram[] = {
    { 0x00100000, 0x10000 },
    { 0x02000000 - 0x1000, 0x1000 },
    { 0, 0 }
};

#define ORIGIN2000_MGRAS(board, devices) \
    { "origin2000-mgras-" board, "origin2000", "sgi-mgras", \
      "-M origin2000 -smp 8 -device sgi-mgras,slot=io3,product=" board, \
      0, devices, node0_ram }
#define ONE_RE                  origin2000_mgras_devices
#define TWO_RE                  origin2000_mgras_two_re_devices

static const RtMachine machines[] = {
    { "origin200", "origin200", NULL, "-M origin200", 0,
      origin200_devices, node0_ram },
    { "origin200-dual", "origin200", NULL,
      "-M origin200,topology=origin200-dual -m 128M -smp 4", 0,
      origin200_dual_devices, node0_ram },
    { "onyx2", "onyx2", NULL, "-M onyx2", 0, onyx2_devices, node0_ram },
    ORIGIN2000_MGRAS("si", ONE_RE),
    ORIGIN2000_MGRAS("ssi", TWO_RE),
    ORIGIN2000_MGRAS("esi", ONE_RE),
    ORIGIN2000_MGRAS("essi", TWO_RE),
    ORIGIN2000_MGRAS("si-t", ONE_RE),
    ORIGIN2000_MGRAS("esi-t", ONE_RE),
    ORIGIN2000_MGRAS("mxi", TWO_RE),
    ORIGIN2000_MGRAS("emxi", TWO_RE),
    { "octane-mgras-si", "octane", "sgi-mgras",
      "-M octane -m 128M -cpu R10000 "
      "-device sgi-mgras,slot=widget-11,product=si", 4,
      octane_si_devices, octane_ram },
    { "octane2-mgras-si", "octane2", "sgi-mgras",
      "-M octane2 -m 256M -cpu " OCTANE2_CPU
      " -device sgi-mgras,slot=widget-11,product=si", 4,
      octane_si_devices, octane_ram },
    { "octane-two-banks", "octane", NULL,
      "-M octane,bank0-size=67108864,bank1-size=67108864 -m 128M "
      "-cpu R10000 -vga none", 4, octane_banks_devices, octane_banks_ram },
    { "origin300", "origin300", NULL, "-M origin300 -cpu R14000 -smp 2 -m 512M",
      PROM_SIZE, origin300_devices, bedrock_ram },
    { "origin300-vpro", "origin300", "sgi-odyssey",
      "-M origin300,topology=origin300-v12-direct -cpu R14000 -smp 2 -m 512M",
      PROM_SIZE, origin300_vpro_devices, bedrock_ram },
    { NULL }
};

/* ---- The generic round trip ---- */

static uint64_t rt_read(QTestState *s, const RtStep *p, uint64_t base)
{
    uint64_t addr = base + p->addr;
    uint8_t bytes[8];

    if (p->via) {
        g_assert_true(qtest_memread_cpu(s, p->via - 1, addr, bytes,
                                        p->width));
        return p->width == 8 ? ldq_be_p(bytes) : ldl_be_p(bytes);
    }
    switch (p->width) {
    case 1:
        return qtest_readb(s, addr);
    case 2:
        return qtest_readw(s, addr);
    case 4:
        return qtest_readl(s, addr);
    case 8:
        return qtest_readq(s, addr);
    }
    g_assert_not_reached();
}

static void rt_write(QTestState *s, const RtStep *p, uint64_t base)
{
    uint64_t addr = base + p->addr;
    uint8_t bytes[8];

    if (p->via) {
        if (p->width == 8) {
            stq_be_p(bytes, p->value);
        } else {
            stl_be_p(bytes, p->value);
        }
        g_assert_true(qtest_memwrite_cpu(s, p->via - 1, addr, bytes,
                                         p->width));
        return;
    }
    switch (p->width) {
    case 1:
        qtest_writeb(s, addr, p->value);
        return;
    case 2:
        qtest_writew(s, addr, p->value);
        return;
    case 4:
        qtest_writel(s, addr, p->value);
        return;
    case 8:
        qtest_writeq(s, addr, p->value);
        return;
    }
    g_assert_not_reached();
}

static bool rt_is_probe(const RtStep *p)
{
    return p->kind == RT_PROBE || p->kind == RT_MOVED || p->kind == RT_KEPT;
}

/* Read every probe of every device, in table order. */
static GArray *rt_read_probes(QTestState *s, const RtMachine *m)
{
    GArray *values = g_array_new(false, false, sizeof(uint64_t));

    for (const RtDevice *const *d = m->devices; *d; d++) {
        for (const RtStep *p = (*d)->steps; p->kind != RT_END; p++) {
            if (p->kind == RT_SELECT) {
                rt_write(s, p, (*d)->base);
            } else if (rt_is_probe(p)) {
                uint64_t v = rt_read(s, p, (*d)->base);

                g_array_append_val(values, v);
            }
        }
    }
    return values;
}

/*
 * Compare @got against @want probe by probe. With @differ, require every
 * RT_MOVED and RT_KEPT probe to differ instead (used against the reset
 * values). With @at_reset, skip the RT_KEPT probes: the model does not
 * clear them on system reset.
 */
static void rt_compare(const RtMachine *m, GArray *want, GArray *got,
                       const char *what, bool differ, bool at_reset)
{
    unsigned i = 0, bad = 0;

    for (const RtDevice *const *d = m->devices; *d; d++) {
        for (const RtStep *p = (*d)->steps; p->kind != RT_END; p++) {
            uint64_t w, g;

            if (!rt_is_probe(p)) {
                continue;
            }
            w = g_array_index(want, uint64_t, i);
            g = g_array_index(got, uint64_t, i);
            i++;
            if (at_reset && p->kind == RT_KEPT) {
                continue;
            }
            if (differ ? (p->kind != RT_PROBE && w == g) : w != g) {
                g_test_message("%s: %s %s 0x%" PRIx64 " (%u bytes): "
                               "0x%" PRIx64 " vs 0x%" PRIx64,
                               m->name, what, (*d)->name, (*d)->base + p->addr,
                               p->width,
                               w, g);
                bad++;
            }
        }
    }
    g_assert_cmpuint(bad, ==, 0);
}

static uint8_t rt_ram_pattern(uint64_t addr)
{
    return (addr * 0x9e3779b1u) >> 24 ^ addr;
}

static void rt_fill_ram(QTestState *s, const RtMachine *m)
{
    for (const RtRam *r = m->ram; r && r->size; r++) {
        g_autofree uint8_t *buf = g_malloc(r->size);

        for (uint32_t k = 0; k < r->size; k++) {
            buf[k] = rt_ram_pattern(r->addr + k);
        }
        qtest_memwrite(s, r->addr, buf, r->size);
    }
}

/* Run on the source too, so a window that is not RAM fails as such. */
static void rt_check_ram(QTestState *s, const RtMachine *m)
{
    for (const RtRam *r = m->ram; r && r->size; r++) {
        g_autofree uint8_t *buf = g_malloc(r->size);

        qtest_memread(s, r->addr, buf, r->size);
        for (uint32_t k = 0; k < r->size; k++) {
            if (buf[k] != rt_ram_pattern(r->addr + k)) {
                g_test_message("%s: RAM 0x%" PRIx64 " does not hold the "
                               "workload's pattern", m->name, r->addr + k);
                g_assert_cmphex(buf[k], ==, rt_ram_pattern(r->addr + k));
            }
        }
    }
}

static char *rt_erased_prom(size_t size)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(size);
    char *path = NULL;
    int fd = g_file_open_tmp("sgi-roundtrip-prom-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, size), ==, (ssize_t)size);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static void test_round_trip(const void *data)
{
    const RtMachine *m = data;
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-roundtrip-XXXXXX", &error);
    g_autofree char *uri = NULL, *prom = NULL, *bios = NULL;
    g_autoptr(GArray) reset = NULL, before = NULL, again = NULL;
    g_autoptr(GArray) after = NULL, rt_reset = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    if (m->prom_size) {
        prom = rt_erased_prom(m->prom_size);
        bios = g_strdup_printf(" -bios %s", prom);
    }
    from = qtest_initf("%s%s -S", m->args, bios ? bios : "");
    to = qtest_initf("%s%s -S -incoming defer", m->args, bios ? bios : "");

    reset = rt_read_probes(to, m);

    for (const RtDevice *const *d = m->devices; *d; d++) {
        for (const RtStep *p = (*d)->steps; p->kind != RT_END; p++) {
            if (p->kind == RT_WRITE) {
                rt_write(from, p, (*d)->base);
            }
        }
    }
    rt_fill_ram(from, m);
    rt_check_ram(from, m);

    before = rt_read_probes(from, m);
    again = rt_read_probes(from, m);
    rt_compare(m, before, again, "unstable source read", false, false);
    rt_compare(m, before, reset, "workload left at reset", true, false);

    qtest_qmp_assert_success(to, "{ 'execute': 'migrate-incoming',"
                             " 'arguments': { 'uri': %s } }", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");

    after = rt_read_probes(to, m);
    rt_compare(m, before, after, "destination differs", false, false);
    rt_check_ram(to, m);

    /*
     * State a load restores but a reset does not clear survives a reset

     */
    qtest_system_reset(to);
    rt_reset = rt_read_probes(to, m);
    rt_compare(m, reset, rt_reset, "not at reset after load and reset", false,
               true);

    qtest_quit(to);
    qtest_quit(from);
    if (prom) {
        unlink(prom);
    }
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (const RtMachine *m = machines; m->name; m++) {
        g_autofree char *path = NULL;

        if (!qtest_has_machine(m->machine) ||
            (m->device && !qtest_has_device(m->device))) {
            continue;
        }
        path = g_strdup_printf("sgi/migration-round-trip/%s", m->name);
        qtest_add_data_func(path, m, test_round_trip);
    }
    return g_test_run();
}
