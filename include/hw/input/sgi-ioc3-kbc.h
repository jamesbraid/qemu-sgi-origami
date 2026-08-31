/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the PS/2 keyboard and mouse controller.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The block is a
 * bit-level PS/2 line engine, not an 8042 clone; it wraps two generic ps2
 * devices and presents SGI's five-register interface. The project tree's
 * IOC3 datasheet is the specification. See docs/specs/sgi-ioc3.rst.
 */

#ifndef HW_INPUT_SGI_IOC3_KBC_H
#define HW_INPUT_SGI_IOC3_KBC_H

#include "hw/core/sysbus.h"
#include "hw/input/ps2.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3_KBC "sgi-ioc3-kbc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3KBCState, SGI_IOC3_KBC)

/*
 * Register offsets within the controller's window, which the chip places at
 * 0x9c in its main register block.
 */
#define SGI_IOC3_KBC_CONTROL        0x00    /* control and status */
#define SGI_IOC3_KBC_KBD_RECEIVE    0x04    /* keyboard receive (read-only) */
#define SGI_IOC3_KBC_AUX_RECEIVE    0x08    /* mouse receive (read-only) */
#define SGI_IOC3_KBC_KBD_TRANSMIT   0x0c    /* keyboard transmit (write-only) */
#define SGI_IOC3_KBC_AUX_TRANSMIT   0x10    /* mouse transmit (write-only) */
#define SGI_IOC3_KBC_MMIO_SIZE      (SGI_IOC3_KBC_AUX_TRANSMIT + 4)

/*
 * Receive register: up to three bytes packed since the last read, oldest in
 * the highest slot. Counting slots p = 2 (oldest) down to 0 (newest), slot p
 * carries its data byte at bits p*8, its valid flag at bit 29+p and its
 * framing/parity error flag at bit 24+p. Reading pops the queue, so the bytes
 * and their flags clear on the next read.
 */
#define SGI_IOC3_KBC_RX_SLOTS       3
#define SGI_IOC3_KBC_RX_DATA(p, b)  ((uint32_t)((b) & 0xff) << ((p) * 8))
#define SGI_IOC3_KBC_RX_VALID(p)    (1u << (29 + (p)))

/*
 * Bit 27 reports whether the packed bytes came from the keyboard or the
 * mouse. The datasheet fixes the field's role but not its polarity, so the
 * assignment here -- clear for the keyboard, set for the mouse -- is this
 * model's choice; no guest on origami's path depends on it.
 */
#define SGI_IOC3_KBC_RX_SOURCE_AUX  (1u << 27)

/*
 * The datasheet's overflow flag (bit 28) marks a fourth byte lost. This
 * model's ps2 queue drops nothing -- surplus stays queued for the next read
 * -- so nothing is ever lost and the flag is never set.
 */

/*
 * Control/status write mask. The datasheet enumerates the writable control
 * fields -- the clamp-after-one and clamp-after-three pairs, the two
 * timeout-enable bits, emulation mode, the fast-clock bit and the manual
 * line-pull bits -- but does not pin their bit positions. This model stores
 * the low half as software control and treats the upper half, where the
 * hardware-owned status (write-pending, idle, timeout and the live line
 * states) lives, as read-only, reading clear in the synchronous model. The
 * mask is deliberately conservative and easy to narrow once positions are
 * pinned.
 */
#define SGI_IOC3_KBC_CONTROL_MASK   0x0000ffffu

struct SGIIOC3KBCState {
    SysBusDevice parent_obj;

    PS2KbdState kbd;
    PS2MouseState mouse;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t control;
};

#endif /* HW_INPUT_SGI_IOC3_KBC_H */
