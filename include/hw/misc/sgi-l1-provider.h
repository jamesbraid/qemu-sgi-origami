/* SPDX-License-Identifier: BSD-3-Clause */
/* The controller side of an L1 node-side UART window. */
#ifndef HW_MISC_SGI_L1_PROVIDER_H
#define HW_MISC_SGI_L1_PROVIDER_H

#include "qom/object.h"

#define TYPE_SGI_L1_PROVIDER "sgi-l1-provider"
typedef struct SGIL1ProviderClass SGIL1ProviderClass;
typedef struct SGIL1Provider SGIL1Provider;
DECLARE_CLASS_CHECKERS(SGIL1ProviderClass, SGI_L1_PROVIDER,
                       TYPE_SGI_L1_PROVIDER)
#define SGI_L1_PROVIDER(obj) \
    INTERFACE_CHECK(SGIL1Provider, (obj), TYPE_SGI_L1_PROVIDER)

typedef struct SGIL1UARTState SGIL1UARTState;

/*
 * What stands behind a UART window. Bytes toward the node use
 * sgi_l1_uart_queue().
 */
struct SGIL1ProviderClass {
    InterfaceClass parent;

    /* The window realizes: take @transport, or fail with @errp. */
    bool (*attach_window)(SGIL1Provider *p, unsigned transport,
                          SGIL1UARTState *window, Error **errp);
    /* Optional: the window reset. */
    void (*window_reset)(SGIL1Provider *p, unsigned transport);
    /* The node wrote @byte to the data register. */
    void (*node_byte)(SGIL1Provider *p, unsigned transport, uint8_t byte);
    /* The node read a byte: the window's queue has room again. */
    void (*node_room)(SGIL1Provider *p, unsigned transport);
};

#endif
