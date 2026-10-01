/* SPDX-License-Identifier: BSD-3-Clause */
/* PsiTech RAD4-KM keyboard and mouse register definitions. */
/*
 * A child of hw/display/psitech-rad4.c, instantiated only when that device
 * answers the RAD4-KM PCI identity (12d1:0105). Two identical hardware
 * channels carry a software role byte the driver assigns at its own runtime
 * by probing each channel with the PS/2 reset command and reading the
 * BAT-pass/ID bytes back. QEMU's own ps2-kbd/ps2-mouse core devices already
 * answer that probe correctly regardless of which channel asks, so this model
 * fixes channel A to the keyboard and channel B to the mouse instead of
 * reproducing the driver's own runtime detection; the guest-visible
 * auto-detect outcome is unchanged.
 *
 * The register map below has sixteen registers, each on its own 64 KiB
 * stride, index-for-index against the public Signetics/Philips
 * SCN2681/SCC68681 DUART family. It implements the keyboard/mouse register
 * contract the driver exercises. The chip vendor and part number are an
 * inferred approximation: the register map matches the public DUART family,
 * but nothing in the driver object names a specific vendor or part number, so
 * the exact silicon behind it is not established. Every register this file
 * does not implement stays LOG_UNIMP rather than guessing a behavior for it.
 *
 * Register offset 0x78 in the driver's own object is established only as a
 * pointer to a distinct, board-level interrupt gate above this chip's own
 * ISR/IMR; no evidence names which physical chip or address it reaches, and
 * this model does not invent that register or claim to model it. What is a
 * plain PCI-structural fact, not a guess: this is a single-function PCI
 * device, so it has exactly one INTx# pin, and IMR gating which ISR bits
 * reach that pin is the ordinary Signetics/Philips SCN2681/SCC68681 DUART
 * family contract the register map's own header comment already cites. This
 * is a public chip-family fact, not a board-specific one. This model wires
 * that much: the channels' ISR condition, masked by IMR, drives this device's
 * own aggregate qemu_irq output, which hw/display/psitech-rad4.c ORs into its
 * single shared PCI interrupt line alongside INTCSR's own frame interrupt. It
 * does not assert where a real board's offset-0x78 gate would additionally
 * report this condition upstream of that pin.
 */

#ifndef HW_INPUT_PSITECH_RAD4_KM_H
#define HW_INPUT_PSITECH_RAD4_KM_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/input/ps2.h"
#include "qom/object.h"

#define TYPE_PSITECH_RAD4_KM "psitech-rad4-km"
OBJECT_DECLARE_SIMPLE_TYPE(PsitechRad4KMState, PSITECH_RAD4_KM)

/*
 * The 64 KiB per-register stride is the board's own PCI-to-local-bus address
 * decode, not the chip family's native register-select spacing, which is an
 * inferred approximation rather than a documented fact for this board. Every
 * access is 32-bit.
 */
#define RAD4_KM_REG_STRIDE      0x10000
#define RAD4_KM_REG_COUNT       16
#define RAD4_KM_MMIO_SIZE       (RAD4_KM_REG_COUNT * RAD4_KM_REG_STRIDE)

/* Channel A (keyboard) and channel B (mouse), index-for-index chip layout. */
#define RAD4_KM_REG_MRA          0x0     /* MR1A/MR2A, pointer-selected */
#define RAD4_KM_REG_SRA_CSRA     0x1     /* SRA (read) / CSRA (write) */
#define RAD4_KM_REG_CRA          0x2     /* command register A */
#define RAD4_KM_REG_RHRA_THRA    0x3     /* data register A, read and write */
#define RAD4_KM_REG_ACR          0x4     /* auxiliary control, chip-wide */
#define RAD4_KM_REG_ISR_IMR      0x5     /* ISR (read) / IMR (write) */
#define RAD4_KM_REG_CTUR         0x6     /* counter/timer upper, never used */
#define RAD4_KM_REG_CTLR         0x7     /* counter/timer lower, never used */
#define RAD4_KM_REG_MRB          0x8     /* MR1B/MR2B, pointer-selected */
#define RAD4_KM_REG_SRB_CSRB     0x9     /* SRB (read) / CSRB (write) */
#define RAD4_KM_REG_CRB          0xa     /* command register B */
#define RAD4_KM_REG_RHRB_THRB    0xb     /* data register B, read and write */
#define RAD4_KM_REG_IVR          0xc     /* interrupt vector; not accessed */
#define RAD4_KM_REG_OPCR         0xd     /* output port configuration */
#define RAD4_KM_REG_OPR_SET      0xe     /* set output port bits */
#define RAD4_KM_REG_OPR_RESET    0xf     /* reset output port bits */

/* Status register bits (index 0x1/0x9, read). */
#define RAD4_KM_SR_RXRDY         (1u << 0)
#define RAD4_KM_SR_TXRDY         (1u << 2)

/*
 * Command register (index 0x2/0xa, write): bits 1:0 select the Rx enable
 * state, bits 3:2 the Tx enable state, and bits 7:4 (MISC) name a command.
 * Values match the driver's own hex writes exactly (0x1a = MISC 1 + Rx/Tx
 * disable, 0x05 = Rx/Tx enable, and so on).
 */
#define RAD4_KM_CR_RX_MASK           0x03
#define RAD4_KM_CR_RX_ENABLE         0x01
#define RAD4_KM_CR_RX_DISABLE        0x02
#define RAD4_KM_CR_TX_MASK           0x0c
#define RAD4_KM_CR_TX_ENABLE         0x04
#define RAD4_KM_CR_TX_DISABLE        0x08
#define RAD4_KM_CR_MISC_SHIFT        4
#define RAD4_KM_CR_MISC_MASK         0x7
#define RAD4_KM_CR_MISC_RESET_MR     1
#define RAD4_KM_CR_MISC_RESET_RX     2
#define RAD4_KM_CR_MISC_RESET_TX     3
#define RAD4_KM_CR_MISC_RESET_ERROR  4
#define RAD4_KM_CR_MISC_RESET_BRK    5

/*
 * ISR bits (index 0x5, read): per-channel RxRDY/TxRDY positions match the bit
 * positions the driver's own IMR shadow test uses (0x02 for channel A, 0x20
 * for channel B).
 */
#define RAD4_KM_ISR_TXRDY_A       (1u << 0)
#define RAD4_KM_ISR_RXRDY_A       (1u << 1)
#define RAD4_KM_ISR_TXRDY_B       (1u << 4)
#define RAD4_KM_ISR_RXRDY_B       (1u << 5)

/* One hardware channel: its mode-register pointer and Rx/Tx enable state. */
typedef struct Rad4KMChannel {
    uint8_t mr_ptr;   /* 0 selects MR1 on the next write, 1 selects MR2 */
    uint8_t mr1;
    uint8_t mr2;
    uint8_t csr;
    bool rx_enabled;
    bool tx_enabled;
} Rad4KMChannel;

struct PsitechRad4KMState {
    SysBusDevice parent_obj;

    PS2KbdState kbd;      /* channel A */
    PS2MouseState mouse;  /* channel B */

    MemoryRegion mmio;
    qemu_irq irq;         /* chan_a/chan_b ISR & IMR, aggregated */

    Rad4KMChannel chan_a;
    Rad4KMChannel chan_b;

    /* Chip-wide state, not duplicated per channel. */
    uint8_t acr;
    uint8_t imr;
    uint8_t opcr;
    uint8_t output_port;
};

#endif /* HW_INPUT_PSITECH_RAD4_KM_H */
