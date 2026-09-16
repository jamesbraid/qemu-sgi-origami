/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_H
#define HW_MISC_SGI_MGRAS_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_SGI_MGRAS "sgi-mgras"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMgrasState, SGI_MGRAS)

#define SGI_MGRAS_MMIO_SIZE (2 * MiB)
#define SGI_MGRAS_WIDTH 1280
#define SGI_MGRAS_HEIGHT 1024

struct SGIMgrasState {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    qemu_irq irq[4];
    QEMUTimer *retrace_timer;
    uint32_t *front;
    uint32_t *back;
    uint32_t front_count;
    uint32_t back_count;
    uint8_t *wid;
    uint32_t hq[5120];
    uint32_t *ge;
    uint32_t ge_addr;
    uint32_t ge_words;
    uint32_t hq_words;
    uint32_t hq_hash;
    uint32_t ge_hash;
    uint32_t pending;
    uint32_t irq_mask;
    uint32_t error;
    uint32_t error_addr;
    uint32_t error_width;
    uint32_t context[63];
    uint32_t context_words;
    uint32_t entry;
    uint32_t protocol;
    uint64_t prefix;
    bool entry_selected;
    bool first_ack;
    bool second_ack;
    bool ready;
    bool display_enabled;
    bool cursor_enabled;
    uint8_t cursor[256];
    uint32_t cursor_control;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t cmap_index;
    uint32_t cmap[256];
    uint32_t fifo[256 * 2];
    uint16_t fifo_head;
    uint16_t fifo_tail;
    uint16_t fifo_count;
    uint32_t command[0x200];
    uint8_t command_valid[0x200];
    uint64_t retraces;
    uint32_t xmap_mode;
    uint32_t vc3[64];
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif
