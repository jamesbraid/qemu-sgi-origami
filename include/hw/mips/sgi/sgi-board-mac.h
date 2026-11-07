/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The onboard Ethernet address every SGI machine takes through its "mac"
 * machine property. Each board keeps the address where its own hardware
 * does -- an L1 identity record on the Bedrock products, the IOC3 bus's
 * Ethernet-address part on Octane and the Hub products -- so only the
 * property, its default and its parsing are shared.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_MAC_H
#define HW_MIPS_SGI_SGI_BOARD_MAC_H

#include "hw/mips/sgi/sgi-board-identity.h"

/*
 * The address when mac= is not given is 08:00:69:E?:xx:xx, worked out from
 * the machine's system disk or type name; see sgi-board-identity.h.
 */

/*
 * @base, or the machine's derived default when NULL, plus @offset over its 48
 * bits, as "xx:xx:xx:xx:xx:xx": the address of node @offset, or of the
 * @offset'th onboard Ethernet where boards and nodes differ. NULL with
 * @errp set when @base is not a nonzero unicast address or the sum is
 * not one.
 */
char *sgi_board_mac(const char *base, unsigned int offset, Error **errp);

/*
 * The default six-character text serial of node @node's system board:
 * "EMUN" and a hex byte of the same seed as the default address, plus
 * @node. See sgi-board-identity.h.
 */
char *sgi_board_default_text_serial(unsigned int node);

/*
 * The default system serial, "L" and seven digits of the same seed. Every
 * node of a machine shares it: it names the system, not a board. See
 * sgi-board-identity.h.
 */
char *sgi_board_default_system_serial(void);

/*
 * Whether the identity came from a disk or identity-key, not the machine
 * name, so two guests differ. Models whose catalogue serials predate the
 * seed keep them unless this is true.
 */
bool sgi_board_identity_is_seeded(void);

/* The ten-character module serial of module @module, "K" and nine more. */
char *sgi_board_default_module_serial(unsigned int module);

/*
 * Add the "mac" and "identity-key" properties to machine class @oc, kept
 * as a string @offset bytes into each instance. A value that is not a
 * nonzero unicast address is refused, as is setting it once the machine is
 * built. @description says which boards take it.
 */
void sgi_board_mac_class_add_property(ObjectClass *oc, size_t offset,
                                      const char *description);

#endif /* HW_MIPS_SGI_SGI_BOARD_MAC_H */
