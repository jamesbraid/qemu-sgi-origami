/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The onboard Ethernet address every SGI machine takes through its "mac"
 * machine property, and the "emu-id", "serial", "hostid" and "hostids"
 * properties that give the machine its system id. Each board keeps the
 * address where its own hardware does -- an L1 identity record on the Bedrock
 * products, the IOC3 bus's Ethernet-address part on Octane and the Hub
 * products -- so only the properties, their defaults and their parsing are
 * shared.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_MAC_H
#define HW_MIPS_SGI_SGI_BOARD_MAC_H

#include "hw/mips/sgi/sgi-board-identity.h"

/*
 * The address when mac= is not given comes from the machine's identity
 * inputs: 08:00:69:E<kind>:xx:xx for an emu-id, or 08:00:69 and the last
 * three bytes of a hostid or serial on the address families; see
 * sgi-board-identity.h.
 */

/* The identity inputs of one machine, and what they come to. */
typedef struct SGIBoardIdentityInputs {
    uint32_t emu_id;
    char *serial;
    char *hostid;
    SGIBoardId id;
    bool resolved;
} SGIBoardIdentityInputs;

/*
 * Work out @in->id from the inputs and @mac (NULL when mac= is not given) for
 * a machine of @family, once the properties are set. False with @errp set,
 * naming the disagreeing inputs and the family's rule, when they conflict.
 */
bool sgi_board_identity_resolve(SGIBoardIdentityInputs *in,
                                SGIBoardFamily family, const char *mac,
                                Error **errp);

/*
 * @base, or the default for @id when NULL, plus @offset over its 48 bits, as
 * "xx:xx:xx:xx:xx:xx": the address of node @offset, or of the @offset'th
 * onboard Ethernet where boards and nodes differ. NULL with @errp set when
 * @base is not a nonzero unicast address, the sum is not one, or the address
 * is the emulator layout and @offset is 16 or more: the default has room for
 * 16 nodes or ports before the sum would reach the emu-id.
 */
char *sgi_board_mac(const char *base, const SGIBoardId *id,
                    unsigned int offset, Error **errp);

/*
 * The default six-character text serial of node @node's system board:
 * "EMUN" and @node as a hex byte. It follows no identity input, because the
 * PROM ties its NVRAM to this serial. See sgi-board-identity.h.
 */
char *sgi_board_default_text_serial(unsigned int node);

/* The system serial text of an IP35 machine, the same on every L1. */
char *sgi_board_default_system_serial(const SGIBoardId *id);

/*
 * The serial of module @module. NULL with @errp set when @module is 10 or
 * more.
 */
char *sgi_board_default_module_serial(const SGIBoardId *id,
                                      unsigned int module, Error **errp);

/*
 * Add the "mac" property to machine class @oc, kept as a string @offset
 * bytes into each instance. A value that is not a nonzero unicast address is
 * refused, as is setting it once the machine is built. @description says
 * which boards take it.
 */
void sgi_board_mac_class_add_property(ObjectClass *oc, size_t offset,
                                      const char *description);

/*
 * Appends the host ids the guest of @obj will print to @ids, one per node or
 * module in order, computed from the serial numbers and addresses the built
 * machine holds. False with @errp set when the machine cannot say. IRIX
 * prints one id per module or node, so a multi-node machine lists several.
 */
typedef bool (*SGIBoardHostidsFn)(Object *obj, GArray *ids, Error **errp);

/*
 * Add the "emu-id" property to machine class @oc and, when @hostids is not
 * NULL, "serial", "hostid" and the read-only "hostids". The inputs are an
 * SGIBoardIdentityInputs @inputs_offset bytes into each instance and cannot
 * change once the machine is built. A machine that takes no identity input
 * but the emu-id, the O2, passes NULL and has no host id property.
 */
void sgi_board_identity_class_add_properties(ObjectClass *oc,
                                             size_t inputs_offset,
                                             SGIBoardHostidsFn hostids);

#endif /* HW_MIPS_SGI_SGI_BOARD_MAC_H */
