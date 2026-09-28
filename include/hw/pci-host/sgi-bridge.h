/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI Bridge register and host bridge definitions. */
/*
 * One side of the chip is an ordinary crosstalk widget with the common widget
 * register block at the bottom of its 16 MB of crosstalk address space. The
 * other side is a PCI host bridge with eight device slots, an eight-pin
 * interrupt controller and a translator for transfers a device initiates.
 *
 * On an Origin 200 this is the only path from the crossbar to any peripheral,
 * so nothing a guest does past its first console banner avoids it.
 */

#ifndef HW_PCI_HOST_SGI_BRIDGE_H
#define HW_PCI_HOST_SGI_BRIDGE_H

#include "qemu/units.h"
#include "hw/1wire/onewire.h"
#include "hw/pci/pci_host.h"
#include "qom/object.h"

#define TYPE_SGI_BRIDGE "sgi-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBridgeState, SGI_BRIDGE)

/*
 * Returns the fragment naming what is wrong with board_part, or NULL when
 * it is fine. Callers own the device-specific prefix, the way
 * hw/pci-host/sgi-bridge.c's own sgi_bridge_record_field_error() leaves its
 * device and field names to its callers: sgi-kona shares this check
 * (hw/misc/sgi-kona.c) and is not a bridge, so a message hard-coding "the
 * I/O bridge" would misname the device that actually rejected it.
 */
static inline const char *sgi_bridge_board_part_error(const char *board_part)
{
    if (!board_part) {
        return "board-part property is required";
    }
    if (strlen(board_part) != 12 || strchr(board_part, ';')) {
        return "board part must be 12 characters without a semicolon";
    }
    return NULL;
}

/*
 * The text fields of one board-identity record, stored at fixed byte offsets
 * in the part's first two pages. `part` is the same twelve-character board
 * part the "board-part" property takes; its first nine characters are the
 * base number and its last three the dash number. The other fields are
 * space-padded to their widths: ten for the barcode, four for the revision
 * and fourteen for the board name.
 */
typedef struct SGIBridgeBoardRecord {
    const char *barcode;
    const char *part;
    const char *revision;
    const char *name;
} SGIBridgeBoardRecord;

/* Two 32-byte pages: sgi_bridge_onewire_build_record()'s own record size. */
#define SGI_BRIDGE_ONEWIRE_RECORD_SIZE 64

/*
 * Build one record's bytes the fixed-offset way, CRC-16 framing included, so
 * a board with its own one-wire bus can carry the same record layout without
 * a second copy of it. The InfiniteReality geometry and Ktown records in
 * hw/misc/sgi-kona.c reuse this layout. `record`'s fields
 * must already satisfy sgi_bridge_board_part_error()'s twelve-character part
 * shape and this layout's field widths (barcode ten characters, revision
 * four, name fourteen); this function does not validate them itself, matching
 * the one call site in sgi-bridge.c, which checks first.
 */
void sgi_bridge_onewire_build_record(
    uint8_t rom[SGI_BRIDGE_ONEWIRE_RECORD_SIZE],
    const SGIBridgeBoardRecord *record);

/*
 * Aim a slot's window the way a guest write to BRIDGE_DEVICE(n) would, so
 * the machine's firmware emulation to arm the console slot before any guest
 * code exists to do it.
 */
void sgi_bridge_assign_window(SGIBridgeState *s, unsigned int slot,
                              hwaddr pci_base, bool memory_space);

/*
 * Add a second board-identity record, stored as `record` gives it, to this
 * Bridge's one-wire bus, for a daughtercard fitted behind the same chip: the
 * bus is architecturally multi-drop, and a guest one-wire search finds every
 * part on it, not only the chip's own board record realize() already
 * attaches. `bridge` takes the generic qdev pointer a carrier-agnostic caller
 * already has (an sgi-baseio preset reaches its host through
 * baseio_bus_host(), which stays decoupled from any one host-bridge type);
 * the Bridge must already be realized, since this attaches directly to its
 * onewire_bus. `serial` must differ from the board's own onewire-serial
 * property, or a guest one-wire search cannot tell the two records apart.
 */
bool sgi_bridge_add_onewire_board_part(DeviceState *bridge,
                                       const SGIBridgeBoardRecord *record,
                                       uint64_t serial, Error **errp);

/*
 * The bridge's plain DMA route for a device on its bus, or NULL when the
 * device is not behind an SGI Bridge. Transfers move through this with
 * dma_memory_read/write: the same address decode and node-qualified fabric
 * routing as the slot's own space, with the byte-lane swap alone left out.
 * Which transfers belong here is the device model's call, made from what its
 * card is programmed to do: a leg whose two swaps compose to the identity (a
 * frame payload everywhere; every leg on a card that swaps descriptors too)
 * moves here rather than paying a per-word dispatch to produce bytes a plain
 * copy gives for free, while a leg whose single swap is observable stays on
 * pci_dma_*, which honours the slot's programmed correction.
 */
AddressSpace *sgi_bridge_plain_dma_as(PCIDevice *dev);

/*
 * What a device plugged into this bridge says about itself: "my transfers
 * already carry the guest's byte order, so hand me bytes uncorrected". A
 * device that models its own big-endian view of guest memory (as SGI's own
 * silicon does) must not have this chip's byte lanes applied on top, on
 * either the DMA path or its register window. The declaration says nothing
 * about PCI identity and nothing about what the guest programmed; a device
 * that does not make it is corrected, which is the safe default for an
 * ordinary little-endian card.
 */
#define SGI_BRIDGE_BYTE_LANE_SELF_ORDERED "byte-lane-self-ordered"

/*
 * The whole of the widget's crosstalk space belongs to this chip. Sized to
 * reach the PIO translated window below, not just the low CSR/cfg0/ssram/
 * small-window region.
 */
#define SGI_BRIDGE_WIDGET_SIZE  (2 * GiB)

/*
 * Inbound PIO translated window: a processor-initiated access landing here
 * reaches PCI memory space directly, offset by this base. Confirmed live: a
 * booted IRIX 6.5.30 kernel programs a Hub big window at exactly this widget
 * offset (widget shifted into the crosstalk address, OR'd with slice 2 at 512
 * MB granularity, landing here once the widget-select bits are routed away)
 * when mapping a PCI BAR too large for the per-slot device window, and the
 * resulting pointer's low 30 bits equal the BAR's own PCI address exactly.
 * This is the same 0x40000000-0x7fffffff range the DMA side treats as the
 * ATE-translated window, but for an inbound access the evidence shows a plain
 * offset into PCI memory space, not an ATE lookup. The ATE stores a
 * crosstalk destination, which has no reverse mapping to a PCI address.
 * "Plain offset" describes the address math only: the model still applies the
 * same byte-lane correction as the per-device windows
 * (sgi_bridge_window_correct), since this is the same
 * big-endian-processor-onto-little-endian-PCI-bus path they correct, just
 * reached without a slot's own programmed base. There is no per-device
 * register behind this window, so unlike a per-device window it has no
 * self-ordered exemption. Every access through it is corrected. Only this
 * range is modeled; the adjacent 0x80000000-0xffffffff "direct-mapped" DMA
 * range has no equivalent inbound evidence yet.
 *
 * Tested directly against this window's only current consumer: PsiTech RAD4's
 * 8 MiB BAR2 (framebuffer plus control registers, hw/display/psitech-rad4.c)
 * is too large for a per-slot window and is reachable only through here. IRIX
 * 6.5.30 booted through it renders a real but wrong-colored xdm login screen
 * (correct shapes and layout, wrong colors). A live A/B boot with this
 * window's correction forced off produced byte-identical screendumps
 * because RAD4's BAR2 device declares 4-byte-only access
 * (rad4_bar2_ops, valid/impl min and max both 4), and
 * sgi_bridge_window_correct's offset adjustment is the identity function at
 * size 4 regardless of the correct flag. The color defect is confirmed
 * independent of this window's correction; since it reproduces even though
 * the control-register sequence a working mode-zero recognizer requires
 * already matches byte-for-byte through this same window, the remaining
 * defect most likely sits in psitech-rad4.c's own pixel-value handling, not
 * here.
 */
#define SGI_BRIDGE_PIO_XLATE_BASE 0x40000000
#define SGI_BRIDGE_PIO_XLATE_SIZE 0x40000000

/* Switched packets use four widget bits above their full 48-bit address. */
#define SGI_BRIDGE_WIDGET_BITS         4
#define SGI_BRIDGE_PACKET_ADDRESS_BITS 48

MemoryRegion *sgi_bridge_packet_face(SGIBridgeState *s);

/*
 * The local register block: 64 KB decoded, the first 0x300 bytes defined, one
 * register per eight-byte slot in data bits 31:0.
 */
#define SGI_BRIDGE_REGS_SIZE    (64 * KiB)
#define SGI_BRIDGE_REG_SLOT     8
#define SGI_BRIDGE_REGS_DEFINED 0x300
#define SGI_BRIDGE_REG_SLOTS    (SGI_BRIDGE_REGS_DEFINED / SGI_BRIDGE_REG_SLOT)

/*
 * Type 0 configuration space, one 4 KB window per device. Public because the
 * machine names configuration addresses too (the console descriptor carries
 * one); same geometry as Linux's asm/pci/bridge.h.
 */
#define SGI_BRIDGE_CFG0_BASE        0x020000
#define SGI_BRIDGE_CFG0_DEVICE_SIZE 0x001000
#define SGI_BRIDGE_CFG0_DEVICE(n)   (SGI_BRIDGE_CFG0_BASE + \
                                     (n) * SGI_BRIDGE_CFG0_DEVICE_SIZE)

/*
 * Eight device slots, which are also the eight interrupt pins: a slot's INTA
 * is wired to the pin of the same number (OpenBSD's rule, confirmed by a real
 * Origin 200's two SCSI controllers).
 */
#define SGI_BRIDGE_DEVICES      8
#define SGI_BRIDGE_PINS         8

/*
 * The internal translation table: 128 eight-byte entries that live in the
 * chip, not in memory. A translation reads a register. External SRAM could
 * extend it; this machine has none.
 */
#define SGI_BRIDGE_ATE_ENTRIES  128

struct SGIBridgeState {
    PCIHostState parent_obj;

    /* The widget's crosstalk space, claimed whole, and the block inside. */
    MemoryRegion widget;
    MemoryRegion packet;
    MemoryRegion packet_io;
    MemoryRegion regs;
    MemoryRegion reserved_data_half;

    /* Empty on this board; completes accesses for the PROM's size probe. */
    MemoryRegion ssram;

    /*
     * The external-flash window: 4 MB at 0xc0_0000, two 2 MB chip selects on
     * a 16-bit port. A board that fits a part links its region as "flash" and
     * it answers the chip select strapped to the base; the window carries a
     * read of any width as 16-bit chip cycles and a write only while the
     * control register's flash write enable is set. The other chip select is
     * unpopulated unless a board links what it fits there as "flash-select1".
     */
    MemoryRegion flash_window;
    MemoryRegion *flash;
    MemoryRegion *flash_select1;

    /* Configuration space window, type 0 only: no PCI-to-PCI bridge here. */
    MemoryRegion cfg0;

    /*
     * The PCI side's two address spaces, which the device windows look into.
     */
    MemoryRegion pci_mmio;
    MemoryRegion pci_io;

    /*
     * Inbound PIO translated window: dedicated ops onto pci_mmio_as at PCI 0,
     * not an alias. It needs the same byte-lane correction as the
     * per-device windows below, which a plain alias cannot carry.
     */
    MemoryRegion pio_xlate;

    /*
     * Address-space views of the two containers above: a window callback
     * needs address_space_read/write to find a BAR subregion, which a bare
     * dispatch against a container with no ops cannot.
     */
    AddressSpace pci_mmio_as;
    AddressSpace pci_io_as;

    /*
     * Two windows per slot at the same address, one per PCI space, exactly
     * one enabled: a region cannot be re-pointed at a different space, so the
     * slot register's space-select bit picks which one answers.
     */
    MemoryRegion device_window_mem[SGI_BRIDGE_DEVICES];
    MemoryRegion device_window_io[SGI_BRIDGE_DEVICES];

    /* Where each window aims: an I/O region cannot hold an alias offset. */
    hwaddr device_window_base[SGI_BRIDGE_DEVICES];

    /*
     * Slots whose device declared itself byte-lane self-ordered, collected as
     * it was plugged so that every access is one mask test rather than a walk
     * of the bus. Not migrated: the destination plugs the same devices into
     * the same slots before any transfer runs.
     */
    uint32_t self_ordered_slots;

    /* Slots forced exempt whatever is plugged in them: a test override. */
    uint32_t byte_lane_exempt;

    /* Per-window context: which slot a callback serves, set at realize. */
    struct SGIBridgeDeviceWindow {
        SGIBridgeState *s;
        unsigned int slot;
    } device_window_mem_ctx[SGI_BRIDGE_DEVICES];
    struct SGIBridgeDeviceWindow device_window_io_ctx[SGI_BRIDGE_DEVICES];

    /*
     * Entries write 64-bit via ate; ate_low is the lower halves' only read.
     */
    MemoryRegion ate;
    MemoryRegion ate_low;

    /* The entries themselves, which are registers in this chip. */
    uint64_t ate_entry[SGI_BRIDGE_ATE_ENTRIES];

    /*
     * Every device transfers through this instead of the system's space. One
     * IOMMU region per slot over one shared table: the swap bits that steer a
     * transfer live in the requesting slot's own Device(x) register, and a
     * shared region cannot tell which slot asked.
     */
    struct SGIBridgeDeviceDMA {
        IOMMUMemoryRegion iommu;
        AddressSpace as;
        SGIBridgeState *s;
        unsigned int slot;
    } dma[SGI_BRIDGE_DEVICES];

    /*
     * The plain route: the same decode, never the byte-lane swap. One for the
     * whole bridge; the swap decision was the only per-slot part.
     */
    IOMMUMemoryRegion plain_iommu;
    AddressSpace plain_dma_as;

    /*
     * Where a lane-permuted transfer is sent: a translation entry can only
     * say where an access lands, so the action lives in the target space.
     */
    MemoryRegion lane_swap;
    AddressSpace lane_swap_as;

    /*
     * The fabric interrupt packets travel: a pin rise is a write to the
     * destination Hub's interrupt-modify register through the machine's own
     * address space, never a direct call. The machine supplies the two
     * constants that compose its addresses.
     */
    MemoryRegion *fabric;
    AddressSpace fabric_as;
    uint64_t fabric_base;
    uint32_t fabric_node_shift;

    /*
     * The fixed four bits above a packet address used for the destination
     * widget. Crosstalk carries that number in a packet header beside the
     * address; a QEMU address space has no header, so a machine with a switch
     * to route through has to make room for it in the address itself. Zero
     * means it makes none, which is the answer for a chip with one peer on
     * the other end of its link.
     */
    uint32_t fabric_widget_shift;

    /* The QOM identity of this Bridge's PCI root bus. */
    char *pci_bus_name;

    /*
     * Every defined register, one word per slot. Decode, reset value and
     * writable mask all come from one table in the implementation, so a
     * register cannot gain a decode without a reset value.
     */
    uint32_t reg[SGI_BRIDGE_REG_SLOTS];

    /*
     * A property: the identity part's serial names the board, not the chip,
     * so the machine supplies it. It does not migrate; the destination
     * constructs the same machine first.
     */
    uint64_t onewire_serial;

    /* A property: the board part stored in the one-wire identity page. */
    char *board_part;

    /*
     * Properties: the rest of the board's identity record. With board-name
     * set, the page is the fixed-offset record and all three are required;
     * unset, the page keeps the older text layout.
     */
    char *board_barcode;
    char *board_revision;
    char *board_name;

    /*
     * The MicroLAN bus. Every Bridge has one and realizes its own
     * board-identity part on it, soldered to the board the chip is on; the
     * bus is architecturally multi-drop, and
     * sgi_bridge_add_onewire_board_part() adds a daughtercard's own record
     * alongside it.
     */
    OneWireBus *onewire_bus;

    /*
     * The status register reports the eight pins' driven levels. It
     * must follow the level, never latch: guests re-arm by re-reading it.
     */
    uint8_t pin_level;

    /*
     * PCI INTx and directly wired GPIO lines are ORed per pin into that
     * status word: one shared word would let the last
     * writer clear a level the other source still holds.
     */
    uint8_t pci_pin_level;
    uint8_t gpio_pin_level;

    /*
     * The status register's latched error bits (31:8); the one this model
     * raises is invalid-address, cleared through reset-interrupt-status.
     */
    uint32_t int_error;

    /* What the identification register answers with. */
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif /* HW_PCI_HOST_SGI_BRIDGE_H */
