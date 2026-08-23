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
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

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

/*
 * The one-wire bus master to the board identity part. Bit 0 is the level
 * sampled off the wire and bit 1 says the slot has finished; the two timing
 * fields say how long to hold the wire down and how long to wait before
 * sampling, both in microseconds, and writing them is what starts a slot.
 *
 * A guest drives the whole protocol through this one register, a bit at a
 * time, so the slot kind has to be recovered from the timings — which is why
 * the pulse widths below are ranges rather than the values any one guest
 * writes. Mainline Linux holds the wire down for 520 microseconds to reset it
 * where OpenBSD uses 500, and a model keyed on either exactly would answer one
 * guest and not the other. What matters is the sequence of slots, not the
 * microseconds.
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
 * What separates a reset from a bit slot: a reset holds the wire down for
 * hundreds of microseconds and a bit slot for tens or fewer. A threshold
 * rather than a value, because mainline resets with 520 microseconds where
 * OpenBSD uses 500, and a model keyed on either exactly would answer one guest
 * and not the other. Both fall the same side of this line.
 *
 * A written zero and a written one are also different widths, and the part
 * does distinguish them: that is how a guest's commands and page offsets
 * arrive, a bit per slot. A read uses the same short slot as a written one,
 * so timing cannot separate those two — the part answers whichever the
 * conversation is expecting, which is why it tracks where it has got to.
 */
#define NIC_PULSE_IS_RESET      400

/*
 * A written zero holds the wire down for far longer than a written one, which
 * is how this bus carries a bit from the guest to the part. A read uses the
 * same short slot as a written one — the guest lets the wire go and samples
 * what is left — so the part cannot tell a read from a written one by timing,
 * and does not need to: it answers whatever the conversation expects next.
 */
#define NIC_PULSE_IS_ZERO       40

/*
 * Where the part is in a conversation. A guest drives one bit per register
 * write, so this has to survive between them.
 */
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
 * The commands this part answers, by the numbers the bus assigns them.
 *
 * There are two ways to address a part and this file answered only one of them
 * until 2026-08-12, on the reasoning that a guest with a single part on the bus
 * would skip the addressing step rather than name the part. That is one guest's
 * habit and not the bus's rule: real firmware searches the bus, learns the
 * identity the search hands back, resets, and then names that identity. That
 * is the ordinary discovery form, and this part answered it by going deaf.
 * So both are here, and a command that is neither says so rather than idling.
 */
#define NIC_CMD_SEARCH_ROM      0xf0
#define NIC_CMD_MATCH_ROM       0x55
#define NIC_CMD_SKIP_ROM        0xcc
#define NIC_CMD_READ_MEMORY     0xa5
#define NIC_CMD_READ_PLAIN      0xf0
#define NIC_CMD_READ_STATUS     0xaa

/*
 * Two commands read the same data memory and they frame it differently, which
 * is the whole reason the check a guest validates has to be *stored* rather
 * than produced on the way out.
 *
 * The extended read walks page redirection and sends, per page, a redirection
 * byte, a check over the request, the page, and a check over the page. The
 * plain read sends nothing but the stored bytes, from the address asked for to
 * the end of the data memory, and a check over all of it at the very end. So a
 * page's own embedded check — the one the board-identity reader validates
 * before it will trust a part number — is not framing that either command
 * adds. It is programmed into the last two bytes of the page at the factory,
 * and both commands return it because both return what is stored.
 *
 * The data memory is sixty-four pages of thirty-two bytes. That size is not a
 * free choice: the status memory this part answers carries one redirection
 * byte per page and has sixty-four of them, so a smaller data memory would
 * contradict the status memory the model already serves.
 */
#define NIC_DATA_PAGES          64
#define NIC_DATA_SIZE           (NIC_DATA_PAGES * NIC_PAGE_SIZE)
#define NIC_UNPROGRAMMED        0xff

/*
 * The part's status memory, which is separate from the memory holding the
 * board's part number and is read with its own command. It carries the bits
 * that write-protect each page, the bits that protect each page's redirection
 * byte, a bitmap the part's own filesystem software uses, and the redirection
 * bytes themselves — which say, per page, whether that page's data is still
 * where it looks like it is or has been superseded by another page.
 *
 * Every byte of ours reads as unprogrammed, and that is a fact about the
 * technology rather than a convenient default. The memory is EPROM: a bit
 * leaves the factory set and programming can only clear it, so a part whose
 * status memory was never written reads ones everywhere. The addresses the
 * part does not implement read the same. In a redirection byte that value is
 * the one that says the page is valid where it sits, which is what our part
 * needs to say — its one programmed page is the board identity, and nothing
 * has ever superseded it.
 *
 * A status page is eight bytes and the last address is 0x13f; the part sends
 * a page's worth of data and then a check pair, page after page, until the end
 * of the status memory or until the guest resets.
 */
#define NIC_STATUS_UNPROGRAMMED 0xff
#define NIC_STATUS_PAGE         8
#define NIC_STATUS_LAST         0x13f

/*
 * The page a guest reads at a time, and what the part sends back for one: a
 * redirection byte saying the page has not been moved, a check pair over the
 * request and that byte, the page itself, and a check pair over the page. The
 * guest reads the first three, checks them, and only then reads the rest.
 */
#define NIC_PAGE_SIZE           32
#define NIC_PAGES               2
#define NIC_NOT_REDIRECTED      0xff
#define NIC_ANSWER_SIZE         (3 + NIC_PAGE_SIZE + 2)

/*
 * What the part calls itself: a family code in the low byte, six bytes of
 * serial number, and a check byte over the seven. **The family code is the
 * load-bearing part** — a guest matches on it to decide this is a memory it
 * knows how to read, and the wrong one means the part is found and ignored.
 * The serial identifies the physical board and is supplied by the machine;
 * the check byte is computed over the other seven and a guest verifies it
 * before believing any of them.
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

/* The external-SSRAM window: 512 KB at 0x08_0000, unpopulated on this board. */
#define BRIDGE_SSRAM_BASE       0x080000
#define BRIDGE_SSRAM_SIZE       (512 * KiB)
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
 *
 * The base and the per-device stride are the chip's public geometry and live
 * in its header, because the machine names a device's configuration window
 * too — the console descriptor hands firmware one. Only the decode of an
 * incoming address is private to this file.
 */
#define BRIDGE_CFG0_SIZE        (SGI_BRIDGE_DEVICES * \
                                 SGI_BRIDGE_CFG0_DEVICE_SIZE)
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

/*
 * Each device's window comes up already aimed at that device's own default
 * region, and the regions are not all the same size: devices 0 and 1 get two
 * megabytes each and devices 2 through 7 get one. Laid end to end from PCI
 * address zero, that puts device n's window at 2n megabytes for the first two
 * and n+2 for the rest, which is what this field holds — it supplies PCI
 * address bits 31:20, so a megabyte is one count.
 *
 * The specification does not give this field a power-up value. Its register
 * table states a reset value for every other field and, for this one, refers to
 * its address-mapping chapter instead — and that chapter describes which of
 * these bits drive which address bits rather than what they start as. So the
 * value here does not come from the specification.
 *
 * It comes from the firmware, which states all eight and rejects a Bridge whose
 * registers hold anything else: its sanity diagnostic prints the value it
 * expected beside the value it read, for each device in turn. The eight it
 * expects are exactly the layout above. That the increments step by two for the
 * first two devices and by one thereafter is the same two-megabyte split the
 * ASIC specification's mapping table and OpenBSD's own header both describe,
 * which is why this is a reading of three agreeing sources rather than a
 * pattern fitted to eight numbers.
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
        "device", SGI_BRIDGE_RW, DEVICE_RESET(n), DEVICE_WRITABLE }
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

    /*
     * The one-wire master. The timing fields are the writable part; the level
     * and the finished flag are answers this model writes into the same word
     * when a slot runs, which is why they are outside the writable mask.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_NIC)] = {
        "one-wire bus master", SGI_BRIDGE_RW, 0, NIC_WRITABLE },
    /*
     * The mask keeps the two status bits out of a guest's reach, and no test
     * asserts that, because with nothing on the bus it cannot be observed: a
     * slot runs on every write and sets both bits afterwards, so the register
     * reads the same whether or not the guest could have written them. It
     * becomes observable when a part can pull the level low, and the test to
     * write then is that the level follows the part rather than the write.
     */

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
     * So the pin bits hold nothing: composed from the devices each time the
     * register is read, they can never latch. The error bits above them are
     * different — an error is a past event a driver reads after the fact, so
     * those do latch. The register is therefore composed (LIVE) rather than
     * stored, ORing the live pins with the one latched error this model raises,
     * the invalid-address bit (held in int_error, cleared through the reset
     * register); the rest stay zero.
     */
    [SGI_BRIDGE_SLOT(BRIDGE_INT_STATUS)] = {
        "interrupt status", SGI_BRIDGE_LIVE, 0, 0 },

    [SGI_BRIDGE_SLOT(BRIDGE_INT_ENABLE)] = {
        "interrupt enable", SGI_BRIDGE_RW, 0, INT_ENABLE_WRITABLE },

    /*
     * Writing a one to a group bit clears every error bit in that group. The
     * one error this model raises is the invalid-address bit, so a group write
     * clears int_error (the write handler does it). The register itself is not
     * storage: one that read back the ones a guest wrote would say the groups
     * were still being cleared. Linux writes all seven at start of day.
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

    s->device_window_base[slot] = base;
    memory_region_set_enabled(&s->device_window_mem[slot], memory);
    memory_region_set_enabled(&s->device_window_io[slot], !memory);
}

/*
 * Every slot's window, re-aimed from its own register. Reset uses this to
 * establish the windows' initial state from the registers it has just set to
 * their reset values, and a post-migration load uses it for the same reason
 * with a different source: `device_window_base` and each window pair's
 * enabled state are both derived from the per-slot register, not independent
 * facts, so nothing about them needs to travel in the migration stream in its
 * own right — only the register does, and it already does
 * (`VMSTATE_UINT32_ARRAY(reg, ...)`). Deriving both call sites from one
 * function is what keeps that true; two copies of this loop would be two
 * places the derivation could go out of step with each other.
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
     * A masked read-modify-write, the same shape a guest's own write to this
     * register gets in sgi_bridge_write, and for the same reason: the base
     * and space-select fields are not the whole register, and a guest
     * writing them leaves the error-lock and page-cross-check bits alone.
     * Firmware aiming a window is a guest action too, and a plain overwrite
     * here would silently clear those two bits with nothing to notice —
     * exactly the register-write bug that rule exists to catch.
     */
    *held = (*held & ~assigned) | (base | (memory_space ? DEV_MEMORY_SPACE
                                                        : 0));
    sgi_bridge_aim_window(s, slot);
}

/*
 * Whether this slot's device needs its byte lanes corrected.
 *
 * TWO CALLERS, AND THEY NEED IT FOR DIFFERENT REASONS. The programmed-access
 * windows ask because the specification's byte-lane tables put a narrow
 * processor access at a reversed offset within its word. The DMA path asks
 * because the IOC3 is a big-endian PCI device and both public drivers clear
 * its DMA swap bits, so reading those bits literally would have this model
 * reverse every Ethernet frame the part moves. The keying below, and the
 * OpenBSD precedent for it, are common to both; the byte-lane tables cited
 * further down are the programmed-access half of the story.
 *
 * Real Bridge hardware has exactly one mechanism for this question: the
 * per-device DEV_SWAP_PROGRAMMED bit (Table 36 of the Bridge ASIC
 * specification), which the chip resets disabled and which neither guest
 * this fork boots ever sets — confirmed by grep on both `pci-ip27.c`'s
 * `bridge_probe()` and `xbridge.c`'s device bring-up, for every device
 * either guest attaches. With that bit disabled, the specification's own
 * byte-lane tables (Appendix A, Tables 65 and 73) show a narrow transfer's
 * bytes land reversed within their 32-bit word — not passed through
 * unmodified — which is what the correction below undoes.
 *
 * What this function decides is therefore not a hardware fact: real
 * silicon has no notion of PCI vendor or device ID, and nothing here claims
 * it does. It is a model-level compatibility policy, and the guest-facing
 * reason it is correct is that OpenBSD's own driver author already had to
 * answer the identical question with the identical hardware signal (none)
 * to ask, and answered it by device identity —
 * `xbridge_probe_device_hook()` exempts IOC3, IOC4 and RAD1 by PCI vendor
 * and device ID and leaves every other device to correct itself in
 * software. This model exempts the same way, checked against the PCI
 * configuration space every device on this bus already carries, rather
 * than against a slot number, which would silently go stale the moment a
 * device moved slots, or against this model's own declared endianness,
 * which is a QEMU-internal detail that happens to correlate with the real
 * fact today and is not the same fact.
 *
 * IOC4 and RAD1 are not modelled in this fork and do not exist on an
 * Origin 200, so the exemption checked here is IOC3 alone — sized for what
 * a device in this machine can actually be, not padded for parts it does
 * not have. An unrecognised device, or an empty slot, is corrected: that
 * is the same default OpenBSD's own else-branch gives a device it does not
 * name, and it means a future device added to this Bridge's window is
 * corrected unless a later change adds it to the exemption list for a
 * stated, sourced reason, rather than silently inheriting a gap.
 */
static bool sgi_bridge_slot_needs_correction(SGIBridgeState *s,
                                             unsigned int slot)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    PCIDevice *dev = phb->bus ? pci_find_device(phb->bus, 0,
                                                PCI_DEVFN(slot, 0)) : NULL;

    if (!dev) {
        return true;
    }
    return !(pci_get_word(dev->config + PCI_VENDOR_ID) ==
             SGI_IOC3_VENDOR_ID &&
             pci_get_word(dev->config + PCI_DEVICE_ID) ==
             SGI_IOC3_DEVICE_ID);
}

/*
 * The byte-lane correction itself. Only the low two bits of a window offset
 * move — a transfer never crosses the 32-bit word its lanes are defined
 * within, which is what the specification's own tables show and why
 * `sgi_bridge_config_address()` above applies the identical arithmetic to
 * configuration space for the identical reason.
 */
static hwaddr sgi_bridge_window_correct(hwaddr offset, unsigned int size)
{
    return (offset & ~3u) | ((offset & 3u) ^ (4u - size));
}

/*
 * Whether the device actually answering a corrected access declares an
 * endianness that `address_space_read`/`write` would silently value-swap
 * against, on top of the address correction above.
 *
 * `address_space_read`/`write` always dispatch to whatever they reach with
 * a bare `size_memop()` — no endianness bit of its own, confirmed directly
 * in `flatview_read_continue_step`/`write_continue_step`
 * (`system/physmem.c`) — and QEMU's `adjust_endianness` then compares that
 * bare op against the target region's own declared endianness and swaps
 * the value whenever they disagree. For a region declared
 * `DEVICE_LITTLE_ENDIAN` on this fork's little-endian-host build tools run
 * on, the bare op already agrees (`MO_LE` is bit pattern zero on a
 * little-endian host) and nothing swaps — which is why the ISP1040
 * mailbox, declared exactly that, needed no correction here beyond the
 * address one. A region declared `DEVICE_BIG_ENDIAN` — IOC3's own blocks —
 * disagrees with that same bare op and gets swapped once, silently, which
 * this window's own address_space_read/write call would otherwise pass
 * straight through uncorrected. This function reads the resolved target's
 * own declaration and says whether that swap happened, so the value can be
 * undone the same way the address already is.
 */
static bool sgi_bridge_leaf_value_was_swapped(AddressSpace *space,
                                              hwaddr addr, unsigned int size,
                                              bool is_write)
{
    hwaddr xlat = addr;
    hwaddr len = size;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    MemoryRegion *leaf;
    enum device_endian end;
    bool leaf_big_endian;

    RCU_READ_LOCK_GUARD();
    leaf = address_space_translate(space, addr, &xlat, &len, is_write, attrs);
    if (!leaf || !leaf->ops) {
        return false;
    }
    end = leaf->ops->endianness;
    leaf_big_endian = end == DEVICE_NATIVE_ENDIAN ? target_big_endian() :
                      end == DEVICE_BIG_ENDIAN;
    return leaf_big_endian != (bool)HOST_BIG_ENDIAN;
}

/*
 * A device-window access, corrected if this slot needs it, then delivered
 * to whichever PCI space (memory or I/O) the window looks into.
 *
 * `pci_mmio`/`pci_io` are containers with no operations of their own — a
 * PCI device's BAR mapping lives in a subregion added under them, found
 * only by walking a real address space's flat view — so reaching one takes
 * `address_space_read`/`write` on `pci_mmio_as`/`pci_io_as`, not a dispatch
 * call directly against the container.
 *
 * Those two move raw bytes and apply no endianness conversion of their own
 * to the *address-corrected* transfer, which is what makes them the right
 * tool for the address half of this window — qtest's own `qtest_memread`
 * is the same call for the same reason. The buffer holds the value this
 * host's own C compiler would give a `uint64_t` variable of that value —
 * its byte order is the host's — decoded and encoded as such below,
 * because that is what a `.read`/`.write` callback's own value looks like
 * as bytes on *this machine running QEMU*. What that raw decode does not
 * see is the swap `adjust_endianness` already applied *inside* the
 * dispatch this window's own call triggers, whenever the device actually
 * answering declares a fixed endianness that disagrees with the bare op
 * `address_space_read`/`write` always use — `sgi_bridge_leaf_value_was_
 * swapped` undoes exactly that, so the value crossing this window is
 * reinterpreted no more than it would be crossing a transparent alias.
 */
static uint64_t sgi_bridge_window_read(SGIBridgeState *s, unsigned int slot,
                                       AddressSpace *space, hwaddr addr,
                                       unsigned int size)
{
    hwaddr offset = sgi_bridge_slot_needs_correction(s, slot) ?
                    sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = s->device_window_base[slot] + offset;
    uint8_t bytes[4] = { 0 };
    uint64_t val;

    /*
     * Both window regions declare an implementation maximum of four bytes, so
     * the memory system splits anything wider before it arrives and this
     * buffer is exactly the right size. The compiler cannot see that cap, and
     * the size-dispatching load below expands to include its eight-byte case,
     * so at any optimisation level above none the array-bounds check reports a
     * read partly outside the buffer. Stating the bound here is what makes it
     * visible. Widening the buffer would silence the same message by making a
     * real overflow possible if those declarations ever changed.
     */
    assert(size <= sizeof(bytes));

    address_space_read(space, target, MEMTXATTRS_UNSPECIFIED, bytes, size);
#if HOST_BIG_ENDIAN
    val = ldn_be_p(bytes, size);
#else
    val = ldn_le_p(bytes, size);
#endif
    if (sgi_bridge_leaf_value_was_swapped(space, target, size, false)) {
        val = bswap64(val) >> (64 - size * 8);
    }
    return val;
}

static void sgi_bridge_window_write(SGIBridgeState *s, unsigned int slot,
                                    AddressSpace *space, hwaddr addr,
                                    uint64_t val, unsigned int size)
{
    hwaddr offset = sgi_bridge_slot_needs_correction(s, slot) ?
                    sgi_bridge_window_correct(addr, size) : addr;
    hwaddr target = s->device_window_base[slot] + offset;
    uint8_t bytes[4];

    /* The same four-byte implementation cap the read path relies on. */
    assert(size <= sizeof(bytes));

    if (sgi_bridge_leaf_value_was_swapped(space, target, size, true)) {
        val = bswap64(val) >> (64 - size * 8);
    }
#if HOST_BIG_ENDIAN
    stn_be_p(bytes, size, val);
#else
    stn_le_p(bytes, size, val);
#endif
    address_space_write(space, target, MEMTXATTRS_UNSPECIFIED, bytes, size);
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
 * A device window takes accesses of one, two, four and eight bytes. The whole
 * Bridge widget space is doubleword-addressed by crosstalk, so a processor
 * doubleword (8-byte) load or store is a native access here, not only the
 * narrower forms — the firmware's own device-pointer dereference is one, and
 * a model capping the width at four rejects it with a bus error. The Bridge
 * serves a doubleword to a 32-bit device as two 32-bit cycles at the two word
 * offsets, so declaring valid up to eight while leaving impl at four makes the
 * memory system split a doubleword into exactly those two four-byte device
 * accesses (big-endian: the high word at the lower offset). The narrower widths
 * still arrive unreshaped, and each split half is a four-byte access in its own
 * right, so the byte-lane correction — which takes the width as a term — is
 * applied per cycle by the same rule and never sees a reshaped width.
 */
static const MemoryRegionOps sgi_bridge_window_mem_ops = {
    .read = sgi_bridge_window_mem_read,
    .write = sgi_bridge_window_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps sgi_bridge_window_io_ops = {
    .read = sgi_bridge_window_io_read,
    .write = sgi_bridge_window_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

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
 * The chip defines the first 0x300 bytes of a 64 KB register block and decodes
 * the whole block. A defined register answers normally; an access above 0x300,
 * to a slot with no register, or to the wrong half of a slot is an undefined
 * address, which the real Bridge does not refuse with a bus error — it posts
 * the access and flags it asynchronously as an invalid-address error. So every
 * access within the block's width is accepted here, and the read and write
 * handlers below tell a defined register from an undefined address and raise
 * the error for the latter. (This is why the block bus-errored undefined
 * addresses once: a firmware self-test provokes one deliberately and fails on
 * any synchronous processor exception, so posting is what lets it pass.)
 */
static bool sgi_bridge_accepts(void *opaque, hwaddr addr, unsigned size,
                               bool is_write, MemTxAttrs attrs)
{
    return true;
}

/*
 * Post an access to an undefined address in the register window and record the
 * invalid-address error, rather than raising a synchronous bus error. The
 * failing access completes; the error is reported asynchronously through the
 * interrupt status register's bit 24, which a guest polls or takes an interrupt
 * on. Still logged, because a genuine firmware probe of an unused slot and a
 * gap in this model's register coverage look alike from here and the log is
 * where they are told apart.
 */
static void sgi_bridge_invalid_address(SGIBridgeState *s, hwaddr addr,
                                       bool is_write)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-bridge: %s of undefined register 0x%" HWADDR_PRIx
                  "; posting it and flagging an invalid-address error\n",
                  is_write ? "write" : "read", addr);
    s->int_error |= INT_INVALID_ADDR;
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
         * The device pins as they are now, composed live so the register
         * follows the level and never latches, ORed with the latched error
         * bits. The one error this model raises is the invalid-address bit,
         * set when a guest touches an undefined register-window address and
         * held until the reset-interrupt-status register clears it.
         */
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
 * Run one bit slot on the one-wire bus and leave its answer in the register.
 *
 * The model completes the slot inside the write that starts it, so a guest
 * polling for the finished flag sees it set on its first read. Nothing here
 * reproduces the timings: they are read only to tell the three kinds of slot
 * apart, because a guest that waited for real microseconds would be waiting on
 * a wire this machine does not have.
 *
 * The wire is open-drain and idles high, so a slot samples one unless the part
 * pulls it down. The board identity part answers a reset by doing exactly
 * that, which is how a guest learns something is there; a guest reads a zero
 * and treats it as presence.
 */
/*
 * The check the guest runs over what it sent and what came back. Reflected
 * polynomial, starting from zero — and **the part sends the result inverted**,
 * which is the part of this that cannot be guessed: the guest does not compare
 * against a value, it runs the check across the whole exchange including the
 * two bytes the part appended and requires a fixed residue. Sending the check
 * uninverted lands on a different residue and the guest rejects the page.
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

/*
 * Assemble the standard one-wire ROM identifier: family, six serial bytes,
 * then the Dallas/Maxim CRC-8 over those seven bytes. This is the same public
 * one-wire algorithm used by the IOC3 identity part, kept local because these
 * two devices otherwise share no bus implementation or state.
 */
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
 * What the identity part holds. Two guests read these same sixty-four bytes
 * and want different things from them, and the layout below is what satisfies
 * both at once.
 *
 * One takes the bytes at offsets eleven through twenty-nine and thirty-two
 * through thirty-seven positionally, drops the spaces, and reads the result as
 * the board's part number — which selects the routine that decides where this
 * board's second interrupt lands. It compares only the leading part number, so
 * anything after that prefix is free.
 *
 * The other scans the whole page as text for named fields written as a name, a
 * colon, a value and a semicolon, and takes the part number from the field
 * named for it. A page carrying a bare number satisfies the first guest and is
 * invisible to the second, which is what this page held before: the name it
 * searches for was absent, its lookup returned empty, and an empty answer is
 * that classifier's default rather than its error.
 *
 * The two are reconciled by arithmetic rather than by compromise. The field
 * name is five bytes, so placing it at offsets six through ten puts its value
 * at offset eleven — exactly where the positional read begins. One string,
 * both readers, neither degraded.
 *
 * The part number is what a real Origin 200 reports, traced through the
 * firmware that reads it, rather than a number chosen to produce a wanted
 * mapping. The difference matters even though both would work: this one stays
 * right if the table it would otherwise have been reverse-engineered against
 * changes.
 *
 * Byte five is a semicolon and it is LOAD-BEARING rather than padding. The
 * scanning reader compares at position zero, and on a miss skips past the next
 * semicolon and compares again. With no semicolon at five, that first miss
 * skips past the terminator at twenty-three instead, lands at twenty-four, and
 * never tries offset six at all. A field name sitting correctly at six is then
 * never reached, and the lookup returns the same empty answer a page carrying
 * no field name gives. One byte is the difference between this page working and
 * only looking as though it does.
 *
 * Byte zero is the vendor manufacturing-data format. The PROM rejects page A
 * before constructing any fields unless this byte is one. Bytes one to four
 * are otherwise inert spaces: the positional reader drops them, and the
 * scanning reader treats them as neither a field name nor a terminator.
 */
static const char sgi_bridge_onewire_image[NIC_PAGE_SIZE * NIC_PAGES] = {
    [0]  = 0x01, ' ', ' ', ' ', ' ', ';',
    [6]  = 'P', 'a', 'r', 't', ':',
    [11] = '0', '3', '0', '-', '1', '2', '4', '4', '-', '0', '0', '1',
    [23] = ';', ' ', ' ', ' ', ' ', ' ', ' ',
    [32] = ' ', ' ', ' ', ' ', ' ', ' ',
};

/*
 * What this part actually holds, assembled from the programmed text and the
 * check each page carries in its own last two bytes.
 *
 * The check has to live here rather than be added to an answer, because two
 * different commands read this memory with different framing and both must
 * return the same bytes for a page. It was produced on the way out of the
 * extended read before, which was right for that command and would have sent a
 * plain read the raw text with zeros where the check belongs.
 *
 * Everything past the programmed pages reads as unprogrammed, which is what an
 * EPROM that was never written there says.
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
 * Start a plain read, which sends stored bytes from the address asked for and
 * keeps going to the end of the data memory. The check comes only at that end,
 * over the command, both address bytes and every byte sent, so it is worked
 * out here in one pass rather than carried along a slot at a time. A guest
 * that resets before the end never sees it, which is the ordinary case: the
 * part number sits in the first two pages and nobody reads two kilobytes to
 * find it.
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
 * Fill in one status page's worth of answer: the bytes themselves, then the
 * check pair the part appends when it reaches the end of the page.
 *
 * The check covers the command and both address bytes as well as the data,
 * but only for the first page — the part clears its check generator at each
 * page boundary and the pages after the first are checked over their data
 * alone. That asymmetry is the part's, not a simplification: the status memory
 * can be programmed after the fact, so a check stored with the data could not
 * stay valid, and the part computes one per page as it sends instead.
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

/*
 * Start a status read at the address the guest asked for.
 *
 * An address past the end of the status memory has nothing to send, so the
 * part is left idle and the guest reads ones. That is what it would get from
 * a part that had run off the end of the memory, and is what this part gives
 * for every address it does not implement anyway.
 */
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
 * Assemble everything the guest will read for one page, once it has finished
 * asking. Built in one go rather than a byte at a time because the checks span
 * the request as well as the answer, so none of it is known until the last
 * byte of the request arrives.
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
     * The block carries its own check over its first thirty bytes, stored
     * inverted in the last two, so that a check across the whole thirty-two
     * lands on the fixed residue. This is the check the board-identity reader
     * runs over the stored bytes before it will trust the part number, and it
     * is a different check from the framing pair the read protocol appends
     * below: a block can satisfy the framing and still fail this one, which is
     * exactly what a page of data with no embedded check does. It arrives
     * here already inside the block because it is part of what the part
     * holds: the plain read returns the same bytes with none of this framing
     * around them, so a check this command manufactured would exist for only
     * one of the two ways a guest can ask.
     *
     * The framing check the read protocol sends after the block covers the
     * whole thirty-two bytes, the embedded check included.
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
        /*
         * A reset. The part holds the wire down, which a guest reads as
         * presence, and forgets whatever conversation was in progress: the
         * next thing it expects is a command.
         */
        trace_sgi_bridge_onewire_reset(s->onewire_phase, s->onewire_outbit);
        s->onewire_phase = NIC_PHASE_COMMAND;
        s->onewire_bit = 0;
        s->onewire_command = 0;
        s->onewire_outbit = 0;
        level = false;
    } else if (s->onewire_phase == NIC_PHASE_COMMAND) {
        /*
         * A command arrives least-significant bit first, one slot per bit. The
         * guest is writing, so what it samples is the idle wire; the part is
         * only listening.
         */
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
                /*
                 * The guest is about to name a part by its whole identity. The
                 * part takes all sixty-four bits before it can know whether it
                 * is the one being addressed, so nothing is decided here.
                 */
                s->onewire_match = 0;
                s->onewire_phase = NIC_PHASE_MATCH;
                break;
            case NIC_CMD_SKIP_ROM:
                /*
                 * The guest has one part on this bus and says so rather than
                 * naming it. The part is now selected and the next byte is a
                 * function rather than an address.
                 */
                s->onewire_command = 0;
                s->onewire_phase = NIC_PHASE_FUNCTION;
                break;
            default:
                /*
                 * A command this part does not answer. The wire is open-drain
                 * and idles high, so the guest reads ones for the rest of
                 * the transaction and cannot tell an unimplemented command
                 * from a part that is not there. A silent default here cost
                 * a boot: real firmware addressed this part by name, read
                 * ones back for every byte it then clocked, and nothing
                 * anywhere reported that a command had been dropped.
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
         * A search asks for each bit of the identity three slots at a time:
         * the bit, then its complement, then the guest writes back the one it
         * chose. With a single part on the bus every choice matches what was
         * offered, so the third slot is taken and ignored rather than
         * compared — comparing is what resolves a collision between two parts,
         * and this board has one.
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
         * The identity the guest is naming, least-significant bit first, the
         * same order a search hands it back in. The guest is writing, so what
         * it samples is the idle wire.
         *
         * A part that is not the one named stops answering until the next
         * reset, which is how a bus with several parts on it leaves exactly one
         * selected. On this bus there is one part, so a mismatch means the
         * guest named something that is not here — and it reads back the same
         * silence a part that is absent would give, which is why the mismatch
         * is reported rather than left to look like nothing happened.
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
         * Stored bytes, one after another, with nothing between them: no
         * redirection byte and no per-page check, because this command frames
         * nothing. The two bytes after the end of the data memory are the
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
        /*
         * The status memory goes out a page at a time, each page followed by
         * its own check pair, and the part carries on into the next page for
         * as long as the guest keeps reading. Running off the end leaves it
         * idle, which the guest reads as ones.
         */
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
        /*
         * Shifting the answer back, least-significant bit of each byte first.
         * The guest reads with the same short slot it uses to write a one, so
         * there is nothing to tell apart here: whatever it does, the part is
         * sending.
         */
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
         * A strobe. Most ask this model to release something it never held, so
         * accepting the write and doing nothing is the whole of the behaviour.
         * The one with an effect is the reset-interrupt-status register: a one
         * in a group bit clears that group's latched errors, and the only one
         * this model latches is the invalid-address bit.
         */
        if (addr - SGI_BRIDGE_REG_DATA == BRIDGE_INT_RESET &&
            ((uint32_t)val & INT_RESET_GROUPS)) {
            s->int_error = 0;
        }
        return;
    case SGI_BRIDGE_RW:
        *held = (*held & ~reg->writable) | ((uint32_t)val & reg->writable);
        if (sgi_bridge_is_device_register(addr, &slot)) {
            /*
             * This model exempts the IOC3 from the DMA byte-lane permutation
             * on the grounds that it is a big-endian part, and both public
             * drivers clear its swap bits to say the same thing. A guest that
             * sets them instead is asking for something the model does not
             * reproduce -- so say so here, where the guest's own write is in
             * hand, rather than in the predicate, whose exemption clause
             * returns before the bits are ever read.
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
 * The external-SSRAM window. This board fits no external RAM, so every access
 * completes but the reads float high. The PROM sizes the RAM with an unguarded
 * probe — it writes a pattern and reads it back, expecting a completing access,
 * not a bus error — and a window that returned the written pattern would make
 * it believe RAM is present. All-ones is the natural float of an undriven bus
 * and differs from any probe pattern, so the size probes as zero. Writes are
 * dropped. Crosstalk addresses this space in doublewords, so accepts up to
 * eight bytes.
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
 * A slot's four legacy interrupt pins fold onto two of the chip's eight in a
 * fixed pattern: pins A and C reach the pin numbered as the slot, and pins B
 * and D reach that number with bit two flipped. Eight slots of four pins each
 * therefore land on eight chip pins with each used twice, which is the whole
 * point — the chip has eight and the bus offers thirty-two.
 *
 * **Neither guest reads this from the chip; both compute it.** Linux folds the
 * five legal pin values through a switch onto a two-entry table it fills with
 * the slot number and the slot number exclusive-ored with four, and OpenBSD
 * writes the same arithmetic directly as a test on the pin's low bit. Two
 * drivers with no shared code agreeing on the arithmetic is what makes this a
 * determined fact rather than a convention one of them invented, and it is why
 * a model may implement it rather than reporting it as board wiring.
 *
 * The low-bit test looks inverted between the two and is not: OpenBSD numbers
 * the pins from one, so its odd pins are A and C, and this interface numbers
 * them from zero, so its even ones are.
 *
 * **This does not cover the base I/O controller's second interrupt**, which
 * does not travel as a PCI interrupt at all — the machine wires it straight
 * to the chip pin the interrupt chain ratifies. That routing is the guest's to
 * compute and the machine's to wire, and nothing here should reproduce it.
 */
static int sgi_bridge_map_irq(PCIDevice *dev, int pin)
{
    int slot = PCI_SLOT(dev->devfn);

    return (pin & 1) ? (slot ^ 4) : slot;
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
 *
 * Concurrency contract: this hook runs under RCU, not necessarily the BQL
 * (see the translate doc in include/system/memory.h). Every DMA initiator
 * on this machine — the IOC3 network device and the two SCSI adapters —
 * reaches it from a BQL-held context, because MMIO dispatch takes the BQL
 * and no device on this machine uses an iothread. The translation state
 * read here, the ATE array plus the CONTROL and DIRECT_MAP registers, is
 * only coherent under that assumption. A future BQL-free DMA path needs a
 * full translation-state synchronization design — atomics on every read
 * this hook makes, plus reset and migration — not spot fixes here.
 */
/*
 * Whether this model must permute byte lanes for a transfer this device is
 * making to this PCI address.
 *
 * Note the inversion, which is the whole reason this reads backwards. The
 * chip's swap bits enable a swapper that turns the crossed wiring between a
 * big-endian crosstalk side and a little-endian PCI side into straight-through
 * byte numbering. A byte-addressed model is straight-through already, so it is
 * the *disabled* case that needs work here.
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
 * The permutation itself.
 *
 * THE PAIR OF ENDIANNESSES HERE IS LOAD-BEARING AND LOOKS REDUNDANT. The body
 * must stay `address_space_read`/`write` into a byte buffer: that resolves the
 * answering region's own declared endianness before this code sees the bytes,
 * so the only byte order left in the composition is this region's declaration
 * against the big-endian decode below. Rewriting either half to a value
 * accessor such as `address_space_ldl_be()` reads as a simplification and
 * reintroduces the answering region's endianness as a live term -- at which
 * point a transfer aimed at another widget's registers starts arriving
 * reversed and nothing else changes.
 *
 * These callbacks always target system memory, never the swapping space, so
 * there is no way back into themselves.
 */
static uint64_t sgi_bridge_lane_swap_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    uint8_t bytes[8];

    address_space_read(&address_space_memory,
                       sgi_bridge_window_correct(addr, size),
                       MEMTXATTRS_UNSPECIFIED, bytes, size);
    return ldn_be_p(bytes, size);
}

static void sgi_bridge_lane_swap_write(void *opaque, hwaddr addr, uint64_t val,
                                       unsigned size)
{
    uint8_t bytes[8];

    stn_be_p(bytes, size, val);
    address_space_write(&address_space_memory,
                        sgi_bridge_window_correct(addr, size),
                        MEMTXATTRS_UNSPECIFIED, bytes, size);
}

static const MemoryRegionOps sgi_bridge_lane_swap_ops = {
    .read = sgi_bridge_lane_swap_read,
    .write = sgi_bridge_lane_swap_write,
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
         * Where the access lands is the only lever this interface offers, so
         * it is the one the byte-lane permutation rides in on.
         *
         * THIS CALL TAKES THE RAW PCI ADDRESS AND MUST KEEP DOING SO. Which
         * window an address falls in is what selects between the two swap
         * bits, and the windows are defined on the address the device issued.
         * This model does not yet decode 64-bit direct addresses properly;
         * whoever fixes that will be normalising addresses somewhere around
         * here, and if this call ends up below that normalisation a 64-bit
         * address can land under the direct-window boundary. Then Linux --
         * which sets the direct bit and leaves the page-mapped one clear --
         * gets its buffers judged by the wrong bit and silently byte-flipped,
         * in a commit about address decoding, months from now.
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
 * Every device on this bus translates through the same table, because the chip
 * has one. What is per-device is the attribute lookup: the swap bits that
 * decide whether a transfer's byte lanes are permuted live in the requesting
 * slot's own Device(x) register, so the requester has to be known at
 * translate time rather than carried in the entry.
 */
static AddressSpace *sgi_bridge_device_address_space(PCIBus *bus, void *opaque,
                                                     int devfn)
{
    SGIBridgeState *s = opaque;
    unsigned int slot = PCI_SLOT(devfn);

    /*
     * The chip decodes eight device slots and this machine populates three, so
     * the fold below cannot be reached by anything a guest does -- it exists so
     * that a machine wiring a device outside the chip's slots gets a defined
     * answer rather than reading off the end. If it ever does fire, the device
     * would silently inherit another slot's swap bits, so it is worth a look
     * rather than a shrug.
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

static void sgi_bridge_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = sgi_bridge_translate;
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

    /*
     * The one register whose reset value the table cannot hold, because it is
     * the machine's statement of what this chip is rather than a constant of
     * the chip. It is composed here so that a read of it takes the same path as
     * every other register.
     */
    s->reg[SGI_BRIDGE_SLOT(BRIDGE_IDENTIFICATION)] =
        sgi_bridge_identification(s);

    s->pin_level = 0;
    s->int_error = 0;

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
    sgi_bridge_aim_all_windows(s);
}

static void sgi_bridge_realize(DeviceState *dev, Error **errp)
{
    SGIBridgeState *s = SGI_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    Object *obj = OBJECT(dev);
    unsigned int slot;

    if (s->onewire_serial & ~NIC_ROM_SERIAL_MASK) {
        error_setg(errp, "the I/O bridge one-wire serial 0x%" PRIx64
                   " does not fit in 48 bits", s->onewire_serial);
        return;
    }
    s->onewire_identity = sgi_bridge_onewire_rom_id(s->onewire_serial);

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
    memory_region_init_io(&s->reserved_data_half, obj,
                          &sgi_bridge_reserved_data_half_ops, s,
                          "sgi-bridge.reserved-data-half", 4);
    memory_region_add_subregion_overlap(&s->widget, BRIDGE_RESERVED_DATA_HALF,
                                        &s->reserved_data_half, 1);
    memory_region_init_io(&s->ssram, obj, &sgi_bridge_ssram_ops, s,
                          "sgi-bridge.ssram", BRIDGE_SSRAM_SIZE);
    memory_region_add_subregion(&s->widget, BRIDGE_SSRAM_BASE, &s->ssram);
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
    address_space_init(&s->pci_mmio_as, &s->pci_mmio, "sgi-bridge.pci-mmio");
    address_space_init(&s->pci_io_as, &s->pci_io, "sgi-bridge.pci-io");

    /*
     * The eight slot windows, two regions each looking into one of those two
     * spaces. Both go in at the slot's place in the widget's address space
     * and reset decides which of the pair answers.
     *
     * Each is a plain I/O region rather than an alias, because an alias has
     * nowhere to carry the byte-lane correction a narrow access needs: it
     * offers only an offset, retargeted by sgi_bridge_aim_window(), with no
     * hook to look at the access width or which device the slot holds. The
     * per-window context below is what a callback uses to recover both.
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
     * The same eight pins, reachable directly rather than only through a PCI
     * function's own INTx line. Real board wiring puts more than PCI devices
     * on this chip's interrupt pins — the base I/O controller's second
     * interrupt is the case this exists for — and sgi_bridge_set_irq() above
     * already is the entire mechanism a pin needs: it does not care whether a
     * transition arrived from the PCI core's own routing or from a plain
     * wire, so there is nothing to duplicate here, only a second way in.
     */
    qdev_init_gpio_in_named_with_opaque(dev, sgi_bridge_set_irq, s, NULL,
                                        SGI_BRIDGE_PINS);

    /*
     * The configuration window goes in after the bus exists, because reading it
     * is reading the bus.
     */
    memory_region_init_io(&s->cfg0, obj, &sgi_bridge_config_ops, s,
                          "sgi-bridge.cfg0", BRIDGE_CFG0_SIZE);
    memory_region_add_subregion(&s->widget, SGI_BRIDGE_CFG0_BASE, &s->cfg0);

    /*
     * Where a transfer whose byte lanes need permuting is sent. One region for
     * the whole chip: the decision is per device but the action is not, so
     * eight copies of it would differ in nothing.
     */
    memory_region_init_io(&s->lane_swap, obj, &sgi_bridge_lane_swap_ops, s,
                          "sgi-bridge.lane-swap", UINT64_MAX);
    /*
     * QEMU's re-entrancy guard is per owning device, and this chip owns both
     * ends of an ordinary transfer: the programmed-access window a guest
     * writes a device's registers through, and the region that device's DMA
     * then lands in. So a command issued the normal way -- guest writes a
     * mailbox through the Bridge window, adapter fetches its descriptor --
     * finds the Bridge already engaged and has the DMA blocked, returning
     * zeroes with only a warning. That is not the recursion the guard exists
     * to catch: these callbacks target system memory and never this address
     * space, so there is no path back into themselves. The guard is measuring
     * ownership, not recursion, and it is wrong here.
     *
     * What this does not give up: the flag is per region, so every region a
     * transfer reaches THROUGH this one still applies its own guard one level
     * down. A DMA aimed at another device's registers is protected there, by
     * that device. Only this region's ownership test is waived.
     */
    s->lane_swap.disable_reentrancy_guard = true;
    address_space_init(&s->lane_swap_as, &s->lane_swap,
                       "sgi-bridge.lane-swap");

    /*
     * The translation every device on this bus transfers through, and the
     * address space each is given in place of the system's. One per slot,
     * because the swap bits that steer a transfer belong to the slot that
     * issued it and a shared region cannot tell which slot that was.
     */
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
 * Re-aim every window after a migration or snapshot load. `device_window_base`
 * and each window pair's enabled state are derived from the per-slot register
 * (sgi_bridge_aim_window(), above) rather than independent facts, and the
 * register is what the migration stream actually carries
 * (`VMSTATE_UINT32_ARRAY(reg, ...)` below) — so post_load's job is exactly
 * reset's, run against restored register values instead of their reset ones.
 * Without this, a guest that resumes after a load reaches whatever slot the
 * windows happened to be aimed at when the destination's device was created
 * rather than where the guest itself last programmed them.
 */
static int sgi_bridge_post_load(void *opaque, int version_id)
{
    SGIBridgeState *s = opaque;

    sgi_bridge_aim_all_windows(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_bridge = {
    .name = "sgi-bridge",
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = sgi_bridge_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIBridgeState, SGI_BRIDGE_REG_SLOTS),
        VMSTATE_UINT8(pin_level, SGIBridgeState),
        VMSTATE_UINT32_V(int_error, SGIBridgeState, 4),
        VMSTATE_UINT64_ARRAY_V(ate_entry, SGIBridgeState,
                               SGI_BRIDGE_ATE_ENTRIES, 2),
        /*
         * The conversation with the board identity part. A guest drives it
         * one bit per register write, so all of this lives between accesses
         * and none of it is recoverable from any register.
         *
         * What is deliberately absent is the stored image itself: it is
         * assembled from constant data at reset, which the destination has
         * already done before it takes anything in, so migrating it would
         * send bytes the other end can only reproduce identically.
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

/*
 * The identities a real Bridge and its board answer with. They are properties
 * rather than constants because the chip revision and board serial belong to
 * the machine that fitted them, not to every instance of the device.
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
    DEFINE_PROP_UINT64("onewire-serial", SGIBridgeState, onewire_serial,
                       NIC_ROM_SERIAL_DEFAULT),
    DEFINE_PROP_STRING("pci-bus-name", SGIBridgeState, pci_bus_name),
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
