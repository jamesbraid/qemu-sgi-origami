/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics pipes against the machine catalogue.
 *
 * A pipe is one -device of its family's device type (sgi-mgras,
 * sgi-odyssey, sgi-kona, psitech-rad4), placed in a slot by the
 * properties QEMU already has for it: slot= for an XIO board, bus= and
 * addr= for a PCI card. Each topology's catalogue record
 * (SGIModelGraphics, qapi/sgi-machines.json) lists its slots, what each
 * offers, the pipe the machine fits by default and the reason for every
 * combination it refuses. An SGI machine checks every pipe against that
 * record before the device realizes, fills in each setting the device
 * leaves unset from the slot's defaults, and fits its default pipe once
 * every -device has been created, unless one was a pipe or -vga none was
 * given (docs/specs/sgi-sn.rst, Graphics pipes).
 */

#ifndef HW_MIPS_SGI_SGI_GRAPHICS_H
#define HW_MIPS_SGI_SGI_GRAPHICS_H

#include "hw/mips/sgi/sgi-machines.h"
#include "hw/core/qdev.h"

/*
 * The family @dev builds, through *@family; false when @dev is not an SGI
 * graphics pipe at all.
 */
bool sgi_graphics_pipe_family(DeviceState *dev,
                              SGIModelGraphicsFamilyKind *family);

/*
 * What -vga asks of a machine whose display is its catalogue's: true
 * through *@none for -vga none (or -nodefaults), which skips the default
 * pipe, and false for no -vga at all. Any other -vga value is refused,
 * because no SGI machine takes a PC display adapter. @machine is the -M
 * name, for the message.
 */
bool sgi_graphics_vga_none(const char *machine, bool *none, Error **errp);

/* @g's slot with a default pipe, or NULL for a machine with none. */
const SGIModelGraphicsSlot *sgi_graphics_default_slot(
    const SGIModelGraphics *g);

/*
 * Check pipe @dev, of @family, against topology @t's graphics record @g
 * before it realizes, as a machine's pre-plug handler does: find the
 * slot its placement properties name, refuse a family, product or
 * setting value the slot does not offer with the catalogue's own reason,
 * refuse a slot the machine's @nodes node boards do not fit and a pipe
 * beyond the @pipes the machine already holds (@first naming the slot of
 * the first), and set every setting @dev leaves unset to the slot's
 * default. @g may be NULL, for a topology with no graphics slot.
 */
bool sgi_graphics_check_pipe(const char *machine, const SGIModelTopology *t,
                             const SGIModelGraphics *g, uint32_t nodes,
                             DeviceState *dev,
                             SGIModelGraphicsFamilyKind family,
                             unsigned int pipes, const char *first,
                             Error **errp);

/*
 * The catalogue slot pipe @dev of @family names, or NULL: by its slot=
 * for an XIO board, by its bus= and addr= for a PCI card.
 */
const SGIModelGraphicsSlot *sgi_graphics_pipe_slot(
    const SGIModelGraphics *g, DeviceState *dev,
    SGIModelGraphicsFamilyKind family);

/*
 * @slot's default pipe as a device created but not realized, with its
 * placement, product and every setting set. A machine creates it while
 * it initializes, so it is not taken for a hot-plugged device, and
 * realizes it once every -device exists and none was a pipe.
 */
DeviceState *sgi_graphics_new_default(const SGIModelGraphicsSlot *slot);

#endif /* HW_MIPS_SGI_SGI_GRAPHICS_H */
