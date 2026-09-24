/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the PS/2 keyboard and mouse controller.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The block is a
 * bit-level PS/2 line engine, not an 8042 clone; it wraps two generic ps2
 * devices and presents SGI's five-register interface. The project tree's
 * IOC3 datasheet is the specification. See docs/specs/sgi-ioc3.rst.
 *
 * Host key events without a console binding reach whichever keyboard
 * registered first, so a machine with two of these controllers (the
 * BaseIO-G's board IOC3 and its MediaIO daughtercard's) only ever types on
 * one. The write-only bool property "activate-keyboard" picks: setting it
 * true on a controller sends later key events to its keyboard, and false
 * moves that keyboard behind every other one. It is host input routing
 * only and changes nothing the guest can read, e.g.
 *   qom-set <ioc3-path>/kbc activate-keyboard true
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
 * Bit 27 reports whether the packed characters came from the keyboard or the
 * mouse. Mainline Linux's own header gives the polarity -- clear for the
 * keyboard, set for the mouse (arch/mips/include/asm/sn/ioc3.h) -- which is
 * what this model follows.
 */
#define SGI_IOC3_KBC_RX_SOURCE_AUX  (1u << 27)

/*
 * Bit 28 reports that a character arrived while the three slots were full
 * and was lost. A read clears it along with the characters it reports.
 */
#define SGI_IOC3_KBC_RX_OVERFLOW    (1u << 28)

/*
 * The two clamp pairs. Arming a port's clamp makes the controller hold that
 * port's clock line low once it has received one, or three, characters, which
 * stops the device instead of losing what it has left to say. Mainline
 * Linux's ioc3kbd driver arms clamp-after-three on both ports as its only
 * initialisation write to this register; the bit positions are its own
 * header's (arch/mips/include/asm/sn/ioc3.h).
 */
#define SGI_IOC3_KBC_CTRL_K_CLAMP_1 (1u << 20)
#define SGI_IOC3_KBC_CTRL_M_CLAMP_1 (1u << 21)
#define SGI_IOC3_KBC_CTRL_K_CLAMP_3 (1u << 22)
#define SGI_IOC3_KBC_CTRL_M_CLAMP_3 (1u << 23)
#define SGI_IOC3_KBC_CTRL_CLAMP_MASK 0x00f00000u

/*
 * Control/status write mask. The datasheet enumerates the writable control
 * fields -- the clamp-after-one and clamp-after-three pairs, the two
 * timeout-enable bits, emulation mode, the fast-clock bit and the manual
 * line-pull bits -- but does not pin their bit positions; Linux's header
 * does, and the clamps are the only ones this model acts on. So the low
 * half is stored as software control, the four clamp bits are stored and
 * obeyed, and the rest of the upper half -- the hardware-owned write-pending,
 * idle, timeout and live line states -- is read-only and reads clear in the
 * synchronous model.
 */
#define SGI_IOC3_KBC_CONTROL_MASK   (0x0000ffffu | SGI_IOC3_KBC_CTRL_CLAMP_MASK)

/* The two ports, in the order their GPIO inputs and registers are wired. */
#define SGI_IOC3_KBC_PORT_KBD       0
#define SGI_IOC3_KBC_PORT_AUX       1
#define SGI_IOC3_KBC_PORTS          2

/*
 * What one port's controller is holding: the characters it has taken from
 * the device, oldest first, and whether one was lost for want of room.
 * Everything that decides the next interrupt lives here, so it all migrates.
 */
typedef struct SGIIOC3KBCPort {
    uint8_t data[SGI_IOC3_KBC_RX_SLOTS];
    uint8_t count;
    bool overflow;
} SGIIOC3KBCPort;

struct SGIIOC3KBCState {
    SysBusDevice parent_obj;

    PS2KbdState kbd;
    PS2MouseState mouse;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t control;
    SGIIOC3KBCPort port[SGI_IOC3_KBC_PORTS];

    /*
     * Set while a fill is moving characters in. Popping one re-raises the
     * ps2 device's own line, which lands back in the fill; the outer pass
     * owns the holding, so the re-entry returns. Never set across a guest
     * access, so nothing migrates it.
     */
    bool filling;
};

#endif /* HW_INPUT_SGI_IOC3_KBC_H */
