/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Bridge — the crosstalk-to-PCI host bridge, as far as its register face
 * goes.
 *
 * The chip is a crosstalk widget on one side and a PCI host bridge on the
 * other. This file builds the widget side's local register block and the PCI
 * root bus; the device windows, the configuration-space window, the address
 * translation table and the interrupt path arrive on top of it.
 *
 * Two things about the register face have to be right before anything else is
 * worth writing, and both are easy to get wrong in a way that looks fine.
 *
 * The first is where a register is. Every register in this chip is 32 bits or
 * narrower and sits in data bits 31:0 of an eight-byte slot, reachable only by
 * a crosstalk double-word packet with four data enables — a 32-bit access. On
 * this big-endian machine that puts the data at the slot's offset plus four,
 * which is the address Linux's own header names, while OpenBSD names the slot
 * and folds the four bytes in when it accesses it. The two sources look four
 * bytes apart everywhere and are not: they name the same registers. A model
 * that serves both halves of a slot would satisfy every guest and would be
 * wrong about where the registers are.
 *
 * The second is what a register holds before anything writes it. This chip's
 * reset state is far more non-zero than a reader would guess, and remembering
 * two exceptions is not enough: the control register alone comes up with seven
 * fields set. A Bridge that reset it to zero would report widget 0 and
 * little-endian byte order, and the guest would fail somewhere a long way from
 * here. So the reset value is not something this file lets an implementer
 * forget: a register's decode, its reset value and the bits a guest may change
 * all come from one table below, and adding a register without a reset value is
 * not something the table can express.
 *
 * Everything the chip does not define raises a bus error rather than answering
 * zero. A register file that answers plausibly for registers nobody wrote is
 * how a missing mechanism stays invisible until a guest fails elsewhere.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"

/*
 * The data half of an eight-byte register slot. Every access this model serves
 * is at a slot's offset plus this, and the other half of the slot holds no
 * register — which is deliberate rather than an omission, because it is the
 * address a reader lands on by forgetting the plus-four, and making it faulty
 * rather than plausible turns that mistake into a message instead of a zero.
 */
#define SGI_BRIDGE_REG_DATA     4

/* Common crosstalk widget registers. */
#define BRIDGE_IDENTIFICATION   0x000
#define BRIDGE_STATUS           0x008
#define BRIDGE_ERR_UPPER        0x010
#define BRIDGE_ERR_LOWER        0x018
#define BRIDGE_CONTROL          0x020
#define BRIDGE_REQUEST_TIMEOUT  0x028
#define BRIDGE_INT_DEST_UPPER   0x030
#define BRIDGE_INT_DEST_LOWER   0x038
#define BRIDGE_ERR_CMD_WORD     0x040
#define BRIDGE_LINK_CONFIG      0x048
#define BRIDGE_TARGET_FLUSH     0x050

/* Registers this chip adds to the common block. */
#define BRIDGE_AUX_ERR_CMD_WORD 0x058
#define BRIDGE_RESP_ERR_UPPER   0x060
#define BRIDGE_RESP_ERR_LOWER   0x068
#define BRIDGE_TEST_PIN_CONTROL 0x070
#define BRIDGE_DIRECT_MAP       0x080
#define BRIDGE_SSRAM_PARITY_ERR 0x090
#define BRIDGE_ARBITRATION      0x0a0
#define BRIDGE_BUS_TIMEOUT      0x0c0
#define BRIDGE_PCI_TYPE1_CONFIG 0x0c8
#define BRIDGE_PCI_ERR_UPPER    0x0d0
#define BRIDGE_PCI_ERR_LOWER    0x0d8

/* The per-slot registers, eight of each. */
#define BRIDGE_DEVICE(n)        (0x200 + SGI_BRIDGE_REG_SLOT * (n))
#define BRIDGE_DEVICE_FLUSH(n)  (0x240 + SGI_BRIDGE_REG_SLOT * (n))

/*
 * PCI type 0 configuration space. Eight devices 4 KB apart, each holding eight
 * functions 256 bytes apart, which is how both guests compute the address:
 * Linux indexes a device array and then a function array, and OpenBSD adds the
 * device shifted left twelve to the function shifted left eight.
 */
#define BRIDGE_CFG0_BASE        0x020000
#define BRIDGE_CFG0_SIZE        (32 * KiB)
#define BRIDGE_CFG0_DEVICE(a)   (((a) >> 12) & 7)
#define BRIDGE_CFG0_FUNCTION(a) (((a) >> 8) & 7)
#define BRIDGE_CFG0_REGISTER    0xff

/* Read response buffer allocation, which this model keeps as storage. */
#define BRIDGE_RRB_EVEN         0x280
#define BRIDGE_RRB_ODD          0x288
#define BRIDGE_RRB_STATUS       0x290
#define BRIDGE_RRB_CLEAR        0x298

/* Identification: revision, part, manufacturer, and a bit that is set. */
#define ID_REVISION_SHIFT       28
#define ID_PART_SHIFT           12
#define ID_MANUFACTURER_SHIFT   1
#define ID_ALWAYS_ONE           1

/*
 * Status. The two bits a strap pin drives are the flash select and the bus
 * mode; this machine straps PCI, so the mode bit reads one. The link retry
 * counters and the outstanding-read count are zero because this model retries
 * nothing and completes every read before it returns.
 */
#define STATUS_FLASH_SELECT     BIT(6)
#define STATUS_PCI_MODE         BIT(5)

/*
 * Control. Seven fields come out of reset set, which is the largest single
 * thing the chip's specification adds over the public headers, and the reason
 * the reset value below is written as a sum of named fields rather than as one
 * number: a total cannot show which of its terms is missing.
 */
#define CONTROL_FLASH_WRITE_EN  BIT(31)
#define CONTROL_CLOCK_50MHZ     BIT(30)
#define CONTROL_CLOCK_40MHZ     BIT(29)
#define CONTROL_CLOCK_33MHZ     BIT(28)
#define CONTROL_PCI_RESET_SHIFT 24
#define CONTROL_PCI_RESET_PINS  (0xfu << CONTROL_PCI_RESET_SHIFT)
#define CONTROL_IO_SWAP         BIT(23)
#define CONTROL_MEM_SWAP        BIT(22)
#define CONTROL_PAGE_16K        BIT(21)
#define CONTROL_SSRAM_BAD_PAR   BIT(20)
#define CONTROL_SSRAM_PAR_EN    BIT(19)
#define CONTROL_SSRAM_SIZE      (0x3u << 17)
#define CONTROL_BAD_LINK_PACKET BIT(16)
#define CONTROL_CREDIT_SHIFT    12
#define CONTROL_CREDIT          (0xfu << CONTROL_CREDIT_SHIFT)
#define CONTROL_CREDIT_RESET    (3u << CONTROL_CREDIT_SHIFT)
/* Two strobes that clear the link retry counters and always read back zero. */
#define CONTROL_RETRY_CLEAR     (0x3u << 10)
#define CONTROL_BIG_ENDIAN      BIT(9)
#define CONTROL_OUTSTANDING_SHIFT 4
#define CONTROL_OUTSTANDING     (0x1fu << CONTROL_OUTSTANDING_SHIFT)
#define CONTROL_OUTSTANDING_RESET (0x1fu << CONTROL_OUTSTANDING_SHIFT)
#define CONTROL_WIDGET          0xfu
#define CONTROL_WIDGET_RESET    0xfu

#define CONTROL_RESET           (CONTROL_CLOCK_50MHZ | \
                                 CONTROL_CLOCK_40MHZ | \
                                 CONTROL_CLOCK_33MHZ | \
                                 CONTROL_PCI_RESET_PINS | \
                                 CONTROL_CREDIT_RESET | \
                                 CONTROL_BIG_ENDIAN | \
                                 CONTROL_OUTSTANDING_RESET | \
                                 CONTROL_WIDGET_RESET)

/*
 * Everything in the register except the two counter-clearing strobes, which
 * accept a write and read back zero. Leaving them out of the writable mask is
 * how they read zero, and it costs nothing because the counters they clear are
 * zero already in a model that never retries.
 */
#define CONTROL_WRITABLE        (~CONTROL_RETRY_CLEAR)

/* Request time-out: a twenty-bit reload value, counting down every 960 ns. */
#define REQUEST_TIMEOUT_RELOAD  0xfffffu
#define REQUEST_TIMEOUT_RESET   0xfffffu

/* Link configuration: maximum retry, null-packet time-out, maximum burst. */
#define LINK_MAX_RETRY_SHIFT    16
#define LINK_MAX_RETRY          (0x3ffu << LINK_MAX_RETRY_SHIFT)
#define LINK_NULL_TIMEOUT_SHIFT 10
#define LINK_NULL_TIMEOUT       (0x3fu << LINK_NULL_TIMEOUT_SHIFT)
#define LINK_MAX_BURST          0x3ffu

#define LINK_CONFIG_RESET       ((0x3ffu << LINK_MAX_RETRY_SHIFT) | \
                                 (0x06u << LINK_NULL_TIMEOUT_SHIFT) | \
                                 0x010u)
#define LINK_CONFIG_WRITABLE    (LINK_MAX_RETRY | LINK_NULL_TIMEOUT | \
                                 LINK_MAX_BURST)

/*
 * Direct map. The add-512-megabytes bit resets set, and it is there to step the
 * direct window over the processor's low compatibility segments rather than to
 * be a spare offset — so a model resetting it clear moves every direct-window
 * transfer half a gigabyte down without saying anything.
 */
#define DIRECT_WIDGET_SHIFT     20
#define DIRECT_WIDGET           (0xfu << DIRECT_WIDGET_SHIFT)
#define DIRECT_REMOTE_MAP_MODE  BIT(18)
#define DIRECT_ADD_512MB        BIT(17)
#define DIRECT_OFFSET           0x1ffffu

#define DIRECT_MAP_RESET        DIRECT_ADD_512MB
#define DIRECT_MAP_WRITABLE     (DIRECT_WIDGET | DIRECT_REMOTE_MAP_MODE | \
                                 DIRECT_ADD_512MB | DIRECT_OFFSET)

/* Arbitration. Nothing here is modelled; the register is storage. */
#define ARB_TICK_SHIFT          16
#define ARB_TICK                (0x3u << ARB_TICK_SHIFT)
#define ARB_REQ_WAIT_ENABLES    (0xffu << 8)
#define ARB_FREEZE_GRANTS       BIT(6)
#define ARB_BRIDGE_HIGH_SHIFT   3
#define ARB_BRIDGE_HIGH         (0x7u << ARB_BRIDGE_HIGH_SHIFT)
#define ARB_BRIDGE_LOW          0x7u

#define ARBITRATION_RESET       ((2u << ARB_TICK_SHIFT) | \
                                 (1u << ARB_BRIDGE_HIGH_SHIFT))
#define ARBITRATION_WRITABLE    (ARB_TICK | ARB_REQ_WAIT_ENABLES | \
                                 ARB_FREEZE_GRANTS | ARB_BRIDGE_HIGH | \
                                 ARB_BRIDGE_LOW)

/*
 * Bus time-out. The retry hold is five bits, not four: a model giving it four
 * would read back a value a guest wrote with its top bit missing, and the guest
 * would wait a sixteenth of the time it asked for.
 */
#define BUS_TIMEOUT_RETRY_HOLD_SHIFT 16
#define BUS_TIMEOUT_RETRY_HOLD  (0x1fu << BUS_TIMEOUT_RETRY_HOLD_SHIFT)
#define BUS_TIMEOUT_GIO         BIT(12)
#define BUS_TIMEOUT_PCI_RETRY   0x3ffu

#define BUS_TIMEOUT_RESET       (1u << BUS_TIMEOUT_RETRY_HOLD_SHIFT)
#define BUS_TIMEOUT_WRITABLE    (BUS_TIMEOUT_RETRY_HOLD | BUS_TIMEOUT_GIO | \
                                 BUS_TIMEOUT_PCI_RETRY)

/*
 * A device register, one per PCI slot. Three bits reset set and not two: the
 * memory-or-I/O select at bit 12 is the third, so a slot comes up pointing at
 * PCI memory space rather than I/O space.
 *
 * The page-cross check at bit 27 is where the two public sources name the same
 * bit oppositely — the specification calls it an enable and Linux calls it a
 * disable — and describe it identically. Nobody is inverted; the descriptions
 * agree and the names do not.
 */
#define DEV_ERROR_LOCK          BIT(28)
#define DEV_PAGE_CROSS_CHECK    BIT(27)
#define DEV_FORCE_PCI_PARITY    BIT(26)
#define DEV_VIRTUAL_ENABLE      BIT(25)
#define DEV_WGATHER_TRANSLATED  BIT(24)
#define DEV_WGATHER_DIRECT      BIT(23)
#define DEV_64BIT_DEVICE        BIT(22)
#define DEV_REAL_TIME           BIT(21)
#define DEV_SWAP_TRANSLATED     BIT(20)
#define DEV_SWAP_DIRECT         BIT(19)
#define DEV_PREFETCH            BIT(18)
#define DEV_PRECISE             BIT(17)
#define DEV_COHERENT            BIT(16)
#define DEV_BARRIER             BIT(15)
#define DEV_GUARANTEED_BW       BIT(14)
#define DEV_SWAP_PROGRAMMED     BIT(13)
#define DEV_MEMORY_SPACE        BIT(12)
#define DEV_WINDOW_BASE         0xfffu

#define DEVICE_RESET            (DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | \
                                 DEV_MEMORY_SPACE)
#define DEVICE_WRITABLE         (DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | \
                                 DEV_FORCE_PCI_PARITY | DEV_VIRTUAL_ENABLE | \
                                 DEV_WGATHER_TRANSLATED | DEV_WGATHER_DIRECT | \
                                 DEV_64BIT_DEVICE | DEV_REAL_TIME | \
                                 DEV_SWAP_TRANSLATED | DEV_SWAP_DIRECT | \
                                 DEV_PREFETCH | DEV_PRECISE | DEV_COHERENT | \
                                 DEV_BARRIER | DEV_GUARANTEED_BW | \
                                 DEV_SWAP_PROGRAMMED | DEV_MEMORY_SPACE | \
                                 DEV_WINDOW_BASE)

/*
 * How this model treats a write to a register.
 *
 * The three are kept apart because collapsing them loses something each time. A
 * read-only register written by a guest is a guest bug worth reporting; a
 * write-only strobe written by a guest is the guest using the chip correctly,
 * and reporting it would train people to ignore the report.
 */
typedef enum {
    SGI_BRIDGE_RO,      /* writes are a guest error and are reported */
    SGI_BRIDGE_RW,      /* the writable mask says which bits a guest may set */
    SGI_BRIDGE_WO,      /* a strobe: a write is taken, a read gives the reset */
} SGIBridgeRegAccess;

/*
 * One row per defined register. The table is the only place that says a
 * register exists, so a register cannot be given a decode here without also
 * being given a reset value and a writable mask — which is the point, because
 * a wrong reset value on this chip surfaces a long way from the register that
 * carries it.
 *
 * Where the reset values come from, because they are not all the same kind of
 * fact. Every non-zero value below is read from the chip's own specification,
 * which is the only source for any of them: the public headers encode bit
 * positions and no power-up values at all, so no reset value here has or can
 * have a second source. The zeros are two different things and the comments say
 * which: a register the specification gives as resetting to zero, and a
 * register the specification leaves blank, where zero is this model's choice
 * and not a reading. The qtest asserts the first kind and not the second.
 */
typedef struct SGIBridgeReg {
    const char *name;
    SGIBridgeRegAccess access;
    uint32_t reset;
    uint32_t writable;
} SGIBridgeReg;

#define SGI_BRIDGE_SLOT(offset) ((offset) / SGI_BRIDGE_REG_SLOT)

/* The per-slot rows, which differ only in which slot they are for. */
#define SGI_BRIDGE_DEVICE_ROW(n)                                              \
    [SGI_BRIDGE_SLOT(BRIDGE_DEVICE(n))] = {                                   \
        "device", SGI_BRIDGE_RW, DEVICE_RESET, DEVICE_WRITABLE }
#define SGI_BRIDGE_DEVICE_FLUSH_ROW(n)                                        \
    [SGI_BRIDGE_SLOT(BRIDGE_DEVICE_FLUSH(n))] = {                             \
        "device write request buffer flush", SGI_BRIDGE_RO, 0, 0 }

static const SGIBridgeReg sgi_bridge_regs[SGI_BRIDGE_REG_SLOTS] = {
    /*
     * The identity is the machine's to state rather than this file's, the same
     * way the crossbar's is, so this row's reset is a placeholder that reset
     * replaces with the value the properties compose. It is the one row whose
     * reset value does not come from the table, and it is called out again
     * where reset does the replacing.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] = {
        "identification", SGI_BRIDGE_RO, 0, 0 },

    /* Specification: retry counters and outstanding reads reset zero. */
    [SGI_BRIDGE_SLOT(BRIDGE_STATUS)] = {
        "status", SGI_BRIDGE_RO, STATUS_PCI_MODE, 0 },

    /* No error has been logged, so the error registers read zero. */
    [SGI_BRIDGE_SLOT(BRIDGE_ERR_UPPER)] = {
        "error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_ERR_LOWER)] = {
        "error address lower", SGI_BRIDGE_RO, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_CONTROL)] = {
        "control", SGI_BRIDGE_RW, CONTROL_RESET, CONTROL_WRITABLE },
    [SGI_BRIDGE_SLOT(BRIDGE_REQUEST_TIMEOUT)] = {
        "request time-out", SGI_BRIDGE_RW, REQUEST_TIMEOUT_RESET,
        REQUEST_TIMEOUT_RELOAD },

    /*
     * The widget-wide interrupt destination pair. The specification gives no
     * power-up value for either, so the zero here is this model's choice rather
     * than a reading, and nothing asserts it.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_UPPER)] = {
        "interrupt destination upper", SGI_BRIDGE_RW, 0, 0xffffffffu },
    [SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_LOWER)] = {
        "interrupt destination lower", SGI_BRIDGE_RW, 0, 0xffffffffu },

    [SGI_BRIDGE_SLOT(BRIDGE_ERR_CMD_WORD)] = {
        "error command word", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_LINK_CONFIG)] = {
        "link configuration", SGI_BRIDGE_RW, LINK_CONFIG_RESET,
        LINK_CONFIG_WRITABLE },

    /*
     * Target flush. Reading it is the operation and the answer means nothing:
     * the read completes only once every earlier programmed access to this
     * widget has, which is how a driver makes a configuration change take
     * effect before relying on it. Every driver read for this chip does it, so
     * the one thing this register must never do is fault.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_TARGET_FLUSH)] = {
        "target flush", SGI_BRIDGE_RO, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_AUX_ERR_CMD_WORD)] = {
        "auxiliary error command word", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_UPPER)] = {
        "response buffer error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_LOWER)] = {
        "response buffer error address lower", SGI_BRIDGE_RO, 0, 0 },

    /* Manufacturing test only; storage, and no power-up value is given. */
    [SGI_BRIDGE_SLOT(BRIDGE_TEST_PIN_CONTROL)] = {
        "test pin control", SGI_BRIDGE_RW, 0, 0xffffffffu },

    [SGI_BRIDGE_SLOT(BRIDGE_DIRECT_MAP)] = {
        "direct map", SGI_BRIDGE_RW, DIRECT_MAP_RESET, DIRECT_MAP_WRITABLE },

    /* There is no external static RAM here, so no parity failure to record. */
    [SGI_BRIDGE_SLOT(BRIDGE_SSRAM_PARITY_ERR)] = {
        "static RAM parity error", SGI_BRIDGE_RO, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_ARBITRATION)] = {
        "arbitration", SGI_BRIDGE_RW, ARBITRATION_RESET, ARBITRATION_WRITABLE },
    [SGI_BRIDGE_SLOT(BRIDGE_BUS_TIMEOUT)] = {
        "bus time-out", SGI_BRIDGE_RW, BUS_TIMEOUT_RESET,
        BUS_TIMEOUT_WRITABLE },

    /*
     * The type 1 configuration window is not modelled, but the register that
     * aims it is defined and harmless as storage. Our sources give neither a
     * field width nor a power-up value, so it takes a whole word and the zero
     * is this model's choice.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_TYPE1_CONFIG)] = {
        "PCI type 1 configuration", SGI_BRIDGE_RW, 0, 0xffffffffu },

    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_UPPER)] = {
        "PCI error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_LOWER)] = {
        "PCI error address lower", SGI_BRIDGE_RO, 0, 0 },

    SGI_BRIDGE_DEVICE_ROW(0),
    SGI_BRIDGE_DEVICE_ROW(1),
    SGI_BRIDGE_DEVICE_ROW(2),
    SGI_BRIDGE_DEVICE_ROW(3),
    SGI_BRIDGE_DEVICE_ROW(4),
    SGI_BRIDGE_DEVICE_ROW(5),
    SGI_BRIDGE_DEVICE_ROW(6),
    SGI_BRIDGE_DEVICE_ROW(7),

    SGI_BRIDGE_DEVICE_FLUSH_ROW(0),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(1),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(2),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(3),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(4),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(5),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(6),
    SGI_BRIDGE_DEVICE_FLUSH_ROW(7),

    /*
     * Read response buffer allocation. The sixteen buffers exist so a real chip
     * can be tuned for throughput; this model completes every read before it
     * returns, so how they are allocated has no observable effect and these are
     * storage. The status register reports no buffer valid and none in use,
     * which is true of a model with nothing outstanding.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_RRB_EVEN)] = {
        "even read response buffers", SGI_BRIDGE_RW, 0, 0xffffffffu },
    [SGI_BRIDGE_SLOT(BRIDGE_RRB_ODD)] = {
        "odd read response buffers", SGI_BRIDGE_RW, 0, 0xffffffffu },
    [SGI_BRIDGE_SLOT(BRIDGE_RRB_STATUS)] = {
        "read response buffer status", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_RRB_CLEAR)] = {
        "read response buffer clear", SGI_BRIDGE_WO, 0, 0 },
};

static const SGIBridgeReg *sgi_bridge_reg(hwaddr addr)
{
    hwaddr slot = addr / SGI_BRIDGE_REG_SLOT;

    if (addr % SGI_BRIDGE_REG_SLOT != SGI_BRIDGE_REG_DATA) {
        return NULL;
    }
    if (slot >= SGI_BRIDGE_REG_SLOTS) {
        return NULL;
    }
    return sgi_bridge_regs[slot].name ? &sgi_bridge_regs[slot] : NULL;
}

static uint32_t sgi_bridge_identification(SGIBridgeState *s)
{
    return (s->revision << ID_REVISION_SHIFT) |
           (s->part << ID_PART_SHIFT) |
           (s->manufacturer << ID_MANUFACTURER_SHIFT) |
           ID_ALWAYS_ONE;
}

/*
 * Which addresses this model answers. The chip defines the first 0x300 bytes of
 * a 64 KB register block and decodes the rest of it, so an access above 0x300,
 * or to a slot with no register in it, or to the wrong half of a slot, is a
 * question we have not answered and is told so rather than served a zero.
 */
static bool sgi_bridge_accepts(void *opaque, hwaddr addr, unsigned size,
                               bool is_write, MemTxAttrs attrs)
{
    if (!sgi_bridge_reg(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bridge: %s of register 0x%" HWADDR_PRIx " which "
                      "this model does not implement; raising a bus error "
                      "rather than answering\n",
                      is_write ? "write" : "read", addr);
        return false;
    }
    return true;
}

static uint64_t sgi_bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);

    /* Anything else was refused before it reached here. */
    g_assert(reg);

    if (reg->access == SGI_BRIDGE_WO) {
        return reg->reset;
    }
    return s->reg[addr / SGI_BRIDGE_REG_SLOT];
}

static void sgi_bridge_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);
    uint32_t *held = &s->reg[addr / SGI_BRIDGE_REG_SLOT];

    g_assert(reg);

    switch (reg->access) {
    case SGI_BRIDGE_RO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: write of 0x%" PRIx64 " to the read-only %s "
                      "register at 0x%" HWADDR_PRIx "\n", val, reg->name, addr);
        return;
    case SGI_BRIDGE_WO:
        /*
         * A strobe. Every one of them asks this model to release something it
         * never held, so accepting the write and doing nothing is the whole of
         * the behaviour; the register keeps reading its reset value.
         */
        return;
    case SGI_BRIDGE_RW:
        *held = (*held & ~reg->writable) | ((uint32_t)val & reg->writable);
        return;
    }

    g_assert_not_reached();
}

/*
 * The chip's own rule, and it is stricter than a model would otherwise be:
 * every register is reached by a crosstalk double-word packet with four data
 * enables, so an access of any other width is an error condition rather than an
 * access to serve. Both the maximum and the minimum are declared, because
 * leaving either unset makes the memory system reshape an access rather than
 * reject it — an unset maximum chops a wide access into pieces and an unset
 * minimum widens a narrow one, and the two fail in opposite directions.
 */
static const MemoryRegionOps sgi_bridge_reg_ops = {
    .read = sgi_bridge_read,
    .write = sgi_bridge_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.accepts = sgi_bridge_accepts,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * Where in PCI configuration space an access through the window lands.
 *
 * The device and function come straight out of the address, and the register
 * within the function needs one correction, which is the part worth writing
 * down. A 32-bit access returns the configuration double-word as a number, so
 * on this big-endian machine the double-word's least significant byte is at the
 * *highest* of its four byte addresses. A guest reaching for a single byte or a
 * halfword therefore aims at the window address whose byte lanes hold the
 * bytes it wants, and Linux composes exactly that: it exclusive-ors the
 * register offset with four minus the width. Undoing it here is what makes a
 * byte read of a vendor identifier return the vendor's low byte rather than the
 * device identifier's high one.
 *
 * OpenBSD never needs the correction because it reads configuration space in
 * 32-bit units only, where four minus the width is zero. That is why this is
 * single-sourced from Linux and stated as a rule about byte lanes rather than
 * as a rule about that driver: the two agree wherever they overlap.
 */
static uint32_t sgi_bridge_config_address(hwaddr addr, unsigned size)
{
    unsigned int reg = addr & BRIDGE_CFG0_REGISTER;
    unsigned int devfn = PCI_DEVFN(BRIDGE_CFG0_DEVICE(addr),
                                   BRIDGE_CFG0_FUNCTION(addr));

    reg = (reg & ~3u) | ((reg & 3u) ^ (4u - size));

    return (devfn << 8) | reg;
}

static uint64_t sgi_bridge_config_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);

    /*
     * A slot with no card in it answers all ones rather than zero, which is
     * what both guests are built to see. Linux reads configuration space
     * through an accessor that catches a bus error and treats the failure as an
     * empty slot; OpenBSD reads it through a guarded load and *substitutes all
     * ones itself* when the load faults. So all ones is the answer OpenBSD
     * would have manufactured anyway, and the answer the PCI layer above both
     * of them reads as nothing being there. Zero would be a real device whose
     * vendor identifier happened to be zero.
     */
    return pci_data_read(phb->bus, sgi_bridge_config_address(addr, size), size);
}

static void sgi_bridge_config_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);

    pci_data_write(phb->bus, sgi_bridge_config_address(addr, size), val, size);
}

/*
 * Configuration space takes accesses of one, two and four bytes, which is the
 * one place this chip is less strict than its register block. Both the minimum
 * and the maximum are declared because leaving either unset makes the memory
 * system reshape an access before this code sees it, and this code needs the
 * width the guest actually issued: the width is a term in the byte-lane
 * correction above, so an access that arrives having been widened or split
 * would be corrected by the wrong amount and read the wrong byte.
 */
static const MemoryRegionOps sgi_bridge_config_ops = {
    .read = sgi_bridge_config_read,
    .write = sgi_bridge_config_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * Which of the eight interrupt pins a PCI function's interrupt reaches.
 *
 * A slot's first interrupt pin goes to the chip pin of the same number, which
 * is not a guess: a real Origin 200 has SCSI controllers at PCI 0 and PCI 1 and
 * a capture shows them taking interrupts 0 and 1. That rule is also what
 * OpenBSD relies on, so it is the one this model implements.
 *
 * A function's other three pins are board wiring rather than a chip rule, and
 * the machine's interrupt chain is what settles where each goes — the base
 * I/O controller's second interrupt is the case in point, and it does not
 * follow the rule above. So the other three are reported rather than guessed
 * at: a plausible wrong pin here would deliver an interrupt to the wrong
 * handler and look like a driver bug.
 */
static int sgi_bridge_map_irq(PCIDevice *dev, int pin)
{
    int slot = PCI_SLOT(dev->devfn);

    if (pin != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bridge: PCI device %d asserted interrupt pin %c, "
                      "whose wiring to a bridge interrupt pin is the machine's "
                      "interrupt chain to state and is not modelled; treating "
                      "it as pin %d\n", slot, 'A' + pin, slot);
    }
    return slot;
}

/*
 * A device driving one of the eight pins. The level is kept as the level,
 * because the interrupt status register a guest reads has to follow it and must
 * never latch: the guest re-arms in software by re-reading that register at the
 * end of every interrupt it services, so a bit that cleared on service would
 * tell it a device with a second condition pending had gone quiet, and that
 * device would never be serviced again.
 *
 * Turning a level into a write across the crossbar into the Hub's
 * interrupt-modify register is the other half, and it arrives with the
 * interrupt registers.
 */
static void sgi_bridge_set_irq(void *opaque, int pin, int level)
{
    SGIBridgeState *s = opaque;

    s->pin_level = deposit32(s->pin_level, pin, 1, level);

    qemu_log_mask(LOG_UNIMP,
                  "sgi-bridge: interrupt pin %d went %s; the model records the "
                  "level but does not yet deliver it to the hub\n",
                  pin, level ? "high" : "low");
}

static void sgi_bridge_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeState *s = SGI_BRIDGE(obj);
    unsigned int slot;

    for (slot = 0; slot < SGI_BRIDGE_REG_SLOTS; slot++) {
        s->reg[slot] = sgi_bridge_regs[slot].reset;
    }

    /*
     * The one register whose reset value the table cannot hold, because it is
     * the machine's statement of what this chip is rather than a constant of
     * the chip. It is composed here so that a read of it takes the same path as
     * every other register.
     */
    s->reg[SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] =
        sgi_bridge_identification(s);

    s->pin_level = 0;
}

static void sgi_bridge_realize(DeviceState *dev, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);

    /*
     * The widget's whole crosstalk space is this chip's, and what is not in it
     * yet is not mapped, so an access there reaches nothing rather than
     * reaching a neighbour. The local register block is the first thing in it.
     */
    memory_region_init(&s->widget, obj, "sgi-bridge.widget",
                       SGI_BRIDGE_WIDGET_SIZE);
    memory_region_init_io(&s->regs, obj, &sgi_bridge_reg_ops, s,
                          "sgi-bridge.regs", SGI_BRIDGE_REGS_SIZE);
    memory_region_add_subregion(&s->widget, 0, &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->widget);

    /*
     * The two address spaces a PCI device sees. Both are the full 32 bits a
     * device can address, because a device window's base supplies PCI address
     * bits 31:20 for I/O space just as it does for memory space — this chip
     * does not narrow I/O space to the low 64 KB the way a PC chipset does.
     */
    memory_region_init(&s->pci_mmio, obj, "sgi-bridge.pci-mmio", 4 * GiB);
    memory_region_init(&s->pci_io, obj, "sgi-bridge.pci-io", 4 * GiB);

    phb->bus = pci_register_root_bus(dev, "pci", sgi_bridge_set_irq,
                                     sgi_bridge_map_irq, s,
                                     &s->pci_mmio, &s->pci_io,
                                     PCI_DEVFN(0, 0), SGI_BRIDGE_PINS,
                                     TYPE_PCI_BUS);

    /*
     * The configuration window goes in after the bus exists, because reading it
     * is reading the bus.
     */
    memory_region_init_io(&s->cfg0, obj, &sgi_bridge_config_ops, s,
                          "sgi-bridge.cfg0", BRIDGE_CFG0_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_CFG0_BASE, &s->cfg0);
}

static const VMStateDescription vmstate_sgi_bridge = {
    .name = "sgi-bridge",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIBridgeState, SGI_BRIDGE_REG_SLOTS),
        VMSTATE_UINT8(pin_level, SGIBridgeState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The identity a real Bridge answers with. It is a property rather than a
 * constant for the same reason the crossbar's is: what a widget reports is part
 * of the machine's description, and a machine carrying a different revision of
 * the chip says so here rather than by editing this file.
 */
static const Property sgi_bridge_properties[] = {
    DEFINE_PROP_UINT32("revision", SGIBridgeState, revision, 1),
    DEFINE_PROP_UINT32("part", SGIBridgeState, part, 0xc002),
    DEFINE_PROP_UINT32("manufacturer", SGIBridgeState, manufacturer, 0x036),
};

static void sgi_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI Bridge crosstalk-to-PCI host bridge";
    dc->realize = sgi_bridge_realize;
    dc->vmsd = &vmstate_sgi_bridge;
    dc->user_creatable = false;
    rc->phases.hold = sgi_bridge_reset_hold;
    device_class_set_props(dc, sgi_bridge_properties);
}

static const TypeInfo sgi_bridge_types[] = {
    {
        .name = TYPE_SGI_BRIDGE,
        .parent = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(SGIBridgeState),
        .class_init = sgi_bridge_class_init,
    },
};

DEFINE_TYPES(sgi_bridge_types)
