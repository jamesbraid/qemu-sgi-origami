/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * One interrupt, travelling the whole way: a PCI device to the
 * processor.
 *
 * Every piece of this chain has had its own test and the chain has had
 * none.
 * The bridge's registers are tested against the bridge, the Hub's
 * against the
 * Hub, and the processor's lines by a ROM that writes the Hub's
 * registers
 * itself. Each of those proves a link. None of them proves the links
 * are
 * joined, and a machine whose interrupt packet went to the wrong
 * address, or
 * carried the wrong number, or never left the bridge at all, passes
 * every one
 * of them.
 *
 * So this drives the first domino and looks only at the last. A device
 * raises
 * its PCI interrupt pin; the bridge turns that rise into a write across
 * the
 * crosstalk fabric to the Hub's interrupt-modify register on the node
 * the
 * guest named; the Hub sets the pending bit that write asks for; the
 * pending
 * bit, if the processor's mask admits it, raises a processor interrupt
 * line.
 * The only thing asserted is the far end.
 *
 * **The address that write goes to is the part with no second opinion
 * available.** It is reconstructed from a compressed per-pin register —
 * ten
 * bits holding the crosstalk address's top bit and the nine-bit node —
 * and
 * three independent sources agree on that layout, but agreement about a
 * layout
 * is not evidence that a model reassembles it correctly. A model that
 * shifted
 * the node one bit would deliver every interrupt to a node that does
 * not
 * exist, silently, on a machine with one node where nothing else would
 * notice.
 * This is the test that notices.
 *
 * What the guest has to do first, and each step is a real one a driver
 * does:
 * give the device's memory window a base and turn its decoding on,
 * point the
 * bridge's window for that slot at the same place, tell the bridge
 * where to
 * send that pin's interrupt and which number to carry, and let the
 * number
 * through the processor's mask. Then raise the pin.
 *
 * The device is at PCI slot 3, so its interrupt reaches bridge pin 3 —
 * the
 * pin
 * this machine's contract ratifies for the base I/O controller's second
 * interrupt. That is not a coincidence worth relying on and the ROM
 * does not
 * rely on it: what it asserts is that the pin the device drives is the
 * pin
 * the
 * bridge reports and the one whose destination register it programmed.
 *
 * **Needs both the bridge and a device to drive a pin**, so it is run
 * as
 *
 *     qemu-system-mips64 -M origin200 -cpu R10000 \
 *         -nodefaults -display none -no-reboot -semihosting \
 *         -device sgi-bridge-dma-test,addr=3 -bios
 * build/tests/functional/mips64/roms/intchain.bin
 *
 * **The `-semihosting` is not optional and its absence does not look
 * like an
 * error.** It is how every ROM here reports; without it this one runs,
 * passes,
 * and cannot say so, which from outside is indistinguishable from a
 * hang.
 * The QEMU functional ROM matrix is registered as
 * `func-mips64-sgi_roms` and source-builds its images. This case stays out
 * of its manifest because that runner cannot add `sgi-bridge-dma-test`.
 * Build the image with `ninja sgi-roms` and use the explicit QEMU command
 * above.
 *
 * Keep it out of the manifest: a ROM that skipped when its subject was
 * absent would be worse than no ROM. R10000 only, for the reason the
 * other machine ROMs are —
 * the
 * route to crosstalk space is an R10000-family addressing extension.
 */

#include "rom.h"

const char rom_name[] = "intchain";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1

/*
 * The two widgets this ROM talks to, through their small windows.
 */
#define BRIDGE_WINDOW   0x08000000ull
#define HUB_WINDOW      0x01000000ull

#define BRIDGE_AT(off)  XKPHYS(CCA_UNCACHED, ATTR_XTALK, BRIDGE_WINDOW + (off))
#define HUB_AT(off)     XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_WINDOW + (off))

/*
 * Bridge registers are 32 bits in the low half of an eight-byte slot.
 */
#define REG_DATA        4
#define BRIDGE_REG(off) BRIDGE_AT((off) + REG_DATA)

#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_CFG0             0x020000
#define DEV_MEMORY_SPACE        (1u << 12)

/*
 * The device: slot 3, so bridge pin 3.
 */
#define SLOT            3
#define CFG0(dev, fn)   (BRIDGE_CFG0 + ((dev) << 12) + ((fn) << 8))
#define CARD_MMIO       (0x10000000u + SLOT * 0x1000000u)
#define WINDOW_OFFSET   (0x600000u + (SLOT - 2) * 0x100000u)
#define DMA_REG_INTERRUPT 0x10

#define PCI_COMMAND         0x04
#define PCI_COMMAND_MEMORY  0x02
#define PCI_BAR0            0x10

/*
 * The Hub's side: where the packet lands, and what the processor sees.
 */
#define PI_INT_PEND0    0x000098
#define PI_INT_MASK0_A  0x0000a8
#define PI_INT_MODIFY   0x000090
#define CAUSE_BANK0     (1u << 10)

/*
 * The interrupt number this ROM asks for. Above the thirteen the
 * hardware
 * assigns itself, so nothing else in the machine drives it, and
 * distinctive
 * enough that a model delivering some other number fails rather than
 * coincidentally matching.
 */
#define INT_NUMBER      42

/*
 * What goes in the per-pin destination register: the node in bits 16:8
 * with
 * the crosstalk address's top bit above it, and the interrupt number in
 * bits
 * 7:0. Node zero, so the host field is just that top bit.
 */
#define INT_HOST_TOP_BIT    (1u << 17)
#define INT_DEST            (INT_HOST_TOP_BIT | INT_NUMBER)

static void put32(uint64_t addr, uint32_t val)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    *(volatile uint32_t *)(unsigned long)addr = val;
}

static uint32_t get32(uint64_t addr)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    return *(volatile uint32_t *)(unsigned long)addr;
}

static void put64(uint64_t addr, uint64_t val)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get64(uint64_t addr)
{
    /*
     * Volatile preserves each required MMIO access.
     */
    return *(volatile uint64_t *)(unsigned long)addr;
}

void rom_main(void)
{
    /*
     * Interrupts off: a raised line is read as a Cause bit, never
     * taken.
     */
    rom_set_status((rom_get_status() & ~ROM_ST_IE) | ROM_ST_KX);

    /*
     * Nothing pending and nothing masked in before this ROM asks for
     * it.
     */
    put64(HUB_AT(PI_INT_MASK0_A), 0);
    rom_assert_eq(1, 0, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(2, 0, rom_get_cause() & CAUSE_BANK0);

    /*
     * Give the device its memory window and turn decoding on, then
     * point the
     * bridge's window for that slot at the same place. Both halves are
     * needed:
     * the card decodes what its base register says, and the bridge only
     * forwards what its device register says.
     */
    put32(BRIDGE_AT(CFG0(SLOT, 0) + PCI_BAR0), CARD_MMIO);
    put32(BRIDGE_AT(CFG0(SLOT, 0) + PCI_COMMAND), PCI_COMMAND_MEMORY);
    put32(BRIDGE_REG(BRIDGE_DEVICE(SLOT)),
          DEV_MEMORY_SPACE | (CARD_MMIO >> 20));

    /*
     * Tell the bridge where this pin's interrupt goes and what number
     * it
     * carries. This is the register the delivery address is
     * reconstructed
     * from, so it is the one whose interpretation the whole chain rests
     * on.
     */
    put32(BRIDGE_REG(BRIDGE_INT_HOST(SLOT)), INT_DEST);

    /*
     * And let that number through to the processor.
     */
    put64(HUB_AT(PI_INT_MASK0_A), 1ull << INT_NUMBER);

    /*
     * Still nothing: programming a destination does not raise an
     * interrupt.
     */
    rom_assert_eq(3, 0, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(4, 0, rom_get_cause() & CAUSE_BANK0);

    /*
     * Raise the pin. Everything after this line is the machine's own
     * doing:
     * the bridge composes a packet, the fabric carries it, the Hub
     * decodes it
     * and sets a bit, and the bit raises a line.
     */
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 1);

    /*
     * The bridge sees its own pin, which says the device reached it.
     */
    rom_assert_eq(5, 1u << SLOT, get32(BRIDGE_REG(BRIDGE_INT_STATUS)));

    /*
     * The Hub has the bit the bridge asked for — the right bit, not
     * merely a
     * bit. A model that dropped the number and set bit zero, or that
     * shifted
     * the node and wrote to a Hub that does not exist, fails here while
     * every
     * register test in the machine still passes.
     */
    rom_assert_eq(6, 1ull << INT_NUMBER, get64(HUB_AT(PI_INT_PEND0)));

    /*
     * And the processor sees the line. This is the far end of the
     * chain.
     */
    rom_assert_eq(7, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /*
     * Dropping the pin leaves the Hub bit set, because this guest did
     * not ask
     * the bridge to send clears — the mode bit for the pin is clear, so
     * clearing is the handler's job. A model that cleared anyway would
     * take
     * the interrupt away from a guest that had not finished with it.
     */
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 0);
    rom_assert_eq(8, 0, get32(BRIDGE_REG(BRIDGE_INT_STATUS)));
    rom_assert_eq(9, 1ull << INT_NUMBER, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(10, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /*
     * Now the node field, which everything above is blind to.
     *
     * With one node numbered zero, a packet addressed to the right node
     * and a
     * packet addressed to the wrong one by a mis-assembled shift arrive
     * at
     * the
     * same place, because the node bits are zero either way. Shifting
     * the
     * host
     * field by one bit in the model passes every assertion above — I
     * checked,
     * and that is why this is here.
     *
     * What can be asked on a machine this small is whether the node is
     * used
     * at
     * all. Naming a node that is not this one must send the packet
     * somewhere
     * that is not this Hub, so the pending bit must not change. A model
     * that
     * dropped the node and always addressed node zero fails here.
     *
     * **What this still cannot check is the node's bit position**,
     * because
     * every wrong node on a one-node machine is equally absent. That
     * needs a
     * second node, and it is the kind of thing this project's rules
     * warn
     * about
     * exactly: single-node correctness is not evidence of a correct
     * encoding.
     */
    put64(HUB_AT(PI_INT_MASK0_A), 0);
    put64(HUB_AT(PI_INT_MODIFY), INT_NUMBER);
    rom_assert_eq(11, 0, get64(HUB_AT(PI_INT_PEND0)));

    put32(BRIDGE_REG(BRIDGE_INT_HOST(SLOT)),
          INT_HOST_TOP_BIT | (1u << 8) | INT_NUMBER);
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 1);
    rom_assert_eq(12, 0, get64(HUB_AT(PI_INT_PEND0)));
}
