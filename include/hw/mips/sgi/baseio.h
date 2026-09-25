/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-baseio: SGI's base I/O functional unit -- the IOC3 plus SCSI
 * controllers, and on the IO-8 a USB controller, as PCI devices behind a
 * crosstalk-to-PCI bridge -- as one device with a preset per shipped
 * board design (docs/designs/sn-machine-model-architecture.md, "Base
 * I/O"). This device does not own the bridge or XBridge chip itself:
 * the carrier (the SN0 node for IP29's onboard I/O, the XIO BaseIO card,
 * the SN1 node for the IO-8) creates that chip, sets its own identity
 * properties and realizes it, then hands this device the chip's PCI bus
 * by link property. sgi_baseio_preset_lookup() exposes each preset's
 * chip-identity defaults (the carrier's own "board-part"/"revision" to
 * set on its Bridge or XBridge before this device ever exists) so the
 * default lives in exactly one table, matching every other board's
 * design-part-number default (see sgi-ip29-io's deleted header comment,
 * carried forward here).
 */
#ifndef HW_MIPS_SGI_BASEIO_H
#define HW_MIPS_SGI_BASEIO_H

#include "qom/object.h"

#define TYPE_SGI_BASEIO "sgi-baseio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBaseioState, SGI_BASEIO)

/*
 * The design-fact defaults a preset's own carrier applies to the chip it
 * creates, before that chip exists for this device to be handed a bus
 * from. board_part is a required, exactly-12-character MicroLAN
 * board-identity string (hw/pci-host/sgi-bridge.h,
 * sgi_bridge_board_part_error()); bridge_revision is the silicon
 * revision a Bridge-based preset's carrier sets on its own "revision"
 * property, 0 for the io8 preset, whose XBridge exposes no equivalent
 * property of its own. board_barcode, board_revision and board_name are
 * the rest of the board's identity record, which the carrier sets on its
 * Bridge's "board-barcode", "board-revision" and "board-name" properties;
 * board_revision is the board's revision letter, not the chip's. They
 * are NULL for the io8 preset, whose XBridge carries no such properties
 * and keeps the older text layout.
 */
typedef struct SGIBaseioPresetIdentity {
    const char *board_part;
    uint32_t bridge_revision;
    const char *board_barcode;
    const char *board_revision;
    const char *board_name;
} SGIBaseioPresetIdentity;

/* NULL if name is not a preset this class knows. */
const SGIBaseioPresetIdentity *sgi_baseio_preset_lookup(const char *name);

/*
 * Set a Bridge-based preset's identity on the carrier's unrealized
 * Bridge: its silicon revision, board part and the rest of the board's
 * identity record.
 */
void sgi_baseio_preset_set_bridge_identity(
    DeviceState *bridge, const SGIBaseioPresetIdentity *identity);

#endif
