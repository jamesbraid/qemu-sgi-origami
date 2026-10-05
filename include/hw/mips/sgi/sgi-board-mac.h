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

/*
 * The address when mac= is not given: SGI's 08:00:69 OUI with a fixed
 * placeholder station part. It is not any machine's factory address.
 */
#define SGI_BOARD_DEFAULT_MAC "08:00:69:12:34:56"

/*
 * @base, or SGI_BOARD_DEFAULT_MAC when NULL, plus @offset over its 48
 * bits, as "xx:xx:xx:xx:xx:xx": the address of node @offset, or of the
 * @offset'th onboard Ethernet where boards and nodes differ. NULL with
 * @errp set when @base is not a nonzero unicast address or the sum is
 * not one.
 */
char *sgi_board_mac(const char *base, unsigned int offset, Error **errp);

/*
 * Add the "mac" property to machine class @oc, kept as a string @offset
 * bytes into each instance. A value that is not a nonzero unicast address
 * is refused, as is setting it once the machine is built. @description
 * says which boards take it.
 */
void sgi_board_mac_class_add_property(ObjectClass *oc, size_t offset,
                                      const char *description);

#endif /* HW_MIPS_SGI_SGI_BOARD_MAC_H */
