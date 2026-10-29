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
 * given (docs/system/mips/sgi/sn.rst, Graphics pipes).
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

/* One pipe a machine holds, by its catalogue slot or its id. */
typedef struct SGIGraphicsFitted {
    DeviceState *dev;
    char *name;
} SGIGraphicsFitted;

/*
 * The pipes one machine holds, which the SN machines and the Octane both
 * embed: the catalogue record they are judged against, the default pipe
 * created at init, and every pipe plugged so far. A machine calls
 * sgi_graphics_pipes_init() while it initializes,
 * sgi_graphics_pipes_pre_plug() and sgi_graphics_pipes_plug() from its
 * hotplug handler, sgi_graphics_pipes_done() once every -device exists
 * and sgi_graphics_pipes_cleanup() when it is finalized.
 */
typedef struct SGIGraphicsPipes {
    /* The -M name, for messages. */
    const char *machine;
    /* The chassis topology on SN machines, whose record graphics is. */
    const SGIModelTopology *topology;
    const SGIModelGraphics *graphics;
    uint32_t nodes;
    /* x-uncatalogued-graphics=on: no checks and no default pipe. */
    bool uncatalogued;
    /* Created at init, realized or dropped at done. */
    DeviceState *default_pipe;
    /* SGIGraphicsFitted, in plug order. */
    GPtrArray *fitted;
} SGIGraphicsPipes;

/*
 * Check -vga and create topology @t's default pipe, unless @uncatalogued
 * or -vga none skips it. @nodes is the node boards the machine fits.
 */
bool sgi_graphics_pipes_init(SGIGraphicsPipes *p, const char *machine,
                             const SGIModelTopology *t, uint32_t nodes,
                             bool uncatalogued, Error **errp);

/*
 * Judge @dev before it realizes. False with @errp set for a pipe the
 * catalogue refuses; true for an accepted pipe and for any device that
 * is not a pipe at all.
 */
bool sgi_graphics_pipes_pre_plug(SGIGraphicsPipes *p, DeviceState *dev,
                                 Error **errp);

/* Count @dev, once it has realized, if it is a pipe. */
void sgi_graphics_pipes_plug(SGIGraphicsPipes *p, DeviceState *dev);

/* Realize the default pipe if no -device was a pipe, else drop it. */
void sgi_graphics_pipes_done(SGIGraphicsPipes *p);

void sgi_graphics_pipes_cleanup(SGIGraphicsPipes *p);

/*
 * Check pipe @dev, of @family, against registry @pipes before it realizes,
 * as a machine's pre-plug handler does: find the slot its placement
 * properties name, refuse a family, product or setting value the slot
 * does not offer with the catalogue's own reason, refuse a slot the
 * machine's node boards do not fit and a pipe beyond the topology's
 * max-pipes, and set every setting @dev leaves unset to the slot's
 * default.
 */
bool sgi_graphics_check_pipe(SGIGraphicsPipes *pipes, DeviceState *dev,
                             SGIModelGraphicsFamilyKind family,
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
