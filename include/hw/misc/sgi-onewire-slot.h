/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI's MicroLAN one-wire slot decode: one register write, one bit slot on
 * the 1-Wire bus. Shared by the IOC3, the Bridge and the Hub, whose control
 * registers all carry the same twenty-bit low-drive/sample-offset/pulse
 * layout.
 */

#ifndef HW_MISC_SGI_ONEWIRE_SLOT_H
#define HW_MISC_SGI_ONEWIRE_SLOT_H

#include "hw/1wire/onewire.h"

/*
 * One bit slot on SGI's MicroLAN register cell. The low-drive time selects
 * what the slot means: a long pulse is a reset, a short one releases the
 * wire (a write-one, which is also how a read is done), and anything between
 * holds it down for a write-zero. Returns the level sampled from the wire.
 *
 * A pure function, not an object: the exchange is synchronous, so the master
 * holds no state between slots. Completion timing is not this function's
 * business -- the IOC3 and the Bridge report it synchronously inside the
 * register write, while the Hub arms a timer and reports it later; each
 * master applies its own policy on top of the sampled level this returns.
 *
 * The 400/40 thresholds are modelling choices, not values read off hardware
 * -- no datasheet gives real pulse widths in register-write units, and the
 * three existing hand-written responders already agreed on these two
 * numbers, so the decode preserves them rather than inventing new ones.
 */
static inline bool sgi_onewire_slot(OneWireBus *bus, uint32_t pulse,
                                    uint32_t sample)
{
    if (pulse >= 400) {
        return !onewire_reset(bus);   /* presence pulls the wire low */
    }
    return onewire_touch_bit(bus, pulse < 40);
}

#endif
