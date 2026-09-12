/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Bridge, the crosstalk-to-PCI host bridge.
 *
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

static inline const char *sgi_bridge_board_part_error(const char *board_part)
{
    if (!board_part) {
        return "the I/O bridge board-part property is required";
    }
    if (strlen(board_part) != 12 || strchr(board_part, ';')) {
        return "the I/O bridge board part must be 12 characters without a "
               "semicolon";
    }
    return NULL;
}

/*
 * Aim a slot's window the way a guest write to BRIDGE_DEVICE(n) would --
 * for the machine's firmware emulation to arm the console slot before any
 * guest code exists to do it.
 */
void sgi_bridge_assign_window(SGIBridgeState *s, unsigned int slot,
                              hwaddr pci_base, bool memory_space);

/*
 * The bridge's plain DMA route for a device on its bus, or NULL when the
 * device is not behind an SGI Bridge. Transfers move through this with
 * dma_memory_read/write: the same address decode and node-qualified fabric
 * routing as the slot's own space, with the byte-lane swap alone left out.
 * Which transfers belong here is the device model's call, made from what
 * its card is programmed to do: a leg whose two swaps compose to the
 * identity (a frame payload everywhere; every leg on a card that swaps
 * descriptors too) moves here rather than paying a per-word dispatch to
 * produce bytes a plain copy gives for free, while a leg whose single swap
 * is observable stays on pci_dma_*, which honours the slot's programmed
 * correction.
 */
AddressSpace *sgi_bridge_plain_dma_as(PCIDevice *dev);

/*
 * What a device plugged into this bridge says about itself: "my transfers
 * already carry the guest's byte order, so hand me bytes uncorrected". A
 * device that models its own big-endian view of guest memory -- SGI's own
 * silicon does -- must not have this chip's byte lanes applied on top, on
 * either the DMA path or its register window. The declaration says nothing
 * about PCI identity and nothing about what the guest programmed; a device
 * that does not make it is corrected, which is the safe default for an
 * ordinary little-endian card.
 */
#define SGI_BRIDGE_BYTE_LANE_SELF_ORDERED "byte-lane-self-ordered"

/* The whole of the widget's crosstalk space belongs to this chip. */
#define SGI_BRIDGE_WIDGET_SIZE  (16 * MiB)

/* Switched packets use four widget bits above their full 48-bit address. */
#define SGI_BRIDGE_WIDGET_BITS         4
#define SGI_BRIDGE_PACKET_ADDRESS_BITS 48

MemoryRegion *sgi_bridge_packet_face(SGIBridgeState *s);

/*
 * The local register block: 64 KB decoded, the first 0x300 bytes defined,
 * one register per eight-byte slot in data bits 31:0.
 */
#define SGI_BRIDGE_REGS_SIZE    (64 * KiB)
#define SGI_BRIDGE_REG_SLOT     8
#define SGI_BRIDGE_REGS_DEFINED 0x300
#define SGI_BRIDGE_REG_SLOTS    (SGI_BRIDGE_REGS_DEFINED / SGI_BRIDGE_REG_SLOT)

/*
 * Type 0 configuration space, one 4 KB window per device. Public because
 * the machine names configuration addresses too (the console descriptor
 * carries one); same geometry as Linux's asm/pci/bridge.h.
 */
#define SGI_BRIDGE_CFG0_BASE        0x020000
#define SGI_BRIDGE_CFG0_DEVICE_SIZE 0x001000
#define SGI_BRIDGE_CFG0_DEVICE(n)   (SGI_BRIDGE_CFG0_BASE + \
                                     (n) * SGI_BRIDGE_CFG0_DEVICE_SIZE)

/*
 * Eight device slots, which are also the eight interrupt pins: a slot's
 * INTA is wired to the pin of the same number (OpenBSD's rule, confirmed
 * by a real Origin 200's two SCSI controllers).
 */
#define SGI_BRIDGE_DEVICES      8
#define SGI_BRIDGE_PINS         8

/*
 * The internal translation table: 128 eight-byte entries that live in the
 * chip, not in memory -- a translation reads a register. External SRAM
 * could extend it; this machine has none.
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

    /* Configuration space window, type 0 only: no PCI-to-PCI bridge here. */
    MemoryRegion cfg0;

    /* The PCI side's two address spaces, which the device windows look into. */
    MemoryRegion pci_mmio;
    MemoryRegion pci_io;

    /*
     * Address-space views of the two containers above: a window callback
     * needs address_space_read/write to find a BAR subregion, which a bare
     * dispatch against a container with no ops cannot.
     */
    AddressSpace pci_mmio_as;
    AddressSpace pci_io_as;

    /*
     * Two windows per slot at the same address, one per PCI space, exactly
     * one enabled: a region cannot be re-pointed at a different space, so
     * the slot register's space-select bit picks which one answers.
     */
    MemoryRegion device_window_mem[SGI_BRIDGE_DEVICES];
    MemoryRegion device_window_io[SGI_BRIDGE_DEVICES];

    /* Where each window aims: an I/O region cannot hold an alias offset. */
    hwaddr device_window_base[SGI_BRIDGE_DEVICES];

    /*
     * Slots whose device declared itself byte-lane self-ordered, collected
     * as it was plugged so that every access is one mask test rather than a
     * walk of the bus. Not migrated: the destination plugs the same devices
     * into the same slots before any transfer runs.
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

    /* Entries write 64-bit via ate; ate_low is the lower halves' only read. */
    MemoryRegion ate;
    MemoryRegion ate_low;

    /* The entries themselves, which are registers in this chip. */
    uint64_t ate_entry[SGI_BRIDGE_ATE_ENTRIES];

    /*
     * Every device transfers through this instead of the system's space.
     * One IOMMU region per slot over one shared table: the swap bits that
     * steer a transfer live in the requesting slot's own Device(x)
     * register, and a shared region cannot tell which slot asked.
     */
    struct SGIBridgeDeviceDMA {
        IOMMUMemoryRegion iommu;
        AddressSpace as;
        SGIBridgeState *s;
        unsigned int slot;
    } dma[SGI_BRIDGE_DEVICES];

    /*
     * The plain route: the same decode, never the byte-lane swap. One for
     * the whole bridge -- the swap decision was the only per-slot part.
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
     * The fixed four bits above a packet address used for the
     * destination widget. Crosstalk carries that number in a packet header
     * beside the address; a QEMU address space has no header, so a machine
     * with a switch to route through has to make room for it in the address
     * itself. Zero means it makes none, which is the answer for a chip with
     * one peer on the other end of its link.
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
     * A property: the identity part's serial names the board, not the
     * chip, so the machine supplies it. It does not migrate -- the
     * destination constructs the same machine first.
     */
    uint64_t onewire_serial;

    /* A property: the board part stored in the one-wire identity page. */
    char *board_part;

    /*
     * The MicroLAN bus, carrying the one board identity part. Every
     * Bridge has both; the part is soldered to the board the chip is on.
     */
    OneWireBus *onewire_bus;

    /*
     * The eight pins' driven levels -- what the status register reports. It
     * must follow the level, never latch: guests re-arm by re-reading it.
     */
    uint8_t pin_level;

    /*
     * The two source domains behind that word -- PCI INTx and the directly
     * wired GPIO lines -- ORed per pin: one shared word would let the last
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
