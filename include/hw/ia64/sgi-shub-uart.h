/* SPDX-License-Identifier: BSD-3-Clause */
/* The 16550 the Altix 350's node controller carries on its junk bus. */
#ifndef HW_IA64_SGI_SHUB_UART_H
#define HW_IA64_SGI_SHUB_UART_H

#include "hw/char/serial.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_SHUB_UART "sgi-shub-uart"
OBJECT_DECLARE_SIMPLE_TYPE(SGIShubUartState, SGI_SHUB_UART)

struct SGIShubUartState {
    SysBusDevice parent_obj;

    SerialState serial;
};

#endif
