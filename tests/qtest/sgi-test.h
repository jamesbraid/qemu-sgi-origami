/*
 * Addresses and widths the SGI register tests share.
 *
 * Every value here is a literal repeated from the machine's published
 * address map. None of it is included from the model's own headers: a test
 * that borrowed the model's constant could not catch the model getting that
 * constant wrong.
 *
 * A name belongs here only when every test that defines it means the same
 * address. Spelling differences do not matter, values do. Names two tests
 * give genuinely different values stay local to each test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SGI_TEST_H
#define SGI_TEST_H

/* Crosstalk, and what sits at the far end of it. */
#define SPACE_XTALK             UINT64_C(0x20000000000)
#define DEV_MEMORY_SPACE        (1u << 12)

/* The Hub and the stride between nodes. */
#define SPACE_HUB               UINT64_C(0x10000000000)
#define HUB_LOCAL               (SPACE_XTALK + 0x01000000)
#define NODE_STRIDE             UINT64_C(0x100000000)
#define PI_INT_PEND0            0x98

/*
 * PCI configuration space. A few of these tests reach Linux's pci_regs.h
 * through hw/pci/pci.h, which gives the first three the same values under
 * the same names but spells the literals differently, so defer to whatever
 * is already in scope rather than redefine it. The two base-address offsets
 * are this project's shorter names for what Linux calls PCI_BASE_ADDRESS_0
 * and PCI_BASE_ADDRESS_1.
 */
#ifndef PCI_COMMAND
#define PCI_COMMAND             0x04
#endif
#ifndef PCI_COMMAND_MEMORY
#define PCI_COMMAND_MEMORY      0x0002
#endif
#ifndef PCI_COMMAND_MASTER
#define PCI_COMMAND_MASTER      0x0004
#endif
#define PCI_BAR0                0x10
#define PCI_BAR1                0x14

/* The PROM image, and where a test plants generated processor code. */
#define PROM_SIZE               UINT64_C(0x1686a8)
#define CODE_START              0x400

#endif /* SGI_TEST_H */
