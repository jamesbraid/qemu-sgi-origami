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
#include "system/address-spaces.h"

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

/*
 * The interrupt registers. Their layouts are confirmed against the chip's own
 * register table field for field, including the one that is an absence: the
 * enable register's table starts at bit 30, so the multiple-error flag at 31
 * has no enable at all.
 */
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_RESET        0x110
#define BRIDGE_INT_MODE         0x118
#define BRIDGE_INT_DEVICE       0x120
#define BRIDGE_HOST_ERR_FIELD   0x128
#define BRIDGE_INT_HOST(n)      (0x130 + SGI_BRIDGE_REG_SLOT * (n))

/* The eight device pins live at the bottom of the status and enable words. */
#define INT_DEVICE_PINS         0xffu
/* All a guest may enable: bit 31, the multiple-error flag, has no bit. */
#define INT_ENABLE_WRITABLE     0x7fffffffu
/* Seven group bits, each clearing one group of error conditions at once. */
#define INT_RESET_GROUPS        0x7fu
/* One bit per pin: send a clearing packet when that pin falls. */
#define INT_MODE_WRITABLE       0xffu
/* Three bits per pin naming the device that owns it, eight pins. */
#define INT_DEVICE_WRITABLE     0xffffffu
/* A host address field and the bit number to set in the host's register. */
#define INT_HOST_ADDRESS        (0x3ffu << 8)
#define INT_HOST_FIELD          0xffu
#define INT_HOST_WRITABLE       (INT_HOST_ADDRESS | INT_HOST_FIELD)
/* The bit the chip sets when it reports an error of its own. */
#define HOST_ERR_FIELD_WRITABLE 0xffu

/* The per-slot registers, eight of each. */
#define BRIDGE_DEVICE(n)        (0x200 + SGI_BRIDGE_REG_SLOT * (n))
#define BRIDGE_DEVICE_FLUSH(n)  (0x240 + SGI_BRIDGE_REG_SLOT * (n))

/*
 * The internal address translation table, reachable at two addresses because
 * neither one alone can do the whole job.
 *
 * An entry is 64 bits and is written as one 64-bit quantity, which makes this
 * table the exception to the chip's rule that every access is a 32-bit
 * double-word. What can be read back there is only the entry's *upper* half,
 * as a 32-bit register in the low half of the eight-byte slot — so at the
 * entry's offset plus four, the same place every other register in this chip
 * sits. The lower half has nowhere to be read in that region at all, and the
 * second region exists for it: 128 read-only 32-bit words, one per entry.
 *
 * A model decoding only the first region leaves the second dead and a driver
 * reading an entry back gets nothing. Nothing on the OpenBSD path reads one
 * back, because that guest declines to use this table on this machine, so
 * this is a completeness requirement rather than a live one.
 */
#define BRIDGE_ATE_BASE         0x010000
#define BRIDGE_ATE_ENTRY_SIZE   8
#define BRIDGE_ATE_SIZE         (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_ENTRY_SIZE)
#define BRIDGE_ATE_LOW_BASE     0x011000
#define BRIDGE_ATE_LOW_STRIDE   4
#define BRIDGE_ATE_LOW_SIZE     (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_LOW_STRIDE)

/*
 * What an entry holds. The remote map field routes between systems across a
 * virtual backplane and is zero on a single machine; the model carries its full
 * sixteen bits even though nothing here will ever set the upper half, because
 * the direct map register's mode bit is what decides whether a device fills
 * eight of them or all sixteen, and a model narrowing the field would make that
 * bit unimplementable.
 */
#define ATE_REMOTE_MAP          MAKE_64BIT_MASK(48, 16)
#define ATE_ADDRESS             MAKE_64BIT_MASK(12, 36)
#define ATE_TARGET_WIDGET       MAKE_64BIT_MASK(8, 4)
#define ATE_BYTE_SWAP           BIT_ULL(5)
#define ATE_BARRIER             BIT_ULL(4)
#define ATE_PREFETCH            BIT_ULL(3)
#define ATE_PRECISE             BIT_ULL(2)
#define ATE_COHERENT            BIT_ULL(1)
#define ATE_VALID               BIT_ULL(0)

/*
 * A device sees four gigabytes of PCI address space, and the chip divides it by
 * the top two address bits: the first gigabyte is local and not translated, the
 * second goes through the table, and the upper two gigabytes are the direct
 * window.
 */
#define BRIDGE_PCI_TRANSLATED   0x40000000u
#define BRIDGE_PCI_DIRECT       0x80000000u

/* The two translation page sizes, chosen by a bit in the control register. */
#define BRIDGE_PAGE_SHIFT_4K    12
#define BRIDGE_PAGE_SHIFT_16K   14

/*
 * The direct window's own arithmetic. The offset field supplies crosstalk
 * address bits 47:31 and the low 31 bits of the PCI address pass through, and
 * the add-512-megabytes bit — which resets set — steps the result over the
 * processor's low compatibility segments, but only when the offset field is
 * zero.
 */
#define BRIDGE_DIRECT_SHIFT     31
#define BRIDGE_DIRECT_PASS      MAKE_64BIT_MASK(0, 31)
#define BRIDGE_DIRECT_512MB     (512 * MiB)

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

/*
 * The window each PCI slot is reached through, and the detail every driver gets
 * wrong once: the first two windows are 2 MB and the remaining six are 1 MB,
 * so they are not evenly spaced. Linux encodes that as a conditional in a
 * macro and OpenBSD as an arithmetic expression, and both produce this table.
 * It is derived from a base and two sizes rather than written out as eight
 * constants, because eight constants is where the seventh one goes wrong.
 */
#define BRIDGE_WINDOW_BASE      0x200000
#define BRIDGE_WINDOW_LARGE     (2 * MiB)
#define BRIDGE_WINDOW_SMALL     (1 * MiB)
#define BRIDGE_WINDOW_LARGE_END 2       /* slots below this get a large one */

/* A slot's window base supplies PCI address bits 31:20. */
#define BRIDGE_WINDOW_BASE_SHIFT 20

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
 * The type 1 configuration register's two fields, which select the bus and the
 * device a type 1 cycle targets. Everything else in the register is reserved
 * and reads back clear.
 */
#define TYPE1_BUS_NUMBER        (0xffu << 16)
#define TYPE1_DEVICE_NUMBER     (0x1fu << 11)
#define TYPE1_WRITABLE          (TYPE1_BUS_NUMBER | TYPE1_DEVICE_NUMBER)

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
    SGI_BRIDGE_LIVE,    /* read-only and computed: no storage to go stale */
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

/* One per interrupt pin: where its packet goes and what bit it sets. */
#define SGI_BRIDGE_INT_HOST_ROW(n)                                            \
    [SGI_BRIDGE_SLOT(BRIDGE_INT_HOST(n))] = {                                 \
        "interrupt host address", SGI_BRIDGE_RW, 0, INT_HOST_WRITABLE }

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
     * The type 1 configuration window is not modelled — this machine has no
     * PCI-to-PCI bridge — but the register that aims it is defined, and it is
     * storage. Its two fields are a bus number and a device number, both
     * resetting to zero, with reserved bits above and below them.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_TYPE1_CONFIG)] = {
        "PCI type 1 configuration", SGI_BRIDGE_RW, 0, TYPE1_WRITABLE },

    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_UPPER)] = {
        "PCI error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_LOWER)] = {
        "PCI error address lower", SGI_BRIDGE_RO, 0, 0 },

    /*
     * The interrupt status register, which is the one register in this chip
     * that must not be storage.
     *
     * Its low eight bits report whether each device is asserting its pin, and
     * they have to follow that level and never latch. The guest re-arms
     * interrupts in software: at the end of every interrupt it services it
     * re-reads this register, and if its own pin is still up — because a
     * second condition in the same device is pending — it pokes the host
     * directly to take another one. A model that latched the bit and cleared
     * it on service would tell that guest a device had gone quiet when it had
     * not, and the device would never be serviced again. It would also pass a
     * simple test, and boot, and then hang.
     *
     * So this register holds nothing. Its value is composed from the pins each
     * time it is read, which makes a latching model impossible to write here
     * rather than merely wrong: there is no bit to latch. The error bits above
     * the pins read zero because this model produces no errors.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_STATUS)] = {
        "interrupt status", SGI_BRIDGE_LIVE, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] = {
        "interrupt enable", SGI_BRIDGE_RW, 0, INT_ENABLE_WRITABLE },

    /*
     * Writing a one to a group bit clears every error bit in that group. This
     * model raises no error bits, so every group is already clear and the write
     * is a strobe with nothing to do — which is why it is not storage: a
     * register that read back the ones a guest wrote would say the groups were
     * still being cleared. Linux writes all seven at start of day.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_RESET)] = {
        "reset interrupt status", SGI_BRIDGE_WO, 0, INT_RESET_GROUPS },

    /*
     * The mode and device registers are read-modify-written by the guest: it
     * reads the current value, sets the bits for the pin it is establishing,
     * and writes it back. A model whose read gave zero rather than the last
     * value written would wipe every previously configured pin each time a new
     * interrupt was established, so only the most recently attached device
     * would work — a failure that looks like a driver bug in whichever device
     * attached first.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_MODE)] = {
        "interrupt mode", SGI_BRIDGE_RW, 0, INT_MODE_WRITABLE },
    [SGI_BRIDGE_SLOT(BRIDGE_INT_DEVICE)] = {
        "interrupt device", SGI_BRIDGE_RW, 0, INT_DEVICE_WRITABLE },

    /* The chip's own error bit. The table gives no power-up value for it. */
    [SGI_BRIDGE_SLOT(BRIDGE_HOST_ERR_FIELD)] = {
        "host error field", SGI_BRIDGE_RW, 0, HOST_ERR_FIELD_WRITABLE },

    SGI_BRIDGE_INT_HOST_ROW(0),
    SGI_BRIDGE_INT_HOST_ROW(1),
    SGI_BRIDGE_INT_HOST_ROW(2),
    SGI_BRIDGE_INT_HOST_ROW(3),
    SGI_BRIDGE_INT_HOST_ROW(4),
    SGI_BRIDGE_INT_HOST_ROW(5),
    SGI_BRIDGE_INT_HOST_ROW(6),
    SGI_BRIDGE_INT_HOST_ROW(7),

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

static hwaddr sgi_bridge_window_size(unsigned int slot)
{
    return slot < BRIDGE_WINDOW_LARGE_END ? BRIDGE_WINDOW_LARGE
                                          : BRIDGE_WINDOW_SMALL;
}

static hwaddr sgi_bridge_window_offset(unsigned int slot)
{
    unsigned int large = MIN(slot, BRIDGE_WINDOW_LARGE_END);
    unsigned int small = slot - large;

    return BRIDGE_WINDOW_BASE + large * BRIDGE_WINDOW_LARGE +
           small * BRIDGE_WINDOW_SMALL;
}

/*
 * Aim a slot's window where its register says, into the space its register
 * selects.
 *
 * The base names PCI address bits 31:20 and the offset within the window
 * supplies the rest, which for the two 2 MB windows means bit 20 arrives from
 * both. This model adds them, which is the arithmetic OpenBSD does: it computes
 * the window's start as the base shifted left twenty and then accounts for the
 * window's whole size above it, with no correction for a base whose lowest bit
 * is set. So a 2 MB window based on an odd megabyte reaches across the 2 MB
 * boundary above it, and this model and that guest agree about where.
 *
 * Which space the window looks into cannot be changed on a region once it
 * exists, so each slot has two windows at the same address and exactly one of
 * them is enabled.
 */
static void sgi_bridge_aim_window(SGIBridgeState *s, unsigned int slot)
{
    uint32_t control = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];
    hwaddr base = (hwaddr)(control & DEV_WINDOW_BASE) <<
                  BRIDGE_WINDOW_BASE_SHIFT;
    bool memory = control & DEV_MEMORY_SPACE;

    memory_region_set_alias_offset(&s->device_window_mem[slot], base);
    memory_region_set_alias_offset(&s->device_window_io[slot], base);
    memory_region_set_enabled(&s->device_window_mem[slot], memory);
    memory_region_set_enabled(&s->device_window_io[slot], !memory);
}

/*
 * Whether a register slot is one of the eight per-device control registers, and
 * which device it belongs to. A write to one of them re-aims that slot's
 * window, so the write path has to recognise them.
 */
static bool sgi_bridge_is_device_register(hwaddr addr, unsigned int *slot)
{
    hwaddr offset = addr - SGI_BRIDGE_REG_DATA;

    if (offset < BRIDGE_DEVICE(0) ||
        offset >= BRIDGE_DEVICE(SGI_BRIDGE_DEVICES)) {
        return false;
    }
    *slot = (offset - BRIDGE_DEVICE(0)) / SGI_BRIDGE_REG_SLOT;
    return true;
}

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

/*
 * A register whose value is composed when it is read rather than held. The
 * interrupt status register is the only one, and composing it is what makes it
 * unable to latch: the pins are read from the devices driving them, so there is
 * nothing that could go on reading set after a device stopped asserting.
 */
static uint32_t sgi_bridge_live(SGIBridgeState *s, hwaddr addr)
{
    switch (addr - SGI_BRIDGE_REG_DATA) {
    case BRIDGE_INT_STATUS:
        /*
         * The pins as they are now, and no error bits: this model produces no
         * errors, so every condition above the pins is clear.
         */
        return s->pin_level & INT_DEVICE_PINS;
    default:
        g_assert_not_reached();
    }
}

static uint64_t sgi_bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);

    /* Anything else was refused before it reached here. */
    g_assert(reg);

    switch (reg->access) {
    case SGI_BRIDGE_LIVE:
        return sgi_bridge_live(s, addr);
    case SGI_BRIDGE_WO:
        return reg->reset;
    default:
        return s->reg[addr / SGI_BRIDGE_REG_SLOT];
    }
}

static void sgi_bridge_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);
    uint32_t *held = &s->reg[addr / SGI_BRIDGE_REG_SLOT];
    unsigned int slot;

    g_assert(reg);

    switch (reg->access) {
    case SGI_BRIDGE_LIVE:
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
        if (sgi_bridge_is_device_register(addr, &slot)) {
            sgi_bridge_aim_window(s, slot);
        }
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
 * An entry is written whole and read in halves, so what this region accepts is
 * narrower than a width check can say: a 64-bit write at an entry's own offset,
 * and a 32-bit read four bytes above it. Every other shape is refused rather
 * than served, including the two a reader would most likely reach for — a
 * 32-bit read at the entry's offset, where nothing lives, and a 32-bit write,
 * which would leave half an entry behind.
 */
static bool sgi_bridge_ate_accepts(void *opaque, hwaddr addr, unsigned size,
                                   bool is_write, MemTxAttrs attrs)
{
    bool whole_entry_write = is_write && size == BRIDGE_ATE_ENTRY_SIZE &&
                             addr % BRIDGE_ATE_ENTRY_SIZE == 0;
    bool upper_half_read = !is_write && size == 4 &&
                           addr % BRIDGE_ATE_ENTRY_SIZE == SGI_BRIDGE_REG_DATA;

    if (!whole_entry_write && !upper_half_read) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: %u-byte %s of translation table offset "
                      "0x%" HWADDR_PRIx "; entries are written whole as 64-bit "
                      "quantities and only their upper halves read back here\n",
                      size, is_write ? "write" : "read", addr);
        return false;
    }
    return true;
}

static uint64_t sgi_bridge_ate_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIBridgeState *s = opaque;

    return s->ate_entry[addr / BRIDGE_ATE_ENTRY_SIZE] >> 32;
}

static void sgi_bridge_ate_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SGIBridgeState *s = opaque;

    s->ate_entry[addr / BRIDGE_ATE_ENTRY_SIZE] = val;
}

static const MemoryRegionOps sgi_bridge_ate_ops = {
    .read = sgi_bridge_ate_read,
    .write = sgi_bridge_ate_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .valid.accepts = sgi_bridge_ate_accepts,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
};

/*
 * The read-only view of the entries' lower halves. A write here is refused
 * rather than ignored: there is no register to write, and a chip that swallowed
 * the write would let a driver believe it had changed an entry through the one
 * address that cannot change one.
 */
static bool sgi_bridge_ate_low_accepts(void *opaque, hwaddr addr, unsigned size,
                                       bool is_write, MemTxAttrs attrs)
{
    if (is_write) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: write to translation table lower-half view "
                      "at 0x%" HWADDR_PRIx ", which is read-only; entries are "
                      "written whole through the table itself\n", addr);
        return false;
    }
    return true;
}

static uint64_t sgi_bridge_ate_low_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SGIBridgeState *s = opaque;

    return (uint32_t)s->ate_entry[addr / BRIDGE_ATE_LOW_STRIDE];
}

static void sgi_bridge_ate_low_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size)
{
    /* Refused before it reached here. */
    g_assert_not_reached();
}

static const MemoryRegionOps sgi_bridge_ate_low_ops = {
    .read = sgi_bridge_ate_low_read,
    .write = sgi_bridge_ate_low_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.accepts = sgi_bridge_ate_low_accepts,
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
/*
 * The per-pin interrupt address register holds the destination in a compressed
 * form rather than whole: bits 17:8 are the crosstalk address's bits 47:38 —
 * that is, the address's top bit and then the nine-bit node — and bits 7:0 are
 * the interrupt number. Three independent sources agree on that layout, which
 * is why it is trusted: SGI's own mask, OpenBSD's identical one, and Linux
 * composing the same value from two terms by different arithmetic.
 */
#define INT_HOST_ADDR_SHIFT     8
#define XTALK_HOST_SHIFT        38
#define XTALK_NODE_SHIFT        38

/* Where the Hub's interrupt-modify register sits inside a node's Hub window. */
#define HUB_NODE_WINDOW         0x01800000
#define HUB_INT_MODIFY          0x90
#define HUB_INT_MODIFY_SET      0x100

/*
 * Turn a pin's transition into the packet the chip really sends: a write to the
 * Hub's interrupt-modify register on the destination node, carrying the
 * interrupt number.
 *
 * The address is reconstructed from what the guest programmed, not invented
 * here. Restoring bits 17:8 of the per-pin register to bits 47:38 puts the
 * crosstalk address's top bit back and the node at bit 38; the Hub's window and
 * the register's offset complete it.
 *
 * That crosstalk address is then issued into the machine's fabric. This model's
 * fabric is addressed the processor's way rather than the packet's, so the node
 * and the offset are recomposed with the machine's own encoding — which is the
 * seam between what this chip does and how the machine represents the bus, and
 * is why the two constants for it come from the machine instead of living here.
 */
static void sgi_bridge_send_interrupt(SGIBridgeState *s, int pin, bool set)
{
    uint64_t addr = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_HOST(pin))];
    uint64_t host = (addr & INT_HOST_ADDRESS) >> INT_HOST_ADDR_SHIFT;
    uint64_t xtalk = (host << XTALK_HOST_SHIFT) | HUB_NODE_WINDOW |
                     HUB_INT_MODIFY;
    unsigned int node = (xtalk >> XTALK_NODE_SHIFT) & 0x1ff;
    uint64_t number = addr & INT_HOST_FIELD;
    uint64_t value = number | (set ? HUB_INT_MODIFY_SET : 0);
    hwaddr where = s->fabric_base +
                   ((uint64_t)node << s->fabric_node_shift) +
                   HUB_NODE_WINDOW + HUB_INT_MODIFY;

    if (!addr) {
        /*
         * Nothing programmed. A guest that has not set the destination has not
         * asked for this interrupt yet, and writing to node zero's Hub because
         * the register happens to read zero would deliver an interrupt nobody
         * routed.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: interrupt pin %d changed with no "
                      "destination programmed; no packet sent\n", pin);
        return;
    }

    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

static void sgi_bridge_set_irq(void *opaque, int pin, int level)
{
    SGIBridgeState *s = opaque;
    bool was = extract32(s->pin_level, pin, 1);

    s->pin_level = deposit32(s->pin_level, pin, 1, level);

    /*
     * The level is what the interrupt status register reports, and it is also
     * what decides whether a packet goes: the chip sends on a transition rather
     * than while a level is held, so a device holding its pin high does not
     * flood the Hub with writes.
     */
    if (level && !was) {
        sgi_bridge_send_interrupt(s, pin, true);
    } else if (!level && was) {
        /*
         * The falling edge sends a clear only if the guest asked for it, which
         * it does precisely so that it need not clear the Hub bit itself. With
         * the mode bit clear the guest clears the bit in its own handler, and a
         * model sending anyway would clear an interrupt the guest had not
         * finished servicing.
         */
        if (s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_MODE)] & (1u << pin)) {
            sgi_bridge_send_interrupt(s, pin, false);
        }
    }
}

/*
 * The translation page size, which a bit in the control register chooses.
 */
static unsigned int sgi_bridge_page_shift(SGIBridgeState *s)
{
    return s->reg[SGI_BRIDGE_SLOT(BRIDGE_CONTROL)] & CONTROL_PAGE_16K ?
           BRIDGE_PAGE_SHIFT_16K : BRIDGE_PAGE_SHIFT_4K;
}

/*
 * Where a crosstalk address a translation produced is issued.
 *
 * This is the one seam between what this chip does and how the machine
 * around it represents the fabric, and it is kept to a single function so
 * that settling the second does not disturb the first. A translated transfer
 * names a target widget and an offset inside it; composing those into one
 * address is the fabric's business, and the fabric's representation is not
 * settled. Until it is, the offset is the address and the widget is carried
 * but not applied — which is right for the machine as it stands, where the
 * only target anything names is the node's own memory.
 *
 * No test depends on this. The two entries the straddle test uses differ
 * only in the page they name and target the same widget, so what they assert
 * stays true whatever this function becomes.
 */
static hwaddr sgi_bridge_compose_xtalk(SGIBridgeState *s,
                                       unsigned int target_widget,
                                       hwaddr offset)
{
    return offset;
}

/*
 * A transfer a device initiates, translated one page at a time.
 *
 * The address mask returned here is one page and never more, and that is the
 * whole mechanism. The memory system clamps a transfer to the end of the
 * range a translation covers and then asks again for the next one, so a mask
 * of one page turns a single transfer spanning several pages into one call
 * here per page.
 *
 * A model returning a larger mask, or hand-rolling the walk instead of going
 * through this interface, translates once and uses that one translation for
 * the whole transfer. That is right for every transfer inside one page and
 * wrong for the tail of every transfer crossing a boundary — a defect a
 * neighbouring emulator of comparable hardware carried for years, because the
 * transfers that expose it are the rare ones.
 */
static IOMMUTLBEntry sgi_bridge_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                          IOMMUAccessFlags flag, int iommu_idx)
{
    SGIBridgeState *s = container_of(iommu, SGIBridgeState, iommu);
    unsigned int shift = sgi_bridge_page_shift(s);
    hwaddr page_mask = MAKE_64BIT_MASK(0, shift);
    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr & ~page_mask,
        .translated_addr = 0,
        .addr_mask = page_mask,
        .perm = IOMMU_NONE,
    };
    uint32_t direct;
    uint64_t ate;
    unsigned int index;
    hwaddr offset;

    if (addr < BRIDGE_PCI_TRANSLATED) {
        /*
         * Local. Not translated to crosstalk at all, so it reaches whatever is
         * at that address on this side rather than being carried across.
         */
        entry.translated_addr = addr & ~page_mask;
        entry.perm = IOMMU_RW;
        return entry;
    }

    if (addr >= BRIDGE_PCI_DIRECT) {
        direct = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DIRECT_MAP)];
        offset = (hwaddr)(direct & DIRECT_OFFSET) << BRIDGE_DIRECT_SHIFT;
        offset |= addr & BRIDGE_DIRECT_PASS;

        /*
         * The add-512-megabytes rule, which applies only when the offset field
         * is zero. It exists to step the window over the processor's low
         * compatibility segments, and the bit resets set, so a machine that has
         * programmed nothing still gets the step.
         */
        if ((direct & DIRECT_OFFSET) == 0 && (direct & DIRECT_ADD_512MB)) {
            offset += BRIDGE_DIRECT_512MB;
        }

        entry.translated_addr =
            sgi_bridge_compose_xtalk(s, (direct & DIRECT_WIDGET) >>
                                     DIRECT_WIDGET_SHIFT, offset) & ~page_mask;
        entry.perm = IOMMU_RW;
        return entry;
    }

    /* Through the table. The index is the page number inside the window. */
    index = (addr - BRIDGE_PCI_TRANSLATED) >> shift;
    if (index >= SGI_BRIDGE_ATE_ENTRIES) {
        /*
         * Past the internal table. An external static RAM would supply the
         * rest, and this machine has none, so the transfer has nowhere to go.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: transfer to PCI address 0x%" HWADDR_PRIx
                      " needs translation entry %u, which is past the %u the "
                      "chip holds and this machine has no external table\n",
                      addr, index, SGI_BRIDGE_ATE_ENTRIES);
        return entry;
    }

    ate = s->ate_entry[index];
    if (!(ate & ATE_VALID)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: transfer to PCI address 0x%" HWADDR_PRIx
                      " uses translation entry %u, which is not valid\n",
                      addr, index);
        return entry;
    }

    entry.translated_addr =
        sgi_bridge_compose_xtalk(s, (ate & ATE_TARGET_WIDGET) >> 8,
                                 ate & ATE_ADDRESS) & ~page_mask;
    entry.perm = IOMMU_RW;
    return entry;
}

/*
 * Every device on this bus translates through the same table: the chip has one,
 * and which device asked is carried in the entry rather than in the lookup.
 */
static AddressSpace *sgi_bridge_device_address_space(PCIBus *bus, void *opaque,
                                                     int devfn)
{
    SGIBridgeState *s = opaque;

    return &s->iommu_as;
}

static const PCIIOMMUOps sgi_bridge_iommu_ops = {
    .get_address_space = sgi_bridge_device_address_space,
};

#define TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION "sgi-bridge-iommu-memory-region"

static void sgi_bridge_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = sgi_bridge_translate;
}

static void sgi_bridge_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeState *s = SGI_BRIDGE(obj);
    unsigned int slot;
    unsigned int i;

    for (i = 0; i < SGI_BRIDGE_REG_SLOTS; i++) {
        s->reg[i] = sgi_bridge_regs[i].reset;
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

    /*
     * Every entry resets with its valid bit clear, which is the state that says
     * a translation through it must fault rather than reach somewhere. Clearing
     * the whole entry rather than the bit is deliberate: a stale page address
     * behind a clear valid bit is a working translation waiting for one write.
     */
    memset(s->ate_entry, 0, sizeof(s->ate_entry));

    /*
     * The windows follow the registers that aim them, so reset re-aims all
     * eight. A slot's register resets selecting PCI memory space with a window
     * base of zero, so every window comes up over the bottom of PCI memory
     * space, which is where a slot that nothing has configured points.
     */
    for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
        sgi_bridge_aim_window(s, slot);
    }
}

static void sgi_bridge_realize(DeviceState *dev, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);
    unsigned int slot;

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
     * The address space this chip issues interrupt packets into. It is the
     * machine's own, given rather than assumed, so that a packet travels the
     * same fabric a processor's access does.
     */
    if (!s->fabric) {
        error_setg(errp, "the I/O bridge needs the fabric its interrupt "
                         "packets travel");
        return;
    }
    address_space_init(&s->fabric_as, s->fabric, "sgi-bridge.fabric");

    /*
     * The two address spaces a PCI device sees. Both are the full 32 bits a
     * device can address, because a device window's base supplies PCI address
     * bits 31:20 for I/O space just as it does for memory space — this chip
     * does not narrow I/O space to the low 64 KB the way a PC chipset does.
     */
    memory_region_init(&s->pci_mmio, obj, "sgi-bridge.pci-mmio", 4 * GiB);
    memory_region_init(&s->pci_io, obj, "sgi-bridge.pci-io", 4 * GiB);

    /*
     * The eight slot windows, two regions each looking into one of those two
     * spaces. Both go in at the slot's place in the widget's address space and
     * reset decides which of the pair answers.
     */
    for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
        g_autofree char *mem_name =
            g_strdup_printf("sgi-bridge.window%u.mem", slot);
        g_autofree char *io_name =
            g_strdup_printf("sgi-bridge.window%u.io", slot);

        memory_region_init_alias(&s->device_window_mem[slot], obj, mem_name,
                                 &s->pci_mmio, 0,
                                 sgi_bridge_window_size(slot));
        memory_region_init_alias(&s->device_window_io[slot], obj, io_name,
                                 &s->pci_io, 0, sgi_bridge_window_size(slot));
        memory_region_add_subregion(&s->widget,
                                    sgi_bridge_window_offset(slot),
                                    &s->device_window_mem[slot]);
        memory_region_add_subregion(&s->widget,
                                    sgi_bridge_window_offset(slot),
                                    &s->device_window_io[slot]);
    }

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

    /*
     * The translation every device on this bus transfers through, and the
     * address space they are given in place of the system's.
     */
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION, obj,
                             "sgi-bridge.iommu", UINT64_MAX);
    address_space_init(&s->iommu_as, MEMORY_REGION(&s->iommu),
                       "sgi-bridge.dma");
    pci_setup_iommu(phb->bus, &sgi_bridge_iommu_ops, s);

    /* The translation table, and the read-only view of its lower halves. */
    memory_region_init_io(&s->ate, obj, &sgi_bridge_ate_ops, s,
                          "sgi-bridge.ate", BRIDGE_ATE_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_BASE, &s->ate);
    memory_region_init_io(&s->ate_low, obj, &sgi_bridge_ate_low_ops, s,
                          "sgi-bridge.ate-low", BRIDGE_ATE_LOW_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_LOW_BASE, &s->ate_low);
}

static const VMStateDescription vmstate_sgi_bridge = {
    .name = "sgi-bridge",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIBridgeState, SGI_BRIDGE_REG_SLOTS),
        VMSTATE_UINT8(pin_level, SGIBridgeState),
        VMSTATE_UINT64_ARRAY_V(ate_entry, SGIBridgeState,
                               SGI_BRIDGE_ATE_ENTRIES, 2),
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
    DEFINE_PROP_LINK("fabric", SGIBridgeState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIBridgeState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIBridgeState,
                       fabric_node_shift, 0),
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
    {
        .name = TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = sgi_bridge_iommu_class_init,
    },
};

DEFINE_TYPES(sgi_bridge_types)
