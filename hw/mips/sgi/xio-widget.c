/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XIO widget layer: what every widget on an XIO link shares
 * (include/hw/mips/sgi/xio-widget.h).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/mips/sgi/xio-widget.h"

SGIXioIntDest sgi_xio_int_dest(uint32_t upper, uint32_t lower)
{
    SGIXioIntDest dest = {
        .vector = upper >> SGI_XIO_INT_DEST_VECTOR_SHIFT,
        .widget = (upper & SGI_XIO_INT_DEST_WIDGET_MASK) >>
                  SGI_XIO_INT_DEST_WIDGET_SHIFT,
        .address = ((uint64_t)(upper & SGI_XIO_INT_DEST_ADDRESS_UPPER) << 32) |
                   lower,
    };

    return dest;
}

bool sgi_xio_compose(const char *who, unsigned int widget, uint64_t address,
                     hwaddr *packet)
{
    if (widget >= SGI_XIO_WIDGETS ||
        address >> SGI_XIO_PACKET_ADDRESS_BITS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: packet fabric cannot encode widget %u address "
                      "0x%" PRIx64 "\n", who, widget, address);
        return false;
    }
    *packet = ((uint64_t)widget << SGI_XIO_PACKET_ADDRESS_BITS) | address;
    return true;
}

MemTxResult sgi_xio_send_interrupt(AddressSpace *fabric, hwaddr packet,
                                   uint8_t number, bool set)
{
    MemTxResult result;

    address_space_stq_be(fabric, packet,
                         number | (set ? SGI_XIO_INTERRUPT_SET : 0),
                         MEMTXATTRS_UNSPECIFIED, &result);
    return result;
}
