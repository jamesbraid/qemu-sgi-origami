/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The old, per-product SN0 descriptor and the Origin 2000 assembly that
 * still consumes it (sn0-origin2000.c and the Crosstalk assembly). The
 * shared node-builder vocabulary these types are built from moved to
 * sn0.h, since sn0.c and the family builder (sn-machine.c) need it for
 * every product, not only for what this header still serves.
 */

#ifndef HW_MIPS_SN0_MODEL_H
#define HW_MIPS_SN0_MODEL_H

#include "sn0.h"

#define SN0_MODEL_MODULE_SERIAL_LEN 10

typedef enum SN0ModelFabricVertexKind {
    SN0_MODEL_FABRIC_HUB,
    SN0_MODEL_FABRIC_ROUTER,
} SN0ModelFabricVertexKind;

typedef enum SN0ModelFabricLinkKind {
    SN0_MODEL_LINK_MIDPLANE,
    SN0_MODEL_LINK_CABLE,
    SN0_MODEL_LINK_XPRESS_CABLE,
    SN0_MODEL_LINK_CARRIER,
    SN0_MODEL_LINK_NULL_ROUTER,
} SN0ModelFabricLinkKind;

typedef struct SN0ModelFabricVertex {
    SN0ModelFabricVertexKind kind;
    unsigned int index;
    unsigned int port;
} SN0ModelFabricVertex;

typedef struct SN0ModelRoutedFabricLink {
    const char *name;
    SN0ModelFabricVertex ends[2];
    SN0ModelFabricLinkKind kind;
} SN0ModelRoutedFabricLink;

typedef struct SN0ModelRouter {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    uint16_t part;
    uint8_t revision;
    uint8_t chip_inputs;
    uint64_t serial;
} SN0ModelRouter;

typedef struct SN0ModelModuleManagement {
    const unsigned int *adapter_nodes;
    const uint64_t *adapter_offsets;
    size_t adapter_count;
    uint8_t msc_address;
    uint8_t nvram_address;
    unsigned int nvram_address_count;
    unsigned int nvram_bytes;
} SN0ModelModuleManagement;

typedef struct SN0ModelModule {
    const char *id;
    char serial[SN0_MODEL_MODULE_SERIAL_LEN + 1];
    /* Selected installation identity; seed newly created MSC NVRAM only. */
    uint8_t initial_module_number;
    const SN0ModelModuleManagement *management;
    const unsigned int *nodes;
    size_t node_count;
} SN0ModelModule;

typedef struct SN0ModelCrossbow {
    const char *id;
    unsigned int module_index;
    uint16_t widget_part;
    uint16_t widget_manufacturer;
    uint8_t widget_revision;
    uint16_t initially_operational_links;
} SN0ModelCrossbow;

typedef struct SN0ModelXtalkAttachment {
    unsigned int crossbow_index;
    unsigned int widget;
    unsigned int node_index;
} SN0ModelXtalkAttachment;

typedef struct SN0ModelIoBoard {
    const char *id;
    unsigned int module_index;
    unsigned int slot;
    unsigned int crossbow_index;
    unsigned int widget;
    const SN0ModelComponent *bridge;
    const SN0ModelComponent *pci;
    size_t pci_count;
    SN0ModelIoc3Population ioc3_population;
} SN0ModelIoBoard;

typedef struct SN0ModelComposition {
    const char *name;
    const SN0ModelHardware *hardware;
    const SN0ModelPhysicalNode *nodes;
    size_t node_count;
    const SN0ModelModule *modules;
    size_t module_count;
    const SN0ModelRouter *routers;
    size_t router_count;
    const SN0ModelRoutedFabricLink *links;
    size_t link_count;
    const SN0ModelCrossbow *crossbows;
    size_t crossbow_count;
    const SN0ModelXtalkAttachment *xtalk_attachments;
    size_t xtalk_attachment_count;
    const SN0ModelIoBoard *io_boards;
    size_t io_board_count;
} SN0ModelComposition;

extern const SN0ModelComposition sn0_origin2000_compositions[];
extern const size_t sn0_origin2000_composition_count;
extern const char sn0_origin2000_default_composition[];

const SN0ModelComposition *sn0_origin2000_find_composition(const char *name);

#endif /* HW_MIPS_SN0_MODEL_H */
