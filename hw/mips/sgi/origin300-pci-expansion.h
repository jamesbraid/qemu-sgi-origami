/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MIPS_SGI_ORIGIN300_PCI_EXPANSION_H
#define HW_MIPS_SGI_ORIGIN300_PCI_EXPANSION_H

#include "hw/pci/pci.h"
#include "hw/misc/sgi-bic-board-interface.h"
#include "qom/object.h"

#define TYPE_SGI_ORIGIN300_PCI_EXPANSION "sgi-origin300-pci-expansion-module"
OBJECT_DECLARE_SIMPLE_TYPE(SGIOrigin300PciExpansionState,
                           SGI_ORIGIN300_PCI_EXPANSION)

#define SGI_ORIGIN300_PCI_HEADS 6

typedef struct SGIL1State SGIL1State;
typedef bool (*SGIOrigin300PowerOperation)(void *opaque, bool on,
                                          Error **errp);
bool sgi_origin300_pci_expansion_bind_power(
    SGIOrigin300PciExpansionState *s, SGIL1State *owner,
    uint64_t identity, bool initial_on, SGIOrigin300PowerOperation operation,
    void *opaque, Error **errp);
bool sgi_origin300_pci_expansion_request_power(
    SGIOrigin300PciExpansionState *s, SGIL1State *issuer,
    uint64_t identity, bool on, Error **errp);
bool sgi_origin300_pci_expansion_power_requested(
    SGIOrigin300PciExpansionState *s);
bool sgi_origin300_pci_expansion_main_power_available(
    SGIOrigin300PciExpansionState *s);

bool sgi_origin300_pci_expansion_bind_storage(
    SGIOrigin300PciExpansionState *s, const char *module_id,
    const char *controller_id, BlockBackend *backend, Error **errp);

/* Synthetic device indices 1 and 2; pins A/B/C/D select head inputs 0..7. */
typedef struct SGIOrigin300PciIntxMap {
    uint8_t pin[SGI_ORIGIN300_PCI_HEADS][2][PCI_NUM_PINS];
} SGIOrigin300PciIntxMap;

bool sgi_origin300_pci_expansion_set_intx_map(
    SGIOrigin300PciExpansionState *s, const SGIOrigin300PciIntxMap *map,
    Error **errp);

/* Immutable attachment; caller supplies and realizes the interface. */
bool sgi_origin300_pci_expansion_attach_board_interface(
    SGIOrigin300PciExpansionState *s, SGIBICBoardInterfaceState *interface,
    Error **errp);

bool sgi_origin300_pci_expansion_enable_ni_route(
    SGIOrigin300PciExpansionState *s, Error **errp);

/* ASIC/head order: U0 E/F, U1 E/F, U2 E/F; chassis slot mapping is unknown. */
PCIBus *sgi_origin300_pci_expansion_bus(SGIOrigin300PciExpansionState *s,
                                       unsigned head);

#endif
