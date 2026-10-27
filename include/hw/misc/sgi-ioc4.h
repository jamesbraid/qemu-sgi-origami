/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI IOC4 I/O controller, revision 0x53: device state.
 */

#ifndef HW_MISC_SGI_IOC4_H
#define HW_MISC_SGI_IOC4_H

#include "hw/char/serial.h"
#include "hw/char/sgi-ioc-serial-dma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/input/sgi-ioc3-kbc.h"
#include "hw/pci/pci_device.h"
#include "qom/object.h"

#define TYPE_SGI_IOC4 "sgi-ioc4"
#define SGI_IOC4_PORTS 4

typedef struct SGIIOC4State SGIIOC4State;
typedef struct SGIIOC4Port SGIIOC4Port;
typedef struct SGIIOC4BusState SGIIOC4BusState;

#define TYPE_SGI_IOC4_BUS "sgi-ioc4-bus"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC4BusState, SGI_IOC4_BUS)

struct SGIIOC4BusState {
    BusState parent_obj;
};

/* A UART window's handle: its chip and port. */
struct SGIIOC4Port {
    SGIIOC4State *ioc4;
    unsigned index;
};

struct SGIIOC4State {
    PCIDevice parent_obj;
    MemoryRegion bar;
    MemoryRegion clock_alias;
    MemoryRegion kbc_be;
    MemoryRegion *clock;
    MemoryRegion *kbc_mmio;
    BusState kbc_bus;
    SGIIOC3KBCState kbc;
    bool clock_alias_initialized;
    bool clock_alias_mapped;
    bool kbc_realized;
    SerialState uart[SGI_IOC4_PORTS];
    SGIIOC4Port dma[SGI_IOC4_PORTS];
    SGIIOCSerialDMA sdma;
    uint32_t other_pending;
    uint32_t dma_error_low;
    uint32_t dma_error_high;
    uint8_t dma_migration_mode;
    uint8_t pci_status_compat_mode;
    uint8_t ata_readback_compat_mode;
    bool serial_dma_experimental;
    /* State is loading; the interrupt words are not all in yet. */
    bool loading;
    bool pci_status_compat;
    bool ata_readback_compat;
    uint32_t ata_readback;
    IRQState uart_irq[SGI_IOC4_PORTS];
    uint32_t serial_pending;
    uint32_t serial_enable;
    uint32_t other_enable;
    uint32_t uart_level;
    /* The receive threshold sources, a level per port. */
    uint32_t threshold_level;
    /* The transmit-ring-empty sources, a level per port. */
    uint32_t tx_empty_level;
    /* The receive-ring-full sources, a level per port. */
    uint32_t rx_full_level;
    uint32_t clock_hz;
};

#endif
