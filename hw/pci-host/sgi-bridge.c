/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI Bridge crosstalk-to-PCI host bridge. */
/*
 * SGI Bridge XIO-to-PCI host bridge.
 *
 * Registers are 32 bits in the upper word of an eight-byte slot -- the data
 * sits at the slot's offset plus four -- and the reset state is heavily
 * non-zero, so decode, reset value and writable mask all come from one
 * table. See docs/specs/sgi-bridge.rst for the register, translation,
 * byte-lane and board-identity contracts.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2505.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/core/hotplug.h"
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

/* Common XIO widget registers. */
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

/* The one-wire master: writing the timing fields runs one bit slot. */
#define BRIDGE_NIC              0x0b0
#define NIC_LEVEL               (1u << 0)
#define NIC_DONE                (1u << 1)
#define NIC_SAMPLE_SHIFT        2
#define NIC_SAMPLE_MASK         (0xffu << NIC_SAMPLE_SHIFT)
#define NIC_PULSE_SHIFT         10
#define NIC_PULSE_MASK          (0x3ffu << NIC_PULSE_SHIFT)
#define NIC_WRITABLE            (NIC_SAMPLE_MASK | NIC_PULSE_MASK)

/* The stored record: two pages, each carrying its own check. */
#define NIC_PAGE_SIZE           32
#define NIC_PAGES               2
#define NIC_ROM_SIZE            (NIC_PAGE_SIZE * NIC_PAGES)

/* Kept equal so a shared caller's buffer size matches this file's own. */
QEMU_BUILD_BUG_ON(NIC_ROM_SIZE != SGI_BRIDGE_ONEWIRE_RECORD_SIZE);

/* Guests match on the family code; the serial is the machine's. */
#define NIC_ROM_FAMILY          0x0b
#define NIC_ROM_SERIAL_DEFAULT  0x060504030201ULL
#define NIC_ROM_SERIAL_MASK     MAKE_64BIT_MASK(0, 48)

#define BRIDGE_SSRAM_PARITY_ERR 0x090
#define BRIDGE_ARBITRATION      0x0a0
#define BRIDGE_BUS_TIMEOUT      0x0c0
#define BRIDGE_PCI_TYPE1_CONFIG 0x0c8
#define BRIDGE_PCI_ERR_UPPER    0x0d0
#define BRIDGE_PCI_ERR_LOWER    0x0d8

/* The PCI error upper address: device number above PCI address 47:32. */
#define PCI_ERR_DEVICE_SHIFT    16
/* Response buffer error upper: the device at 22:20, the buffer at 19:16. */
#define RESP_ERR_DEVICE_SHIFT   20

/* The multiple-error flag at bit 31 has no enable bit. */
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
/*
 * Bit 24, "address invalid for this widget": set when a guest touches an
 * undefined address inside the register window.
 */
#define INT_INVALID_ADDR        (1u << 24)
/* Bit 11: a cycle no PCI device claimed (a master time-out). */
#define INT_PCI_MASTER_TIMEOUT  (1u << 11)
/* Bit 15: the target claimed the cycle and then aborted it. */
#define INT_PCI_TARGET_ABORT    (1u << 15)
/* The PCI group: abort, the three parity errors, master time-out, retries. */
#define INT_PCI_GROUP           (0x3fu << 10)
/* Bit 9: a read this chip sent for a device got no response in time. */
#define INT_XREAD_REQ_TIMEOUT   (1u << 9)
/* The response-buffer group: framing and error responses, the time-out. */
#define INT_RESP_BUF_GROUP      ((1u << 28) | (1u << 26) | (1u << 9))
/* A second error in a group before the first was cleared. */
#define INT_MULTI_ERR           (1u << 31)
/* All a guest may enable: bit 31, the multiple-error flag, has no bit. */
#define INT_ENABLE_WRITABLE     0x7fffffffu
/* Seven group bits, each clearing one group of error conditions at once. */
#define INT_RESET_GROUPS        0x7fu
/* Bit 0 clears the PCI group and rearms its error address capture. */
#define INT_RESET_PCI_GRP       (1u << 0)
/*
 * Bit 3 is the request-dispatch group's strobe. The invalid-address error is
 * in that group, so it alone clears it; a strobe for any other group does
 * not.
 */
#define INT_RESET_REQ_DSP_GRP   (1u << 3)
#define INT_RESET_RESP_BUF_GRP  (1u << 4)
/* Bit 6 clears the multiple-error flag. */
#define INT_RESET_MULTI_ERR     (1u << 6)
/* One bit per pin: send a clearing packet when that pin falls. */
#define INT_MODE_WRITABLE       0xffu
/* Three bits per pin naming the device that owns it, eight pins. */
#define INT_DEVICE_WRITABLE     0xffffffu
/*
 * The widget-level destination: a target widget and the address's top half.
 */
#define INT_DEST_ADDRESS_UPPER  0xffffu
#define INT_DEST_WIDGET_SHIFT   16
#define INT_DEST_WIDGET         (0xfu << INT_DEST_WIDGET_SHIFT)
/* A host address field and the bit number to set in the host's register. */
#define INT_HOST_ADDRESS        (0x3ffu << 8)
#define INT_HOST_FIELD          0xffu
#define INT_HOST_WRITABLE       (INT_HOST_ADDRESS | INT_HOST_FIELD)
/* The bit the chip sets when it reports an error of its own. */
#define HOST_ERR_FIELD_WRITABLE 0xffu

/* The per-slot registers, eight of each. */
#define BRIDGE_DEVICE(n)        (0x200 + SGI_BRIDGE_REG_SLOT * (n))
#define BRIDGE_DEVICE_FLUSH(n)  (0x240 + SGI_BRIDGE_REG_SLOT * (n))

/* Entries write whole as 64 bits; the second region reads the low halves. */
#define BRIDGE_ATE_BASE         0x010000
#define BRIDGE_ATE_ENTRY_SIZE   8
#define BRIDGE_ATE_SIZE         (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_ENTRY_SIZE)
#define BRIDGE_ATE_LOW_BASE     0x011000
#define BRIDGE_ATE_LOW_STRIDE   4

/*
 * The external-SSRAM window: 512 KB at 0x08_0000, unpopulated on this board.
 */
#define BRIDGE_SSRAM_BASE       0x080000
#define BRIDGE_SSRAM_SIZE       (512 * KiB)
#define BRIDGE_ATE_LOW_SIZE     (SGI_BRIDGE_ATE_ENTRIES * BRIDGE_ATE_LOW_STRIDE)

/*
 * The external-flash window: two 2 MB chip selects on the 16-bit SSRAM port,
 * the one the FLASH_SELECT strap names answering at the base.
 */
#define BRIDGE_FLASH_BASE        0xc00000
#define BRIDGE_FLASH_SELECT_SIZE (2 * MiB)
#define BRIDGE_FLASH_SIZE        (2 * BRIDGE_FLASH_SELECT_SIZE)

/* Full sixteen bits: the direct-map mode bit decides how many fill. */
#define ATE_REMOTE_MAP          MAKE_64BIT_MASK(48, 16)
#define ATE_ADDRESS             MAKE_64BIT_MASK(12, 36)
#define ATE_TARGET_WIDGET       MAKE_64BIT_MASK(8, 4)
#define ATE_BARRIER             BIT_ULL(4)
#define ATE_PREFETCH            BIT_ULL(3)
#define ATE_PRECISE             BIT_ULL(2)
#define ATE_COHERENT            BIT_ULL(1)
#define ATE_VALID               BIT_ULL(0)

#define BRIDGE_PCI_TRANSLATED   0x40000000u
#define BRIDGE_PCI_DIRECT       0x80000000u

/* The two translation page sizes, chosen by a bit in the control register. */
#define BRIDGE_PAGE_SHIFT_4K    12
#define BRIDGE_PAGE_SHIFT_16K   14

#define BRIDGE_DIRECT_SHIFT     31
#define BRIDGE_DIRECT_PASS      MAKE_64BIT_MASK(0, 31)
#define BRIDGE_DIRECT_512MB     (512 * MiB)

#define BRIDGE_PCI_64BIT        (1ULL << 32)
#define BRIDGE_XIO_PASS         MAKE_64BIT_MASK(0, 48)
#define BRIDGE_DIRECT64_WIDGET_SHIFT 60

/* The geometry is public in the header: the machine names windows too. */
#define BRIDGE_CFG0_SIZE        (SGI_BRIDGE_DEVICES * \
                                 SGI_BRIDGE_CFG0_DEVICE_SIZE)
#define BRIDGE_CFG0_DEVICE(a)   (((a) >> 12) & 7)
#define BRIDGE_CFG0_FUNCTION(a) (((a) >> 8) & 7)
#define BRIDGE_CFG0_REGISTER    0xff

/* The first two windows are 2 MB, the rest 1 MB: not evenly spaced. */
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

/* Seven fields reset set, summed from names so a missing term shows. */
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
/*
 * Two strobes that clear the link retry counters and always read back zero.
 */
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

/* Resets set: cleared, every direct transfer moves half a gigabyte down. */
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

/* Three bits reset set, the memory-or-I/O select included. */
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

/* Firmware-sourced reset: its sanity diagnostic asserts all eight. */
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

/* A read-only write is a guest bug; a strobe write is correct use. */
typedef enum {
    SGI_BRIDGE_RO,      /* writes are a guest error and are reported */
    SGI_BRIDGE_RW,      /* writable mask selects guest-settable bits */
    SGI_BRIDGE_WO,      /* strobe: write taken, read gives reset value */
    SGI_BRIDGE_LIVE,    /* read-only and computed: no storage to go stale */
} SGIBridgeRegAccess;

/*
 * One row per defined register: a decode cannot exist without a reset value
 * and writable mask. Non-zero resets come from the specification; a zero is
 * either the specification's or, where marked, this model's.
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

    /* The level and finished flag are answers, outside the mask. */
    [SGI_BRIDGE_SLOT(BRIDGE_NIC)] = {
        "one-wire bus master", SGI_BRIDGE_RW, 0, NIC_WRITABLE },

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

    /* Reading it is the operation; it must never fault. */
    [SGI_BRIDGE_SLOT(BRIDGE_TARGET_FLUSH)] = {
        "target flush", SGI_BRIDGE_RO, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_AUX_ERR_CMD_WORD)] = {
        "auxiliary error command word", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_UPPER)] = {
        "response buffer error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_LOWER)] = {
        "response buffer error address lower", SGI_BRIDGE_RO, 0, 0 },

    /* No specified power-up value; zero is this model's choice. */
    [SGI_BRIDGE_SLOT(BRIDGE_TEST_PIN_CONTROL)] = {
        "test pin control", SGI_BRIDGE_RW, 0, 0xffffffffu },

    [SGI_BRIDGE_SLOT(BRIDGE_DIRECT_MAP)] = {
        "direct map", SGI_BRIDGE_RW, DIRECT_MAP_RESET, DIRECT_MAP_WRITABLE },

    [SGI_BRIDGE_SLOT(BRIDGE_SSRAM_PARITY_ERR)] = {
        "static RAM parity error", SGI_BRIDGE_RO, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_ARBITRATION)] = {
        "arbitration", SGI_BRIDGE_RW, ARBITRATION_RESET, ARBITRATION_WRITABLE },
    [SGI_BRIDGE_SLOT(BRIDGE_BUS_TIMEOUT)] = {
        "bus time-out", SGI_BRIDGE_RW, BUS_TIMEOUT_RESET,
        BUS_TIMEOUT_WRITABLE },

    [SGI_BRIDGE_SLOT(BRIDGE_PCI_TYPE1_CONFIG)] = {
        "PCI type 1 configuration", SGI_BRIDGE_RW, 0, TYPE1_WRITABLE },

    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_UPPER)] = {
        "PCI error address upper", SGI_BRIDGE_RO, 0, 0 },
    [SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_LOWER)] = {
        "PCI error address lower", SGI_BRIDGE_RO, 0, 0 },

    /*
     * The pin bits must follow the level and never latch, a latched bit
     * reports a device with a second condition pending as quiet, forever.
     * Composed on every read (LIVE); only the error bits latch.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_STATUS)] = {
        "interrupt status", SGI_BRIDGE_LIVE, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] = {
        "interrupt enable", SGI_BRIDGE_RW, 0, INT_ENABLE_WRITABLE },

    /* A strobe: a one in a group bit clears that group's errors. */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_RESET)] = {
        "reset interrupt status", SGI_BRIDGE_WO, 0, INT_RESET_GROUPS },

    /* RMW by the guest: a zero read would wipe every configured pin. */
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

    /* Storage: every read completes before returning, so unobservable. */
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
 * For a 2 MB window bit 20 arrives from both base and offset; adding them is
 * OpenBSD's own arithmetic. A region's target space cannot change, so each
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

/* Shared by reset and post_load: windows derive from the registers. */
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

    /* Masked like a guest write, or two unrelated bits clear silently. */
    *held = (*held & ~assigned) | (base | (memory_space ? DEV_MEMORY_SPACE
                                                        : 0));
    sgi_bridge_aim_window(s, slot);
}

/*
 * Two callers: the programmed-access windows (the byte-lane tables land a
 * narrow access reversed with swap disabled, the state both guests run in)
 * and the DMA path. A slot is exempt when the device plugged into it declared
 * that it orders these byte lanes itself, which is a fact about the model on
 * the other end of the wires and travels with it from slot to slot; the
 * property beside it forces the answer for tests and debugging.
 */
static bool sgi_bridge_slot_needs_correction(SGIBridgeState *s,
                                             unsigned int slot)
{
    return !((s->self_ordered_slots | s->byte_lane_exempt) & (1u << slot));
}

/*
 * Only the low two bits move: lanes are defined within one word. The mask is
 * address-wide because the DMA path passes whole fabric addresses.
 */
static hwaddr sgi_bridge_window_correct(hwaddr offset, unsigned int size)
{
    return (offset & ~(hwaddr)3) | ((offset & 3u) ^ (4u - size));
}


static void sgi_bridge_pci_error(SGIBridgeState *s, uint32_t bit,
                                 unsigned int device, uint64_t pci_address);

/*
 * The device on this Bridge's bus whose enabled base address register of
 * the given kind covers a PCI address, or NULL when none does. A PCI
 * target drives device select for every address its enabled base address
 * registers decode, so only an address no register covers ends in the
 * master time-out the Bridge records when nothing drives it (table 57,
 * below). A device model
 * that leaves part of its register window without a region is missing
 * behaviour, not declining the cycle. A device behind a PCI-to-PCI bridge
 * is not searched: no board this Bridge carries has one.
 */
static PCIDevice *sgi_bridge_pci_claimant(SGIBridgeState *s, bool io,
                                          hwaddr pci_address)
{
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;
    unsigned int devfn;
    int i;

    for (devfn = 0; devfn < ARRAY_SIZE(bus->devices); devfn++) {
        PCIDevice *dev = bus->devices[devfn];

        if (!dev) {
            continue;
        }
        for (i = 0; i < PCI_NUM_REGIONS; i++) {
            PCIIORegion *r = &dev->io_regions[i];

            if (!r->size || r->addr == PCI_BAR_UNMAPPED ||
                !(r->type & PCI_BASE_ADDRESS_SPACE_IO) != !io) {
                continue;
            }
            if (pci_address >= r->addr && pci_address - r->addr < r->size) {
                return dev;
            }
        }
    }
    return NULL;
}

/*
 * A cycle that fails on PCI is one of the Bridge-as-master errors
 * (Bridge ASIC specification rev 4.0, table 57): no device drove device
 * select, the master time-out, or the target claimed it and aborted. Each
 * records the PCI address and device in the PCI error group. A read that
 * failed has no data to return, so it fails back to the requester; a
 * write posts and its failure stays in the Bridge's own record.
 */
static MemTxResult sgi_bridge_window_failed(SGIBridgeState *s,
                                            unsigned int device,
                                            AddressSpace *space,
                                            hwaddr pci_address,
                                            MemTxResult result)
{
    PCIDevice *claimant;

    if (result == MEMTX_OK) {
        return MEMTX_OK;
    }
    if (result == MEMTX_DECODE_ERROR) {
        claimant = sgi_bridge_pci_claimant(s, space == &s->pci_io_as,
                                           pci_address);
        if (claimant) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-bridge: PCI address 0x%" HWADDR_PRIx
                          " is in a base address register of %s, whose "
                          "model decodes nothing there; the cycle "
                          "completes with no data\n", pci_address,
                          object_get_typename(OBJECT(claimant)));
            return MEMTX_OK;
        }
    }
    sgi_bridge_pci_error(s, result == MEMTX_DECODE_ERROR ?
                         INT_PCI_MASTER_TIMEOUT : INT_PCI_TARGET_ABORT,
                         device, pci_address);
    return MEMTX_ERROR;
}

/*
 * address_space_read/write move raw bytes, so the only byte order in play is
 * this window's LE declaration against the BE packing below: one swap at the
 * window boundary, independent of the host's endianness.
 */
/*
 * One window cycle of up to four bytes, corrected if the caller needs it: a
 * byte, halfword or word, and each half of a 32-bit device's split
 * doubleword. impl.max caps size at four here; the assert makes that bound
 * visible to the compiler's array-bounds check, the eight-byte doubleword
 * being the caller's. Parameterized on a plain base and a correction flag
 * rather than a slot: the per-device windows pass their slot's own base and
 * its self-ordered exemption, the inbound PIO translated window (no slot, no
 * exemption a card can declare into) always corrects from PCI address zero.
 * device is the number a failure records.
 */
static MemTxResult sgi_bridge_window_read_word(SGIBridgeState *s,
                                               unsigned int device,
                                               hwaddr base, bool correct,
                                               AddressSpace *space,
                                               hwaddr addr, unsigned int size,
                                               uint64_t *value)
{
    hwaddr offset = correct ? sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = base + offset;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    assert(size <= sizeof(bytes));
    result = address_space_read(space, target, MEMTXATTRS_UNSPECIFIED, bytes,
                                size);
    *value = ldn_be_p(bytes, size);
    return sgi_bridge_window_failed(s, device, space, target, result);
}

/*
 * Two word cycles for a doubleword, big-endian high word at the lower
 * offset. A first cycle that fails ends the read there: the specification
 * does not say whether the Bridge still issues the second, and the
 * requester gets the failure either way.
 */
static MemTxResult sgi_bridge_window_read_split(SGIBridgeState *s,
                                                unsigned int device,
                                                hwaddr base, bool correct,
                                                AddressSpace *space,
                                                hwaddr addr, uint64_t *value)
{
    uint64_t word0, word1;
    MemTxResult result;

    result = sgi_bridge_window_read_word(s, device, base, correct, space,
                                         addr, 4, &word0);
    if (result == MEMTX_OK) {
        result = sgi_bridge_window_read_word(s, device, base, correct, space,
                                             addr + 4, 4, &word1);
    }
    *value = result == MEMTX_OK ? (word0 << 32) | word1 : 0;
    return result;
}

/*
 * A device-window read. The slot's device-size bit picks how a doubleword is
 * served: a 32-bit device gets two word cycles (each corrected), a 64-bit
 * device one eight-byte cycle with no narrow correction, mirrored end to end
 * on the programmed-access swap bit. The window is DEVICE_LITTLE_ENDIAN, so
 * reading the buffer big-endian nets to the doubleword unchanged and
 * little-endian to the mirror. A byte, halfword or word is one corrected
 * cycle.
 */
static MemTxResult sgi_bridge_window_read(SGIBridgeState *s,
                                          unsigned int slot,
                                          AddressSpace *space, hwaddr addr,
                                          unsigned int size, uint64_t *value)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];
    bool correct = sgi_bridge_slot_needs_correction(s, slot);
    hwaddr base = s->device_window_base[slot];

    if (size == 8 && (dev & DEV_64BIT_DEVICE)) {
        uint8_t bytes[8] = { 0 };
        MemTxResult result;

        result = address_space_read(space, base + addr,
                                    MEMTXATTRS_UNSPECIFIED, bytes, 8);
        *value = (dev & DEV_SWAP_PROGRAMMED) ? ldq_le_p(bytes)
                                             : ldq_be_p(bytes);
        return sgi_bridge_window_failed(s, slot, space, base + addr, result);
    }

    if (size == 8) {
        return sgi_bridge_window_read_split(s, slot, base, correct, space,
                                            addr, value);
    }

    return sgi_bridge_window_read_word(s, slot, base, correct, space, addr,
                                       size, value);
}

/*
 * One window write cycle of up to four bytes, the counterpart of
 * sgi_bridge_window_read_word.
 */
static void sgi_bridge_window_write_word(SGIBridgeState *s,
                                         unsigned int device, hwaddr base,
                                         bool correct, AddressSpace *space,
                                         hwaddr addr, uint64_t val,
                                         unsigned int size)
{
    hwaddr offset = correct ? sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = base + offset;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_be_p(bytes, size, val);
    sgi_bridge_window_failed(s, device, space, target,
                             address_space_write(space, target,
                                                 MEMTXATTRS_UNSPECIFIED,
                                                 bytes, size));
}

/*
 * The write counterpart of sgi_bridge_window_read: the device-size bit picks
 * the same split or single cycle and the swap bit mirrors a 64-bit
 * doubleword, so a value round-trips through a 64-bit window under either
 * swap setting.
 */
static void sgi_bridge_window_write(SGIBridgeState *s, unsigned int slot,
                                    AddressSpace *space, hwaddr addr,
                                    uint64_t val, unsigned int size)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];
    bool correct = sgi_bridge_slot_needs_correction(s, slot);
    hwaddr base = s->device_window_base[slot];

    if (size == 8 && (dev & DEV_64BIT_DEVICE)) {
        uint8_t bytes[8];

        if (dev & DEV_SWAP_PROGRAMMED) {
            stq_le_p(bytes, val);
        } else {
            stq_be_p(bytes, val);
        }
        sgi_bridge_window_failed(s, slot, space, base + addr,
                                 address_space_write(space, base + addr,
                                                     MEMTXATTRS_UNSPECIFIED,
                                                     bytes, 8));
        return;
    }

    if (size == 8) {
        sgi_bridge_window_write_word(s, slot, base, correct, space, addr,
                                     val >> 32, 4);
        sgi_bridge_window_write_word(s, slot, base, correct, space, addr + 4,
                                     val, 4);
        return;
    }

    sgi_bridge_window_write_word(s, slot, base, correct, space, addr, val,
                                 size);
}

static MemTxResult sgi_bridge_window_mem_read(void *opaque, hwaddr addr,
                                              uint64_t *data, unsigned size,
                                              MemTxAttrs attrs)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    return sgi_bridge_window_read(win->s, win->slot, &win->s->pci_mmio_as,
                                  addr, size, data);
}

static MemTxResult sgi_bridge_window_mem_write(void *opaque, hwaddr addr,
                                               uint64_t val, unsigned size,
                                               MemTxAttrs attrs)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    sgi_bridge_window_write(win->s, win->slot, &win->s->pci_mmio_as, addr,
                            val, size);
    return MEMTX_OK;
}

static MemTxResult sgi_bridge_window_io_read(void *opaque, hwaddr addr,
                                             uint64_t *data, unsigned size,
                                             MemTxAttrs attrs)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    return sgi_bridge_window_read(win->s, win->slot, &win->s->pci_io_as, addr,
                                  size, data);
}

static MemTxResult sgi_bridge_window_io_write(void *opaque, hwaddr addr,
                                              uint64_t val, unsigned size,
                                              MemTxAttrs attrs)
{
    struct SGIBridgeDeviceWindow *win = opaque;

    sgi_bridge_window_write(win->s, win->slot, &win->s->pci_io_as, addr, val,
                            size);
    return MEMTX_OK;
}

/*
 * A doubleword is a native access here (the firmware's device-pointer
 * dereference is one). Both maxima are eight so it reaches the handlers
 * whole: a 64-bit device is served one cycle, a 32-bit device the two
 * four-byte cycles the handler now splits itself, since impl past four stops
 * the core doing it. The byte-lane correction still sees each four-byte cycle
 * in its own right.
 */
static const MemoryRegionOps sgi_bridge_window_mem_ops = {
    .read_with_attrs = sgi_bridge_window_mem_read,
    .write_with_attrs = sgi_bridge_window_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps sgi_bridge_window_io_ops = {
    .read_with_attrs = sgi_bridge_window_io_read,
    .write_with_attrs = sgi_bridge_window_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The inbound PIO translated window's cycle: the same byte-lane correction
 * as a per-device window's, but based at PCI address zero rather than a
 * slot's programmed base, and always corrected. There is no per-device
 * register behind this window to carry a self-ordered exemption or a
 * 64-bit-device declaration, so a doubleword access always gets the two
 * corrected four-byte cycles a 32-bit device's window would use, the common
 * case, and the only one this window can express. No slot selects
 * the cycle either, so a failure records device number zero: which number
 * the chip records for a cycle this window issues is not stated.
 *
 * This correction is a no-op for the only traffic confirmed to use this
 * window today: PsiTech RAD4's BAR2 is 4-byte-access-only end to end
 * (rad4_bar2_ops), and sgi_bridge_window_correct's offset adjustment is the
 * identity function at size 4. A live boot with the correction forced off
 * produced byte-identical screendumps to a corrected boot. See the header
 * comment on SGI_BRIDGE_PIO_XLATE_BASE for what was tested and what remains
 * unexplained. RAD4's wrong login-screen colors are confirmed independent
 * of this flag.
 */
static MemTxResult sgi_bridge_pio_xlate_read(void *opaque, hwaddr addr,
                                             uint64_t *data, unsigned size,
                                             MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;

    if (size == 8) {
        return sgi_bridge_window_read_split(s, 0, 0, true, &s->pci_mmio_as,
                                            addr, data);
    }

    return sgi_bridge_window_read_word(s, 0, 0, true, &s->pci_mmio_as, addr,
                                       size, data);
}

static MemTxResult sgi_bridge_pio_xlate_write(void *opaque, hwaddr addr,
                                              uint64_t val, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;

    if (size == 8) {
        sgi_bridge_window_write_word(s, 0, 0, true, &s->pci_mmio_as, addr,
                                     val >> 32, 4);
        sgi_bridge_window_write_word(s, 0, 0, true, &s->pci_mmio_as, addr + 4,
                                     val, 4);
        return MEMTX_OK;
    }

    sgi_bridge_window_write_word(s, 0, 0, true, &s->pci_mmio_as, addr, val,
                                 size);
    return MEMTX_OK;
}

static const MemoryRegionOps sgi_bridge_pio_xlate_ops = {
    .read_with_attrs = sgi_bridge_pio_xlate_read,
    .write_with_attrs = sgi_bridge_pio_xlate_write,
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
 * Posted and flagged, never faulted: a firmware self-test provokes one
 * deliberately and fails on any synchronous exception.
 */
static void sgi_bridge_raise_host_error(SGIBridgeState *s);
static void sgi_bridge_send_interrupt(SGIBridgeState *s, int pin, bool set);

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

    /*
     * An undefined address in the block posts and flags rather than faulting.
     */
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
 * The older text layout: `;Part:` and the part number from byte 11. Linux
 * finds its part number there, but IRIX reads the page at fixed offsets and
 * prints the barcode as "?Part?" and the revision and name as NUL, the page
 * is a fixed-offset binary record, not the text IRIX expects. Boards whose
 * barcode, revision and name are not yet known still use it: a bare PCI
 * expansion board given no board name (board-pci-expansion.c). The IO-8's
 * XBridge keeps its own copy of this layout in sgi-xbridge.c. A board with a
 * board name set uses the record sgi_bridge_onewire_build_record() stores
 * instead.
 */
static const char sgi_bridge_onewire_image[NIC_ROM_SIZE] = {
    [0]  = 0x01, ' ', ' ', ' ', ' ', ';',
    [6]  = 'P', 'a', 'r', 't', ':',
    [11] = ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    [23] = ';', ' ', ' ', ' ', ' ', ' ', ' ',
    [32] = ' ', ' ', ' ', ' ', ' ', ' ',
};

/*
 * The fixed-offset board-identity record. The part number is split around
 * page 0's check value: spaces then the nine-character base number at 11-29,
 * spaces then the three-digit dash number at 32-37.
 */
#define NIC_RECORD_FORMAT        0x01
#define NIC_RECORD_BARCODE       1
#define NIC_RECORD_BARCODE_LEN   10
#define NIC_RECORD_PART_BASE     21
#define NIC_RECORD_PART_BASE_LEN 9
#define NIC_RECORD_PART_DASH     35
#define NIC_RECORD_PART_DASH_LEN 3
#define NIC_RECORD_REVISION      38
#define NIC_RECORD_REVISION_LEN  4
#define NIC_RECORD_FLAGS         42  /* group, capability, variety */
#define NIC_RECORD_FLAGS_LEN     6
#define NIC_RECORD_NAME          48
#define NIC_RECORD_NAME_LEN      14

/*
 * The formatter IRIX shows these fields through substitutes '?' for ':' and
 * ';', and a NUL shows as '.' or '_', so a field is printable ASCII other
 * than those two.
 */
static const char *sgi_bridge_record_field_error(const char *field,
                                                 size_t width)
{
    const char *c;

    if (!field || !*field || strlen(field) > width) {
        return "is required and must fit its field";
    }
    for (c = field; *c; c++) {
        if (*c < 0x20 || *c > 0x7e || *c == ':' || *c == ';') {
            return "must be printable, without ':' or ';'";
        }
    }
    return NULL;
}

static bool sgi_bridge_board_record_check(const SGIBridgeBoardRecord *record,
                                          Error **errp)
{
    const struct {
        const char *what;
        const char *value;
        size_t width;
    } fields[] = {
        { "barcode", record->barcode, NIC_RECORD_BARCODE_LEN },
        { "revision", record->revision, NIC_RECORD_REVISION_LEN },
        { "name", record->name, NIC_RECORD_NAME_LEN },
    };
    const char *error = sgi_bridge_board_part_error(record->part);
    unsigned int i;

    if (error) {
        error_setg(errp, "the I/O bridge %s", error);
        return false;
    }
    /*
     * OpenBSD's reader rejects a record whose part, bytes 21-29 and 35-37,
     * holds anything but digits and dashes; that also keeps out the ':' the
     * formatter IRIX prints it through would change.
     */
    if (strspn(record->part, "0123456789-") != strlen(record->part)) {
        error_setg(errp, "the I/O bridge board part in a board record must "
                   "be digits and dashes");
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(fields); i++) {
        error = sgi_bridge_record_field_error(fields[i].value,
                                              fields[i].width);
        if (error) {
            error_setg(errp, "the I/O bridge board %s %s (at most %zu "
                       "characters)", fields[i].what, error, fields[i].width);
            return false;
        }
    }
    return true;
}

/*
 * The check in each page's last two bytes is image content, not device
 * behaviour: a part's own check always covers a whole page, so a check stored
 * inside one is a software record convention. It is stored inverted, so a
 * guest shifting a page and its check through the generator lands on the
 * fixed residue every one-wire CRC-16 stream ends on.
 */
static void sgi_bridge_onewire_seal_pages(uint8_t rom[NIC_ROM_SIZE])
{
    unsigned int page;

    for (page = 0; page < NIC_PAGES; page++) {
        uint8_t *at = &rom[page * NIC_PAGE_SIZE];
        uint16_t crc = ~onewire_crc16(0, at, NIC_PAGE_SIZE - 2);

        at[NIC_PAGE_SIZE - 2] = crc & 0xff;
        at[NIC_PAGE_SIZE - 1] = crc >> 8;
    }
}

static void sgi_bridge_onewire_build_rom(uint8_t rom[NIC_ROM_SIZE],
                                         const char *board_part)
{
    memcpy(rom, sgi_bridge_onewire_image, NIC_ROM_SIZE);
    memcpy(&rom[11], board_part, 12);
    sgi_bridge_onewire_seal_pages(rom);
}

void sgi_bridge_onewire_build_record(uint8_t rom[NIC_ROM_SIZE],
                                     const SGIBridgeBoardRecord *record)
{
    memset(rom, ' ', NIC_ROM_SIZE);
    rom[0] = NIC_RECORD_FORMAT;
    memcpy(&rom[NIC_RECORD_BARCODE], record->barcode,
           strlen(record->barcode));
    memcpy(&rom[NIC_RECORD_PART_BASE], record->part,
           NIC_RECORD_PART_BASE_LEN);
    memcpy(&rom[NIC_RECORD_PART_DASH],
           record->part + NIC_RECORD_PART_BASE_LEN, NIC_RECORD_PART_DASH_LEN);
    memcpy(&rom[NIC_RECORD_REVISION], record->revision,
           strlen(record->revision));
    memset(&rom[NIC_RECORD_FLAGS], 0xff, NIC_RECORD_FLAGS_LEN);
    memcpy(&rom[NIC_RECORD_NAME], record->name, strlen(record->name));
    sgi_bridge_onewire_seal_pages(rom);
}

/*
 * Realize one DS2505 board-identity part onto the bus and give it
 * `child_name`, so the realize-time board record and a later
 * sgi_bridge_add_onewire_board_part() attach can share this without QOM
 * child-name collisions. A record with no name stores the older text layout
 * from its part number alone. `s->onewire_bus` must already exist.
 */
static bool sgi_bridge_onewire_attach_part(SGIBridgeState *s,
                                           const char *child_name,
                                           const SGIBridgeBoardRecord *record,
                                           uint64_t serial, Error **errp)
{
    uint8_t rom[NIC_ROM_SIZE];
    DeviceState *part;

    if (record->name) {
        if (!sgi_bridge_board_record_check(record, errp)) {
            return false;
        }
    } else {
        const char *error = sgi_bridge_board_part_error(record->part);

        if (error) {
            error_setg(errp, "the I/O bridge %s", error);
            return false;
        }
    }
    if (serial & ~NIC_ROM_SERIAL_MASK) {
        error_setg(errp, "the I/O bridge one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", serial);
        return false;
    }

    if (record->name) {
        sgi_bridge_onewire_build_record(rom, record);
    } else {
        sgi_bridge_onewire_build_rom(rom, record->part);
    }
    part = qdev_new(TYPE_ONEWIRE_DS2505);
    object_property_add_child(OBJECT(s), child_name, OBJECT(part));
    qdev_prop_set_uint8(part, "family", NIC_ROM_FAMILY);
    qdev_prop_set_uint64(part, "serial", serial);
    ds2505_set_image(part, rom, sizeof(rom));
    return qdev_realize_and_unref(part, BUS(s->onewire_bus), errp);
}

bool sgi_bridge_add_onewire_board_part(DeviceState *bridge,
                                       const SGIBridgeBoardRecord *record,
                                       uint64_t serial, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(bridge);

    if (!record->name) {
        error_setg(errp, "a daughtercard's board-identity record needs a "
                   "board name");
        return false;
    }
    return sgi_bridge_onewire_attach_part(s, "onewire-part-2", record,
                                          serial, errp);
}

/* One bit slot, reported finished inside the write the guest polls after. */
static void sgi_bridge_onewire_slot(SGIBridgeState *s, uint32_t val)
{
    uint32_t *held = &s->reg[SGI_BRIDGE_SLOT(BRIDGE_NIC)];
    uint32_t pulse = (val & NIC_PULSE_MASK) >> NIC_PULSE_SHIFT;
    uint32_t sample = (val & NIC_SAMPLE_MASK) >> NIC_SAMPLE_SHIFT;
    bool level = sgi_onewire_slot(s->onewire_bus, pulse, sample);

    *held &= ~NIC_LEVEL;
    *held |= NIC_DONE | (level ? NIC_LEVEL : 0);
}

static void sgi_bridge_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    SGIBridgeState *s = opaque;
    const SGIBridgeReg *reg = sgi_bridge_reg(addr);
    uint32_t *held;
    uint32_t old;
    unsigned int slot;

    /*
     * An undefined address in the block posts and flags rather than faulting.
     */
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
        /* A group's strobe clears that group's errors and no other's. */
        if (addr - SGI_BRIDGE_REG_DATA == BRIDGE_INT_RESET) {
            if ((uint32_t)val & INT_RESET_PCI_GRP) {
                s->int_error &= ~INT_PCI_GROUP;
            }
            if ((uint32_t)val & INT_RESET_REQ_DSP_GRP) {
                s->int_error &= ~INT_INVALID_ADDR;
            }
            if ((uint32_t)val & INT_RESET_RESP_BUF_GRP) {
                s->int_error &= ~INT_RESP_BUF_GROUP;
            }
            if ((uint32_t)val & INT_RESET_MULTI_ERR) {
                s->int_error &= ~INT_MULTI_ERR;
            }
        }
        return;
    case SGI_BRIDGE_RW:
        old = *held;
        *held = (*held & ~reg->writable) | ((uint32_t)val & reg->writable);
        if (sgi_bridge_is_device_register(addr, &slot)) {
            /* Swap on an exempt slot is not reproduced; say so. */
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
        } else if (addr - SGI_BRIDGE_REG_DATA == BRIDGE_INT_ENABLE) {
            uint32_t newly_enabled = (*held & ~old) & s->pin_level &
                                     INT_DEVICE_PINS;

            for (slot = 0; slot < SGI_BRIDGE_DEVICES; slot++) {
                if (newly_enabled & (1u << slot)) {
                    sgi_bridge_send_interrupt(s, slot, true);
                }
            }
        }
        return;
    }

    g_assert_not_reached();
}

/* Both bounds declared so the core rejects instead of reshaping. */
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
 * Unpopulated SSRAM must complete accesses and float reads high: the PROM
 * sizes it with an unguarded probe, and all-ones probes as zero.
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
 * The flash port is 16 bits wide, so an XIO read of any size is the
 * part's words read one cycle at a time with the requested byte lanes kept,
 * and a part repeats across its chip select above the address lines it
 * decodes. A chip select with nothing on it, including the whole window on a
 * Bridge with no part fitted, fails the way an unpopulated widget address
 * does. Returns the part the window offset selects and the offset within its
 * chip select.
 */
static MemoryRegion *sgi_bridge_flash_part(SGIBridgeState *s, hwaddr addr,
                                           bool is_write, hwaddr *offset)
{
    MemoryRegion *part = addr < BRIDGE_FLASH_SELECT_SIZE ? s->flash :
                                                           s->flash_select1;

    if (part) {
        *offset = addr % BRIDGE_FLASH_SELECT_SIZE;
        return part;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-bridge: flash %s at window offset 0x%" HWADDR_PRIx
                  " with no part there\n", is_write ? "write" : "read", addr);
    return NULL;
}

static MemTxResult sgi_bridge_flash_read(void *opaque, hwaddr addr,
                                         uint64_t *value, unsigned size,
                                         MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    MemoryRegion *part;
    uint64_t part_size;
    uint64_t result = 0;
    hwaddr offset;
    unsigned i;

    part = sgi_bridge_flash_part(s, addr, false, &offset);
    if (!part) {
        return MEMTX_DECODE_ERROR;
    }
    part_size = memory_region_size(part);
    for (i = 0; i < size; i++) {
        hwaddr byte = (offset + i) % part_size;
        uint64_t word;
        MemTxResult r;

        r = memory_region_dispatch_read(part, byte & ~(hwaddr)1, &word,
                                        MO_BEUW, attrs);
        if (r != MEMTX_OK) {
            return r;
        }
        result = (result << 8) | (((byte & 1) ? word : word >> 8) & 0xff);
    }
    *value = result;
    return MEMTX_OK;
}

/*
 * The specification takes 16-bit writes only, on account of the parts'
 * command sequences, and completes none while the control register's flash
 * write enable is clear. How the chip reports the refused write is not
 * established; it is logged and dropped.
 */
static MemTxResult sgi_bridge_flash_write(void *opaque, hwaddr addr,
                                          uint64_t value, unsigned size,
                                          MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    MemoryRegion *part;
    hwaddr offset;

    part = sgi_bridge_flash_part(s, addr, true, &offset);
    if (!part) {
        return MEMTX_DECODE_ERROR;
    }
    if (!(s->reg[SGI_BRIDGE_SLOT(BRIDGE_CONTROL)] & CONTROL_FLASH_WRITE_EN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: flash write of 0x%" PRIx64 " at window "
                      "offset 0x%" HWADDR_PRIx " with the write enable "
                      "clear; not completed\n", value, addr);
        return MEMTX_OK;
    }
    if (size != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: %u-byte flash write of 0x%" PRIx64 " at "
                      "window offset 0x%" HWADDR_PRIx "; the port takes "
                      "16-bit writes only, not completed\n", size, value,
                      addr);
        return MEMTX_OK;
    }
    offset %= memory_region_size(part);
    return memory_region_dispatch_write(part, offset & ~(hwaddr)1, value,
                                        MO_BEUW, attrs);
}

static const MemoryRegionOps sgi_bridge_flash_ops = {
    .read_with_attrs = sgi_bridge_flash_read,
    .write_with_attrs = sgi_bridge_flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * 64-bit write or 32-bit upper-half read only; a 32-bit write would leave
 * half an entry behind.
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

/* The read-only lower-half view; a write is refused rather than swallowed. */
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
 * Undoes the guest's byte-lane aim (offset xor (4 - width)): what makes a
 * byte read of a vendor ID return the vendor's low byte.
 */
static uint32_t sgi_bridge_config_address(hwaddr addr, unsigned size)
{
    unsigned int reg = addr & BRIDGE_CFG0_REGISTER;
    unsigned int devfn = PCI_DEVFN(BRIDGE_CFG0_DEVICE(addr),
                                   BRIDGE_CFG0_FUNCTION(addr));

    reg = sgi_bridge_window_correct(reg, size);

    return (devfn << 8) | reg;
}

/*
 * No device drives device select: the specification's master time-out. A
 * device model present at the function's number is the one that claims the
 * cycle; one absent is an empty slot, or a function a card does not have.
 */
static bool sgi_bridge_config_claimed(SGIBridgeState *s, uint32_t address)
{
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;

    return pci_find_device(bus, pci_bus_num(bus), address >> 8) != NULL;
}

/*
 * The first enabled error in the PCI group owns its capture registers until
 * software clears the group; a later one only adds its status bit and the
 * multiple-error flag. A disabled error leaves no trace at all.
 */
static void sgi_bridge_pci_error(SGIBridgeState *s, uint32_t bit,
                                 unsigned int device, uint64_t pci_address)
{
    if (!(s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] & bit)) {
        return;
    }

    if (s->int_error & INT_PCI_GROUP) {
        s->int_error |= INT_MULTI_ERR;
    } else {
        s->reg[SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_UPPER)] =
            (device << PCI_ERR_DEVICE_SHIFT) |
            extract64(pci_address, 32, 16);
        s->reg[SGI_BRIDGE_SLOT(BRIDGE_PCI_ERR_LOWER)] = (uint32_t)pci_address;
    }
    s->int_error |= bit;
    sgi_bridge_raise_host_error(s);
}

/*
 * A read this chip sent for a device and a crossbar dropped: no response
 * comes back, so the chip's own request timer expires (Bridge ASIC
 * specification rev 4.0, table 58, "Crosstalk Read Request from PCI
 * device time-out"; the Crossbow datasheet's "DMA to an unavailable
 * Crossbow destination"). With bit 9 enabled it records the lower 48 bits
 * of the PCI address, the response buffer and the requesting device in
 * the response-buffer group. Table 58 names the Bridge error address
 * registers as the destination; the response buffer error registers
 * (tables 18 and 19) are the pair whose fields are a device, a buffer
 * and 48 address bits, so the model takes them (inferred). It holds no
 * response buffers, so the buffer field is zero. The timer expires at
 * once: the read has already failed back to the device.
 */
static void sgi_bridge_read_timeout(SGIBridgeState *s)
{
    if (!(s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] &
          INT_XREAD_REQ_TIMEOUT)) {
        return;
    }
    if (s->int_error & INT_RESP_BUF_GROUP) {
        s->int_error |= INT_MULTI_ERR;
    } else {
        s->reg[SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_UPPER)] =
            (s->dma_device << RESP_ERR_DEVICE_SHIFT) |
            extract64(s->dma_pci_address, 32, 16);
        s->reg[SGI_BRIDGE_SLOT(BRIDGE_RESP_ERR_LOWER)] =
            (uint32_t)s->dma_pci_address;
    }
    s->int_error |= INT_XREAD_REQ_TIMEOUT;
    sgi_bridge_raise_host_error(s);
}

/*
 * The cycle's PCI address: the device's ID select, one address line per
 * device from line 24, then the function and the register's word.
 */
static void sgi_bridge_master_timeout(SGIBridgeState *s, uint32_t address)
{
    unsigned int devfn = address >> 8;
    unsigned int device = PCI_SLOT(devfn);

    sgi_bridge_pci_error(s, INT_PCI_MASTER_TIMEOUT, device,
                         (1u << (24 + device)) | (address & 0x7fc));
}

/*
 * An unclaimed read fails back to the requester: the guests read empty slots
 * only inside a bus-error guard, and the IP27 PROM's probe takes a fault as
 * an empty slot. The data half floats high for a caller, such as qtest, that
 * sees the value without the failure.
 */
static MemTxResult sgi_bridge_config_read(void *opaque, hwaddr addr,
                                          uint64_t *data, unsigned size,
                                          MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);
    uint32_t address = sgi_bridge_config_address(addr, size);

    trace_sgi_bridge_config_read(addr, size);
    if (!sgi_bridge_config_claimed(s, address)) {
        sgi_bridge_master_timeout(s, address);
        *data = MAKE_64BIT_MASK(0, size * 8);
        return MEMTX_ERROR;
    }
    *data = pci_data_read(phb->bus, address, size);
    return MEMTX_OK;
}

/* A store posts: it has no response to carry the failure back. */
static MemTxResult sgi_bridge_config_write(void *opaque, hwaddr addr,
                                           uint64_t val, unsigned size,
                                           MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(opaque);
    uint32_t address = sgi_bridge_config_address(addr, size);

    trace_sgi_bridge_config_write(addr, size, val);
    if (!sgi_bridge_config_claimed(s, address)) {
        sgi_bridge_master_timeout(s, address);
        return MEMTX_OK;
    }
    pci_data_write(phb->bus, address, val, size);
    return MEMTX_OK;
}

/* The issued width is a term in the correction; no reshaping allowed. */
static const MemoryRegionOps sgi_bridge_config_ops = {
    .read_with_attrs = sgi_bridge_config_read,
    .write_with_attrs = sgi_bridge_config_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * Pins A and C reach the pin numbered as the slot, B and D that number with
 * bit two flipped, the arithmetic both guests compute themselves.
 */
static int sgi_bridge_map_irq(PCIDevice *dev, int pin)
{
    int slot = PCI_SLOT(dev->devfn);

    return (pin & 1) ? (slot ^ 4) : slot;
}

/*
 * The destination, compressed: bits 17:8 are XIO address bits 47:38,
 * bits 7:0 the interrupt number.
 */
#define INT_HOST_ADDR_SHIFT     8
#define INT_HOST_ADDR_BITS      10
#define XIO_HOST_ADDR_SHIFT     38

/* The host's interrupt register: data bit 8 selects raise. */
#define HUB_INT_MODIFY_SET      0x100

/* A write to the host's interrupt register; data bit 8 selects raise. */
static void sgi_bridge_deliver_interrupt(SGIBridgeState *s, hwaddr where,
                                         unsigned int bit, bool set)
{
    uint64_t value = bit | (set ? HUB_INT_MODIFY_SET : 0);

    address_space_stq(&s->fabric_as, where, value, MEMTXATTRS_UNSPECIFIED,
                      NULL);
}

/*
 * A (widget, XIO address) pair as the fabric address it travels at: the
 * widget in bits 51:48, where a link makes room for the packet header's
 * destination field.
 */
static bool sgi_bridge_compose_xio(SGIBridgeState *s,
                                   unsigned int widget, hwaddr address,
                                   hwaddr *fabric)
{
    if (widget >= (1U << SGI_BRIDGE_WIDGET_BITS) ||
        address >> SGI_BRIDGE_PACKET_ADDRESS_BITS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: packet fabric cannot encode widget %u "
                      "address 0x%" HWADDR_PRIx "\n", widget, address);
        return false;
    }
    *fabric = ((uint64_t)widget << SGI_BRIDGE_PACKET_ADDRESS_BITS) | address;
    return true;
}

/* The widget-level destination pair: one XIO address and its widget. */
static uint64_t sgi_bridge_int_dest(SGIBridgeState *s, unsigned int *widget)
{
    uint32_t upper = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_UPPER)];
    uint32_t lower = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_DEST_LOWER)];

    *widget = (upper & INT_DEST_WIDGET) >> INT_DEST_WIDGET_SHIFT;
    return ((uint64_t)(upper & INT_DEST_ADDRESS_UPPER) << 32) | lower;
}

/*
 * A pin's packet goes to the widget-level destination pair, with the
 * pin's own host-address field in place of the destination's bits 47:38.
 */
static void sgi_bridge_send_interrupt(SGIBridgeState *s, int pin, bool set)
{
    uint32_t host = s->reg[SGI_BRIDGE_SLOT(BRIDGE_INT_HOST(pin))];
    unsigned int address = (host & INT_HOST_ADDRESS) >> INT_HOST_ADDR_SHIFT;
    unsigned int widget;
    uint64_t dest;
    hwaddr where;

    if (!host) {
        /* An unprogrammed destination is an interrupt nobody routed. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: interrupt pin %d changed with no "
                      "destination programmed; no packet sent\n", pin);
        return;
    }

    /* The pin's own bits replace the destination's, they do not join. */
    dest = sgi_bridge_int_dest(s, &widget);
    dest = deposit64(dest, XIO_HOST_ADDR_SHIFT, INT_HOST_ADDR_BITS, address);
    if (!sgi_bridge_compose_xio(s, widget, dest, &where)) {
        return;
    }
    sgi_bridge_deliver_interrupt(s, where, host & INT_HOST_FIELD, set);
}

/* Chip errors go to the host-error bit at the widget destination pair. */
static void sgi_bridge_raise_host_error(SGIBridgeState *s)
{
    unsigned int number = s->reg[SGI_BRIDGE_SLOT(BRIDGE_HOST_ERR_FIELD)] &
                          INT_HOST_FIELD;
    unsigned int widget;
    uint64_t dest = sgi_bridge_int_dest(s, &widget);
    hwaddr where;

    /*
     * An unrouted error is one nobody asked for. This also refuses widget
     * W at XIO address zero, which a guest could legitimately program:
     * the guard predates the widget field and the register description is
     * silent about it, so it stays as it was.
     */
    if (!dest) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bridge: error interrupt with no destination "
                      "programmed; no packet sent\n");
        return;
    }
    if (!sgi_bridge_compose_xio(s, widget, dest, &where)) {
        return;
    }
    sgi_bridge_deliver_interrupt(s, where, number, true);
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

/*
 * A directly wired line: the machine driving one of the chip's GPIO inputs.
 */
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
 * Note the inversion: the swap bits enable a swapper that straightens the
 * crossed wiring, and a byte-addressed model is straight through already --
 * so the disabled case is the one that needs work.
 */
static bool sgi_bridge_dma_needs_correction(SGIBridgeState *s, unsigned int slot,
                                            hwaddr pci_addr)
{
    uint32_t dev = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DEVICE(slot))];

    if (!sgi_bridge_slot_needs_correction(s, slot)) {
        return false;
    }
    /* Local space never crosses the swapper. */
    if (pci_addr < BRIDGE_PCI_TRANSLATED) {
        return false;
    }
    /* Both direct windows take one bit, the page-mapped window its own. */
    return !(dev & (pci_addr < BRIDGE_PCI_DIRECT ? DEV_SWAP_TRANSLATED
                                                 : DEV_SWAP_DIRECT));
}

/*
 * THE PAIR OF ENDIANNESSES HERE IS REQUIRED DESPITE THE APPARENT REDUNDANCY:
 * the body must stay address_space_read/write into a byte buffer, which
 * resolves the answering region's endianness before this code sees the bytes.
 * A value accessor such as address_space_ldl_be() reads as a simplification
 * and reintroduces that endianness as a live term, transfers aimed at
 * another widget's registers arrive reversed. The with-attrs form is what
 * lets a refused transfer be refused here too instead of answered with
 * zeroes. These callbacks never target this space, so there is no way back
 * into themselves.
 */
static MemTxResult sgi_bridge_lane_swap_read(void *opaque, hwaddr addr,
                                             uint64_t *val, unsigned size,
                                             MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    uint8_t bytes[8];
    MemTxResult res;

    trace_sgi_bridge_lane_swap_read(addr, size);
    res = address_space_read(&s->fabric_as,
                             sgi_bridge_window_correct(addr, size),
                             attrs, bytes, size);
    *val = ldn_be_p(bytes, size);
    return res;
}

static MemTxResult sgi_bridge_lane_swap_write(void *opaque, hwaddr addr,
                                              uint64_t val, unsigned size,
                                              MemTxAttrs attrs)
{
    SGIBridgeState *s = opaque;
    uint8_t bytes[8];

    trace_sgi_bridge_lane_swap_write(addr, size);
    stn_be_p(bytes, size, val);
    return address_space_write(&s->fabric_as,
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

/*
 * The decode shared by every DMA route. A slot's own route (slot >= 0) may
 * send the access through the byte-lane swap; the plain route (slot < 0)
 * never does. It exists for byte-stream transfers whose swap nothing
 * observes, so paying a per-word dispatch for it buys nothing.
 */
static IOMMUTLBEntry sgi_bridge_translate_common(SGIBridgeState *s, int slot,
                                                 hwaddr addr,
                                                 IOMMUAccessFlags flag)
{
    unsigned int shift = sgi_bridge_page_shift(s);
    hwaddr page_mask = MAKE_64BIT_MASK(0, shift);
    IOMMUTLBEntry entry = {
        /*
         * The swap decision is taken on the raw PCI address, ahead of the
         * 64-bit decode below: rewritten first, a wide access is measured
         * against the wrong window. Both arms land in the requesting node's
         * fabric view, the claim view the machine roots at fabric_as, and
         * the swapping arm reaches it through the lane_swap ops, so a
         * transfer resolves to the node it names by programmed number, the
         * same route the interrupt path takes. The global map would collapse
         * a second node's DMA into node zero's memory while the nodes still
         * read the same nasid during renumbering. The plain route rides the
         * unswapped arm: it skips the byte lanes alone, never the routing.
         */
        .target_as = slot >= 0 &&
                     sgi_bridge_dma_needs_correction(s, slot, addr) ?
                     &s->lane_swap_as : &s->fabric_as,
        .iova = addr & ~page_mask,
        .translated_addr = 0,
        .addr_mask = page_mask,
        .perm = IOMMU_NONE,
    };
    uint32_t direct;
    uint64_t ate;
    unsigned int index;
    hwaddr offset;
    hwaddr fabric;

    if (slot >= 0) {
        trace_sgi_bridge_dma_xlate(addr, slot, flag);
    } else {
        trace_sgi_bridge_dma_xlate_plain(addr, flag);
    }
    /*
     * The plain route serves a device that skips the byte lanes and names
     * no slot here, so a timed-out read on it records device zero.
     */
    s->dma_pci_address = addr;
    s->dma_device = slot >= 0 ? slot : 0;

    if (addr < BRIDGE_PCI_TRANSLATED) {
        /* Local: not carried to XIO at all. */
        entry.translated_addr = addr & ~page_mask;
        entry.perm = IOMMU_RW;
        return entry;
    }

    if (addr >= BRIDGE_PCI_64BIT) {
        /*
         * Ahead of the 31-bit direct path, which would drop the NASID at bit
         * 32 and collapse another node's transfer into node zero's.
         */
        unsigned int widget = (addr >> BRIDGE_DIRECT64_WIDGET_SHIFT) & 0xf;

        if (widget == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-bridge: 64-bit DMA names widget 0, which the "
                          "crossbar cannot answer\n");
        }
        if (!sgi_bridge_compose_xio(s, widget, addr & BRIDGE_XIO_PASS,
                                    &fabric)) {
            return entry;
        }
        entry.translated_addr = fabric & ~page_mask;
        entry.perm = IOMMU_RW;
        trace_sgi_bridge_dma_xlate_64(addr, entry.translated_addr, widget);
        return entry;
    }

    if (addr >= BRIDGE_PCI_DIRECT) {
        unsigned int widget;

        direct = s->reg[SGI_BRIDGE_SLOT(BRIDGE_DIRECT_MAP)];
        offset = (hwaddr)(direct & DIRECT_OFFSET) << BRIDGE_DIRECT_SHIFT;
        offset |= addr & BRIDGE_DIRECT_PASS;

        /* Applies only with a zero offset field; the bit resets set. */
        if ((direct & DIRECT_OFFSET) == 0 && (direct & DIRECT_ADD_512MB)) {
            offset += BRIDGE_DIRECT_512MB;
        }

        widget = (direct & DIRECT_WIDGET) >> DIRECT_WIDGET_SHIFT;
        if (!sgi_bridge_compose_xio(s, widget, offset, &fabric)) {
            return entry;
        }
        entry.translated_addr = fabric & ~page_mask;
        entry.perm = IOMMU_RW;
        trace_sgi_bridge_dma_xlate_direct(addr, entry.translated_addr, widget);
        return entry;
    }

    /* Through the table. The index is the page number inside the window. */
    index = (addr - BRIDGE_PCI_TRANSLATED) >> shift;
    if (index >= SGI_BRIDGE_ATE_ENTRIES) {
        /*
         * An external static RAM would supply the rest; this board has none.
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

    if (!sgi_bridge_compose_xio(s, (ate & ATE_TARGET_WIDGET) >> 8,
                                ate & ATE_ADDRESS, &fabric)) {
        return entry;
    }
    entry.translated_addr = fabric & ~page_mask;
    entry.perm = IOMMU_RW;
    return entry;
}

static IOMMUTLBEntry sgi_bridge_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                          IOMMUAccessFlags flag, int iommu_idx)
{
    struct SGIBridgeDeviceDMA *dma =
        container_of(iommu, struct SGIBridgeDeviceDMA, iommu);

    return sgi_bridge_translate_common(dma->s, dma->slot, addr, flag);
}

static IOMMUTLBEntry sgi_bridge_plain_translate(IOMMUMemoryRegion *iommu,
                                                hwaddr addr,
                                                IOMMUAccessFlags flag,
                                                int iommu_idx)
{
    SGIBridgeState *s = container_of(iommu, SGIBridgeState, plain_iommu);

    return sgi_bridge_translate_common(s, -1, addr, flag);
}

/* One table for the bus; the swap attribute is the requesting slot's. */
static AddressSpace *sgi_bridge_device_address_space(PCIBus *bus, void *opaque,
                                                     int devfn)
{
    SGIBridgeState *s = opaque;
    unsigned int slot = PCI_SLOT(devfn);

    /* Unreachable by guests; logged so a mis-wired machine shows. */
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

/*
 * The plain route, for the transfers whose swaps compose to the identity. The
 * slot's own space applies the byte-lane swap its Device(x) register asks
 * for; where the card's own programming swaps the same data back, the pair is
 * unobservable and the per-word window only costs. The device model moves
 * those legs here, which legs they are is its call, made from the card's
 * programmed pairing. Same decode, same node-qualified fabric routing: only
 * the swap differs.
 */
AddressSpace *sgi_bridge_plain_dma_as(PCIDevice *dev)
{
    SGIBridgeState *s = (SGIBridgeState *)
        object_dynamic_cast(OBJECT(BUS(pci_get_bus(dev))->parent),
                            TYPE_SGI_BRIDGE);

    return s ? &s->plain_dma_as : NULL;
}

#define TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION "sgi-bridge-iommu-memory-region"

/*
 * Nothing invalidates a cached mapping when the guest rewrites an entry, so
 * map notifiers (vfio, vhost) are refused rather than fed stale ones.
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

#define TYPE_SGI_BRIDGE_PLAIN_IOMMU_MEMORY_REGION \
    "sgi-bridge-plain-iommu-memory-region"

static void sgi_bridge_plain_iommu_class_init(ObjectClass *oc,
                                              const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = sgi_bridge_plain_translate;
    imrc->notify_flag_changed = sgi_bridge_iommu_notify_flag_changed;
}

/*
 * The chip's own core: the register file, the translation entries and the
 * latched error bit. A cold reset and the core reset an inbound link reset
 * drives share this, so the two cannot drift apart. The interrupt pins are
 * not in here; their levels belong to whatever is driving them.
 */
static void sgi_bridge_reset_core(SGIBridgeState *s)
{
    unsigned int i;

    for (i = 0; i < SGI_BRIDGE_REG_SLOTS; i++) {
        s->reg[i] = sgi_bridge_regs[i].reset;
    }

    /* The one reset value the table cannot hold: the machine's identity. */
    s->reg[SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] =
        sgi_bridge_identification(s);

    s->int_error = 0;

    /*
     * Whole entries, not just valid bits: a stale page address behind a clear
     * valid bit is a working translation waiting for one write.
     */
    memset(s->ate_entry, 0, sizeof(s->ate_entry));

    sgi_bridge_aim_all_windows(s);
}

static void sgi_bridge_reset_hold(Object *obj, ResetType type)
{
    SGIBridgeState *s = SGI_BRIDGE(obj);

    sgi_bridge_reset_core(s);

    /* Every source of a pin resets too, so the levels start clear. */
    s->pin_level = 0;
    s->pci_pin_level = 0;
    s->gpio_pin_level = 0;
}

/*
 * The crossbar's per-link reset, arriving. Revision D feeds the link-layer
 * block's received link-reset output into its core-logic reset, and IRIX
 * treats Bridge core programming as lost across the pulse: it saves the
 * widget control and link configuration registers, has the crossbar reset the
 * link, restores those two and rebuilds the rest. This model answers that by
 * taking the whole core as lost, the register file, the translation entries
 * and the latched error back to their power-up values, the control register's
 * four bus-reset bits included, which this chip only stores. The bounded IRIX
 * read does not prove the reset or retention state of any individual
 * register, and that boundary is open, so this is a whole-core answer to
 * whole-core evidence rather than a register-by-register account.
 *
 * Two things deliberately stay. The interrupt pins keep their levels: the
 * devices holding them are not in this reset, and a level cleared here would
 * never rise again. And nothing reaches the PCI side. No reviewed source
 * establishes that Bridge pulses its four external bus-reset outputs or that
 * a downstream device resets, so a card keeps everything it was programmed
 * with.
 */
static void sgi_bridge_link_reset(void *opaque, int line, int level)
{
    SGIBridgeState *s = opaque;

    if (!level) {
        return;
    }
    sgi_bridge_reset_core(s);
    qemu_log_mask(LOG_UNIMP,
                  "sgi-bridge: inbound link reset; the core is reset once "
                  "and released, not held until the link connection is "
                  "determined: no reviewed source says what a core held in "
                  "reset answers\n");
}

/*
 * The one place a device's byte-lane declaration is read: at plug time, into
 * a slot mask, so that every later access is a mask test rather than a walk
 * of the bus. Reading it here also means a card carries its own answer into
 * whatever slot it is put in, which a mask the board computed cannot.
 */
static void sgi_bridge_plug(const HotplugHandler *handler, DeviceState *dev,
                            Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(handler);
    Object *obj = OBJECT(dev);
    Error *local_err = NULL;
    bool self_ordered;

    /* Saying nothing is the answer an ordinary card gives: correct it. */
    if (!object_dynamic_cast(obj, TYPE_PCI_DEVICE) ||
        !object_property_find(obj, SGI_BRIDGE_BYTE_LANE_SELF_ORDERED)) {
        return;
    }

    self_ordered = object_property_get_bool(obj,
                                            SGI_BRIDGE_BYTE_LANE_SELF_ORDERED,
                                            &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (self_ordered) {
        /*
         * Folded the way the DMA attributes are, for a slot past the eight.
         */
        s->self_ordered_slots |=
            1u << (PCI_SLOT(PCI_DEVICE(dev)->devfn) % SGI_BRIDGE_DEVICES);
    }
}

/*
 * The slots are soldered on this machine, so the handler serves cold plug
 * only. Admitting hot plug would let a card appear that the firmware's
 * configuration, built once at startup, never describes, a state the real
 * machine cannot reach.
 */
static bool sgi_bridge_is_hotpluggable_bus(const HotplugHandler *handler, BusState *bus)
{
    return false;
}

static void sgi_bridge_realize(DeviceState *dev, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);
    const SGIBridgeBoardRecord record = {
        .barcode = s->board_barcode,
        .part = s->board_part,
        .revision = s->board_revision,
        .name = s->board_name,
    };
    const char *board_part_error;
    unsigned int slot;

    /* Refuse property errors before the first region exists. */
    board_part_error = sgi_bridge_board_part_error(record.part);
    if (board_part_error) {
        error_setg(errp, "the I/O bridge %s", board_part_error);
        return;
    }
    if (record.name) {
        if (!sgi_bridge_board_record_check(&record, errp)) {
            return;
        }
    } else if (record.barcode || record.revision) {
        error_setg(errp, "the I/O bridge board-barcode and board-revision "
                   "properties need board-name");
        return;
    }
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

    /*
     * The MicroLAN bus and the one board-identity part every Bridge realizes
     * for its own board: a DS2505 EPROM, which is what the family code 0x0b
     * names. The bus is architecturally multi-drop. A carrier fitting a
     * daughtercard behind this same chip calls
     * sgi_bridge_add_onewire_board_part() afterwards to add that card's own
     * record alongside this one.
     *
     * The record is composed here and never again. An EPROM keeps its
     * contents across a reset, that is what the technology is, so a chip
     * that rebuilt the bytes on every reset was modelling something the
     * hardware does not do. Those bytes are now the part's own migrated state
     * as well, and state with one owner wants one writer. Reaching into a
     * child device's storage from a parent's reset is the wrong shape
     * whichever order the phases run in, and they run in the safe one:
     * hw/core/resettable.c holds children before the object itself, so the
     * ordering was never the hazard.
     */
    s->onewire_bus = onewire_create_bus(dev, "onewire");
    if (!sgi_bridge_onewire_attach_part(s, "onewire-part", &record,
                                        s->onewire_serial, errp)) {
        return;
    }

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
    for (unsigned int select = 0; select < 2; select++) {
        MemoryRegion *part = select ? s->flash_select1 : s->flash;
        uint64_t part_size = part ? memory_region_size(part) : 0;

        if (part && (!is_power_of_2(part_size) ||
                     part_size > BRIDGE_FLASH_SELECT_SIZE)) {
            error_setg(errp, "the I/O bridge flash part must be a power of "
                       "two of at most 2 MB, not %" PRIu64 " bytes",
                       part_size);
            return;
        }
    }
    memory_region_init_io(&s->flash_window, obj, &sgi_bridge_flash_ops, s,
                          "sgi-bridge.flash", BRIDGE_FLASH_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_FLASH_BASE,
                                &s->flash_window);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->widget);

    memory_region_init(&s->packet, obj, "sgi-bridge.packet",
                       1ULL << SGI_BRIDGE_PACKET_ADDRESS_BITS);
    memory_region_init_alias(&s->packet_io, obj, "sgi-bridge.packet-io",
                             &s->widget, 0, memory_region_size(&s->widget));
    memory_region_add_subregion(&s->packet,
                                1ULL << (SGI_BRIDGE_PACKET_ADDRESS_BITS - 1),
                                &s->packet_io);

    /* Interrupt packets travel the same fabric a processor's access does. */
    address_space_init(&s->fabric_as, s->fabric, "sgi-bridge.fabric");

    /* Full 32-bit I/O space: this chip does not narrow it to 64 KB. */
    memory_region_init(&s->pci_mmio, obj, "sgi-bridge.pci-mmio", 4 * GiB);
    memory_region_init(&s->pci_io, obj, "sgi-bridge.pci-io", 4 * GiB);
    address_space_init(&s->pci_mmio_as, &s->pci_mmio, "sgi-bridge.pci-mmio");
    address_space_init(&s->pci_io_as, &s->pci_io, "sgi-bridge.pci-io");

    /*
     * See the header comment on SGI_BRIDGE_PIO_XLATE_BASE for the evidence.
     * Not an alias: an alias cannot carry the byte-lane correction, exactly
     * the reason the per-device windows below use dedicated ops too.
     */
    memory_region_init_io(&s->pio_xlate, obj, &sgi_bridge_pio_xlate_ops, s,
                          "sgi-bridge.pio-xlate", SGI_BRIDGE_PIO_XLATE_SIZE);
    memory_region_add_subregion(&s->widget, SGI_BRIDGE_PIO_XLATE_BASE,
                                &s->pio_xlate);

    /* Not aliases: an alias cannot carry the byte-lane correction. */
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
     * Refuse a device past slot 7 when it is added, rather than aborting in
     * the interrupt core the first time it drives an unwired pin.
     */
    pci_bus_set_slot_reserved_mask(phb->bus, ~((1u << SGI_BRIDGE_PINS) - 1));

    /* Every card passes through here, so each one's declaration is read. */
    qbus_set_hotplug_handler(BUS(phb->bus), obj);

    /*
     * The same pins, directly wired: both domains can drive one pin, so the
     * wire lines keep their own level word beside the PCI core's.
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
     * The re-entrancy guard is per owning device and this chip owns both ends
     * of an ordinary transfer, so normal DMA would trip it; regions reached
     * through this one still apply their own guards.
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
    /* The plain route: one per bridge, the swap was the per-slot part. */
    memory_region_init_iommu(&s->plain_iommu, sizeof(s->plain_iommu),
                             TYPE_SGI_BRIDGE_PLAIN_IOMMU_MEMORY_REGION, obj,
                             "sgi-bridge.iommu.plain", UINT64_MAX);
    address_space_init(&s->plain_dma_as, MEMORY_REGION(&s->plain_iommu),
                       "sgi-bridge.dma.plain");
    pci_setup_iommu(phb->bus, &sgi_bridge_iommu_ops, s);

    /* The translation table, and the read-only view of its lower halves. */
    memory_region_init_io(&s->ate, obj, &sgi_bridge_ate_ops, s,
                          "sgi-bridge.ate", BRIDGE_ATE_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_BASE, &s->ate);
    memory_region_init_io(&s->ate_low, obj, &sgi_bridge_ate_low_ops, s,
                          "sgi-bridge.ate-low", BRIDGE_ATE_LOW_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_ATE_LOW_BASE, &s->ate_low);
}

MemoryRegion *sgi_bridge_packet_face(SGIBridgeState *s)
{
    return &s->packet;
}

static void sgi_bridge_unrealize(DeviceState *dev)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    int slot;

    /*
     * pci_register_root_bus() put this chip on the PCI core's file-scope list
     * of host bridges and took no reference for it. query-pci and
     * pci_qdev_find_device() both walk that list, so an entry left behind
     * hands them a bridge that has been freed. Undoing the registration is
     * this device's job; nothing else in the core does it.
     *
     * qbus_unrealize() inside pci_root_bus_cleanup() is a second call on a
     * bus device_set_realized() has already unrealized, and is a no-op.
     */
    pci_unregister_root_bus(phb->bus);

    address_space_destroy(&s->plain_dma_as);
    for (slot = SGI_BRIDGE_DEVICES - 1; slot >= 0; slot--) {
        address_space_destroy(&s->dma[slot].as);
    }
    address_space_destroy(&s->lane_swap_as);
    address_space_destroy(&s->pci_io_as);
    address_space_destroy(&s->pci_mmio_as);
    address_space_destroy(&s->fabric_as);
}

/* post_load re-aims the windows from the restored registers. */
static int sgi_bridge_post_load(void *opaque, int version_id)
{
    SGIBridgeState *s = opaque;

    sgi_bridge_aim_all_windows(s);
    /*
     * An older stream carries only the effective levels; attribute them to
 * the PCI side. A wire level is re-driven by its source device.
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
         * Sixty bytes of the chip's own one-wire responder state, kept as
         * padding so streams written before the part became a device on a bus
         * still load. That responder is gone and the part migrates its own
         * conversation now, so the bytes are written zero and discarded
         * rather than translated.
         */
        VMSTATE_UNUSED_V(3, 60),
        VMSTATE_END_OF_LIST()
    }
};

/* The chip revision and board serial belong to the machine that fits them. */
static const Property sgi_bridge_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIBridgeState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("revision", SGIBridgeState, revision, 1),
    DEFINE_PROP_UINT32("part", SGIBridgeState, part, 0xc002),
    DEFINE_PROP_UINT32("manufacturer", SGIBridgeState, manufacturer, 0x036),
    DEFINE_PROP_UINT64("onewire-serial", SGIBridgeState, onewire_serial,
                       NIC_ROM_SERIAL_DEFAULT),
    DEFINE_PROP_STRING("board-part", SGIBridgeState, board_part),
    DEFINE_PROP_STRING("board-barcode", SGIBridgeState, board_barcode),
    DEFINE_PROP_STRING("board-revision", SGIBridgeState, board_revision),
    DEFINE_PROP_STRING("board-name", SGIBridgeState, board_name),
    DEFINE_PROP_STRING("pci-bus-name", SGIBridgeState, pci_bus_name),
    /*
     * The part on the flash port's base chip select, a 16-bit region of at
     * most 2 MB; unset on a board that fits none.
     */
    DEFINE_PROP_LINK("flash", SGIBridgeState, flash, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    /*
     * Whatever a board fits on the port's other chip select, answering 2 MB
     * into the window with the same 16-bit cycles; unset where it is empty.
     */
    DEFINE_PROP_LINK("flash-select1", SGIBridgeState, flash_select1,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    /*
     * Forces slots exempt whatever their cards say, for a test or a debug run
     * that wants the answer a card cannot be asked for. Cards declare
     * themselves; this is the override, not the mechanism.
     */
    DEFINE_PROP_UINT32("byte-lane-exempt", SGIBridgeState, byte_lane_exempt, 0),
};

/*
 * ---- The XIO port interface ----
 *
 * A Bridge is a leaf widget: a peer's programmed I/O lands on its widget
 * space, and a packet on its packet face, with no destination widget in
 * either. Its link peer's packet fabric is where its own DMA and
 * interrupt packets go, composed as (widget << 48) | address; the link
 * presents a leaf peer at every widget number, so this Bridge never needs
 * to know whether a Hub or a Crossbow is on the other end. index is
 * unused: a Bridge has one XIO port.
 */

static const char *sgi_bridge_xio_port_describe(Object *port,
                                                unsigned int index)
{
    return "a Bridge";
}

/* Both faces exist once the Bridge has realized, and not before. */
static MemoryRegion *sgi_bridge_xio_port_cpu_face(Object *port,
                                                  unsigned int index)
{
    return DEVICE(port)->realized ?
        sysbus_mmio_get_region(SYS_BUS_DEVICE(port), 0) : NULL;
}

static MemoryRegion *sgi_bridge_xio_port_packet_face(Object *port,
                                                     unsigned int index)
{
    return DEVICE(port)->realized ?
        sgi_bridge_packet_face(SGI_BRIDGE(port)) : NULL;
}

static bool sgi_bridge_xio_port_connect(Object *port, unsigned int index,
                                        const SGIXioPeer *peer, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(port);

    if (DEVICE(s)->realized) {
        error_setg(errp, "a Bridge takes its link before it realizes");
        return false;
    }
    if (!peer->packet_fabric) {
        error_setg(errp, "a Bridge's link peer must take packets");
        return false;
    }
    s->fabric = peer->packet_fabric;
    return true;
}

/* A reset from the far end of the link clears the chip's core. */
/* Only a read owns a response buffer; a dropped write is never heard of. */
static void sgi_bridge_xio_port_request_dropped(Object *port,
                                                unsigned int index,
                                                hwaddr addr, bool write)
{
    if (!write) {
        sgi_bridge_read_timeout(SGI_BRIDGE(port));
    }
}

static qemu_irq sgi_bridge_xio_port_reset_input(Object *port,
                                                unsigned int index)
{
    return qdev_get_gpio_in_named(DEVICE(port), "link-reset", 0);
}

/*
 * One XIO link, so one line: what the crossbar drives at us. It exists
 * from instance init so a link can wire it before realize.
 */
static void sgi_bridge_instance_init(Object *obj)
{
    qdev_init_gpio_in_named_with_opaque(DEVICE(obj), sgi_bridge_link_reset,
                                        SGI_BRIDGE(obj), "link-reset", 1);
}

static void sgi_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(oc);

    dc->desc = "SGI Bridge XIO-to-PCI host bridge";
    dc->fw_name = "pci";
    dc->realize = sgi_bridge_realize;
    dc->unrealize = sgi_bridge_unrealize;
    dc->vmsd = &vmstate_sgi_bridge;
    /*
     * A machine wires this chip to a fabric and a board identity, and a
     * command line can wire the same links to test parts instead, which is
     * how the chip's own address arithmetic is asserted without a machine's
     * constants standing between the test and the answer.
     */
    dc->user_creatable = true;
    hc->plug = sgi_bridge_plug;
    hc->is_hotpluggable_bus = sgi_bridge_is_hotpluggable_bus;
    rc->phases.hold = sgi_bridge_reset_hold;
    device_class_set_props(dc, sgi_bridge_properties);
    pc->describe = sgi_bridge_xio_port_describe;
    pc->cpu_face = sgi_bridge_xio_port_cpu_face;
    pc->packet_face = sgi_bridge_xio_port_packet_face;
    pc->connect = sgi_bridge_xio_port_connect;
    pc->reset_input = sgi_bridge_xio_port_reset_input;
    pc->request_dropped = sgi_bridge_xio_port_request_dropped;
}

static const TypeInfo sgi_bridge_types[] = {
    {
        .name = TYPE_SGI_BRIDGE,
        .parent = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(SGIBridgeState),
        .instance_init = sgi_bridge_instance_init,
        .class_init = sgi_bridge_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_HOTPLUG_HANDLER },
            { TYPE_SGI_XIO_PORT },
            { },
        },
    },
    {
        .name = TYPE_SGI_BRIDGE_IOMMU_MEMORY_REGION,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = sgi_bridge_iommu_class_init,
    },
    {
        .name = TYPE_SGI_BRIDGE_PLAIN_IOMMU_MEMORY_REGION,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = sgi_bridge_plain_iommu_class_init,
    },
};

DEFINE_TYPES(sgi_bridge_types)
