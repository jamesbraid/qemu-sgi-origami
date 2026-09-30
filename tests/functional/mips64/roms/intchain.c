/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * End-to-end PCI interrupt delivery through the Bridge, Hub and processor.
 * Slot 3 drives Bridge pin 3. The test programs its destination, raises
 * the pin and checks Bridge status, Hub pending state and processor cause.
 * A shifted node field can target an absent node without changing
 * single-node component results.
 *
 *     qemu-system-mips64 -M origin200 -cpu R10000 \
 *         -nodefaults -display none -no-reboot -semihosting \
 *         -device sgi-bridge-dma-test,addr=3 -bios \
 *         build/tests/functional/mips64/roms/intchain.bin
 *
 * The ROM matrix does not register this test device. Semihosting reports
 * the ROM result. The crosstalk address requires R10000-family addressing.
 */

#include "rom.h"

const char rom_name[] = "intchain";

#define XKPHYS(cca, attr, pa)                                                 \
    (0x8000000000000000ull | ((uint64_t)(cca) << 59) |                        \
     ((uint64_t)(attr) << 57) | (uint64_t)(pa))

#define CCA_UNCACHED    2
#define ATTR_XTALK      1

#define BRIDGE_WINDOW   0x08000000ull
#define HUB_WINDOW      0x01000000ull

#define BRIDGE_AT(off)  XKPHYS(CCA_UNCACHED, ATTR_XTALK, BRIDGE_WINDOW + (off))
#define HUB_AT(off)     XKPHYS(CCA_UNCACHED, ATTR_XTALK, HUB_WINDOW + (off))

/* Bridge registers occupy the low word of each eight-byte slot. */
#define REG_DATA        4
#define BRIDGE_REG(off) BRIDGE_AT((off) + REG_DATA)

#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define BRIDGE_INT_HOST(n)      (0x130 + 8 * (n))
#define BRIDGE_INT_STATUS       0x100
#define BRIDGE_CFG0             0x020000
#define DEV_MEMORY_SPACE        (1u << 12)

#define SLOT            3
#define CFG0(dev, fn)   (BRIDGE_CFG0 + ((dev) << 12) + ((fn) << 8))
#define CARD_MMIO       (0x10000000u + SLOT * 0x1000000u)
#define WINDOW_OFFSET   (0x600000u + (SLOT - 2) * 0x100000u)
#define DMA_REG_INTERRUPT 0x10

#define PCI_COMMAND         0x04
#define PCI_COMMAND_MEMORY  0x02
#define PCI_BAR0            0x10

#define PI_INT_PEND0    0x000098
#define PI_INT_MASK0_A  0x0000a8
#define PI_INT_MODIFY   0x000090
#define CAUSE_BANK0     (1u << 10)

/* Above the thirteen interrupt numbers assigned by the hardware. */
#define INT_NUMBER      42

/* Node number in bits 16:8, address top bit in bit 17, interrupt in 7:0. */
#define INT_HOST_TOP_BIT    (1u << 17)
#define INT_DEST            (INT_HOST_TOP_BIT | INT_NUMBER)

static void put32(uint64_t addr, uint32_t val)
{
    *(volatile uint32_t *)(unsigned long)addr = val;
}

static uint32_t get32(uint64_t addr)
{
    return *(volatile uint32_t *)(unsigned long)addr;
}

static void put64(uint64_t addr, uint64_t val)
{
    *(volatile uint64_t *)(unsigned long)addr = val;
}

static uint64_t get64(uint64_t addr)
{
    return *(volatile uint64_t *)(unsigned long)addr;
}

void rom_main(void)
{
    /* Keep interrupts masked so the test can inspect Cause directly. */
    rom_set_status((rom_get_status() & ~ROM_ST_IE) | ROM_ST_KX);

    put64(HUB_AT(PI_INT_MASK0_A), 0);
    rom_assert_eq(1, 0, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(2, 0, rom_get_cause() & CAUSE_BANK0);

    /* Enable the device BAR and its matching Bridge window. */
    put32(BRIDGE_AT(CFG0(SLOT, 0) + PCI_BAR0), CARD_MMIO);
    put32(BRIDGE_AT(CFG0(SLOT, 0) + PCI_COMMAND), PCI_COMMAND_MEMORY);
    put32(BRIDGE_REG(BRIDGE_DEVICE(SLOT)),
          DEV_MEMORY_SPACE | (CARD_MMIO >> 20));

    /* Program the pin's destination address and interrupt number. */
    put32(BRIDGE_REG(BRIDGE_INT_HOST(SLOT)), INT_DEST);

    put64(HUB_AT(PI_INT_MASK0_A), 1ull << INT_NUMBER);

    /* Destination setup alone must not raise the interrupt. */
    rom_assert_eq(3, 0, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(4, 0, rom_get_cause() & CAUSE_BANK0);

    /* The remaining checks cover delivery through the Bridge and Hub. */
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 1);

    rom_assert_eq(5, 1u << SLOT, get32(BRIDGE_REG(BRIDGE_INT_STATUS)));

    /* A wrong interrupt number or node can pass component tests. */
    rom_assert_eq(6, 1ull << INT_NUMBER, get64(HUB_AT(PI_INT_PEND0)));

    rom_assert_eq(7, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /* Clearing the pin does not clear the Hub pending bit in this mode. */
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 0);
    rom_assert_eq(8, 0, get32(BRIDGE_REG(BRIDGE_INT_STATUS)));
    rom_assert_eq(9, 1ull << INT_NUMBER, get64(HUB_AT(PI_INT_PEND0)));
    rom_assert_eq(10, CAUSE_BANK0, rom_get_cause() & CAUSE_BANK0);

    /* A nonzero node must not set this Hub's pending bit. A second node
     * is required to test the exact node-bit encoding.
     */
    put64(HUB_AT(PI_INT_MASK0_A), 0);
    put64(HUB_AT(PI_INT_MODIFY), INT_NUMBER);
    rom_assert_eq(11, 0, get64(HUB_AT(PI_INT_PEND0)));

    put32(BRIDGE_REG(BRIDGE_INT_HOST(SLOT)),
          INT_HOST_TOP_BIT | (1u << 8) | INT_NUMBER);
    put32(BRIDGE_AT(WINDOW_OFFSET + DMA_REG_INTERRUPT), 1);
    rom_assert_eq(12, 0, get64(HUB_AT(PI_INT_PEND0)));
}
