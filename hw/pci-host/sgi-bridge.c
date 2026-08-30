/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Bridge crosstalk-to-PCI host bridge.
 *
 * A crosstalk widget on one side, a PCI host bridge on the other: the local
 * register block, the per-slot device windows, configuration space, the
 * address translation table, the interrupt path and the board identity part.
 *
 * Registers are 32 bits in data bits 31:0 of an eight-byte slot, so the data
 * sits at the slot's offset plus four. Linux's header names that address;
 * OpenBSD names the slot and folds the four in -- the same registers, not
 * four bytes apart. Reset state is heavily non-zero (the control register
 * alone comes up with seven fields set), so decode, reset value and writable
 * mask all come from one table. An access the chip does not define is posted
 * and flagged as an invalid-address error, the way the real chip does it.
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
#include "trace.h"

/* The data half of a slot; the other half holds no register. */
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

/*
 * The one-wire bus master to the board identity part. Bit 0 is the sampled
 * level, bit 1 the finished flag; writing the two timing fields starts a
 * slot. The whole protocol arrives through this register a bit at a time,
 * so the slot kind is recovered from the timings.
 */
#define BRIDGE_NIC              0x0b0
#define NIC_LEVEL               (1u << 0)
#define NIC_DONE                (1u << 1)
#define NIC_SAMPLE_SHIFT        2
#define NIC_SAMPLE_MASK         (0xffu << NIC_SAMPLE_SHIFT)
#define NIC_PULSE_SHIFT         10
#define NIC_PULSE_MASK          (0x3ffu << NIC_PULSE_SHIFT)
#define NIC_WRITABLE            (NIC_SAMPLE_MASK | NIC_PULSE_MASK)

/*
 * Thresholds, not exact values: Linux resets with a 520 us pulse where
 * OpenBSD uses 500, and both must land the same side of the line.
 */
#define NIC_PULSE_IS_RESET      400

/*
 * A read uses the same short slot as a written one, so timing cannot tell
 * them apart; the part answers whatever the conversation expects next.
 */
#define NIC_PULSE_IS_ZERO       40

/* Conversation state, surviving between the per-bit register writes. */
#define NIC_PHASE_IDLE          0   /* nothing asked since the last reset */
#define NIC_PHASE_COMMAND       1   /* taking the eight bits of a command */
#define NIC_PHASE_SEARCH        2   /* answering a search for parts */
#define NIC_PHASE_FUNCTION      3   /* selected; taking a function command */
#define NIC_PHASE_OFFSET        4   /* taking the two bytes of a page offset */
#define NIC_PHASE_ANSWER        5   /* shifting a page's answer back out */
#define NIC_PHASE_MATCH         6   /* taking the identity a guest is naming */
#define NIC_PHASE_STATUS        7   /* shifting the status memory back out */
#define NIC_PHASE_READ          8   /* streaming the data memory, unframed */

/*
 * Both addressing forms are answered: real firmware searches the bus,
 * resets, and then names the identity the search handed back.
 */
#define NIC_CMD_SEARCH_ROM      0xf0
#define NIC_CMD_MATCH_ROM       0x55
#define NIC_CMD_SKIP_ROM        0xcc
#define NIC_CMD_READ_MEMORY     0xa5
#define NIC_CMD_READ_PLAIN      0xf0
#define NIC_CMD_READ_STATUS     0xaa

/*
 * Two commands read the same data memory with different framing, so a
 * page's embedded check is stored in its last two bytes rather than
 * manufactured on the way out. Sixty-four pages matches the sixty-four
 * redirection bytes the status memory answers.
 */
#define NIC_DATA_PAGES          64
#define NIC_DATA_SIZE           (NIC_DATA_PAGES * NIC_PAGE_SIZE)
#define NIC_UNPROGRAMMED        0xff

/*
 * The status memory, read with its own command: protection bits and the
 * per-page redirection bytes. EPROM leaves unwritten bits set, so every
 * byte of ours reads 0xff -- which in a redirection byte means the page is
 * valid where it sits, exactly what this part needs to say.
 */
#define NIC_STATUS_UNPROGRAMMED 0xff
#define NIC_STATUS_PAGE         8
#define NIC_STATUS_LAST         0x13f

/*
 * One page's answer: a redirection byte, a check pair over the request and
 * that byte, the page, and a check pair over the page.
 */
#define NIC_PAGE_SIZE           32
#define NIC_PAGES               2
#define NIC_NOT_REDIRECTED      0xff
#define NIC_ANSWER_SIZE         (3 + NIC_PAGE_SIZE + 2)

/*
 * The ROM identity: family code, six serial bytes, a check byte. Guests
 * match on the family code; the wrong one means found and ignored. The
 * serial is the machine's to supply.
 */
#define NIC_ROM_FAMILY          0x0b
#define NIC_ROM_SERIAL_DEFAULT  0x060504030201ULL
#define NIC_ROM_SERIAL_MASK     MAKE_64BIT_MASK(0, 48)
#define NIC_ROM_BITS            64


#define BRIDGE_SSRAM_PARITY_ERR 0x090
#define BRIDGE_ARBITRATION      0x0a0
#define BRIDGE_BUS_TIMEOUT      0x0c0
#define BRIDGE_PCI_TYPE1_CONFIG 0x0c8
#define BRIDGE_PCI_ERR_UPPER    0x0d0
#define BRIDGE_PCI_ERR_LOWER    0x0d8

/*
 * Interrupt registers, confirmed against the chip's register table field
 * for field; the multiple-error flag at bit 31 has no enable bit.
 */
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_INT_ENABLE       0x108
#define BRIDGE_INT_RESET        0x110
#define BRIDGE_INT_MODE         0x118
#define BRIDGE_INT_DEVICE       0x120
#define BRIDGE_HOST_ERR_FIELD   0x128
#define BRIDGE_INT_HOST(n)      (0x130 + SGI_BRIDGE_REG_SLOT * (n))

/* The only reserved data half this model accepts. */
#define BRIDGE_RESERVED_DATA_HALF 0x19c

/* The eight device pins live at the bottom of the status and enable words. */
#define INT_DEVICE_PINS         0xffu
/* Bit 24, "address invalid for this widget": set when a guest touches an
 * undefined address inside the register window. The one error bit this model
 * raises. */
#define INT_INVALID_ADDR        (1u << 24)
/* All a guest may enable: bit 31, the multiple-error flag, has no bit. */
#define INT_ENABLE_WRITABLE     0x7fffffffu
/* Seven group bits, each clearing one group of error conditions at once. */
#define INT_RESET_GROUPS        0x7fu
/* Bit 3 is the request-dispatch group's strobe. The invalid-address error is
 * in that group, so it alone clears it; a strobe for any other group does not. */
#define INT_RESET_REQ_DSP_GRP   (1u << 3)
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
 * The translation table, at two addresses: entries are written whole as
 * 64-bit quantities (the exception to the 32-bit rule), only the upper half
 * reads back at the entry's offset plus four, and the second region serves
 * the lower halves as 128 read-only words.
 */
#define BRIDGE_ATE_BASE         0x010000
#define BRIDGE_ATE_ENTRY_SIZE   8
#define BRIDGE_ATE_SIZE         (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_ENTRY_SIZE)
#define BRIDGE_ATE_LOW_BASE     0x011000
#define BRIDGE_ATE_LOW_STRIDE   4

/* The external-SSRAM window: 512 KB at 0x08_0000, unpopulated on this board. */
#define BRIDGE_SSRAM_BASE       0x080000
#define BRIDGE_SSRAM_SIZE       (512 * KiB)
#define BRIDGE_ATE_LOW_SIZE     (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_LOW_STRIDE)

/*
 * The remote map field keeps its full sixteen bits: the direct map mode bit
 * decides whether a device fills eight or all of them.
 */
#define ATE_REMOTE_MAP          MAKE_64BIT_MASK(48, 16)
#define ATE_ADDRESS             MAKE_64BIT_MASK(12, 36)
#define ATE_TARGET_WIDGET       MAKE_64BIT_MASK(8, 4)
#define ATE_BARRIER             BIT_ULL(4)
#define ATE_PREFETCH            BIT_ULL(3)
#define ATE_PRECISE             BIT_ULL(2)
#define ATE_COHERENT            BIT_ULL(1)
#define ATE_VALID               BIT_ULL(0)

/*
 * The top two PCI address bits split the space: first gigabyte local, the
 * second through the table, the upper two the direct window.
 */
#define BRIDGE_PCI_TRANSLATED   0x40000000u
#define BRIDGE_PCI_DIRECT       0x80000000u

/* The two translation page sizes, chosen by a bit in the control register. */
#define BRIDGE_PAGE_SHIFT_4K    12
#define BRIDGE_PAGE_SHIFT_16K   14

/*
 * Direct window: the offset field supplies crosstalk bits 47:31, the low 31
 * PCI bits pass through, and the add-512MB bit (reset set) steps over the
 * processor's low compatibility segments when the offset field is zero.
 */
#define BRIDGE_DIRECT_SHIFT     31
#define BRIDGE_DIRECT_PASS      MAKE_64BIT_MASK(0, 31)
#define BRIDGE_DIRECT_512MB     (512 * MiB)

/*
 * A 64-bit DMA cycle: bits 47:0 are the crosstalk address, 63:60 the
 * target widget; the offset registers are bypassed so the NASID survives.
 */
#define BRIDGE_PCI_64BIT        (1ULL << 32)
#define BRIDGE_XTALK_PASS       MAKE_64BIT_MASK(0, 48)
#define BRIDGE_DIRECT64_WIDGET_SHIFT 60

/*
 * Type 0 configuration space: eight devices 4 KB apart, eight functions
 * 256 bytes apart. The public geometry lives in the header because the
 * machine names configuration windows too; only the decode is private.
 */
#define BRIDGE_CFG0_SIZE        (SGI_BRIDGE_DEVICES * \
                                 SGI_BRIDGE_CFG0_DEVICE_SIZE)
#define BRIDGE_CFG0_DEVICE(a)   (((a) >> 12) & 7)
#define BRIDGE_CFG0_FUNCTION(a) (((a) >> 8) & 7)
#define BRIDGE_CFG0_REGISTER    0xff

/*
 * Per-slot windows: the first two are 2 MB, the remaining six 1 MB, so they
 * are not evenly spaced. Derived rather than written as eight constants.
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

/* Strap pins: this machine straps PCI, so the mode bit reads one. */
#define STATUS_FLASH_SELECT     BIT(6)
#define STATUS_PCI_MODE         BIT(5)

/*
 * Control. Seven fields reset set (specification only -- the public headers
 * carry no power-up values), summed from named fields so a missing term
 * shows.
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

/* All but the counter-clearing strobes, which read back zero. */
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
 * The add-512MB bit resets set; clearing it at reset would silently move
 * every direct-window transfer half a gigabyte down.
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

/* The retry hold is five bits, not four. */
#define BUS_TIMEOUT_RETRY_HOLD_SHIFT 16
#define BUS_TIMEOUT_RETRY_HOLD  (0x1fu << BUS_TIMEOUT_RETRY_HOLD_SHIFT)
#define BUS_TIMEOUT_GIO         BIT(12)
#define BUS_TIMEOUT_PCI_RETRY   0x3ffu

#define BUS_TIMEOUT_RESET       (1u << BUS_TIMEOUT_RETRY_HOLD_SHIFT)
#define BUS_TIMEOUT_WRITABLE    (BUS_TIMEOUT_RETRY_HOLD | BUS_TIMEOUT_GIO | \
                                 BUS_TIMEOUT_PCI_RETRY)

/* Bus and device for a type 1 cycle; the rest is reserved. */
#define TYPE1_BUS_NUMBER        (0xffu << 16)
#define TYPE1_DEVICE_NUMBER     (0x1fu << 11)
#define TYPE1_WRITABLE          (TYPE1_BUS_NUMBER | TYPE1_DEVICE_NUMBER)

/*
 * Per-slot device register. Three bits reset set, the memory-or-I/O select
 * at bit 12 included. The specification and Linux name the page-cross bit
 * oppositely (enable vs disable) while describing it identically.
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

/*
 * The window base field supplies PCI address bits 31:20; windows lie end to
 * end, 2n MB for the first two slots and n+2 for the rest. The reset value
 * comes from the firmware, whose sanity diagnostic asserts all eight -- the
 * specification gives this field no power-up value.
 */
#define DEVICE_WINDOW_RESET(n)  ((n) < 2 ? 2 * (n) : (n) + 2)
#define DEVICE_RESET(n)         (DEV_ERROR_LOCK | DEV_PAGE_CROSS_CHECK | \
                                 DEV_MEMORY_SPACE | DEVICE_WINDOW_RESET(n))
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
 * Kept apart because a write to a read-only register is a guest bug worth
 * reporting, while a write to a strobe is the chip used correctly.
 */
typedef enum {
    SGI_BRIDGE_RO,      /* writes are a guest error and are reported */
    SGI_BRIDGE_RW,      /* the writable mask says which bits a guest may set */
    SGI_BRIDGE_WO,      /* a strobe: a write is taken, a read gives the reset */
    SGI_BRIDGE_LIVE,    /* read-only and computed: no storage to go stale */
} SGIBridgeRegAccess;

/*
 * One row per defined register: a decode cannot exist without a reset value
 * and writable mask. Deliberately not hw/core/register.h's
 * RegisterAccessInfo, which builds its own region at a four-byte stride and
 * cannot post undefined addresses. Non-zero reset values come from the
 * chip's specification (the public headers carry none); a zero is either
 * the specification's or, where marked, this model's choice.
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
        "device", SGI_BRIDGE_RW, DEVICE_RESET(n), DEVICE_WRITABLE }
#define SGI_BRIDGE_DEVICE_FLUSH_ROW(n)                                        \
    [SGI_BRIDGE_SLOT(BRIDGE_DEVICE_FLUSH(n))] = {                             \
        "device write request buffer flush", SGI_BRIDGE_RO, 0, 0 }

static const SGIBridgeReg sgi_bridge_regs[SGI_BRIDGE_REG_SLOTS] = {
    /* The identity is the machine's; reset composes it from properties. */
    [SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] = {
        "identification", SGI_BRIDGE_RO, 0, 0 },

    /* Specification: retry counters and outstanding reads reset zero. */
    [SGI_BRIDGE_SLOT(BRIDGE_STATUS)] = {
        "status", SGI_BRIDGE_RO, STATUS_PCI_MODE, 0 },

    /*
     * The timing fields are the writable part; the level and finished flag
     * are answers the model writes when a slot runs.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_NIC)] = {
        "one-wire bus master", SGI_BRIDGE_RW, 0, NIC_WRITABLE },

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

    /* No specified power-up value; zero is this model's choice. */
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
     * Reading it is the operation; the answer means nothing. The one thing
     * this register must never do is fault.
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

    /* The type 1 window is not modelled; the register is storage. */
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_TYPE1_CONFIG)] = {
        "PCI type 1 configuration", SGI_BRIDGE_RW, 0, TYPE1_WRITABLE },

    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_UPPER)] = {
        "PCI error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_LOWER)] = {
        "PCI error address lower", SGI_BRIDGE_RO, 0, 0 },

    /*
     * The pin bits must follow the level and never latch: the guest re-arms
     * by re-reading this register after each interrupt it services, and a
     * latched bit would report a device with a second condition pending as
     * quiet, never to be serviced again. So the pins are composed on every
     * read (LIVE) and only the error bits latch.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_STATUS)] = {
        "interrupt status", SGI_BRIDGE_LIVE, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] = {
        "interrupt enable", SGI_BRIDGE_RW, 0, INT_ENABLE_WRITABLE },

    /*
     * A one in a group bit clears that group's latched errors; the register
     * itself is a strobe, not storage.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_RESET)] = {
        "reset interrupt status", SGI_BRIDGE_WO, 0, INT_RESET_GROUPS },

    /*
     * Read-modify-written by the guest; a read answering zero would wipe
     * every previously configured pin on each new establishment.
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
     * Storage: this model completes every read before it returns, so the
     * buffer allocation has no observable effect.
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
 * Aim a slot's window where its register says. For a 2 MB window bit 20
 * arrives from both the base and the offset; adding them is OpenBSD's own
 * arithmetic. A region's target space cannot change once it exists, so each
 * slot has two windows and exactly one is enabled.
 */
static void sgi_bridge_aim_window(SGIBridgeState *s, unsigned int slot)
{
    uint32_t control = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];
    hwaddr base = (hwaddr)(control & DEV_WINDOW_BASE) <<
                  BRIDGE_WINDOW_BASE_SHIFT;
    bool memory = control & DEV_MEMORY_SPACE;

    s->device_window_base[slot] = base;
    memory_region_set_enabled(&s->device_window_mem[slot], memory);
    memory_region_set_enabled(&s->device_window_io[slot], !memory);
}

/*
 * Used by both reset and post_load: the windows are derived from the
 * per-slot registers, so only the registers travel in the stream.
 */
static void sgi_bridge_aim_all_windows(SGIBridgeState *s)
{
    unsigned int slot;

    for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
        sgi_bridge_aim_window(s, slot);
    }
}

void sgi_bridge_assign_window(SGIBridgeState *s, unsigned int slot,
                              hwaddr pci_base, bool memory_space)
{
    uint32_t *held = &s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];
    uint32_t assigned = DEV_WINDOW_BASE | DEV_MEMORY_SPACE;
    uint32_t base = (uint32_t)(pci_base >> BRIDGE_WINDOW_BASE_SHIFT) &
                    DEV_WINDOW_BASE;

    /*
     * Masked like a guest's own write: a plain overwrite would silently
     * clear the error-lock and page-cross-check bits.
     */
    *held = (*held & ~assigned) | (base | (memory_space ? DEV_MEMORY_SPACE
                                                        : 0));
    sgi_bridge_aim_window(s, slot);
}

/*
 * Two callers with different needs: the programmed-access windows, because
 * the specification's byte-lane tables (Appendix A, Tables 65 and 73) land
 * a narrow access reversed within its word with DEV_SWAP_PROGRAMMED
 * disabled -- its reset state, and neither guest ever sets it; and the DMA
 * path, because the IOC3 is big-endian and both drivers clear its swap
 * bits. Which slots are exempt is board policy handed down as
 * byte_lane_exempt -- real silicon cannot read a PCI vendor ID.
 */
static bool sgi_bridge_slot_needs_correction(SGIBridgeState *s,
                                             unsigned int slot)
{
    return !(s->byte_lane_exempt & (1u << slot));
}

/*
 * Only the low two bits move: a transfer never crosses the 32-bit word its
 * lanes are defined within.
 */
static hwaddr sgi_bridge_window_correct(hwaddr offset, unsigned int size)
{
    return (offset & ~3u) | ((offset & 3u) ^ (4u - size));
}


/*
 * A device-window access, corrected if the slot needs it, delivered into
 * the selected PCI space. The containers have no ops of their own, so this
 * goes through address_space_read/write -- which move raw bytes, so the
 * only byte order in play is this window's DEVICE_LITTLE_ENDIAN
 * declaration against the big-endian packing below: one swap at the
 * window boundary, independent of the host's own endianness.
 */
/*
 * One device-window cycle of up to four bytes, corrected if the slot needs it:
 * a byte, halfword or word, and each half of a 32-bit device's split
 * doubleword. impl.max caps size at four here; the assert makes that bound
 * visible to the compiler's array-bounds check, the eight-byte doubleword being
 * the caller's.
 */
static uint64_t sgi_bridge_window_read_word(SGIBridgeState *s,
                                            unsigned int slot,
                                            AddressSpace *space, hwaddr addr,
                                            unsigned int size)
{
    hwaddr offset = sgi_bridge_slot_needs_correction(s, slot) ?
                    sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = s->device_window_base[slot] + offset;
    uint8_t bytes[4] = { 0 };

    assert(size <= sizeof(bytes));
    address_space_read(space, target, MEMTXATTRS_UNSPECIFIED, bytes, size);
    return ldn_be_p(bytes, size);
}

/*
 * A device-window read. The slot's device-size bit picks how a doubleword is
 * served: a 32-bit device gets two word cycles (big-endian high word at the
 * lower offset, each corrected), a 64-bit device one eight-byte cycle with no
 * narrow correction, mirrored end to end on the programmed-access swap bit.
 * The window is DEVICE_LITTLE_ENDIAN, so reading the buffer big-endian nets to
 * the doubleword unchanged and little-endian to the mirror. A byte, halfword or
 * word is one corrected cycle.
 */
static uint64_t sgi_bridge_window_read(SGIBridgeState *s, unsigned int slot,
                                       AddressSpace *space, hwaddr addr,
                                       unsigned int size)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];

    if (size == 8 && (dev & DEV_64BIT_DEVICE)) {
        uint8_t bytes[8] = { 0 };

        address_space_read(space, s->device_window_base[slot] + addr,
                           MEMTXATTRS_UNSPECIFIED, bytes, 8);
        return (dev & DEV_SWAP_PROGRAMMED) ? ldq_le_p(bytes) : ldq_be_p(bytes);
    }

    if (size == 8) {
        uint64_t word0 = sgi_bridge_window_read_word(s, slot, space, addr, 4);
        uint64_t word1 = sgi_bridge_window_read_word(s, slot, space,
                                                     addr + 4, 4);

        return word0 | (word1 << 32);
    }

    return sgi_bridge_window_read_word(s, slot, space, addr, size);
}

/* One device-window write cycle of up to four bytes, the counterpart of
 * sgi_bridge_window_read_word. */
static void sgi_bridge_window_write_word(SGIBridgeState *s, unsigned int slot,
                                         AddressSpace *space, hwaddr addr,
                                         uint64_t val, unsigned int size)
{
    hwaddr offset = sgi_bridge_slot_needs_correction(s, slot) ?
                    sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = s->device_window_base[slot] + offset;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_be_p(bytes, size, val);
    address_space_write(space, target, MEMTXATTRS_UNSPECIFIED, bytes, size);
}

/*
 * The write counterpart of sgi_bridge_window_read: the device-size bit picks the
 * same split or single cycle and the swap bit mirrors a 64-bit doubleword, so a
 * value round-trips through a 64-bit window under either swap setting.
 */
static void sgi_bridge_window_write(SGIBridgeState *s, unsigned int slot,
                                    AddressSpace *space, hwaddr addr,
                                    uint64_t val, unsigned int size)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];

    if (size == 8 && (dev & DEV_64BIT_DEVICE)) {
        uint8_t bytes[8];

        if (dev & DEV_SWAP_PROGRAMMED) {
            stq_le_p(bytes, val);
        } else {
            stq_be_p(bytes, val);
        }
        address_space_write(space, s->device_window_base[slot] + addr,
                            MEMTXATTRS_UNSPECIFIED, bytes, 8);
        return;
    }

    if (size == 8) {
        sgi_bridge_window_write_word(s, slot, space, addr, val, 4);
        sgi_bridge_window_write_word(s, slot, space, addr + 4, val >> 32, 4);
        return;
    }

    sgi_bridge_window_write_word(s, slot, space, addr, val, size);
}

static uint64_t sgi_bridge_window_mem_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    return sgi_bridge_window_read(win->s, win->slot, &win->s->pci_mmio_as,
                                  addr, size);
}

static void sgi_bridge_window_mem_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    sgi_bridge_window_write(win->s, win->slot, &win->s->pci_mmio_as, addr,
                            val, size);
}

static uint64_t sgi_bridge_window_io_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    return sgi_bridge_window_read(win->s, win->slot, &win->s->pci_io_as, addr,
                                  size);
}

static void sgi_bridge_window_io_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    sgi_bridge_window_write(win->s, win->slot, &win->s->pci_io_as, addr, val,
                            size);
}

/*
 * A doubleword is a native access here (the firmware's device-pointer
 * dereference is one). Both maxima are eight so it reaches the handlers whole:
 * a 64-bit device is served one cycle, a 32-bit device the two four-byte cycles
 * the handler now splits itself, since impl past four stops the core doing it.
 * The byte-lane correction still sees each four-byte cycle in its own right.
 */
static const MemoryRegionOps sgi_bridge_window_mem_ops = {
    .read = sgi_bridge_window_mem_read,
    .write = sgi_bridge_window_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps sgi_bridge_window_io_ops = {
    .read = sgi_bridge_window_io_read,
    .write = sgi_bridge_window_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/* A write to a per-device register re-aims that slot's window. */
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
 * The real chip posts an undefined register access and flags it
 * asynchronously through interrupt status bit 24 -- a firmware self-test
 * provokes one deliberately and fails on any synchronous exception. Still
 * logged: a genuine probe and a coverage gap look alike from here.
 */
static void sgi_bridge_raise_host_error(SGIBridgeState *s);

static void sgi_bridge_invalid_address(SGIBridgeState *s, hwaddr addr,
                                       bool is_write)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-bridge: %s of undefined register 0x%" HWADDR_PRIx
                  "; posting it and flagging an invalid-address error\n",
                  is_write ? "write" : "read", addr);
    s->int_error |= INT_INVALID_ADDR;

    /* The bit always latches; the interrupt goes only when enabled. */
    if (s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] & INT_INVALID_ADDR) {
        sgi_bridge_raise_host_error(s);
    }
}

/* Composed on read rather than held, so the pin bits cannot latch. */
static uint32_t sgi_bridge_live(SGIBridgeState *s, hwaddr addr)
{
    switch (addr - SGI_BRIDGE_REG_DATA) {
    case BRIDGE_INT_STATUS:
        return (s->pin_level & INT_DEVICE_PINS) | s->int_error;
    default:
        g_assert_not_reached();
    }
}

static uint64_t sgi_bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);

    /* An undefined address in the block posts and flags rather than faulting. */
    if (!reg) {
        sgi_bridge_invalid_address(s, addr, false);
        return ~(uint64_t)0;
    }

    switch (reg->access) {
    case SGI_BRIDGE_LIVE:
        return sgi_bridge_live(s, addr);
    case SGI_BRIDGE_WO:
        return reg->reset;
    default:
        return s->reg[addr / SGI_BRIDGE_REG_SLOT];
    }
}

/*
 * The CRC the guest runs over the exchange: reflected polynomial from zero,
 * and the part sends the result INVERTED -- the guest checks for a fixed
 * residue over the whole exchange, and an uninverted check lands elsewhere.
 */
static uint16_t sgi_bridge_onewire_crc(uint16_t crc, const uint8_t *data,
                                       unsigned int len)
{
    unsigned int i;
    unsigned int bit;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

/* Family, six serial bytes, then the Dallas/Maxim CRC-8 over the seven. */
static uint64_t sgi_bridge_onewire_rom_id(uint64_t serial)
{
    uint64_t identity = NIC_ROM_FAMILY | (serial << 8);
    uint64_t data = identity;
    uint8_t crc = 0;
    unsigned int byte, bit;

    for (byte = 0; byte < 7; byte++) {
        crc ^= data & 0xff;
        data >>= 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }

    return identity | ((uint64_t)crc << 56);
}

/*
 * One page, two readers. One guest takes offsets 11-29 and 32-37
 * positionally as the part number; the other scans for name:value; fields.
 * Placing the five-byte field name at offsets 6-10 puts its value at 11,
 * satisfying both. The part number is the one a real Origin 200 reports.
 *
 * Byte 0 must be 1 or the PROM rejects the page. Byte 5's semicolon is
 * LOAD-BEARING: the scanner skips past a semicolon on a miss, and without
 * this one it overshoots the field name at 6 and the lookup silently
 * returns empty.
 */
static const char sgi_bridge_onewire_image[NIC_PAGE_SIZE * NIC_PAGES] = {
    [0]  = 0x01, ' ', ' ', ' ', ' ', ';',
    [6]  = 'P', 'a', 'r', 't', ':',
    [11] = '0', '3', '0', '-', '1', '2', '4', '4', '-', '0', '0', '1',
    [23] = ';', ' ', ' ', ' ', ' ', ' ', ' ',
    [32] = ' ', ' ', ' ', ' ', ' ', ' ',
};

/*
 * The stored image: programmed text plus each page's check in its own last
 * two bytes, stored rather than manufactured because two commands read the
 * same memory with different framing. Past the programmed pages, EPROM
 * reads unprogrammed.
 */
static void sgi_bridge_onewire_build_rom(SGIBridgeState *s)
{
    unsigned int page;

    memset(s->onewire_rom, NIC_UNPROGRAMMED, sizeof(s->onewire_rom));
    memcpy(s->onewire_rom, sgi_bridge_onewire_image,
           sizeof(sgi_bridge_onewire_image));

    for (page = 0; page < NIC_PAGES; page++) {
        uint8_t *at = &s->onewire_rom[page * NIC_PAGE_SIZE];
        uint16_t crc = ~sgi_bridge_onewire_crc(0, at, NIC_PAGE_SIZE - 2);

        at[NIC_PAGE_SIZE - 2] = crc & 0xff;
        at[NIC_PAGE_SIZE - 1] = crc >> 8;
    }
}

/* One byte of the data memory, wherever it falls. */
static uint8_t sgi_bridge_onewire_data(SGIBridgeState *s, unsigned int at)
{
    return at < sizeof(s->onewire_rom) ? s->onewire_rom[at]
                                       : NIC_UNPROGRAMMED;
}

/*
 * A plain read sends stored bytes to the end of the data memory with one
 * check at the very end, worked out here in one pass.
 */
static void sgi_bridge_onewire_plain_answer(SGIBridgeState *s)
{
    unsigned int at = s->onewire_args[0] | (s->onewire_args[1] << 8);
    uint8_t request[3];
    uint16_t crc;
    unsigned int i;

    request[0] = NIC_CMD_READ_PLAIN;
    request[1] = s->onewire_args[0];
    request[2] = s->onewire_args[1];

    trace_sgi_bridge_onewire_plain(at, at < NIC_DATA_SIZE);

    if (at >= NIC_DATA_SIZE) {
        s->onewire_phase = NIC_PHASE_IDLE;
        return;
    }

    crc = sgi_bridge_onewire_crc(0, request, sizeof(request));
    for (i = at; i < NIC_DATA_SIZE; i++) {
        uint8_t byte = sgi_bridge_onewire_data(s, i);

        crc = sgi_bridge_onewire_crc(crc, &byte, 1);
    }
    crc = ~crc;

    s->onewire_read = at;
    s->onewire_end[0] = crc & 0xff;
    s->onewire_end[1] = crc >> 8;
    s->onewire_outbit = 0;
    s->onewire_phase = NIC_PHASE_READ;
}

/*
 * One status page plus its check pair. Only the first page's check covers
 * the request; later pages are checked over their data alone -- the part's
 * own asymmetry, since this memory is programmable after the fact.
 */
static void sgi_bridge_onewire_status_page(SGIBridgeState *s, bool first)
{
    unsigned int end = s->onewire_status | (NIC_STATUS_PAGE - 1);
    uint8_t request[3];
    uint16_t crc = 0;
    unsigned int n = 0;

    while (s->onewire_status + n <= end) {
        s->onewire_out[n] = NIC_STATUS_UNPROGRAMMED;
        n++;
    }

    if (first) {
        request[0] = NIC_CMD_READ_STATUS;
        request[1] = s->onewire_args[0];
        request[2] = s->onewire_args[1];
        crc = sgi_bridge_onewire_crc(0, request, sizeof(request));
    }
    crc = ~sgi_bridge_onewire_crc(crc, s->onewire_out, n);
    s->onewire_out[n++] = crc & 0xff;
    s->onewire_out[n++] = crc >> 8;

    trace_sgi_bridge_onewire_status(s->onewire_status, first);
    s->onewire_status = end + 1;
    s->onewire_outlen = n;
    s->onewire_outbit = 0;
}

/* Past the end of the status memory the part idles and reads as ones. */
static void sgi_bridge_onewire_status_answer(SGIBridgeState *s)
{
    s->onewire_status = s->onewire_args[0] | (s->onewire_args[1] << 8);

    if (s->onewire_status > NIC_STATUS_LAST) {
        trace_sgi_bridge_onewire_status(s->onewire_status, true);
        s->onewire_phase = NIC_PHASE_IDLE;
        return;
    }

    sgi_bridge_onewire_status_page(s, true);
    s->onewire_phase = NIC_PHASE_STATUS;
}

/*
 * One page's whole answer, built in one go: the checks span the request,
 * so nothing is known until its last byte arrives.
 */
static void sgi_bridge_onewire_answer(SGIBridgeState *s)
{
    unsigned int offset = s->onewire_args[0] | (s->onewire_args[1] << 8);
    uint8_t request[4];
    uint16_t crc;

    if (offset > sizeof(sgi_bridge_onewire_image) - NIC_PAGE_SIZE) {
        trace_sgi_bridge_onewire_page(offset, true);
        offset = 0;             /* a page this part does not have */
    } else {
        trace_sgi_bridge_onewire_page(offset, false);
    }

    request[0] = NIC_CMD_READ_MEMORY;
    request[1] = s->onewire_args[0];
    request[2] = s->onewire_args[1];
    request[3] = NIC_NOT_REDIRECTED;

    s->onewire_out[0] = NIC_NOT_REDIRECTED;
    crc = ~sgi_bridge_onewire_crc(0, request, sizeof(request));
    s->onewire_out[1] = crc & 0xff;
    s->onewire_out[2] = crc >> 8;

    memcpy(&s->onewire_out[3], &s->onewire_rom[offset], NIC_PAGE_SIZE);

    /*
     * The page's embedded check is already inside the stored block; this
     * framing pair covers the whole thirty-two bytes, embedded check
     * included, and is a different check a page can fail independently.
     */
    crc = ~sgi_bridge_onewire_crc(0, &s->onewire_out[3], NIC_PAGE_SIZE);
    s->onewire_out[3 + NIC_PAGE_SIZE] = crc & 0xff;
    s->onewire_out[4 + NIC_PAGE_SIZE] = crc >> 8;

    s->onewire_outbit = 0;
    s->onewire_phase = NIC_PHASE_ANSWER;
}

static void sgi_bridge_onewire_slot(SGIBridgeState *s, uint32_t val)
{
    uint32_t *held = &s->reg[SGI_BRIDGE_SLOT(BRIDGE_NIC)];
    uint32_t pulse = (val & NIC_PULSE_MASK) >> NIC_PULSE_SHIFT;
    bool level = true;

    if (pulse >= NIC_PULSE_IS_RESET) {
        /* A reset: the part pulls the wire down, which reads as presence. */
        trace_sgi_bridge_onewire_reset(s->onewire_phase, s->onewire_outbit);
        s->onewire_phase = NIC_PHASE_COMMAND;
        s->onewire_bit = 0;
        s->onewire_command = 0;
        s->onewire_outbit = 0;
        level = false;
    } else if (s->onewire_phase == NIC_PHASE_COMMAND) {
        /* A command, least-significant bit first, one slot per bit. */
        if (pulse < NIC_PULSE_IS_ZERO) {
            s->onewire_command |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            trace_sgi_bridge_onewire_command(s->onewire_command);
            switch (s->onewire_command) {
            case NIC_CMD_SEARCH_ROM:
                s->onewire_phase = NIC_PHASE_SEARCH;
                break;
            case NIC_CMD_MATCH_ROM:
                s->onewire_match = 0;
                s->onewire_phase = NIC_PHASE_MATCH;
                break;
            case NIC_CMD_SKIP_ROM:
                s->onewire_command = 0;
                s->onewire_phase = NIC_PHASE_FUNCTION;
                break;
            default:
                /*
                 * The wire idles high, so the guest cannot tell an
                 * unimplemented command from an absent part; say so here.
                 */
                qemu_log_mask(LOG_UNIMP,
                              "sgi-bridge: one-wire rom command 0x%02x is not "
                              "answered by the board identity part\n",
                              s->onewire_command);
                s->onewire_phase = NIC_PHASE_IDLE;
                break;
            }
        }
    } else if (s->onewire_phase == NIC_PHASE_SEARCH) {
        /*
         * Three slots per identity bit: the bit, its complement, the
         * guest's choice. With one part on the bus the choice always
         * matches, so the third slot is ignored.
         */
        unsigned int step = s->onewire_bit % 3;
        unsigned int index = s->onewire_bit / 3;
        bool bit = (s->onewire_identity >> index) & 1;

        if (step == 0) {
            level = bit;
        } else if (step == 1) {
            level = !bit;
        }

        if (++s->onewire_bit == NIC_ROM_BITS * 3) {
            s->onewire_bit = 0;
            s->onewire_phase = NIC_PHASE_IDLE;
            trace_sgi_bridge_onewire_search_done(s->onewire_identity);
        }
    } else if (s->onewire_phase == NIC_PHASE_MATCH) {
        /*
         * The named identity, least-significant bit first. A mismatched
         * part goes silent until reset -- indistinguishable from absence,
         * so the mismatch is reported.
         */
        if (pulse < NIC_PULSE_IS_ZERO) {
            s->onewire_match |= 1ULL << s->onewire_bit;
        }
        if (++s->onewire_bit == NIC_ROM_BITS) {
            s->onewire_bit = 0;
            if (s->onewire_match == s->onewire_identity) {
                s->onewire_phase = NIC_PHASE_FUNCTION;
                s->onewire_command = 0;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-bridge: one-wire match names identity "
                              "0x%016" PRIx64 ", but the board identity part "
                              "is 0x%016" PRIx64 "\n",
                              s->onewire_match, s->onewire_identity);
                s->onewire_phase = NIC_PHASE_IDLE;
            }
            trace_sgi_bridge_onewire_match(s->onewire_match,
                                           s->onewire_phase ==
                                           NIC_PHASE_FUNCTION);
        }
    } else if (s->onewire_phase == NIC_PHASE_FUNCTION) {
        /* The function asked of the selected part, a bit at a time. */
        if (pulse < NIC_PULSE_IS_ZERO) {
            s->onewire_command |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            trace_sgi_bridge_onewire_function(s->onewire_command);
            s->onewire_argn = 0;
            s->onewire_args[0] = 0;
            s->onewire_args[1] = 0;
            if (s->onewire_command == NIC_CMD_READ_MEMORY ||
                s->onewire_command == NIC_CMD_READ_STATUS ||
                s->onewire_command == NIC_CMD_READ_PLAIN) {
                s->onewire_phase = NIC_PHASE_OFFSET;
            } else {
                /* Same silence as an unanswered rom command, one step later. */
                qemu_log_mask(LOG_UNIMP,
                              "sgi-bridge: one-wire memory function 0x%02x is "
                              "not answered by the board identity part\n",
                              s->onewire_command);
                s->onewire_phase = NIC_PHASE_IDLE;
            }
        }
    } else if (s->onewire_phase == NIC_PHASE_OFFSET) {
        /* Two bytes of page offset, low first, still one slot per bit. */
        if (pulse < NIC_PULSE_IS_ZERO) {
            s->onewire_args[s->onewire_argn] |= 1u << s->onewire_bit;
        }
        if (++s->onewire_bit == 8) {
            s->onewire_bit = 0;
            if (++s->onewire_argn == 2) {
                if (s->onewire_command == NIC_CMD_READ_STATUS) {
                    sgi_bridge_onewire_status_answer(s);
                } else if (s->onewire_command == NIC_CMD_READ_PLAIN) {
                    sgi_bridge_onewire_plain_answer(s);
                } else {
                    sgi_bridge_onewire_answer(s);
                }
            }
        }
    } else if (s->onewire_phase == NIC_PHASE_READ) {
        /*
         * Stored bytes with no framing; the two bytes past the end are the
         * check over everything sent, and past those the part goes quiet.
         */
        unsigned int at = s->onewire_read + s->onewire_outbit / 8;
        unsigned int bit = s->onewire_outbit % 8;
        uint8_t byte;

        if (at < NIC_DATA_SIZE) {
            byte = sgi_bridge_onewire_data(s, at);
        } else {
            byte = s->onewire_end[at - NIC_DATA_SIZE];
        }
        level = (byte >> bit) & 1;

        if (++s->onewire_outbit ==
            (NIC_DATA_SIZE + 2 - s->onewire_read) * 8) {
            s->onewire_outbit = 0;
            s->onewire_phase = NIC_PHASE_IDLE;
            trace_sgi_bridge_onewire_plain_done(s->onewire_read);
        }
    } else if (s->onewire_phase == NIC_PHASE_STATUS) {
        /* Page after page, each with its check pair, until the end. */
        unsigned int byte = s->onewire_outbit / 8;

        level = (s->onewire_out[byte] >> (s->onewire_outbit % 8)) & 1;
        if (++s->onewire_outbit == s->onewire_outlen * 8) {
            if (s->onewire_status > NIC_STATUS_LAST) {
                s->onewire_outbit = 0;
                s->onewire_phase = NIC_PHASE_IDLE;
            } else {
                sgi_bridge_onewire_status_page(s, false);
            }
        }
    } else if (s->onewire_phase == NIC_PHASE_ANSWER) {
        /* The answer, least-significant bit of each byte first. */
        unsigned int byte = s->onewire_outbit / 8;

        level = (s->onewire_out[byte] >> (s->onewire_outbit % 8)) & 1;
        if (++s->onewire_outbit == NIC_ANSWER_SIZE * 8) {
            s->onewire_outbit = 0;
            s->onewire_phase = NIC_PHASE_IDLE;
            trace_sgi_bridge_onewire_answer_done(NIC_ANSWER_SIZE);
        }
    }

    *held &= ~NIC_LEVEL;
    *held |= NIC_DONE | (level ? NIC_LEVEL : 0);
}

static void sgi_bridge_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);
    uint32_t *held;
    unsigned int slot;

    /* An undefined address in the block posts and flags rather than faulting. */
    if (!reg) {
        sgi_bridge_invalid_address(s, addr, true);
        return;
    }

    held = &s->reg[addr / SGI_BRIDGE_REG_SLOT];

    switch (reg->access) {
    case SGI_BRIDGE_LIVE:
    case SGI_BRIDGE_RO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: write of 0x%" PRIx64 " to the read-only %s "
                      "register at 0x%" HWADDR_PRIx "\n", val, reg->name, addr);
        return;
    case SGI_BRIDGE_WO:
        /*
         * A strobe. The one with an effect clears latched errors by group:
         * the invalid-address bit is in the request-dispatch group, so only
         * that group's strobe clears it.
         */
        if (addr - SGI_BRIDGE_REG_DATA == BRIDGE_INT_RESET &&
            ((uint32_t)val & INT_RESET_REQ_DSP_GRP)) {
            s->int_error &= ~INT_INVALID_ADDR;
        }
        return;
    case SGI_BRIDGE_RW:
        *held = (*held & ~reg->writable) | ((uint32_t)val & reg->writable);
        if (sgi_bridge_is_device_register(addr, &slot)) {
            /*
             * A guest enabling swap on an exempt slot asks for something
             * the model does not reproduce; say so at the write.
             */
            if ((*held & (DEV_SWAP_DIRECT | DEV_SWAP_TRANSLATED)) &&
                !sgi_bridge_slot_needs_correction(s, slot)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-bridge: slot %u holds a device this model "
                              "exempts from the DMA byte swap, and the guest "
                              "enabled swapping for it; the transfer will not "
                              "be permuted\n", slot);
            }
            sgi_bridge_aim_window(s, slot);
        } else if (addr - SGI_BRIDGE_REG_DATA == BRIDGE_NIC) {
            sgi_bridge_onewire_slot(s, (uint32_t)val);
        }
        return;
    }

    g_assert_not_reached();
}

/*
 * Every register access is a crosstalk double-word; any other width is an
 * error. Both bounds are declared so the core rejects instead of reshaping.
 */
static const MemoryRegionOps sgi_bridge_reg_ops = {
    .read = sgi_bridge_read,
    .write = sgi_bridge_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static bool sgi_bridge_reserved_data_half_accepts(void *opaque, hwaddr addr,
                                                  unsigned size, bool is_write,
                                                  MemTxAttrs attrs)
{
    if (!is_write) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bridge: read of register 0x%" HWADDR_PRIx " which "
                      "this model does not implement; raising a bus error "
                      "rather than answering\n",
                      (hwaddr)BRIDGE_RESERVED_DATA_HALF);
    }
    return is_write;
}

static MemTxResult sgi_bridge_reserved_data_half_write(void *opaque,
                                                        hwaddr addr,
                                                        uint64_t val,
                                                        unsigned size,
                                                        MemTxAttrs attrs)
{
    if (val != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bridge: nonzero write of 0x%" PRIx64 " to "
                      "reserved register 0x%" HWADDR_PRIx "; raising a "
                      "bus error\n", val,
                      (hwaddr)BRIDGE_RESERVED_DATA_HALF);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps sgi_bridge_reserved_data_half_ops = {
    .write_with_attrs = sgi_bridge_reserved_data_half_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.accepts = sgi_bridge_reserved_data_half_accepts,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * The external-SSRAM window, unpopulated on this board. The PROM sizes it
 * with an unguarded write-and-read-back probe, so accesses must complete
 * and reads must float high: all-ones differs from any probe pattern and
 * the size probes as zero.
 */
static uint64_t sgi_bridge_ssram_read(void *opaque, hwaddr addr, unsigned size)
{
    return ~(uint64_t)0;
}

static void sgi_bridge_ssram_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
}

static const MemoryRegionOps sgi_bridge_ssram_ops = {
    .read = sgi_bridge_ssram_read,
    .write = sgi_bridge_ssram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * Only two shapes are accepted: a 64-bit write at an entry's offset and a
 * 32-bit read four bytes above it. A 32-bit write would leave half an
 * entry behind.
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
 * The read-only lower-half view; a write is refused rather than swallowed.
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
 * A narrow configuration access aims at the window address whose byte
 * lanes hold the bytes it wants -- Linux exclusive-ors the register offset
 * with four minus the width -- and undoing that here is what makes a byte
 * read of a vendor ID return the vendor's low byte. OpenBSD reads in
 * 32-bit units only, where the correction is zero.
 */
static uint32_t sgi_bridge_config_address(hwaddr addr, unsigned size)
{
    unsigned int reg = addr & BRIDGE_CFG0_REGISTER;
    unsigned int devfn = PCI_DEVFN(BRIDGE_CFG0_DEVICE(addr),
                                   BRIDGE_CFG0_FUNCTION(addr));

    reg = sgi_bridge_window_correct(reg, size);

    return (devfn << 8) | reg;
}

static uint64_t sgi_bridge_config_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);

    /*
     * An empty slot answers all ones -- the value OpenBSD substitutes
     * itself on a faulting load; zero would be a real device.
     */
    trace_sgi_bridge_config_read(addr, size);
    return pci_data_read(phb->bus, sgi_bridge_config_address(addr, size), size);
}

static void sgi_bridge_config_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);

    trace_sgi_bridge_config_write(addr, size, val);
    pci_data_write(phb->bus, sgi_bridge_config_address(addr, size), val, size);
}

/*
 * Both bounds declared: the width the guest issued is a term in the
 * byte-lane correction, so a reshaped access would read the wrong byte.
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
 * INTx folding: pins A and C reach the pin numbered as the slot, B and D
 * that number with bit two flipped -- the arithmetic both guests compute
 * for themselves (Linux's table, OpenBSD's low-bit test). The base I/O
 * controller's second interrupt is not a PCI interrupt at all; the machine
 * wires it straight to a chip pin and nothing here covers it.
 */
static int sgi_bridge_map_irq(PCIDevice *dev, int pin)
{
    int slot = PCI_SLOT(dev->devfn);

    return (pin & 1) ? (slot ^ 4) : slot;
}

/*
 * A pin's interrupt-address register holds its destination compressed:
 * bits 17:8 are crosstalk address bits 47:38 (the top bit, then the node),
 * bits 7:0 the interrupt number. SGI's mask, OpenBSD's and Linux's
 * arithmetic all agree.
 */
#define INT_HOST_ADDR_SHIFT     8
#define XTALK_NODE_SHIFT        38

/* Where the Hub's interrupt-modify register sits inside a node's Hub window. */
#define HUB_NODE_WINDOW         0x01800000
#define HUB_INT_MODIFY          0x90
#define HUB_INT_MODIFY_SET      0x100

/*
 * An interrupt packet: a write to the Hub's interrupt-modify register,
 * data bit 8 selecting raise over lower. The fabric is addressed the
 * machine's way, which is why its constants come from the machine.
 */
static void sgi_bridge_deliver_interrupt(SGIBridgeState *s, unsigned int node,
                                         uint64_t offset, unsigned int bit,
                                         bool set)
{
    uint64_t value = bit | (set ? HUB_INT_MODIFY_SET : 0);
    hwaddr where = s->fabric_base +
                   ((uint64_t)node << s->fabric_node_shift) + offset;

    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

/*
 * A device pin's transition takes its destination and its host bit from that
 * pin's own interrupt-address register. Bits 17:8 of that register are crosstalk
 * address bits 47:38, which name the destination node; the Hub's window and its
 * interrupt-modify register complete the offset.
 */
static void sgi_bridge_send_interrupt(SGIBridgeState *s, int pin, bool set)
{
    uint64_t addr = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_HOST(pin))];
    unsigned int node;

    if (!addr) {
        /* An unprogrammed destination is an interrupt nobody routed. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: interrupt pin %d changed with no "
                      "destination programmed; no packet sent\n", pin);
        return;
    }

    node = ((addr & INT_HOST_ADDRESS) >> INT_HOST_ADDR_SHIFT) & 0x1ff;
    sgi_bridge_deliver_interrupt(s, node, HUB_NODE_WINDOW + HUB_INT_MODIFY,
                                 addr & INT_HOST_FIELD, set);
}

/*
 * The chip's own errors go to the host bit in the host-error field, at the
 * full crosstalk address in the widget interrupt-destination pair.
 */
static void sgi_bridge_raise_host_error(SGIBridgeState *s)
{
    unsigned int number = s->reg[SGI_BRIDGE_SLOT(BRIDGE_HOST_ERR_FIELD)] &
                          INT_HOST_FIELD;
    uint64_t upper = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_UPPER)];
    uint64_t lower = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_LOWER)];
    uint64_t dest = ((upper & 0xffffu) << 32) | (uint32_t)lower;
    unsigned int node;

    if (!dest) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: invalid-address error with no destination "
                      "programmed; no packet sent\n");
        return;
    }

    node = (dest >> XTALK_NODE_SHIFT) & 0x1ff;
    sgi_bridge_deliver_interrupt(s, node,
                                 dest & ((1ULL << XTALK_NODE_SHIFT) - 1),
                                 number, true);
}

static void sgi_bridge_update_pin(SGIBridgeState *s, int pin)
{
    bool level = extract32(s->pci_pin_level | s->gpio_pin_level, pin, 1);
    bool was = extract32(s->pin_level, pin, 1);
    bool enabled = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] & (1u << pin);

    s->pin_level = deposit32(s->pin_level, pin, 1, level);

    /* The chip sends on a transition, not while a level is held. */
    if (level && !was && enabled) {
        sgi_bridge_send_interrupt(s, pin, true);
    } else if (!level && was && enabled) {
        /*
         * A falling edge clears the Hub bit only when the mode bit asks;
         * otherwise the guest clears it in its own handler.
         */
        if (s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_MODE)] & (1u << pin)) {
            sgi_bridge_send_interrupt(s, pin, false);
        }
    }
}

/* The PCI core's view of a pin: its aggregated INTx level for that pin. */
static void sgi_bridge_set_irq(void *opaque, int pin, int level)
{
    SGIBridgeState *s = opaque;

    s->pci_pin_level = deposit32(s->pci_pin_level, pin, 1, level);
    sgi_bridge_update_pin(s, pin);
}

/* A directly wired line: the machine driving one of the chip's GPIO inputs. */
static void sgi_bridge_set_wire_irq(void *opaque, int pin, int level)
{
    SGIBridgeState *s = opaque;

    s->gpio_pin_level = deposit32(s->gpio_pin_level, pin, 1, level);
    sgi_bridge_update_pin(s, pin);
}

/* The translation page size, chosen by a control register bit. */
static unsigned int sgi_bridge_page_shift(SGIBridgeState *s)
{
    return s->reg[SGI_BRIDGE_SLOT(BRIDGE_CONTROL)] & CONTROL_PAGE_16K ?
           BRIDGE_PAGE_SHIFT_16K : BRIDGE_PAGE_SHIFT_4K;
}

/*
 * The seam between this chip and the machine's fabric representation,
 * which is not settled: until it is, the offset is the address and the
 * widget is carried but not applied -- right for a machine where the only
 * named target is the node's own memory.
 */
static hwaddr sgi_bridge_compose_xtalk(SGIBridgeState *s,
                                       unsigned int target_widget,
                                       hwaddr offset)
{
    return offset;
}

/*
 * Note the inversion: the chip's swap bits enable a swapper that
 * straightens the crossed wiring, and a byte-addressed model is straight
 * through already -- so it is the disabled case that needs work here.
 */
static bool sgi_bridge_dma_needs_correction(SGIBridgeState *s, unsigned int slot,
                                            hwaddr pci_addr)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];

    if (!sgi_bridge_slot_needs_correction(s, slot)) {
        return false;
    }
    /*
     * Local space is not carried to crosstalk at all, so there is no swapper
     * in its path. That this model passes it through to system memory anyway
     * is a separate fidelity gap, and the swap rule must not paper over it.
     */
    if (pci_addr < BRIDGE_PCI_TRANSLATED) {
        return false;
    }
    /*
     * Specification Tables 48, 49 and 50: both direct windows take the
     * direct-mapped bit and the page-mapped window takes its own.
     */
    return !(dev & (pci_addr < BRIDGE_PCI_DIRECT ? DEV_SWAP_TRANSLATED
                                                 : DEV_SWAP_DIRECT));
}

/*
 * THE PAIR OF ENDIANNESSES HERE IS LOAD-BEARING AND LOOKS REDUNDANT: the
 * body must stay address_space_read/write into a byte buffer, which
 * resolves the answering region's endianness before this code sees the
 * bytes. A value accessor such as address_space_ldl_be() reads as a
 * simplification and reintroduces that endianness as a live term --
 * transfers aimed at another widget's registers arrive reversed. The
 * with-attrs form is what lets a refused transfer be refused here too
 * instead of answered with zeroes. These callbacks never target this
 * space, so there is no way back into themselves.
 */
static MemTxResult sgi_bridge_lane_swap_read(void *opaque, hwaddr addr,
                                             uint64_t *val, unsigned size,
                                             MemTxAttrs attrs)
{
    uint8_t bytes[8];
    MemTxResult res;

    res = address_space_read(&address_space_memory,
                             sgi_bridge_window_correct(addr, size),
                             attrs, bytes, size);
    *val = ldn_be_p(bytes, size);
    return res;
}

static MemTxResult sgi_bridge_lane_swap_write(void *opaque, hwaddr addr,
                                              uint64_t val, unsigned size,
                                              MemTxAttrs attrs)
{
    uint8_t bytes[8];

    stn_be_p(bytes, size, val);
    return address_space_write(&address_space_memory,
                               sgi_bridge_window_correct(addr, size),
                               attrs, bytes, size);
}

static const MemoryRegionOps sgi_bridge_lane_swap_ops = {
    .read_with_attrs = sgi_bridge_lane_swap_read,
    .write_with_attrs = sgi_bridge_lane_swap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static IOMMUTLBEntry sgi_bridge_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                          IOMMUAccessFlags flag, int iommu_idx)
{
    struct SGIBridgeDeviceDMA *dma =
        container_of(iommu, struct SGIBridgeDeviceDMA, iommu);
    SGIBridgeState *s = dma->s;
    unsigned int shift = sgi_bridge_page_shift(s);
    hwaddr page_mask = MAKE_64BIT_MASK(0, shift);
    IOMMUTLBEntry entry = {
        /*
         * The swap decision is taken on the raw PCI address, ahead of the
         * 64-bit decode below: rewritten first, a wide access is measured
         * against the wrong window.
         */
        .target_as = sgi_bridge_dma_needs_correction(s, dma->slot, addr) ?
                     &s->lane_swap_as : &address_space_memory,
        .iova = addr & ~page_mask,
        .translated_addr = 0,
        .addr_mask = page_mask,
        .perm = IOMMU_NONE,
    };
    uint32_t direct;
    uint64_t ate;
    unsigned int index;
    hwaddr offset;

    trace_sgi_bridge_dma_xlate(addr, dma->slot, flag);

    if (addr < BRIDGE_PCI_TRANSLATED) {
        /* Local: not carried to crosstalk at all. */
        entry.translated_addr = addr & ~page_mask;
        entry.perm = IOMMU_RW;
        return entry;
    }

    if (addr >= BRIDGE_PCI_64BIT) {
        /*
         * Ahead of the 31-bit direct path, which would drop the NASID at
         * bit 32 and collapse another node's transfer into node zero's.
         */
        unsigned int widget = (addr >> BRIDGE_DIRECT64_WIDGET_SHIFT) & 0xf;

        if (widget == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bridge: 64-bit DMA names widget 0, which the "
                          "crossbar cannot answer\n");
        }
        entry.translated_addr =
            sgi_bridge_compose_xtalk(s, widget, addr & BRIDGE_XTALK_PASS) &
            ~page_mask;
        entry.perm = IOMMU_RW;
        trace_sgi_bridge_dma_xlate_64(addr, entry.translated_addr, widget);
        return entry;
    }

    if (addr >= BRIDGE_PCI_DIRECT) {
        direct = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DIRECT_MAP)];
        offset = (hwaddr)(direct & DIRECT_OFFSET) << BRIDGE_DIRECT_SHIFT;
        offset |= addr & BRIDGE_DIRECT_PASS;

        /* Applies only with a zero offset field; the bit resets set. */
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
        /* An external static RAM would supply the rest; this board has none. */
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
 * One table for the whole bus; what is per-device is the swap attribute in
 * the requesting slot's own register, so the requester must be known at
 * translate time.
 */
static AddressSpace *sgi_bridge_device_address_space(PCIBus *bus, void *opaque,
                                                     int devfn)
{
    SGIBridgeState *s = opaque;
    unsigned int slot = PCI_SLOT(devfn);

    /*
     * Unreachable from a guest; a device wired outside the chip's slots
     * would silently inherit another slot's swap bits, so it is logged.
     */
    if (slot >= SGI_BRIDGE_DEVICES) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bridge: device at slot %u is outside the eight this "
                      "chip decodes; folding onto slot %u for DMA attributes\n",
                      slot, slot % SGI_BRIDGE_DEVICES);
        slot %= SGI_BRIDGE_DEVICES;
    }
    return &s->dma[slot].as;
}

static const PCIIOMMUOps sgi_bridge_iommu_ops = {
    .get_address_space = sgi_bridge_device_address_space,
};

#define TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION "sgi-bridge-iommu-memory-region"

/*
 * Nothing invalidates a cached mapping when the guest rewrites an entry,
 * so map notifiers (vfio, vhost) are refused rather than fed stale ones.
 */
static int sgi_bridge_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                                IOMMUNotifierFlag old_flags,
                                                IOMMUNotifierFlag new_flags,
                                                Error **errp)
{
    if (new_flags & IOMMU_NOTIFIER_MAP) {
        error_setg(errp,
                   "sgi-bridge does not notify of translation changes");
        return -EINVAL;
    }
    return 0;
}

static void sgi_bridge_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = sgi_bridge_translate;
    imrc->notify_flag_changed = sgi_bridge_iommu_notify_flag_changed;
}

static void sgi_bridge_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeState *s = SGI_BRIDGE(obj);
    unsigned int i;

    for (i = 0; i < SGI_BRIDGE_REG_SLOTS; i++) {
        s->reg[i] = sgi_bridge_regs[i].reset;
    }

    /* Nothing has been asked of the identity part since the machine started. */
    s->onewire_phase = NIC_PHASE_IDLE;
    s->onewire_bit = 0;
    s->onewire_command = 0;
    sgi_bridge_onewire_build_rom(s);

    /* The one reset value the table cannot hold: the machine's identity. */
    s->reg[SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] =
        sgi_bridge_identification(s);

    s->pin_level = 0;
    s->pci_pin_level = 0;
    s->gpio_pin_level = 0;
    s->int_error = 0;

    /*
     * Whole entries, not just valid bits: a stale page address behind a
     * clear valid bit is a working translation waiting for one write.
     */
    memset(s->ate_entry, 0, sizeof(s->ate_entry));

    sgi_bridge_aim_all_windows(s);
}

static void sgi_bridge_realize(DeviceState *dev, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);
    unsigned int slot;

    /* Refuse property errors before the first region exists. */
    if (s->onewire_serial & ~NIC_ROM_SERIAL_MASK) {
        error_setg(errp, "the I/O bridge one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }
    if (!s->fabric) {
        error_setg(errp, "the I/O bridge needs the fabric its interrupt "
                         "packets travel");
        return;
    }
    s->onewire_identity = sgi_bridge_onewire_rom_id(s->onewire_serial);

    memory_region_init(&s->widget, obj, "sgi-bridge.widget",
                       SGI_BRIDGE_WIDGET_SIZE);
    memory_region_init_io(&s->regs, obj, &sgi_bridge_reg_ops, s,
                          "sgi-bridge.regs", SGI_BRIDGE_REGS_SIZE);
    memory_region_add_subregion(&s->widget, 0, &s->regs);
    memory_region_init_io(&s->reserved_data_half, obj,
                          &sgi_bridge_reserved_data_half_ops, s,
                          "sgi-bridge.reserved-data-half", 4);
    memory_region_add_subregion_overlap(&s->widget, BRIDGE_RESERVED_DATA_HALF,
                                        &s->reserved_data_half, 1);
    memory_region_init_io(&s->ssram, obj, &sgi_bridge_ssram_ops, s,
                          "sgi-bridge.ssram", BRIDGE_SSRAM_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_SSRAM_BASE, &s->ssram);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->widget);

    /* Interrupt packets travel the same fabric a processor's access does. */
    address_space_init(&s->fabric_as, s->fabric, "sgi-bridge.fabric");

    /* Full 32-bit I/O space: this chip does not narrow it to 64 KB. */
    memory_region_init(&s->pci_mmio, obj, "sgi-bridge.pci-mmio", 4 * GiB);
    memory_region_init(&s->pci_io, obj, "sgi-bridge.pci-io", 4 * GiB);
    address_space_init(&s->pci_mmio_as, &s->pci_mmio, "sgi-bridge.pci-mmio");
    address_space_init(&s->pci_io_as, &s->pci_io, "sgi-bridge.pci-io");

    /*
     * Two windows per slot, one enabled; plain I/O regions rather than
     * aliases because an alias has nowhere to carry the byte-lane
     * correction, which needs the access width and the slot.
     */
    for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
        g_autofree char *mem_name =
            g_strdup_printf("sgi-bridge.window%u.mem", slot);
        g_autofree char *io_name =
            g_strdup_printf("sgi-bridge.window%u.io", slot);

        s->device_window_mem_ctx[slot].s = s;
        s->device_window_mem_ctx[slot].slot = slot;
        s->device_window_io_ctx[slot].s = s;
        s->device_window_io_ctx[slot].slot = slot;

        memory_region_init_io(&s->device_window_mem[slot], obj,
                              &sgi_bridge_window_mem_ops,
                              &s->device_window_mem_ctx[slot], mem_name,
                              sgi_bridge_window_size(slot));
        memory_region_init_io(&s->device_window_io[slot], obj,
                              &sgi_bridge_window_io_ops,
                              &s->device_window_io_ctx[slot], io_name,
                              sgi_bridge_window_size(slot));
        memory_region_add_subregion(&s->widget,
                                    sgi_bridge_window_offset(slot),
                                    &s->device_window_mem[slot]);
        memory_region_add_subregion(&s->widget,
                                    sgi_bridge_window_offset(slot),
                                    &s->device_window_io[slot]);
    }

    phb->bus = pci_register_root_bus(dev,
                                     s->pci_bus_name ? s->pci_bus_name : "pci",
                                     sgi_bridge_set_irq,
                                     sgi_bridge_map_irq, s,
                                     &s->pci_mmio, &s->pci_io,
                                     PCI_DEVFN(0, 0), SGI_BRIDGE_PINS,
                                     TYPE_PCI_BUS);

    /*
     * Refuse a device past slot 7 when it is added, rather than aborting
     * in the interrupt core the first time it drives an unwired pin.
     */
    pci_bus_set_slot_reserved_mask(phb->bus, ~((1u << SGI_BRIDGE_PINS) - 1));

    /*
     * The same pins, directly wired (the base I/O controller's second
     * interrupt). The wire lines keep their own level word beside the PCI
     * core's: both domains can drive one pin, each holding it on its own.
     */
    qdev_init_gpio_in_named_with_opaque(dev, sgi_bridge_set_wire_irq, s, NULL,
                                        SGI_BRIDGE_PINS);

    memory_region_init_io(&s->cfg0, obj, &sgi_bridge_config_ops, s,
                          "sgi-bridge.cfg0", BRIDGE_CFG0_SIZE);
    memory_region_add_subregion(&s->widget, SGI_BRIDGE_CFG0_BASE, &s->cfg0);

    /* One swap region for the chip: per-device decision, shared action. */
    memory_region_init_io(&s->lane_swap, obj, &sgi_bridge_lane_swap_ops, s,
                          "sgi-bridge.lane-swap", UINT64_MAX);
    /*
     * The re-entrancy guard is per OWNING DEVICE and this chip owns both
     * ends of an ordinary transfer, so normal DMA would trip it; these
     * callbacks never target this region, so there is no recursion to
     * catch. Regions reached through this one still apply their own
     * guards.
     */
    s->lane_swap.disable_reentrancy_guard = true;
    address_space_init(&s->lane_swap_as, &s->lane_swap,
                       "sgi-bridge.lane-swap");

    /* One IOMMU region per slot: the swap bits belong to the issuer. */
    for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
        g_autofree char *name = g_strdup_printf("sgi-bridge.iommu.%u", slot);
        g_autofree char *as_name = g_strdup_printf("sgi-bridge.dma.%u", slot);

        s->dma[slot].s = s;
        s->dma[slot].slot = slot;
        memory_region_init_iommu(&s->dma[slot].iommu,
                                 sizeof(s->dma[slot].iommu),
                                 TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION, obj,
                                 name, UINT64_MAX);
        address_space_init(&s->dma[slot].as,
                           MEMORY_REGION(&s->dma[slot].iommu), as_name);
    }
    pci_setup_iommu(phb->bus, &sgi_bridge_iommu_ops, s);

    /* The translation table, and the read-only view of its lower halves. */
    memory_region_init_io(&s->ate, obj, &sgi_bridge_ate_ops, s,
                          "sgi-bridge.ate", BRIDGE_ATE_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_BASE, &s->ate);
    memory_region_init_io(&s->ate_low, obj, &sgi_bridge_ate_low_ops, s,
                          "sgi-bridge.ate-low", BRIDGE_ATE_LOW_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_LOW_BASE, &s->ate_low);
}

/*
 * The windows are derived from the per-slot registers the stream carries,
 * so post_load re-aims them the way reset does -- without it a resumed
 * guest reaches wherever the destination's windows happened to point.
 */
static int sgi_bridge_post_load(void *opaque, int version_id)
{
    SGIBridgeState *s = opaque;

    sgi_bridge_aim_all_windows(s);
    /*
     * An older stream carries only the effective levels; attribute them
     * to the PCI side -- a wire level is re-driven by its source device.
     */
    if (version_id < 5) {
        s->pci_pin_level = s->pin_level;
        s->gpio_pin_level = 0;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_bridge = {
    .name = "sgi-bridge",
    .version_id = 5,
    .minimum_version_id = 1,
    .post_load = sgi_bridge_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIBridgeState, SGI_BRIDGE_REG_SLOTS),
        VMSTATE_UINT8(pin_level, SGIBridgeState),
        VMSTATE_UINT8_V(pci_pin_level, SGIBridgeState, 5),
        VMSTATE_UINT8_V(gpio_pin_level, SGIBridgeState, 5),
        VMSTATE_UINT32_V(int_error, SGIBridgeState, 4),
        VMSTATE_UINT64_ARRAY_V(ate_entry, SGIBridgeState,
                               SGI_BRIDGE_ATE_ENTRIES, 2),
        /*
         * The one-wire conversation lives between register accesses. The
         * stored image is absent by design: reset rebuilds it identically
         * on the destination.
         */
        VMSTATE_UINT8_V(onewire_phase, SGIBridgeState, 3),
        VMSTATE_UINT8_V(onewire_bit, SGIBridgeState, 3),
        VMSTATE_UINT8_V(onewire_command, SGIBridgeState, 3),
        VMSTATE_UINT8_ARRAY_V(onewire_args, SGIBridgeState, 2, 3),
        VMSTATE_UINT8_V(onewire_argn, SGIBridgeState, 3),
        VMSTATE_UINT64_V(onewire_match, SGIBridgeState, 3),
        VMSTATE_UINT8_ARRAY_V(onewire_out, SGIBridgeState, 37, 3),
        VMSTATE_UINT16_V(onewire_outbit, SGIBridgeState, 3),
        VMSTATE_UINT8_V(onewire_outlen, SGIBridgeState, 3),
        VMSTATE_UINT16_V(onewire_status, SGIBridgeState, 3),
        VMSTATE_UINT16_V(onewire_read, SGIBridgeState, 3),
        VMSTATE_UINT8_ARRAY_V(onewire_end, SGIBridgeState, 2, 3),
        VMSTATE_END_OF_LIST()
    }
};

/* The chip revision and board serial belong to the machine that fits them. */
static const Property sgi_bridge_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIBridgeState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIBridgeState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIBridgeState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("revision", SGIBridgeState, revision, 1),
    DEFINE_PROP_UINT32("part", SGIBridgeState, part, 0xc002),
    DEFINE_PROP_UINT32("manufacturer", SGIBridgeState, manufacturer, 0x036),
    DEFINE_PROP_UINT64("onewire-serial", SGIBridgeState, onewire_serial,
                       NIC_ROM_SERIAL_DEFAULT),
    DEFINE_PROP_STRING("pci-bus-name", SGIBridgeState, pci_bus_name),
    DEFINE_PROP_UINT32("byte-lane-exempt", SGIBridgeState, byte_lane_exempt, 0),
};

static void sgi_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI Bridge crosstalk-to-PCI host bridge";
    dc->fw_name = "pci";
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
